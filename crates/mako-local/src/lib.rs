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

/// Fixed, thread-affine workers and bounded OCC retry policy.
pub mod worker;

use std::cell::Cell;
use std::ffi::{c_void, CStr};
use std::fmt;
use std::marker::PhantomData;
use std::mem::{ManuallyDrop, MaybeUninit};
use std::num::{NonZeroU32, NonZeroU64};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr::NonNull;
use std::rc::Rc;

use mako_local_sys as sys;

#[cfg(not(test))]
use mako_local_sys as abi;
#[cfg(test)]
mod fake_abi;
#[cfg(test)]
use fake_abi as abi;

// This build-private ABI is deliberately absent from mako-local-sys and the
// stable v0 export manifest. It is a trusted optimization seam for the Rust
// cache wrapper built from the exact same source fingerprint as the C++
// engine. The public methods below continue to enforce Rust ownership and
// thread affinity before reaching these unchecked native entries.
#[cfg(not(test))]
mod fast_abi {
    use std::ffi::c_void;

    use super::sys;

    pub(super) type RecordBindHook = Option<
        unsafe extern "C" fn(
            context: *mut c_void,
            mako_timestamp: u32,
            exact_record_bytes: usize,
            sequence_out: *mut u64,
            record_bytes_out: *mut *mut u8,
            record_capacity_out: *mut usize,
        ) -> i32,
    >;

    extern "C" {
        pub(super) fn mako_rust_fast_txn_begin(
            db: *mut sys::mako_local_db,
            bound_table: *mut sys::mako_local_table,
            out: *mut *mut sys::mako_local_txn,
        ) -> i32;

        pub(super) fn mako_rust_fast_txn_put(
            txn: *mut sys::mako_local_txn,
            key: *const u8,
            key_len: u32,
            value: *const u8,
            value_len: u32,
        ) -> u64;

        pub(super) fn mako_rust_fast_txn_commit_and_destroy(txn: *mut sys::mako_local_txn) -> u64;

        pub(super) fn mako_rust_fast_txn_commit_with_hook_and_destroy(
            txn: *mut sys::mako_local_txn,
            hook: sys::mako_local_post_validate_hook,
            context: *mut c_void,
        ) -> u64;

        pub(super) fn mako_rust_fast_txn_record_preflight(
            txn: *mut sys::mako_local_txn,
            max_record_bytes: usize,
            exact_record_bytes_out: *mut usize,
            op_count_out: *mut u32,
        ) -> i32;

        pub(super) fn mako_rust_fast_txn_commit_record_and_destroy(
            txn: *mut sys::mako_local_txn,
            hook: RecordBindHook,
            context: *mut c_void,
            record_written_out: *mut u8,
        ) -> u64;

        pub(super) fn mako_rust_fast_txn_abort_and_destroy(txn: *mut sys::mako_local_txn) -> u64;
    }
}

#[cfg(test)]
use fake_abi as fast_abi;

#[cfg(all(not(test), have_mako))]
mod identity_abi {
    include!(concat!(env!("OUT_DIR"), "/mako_local_build_identity.rs"));
}

// Preserve the crate's compile-only mode when no CMake tree is available.
// Programs cannot actually call the native API in that mode (the ordinary ABI
// symbols are also absent), but cargo check and documentation remain useful.
#[cfg(all(not(test), not(have_mako)))]
mod identity_abi {
    pub(super) const EXPECTED_ENGINE_ID: &[u8] = b"mako-local/sto-masstrans";
    pub(super) const EXPECTED_BUILD_FINGERPRINT: [u8; 32] = [0; 32];

    pub(super) unsafe fn require_build_anchor() {}
}

#[cfg(test)]
mod identity_abi {
    pub(super) const EXPECTED_ENGINE_ID: &[u8] = b"mako-local/sto-masstrans";
    pub(super) const EXPECTED_BUILD_FINGERPRINT: [u8; 32] = [0x5a; 32];

    pub(super) unsafe fn require_build_anchor() {}
}

/// A failure reported by the native local transaction boundary.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[non_exhaustive]
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
    /// Native state failed or violated an invariant. On commit, visibility may
    /// consequently be uncertain.
    Internal,
    /// Native cleanup could not be proved complete, so this worker is retired.
    WorkerPoisoned,
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
    /// A test-only or optional native capability was not compiled in.
    FeatureUnavailable,
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
    /// The generated Rust status catalog and linked native catalog disagree.
    AbiStatusCatalogMismatch {
        /// Status whose canonical identity or diagnostic did not match.
        status: i32,
    },
    /// The linked implementation is not the expected STO/MassTrans engine family.
    AbiEngineMismatch,
    /// The linked native build returned a fingerprint with the wrong byte length.
    AbiBuildFingerprintSizeMismatch {
        /// Number of SHA-256 bytes required by this crate.
        expected: usize,
        /// Number of bytes reported by the native library.
        found: usize,
    },
    /// Current source/configuration and the linked native archive have different identities.
    AbiBuildFingerprintMismatch,
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
            Self::Internal => write!(f, "the local ABI reported an internal failure"),
            Self::WorkerPoisoned => write!(
                f,
                "native transaction cleanup is uncertain and this worker is quarantined"
            ),
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
            Self::FeatureUnavailable => {
                write!(f, "the requested native feature is unavailable")
            }
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
            Self::AbiStatusCatalogMismatch { status } => write!(
                f,
                "mako-local ABI status catalog mismatch at status {status}"
            ),
            Self::AbiEngineMismatch => write!(
                f,
                "mako-local native engine identity does not match STO/MassTrans"
            ),
            Self::AbiBuildFingerprintSizeMismatch { expected, found } => write!(
                f,
                "mako-local native build fingerprint has {found} bytes; expected {expected}"
            ),
            Self::AbiBuildFingerprintMismatch => write!(
                f,
                "mako-local native archive does not match current source, configuration, and toolchain"
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

/// A test-only synchronous observation point in native local commit.
///
/// This type is public solely for fresh-process crash tests and is available at
/// runtime only when the native library advertises
/// `MAKO_LOCAL_FEATURE_TEST_COMMIT_OBSERVER`.
#[doc(hidden)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum TestCommitPhase {
    /// The complete write set is locked; no Mako timestamp exists yet.
    WritesetLocked = sys::MAKO_LOCAL_TEST_COMMIT_WRITESET_LOCKED,
    /// The checked nonzero Mako timestamp has been assigned.
    MakoTimestampAllocated = sys::MAKO_LOCAL_TEST_COMMIT_MAKO_TIMESTAMP_ALLOCATED,
    /// Every local validation has succeeded, before the preinstall hook.
    LocalValidationComplete = sys::MAKO_LOCAL_TEST_COMMIT_LOCAL_VALIDATION_COMPLETE,
    /// The preinstall hook accepted, before the first write installation.
    PreinstallAccepted = sys::MAKO_LOCAL_TEST_COMMIT_PREINSTALL_ACCEPTED,
    /// The first write was installed and at least one more write remains.
    FirstWriteInstalled = sys::MAKO_LOCAL_TEST_COMMIT_FIRST_WRITE_INSTALLED,
    /// Every local write was installed, before commit cleanup and return.
    AllWritesInstalled = sys::MAKO_LOCAL_TEST_COMMIT_ALL_WRITES_INSTALLED,
}

impl TestCommitPhase {
    const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            sys::MAKO_LOCAL_TEST_COMMIT_WRITESET_LOCKED => Some(Self::WritesetLocked),
            sys::MAKO_LOCAL_TEST_COMMIT_MAKO_TIMESTAMP_ALLOCATED => {
                Some(Self::MakoTimestampAllocated)
            }
            sys::MAKO_LOCAL_TEST_COMMIT_LOCAL_VALIDATION_COMPLETE => {
                Some(Self::LocalValidationComplete)
            }
            sys::MAKO_LOCAL_TEST_COMMIT_PREINSTALL_ACCEPTED => Some(Self::PreinstallAccepted),
            sys::MAKO_LOCAL_TEST_COMMIT_FIRST_WRITE_INSTALLED => Some(Self::FirstWriteInstalled),
            sys::MAKO_LOCAL_TEST_COMMIT_ALL_WRITES_INSTALLED => Some(Self::AllWritesInstalled),
            _ => None,
        }
    }
}

/// Test-only plain function pointer invoked synchronously at native commit
/// seams. Timestamp zero occurs only at [`TestCommitPhase::WritesetLocked`].
#[doc(hidden)]
pub type TestCommitObserver = fn(TestCommitPhase, u32);

thread_local! {
    static TEST_COMMIT_OBSERVER: Cell<Option<TestCommitObserver>> = const { Cell::new(None) };
}

#[cfg(not(test))]
thread_local! {
    // Native attachment is process-lifetime for an OS thread and has no detach
    // operation. Remember successful calls so the safe transaction fast path
    // does not cross the ABI merely to repeat that idempotent check.
    static SAFE_WRAPPER_ATTACHED: Cell<bool> = const { Cell::new(false) };
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

/// Exact native sizing for one canonical cache commit record.
///
/// This build-private type is returned only for a trusted transaction whose
/// native write plan has been sealed. The byte count includes the complete v3
/// header and CRC; `op_count == 0` identifies a logical read-only transaction
/// that must use the ordinary no-record commit terminal.
#[doc(hidden)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CommitRecordPreflight {
    exact_record_bytes: usize,
    op_count: u32,
}

impl CommitRecordPreflight {
    /// Complete encoded record size, including header and CRC.
    pub const fn exact_record_bytes(self) -> usize {
        self.exact_record_bytes
    }

    /// Number of canonical final-effect mutations in the record.
    pub const fn op_count(self) -> u32 {
        self.op_count
    }

    /// Whether native canonicalization found no externally visible mutation.
    pub const fn is_empty(self) -> bool {
        self.op_count == 0
    }
}

/// Preallocated storage that exposes bytes only after native completion.
///
/// Allocation happens in [`Self::try_for`], before native validation and write
/// locking. The private completion state is set only when the consuming native
/// terminal reports that it initialized exactly the preflight extent. Thus
/// safe callers cannot read spare or partially initialized storage.
#[doc(hidden)]
pub struct UninitCommitRecord {
    bytes: Vec<MaybeUninit<u8>>,
    preflight: CommitRecordPreflight,
    written: bool,
}

impl fmt::Debug for UninitCommitRecord {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("UninitCommitRecord")
            .field("preflight", &self.preflight)
            .field("written", &self.written)
            .finish_non_exhaustive()
    }
}

impl UninitCommitRecord {
    /// Allocate the exact writable extent described by `preflight`.
    pub fn try_for(preflight: CommitRecordPreflight) -> Result<Self> {
        let exact = preflight.exact_record_bytes;
        if exact == 0 {
            return Err(Error::Internal);
        }
        let mut bytes = Vec::new();
        bytes
            .try_reserve_exact(exact)
            .map_err(|_| Error::OutOfMemory)?;
        // SAFETY: `MaybeUninit<u8>` may be left uninitialized. Native receives
        // this exact extent only through the synchronous terminal below.
        unsafe { bytes.set_len(exact) };
        Ok(Self {
            bytes,
            preflight,
            written: false,
        })
    }

    /// Writable extent supplied to native serialization.
    pub fn capacity(&self) -> usize {
        self.bytes.len()
    }

    /// Whether native supplied and the wrapper accepted its completion witness.
    pub const fn is_written(&self) -> bool {
        self.written
    }

    /// Borrow the complete initialized record, if native finished writing it.
    pub fn written_bytes(&self) -> Option<&[u8]> {
        if !self.written || self.bytes.len() != self.preflight.exact_record_bytes {
            return None;
        }
        // SAFETY: `written` is set only after the consuming native terminal's
        // completion witness says it initialized this exact allocation.
        Some(unsafe {
            std::slice::from_raw_parts(self.bytes.as_ptr().cast::<u8>(), self.bytes.len())
        })
    }

    /// Consume the wrapper and return the initialized encoded record.
    ///
    /// `None` fails closed when no valid native completion witness exists.
    pub fn into_written(self) -> Option<Vec<u8>> {
        if !self.written || self.bytes.len() != self.preflight.exact_record_bytes {
            return None;
        }
        let mut bytes = ManuallyDrop::new(self.bytes);
        let ptr = bytes.as_mut_ptr().cast::<u8>();
        let len = bytes.len();
        let capacity = bytes.capacity();
        // SAFETY: `MaybeUninit<u8>` has the same allocation layout as `u8`,
        // and the validated witness covers every element in `len`.
        Some(unsafe { Vec::from_raw_parts(ptr, len, capacity) })
    }

    fn mark_written(&mut self, preflight: CommitRecordPreflight) -> bool {
        if self.written
            || self.preflight != preflight
            || self.bytes.len() != preflight.exact_record_bytes
        {
            return false;
        }
        self.written = true;
        true
    }
}

/// Commit visibility together with native record-binding progress.
#[doc(hidden)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[must_use = "a bound record must be published or pinned before acknowledgement"]
pub struct CommitRecordReport {
    /// Native commit visibility and facade cleanup outcomes.
    pub commit: CommitReport,
    /// Whether native's terminal status, bind callback, and 0/1 completion
    /// witness form one internally consistent result.
    ///
    /// A false value is an ABI contract violation, not an ordinary native
    /// error. When no record was bound, a durability adapter has no ordered
    /// slot it can pin and must fail-stop the process.
    pub completion_contract_valid: bool,
    /// Whether the synchronous acquire callback returned a sequence and storage.
    pub record_bound: bool,
    /// Whether native initialized the complete record and issued its witness.
    pub record_written: bool,
}

/// Maximum table-name length accepted by the draft ABI.
pub const MAX_TABLE_NAME_BYTES: usize = sys::MAKO_LOCAL_MAX_TABLE_NAME_BYTES as usize;
/// Maximum key length accepted by the draft ABI.
pub const MAX_KEY_BYTES: usize = sys::MAKO_LOCAL_MAX_KEY_BYTES as usize;
/// Maximum value length accepted by the draft ABI.
pub const MAX_VALUE_BYTES: usize = sys::MAKO_LOCAL_MAX_VALUE_BYTES as usize;
/// Weighted native item budget for one draft transaction.
pub const TRANSACTION_ITEM_BUDGET: usize = sys::MAKO_LOCAL_TXN_ITEM_BUDGET as usize;
/// Maximum number of OS workers that may attach to STO in one process.
///
/// Worker identifiers are process-lifetime resources and are not recycled.
pub const MAX_WORKERS: usize = sys::MAKO_LOCAL_MAX_WORKERS as usize;

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

    /// Test-only synchronous local commit crash-seam observation is compiled in.
    pub const fn test_commit_observer(self) -> bool {
        self.0 & sys::MAKO_LOCAL_FEATURE_TEST_COMMIT_OBSERVER != 0
    }

    /// Deterministic native cleanup-failure injection is compiled in for tests.
    #[doc(hidden)]
    pub const fn test_cleanup_failures(self) -> bool {
        self.0 & sys::MAKO_LOCAL_FEATURE_TEST_CLEANUP_FAILURES != 0
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
    Ok(Features(unsafe { abi::mako_local_feature_bits() }))
}

/// Install a test-only synchronous commit observer on the current attached
/// worker thread.
///
/// The callback must not allocate, unwind, or re-enter mako-local. It may park
/// the thread only for a fresh-process crash test whose controller will issue
/// `SIGKILL`. A second install before [`clear_test_commit_observer`] returns
/// [`Error::Busy`]. Production-default native builds return
/// [`Error::FeatureUnavailable`]. In a hook-enabled test build, registration
/// also makes an ordinary write commit allocate a Mako timestamp; timestamp
/// exhaustion can therefore make that otherwise non-durable commit fail.
#[doc(hidden)]
pub fn install_test_commit_observer(observer: TestCommitObserver) -> Result<()> {
    verify_abi()?;
    TEST_COMMIT_OBSERVER.with(|slot| {
        if slot.get().is_some() {
            return Err(Error::Busy);
        }
        // SAFETY: the trampoline and its null context are static. Native stores
        // them only in this thread's TLS until the paired clear call.
        status(unsafe {
            abi::mako_local_test_set_commit_observer(
                Some(test_commit_observer_trampoline),
                std::ptr::null_mut(),
            )
        })?;
        slot.set(Some(observer));
        Ok(())
    })
}

/// Clear the test-only commit observer on the current attached worker thread.
///
/// Clearing an already-clear observer is harmless. The callback is removed
/// from native TLS before its Rust function pointer is forgotten.
#[doc(hidden)]
pub fn clear_test_commit_observer() -> Result<()> {
    verify_abi()?;
    // SAFETY: registration is thread-local and native clear is idempotent.
    status(unsafe { abi::mako_local_test_clear_commit_observer() })?;
    TEST_COMMIT_OBSERVER.with(|slot| slot.set(None));
    Ok(())
}

/// Native cleanup boundary targeted by the deterministic test failpoint.
#[doc(hidden)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum TestCleanupBoundary {
    /// Cleanup after transaction startup fails.
    Begin = sys::MAKO_LOCAL_CLEANUP_BOUNDARY_BEGIN,
    /// Cleanup after a point or scan operation fails.
    Operation = sys::MAKO_LOCAL_CLEANUP_BOUNDARY_OPERATION,
    /// Cleanup after commit fails.
    Commit = sys::MAKO_LOCAL_CLEANUP_BOUNDARY_COMMIT,
    /// Explicit or drop-driven abort cleanup fails.
    Abort = sys::MAKO_LOCAL_CLEANUP_BOUNDARY_ABORT,
    /// Final facade destruction fails.
    Destroy = sys::MAKO_LOCAL_CLEANUP_BOUNDARY_DESTROY,
}

/// Arm a one-shot native cleanup failure on the current worker.
#[doc(hidden)]
pub fn arm_test_cleanup_failure(boundary: TestCleanupBoundary) -> Result<()> {
    verify_abi()?;
    // SAFETY: scalar-only test control; native state is thread-local.
    status(unsafe { abi::mako_local_test_arm_cleanup_failure(boundary as u32) })
}

/// Clear any unconsumed cleanup failure on the current worker.
#[doc(hidden)]
pub fn clear_test_cleanup_failure() -> Result<()> {
    verify_abi()?;
    // SAFETY: scalar-only idempotent test control.
    status(unsafe { abi::mako_local_test_clear_cleanup_failure() })
}

/// Health of the current OS worker's native transaction runtime.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[must_use = "poisoned workers must be retired"]
pub enum WorkerHealth {
    /// This worker has not been attached to the local STO runtime.
    NotAttached,
    /// The worker is attached and remains safe for transactions.
    Healthy,
    /// Cleanup is uncertain and the worker must be retired permanently.
    Poisoned,
}

/// Query the current OS worker's native transaction health.
///
/// Unlike ordinary operations, the two expected negative states are returned
/// as values. `Err` is reserved for ABI drift or a status outside the health
/// query's contract.
pub fn worker_health() -> Result<WorkerHealth> {
    verify_abi()?;
    // SAFETY: scalar-only thread-local health query.
    let code = unsafe { abi::mako_local_worker_health() };
    match sys::KnownStatus::from_code(code) {
        Some(sys::KnownStatus::Ok) => Ok(WorkerHealth::Healthy),
        Some(sys::KnownStatus::NotAttached) => Ok(WorkerHealth::NotAttached),
        Some(sys::KnownStatus::WorkerPoisoned) => Ok(WorkerHealth::Poisoned),
        Some(status) => Err(known_status(status)
            .expect_err("only native success maps to Ok, and it was handled above")),
        None => Err(Error::UnknownStatus(code)),
    }
}

/// Return the process-wide number of workers quarantined by uncertain cleanup.
pub fn quarantined_worker_count() -> Result<u64> {
    verify_abi()?;
    // SAFETY: pure atomic diagnostic accessor.
    Ok(unsafe { abi::mako_local_quarantined_worker_count() })
}

/// Verifies that the generated declarations, linked native archive, engine
/// family, build fingerprint, layouts, feature catalog, and status catalog all
/// agree before native state is opened.
///
/// Normal database construction performs this handshake automatically. This
/// entry point is public so deployment probes and ABI link tests can fail fast
/// without allocating a database facade.
pub fn verify_abi() -> Result<()> {
    // SAFETY: the digest-named function is a no-op identity anchor. Calling it
    // makes the linker require the archive whose fingerprint Cargo verified.
    unsafe { identity_abi::require_build_anchor() };

    // SAFETY: a conforming native identity accessor returns a process-lifetime
    // NUL-terminated string. Null and mismatched values are rejected below.
    let engine_id = unsafe { abi::mako_local_engine_id() };
    if engine_id.is_null() {
        return Err(Error::AbiEngineMismatch);
    }
    // SAFETY: guarded non-null and specified to have process lifetime.
    let engine_id = unsafe { CStr::from_ptr(engine_id) }.to_bytes();

    // SAFETY: scalar-only native build-identity accessor.
    let fingerprint_size = unsafe { abi::mako_local_build_fingerprint_size() };
    if fingerprint_size != identity_abi::EXPECTED_BUILD_FINGERPRINT.len() {
        return validate_build_identity(engine_id, fingerprint_size, None);
    }
    // SAFETY: a conforming accessor returns fingerprint_size static bytes. A
    // null result is rejected before constructing the slice.
    let fingerprint = unsafe { abi::mako_local_build_fingerprint() };
    let fingerprint = if fingerprint.is_null() {
        None
    } else {
        // SAFETY: non-null and native promises that its reported static byte
        // extent is readable. validate_build_identity checks that extent.
        Some(unsafe { std::slice::from_raw_parts(fingerprint, fingerprint_size) })
    };
    validate_build_identity(engine_id, fingerprint_size, fingerprint)?;

    // SAFETY: pure ABI identity accessor.
    let found = unsafe { abi::mako_local_abi_version() };
    if found != sys::MAKO_LOCAL_ABI_VERSION {
        return Err(Error::AbiMismatch {
            expected: sys::MAKO_LOCAL_ABI_VERSION,
            found,
        });
    }

    for (structure, expected, found) in [
        (
            "mako_local_db_options",
            sys::MAKO_LOCAL_DB_OPTIONS_V0_SIZE as usize,
            // SAFETY: pure ABI identity accessor.
            unsafe { abi::mako_local_db_options_size() },
        ),
        (
            "mako_local_scan_options",
            sys::MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE as usize,
            // SAFETY: pure ABI identity accessor.
            unsafe { abi::mako_local_scan_options_size() },
        ),
        (
            "mako_local_scan_entry",
            std::mem::size_of::<sys::mako_local_scan_entry>(),
            // SAFETY: pure ABI identity accessor.
            unsafe { abi::mako_local_scan_entry_size() },
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

    for native_status in sys::ALL_KNOWN_STATUSES {
        let code = native_status.code();
        // SAFETY: the native function returns a static NUL-terminated string
        // for every integer. A null pointer is still treated as ABI drift.
        let message = unsafe { abi::mako_local_status_string(code) };
        if message.is_null()
            // SAFETY: non-null native status strings have static lifetime.
            || unsafe { CStr::from_ptr(message) }.to_bytes() != native_status.message().as_bytes()
        {
            return Err(Error::AbiStatusCatalogMismatch { status: code });
        }
    }
    Ok(())
}

fn validate_build_identity(
    engine_id: &[u8],
    fingerprint_size: usize,
    fingerprint: Option<&[u8]>,
) -> Result<()> {
    if engine_id != identity_abi::EXPECTED_ENGINE_ID {
        return Err(Error::AbiEngineMismatch);
    }
    if fingerprint_size != identity_abi::EXPECTED_BUILD_FINGERPRINT.len() {
        return Err(Error::AbiBuildFingerprintSizeMismatch {
            expected: identity_abi::EXPECTED_BUILD_FINGERPRINT.len(),
            found: fingerprint_size,
        });
    }
    if fingerprint != Some(identity_abi::EXPECTED_BUILD_FINGERPRINT.as_slice()) {
        return Err(Error::AbiBuildFingerprintMismatch);
    }
    Ok(())
}

#[inline]
fn status(code: i32) -> Result<()> {
    if code == sys::MAKO_LOCAL_OK {
        return Ok(());
    }
    status_error(code)
}

#[cold]
#[inline(never)]
fn status_error(code: i32) -> Result<()> {
    match sys::KnownStatus::from_code(code) {
        Some(known) => known_status(known),
        None => Err(Error::UnknownStatus(code)),
    }
}

#[inline]
fn known_status(status: sys::KnownStatus) -> Result<()> {
    match status {
        sys::KnownStatus::Ok => Ok(()),
        sys::KnownStatus::Conflict => Err(Error::Conflict),
        sys::KnownStatus::NotAttached => Err(Error::NotAttached),
        sys::KnownStatus::WrongThread => Err(Error::WrongThread),
        sys::KnownStatus::TxnAlreadyActive => Err(Error::TransactionAlreadyActive),
        sys::KnownStatus::TxnFinished => Err(Error::TransactionFinished),
        sys::KnownStatus::WrongDbOrTable => Err(Error::WrongDatabaseOrTable),
        sys::KnownStatus::InvalidArgument => Err(Error::InvalidArgument),
        sys::KnownStatus::ThreadLimit => Err(Error::ThreadLimit),
        sys::KnownStatus::Busy => Err(Error::Busy),
        sys::KnownStatus::OutOfMemory => Err(Error::OutOfMemory),
        sys::KnownStatus::Internal => Err(Error::Internal),
        sys::KnownStatus::WorkerPoisoned => Err(Error::WorkerPoisoned),
        sys::KnownStatus::DuplicateWrite => Err(Error::DuplicateWrite),
        sys::KnownStatus::TxnTooLarge => Err(Error::TransactionTooLarge),
        sys::KnownStatus::ValueTooLarge => Err(Error::ValueTooLarge),
        sys::KnownStatus::CommitHookRejected => Err(Error::CommitHookRejected),
        sys::KnownStatus::TimestampExhausted => Err(Error::TimestampExhausted),
        sys::KnownStatus::BufferTooSmall => Err(Error::BufferTooSmall),
        sys::KnownStatus::FeatureUnavailable => Err(Error::FeatureUnavailable),
    }
}

#[inline]
fn decode_fast_put(packed: u64) -> Option<(i32, bool)> {
    let status = packed as u32 as i32;
    let created = packed & (1_u64 << 32) != 0;
    // Bits above the documented created flag are reserved. Native must not
    // claim creation for an operation it also reports as failed.
    if packed >> 33 != 0 || (status != sys::MAKO_LOCAL_OK && created) {
        return None;
    }
    Some((status, created))
}

#[inline]
fn decode_fast_commit(packed: u64) -> Option<(i32, i32)> {
    let commit = packed as u32 as i32;
    let cleanup = (packed >> 32) as u32 as i32;
    let definite_terminal = matches!(
        commit,
        sys::MAKO_LOCAL_OK
            | sys::MAKO_LOCAL_CONFLICT
            | sys::MAKO_LOCAL_COMMIT_HOOK_REJECTED
            | sys::MAKO_LOCAL_TIMESTAMP_EXHAUSTED
    ) && cleanup == sys::MAKO_LOCAL_OK;
    let quarantined =
        commit == sys::MAKO_LOCAL_WORKER_POISONED && cleanup == sys::MAKO_LOCAL_WORKER_POISONED;
    (definite_terminal || quarantined).then_some((commit, cleanup))
}

#[inline]
fn decode_fast_abort(packed: u64) -> Option<(i32, i32)> {
    let abort = packed as u32 as i32;
    let cleanup = (packed >> 32) as u32 as i32;
    let definite_terminal = abort == sys::MAKO_LOCAL_OK && cleanup == sys::MAKO_LOCAL_OK;
    let quarantined =
        abort == sys::MAKO_LOCAL_WORKER_POISONED && cleanup == sys::MAKO_LOCAL_WORKER_POISONED;
    (definite_terminal || quarantined).then_some((abort, cleanup))
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum OperationEffect {
    Active,
    Finished,
    Quarantined,
    Uncertain,
}

#[inline]
fn operation_effect(code: i32) -> OperationEffect {
    let Some(status) = sys::KnownStatus::from_code(code) else {
        return OperationEffect::Uncertain;
    };
    match status {
        sys::KnownStatus::Ok
        | sys::KnownStatus::WrongThread
        | sys::KnownStatus::WrongDbOrTable
        | sys::KnownStatus::InvalidArgument
        | sys::KnownStatus::DuplicateWrite
        | sys::KnownStatus::ValueTooLarge
        | sys::KnownStatus::BufferTooSmall => OperationEffect::Active,
        sys::KnownStatus::Conflict
        | sys::KnownStatus::OutOfMemory
        | sys::KnownStatus::TxnTooLarge
        | sys::KnownStatus::Internal => OperationEffect::Finished,
        // These statuses are either cleanup-uncertain or outside the point and
        // scan operation contract. Fail closed if a linked implementation
        // returns one through that surface.
        sys::KnownStatus::NotAttached
        | sys::KnownStatus::TxnAlreadyActive
        | sys::KnownStatus::TxnFinished
        | sys::KnownStatus::ThreadLimit
        | sys::KnownStatus::Busy
        | sys::KnownStatus::CommitHookRejected
        | sys::KnownStatus::TimestampExhausted
        | sys::KnownStatus::FeatureUnavailable => OperationEffect::Uncertain,
        sys::KnownStatus::WorkerPoisoned => OperationEffect::Quarantined,
    }
}

#[inline]
fn commit_disposition(code: i32) -> CommitDisposition {
    if code == sys::MAKO_LOCAL_OK {
        return CommitDisposition::Committed;
    }
    commit_disposition_error(code)
}

#[cold]
#[inline(never)]
fn commit_disposition_error(code: i32) -> CommitDisposition {
    let Some(status) = sys::KnownStatus::from_code(code) else {
        return CommitDisposition::Unknown(Error::UnknownStatus(code));
    };
    match status {
        sys::KnownStatus::Ok => CommitDisposition::Committed,
        sys::KnownStatus::Conflict => CommitDisposition::Aborted(Error::Conflict),
        sys::KnownStatus::CommitHookRejected => {
            CommitDisposition::Aborted(Error::CommitHookRejected)
        }
        sys::KnownStatus::TimestampExhausted => {
            CommitDisposition::Aborted(Error::TimestampExhausted)
        }
        sys::KnownStatus::NotAttached
        | sys::KnownStatus::WrongThread
        | sys::KnownStatus::TxnAlreadyActive
        | sys::KnownStatus::TxnFinished
        | sys::KnownStatus::WrongDbOrTable
        | sys::KnownStatus::InvalidArgument
        | sys::KnownStatus::ThreadLimit
        | sys::KnownStatus::Busy
        | sys::KnownStatus::OutOfMemory
        | sys::KnownStatus::Internal
        | sys::KnownStatus::DuplicateWrite
        | sys::KnownStatus::TxnTooLarge
        | sys::KnownStatus::ValueTooLarge
        | sys::KnownStatus::BufferTooSmall
        | sys::KnownStatus::FeatureUnavailable
        | sys::KnownStatus::WorkerPoisoned => CommitDisposition::Unknown(
            known_status(status).expect_err("a non-success native status has a typed error"),
        ),
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
    status(unsafe { abi::mako_local_advance_mako_timestamp_past(observed.get()) })
}

fn attach_current_thread() -> Result<()> {
    // SAFETY: no pointer arguments; native side is idempotent per OS thread.
    let result = status(unsafe { abi::mako_local_thread_attach() });
    #[cfg(not(test))]
    if result.is_ok() {
        SAFE_WRAPPER_ATTACHED.with(|attached| attached.set(true));
    }
    result
}

#[inline]
fn ensure_current_thread_attached() -> Result<()> {
    #[cfg(not(test))]
    if SAFE_WRAPPER_ATTACHED.with(Cell::get) {
        return Ok(());
    }
    // Unit tests deliberately reset their fake native TLS between cases, so
    // cfg(test) continues to exercise the real attach call every time.
    attach_current_thread()
}

/// A local in-memory Mako database using the C++ STO/MassTrans engine.
///
/// The facade can be shared between fixed, long-lived workers. Its underlying
/// MassTrans tables remain process-lifetime in this draft; dropping this value
/// releases the facade handles after all safe Rust borrows have ended.
pub struct LocalDb {
    raw: NonNull<sys::mako_local_db>,
}

/// Options for opening a local database facade.
///
/// Revision 0 has no behavioral fields yet. The public type and the sized C
/// representation reserve an append-only negotiation seam before ABI v1.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
#[non_exhaustive]
pub struct DbOptions {}

// SAFETY: table-map mutation is protected by the native database mutex, and
// MassTrans is designed for concurrent access. Transactions themselves carry
// the thread-affinity restriction and are not Send/Sync.
unsafe impl Send for LocalDb {}
// SAFETY: as above.
unsafe impl Sync for LocalDb {}

impl LocalDb {
    /// Open a new local database facade and attach the calling worker.
    pub fn open() -> Result<Self> {
        Self::open_with_options(DbOptions::default())
    }

    /// Open with the revision-0 sized options contract.
    pub fn open_with_options(_options: DbOptions) -> Result<Self> {
        verify_abi()?;
        attach_current_thread()?;
        let mut raw = std::ptr::null_mut();
        let raw_options = sys::mako_local_db_options {
            struct_size: sys::MAKO_LOCAL_DB_OPTIONS_V0_SIZE,
            flags: 0,
        };
        // SAFETY: both options and output remain live for this synchronous
        // call, and the returned handle is checked before use.
        status(unsafe { abi::mako_local_db_open_with_options(&raw_options, &mut raw) })?;
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
            abi::mako_local_table_open(
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
    #[inline]
    pub fn transaction(&self) -> Result<Transaction<'_>> {
        ensure_current_thread_attached()?;
        let mut raw = std::ptr::null_mut();
        // SAFETY: database is live and raw is a valid out-pointer.
        status(unsafe { abi::mako_local_txn_begin(self.raw.as_ptr(), &mut raw) })?;
        let raw = NonNull::new(raw).ok_or(Error::Internal)?;
        Ok(Transaction {
            raw: Some(raw),
            active: true,
            fast_bound_table: None,
            record_preflight: None,
            _db: PhantomData,
            _thread_affine: PhantomData,
        })
    }

    /// Begin a transaction using the build-private Rust/C++ fast path.
    ///
    /// This entry point exists for `mako-cache`, which has already checked the
    /// exact native build fingerprint and whose safe wrapper supplies the
    /// pointer, length, database, table, and thread-affinity invariants omitted
    /// by the hot native Put and consuming terminal entries. Other operations
    /// retain the checked v0 ABI. The transaction has exactly the same public
    /// ownership and error semantics as one returned by [`Self::transaction`].
    ///
    /// This is intentionally not the long-term compiler optimization story.
    /// ABI-only synthetic PGO came within about one percent of an unprofiled
    /// native control, but that was only an optimization ceiling: profiling
    /// both paths equally retained about a ten-percent dynamic-instruction gap.
    /// Reconsider PGO once a production workload and pinned toolchain can
    /// support a reviewed, reproducible training profile.
    #[doc(hidden)]
    pub fn trusted_transaction<'db>(
        &'db self,
        bound_table: &Table<'db>,
    ) -> Result<Transaction<'db>> {
        ensure_current_thread_attached()?;
        let mut raw = std::ptr::null_mut();
        // SAFETY: `self` and `bound_table` remain borrowed for the returned
        // transaction's complete lifetime. Native begin validates their owner
        // relationship once and binds the table into the pooled facade.
        status(unsafe {
            fast_abi::mako_rust_fast_txn_begin(
                self.raw.as_ptr(),
                bound_table.raw.as_ptr(),
                &mut raw,
            )
        })?;
        let raw = NonNull::new(raw).ok_or(Error::Internal)?;
        Ok(Transaction {
            raw: Some(raw),
            active: true,
            fast_bound_table: Some(bound_table.raw),
            record_preflight: None,
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
        status(unsafe { abi::mako_local_db_close(this.raw.as_ptr()) })
    }
}

impl Drop for LocalDb {
    fn drop(&mut self) {
        // SAFETY: this is the unique facade handle returned by open. If a
        // transaction was deliberately forgotten, native close returns BUSY
        // without freeing anything; leaking is required for memory safety.
        let _ = unsafe { abi::mako_local_db_close(self.raw.as_ptr()) };
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
        unsafe { abi::mako_local_table_id(self.raw.as_ptr()) }
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
/// A transaction cannot outlive the database whose native marker and tables
/// it borrows:
///
/// ```compile_fail
/// let tx = {
///     let db = mako_local::LocalDb::open().unwrap();
///     db.transaction().unwrap()
/// };
/// drop(tx);
/// ```
/// Nor can an executor accept it as a `Send` future held across suspension:
///
/// ```compile_fail
/// # fn require_send<T: Send>(_value: T) {}
/// # let db = mako_local::LocalDb::open().unwrap();
/// let tx = db.transaction().unwrap();
/// require_send(async move {
///     let tx = tx;
///     std::future::pending::<()>().await;
///     drop(tx);
/// });
/// ```
pub struct Transaction<'db> {
    raw: Option<NonNull<sys::mako_local_txn>>,
    active: bool,
    // Present only for the build-private cache path. Exact pointer equality is
    // required before the unchecked Put can omit table ownership validation.
    fast_bound_table: Option<NonNull<sys::mako_local_table>>,
    // A successful private preflight seals native's canonical write plan. It
    // is retained so storage sized for another transaction cannot be supplied
    // to the consuming serialization terminal.
    record_preflight: Option<CommitRecordPreflight>,
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

    fn fail<T>(&mut self, error: Error) -> Result<T> {
        let result = self.transaction.fail_closed(error);
        self.done = true;
        result
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
                    ScanDirection::Forward => abi::mako_local_txn_scan_chunk(
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
                    ScanDirection::Reverse => abi::mako_local_txn_rscan_chunk(
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
                || arena_required != 0
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
    #[inline]
    fn active_raw(&self) -> Result<*mut sys::mako_local_txn> {
        if !self.active {
            return Err(Error::TransactionFinished);
        }
        // Native preflight seals the canonical write plan. In particular, the
        // trusted fast put entry intentionally omits most public-ABI checks, so
        // safe Rust must never let a later operation reach it after sealing.
        if self.record_preflight.is_some() {
            return Err(Error::Busy);
        }
        Ok(self
            .raw
            .expect("transaction handle already consumed")
            .as_ptr())
    }

    #[inline]
    fn operation_status(&mut self, code: i32) -> Result<()> {
        if code == sys::MAKO_LOCAL_OK {
            return Ok(());
        }
        self.operation_status_error(code)
    }

    #[cold]
    #[inline(never)]
    fn operation_status_error(&mut self, code: i32) -> Result<()> {
        // A finished or terminal-uncertain native transaction must never be
        // used for another operation or commit. Drop still calls destroy once,
        // which either consumes a clean terminal facade or observes
        // quarantine. Native storage may be recycled after consumption.
        if operation_effect(code) != OperationEffect::Active {
            self.active = false;
        }
        status(code)
    }

    fn abort_after_wrapper_failure(&mut self) -> Result<()> {
        if !self.active {
            return Ok(());
        }
        let cleanup = if let Some(raw) = self.raw {
            // SAFETY: the wrapper holds the unique, live, thread-affine
            // transaction facade. Malformed native output must never be
            // allowed to reach commit; Drop performs the one later destroy.
            status(unsafe { abi::mako_local_txn_abort(raw.as_ptr()) })
        } else {
            Ok(())
        };
        // The abort call is terminal whether cleanup completed or quarantined
        // the worker. Drop must only perform the one-shot destroy probe.
        self.active = false;
        cleanup
    }

    fn fail_closed<T>(&mut self, error: Error) -> Result<T> {
        match self.abort_after_wrapper_failure() {
            Ok(()) => Err(error),
            Err(cleanup) => Err(cleanup),
        }
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
            abi::mako_local_txn_get(
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
        if found > 1 || len > MAX_VALUE_BYTES {
            return self.fail_closed(Error::Internal);
        }
        if found == 0 {
            return if owned.0.is_null() && len == 0 {
                Ok(None)
            } else {
                self.fail_closed(Error::Internal)
            };
        }
        let Some(bytes) = NonNull::new(owned.0) else {
            return self.fail_closed(Error::Internal);
        };
        // SAFETY: native get allocated at least one byte and reports the
        // initialized payload length; ForeignBytes keeps it live through copy.
        let borrowed = unsafe { std::slice::from_raw_parts(bytes.as_ptr(), len) };
        let result = match copy_scan_bytes(borrowed) {
            Ok(result) => result,
            Err(error) => return self.fail_closed(error),
        };
        Ok(Some(result))
    }

    /// Upsert `key`, returning `true` when it was absent immediately before
    /// this operation, including after an earlier same-transaction removal.
    #[inline]
    pub fn put(&mut self, table: &Table<'db>, key: &[u8], value: &[u8]) -> Result<bool> {
        let raw = self.active_raw()?;
        if self.fast_bound_table == Some(table.raw) {
            if key.len() > MAX_KEY_BYTES || value.len() > MAX_VALUE_BYTES {
                return Err(Error::ValueTooLarge);
            }
            // SAFETY: the safe Rust transaction/table borrows prove the
            // thread, database, table, and slice-lifetime invariants that this
            // build-private entry intentionally omits. The representation
            // limits above make both narrowing conversions exact.
            let packed = unsafe {
                fast_abi::mako_rust_fast_txn_put(
                    raw,
                    key.as_ptr(),
                    key.len() as u32,
                    value.as_ptr(),
                    value.len() as u32,
                )
            };
            let Some((code, created)) = decode_fast_put(packed) else {
                return self.fail_closed(Error::Internal);
            };
            self.operation_status(code)?;
            return Ok(created);
        }

        let mut created = 0u8;
        // SAFETY: input slices live through the call. C++ copies/encodes the
        // value into transaction-owned stable storage before returning.
        let code = unsafe {
            abi::mako_local_txn_put(
                raw,
                table.raw.as_ptr(),
                key.as_ptr(),
                key.len(),
                value.as_ptr(),
                value.len(),
                &mut created,
            )
        };
        self.operation_status(code)?;
        if created > 1 {
            return self.fail_closed(Error::Internal);
        }
        Ok(created != 0)
    }

    /// Insert only when absent in the transaction's current view, returning
    /// whether insertion was staged.
    pub fn insert(&mut self, table: &Table<'db>, key: &[u8], value: &[u8]) -> Result<bool> {
        let mut inserted = 0u8;
        // SAFETY: same ownership contract as put.
        let code = unsafe {
            abi::mako_local_txn_insert(
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
        if inserted > 1 {
            return self.fail_closed(Error::Internal);
        }
        Ok(inserted != 0)
    }

    /// Remove a key, returning whether a live value existed in the
    /// transaction's current view.
    pub fn remove(&mut self, table: &Table<'db>, key: &[u8]) -> Result<bool> {
        let mut existed = 0u8;
        // SAFETY: key slice lives through the call and existed is writable.
        let code = unsafe {
            abi::mako_local_txn_remove(
                self.active_raw()?,
                table.raw.as_ptr(),
                key.as_ptr(),
                key.len(),
                &mut existed,
            )
        };
        self.operation_status(code)?;
        if existed > 1 {
            return self.fail_closed(Error::Internal);
        }
        Ok(existed != 0)
    }

    /// Seal and size native's canonical cache commit record.
    ///
    /// This private cache integration is available only on a transaction from
    /// [`LocalDb::trusted_transaction`]. It performs no Rust allocation and
    /// runs before commit validation or write locking. A successful call seals
    /// the transaction's native write plan; call it exactly once, after the
    /// final transaction operation. `max_record_bytes` bounds nonempty plans
    /// and guards the eventual allocation request. A read-only or net-empty
    /// plan succeeds without allocation even when its diagnostic 30-byte
    /// framing size exceeds that cap.
    #[doc(hidden)]
    pub fn commit_record_preflight(
        &mut self,
        max_record_bytes: usize,
    ) -> Result<CommitRecordPreflight> {
        let raw = self.active_raw()?;
        if self.fast_bound_table.is_none() {
            return Err(Error::FeatureUnavailable);
        }
        if self.record_preflight.is_some() {
            return Err(Error::Busy);
        }

        let mut exact_record_bytes = 0usize;
        let mut op_count = 0u32;
        // SAFETY: this is a live trusted transaction. Both initialized output
        // scalars remain writable for the synchronous private ABI call.
        let code = unsafe {
            fast_abi::mako_rust_fast_txn_record_preflight(
                raw,
                max_record_bytes,
                &mut exact_record_bytes,
                &mut op_count,
            )
        };
        if let Err(error) = status(code) {
            // Native seals the plan even when the caller's byte cap rejects
            // it. Abort now so a caller cannot accidentally fall back to an
            // ordinary unlogged commit after observing the preflight error.
            return self.fail_closed(error);
        }
        if exact_record_bytes == 0
            || (op_count != 0 && exact_record_bytes > max_record_bytes)
            || op_count as usize > TRANSACTION_ITEM_BUDGET
        {
            return self.fail_closed(Error::Internal);
        }
        let preflight = CommitRecordPreflight {
            exact_record_bytes,
            op_count,
        };
        self.record_preflight = Some(preflight);
        Ok(preflight)
    }

    /// Validate and atomically install the transaction's local writes.
    ///
    /// Consumes the handle. [`Error::Conflict`] is a normal OCC outcome.
    /// A non-conflict error can theoretically be handle cleanup failing after
    /// a successful install; durability integrations must use
    /// [`Self::commit_report`] so they do not lose that distinction.
    #[inline]
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
    #[inline]
    pub fn commit_report(self) -> CommitReport {
        self.finish_commit(None, std::ptr::null_mut())
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
        self.finish_commit(
            Some(post_validate_trampoline::<F>),
            std::ptr::from_mut(&mut state).cast::<c_void>(),
        )
    }

    /// Commit while native serializes its canonical record into owned storage.
    ///
    /// The transaction must have a successful nonempty
    /// [`Self::commit_record_preflight`], and `record` must have been allocated
    /// from those exact bounds. For this record-only terminal, native orders
    /// Mako timestamp assignment, final validation, and `acquire` with a
    /// per-database gate. Thus successful callbacks can bind the next dense
    /// serialization-safe slot while validation losers consume no slot. Native
    /// retires that short turn before walking/copying the canonical record, but
    /// retains every write lock and installs nothing until serialization ends.
    /// `acquire` runs synchronously before installation and must return its
    /// nonzero sequence without allocating, performing I/O, waiting for
    /// capacity, re-entering mako-local, or unwinding. Returning `None`
    /// definitely rejects installation.
    ///
    /// A callback panic is contained and treated as rejection in unwind builds.
    /// `record` exposes bytes only when native confirms complete initialization;
    /// callers must publish a written committed record before acknowledging it,
    /// and pin every non-committed outcome for which `record_bound` is true.
    #[doc(hidden)]
    pub fn commit_report_with_record<F>(
        mut self,
        record: &mut UninitCommitRecord,
        acquire: F,
    ) -> CommitRecordReport
    where
        F: FnOnce(MakoTimestamp, CommitRecordPreflight) -> Option<NonZeroU64>,
    {
        let Some(preflight) = self.record_preflight else {
            return self.reject_record_commit(Error::InvalidArgument);
        };
        if self.fast_bound_table.is_none()
            || preflight.is_empty()
            || record.preflight != preflight
            || record.written
            || record.bytes.len() != preflight.exact_record_bytes
        {
            return self.reject_record_commit(Error::InvalidArgument);
        }

        let raw = self
            .raw
            .take()
            .expect("transaction handle already consumed");
        if !self.active {
            // Reuse the established terminal-handle cleanup path. Its inactive
            // branch never invokes commit.
            self.raw = Some(raw);
            let commit = self.finish_commit(None, std::ptr::null_mut());
            return CommitRecordReport {
                commit,
                completion_contract_valid: true,
                record_bound: false,
                record_written: false,
            };
        }
        self.active = false;

        let mut state = RecordBindHook {
            hook: Some(acquire),
            record: NonNull::from(&mut *record),
            preflight,
            bound: false,
        };
        let mut record_written = 0u8;
        // SAFETY: this active fast-bound handle is consumed by the private
        // terminal on every outcome. The callback state and record allocation
        // remain live and immovable until this synchronous call returns.
        let packed = unsafe {
            fast_abi::mako_rust_fast_txn_commit_record_and_destroy(
                raw.as_ptr(),
                Some(record_bind_trampoline::<F>),
                std::ptr::from_mut(&mut state).cast::<c_void>(),
                &mut record_written,
            )
        };

        let record_bound = state.bound;
        let witness_claimed = record_written == 1;
        // A bound-but-unwritten result is the legitimate fail-stop shape when
        // native assigned the dense sequence and then rejected post-gate
        // serialization before install. Written without bound is impossible;
        // a successful terminal still separately requires a written witness.
        let mut contract_valid = record_written <= 1 && (!witness_claimed || record_bound);
        if contract_valid && witness_claimed && !record.mark_written(preflight) {
            contract_valid = false;
        }

        let decoded = decode_fast_commit(packed);
        if decoded.is_none() {
            contract_valid = false;
        }
        if let Some((commit, _)) = decoded {
            // A reported successful installation without a bound and written
            // record would have escaped durability coverage.
            if commit == sys::MAKO_LOCAL_OK && !witness_claimed {
                contract_valid = false;
            }
            // Once binding succeeded, a zero witness has only two native
            // shapes: serializer rejection is the definite pre-install
            // COMMIT_HOOK_REJECTED result, while an exception poisons the
            // worker and leaves visibility unknown. A conflict, timestamp
            // exhaustion, or argument failure after binding contradicts the
            // validation-gate protocol and must never be exposed as retryable.
            if record_bound
                && !witness_claimed
                && commit != sys::MAKO_LOCAL_COMMIT_HOOK_REJECTED
                && commit != sys::MAKO_LOCAL_WORKER_POISONED
            {
                contract_valid = false;
            }
        }
        let commit = if contract_valid {
            decoded.map_or(
                CommitReport {
                    disposition: CommitDisposition::Unknown(Error::Internal),
                    cleanup: Err(Error::Internal),
                },
                |(commit, cleanup)| CommitReport {
                    disposition: commit_disposition(commit),
                    cleanup: status(cleanup),
                },
            )
        } else {
            // The native handle was consumed, so malformed completion cannot
            // be retried. Never expose bytes without the exact 0/1 witness.
            CommitReport {
                disposition: CommitDisposition::Unknown(Error::Internal),
                cleanup: Err(Error::Internal),
            }
        };
        CommitRecordReport {
            commit,
            completion_contract_valid: contract_valid,
            record_bound,
            record_written: contract_valid && witness_claimed,
        }
    }

    fn reject_record_commit(self, error: Error) -> CommitRecordReport {
        let cleanup = self.abort();
        CommitRecordReport {
            commit: CommitReport {
                disposition: if cleanup.is_ok() {
                    CommitDisposition::Aborted(error)
                } else {
                    CommitDisposition::Unknown(error)
                },
                cleanup,
            },
            completion_contract_valid: true,
            record_bound: false,
            record_written: false,
        }
    }

    #[inline(always)]
    fn finish_commit(
        mut self,
        hook: sys::mako_local_post_validate_hook,
        context: *mut c_void,
    ) -> CommitReport {
        if self.record_preflight.is_some_and(|plan| !plan.is_empty()) {
            // A nonempty sealed plan may install only through the record
            // terminal. Ordinary commit here would bypass durability.
            let cleanup = self.abort();
            return CommitReport {
                disposition: match cleanup {
                    Ok(()) => CommitDisposition::Aborted(Error::InvalidArgument),
                    Err(error) => CommitDisposition::Unknown(error),
                },
                cleanup,
            };
        }
        let raw = self
            .raw
            .take()
            .expect("transaction handle already consumed");
        if !self.active {
            // An earlier terminal operation may have ended or quarantined
            // native state. Destroy consumes and invalidates a clean terminal
            // handle or reports quarantine, but commit must never touch
            // either state.
            let destroy = unsafe { abi::mako_local_txn_destroy(raw.as_ptr()) };
            return CommitReport {
                disposition: CommitDisposition::Aborted(Error::TransactionFinished),
                cleanup: status(destroy),
            };
        }
        self.active = false;
        if self.fast_bound_table.is_some() {
            // SAFETY: the handle is active and thread-affine. The callback and
            // context, when non-null, remain live for this synchronous call.
            // Native consumes and invalidates the pooled facade on every
            // returned terminal outcome and packs commit/cleanup separately.
            let packed = unsafe {
                match hook {
                    Some(_) => fast_abi::mako_rust_fast_txn_commit_with_hook_and_destroy(
                        raw.as_ptr(),
                        hook,
                        context,
                    ),
                    None => fast_abi::mako_rust_fast_txn_commit_and_destroy(raw.as_ptr()),
                }
            };
            let Some((commit, cleanup)) = decode_fast_commit(packed) else {
                // The terminal entry consumed the native handle, so there is
                // no safe cleanup retry. Preserve visibility as unknown and
                // make the malformed cleanup result explicit.
                return CommitReport {
                    disposition: CommitDisposition::Unknown(Error::Internal),
                    cleanup: Err(Error::Internal),
                };
            };
            return CommitReport {
                disposition: commit_disposition(commit),
                cleanup: status(cleanup),
            };
        }

        // SAFETY: the checked handle is live, active, and thread-affine. A
        // supplied callback is synchronous and does not outlive `context`.
        let commit = unsafe {
            match hook {
                Some(_) => abi::mako_local_txn_commit_with_hook(raw.as_ptr(), hook, context),
                None => abi::mako_local_txn_commit(raw.as_ptr()),
            }
        };
        // SAFETY: commit is terminal; destroy only consumes and invalidates
        // the facade handle. Native storage may be recycled internally.
        let destroy = unsafe { abi::mako_local_txn_destroy(raw.as_ptr()) };
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
        if !self.active {
            // A prior terminal operation may already have finished or
            // quarantined native state. In particular, never retry abort after
            // INTERNAL: the ABI permits only the one-shot destroy probe.
            let destroy = unsafe { abi::mako_local_txn_destroy(raw.as_ptr()) };
            status(destroy)?;
            return Err(Error::TransactionFinished);
        }
        self.active = false;
        if self.fast_bound_table.is_some() {
            // SAFETY: active fast-bound handle is uniquely owned on its
            // creator thread. Native aborts and consumes it in one call.
            let Some((abort, destroy)) = decode_fast_abort(unsafe {
                fast_abi::mako_rust_fast_txn_abort_and_destroy(raw.as_ptr())
            }) else {
                // The consuming native entry cannot be retried even when its
                // packed result violates the build-private contract.
                return Err(Error::Internal);
            };
            status(abort)?;
            return status(destroy);
        }
        // SAFETY: handle is live, active, and thread-affine by type.
        let abort = unsafe { abi::mako_local_txn_abort(raw.as_ptr()) };
        // SAFETY: abort is terminal.
        let destroy = unsafe { abi::mako_local_txn_destroy(raw.as_ptr()) };
        status(abort)?;
        status(destroy)
    }
}

struct PostValidateHook<F> {
    hook: Option<F>,
}

struct RecordBindHook<F> {
    hook: Option<F>,
    record: NonNull<UninitCommitRecord>,
    preflight: CommitRecordPreflight,
    bound: bool,
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

#[allow(clippy::too_many_arguments)]
unsafe extern "C" fn record_bind_trampoline<F>(
    context: *mut c_void,
    raw_timestamp: u32,
    exact_record_bytes: usize,
    sequence_out: *mut u64,
    record_bytes_out: *mut *mut u8,
    record_capacity_out: *mut usize,
) -> i32
where
    F: FnOnce(MakoTimestamp, CommitRecordPreflight) -> Option<NonZeroU64>,
{
    // Initialize every present output before inspecting any other argument.
    // Native supplies all three, but this keeps malformed calls fail-closed.
    if !sequence_out.is_null() {
        // SAFETY: a non-null ABI output is writable for this callback.
        unsafe { sequence_out.write(0) };
    }
    if !record_bytes_out.is_null() {
        // SAFETY: a non-null ABI output is writable for this callback.
        unsafe { record_bytes_out.write(std::ptr::null_mut()) };
    }
    if !record_capacity_out.is_null() {
        // SAFETY: a non-null ABI output is writable for this callback.
        unsafe { record_capacity_out.write(0) };
    }
    if context.is_null()
        || sequence_out.is_null()
        || record_bytes_out.is_null()
        || record_capacity_out.is_null()
    {
        return 0;
    }

    // SAFETY: commit_report_with_record passes this exact stack value and the
    // native contract invokes the callback synchronously at most once.
    let state = unsafe { &mut *context.cast::<RecordBindHook<F>>() };
    let Some(timestamp) = MakoTimestamp::new(raw_timestamp) else {
        return 0;
    };
    if exact_record_bytes != state.preflight.exact_record_bytes {
        return 0;
    }
    // SAFETY: the caller's exclusive record borrow spans the complete native
    // terminal call, and the allocation is never resized during that call.
    let record = unsafe { state.record.as_mut() };
    if record.written
        || record.preflight != state.preflight
        || record.bytes.len() != exact_record_bytes
    {
        return 0;
    }
    let Some(hook) = state.hook.take() else {
        return 0;
    };
    let Some(sequence) = catch_unwind(AssertUnwindSafe(|| hook(timestamp, state.preflight)))
        .ok()
        .flatten()
    else {
        return 0;
    };

    // Nothing below is fallible: once `bound` becomes true, native owns the
    // obligation to fill this exact storage before attempting installation.
    // SAFETY: required outputs were checked above.
    unsafe {
        sequence_out.write(sequence.get());
        record_bytes_out.write(record.bytes.as_mut_ptr().cast::<u8>());
        record_capacity_out.write(record.bytes.len());
    }
    state.bound = true;
    1
}

unsafe extern "C" fn test_commit_observer_trampoline(
    _context: *mut c_void,
    raw_phase: u32,
    mako_timestamp: u32,
) {
    let Some(phase) = TestCommitPhase::from_raw(raw_phase) else {
        return;
    };
    TEST_COMMIT_OBSERVER.with(|slot| {
        let Some(observer) = slot.get() else {
            return;
        };
        // A panic must never unwind through the C ABI or the noexcept C++
        // transaction core. A callback panic cannot change commit disposition.
        let _ = catch_unwind(AssertUnwindSafe(|| observer(phase, mako_timestamp)));
    });
}

impl Drop for Transaction<'_> {
    #[inline]
    fn drop(&mut self) {
        let Some(raw) = self.raw.take() else {
            return;
        };
        if self.fast_bound_table.is_some() && self.active {
            // SAFETY: !Send keeps Drop on the creator thread. The combined
            // operation consumes the facade even when abort cleanup poisons
            // the worker, so no separate destroy may follow.
            let _ = unsafe { fast_abi::mako_rust_fast_txn_abort_and_destroy(raw.as_ptr()) };
            return;
        }
        if self.active {
            // SAFETY: !Send keeps Drop on the creator thread in safe Rust.
            let _ = unsafe { abi::mako_local_txn_abort(raw.as_ptr()) };
        }
        // SAFETY: handle is no longer used after this call.
        let _ = unsafe { abi::mako_local_txn_destroy(raw.as_ptr()) };
    }
}

struct ForeignBytes(*mut u8);

impl Drop for ForeignBytes {
    fn drop(&mut self) {
        // SAFETY: null is accepted; any non-null pointer came from native get
        // and is freed exactly once by this guard.
        unsafe { abi::mako_local_bytes_free(self.0.cast::<c_void>()) };
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
    fn native_build_identity_is_checked_before_ordinary_abi_use() {
        let expected = identity_abi::EXPECTED_BUILD_FINGERPRINT;
        assert_eq!(
            validate_build_identity(
                identity_abi::EXPECTED_ENGINE_ID,
                expected.len(),
                Some(&expected)
            ),
            Ok(())
        );
        assert_eq!(
            validate_build_identity(b"another-engine", expected.len(), Some(&expected)),
            Err(Error::AbiEngineMismatch)
        );
        assert_eq!(
            validate_build_identity(identity_abi::EXPECTED_ENGINE_ID, expected.len() - 1, None),
            Err(Error::AbiBuildFingerprintSizeMismatch {
                expected: expected.len(),
                found: expected.len() - 1,
            })
        );
        assert_eq!(
            validate_build_identity(identity_abi::EXPECTED_ENGINE_ID, expected.len(), None),
            Err(Error::AbiBuildFingerprintMismatch)
        );
        let mut wrong = expected;
        wrong[0] ^= 0xff;
        assert_eq!(
            validate_build_identity(identity_abi::EXPECTED_ENGINE_ID, wrong.len(), Some(&wrong)),
            Err(Error::AbiBuildFingerprintMismatch)
        );
    }

    #[test]
    fn test_commit_phase_ids_and_feature_bit_are_stable() {
        let phases = [
            (
                sys::MAKO_LOCAL_TEST_COMMIT_WRITESET_LOCKED,
                TestCommitPhase::WritesetLocked,
            ),
            (
                sys::MAKO_LOCAL_TEST_COMMIT_MAKO_TIMESTAMP_ALLOCATED,
                TestCommitPhase::MakoTimestampAllocated,
            ),
            (
                sys::MAKO_LOCAL_TEST_COMMIT_LOCAL_VALIDATION_COMPLETE,
                TestCommitPhase::LocalValidationComplete,
            ),
            (
                sys::MAKO_LOCAL_TEST_COMMIT_PREINSTALL_ACCEPTED,
                TestCommitPhase::PreinstallAccepted,
            ),
            (
                sys::MAKO_LOCAL_TEST_COMMIT_FIRST_WRITE_INSTALLED,
                TestCommitPhase::FirstWriteInstalled,
            ),
            (
                sys::MAKO_LOCAL_TEST_COMMIT_ALL_WRITES_INSTALLED,
                TestCommitPhase::AllWritesInstalled,
            ),
        ];
        for (raw, phase) in phases {
            assert_eq!(TestCommitPhase::from_raw(raw), Some(phase));
            assert_eq!(phase as u32, raw);
        }
        assert_eq!(TestCommitPhase::from_raw(0), None);
        assert_eq!(TestCommitPhase::from_raw(u32::MAX), None);
        assert!(Features(sys::MAKO_LOCAL_FEATURE_TEST_COMMIT_OBSERVER).test_commit_observer());
        assert!(!Features(0).test_commit_observer());
        assert!(Features(sys::MAKO_LOCAL_FEATURE_TEST_CLEANUP_FAILURES).test_cleanup_failures());
        assert!(!Features(0).test_cleanup_failures());
        assert_eq!(
            TestCleanupBoundary::Begin as u32,
            sys::MAKO_LOCAL_CLEANUP_BOUNDARY_BEGIN
        );
        assert_eq!(
            TestCleanupBoundary::Operation as u32,
            sys::MAKO_LOCAL_CLEANUP_BOUNDARY_OPERATION
        );
        assert_eq!(
            TestCleanupBoundary::Commit as u32,
            sys::MAKO_LOCAL_CLEANUP_BOUNDARY_COMMIT
        );
        assert_eq!(
            TestCleanupBoundary::Abort as u32,
            sys::MAKO_LOCAL_CLEANUP_BOUNDARY_ABORT
        );
        assert_eq!(
            TestCleanupBoundary::Destroy as u32,
            sys::MAKO_LOCAL_CLEANUP_BOUNDARY_DESTROY
        );
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
            (
                sys::MAKO_LOCAL_FEATURE_UNAVAILABLE,
                Err(Error::FeatureUnavailable),
            ),
            (sys::MAKO_LOCAL_WORKER_POISONED, Err(Error::WorkerPoisoned)),
        ];

        assert_eq!(cases.len(), sys::ALL_KNOWN_STATUSES.len());
        for ((code, expected), known) in cases.into_iter().zip(sys::ALL_KNOWN_STATUSES) {
            assert_eq!(code, known.code(), "{}", known.c_symbol());
            assert_eq!(status(code), expected, "status {code}");
        }
        assert_eq!(status(-1), Err(Error::UnknownStatus(-1)));
        assert_eq!(status(i32::MAX), Err(Error::UnknownStatus(i32::MAX)));
    }

    #[test]
    fn every_status_has_an_explicit_operation_lifecycle() {
        for status in sys::ALL_KNOWN_STATUSES {
            let expected = match status {
                sys::KnownStatus::Ok
                | sys::KnownStatus::WrongThread
                | sys::KnownStatus::WrongDbOrTable
                | sys::KnownStatus::InvalidArgument
                | sys::KnownStatus::DuplicateWrite
                | sys::KnownStatus::ValueTooLarge
                | sys::KnownStatus::BufferTooSmall => OperationEffect::Active,
                sys::KnownStatus::Conflict
                | sys::KnownStatus::OutOfMemory
                | sys::KnownStatus::TxnTooLarge
                | sys::KnownStatus::Internal => OperationEffect::Finished,
                sys::KnownStatus::NotAttached
                | sys::KnownStatus::TxnAlreadyActive
                | sys::KnownStatus::TxnFinished
                | sys::KnownStatus::ThreadLimit
                | sys::KnownStatus::Busy
                | sys::KnownStatus::CommitHookRejected
                | sys::KnownStatus::TimestampExhausted
                | sys::KnownStatus::FeatureUnavailable => OperationEffect::Uncertain,
                sys::KnownStatus::WorkerPoisoned => OperationEffect::Quarantined,
            };
            assert_eq!(operation_effect(status.code()), expected, "{status:?}");
        }
        assert_eq!(operation_effect(-1), OperationEffect::Uncertain);
        assert_eq!(operation_effect(i32::MAX), OperationEffect::Uncertain);
    }

    #[test]
    fn every_status_has_an_explicit_commit_disposition() {
        for status in sys::ALL_KNOWN_STATUSES {
            let expected = match status {
                sys::KnownStatus::Ok => CommitDisposition::Committed,
                sys::KnownStatus::Conflict => CommitDisposition::Aborted(Error::Conflict),
                sys::KnownStatus::CommitHookRejected => {
                    CommitDisposition::Aborted(Error::CommitHookRejected)
                }
                sys::KnownStatus::TimestampExhausted => {
                    CommitDisposition::Aborted(Error::TimestampExhausted)
                }
                sys::KnownStatus::NotAttached
                | sys::KnownStatus::WrongThread
                | sys::KnownStatus::TxnAlreadyActive
                | sys::KnownStatus::TxnFinished
                | sys::KnownStatus::WrongDbOrTable
                | sys::KnownStatus::InvalidArgument
                | sys::KnownStatus::ThreadLimit
                | sys::KnownStatus::Busy
                | sys::KnownStatus::OutOfMemory
                | sys::KnownStatus::Internal
                | sys::KnownStatus::DuplicateWrite
                | sys::KnownStatus::TxnTooLarge
                | sys::KnownStatus::ValueTooLarge
                | sys::KnownStatus::BufferTooSmall
                | sys::KnownStatus::FeatureUnavailable
                | sys::KnownStatus::WorkerPoisoned => CommitDisposition::Unknown(
                    known_status(status).expect_err("non-success status has a typed error"),
                ),
            };
            assert_eq!(commit_disposition(status.code()), expected, "{status:?}");
        }
        assert_eq!(
            commit_disposition(-1),
            CommitDisposition::Unknown(Error::UnknownStatus(-1))
        );
    }
}
