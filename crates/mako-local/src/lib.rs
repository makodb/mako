//! Safe local transactions over Mako's existing C++ STO/MassTrans engine.
//!
//! This crate is deliberately an ownership layer, not a second transaction
//! implementation. C++ still owns OCC, Masstree, validation, installation,
//! and RCU; Rust owns handle lifetimes, thread affinity, byte ownership, and
//! error typing.
//!
//! ```no_run
//! # fn main() -> Result<(), Box<dyn std::error::Error>> {
//! let db = mako_local::LocalDb::open()?;
//! let accounts = db.open_table("accounts", 1)?;
//! let mut tx = db.transaction()?;
//! tx.put(&accounts, b"alice", b"10")?;
//! tx.put(&accounts, b"bob", b"20")?;
//! tx.commit()?;
//! # Ok(()) }
//! ```
//!
//! Transactions are intentionally neither `Send` nor `Sync`: STO state is
//! attached to the current OS thread. Do not hold one across `.await`, even on
//! a single-thread executor, because another task could attempt to begin a
//! transaction on the same ambient native state.

#![warn(missing_docs)]

use std::ffi::c_void;
use std::fmt;
use std::marker::PhantomData;
use std::num::NonZeroU64;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr::NonNull;
use std::rc::Rc;

use mako_local_sys as sys;

/// A failure reported by the native local transaction boundary.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    /// Optimistic validation or write locking rejected the transaction.
    Conflict,
    /// The current worker was not attached to STO.
    NotAttached,
    /// A thread-affine transaction was used from another OS thread.
    WrongThread,
    /// This OS thread already has one active transaction.
    TransactionAlreadyActive,
    /// An operation targeted a transaction that has already ended.
    TransactionFinished,
    /// A table from a different local database was supplied.
    WrongDatabaseOrTable,
    /// The ABI rejected an invalid pointer or length.
    InvalidArgument,
    /// STO's 460 process-lifetime worker slots are exhausted.
    ThreadLimit,
    /// A database or native thread resource is still in use.
    Busy,
    /// Native allocation failed.
    OutOfMemory,
    /// A catchable C++ exception was contained at the boundary.
    Internal,
    /// This engine build cannot safely compose two writes to one key yet.
    DuplicateWrite,
    /// The transaction exceeded a native item or write-set limit and was aborted.
    TransactionTooLarge,
    /// A key or value exceeded a native representation limit.
    ValueTooLarge,
    /// The post-validation durability hook rejected the transaction before install.
    CommitHookRejected,
    /// Silo's process-wide 64-bit commit timestamp space is exhausted.
    TimestampExhausted,
    /// Rust declarations and the linked C++ ABI have different versions.
    AbiMismatch {
        /// Version compiled into the Rust declarations.
        expected: u32,
        /// Version reported by the linked C++ library.
        found: u32,
    },
    /// The native library returned a status unknown to this crate.
    UnknownStatus(i32),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Conflict => write!(f, "local transaction conflict"),
            Self::NotAttached => write!(f, "worker thread is not attached to STO"),
            Self::WrongThread => write!(f, "transaction used from a different OS thread"),
            Self::TransactionAlreadyActive => {
                write!(f, "this worker already has an active transaction")
            }
            Self::TransactionFinished => write!(f, "transaction has already finished"),
            Self::WrongDatabaseOrTable => write!(f, "table belongs to another database"),
            Self::InvalidArgument => write!(f, "invalid argument reached the local ABI"),
            Self::ThreadLimit => write!(
                f,
                "STO's 460 process-lifetime thread slots are exhausted; use a fixed worker pool"
            ),
            Self::Busy => write!(f, "native resource is busy"),
            Self::OutOfMemory => write!(f, "native allocation failed"),
            Self::Internal => write!(f, "the local ABI contained a C++ failure"),
            Self::DuplicateWrite => write!(
                f,
                "a transaction cannot mutate the same table/key twice in the draft ABI"
            ),
            Self::TransactionTooLarge => {
                write!(
                    f,
                    "local transaction exceeded a native size limit and was aborted"
                )
            }
            Self::ValueTooLarge => write!(f, "key or value exceeded a native size limit"),
            Self::CommitHookRejected => {
                write!(f, "post-validation commit hook rejected the transaction")
            }
            Self::TimestampExhausted => write!(f, "Silo commit timestamp exhausted"),
            Self::AbiMismatch { expected, found } => write!(
                f,
                "mako-local ABI mismatch: Rust expects {expected}, linked C++ reports {found}"
            ),
            Self::UnknownStatus(status) => {
                write!(f, "mako-local returned unknown status {status}")
            }
        }
    }
}

impl std::error::Error for Error {}

/// This crate's result type.
pub type Result<T> = std::result::Result<T, Error>;

/// A raw, nonzero 64-bit Silo/STO transaction serialization timestamp.
///
/// This is the exact `Transaction::commit_tid_` value, including its native
/// spacing. It is distinct from both the cache commit sequence and Mako's
/// distributed `tid_unique_` timestamp.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
#[repr(transparent)]
pub struct SiloTimestamp(NonZeroU64);

impl SiloTimestamp {
    /// Construct a timestamp, returning `None` for Silo's unassigned sentinel.
    pub const fn new(raw: u64) -> Option<Self> {
        match NonZeroU64::new(raw) {
            Some(raw) => Some(Self(raw)),
            None => None,
        }
    }

    /// Return the exact raw native TID.
    pub const fn get(self) -> u64 {
        self.0.get()
    }
}

/// Whether a native commit definitely became visible.
///
/// This is deliberately separate from handle cleanup. A transaction can be
/// installed successfully and then encounter a failure while destroying its
/// small facade handle; a write-back cache must still publish that committed
/// transaction to its durability log.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[must_use = "commit visibility must be handled before releasing durability coverage"]
pub enum CommitDisposition {
    /// Validation succeeded and the transaction's writes are visible.
    Committed,
    /// The transaction definitely did not become visible.
    Aborted(Error),
    /// The native boundary could not prove whether installation occurred.
    Unknown(Error),
}

/// The two independently relevant results of consuming a transaction.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[must_use = "commit visibility and handle cleanup are independent outcomes"]
pub struct CommitReport {
    /// Whether the transaction became visible.
    pub disposition: CommitDisposition,
    /// Whether the now-terminal facade handle was destroyed cleanly.
    pub cleanup: Result<()>,
}

/// Maximum table-name length accepted by the draft ABI.
pub const MAX_TABLE_NAME_BYTES: usize = sys::MAKO_LOCAL_MAX_TABLE_NAME_BYTES;
/// Maximum key length accepted by the draft ABI.
pub const MAX_KEY_BYTES: usize = sys::MAKO_LOCAL_MAX_KEY_BYTES;
/// Maximum value length accepted by the draft ABI.
pub const MAX_VALUE_BYTES: usize = sys::MAKO_LOCAL_MAX_VALUE_BYTES;
/// Weighted native item budget for one draft transaction.
pub const TRANSACTION_ITEM_BUDGET: usize = sys::MAKO_LOCAL_TXN_ITEM_BUDGET;

/// Compile-time behavior exposed by the linked C++ STO build.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Features(u64);

impl Features {
    /// Local point reads and writes inside atomic transactions are available.
    pub const fn point_transactions(self) -> bool {
        self.0 & sys::MAKO_LOCAL_FEATURE_POINT_TRANSACTIONS != 0
    }

    /// Reads in a transaction are guaranteed to observe its earlier writes.
    pub const fn read_my_writes(self) -> bool {
        self.0 & sys::MAKO_LOCAL_FEATURE_READ_MY_WRITES != 0
    }

    /// The native engine was built with STO opacity checks enabled.
    pub const fn opacity(self) -> bool {
        self.0 & sys::MAKO_LOCAL_FEATURE_OPACITY != 0
    }

    /// Raw ABI feature bits, including future bits unknown to this crate.
    pub const fn bits(self) -> u64 {
        self.0
    }
}

/// Return capabilities of the linked native engine after verifying its ABI.
pub fn features() -> Result<Features> {
    verify_abi()?;
    // SAFETY: pure ABI identity accessor.
    Ok(Features(unsafe { sys::mako_local_feature_bits() }))
}

fn verify_abi() -> Result<()> {
    // SAFETY: pure ABI identity accessor.
    let found = unsafe { sys::mako_local_abi_version() };
    if found == sys::MAKO_LOCAL_ABI_VERSION {
        Ok(())
    } else {
        Err(Error::AbiMismatch {
            expected: sys::MAKO_LOCAL_ABI_VERSION,
            found,
        })
    }
}

fn status(code: i32) -> Result<()> {
    match code {
        sys::MAKO_LOCAL_OK => Ok(()),
        sys::MAKO_LOCAL_CONFLICT => Err(Error::Conflict),
        sys::MAKO_LOCAL_NOT_ATTACHED => Err(Error::NotAttached),
        sys::MAKO_LOCAL_WRONG_THREAD => Err(Error::WrongThread),
        sys::MAKO_LOCAL_TXN_ALREADY_ACTIVE => Err(Error::TransactionAlreadyActive),
        sys::MAKO_LOCAL_TXN_FINISHED => Err(Error::TransactionFinished),
        sys::MAKO_LOCAL_WRONG_DB_OR_TABLE => Err(Error::WrongDatabaseOrTable),
        sys::MAKO_LOCAL_INVALID_ARGUMENT => Err(Error::InvalidArgument),
        sys::MAKO_LOCAL_THREAD_LIMIT => Err(Error::ThreadLimit),
        sys::MAKO_LOCAL_BUSY => Err(Error::Busy),
        sys::MAKO_LOCAL_OUT_OF_MEMORY => Err(Error::OutOfMemory),
        sys::MAKO_LOCAL_INTERNAL => Err(Error::Internal),
        sys::MAKO_LOCAL_DUPLICATE_WRITE => Err(Error::DuplicateWrite),
        sys::MAKO_LOCAL_TXN_TOO_LARGE => Err(Error::TransactionTooLarge),
        sys::MAKO_LOCAL_VALUE_TOO_LARGE => Err(Error::ValueTooLarge),
        sys::MAKO_LOCAL_COMMIT_HOOK_REJECTED => Err(Error::CommitHookRejected),
        sys::MAKO_LOCAL_TIMESTAMP_EXHAUSTED => Err(Error::TimestampExhausted),
        other => Err(Error::UnknownStatus(other)),
    }
}

fn operation_is_terminal(code: i32) -> bool {
    matches!(
        code,
        sys::MAKO_LOCAL_CONFLICT
            | sys::MAKO_LOCAL_OUT_OF_MEMORY
            | sys::MAKO_LOCAL_INTERNAL
            | sys::MAKO_LOCAL_TXN_TOO_LARGE
    )
}

fn commit_disposition(code: i32) -> CommitDisposition {
    match code {
        sys::MAKO_LOCAL_OK => CommitDisposition::Committed,
        sys::MAKO_LOCAL_CONFLICT => CommitDisposition::Aborted(Error::Conflict),
        sys::MAKO_LOCAL_COMMIT_HOOK_REJECTED => {
            CommitDisposition::Aborted(Error::CommitHookRejected)
        }
        sys::MAKO_LOCAL_TIMESTAMP_EXHAUSTED => {
            CommitDisposition::Aborted(Error::TimestampExhausted)
        }
        other => {
            CommitDisposition::Unknown(status(other).expect_err("non-success native commit status"))
        }
    }
}

/// Advance Silo's process-wide commit clock past a recovered durable timestamp.
///
/// This operation is atomic and monotonic and does not require attaching the
/// calling thread. `observed` must be an exact timestamp previously returned by
/// a Silo post-validation hook. Calling this before admitting new transactions
/// prevents timestamp reuse after process recovery. [`Error::TimestampExhausted`]
/// means advancing would leave no timestamp that a subsequent checked commit
/// could mint.
pub fn advance_commit_tid_past(observed: SiloTimestamp) -> Result<()> {
    verify_abi()?;
    // SAFETY: scalar-only process-global monotonic operation.
    status(unsafe { sys::mako_local_advance_commit_tid_past(observed.get()) })
}

fn attach_current_thread() -> Result<()> {
    // SAFETY: no pointer arguments; native side is idempotent per OS thread.
    status(unsafe { sys::mako_local_thread_attach() })
}

/// A local in-memory Mako database using the C++ STO/MassTrans engine.
///
/// The facade can be shared between fixed, long-lived workers. Its underlying
/// MassTrans tables remain process-lifetime in this draft; dropping this value
/// releases the facade handles after all safe Rust borrows have ended.
pub struct LocalDb {
    raw: NonNull<sys::mako_local_db>,
}

// SAFETY: table-map mutation is protected by the native database mutex, and
// MassTrans is designed for concurrent access. Transactions themselves carry
// the thread-affinity restriction and are not Send/Sync.
unsafe impl Send for LocalDb {}
// SAFETY: as above.
unsafe impl Sync for LocalDb {}

impl LocalDb {
    /// Open a new local database facade and attach the calling worker.
    pub fn open() -> Result<Self> {
        verify_abi()?;
        attach_current_thread()?;
        let mut raw = std::ptr::null_mut();
        // SAFETY: `raw` is a valid out-pointer and is checked before use.
        status(unsafe { sys::mako_local_db_open(&mut raw) })?;
        let raw = NonNull::new(raw).ok_or(Error::Internal)?;
        Ok(Self { raw })
    }

    /// Find or create a table with a stable numeric identifier.
    ///
    /// Reopening `name` with another identifier is an error. Names and later
    /// keys are binary too. Both `&str` and byte slices implement
    /// `AsRef<[u8]>`.
    pub fn open_table(&self, name: impl AsRef<[u8]>, table_id: u64) -> Result<Table<'_>> {
        attach_current_thread()?;
        let name = name.as_ref();
        let mut raw = std::ptr::null_mut();
        // SAFETY: database is live; name is borrowed for this call only; raw
        // is a valid out-pointer. Native code copies the name.
        status(unsafe {
            sys::mako_local_table_open(
                self.raw.as_ptr(),
                name.as_ptr(),
                name.len(),
                table_id,
                &mut raw,
            )
        })?;
        let raw = NonNull::new(raw).ok_or(Error::Internal)?;
        Ok(Table {
            raw,
            _db: PhantomData,
        })
    }

    /// Begin one transaction on the current OS thread.
    ///
    /// Conflicts are returned by [`Transaction::commit`]; this method never
    /// retries user code implicitly.
    pub fn transaction(&self) -> Result<Transaction<'_>> {
        attach_current_thread()?;
        let mut raw = std::ptr::null_mut();
        // SAFETY: database is live and raw is a valid out-pointer.
        status(unsafe { sys::mako_local_txn_begin(self.raw.as_ptr(), &mut raw) })?;
        let raw = NonNull::new(raw).ok_or(Error::Internal)?;
        Ok(Transaction {
            raw: Some(raw),
            active: true,
            _db: PhantomData,
            _thread_affine: PhantomData,
        })
    }

    /// Explicitly close and consume this database facade.
    ///
    /// Safe borrows ensure that no ordinary Rust transaction is live here.
    /// If native close nevertheless fails (for example, because a transaction
    /// was deliberately forgotten), the handle is not retried: an error does
    /// not establish that a native handle remains valid, so retrying from
    /// [`Drop`] could double-close it. The native resource may therefore remain
    /// allocated on an error path.
    pub fn close(self) -> Result<()> {
        let this = std::mem::ManuallyDrop::new(self);
        // SAFETY: consuming `self` makes this the facade handle's final use.
        // ManuallyDrop prevents a second close regardless of the status.
        status(unsafe { sys::mako_local_db_close(this.raw.as_ptr()) })
    }
}

impl Drop for LocalDb {
    fn drop(&mut self) {
        // SAFETY: this is the unique facade handle returned by open. If a
        // transaction was deliberately forgotten, native close returns BUSY
        // without freeing anything; leaking is required for memory safety.
        let _ = unsafe { sys::mako_local_db_close(self.raw.as_ptr()) };
    }
}

/// A table borrowed from a [`LocalDb`]. It cannot escape that database:
///
/// ```compile_fail
/// let table = {
///     let db = mako_local::LocalDb::open().unwrap();
///     db.open_table("short-lived", 1).unwrap()
/// };
/// let _ = table.id();
/// ```
#[derive(Clone, Copy)]
pub struct Table<'db> {
    raw: NonNull<sys::mako_local_table>,
    _db: PhantomData<&'db LocalDb>,
}

// SAFETY: this is a borrowed handle to a concurrent MassTrans table.
unsafe impl Send for Table<'_> {}
// SAFETY: as above.
unsafe impl Sync for Table<'_> {}

impl Table<'_> {
    /// Stable table identifier supplied at open.
    pub fn id(&self) -> u64 {
        // SAFETY: table handle remains live through its database borrow.
        unsafe { sys::mako_local_table_id(self.raw.as_ptr()) }
    }
}

/// One local optimistic transaction.
///
/// `Rc` in the marker makes this type neither `Send` nor `Sync`; the native
/// read/write set is ambient TLS. Dropping an active transaction aborts it.
/// The compiler therefore rejects moving it even to a scoped worker:
///
/// ```compile_fail
/// # let db = mako_local::LocalDb::open().unwrap();
/// let tx = db.transaction().unwrap();
/// std::thread::scope(|scope| {
///     scope.spawn(move || drop(tx));
/// });
/// ```
/// Sharing a reference is rejected too, because the type is not `Sync`:
///
/// ```compile_fail
/// # let db = mako_local::LocalDb::open().unwrap();
/// let tx = db.transaction().unwrap();
/// std::thread::scope(|scope| {
///     scope.spawn(|| assert!(std::mem::size_of_val(&tx) > 0));
/// });
/// ```
pub struct Transaction<'db> {
    raw: Option<NonNull<sys::mako_local_txn>>,
    active: bool,
    _db: PhantomData<&'db LocalDb>,
    _thread_affine: PhantomData<Rc<()>>,
}

impl<'db> Transaction<'db> {
    fn active_raw(&self) -> Result<*mut sys::mako_local_txn> {
        if !self.active {
            return Err(Error::TransactionFinished);
        }
        Ok(self
            .raw
            .expect("transaction handle already consumed")
            .as_ptr())
    }

    fn operation_status(&mut self, code: i32) -> Result<()> {
        // The native facade aborts and finishes an operation that encounters
        // these terminal failures. Value-too-large is checked before native
        // mutation and, like other contract errors, leaves it abortable.
        if operation_is_terminal(code) {
            self.active = false;
        }
        status(code)
    }

    /// Read a key, returning owned bytes. Missing and present-empty are
    /// distinct (`None` versus `Some(Vec::new())`).
    pub fn get(&mut self, table: &Table<'db>, key: &[u8]) -> Result<Option<Vec<u8>>> {
        let mut bytes = std::ptr::null_mut();
        let mut len = 0usize;
        let mut found = 0u8;
        // SAFETY: all slices remain valid for the call, out-pointers are live,
        // and the native result is copied before being freed below.
        let code = unsafe {
            sys::mako_local_txn_get(
                self.active_raw()?,
                table.raw.as_ptr(),
                key.as_ptr(),
                key.len(),
                &mut bytes,
                &mut len,
                &mut found,
            )
        };
        let owned = ForeignBytes(bytes);
        self.operation_status(code)?;
        if found == 0 {
            return Ok(None);
        }
        let bytes = NonNull::new(owned.0).ok_or(Error::Internal)?;
        // SAFETY: native get allocated at least one byte and reports the
        // initialized payload length; ForeignBytes keeps it live through copy.
        let result = unsafe { std::slice::from_raw_parts(bytes.as_ptr(), len) }.to_vec();
        Ok(Some(result))
    }

    /// Upsert `key`, returning `true` when it was newly created.
    pub fn put(&mut self, table: &Table<'db>, key: &[u8], value: &[u8]) -> Result<bool> {
        let mut created = 0u8;
        // SAFETY: input slices live through the call. C++ copies/encodes the
        // value into transaction-owned stable storage before returning.
        let code = unsafe {
            sys::mako_local_txn_put(
                self.active_raw()?,
                table.raw.as_ptr(),
                key.as_ptr(),
                key.len(),
                value.as_ptr(),
                value.len(),
                &mut created,
            )
        };
        self.operation_status(code)?;
        Ok(created != 0)
    }

    /// Insert only when absent, returning whether insertion was staged.
    pub fn insert(&mut self, table: &Table<'db>, key: &[u8], value: &[u8]) -> Result<bool> {
        let mut inserted = 0u8;
        // SAFETY: same ownership contract as put.
        let code = unsafe {
            sys::mako_local_txn_insert(
                self.active_raw()?,
                table.raw.as_ptr(),
                key.as_ptr(),
                key.len(),
                value.as_ptr(),
                value.len(),
                &mut inserted,
            )
        };
        self.operation_status(code)?;
        Ok(inserted != 0)
    }

    /// Remove a key, returning whether a live value existed.
    pub fn remove(&mut self, table: &Table<'db>, key: &[u8]) -> Result<bool> {
        let mut existed = 0u8;
        // SAFETY: key slice lives through the call and existed is writable.
        let code = unsafe {
            sys::mako_local_txn_remove(
                self.active_raw()?,
                table.raw.as_ptr(),
                key.as_ptr(),
                key.len(),
                &mut existed,
            )
        };
        self.operation_status(code)?;
        Ok(existed != 0)
    }

    /// Validate and atomically install the transaction's local writes.
    ///
    /// Consumes the handle. [`Error::Conflict`] is a normal OCC outcome.
    /// A non-conflict error can theoretically be handle cleanup failing after
    /// a successful install; durability integrations must use
    /// [`Self::commit_report`] so they do not lose that distinction.
    pub fn commit(self) -> Result<()> {
        let report = self.commit_report();
        match report.disposition {
            CommitDisposition::Committed => report.cleanup,
            CommitDisposition::Aborted(error) | CommitDisposition::Unknown(error) => Err(error),
        }
    }

    /// Commit while preserving visibility and cleanup as separate outcomes.
    ///
    /// Durability adapters should use this method instead of [`Self::commit`].
    /// A [`CommitDisposition::Committed`] transaction must be handed to
    /// durable write-back even when `cleanup` is an error. Conversely, an
    /// [`CommitDisposition::Unknown`] result must not be treated as an abort;
    /// pinning the corresponding durability obligation is the safe response.
    pub fn commit_report(self) -> CommitReport {
        self.finish_commit(|raw| {
            // SAFETY: handle is live, active, and cannot have moved threads.
            unsafe { sys::mako_local_txn_commit(raw) }
        })
    }

    /// Commit with an allocation-free post-validation, pre-install hook.
    ///
    /// Native Silo assigns the timestamp after locking the full write set and
    /// before validation. `hook` runs only after validation succeeds, while all
    /// write locks remain held and before any write becomes visible. It should
    /// therefore only bind already-owned storage to an already-reserved queue
    /// slot. It may enter a bounded in-memory critical section, but must not do
    /// I/O, wait for capacity, allocate, or unwind. Returning `false` definitely
    /// aborts the transaction with [`Error::CommitHookRejected`].
    ///
    /// In builds with panic unwinding, a panic is contained inside the Rust
    /// trampoline and treated exactly like `false`. In a `panic = "abort"`
    /// build (including this workspace's release profile), a panic terminates
    /// the process before it can cross the C boundary, so hooks must not panic.
    /// Read-only and conflicting transactions do not invoke the hook.
    pub fn commit_report_with_hook<F>(self, hook: F) -> CommitReport
    where
        F: FnOnce(SiloTimestamp) -> bool,
    {
        let mut state = PostValidateHook { hook: Some(hook) };
        self.finish_commit(|raw| {
            // SAFETY: native invokes the callback synchronously at most once
            // and does not retain the stack context after returning.
            unsafe {
                sys::mako_local_txn_commit_with_hook(
                    raw,
                    Some(post_validate_trampoline::<F>),
                    std::ptr::from_mut(&mut state).cast::<c_void>(),
                )
            }
        })
    }

    fn finish_commit<F>(mut self, native_commit: F) -> CommitReport
    where
        F: FnOnce(*mut sys::mako_local_txn) -> i32,
    {
        let raw = self
            .raw
            .take()
            .expect("transaction handle already consumed");
        if !self.active {
            // An earlier terminal operation may have ended or quarantined
            // native state. Destroy frees a clean terminal handle or reports
            // quarantine, but commit must never touch either state.
            let destroy = unsafe { sys::mako_local_txn_destroy(raw.as_ptr()) };
            return CommitReport {
                disposition: CommitDisposition::Aborted(Error::TransactionFinished),
                cleanup: status(destroy),
            };
        }
        self.active = false;
        let commit = native_commit(raw.as_ptr());
        // SAFETY: commit is terminal; destroy only frees the facade handle.
        let destroy = unsafe { sys::mako_local_txn_destroy(raw.as_ptr()) };
        let disposition = commit_disposition(commit);
        CommitReport {
            disposition,
            cleanup: status(destroy),
        }
    }

    /// Explicitly abort and consume this transaction.
    pub fn abort(mut self) -> Result<()> {
        let raw = self
            .raw
            .take()
            .expect("transaction handle already consumed");
        self.active = false;
        // SAFETY: handle is live, active, and thread-affine by type.
        let abort = unsafe { sys::mako_local_txn_abort(raw.as_ptr()) };
        // SAFETY: abort is terminal.
        let destroy = unsafe { sys::mako_local_txn_destroy(raw.as_ptr()) };
        status(abort)?;
        status(destroy)
    }
}

struct PostValidateHook<F> {
    hook: Option<F>,
}

unsafe extern "C" fn post_validate_trampoline<F>(context: *mut c_void, raw_timestamp: u64) -> i32
where
    F: FnOnce(SiloTimestamp) -> bool,
{
    if context.is_null() {
        return 0;
    }
    // SAFETY: commit_report_with_hook passes this exact stack value, and the
    // native contract invokes the trampoline synchronously at most once.
    let state = unsafe { &mut *context.cast::<PostValidateHook<F>>() };
    let Some(hook) = state.hook.take() else {
        return 0;
    };
    let Some(timestamp) = SiloTimestamp::new(raw_timestamp) else {
        return 0;
    };
    if catch_unwind(AssertUnwindSafe(|| hook(timestamp))).unwrap_or(false) {
        1
    } else {
        0
    }
}

impl Drop for Transaction<'_> {
    fn drop(&mut self) {
        let Some(raw) = self.raw.take() else {
            return;
        };
        if self.active {
            // SAFETY: !Send keeps Drop on the creator thread in safe Rust.
            let _ = unsafe { sys::mako_local_txn_abort(raw.as_ptr()) };
        }
        // SAFETY: handle is no longer used after this call.
        let _ = unsafe { sys::mako_local_txn_destroy(raw.as_ptr()) };
    }
}

struct ForeignBytes(*mut u8);

impl Drop for ForeignBytes {
    fn drop(&mut self) {
        // SAFETY: null is accepted; any non-null pointer came from native get
        // and is freed exactly once by this guard.
        unsafe { sys::mako_local_bytes_free(self.0.cast::<c_void>()) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn silo_timestamp_reserves_zero_as_unassigned() {
        assert_eq!(SiloTimestamp::new(0), None);
        assert_eq!(SiloTimestamp::new(1).map(SiloTimestamp::get), Some(1));
    }

    #[test]
    fn every_native_status_has_a_typed_mapping() {
        let cases = [
            (sys::MAKO_LOCAL_OK, Ok(())),
            (sys::MAKO_LOCAL_CONFLICT, Err(Error::Conflict)),
            (sys::MAKO_LOCAL_NOT_ATTACHED, Err(Error::NotAttached)),
            (sys::MAKO_LOCAL_WRONG_THREAD, Err(Error::WrongThread)),
            (
                sys::MAKO_LOCAL_TXN_ALREADY_ACTIVE,
                Err(Error::TransactionAlreadyActive),
            ),
            (
                sys::MAKO_LOCAL_TXN_FINISHED,
                Err(Error::TransactionFinished),
            ),
            (
                sys::MAKO_LOCAL_WRONG_DB_OR_TABLE,
                Err(Error::WrongDatabaseOrTable),
            ),
            (
                sys::MAKO_LOCAL_INVALID_ARGUMENT,
                Err(Error::InvalidArgument),
            ),
            (sys::MAKO_LOCAL_THREAD_LIMIT, Err(Error::ThreadLimit)),
            (sys::MAKO_LOCAL_BUSY, Err(Error::Busy)),
            (sys::MAKO_LOCAL_OUT_OF_MEMORY, Err(Error::OutOfMemory)),
            (sys::MAKO_LOCAL_INTERNAL, Err(Error::Internal)),
            (sys::MAKO_LOCAL_DUPLICATE_WRITE, Err(Error::DuplicateWrite)),
            (
                sys::MAKO_LOCAL_TXN_TOO_LARGE,
                Err(Error::TransactionTooLarge),
            ),
            (sys::MAKO_LOCAL_VALUE_TOO_LARGE, Err(Error::ValueTooLarge)),
            (
                sys::MAKO_LOCAL_COMMIT_HOOK_REJECTED,
                Err(Error::CommitHookRejected),
            ),
            (
                sys::MAKO_LOCAL_TIMESTAMP_EXHAUSTED,
                Err(Error::TimestampExhausted),
            ),
        ];

        for (code, expected) in cases {
            assert_eq!(status(code), expected, "status {code}");
        }
        assert_eq!(status(-1), Err(Error::UnknownStatus(-1)));
        assert_eq!(status(i32::MAX), Err(Error::UnknownStatus(i32::MAX)));
    }

    #[test]
    fn terminal_operation_statuses_match_native_lifecycle_contract() {
        for code in [
            sys::MAKO_LOCAL_CONFLICT,
            sys::MAKO_LOCAL_OUT_OF_MEMORY,
            sys::MAKO_LOCAL_INTERNAL,
            sys::MAKO_LOCAL_TXN_TOO_LARGE,
        ] {
            assert!(operation_is_terminal(code), "status {code}");
        }

        assert!(!operation_is_terminal(sys::MAKO_LOCAL_VALUE_TOO_LARGE));
        assert!(!operation_is_terminal(sys::MAKO_LOCAL_DUPLICATE_WRITE));
        assert!(!operation_is_terminal(sys::MAKO_LOCAL_INVALID_ARGUMENT));
    }

    #[test]
    fn detailed_commit_only_calls_a_definite_conflict_aborted() {
        assert_eq!(
            commit_disposition(sys::MAKO_LOCAL_OK),
            CommitDisposition::Committed
        );
        assert_eq!(
            commit_disposition(sys::MAKO_LOCAL_CONFLICT),
            CommitDisposition::Aborted(Error::Conflict)
        );
        assert_eq!(
            commit_disposition(sys::MAKO_LOCAL_COMMIT_HOOK_REJECTED),
            CommitDisposition::Aborted(Error::CommitHookRejected)
        );
        assert_eq!(
            commit_disposition(sys::MAKO_LOCAL_TIMESTAMP_EXHAUSTED),
            CommitDisposition::Aborted(Error::TimestampExhausted)
        );
        for code in [
            sys::MAKO_LOCAL_OUT_OF_MEMORY,
            sys::MAKO_LOCAL_INTERNAL,
            sys::MAKO_LOCAL_TXN_FINISHED,
            -1,
        ] {
            assert!(
                matches!(commit_disposition(code), CommitDisposition::Unknown(_)),
                "commit status {code} must pin a durability obligation"
            );
        }
    }
}
