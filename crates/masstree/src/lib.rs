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

/// One result slot produced by a fixed-key point-read batch.
///
/// The zero representation denotes an absent key. Native record IDs are
/// nonzero, so callers can inspect a reusable result buffer without a second
/// allocation or sentinel side table.
#[repr(transparent)]
#[derive(Clone, Copy, Debug, Default, Eq, Hash, PartialEq)]
pub struct PointReadResult(u64);

impl PointReadResult {
    /// Returns the immutable record ID, or `None` when the key was absent.
    pub const fn record_id(self) -> Option<RecordId> {
        RecordId::new(self.0)
    }
}

/// One ordered result from a trusted fixed-shape get-or-insert batch.
///
/// Callers normally reuse a `Vec<FixedInsertResult>` across transactions.
/// Fields stay private so the only observable classifications are values that
/// passed the native ABI consistency checks performed by [`Tree`].
#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FixedInsertResult {
    winner: u64,
    publication: u32,
    inserted: u8,
    reserved: [u8; 3],
}

impl Default for FixedInsertResult {
    fn default() -> Self {
        Self {
            winner: 0,
            publication: mtree_sys::PUBLICATION_FAILURE_BEFORE_PUBLICATION,
            inserted: 0,
            reserved: [0; 3],
        }
    }
}

impl FixedInsertResult {
    /// Returns this candidate's publication disposition and optional stable
    /// winner. This accepts both completed and interrupted batch slots.
    pub fn classification(
        self,
        candidate: RecordId,
    ) -> Result<(PublicationDisposition, Option<RecordId>), Error> {
        native::decode_fixed_insert_result(self, candidate)
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
    StructureSealed,
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
            mtree_sys::ERR_STRUCTURE_SEALED => Some(Self::StructureSealed),
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
    InvalidBatch(&'static str),
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
    /// Holds one native RCU region across several ordinary tree operations.
    ///
    /// This scope is worker-bound but tree-independent. It amortizes native
    /// RCU entry/exit for a short synchronous transaction-shaped sequence; it
    /// is not a snapshot and does not hold structural-reader admission.
    /// Operations on several trees, including `get_or_insert`, remain valid.
    /// After native validation those operations reuse this retained RCU region
    /// while preserving their ordinary per-operation structural admission.
    /// Read-scope creation, quiescence, and native lifecycle calls are rejected
    /// until the returned guard is closed or dropped.
    pub fn rcu_scope(&self) -> Result<RcuScope<'_>, Error> {
        self.ensure(&self.runtime)?;
        let raw = native::rcu_scope_begin(self.raw)?;
        Ok(RcuScope {
            _worker: self,
            raw: Some(raw),
            not_send_sync: PhantomData,
        })
    }

    #[inline]
    pub fn quiesce(&self) -> Result<(), Error> {
        self.ensure(&self.runtime)?;
        native::thread_quiesce(self.raw)
    }

    #[inline]
    fn ensure(&self, runtime: &Arc<RuntimeInner>) -> Result<(), Error> {
        // `Worker` is neither `Send` nor `Sync`, so safe Rust cannot move this
        // capability away from its attaching thread. Keep the assertion in
        // diagnostic builds without paying for a TLS/thread-ID lookup on
        // every point operation in release builds.
        debug_assert_eq!(thread::current().id(), self.owner);
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

/// A worker-affine native RCU region spanning ordinary Masstree operations.
///
/// The borrowed [`Worker`] makes this guard neither sendable nor shareable.
/// Native cleanup runs during ordinary return and Rust unwinding. Keep it
/// synchronous and short; do not retain it across blocking work, I/O,
/// `.await`, or unrelated native calls. The guard supplies memory-lifetime
/// protection only: it is not a transaction, snapshot, or structural lock.
///
/// ```compile_fail
/// fn require_send<T: Send>() {}
/// require_send::<masstree::RcuScope<'static>>();
/// ```
///
/// ```compile_fail
/// fn require_sync<T: Sync>() {}
/// require_sync::<masstree::RcuScope<'static>>();
/// ```
#[must_use = "dropping the RCU scope immediately provides no amortization"]
pub struct RcuScope<'worker> {
    _worker: &'worker Worker,
    raw: Option<native::RcuScopeHandle>,
    not_send_sync: PhantomData<Rc<()>>,
}

impl fmt::Debug for RcuScope<'_> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("RcuScope")
            .field("active", &self.raw.is_some())
            .finish_non_exhaustive()
    }
}

impl RcuScope<'_> {
    /// Ends the native scope and reports any boundary invariant failure.
    pub fn close(mut self) -> Result<(), Error> {
        self.end()
    }

    fn end(&mut self) -> Result<(), Error> {
        let Some(raw) = self.raw.as_mut() else {
            return Ok(());
        };
        native::rcu_scope_end(raw)?;
        self.raw = None;
        Ok(())
    }
}

impl Drop for RcuScope<'_> {
    fn drop(&mut self) {
        let _ = self.end();
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
    /// Permanently prevents this directory's key set from growing.
    ///
    /// The native transition waits for current structural readers and is
    /// idempotent. Point reads and scans continue to work. Every later
    /// get-or-insert fails before candidate publication, even for an existing
    /// key. Sealing needs no worker because it enters neither Masstree nor RCU.
    pub fn seal_structure(&self) -> Result<(), Error> {
        native::tree_seal_structure(self.inner.raw)
    }

    #[inline]
    pub fn get(&self, worker: &Worker, key: &[u8]) -> Result<Option<RecordId>, Error> {
        self.check(worker, key)?;
        native::get(self.inner.raw, worker.raw, key).map(RecordId::new)
    }

    /// Looks up a contiguous array of equally sized binary keys in one native
    /// operation.
    ///
    /// Handles, worker affinity, runtime health, and the common key shape are
    /// validated once. Native structural-read and RCU guards cover the whole
    /// nonempty batch and end before this method returns. `results` is resized
    /// and reused; after success it contains exactly one result per input key.
    /// An empty key type (`KEY_LENGTH == 0`) repeatedly addresses the
    /// directory's empty binary key.
    pub fn get_fixed<const KEY_LENGTH: usize>(
        &self,
        worker: &Worker,
        keys: &[[u8; KEY_LENGTH]],
        results: &mut Vec<PointReadResult>,
    ) -> Result<(), Error> {
        if let Err(error) = worker.ensure(&self.inner.runtime) {
            results.clear();
            return Err(error);
        }
        if KEY_LENGTH > self.inner.runtime.max_key_length {
            results.clear();
            return Err(Error::KeyTooLarge {
                length: KEY_LENGTH,
                maximum: self.inner.runtime.max_key_length,
            });
        }
        let additional = keys.len().saturating_sub(results.len());
        if results.try_reserve_exact(additional).is_err() {
            results.clear();
            return Err(Error::AllocationLimit {
                requested: keys.len(),
            });
        }
        results.resize(keys.len(), PointReadResult::default());
        native::get_strided(self.inner.raw, worker.raw, keys, results)
    }

    /// Begins one worker-affine point-read scope for this tree.
    ///
    /// The scope amortizes native handle validation, structural-reader
    /// admission, and RCU protection across its [`ReadScope::get`] calls. It
    /// is not a snapshot. Drop or explicitly close it before calling
    /// [`Self::get_or_insert`], [`Self::scan_chunk`], worker quiescence, or an
    /// operation on another tree with the same worker. A miss does not close
    /// the scope automatically. Do not retain it across blocking work, I/O,
    /// `.await`, or native calls outside this scope's point-read methods.
    pub fn read_scope<'tree, 'worker>(
        &'tree self,
        worker: &'worker Worker,
    ) -> Result<ReadScope<'tree, 'worker>, Error> {
        worker.ensure(&self.inner.runtime)?;
        let raw = native::read_scope_begin(self.inner.raw, worker.raw)?;
        Ok(ReadScope {
            tree: self,
            _worker: worker,
            raw: Some(raw),
            not_send_sync: PhantomData,
        })
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

    /// Resolves or publishes a strided array of equally sized binary keys.
    ///
    /// The native call preserves input order and owns one structural-writer
    /// admission plus one RCU region for the complete nonempty batch. This is
    /// intentionally a narrow trusted bridge for Rust-owned slices: candidates
    /// must be pairwise distinct, the two slice lengths must match, and
    /// `KEY_LENGTH` may not exceed either `KEY_STRIDE` or the negotiated key
    /// limit. Duplicate keys are allowed and therefore observe earlier
    /// publications in the same call.
    ///
    /// On a native error `results` still contains one validated publication
    /// classification per candidate. If safe preflight or output allocation
    /// fails, `results` is empty and no candidate reached native code.
    pub fn get_or_insert_fixed_strided<const KEY_LENGTH: usize, const KEY_STRIDE: usize>(
        &self,
        worker: &Worker,
        keys: &[[u8; KEY_STRIDE]],
        candidates: &[RecordId],
        results: &mut Vec<FixedInsertResult>,
    ) -> Result<(), Error> {
        results.clear();
        worker.ensure(&self.inner.runtime)?;
        if KEY_LENGTH > KEY_STRIDE {
            return Err(Error::InvalidBatch("key length exceeds key stride"));
        }
        if KEY_LENGTH > self.inner.runtime.max_key_length {
            return Err(Error::KeyTooLarge {
                length: KEY_LENGTH,
                maximum: self.inner.runtime.max_key_length,
            });
        }
        if keys.len() != candidates.len() {
            return Err(Error::InvalidBatch(
                "key and candidate batch lengths differ",
            ));
        }
        for (index, candidate) in candidates.iter().enumerate() {
            if candidates[..index].contains(candidate) {
                return Err(Error::InvalidBatch(
                    "candidate record identities are not distinct",
                ));
            }
        }
        results
            .try_reserve_exact(keys.len())
            .map_err(|_| Error::AllocationLimit {
                requested: keys.len(),
            })?;
        results.resize(keys.len(), FixedInsertResult::default());
        native::get_or_insert_strided::<KEY_LENGTH, KEY_STRIDE>(
            self.inner.raw,
            worker.raw,
            keys,
            candidates,
            results,
        )
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
        self.scan_packed_chunk(worker, request)?.try_into_owned()
    }

    /// Copies one weakly consistent, key-ordered directory chunk into a
    /// packed key arena.
    ///
    /// Unlike [`Self::scan_chunk`], this representation does not allocate a
    /// separate box for every key. Entry and continuation keys borrow the
    /// chunk's single arena while they are inspected. The native RCU scope
    /// still ends before this method returns; logical range validation remains
    /// the transactional caller's responsibility.
    pub fn scan_packed_chunk(
        &self,
        worker: &Worker,
        request: ScanRequest<'_>,
    ) -> Result<PackedScanChunk, Error> {
        worker.ensure(&self.inner.runtime)?;
        self.check_bound(request.lower)?;
        self.check_bound(request.upper)?;
        native::scan(
            self.inner.raw,
            worker.raw,
            request,
            self.inner.runtime.max_key_length,
        )
    }

    /// Copies one packed directory chunk into caller-owned reusable storage.
    ///
    /// The first call grows `scratch` to the requested entry and key-arena
    /// capacities. Later calls at the same or smaller capacities neither
    /// allocate nor clear those buffers. The returned view borrows `scratch`,
    /// so it must be consumed before the next reuse.
    pub fn scan_packed_chunk_reusing<'scratch>(
        &self,
        worker: &Worker,
        request: ScanRequest<'_>,
        scratch: &'scratch mut PackedScanScratch,
    ) -> Result<PackedScanChunkRef<'scratch>, Error> {
        worker.ensure(&self.inner.runtime)?;
        self.check_bound(request.lower)?;
        self.check_bound(request.upper)?;
        native::scan_reusing(
            self.inner.raw,
            worker.raw,
            request,
            self.inner.runtime.max_key_length,
            scratch,
        )
    }

    /// Copies one packed directory chunk while trusting native key semantics.
    ///
    /// The decoder still validates all storage counts and slice offsets and
    /// lengths before it constructs a borrowed Rust view. It omits the second
    /// key-order and range-membership walk used by
    /// [`Self::scan_packed_chunk_reusing`].
    ///
    /// # Safety
    ///
    /// The caller must exclusively own every access path to this tree and
    /// preserve the native scan contract: successful results contain only
    /// keys within `request`, in strict directional order, and no returned key
    /// exceeds the runtime's negotiated maximum length.
    #[allow(
        unsafe_code,
        reason = "the private-tree caller opts into native scan semantics"
    )]
    #[doc(hidden)]
    pub unsafe fn scan_packed_chunk_reusing_trusted<'scratch>(
        &self,
        worker: &Worker,
        request: ScanRequest<'_>,
        scratch: &'scratch mut PackedScanScratch,
    ) -> Result<PackedScanChunkRef<'scratch>, Error> {
        worker.ensure(&self.inner.runtime)?;
        self.check_bound(request.lower)?;
        self.check_bound(request.upper)?;
        // SAFETY: This method exposes the native semantic preconditions to its
        // caller. The decoder itself still proves every Rust memory-safety
        // condition before returning a view.
        unsafe {
            native::scan_reusing_trusted(
                self.inner.raw,
                worker.raw,
                request,
                self.inner.runtime.max_key_length,
                scratch,
            )
        }
    }

    /// Returns a lower-inclusive, upper-exclusive forward chunk of RecordIds.
    ///
    /// The native callback copies no emitted key or packed entry metadata. If
    /// the chunk fills, it copies only the first omitted key for continuation.
    ///
    /// # Safety
    ///
    /// The caller must exclusively own every access path to this tree and
    /// preserve the native bounded forward-scan contract. Successful IDs must
    /// be nonzero and emitted in strict key order within `[lower, upper)`.
    #[allow(
        unsafe_code,
        reason = "the private-tree caller opts into native bounded scan semantics"
    )]
    #[doc(hidden)]
    pub unsafe fn scan_record_ids_bounded_reusing_trusted<'scratch>(
        &self,
        worker: &Worker,
        lower: &[u8],
        upper: &[u8],
        entry_capacity: usize,
        continuation_capacity: usize,
        scratch: &'scratch mut PackedScanScratch,
    ) -> Result<BoundedRecordIdScanChunkRef<'scratch>, Error> {
        worker.ensure(&self.inner.runtime)?;
        self.check_bound(KeyBound::Included(lower))?;
        self.check_bound(KeyBound::Excluded(upper))?;
        // SAFETY: This method exposes the native ordering, bounds, and private
        // ownership preconditions to its caller. The decoder validates every
        // count, token, and borrowed slice before returning.
        unsafe {
            native::scan_record_ids_bounded_reusing_trusted(
                self.inner.raw,
                worker.raw,
                lower,
                upper,
                entry_capacity,
                continuation_capacity,
                self.inner.runtime.max_key_length,
                scratch,
            )
        }
    }

    #[inline]
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

/// An explicit RAII native structural-read and RCU scope for one tree and worker.
///
/// The borrowed [`Worker`] makes this type thread-affine; it cannot be sent or
/// shared across threads. Native cleanup runs during ordinary return and Rust
/// unwinding through this type's `Drop` implementation. While the scope is
/// active, unrelated operations through the same worker are rejected and a
/// same-tree structural writer on any worker waits for scope exit. Keep the
/// scope synchronous and short; do not deliberately retain it across blocking
/// waits, I/O, `.await`, or reentrant native work. Fixed-batch helpers may grow
/// caller-owned result storage and are not claimed to be allocation-free.
pub struct ReadScope<'tree, 'worker> {
    tree: &'tree Tree,
    _worker: &'worker Worker,
    raw: Option<native::ReadScopeHandle>,
    not_send_sync: PhantomData<Rc<()>>,
}

impl fmt::Debug for ReadScope<'_, '_> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("ReadScope")
            .field("active", &self.raw.is_some())
            .finish_non_exhaustive()
    }
}

impl ReadScope<'_, '_> {
    /// Looks up one binary key while retaining this scope's native guards.
    #[inline]
    pub fn get(&mut self, key: &[u8]) -> Result<Option<RecordId>, Error> {
        if key.len() > self.tree.inner.runtime.max_key_length {
            return Err(Error::KeyTooLarge {
                length: key.len(),
                maximum: self.tree.inner.runtime.max_key_length,
            });
        }
        let raw = self
            .raw
            .as_ref()
            .expect("a publicly reachable read scope is active");
        native::read_scope_get(raw, key).map(RecordId::new)
    }

    /// Looks up a contiguous array of equally sized binary keys in one native
    /// boundary crossing.
    ///
    /// Token affinity, tree/runtime health, and the common key length are
    /// validated once for the batch. `results` is resized and reused; after a
    /// successful call it contains exactly one entry per input key. An empty
    /// key type (`KEY_LENGTH == 0`) is valid and repeatedly addresses the
    /// directory's empty binary key.
    pub fn get_fixed<const KEY_LENGTH: usize>(
        &mut self,
        keys: &[[u8; KEY_LENGTH]],
        results: &mut Vec<PointReadResult>,
    ) -> Result<(), Error> {
        if KEY_LENGTH > self.tree.inner.runtime.max_key_length {
            results.clear();
            return Err(Error::KeyTooLarge {
                length: KEY_LENGTH,
                maximum: self.tree.inner.runtime.max_key_length,
            });
        }
        let additional = keys.len().saturating_sub(results.len());
        if results.try_reserve_exact(additional).is_err() {
            results.clear();
            return Err(Error::AllocationLimit {
                requested: keys.len(),
            });
        }
        results.resize(keys.len(), PointReadResult::default());
        let raw = self
            .raw
            .as_ref()
            .expect("a publicly reachable read scope is active");
        native::read_scope_get_strided(raw, keys, results)
    }

    /// Ends the native scope and reports any boundary invariant failure.
    pub fn close(mut self) -> Result<(), Error> {
        self.end()
    }

    fn end(&mut self) -> Result<(), Error> {
        let Some(raw) = self.raw.as_mut() else {
            return Ok(());
        };
        native::read_scope_end(raw)?;
        self.raw = None;
        Ok(())
    }
}

impl Drop for ReadScope<'_, '_> {
    fn drop(&mut self) {
        let _ = self.end();
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

/// One borrowed entry in a [`PackedScanChunk`].
///
/// The key points into the chunk's single owned arena; copying or allocating
/// it is unnecessary when the caller can consume the entry synchronously.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PackedScanEntry<'chunk> {
    key: &'chunk [u8],
    record_id: RecordId,
}

impl<'chunk> PackedScanEntry<'chunk> {
    pub const fn key(self) -> &'chunk [u8] {
        self.key
    }

    pub const fn record_id(self) -> RecordId {
        self.record_id
    }
}

/// Borrowing iterator over the metadata and shared key arena of a packed scan.
#[derive(Clone, Debug)]
pub struct PackedScanEntries<'chunk> {
    entries: std::slice::Iter<'chunk, mtree_sys::ScanEntry>,
    key_arena: &'chunk [u8],
}

impl<'chunk> Iterator for PackedScanEntries<'chunk> {
    type Item = PackedScanEntry<'chunk>;

    #[inline]
    fn next(&mut self) -> Option<Self::Item> {
        self.entries.next().map(|entry| {
            let end = entry.key_offset + entry.key_length;
            PackedScanEntry {
                key: &self.key_arena[entry.key_offset..end],
                record_id: RecordId::new(entry.record_id)
                    .expect("packed scan metadata was validated at construction"),
            }
        })
    }

    #[inline]
    fn size_hint(&self) -> (usize, Option<usize>) {
        self.entries.size_hint()
    }
}

impl DoubleEndedIterator for PackedScanEntries<'_> {
    #[inline]
    fn next_back(&mut self) -> Option<Self::Item> {
        self.entries.next_back().map(|entry| {
            let end = entry.key_offset + entry.key_length;
            PackedScanEntry {
                key: &self.key_arena[entry.key_offset..end],
                record_id: RecordId::new(entry.record_id)
                    .expect("packed scan metadata was validated at construction"),
            }
        })
    }
}

impl ExactSizeIterator for PackedScanEntries<'_> {}
impl std::iter::FusedIterator for PackedScanEntries<'_> {}

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

/// Borrowed continuation metadata for a [`PackedScanChunk`].
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PackedScanResume<'chunk> {
    /// The range is exhausted.
    None,
    /// No entry fit; grow the limiting buffer and retry the identical request.
    UnchangedInput,
    /// Continue exclusively after (forward) or before (reverse) this key.
    Exclusive(&'chunk [u8]),
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum PackedScanResumeMetadata {
    None,
    UnchangedInput,
    Exclusive { offset: usize, length: usize },
}

/// Reusable caller-owned storage for packed native scans.
///
/// Storage grows on demand and is retained until this value is dropped. It is
/// deliberately opaque: only the packed scan methods on [`Tree`] may populate
/// it, and their validated result borrows it for the duration of inspection.
#[derive(Debug, Default)]
pub struct PackedScanScratch {
    pub(crate) entries: Vec<mtree_sys::ScanEntry>,
    pub(crate) key_arena: Vec<u8>,
    pub(crate) record_ids: Vec<mtree_sys::RecordId>,
    pub(crate) continuation_key: Vec<u8>,
}

impl PackedScanScratch {
    /// Number of entry descriptors currently retained for reuse.
    pub const fn entry_capacity(&self) -> usize {
        self.entries.len()
    }

    /// Number of key-arena bytes currently retained for reuse.
    pub const fn key_arena_capacity(&self) -> usize {
        self.key_arena.len()
    }
}

/// Continuation state for the private bounded RecordId scan.
#[doc(hidden)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum BoundedRecordIdScanResume<'chunk> {
    None,
    UnchangedInput,
    InclusiveNext(&'chunk [u8]),
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum BoundedRecordIdScanResumeMetadata {
    None,
    UnchangedInput,
    InclusiveNext,
}

/// One private native scan chunk containing IDs and no copied row keys.
#[doc(hidden)]
#[derive(Clone, Copy, Debug)]
pub struct BoundedRecordIdScanChunkRef<'scratch> {
    pub(crate) record_ids: &'scratch [mtree_sys::RecordId],
    pub(crate) continuation_key: &'scratch [u8],
    pub(crate) stop_reason: ScanStopReason,
    pub(crate) resume: BoundedRecordIdScanResumeMetadata,
    pub(crate) next_key_bytes_required: usize,
}

impl<'scratch> BoundedRecordIdScanChunkRef<'scratch> {
    #[inline]
    pub fn record_ids(self) -> impl ExactSizeIterator<Item = RecordId> + 'scratch {
        self.record_ids.iter().copied().map(|record_id| {
            RecordId::new(record_id).expect("bounded RecordId scan validated every token")
        })
    }

    pub const fn len(self) -> usize {
        self.record_ids.len()
    }

    pub const fn is_empty(self) -> bool {
        self.record_ids.is_empty()
    }

    pub const fn stop_reason(self) -> ScanStopReason {
        self.stop_reason
    }

    pub const fn resume(self) -> BoundedRecordIdScanResume<'scratch> {
        match self.resume {
            BoundedRecordIdScanResumeMetadata::None => BoundedRecordIdScanResume::None,
            BoundedRecordIdScanResumeMetadata::UnchangedInput => {
                BoundedRecordIdScanResume::UnchangedInput
            }
            BoundedRecordIdScanResumeMetadata::InclusiveNext => {
                BoundedRecordIdScanResume::InclusiveNext(self.continuation_key)
            }
        }
    }

    pub const fn next_key_bytes_required(self) -> usize {
        self.next_key_bytes_required
    }
}

/// One validated packed directory chunk borrowing caller-owned scan scratch.
#[derive(Clone, Copy, Debug)]
pub struct PackedScanChunkRef<'scratch> {
    pub(crate) entries: &'scratch [mtree_sys::ScanEntry],
    pub(crate) key_arena: &'scratch [u8],
    pub(crate) stop_reason: ScanStopReason,
    pub(crate) resume: PackedScanResumeMetadata,
    pub(crate) next_key_bytes_required: usize,
}

impl<'scratch> PackedScanChunkRef<'scratch> {
    #[inline]
    pub fn entries(self) -> PackedScanEntries<'scratch> {
        PackedScanEntries {
            entries: self.entries.iter(),
            key_arena: self.key_arena,
        }
    }

    pub const fn len(self) -> usize {
        self.entries.len()
    }

    pub const fn is_empty(self) -> bool {
        self.entries.is_empty()
    }

    pub const fn stop_reason(self) -> ScanStopReason {
        self.stop_reason
    }

    pub fn resume(self) -> PackedScanResume<'scratch> {
        match self.resume {
            PackedScanResumeMetadata::None => PackedScanResume::None,
            PackedScanResumeMetadata::UnchangedInput => PackedScanResume::UnchangedInput,
            PackedScanResumeMetadata::Exclusive { offset, length } => {
                PackedScanResume::Exclusive(&self.key_arena[offset..offset + length])
            }
        }
    }

    pub const fn next_key_bytes_required(self) -> usize {
        self.next_key_bytes_required
    }
}

/// One fully owned, validated directory chunk using a single packed key arena.
///
/// Each entry occupies only its offset, length, and immutable record ID in the
/// metadata vector. Use [`Self::entries`] and [`Self::resume`] to borrow keys
/// without allocating one object per result.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PackedScanChunk {
    pub(crate) entries: Vec<mtree_sys::ScanEntry>,
    pub(crate) key_arena: Vec<u8>,
    pub(crate) stop_reason: ScanStopReason,
    pub(crate) resume: PackedScanResumeMetadata,
    pub(crate) next_key_bytes_required: usize,
}

impl PackedScanChunk {
    #[inline]
    pub fn entries(&self) -> PackedScanEntries<'_> {
        PackedScanEntries {
            entries: self.entries.iter(),
            key_arena: &self.key_arena,
        }
    }

    pub const fn len(&self) -> usize {
        self.entries.len()
    }

    pub const fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    pub const fn stop_reason(&self) -> ScanStopReason {
        self.stop_reason
    }

    pub fn resume(&self) -> PackedScanResume<'_> {
        match self.resume {
            PackedScanResumeMetadata::None => PackedScanResume::None,
            PackedScanResumeMetadata::UnchangedInput => PackedScanResume::UnchangedInput,
            PackedScanResumeMetadata::Exclusive { offset, length } => {
                PackedScanResume::Exclusive(&self.key_arena[offset..offset + length])
            }
        }
    }

    pub const fn next_key_bytes_required(&self) -> usize {
        self.next_key_bytes_required
    }

    /// Converts to the compatibility representation with one allocation per
    /// key. Prefer consuming the packed iterator directly on hot paths.
    pub fn try_into_owned(self) -> Result<ScanChunk, Error> {
        let Self {
            entries: packed_entries,
            key_arena,
            stop_reason,
            resume,
            next_key_bytes_required,
        } = self;
        let mut entries = Vec::new();
        entries
            .try_reserve_exact(packed_entries.len())
            .map_err(|_| Error::AllocationLimit {
                requested: packed_entries.len(),
            })?;
        for packed in packed_entries {
            let end = packed.key_offset + packed.key_length;
            let key = &key_arena[packed.key_offset..end];
            let mut owned = Vec::new();
            owned
                .try_reserve_exact(key.len())
                .map_err(|_| Error::AllocationLimit {
                    requested: key.len(),
                })?;
            owned.extend_from_slice(key);
            entries.push(ScanEntry {
                key: owned.into_boxed_slice(),
                record_id: RecordId::new(packed.record_id)
                    .expect("packed scan metadata was validated at construction"),
            });
        }
        let resume = match resume {
            PackedScanResumeMetadata::None => ScanResume::None,
            PackedScanResumeMetadata::UnchangedInput => ScanResume::UnchangedInput,
            PackedScanResumeMetadata::Exclusive { offset, length } => {
                ScanResume::Exclusive(key_arena[offset..offset + length].into())
            }
        };
        Ok(ScanChunk {
            entries,
            stop_reason,
            resume,
            next_key_bytes_required,
        })
    }
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
    fn point_read_result_is_one_native_record_id_slot() {
        assert_eq!(
            std::mem::size_of::<PointReadResult>(),
            std::mem::size_of::<u64>()
        );
        assert_eq!(
            std::mem::align_of::<PointReadResult>(),
            std::mem::align_of::<u64>()
        );
        assert_eq!(PointReadResult::default().record_id(), None);
        assert_eq!(
            PointReadResult(u64::MAX).record_id().unwrap().get(),
            u64::MAX
        );
    }

    #[test]
    fn fixed_insert_result_matches_native_layout_and_rejects_hostile_fields() {
        assert_eq!(
            std::mem::size_of::<FixedInsertResult>(),
            std::mem::size_of::<mtree_sys::GetOrInsertResult>()
        );
        assert_eq!(
            std::mem::align_of::<FixedInsertResult>(),
            std::mem::align_of::<mtree_sys::GetOrInsertResult>()
        );
        let candidate = RecordId::new(7).unwrap();
        assert_eq!(
            FixedInsertResult::default()
                .classification(candidate)
                .unwrap(),
            (PublicationDisposition::FailureBeforePublication, None)
        );
        let malformed = FixedInsertResult {
            winner: candidate.get(),
            publication: mtree_sys::PUBLICATION_CANDIDATE_INSERTED,
            inserted: 0,
            reserved: [0; 3],
        };
        assert_eq!(
            malformed.classification(candidate),
            Err(Error::InvalidPublication)
        );
        let malformed = FixedInsertResult {
            reserved: [1, 0, 0],
            ..FixedInsertResult::default()
        };
        assert_eq!(
            malformed.classification(candidate),
            Err(Error::InvalidPublication)
        );
    }

    #[test]
    fn every_known_native_status_is_classified_without_boolean_loss() {
        for raw in 1..=18 {
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
    fn packed_scan_borrows_binary_keys_and_converts_to_owned_compatibility() {
        let chunk = PackedScanChunk {
            entries: vec![
                mtree_sys::ScanEntry {
                    key_offset: 0,
                    key_length: 0,
                    record_id: 7,
                },
                mtree_sys::ScanEntry {
                    key_offset: 0,
                    key_length: 3,
                    record_id: 8,
                },
            ],
            key_arena: vec![0, 0xff, b'k'],
            stop_reason: ScanStopReason::EntryCapacity,
            resume: PackedScanResumeMetadata::Exclusive {
                offset: 0,
                length: 3,
            },
            next_key_bytes_required: 5,
        };

        let entries: Vec<_> = chunk.entries().collect();
        assert_eq!(entries[0].key(), b"");
        assert_eq!(entries[0].record_id().get(), 7);
        assert_eq!(entries[1].key(), &[0, 0xff, b'k']);
        assert_eq!(
            chunk.resume(),
            PackedScanResume::Exclusive(&[0, 0xff, b'k'])
        );

        let owned = chunk.try_into_owned().unwrap();
        assert_eq!(owned.entries()[0].key(), b"");
        assert_eq!(owned.entries()[1].key(), &[0, 0xff, b'k']);
        assert_eq!(
            owned.resume(),
            &ScanResume::Exclusive(Box::from(&[0, 0xff, b'k'][..]))
        );
    }

    #[test]
    fn shareable_facades_are_send_sync_without_unsafe_impls() {
        fn assert_send_sync<T: Send + Sync>() {}

        assert_send_sync::<Runtime>();
        assert_send_sync::<Tree>();
        assert_send_sync::<ScanEntry>();
        assert_send_sync::<ScanChunk>();
        assert_send_sync::<PackedScanChunk>();
        assert_send_sync::<PackedScanScratch>();
        assert_send_sync::<PackedScanEntry<'static>>();
        assert_send_sync::<PackedScanEntries<'static>>();
    }
}
