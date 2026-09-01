//! Transaction-ordered, asynchronous RocksDB write-back.
//!
//! Before entering native commit, STO preflights its canonical write set while
//! Rust claims bounded queue capacity. An oversized record also checks out and
//! grows recycled vector storage here; a common small record needs only its
//! exact length because its fixed 256-byte arena block is selected by the later
//! dense sequence. The transaction does not yet receive that cache sequence or
//! occupy an ordered slot. General transactions enter a per-database native
//! gate after locking their complete write set; the gate orders Mako timestamp
//! assignment, final validation, and the hook. A one-key update whose
//! sole observation is covered by that key's write lock validates first and
//! enters the gate only for timestamp and dense-sequence assignment. Native
//! then retires the ordering turn; Rust Acquires the assigned sequence's exact
//! ring generation and hands native a direct pointer to stable queue-owned
//! storage. STO serializes while retaining its write locks and before
//! installation.
//! Rust treats the bytes as initialized only after native returns the exact
//! completion witness. The hook-time operation is allocation-free and never
//! performs backend IO.
//!
//! A bound slot remains Prepared until native commit returns successfully and
//! Rust attaches the completed bytes. Publishing flips the slot Ready. General
//! transactions wait for the dense Ready prefix through their slot; the
//! trusted concurrent one-Put terminal may return as soon as its own exact
//! cell is Ready and advertises that caller-visible high-water mark. Explicit
//! barriers and the backend consumer still wait for a dense prefix. An
//! ambiguous post-bind outcome pins the slot, so the consumer can neither skip
//! it nor mistake it for an abort. The consumer decodes records off the
//! foreground path and replays a bounded contiguous Ready prefix in one atomic
//! backend batch.

use std::cell::UnsafeCell;
use std::collections::VecDeque;
use std::fmt;
use std::mem::{ManuallyDrop, MaybeUninit};
use std::num::{NonZeroU32, NonZeroU64};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr::NonNull;
use std::sync::atomic::{AtomicBool, AtomicU8, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Condvar, Mutex, MutexGuard};
use std::time::Duration;

use mako_local::{CommitRecordTarget, MakoTimestamp, TrustedOnePutHolderPool};
use mrx_core::{BlobError, Blobs};
#[cfg(test)]
use std::cell::RefCell;

#[cfg(test)]
use crate::record::PreparedCommitRecord;
use crate::record::{
    CommitSeq, DeferredOnePutRecord, LegacyCommitRecord, Mutation, NativeCommitRecord,
    QueuedCommitRecord, RecordError, RecycledNativeRecord,
};

#[cfg(test)]
thread_local! {
    /// Deterministic materialization failure injection for sequence-bounded
    /// consumer tests. Thread-local storage prevents parallel tests from
    /// perturbing one another or a background worker.
    static TEST_MATERIALIZATION_FAILURE: RefCell<Option<(CommitSeq, RecordError)>> =
        const { RefCell::new(None) };
}

/// Default maximum encoded transaction-record size (8 MiB).
pub const DEFAULT_MAX_RECORD_BYTES: usize = 8 * 1024 * 1024;

/// Bytes reserved per bounded queue slot for the common one-mutation record.
///
/// The fixed arena turns the overwhelmingly common small-record path into one
/// startup allocation and makes a producer burst allocation-free even when it
/// outruns RocksDB all the way to the configured queue capacity. Larger records
/// use a separately recycled growable buffer.
const NATIVE_RECORD_ARENA_BLOCK_BYTES: usize = 256;
const CACHE_LINE_BYTES: usize = 64;
/// Tag stored in `PublicationCell::arena_record_bytes` when the payload lives
/// in the native deferred holder pool rather than the byte arena.
const NATIVE_HOLDER_RECORD_TAG: usize = 1usize << (usize::BITS - 1);

#[inline(always)]
pub(crate) const fn native_holder_record_supported(exact_record_bytes: usize) -> bool {
    exact_record_bytes != 0 && exact_record_bytes < NATIVE_HOLDER_RECORD_TAG
}

#[inline(always)]
fn tagged_native_holder_extent(exact_record_bytes: usize) -> usize {
    assert!(native_holder_record_supported(exact_record_bytes));
    NATIVE_HOLDER_RECORD_TAG | exact_record_bytes
}
/// Maximum number of packed concurrent capacity claims acquired by one shared
/// occupancy RMW. The unused claims remain on their owner's private cache
/// line until that worker consumes them or a cold path reclaims them.
const PACKED_OCCUPANCY_CREDIT_BATCH_MAX: usize = 16;
/// Bound all idle packed credits to at most roughly one eighth of configured
/// capacity. Small queues retain no idle credits and preserve scalar claiming.
const PACKED_OCCUPANCY_CREDIT_HOARD_DIVISOR: usize = 8;
/// Short out-of-order publication gaps are normal when several native
/// transactions finish together. Keep those gaps in userspace instead of
/// immediately entering the mutex/condition-variable protocol.
const CONCURRENT_ACKNOWLEDGEMENT_SPINS: usize = 64;

fn packed_occupancy_credit_batch_size(capacity: usize) -> usize {
    let all_worker_share = mako_local::MAX_WORKERS
        .max(1)
        .saturating_mul(PACKED_OCCUPANCY_CREDIT_HOARD_DIVISOR);
    (capacity / all_worker_share.max(1)).clamp(1, PACKED_OCCUPANCY_CREDIT_BATCH_MAX)
}

/// Issue one write-intent cache hint on processors which advertise PRFCHW.
///
/// # Safety
///
/// `address` need only be a canonical address suitable for a non-faulting
/// prefetch hint; it is never dereferenced. The caller performs the runtime
/// feature check before reaching this instruction.
#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
#[inline(always)]
unsafe fn prefetch_write_unchecked(address: *const u8) {
    // SAFETY: required by this function's contract. PREFETCHW has no
    // architecturally visible memory access and preserves registers/flags.
    unsafe {
        core::arch::asm!(
            "prefetchw [{address}]",
            address = in(reg) address,
            options(nostack, preserves_flags, readonly)
        )
    };
}

/// Cache the architectural PRFCHW capability without putting CPUID in the
/// transaction hot path after the first call.
#[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
// `__cpuid` is unsafe on the Rust 1.91 MSRV but safe on newer toolchains. Keep
// the explicit blocks for the MSRV without making newer builds fail linting.
#[allow(unused_unsafe)]
#[inline]
fn prefetch_write_supported() -> bool {
    const UNKNOWN: u8 = 0;
    const NO: u8 = 1;
    const YES: u8 = 2;
    static SUPPORT: AtomicU8 = AtomicU8::new(UNKNOWN);

    match SUPPORT.load(Ordering::Relaxed) {
        NO => false,
        YES => true,
        _ => {
            #[cfg(target_arch = "x86")]
            use core::arch::x86::__cpuid;
            #[cfg(target_arch = "x86_64")]
            use core::arch::x86_64::__cpuid;

            // CPUID is available on Rust's supported x86 targets. The
            // extended-leaf maximum is checked before querying PRFCHW (ECX 8).
            // SAFETY: Rust's x86 targets which reach this implementation
            // support CPUID. The base extended leaf is always valid, and its
            // result gates the only subsequent extended-leaf query.
            let maximum = unsafe { __cpuid(0x8000_0000) }.eax;
            let extended_features = if maximum >= 0x8000_0001 {
                // SAFETY: the maximum-leaf result above proves this query is
                // implemented by the current processor.
                unsafe { __cpuid(0x8000_0001) }.ecx
            } else {
                0
            };
            let supported = (extended_features & (1 << 8)) != 0;
            SUPPORT.store(if supported { YES } else { NO }, Ordering::Relaxed);
            supported
        }
    }
}

/// In-memory progress of the ordered RocksDB consumer.
///
/// `sequence` is the dense, serialization-safe [`CommitSeq`] prefix and is
/// therefore the field that proves contiguity. `mako_timestamp` identifies the
/// record at exactly that frontier. Mako timestamps remain strictly increasing
/// for production cache records but may contain gaps from validation aborts or
/// unrelated native work, so the timestamp alone is not a dense log position.
/// Neither field claims that RocksDB has synced data to disk.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct AppliedWatermark {
    sequence: u64,
    mako_timestamp: Option<MakoTimestamp>,
}

impl AppliedWatermark {
    /// Reconstruct progress from the backend during cache open.
    pub(crate) const fn recovered(sequence: u64, mako_timestamp: Option<MakoTimestamp>) -> Self {
        Self {
            sequence,
            mako_timestamp,
        }
    }

    /// Highest contiguous cache sequence accepted by the backend.
    pub const fn sequence(self) -> u64 {
        self.sequence
    }

    /// Mako timestamp of the transaction at the applied sequence frontier.
    pub const fn mako_timestamp(self) -> Option<MakoTimestamp> {
        self.mako_timestamp
    }

    fn advance(&mut self, sequence: CommitSeq, mako_timestamp: MakoTimestamp) {
        debug_assert_eq!(
            sequence.get(),
            self.sequence + 1,
            "the applied watermark must advance one cache sequence at a time"
        );
        self.sequence = sequence.get();
        self.mako_timestamp = Some(mako_timestamp);
    }
}

/// Controls the bounded commit queue and synchronous drain retries.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct WritebackConfig {
    /// Maximum number of detached permits plus prepared or ready slots.
    pub capacity: usize,
    /// Maximum encoded size accepted by native record preflight.
    pub max_record_bytes: usize,
    /// Maximum number of consecutive Ready transactions submitted in one
    /// atomic backend batch.
    pub max_batch_records: usize,
    /// Approximate encoded-log byte budget for one backend batch. A front
    /// record larger than this limit is submitted alone.
    pub max_batch_bytes: usize,
    /// Extra attempts made by [`Writeback::wait_applied`] after a failed backend
    /// write. Zero means that the first failed attempt is returned.
    pub max_apply_retries: usize,
    /// Delay between backend retry attempts.
    pub retry_delay: Duration,
}

impl Default for WritebackConfig {
    fn default() -> Self {
        Self {
            capacity: 1_024,
            max_record_bytes: DEFAULT_MAX_RECORD_BYTES,
            max_batch_records: 64,
            max_batch_bytes: 1024 * 1024,
            max_apply_retries: 8,
            retry_delay: Duration::from_millis(1),
        }
    }
}

/// Invalid construction parameters for [`Writeback`].
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ConfigError {
    /// A zero-capacity queue can never admit a commit.
    ZeroCapacity,
    /// A zero-byte record budget cannot hold the record envelope.
    ZeroRecordBudget,
    /// A zero-record batch can never make progress.
    ZeroBatchRecords,
    /// A zero-byte batch can never make progress.
    ZeroBatchBytes,
    /// A zero retry delay would turn a persistent backend error into a spin
    /// loop.
    ZeroRetryDelay,
    /// The applied seed leaves no sequence number for a future bind.
    SequenceExhausted,
    /// Queue capacity cannot be represented by the fixed native-record arena.
    NativeRecordArenaTooLarge,
    /// The native queue-owned one-Put holder ring could not be created.
    NativeHolderPool,
}

impl fmt::Display for ConfigError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::ZeroCapacity => write!(f, "write-back capacity must be nonzero"),
            Self::ZeroRecordBudget => write!(f, "transaction record budget must be nonzero"),
            Self::ZeroBatchRecords => write!(f, "write-back batch record limit must be nonzero"),
            Self::ZeroBatchBytes => write!(f, "write-back batch byte limit must be nonzero"),
            Self::ZeroRetryDelay => write!(f, "write-back retry delay must be nonzero"),
            Self::SequenceExhausted => write!(f, "commit sequence space is exhausted"),
            Self::NativeRecordArenaTooLarge => {
                write!(f, "native record arena size exceeds addressable memory")
            }
            Self::NativeHolderPool => write!(f, "could not create native one-Put holder pool"),
        }
    }
}

impl std::error::Error for ConfigError {}

/// Failure to prepare a complete transaction record and claim queue capacity.
#[derive(Debug)]
pub enum ReserveError {
    /// The transaction could not be represented within the configured record
    /// limit.
    Record(RecordError),
    /// No nonzero commit sequence remains.
    SequenceExhausted,
    /// An earlier native commit has an ambiguous outcome. New commits are
    /// rejected because they could never safely pass that slot.
    UnknownOutcome {
        /// The first pinned sequence.
        sequence: CommitSeq,
    },
    /// A previously acknowledged native record is permanently malformed.
    /// New work cannot safely pass its ordered slot.
    PermanentRecordFailure {
        /// Sequence of the malformed record.
        sequence: CommitSeq,
        /// Exact structural validation failure retained by the queue.
        source: RecordError,
    },
}

impl fmt::Display for ReserveError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Record(error) => write!(f, "cannot prepare transaction record: {error}"),
            Self::SequenceExhausted => write!(f, "commit sequence space is exhausted"),
            Self::UnknownOutcome { sequence } => write!(
                f,
                "commit sequence {} has an unknown native outcome",
                sequence.get()
            ),
            Self::PermanentRecordFailure { sequence, source } => write!(
                f,
                "commit record {} is permanently unreplayable: {source}",
                sequence.get()
            ),
        }
    }
}

impl std::error::Error for ReserveError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Record(error) | Self::PermanentRecordFailure { source: error, .. } => Some(error),
            Self::SequenceExhausted | Self::UnknownOutcome { .. } => None,
        }
    }
}

impl From<RecordError> for ReserveError {
    fn from(value: RecordError) -> Self {
        Self::Record(value)
    }
}

/// An impossible-in-normal-use reservation transition was rejected.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ResolveError {
    /// The slot is no longer present in the queue.
    Missing {
        /// Sequence of the missing slot.
        sequence: CommitSeq,
    },
    /// The slot had already left its prepared state.
    AlreadyResolved {
        /// Sequence of the resolved slot.
        sequence: CommitSeq,
    },
    /// The slot is pinned because the native commit outcome is unknown.
    Pinned {
        /// Sequence of the pinned slot.
        sequence: CommitSeq,
    },
    /// A known-committed transaction cannot be acknowledged because an
    /// earlier ordered slot has an ambiguous native outcome.
    BlockedByPriorUnknown {
        /// Sequence of the known-committed transaction retained and pinned.
        sequence: CommitSeq,
        /// Earlier sequence whose native outcome is ambiguous.
        prior_unknown: CommitSeq,
    },
    /// A known-committed transaction cannot be acknowledged because an
    /// earlier record is permanently malformed.
    BlockedByPriorRecordFailure {
        /// Sequence of the known-committed transaction retained in the queue.
        sequence: CommitSeq,
        /// Earlier malformed sequence that prevents replay.
        prior_failure: CommitSeq,
        /// Exact structural validation failure retained by the queue.
        source: RecordError,
    },
}

impl fmt::Display for ResolveError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Missing { sequence } => {
                write!(f, "reservation {} is no longer queued", sequence.get())
            }
            Self::AlreadyResolved { sequence } => {
                write!(f, "reservation {} is already resolved", sequence.get())
            }
            Self::Pinned { sequence } => write!(
                f,
                "reservation {} has an unknown native outcome",
                sequence.get()
            ),
            Self::BlockedByPriorUnknown {
                sequence,
                prior_unknown,
            } => write!(
                f,
                "reservation {} cannot be acknowledged because prior reservation {} has an unknown native outcome",
                sequence.get(),
                prior_unknown.get()
            ),
            Self::BlockedByPriorRecordFailure {
                sequence,
                prior_failure,
                source,
            } => write!(
                f,
                "reservation {} cannot be acknowledged because prior record {} is permanently unreplayable: {source}",
                sequence.get(),
                prior_failure.get()
            ),
        }
    }
}

impl std::error::Error for ResolveError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::BlockedByPriorRecordFailure { source, .. } => Some(source),
            Self::Missing { .. }
            | Self::AlreadyResolved { .. }
            | Self::Pinned { .. }
            | Self::BlockedByPriorUnknown { .. } => None,
        }
    }
}

/// A queue drain could not apply its acknowledged snapshot to the backend.
#[derive(Debug, Clone)]
pub enum ApplyError {
    /// A commit at or before the target has an ambiguous native outcome.
    UnknownOutcome {
        /// The first pinned sequence.
        sequence: CommitSeq,
    },
    /// Rocks (or another [`Blobs`] implementation) kept rejecting the atomic
    /// Ready prefix beginning at the front transaction after the configured
    /// retry budget.
    Backend {
        /// Sequence whose atomic backend batch failed.
        sequence: CommitSeq,
        /// Number of failed attempts made by this application barrier.
        attempts: usize,
        /// Last backend error.
        source: BlobError,
    },
    /// A native-produced record could not be decoded for replay. The ordered
    /// slot remains present and later transactions cannot pass it.
    Record {
        /// Sequence of the malformed record.
        sequence: CommitSeq,
        /// Record validation failure.
        source: RecordError,
    },
}

impl fmt::Display for ApplyError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::UnknownOutcome { sequence } => write!(
                f,
                "commit sequence {} has an unknown native outcome",
                sequence.get()
            ),
            Self::Backend {
                sequence,
                attempts,
                source,
            } => write!(
                f,
                "backend rejected commit sequence {} after {attempts} attempt(s): {source}",
                sequence.get()
            ),
            Self::Record { sequence, source } => write!(
                f,
                "commit record {} cannot be replayed: {source}",
                sequence.get()
            ),
        }
    }
}

impl std::error::Error for ApplyError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::UnknownOutcome { .. } => None,
            Self::Backend { source, .. } => Some(source),
            Self::Record { source, .. } => Some(source),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum SlotState {
    Prepared {
        pinned: bool,
    },
    Ready,
    /// The serialized consumer temporarily owns this slot's record while the
    /// queue lock is released for decoding and backend IO.
    Applying,
}

#[derive(Debug)]
struct Slot {
    sequence: CommitSeq,
    record: Option<QueuedCommitRecord>,
    state: SlotState,
}

const TURN_PHASE_BITS: u32 = 2;
const TURN_FREE: u64 = 0;
const TURN_BOUND: u64 = 1;
const TURN_WRITTEN: u64 = 2;
const TURN_READY: u64 = 3;

fn turn_token(raw_sequence: u64, ring_shift: u32, phase: u64) -> u64 {
    debug_assert!(ring_shift >= TURN_PHASE_BITS);
    debug_assert!(phase < (1 << TURN_PHASE_BITS));
    (raw_sequence >> ring_shift) << TURN_PHASE_BITS | phase
}

/// Stable bind and publication storage for one sequence-indexed ring position.
///
/// `turn` combines the implicit ring index, sequence lap, and lifecycle phase
/// in one exact token. Concurrent binders acquire the FREE token directly.
/// The SPSC producer instead derives exclusive ownership from its cached
/// applied frontier: an Acquire refresh observes the consumer's Release after
/// its final old-generation read, so the hot path needs no per-cell load.
/// Common arena records bypass this cell entirely, while the cold
/// legacy/oversized path transfers owned state through `record`.
#[repr(C, align(64))]
struct PublicationCell {
    turn: AtomicU64,
    arena_mako_timestamp: UnsafeCell<u32>,
    arena_record_bytes: UnsafeCell<usize>,
}

/// Cold ownership paired one-for-one with a [`PublicationCell`].
///
/// Common arena records never touch this allocation. Keeping the vector-sized
/// record enum off the producer/consumer turn line lets each hot ring cell fit
/// in one cache line. The paired publication turn is also this value's
/// exclusive-access token: a producer may mutate it only while owning BOUND,
/// and the state-locked consumer finishes its last access before publishing
/// the next lap's FREE turn.
struct ColdPublicationCell {
    record: UnsafeCell<Option<QueuedCommitRecord>>,
}

/// Producer-written description of one directly published SPSC arena record.
///
/// This cell deliberately has no atomic lifecycle word. The exclusive
/// producer writes all three scalars and the arena bytes before advancing the
/// shared acknowledged/published frontier with Release. The sole consumer
/// Acquires that frontier before reading them, and does not permit this ring
/// position to be reused until it Release-publishes the applied frontier.
#[repr(C, align(64))]
struct SpscArenaPublication {
    sequence: UnsafeCell<u64>,
    mako_timestamp: UnsafeCell<u32>,
    exact_record_bytes: UnsafeCell<u32>,
}

// SAFETY: concurrent binders acquire the exact FREE -> BOUND CAS. The unique
// SPSC producer proves the previous generation retired from tail minus its
// cached applied frontier, then exclusively retains that future sequence until
// publication. Either protocol grants one producer exclusive access to every
// UnsafeCell. WRITTEN/READY Release publishes initialized scalar/raw bytes or
// cold-owned state. Only code holding `Writeback::state` may harvest cold
// ownership, and successful replay advances applied only after the sole
// consumer's final access.
unsafe impl Send for PublicationCell {}
unsafe impl Sync for PublicationCell {}
// SAFETY: MPMC access is governed by the paired PublicationCell's exact turn;
// SPSC access uses the unique producer and published/applied frontiers above.
// The cold allocation is fixed for Writeback's lifetime and only the
// state-locked consumer may take published ownership.
unsafe impl Send for ColdPublicationCell {}
unsafe impl Sync for ColdPublicationCell {}
// SAFETY: the single-producer published/applied frontier protocol documented
// on the type gives one producer exclusive write access and orders the sole
// consumer's reads. These cells are never used by the concurrent queue mode.
unsafe impl Send for SpscArenaPublication {}
unsafe impl Sync for SpscArenaPublication {}

const _: () = {
    assert!(std::mem::size_of::<PublicationCell>() == 64);
    assert!(std::mem::align_of::<PublicationCell>() == 64);
    assert!(std::mem::offset_of!(PublicationCell, turn) == 0);
    assert!(std::mem::offset_of!(PublicationCell, arena_mako_timestamp) == 8);
    assert!(std::mem::offset_of!(PublicationCell, arena_record_bytes) == 16);
    assert!(TURN_FREE == 0);
    assert!(TURN_BOUND == 1);
    assert!(TURN_WRITTEN == 2);
    assert!(TURN_READY == 3);
};
const _: () = assert!(std::mem::size_of::<SpscArenaPublication>() == 64);

impl ColdPublicationCell {
    const fn empty() -> Self {
        Self {
            record: UnsafeCell::new(None),
        }
    }
}

impl SpscArenaPublication {
    const fn empty() -> Self {
        Self {
            sequence: UnsafeCell::new(0),
            mako_timestamp: UnsafeCell::new(0),
            exact_record_bytes: UnsafeCell::new(0),
        }
    }

    /// Install the scalar description paired with already-initialized arena
    /// bytes. The caller publishes the shared dense frontier afterward.
    ///
    /// # Safety
    ///
    /// `sequence` must be the unique producer's retained next sequence and its
    /// capacity proof must establish that the prior ring generation has been
    /// applied. `exact_record_bytes` must describe the completely initialized
    /// native arena target for this same sequence.
    unsafe fn install(
        &self,
        sequence: CommitSeq,
        mako_timestamp: MakoTimestamp,
        exact_record_bytes: usize,
    ) {
        let exact_record_bytes =
            u32::try_from(exact_record_bytes).expect("the fixed native arena extent fits u32");
        // SAFETY: required by this method's exclusive-generation contract.
        unsafe {
            self.sequence.get().write(sequence.get());
            self.mako_timestamp.get().write(mako_timestamp.get());
            self.exact_record_bytes.get().write(exact_record_bytes);
        }
    }

    /// Reconstruct the published arena record after acquiring the shared
    /// published frontier. A mismatched generation belongs to a cold/cell
    /// publication and is left to the established fallback.
    ///
    /// # Safety
    ///
    /// The caller must have Acquired an acknowledged frontier at least as high
    /// as `sequence` and must hold the serialized queue-state consumer lock.
    unsafe fn harvest(
        &self,
        sequence: CommitSeq,
        arena: &NativeRecordArena,
        arena_block: usize,
    ) -> Option<QueuedCommitRecord> {
        // SAFETY: the caller's frontier Acquire observes the producer's scalar
        // writes; the applied-frontier reuse rule prevents concurrent rewrite.
        if unsafe { *self.sequence.get() } != sequence.get() {
            return None;
        }
        let mako_timestamp = MakoTimestamp::new(unsafe { *self.mako_timestamp.get() })
            .expect("a published SPSC arena record retains a Mako timestamp");
        let exact_record_bytes = unsafe { *self.exact_record_bytes.get() } as usize;
        // SAFETY: the same published/applied frontier proof owns this exact
        // block through retirement, and native initialized the advertised
        // extent before the producer's Release publication.
        let bytes = unsafe { arena.target(arena_block, exact_record_bytes) };
        Some(QueuedCommitRecord::Native(unsafe {
            NativeCommitRecord::from_native_arena(
                sequence,
                mako_timestamp,
                bytes,
                exact_record_bytes,
                arena_block,
            )
        }))
    }

    /// Whether this generation used direct SPSC arena publication.
    ///
    /// # Safety
    ///
    /// The caller must hold the consumer state lock and have acquired a
    /// published frontier covering `sequence`.
    unsafe fn contains(&self, sequence: CommitSeq) -> bool {
        unsafe { *self.sequence.get() == sequence.get() }
    }
}

impl PublicationCell {
    fn free(initial_free_turn: u64) -> Self {
        Self {
            turn: AtomicU64::new(initial_free_turn),
            arena_mako_timestamp: UnsafeCell::new(0),
            arena_record_bytes: UnsafeCell::new(0),
        }
    }

    /// Acquire the exact free turn and publish one dense bound generation.
    fn publish_bound(&self, cold: &ColdPublicationCell, sequence: CommitSeq, ring_shift: u32) {
        let free = turn_token(sequence.get(), ring_shift, TURN_FREE);
        let bound = turn_token(sequence.get(), ring_shift, TURN_BOUND);
        self.turn
            .compare_exchange(free, bound, Ordering::AcqRel, Ordering::Acquire)
            .expect("a bound sequence must acquire its exact free ring turn");
        // SAFETY: the successful Acquire above observes prior retirement and
        // grants this producer exclusive cell ownership.
        unsafe { self.arena_record_bytes.get().write(0) };
        debug_assert!(unsafe { (&*cold.record.get()).is_none() });
    }

    /// Bind one exact generation when a stronger external gate excludes every
    /// other sequence allocator for this Writeback.
    ///
    /// # Safety
    ///
    /// No other concurrent binder may read or update the queue tail or claim a
    /// publication cell. `sequence` must be the next dense sequence and the
    /// caller must own one detached capacity claim. The ring-capacity invariant
    /// must therefore prove this exact FREE turn belongs to the caller.
    unsafe fn publish_bound_externally_serialized(
        &self,
        cold: &ColdPublicationCell,
        sequence: CommitSeq,
        ring_shift: u32,
    ) {
        let free = turn_token(sequence.get(), ring_shift, TURN_FREE);
        let bound = turn_token(sequence.get(), ring_shift, TURN_BOUND);
        assert_eq!(
            self.turn.load(Ordering::Acquire),
            free,
            "an externally serialized bind must observe its exact free ring turn"
        );
        // SAFETY: the exact FREE Acquire plus the caller's external exclusion
        // contract grants unique access to both paired cells. Initialize them
        // before publishing BOUND so importers cannot observe stale metadata.
        unsafe { self.arena_record_bytes.get().write(0) };
        debug_assert!(unsafe { (&*cold.record.get()).is_none() });
        self.turn.store(bound, Ordering::Release);
    }

    /// Publish an exact generation whose dense sequence was already assigned.
    ///
    /// # Safety
    ///
    /// The caller must own one detached occupancy claim converted to this
    /// unique sequence. With live occupancy at most the configured capacity
    /// and ring length at least that capacity, no other assigned live sequence
    /// can alias this cell, even though other producers may bind distinct cells
    /// concurrently. The prior generation must have retired before its
    /// occupancy credit was released.
    unsafe fn publish_bound_preassigned(
        &self,
        cold: &ColdPublicationCell,
        sequence: CommitSeq,
        ring_shift: u32,
    ) {
        let free = turn_token(sequence.get(), ring_shift, TURN_FREE);
        let bound = turn_token(sequence.get(), ring_shift, TURN_BOUND);
        if self.turn.load(Ordering::Acquire) != free {
            // Native has already advanced the dense tail. Unwinding would let
            // the permit release occupancy across an unfillable hole, so an
            // ABI/invariant mismatch must terminate fail-closed.
            std::process::abort();
        }
        // SAFETY: the capacity/sequence proof grants unique ownership of both
        // paired cells until this generation is retired.
        unsafe { self.arena_record_bytes.get().write(0) };
        if unsafe { (&*cold.record.get()).is_some() } {
            // The dense tail has already moved past this generation. Do not
            // unwind across an obligation which can no longer be canceled.
            std::process::abort();
        }
        self.turn.store(bound, Ordering::Release);
    }

    /// Publish a previously retained single-producer reservation as BOUND.
    ///
    /// # Safety
    ///
    /// The caller must retain the unique foreground lease and capacity credit,
    /// `sequence` must still be the next dense tail, and the applied frontier
    /// must prove the prior ring generation has completed its final read. Those
    /// conditions grant exclusive access to the paired `UnsafeCell` metadata
    /// without reading or compare-exchanging the old turn.
    unsafe fn publish_bound_single_reserved(
        &self,
        cold: &ColdPublicationCell,
        sequence: CommitSeq,
        ring_shift: u32,
    ) {
        // SAFETY: the method contract grants this producer unique ownership of
        // the reused generation until the following Release publication.
        unsafe { self.arena_record_bytes.get().write(0) };
        debug_assert!(unsafe { (&*cold.record.get()).is_none() });
        self.turn.store(
            turn_token(sequence.get(), ring_shift, TURN_BOUND),
            Ordering::Release,
        );
    }

    /// Materialize one completed SPSC arena record in the legacy READY cell.
    ///
    /// # Safety
    ///
    /// The caller must own the retained future sequence, keep the unique
    /// producer lease live, prove through the applied frontier that the prior
    /// ring generation completed its final read, and hold native's exact
    /// completion witness for every arena byte. No consumer may observe this
    /// generation until the final Release store.
    #[inline(always)]
    unsafe fn publish_arena_ready_single_reserved(
        &self,
        cold: &ColdPublicationCell,
        sequence: CommitSeq,
        ring_shift: u32,
        mako_timestamp: MakoTimestamp,
        exact_record_bytes: usize,
    ) {
        // SPSC retirement deliberately does not write this legacy line. It may
        // therefore contain any phase from an already-applied prior lap. The
        // caller's capacity/applied proof, rather than that stale word, grants
        // exclusive ownership of the cell and cold metadata now.
        debug_assert!(unsafe { (&*cold.record.get()).is_none() });
        // SAFETY: the method contract grants exclusive access to this exact
        // generation until READY is published below.
        unsafe {
            self.arena_mako_timestamp.get().write(mako_timestamp.get());
            self.arena_record_bytes.get().write(exact_record_bytes);
        }
        self.turn.store(
            turn_token(sequence.get(), ring_shift, TURN_READY),
            Ordering::Release,
        );
    }

    fn is_bound(&self, sequence: CommitSeq, ring_shift: u32) -> bool {
        let turn = self.turn.load(Ordering::Acquire);
        turn == turn_token(sequence.get(), ring_shift, TURN_BOUND)
            || turn == turn_token(sequence.get(), ring_shift, TURN_WRITTEN)
            || turn == turn_token(sequence.get(), ring_shift, TURN_READY)
    }

    /// Attach a complete cold owned record while this generation is Prepared.
    fn attach(
        &self,
        cold: &ColdPublicationCell,
        sequence: CommitSeq,
        ring_shift: u32,
        record: QueuedCommitRecord,
    ) {
        // SAFETY: the unique BoundReservation owns this unpublished cell. A
        // consumer cannot inspect record until a later Release publication.
        debug_assert!(
            unsafe { (&*cold.record.get()).is_none() },
            "a Prepared generation may receive one complete record"
        );
        unsafe { cold.record.get().write(Some(record)) };
        self.publish_written(sequence, ring_shift);
    }

    /// Publish native's exact completion witness for one arena-backed record.
    fn attach_arena(
        &self,
        sequence: CommitSeq,
        ring_shift: u32,
        mako_timestamp: MakoTimestamp,
        exact_record_bytes: usize,
    ) {
        debug_assert_eq!(
            self.turn.load(Ordering::Acquire),
            turn_token(sequence.get(), ring_shift, TURN_BOUND)
        );
        // SAFETY: the unique unpublished BoundReservation owns this exact
        // generation. The following Release store transfers these scalars and
        // native's already-initialized arena bytes to pin/consumer paths.
        unsafe {
            self.arena_mako_timestamp.get().write(mako_timestamp.get());
            self.arena_record_bytes.get().write(exact_record_bytes);
        }
        self.publish_written(sequence, ring_shift);
    }

    /// Attach native's completed common-arena record and publish READY in one
    /// producer-owned transition.
    fn attach_arena_ready(
        &self,
        sequence: CommitSeq,
        ring_shift: u32,
        mako_timestamp: MakoTimestamp,
        exact_record_bytes: usize,
    ) {
        debug_assert_eq!(
            self.turn.load(Ordering::Acquire),
            turn_token(sequence.get(), ring_shift, TURN_BOUND)
        );
        // SAFETY: the unique NativeArenaBoundReservation owns this generation;
        // native's exact completion witness covers the arena bytes. Publishing
        // READY transfers both scalars and bytes directly to helpers/consumer.
        unsafe {
            self.arena_mako_timestamp.get().write(mako_timestamp.get());
            self.arena_record_bytes.get().write(exact_record_bytes);
        }
        self.turn.store(
            turn_token(sequence.get(), ring_shift, TURN_READY),
            Ordering::Release,
        );
    }

    /// Attach a sealed deferred holder and publish READY without constructing
    /// a cold Rust record object on the foreground path.
    fn attach_holder_ready(
        &self,
        sequence: CommitSeq,
        ring_shift: u32,
        mako_timestamp: MakoTimestamp,
        exact_record_bytes: usize,
    ) {
        debug_assert_eq!(
            self.turn.load(Ordering::Acquire),
            turn_token(sequence.get(), ring_shift, TURN_BOUND)
        );
        // SAFETY: the exact BOUND generation owns these scalars. Native sealed
        // the pool generation before returning; READY transfers both facts to
        // the sole consumer.
        unsafe {
            self.arena_mako_timestamp.get().write(mako_timestamp.get());
            self.arena_record_bytes
                .get()
                .write(tagged_native_holder_extent(exact_record_bytes));
        }
        self.turn.store(
            turn_token(sequence.get(), ring_shift, TURN_READY),
            Ordering::Release,
        );
    }

    /// Attach a sealed holder without making an uncertain commit replayable.
    fn attach_holder_written(
        &self,
        sequence: CommitSeq,
        ring_shift: u32,
        mako_timestamp: MakoTimestamp,
        exact_record_bytes: usize,
    ) {
        debug_assert_eq!(
            self.turn.load(Ordering::Acquire),
            turn_token(sequence.get(), ring_shift, TURN_BOUND)
        );
        unsafe {
            self.arena_mako_timestamp.get().write(mako_timestamp.get());
            self.arena_record_bytes
                .get()
                .write(tagged_native_holder_extent(exact_record_bytes));
        }
        self.publish_written(sequence, ring_shift);
    }

    fn publish_written(&self, sequence: CommitSeq, ring_shift: u32) {
        let bound = turn_token(sequence.get(), ring_shift, TURN_BOUND);
        let written = turn_token(sequence.get(), ring_shift, TURN_WRITTEN);
        assert_eq!(
            self.turn.load(Ordering::Acquire),
            bound,
            "a Prepared generation accepts one completion witness"
        );
        // The unique `&mut BoundReservation` is the only possible writer after
        // FREE -> BOUND. A Release store publishes the completed payload
        // without another locked RMW on this producer-owned line.
        self.turn.store(written, Ordering::Release);
    }

    /// Make an already-attached Prepared record visible to the consumer.
    fn publish_attached(&self, sequence: CommitSeq, ring_shift: u32) {
        let written = turn_token(sequence.get(), ring_shift, TURN_WRITTEN);
        let ready = turn_token(sequence.get(), ring_shift, TURN_READY);
        assert_eq!(
            self.turn.load(Ordering::Acquire),
            written,
            "publication requires one exact completion witness"
        );
        // The reservation remains the unique writer; the consumer only begins
        // after this Release makes the exact Ready turn visible.
        self.turn.store(ready, Ordering::Release);
    }

    fn is_published(&self, sequence: CommitSeq, ring_shift: u32) -> bool {
        self.turn.load(Ordering::Acquire) == turn_token(sequence.get(), ring_shift, TURN_READY)
    }

    /// Reconstruct or move a published record under the queue-state mutex.
    ///
    /// # Safety
    ///
    /// The caller must own the one-shot Prepared -> Ready transition for this
    /// exact sequence. A second harvest could duplicate conceptual ownership
    /// of an arena block or inspect a cold record after it was moved out.
    unsafe fn harvest(
        &self,
        cold: &ColdPublicationCell,
        sequence: CommitSeq,
        ring_shift: u32,
        arena: &NativeRecordArena,
        arena_block: usize,
    ) -> Option<QueuedCommitRecord> {
        if !self.is_published(sequence, ring_shift) {
            return None;
        }
        let exact_record_bytes = unsafe { *self.arena_record_bytes.get() };
        if exact_record_bytes & NATIVE_HOLDER_RECORD_TAG != 0 {
            let mako_timestamp = MakoTimestamp::new(unsafe { *self.arena_mako_timestamp.get() })
                .expect("a published holder retains a valid Mako timestamp");
            return Some(QueuedCommitRecord::Holder(DeferredOnePutRecord::new(
                sequence,
                mako_timestamp,
                exact_record_bytes & !NATIVE_HOLDER_RECORD_TAG,
            )));
        }
        if exact_record_bytes != 0 {
            // SAFETY: the exact published Acquire observes scalar metadata and
            // native's completion-covered bytes. In concurrent mode aggregate
            // occupancy prevents reuse; in single-producer mode the bounded
            // tail/applied window and retained next sequence do. Both modes
            // keep this block owned until the returned record is retired.
            let mako_timestamp = MakoTimestamp::new(unsafe { *self.arena_mako_timestamp.get() })
                .expect("a bound arena record retains a valid Mako timestamp");
            let bytes = unsafe { arena.target(arena_block, exact_record_bytes) };
            let record = unsafe {
                NativeCommitRecord::from_native_arena(
                    sequence,
                    mako_timestamp,
                    bytes,
                    exact_record_bytes,
                    arena_block,
                )
            };
            return Some(QueuedCommitRecord::Native(record));
        }
        // SAFETY: acquiring the exact published generation observes the
        // producer's initialized record. The caller's queue-state mutex and
        // Prepared slot grant unique harvesting ownership.
        Some(
            unsafe { (&mut *cold.record.get()).take() }
                .expect("a Ready publication cell owns one complete record"),
        )
    }

    /// Mark a never-published generation permanently ambiguous.
    fn pin(&self, sequence: CommitSeq, ring_shift: u32) {
        let turn = self.turn.load(Ordering::Acquire);
        assert!(
            turn == turn_token(sequence.get(), ring_shift, TURN_BOUND)
                || turn == turn_token(sequence.get(), ring_shift, TURN_WRITTEN),
            "only an unpublished queue generation may become unknown"
        );
    }

    /// Reconstruct or move an attached unpublished record into a pinned slot.
    fn take_unpublished(
        &self,
        cold: &ColdPublicationCell,
        sequence: CommitSeq,
        ring_shift: u32,
        arena: &NativeRecordArena,
        arena_block: usize,
    ) -> Option<QueuedCommitRecord> {
        let turn = self.turn.load(Ordering::Acquire);
        let bound = turn_token(sequence.get(), ring_shift, TURN_BOUND);
        let written = turn_token(sequence.get(), ring_shift, TURN_WRITTEN);
        assert!(turn == bound || turn == written);
        if turn == bound {
            return None;
        }
        let exact_record_bytes = unsafe { *self.arena_record_bytes.get() };
        if exact_record_bytes & NATIVE_HOLDER_RECORD_TAG != 0 {
            let mako_timestamp = MakoTimestamp::new(unsafe { *self.arena_mako_timestamp.get() })
                .expect("a written holder retains a valid Mako timestamp");
            return Some(QueuedCommitRecord::Holder(DeferredOnePutRecord::new(
                sequence,
                mako_timestamp,
                exact_record_bytes & !NATIVE_HOLDER_RECORD_TAG,
            )));
        }
        if exact_record_bytes != 0 {
            let mako_timestamp = MakoTimestamp::new(unsafe { *self.arena_mako_timestamp.get() })
                .expect("a bound arena record retains a valid Mako timestamp");
            let bytes = unsafe { arena.target(arena_block, exact_record_bytes) };
            // SAFETY: exact written generation is native's completion witness;
            // pinning permanently stops the dense tail in either queue mode,
            // so this arena block stays live.
            return Some(QueuedCommitRecord::Native(unsafe {
                NativeCommitRecord::from_native_arena(
                    sequence,
                    mako_timestamp,
                    bytes,
                    exact_record_bytes,
                    arena_block,
                )
            }));
        }
        // SAFETY: the unique producer relinquished this generation to the
        // state-locked pin transition, and a zero publication marker prevents
        // the consumer from accessing the cell.
        unsafe { (&mut *cold.record.get()).take() }
    }

    /// Release this exact ring turn for its next representable generation.
    fn retire(
        &self,
        cold: &ColdPublicationCell,
        sequence: CommitSeq,
        ring_shift: u32,
        ring_len: usize,
    ) {
        let ready = turn_token(sequence.get(), ring_shift, TURN_READY);
        // The state-locked Applying slot proves cold owned state was harvested.
        debug_assert!(unsafe { (&*cold.record.get()).is_none() });
        let Some(next_sequence) = u64::try_from(ring_len)
            .ok()
            .and_then(|ring_len| sequence.get().checked_add(ring_len))
        else {
            assert_eq!(self.turn.load(Ordering::Acquire), ready);
            return;
        };
        let next_free = turn_token(next_sequence, ring_shift, TURN_FREE);
        assert_eq!(
            self.turn.load(Ordering::Acquire),
            ready,
            "retirement must release the exact ready ring turn"
        );
        // `consumer` plus Applying ownership makes this the sole writer until
        // next-lap bind Acquires the exact Free token.
        self.turn.store(next_free, Ordering::Release);
    }
}

/// One stable allocation split into sequence-ring small-record blocks.
///
/// `UnsafeCell` is required because native initializes a uniquely checked-out
/// block through a raw pointer while producers hold only a shared `Writeback`
/// reference. Dense sequence assignment plus the mode-specific bounded-live
/// proof is the exclusive-access proof for each modulo-ring block.
#[repr(C, align(64))]
struct NativeRecordArenaBlock {
    bytes: UnsafeCell<[MaybeUninit<u8>; NATIVE_RECORD_ARENA_BLOCK_BYTES]>,
}

const _: () = {
    assert!(std::mem::size_of::<NativeRecordArenaBlock>() == 256);
    assert!(std::mem::align_of::<NativeRecordArenaBlock>() == 64);
    assert!(std::mem::offset_of!(NativeRecordArenaBlock, bytes) == 0);
};

struct NativeRecordArena {
    blocks: Box<[NativeRecordArenaBlock]>,
    block_bytes: usize,
}

// SAFETY: a bound sequence owns its same-index ring block until backend
// retirement. Concurrent mode bounds live generations with aggregate
// Occupancy. Single-producer mode bounds `tail - applied` and permits only its
// unique lease to retain the exact next turn before bind. Thus, with logical
// capacity C and ring length R >= C, neither mode can reach a generation's Rth
// successor while the old generation remains live. Concurrent mode completes
// the byte handoff through the exact turn Acquire. SPSC refreshes applied under
// the retirement state mutex; retaining a sequence whose prior lap is at or
// below that frontier supplies the equivalent happens-before edge without a
// per-cell read. Blocks never overlap, the boxed allocation never moves, and
// Writeback drops State and publication cells before this arena field.
unsafe impl Send for NativeRecordArena {}
unsafe impl Sync for NativeRecordArena {}

impl NativeRecordArena {
    fn new(capacity: usize, max_record_bytes: usize) -> Result<Self, ConfigError> {
        let block_bytes = NATIVE_RECORD_ARENA_BLOCK_BYTES.min(max_record_bytes);
        capacity
            .checked_mul(std::mem::size_of::<NativeRecordArenaBlock>())
            .ok_or(ConfigError::NativeRecordArenaTooLarge)?;
        let blocks = Box::<[NativeRecordArenaBlock]>::new_uninit_slice(capacity);
        // SAFETY: the only data bytes are MaybeUninit<u8>; an uninitialized
        // payload and alignment padding are both valid. UnsafeCell adds no
        // initialization invariant. Native initializes an exact extent only
        // after the sequence-indexed block is exclusively retained.
        let blocks = unsafe { blocks.assume_init() };
        Ok(Self {
            blocks,
            block_bytes,
        })
    }

    const fn block_bytes(&self) -> usize {
        self.block_bytes
    }

    /// Return a stable block address solely for a non-dereferencing prefetch.
    fn prefetch_target(&self, block: usize) -> NonNull<u8> {
        debug_assert!(block < self.blocks.len());
        // SAFETY: the boxed array is stable and every aligned block contains a
        // 256-byte MaybeUninit payload at offset zero.
        unsafe {
            let cell = std::ptr::addr_of!((*self.blocks.as_ptr().add(block)).bytes);
            NonNull::new_unchecked(UnsafeCell::raw_get(cell).cast::<u8>())
        }
    }

    /// Return the start of one exclusively checked-out block.
    ///
    /// # Safety
    ///
    /// `block` must belong to the caller's live bound sequence and be unique,
    /// and `len` must not exceed the configured block extent.
    unsafe fn target(&self, block: usize, len: usize) -> NonNull<u8> {
        debug_assert!(len <= self.block_bytes);
        debug_assert!(block < self.blocks.len());
        // SAFETY: construction created one stable, cache-line-aligned block per
        // ring index. The caller's frontier/turn proof makes it unique.
        unsafe {
            let cell = std::ptr::addr_of!((*self.blocks.as_ptr().add(block)).bytes);
            NonNull::new_unchecked(UnsafeCell::raw_get(cell).cast::<u8>())
        }
    }
}

#[derive(Debug)]
enum NativeRecordBuffer {
    /// Small-record capacity claimed before validation but not yet assigned a
    /// dense sequence/ring block.
    UnboundArena {
        exact_record_bytes: usize,
    },
    Arena {
        block: usize,
        exact_record_bytes: usize,
    },
    Owned(Vec<MaybeUninit<u8>>),
}

impl NativeRecordBuffer {
    fn prepare_owned(
        mut bytes: Vec<MaybeUninit<u8>>,
        exact_record_bytes: usize,
    ) -> Result<Self, RecordError> {
        bytes.clear();
        if bytes.capacity() < exact_record_bytes {
            bytes
                .try_reserve_exact(exact_record_bytes)
                .map_err(|_| RecordError::AllocationFailed)?;
        }
        // SAFETY: MaybeUninit elements need not be initialized. Native receives
        // this exact extent only through the synchronous target terminal.
        unsafe { bytes.set_len(exact_record_bytes) };
        Ok(Self::Owned(bytes))
    }

    const fn exact_record_bytes(&self) -> usize {
        match self {
            Self::UnboundArena { exact_record_bytes } => *exact_record_bytes,
            Self::Arena {
                exact_record_bytes, ..
            } => *exact_record_bytes,
            Self::Owned(bytes) => bytes.len(),
        }
    }

    fn bind_arena(self, block: usize) -> Self {
        match self {
            Self::UnboundArena { exact_record_bytes } => Self::Arena {
                block,
                exact_record_bytes,
            },
            owned @ Self::Owned(_) => owned,
            Self::Arena { .. } => panic!("an arena buffer may be bound only once"),
        }
    }

    fn target(&mut self, arena: &NativeRecordArena) -> NonNull<u8> {
        match self {
            Self::UnboundArena { .. } => {
                panic!("a native target is requested only after sequence bind")
            }
            Self::Arena {
                block,
                exact_record_bytes,
            } => {
                // SAFETY: ownership of this buffer proves its block token is
                // checked out and unique until the buffer is consumed.
                unsafe { arena.target(*block, *exact_record_bytes) }
            }
            Self::Owned(bytes) => NonNull::new(bytes.as_mut_ptr().cast::<u8>())
                .expect("a nonempty prepared record buffer has storage"),
        }
    }

    /// Convert storage covered by native's exact completion witness into an
    /// immutable queued record without copying.
    ///
    /// # Safety
    ///
    /// Native must have initialized every byte in `exact_record_bytes`,
    /// returned its validated 0/1 completion witness, and encoded the canonical
    /// record for exactly `sequence`, `mako_timestamp`, and the integrity mode
    /// sealed by preflight. Background materialization verifies that contract
    /// before backend replay.
    unsafe fn into_record(
        self,
        arena: &NativeRecordArena,
        sequence: CommitSeq,
        mako_timestamp: MakoTimestamp,
    ) -> NativeCommitRecord {
        match self {
            Self::UnboundArena { .. } => {
                panic!("an unbound arena buffer cannot have a completion witness")
            }
            Self::Arena {
                block,
                exact_record_bytes,
            } => {
                // SAFETY: this buffer uniquely owns the block and the caller's
                // witness proves its exact extent initialized.
                let bytes = unsafe { arena.target(block, exact_record_bytes) };
                // SAFETY: the same witness and arena lifetime establish the
                // NativeCommitRecord arena-storage contract.
                unsafe {
                    NativeCommitRecord::from_native_arena(
                        sequence,
                        mako_timestamp,
                        bytes,
                        exact_record_bytes,
                        block,
                    )
                }
            }
            Self::Owned(bytes) => {
                let mut bytes = ManuallyDrop::new(bytes);
                let pointer = bytes.as_mut_ptr().cast::<u8>();
                let len = bytes.len();
                let capacity = bytes.capacity();
                // SAFETY: MaybeUninit<u8> and u8 have identical allocation
                // layouts, and the completion witness covers all `len` bytes.
                let initialized = unsafe { Vec::from_raw_parts(pointer, len, capacity) };
                NativeCommitRecord::from_native(sequence, mako_timestamp, initialized)
            }
        }
    }
}

/// One bounded occupancy word isolated from unrelated queue metadata.
///
/// Every update is an RMW and the value enforces detached + bound <= capacity.
/// The exact per-cell turn, rather than this aggregate count, supplies the
/// `UnsafeCell`/arena reuse handoff.
#[repr(align(64))]
struct Occupancy {
    value: AtomicUsize,
}

impl Occupancy {
    const fn empty() -> Self {
        Self {
            value: AtomicUsize::new(0),
        }
    }

    fn try_claim(&self, capacity: usize) -> bool {
        // The old load/CAS loop could issue several locked cmpxchg operations
        // when concurrent producers raced below the limit. Give every attempt
        // one position with fetch_add instead. An over-limit attempt retracts
        // its provisional count before taking the slow path. Provisional
        // overclaims only make another claimant fail conservatively: the
        // atomic value is never below the number of successful live claims.
        let prior = self.value.fetch_add(1, Ordering::AcqRel);
        if prior < capacity {
            return true;
        }
        let rollback_prior = self.value.fetch_sub(1, Ordering::Release);
        assert_ne!(rollback_prior, 0, "occupancy overclaim rollback underflow");
        false
    }

    /// Claim up to `maximum` logical rights with one shared RMW.
    ///
    /// This counter only enforces the numeric capacity bound. Exact ring-turn
    /// Acquire/Release transitions, not occupancy, transfer arena ownership,
    /// so the batch reservation needs no inter-thread memory ordering beyond
    /// the atomic word's modification order.
    #[inline(always)]
    fn try_claim_batch_relaxed(&self, maximum: usize, capacity: usize) -> usize {
        debug_assert_ne!(maximum, 0);
        let prior = self.value.fetch_add(maximum, Ordering::Relaxed);
        if prior >= capacity {
            let rollback_prior = self.value.fetch_sub(maximum, Ordering::Relaxed);
            assert!(
                rollback_prior >= maximum,
                "occupancy batch overclaim rollback underflow"
            );
            return 0;
        }

        let claimed = maximum.min(capacity - prior);
        let excess = maximum - claimed;
        if excess != 0 {
            let rollback_prior = self.value.fetch_sub(excess, Ordering::Relaxed);
            assert!(
                rollback_prior >= excess,
                "occupancy partial-batch rollback underflow"
            );
        }
        claimed
    }

    fn release(&self) {
        self.release_many(1);
    }

    fn release_many(&self, count: usize) {
        debug_assert_ne!(count, 0);
        let prior = self.value.fetch_sub(count, Ordering::Release);
        assert!(prior >= count, "bounded occupancy underflow");
    }

    fn load(&self) -> usize {
        self.value.load(Ordering::Acquire)
    }
}

const _: () = assert!(std::mem::size_of::<Occupancy>() == 64);

/// One independently owned, cache-line-isolated dense frontier.
///
/// The SPSC producer writes the published frontier while the consumer writes
/// the applied frontier. Giving each word its own line avoids making those two
/// cores exchange ownership merely because the allocator placed adjacent
/// atomics in one line. The address returned by [`Self::as_ptr`] is stable for
/// the lifetime of the enclosing [`Writeback`].
#[repr(C, align(64))]
struct CacheLineAtomicU64 {
    value: AtomicU64,
}

impl CacheLineAtomicU64 {
    const fn new(value: u64) -> Self {
        Self {
            value: AtomicU64::new(value),
        }
    }

    #[inline(always)]
    fn load(&self, ordering: Ordering) -> u64 {
        self.value.load(ordering)
    }

    #[inline(always)]
    fn store(&self, value: u64, ordering: Ordering) {
        self.value.store(value, ordering);
    }

    #[inline(always)]
    fn compare_exchange_weak(
        &self,
        current: u64,
        new: u64,
        success: Ordering,
        failure: Ordering,
    ) -> Result<u64, u64> {
        self.value
            .compare_exchange_weak(current, new, success, failure)
    }

    #[inline(always)]
    fn compare_exchange(
        &self,
        current: u64,
        new: u64,
        success: Ordering,
        failure: Ordering,
    ) -> Result<u64, u64> {
        self.value
            .compare_exchange(current, new, success, failure)
    }

    #[inline(always)]
    fn fetch_add(&self, value: u64, ordering: Ordering) -> u64 {
        self.value.fetch_add(value, ordering)
    }

    #[inline(always)]
    fn fetch_update<F>(
        &self,
        set_order: Ordering,
        fetch_order: Ordering,
        f: F,
    ) -> Result<u64, u64>
    where
        F: FnMut(u64) -> Option<u64>,
    {
        self.value.fetch_update(set_order, fetch_order, f)
    }

    #[inline(always)]
    const fn atomic(&self) -> &AtomicU64 {
        &self.value
    }

    #[inline(always)]
    fn as_ptr(&self) -> *mut u64 {
        self.value.as_ptr()
    }
}

const _: () = assert!(std::mem::size_of::<CacheLineAtomicU64>() == 64);

/// One cache-line-isolated waiter count used by the acknowledgement handoff.
///
/// A publisher reads this only after it closes a dense-prefix hole. A sleeping
/// out-of-order publisher writes it. Isolation keeps that uncommon exchange
/// away from the frontiers touched by every transaction.
#[repr(C, align(64))]
struct CacheLineAtomicUsize {
    value: AtomicUsize,
}

impl CacheLineAtomicUsize {
    const fn new(value: usize) -> Self {
        Self {
            value: AtomicUsize::new(value),
        }
    }

    #[inline(always)]
    fn load(&self, ordering: Ordering) -> usize {
        self.value.load(ordering)
    }

    #[inline(always)]
    fn store(&self, value: usize, ordering: Ordering) {
        self.value.store(value, ordering);
    }

    #[inline(always)]
    fn fetch_add(&self, value: usize, ordering: Ordering) -> usize {
        self.value.fetch_add(value, ordering)
    }

    #[inline(always)]
    fn fetch_sub(&self, value: usize, ordering: Ordering) -> usize {
        self.value.fetch_sub(value, ordering)
    }

    #[inline(always)]
    fn compare_exchange_weak(
        &self,
        current: usize,
        new: usize,
        success: Ordering,
        failure: Ordering,
    ) -> Result<usize, usize> {
        self.value
            .compare_exchange_weak(current, new, success, failure)
    }

    #[inline(always)]
    fn swap(&self, value: usize, ordering: Ordering) -> usize {
        self.value.swap(value, ordering)
    }
}

const _: () = assert!(std::mem::size_of::<CacheLineAtomicUsize>() == 64);

/// Producer-local capacity cursor owned by one thread-affine cache lease.
///
/// The exclusive capacity limit is `applied.saturating_add(capacity)`. A stale
/// limit only makes the queue appear conservatively fuller. The producer
/// refreshes it after consuming its local window, so no consumer writes the
/// producer's hot cache line.
pub(crate) struct SingleProducerState {
    // These are producer-local despite their atomic representation. Relaxed
    // words keep internal permit types structurally Send for concurrent-path
    // tests; the public SingleProducer capability remains !Send/!Sync and is
    // the sole accessor in production.
    capacity_limit: AtomicU64,
    next_sequence: AtomicU64,
    fused_holder_control: mako_local::TrustedSpscOnePutHolderControl,
}

impl SingleProducerState {
    fn new(
        applied: u64,
        next_sequence: u64,
        capacity: u64,
        fused_holder_control: mako_local::TrustedSpscOnePutHolderControl,
    ) -> Self {
        Self {
            capacity_limit: AtomicU64::new(applied.saturating_add(capacity)),
            next_sequence: AtomicU64::new(next_sequence),
            fused_holder_control,
        }
    }

    #[inline(always)]
    pub(crate) fn fused_holder_control(&self) -> &mako_local::TrustedSpscOnePutHolderControl {
        &self.fused_holder_control
    }

    #[inline(always)]
    pub(crate) fn next_sequence_ptr(&self) -> *mut u64 {
        self.next_sequence.as_ptr()
    }

    #[inline(always)]
    pub(crate) fn capacity_limit_ptr(&self) -> *const u64 {
        self.capacity_limit.as_ptr().cast_const()
    }

    /// Return the future sequence retained after an untouched fused attempt.
    #[inline(always)]
    pub(crate) fn retained_sequence(&self) -> NonZeroU64 {
        let next = self.next_sequence.load(Ordering::Relaxed);
        let Some(retained) = next.checked_add(1).and_then(NonZeroU64::new) else {
            // A consumed outcome after native observed a saturated cursor is a
            // same-build lifecycle contradiction. Fail-stop without creating
            // an invalid NonZeroU64 from foreign-controlled state.
            std::process::abort();
        };
        retained
    }

    /// Return the sequence already accepted by the fused native terminal.
    #[inline(always)]
    pub(crate) fn accepted_sequence(&self) -> NonZeroU64 {
        let accepted = self.next_sequence.load(Ordering::Relaxed);
        let Some(accepted) = NonZeroU64::new(accepted) else {
            // A committed-unpublished result promises cursor advancement.
            // Treat a missing generation as protocol corruption, not an
            // unchecked niche construction.
            std::process::abort();
        };
        accepted
    }

    #[inline(always)]
    fn accept(&self, sequence: CommitSeq) {
        // SAFETY: CommitSeq is nonzero by construction.
        self.accept_raw(unsafe { NonZeroU64::new_unchecked(sequence.get()) });
    }

    #[inline(always)]
    fn accept_raw(&self, sequence: NonZeroU64) {
        debug_assert_eq!(
            self.next_sequence.load(Ordering::Relaxed).checked_add(1),
            Some(sequence.get()),
            "the exclusive producer accepts its retained next sequence"
        );
        self.next_sequence.store(sequence.get(), Ordering::Relaxed);
    }
}

/// Cold recycled storage for records larger than the fixed arena block.
struct OversizedPool {
    buffers: Mutex<Vec<Vec<MaybeUninit<u8>>>>,
}

impl OversizedPool {
    fn new(maximum: usize) -> Result<Self, ConfigError> {
        let mut buffers = Vec::new();
        buffers
            .try_reserve_exact(maximum)
            .map_err(|_| ConfigError::NativeRecordArenaTooLarge)?;
        Ok(Self {
            buffers: Mutex::new(buffers),
        })
    }

    fn take(&self) -> Vec<MaybeUninit<u8>> {
        lock_recover(&self.buffers).pop().unwrap_or_default()
    }

    fn recycle(&self, mut bytes: Vec<MaybeUninit<u8>>, maximum: usize) {
        bytes.clear();
        let mut buffers = lock_recover(&self.buffers);
        if buffers.len() < maximum {
            buffers.push(bytes);
            return;
        }
        if let Some((smallest, _)) = buffers
            .iter()
            .enumerate()
            .min_by_key(|(_, current)| current.capacity())
            .filter(|(_, current)| current.capacity() < bytes.capacity())
        {
            buffers[smallest] = bytes;
        }
    }
}

/// Stable identity for one slot in the dense ordered queue.
///
/// Slots are only removed from the front and every bound transaction receives
/// the next dense commit sequence. Consequently subtracting the current front
/// sequence from this token gives the slot's current `VecDeque` offset without
/// scanning unrelated transactions.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct QueueToken(CommitSeq);

impl QueueToken {
    const fn new(sequence: CommitSeq) -> Self {
        Self(sequence)
    }

    const fn sequence(self) -> CommitSeq {
        self.0
    }
}

/// Earliest structural record failure discovered by the ordered consumer.
///
/// Allocation failure is deliberately excluded: it does not prove the native
/// bytes are malformed and can succeed on a later replay attempt.
#[derive(Clone, Debug, Eq, PartialEq)]
struct PermanentRecordFailure {
    sequence: CommitSeq,
    source: RecordError,
}

impl PermanentRecordFailure {
    fn apply_error(&self) -> ApplyError {
        ApplyError::Record {
            sequence: self.sequence,
            source: self.source.clone(),
        }
    }

    fn reserve_error(&self) -> ReserveError {
        ReserveError::PermanentRecordFailure {
            sequence: self.sequence,
            source: self.source.clone(),
        }
    }

    fn resolve_error(&self, sequence: CommitSeq) -> ResolveError {
        ResolveError::BlockedByPriorRecordFailure {
            sequence,
            prior_failure: self.sequence,
            source: self.source.clone(),
        }
    }
}

#[derive(Debug)]
struct State {
    queue: VecDeque<Slot>,
    /// Highest dense lock-free bind descriptor imported into `queue`.
    last_bound: u64,
    applied: AppliedWatermark,
    first_unknown: Option<CommitSeq>,
    permanent_record_failure: Option<PermanentRecordFailure>,
}

impl State {
    /// Resolve a stable queue token to its current `VecDeque` offset.
    ///
    /// A token before the current front has already been consumed; a token
    /// beyond the back was never inserted (or has otherwise gone missing).
    /// Retain the final equality check as a fail-closed guard around the dense
    /// queue invariant.
    fn queue_offset(&self, token: QueueToken) -> Option<usize> {
        let front = self.queue.front()?.sequence;
        let offset = token
            .sequence()
            .get()
            .checked_sub(front.get())
            .and_then(|offset| usize::try_from(offset).ok())?;
        self.queue
            .get(offset)
            .is_some_and(|slot| slot.sequence == token.sequence())
            .then_some(offset)
    }

    /// Return the earliest fail-stop condition that rejects new work.
    fn reserve_health_error(&self) -> Option<ReserveError> {
        match (&self.permanent_record_failure, self.first_unknown) {
            (Some(failure), Some(unknown)) if unknown < failure.sequence => {
                Some(ReserveError::UnknownOutcome { sequence: unknown })
            }
            (Some(failure), _) => Some(failure.reserve_error()),
            (None, Some(sequence)) => Some(ReserveError::UnknownOutcome { sequence }),
            (None, None) => None,
        }
    }

    /// Return the earliest fail-stop condition relevant to `target`.
    fn apply_health_error_through(&self, target: u64) -> Option<ApplyError> {
        let record = self
            .permanent_record_failure
            .as_ref()
            .filter(|failure| failure.sequence.get() <= target);
        let unknown = self
            .first_unknown
            .filter(|sequence| sequence.get() <= target);
        match (record, unknown) {
            (Some(failure), Some(sequence)) if sequence < failure.sequence => {
                Some(ApplyError::UnknownOutcome { sequence })
            }
            (Some(failure), _) => Some(failure.apply_error()),
            (None, Some(sequence)) => Some(ApplyError::UnknownOutcome { sequence }),
            (None, None) => None,
        }
    }

    /// Return any fail-stop health condition, including one after an already
    /// applied target. Clean shutdown uses this stronger check.
    fn health_error(&self) -> Option<ApplyError> {
        self.apply_health_error_through(u64::MAX)
    }
}

/// A bounded, transaction-ordered bridge from native Silo commits to Rocks.
///
/// `B` is stored directly. Use `Arc<B>` as the type argument when another
/// component also needs an owning backend handle; `mrx_core` implements
/// [`Blobs`] for `Arc<B>`.
pub struct Writeback<B: Blobs> {
    backend: B,
    config: WritebackConfig,
    /// Fixed at construction. A single-producer queue never mixes aggregate
    /// Occupancy accounting with its lease-owned logical capacity credits.
    single_producer: bool,
    state: Mutex<State>,
    changed: Condvar,
    acknowledgement_changed: Condvar,
    descriptor_available: Condvar,
    capacity_available: Condvar,
    /// Number of producers in the locked capacity-wait protocol. A release
    /// locks `state` only when this is nonzero, closing the predicate/wait
    /// notification window while leaving the ordinary non-full path lock-free.
    capacity_waiters: AtomicUsize,
    /// Slow-path resolvers waiting for an earlier producer to publish the
    /// descriptor behind an already allocated dense sequence.
    descriptor_waiters: AtomicUsize,
    /// Synchronous apply barriers currently enrolled in the `changed`
    /// condition-variable handoff. The dedicated runtime polls independently;
    /// the ordinary publisher reads this first and avoids `notify_one`
    /// entirely when no explicit barrier can be asleep.
    activity_waiters: AtomicUsize,
    consumer: Mutex<()>,
    /// Monotonic fast-path health summary. The detailed error remains under
    /// `state`; a healthy commit avoids taking the queue mutex merely to prove
    /// that no fail-stop condition has ever been latched.
    unhealthy: AtomicBool,
    /// Publishers sleeping until an earlier Ready hole closes. The count and
    /// dense acknowledgement use a SeqCst registration/recheck handshake, so
    /// a hole-closing publisher can skip the mutex and futex wake when every
    /// later publisher is still spinning.
    acknowledgement_waiters: CacheLineAtomicUsize,
    /// Dense prefix of records whose producers have published a known-success
    /// native outcome. Healthy in-order publication advances this without the
    /// queue-state mutex.
    acknowledged: CacheLineAtomicU64,
    /// Latest sequence returned by each process-lifetime native worker.
    ///
    /// Trusted one-Put publishers touch only their own cache line after
    /// publishing READY. Barriers Acquire-scan these high-water marks; the
    /// backend consumer remains responsible for advancing the dense prefix.
    trusted_caller_ack_by_worker: Box<[CacheLineAtomicU64]>,
    /// Dense prefix whose arena/cold storage is no longer read by the sole
    /// consumer. SPSC producers Acquire this only when refreshing their local
    /// capacity cursor; it is the cross-generation storage-reuse handoff.
    applied_frontier: CacheLineAtomicU64,
    /// Legacy Rust-side sequence tail retained by the same-build ABI. Packed
    /// concurrent terminals neither allocate from nor update this word;
    /// descriptor discovery probes exact ring generations instead. Its own
    /// cache line keeps legacy traffic away from occupancy and acknowledgement
    /// words touched by other producers.
    next_bound: CacheLineAtomicU64,
    /// Maximum aggregate rights acquired by one packed concurrent refill.
    /// One preserves scalar occupancy behavior for small queues.
    packed_occupancy_credit_batch: usize,
    /// Idle occupancy rights owned by each process-lifetime writer slot.
    ///
    /// The slot's sole worker consumes its isolated line. Capacity pressure
    /// may steal idle rights, but the process-lifetime slot allocator never
    /// recycles a departed thread's index and this allocation belongs to only
    /// one Writeback/cache namespace.
    packed_occupancy_credits_by_worker: Box<[CacheLineAtomicUsize]>,
    /// Detached plus bound transactions. Successful backend retirement and
    /// pre-bind cancellation release active rights; packed-credit reclamation
    /// releases idle rights which never became a transaction.
    occupied: Occupancy,
    /// Recycled allocations used only by records larger than the fixed arena.
    oversized_pool: OversizedPool,
    publication_shift: u32,
    /// Fixed-address cells indexed by commit sequence modulo the power-of-two
    /// ring length. The fixed allocation makes raw cell access stable across
    /// every `VecDeque` front retirement and reuse.
    publication_cells: Box<[PublicationCell]>,
    /// Cold ownership paired one-for-one with the hot publication ring.
    cold_publication_cells: Box<[ColdPublicationCell]>,
    /// Scalar descriptions for common records published through the direct
    /// SPSC frontier. The consumer never writes these cells.
    spsc_arena_publications: Box<[SpscArenaPublication]>,
    /// Declared after `state`, occupancy metadata, and both publication arrays
    /// so every queued or attached arena-backed record drops before its bytes.
    native_arena: NativeRecordArena,
    /// Declared last so every queued holder descriptor and consumer borrow is
    /// gone before pool destruction. A fail-stopped sealed generation makes
    /// the native RAII wrapper intentionally leak rather than free live spans.
    native_holder_pool: Option<TrustedOnePutHolderPool>,
}

impl<B: Blobs> Writeback<B> {
    /// Create an empty queue whose first reservation is `applied_seed + 1`.
    ///
    /// This constructor is used by tests that only need a sequence seed. Cache
    /// recovery uses [`Self::new_with_watermark`] to retain the recovered Mako
    /// timestamp as well.
    #[cfg(test)]
    pub fn new(
        backend: B,
        applied_seed: u64,
        config: WritebackConfig,
    ) -> Result<Self, ConfigError> {
        Self::new_with_watermark(
            backend,
            AppliedWatermark::recovered(applied_seed, None),
            config,
        )
    }

    #[cfg(test)]
    fn new_single(
        backend: B,
        applied_seed: u64,
        config: WritebackConfig,
    ) -> Result<Self, ConfigError> {
        Self::new_with_watermark_mode(
            backend,
            AppliedWatermark::recovered(applied_seed, None),
            config,
            true,
        )
    }

    /// Create a queue from progress reconstructed while opening the backend.
    pub(crate) fn new_with_watermark(
        backend: B,
        applied_seed: AppliedWatermark,
        config: WritebackConfig,
    ) -> Result<Self, ConfigError> {
        Self::new_with_watermark_mode(backend, applied_seed, config, false)
    }

    /// Construct either the ordinary MPMC queue or the permanently selected
    /// single-producer accounting profile used by [`crate::SingleProducer`].
    pub(crate) fn new_with_watermark_mode(
        backend: B,
        applied_seed: AppliedWatermark,
        config: WritebackConfig,
        single_producer: bool,
    ) -> Result<Self, ConfigError> {
        if config.capacity == 0 {
            return Err(ConfigError::ZeroCapacity);
        }
        if config.max_record_bytes == 0 {
            return Err(ConfigError::ZeroRecordBudget);
        }
        if config.max_batch_records == 0 {
            return Err(ConfigError::ZeroBatchRecords);
        }
        if config.max_batch_bytes == 0 {
            return Err(ConfigError::ZeroBatchBytes);
        }
        if config.retry_delay.is_zero() {
            return Err(ConfigError::ZeroRetryDelay);
        }
        if applied_seed.sequence == u64::MAX {
            return Err(ConfigError::SequenceExhausted);
        }

        // A power-of-two ring lets the publication hot path map a sequence to
        // its stable cell with one mask instead of a hardware integer divide.
        // Extra cells never grant queue capacity and therefore do not change
        // boundedness or reuse safety.
        let publication_capacity = config
            .capacity
            .max(1 << TURN_PHASE_BITS)
            .checked_next_power_of_two()
            .ok_or(ConfigError::NativeRecordArenaTooLarge)?;
        let publication_shift = publication_capacity.trailing_zeros();
        let publication_mask = publication_capacity - 1;
        let first_sequence = applied_seed
            .sequence
            .checked_add(1)
            .expect("the maximum applied seed was rejected above");
        let first_index = (first_sequence as usize) & publication_mask;
        let publication_cells = (0..publication_capacity)
            .map(|index| {
                let delta = index.wrapping_sub(first_index) & publication_mask;
                let initial_free_turn = u64::try_from(delta)
                    .ok()
                    .and_then(|delta| first_sequence.checked_add(delta))
                    .map(|sequence| turn_token(sequence, publication_shift, TURN_FREE))
                    // A near-u64::MAX ring position with no future generation
                    // is unreachable and may retain any terminal token.
                    .unwrap_or(u64::MAX);
                PublicationCell::free(initial_free_turn)
            })
            .collect::<Vec<_>>()
            .into_boxed_slice();
        let cold_publication_cells = (0..publication_capacity)
            .map(|_| ColdPublicationCell::empty())
            .collect::<Vec<_>>()
            .into_boxed_slice();
        let spsc_arena_publications = (0..publication_capacity)
            .map(|_| SpscArenaPublication::empty())
            .collect::<Vec<_>>()
            .into_boxed_slice();
        let native_arena = NativeRecordArena::new(publication_capacity, config.max_record_bytes)?;
        let native_holder_pool = Some(
            TrustedOnePutHolderPool::new(publication_capacity, 0, 0)
                .map_err(|_| ConfigError::NativeHolderPool)?,
        );
        let packed_occupancy_credit_batch = if single_producer {
            1
        } else {
            packed_occupancy_credit_batch_size(config.capacity)
        };
        let packed_occupancy_credits_by_worker = if single_producer {
            Vec::new().into_boxed_slice()
        } else {
            (0..mako_local::MAX_WORKERS)
                .map(|_| CacheLineAtomicUsize::new(0))
                .collect::<Vec<_>>()
                .into_boxed_slice()
        };

        Ok(Self {
            backend,
            config,
            single_producer,
            state: Mutex::new(State {
                queue: VecDeque::with_capacity(config.capacity),
                last_bound: applied_seed.sequence,
                applied: applied_seed,
                first_unknown: None,
                permanent_record_failure: None,
            }),
            changed: Condvar::new(),
            acknowledgement_changed: Condvar::new(),
            descriptor_available: Condvar::new(),
            capacity_available: Condvar::new(),
            capacity_waiters: AtomicUsize::new(0),
            descriptor_waiters: AtomicUsize::new(0),
            activity_waiters: AtomicUsize::new(0),
            consumer: Mutex::new(()),
            unhealthy: AtomicBool::new(false),
            acknowledgement_waiters: CacheLineAtomicUsize::new(0),
            acknowledged: CacheLineAtomicU64::new(applied_seed.sequence),
            trusted_caller_ack_by_worker: (0..mako_local::MAX_WORKERS)
                .map(|_| CacheLineAtomicU64::new(applied_seed.sequence))
                .collect::<Vec<_>>()
                .into_boxed_slice(),
            applied_frontier: CacheLineAtomicU64::new(applied_seed.sequence),
            next_bound: CacheLineAtomicU64::new(applied_seed.sequence),
            packed_occupancy_credit_batch,
            packed_occupancy_credits_by_worker,
            occupied: Occupancy::empty(),
            oversized_pool: OversizedPool::new(config.capacity)?,
            publication_shift,
            publication_cells,
            cold_publication_cells,
            spsc_arena_publications,
            native_arena,
            native_holder_pool,
        })
    }

    /// Access the underlying backend.
    pub fn backend(&self) -> &B {
        &self.backend
    }

    /// Maximum native/log record extent accepted by this queue.
    pub(crate) const fn max_record_bytes(&self) -> usize {
        self.config.max_record_bytes
    }

    #[inline(always)]
    fn packed_occupancy_credit_slot(&self, worker_slot: usize) -> &CacheLineAtomicUsize {
        self.packed_occupancy_credits_by_worker
            .get(worker_slot)
            .expect("packed occupancy-credit worker slot is in range")
    }

    /// Transfer one idle right from this worker's private line to its returned
    /// permit. Reclamation is the only concurrent writer of the line.
    ///
    /// Relaxed is sufficient: the atomic modification order decides whether
    /// this CAS or a reclaiming swap owns each numeric right. Ring-turn
    /// Acquire/Release transitions independently transfer record storage, and
    /// the packed native word independently orders read-only history.
    #[inline(always)]
    fn try_take_packed_occupancy_credit(&self, worker_slot: usize) -> bool {
        let credit = self.packed_occupancy_credit_slot(worker_slot);
        let mut current = credit.load(Ordering::Relaxed);
        loop {
            if current == 0 {
                return false;
            }
            match credit.compare_exchange_weak(
                current,
                current - 1,
                Ordering::Relaxed,
                Ordering::Relaxed,
            ) {
                Ok(_) => return true,
                Err(observed) => current = observed,
            }
        }
    }

    /// Acquire the current permit plus a bounded set of idle owner-local
    /// rights. Only the owner can refill its line, and it reaches this method
    /// only after observing zero. The Release store and a reclaimer's Acquire
    /// swap keep the preceding aggregate batch claim before any subtraction of
    /// those idle rights. A pressure scan which linearizes before this store
    /// either claims remaining scalar capacity or fails and scans again.
    #[inline(always)]
    fn try_refill_packed_occupancy_credits(&self, worker_slot: usize) -> bool {
        let credit = self.packed_occupancy_credit_slot(worker_slot);
        debug_assert_eq!(credit.load(Ordering::Relaxed), 0);
        let claimed = self
            .occupied
            .try_claim_batch_relaxed(self.packed_occupancy_credit_batch, self.config.capacity);
        if claimed == 0 {
            return false;
        }
        if claimed != 1 {
            credit.store(claimed - 1, Ordering::Release);
        }
        true
    }

    #[inline(always)]
    fn try_claim_packed_occupancy(&self, worker_slot: usize) -> bool {
        if self.packed_occupancy_credit_batch == 1 {
            return self.occupied.try_claim(self.config.capacity);
        }
        self.try_take_packed_occupancy_credit(worker_slot)
            || self.try_refill_packed_occupancy_credits(worker_slot)
    }

    /// Steal every currently published idle right and return it to aggregate
    /// occupancy. Each Acquire swap which observes a nonzero Release-published
    /// credit also keeps its aggregate batch claim before the subtraction;
    /// the slot's atomic modification order divides ownership with worker CASes.
    fn reclaim_all_packed_occupancy_credits(&self) -> usize {
        if self.packed_occupancy_credit_batch == 1 {
            return 0;
        }
        let reclaimed = self
            .packed_occupancy_credits_by_worker
            .iter()
            .map(|credit| credit.swap(0, Ordering::Acquire))
            .try_fold(0usize, usize::checked_add)
            .expect("packed occupancy-credit sum cannot exceed configured capacity");
        if reclaimed != 0 {
            self.occupied.release_many(reclaimed);
        }
        reclaimed
    }

    /// Return idle packed rights before exclusive cache shutdown.
    ///
    /// Safe `Cache::close`, `abort_without_flush`, and final Drop own the cache
    /// value, so no foreground transaction can refill after this one-shot scan.
    pub(crate) fn reclaim_packed_occupancy_credits_for_shutdown(&self) {
        self.reclaim_all_packed_occupancy_credits();
    }

    /// Stable atomic words borrowed by the same-build native ordering seam.
    ///
    /// Native accesses these addresses only during one synchronous terminal.
    /// It Acquire-loads `unhealthy`; packed native state is the sole concurrent
    /// allocator. `next_bound` is retained as a stable compatibility field but
    /// concurrent terminals do not read or update its value. Both Rust
    /// allocations must remain live for that whole call.
    #[inline(always)]
    pub(crate) const fn native_ordering_words(&self) -> (&AtomicU64, &AtomicBool) {
        (self.next_bound.atomic(), &self.unhealthy)
    }

    /// Borrow the stable ring layout used by the callback-free native binder.
    ///
    /// The returned control is valid only while this write-back queue remains
    /// alive. Native uses its packed pair CAS to assign one sequence, acquires
    /// that generation's exact FREE turn, publishes BOUND, and serializes into
    /// the matching arena block. Exact-turn probing, not `next_bound`, makes
    /// the descriptor discoverable.
    ///
    /// # Safety
    ///
    /// The caller must retain one concurrent detached occupancy claim and may
    /// pass this control only to the matching same-build trusted one-Put
    /// terminal. The LocalDb must hold an immutable Concurrent cache-order
    /// claim, and every cache-record terminal must use that packed namespace.
    #[inline(always)]
    pub(crate) unsafe fn native_ordered_arena_control(
        &self,
    ) -> mako_local::TrustedNativeOrderedArenaControl {
        debug_assert!(!self.single_producer);
        debug_assert!(self.publication_cells.len().is_power_of_two());
        let publication_stride = u32::try_from(std::mem::size_of::<PublicationCell>())
            .expect("the native publication stride fits u32");
        let arena_stride = u32::try_from(std::mem::size_of::<NativeRecordArenaBlock>())
            .expect("the native arena stride fits u32");
        let arena_block_bytes = u32::try_from(self.native_arena.block_bytes())
            .expect("the native arena block extent fits u32");
        // SAFETY: both boxed arrays have stable addresses for `self`'s
        // lifetime, their exact layouts are asserted above, and the terminal
        // borrows every pointer only for its synchronous call.
        unsafe {
            mako_local::TrustedNativeOrderedArenaControl::from_raw_parts(
                self.next_bound.as_ptr(),
                self.unhealthy.as_ptr().cast::<u8>(),
                self.publication_cells.as_ptr().cast_mut().cast::<u8>(),
                self.native_arena.blocks.as_ptr().cast_mut().cast::<u8>(),
                self.publication_cells.len() - 1,
                self.publication_shift,
                publication_stride,
                arena_stride,
                arena_block_bytes,
            )
        }
    }

    /// Borrow the stable publication layout and deferred holder pool used by
    /// the callback-free concurrent one-Put terminal.
    ///
    /// # Safety
    ///
    /// The caller must retain one concurrent detached occupancy claim and pass
    /// this control only to the matching packed native holder terminal. This
    /// queue, pool, and every pointer in the returned value must outlive that
    /// synchronous call.
    #[inline(always)]
    pub(crate) unsafe fn native_ordered_holder_control(
        &self,
    ) -> mako_local::TrustedNativeOrderedHolderControl {
        debug_assert!(!self.single_producer);
        let publication_stride = u32::try_from(std::mem::size_of::<PublicationCell>())
            .expect("the native publication stride fits u32");
        // The publication descriptor reserves usize's high bit as its holder
        // tag. On 32-bit targets, keep larger valid records on the byte-arena
        // terminal instead of admitting an ambiguous tagged extent.
        let holder_record_limit = NATIVE_HOLDER_RECORD_TAG - 1;
        let max_record_bytes = u32::try_from(self.config.max_record_bytes.min(holder_record_limit))
            .unwrap_or(u32::MAX);
        // SAFETY: queue construction gives the pool and publication ring the
        // same power-of-two capacity and stable allocation lifetime.
        unsafe {
            mako_local::TrustedNativeOrderedHolderControl::new(
                self.native_holder_pool
                    .as_ref()
                    .expect("every queue owns a native holder pool"),
                self.unhealthy.as_ptr().cast::<u8>(),
                self.publication_cells.as_ptr().cast_mut().cast::<u8>(),
                self.publication_cells.len() - 1,
                self.publication_shift,
                publication_stride,
                max_record_bytes,
            )
        }
    }

    pub(crate) fn native_holder_pool(&self) -> Option<&TrustedOnePutHolderPool> {
        self.native_holder_pool.as_ref()
    }

    /// Return the holder pool whose presence is guaranteed by SPSC creation.
    ///
    /// # Safety
    ///
    /// This write-back queue must have been constructed in single-producer
    /// mode and must remain live through every native use of the returned pool.
    #[inline(always)]
    pub(crate) unsafe fn native_holder_pool_single_unchecked(&self) -> &TrustedOnePutHolderPool {
        debug_assert!(self.single_producer);
        debug_assert!(self.native_holder_pool.is_some());
        // SAFETY: required by this method's construction-mode contract.
        unsafe { self.native_holder_pool.as_ref().unwrap_unchecked() }
    }

    /// Stable address of the SPSC published tail for a future trusted native
    /// publication terminal.
    ///
    /// Merely obtaining the pointer is safe. Any foreign write through it is
    /// unsafe and must be one naturally aligned atomic Release store by the
    /// unique foreground producer, after the exact arena bytes and scalar
    /// publication descriptor have been sealed. The enclosing `Writeback`
    /// allocation must remain alive until foreign code has returned.
    #[allow(dead_code)]
    pub(crate) fn spsc_published_tail_ptr(&self) -> *mut u64 {
        assert!(
            self.single_producer,
            "the direct published-tail address is SPSC-only"
        );
        self.acknowledged.as_ptr()
    }

    /// Initialize one producer-local capacity cursor for an exclusive cache
    /// lease. Lease acquisition is cold, so reading the state-locked applied
    /// frontier here does not burden ordinary commits.
    pub(crate) fn single_producer_state(&self) -> SingleProducerState {
        assert!(
            self.single_producer,
            "single-producer state requires a single-producer queue"
        );
        let mut state = lock_recover(&self.state);
        self.import_bound_locked(&mut state);
        let acknowledged = self.acknowledged.load(Ordering::Acquire);
        debug_assert!(state.last_bound >= acknowledged);
        let capacity = NonZeroU64::new(
            u64::try_from(self.config.capacity).expect("queue capacity fits the native u64 ABI"),
        )
        .expect("validated queue capacity is nonzero");
        // The native candidate itself is a u32. A larger configured cap should
        // admit every representable fused candidate rather than truncate or
        // reject queue construction.
        let max_record_bytes =
            NonZeroU32::new(u32::try_from(self.config.max_record_bytes).unwrap_or(u32::MAX))
                .expect("validated record cap is nonzero");
        // SAFETY: a claimed SingleProducer holds `self` immovably borrowed for
        // the state's lifetime. These atomic fields and the SPSC holder pool
        // therefore remain stable, and that capability is the sole foreground
        // terminal. The physical power-of-two holder ring covers at least the
        // logical configured capacity used by native's fullness predicate.
        let fused_holder_control = unsafe {
            mako_local::TrustedSpscOnePutHolderControl::new(
                self.native_holder_pool
                    .as_ref()
                    .expect("single-producer queue owns a native holder pool"),
                self.acknowledged.as_ptr(),
                self.unhealthy.as_ptr().cast::<u8>(),
                capacity,
                max_record_bytes,
            )
        };
        SingleProducerState::new(
            state.applied.sequence,
            state.last_bound,
            capacity.get(),
            fused_holder_control,
        )
    }

    fn publication_cell(&self, sequence: CommitSeq) -> &PublicationCell {
        debug_assert!(self.publication_cells.len().is_power_of_two());
        // Truncating on a 32-bit target retains exactly the low bits selected
        // by this representable power-of-two ring.
        let index = (sequence.get() as usize) & (self.publication_cells.len() - 1);
        &self.publication_cells[index]
    }

    fn publication_index(&self, sequence: CommitSeq) -> usize {
        (sequence.get() as usize) & (self.publication_cells.len() - 1)
    }

    fn cold_publication_cell(&self, sequence: CommitSeq) -> &ColdPublicationCell {
        &self.cold_publication_cells[self.publication_index(sequence)]
    }

    fn spsc_arena_publication(&self, sequence: CommitSeq) -> &SpscArenaPublication {
        &self.spsc_arena_publications[self.publication_index(sequence)]
    }

    /// Whether the acquired dense published frontier exposes this exact SPSC
    /// arena generation. A false result selects the cold publication cell.
    fn has_spsc_arena_publication(&self, sequence: CommitSeq, published: u64) -> bool {
        if !self.single_producer || sequence.get() > published {
            return false;
        }
        // SAFETY: `published` came from an Acquire load of `acknowledged` and
        // covers this sequence. Applied-frontier reuse prevents a concurrent
        // rewrite while the state-locked consumer retains the generation.
        unsafe { self.spsc_arena_publication(sequence).contains(sequence) }
    }

    /// Reconstruct a directly published holder from its exact native
    /// generation. The holder itself is the descriptor: native sealed its
    /// sequence, timestamp, and lengths before the producer's ACK Release, so
    /// the foreground need not stream a second Rust descriptor cache line.
    fn harvest_spsc_holder(
        &self,
        sequence: CommitSeq,
        published: u64,
    ) -> Option<QueuedCommitRecord> {
        if !self.single_producer || sequence.get() > published {
            return None;
        }
        let pool = self.native_holder_pool.as_ref()?;
        let raw = NonZeroU64::new(sequence.get()).expect("cache commit sequences are nonzero");
        // SAFETY: the caller Acquired `published`, the sole consumer invokes
        // this helper under `state`, and applied has not advanced across this
        // exact generation. The view is dropped after copying scalars; holder
        // bytes remain sealed for later materialization.
        let view = unsafe { pool.view(raw) }.ok()?;
        let exact_record_bytes =
            DeferredOnePutRecord::encoded_len_for(view.key().len(), view.value().len()).ok()?;
        Some(QueuedCommitRecord::Holder(DeferredOnePutRecord::new(
            sequence,
            view.mako_timestamp(),
            exact_record_bytes,
        )))
    }

    fn is_record_published(&self, sequence: CommitSeq, ordering: Ordering) -> bool {
        let published = self.acknowledged.load(ordering);
        self.has_spsc_arena_publication(sequence, published)
            || self.harvest_spsc_holder(sequence, published).is_some()
            || self
                .publication_cell(sequence)
                .is_published(sequence, self.publication_shift)
    }

    /// Import every completely published dense bind descriptor into the
    /// existing state-locked queue. A producer that has allocated the next
    /// sequence but not yet Release-published its descriptor is a temporary
    /// ordering hole; the importer simply stops and a later state entrant
    /// resumes at the same sequence.
    fn import_bound_locked(&self, state: &mut State) {
        loop {
            let Some(raw_sequence) = state.last_bound.checked_add(1) else {
                return;
            };
            let sequence = CommitSeq::new(raw_sequence)
                .expect("the sequence after a valid applied seed is nonzero");
            // In SPSC mode a known-success direct arena record has no per-cell
            // lifecycle traffic. Its one Release-published dense frontier is
            // simultaneously the bind, Ready, and caller-ACK witness. Cold
            // general/oversized and unknown paths retain the old cell state.
            let direct_spsc =
                self.single_producer && raw_sequence <= self.acknowledged.load(Ordering::Acquire);
            if !direct_spsc
                && !self
                    .publication_cell(sequence)
                    .is_bound(sequence, self.publication_shift)
            {
                return;
            }
            assert!(
                state.queue.len() < self.config.capacity,
                "bounded permits prevent bind-descriptor queue overflow"
            );
            assert!(
                state.queue.len() < state.queue.capacity(),
                "the ordered queue was preallocated to bounded capacity"
            );
            state.queue.push_back(Slot {
                sequence,
                record: None,
                state: SlotState::Prepared { pinned: false },
            });
            state.last_bound = raw_sequence;
        }
    }

    /// Resolve a token after importing any dense descriptors available now.
    ///
    /// Concurrent producers can allocate adjacent sequences and briefly
    /// publish their descriptors out of order. A resolver for the suffix must
    /// not misclassify that temporary hole as a missing slot. Registration and
    /// the predicate recheck happen under `state`; a producer only takes that
    /// mutex when a waiter exists, closing the ordinary notification gap
    /// without adding a mutex operation to the ordinary bind path. A short
    /// timeout is the fail-safe against a stale advisory waiter-count load on
    /// weak memory: descriptor readiness itself remains the sole predicate.
    fn wait_for_bound_token_locked<'a>(
        &'a self,
        mut state: MutexGuard<'a, State>,
        token: QueueToken,
    ) -> (MutexGuard<'a, State>, Option<usize>) {
        loop {
            self.import_bound_locked(&mut state);
            if let Some(offset) = state.queue_offset(token) {
                return (state, Some(offset));
            }
            if state.applied.sequence >= token.sequence().get() {
                return (state, None);
            }

            self.descriptor_waiters.fetch_add(1, Ordering::AcqRel);
            self.import_bound_locked(&mut state);
            if let Some(offset) = state.queue_offset(token) {
                self.descriptor_waiters.fetch_sub(1, Ordering::AcqRel);
                return (state, Some(offset));
            }
            state =
                wait_timeout_recover(&self.descriptor_available, state, Duration::from_millis(1));
            let prior = self.descriptor_waiters.fetch_sub(1, Ordering::AcqRel);
            debug_assert_ne!(prior, 0, "descriptor waiter registration underflow");
        }
    }

    fn notify_descriptor_waiters(&self) {
        if self.descriptor_waiters.load(Ordering::Acquire) != 0 {
            let state = lock_recover(&self.state);
            self.descriptor_available.notify_all();
            drop(state);
        }
    }

    /// Advance the dense acknowledgement prefix across every atomically
    /// published successor. Multiple out-of-order publishers may help one
    /// another; the CAS makes advancement linearizable without queue locking.
    fn advance_atomic_acknowledgement_once(
        &self,
        mut acknowledged: u64,
        maximum: u64,
    ) -> u64 {
        loop {
            if acknowledged >= maximum {
                return acknowledged;
            }
            let Some(raw_next) = acknowledged.checked_add(1) else {
                return acknowledged;
            };
            let Some(next) = CommitSeq::new(raw_next) else {
                return acknowledged;
            };
            if !self
                .publication_cell(next)
                .is_published(next, self.publication_shift)
            {
                return acknowledged;
            }
            match self.acknowledged.compare_exchange_weak(
                acknowledged,
                raw_next,
                // This frontier also participates in the activity-waiter
                // Dekker handshake. SeqCst keeps either the waiter's
                // post-registration predicate read or the publisher's waiter
                // count read from missing the other side.
                Ordering::SeqCst,
                Ordering::SeqCst,
            ) {
                Ok(_) => acknowledged = raw_next,
                Err(observed) => acknowledged = observed,
            }
        }
    }

    /// Sweep the dense READY prefix.
    ///
    /// Trusted publishers deliberately do not call this helper. The sole
    /// consumer and general publishers close the prefix, so the trusted
    /// foreground terminal has no shared acknowledgement cache line.
    fn advance_atomic_acknowledgement(&self, acknowledged: u64, maximum: u64) -> u64 {
        self.advance_atomic_acknowledgement_once(acknowledged, maximum)
    }

    /// Move one atomically published record into its ordinary queue slot.
    /// The caller holds `state`, so this is the unique harvester and the record
    /// can subsequently follow the existing retry/recycle state machine.
    fn harvest_published_slot_locked(&self, state: &mut State, offset: usize) -> bool {
        let slot = &mut state.queue[offset];
        if slot.state != (SlotState::Prepared { pinned: false }) {
            return false;
        }
        let acknowledged = self.acknowledged.load(Ordering::Acquire);
        // SAFETY: `state` serializes the sole Prepared -> Ready transition.
        // In SPSC mode the Acquire above covers the direct publication's
        // scalar metadata and arena bytes; a generation mismatch selects the
        // established publication-cell fallback.
        let direct_arena = if self.has_spsc_arena_publication(slot.sequence, acknowledged) {
            unsafe {
                self.spsc_arena_publication(slot.sequence).harvest(
                    slot.sequence,
                    &self.native_arena,
                    self.publication_index(slot.sequence),
                )
            }
        } else {
            None
        };
        let direct = direct_arena.or_else(|| self.harvest_spsc_holder(slot.sequence, acknowledged));
        let record = direct.or_else(|| unsafe {
            self.publication_cell(slot.sequence).harvest(
                self.cold_publication_cell(slot.sequence),
                slot.sequence,
                self.publication_shift,
                &self.native_arena,
                self.publication_index(slot.sequence),
            )
        });
        let Some(record) = record else {
            return false;
        };
        assert!(slot.record.replace(record).is_none());
        slot.state = SlotState::Ready;
        true
    }

    /// Materialize atomic publication metadata for the acknowledged queue
    /// prefix before the state-locked consumer inspects it.
    fn harvest_acknowledged_locked(&self, state: &mut State) {
        self.import_bound_locked(state);
        let acknowledged = self.acknowledged.load(Ordering::Acquire);
        let harvest_limit = state.queue.len().min(self.config.max_batch_records);
        for offset in 0..harvest_limit {
            let sequence = state.queue[offset].sequence;
            if sequence.get() > acknowledged {
                break;
            }
            if state.queue[offset].state == (SlotState::Prepared { pinned: false }) {
                assert!(
                    self.harvest_published_slot_locked(state, offset),
                    "an acknowledged slot must expose its complete publication"
                );
            }
            if state.queue[offset].state != SlotState::Ready {
                break;
            }
        }
    }

    /// Prepare a complete transaction record and claim capacity before native
    /// commit, without assigning a sequence or inserting an ordered log slot.
    ///
    /// Capacity counts both detached permits and queued slots. Record encoding
    /// and key construction finish after capacity is claimed but before this
    /// method returns. Consequently
    /// [`DetachedPermit::bind`] can run inside Silo's post-validation hook
    /// without allocation, waiting for capacity, or touching the backend.
    pub fn reserve(&self, mutations: Vec<Mutation>) -> Result<DetachedPermit<'_, B>, ReserveError> {
        assert!(
            !self.single_producer,
            "a single-producer queue requires its lease-owned reserve path"
        );
        self.reserve_inner(mutations, None, None)
    }

    #[cfg(test)]
    fn reserve_single<'a>(
        &'a self,
        producer: &'a SingleProducerState,
        mutations: Vec<Mutation>,
    ) -> Result<DetachedPermit<'a, B>, ReserveError> {
        let sequence = self.claim_detached_capacity_single(producer)?;
        self.reserve_inner(mutations, Some(sequence), Some(producer))
    }

    fn reserve_inner<'a>(
        &'a self,
        mutations: Vec<Mutation>,
        single_sequence: Option<CommitSeq>,
        single_producer: Option<&'a SingleProducerState>,
    ) -> Result<DetachedPermit<'a, B>, ReserveError> {
        if single_sequence.is_none() {
            self.claim_detached_capacity_concurrent()?;
        }

        // Build the returned value first so unwinding or a preparation error
        // releases the detached claim through Drop.
        let mut permit = DetachedPermit {
            owner: self,
            prepared: None,
            native_record: false,
            native_buffer: None,
            single_sequence,
            single_producer,
            owns_claim: true,
        };
        permit.prepared = Some(Box::new(LegacyCommitRecord::prepare(
            mutations,
            self.config.max_record_bytes,
        )?));
        Ok(permit)
    }

    /// Claim queue capacity for a record that the trusted native transaction
    /// adapter will serialize directly.
    ///
    /// The exact byte buffer is owned by `mako-local` across the synchronous
    /// native commit call. This permit owns only the bounded queue slot and
    /// therefore adds no duplicate write-set representation.
    pub(crate) fn reserve_native(
        &self,
        record_bytes: usize,
    ) -> Result<DetachedPermit<'_, B>, ReserveError> {
        assert!(
            !self.single_producer,
            "a single-producer queue requires its lease-owned native reserve"
        );
        self.reserve_native_inner(record_bytes, None)
    }

    pub(crate) fn reserve_native_single<'a>(
        &'a self,
        producer: &'a SingleProducerState,
        record_bytes: usize,
    ) -> Result<DetachedPermit<'a, B>, ReserveError> {
        self.reserve_native_inner(record_bytes, Some(producer))
    }

    fn reserve_native_inner<'a>(
        &'a self,
        record_bytes: usize,
        producer: Option<&'a SingleProducerState>,
    ) -> Result<DetachedPermit<'a, B>, ReserveError> {
        if record_bytes == 0 {
            return Err(ReserveError::Record(RecordError::Truncated));
        }
        if record_bytes > self.config.max_record_bytes {
            return Err(ReserveError::Record(RecordError::RecordTooLarge {
                size: record_bytes,
                max: self.config.max_record_bytes,
            }));
        }
        let (single_sequence, buffer) =
            self.claim_detached_native_buffer(record_bytes, producer)?;
        let mut permit = DetachedPermit {
            owner: self,
            prepared: None,
            native_record: true,
            native_buffer: None,
            single_sequence,
            single_producer: producer,
            owns_claim: true,
        };
        // Arena buffers are already exact. Oversized recycled buffers may need
        // to grow, but this happens before native takes validation/write locks.
        permit.native_buffer = Some(match buffer {
            NativeRecordBuffer::UnboundArena { .. } => buffer,
            NativeRecordBuffer::Arena { .. } => buffer,
            NativeRecordBuffer::Owned(bytes) => {
                NativeRecordBuffer::prepare_owned(bytes, record_bytes)?
            }
        });
        Ok(permit)
    }

    /// Claim the allocation-free common arena representation used by the
    /// trusted one-Put terminal.
    ///
    /// `Ok(None)` means the exact record needs the generic oversized-buffer
    /// path. No capacity is claimed in that case.
    pub(crate) fn reserve_native_arena_fast(
        &self,
        record_bytes: usize,
    ) -> Result<Option<NativeArenaPermit<'_, B>>, ReserveError> {
        assert!(
            !self.single_producer,
            "a single-producer queue requires its lease-owned arena reserve"
        );
        self.reserve_native_arena_fast_inner(record_bytes, None)
    }

    /// Claim the packed concurrent one-Put arena using this worker's isolated
    /// occupancy-credit line.
    ///
    /// Record validation intentionally matches [`Self::reserve_native_arena_fast`].
    /// Only the capacity accounting differs; general, legacy, oversized, and
    /// single-producer transactions retain their scalar reservation paths.
    #[inline(always)]
    pub(crate) fn reserve_native_arena_fast_packed(
        &self,
        record_bytes: usize,
        worker_slot: usize,
    ) -> Result<Option<NativeArenaPermit<'_, B>>, ReserveError> {
        assert!(
            !self.single_producer,
            "packed occupancy credits require a concurrent queue"
        );
        if record_bytes == 0 {
            return Err(ReserveError::Record(RecordError::Truncated));
        }
        if record_bytes > self.config.max_record_bytes {
            return Err(ReserveError::Record(RecordError::RecordTooLarge {
                size: record_bytes,
                max: self.config.max_record_bytes,
            }));
        }
        if record_bytes > self.native_arena.block_bytes() {
            return Ok(None);
        }
        self.claim_detached_capacity_packed(worker_slot)?;
        Ok(Some(NativeArenaPermit {
            owner: self,
            exact_record_bytes: record_bytes,
            single_sequence: None,
            single_producer: None,
            owns_claim: true,
        }))
    }

    pub(crate) fn reserve_native_arena_fast_single<'a>(
        &'a self,
        producer: &'a SingleProducerState,
        record_bytes: usize,
    ) -> Result<Option<NativeArenaPermit<'a, B>>, ReserveError> {
        self.reserve_native_arena_fast_inner(record_bytes, Some(producer))
    }

    /// Try to retain one exact SPSC sequence using only producer-local words.
    ///
    /// The returned nonzero scalar is the complete reservation: unlike the
    /// concurrent queue, an SPSC reservation mutates no shared state until
    /// native accepts the transaction. Consequently an OCC loser needs no
    /// permit Drop or cancellation write. `None` selects the cold health,
    /// capacity-refresh, or sequence-exhaustion path.
    ///
    /// # Safety
    ///
    /// `producer` must be the live unique lease for this single-producer
    /// queue, and `record_bytes` must already have been checked against this
    /// queue's record limit. The caller must perform no other foreground
    /// terminal until it publishes, pins, or abandons the returned generation
    /// after a definite pre-acceptance abort.
    #[inline(always)]
    pub(crate) unsafe fn try_reserve_native_holder_single(
        &self,
        producer: &SingleProducerState,
        record_bytes: NonZeroU32,
    ) -> Option<NonZeroU64> {
        debug_assert!(self.single_producer);
        debug_assert!((record_bytes.get() as usize) <= self.config.max_record_bytes);

        if self.unhealthy.load(Ordering::Acquire) {
            return None;
        }
        let tail = producer.next_sequence.load(Ordering::Relaxed);
        let capacity_limit = producer.capacity_limit.load(Ordering::Relaxed);
        if tail >= capacity_limit {
            return None;
        }

        // SAFETY: `tail != u64::MAX` proves the addition cannot wrap, and its
        // successor is nonzero. The unique producer retains this generation
        // without modifying either cursor until native acceptance.
        Some(unsafe { NonZeroU64::new_unchecked(tail + 1) })
    }

    /// Resolve a holder reservation which missed the producer-local fast path.
    ///
    /// This intentionally returns the rich queue error only out of line. The
    /// ordinary ACK path uses [`Self::try_reserve_native_holder_single`] and
    /// never constructs or moves a `Result<_, ReserveError>`.
    #[cold]
    #[inline(never)]
    pub(crate) fn reserve_native_holder_single_slow(
        &self,
        producer: &SingleProducerState,
        record_bytes: NonZeroU32,
    ) -> Result<NonZeroU64, ReserveError> {
        let record_bytes = record_bytes.get() as usize;
        if record_bytes > self.config.max_record_bytes {
            return Err(ReserveError::Record(RecordError::RecordTooLarge {
                size: record_bytes,
                max: self.config.max_record_bytes,
            }));
        }
        let capacity = u64::try_from(self.config.capacity).unwrap_or(u64::MAX);
        let sequence = self.claim_detached_capacity_single_slow(producer, capacity)?;
        // SAFETY: CommitSeq is nonzero by construction.
        Ok(unsafe { NonZeroU64::new_unchecked(sequence.get()) })
    }

    /// Checked convenience reserve used by protocol tests and cold callers.
    #[cfg(test)]
    pub(crate) fn reserve_native_holder_single(
        &self,
        producer: &SingleProducerState,
        record_bytes: NonZeroU32,
    ) -> Result<NonZeroU64, ReserveError> {
        // SAFETY: this checked wrapper is used with the unique producer state
        // owned by its test queue and validates the record bound below.
        if (record_bytes.get() as usize) <= self.config.max_record_bytes {
            if let Some(sequence) =
                unsafe { self.try_reserve_native_holder_single(producer, record_bytes) }
            {
                return Ok(sequence);
            }
        }
        self.reserve_native_holder_single_slow(producer, record_bytes)
    }

    fn reserve_native_arena_fast_inner<'a>(
        &'a self,
        record_bytes: usize,
        producer: Option<&'a SingleProducerState>,
    ) -> Result<Option<NativeArenaPermit<'a, B>>, ReserveError> {
        if record_bytes == 0 {
            return Err(ReserveError::Record(RecordError::Truncated));
        }
        if record_bytes > self.config.max_record_bytes {
            return Err(ReserveError::Record(RecordError::RecordTooLarge {
                size: record_bytes,
                max: self.config.max_record_bytes,
            }));
        }
        if record_bytes > self.native_arena.block_bytes() {
            return Ok(None);
        }
        let single_sequence = match producer {
            Some(producer) => Some(self.claim_detached_capacity_single(producer)?),
            None => {
                self.claim_detached_capacity_concurrent()?;
                None
            }
        };
        // Only the single producer knows its exact sequence here. Concurrent
        // prediction made every worker issue PREFETCHW for the same likely
        // cell before the native ordering gate, creating ownership traffic on
        // a cache line most predictors would not receive. A future concurrent
        // hint belongs in native after exact sequence assignment.
        if let Some(sequence) = single_sequence {
            self.prefetch_native_arena(sequence, record_bytes, false);
        }
        Ok(Some(NativeArenaPermit {
            owner: self,
            exact_record_bytes: record_bytes,
            single_sequence,
            single_producer: producer,
            owns_claim: true,
        }))
    }

    /// Hint one already-selected direct arena target and its mode-specific
    /// producer metadata. SPSC hints the scalar descriptor rather than the
    /// legacy lifecycle cell it deliberately never touches.
    #[inline]
    fn prefetch_native_arena(
        &self,
        sequence: CommitSeq,
        record_bytes: usize,
        include_publication_cell: bool,
    ) {
        #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
        {
            if !prefetch_write_supported() {
                return;
            }
            let target = self
                .native_arena
                .prefetch_target(self.publication_index(sequence))
                .as_ptr();
            // SAFETY: both addresses point into stable queue-owned allocations,
            // and the feature check above authorizes PREFETCHW. Wrong
            // predictions never grant access and are only cache hints.
            unsafe {
                if include_publication_cell {
                    let cell = std::ptr::from_ref(self.publication_cell(sequence)).cast::<u8>();
                    prefetch_write_unchecked(cell);
                } else {
                    let descriptor =
                        std::ptr::from_ref(self.spsc_arena_publication(sequence)).cast::<u8>();
                    prefetch_write_unchecked(descriptor);
                }
                let mut offset = 0usize;
                while offset < record_bytes {
                    prefetch_write_unchecked(target.add(offset));
                    offset += CACHE_LINE_BYTES;
                }
            }
        }
        #[cfg(not(any(target_arch = "x86", target_arch = "x86_64")))]
        let _ = (sequence, record_bytes, include_publication_cell);
    }

    fn claim_detached_native_buffer(
        &self,
        record_bytes: usize,
        producer: Option<&SingleProducerState>,
    ) -> Result<(Option<CommitSeq>, NativeRecordBuffer), ReserveError> {
        let single_sequence = match producer {
            Some(producer) => Some(self.claim_detached_capacity_single(producer)?),
            None => {
                self.claim_detached_capacity_concurrent()?;
                None
            }
        };
        let buffer = if record_bytes <= self.native_arena.block_bytes() {
            NativeRecordBuffer::UnboundArena {
                exact_record_bytes: record_bytes,
            }
        } else {
            NativeRecordBuffer::Owned(self.oversized_pool.take())
        };
        Ok((single_sequence, buffer))
    }

    fn claim_detached_capacity_concurrent(&self) -> Result<(), ReserveError> {
        debug_assert!(!self.single_producer);
        if !self.unhealthy.load(Ordering::Acquire)
            && self.occupied.try_claim(self.config.capacity)
        {
            return Ok(());
        }

        self.claim_detached_capacity_concurrent_slow()
    }

    #[inline(always)]
    fn claim_detached_capacity_packed(&self, worker_slot: usize) -> Result<(), ReserveError> {
        debug_assert!(!self.single_producer);
        if !self.unhealthy.load(Ordering::Acquire) && self.try_claim_packed_occupancy(worker_slot) {
            return Ok(());
        }

        self.claim_detached_capacity_concurrent_slow()
    }

    fn claim_detached_capacity_concurrent_slow(&self) -> Result<(), ReserveError> {
        let mut state = lock_recover(&self.state);
        self.capacity_waiters.fetch_add(1, Ordering::AcqRel);
        let _waiter = CapacityWaiter(&self.capacity_waiters);
        loop {
            self.import_bound_locked(&mut state);
            if let Some(error) = state.reserve_health_error() {
                return Err(error);
            }
            if self.reclaim_all_packed_occupancy_credits() != 0 {
                // `state` closes the waiter predicate window. Notify directly
                // because the ordinary helper would try to lock it again.
                self.capacity_available.notify_all();
            }
            if self.occupied.try_claim(self.config.capacity) {
                return Ok(());
            }
            // `capacity_waiters` is an advisory fast-path notification hint.
            // Periodic predicate rechecks guarantee progress even on a weak
            // memory machine where a releaser transiently observes stale zero.
            state =
                wait_timeout_recover(&self.capacity_available, state, Duration::from_millis(100));
        }
    }

    /// Reserve the next logical capacity credit without publishing a sequence.
    ///
    /// Only the unique [`crate::SingleProducer`] lease can reach this method.
    /// Therefore one future sequence may be retained in the returned permit
    /// without modifying either `occupied` or the cell: a validation abort
    /// leaves no cancellation marker, and no second producer can steal the
    /// promise. The cached exclusive capacity limit may lag but never lead the
    /// consumer-derived limit, so its fast capacity check is conservative.
    #[inline(always)]
    fn claim_detached_capacity_single(
        &self,
        producer: &SingleProducerState,
    ) -> Result<CommitSeq, ReserveError> {
        debug_assert!(
            self.single_producer,
            "single-producer claims require a single-producer queue"
        );
        let capacity = u64::try_from(self.config.capacity).unwrap_or(u64::MAX);
        if !self.unhealthy.load(Ordering::Acquire) {
            let tail = producer.next_sequence.load(Ordering::Relaxed);
            let capacity_limit = producer.capacity_limit.load(Ordering::Relaxed);
            if tail < capacity_limit {
                return self.acquire_single_detached_turn(tail);
            }
        }

        self.claim_detached_capacity_single_slow(producer, capacity)
    }

    /// Refresh or wait after the producer-local capacity window is exhausted.
    /// Keeping this state/condvar machinery out of the always-inlined common
    /// claim avoids a call, stack frame, and large Result return on every ACK.
    #[cold]
    #[inline(never)]
    fn claim_detached_capacity_single_slow(
        &self,
        producer: &SingleProducerState,
        capacity: u64,
    ) -> Result<CommitSeq, ReserveError> {
        if !self.unhealthy.load(Ordering::Acquire) {
            let tail = producer.next_sequence.load(Ordering::Relaxed);
            // Refresh the cross-generation handoff without taking the queue
            // mutex. Acquire observes the consumer's final arena/cold reads
            // before admitting reuse of that applied generation.
            let applied = self.applied_frontier.load(Ordering::Acquire);
            let capacity_limit = applied.saturating_add(capacity);
            producer
                .capacity_limit
                .store(capacity_limit, Ordering::Relaxed);
            if tail < capacity_limit {
                return self.acquire_single_detached_turn(tail);
            }
        }

        let mut state = lock_recover(&self.state);
        self.capacity_waiters.fetch_add(1, Ordering::AcqRel);
        let _waiter = CapacityWaiter(&self.capacity_waiters);
        loop {
            self.import_bound_locked(&mut state);
            if let Some(error) = state.reserve_health_error() {
                return Err(error);
            }
            let tail = producer.next_sequence.load(Ordering::Relaxed);
            producer.capacity_limit.store(
                state.applied.sequence.saturating_add(capacity),
                Ordering::Relaxed,
            );
            if tail == u64::MAX {
                return Err(ReserveError::SequenceExhausted);
            }
            let live = tail
                .checked_sub(state.applied.sequence)
                .expect("the applied frontier cannot exceed the bound tail");
            if live < capacity {
                return self.acquire_single_detached_turn(tail);
            }
            state =
                wait_timeout_recover(&self.capacity_available, state, Duration::from_millis(100));
        }
    }

    #[inline(always)]
    fn acquire_single_detached_turn(&self, tail: u64) -> Result<CommitSeq, ReserveError> {
        let raw_sequence = tail.checked_add(1).ok_or(ReserveError::SequenceExhausted)?;
        let sequence = CommitSeq::new(raw_sequence)
            .expect("the sequence after a valid applied seed is nonzero");

        // For configured capacity C and power-of-two ring length R >= C,
        // tail-applied < C implies sequence-R <= applied. The applied-frontier
        // Acquire in the refresh path observes the consumer's final access to
        // that generation. The unique producer lease then retains this future
        // generation without any per-cell load or mutation.
        Ok(sequence)
    }

    /// Assign and publish the next dense bound descriptor.
    ///
    /// `ExternallySerialized` is selected only by the legacy unsafe native
    /// binder. Its caller proves an external exclusion covers every sequence
    /// allocator for this Writeback. Mixing that mode concurrently with the
    /// ordinary binder would lose a tail update and assign one sequence twice.
    fn bind_sequence(&self, mode: BindSequenceMode) -> Result<CommitSeq, ReserveError> {
        debug_assert!(!self.single_producer);
        if self.unhealthy.load(Ordering::Acquire) {
            let mut state = lock_recover(&self.state);
            self.import_bound_locked(&mut state);
            if let Some(error) = state.reserve_health_error() {
                return Err(error);
            }
        }

        let observed = self.next_bound.load(Ordering::Acquire);
        let fast_headroom = u64::try_from(self.config.capacity)
            .ok()
            .and_then(|capacity| u64::MAX.checked_sub(capacity))
            .is_some_and(|safe_tail| observed <= safe_tail);
        // Only the final `capacity` values need the state mutex. This legacy
        // Rust-side binder retains the cold checked-RMW path; the newer native
        // allocator relies on its already-claimed occupancy credit and checks
        // MAX before advancing the same atomic tail.
        let sequence_guard = if fast_headroom {
            None
        } else {
            let mut state = lock_recover(&self.state);
            self.import_bound_locked(&mut state);
            if let Some(error) = state.reserve_health_error() {
                return Err(error);
            }
            Some(state)
        };
        let previous_sequence = if fast_headroom {
            match mode {
                BindSequenceMode::Concurrent => {
                    // Bounded permits prove that at most `capacity` concurrent
                    // binds can advance from this safe value.
                    self.next_bound.fetch_add(1, Ordering::AcqRel)
                }
                BindSequenceMode::ExternallySerialized => {
                    // SAFETY CONTRACT: the caller supplies the missing
                    // single-writer exclusion. Its preceding Release and this
                    // turn's Acquire prevent a new binder from observing an
                    // older externally serialized tail.
                    self.next_bound.store(observed + 1, Ordering::Release);
                    observed
                }
            }
        } else {
            self.next_bound
                .fetch_update(Ordering::AcqRel, Ordering::Acquire, |current| {
                    current.checked_add(1)
                })
                .map_err(|_| ReserveError::SequenceExhausted)?
        };
        let raw_sequence = previous_sequence
            .checked_add(1)
            .expect("bounded fast sequence allocation cannot overflow");
        let sequence = CommitSeq::new(raw_sequence)
            .expect("the sequence after a valid applied seed is nonzero");

        let publication = self.publication_cell(sequence);
        let cold = self.cold_publication_cell(sequence);
        match mode {
            BindSequenceMode::Concurrent => {
                publication.publish_bound(cold, sequence, self.publication_shift);
            }
            BindSequenceMode::ExternallySerialized => {
                // SAFETY: constructing this mode is confined to the unsafe
                // native binder whose contract proves single-writer exclusion.
                unsafe {
                    publication.publish_bound_externally_serialized(
                        cold,
                        sequence,
                        self.publication_shift,
                    )
                };
            }
        }
        drop(sequence_guard);
        self.notify_descriptor_waiters();
        if raw_sequence == u64::MAX {
            self.capacity_available.notify_all();
        }
        Ok(sequence)
    }

    /// Consume the exact future generation retained by one foreground lease.
    ///
    /// No sequence is allocated until this post-validation call. The permit's
    /// retained capacity credit and the still-live mutable producer borrow
    /// exclude every other tail/cell writer, so ordinary Release stores replace
    /// the MPMC tail RMW and turn compare-exchange.
    fn bind_single_reserved_sequence(
        &self,
        producer: &SingleProducerState,
        sequence: CommitSeq,
    ) -> Result<CommitSeq, ReserveError> {
        assert!(
            self.single_producer,
            "single-producer bind requires a single-producer queue"
        );
        if self.unhealthy.load(Ordering::Acquire) {
            let mut state = lock_recover(&self.state);
            self.import_bound_locked(&mut state);
            if let Some(error) = state.reserve_health_error() {
                return Err(error);
            }
        }

        producer.accept(sequence);
        // SAFETY: the unique lease and capacity credit were retained
        // continuously from `claim_detached_capacity_single`, so no other
        // binder could consume or replace that generation.
        unsafe {
            self.publication_cell(sequence)
                .publish_bound_single_reserved(
                    self.cold_publication_cell(sequence),
                    sequence,
                    self.publication_shift,
                )
        };
        // There is no suffix producer which can be waiting for this descriptor
        // in SPSC mode. Unknown/failure resolution imports its own BOUND turn
        // synchronously, while the background consumer independently polls.
        if sequence.get() == u64::MAX {
            self.capacity_available.notify_all();
        }
        Ok(sequence)
    }

    /// Publish an exact SPSC sequence after native has already accepted it into
    /// the serialization order and initialized its record bytes.
    ///
    /// Unlike [`Self::bind_single_reserved_sequence`], this cannot reject on a
    /// newly latched write-back failure: native has crossed its commit point,
    /// so leaving the preselected turn invisible would lose a visible write.
    /// The caller must bind first and let the ordinary publication/pinning
    /// protocol retain the record behind any earlier failure.
    ///
    /// # Safety
    ///
    /// The caller must retain the unique single-producer lease and capacity
    /// credit for `sequence`. Native must already have returned a nonzero
    /// accepted timestamp for the matching preselected record target.
    #[inline(always)]
    unsafe fn bind_single_reserved_sequence_after_acceptance(
        &self,
        producer: &SingleProducerState,
        sequence: CommitSeq,
    ) {
        debug_assert!(self.single_producer);
        producer.accept(sequence);
        // SAFETY: required by this method's retained-sequence and unique-lease
        // contract. The following BOUND Release makes the already accepted
        // dense descriptor discoverable before it can be published or pinned.
        unsafe {
            self.publication_cell(sequence)
                .publish_bound_single_reserved(
                    self.cold_publication_cell(sequence),
                    sequence,
                    self.publication_shift,
                )
        };
    }

    fn release_detached_claim(&self, native_buffer: Option<NativeRecordBuffer>) {
        if let Some(buffer) = native_buffer {
            self.recycle_native_buffer(buffer);
        }
        if self.single_producer {
            // The logical credit never changed tail or aggregate occupancy.
            // Dropping before bind therefore needs no shared metadata write
            // and the same future turn remains available.
        } else {
            self.occupied.release();
            self.notify_capacity_release();
        }
    }

    fn notify_capacity_release(&self) {
        if self.capacity_waiters.load(Ordering::Acquire) != 0 {
            // The waiter registers and rechecks while holding `state`. Taking
            // the same mutex before notification closes the only lost-wakeup
            // window without burdening the ordinary non-full fast path.
            let state = lock_recover(&self.state);
            self.capacity_available.notify_one();
            drop(state);
        }
    }

    /// Wake one explicit apply waiter only when somebody may be asleep.
    ///
    /// Concurrent mode retains the SeqCst acknowledgement/count handshake, so
    /// it cannot lose a wake. Single-producer mode uses this as an advisory
    /// latency hint and bounds the wait independently.
    fn notify_activity_waiter(&self) {
        if self.activity_waiters.load(Ordering::SeqCst) != 0 {
            let state = lock_recover(&self.state);
            self.changed.notify_one();
            drop(state);
        }
    }

    /// Wake out-of-order publishers only after one has left the bounded spin
    /// path and enrolled in the condition-variable handoff.
    fn notify_acknowledgement_waiters(&self) {
        if self.acknowledgement_waiters.load(Ordering::SeqCst) != 0 {
            // Registration and the acknowledgement recheck happen under this
            // mutex. Taking it before notification closes the final
            // predicate-to-wait window without taxing the no-sleeper path.
            let state = lock_recover(&self.state);
            self.acknowledgement_changed.notify_all();
            drop(state);
        }
    }

    fn recycle_native_buffer(&self, buffer: NativeRecordBuffer) {
        if let NativeRecordBuffer::Owned(bytes) = buffer {
            self.oversized_pool.recycle(bytes, self.config.capacity);
        }
    }

    fn recycle_native_record(&self, record: RecycledNativeRecord) {
        match record {
            RecycledNativeRecord::Arena(block) => {
                debug_assert!(block < self.publication_cells.len());
            }
            RecycledNativeRecord::Owned(bytes) => {
                self.oversized_pool.recycle(bytes, self.config.capacity);
            }
            RecycledNativeRecord::Holder(sequence) => {
                let Some(pool) = self.native_holder_pool.as_ref() else {
                    std::process::abort();
                };
                let raw =
                    NonZeroU64::new(sequence.get()).expect("cache commit sequences are nonzero");
                // SAFETY: successful backend retirement is the consumer's
                // final use of this exact generation. `state` remains locked.
                // After this synchronous release, retirement Release-publishes
                // either the dense SPSC applied frontier or this concurrent
                // cell's exact FREE turn before its occupancy becomes
                // claimable again.
                let view = match unsafe { pool.view(raw) } {
                    Ok(view) => view,
                    Err(_) => std::process::abort(),
                };
                if unsafe { pool.release(view) }.is_err() {
                    // Same-build protocol corruption after a successful
                    // backend write cannot be recovered by restoring a batch
                    // whose earlier holders may already have been released.
                    std::process::abort();
                }
            }
        }
    }

    /// Current in-memory applied watermark.
    pub fn applied_watermark(&self) -> AppliedWatermark {
        lock_recover(&self.state).applied
    }

    /// Highest contiguous bound record accepted atomically by the backend.
    pub fn applied_sequence(&self) -> u64 {
        self.applied_watermark().sequence()
    }

    /// Highest dense Ready sequence prefix available for ordered replay.
    ///
    /// This is deliberately distinct from both the queue tail and the
    /// caller-visible high-water mark of trusted out-of-order publication.
    pub fn highest_acknowledged(&self) -> u64 {
        self.acknowledged.load(Ordering::Acquire)
    }

    /// Highest sequence acknowledged by any foreground terminal.
    ///
    /// This is a high-water mark, not a prefix. The trusted concurrent one-Put
    /// path may complete a later READY cell while a lower sequence remains in
    /// flight or becomes pinned. The backend still consumes only the dense
    /// `acknowledged` prefix; barriers include this high-water mark and either
    /// wait for every hole to close or report the earlier fail-stop condition.
    pub fn highest_caller_acknowledged(&self) -> u64 {
        self.trusted_caller_ack_by_worker
            .iter()
            .fold(self.highest_acknowledged(), |highest, worker| {
                highest.max(worker.load(Ordering::Acquire))
            })
    }

    #[inline(always)]
    fn trusted_caller_ack_slot(&self, worker_slot: usize) -> &CacheLineAtomicU64 {
        self.trusted_caller_ack_by_worker
            .get(worker_slot)
            .expect("trusted caller-ack worker slot is in range")
    }

    /// Number of bound queue slots. Detached permits are deliberately omitted.
    pub fn queue_len(&self) -> usize {
        let mut state = lock_recover(&self.state);
        self.import_bound_locked(&mut state);
        state.queue.len()
    }

    #[cfg(test)]
    fn detached_len(&self) -> usize {
        let state = lock_recover(&self.state);
        let occupied = self.occupied.load();
        // Concurrent `next_bound` is deliberately stale. Under the retained
        // state lock, inspect the bounded live window instead so an
        // out-of-order BOUND suffix is not miscounted as detached.
        let bound = (1..=occupied)
            .filter_map(|offset| {
                u64::try_from(offset)
                    .ok()
                    .and_then(|offset| state.applied.sequence.checked_add(offset))
                    .and_then(CommitSeq::new)
            })
            .filter(|&sequence| {
                self.publication_cell(sequence)
                    .is_bound(sequence, self.publication_shift)
            })
            .count();
        let idle_packed_credits = self
            .packed_occupancy_credits_by_worker
            .iter()
            .map(|credit| credit.load(Ordering::Relaxed))
            .sum::<usize>();
        occupied
            .saturating_sub(idle_packed_credits)
            .saturating_sub(bound)
    }

    #[cfg(test)]
    fn free_len(&self) -> usize {
        self.config.capacity.saturating_sub(self.occupied.load())
    }

    /// Snapshot the highest acknowledged sequence and apply that snapshot to
    /// the backend.
    ///
    /// Transactions acknowledged after the snapshot are intentionally not
    /// part of this barrier. That includes a later pinned unknown slot: it does
    /// not invalidate an earlier acknowledged prefix, although clean shutdown
    /// separately rejects any unknown outcome. Consecutive ready records are
    /// submitted in bounded atomic `write_batch` calls. A failed prefix remains
    /// unchanged at the front for the next retry.
    pub fn wait_applied(&self) -> Result<u64, ApplyError> {
        let target = self.highest_caller_acknowledged();
        self.wait_applied_through(target)
    }

    /// Apply exactly one caller-visible acknowledgement snapshot.
    ///
    /// The background consumer may batch every currently Ready slot, but a
    /// synchronous barrier must not inspect or materialize a suffix published
    /// after it captured `target`. Otherwise corruption or transient memory
    /// pressure in later work can incorrectly fail an earlier barrier.
    fn wait_applied_through(&self, target: u64) -> Result<u64, ApplyError> {
        if let Some(error) = lock_recover(&self.state).apply_health_error_through(target) {
            return Err(error);
        }

        let mut failed_sequence = None;
        let mut failed_attempts = 0usize;

        loop {
            {
                let state = lock_recover(&self.state);
                if let Some(error) = state.apply_health_error_through(target) {
                    return Err(error);
                }
                if state.applied.sequence >= target {
                    return Ok(target);
                }
            }

            match self.process_front_through(Some(target)) {
                ProcessOutcome::Advanced => {
                    failed_sequence = None;
                    failed_attempts = 0;
                }
                ProcessOutcome::BackendFailed { sequence, error } => {
                    if failed_sequence == Some(sequence) {
                        failed_attempts = failed_attempts.saturating_add(1);
                    } else {
                        failed_sequence = Some(sequence);
                        failed_attempts = 1;
                    }

                    if failed_attempts > self.config.max_apply_retries {
                        // A concurrent consumer may have recovered between our
                        // failed attempt and this decision.
                        let applied = lock_recover(&self.state).applied.sequence;
                        match retry_progress(applied, target, sequence) {
                            RetryProgress::TargetApplied => return Ok(target),
                            RetryProgress::FailedSequenceApplied => {
                                failed_sequence = None;
                                failed_attempts = 0;
                                continue;
                            }
                            RetryProgress::NoProgress => {}
                        }
                        return Err(ApplyError::Backend {
                            sequence,
                            attempts: failed_attempts,
                            source: error,
                        });
                    }
                    self.wait_for_activity(self.config.retry_delay);
                }
                ProcessOutcome::RecordFailed { sequence, error } => {
                    if sequence.get() <= target {
                        return Err(ApplyError::Record {
                            sequence,
                            source: error,
                        });
                    }
                    // The sequence cap makes this unreachable for a healthy
                    // dense queue. Retain the defensive branch for a cached
                    // concurrent failure, then re-check applied progress.
                }
                ProcessOutcome::Blocked | ProcessOutcome::Idle => {
                    self.wait_for_apply_progress(target);
                }
                ProcessOutcome::Pinned(sequence) => {
                    return Err(ApplyError::UnknownOutcome { sequence });
                }
            }
        }
    }

    fn resolve(&self, token: QueueToken, resolution: Resolution) -> Result<(), ResolveError> {
        match resolution {
            Resolution::Publish => self.publish_record(token),
            Resolution::PinUnknown => self.pin_unknown_record(token),
        }
    }

    /// Attach a complete record to its stable Prepared publication cell.
    fn attach_record(&self, token: QueueToken, record: QueuedCommitRecord) {
        self.publication_cell(token.sequence()).attach(
            self.cold_publication_cell(token.sequence()),
            token.sequence(),
            self.publication_shift,
            record,
        );
    }

    fn attach_arena_record(
        &self,
        token: QueueToken,
        mako_timestamp: MakoTimestamp,
        exact_record_bytes: usize,
    ) {
        self.publication_cell(token.sequence()).attach_arena(
            token.sequence(),
            self.publication_shift,
            mako_timestamp,
            exact_record_bytes,
        );
    }

    /// Publish the common known-success outcome without taking `state`.
    ///
    /// The fixed ring cell owns the complete record before Ready is released,
    /// and the dense atomic frontier ensures a consumer never harvests an
    /// unacknowledged suffix. Only an out-of-order publisher or a latched
    /// fail-stop condition enters the state-locked fallback.
    fn publish_record(&self, token: QueueToken) -> Result<(), ResolveError> {
        let sequence = token.sequence();
        self.publication_cell(sequence)
            .publish_attached(sequence, self.publication_shift);
        self.finish_ready_publication(token)
    }

    /// Advance acknowledgement for a record whose exact ring turn is already
    /// READY. The specialized arena path reaches this directly from BOUND,
    /// while the general path first publishes its attached record above.
    fn finish_ready_publication(&self, token: QueueToken) -> Result<(), ResolveError> {
        if self.single_producer {
            return self.finish_ready_publication_single(token);
        }
        self.finish_ready_publication_concurrent(token)
    }

    /// Publish the necessarily in-order healthy result of one exclusive
    /// foreground producer. Runtime and explicit barrier waits are bounded
    /// polls, so the acknowledgement Release store is the consumer gate; no
    /// waiter-count load, notification, SeqCst operation, or RMW is needed.
    fn finish_ready_publication_single(&self, token: QueueToken) -> Result<(), ResolveError> {
        let sequence = token.sequence();
        let healthy = !self.unhealthy.load(Ordering::Acquire);
        if healthy {
            // Recovery initializes ACK == tail == applied. The exclusive
            // producer cannot start its next transaction until this call
            // returns, and an abort binds no sequence. Thus known-success SPSC
            // publications are necessarily the next dense acknowledgement.
            debug_assert_eq!(
                self.acknowledged.load(Ordering::Relaxed).checked_add(1),
                Some(sequence.get())
            );
            self.acknowledged.store(sequence.get(), Ordering::Release);

            #[cfg(test)]
            self.acknowledgement_changed.notify_all();
            return Ok(());
        }

        // Any observed health barrier retains the established state-locked
        // fail-stop resolver.
        self.finish_published_locked(token)
    }

    fn finish_ready_publication_concurrent(&self, token: QueueToken) -> Result<(), ResolveError> {
        let sequence = token.sequence();
        // This load is the healthy/fail-stop ordering point. If a concurrent
        // consumer latches a record failure after it, this publication is
        // equivalent to the old implementation winning `state` first. If the
        // failure was already observed, do not extend caller ACK across it.
        let healthy = !self.unhealthy.load(Ordering::Acquire);
        let mut acknowledged = self.acknowledged.load(Ordering::Relaxed);

        #[cfg(test)]
        self.acknowledgement_changed.notify_all();

        if healthy {
            // The publisher which owns the next dense sequence claims the ACK
            // baton with one CAS, then sweeps any successors which are already
            // Ready. Suffix publishers wait instead of all rescanning and
            // racing on the same head cell.
            for spin in 0..=CONCURRENT_ACKNOWLEDGEMENT_SPINS {
                if acknowledged >= sequence.get() {
                    return Ok(());
                }
                if acknowledged.checked_add(1) == Some(sequence.get()) {
                    match self.acknowledged.compare_exchange(
                        acknowledged,
                        sequence.get(),
                        // The Release half publishes this thread's already
                        // Ready cell; SeqCst also closes both waiter-count
                        // handshakes.
                        Ordering::SeqCst,
                        Ordering::Relaxed,
                    ) {
                        Ok(_) => {
                            // This publisher now owns the dense-prefix baton.
                            // Sweep successors which became READY before their
                            // missing predecessor, otherwise no producer may
                            // remain to move the consumer frontier again.
                            self.advance_atomic_acknowledgement(sequence.get(), u64::MAX);
                            self.notify_activity_waiter();
                            // A later publisher may release Ready just after
                            // this CAS, then enroll to sleep. Every frontier
                            // advance participates in its waiter handshake.
                            self.notify_acknowledgement_waiters();
                            return Ok(());
                        }
                        Err(observed) => {
                            acknowledged = observed;
                            continue;
                        }
                    }
                }
                if spin == CONCURRENT_ACKNOWLEDGEMENT_SPINS {
                    break;
                }
                // Health failure is exceptional. Poll it periodically while
                // keeping the ordinary ACK handoff to one relaxed load.
                if spin & 31 == 31 && self.unhealthy.load(Ordering::Acquire) {
                    break;
                }
                std::hint::spin_loop();
                acknowledged = self.acknowledged.load(Ordering::Relaxed);
            }

            if !self.unhealthy.load(Ordering::Acquire) {
                // A predecessor can Release-publish Ready and then lose its
                // CPU before advancing the dense frontier. Help it once before
                // paying for the locked fallback.
                let prior_acknowledgement = self.acknowledged.load(Ordering::SeqCst);
                let acknowledged =
                    self.advance_atomic_acknowledgement(prior_acknowledgement, u64::MAX);
                if acknowledged > prior_acknowledgement {
                    self.notify_activity_waiter();
                    self.notify_acknowledgement_waiters();
                }
                if !self.unhealthy.load(Ordering::Acquire) && acknowledged >= sequence.get() {
                    return Ok(());
                }
            }
        }

        self.finish_published_locked(token)
    }

    /// Acknowledge one trusted native commit without waiting for an earlier
    /// producer to close the dense READY prefix.
    ///
    /// The caller's exact cell is already READY and its native terminal has
    /// reported definite visibility plus successful cleanup. A later caller
    /// may therefore return independently, while RocksDB replay and explicit
    /// barriers retain dense ordering through `acknowledged`.
    fn finish_trusted_ready_publication_concurrent(
        &self,
        token: QueueToken,
        caller_ack: &CacheLineAtomicU64,
    ) -> Result<(), ResolveError> {
        debug_assert!(!self.single_producer);
        let sequence = token.sequence();
        if self.unhealthy.load(Ordering::Acquire) {
            return self.finish_published_locked(token);
        }

        // READY is Release-published before this Release store. The native
        // worker owns this cache line for process lifetime, so its accepted
        // sequences are strictly increasing and no RMW is needed. A barrier
        // which Acquire-observes this marker consequently also observes the
        // immutable record bytes. Dense acknowledgement is left to the sole
        // consumer (or an ordinary general publisher).
        debug_assert!(
            caller_ack.load(Ordering::Relaxed) < sequence.get(),
            "one native worker's trusted caller acknowledgements are monotonic"
        );
        caller_ack.store(sequence.get(), Ordering::Release);

        Ok(())
    }

    fn finish_published_locked(&self, token: QueueToken) -> Result<(), ResolveError> {
        let sequence = token.sequence();
        let state = lock_recover(&self.state);
        let (mut state, offset) = self.wait_for_bound_token_locked(state, token);
        let Some(offset) = offset else {
            // A consumer may apply a healthy acknowledged record in the short
            // interval before a concurrently latched, later failure diverts us
            // here. Applied retirement proves this publication succeeded.
            if state.applied.sequence >= sequence.get() {
                return Ok(());
            }
            return Err(ResolveError::Missing { sequence });
        };
        if state.queue[offset].state == (SlotState::Prepared { pinned: false }) {
            assert!(
                self.harvest_published_slot_locked(&mut state, offset),
                "a slow published slot must retain its atomic record"
            );
        }

        loop {
            let health_barrier = match (
                state.first_unknown,
                state
                    .permanent_record_failure
                    .as_ref()
                    .map(|failure| failure.sequence),
            ) {
                (Some(unknown), Some(failure)) => Some(unknown.min(failure)),
                (Some(unknown), None) => Some(unknown),
                (None, Some(failure)) => Some(failure),
                (None, None) => None,
            };
            let safe_maximum = health_barrier
                .map(|barrier| barrier.get().saturating_sub(1))
                .unwrap_or(u64::MAX);
            let prior_acknowledgement = self.acknowledged.load(Ordering::Acquire);
            if prior_acknowledgement < safe_maximum {
                let acknowledged =
                    self.advance_atomic_acknowledgement(prior_acknowledgement, safe_maximum);
                if acknowledged > prior_acknowledgement {
                    // We already hold `state`, so this notification cannot race
                    // an out-of-order publisher's predicate-to-wait handoff.
                    self.acknowledgement_changed.notify_all();
                    self.changed.notify_one();
                }
            }

            let prior_unknown = state.first_unknown.filter(|unknown| *unknown < sequence);
            let prior_failure = state
                .permanent_record_failure
                .as_ref()
                .filter(|failure| failure.sequence < sequence)
                .cloned();
            if let Some(prior_unknown) = prior_unknown.filter(|prior_unknown| {
                prior_failure
                    .as_ref()
                    .is_none_or(|failure| *prior_unknown < failure.sequence)
            }) {
                let current_offset = state
                    .queue_offset(token)
                    .expect("an unacknowledged published slot cannot leave the ordered queue");
                let current = &mut state.queue[current_offset];
                assert_eq!(
                    current.state,
                    SlotState::Ready,
                    "a published suffix remains Ready until its prefix resolves"
                );
                current.state = SlotState::Prepared { pinned: true };
                return Err(ResolveError::BlockedByPriorUnknown {
                    sequence,
                    prior_unknown,
                });
            }
            if let Some(failure) = prior_failure {
                return Err(failure.resolve_error(sequence));
            }
            if self.acknowledged.load(Ordering::Acquire) >= sequence.get() {
                return Ok(());
            }

            let acknowledgement_waiter = AcknowledgementWaiter::new(&self.acknowledgement_waiters);
            // Registration and this SeqCst predicate read retain the direct
            // notification handoff between ordinary publishers. Trusted
            // publishers deliberately touch neither this count nor this
            // condition variable, so the bounded wait below also provides
            // progress when such a publisher closes the missing READY cell.
            if self.acknowledged.load(Ordering::SeqCst) >= sequence.get() {
                drop(acknowledgement_waiter);
                return Ok(());
            }
            state = wait_timeout_recover(
                &self.acknowledgement_changed,
                state,
                Duration::from_millis(1),
            );
            drop(acknowledgement_waiter);
        }
    }

    fn pin_unknown_record(&self, token: QueueToken) -> Result<(), ResolveError> {
        let sequence = token.sequence();
        let state = lock_recover(&self.state);
        let (mut state, offset) = self.wait_for_bound_token_locked(state, token);
        let Some(offset) = offset else {
            return Err(ResolveError::Missing { sequence });
        };
        match state.queue[offset].state {
            SlotState::Prepared { pinned: true } => {
                return Err(ResolveError::Pinned { sequence });
            }
            SlotState::Prepared { pinned: false } => {}
            SlotState::Ready | SlotState::Applying => {
                return Err(ResolveError::AlreadyResolved { sequence });
            }
        }
        assert!(state.queue[offset].record.is_none());
        self.publication_cell(sequence)
            .pin(sequence, self.publication_shift);
        state.queue[offset].record = self.publication_cell(sequence).take_unpublished(
            self.cold_publication_cell(sequence),
            sequence,
            self.publication_shift,
            &self.native_arena,
            self.publication_index(sequence),
        );
        state.queue[offset].state = SlotState::Prepared { pinned: true };
        // Publish the slow-path marker while holding `state`, before installing
        // detailed health. A racing observer that sees true must acquire this
        // mutex and therefore cannot miss the first unknown sequence.
        self.unhealthy.store(true, Ordering::Release);
        state.first_unknown = Some(match state.first_unknown {
            Some(current) => current.min(sequence),
            None => sequence,
        });
        drop(state);

        self.changed.notify_all();
        self.acknowledgement_changed.notify_all();
        self.capacity_available.notify_all();
        Ok(())
    }

    pub(crate) fn process_front(&self) -> ProcessOutcome {
        self.process_front_through(None)
    }

    /// Process a bounded Ready prefix, optionally capped at an inclusive
    /// sequence. `None` is the background worker's ordinary unbounded mode;
    /// synchronous barriers pass their immutable acknowledgement snapshot.
    fn process_front_through(&self, max_sequence: Option<u64>) -> ProcessOutcome {
        // This guard is separate from the queue state so the single-consumer
        // rule remains explicit if the state lock is narrowed around IO later.
        // Both locks deliberately recover poison: if a backend panics, the
        // untouched Ready prefix is safe to retry.
        let _consumer = lock_recover(&self.consumer);
        let mut state = lock_recover(&self.state);
        self.import_bound_locked(&mut state);

        // Trusted foreground terminals publish only their own READY cell and
        // per-worker caller marker. The sole consumer closes the dense replay
        // prefix here. Never extend it across a fail-stop boundary discovered
        // by an earlier consumer or unknown-outcome publisher.
        let health_barrier = match (
            state.first_unknown,
            state
                .permanent_record_failure
                .as_ref()
                .map(|failure| failure.sequence),
        ) {
            (Some(unknown), Some(failure)) => Some(unknown.min(failure)),
            (Some(unknown), None) => Some(unknown),
            (None, Some(failure)) => Some(failure),
            (None, None) => None,
        };
        let health_maximum = health_barrier
            .map(|barrier| barrier.get().saturating_sub(1))
            .unwrap_or(u64::MAX);
        let sweep_maximum = max_sequence.unwrap_or(u64::MAX).min(health_maximum);
        let prior_acknowledgement = self.acknowledged.load(Ordering::Acquire);
        let acknowledged =
            self.advance_atomic_acknowledgement(prior_acknowledgement, sweep_maximum);
        if acknowledged > prior_acknowledgement {
            // `state` closes the predicate-to-wait window for slow ordinary
            // publishers. Apply waiters are notified after backend progress;
            // their bounded poll also covers a consumer which stops here.
            self.acknowledgement_changed.notify_all();
        }
        self.harvest_acknowledged_locked(&mut state);

        let Some(front) = state.queue.front() else {
            return ProcessOutcome::Idle;
        };
        let first_sequence = front.sequence;
        if max_sequence.is_some_and(|maximum| first_sequence.get() > maximum) {
            return ProcessOutcome::Idle;
        }
        // A pinned outcome is deliberately outside the dense Ready/replay
        // prefix, so classify it before the generic dense gate below.
        // Otherwise every unknown front would be reported merely as Blocked
        // and clean drains/background diagnostics could not distinguish a
        // permanent ordering barrier from a publisher that is still running.
        match front.state {
            SlotState::Prepared { pinned: true } => return ProcessOutcome::Pinned(first_sequence),
            SlotState::Prepared { pinned: false } | SlotState::Ready => {}
            SlotState::Applying => {
                unreachable!("the serialized consumer cannot observe another applying batch")
            }
        }
        if first_sequence.get() > self.acknowledged.load(Ordering::Acquire) {
            // A publisher may have exposed immutable bytes out of order, but
            // the consumer must not take ownership until the dense Ready
            // frontier has crossed this exact slot.
            return ProcessOutcome::Blocked;
        }
        let permanent_record_failure = state.permanent_record_failure.clone();
        if let Some(failure) = permanent_record_failure
            .as_ref()
            .filter(|failure| failure.sequence <= first_sequence)
        {
            // The first discovery left this exact slot in place. Never decode
            // it again: structural invalidity cannot be repaired by retrying.
            return ProcessOutcome::RecordFailed {
                sequence: failure.sequence,
                error: failure.source.clone(),
            };
        }
        if front.state == (SlotState::Prepared { pinned: false }) {
            return ProcessOutcome::Blocked;
        }

        // Capture a bounded contiguous Ready prefix. Prepared and pinned slots
        // are ordering barriers; a later Ready transaction must never pass
        // either one. Move each record out of its preallocated queue slot so
        // the foreground path needs neither an Arc allocation nor OnceLock.
        // CapturedBatch restores every slot if decoding, IO, or unwinding does
        // not retire the complete prefix.
        let mut capture_len = 0usize;
        let mut encoded_bytes = 0usize;
        let acknowledged = self.acknowledged.load(Ordering::Acquire);
        for slot in &state.queue {
            if slot.state != SlotState::Ready || capture_len == self.config.max_batch_records {
                break;
            }
            if slot.sequence.get() > acknowledged {
                break;
            }
            if max_sequence.is_some_and(|maximum| slot.sequence.get() > maximum) {
                break;
            }
            if permanent_record_failure
                .as_ref()
                .is_some_and(|failure| slot.sequence >= failure.sequence)
            {
                // A prior application barrier may still drain a safe Ready
                // prefix before the later permanent failure.
                break;
            }
            let record = slot
                .record
                .as_ref()
                .expect("a Ready slot must contain its finalized record");
            let next_bytes = encoded_bytes.saturating_add(record.encoded_len());
            if capture_len != 0 && next_bytes > self.config.max_batch_bytes {
                break;
            }
            encoded_bytes = next_bytes;
            capture_len += 1;
        }
        assert!(capture_len != 0, "a Ready front must form a nonempty batch");
        let mut captured_records = Vec::with_capacity(capture_len);
        for slot in state.queue.iter_mut().take(capture_len) {
            let record = slot
                .record
                .take()
                .expect("a captured Ready slot must own its finalized record");
            slot.state = SlotState::Applying;
            captured_records.push(record);
        }
        drop(state);
        let mut captured = CapturedBatch::new(self, captured_records);

        #[cfg(test)]
        crate::failpoint::hit(crate::failpoint::Point::ReadyBeforeBackend);

        // Native records are decoded and materialized only on this background
        // path. Keeping the owned decoded records together lets one flat
        // BlobOp vector borrow from all of them for the synchronous batch call.
        let mut records = Vec::with_capacity(captured.records.len());
        for queued in &captured.records {
            match self.materialize_queued_record(queued) {
                Ok(record) => records.push(record),
                Err(error) => {
                    let sequence = queued.sequence();
                    captured.restore();
                    return self.record_failure_outcome(sequence, error);
                }
            }
        }

        let operation_count = records.iter().fold(0usize, |total, record| {
            total.saturating_add(record.backend_op_count())
        });
        let mut operations = Vec::with_capacity(operation_count);
        for record in &records {
            record.append_backend_ops(&mut operations);
        }
        let result = self.backend.write_batch(&operations);

        match result {
            Ok(()) => {
                #[cfg(test)]
                crate::failpoint::hit(crate::failpoint::Point::BackendWrittenBeforeApplied);
                captured.retire(&records);
                #[cfg(test)]
                crate::failpoint::hit(crate::failpoint::Point::AppliedAdvanced);
                // A whole prefix became free. Wake all bounded-capacity
                // producers once, and every barrier/consumer observing the
                // new applied frontier once.
                self.capacity_available.notify_all();
                self.changed.notify_all();
                ProcessOutcome::Advanced
            }
            Err(error) => {
                captured.restore();
                ProcessOutcome::BackendFailed {
                    sequence: first_sequence,
                    error,
                }
            }
        }
    }

    fn materialize_queued_record(
        &self,
        queued: &QueuedCommitRecord,
    ) -> Result<crate::record::CommitRecord, RecordError> {
        #[cfg(test)]
        if let Some(error) = TEST_MATERIALIZATION_FAILURE.with(|failure| {
            failure
                .borrow()
                .as_ref()
                .filter(|(sequence, _)| *sequence == queued.sequence())
                .map(|(_, error)| error.clone())
        }) {
            return Err(error);
        }

        if let Some(holder) = queued.deferred_holder() {
            let pool =
                self.native_holder_pool
                    .as_ref()
                    .ok_or(RecordError::NativeHolderUnavailable {
                        sequence: holder.sequence().get(),
                    })?;
            let raw = NonZeroU64::new(holder.sequence().get())
                .expect("cache commit sequences are nonzero");
            // SAFETY: harvesting this descriptor followed an Acquire of
            // either the dense SPSC published tail or this concurrent
            // generation's exact READY turn. The captured record retains its
            // occupancy right and the cell is not retired to FREE until
            // materialization and backend application finish, so no producer
            // can reuse the masked holder while this view lives.
            let view =
                unsafe { pool.view(raw) }.map_err(|_| RecordError::NativeHolderUnavailable {
                    sequence: holder.sequence().get(),
                })?;
            if view.mako_timestamp() != holder.mako_timestamp() {
                return Err(RecordError::WrongMakoTimestamp {
                    expected: holder.mako_timestamp().get(),
                    record: view.mako_timestamp().get(),
                });
            }
            return holder.materialize(
                view.table_id(),
                view.key(),
                view.value(),
                self.config.max_record_bytes,
            );
        }

        queued.materialize(self.config.max_record_bytes)
    }

    /// Classify a replay-materialization failure and retain structural errors
    /// as a permanent queue-health condition.
    ///
    /// Allocation failure is transient: the Ready slot remains untouched and
    /// a future background or synchronous consumer may retry materialization.
    fn record_failure_outcome(&self, sequence: CommitSeq, error: RecordError) -> ProcessOutcome {
        if error == RecordError::AllocationFailed {
            return ProcessOutcome::RecordFailed { sequence, error };
        }

        let failure = {
            let mut state = lock_recover(&self.state);
            // See the matching ordering argument in the unknown-outcome path.
            self.unhealthy.store(true, Ordering::Release);
            let replace = state
                .permanent_record_failure
                .as_ref()
                .is_none_or(|current| sequence < current.sequence);
            if replace {
                state.permanent_record_failure = Some(PermanentRecordFailure {
                    sequence,
                    source: error,
                });
            }
            state
                .permanent_record_failure
                .clone()
                .expect("a structural record failure was just latched")
        };

        // Every kind of waiter must re-check queue health. In particular, a
        // capacity waiter must not remain parked behind the malformed Ready
        // slot and an out-of-order publisher must not wait forever for an
        // acknowledgement frontier that can no longer be safely consumed.
        self.changed.notify_all();
        self.acknowledgement_changed.notify_all();
        self.capacity_available.notify_all();

        ProcessOutcome::RecordFailed {
            sequence: failure.sequence,
            error: failure.source,
        }
    }

    /// Number of explicit apply barriers registered for publication wakeups.
    #[cfg(test)]
    pub(crate) fn activity_waiter_count(&self) -> usize {
        self.activity_waiters.load(Ordering::SeqCst)
    }

    pub(crate) fn wait_for_activity(&self, timeout: Duration) {
        let mut state = lock_recover(&self.state);
        let waiter = ActivityWaiter::new(&self.activity_waiters);
        self.import_bound_locked(&mut state);
        if self.front_has_atomic_publication_locked(&state) {
            // Publication may have raced between the caller's preceding
            // process_front() and its wait call. Registration-before-recheck
            // makes the no-notify fast path safe in either ordering.
            drop(waiter);
            return;
        }
        state = wait_timeout_recover(&self.changed, state, timeout);
        drop(waiter);
        drop(state);
    }

    pub(crate) fn retry_delay(&self) -> Duration {
        if lock_recover(&self.state).permanent_record_failure.is_some() {
            // A permanent structural failure cannot improve through retrying,
            // so keep the worker parked instead of re-decoding the same bytes
            // at the ordinary transient-error cadence. Runtime checks its
            // stop flag independently in bounded park intervals.
            Duration::MAX
        } else {
            self.config.retry_delay
        }
    }

    #[inline(always)]
    pub(crate) fn ensure_no_unknown(&self) -> Result<(), ApplyError> {
        if !self.unhealthy.load(Ordering::Acquire) {
            return Ok(());
        }
        self.ensure_no_unknown_slow()
    }

    #[cold]
    #[inline(never)]
    fn ensure_no_unknown_slow(&self) -> Result<(), ApplyError> {
        match lock_recover(&self.state).health_error() {
            Some(error) => Err(error),
            None => Ok(()),
        }
    }

    pub(crate) fn wake_waiters(&self) {
        self.changed.notify_all();
        self.acknowledgement_changed.notify_all();
        self.descriptor_available.notify_all();
        self.capacity_available.notify_all();
    }

    fn wait_for_apply_progress(&self, target: u64) {
        let mut state = lock_recover(&self.state);
        self.import_bound_locked(&mut state);
        if self.apply_wait_satisfied_locked(&state, target) {
            return;
        }
        let waiter = ActivityWaiter::new(&self.activity_waiters);
        // Register before the second predicate read. Together with the
        // publisher's SeqCst acknowledgement/count ordering, this is the
        // no-lost-wakeup half of the conditional notification handshake.
        self.import_bound_locked(&mut state);
        if self.apply_wait_satisfied_locked(&state, target) {
            drop(waiter);
            return;
        }

        // Holding `state` until the wait atomically releases it closes the
        // usual predicate/notification gap for notifying publishers. SPSC and
        // trusted concurrent publication intentionally avoid that shared
        // notification path, so every mode uses a bounded poll: a missed
        // advisory wake can delay but never strand a barrier.
        state = wait_timeout_recover(&self.changed, state, Duration::from_millis(10));
        drop(waiter);
        drop(state);
    }

    fn front_has_atomic_publication_locked(&self, state: &State) -> bool {
        state.queue.front().is_some_and(|slot| {
            slot.state == (SlotState::Prepared { pinned: false })
                && slot.sequence.get() <= self.acknowledged.load(Ordering::SeqCst)
                && self.is_record_published(slot.sequence, Ordering::SeqCst)
        })
    }

    fn apply_wait_satisfied_locked(&self, state: &State, target: u64) -> bool {
        state.applied.sequence >= target
            || state.apply_health_error_through(target).is_some()
            || state.queue.front().is_some_and(|slot| {
                slot.state == SlotState::Ready
                    || (slot.state == (SlotState::Prepared { pinned: false })
                        && slot.sequence.get() <= self.acknowledged.load(Ordering::SeqCst)
                        && self.is_record_published(slot.sequence, Ordering::SeqCst))
            })
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Resolution {
    Publish,
    PinUnknown,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum DropAction {
    PinUnknown,
    Done,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum BindSequenceMode {
    Concurrent,
    /// The caller's native ordering exclusion covers every other binder for
    /// this Writeback until the callback returns.
    ExternallySerialized,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum RetryProgress {
    TargetApplied,
    FailedSequenceApplied,
    NoProgress,
}

/// RAII registration for the locked capacity-wait protocol.
struct CapacityWaiter<'a>(&'a AtomicUsize);

impl Drop for CapacityWaiter<'_> {
    fn drop(&mut self) {
        let prior = self.0.fetch_sub(1, Ordering::AcqRel);
        debug_assert_ne!(prior, 0, "capacity waiter registration underflow");
    }
}

/// Registered participant in the conditional `changed` notification handoff.
struct ActivityWaiter<'a>(&'a AtomicUsize);

impl<'a> ActivityWaiter<'a> {
    fn new(waiters: &'a AtomicUsize) -> Self {
        waiters.fetch_add(1, Ordering::SeqCst);
        Self(waiters)
    }
}

impl Drop for ActivityWaiter<'_> {
    fn drop(&mut self) {
        let prior = self.0.fetch_sub(1, Ordering::SeqCst);
        debug_assert_ne!(prior, 0, "activity waiter registration underflow");
    }
}

/// Registered participant in the dense-acknowledgement notification handoff.
struct AcknowledgementWaiter<'a>(&'a CacheLineAtomicUsize);

impl<'a> AcknowledgementWaiter<'a> {
    fn new(waiters: &'a CacheLineAtomicUsize) -> Self {
        waiters.fetch_add(1, Ordering::SeqCst);
        Self(waiters)
    }
}

impl Drop for AcknowledgementWaiter<'_> {
    fn drop(&mut self) {
        // Registration and the post-registration predicate read carry the
        // handshake. A stale nonzero observation during deregistration merely
        // causes one unnecessary notifier lock.
        let prior = self.0.fetch_sub(1, Ordering::Relaxed);
        debug_assert_ne!(prior, 0, "acknowledgement waiter registration underflow");
    }
}

fn retry_progress(applied: u64, target: u64, failed: CommitSeq) -> RetryProgress {
    if applied >= target {
        RetryProgress::TargetApplied
    } else if applied >= failed.get() {
        RetryProgress::FailedSequenceApplied
    } else {
        RetryProgress::NoProgress
    }
}

impl<B: Blobs> Writeback<B> {
    /// Publish the exact ordinary holder success using only scalar state.
    ///
    /// The producer cursor is advanced before the health read so a concurrent
    /// fail-stop latch cannot leave an accepted generation reusable. `true`
    /// means the one ACK Release completed publication. `false` means the
    /// caller must immediately invoke
    /// [`Self::publish_native_holder_single_cold`] with the accepted timestamp
    /// and exact extent; the producer cursor has already advanced in either
    /// case.
    ///
    /// # Safety
    ///
    /// Native must have definitely committed and sealed `sequence` in this
    /// queue's holder pool, and `producer` must still be the unique lease which
    /// retained that future generation.
    #[inline(always)]
    pub(crate) unsafe fn try_publish_native_holder_single(
        &self,
        producer: &SingleProducerState,
        sequence: NonZeroU64,
    ) -> bool {
        producer.accept_raw(sequence);
        if self.unhealthy.load(Ordering::Acquire) {
            return false;
        }

        debug_assert_eq!(
            self.acknowledged.load(Ordering::Relaxed).checked_add(1),
            Some(sequence.get())
        );
        self.acknowledged.store(sequence.get(), Ordering::Release);
        #[cfg(test)]
        self.acknowledgement_changed.notify_all();
        true
    }

    /// Preserve deterministic unit-test waiter wakeups after native performs
    /// the fused ACK store. Production consumers use bounded polling and this
    /// method is compiled out entirely there.
    #[cfg(test)]
    pub(crate) fn notify_fused_holder_published_for_test(&self) {
        self.acknowledgement_changed.notify_all();
    }

    /// Retain a definitely committed holder behind a previously latched
    /// write-back barrier.
    #[cold]
    #[inline(never)]
    pub(crate) fn publish_native_holder_single_cold(
        &self,
        sequence: NonZeroU64,
        mako_timestamp: MakoTimestamp,
        exact_record_bytes: NonZeroU32,
    ) -> Result<CommitSeq, ResolveError> {
        let sequence =
            CommitSeq::new(sequence.get()).expect("a native holder reservation is always nonzero");
        // SAFETY: the unique producer already accepted this exact generation;
        // the unhealthy fast-path observation requires retaining it in the
        // established cell/state protocol rather than advancing the ACK tail.
        unsafe {
            self.publication_cell(sequence)
                .publish_bound_single_reserved(
                    self.cold_publication_cell(sequence),
                    sequence,
                    self.publication_shift,
                )
        };
        self.attach_record(
            QueueToken::new(sequence),
            QueuedCommitRecord::Holder(DeferredOnePutRecord::new(
                sequence,
                mako_timestamp,
                exact_record_bytes.get() as usize,
            )),
        );
        self.publish_record(QueueToken::new(sequence))?;
        Ok(sequence)
    }

    /// Pin an accepted holder whose native visibility is not definite.
    ///
    /// # Safety
    ///
    /// Native must have sealed this exact generation before returning its
    /// accepted timestamp, and `producer` must own the retained generation.
    #[cold]
    #[inline(never)]
    pub(crate) unsafe fn pin_native_holder_sealed_unknown_single(
        &self,
        producer: &SingleProducerState,
        sequence: NonZeroU64,
        mako_timestamp: MakoTimestamp,
        exact_record_bytes: NonZeroU32,
    ) -> Result<CommitSeq, ResolveError> {
        producer.accept_raw(sequence);
        let sequence =
            CommitSeq::new(sequence.get()).expect("a native holder reservation is always nonzero");
        // SAFETY: this method's accepted-generation contract retains the exact
        // turn while the locked fail-stop state becomes visible.
        unsafe {
            self.publication_cell(sequence)
                .publish_bound_single_reserved(
                    self.cold_publication_cell(sequence),
                    sequence,
                    self.publication_shift,
                )
        };
        self.attach_record(
            QueueToken::new(sequence),
            QueuedCommitRecord::Holder(DeferredOnePutRecord::new(
                sequence,
                mako_timestamp,
                exact_record_bytes.get() as usize,
            )),
        );
        self.pin_unknown_record(QueueToken::new(sequence))?;
        Ok(sequence)
    }

    /// Pin an accepted terminal which failed to seal its holder witness.
    #[cold]
    #[inline(never)]
    pub(crate) fn pin_native_holder_unsealed_unknown_single(
        &self,
        producer: &SingleProducerState,
        sequence: NonZeroU64,
    ) -> Result<CommitSeq, ResolveError> {
        producer.accept_raw(sequence);
        let sequence =
            CommitSeq::new(sequence.get()).expect("a native holder reservation is always nonzero");
        // SAFETY: the retained unique generation is being made visible only
        // to the locked pin path; no replayable holder descriptor is attached.
        unsafe {
            self.publication_cell(sequence)
                .publish_bound_single_reserved(
                    self.cold_publication_cell(sequence),
                    sequence,
                    self.publication_shift,
                )
        };
        self.pin_unknown_record(QueueToken::new(sequence))?;
        Ok(sequence)
    }
}

/// Detached capacity claim for the trusted native one-Put terminal.
///
/// Unlike [`DetachedPermit`], this hot representation has no record-kind enum,
/// optional buffer, or cold legacy ownership. After native validation, the
/// accepted sequence selects either its stable arena block or the matching
/// deferred holder generation. The arena extent remains available as the
/// compatibility and failpoint path.
pub(crate) struct NativeArenaPermit<'a, B: Blobs> {
    owner: &'a Writeback<B>,
    exact_record_bytes: usize,
    single_sequence: Option<CommitSeq>,
    single_producer: Option<&'a SingleProducerState>,
    owns_claim: bool,
}

/// Exact arena address selected while an SPSC sequence is still invisible.
///
/// The native-facing value carries the sequence and capacity used to seal the
/// record. Keeping the raw address alongside it avoids re-deriving the arena
/// block after the terminal returns, without enlarging every detached permit.
#[derive(Clone, Copy)]
pub(crate) struct NativeArenaPreselectedTarget {
    native: CommitRecordTarget,
    target: NonNull<u8>,
    sequence: CommitSeq,
}

impl NativeArenaPreselectedTarget {
    #[inline(always)]
    pub(crate) const fn native(self) -> CommitRecordTarget {
        self.native
    }
}

impl<'a, B: Blobs> NativeArenaPermit<'a, B> {
    /// Select the exact target protected by this still-invisible SPSC permit.
    ///
    /// # Safety
    ///
    /// The caller must retain the mutable, thread-affine single-producer lease
    /// until the synchronous native terminal returns. The target may be used
    /// only by that terminal and must subsequently be paired with
    /// [`Self::bind_preselected_after_native`] or
    /// [`Self::publish_preselected_committed_after_native`] if native reports
    /// acceptance.
    #[inline(always)]
    pub(crate) unsafe fn preselected_record_target(&self) -> NativeArenaPreselectedTarget {
        assert!(
            self.owns_claim,
            "target selection must own a detached claim"
        );
        let sequence = self
            .single_sequence
            .expect("preselected targets require a single-producer permit");
        let block = self.owner.publication_index(sequence);
        // SAFETY: the retained capacity credit and live unique producer lease
        // prevent reuse of this arena block through the synchronous native
        // call.
        let target = unsafe {
            self.owner
                .native_arena
                .target(block, self.exact_record_bytes)
        };
        let native_sequence =
            NonZeroU64::new(sequence.get()).expect("cache commit sequences are nonzero");
        // SAFETY: the permit owns stable, exclusive queue arena storage for
        // the advertised exact extent through the matching terminal call.
        let native = unsafe {
            CommitRecordTarget::from_raw_parts(native_sequence, target, self.exact_record_bytes)
        };
        NativeArenaPreselectedTarget {
            native,
            target,
            sequence,
        }
    }

    /// Make a native-accepted preselected SPSC record visible to the queue.
    ///
    /// This operation deliberately cannot reject a concurrently latched
    /// write-back failure. Once native publishes a nonzero accepted timestamp,
    /// binding the exact dense turn is mandatory; later resolution either
    /// acknowledges it or pins it behind the failure.
    ///
    /// # Safety
    ///
    /// `preselected` must come from this permit immediately before the one
    /// synchronous native terminal which returned `mako_timestamp`, and that
    /// terminal must have initialized the complete exact target.
    #[inline(always)]
    pub(crate) unsafe fn bind_preselected_after_native(
        &mut self,
        mako_timestamp: MakoTimestamp,
        preselected: NativeArenaPreselectedTarget,
    ) -> NativeArenaBoundReservation<'a, B> {
        assert!(self.owns_claim, "binding must own a detached claim");
        let sequence = self
            .single_sequence
            .expect("preselected binding requires a single-producer permit");
        debug_assert_eq!(preselected.sequence, sequence);
        // SAFETY: established by this method's unique-lease, exact-turn, and
        // post-acceptance contract.
        unsafe {
            self.owner.bind_single_reserved_sequence_after_acceptance(
                self.single_producer
                    .expect("preselected binding retains its producer state"),
                sequence,
            )
        };
        self.owns_claim = false;
        NativeArenaBoundReservation {
            owner: self.owner,
            token: QueueToken::new(sequence),
            mako_timestamp,
            exact_record_bytes: self.exact_record_bytes,
            target: preselected.target,
            on_drop: DropAction::PinUnknown,
        }
    }

    /// Publish the ordinary successful preselected SPSC outcome directly.
    ///
    /// This combines dense binding, complete-record attachment, READY
    /// publication, and in-order acknowledgement. It deliberately omits the
    /// intermediate BOUND turn: the unique producer still owns FREE, native's
    /// completion witness covers the exact arena bytes, and the consumer can
    /// observe neither metadata nor bytes before the READY Release.
    ///
    /// # Safety
    ///
    /// `preselected` must come from this permit immediately before a native
    /// terminal whose compact outcome proves ordinary committed visibility,
    /// successful cleanup, the same accepted timestamp, and a complete record.
    #[inline(always)]
    pub(crate) unsafe fn publish_preselected_committed_after_native(
        &mut self,
        mako_timestamp: MakoTimestamp,
        preselected: NativeArenaPreselectedTarget,
    ) -> Result<CommitSeq, ResolveError> {
        assert!(self.owns_claim, "publication must own a detached claim");
        let sequence = self
            .single_sequence
            .expect("preselected publication requires a single-producer permit");
        let producer = self
            .single_producer
            .expect("preselected publication retains its producer state");
        debug_assert_eq!(preselected.sequence, sequence);

        // SAFETY: required by this method's retained-sequence and native
        // completion contract. These scalar writes and the native arena bytes
        // remain invisible until the one dense Release publication below.
        unsafe {
            self.owner.spsc_arena_publication(sequence).install(
                sequence,
                mako_timestamp,
                self.exact_record_bytes,
            )
        };
        // Native accepted this exact dense obligation. Advance the private
        // producer cursor before any shared health/fail-stop resolution.
        producer.accept(sequence);
        self.owns_claim = false;

        if !self.owner.unhealthy.load(Ordering::Acquire) {
            debug_assert_eq!(
                self.owner
                    .acknowledged
                    .load(Ordering::Relaxed)
                    .checked_add(1),
                Some(sequence.get())
            );
            // This is simultaneously dense binding, Ready publication, and
            // caller acknowledgement. The consumer's Acquire observes every
            // native byte and scalar descriptor write above.
            self.owner
                .acknowledged
                .store(sequence.get(), Ordering::Release);
            #[cfg(test)]
            self.owner.acknowledgement_changed.notify_all();
            return Ok(sequence);
        }

        // A pre-existing fail-stop barrier is cold. Materialize the old READY
        // token so the established locked resolver can retain this known
        // committed suffix without extending the published frontier. A future
        // native-holder path must preserve this exact branch: transfer the
        // accepted generation into state/cell-owned retention before returning
        // its holder, and never advance the direct tail across the barrier.
        unsafe {
            self.owner
                .publication_cell(sequence)
                .publish_arena_ready_single_reserved(
                    self.owner.cold_publication_cell(sequence),
                    sequence,
                    self.owner.publication_shift,
                    mako_timestamp,
                    self.exact_record_bytes,
                )
        };
        self.owner
            .finish_published_locked(QueueToken::new(sequence))?;
        Ok(sequence)
    }

    /// Bind under a caller-provided native ordering exclusion.
    ///
    /// # Safety
    ///
    /// For a concurrent claim, the caller must be executing the cache-private
    /// one-Put bind callback while native excludes every other binder for this
    /// Writeback. For a single-producer claim, the
    /// thread-affine mutable lease must instead exclude every foreground
    /// terminal for the whole native call. Otherwise the load/Release-store
    /// tail allocation can race another allocator, duplicate a dense sequence,
    /// and violate the arena/cell exclusive-access proof.
    pub(crate) unsafe fn bind_externally_serialized(
        &mut self,
        mako_timestamp: MakoTimestamp,
    ) -> Result<NativeArenaBoundReservation<'a, B>, ReserveError> {
        assert!(self.owns_claim, "binding must own a detached claim");
        let sequence = match self.single_sequence {
            None => self
                .owner
                .bind_sequence(BindSequenceMode::ExternallySerialized)?,
            Some(reserved) => self.owner.bind_single_reserved_sequence(
                self.single_producer
                    .expect("single-producer arena binding retains producer state"),
                reserved,
            )?,
        };
        let block = self.owner.publication_index(sequence);
        // SAFETY: the exact BOUND turn now uniquely owns this arena block
        // through backend retirement. The SP path selected the block before
        // validation; this lookup merely retrieves its precomputed pointer.
        let target = unsafe {
            self.owner
                .native_arena
                .target(block, self.exact_record_bytes)
        };
        self.owns_claim = false;
        Ok(NativeArenaBoundReservation {
            owner: self.owner,
            token: QueueToken::new(sequence),
            mako_timestamp,
            exact_record_bytes: self.exact_record_bytes,
            target,
            on_drop: DropAction::PinUnknown,
        })
    }

    /// Adopt a sequence already assigned by native's packed order state.
    ///
    /// The exact publication cell is intentionally acquired here, after the
    /// gate has been released. This keeps arena/cache-line acquisition out of
    /// the timestamp-and-sequence critical section while retaining the same
    /// dense order. Once native reports a sequence, this operation is
    /// infallible by protocol and the resulting reservation must be published
    /// or pinned.
    ///
    /// # Safety
    ///
    /// `sequence` must be the exact successor assigned by native's packed CAS
    /// while this LocalDb held its immutable Concurrent claim. This permit must
    /// still own its pre-commit capacity claim, and native must not serialize
    /// into the returned target before this call completes.
    pub(crate) unsafe fn bind_externally_ordered(
        &mut self,
        mako_timestamp: MakoTimestamp,
        sequence: NonZeroU64,
    ) -> NativeArenaBoundReservation<'a, B> {
        if !self.owns_claim || self.single_sequence.is_some() {
            // Native has already advanced the dense tail. Releasing the claim
            // during unwinding would leave a permanent queue hole.
            std::process::abort();
        }
        // `sequence` is nonzero by type, so this conversion cannot fail.
        let sequence = CommitSeq::new(sequence.get()).unwrap_or_else(|| std::process::abort());
        let publication = self.owner.publication_cell(sequence);
        // SAFETY: the native-assigned sequence consumes this permit's unique
        // occupancy claim. Distinct live assignments cannot alias one ring
        // generation under the capacity invariant.
        unsafe {
            publication.publish_bound_preassigned(
                self.owner.cold_publication_cell(sequence),
                sequence,
                self.owner.publication_shift,
            )
        };
        self.owner.notify_descriptor_waiters();
        if sequence.get() == u64::MAX {
            self.owner.capacity_available.notify_all();
        }
        let block = self.owner.publication_index(sequence);
        // SAFETY: the exact FREE -> BOUND transition above grants this
        // reservation exclusive arena ownership through backend retirement.
        let target = unsafe {
            self.owner
                .native_arena
                .target(block, self.exact_record_bytes)
        };
        self.owns_claim = false;
        NativeArenaBoundReservation {
            owner: self.owner,
            token: QueueToken::new(sequence),
            mako_timestamp,
            exact_record_bytes: self.exact_record_bytes,
            target,
            on_drop: DropAction::PinUnknown,
        }
    }

    /// Adopt one generation which native already changed from FREE to BOUND.
    ///
    /// This is the callback-free counterpart of
    /// [`Self::bind_externally_ordered`]. Native assigned the dense sequence
    /// with its packed pair CAS, acquired the exact publication cell, and
    /// selected either the matching arena address or deferred holder before
    /// completing the record. Rust only transfers this permit's occupancy
    /// ownership into the ordinary bound-reservation RAII state. The retained
    /// arena target is meaningful only when the caller later resolves this as
    /// an arena record; holder resolution publishes the holder tag instead.
    ///
    /// # Safety
    ///
    /// `sequence` must be the accepted sequence returned by the immediately
    /// preceding same-build direct-arena or deferred-holder terminal using
    /// this permit owner's matching control. That terminal must have
    /// Release-published the exact BOUND turn and must no longer access the
    /// control, arena target, or holder after returning. The caller must use
    /// the resolution method corresponding to the terminal it invoked.
    pub(crate) unsafe fn adopt_externally_bound(
        &mut self,
        mako_timestamp: MakoTimestamp,
        sequence: NonZeroU64,
    ) -> NativeArenaBoundReservation<'a, B> {
        if !self.owns_claim || self.single_sequence.is_some() {
            // Native has already advanced the dense tail. Releasing this
            // detached credit would leave a permanent unfillable hole.
            std::process::abort();
        }
        let sequence = CommitSeq::new(sequence.get()).unwrap_or_else(|| std::process::abort());
        let publication = self.owner.publication_cell(sequence);
        let expected_bound = turn_token(sequence.get(), self.owner.publication_shift, TURN_BOUND);
        if publication.turn.load(Ordering::Acquire) != expected_bound {
            // A nonzero native order owns this exact generation. Returning or
            // unwinding without its BOUND descriptor would corrupt dense replay.
            std::process::abort();
        }
        // The exact BOUND Acquire transfers ownership from native and observes
        // retirement of the prior generation's cold state.
        if unsafe { (&*self.owner.cold_publication_cell(sequence).record.get()).is_some() } {
            std::process::abort();
        }

        self.owner.notify_descriptor_waiters();
        if sequence.get() == u64::MAX {
            self.owner.capacity_available.notify_all();
        }
        let block = self.owner.publication_index(sequence);
        // SAFETY: this masked arena block belongs to the same exact BOUND
        // generation and remains stable through backend retirement. An arena
        // terminal selected and initialized it; a holder terminal leaves it
        // unused and resolves the reservation through the holder-only method.
        let target = unsafe {
            self.owner
                .native_arena
                .target(block, self.exact_record_bytes)
        };
        self.owns_claim = false;
        NativeArenaBoundReservation {
            owner: self.owner,
            token: QueueToken::new(sequence),
            mako_timestamp,
            exact_record_bytes: self.exact_record_bytes,
            target,
            on_drop: DropAction::PinUnknown,
        }
    }

    /// Transfer an exact generation which native already published READY and
    /// release-acknowledge it for this caller.
    ///
    /// This path deliberately does not reread the publication cell. The
    /// same-build terminal's scalar witness certifies that native wrote the
    /// exact timestamp and high-bit-tagged extent before its READY Release.
    /// Descriptor waiters retain their bounded timeout fallback; avoiding a
    /// Rust notification is part of this restricted hot-path contract.
    ///
    /// # Safety
    ///
    /// `sequence` must come from the immediately preceding concurrent holder
    /// terminal using this permit and must carry its exact ordinary-success,
    /// sealed-holder, and native-READY witnesses. The permit must have been in
    /// `ManuallyDrop` across that call because a consumer may retire READY
    /// before Rust resumes. Every failure/unknown outcome must use
    /// [`Self::adopt_externally_bound`] instead.
    pub(crate) unsafe fn acknowledge_native_holder_ready_concurrent_nonblocking(
        &mut self,
        sequence: NonZeroU64,
        worker_slot: usize,
    ) -> Result<CommitSeq, ResolveError> {
        if !self.owns_claim
            || self.single_sequence.is_some()
            || !native_holder_record_supported(self.exact_record_bytes)
        {
            std::process::abort();
        }
        let sequence = CommitSeq::new(sequence.get()).unwrap_or_else(|| std::process::abort());
        let Some(caller_ack) = self.owner.trusted_caller_ack_by_worker.get(worker_slot) else {
            std::process::abort();
        };

        // Native READY made the generation consumer-owned. Disarm before the
        // health check or locked resolver can return an error; permit Drop must
        // never release occupancy after consumer retirement did so.
        self.owns_claim = false;
        self.owner
            .finish_trusted_ready_publication_concurrent(
                QueueToken::new(sequence),
                caller_ack,
            )?;
        Ok(sequence)
    }
}

impl<B: Blobs> Drop for NativeArenaPermit<'_, B> {
    fn drop(&mut self) {
        if self.owns_claim {
            self.owner.release_detached_claim(None);
            self.owns_claim = false;
        }
    }
}

/// Bound trusted one-Put record with a precomputed compatibility arena target.
///
/// Dropping an unresolved value pins its exact sequence. The record becomes
/// replayable only after an exact native completion witness is supplied to one
/// of the unsafe arena- or holder-specific resolution methods below.
pub(crate) struct NativeArenaBoundReservation<'a, B: Blobs> {
    owner: &'a Writeback<B>,
    token: QueueToken,
    mako_timestamp: MakoTimestamp,
    exact_record_bytes: usize,
    target: NonNull<u8>,
    on_drop: DropAction,
}

impl<B: Blobs> NativeArenaBoundReservation<'_, B> {
    pub(crate) const fn sequence(&self) -> CommitSeq {
        self.token.sequence()
    }

    /// Return the already-computed exact arena target.
    ///
    /// # Safety
    ///
    /// The target may be passed only to the synchronous native terminal which
    /// created this reservation. `self` must remain alive and unresolved until
    /// that call returns, and the callback must not unwind.
    pub(crate) unsafe fn native_record_target(&mut self) -> CommitRecordTarget {
        let sequence = NonZeroU64::new(self.token.sequence().get())
            .expect("cache commit sequences are nonzero");
        // SAFETY: established by this method's contract and the exact BOUND
        // turn retained by the reservation.
        unsafe {
            CommitRecordTarget::from_raw_parts(sequence, self.target, self.exact_record_bytes)
        }
    }

    /// Attach native's completed bytes and publish known success in one
    /// BOUND-to-READY Release transition.
    ///
    /// # Safety
    ///
    /// The immediately preceding trusted terminal must have returned its exact
    /// completion witness for this target and a definitely committed outcome.
    pub(crate) unsafe fn publish_completed(&mut self) -> Result<CommitSeq, ResolveError> {
        assert_eq!(self.on_drop, DropAction::PinUnknown);
        let sequence = self.token.sequence();
        self.owner.publication_cell(sequence).attach_arena_ready(
            sequence,
            self.owner.publication_shift,
            self.mako_timestamp,
            self.exact_record_bytes,
        );
        self.owner.finish_ready_publication(self.token)?;
        self.on_drop = DropAction::Done;
        Ok(sequence)
    }

    /// Publish a trusted concurrent commit without waiting for its dense
    /// acknowledgement predecessor.
    ///
    /// # Safety
    ///
    /// In addition to [`Self::publish_completed`]'s requirements, this must be
    /// the native-ordered concurrent one-Put terminal. That terminal makes an
    /// accepted sequence non-cancelable and reports success only after STO
    /// installation and cleanup have completed.
    pub(crate) unsafe fn publish_completed_concurrent_nonblocking(
        &mut self,
        worker_slot: usize,
    ) -> Result<CommitSeq, ResolveError> {
        assert_eq!(self.on_drop, DropAction::PinUnknown);
        assert!(!self.owner.single_producer);
        let sequence = self.token.sequence();
        // Validate the process-lifetime worker identity before making this
        // definitely committed record READY. The caller owns this slot until
        // its CommitWriterGuard drops after we return.
        let caller_ack = self.owner.trusted_caller_ack_slot(worker_slot);
        self.owner.publication_cell(sequence).attach_arena_ready(
            sequence,
            self.owner.publication_shift,
            self.mako_timestamp,
            self.exact_record_bytes,
        );
        self.owner
            .finish_trusted_ready_publication_concurrent(self.token, caller_ack)?;
        self.on_drop = DropAction::Done;
        Ok(sequence)
    }

    /// Publish a native-sealed deferred holder for a trusted concurrent
    /// one-Put commit.
    ///
    /// # Safety
    ///
    /// The matching native holder terminal must have returned definite commit,
    /// cleanup, and exact sealed-holder witnesses for this reservation's
    /// timestamp, sequence, and record extent.
    pub(crate) unsafe fn publish_holder_completed_concurrent_nonblocking(
        &mut self,
        worker_slot: usize,
    ) -> Result<CommitSeq, ResolveError> {
        assert_eq!(self.on_drop, DropAction::PinUnknown);
        assert!(!self.owner.single_producer);
        let sequence = self.token.sequence();
        let caller_ack = self.owner.trusted_caller_ack_slot(worker_slot);
        self.owner.publication_cell(sequence).attach_holder_ready(
            sequence,
            self.owner.publication_shift,
            self.mako_timestamp,
            self.exact_record_bytes,
        );
        self.owner
            .finish_trusted_ready_publication_concurrent(self.token, caller_ack)?;
        self.on_drop = DropAction::Done;
        Ok(sequence)
    }

    /// Retain completed bytes behind a permanently unknown native outcome.
    ///
    /// # Safety
    ///
    /// The terminal must have returned the exact completion witness for this
    /// target, but its visibility outcome is not definitely committed.
    pub(crate) unsafe fn pin_completed_unknown(&mut self) -> Result<CommitSeq, ResolveError> {
        assert_eq!(self.on_drop, DropAction::PinUnknown);
        let sequence = self.token.sequence();
        self.owner.publication_cell(sequence).attach_arena(
            sequence,
            self.owner.publication_shift,
            self.mako_timestamp,
            self.exact_record_bytes,
        );
        self.owner.pin_unknown_record(self.token)?;
        self.on_drop = DropAction::Done;
        Ok(sequence)
    }

    /// Retain a sealed holder behind a permanently unknown native outcome.
    ///
    /// # Safety
    ///
    /// Native must have returned the exact sealed-holder witness for this
    /// accepted timestamp and sequence.
    pub(crate) unsafe fn pin_holder_completed_unknown(
        &mut self,
    ) -> Result<CommitSeq, ResolveError> {
        assert_eq!(self.on_drop, DropAction::PinUnknown);
        let sequence = self.token.sequence();
        self.owner.publication_cell(sequence).attach_holder_written(
            sequence,
            self.owner.publication_shift,
            self.mako_timestamp,
            self.exact_record_bytes,
        );
        self.owner.pin_unknown_record(self.token)?;
        self.on_drop = DropAction::Done;
        Ok(sequence)
    }

    /// Pin a bound slot for which native supplied no completion witness.
    pub(crate) fn pin_unwritten_unknown(&mut self) -> Result<CommitSeq, ResolveError> {
        assert_eq!(self.on_drop, DropAction::PinUnknown);
        let sequence = self.token.sequence();
        self.owner.pin_unknown_record(self.token)?;
        self.on_drop = DropAction::Done;
        Ok(sequence)
    }
}

impl<B: Blobs> Drop for NativeArenaBoundReservation<'_, B> {
    fn drop(&mut self) {
        if self.on_drop == DropAction::Done {
            return;
        }
        let _ = catch_unwind(AssertUnwindSafe(|| {
            let _ = self.owner.pin_unknown_record(self.token);
        }));
        self.on_drop = DropAction::Done;
    }
}

/// A bounded capacity claim that has not entered the ordered commit log.
///
/// This value is created before native commit. Dropping it means Silo failed
/// before reaching the post-validation hook: capacity is released without
/// assigning a sequence or leaving a cancellation marker.
pub struct DetachedPermit<'a, B: Blobs> {
    owner: &'a Writeback<B>,
    /// Cold legacy materialized state. Boxing keeps the production native
    /// permit small without adding allocation to native bind or publication.
    prepared: Option<Box<LegacyCommitRecord>>,
    native_record: bool,
    native_buffer: Option<NativeRecordBuffer>,
    /// `Some` only in the fixed single-producer mode. Concurrent mode assigns
    /// its sequence at bind, while the exclusive producer can retain this
    /// exact future turn without publishing it or modifying shared occupancy.
    single_sequence: Option<CommitSeq>,
    single_producer: Option<&'a SingleProducerState>,
    owns_claim: bool,
}

impl<'a, B: Blobs> DetachedPermit<'a, B> {
    /// Bind this record in Silo's ordered post-validation, pre-install hook.
    ///
    /// Production callers enter this hook under native's per-database
    /// validation gate, so successful binds follow a legal Silo serialization
    /// order even though failed validations consumed no slot. This assigns the
    /// next cache sequence, embeds Mako's transaction timestamp, and publishes
    /// one Prepared descriptor for state-locked import. The method performs no heap
    /// allocation, capacity wait, backend IO, or record-length work. It can
    /// reject the transaction if an earlier commit became ambiguous after this
    /// permit was detached; the caller must then abort native commit before
    /// installation. A rejection leaves the prepared bytes on this permit so
    /// they can be destroyed after Silo returns and releases its write locks.
    /// Healthy hooks do not take the queue-state mutex; concurrent producers
    /// serialize only on the dense atomic sequence word. Fail-stop handling
    /// and the final capacity-sized tail of sequence space use the locked path.
    #[must_use = "a bound native commit must be published or pinned unknown"]
    pub fn bind(
        &mut self,
        mako_timestamp: MakoTimestamp,
    ) -> Result<BoundReservation<'a, B>, ReserveError> {
        assert!(
            !self.native_record,
            "native record permits must use bind_native"
        );
        self.bind_inner(mako_timestamp)
    }

    /// Assign a dense sequence and insert an empty Prepared slot for a record
    /// being filled directly by native code. The returned reservation must be
    /// given the completed native bytes before it is published or pinned.
    pub(crate) fn bind_native(
        &mut self,
        mako_timestamp: MakoTimestamp,
    ) -> Result<BoundReservation<'a, B>, ReserveError> {
        assert!(
            self.native_record,
            "materialized record permits must use bind"
        );
        self.bind_inner(mako_timestamp)
    }

    /// Adopt one dense sequence assigned by native's packed order state.
    ///
    /// This is the general/oversized-record counterpart of
    /// [`NativeArenaPermit::bind_externally_ordered`]. Native has already
    /// crossed the non-cancelable dense assignment point, so every invariant
    /// failure below terminates rather than unwinding and releasing occupancy.
    ///
    /// # Safety
    ///
    /// `sequence` must be the exact successor assigned while native owned the
    /// packed general-certification bit for this cache namespace. This permit
    /// must own one concurrent detached claim. The LocalDb mode claim must
    /// exclude every legacy Rust-side sequence allocator for this queue.
    pub(crate) unsafe fn bind_native_externally_ordered(
        &mut self,
        mako_timestamp: MakoTimestamp,
        sequence: NonZeroU64,
    ) -> BoundReservation<'a, B> {
        if !self.owns_claim || self.single_sequence.is_some() || !self.native_record {
            std::process::abort();
        }
        if self.prepared.is_some() || self.native_buffer.is_none() {
            std::process::abort();
        }
        let sequence = CommitSeq::new(sequence.get()).unwrap_or_else(|| std::process::abort());
        let publication = self.owner.publication_cell(sequence);
        // SAFETY: native's packed allocator and this permit's unique occupancy
        // claim select a distinct live ring generation.
        unsafe {
            publication.publish_bound_preassigned(
                self.owner.cold_publication_cell(sequence),
                sequence,
                self.owner.publication_shift,
            )
        };
        self.owner.notify_descriptor_waiters();

        let native_buffer = self
            .native_buffer
            .take()
            .map(|buffer| buffer.bind_arena(self.owner.publication_index(sequence)));
        self.owns_claim = false;
        BoundReservation {
            owner: self.owner,
            token: QueueToken::new(sequence),
            mako_timestamp,
            legacy_prepared: None,
            native_buffer,
            on_drop: DropAction::PinUnknown,
        }
    }

    fn bind_inner(
        &mut self,
        mako_timestamp: MakoTimestamp,
    ) -> Result<BoundReservation<'a, B>, ReserveError> {
        assert!(self.owns_claim, "binding must own a detached claim");
        // Legacy preparation remains boxed through the ordered hook. Its large
        // vector-owning value is finalized only on the cold legacy path after
        // native returns, so binding never copies or reallocates it.
        assert_eq!(
            self.prepared.is_none(),
            self.native_record,
            "detached permit record kind changed before binding"
        );
        assert_eq!(
            self.native_buffer.is_some(),
            self.native_record,
            "native permit lost its checked-out serialization buffer"
        );
        let sequence = match self.single_sequence {
            None => self.owner.bind_sequence(BindSequenceMode::Concurrent)?,
            Some(sequence) => self.owner.bind_single_reserved_sequence(
                self.single_producer
                    .expect("single-producer binding retains producer state"),
                sequence,
            )?,
        };
        let legacy_prepared = self.prepared.take();
        let native_buffer = self
            .native_buffer
            .take()
            .map(|buffer| buffer.bind_arena(self.owner.publication_index(sequence)));
        self.owns_claim = false;

        Ok(BoundReservation {
            owner: self.owner,
            token: QueueToken::new(sequence),
            mako_timestamp,
            legacy_prepared,
            native_buffer,
            on_drop: DropAction::PinUnknown,
        })
    }
}

impl<B: Blobs> Drop for DetachedPermit<'_, B> {
    fn drop(&mut self) {
        if self.owns_claim {
            let native_buffer = self.native_buffer.take();
            self.owner.release_detached_claim(native_buffer);
            self.owns_claim = false;
        }
    }
}

/// A Prepared ordered slot created after native validation.
///
/// Dropping this handle pins its slot because Silo may already have installed
/// the transaction. A definite native success must call [`Self::publish`]; an
/// ambiguous return may call [`Self::pin_unknown`] explicitly or simply drop.
pub struct BoundReservation<'a, B: Blobs> {
    owner: &'a Writeback<B>,
    token: QueueToken,
    mako_timestamp: MakoTimestamp,
    /// Legacy/unit-test write-set representation. Production native
    /// reservations carry only a null pointer here.
    legacy_prepared: Option<Box<LegacyCommitRecord>>,
    native_buffer: Option<NativeRecordBuffer>,
    on_drop: DropAction,
}

impl<B: Blobs> BoundReservation<'_, B> {
    /// Return the cache sequence assigned at the native serialization hook.
    pub const fn sequence(&self) -> CommitSeq {
        self.token.sequence()
    }

    /// Expose this reservation's checked-out storage to mako-local's unsafe
    /// synchronous record terminal.
    ///
    /// # Safety
    ///
    /// The returned target may be passed only to the native terminal which
    /// created this reservation. `self` must stay alive until that terminal
    /// returns, and the callback must not unwind. Moving the reservation is
    /// permitted because both arena and Vec allocations remain stable. Callers
    /// may attach the buffer as initialized only after an exact completion
    /// witness.
    pub(crate) unsafe fn native_record_target(&mut self) -> CommitRecordTarget {
        assert!(
            self.legacy_prepared.is_none(),
            "only a native Prepared reservation has raw target storage"
        );
        let buffer = self
            .native_buffer
            .as_mut()
            .expect("a native reservation owns one serialization buffer");
        let exact_record_bytes = buffer.exact_record_bytes();
        let bytes = buffer.target(&self.owner.native_arena);
        let sequence = NonZeroU64::new(self.token.sequence().get())
            .expect("cache commit sequences are nonzero");
        // SAFETY: the reservation uniquely owns this checked-out buffer. Its
        // allocation is stable in either the arena or Vec representation, and
        // the caller keeps this reservation live through the terminal.
        unsafe { CommitRecordTarget::from_raw_parts(sequence, bytes, exact_record_bytes) }
    }

    /// Accept native's exact completion witness and move the checked-out bytes
    /// into this queue reservation without allocation or copying.
    ///
    /// # Safety
    ///
    /// The immediately preceding synchronous native terminal must have
    /// returned a valid report with `record_written == true` for the target
    /// produced by [`Self::native_record_target`]. That witness must cover the
    /// canonical record for this reservation's exact sequence, bound Mako
    /// timestamp, and preflight integrity mode; background materialization
    /// verifies those fields before replay.
    pub(crate) unsafe fn attach_written_native_record(&mut self) {
        assert!(
            self.legacy_prepared.is_none(),
            "only a native Prepared reservation accepts target bytes"
        );
        let buffer = self
            .native_buffer
            .take()
            .expect("a native reservation owns one serialization buffer");
        match buffer {
            NativeRecordBuffer::Arena {
                block,
                exact_record_bytes,
            } => {
                assert_eq!(block, self.owner.publication_index(self.token.sequence()));
                self.owner
                    .attach_arena_record(self.token, self.mako_timestamp, exact_record_bytes);
            }
            owned @ NativeRecordBuffer::Owned(_) => {
                // SAFETY: required by this method's completion-witness contract.
                let record = unsafe {
                    owned.into_record(
                        &self.owner.native_arena,
                        self.token.sequence(),
                        self.mako_timestamp,
                    )
                };
                self.owner
                    .attach_record(self.token, QueuedCommitRecord::Native(record));
            }
            NativeRecordBuffer::UnboundArena { .. } => {
                panic!("a bound reservation must own an assigned arena block")
            }
        }
    }

    /// Attach the exact bytes initialized by the trusted native serializer.
    /// This is constant-time and allocation-free; decoding is deferred to the
    /// background replay path.
    pub(crate) fn attach_native_record(&mut self, encoded: Vec<u8>) {
        assert!(
            self.legacy_prepared.is_none(),
            "a materialized reservation cannot accept native bytes"
        );
        if let Some(buffer) = self.native_buffer.take() {
            self.owner.recycle_native_buffer(buffer);
        }
        let record =
            NativeCommitRecord::from_native(self.token.sequence(), self.mako_timestamp, encoded);
        self.owner
            .attach_record(self.token, QueuedCommitRecord::Native(record));
    }

    /// Publish a successfully committed transaction.
    ///
    /// A native reservation already has its complete serialized bytes attached.
    /// The legacy materialized test path finalizes its preallocated record here;
    /// either way the complete record is installed before the slot becomes
    /// Ready. If an earlier bound transaction has not published yet, this call
    /// waits without holding the queue mutex until that dense acknowledgement
    /// prefix catches up. An earlier unknown outcome instead retains this
    /// known-committed record as pinned and returns
    /// [`ResolveError::BlockedByPriorUnknown`].
    pub fn publish(&mut self) -> Result<CommitSeq, ResolveError> {
        assert_eq!(
            self.on_drop,
            DropAction::PinUnknown,
            "a bound reservation may be resolved only once"
        );
        self.finalize_once();
        assert!(
            self.native_buffer.is_none(),
            "native bytes need an exact completion witness before publication"
        );
        self.owner.resolve(self.token, Resolution::Publish)?;
        self.on_drop = DropAction::Done;
        Ok(self.token.sequence())
    }

    /// Permanently pin the slot because the native commit outcome is unknown.
    ///
    /// The write-back layer cannot safely skip such a slot. Flushes fail and
    /// new reservations are rejected until the database is reopened and
    /// recovered by a higher-level protocol.
    pub fn pin_unknown(&mut self) -> Result<CommitSeq, ResolveError> {
        assert_eq!(
            self.on_drop,
            DropAction::PinUnknown,
            "a bound reservation may be resolved only once"
        );
        self.on_drop = DropAction::PinUnknown;
        self.finalize_once();
        let native_buffer = self.native_buffer.take();
        let result = self.owner.resolve(self.token, Resolution::PinUnknown);
        if let Some(buffer) = native_buffer {
            // A bound-but-unwritten record has no replayable bytes. Its queue
            // slot remains pinned, while the unused storage can be reused.
            self.owner.recycle_native_buffer(buffer);
        }
        result?;
        self.on_drop = DropAction::Done;
        Ok(self.token.sequence())
    }

    /// Finalize and retain the exact write set at most once.
    ///
    /// All storage is preallocated. This performs only the deferred checksum
    /// scan and moves the record into this reservation, so neither publication
    /// nor fail-stop retention can fail because of allocation.
    fn finalize_once(&mut self) {
        let Some(mut prepared) = self.legacy_prepared.take() else {
            return;
        };
        prepared.finalize_in_place(self.token.sequence(), self.mako_timestamp);
        self.owner
            .attach_record(self.token, QueuedCommitRecord::Materialized(prepared));
    }
}

impl<B: Blobs> Drop for BoundReservation<'_, B> {
    fn drop(&mut self) {
        let resolution = match self.on_drop {
            DropAction::PinUnknown => Resolution::PinUnknown,
            DropAction::Done => return,
        };
        // Preserve the ambiguous write set before pinning. Drop remains
        // best-effort and panic-free even if a future finalizer invariant
        // regresses; pinning is still attempted after a contained panic.
        let _ = catch_unwind(AssertUnwindSafe(|| self.finalize_once()));
        let _ = catch_unwind(AssertUnwindSafe(|| {
            let _ = self.owner.resolve(self.token, resolution);
        }));
        if let Some(buffer) = self.native_buffer.take() {
            let _ = catch_unwind(AssertUnwindSafe(|| {
                self.owner.recycle_native_buffer(buffer)
            }));
        }
        self.on_drop = DropAction::Done;
    }
}

/// Records temporarily moved out of their queue slots by the sole consumer.
///
/// Moving ownership is the key foreground optimization: queue entries can own
/// records directly, without one separately allocated `Arc<OnceLock<_>>` per
/// transaction. The guard also preserves the old panic/retry guarantee. Until
/// an entire batch is retired, unwinding puts every record back into the exact
/// dense slot from which it came.
struct CapturedBatch<'a, B: Blobs> {
    owner: &'a Writeback<B>,
    records: Vec<QueuedCommitRecord>,
    completed: bool,
}

impl<'a, B: Blobs> CapturedBatch<'a, B> {
    fn new(owner: &'a Writeback<B>, records: Vec<QueuedCommitRecord>) -> Self {
        Self {
            owner,
            records,
            completed: false,
        }
    }

    fn restore(&mut self) {
        if self.completed {
            return;
        }
        let mut state = lock_recover(&self.owner.state);
        for record in self.records.drain(..) {
            let token = QueueToken::new(record.sequence());
            let offset = state
                .queue_offset(token)
                .expect("an applying slot cannot leave the serialized queue");
            let slot = &mut state.queue[offset];
            assert_eq!(slot.state, SlotState::Applying);
            assert!(slot.record.is_none());
            slot.record = Some(record);
            slot.state = SlotState::Ready;
        }
        self.completed = true;
    }

    fn retire(&mut self, materialized: &[crate::record::CommitRecord]) {
        assert_eq!(self.records.len(), materialized.len());
        let retired = self.records.len();
        let mut state = lock_recover(&self.owner.state);
        // Validate the whole prefix before consuming any captured ownership so
        // a future invariant panic still leaves Drop able to restore it.
        assert!(state.queue.len() >= retired);
        for (current, record) in state.queue.iter().zip(materialized) {
            assert_eq!(
                current.sequence,
                record.sequence(),
                "serialized consumer changed the captured prefix"
            );
            assert_eq!(
                current.state,
                SlotState::Applying,
                "captured slot changed state during backend IO"
            );
            assert!(
                current.record.is_none(),
                "the consumer must retain ownership until retirement"
            );
            assert!(
                self.owner
                    .is_record_published(current.sequence, Ordering::Acquire),
                "captured retirement must retain its exact Ready ring turn"
            );
        }
        for (queued, record) in self.records.drain(..).zip(materialized) {
            let sequence = queued.sequence();
            let slot = state
                .queue
                .pop_front()
                .expect("a captured record retains its applying queue slot");
            assert_eq!(slot.sequence, sequence);
            state
                .applied
                .advance(record.sequence(), record.mako_timestamp());
            if let Some(recycled) = queued.into_recycled_native() {
                self.owner.recycle_native_record(recycled);
            }
            if !self.owner.single_producer {
                self.owner.publication_cell(sequence).retire(
                    self.owner.cold_publication_cell(sequence),
                    sequence,
                    self.owner.publication_shift,
                    self.owner.publication_cells.len(),
                );
            }
        }
        // Every exact ring turn is FREE before capacity becomes claimable. The
        // MPMC profile releases its aggregate claims in one cross-core RMW;
        // the SPSC producer derives capacity from dense tail minus the applied
        // frontier and therefore deliberately never mutates Occupancy.
        if !self.owner.single_producer {
            self.owner.occupied.release_many(retired);
        } else {
            // Release only after every record/arena read and cold ownership
            // recycle above. A producer which Acquires this frontier may now
            // overwrite the corresponding ring generations.
            self.owner
                .applied_frontier
                .store(state.applied.sequence, Ordering::Release);
        }
        self.completed = true;
    }
}

impl<B: Blobs> Drop for CapturedBatch<'_, B> {
    fn drop(&mut self) {
        self.restore();
    }
}

#[derive(Debug)]
pub(crate) enum ProcessOutcome {
    Idle,
    Blocked,
    Pinned(CommitSeq),
    Advanced,
    BackendFailed {
        sequence: CommitSeq,
        error: BlobError,
    },
    RecordFailed {
        sequence: CommitSeq,
        error: RecordError,
    },
}

fn lock_recover<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn wait_timeout_recover<'a, T>(
    condvar: &Condvar,
    guard: MutexGuard<'a, T>,
    timeout: Duration,
) -> MutexGuard<'a, T> {
    match condvar.wait_timeout(guard, timeout) {
        Ok((guard, _)) => guard,
        Err(poisoned) => poisoned.into_inner().0,
    }
}

#[cfg(test)]
mod tests {
    use std::panic::{AssertUnwindSafe, catch_unwind};
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::{Arc, Barrier, mpsc};
    use std::time::Instant;

    use mrx_core::BlobOp;
    use mrx_core::fakes::MemBlobs;

    use super::*;

    const TABLE: u64 = 1;

    /// Guard the native ACK path against accidentally re-inlining the legacy
    /// five-vector record state. The old representation was 0x110 bytes and
    /// generated two 272-byte moves per ordinary cache commit.
    #[test]
    fn native_reservation_handles_stay_compact() {
        const PRE_OPTIMIZATION_BOUND_BYTES: usize = 0x110;
        let detached = std::mem::size_of::<DetachedPermit<'static, MemBlobs>>();
        let bound = std::mem::size_of::<BoundReservation<'static, MemBlobs>>();
        let queued = std::mem::size_of::<QueuedCommitRecord>();
        let arena_permit = std::mem::size_of::<NativeArenaPermit<'static, MemBlobs>>();
        let holder_reservation = std::mem::size_of::<NonZeroU64>();
        eprintln!(
            "native reservation sizes: bound {PRE_OPTIMIZATION_BOUND_BYTES} -> {bound} bytes; detached={detached}; queued={queued}; arena_permit={arena_permit}; holder_reservation={holder_reservation}"
        );
        assert!(
            bound <= 64,
            "native bound handle regressed to {bound} bytes"
        );
        assert!(
            detached <= 64,
            "native detached handle regressed to {detached} bytes"
        );
        assert!(
            queued <= 64,
            "queued native record regressed to {queued} bytes"
        );
        assert_eq!(std::mem::size_of::<PublicationCell>(), 64);
        assert!(
            arena_permit <= 40,
            "trusted arena permit should remain a scalar hot-path handle"
        );
        assert_eq!(
            holder_reservation,
            std::mem::size_of::<u64>(),
            "trusted holder reservation must remain one scalar"
        );
        assert!(
            std::mem::size_of::<NativeArenaBoundReservation<'static, MemBlobs>>() <= 64,
            "trusted arena reservation should fit one cache line"
        );
    }

    #[cfg(target_pointer_width = "64")]
    #[test]
    fn fused_control_saturates_oversized_record_cap_without_rejecting_queue() {
        let backend = Arc::new(MemBlobs::new());
        let mut config = config(2, 0);
        config.max_record_bytes = u32::MAX as usize + 1;
        let writeback = Writeback::new_single(backend, 0, config).unwrap();
        let producer = writeback.single_producer_state();
        assert_eq!(
            producer.fused_holder_control().max_record_bytes(),
            u32::MAX,
            "every representable native candidate must remain admitted"
        );
    }

    #[test]
    fn healthy_claim_and_bind_do_not_take_the_queue_state_mutex() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(backend, 0, config(2, 0)).unwrap();
        let state = lock_recover(&writeback.state);

        std::thread::scope(|scope| {
            let (bound_tx, bound_rx) = mpsc::channel();
            let (release_tx, release_rx) = mpsc::channel();
            let producer_writeback = &writeback;
            let producer = scope.spawn(move || {
                let mut permit = producer_writeback
                    .reserve(vec![put(b"fast", b"path")])
                    .unwrap();
                let bound = permit.bind(mako_timestamp_of(1)).unwrap();
                bound_tx.send(bound.sequence()).unwrap();
                release_rx.recv().unwrap();
                // Drop pins the unresolved reservation and intentionally runs
                // only after the test releases `state` below.
                drop(bound);
            });

            assert_eq!(
                bound_rx
                    .recv_timeout(Duration::from_secs(1))
                    .expect("healthy reserve+bind blocked on Writeback::state")
                    .get(),
                1
            );
            drop(state);
            release_tx.send(()).unwrap();
            producer.join().unwrap();
        });
    }

    #[test]
    fn packed_occupancy_credit_batch_is_bounded_and_scalar_for_small_queues() {
        let all_worker_share = mako_local::MAX_WORKERS * PACKED_OCCUPANCY_CREDIT_HOARD_DIVISOR;
        assert_eq!(packed_occupancy_credit_batch_size(1), 1);
        assert_eq!(packed_occupancy_credit_batch_size(all_worker_share), 1);
        assert_eq!(packed_occupancy_credit_batch_size(all_worker_share * 2), 2);
        assert_eq!(packed_occupancy_credit_batch_size(1_048_576), 16);
        assert_eq!(
            packed_occupancy_credit_batch_size(
                all_worker_share * (PACKED_OCCUPANCY_CREDIT_BATCH_MAX + 1)
            ),
            PACKED_OCCUPANCY_CREDIT_BATCH_MAX
        );

        for capacity in [
            1,
            all_worker_share,
            all_worker_share * 2,
            1_048_576,
            all_worker_share * 64,
        ] {
            let batch = packed_occupancy_credit_batch_size(capacity);
            let maximum_idle = mako_local::MAX_WORKERS * (batch - 1);
            assert!(
                maximum_idle <= capacity / PACKED_OCCUPANCY_CREDIT_HOARD_DIVISOR,
                "capacity {capacity} batch {batch} can hoard {maximum_idle} idle rights"
            );
        }
    }

    #[test]
    fn packed_occupancy_credit_reuses_one_shared_batch_claim() {
        let backend = Arc::new(MemBlobs::new());
        let mut writeback = Writeback::new(backend, 0, config(2, 0)).unwrap();
        writeback.packed_occupancy_credit_batch = 2;

        let first = writeback
            .reserve_native_arena_fast_packed(64, 0)
            .unwrap()
            .expect("the first packed reservation fits the arena");
        assert_eq!(writeback.occupied.load(), 2);
        assert_eq!(
            writeback
                .packed_occupancy_credit_slot(0)
                .load(Ordering::Relaxed),
            1
        );
        drop(first);
        assert_eq!(writeback.occupied.load(), 1);

        let second = writeback
            .reserve_native_arena_fast_packed(64, 0)
            .unwrap()
            .expect("the owner-local right admits a second reservation");
        assert_eq!(
            writeback.occupied.load(),
            1,
            "consuming an idle right must not touch shared occupancy"
        );
        assert_eq!(
            writeback
                .packed_occupancy_credit_slot(0)
                .load(Ordering::Relaxed),
            0
        );
        drop(second);
        assert_eq!(writeback.occupied.load(), 0);
    }

    #[test]
    fn packed_occupancy_batch_preserves_partial_tail_capacity() {
        let backend = Arc::new(MemBlobs::new());
        let mut writeback = Writeback::new(backend, 0, config(3, 0)).unwrap();
        writeback.packed_occupancy_credit_batch = 2;

        let first = writeback
            .reserve_native_arena_fast_packed(64, 0)
            .unwrap()
            .unwrap();
        let second = writeback
            .reserve_native_arena_fast_packed(64, 1)
            .unwrap()
            .unwrap();
        assert_eq!(writeback.occupied.load(), 3);
        assert_eq!(
            writeback
                .packed_occupancy_credit_slot(0)
                .load(Ordering::Relaxed),
            1
        );
        assert_eq!(
            writeback
                .packed_occupancy_credit_slot(1)
                .load(Ordering::Relaxed),
            0,
            "the final capacity unit must admit an active permit, not an idle right"
        );

        drop(first);
        drop(second);
        assert_eq!(writeback.occupied.load(), 1);
        assert_eq!(writeback.reclaim_all_packed_occupancy_credits(), 1);
        assert_eq!(writeback.occupied.load(), 0);
    }

    #[test]
    fn packed_credit_scalar_capacity_pressure_reclaims_idle_rights() {
        let backend = Arc::new(MemBlobs::new());
        let mut writeback = Writeback::new(backend, 0, config(2, 0)).unwrap();
        writeback.packed_occupancy_credit_batch = 2;

        let packed = writeback
            .reserve_native_arena_fast_packed(64, 0)
            .unwrap()
            .unwrap();
        assert_eq!(writeback.occupied.load(), 2);
        let ordinary = writeback
            .reserve_native_arena_fast(64)
            .unwrap()
            .expect("the slow scalar path must reclaim an idle packed right");
        assert_eq!(writeback.occupied.load(), 2);
        assert_eq!(
            writeback
                .packed_occupancy_credit_slot(0)
                .load(Ordering::Relaxed),
            0
        );

        drop(ordinary);
        drop(packed);
        assert_eq!(writeback.occupied.load(), 0);
    }

    #[test]
    fn packed_credit_general_oversized_and_single_producer_paths_are_isolated() {
        let backend = Arc::new(MemBlobs::new());
        let mut concurrent = Writeback::new(Arc::clone(&backend), 0, config(4, 0)).unwrap();
        concurrent.packed_occupancy_credit_batch = 3;

        let general = concurrent
            .reserve(vec![put(b"general", b"record")])
            .unwrap();
        let oversized = concurrent
            .reserve_native(concurrent.native_arena.block_bytes() + 1)
            .unwrap();
        assert_eq!(concurrent.occupied.load(), 2);
        assert!(
            concurrent
                .packed_occupancy_credits_by_worker
                .iter()
                .all(|credit| credit.load(Ordering::Relaxed) == 0),
            "ordinary general and oversized reservations remain scalar"
        );
        drop(general);
        drop(oversized);
        assert_eq!(concurrent.occupied.load(), 0);

        let single = Writeback::new_single(backend, 0, config(4, 0)).unwrap();
        assert_eq!(single.packed_occupancy_credit_batch, 1);
        assert!(single.packed_occupancy_credits_by_worker.is_empty());
        let producer = single.single_producer_state();
        let permit = single
            .reserve_native_arena_fast_single(&producer, 64)
            .unwrap()
            .unwrap();
        drop(permit);
        single.reclaim_packed_occupancy_credits_for_shutdown();
        assert_eq!(single.occupied.load(), 0);
    }

    #[test]
    fn packed_credit_pressure_rescans_a_refill_published_after_its_first_scan() {
        let backend = Arc::new(MemBlobs::new());
        let mut writeback = Writeback::new(backend, 0, config(2, 0)).unwrap();
        writeback.packed_occupancy_credit_batch = 2;
        let credit = writeback.packed_occupancy_credit_slot(0);

        // Pause a refill after its aggregate claim but before its idle-right
        // store. A pressure scan may legitimately linearize in this window.
        assert_eq!(writeback.occupied.try_claim_batch_relaxed(2, 2), 2);
        assert_eq!(writeback.reclaim_all_packed_occupancy_credits(), 0);
        credit.store(1, Ordering::Release);
        assert!(!writeback.occupied.try_claim(2));

        // The locked slow path loops after that failed scalar claim. Its next
        // scan sees the late publication and restores the final capacity unit.
        assert_eq!(writeback.reclaim_all_packed_occupancy_credits(), 1);
        assert!(writeback.occupied.try_claim(2));
        writeback.occupied.release_many(2);
        assert_eq!(writeback.occupied.load(), 0);
    }

    #[test]
    fn packed_credit_exclusive_shutdown_reclaims_and_cache_allocations_are_isolated() {
        let backend = Arc::new(MemBlobs::new());
        let mut first_writeback = Writeback::new(Arc::clone(&backend), 0, config(3, 0)).unwrap();
        first_writeback.packed_occupancy_credit_batch = 3;
        let first = first_writeback
            .reserve_native_arena_fast_packed(64, 0)
            .unwrap()
            .unwrap();
        assert_eq!(first_writeback.occupied.load(), 3);

        let mut second_writeback = Writeback::new(backend, 0, config(3, 0)).unwrap();
        second_writeback.packed_occupancy_credit_batch = 3;
        let second = second_writeback
            .reserve_native_arena_fast_packed(64, 0)
            .unwrap()
            .unwrap();
        assert_eq!(second_writeback.occupied.load(), 3);
        assert_eq!(first_writeback.occupied.load(), 3);
        drop(first);
        drop(second);
        assert_eq!(first_writeback.occupied.load(), 2);
        assert_eq!(second_writeback.occupied.load(), 2);

        first_writeback.reclaim_packed_occupancy_credits_for_shutdown();
        first_writeback.reclaim_packed_occupancy_credits_for_shutdown();
        second_writeback.reclaim_packed_occupancy_credits_for_shutdown();
        assert_eq!(first_writeback.occupied.load(), 0);
        assert_eq!(second_writeback.occupied.load(), 0);
    }

    #[test]
    fn packed_credit_owner_consumption_races_reclamation_without_capacity_loss() {
        const OWNER_ITERATIONS: usize = 20_000;
        const RECLAIM_ITERATIONS: usize = 5_000;

        let backend = Arc::new(MemBlobs::new());
        let mut writeback = Writeback::new(backend, 0, config(8, 0)).unwrap();
        writeback.packed_occupancy_credit_batch = 4;
        let writeback = Arc::new(writeback);
        let start = Arc::new(Barrier::new(2));

        let owner_writeback = Arc::clone(&writeback);
        let owner_start = Arc::clone(&start);
        let owner = std::thread::spawn(move || {
            owner_start.wait();
            for _ in 0..OWNER_ITERATIONS {
                let permit = owner_writeback
                    .reserve_native_arena_fast_packed(64, 0)
                    .unwrap()
                    .expect("one owner and a reclaimer cannot exhaust eight rights");
                drop(permit);
            }
        });

        start.wait();
        for _ in 0..RECLAIM_ITERATIONS {
            writeback.reclaim_all_packed_occupancy_credits();
            std::hint::spin_loop();
        }
        owner.join().unwrap();
        writeback.reclaim_packed_occupancy_credits_for_shutdown();
        assert_eq!(writeback.occupied.load(), 0);
        assert!(writeback
            .packed_occupancy_credits_by_worker
            .iter()
            .all(|credit| credit.load(Ordering::Relaxed) == 0));
    }

    fn put(key: &[u8], value: &[u8]) -> Mutation {
        Mutation::Put {
            table_id: TABLE,
            key: key.to_vec(),
            value: value.to_vec(),
        }
    }

    fn delete(key: &[u8]) -> Mutation {
        Mutation::Delete {
            table_id: TABLE,
            key: key.to_vec(),
        }
    }

    fn config(capacity: usize, max_apply_retries: usize) -> WritebackConfig {
        WritebackConfig {
            capacity,
            // Legacy tests exercise one-at-a-time application semantics. New
            // prefix tests opt into larger batches explicitly below.
            max_batch_records: 1,
            max_apply_retries,
            retry_delay: Duration::from_millis(1),
            ..WritebackConfig::default()
        }
    }

    fn batch_config(
        capacity: usize,
        max_batch_records: usize,
        max_batch_bytes: usize,
    ) -> WritebackConfig {
        WritebackConfig {
            max_batch_records,
            max_batch_bytes,
            ..config(capacity, 0)
        }
    }

    fn record_encoded_len(mutations: Vec<Mutation>) -> usize {
        PreparedCommitRecord::prepare(mutations, DEFAULT_MAX_RECORD_BYTES)
            .unwrap()
            .bind(CommitSeq::new(1).unwrap(), mako_timestamp_of(1))
            .finalize()
            .encoded()
            .len()
    }

    fn encoded_native_record(
        raw_sequence: u64,
        raw_mako_timestamp: u32,
        mutations: Vec<Mutation>,
    ) -> Vec<u8> {
        PreparedCommitRecord::prepare(mutations, DEFAULT_MAX_RECORD_BYTES)
            .unwrap()
            .bind(
                CommitSeq::new(raw_sequence).expect("test sequence is nonzero"),
                mako_timestamp_of(raw_mako_timestamp),
            )
            .finalize()
            .encoded()
            .to_vec()
    }

    fn publish_native_bytes<B: Blobs>(
        writeback: &Writeback<B>,
        raw_mako_timestamp: u32,
        encoded: Vec<u8>,
    ) -> CommitSeq {
        let mut permit = writeback.reserve_native(encoded.len()).unwrap();
        let mut reservation = permit
            .bind_native(mako_timestamp_of(raw_mako_timestamp))
            .unwrap();
        reservation.attach_native_record(encoded);
        reservation.publish().unwrap()
    }

    fn fill_fast_arena_reservation<B: Blobs>(
        reservation: &mut NativeArenaBoundReservation<'_, B>,
        encoded: &[u8],
    ) {
        assert_eq!(reservation.exact_record_bytes, encoded.len());
        // SAFETY: the live bound reservation exclusively owns this exact arena
        // extent. Copying a complete encoded test record simulates native's
        // synchronous initialization and exact completion witness.
        unsafe {
            std::ptr::copy_nonoverlapping(
                encoded.as_ptr(),
                reservation.target.as_ptr(),
                encoded.len(),
            );
        }
    }

    fn publish_fast_arena<B: Blobs>(
        writeback: &Writeback<B>,
        raw_mako_timestamp: u32,
        mutations: Vec<Mutation>,
    ) -> CommitSeq {
        let exact_record_bytes = record_encoded_len(mutations.clone());
        let mut permit = writeback
            .reserve_native_arena_fast(exact_record_bytes)
            .unwrap()
            .expect("small test record uses the common arena");
        // SAFETY: this single-threaded helper is the only queue binder until
        // the method returns, exactly matching the external gate contract.
        let mut reservation = unsafe {
            permit
                .bind_externally_serialized(mako_timestamp_of(raw_mako_timestamp))
                .unwrap()
        };
        let sequence = reservation.sequence();
        let encoded = encoded_native_record(sequence.get(), raw_mako_timestamp, mutations);
        fill_fast_arena_reservation(&mut reservation, &encoded);
        // SAFETY: the full exact target was initialized immediately above.
        unsafe { reservation.publish_completed().unwrap() }
    }

    fn publish_fast_arena_single<B: Blobs>(
        writeback: &Writeback<B>,
        producer: &SingleProducerState,
        raw_mako_timestamp: u32,
        mutations: Vec<Mutation>,
    ) -> CommitSeq {
        let exact_record_bytes = record_encoded_len(mutations.clone());
        let mut permit = writeback
            .reserve_native_arena_fast_single(producer, exact_record_bytes)
            .unwrap()
            .expect("small test record uses the common arena");
        // SAFETY: this helper models the unique thread-affine foreground lease
        // and retains it through target initialization and post-accept bind.
        let preselected = unsafe { permit.preselected_record_target() };
        let sequence = preselected.sequence;
        let encoded = encoded_native_record(sequence.get(), raw_mako_timestamp, mutations);
        // SAFETY: the live invisible permit uniquely owns this exact arena
        // extent. Copying the complete encoded record simulates native's
        // accepted serialization before it returns the timestamp witness.
        unsafe {
            std::ptr::copy_nonoverlapping(
                encoded.as_ptr(),
                preselected.target.as_ptr(),
                encoded.len(),
            );
        }
        // SAFETY: the target and timestamp represent the same simulated native
        // acceptance, the exact extent is initialized, and the unique producer
        // exclusion remains live. This exercises the direct frontier protocol
        // used by the production trusted one-Put path.
        unsafe {
            permit
                .publish_preselected_committed_after_native(
                    mako_timestamp_of(raw_mako_timestamp),
                    preselected,
                )
                .unwrap()
        }
    }

    #[test]
    fn single_producer_abort_before_bind_reuses_the_unpublished_turn() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new_single(backend, 0, config(1, 0)).unwrap();
        let producer = writeback.single_producer_state();
        let mutations = vec![put(b"abort", b"before-bind")];
        let exact_record_bytes = record_encoded_len(mutations.clone());

        let permit = writeback
            .reserve_native_arena_fast_single(&producer, exact_record_bytes)
            .unwrap()
            .unwrap();
        assert_eq!(writeback.next_bound.load(Ordering::Relaxed), 0);
        assert_eq!(writeback.occupied.load(), 0);
        drop(permit);
        assert_eq!(writeback.next_bound.load(Ordering::Relaxed), 0);
        assert_eq!(writeback.queue_len(), 0);

        let sequence = publish_fast_arena_single(&writeback, &producer, 1, mutations);
        assert_eq!(sequence.get(), 1, "an abort must leave no sequence hole");
        assert_eq!(writeback.highest_acknowledged(), 1);
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 1);
        assert_eq!(writeback.occupied.load(), 0);
    }

    #[test]
    fn single_producer_capacity_one_and_three_wrap_exact_turns() {
        for capacity in [1, 3] {
            let backend = Arc::new(MemBlobs::new());
            let writeback = Writeback::new_single(backend, 0, config(capacity, 0)).unwrap();
            let producer = writeback.single_producer_state();
            let rounds = u64::try_from(writeback.publication_cells.len())
                .unwrap()
                .checked_mul(3)
                .unwrap();

            for raw_sequence in 1..=rounds {
                let expected = CommitSeq::new(raw_sequence).unwrap();
                let turn_before = writeback
                    .publication_cell(expected)
                    .turn
                    .load(Ordering::Relaxed);
                let key = raw_sequence.to_le_bytes();
                let sequence = publish_fast_arena_single(
                    &writeback,
                    &producer,
                    raw_sequence as u32,
                    vec![put(&key, b"value")],
                );
                assert_eq!(sequence.get(), raw_sequence);
                assert_eq!(writeback.highest_acknowledged(), raw_sequence);
                assert_eq!(
                    writeback
                        .publication_cell(sequence)
                        .turn
                        .load(Ordering::Relaxed),
                    turn_before,
                    "the direct SPSC path must not transfer the legacy turn line"
                );
                assert_eq!(
                    writeback.next_bound.load(Ordering::Relaxed),
                    0,
                    "the direct SPSC path owns its producer-local tail"
                );
                assert_eq!(writeback.occupied.load(), 0);
                assert!(matches!(
                    writeback.process_front(),
                    ProcessOutcome::Advanced
                ));
                assert_eq!(writeback.applied_sequence(), raw_sequence);
                assert_eq!(
                    writeback.applied_frontier.load(Ordering::Acquire),
                    raw_sequence
                );
            }
        }
    }

    #[test]
    fn single_producer_direct_batch_publishes_and_retires_dense_frontiers() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new_single(
            Arc::clone(&backend),
            0,
            batch_config(3, 3, DEFAULT_MAX_RECORD_BYTES),
        )
        .unwrap();
        let producer = writeback.single_producer_state();
        let initial_turns = writeback
            .publication_cells
            .iter()
            .map(|cell| cell.turn.load(Ordering::Relaxed))
            .collect::<Vec<_>>();

        for raw_sequence in 1_u64..=3 {
            let key = raw_sequence.to_le_bytes();
            assert_eq!(
                publish_fast_arena_single(
                    &writeback,
                    &producer,
                    raw_sequence as u32,
                    vec![put(&key, b"batch")],
                )
                .get(),
                raw_sequence
            );
        }
        assert_eq!(writeback.highest_acknowledged(), 3);
        assert_eq!(writeback.applied_frontier.load(Ordering::Acquire), 0);
        assert_eq!(
            writeback
                .publication_cells
                .iter()
                .map(|cell| cell.turn.load(Ordering::Relaxed))
                .collect::<Vec<_>>(),
            initial_turns,
            "direct publication must leave every legacy turn cache line cold"
        );

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 3);
        assert_eq!(writeback.applied_frontier.load(Ordering::Acquire), 3);
        assert_eq!(backend.batch_count(), 1);
        assert_eq!(backend.op_count(), 6, "each record contributes log + data");
    }

    #[test]
    fn single_producer_reacquisition_accounts_for_an_unapplied_tail() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new_single(backend, 0, config(2, 0)).unwrap();
        let first_owner = writeback.single_producer_state();
        assert_eq!(
            publish_fast_arena_single(&writeback, &first_owner, 1, vec![put(b"first", b"queued")],)
                .get(),
            1
        );
        assert_eq!(writeback.applied_sequence(), 0);
        drop(first_owner);

        // A newly acquired lease seeds its producer-local cursor from applied,
        // not tail. The shared tail therefore still charges the first queued
        // generation against logical capacity while admitting exactly one more.
        let second_owner = writeback.single_producer_state();
        assert_eq!(
            publish_fast_arena_single(
                &writeback,
                &second_owner,
                2,
                vec![put(b"second", b"queued")],
            )
            .get(),
            2
        );
        assert_eq!(writeback.highest_acknowledged(), 2);
        assert_eq!(writeback.applied_sequence(), 0);
        assert_eq!(writeback.occupied.load(), 0);

        for expected in 1..=2 {
            assert!(matches!(
                writeback.process_front(),
                ProcessOutcome::Advanced
            ));
            assert_eq!(writeback.applied_sequence(), expected);
        }
    }

    #[test]
    fn single_producer_all_record_paths_share_logical_capacity_accounting() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new_single(backend, 0, config(4, 0)).unwrap();
        let producer = writeback.single_producer_state();

        let mut legacy = writeback
            .reserve_single(&producer, vec![put(b"legacy", b"one")])
            .unwrap();
        let mut legacy = legacy.bind(mako_timestamp_of(1)).unwrap();
        assert_eq!(legacy.publish().unwrap().get(), 1);

        let small = encoded_native_record(2, 2, vec![put(b"generic-arena", b"two")]);
        assert!(small.len() <= NATIVE_RECORD_ARENA_BLOCK_BYTES);
        let mut permit = writeback
            .reserve_native_single(&producer, small.len())
            .unwrap();
        assert!(matches!(
            permit.native_buffer,
            Some(NativeRecordBuffer::UnboundArena { .. })
        ));
        let mut reservation = permit.bind_native(mako_timestamp_of(2)).unwrap();
        assert!(matches!(
            reservation.native_buffer,
            Some(NativeRecordBuffer::Arena { .. })
        ));
        fill_checked_out_native_buffer(&mut reservation, &small);
        assert_eq!(reservation.publish().unwrap().get(), 2);

        let large_value = vec![b'x'; NATIVE_RECORD_ARENA_BLOCK_BYTES + 64];
        let oversized_mutations = vec![put(b"oversized", &large_value)];
        let oversized = encoded_native_record(3, 3, oversized_mutations);
        assert!(oversized.len() > NATIVE_RECORD_ARENA_BLOCK_BYTES);
        let mut permit = writeback
            .reserve_native_single(&producer, oversized.len())
            .unwrap();
        let mut reservation = permit.bind_native(mako_timestamp_of(3)).unwrap();
        assert!(matches!(
            reservation.native_buffer,
            Some(NativeRecordBuffer::Owned(_))
        ));
        fill_checked_out_native_buffer(&mut reservation, &oversized);
        assert_eq!(reservation.publish().unwrap().get(), 3);

        let fourth =
            publish_fast_arena_single(&writeback, &producer, 4, vec![put(b"fast-arena", b"four")]);
        assert_eq!(fourth.get(), 4);
        assert_eq!(writeback.highest_acknowledged(), 4);
        assert_eq!(writeback.occupied.load(), 0);

        for expected in 1..=4 {
            assert!(matches!(
                writeback.process_front(),
                ProcessOutcome::Advanced
            ));
            assert_eq!(writeback.applied_sequence(), expected);
        }
        assert_eq!(writeback.queue_len(), 0);
        assert_eq!(writeback.occupied.load(), 0);
    }

    #[test]
    fn single_producer_direct_and_cold_records_reuse_one_ring_position() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new_single(Arc::clone(&backend), 0, config(1, 0)).unwrap();
        let producer = writeback.single_producer_state();

        let mut legacy = writeback
            .reserve_single(&producer, vec![put(b"legacy", b"one")])
            .unwrap();
        let mut legacy = legacy.bind(mako_timestamp_of(1)).unwrap();
        assert_eq!(legacy.publish().unwrap().get(), 1);
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));

        assert_eq!(
            publish_fast_arena_single(&writeback, &producer, 2, vec![put(b"direct", b"two")],)
                .get(),
            2
        );
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));

        let large_value = vec![b'x'; NATIVE_RECORD_ARENA_BLOCK_BYTES + 64];
        let oversized = encoded_native_record(3, 3, vec![put(b"oversized", &large_value)]);
        let mut permit = writeback
            .reserve_native_single(&producer, oversized.len())
            .unwrap();
        let mut reservation = permit.bind_native(mako_timestamp_of(3)).unwrap();
        fill_checked_out_native_buffer(&mut reservation, &oversized);
        assert_eq!(reservation.publish().unwrap().get(), 3);
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));

        assert_eq!(
            publish_fast_arena_single(
                &writeback,
                &producer,
                4,
                vec![put(b"direct-again", b"four")],
            )
            .get(),
            4
        );
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));

        assert_eq!(writeback.applied_sequence(), 4);
        assert_eq!(writeback.applied_frontier.load(Ordering::Acquire), 4);
        assert_eq!(backend.batch_count(), 4);
        assert_eq!(backend.op_count(), 8, "each record contributes log + data");
    }

    #[test]
    fn single_producer_preserves_unknown_fail_stop() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new_single(backend, 0, config(2, 0)).unwrap();
        let producer = writeback.single_producer_state();
        let mutations = vec![put(b"unknown", b"value")];
        let exact_record_bytes = record_encoded_len(mutations.clone());
        let mut permit = writeback
            .reserve_native_arena_fast_single(&producer, exact_record_bytes)
            .unwrap()
            .unwrap();
        // SAFETY: this test is the only foreground binder.
        let mut reservation = unsafe {
            permit
                .bind_externally_serialized(mako_timestamp_of(1))
                .unwrap()
        };
        let encoded = encoded_native_record(1, 1, mutations);
        fill_fast_arena_reservation(&mut reservation, &encoded);
        // SAFETY: the exact target is fully initialized but visibility is
        // deliberately classified as unknown.
        assert_eq!(
            unsafe { reservation.pin_completed_unknown().unwrap() }.get(),
            1
        );
        assert!(matches!(
            writeback.reserve_native_arena_fast_single(&producer, exact_record_bytes),
            Err(ReserveError::UnknownOutcome { sequence }) if sequence.get() == 1
        ));
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Pinned(sequence) if sequence.get() == 1
        ));
        assert_eq!(writeback.occupied.load(), 0);
    }

    #[test]
    fn single_producer_preserves_permanent_record_fail_stop() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new_single(backend, 0, config(2, 0)).unwrap();
        let producer = writeback.single_producer_state();
        let mutations = vec![put(b"malformed", b"checksum")];
        let mut encoded = encoded_native_record(1, 1, mutations);
        *encoded.last_mut().unwrap() ^= 1;
        let mut permit = writeback
            .reserve_native_arena_fast_single(&producer, encoded.len())
            .unwrap()
            .unwrap();
        // SAFETY: this test is the only foreground binder.
        let mut reservation = unsafe {
            permit
                .bind_externally_serialized(mako_timestamp_of(1))
                .unwrap()
        };
        fill_fast_arena_reservation(&mut reservation, &encoded);
        // SAFETY: the exact extent is initialized; corruption is deliberate
        // and must be detected only by background materialization.
        unsafe { reservation.publish_completed().unwrap() };

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::RecordFailed {
                sequence,
                error: RecordError::BadChecksum { .. },
            } if sequence.get() == 1
        ));
        assert!(matches!(
            writeback.reserve_native_arena_fast_single(&producer, encoded.len()),
            Err(ReserveError::PermanentRecordFailure {
                sequence,
                source: RecordError::BadChecksum { .. },
            }) if sequence.get() == 1
        ));
        assert_eq!(writeback.occupied.load(), 0);
    }

    #[test]
    fn single_producer_preserves_sequence_exhaustion() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new_single(backend, u64::MAX - 2, config(1, 0)).unwrap();
        let producer = writeback.single_producer_state();
        let mutations = vec![put(b"tail", b"value")];

        for (sequence, timestamp) in [(u64::MAX - 1, 1), (u64::MAX, 2)] {
            assert_eq!(
                publish_fast_arena_single(&writeback, &producer, timestamp, mutations.clone(),)
                    .get(),
                sequence
            );
            assert!(matches!(
                writeback.process_front(),
                ProcessOutcome::Advanced
            ));
        }
        let exact_record_bytes = record_encoded_len(mutations);
        assert!(matches!(
            writeback.reserve_native_arena_fast_single(&producer, exact_record_bytes),
            Err(ReserveError::SequenceExhausted)
        ));
        assert_eq!(writeback.occupied.load(), 0);
    }

    #[test]
    fn externally_serialized_arena_bind_wraps_and_reuses_exact_turns() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(3, 0)).unwrap();

        // Capacity three rounds up to a four-cell turn ring. Twelve records
        // exercise three complete generations of every cell.
        for raw_sequence in 1_u64..=12 {
            let key = raw_sequence.to_le_bytes();
            let sequence =
                publish_fast_arena(&writeback, raw_sequence as u32, vec![put(&key, b"value")]);
            assert_eq!(sequence.get(), raw_sequence);
            assert!(matches!(
                writeback.process_front(),
                ProcessOutcome::Advanced
            ));
        }
        assert_eq!(writeback.applied_sequence(), 12);
    }

    #[test]
    fn externally_serialized_arena_binders_assign_unique_dense_sequences() {
        const PRODUCERS: usize = 16;
        let backend = Arc::new(MemBlobs::new());
        let writeback =
            Arc::new(Writeback::new(Arc::clone(&backend), 0, config(PRODUCERS, 0)).unwrap());
        let external_gate = Arc::new(Mutex::new(0_u32));
        let exact_record_bytes = record_encoded_len(vec![put(b"key", b"value")]);
        let (sequence_tx, sequence_rx) = mpsc::channel();

        std::thread::scope(|scope| {
            for _ in 0..PRODUCERS {
                let writeback = Arc::clone(&writeback);
                let external_gate = Arc::clone(&external_gate);
                let sequence_tx = sequence_tx.clone();
                scope.spawn(move || {
                    let mut permit = writeback
                        .reserve_native_arena_fast(exact_record_bytes)
                        .unwrap()
                        .unwrap();
                    let (mut reservation, raw_mako_timestamp) = {
                        let mut next_timestamp = external_gate.lock().unwrap();
                        *next_timestamp += 1;
                        let raw_mako_timestamp = *next_timestamp;
                        // SAFETY: `external_gate` excludes every test binder
                        // through the complete load/store sequence allocation.
                        let reservation = unsafe {
                            permit
                                .bind_externally_serialized(mako_timestamp_of(raw_mako_timestamp))
                                .unwrap()
                        };
                        (reservation, raw_mako_timestamp)
                    };
                    let sequence = reservation.sequence();
                    let encoded = encoded_native_record(
                        sequence.get(),
                        raw_mako_timestamp,
                        vec![put(b"key", b"value")],
                    );
                    fill_fast_arena_reservation(&mut reservation, &encoded);
                    // SAFETY: native completion is simulated by the exact copy.
                    unsafe { reservation.publish_completed().unwrap() };
                    sequence_tx.send(sequence.get()).unwrap();
                });
            }
        });
        drop(sequence_tx);

        let mut sequences = sequence_rx.into_iter().collect::<Vec<_>>();
        sequences.sort_unstable();
        assert_eq!(sequences, (1_u64..=PRODUCERS as u64).collect::<Vec<_>>());
        for _ in 0..PRODUCERS {
            assert!(matches!(
                writeback.process_front(),
                ProcessOutcome::Advanced
            ));
        }
    }

    #[test]
    fn direct_native_bound_adoption_transfers_the_detached_claim_once() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(2, 0)).unwrap();
        let mutations = vec![put(b"direct-bound", b"value")];
        let encoded = encoded_native_record(1, 1, mutations);
        let mut permit = writeback
            .reserve_native_arena_fast(encoded.len())
            .unwrap()
            .unwrap();
        let sequence = CommitSeq::new(1).unwrap();

        // Model exactly the same-build terminal: packed assignment selects the
        // order, then its direct binder owns FREE and Release-publishes BOUND
        // before returning that order. The legacy tail remains stale.
        unsafe {
            writeback.publication_cell(sequence).publish_bound_preassigned(
                writeback.cold_publication_cell(sequence),
                sequence,
                writeback.publication_shift,
            )
        };
        let mut reservation = unsafe {
            permit.adopt_externally_bound(mako_timestamp_of(1), NonZeroU64::new(1).unwrap())
        };
        assert_eq!(reservation.sequence(), sequence);
        assert_eq!(writeback.next_bound.load(Ordering::Acquire), 0);
        assert_eq!(writeback.detached_len(), 0);
        assert_eq!(writeback.queue_len(), 1);

        fill_fast_arena_reservation(&mut reservation, &encoded);
        // SAFETY: the exact simulated native target is completely initialized.
        assert_eq!(
            unsafe { reservation.publish_completed_concurrent_nonblocking(0).unwrap() }.get(),
            1
        );
        assert_eq!(writeback.wait_applied().unwrap(), 1);
        assert_eq!(backend.batch_count(), 1);
    }

    #[test]
    fn packed_bound_suffix_imports_without_legacy_tail_updates() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(2, 0)).unwrap();
        let first_mutations = vec![put(b"packed-first", b"one")];
        let second_mutations = vec![put(b"packed-second", b"two")];
        let mut first_permit = writeback
            .reserve_native_arena_fast(record_encoded_len(first_mutations.clone()))
            .unwrap()
            .unwrap();
        let mut second_permit = writeback
            .reserve_native_arena_fast(record_encoded_len(second_mutations.clone()))
            .unwrap()
            .unwrap();

        // Model two packed assignments whose post-gate callbacks run in the
        // opposite order. Sequence two may become BOUND first, but exact-turn
        // import must stop at the sequence-one hole without consulting the
        // deliberately stale legacy tail.
        let mut second = unsafe {
            second_permit.bind_externally_ordered(
                mako_timestamp_of(2),
                NonZeroU64::new(2).unwrap(),
            )
        };
        assert_eq!(writeback.next_bound.load(Ordering::Acquire), 0);
        assert_eq!(writeback.detached_len(), 1);
        assert_eq!(writeback.queue_len(), 0);

        let mut first = unsafe {
            first_permit.bind_externally_ordered(
                mako_timestamp_of(1),
                NonZeroU64::new(1).unwrap(),
            )
        };
        assert_eq!(writeback.next_bound.load(Ordering::Acquire), 0);
        assert_eq!(writeback.detached_len(), 0);
        assert_eq!(writeback.queue_len(), 2);

        fill_fast_arena_reservation(
            &mut second,
            &encoded_native_record(2, 2, second_mutations),
        );
        fill_fast_arena_reservation(
            &mut first,
            &encoded_native_record(1, 1, first_mutations),
        );
        // SAFETY: both exact simulated native targets are fully initialized.
        assert_eq!(
            unsafe { second.publish_completed_concurrent_nonblocking(1).unwrap() }.get(),
            2
        );
        assert_eq!(
            unsafe { first.publish_completed_concurrent_nonblocking(0).unwrap() }.get(),
            1
        );
        assert_eq!(writeback.wait_applied().unwrap(), 2);
        assert_eq!(backend.batch_count(), 2);
    }

    #[test]
    fn packed_oversized_adoption_keeps_legacy_tail_stale() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(1, 0)).unwrap();
        let mutations = vec![put(b"packed-oversized", &vec![0x5a; 512])];
        let encoded = encoded_native_record(1, 1, mutations);
        assert!(encoded.len() > writeback.native_arena.block_bytes());
        let mut permit = writeback.reserve_native(encoded.len()).unwrap();

        let mut reservation = unsafe {
            permit.bind_native_externally_ordered(
                mako_timestamp_of(1),
                NonZeroU64::new(1).unwrap(),
            )
        };
        assert_eq!(writeback.next_bound.load(Ordering::Acquire), 0);
        assert_eq!(writeback.detached_len(), 0);
        assert_eq!(writeback.queue_len(), 1);

        fill_checked_out_native_buffer(&mut reservation, &encoded);
        assert_eq!(reservation.publish().unwrap().get(), 1);
        assert_eq!(writeback.wait_applied().unwrap(), 1);
        assert_eq!(backend.batch_count(), 1);
    }

    #[test]
    fn trusted_ready_suffix_wakes_parked_dense_apply_barrier() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Arc::new(Writeback::new(Arc::clone(&backend), 0, config(2, 0)).unwrap());
        let first_mutations = vec![put(b"first", b"one")];
        let second_mutations = vec![put(b"second", b"two")];
        let mut first_permit = writeback
            .reserve_native_arena_fast(record_encoded_len(first_mutations.clone()))
            .unwrap()
            .unwrap();
        let mut second_permit = writeback
            .reserve_native_arena_fast(record_encoded_len(second_mutations.clone()))
            .unwrap()
            .unwrap();

        // SAFETY: this test serializes both dense assignments exactly as the
        // legacy native ordering exclusion does.
        let mut first = unsafe {
            first_permit
                .bind_externally_serialized(mako_timestamp_of(1))
                .unwrap()
        };
        let mut second = unsafe {
            second_permit
                .bind_externally_serialized(mako_timestamp_of(2))
                .unwrap()
        };
        fill_fast_arena_reservation(&mut first, &encoded_native_record(1, 1, first_mutations));
        fill_fast_arena_reservation(&mut second, &encoded_native_record(2, 2, second_mutations));

        // SAFETY: both exact native targets are fully initialized and this
        // test models definitely committed, successfully cleaned terminals.
        assert_eq!(
            unsafe { second.publish_completed_concurrent_nonblocking(1).unwrap() }.get(),
            2
        );
        assert_eq!(writeback.highest_acknowledged(), 0);
        assert_eq!(writeback.highest_caller_acknowledged(), 2);
        assert!(matches!(writeback.process_front(), ProcessOutcome::Blocked));

        let waiter_writeback = Arc::clone(&writeback);
        let (returned_tx, returned_rx) = mpsc::channel();
        let waiter = std::thread::spawn(move || {
            returned_tx.send(waiter_writeback.wait_applied()).unwrap();
        });
        let deadline = Instant::now() + Duration::from_secs(1);
        while writeback.activity_waiter_count() == 0 {
            assert!(
                Instant::now() < deadline,
                "dense apply barrier did not park behind sequence one"
            );
            std::thread::yield_now();
        }
        assert!(matches!(
            returned_rx.try_recv(),
            Err(mpsc::TryRecvError::Empty)
        ));

        assert_eq!(
            // SAFETY: the first target is complete. This trusted publisher
            // emits no condition-variable notification and does not sweep;
            // the barrier's bounded poll must wake and consume both records.
            unsafe { first.publish_completed_concurrent_nonblocking(0).unwrap() }.get(),
            1
        );
        assert_eq!(writeback.highest_acknowledged(), 0);
        assert_eq!(
            returned_rx
                .recv_timeout(Duration::from_secs(1))
                .expect("sequence one did not wake the dense apply barrier")
                .unwrap(),
            2
        );
        waiter.join().unwrap();
        assert_eq!(writeback.highest_acknowledged(), 2);
        assert_eq!(writeback.activity_waiter_count(), 0);
        assert_eq!(backend.batch_count(), 2);
    }

    #[test]
    fn consumer_sweeps_two_trusted_nonblocking_publications() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(2, 0)).unwrap();
        let first_mutations = vec![put(b"first", b"one")];
        let second_mutations = vec![put(b"second", b"two")];
        let mut first_permit = writeback
            .reserve_native_arena_fast(record_encoded_len(first_mutations.clone()))
            .unwrap()
            .unwrap();
        let mut second_permit = writeback
            .reserve_native_arena_fast(record_encoded_len(second_mutations.clone()))
            .unwrap()
            .unwrap();

        // SAFETY: this test serializes dense assignments in native-ticket order.
        let mut first = unsafe {
            first_permit
                .bind_externally_serialized(mako_timestamp_of(1))
                .unwrap()
        };
        let mut second = unsafe {
            second_permit
                .bind_externally_serialized(mako_timestamp_of(2))
                .unwrap()
        };
        fill_fast_arena_reservation(&mut first, &encoded_native_record(1, 1, first_mutations));
        fill_fast_arena_reservation(&mut second, &encoded_native_record(2, 2, second_mutations));

        // SAFETY: both exact native targets model definitely committed,
        // successfully cleaned trusted terminals.
        assert_eq!(
            unsafe { second.publish_completed_concurrent_nonblocking(1).unwrap() }.get(),
            2
        );
        assert_eq!(writeback.highest_acknowledged(), 0);
        assert_eq!(
            unsafe { first.publish_completed_concurrent_nonblocking(0).unwrap() }.get(),
            1
        );
        // Neither trusted writer scans or contends on the dense frontier.
        assert_eq!(writeback.highest_acknowledged(), 0);
        assert_eq!(writeback.highest_caller_acknowledged(), 2);
        assert_eq!(writeback.wait_applied().unwrap(), 2);
        assert_eq!(writeback.highest_acknowledged(), 2);
        assert_eq!(backend.batch_count(), 2);
    }

    #[test]
    fn one_worker_marker_tracks_repeated_trusted_commits() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(2, 0)).unwrap();
        let first_mutations = vec![put(b"same-worker-first", b"one")];
        let second_mutations = vec![put(b"same-worker-second", b"two")];
        let mut first_permit = writeback
            .reserve_native_arena_fast(record_encoded_len(first_mutations.clone()))
            .unwrap()
            .unwrap();
        let mut second_permit = writeback
            .reserve_native_arena_fast(record_encoded_len(second_mutations.clone()))
            .unwrap()
            .unwrap();

        // SAFETY: this test serializes dense assignments in native-ticket
        // order and models two consecutive commits by worker zero.
        let mut first = unsafe {
            first_permit
                .bind_externally_serialized(mako_timestamp_of(1))
                .unwrap()
        };
        let mut second = unsafe {
            second_permit
                .bind_externally_serialized(mako_timestamp_of(2))
                .unwrap()
        };
        fill_fast_arena_reservation(&mut first, &encoded_native_record(1, 1, first_mutations));
        fill_fast_arena_reservation(&mut second, &encoded_native_record(2, 2, second_mutations));

        // SAFETY: both exact targets model definitely committed, successfully
        // cleaned terminals from the same process-lifetime worker.
        assert_eq!(
            unsafe { first.publish_completed_concurrent_nonblocking(0).unwrap() }.get(),
            1
        );
        assert_eq!(writeback.highest_caller_acknowledged(), 1);
        assert_eq!(
            unsafe { second.publish_completed_concurrent_nonblocking(0).unwrap() }.get(),
            2
        );
        assert_eq!(writeback.highest_acknowledged(), 0);
        assert_eq!(writeback.highest_caller_acknowledged(), 2);
        assert_eq!(writeback.wait_applied().unwrap(), 2);
        assert_eq!(backend.batch_count(), 2);
    }

    #[test]
    fn structural_failure_caps_consumer_before_trusted_ready_suffix() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(3, 0)).unwrap();
        let mut first = bind(
            writeback
                .reserve(vec![put(b"general-first", b"one")])
                .unwrap(),
            1,
        );
        let mut second = bind(
            writeback
                .reserve(vec![put(b"general-failed", b"two")])
                .unwrap(),
            2,
        );
        let third_mutations = vec![put(b"trusted-suffix", b"three")];
        let mut third_permit = writeback
            .reserve_native_arena_fast(record_encoded_len(third_mutations.clone()))
            .unwrap()
            .unwrap();
        // SAFETY: both general binders above and this external binder are
        // serialized exactly as native validation-ticket assignment is.
        let mut third = unsafe {
            third_permit
                .bind_externally_serialized(mako_timestamp_of(3))
                .unwrap()
        };
        fill_fast_arena_reservation(&mut third, &encoded_native_record(3, 3, third_mutations));

        assert_eq!(first.publish().unwrap().get(), 1);
        assert_eq!(second.publish().unwrap().get(), 2);
        // SAFETY: the exact third target models a definitely committed,
        // successfully cleaned trusted terminal racing with failure discovery.
        assert_eq!(
            unsafe { third.publish_completed_concurrent_nonblocking(0).unwrap() }.get(),
            3
        );
        assert_eq!(writeback.highest_acknowledged(), 2);
        assert_eq!(writeback.highest_caller_acknowledged(), 3);

        // Model a consumer which has just restored sequence two after finding
        // structural corruption while sequence three's trusted terminal raced
        // immediately before the health latch.
        assert!(matches!(
            writeback.record_failure_outcome(
                CommitSeq::new(2).unwrap(),
                RecordError::BadMagic,
            ),
            ProcessOutcome::RecordFailed {
                sequence,
                error: RecordError::BadMagic,
            } if sequence.get() == 2
        ));
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 1);
        assert_eq!(
            writeback.highest_acknowledged(),
            2,
            "the consumer must not sweep READY sequence three past failure two"
        );
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::RecordFailed {
                sequence,
                error: RecordError::BadMagic,
            } if sequence.get() == 2
        ));
        assert!(matches!(
            writeback.wait_applied(),
            Err(ApplyError::Record {
                sequence,
                source: RecordError::BadMagic,
            }) if sequence.get() == 2
        ));
        assert_eq!(backend.batch_count(), 1);
    }

    #[test]
    fn trusted_ready_suffix_stays_blocked_behind_unknown_predecessor() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Arc::new(Writeback::new(Arc::clone(&backend), 0, config(2, 0)).unwrap());
        let first_mutations = vec![put(b"first-unknown", b"uncertain")];
        let second_mutations = vec![put(b"second-ready", b"committed")];
        let mut first_permit = writeback
            .reserve_native_arena_fast(record_encoded_len(first_mutations))
            .unwrap()
            .unwrap();
        let mut second_permit = writeback
            .reserve_native_arena_fast(record_encoded_len(second_mutations.clone()))
            .unwrap()
            .unwrap();

        // SAFETY: this test serializes dense assignments in native-ticket order.
        let mut first = unsafe {
            first_permit
                .bind_externally_serialized(mako_timestamp_of(1))
                .unwrap()
        };
        let mut second = unsafe {
            second_permit
                .bind_externally_serialized(mako_timestamp_of(2))
                .unwrap()
        };
        fill_fast_arena_reservation(&mut second, &encoded_native_record(2, 2, second_mutations));

        // SAFETY: the second target models a definitely committed,
        // successfully cleaned trusted terminal.
        assert_eq!(
            unsafe { second.publish_completed_concurrent_nonblocking(1).unwrap() }.get(),
            2
        );
        assert_eq!(writeback.highest_acknowledged(), 0);
        assert_eq!(writeback.highest_caller_acknowledged(), 2);

        assert_eq!(first.pin_unwritten_unknown().unwrap().get(), 1);
        assert!(matches!(
            writeback.wait_applied(),
            Err(ApplyError::UnknownOutcome { sequence }) if sequence.get() == 1
        ));
        assert_eq!(writeback.applied_sequence(), 0);
        assert_eq!(backend.batch_count(), 0);

        let mut runtime = crate::runtime::Runtime::start(Arc::clone(&writeback)).unwrap();
        assert!(matches!(
            runtime.shutdown(),
            Err(crate::runtime::RuntimeError::Apply(
                ApplyError::UnknownOutcome { sequence }
            )) if sequence.get() == 1
        ));
        assert_eq!(writeback.applied_sequence(), 0);
        assert_eq!(backend.batch_count(), 0);
    }

    #[test]
    fn external_arena_and_concurrent_general_binders_interoperate() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(backend, 0, config(2, 0)).unwrap();
        let fast_mutations = vec![put(b"fast", b"one")];
        let general_mutations = vec![put(b"general", b"two")];
        let mut fast = writeback
            .reserve_native_arena_fast(record_encoded_len(fast_mutations.clone()))
            .unwrap()
            .unwrap();
        let mut general = writeback
            .reserve_native(record_encoded_len(general_mutations.clone()))
            .unwrap();

        // SAFETY: no other binder executes during this exact external bind.
        let mut first = unsafe {
            fast.bind_externally_serialized(mako_timestamp_of(1))
                .unwrap()
        };
        let mut second = general.bind_native(mako_timestamp_of(2)).unwrap();
        assert_eq!(first.sequence().get(), 1);
        assert_eq!(second.sequence().get(), 2);

        let first_encoded = encoded_native_record(1, 1, fast_mutations);
        fill_fast_arena_reservation(&mut first, &first_encoded);
        let second_encoded = encoded_native_record(2, 2, general_mutations);
        fill_checked_out_native_buffer(&mut second, &second_encoded);
        // SAFETY: the exact first target was completely initialized above.
        unsafe { first.publish_completed().unwrap() };
        second.publish().unwrap();
        assert_eq!(writeback.highest_acknowledged(), 2);
    }

    #[test]
    fn externally_serialized_arena_bind_preserves_sequence_exhaustion() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(backend, u64::MAX - 2, config(2, 0)).unwrap();
        let mutations = vec![put(b"tail", b"value")];
        let exact_record_bytes = record_encoded_len(mutations.clone());
        let mut first_permit = writeback
            .reserve_native_arena_fast(exact_record_bytes)
            .unwrap()
            .unwrap();
        // SAFETY: this test calls no other binder concurrently.
        let mut first = unsafe {
            first_permit
                .bind_externally_serialized(mako_timestamp_of(1))
                .unwrap()
        };
        let mut second_permit = writeback
            .reserve_native_arena_fast(exact_record_bytes)
            .unwrap()
            .unwrap();
        // SAFETY: this test calls no other binder concurrently. This second
        // bind intentionally exercises the checked near-exhaustion fallback.
        let mut second = unsafe {
            second_permit
                .bind_externally_serialized(mako_timestamp_of(2))
                .unwrap()
        };
        assert_eq!(first.sequence().get(), u64::MAX - 1);
        assert_eq!(second.sequence().get(), u64::MAX);

        let first_encoded = encoded_native_record(u64::MAX - 1, 1, mutations.clone());
        let second_encoded = encoded_native_record(u64::MAX, 2, mutations);
        fill_fast_arena_reservation(&mut first, &first_encoded);
        fill_fast_arena_reservation(&mut second, &second_encoded);
        // SAFETY: both exact targets were initialized completely above.
        unsafe { first.publish_completed().unwrap() };
        unsafe { second.publish_completed().unwrap() };
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        let mut exhausted = writeback
            .reserve_native_arena_fast(exact_record_bytes)
            .expect("capacity remains independent of the legacy tail")
            .unwrap();
        assert!(matches!(
            unsafe { exhausted.bind_externally_serialized(mako_timestamp_of(3)) },
            Err(ReserveError::SequenceExhausted)
        ));
    }

    fn fill_checked_out_native_buffer<B: Blobs>(
        reservation: &mut BoundReservation<'_, B>,
        encoded: &[u8],
    ) {
        let buffer = reservation
            .native_buffer
            .as_mut()
            .expect("test native reservation owns a checked-out buffer");
        assert_eq!(buffer.exact_record_bytes(), encoded.len());
        let destination = buffer.target(&reservation.owner.native_arena);
        // SAFETY: the reservation uniquely owns an exact writable extent, and
        // `encoded` has that same checked length. This simulates native's write
        // immediately before its exact completion witness.
        unsafe {
            std::ptr::copy_nonoverlapping(encoded.as_ptr(), destination.as_ptr(), encoded.len());
            reservation.attach_written_native_record();
        }
    }

    struct MaterializationFailureGuard;

    impl Drop for MaterializationFailureGuard {
        fn drop(&mut self) {
            TEST_MATERIALIZATION_FAILURE.with(|failure| {
                *failure.borrow_mut() = None;
            });
        }
    }

    fn force_materialization_failure(
        sequence: CommitSeq,
        error: RecordError,
    ) -> MaterializationFailureGuard {
        TEST_MATERIALIZATION_FAILURE.with(|failure| {
            let previous = failure.borrow_mut().replace((sequence, error));
            assert!(previous.is_none(), "test failure injection already armed");
        });
        MaterializationFailureGuard
    }

    fn bind<'a, B: Blobs>(
        mut permit: DetachedPermit<'a, B>,
        mako_timestamp: u32,
    ) -> BoundReservation<'a, B> {
        permit.bind(mako_timestamp_of(mako_timestamp)).unwrap()
    }

    fn publish<B: Blobs>(
        writeback: &Writeback<B>,
        mutations: Vec<Mutation>,
        mako_timestamp: u32,
    ) -> CommitSeq {
        writeback
            .reserve(mutations)
            .unwrap()
            .bind(mako_timestamp_of(mako_timestamp))
            .unwrap()
            .publish()
            .unwrap()
    }

    fn mako_timestamp_of(raw: u32) -> MakoTimestamp {
        MakoTimestamp::new(raw).expect("test Mako timestamps are nonzero")
    }

    fn wait_until_ready<B: Blobs>(writeback: &Writeback<B>, raw_sequence: u64) {
        let sequence = CommitSeq::new(raw_sequence).expect("test sequence is nonzero");
        let token = QueueToken::new(sequence);
        let deadline = Instant::now() + Duration::from_secs(1);
        let mut state = lock_recover(&writeback.state);
        loop {
            writeback.import_bound_locked(&mut state);
            let offset = state
                .queue_offset(token)
                .expect("publication slot remains queued while acknowledgement waits");
            if state.queue[offset].state == SlotState::Ready
                || writeback
                    .publication_cell(sequence)
                    .is_published(sequence, writeback.publication_shift)
            {
                return;
            }

            let remaining = deadline
                .checked_duration_since(Instant::now())
                .expect("publication did not make its slot Ready within one second");
            let (next_state, timeout) = writeback
                .acknowledgement_changed
                .wait_timeout(state, remaining)
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            state = next_state;
            if timeout.timed_out() {
                let offset = state
                    .queue_offset(token)
                    .expect("timed-out publication slot remains queued");
                assert!(
                    state.queue[offset].state == SlotState::Ready
                        || writeback
                            .publication_cell(sequence)
                            .is_published(sequence, writeback.publication_shift),
                    "publication did not make its slot Ready within one second"
                );
                return;
            }
        }
    }

    fn wait_for_acknowledgement_waiters<B: Blobs>(writeback: &Writeback<B>, expected: usize) {
        let deadline = Instant::now() + Duration::from_secs(1);
        while writeback.acknowledgement_waiters.load(Ordering::SeqCst) < expected {
            assert!(
                Instant::now() < deadline,
                "{expected} acknowledgement publishers did not enroll to sleep"
            );
            std::thread::yield_now();
        }
    }

    #[derive(Clone, Debug, Eq, PartialEq)]
    enum OwnedBlobOp {
        Put { key: Vec<u8>, value: Vec<u8> },
        Delete { key: Vec<u8> },
    }

    impl OwnedBlobOp {
        fn capture(operation: &BlobOp<'_>) -> Self {
            match operation {
                BlobOp::Put { key, val } => Self::Put {
                    key: key.to_vec(),
                    value: val.to_vec(),
                },
                BlobOp::Delete { key } => Self::Delete { key: key.to_vec() },
            }
        }
    }

    #[derive(Debug, Default)]
    struct RecordingBlobs {
        inner: MemBlobs,
        attempts: Mutex<Vec<Vec<OwnedBlobOp>>>,
    }

    impl RecordingBlobs {
        fn fail_next_writes(&self, count: usize) {
            self.inner.fail_next_writes(count);
        }

        fn attempts(&self) -> Vec<Vec<OwnedBlobOp>> {
            lock_recover(&self.attempts).clone()
        }
    }

    impl Blobs for RecordingBlobs {
        fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
            self.inner.get(key)
        }

        fn write_batch(&self, operations: &[BlobOp<'_>]) -> Result<(), BlobError> {
            lock_recover(&self.attempts)
                .push(operations.iter().map(OwnedBlobOp::capture).collect());
            self.inner.write_batch(operations)
        }

        fn for_each_key(&self, f: &mut dyn FnMut(&[u8])) -> Result<(), BlobError> {
            self.inner.for_each_key(f)
        }
    }

    #[test]
    fn native_arena_exposes_disjoint_concurrent_block_targets() {
        let arena = NativeRecordArena::new(2, NATIVE_RECORD_ARENA_BLOCK_BYTES).unwrap();
        // SAFETY: this test exclusively owns both block tokens, the targets
        // have the exact configured extent, and block zero and one are
        // disjoint. Keeping both pointers live exercises the aliasing contract
        // under Miri as well as ordinary test execution.
        let (first, second) = unsafe {
            (
                arena.target(0, NATIVE_RECORD_ARENA_BLOCK_BYTES),
                arena.target(1, NATIVE_RECORD_ARENA_BLOCK_BYTES),
            )
        };
        unsafe {
            std::ptr::write_bytes(first.as_ptr(), 0x11, NATIVE_RECORD_ARENA_BLOCK_BYTES);
            std::ptr::write_bytes(second.as_ptr(), 0x22, NATIVE_RECORD_ARENA_BLOCK_BYTES);
            assert_eq!(first.as_ptr().read(), 0x11);
            assert_eq!(second.as_ptr().read(), 0x22);
        }
    }

    #[test]
    fn small_native_arena_survives_backend_retry_and_reuses_the_same_block() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(4, 0)).unwrap();
        let encoded = encoded_native_record(1, 201, vec![put(b"small", b"value")]);
        assert!(encoded.len() <= NATIVE_RECORD_ARENA_BLOCK_BYTES);

        let mut permit = writeback.reserve_native(encoded.len()).unwrap();
        assert!(matches!(
            permit.native_buffer.as_ref(),
            Some(&NativeRecordBuffer::UnboundArena { .. })
        ));
        assert_eq!(writeback.free_len(), 3);
        let mut reservation = permit.bind_native(mako_timestamp_of(201)).unwrap();
        let first_block = match reservation.native_buffer.as_ref().unwrap() {
            NativeRecordBuffer::Arena { block, .. } => *block,
            NativeRecordBuffer::UnboundArena { .. } | NativeRecordBuffer::Owned(_) => {
                panic!("small record missed its sequence-indexed arena block")
            }
        };
        assert_eq!(
            first_block,
            writeback.publication_index(reservation.sequence())
        );
        fill_checked_out_native_buffer(&mut reservation, &encoded);
        reservation.publish().unwrap();

        backend.fail_next_writes(1);
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::BackendFailed { sequence, .. } if sequence.get() == 1
        ));
        assert_eq!(
            writeback.free_len(),
            3,
            "a retryable backend failure must retain the initialized block"
        );
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.free_len(), 4);
    }

    #[test]
    fn unbound_and_bound_unwritten_native_buffers_are_recycled() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(backend, 0, config(2, 0)).unwrap();
        let encoded = encoded_native_record(1, 202, vec![put(b"key", b"value")]);

        let permit = writeback.reserve_native(encoded.len()).unwrap();
        assert_eq!(writeback.free_len(), 1);
        drop(permit);
        assert_eq!(writeback.free_len(), 2);

        let mut permit = writeback.reserve_native(encoded.len()).unwrap();
        let mut reservation = permit.bind_native(mako_timestamp_of(202)).unwrap();
        reservation.pin_unknown().unwrap();
        let state = lock_recover(&writeback.state);
        assert_eq!(
            writeback.free_len(),
            1,
            "the unused block is recycled but its pinned capacity stays bound"
        );
        assert_eq!(state.first_unknown.map(CommitSeq::get), Some(1));
        drop(state);
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Pinned(sequence) if sequence.get() == 1
        ));
    }

    #[test]
    fn oversized_native_vector_is_recycled_without_moving_its_allocation() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(backend, 0, config(2, 0)).unwrap();
        let large_value = vec![b'x'; NATIVE_RECORD_ARENA_BLOCK_BYTES + 64];
        let encoded = encoded_native_record(1, 203, vec![put(b"large", &large_value)]);
        assert!(encoded.len() > NATIVE_RECORD_ARENA_BLOCK_BYTES);

        let mut permit = writeback.reserve_native(encoded.len()).unwrap();
        let first_pointer = match permit.native_buffer.as_ref().unwrap() {
            NativeRecordBuffer::Owned(bytes) => bytes.as_ptr(),
            NativeRecordBuffer::UnboundArena { .. } | NativeRecordBuffer::Arena { .. } => {
                panic!("oversized record entered fixed arena")
            }
        };
        let mut reservation = permit.bind_native(mako_timestamp_of(203)).unwrap();
        fill_checked_out_native_buffer(&mut reservation, &encoded);
        reservation.publish().unwrap();
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.free_len(), 2);

        let mut saw_reused_pointer = false;
        for _ in 0..writeback.config.capacity {
            let permit = writeback.reserve_native(encoded.len()).unwrap();
            let reused_pointer = match permit.native_buffer.as_ref().unwrap() {
                NativeRecordBuffer::Owned(bytes) => bytes.as_ptr(),
                NativeRecordBuffer::UnboundArena { .. } | NativeRecordBuffer::Arena { .. } => {
                    panic!("oversized record entered fixed arena")
                }
            };
            saw_reused_pointer |= reused_pointer == first_pointer;
            drop(permit);
        }
        assert!(
            saw_reused_pointer,
            "producer hint must revisit every recycled oversized allocation"
        );
        assert_eq!(writeback.free_len(), 2);
    }

    #[test]
    fn one_transaction_is_exactly_one_backend_batch() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(4, 0)).unwrap();

        writeback
            .reserve(vec![put(b"a", b"one"), delete(b"b")])
            .unwrap()
            .bind(mako_timestamp_of(11))
            .unwrap()
            .publish()
            .unwrap();

        assert_eq!(writeback.wait_applied().unwrap(), 1);
        assert_eq!(backend.batch_count(), 1);
        // One backend commit-record entry plus the transaction's two data ops.
        assert_eq!(backend.op_count(), 3);
        assert_eq!(
            writeback.applied_watermark(),
            AppliedWatermark::recovered(1, Some(mako_timestamp_of(11)))
        );
        assert_eq!(writeback.queue_len(), 0);
    }

    #[test]
    fn contiguous_ready_prefix_uses_one_backend_call() {
        let backend = Arc::new(MemBlobs::new());
        let writeback =
            Writeback::new(Arc::clone(&backend), 0, batch_config(4, 4, usize::MAX)).unwrap();
        publish(&writeback, vec![put(b"a", b"one")], 111);
        publish(&writeback, vec![put(b"b", b"two")], 112);
        publish(&writeback, vec![put(b"c", b"three")], 113);

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(backend.batch_count(), 1);
        assert_eq!(backend.op_count(), 6, "each record contributes log + data");
        assert_eq!(writeback.applied_sequence(), 3);
        assert_eq!(writeback.queue_len(), 0);
    }

    #[test]
    fn ready_prefix_respects_record_count_cap() {
        let backend = Arc::new(MemBlobs::new());
        let writeback =
            Writeback::new(Arc::clone(&backend), 0, batch_config(5, 2, usize::MAX)).unwrap();
        publish(&writeback, vec![put(b"a", b"one")], 121);
        publish(&writeback, vec![put(b"b", b"two")], 122);
        publish(&writeback, vec![put(b"c", b"three")], 123);
        publish(&writeback, vec![put(b"d", b"four")], 124);
        publish(&writeback, vec![put(b"e", b"five")], 125);

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 2);
        assert_eq!(writeback.queue_len(), 3);
        assert_eq!(backend.batch_count(), 1);

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 4);
        assert_eq!(writeback.queue_len(), 1);
        assert_eq!(backend.batch_count(), 2);

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 5);
        assert_eq!(writeback.queue_len(), 0);
        assert_eq!(backend.batch_count(), 3);
    }

    #[test]
    fn ready_prefix_respects_encoded_byte_cap() {
        let record_bytes = record_encoded_len(vec![put(b"a", b"one")]);
        let two_record_cap = record_bytes.checked_mul(2).unwrap();
        let backend = Arc::new(MemBlobs::new());
        let writeback =
            Writeback::new(Arc::clone(&backend), 0, batch_config(3, 3, two_record_cap)).unwrap();
        publish(&writeback, vec![put(b"a", b"one")], 131);
        publish(&writeback, vec![put(b"b", b"two")], 132);
        publish(&writeback, vec![put(b"c", b"six")], 133);

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 2);
        assert_eq!(writeback.queue_len(), 1);
        assert_eq!(backend.batch_count(), 1);
        assert_eq!(backend.op_count(), 4);

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 3);
        assert_eq!(backend.batch_count(), 2);
    }

    #[test]
    fn oversized_ready_front_is_submitted_alone() {
        let large_value = vec![b'x'; 128];
        let oversized_mutation = put(b"large", &large_value);
        let oversized_bytes = record_encoded_len(vec![oversized_mutation.clone()]);
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(
            Arc::clone(&backend),
            0,
            batch_config(2, 2, oversized_bytes - 1),
        )
        .unwrap();
        publish(&writeback, vec![oversized_mutation], 141);
        publish(&writeback, vec![put(b"small", b"value")], 142);

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 1);
        assert_eq!(writeback.queue_len(), 1);
        assert_eq!(backend.batch_count(), 1);

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 2);
        assert_eq!(backend.batch_count(), 2);
    }

    #[test]
    fn prepared_and_pinned_slots_cut_the_ready_prefix() {
        let backend = Arc::new(MemBlobs::new());
        let writeback =
            Writeback::new(Arc::clone(&backend), 0, batch_config(3, 3, usize::MAX)).unwrap();
        let mut first = bind(writeback.reserve(vec![put(b"a", b"one")]).unwrap(), 151);
        let mut second = bind(writeback.reserve(vec![put(b"b", b"two")]).unwrap(), 152);
        let mut third = bind(writeback.reserve(vec![put(b"c", b"three")]).unwrap(), 153);
        first.publish().unwrap();
        std::thread::scope(|scope| {
            let (published_tx, published_rx) = mpsc::channel();
            let publisher = scope.spawn(move || published_tx.send(third.publish()).unwrap());
            wait_until_ready(&writeback, 3);
            assert_eq!(writeback.highest_acknowledged(), 1);

            assert!(matches!(
                writeback.process_front(),
                ProcessOutcome::Advanced
            ));
            assert_eq!(writeback.applied_sequence(), 1);
            assert_eq!(writeback.queue_len(), 2);
            assert_eq!(backend.batch_count(), 1);
            assert!(matches!(writeback.process_front(), ProcessOutcome::Blocked));

            second.pin_unknown().unwrap();
            assert!(matches!(
                published_rx.recv_timeout(Duration::from_secs(1)).unwrap(),
                Err(ResolveError::BlockedByPriorUnknown {
                    sequence,
                    prior_unknown,
                }) if sequence.get() == 3 && prior_unknown.get() == 2
            ));
            publisher.join().unwrap();
        });
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Pinned(sequence) if sequence.get() == 2
        ));
        assert_eq!(writeback.applied_sequence(), 1);
        assert_eq!(writeback.queue_len(), 2);
        assert_eq!(backend.batch_count(), 1);
        let state = lock_recover(&writeback.state);
        assert_eq!(state.queue[0].state, SlotState::Prepared { pinned: true });
        assert_eq!(state.queue[1].state, SlotState::Prepared { pinned: true });
    }

    #[test]
    fn failed_ready_prefix_retires_none_and_retries_the_same_operations() {
        let backend = Arc::new(RecordingBlobs::default());
        backend.fail_next_writes(1);
        let writeback =
            Writeback::new(Arc::clone(&backend), 0, batch_config(4, 3, usize::MAX)).unwrap();
        publish(&writeback, vec![put(b"a", b"one")], 161);
        publish(&writeback, vec![put(b"b", b"two")], 162);
        publish(&writeback, vec![put(b"c", b"three")], 163);
        publish(&writeback, vec![put(b"d", b"four")], 164);

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::BackendFailed { sequence, .. } if sequence.get() == 1
        ));
        assert_eq!(writeback.applied_sequence(), 0);
        assert_eq!(writeback.queue_len(), 4);
        assert!(backend.inner.snapshot().is_empty());
        assert_eq!(backend.attempts().len(), 1);

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        let attempts = backend.attempts();
        assert_eq!(attempts.len(), 2);
        assert_eq!(
            attempts[0], attempts[1],
            "retry changed the captured prefix"
        );
        assert_eq!(attempts[0].len(), 6, "three log + data record pairs");
        assert_eq!(writeback.applied_sequence(), 3);
        assert_eq!(writeback.queue_len(), 1);
        assert_eq!(backend.inner.batch_count(), 1);
    }

    #[test]
    fn publication_during_backend_io_is_not_retired_with_captured_prefix() {
        let backend = Arc::new(BlockingBlobs::default());
        let writeback =
            Writeback::new(Arc::clone(&backend), 0, batch_config(2, 2, usize::MAX)).unwrap();
        let mut first = bind(writeback.reserve(vec![put(b"a", b"one")]).unwrap(), 171);
        let mut second = bind(writeback.reserve(vec![put(b"b", b"two")]).unwrap(), 172);
        first.publish().unwrap();

        std::thread::scope(|scope| {
            let consumer = scope.spawn(|| writeback.process_front());
            backend.wait_until_entered();
            second.publish().unwrap();
            backend.release();
            assert!(matches!(consumer.join().unwrap(), ProcessOutcome::Advanced));
        });

        assert_eq!(writeback.applied_sequence(), 1);
        assert_eq!(writeback.highest_acknowledged(), 2);
        assert_eq!(writeback.queue_len(), 1);
        assert_eq!(backend.inner.batch_count(), 1);

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 2);
        assert_eq!(backend.inner.batch_count(), 2);
    }

    #[test]
    fn same_key_updates_preserve_transaction_order_inside_one_batch() {
        let backend = Arc::new(RecordingBlobs::default());
        let writeback =
            Writeback::new(Arc::clone(&backend), 0, batch_config(2, 2, usize::MAX)).unwrap();
        publish(&writeback, vec![put(b"same", b"first")], 181);
        publish(&writeback, vec![put(b"same", b"second")], 182);

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 2);
        assert_eq!(backend.inner.batch_count(), 1);

        let attempts = backend.attempts();
        assert_eq!(attempts.len(), 1);
        assert_eq!(attempts[0].len(), 4);
        match (&attempts[0][1], &attempts[0][3]) {
            (
                OwnedBlobOp::Put {
                    key: first_key,
                    value: first_value,
                },
                OwnedBlobOp::Put {
                    key: second_key,
                    value: second_value,
                },
            ) => {
                assert_eq!(first_key, second_key);
                assert_eq!(first_value, b"first");
                assert_eq!(second_value, b"second");
            }
            operations => panic!("unexpected data operation order: {operations:?}"),
        }

        let snapshot = backend.inner.snapshot();
        assert_eq!(snapshot.len(), 3, "two logs and one materialized data key");
        assert!(!snapshot.values().any(|value| value.as_slice() == b"first"));
        assert_eq!(
            snapshot
                .values()
                .filter(|value| value.as_slice() == b"second")
                .count(),
            1
        );
    }

    #[test]
    fn failed_batch_is_retained_whole_and_retried() {
        let backend = Arc::new(MemBlobs::new());
        backend.fail_next_writes(1);
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(4, 0)).unwrap();
        writeback
            .reserve(vec![put(b"a", b"one"), put(b"b", b"two")])
            .unwrap()
            .bind(mako_timestamp_of(12))
            .unwrap()
            .publish()
            .unwrap();
        writeback
            .reserve(vec![put(b"c", b"three")])
            .unwrap()
            .bind(mako_timestamp_of(13))
            .unwrap()
            .publish()
            .unwrap();

        assert!(matches!(
            writeback.wait_applied(),
            Err(ApplyError::Backend {
                sequence,
                attempts: 1,
                ..
            }) if sequence.get() == 1
        ));
        assert!(
            backend.snapshot().is_empty(),
            "failed batch was all-or-none"
        );
        assert_eq!(
            writeback.applied_watermark(),
            AppliedWatermark::default(),
            "a rejected backend call cannot advance progress"
        );
        assert_eq!(
            writeback.queue_len(),
            2,
            "failed front and its ready suffix remain queued"
        );

        assert_eq!(writeback.wait_applied().unwrap(), 2);
        assert_eq!(backend.batch_count(), 2);
        assert_eq!(backend.op_count(), 5);
        assert_eq!(
            writeback.applied_watermark(),
            AppliedWatermark::recovered(2, Some(mako_timestamp_of(13)))
        );
        assert_eq!(writeback.queue_len(), 0);
    }

    #[test]
    fn exhausted_retry_continues_when_concurrent_consumer_completed_failed_sequence() {
        let failed = CommitSeq::new(7).unwrap();

        assert_eq!(
            retry_progress(7, 8, failed),
            RetryProgress::FailedSequenceApplied,
            "the exact failed sequence is no longer a live backend failure"
        );
        assert_eq!(retry_progress(8, 8, failed), RetryProgress::TargetApplied);
        assert_eq!(retry_progress(6, 8, failed), RetryProgress::NoProgress);
    }

    #[test]
    fn detached_abort_uses_capacity_but_never_assigns_a_sequence_or_slot() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(backend, 9, config(2, 0)).unwrap();

        let aborted = writeback.reserve(vec![put(b"a", b"one")]).unwrap();
        assert_eq!(writeback.detached_len(), 1);
        assert_eq!(writeback.queue_len(), 0);
        assert_eq!(writeback.highest_acknowledged(), 9);
        drop(aborted);

        let mut committed = bind(writeback.reserve(vec![put(b"b", b"two")]).unwrap(), 100);
        assert_eq!(committed.sequence().get(), 10, "abort left no sequence gap");
        committed.publish().unwrap();
        assert_eq!(writeback.wait_applied().unwrap(), 10);
    }

    #[test]
    fn record_preparation_failure_releases_detached_capacity() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(backend, 0, config(1, 0)).unwrap();

        assert!(matches!(
            writeback.reserve(vec![put(b"same", b"one"), put(b"same", b"two")]),
            Err(ReserveError::Record(_))
        ));
        assert_eq!(writeback.detached_len(), 0);
        assert_eq!(writeback.queue_len(), 0);

        let permit = writeback.reserve(vec![put(b"valid", b"value")]).unwrap();
        assert_eq!(writeback.detached_len(), 1);
        drop(permit);
    }

    #[test]
    fn permanent_native_record_failure_latches_exact_health_error() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(3, 0)).unwrap();
        let mut detached_before_failure = writeback
            .reserve(vec![put(b"already-detached", b"value")])
            .unwrap();

        let mut malformed = encoded_native_record(1, 191, vec![put(b"bad", b"crc")]);
        *malformed.last_mut().unwrap() ^= 1;
        assert_eq!(publish_native_bytes(&writeback, 191, malformed).get(), 1);

        let latched_source = match writeback.process_front() {
            ProcessOutcome::RecordFailed { sequence, error } => {
                assert_eq!(sequence.get(), 1);
                assert!(matches!(error, RecordError::BadChecksum { .. }));
                error
            }
            outcome => panic!("malformed native record was not rejected: {outcome:?}"),
        };
        assert_eq!(writeback.queue_len(), 1, "malformed slot stays queued");
        assert_eq!(
            backend.batch_count(),
            0,
            "malformed bytes never reach Rocks"
        );

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::RecordFailed { sequence, error }
                if sequence.get() == 1 && error == latched_source
        ));
        assert!(matches!(
            writeback.ensure_no_unknown(),
            Err(ApplyError::Record { sequence, source })
                if sequence.get() == 1 && source == latched_source
        ));
        assert!(matches!(
            writeback.wait_applied(),
            Err(ApplyError::Record { sequence, source })
                if sequence.get() == 1 && source == latched_source
        ));
        assert!(matches!(
            writeback.reserve(vec![put(b"new", b"work")]),
            Err(ReserveError::PermanentRecordFailure { sequence, source })
                if sequence.get() == 1 && source == latched_source
        ));
        assert!(matches!(
            detached_before_failure.bind(mako_timestamp_of(192)),
            Err(ReserveError::PermanentRecordFailure { sequence, source })
                if sequence.get() == 1 && source == latched_source
        ));
        assert!(detached_before_failure.owns_claim);
    }

    #[test]
    fn barrier_snapshot_ignores_a_later_permanent_record_failure() {
        let backend = Arc::new(MemBlobs::new());
        let writeback =
            Writeback::new(Arc::clone(&backend), 0, batch_config(2, 2, usize::MAX)).unwrap();
        publish(&writeback, vec![put(b"safe", b"prefix")], 197);
        let mut malformed = encoded_native_record(2, 198, vec![put(b"bad", b"suffix")]);
        *malformed.last_mut().unwrap() ^= 1;
        publish_native_bytes(&writeback, 198, malformed);
        assert_eq!(writeback.highest_acknowledged(), 2);

        // Model a barrier that captured sequence 1 immediately before the
        // second publication. It must neither decode nor apply sequence 2.
        assert_eq!(writeback.wait_applied_through(1).unwrap(), 1);
        assert_eq!(writeback.applied_sequence(), 1);
        assert_eq!(writeback.queue_len(), 1);
        assert_eq!(backend.batch_count(), 1);
        assert!(writeback.ensure_no_unknown().is_ok());

        // The ordinary background mode remains unbounded and discovers the
        // malformed suffix on its next attempt.
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::RecordFailed {
                sequence,
                error: RecordError::BadChecksum { .. },
            } if sequence.get() == 2
        ));
        assert!(matches!(
            writeback.ensure_no_unknown(),
            Err(ApplyError::Record {
                sequence,
                source: RecordError::BadChecksum { .. },
            }) if sequence.get() == 2
        ));
    }

    #[test]
    fn barrier_snapshot_ignores_a_later_retryable_allocation_failure() {
        let backend = Arc::new(MemBlobs::new());
        let writeback =
            Writeback::new(Arc::clone(&backend), 0, batch_config(2, 2, usize::MAX)).unwrap();
        publish(&writeback, vec![put(b"safe", b"prefix")], 199);
        publish(&writeback, vec![put(b"retry", b"suffix")], 200);
        let later = CommitSeq::new(2).unwrap();
        let failure = force_materialization_failure(later, RecordError::AllocationFailed);

        assert_eq!(writeback.wait_applied_through(1).unwrap(), 1);
        assert_eq!(writeback.applied_sequence(), 1);
        assert_eq!(writeback.queue_len(), 1);
        assert_eq!(backend.batch_count(), 1);

        // Background processing is still unbounded, observes the transient
        // failure, and leaves queue health retryable.
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::RecordFailed {
                sequence,
                error: RecordError::AllocationFailed,
            } if sequence == later
        ));
        assert_eq!(writeback.applied_sequence(), 1);
        assert_eq!(writeback.queue_len(), 1);
        assert!(writeback.ensure_no_unknown().is_ok());

        drop(failure);
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 2);
        assert_eq!(backend.batch_count(), 2);
    }

    #[test]
    fn permanent_record_failure_wakes_capacity_and_acknowledgement_waiters() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Arc::new(Writeback::new(backend, 0, config(2, 0)).unwrap());
        let first = bind(writeback.reserve(vec![put(b"front", b"one")]).unwrap(), 193);
        let mut second = bind(writeback.reserve(vec![put(b"later", b"two")]).unwrap(), 194);

        std::thread::scope(|scope| {
            let (published_tx, published_rx) = mpsc::channel();
            let publisher = scope.spawn(move || published_tx.send(second.publish()).unwrap());
            wait_until_ready(&writeback, 2);
            assert!(
                published_rx
                    .recv_timeout(Duration::from_millis(30))
                    .is_err()
            );

            assert!(matches!(
                writeback.record_failure_outcome(
                    CommitSeq::new(1).unwrap(),
                    RecordError::BadMagic,
                ),
                ProcessOutcome::RecordFailed { sequence, error: RecordError::BadMagic }
                    if sequence.get() == 1
            ));
            assert!(matches!(
                published_rx.recv_timeout(Duration::from_secs(1)).unwrap(),
                Err(ResolveError::BlockedByPriorRecordFailure {
                    sequence,
                    prior_failure,
                    source: RecordError::BadMagic,
                }) if sequence.get() == 2 && prior_failure.get() == 1
            ));
            publisher.join().unwrap();
        });
        drop(first);

        let backend = Arc::new(MemBlobs::new());
        let writeback = Arc::new(Writeback::new(backend, 0, config(1, 0)).unwrap());
        let mut malformed = encoded_native_record(1, 195, vec![put(b"bad", b"crc")]);
        *malformed.last_mut().unwrap() ^= 1;
        publish_native_bytes(&writeback, 195, malformed);

        let (result_tx, result_rx) = mpsc::channel();
        let producer_writeback = Arc::clone(&writeback);
        let producer = std::thread::spawn(move || {
            let result = match producer_writeback.reserve(vec![put(b"blocked", b"work")]) {
                Err(ReserveError::PermanentRecordFailure { sequence, source }) => {
                    Ok((sequence, source))
                }
                Err(error) => Err(format!("unexpected reservation error: {error}")),
                Ok(permit) => {
                    drop(permit);
                    Err("reservation unexpectedly succeeded".to_owned())
                }
            };
            result_tx.send(result).unwrap();
        });
        assert!(result_rx.recv_timeout(Duration::from_millis(30)).is_err());
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::RecordFailed { sequence, error: RecordError::BadChecksum { .. } }
                if sequence.get() == 1
        ));
        let (sequence, source) = result_rx
            .recv_timeout(Duration::from_secs(1))
            .unwrap()
            .expect("backpressured reservation observed the permanent failure");
        assert_eq!(sequence.get(), 1);
        assert!(matches!(source, RecordError::BadChecksum { .. }));
        producer.join().unwrap();
    }

    #[test]
    fn allocation_failure_remains_retryable_and_does_not_poison_health() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(2, 0)).unwrap();
        publish(&writeback, vec![put(b"retry", b"value")], 196);
        let sequence = CommitSeq::new(1).unwrap();

        assert!(matches!(
            writeback.record_failure_outcome(sequence, RecordError::AllocationFailed),
            ProcessOutcome::RecordFailed {
                sequence: failed,
                error: RecordError::AllocationFailed,
            } if failed == sequence
        ));
        assert!(writeback.ensure_no_unknown().is_ok());
        let detached = writeback.reserve(vec![put(b"still", b"healthy")]).unwrap();
        drop(detached);

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 1);
        assert_eq!(backend.batch_count(), 1);
    }

    #[test]
    fn permanent_record_latch_retains_the_earliest_sequence() {
        let writeback = Writeback::new(MemBlobs::new(), 0, config(1, 0)).unwrap();
        let seventh = CommitSeq::new(7).unwrap();
        let third = CommitSeq::new(3).unwrap();

        let _ = writeback.record_failure_outcome(seventh, RecordError::BadMagic);
        let _ = writeback.record_failure_outcome(third, RecordError::Truncated);
        let _ = writeback.record_failure_outcome(seventh, RecordError::InvalidSequence);

        assert!(matches!(
            writeback.ensure_no_unknown(),
            Err(ApplyError::Record {
                sequence,
                source: RecordError::Truncated,
            }) if sequence == third
        ));
    }

    #[test]
    fn cache_sequence_is_assigned_by_bind_order_not_reserve_order() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(backend, 0, config(2, 0)).unwrap();
        let reserved_first = writeback.reserve(vec![put(b"a", b"one")]).unwrap();
        let reserved_second = writeback.reserve(vec![put(b"b", b"two")]).unwrap();

        let mut bound_first = bind(reserved_second, 202);
        let mut bound_second = bind(reserved_first, 101);
        assert_eq!(bound_first.sequence().get(), 1);
        assert_eq!(bound_second.sequence().get(), 2);

        std::thread::scope(|scope| {
            let (published_tx, published_rx) = mpsc::channel();
            let publisher = scope.spawn(move || published_tx.send(bound_second.publish()).unwrap());
            wait_until_ready(&writeback, 2);
            assert!(matches!(writeback.process_front(), ProcessOutcome::Blocked));
            assert_eq!(writeback.applied_watermark(), AppliedWatermark::default());
            assert_eq!(writeback.highest_acknowledged(), 0);

            bound_first.publish().unwrap();
            assert_eq!(
                published_rx
                    .recv_timeout(Duration::from_secs(1))
                    .unwrap()
                    .unwrap()
                    .get(),
                2
            );
            publisher.join().unwrap();
        });
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(
            writeback.applied_watermark(),
            AppliedWatermark::recovered(1, Some(mako_timestamp_of(202)))
        );
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(
            writeback.applied_watermark(),
            AppliedWatermark::recovered(2, Some(mako_timestamp_of(101))),
            "the timestamp names the frontier record; it is not a numeric maximum"
        );
    }

    #[test]
    fn queue_tokens_track_current_vecdeque_offsets_after_front_retirement() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(backend, 0, config(4, 0)).unwrap();
        let mut first = bind(writeback.reserve(vec![put(b"a", b"one")]).unwrap(), 203);
        let mut second = bind(writeback.reserve(vec![put(b"b", b"two")]).unwrap(), 204);
        let mut third = bind(writeback.reserve(vec![put(b"c", b"three")]).unwrap(), 205);
        let first_token = first.token;
        let second_token = second.token;
        let third_token = third.token;

        {
            let mut state = lock_recover(&writeback.state);
            writeback.import_bound_locked(&mut state);
            assert_eq!(state.queue_offset(first_token), Some(0));
            assert_eq!(state.queue_offset(second_token), Some(1));
            assert_eq!(state.queue_offset(third_token), Some(2));
        }

        first.publish().unwrap();
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        {
            let state = lock_recover(&writeback.state);
            assert_eq!(state.queue_offset(first_token), None);
            assert_eq!(state.queue_offset(second_token), Some(0));
            assert_eq!(state.queue_offset(third_token), Some(1));
        }
        assert!(matches!(
            writeback.resolve(first_token, Resolution::PinUnknown),
            Err(ResolveError::Missing { sequence }) if sequence.get() == 1
        ));

        // Both handles were minted before the front moved. Resolving them
        // afterward must use their new offsets rather than a stale bind-time
        // index or a scan from the current front.
        std::thread::scope(|scope| {
            let (published_tx, published_rx) = mpsc::channel();
            let publisher = scope.spawn(move || published_tx.send(third.publish()).unwrap());
            wait_until_ready(&writeback, 3);
            assert_eq!(writeback.highest_acknowledged(), 1);

            second.publish().unwrap();
            assert_eq!(
                published_rx
                    .recv_timeout(Duration::from_secs(1))
                    .unwrap()
                    .unwrap()
                    .get(),
                3
            );
            publisher.join().unwrap();
        });
        assert_eq!(writeback.wait_applied().unwrap(), 3);
    }

    #[test]
    fn non_power_of_two_capacity_reuses_publication_ring_without_aliasing() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(3, 0)).unwrap();

        for raw_sequence in 1_u64..=12 {
            assert_eq!(
                publish(
                    &writeback,
                    vec![put(b"ring", &raw_sequence.to_le_bytes())],
                    300 + raw_sequence as u32,
                )
                .get(),
                raw_sequence
            );
            assert!(matches!(
                writeback.process_front(),
                ProcessOutcome::Advanced
            ));
        }

        assert_eq!(writeback.highest_acknowledged(), 12);
        assert_eq!(writeback.applied_sequence(), 12);
        assert_eq!(backend.batch_count(), 12);
    }

    #[test]
    fn prepared_hole_blocks_later_ready_transaction_and_acknowledgement() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(4, 0)).unwrap();
        let mut first = bind(writeback.reserve(vec![put(b"a", b"one")]).unwrap(), 21);
        let mut second = bind(writeback.reserve(vec![put(b"b", b"two")]).unwrap(), 22);
        std::thread::scope(|scope| {
            let (published_tx, published_rx) = mpsc::channel();
            let publisher = scope.spawn(move || published_tx.send(second.publish()).unwrap());
            wait_until_ready(&writeback, 2);

            assert!(matches!(writeback.process_front(), ProcessOutcome::Blocked));
            assert_eq!(backend.batch_count(), 0);
            assert_eq!(writeback.highest_acknowledged(), 0);
            assert!(matches!(
                published_rx.try_recv(),
                Err(mpsc::TryRecvError::Empty)
            ));

            first.publish().unwrap();
            assert_eq!(
                published_rx
                    .recv_timeout(Duration::from_secs(1))
                    .unwrap()
                    .unwrap()
                    .get(),
                2
            );
            publisher.join().unwrap();
        });
        assert_eq!(writeback.wait_applied().unwrap(), 2);
        assert_eq!(backend.batch_count(), 2);
        assert_eq!(writeback.applied_sequence(), 2);
    }

    #[test]
    fn bind_and_publish_make_the_prepared_to_ready_transition_explicit() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(1, 0)).unwrap();
        let mut bound = bind(
            writeback
                .reserve(vec![put(b"ready-transition", b"value")])
                .unwrap(),
            23,
        );

        {
            let mut state = lock_recover(&writeback.state);
            writeback.import_bound_locked(&mut state);
            let slot = state.queue.front().expect("bind creates one slot");
            assert_eq!(slot.sequence.get(), 1);
            assert_eq!(slot.state, SlotState::Prepared { pinned: false });
            assert!(
                slot.record.is_none(),
                "bind must not expose an unfinalized record as Ready"
            );
            assert_eq!(writeback.highest_acknowledged(), 0);
        }

        assert_eq!(bound.publish().unwrap().get(), 1);
        {
            let mut state = lock_recover(&writeback.state);
            writeback.import_bound_locked(&mut state);
            assert!(writeback.harvest_published_slot_locked(&mut state, 0));
            let slot = state.queue.front().expect("published slot remains queued");
            assert_eq!(slot.state, SlotState::Ready);
            assert!(
                slot.record.is_some(),
                "publication must finalize the record before Ready"
            );
            assert_eq!(writeback.highest_acknowledged(), 1);
        }

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 1);
        assert_eq!(backend.batch_count(), 1);
    }

    #[test]
    fn out_of_order_publications_wait_for_one_dense_acknowledgement_prefix() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(4, 0)).unwrap();
        let mut first = bind(writeback.reserve(vec![put(b"a", b"one")]).unwrap(), 31);
        let mut second = bind(writeback.reserve(vec![put(b"b", b"two")]).unwrap(), 32);
        let mut third = bind(writeback.reserve(vec![put(b"c", b"three")]).unwrap(), 33);

        std::thread::scope(|scope| {
            let (second_tx, second_rx) = mpsc::channel();
            let (third_tx, third_rx) = mpsc::channel();
            let second_publisher = scope.spawn(move || second_tx.send(second.publish()).unwrap());
            let third_publisher = scope.spawn(move || third_tx.send(third.publish()).unwrap());
            wait_until_ready(&writeback, 2);
            wait_until_ready(&writeback, 3);
            wait_for_acknowledgement_waiters(&writeback, 2);

            assert_eq!(writeback.highest_acknowledged(), 0);
            assert!(matches!(
                second_rx.try_recv(),
                Err(mpsc::TryRecvError::Empty)
            ));
            assert!(matches!(
                third_rx.try_recv(),
                Err(mpsc::TryRecvError::Empty)
            ));
            assert!(matches!(writeback.process_front(), ProcessOutcome::Blocked));

            first.publish().unwrap();
            assert_eq!(
                second_rx
                    .recv_timeout(Duration::from_secs(1))
                    .unwrap()
                    .unwrap()
                    .get(),
                2
            );
            assert_eq!(
                third_rx
                    .recv_timeout(Duration::from_secs(1))
                    .unwrap()
                    .unwrap()
                    .get(),
                3
            );
            assert_eq!(writeback.highest_acknowledged(), 3);
            second_publisher.join().unwrap();
            third_publisher.join().unwrap();
        });

        assert_eq!(writeback.acknowledgement_waiters.load(Ordering::SeqCst), 0);
        assert_eq!(writeback.wait_applied().unwrap(), 3);
        assert_eq!(backend.batch_count(), 3);
        assert_eq!(writeback.applied_sequence(), 3);
    }

    #[test]
    fn later_unknown_does_not_block_acknowledging_an_earlier_delayed_publication() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(4, 0)).unwrap();
        let mut first = bind(writeback.reserve(vec![put(b"a", b"one")]).unwrap(), 34);
        let mut second = bind(writeback.reserve(vec![put(b"b", b"two")]).unwrap(), 35);

        second.pin_unknown().unwrap();
        assert_eq!(first.publish().unwrap().get(), 1);
        assert_eq!(writeback.highest_acknowledged(), 1);
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.applied_sequence(), 1);
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Pinned(sequence) if sequence.get() == 2
        ));
    }

    #[test]
    fn unknown_outcome_pins_queue_and_rejects_new_reservations() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(backend, 0, config(4, 0)).unwrap();
        let mut unknown = bind(writeback.reserve(vec![put(b"a", b"one")]).unwrap(), 41);
        let mut later = bind(writeback.reserve(vec![put(b"b", b"two")]).unwrap(), 42);
        unknown.pin_unknown().unwrap();
        assert!(matches!(
            later.publish(),
            Err(ResolveError::BlockedByPriorUnknown {
                sequence,
                prior_unknown,
            }) if sequence.get() == 2 && prior_unknown.get() == 1
        ));

        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Pinned(sequence) if sequence.get() == 1
        ));
        // No sequence beyond the ambiguity was acknowledged, so the empty
        // acknowledged prefix is a valid application target. Clean close still
        // calls ensure_no_unknown and rejects the pinned queue.
        assert_eq!(writeback.wait_applied().unwrap(), 0);
        assert!(matches!(
            writeback.ensure_no_unknown(),
            Err(ApplyError::UnknownOutcome { sequence }) if sequence.get() == 1
        ));
        assert!(matches!(
            writeback.reserve(vec![put(b"b", b"two")]),
            Err(ReserveError::UnknownOutcome { sequence }) if sequence.get() == 1
        ));
        assert_eq!(writeback.applied_sequence(), 0);
        assert_eq!(writeback.highest_acknowledged(), 0);
        assert_eq!(writeback.queue_len(), 2);
        let state = lock_recover(&writeback.state);
        for slot in &state.queue {
            assert!(
                matches!(slot.state, SlotState::Prepared { pinned: true }),
                "ambiguous prefix and its known-committed suffix stay pinned"
            );
            assert!(
                slot.record.is_some(),
                "every pinned slot retains its finalized write set"
            );
        }
    }

    #[test]
    fn dropping_bound_unknown_finalizes_and_retains_its_write_set() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(backend, 0, config(2, 0)).unwrap();
        let unknown = bind(
            writeback
                .reserve(vec![put(b"retained", b"ambiguous")])
                .unwrap(),
            43,
        );
        drop(unknown);

        let state = lock_recover(&writeback.state);
        let slot = state.queue.front().expect("unknown slot remains queued");
        assert!(matches!(slot.state, SlotState::Prepared { pinned: true }));
        let record = slot
            .record
            .as_ref()
            .expect("Drop finalized and retained the ambiguous write set");
        assert_eq!(record.mako_timestamp(), mako_timestamp_of(43));
    }

    #[test]
    fn bind_rejects_unknown_health_that_arose_after_detached_reserve() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(backend, 0, config(3, 0)).unwrap();
        let first = writeback.reserve(vec![put(b"a", b"one")]).unwrap();
        let mut detached_before_unknown = writeback.reserve(vec![put(b"b", b"two")]).unwrap();
        bind(first, 45).pin_unknown().unwrap();

        assert!(matches!(
            detached_before_unknown.bind(mako_timestamp_of(46)),
            Err(ReserveError::UnknownOutcome { sequence }) if sequence.get() == 1
        ));
        assert!(
            detached_before_unknown.prepared.is_some(),
            "hook-time rejection retains the fully encoded record"
        );
        assert!(detached_before_unknown.owns_claim);
        assert_eq!(
            writeback.detached_len(),
            1,
            "rejected bind remains detached until native abort returns"
        );
        assert_eq!(writeback.queue_len(), 1, "rejected bind left no slot");

        drop(detached_before_unknown);
        assert_eq!(
            writeback.detached_len(),
            0,
            "prepared storage is released outside the hook"
        );
    }

    #[test]
    fn wait_applied_covers_safe_acknowledged_prefix_before_later_unknown() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(4, 0)).unwrap();
        let mut acknowledged = bind(writeback.reserve(vec![put(b"a", b"one")]).unwrap(), 51);
        let mut later_unknown = bind(writeback.reserve(vec![put(b"b", b"two")]).unwrap(), 52);
        later_unknown.pin_unknown().unwrap();
        // A later ambiguity must not prevent acknowledgement and application of
        // an earlier safe prefix whose native outcome is known.
        acknowledged.publish().unwrap();

        assert_eq!(writeback.highest_acknowledged(), 1);
        assert_eq!(writeback.wait_applied().unwrap(), 1);
        assert_eq!(writeback.applied_sequence(), 1);
        assert_eq!(backend.batch_count(), 1);
        assert_eq!(writeback.queue_len(), 1, "later pinned slot remains queued");
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Pinned(sequence) if sequence.get() == 2
        ));
    }

    #[test]
    fn unknown_outcome_wakes_a_backpressured_reserver() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Arc::new(Writeback::new(backend, 0, config(1, 0)).unwrap());
        let mut first = bind(writeback.reserve(vec![put(b"a", b"one")]).unwrap(), 61);

        let (result_tx, result_rx) = mpsc::channel();
        let producer_writeback = Arc::clone(&writeback);
        let producer = std::thread::spawn(move || {
            let rejected = matches!(
                producer_writeback.reserve(vec![put(b"b", b"two")]),
                Err(ReserveError::UnknownOutcome { sequence }) if sequence.get() == 1
            );
            result_tx.send(rejected).unwrap();
        });

        assert!(result_rx.recv_timeout(Duration::from_millis(30)).is_err());
        first.pin_unknown().unwrap();
        assert!(result_rx.recv_timeout(Duration::from_secs(1)).unwrap());
        producer.join().unwrap();
    }

    #[test]
    fn wait_applied_targets_acknowledged_not_reserved_tail() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(backend, 40, config(4, 0)).unwrap();
        writeback
            .reserve(vec![put(b"a", b"one")])
            .unwrap()
            .bind(mako_timestamp_of(71))
            .unwrap()
            .publish()
            .unwrap();
        let mut still_prepared = bind(writeback.reserve(vec![put(b"b", b"two")]).unwrap(), 72);

        assert_eq!(writeback.highest_acknowledged(), 41);
        assert_eq!(writeback.wait_applied().unwrap(), 41);
        assert_eq!(writeback.applied_sequence(), 41);
        assert_eq!(writeback.queue_len(), 1);

        still_prepared.publish().unwrap();
        assert_eq!(writeback.wait_applied().unwrap(), 42);
        assert_eq!(writeback.applied_sequence(), 42);
    }

    #[test]
    fn apply_progress_wait_does_not_sleep_past_ready_publication() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Arc::new(Writeback::new(backend, 0, config(2, 0)).unwrap());
        bind(
            writeback.reserve(vec![put(b"ready", b"value")]).unwrap(),
            73,
        )
        .publish()
        .unwrap();

        let (returned_tx, returned_rx) = mpsc::channel();
        let waiter = Arc::clone(&writeback);
        let thread = std::thread::spawn(move || {
            waiter.wait_for_apply_progress(1);
            returned_tx.send(()).unwrap();
        });

        returned_rx
            .recv_timeout(Duration::from_secs(1))
            .expect("a Ready front must not wait for a second notification");
        thread.join().unwrap();
    }

    #[test]
    fn explicit_apply_progress_waiter_remains_notification_driven() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Arc::new(Writeback::new(backend, 0, config(1, 0)).unwrap());
        let mut pending = bind(
            writeback
                .reserve(vec![put(b"explicit", b"barrier")])
                .unwrap(),
            730,
        );

        let (returned_tx, returned_rx) = mpsc::channel();
        let waiter_writeback = Arc::clone(&writeback);
        let waiter = std::thread::spawn(move || {
            waiter_writeback.wait_for_apply_progress(1);
            returned_tx.send(()).unwrap();
        });

        let registration_deadline = Instant::now() + Duration::from_secs(1);
        while writeback.activity_waiter_count() == 0 {
            assert!(
                Instant::now() < registration_deadline,
                "explicit apply waiter did not register"
            );
            std::thread::yield_now();
        }
        pending.publish().unwrap();
        returned_rx
            .recv_timeout(Duration::from_secs(1))
            .expect("publication did not wake the explicit apply waiter");
        waiter.join().unwrap();
        assert_eq!(writeback.activity_waiter_count(), 0);
    }

    #[test]
    fn healthy_publication_uses_the_registered_activity_waiter_handshake() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Arc::new(Writeback::new(backend, 0, config(2, 0)).unwrap());

        // With no waiter registered, publication deliberately skips the
        // condition variable. A waiter entering afterward must observe the
        // atomic publication during its post-registration predicate recheck.
        let mut first = bind(
            writeback.reserve(vec![put(b"before", b"wait")]).unwrap(),
            731,
        );
        first.publish().unwrap();
        assert_eq!(writeback.activity_waiters.load(Ordering::SeqCst), 0);
        let started = Instant::now();
        writeback.wait_for_activity(Duration::from_secs(1));
        assert!(
            started.elapsed() < Duration::from_millis(100),
            "post-registration recheck slept after an earlier publication"
        );
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));

        // Conversely, a publisher racing after registration must take the
        // guarded notification path and wake the explicit waiter promptly.
        let mut second = bind(
            writeback.reserve(vec![put(b"after", b"wait")]).unwrap(),
            732,
        );
        let (returned_tx, returned_rx) = mpsc::channel();
        let waiter_writeback = Arc::clone(&writeback);
        let waiter = std::thread::spawn(move || {
            waiter_writeback.wait_for_activity(Duration::from_secs(10));
            returned_tx.send(()).unwrap();
        });
        let deadline = Instant::now() + Duration::from_secs(1);
        while writeback.activity_waiters.load(Ordering::SeqCst) == 0 {
            assert!(
                Instant::now() < deadline,
                "activity waiter did not register"
            );
            std::thread::yield_now();
        }
        second.publish().unwrap();
        returned_rx
            .recv_timeout(Duration::from_secs(1))
            .expect("registered activity waiter missed healthy publication");
        waiter.join().unwrap();
        assert_eq!(writeback.activity_waiters.load(Ordering::SeqCst), 0);
    }

    #[test]
    fn publishing_a_later_slot_does_not_wake_a_waiter_blocked_on_the_front() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Arc::new(Writeback::new(backend, 0, config(2, 0)).unwrap());
        let mut first = bind(writeback.reserve(vec![put(b"front", b"one")]).unwrap(), 74);
        let mut second = bind(writeback.reserve(vec![put(b"later", b"two")]).unwrap(), 75);

        std::thread::scope(|scope| {
            let (started_tx, started_rx) = mpsc::channel();
            let (returned_tx, returned_rx) = mpsc::channel();
            let waiter_writeback = Arc::clone(&writeback);
            let waiter = scope.spawn(move || {
                started_tx.send(()).unwrap();
                waiter_writeback.wait_for_apply_progress(2);
                returned_tx.send(()).unwrap();
            });
            started_rx.recv_timeout(Duration::from_secs(1)).unwrap();
            let deadline = Instant::now() + Duration::from_secs(1);
            while writeback.activity_waiters.load(Ordering::SeqCst) == 0 {
                assert!(
                    Instant::now() < deadline,
                    "apply-progress waiter did not register"
                );
                std::thread::yield_now();
            }

            let (published_tx, published_rx) = mpsc::channel();
            let publisher = scope.spawn(move || published_tx.send(second.publish()).unwrap());
            wait_until_ready(&writeback, 2);
            assert!(matches!(
                published_rx.try_recv(),
                Err(mpsc::TryRecvError::Empty)
            ));
            assert!(
                matches!(returned_rx.try_recv(), Err(mpsc::TryRecvError::Empty)),
                "a Ready suffix cannot unblock a Prepared front"
            );

            first.publish().unwrap();
            assert_eq!(
                published_rx
                    .recv_timeout(Duration::from_secs(1))
                    .unwrap()
                    .unwrap()
                    .get(),
                2
            );
            returned_rx.recv_timeout(Duration::from_secs(1)).unwrap();
            publisher.join().unwrap();
            waiter.join().unwrap();
        });
        assert_eq!(writeback.activity_waiters.load(Ordering::SeqCst), 0);
        assert_eq!(writeback.wait_applied().unwrap(), 2);
    }

    #[test]
    fn full_queue_backpressures_until_front_advances() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Arc::new(Writeback::new(backend, 0, config(1, 0)).unwrap());
        writeback
            .reserve(vec![put(b"a", b"one")])
            .unwrap()
            .bind(mako_timestamp_of(81))
            .unwrap()
            .publish()
            .unwrap();

        let (reserved_tx, reserved_rx) = mpsc::channel();
        let producer_writeback = Arc::clone(&writeback);
        let producer = std::thread::spawn(move || {
            let reservation = producer_writeback.reserve(vec![put(b"b", b"two")]).unwrap();
            reserved_tx.send(()).unwrap();
            drop(reservation);
        });

        assert!(reserved_rx.recv_timeout(Duration::from_millis(30)).is_err());
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        reserved_rx.recv_timeout(Duration::from_secs(1)).unwrap();
        producer.join().unwrap();
        assert_eq!(writeback.queue_len(), 0);
        assert_eq!(writeback.detached_len(), 0);
    }

    #[test]
    fn detached_permit_backpressures_another_reserver_until_drop() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Arc::new(Writeback::new(backend, 0, config(1, 0)).unwrap());
        let first = writeback.reserve(vec![put(b"a", b"one")]).unwrap();
        assert_eq!(writeback.detached_len(), 1);
        assert_eq!(writeback.queue_len(), 0);

        let (reserved_tx, reserved_rx) = mpsc::channel();
        let producer_writeback = Arc::clone(&writeback);
        let producer = std::thread::spawn(move || {
            let second = producer_writeback.reserve(vec![put(b"b", b"two")]).unwrap();
            reserved_tx.send(()).unwrap();
            drop(second);
        });

        assert!(reserved_rx.recv_timeout(Duration::from_millis(30)).is_err());
        drop(first);
        reserved_rx.recv_timeout(Duration::from_secs(1)).unwrap();
        producer.join().unwrap();
        assert_eq!(writeback.detached_len(), 0);
        assert_eq!(writeback.queue_len(), 0);
    }

    #[test]
    fn concurrent_capacity_claim_ignores_stale_legacy_tail_and_drop_releases() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Writeback::new(backend, 0, config(1, 0)).unwrap();
        writeback.next_bound.store(u64::MAX, Ordering::Release);
        let exact_record_bytes = record_encoded_len(vec![put(b"packed", b"capacity")]);

        let permit = writeback
            .reserve_native_arena_fast(exact_record_bytes)
            .expect("packed capacity admission must not read the legacy tail")
            .unwrap();
        assert_eq!(writeback.detached_len(), 1);
        assert_eq!(writeback.occupied.load(), 1);
        drop(permit);
        assert_eq!(writeback.detached_len(), 0);
        assert_eq!(writeback.occupied.load(), 0);

        let permit = writeback
            .reserve_native_arena_fast(exact_record_bytes)
            .expect("dropping the rejected native transaction must restore capacity")
            .unwrap();
        drop(permit);
        assert_eq!(writeback.occupied.load(), 0);
    }

    #[test]
    fn retiring_final_legacy_sequence_wakes_waiter_which_rejects_at_bind() {
        let backend = Arc::new(MemBlobs::new());
        let writeback = Arc::new(
            Writeback::new(backend, u64::MAX - 1, config(1, 0))
                .expect("the final sequence remains available"),
        );
        let final_permit = writeback.reserve(vec![put(b"final", b"value")]).unwrap();
        let start = Arc::new(Barrier::new(2));
        let (result_tx, result_rx) = mpsc::channel();
        let waiter_writeback = Arc::clone(&writeback);
        let waiter_start = Arc::clone(&start);
        let waiter = std::thread::spawn(move || {
            waiter_start.wait();
            let exhausted = match waiter_writeback.reserve(vec![put(b"blocked", b"never")]) {
                Ok(mut permit) => matches!(
                    permit.bind(mako_timestamp_of(83)),
                    Err(ReserveError::SequenceExhausted)
                ),
                Err(_) => false,
            };
            result_tx.send(exhausted).unwrap();
        });
        start.wait();
        assert!(result_rx.recv_timeout(Duration::from_millis(30)).is_err());

        let mut final_reservation = bind(final_permit, 82);
        assert_eq!(final_reservation.sequence().get(), u64::MAX);
        final_reservation.publish().unwrap();
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert!(
            result_rx.recv_timeout(Duration::from_secs(1)).unwrap(),
            "the capacity waiter did not reach legacy tail exhaustion"
        );
        waiter.join().unwrap();
        assert_eq!(writeback.applied_sequence(), u64::MAX);
        assert_eq!(writeback.occupied.load(), 0);
    }

    #[derive(Debug, Default)]
    struct BlockingBlobs {
        inner: MemBlobs,
        gate: Mutex<(bool, bool)>,
        changed: Condvar,
    }

    impl BlockingBlobs {
        fn wait_until_entered(&self) {
            let mut gate = self.gate.lock().unwrap();
            while !gate.0 {
                gate = self.changed.wait(gate).unwrap();
            }
        }

        fn release(&self) {
            let mut gate = self.gate.lock().unwrap();
            gate.1 = true;
            self.changed.notify_all();
        }
    }

    impl Blobs for BlockingBlobs {
        fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
            self.inner.get(key)
        }

        fn write_batch(&self, operations: &[BlobOp<'_>]) -> Result<(), BlobError> {
            let mut gate = self.gate.lock().unwrap();
            gate.0 = true;
            self.changed.notify_all();
            while !gate.1 {
                gate = self.changed.wait(gate).unwrap();
            }
            drop(gate);
            self.inner.write_batch(operations)
        }

        fn for_each_key(&self, f: &mut dyn FnMut(&[u8])) -> Result<(), BlobError> {
            self.inner.for_each_key(f)
        }
    }

    #[derive(Debug, Default)]
    struct ApplyThenBlockBlobs {
        inner: MemBlobs,
        gate: Mutex<(bool, bool)>,
        changed: Condvar,
    }

    impl ApplyThenBlockBlobs {
        fn wait_until_applied(&self) -> bool {
            let mut gate = self.gate.lock().unwrap();
            let deadline = std::time::Instant::now() + Duration::from_secs(1);
            while !gate.0 {
                let now = std::time::Instant::now();
                if now >= deadline {
                    return false;
                }
                let (next, timeout) = self.changed.wait_timeout(gate, deadline - now).unwrap();
                gate = next;
                if timeout.timed_out() && !gate.0 {
                    return false;
                }
            }
            true
        }

        fn release(&self) {
            let mut gate = self.gate.lock().unwrap();
            gate.1 = true;
            self.changed.notify_all();
        }
    }

    impl Blobs for ApplyThenBlockBlobs {
        fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
            self.inner.get(key)
        }

        fn write_batch(&self, operations: &[BlobOp<'_>]) -> Result<(), BlobError> {
            self.inner.write_batch(operations)?;
            let mut gate = self.gate.lock().unwrap();
            gate.0 = true;
            self.changed.notify_all();
            while !gate.1 {
                gate = self.changed.wait(gate).unwrap();
            }
            Ok(())
        }

        fn for_each_key(&self, f: &mut dyn FnMut(&[u8])) -> Result<(), BlobError> {
            self.inner.for_each_key(f)
        }
    }

    #[test]
    fn watermark_advances_only_after_a_successful_backend_call_returns() {
        let backend = Arc::new(ApplyThenBlockBlobs::default());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(2, 0)).unwrap();
        bind(
            writeback.reserve(vec![put(b"applied", b"value")]).unwrap(),
            90,
        )
        .publish()
        .unwrap();

        std::thread::scope(|scope| {
            let consumer_writeback = &writeback;
            let consumer = scope.spawn(move || consumer_writeback.process_front());
            let reached_backend = backend.wait_until_applied();
            let batch_count = reached_backend.then(|| backend.inner.batch_count());
            let (snapshot_tx, snapshot_rx) = mpsc::channel();
            let observer = if reached_backend {
                let observer_writeback = &writeback;
                Some(scope.spawn(move || {
                    snapshot_tx
                        .send((
                            observer_writeback.applied_watermark(),
                            observer_writeback.queue_len(),
                        ))
                        .unwrap();
                }))
            } else {
                None
            };
            let snapshot_while_blocked =
                reached_backend.then(|| snapshot_rx.recv_timeout(Duration::from_secs(1)));

            // Release before asserting or joining. If a regression held the
            // queue-state mutex across backend IO, the observer above times
            // out but can finish after this release instead of hanging.
            backend.release();
            let outcome = consumer.join().unwrap();
            if let Some(observer) = observer {
                observer.join().unwrap();
            }

            assert!(reached_backend, "backend did not reach its return gate");
            assert_eq!(batch_count, Some(1));
            let (watermark_while_blocked, queue_len_while_blocked) = snapshot_while_blocked
                .expect("backend was reached")
                .expect("watermark read blocked behind backend IO");
            assert_eq!(
                watermark_while_blocked,
                AppliedWatermark::default(),
                "backend side effects alone do not advance the watermark"
            );
            assert_eq!(queue_len_while_blocked, 1);
            assert!(matches!(outcome, ProcessOutcome::Advanced));
        });

        assert_eq!(
            writeback.applied_watermark(),
            AppliedWatermark::recovered(1, Some(mako_timestamp_of(90)))
        );
        assert_eq!(writeback.queue_len(), 0);
    }

    #[test]
    fn later_bind_does_not_wait_for_front_backend_io() {
        let backend = Arc::new(BlockingBlobs::default());
        let writeback = Writeback::new(Arc::clone(&backend), 0, config(4, 0)).unwrap();
        let mut first = bind(writeback.reserve(vec![put(b"a", b"one")]).unwrap(), 91);
        let second = writeback.reserve(vec![put(b"b", b"two")]).unwrap();
        first.publish().unwrap();

        std::thread::scope(|scope| {
            let consumer = scope.spawn(|| writeback.process_front());
            backend.wait_until_entered();

            let (bound_tx, bound_rx) = mpsc::channel();
            let binder = scope.spawn(move || {
                let mut second = bind(second, 92);
                bound_tx.send(second.sequence()).unwrap();
                second.publish().unwrap();
            });
            let bound = bound_rx.recv_timeout(Duration::from_secs(1));
            // Always release the consumer before asserting, so a regression
            // reports a failure instead of hanging the scoped-thread join.
            backend.release();

            assert!(
                bound.is_ok(),
                "post-validation bind was serialized behind backend IO"
            );
            assert_eq!(bound.unwrap().get(), 2);
            binder.join().unwrap();
            assert!(matches!(consumer.join().unwrap(), ProcessOutcome::Advanced));
        });

        assert_eq!(writeback.highest_acknowledged(), 2);
        assert_eq!(writeback.wait_applied().unwrap(), 2);
        assert_eq!(backend.inner.batch_count(), 2);
    }

    #[derive(Debug, Default)]
    struct PanicOnceBlobs {
        inner: MemBlobs,
        panic_next: AtomicBool,
    }

    impl PanicOnceBlobs {
        fn new() -> Self {
            Self {
                inner: MemBlobs::new(),
                panic_next: AtomicBool::new(true),
            }
        }
    }

    impl Blobs for PanicOnceBlobs {
        fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
            self.inner.get(key)
        }

        fn write_batch(&self, operations: &[BlobOp<'_>]) -> Result<(), BlobError> {
            if self.panic_next.swap(false, Ordering::SeqCst) {
                panic!("injected backend panic");
            }
            self.inner.write_batch(operations)
        }

        fn for_each_key(&self, f: &mut dyn FnMut(&[u8])) -> Result<(), BlobError> {
            self.inner.for_each_key(f)
        }
    }

    #[test]
    fn poisoned_consumer_retries_the_untouched_ready_record() {
        let writeback = Writeback::new(PanicOnceBlobs::new(), 0, config(2, 0)).unwrap();
        writeback
            .reserve(vec![put(b"a", b"one")])
            .unwrap()
            .bind(mako_timestamp_of(101))
            .unwrap()
            .publish()
            .unwrap();

        assert!(catch_unwind(AssertUnwindSafe(|| writeback.process_front())).is_err());
        assert_eq!(writeback.queue_len(), 1);
        assert_eq!(writeback.applied_watermark(), AppliedWatermark::default());
        assert!(matches!(
            writeback.process_front(),
            ProcessOutcome::Advanced
        ));
        assert_eq!(writeback.backend().inner.batch_count(), 1);
        assert_eq!(writeback.applied_sequence(), 1);
    }
}
