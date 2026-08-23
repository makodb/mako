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
use std::num::NonZeroU32;
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
    /// Native allocation, or a fallible allocation needed by this wrapper, failed.
    OutOfMemory,
    /// A catchable C++ exception was contained at the boundary.
    Internal,
    /// The linked legacy/no-RYW engine cannot compose two writes to one key.
    DuplicateWrite,
    /// The transaction exceeded a native item or write-set limit and was aborted.
    TransactionTooLarge,
    /// A key or value exceeded a native representation limit.
    ValueTooLarge,
    /// The post-validation durability hook rejected the transaction before install.
    CommitHookRejected,
    /// Mako's process-wide representable base timestamp space is exhausted.
    TimestampExhausted,
    /// A transactional scan needs the scan/read-your-writes native feature.
    TransactionalScansUnavailable,
    /// A raw chunk scan's caller-owned arena could not hold its first result.
    ///
    /// The safe [`Scan`] iterator grows its arena and retries internally, so
    /// ordinary safe callers should not observe this status.
    BufferTooSmall,
    /// Rust declarations and the linked C++ ABI have different versions.
    AbiMismatch {
        /// Version compiled into the Rust declarations.
        expected: u32,
        /// Version reported by the linked C++ library.
        found: u32,
    },
    /// A scan ABI structure has a different native and Rust layout.
    AbiLayoutMismatch {
        /// Structure whose layout did not match.
        structure: &'static str,
        /// Size compiled into this crate.
        expected: usize,
        /// Size reported by the linked native library.
        found: usize,
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
            Self::OutOfMemory => write!(f, "native or wrapper allocation failed"),
            Self::Internal => write!(f, "the local ABI contained a C++ failure"),
            Self::DuplicateWrite => write!(
                f,
                "the linked engine requires read-your-writes support to mutate one key twice"
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
            Self::TimestampExhausted => write!(f, "Mako logical timestamp exhausted"),
            Self::TransactionalScansUnavailable => write!(
                f,
                "the linked engine does not support transactional scans with read-your-writes"
            ),
            Self::BufferTooSmall => write!(f, "transactional scan buffer is too small"),
            Self::AbiMismatch { expected, found } => write!(
                f,
                "mako-local ABI mismatch: Rust expects {expected}, linked C++ reports {found}"
            ),
            Self::AbiLayoutMismatch {
                structure,
                expected,
                found,
            } => write!(
                f,
                "mako-local ABI layout mismatch for {structure}: Rust expects {expected} bytes, linked C++ reports {found}"
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

/// Largest Mako base timestamp representable by its `timestamp * 10 + term`
/// encoding when `term` is a decimal digit.
pub const MAX_MAKO_TIMESTAMP: u32 = sys::MAKO_LOCAL_MAX_MAKO_TIMESTAMP;

/// A nonzero 32-bit Mako logical transaction timestamp.
///
/// This is the exact `Transaction::tid_unique_` value used by Mako's
/// distributed transaction and replication paths. It is distinct from both
/// the cache commit sequence and Silo's internal record-version clock.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
#[repr(transparent)]
pub struct MakoTimestamp(NonZeroU32);

impl MakoTimestamp {
    /// Construct a timestamp, rejecting Mako's zero sentinel and values outside
    /// the legacy one-digit-term encoding's base range.
    pub const fn new(raw: u32) -> Option<Self> {
        match NonZeroU32::new(raw) {
            Some(raw) if raw.get() <= MAX_MAKO_TIMESTAMP => Some(Self(raw)),
            None => None,
            Some(_) => None,
        }
    }

    /// Return the exact raw native logical timestamp.
    pub const fn get(self) -> u32 {
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

    /// Transactional forward and reverse chunk scans are available.
    pub const fn transactional_scans(self) -> bool {
        self.0 & sys::MAKO_LOCAL_FEATURE_TRANSACTIONAL_SCANS != 0
    }

    /// Transactional scans observe earlier writes from the same transaction.
    pub const fn scan_read_my_writes(self) -> bool {
        self.0 & sys::MAKO_LOCAL_FEATURE_SCAN_READ_MY_WRITES != 0
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
    if found != sys::MAKO_LOCAL_ABI_VERSION {
        return Err(Error::AbiMismatch {
            expected: sys::MAKO_LOCAL_ABI_VERSION,
            found,
        });
    }

    for (structure, expected, found) in [
        (
            "mako_local_scan_options",
            sys::MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE as usize,
            // SAFETY: pure ABI identity accessor.
            unsafe { sys::mako_local_scan_options_size() },
        ),
        (
            "mako_local_scan_entry",
            std::mem::size_of::<sys::mako_local_scan_entry>(),
            // SAFETY: pure ABI identity accessor.
            unsafe { sys::mako_local_scan_entry_size() },
        ),
    ] {
        if expected != found {
            return Err(Error::AbiLayoutMismatch {
                structure,
                expected,
                found,
            });
        }
    }
    Ok(())
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
        sys::MAKO_LOCAL_BUFFER_TOO_SMALL => Err(Error::BufferTooSmall),
        other => Err(Error::UnknownStatus(other)),
    }
}

fn operation_is_terminal(code: i32) -> bool {
    match code {
        sys::MAKO_LOCAL_CONFLICT
        | sys::MAKO_LOCAL_OUT_OF_MEMORY
        | sys::MAKO_LOCAL_INTERNAL
        | sys::MAKO_LOCAL_TXN_TOO_LARGE
        | sys::MAKO_LOCAL_TXN_FINISHED => true,
        sys::MAKO_LOCAL_OK
        | sys::MAKO_LOCAL_NOT_ATTACHED
        | sys::MAKO_LOCAL_WRONG_THREAD
        | sys::MAKO_LOCAL_TXN_ALREADY_ACTIVE
        | sys::MAKO_LOCAL_WRONG_DB_OR_TABLE
        | sys::MAKO_LOCAL_INVALID_ARGUMENT
        | sys::MAKO_LOCAL_THREAD_LIMIT
        | sys::MAKO_LOCAL_BUSY
        | sys::MAKO_LOCAL_DUPLICATE_WRITE
        | sys::MAKO_LOCAL_VALUE_TOO_LARGE
        | sys::MAKO_LOCAL_COMMIT_HOOK_REJECTED
        | sys::MAKO_LOCAL_TIMESTAMP_EXHAUSTED
        | sys::MAKO_LOCAL_BUFFER_TOO_SMALL => false,
        // A future operation status has no known lifecycle contract. Never
        // let callers commit a handle whose native state may already be
        // terminal or uncertain.
        _ => true,
    }
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

/// Advance Mako's process-wide logical clock past a durable timestamp.
///
/// This operation is atomic and monotonic and does not require attaching the
/// calling thread. `observed` must be an exact timestamp previously returned by
/// a Mako post-validation hook. Calling this before admitting new transactions
/// prevents timestamp reuse after process recovery. [`Error::TimestampExhausted`]
/// means advancing would leave no timestamp that a subsequent checked commit
/// could mint.
pub fn advance_mako_timestamp_past(observed: MakoTimestamp) -> Result<()> {
    verify_abi()?;
    // SAFETY: scalar-only process-global monotonic operation.
    status(unsafe { sys::mako_local_advance_mako_timestamp_past(observed.get()) })
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

const SCAN_CHUNK_ENTRIES: usize = 64;
const INITIAL_SCAN_ARENA_BYTES: usize = 4 * 1024;

#[derive(Clone, Copy)]
enum ScanDirection {
    Forward,
    Reverse,
}

/// A fallible transactional range iterator.
///
/// Both directions traverse the same logical binary-key range `[lower,
/// upper)`. Forward scans yield ascending keys and reverse scans yield
/// descending keys. Each returned key and value is owned, so no native pointer
/// escapes a chunk call.
///
/// The iterator fetches bounded chunks. Dropping it early keeps the
/// transaction active, but its prefetched rows and predicates remain in the
/// transaction's read set and can conservatively cause a later conflict.
pub struct Scan<'txn, 'db> {
    transaction: &'txn mut Transaction<'db>,
    table: Table<'db>,
    direction: ScanDirection,
    lower: Vec<u8>,
    upper: Option<Vec<u8>>,
    resume: Option<Vec<u8>>,
    entries: Vec<sys::mako_local_scan_entry>,
    arena: Vec<u8>,
    current: std::vec::IntoIter<(Vec<u8>, Vec<u8>)>,
    done: bool,
}

impl<'txn, 'db> Scan<'txn, 'db> {
    fn new(
        transaction: &'txn mut Transaction<'db>,
        table: Table<'db>,
        direction: ScanDirection,
        lower: &[u8],
        upper: Option<&[u8]>,
    ) -> Result<Self> {
        transaction.active_raw()?;
        if lower.len() > MAX_KEY_BYTES || upper.is_some_and(|bound| bound.len() > MAX_KEY_BYTES) {
            return Err(Error::ValueTooLarge);
        }
        let capabilities = features()?;
        if !capabilities.transactional_scans() || !capabilities.scan_read_my_writes() {
            return Err(Error::TransactionalScansUnavailable);
        }

        let lower = copy_scan_bytes(lower)?;
        let upper = upper.map(copy_scan_bytes).transpose()?;

        let mut entries = Vec::new();
        entries
            .try_reserve_exact(SCAN_CHUNK_ENTRIES)
            .map_err(|_| Error::OutOfMemory)?;
        entries.resize(SCAN_CHUNK_ENTRIES, sys::mako_local_scan_entry::default());

        let mut arena = Vec::new();
        arena
            .try_reserve_exact(INITIAL_SCAN_ARENA_BYTES)
            .map_err(|_| Error::OutOfMemory)?;
        arena.resize(INITIAL_SCAN_ARENA_BYTES, 0);

        Ok(Self {
            transaction,
            table,
            direction,
            lower,
            upper,
            resume: None,
            entries,
            arena,
            current: Vec::new().into_iter(),
            // Even a statically empty range must cross the ABI once so native
            // handle/database identity is validated before returning success.
            done: false,
        })
    }

    fn poison(&mut self) {
        if !self.transaction.active {
            return;
        }
        if let Some(raw) = self.transaction.raw {
            // SAFETY: Scan's exclusive transaction borrow proves the live
            // thread-affine handle cannot be used concurrently. A wrapper-side
            // failure must abort so incomplete results cannot later commit.
            let _ = unsafe { sys::mako_local_txn_abort(raw.as_ptr()) };
        }
        self.transaction.active = false;
    }

    fn fail<T>(&mut self, error: Error) -> Result<T> {
        self.poison();
        self.done = true;
        Err(error)
    }

    fn grow_arena(&mut self, required: usize) -> Result<()> {
        let max_entry_bytes = MAX_KEY_BYTES
            .checked_add(MAX_VALUE_BYTES)
            .expect("scan representation limits fit usize");
        if required <= self.arena.len()
            || required > max_entry_bytes
            || required > u32::MAX as usize
        {
            return self.fail(Error::Internal);
        }
        self.arena
            .try_reserve_exact(required - self.arena.len())
            .map_err(|_| Error::OutOfMemory)
            .or_else(|error| self.fail(error))?;
        self.arena.resize(required, 0);
        Ok(())
    }

    fn entry_slice(arena: &[u8], offset: u32, length: u32) -> Option<&[u8]> {
        let start = offset as usize;
        let end = start.checked_add(length as usize)?;
        arena.get(start..end)
    }

    fn key_is_valid(&self, key: &[u8], previous: Option<&[u8]>) -> bool {
        if key < self.lower.as_slice() || self.upper.as_deref().is_some_and(|upper| key >= upper) {
            return false;
        }
        match (self.direction, previous) {
            (ScanDirection::Forward, Some(previous)) => key > previous,
            (ScanDirection::Reverse, Some(previous)) => key < previous,
            (_, None) => true,
        }
    }

    fn refill(&mut self) -> Result<()> {
        loop {
            let mut flags = 0;
            let (upper, upper_len) = match self.upper.as_deref() {
                Some(upper) => {
                    flags |= sys::MAKO_LOCAL_SCAN_HAS_UPPER;
                    (upper.as_ptr(), upper.len())
                }
                None => (std::ptr::null(), 0),
            };
            let (resume, resume_len) = match self.resume.as_deref() {
                Some(resume) => {
                    flags |= sys::MAKO_LOCAL_SCAN_HAS_RESUME;
                    (resume.as_ptr(), resume.len())
                }
                None => (std::ptr::null(), 0),
            };
            let options = sys::mako_local_scan_options {
                struct_size: sys::MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE,
                flags,
                lower: self.lower.as_ptr(),
                lower_len: self.lower.len(),
                upper,
                upper_len,
                resume,
                resume_len,
            };
            let mut entry_count = 0usize;
            let mut arena_used = 0usize;
            let mut arena_required = 0usize;
            let mut done = 0u8;
            let raw = self.transaction.active_raw()?;
            // SAFETY: every input and output buffer remains live and uniquely
            // borrowed for this call. Native code writes no more than the
            // supplied descriptor and arena capacities and retains nothing.
            let code = unsafe {
                match self.direction {
                    ScanDirection::Forward => sys::mako_local_txn_scan_chunk(
                        raw,
                        self.table.raw.as_ptr(),
                        &options,
                        self.entries.as_mut_ptr(),
                        self.entries.len(),
                        self.arena.as_mut_ptr(),
                        self.arena.len(),
                        &mut entry_count,
                        &mut arena_used,
                        &mut arena_required,
                        &mut done,
                    ),
                    ScanDirection::Reverse => sys::mako_local_txn_rscan_chunk(
                        raw,
                        self.table.raw.as_ptr(),
                        &options,
                        self.entries.as_mut_ptr(),
                        self.entries.len(),
                        self.arena.as_mut_ptr(),
                        self.arena.len(),
                        &mut entry_count,
                        &mut arena_used,
                        &mut arena_required,
                        &mut done,
                    ),
                }
            };

            if code == sys::MAKO_LOCAL_BUFFER_TOO_SMALL {
                if entry_count != 0
                    || arena_used != 0
                    || done != 0
                    || arena_required <= self.arena.len()
                {
                    return self.fail(Error::Internal);
                }
                self.grow_arena(arena_required)?;
                continue;
            }
            if let Err(error) = self.transaction.operation_status(code) {
                self.done = true;
                return Err(error);
            }
            if entry_count > self.entries.len()
                || arena_used > self.arena.len()
                || done > 1
                || (entry_count == 0 && done == 0)
            {
                return self.fail(Error::Internal);
            }

            let arena = &self.arena[..arena_used];
            let mut items = Vec::new();
            if items.try_reserve_exact(entry_count).is_err() {
                return self.fail(Error::OutOfMemory);
            }
            for descriptor in &self.entries[..entry_count] {
                let Some(key) =
                    Self::entry_slice(arena, descriptor.key_offset, descriptor.key_length)
                else {
                    return self.fail(Error::Internal);
                };
                let Some(value) =
                    Self::entry_slice(arena, descriptor.value_offset, descriptor.value_length)
                else {
                    return self.fail(Error::Internal);
                };
                let previous = items
                    .last()
                    .map(|(key, _): &(Vec<u8>, Vec<u8>)| key.as_slice())
                    .or(self.resume.as_deref());
                if key.len() > MAX_KEY_BYTES
                    || value.len() > MAX_VALUE_BYTES
                    || !self.key_is_valid(key, previous)
                {
                    return self.fail(Error::Internal);
                }
                let key = match copy_scan_bytes(key) {
                    Ok(key) => key,
                    Err(error) => return self.fail(error),
                };
                let value = match copy_scan_bytes(value) {
                    Ok(value) => value,
                    Err(error) => return self.fail(error),
                };
                items.push((key, value));
            }

            if let Some((last_key, _)) = items.last() {
                self.resume = match copy_scan_bytes(last_key) {
                    Ok(key) => Some(key),
                    Err(error) => return self.fail(error),
                };
            }
            self.current = items.into_iter();
            self.done = done != 0;
            return Ok(());
        }
    }
}

impl Iterator for Scan<'_, '_> {
    type Item = Result<(Vec<u8>, Vec<u8>)>;

    fn next(&mut self) -> Option<Self::Item> {
        if let Some(item) = self.current.next() {
            return Some(Ok(item));
        }
        if self.done {
            return None;
        }
        match self.refill() {
            Ok(()) => self.current.next().map(Ok),
            Err(error) => {
                self.done = true;
                Some(Err(error))
            }
        }
    }
}

fn copy_scan_bytes(bytes: &[u8]) -> Result<Vec<u8>> {
    let mut owned = Vec::new();
    owned
        .try_reserve_exact(bytes.len())
        .map_err(|_| Error::OutOfMemory)?;
    owned.extend_from_slice(bytes);
    Ok(owned)
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

    /// Scan `[lower, upper)` in ascending binary-key order.
    ///
    /// `None` for `upper` means the range is unbounded above. Earlier staged
    /// puts, inserts, and removes are reflected in the iterator.
    pub fn scan<'txn>(
        &'txn mut self,
        table: &Table<'db>,
        lower: &[u8],
        upper: Option<&[u8]>,
    ) -> Result<Scan<'txn, 'db>> {
        Scan::new(self, *table, ScanDirection::Forward, lower, upper)
    }

    /// Scan `[lower, upper)` in descending binary-key order.
    ///
    /// The upper endpoint remains exclusive and the lower endpoint inclusive,
    /// exactly as in [`Self::scan`].
    pub fn rscan<'txn>(
        &'txn mut self,
        table: &Table<'db>,
        lower: &[u8],
        upper: Option<&[u8]>,
    ) -> Result<Scan<'txn, 'db>> {
        Scan::new(self, *table, ScanDirection::Reverse, lower, upper)
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

    /// Upsert `key`, returning `true` when it was absent immediately before
    /// this operation, including after an earlier same-transaction removal.
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

    /// Insert only when absent in the transaction's current view, returning
    /// whether insertion was staged.
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

    /// Remove a key, returning whether a live value existed in the
    /// transaction's current view.
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
    /// Native Mako assigns its logical timestamp after Silo locks the full write
    /// set and before the remaining read-set validation. `hook` runs only after
    /// all validation succeeds, while all write locks remain held and before
    /// any write becomes visible.
    /// It should therefore only bind already-owned storage to an
    /// already-reserved queue slot. It may enter a bounded in-memory critical
    /// section, but must not do I/O, wait for capacity, allocate, or unwind.
    /// Returning `false` definitely aborts the transaction with
    /// [`Error::CommitHookRejected`].
    ///
    /// In builds with panic unwinding, a panic is contained inside the Rust
    /// trampoline and treated exactly like `false`. In a `panic = "abort"`
    /// build (including this workspace's release profile), a panic terminates
    /// the process before it can cross the C boundary, so hooks must not panic.
    /// Read-only and conflicting transactions do not invoke the hook.
    pub fn commit_report_with_hook<F>(self, hook: F) -> CommitReport
    where
        F: FnOnce(MakoTimestamp) -> bool,
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

unsafe extern "C" fn post_validate_trampoline<F>(context: *mut c_void, raw_timestamp: u32) -> i32
where
    F: FnOnce(MakoTimestamp) -> bool,
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
    let Some(timestamp) = MakoTimestamp::new(raw_timestamp) else {
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
    fn mako_timestamp_reserves_zero_as_unassigned() {
        assert_eq!(MakoTimestamp::new(0), None);
        assert_eq!(MakoTimestamp::new(1).map(MakoTimestamp::get), Some(1));
        assert_eq!(
            MakoTimestamp::new(MAX_MAKO_TIMESTAMP).map(MakoTimestamp::get),
            Some(MAX_MAKO_TIMESTAMP)
        );
        assert_eq!(MakoTimestamp::new(MAX_MAKO_TIMESTAMP + 1), None);
        assert_eq!(MakoTimestamp::new(u32::MAX), None);
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
            (sys::MAKO_LOCAL_BUFFER_TOO_SMALL, Err(Error::BufferTooSmall)),
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
            sys::MAKO_LOCAL_TXN_FINISHED,
        ] {
            assert!(operation_is_terminal(code), "status {code}");
        }

        assert!(!operation_is_terminal(sys::MAKO_LOCAL_VALUE_TOO_LARGE));
        assert!(!operation_is_terminal(sys::MAKO_LOCAL_DUPLICATE_WRITE));
        assert!(!operation_is_terminal(sys::MAKO_LOCAL_INVALID_ARGUMENT));
        assert!(!operation_is_terminal(sys::MAKO_LOCAL_BUFFER_TOO_SMALL));
        assert!(operation_is_terminal(-1));
        assert!(operation_is_terminal(i32::MAX));
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
