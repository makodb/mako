#![deny(unsafe_code)]

//! Safe ownership and threading policy over Mako's Masstree C ABI.
//!
//! Masstree is exposed only as an append-only binary-key directory from keys
//! to immutable, nonzero [`RecordId`] values. Native cursors, node/value
//! pointers, and RCU guards never cross this crate's private FFI boundary.

mod native;

use std::{
    collections::HashSet,
    fmt,
    marker::PhantomData,
    num::NonZeroU64,
    rc::Rc,
    sync::{Arc, Mutex, MutexGuard, OnceLock, Weak},
    thread::{self, ThreadId},
};

/// A nonzero scalar stored immutably in the native directory.
#[repr(transparent)]
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct RecordId(NonZeroU64);

impl RecordId {
    pub const fn new(value: u64) -> Option<Self> {
        match NonZeroU64::new(value) {
            Some(value) => Some(Self(value)),
            None => None,
        }
    }

    pub const fn get(self) -> u64 {
        self.0.get()
    }
}

impl TryFrom<u64> for RecordId {
    type Error = Error;

    fn try_from(value: u64) -> Result<Self, Self::Error> {
        Self::new(value).ok_or(Error::ZeroRecordId)
    }
}

impl From<RecordId> for u64 {
    fn from(value: RecordId) -> Self {
        value.get()
    }
}

/// Stable classification of every native status code.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum NativeStatus {
    Invalid,
    KeyTooLarge,
    BufferTooSmall,
    NotAttached,
    WrongThread,
    WrongRuntime,
    ThreadLimit,
    OutOfMemory,
    Busy,
    ActiveGuards,
    AbiMismatch,
    CppException,
    Internal,
    Unsupported,
    IncompatibleRuntime,
    Poisoned,
    Closed,
    Unknown(i32),
}

impl NativeStatus {
    fn from_raw(status: i32) -> Option<Self> {
        match status {
            mtree_sys::OK => None,
            mtree_sys::ERR_INVALID => Some(Self::Invalid),
            mtree_sys::ERR_KEY_TOO_LARGE => Some(Self::KeyTooLarge),
            mtree_sys::ERR_BUFFER_TOO_SMALL => Some(Self::BufferTooSmall),
            mtree_sys::ERR_NOT_ATTACHED => Some(Self::NotAttached),
            mtree_sys::ERR_WRONG_THREAD => Some(Self::WrongThread),
            mtree_sys::ERR_WRONG_RUNTIME => Some(Self::WrongRuntime),
            mtree_sys::ERR_THREAD_LIMIT => Some(Self::ThreadLimit),
            mtree_sys::ERR_OUT_OF_MEMORY => Some(Self::OutOfMemory),
            mtree_sys::ERR_BUSY => Some(Self::Busy),
            mtree_sys::ERR_ACTIVE_GUARDS => Some(Self::ActiveGuards),
            mtree_sys::ERR_ABI_MISMATCH => Some(Self::AbiMismatch),
            mtree_sys::ERR_CPP_EXCEPTION => Some(Self::CppException),
            mtree_sys::ERR_INTERNAL => Some(Self::Internal),
            mtree_sys::ERR_UNSUPPORTED => Some(Self::Unsupported),
            mtree_sys::ERR_INCOMPATIBLE_RUNTIME => Some(Self::IncompatibleRuntime),
            mtree_sys::ERR_POISONED => Some(Self::Poisoned),
            mtree_sys::ERR_CLOSED => Some(Self::Closed),
            unknown => Some(Self::Unknown(unknown)),
        }
    }
}

/// Safe-wrapper or native-boundary failure.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Error {
    Native(NativeStatus),
    AbiMismatch(&'static str),
    WrongThread,
    WrongRuntime,
    DuplicateWorker,
    KeyTooLarge { length: usize, maximum: usize },
    ZeroRecordId,
    InvalidPublication,
    AllocationLimit { requested: usize },
}

impl fmt::Display for Error {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "{self:?}")
    }
}

impl std::error::Error for Error {}

fn status_result(status: i32) -> Result<(), Error> {
    match NativeStatus::from_raw(status) {
        None => Ok(()),
        Some(status) => Err(Error::Native(status)),
    }
}

/// Requested singleton-runtime limits.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct RuntimeConfig {
    max_threads: Option<u32>,
    max_key_length: Option<u32>,
}

impl RuntimeConfig {
    pub const fn new() -> Self {
        Self {
            max_threads: None,
            max_key_length: None,
        }
    }

    pub const fn with_max_threads(mut self, maximum: u32) -> Self {
        self.max_threads = Some(maximum);
        self
    }

    pub const fn with_max_key_length(mut self, maximum: u32) -> Self {
        self.max_key_length = Some(maximum);
        self
    }
}

struct RuntimeInner {
    raw: native::RuntimeHandle,
    max_threads: u32,
    max_key_length: usize,
    features: u64,
    build_id: mtree_sys::BuildId,
    attached: Mutex<HashSet<ThreadId>>,
}

static SHARED_RUNTIME: OnceLock<Mutex<Weak<RuntimeInner>>> = OnceLock::new();

/// Shareable ownership of the negotiated process-wide native runtime.
///
/// Dropping the final Rust handle releases only Rust-side bookkeeping. The
/// inherited native singleton cannot be shut down safely and remains alive
/// for the process.
#[derive(Clone)]
pub struct Runtime {
    inner: Arc<RuntimeInner>,
}

impl fmt::Debug for Runtime {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("Runtime")
            .field("max_threads", &self.inner.max_threads)
            .field("max_key_length", &self.inner.max_key_length)
            .field("features", &self.inner.features)
            .field("build_id", &self.inner.build_id)
            .finish_non_exhaustive()
    }
}

impl Runtime {
    /// Negotiates ABI/layout/build identity and acquires the singleton runtime.
    pub fn new(config: RuntimeConfig) -> Result<Self, Error> {
        let acquired = native::acquire(config)?;
        let shared = SHARED_RUNTIME.get_or_init(|| Mutex::new(Weak::new()));
        let mut slot = recover_lock(shared);
        if let Some(inner) = slot.upgrade() {
            if inner.raw != acquired.raw
                || inner.max_threads != acquired.max_threads
                || inner.max_key_length != acquired.max_key_length
                || inner.features != acquired.features
                || inner.build_id != acquired.build_id
            {
                return Err(Error::AbiMismatch(
                    "singleton runtime changed identity while still reachable",
                ));
            }
            return Ok(Self { inner });
        }

        let inner = Arc::new(RuntimeInner {
            raw: acquired.raw,
            max_threads: acquired.max_threads,
            max_key_length: acquired.max_key_length,
            features: acquired.features,
            build_id: acquired.build_id,
            attached: Mutex::new(HashSet::new()),
        });
        *slot = Arc::downgrade(&inner);
        Ok(Self { inner })
    }

    pub fn max_threads(&self) -> u32 {
        self.inner.max_threads
    }

    pub fn max_key_length(&self) -> usize {
        self.inner.max_key_length
    }

    pub fn feature_bits(&self) -> u64 {
        self.inner.features
    }

    pub fn build_id(&self) -> mtree_sys::BuildId {
        self.inner.build_id
    }

    pub fn health(&self) -> Result<RuntimeHealth, Error> {
        native::runtime_health(self.inner.raw)
    }

    /// Attaches one non-sendable worker facade to the calling OS thread.
    pub fn attach(&self) -> Result<Worker, Error> {
        let owner = thread::current().id();
        let mut attached = recover_lock(&self.inner.attached);
        if attached.contains(&owner) {
            return Err(Error::DuplicateWorker);
        }
        attached
            .try_reserve(1)
            .map_err(|_| Error::Native(NativeStatus::OutOfMemory))?;
        let raw = native::thread_attach(self.inner.raw)?;
        attached.insert(owner);
        drop(attached);
        Ok(Worker {
            runtime: Arc::clone(&self.inner),
            raw,
            owner,
            not_send_sync: PhantomData,
        })
    }

    pub fn create_tree(&self, worker: &Worker) -> Result<Tree, Error> {
        worker.ensure(&self.inner)?;
        let raw = native::tree_create(self.inner.raw, worker.raw)?;
        Ok(Tree {
            inner: Arc::new(TreeInner {
                runtime: Arc::clone(&self.inner),
                raw,
            }),
        })
    }

    /// Requests native shutdown. V1 returns `Unsupported` by negotiation.
    pub fn shutdown(&self, worker: &Worker) -> Result<(), Error> {
        worker.ensure(&self.inner)?;
        native::runtime_shutdown(self.inner.raw, worker.raw)
    }
}

/// Current native runtime health.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RuntimeHealth {
    Healthy,
    Poisoned,
}

/// One worker bound to its creating thread and runtime.
///
/// Drop permits another safe facade to attach on this thread; it does not
/// unregister or destroy the long-lived native worker handle.
///
/// A worker deliberately cannot migrate to another thread:
///
/// ```compile_fail
/// fn require_send<T: Send>() {}
/// require_send::<masstree::Worker>();
/// ```
///
/// ```compile_fail
/// fn require_sync<T: Sync>() {}
/// require_sync::<masstree::Worker>();
/// ```
pub struct Worker {
    runtime: Arc<RuntimeInner>,
    raw: native::ThreadHandle,
    owner: ThreadId,
    not_send_sync: PhantomData<Rc<()>>,
}

impl fmt::Debug for Worker {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("Worker")
            .field("owner", &self.owner)
            .finish_non_exhaustive()
    }
}

impl Worker {
    pub fn quiesce(&self) -> Result<(), Error> {
        self.ensure(&self.runtime)?;
        native::thread_quiesce(self.raw)
    }

    fn ensure(&self, runtime: &Arc<RuntimeInner>) -> Result<(), Error> {
        if thread::current().id() != self.owner {
            return Err(Error::WrongThread);
        }
        if !Arc::ptr_eq(&self.runtime, runtime) {
            return Err(Error::WrongRuntime);
        }
        Ok(())
    }
}

impl Drop for Worker {
    fn drop(&mut self) {
        let mut attached = recover_lock(&self.runtime.attached);
        attached.remove(&self.owner);
    }
}

struct TreeInner {
    runtime: Arc<RuntimeInner>,
    raw: native::TreeHandle,
}

impl Drop for TreeInner {
    fn drop(&mut self) {
        native::tree_release_best_effort(self.raw);
    }
}

/// Cloneable, thread-safe facade for one append-only native directory.
///
/// Dropping the final clone closes only the C facade. Native tree storage is
/// process-lived and is never destroyed from Rust.
#[derive(Clone)]
pub struct Tree {
    inner: Arc<TreeInner>,
}

impl fmt::Debug for Tree {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.debug_struct("Tree").finish_non_exhaustive()
    }
}

impl Tree {
    pub fn get(&self, worker: &Worker, key: &[u8]) -> Result<Option<RecordId>, Error> {
        self.check(worker, key)?;
        native::get(self.inner.raw, worker.raw, key).map(RecordId::new)
    }

    pub fn get_or_insert(
        &self,
        worker: &Worker,
        key: &[u8],
        candidate: RecordId,
    ) -> Result<InsertOutcome, InsertError> {
        if let Err(error) = self.check(worker, key) {
            return Err(InsertError {
                error,
                publication: PublicationDisposition::FailureBeforePublication,
                winner: None,
            });
        }
        native::get_or_insert(self.inner.raw, worker.raw, key, candidate)
    }

    /// Copies one weakly consistent, key-ordered directory chunk.
    ///
    /// All returned keys are Rust-owned. The native RCU scope ends before this
    /// method returns. Transactional callers must separately validate their
    /// logical range predicate (the STO Masstree adapter uses its membership
    /// resource for that purpose).
    pub fn scan_chunk(
        &self,
        worker: &Worker,
        request: ScanRequest<'_>,
    ) -> Result<ScanChunk, Error> {
        worker.ensure(&self.inner.runtime)?;
        self.check_bound(request.lower)?;
        self.check_bound(request.upper)?;
        let chunk = native::scan(self.inner.raw, worker.raw, request)?;
        if chunk.next_key_bytes_required > self.inner.runtime.max_key_length
            || chunk
                .entries
                .iter()
                .any(|entry| entry.key.len() > self.inner.runtime.max_key_length)
        {
            return Err(Error::AbiMismatch(
                "scan returned a key above the negotiated maximum",
            ));
        }
        Ok(chunk)
    }

    fn check(&self, worker: &Worker, key: &[u8]) -> Result<(), Error> {
        worker.ensure(&self.inner.runtime)?;
        if key.len() > self.inner.runtime.max_key_length {
            return Err(Error::KeyTooLarge {
                length: key.len(),
                maximum: self.inner.runtime.max_key_length,
            });
        }
        Ok(())
    }

    fn check_bound(&self, bound: KeyBound<'_>) -> Result<(), Error> {
        let Some(key) = bound.key() else {
            return Ok(());
        };
        if key.len() > self.inner.runtime.max_key_length {
            return Err(Error::KeyTooLarge {
                length: key.len(),
                maximum: self.inner.runtime.max_key_length,
            });
        }
        Ok(())
    }
}

/// Direction of a copied directory range scan.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum ScanDirection {
    Forward,
    Reverse,
}

/// One explicit binary-key range bound.
///
/// `Unbounded` is distinct from `Included(&[])` and `Excluded(&[])`.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum KeyBound<'key> {
    Unbounded,
    Included(&'key [u8]),
    Excluded(&'key [u8]),
}

impl<'key> KeyBound<'key> {
    fn key(self) -> Option<&'key [u8]> {
        match self {
            Self::Unbounded => None,
            Self::Included(key) | Self::Excluded(key) => Some(key),
        }
    }
}

/// Bounded allocation request for one copied scan call.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ScanRequest<'key> {
    direction: ScanDirection,
    lower: KeyBound<'key>,
    upper: KeyBound<'key>,
    entry_capacity: usize,
    key_arena_capacity: usize,
}

impl<'key> ScanRequest<'key> {
    pub const fn new(direction: ScanDirection) -> Self {
        Self {
            direction,
            lower: KeyBound::Unbounded,
            upper: KeyBound::Unbounded,
            entry_capacity: 128,
            key_arena_capacity: 16 * 1024,
        }
    }

    pub const fn with_lower(mut self, lower: KeyBound<'key>) -> Self {
        self.lower = lower;
        self
    }

    pub const fn with_upper(mut self, upper: KeyBound<'key>) -> Self {
        self.upper = upper;
        self
    }

    pub const fn with_entry_capacity(mut self, capacity: usize) -> Self {
        self.entry_capacity = capacity;
        self
    }

    pub const fn with_key_arena_capacity(mut self, capacity: usize) -> Self {
        self.key_arena_capacity = capacity;
        self
    }

    pub const fn direction(self) -> ScanDirection {
        self.direction
    }

    pub const fn lower(self) -> KeyBound<'key> {
        self.lower
    }

    pub const fn upper(self) -> KeyBound<'key> {
        self.upper
    }

    pub const fn entry_capacity(self) -> usize {
        self.entry_capacity
    }

    pub const fn key_arena_capacity(self) -> usize {
        self.key_arena_capacity
    }
}

/// One copied binary key and its immutable directory identifier.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ScanEntry {
    key: Box<[u8]>,
    record_id: RecordId,
}

impl ScanEntry {
    pub fn key(&self) -> &[u8] {
        &self.key
    }

    pub const fn record_id(&self) -> RecordId {
        self.record_id
    }
}

/// Why a native scan chunk stopped.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum ScanStopReason {
    End,
    EntryCapacity,
    KeyArenaCapacity,
}

/// Authoritative input for continuing a capacity-limited scan.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ScanResume {
    /// The range is exhausted.
    None,
    /// No entry fit; grow the limiting buffer and retry the identical request.
    UnchangedInput,
    /// Continue exclusively after (forward) or before (reverse) this key.
    Exclusive(Box<[u8]>),
}

/// One fully owned, validated directory chunk.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ScanChunk {
    entries: Vec<ScanEntry>,
    stop_reason: ScanStopReason,
    resume: ScanResume,
    next_key_bytes_required: usize,
}

impl ScanChunk {
    pub fn entries(&self) -> &[ScanEntry] {
        &self.entries
    }

    pub fn into_entries(self) -> Vec<ScanEntry> {
        self.entries
    }

    pub const fn stop_reason(&self) -> ScanStopReason {
        self.stop_reason
    }

    pub fn resume(&self) -> &ScanResume {
        &self.resume
    }

    pub const fn next_key_bytes_required(&self) -> usize {
        self.next_key_bytes_required
    }
}

/// Successful atomic get-or-insert classification.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum InsertOutcome {
    Inserted(RecordId),
    Existing(RecordId),
}

/// Whether a failed native insertion could have published its candidate.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PublicationDisposition {
    FailureBeforePublication,
    CandidateInserted,
    CandidateProvenUnpublished,
    Unknown,
}

/// Error preserving the candidate-publication disposition.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct InsertError {
    error: Error,
    publication: PublicationDisposition,
    winner: Option<RecordId>,
}

impl InsertError {
    pub const fn error(&self) -> Error {
        self.error
    }

    pub const fn publication(&self) -> PublicationDisposition {
        self.publication
    }

    pub const fn winner(&self) -> Option<RecordId> {
        self.winner
    }
}

impl fmt::Display for InsertError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "{:?} ({:?})", self.error, self.publication)
    }
}

impl std::error::Error for InsertError {}

fn recover_lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn record_ids_reject_the_reserved_zero() {
        assert_eq!(RecordId::new(0), None);
        assert_eq!(RecordId::new(u64::MAX).unwrap().get(), u64::MAX);
    }

    #[test]
    fn every_known_native_status_is_classified_without_boolean_loss() {
        for raw in 1..=17 {
            assert!(!matches!(
                NativeStatus::from_raw(raw),
                Some(NativeStatus::Unknown(_)) | None
            ));
        }
        assert_eq!(NativeStatus::from_raw(0), None);
        assert_eq!(NativeStatus::from_raw(99), Some(NativeStatus::Unknown(99)));
    }

    #[test]
    fn runtime_config_builders_preserve_explicit_limits() {
        let config = RuntimeConfig::new()
            .with_max_threads(8)
            .with_max_key_length(64);
        assert_eq!(config.max_threads, Some(8));
        assert_eq!(config.max_key_length, Some(64));
    }

    #[test]
    fn scan_request_builders_preserve_binary_bounds_and_capacities() {
        let request = ScanRequest::new(ScanDirection::Reverse)
            .with_lower(KeyBound::Included(b"\0lower"))
            .with_upper(KeyBound::Excluded(b"upper\xff"))
            .with_entry_capacity(7)
            .with_key_arena_capacity(99);

        assert_eq!(request.direction(), ScanDirection::Reverse);
        assert_eq!(request.lower(), KeyBound::Included(b"\0lower"));
        assert_eq!(request.upper(), KeyBound::Excluded(b"upper\xff"));
        assert_eq!(request.entry_capacity(), 7);
        assert_eq!(request.key_arena_capacity(), 99);
    }

    #[test]
    fn shareable_facades_are_send_sync_without_unsafe_impls() {
        fn assert_send_sync<T: Send + Sync>() {}

        assert_send_sync::<Runtime>();
        assert_send_sync::<Tree>();
        assert_send_sync::<ScanEntry>();
        assert_send_sync::<ScanChunk>();
    }
}
