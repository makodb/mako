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

use std::cell::{Cell, UnsafeCell};
use std::ffi::{CStr, c_void};
use std::fmt;
use std::marker::PhantomData;
use std::mem::{ManuallyDrop, MaybeUninit};
use std::num::{NonZeroU32, NonZeroU64};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr::NonNull;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, AtomicPtr, AtomicU64, Ordering};

use mako_local_sys as sys;

/// Return value of the cache-private callback-free record terminal.
///
/// Keep this layout in lockstep with
/// `mako_rust_fast_preselected_record_result` in the private native header.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
struct FastPreselectedRecordResult {
    terminal: u64,
    record_state: u64,
}

const _: [(); 16] = [(); std::mem::size_of::<FastPreselectedRecordResult>()];
const _: [(); 8] = [(); std::mem::align_of::<FastPreselectedRecordResult>()];

/// Return value of the cache-private callback-free concurrent arena terminal.
///
/// Keep this layout in lockstep with
/// `mako_rust_fast_native_ordered_arena_result` in the private native header.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
struct FastNativeOrderedArenaResult {
    terminal: u64,
    ordered_sequence: u64,
    record_state: u64,
}

const _: [(); 24] = [(); std::mem::size_of::<FastNativeOrderedArenaResult>()];
const _: [(); 8] = [(); std::mem::align_of::<FastNativeOrderedArenaResult>()];
const _: () = {
    assert!(std::mem::offset_of!(FastNativeOrderedArenaResult, terminal) == 0);
    assert!(std::mem::offset_of!(FastNativeOrderedArenaResult, ordered_sequence) == 8);
    assert!(std::mem::offset_of!(FastNativeOrderedArenaResult, record_state) == 16);
};

/// Stable Rust queue layout lent to the callback-free native arena terminal.
///
/// Native validates this same-build layout before assigning a sequence. The
/// fields stay private because changing any pointer or stride while a terminal
/// runs would break the dense-log and exclusive-arena ownership proofs.
#[doc(hidden)]
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct TrustedNativeOrderedArenaControl {
    next_bound: *mut u64,
    unhealthy: *const u8,
    publication_base: *mut u8,
    arena_base: *mut u8,
    publication_mask: usize,
    publication_shift: u32,
    publication_stride: u32,
    arena_stride: u32,
    arena_block_bytes: u32,
}

const _: [(); 56] = [(); std::mem::size_of::<TrustedNativeOrderedArenaControl>()];
const _: [(); 8] = [(); std::mem::align_of::<TrustedNativeOrderedArenaControl>()];
const _: () = {
    assert!(std::mem::offset_of!(TrustedNativeOrderedArenaControl, next_bound) == 0);
    assert!(std::mem::offset_of!(TrustedNativeOrderedArenaControl, unhealthy) == 8);
    assert!(std::mem::offset_of!(TrustedNativeOrderedArenaControl, publication_base) == 16);
    assert!(std::mem::offset_of!(TrustedNativeOrderedArenaControl, arena_base) == 24);
    assert!(std::mem::offset_of!(TrustedNativeOrderedArenaControl, publication_mask) == 32);
    assert!(std::mem::offset_of!(TrustedNativeOrderedArenaControl, publication_shift) == 40);
    assert!(std::mem::offset_of!(TrustedNativeOrderedArenaControl, publication_stride) == 44);
    assert!(std::mem::offset_of!(TrustedNativeOrderedArenaControl, arena_stride) == 48);
    assert!(std::mem::offset_of!(TrustedNativeOrderedArenaControl, arena_block_bytes) == 52);
};

impl TrustedNativeOrderedArenaControl {
    /// Describe one stable publication ring and its matching record arena.
    ///
    /// # Safety
    ///
    /// `next_bound` must point to a live, naturally aligned `AtomicU64`, and
    /// `unhealthy` must point to a live `AtomicBool`. Native validates the
    /// retained `next_bound` compatibility field without reading or updating
    /// its value; it accesses `unhealthy` with matching compiler atomics.
    /// `publication_base` must address
    /// `publication_mask + 1` fixed-stride cells whose atomic turn is at byte
    /// zero, Mako timestamp at byte 8, and record extent at byte 16.
    /// `arena_base` must address the same number of fixed-stride blocks with
    /// payload at byte zero. Both allocations and atomic words must remain at
    /// stable addresses until every call using this control has returned.
    ///
    /// The mask must describe a power-of-two ring and `publication_shift` must
    /// equal its base-two logarithm. Each prior generation must be retired
    /// before native can reuse its cell or arena block. Every concurrent
    /// cache-record terminal for the associated `LocalDb` must use the same
    /// packed namespace, compatibility control, and health word.
    #[allow(clippy::too_many_arguments)]
    pub const unsafe fn from_raw_parts(
        next_bound: *mut u64,
        unhealthy: *const u8,
        publication_base: *mut u8,
        arena_base: *mut u8,
        publication_mask: usize,
        publication_shift: u32,
        publication_stride: u32,
        arena_stride: u32,
        arena_block_bytes: u32,
    ) -> Self {
        Self {
            next_bound,
            unhealthy,
            publication_base,
            arena_base,
            publication_mask,
            publication_shift,
            publication_stride,
            arena_stride,
            arena_block_bytes,
        }
    }
}

/// Opaque build-private native holder-pool handle.
#[repr(C)]
struct FastOnePutHolderPool {
    _private: [u8; 0],
}

/// Stable queue-global inputs for the private fused SPSC terminal.
///
/// Keep this layout in lockstep with `mako_rust_fast_spsc_holder_control` in
/// the private native header. The pointed-to words are Rust atomics accessed
/// by native with matching compiler atomics during one synchronous call.
#[repr(C)]
struct FastSpscHolderControl {
    pool: *mut FastOnePutHolderPool,
    holder_base: *mut c_void,
    holder_mask: usize,
    acknowledged: *mut u64,
    unhealthy: *const u8,
    capacity: u64,
    max_record_bytes: u32,
    reserved: u32,
    cold_out: UnsafeCell<FastPreselectedRecordResult>,
}

const _: [(); 72] = [(); std::mem::size_of::<FastSpscHolderControl>()];
const _: [(); 8] = [(); std::mem::align_of::<FastSpscHolderControl>()];

/// Borrowed snapshot returned for one exact sealed holder generation.
///
/// Keep this layout in lockstep with
/// `mako_rust_fast_one_put_holder_view` in the private native header.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
struct FastOnePutHolderView {
    sequence: u64,
    table_id: u64,
    key: *const u8,
    value: *const u8,
    key_len: u32,
    value_len: u32,
    mako_timestamp: u32,
    reserved: u32,
}

const _: [(); 48] = [(); std::mem::size_of::<FastOnePutHolderView>()];
const _: [(); 8] = [(); std::mem::align_of::<FastOnePutHolderView>()];

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

    use super::{
        FastNativeOrderedArenaResult, FastOnePutHolderPool, FastOnePutHolderView,
        FastPreselectedRecordResult, FastSpscHolderControl, TrustedNativeOrderedArenaControl, sys,
    };

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

        pub(super) fn mako_rust_fast_txn_record_preflight_with_checksum(
            txn: *mut sys::mako_local_txn,
            max_record_bytes: usize,
            checksum_mode: u32,
            exact_record_bytes_out: *mut usize,
            op_count_out: *mut u32,
        ) -> i32;

        pub(super) fn mako_rust_fast_txn_commit_record_and_destroy(
            txn: *mut sys::mako_local_txn,
            hook: RecordBindHook,
            context: *mut c_void,
            record_written_out: *mut u8,
        ) -> u64;

        pub(super) fn mako_rust_fast_txn_commit_native_ordered_record_and_destroy(
            txn: *mut sys::mako_local_txn,
            unhealthy: *const u8,
            hook: RecordBindHook,
            context: *mut c_void,
            ordered_sequence_out: *mut u64,
            ordered_timestamp_out: *mut u32,
            record_written_out: *mut u8,
        ) -> u64;

        pub(super) fn mako_rust_fast_txn_commit_unchecked_one_put_record_and_destroy(
            txn: *mut sys::mako_local_txn,
            expected_record_bytes: u32,
            hook: RecordBindHook,
            context: *mut c_void,
            record_written_out: *mut u8,
        ) -> u64;

        pub(super) fn mako_rust_fast_txn_commit_native_ordered_unchecked_one_put_record_and_destroy(
            txn: *mut sys::mako_local_txn,
            expected_record_bytes: u32,
            next_bound: *mut u64,
            unhealthy: *const u8,
            hook: RecordBindHook,
            context: *mut c_void,
            ordered_sequence_out: *mut u64,
            ordered_timestamp_out: *mut u32,
            record_written_out: *mut u8,
        ) -> u64;

        pub(super) fn mako_rust_fast_txn_commit_native_ordered_unchecked_one_put_arena_and_destroy(
            txn: *mut sys::mako_local_txn,
            expected_record_bytes: u32,
            control: *const TrustedNativeOrderedArenaControl,
        ) -> FastNativeOrderedArenaResult;

        pub(super) fn mako_rust_fast_txn_commit_unchecked_one_put_record_single_producer_and_destroy(
            txn: *mut sys::mako_local_txn,
            expected_record_bytes: u32,
            hook: RecordBindHook,
            context: *mut c_void,
            record_written_out: *mut u8,
        ) -> u64;

        pub(super) fn mako_rust_fast_txn_commit_preselected_unchecked_one_put_record_single_producer_and_destroy(
            txn: *mut sys::mako_local_txn,
            expected_record_bytes: u32,
            sequence: u64,
            record: *mut u8,
            record_capacity: usize,
        ) -> FastPreselectedRecordResult;

        pub(super) fn mako_rust_fast_one_put_holder_pool_create(
            capacity: usize,
            key_reserve_bytes: u32,
            value_reserve_bytes: u32,
            out: *mut *mut FastOnePutHolderPool,
        ) -> i32;

        pub(super) fn mako_rust_fast_one_put_holder_pool_destroy(
            pool: *mut FastOnePutHolderPool,
        ) -> i32;

        pub(super) fn mako_rust_fast_one_put_holder_pool_get_hot_layout(
            pool: *mut FastOnePutHolderPool,
            holder_base_out: *mut *mut c_void,
            holder_mask_out: *mut usize,
        ) -> i32;

        pub(super) fn mako_rust_fast_txn_commit_preselected_unchecked_one_put_holder_single_producer_and_destroy(
            txn: *mut sys::mako_local_txn,
            expected_record_bytes: u32,
            pool: *mut FastOnePutHolderPool,
            sequence: u64,
        ) -> FastPreselectedRecordResult;

        pub(super) fn mako_rust_fast_txn_try_commit_fused_one_put_holder_single_producer_and_destroy(
            txn: *mut sys::mako_local_txn,
            acknowledged: *mut u64,
            unhealthy: *const u8,
            control: *mut FastSpscHolderControl,
            capacity_limit: u64,
        ) -> u64;

        pub(super) fn mako_rust_fast_one_put_holder_pool_get_view(
            pool: *const FastOnePutHolderPool,
            expected_sequence: u64,
            out: *mut FastOnePutHolderView,
        ) -> i32;

        pub(super) fn mako_rust_fast_one_put_holder_pool_release(
            pool: *mut FastOnePutHolderPool,
            expected_sequence: u64,
        ) -> i32;

        pub(super) fn mako_rust_fast_txn_abort_and_destroy(txn: *mut sys::mako_local_txn) -> u64;

        pub(super) fn mako_rust_fast_db_order_record_validation_prefix(db: *mut sys::mako_local_db);

        pub(super) fn mako_rust_fast_db_claim_cache_order_namespace(
            db: *mut sys::mako_local_db,
            foreground_mode: u32,
        ) -> i32;

        pub(super) fn mako_rust_fast_db_reseed_cache_order_namespace(
            db: *mut sys::mako_local_db,
            recovered_sequence: u64,
        ) -> i32;

        pub(super) fn mako_rust_fast_db_cache_order_snapshot(
            db: *const sys::mako_local_db,
        ) -> u64;
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

/// Immutable dense-order authority selected for one claimed cache namespace.
#[doc(hidden)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum CacheOrderMode {
    /// Native's packed process word allocates every concurrent dense sequence.
    Concurrent = 1,
    /// The exclusive Rust producer owns dense allocation for the claim.
    SingleProducer = 2,
}

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

/// Integrity mode for a native cache commit record.
///
/// CRC32C is the default and produces the backwards-compatible v3 format.
/// `None` produces a self-describing v4 record without a checksum trailer. It
/// avoids checksum work on the foreground commit path, but replay can then
/// validate only the record's structure, not arbitrary payload corruption.
#[doc(hidden)]
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum CommitRecordChecksum {
    /// Emit v4 without an integrity checksum.
    None = 0,
    /// Emit v3 with a CRC-32C trailer.
    #[default]
    Crc32c = 1,
}

/// Exact native sizing for one canonical cache commit record.
///
/// This build-private type is returned only for a trusted transaction whose
/// native write plan has been sealed. The byte count includes the selected
/// format's complete header and, for CRC32C/v3, its checksum trailer;
/// `op_count == 0` identifies a logical read-only transaction that must use the
/// ordinary no-record commit terminal.
#[doc(hidden)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CommitRecordPreflight {
    exact_record_bytes: usize,
    op_count: u32,
    checksum: CommitRecordChecksum,
}

impl CommitRecordPreflight {
    /// Complete encoded record size, including the header and optional CRC.
    pub const fn exact_record_bytes(self) -> usize {
        self.exact_record_bytes
    }

    /// Number of canonical final-effect mutations in the record.
    pub const fn op_count(self) -> u32 {
        self.op_count
    }

    /// Integrity mode sealed into this record plan.
    pub const fn checksum(self) -> CommitRecordChecksum {
        self.checksum
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

/// Compact result from the cache-private fused one-Put terminal.
///
/// The common success path retains native's packed terminal word and two
/// completion scalars instead of eagerly constructing the larger public
/// [`CommitRecordReport`]. A cache can branch on [`Self::is_committed`] and
/// defer full status/contract decoding to [`Self::into_report`] only for an
/// anomalous or unsuccessful native return.
///
/// This compact representation is private to the hidden fast ABI and assumes
/// the Rust crate and C++ engine were built from the same source/configuration
/// fingerprint. It is not a compatibility surface for an arbitrary provider
/// of the stable public ABI.
#[doc(hidden)]
#[derive(Debug, Clone, Copy)]
#[must_use = "a bound record outcome must be published or decoded and pinned before acknowledgement"]
pub struct TrustedUncheckedOnePutRecordOutcome {
    packed: u64,
    record_bound: bool,
    record_written: u8,
}

impl TrustedUncheckedOnePutRecordOutcome {
    /// Whether native returned the exact ordinary committed outcome.
    ///
    /// A true result proves both native visibility and cleanup succeeded and
    /// that the fused callback bound and completely initialized its target.
    #[inline(always)]
    pub const fn is_committed(self) -> bool {
        const PACKED_OK: u64 =
            (sys::MAKO_LOCAL_OK as u32 as u64) | ((sys::MAKO_LOCAL_OK as u32 as u64) << 32);
        self.packed == PACKED_OK && self.record_bound && self.record_written == 1
    }

    /// Perform the general fail-closed terminal and completion-contract decode.
    ///
    /// Cache code should call this only when [`Self::is_committed`] is false.
    #[cold]
    #[inline(never)]
    pub fn into_report(self) -> CommitRecordReport {
        let record_bound = self.record_bound;
        let witness_claimed = self.record_written == 1;
        let mut contract_valid = self.record_written <= 1 && (!witness_claimed || record_bound);
        let decoded = decode_fast_unchecked_record_commit(self.packed);
        if decoded.is_none() {
            contract_valid = false;
        }
        if let Some((commit, _)) = decoded {
            if commit == sys::MAKO_LOCAL_OK && !witness_claimed {
                contract_valid = false;
            }
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
                    disposition: if commit == sys::MAKO_LOCAL_INVALID_ARGUMENT {
                        CommitDisposition::Aborted(Error::InvalidArgument)
                    } else {
                        commit_disposition(commit)
                    },
                    cleanup: status(cleanup),
                },
            )
        } else {
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
}

/// Compact outcome for native-assigned concurrent cache ordering.
///
/// Native writes the accepted Mako timestamp and dense cache sequence while
/// certifying against the same packed process word, then invokes the target
/// callback after retiring the short ordering operation. A nonzero pair must
/// be adopted and published or pinned even when the callback was not reached.
#[doc(hidden)]
#[derive(Debug, Clone, Copy)]
#[must_use = "an accepted native order must be adopted before acknowledgement"]
pub struct TrustedNativeOrderedOnePutRecordOutcome {
    inner: TrustedUncheckedOnePutRecordOutcome,
    ordered_sequence: u64,
    ordered_timestamp: u32,
    target_bound: bool,
}

impl TrustedNativeOrderedOnePutRecordOutcome {
    /// Whether native returned either no order or one complete timestamp/sequence pair.
    ///
    /// Both members of a packed cache order fit the native 29-bit domain,
    /// whose largest representable value is [`MAX_MAKO_TIMESTAMP`].
    #[inline(always)]
    pub const fn order_witness_valid(self) -> bool {
        if self.ordered_sequence == 0 {
            self.ordered_timestamp == 0
        } else {
            self.ordered_sequence <= MAX_MAKO_TIMESTAMP as u64
                && self.ordered_timestamp != 0
                && self.ordered_timestamp <= MAX_MAKO_TIMESTAMP
        }
    }

    /// Return the accepted timestamp/sequence pair, if native assigned one.
    ///
    /// A one-zero/one-nonzero pair is malformed same-build ABI state. It is
    /// omitted here and makes [`Self::into_report`] fail its completion
    /// contract so the cache terminates rather than losing an obligation.
    #[inline(always)]
    pub fn accepted_order(self) -> Option<(MakoTimestamp, NonZeroU64)> {
        if !self.order_witness_valid() {
            return None;
        }
        Some((
            MakoTimestamp::new(self.ordered_timestamp)?,
            NonZeroU64::new(self.ordered_sequence)?,
        ))
    }

    /// Whether native committed, cleaned up, and completed the ordered target.
    #[inline(always)]
    pub const fn is_committed(self) -> bool {
        self.order_witness_valid()
            && self.ordered_sequence != 0
            && self.target_bound
            && self.inner.is_committed()
    }

    /// Decode the cold fail-closed completion report.
    #[cold]
    #[inline(never)]
    pub fn into_report(self) -> CommitRecordReport {
        let order_pair_valid = self.order_witness_valid();
        let mut report = self.inner.into_report();
        if !order_pair_valid || (self.inner.record_written == 1 && !self.target_bound) {
            report.completion_contract_valid = false;
            report.record_written = false;
        }
        report
    }
}

/// Compact result from the cache-private callback-free one-Put terminal.
///
/// The target and its dense sequence are selected before native validation.
/// Native's `record_state` then says whether that target was accepted into the
/// commit order: its low 32 bits carry the accepted Mako timestamp and bit 32
/// is the complete-record witness. Bits 33 through 63 are reserved and must be
/// zero. This representation lets the common committed path avoid callback
/// setup and output-pointer traffic while retaining a strict fail-stop decode
/// for every anomalous native result.
#[doc(hidden)]
#[derive(Debug, Clone, Copy)]
#[must_use = "a preselected record outcome must be published, released, or decoded and pinned"]
pub struct TrustedPreselectedUncheckedOnePutRecordOutcome {
    terminal: u64,
    record_state: u64,
}

impl TrustedPreselectedUncheckedOnePutRecordOutcome {
    /// Return the Mako timestamp with which native accepted the preselected
    /// target, if its low 32-bit timestamp field is valid and nonzero.
    ///
    /// This intentionally ignores the completion bit and reserved high bits.
    /// A cache must bind/pin the preselected slot whenever this returns `Some`,
    /// then separately use [`Self::is_committed`] or [`Self::into_report`] to
    /// validate the complete result. That ordering remains conservative when a
    /// same-build ABI defect corrupts another `record_state` bit.
    #[inline(always)]
    pub const fn accepted_timestamp(self) -> Option<MakoTimestamp> {
        MakoTimestamp::new(self.record_state as u32)
    }

    /// Whether native claims to have initialized every byte of the target.
    ///
    /// This is only the raw bit-32 witness. Bytes may be read only after the
    /// whole completion contract has also been validated by
    /// [`Self::is_committed`] or [`Self::into_report`].
    #[inline(always)]
    pub const fn record_written(self) -> bool {
        self.record_state & (1u64 << 32) != 0
    }

    /// Whether native returned the exact valid ordinary committed outcome.
    ///
    /// A true result proves successful visibility and cleanup, a valid accepted
    /// Mako timestamp, the complete-record witness, and zero reserved bits.
    #[inline(always)]
    pub const fn is_committed(self) -> bool {
        const PACKED_OK: u64 =
            (sys::MAKO_LOCAL_OK as u32 as u64) | ((sys::MAKO_LOCAL_OK as u32 as u64) << 32);
        self.terminal == PACKED_OK
            && self.record_state >> 33 == 0
            && self.record_written()
            && self.accepted_timestamp().is_some()
    }

    /// Perform the general fail-closed terminal and completion-contract decode.
    ///
    /// A zero timestamp is a valid pre-acceptance result only for a non-OK
    /// terminal. Once native reports an accepted timestamp, a complete witness
    /// is mandatory and only ordinary success or a quarantined unknown outcome
    /// is possible. Every other combination is an ABI contract violation and
    /// decodes to an internal unknown result so a cache cannot retry or reuse an
    /// accepted ordered slot.
    #[cold]
    #[inline(never)]
    pub fn into_report(self) -> CommitRecordReport {
        let raw_timestamp = self.record_state as u32;
        let accepted_timestamp = self.accepted_timestamp();
        let record_bound = accepted_timestamp.is_some();
        let witness_claimed = self.record_written();
        let reserved_bits_valid = self.record_state >> 33 == 0;
        let timestamp_valid = raw_timestamp == 0 || record_bound;
        let decoded = decode_fast_unchecked_record_commit(self.terminal);

        let mut contract_valid = reserved_bits_valid
            && timestamp_valid
            && (!witness_claimed || record_bound)
            && decoded.is_some();
        if let Some((commit, _)) = decoded {
            if record_bound {
                // The callback-free native terminal publishes its timestamp
                // only after the serializer completed successfully. From that
                // point the only valid outcomes are installation success or a
                // quarantined exception with uncertain visibility.
                contract_valid &= witness_claimed
                    && (commit == sys::MAKO_LOCAL_OK || commit == sys::MAKO_LOCAL_WORKER_POISONED);
            } else {
                // Success without acceptance would install a write that has no
                // durable ordered record. A witness without acceptance was
                // rejected above as well.
                contract_valid &= !witness_claimed && commit != sys::MAKO_LOCAL_OK;
            }
        }

        let commit = if contract_valid {
            decoded.map_or(
                CommitReport {
                    disposition: CommitDisposition::Unknown(Error::Internal),
                    cleanup: Err(Error::Internal),
                },
                |(commit, cleanup)| CommitReport {
                    disposition: if commit == sys::MAKO_LOCAL_INVALID_ARGUMENT {
                        CommitDisposition::Aborted(Error::InvalidArgument)
                    } else {
                        commit_disposition(commit)
                    },
                    cleanup: status(cleanup),
                },
            )
        } else {
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
}

/// Native commit visibility together with one preselected holder's progress.
///
/// This is the holder-pool counterpart of [`CommitRecordReport`]. A bound
/// holder is identified by a nonzero accepted Mako timestamp. `holder_sealed`
/// means native transferred the complete one-Put key/value payload into that
/// exact holder generation; it does not mean a cache record has already been
/// encoded.
#[doc(hidden)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[must_use = "an accepted holder must be published or pinned before acknowledgement"]
pub struct CommitHolderReport {
    /// Native commit visibility and facade cleanup outcomes.
    pub commit: CommitReport,
    /// Whether native's packed terminal and holder-state words form one
    /// internally consistent result.
    pub completion_contract_valid: bool,
    /// Whether native accepted this exact preselected generation into Mako's
    /// commit order.
    pub holder_bound: bool,
    /// Whether native sealed the complete key/value payload in the holder.
    pub holder_sealed: bool,
}

/// Compact result from the cache-private callback-free holder terminal.
///
/// The low 32 bits of `holder_state` carry the accepted Mako timestamp, bit 32
/// is the sealed-payload witness, and every higher bit is reserved. The cache
/// inspects the compact ordinary-success predicate before paying for the full
/// fail-closed decode.
#[doc(hidden)]
#[derive(Debug, Clone, Copy)]
#[must_use = "a preselected holder outcome must be published, released, or decoded and pinned"]
pub struct TrustedPreselectedUncheckedOnePutHolderOutcome {
    terminal: u64,
    holder_state: u64,
}

impl TrustedPreselectedUncheckedOnePutHolderOutcome {
    /// Return the timestamp only for the exact ordinary success word.
    ///
    /// This fuses terminal, sealed-bit, reserved-bit, and nonzero-timestamp
    /// validation so the cache's common path decodes the two-word ABI result
    /// once. Every non-success result must use the fail-closed accessors below.
    #[inline(always)]
    pub const fn committed_timestamp(self) -> Option<MakoTimestamp> {
        const PACKED_OK: u64 =
            (sys::MAKO_LOCAL_OK as u32 as u64) | ((sys::MAKO_LOCAL_OK as u32 as u64) << 32);
        if self.terminal == PACKED_OK && self.holder_state >> 32 == 1 {
            MakoTimestamp::new(self.holder_state as u32)
        } else {
            None
        }
    }

    /// Return native's accepted Mako timestamp, if it is nonzero and in range.
    ///
    /// A cache must make the exact preselected sequence visible or pin it
    /// whenever this returns `Some`, before performing any fallible decode.
    #[inline(always)]
    pub const fn accepted_timestamp(self) -> Option<MakoTimestamp> {
        MakoTimestamp::new(self.holder_state as u32)
    }

    /// Whether native claims the complete one-Put payload is sealed.
    #[inline(always)]
    pub const fn holder_sealed(self) -> bool {
        self.holder_state & (1u64 << 32) != 0
    }

    /// Whether native returned the exact ordinary committed holder outcome.
    #[inline(always)]
    pub const fn is_committed(self) -> bool {
        self.committed_timestamp().is_some()
    }

    /// Perform the complete anomalous/unsuccessful outcome decode.
    ///
    /// An accepted timestamp requires a sealed witness and permits only
    /// ordinary success or a quarantined unknown result. Without acceptance,
    /// success or a sealed witness would describe an uncovered visible write.
    #[cold]
    #[inline(never)]
    pub fn into_report(self) -> CommitHolderReport {
        let raw_timestamp = self.holder_state as u32;
        let accepted_timestamp = self.accepted_timestamp();
        let holder_bound = accepted_timestamp.is_some();
        let sealed_claimed = self.holder_sealed();
        let reserved_bits_valid = self.holder_state >> 33 == 0;
        let timestamp_valid = raw_timestamp == 0 || holder_bound;
        let decoded = decode_fast_unchecked_holder_commit(self.terminal);

        let mut contract_valid = reserved_bits_valid
            && timestamp_valid
            && (!sealed_claimed || holder_bound)
            && decoded.is_some();
        if let Some((commit, _)) = decoded {
            if holder_bound {
                contract_valid &= sealed_claimed
                    && (commit == sys::MAKO_LOCAL_OK || commit == sys::MAKO_LOCAL_WORKER_POISONED);
            } else {
                contract_valid &= !sealed_claimed && commit != sys::MAKO_LOCAL_OK;
            }
        }

        let commit = if contract_valid {
            decoded.map_or(
                CommitReport {
                    disposition: CommitDisposition::Unknown(Error::Internal),
                    cleanup: Err(Error::Internal),
                },
                |(commit, cleanup)| CommitReport {
                    disposition: holder_commit_disposition(commit),
                    cleanup: status(cleanup),
                },
            )
        } else {
            CommitReport {
                disposition: CommitDisposition::Unknown(Error::Internal),
                cleanup: Err(Error::Internal),
            }
        };
        CommitHolderReport {
            commit,
            completion_contract_valid: contract_valid,
            holder_bound,
            holder_sealed: contract_valid && sealed_claimed,
        }
    }
}

/// Fixed-capacity native storage for callback-free one-Put write-back.
///
/// The pool owns one holder per power-of-two queue ring position. It is safe
/// to share the handle, but access to each holder is governed by the cache's
/// external SPSC published/applied frontier protocol rather than an atomic in
/// the native holder itself.
#[doc(hidden)]
pub struct TrustedOnePutHolderPool {
    raw: NonNull<FastOnePutHolderPool>,
    holder_base: NonNull<c_void>,
    holder_mask: usize,
    capacity: usize,
}

impl fmt::Debug for TrustedOnePutHolderPool {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("TrustedOnePutHolderPool")
            .field("capacity", &self.capacity)
            .finish_non_exhaustive()
    }
}

// SAFETY: the opaque pool allocation has a stable address. Its private ABI
// requires the producer/consumer frontier happens-before protocol documented
// on the unsafe terminal/view/release methods below; sharing the handle alone
// reads or writes no holder payload.
unsafe impl Send for TrustedOnePutHolderPool {}
// SAFETY: as above.
unsafe impl Sync for TrustedOnePutHolderPool {}

impl TrustedOnePutHolderPool {
    /// Allocate an independent holder pool for one SPSC write-back ring.
    ///
    /// `capacity` must be a nonzero power of two and must equal the physical
    /// publication-ring capacity. The reserve values are cold allocation
    /// hints; zero leaves payload allocations lazy.
    pub fn new(capacity: usize, key_reserve_bytes: u32, value_reserve_bytes: u32) -> Result<Self> {
        verify_abi()?;
        if !capacity.is_power_of_two() || capacity == 0 {
            return Err(Error::InvalidArgument);
        }
        if key_reserve_bytes as usize > MAX_KEY_BYTES
            || value_reserve_bytes as usize > MAX_VALUE_BYTES
        {
            return Err(Error::ValueTooLarge);
        }

        let mut raw = std::ptr::null_mut();
        // SAFETY: `raw` remains writable for this synchronous call. Native
        // returns either one uniquely owned allocation or a null output.
        status(unsafe {
            fast_abi::mako_rust_fast_one_put_holder_pool_create(
                capacity,
                key_reserve_bytes,
                value_reserve_bytes,
                &mut raw,
            )
        })?;
        let raw = NonNull::new(raw).ok_or(Error::Internal)?;
        let mut holder_base = std::ptr::null_mut();
        let mut holder_mask = 0usize;
        // SAFETY: `raw` is the live pool allocation returned immediately
        // above. Native returns its immutable holder-array layout and retains
        // no output pointer.
        let layout = status(unsafe {
            fast_abi::mako_rust_fast_one_put_holder_pool_get_hot_layout(
                raw.as_ptr(),
                &mut holder_base,
                &mut holder_mask,
            )
        });
        let Some(holder_base) = NonNull::new(holder_base) else {
            // SAFETY: no holder generation has been exposed yet.
            let _ = unsafe { fast_abi::mako_rust_fast_one_put_holder_pool_destroy(raw.as_ptr()) };
            return Err(layout.err().unwrap_or(Error::Internal));
        };
        if let Err(error) = layout {
            // SAFETY: no holder generation has been exposed yet.
            let _ = unsafe { fast_abi::mako_rust_fast_one_put_holder_pool_destroy(raw.as_ptr()) };
            return Err(error);
        }
        if holder_mask != capacity - 1 {
            // SAFETY: no holder generation has been exposed yet.
            let _ = unsafe { fast_abi::mako_rust_fast_one_put_holder_pool_destroy(raw.as_ptr()) };
            return Err(Error::Internal);
        }
        Ok(Self {
            raw,
            holder_base,
            holder_mask,
            capacity,
        })
    }

    /// Number of exact generations addressable before a ring position repeats.
    pub const fn capacity(&self) -> usize {
        self.capacity
    }

    /// Borrow one exact sealed holder generation after queue publication.
    ///
    /// # Safety
    ///
    /// The caller must be the sole serialized consumer and must have acquired
    /// a Rust publication frontier covering `expected_sequence`. No overlapping
    /// view or release of this generation may exist. The returned value must
    /// remain alive through every use of its borrowed slices.
    pub unsafe fn get_view(
        &self,
        expected_sequence: NonZeroU64,
    ) -> Result<TrustedOnePutHolderView<'_>> {
        let mut raw = FastOnePutHolderView {
            sequence: 0,
            table_id: 0,
            key: std::ptr::null(),
            value: std::ptr::null(),
            key_len: 0,
            value_len: 0,
            mako_timestamp: 0,
            reserved: 0,
        };
        // SAFETY: the caller supplies the required cross-language Acquire and
        // exact-generation ownership. `raw` is a live writable output.
        status(unsafe {
            fast_abi::mako_rust_fast_one_put_holder_pool_get_view(
                self.raw.as_ptr(),
                expected_sequence.get(),
                &mut raw,
            )
        })?;

        if raw.sequence != expected_sequence.get()
            || raw.reserved != 0
            || raw.key_len as usize > MAX_KEY_BYTES
            || raw.value_len as usize > MAX_VALUE_BYTES
        {
            return Err(Error::Internal);
        }
        let mako_timestamp = MakoTimestamp::new(raw.mako_timestamp).ok_or(Error::Internal)?;
        let key = checked_holder_span(raw.key, raw.key_len as usize)?;
        let value = checked_holder_span(raw.value, raw.value_len as usize)?;
        Ok(TrustedOnePutHolderView {
            pool: self,
            sequence: expected_sequence,
            table_id: raw.table_id,
            mako_timestamp,
            key,
            key_len: raw.key_len as usize,
            value,
            value_len: raw.value_len as usize,
        })
    }

    /// Preferred compact spelling for [`Self::get_view`].
    ///
    /// # Safety
    ///
    /// The caller must satisfy the exact publication-Acquire, sole-consumer,
    /// and generation-ownership requirements of [`Self::get_view`].
    #[inline(always)]
    pub unsafe fn view(
        &self,
        expected_sequence: NonZeroU64,
    ) -> Result<TrustedOnePutHolderView<'_>> {
        // SAFETY: delegated unchanged to this method's contract.
        unsafe { self.get_view(expected_sequence) }
    }

    /// Release an exact borrowed generation after backend retirement.
    ///
    /// Consuming `view` ensures its safe slice accessors cannot be used after
    /// native makes this ring position reusable.
    ///
    /// # Safety
    ///
    /// Every backend use of the view's key/value must be complete. The caller
    /// must publish the matching applied frontier with Release only after this
    /// function succeeds.
    pub unsafe fn release(&self, view: TrustedOnePutHolderView<'_>) -> Result<()> {
        if !std::ptr::eq(self, view.pool) {
            return Err(Error::InvalidArgument);
        }
        let sequence = view.sequence;
        // SAFETY: the caller supplies backend retirement and sole-consumer
        // ownership; native validates the exact sequence generation again.
        status(unsafe {
            fast_abi::mako_rust_fast_one_put_holder_pool_release(self.raw.as_ptr(), sequence.get())
        })
    }
}

impl Drop for TrustedOnePutHolderPool {
    fn drop(&mut self) {
        // SAFETY: this is the unique allocation returned by create. Native
        // refuses to destroy a pool containing any non-free (PREPARED or
        // SEALED) generation. In that fail-stop/crash-simulation case ignoring
        // BUSY intentionally leaks the allocation rather than invalidating
        // queued raw spans.
        let _ = unsafe { fast_abi::mako_rust_fast_one_put_holder_pool_destroy(self.raw.as_ptr()) };
    }
}

/// Stable, same-build control block for the fused one-Put SPSC terminal.
///
/// This is deliberately a capability rather than a general public queue ABI.
/// It snapshots immutable queue configuration and process-stable addresses so
/// the ordinary terminal does not repeatedly assemble or copy them.
#[doc(hidden)]
pub struct TrustedSpscOnePutHolderControl {
    raw: FastSpscHolderControl,
}

impl fmt::Debug for TrustedSpscOnePutHolderControl {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("TrustedSpscOnePutHolderControl")
            .field("capacity", &self.raw.capacity)
            .field("max_record_bytes", &self.raw.max_record_bytes)
            .finish_non_exhaustive()
    }
}

// SAFETY: moving or sharing the control performs no foreign access. Its
// non-atomic cold scratch is touched only by unsafe terminal calls whose
// contract requires unique-producer serialization through the complete cold
// decode. The pointer lifetimes and atomic orders are documented on `new` and
// the fused terminal.
unsafe impl Send for TrustedSpscOnePutHolderControl {}
// SAFETY: as above.
unsafe impl Sync for TrustedSpscOnePutHolderControl {}

impl TrustedSpscOnePutHolderControl {
    /// Snapshot stable queue-global inputs for the private fused terminal.
    ///
    /// # Safety
    ///
    /// `pool`, `acknowledged`, and `unhealthy` must remain live and at stable
    /// addresses until the last terminal call using this value returns.
    /// `acknowledged` must point at naturally aligned `AtomicU64` storage and
    /// `unhealthy` at `AtomicBool` storage. Native Relaxed-loads and
    /// Release-stores `acknowledged`, Acquire-loads `unhealthy`, and
    /// plain-writes this control's `cold_out` scratch before returning either
    /// consumed cold code. No terminal call or cold decode using this control
    /// may overlap another terminal call using it.
    /// `capacity` must describe the same SPSC queue and must not exceed the
    /// holder pool's physical capacity. The caller must externally guarantee
    /// that only its unique producer invokes a fused terminal at a time.
    #[doc(hidden)]
    pub unsafe fn new(
        pool: &TrustedOnePutHolderPool,
        acknowledged: *mut u64,
        unhealthy: *const u8,
        capacity: NonZeroU64,
        max_record_bytes: NonZeroU32,
    ) -> Self {
        debug_assert!(!acknowledged.is_null());
        debug_assert!(!unhealthy.is_null());
        debug_assert!(capacity.get() <= pool.capacity as u64);
        Self {
            raw: FastSpscHolderControl {
                pool: pool.raw.as_ptr(),
                holder_base: pool.holder_base.as_ptr(),
                holder_mask: pool.holder_mask,
                acknowledged,
                unhealthy,
                capacity: capacity.get(),
                max_record_bytes: max_record_bytes.get(),
                reserved: 0,
                cold_out: UnsafeCell::new(FastPreselectedRecordResult {
                    terminal: 0,
                    record_state: 0,
                }),
            },
        }
    }

    /// Return the maximum representable record extent supplied to native.
    ///
    /// This diagnostic accessor is primarily for same-build boundary tests;
    /// fused candidates themselves are always `u32` extents.
    #[doc(hidden)]
    pub const fn max_record_bytes(&self) -> u32 {
        self.raw.max_record_bytes
    }
}

/// Ownership result of one attempt at the fused one-Put SPSC terminal.
///
/// Only the two `Untouched` variants retain the native transaction. Every
/// other variant means native consumed it, including malformed same-build ABI
/// results; this distinction prevents a later Rust Drop from touching a freed
/// native facade.
#[doc(hidden)]
#[derive(Debug)]
#[must_use = "the fused terminal result owns transaction and queue progress"]
pub enum TrustedFusedOnePutHolderAttempt {
    /// Native committed and Release-published ACK as the canonical tail.
    Published,
    /// The transaction is still active and needs the general commit path.
    UntouchedGeneral,
    /// The transaction is active, but the queue needs a cold capacity refresh.
    UntouchedSlow {
        /// Exact unchecked-v4 record extent rederived by native.
        exact_record_bytes: NonZeroU32,
    },
    /// Native committed behind a fail-stop barrier; Rust advanced the local cursor.
    CommittedUnpublished {
        /// Accepted Mako serialization timestamp.
        timestamp: MakoTimestamp,
        /// Exact unchecked-v4 record extent retained for cold pinning.
        exact_record_bytes: NonZeroU32,
    },
    /// Native consumed the transaction with a non-ordinary terminal outcome.
    ConsumedOutcome {
        /// Full compact terminal result for fail-closed cold decoding.
        outcome: TrustedPreselectedUncheckedOnePutHolderOutcome,
        /// Exact unchecked-v4 record extent retained for possible pinning.
        exact_record_bytes: NonZeroU32,
    },
    /// An explicit untouched result had malformed metadata; the handle is live.
    UntouchedMalformed,
    /// Any malformed non-untouched result; the handle is conservatively consumed.
    ConsumedMalformed,
}

/// Exact-generation borrowed view of one sealed native one-Put holder.
///
/// Slice borrows are tied to this value. Releasing the generation consumes the
/// view, so safe Rust cannot use a slice after the matching native release.
#[doc(hidden)]
pub struct TrustedOnePutHolderView<'pool> {
    pool: &'pool TrustedOnePutHolderPool,
    sequence: NonZeroU64,
    table_id: u64,
    mako_timestamp: MakoTimestamp,
    key: NonNull<u8>,
    key_len: usize,
    value: NonNull<u8>,
    value_len: usize,
}

impl fmt::Debug for TrustedOnePutHolderView<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("TrustedOnePutHolderView")
            .field("sequence", &self.sequence)
            .field("table_id", &self.table_id)
            .field("mako_timestamp", &self.mako_timestamp)
            .field("key_len", &self.key_len)
            .field("value_len", &self.value_len)
            .finish_non_exhaustive()
    }
}

impl TrustedOnePutHolderView<'_> {
    /// Exact dense sequence naming this holder generation.
    pub const fn sequence(&self) -> NonZeroU64 {
        self.sequence
    }

    /// Stable logical table identifier captured by native.
    pub const fn table_id(&self) -> u64 {
        self.table_id
    }

    /// Mako timestamp accepted for this transaction.
    pub const fn mako_timestamp(&self) -> MakoTimestamp {
        self.mako_timestamp
    }

    /// Raw application key, borrowed until this view is released or dropped.
    pub fn key(&self) -> &[u8] {
        // SAFETY: `get_view` validated the pointer/length pair, and the exact
        // holder remains sealed for this view's complete lifetime.
        unsafe { std::slice::from_raw_parts(self.key.as_ptr(), self.key_len) }
    }

    /// Raw application value without STO's private encoded-value trailer.
    pub fn value(&self) -> &[u8] {
        // SAFETY: same exact-generation borrow as `key`.
        unsafe { std::slice::from_raw_parts(self.value.as_ptr(), self.value_len) }
    }

    /// Release this generation after its backend batch has succeeded.
    ///
    /// # Safety
    ///
    /// Every backend use of [`Self::key`] and [`Self::value`] must be complete.
    /// The caller must publish the corresponding applied frontier with Release
    /// only after this function returns successfully.
    pub unsafe fn release_after_backend(self) -> Result<()> {
        let pool = self.pool;
        // SAFETY: delegated unchanged to this method's contract. Consuming
        // `self` prevents safe slice access after the release boundary.
        unsafe { pool.release(self) }
    }
}

fn checked_holder_span(pointer: *const u8, len: usize) -> Result<NonNull<u8>> {
    match NonNull::new(pointer.cast_mut()) {
        Some(pointer) => Ok(pointer),
        None if len == 0 => Ok(NonNull::dangling()),
        None => Err(Error::Internal),
    }
}

/// Raw writable storage for the allocation-free native record terminal.
///
/// This is a build-private optimization seam. It deliberately carries no Rust
/// lifetime: native writes the record after the binding callback returns, while
/// the same synchronous terminal call is still active. Callers must therefore
/// uphold the lifetime and aliasing contract of [`Self::from_raw_parts`].
#[doc(hidden)]
#[derive(Debug, Clone, Copy)]
pub struct CommitRecordTarget {
    sequence: NonZeroU64,
    bytes: NonNull<u8>,
    exact_record_bytes: usize,
}

impl CommitRecordTarget {
    /// Describe storage into which native may initialize one complete record.
    ///
    /// # Safety
    ///
    /// `bytes` must remain allocated, stable, and exclusively writable for
    /// `exact_record_bytes` bytes until the synchronous record terminal that
    /// receives this target returns. This includes
    /// [`Transaction::commit_report_with_record_target`],
    /// [`Transaction::commit_report_with_unchecked_one_put_record_target`], and
    /// [`Transaction::commit_trusted_unchecked_one_put_record_target`], as well
    /// as the callback-free preselected terminal. The storage may be
    /// uninitialized on entry, and any callback that returns this target must
    /// not unwind.
    ///
    /// For a terminal returning [`CommitRecordReport`], the storage must not be
    /// read as initialized unless its completion contract is valid and
    /// `record_written` is true. For the trusted fused terminal, an
    /// [`TrustedUncheckedOnePutRecordOutcome::is_committed`] result of `true`
    /// provides that witness. Otherwise the caller must first use
    /// [`TrustedUncheckedOnePutRecordOutcome::into_report`] and apply the same
    /// valid-contract plus `record_written` rule.
    pub unsafe fn from_raw_parts(
        sequence: NonZeroU64,
        bytes: NonNull<u8>,
        exact_record_bytes: usize,
    ) -> Self {
        Self {
            sequence,
            bytes,
            exact_record_bytes,
        }
    }
}

/// Maximum table-name length accepted by the draft ABI.
pub const MAX_TABLE_NAME_BYTES: usize = sys::MAKO_LOCAL_MAX_TABLE_NAME_BYTES as usize;
/// Maximum key length accepted by the draft ABI.
pub const MAX_KEY_BYTES: usize = sys::MAKO_LOCAL_MAX_KEY_BYTES as usize;
/// Maximum value length accepted by the draft ABI.
pub const MAX_VALUE_BYTES: usize = sys::MAKO_LOCAL_MAX_VALUE_BYTES as usize;
/// Weighted native item budget for one draft transaction.
pub const TRANSACTION_ITEM_BUDGET: usize = sys::MAKO_LOCAL_TXN_ITEM_BUDGET as usize;

const UNCHECKED_ONE_PUT_RECORD_OVERHEAD_BYTES: usize = 26 + 17;
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
fn decode_fast_put(
    packed: u64,
    expected_unchecked_record_bytes: usize,
) -> Option<(i32, bool, Option<NonZeroU32>)> {
    let status = packed as u32 as i32;
    let created = packed & (1_u64 << 32) != 0;
    let raw_record_bytes = u32::try_from(packed >> 33).ok()?;
    let record_bytes = NonZeroU32::new(raw_record_bytes);
    // Native must not claim creation or a usable direct-write candidate for a
    // failed operation. A nonzero candidate must exactly match the key/value
    // lengths Rust supplied to this same synchronous call.
    if status != sys::MAKO_LOCAL_OK && (created || record_bytes.is_some()) {
        return None;
    }
    if let Some(record_bytes) = record_bytes {
        if usize::try_from(record_bytes.get()).ok()? != expected_unchecked_record_bytes {
            return None;
        }
    }
    Some((status, created, record_bytes))
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
fn decode_fast_unchecked_record_commit(packed: u64) -> Option<(i32, i32)> {
    let commit = packed as u32 as i32;
    let cleanup = (packed >> 32) as u32 as i32;
    if commit == sys::MAKO_LOCAL_INVALID_ARGUMENT && cleanup == sys::MAKO_LOCAL_OK {
        // The fused terminal revalidates its borrowed direct-write witness and
        // exact size natively. A mismatch is a documented definite abort even
        // if Rust's conservative mirror still held a candidate.
        Some((commit, cleanup))
    } else {
        decode_fast_commit(packed)
    }
}

#[inline]
fn decode_fast_unchecked_holder_commit(packed: u64) -> Option<(i32, i32)> {
    let commit = packed as u32 as i32;
    let cleanup = (packed >> 32) as u32 as i32;
    if matches!(
        commit,
        sys::MAKO_LOCAL_INVALID_ARGUMENT | sys::MAKO_LOCAL_OUT_OF_MEMORY | sys::MAKO_LOCAL_INTERNAL
    ) && cleanup == sys::MAKO_LOCAL_OK
    {
        // Holder shape/generation rejection and long-key preparation happen
        // before native marks the holder PREPARED or enters commit. The native
        // helper completes abort cleanup before returning these statuses, so
        // they are definite pre-acceptance aborts rather than unknown writes.
        Some((commit, cleanup))
    } else {
        decode_fast_commit(packed)
    }
}

fn holder_commit_disposition(code: i32) -> CommitDisposition {
    if matches!(
        code,
        sys::MAKO_LOCAL_INVALID_ARGUMENT | sys::MAKO_LOCAL_OUT_OF_MEMORY | sys::MAKO_LOCAL_INTERNAL
    ) {
        let error = sys::KnownStatus::from_code(code)
            .and_then(|status| known_status(status).err())
            .unwrap_or(Error::UnknownStatus(code));
        CommitDisposition::Aborted(error)
    } else {
        commit_disposition(code)
    }
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
    // Cache the single build-private table binding used by mako-cache. Native
    // owns this handle until `raw` is closed, and every load is converted back
    // into a borrow tied to `self`; the atomic keeps ordinary LocalDb sharing
    // unchanged while removing table-name construction and the native table
    // map mutex from every transaction begin.
    trusted_table: AtomicPtr<sys::mako_local_table>,
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
        Ok(Self {
            raw,
            trusted_table: AtomicPtr::new(std::ptr::null_mut()),
        })
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
            unchecked_one_put_record_bytes: None,
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
            unchecked_one_put_record_bytes: None,
            _db: PhantomData,
            _thread_affine: PhantomData,
        })
    }

    /// Bind the one table used by the build-private cache transaction path.
    ///
    /// Repeating this call for the same native table is harmless. Attempting
    /// to replace it with a different table fails, keeping later lock-free
    /// loads unambiguous. The ordinary public multi-table API is unaffected.
    #[doc(hidden)]
    pub fn bind_trusted_table(&self, name: impl AsRef<[u8]>, table_id: u64) -> Result<()> {
        let table = self.open_table(name, table_id)?;
        let raw = table.raw.as_ptr();
        match self.trusted_table.compare_exchange(
            std::ptr::null_mut(),
            raw,
            Ordering::Release,
            Ordering::Acquire,
        ) {
            Ok(_) => Ok(()),
            Err(existing) if existing == raw => Ok(()),
            Err(_) => Err(Error::WrongDatabaseOrTable),
        }
    }

    /// Begin on the table previously installed by [`Self::bind_trusted_table`].
    ///
    /// The returned table and transaction both borrow this database. The
    /// native table pointer is process-stable until the database facade is
    /// closed, so the common path is one atomic load plus the existing trusted
    /// transaction begin instead of a table-map lookup and mutex acquisition.
    #[doc(hidden)]
    pub fn trusted_bound_transaction<'db>(&'db self) -> Result<(Table<'db>, Transaction<'db>)> {
        let raw = NonNull::new(self.trusted_table.load(Ordering::Acquire))
            .ok_or(Error::FeatureUnavailable)?;
        let table = Table {
            raw,
            _db: PhantomData,
        };
        let transaction = self.trusted_transaction(&table)?;
        Ok((table, transaction))
    }

    /// Claim the process-wide cache-order namespace for this database.
    ///
    /// The local cache supports one recovered namespace per process. This
    /// build-private call enforces that contract before recovery admits work,
    /// records one immutable foreground mode, and resets only the
    /// namespace-local dense sequence.
    ///
    /// # Safety
    ///
    /// The caller must exclusively own this newly opened facade. No legacy or
    /// packed record terminal may overlap the claim, and `mode` must remain the
    /// cache's foreground mode until the facade closes.
    #[doc(hidden)]
    pub unsafe fn claim_cache_order_namespace(&self, mode: CacheOrderMode) -> Result<()> {
        // SAFETY: `self.raw` is live for this synchronous claim. Native keeps
        // only the facade identity and releases it when the facade closes.
        status(unsafe {
            fast_abi::mako_rust_fast_db_claim_cache_order_namespace(
                self.raw.as_ptr(),
                mode as u32,
            )
        })
    }

    /// Set the recovered dense cache tail for this claimed namespace.
    ///
    /// # Safety
    ///
    /// The caller must exclusively own construction/recovery for this claimed
    /// LocalDb. No foreground transaction, cache-order terminal, or read-only
    /// cut may overlap this reset; lowering a live dense field would duplicate
    /// an already assigned sequence.
    #[doc(hidden)]
    pub unsafe fn reseed_cache_order_namespace(&self, recovered_sequence: u64) -> Result<()> {
        // SAFETY: cache construction calls this before sharing the facade or
        // admitting a foreground terminal.
        status(unsafe {
            fast_abi::mako_rust_fast_db_reseed_cache_order_namespace(
                self.raw.as_ptr(),
                recovered_sequence,
            )
        })
    }

    /// Return the packed cache-order state for diagnostics and cold checks.
    #[doc(hidden)]
    pub fn cache_order_snapshot(&self) -> u64 {
        // SAFETY: the live claimed facade is borrowed for this load only.
        unsafe { fast_abi::mako_rust_fast_db_cache_order_snapshot(self.raw.as_ptr()) }
    }

    /// Place a packed-state modification-order cut before a following cache
    /// outcome scan.
    ///
    /// This build-private synchronization seam is used only by mako-cache's
    /// read-only commit fence. Concurrent cache writers publish their Rust
    /// outcome slot before native assigns its packed order, then clear it only
    /// after the native outcome is represented in write-back.
    #[doc(hidden)]
    #[inline]
    pub fn order_record_validation_prefix(&self) {
        // SAFETY: `self.raw` is the live database allocation for this shared
        // facade. The private helper borrows it only for this synchronous RMW.
        unsafe { fast_abi::mako_rust_fast_db_order_record_validation_prefix(self.raw.as_ptr()) }
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
    // Exact unchecked-v4 extent advertised by the latest private fast Put.
    // Native returns it only while a direct one-Put canonical witness remains
    // usable; every other operation clears this conservative Rust mirror.
    unchecked_one_put_record_bytes: Option<NonZeroU32>,
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
        self.unchecked_one_put_record_bytes = None;
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
        self.unchecked_one_put_record_bytes = None;
        Scan::new(self, *table, ScanDirection::Reverse, lower, upper)
    }

    /// Read a key, returning owned bytes. Missing and present-empty are
    /// distinct (`None` versus `Some(Vec::new())`).
    pub fn get(&mut self, table: &Table<'db>, key: &[u8]) -> Result<Option<Vec<u8>>> {
        self.unchecked_one_put_record_bytes = None;
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
            self.unchecked_one_put_record_bytes = None;
            if key.len() > MAX_KEY_BYTES || value.len() > MAX_VALUE_BYTES {
                return Err(Error::ValueTooLarge);
            }
            let expected_unchecked_record_bytes = UNCHECKED_ONE_PUT_RECORD_OVERHEAD_BYTES
                .checked_add(key.len())
                .and_then(|bytes| bytes.checked_add(value.len()))
                .ok_or(Error::Internal)?;
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
            let Some((code, created, unchecked_record_bytes)) =
                decode_fast_put(packed, expected_unchecked_record_bytes)
            else {
                return self.fail_closed(Error::Internal);
            };
            self.unchecked_one_put_record_bytes = unchecked_record_bytes;
            self.operation_status(code)?;
            return Ok(created);
        }

        self.unchecked_one_put_record_bytes = None;
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
        self.unchecked_one_put_record_bytes = None;
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
        self.unchecked_one_put_record_bytes = None;
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

    /// Return the exact direct one-Put unchecked-v4 record candidate.
    ///
    /// Unlike [`Self::commit_record_preflight_with_checksum`], this performs
    /// no ABI call and does not seal native state. It is available only when
    /// the latest operation was the transaction's first trusted fast Put and
    /// no later read or mutation retired its direct canonical witness. The
    /// caller may reserve this exact extent before write locking, but must
    /// perform no further transaction operation before consuming `self` with
    /// [`Self::commit_report_with_unchecked_one_put_record_target`]. Native
    /// revalidates the candidate fail-closed at that terminal boundary.
    #[doc(hidden)]
    pub fn unchecked_one_put_record_candidate(&self) -> Option<CommitRecordPreflight> {
        if !self.active
            || self.raw.is_none()
            || self.fast_bound_table.is_none()
            || self.record_preflight.is_some()
        {
            return None;
        }
        self.unchecked_one_put_record_bytes
            .map(|exact_record_bytes| CommitRecordPreflight {
                exact_record_bytes: exact_record_bytes.get() as usize,
                op_count: 1,
                checksum: CommitRecordChecksum::None,
            })
    }

    /// Return only the compact unchecked-v4 extent for the cache-private
    /// single-producer fast path.
    ///
    /// This deliberately omits the redundant facade-state checks and
    /// [`CommitRecordPreflight`] construction performed by
    /// [`Self::unchecked_one_put_record_candidate`]. Native revalidates the
    /// complete one-Put witness at the consuming terminal. Keeping this as an
    /// unsafe, build-private primitive lets a caller which already owns the
    /// active transaction protocol avoid materializing a three-field plan on
    /// every acknowledgement.
    ///
    /// # Safety
    ///
    /// The caller must own an active trusted-bound transaction and must not
    /// perform another transaction operation between this accessor and the
    /// matching consuming terminal. `None` must select the general preflight
    /// path rather than an unchecked terminal.
    #[doc(hidden)]
    #[inline(always)]
    pub unsafe fn trusted_unchecked_one_put_record_bytes_candidate(&self) -> Option<NonZeroU32> {
        debug_assert!(self.active);
        debug_assert!(self.raw.is_some());
        debug_assert!(self.fast_bound_table.is_some());
        debug_assert!(self.record_preflight.is_none());
        self.unchecked_one_put_record_bytes
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
        self.commit_record_preflight_with_checksum(max_record_bytes, CommitRecordChecksum::Crc32c)
    }

    /// Seal and size native's canonical cache record in an explicit integrity
    /// mode.
    ///
    /// This has the same one-shot and fail-closed contract as
    /// [`Self::commit_record_preflight`]. CRC32C is the safe default; selecting
    /// [`CommitRecordChecksum::None`] produces a self-describing unchecked v4
    /// record and deliberately gives up payload-corruption detection.
    #[doc(hidden)]
    pub fn commit_record_preflight_with_checksum(
        &mut self,
        max_record_bytes: usize,
        checksum: CommitRecordChecksum,
    ) -> Result<CommitRecordPreflight> {
        let raw = self.active_raw()?;
        self.unchecked_one_put_record_bytes = None;
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
            fast_abi::mako_rust_fast_txn_record_preflight_with_checksum(
                raw,
                max_record_bytes,
                checksum as u32,
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
            checksum,
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

    /// Commit while native serializes directly into caller-managed storage.
    ///
    /// This is the allocation-free counterpart of
    /// [`Self::commit_report_with_record`]. It keeps the same native ordering
    /// and completion-witness protocol, but the post-validation callback
    /// supplies the final storage together with the dense cache sequence. This
    /// lets a durability adapter bind a preallocated queue buffer without
    /// allocating a separate [`UninitCommitRecord`] on every transaction.
    ///
    /// # Safety
    ///
    /// Every target returned by `acquire` must satisfy the safety contract of
    /// [`CommitRecordTarget::from_raw_parts`]. In particular, its allocation
    /// must remain stable and exclusively writable until this method returns,
    /// even though `acquire` itself has already returned. `acquire` must not
    /// panic or otherwise unwind. The target bytes remain uninitialized unless
    /// the returned report has both a valid completion contract and
    /// `record_written == true`.
    #[doc(hidden)]
    pub unsafe fn commit_report_with_record_target<F>(mut self, acquire: F) -> CommitRecordReport
    where
        F: FnOnce(MakoTimestamp, CommitRecordPreflight) -> Option<CommitRecordTarget>,
    {
        let Some(preflight) = self.record_preflight else {
            return self.reject_record_commit(Error::InvalidArgument);
        };
        if self.fast_bound_table.is_none() || preflight.is_empty() {
            return self.reject_record_commit(Error::InvalidArgument);
        }

        let raw = self
            .raw
            .take()
            .expect("transaction handle already consumed");
        if !self.active {
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

        let mut state = RecordTargetBindHook {
            hook: Some(acquire),
            preflight,
            bound: false,
        };
        let mut record_written = 0u8;
        // SAFETY: the transaction is active and consumed on every outcome.
        // The method's contract makes the callback and any returned storage
        // live, stable, exclusively writable, and non-unwinding through this
        // complete synchronous call.
        let packed = unsafe {
            fast_abi::mako_rust_fast_txn_commit_record_and_destroy(
                raw.as_ptr(),
                Some(record_target_bind_trampoline::<F>),
                std::ptr::from_mut(&mut state).cast::<c_void>(),
                &mut record_written,
            )
        };

        let record_bound = state.bound;
        let witness_claimed = record_written == 1;
        let mut contract_valid = record_written <= 1 && (!witness_claimed || record_bound);
        let decoded = decode_fast_commit(packed);
        if decoded.is_none() {
            contract_valid = false;
        }
        if let Some((commit, _)) = decoded {
            if commit == sys::MAKO_LOCAL_OK && !witness_claimed {
                contract_valid = false;
            }
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

    /// Commit one preflighted general transaction with a native-assigned
    /// timestamp/dense-sequence pair.
    ///
    /// Native owns the packed general-certification bit across timestamp
    /// allocation and final validation, assigns the dense sequence only after
    /// validation succeeds, then releases the bit before invoking `acquire`.
    /// The callback must adopt that exact sequence into the cache queue; it
    /// must never allocate or substitute a Rust-side sequence.
    ///
    /// # Safety
    ///
    /// The transaction must be active, bound to the claimed cache-order
    /// LocalDb, and hold the exact current nonempty record preflight. The
    /// `unhealthy` word and every target returned by `acquire` must remain live
    /// for this synchronous call. Once native exposes a nonzero order, the
    /// caller must publish or pin that exact generation on every outcome.
    #[doc(hidden)]
    pub unsafe fn commit_trusted_native_ordered_record_target<F>(
        mut self,
        unhealthy: &AtomicBool,
        acquire: F,
    ) -> TrustedNativeOrderedOnePutRecordOutcome
    where
        F: FnOnce(MakoTimestamp, CommitRecordPreflight, NonZeroU64) -> Option<CommitRecordTarget>,
    {
        debug_assert!(self.fast_bound_table.is_some());
        debug_assert!(self.active);
        debug_assert!(self.raw.is_some());
        let preflight = self
            .record_preflight
            .expect("native-ordered general commit requires preflight");
        debug_assert!(!preflight.is_empty());

        // SAFETY: this trusted terminal consumes the live thread-affine handle
        // on every outcome, and the method contract retains all borrowed state.
        let raw = unsafe { self.raw.take().unwrap_unchecked() };
        self.active = false;
        let mut state = NativeOrderedRecordTargetBindHook {
            hook: Some(acquire),
            preflight,
            bound: false,
        };
        let mut ordered_sequence = 0u64;
        let mut ordered_timestamp = 0u32;
        let mut record_written = 0u8;
        // SAFETY: the callback state and scalar outputs remain live and pinned
        // until the same-build synchronous terminal has returned.
        let packed = unsafe {
            fast_abi::mako_rust_fast_txn_commit_native_ordered_record_and_destroy(
                raw.as_ptr(),
                unhealthy.as_ptr().cast::<u8>(),
                Some(native_ordered_one_put_record_target_bind_trampoline::<F>),
                std::ptr::from_mut(&mut state).cast::<c_void>(),
                &mut ordered_sequence,
                &mut ordered_timestamp,
                &mut record_written,
            )
        };
        debug_assert!(!state.bound || ordered_sequence != 0);
        TrustedNativeOrderedOnePutRecordOutcome {
            inner: TrustedUncheckedOnePutRecordOutcome {
                packed,
                record_bound: ordered_sequence != 0,
                record_written,
            },
            ordered_sequence,
            ordered_timestamp,
            target_bound: state.bound,
        }
    }

    /// Commit a direct one-Put transaction without a separate preflight call.
    ///
    /// `candidate` must be the value returned by
    /// [`Self::unchecked_one_put_record_candidate`] after the final operation.
    /// Native rederives that exact one-Put shape before acquiring write locks,
    /// seals it as an unchecked-v4 record, and otherwise uses the same ordered
    /// post-validation binding and serialization-before-install protocol as
    /// [`Self::commit_report_with_record_target`]. A stale candidate, later
    /// operation, or native shape mismatch definitely aborts without invoking
    /// `acquire`.
    ///
    /// # Safety
    ///
    /// Before entering this method, the caller must have reserved capacity for
    /// exactly `candidate.exact_record_bytes()` without holding any native
    /// write lock. Every target returned by `acquire` must satisfy
    /// [`CommitRecordTarget::from_raw_parts`], remain stable and exclusively
    /// writable through this synchronous call, and must not alias the
    /// transaction's key/value storage. Its extent must equal
    /// `candidate.exact_record_bytes()`. `acquire` must not unwind.
    ///
    /// This fused build-private terminal trusts native to pass its validated
    /// non-null callback context, outputs, timestamp, and exact candidate size
    /// directly to the callback. The dedicated callback deliberately does not
    /// repeat those generic-boundary checks. Returning `None` remains a
    /// fail-closed preinstall rejection.
    #[doc(hidden)]
    pub unsafe fn commit_report_with_unchecked_one_put_record_target<F>(
        self,
        candidate: CommitRecordPreflight,
        acquire: F,
    ) -> CommitRecordReport
    where
        F: FnOnce(MakoTimestamp, CommitRecordPreflight) -> Option<CommitRecordTarget>,
    {
        let expected_record_bytes = u32::try_from(candidate.exact_record_bytes)
            .ok()
            .and_then(NonZeroU32::new);
        if self.fast_bound_table.is_none()
            || self.record_preflight.is_some()
            || candidate.is_empty()
            || candidate.op_count != 1
            || candidate.checksum != CommitRecordChecksum::None
            || expected_record_bytes != self.unchecked_one_put_record_bytes
        {
            return self.reject_record_commit(Error::InvalidArgument);
        }
        let expected_record_bytes = expected_record_bytes
            .expect("validated unchecked one-put record size fits u32")
            .get();

        if !self.active {
            let commit = self.finish_commit(None, std::ptr::null_mut());
            return CommitRecordReport {
                commit,
                completion_contract_valid: true,
                record_bound: false,
                record_written: false,
            };
        }
        debug_assert_eq!(expected_record_bytes as usize, candidate.exact_record_bytes);
        // SAFETY: every trusted precondition below was checked above, and this
        // method carries the same callback/storage contract.
        unsafe {
            self.commit_trusted_unchecked_one_put_record_target(candidate, acquire)
                .into_report()
        }
    }

    /// Commit the cache's already-verified direct one-Put candidate.
    ///
    /// This is the success-biased counterpart of
    /// [`Self::commit_report_with_unchecked_one_put_record_target`]. It returns
    /// native's compact terminal state without eagerly decoding a
    /// [`CommitRecordReport`]. Native still independently rederives the direct
    /// one-Put shape and definitely aborts before invoking `acquire` if that
    /// witness is stale.
    ///
    /// # Safety
    ///
    /// `self` must be an active trusted-bound transaction, and `candidate`
    /// must be the exact current result of
    /// [`Self::unchecked_one_put_record_candidate`] after the final operation.
    /// The caller must have reserved exactly its advertised nonzero extent
    /// before entering native commit. Every target returned by `acquire` must
    /// have that same extent, satisfy [`CommitRecordTarget::from_raw_parts`],
    /// remain stable and exclusively writable for the synchronous call, not
    /// alias transaction key/value storage, and the callback must not unwind.
    /// A false [`TrustedUncheckedOnePutRecordOutcome::is_committed`] result
    /// must be converted with
    /// [`TrustedUncheckedOnePutRecordOutcome::into_report`] and handled with
    /// the general fail-closed outcome protocol.
    ///
    /// The hidden fused ABI must come from the exact source/configuration
    /// fingerprint expected by this crate. In release builds its dedicated
    /// callback intentionally trusts native's validated non-null pointers,
    /// timestamp, extent, and single-invocation guarantee instead of repeating
    /// those checks; linking a merely layout-compatible implementation does
    /// not satisfy this safety contract.
    #[doc(hidden)]
    pub unsafe fn commit_trusted_unchecked_one_put_record_target<F>(
        self,
        candidate: CommitRecordPreflight,
        acquire: F,
    ) -> TrustedUncheckedOnePutRecordOutcome
    where
        F: FnOnce(MakoTimestamp, CommitRecordPreflight) -> Option<CommitRecordTarget>,
    {
        // SAFETY: this public method's contract is exactly the common helper's
        // target/lifetime contract; the ordinary spelling retains the native
        // database-wide validation ticket.
        unsafe {
            self.commit_trusted_unchecked_one_put_record_target_impl::<false, F>(candidate, acquire)
        }
    }

    /// Commit one verified one-Put while native assigns the cache sequence.
    ///
    /// One packed native CAS pairs Mako timestamp allocation with the dense
    /// sequence after restricted validation. `acquire` receives that exact
    /// sequence and lends its target for serialization-before-install.
    ///
    /// # Safety
    ///
    /// Every target and transaction requirement of
    /// [`Self::commit_trusted_unchecked_one_put_record_target`] applies.
    /// `next_bound` and `unhealthy` must belong to the one write-back queue
    /// used by every cache-record terminal for this Concurrent-claimed LocalDb
    /// and must remain alive through this synchronous call. `next_bound` is a
    /// retained ABI field which this concurrent terminal validates but does
    /// not use for allocation or descriptor discovery.
    /// Whenever the returned outcome exposes an accepted order, the caller
    /// must adopt and publish or pin that exact sequence, including when
    /// `acquire` was not reached.
    #[doc(hidden)]
    pub unsafe fn commit_trusted_native_ordered_unchecked_one_put_record_target<F>(
        mut self,
        candidate: CommitRecordPreflight,
        next_bound: &AtomicU64,
        unhealthy: &AtomicBool,
        acquire: F,
    ) -> TrustedNativeOrderedOnePutRecordOutcome
    where
        F: FnOnce(MakoTimestamp, CommitRecordPreflight, NonZeroU64) -> Option<CommitRecordTarget>,
    {
        debug_assert!(self.fast_bound_table.is_some());
        debug_assert!(self.record_preflight.is_none());
        debug_assert!(self.active);
        debug_assert!(self.raw.is_some());
        debug_assert!(!candidate.is_empty());
        debug_assert_eq!(candidate.op_count, 1);
        debug_assert_eq!(candidate.checksum, CommitRecordChecksum::None);
        debug_assert_ne!(candidate.exact_record_bytes, 0);
        debug_assert!(candidate.exact_record_bytes <= u32::MAX as usize);
        debug_assert_eq!(
            NonZeroU32::new(candidate.exact_record_bytes as u32),
            self.unchecked_one_put_record_bytes
        );

        // SAFETY: this trusted terminal consumes the live thread-affine handle
        // on every outcome. The method contract keeps both atomic words and
        // any callback target valid for the whole synchronous call.
        let raw = unsafe { self.raw.take().unwrap_unchecked() };
        self.active = false;
        let mut state = NativeOrderedRecordTargetBindHook {
            hook: Some(acquire),
            preflight: candidate,
            bound: false,
        };
        let mut ordered_sequence = 0u64;
        let mut ordered_timestamp = 0u32;
        let mut record_written = 0u8;
        // SAFETY: all raw addresses point to naturally aligned, live Rust
        // atomic/stack storage, and the callback state remains pinned here
        // until native has returned.
        let packed = unsafe {
            fast_abi::mako_rust_fast_txn_commit_native_ordered_unchecked_one_put_record_and_destroy(
                raw.as_ptr(),
                candidate.exact_record_bytes as u32,
                next_bound.as_ptr(),
                unhealthy.as_ptr().cast::<u8>(),
                Some(native_ordered_one_put_record_target_bind_trampoline::<F>),
                std::ptr::from_mut(&mut state).cast::<c_void>(),
                &mut ordered_sequence,
                &mut ordered_timestamp,
                &mut record_written,
            )
        };
        debug_assert!(!state.bound || ordered_sequence != 0);
        TrustedNativeOrderedOnePutRecordOutcome {
            inner: TrustedUncheckedOnePutRecordOutcome {
                packed,
                record_bound: ordered_sequence != 0,
                record_written,
            },
            ordered_sequence,
            ordered_timestamp,
            target_bound: state.bound,
        }
    }

    /// Commit one verified one-Put directly into the native record arena.
    ///
    /// Native uses the restricted pair CAS when the write lock covers final
    /// validation. Insert/predicate fallback instead assigns under the packed
    /// general-certification bit. It then binds the exact publication
    /// generation and serializes into the matching arena block without a Rust
    /// callback. A returned nonzero order already owns a BOUND cell.
    ///
    /// # Safety
    ///
    /// `self` must be an active trusted-bound transaction, and `candidate`
    /// must be its exact current
    /// [`Self::unchecked_one_put_record_candidate`] after the final operation.
    /// `control` must satisfy
    /// [`TrustedNativeOrderedArenaControl::from_raw_parts`] for this
    /// transaction's LocalDb and the one queue used by all of its concurrent
    /// cache-record terminals. The caller must already own one capacity claim
    /// which covers native's next dense sequence and exact arena generation.
    ///
    /// Whenever the result exposes an accepted order, the caller must adopt
    /// the cell that native already bound and publish or pin it, even when the
    /// terminal reports cleanup uncertainty or an unwritten record.
    #[doc(hidden)]
    #[inline(always)]
    pub unsafe fn commit_trusted_native_ordered_unchecked_one_put_arena(
        mut self,
        candidate: CommitRecordPreflight,
        control: &TrustedNativeOrderedArenaControl,
    ) -> TrustedNativeOrderedOnePutRecordOutcome {
        debug_assert!(self.fast_bound_table.is_some());
        debug_assert!(self.record_preflight.is_none());
        debug_assert!(self.active);
        debug_assert!(self.raw.is_some());
        debug_assert!(!candidate.is_empty());
        debug_assert_eq!(candidate.op_count, 1);
        debug_assert_eq!(candidate.checksum, CommitRecordChecksum::None);
        debug_assert_ne!(candidate.exact_record_bytes, 0);
        debug_assert!(candidate.exact_record_bytes <= u32::MAX as usize);
        debug_assert_eq!(
            NonZeroU32::new(candidate.exact_record_bytes as u32),
            self.unchecked_one_put_record_bytes
        );

        // SAFETY: the trusted terminal consumes the live thread-affine handle
        // on every outcome. The method contract keeps the full queue layout
        // and its capacity claim valid through native's synchronous call.
        let raw = unsafe { self.raw.take().unwrap_unchecked() };
        self.active = false;
        let result = unsafe {
            fast_abi::mako_rust_fast_txn_commit_native_ordered_unchecked_one_put_arena_and_destroy(
                raw.as_ptr(),
                candidate.exact_record_bytes as u32,
                std::ptr::from_ref(control),
            )
        };
        let ordered_timestamp = result.record_state as u32;
        let record_written = if result.record_state >> 33 == 0 {
            ((result.record_state >> 32) & 1) as u8
        } else {
            // Preserve malformed reserved bits for the existing cold decoder.
            // A value above one can never satisfy its completion contract.
            u8::MAX
        };
        let target_bound = result.ordered_sequence != 0;
        TrustedNativeOrderedOnePutRecordOutcome {
            inner: TrustedUncheckedOnePutRecordOutcome {
                packed: result.terminal,
                record_bound: target_bound,
                record_written,
            },
            ordered_sequence: result.ordered_sequence,
            ordered_timestamp,
            target_bound,
        }
    }

    /// Commit one verified one-Put candidate under external single-producer
    /// exclusion.
    ///
    /// This has the same outcome and target contract as
    /// [`Self::commit_trusted_unchecked_one_put_record_target`], but calls the
    /// private native terminal which omits the database validation-ticket RMW.
    /// Silo still locks the write set, allocates the Mako timestamp, repeats
    /// ordered predicate validation, validates point reads, serializes the
    /// record, and only then installs the write.
    ///
    /// # Safety
    ///
    /// Every safety requirement of
    /// [`Self::commit_trusted_unchecked_one_put_record_target`] applies. In
    /// addition, from before this call until it returns, the caller must prove
    /// that no other cache-record terminal for this transaction's `LocalDb` is
    /// running or waiting, including an ordinary ticketed terminal. Violating
    /// that exclusion can make timestamp and cache-sequence order diverge.
    #[doc(hidden)]
    pub unsafe fn commit_trusted_single_producer_unchecked_one_put_record_target<F>(
        self,
        candidate: CommitRecordPreflight,
        acquire: F,
    ) -> TrustedUncheckedOnePutRecordOutcome
    where
        F: FnOnce(MakoTimestamp, CommitRecordPreflight) -> Option<CommitRecordTarget>,
    {
        // SAFETY: delegated unchanged to this method's stronger contract.
        unsafe {
            self.commit_trusted_unchecked_one_put_record_target_impl::<true, F>(candidate, acquire)
        }
    }

    /// Commit one verified one-Put candidate into an already-selected target.
    ///
    /// This callback-free terminal is the narrowest cache-only spelling. The
    /// caller selects the dense sequence and exact record storage before native
    /// validation. Native independently rederives the candidate, acquires the
    /// write set, assigns the Mako timestamp, performs final validation,
    /// serializes into `target`, and only then installs the write. The returned
    /// [`TrustedPreselectedUncheckedOnePutRecordOutcome::accepted_timestamp`]
    /// is the sole indication that native accepted the preselected target into
    /// that commit order.
    ///
    /// # Safety
    ///
    /// `self` must be an active trusted-bound transaction, and `candidate` must
    /// be its exact current [`Self::unchecked_one_put_record_candidate`] after
    /// the final operation. `target` must have a nonzero sequence, its extent
    /// must equal `candidate.exact_record_bytes()`, and its allocation must
    /// remain stable, exclusively writable, and non-aliasing with transaction
    /// key/value storage throughout this synchronous call.
    ///
    /// From before this call until it returns, the caller must also prove that
    /// no other cache-record terminal for this transaction's `LocalDb` is
    /// running or waiting, including any ordinary ticketed terminal. This call
    /// does not provide mutual exclusion itself. The preselected target must
    /// remain invisible before the call. On return, the caller must bind it
    /// unconditionally if `accepted_timestamp()` is `Some`; it may publish the
    /// initialized bytes only when `is_committed()` is true. Every other
    /// accepted outcome must be decoded and pinned under the cache's fail-stop
    /// protocol. A target with no accepted timestamp may be reused only after a
    /// valid definite pre-acceptance abort has been decoded.
    #[doc(hidden)]
    #[inline(always)]
    pub unsafe fn commit_trusted_preselected_single_producer_unchecked_one_put_record_target(
        mut self,
        candidate: CommitRecordPreflight,
        target: CommitRecordTarget,
    ) -> TrustedPreselectedUncheckedOnePutRecordOutcome {
        debug_assert!(self.fast_bound_table.is_some());
        debug_assert!(self.record_preflight.is_none());
        debug_assert!(self.active);
        debug_assert!(self.raw.is_some());
        debug_assert!(!candidate.is_empty());
        debug_assert_eq!(candidate.op_count, 1);
        debug_assert_eq!(candidate.checksum, CommitRecordChecksum::None);
        debug_assert_ne!(candidate.exact_record_bytes, 0);
        debug_assert!(candidate.exact_record_bytes <= u32::MAX as usize);
        debug_assert_eq!(target.exact_record_bytes, candidate.exact_record_bytes);
        debug_assert_eq!(
            NonZeroU32::new(candidate.exact_record_bytes as u32),
            self.unchecked_one_put_record_bytes
        );

        // SAFETY: the trusted cache contract guarantees a live native handle;
        // consuming it here prevents every later facade use. The same contract
        // keeps the preselected target stable and uniquely writable.
        let raw = unsafe { self.raw.take().unwrap_unchecked() };
        self.active = false;
        // SAFETY: the active handle is consumed on every outcome. Native
        // revalidates the direct one-Put witness and exact extent before using
        // the caller-provided nonzero sequence and writable target.
        let result = unsafe {
            fast_abi::mako_rust_fast_txn_commit_preselected_unchecked_one_put_record_single_producer_and_destroy(
                raw.as_ptr(),
                candidate.exact_record_bytes as u32,
                target.sequence.get(),
                target.bytes.as_ptr(),
                target.exact_record_bytes,
            )
        };
        TrustedPreselectedUncheckedOnePutRecordOutcome {
            terminal: result.terminal,
            record_state: result.record_state,
        }
    }

    /// Commit one verified one-Put candidate into a preselected native holder.
    ///
    /// Unlike the record-target terminal, this path does not encode or copy a
    /// cache record on the foreground thread. Native installs the transaction
    /// first, then transfers its stable staged value allocation and key into
    /// the exact pool generation. A background consumer later borrows that
    /// holder and constructs the RocksDB log record.
    ///
    /// # Safety
    ///
    /// `self` must be an active trusted-bound transaction, and `candidate`
    /// must be its exact current [`Self::unchecked_one_put_record_candidate`]
    /// after the final operation. `sequence` must be the unique producer's
    /// retained next dense sequence, and its `(sequence - 1) % pool.capacity()`
    /// generation must be FREE. The caller must retain the pool and unique
    /// single-producer lease through this call.
    ///
    /// No other cache-record or holder terminal for this transaction's
    /// `LocalDb` may run or wait during the call. A returned accepted timestamp
    /// transfers an unconditional obligation to publish or pin `sequence`,
    /// even when terminal visibility is unknown. A zero accepted timestamp
    /// permits reuse only after the complete result decodes as a valid
    /// pre-acceptance abort.
    #[doc(hidden)]
    #[inline(always)]
    pub unsafe fn commit_trusted_preselected_single_producer_unchecked_one_put_holder(
        self,
        candidate: CommitRecordPreflight,
        pool: &TrustedOnePutHolderPool,
        sequence: NonZeroU64,
    ) -> TrustedPreselectedUncheckedOnePutHolderOutcome {
        debug_assert!(self.fast_bound_table.is_some());
        debug_assert!(self.record_preflight.is_none());
        debug_assert!(self.active);
        debug_assert!(self.raw.is_some());
        debug_assert!(!candidate.is_empty());
        debug_assert_eq!(candidate.op_count, 1);
        debug_assert_eq!(candidate.checksum, CommitRecordChecksum::None);
        debug_assert_ne!(candidate.exact_record_bytes, 0);
        debug_assert!(candidate.exact_record_bytes <= u32::MAX as usize);
        debug_assert_eq!(
            NonZeroU32::new(candidate.exact_record_bytes as u32),
            self.unchecked_one_put_record_bytes
        );

        // SAFETY: this method's contract proves the same ownership and
        // generation invariants required by the compact primitive below.
        unsafe {
            self.commit_trusted_preselected_single_producer_unchecked_one_put_holder_bytes(
                NonZeroU32::new_unchecked(candidate.exact_record_bytes as u32),
                pool,
                sequence,
            )
        }
    }

    /// Attempt the fused native one-Put holder terminal without first
    /// materializing a Rust candidate or reserving a queue generation.
    ///
    /// Native rederives the retained direct one-Put witness, checks the queue
    /// health/capacity snapshot, commits into the future holder, and on exact
    /// success Release-publishes ACK as the canonical SPSC tail. Rust only
    /// synchronizes the producer-local cursor when decoding a cold result. A
    /// cold capacity result leaves this transaction active so the caller can
    /// refresh and retry; a general result likewise permits the ordinary safe
    /// terminal.
    ///
    /// # Safety
    ///
    /// `self` must be the active trusted-bound transaction owned by the unique
    /// producer represented by `control`. `producer_next` and
    /// `capacity_limit` must point to live, naturally aligned `AtomicU64`
    /// storage owned by that producer for the whole synchronous call. Rust
    /// loads the limit before entering native and may update `producer_next`
    /// while decoding a cold result. Those two words and the control's
    /// `acknowledged` word must be distinct storage. The limit must equal
    /// `applied_frontier.saturating_add(control.capacity)` for the same queue.
    /// A stale snapshot may lag the current applied frontier, but it must never
    /// exceed the limit derived from that current frontier. The control's pool
    /// and queue pointers must satisfy
    /// [`TrustedSpscOnePutHolderControl::new`]. No other cache-record or holder
    /// terminal for this `LocalDb` may run or wait during the call.
    ///
    /// The caller must inspect the returned ownership state exactly once. It
    /// may continue using this transaction only for an explicit untouched
    /// result. Every consumed outcome carries the same publish-or-pin
    /// obligations as the preselected terminal above.
    #[doc(hidden)]
    #[inline(always)]
    pub unsafe fn try_commit_trusted_fused_single_producer_one_put_holder(
        &mut self,
        control: &TrustedSpscOnePutHolderControl,
        producer_next: *mut u64,
        capacity_limit: *const u64,
    ) -> TrustedFusedOnePutHolderAttempt {
        let mut cold_attempt = MaybeUninit::uninit();
        // SAFETY: this method has exactly the fast primitive's contract. The
        // false result initializes `cold_attempt` before it is read.
        if unsafe {
            self.try_commit_trusted_fused_single_producer_one_put_holder_fast(
                control,
                producer_next,
                capacity_limit,
                &mut cold_attempt,
            )
        } {
            TrustedFusedOnePutHolderAttempt::Published
        } else {
            // SAFETY: guaranteed by the false result above.
            unsafe { cold_attempt.assume_init() }
        }
    }

    /// Success-biased form of the fused SPSC terminal.
    ///
    /// `true` is the exact ordinary success: native consumed the transaction
    /// and Release-published its ACK. `false` initializes `cold_attempt` with
    /// every other lifecycle result. Keeping the rich decoder out of line
    /// leaves the production acknowledgement path as one native call, one
    /// zero test, and facade-ownership retirement.
    ///
    /// # Safety
    ///
    /// This has the same safety contract as
    /// [`Self::try_commit_trusted_fused_single_producer_one_put_holder`]. The
    /// caller must read `cold_attempt` exactly once after a false result and
    /// must not read it after a true result.
    #[doc(hidden)]
    #[inline(always)]
    pub unsafe fn try_commit_trusted_fused_single_producer_one_put_holder_fast(
        &mut self,
        control: &TrustedSpscOnePutHolderControl,
        producer_next: *mut u64,
        capacity_limit: *const u64,
        cold_attempt: &mut MaybeUninit<TrustedFusedOnePutHolderAttempt>,
    ) -> bool {
        // SAFETY: this wrapper preserves the public fast method's contract and
        // retires its facade before reporting a published result.
        unsafe {
            self.try_commit_trusted_fused_single_producer_one_put_holder_impl::<true>(
                control,
                producer_next,
                capacity_limit,
                cold_attempt,
            )
        }
    }

    /// Exact-success spelling for an outer consuming facade.
    ///
    /// Unlike the ordinary fast method, `true` deliberately leaves this Rust
    /// facade's scalar ownership fields unchanged after native destroys the
    /// raw handle. The caller must immediately forget the enclosing facade and
    /// must never read, drop, or otherwise use `self` again. `false` retains
    /// the ordinary decoded ownership state and permits normal Drop.
    ///
    /// # Safety
    ///
    /// This has every requirement of
    /// [`Self::try_commit_trusted_fused_single_producer_one_put_holder_fast`].
    /// In addition, a `true` return creates the immediate-forget obligation
    /// described above.
    #[doc(hidden)]
    #[inline(always)]
    pub unsafe fn try_commit_trusted_fused_single_producer_one_put_holder_fast_forget_on_publish(
        &mut self,
        control: &TrustedSpscOnePutHolderControl,
        producer_next: *mut u64,
        capacity_limit: *const u64,
        cold_attempt: &mut MaybeUninit<TrustedFusedOnePutHolderAttempt>,
    ) -> bool {
        // SAFETY: delegated unchanged to this method's stronger contract.
        unsafe {
            self.try_commit_trusted_fused_single_producer_one_put_holder_impl::<false>(
                control,
                producer_next,
                capacity_limit,
                cold_attempt,
            )
        }
    }

    #[inline(always)]
    unsafe fn try_commit_trusted_fused_single_producer_one_put_holder_impl<
        const RETIRE_PUBLISHED: bool,
    >(
        &mut self,
        control: &TrustedSpscOnePutHolderControl,
        producer_next: *mut u64,
        capacity_limit: *const u64,
        cold_attempt: &mut MaybeUninit<TrustedFusedOnePutHolderAttempt>,
    ) -> bool {
        const CONSUMED_PUBLISHED: u64 = 0;

        debug_assert!(self.fast_bound_table.is_some());
        debug_assert!(self.record_preflight.is_none());
        debug_assert!(self.active);
        debug_assert!(self.raw.is_some());
        debug_assert!(!producer_next.is_null());
        debug_assert!(!capacity_limit.is_null());
        debug_assert_ne!(producer_next.cast_const(), capacity_limit);
        debug_assert_ne!(producer_next, control.raw.acknowledged);
        debug_assert_ne!(capacity_limit, control.raw.acknowledged.cast_const());

        // SAFETY: the caller promises that `capacity_limit` points to live,
        // naturally aligned AtomicU64 storage for this whole call.
        let capacity_limit_value =
            unsafe { AtomicU64::from_ptr(capacity_limit.cast_mut()).load(Ordering::Relaxed) };
        // SAFETY: the caller supplies the transaction, unique-producer,
        // stable-pointer, pool-generation, and whole-call exclusion proofs.
        let raw_result = unsafe {
            fast_abi::mako_rust_fast_txn_try_commit_fused_one_put_holder_single_producer_and_destroy(
                self.raw.unwrap_unchecked().as_ptr(),
                control.raw.acknowledged,
                control.raw.unhealthy,
                std::ptr::from_ref(&control.raw).cast_mut(),
                capacity_limit_value,
            )
        };

        if raw_result == CONSUMED_PUBLISHED {
            // Exact zero is the only ordinary result. Native consumed the raw
            // handle before publishing. The cache-only false specialization
            // transfers facade retirement to its caller's immediate forget.
            if RETIRE_PUBLISHED {
                let _ = unsafe { self.raw.take().unwrap_unchecked() };
                self.active = false;
            }
            return true;
        }

        // SAFETY: every nonzero word is decoded out of line; that decoder
        // alone knows whether native left the raw transaction untouched.
        cold_attempt.write(unsafe {
            self.decode_fused_holder_attempt_cold(raw_result, control, producer_next)
        });
        false
    }

    #[cold]
    #[inline(never)]
    unsafe fn decode_fused_holder_attempt_cold(
        &mut self,
        raw_result: u64,
        control: &TrustedSpscOnePutHolderControl,
        producer_next: *mut u64,
    ) -> TrustedFusedOnePutHolderAttempt {
        const CONSUMED_PUBLISHED: u32 = 0;
        const UNTOUCHED_NEED_GENERAL: u32 = 1;
        const UNTOUCHED_NEED_SLOW: u32 = 2;
        const CONSUMED_COMMITTED_UNPUBLISHED: u32 = 3;
        const CONSUMED_OUTCOME: u32 = 4;
        const HOLDER_STATE_MASK: u64 = (1u64 << 33) - 1;
        let code = raw_result as u32;
        let payload = (raw_result >> 32) as u32;
        // SAFETY: the fused terminal contract keeps these naturally aligned
        // atomic words live through this cold decode. Native alone writes ACK;
        // this unique producer alone writes its local cursor.
        let acknowledged = unsafe { AtomicU64::from_ptr(control.raw.acknowledged) };
        let unhealthy =
            unsafe { AtomicBool::from_ptr(control.raw.unhealthy.cast_mut().cast::<bool>()) };
        let producer_next = unsafe { AtomicU64::from_ptr(producer_next) };

        // These are the only ABI codes that guarantee the raw transaction was
        // not consumed. Even corrupt payload metadata cannot change that
        // ownership fact, so leave the facade live for fail-stop handling.
        if code == UNTOUCHED_NEED_GENERAL {
            if !unhealthy.load(Ordering::Acquire) {
                producer_next.store(acknowledged.load(Ordering::Relaxed), Ordering::Relaxed);
            }
            return if payload == 0 {
                TrustedFusedOnePutHolderAttempt::UntouchedGeneral
            } else {
                TrustedFusedOnePutHolderAttempt::UntouchedMalformed
            };
        }
        if code == UNTOUCHED_NEED_SLOW {
            if !unhealthy.load(Ordering::Acquire) {
                producer_next.store(acknowledged.load(Ordering::Relaxed), Ordering::Relaxed);
            }
            return match NonZeroU32::new(payload) {
                Some(exact_record_bytes) => {
                    TrustedFusedOnePutHolderAttempt::UntouchedSlow { exact_record_bytes }
                }
                None => TrustedFusedOnePutHolderAttempt::UntouchedMalformed,
            };
        }

        // Any other code is conservatively consuming, including an unknown or
        // malformed code. Clear Rust ownership before inspecting metadata so a
        // future decoder panic or Drop can never touch freed native storage.
        let _ = unsafe { self.raw.take().unwrap_unchecked() };
        self.active = false;

        match code {
            // Exact published success was consumed by the caller before this
            // cold decoder. Code zero with a nonzero payload is malformed.
            CONSUMED_PUBLISHED => TrustedFusedOnePutHolderAttempt::ConsumedMalformed,
            CONSUMED_COMMITTED_UNPUBLISHED => {
                let Some(sequence) = acknowledged.load(Ordering::Relaxed).checked_add(1) else {
                    return TrustedFusedOnePutHolderAttempt::ConsumedMalformed;
                };
                producer_next.store(sequence, Ordering::Relaxed);
                let Some(timestamp) = MakoTimestamp::new(payload) else {
                    return TrustedFusedOnePutHolderAttempt::ConsumedMalformed;
                };
                // SAFETY: native initializes cold_out for both consumed cold
                // codes. Code 3 was established above.
                let cold_out = unsafe { *control.raw.cold_out.get() };
                let Some(exact_record_bytes) =
                    NonZeroU32::new((cold_out.record_state >> 33) as u32)
                else {
                    return TrustedFusedOnePutHolderAttempt::ConsumedMalformed;
                };
                let outcome = TrustedPreselectedUncheckedOnePutHolderOutcome {
                    terminal: cold_out.terminal,
                    holder_state: cold_out.record_state & HOLDER_STATE_MASK,
                };
                if !outcome.is_committed() || outcome.accepted_timestamp() != Some(timestamp) {
                    return TrustedFusedOnePutHolderAttempt::ConsumedMalformed;
                }
                TrustedFusedOnePutHolderAttempt::CommittedUnpublished {
                    timestamp,
                    exact_record_bytes,
                }
            }
            CONSUMED_OUTCOME if payload == 0 => {
                producer_next.store(acknowledged.load(Ordering::Relaxed), Ordering::Relaxed);
                // SAFETY: the frozen ABI initializes `cold_out` for code 4 and
                // code 3. The exact code was established above.
                let cold_out = unsafe { *control.raw.cold_out.get() };
                let Some(exact_record_bytes) =
                    NonZeroU32::new((cold_out.record_state >> 33) as u32)
                else {
                    return TrustedFusedOnePutHolderAttempt::ConsumedMalformed;
                };
                TrustedFusedOnePutHolderAttempt::ConsumedOutcome {
                    outcome: TrustedPreselectedUncheckedOnePutHolderOutcome {
                        terminal: cold_out.terminal,
                        holder_state: cold_out.record_state & HOLDER_STATE_MASK,
                    },
                    exact_record_bytes,
                }
            }
            _ => TrustedFusedOnePutHolderAttempt::ConsumedMalformed,
        }
    }

    /// Compact form of the preselected one-Put holder terminal.
    ///
    /// This is identical to
    /// [`Self::commit_trusted_preselected_single_producer_unchecked_one_put_holder`]
    /// except that a cache which already obtained the build-private compact
    /// extent need not expand it into a [`CommitRecordPreflight`] and pass that
    /// larger value back through Rust. All exact-shape validation remains in
    /// the native consuming terminal.
    ///
    /// # Safety
    ///
    /// `self` must be the active trusted-bound transaction from which
    /// `exact_record_bytes` was obtained, with no intervening operation. The
    /// pool, sequence, unique-producer, and publish-or-pin obligations are the
    /// same as the full-plan terminal above.
    #[doc(hidden)]
    #[inline(always)]
    pub unsafe fn commit_trusted_preselected_single_producer_unchecked_one_put_holder_bytes(
        mut self,
        exact_record_bytes: NonZeroU32,
        pool: &TrustedOnePutHolderPool,
        sequence: NonZeroU64,
    ) -> TrustedPreselectedUncheckedOnePutHolderOutcome {
        debug_assert!(self.fast_bound_table.is_some());
        debug_assert!(self.record_preflight.is_none());
        debug_assert!(self.active);
        debug_assert!(self.raw.is_some());
        debug_assert_eq!(
            Some(exact_record_bytes),
            self.unchecked_one_put_record_bytes
        );

        // SAFETY: the trusted caller guarantees a live native handle and
        // consumes it here on every terminal outcome.
        let raw = unsafe { self.raw.take().unwrap_unchecked() };
        self.active = false;
        // SAFETY: the method contract proves exact candidate, holder
        // generation, whole-call exclusion, and pool lifetime. Native
        // independently revalidates the one-Put shape before lock acquisition.
        let result = unsafe {
            fast_abi::mako_rust_fast_txn_commit_preselected_unchecked_one_put_holder_single_producer_and_destroy(
                raw.as_ptr(),
                exact_record_bytes.get(),
                pool.raw.as_ptr(),
                sequence.get(),
            )
        };
        TrustedPreselectedUncheckedOnePutHolderOutcome {
            terminal: result.terminal,
            holder_state: result.record_state,
        }
    }

    #[inline(always)]
    unsafe fn commit_trusted_unchecked_one_put_record_target_impl<const SINGLE_PRODUCER: bool, F>(
        mut self,
        candidate: CommitRecordPreflight,
        acquire: F,
    ) -> TrustedUncheckedOnePutRecordOutcome
    where
        F: FnOnce(MakoTimestamp, CommitRecordPreflight) -> Option<CommitRecordTarget>,
    {
        debug_assert!(self.fast_bound_table.is_some());
        debug_assert!(self.record_preflight.is_none());
        debug_assert!(self.active);
        debug_assert!(self.raw.is_some());
        debug_assert!(!candidate.is_empty());
        debug_assert_eq!(candidate.op_count, 1);
        debug_assert_eq!(candidate.checksum, CommitRecordChecksum::None);
        debug_assert_ne!(candidate.exact_record_bytes, 0);
        debug_assert!(candidate.exact_record_bytes <= u32::MAX as usize);
        debug_assert_eq!(
            NonZeroU32::new(candidate.exact_record_bytes as u32),
            self.unchecked_one_put_record_bytes
        );

        // SAFETY: the trusted cache contract above guarantees a live native
        // handle; consuming it here prevents every later facade use.
        let raw = unsafe { self.raw.take().unwrap_unchecked() };
        self.active = false;
        let mut state = RecordTargetBindHook {
            hook: Some(acquire),
            preflight: candidate,
            bound: false,
        };
        let mut record_written = 0u8;
        // SAFETY: this active fast-bound handle is consumed on every outcome.
        // Native independently revalidates `expected_record_bytes` before
        // locking. The method contract keeps callback state and any returned
        // storage stable, exclusively writable, and non-unwinding throughout.
        let packed = if SINGLE_PRODUCER {
            // SAFETY: the caller additionally proves whole-call exclusion for
            // this LocalDb, while the common method contract covers the raw
            // handle, callback state, and target storage.
            unsafe {
                fast_abi::mako_rust_fast_txn_commit_unchecked_one_put_record_single_producer_and_destroy(
                    raw.as_ptr(),
                    candidate.exact_record_bytes as u32,
                    Some(unchecked_one_put_record_target_bind_trampoline::<F>),
                    std::ptr::from_mut(&mut state).cast::<c_void>(),
                    &mut record_written,
                )
            }
        } else {
            // SAFETY: this active fast-bound handle is consumed on every
            // outcome and the method contract covers the callback storage.
            unsafe {
                fast_abi::mako_rust_fast_txn_commit_unchecked_one_put_record_and_destroy(
                    raw.as_ptr(),
                    candidate.exact_record_bytes as u32,
                    Some(unchecked_one_put_record_target_bind_trampoline::<F>),
                    std::ptr::from_mut(&mut state).cast::<c_void>(),
                    &mut record_written,
                )
            }
        };
        TrustedUncheckedOnePutRecordOutcome {
            packed,
            record_bound: state.bound,
            record_written,
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

struct RecordTargetBindHook<F> {
    hook: Option<F>,
    preflight: CommitRecordPreflight,
    bound: bool,
}

struct NativeOrderedRecordTargetBindHook<F> {
    hook: Option<F>,
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

#[allow(clippy::too_many_arguments)]
unsafe extern "C" fn record_target_bind_trampoline<F>(
    context: *mut c_void,
    raw_timestamp: u32,
    exact_record_bytes: usize,
    sequence_out: *mut u64,
    record_bytes_out: *mut *mut u8,
    record_capacity_out: *mut usize,
) -> i32
where
    F: FnOnce(MakoTimestamp, CommitRecordPreflight) -> Option<CommitRecordTarget>,
{
    // Fail closed and leave deterministic outputs even if native violates its
    // private callback contract.
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

    // SAFETY: commit_report_with_record_target passes this exact stack value;
    // native invokes the callback synchronously at most once.
    let state = unsafe { &mut *context.cast::<RecordTargetBindHook<F>>() };
    let Some(timestamp) = MakoTimestamp::new(raw_timestamp) else {
        return 0;
    };
    if exact_record_bytes != state.preflight.exact_record_bytes {
        return 0;
    }
    let Some(hook) = state.hook.take() else {
        return 0;
    };
    // The unsafe terminal contract requires a non-unwinding callback. Avoiding
    // catch_unwind is intentional on this production hot path.
    let Some(target) = hook(timestamp, state.preflight) else {
        return 0;
    };
    if target.exact_record_bytes != exact_record_bytes {
        return 0;
    }

    // Nothing below is fallible. The target constructor and terminal contract
    // guarantee the pointer remains valid and uniquely writable through the
    // native serialization which follows this callback.
    // SAFETY: required outputs were checked above.
    unsafe {
        sequence_out.write(target.sequence.get());
        record_bytes_out.write(target.bytes.as_ptr());
        record_capacity_out.write(target.exact_record_bytes);
    }
    state.bound = true;
    1
}

/// Trusted callback used only by the hidden fused one-Put terminal.
///
/// Unlike [`record_target_bind_trampoline`], this path relies on the native
/// terminal's already-validated callback contract and the unsafe Rust method's
/// exact-target precondition. Keeping the checked trampoline separate ensures
/// ordinary record targets continue to fail closed against malformed ABI
/// inputs while the fused path does not zero and revalidate six trusted
/// scalars on every successful commit.
#[allow(clippy::too_many_arguments)]
unsafe extern "C" fn unchecked_one_put_record_target_bind_trampoline<F>(
    context: *mut c_void,
    raw_timestamp: u32,
    exact_record_bytes: usize,
    sequence_out: *mut u64,
    record_bytes_out: *mut *mut u8,
    record_capacity_out: *mut usize,
) -> i32
where
    F: FnOnce(MakoTimestamp, CommitRecordPreflight) -> Option<CommitRecordTarget>,
{
    debug_assert!(!context.is_null());
    debug_assert!(!sequence_out.is_null());
    debug_assert!(!record_bytes_out.is_null());
    debug_assert!(!record_capacity_out.is_null());
    debug_assert!(MakoTimestamp::new(raw_timestamp).is_some());

    // SAFETY: the hidden native terminal receives this exact live stack state,
    // invokes the callback synchronously at most once, and supplies the
    // validated non-null context promised by this unsafe terminal's contract.
    let state = unsafe { &mut *context.cast::<RecordTargetBindHook<F>>() };
    debug_assert_eq!(exact_record_bytes, state.preflight.exact_record_bytes);

    // SAFETY: native allocates a nonzero in-range Mako timestamp before it can
    // invoke the fused bind hook. The debug assertion retains a diagnostic for
    // a mismatched development ABI without a production hot-path branch.
    let timestamp = MakoTimestamp(unsafe { NonZeroU32::new_unchecked(raw_timestamp) });
    // SAFETY: native invokes this fused callback at most once for the live
    // state, so the FnOnce value is present on its sole invocation.
    let hook = unsafe { state.hook.take().unwrap_unchecked() };
    let Some(target) = hook(timestamp, state.preflight) else {
        // Native ignores every output when the hook rejects. Its own zeroed
        // locals preserve deterministic diagnostics; no Rust-side stores are
        // necessary on this fail-closed path.
        return 0;
    };
    debug_assert_eq!(target.exact_record_bytes, exact_record_bytes);

    // SAFETY: the hidden terminal supplies writable non-null output pointers,
    // and the unsafe caller guarantees this target is the exact stable extent
    // reserved for the candidate through the synchronous native terminal.
    unsafe {
        sequence_out.write(target.sequence.get());
        record_bytes_out.write(target.bytes.as_ptr());
        record_capacity_out.write(target.exact_record_bytes);
    }
    state.bound = true;
    1
}

/// Trusted target binder for the native-assigned sequence path.
///
/// Native initializes `sequence_in_out` to the sequence paired with
/// `raw_timestamp` in packed state. The callback adopts that exact queue
/// generation and returns its stable arena target after pair assignment.
#[allow(clippy::too_many_arguments)]
unsafe extern "C" fn native_ordered_one_put_record_target_bind_trampoline<F>(
    context: *mut c_void,
    raw_timestamp: u32,
    exact_record_bytes: usize,
    sequence_in_out: *mut u64,
    record_bytes_out: *mut *mut u8,
    record_capacity_out: *mut usize,
) -> i32
where
    F: FnOnce(MakoTimestamp, CommitRecordPreflight, NonZeroU64) -> Option<CommitRecordTarget>,
{
    debug_assert!(!context.is_null());
    debug_assert!(!sequence_in_out.is_null());
    debug_assert!(!record_bytes_out.is_null());
    debug_assert!(!record_capacity_out.is_null());
    // SAFETY: the private terminal passes this exact live stack state and
    // invokes the callback synchronously at most once after retiring its gate.
    let state = unsafe { &mut *context.cast::<NativeOrderedRecordTargetBindHook<F>>() };
    let Some(timestamp) = MakoTimestamp::new(raw_timestamp) else {
        return 0;
    };
    // SAFETY: the hidden terminal supplies a readable, non-null in/out word.
    let raw_sequence = unsafe { sequence_in_out.read() };
    let Some(ordered_sequence) = NonZeroU64::new(raw_sequence) else {
        return 0;
    };
    if raw_sequence > u64::from(MAX_MAKO_TIMESTAMP)
        || exact_record_bytes != state.preflight.exact_record_bytes
    {
        return 0;
    }
    // SAFETY: this private callback is invoked once for the live state.
    let hook = unsafe { state.hook.take().unwrap_unchecked() };
    let Some(target) = hook(timestamp, state.preflight, ordered_sequence) else {
        return 0;
    };
    if target.sequence != ordered_sequence || target.exact_record_bytes != exact_record_bytes {
        // The callback may already have adopted the native-assigned dense
        // generation. Returning a retryable rejection would risk releasing
        // that obligation during unwinding, so terminate fail-closed.
        std::process::abort();
    }

    // SAFETY: the target contract guarantees writable output pointers and a
    // stable exact arena extent through native serialization.
    unsafe {
        sequence_in_out.write(target.sequence.get());
        record_bytes_out.write(target.bytes.as_ptr());
        record_capacity_out.write(target.exact_record_bytes);
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
    fn native_order_witness_rejects_sequence_outside_packed_domain() {
        let malformed = TrustedNativeOrderedOnePutRecordOutcome {
            inner: TrustedUncheckedOnePutRecordOutcome {
                packed: 0,
                record_bound: false,
                record_written: 0,
            },
            ordered_sequence: u64::from(MAX_MAKO_TIMESTAMP) + 1,
            ordered_timestamp: 1,
            target_bound: false,
        };

        assert!(!malformed.order_witness_valid());
        assert_eq!(malformed.accepted_order(), None);
        assert!(!malformed.into_report().completion_contract_valid);
    }

    #[test]
    fn native_ordered_binder_rejects_overrange_sequence_before_publication() {
        type Hook = fn(
            MakoTimestamp,
            CommitRecordPreflight,
            NonZeroU64,
        ) -> Option<CommitRecordTarget>;
        static CALLS: std::sync::atomic::AtomicUsize =
            std::sync::atomic::AtomicUsize::new(0);
        fn count_call(
            _: MakoTimestamp,
            _: CommitRecordPreflight,
            _: NonZeroU64,
        ) -> Option<CommitRecordTarget> {
            CALLS.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            None
        }

        CALLS.store(0, std::sync::atomic::Ordering::Relaxed);
        let preflight = CommitRecordPreflight {
            exact_record_bytes: 64,
            op_count: 1,
            checksum: CommitRecordChecksum::None,
        };
        let mut state = NativeOrderedRecordTargetBindHook {
            hook: Some(count_call as Hook),
            preflight,
            bound: false,
        };
        let mut sequence = u64::from(MAX_MAKO_TIMESTAMP) + 1;
        let mut record_bytes = std::ptr::null_mut();
        let mut record_capacity = 0;

        // SAFETY: all pointers refer to live test storage for this synchronous
        // call. The malformed sequence models a same-build native ABI defect.
        let accepted = unsafe {
            native_ordered_one_put_record_target_bind_trampoline::<Hook>(
                (&mut state as *mut NativeOrderedRecordTargetBindHook<Hook>).cast(),
                1,
                preflight.exact_record_bytes,
                &mut sequence,
                &mut record_bytes,
                &mut record_capacity,
            )
        };

        assert_eq!(accepted, 0);
        assert_eq!(CALLS.load(std::sync::atomic::Ordering::Relaxed), 0);
        assert!(state.hook.is_some());
        assert!(!state.bound);
        assert!(record_bytes.is_null());
        assert_eq!(record_capacity, 0);
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
