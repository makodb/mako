#![deny(unsafe_code)]

//! Transactional binary-key records over the safe Masstree directory.
//!
//! Masstree owns only the append-only `key -> RecordId` index. This crate owns
//! stable registry slots, immutable value/tombstone snapshots, OCC versions,
//! physical locks, and a scan-only physical-directory generation. Point
//! misses eagerly intern a tombstone, but every abstract mutation remains
//! deferred until the native Rust STO commit protocol installs it.
//!
//! With the `fixed-u64` feature, `FixedU64Table` provides a separate,
//! deliberately restricted all-live table whose atomic OCC word and `u64`
//! value occupy one 16-byte hot record. The general binary-value [`Table`]
//! remains unchanged and is still required for variable-width values and
//! scans.

mod direct_record;
mod record_prefetch;

#[cfg(test)]
mod history_tests;

#[cfg(feature = "fixed-u64")]
mod fixed_u64;

#[cfg(feature = "fixed-u64")]
pub use fixed_u64::{FixedU64Batch, FixedU64CreateError, FixedU64Mutation, FixedU64Table};

use arc_swap::{ArcSwapOption, Guard as ArcSwapGuard};
use std::{
    borrow::Borrow,
    fmt,
    ops::Deref,
    sync::{
        atomic::{AtomicBool, AtomicU64, AtomicU8, Ordering},
        Arc, OnceLock, RwLock, RwLockReadGuard, RwLockWriteGuard, TryLockError,
    },
};

#[cfg(test)]
use std::collections::BTreeMap;

#[cfg(not(test))]
use masstree::{
    BoundedRecordIdScanChunkRef as NativeBoundedRecordIdScanChunkRef,
    BoundedRecordIdScanResume as NativeBoundedRecordIdScanResume,
    InsertError as MasstreeInsertError, NativeStatus,
    PackedScanChunkRef as NativePackedScanChunkRef, PackedScanEntries as NativePackedScanEntries,
    PackedScanResume as NativePackedScanResume, PackedScanScratch as NativePackedScanScratch,
    PublicationDisposition, ReadScope as NativeReadScope, Runtime as MasstreeRuntime,
    ScanRequest as NativeScanRequest, Tree,
};
use masstree::{
    Error as MasstreeError, FixedInsertResult as DirectoryFixedInsertResult,
    InsertOutcome as DirectoryInsertOutcome, PointReadResult as DirectoryPointReadResult, RecordId,
    ScanStopReason, Worker,
};
pub use masstree::{KeyBound as ScanBound, ScanDirection};
use sto_core::{
    AccessError, AcquireContext, AcquireError, Active, AdapterFault, AdapterFaultKind,
    AdapterPhase, AtomicVersion, BorrowedInjectiveLockCommitCapability, BorrowedLockToken,
    CapacityError, CheckError, Conflict, DetachedVersionGuard, DirectBorrowedLockTarget,
    DirectCommitCapability, DirectInstallContext, DirectLockMut, DirectLockRef, DirectTokenLock,
    DirectValidationContext, DirectValidationItem, Entry, ErasedLockUse, ExecutionCheckContext,
    FinishContext, FinishDisposition, FinishItem, InstallContext, InstallItem, InvalidUse,
    ItemBatchControl, ItemBatchOutcome, LockClass, LockDisposition, LockIdentity, LockNamespaceId,
    LockRequest, NoPredicate, ObjectId, ObservationOrder, ObservationRef, OccCommitId, OccVersion,
    OpacityToken, OwnerId, PredicateContext, PreflightContext, PreflightFreeReadCapability,
    PreflightFreeValidationContext, PreflightItem, PrepareError, RegisteredResource,
    RegistrationError, ReleaseContext, ResourceClass, Runtime, RuntimeId,
    TerminalReadBatchCapability, TerminalReadOpen, TerminalReadReady, TerminalReadTransaction,
    Transaction, TransactionLock, TransactionalResource, UniqueItemKeyIndex, UniqueItemKeys,
    Unsupported, ValidationContext,
};
#[cfg(not(test))]
use sto_core::{FailurePhase, PoisonInfo};

const RECORD_RESOURCE_CLASS_VALUE: u32 = 1;
const DIRECTORY_RESOURCE_CLASS_VALUE: u32 = 2;
const SCAN_RESOURCE_CLASS_VALUE: u32 = 3;
const RECORD_LOCK_CLASS_VALUE: u32 = 2;
const PRIVATE_BOUNDED_FORWARD_VALUE_SCAN_MAX_RECORDS: usize = 300;

const SLOT_UNALLOCATED: u8 = 0;
const SLOT_RESERVED: u8 = 1;
const SLOT_READY: u8 = 2;
const SLOT_PUBLISHED: u8 = 3;
const SLOT_PROVEN_UNPUBLISHED: u8 = 4;
const SLOT_PUBLICATION_UNKNOWN: u8 = 5;

const TABLE_HEALTHY: u8 = 0;
const TABLE_POISONED: u8 = 1;
const TABLE_PUBLICATION_UNKNOWN: u8 = 2;

// The committed-state descriptor is independent of the OCC version word.
// Every mutation still occurs while that version is exclusively held; these
// descriptors only make the physical snapshot copy race-free without a read-side
// lock. Empty inline values and tombstones deliberately have distinct tags.
const RECORD_STATE_TOMBSTONE: u16 = 0;
const RECORD_STATE_SHARED: u16 = 1;
const RECORD_STATE_INLINE_BASE: u16 = 2;
const RECORD_STATE_STABLE_BASE: u16 = 0x0100;
const RECORD_STATE_UPDATING: u16 = u16::MAX - 1;
const RECORD_STATE_POISONED: u16 = u16::MAX;
const RECORD_STATE_DESCRIPTOR_SHIFT: u32 = 48;
const RECORD_STATE_TAIL_MASK: u64 = (1_u64 << RECORD_STATE_DESCRIPTOR_SHIFT) - 1;

// Registry slots are append-only and RecordIds are never reused. Group them
// into immutable, lazily published physical segments so a normal RecordId
// resolution needs no registry-wide reader lock and retains page locality.
// Physical storage segmentation is deliberately independent of the smaller
// shared lock targets: lock reference counts are distributed across the
// keyspace without fragmenting the read-hot record arena.
const REGISTRY_SEGMENT_SLOTS: usize = 1_024;
const RECORD_LOCK_SEGMENT_SLOTS: usize = 16;
const RECORD_LOCK_SEGMENTS_PER_REGISTRY_SEGMENT: usize =
    REGISTRY_SEGMENT_SLOTS / RECORD_LOCK_SEGMENT_SLOTS;
const REGISTRY_ENTRY_SLOT_BYTES: usize = 64;

#[cfg(not(test))]
type DirectoryScanStorage = NativePackedScanScratch;
#[cfg(test)]
type DirectoryScanStorage = ();

// Thirty-eight bytes covers the measured TPC-C new-order, stock, order-line,
// and secondary-index values while keeping Value and RecordState in the
// compiler's 40-byte layout class. Five independently atomic payload words
// provide a race-free committed copy under the OCC version sandwich.
const INLINE_VALUE_CAPACITY: usize = 38;
const INLINE_VALUE_WORDS: usize = INLINE_VALUE_CAPACITY.div_ceil(std::mem::size_of::<u64>());
const INLINE_VALUE_HEAD_WORDS: usize = INLINE_VALUE_WORDS - 1;
const INLINE_VALUE_TAIL_CAPACITY: usize =
    INLINE_VALUE_CAPACITY - INLINE_VALUE_HEAD_WORDS * std::mem::size_of::<u64>();

/// Maximum value length stored in an opted-in record's stable atomic cell.
///
/// The first `INLINE_VALUE_CAPACITY` bytes reuse the compact record's
/// existing atomic words. The adjacent cell supplies the remaining words.
/// Larger values retain the ordinary immutable ArcSwap representation.
pub const STABLE_ATOMIC_VALUE_CAPACITY: usize = 160;
const STABLE_ATOMIC_VALUE_SUFFIX_CAPACITY: usize =
    STABLE_ATOMIC_VALUE_CAPACITY - INLINE_VALUE_CAPACITY;
const STABLE_ATOMIC_VALUE_SUFFIX_WORDS: usize =
    STABLE_ATOMIC_VALUE_SUFFIX_CAPACITY.div_ceil(std::mem::size_of::<u64>());

// Values beyond the record-inline tier still need a thin `Arc` for ArcSwap.
// Keeping this common medium tier in that same allocation removes the second
// allocation and dependent pointer load without enlarging the record itself.
const SHARED_INLINE_VALUE_CAPACITY: usize = 128;

/// An immutable binary value snapshot returned by point operations.
///
/// Short values are stored inline and clone without allocation or shared
/// reference-count traffic. Larger values use immutable shared storage. Empty
/// and arbitrary binary values (including embedded NUL bytes) are preserved.
pub struct Value {
    repr: ValueRepr,
}

enum ValueRepr {
    Inline {
        len: u8,
        bytes: [u8; INLINE_VALUE_CAPACITY],
    },
    Shared(Arc<SharedValue>),
    // Private transaction-owned storage for an opted-in bounded publication.
    // Cloning this variant copies the bytes, so no mutable committed cell can
    // ever alias an owned Value snapshot.
    Staged(Box<[u8]>),
    // Private fast-lane storage whose caller guarantees that these immutable
    // bytes outlive the active transaction. Any clone becomes owned, so this
    // representation cannot escape through a safe point-operation result.
    BorrowedStaged(&'static [u8]),
}

/// Sized, immutable ownership for bytes kept behind the thin `Arc` required
/// by `ArcSwapOption`.
///
/// Common medium values live in the `Arc` allocation itself. Larger values
/// retain an owned `Vec` fallback, avoiding unsafe thin-DST machinery and
/// preserving arbitrary value lengths. Constructing either representation
/// from a borrowed slice copies the bytes once, as any owned conversion from
/// a borrowed slice must.
// Keep the hot Medium tag, length, and leading bytes adjacent. In particular,
// do not let the compiler place an implicit discriminant beyond the 128-byte
// payload and recreate the second-cache-line dependency this tier removes.
#[derive(Debug, Eq, PartialEq)]
#[repr(u8)]
enum SharedValue {
    Medium {
        len: u8,
        bytes: [u8; SHARED_INLINE_VALUE_CAPACITY],
    },
    Heap(Vec<u8>),
}

impl SharedValue {
    #[inline]
    fn from_slice(bytes: &[u8]) -> Arc<Self> {
        Arc::new(Self::from_borrowed(bytes))
    }

    #[inline]
    fn from_vec(bytes: Vec<u8>) -> Arc<Self> {
        if bytes.len() <= SHARED_INLINE_VALUE_CAPACITY {
            Arc::new(Self::from_borrowed(&bytes))
        } else {
            Arc::new(Self::Heap(bytes))
        }
    }

    #[inline]
    fn from_borrowed(bytes: &[u8]) -> Self {
        if bytes.len() <= SHARED_INLINE_VALUE_CAPACITY {
            let mut inline = [0_u8; SHARED_INLINE_VALUE_CAPACITY];
            inline[..bytes.len()].copy_from_slice(bytes);
            Self::Medium {
                len: bytes.len() as u8,
                bytes: inline,
            }
        } else {
            Self::Heap(bytes.to_vec())
        }
    }

    #[inline(always)]
    fn as_bytes(&self) -> &[u8] {
        match self {
            Self::Medium { len, bytes } => &bytes[..usize::from(*len)],
            Self::Heap(bytes) => bytes.as_slice(),
        }
    }
}

impl Value {
    #[inline]
    fn from_slice(bytes: &[u8]) -> Self {
        if bytes.len() <= INLINE_VALUE_CAPACITY {
            let mut inline = [0_u8; INLINE_VALUE_CAPACITY];
            inline[..bytes.len()].copy_from_slice(bytes);
            Self {
                repr: ValueRepr::Inline {
                    len: bytes.len() as u8,
                    bytes: inline,
                },
            }
        } else {
            Self {
                repr: ValueRepr::Shared(SharedValue::from_slice(bytes)),
            }
        }
    }

    #[inline]
    fn from_staging_slice(bytes: &[u8], bounded_atomic_values: bool) -> Self {
        if bounded_atomic_values
            && bytes.len() > INLINE_VALUE_CAPACITY
            && bytes.len() <= STABLE_ATOMIC_VALUE_CAPACITY
        {
            Self {
                repr: ValueRepr::Staged(bytes.into()),
            }
        } else {
            Self::from_slice(bytes)
        }
    }

    /// Creates a write-only transaction value that borrows `bytes` until the
    /// active transaction finishes.
    ///
    /// # Safety
    ///
    /// `bytes` must remain readable and immutable until the transaction that
    /// receives this value commits or aborts. The value must qualify for
    /// stable bounded publication, so commit never retains the borrowed
    /// pointer in committed storage.
    #[allow(
        unsafe_code,
        reason = "the private transaction fast lane carries an external lifetime contract"
    )]
    #[inline(always)]
    unsafe fn from_borrowed_staging_slice(bytes: &[u8]) -> Self {
        debug_assert!(bytes.len() > INLINE_VALUE_CAPACITY);
        debug_assert!(bytes.len() <= STABLE_ATOMIC_VALUE_CAPACITY);
        // SAFETY: The caller guarantees the real borrow outlives the active
        // transaction. Giving the slice a static type keeps pointer provenance
        // intact; this private variant never escapes through a safe clone.
        let bytes = unsafe { std::mem::transmute::<&[u8], &'static [u8]>(bytes) };
        Self {
            repr: ValueRepr::BorrowedStaged(bytes),
        }
    }
}

impl Clone for Value {
    #[inline]
    fn clone(&self) -> Self {
        let repr = match &self.repr {
            ValueRepr::Inline { len, bytes } => ValueRepr::Inline {
                len: *len,
                bytes: *bytes,
            },
            ValueRepr::Shared(bytes) => ValueRepr::Shared(Arc::clone(bytes)),
            ValueRepr::Staged(bytes) => ValueRepr::Staged(bytes.clone()),
            ValueRepr::BorrowedStaged(_) => return Self::from_slice(self.as_ref()),
        };
        Self { repr }
    }
}

/// A borrowed write input that becomes an owned [`Value`] only after the
/// transaction item has been resolved and its prior state has been checked.
/// Keeping this capture small avoids carrying the larger `Value` enum through
/// the directory and core item-lookup path.
#[derive(Clone, Copy)]
struct StagingValue<'value> {
    bytes: &'value [u8],
    bounded_atomic_values: bool,
}

/// The same borrowed slice shape as [`StagingValue`], with the stronger
/// caller-owned lifetime needed by the private zero-allocation write lane.
#[derive(Clone, Copy)]
struct BorrowedStagingValue<'value> {
    bytes: &'value [u8],
    bounded_atomic_values: bool,
}

impl From<StagingValue<'_>> for Value {
    #[inline(always)]
    fn from(value: StagingValue<'_>) -> Self {
        Self::from_staging_slice(value.bytes, value.bounded_atomic_values)
    }
}

impl From<BorrowedStagingValue<'_>> for Value {
    #[allow(
        unsafe_code,
        reason = "only unsafe Table entry points can construct this private lifetime proof"
    )]
    #[inline(always)]
    fn from(value: BorrowedStagingValue<'_>) -> Self {
        if value.bounded_atomic_values
            && value.bytes.len() > INLINE_VALUE_CAPACITY
            && value.bytes.len() <= STABLE_ATOMIC_VALUE_CAPACITY
        {
            // SAFETY: Construction of `BorrowedStagingValue` is confined to
            // Table methods whose caller promises the transaction lifetime.
            unsafe { Self::from_borrowed_staging_slice(value.bytes) }
        } else {
            Self::from_staging_slice(value.bytes, value.bounded_atomic_values)
        }
    }
}

impl AsRef<[u8]> for Value {
    #[inline]
    fn as_ref(&self) -> &[u8] {
        match &self.repr {
            ValueRepr::Inline { len, bytes } => &bytes[..usize::from(*len)],
            ValueRepr::Shared(bytes) => bytes.as_bytes(),
            ValueRepr::Staged(bytes) => bytes,
            ValueRepr::BorrowedStaged(bytes) => bytes,
        }
    }
}

impl Borrow<[u8]> for Value {
    #[inline]
    fn borrow(&self) -> &[u8] {
        self.as_ref()
    }
}

impl Deref for Value {
    type Target = [u8];

    #[inline]
    fn deref(&self) -> &Self::Target {
        self.as_ref()
    }
}

impl From<&[u8]> for Value {
    #[inline]
    fn from(bytes: &[u8]) -> Self {
        Self::from_slice(bytes)
    }
}

impl<const N: usize> From<&[u8; N]> for Value {
    #[inline]
    fn from(bytes: &[u8; N]) -> Self {
        Self::from_slice(bytes)
    }
}

impl From<Vec<u8>> for Value {
    #[inline]
    fn from(bytes: Vec<u8>) -> Self {
        if bytes.len() <= INLINE_VALUE_CAPACITY {
            Self::from_slice(&bytes)
        } else {
            Self {
                repr: ValueRepr::Shared(SharedValue::from_vec(bytes)),
            }
        }
    }
}

impl From<Box<[u8]>> for Value {
    #[inline]
    fn from(bytes: Box<[u8]>) -> Self {
        if bytes.len() <= INLINE_VALUE_CAPACITY {
            Self::from_slice(&bytes)
        } else {
            Self {
                repr: ValueRepr::Shared(SharedValue::from_vec(bytes.into_vec())),
            }
        }
    }
}

impl From<Arc<[u8]>> for Value {
    #[inline]
    fn from(bytes: Arc<[u8]>) -> Self {
        Self::from_slice(&bytes)
    }
}

impl Default for Value {
    fn default() -> Self {
        Self::from_slice(&[])
    }
}

impl fmt::Debug for Value {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.debug_list().entries(self.as_ref()).finish()
    }
}

impl PartialEq for Value {
    #[inline]
    fn eq(&self, other: &Self) -> bool {
        self.as_ref() == other.as_ref()
    }
}

impl Eq for Value {}

/// One deferred action selected while processing a fixed point batch.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum PointMutation {
    /// Retains the value observed at this position without staging a write.
    Keep,
    /// Stages an unconditional live replacement.
    Put(Value),
    /// Stages a tombstone when the transaction-local record is live.
    Remove,
}

/// Reusable storage for a fixed-width transactional point-read batch.
///
/// Construct this once with the largest expected batch and reuse it across
/// [`PointSession::get_fixed`], [`PointSession::visit_fixed`],
/// [`PointSession::modify_fixed`], or [`PointSession::modify_fixed_visit`]
/// calls. Successful owning calls replace the prior results without allocating
/// while the requested length is within the retained capacity. Visitor calls
/// retain no results. Failed nonempty calls leave the result slice empty and
/// doom the active transaction exactly like a failed scalar point access.
#[derive(Debug, Default)]
pub struct PointReadBatch {
    directory_results: Vec<DirectoryPointReadResult>,
    record_ids: Vec<Option<RecordId>>,
    item_keys: Vec<TableKey>,
    unique_key_index: UniqueItemKeyIndex,
    alias_order: Vec<usize>,
    values: Vec<Option<Value>>,
    fixed_inserts: FixedInsertScratch,
}

#[derive(Debug, Default)]
struct FixedInsertScratch {
    keys: Vec<[u8; 16]>,
    positions: Vec<usize>,
    candidates: Vec<Candidate>,
    directory_tokens: Vec<RecordId>,
    results: Vec<DirectoryFixedInsertResult>,
}

impl FixedInsertScratch {
    const fn new() -> Self {
        Self {
            keys: Vec::new(),
            positions: Vec::new(),
            candidates: Vec::new(),
            directory_tokens: Vec::new(),
            results: Vec::new(),
        }
    }

    fn with_capacity(capacity: usize) -> Self {
        Self {
            keys: Vec::with_capacity(capacity),
            positions: Vec::with_capacity(capacity),
            candidates: Vec::with_capacity(capacity),
            directory_tokens: Vec::with_capacity(capacity),
            results: Vec::with_capacity(capacity),
        }
    }

    fn clear(&mut self) {
        self.keys.clear();
        self.positions.clear();
        self.candidates.clear();
        self.directory_tokens.clear();
        self.results.clear();
    }

    fn capacity(&self) -> usize {
        self.keys
            .capacity()
            .min(self.positions.capacity())
            .min(self.candidates.capacity())
            .min(self.directory_tokens.capacity())
            .min(self.results.capacity())
    }

    fn prepare(&mut self, capacity: usize) -> Result<(), AccessError> {
        self.clear();
        self.keys
            .try_reserve_exact(capacity)
            .map_err(|_| CapacityError::BufferLimit)?;
        self.positions
            .try_reserve_exact(capacity)
            .map_err(|_| CapacityError::BufferLimit)?;
        self.candidates
            .try_reserve_exact(capacity)
            .map_err(|_| CapacityError::BufferLimit)?;
        self.directory_tokens
            .try_reserve_exact(capacity)
            .map_err(|_| CapacityError::BufferLimit)?;
        self.results
            .try_reserve_exact(capacity)
            .map_err(|_| CapacityError::BufferLimit)?;
        Ok(())
    }
}

/// Result of attempting the all-hit terminal fixed-read protocol.
///
/// A directory miss is discovered before any visitor callback or STO item is
/// created. In that case the restricted transaction is definitely aborted and
/// the caller may begin a fresh general transaction for ordinary miss handling.
#[derive(Debug)]
#[must_use = "a ready terminal transaction must be committed or aborted"]
#[allow(
    clippy::large_enum_variant,
    reason = "boxing the consumed transaction would allocate on the read-only hot path"
)]
pub enum TerminalReadVisitOutcome<'worker> {
    /// Every input was visited and the complete batch is ready to certify.
    Ready {
        transaction: TerminalReadTransaction<'worker, TerminalReadReady>,
        visited: usize,
    },
    /// At least one key was absent, so the consumed terminal transaction was
    /// aborted without invoking the visitor.
    RetryOrdinary,
}

impl PointReadBatch {
    /// Creates empty scratch storage.
    pub const fn new() -> Self {
        Self {
            directory_results: Vec::new(),
            record_ids: Vec::new(),
            item_keys: Vec::new(),
            unique_key_index: UniqueItemKeyIndex::new(),
            alias_order: Vec::new(),
            values: Vec::new(),
            fixed_inserts: FixedInsertScratch::new(),
        }
    }

    /// Creates empty scratch storage sized for at least `capacity` reads.
    pub fn with_capacity(capacity: usize) -> Self {
        Self {
            directory_results: Vec::with_capacity(capacity),
            record_ids: Vec::with_capacity(capacity),
            item_keys: Vec::with_capacity(capacity),
            unique_key_index: if capacity <= SMALL_UNIQUE_KEY_BATCH {
                UniqueItemKeyIndex::new()
            } else {
                UniqueItemKeyIndex::with_capacity(capacity)
            },
            alias_order: Vec::with_capacity(capacity),
            values: Vec::with_capacity(capacity),
            fixed_inserts: FixedInsertScratch::with_capacity(capacity),
        }
    }

    /// Returns the number of snapshots produced by the last successful call.
    pub fn len(&self) -> usize {
        self.values.len()
    }

    /// Returns whether the last successful call produced no snapshots.
    pub fn is_empty(&self) -> bool {
        self.values.is_empty()
    }

    /// Returns the allocation-free batch length currently retained by every
    /// internal scratch/result buffer.
    pub fn capacity(&self) -> usize {
        self.directory_results
            .capacity()
            .min(self.record_ids.capacity())
            .min(self.item_keys.capacity())
            // Pairwise equality needs no hash scratch for this prefix.
            .min(self.unique_key_index.capacity().max(SMALL_UNIQUE_KEY_BATCH))
            .min(self.alias_order.capacity())
            .min(self.values.capacity())
            .min(self.fixed_inserts.capacity())
    }

    /// Returns the snapshots from the last successful call in input order.
    pub fn results(&self) -> &[Option<Value>] {
        &self.values
    }

    /// Drops prior snapshots while retaining all scratch allocations.
    pub fn clear(&mut self) {
        self.directory_results.clear();
        self.record_ids.clear();
        self.item_keys.clear();
        self.alias_order.clear();
        self.values.clear();
        self.fixed_inserts.clear();
    }

    fn prepare_read<const CAPTURE_VALUES: bool>(
        &mut self,
        length: usize,
    ) -> Result<(), AccessError> {
        self.clear();
        self.directory_results
            .try_reserve_exact(length)
            .map_err(|_| CapacityError::BufferLimit)?;
        self.record_ids
            .try_reserve_exact(length)
            .map_err(|_| CapacityError::BufferLimit)?;
        self.item_keys
            .try_reserve_exact(length)
            .map_err(|_| CapacityError::BufferLimit)?;
        if length > SMALL_UNIQUE_KEY_BATCH {
            self.unique_key_index
                .try_reserve_for_len(length)
                .map_err(|_| CapacityError::BufferLimit)?;
        }
        self.alias_order
            .try_reserve_exact(length)
            .map_err(|_| CapacityError::BufferLimit)?;
        if CAPTURE_VALUES {
            self.values
                .try_reserve_exact(length)
                .map_err(|_| CapacityError::BufferLimit)?;
        }
        Ok(())
    }

    fn prepare_modify<const CAPTURE_VALUES: bool>(
        &mut self,
        length: usize,
    ) -> Result<(), AccessError> {
        self.prepare_read::<CAPTURE_VALUES>(length)
    }

    #[cfg(test)]
    fn push_record_id(&mut self, record_id: Option<RecordId>) {
        self.record_ids.push(record_id);
    }
}

/// Physical storage strategy for one table's append-only record registry.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum RegistryLayout {
    /// Allocate fixed-size segments only as consumed IDs first reach them.
    ///
    /// This minimizes startup work and commits record memory incrementally,
    /// but resolving an ID depends on its segment's published `OnceLock`.
    #[default]
    LazySegmented,
    /// Allocate the entire bounded record arena and all lock targets when the
    /// table is created, subject to an explicit allocation budget.
    ///
    /// This makes lookup one direct base-plus-index operation. In exchange,
    /// construction initializes `max_consumed_record_ids` stable slots
    /// (currently 64 bytes each) and one shared lock target per 16 slots, even
    /// when the table never consumes that capacity.
    EagerContiguous {
        /// Maximum accounted bytes for the slot arena, lock-target pointer
        /// array, and Arc-owned lock targets. Construction fails rather than
        /// falling back when the configured ID bound exceeds this budget.
        max_bytes: usize,
    },
}

/// Bounded append-only registry limits for one table.
///
/// Retained-record and key-byte limits account candidates that may be
/// directory-reachable. Consumed IDs bound the stable arena itself, including
/// in-place tombstone slots for candidates proved unpublished.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TableConfig {
    max_retained_records: u64,
    max_retained_key_bytes: u64,
    max_consumed_record_ids: u64,
    registry_layout: RegistryLayout,
    unique_lock_requests: bool,
    scan_chunk_records: usize,
    scan_initial_key_arena_bytes: usize,
    scan_max_key_arena_bytes: usize,
    max_scan_chunks: usize,
    max_scan_physical_records: usize,
    trusted_scan_value_generation: bool,
    bounded_atomic_values: bool,
}

impl TableConfig {
    /// Conservative, explicitly bounded defaults.
    pub const fn new() -> Self {
        Self {
            max_retained_records: 1_000_000,
            max_retained_key_bytes: 1 << 30,
            max_consumed_record_ids: 4_000_000,
            registry_layout: RegistryLayout::LazySegmented,
            unique_lock_requests: false,
            scan_chunk_records: 128,
            scan_initial_key_arena_bytes: 16 * 1024,
            scan_max_key_arena_bytes: 64 * 1024,
            max_scan_chunks: 4_096,
            max_scan_physical_records: 1_000_000,
            trusted_scan_value_generation: false,
            bounded_atomic_values: false,
        }
    }

    pub const fn with_max_retained_records(mut self, maximum: u64) -> Self {
        self.max_retained_records = maximum;
        self
    }

    pub const fn with_max_retained_key_bytes(mut self, maximum: u64) -> Self {
        self.max_retained_key_bytes = maximum;
        self
    }

    pub const fn with_max_consumed_record_ids(mut self, maximum: u64) -> Self {
        self.max_consumed_record_ids = maximum;
        self
    }

    /// Selects when the bounded stable record arena is physically allocated.
    ///
    /// [`RegistryLayout::EagerContiguous`] trades proportional table-creation
    /// time and resident memory for direct RecordId indexing. Its explicit
    /// byte budget also guards against an accidental consumed-ID limit change.
    /// The default is [`RegistryLayout::LazySegmented`].
    pub const fn with_registry_layout(mut self, layout: RegistryLayout) -> Self {
        self.registry_layout = layout;
        self
    }

    /// Selects transaction-wide unique, request-order physical lock planning.
    ///
    /// This is an integration contract, not a table-local hint. When enabled,
    /// every lock-emitting adapter that may share a transaction with this table
    /// must also use [`PreflightContext::require_unique_lock`], and every
    /// emitted [`LockIdentity`] must be distinct. Mixing this table with a
    /// canonical lock request, or emitting the same physical identity twice,
    /// makes preflight fail closed. The default is `false`.
    pub const fn with_unique_lock_requests(mut self, enabled: bool) -> Self {
        self.unique_lock_requests = enabled;
        self
    }

    pub const fn with_scan_chunk_records(mut self, maximum: usize) -> Self {
        self.scan_chunk_records = maximum;
        self
    }

    pub const fn with_scan_initial_key_arena_bytes(mut self, initial: usize) -> Self {
        self.scan_initial_key_arena_bytes = initial;
        self
    }

    pub const fn with_scan_max_key_arena_bytes(mut self, maximum: usize) -> Self {
        self.scan_max_key_arena_bytes = maximum;
        self
    }

    pub const fn with_max_scan_chunks(mut self, maximum: usize) -> Self {
        self.max_scan_chunks = maximum;
        self
    }

    pub const fn with_max_scan_physical_records(mut self, maximum: usize) -> Self {
        self.max_scan_physical_records = maximum;
        self
    }

    /// Enables the trusted direct-table scan generation protocol.
    ///
    /// A transaction that commits one or more record publications advances the
    /// table-local atomic generation once. Directory publications advance it
    /// separately during transaction execution. Leave this disabled for tables
    /// that never use the trusted range-scan API so their point writes pay no
    /// global counter cost.
    pub const fn with_trusted_scan_value_generation(mut self, enabled: bool) -> Self {
        self.trusted_scan_value_generation = enabled;
        self
    }

    /// Stores values through 160 bytes in stable per-record atomic words.
    ///
    /// This preserves the compact 64-byte record layout for tables that leave
    /// the option disabled. Enabled tables use 192-byte registry entries and
    /// retain the normal ArcSwap fallback for larger values.
    pub const fn with_bounded_atomic_values(mut self, enabled: bool) -> Self {
        self.bounded_atomic_values = enabled;
        self
    }

    pub const fn max_retained_records(self) -> u64 {
        self.max_retained_records
    }

    pub const fn max_retained_key_bytes(self) -> u64 {
        self.max_retained_key_bytes
    }

    pub const fn max_consumed_record_ids(self) -> u64 {
        self.max_consumed_record_ids
    }

    pub const fn registry_layout(self) -> RegistryLayout {
        self.registry_layout
    }

    /// Returns whether this table claims transaction-wide unique lock requests.
    pub const fn unique_lock_requests(self) -> bool {
        self.unique_lock_requests
    }

    pub const fn scan_chunk_records(self) -> usize {
        self.scan_chunk_records
    }

    pub const fn scan_initial_key_arena_bytes(self) -> usize {
        self.scan_initial_key_arena_bytes
    }

    pub const fn scan_max_key_arena_bytes(self) -> usize {
        self.scan_max_key_arena_bytes
    }

    pub const fn max_scan_chunks(self) -> usize {
        self.max_scan_chunks
    }

    pub const fn max_scan_physical_records(self) -> usize {
        self.max_scan_physical_records
    }

    pub const fn trusted_scan_value_generation(self) -> bool {
        self.trusted_scan_value_generation
    }

    pub const fn bounded_atomic_values(self) -> bool {
        self.bounded_atomic_values
    }
}

impl Default for TableConfig {
    fn default() -> Self {
        Self::new()
    }
}

/// Observable table-local quarantine state.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TableHealth {
    Healthy,
    Poisoned,
    PublicationUnknown,
}

/// Abstract result of conditional insertion.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum InsertOutcome {
    Inserted,
    AlreadyPresent(Value),
}

/// One bounded transactional range request.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ScanRequest<'key> {
    direction: ScanDirection,
    lower: ScanBound<'key>,
    upper: ScanBound<'key>,
    limit: usize,
}

impl<'key> ScanRequest<'key> {
    pub const fn new(direction: ScanDirection, limit: usize) -> Self {
        Self {
            direction,
            lower: ScanBound::Unbounded,
            upper: ScanBound::Unbounded,
            limit,
        }
    }

    pub const fn with_lower(mut self, lower: ScanBound<'key>) -> Self {
        self.lower = lower;
        self
    }

    pub const fn with_upper(mut self, upper: ScanBound<'key>) -> Self {
        self.upper = upper;
        self
    }

    pub const fn direction(self) -> ScanDirection {
        self.direction
    }

    pub const fn lower(self) -> ScanBound<'key> {
        self.lower
    }

    pub const fn upper(self) -> ScanBound<'key> {
        self.upper
    }

    pub const fn limit(self) -> usize {
        self.limit
    }
}

/// One owned live row returned by a transactional scan.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ScanRecord {
    key: Arc<[u8]>,
    value: Value,
    resolved: ResolvedRecord,
}

/// One borrowed live row delivered by a transactional scan visitor.
///
/// Both byte slices are valid only for the callback invocation. The resolved
/// token remains a normal owned scalar and may be retained for later point
/// operations on this table.
#[derive(Clone, Copy, Debug)]
pub struct ScanRecordRef<'row> {
    key: &'row [u8],
    value: &'row Value,
    resolved: ResolvedRecord,
}

/// One live row whose value bytes are borrowed directly from committed state.
///
/// Inline bytes live in operation-local storage, while large values remain
/// pinned by an ArcSwap read guard. Both borrows end with the visitor
/// invocation. The resolved token is an ordinary owned scalar and may be
/// retained for later point operations on this table.
#[derive(Clone, Copy, Debug)]
pub struct ScanBytesRef<'row> {
    key: &'row [u8],
    value: &'row [u8],
    resolved: ResolvedRecord,
}

impl<'row> ScanBytesRef<'row> {
    pub const fn key(self) -> &'row [u8] {
        self.key
    }

    pub const fn value(self) -> &'row [u8] {
        self.value
    }

    pub const fn resolved(self) -> ResolvedRecord {
        self.resolved
    }
}

impl<'row> ScanRecordRef<'row> {
    pub const fn key(self) -> &'row [u8] {
        self.key
    }

    pub fn value(self) -> &'row [u8] {
        self.value.as_ref()
    }

    pub const fn value_snapshot(self) -> &'row Value {
        self.value
    }

    pub const fn resolved(self) -> ResolvedRecord {
        self.resolved
    }
}

/// Flow control returned by a transactional scan visitor.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum ScanControl {
    Continue,
    Stop,
}

/// Successful completion metadata for a transactional scan visitor.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct ScanVisitOutcome {
    visited: usize,
    stopped: bool,
}

impl ScanVisitOutcome {
    pub const fn visited(self) -> usize {
        self.visited
    }

    pub const fn stopped(self) -> bool {
        self.stopped
    }
}

/// Caller-owned reusable storage for transactional directory scans.
///
/// Native entry descriptors, key-arena bytes, and exact uniqueness proof
/// storage grow on demand and remain available for later scans. A scratch
/// value must not be shared by concurrent or reentrant scans.
#[derive(Debug, Default)]
pub struct ScanScratch {
    native: DirectoryScanStorage,
    item_keys: Vec<TableKey>,
    unique_order: Vec<usize>,
}

impl ScanRecord {
    pub fn key(&self) -> &[u8] {
        &self.key
    }

    pub fn value(&self) -> &[u8] {
        &self.value
    }

    pub fn value_snapshot(&self) -> &Value {
        &self.value
    }

    /// Returns the stable record identity resolved by this scan row.
    ///
    /// Reusing the token with this table skips only the append-only directory
    /// lookup. Resolved operations still perform ordinary STO observation,
    /// record observation, validation, and commit processing.
    pub const fn resolved(&self) -> ResolvedRecord {
        self.resolved
    }
}

/// Current bounded registry accounting.
///
/// `retained_*` counts directory-reachable candidates and records. Every
/// consumed ID also owns one bounded stable arena slot until table destruction,
/// even after a proven-unpublished candidate releases retained quota.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TableUsage {
    retained_records: u64,
    retained_key_bytes: u64,
    consumed_record_ids: u64,
}

impl TableUsage {
    pub const fn retained_records(self) -> u64 {
        self.retained_records
    }

    pub const fn retained_key_bytes(self) -> u64 {
        self.retained_key_bytes
    }

    pub const fn consumed_record_ids(self) -> u64 {
        self.consumed_record_ids
    }
}

/// Failure to create a general table and its private native directory.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TableCreateError {
    /// Creating the fresh Masstree directory failed.
    Directory(MasstreeError),
    /// Registering or allocating the STO-side table failed.
    Registration(RegistrationError),
}

impl fmt::Display for TableCreateError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Directory(error) => write!(formatter, "create private directory: {error}"),
            Self::Registration(error) => write!(formatter, "register table: {error}"),
        }
    }
}

impl std::error::Error for TableCreateError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Directory(error) => Some(error),
            Self::Registration(error) => Some(error),
        }
    }
}

impl From<MasstreeError> for TableCreateError {
    fn from(error: MasstreeError) -> Self {
        Self::Directory(error)
    }
}

impl From<RegistrationError> for TableCreateError {
    fn from(error: RegistrationError) -> Self {
        Self::Registration(error)
    }
}

/// A cloneable transactional table backed by one safe Masstree tree.
///
/// The supplied native worker remains an operation-scoped capability. It is
/// never stored in the table, an STO item, or an adapter callback.
pub struct Table {
    record_resource: RegisteredResource<TableAdapter>,
    directory_resource: RegisteredResource<TableAdapter>,
    scan_resource: RegisteredResource<TableAdapter>,
}

/// An opaque stable record-identity capability minted by a [`Table`] lookup.
///
/// It contains no borrow and exposes no address operations. A token may be
/// reused only with the table that minted it; resolved operations validate
/// both runtime and object identity before interpreting its private scalar.
/// The ordinary externally-supplied-tree constructor uses append-only record
/// IDs. An opt-in private-tree table may internally encode a stable record
/// address, whose lifetime remains owned by that table.
#[derive(Clone, Copy, Debug, Eq, PartialEq, Hash)]
pub struct ResolvedRecord {
    runtime_id: RuntimeId,
    object_id: ObjectId,
    record_id: RecordId,
}

/// Dense shared storage for stable record identities from one table.
///
/// This cache is intended for integrations that have a bounded, direct
/// logical-key domain such as a TPC-C item ID. Each slot starts empty and may
/// be bound exactly once. It stores no value or OCC state: every use of a
/// returned [`ResolvedRecord`] still performs the normal transactional record
/// observation and validation.
///
/// The cache retains a table handle so a published record identity cannot
/// outlive the registry that minted it. Callers must use the same logical
/// slot for the same table key. A conflicting second identity is rejected and
/// never overwrites the first.
#[doc(hidden)]
pub struct DenseResolvedCache {
    table: Table,
    record_ids: Box<[AtomicU64]>,
}

impl DenseResolvedCache {
    /// Returns the number of exact logical-key slots.
    pub fn len(&self) -> usize {
        self.record_ids.len()
    }

    /// Returns whether this cache has no logical-key slots.
    pub fn is_empty(&self) -> bool {
        self.record_ids.is_empty()
    }

    /// Loads one cached identity.
    ///
    /// An index outside the configured domain fails with
    /// [`CapacityError::KeyLimit`].
    #[inline(always)]
    pub fn get(&self, index: usize) -> Result<Option<ResolvedRecord>, AccessError> {
        let slot = self.record_ids.get(index).ok_or(CapacityError::KeyLimit)?;
        let raw = slot.load(Ordering::Acquire);
        Ok(RecordId::new(raw).map(|record_id| self.table.mint_resolved(record_id)))
    }

    /// Publishes one exact identity without replacing an existing binding.
    ///
    /// Repeating the same binding is idempotent. A token from another runtime
    /// or table is rejected before touching the slot. A different token for an
    /// occupied slot fails with [`InvalidUse::IllegalItemState`] and leaves the
    /// original identity intact.
    #[inline]
    pub fn remember(&self, index: usize, resolved: ResolvedRecord) -> Result<(), AccessError> {
        let record_id = self.table.validate_resolved(resolved)?;
        let slot = self.record_ids.get(index).ok_or(CapacityError::KeyLimit)?;
        match slot.compare_exchange(0, record_id.get(), Ordering::Release, Ordering::Acquire) {
            Ok(_) => Ok(()),
            Err(existing) if existing == record_id.get() => Ok(()),
            Err(_) => Err(InvalidUse::IllegalItemState.into()),
        }
    }
}

impl fmt::Debug for DenseResolvedCache {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("DenseResolvedCache")
            .field("runtime_id", &self.table.record_resource.runtime_id())
            .field("object_id", &self.table.object_id())
            .field("len", &self.len())
            .finish_non_exhaustive()
    }
}

/// Result of copying one transactional point value into caller-owned storage.
///
/// Missing values and insufficient output capacity never modify the supplied
/// buffer. A copied value writes exactly `len` bytes and leaves the remainder
/// of the buffer unchanged.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum ValueCopyOutcome {
    Miss,
    Copied { len: usize },
    BufferTooSmall { required: usize },
}

/// Transaction-bound point operations with amortized native read admission.
///
/// A scalar lookup opens a scope and retains it across subsequent scalar hits.
/// A fixed-width batch uses one self-contained native lookup when no scope is
/// active, or reuses an already-open scalar scope. A directory miss closes an
/// active scope before Masstree insertion is attempted; later scalar point
/// operations may open another scope. [`Self::scan`] also closes it before
/// entering the native scan boundary. Explicit [`Self::close`] is useful
/// before worker quiescence or native lifecycle operations, while Drop
/// provides cleanup on errors and unwinding.
///
/// The mutable transaction borrow ensures commit and abort cannot begin while
/// this session is reachable. The caller must likewise avoid using another
/// transaction or tree with the same shared native worker until the session is
/// closed or dropped. Because an open session may retain native RCU protection,
/// keep it synchronous and short. Caller-supplied visitor callbacks must not
/// deliberately block, perform I/O, suspend at `.await`, or re-enter native
/// work. Batch scratch storage may grow and is not claimed to be allocation-free.
#[must_use = "a point session must remain alive while its batched lookups run"]
pub struct PointSession<'session, 'context> {
    table: &'session Table,
    transaction: &'session mut Transaction<'context, Active>,
    worker: &'session Worker,
    #[cfg(not(test))]
    read_scope: Option<NativeReadScope<'session, 'session>>,
}

impl fmt::Debug for PointSession<'_, '_> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("PointSession")
            .field("table", &self.table.object_id())
            .field("native_scope_active", &{
                #[cfg(not(test))]
                {
                    self.read_scope.is_some()
                }
                #[cfg(test)]
                {
                    false
                }
            })
            .finish_non_exhaustive()
    }
}

impl Table {
    /// Registers a transactional table around an already-created safe tree.
    ///
    /// `tree` must be freshly created for this table and placed under this
    /// table's exclusive semantic ownership. Publishing through another tree
    /// handle or constructing a second registry-backed `Table` over a clone of
    /// the same tree violates the adapter contract: native `RecordId` values
    /// are capabilities into exactly this table's private Rust registry.
    #[cfg(not(test))]
    pub fn new(
        runtime: &Arc<Runtime>,
        tree: Tree,
        config: TableConfig,
    ) -> Result<Self, RegistrationError> {
        Self::with_directory(runtime, Directory::Native(NativeDirectory { tree }), config)
    }

    /// Creates a table with a fresh, internally owned native directory and
    /// enables direct stable-record tokens for its private lookup lane.
    ///
    /// Unlike [`Self::new`], the tree handle is never exposed to the caller.
    /// Consequently safe code cannot publish a scalar that did not originate
    /// in this table's stable registry. That closed provenance boundary lets
    /// point and scan hits recover the record directly without the ordinary
    /// ID-to-registry traversal. Transaction resources and physical lock
    /// targets retain the registry for every possible token dereference.
    #[cfg(not(test))]
    pub fn new_direct(
        runtime: &Arc<Runtime>,
        native_runtime: &MasstreeRuntime,
        native_worker: &Worker,
        config: TableConfig,
    ) -> Result<Self, TableCreateError> {
        let tree = native_runtime.create_tree(native_worker)?;
        Self::with_directory_mode(
            runtime,
            Directory::Native(NativeDirectory { tree }),
            config,
            RecordTokenMode::DirectRecordPointer,
        )
        .map_err(Into::into)
    }

    fn with_directory(
        runtime: &Arc<Runtime>,
        directory: Directory,
        config: TableConfig,
    ) -> Result<Self, RegistrationError> {
        Self::with_directory_mode(runtime, directory, config, RecordTokenMode::RegistryId)
    }

    fn with_directory_mode(
        runtime: &Arc<Runtime>,
        directory: Directory,
        config: TableConfig,
        record_token_mode: RecordTokenMode,
    ) -> Result<Self, RegistrationError> {
        let object = runtime.register_object()?;
        let namespace = LockNamespaceId::new(object.object_id().get())
            .expect("nonzero ObjectId always forms a lock namespace");
        let record_lock_class =
            LockClass::new(RECORD_LOCK_CLASS_VALUE).expect("the record lock class is nonzero");

        let registry = Registry::new(config, object.runtime_id(), namespace, record_lock_class)?;
        let scan_publication_owners =
            scan_publication_owners(runtime, config.trusted_scan_value_generation)?;
        let shared = Arc::new(TableShared {
            directory,
            registry,
            record_token_mode,
            structural: StructuralGate::default(),
            health: AtomicU8::new(TABLE_HEALTHY),
            runtime_id: object.runtime_id(),
            namespace,
            record_lock_class,
            directory_generation: AtomicU64::new(0),
            scan_generation: AtomicU64::new(0),
            scan_publication_owners,
        });

        let record_class = ResourceClass::new(RECORD_RESOURCE_CLASS_VALUE)
            .expect("the record resource class is nonzero");
        let directory_class = ResourceClass::new(DIRECTORY_RESOURCE_CLASS_VALUE)
            .expect("the directory resource class is nonzero");
        let record_resource = object.register_resource(
            record_class,
            TableAdapter {
                table: Arc::clone(&shared),
                role: AdapterRole::Record,
            },
        )?;
        let directory_resource = object.register_resource(
            directory_class,
            TableAdapter {
                table: Arc::clone(&shared),
                role: AdapterRole::Directory,
            },
        )?;
        let scan_class = ResourceClass::new(SCAN_RESOURCE_CLASS_VALUE)
            .expect("the scan resource class is nonzero");
        let scan_resource = object.register_resource(
            scan_class,
            TableAdapter {
                table: Arc::clone(&shared),
                role: AdapterRole::Scan,
            },
        )?;

        Ok(Self {
            record_resource,
            directory_resource,
            scan_resource,
        })
    }

    /// Binds point operations for `transaction` to one lazy native read scope.
    ///
    /// Existing per-operation methods remain available for compatibility. Use
    /// a session when several point operations will run on the same table and
    /// worker so native validation, structural admission, and RCU entry can be
    /// amortized across directory hits.
    #[inline]
    pub fn point_session<'session, 'context>(
        &'session self,
        transaction: &'session mut Transaction<'context, Active>,
        worker: &'session Worker,
    ) -> PointSession<'session, 'context> {
        PointSession {
            table: self,
            transaction,
            worker,
            #[cfg(not(test))]
            read_scope: None,
        }
    }

    /// Visits one all-present fixed-width read batch as the transaction's
    /// terminal operation.
    ///
    /// This restricted path stores only each record ID and OCC observation in
    /// STO until certification. Committed value snapshots live only for their
    /// visitor invocation, and [`PointReadBatch::results`] remains empty.
    /// Duplicate input keys are permitted and are visited independently in
    /// input order.
    ///
    /// Directory lookup completes, including release of the native RCU read
    /// scope, before the first visitor runs. If any key is absent, no visitor
    /// runs and [`TerminalReadVisitOutcome::RetryOrdinary`] is returned after
    /// definitely aborting `transaction`; the caller may then use an ordinary
    /// transaction to intern or mutate the missing key.
    ///
    /// Visitor side effects are not rolled back if later certification
    /// conflicts. The returned ready transaction permits only commit or abort.
    #[inline]
    pub fn visit_fixed_terminal<'worker, const KEY_LENGTH: usize>(
        &self,
        transaction: TerminalReadTransaction<'worker, TerminalReadOpen>,
        worker: &Worker,
        keys: &[[u8; KEY_LENGTH]],
        batch: &mut PointReadBatch,
        visit: impl for<'value> FnMut(usize, Option<&'value Value>),
    ) -> Result<TerminalReadVisitOutcome<'worker>, AccessError> {
        #[cfg(not(test))]
        {
            let mut read_scope = None;
            self.visit_fixed_terminal_inner(transaction, keys, batch, visit, |batch| {
                self.lookup_fixed_in_read_scope(worker, &mut read_scope, keys, batch)
            })
        }
        #[cfg(test)]
        {
            let _ = worker;
            self.visit_fixed_terminal_inner(transaction, keys, batch, visit, |batch| {
                for key in keys {
                    batch.push_record_id(self.shared().lookup(None, key)?);
                }
                Ok(())
            })
        }
    }

    /// Reads the staged value or a committed value reloaded at the record's
    /// first observed OCC generation.
    #[inline]
    pub fn get(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
    ) -> Result<Option<Value>, AccessError> {
        self.get_inner(txn, Some(worker), key)
    }

    /// Visits the staged value or a committed value reloaded at the record's
    /// first observed OCC generation without retaining it in the transaction
    /// item.
    ///
    /// The value borrow is valid only for the callback invocation. This is
    /// useful at copy-oriented boundaries (for example, a C ABI with a caller
    /// supplied output buffer) where returning an owned [`Value`] would add
    /// shared-reference traffic that the caller cannot use.
    #[inline]
    pub fn visit_get<R>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        visit: impl for<'value> FnOnce(Option<&'value Value>) -> R,
    ) -> Result<R, AccessError> {
        self.visit_get_inner(txn, Some(worker), key, visit)
    }

    /// Visits one value and returns the stable identity resolved by that same
    /// directory access.
    ///
    /// A logical miss also returns a token: point misses intern a stable
    /// tombstone, so a later resolved put can reuse its identity without
    /// another Masstree traversal.
    #[inline]
    pub fn visit_get_resolving<R>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        visit: impl for<'value> FnOnce(Option<&'value Value>) -> R,
    ) -> Result<(R, ResolvedRecord), AccessError> {
        self.visit_get_resolving_inner(txn, Some(worker), key, visit)
    }

    /// Visits a previously resolved record without entering the native
    /// directory. All ordinary STO observation and validation still applies.
    #[inline]
    pub fn visit_get_resolved<R>(
        &self,
        txn: &mut Transaction<'_, Active>,
        resolved: ResolvedRecord,
        visit: impl for<'value> FnOnce(Option<&'value Value>) -> R,
    ) -> Result<R, AccessError> {
        let record_id = self.validate_resolved(resolved)?;
        let adapter = self.record_resource.adapter();
        txn.with_item(
            &self.record_resource,
            TableKey::Record(record_id),
            |entry| {
                let loaded = adapter.prepare_access(record_id, entry)?;
                Ok(visit(current_state(entry, loaded.as_ref())?.value()))
            },
        )
    }

    /// Reports transaction-local logical presence and returns the stable
    /// identity resolved by that same directory access.
    ///
    /// A logical miss interns a stable tombstone and returns its token. The
    /// operation observes only record metadata, but remains an ordinary STO
    /// read that participates in final OCC validation.
    #[inline]
    pub fn contains_resolving(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
    ) -> Result<(bool, ResolvedRecord), AccessError> {
        self.contains_resolving_inner(txn, Some(worker), key)
    }

    /// Reports transaction-local logical presence through a previously
    /// resolved record without entering the native directory.
    ///
    /// Staged puts and removes supply read-your-writes liveness. Unstaged
    /// accesses retain an ordinary read observation and final OCC validation.
    #[inline]
    pub fn contains_resolved(
        &self,
        txn: &mut Transaction<'_, Active>,
        resolved: ResolvedRecord,
    ) -> Result<bool, AccessError> {
        let record_id = self.validate_resolved(resolved)?;
        let adapter = self.record_resource.adapter();
        let access = self.shared().resolve_directory_access(record_id)?;
        txn.with_item(
            &self.record_resource,
            TableKey::Record(record_id),
            |entry| adapter.prepare_resolved_presence_access(access, entry),
        )
    }

    /// Visits the staged or committed value as an operation-scoped byte
    /// slice, avoiding an owned snapshot and shared-value reference-count
    /// increment on the committed large-value path.
    ///
    /// The callback is higher-ranked: its byte slice cannot be retained after
    /// the invocation. Inline values borrow operation-local storage and large
    /// committed values are pinned by an ArcSwap guard for exactly that same
    /// dynamic scope.
    ///
    /// ```compile_fail
    /// use masstree::Worker;
    /// use sto_core::{Active, Transaction};
    /// use sto_masstree::Table;
    ///
    /// fn retain<'outer>(
    ///     table: &Table,
    ///     txn: &mut Transaction<'_, Active>,
    ///     worker: &Worker,
    ///     retained: &mut Option<&'outer [u8]>,
    /// ) {
    ///     table
    ///         .visit_get_bytes(txn, worker, b"key", |value| *retained = value)
    ///         .unwrap();
    /// }
    /// ```
    #[inline]
    pub fn visit_get_bytes<R>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        visit: impl for<'value> FnOnce(Option<&'value [u8]>) -> R,
    ) -> Result<R, AccessError> {
        self.visit_get_bytes_inner(txn, Some(worker), key, visit)
    }

    /// Visits operation-scoped value bytes and returns the stable identity
    /// resolved by that same directory access.
    ///
    /// A logical miss still interns and returns a stable tombstone token, as
    /// in [`Self::visit_get_resolving`]. The byte borrow cannot escape the
    /// higher-ranked callback.
    #[inline]
    pub fn visit_get_resolving_bytes<R>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        visit: impl for<'value> FnOnce(Option<&'value [u8]>) -> R,
    ) -> Result<(R, ResolvedRecord), AccessError> {
        self.visit_get_resolving_bytes_inner(txn, Some(worker), key, visit)
    }

    /// Visits operation-scoped bytes through a previously resolved record.
    ///
    /// This skips only directory lookup; observation, stable-state loading,
    /// final OCC validation, and fail-closed poisoning are identical to
    /// [`Self::visit_get_bytes`].
    #[inline]
    pub fn visit_get_resolved_bytes<R>(
        &self,
        txn: &mut Transaction<'_, Active>,
        resolved: ResolvedRecord,
        visit: impl for<'value> FnOnce(Option<&'value [u8]>) -> R,
    ) -> Result<R, AccessError> {
        let record_id = self.validate_resolved(resolved)?;
        let adapter = self.record_resource.adapter();
        txn.with_item(
            &self.record_resource,
            TableKey::Record(record_id),
            |entry| adapter.visit_access_bytes(record_id, entry, visit),
        )
    }

    /// Copies staged or committed value bytes directly into caller-owned
    /// storage and returns the stable identity resolved by the same directory
    /// access.
    ///
    /// Unlike the byte-visitor API, the committed inline path does not first
    /// materialize a complete byte-array snapshot. Missing values, conflicts,
    /// faults, and insufficient capacity leave `output` unchanged.
    #[inline]
    pub fn copy_get_resolving(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        output: &mut [u8],
    ) -> Result<(ValueCopyOutcome, ResolvedRecord), AccessError> {
        self.copy_get_resolving_inner(txn, Some(worker), key, output)
    }

    /// Copies bytes through a previously resolved record without entering the
    /// native directory.
    ///
    /// The token is validated and converted to a table-bound proof once. All
    /// ordinary STO observation and final OCC validation still apply. Every
    /// non-`Copied` outcome leaves `output` unchanged.
    #[inline]
    pub fn copy_get_resolved(
        &self,
        txn: &mut Transaction<'_, Active>,
        resolved: ResolvedRecord,
        output: &mut [u8],
    ) -> Result<ValueCopyOutcome, AccessError> {
        let resolved = self.bind_resolved(resolved)?;
        self.copy_get_validated(txn, resolved, output)
    }

    /// Copies and replaces one live value while resolving its stable identity.
    ///
    /// Directory lookup, STO item access, value copy, `modify`, and replacement
    /// staging occur under one transaction failure boundary. A logical miss
    /// returns `Ok((None, resolved))`, where `resolved` names the stable
    /// tombstone, without invoking `modify` or changing `output`. A live value
    /// that does not fit `output`, an error from `modify`, or a replacement
    /// length beyond `output` returns an error and dooms the transaction.
    ///
    /// This is a private integration seam for callers that own storage through
    /// transaction finish; it intentionally avoids [`PointReadBatch`].
    ///
    /// # Safety
    ///
    /// If `modify` returns a length successfully, `output` must stay at the
    /// same address and `output[..replacement_len]` must remain readable and
    /// immutable until `txn` commits or aborts. This promise applies even if a
    /// later internal step returns an error or unwinds. The allocation must not
    /// alias table storage or another live borrowed transaction intent.
    #[doc(hidden)]
    #[allow(
        unsafe_code,
        reason = "the caller supplies the external value lifetime retained by the transaction"
    )]
    #[inline]
    pub unsafe fn try_modify_resolving_borrowed(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        output: &mut [u8],
        modify: impl for<'buffer> FnOnce(&'buffer mut [u8], usize) -> Result<usize, AccessError>,
    ) -> Result<(Option<usize>, ResolvedRecord), AccessError> {
        // SAFETY: This method forwards its output-lifetime and non-aliasing
        // contract unchanged to the shared scalar implementation.
        unsafe {
            self.try_modify_resolving_borrowed_inner(txn, Some(worker), key, output, modify, || {
                self.shared().lookup(Some(worker), key)
            })
        }
    }

    /// Copies and replaces one live value through a resolved-record token.
    ///
    /// A matching token skips the native directory and performs value copy,
    /// `modify`, and replacement staging in one STO item access. Miss, buffer,
    /// callback, output-lifetime, and OCC semantics match
    /// [`Self::try_modify_resolving_borrowed`]. Token validation itself occurs
    /// before item access, as for [`Self::copy_get_resolved`].
    ///
    /// # Safety
    ///
    /// This has the same output lifetime and non-aliasing requirements as
    /// [`Self::try_modify_resolving_borrowed`].
    #[doc(hidden)]
    #[allow(
        unsafe_code,
        reason = "the caller supplies the external value lifetime retained by the transaction"
    )]
    #[inline]
    pub unsafe fn try_modify_resolved_borrowed(
        &self,
        txn: &mut Transaction<'_, Active>,
        resolved: ResolvedRecord,
        output: &mut [u8],
        modify: impl for<'buffer> FnOnce(&'buffer mut [u8], usize) -> Result<usize, AccessError>,
    ) -> Result<Option<usize>, AccessError> {
        let resolved = self.bind_resolved(resolved)?;
        // SAFETY: This method forwards its output-lifetime and non-aliasing
        // contract unchanged to the validated-token implementation.
        unsafe { self.try_modify_resolved_borrowed_inner(txn, resolved, output, modify) }
    }

    /// Tries a reusable resolved-record cache entry and copies its value.
    ///
    /// A token minted by another table is reported as `Ok(None)` so a cache
    /// owner can perform an ordinary key lookup instead. A matching token is
    /// table-bound once, and its stable record slot is resolved at most once
    /// inside the transaction access. Errors from a matching record retain the
    /// ordinary transaction failure semantics.
    #[inline]
    pub fn try_copy_get_cached_resolved(
        &self,
        txn: &mut Transaction<'_, Active>,
        resolved: ResolvedRecord,
        output: &mut [u8],
    ) -> Result<Option<ValueCopyOutcome>, AccessError> {
        let Some(resolved) = self.try_bind_resolved(resolved) else {
            return Ok(None);
        };
        self.copy_get_validated(txn, resolved, output).map(Some)
    }

    /// Returns whether `resolved` was minted by this table.
    #[inline]
    pub fn owns_resolved(&self, resolved: ResolvedRecord) -> bool {
        resolved.runtime_id == self.record_resource.runtime_id()
            && resolved.object_id == self.record_resource.object_id()
    }

    /// Stages an unconditional live value and returns the prior abstract value.
    #[inline]
    pub fn put(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        value: &[u8],
    ) -> Result<Option<Value>, AccessError> {
        self.put_inner(txn, Some(worker), key, self.staging_input(value))
    }

    /// Stages an unconditional live value and reports whether the prior
    /// abstract value was present, without loading its payload.
    #[inline]
    pub fn put_with_previous_presence(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        value: &[u8],
    ) -> Result<bool, AccessError> {
        self.put_presence_inner(txn, Some(worker), key, self.staging_input(value))
    }

    /// Stages an unconditional live value without copying a caller-owned
    /// buffer that is already guaranteed to outlive this transaction.
    ///
    /// # Safety
    ///
    /// `value` must remain readable and immutable until `txn` commits or
    /// aborts. Inline and oversized values become owned before this method
    /// returns; eligible bounded values retain the borrow only in the
    /// transaction-local intent.
    #[allow(
        unsafe_code,
        reason = "the caller supplies the external value lifetime required by this fast lane"
    )]
    #[inline]
    pub unsafe fn put_borrowed_with_previous_presence(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        value: &[u8],
    ) -> Result<bool, AccessError> {
        self.put_presence_inner(txn, Some(worker), key, self.borrowed_staging_input(value))
    }

    /// Stages an unconditional live value through a previously resolved
    /// record identity and reports whether its prior abstract value existed.
    /// This skips only directory lookup; OCC and commit behavior are identical
    /// to [`Self::put_with_previous_presence`].
    #[inline]
    pub fn put_resolved_with_previous_presence(
        &self,
        txn: &mut Transaction<'_, Active>,
        resolved: ResolvedRecord,
        value: &[u8],
    ) -> Result<bool, AccessError> {
        self.put_resolved_presence_inner(txn, resolved, self.staging_input(value))
    }

    /// Borrowing counterpart to [`Self::put_resolved_with_previous_presence`].
    ///
    /// # Safety
    ///
    /// `value` must remain readable and immutable until `txn` commits or
    /// aborts.
    #[allow(
        unsafe_code,
        reason = "the caller supplies the external value lifetime required by this fast lane"
    )]
    #[inline]
    pub unsafe fn put_resolved_borrowed_with_previous_presence(
        &self,
        txn: &mut Transaction<'_, Active>,
        resolved: ResolvedRecord,
        value: &[u8],
    ) -> Result<bool, AccessError> {
        self.put_resolved_presence_inner(txn, resolved, self.borrowed_staging_input(value))
    }

    /// Stages a value only when the transaction-local record is absent.
    #[inline]
    pub fn insert(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        value: &[u8],
    ) -> Result<InsertOutcome, AccessError> {
        self.insert_inner(txn, Some(worker), key, self.staging_input(value))
    }

    /// Stages a value on a key the caller expects to be absent and returns
    /// `true` when it was inserted.
    ///
    /// This lane asks the append-only directory to resolve-or-publish in one
    /// traversal. It remains fully conditional: an existing live value is
    /// left unchanged and returns `false`. Because even that unexpected
    /// duplicate consumes one never-reused candidate ID, callers should use
    /// this only when absence is the normal case.
    #[inline]
    pub fn insert_expected_absent(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        value: &[u8],
    ) -> Result<bool, AccessError> {
        self.insert_expected_absent_inner(txn, Some(worker), key, self.staging_input(value))
    }

    /// Borrowing counterpart to [`Self::insert_expected_absent`].
    ///
    /// # Safety
    ///
    /// `value` must remain readable and immutable until `txn` commits or
    /// aborts.
    #[allow(
        unsafe_code,
        reason = "the caller supplies the external value lifetime required by this fast lane"
    )]
    #[inline]
    pub unsafe fn insert_expected_absent_borrowed(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        value: &[u8],
    ) -> Result<bool, AccessError> {
        self.insert_expected_absent_inner(
            txn,
            Some(worker),
            key,
            self.borrowed_staging_input(value),
        )
    }

    /// Stages a logical tombstone and returns the prior abstract value.
    #[inline]
    pub fn remove(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
    ) -> Result<Option<Value>, AccessError> {
        self.remove_inner(txn, Some(worker), key)
    }

    /// Stages a tombstone and reports whether the prior abstract value was
    /// present, without loading its payload.
    #[inline]
    pub fn remove_with_previous_presence(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
    ) -> Result<bool, AccessError> {
        self.remove_presence_inner(txn, Some(worker), key)
    }

    /// Stages a tombstone through a previously resolved record identity and
    /// reports whether its prior abstract value existed.
    ///
    /// This skips only directory lookup; OCC and commit behavior are identical
    /// to [`Self::remove_with_previous_presence`].
    #[inline]
    pub fn remove_resolved_with_previous_presence(
        &self,
        txn: &mut Transaction<'_, Active>,
        resolved: ResolvedRecord,
    ) -> Result<bool, AccessError> {
        let record_id = self.validate_resolved(resolved)?;
        let adapter = self.record_resource.adapter();
        let access = self.shared().resolve_directory_access(record_id)?;
        txn.with_item(
            &self.record_resource,
            TableKey::Record(record_id),
            |entry| {
                let previous_present = adapter.prepare_resolved_presence_access(access, entry)?;
                if !previous_present {
                    return Ok(false);
                }
                stage_record_state(adapter, access, entry, RecordState::tombstone())?;
                Ok(true)
            },
        )
    }

    /// Returns an owned, bounded transactional range snapshot.
    ///
    /// The scan observes the physical-directory generation, validates every
    /// copied physical record as an ordinary STO read, and observes point
    /// mutations already staged by `txn` before applying the logical limit.
    pub fn scan(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        request: ScanRequest<'_>,
    ) -> Result<Vec<ScanRecord>, AccessError> {
        self.scan_inner(txn, Some(worker), request)
    }

    /// Visits live rows from one bounded transactional range scan.
    ///
    /// Directory generation, per-record OCC observation, tombstone filtering,
    /// staged read-your-writes state, bounds, direction, and logical limit are
    /// identical to [`Self::scan`]. Returning [`ScanControl::Stop`] ends the
    /// scan immediately after the current row; later physical rows are not
    /// added to the transaction's read set.
    ///
    /// Delivery is streaming. If an error is discovered after earlier calls,
    /// those callback effects are not rolled back even though this method
    /// returns `Err`; the transaction retains the observations made before the
    /// error and should follow its ordinary abort/retry policy.
    pub fn visit_scan(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        request: ScanRequest<'_>,
        visit: impl for<'row> FnMut(ScanRecordRef<'row>) -> ScanControl,
    ) -> Result<ScanVisitOutcome, AccessError> {
        let mut scratch = ScanScratch::default();
        self.visit_scan_with_scratch(txn, worker, request, &mut scratch, visit)
    }

    /// Visits a transactional range while reusing caller-owned native scan
    /// storage. See [`Self::visit_scan`] for observation and error semantics.
    pub fn visit_scan_with_scratch(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        request: ScanRequest<'_>,
        scratch: &mut ScanScratch,
        mut visit: impl for<'row> FnMut(ScanRecordRef<'row>) -> ScanControl,
    ) -> Result<ScanVisitOutcome, AccessError> {
        self.visit_scan_inner(txn, Some(worker), request, scratch, &mut |row| {
            Ok(visit(row))
        })
    }

    /// Visits live scan rows as operation-scoped byte slices.
    ///
    /// This has the same ordering, filtering, flow control, and partial-effect
    /// semantics as [`Self::visit_scan`], but committed large values remain
    /// protected by an ArcSwap guard instead of cloning an `Arc`. Neither the
    /// key nor value borrow can escape its higher-ranked callback invocation.
    pub fn visit_scan_bytes(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        request: ScanRequest<'_>,
        visit: impl for<'row> FnMut(ScanBytesRef<'row>) -> ScanControl,
    ) -> Result<ScanVisitOutcome, AccessError> {
        let mut scratch = ScanScratch::default();
        self.visit_scan_bytes_with_scratch(txn, worker, request, &mut scratch, visit)
    }

    /// Visits operation-scoped scan bytes while reusing caller-owned native
    /// scan storage. See [`Self::visit_scan_bytes`] for borrow and transaction
    /// semantics.
    pub fn visit_scan_bytes_with_scratch(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        request: ScanRequest<'_>,
        scratch: &mut ScanScratch,
        mut visit: impl for<'row> FnMut(ScanBytesRef<'row>) -> ScanControl,
    ) -> Result<ScanVisitOutcome, AccessError> {
        self.visit_scan_bytes_inner(txn, Some(worker), request, scratch, &mut |row| {
            Ok(visit(row))
        })
    }

    /// Visits a private direct table using one conservative table observation.
    ///
    /// The ordinary scan remains the fallback if this transaction already has
    /// record-local state for the table. On the fast path each committed row is
    /// read through an OCC version sandwich, while one value-generation
    /// observation covers every trusted scan of this table in the attempt and
    /// remains for final validation. The scan generation advances for both
    /// directory changes and committed record-state publications.
    ///
    /// # Safety
    ///
    /// Until this call returns, `visit` must not retain either byte pointer or
    /// re-enter this transaction, table, native worker, or its Masstree runtime
    /// through FFI or another unsafe alias. The callback must return normally.
    /// This method checks that the table owns a private direct-token directory;
    /// other tables take the ordinary row-item path.
    #[doc(hidden)]
    #[allow(
        unsafe_code,
        reason = "the caller promises callback non-retention and non-reentry across FFI"
    )]
    pub unsafe fn visit_scan_bytes_trusted_with_scratch(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        request: ScanRequest<'_>,
        scratch: &mut ScanScratch,
        mut visit: impl for<'row> FnMut(ScanBytesRef<'row>) -> ScanControl,
    ) -> Result<ScanVisitOutcome, AccessError> {
        self.visit_scan_bytes_trusted_inner(txn, Some(worker), request, scratch, &mut visit)
    }

    /// Visits up to 300 values in a lower-inclusive, upper-exclusive range.
    ///
    /// Direct tables use the private native RecordId scan, which emits no row
    /// keys. Other table modes and transactions with prior local record state
    /// retain the ordinary scan behavior.
    ///
    /// # Safety
    ///
    /// Until this call returns, `visit` must not retain the value pointer or
    /// re-enter this transaction, table, native worker, or Masstree runtime.
    /// The callback must return normally.
    #[doc(hidden)]
    #[allow(
        unsafe_code,
        clippy::too_many_arguments,
        reason = "the caller promises callback non-retention and non-reentry across FFI"
    )]
    pub unsafe fn visit_bounded_forward_values_trusted_with_scratch(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        lower: &[u8],
        upper: &[u8],
        limit: usize,
        scratch: &mut ScanScratch,
        mut visit: impl for<'value> FnMut(&'value [u8], ResolvedRecord) -> ScanControl,
    ) -> Result<ScanVisitOutcome, AccessError> {
        let request = ScanRequest::new(ScanDirection::Forward, limit)
            .with_lower(ScanBound::Included(lower))
            .with_upper(ScanBound::Excluded(upper));
        if limit > PRIVATE_BOUNDED_FORWARD_VALUE_SCAN_MAX_RECORDS {
            return self.fail_scan(txn, CapacityError::BufferLimit.into());
        }
        if self.shared().record_token_mode != RecordTokenMode::DirectRecordPointer
            || !self.shared().registry.config.trusted_scan_value_generation
            || txn.has_items_for(&self.record_resource)
        {
            return self.visit_scan_bytes_inner(txn, Some(worker), request, scratch, &mut |row| {
                Ok(visit(row.value(), row.resolved()))
            });
        }

        #[cfg(test)]
        {
            self.visit_scan_bytes_trusted_inner(txn, Some(worker), request, scratch, &mut |row| {
                visit(row.value(), row.resolved())
            })
        }
        #[cfg(not(test))]
        {
            self.visit_bounded_forward_values_keyless_inner(
                txn, worker, lower, upper, limit, scratch, &mut visit,
            )
        }
    }

    pub fn object_id(&self) -> ObjectId {
        self.record_resource.object_id()
    }

    /// Allocates an exact-index cache for stable identities minted by this
    /// table.
    ///
    /// The cache owns eight atomic bytes per slot and retains a clone of the
    /// table. Slots are empty until [`DenseResolvedCache::remember`] publishes
    /// an identity.
    #[doc(hidden)]
    pub fn dense_resolved_cache(
        &self,
        slot_count: usize,
    ) -> Result<DenseResolvedCache, CapacityError> {
        let mut record_ids = Vec::new();
        record_ids
            .try_reserve_exact(slot_count)
            .map_err(|_| CapacityError::BufferLimit)?;
        record_ids.resize_with(slot_count, || AtomicU64::new(0));
        Ok(DenseResolvedCache {
            table: self.clone(),
            record_ids: record_ids.into_boxed_slice(),
        })
    }

    pub fn health(&self) -> TableHealth {
        self.shared().health()
    }

    pub fn usage(&self) -> TableUsage {
        self.shared().registry.usage()
    }

    /// Permanently prevents this table from adding physical directory keys.
    ///
    /// Existing records remain readable and mutable, including physical
    /// tombstones that a later transaction resurrects. Once this call returns
    /// successfully, a point miss fails with [`Unsupported::Capability`]
    /// before reserving a record ID. Range scans also stop taking the Rust
    /// structural read lock because no later directory publication is possible;
    /// native RCU admission and all STO observation and validation remain.
    ///
    /// This administrative operation waits for an admitted structural scan or
    /// publisher to leave the Rust gate. Calls after the first successful seal
    /// are idempotent.
    pub fn seal_directory_structure(&self) -> Result<(), AccessError> {
        let shared = self.shared();
        shared.ensure_healthy()?;
        if shared.structural.is_sealed() {
            return Ok(());
        }

        let structural = shared
            .structural
            .write()
            .inspect_err(|error| shared.note_access_error(error))?;
        if shared.structural.is_sealed() {
            drop(structural);
            return Ok(());
        }
        shared
            .directory
            .seal_structure()
            .inspect_err(|error| shared.note_access_error(error))?;
        shared.structural.mark_sealed();
        drop(structural);
        Ok(())
    }

    #[inline]
    fn get_inner(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
    ) -> Result<Option<Value>, AccessError> {
        self.visit_get_inner(txn, worker, key, |value| value.cloned())
    }

    #[inline]
    fn visit_get_inner<R>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        visit: impl for<'value> FnOnce(Option<&'value Value>) -> R,
    ) -> Result<R, AccessError> {
        self.visit_get_resolving_inner(txn, worker, key, visit)
            .map(|(result, _resolved)| result)
    }

    #[inline]
    fn visit_get_resolving_inner<R>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        visit: impl for<'value> FnOnce(Option<&'value Value>) -> R,
    ) -> Result<(R, ResolvedRecord), AccessError> {
        self.visit_get_resolving_inner_with_lookup(txn, worker, key, visit, || {
            self.shared().lookup(worker, key)
        })
    }

    #[inline]
    #[cfg_attr(test, allow(dead_code))]
    fn visit_get_inner_with_lookup<R>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        visit: impl for<'value> FnOnce(Option<&'value Value>) -> R,
        lookup: impl FnOnce() -> Result<Option<RecordId>, AccessError>,
    ) -> Result<R, AccessError> {
        self.visit_get_resolving_inner_with_lookup(txn, worker, key, visit, lookup)
            .map(|(result, _resolved)| result)
    }

    #[inline]
    fn visit_get_resolving_inner_with_lookup<R>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        visit: impl for<'value> FnOnce(Option<&'value Value>) -> R,
        lookup: impl FnOnce() -> Result<Option<RecordId>, AccessError>,
    ) -> Result<(R, ResolvedRecord), AccessError> {
        let adapter = self.record_resource.adapter();
        self.with_key_record_lookup(txn, worker, key, lookup, |entry, access| {
            let loaded = adapter.prepare_resolved_access(access, entry)?;
            let result = visit(current_state(entry, loaded.as_ref())?.value());
            Ok((result, self.mint_resolved(access.record_id)))
        })
    }

    #[inline]
    fn contains_resolving_inner(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
    ) -> Result<(bool, ResolvedRecord), AccessError> {
        self.contains_resolving_inner_with_lookup(txn, worker, key, || {
            self.shared().lookup(worker, key)
        })
    }

    #[inline]
    fn contains_resolving_inner_with_lookup(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        lookup: impl FnOnce() -> Result<Option<RecordId>, AccessError>,
    ) -> Result<(bool, ResolvedRecord), AccessError> {
        let adapter = self.record_resource.adapter();
        self.with_key_record_lookup(txn, worker, key, lookup, |entry, access| {
            let present = adapter.prepare_resolved_presence_access(access, entry)?;
            Ok((present, self.mint_resolved(access.record_id)))
        })
    }

    #[inline]
    fn visit_get_bytes_inner<R>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        visit: impl for<'value> FnOnce(Option<&'value [u8]>) -> R,
    ) -> Result<R, AccessError> {
        self.visit_get_resolving_bytes_inner(txn, worker, key, visit)
            .map(|(result, _resolved)| result)
    }

    #[inline]
    fn visit_get_resolving_bytes_inner<R>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        visit: impl for<'value> FnOnce(Option<&'value [u8]>) -> R,
    ) -> Result<(R, ResolvedRecord), AccessError> {
        self.visit_get_resolving_bytes_inner_with_lookup(txn, worker, key, visit, || {
            self.shared().lookup(worker, key)
        })
    }

    #[inline]
    fn visit_get_resolving_bytes_inner_with_lookup<R>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        visit: impl for<'value> FnOnce(Option<&'value [u8]>) -> R,
        lookup: impl FnOnce() -> Result<Option<RecordId>, AccessError>,
    ) -> Result<(R, ResolvedRecord), AccessError> {
        let adapter = self.record_resource.adapter();
        self.with_key_record_lookup(txn, worker, key, lookup, |entry, access| {
            let result = adapter.visit_resolved_access_bytes(access, entry, visit)?;
            Ok((result, self.mint_resolved(access.record_id)))
        })
    }

    #[inline]
    fn copy_get_resolving_inner(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        output: &mut [u8],
    ) -> Result<(ValueCopyOutcome, ResolvedRecord), AccessError> {
        self.copy_get_resolving_inner_with_lookup(txn, worker, key, output, || {
            self.shared().lookup(worker, key)
        })
    }

    #[inline]
    fn copy_get_resolving_inner_with_lookup(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        output: &mut [u8],
        lookup: impl FnOnce() -> Result<Option<RecordId>, AccessError>,
    ) -> Result<(ValueCopyOutcome, ResolvedRecord), AccessError> {
        let adapter = self.record_resource.adapter();
        self.with_key_record_lookup(txn, worker, key, lookup, |entry, access| {
            let result = adapter.copy_resolved_access(access, entry, output)?;
            Ok((result, self.mint_resolved(access.record_id)))
        })
    }

    #[inline]
    fn copy_get_validated(
        &self,
        txn: &mut Transaction<'_, Active>,
        resolved: ValidatedResolved<'_>,
        output: &mut [u8],
    ) -> Result<ValueCopyOutcome, AccessError> {
        let adapter = self.record_resource.adapter();
        txn.with_item(
            &self.record_resource,
            TableKey::Record(resolved.record_id),
            |entry| adapter.copy_access_inner(resolved.record_id, None, entry, output),
        )
    }

    #[allow(
        unsafe_code,
        clippy::too_many_arguments,
        reason = "the caller supplies the external value lifetime retained by the transaction"
    )]
    #[inline]
    unsafe fn try_modify_resolving_borrowed_inner(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        output: &mut [u8],
        modify: impl for<'buffer> FnOnce(&'buffer mut [u8], usize) -> Result<usize, AccessError>,
        lookup: impl FnOnce() -> Result<Option<RecordId>, AccessError>,
    ) -> Result<(Option<usize>, ResolvedRecord), AccessError> {
        let adapter = self.record_resource.adapter();
        self.with_key_record_lookup(txn, worker, key, lookup, |entry, access| {
            let copied = adapter.copy_resolved_access(access, entry, output)?;
            // SAFETY: Forwarded from this method's caller-owned output
            // lifetime and non-aliasing contract.
            let length =
                unsafe { self.finish_try_modify_borrowed(access, entry, copied, output, modify) }?;
            Ok((length, self.mint_resolved(access.record_id)))
        })
    }

    #[allow(
        unsafe_code,
        reason = "the caller supplies the external value lifetime retained by the transaction"
    )]
    #[inline]
    unsafe fn try_modify_resolved_borrowed_inner(
        &self,
        txn: &mut Transaction<'_, Active>,
        resolved: ValidatedResolved<'_>,
        output: &mut [u8],
        modify: impl for<'buffer> FnOnce(&'buffer mut [u8], usize) -> Result<usize, AccessError>,
    ) -> Result<Option<usize>, AccessError> {
        let adapter = self.record_resource.adapter();
        let access = self.shared().resolve_directory_access(resolved.record_id)?;
        txn.with_item(
            &self.record_resource,
            TableKey::Record(resolved.record_id),
            |entry| {
                let copied = adapter.copy_resolved_access(access, entry, output)?;
                // SAFETY: Forwarded from this method's caller-owned output
                // lifetime and non-aliasing contract.
                unsafe { self.finish_try_modify_borrowed(access, entry, copied, output, modify) }
            },
        )
    }

    #[allow(
        unsafe_code,
        reason = "the caller supplies the external value lifetime retained by the transaction"
    )]
    #[inline(always)]
    unsafe fn finish_try_modify_borrowed(
        &self,
        access: RecordAccess<'_>,
        entry: &mut Entry<'_, TableAdapter>,
        copied: ValueCopyOutcome,
        output: &mut [u8],
        modify: impl for<'buffer> FnOnce(&'buffer mut [u8], usize) -> Result<usize, AccessError>,
    ) -> Result<Option<usize>, AccessError> {
        let current_len = match copied {
            ValueCopyOutcome::Miss => return Ok(None),
            ValueCopyOutcome::Copied { len } => len,
            ValueCopyOutcome::BufferTooSmall { .. } => {
                return Err(CapacityError::BufferLimit.into());
            }
        };
        let replacement_len = modify(output, current_len)?;
        if replacement_len > output.len() {
            return Err(CapacityError::BufferLimit.into());
        }

        // The unsafe caller contract keeps this exact prefix readable and
        // immutable until transaction finish. Eligible bounded values retain
        // the borrow; inline and oversized values take the ordinary owned path.
        let replacement = Value::from(self.borrowed_staging_input(&output[..replacement_len]));
        stage_record_state(
            self.record_resource.adapter(),
            access,
            entry,
            RecordState::Live(replacement),
        )?;
        Ok(Some(replacement_len))
    }

    #[inline]
    fn visit_fixed_inner<const CAPTURE_VALUES: bool, const KEY_LENGTH: usize>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        keys: &[[u8; KEY_LENGTH]],
        batch: &mut PointReadBatch,
        mut visit: impl for<'value> FnMut(usize, Option<&'value Value>),
        lookup_batch: impl FnOnce(&mut PointReadBatch) -> Result<(), AccessError>,
    ) -> Result<usize, AccessError> {
        if keys.is_empty() {
            batch.clear();
            return Ok(0);
        }

        let shared = self.shared();
        let adapter = self.record_resource.adapter();
        let result = txn.with_item_session(&self.record_resource, |items| {
            batch.prepare_read::<CAPTURE_VALUES>(keys.len())?;
            lookup_batch(batch)?;
            if batch.record_ids.len() != keys.len() {
                return Err(table_fault(
                    "fixed point lookup returned the wrong result count",
                ));
            }
            shared.prefetch_direct_directory_records(&batch.record_ids)?;
            batch.item_keys.extend(
                batch
                    .record_ids
                    .iter()
                    .flatten()
                    .copied()
                    .map(TableKey::Record),
            );
            let mut aliases_proven = false;
            if batch.item_keys.len() == batch.record_ids.len() {
                let unique = prove_unique_fixed_records(
                    shared,
                    keys,
                    &batch.record_ids,
                    &batch.item_keys,
                    &mut batch.unique_key_index,
                    &mut batch.alias_order,
                )?;
                aliases_proven = true;
                if items.can_start_unique_item_batch() {
                    if let Some(unique) = unique {
                        let record_ids = &batch.record_ids;
                        let values = &mut batch.values;
                        if items.try_with_unique_item_batch(unique, |index, entry| {
                            let record_id = record_ids[index]
                                .expect("the all-hit unique batch contains a record ID");
                            let access = shared.resolve_directory_access(record_id)?;
                            visit_fixed_value::<CAPTURE_VALUES>(
                                adapter, entry, access, index, &mut visit, values,
                            )
                        })? {
                            return Ok(());
                        }
                    }
                }
            }
            if !aliases_proven {
                validate_fixed_record_aliases(
                    shared,
                    keys,
                    &batch.record_ids,
                    &mut batch.alias_order,
                )?;
            }

            for (index, key) in keys.iter().enumerate() {
                let batched_record_id = batch.record_ids[index];
                items.with_resolved_item(
                    || {
                        let found = match batched_record_id {
                            Some(record_id) => Some(record_id),
                            None => shared.lookup(worker, key)?,
                        };
                        match found {
                            Some(record_id) => {
                                let access = shared.resolve_directory_access(record_id)?;
                                Ok(Some((TableKey::Record(record_id), access)))
                            }
                            None => Ok(None),
                        }
                    },
                    || {
                        let record_id = shared.intern_missing(worker, key)?;
                        let access = shared.resolve_directory_access(record_id)?;
                        Ok((TableKey::Record(record_id), access))
                    },
                    |entry, access| {
                        visit_fixed_value::<CAPTURE_VALUES>(
                            adapter,
                            entry,
                            access,
                            index,
                            &mut visit,
                            &mut batch.values,
                        )
                    },
                )?;
            }
            Ok(())
        });
        if let Err(error) = result {
            batch.clear();
            return Err(error);
        }
        Ok(keys.len())
    }

    #[inline]
    fn visit_fixed_bytes_inner<const KEY_LENGTH: usize>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        keys: &[[u8; KEY_LENGTH]],
        batch: &mut PointReadBatch,
        mut visit: impl for<'value> FnMut(usize, Option<&'value [u8]>),
        lookup_batch: impl FnOnce(&mut PointReadBatch) -> Result<(), AccessError>,
    ) -> Result<usize, AccessError> {
        self.visit_fixed_resolving_bytes_inner(
            txn,
            worker,
            keys,
            batch,
            |index, current, _resolved| visit(index, current),
            lookup_batch,
        )
    }

    #[inline]
    fn visit_fixed_resolving_bytes_inner<const KEY_LENGTH: usize>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        keys: &[[u8; KEY_LENGTH]],
        batch: &mut PointReadBatch,
        mut visit: impl for<'value> FnMut(usize, Option<&'value [u8]>, ResolvedRecord),
        lookup_batch: impl FnOnce(&mut PointReadBatch) -> Result<(), AccessError>,
    ) -> Result<usize, AccessError> {
        if keys.is_empty() {
            batch.clear();
            return Ok(0);
        }

        let shared = self.shared();
        let adapter = self.record_resource.adapter();
        let result = txn.with_item_session(&self.record_resource, |items| {
            batch.prepare_read::<false>(keys.len())?;
            lookup_batch(batch)?;
            if batch.record_ids.len() != keys.len() {
                return Err(table_fault(
                    "fixed point lookup returned the wrong result count",
                ));
            }
            shared.prefetch_direct_directory_records(&batch.record_ids)?;
            batch.item_keys.extend(
                batch
                    .record_ids
                    .iter()
                    .flatten()
                    .copied()
                    .map(TableKey::Record),
            );
            let mut aliases_proven = false;
            if batch.item_keys.len() == batch.record_ids.len() {
                let unique = prove_unique_fixed_records(
                    shared,
                    keys,
                    &batch.record_ids,
                    &batch.item_keys,
                    &mut batch.unique_key_index,
                    &mut batch.alias_order,
                )?;
                aliases_proven = true;
                if items.can_start_unique_item_batch() {
                    if let Some(unique) = unique {
                        let record_ids = &batch.record_ids;
                        if items.try_with_unique_item_batch(unique, |index, entry| {
                            let record_id = record_ids[index]
                                .expect("the all-hit unique batch contains a record ID");
                            let access = shared.resolve_directory_access(record_id)?;
                            visit_fixed_bytes_value(
                                adapter,
                                entry,
                                access,
                                index,
                                self.mint_resolved(record_id),
                                &mut visit,
                            )
                        })? {
                            return Ok(());
                        }
                    }
                }
            }
            if !aliases_proven {
                validate_fixed_record_aliases(
                    shared,
                    keys,
                    &batch.record_ids,
                    &mut batch.alias_order,
                )?;
            }

            for (index, key) in keys.iter().enumerate() {
                let batched_record_id = batch.record_ids[index];
                items.with_resolved_item(
                    || {
                        let found = match batched_record_id {
                            Some(record_id) => Some(record_id),
                            None => shared.lookup(worker, key)?,
                        };
                        match found {
                            Some(record_id) => {
                                let access = shared.resolve_directory_access(record_id)?;
                                Ok(Some((TableKey::Record(record_id), access)))
                            }
                            None => Ok(None),
                        }
                    },
                    || {
                        let record_id = shared.intern_missing(worker, key)?;
                        let access = shared.resolve_directory_access(record_id)?;
                        Ok((TableKey::Record(record_id), access))
                    },
                    |entry, access| {
                        visit_fixed_bytes_value(
                            adapter,
                            entry,
                            access,
                            index,
                            self.mint_resolved(access.record_id),
                            &mut visit,
                        )
                    },
                )?;
            }
            Ok(())
        });
        if let Err(error) = result {
            batch.clear();
            return Err(error);
        }
        Ok(keys.len())
    }

    #[inline]
    fn visit_fixed_terminal_inner<'worker, const KEY_LENGTH: usize>(
        &self,
        transaction: TerminalReadTransaction<'worker, TerminalReadOpen>,
        keys: &[[u8; KEY_LENGTH]],
        batch: &mut PointReadBatch,
        mut visit: impl for<'value> FnMut(usize, Option<&'value Value>),
        lookup_batch: impl FnOnce(&mut PointReadBatch) -> Result<(), AccessError>,
    ) -> Result<TerminalReadVisitOutcome<'worker>, AccessError> {
        let lookup = (|| {
            batch.prepare_read::<false>(keys.len())?;
            lookup_batch(batch)?;
            if batch.record_ids.len() != keys.len() {
                return Err(table_fault(
                    "fixed point lookup returned the wrong result count",
                ));
            }
            Ok(())
        })();
        if let Err(error) = lookup {
            batch.clear();
            return Err(transaction.abort_with_access_error(error));
        }

        if batch.record_ids.iter().any(Option::is_none) {
            batch.clear();
            drop(transaction);
            return Ok(TerminalReadVisitOutcome::RetryOrdinary);
        }

        if let Err(error) = self
            .shared()
            .prefetch_direct_directory_records(&batch.record_ids)
        {
            batch.clear();
            return Err(transaction.abort_with_access_error(error));
        }

        batch.item_keys.extend(
            batch
                .record_ids
                .iter()
                .flatten()
                .copied()
                .map(TableKey::Record),
        );
        if let Err(error) = prove_unique_fixed_records(
            self.shared(),
            keys,
            &batch.record_ids,
            &batch.item_keys,
            &mut batch.unique_key_index,
            &mut batch.alias_order,
        ) {
            batch.clear();
            return Err(transaction.abort_with_access_error(error));
        }

        let adapter = self.record_resource.adapter();
        let ready = transaction.with_terminal_read_batch(
            &self.record_resource,
            &batch.item_keys,
            |index, entry| {
                let record_id =
                    batch.record_ids[index].expect("the terminal miss check rejected absent IDs");
                let access = self.shared().resolve_directory_access(record_id)?;
                let (version, snapshot, old_was_shared) =
                    adapter.prepare_terminal_read_access(access)?;
                // Transfer the OCC token to core before calling user code, so
                // an unwind leaves a complete prefix for definite cleanup.
                entry.record_read(TableObservation::Record(RecordObservation {
                    version,
                    original_live: snapshot.is_live(),
                    old_was_shared,
                }))?;
                visit(index, snapshot.value());
                Ok(())
            },
        );
        match ready {
            Ok(transaction) => Ok(TerminalReadVisitOutcome::Ready {
                transaction,
                visited: keys.len(),
            }),
            Err(error) => {
                batch.clear();
                Err(error)
            }
        }
    }

    #[inline]
    fn get_fixed_inner<'batch, const KEY_LENGTH: usize>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        keys: &[[u8; KEY_LENGTH]],
        batch: &'batch mut PointReadBatch,
        lookup_batch: impl FnOnce(&mut PointReadBatch) -> Result<(), AccessError>,
    ) -> Result<&'batch [Option<Value>], AccessError> {
        self.visit_fixed_inner::<true, KEY_LENGTH>(
            txn,
            worker,
            keys,
            batch,
            |_index, _value| {},
            lookup_batch,
        )?;
        Ok(batch.results())
    }

    fn modify_fixed_visit_inner<const CAPTURE_VALUES: bool, const KEY_LENGTH: usize>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        keys: &[[u8; KEY_LENGTH]],
        batch: &mut PointReadBatch,
        mut modify: impl for<'value> FnMut(
            usize,
            Option<&'value Value>,
        ) -> Result<PointMutation, AccessError>,
        lookup_batch: impl FnOnce(&mut PointReadBatch) -> Result<(), AccessError>,
    ) -> Result<usize, AccessError> {
        self.modify_fixed_resolving_visit_inner::<CAPTURE_VALUES, KEY_LENGTH>(
            txn,
            worker,
            keys,
            batch,
            |index, current, _resolved| modify(index, current),
            lookup_batch,
        )
    }

    fn modify_fixed_resolving_visit_inner<const CAPTURE_VALUES: bool, const KEY_LENGTH: usize>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        keys: &[[u8; KEY_LENGTH]],
        batch: &mut PointReadBatch,
        mut modify: impl for<'value> FnMut(
            usize,
            Option<&'value Value>,
            ResolvedRecord,
        ) -> Result<PointMutation, AccessError>,
        lookup_batch: impl FnOnce(&mut PointReadBatch) -> Result<(), AccessError>,
    ) -> Result<usize, AccessError> {
        if keys.is_empty() {
            batch.clear();
            return Ok(0);
        }

        let shared = self.shared();
        let adapter = self.record_resource.adapter();
        let result = txn.with_item_session(&self.record_resource, |items| {
            batch.prepare_modify::<CAPTURE_VALUES>(keys.len())?;
            lookup_batch(batch)?;
            if batch.record_ids.len() != keys.len() {
                return Err(table_fault(
                    "fixed point lookup returned the wrong result count",
                ));
            }
            // Resolve every missing binding before creating the first STO
            // item, then prove that distinct directory keys retained distinct
            // stable record identities. Publishing one of these bindings is
            // a physical, append-only side effect: a later transaction abort
            // leaves a reachable tombstone, just like scalar `intern_missing`.
            // No logical row becomes live until commit.
            //
            // Duplicate input keys deliberately skip this pre-intern lane.
            // The ordinary fallback below must visit them sequentially so a
            // later position observes an earlier staged mutation.
            // Native fixed-width interning does not depend on whether core's
            // item index is still empty. TPC-C reaches its insert batch after
            // earlier scalar accesses, so tie only the later typed-item fast
            // path to `can_start_unique_item_batch`.
            let unique_input = prove_unique_keys(keys, &mut batch.alias_order)?.is_some();
            let missing_count =
                shared.count_missing_and_prefetch_direct_directory_records(&batch.record_ids)?;
            let mut aliases_proven = false;
            if unique_input && missing_count != 0 {
                // A malformed mixed lookup must fail before native interning
                // makes any additional append-only binding reachable. An
                // all-miss batch has no preexisting identities to compare.
                if missing_count != keys.len() {
                    validate_fixed_record_aliases(
                        shared,
                        keys,
                        &batch.record_ids,
                        &mut batch.alias_order,
                    )?;
                }
                #[cfg(not(test))]
                if KEY_LENGTH <= 16 {
                    shared.intern_fixed_missing(worker, keys, missing_count, batch)?;
                } else {
                    for (index, key) in keys.iter().enumerate() {
                        if batch.record_ids[index].is_none() {
                            batch.record_ids[index] = Some(shared.intern_missing(worker, key)?);
                        }
                    }
                }
                #[cfg(test)]
                for (index, key) in keys.iter().enumerate() {
                    if batch.record_ids[index].is_none() {
                        batch.record_ids[index] = Some(shared.intern_missing(worker, key)?);
                    }
                }
            }

            batch.item_keys.extend(
                batch
                    .record_ids
                    .iter()
                    .flatten()
                    .copied()
                    .map(TableKey::Record),
            );
            if batch.item_keys.len() == batch.record_ids.len() {
                let unique = prove_unique_fixed_records(
                    shared,
                    keys,
                    &batch.record_ids,
                    &batch.item_keys,
                    &mut batch.unique_key_index,
                    &mut batch.alias_order,
                )?;
                aliases_proven = true;
                if unique_input && items.can_start_unique_item_batch() {
                    if let Some(unique) = unique {
                        let record_ids = &batch.record_ids;
                        let values = &mut batch.values;
                        if items.try_with_unique_item_batch(unique, |index, entry| {
                            let record_id = record_ids[index]
                                .expect("the all-hit unique batch contains a record ID");
                            let access = shared.resolve_directory_access(record_id)?;
                            apply_fixed_mutation::<CAPTURE_VALUES>(
                                adapter,
                                entry,
                                access,
                                index,
                                self.mint_resolved(record_id),
                                &mut modify,
                                values,
                            )
                        })? {
                            return Ok(());
                        }
                    }
                }
            }
            if !aliases_proven {
                validate_fixed_record_aliases(
                    shared,
                    keys,
                    &batch.record_ids,
                    &mut batch.alias_order,
                )?;
            }

            for (index, key) in keys.iter().enumerate() {
                let batched_record_id = batch.record_ids[index];
                items.with_resolved_item(
                    || {
                        let found = match batched_record_id {
                            Some(record_id) => Some(record_id),
                            // Recheck a batched miss at its sequential position.
                            // This observes a concurrent append-only publication
                            // and makes duplicate misses reuse an earlier ID.
                            None => shared.lookup(worker, key)?,
                        };
                        match found {
                            Some(record_id) => {
                                let access = shared.resolve_directory_access(record_id)?;
                                Ok(Some((TableKey::Record(record_id), access)))
                            }
                            None => Ok(None),
                        }
                    },
                    || {
                        let record_id = shared.intern_missing(worker, key)?;
                        let access = shared.resolve_directory_access(record_id)?;
                        Ok((TableKey::Record(record_id), access))
                    },
                    |entry, access| {
                        apply_fixed_mutation::<CAPTURE_VALUES>(
                            adapter,
                            entry,
                            access,
                            index,
                            self.mint_resolved(access.record_id),
                            &mut modify,
                            &mut batch.values,
                        )
                    },
                )?;
            }
            Ok(())
        });
        if let Err(error) = result {
            batch.clear();
            return Err(error);
        }

        Ok(keys.len())
    }

    fn modify_fixed_inner<'batch, const KEY_LENGTH: usize>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        keys: &[[u8; KEY_LENGTH]],
        batch: &'batch mut PointReadBatch,
        mut modify: impl for<'value> FnMut(usize, Option<&'value Value>) -> PointMutation,
        lookup_batch: impl FnOnce(&mut PointReadBatch) -> Result<(), AccessError>,
    ) -> Result<&'batch [Option<Value>], AccessError> {
        self.modify_fixed_visit_inner::<true, KEY_LENGTH>(
            txn,
            worker,
            keys,
            batch,
            |index, current| Ok(modify(index, current)),
            lookup_batch,
        )?;
        Ok(batch.results())
    }

    #[allow(
        unsafe_code,
        clippy::too_many_arguments,
        reason = "the caller supplies the external value lifetime retained by the transaction"
    )]
    unsafe fn try_modify_fixed_borrowed_inner<const KEY_LENGTH: usize>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8; KEY_LENGTH],
        batch: &mut PointReadBatch,
        output: &mut [u8],
        mut modify: impl for<'buffer> FnMut(&'buffer mut [u8], usize) -> Result<usize, AccessError>,
        lookup_batch: impl FnOnce(&mut PointReadBatch) -> Result<(), AccessError>,
    ) -> Result<Option<usize>, AccessError> {
        let keys = std::slice::from_ref(key);
        let mut replacement_len = None;
        let visited = {
            let mut apply =
                |_index: usize, current: Option<&Value>| -> Result<PointMutation, AccessError> {
                    let Some(current) = current else {
                        return Ok(PointMutation::Keep);
                    };
                    let current = current.as_ref();
                    if current.len() > output.len() {
                        return Err(CapacityError::BufferLimit.into());
                    }

                    output[..current.len()].copy_from_slice(current);
                    let length = modify(output, current.len())?;
                    if length > output.len() {
                        return Err(CapacityError::BufferLimit.into());
                    }

                    replacement_len = Some(length);
                    // The public unsafe contract keeps this exact range readable and
                    // immutable until the attempt finishes. Eligible bounded tables
                    // therefore construct `BorrowedStaged`; other sizes retain the
                    // borrowed-PUT path's owned fallback.
                    let replacement = Value::from(self.borrowed_staging_input(&output[..length]));
                    Ok(PointMutation::Put(replacement))
                };

            self.modify_fixed_visit_inner::<false, KEY_LENGTH>(
                txn,
                worker,
                keys,
                batch,
                &mut apply,
                lookup_batch,
            )
        };
        let visited = visited?;
        debug_assert_eq!(visited, 1);
        Ok(replacement_len)
    }

    #[inline]
    fn put_resolved_presence_inner<V: Into<Value>>(
        &self,
        txn: &mut Transaction<'_, Active>,
        resolved: ResolvedRecord,
        value: V,
    ) -> Result<bool, AccessError> {
        let record_id = self.validate_resolved(resolved)?;
        let adapter = self.record_resource.adapter();
        txn.with_item(
            &self.record_resource,
            TableKey::Record(record_id),
            |entry| {
                let access = self.shared().resolve_directory_access(record_id)?;
                let previous_present = adapter.prepare_resolved_presence_access(access, entry)?;
                stage_record_state(adapter, access, entry, RecordState::Live(value.into()))?;
                Ok(previous_present)
            },
        )
    }

    #[inline]
    fn put_inner<V: Into<Value>>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        value: V,
    ) -> Result<Option<Value>, AccessError> {
        self.put_inner_with_lookup(txn, worker, key, value, || {
            self.shared().lookup(worker, key)
        })
    }

    #[inline]
    fn put_presence_inner<V: Into<Value>>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        value: V,
    ) -> Result<bool, AccessError> {
        self.put_presence_inner_with_lookup(txn, worker, key, value, || {
            self.shared().lookup(worker, key)
        })
    }

    #[inline]
    fn put_presence_inner_with_lookup<V: Into<Value>>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        value: V,
        lookup: impl FnOnce() -> Result<Option<RecordId>, AccessError>,
    ) -> Result<bool, AccessError> {
        let adapter = self.record_resource.adapter();
        self.with_key_record_lookup(txn, worker, key, lookup, move |entry, access| {
            let previous_present = adapter.prepare_resolved_presence_access(access, entry)?;
            stage_record_state(adapter, access, entry, RecordState::Live(value.into()))?;
            Ok(previous_present)
        })
    }

    #[inline]
    fn put_inner_with_lookup<V: Into<Value>>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        value: V,
        lookup: impl FnOnce() -> Result<Option<RecordId>, AccessError>,
    ) -> Result<Option<Value>, AccessError> {
        self.put_inner_with_lookup_and_result(
            txn,
            worker,
            key,
            value,
            |previous| previous.cloned(),
            lookup,
        )
    }

    #[inline]
    fn put_inner_with_lookup_and_result<R, V: Into<Value>>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        value: V,
        previous_result: impl FnOnce(Option<&Value>) -> R,
        lookup: impl FnOnce() -> Result<Option<RecordId>, AccessError>,
    ) -> Result<R, AccessError> {
        let adapter = self.record_resource.adapter();
        self.with_key_record_lookup(txn, worker, key, lookup, move |entry, access| {
            let loaded = adapter.prepare_resolved_access(access, entry)?;
            let current = current_state(entry, loaded.as_ref())?;
            let previous = previous_result(current.value());
            let replacement = RecordState::Live(value.into());
            stage_record_state(adapter, access, entry, replacement)?;
            Ok(previous)
        })
    }

    #[inline]
    fn insert_inner<V: Into<Value>>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        value: V,
    ) -> Result<InsertOutcome, AccessError> {
        self.insert_inner_with_lookup(txn, worker, key, value, || {
            self.shared().lookup(worker, key)
        })
    }

    #[inline]
    fn insert_inner_with_lookup<V: Into<Value>>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        value: V,
        lookup: impl FnOnce() -> Result<Option<RecordId>, AccessError>,
    ) -> Result<InsertOutcome, AccessError> {
        self.insert_inner_with_lookup_and_result(
            txn,
            worker,
            key,
            value,
            || InsertOutcome::Inserted,
            |current| InsertOutcome::AlreadyPresent(current.clone()),
            lookup,
        )
    }

    #[inline]
    fn insert_expected_absent_inner<V: Into<Value>>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        value: V,
    ) -> Result<bool, AccessError> {
        let adapter = self.record_resource.adapter();
        self.with_key_record_lookup(
            txn,
            worker,
            key,
            || Ok(None),
            move |entry, access| {
                let previous_present = adapter.prepare_resolved_presence_access(access, entry)?;
                if previous_present {
                    return Ok(false);
                }
                stage_record_state(adapter, access, entry, RecordState::Live(value.into()))?;
                Ok(true)
            },
        )
    }

    #[inline]
    #[allow(
        clippy::too_many_arguments,
        reason = "the internal insert seam keeps lookup and result policy statically dispatched"
    )]
    fn insert_inner_with_lookup_and_result<R, V: Into<Value>>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        value: V,
        inserted_result: impl FnOnce() -> R,
        present_result: impl FnOnce(&Value) -> R,
        lookup: impl FnOnce() -> Result<Option<RecordId>, AccessError>,
    ) -> Result<R, AccessError> {
        let adapter = self.record_resource.adapter();
        self.with_key_record_lookup(txn, worker, key, lookup, move |entry, access| {
            let loaded = adapter.prepare_resolved_access(access, entry)?;
            let current = current_state(entry, loaded.as_ref())?;
            if let Some(value) = current.value() {
                return Ok(present_result(value));
            }
            let replacement = RecordState::Live(value.into());
            stage_record_state(adapter, access, entry, replacement)?;
            Ok(inserted_result())
        })
    }

    #[inline]
    fn remove_inner(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
    ) -> Result<Option<Value>, AccessError> {
        self.remove_inner_with_lookup(txn, worker, key, || self.shared().lookup(worker, key))
    }

    #[inline]
    fn remove_presence_inner(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
    ) -> Result<bool, AccessError> {
        self.remove_presence_inner_with_lookup(txn, worker, key, || {
            self.shared().lookup(worker, key)
        })
    }

    #[inline]
    fn remove_presence_inner_with_lookup(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        lookup: impl FnOnce() -> Result<Option<RecordId>, AccessError>,
    ) -> Result<bool, AccessError> {
        let adapter = self.record_resource.adapter();
        self.with_key_record_lookup(txn, worker, key, lookup, |entry, access| {
            let previous_present = adapter.prepare_resolved_presence_access(access, entry)?;
            if !previous_present {
                return Ok(false);
            }
            stage_record_state(adapter, access, entry, RecordState::tombstone())?;
            Ok(true)
        })
    }

    #[inline]
    fn remove_inner_with_lookup(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        lookup: impl FnOnce() -> Result<Option<RecordId>, AccessError>,
    ) -> Result<Option<Value>, AccessError> {
        self.remove_inner_with_lookup_and_result(
            txn,
            worker,
            key,
            |previous| previous.cloned(),
            lookup,
        )
    }

    #[inline]
    fn remove_inner_with_lookup_and_result<R>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        previous_result: impl FnOnce(Option<&Value>) -> R,
        lookup: impl FnOnce() -> Result<Option<RecordId>, AccessError>,
    ) -> Result<R, AccessError> {
        let adapter = self.record_resource.adapter();
        self.with_key_record_lookup(txn, worker, key, lookup, |entry, access| {
            let loaded = adapter.prepare_resolved_access(access, entry)?;
            let current = current_state(entry, loaded.as_ref())?;
            let previous = current.value();
            if previous.is_none() {
                return Ok(previous_result(None));
            };
            let result = previous_result(previous);
            let replacement = RecordState::tombstone();
            stage_record_state(adapter, access, entry, replacement)?;
            Ok(result)
        })
    }

    fn scan_inner(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        request: ScanRequest<'_>,
    ) -> Result<Vec<ScanRecord>, AccessError> {
        let config = self.shared().registry.config;
        let initial_capacity = request.limit.min(config.scan_chunk_records);
        let mut result = Vec::new();
        if result.try_reserve_exact(initial_capacity).is_err() {
            return self.fail_scan(txn, CapacityError::BufferLimit.into());
        }
        let mut scratch = ScanScratch::default();
        self.visit_scan_inner(txn, worker, request, &mut scratch, &mut |row| {
            if result.len() == result.capacity() {
                let remaining = request.limit.saturating_sub(result.len());
                let additional = remaining.min(config.scan_chunk_records);
                if additional == 0 || result.try_reserve_exact(additional).is_err() {
                    return Err(CapacityError::BufferLimit.into());
                }
            }
            result.push(ScanRecord {
                key: Arc::from(row.key()),
                value: row.value_snapshot().clone(),
                resolved: row.resolved(),
            });
            Ok(ScanControl::Continue)
        })?;
        Ok(result)
    }

    fn visit_scan_inner<F>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        request: ScanRequest<'_>,
        scratch: &mut ScanScratch,
        visit: &mut F,
    ) -> Result<ScanVisitOutcome, AccessError>
    where
        F: for<'row> FnMut(ScanRecordRef<'row>) -> Result<ScanControl, AccessError>,
    {
        let adapter = self.record_resource.adapter();
        self.visit_scan_records(txn, worker, request, scratch, &mut |key, entry, access| {
            let loaded = adapter.prepare_resolved_access(access, entry)?;
            let Some(value) = current_state_snapshot(entry, loaded.as_ref())?.value() else {
                return Ok(None);
            };
            visit(ScanRecordRef {
                key,
                value,
                resolved: self.mint_resolved(access.record_id),
            })
            .map(Some)
        })
    }

    fn visit_scan_bytes_inner<F>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        request: ScanRequest<'_>,
        scratch: &mut ScanScratch,
        visit: &mut F,
    ) -> Result<ScanVisitOutcome, AccessError>
    where
        F: for<'row> FnMut(ScanBytesRef<'row>) -> Result<ScanControl, AccessError>,
    {
        let adapter = self.record_resource.adapter();
        self.visit_scan_records(txn, worker, request, scratch, &mut |key, entry, access| {
            adapter.visit_resolved_access_bytes(access, entry, |value| {
                let Some(value) = value else {
                    return Ok(None);
                };
                visit(ScanBytesRef {
                    key,
                    value,
                    resolved: self.mint_resolved(access.record_id),
                })
                .map(Some)
            })?
        })
    }

    fn visit_scan_bytes_trusted_inner<F>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        request: ScanRequest<'_>,
        scratch: &mut ScanScratch,
        visit: &mut F,
    ) -> Result<ScanVisitOutcome, AccessError>
    where
        F: for<'row> FnMut(ScanBytesRef<'row>) -> ScanControl,
    {
        if request.limit == 0 || range_is_empty(request.lower, request.upper) {
            return Ok(ScanVisitOutcome {
                visited: 0,
                stopped: false,
            });
        }

        // Existing local record state may change liveness, values, and which
        // rows satisfy the logical limit. The ordinary path is the only one
        // that can compose those intents with committed directory rows.
        if self.shared().record_token_mode != RecordTokenMode::DirectRecordPointer
            || !self.shared().registry.config.trusted_scan_value_generation
            || txn.has_items_for(&self.record_resource)
        {
            return self
                .visit_scan_bytes_inner(txn, worker, request, scratch, &mut |row| Ok(visit(row)));
        }

        let config = self.shared().registry.config;
        if config.scan_chunk_records == 0
            || config.max_scan_chunks == 0
            || config.scan_initial_key_arena_bytes > config.scan_max_key_arena_bytes
        {
            return self.fail_scan(txn, CapacityError::BufferLimit.into());
        }

        let scan = self.scan_resource.adapter();
        let fast_outcome = txn.with_item(
            &self.scan_resource,
            TableKey::DirectoryGeneration,
            |entry| {
                self.shared().ensure_healthy()?;
                let structural = self.shared().try_scan_structure()?;
                let observation = scan.observe_trusted_scan_generation(entry)?;
                let outcome = self.visit_trusted_scan_records_while_structurally_stable(
                    worker, request, config, scratch, visit,
                );
                let result = match outcome {
                    Ok(outcome) => {
                        self.shared().ensure_healthy()?;
                        if self.shared().scan_generation.load(Ordering::Acquire)
                            != observation.generation
                        {
                            Err(Conflict::ReadValidation.into())
                        } else {
                            Ok(outcome)
                        }
                    }
                    Err(error) => Err(error),
                };
                drop(structural);
                result
            },
        )?;
        Ok(fast_outcome)
    }

    #[cfg(not(test))]
    #[allow(clippy::too_many_arguments)]
    fn visit_bounded_forward_values_keyless_inner<F>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        lower: &[u8],
        upper: &[u8],
        limit: usize,
        scratch: &mut ScanScratch,
        visit: &mut F,
    ) -> Result<ScanVisitOutcome, AccessError>
    where
        F: for<'value> FnMut(&'value [u8], ResolvedRecord) -> ScanControl,
    {
        if limit == 0 || lower >= upper {
            return Ok(ScanVisitOutcome {
                visited: 0,
                stopped: false,
            });
        }
        let config = self.shared().registry.config;
        if config.scan_chunk_records == 0
            || config.max_scan_chunks == 0
            || config.scan_initial_key_arena_bytes > config.scan_max_key_arena_bytes
        {
            return self.fail_scan(txn, CapacityError::BufferLimit.into());
        }

        let scan = self.scan_resource.adapter();
        let outcome = txn.with_item(
            &self.scan_resource,
            TableKey::DirectoryGeneration,
            |entry| {
                self.shared().ensure_healthy()?;
                let structural = self.shared().try_scan_structure()?;
                let observation = scan.observe_trusted_scan_generation(entry)?;
                let outcome = self.visit_bounded_forward_values_while_structurally_stable(
                    worker, lower, upper, limit, config, scratch, visit,
                );
                let result = match outcome {
                    Ok(outcome) => {
                        self.shared().ensure_healthy()?;
                        if self.shared().scan_generation.load(Ordering::Acquire)
                            != observation.generation
                        {
                            Err(Conflict::ReadValidation.into())
                        } else {
                            Ok(outcome)
                        }
                    }
                    Err(error) => Err(error),
                };
                drop(structural);
                result
            },
        )?;
        Ok(outcome)
    }

    #[cfg(not(test))]
    #[allow(clippy::too_many_arguments)]
    fn visit_bounded_forward_values_while_structurally_stable<F>(
        &self,
        worker: &Worker,
        lower: &[u8],
        upper: &[u8],
        limit: usize,
        config: TableConfig,
        scratch: &mut ScanScratch,
        visit: &mut F,
    ) -> Result<ScanVisitOutcome, AccessError>
    where
        F: for<'value> FnMut(&'value [u8], ResolvedRecord) -> ScanControl,
    {
        let mut resume_key: Option<Box<[u8]>> = None;
        let request_arena_hint = limit.saturating_mul(64).max(1);
        let mut continuation_capacity = config.scan_initial_key_arena_bytes.min(request_arena_hint);
        let mut physical_records = 0_usize;
        let mut chunks = 0_usize;
        let mut visited = 0_usize;
        let native = &mut scratch.native;
        let adapter = self.record_resource.adapter();

        loop {
            if chunks >= config.max_scan_chunks {
                return Err(CapacityError::BufferLimit.into());
            }
            chunks += 1;
            let chunk_lower = resume_key.as_deref().unwrap_or(lower);
            let entry_capacity = config.scan_chunk_records.min(limit - visited).max(1);
            self.shared().ensure_healthy()?;
            let chunk = self
                .shared()
                .directory
                .scan_record_ids_bounded(
                    Some(worker),
                    chunk_lower,
                    upper,
                    entry_capacity,
                    continuation_capacity,
                    native,
                )
                .inspect_err(|error| self.shared().note_access_error(error))?;

            physical_records = physical_records
                .checked_add(chunk.len())
                .filter(|count| *count <= config.max_scan_physical_records)
                .ok_or(CapacityError::BufferLimit)?;

            for record_id in chunk.record_ids() {
                let access = self.shared().resolve_directory_access(record_id)?;
                let control = adapter.visit_untracked_resolved_access_bytes(access, |value| {
                    value.map(|value| visit(value, self.mint_resolved(access.record_id)))
                })?;
                let Some(control) = control else {
                    continue;
                };
                visited += 1;
                if control == ScanControl::Stop {
                    return Ok(ScanVisitOutcome {
                        visited,
                        stopped: true,
                    });
                }
                if visited == limit {
                    return Ok(ScanVisitOutcome {
                        visited,
                        stopped: false,
                    });
                }
            }

            match (chunk.stop_reason(), chunk.resume()) {
                (ScanStopReason::End, NativeBoundedRecordIdScanResume::None) => break,
                (
                    ScanStopReason::EntryCapacity,
                    NativeBoundedRecordIdScanResume::InclusiveNext(key),
                ) => {
                    if physical_records >= config.max_scan_physical_records {
                        return Err(CapacityError::BufferLimit.into());
                    }
                    resume_key = Some(key.into());
                }
                (
                    ScanStopReason::KeyArenaCapacity,
                    NativeBoundedRecordIdScanResume::UnchangedInput,
                ) => {
                    if chunk.next_key_bytes_required() == 0 {
                        return Err(table_fault(
                            "bounded RecordId scan reported no continuation size",
                        ));
                    }
                    let doubled = continuation_capacity.saturating_mul(2);
                    let grown = doubled.max(chunk.next_key_bytes_required()).max(1);
                    if grown > config.scan_max_key_arena_bytes || grown <= continuation_capacity {
                        return Err(CapacityError::BufferLimit.into());
                    }
                    continuation_capacity = grown;
                }
                _ => {
                    return Err(table_fault(
                        "bounded RecordId scan stop metadata is inconsistent",
                    ));
                }
            }
        }

        Ok(ScanVisitOutcome {
            visited,
            stopped: false,
        })
    }

    fn visit_trusted_scan_records_while_structurally_stable<F>(
        &self,
        worker: Option<&Worker>,
        request: ScanRequest<'_>,
        config: TableConfig,
        scratch: &mut ScanScratch,
        visit: &mut F,
    ) -> Result<ScanVisitOutcome, AccessError>
    where
        F: for<'row> FnMut(ScanBytesRef<'row>) -> ScanControl,
    {
        let mut resume_key: Option<Box<[u8]>> = None;
        let request_arena_hint = request.limit.saturating_mul(64).max(1);
        let mut arena_capacity = config.scan_initial_key_arena_bytes.min(request_arena_hint);
        let mut physical_records = 0_usize;
        let mut chunks = 0_usize;
        let mut visited = 0_usize;
        let native = &mut scratch.native;
        let adapter = self.record_resource.adapter();

        loop {
            if chunks >= config.max_scan_chunks {
                return Err(CapacityError::BufferLimit.into());
            }
            chunks += 1;

            let (lower, upper) = resumed_bounds(request, resume_key.as_deref());
            let entry_capacity = config
                .scan_chunk_records
                .min(request.limit - visited)
                .max(1);
            let directory_request = DirectoryScanRequest {
                direction: request.direction,
                lower,
                upper,
                entry_capacity,
                key_arena_capacity: arena_capacity,
            };
            self.shared().ensure_healthy()?;
            let chunk = self
                .shared()
                .directory
                .scan(
                    worker,
                    directory_request,
                    native,
                    self.shared().record_token_mode == RecordTokenMode::DirectRecordPointer,
                )
                .inspect_err(|error| self.shared().note_access_error(error))?;

            physical_records = physical_records
                .checked_add(chunk.len())
                .filter(|count| *count <= config.max_scan_physical_records)
                .ok_or(CapacityError::BufferLimit)?;

            for copied in chunk.entries() {
                let access = self.shared().resolve_directory_access(copied.record_id)?;
                let control = adapter.visit_untracked_resolved_access_bytes(access, |value| {
                    value.map(|value| {
                        visit(ScanBytesRef {
                            key: copied.key,
                            value,
                            resolved: self.mint_resolved(access.record_id),
                        })
                    })
                })?;
                let Some(control) = control else {
                    continue;
                };
                visited += 1;
                if control == ScanControl::Stop {
                    return Ok(ScanVisitOutcome {
                        visited,
                        stopped: true,
                    });
                }
                if visited == request.limit {
                    return Ok(ScanVisitOutcome {
                        visited,
                        stopped: false,
                    });
                }
            }

            match (chunk.stop_reason(), chunk.resume()) {
                (ScanStopReason::End, DirectoryScanResumeRef::None) => break,
                (
                    ScanStopReason::EntryCapacity | ScanStopReason::KeyArenaCapacity,
                    DirectoryScanResumeRef::Exclusive(key),
                ) => {
                    if physical_records >= config.max_scan_physical_records {
                        return Err(CapacityError::BufferLimit.into());
                    }
                    resume_key = Some(key.into());
                }
                (ScanStopReason::KeyArenaCapacity, DirectoryScanResumeRef::UnchangedInput) => {
                    if chunk.next_key_bytes_required() == 0 {
                        return Err(table_fault("arena-limited scan reported no required size"));
                    }
                    let doubled = arena_capacity.saturating_mul(2);
                    let grown = doubled.max(chunk.next_key_bytes_required()).max(1);
                    if grown > config.scan_max_key_arena_bytes || grown <= arena_capacity {
                        return Err(CapacityError::BufferLimit.into());
                    }
                    arena_capacity = grown;
                }
                (ScanStopReason::EntryCapacity, DirectoryScanResumeRef::UnchangedInput) => {
                    return Err(CapacityError::BufferLimit.into());
                }
                _ => {
                    return Err(table_fault("directory scan stop metadata is inconsistent"));
                }
            }
        }
        Ok(ScanVisitOutcome {
            visited,
            stopped: false,
        })
    }

    fn visit_scan_records<F>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        request: ScanRequest<'_>,
        scratch: &mut ScanScratch,
        visit_record: &mut F,
    ) -> Result<ScanVisitOutcome, AccessError>
    where
        F: for<'entry, 'record> FnMut(
            &[u8],
            &mut Entry<'entry, TableAdapter>,
            RecordAccess<'record>,
        ) -> Result<Option<ScanControl>, AccessError>,
    {
        if request.limit == 0 || range_is_empty(request.lower, request.upper) {
            return Ok(ScanVisitOutcome {
                visited: 0,
                stopped: false,
            });
        }

        let config = self.shared().registry.config;
        if config.scan_chunk_records == 0
            || config.max_scan_chunks == 0
            || config.scan_initial_key_arena_bytes > config.scan_max_key_arena_bytes
        {
            return self.fail_scan(txn, CapacityError::BufferLimit.into());
        }

        let directory = self.directory_resource.adapter();
        let structural = txn.with_item(
            &self.directory_resource,
            TableKey::DirectoryGeneration,
            |entry| {
                self.shared().ensure_healthy()?;
                let structural = self.shared().try_scan_structure()?;
                directory.observe_directory_generation(entry)?;
                Ok(structural)
            },
        )?;
        let result = self.visit_scan_records_while_structurally_stable(
            txn,
            worker,
            request,
            config,
            scratch,
            visit_record,
        );
        drop(structural);
        result
    }

    fn visit_scan_records_while_structurally_stable<F>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        request: ScanRequest<'_>,
        config: TableConfig,
        scratch: &mut ScanScratch,
        visit_record: &mut F,
    ) -> Result<ScanVisitOutcome, AccessError>
    where
        F: for<'entry, 'record> FnMut(
            &[u8],
            &mut Entry<'entry, TableAdapter>,
            RecordAccess<'record>,
        ) -> Result<Option<ScanControl>, AccessError>,
    {
        let mut resume_key: Option<Box<[u8]>> = None;
        // A small bounded result rarely needs the table-wide default arena.
        // Begin with up to 64 key bytes per requested live row and retain the
        // existing exact-size growth path when a key or tombstone-heavy range
        // needs more. This avoids allocating and zeroing 16 KiB for a limit-1
        // scan while preserving every configured hard maximum.
        let request_arena_hint = request.limit.saturating_mul(64).max(1);
        let mut arena_capacity = config.scan_initial_key_arena_bytes.min(request_arena_hint);
        let mut physical_records = 0_usize;
        let mut chunks = 0_usize;
        let mut visited = 0_usize;
        let ScanScratch {
            native,
            item_keys,
            unique_order,
        } = scratch;

        loop {
            if chunks >= config.max_scan_chunks {
                return self.fail_scan(txn, CapacityError::BufferLimit.into());
            }
            chunks += 1;

            let (lower, upper) = resumed_bounds(request, resume_key.as_deref());
            // Do not ask the native directory to copy rows beyond the number
            // that can still contribute to this bounded logical result.
            // Tombstones may require another chunk, but live TPC-C scans avoid
            // copying a full default chunk for limits such as 1 or 15.
            let entry_capacity = config
                .scan_chunk_records
                .min(request.limit - visited)
                .max(1);
            let directory_request = DirectoryScanRequest {
                direction: request.direction,
                lower,
                upper,
                entry_capacity,
                key_arena_capacity: arena_capacity,
            };
            if let Err(error) = self.shared().ensure_healthy() {
                return self.fail_scan(txn, error);
            }
            let chunk = match self.shared().directory.scan(
                worker,
                directory_request,
                native,
                self.shared().record_token_mode == RecordTokenMode::DirectRecordPointer,
            ) {
                Ok(chunk) => chunk,
                Err(error) => {
                    self.shared().note_access_error(&error);
                    return self.fail_scan(txn, error);
                }
            };

            let next_physical = physical_records
                .checked_add(chunk.len())
                .ok_or(CapacityError::BufferLimit);
            let next_physical = match next_physical {
                Ok(count) if count <= config.max_scan_physical_records => count,
                _ => return self.fail_scan(txn, CapacityError::BufferLimit.into()),
            };
            physical_records = next_physical;

            if let Some(outcome) = self.visit_copied_scan_chunk(
                txn,
                &chunk,
                item_keys,
                unique_order,
                &mut visited,
                request.limit,
                visit_record,
            )? {
                return Ok(outcome);
            }

            match (chunk.stop_reason(), chunk.resume()) {
                (ScanStopReason::End, DirectoryScanResumeRef::None) => break,
                (
                    ScanStopReason::EntryCapacity | ScanStopReason::KeyArenaCapacity,
                    DirectoryScanResumeRef::Exclusive(key),
                ) => {
                    if physical_records >= config.max_scan_physical_records {
                        return self.fail_scan(txn, CapacityError::BufferLimit.into());
                    }
                    resume_key = Some(key.into());
                }
                (ScanStopReason::KeyArenaCapacity, DirectoryScanResumeRef::UnchangedInput) => {
                    if chunk.next_key_bytes_required() == 0 {
                        return self.fail_scan(
                            txn,
                            table_fault("arena-limited scan reported no required size"),
                        );
                    }
                    let doubled = arena_capacity.saturating_mul(2);
                    let grown = doubled.max(chunk.next_key_bytes_required()).max(1);
                    if grown > config.scan_max_key_arena_bytes || grown <= arena_capacity {
                        return self.fail_scan(txn, CapacityError::BufferLimit.into());
                    }
                    arena_capacity = grown;
                }
                (ScanStopReason::EntryCapacity, DirectoryScanResumeRef::UnchangedInput) => {
                    return self.fail_scan(txn, CapacityError::BufferLimit.into());
                }
                _ => {
                    return self.fail_scan(
                        txn,
                        table_fault("directory scan stop metadata is inconsistent"),
                    );
                }
            }
        }
        Ok(ScanVisitOutcome {
            visited,
            stopped: false,
        })
    }

    #[allow(clippy::too_many_arguments)]
    #[inline]
    fn visit_copied_scan_chunk<F>(
        &self,
        txn: &mut Transaction<'_, Active>,
        chunk: &DirectoryScanChunk<'_>,
        item_keys: &mut Vec<TableKey>,
        unique_order: &mut Vec<usize>,
        visited: &mut usize,
        limit: usize,
        visit_record: &mut F,
    ) -> Result<Option<ScanVisitOutcome>, AccessError>
    where
        F: for<'entry, 'record> FnMut(
            &[u8],
            &mut Entry<'entry, TableAdapter>,
            RecordAccess<'record>,
        ) -> Result<Option<ScanControl>, AccessError>,
    {
        if chunk.len() == 0 {
            return Ok(None);
        }

        txn.with_item_session(&self.record_resource, |items| {
            if items.can_start_unique_item_batch() {
                item_keys.clear();
                let unique = if item_keys.try_reserve_exact(chunk.len()).is_ok() {
                    item_keys.extend(
                        chunk
                            .entries()
                            .map(|copied| TableKey::Record(copied.record_id)),
                    );
                    // Auxiliary proof storage must not add a new failure point
                    // before callbacks that the scalar scan would have run.
                    // If it cannot grow, retain the ordinary scalar behavior.
                    prove_unique_keys(item_keys, unique_order).unwrap_or(None)
                } else {
                    None
                };

                if let Some(unique) = unique {
                    let mut entries = chunk.entries();
                    let mut completion = None;
                    let outcome =
                        items.try_with_unique_item_batch_while(unique, |index, entry| {
                            let copied = entries
                                .next()
                                .expect("the uniqueness input mirrors the copied scan chunk");
                            debug_assert_eq!(item_keys[index], TableKey::Record(copied.record_id));
                            let access =
                                self.shared().resolve_directory_access(copied.record_id)?;
                            let control = visit_record(copied.key, entry, access)?;
                            let Some(control) = control else {
                                return Ok(ItemBatchControl::Continue);
                            };

                            *visited += 1;
                            let stopped = control == ScanControl::Stop;
                            if stopped || *visited == limit {
                                completion = Some(ScanVisitOutcome {
                                    visited: *visited,
                                    stopped,
                                });
                                Ok(ItemBatchControl::Stop)
                            } else {
                                Ok(ItemBatchControl::Continue)
                            }
                        })?;

                    match outcome {
                        ItemBatchOutcome::Ineligible => {}
                        ItemBatchOutcome::Complete { appended } => {
                            debug_assert_eq!(appended, chunk.len());
                            debug_assert!(completion.is_none());
                            return Ok(None);
                        }
                        ItemBatchOutcome::Stopped { appended } => {
                            debug_assert!(appended <= chunk.len());
                            return Ok(Some(
                                completion.expect("a stopped scan batch records its completion"),
                            ));
                        }
                    }
                }
            }

            for copied in chunk.entries() {
                let record_id = copied.record_id;
                let control = items.with_resolved_item(
                    || {
                        let access = self.shared().resolve_directory_access(record_id)?;
                        Ok(Some((TableKey::Record(record_id), access)))
                    },
                    || Err(table_fault("stable directory record disappeared")),
                    |entry, access| visit_record(copied.key, entry, access),
                )?;
                if let Some(control) = control {
                    *visited += 1;
                    if control == ScanControl::Stop {
                        return Ok(Some(ScanVisitOutcome {
                            visited: *visited,
                            stopped: true,
                        }));
                    }
                    if *visited == limit {
                        return Ok(Some(ScanVisitOutcome {
                            visited: *visited,
                            stopped: false,
                        }));
                    }
                }
            }
            Ok(None)
        })
    }

    fn fail_scan<R>(
        &self,
        txn: &mut Transaction<'_, Active>,
        error: AccessError,
    ) -> Result<R, AccessError> {
        txn.with_item(
            &self.directory_resource,
            TableKey::DirectoryGeneration,
            |_entry| Err(error),
        )
    }

    #[inline]
    fn with_key_record_lookup<R>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        lookup: impl FnOnce() -> Result<Option<RecordId>, AccessError>,
        operation: impl for<'entry, 'record> FnOnce(
            &mut Entry<'entry, TableAdapter>,
            RecordAccess<'record>,
        ) -> Result<R, AccessError>,
    ) -> Result<R, AccessError> {
        let shared = self.shared();
        txn.with_resolved_item(
            &self.record_resource,
            || {
                let found = lookup()?;
                match found {
                    Some(record_id) => {
                        let access = shared.resolve_directory_access(record_id)?;
                        Ok(Some((TableKey::Record(record_id), access)))
                    }
                    None => Ok(None),
                }
            },
            || {
                let record_id = shared.intern_missing(worker, key)?;
                let access = shared.resolve_directory_access(record_id)?;
                Ok((TableKey::Record(record_id), access))
            },
            operation,
        )
    }

    #[inline(always)]
    fn mint_resolved(&self, record_id: RecordId) -> ResolvedRecord {
        ResolvedRecord {
            runtime_id: self.record_resource.runtime_id(),
            object_id: self.record_resource.object_id(),
            record_id,
        }
    }

    #[inline(always)]
    fn validate_resolved(&self, resolved: ResolvedRecord) -> Result<RecordId, AccessError> {
        if resolved.runtime_id != self.record_resource.runtime_id() {
            return Err(InvalidUse::WrongRuntime.into());
        }
        if resolved.object_id != self.record_resource.object_id() {
            return Err(InvalidUse::ResourceTypeMismatch.into());
        }
        Ok(resolved.record_id)
    }

    #[inline(always)]
    fn bind_resolved(
        &self,
        resolved: ResolvedRecord,
    ) -> Result<ValidatedResolved<'_>, AccessError> {
        let record_id = self.validate_resolved(resolved)?;
        Ok(ValidatedResolved {
            record_id,
            _table: std::marker::PhantomData,
        })
    }

    #[inline(always)]
    fn try_bind_resolved(&self, resolved: ResolvedRecord) -> Option<ValidatedResolved<'_>> {
        if !self.owns_resolved(resolved) {
            return None;
        }
        Some(ValidatedResolved {
            record_id: resolved.record_id,
            _table: std::marker::PhantomData,
        })
    }

    fn prepare_hinted_fixed_lookup<const KEY_LENGTH: usize>(
        &self,
        keys: &[[u8; KEY_LENGTH]],
        hints: &[Option<ResolvedRecord>],
        missing_keys: &[[u8; KEY_LENGTH]],
        missing_positions: &[usize],
        batch: &mut PointReadBatch,
    ) -> Result<(), AccessError> {
        if hints.len() != keys.len() || missing_keys.len() != missing_positions.len() {
            return Err(InvalidUse::IllegalItemState.into());
        }

        let mut next_missing = 0;
        for (index, (key, hint)) in keys.iter().zip(hints).enumerate() {
            match hint {
                Some(resolved) => batch
                    .record_ids
                    .push(Some(self.validate_resolved(*resolved)?)),
                None => {
                    if missing_positions.get(next_missing) != Some(&index)
                        || missing_keys.get(next_missing) != Some(key)
                    {
                        return Err(InvalidUse::IllegalItemState.into());
                    }
                    batch.record_ids.push(None);
                    next_missing += 1;
                }
            }
        }
        if next_missing != missing_keys.len() {
            return Err(InvalidUse::IllegalItemState.into());
        }
        Ok(())
    }

    #[cfg(not(test))]
    #[inline]
    fn lookup_in_read_scope<'session>(
        &'session self,
        worker: &'session Worker,
        read_scope: &mut Option<NativeReadScope<'session, 'session>>,
        key: &[u8],
    ) -> Result<Option<RecordId>, AccessError> {
        let shared = self.shared();
        shared.ensure_healthy()?;
        if read_scope.is_none() {
            let Directory::Native(directory) = &shared.directory;
            let opened = directory
                .tree
                .read_scope(worker)
                .map_err(map_masstree_error)
                .inspect_err(|error| shared.note_access_error(error))?;
            *read_scope = Some(opened);
        }

        let found = read_scope
            .as_mut()
            .expect("a point session just opened its read scope")
            .get(key)
            .map_err(map_masstree_error)
            .inspect_err(|error| shared.note_access_error(error))?;
        if found.is_none() {
            self.close_read_scope(read_scope)?;
        }
        Ok(found)
    }

    #[cfg(not(test))]
    fn lookup_fixed_in_read_scope<'session, const KEY_LENGTH: usize>(
        &'session self,
        worker: &'session Worker,
        read_scope: &mut Option<NativeReadScope<'session, 'session>>,
        keys: &[[u8; KEY_LENGTH]],
        batch: &mut PointReadBatch,
    ) -> Result<(), AccessError> {
        let shared = self.shared();
        shared.ensure_healthy()?;
        let used_existing_scope = read_scope.is_some();
        if let Some(read_scope) = read_scope.as_mut() {
            read_scope
                .get_fixed(keys, &mut batch.directory_results)
                .map_err(map_masstree_error)
                .inspect_err(|error| shared.note_access_error(error))?;
        } else {
            let Directory::Native(directory) = &shared.directory;
            directory
                .tree
                .get_fixed(worker, keys, &mut batch.directory_results)
                .map_err(map_masstree_error)
                .inspect_err(|error| shared.note_access_error(error))?;
        }
        batch.record_ids.extend(
            batch
                .directory_results
                .iter()
                .map(|result| result.record_id()),
        );
        // Masstree insertion is forbidden while this worker owns an RCU read
        // scope. Close a preexisting scalar scope before transactional
        // processing when any result may need lookup/intern fallback. The
        // one-shot fixed call owns and closes its native scope internally.
        if used_existing_scope && batch.record_ids.iter().any(Option::is_none) {
            self.close_read_scope(read_scope)?;
        }
        Ok(())
    }

    #[cfg(not(test))]
    #[allow(
        clippy::too_many_arguments,
        reason = "the native lookup needs the original batch, validated hints, and caller-owned compact miss slices"
    )]
    fn lookup_fixed_hinted_in_read_scope<'session, const KEY_LENGTH: usize>(
        &'session self,
        worker: &'session Worker,
        read_scope: &mut Option<NativeReadScope<'session, 'session>>,
        keys: &[[u8; KEY_LENGTH]],
        hints: &[Option<ResolvedRecord>],
        missing_keys: &[[u8; KEY_LENGTH]],
        missing_positions: &[usize],
        batch: &mut PointReadBatch,
    ) -> Result<(), AccessError> {
        self.prepare_hinted_fixed_lookup(keys, hints, missing_keys, missing_positions, batch)?;
        if missing_keys.is_empty() {
            return Ok(());
        }

        let shared = self.shared();
        shared.ensure_healthy()?;
        let used_existing_scope = read_scope.is_some();
        if let Some(read_scope) = read_scope.as_mut() {
            read_scope
                .get_fixed(missing_keys, &mut batch.directory_results)
                .map_err(map_masstree_error)
                .inspect_err(|error| shared.note_access_error(error))?;
        } else {
            let Directory::Native(directory) = &shared.directory;
            directory
                .tree
                .get_fixed(worker, missing_keys, &mut batch.directory_results)
                .map_err(map_masstree_error)
                .inspect_err(|error| shared.note_access_error(error))?;
        }
        if batch.directory_results.len() != missing_positions.len() {
            return Err(table_fault(
                "hinted fixed point lookup returned the wrong result count",
            ));
        }
        for (&index, result) in missing_positions.iter().zip(&batch.directory_results) {
            batch.record_ids[index] = result.record_id();
        }
        // A preexisting scalar scope must close before the ordinary fallback
        // interns any physical tombstone for a directory miss. A one-shot
        // fixed lookup owns and closes its native scope internally.
        if used_existing_scope
            && missing_positions
                .iter()
                .any(|&index| batch.record_ids[index].is_none())
        {
            self.close_read_scope(read_scope)?;
        }
        Ok(())
    }

    #[cfg(not(test))]
    fn close_read_scope(
        &self,
        read_scope: &mut Option<NativeReadScope<'_, '_>>,
    ) -> Result<(), AccessError> {
        let Some(read_scope) = read_scope.take() else {
            return Ok(());
        };
        read_scope
            .close()
            .map_err(map_masstree_error)
            .inspect_err(|error| self.shared().note_access_error(error))
    }

    #[cfg(test)]
    #[inline]
    fn staging_value(&self, bytes: &[u8]) -> Value {
        Value::from_staging_slice(bytes, self.shared().registry.config.bounded_atomic_values)
    }

    #[inline(always)]
    fn staging_input<'value>(&self, bytes: &'value [u8]) -> StagingValue<'value> {
        StagingValue {
            bytes,
            bounded_atomic_values: self.shared().registry.config.bounded_atomic_values,
        }
    }

    #[inline(always)]
    fn borrowed_staging_input<'value>(&self, bytes: &'value [u8]) -> BorrowedStagingValue<'value> {
        BorrowedStagingValue {
            bytes,
            bounded_atomic_values: self.shared().registry.config.bounded_atomic_values,
        }
    }

    #[inline]
    fn shared(&self) -> &TableShared {
        self.record_resource.adapter().table.as_ref()
    }

    #[cfg(test)]
    fn new_memory(runtime: &Arc<Runtime>, config: TableConfig) -> Self {
        Self::with_directory(
            runtime,
            Directory::Memory(MemoryDirectory::default()),
            config,
        )
        .expect("test table registration must succeed")
    }

    #[cfg(test)]
    fn new_memory_direct(runtime: &Arc<Runtime>, config: TableConfig) -> Self {
        Self::with_directory_mode(
            runtime,
            Directory::Memory(MemoryDirectory::default()),
            config,
            RecordTokenMode::DirectRecordPointer,
        )
        .expect("test direct-token table registration must succeed")
    }

    #[cfg(test)]
    fn new_memory_with_first_miss_barrier(
        runtime: &Arc<Runtime>,
        config: TableConfig,
        barrier: Arc<std::sync::Barrier>,
    ) -> Self {
        Self::with_directory(
            runtime,
            Directory::Memory(MemoryDirectory::with_first_miss_barrier(barrier)),
            config,
        )
        .expect("test table registration must succeed")
    }

    #[cfg(test)]
    fn new_memory_with_candidate_reservation_pause(
        runtime: &Arc<Runtime>,
        config: TableConfig,
        reserved: Arc<std::sync::Barrier>,
        resume: Arc<std::sync::Barrier>,
    ) -> Self {
        Self::with_directory(
            runtime,
            Directory::Memory(MemoryDirectory::with_candidate_reservation_pause(
                reserved, resume,
            )),
            config,
        )
        .expect("test table registration must succeed")
    }
}

impl<'session, 'context> PointSession<'session, 'context> {
    /// Reads the staged value or a committed value reloaded at the record's
    /// first observed OCC generation.
    #[inline]
    pub fn get(&mut self, key: &[u8]) -> Result<Option<Value>, AccessError> {
        #[cfg(not(test))]
        {
            let table = self.table;
            let worker = self.worker;
            let read_scope = &mut self.read_scope;
            table.visit_get_inner_with_lookup(
                &mut *self.transaction,
                Some(worker),
                key,
                |value| value.cloned(),
                || table.lookup_in_read_scope(worker, read_scope, key),
            )
        }
        #[cfg(test)]
        {
            self.table
                .get_inner(&mut *self.transaction, Some(self.worker), key)
        }
    }

    /// Reads a contiguous batch of equally sized binary keys.
    ///
    /// The append-only directory lookups share one native boundary crossing,
    /// then each key is processed in input order as an ordinary STO read. If
    /// no scalar read scope is active, the native structural and RCU guards
    /// end in that same boundary call. Duplicate keys reuse the same
    /// transaction item, staged values retain read-your-writes behavior, and
    /// every committed snapshot receives the scalar path's
    /// observe-and-validate checks. If the initial directory batch contains a
    /// miss, any active native read scope is closed before an item can intern
    /// that key. Reuse `batch` across attempts to avoid allocation.
    #[inline]
    pub fn get_fixed<'batch, const KEY_LENGTH: usize>(
        &mut self,
        keys: &[[u8; KEY_LENGTH]],
        batch: &'batch mut PointReadBatch,
    ) -> Result<&'batch [Option<Value>], AccessError> {
        #[cfg(not(test))]
        {
            let table = self.table;
            let worker = self.worker;
            let read_scope = &mut self.read_scope;
            table.get_fixed_inner(&mut *self.transaction, Some(worker), keys, batch, |batch| {
                table.lookup_fixed_in_read_scope(worker, read_scope, keys, batch)
            })
        }
        #[cfg(test)]
        {
            let table = self.table;
            table.get_fixed_inner(
                &mut *self.transaction,
                Some(self.worker),
                keys,
                batch,
                |batch| {
                    for key in keys {
                        batch.push_record_id(table.shared().lookup(None, key)?);
                    }
                    Ok(())
                },
            )
        }
    }

    /// Visits a contiguous batch of equally sized binary keys without
    /// retaining owned snapshots.
    ///
    /// `visit` runs exactly once per key, in input order, with that key's
    /// current transaction-local value. Its borrow is valid only for that
    /// callback invocation. Duplicate keys preserve scalar read-your-writes
    /// behavior, while an all-present unique batch may use the transaction's
    /// contiguous typed-item lane. On success, the returned count equals
    /// `keys.len()` and [`PointReadBatch::results`] is empty.
    ///
    /// If access fails after an input prefix was visited, those callback side
    /// effects are not rolled back and the transaction is doomed. A callback
    /// unwind likewise leaves the transaction doomed.
    ///
    /// A borrowed value cannot escape its callback:
    ///
    /// ```compile_fail
    /// use sto_masstree::{PointReadBatch, PointSession, Value};
    ///
    /// fn retain_value<'value>(
    ///     session: &mut PointSession<'_, '_>,
    ///     batch: &mut PointReadBatch,
    ///     retained: &mut Option<&'value Value>,
    /// ) {
    ///     let keys = [[0_u8; 8]];
    ///     session
    ///         .visit_fixed(&keys, batch, |_index, value| *retained = value)
    ///         .unwrap();
    /// }
    /// ```
    #[inline]
    pub fn visit_fixed<const KEY_LENGTH: usize>(
        &mut self,
        keys: &[[u8; KEY_LENGTH]],
        batch: &mut PointReadBatch,
        visit: impl for<'value> FnMut(usize, Option<&'value Value>),
    ) -> Result<usize, AccessError> {
        #[cfg(not(test))]
        {
            let table = self.table;
            let worker = self.worker;
            let read_scope = &mut self.read_scope;
            table.visit_fixed_inner::<false, KEY_LENGTH>(
                &mut *self.transaction,
                Some(worker),
                keys,
                batch,
                visit,
                |batch| table.lookup_fixed_in_read_scope(worker, read_scope, keys, batch),
            )
        }
        #[cfg(test)]
        {
            let table = self.table;
            table.visit_fixed_inner::<false, KEY_LENGTH>(
                &mut *self.transaction,
                Some(self.worker),
                keys,
                batch,
                visit,
                |batch| {
                    for key in keys {
                        batch.push_record_id(table.shared().lookup(None, key)?);
                    }
                    Ok(())
                },
            )
        }
    }

    /// Visits fixed-width keys through operation-scoped byte leases.
    ///
    /// This has the same ordering, duplicate, miss, failure, and transaction
    /// semantics as [`Self::visit_fixed`], but exposes the stored byte slice
    /// directly. In particular, a committed shared value remains protected by
    /// the storage guard only for the callback invocation; no owned [`Value`]
    /// (and therefore no shared-reference clone) is created for the visitor.
    /// [`PointReadBatch::results`] remains empty on success.
    ///
    /// A borrowed byte slice cannot escape its callback:
    ///
    /// ```compile_fail
    /// use sto_masstree::{PointReadBatch, PointSession};
    ///
    /// fn retain_bytes<'value>(
    ///     session: &mut PointSession<'_, '_>,
    ///     batch: &mut PointReadBatch,
    ///     retained: &mut Option<&'value [u8]>,
    /// ) {
    ///     let keys = [[0_u8; 8]];
    ///     session
    ///         .visit_fixed_bytes(&keys, batch, |_index, value| *retained = value)
    ///         .unwrap();
    /// }
    /// ```
    #[inline]
    pub fn visit_fixed_bytes<const KEY_LENGTH: usize>(
        &mut self,
        keys: &[[u8; KEY_LENGTH]],
        batch: &mut PointReadBatch,
        visit: impl for<'value> FnMut(usize, Option<&'value [u8]>),
    ) -> Result<usize, AccessError> {
        #[cfg(not(test))]
        {
            let table = self.table;
            let worker = self.worker;
            let read_scope = &mut self.read_scope;
            table.visit_fixed_bytes_inner(
                &mut *self.transaction,
                Some(worker),
                keys,
                batch,
                visit,
                |batch| table.lookup_fixed_in_read_scope(worker, read_scope, keys, batch),
            )
        }
        #[cfg(test)]
        {
            let table = self.table;
            table.visit_fixed_bytes_inner(
                &mut *self.transaction,
                Some(self.worker),
                keys,
                batch,
                visit,
                |batch| {
                    for key in keys {
                        batch.push_record_id(table.shared().lookup(None, key)?);
                    }
                    Ok(())
                },
            )
        }
    }

    /// Visits fixed-width keys as operation-scoped bytes and returns each
    /// key's stable record identity through the same callback.
    ///
    /// This has the ordering, duplicate, miss, failure, and transactional
    /// observation semantics of [`Self::visit_fixed_bytes`]. The byte borrow
    /// cannot escape its callback, while the owned [`ResolvedRecord`] may be
    /// retained and reused with this table after the callback returns. A
    /// logical miss also supplies its stable tombstone identity.
    #[inline]
    pub fn visit_fixed_resolving_bytes<const KEY_LENGTH: usize>(
        &mut self,
        keys: &[[u8; KEY_LENGTH]],
        batch: &mut PointReadBatch,
        visit: impl for<'value> FnMut(usize, Option<&'value [u8]>, ResolvedRecord),
    ) -> Result<usize, AccessError> {
        #[cfg(not(test))]
        {
            let table = self.table;
            let worker = self.worker;
            let read_scope = &mut self.read_scope;
            table.visit_fixed_resolving_bytes_inner(
                &mut *self.transaction,
                Some(worker),
                keys,
                batch,
                visit,
                |batch| table.lookup_fixed_in_read_scope(worker, read_scope, keys, batch),
            )
        }
        #[cfg(test)]
        {
            let table = self.table;
            table.visit_fixed_resolving_bytes_inner(
                &mut *self.transaction,
                Some(self.worker),
                keys,
                batch,
                visit,
                |batch| {
                    for key in keys {
                        batch.push_record_id(table.shared().lookup(None, key)?);
                    }
                    Ok(())
                },
            )
        }
    }

    /// Visits a mixed batch of cached identities and unresolved fixed keys.
    ///
    /// `hints` must contain exactly one entry per key. A present hint is the
    /// authoritative record for that position and skips Masstree. Every absent
    /// hint must appear once in `missing_keys` and `missing_positions`. The
    /// positions must be strictly ascending and each compact key must equal
    /// its original key. These caller-owned compact arrays permit one native
    /// fixed lookup without allocating. All records are then visited once in
    /// original input order. Present hints and the compact mapping are checked
    /// before native lookup or the first callback. The existing unique-item
    /// batch is used when possible; duplicate identities retain the scalar
    /// fallback's sequential read-your-writes behavior.
    ///
    /// A same-table hint is authoritative and cannot be checked against its
    /// key without performing the directory lookup this API exists to skip.
    /// The caller must therefore preserve an exact key-to-token mapping.
    ///
    /// The callback receives the stable identity used for each position, so a
    /// caller may fill empty cache slots after resolution. As with
    /// [`Self::visit_fixed_resolving_bytes`], a logical directory miss is
    /// interned as a stable tombstone before its callback.
    #[doc(hidden)]
    #[inline]
    pub fn visit_fixed_hinted_bytes<const KEY_LENGTH: usize>(
        &mut self,
        keys: &[[u8; KEY_LENGTH]],
        hints: &[Option<ResolvedRecord>],
        missing_keys: &[[u8; KEY_LENGTH]],
        missing_positions: &[usize],
        batch: &mut PointReadBatch,
        visit: impl for<'value> FnMut(usize, Option<&'value [u8]>, ResolvedRecord),
    ) -> Result<usize, AccessError> {
        #[cfg(not(test))]
        {
            let table = self.table;
            let worker = self.worker;
            let read_scope = &mut self.read_scope;
            table.visit_fixed_resolving_bytes_inner(
                &mut *self.transaction,
                Some(worker),
                keys,
                batch,
                visit,
                |batch| {
                    table.lookup_fixed_hinted_in_read_scope(
                        worker,
                        read_scope,
                        keys,
                        hints,
                        missing_keys,
                        missing_positions,
                        batch,
                    )
                },
            )
        }
        #[cfg(test)]
        {
            let table = self.table;
            table.visit_fixed_resolving_bytes_inner(
                &mut *self.transaction,
                Some(self.worker),
                keys,
                batch,
                visit,
                |batch| {
                    table.prepare_hinted_fixed_lookup(
                        keys,
                        hints,
                        missing_keys,
                        missing_positions,
                        batch,
                    )?;
                    for (&original_index, key) in missing_positions.iter().zip(missing_keys) {
                        batch.record_ids[original_index] = table.shared().lookup(None, key)?;
                    }
                    Ok(())
                },
            )
        }
    }

    /// Reads and optionally mutates a contiguous batch of fixed-width keys.
    ///
    /// `modify` runs once per key in input order with that key's current
    /// transaction-local value. Its [`PointMutation`] is applied immediately,
    /// so duplicate keys preserve scalar read-your-writes behavior. The
    /// returned snapshots are the values presented to the callback before
    /// each mutation. Reuse `batch` across attempts to retain all scratch
    /// allocation.
    #[inline]
    pub fn modify_fixed<'batch, const KEY_LENGTH: usize>(
        &mut self,
        keys: &[[u8; KEY_LENGTH]],
        batch: &'batch mut PointReadBatch,
        modify: impl for<'value> FnMut(usize, Option<&'value Value>) -> PointMutation,
    ) -> Result<&'batch [Option<Value>], AccessError> {
        #[cfg(not(test))]
        {
            let table = self.table;
            let worker = self.worker;
            let read_scope = &mut self.read_scope;
            table.modify_fixed_inner(
                &mut *self.transaction,
                Some(worker),
                keys,
                batch,
                modify,
                |batch| table.lookup_fixed_in_read_scope(worker, read_scope, keys, batch),
            )
        }
        #[cfg(test)]
        {
            let table = self.table;
            table.modify_fixed_inner(
                &mut *self.transaction,
                Some(self.worker),
                keys,
                batch,
                modify,
                |batch| {
                    for key in keys {
                        batch.push_record_id(table.shared().lookup(None, key)?);
                    }
                    Ok(())
                },
            )
        }
    }

    /// Reads and optionally mutates a fixed-width key batch without retaining
    /// owned pre-mutation snapshots.
    ///
    /// `modify` runs exactly once per key in input order. Each returned
    /// [`PointMutation`] is applied after that invocation's borrowed value is
    /// released, so duplicate positions observe earlier staged mutations.
    /// Record mutations remain deferred until their callbacks finish. On
    /// success, the returned count equals `keys.len()` and
    /// [`PointReadBatch::results`] is empty.
    ///
    /// If access fails after an input prefix was processed, callback side
    /// effects are not rolled back and the transaction is doomed. A callback
    /// unwind likewise leaves the transaction doomed. When the input keys are
    /// exactly unique and the transaction can enter core's direct batch lane,
    /// directory misses are all interned before the first callback. Those
    /// append-only bindings remain as physical tombstones if a later intern or
    /// transaction step fails; no logical value becomes live until commit.
    #[inline]
    pub fn modify_fixed_visit<const KEY_LENGTH: usize>(
        &mut self,
        keys: &[[u8; KEY_LENGTH]],
        batch: &mut PointReadBatch,
        mut modify: impl for<'value> FnMut(usize, Option<&'value Value>) -> PointMutation,
    ) -> Result<usize, AccessError> {
        #[cfg(not(test))]
        {
            let table = self.table;
            let worker = self.worker;
            let read_scope = &mut self.read_scope;
            table.modify_fixed_visit_inner::<false, KEY_LENGTH>(
                &mut *self.transaction,
                Some(worker),
                keys,
                batch,
                |index, current| Ok(modify(index, current)),
                |batch| table.lookup_fixed_in_read_scope(worker, read_scope, keys, batch),
            )
        }
        #[cfg(test)]
        {
            let table = self.table;
            table.modify_fixed_visit_inner::<false, KEY_LENGTH>(
                &mut *self.transaction,
                Some(self.worker),
                keys,
                batch,
                |index, current| Ok(modify(index, current)),
                |batch| {
                    for key in keys {
                        batch.push_record_id(table.shared().lookup(None, key)?);
                    }
                    Ok(())
                },
            )
        }
    }

    /// Reads and optionally mutates a fixed-width key batch while returning
    /// each key's stable record identity to the mutation callback.
    ///
    /// This is the resolving counterpart to [`Self::modify_fixed_visit`]. It
    /// preserves input order and sequential read-your-writes behavior for
    /// duplicate keys. The owned [`ResolvedRecord`] may be retained after the
    /// borrowed value is released and reused only with the table that minted
    /// it.
    #[inline]
    pub fn modify_fixed_resolving_visit<const KEY_LENGTH: usize>(
        &mut self,
        keys: &[[u8; KEY_LENGTH]],
        batch: &mut PointReadBatch,
        mut modify: impl for<'value> FnMut(
            usize,
            Option<&'value Value>,
            ResolvedRecord,
        ) -> PointMutation,
    ) -> Result<usize, AccessError> {
        #[cfg(not(test))]
        {
            let table = self.table;
            let worker = self.worker;
            let read_scope = &mut self.read_scope;
            table.modify_fixed_resolving_visit_inner::<false, KEY_LENGTH>(
                &mut *self.transaction,
                Some(worker),
                keys,
                batch,
                |index, current, resolved| Ok(modify(index, current, resolved)),
                |batch| table.lookup_fixed_in_read_scope(worker, read_scope, keys, batch),
            )
        }
        #[cfg(test)]
        {
            let table = self.table;
            table.modify_fixed_resolving_visit_inner::<false, KEY_LENGTH>(
                &mut *self.transaction,
                Some(self.worker),
                keys,
                batch,
                |index, current, resolved| Ok(modify(index, current, resolved)),
                |batch| {
                    for key in keys {
                        batch.push_record_id(table.shared().lookup(None, key)?);
                    }
                    Ok(())
                },
            )
        }
    }

    /// Reads and optionally mutates a mixed batch of cached identities and
    /// unresolved fixed keys.
    ///
    /// This is the mutation counterpart to [`Self::visit_fixed_hinted_bytes`].
    /// It validates every present hint first, resolves all absent hints in one
    /// compact native lookup, and invokes `modify` once per input position in
    /// original order. Duplicate records use the sequential fallback, so a
    /// later callback observes an earlier staged mutation. The returned token
    /// is suitable for filling an empty exact cache slot.
    ///
    /// As on the read counterpart, the caller must preserve an exact mapping
    /// between every present same-table hint and its key.
    #[doc(hidden)]
    #[inline]
    pub fn modify_fixed_hinted_visit<const KEY_LENGTH: usize>(
        &mut self,
        keys: &[[u8; KEY_LENGTH]],
        hints: &[Option<ResolvedRecord>],
        missing_keys: &[[u8; KEY_LENGTH]],
        missing_positions: &[usize],
        batch: &mut PointReadBatch,
        mut modify: impl for<'value> FnMut(
            usize,
            Option<&'value Value>,
            ResolvedRecord,
        ) -> PointMutation,
    ) -> Result<usize, AccessError> {
        #[cfg(not(test))]
        {
            let table = self.table;
            let worker = self.worker;
            let read_scope = &mut self.read_scope;
            table.modify_fixed_resolving_visit_inner::<false, KEY_LENGTH>(
                &mut *self.transaction,
                Some(worker),
                keys,
                batch,
                |index, current, resolved| Ok(modify(index, current, resolved)),
                |batch| {
                    table.lookup_fixed_hinted_in_read_scope(
                        worker,
                        read_scope,
                        keys,
                        hints,
                        missing_keys,
                        missing_positions,
                        batch,
                    )
                },
            )
        }
        #[cfg(test)]
        {
            let table = self.table;
            table.modify_fixed_resolving_visit_inner::<false, KEY_LENGTH>(
                &mut *self.transaction,
                Some(self.worker),
                keys,
                batch,
                |index, current, resolved| Ok(modify(index, current, resolved)),
                |batch| {
                    table.prepare_hinted_fixed_lookup(
                        keys,
                        hints,
                        missing_keys,
                        missing_positions,
                        batch,
                    )?;
                    for (&original_index, key) in missing_positions.iter().zip(missing_keys) {
                        batch.record_ids[original_index] = table.shared().lookup(None, key)?;
                    }
                    Ok(())
                },
            )
        }
    }

    /// Copies and replaces one live fixed-key value in one STO item access.
    ///
    /// Before `modify` runs, `output[..current_len]` contains the current
    /// transaction-local value. The remainder of `output` is left unchanged,
    /// so the callback may use that capacity to grow a variable-width field.
    /// The callback returns the replacement length, which must fit `output`.
    /// A missing value returns `Ok(None)` without invoking the callback or
    /// changing `output`.
    ///
    /// On a table configured with [`TableConfig::with_bounded_atomic_values`],
    /// an eligible replacement through [`STABLE_ATOMIC_VALUE_CAPACITY`] is
    /// retained as a private borrowed transaction intent and copied into the
    /// stable cell during commit. Inline and larger values use their ordinary
    /// owned representations because borrowing cannot serve their publication
    /// paths. The returned length is `Some` exactly when a replacement was
    /// staged.
    ///
    /// Buffer and callback errors retain the fixed-modify failure contract:
    /// the active transaction is doomed, this item receives no replacement,
    /// and [`PointReadBatch`] is cleared. Earlier transaction intents remain
    /// unreachable to commit and are discarded by abort cleanup. An unwind
    /// from `modify` likewise leaves the transaction doomed.
    ///
    /// # Safety
    ///
    /// If `modify` returns a length successfully, `output` must stay at the
    /// same address and `output[..replacement_len]` must remain readable and
    /// immutable until the active transaction commits or aborts. This promise
    /// applies even if a later internal step returns an error or unwinds. The
    /// allocation must not alias table storage or any other live borrowed
    /// transaction intent.
    #[allow(
        unsafe_code,
        reason = "the caller supplies the external value lifetime retained by the transaction"
    )]
    #[inline]
    pub unsafe fn modify_fixed_borrowed<const KEY_LENGTH: usize>(
        &mut self,
        key: &[u8; KEY_LENGTH],
        batch: &mut PointReadBatch,
        output: &mut [u8],
        mut modify: impl for<'buffer> FnMut(&'buffer mut [u8], usize) -> usize,
    ) -> Result<Option<usize>, AccessError> {
        // SAFETY: This method forwards its output-lifetime and non-aliasing
        // contract unchanged to the fallible implementation.
        unsafe {
            self.try_modify_fixed_borrowed(key, batch, output, |buffer, current_len| {
                Ok(modify(buffer, current_len))
            })
        }
    }

    /// Fallible counterpart to [`Self::modify_fixed_borrowed`].
    ///
    /// An error returned by `modify` dooms the transaction before this item
    /// can retain a replacement. The caller-owned output may contain the
    /// callback's partial edits, but no table state can commit from the doomed
    /// attempt.
    ///
    /// # Safety
    ///
    /// This has the same output lifetime and non-aliasing requirements as
    /// [`Self::modify_fixed_borrowed`].
    #[allow(
        unsafe_code,
        reason = "the caller supplies the external value lifetime retained by the transaction"
    )]
    #[inline]
    pub unsafe fn try_modify_fixed_borrowed<const KEY_LENGTH: usize>(
        &mut self,
        key: &[u8; KEY_LENGTH],
        batch: &mut PointReadBatch,
        output: &mut [u8],
        modify: impl for<'buffer> FnMut(&'buffer mut [u8], usize) -> Result<usize, AccessError>,
    ) -> Result<Option<usize>, AccessError> {
        let table = self.table;
        #[cfg(not(test))]
        {
            let worker = self.worker;
            let read_scope = &mut self.read_scope;
            // SAFETY: Forwarded from this method's caller-owned output
            // lifetime and non-aliasing contract.
            unsafe {
                table.try_modify_fixed_borrowed_inner(
                    &mut *self.transaction,
                    Some(worker),
                    key,
                    batch,
                    output,
                    modify,
                    |batch| {
                        table.lookup_fixed_in_read_scope(
                            worker,
                            read_scope,
                            std::slice::from_ref(key),
                            batch,
                        )
                    },
                )
            }
        }
        #[cfg(test)]
        {
            // SAFETY: Forwarded from this method's caller-owned output
            // lifetime and non-aliasing contract.
            unsafe {
                table.try_modify_fixed_borrowed_inner(
                    &mut *self.transaction,
                    Some(self.worker),
                    key,
                    batch,
                    output,
                    modify,
                    |batch| {
                        batch.push_record_id(table.shared().lookup(None, key)?);
                        Ok(())
                    },
                )
            }
        }
    }

    /// Reads and optionally mutates a fixed-width batch whose keys the caller
    /// expects to be absent, without a separate directory lookup.
    ///
    /// The expectation is only a performance hint. The directory's atomic
    /// resolve-or-publish operation still discovers existing bindings, and
    /// `modify` receives each live transaction-local value in input order.
    /// Duplicate input keys take the ordinary sequential path so later
    /// positions observe earlier staged mutations. Exact-unique misses are
    /// physically interned before the first callback; those tombstone
    /// bindings survive abort exactly as in [`Table::insert_expected_absent`].
    #[inline]
    pub fn modify_fixed_expected_absent_visit<const KEY_LENGTH: usize>(
        &mut self,
        keys: &[[u8; KEY_LENGTH]],
        batch: &mut PointReadBatch,
        mut modify: impl for<'value> FnMut(usize, Option<&'value Value>) -> PointMutation,
    ) -> Result<usize, AccessError> {
        let table = self.table;
        table.modify_fixed_visit_inner::<false, KEY_LENGTH>(
            &mut *self.transaction,
            Some(self.worker),
            keys,
            batch,
            |index, current| Ok(modify(index, current)),
            |batch| {
                batch.record_ids.resize(keys.len(), None);
                Ok(())
            },
        )
    }

    /// Stages an unconditional live value and returns the prior value.
    #[inline]
    pub fn put(&mut self, key: &[u8], value: &[u8]) -> Result<Option<Value>, AccessError> {
        #[cfg(not(test))]
        {
            let table = self.table;
            let worker = self.worker;
            let read_scope = &mut self.read_scope;
            table.put_inner_with_lookup(
                &mut *self.transaction,
                Some(worker),
                key,
                table.staging_input(value),
                || table.lookup_in_read_scope(worker, read_scope, key),
            )
        }
        #[cfg(test)]
        {
            self.table.put_inner(
                &mut *self.transaction,
                Some(self.worker),
                key,
                self.table.staging_input(value),
            )
        }
    }

    /// Stages a value only when the transaction-local record is absent.
    #[inline]
    pub fn insert(&mut self, key: &[u8], value: &[u8]) -> Result<InsertOutcome, AccessError> {
        #[cfg(not(test))]
        {
            let table = self.table;
            let worker = self.worker;
            let read_scope = &mut self.read_scope;
            table.insert_inner_with_lookup(
                &mut *self.transaction,
                Some(worker),
                key,
                table.staging_input(value),
                || table.lookup_in_read_scope(worker, read_scope, key),
            )
        }
        #[cfg(test)]
        {
            self.table.insert_inner(
                &mut *self.transaction,
                Some(self.worker),
                key,
                self.table.staging_input(value),
            )
        }
    }

    /// Stages a tombstone and returns the prior abstract value.
    #[inline]
    pub fn remove(&mut self, key: &[u8]) -> Result<Option<Value>, AccessError> {
        #[cfg(not(test))]
        {
            let table = self.table;
            let worker = self.worker;
            let read_scope = &mut self.read_scope;
            table.remove_inner_with_lookup(&mut *self.transaction, Some(worker), key, || {
                table.lookup_in_read_scope(worker, read_scope, key)
            })
        }
        #[cfg(test)]
        {
            self.table
                .remove_inner(&mut *self.transaction, Some(self.worker), key)
        }
    }

    /// Closes the point scope before running one transactional range scan.
    ///
    /// Later point operations on this session lazily open another scope.
    pub fn scan(&mut self, request: ScanRequest<'_>) -> Result<Vec<ScanRecord>, AccessError> {
        self.close_active_scope()?;
        self.table
            .scan_inner(&mut *self.transaction, Some(self.worker), request)
    }

    /// Explicitly closes the native point scope and releases all borrows.
    pub fn close(mut self) -> Result<(), AccessError> {
        self.close_active_scope()
    }

    fn close_active_scope(&mut self) -> Result<(), AccessError> {
        #[cfg(not(test))]
        {
            self.table.close_read_scope(&mut self.read_scope)
        }
        #[cfg(test)]
        {
            Ok(())
        }
    }
}

impl Drop for PointSession<'_, '_> {
    fn drop(&mut self) {
        let _ = self.close_active_scope();
    }
}

impl Clone for Table {
    fn clone(&self) -> Self {
        Self {
            record_resource: self.record_resource.clone(),
            directory_resource: self.directory_resource.clone(),
            scan_resource: self.scan_resource.clone(),
        }
    }
}

impl fmt::Debug for Table {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("Table")
            .field("runtime_id", &self.record_resource.runtime_id())
            .field("object_id", &self.object_id())
            .field("health", &self.health())
            .field("usage", &self.usage())
            .finish_non_exhaustive()
    }
}

#[derive(Clone, Copy)]
struct DirectoryScanRequest<'key> {
    direction: ScanDirection,
    lower: ScanBound<'key>,
    upper: ScanBound<'key>,
    entry_capacity: usize,
    key_arena_capacity: usize,
}

#[derive(Clone, Copy)]
struct DirectoryScanEntryRef<'chunk> {
    key: &'chunk [u8],
    record_id: RecordId,
}

#[cfg(test)]
struct MemoryDirectoryScanEntry {
    key: Box<[u8]>,
    record_id: RecordId,
}

enum DirectoryScanResumeRef<'chunk> {
    None,
    UnchangedInput,
    Exclusive(&'chunk [u8]),
}

#[cfg(test)]
struct MemoryDirectoryScanChunk {
    entries: Vec<MemoryDirectoryScanEntry>,
    stop_reason: ScanStopReason,
    resume: MemoryDirectoryScanResume,
    next_key_bytes_required: usize,
}

#[cfg(test)]
enum MemoryDirectoryScanResume {
    None,
    UnchangedInput,
    Exclusive(Box<[u8]>),
}

enum DirectoryScanChunk<'scratch> {
    #[cfg(not(test))]
    Native(NativePackedScanChunkRef<'scratch>),
    #[cfg(test)]
    Memory(
        MemoryDirectoryScanChunk,
        std::marker::PhantomData<&'scratch mut ScanScratch>,
    ),
}

enum DirectoryScanEntries<'chunk> {
    #[cfg(not(test))]
    Native(NativePackedScanEntries<'chunk>),
    #[cfg(test)]
    Memory(std::slice::Iter<'chunk, MemoryDirectoryScanEntry>),
}

impl<'chunk> Iterator for DirectoryScanEntries<'chunk> {
    type Item = DirectoryScanEntryRef<'chunk>;

    #[inline]
    fn next(&mut self) -> Option<Self::Item> {
        match self {
            #[cfg(not(test))]
            Self::Native(entries) => entries.next().map(|entry| DirectoryScanEntryRef {
                key: entry.key(),
                record_id: entry.record_id(),
            }),
            #[cfg(test)]
            Self::Memory(entries) => entries.next().map(|entry| DirectoryScanEntryRef {
                key: &entry.key,
                record_id: entry.record_id,
            }),
        }
    }

    #[inline]
    fn size_hint(&self) -> (usize, Option<usize>) {
        match self {
            #[cfg(not(test))]
            Self::Native(entries) => entries.size_hint(),
            #[cfg(test)]
            Self::Memory(entries) => entries.size_hint(),
        }
    }
}

impl DoubleEndedIterator for DirectoryScanEntries<'_> {
    #[inline]
    fn next_back(&mut self) -> Option<Self::Item> {
        match self {
            #[cfg(not(test))]
            Self::Native(entries) => entries.next_back().map(|entry| DirectoryScanEntryRef {
                key: entry.key(),
                record_id: entry.record_id(),
            }),
            #[cfg(test)]
            Self::Memory(entries) => entries.next_back().map(|entry| DirectoryScanEntryRef {
                key: &entry.key,
                record_id: entry.record_id,
            }),
        }
    }
}

impl ExactSizeIterator for DirectoryScanEntries<'_> {}
impl std::iter::FusedIterator for DirectoryScanEntries<'_> {}

impl DirectoryScanChunk<'_> {
    #[inline]
    fn entries(&self) -> DirectoryScanEntries<'_> {
        match self {
            #[cfg(not(test))]
            Self::Native(chunk) => DirectoryScanEntries::Native(chunk.entries()),
            #[cfg(test)]
            Self::Memory(chunk, _) => DirectoryScanEntries::Memory(chunk.entries.iter()),
        }
    }

    #[inline]
    fn len(&self) -> usize {
        match self {
            #[cfg(not(test))]
            Self::Native(chunk) => chunk.len(),
            #[cfg(test)]
            Self::Memory(chunk, _) => chunk.entries.len(),
        }
    }

    #[inline]
    fn stop_reason(&self) -> ScanStopReason {
        match self {
            #[cfg(not(test))]
            Self::Native(chunk) => chunk.stop_reason(),
            #[cfg(test)]
            Self::Memory(chunk, _) => chunk.stop_reason,
        }
    }

    #[inline]
    fn resume(&self) -> DirectoryScanResumeRef<'_> {
        match self {
            #[cfg(not(test))]
            Self::Native(chunk) => match chunk.resume() {
                NativePackedScanResume::None => DirectoryScanResumeRef::None,
                NativePackedScanResume::UnchangedInput => DirectoryScanResumeRef::UnchangedInput,
                NativePackedScanResume::Exclusive(key) => DirectoryScanResumeRef::Exclusive(key),
            },
            #[cfg(test)]
            Self::Memory(chunk, _) => match &chunk.resume {
                MemoryDirectoryScanResume::None => DirectoryScanResumeRef::None,
                MemoryDirectoryScanResume::UnchangedInput => DirectoryScanResumeRef::UnchangedInput,
                MemoryDirectoryScanResume::Exclusive(key) => DirectoryScanResumeRef::Exclusive(key),
            },
        }
    }

    #[inline]
    fn next_key_bytes_required(&self) -> usize {
        match self {
            #[cfg(not(test))]
            Self::Native(chunk) => chunk.next_key_bytes_required(),
            #[cfg(test)]
            Self::Memory(chunk, _) => chunk.next_key_bytes_required,
        }
    }
}

fn resumed_bounds<'key>(
    request: ScanRequest<'key>,
    resume: Option<&'key [u8]>,
) -> (ScanBound<'key>, ScanBound<'key>) {
    match (request.direction, resume) {
        (ScanDirection::Forward, Some(key)) => (ScanBound::Excluded(key), request.upper),
        (ScanDirection::Reverse, Some(key)) => (request.lower, ScanBound::Excluded(key)),
        (_, None) => (request.lower, request.upper),
    }
}

#[cfg(test)]
fn key_in_bounds(key: &[u8], lower: ScanBound<'_>, upper: ScanBound<'_>) -> bool {
    let above_lower = match lower {
        ScanBound::Unbounded => true,
        ScanBound::Included(bound) => key >= bound,
        ScanBound::Excluded(bound) => key > bound,
    };
    let below_upper = match upper {
        ScanBound::Unbounded => true,
        ScanBound::Included(bound) => key <= bound,
        ScanBound::Excluded(bound) => key < bound,
    };
    above_lower && below_upper
}

fn range_is_empty(lower: ScanBound<'_>, upper: ScanBound<'_>) -> bool {
    let (lower_key, lower_included) = match lower {
        ScanBound::Unbounded => return false,
        ScanBound::Included(key) => (key, true),
        ScanBound::Excluded(key) => (key, false),
    };
    let (upper_key, upper_included) = match upper {
        ScanBound::Unbounded => return false,
        ScanBound::Included(key) => (key, true),
        ScanBound::Excluded(key) => (key, false),
    };
    lower_key > upper_key || (lower_key == upper_key && !(lower_included && upper_included))
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum RecordTokenMode {
    /// Masstree stores the registry's monotonically allocated logical ID.
    RegistryId,
    /// Masstree stores an exposed address of one stable `RegistryEntry`.
    DirectRecordPointer,
}

// Runtime owner IDs are exclusive to one attached worker at a time. Keeping
// each owner's last committed publication on its own cache line lets one
// transaction invalidate trusted scans once per table without adding a new
// writer-writer false-sharing point.
#[repr(align(64))]
struct ScanPublicationOwner {
    last_commit_id: AtomicU64,
}

impl ScanPublicationOwner {
    const fn new() -> Self {
        Self {
            last_commit_id: AtomicU64::new(0),
        }
    }
}

fn scan_publication_owners(
    runtime: &Runtime,
    enabled: bool,
) -> Result<Box<[ScanPublicationOwner]>, RegistrationError> {
    if !enabled {
        return Ok(Box::new([]));
    }
    let owner_count = runtime.config().max_workers();
    let mut owners = Vec::new();
    owners
        .try_reserve_exact(owner_count)
        .map_err(|_| RegistrationError::Capacity(CapacityError::BufferLimit))?;
    owners.resize_with(owner_count, ScanPublicationOwner::new);
    Ok(owners.into_boxed_slice())
}

struct TableShared {
    directory: Directory,
    registry: Registry,
    record_token_mode: RecordTokenMode,
    structural: StructuralGate,
    health: AtomicU8,
    runtime_id: sto_core::RuntimeId,
    namespace: LockNamespaceId,
    record_lock_class: LockClass,
    directory_generation: AtomicU64,
    // Opted-in directory changes and each transaction that commits record
    // publications advance this sequence. Trusted scans sandwich all untracked
    // row snapshots with it and retain the observed value in one scan-role STO
    // item.
    scan_generation: AtomicU64,
    // A commit ID never repeats within this table's runtime. An owner-local
    // match therefore proves that this transaction already performed its
    // table-wide committed-publication invalidation.
    scan_publication_owners: Box<[ScanPublicationOwner]>,
}

impl TableShared {
    #[inline(always)]
    fn advance_scan_generation(&self) {
        if !self.registry.config.trusted_scan_value_generation {
            return;
        }
        let prior_generation = self.scan_generation.fetch_add(1, Ordering::AcqRel);
        if prior_generation == u64::MAX {
            // A wrapped observation could validate after intervening changes.
            // This limit is unreachable in practice, but poisoning and panic
            // keep both execution and install callbacks fail-closed.
            self.poison();
            panic!("sto-masstree scan generation exhausted");
        }
    }

    #[inline(always)]
    fn advance_scan_generation_for_commit(&self, owner: OwnerId, commit_id: OccCommitId) {
        if !self.registry.config.trusted_scan_value_generation {
            return;
        }
        let Some(publication) = self.scan_publication_owners.get(owner.get() as usize) else {
            self.poison();
            panic!("sto-masstree scan publication owner exceeds the table runtime");
        };
        let commit_id = commit_id.get();
        if publication.last_commit_id.load(Ordering::Relaxed) == commit_id {
            return;
        }

        // Core acquires every transaction lock before install begins. This
        // bump therefore precedes the first readable value from this commit:
        // an older scan observes the change, while a newer scan encounters a
        // held record version until the complete install pass has finished.
        self.advance_scan_generation();
        // Only the worker owning this slot can publish its commit ID. This is
        // a deduplication token, not the scan synchronization edge; the AcqRel
        // generation increment above supplies that edge.
        publication
            .last_commit_id
            .store(commit_id, Ordering::Relaxed);
    }

    #[inline(always)]
    fn health(&self) -> TableHealth {
        match self.health.load(Ordering::Acquire) {
            TABLE_HEALTHY => TableHealth::Healthy,
            TABLE_POISONED => TableHealth::Poisoned,
            _ => TableHealth::PublicationUnknown,
        }
    }

    #[inline(always)]
    fn ensure_healthy(&self) -> Result<(), AccessError> {
        match self.health() {
            TableHealth::Healthy => Ok(()),
            TableHealth::Poisoned => Err(table_fault("Masstree table is poisoned")),
            TableHealth::PublicationUnknown => {
                Err(table_fault("Masstree publication outcome is unknown"))
            }
        }
    }

    fn poison(&self) {
        let _ = self.health.compare_exchange(
            TABLE_HEALTHY,
            TABLE_POISONED,
            Ordering::AcqRel,
            Ordering::Acquire,
        );
    }

    fn note_access_error(&self, error: &AccessError) {
        if matches!(
            error,
            AccessError::Fault(_) | AccessError::Poisoned(_) | AccessError::Internal(_)
        ) {
            self.poison();
        }
    }

    fn mark_publication_unknown(&self) {
        self.health
            .store(TABLE_PUBLICATION_UNKNOWN, Ordering::Release);
    }

    #[inline]
    fn lookup(&self, worker: Option<&Worker>, key: &[u8]) -> Result<Option<RecordId>, AccessError> {
        self.ensure_healthy()?;
        self.directory
            .get(worker, key)
            .inspect_err(|error| self.note_access_error(error))
    }

    fn intern_missing(&self, worker: Option<&Worker>, key: &[u8]) -> Result<RecordId, AccessError> {
        self.ensure_healthy()?;
        self.structural.ensure_unsealed()?;
        let (mut candidate, directory_token) = self
            .registry
            .reserve_candidate_with_mode(key, self.record_token_mode)
            .inspect_err(|error| self.note_access_error(error))?;
        #[cfg(test)]
        self.directory.pause_after_candidate_reservation();
        let structural = match self.structural.try_write() {
            Ok(guard) => guard,
            Err(error) => {
                self.note_access_error(&error);
                self.registry
                    .prove_unpublished(&candidate)
                    .inspect_err(|cleanup| self.note_access_error(cleanup))?;
                return Err(error);
            }
        };
        if let Err(error) = self.structural.ensure_unsealed() {
            let cleanup = self
                .registry
                .prove_unpublished(&candidate)
                .inspect_err(|cleanup| self.note_access_error(cleanup));
            drop(structural);
            return cleanup.and(Err(error));
        }

        // Advance while exclusive structural admission is held and before
        // native publication can become visible to ungated point lookups. A
        // scan either holds the read side (so this attempt fails admission),
        // or its final generation check observes this conservative change.
        // Every admitted attempt owns a distinct consumed candidate and the
        // registry caps those below `u64::MAX`, so this counter cannot wrap.
        let previous_generation = self.directory_generation.fetch_add(1, Ordering::AcqRel);
        debug_assert_ne!(previous_generation, u64::MAX);
        self.advance_scan_generation();

        let result = self.directory.get_or_insert(worker, key, directory_token);
        let resolved = match result {
            Ok(DirectoryInsertOutcome::Inserted(winner)) => {
                if winner != directory_token {
                    self.poison();
                    return Err(table_fault("inserted winner differs from candidate"));
                }
                self.registry
                    .mark_published(&mut candidate)
                    .inspect_err(|error| self.note_access_error(error))?;
                Ok(winner)
            }
            Ok(DirectoryInsertOutcome::Existing(winner)) => {
                self.registry
                    .prove_unpublished(&candidate)
                    .inspect_err(|error| self.note_access_error(error))?;
                Ok(winner)
            }
            Err(error) => self.handle_insert_error(&mut candidate, error),
        };
        drop(structural);
        resolved
    }

    #[cfg(not(test))]
    fn intern_fixed_missing<const KEY_LENGTH: usize>(
        &self,
        worker: Option<&Worker>,
        keys: &[[u8; KEY_LENGTH]],
        missing_count: usize,
        batch: &mut PointReadBatch,
    ) -> Result<(), AccessError> {
        debug_assert!(KEY_LENGTH <= 16);
        debug_assert_ne!(missing_count, 0);
        debug_assert_eq!(
            missing_count,
            batch
                .record_ids
                .iter()
                .filter(|record_id| record_id.is_none())
                .count()
        );
        self.ensure_healthy()?;
        self.structural.ensure_unsealed()?;
        let scratch = &mut batch.fixed_inserts;
        debug_assert!(scratch.candidates.is_empty());
        scratch.prepare(missing_count)?;
        let mut reservation_error = None;

        for (position, key) in keys.iter().enumerate() {
            if batch.record_ids[position].is_none() {
                let mut packed = [0_u8; 16];
                packed[..KEY_LENGTH].copy_from_slice(key);
                scratch.keys.push(packed);
                scratch.positions.push(position);
            }
        }
        debug_assert_eq!(scratch.keys.len(), missing_count);

        let batch_reservation = self
            .registry
            .reserve_candidate_batch_with_mode(
                missing_count,
                KEY_LENGTH as u64,
                self.record_token_mode,
                &mut scratch.candidates,
                &mut scratch.directory_tokens,
            )
            .inspect_err(|error| self.note_access_error(error));
        match batch_reservation {
            Ok(CandidateBatchReservation::Reserved) => {}
            Err(error) => {
                // Range validation fails before the consumed-ID CAS and
                // therefore leaves no READY candidate. Keep every scratch
                // vector aligned before the common error exit below.
                scratch.keys.truncate(scratch.candidates.len());
                scratch.positions.truncate(scratch.candidates.len());
                reservation_error = Some(error);
            }
            Ok(CandidateBatchReservation::RetryScalar) => {
                // A whole-range quota reservation is intentionally
                // all-or-nothing. If the complete range does not fit, replay
                // scalar reservation so an existing physical tombstone can
                // still be found and a genuine miss retains the historical
                // successful-prefix/error behavior.
                scratch.clear();
                for (position, key) in keys.iter().enumerate() {
                    if batch.record_ids[position].is_some() {
                        continue;
                    }
                    let reserved = self
                        .registry
                        .reserve_candidate_with_mode(key, self.record_token_mode)
                        .inspect_err(|error| self.note_access_error(error));
                    let (candidate, directory_token) = match reserved {
                        Ok(reserved) => reserved,
                        Err(error) => {
                            // The expected-absent fixed lane deliberately skips
                            // its first directory traversal. If candidate quota
                            // is exhausted, retain the old lookup-first behavior
                            // for a binding that already exists (notably an
                            // aborted insert's reusable tombstone) before
                            // reporting the reservation error for a genuine miss.
                            match self.lookup(worker, key) {
                                Ok(Some(record_id)) => {
                                    batch.record_ids[position] = Some(record_id);
                                    continue;
                                }
                                Ok(None) => reservation_error = Some(error),
                                Err(lookup_error) => reservation_error = Some(lookup_error),
                            }
                            break;
                        }
                    };
                    let mut packed = [0_u8; 16];
                    packed[..KEY_LENGTH].copy_from_slice(key);
                    scratch.keys.push(packed);
                    scratch.positions.push(position);
                    scratch.candidates.push(candidate);
                    scratch.directory_tokens.push(directory_token);
                }
            }
        }
        if scratch.candidates.is_empty() {
            scratch.clear();
            return match reservation_error {
                Some(error) => Err(error),
                None => Ok(()),
            };
        }

        let structural = match self.structural.try_write() {
            Ok(guard) => guard,
            Err(error) => {
                self.note_access_error(&error);
                let cleanup = self.prove_fixed_candidates_unpublished(&scratch.candidates);
                scratch.clear();
                return cleanup.and(Err(error));
            }
        };
        if let Err(error) = self.cleanup_fixed_candidates_if_sealed(&scratch.candidates) {
            scratch.clear();
            drop(structural);
            return Err(error);
        }

        // One conservative generation change covers every publication in this
        // structurally exclusive native batch. A scan either prevented this
        // admission or observes the changed generation during validation.
        let previous_generation = self.directory_generation.fetch_add(1, Ordering::AcqRel);
        debug_assert_ne!(previous_generation, u64::MAX);
        self.advance_scan_generation();
        let native_result = self.directory.get_or_insert_fixed_strided::<KEY_LENGTH>(
            worker,
            &scratch.keys,
            &scratch.directory_tokens,
            &mut scratch.results,
        );

        if scratch.results.len() != scratch.candidates.len()
            || scratch
                .results
                .iter()
                .copied()
                .zip(scratch.directory_tokens.iter().copied())
                .any(|(result, candidate)| result.classification(candidate).is_err())
        {
            let transition = self.mark_fixed_candidates_unknown(&scratch.candidates);
            self.mark_publication_unknown();
            scratch.clear();
            drop(structural);
            transition?;
            return Err(table_fault(
                "fixed directory insertion returned an invalid classification",
            ));
        }

        let mut transition_error = None;
        let mut inserted_before_error = false;
        let mut unknown_publication = false;
        for index in 0..scratch.candidates.len() {
            let directory_token = scratch.directory_tokens[index];
            let position = scratch.positions[index];
            let (publication, winner) = scratch.results[index]
                .classification(directory_token)
                .expect("the fixed insertion result was validated above");
            let candidate = &mut scratch.candidates[index];
            let transition = match publication {
                PublicationDisposition::CandidateInserted => {
                    if native_result.is_err() {
                        inserted_before_error = true;
                    }
                    if winner != Some(directory_token) {
                        Err(table_fault("inserted winner differs from candidate"))
                    } else {
                        batch.record_ids[position] = winner;
                        self.registry.mark_published(candidate)
                    }
                }
                PublicationDisposition::CandidateProvenUnpublished => {
                    if let Some(winner) = winner {
                        batch.record_ids[position] = Some(winner);
                    }
                    self.registry.prove_unpublished(candidate)
                }
                PublicationDisposition::FailureBeforePublication => {
                    self.registry.prove_unpublished(candidate)
                }
                PublicationDisposition::Unknown => {
                    unknown_publication = true;
                    self.registry.mark_unknown(candidate)
                }
            };
            if let Err(error) = transition {
                self.note_access_error(&error);
                if transition_error.is_none() {
                    transition_error = Some(error);
                }
            }
        }

        let successful_call_omitted_winner = native_result.is_ok()
            && scratch
                .positions
                .iter()
                .any(|position| batch.record_ids[*position].is_none());
        scratch.clear();
        drop(structural);
        if unknown_publication {
            self.mark_publication_unknown();
            return Err(transition_error
                .unwrap_or_else(|| table_fault("native fixed insertion publication is unknown")));
        }
        if let Some(error) = transition_error {
            self.poison();
            return Err(error);
        }
        if let Err(error) = native_result {
            let mapped = map_masstree_error(error);
            self.note_access_error(&mapped);
            if inserted_before_error {
                self.poison();
                return Err(table_fault(
                    "native fixed insertion failed after publishing a candidate",
                ));
            }
            return Err(mapped);
        }
        if successful_call_omitted_winner {
            self.poison();
            return Err(table_fault(
                "successful fixed directory insertion omitted a winner",
            ));
        }
        if let Some(error) = reservation_error {
            // Preserve scalar interning's physical side effect: a reservation
            // failure after a valid prefix still publishes/classifies that
            // prefix in one native admission. The caller observes the original
            // error before any logical STO item or visitor callback exists.
            return Err(error);
        }
        Ok(())
    }

    fn prove_fixed_candidates_unpublished(
        &self,
        candidates: &[Candidate],
    ) -> Result<(), AccessError> {
        let mut first_error = None;
        for candidate in candidates {
            if let Err(error) = self.registry.prove_unpublished(candidate) {
                self.note_access_error(&error);
                if first_error.is_none() {
                    first_error = Some(error);
                }
            }
        }
        match first_error {
            Some(error) => Err(error),
            None => Ok(()),
        }
    }

    fn cleanup_fixed_candidates_if_sealed(
        &self,
        candidates: &[Candidate],
    ) -> Result<(), AccessError> {
        let seal_error = match self.structural.ensure_unsealed() {
            Ok(()) => return Ok(()),
            Err(error) => error,
        };
        self.prove_fixed_candidates_unpublished(candidates)
            .and(Err(seal_error))
    }

    #[cfg(not(test))]
    fn mark_fixed_candidates_unknown(&self, candidates: &[Candidate]) -> Result<(), AccessError> {
        let mut first_error = None;
        for candidate in candidates {
            if let Err(error) = self.registry.mark_unknown(candidate) {
                self.note_access_error(&error);
                if first_error.is_none() {
                    first_error = Some(error);
                }
            }
        }
        match first_error {
            Some(error) => Err(error),
            None => Ok(()),
        }
    }

    #[cfg(not(test))]
    fn handle_insert_error(
        &self,
        candidate: &mut Candidate,
        error: MasstreeInsertError,
    ) -> Result<RecordId, AccessError> {
        match error.publication() {
            PublicationDisposition::FailureBeforePublication
            | PublicationDisposition::CandidateProvenUnpublished => {
                self.registry
                    .prove_unpublished(candidate)
                    .inspect_err(|cleanup| self.note_access_error(cleanup))?;
                let mapped = map_masstree_error(error.error());
                self.note_access_error(&mapped);
                Err(mapped)
            }
            PublicationDisposition::CandidateInserted => {
                self.registry
                    .mark_published(candidate)
                    .inspect_err(|transition| self.note_access_error(transition))?;
                self.poison();
                Err(table_fault(
                    "native insertion failed after publishing its candidate",
                ))
            }
            PublicationDisposition::Unknown => {
                self.registry
                    .mark_unknown(candidate)
                    .inspect_err(|transition| self.note_access_error(transition))?;
                self.mark_publication_unknown();
                Err(table_fault("native insertion publication is unknown"))
            }
        }
    }

    #[cfg(test)]
    fn handle_insert_error(
        &self,
        _candidate: &mut Candidate,
        error: MemoryInsertError,
    ) -> Result<RecordId, AccessError> {
        match error {}
    }

    #[inline(always)]
    fn resolve_directory_record(&self, record_id: RecordId) -> Result<&Record, AccessError> {
        // The directory is private to this table and append-only. The public
        // supplied-tree lane stores a logical ID and performs the ordinary
        // registry traversal. The opt-in internally-owned tree stores only
        // addresses minted from this registry's stable Arc arenas; its unsafe
        // provenance reconstruction is isolated in `direct_record`.
        self.resolve_directory_access(record_id)
            .map(|access| access.record)
    }

    /// Resolves one directory-proven identity into an operation-local record
    /// borrow. The borrow is tied to this table and is never retained by STO;
    /// callers pass it directly through the current item operation.
    #[inline(always)]
    fn resolve_directory_access(
        &self,
        record_id: RecordId,
    ) -> Result<RecordAccess<'_>, AccessError> {
        let access = match self.record_token_mode {
            RecordTokenMode::RegistryId => self.registry.resolve_access(record_id),
            RecordTokenMode::DirectRecordPointer => self.registry.resolve_direct_access(record_id),
        }
        .inspect_err(|error| self.note_access_error(error))?;
        Ok(RecordAccess {
            record_id,
            record: access.record(),
            stable: access.stable,
        })
    }

    /// Prefetches private direct-token records without dereferencing them.
    ///
    /// The subsequent ordinary resolver still checks pointer shape, debug
    /// arena ownership, and slot state before user code can observe a value.
    /// Registry-ID tables deliberately retain their existing behavior.
    #[inline(always)]
    fn prefetch_direct_directory_records(
        &self,
        record_ids: &[Option<RecordId>],
    ) -> Result<(), AccessError> {
        if self.record_token_mode != RecordTokenMode::DirectRecordPointer {
            return Ok(());
        }
        for record_id in record_ids.iter().flatten().copied() {
            direct_record::prefetch(record_id)
                .inspect_err(|error| self.note_access_error(error))?;
        }
        Ok(())
    }

    /// Counts misses while issuing the direct-token hints needed by mutation
    /// batches, avoiding a second walk over the lookup results.
    #[inline(always)]
    fn count_missing_and_prefetch_direct_directory_records(
        &self,
        record_ids: &[Option<RecordId>],
    ) -> Result<usize, AccessError> {
        if self.record_token_mode != RecordTokenMode::DirectRecordPointer {
            return Ok(record_ids
                .iter()
                .filter(|record_id| record_id.is_none())
                .count());
        }
        let mut missing = 0;
        for record_id in record_ids {
            match record_id {
                Some(record_id) => direct_record::prefetch(*record_id)
                    .inspect_err(|error| self.note_access_error(error))?,
                None => missing += 1,
            }
        }
        Ok(missing)
    }

    #[inline]
    fn resolve_for_phase(
        &self,
        record_id: RecordId,
        phase: AdapterPhase,
    ) -> Result<&Record, AdapterFault> {
        self.resolve_directory_record(record_id).map_err(|error| {
            self.note_access_error(&error);
            AdapterFault::invariant(phase)
        })
    }

    #[inline]
    fn resolve_with_lock_segment_for_phase(
        &self,
        record_id: RecordId,
        phase: AdapterPhase,
    ) -> Result<(&Record, &Arc<RecordLockSegment>), AdapterFault> {
        if self.record_token_mode != RecordTokenMode::RegistryId {
            self.poison();
            return Err(AdapterFault::new(
                phase,
                AdapterFaultKind::LockIdentityMismatch,
            ));
        }
        self.registry
            .resolve_with_segment(record_id)
            .map_err(|error| {
                self.note_access_error(&error);
                AdapterFault::invariant(phase)
            })
    }

    fn try_scan_structure(&self) -> Result<Option<RwLockReadGuard<'_, ()>>, AccessError> {
        self.structural.try_read()
    }
}

#[derive(Default)]
struct StructuralGate {
    sealed: AtomicBool,
    lock: RwLock<()>,
}

impl StructuralGate {
    #[inline(always)]
    fn is_sealed(&self) -> bool {
        self.sealed.load(Ordering::Acquire)
    }

    #[inline(always)]
    fn ensure_unsealed(&self) -> Result<(), AccessError> {
        if self.is_sealed() {
            Err(directory_structure_sealed())
        } else {
            Ok(())
        }
    }

    #[inline(always)]
    fn mark_sealed(&self) {
        self.sealed.store(true, Ordering::Release);
    }

    fn write(&self) -> Result<RwLockWriteGuard<'_, ()>, AccessError> {
        self.lock
            .write()
            .map_err(|_| table_fault("Masstree structural gate is poisoned"))
    }

    fn try_write(&self) -> Result<RwLockWriteGuard<'_, ()>, AccessError> {
        match self.lock.try_write() {
            Ok(guard) => Ok(guard),
            Err(TryLockError::WouldBlock) => Err(Conflict::HiddenLockBusy.into()),
            Err(TryLockError::Poisoned(_)) => {
                Err(table_fault("Masstree structural gate is poisoned"))
            }
        }
    }

    fn try_read(&self) -> Result<Option<RwLockReadGuard<'_, ()>>, AccessError> {
        if self.is_sealed() {
            return Ok(None);
        }
        match self.lock.try_read() {
            Ok(guard) => {
                if self.is_sealed() {
                    drop(guard);
                    Ok(None)
                } else {
                    Ok(Some(guard))
                }
            }
            Err(TryLockError::WouldBlock) => Err(Conflict::HiddenLockBusy.into()),
            Err(TryLockError::Poisoned(_)) => {
                Err(table_fault("Masstree structural gate is poisoned"))
            }
        }
    }
}

enum Directory {
    #[cfg(not(test))]
    Native(NativeDirectory),
    #[cfg(test)]
    Memory(MemoryDirectory),
}

impl Directory {
    fn seal_structure(&self) -> Result<(), AccessError> {
        match self {
            #[cfg(not(test))]
            Self::Native(directory) => directory.seal_structure(),
            #[cfg(test)]
            Self::Memory(directory) => {
                directory.seal_structure();
                Ok(())
            }
        }
    }

    #[cfg(test)]
    fn pause_after_candidate_reservation(&self) {
        match self {
            Self::Memory(directory) => directory.pause_after_candidate_reservation(),
        }
    }

    #[inline]
    fn get(&self, _worker: Option<&Worker>, key: &[u8]) -> Result<Option<RecordId>, AccessError> {
        match self {
            #[cfg(not(test))]
            Self::Native(directory) => directory.get(_worker, key),
            #[cfg(test)]
            Self::Memory(directory) => directory.get(key),
        }
    }

    fn get_or_insert(
        &self,
        _worker: Option<&Worker>,
        key: &[u8],
        candidate: RecordId,
    ) -> Result<DirectoryInsertOutcome, DirectoryInsertError> {
        match self {
            #[cfg(not(test))]
            Self::Native(directory) => directory.get_or_insert(_worker, key, candidate),
            #[cfg(test)]
            Self::Memory(directory) => Ok(directory.get_or_insert(key, candidate)),
        }
    }

    #[cfg(not(test))]
    fn get_or_insert_fixed_strided<const KEY_LENGTH: usize>(
        &self,
        worker: Option<&Worker>,
        keys: &[[u8; 16]],
        candidates: &[RecordId],
        results: &mut Vec<DirectoryFixedInsertResult>,
    ) -> Result<(), MasstreeError> {
        match self {
            Self::Native(directory) => directory
                .get_or_insert_fixed_strided::<KEY_LENGTH>(worker, keys, candidates, results),
        }
    }

    /// Returns a chunk proven to satisfy the exact request.
    ///
    /// Registry-ID tables use the native wrapper's checked decoder. Direct
    /// tables instead rely on their private native tree's ordered-range and
    /// packed-copy contract while retaining every Rust slice-layout check.
    /// The deterministic test backend constructs the same invariants directly
    /// from its ordered map. Callers may therefore consume the chunk without a
    /// second validation walk.
    fn scan<'scratch>(
        &self,
        _worker: Option<&Worker>,
        request: DirectoryScanRequest<'_>,
        scratch: &'scratch mut DirectoryScanStorage,
        trust_native_semantics: bool,
    ) -> Result<DirectoryScanChunk<'scratch>, AccessError> {
        match self {
            #[cfg(not(test))]
            Self::Native(directory) => {
                directory.scan(_worker, request, scratch, trust_native_semantics)
            }
            #[cfg(test)]
            Self::Memory(directory) => {
                let _ = trust_native_semantics;
                let _ = scratch;
                directory.scan(request)
            }
        }
    }

    #[cfg(not(test))]
    fn scan_record_ids_bounded<'scratch>(
        &self,
        worker: Option<&Worker>,
        lower: &[u8],
        upper: &[u8],
        entry_capacity: usize,
        continuation_capacity: usize,
        scratch: &'scratch mut DirectoryScanStorage,
    ) -> Result<NativeBoundedRecordIdScanChunkRef<'scratch>, AccessError> {
        match self {
            Self::Native(directory) => directory.scan_record_ids_bounded(
                worker,
                lower,
                upper,
                entry_capacity,
                continuation_capacity,
                scratch,
            ),
        }
    }
}

#[cfg(not(test))]
type DirectoryInsertError = MasstreeInsertError;

#[cfg(test)]
type DirectoryInsertError = MemoryInsertError;

#[cfg(test)]
enum MemoryInsertError {}

#[cfg(not(test))]
struct NativeDirectory {
    tree: Tree,
}

#[cfg(not(test))]
impl NativeDirectory {
    fn seal_structure(&self) -> Result<(), AccessError> {
        self.tree.seal_structure().map_err(map_masstree_error)
    }

    #[inline]
    fn get(&self, worker: Option<&Worker>, key: &[u8]) -> Result<Option<RecordId>, AccessError> {
        let worker = worker.ok_or_else(|| table_fault("native worker capability is missing"))?;
        self.tree.get(worker, key).map_err(map_masstree_error)
    }

    fn get_or_insert(
        &self,
        worker: Option<&Worker>,
        key: &[u8],
        candidate: RecordId,
    ) -> Result<DirectoryInsertOutcome, MasstreeInsertError> {
        let Some(worker) = worker else {
            unreachable!("a native lookup validates its worker before interning")
        };
        self.tree.get_or_insert(worker, key, candidate)
    }

    fn get_or_insert_fixed_strided<const KEY_LENGTH: usize>(
        &self,
        worker: Option<&Worker>,
        keys: &[[u8; 16]],
        candidates: &[RecordId],
        results: &mut Vec<DirectoryFixedInsertResult>,
    ) -> Result<(), MasstreeError> {
        let Some(worker) = worker else {
            unreachable!("a native lookup validates its worker before interning")
        };
        self.tree
            .get_or_insert_fixed_strided::<KEY_LENGTH, 16>(worker, keys, candidates, results)
    }

    #[allow(
        unsafe_code,
        reason = "direct mode owns the complete native tree access path"
    )]
    fn scan<'scratch>(
        &self,
        worker: Option<&Worker>,
        request: DirectoryScanRequest<'_>,
        scratch: &'scratch mut DirectoryScanStorage,
        trust_native_semantics: bool,
    ) -> Result<DirectoryScanChunk<'scratch>, AccessError> {
        let worker = worker.ok_or_else(|| table_fault("native worker capability is missing"))?;
        let native_request = NativeScanRequest::new(request.direction)
            .with_lower(request.lower)
            .with_upper(request.upper)
            .with_entry_capacity(request.entry_capacity)
            .with_key_arena_capacity(request.key_arena_capacity);
        let chunk = if trust_native_semantics {
            // SAFETY: Direct-record mode creates a fresh tree and keeps its
            // handle private for the table's lifetime. Every scalar or
            // fixed-width publication passes through the safe `Tree` facade,
            // which enforces the negotiated key-length maximum. The native
            // scan's packed-copy, ordered-range contract supplies the other
            // omitted semantic checks; the trusted decoder still validates
            // all slice bounds.
            unsafe {
                self.tree
                    .scan_packed_chunk_reusing_trusted(worker, native_request, scratch)
            }
        } else {
            self.tree
                .scan_packed_chunk_reusing(worker, native_request, scratch)
        }
        .map_err(map_masstree_error)?;
        Ok(DirectoryScanChunk::Native(chunk))
    }

    #[allow(
        unsafe_code,
        reason = "the table owns the complete native directory access path"
    )]
    fn scan_record_ids_bounded<'scratch>(
        &self,
        worker: Option<&Worker>,
        lower: &[u8],
        upper: &[u8],
        entry_capacity: usize,
        continuation_capacity: usize,
        scratch: &'scratch mut DirectoryScanStorage,
    ) -> Result<NativeBoundedRecordIdScanChunkRef<'scratch>, AccessError> {
        let worker = worker.ok_or_else(|| table_fault("native worker capability is missing"))?;
        // SAFETY: The table creates a fresh tree and retains every handle.
        // All keys enter through the checked safe facade, while the native
        // bounded callback supplies the trusted ordering and range contract.
        unsafe {
            self.tree.scan_record_ids_bounded_reusing_trusted(
                worker,
                lower,
                upper,
                entry_capacity,
                continuation_capacity,
                scratch,
            )
        }
        .map_err(map_masstree_error)
    }
}

#[cfg(test)]
#[derive(Default)]
struct MemoryDirectory {
    entries: RwLock<std::collections::BTreeMap<Vec<u8>, RecordId>>,
    first_miss_barrier: Option<Arc<std::sync::Barrier>>,
    candidate_reservation_pause: Option<(Arc<std::sync::Barrier>, Arc<std::sync::Barrier>)>,
    coordinated_misses: AtomicU64,
    point_lookups: AtomicU64,
    seal_calls: AtomicU64,
}

#[cfg(test)]
impl MemoryDirectory {
    fn with_first_miss_barrier(barrier: Arc<std::sync::Barrier>) -> Self {
        Self {
            entries: RwLock::new(std::collections::BTreeMap::new()),
            first_miss_barrier: Some(barrier),
            candidate_reservation_pause: None,
            coordinated_misses: AtomicU64::new(0),
            point_lookups: AtomicU64::new(0),
            seal_calls: AtomicU64::new(0),
        }
    }

    fn with_candidate_reservation_pause(
        reserved: Arc<std::sync::Barrier>,
        resume: Arc<std::sync::Barrier>,
    ) -> Self {
        Self {
            entries: RwLock::new(std::collections::BTreeMap::new()),
            first_miss_barrier: None,
            candidate_reservation_pause: Some((reserved, resume)),
            coordinated_misses: AtomicU64::new(0),
            point_lookups: AtomicU64::new(0),
            seal_calls: AtomicU64::new(0),
        }
    }

    fn seal_structure(&self) {
        self.seal_calls.fetch_add(1, Ordering::Relaxed);
    }

    fn pause_after_candidate_reservation(&self) {
        if let Some((reserved, resume)) = &self.candidate_reservation_pause {
            reserved.wait();
            resume.wait();
        }
    }

    fn get(&self, key: &[u8]) -> Result<Option<RecordId>, AccessError> {
        self.point_lookups.fetch_add(1, Ordering::Relaxed);
        let found = match self.entries.try_read() {
            Ok(entries) => Ok(entries.get(key).copied()),
            Err(TryLockError::WouldBlock) => Err(Conflict::HiddenLockBusy.into()),
            Err(TryLockError::Poisoned(_)) => Err(table_fault("memory directory is poisoned")),
        }?;
        if found.is_none() && self.coordinated_misses.fetch_add(1, Ordering::AcqRel) < 2 {
            if let Some(barrier) = self.first_miss_barrier.as_ref() {
                barrier.wait();
            }
        }
        Ok(found)
    }

    fn get_or_insert(&self, key: &[u8], candidate: RecordId) -> DirectoryInsertOutcome {
        let mut entries = self
            .entries
            .try_write()
            .expect("the deterministic memory directory is uncontended");
        match entries.get(key).copied() {
            Some(existing) => DirectoryInsertOutcome::Existing(existing),
            None => {
                entries.insert(key.to_vec(), candidate);
                DirectoryInsertOutcome::Inserted(candidate)
            }
        }
    }

    fn scan(
        &self,
        request: DirectoryScanRequest<'_>,
    ) -> Result<DirectoryScanChunk<'static>, AccessError> {
        let entries = match self.entries.try_read() {
            Ok(entries) => entries,
            Err(TryLockError::WouldBlock) => return Err(Conflict::HiddenLockBusy.into()),
            Err(TryLockError::Poisoned(_)) => {
                return Err(table_fault("memory directory is poisoned"));
            }
        };
        match request.direction {
            ScanDirection::Forward => collect_memory_scan(
                entries
                    .iter()
                    .filter(|(key, _)| key_in_bounds(key, request.lower, request.upper)),
                request,
            ),
            ScanDirection::Reverse => collect_memory_scan(
                entries
                    .iter()
                    .rev()
                    .filter(|(key, _)| key_in_bounds(key, request.lower, request.upper)),
                request,
            ),
        }
    }
}

#[cfg(test)]
fn collect_memory_scan<'entry>(
    entries: impl Iterator<Item = (&'entry Vec<u8>, &'entry RecordId)>,
    request: DirectoryScanRequest<'_>,
) -> Result<DirectoryScanChunk<'static>, AccessError> {
    let mut copied = Vec::new();
    let mut arena_used = 0_usize;
    for (key, record_id) in entries {
        if copied.len() == request.entry_capacity {
            let resume = copied.last().map_or(
                MemoryDirectoryScanResume::UnchangedInput,
                |last: &MemoryDirectoryScanEntry| {
                    MemoryDirectoryScanResume::Exclusive(last.key.clone())
                },
            );
            return Ok(DirectoryScanChunk::Memory(
                MemoryDirectoryScanChunk {
                    entries: copied,
                    stop_reason: ScanStopReason::EntryCapacity,
                    resume,
                    next_key_bytes_required: key.len(),
                },
                std::marker::PhantomData,
            ));
        }
        let remaining = request.key_arena_capacity.saturating_sub(arena_used);
        if key.len() > remaining {
            let resume = copied.last().map_or(
                MemoryDirectoryScanResume::UnchangedInput,
                |last: &MemoryDirectoryScanEntry| {
                    MemoryDirectoryScanResume::Exclusive(last.key.clone())
                },
            );
            return Ok(DirectoryScanChunk::Memory(
                MemoryDirectoryScanChunk {
                    entries: copied,
                    stop_reason: ScanStopReason::KeyArenaCapacity,
                    resume,
                    next_key_bytes_required: key.len(),
                },
                std::marker::PhantomData,
            ));
        }
        arena_used = arena_used
            .checked_add(key.len())
            .ok_or(CapacityError::BufferLimit)?;
        copied.push(MemoryDirectoryScanEntry {
            key: key.clone().into_boxed_slice(),
            record_id: *record_id,
        });
    }
    Ok(DirectoryScanChunk::Memory(
        MemoryDirectoryScanChunk {
            entries: copied,
            stop_reason: ScanStopReason::End,
            resume: MemoryDirectoryScanResume::None,
            next_key_bytes_required: 0,
        },
        std::marker::PhantomData,
    ))
}

#[cfg(not(test))]
fn map_masstree_error(error: MasstreeError) -> AccessError {
    match error {
        MasstreeError::WrongThread => InvalidUse::WrongThread.into(),
        MasstreeError::WrongRuntime => InvalidUse::WrongRuntime.into(),
        MasstreeError::DuplicateWorker => InvalidUse::WorkerBusy.into(),
        MasstreeError::KeyTooLarge { .. } => CapacityError::KeyLimit.into(),
        MasstreeError::AllocationLimit { .. } => CapacityError::BufferLimit.into(),
        MasstreeError::Native(NativeStatus::Busy | NativeStatus::ActiveGuards) => {
            Conflict::HiddenLockBusy.into()
        }
        MasstreeError::Native(NativeStatus::OutOfMemory) => CapacityError::BufferLimit.into(),
        MasstreeError::Native(NativeStatus::ThreadLimit) => CapacityError::WorkerLimit.into(),
        MasstreeError::Native(NativeStatus::StructureSealed) => directory_structure_sealed(),
        MasstreeError::Native(NativeStatus::Poisoned) => AccessError::Poisoned(PoisonInfo::new(
            FailurePhase::Execution,
            "native Masstree runtime is poisoned",
        )),
        MasstreeError::Native(NativeStatus::WrongThread | NativeStatus::NotAttached) => {
            InvalidUse::WrongThread.into()
        }
        MasstreeError::Native(NativeStatus::WrongRuntime) => InvalidUse::WrongRuntime.into(),
        MasstreeError::Native(_)
        | MasstreeError::AbiMismatch(_)
        | MasstreeError::ZeroRecordId
        | MasstreeError::InvalidPublication
        | MasstreeError::InvalidBatch(_) => table_fault("Masstree boundary contract failed"),
    }
}

fn directory_structure_sealed() -> AccessError {
    Unsupported::Capability("Masstree directory structure is sealed").into()
}

fn table_fault(reason: &'static str) -> AccessError {
    AdapterFault::new(AdapterPhase::Execute, AdapterFaultKind::Other(reason)).into()
}

#[derive(Clone, Copy, Eq, PartialEq)]
struct RecordLockDomain {
    runtime_id: sto_core::RuntimeId,
    namespace: LockNamespaceId,
    lock_class: LockClass,
}

enum RegistryStorage {
    LazySegmented(SegmentedRegistry),
    EagerContiguous(ContiguousRegistry),
}

/// One table-wide concrete registry-entry layout.
///
/// The stable variant keeps `StableRegistryEntry::base` at offset zero, so a
/// private direct-directory token still names the read-hot record line. A
/// thin Arc-owned storage object retains the concrete boxed allocation through
/// every lock-frame use and prevents interpreting one layout as the other.
#[derive(Clone)]
struct RegistryArena {
    storage: Arc<RegistryArenaStorage>,
    len: usize,
}

enum RegistryArenaStorage {
    Standard(Box<[RegistryEntry]>),
    Stable160(Box<[StableRegistryEntry]>),
}

#[derive(Clone, Copy)]
struct RegistrySlotAccess<'slot> {
    entry: &'slot RegistryEntry,
    stable: Option<&'slot StableAtomicValueCell>,
    element_address: usize,
}

impl<'slot> RegistrySlotAccess<'slot> {
    #[inline(always)]
    fn record(self) -> &'slot Record {
        &self.entry.record
    }
}

impl RegistryArena {
    fn allocate(slot_count: usize, bounded_atomic_values: bool) -> Result<Self, CapacityError> {
        let storage = if bounded_atomic_values {
            allocate_stable_registry_slots(slot_count).map(RegistryArenaStorage::Stable160)?
        } else {
            allocate_registry_slots(slot_count).map(RegistryArenaStorage::Standard)?
        };
        Ok(Self {
            storage: Arc::new(storage),
            len: slot_count,
        })
    }

    #[inline(always)]
    fn len(&self) -> usize {
        self.len
    }

    #[inline(always)]
    fn get(&self, index: usize) -> Option<RegistrySlotAccess<'_>> {
        match self.storage.as_ref() {
            RegistryArenaStorage::Standard(slots) => {
                slots.get(index).map(|entry| RegistrySlotAccess {
                    entry,
                    stable: None,
                    element_address: std::ptr::from_ref(entry).expose_provenance(),
                })
            }
            RegistryArenaStorage::Stable160(slots) => {
                slots.get(index).map(|entry| RegistrySlotAccess {
                    entry: &entry.base,
                    stable: Some(&entry.cell),
                    element_address: std::ptr::from_ref(entry).expose_provenance(),
                })
            }
        }
    }

    #[inline(always)]
    fn entry(&self, index: usize) -> Option<&RegistryEntry> {
        self.get(index).map(|access| access.entry)
    }

    #[cfg(debug_assertions)]
    fn owns_element_address(&self, address: usize) -> bool {
        match self.storage.as_ref() {
            RegistryArenaStorage::Standard(slots) => registry_slice_owns_address(slots, address),
            RegistryArenaStorage::Stable160(slots) => registry_slice_owns_address(slots, address),
        }
    }

    #[cfg(test)]
    fn is_stable(&self) -> bool {
        matches!(self.storage.as_ref(), RegistryArenaStorage::Stable160(_))
    }

    #[cfg(test)]
    fn standard_slots(&self) -> &[RegistryEntry] {
        match self.storage.as_ref() {
            RegistryArenaStorage::Standard(slots) => slots,
            RegistryArenaStorage::Stable160(_) => {
                panic!("test expected the standard registry arena")
            }
        }
    }

    #[cfg(test)]
    fn shares_allocation_with(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.storage, &other.storage)
    }
}

struct Registry {
    storage: RegistryStorage,
    consumed: AtomicU64,
    retained_records: AtomicU64,
    retained_key_bytes: AtomicU64,
    config: TableConfig,
    effective_id_limit: u64,
    lock_domain: RecordLockDomain,
}

impl Registry {
    fn new(
        config: TableConfig,
        runtime_id: sto_core::RuntimeId,
        namespace: LockNamespaceId,
        lock_class: LockClass,
    ) -> Result<Self, RegistrationError> {
        let addressable = u64::try_from(isize::MAX).unwrap_or(u64::MAX);
        let effective_id_limit = config.max_consumed_record_ids.min(addressable);
        let lock_domain = RecordLockDomain {
            runtime_id,
            namespace,
            lock_class,
        };
        let storage = match config.registry_layout {
            RegistryLayout::LazySegmented => RegistryStorage::LazySegmented(
                SegmentedRegistry::new(effective_id_limit, config.bounded_atomic_values)?,
            ),
            RegistryLayout::EagerContiguous { max_bytes } => {
                RegistryStorage::EagerContiguous(ContiguousRegistry::new(
                    effective_id_limit,
                    max_bytes,
                    lock_domain,
                    config.bounded_atomic_values,
                )?)
            }
        };
        Ok(Self {
            storage,
            consumed: AtomicU64::new(0),
            retained_records: AtomicU64::new(0),
            retained_key_bytes: AtomicU64::new(0),
            effective_id_limit,
            config,
            lock_domain,
        })
    }

    fn usage(&self) -> TableUsage {
        TableUsage {
            retained_records: self.retained_records.load(Ordering::Acquire),
            retained_key_bytes: self.retained_key_bytes.load(Ordering::Acquire),
            consumed_record_ids: self.consumed.load(Ordering::Acquire),
        }
    }

    #[cfg(test)]
    fn reserve_candidate(&self, key: &[u8]) -> Result<Candidate, AccessError> {
        self.reserve_candidate_with_mode(key, RecordTokenMode::RegistryId)
            .map(|(candidate, _directory_token)| candidate)
    }

    fn reserve_candidate_with_mode(
        &self,
        key: &[u8],
        record_token_mode: RecordTokenMode,
    ) -> Result<(Candidate, RecordId), AccessError> {
        self.reserve_retained(key)?;
        let key_bytes = key.len() as u64;
        let raw_id =
            match self
                .consumed
                .fetch_update(Ordering::AcqRel, Ordering::Acquire, |current| {
                    (current < self.effective_id_limit && current < u64::MAX).then_some(current + 1)
                }) {
                Ok(previous) => previous + 1,
                Err(_) => {
                    self.release_retained(key_bytes);
                    return Err(CapacityError::BufferLimit.into());
                }
            };
        let record_id = RecordId::new(raw_id).expect("checked allocation never produces zero");
        let access = self.claim_slot(record_id).inspect_err(|_| {
            // The entry was never published into the registry, so no other
            // thread can release its retained-resource reservation.
            self.release_retained(key_bytes);
        })?;
        if access.entry.state.load(Ordering::Acquire) != SLOT_RESERVED {
            // No directory operation can observe this candidate before READY,
            // so this owner is also the only legal RESERVED-state writer.
            self.release_retained(key_bytes);
            return Err(table_fault("illegal record registry state transition"));
        }
        access.entry.state.store(SLOT_READY, Ordering::Release);
        let directory_token = match record_token_mode {
            RecordTokenMode::RegistryId => record_id,
            RecordTokenMode::DirectRecordPointer => match direct_record::encode(access) {
                Ok(token) => token,
                Err(error) => {
                    transition_slot(access.entry, SLOT_READY, SLOT_PROVEN_UNPUBLISHED)?;
                    self.release_retained(key_bytes);
                    return Err(error);
                }
            },
        };
        Ok((
            Candidate {
                id: record_id,
                key_bytes,
            },
            directory_token,
        ))
    }

    /// Attempts to reserve one contiguous candidate range for fixed-width
    /// keys. `RetryScalar` leaves no quota, consumed-ID, or candidate side
    /// effect: callers use the ordinary per-key path when the complete range
    /// does not fit, preserving its successful-prefix and existing-tombstone
    /// behavior at quota boundaries. It may retain an initialized but wholly
    /// UNALLOCATED lazy arena segment after a failed allocation attempt.
    ///
    /// Lazy segments are fully initialized before the consumed-ID CAS. A
    /// successful CAS therefore proves exclusive ownership of initialized,
    /// UNALLOCATED slots. No directory token is visible yet, so publishing the
    /// slots with Release stores uses the same ownership rule as scalar
    /// reservation. The scalar path retains RESERVED as a diagnostic state;
    /// neither path needs a locked state-word RMW.
    fn reserve_candidate_batch_with_mode(
        &self,
        count: usize,
        key_bytes: u64,
        record_token_mode: RecordTokenMode,
        candidates: &mut Vec<Candidate>,
        directory_tokens: &mut Vec<RecordId>,
    ) -> Result<CandidateBatchReservation, AccessError> {
        debug_assert!(candidates.is_empty());
        debug_assert!(directory_tokens.is_empty());
        debug_assert!(candidates.capacity() >= count);
        debug_assert!(directory_tokens.capacity() >= count);
        if count == 0 {
            return Ok(CandidateBatchReservation::Reserved);
        }

        let Ok(count) = u64::try_from(count) else {
            return Ok(CandidateBatchReservation::RetryScalar);
        };
        let Some(total_key_bytes) = key_bytes.checked_mul(count) else {
            return Ok(CandidateBatchReservation::RetryScalar);
        };
        if reserve_atomic(
            &self.retained_records,
            count,
            self.config.max_retained_records,
        )
        .is_err()
        {
            return Ok(CandidateBatchReservation::RetryScalar);
        }
        if reserve_atomic(
            &self.retained_key_bytes,
            total_key_bytes,
            self.config.max_retained_key_bytes,
        )
        .is_err()
        {
            self.retained_records.fetch_sub(count, Ordering::AcqRel);
            return Ok(CandidateBatchReservation::RetryScalar);
        }

        let first_raw = loop {
            let current = self.consumed.load(Ordering::Acquire);
            let Some(next) = current
                .checked_add(count)
                .filter(|next| *next <= self.effective_id_limit)
            else {
                self.release_retained_batch(count, total_key_bytes);
                return Ok(CandidateBatchReservation::RetryScalar);
            };
            let first_index = match usize::try_from(current) {
                Ok(index) => index,
                Err(_) => {
                    self.release_retained_batch(count, total_key_bytes);
                    return Ok(CandidateBatchReservation::RetryScalar);
                }
            };
            let slot_count = usize::try_from(count)
                .expect("a usize-derived candidate count must round-trip through u64");

            // Segment allocation may fail, but it happens before the range is
            // consumed. Returning RetryScalar after releasing the batch quota
            // therefore reproduces the scalar path without an ID hole.
            if self.ensure_claim_range(first_index, slot_count).is_err() {
                self.release_retained_batch(count, total_key_bytes);
                return Ok(CandidateBatchReservation::RetryScalar);
            }

            directory_tokens.clear();
            let mut observed_claimed_slot = false;
            for offset in 0..slot_count {
                let access = match self.claim_range_entry(first_index + offset) {
                    Ok(access) => access,
                    Err(error) => {
                        directory_tokens.clear();
                        self.release_retained_batch(count, total_key_bytes);
                        return Err(error);
                    }
                };
                if access.entry.state.load(Ordering::Acquire) != SLOT_UNALLOCATED {
                    observed_claimed_slot = true;
                    break;
                }
                let raw_id = current + offset as u64 + 1;
                let record_id =
                    RecordId::new(raw_id).expect("checked batch allocation never produces zero");
                let directory_token = match record_token_mode {
                    RecordTokenMode::RegistryId => record_id,
                    RecordTokenMode::DirectRecordPointer => match direct_record::encode(access) {
                        Ok(token) => token,
                        Err(error) => {
                            directory_tokens.clear();
                            self.release_retained_batch(count, total_key_bytes);
                            return Err(error);
                        }
                    },
                };
                directory_tokens.push(directory_token);
            }
            if observed_claimed_slot {
                directory_tokens.clear();
                // Another batch can win the range CAS and publish READY while
                // this contender is still preflighting the old range. That is
                // an ordinary retry, not slot reuse. A claimed slot while the
                // allocation frontier is unchanged is the actual invariant
                // violation.
                if self.consumed.load(Ordering::Acquire) != current {
                    continue;
                }
                self.release_retained_batch(count, total_key_bytes);
                return Err(table_fault("record ID registry slot was reused"));
            }

            match self
                .consumed
                .compare_exchange(current, next, Ordering::AcqRel, Ordering::Acquire)
            {
                Ok(_) => break current + 1,
                Err(_) => directory_tokens.clear(),
            }
        };

        let slot_count = usize::try_from(count)
            .expect("a usize-derived candidate count must round-trip through u64");
        let first_index = usize::try_from(first_raw - 1)
            .expect("an addressable candidate ID must have an addressable index");
        for offset in 0..slot_count {
            let access = self
                .claim_range_entry(first_index + offset)
                .expect("a consumed batch range must retain its initialized registry segment");
            // The consumed-range CAS is the unique allocation claim. No other
            // allocator can own this slot and no directory can observe its
            // token until the caller's later native insertion.
            access.entry.state.store(SLOT_READY, Ordering::Release);
            let record_id = RecordId::new(first_raw + offset as u64)
                .expect("checked batch allocation never produces zero");
            candidates.push(Candidate {
                id: record_id,
                key_bytes,
            });
        }
        debug_assert_eq!(candidates.len(), slot_count);
        debug_assert_eq!(directory_tokens.len(), slot_count);
        Ok(CandidateBatchReservation::Reserved)
    }

    fn ensure_claim_range(&self, first_index: usize, count: usize) -> Result<(), AccessError> {
        let last_index = first_index
            .checked_add(count - 1)
            .ok_or(CapacityError::BufferLimit)?;
        match &self.storage {
            RegistryStorage::LazySegmented(storage) => {
                let first_segment = first_index / REGISTRY_SEGMENT_SLOTS;
                let last_segment = last_index / REGISTRY_SEGMENT_SLOTS;
                for segment_index in first_segment..=last_segment {
                    storage.ensure_segment(segment_index, self.lock_domain)?;
                }
                Ok(())
            }
            RegistryStorage::EagerContiguous(storage) => storage
                .arena
                .get(last_index)
                .map(|_| ())
                .ok_or_else(|| CapacityError::BufferLimit.into()),
        }
    }

    fn claim_range_entry(&self, index: usize) -> Result<RegistrySlotAccess<'_>, AccessError> {
        match &self.storage {
            RegistryStorage::LazySegmented(storage) => storage.access(index),
            RegistryStorage::EagerContiguous(storage) => storage.access(index),
        }
    }

    fn reserve_retained(&self, key: &[u8]) -> Result<(), AccessError> {
        reserve_atomic(&self.retained_records, 1, self.config.max_retained_records)
            .map_err(|()| AccessError::from(CapacityError::BufferLimit))?;
        let key_bytes = key.len() as u64;
        if reserve_atomic(
            &self.retained_key_bytes,
            key_bytes,
            self.config.max_retained_key_bytes,
        )
        .is_err()
        {
            self.retained_records.fetch_sub(1, Ordering::AcqRel);
            return Err(CapacityError::KeyLimit.into());
        }
        Ok(())
    }

    fn claim_slot(&self, record_id: RecordId) -> Result<RegistrySlotAccess<'_>, AccessError> {
        let index = record_index(record_id)?;
        let access = match &self.storage {
            RegistryStorage::LazySegmented(storage) => {
                storage.claim_access(index, self.lock_domain)?
            }
            RegistryStorage::EagerContiguous(storage) => storage.claim_access(index)?,
        };
        // The consumed-ID RMW already granted this allocator exclusive
        // ownership of the slot. A batch may inspect the same frontier, but it
        // cannot write the slot unless its later consumed-range CAS wins.
        if access.entry.state.load(Ordering::Acquire) != SLOT_UNALLOCATED {
            return Err(table_fault("record ID registry slot was reused"));
        }
        access.entry.state.store(SLOT_RESERVED, Ordering::Release);
        Ok(access)
    }

    #[cfg(test)]
    fn ensure_segment(&self, segment_index: usize) -> Result<&RegistrySegment, AccessError> {
        match &self.storage {
            RegistryStorage::LazySegmented(storage) => {
                storage.ensure_segment(segment_index, self.lock_domain)
            }
            RegistryStorage::EagerContiguous(_) => Err(table_fault(
                "contiguous record registries have no physical segments",
            )),
        }
    }

    #[inline(always)]
    fn entry(&self, record_id: RecordId) -> Result<&RegistryEntry, AccessError> {
        let index = record_index(record_id)?;
        // Dispatch once at the registry helper boundary. Each variant then
        // follows a mode-specialized path: eager lookup is a direct slice
        // index, while lazy lookup performs its segment publication check.
        match &self.storage {
            RegistryStorage::LazySegmented(storage) => storage.entry(index),
            RegistryStorage::EagerContiguous(storage) => storage.entry(index),
        }
    }

    #[inline(always)]
    fn access(&self, record_id: RecordId) -> Result<RegistrySlotAccess<'_>, AccessError> {
        let index = record_index(record_id)?;
        match &self.storage {
            RegistryStorage::LazySegmented(storage) => storage.access(index),
            RegistryStorage::EagerContiguous(storage) => storage.access(index),
        }
    }

    #[cfg(test)]
    #[inline(always)]
    fn resolve(&self, record_id: RecordId) -> Result<&Record, AccessError> {
        self.resolve_access(record_id)
            .map(RegistrySlotAccess::record)
    }

    #[inline(always)]
    fn resolve_access(&self, record_id: RecordId) -> Result<RegistrySlotAccess<'_>, AccessError> {
        let access = self.access(record_id)?;
        match access.entry.state.load(Ordering::Acquire) {
            SLOT_READY | SLOT_PUBLISHED => Ok(access),
            _ => Err(table_fault("directory returned an unusable registry slot")),
        }
    }

    #[cfg(test)]
    #[inline(always)]
    fn resolve_direct(&self, token: RecordId) -> Result<&Record, AccessError> {
        self.resolve_direct_access(token)
            .map(RegistrySlotAccess::record)
    }

    #[inline(always)]
    fn resolve_direct_access(
        &self,
        token: RecordId,
    ) -> Result<RegistrySlotAccess<'_>, AccessError> {
        let access = direct_record::resolve(self, token)?;
        match access.entry.state.load(Ordering::Acquire) {
            SLOT_READY | SLOT_PUBLISHED => Ok(access),
            _ => Err(table_fault(
                "directory returned an unusable direct record token",
            )),
        }
    }

    #[cfg(debug_assertions)]
    fn owns_entry_address(&self, address: usize) -> bool {
        match &self.storage {
            RegistryStorage::LazySegmented(storage) => storage
                .segments
                .iter()
                .filter_map(OnceLock::get)
                .any(|segment| segment.arena.owns_element_address(address)),
            RegistryStorage::EagerContiguous(storage) => {
                storage.arena.owns_element_address(address)
            }
        }
    }

    #[inline(always)]
    fn resolve_with_segment(
        &self,
        record_id: RecordId,
    ) -> Result<(&Record, &Arc<RecordLockSegment>), AccessError> {
        let index = record_index(record_id)?;
        let (entry, lock_segment) = match &self.storage {
            RegistryStorage::LazySegmented(storage) => storage.entry_with_lock(index)?,
            RegistryStorage::EagerContiguous(storage) => storage.entry_with_lock(index)?,
        };
        // Table construction or segment OnceLock publication exposes the
        // initialized record, and the exact RESERVED -> READY Release
        // transition makes its slot directory-eligible. READY must remain
        // resolvable because the native directory can expose a winner before
        // its inserter records the final PUBLISHED transition. Both states
        // borrow the same stable arena address; every other state is rejected.
        match entry.state.load(Ordering::Acquire) {
            SLOT_READY | SLOT_PUBLISHED => Ok((&entry.record, lock_segment)),
            _ => Err(table_fault("directory returned an unusable registry slot")),
        }
    }

    fn mark_published(&self, candidate: &mut Candidate) -> Result<(), AccessError> {
        let entry = self.entry(candidate.id)?;
        // Candidate is private and non-Clone. Its exclusive borrow represents
        // the one classifier that holds the table's structural write guard;
        // concurrent accesses only read the state and accept both READY and
        // PUBLISHED. Keep the checked load so sequential misuse still fails.
        if entry.state.load(Ordering::Acquire) != SLOT_READY {
            return Err(table_fault("illegal record registry state transition"));
        }
        entry.state.store(SLOT_PUBLISHED, Ordering::Release);
        Ok(())
    }

    fn prove_unpublished(&self, candidate: &Candidate) -> Result<(), AccessError> {
        let entry = self.entry(candidate.id)?;
        transition_slot(entry, SLOT_READY, SLOT_PROVEN_UNPUBLISHED)?;
        // The successful READY transition is the exact-once proof that this
        // candidate never became directory-reachable. Quota metadata only has
        // meaning until that publication disposition is known, so retaining
        // it in every stable read-hot registry slot would add no recovery
        // capability. A second caller loses the transition and cannot release
        // the same candidate's reservation twice.
        self.release_retained(candidate.key_bytes);
        Ok(())
    }

    fn mark_unknown(&self, candidate: &Candidate) -> Result<(), AccessError> {
        let entry = self.entry(candidate.id)?;
        transition_slot(entry, SLOT_READY, SLOT_PUBLICATION_UNKNOWN)
    }

    fn release_retained(&self, key_bytes: u64) {
        self.retained_key_bytes
            .fetch_sub(key_bytes, Ordering::AcqRel);
        self.retained_records.fetch_sub(1, Ordering::AcqRel);
    }

    fn release_retained_batch(&self, records: u64, key_bytes: u64) {
        self.retained_key_bytes
            .fetch_sub(key_bytes, Ordering::AcqRel);
        self.retained_records.fetch_sub(records, Ordering::AcqRel);
    }
}

#[cfg(debug_assertions)]
fn registry_slice_owns_address<T>(slots: &[T], address: usize) -> bool {
    let start = slots.as_ptr().expose_provenance();
    address.checked_sub(start).is_some_and(|offset| {
        offset % std::mem::size_of::<T>() == 0 && offset / std::mem::size_of::<T>() < slots.len()
    })
}

fn record_index(record_id: RecordId) -> Result<usize, AccessError> {
    usize::try_from(record_id.get() - 1).map_err(|_| AccessError::from(CapacityError::KeyLimit))
}

struct SegmentedRegistry {
    segments: Box<[OnceLock<RegistrySegment>]>,
    bounded_atomic_values: bool,
}

impl SegmentedRegistry {
    fn new(effective_id_limit: u64, bounded_atomic_values: bool) -> Result<Self, CapacityError> {
        let segment_slots = REGISTRY_SEGMENT_SLOTS as u64;
        let segment_count = effective_id_limit
            .checked_add(segment_slots - 1)
            .ok_or(CapacityError::BufferLimit)?
            / segment_slots;
        let segment_count =
            usize::try_from(segment_count).map_err(|_| CapacityError::BufferLimit)?;
        let mut segments = Vec::new();
        segments
            .try_reserve_exact(segment_count)
            .map_err(|_| CapacityError::BufferLimit)?;
        segments.resize_with(segment_count, OnceLock::new);
        Ok(Self {
            segments: segments.into_boxed_slice(),
            bounded_atomic_values,
        })
    }

    fn ensure_segment(
        &self,
        segment_index: usize,
        lock_domain: RecordLockDomain,
    ) -> Result<&RegistrySegment, AccessError> {
        let slot = self
            .segments
            .get(segment_index)
            .ok_or(CapacityError::BufferLimit)?;
        if let Some(segment) = slot.get() {
            return Ok(segment);
        }

        let candidate =
            RegistrySegment::new(segment_index, lock_domain, self.bounded_atomic_values)?;
        let _ = slot.set(candidate);
        slot.get()
            .ok_or_else(|| table_fault("record registry segment publication failed"))
    }

    fn claim_access(
        &self,
        index: usize,
        lock_domain: RecordLockDomain,
    ) -> Result<RegistrySlotAccess<'_>, AccessError> {
        let segment = self.ensure_segment(index / REGISTRY_SEGMENT_SLOTS, lock_domain)?;
        segment
            .arena
            .get(index % REGISTRY_SEGMENT_SLOTS)
            .ok_or_else(|| CapacityError::BufferLimit.into())
    }

    #[inline(always)]
    fn segment_and_slot(&self, index: usize) -> Result<(&RegistrySegment, usize), AccessError> {
        let segment_index = index / REGISTRY_SEGMENT_SLOTS;
        let slot_index = index % REGISTRY_SEGMENT_SLOTS;
        let segment = self
            .segments
            .get(segment_index)
            .and_then(OnceLock::get)
            .ok_or_else(|| table_fault("directory returned an unallocated RecordId"))?;
        Ok((segment, slot_index))
    }

    #[inline(always)]
    fn entry(&self, index: usize) -> Result<&RegistryEntry, AccessError> {
        self.access(index).map(|access| access.entry)
    }

    #[inline(always)]
    fn access(&self, index: usize) -> Result<RegistrySlotAccess<'_>, AccessError> {
        let (segment, slot_index) = self.segment_and_slot(index)?;
        segment
            .arena
            .get(slot_index)
            .ok_or_else(|| table_fault("record registry segment has no requested slot"))
    }

    #[inline(always)]
    fn entry_with_lock(
        &self,
        index: usize,
    ) -> Result<(&RegistryEntry, &Arc<RecordLockSegment>), AccessError> {
        let (segment, slot_index) = self.segment_and_slot(index)?;
        let entry = segment
            .arena
            .entry(slot_index)
            .ok_or_else(|| table_fault("record registry segment has no requested slot"))?;
        Ok((entry, segment.lock_segment(slot_index)))
    }
}

struct ContiguousRegistry {
    arena: RegistryArena,
    lock_segments: Box<[Arc<RecordLockSegment>]>,
}

impl ContiguousRegistry {
    fn new(
        effective_id_limit: u64,
        max_bytes: usize,
        lock_domain: RecordLockDomain,
        bounded_atomic_values: bool,
    ) -> Result<Self, CapacityError> {
        let slot_count = checked_registry_slot_count(effective_id_limit, bounded_atomic_values)?;
        let accounted_bytes = eager_registry_accounted_bytes(slot_count, bounded_atomic_values)?;
        if accounted_bytes > max_bytes {
            return Err(CapacityError::BufferLimit);
        }

        // The budget and all size arithmetic are validated before either
        // proportional allocation begins. Failure never silently selects the
        // segmented backend because that would invalidate benchmark intent.
        let arena = RegistryArena::allocate(slot_count, bounded_atomic_values)?;
        let lock_segments = build_record_lock_segments(&arena, 0, lock_domain)?;
        Ok(Self {
            arena,
            lock_segments,
        })
    }

    fn claim_access(&self, index: usize) -> Result<RegistrySlotAccess<'_>, AccessError> {
        self.arena
            .get(index)
            .ok_or_else(|| CapacityError::BufferLimit.into())
    }

    #[inline(always)]
    fn entry(&self, index: usize) -> Result<&RegistryEntry, AccessError> {
        self.arena
            .entry(index)
            .ok_or_else(|| table_fault("directory returned an unallocated RecordId"))
    }

    #[inline(always)]
    fn access(&self, index: usize) -> Result<RegistrySlotAccess<'_>, AccessError> {
        self.arena
            .get(index)
            .ok_or_else(|| table_fault("directory returned an unallocated RecordId"))
    }

    #[inline(always)]
    fn entry_with_lock(
        &self,
        index: usize,
    ) -> Result<(&RegistryEntry, &Arc<RecordLockSegment>), AccessError> {
        let entry = self.entry(index)?;
        let lock_segment = self
            .lock_segments
            .get(index / RECORD_LOCK_SEGMENT_SLOTS)
            .ok_or_else(|| table_fault("record registry lock target is missing"))?;
        Ok((entry, lock_segment))
    }
}

fn checked_registry_slot_count(
    effective_id_limit: u64,
    bounded_atomic_values: bool,
) -> Result<usize, CapacityError> {
    let slot_count = usize::try_from(effective_id_limit).map_err(|_| CapacityError::BufferLimit)?;
    let entry_size = if bounded_atomic_values {
        std::mem::size_of::<StableRegistryEntry>()
    } else {
        std::mem::size_of::<RegistryEntry>()
    };
    let allocation_bytes = slot_count
        .checked_mul(entry_size)
        .ok_or(CapacityError::BufferLimit)?;
    if allocation_bytes > isize::MAX as usize {
        return Err(CapacityError::BufferLimit);
    }
    Ok(slot_count)
}

fn record_lock_segment_count(slot_count: usize) -> Result<usize, CapacityError> {
    slot_count
        .checked_add(RECORD_LOCK_SEGMENT_SLOTS - 1)
        .map(|rounded| rounded / RECORD_LOCK_SEGMENT_SLOTS)
        .ok_or(CapacityError::BufferLimit)
}

fn eager_registry_accounted_bytes(
    slot_count: usize,
    bounded_atomic_values: bool,
) -> Result<usize, CapacityError> {
    if slot_count == 0 {
        return Ok(0);
    }
    let lock_count = record_lock_segment_count(slot_count)?;
    let arc_header_bytes = 2_usize
        .checked_mul(std::mem::size_of::<usize>())
        .ok_or(CapacityError::BufferLimit)?;
    let entry_size = if bounded_atomic_values {
        std::mem::size_of::<StableRegistryEntry>()
    } else {
        std::mem::size_of::<RegistryEntry>()
    };
    // Slots live in one exactly sized Box allocation. One thin Arc allocation
    // owns that concrete box and the immutable layout discriminant shared by
    // the registry and every lock target.
    let arena_owner_bytes = arc_header_bytes
        .checked_add(std::mem::size_of::<RegistryArenaStorage>())
        .ok_or(CapacityError::BufferLimit)?;
    let slot_bytes = slot_count
        .checked_mul(entry_size)
        .and_then(|bytes| bytes.checked_add(arena_owner_bytes))
        .ok_or(CapacityError::BufferLimit)?;
    let lock_pointer_bytes = lock_count
        .checked_mul(std::mem::size_of::<Arc<RecordLockSegment>>())
        .ok_or(CapacityError::BufferLimit)?;
    let one_lock_bytes = std::mem::size_of::<RecordLockSegment>()
        .checked_add(arc_header_bytes)
        .ok_or(CapacityError::BufferLimit)?;
    let lock_allocation_bytes = lock_count
        .checked_mul(one_lock_bytes)
        .ok_or(CapacityError::BufferLimit)?;
    slot_bytes
        .checked_add(lock_pointer_bytes)
        .and_then(|bytes| bytes.checked_add(lock_allocation_bytes))
        .ok_or(CapacityError::BufferLimit)
}

fn allocate_registry_slots(slot_count: usize) -> Result<Box<[RegistryEntry]>, CapacityError> {
    let allocation_bytes = slot_count
        .checked_mul(std::mem::size_of::<RegistryEntry>())
        .ok_or(CapacityError::BufferLimit)?;
    if allocation_bytes > isize::MAX as usize {
        return Err(CapacityError::BufferLimit);
    }
    let mut slots = Vec::new();
    slots
        .try_reserve_exact(slot_count)
        .map_err(|_| CapacityError::BufferLimit)?;
    slots.resize_with(slot_count, RegistryEntry::unallocated);
    Ok(slots.into_boxed_slice())
}

fn allocate_stable_registry_slots(
    slot_count: usize,
) -> Result<Box<[StableRegistryEntry]>, CapacityError> {
    let allocation_bytes = slot_count
        .checked_mul(std::mem::size_of::<StableRegistryEntry>())
        .ok_or(CapacityError::BufferLimit)?;
    if allocation_bytes > isize::MAX as usize {
        return Err(CapacityError::BufferLimit);
    }
    let mut slots = Vec::new();
    slots
        .try_reserve_exact(slot_count)
        .map_err(|_| CapacityError::BufferLimit)?;
    slots.resize_with(slot_count, StableRegistryEntry::unallocated);
    Ok(slots.into_boxed_slice())
}

fn build_record_lock_segments(
    arena: &RegistryArena,
    logical_base: usize,
    lock_domain: RecordLockDomain,
) -> Result<Box<[Arc<RecordLockSegment>]>, CapacityError> {
    let lock_count = record_lock_segment_count(arena.len())?;
    let mut lock_segments = Vec::new();
    lock_segments
        .try_reserve_exact(lock_count)
        .map_err(|_| CapacityError::BufferLimit)?;
    for lock_index in 0..lock_count {
        let physical_base = lock_index
            .checked_mul(RECORD_LOCK_SEGMENT_SLOTS)
            .ok_or(CapacityError::BufferLimit)?;
        let record_base = logical_base
            .checked_add(physical_base)
            .ok_or(CapacityError::BufferLimit)?;
        lock_segments.push(Arc::new(RecordLockSegment {
            arena: arena.clone(),
            logical_base: record_base,
            physical_base,
            lock_domain,
        }));
    }
    Ok(lock_segments.into_boxed_slice())
}

struct RegistrySegment {
    // The Arc allocation gives every published Record (and its inline
    // AtomicVersion) a stable address. Lock targets clone this Arc, so a core
    // lock frame keeps the exact record alive through detached-guard release.
    arena: RegistryArena,
    // Targets never point back to this owner: each owns only `slots`, so table
    // teardown cannot dangle a guard and the ownership graph has no cycle.
    lock_segments: Box<[Arc<RecordLockSegment>]>,
}

impl RegistrySegment {
    fn new(
        segment_index: usize,
        lock_domain: RecordLockDomain,
        bounded_atomic_values: bool,
    ) -> Result<Self, AccessError> {
        debug_assert_eq!(REGISTRY_SEGMENT_SLOTS % RECORD_LOCK_SEGMENT_SLOTS, 0);
        let logical_base = segment_index
            .checked_mul(REGISTRY_SEGMENT_SLOTS)
            .ok_or(CapacityError::BufferLimit)?;
        let arena = RegistryArena::allocate(REGISTRY_SEGMENT_SLOTS, bounded_atomic_values)?;
        let lock_segments = build_record_lock_segments(&arena, logical_base, lock_domain)?;
        debug_assert_eq!(
            lock_segments.len(),
            RECORD_LOCK_SEGMENTS_PER_REGISTRY_SEGMENT
        );
        Ok(Self {
            arena,
            lock_segments,
        })
    }

    fn lock_segment(&self, slot_index: usize) -> &Arc<RecordLockSegment> {
        &self.lock_segments[slot_index / RECORD_LOCK_SEGMENT_SLOTS]
    }
}

struct RecordLockSegment {
    // Many exact LockIdentities multiplex this target, but acquisition checks
    // the complete domain and the target's 16-record range before selecting
    // one inline version. Core deduplication therefore remains per identity,
    // never per shared target pointer.
    arena: RegistryArena,
    // The same target shape serves both backends. `logical_base` identifies
    // the first global RecordId index accepted by this target, while
    // `physical_base` identifies that record inside its owning Arc arena.
    logical_base: usize,
    physical_base: usize,
    lock_domain: RecordLockDomain,
}

impl RecordLockSegment {
    #[inline(always)]
    fn record_id_slot(
        &self,
        record_id: RecordId,
        phase: AdapterPhase,
    ) -> Result<usize, AdapterFault> {
        let index = usize::try_from(record_id.get() - 1)
            .map_err(|_| AdapterFault::new(phase, AdapterFaultKind::LockIdentityMismatch))?;
        let offset = index
            .checked_sub(self.logical_base)
            .filter(|offset| *offset < RECORD_LOCK_SEGMENT_SLOTS)
            .ok_or_else(|| AdapterFault::new(phase, AdapterFaultKind::LockIdentityMismatch))?;
        self.physical_base
            .checked_add(offset)
            .filter(|slot| *slot < self.arena.len())
            .ok_or_else(|| AdapterFault::new(phase, AdapterFaultKind::LockIdentityMismatch))
    }

    fn identity_record_slot(
        &self,
        identity: &LockIdentity,
        phase: AdapterPhase,
    ) -> Result<(RecordId, usize), AdapterFault> {
        if identity.runtime_id() != self.lock_domain.runtime_id
            || identity.namespace_id() != self.lock_domain.namespace
            || identity.class() != self.lock_domain.lock_class
        {
            return Err(AdapterFault::new(
                phase,
                AdapterFaultKind::LockIdentityMismatch,
            ));
        }
        let raw = identity
            .key()
            .as_u64()
            .ok_or_else(|| AdapterFault::new(phase, AdapterFaultKind::LockIdentityMismatch))?;
        let record_id = RecordId::new(raw)
            .ok_or_else(|| AdapterFault::new(phase, AdapterFaultKind::LockIdentityMismatch))?;
        let slot = self.record_id_slot(record_id, phase)?;
        Ok((record_id, slot))
    }

    fn access_at(
        &self,
        slot: usize,
        phase: AdapterPhase,
    ) -> Result<RegistrySlotAccess<'_>, AdapterFault> {
        let access = self
            .arena
            .get(slot)
            .ok_or_else(|| AdapterFault::invariant(phase))?;
        match access.entry.state.load(Ordering::Acquire) {
            SLOT_READY | SLOT_PUBLISHED => Ok(access),
            _ => Err(AdapterFault::invariant(phase)),
        }
    }

    fn record_at(&self, slot: usize, phase: AdapterPhase) -> Result<&Record, AdapterFault> {
        self.access_at(slot, phase).map(RegistrySlotAccess::record)
    }
}

struct RecordLockGuard {
    record_id: RecordId,
    slot: usize,
    detached: DetachedVersionGuard,
}

impl RecordLockGuard {
    fn before(&self) -> OccVersion {
        self.detached.before()
    }

    fn owner(&self) -> sto_core::OwnerId {
        self.detached.owner()
    }

    fn is_held(&self) -> bool {
        self.detached.is_held()
    }

    fn is_for(&self, record_id: RecordId, version: &AtomicVersion) -> bool {
        self.record_id == record_id && self.detached.is_for(version)
    }
}

impl TransactionLock for RecordLockSegment {
    type Guard = RecordLockGuard;

    fn try_acquire(
        &self,
        identity: &LockIdentity,
        cx: &AcquireContext<'_>,
    ) -> Result<Self::Guard, AcquireError> {
        let (record_id, slot) = self.identity_record_slot(identity, AdapterPhase::Acquire)?;
        let record = self.record_at(slot, AdapterPhase::Acquire)?;
        let detached = record.version.try_acquire_detached(cx.owner())?;
        Ok(RecordLockGuard {
            record_id,
            slot,
            detached,
        })
    }

    #[allow(
        unsafe_code,
        reason = "the retained record-lock segment proves stable inline version identity"
    )]
    fn release(
        &self,
        guard: &mut Self::Guard,
        disposition: LockDisposition,
        cx: &ReleaseContext<'_>,
    ) {
        let record = self
            .record_at(guard.slot, AdapterPhase::Release)
            .unwrap_or_else(|error| panic!("sto-masstree record lock target invariant: {error}"));
        if guard.owner() != cx.owner() || !guard.detached.is_for(&record.version) {
            panic!("sto-masstree record lock received a mismatched detached guard");
        }

        // SAFETY: core retains this exact Arc<RecordLockSegment> through the
        // callback. The segment owns a clone of the RegistryArena Arc, whose
        // record entries live in one boxed slice and are never moved, reused,
        // or reinitialized. The owner/address checks above bind the unique
        // held guard to this inline version, so no legal operation can change
        // its word between the checked load and release store.
        let result = unsafe {
            match disposition {
                LockDisposition::Aborted => guard
                    .detached
                    .release_abort_stable_target(&record.version)
                    .map(|()| None),
                LockDisposition::Committed {
                    occ_commit_id: Some(commit_id),
                } => guard
                    .detached
                    .release_commit_stable_target(&record.version, commit_id)
                    .map(Some),
                LockDisposition::Committed {
                    occ_commit_id: None,
                } => panic!("sto-masstree committed record write has no OCC commit ID"),
                LockDisposition::Indeterminate { occ_commit_id } => guard
                    .detached
                    .release_indeterminate_stable_target(&record.version, occ_commit_id)
                    .map(Some),
            }
        };
        if let Err(error) = result {
            panic!("sto-masstree record version release failed: {error}");
        }
    }
}

struct DirectRecordLockGuard {
    record_token: RecordId,
    record: direct_record::CachedRecord,
    detached: DetachedVersionGuard,
}

impl DirectRecordLockGuard {
    fn before(&self) -> OccVersion {
        self.detached.before()
    }

    fn owner(&self) -> sto_core::OwnerId {
        self.detached.owner()
    }

    fn is_held(&self) -> bool {
        self.detached.is_held()
    }

    fn is_for(&self, record_token: RecordId, version: &AtomicVersion) -> bool {
        self.record_token == record_token && self.detached.is_for(version)
    }

    fn record<'table>(
        &self,
        table: &'table TableShared,
        phase: AdapterPhase,
    ) -> Result<&'table Record, AdapterFault> {
        self.access(table, phase).map(|access| access.record)
    }

    fn access<'table>(
        &self,
        table: &'table TableShared,
        phase: AdapterPhase,
    ) -> Result<RecordAccess<'table>, AdapterFault> {
        if table.record_token_mode != RecordTokenMode::DirectRecordPointer {
            return Err(AdapterFault::new(
                phase,
                AdapterFaultKind::LockIdentityMismatch,
            ));
        }
        self.record
            .get(&table.registry, self.record_token)
            .map_err(|error| {
                table.note_access_error(&error);
                AdapterFault::invariant(phase)
            })
    }
}

impl TableShared {
    fn direct_identity_record(
        &self,
        identity: &LockIdentity,
        phase: AdapterPhase,
    ) -> Result<RecordAccess<'_>, AdapterFault> {
        if self.record_token_mode != RecordTokenMode::DirectRecordPointer
            || identity.runtime_id() != self.runtime_id
            || identity.namespace_id() != self.namespace
            || identity.class() != self.record_lock_class
        {
            return Err(AdapterFault::new(
                phase,
                AdapterFaultKind::LockIdentityMismatch,
            ));
        }
        let raw = identity
            .key()
            .as_u64()
            .ok_or_else(|| AdapterFault::new(phase, AdapterFaultKind::LockIdentityMismatch))?;
        let record_token = RecordId::new(raw)
            .ok_or_else(|| AdapterFault::new(phase, AdapterFaultKind::LockIdentityMismatch))?;
        self.resolve_directory_access(record_token)
            .map_err(|_| AdapterFault::new(phase, AdapterFaultKind::LockIdentityMismatch))
    }
}

/// Direct-mode guards cache one stable record address after the target has
/// validated its private address token. Both the ordinary fallback plan and
/// the borrowed direct plan retain the owning table through guard release.
impl TransactionLock for TableShared {
    type Guard = DirectRecordLockGuard;

    fn try_acquire(
        &self,
        identity: &LockIdentity,
        cx: &AcquireContext<'_>,
    ) -> Result<Self::Guard, AcquireError> {
        let access = self.direct_identity_record(identity, AdapterPhase::Acquire)?;
        let detached = access.record.version.try_acquire_detached(cx.owner())?;
        Ok(DirectRecordLockGuard {
            record_token: access.record_id,
            record: direct_record::CachedRecord::new(access),
            detached,
        })
    }

    #[allow(
        unsafe_code,
        reason = "the retained direct table proves stable cached-record version identity"
    )]
    fn release(
        &self,
        guard: &mut Self::Guard,
        disposition: LockDisposition,
        cx: &ReleaseContext<'_>,
    ) {
        let record = guard
            .record(self, AdapterPhase::Release)
            .unwrap_or_else(|error| panic!("sto-masstree direct lock target invariant: {error}"));
        if guard.owner() != cx.owner() || !guard.detached.is_for(&record.version) {
            panic!("sto-masstree direct record lock received a mismatched detached guard");
        }

        // SAFETY: core retains this exact TableShared through the ordinary
        // Arc target or through the direct plan's live TableAdapter item. Its
        // registry owns append-only boxed arenas whose entries are never moved,
        // reused, or reinitialized. CachedRecord reborrows the entry under that
        // registry, and the owner/address checks above bind the unique held
        // guard to its inline version. No legal operation can change the word
        // between the checked load and release store.
        let result = unsafe {
            match disposition {
                LockDisposition::Aborted => guard
                    .detached
                    .release_abort_stable_target(&record.version)
                    .map(|()| None),
                LockDisposition::Committed {
                    occ_commit_id: Some(commit_id),
                } => guard
                    .detached
                    .release_commit_stable_target(&record.version, commit_id)
                    .map(Some),
                LockDisposition::Committed {
                    occ_commit_id: None,
                } => panic!("sto-masstree committed direct record write has no OCC commit ID"),
                LockDisposition::Indeterminate { occ_commit_id } => guard
                    .detached
                    .release_indeterminate_stable_target(&record.version, occ_commit_id)
                    .map(Some),
            }
        };
        if let Err(error) = result {
            panic!("sto-masstree direct record version release failed: {error}");
        }
    }
}

// SAFETY: a TableShared in direct-record mode owns the private Masstree
// directory that mints every RecordId token from its stable, append-only
// registry. The exact TableShared address therefore supplies the namespace and
// fixes the record-lock class, while the unforgeable DirectRecordLockToken
// supplies the physical record. Runtime, mode, pointer shape, registry state,
// and the OCC word are checked before a guard is constructed. The owning
// transaction item retains this TableShared through release and guard
// destruction.
#[allow(
    unsafe_code,
    reason = "the private direct-record directory proves the exact target/token lock identity"
)]
unsafe impl DirectTokenLock for TableShared {
    type Token = DirectRecordLockToken;

    fn try_acquire_token(
        &self,
        runtime_id: RuntimeId,
        record_token: Self::Token,
        cx: &AcquireContext<'_>,
    ) -> Result<Self::Guard, AcquireError> {
        if self.record_token_mode != RecordTokenMode::DirectRecordPointer
            || self.runtime_id != runtime_id
        {
            return Err(AdapterFault::new(
                AdapterPhase::Acquire,
                AdapterFaultKind::LockIdentityMismatch,
            )
            .into());
        }
        let observed = record_token.observed();
        let record_id = record_token.record_id();
        let access = record_token
            .record
            .get(&self.registry, record_id)
            .map_err(|_| AdapterFault::invariant(AdapterPhase::Acquire))?;
        let detached = access
            .record
            .version
            .try_acquire_detached_observed(observed, cx.owner())?;
        Ok(DirectRecordLockGuard {
            record_token: record_id,
            record: record_token.record,
            detached,
        })
    }
}

/// Unforgeable proof that a direct record token came from this crate's private
/// native-directory path.
///
/// The field and minting operation stay private. Safe generic code can copy a
/// value supplied by the adapter but cannot turn an arbitrary public
/// `RecordId` into a token accepted by [`DirectTokenLock`].
#[derive(Clone, Copy, Eq, PartialEq)]
struct DirectRecordLockToken {
    observed: OccVersion,
    record: direct_record::CachedRecord,
}

impl DirectRecordLockToken {
    /// Mints a token after the caller has established the private-directory
    /// provenance invariant documented by `direct_record`.
    ///
    /// `access` must come from the direct-mode table's private directory, and
    /// `observed` must describe that same physical record.
    fn from_private_access(access: RecordAccess<'_>, observed: OccVersion) -> Self {
        Self {
            observed,
            record: direct_record::CachedRecord::new(access),
        }
    }

    fn record_id(self) -> RecordId {
        self.record.record_id()
    }

    const fn observed(self) -> OccVersion {
        self.observed
    }
}

fn reserve_atomic(counter: &AtomicU64, amount: u64, limit: u64) -> Result<(), ()> {
    counter
        .fetch_update(Ordering::AcqRel, Ordering::Acquire, |current| {
            current.checked_add(amount).filter(|next| *next <= limit)
        })
        .map(|_| ())
        .map_err(|_| ())
}

fn transition_slot(entry: &RegistryEntry, from: u8, to: u8) -> Result<(), AccessError> {
    entry
        .state
        .compare_exchange(from, to, Ordering::AcqRel, Ordering::Acquire)
        .map(|_| ())
        .map_err(|_| table_fault("illegal record registry state transition"))
}

#[derive(Debug)]
struct Candidate {
    id: RecordId,
    key_bytes: u64,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum CandidateBatchReservation {
    Reserved,
    RetryScalar,
}

const REGISTRY_ENTRY_PADDING_BYTES: usize =
    REGISTRY_ENTRY_SLOT_BYTES - std::mem::size_of::<Record>() - std::mem::size_of::<AtomicU8>();

/// Atomic suffix storage for an opted-in bounded value.
///
/// The owning record supplies bytes `0..INLINE_VALUE_CAPACITY` and the stable
/// descriptor. Every runtime payload access remains atomic; a reader closes
/// the normal OCC version sandwich before exposing its reconstructed bytes.
#[repr(C)]
struct StableAtomicValueCell {
    suffix: [AtomicU64; STABLE_ATOMIC_VALUE_SUFFIX_WORDS],
}

impl StableAtomicValueCell {
    fn empty() -> Self {
        Self {
            suffix: std::array::from_fn(|_| AtomicU64::new(0)),
        }
    }

    #[inline]
    fn store_suffix(&self, bytes: &[u8]) {
        debug_assert!(bytes.len() > INLINE_VALUE_CAPACITY);
        debug_assert!(bytes.len() <= STABLE_ATOMIC_VALUE_CAPACITY);
        let suffix = &bytes[INLINE_VALUE_CAPACITY..];
        let mut full_chunks = suffix.chunks_exact(std::mem::size_of::<u64>());
        for (index, chunk) in full_chunks.by_ref().enumerate() {
            let word = u64::from_ne_bytes(
                chunk
                    .try_into()
                    .expect("an exact eight-byte suffix chunk must form one word"),
            );
            self.suffix[index].store(word, Ordering::Relaxed);
        }
        let tail = full_chunks.remainder();
        if !tail.is_empty() {
            let mut word = [0_u8; std::mem::size_of::<u64>()];
            word[..tail.len()].copy_from_slice(tail);
            self.suffix[suffix.len() / std::mem::size_of::<u64>()]
                .store(u64::from_ne_bytes(word), Ordering::Relaxed);
        }
    }

    #[inline]
    fn load_suffix(&self, length: usize, output: &mut [u8]) {
        debug_assert!(length > INLINE_VALUE_CAPACITY);
        debug_assert!(length <= STABLE_ATOMIC_VALUE_CAPACITY);
        debug_assert!(output.len() >= length);
        let suffix_length = length - INLINE_VALUE_CAPACITY;
        let suffix_output = &mut output[INLINE_VALUE_CAPACITY..length];
        let mut full_chunks = suffix_output.chunks_exact_mut(std::mem::size_of::<u64>());
        for (index, chunk) in full_chunks.by_ref().enumerate() {
            let chunk: &mut [u8; std::mem::size_of::<u64>()] = chunk
                .try_into()
                .expect("an exact eight-byte suffix output must form one word");
            *chunk = self.suffix[index].load(Ordering::Acquire).to_ne_bytes();
        }
        let tail = full_chunks.into_remainder();
        if !tail.is_empty() {
            let word = self.suffix[suffix_length / std::mem::size_of::<u64>()]
                .load(Ordering::Acquire)
                .to_ne_bytes();
            tail.copy_from_slice(&word[..tail.len()]);
        }
    }
}

#[repr(C, align(64))]
struct RegistryEntry {
    // Put the read-hot record first. Publication state is the only per-slot
    // metadata needed after candidate resolution; retained quota belongs to
    // the short-lived Candidate until its publication disposition is known.
    record: Record,
    state: AtomicU8,
    // Five atomic payload words make the read-hot record 56 bytes. Pad the
    // physical arena slot to an explicit 64-byte stride and alignment on the
    // supported 64-bit layout, keeping its complete Record in one cache line.
    _slot_padding: [u8; REGISTRY_ENTRY_PADDING_BYTES],
}

impl RegistryEntry {
    fn unallocated() -> Self {
        Self {
            record: Record {
                version: AtomicVersion::default(),
                state: CommittedRecordState::tombstone(),
            },
            state: AtomicU8::new(SLOT_UNALLOCATED),
            _slot_padding: [0; REGISTRY_ENTRY_PADDING_BYTES],
        }
    }
}

/// Extended physical entry selected only for opted-in tables.
///
/// `base` is deliberately the offset-zero field. Private direct-directory
/// tokens encode the address of this concrete object, and decoding selects
/// the concrete arena before borrowing either field.
#[repr(C, align(64))]
struct StableRegistryEntry {
    base: RegistryEntry,
    cell: StableAtomicValueCell,
}

impl StableRegistryEntry {
    fn unallocated() -> Self {
        Self {
            base: RegistryEntry::unallocated(),
            cell: StableAtomicValueCell::empty(),
        }
    }
}

/// A registry-validated record borrow for one synchronous item operation.
///
/// This type deliberately has no owned or raw-pointer representation. Its
/// lifetime prevents it from entering transaction state, reusable scan
/// scratch, or an FFI-resolved token.
#[derive(Clone, Copy)]
struct RecordAccess<'record> {
    record_id: RecordId,
    record: &'record Record,
    stable: Option<&'record StableAtomicValueCell>,
}

/// One resolved token after its table identity has been checked. Its table
/// borrow prevents this operation-local proof from entering a reusable
/// external cache. Stable-slot resolution remains inside the transaction
/// failure boundary and is skipped entirely for a staged read.
#[derive(Clone, Copy)]
struct ValidatedResolved<'table> {
    record_id: RecordId,
    _table: std::marker::PhantomData<&'table Table>,
}

struct Record {
    version: AtomicVersion,
    state: CommittedRecordState,
}

#[derive(Clone, Debug, Eq, PartialEq)]
enum RecordState {
    Tombstone,
    Live(Value),
}

impl RecordState {
    #[inline]
    fn tombstone() -> Self {
        Self::Tombstone
    }

    #[inline(always)]
    fn value(&self) -> Option<&Value> {
        match self {
            Self::Tombstone => None,
            Self::Live(value) => Some(value),
        }
    }

    #[inline(always)]
    fn is_live(&self) -> bool {
        matches!(self, Self::Live(_))
    }
}

/// Race-free committed storage specialized for the common short-value case.
///
/// The enclosing record's OCC version is observed before `load` and checked
/// afterwards. Writers exclusively hold that version while publishing. This
/// makes independently atomic descriptor/payload observations sufficient: a
/// reader that overlaps any publication must reject its version sandwich,
/// while a reader that observes the new version through Acquire also observes
/// the payload stores sequenced before the writer's Release unlock.
///
/// Values through 38 bytes occupy four full atomic words plus the low 48 bits
/// of a fifth word and need neither allocation nor reference-count traffic.
/// The high 16 bits of that fifth word carry the descriptor. Accessing both
/// fields through the same `AtomicU64` avoids mixed-size atomic accesses while
/// reclaiming the former descriptor padding for an embedded large-value slot.
/// ArcSwap publishes a thin `Arc<SharedValue>` and gives readers a protected
/// strong-reference clone without a mutex. Writers already hold the OCC
/// version exclusively; the surrounding version sandwich rejects any reader
/// that overlaps a slot replacement or removal.
#[repr(C)]
struct CommittedRecordState {
    inline_head: [AtomicU64; INLINE_VALUE_HEAD_WORDS],
    tail_and_descriptor: AtomicU64,
    shared: ArcSwapOption<SharedValue>,
}

enum CommittedStateLoad {
    Complete { state: RecordState, shared: bool },
    Incomplete(&'static str),
}

/// Operation-scoped committed-state storage for byte visitors.
///
/// The shared variant deliberately retains ArcSwap's guard instead of cloning
/// its `Arc`. No reference derived from this lease is allowed to escape the
/// HRTB visitor that owns the lease's dynamic scope.
enum CommittedStateLease {
    Tombstone,
    Inline {
        len: u8,
        bytes: [u8; INLINE_VALUE_CAPACITY],
    },
    Stable {
        len: u8,
        bytes: [u8; STABLE_ATOMIC_VALUE_CAPACITY],
    },
    Shared(ArcSwapGuard<Option<Arc<SharedValue>>>),
}

impl CommittedStateLease {
    #[inline(always)]
    fn value(&self) -> Option<&[u8]> {
        match self {
            Self::Tombstone => None,
            Self::Inline { len, bytes } => Some(&bytes[..usize::from(*len)]),
            Self::Stable { len, bytes } => Some(&bytes[..usize::from(*len)]),
            Self::Shared(lease) => Some(
                lease
                    .as_ref()
                    .expect("a complete shared-state lease retains its value")
                    .as_bytes(),
            ),
        }
    }

    #[inline(always)]
    fn is_live(&self) -> bool {
        !matches!(self, Self::Tombstone)
    }

    #[inline(always)]
    fn uses_shared_storage(&self) -> bool {
        matches!(self, Self::Shared(_))
    }
}

enum CommittedStateLeaseLoad {
    Complete(CommittedStateLease),
    Incomplete(&'static str),
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct RecordStateMetadata {
    live: bool,
    shared: bool,
}

#[derive(Debug, Eq, PartialEq)]
enum CommittedStateMetadataLoad {
    Complete(RecordStateMetadata),
    Incomplete(&'static str),
}

impl CommittedRecordState {
    fn tombstone() -> Self {
        Self {
            inline_head: std::array::from_fn(|_| AtomicU64::new(0)),
            tail_and_descriptor: AtomicU64::new(pack_record_state_word(RECORD_STATE_TOMBSTONE, 0)),
            shared: ArcSwapOption::from(None),
        }
    }

    #[inline(always)]
    fn load_inline_prefix(&self, tail_and_descriptor: u64, length: usize, output: &mut [u8]) {
        debug_assert!(length <= INLINE_VALUE_CAPACITY);
        debug_assert!(output.len() >= length);
        let head_length = length.min(INLINE_VALUE_HEAD_WORDS * std::mem::size_of::<u64>());
        let word_count = head_length.div_ceil(std::mem::size_of::<u64>());
        for index in 0..word_count {
            let word = self.inline_head[index]
                .load(Ordering::Acquire)
                .to_ne_bytes();
            let start = index * std::mem::size_of::<u64>();
            let end = (start + std::mem::size_of::<u64>()).min(head_length);
            output[start..end].copy_from_slice(&word[..end - start]);
        }
        unpack_inline_tail(tail_and_descriptor, length, output);
    }

    /// Loads only the atomic descriptor needed by presence-oriented APIs.
    ///
    /// The enclosing OCC sandwich gives this descriptor the same consistency
    /// proof as `load`; head payload words and the embedded ArcSwap slot are
    /// untouched.
    #[inline(always)]
    fn load_metadata(&self, stable: Option<&StableAtomicValueCell>) -> CommittedStateMetadataLoad {
        let tail_and_descriptor = self.tail_and_descriptor.load(Ordering::Acquire);
        let descriptor = record_state_descriptor(tail_and_descriptor);
        let inline_length = descriptor.wrapping_sub(RECORD_STATE_INLINE_BASE);
        if usize::from(inline_length) <= INLINE_VALUE_CAPACITY {
            return CommittedStateMetadataLoad::Complete(RecordStateMetadata {
                live: true,
                shared: false,
            });
        }

        if stable_record_state_length(descriptor).is_some() {
            return if stable.is_some() {
                CommittedStateMetadataLoad::Complete(RecordStateMetadata {
                    live: true,
                    shared: false,
                })
            } else {
                CommittedStateMetadataLoad::Incomplete(
                    "stable record descriptor has no bounded atomic cell",
                )
            };
        }

        match descriptor {
            RECORD_STATE_TOMBSTONE => CommittedStateMetadataLoad::Complete(RecordStateMetadata {
                live: false,
                shared: false,
            }),
            RECORD_STATE_SHARED => CommittedStateMetadataLoad::Complete(RecordStateMetadata {
                live: true,
                shared: true,
            }),
            RECORD_STATE_UPDATING => CommittedStateMetadataLoad::Incomplete(
                "committed record publication remained in progress",
            ),
            RECORD_STATE_POISONED => {
                CommittedStateMetadataLoad::Incomplete("committed record state is poisoned")
            }
            _ => CommittedStateMetadataLoad::Incomplete(
                "committed record state descriptor is invalid",
            ),
        }
    }

    #[inline(always)]
    fn load(&self, stable: Option<&StableAtomicValueCell>) -> CommittedStateLoad {
        // This word must be sampled first. Its Acquire synchronizes the head
        // payload stores for this generation and gives us one indivisible
        // descriptor/tail observation.
        let tail_and_descriptor = self.tail_and_descriptor.load(Ordering::Acquire);
        let descriptor = record_state_descriptor(tail_and_descriptor);
        let inline_length = descriptor.wrapping_sub(RECORD_STATE_INLINE_BASE);
        if usize::from(inline_length) <= INLINE_VALUE_CAPACITY {
            let length = usize::from(inline_length);
            let mut bytes = [0_u8; INLINE_VALUE_CAPACITY];
            self.load_inline_prefix(tail_and_descriptor, length, &mut bytes);
            return CommittedStateLoad::Complete {
                state: RecordState::Live(Value {
                    repr: ValueRepr::Inline {
                        len: inline_length as u8,
                        bytes,
                    },
                }),
                shared: false,
            };
        }

        if let Some(length) = stable_record_state_length(descriptor) {
            let Some(stable) = stable else {
                return CommittedStateLoad::Incomplete(
                    "stable record descriptor has no bounded atomic cell",
                );
            };
            let mut bytes = [0_u8; STABLE_ATOMIC_VALUE_CAPACITY];
            self.load_inline_prefix(tail_and_descriptor, INLINE_VALUE_CAPACITY, &mut bytes);
            stable.load_suffix(length, &mut bytes);
            return CommittedStateLoad::Complete {
                state: RecordState::Live(Value::from_slice(&bytes[..length])),
                shared: false,
            };
        }

        match descriptor {
            RECORD_STATE_TOMBSTONE => CommittedStateLoad::Complete {
                state: RecordState::Tombstone,
                shared: false,
            },
            RECORD_STATE_SHARED => match self.shared.load_full() {
                Some(bytes) => CommittedStateLoad::Complete {
                    state: RecordState::Live(Value {
                        repr: ValueRepr::Shared(bytes),
                    }),
                    shared: true,
                },
                // A delayed reader can sample the old Shared descriptor before a
                // shared-to-inline publication clears the embedded slot. Its
                // following version check must fail, so do not misclassify
                // this safe transient as corruption until after validation.
                None => CommittedStateLoad::Incomplete(
                    "committed large record publication is incomplete",
                ),
            },
            RECORD_STATE_UPDATING => {
                CommittedStateLoad::Incomplete("committed record publication remained in progress")
            }
            RECORD_STATE_POISONED => {
                CommittedStateLoad::Incomplete("committed record state is poisoned")
            }
            _ => CommittedStateLoad::Incomplete("committed record state descriptor is invalid"),
        }
    }

    /// Loads one committed value for a synchronous byte visitor.
    ///
    /// Descriptor and inline sampling intentionally match `load`. The shared
    /// path differs only in retaining ArcSwap's operation-local guard rather
    /// than materializing a strong-reference clone.
    #[inline(always)]
    fn load_lease(&self, stable: Option<&StableAtomicValueCell>) -> CommittedStateLeaseLoad {
        // Sample the descriptor/tail first. Its Acquire pairs with publication
        // exactly as in the owned load path and precedes every payload read.
        let tail_and_descriptor = self.tail_and_descriptor.load(Ordering::Acquire);
        let descriptor = record_state_descriptor(tail_and_descriptor);
        let inline_length = descriptor.wrapping_sub(RECORD_STATE_INLINE_BASE);
        if usize::from(inline_length) <= INLINE_VALUE_CAPACITY {
            let length = usize::from(inline_length);
            let mut bytes = [0_u8; INLINE_VALUE_CAPACITY];
            self.load_inline_prefix(tail_and_descriptor, length, &mut bytes);
            return CommittedStateLeaseLoad::Complete(CommittedStateLease::Inline {
                len: inline_length as u8,
                bytes,
            });
        }

        if let Some(length) = stable_record_state_length(descriptor) {
            let Some(stable) = stable else {
                return CommittedStateLeaseLoad::Incomplete(
                    "stable record descriptor has no bounded atomic cell",
                );
            };
            let mut bytes = [0_u8; STABLE_ATOMIC_VALUE_CAPACITY];
            self.load_inline_prefix(tail_and_descriptor, INLINE_VALUE_CAPACITY, &mut bytes);
            stable.load_suffix(length, &mut bytes);
            return CommittedStateLeaseLoad::Complete(CommittedStateLease::Stable {
                len: length as u8,
                bytes,
            });
        }

        match descriptor {
            RECORD_STATE_TOMBSTONE => {
                CommittedStateLeaseLoad::Complete(CommittedStateLease::Tombstone)
            }
            RECORD_STATE_SHARED => {
                let lease = self.shared.load();
                if lease.is_some() {
                    CommittedStateLeaseLoad::Complete(CommittedStateLease::Shared(lease))
                } else {
                    // Validate the enclosing OCC sandwich before deciding
                    // whether this is a racing replacement or stable damage.
                    CommittedStateLeaseLoad::Incomplete(
                        "committed large record publication is incomplete",
                    )
                }
            }
            RECORD_STATE_UPDATING => CommittedStateLeaseLoad::Incomplete(
                "committed record publication remained in progress",
            ),
            RECORD_STATE_POISONED => {
                CommittedStateLeaseLoad::Incomplete("committed record state is poisoned")
            }
            _ => {
                CommittedStateLeaseLoad::Incomplete("committed record state descriptor is invalid")
            }
        }
    }

    /// Captures only the inline words used by `length`, closes the OCC
    /// sandwich, and then writes the captured scalars to `output`.
    ///
    /// Keeping the payload in scalar locals avoids both the 38-byte temporary
    /// used by `load_lease` and a payload-carrying enum. No output byte is
    /// touched until the final version validation succeeds.
    #[inline(always)]
    fn copy_inline_after_observation(
        &self,
        version: &AtomicVersion,
        observed: OccVersion,
        tail_and_descriptor: u64,
        length: usize,
        output: &mut [u8],
    ) -> Result<ValueCopyOutcome, AccessError> {
        debug_assert!(length <= INLINE_VALUE_CAPACITY);
        if output.len() < length {
            if !version.validate(observed) {
                return Err(Conflict::ReadValidation.into());
            }
            return Ok(ValueCopyOutcome::BufferTooSmall { required: length });
        }

        let head_length = length.min(INLINE_VALUE_HEAD_WORDS * std::mem::size_of::<u64>());
        let word_count = head_length.div_ceil(std::mem::size_of::<u64>());
        match word_count {
            0 => {
                if !version.validate(observed) {
                    return Err(Conflict::ReadValidation.into());
                }
            }
            1 => {
                let word0 = self.inline_head[0].load(Ordering::Acquire);
                if !version.validate(observed) {
                    return Err(Conflict::ReadValidation.into());
                }
                copy_inline_head_word(output, 0, head_length, word0);
            }
            2 => {
                let word0 = self.inline_head[0].load(Ordering::Acquire);
                let word1 = self.inline_head[1].load(Ordering::Acquire);
                if !version.validate(observed) {
                    return Err(Conflict::ReadValidation.into());
                }
                copy_inline_head_word(output, 0, 8, word0);
                copy_inline_head_word(output, 8, head_length - 8, word1);
            }
            3 => {
                let word0 = self.inline_head[0].load(Ordering::Acquire);
                let word1 = self.inline_head[1].load(Ordering::Acquire);
                let word2 = self.inline_head[2].load(Ordering::Acquire);
                if !version.validate(observed) {
                    return Err(Conflict::ReadValidation.into());
                }
                copy_inline_head_word(output, 0, 8, word0);
                copy_inline_head_word(output, 8, 8, word1);
                copy_inline_head_word(output, 16, head_length - 16, word2);
            }
            4 => {
                let word0 = self.inline_head[0].load(Ordering::Acquire);
                let word1 = self.inline_head[1].load(Ordering::Acquire);
                let word2 = self.inline_head[2].load(Ordering::Acquire);
                let word3 = self.inline_head[3].load(Ordering::Acquire);
                if !version.validate(observed) {
                    return Err(Conflict::ReadValidation.into());
                }
                copy_inline_head_word(output, 0, 8, word0);
                copy_inline_head_word(output, 8, 8, word1);
                copy_inline_head_word(output, 16, 8, word2);
                copy_inline_head_word(output, 24, head_length - 24, word3);
            }
            _ => unreachable!("inline value uses at most four head words"),
        }

        let tail_length = length.saturating_sub(INLINE_VALUE_HEAD_WORDS * 8);
        if tail_length != 0 {
            let tail = tail_and_descriptor.to_le_bytes();
            output[INLINE_VALUE_HEAD_WORDS * 8..length].copy_from_slice(&tail[..tail_length]);
        }
        Ok(ValueCopyOutcome::Copied { len: length })
    }

    /// Captures one bounded atomic value in scalar scratch and writes each
    /// output byte once, after the enclosing OCC version sandwich closes.
    #[allow(
        unsafe_code,
        reason = "the capture loop initializes exactly the suffix-word prefix read after validation"
    )]
    #[inline(always)]
    fn copy_stable_after_observation(
        &self,
        stable: &StableAtomicValueCell,
        version: &AtomicVersion,
        observed: OccVersion,
        tail_and_descriptor: u64,
        length: usize,
        output: &mut [u8],
    ) -> Result<ValueCopyOutcome, AccessError> {
        debug_assert!(length > INLINE_VALUE_CAPACITY);
        debug_assert!(length <= STABLE_ATOMIC_VALUE_CAPACITY);
        if output.len() < length {
            if !version.validate(observed) {
                return Err(Conflict::ReadValidation.into());
            }
            return Ok(ValueCopyOutcome::BufferTooSmall { required: length });
        }

        let word0 = self.inline_head[0].load(Ordering::Acquire);
        let word1 = self.inline_head[1].load(Ordering::Acquire);
        let word2 = self.inline_head[2].load(Ordering::Acquire);
        let word3 = self.inline_head[3].load(Ordering::Acquire);
        let suffix_length = length - INLINE_VALUE_CAPACITY;
        let full_suffix_words = suffix_length / std::mem::size_of::<u64>();
        let suffix_tail_length = suffix_length % std::mem::size_of::<u64>();
        let suffix_word_count = full_suffix_words + usize::from(suffix_tail_length != 0);
        let mut suffix_words =
            [std::mem::MaybeUninit::<u64>::uninit(); STABLE_ATOMIC_VALUE_SUFFIX_WORDS];
        for (captured, source) in suffix_words[..suffix_word_count]
            .iter_mut()
            .zip(&stable.suffix)
        {
            captured.write(source.load(Ordering::Acquire));
        }
        if !version.validate(observed) {
            return Err(Conflict::ReadValidation.into());
        }

        copy_inline_head_word(output, 0, 8, word0);
        copy_inline_head_word(output, 8, 8, word1);
        copy_inline_head_word(output, 16, 8, word2);
        copy_inline_head_word(output, 24, 8, word3);
        let tail = tail_and_descriptor.to_le_bytes();
        output[INLINE_VALUE_HEAD_WORDS * 8..INLINE_VALUE_CAPACITY]
            .copy_from_slice(&tail[..INLINE_VALUE_TAIL_CAPACITY]);
        let suffix_output = &mut output[INLINE_VALUE_CAPACITY..length];
        for (index, captured) in suffix_words.iter().take(full_suffix_words).enumerate() {
            // SAFETY: The capture loop initialized every index below
            // `suffix_word_count`, which includes all complete suffix words.
            let word = unsafe { captured.assume_init_read() }.to_ne_bytes();
            let start = index * std::mem::size_of::<u64>();
            suffix_output[start..start + std::mem::size_of::<u64>()].copy_from_slice(&word);
        }
        if suffix_tail_length != 0 {
            // SAFETY: A partial suffix adds this exact slot to
            // `suffix_word_count`, so the capture loop initialized it.
            let word = unsafe { suffix_words[full_suffix_words].assume_init_read() }.to_ne_bytes();
            let start = full_suffix_words * std::mem::size_of::<u64>();
            suffix_output[start..].copy_from_slice(&word[..suffix_tail_length]);
        }
        Ok(ValueCopyOutcome::Copied { len: length })
    }

    fn begin_publication<'state>(
        &'state self,
        table: &'state TableShared,
        stable: Option<&'state StableAtomicValueCell>,
    ) -> CommittedStatePublication<'state> {
        // Once this marker is visible, all payload fields may be changed in
        // any order. A normal reader either observes the held OCC version or
        // rejects its final version check. The publication guard converts an
        // unwind into a permanent fail-closed marker.
        self.tail_and_descriptor.store(
            pack_record_state_word(RECORD_STATE_UPDATING, 0),
            Ordering::Release,
        );
        CommittedStatePublication {
            state: self,
            table,
            stable,
            completed: false,
        }
    }
}

struct CommittedStatePublication<'state> {
    state: &'state CommittedRecordState,
    table: &'state TableShared,
    stable: Option<&'state StableAtomicValueCell>,
    completed: bool,
}

impl CommittedStatePublication<'_> {
    fn publish(
        mut self,
        replacement: &RecordState,
        old_was_shared: bool,
        owner: OwnerId,
        commit_id: OccCommitId,
    ) {
        self.table
            .advance_scan_generation_for_commit(owner, commit_id);
        let final_word = match replacement {
            RecordState::Tombstone => {
                if old_was_shared {
                    self.clear_old_shared();
                }
                pack_record_state_word(RECORD_STATE_TOMBSTONE, 0)
            }
            RecordState::Live(value) => {
                let bytes = value.as_ref();
                let length = bytes.len();
                if length <= INLINE_VALUE_CAPACITY {
                    self.store_inline_prefix(bytes, length);
                    if old_was_shared {
                        self.clear_old_shared();
                    }
                    let descriptor = RECORD_STATE_INLINE_BASE
                        .checked_add(length as u16)
                        .expect("inline record-state descriptor cannot overflow");
                    pack_record_state_word(descriptor, pack_inline_tail(bytes, length))
                } else if length <= STABLE_ATOMIC_VALUE_CAPACITY && self.stable.is_some() {
                    let stable = self
                        .stable
                        .expect("bounded staged record value has no stable atomic cell");
                    self.store_inline_prefix(bytes, INLINE_VALUE_CAPACITY);
                    stable.store_suffix(bytes);
                    if old_was_shared {
                        self.clear_old_shared();
                    }
                    let descriptor = RECORD_STATE_STABLE_BASE
                        .checked_add(length as u16)
                        .expect("stable record-state descriptor cannot overflow");
                    pack_record_state_word(
                        descriptor,
                        pack_inline_tail(bytes, INLINE_VALUE_CAPACITY),
                    )
                } else {
                    let ValueRepr::Shared(bytes) = &value.repr else {
                        panic!("oversized bounded staged value has no immutable shared owner");
                    };
                    let previous = self.state.shared.swap(Some(Arc::clone(bytes)));
                    assert_eq!(
                        previous.is_some(),
                        old_was_shared,
                        "committed embedded large-value slot disagrees with its old descriptor"
                    );
                    drop(previous);
                    pack_record_state_word(RECORD_STATE_SHARED, 0)
                }
            }
        };

        // This is the linear physical-state publication. The transaction's
        // logical publication follows when the core Release-unlocks version.
        self.state
            .tail_and_descriptor
            .store(final_word, Ordering::Release);
        self.completed = true;
    }

    fn store_inline_prefix(&self, bytes: &[u8], length: usize) {
        debug_assert!(length <= INLINE_VALUE_CAPACITY);
        debug_assert!(bytes.len() >= length);
        let head_length = length.min(INLINE_VALUE_HEAD_WORDS * std::mem::size_of::<u64>());
        for (index, chunk) in bytes[..head_length]
            .chunks(std::mem::size_of::<u64>())
            .enumerate()
        {
            let mut word = [0_u8; std::mem::size_of::<u64>()];
            word[..chunk.len()].copy_from_slice(chunk);
            self.state.inline_head[index].store(u64::from_ne_bytes(word), Ordering::Relaxed);
        }
    }

    fn clear_old_shared(&self) {
        let previous = self.state.shared.swap(None);
        assert!(
            previous.is_some(),
            "committed shared descriptor has no embedded large-value payload"
        );
        drop(previous);
    }
}

impl Drop for CommittedStatePublication<'_> {
    fn drop(&mut self) {
        if !self.completed {
            self.state.tail_and_descriptor.store(
                pack_record_state_word(RECORD_STATE_POISONED, 0),
                Ordering::Release,
            );
            self.table.poison();
        }
    }
}

#[inline(always)]
const fn pack_record_state_word(descriptor: u16, tail: u64) -> u64 {
    ((descriptor as u64) << RECORD_STATE_DESCRIPTOR_SHIFT) | (tail & RECORD_STATE_TAIL_MASK)
}

#[inline(always)]
const fn record_state_descriptor(word: u64) -> u16 {
    (word >> RECORD_STATE_DESCRIPTOR_SHIFT) as u16
}

#[inline(always)]
fn stable_record_state_length(descriptor: u16) -> Option<usize> {
    let length = usize::from(descriptor.wrapping_sub(RECORD_STATE_STABLE_BASE));
    (length > INLINE_VALUE_CAPACITY && length <= STABLE_ATOMIC_VALUE_CAPACITY).then_some(length)
}

#[inline]
fn pack_inline_tail(bytes: &[u8], length: usize) -> u64 {
    let tail_length = length.saturating_sub(INLINE_VALUE_HEAD_WORDS * std::mem::size_of::<u64>());
    debug_assert!(tail_length <= INLINE_VALUE_TAIL_CAPACITY);
    let mut packed = 0_u64;
    for index in 0..tail_length {
        packed |= u64::from(bytes[INLINE_VALUE_HEAD_WORDS * std::mem::size_of::<u64>() + index])
            << (index * u8::BITS as usize);
    }
    packed
}

#[inline]
fn unpack_inline_tail(tail_and_descriptor: u64, length: usize, bytes: &mut [u8]) {
    let tail_length = length.saturating_sub(INLINE_VALUE_HEAD_WORDS * std::mem::size_of::<u64>());
    debug_assert!(tail_length <= INLINE_VALUE_TAIL_CAPACITY);
    for index in 0..tail_length {
        bytes[INLINE_VALUE_HEAD_WORDS * std::mem::size_of::<u64>() + index] =
            ((tail_and_descriptor >> (index * u8::BITS as usize)) & u64::from(u8::MAX)) as u8;
    }
}

// TPC-C's latency-sensitive item and stock batches contain at most 15 keys.
// Keep those on the allocation-free pairwise proof: even its 105-comparison
// worst case is cheaper than initializing and sorting an index on this path.
// Larger general batches use retained scratch. Fixed record batches take the
// exact hashed path below; callers that need a sorted permutation retain the
// comparison-indexed constructor.
const SMALL_UNIQUE_KEY_BATCH: usize = 32;

fn prove_unique_keys<'keys, K: sto_core::ResourceKey>(
    keys: &'keys [K],
    order: &mut Vec<usize>,
) -> Result<Option<UniqueItemKeys<'keys, K>>, AccessError> {
    if keys.len() <= SMALL_UNIQUE_KEY_BATCH {
        order.clear();
        Ok(UniqueItemKeys::try_new(keys))
    } else {
        UniqueItemKeys::try_new_indexed(keys, order).map_err(|_| CapacityError::BufferLimit.into())
    }
}

/// Proves unique resolved identities. When an identity repeats, validates the
/// directory binding through a cold sorted pass. A duplicate identity is legal
/// only for equal input keys; a foreign alias fails closed before any visitor
/// callback runs.
fn prove_unique_fixed_records<'items, const KEY_LENGTH: usize>(
    shared: &TableShared,
    keys: &[[u8; KEY_LENGTH]],
    record_ids: &[Option<RecordId>],
    item_keys: &'items [TableKey],
    unique_key_index: &mut UniqueItemKeyIndex,
    alias_order: &mut Vec<usize>,
) -> Result<Option<UniqueItemKeys<'items, TableKey>>, AccessError> {
    debug_assert_eq!(keys.len(), record_ids.len());
    debug_assert_eq!(item_keys.len(), record_ids.len());
    debug_assert!(record_ids.iter().all(Option::is_some));

    let unique = if item_keys.len() <= SMALL_UNIQUE_KEY_BATCH {
        alias_order.clear();
        UniqueItemKeys::try_new(item_keys)
    } else {
        UniqueItemKeys::try_new_hashed(item_keys, unique_key_index)
            .map_err(|_| CapacityError::BufferLimit)?
    };
    if unique.is_none() {
        // The hash proof deliberately retains no ordering. Duplicate input is
        // cold, so sort the resolved IDs only then and preserve the existing
        // exact foreign-alias validation before any callback runs.
        validate_fixed_record_aliases(shared, keys, record_ids, alias_order)?;
    }
    Ok(unique)
}

fn validate_fixed_record_aliases<const KEY_LENGTH: usize>(
    shared: &TableShared,
    keys: &[[u8; KEY_LENGTH]],
    record_ids: &[Option<RecordId>],
    alias_order: &mut Vec<usize>,
) -> Result<(), AccessError> {
    debug_assert_eq!(keys.len(), record_ids.len());
    alias_order.clear();
    alias_order.extend(
        record_ids
            .iter()
            .enumerate()
            .filter_map(|(index, record_id)| record_id.map(|_| index)),
    );
    alias_order.sort_unstable_by_key(|&index| {
        record_ids[index].expect("the alias order contains only present record IDs")
    });

    for adjacent in alias_order.windows(2) {
        let prior = adjacent[0];
        let current = adjacent[1];
        if record_ids[prior] == record_ids[current] && keys[prior] != keys[current] {
            shared.poison();
            return Err(table_fault(
                "distinct directory keys resolved to one record ID",
            ));
        }
    }
    Ok(())
}

#[inline(always)]
fn copy_inline_head_word(output: &mut [u8], offset: usize, length: usize, word: u64) {
    debug_assert!(length <= std::mem::size_of::<u64>());
    let bytes = word.to_ne_bytes();
    output[offset..offset + length].copy_from_slice(&bytes[..length]);
}

#[inline(always)]
fn copy_value_into(value: Option<&[u8]>, output: &mut [u8]) -> ValueCopyOutcome {
    let Some(value) = value else {
        return ValueCopyOutcome::Miss;
    };
    if output.len() < value.len() {
        return ValueCopyOutcome::BufferTooSmall {
            required: value.len(),
        };
    }
    output[..value.len()].copy_from_slice(value);
    ValueCopyOutcome::Copied { len: value.len() }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum AdapterRole {
    Record,
    Directory,
    Scan,
}

impl AdapterRole {
    #[inline(always)]
    const fn is_generation(self) -> bool {
        matches!(self, Self::Directory | Self::Scan)
    }
}

#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
enum TableKey {
    DirectoryGeneration,
    Record(RecordId),
}

struct TableAdapter {
    table: Arc<TableShared>,
    role: AdapterRole,
}

static RECORD_PREFLIGHT_FREE_READ: PreflightFreeReadCapability<TableAdapter> =
    // A committed prepared-free record read has no intent or Prepared value.
    // The compact observation retains only the OCC generation and two state
    // classification bits, so core-owned teardown is complete while retaining
    // the table binding. Aborted reads still run TableAdapter::finish.
    PreflightFreeReadCapability::new_drop_only(validate_record_preflight_free_read);

static RECORD_TERMINAL_READ_BATCH: TerminalReadBatchCapability<TableAdapter> =
    // A terminal record read retains no local, intent, or Prepared value.
    // Dropping its RecordId and compact RecordObservation is complete cleanup
    // after both commit and abort. The one retained table resource keeps the
    // adapter and registry alive through teardown.
    TerminalReadBatchCapability::new_drop_only(validate_record_preflight_free_read);

static DIRECTORY_PREFLIGHT_FREE_READ: PreflightFreeReadCapability<TableAdapter> =
    // A directory generation is an immutable transaction-local scalar. Final
    // equality validation is its complete commit protocol.
    PreflightFreeReadCapability::new_drop_only(validate_directory_preflight_free_read);

#[allow(
    unsafe_code,
    reason = "the adjacent proof binds write observations to exact token acquisition"
)]
static RECORD_DIRECT_INJECTIVE_LOCKS: BorrowedInjectiveLockCommitCapability<
    TableAdapter,
    TableShared,
> = {
    // SAFETY: prepare_direct_item stores the record observation in the private
    // token after checking every item shape. TableShared acquires the exact
    // record only if that generation remains current, and the returned guard
    // excludes writers through installation and release. install_direct_record
    // checks the target, record, owner, held state, and commit generation again.
    unsafe {
        BorrowedInjectiveLockCommitCapability::new(
            prepare_direct_item,
            validate_direct_item,
            install_direct_record,
        )
        .with_write_acquisition_certification()
    }
};

// Direct preparation proves the role plus the local, observation, and intent
// shapes for every item. Record write tokens bind that observation to lock
// acquisition, so final validation only revisits read-only items. A directory
// generation item emits no token and rechecks the same observation used by the
// ordinary prepared-free read lane after every record lock has been acquired.
// A writing item's direct install copies inline bytes or clones the immutable
// shared value into committed storage before returning, and the held OCC
// version is committed before item cleanup begins. The ordinary
// TableAdapter::finish callback would only repeat those shape checks and take
// a still-owned record intent; directory reads have no intent or other cleanup
// obligation. Core teardown can therefore perform the complete committed
// cleanup by dropping the remaining item fields. Abort still needs
// TableAdapter::finish and is not elided.
#[allow(
    unsafe_code,
    reason = "the adjacent proof discharges the direct capability's injectivity contract"
)]
static RECORD_DIRECT_COMMIT: DirectCommitCapability<TableAdapter> = {
    // SAFETY: this capability is exposed only in DirectRecordPointer mode.
    // Directory-role items are read-only and prepare_direct_item always maps
    // them to no token, so they do not participate in the injectivity proof.
    // Core deduplicates every full logical item identity (exact registered
    // binding plus TableKey) before commit. Within a record binding,
    // prepare_direct_item maps TableKey::Record(record_id) to that exact
    // RecordId, so distinct keys produce distinct target/token pairs. Each
    // TableShared is an independently retained target; distinct bindings
    // therefore remain distinct even if their scalar token values match.
    // Unsafe private-directory minting makes the compact token unforgeable by
    // safe code. Runtime identity is retained in BorrowedLockToken and
    // rechecked by both core and TableShared. DirectTokenLock performs the
    // ordinary mode, pointer-shape, registry-state, and OCC acquisition checks
    // before caching a record. No callback-visible state changes this map.
    unsafe { DirectCommitCapability::borrowed_injective_token_lock(&RECORD_DIRECT_INJECTIVE_LOCKS) }
        .with_drop_only_committed_finish()
};

struct RecordObservation {
    version: OccVersion,
    // Committed snapshots live only for the operation that reloads them.
    // These two bits describe the first version-sandwiched state and suffice
    // for repeated presence-only access and embedded shared-slot cleanup.
    original_live: bool,
    old_was_shared: bool,
}

#[derive(Clone, Copy)]
struct DirectoryObservation {
    generation: u64,
}

enum TableObservation {
    Record(RecordObservation),
    Directory(DirectoryObservation),
}

impl OpacityToken for TableObservation {
    fn observation_order(&self) -> ObservationOrder {
        match self {
            Self::Record(observation) => ObservationOrder::Ordered(observation.version),
            // Physical publication attempts are independent of the runtime's
            // OCC commit clock and require conservative full revalidation.
            Self::Directory(_) => ObservationOrder::Unordered,
        }
    }
}

enum TableIntent {
    Record { replacement: RecordState },
}

impl TableIntent {
    #[inline(always)]
    fn record_state(&self) -> &RecordState {
        match self {
            Self::Record { replacement, .. } => replacement,
        }
    }

    #[inline(always)]
    fn replace_record_state(&mut self, replacement: RecordState) {
        match self {
            Self::Record {
                replacement: current,
                ..
            } => *current = replacement,
        }
    }
}

struct TablePrepared {
    // At most one planned physical lock covers a record item. Directory
    // observations are prepared-free reads and never emit lock requests.
    lock_use: Option<ErasedLockUse>,
}

fn validate_record_preflight_free_read(
    adapter: &TableAdapter,
    key: &TableKey,
    observation: &TableObservation,
    _cx: &PreflightFreeValidationContext<'_>,
) -> Result<(), CheckError> {
    match (adapter.role, key, observation) {
        (
            AdapterRole::Record,
            TableKey::Record(record_id),
            TableObservation::Record(observation),
        ) => adapter.validate_read_only_observation(*record_id, observation),
        _ => Err(adapter.type_mismatch(AdapterPhase::Validation).into()),
    }
}

fn validate_directory_preflight_free_read(
    adapter: &TableAdapter,
    key: &TableKey,
    observation: &TableObservation,
    _cx: &PreflightFreeValidationContext<'_>,
) -> Result<(), CheckError> {
    match (key, observation) {
        (TableKey::DirectoryGeneration, TableObservation::Directory(observation))
            if adapter.role.is_generation() =>
        {
            adapter.validate_directory_observation(observation)
        }
        _ => Err(adapter.type_mismatch(AdapterPhase::Validation).into()),
    }
}

fn prepare_direct_item(
    adapter: &TableAdapter,
    key: &TableKey,
    item: PreflightItem<'_, TableAdapter>,
) -> Result<Option<BorrowedLockToken<DirectRecordLockToken>>, PrepareError> {
    if adapter.role.is_generation() && matches!(key, TableKey::DirectoryGeneration) {
        if adapter.table.record_token_mode != RecordTokenMode::DirectRecordPointer {
            return Err(AdapterFault::new(
                AdapterPhase::Preflight,
                AdapterFaultKind::LockIdentityMismatch,
            )
            .into());
        }
        if *item.local() != adapter.role
            || !matches!(
                item.observation(),
                ObservationRef::Read(TableObservation::Directory(_))
                    | ObservationRef::UpgradedPredicate(TableObservation::Directory(_))
            )
            || item.intent().is_some()
        {
            return Err(adapter.type_mismatch(AdapterPhase::Preflight).into());
        }
        return Ok(None);
    }

    let (AdapterRole::Record, TableKey::Record(record_id)) = (adapter.role, key) else {
        return Err(adapter.type_mismatch(AdapterPhase::Preflight).into());
    };
    if adapter.table.record_token_mode != RecordTokenMode::DirectRecordPointer {
        return Err(AdapterFault::new(
            AdapterPhase::Preflight,
            AdapterFaultKind::LockIdentityMismatch,
        )
        .into());
    }
    if *item.local() != AdapterRole::Record {
        return Err(adapter.type_mismatch(AdapterPhase::Preflight).into());
    }
    let observation = match item.observation() {
        ObservationRef::Read(TableObservation::Record(observation))
        | ObservationRef::UpgradedPredicate(TableObservation::Record(observation)) => observation,
        _ => return Err(adapter.type_mismatch(AdapterPhase::Preflight).into()),
    };
    match item.intent() {
        None => Ok(None),
        Some(TableIntent::Record { .. }) => {
            let access = adapter
                .table
                .resolve_directory_access(*record_id)
                .map_err(|_| adapter.type_mismatch(AdapterPhase::Preflight))?;
            let token = DirectRecordLockToken::from_private_access(access, observation.version);
            if token.record_id() != *record_id || token.observed() != observation.version {
                return Err(adapter.type_mismatch(AdapterPhase::Preflight).into());
            }
            Ok(Some(BorrowedLockToken::new(
                adapter.table.runtime_id,
                token,
            )))
        }
    }
}

fn validate_direct_item(
    adapter: &TableAdapter,
    key: &TableKey,
    item: DirectValidationItem<'_, TableAdapter>,
    lock: Option<DirectLockRef<'_, TableShared>>,
    cx: &DirectValidationContext,
) -> Result<(), CheckError> {
    if adapter.role.is_generation() && matches!(key, TableKey::DirectoryGeneration) {
        if adapter.table.record_token_mode != RecordTokenMode::DirectRecordPointer
            || *item.local() != adapter.role
            || item.intent().is_some()
            || lock.is_some()
        {
            return Err(adapter.type_mismatch(AdapterPhase::Validation).into());
        }
        return match item.observation() {
            ObservationRef::Read(TableObservation::Directory(observation))
            | ObservationRef::UpgradedPredicate(TableObservation::Directory(observation)) => {
                adapter.validate_directory_observation(observation)
            }
            _ => Err(adapter.type_mismatch(AdapterPhase::Validation).into()),
        };
    }

    let (AdapterRole::Record, TableKey::Record(record_id)) = (adapter.role, key) else {
        return Err(adapter.type_mismatch(AdapterPhase::Validation).into());
    };
    if adapter.table.record_token_mode != RecordTokenMode::DirectRecordPointer
        || *item.local() != AdapterRole::Record
        || !matches!(item.intent(), None | Some(TableIntent::Record { .. }))
    {
        return Err(adapter.type_mismatch(AdapterPhase::Validation).into());
    }
    let observation = match item.observation() {
        ObservationRef::Read(TableObservation::Record(observation))
        | ObservationRef::UpgradedPredicate(TableObservation::Record(observation)) => observation,
        _ => return Err(adapter.type_mismatch(AdapterPhase::Validation).into()),
    };
    match (item.intent().is_some(), lock) {
        (false, None) => adapter.validate_read_only_observation(*record_id, observation),
        (true, Some(lock)) => {
            if !std::ptr::eq(lock.target(), adapter.table.as_ref()) {
                return Err(AdapterFault::new(
                    AdapterPhase::Validation,
                    AdapterFaultKind::LockIdentityMismatch,
                )
                .into());
            }
            let guard = lock.guard();
            let record = guard.record(adapter.table.as_ref(), AdapterPhase::Validation)?;
            if !guard.is_held()
                || !guard.is_for(*record_id, &record.version)
                || guard.owner() != cx.owner()
            {
                return Err(AdapterFault::new(
                    AdapterPhase::Validation,
                    AdapterFaultKind::LockIdentityMismatch,
                )
                .into());
            }
            if guard.before() == observation.version {
                Ok(())
            } else {
                Err(Conflict::ReadValidation.into())
            }
        }
        _ => Err(AdapterFault::new(
            AdapterPhase::Validation,
            AdapterFaultKind::LockIdentityMismatch,
        )
        .into()),
    }
}

fn install_direct_record(
    adapter: &TableAdapter,
    key: &TableKey,
    mut item: InstallItem<'_, TableAdapter>,
    mut lock: DirectLockMut<'_, TableShared>,
    cx: &mut DirectInstallContext,
) {
    let (AdapterRole::Record, TableKey::Record(record_id)) = (adapter.role, key) else {
        adapter.panic_type_mismatch("direct install");
    };
    if adapter.table.record_token_mode != RecordTokenMode::DirectRecordPointer
        || *item.local_mut() != AdapterRole::Record
        || !matches!(item.intent(), TableIntent::Record { .. })
        || !std::ptr::eq(lock.target(), adapter.table.as_ref())
    {
        adapter.panic_type_mismatch("direct install item");
    }
    let commit_id = cx
        .occ_commit_id()
        .expect("sto-masstree direct record write has no OCC commit ID");
    let owner = cx.owner();
    let guard = lock.guard_mut();
    let publication_access = guard
        .access(adapter.table.as_ref(), AdapterPhase::Install)
        .unwrap_or_else(|error| panic!("sto-masstree direct record install invariant: {error}"));
    let record = publication_access.record;
    if !guard.is_held()
        || !guard.is_for(*record_id, &record.version)
        || guard.owner() != owner
        || commit_id.to_version() <= guard.before()
    {
        panic!("sto-masstree direct install received the wrong version guard");
    }
    let old_was_shared = match item.observation() {
        ObservationRef::Read(TableObservation::Record(observation))
        | ObservationRef::UpgradedPredicate(TableObservation::Record(observation)) => {
            observation.old_was_shared
        }
        _ => adapter.panic_type_mismatch("direct install observation"),
    };
    let replacement = item.intent().record_state();
    record
        .state
        .begin_publication(adapter.table.as_ref(), publication_access.stable)
        .publish(replacement, old_was_shared, owner, commit_id);
}

impl TableAdapter {
    fn type_mismatch(&self, phase: AdapterPhase) -> AdapterFault {
        self.table.poison();
        AdapterFault::new(phase, AdapterFaultKind::TypeMismatch)
    }

    #[inline(always)]
    fn require_physical_lock<L: TransactionLock>(
        &self,
        cx: &mut PreflightContext<'_>,
        request: LockRequest<L>,
    ) -> Result<ErasedLockUse, PrepareError> {
        let lock_use = if self.table.registry.config.unique_lock_requests {
            cx.require_unique_lock(request)?
        } else {
            cx.require_lock(request)?
        };
        Ok(lock_use.erase())
    }

    #[inline(always)]
    fn ensure_role(&self, role: AdapterRole) -> Result<(), AccessError> {
        if self.role == role {
            Ok(())
        } else {
            Err(self.type_mismatch(AdapterPhase::Execute).into())
        }
    }

    fn validate_read_only_observation(
        &self,
        record_id: RecordId,
        observation: &RecordObservation,
    ) -> Result<(), CheckError> {
        let record = self
            .table
            .resolve_for_phase(record_id, AdapterPhase::Validation)?;
        if record.version.validate(observation.version) {
            Ok(())
        } else {
            Err(Conflict::ReadValidation.into())
        }
    }

    #[inline(always)]
    fn validate_directory_observation(
        &self,
        observation: &DirectoryObservation,
    ) -> Result<(), CheckError> {
        let current = match self.role {
            AdapterRole::Directory => self.table.directory_generation.load(Ordering::Acquire),
            AdapterRole::Scan => {
                if self.table.health() != TableHealth::Healthy {
                    return Err(AdapterFault::new(
                        AdapterPhase::Validation,
                        AdapterFaultKind::Other(
                            "Masstree table became unhealthy after a trusted scan",
                        ),
                    )
                    .into());
                }
                self.table.scan_generation.load(Ordering::Acquire)
            }
            AdapterRole::Record => {
                return Err(self.type_mismatch(AdapterPhase::Validation).into());
            }
        };
        if current != observation.generation {
            return Err(Conflict::ReadValidation.into());
        }
        Ok(())
    }

    #[inline(always)]
    fn prepare_access(
        &self,
        record_id: RecordId,
        entry: &mut Entry<'_, Self>,
    ) -> Result<Option<RecordState>, AccessError> {
        self.prepare_access_inner(record_id, None, entry)
    }

    #[inline(always)]
    fn prepare_resolved_access(
        &self,
        access: RecordAccess<'_>,
        entry: &mut Entry<'_, Self>,
    ) -> Result<Option<RecordState>, AccessError> {
        self.prepare_access_inner(access.record_id, Some(access), entry)
    }

    #[inline(always)]
    fn visit_access_bytes<R>(
        &self,
        record_id: RecordId,
        entry: &mut Entry<'_, Self>,
        visit: impl for<'value> FnOnce(Option<&'value [u8]>) -> R,
    ) -> Result<R, AccessError> {
        self.visit_access_bytes_inner(record_id, None, entry, visit)
    }

    #[inline(always)]
    fn visit_resolved_access_bytes<R>(
        &self,
        access: RecordAccess<'_>,
        entry: &mut Entry<'_, Self>,
        visit: impl for<'value> FnOnce(Option<&'value [u8]>) -> R,
    ) -> Result<R, AccessError> {
        self.visit_access_bytes_inner(access.record_id, Some(access), entry, visit)
    }

    /// Visits one committed snapshot without creating record-local STO state.
    /// The trusted scan's enclosing value-generation observation certifies the
    /// whole result again at commit; this per-record sandwich prevents torn or
    /// lock-covered bytes from reaching the callback during execution.
    #[inline(always)]
    fn visit_untracked_resolved_access_bytes<R>(
        &self,
        access: RecordAccess<'_>,
        visit: impl for<'value> FnOnce(Option<&'value [u8]>) -> R,
    ) -> Result<R, AccessError> {
        self.ensure_role(AdapterRole::Record)?;
        self.table.ensure_healthy()?;
        let observed = access
            .record
            .version
            .observe()
            .map_err(|_| AccessError::from(Conflict::LockBusy))?;
        let lease = self.snapshot_state_lease(access, observed)?;
        Ok(visit(lease.value()))
    }

    /// Copies staged bytes or one stable committed snapshot directly into
    /// caller-owned storage.
    #[inline(always)]
    fn copy_resolved_access(
        &self,
        access: RecordAccess<'_>,
        entry: &mut Entry<'_, Self>,
        output: &mut [u8],
    ) -> Result<ValueCopyOutcome, AccessError> {
        self.copy_access_inner(access.record_id, Some(access), entry, output)
    }

    #[inline(always)]
    fn copy_access_inner(
        &self,
        record_id: RecordId,
        access: Option<RecordAccess<'_>>,
        entry: &mut Entry<'_, Self>,
        output: &mut [u8],
    ) -> Result<ValueCopyOutcome, AccessError> {
        self.ensure_role(AdapterRole::Record)?;
        self.table.ensure_healthy()?;
        if *entry.local() != AdapterRole::Record {
            return Err(self.type_mismatch(AdapterPhase::Execute).into());
        }
        let prior_version = match entry.observation() {
            ObservationRef::Read(TableObservation::Record(observation))
            | ObservationRef::UpgradedPredicate(TableObservation::Record(observation)) => {
                Some(observation.version)
            }
            ObservationRef::Unobserved => None,
            ObservationRef::Read(TableObservation::Directory(_))
            | ObservationRef::UpgradedPredicate(TableObservation::Directory(_))
            | ObservationRef::Predicate(_) => {
                return Err(self.type_mismatch(AdapterPhase::Execute).into());
            }
        };

        match entry.intent() {
            Some(intent) if prior_version.is_some() => {
                return Ok(copy_value_into(
                    intent.record_state().value().map(Value::as_ref),
                    output,
                ));
            }
            Some(TableIntent::Record { .. }) => {
                self.table.poison();
                return Err(table_fault("staged record has no read observation"));
            }
            None => {}
        }

        let access = match access {
            Some(access) => access,
            None => self.table.resolve_directory_access(record_id)?,
        };
        let record = access.record;
        let observed = match prior_version {
            Some(version) => version,
            None => record
                .version
                .observe()
                .map_err(|_| AccessError::from(Conflict::LockBusy))?,
        };
        let tail_and_descriptor = record.state.tail_and_descriptor.load(Ordering::Acquire);
        let descriptor = record_state_descriptor(tail_and_descriptor);
        let inline_length = descriptor.wrapping_sub(RECORD_STATE_INLINE_BASE);

        if usize::from(inline_length) <= INLINE_VALUE_CAPACITY {
            if prior_version.is_none() {
                entry.record_read(TableObservation::Record(RecordObservation {
                    version: observed,
                    original_live: true,
                    old_was_shared: false,
                }))?;
            }
            return record.state.copy_inline_after_observation(
                &record.version,
                observed,
                tail_and_descriptor,
                usize::from(inline_length),
                output,
            );
        }

        if let Some(length) = stable_record_state_length(descriptor) {
            let Some(stable) = access.stable else {
                return self.reject_incomplete_copy_state(
                    record,
                    observed,
                    "stable record descriptor has no bounded atomic cell",
                );
            };
            if prior_version.is_none() {
                entry.record_read(TableObservation::Record(RecordObservation {
                    version: observed,
                    original_live: true,
                    old_was_shared: false,
                }))?;
            }
            return record.state.copy_stable_after_observation(
                stable,
                &record.version,
                observed,
                tail_and_descriptor,
                length,
                output,
            );
        }

        match descriptor {
            RECORD_STATE_TOMBSTONE => {
                if prior_version.is_none() {
                    entry.record_read(TableObservation::Record(RecordObservation {
                        version: observed,
                        original_live: false,
                        old_was_shared: false,
                    }))?;
                }
                if !record.version.validate(observed) {
                    return Err(Conflict::ReadValidation.into());
                }
                Ok(ValueCopyOutcome::Miss)
            }
            RECORD_STATE_SHARED => {
                if prior_version.is_none() {
                    entry.record_read(TableObservation::Record(RecordObservation {
                        version: observed,
                        original_live: true,
                        old_was_shared: true,
                    }))?;
                }
                let lease = record.state.shared.load();
                if !record.version.validate(observed) {
                    return Err(Conflict::ReadValidation.into());
                }
                let Some(value) = lease.as_ref() else {
                    self.table.poison();
                    return Err(table_fault(
                        "committed large record publication is incomplete",
                    ));
                };
                Ok(copy_value_into(Some(value.as_bytes()), output))
            }
            RECORD_STATE_UPDATING => self.reject_incomplete_copy_state(
                record,
                observed,
                "committed record publication remained in progress",
            ),
            RECORD_STATE_POISONED => self.reject_incomplete_copy_state(
                record,
                observed,
                "committed record state is poisoned",
            ),
            _ => self.reject_incomplete_copy_state(
                record,
                observed,
                "committed record state descriptor is invalid",
            ),
        }
    }

    #[inline(always)]
    fn reject_incomplete_copy_state(
        &self,
        record: &Record,
        observed: OccVersion,
        reason: &'static str,
    ) -> Result<ValueCopyOutcome, AccessError> {
        if !record.version.validate(observed) {
            return Err(Conflict::ReadValidation.into());
        }
        self.table.poison();
        Err(table_fault(reason))
    }

    /// Visits staged bytes or an operation-scoped committed-state lease.
    ///
    /// The higher-ranked callback is intentional: neither inline stack bytes
    /// nor a reference protected by the ArcSwap guard may escape this call.
    #[inline(always)]
    fn visit_access_bytes_inner<R>(
        &self,
        record_id: RecordId,
        access: Option<RecordAccess<'_>>,
        entry: &mut Entry<'_, Self>,
        visit: impl for<'value> FnOnce(Option<&'value [u8]>) -> R,
    ) -> Result<R, AccessError> {
        self.ensure_role(AdapterRole::Record)?;
        self.table.ensure_healthy()?;
        if *entry.local() != AdapterRole::Record {
            return Err(self.type_mismatch(AdapterPhase::Execute).into());
        }
        let prior_version = match entry.observation() {
            ObservationRef::Read(TableObservation::Record(observation))
            | ObservationRef::UpgradedPredicate(TableObservation::Record(observation)) => {
                Some(observation.version)
            }
            ObservationRef::Unobserved => None,
            ObservationRef::Read(TableObservation::Directory(_))
            | ObservationRef::UpgradedPredicate(TableObservation::Directory(_))
            | ObservationRef::Predicate(_) => {
                return Err(self.type_mismatch(AdapterPhase::Execute).into());
            }
        };

        match entry.intent() {
            Some(intent) if prior_version.is_some() => {
                return Ok(visit(intent.record_state().value().map(Value::as_ref)));
            }
            Some(TableIntent::Record { .. }) => {
                self.table.poison();
                return Err(table_fault("staged record has no read observation"));
            }
            None => {}
        }

        let access = match access {
            Some(access) => access,
            None => self.table.resolve_directory_access(record_id)?,
        };
        let record = access.record;
        let observed = match prior_version {
            Some(version) => version,
            None => record
                .version
                .observe()
                .map_err(|_| AccessError::from(Conflict::LockBusy))?,
        };
        let lease = self.snapshot_state_lease(access, observed)?;
        if prior_version.is_none() {
            entry.record_read(TableObservation::Record(RecordObservation {
                version: observed,
                original_live: lease.is_live(),
                old_was_shared: lease.uses_shared_storage(),
            }))?;
        }
        Ok(visit(lease.value()))
    }

    #[inline(always)]
    fn prepare_access_inner(
        &self,
        record_id: RecordId,
        access: Option<RecordAccess<'_>>,
        entry: &mut Entry<'_, Self>,
    ) -> Result<Option<RecordState>, AccessError> {
        self.ensure_role(AdapterRole::Record)?;
        self.table.ensure_healthy()?;
        if *entry.local() != AdapterRole::Record {
            return Err(self.type_mismatch(AdapterPhase::Execute).into());
        }
        let prior_version = match entry.observation() {
            ObservationRef::Read(TableObservation::Record(observation))
            | ObservationRef::UpgradedPredicate(TableObservation::Record(observation)) => {
                Some(observation.version)
            }
            ObservationRef::Unobserved => None,
            ObservationRef::Read(TableObservation::Directory(_))
            | ObservationRef::UpgradedPredicate(TableObservation::Directory(_))
            | ObservationRef::Predicate(_) => {
                return Err(self.type_mismatch(AdapterPhase::Execute).into());
            }
        };

        match entry.intent() {
            Some(TableIntent::Record { .. }) if prior_version.is_some() => return Ok(None),
            Some(TableIntent::Record { .. }) => {
                self.table.poison();
                return Err(table_fault("staged record has no read observation"));
            }
            None => {}
        }

        let access = match access {
            Some(access) => access,
            None => self.table.resolve_directory_access(record_id)?,
        };
        let record = access.record;
        let observed = match prior_version {
            Some(version) => version,
            None => record
                .version
                .observe()
                .map_err(|_| AccessError::from(Conflict::LockBusy))?,
        };
        let (snapshot, old_was_shared) = self.snapshot_state(access, observed)?;
        if prior_version.is_none() {
            entry.record_read(TableObservation::Record(RecordObservation {
                version: observed,
                original_live: snapshot.is_live(),
                old_was_shared,
            }))?;
        }
        Ok(Some(snapshot))
    }

    /// Prepares an access that needs only logical presence and storage class.
    ///
    /// The first access sandwiches only the committed descriptor. Later unstaged
    /// accesses reuse the first classification, while a staged intent supplies
    /// read-your-writes liveness without touching committed storage.
    #[inline(always)]
    fn prepare_resolved_presence_access(
        &self,
        access: RecordAccess<'_>,
        entry: &mut Entry<'_, Self>,
    ) -> Result<bool, AccessError> {
        self.prepare_presence_access_inner(access.record_id, Some(access), entry)
    }

    #[inline(always)]
    fn prepare_presence_access_inner(
        &self,
        record_id: RecordId,
        access: Option<RecordAccess<'_>>,
        entry: &mut Entry<'_, Self>,
    ) -> Result<bool, AccessError> {
        self.ensure_role(AdapterRole::Record)?;
        self.table.ensure_healthy()?;
        if *entry.local() != AdapterRole::Record {
            return Err(self.type_mismatch(AdapterPhase::Execute).into());
        }
        let prior_live = match entry.observation() {
            ObservationRef::Read(TableObservation::Record(observation))
            | ObservationRef::UpgradedPredicate(TableObservation::Record(observation)) => {
                Some(observation.original_live)
            }
            ObservationRef::Unobserved => None,
            ObservationRef::Read(TableObservation::Directory(_))
            | ObservationRef::UpgradedPredicate(TableObservation::Directory(_))
            | ObservationRef::Predicate(_) => {
                return Err(self.type_mismatch(AdapterPhase::Execute).into());
            }
        };

        match entry.intent() {
            Some(intent) if prior_live.is_some() => return Ok(intent.record_state().is_live()),
            Some(TableIntent::Record { .. }) => {
                self.table.poison();
                return Err(table_fault("staged record has no read observation"));
            }
            None => {}
        }
        if let Some(original_live) = prior_live {
            return Ok(original_live);
        }

        let access = match access {
            Some(access) => access,
            None => self.table.resolve_directory_access(record_id)?,
        };
        let record = access.record;
        let observed = record
            .version
            .observe()
            .map_err(|_| AccessError::from(Conflict::LockBusy))?;
        let metadata = self.snapshot_metadata(access, observed)?;
        entry.record_read(TableObservation::Record(RecordObservation {
            version: observed,
            original_live: metadata.live,
            old_was_shared: metadata.shared,
        }))?;
        Ok(metadata.live)
    }

    #[inline(always)]
    fn prepare_terminal_read_access(
        &self,
        access: RecordAccess<'_>,
    ) -> Result<(OccVersion, RecordState, bool), AccessError> {
        self.ensure_role(AdapterRole::Record)?;
        self.table.ensure_healthy()?;
        let record = access.record;
        let observed = record
            .version
            .observe()
            .map_err(|_| AccessError::from(Conflict::LockBusy))?;
        let (snapshot, old_was_shared) = self.snapshot_state(access, observed)?;
        Ok((observed, snapshot, old_was_shared))
    }

    #[inline(always)]
    fn snapshot_state(
        &self,
        access: RecordAccess<'_>,
        observed: OccVersion,
    ) -> Result<(RecordState, bool), AccessError> {
        let record = access.record;
        let candidate = record.state.load(access.stable);
        // Classify intermediate field combinations only after closing the OCC
        // sandwich. They are expected when a writer acquired the version
        // after our first observation; they are corruption only if the exact
        // unlocked generation remained stable.
        if !record.version.validate(observed) {
            return Err(Conflict::ReadValidation.into());
        }
        match candidate {
            CommittedStateLoad::Complete { state, shared } => Ok((state, shared)),
            CommittedStateLoad::Incomplete(reason) => {
                self.table.poison();
                Err(table_fault(reason))
            }
        }
    }

    #[inline(always)]
    fn snapshot_state_lease(
        &self,
        access: RecordAccess<'_>,
        observed: OccVersion,
    ) -> Result<CommittedStateLease, AccessError> {
        // Keep the stable-read protocol explicit: observe the version before
        // entering here, load descriptor/payload, close the OCC sandwich, and
        // only then classify an incomplete state as stable corruption.
        let record = access.record;
        let candidate = record.state.load_lease(access.stable);
        if !record.version.validate(observed) {
            return Err(Conflict::ReadValidation.into());
        }
        match candidate {
            CommittedStateLeaseLoad::Complete(lease) => Ok(lease),
            CommittedStateLeaseLoad::Incomplete(reason) => {
                self.table.poison();
                Err(table_fault(reason))
            }
        }
    }

    #[inline(always)]
    fn snapshot_metadata(
        &self,
        access: RecordAccess<'_>,
        observed: OccVersion,
    ) -> Result<RecordStateMetadata, AccessError> {
        let record = access.record;
        let candidate = record.state.load_metadata(access.stable);
        // As with a full load, classify an intermediate descriptor only after the OCC
        // sandwich distinguishes a racing publication from stable corruption.
        if !record.version.validate(observed) {
            return Err(Conflict::ReadValidation.into());
        }
        match candidate {
            CommittedStateMetadataLoad::Complete(metadata) => Ok(metadata),
            CommittedStateMetadataLoad::Incomplete(reason) => {
                self.table.poison();
                Err(table_fault(reason))
            }
        }
    }

    fn validate_observation(
        &self,
        record_id: RecordId,
        observation: &RecordObservation,
        prepared: &TablePrepared,
        cx: &ValidationContext<'_>,
    ) -> Result<(), CheckError> {
        let Some(lock_use) = prepared.lock_use.as_ref() else {
            return self.validate_read_only_observation(record_id, observation);
        };
        let acquired_version = match self.table.record_token_mode {
            RecordTokenMode::RegistryId => {
                let guard = cx.guard_erased::<RecordLockSegment>(lock_use)?;
                if !guard.is_held() || guard.record_id != record_id || guard.owner() != cx.owner() {
                    return Err(AdapterFault::new(
                        AdapterPhase::Validation,
                        AdapterFaultKind::LockIdentityMismatch,
                    )
                    .into());
                }
                guard.before()
            }
            RecordTokenMode::DirectRecordPointer => {
                let guard = cx.guard_erased::<TableShared>(lock_use)?;
                let record = self
                    .table
                    .resolve_for_phase(record_id, AdapterPhase::Validation)?;
                if !guard.is_held()
                    || !guard.is_for(record_id, &record.version)
                    || guard.owner() != cx.owner()
                {
                    return Err(AdapterFault::new(
                        AdapterPhase::Validation,
                        AdapterFaultKind::LockIdentityMismatch,
                    )
                    .into());
                }
                guard.before()
            }
        };

        // Successful acquisition recorded the exact unlocked version that
        // its compare-exchange replaced with this owner's lock word. The
        // private guard can only be constructed by the mode's retained lock
        // target for the identity above, and the held lock excludes writers.
        // Equality with that acquisition snapshot is therefore the final
        // write-read certification; resolving the registry and loading the
        // same AtomicVersion again would add no information.
        if acquired_version == observation.version {
            Ok(())
        } else {
            Err(Conflict::ReadValidation.into())
        }
    }
}

impl DirectBorrowedLockTarget<TableShared> for TableAdapter {
    #[inline(always)]
    fn direct_borrowed_lock_target(&self) -> &TableShared {
        self.table.as_ref()
    }
}

impl TransactionalResource for TableAdapter {
    type Key = TableKey;
    type Local = AdapterRole;
    type Observation = TableObservation;
    type Predicate = NoPredicate;
    type Intent = TableIntent;
    type Prepared = TablePrepared;

    #[inline(always)]
    fn new_local(&self, key: &Self::Key) -> Result<Self::Local, sto_core::ItemInitError> {
        match (self.role, key) {
            (AdapterRole::Record, TableKey::Record(_)) => Ok(AdapterRole::Record),
            (AdapterRole::Directory, TableKey::DirectoryGeneration) => Ok(AdapterRole::Directory),
            (AdapterRole::Scan, TableKey::DirectoryGeneration) => Ok(AdapterRole::Scan),
            _ => Err(self.type_mismatch(AdapterPhase::ItemInit).into()),
        }
    }

    fn preflight_free_read_capability(&self) -> Option<&'static PreflightFreeReadCapability<Self>> {
        match self.role {
            AdapterRole::Record => Some(&RECORD_PREFLIGHT_FREE_READ),
            AdapterRole::Directory | AdapterRole::Scan => Some(&DIRECTORY_PREFLIGHT_FREE_READ),
        }
    }

    fn terminal_read_batch_capability(&self) -> Option<&'static TerminalReadBatchCapability<Self>> {
        (self.role == AdapterRole::Record).then_some(&RECORD_TERMINAL_READ_BATCH)
    }

    fn direct_commit_capability(&self) -> Option<&'static DirectCommitCapability<Self>> {
        (self.table.record_token_mode == RecordTokenMode::DirectRecordPointer)
            .then_some(&RECORD_DIRECT_COMMIT)
    }

    fn preflight(
        &self,
        key: &Self::Key,
        item: PreflightItem<'_, Self>,
        cx: &mut PreflightContext<'_>,
    ) -> Result<Self::Prepared, PrepareError> {
        match (self.role, key) {
            (AdapterRole::Record, TableKey::Record(record_id)) => {
                self.preflight_record(*record_id, item, cx)
            }
            (AdapterRole::Directory, TableKey::DirectoryGeneration) => {
                self.preflight_directory(item)
            }
            (AdapterRole::Scan, TableKey::DirectoryGeneration) => self.preflight_directory(item),
            _ => Err(self.type_mismatch(AdapterPhase::Preflight).into()),
        }
    }

    fn revalidate_read(
        &self,
        key: &Self::Key,
        observation: &Self::Observation,
        _cx: &ExecutionCheckContext<'_>,
    ) -> Result<(), CheckError> {
        match (self.role, key, observation) {
            (
                AdapterRole::Record,
                TableKey::Record(record_id),
                TableObservation::Record(observation),
            ) => {
                let record = self
                    .table
                    .resolve_for_phase(*record_id, AdapterPhase::ExecutionCheck)?;
                if record.version.validate(observation.version) {
                    Ok(())
                } else {
                    Err(Conflict::ReadValidation.into())
                }
            }
            (role, TableKey::DirectoryGeneration, TableObservation::Directory(observation))
                if role.is_generation() =>
            {
                self.validate_directory_observation(observation)
            }
            _ => Err(self.type_mismatch(AdapterPhase::ExecutionCheck).into()),
        }
    }

    fn revalidate_predicate(
        &self,
        _key: &Self::Key,
        predicate: &Self::Predicate,
        _cx: &ExecutionCheckContext<'_>,
    ) -> Result<ObservationOrder, CheckError> {
        match *predicate {}
    }

    fn upgrade_predicate(
        &self,
        _key: &Self::Key,
        predicate: &Self::Predicate,
        _prepared: &Self::Prepared,
        _cx: &PredicateContext<'_>,
    ) -> Result<Self::Observation, CheckError> {
        match *predicate {}
    }

    fn validate_read(
        &self,
        key: &Self::Key,
        observation: &Self::Observation,
        prepared: &Self::Prepared,
        cx: &ValidationContext<'_>,
    ) -> Result<(), CheckError> {
        match (self.role, key, observation) {
            (
                AdapterRole::Record,
                TableKey::Record(record_id),
                TableObservation::Record(observation),
            ) => self.validate_observation(*record_id, observation, prepared, cx),
            (role, TableKey::DirectoryGeneration, TableObservation::Directory(observation))
                if role.is_generation() =>
            {
                if prepared.lock_use.is_some() {
                    return Err(AdapterFault::new(
                        AdapterPhase::Validation,
                        AdapterFaultKind::LockIdentityMismatch,
                    )
                    .into());
                }
                self.validate_directory_observation(observation)
            }
            _ => Err(self.type_mismatch(AdapterPhase::Validation).into()),
        }
    }

    fn install(
        &self,
        key: &Self::Key,
        item: InstallItem<'_, Self>,
        prepared: &mut Self::Prepared,
        cx: &mut InstallContext<'_>,
    ) {
        match (self.role, key) {
            (AdapterRole::Record, TableKey::Record(record_id)) => {
                self.install_record(*record_id, item, prepared, cx)
            }
            (role, TableKey::DirectoryGeneration) if role.is_generation() => {
                self.panic_type_mismatch("directory install")
            }
            _ => self.panic_type_mismatch("install"),
        }
    }

    fn finish(
        &self,
        key: &Self::Key,
        mut item: FinishItem<'_, Self>,
        _prepared: Option<&mut Self::Prepared>,
        _disposition: FinishDisposition,
        _cx: &mut FinishContext<'_>,
    ) {
        let local_matches = match (self.role, key) {
            (AdapterRole::Record, TableKey::Record(_)) => *item.local_mut() == AdapterRole::Record,
            (AdapterRole::Directory, TableKey::DirectoryGeneration) => {
                *item.local_mut() == AdapterRole::Directory
            }
            (AdapterRole::Scan, TableKey::DirectoryGeneration) => {
                *item.local_mut() == AdapterRole::Scan
            }
            _ => false,
        };
        let intent_matches = item.take_remaining_intent().is_none_or(|intent| {
            matches!(
                (self.role, intent),
                (AdapterRole::Record, TableIntent::Record { .. })
            )
        });
        if !(local_matches && intent_matches) {
            self.table.poison();
        }
    }
}

impl TableAdapter {
    fn preflight_record(
        &self,
        record_id: RecordId,
        item: PreflightItem<'_, Self>,
        cx: &mut PreflightContext<'_>,
    ) -> Result<TablePrepared, PrepareError> {
        match item.observation() {
            ObservationRef::Read(TableObservation::Record(_))
            | ObservationRef::UpgradedPredicate(TableObservation::Record(_)) => {}
            ObservationRef::Unobserved | ObservationRef::Predicate(_) => {
                return Err(AdapterFault::new(
                    AdapterPhase::Preflight,
                    AdapterFaultKind::LockIdentityMismatch,
                )
                .into());
            }
            ObservationRef::Read(TableObservation::Directory(_))
            | ObservationRef::UpgradedPredicate(TableObservation::Directory(_)) => {
                return Err(self.type_mismatch(AdapterPhase::Preflight).into());
            }
        };
        if *item.local() != AdapterRole::Record {
            return Err(self.type_mismatch(AdapterPhase::Preflight).into());
        }
        // Match C++ MassTrans: recording a write makes this a writer even if
        // its bytes equal the observed value or later return to those bytes.
        let writes_record = match item.intent() {
            Some(TableIntent::Record { .. }) => true,
            None => false,
        };
        if writes_record {
            let lock_identity = LockIdentity::new(
                self.table.runtime_id,
                self.table.namespace,
                self.table.record_lock_class,
                record_id.get(),
            );
            let lock_use = match self.table.record_token_mode {
                RecordTokenMode::RegistryId => {
                    let (_record, lock_segment) = self
                        .table
                        .resolve_with_lock_segment_for_phase(record_id, AdapterPhase::Preflight)?;
                    self.require_physical_lock(
                        cx,
                        LockRequest::new(lock_identity, Arc::clone(lock_segment)),
                    )?
                }
                RecordTokenMode::DirectRecordPointer => {
                    self.table
                        .resolve_for_phase(record_id, AdapterPhase::Preflight)?;
                    self.require_physical_lock(
                        cx,
                        LockRequest::new(lock_identity, Arc::clone(&self.table)),
                    )?
                }
            };
            Ok(TablePrepared {
                lock_use: Some(lock_use),
            })
        } else {
            Ok(TablePrepared { lock_use: None })
        }
    }

    fn preflight_directory(
        &self,
        item: PreflightItem<'_, Self>,
    ) -> Result<TablePrepared, PrepareError> {
        if !self.role.is_generation()
            || *item.local() != self.role
            || !matches!(
                item.observation(),
                ObservationRef::Read(TableObservation::Directory(_))
                    | ObservationRef::UpgradedPredicate(TableObservation::Directory(_))
            )
            || item.intent().is_some()
        {
            return Err(self.type_mismatch(AdapterPhase::Preflight).into());
        }
        Ok(TablePrepared { lock_use: None })
    }

    fn install_record(
        &self,
        record_id: RecordId,
        mut item: InstallItem<'_, Self>,
        prepared: &mut TablePrepared,
        cx: &mut InstallContext<'_>,
    ) {
        if *item.local_mut() != AdapterRole::Record
            || !matches!(item.intent(), TableIntent::Record { .. })
        {
            self.panic_type_mismatch("record install item");
        }
        let lock_use = prepared
            .lock_use
            .as_ref()
            .expect("sto-masstree record write has no planned lock");
        let commit_id = cx
            .occ_commit_id()
            .expect("sto-masstree record write has no OCC commit ID");
        let owner = cx.owner();
        let publication_access = match self.table.record_token_mode {
            RecordTokenMode::RegistryId => {
                let (lock_segment, guard) = cx
                    .target_and_guard_mut_erased::<RecordLockSegment>(lock_use)
                    .unwrap_or_else(|error| panic!("sto-masstree record lock invariant: {error}"));
                let slot_access = lock_segment
                    .access_at(guard.slot, AdapterPhase::Install)
                    .unwrap_or_else(|error| {
                        panic!("sto-masstree record install invariant: {error}")
                    });
                let record = slot_access.record();
                if !guard.is_held()
                    || !guard.is_for(record_id, &record.version)
                    || guard.owner() != owner
                    || commit_id.to_version() <= guard.before()
                {
                    panic!("sto-masstree record install received the wrong version guard");
                }
                RecordAccess {
                    record_id,
                    record,
                    stable: slot_access.stable,
                }
            }
            RecordTokenMode::DirectRecordPointer => {
                let (table, guard) = cx
                    .target_and_guard_mut_erased::<TableShared>(lock_use)
                    .unwrap_or_else(|error| {
                        panic!("sto-masstree direct record lock invariant: {error}")
                    });
                let access = guard
                    .access(table, AdapterPhase::Install)
                    .unwrap_or_else(|error| {
                        panic!("sto-masstree direct record install invariant: {error}")
                    });
                let record = access.record;
                if !guard.is_held()
                    || !guard.is_for(record_id, &record.version)
                    || guard.owner() != owner
                    || commit_id.to_version() <= guard.before()
                {
                    panic!("sto-masstree direct install received the wrong version guard");
                }
                access
            }
        };
        let record = publication_access.record;
        let old_was_shared = match item.observation() {
            ObservationRef::Read(TableObservation::Record(observation))
            | ObservationRef::UpgradedPredicate(TableObservation::Record(observation)) => {
                observation.old_was_shared
            }
            _ => self.panic_type_mismatch("record install observation"),
        };

        // Publication borrows the staged intent. Inline values copy directly
        // into the atomic lane; large immutable values increment one Arc when
        // installing into the record's embedded slot. ArcSwap reclamation
        // keeps any operation-local old snapshot alive across replacement;
        // the observation needs only the old storage-class bit for cleanup.
        let replacement = item.intent().record_state();
        record
            .state
            .begin_publication(self.table.as_ref(), publication_access.stable)
            .publish(replacement, old_was_shared, owner, commit_id);
    }

    #[cold]
    #[inline(never)]
    fn panic_type_mismatch(&self, operation: &str) -> ! {
        self.table.poison();
        panic!("sto-masstree {operation} received a cross-role transaction item")
    }
}

#[inline(always)]
fn visit_fixed_value<const CAPTURE_VALUE: bool>(
    adapter: &TableAdapter,
    entry: &mut Entry<'_, TableAdapter>,
    access: RecordAccess<'_>,
    index: usize,
    visit: &mut impl for<'value> FnMut(usize, Option<&'value Value>),
    values: &mut Vec<Option<Value>>,
) -> Result<(), AccessError> {
    let loaded = adapter.prepare_resolved_access(access, entry)?;
    let current = current_state(entry, loaded.as_ref())?.value();
    visit(index, current);
    if CAPTURE_VALUE {
        values.push(current.cloned());
    }
    Ok(())
}

#[inline(always)]
fn visit_fixed_bytes_value(
    adapter: &TableAdapter,
    entry: &mut Entry<'_, TableAdapter>,
    access: RecordAccess<'_>,
    index: usize,
    resolved: ResolvedRecord,
    visit: &mut impl for<'value> FnMut(usize, Option<&'value [u8]>, ResolvedRecord),
) -> Result<(), AccessError> {
    adapter.visit_resolved_access_bytes(access, entry, |current| visit(index, current, resolved))
}

#[inline(always)]
fn apply_fixed_mutation<const CAPTURE_VALUE: bool>(
    adapter: &TableAdapter,
    entry: &mut Entry<'_, TableAdapter>,
    access: RecordAccess<'_>,
    index: usize,
    resolved: ResolvedRecord,
    modify: &mut impl for<'value> FnMut(
        usize,
        Option<&'value Value>,
        ResolvedRecord,
    ) -> Result<PointMutation, AccessError>,
    values: &mut Vec<Option<Value>>,
) -> Result<(), AccessError> {
    let loaded = adapter.prepare_resolved_access(access, entry)?;
    let current = current_state(entry, loaded.as_ref())?;
    let previous = current.value();
    let mutation = modify(index, previous, resolved)?;
    if CAPTURE_VALUE {
        values.push(previous.cloned());
    }
    match mutation {
        PointMutation::Keep => {}
        PointMutation::Put(value) => {
            stage_record_state(adapter, access, entry, RecordState::Live(value))?;
        }
        PointMutation::Remove => {
            if previous.is_some() {
                stage_record_state(adapter, access, entry, RecordState::tombstone())?;
            }
        }
    }
    Ok(())
}

#[inline(always)]
fn current_state<'state>(
    entry: &'state Entry<'_, TableAdapter>,
    loaded: Option<&'state RecordState>,
) -> Result<&'state RecordState, AccessError> {
    if *entry.local() != AdapterRole::Record {
        return Err(table_fault(
            "record operation received directory-local state",
        ));
    }
    match entry.intent() {
        Some(intent) => Ok(intent.record_state()),
        None => loaded.ok_or_else(|| table_fault("record operation has no reloaded snapshot")),
    }
}

#[inline(always)]
fn current_state_snapshot<'state>(
    entry: &'state Entry<'_, TableAdapter>,
    loaded: Option<&'state RecordState>,
) -> Result<&'state RecordState, AccessError> {
    current_state(entry, loaded)
}

#[inline(always)]
fn stage_record_state(
    _adapter: &TableAdapter,
    _access: RecordAccess<'_>,
    entry: &mut Entry<'_, TableAdapter>,
    replacement: RecordState,
) -> Result<(), AccessError> {
    if *entry.local() != AdapterRole::Record {
        return Err(table_fault(
            "record operation received directory-local state",
        ));
    }
    if let Some(intent) = entry.intent_mut() {
        intent.replace_record_state(replacement);
        return Ok(());
    }

    entry.stage(TableIntent::Record { replacement })
}

impl TableAdapter {
    fn observe_directory_generation(
        &self,
        entry: &mut Entry<'_, Self>,
    ) -> Result<u64, AccessError> {
        self.ensure_role(AdapterRole::Directory)?;
        if *entry.local() != AdapterRole::Directory {
            return Err(self.type_mismatch(AdapterPhase::Execute).into());
        }
        match entry.observation() {
            ObservationRef::Read(TableObservation::Directory(observation))
            | ObservationRef::UpgradedPredicate(TableObservation::Directory(observation)) => {
                return Ok(observation.generation);
            }
            ObservationRef::Unobserved => {}
            ObservationRef::Read(TableObservation::Record(_))
            | ObservationRef::UpgradedPredicate(TableObservation::Record(_))
            | ObservationRef::Predicate(_) => {
                self.table.poison();
                return Err(table_fault("directory observation state is inconsistent"));
            }
        }
        let observed = self.table.directory_generation.load(Ordering::Acquire);
        entry.record_read(TableObservation::Directory(DirectoryObservation {
            generation: observed,
        }))?;
        Ok(observed)
    }

    fn observe_trusted_scan_generation(
        &self,
        entry: &mut Entry<'_, Self>,
    ) -> Result<DirectoryObservation, AccessError> {
        self.ensure_role(AdapterRole::Scan)?;
        if *entry.local() != AdapterRole::Scan {
            return Err(self.type_mismatch(AdapterPhase::Execute).into());
        }
        match entry.observation() {
            ObservationRef::Read(TableObservation::Directory(observation))
            | ObservationRef::UpgradedPredicate(TableObservation::Directory(observation)) => {
                if self.table.scan_generation.load(Ordering::Acquire) != observation.generation {
                    return Err(Conflict::ReadValidation.into());
                }
                return Ok(*observation);
            }
            ObservationRef::Unobserved => {}
            ObservationRef::Read(TableObservation::Record(_))
            | ObservationRef::UpgradedPredicate(TableObservation::Record(_))
            | ObservationRef::Predicate(_) => {
                self.table.poison();
                return Err(table_fault(
                    "scan-generation observation state is inconsistent",
                ));
            }
        }
        let observation = DirectoryObservation {
            generation: self.table.scan_generation.load(Ordering::Acquire),
        };
        entry.record_read(TableObservation::Directory(observation))?;
        Ok(observation)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{atomic::AtomicBool, Barrier};
    use sto_core::{AbortReason, CommitOutcome, InvalidUse, RuntimeConfig};

    fn runtime_and_table(config: TableConfig) -> (Arc<Runtime>, Table) {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let table = Table::new_memory(&runtime, config);
        (runtime, table)
    }

    fn try_isolated_registry(config: TableConfig) -> Result<Registry, RegistrationError> {
        Registry::new(
            config,
            sto_core::RuntimeId::new(1).unwrap(),
            LockNamespaceId::new(1).unwrap(),
            LockClass::new(RECORD_LOCK_CLASS_VALUE).unwrap(),
        )
    }

    fn isolated_registry(config: TableConfig) -> Registry {
        try_isolated_registry(config).unwrap()
    }

    fn eager_registry_config(maximum: usize) -> TableConfig {
        let max_bytes = eager_registry_accounted_bytes(maximum, false).unwrap();
        TableConfig::new()
            .with_max_retained_records(maximum as u64)
            .with_max_retained_key_bytes((maximum as u64).saturating_mul(16))
            .with_max_consumed_record_ids(maximum as u64)
            .with_registry_layout(RegistryLayout::EagerContiguous { max_bytes })
    }

    fn trusted_scan_config() -> TableConfig {
        TableConfig::default().with_trusted_scan_value_generation(true)
    }

    fn bounded_value_config() -> TableConfig {
        TableConfig::default().with_bounded_atomic_values(true)
    }

    fn store_test_stable_value(
        state: &CommittedRecordState,
        cell: &StableAtomicValueCell,
        value: &[u8],
    ) -> u64 {
        assert!(value.len() > INLINE_VALUE_CAPACITY);
        assert!(value.len() <= STABLE_ATOMIC_VALUE_CAPACITY);
        for (index, chunk) in value[..INLINE_VALUE_HEAD_WORDS * 8]
            .chunks(std::mem::size_of::<u64>())
            .enumerate()
        {
            let mut word = [0_u8; std::mem::size_of::<u64>()];
            word[..chunk.len()].copy_from_slice(chunk);
            state.inline_head[index].store(u64::from_ne_bytes(word), Ordering::Relaxed);
        }
        cell.store_suffix(value);
        let descriptor = RECORD_STATE_STABLE_BASE + value.len() as u16;
        let packed =
            pack_record_state_word(descriptor, pack_inline_tail(value, INLINE_VALUE_CAPACITY));
        state.tail_and_descriptor.store(packed, Ordering::Release);
        packed
    }

    fn committed(outcome: Result<CommitOutcome, sto_core::CommitFailure>) {
        assert!(matches!(outcome, Ok(CommitOutcome::Committed(_))));
    }

    fn terminal_ready(
        outcome: TerminalReadVisitOutcome<'_>,
    ) -> (TerminalReadTransaction<'_, TerminalReadReady>, usize) {
        match outcome {
            TerminalReadVisitOutcome::Ready {
                transaction,
                visited,
            } => (transaction, visited),
            TerminalReadVisitOutcome::RetryOrdinary => {
                panic!("an all-hit terminal batch must be ready")
            }
        }
    }

    fn terminal_retry(outcome: TerminalReadVisitOutcome<'_>) {
        match outcome {
            TerminalReadVisitOutcome::RetryOrdinary => {}
            TerminalReadVisitOutcome::Ready { transaction, .. } => {
                transaction.abort();
                panic!("a terminal batch containing a miss must request retry");
            }
        }
    }

    fn committed_record_snapshot(record: &Record) -> RecordState {
        let observed = record.version.observe().unwrap();
        let candidate = record.state.load(None);
        assert!(record.version.validate(observed));
        match candidate {
            CommittedStateLoad::Complete { state, .. } => state,
            CommittedStateLoad::Incomplete(reason) => {
                panic!("stable committed record state is incomplete: {reason}")
            }
        }
    }

    fn fixed_insert_capacities(batch: &PointReadBatch) -> [usize; 5] {
        [
            batch.fixed_inserts.keys.capacity(),
            batch.fixed_inserts.positions.capacity(),
            batch.fixed_inserts.candidates.capacity(),
            batch.fixed_inserts.directory_tokens.capacity(),
            batch.fixed_inserts.results.capacity(),
        ]
    }

    fn seed(table: &Table, worker: &mut sto_core::WorkerContext, entries: &[(&[u8], &[u8])]) {
        let mut txn = worker.begin().unwrap();
        for (key, value) in entries {
            table
                .put_inner(&mut txn, None, key, Arc::from(*value))
                .unwrap();
        }
        committed(txn.commit());
    }

    #[allow(
        unsafe_code,
        reason = "tests keep borrowed output storage immutable through transaction finish"
    )]
    unsafe fn try_modify_fixed_borrowed_for_test<const KEY_LENGTH: usize>(
        table: &Table,
        txn: &mut Transaction<'_, Active>,
        key: &[u8; KEY_LENGTH],
        batch: &mut PointReadBatch,
        output: &mut [u8],
        modify: impl for<'buffer> FnMut(&'buffer mut [u8], usize) -> Result<usize, AccessError>,
    ) -> Result<Option<usize>, AccessError> {
        // SAFETY: Forwarded from this test helper's caller. Every call site
        // retains its output allocation unchanged until commit or abort.
        unsafe {
            table.try_modify_fixed_borrowed_inner(txn, None, key, batch, output, modify, |batch| {
                batch.push_record_id(table.shared().lookup(None, key)?);
                Ok(())
            })
        }
    }

    #[allow(
        unsafe_code,
        reason = "tests keep borrowed output storage immutable through transaction finish"
    )]
    unsafe fn try_modify_resolving_borrowed_for_test(
        table: &Table,
        txn: &mut Transaction<'_, Active>,
        key: &[u8],
        output: &mut [u8],
        modify: impl for<'buffer> FnOnce(&'buffer mut [u8], usize) -> Result<usize, AccessError>,
    ) -> Result<(Option<usize>, ResolvedRecord), AccessError> {
        // SAFETY: Forwarded from this test helper's caller. Every call site
        // retains its output allocation unchanged until commit or abort.
        unsafe {
            table.try_modify_resolving_borrowed_inner(txn, None, key, output, modify, || {
                table.shared().lookup(None, key)
            })
        }
    }

    fn scan_rows(
        table: &Table,
        txn: &mut Transaction<'_, Active>,
        request: ScanRequest<'_>,
    ) -> Vec<(Vec<u8>, Vec<u8>)> {
        table
            .scan_inner(txn, None, request)
            .unwrap()
            .into_iter()
            .map(|record| (record.key().to_vec(), record.value().to_vec()))
            .collect()
    }

    type OwnedScanRows = Vec<(Vec<u8>, Vec<u8>)>;

    fn trusted_scan_rows(
        table: &Table,
        txn: &mut Transaction<'_, Active>,
        request: ScanRequest<'_>,
    ) -> Result<(ScanVisitOutcome, OwnedScanRows), AccessError> {
        let mut rows = Vec::new();
        let mut scratch = ScanScratch::default();
        let outcome =
            table.visit_scan_bytes_trusted_inner(txn, None, request, &mut scratch, &mut |row| {
                rows.push((row.key().to_vec(), row.value().to_vec()));
                ScanControl::Continue
            })?;
        Ok((outcome, rows))
    }

    #[test]
    fn table_handles_are_send_sync_and_clones_preserve_identity() {
        fn assert_send_sync<T: Send + Sync>() {}
        assert_send_sync::<Table>();
        assert_send_sync::<Value>();
        assert_send_sync::<ResolvedRecord>();
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let clone = table.clone();
        assert_eq!(table.object_id(), clone.object_id());
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Healthy);
    }

    #[test]
    fn public_id_lane_and_private_direct_lane_keep_compact_opaque_tokens() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();

        let id_table = Table::new_memory(&runtime, TableConfig::default());
        assert_eq!(
            id_table.shared().record_token_mode,
            RecordTokenMode::RegistryId
        );
        let mut id_transaction = worker.begin().unwrap();
        let (_, id_resolved) = id_table
            .visit_get_resolving_inner(&mut id_transaction, None, b"id/lane", |_| ())
            .unwrap();
        assert_eq!(id_resolved.record_id.get(), 1);
        committed(id_transaction.commit());

        let direct_table = Table::new_memory_direct(&runtime, TableConfig::default());
        assert_eq!(
            direct_table.shared().record_token_mode,
            RecordTokenMode::DirectRecordPointer
        );
        let mut direct_transaction = worker.begin().unwrap();
        let (_, direct_resolved) = direct_table
            .visit_get_resolving_inner(&mut direct_transaction, None, b"direct/lane", |_| ())
            .unwrap();
        let logical_first = RecordId::new(1).unwrap();
        let first_entry = direct_table
            .shared()
            .registry
            .resolve_access(logical_first)
            .unwrap();
        assert_eq!(
            direct_resolved.record_id,
            direct_record::encode(first_entry).unwrap()
        );
        assert_ne!(direct_resolved.record_id, logical_first);
        committed(direct_transaction.commit());

        assert_eq!(std::mem::size_of::<TableKey>(), 8);
        assert_eq!(std::mem::size_of::<ResolvedRecord>(), 24);
        // The direct token keeps the observation and cached record.
        // BorrowedLockToken adds the runtime identity.
        assert_eq!(std::mem::size_of::<DirectRecordLockToken>(), 16);
        assert_eq!(
            std::mem::size_of::<BorrowedLockToken<DirectRecordLockToken>>(),
            24
        );
    }

    #[test]
    fn direct_write_rejects_a_generation_that_changed_before_lock_acquisition() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let table = Table::new_memory_direct(&runtime, TableConfig::default());
        let mut setup_worker = runtime.attach().unwrap();
        seed(&table, &mut setup_worker, &[(b"stale/write", b"before")]);
        drop(setup_worker);

        let mut stale_worker = runtime.attach().unwrap();
        let mut stale = stale_worker.begin().unwrap();
        assert_eq!(
            table
                .get_inner(&mut stale, None, b"stale/write")
                .unwrap()
                .as_deref(),
            Some(&b"before"[..])
        );

        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            scope
                .spawn(move || {
                    let mut worker = runtime.attach().unwrap();
                    let mut winner = worker.begin().unwrap();
                    table
                        .put_presence_inner(
                            &mut winner,
                            None,
                            b"stale/write",
                            Value::from(&b"winner"[..]),
                        )
                        .unwrap();
                    committed(winner.commit());
                })
                .join()
                .unwrap();
        });

        assert!(table
            .put_presence_inner(&mut stale, None, b"stale/write", Value::from(&b"loser"[..]),)
            .unwrap());
        assert_eq!(
            stale.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );

        let mut verify = stale_worker.begin().unwrap();
        assert_eq!(
            table
                .get_inner(&mut verify, None, b"stale/write")
                .unwrap()
                .as_deref(),
            Some(&b"winner"[..])
        );
        committed(verify.commit());
    }

    #[test]
    fn direct_mixed_batch_validates_reads_after_write_acquisition() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let read_table = Table::new_memory_direct(&runtime, TableConfig::default());
        let write_table = Table::new_memory_direct(&runtime, TableConfig::default());
        let mut setup_worker = runtime.attach().unwrap();
        seed(
            &read_table,
            &mut setup_worker,
            &[(b"mixed/read", b"before")],
        );
        seed(
            &write_table,
            &mut setup_worker,
            &[(b"mixed/write", b"before")],
        );
        drop(setup_worker);

        let mut worker = runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        assert_eq!(
            read_table
                .get_inner(&mut transaction, None, b"mixed/read")
                .unwrap()
                .as_deref(),
            Some(&b"before"[..])
        );
        assert!(write_table
            .put_presence_inner(
                &mut transaction,
                None,
                b"mixed/write",
                Value::from(&b"unpublished"[..]),
            )
            .unwrap());

        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let read_table = read_table.clone();
            scope
                .spawn(move || {
                    let mut worker = runtime.attach().unwrap();
                    let mut winner = worker.begin().unwrap();
                    read_table
                        .put_presence_inner(
                            &mut winner,
                            None,
                            b"mixed/read",
                            Value::from(&b"changed"[..]),
                        )
                        .unwrap();
                    committed(winner.commit());
                })
                .join()
                .unwrap();
        });

        assert_eq!(
            transaction.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );
        let mut verify = worker.begin().unwrap();
        assert_eq!(
            write_table
                .get_inner(&mut verify, None, b"mixed/write")
                .unwrap()
                .as_deref(),
            Some(&b"before"[..])
        );
        committed(verify.commit());
    }

    #[test]
    fn repeated_direct_write_uses_one_observation_and_the_last_intent() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let table = Table::new_memory_direct(&runtime, TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        seed(&table, &mut worker, &[(b"repeat/write", b"before")]);

        let mut transaction = worker.begin().unwrap();
        assert!(table
            .put_presence_inner(
                &mut transaction,
                None,
                b"repeat/write",
                Value::from(&b"first"[..]),
            )
            .unwrap());
        let record_id = table
            .shared()
            .lookup(None, b"repeat/write")
            .unwrap()
            .unwrap();
        transaction
            .with_item(
                &table.record_resource,
                TableKey::Record(record_id),
                |entry| {
                    let intent = entry
                        .intent()
                        .ok_or_else(|| table_fault("direct write did not stage an intent"))?;
                    assert_eq!(intent.record_state().value().unwrap().as_ref(), b"first");
                    Ok(())
                },
            )
            .unwrap();
        assert!(table
            .put_presence_inner(
                &mut transaction,
                None,
                b"repeat/write",
                Value::from(&b"last"[..]),
            )
            .unwrap());
        transaction
            .with_item(
                &table.record_resource,
                TableKey::Record(record_id),
                |entry| {
                    let intent = entry
                        .intent()
                        .ok_or_else(|| table_fault("repeated write lost its intent"))?;
                    assert_eq!(intent.record_state().value().unwrap().as_ref(), b"last");
                    Ok(())
                },
            )
            .unwrap();
        committed(transaction.commit());

        let mut verify = worker.begin().unwrap();
        assert_eq!(
            table
                .get_inner(&mut verify, None, b"repeat/write")
                .unwrap()
                .as_deref(),
            Some(&b"last"[..])
        );
        committed(verify.commit());
    }

    #[test]
    fn direct_lock_failure_releases_every_earlier_write() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let first = Table::new_memory_direct(&runtime, TableConfig::default());
        let blocked = Table::new_memory_direct(&runtime, TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        seed(&first, &mut worker, &[(b"first", b"before")]);
        seed(&blocked, &mut worker, &[(b"blocked", b"before")]);

        let first_id = first.shared().lookup(None, b"first").unwrap().unwrap();
        let blocked_id = blocked.shared().lookup(None, b"blocked").unwrap().unwrap();
        let first_record = first.shared().resolve_directory_record(first_id).unwrap();
        let blocked_record = blocked
            .shared()
            .resolve_directory_record(blocked_id)
            .unwrap();
        let first_version = first_record.version.observe().unwrap();
        let blocked_version = blocked_record.version.observe().unwrap();

        let mut transaction = worker.begin().unwrap();
        assert!(first
            .put_presence_inner(
                &mut transaction,
                None,
                b"first",
                Value::from(&b"unpublished-first"[..]),
            )
            .unwrap());
        assert!(blocked
            .put_presence_inner(
                &mut transaction,
                None,
                b"blocked",
                Value::from(&b"unpublished-blocked"[..]),
            )
            .unwrap());

        let mut blocker = blocked_record
            .version
            .try_acquire_detached(sto_core::OwnerId::new(777).unwrap())
            .unwrap();
        assert_eq!(
            transaction.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::LockBusy))
        );
        assert_eq!(first_record.version.observe().unwrap(), first_version);
        blocker.release_abort(&blocked_record.version).unwrap();
        assert_eq!(blocked_record.version.observe().unwrap(), blocked_version);

        let mut verify = worker.begin().unwrap();
        assert_eq!(
            first
                .get_inner(&mut verify, None, b"first")
                .unwrap()
                .as_deref(),
            Some(&b"before"[..])
        );
        assert_eq!(
            blocked
                .get_inner(&mut verify, None, b"blocked")
                .unwrap()
                .as_deref(),
            Some(&b"before"[..])
        );
        committed(verify.commit());
    }

    #[test]
    fn direct_directory_observation_shares_the_record_commit_capability() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let table = Table::new_memory_direct(&runtime, TableConfig::default());

        let record = table
            .record_resource
            .adapter()
            .direct_commit_capability()
            .unwrap();
        let directory = table
            .directory_resource
            .adapter()
            .direct_commit_capability()
            .unwrap();
        assert!(std::ptr::eq(record, directory));
    }

    #[test]
    fn direct_scan_and_record_write_commit_together() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let table = Table::new_memory_direct(&runtime, TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        seed(&table, &mut worker, &[(b"a", b"old-a"), (b"b", b"old-b")]);

        let mut transaction = worker.begin().unwrap();
        assert_eq!(
            scan_rows(
                &table,
                &mut transaction,
                ScanRequest::new(ScanDirection::Forward, 16),
            ),
            vec![
                (b"a".to_vec(), b"old-a".to_vec()),
                (b"b".to_vec(), b"old-b".to_vec())
            ]
        );
        table
            .put_inner(&mut transaction, None, b"a", Value::from(&b"new-a"[..]))
            .unwrap();
        committed(transaction.commit());

        let mut verify = worker.begin().unwrap();
        assert_eq!(
            table.get_inner(&mut verify, None, b"a").unwrap().as_deref(),
            Some(&b"new-a"[..])
        );
        committed(verify.commit());
    }

    #[test]
    fn direct_resolved_token_is_rejected_by_the_wrong_table_before_dereference() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let first = Table::new_memory_direct(&runtime, TableConfig::default());
        let second = Table::new_memory_direct(&runtime, TableConfig::default());
        seed(&first, &mut worker, &[(b"direct/wrong-table", b"value")]);

        let mut resolve = worker.begin().unwrap();
        let (_, token) = first
            .visit_get_resolving_inner(&mut resolve, None, b"direct/wrong-table", |_| ())
            .unwrap();
        committed(resolve.commit());

        let mut wrong = worker.begin().unwrap();
        assert_eq!(
            second.visit_get_resolved_bytes(&mut wrong, token, |_| ()),
            Err(AccessError::InvalidUse(InvalidUse::ResourceTypeMismatch))
        );
        wrong.abort();
        assert_eq!(first.health(), TableHealth::Healthy);
        assert_eq!(second.health(), TableHealth::Healthy);
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Healthy);
    }

    #[test]
    fn direct_prefetch_is_a_nonvalidating_non_dereferencing_hint() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let table = Table::new_memory_direct(&runtime, TableConfig::default());
        seed(&table, &mut worker, &[(b"prefetch", b"value")]);

        let token = table.shared().lookup(None, b"prefetch").unwrap().unwrap();
        direct_record::prefetch(token).unwrap();

        let malformed = RecordId::new(token.get() + 1).unwrap();
        direct_record::prefetch(malformed).unwrap();
        assert_eq!(table.health(), TableHealth::Healthy);
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Healthy);
    }

    #[test]
    fn fixed_byte_prefetch_retains_callback_free_slot_validation() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let table = Table::new_memory_direct(&runtime, TableConfig::default());
        seed(&table, &mut worker, &[(b"seed-key", b"value")]);
        let token = table.shared().lookup(None, b"seed-key").unwrap().unwrap();
        let malformed = RecordId::new(token.get() + 1).unwrap();

        let mut transaction = worker.begin().unwrap();
        let mut batch = PointReadBatch::with_capacity(1);
        let mut callbacks = 0;
        let result = table.visit_fixed_bytes_inner(
            &mut transaction,
            None,
            &[*b"read-key"],
            &mut batch,
            |_index, _value| callbacks += 1,
            |batch| {
                batch.push_record_id(Some(malformed));
                Ok(())
            },
        );
        assert!(result.is_err());
        assert_eq!(callbacks, 0);
        assert!(batch.is_empty());
        assert_eq!(table.health(), TableHealth::Poisoned);
        transaction.abort();
    }

    #[test]
    fn fixed_modify_prefetch_retains_callback_free_slot_validation() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let table = Table::new_memory_direct(&runtime, TableConfig::default());
        seed(&table, &mut worker, &[(b"seed-key", b"value")]);
        let token = table.shared().lookup(None, b"seed-key").unwrap().unwrap();
        let unallocated = RecordId::new(token.get() + REGISTRY_ENTRY_SLOT_BYTES as u64).unwrap();

        let mut transaction = worker.begin().unwrap();
        let mut batch = PointReadBatch::with_capacity(1);
        let mut callbacks = 0;
        let result = table.modify_fixed_visit_inner::<false, 8>(
            &mut transaction,
            None,
            &[*b"modify!!"],
            &mut batch,
            |_index, _value| {
                callbacks += 1;
                Ok(PointMutation::Keep)
            },
            |batch| {
                batch.push_record_id(Some(unallocated));
                Ok(())
            },
        );
        assert!(result.is_err());
        assert_eq!(callbacks, 0);
        assert!(batch.is_empty());
        assert_eq!(table.health(), TableHealth::Poisoned);
        transaction.abort();
    }

    #[test]
    fn direct_transaction_resource_retains_record_arena_through_install() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let table = Table::new_memory_direct(&runtime, TableConfig::default());
        let shared = Arc::downgrade(&table.record_resource.adapter().table);

        let mut transaction = worker.begin().unwrap();
        table
            .put_inner(
                &mut transaction,
                None,
                b"direct/retained",
                Value::from(&b"installed-after-handle-drop"[..]),
            )
            .unwrap();
        drop(table);
        assert!(shared.upgrade().is_some());
        committed(transaction.commit());
    }

    #[test]
    fn direct_tokens_remain_stable_across_lazy_registry_segment_growth() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let table = Table::new_memory_direct(&runtime, TableConfig::default());
        seed(&table, &mut worker, &[(b"direct/first", b"first")]);

        let mut resolve = worker.begin().unwrap();
        let (_, first_token) = table
            .visit_get_resolving_inner(&mut resolve, None, b"direct/first", |_| ())
            .unwrap();
        committed(resolve.commit());
        let first_access = table
            .shared()
            .resolve_directory_access(first_token.record_id)
            .unwrap();
        let first_record = first_access.record;
        let first_address = std::ptr::from_ref(first_record);
        let cached = direct_record::CachedRecord::new(first_access);

        let mut grow = worker.begin().unwrap();
        for index in 0..REGISTRY_SEGMENT_SLOTS {
            let key = format!("direct/grow/{index:04}");
            table
                .put_inner(
                    &mut grow,
                    None,
                    key.as_bytes(),
                    Value::from(&(index as u64).to_le_bytes()),
                )
                .unwrap();
        }
        committed(grow.commit());
        assert!(std::ptr::eq(
            cached
                .get(&table.shared().registry, first_token.record_id)
                .unwrap()
                .record,
            first_address
        ));

        let mut reuse = worker.begin().unwrap();
        table
            .visit_get_resolved_bytes(&mut reuse, first_token, |value| {
                assert_eq!(value, Some(&b"first"[..]));
            })
            .unwrap();
        assert!(table
            .put_resolved_with_previous_presence(&mut reuse, first_token, b"updated")
            .unwrap());
        assert_eq!(
            table
                .get_inner(&mut reuse, None, b"direct/grow/1023")
                .unwrap()
                .as_deref(),
            Some(&(1023_u64).to_le_bytes()[..])
        );
        committed(reuse.commit());
        assert_eq!(table.health(), TableHealth::Healthy);
    }

    #[test]
    fn cached_direct_record_reborrows_the_exact_standard_and_bounded_slots() {
        for bounded_atomic_values in [false, true] {
            let registry = isolated_registry(
                TableConfig::new()
                    .with_max_retained_records(1)
                    .with_max_retained_key_bytes(1)
                    .with_max_consumed_record_ids(1)
                    .with_bounded_atomic_values(bounded_atomic_values),
            );
            let (_candidate, token) = registry
                .reserve_candidate_with_mode(b"a", RecordTokenMode::DirectRecordPointer)
                .unwrap();
            let resolved = registry.resolve_direct_access(token).unwrap();
            let expected_record = std::ptr::from_ref(resolved.record());
            let expected_stable = resolved.stable.map(std::ptr::from_ref);
            let cached = direct_record::CachedRecord::new(RecordAccess {
                record_id: token,
                record: resolved.record(),
                stable: resolved.stable,
            });

            let rebound = cached.get(&registry, token).unwrap();
            assert!(std::ptr::eq(rebound.record, expected_record));
            assert_eq!(rebound.stable.is_some(), bounded_atomic_values);
            match (rebound.stable, expected_stable) {
                (None, None) => {}
                (Some(rebound), Some(expected)) => {
                    assert!(std::ptr::eq(rebound, expected));
                }
                _ => panic!("cached direct record changed its concrete registry layout"),
            }
        }
    }

    #[test]
    #[cfg(debug_assertions)]
    fn cached_direct_record_debug_check_rejects_a_different_token() {
        let registry = isolated_registry(
            TableConfig::new()
                .with_max_retained_records(2)
                .with_max_retained_key_bytes(2)
                .with_max_consumed_record_ids(2)
                .with_bounded_atomic_values(true),
        );
        let (_first_candidate, first_token) = registry
            .reserve_candidate_with_mode(b"a", RecordTokenMode::DirectRecordPointer)
            .unwrap();
        let (_second_candidate, second_token) = registry
            .reserve_candidate_with_mode(b"b", RecordTokenMode::DirectRecordPointer)
            .unwrap();
        let first = registry.resolve_direct_access(first_token).unwrap();
        let cached = direct_record::CachedRecord::new(RecordAccess {
            record_id: first_token,
            record: first.record(),
            stable: first.stable,
        });

        assert!(cached.get(&registry, second_token).is_err());
    }

    #[test]
    fn resolved_record_lane_skips_directory_lookup_and_preserves_occ_semantics() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        seed(&table, &mut worker, &[(b"resolved", b"original-value")]);
        let lookup_count = || match &table.shared().directory {
            Directory::Memory(directory) => directory.point_lookups.load(Ordering::Relaxed),
        };
        let before = lookup_count();

        let mut txn = worker.begin().unwrap();
        let ((), resolved) = table
            .visit_get_resolving_inner(&mut txn, None, b"resolved", |value| {
                assert_eq!(value.map(Value::as_ref), Some(&b"original-value"[..]));
            })
            .unwrap();
        assert_eq!(lookup_count(), before + 1);
        assert!(table.owns_resolved(resolved));
        assert!(table
            .put_resolved_with_previous_presence(&mut txn, resolved, b"replacement-value")
            .unwrap());
        table
            .visit_get_resolved(&mut txn, resolved, |value| {
                assert_eq!(value.map(Value::as_ref), Some(&b"replacement-value"[..]));
            })
            .unwrap();
        assert_eq!(lookup_count(), before + 1);
        committed(txn.commit());

        let mut verify = worker.begin().unwrap();
        assert_eq!(
            table
                .get_inner(&mut verify, None, b"resolved")
                .unwrap()
                .as_deref(),
            Some(&b"replacement-value"[..])
        );
        committed(verify.commit());
    }

    #[test]
    fn presence_reads_cover_present_missing_resolved_and_staged_liveness() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        seed(&table, &mut worker, &[(b"presence/live", b"payload")]);
        let lookup_count = || match &table.shared().directory {
            Directory::Memory(directory) => directory.point_lookups.load(Ordering::Relaxed),
        };
        let before = lookup_count();

        let mut transaction = worker.begin().unwrap();
        let (present, live) = table
            .contains_resolving_inner(&mut transaction, None, b"presence/live")
            .unwrap();
        assert!(present);
        assert!(table.owns_resolved(live));
        assert_eq!(lookup_count(), before + 1);
        assert!(table.contains_resolved(&mut transaction, live).unwrap());
        assert!(table.contains_resolved(&mut transaction, live).unwrap());
        assert_eq!(lookup_count(), before + 1);

        let (present, missing) = table
            .contains_resolving_inner(&mut transaction, None, b"presence/missing")
            .unwrap();
        assert!(!present);
        assert!(table.owns_resolved(missing));
        assert_eq!(lookup_count(), before + 2);
        assert!(!table.contains_resolved(&mut transaction, missing).unwrap());

        assert!(!table
            .put_resolved_with_previous_presence(&mut transaction, missing, b"staged")
            .unwrap());
        assert!(table.contains_resolved(&mut transaction, missing).unwrap());
        assert!(table
            .remove_resolved_with_previous_presence(&mut transaction, missing)
            .unwrap());
        assert!(!table.contains_resolved(&mut transaction, missing).unwrap());
        assert_eq!(lookup_count(), before + 2);
        transaction.abort();
    }

    #[test]
    fn presence_resolved_rejects_a_token_from_another_table() {
        let (runtime, first) = runtime_and_table(TableConfig::default());
        let second = Table::new_memory(&runtime, TableConfig::default());
        let mut worker = runtime.attach().unwrap();

        let mut resolve = worker.begin().unwrap();
        let (_, token) = first
            .contains_resolving_inner(&mut resolve, None, b"presence/token")
            .unwrap();
        resolve.abort();

        let mut wrong_table = worker.begin().unwrap();
        assert_eq!(
            second.contains_resolved(&mut wrong_table, token),
            Err(AccessError::InvalidUse(InvalidUse::ResourceTypeMismatch))
        );
        assert!(!wrong_table.is_doomed());
        wrong_table.abort();
    }

    #[test]
    fn repeated_presence_read_keeps_its_snapshot_and_commit_validates() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut setup_worker = runtime.attach().unwrap();
        seed(
            &table,
            &mut setup_worker,
            &[(b"presence/conflict", b"before")],
        );
        drop(setup_worker);

        let mut reader_worker = runtime.attach().unwrap();
        let mut reader = reader_worker.begin().unwrap();
        let (present, token) = table
            .contains_resolving_inner(&mut reader, None, b"presence/conflict")
            .unwrap();
        assert!(present);

        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            scope
                .spawn(move || {
                    let mut worker = runtime.attach().unwrap();
                    let mut writer = worker.begin().unwrap();
                    assert!(table
                        .remove_presence_inner(&mut writer, None, b"presence/conflict")
                        .unwrap());
                    committed(writer.commit());
                })
                .join()
                .unwrap();
        });

        // Repeated metadata-only access retains this transaction's first
        // logical snapshot; certification still rejects the changed version.
        assert!(table.contains_resolved(&mut reader, token).unwrap());
        assert_eq!(
            reader.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );

        let mut verify = reader_worker.begin().unwrap();
        assert!(!table.contains_resolved(&mut verify, token).unwrap());
        committed(verify.commit());
    }

    #[test]
    fn byte_visitors_preserve_inline_shared_tombstone_resolved_and_staged_semantics() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let shared = vec![0xa7; INLINE_VALUE_CAPACITY + 19];
        seed(
            &table,
            &mut worker,
            &[(b"bytes/inline", b"inline"), (b"bytes/shared", &shared)],
        );

        let mut transaction = worker.begin().unwrap();
        let (inline, inline_resolved) = table
            .visit_get_resolving_bytes_inner(&mut transaction, None, b"bytes/inline", |value| {
                value.map(<[u8]>::to_vec)
            })
            .unwrap();
        assert_eq!(inline.as_deref(), Some(&b"inline"[..]));
        assert!(table.owns_resolved(inline_resolved));

        let (large, large_resolved) = table
            .visit_get_resolving_bytes_inner(&mut transaction, None, b"bytes/shared", |value| {
                value.map(<[u8]>::to_vec)
            })
            .unwrap();
        assert_eq!(large.as_deref(), Some(shared.as_slice()));
        assert_eq!(
            table
                .visit_get_resolved_bytes(&mut transaction, large_resolved, |value| {
                    value.map(<[u8]>::to_vec)
                })
                .unwrap()
                .as_deref(),
            Some(shared.as_slice())
        );

        let (missing, missing_resolved) = table
            .visit_get_resolving_bytes_inner(&mut transaction, None, b"bytes/missing", |value| {
                value.map(<[u8]>::to_vec)
            })
            .unwrap();
        assert_eq!(missing, None);
        assert!(table.owns_resolved(missing_resolved));

        assert!(table
            .put_resolved_with_previous_presence(
                &mut transaction,
                inline_resolved,
                b"staged replacement",
            )
            .unwrap());
        table
            .visit_get_resolved_bytes(&mut transaction, inline_resolved, |value| {
                assert_eq!(value, Some(&b"staged replacement"[..]));
            })
            .unwrap();
        assert!(table
            .remove_resolved_with_previous_presence(&mut transaction, large_resolved)
            .unwrap());
        table
            .visit_get_resolved_bytes(&mut transaction, large_resolved, |value| {
                assert_eq!(value, None);
            })
            .unwrap();
        transaction.abort();
    }

    #[test]
    fn caller_buffer_reads_preserve_boundaries_capacity_misses_and_staged_values() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let lengths = [0_usize, 1, 8, 9, 31, 32, 33, 38, 39];
        let values = lengths
            .into_iter()
            .map(|length| {
                (0..length)
                    .map(|index| (index as u8).wrapping_mul(37).wrapping_add(11))
                    .collect::<Vec<_>>()
            })
            .collect::<Vec<_>>();

        let mut setup = worker.begin().unwrap();
        for (index, value) in values.iter().enumerate() {
            let key = format!("copy/boundary/{index}");
            table
                .put_inner(
                    &mut setup,
                    None,
                    key.as_bytes(),
                    Value::from(value.as_slice()),
                )
                .unwrap();
        }
        committed(setup.commit());

        let mut read = worker.begin().unwrap();
        let mut tokens = Vec::new();
        for (index, value) in values.iter().enumerate() {
            let key = format!("copy/boundary/{index}");
            let mut output = vec![0xd3; value.len() + 3];
            let (outcome, resolved) = table
                .copy_get_resolving_inner(&mut read, None, key.as_bytes(), &mut output)
                .unwrap();
            assert_eq!(outcome, ValueCopyOutcome::Copied { len: value.len() });
            assert_eq!(&output[..value.len()], value);
            assert_eq!(&output[value.len()..], &[0xd3; 3]);
            tokens.push(resolved);
        }

        for (index, value) in values
            .iter()
            .enumerate()
            .filter(|(_, value)| !value.is_empty())
        {
            let mut output = vec![0xa5; value.len() - 1];
            assert_eq!(
                table
                    .copy_get_resolved(&mut read, tokens[index], &mut output)
                    .unwrap(),
                ValueCopyOutcome::BufferTooSmall {
                    required: value.len()
                }
            );
            assert_eq!(output, vec![0xa5; value.len() - 1]);
        }

        let mut missing_output = [0x6c; 7];
        let (missing, missing_resolved) = table
            .copy_get_resolving_inner(&mut read, None, b"copy/missing", &mut missing_output)
            .unwrap();
        assert_eq!(missing, ValueCopyOutcome::Miss);
        assert_eq!(missing_output, [0x6c; 7]);

        assert!(
            table
                .put_resolved_with_previous_presence(
                    &mut read,
                    tokens[4],
                    b"staged inline replacement",
                )
                .unwrap()
        );
        let mut staged_inline = [0_u8; 25];
        assert_eq!(
            table
                .copy_get_resolved(&mut read, tokens[4], &mut staged_inline)
                .unwrap(),
            ValueCopyOutcome::Copied { len: 25 }
        );
        assert_eq!(&staged_inline, b"staged inline replacement");

        let staged_shared = vec![0x91; INLINE_VALUE_CAPACITY + 23];
        assert!(table
            .put_resolved_with_previous_presence(&mut read, tokens[8], &staged_shared)
            .unwrap());
        let mut shared_output = vec![0_u8; staged_shared.len()];
        assert_eq!(
            table
                .copy_get_resolved(&mut read, tokens[8], &mut shared_output)
                .unwrap(),
            ValueCopyOutcome::Copied {
                len: staged_shared.len()
            }
        );
        assert_eq!(shared_output, staged_shared);

        assert!(!table
            .remove_resolved_with_previous_presence(&mut read, missing_resolved)
            .unwrap());
        let mut removed_output = [0x44; 3];
        assert_eq!(
            table
                .copy_get_resolved(&mut read, missing_resolved, &mut removed_output)
                .unwrap(),
            ValueCopyOutcome::Miss
        );
        assert_eq!(removed_output, [0x44; 3]);
        read.abort();
    }

    #[test]
    fn caller_buffer_inline_conflict_and_foreign_cache_token_leave_output_unchanged() {
        let (runtime, first) = runtime_and_table(TableConfig::default());
        let second = Table::new_memory(&runtime, TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        seed(
            &first,
            &mut worker,
            &[(b"copy/conflict", b"inline payload")],
        );

        let record_id = first
            .shared()
            .lookup(None, b"copy/conflict")
            .unwrap()
            .unwrap();
        let record = first.shared().resolve_directory_record(record_id).unwrap();
        let observed = record.version.observe().unwrap();
        let tail_and_descriptor = record.state.tail_and_descriptor.load(Ordering::Acquire);
        let owner = sto_core::OwnerId::new(0).unwrap();
        let mut guard = record.version.try_acquire_detached(owner).unwrap();
        let mut conflicted_output = [0xbe; INLINE_VALUE_CAPACITY];
        assert_eq!(
            record.state.copy_inline_after_observation(
                &record.version,
                observed,
                tail_and_descriptor,
                b"inline payload".len(),
                &mut conflicted_output,
            ),
            Err(AccessError::Conflict(Conflict::ReadValidation))
        );
        assert_eq!(conflicted_output, [0xbe; INLINE_VALUE_CAPACITY]);
        guard.release_abort(&record.version).unwrap();

        let mut resolve = worker.begin().unwrap();
        let (_, token) = first
            .copy_get_resolving_inner(
                &mut resolve,
                None,
                b"copy/conflict",
                &mut [0_u8; INLINE_VALUE_CAPACITY],
            )
            .unwrap();
        resolve.abort();

        let mut foreign = worker.begin().unwrap();
        let mut foreign_output = [0x72; 9];
        assert_eq!(
            second
                .try_copy_get_cached_resolved(&mut foreign, token, &mut foreign_output)
                .unwrap(),
            None
        );
        assert_eq!(foreign_output, [0x72; 9]);
        assert!(!foreign.is_doomed());
        foreign.abort();
    }

    #[test]
    fn shared_byte_lease_stays_valid_across_replacement_and_reader_conflicts() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let original = vec![0x41; INLINE_VALUE_CAPACITY + 31];
        let replacement = vec![0xc9; INLINE_VALUE_CAPACITY + 47];
        let mut setup_worker = runtime.attach().unwrap();
        seed(
            &table,
            &mut setup_worker,
            &[(b"bytes/pinned", original.as_slice())],
        );
        drop(setup_worker);

        let mut reader_worker = runtime.attach().unwrap();
        let mut reader = reader_worker.begin().unwrap();
        table
            .visit_get_bytes_inner(&mut reader, None, b"bytes/pinned", |value| {
                let value = value.expect("the committed shared value is live");
                assert_eq!(value, original);

                std::thread::scope(|scope| {
                    let runtime = Arc::clone(&runtime);
                    let table = table.clone();
                    let replacement = replacement.clone();
                    scope
                        .spawn(move || {
                            let mut writer_worker = runtime.attach().unwrap();
                            let mut writer = writer_worker.begin().unwrap();
                            table
                                .put_inner(
                                    &mut writer,
                                    None,
                                    b"bytes/pinned",
                                    Value::from(replacement),
                                )
                                .unwrap();
                            committed(writer.commit());
                        })
                        .join()
                        .unwrap();
                });

                // The ArcSwap guard protects the old allocation for the whole
                // callback even though the embedded slot now names a new Arc.
                assert_eq!(value, original);
            })
            .unwrap();
        assert_eq!(
            reader.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );

        let mut verify = reader_worker.begin().unwrap();
        assert_eq!(
            table
                .visit_get_bytes_inner(&mut verify, None, b"bytes/pinned", |value| {
                    value.map(<[u8]>::to_vec)
                })
                .unwrap()
                .as_deref(),
            Some(replacement.as_slice())
        );
        committed(verify.commit());
    }

    #[test]
    fn scanned_resolved_records_skip_point_lookup_for_put_and_remove() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        seed(&table, &mut worker, &[(b"a", b"old-a"), (b"b", b"old-b")]);
        let lookup_count = || match &table.shared().directory {
            Directory::Memory(directory) => directory.point_lookups.load(Ordering::Relaxed),
        };
        let before = lookup_count();

        let mut txn = worker.begin().unwrap();
        let rows = table
            .scan_inner(&mut txn, None, ScanRequest::new(ScanDirection::Forward, 2))
            .unwrap();
        assert_eq!(lookup_count(), before);
        let resolved_a = rows
            .iter()
            .find(|row| row.key() == b"a")
            .expect("scan returned a")
            .resolved();
        let resolved_b = rows
            .iter()
            .find(|row| row.key() == b"b")
            .expect("scan returned b")
            .resolved();
        assert!(table
            .put_resolved_with_previous_presence(&mut txn, resolved_a, b"new-a")
            .unwrap());
        assert!(table
            .remove_resolved_with_previous_presence(&mut txn, resolved_b)
            .unwrap());
        assert_eq!(lookup_count(), before);
        committed(txn.commit());

        let mut verify = worker.begin().unwrap();
        assert_eq!(
            table.get_inner(&mut verify, None, b"a").unwrap().as_deref(),
            Some(&b"new-a"[..])
        );
        assert!(table.get_inner(&mut verify, None, b"b").unwrap().is_none());
        committed(verify.commit());
    }

    #[test]
    fn resolved_record_validates_table_and_survives_tombstone_abort() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let other_table = Table::new_memory(&runtime, TableConfig::default());
        let mut worker = runtime.attach().unwrap();

        let mut miss = worker.begin().unwrap();
        let ((), resolved) = table
            .visit_get_resolving_inner(&mut miss, None, b"missing", |value| {
                assert!(value.is_none());
            })
            .unwrap();
        assert_eq!(miss.abort().reason(), &AbortReason::Explicit);

        let mut wrong_table = worker.begin().unwrap();
        assert_eq!(
            other_table
                .visit_get_resolved(&mut wrong_table, resolved, |_| ())
                .unwrap_err(),
            AccessError::InvalidUse(InvalidUse::ResourceTypeMismatch)
        );
        assert_eq!(
            other_table
                .remove_resolved_with_previous_presence(&mut wrong_table, resolved)
                .unwrap_err(),
            AccessError::InvalidUse(InvalidUse::ResourceTypeMismatch)
        );
        assert_eq!(wrong_table.abort().reason(), &AbortReason::Explicit);

        let mut resurrect = worker.begin().unwrap();
        table
            .visit_get_resolved(&mut resurrect, resolved, |value| {
                assert!(value.is_none());
            })
            .unwrap();
        assert!(!table
            .put_resolved_with_previous_presence(&mut resurrect, resolved, b"now-live")
            .unwrap());
        committed(resurrect.commit());

        let (foreign_runtime, foreign_table) = runtime_and_table(TableConfig::default());
        let mut foreign_worker = foreign_runtime.attach().unwrap();
        let mut foreign_txn = foreign_worker.begin().unwrap();
        assert_eq!(
            foreign_table
                .visit_get_resolved(&mut foreign_txn, resolved, |_| ())
                .unwrap_err(),
            AccessError::InvalidUse(InvalidUse::WrongRuntime)
        );
        assert_eq!(foreign_txn.abort().reason(), &AbortReason::Explicit);
    }

    #[test]
    fn registry_layout_configuration_is_explicit_and_lazy_by_default() {
        let default = TableConfig::new();
        assert_eq!(default.registry_layout(), RegistryLayout::LazySegmented);
        assert!(!default.unique_lock_requests());
        let eager = default.with_registry_layout(RegistryLayout::EagerContiguous {
            max_bytes: 8 * 1024 * 1024,
        });
        assert_eq!(
            eager.registry_layout(),
            RegistryLayout::EagerContiguous {
                max_bytes: 8 * 1024 * 1024
            }
        );
        assert_eq!(eager.max_consumed_record_ids(), 4_000_000);
        assert!(eager.with_unique_lock_requests(true).unique_lock_requests());
    }

    #[test]
    #[cfg(target_pointer_width = "64")]
    fn record_transaction_types_stay_cache_compact() {
        assert_eq!(INLINE_VALUE_CAPACITY, 38);
        assert_eq!(INLINE_VALUE_WORDS, 5);
        assert_eq!(SHARED_INLINE_VALUE_CAPACITY, 128);
        assert_eq!(std::mem::size_of::<SharedValue>(), 136);
        assert_eq!(std::mem::size_of::<ValueRepr>(), 40);
        assert_eq!(std::mem::size_of::<Value>(), 40);
        assert_eq!(std::mem::size_of::<BorrowedStagingValue<'_>>(), 24);
        assert_eq!(std::mem::size_of::<RecordState>(), 40);
        assert_eq!(std::mem::size_of::<Option<RecordState>>(), 40);
        assert_eq!(std::mem::size_of::<Arc<SharedValue>>(), 8);
        assert_eq!(std::mem::size_of::<ArcSwapOption<SharedValue>>(), 8);
        assert_eq!(std::mem::size_of::<CommittedRecordState>(), 48);
        assert_eq!(std::mem::offset_of!(CommittedRecordState, inline_head), 0);
        assert_eq!(
            std::mem::offset_of!(CommittedRecordState, tail_and_descriptor),
            32
        );
        assert_eq!(std::mem::offset_of!(CommittedRecordState, shared), 40);
        assert_eq!(std::mem::size_of::<Record>(), 56);
        assert_eq!(REGISTRY_ENTRY_PADDING_BYTES, 7);
        assert_eq!(std::mem::size_of::<RegistryEntry>(), 64);
        assert_eq!(std::mem::align_of::<RegistryEntry>(), 64);
        assert_eq!(std::mem::offset_of!(RegistryEntry, record), 0);
        assert_eq!(std::mem::offset_of!(RegistryEntry, state), 56);
        assert_eq!(std::mem::size_of::<direct_record::CachedRecord>(), 8);
        assert_eq!(std::mem::align_of::<direct_record::CachedRecord>(), 8);
        assert_eq!(std::mem::size_of::<DirectRecordLockGuard>(), 40);
        assert_eq!(std::mem::size_of::<Option<DirectRecordLockGuard>>(), 40);
        assert_eq!(std::mem::size_of::<RecordLockSegment>(), 56);
        assert_eq!(std::mem::size_of::<Candidate>(), 16);
        assert_eq!(std::mem::size_of::<RecordObservation>(), 16);
        assert_eq!(std::mem::size_of::<DirectoryObservation>(), 8);
        assert_eq!(std::mem::size_of::<TableKey>(), 8);
        assert_eq!(std::mem::size_of::<ResolvedRecord>(), 24);
        assert_eq!(std::mem::size_of::<AdapterRole>(), 1);
        assert_eq!(std::mem::size_of::<Option<AdapterRole>>(), 1);
        assert_eq!(std::mem::size_of::<TableObservation>(), 16);
        assert_eq!(std::mem::size_of::<TableIntent>(), 40);
        assert_eq!(std::mem::size_of::<Option<TableIntent>>(), 40);
        assert_eq!(std::mem::size_of::<TablePrepared>(), 24);
        assert_eq!(std::mem::size_of::<RegisteredResource<TableAdapter>>(), 8);
        // The erased token retains runtime and plan identities while its
        // nonzero slot-plus-one encoding gives Option its empty-state niche.
        assert_eq!(std::mem::size_of::<Option<ErasedLockUse>>(), 24);
    }

    #[test]
    fn role_tagged_adapter_rejects_cross_role_keys_at_item_initialization() {
        fn assert_type_mismatch(
            result: Result<AdapterRole, sto_core::ItemInitError>,
            expected_phase: AdapterPhase,
        ) {
            match result {
                Err(sto_core::ItemInitError::Fault(fault)) => {
                    assert_eq!(fault.phase(), expected_phase);
                    assert_eq!(*fault.kind(), AdapterFaultKind::TypeMismatch);
                }
                Err(other) => panic!("expected type mismatch, got {other:?}"),
                Ok(_) => panic!("cross-role key unexpectedly initialized an item"),
            }
        }

        let (_runtime, table) = runtime_and_table(TableConfig::default());
        assert!(table
            .record_resource
            .adapter()
            .preflight_free_read_capability()
            .is_some());
        assert!(table
            .record_resource
            .adapter()
            .terminal_read_batch_capability()
            .is_some());
        assert!(table
            .directory_resource
            .adapter()
            .preflight_free_read_capability()
            .is_some());
        assert!(table
            .directory_resource
            .adapter()
            .terminal_read_batch_capability()
            .is_none());
        assert_ne!(
            table.record_resource.resource_class(),
            table.directory_resource.resource_class()
        );
        assert_type_mismatch(
            table
                .record_resource
                .adapter()
                .new_local(&TableKey::DirectoryGeneration),
            AdapterPhase::ItemInit,
        );
        assert_eq!(table.health(), TableHealth::Poisoned);

        let (_runtime, table) = runtime_and_table(TableConfig::default());
        assert_type_mismatch(
            table
                .directory_resource
                .adapter()
                .new_local(&TableKey::Record(RecordId::new(1).unwrap())),
            AdapterPhase::ItemInit,
        );
        assert_eq!(table.health(), TableHealth::Poisoned);
    }

    #[test]
    fn ordinary_reads_drop_shared_reload_snapshots_before_certification() {
        const KEY: [u8; 8] = *b"snapshot";
        const LARGE_VALUE: [u8; INLINE_VALUE_CAPACITY + 1] = [0x7b; INLINE_VALUE_CAPACITY + 1];
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        seed(&table, &mut worker, &[(&KEY, &LARGE_VALUE)]);

        let record_id = table.shared().lookup(None, &KEY).unwrap().unwrap();
        let record = table.shared().resolve_directory_record(record_id).unwrap();
        let pinned = match committed_record_snapshot(record) {
            RecordState::Live(Value {
                repr: ValueRepr::Shared(bytes),
            }) => bytes,
            _ => panic!("fixture value must use shared storage"),
        };
        let baseline = Arc::strong_count(&pinned);

        // The scalar lane clones the operation-local reload for its owned
        // result, then drops the reload before returning. The compact
        // transaction observation never pins shared bytes through commit.
        let mut scalar = worker.begin().unwrap();
        let returned = table.get_inner(&mut scalar, None, &KEY).unwrap().unwrap();
        assert!(matches!(&returned.repr, ValueRepr::Shared(_)));
        assert_eq!(Arc::strong_count(&pinned), baseline + 1);
        drop(returned);
        assert_eq!(Arc::strong_count(&pinned), baseline);
        committed(scalar.commit());
        assert_eq!(Arc::strong_count(&pinned), baseline);

        // The fixed visitor is the typed unique-batch lane used by the point
        // benchmark. Its reload pins the value only during the callback.
        let mut batch = PointReadBatch::with_capacity(1);
        let mut typed = worker.begin().unwrap();
        assert_eq!(
            table
                .visit_fixed_inner::<false, 8>(
                    &mut typed,
                    None,
                    &[KEY],
                    &mut batch,
                    |_index, current| {
                        assert_eq!(current.map(Value::as_ref), Some(&LARGE_VALUE[..]));
                        assert_eq!(Arc::strong_count(&pinned), baseline + 1);
                    },
                    |batch| {
                        batch.push_record_id(Some(record_id));
                        Ok(())
                    },
                )
                .unwrap(),
            1
        );
        assert_eq!(Arc::strong_count(&pinned), baseline);
        committed(typed.commit());
        assert_eq!(Arc::strong_count(&pinned), baseline);

        // Explicit abort likewise has no snapshot ownership to release.
        let mut aborted = worker.begin().unwrap();
        let returned = table.get_inner(&mut aborted, None, &KEY).unwrap().unwrap();
        drop(returned);
        assert_eq!(Arc::strong_count(&pinned), baseline);
        aborted.abort();
        assert_eq!(Arc::strong_count(&pinned), baseline);
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Healthy);
    }

    #[test]
    fn presence_only_writes_do_not_clone_shared_payloads_and_clear_embedded_slots() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let put_backing = SharedValue::from_slice(&[0x6a; INLINE_VALUE_CAPACITY + 1]);
        let remove_backing = SharedValue::from_slice(&[0x7b; INLINE_VALUE_CAPACITY + 2]);

        let mut setup = worker.begin().unwrap();
        for (key, backing) in [
            (&b"presence/put"[..], &put_backing),
            (&b"presence/remove"[..], &remove_backing),
        ] {
            table
                .put_inner(
                    &mut setup,
                    None,
                    key,
                    Value {
                        repr: ValueRepr::Shared(Arc::clone(backing)),
                    },
                )
                .unwrap();
        }
        committed(setup.commit());
        assert_eq!(Arc::strong_count(&put_backing), 2);
        assert_eq!(Arc::strong_count(&remove_backing), 2);

        let put_record_id = table
            .shared()
            .lookup(None, b"presence/put")
            .unwrap()
            .unwrap();
        let remove_record_id = table
            .shared()
            .lookup(None, b"presence/remove")
            .unwrap()
            .unwrap();
        let mut transaction = worker.begin().unwrap();
        assert!(table
            .put_presence_inner(
                &mut transaction,
                None,
                b"presence/put",
                Value::from(&b"inline"[..]),
            )
            .unwrap());
        assert!(table
            .remove_presence_inner(&mut transaction, None, b"presence/remove")
            .unwrap());

        // Neither the compact observation nor either staged replacement
        // retains the displaced shared payload.
        assert_eq!(Arc::strong_count(&put_backing), 2);
        assert_eq!(Arc::strong_count(&remove_backing), 2);
        committed(transaction.commit());

        // The retained old_was_shared bit is nevertheless sufficient for
        // installation to clear both embedded slots.
        let put_record = table
            .shared()
            .resolve_directory_record(put_record_id)
            .unwrap();
        let remove_record = table
            .shared()
            .resolve_directory_record(remove_record_id)
            .unwrap();
        assert!(put_record.state.shared.load_full().is_none());
        assert!(remove_record.state.shared.load_full().is_none());
        assert_eq!(Arc::strong_count(&put_backing), 1);
        assert_eq!(Arc::strong_count(&remove_backing), 1);
        assert_eq!(
            put_record.state.load_metadata(None),
            CommittedStateMetadataLoad::Complete(RecordStateMetadata {
                live: true,
                shared: false,
            })
        );
        assert_eq!(
            remove_record.state.load_metadata(None),
            CommittedStateMetadataLoad::Complete(RecordStateMetadata {
                live: false,
                shared: false,
            })
        );
    }

    #[test]
    #[cfg(target_pointer_width = "64")]
    fn eager_registry_entries_preserve_the_measured_slot_stride() {
        let registry = isolated_registry(
            TableConfig::new()
                .with_max_retained_records(1)
                .with_max_retained_key_bytes(1)
                .with_max_consumed_record_ids(2),
        );
        let segment = registry.ensure_segment(0).unwrap();
        let slots = segment.arena.standard_slots();
        assert_eq!(slots.len(), REGISTRY_SEGMENT_SLOTS);
        assert_eq!(slots.as_ptr().addr() % 64, 0);
        assert_eq!(
            std::mem::size_of::<RegistryEntry>(),
            REGISTRY_ENTRY_SLOT_BYTES
        );
        assert_eq!(
            (&slots[1] as *const RegistryEntry as usize)
                - (&slots[0] as *const RegistryEntry as usize),
            REGISTRY_ENTRY_SLOT_BYTES
        );
        assert!(slots
            .iter()
            .all(|entry| entry.state.load(Ordering::Acquire) == SLOT_UNALLOCATED));
    }

    #[test]
    #[cfg(target_pointer_width = "64")]
    fn bounded_registry_layout_is_extended_without_changing_the_standard_slot() {
        assert_eq!(std::mem::size_of::<RegistryEntry>(), 64);
        assert_eq!(std::mem::align_of::<RegistryEntry>(), 64);
        assert_eq!(std::mem::size_of::<StableAtomicValueCell>(), 128);
        assert_eq!(std::mem::size_of::<StableRegistryEntry>(), 192);
        assert_eq!(std::mem::align_of::<StableRegistryEntry>(), 64);
        assert_eq!(std::mem::offset_of!(StableRegistryEntry, base), 0);
        assert_eq!(std::mem::offset_of!(StableRegistryEntry, cell), 64);

        const SLOTS: usize = 33;
        let standard = eager_registry_accounted_bytes(SLOTS, false).unwrap();
        let stable = eager_registry_accounted_bytes(SLOTS, true).unwrap();
        assert_eq!(stable - standard, SLOTS * 128);

        let registry = isolated_registry(
            TableConfig::new()
                .with_max_retained_records(1)
                .with_max_retained_key_bytes(1)
                .with_max_consumed_record_ids(1)
                .with_bounded_atomic_values(true),
        );
        assert!(registry.ensure_segment(0).unwrap().arena.is_stable());
    }

    #[test]
    #[cfg(target_pointer_width = "64")]
    fn bounded_direct_tokens_name_concrete_extended_entries_at_their_real_stride() {
        let registry = isolated_registry(
            TableConfig::new()
                .with_max_retained_records(2)
                .with_max_retained_key_bytes(2)
                .with_max_consumed_record_ids(2)
                .with_bounded_atomic_values(true),
        );
        let (_first, first_token) = registry
            .reserve_candidate_with_mode(b"a", RecordTokenMode::DirectRecordPointer)
            .unwrap();
        let (_second, second_token) = registry
            .reserve_candidate_with_mode(b"b", RecordTokenMode::DirectRecordPointer)
            .unwrap();
        assert_eq!(
            second_token.get().checked_sub(first_token.get()).unwrap(),
            std::mem::size_of::<StableRegistryEntry>() as u64
        );

        let first = registry.resolve_direct_access(first_token).unwrap();
        let second = registry.resolve_direct_access(second_token).unwrap();
        assert!(first.stable.is_some());
        assert!(second.stable.is_some());
        assert_eq!(first.element_address as u64, first_token.get());
        assert_eq!(second.element_address as u64, second_token.get());
        assert_eq!(direct_record::encode(first).unwrap(), first_token);

        let RegistryStorage::LazySegmented(storage) = &registry.storage else {
            panic!("the test uses the default segmented registry");
        };
        let segment = storage.segments[0].get().unwrap();
        let RegistryArenaStorage::Stable160(slots) = segment.arena.storage.as_ref() else {
            panic!("bounded tables must allocate extended entries");
        };
        assert!(std::ptr::eq(first.entry, &slots[0].base));
        assert!(std::ptr::eq(first.stable.unwrap(), &slots[0].cell));
        assert!(std::ptr::eq(second.entry, &slots[1].base));
        assert!(std::ptr::eq(second.stable.unwrap(), &slots[1].cell));
    }

    #[test]
    #[cfg(target_pointer_width = "64")]
    fn contiguous_registry_allocates_one_exact_stable_arena_and_all_lock_targets() {
        const SLOTS: usize = 33;
        let required_bytes = eager_registry_accounted_bytes(SLOTS, false).unwrap();
        let registry = isolated_registry(eager_registry_config(SLOTS));
        let RegistryStorage::EagerContiguous(storage) = &registry.storage else {
            panic!("explicit eager configuration must not fall back to segmented storage");
        };

        let slots = storage.arena.standard_slots();
        assert_eq!(slots.len(), SLOTS);
        assert_eq!(slots.as_ptr().addr() % 64, 0);
        assert_eq!(
            std::mem::size_of::<RegistryEntry>(),
            REGISTRY_ENTRY_SLOT_BYTES
        );
        assert_eq!(
            (&slots[1] as *const RegistryEntry as usize)
                - (&slots[0] as *const RegistryEntry as usize),
            REGISTRY_ENTRY_SLOT_BYTES
        );
        assert_eq!(
            storage.lock_segments.len(),
            record_lock_segment_count(SLOTS).unwrap()
        );
        assert_eq!(
            eager_registry_accounted_bytes(slots.len(), false).unwrap(),
            required_bytes
        );
        assert_eq!(
            eager_registry_accounted_bytes(100_000, false).unwrap(),
            6_900_040
        );
        assert!(eager_registry_accounted_bytes(100_000, false).unwrap() <= 8 * 1024 * 1024);
        assert!(slots
            .iter()
            .all(|entry| entry.state.load(Ordering::Acquire) == SLOT_UNALLOCATED));
        for (index, target) in storage.lock_segments.iter().enumerate() {
            assert!(storage.arena.shares_allocation_with(&target.arena));
            assert_eq!(target.logical_base, index * RECORD_LOCK_SEGMENT_SLOTS);
            assert_eq!(target.physical_base, index * RECORD_LOCK_SEGMENT_SLOTS);
        }
    }

    #[test]
    fn contiguous_registry_resolves_direct_slots_and_preserves_terminal_states() {
        let registry = isolated_registry(eager_registry_config(3));
        let first_id = RecordId::new(1).unwrap();
        assert!(matches!(
            registry.resolve(first_id),
            Err(AccessError::Fault(_))
        ));

        let mut published = registry.reserve_candidate(b"a").unwrap();
        let ready = registry.resolve(published.id).unwrap();
        let RegistryStorage::EagerContiguous(storage) = &registry.storage else {
            panic!("explicit eager configuration must remain contiguous");
        };
        assert!(std::ptr::eq(
            ready,
            &storage.arena.standard_slots()[0].record
        ));
        let (with_lock, lock_segment) = registry.resolve_with_segment(published.id).unwrap();
        assert!(std::ptr::eq(ready, with_lock));
        assert!(Arc::ptr_eq(lock_segment, &storage.lock_segments[0]));
        registry.mark_published(&mut published).unwrap();
        assert!(std::ptr::eq(ready, registry.resolve(published.id).unwrap()));

        let unpublished = registry.reserve_candidate(b"b").unwrap();
        registry.prove_unpublished(&unpublished).unwrap();
        assert!(matches!(
            registry.resolve(unpublished.id),
            Err(AccessError::Fault(_))
        ));
        assert_eq!(
            registry
                .entry(unpublished.id)
                .unwrap()
                .state
                .load(Ordering::Acquire),
            SLOT_PROVEN_UNPUBLISHED
        );

        let unknown = registry.reserve_candidate(b"c").unwrap();
        registry.mark_unknown(&unknown).unwrap();
        assert!(matches!(
            registry.resolve(unknown.id),
            Err(AccessError::Fault(_))
        ));
        assert_eq!(
            registry
                .entry(unknown.id)
                .unwrap()
                .state
                .load(Ordering::Acquire),
            SLOT_PUBLICATION_UNKNOWN
        );
        assert_eq!(registry.usage().consumed_record_ids(), 3);
    }

    #[test]
    fn contiguous_registry_lock_targets_commit_record_writes() {
        let (runtime, table) = runtime_and_table(eager_registry_config(4));
        let mut worker = runtime.attach().unwrap();
        seed(&table, &mut worker, &[(b"a", b"old-a"), (b"b", b"old-b")]);

        let mut write = worker.begin().unwrap();
        assert_eq!(
            table
                .put_inner(&mut write, None, b"a", Value::from(&b"new-a"[..]))
                .unwrap()
                .as_ref()
                .map(Value::as_ref),
            Some(&b"old-a"[..])
        );
        assert_eq!(
            table
                .put_inner(&mut write, None, b"b", Value::from(&b"new-b"[..]))
                .unwrap()
                .as_ref()
                .map(Value::as_ref),
            Some(&b"old-b"[..])
        );
        committed(write.commit());

        let mut verify = worker.begin().unwrap();
        assert_eq!(
            table
                .get_inner(&mut verify, None, b"a")
                .unwrap()
                .as_ref()
                .map(Value::as_ref),
            Some(&b"new-a"[..])
        );
        assert_eq!(
            table
                .get_inner(&mut verify, None, b"b")
                .unwrap()
                .as_ref()
                .map(Value::as_ref),
            Some(&b"new-b"[..])
        );
        committed(verify.commit());
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Healthy);
        assert_eq!(table.health(), TableHealth::Healthy);
    }

    #[test]
    fn contiguous_registry_capacity_and_budget_fail_without_layout_fallback() {
        const SLOTS: usize = 17;
        let required_bytes = eager_registry_accounted_bytes(SLOTS, false).unwrap();
        let insufficient = TableConfig::new()
            .with_max_consumed_record_ids(SLOTS as u64)
            .with_registry_layout(RegistryLayout::EagerContiguous {
                max_bytes: required_bytes - 1,
            });
        assert!(matches!(
            try_isolated_registry(insufficient),
            Err(RegistrationError::Capacity(CapacityError::BufferLimit))
        ));

        let config = TableConfig::new()
            .with_max_retained_records((SLOTS + 1) as u64)
            .with_max_retained_key_bytes((SLOTS + 1) as u64)
            .with_max_consumed_record_ids(SLOTS as u64)
            .with_registry_layout(RegistryLayout::EagerContiguous {
                max_bytes: required_bytes,
            });
        let registry = isolated_registry(config);
        let mut candidates = Vec::new();
        for _ in 0..SLOTS {
            candidates.push(registry.reserve_candidate(b"x").unwrap());
        }
        assert!(matches!(
            registry.reserve_candidate(b"x"),
            Err(AccessError::Capacity(CapacityError::BufferLimit))
        ));
        assert_eq!(registry.usage().consumed_record_ids(), SLOTS as u64);

        let last = candidates.last().unwrap();
        let (_, last_target) = registry.resolve_with_segment(last.id).unwrap();
        let RegistryStorage::EagerContiguous(storage) = &registry.storage else {
            panic!("explicit eager configuration must remain contiguous");
        };
        assert!(Arc::ptr_eq(last_target, &storage.lock_segments[1]));
        let identity = LockIdentity::new(
            last_target.lock_domain.runtime_id,
            last_target.lock_domain.namespace,
            last_target.lock_domain.lock_class,
            last.id.get(),
        );
        assert_eq!(
            last_target
                .identity_record_slot(&identity, AdapterPhase::Acquire)
                .unwrap(),
            (last.id, SLOTS - 1)
        );
        assert!(storage.lock_segments[0]
            .identity_record_slot(&identity, AdapterPhase::Acquire)
            .is_err());

        let zero = TableConfig::new()
            .with_max_retained_records(1)
            .with_max_retained_key_bytes(1)
            .with_max_consumed_record_ids(0)
            .with_registry_layout(RegistryLayout::EagerContiguous { max_bytes: 0 });
        let zero = isolated_registry(zero);
        let RegistryStorage::EagerContiguous(storage) = &zero.storage else {
            panic!("zero-capacity eager storage must not fall back");
        };
        assert!(storage.arena.standard_slots().is_empty());
        assert!(storage.lock_segments.is_empty());
        assert!(matches!(
            zero.reserve_candidate(b"x"),
            Err(AccessError::Capacity(CapacityError::BufferLimit))
        ));
        assert_eq!(zero.usage().consumed_record_ids(), 0);

        let huge = TableConfig::new()
            .with_max_consumed_record_ids(u64::MAX)
            .with_registry_layout(RegistryLayout::EagerContiguous {
                max_bytes: usize::MAX,
            });
        assert!(matches!(
            try_isolated_registry(huge),
            Err(RegistrationError::Capacity(CapacityError::BufferLimit))
        ));
    }

    #[test]
    fn concurrent_contiguous_reservations_claim_unique_direct_slots() {
        const THREADS: usize = 16;
        let registry = Arc::new(isolated_registry(eager_registry_config(THREADS)));
        let barrier = Arc::new(Barrier::new(THREADS));
        let mut handles = Vec::with_capacity(THREADS);
        for index in 0..THREADS {
            let registry = Arc::clone(&registry);
            let barrier = Arc::clone(&barrier);
            handles.push(std::thread::spawn(move || {
                let key = format!("eager-{index}");
                barrier.wait();
                registry.reserve_candidate(key.as_bytes()).unwrap()
            }));
        }

        let mut candidates = handles
            .into_iter()
            .map(|handle| handle.join().unwrap())
            .collect::<Vec<_>>();
        candidates.sort_unstable_by_key(|candidate| candidate.id.get());
        let RegistryStorage::EagerContiguous(storage) = &registry.storage else {
            panic!("explicit eager configuration must remain contiguous");
        };
        for (index, candidate) in candidates.iter().enumerate() {
            assert_eq!(candidate.id.get(), (index + 1) as u64);
            assert_eq!(
                storage.arena.standard_slots()[index]
                    .state
                    .load(Ordering::Acquire),
                SLOT_READY
            );
            assert!(std::ptr::eq(
                registry.resolve(candidate.id).unwrap(),
                &storage.arena.standard_slots()[index].record
            ));
        }
    }

    #[test]
    fn fixed_candidate_batch_reserves_one_ready_range_and_releases_exact_quota() {
        let registry = isolated_registry(
            TableConfig::new()
                .with_max_retained_records(16)
                .with_max_retained_key_bytes(128)
                .with_max_consumed_record_ids(16),
        );
        let mut candidates = Vec::with_capacity(4);
        let mut tokens = Vec::with_capacity(4);
        assert_eq!(
            registry
                .reserve_candidate_batch_with_mode(
                    4,
                    7,
                    RecordTokenMode::RegistryId,
                    &mut candidates,
                    &mut tokens,
                )
                .unwrap(),
            CandidateBatchReservation::Reserved
        );
        assert_eq!(
            candidates
                .iter()
                .map(|candidate| candidate.id.get())
                .collect::<Vec<_>>(),
            vec![1, 2, 3, 4]
        );
        assert_eq!(
            tokens.iter().map(|token| token.get()).collect::<Vec<_>>(),
            vec![1, 2, 3, 4]
        );
        assert_eq!(
            registry.usage(),
            TableUsage {
                retained_records: 4,
                retained_key_bytes: 28,
                consumed_record_ids: 4,
            }
        );
        for candidate in &candidates {
            assert_eq!(
                registry
                    .entry(candidate.id)
                    .unwrap()
                    .state
                    .load(Ordering::Acquire),
                SLOT_READY
            );
        }

        registry.mark_published(&mut candidates[0]).unwrap();
        registry.mark_published(&mut candidates[1]).unwrap();
        registry.prove_unpublished(&candidates[2]).unwrap();
        registry.prove_unpublished(&candidates[3]).unwrap();
        assert_eq!(
            registry.usage(),
            TableUsage {
                retained_records: 2,
                retained_key_bytes: 14,
                consumed_record_ids: 4,
            }
        );
    }

    #[test]
    fn fixed_candidate_batch_mints_direct_tokens_for_the_exact_registry_entries() {
        let registry = isolated_registry(
            TableConfig::new()
                .with_max_retained_records(4)
                .with_max_retained_key_bytes(32)
                .with_max_consumed_record_ids(4),
        );
        let mut candidates = Vec::with_capacity(3);
        let mut tokens = Vec::with_capacity(3);
        assert_eq!(
            registry
                .reserve_candidate_batch_with_mode(
                    3,
                    8,
                    RecordTokenMode::DirectRecordPointer,
                    &mut candidates,
                    &mut tokens,
                )
                .unwrap(),
            CandidateBatchReservation::Reserved
        );
        for (candidate, token) in candidates.iter_mut().zip(&tokens) {
            assert!(std::ptr::eq(
                registry.resolve(candidate.id).unwrap(),
                registry.resolve_direct(*token).unwrap()
            ));
            registry.mark_published(candidate).unwrap();
        }
    }

    #[test]
    fn fixed_candidate_batch_quota_misses_are_side_effect_free_before_scalar_retry() {
        let configs = [
            TableConfig::new()
                .with_max_retained_records(2)
                .with_max_retained_key_bytes(100)
                .with_max_consumed_record_ids(100),
            TableConfig::new()
                .with_max_retained_records(100)
                .with_max_retained_key_bytes(5)
                .with_max_consumed_record_ids(100),
            TableConfig::new()
                .with_max_retained_records(100)
                .with_max_retained_key_bytes(100)
                .with_max_consumed_record_ids(2),
        ];
        for config in configs {
            let registry = isolated_registry(config);
            let mut candidates = Vec::with_capacity(3);
            let mut tokens = Vec::with_capacity(3);
            assert_eq!(
                registry
                    .reserve_candidate_batch_with_mode(
                        3,
                        2,
                        RecordTokenMode::RegistryId,
                        &mut candidates,
                        &mut tokens,
                    )
                    .unwrap(),
                CandidateBatchReservation::RetryScalar
            );
            assert!(candidates.is_empty());
            assert!(tokens.is_empty());
            assert_eq!(
                registry.usage(),
                TableUsage {
                    retained_records: 0,
                    retained_key_bytes: 0,
                    consumed_record_ids: 0,
                }
            );
            let RegistryStorage::LazySegmented(storage) = &registry.storage else {
                panic!("the default registry layout must remain lazy segmented");
            };
            assert!(storage
                .segments
                .iter()
                .all(|segment| segment.get().is_none()));
        }

        // The caller's scalar replay retains its established prefix behavior.
        let registry = isolated_registry(
            TableConfig::new()
                .with_max_retained_records(2)
                .with_max_retained_key_bytes(100)
                .with_max_consumed_record_ids(100),
        );
        let first = registry.reserve_candidate(b"aa").unwrap();
        let second = registry.reserve_candidate(b"bb").unwrap();
        assert!(matches!(
            registry.reserve_candidate(b"cc"),
            Err(AccessError::Capacity(CapacityError::BufferLimit))
        ));
        assert_eq!(first.id.get(), 1);
        assert_eq!(second.id.get(), 2);
        assert_eq!(registry.usage().consumed_record_ids(), 2);
    }

    #[test]
    fn fixed_candidate_batch_crosses_lazy_segment_boundaries_without_gaps() {
        let maximum = REGISTRY_SEGMENT_SLOTS + 4;
        let registry = isolated_registry(
            TableConfig::new()
                .with_max_retained_records(maximum as u64)
                .with_max_retained_key_bytes(maximum as u64)
                .with_max_consumed_record_ids(maximum as u64),
        );
        for _ in 0..(REGISTRY_SEGMENT_SLOTS - 2) {
            let mut candidate = registry.reserve_candidate(b"x").unwrap();
            registry.mark_published(&mut candidate).unwrap();
        }

        let mut candidates = Vec::with_capacity(4);
        let mut tokens = Vec::with_capacity(4);
        assert_eq!(
            registry
                .reserve_candidate_batch_with_mode(
                    4,
                    1,
                    RecordTokenMode::RegistryId,
                    &mut candidates,
                    &mut tokens,
                )
                .unwrap(),
            CandidateBatchReservation::Reserved
        );
        assert_eq!(
            candidates
                .iter()
                .map(|candidate| candidate.id.get())
                .collect::<Vec<_>>(),
            vec![
                (REGISTRY_SEGMENT_SLOTS - 1) as u64,
                REGISTRY_SEGMENT_SLOTS as u64,
                (REGISTRY_SEGMENT_SLOTS + 1) as u64,
                (REGISTRY_SEGMENT_SLOTS + 2) as u64,
            ]
        );
        let RegistryStorage::LazySegmented(storage) = &registry.storage else {
            panic!("the default registry layout must remain lazy segmented");
        };
        assert!(storage.segments[0].get().is_some());
        assert!(storage.segments[1].get().is_some());
        assert_eq!(registry.usage().consumed_record_ids(), maximum as u64 - 2);
    }

    #[test]
    fn concurrent_fixed_candidate_batches_claim_disjoint_contiguous_ranges() {
        const THREADS: usize = 8;
        const BATCH: usize = 7;
        const TOTAL: usize = THREADS * BATCH;
        let registry = Arc::new(isolated_registry(
            TableConfig::new()
                .with_max_retained_records(TOTAL as u64)
                .with_max_retained_key_bytes((TOTAL * 4) as u64)
                .with_max_consumed_record_ids(TOTAL as u64),
        ));
        let barrier = Arc::new(Barrier::new(THREADS));
        let mut handles = Vec::with_capacity(THREADS);
        for _ in 0..THREADS {
            let registry = Arc::clone(&registry);
            let barrier = Arc::clone(&barrier);
            handles.push(std::thread::spawn(move || {
                let mut candidates = Vec::with_capacity(BATCH);
                let mut tokens = Vec::with_capacity(BATCH);
                barrier.wait();
                let outcome = registry
                    .reserve_candidate_batch_with_mode(
                        BATCH,
                        4,
                        RecordTokenMode::RegistryId,
                        &mut candidates,
                        &mut tokens,
                    )
                    .unwrap();
                assert_eq!(outcome, CandidateBatchReservation::Reserved);
                candidates
            }));
        }

        let mut ids = handles
            .into_iter()
            .flat_map(|handle| handle.join().unwrap())
            .map(|candidate| candidate.id.get())
            .collect::<Vec<_>>();
        ids.sort_unstable();
        assert_eq!(ids, (1..=TOTAL as u64).collect::<Vec<_>>());
        assert_eq!(
            registry.usage(),
            TableUsage {
                retained_records: TOTAL as u64,
                retained_key_bytes: (TOTAL * 4) as u64,
                consumed_record_ids: TOTAL as u64,
            }
        );
    }

    #[test]
    fn concurrent_fixed_candidate_batches_never_overcommit_a_partial_quota() {
        const THREADS: usize = 12;
        const BATCH: usize = 8;
        const LIMIT: usize = 31;
        let registry = Arc::new(isolated_registry(
            TableConfig::new()
                .with_max_retained_records(LIMIT as u64)
                .with_max_retained_key_bytes((LIMIT * 4) as u64)
                .with_max_consumed_record_ids((THREADS * BATCH) as u64),
        ));
        let barrier = Arc::new(Barrier::new(THREADS));
        let mut handles = Vec::with_capacity(THREADS);
        for _ in 0..THREADS {
            let registry = Arc::clone(&registry);
            let barrier = Arc::clone(&barrier);
            handles.push(std::thread::spawn(move || {
                let mut candidates = Vec::with_capacity(BATCH);
                let mut tokens = Vec::with_capacity(BATCH);
                barrier.wait();
                let outcome = registry
                    .reserve_candidate_batch_with_mode(
                        BATCH,
                        4,
                        RecordTokenMode::RegistryId,
                        &mut candidates,
                        &mut tokens,
                    )
                    .unwrap();
                (outcome, candidates.len(), tokens.len())
            }));
        }

        let outcomes = handles
            .into_iter()
            .map(|handle| handle.join().unwrap())
            .collect::<Vec<_>>();
        let successful = outcomes
            .iter()
            .filter(|(outcome, candidates, tokens)| {
                assert_eq!(candidates, tokens);
                match outcome {
                    CandidateBatchReservation::Reserved => {
                        assert_eq!(*candidates, BATCH);
                        true
                    }
                    CandidateBatchReservation::RetryScalar => {
                        assert_eq!(*candidates, 0);
                        false
                    }
                }
            })
            .count();
        assert_eq!(successful, LIMIT / BATCH);
        let retained = (successful * BATCH) as u64;
        assert_eq!(
            registry.usage(),
            TableUsage {
                retained_records: retained,
                retained_key_bytes: retained * 4,
                consumed_record_ids: retained,
            }
        );
    }

    #[test]
    fn fixed_candidate_batch_detects_a_reused_slot_before_consuming_the_range() {
        let registry = isolated_registry(
            TableConfig::new()
                .with_max_retained_records(4)
                .with_max_retained_key_bytes(16)
                .with_max_consumed_record_ids(4),
        );
        let segment = registry.ensure_segment(0).unwrap();
        let slots = segment.arena.standard_slots();
        slots[0].state.store(SLOT_READY, Ordering::Release);
        let mut candidates = Vec::with_capacity(3);
        let mut tokens = Vec::with_capacity(3);
        assert!(matches!(
            registry.reserve_candidate_batch_with_mode(
                3,
                4,
                RecordTokenMode::RegistryId,
                &mut candidates,
                &mut tokens,
            ),
            Err(AccessError::Fault(_))
        ));
        assert!(candidates.is_empty());
        assert!(tokens.is_empty());
        assert_eq!(
            registry.usage(),
            TableUsage {
                retained_records: 0,
                retained_key_bytes: 0,
                consumed_record_ids: 0,
            }
        );
        assert!(slots[1..3]
            .iter()
            .all(|entry| entry.state.load(Ordering::Acquire) == SLOT_UNALLOCATED));
    }

    #[test]
    fn scalar_candidate_rejects_preclaimed_slot_without_overwrite_or_quota_leak() {
        let registry = isolated_registry(
            TableConfig::new()
                .with_max_retained_records(2)
                .with_max_retained_key_bytes(8)
                .with_max_consumed_record_ids(2),
        );
        let segment = registry.ensure_segment(0).unwrap();
        let slots = segment.arena.standard_slots();
        slots[0].state.store(SLOT_PUBLISHED, Ordering::Release);

        assert!(matches!(
            registry.reserve_candidate(b"used"),
            Err(AccessError::Fault(_))
        ));
        assert_eq!(slots[0].state.load(Ordering::Acquire), SLOT_PUBLISHED);
        assert_eq!(
            registry.usage(),
            TableUsage {
                retained_records: 0,
                retained_key_bytes: 0,
                consumed_record_ids: 1,
            }
        );

        let second = registry.reserve_candidate(b"b").unwrap();
        assert_eq!(second.id.get(), 2);
        assert_eq!(slots[1].state.load(Ordering::Acquire), SLOT_READY);
        assert_eq!(
            registry.usage(),
            TableUsage {
                retained_records: 1,
                retained_key_bytes: 1,
                consumed_record_ids: 2,
            }
        );
    }

    #[test]
    fn mixed_scalar_and_batch_reservations_claim_disjoint_ready_slots() {
        const SCALAR_THREADS: usize = 4;
        const BATCH_THREADS: usize = 4;
        const BATCH_SIZE: usize = 3;
        const THREADS: usize = SCALAR_THREADS + BATCH_THREADS;
        const TOTAL: usize = SCALAR_THREADS + BATCH_THREADS * BATCH_SIZE;

        let registry = Arc::new(isolated_registry(
            TableConfig::new()
                .with_max_retained_records(TOTAL as u64)
                .with_max_retained_key_bytes(TOTAL as u64)
                .with_max_consumed_record_ids(TOTAL as u64),
        ));
        let barrier = Arc::new(Barrier::new(THREADS));
        let mut handles = Vec::with_capacity(THREADS);
        for index in 0..THREADS {
            let registry = Arc::clone(&registry);
            let barrier = Arc::clone(&barrier);
            handles.push(std::thread::spawn(move || {
                barrier.wait();
                if index < SCALAR_THREADS {
                    vec![registry.reserve_candidate(b"s").unwrap()]
                } else {
                    let mut candidates = Vec::with_capacity(BATCH_SIZE);
                    let mut tokens = Vec::with_capacity(BATCH_SIZE);
                    assert_eq!(
                        registry
                            .reserve_candidate_batch_with_mode(
                                BATCH_SIZE,
                                1,
                                RecordTokenMode::RegistryId,
                                &mut candidates,
                                &mut tokens,
                            )
                            .unwrap(),
                        CandidateBatchReservation::Reserved
                    );
                    candidates
                }
            }));
        }

        let mut candidates = handles
            .into_iter()
            .flat_map(|handle| handle.join().unwrap())
            .collect::<Vec<_>>();
        candidates.sort_unstable_by_key(|candidate| candidate.id.get());
        assert_eq!(
            candidates
                .iter()
                .map(|candidate| candidate.id.get())
                .collect::<Vec<_>>(),
            (1..=TOTAL as u64).collect::<Vec<_>>()
        );
        assert!(candidates.iter().all(|candidate| {
            registry
                .entry(candidate.id)
                .is_ok_and(|entry| entry.state.load(Ordering::Acquire) == SLOT_READY)
        }));
        assert_eq!(
            registry.usage(),
            TableUsage {
                retained_records: TOTAL as u64,
                retained_key_bytes: TOTAL as u64,
                consumed_record_ids: TOTAL as u64,
            }
        );
    }

    #[test]
    fn unallocated_entries_reject_resolution_and_only_exact_transitions_publish() {
        let registry = isolated_registry(
            TableConfig::new()
                .with_max_retained_records(1)
                .with_max_retained_key_bytes(8)
                .with_max_consumed_record_ids(2),
        );
        let unallocated_id = RecordId::new(1).unwrap();
        let segment = registry.ensure_segment(0).unwrap();
        let slots = segment.arena.standard_slots();
        let entry = &slots[0];

        assert_eq!(entry.state.load(Ordering::Acquire), SLOT_UNALLOCATED);
        assert!(matches!(
            registry.resolve(unallocated_id),
            Err(AccessError::Fault(_))
        ));
        assert!(matches!(
            transition_slot(entry, SLOT_RESERVED, SLOT_READY),
            Err(AccessError::Fault(_))
        ));
        assert_eq!(entry.state.load(Ordering::Acquire), SLOT_UNALLOCATED);

        let mut candidate = registry.reserve_candidate(b"record").unwrap();
        assert_eq!(candidate.id, unallocated_id);
        assert_eq!(entry.state.load(Ordering::Acquire), SLOT_READY);
        assert!(std::ptr::eq(
            registry.resolve(candidate.id).unwrap(),
            &entry.record
        ));
        assert!(matches!(
            transition_slot(entry, SLOT_UNALLOCATED, SLOT_RESERVED),
            Err(AccessError::Fault(_))
        ));
        assert_eq!(entry.state.load(Ordering::Acquire), SLOT_READY);

        registry.mark_published(&mut candidate).unwrap();
        assert_eq!(entry.state.load(Ordering::Acquire), SLOT_PUBLISHED);
        assert!(matches!(
            registry.prove_unpublished(&candidate),
            Err(AccessError::Fault(_))
        ));
        assert_eq!(entry.state.load(Ordering::Acquire), SLOT_PUBLISHED);

        let untouched_id = RecordId::new(2).unwrap();
        assert!(matches!(
            registry.resolve(untouched_id),
            Err(AccessError::Fault(_))
        ));
        assert_eq!(slots[1].state.load(Ordering::Acquire), SLOT_UNALLOCATED);
    }

    #[test]
    fn second_published_classification_is_rejected_without_state_change() {
        let registry = isolated_registry(
            TableConfig::new()
                .with_max_retained_records(1)
                .with_max_retained_key_bytes(8)
                .with_max_consumed_record_ids(1),
        );
        let mut candidate = registry.reserve_candidate(b"record").unwrap();
        registry.mark_published(&mut candidate).unwrap();
        let usage = registry.usage();

        assert!(matches!(
            registry.mark_published(&mut candidate),
            Err(AccessError::Fault(_))
        ));
        assert_eq!(
            registry
                .entry(candidate.id)
                .unwrap()
                .state
                .load(Ordering::Acquire),
            SLOT_PUBLISHED
        );
        assert_eq!(registry.usage(), usage);
    }

    #[test]
    fn concurrent_reservations_claim_distinct_eager_slots() {
        const THREADS: usize = 16;
        let registry = Arc::new(isolated_registry(
            TableConfig::new()
                .with_max_retained_records(THREADS as u64)
                .with_max_retained_key_bytes(1_024)
                .with_max_consumed_record_ids((THREADS + 1) as u64),
        ));
        let barrier = Arc::new(Barrier::new(THREADS));
        let mut handles = Vec::with_capacity(THREADS);
        for index in 0..THREADS {
            let registry = Arc::clone(&registry);
            let barrier = Arc::clone(&barrier);
            handles.push(std::thread::spawn(move || {
                let key = format!("record-{index}");
                barrier.wait();
                registry.reserve_candidate(key.as_bytes()).unwrap()
            }));
        }

        let mut candidates = handles
            .into_iter()
            .map(|handle| handle.join().unwrap())
            .collect::<Vec<_>>();
        candidates.sort_unstable_by_key(|candidate| candidate.id.get());
        for (index, candidate) in candidates.iter().enumerate() {
            assert_eq!(candidate.id.get(), (index + 1) as u64);
            let entry = registry.entry(candidate.id).unwrap();
            assert_eq!(entry.state.load(Ordering::Acquire), SLOT_READY);
            assert!(std::ptr::eq(
                registry.resolve(candidate.id).unwrap(),
                &entry.record
            ));
        }

        let RegistryStorage::LazySegmented(storage) = &registry.storage else {
            panic!("the default registry layout must remain lazy segmented");
        };
        let segment = storage.segments[0].get().unwrap();
        let slots = segment.arena.standard_slots();
        assert!(slots[..THREADS]
            .iter()
            .all(|entry| entry.state.load(Ordering::Acquire) == SLOT_READY));
        assert_eq!(
            slots[THREADS].state.load(Ordering::Acquire),
            SLOT_UNALLOCATED
        );
    }

    #[test]
    fn values_preserve_binary_bytes_across_the_inline_boundary() {
        for length in [0, 1, 8, 32, 33, 37, INLINE_VALUE_CAPACITY] {
            let bytes: Vec<_> = (0..length).map(|index| index as u8).collect();
            let value = Value::from(bytes.as_slice());
            assert!(matches!(&value.repr, ValueRepr::Inline { .. }));
            assert_eq!(value.as_ref(), bytes);
            assert_eq!(value.clone(), value);
        }

        let bytes = (0..=INLINE_VALUE_CAPACITY)
            .map(|index| (index as u8).wrapping_mul(17))
            .collect::<Vec<_>>();
        let value = Value::from(bytes.clone());
        let ValueRepr::Shared(shared) = &value.repr else {
            panic!("a value beyond the inline boundary must use shared storage");
        };
        assert!(matches!(shared.as_ref(), SharedValue::Medium { .. }));
        assert_eq!(shared.as_bytes(), bytes);
        let cloned = value.clone();
        let ValueRepr::Shared(cloned_shared) = &cloned.repr else {
            panic!("cloning a shared value must preserve shared storage");
        };
        assert!(Arc::ptr_eq(shared, cloned_shared));
        assert_eq!(cloned, value);

        let arc_slice: Arc<[u8]> = Arc::from(bytes.clone());
        let arc_value = Value::from(Arc::clone(&arc_slice));
        let ValueRepr::Shared(value_slice) = &arc_value.repr else {
            panic!("a large Arc slice must preserve shared storage");
        };
        assert_eq!(value_slice.as_bytes(), arc_slice.as_ref());
        assert_eq!(arc_value.as_ref(), bytes);

        let boxed_value = Value::from(bytes.clone().into_boxed_slice());
        assert_eq!(boxed_value.as_ref(), bytes);
        assert!(matches!(&boxed_value.repr, ValueRepr::Shared(_)));

        let binary = Value::from(&b"\0a\0\xff"[..]);
        assert_eq!(binary.as_ref(), b"\0a\0\xff");
    }

    #[test]
    fn bounded_tables_use_private_exact_staging_only_inside_the_cell_range() {
        let inline = Value::from_staging_slice(&[0x11; INLINE_VALUE_CAPACITY], true);
        assert!(matches!(inline.repr, ValueRepr::Inline { .. }));

        for length in [INLINE_VALUE_CAPACITY + 1, STABLE_ATOMIC_VALUE_CAPACITY] {
            let bytes = vec![length as u8; length];
            let staged = Value::from_staging_slice(&bytes, true);
            let cloned = staged.clone();
            let (ValueRepr::Staged(staged_bytes), ValueRepr::Staged(cloned_bytes)) =
                (&staged.repr, &cloned.repr)
            else {
                panic!("an opted-in bounded write must use private staging");
            };
            assert_eq!(staged_bytes.len(), length);
            assert_eq!(staged_bytes.as_ref(), bytes);
            assert_ne!(staged_bytes.as_ptr(), cloned_bytes.as_ptr());
            assert_eq!(cloned_bytes.as_ref(), bytes);
        }

        assert!(matches!(
            Value::from_staging_slice(&[0x22; INLINE_VALUE_CAPACITY + 1], false).repr,
            ValueRepr::Shared(_)
        ));
        assert!(matches!(
            Value::from_staging_slice(&[0x33; STABLE_ATOMIC_VALUE_CAPACITY + 1], true).repr,
            ValueRepr::Shared(_)
        ));
    }

    #[test]
    fn borrowed_staging_is_transaction_local_and_clones_into_owned_storage() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let table = Table::new_memory_direct(&runtime, bounded_value_config());

        let inline_source = vec![0x31; INLINE_VALUE_CAPACITY];
        assert!(matches!(
            Value::from(table.borrowed_staging_input(&inline_source)).repr,
            ValueRepr::Inline { .. }
        ));
        for length in [INLINE_VALUE_CAPACITY + 1, STABLE_ATOMIC_VALUE_CAPACITY] {
            let boundary_source = vec![length as u8; length];
            let borrowed = Value::from(table.borrowed_staging_input(&boundary_source));
            assert!(matches!(
                &borrowed.repr,
                ValueRepr::BorrowedStaged(bytes)
                    if bytes.as_ptr() == boundary_source.as_ptr() && bytes.len() == length
            ));
        }
        let oversized = vec![0x91; STABLE_ATOMIC_VALUE_CAPACITY + 1];
        let fallback = Value::from(table.borrowed_staging_input(&oversized));
        assert!(matches!(fallback.repr, ValueRepr::Shared(_)));
        assert_eq!(fallback.as_ref(), oversized);

        let key = b"bounded/borrowed-staging";
        let mut source = (0..159)
            .map(|index| (index as u8).wrapping_mul(19).wrapping_add(7))
            .collect::<Vec<_>>();
        let expected = source.clone();
        let mut write = worker.begin().unwrap();
        assert!(!table
            .put_presence_inner(&mut write, None, key, table.borrowed_staging_input(&source),)
            .unwrap());
        let record_id = table.shared().lookup(None, key).unwrap().unwrap();
        write
            .with_item(
                &table.record_resource,
                TableKey::Record(record_id),
                |entry| {
                    let Some(TableIntent::Record {
                        replacement: RecordState::Live(staged),
                        ..
                    }) = entry.intent()
                    else {
                        panic!("borrowed put must retain one live record intent");
                    };
                    assert!(matches!(
                        staged.repr,
                        ValueRepr::BorrowedStaged(bytes)
                            if bytes.as_ptr() == source.as_ptr() && bytes.len() == 159
                    ));
                    let cloned = staged.clone();
                    assert_eq!(cloned.as_ref(), expected);
                    assert!(matches!(cloned.repr, ValueRepr::Shared(_)));
                    Ok(())
                },
            )
            .unwrap();
        committed(write.commit());

        source.fill(0xee);
        let mut verify = worker.begin().unwrap();
        assert_eq!(
            table.get_inner(&mut verify, None, key).unwrap().as_deref(),
            Some(expected.as_slice())
        );
        committed(verify.commit());

        let mut aborted_source = vec![0xa7; 159];
        let mut aborted = worker.begin().unwrap();
        assert!(table
            .put_presence_inner(
                &mut aborted,
                None,
                key,
                table.borrowed_staging_input(&aborted_source),
            )
            .unwrap());
        aborted.abort();
        aborted_source.fill(0x19);

        let mut verify_abort = worker.begin().unwrap();
        assert_eq!(
            table
                .get_inner(&mut verify_abort, None, key)
                .unwrap()
                .as_deref(),
            Some(expected.as_slice())
        );
        committed(verify_abort.commit());
    }

    #[test]
    #[allow(
        unsafe_code,
        reason = "the test retains each output buffer unchanged through transaction finish"
    )]
    fn scalar_borrowed_modify_resolves_reuses_and_commits_one_item_intent() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let table = Table::new_memory_direct(&runtime, bounded_value_config());
        let key = b"scalar/rmw";
        let original = (0..159)
            .map(|index| (index as u8).wrapping_mul(29).wrapping_add(3))
            .collect::<Vec<_>>();
        seed(&table, &mut worker, &[(key, &original)]);

        let mut first_output = vec![0x5a; STABLE_ATOMIC_VALUE_CAPACITY];
        let first_pointer = first_output.as_ptr();
        let mut first_expected = original.clone();
        first_expected[0] ^= 0xff;
        first_expected.push(0xa7);
        let mut first = worker.begin().unwrap();
        let (length, resolved) = unsafe {
            try_modify_resolving_borrowed_for_test(
                &table,
                &mut first,
                key,
                &mut first_output,
                |buffer, current_len| {
                    assert_eq!(&buffer[..current_len], original.as_slice());
                    buffer[0] ^= 0xff;
                    buffer[current_len] = 0xa7;
                    Ok(current_len + 1)
                },
            )
        }
        .unwrap();
        assert_eq!(length, Some(STABLE_ATOMIC_VALUE_CAPACITY));
        assert!(table.owns_resolved(resolved));
        first
            .with_item(
                &table.record_resource,
                TableKey::Record(resolved.record_id),
                |entry| {
                    let Some(TableIntent::Record {
                        replacement: RecordState::Live(staged),
                        ..
                    }) = entry.intent()
                    else {
                        panic!("scalar modify must retain one live intent");
                    };
                    assert!(matches!(
                        &staged.repr,
                        ValueRepr::BorrowedStaged(bytes)
                            if bytes.as_ptr() == first_pointer
                                && bytes.len() == STABLE_ATOMIC_VALUE_CAPACITY
                    ));
                    Ok(())
                },
            )
            .unwrap();
        committed(first.commit());
        first_output.fill(0x19);

        let mut second_output = vec![0x6b; STABLE_ATOMIC_VALUE_CAPACITY];
        let mut second_expected = first_expected.clone();
        second_expected[17] = 0xd4;
        let mut second = worker.begin().unwrap();
        assert_eq!(
            unsafe {
                table.try_modify_resolved_borrowed(
                    &mut second,
                    resolved,
                    &mut second_output,
                    |buffer, current_len| {
                        assert_eq!(&buffer[..current_len], first_expected.as_slice());
                        buffer[17] = 0xd4;
                        Ok(current_len)
                    },
                )
            }
            .unwrap(),
            Some(second_expected.len())
        );
        committed(second.commit());
        second_output.fill(0x2a);

        let mut verify = worker.begin().unwrap();
        assert_eq!(
            table.get_inner(&mut verify, None, key).unwrap().as_deref(),
            Some(second_expected.as_slice())
        );
        committed(verify.commit());
    }

    #[test]
    #[allow(
        unsafe_code,
        reason = "the tested miss and pre-callback failure paths retain no borrowed replacement"
    )]
    fn scalar_borrowed_modify_preserves_miss_and_small_buffer_contracts() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let table = Table::new_memory_direct(&runtime, bounded_value_config());
        let key = b"scalar/live";
        let missing_key = b"scalar/missing";
        let original = vec![0x45; 159];
        seed(&table, &mut worker, &[(key, &original)]);

        let mut missing_output = vec![0x67; STABLE_ATOMIC_VALUE_CAPACITY];
        let missing_before = missing_output.clone();
        let mut callback_called = false;
        let mut missing = worker.begin().unwrap();
        let (length, resolved_missing) = unsafe {
            try_modify_resolving_borrowed_for_test(
                &table,
                &mut missing,
                missing_key,
                &mut missing_output,
                |_, current_len| {
                    callback_called = true;
                    Ok(current_len)
                },
            )
        }
        .unwrap();
        assert_eq!(length, None);
        assert!(!callback_called);
        assert_eq!(missing_output, missing_before);
        committed(missing.commit());

        let mut resolved_output = vec![0x78; STABLE_ATOMIC_VALUE_CAPACITY];
        let resolved_before = resolved_output.clone();
        let mut resolved_callback_called = false;
        let mut resolved = worker.begin().unwrap();
        assert_eq!(
            unsafe {
                table.try_modify_resolved_borrowed(
                    &mut resolved,
                    resolved_missing,
                    &mut resolved_output,
                    |_, current_len| {
                        resolved_callback_called = true;
                        Ok(current_len)
                    },
                )
            }
            .unwrap(),
            None
        );
        assert!(!resolved_callback_called);
        assert_eq!(resolved_output, resolved_before);
        committed(resolved.commit());

        let mut short_output = vec![0x89; original.len() - 1];
        let short_before = short_output.clone();
        let mut short_callback_called = false;
        let mut short = worker.begin().unwrap();
        let error = unsafe {
            try_modify_resolving_borrowed_for_test(
                &table,
                &mut short,
                key,
                &mut short_output,
                |_, current_len| {
                    short_callback_called = true;
                    Ok(current_len)
                },
            )
        }
        .unwrap_err();
        assert_eq!(error, AccessError::Capacity(CapacityError::BufferLimit));
        assert!(!short_callback_called);
        assert_eq!(short_output, short_before);
        assert!(short.is_doomed());
        short.abort();
    }

    #[test]
    #[allow(
        unsafe_code,
        reason = "failure callbacks retain no replacement and successful outputs live through finish"
    )]
    fn scalar_borrowed_modify_dooms_on_callback_length_error_and_unwind() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let table = Table::new_memory_direct(&runtime, bounded_value_config());
        let key = b"scalar/fail";
        let original = vec![0x31; 159];
        seed(&table, &mut worker, &[(key, &original)]);

        let mut callback_output = vec![0_u8; STABLE_ATOMIC_VALUE_CAPACITY];
        let mut callback = worker.begin().unwrap();
        let error = unsafe {
            try_modify_resolving_borrowed_for_test(
                &table,
                &mut callback,
                key,
                &mut callback_output,
                |buffer, _current_len| {
                    buffer[0] = 0x42;
                    Err(InvalidUse::IllegalItemState.into())
                },
            )
        }
        .unwrap_err();
        assert_eq!(error, AccessError::InvalidUse(InvalidUse::IllegalItemState));
        assert!(callback.is_doomed());
        assert_eq!(
            callback.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Doomed)
        );

        let mut length_output = vec![0_u8; STABLE_ATOMIC_VALUE_CAPACITY];
        let mut length = worker.begin().unwrap();
        let error = unsafe {
            try_modify_resolving_borrowed_for_test(
                &table,
                &mut length,
                key,
                &mut length_output,
                |_, _current_len| Ok(STABLE_ATOMIC_VALUE_CAPACITY + 1),
            )
        }
        .unwrap_err();
        assert_eq!(error, AccessError::Capacity(CapacityError::BufferLimit));
        assert!(length.is_doomed());
        length.abort();

        let mut unwind_output = vec![0_u8; STABLE_ATOMIC_VALUE_CAPACITY];
        let mut unwind = worker.begin().unwrap();
        let panic = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| unsafe {
            let _ = try_modify_resolving_borrowed_for_test(
                &table,
                &mut unwind,
                key,
                &mut unwind_output,
                |buffer, _current_len| -> Result<usize, AccessError> {
                    buffer[0] = 0x53;
                    panic!("intentional scalar modify unwind")
                },
            );
        }));
        assert!(panic.is_err());
        assert!(unwind.is_doomed());
        unwind.abort();

        let mut verify = worker.begin().unwrap();
        assert_eq!(
            table.get_inner(&mut verify, None, key).unwrap().as_deref(),
            Some(original.as_slice())
        );
        committed(verify.commit());
    }

    #[test]
    #[allow(
        unsafe_code,
        reason = "the test retains the output buffer unchanged until commit"
    )]
    fn fixed_borrowed_modify_stages_the_caller_buffer_and_commits_its_bytes() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let table = Table::new_memory_direct(&runtime, bounded_value_config());
        let key = *b"rmw/one!";
        let original = (0..159)
            .map(|index| (index as u8).wrapping_mul(29).wrapping_add(3))
            .collect::<Vec<_>>();
        seed(&table, &mut worker, &[(&key, &original)]);

        let mut output = vec![0x5a; STABLE_ATOMIC_VALUE_CAPACITY];
        let output_pointer = output.as_ptr();
        let mut expected = original.clone();
        expected[0] ^= 0xff;
        expected.push(0xa7);
        let mut batch = PointReadBatch::with_capacity(1);
        let mut transaction = worker.begin().unwrap();
        let staged = unsafe {
            try_modify_fixed_borrowed_for_test(
                &table,
                &mut transaction,
                &key,
                &mut batch,
                &mut output,
                |buffer, current_len| {
                    assert_eq!(current_len, original.len());
                    assert_eq!(&buffer[..current_len], original.as_slice());
                    buffer[0] ^= 0xff;
                    buffer[current_len] = 0xa7;
                    Ok(current_len + 1)
                },
            )
        }
        .unwrap();
        assert_eq!(staged, Some(STABLE_ATOMIC_VALUE_CAPACITY));
        assert!(batch.results().is_empty());

        let record_id = table.shared().lookup(None, &key).unwrap().unwrap();
        transaction
            .with_item(
                &table.record_resource,
                TableKey::Record(record_id),
                |entry| {
                    let Some(TableIntent::Record {
                        replacement: RecordState::Live(staged),
                        ..
                    }) = entry.intent()
                    else {
                        panic!("borrowed fixed modify must retain a live intent");
                    };
                    assert!(matches!(
                        &staged.repr,
                        ValueRepr::BorrowedStaged(bytes)
                            if bytes.as_ptr() == output_pointer
                                && bytes.len() == STABLE_ATOMIC_VALUE_CAPACITY
                    ));
                    Ok(())
                },
            )
            .unwrap();
        committed(transaction.commit());

        output.fill(0x19);
        let mut verify = worker.begin().unwrap();
        assert_eq!(
            table.get_inner(&mut verify, None, &key).unwrap().as_deref(),
            Some(expected.as_slice())
        );
        committed(verify.commit());
    }

    #[test]
    #[allow(
        unsafe_code,
        reason = "the test retains the output allocation through commit"
    )]
    fn fixed_borrowed_modify_owns_replacements_beyond_the_stable_atomic_tier() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let table = Table::new_memory_direct(&runtime, bounded_value_config());
        let key = *b"rmw/heap";
        let original = vec![0x41; STABLE_ATOMIC_VALUE_CAPACITY + 1];
        seed(&table, &mut worker, &[(&key, &original)]);

        let mut output = vec![0_u8; STABLE_ATOMIC_VALUE_CAPACITY + 4];
        let mut expected = original.clone();
        expected[0] = 0x52;
        expected.push(0x63);
        let mut batch = PointReadBatch::with_capacity(1);
        let mut transaction = worker.begin().unwrap();
        assert_eq!(
            unsafe {
                try_modify_fixed_borrowed_for_test(
                    &table,
                    &mut transaction,
                    &key,
                    &mut batch,
                    &mut output,
                    |buffer, current_len| {
                        assert_eq!(&buffer[..current_len], original.as_slice());
                        buffer[0] = 0x52;
                        buffer[current_len] = 0x63;
                        Ok(current_len + 1)
                    },
                )
            }
            .unwrap(),
            Some(expected.len())
        );

        let record_id = table.shared().lookup(None, &key).unwrap().unwrap();
        transaction
            .with_item(
                &table.record_resource,
                TableKey::Record(record_id),
                |entry| {
                    let Some(TableIntent::Record {
                        replacement: RecordState::Live(staged),
                        ..
                    }) = entry.intent()
                    else {
                        panic!("oversized fixed modify must retain one live intent");
                    };
                    assert!(matches!(&staged.repr, ValueRepr::Shared(_)));
                    assert_eq!(staged.as_ref(), expected);
                    Ok(())
                },
            )
            .unwrap();
        committed(transaction.commit());

        output.fill(0x74);
        let mut verify = worker.begin().unwrap();
        assert_eq!(
            table.get_inner(&mut verify, None, &key).unwrap().as_deref(),
            Some(expected.as_slice())
        );
        committed(verify.commit());
    }

    #[test]
    #[allow(
        unsafe_code,
        reason = "the test retains both output buffers unchanged until abort"
    )]
    fn fixed_borrowed_modify_reads_an_earlier_borrowed_intent_and_aborts_cleanly() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let table = Table::new_memory_direct(&runtime, bounded_value_config());
        let key = *b"rmw/ryw!";
        let original = (0..159)
            .map(|index| (index as u8).wrapping_mul(17).wrapping_add(11))
            .collect::<Vec<_>>();
        seed(&table, &mut worker, &[(&key, &original)]);

        let mut first_output = vec![0_u8; STABLE_ATOMIC_VALUE_CAPACITY];
        let mut second_output = vec![0_u8; STABLE_ATOMIC_VALUE_CAPACITY];
        let mut first_expected = original.clone();
        first_expected[7] = 0xd1;
        let mut second_expected = first_expected.clone();
        second_expected[13] = 0xe2;
        let mut batch = PointReadBatch::with_capacity(1);
        let mut transaction = worker.begin().unwrap();

        assert_eq!(
            unsafe {
                try_modify_fixed_borrowed_for_test(
                    &table,
                    &mut transaction,
                    &key,
                    &mut batch,
                    &mut first_output,
                    |buffer, current_len| {
                        assert_eq!(&buffer[..current_len], original.as_slice());
                        buffer[7] = 0xd1;
                        Ok(current_len)
                    },
                )
            }
            .unwrap(),
            Some(original.len())
        );
        assert_eq!(
            unsafe {
                try_modify_fixed_borrowed_for_test(
                    &table,
                    &mut transaction,
                    &key,
                    &mut batch,
                    &mut second_output,
                    |buffer, current_len| {
                        assert_eq!(&buffer[..current_len], first_expected.as_slice());
                        buffer[13] = 0xe2;
                        Ok(current_len)
                    },
                )
            }
            .unwrap(),
            Some(second_expected.len())
        );
        transaction.abort();

        first_output.fill(0x31);
        second_output.fill(0x42);
        let mut verify = worker.begin().unwrap();
        assert_eq!(
            table.get_inner(&mut verify, None, &key).unwrap().as_deref(),
            Some(original.as_slice())
        );
        committed(verify.commit());
    }

    #[test]
    #[allow(
        unsafe_code,
        reason = "the test retains successful borrowed output through the doomed commit"
    )]
    fn fixed_borrowed_modify_callback_error_dooms_and_discards_earlier_intents() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let table = Table::new_memory_direct(&runtime, bounded_value_config());
        let first_key = *b"rmw/err1";
        let second_key = *b"rmw/err2";
        let first_original = vec![0x21; 159];
        let second_original = vec![0x32; 159];
        seed(
            &table,
            &mut worker,
            &[
                (&first_key, &first_original),
                (&second_key, &second_original),
            ],
        );

        let mut first_output = vec![0_u8; STABLE_ATOMIC_VALUE_CAPACITY];
        let mut second_output = vec![0_u8; STABLE_ATOMIC_VALUE_CAPACITY];
        let mut batch = PointReadBatch::with_capacity(1);
        let mut transaction = worker.begin().unwrap();
        assert_eq!(
            unsafe {
                try_modify_fixed_borrowed_for_test(
                    &table,
                    &mut transaction,
                    &first_key,
                    &mut batch,
                    &mut first_output,
                    |buffer, current_len| {
                        buffer[0] = 0x91;
                        Ok(current_len)
                    },
                )
            }
            .unwrap(),
            Some(first_original.len())
        );

        let error = unsafe {
            try_modify_fixed_borrowed_for_test(
                &table,
                &mut transaction,
                &second_key,
                &mut batch,
                &mut second_output,
                |buffer, _current_len| {
                    assert_eq!(&buffer[..second_original.len()], second_original.as_slice());
                    buffer[0] = 0xa2;
                    Err(InvalidUse::IllegalItemState.into())
                },
            )
        }
        .unwrap_err();
        assert_eq!(error, AccessError::InvalidUse(InvalidUse::IllegalItemState));
        assert!(transaction.is_doomed());
        assert!(batch.is_empty());
        assert_eq!(
            transaction.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Doomed)
        );

        first_output.fill(0x43);
        second_output.fill(0x54);
        let mut verify = worker.begin().unwrap();
        assert_eq!(
            table
                .get_inner(&mut verify, None, &first_key)
                .unwrap()
                .as_deref(),
            Some(first_original.as_slice())
        );
        assert_eq!(
            table
                .get_inner(&mut verify, None, &second_key)
                .unwrap()
                .as_deref(),
            Some(second_original.as_slice())
        );
        committed(verify.commit());
    }

    #[test]
    #[allow(
        unsafe_code,
        reason = "the tested failure paths retain no borrowed replacement"
    )]
    fn fixed_borrowed_modify_handles_small_output_and_missing_rows_without_partial_copy() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let table = Table::new_memory_direct(&runtime, bounded_value_config());
        let present_key = *b"rmw/full";
        let missing_key = *b"rmw/miss";
        let original = vec![0x65; 159];
        seed(&table, &mut worker, &[(&present_key, &original)]);

        let mut small_output = vec![0x76; original.len() - 1];
        let small_before = small_output.clone();
        let mut callback_called = false;
        let mut batch = PointReadBatch::with_capacity(1);
        let mut failed = worker.begin().unwrap();
        let error = unsafe {
            try_modify_fixed_borrowed_for_test(
                &table,
                &mut failed,
                &present_key,
                &mut batch,
                &mut small_output,
                |_, current_len| {
                    callback_called = true;
                    Ok(current_len)
                },
            )
        }
        .unwrap_err();
        assert_eq!(error, AccessError::Capacity(CapacityError::BufferLimit));
        assert!(!callback_called);
        assert_eq!(small_output, small_before);
        assert!(failed.is_doomed());
        failed.abort();

        let mut missing_output = vec![0x87; STABLE_ATOMIC_VALUE_CAPACITY];
        let missing_before = missing_output.clone();
        let mut missing_callback_called = false;
        let mut missing = worker.begin().unwrap();
        assert_eq!(
            unsafe {
                try_modify_fixed_borrowed_for_test(
                    &table,
                    &mut missing,
                    &missing_key,
                    &mut batch,
                    &mut missing_output,
                    |_, current_len| {
                        missing_callback_called = true;
                        Ok(current_len)
                    },
                )
            }
            .unwrap(),
            None
        );
        assert!(!missing_callback_called);
        assert_eq!(missing_output, missing_before);
        committed(missing.commit());
    }

    #[test]
    #[allow(
        unsafe_code,
        reason = "the borrowed output remains immutable through the conflicting commit"
    )]
    fn fixed_borrowed_modify_keeps_occ_validation_on_a_concurrent_write() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let table = Table::new_memory_direct(&runtime, bounded_value_config());
        let mut stale_worker = runtime.attach().unwrap();
        let key = *b"rmw/occ!";
        let original = vec![0x18; 159];
        let winner = vec![0x29; 159];
        seed(&table, &mut stale_worker, &[(&key, &original)]);

        let mut output = vec![0_u8; STABLE_ATOMIC_VALUE_CAPACITY];
        let mut batch = PointReadBatch::with_capacity(1);
        let mut stale = stale_worker.begin().unwrap();
        assert_eq!(
            unsafe {
                try_modify_fixed_borrowed_for_test(
                    &table,
                    &mut stale,
                    &key,
                    &mut batch,
                    &mut output,
                    |buffer, current_len| {
                        buffer[0] = 0x3a;
                        Ok(current_len)
                    },
                )
            }
            .unwrap(),
            Some(original.len())
        );

        std::thread::scope(|scope| {
            scope
                .spawn(|| {
                    let mut worker = runtime.attach().unwrap();
                    let mut update = worker.begin().unwrap();
                    table
                        .put_inner(&mut update, None, &key, Value::from(winner.as_slice()))
                        .unwrap();
                    committed(update.commit());
                })
                .join()
                .unwrap();
        });

        assert_eq!(
            stale.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );
        output.fill(0x4b);
        let mut verify = stale_worker.begin().unwrap();
        assert_eq!(
            table.get_inner(&mut verify, None, &key).unwrap().as_deref(),
            Some(winner.as_slice())
        );
        committed(verify.commit());
    }

    #[test]
    fn bounded_direct_values_preserve_owned_copy_visitor_and_storage_transitions() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = runtime.attach().unwrap();
        let table = Table::new_memory_direct(&runtime, bounded_value_config());
        let key = b"bounded/transitions";
        let stable_159 = (0..159)
            .map(|index| (index as u8).wrapping_mul(17).wrapping_add(3))
            .collect::<Vec<_>>();

        let mut insert = worker.begin().unwrap();
        let staged = table.staging_value(&stable_159);
        assert!(matches!(&staged.repr, ValueRepr::Staged(_)));
        assert_eq!(table.put_inner(&mut insert, None, key, staged), Ok(None));
        committed(insert.commit());

        let token = table.shared().lookup(None, key).unwrap().unwrap();
        let access = table.shared().resolve_directory_access(token).unwrap();
        assert!(access.stable.is_some());
        assert_eq!(
            record_state_descriptor(
                access
                    .record
                    .state
                    .tail_and_descriptor
                    .load(Ordering::Acquire)
            ),
            RECORD_STATE_STABLE_BASE + 159
        );
        assert!(access.record.state.shared.load_full().is_none());

        let mut owned_read = worker.begin().unwrap();
        let owned = table
            .get_inner(&mut owned_read, None, key)
            .unwrap()
            .unwrap();
        assert_eq!(owned.as_ref(), stable_159);
        assert!(matches!(&owned.repr, ValueRepr::Shared(_)));
        committed(owned_read.commit());

        let mut visitor_read = worker.begin().unwrap();
        table
            .visit_get_bytes_inner(&mut visitor_read, None, key, |value| {
                assert_eq!(value, Some(stable_159.as_slice()));
            })
            .unwrap();
        committed(visitor_read.commit());

        let mut too_small = [0x5a_u8; 158];
        let unchanged = too_small;
        let mut short_read = worker.begin().unwrap();
        let (outcome, resolved) = table
            .copy_get_resolving_inner(&mut short_read, None, key, &mut too_small)
            .unwrap();
        assert_eq!(outcome, ValueCopyOutcome::BufferTooSmall { required: 159 });
        assert_eq!(too_small, unchanged);
        committed(short_read.commit());

        let mut copied = [0_u8; STABLE_ATOMIC_VALUE_CAPACITY];
        let mut copy_read = worker.begin().unwrap();
        assert_eq!(
            table
                .copy_get_resolved(&mut copy_read, resolved, &mut copied)
                .unwrap(),
            ValueCopyOutcome::Copied { len: 159 }
        );
        assert_eq!(&copied[..159], stable_159);
        committed(copy_read.commit());

        let shared_161 = vec![0x61; STABLE_ATOMIC_VALUE_CAPACITY + 1];
        let mut to_shared = worker.begin().unwrap();
        table
            .put_inner(&mut to_shared, None, key, table.staging_value(&shared_161))
            .unwrap();
        committed(to_shared.commit());
        let access = table.shared().resolve_directory_access(token).unwrap();
        assert_eq!(
            record_state_descriptor(
                access
                    .record
                    .state
                    .tail_and_descriptor
                    .load(Ordering::Acquire)
            ),
            RECORD_STATE_SHARED
        );
        assert!(access.record.state.shared.load_full().is_some());
        assert_eq!(owned.as_ref(), stable_159);

        let stable_160 = vec![0x70; STABLE_ATOMIC_VALUE_CAPACITY];
        let mut back_to_stable = worker.begin().unwrap();
        table
            .put_inner(
                &mut back_to_stable,
                None,
                key,
                table.staging_value(&stable_160),
            )
            .unwrap();
        committed(back_to_stable.commit());
        let access = table.shared().resolve_directory_access(token).unwrap();
        assert_eq!(
            record_state_descriptor(
                access
                    .record
                    .state
                    .tail_and_descriptor
                    .load(Ordering::Acquire)
            ),
            RECORD_STATE_STABLE_BASE + STABLE_ATOMIC_VALUE_CAPACITY as u16
        );
        assert!(access.record.state.shared.load_full().is_none());

        let inline = vec![0x38; INLINE_VALUE_CAPACITY];
        let mut to_inline = worker.begin().unwrap();
        table
            .put_inner(&mut to_inline, None, key, table.staging_value(&inline))
            .unwrap();
        committed(to_inline.commit());
        let access = table.shared().resolve_directory_access(token).unwrap();
        assert_eq!(
            record_state_descriptor(
                access
                    .record
                    .state
                    .tail_and_descriptor
                    .load(Ordering::Acquire)
            ),
            RECORD_STATE_INLINE_BASE + INLINE_VALUE_CAPACITY as u16
        );

        let mut to_stable = worker.begin().unwrap();
        table
            .put_inner(
                &mut to_stable,
                None,
                key,
                table.staging_value(&[0x39; INLINE_VALUE_CAPACITY + 1]),
            )
            .unwrap();
        committed(to_stable.commit());
        let mut remove = worker.begin().unwrap();
        table.remove_inner(&mut remove, None, key).unwrap();
        committed(remove.commit());
        let access = table.shared().resolve_directory_access(token).unwrap();
        assert_eq!(
            record_state_descriptor(
                access
                    .record
                    .state
                    .tail_and_descriptor
                    .load(Ordering::Acquire)
            ),
            RECORD_STATE_TOMBSTONE
        );
        assert!(access.record.state.shared.load_full().is_none());
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Healthy);
    }

    #[test]
    fn shared_values_preserve_all_constructors_across_the_medium_boundary() {
        for length in [
            INLINE_VALUE_CAPACITY + 1,
            SHARED_INLINE_VALUE_CAPACITY - 1,
            SHARED_INLINE_VALUE_CAPACITY,
            SHARED_INLINE_VALUE_CAPACITY + 1,
        ] {
            let bytes = (0..length)
                .map(|index| (index as u8).wrapping_mul(29).wrapping_add(7))
                .collect::<Vec<_>>();
            let arc_bytes: Arc<[u8]> = Arc::from(bytes.clone());
            let values = [
                Value::from(bytes.as_slice()),
                Value::from(bytes.clone()),
                Value::from(bytes.clone().into_boxed_slice()),
                Value::from(arc_bytes),
            ];

            for value in values {
                assert_eq!(value.as_ref(), bytes);
                let ValueRepr::Shared(shared) = &value.repr else {
                    panic!("a value beyond the record-inline tier must use shared storage");
                };
                match shared.as_ref() {
                    SharedValue::Medium { len, bytes: inline } => {
                        assert!(length <= SHARED_INLINE_VALUE_CAPACITY);
                        assert_eq!(usize::from(*len), length);
                        assert_eq!(&inline[..length], bytes);
                        assert!(inline[length..].iter().all(|byte| *byte == 0));
                    }
                    SharedValue::Heap(heap) => {
                        assert!(length > SHARED_INLINE_VALUE_CAPACITY);
                        assert_eq!(heap, &bytes);
                    }
                }
            }
        }
    }

    #[test]
    fn committed_values_preserve_exact_packed_tail_and_shared_boundaries() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();

        for length in [0, 32, 33, 37, 38, 39, 127, 128, 129] {
            let key = format!("packed/boundary/{length}").into_bytes();
            let mut bytes = (0..length)
                .map(|index| (index as u8).wrapping_mul(37).wrapping_add(0x80))
                .collect::<Vec<_>>();
            if let Some(first) = bytes.first_mut() {
                *first = 0;
            }
            if let Some(last) = bytes.last_mut() {
                *last = u8::MAX;
            }

            let mut transaction = worker.begin().unwrap();
            table
                .put_inner(&mut transaction, None, &key, Value::from(bytes.clone()))
                .unwrap();
            committed(transaction.commit());

            let record_id = table.shared().lookup(None, &key).unwrap().unwrap();
            let record = table.shared().resolve_directory_record(record_id).unwrap();
            let descriptor =
                record_state_descriptor(record.state.tail_and_descriptor.load(Ordering::Acquire));
            if length <= INLINE_VALUE_CAPACITY {
                assert_eq!(descriptor, RECORD_STATE_INLINE_BASE + length as u16);
                assert!(record.state.shared.load_full().is_none());
            } else {
                assert_eq!(descriptor, RECORD_STATE_SHARED);
                let shared = record.state.shared.load_full().unwrap();
                if length <= SHARED_INLINE_VALUE_CAPACITY {
                    assert!(matches!(shared.as_ref(), SharedValue::Medium { .. }));
                } else {
                    assert!(matches!(shared.as_ref(), SharedValue::Heap(_)));
                }
            }
            assert_eq!(
                committed_record_snapshot(record).value().unwrap().as_ref(),
                bytes
            );
        }
    }

    #[test]
    fn committed_state_metadata_classifies_only_the_packed_descriptor() {
        let state = CommittedRecordState::tombstone();
        let arbitrary_low_48_bits = [0, 1, 0x1234_5678_9abc, RECORD_STATE_TAIL_MASK];
        for tail in arbitrary_low_48_bits {
            state.tail_and_descriptor.store(
                pack_record_state_word(RECORD_STATE_TOMBSTONE, tail),
                Ordering::Release,
            );
            assert_eq!(
                state.load_metadata(None),
                CommittedStateMetadataLoad::Complete(RecordStateMetadata {
                    live: false,
                    shared: false,
                })
            );

            // Successfully classifying Shared without populating the embedded
            // slot proves that metadata neither enters ArcSwap nor clones an
            // Arc. Arbitrary low bits cannot alter the high descriptor.
            state.tail_and_descriptor.store(
                pack_record_state_word(RECORD_STATE_SHARED, tail),
                Ordering::Release,
            );
            assert_eq!(
                state.load_metadata(None),
                CommittedStateMetadataLoad::Complete(RecordStateMetadata {
                    live: true,
                    shared: true,
                })
            );

            for length in [0, 32, 33, 37, INLINE_VALUE_CAPACITY] {
                state.tail_and_descriptor.store(
                    pack_record_state_word(RECORD_STATE_INLINE_BASE + length as u16, tail),
                    Ordering::Release,
                );
                assert_eq!(
                    state.load_metadata(None),
                    CommittedStateMetadataLoad::Complete(RecordStateMetadata {
                        live: true,
                        shared: false,
                    })
                );
            }

            for descriptor in [
                RECORD_STATE_INLINE_BASE + INLINE_VALUE_CAPACITY as u16 + 1,
                0x1234,
                RECORD_STATE_UPDATING,
                RECORD_STATE_POISONED,
            ] {
                state
                    .tail_and_descriptor
                    .store(pack_record_state_word(descriptor, tail), Ordering::Release);
                assert!(matches!(
                    state.load_metadata(None),
                    CommittedStateMetadataLoad::Incomplete(_)
                ));
            }
        }
    }

    #[test]
    fn bounded_descriptors_require_the_extended_cell_and_reject_out_of_range_lengths() {
        let state = CommittedRecordState::tombstone();
        let cell = StableAtomicValueCell::empty();
        for length in [INLINE_VALUE_CAPACITY + 1, STABLE_ATOMIC_VALUE_CAPACITY] {
            state.tail_and_descriptor.store(
                pack_record_state_word(RECORD_STATE_STABLE_BASE + length as u16, 0),
                Ordering::Release,
            );
            assert_eq!(
                state.load_metadata(Some(&cell)),
                CommittedStateMetadataLoad::Complete(RecordStateMetadata {
                    live: true,
                    shared: false,
                })
            );
            assert!(matches!(
                state.load_metadata(None),
                CommittedStateMetadataLoad::Incomplete(_)
            ));
            assert!(matches!(
                state.load(None),
                CommittedStateLoad::Incomplete(_)
            ));
            assert!(matches!(
                state.load_lease(None),
                CommittedStateLeaseLoad::Incomplete(_)
            ));
        }

        for invalid_length in [INLINE_VALUE_CAPACITY, STABLE_ATOMIC_VALUE_CAPACITY + 1] {
            state.tail_and_descriptor.store(
                pack_record_state_word(RECORD_STATE_STABLE_BASE + invalid_length as u16, 0),
                Ordering::Release,
            );
            assert!(matches!(
                state.load_metadata(Some(&cell)),
                CommittedStateMetadataLoad::Incomplete(_)
            ));
        }
    }

    #[test]
    fn bounded_copy_preserves_suffix_word_boundaries_and_output_remainder() {
        for length in [39, 46, 47, 158, STABLE_ATOMIC_VALUE_CAPACITY] {
            let state = CommittedRecordState::tombstone();
            let cell = StableAtomicValueCell::empty();
            let value = (0..length)
                .map(|index| (index as u8).wrapping_mul(37).wrapping_add(11))
                .collect::<Vec<_>>();
            let packed = store_test_stable_value(&state, &cell, &value);
            let version = AtomicVersion::default();
            let observed = version.observe().unwrap();
            let mut output = [0x5c; STABLE_ATOMIC_VALUE_CAPACITY + 7];

            assert_eq!(
                state.copy_stable_after_observation(
                    &cell,
                    &version,
                    observed,
                    packed,
                    length,
                    &mut output,
                ),
                Ok(ValueCopyOutcome::Copied { len: length })
            );
            assert_eq!(&output[..length], value.as_slice());
            assert_eq!(
                &output[length..],
                &[0x5c; STABLE_ATOMIC_VALUE_CAPACITY + 7][length..]
            );
        }
    }

    #[test]
    fn bounded_copy_conflict_leaves_caller_output_untouched() {
        for length in [39, 46, 47, STABLE_ATOMIC_VALUE_CAPACITY] {
            let state = CommittedRecordState::tombstone();
            let cell = StableAtomicValueCell::empty();
            let value = vec![0x9b; length];
            let packed = store_test_stable_value(&state, &cell, &value);
            let version = AtomicVersion::default();
            let observed = version.observe().unwrap();
            let owner = sto_core::OwnerId::new(0).unwrap();
            let mut guard = version.try_acquire_detached(owner).unwrap();
            let mut output = [0x5c; STABLE_ATOMIC_VALUE_CAPACITY];
            let unchanged = output;

            assert_eq!(
                state.copy_stable_after_observation(
                    &cell,
                    &version,
                    observed,
                    packed,
                    length,
                    &mut output,
                ),
                Err(AccessError::Conflict(Conflict::ReadValidation))
            );
            assert_eq!(output, unchanged);
            guard.release_abort(&version).unwrap();
        }
    }

    #[test]
    fn embedded_large_value_slot_replaces_clears_and_keeps_old_arcs_alive() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let first = SharedValue::from_slice(&[0x41; INLINE_VALUE_CAPACITY + 1]);
        let second = SharedValue::from_slice(&[0x52; INLINE_VALUE_CAPACITY + 2]);

        let mut initial = worker.begin().unwrap();
        table
            .put_inner(
                &mut initial,
                None,
                b"embedded/shared",
                Value {
                    repr: ValueRepr::Shared(Arc::clone(&first)),
                },
            )
            .unwrap();
        committed(initial.commit());
        let record_id = table
            .shared()
            .lookup(None, b"embedded/shared")
            .unwrap()
            .unwrap();
        let record = table.shared().resolve_directory_record(record_id).unwrap();
        let pinned_first = record.state.shared.load_full().unwrap();
        assert!(Arc::ptr_eq(&pinned_first, &first));

        let mut replace = worker.begin().unwrap();
        table
            .put_inner(
                &mut replace,
                None,
                b"embedded/shared",
                Value {
                    repr: ValueRepr::Shared(Arc::clone(&second)),
                },
            )
            .unwrap();
        committed(replace.commit());
        let loaded_second = record.state.shared.load_full().unwrap();
        assert!(Arc::ptr_eq(&loaded_second, &second));
        assert_eq!(pinned_first.as_bytes(), &[0x41; INLINE_VALUE_CAPACITY + 1]);
        assert_eq!(Arc::strong_count(&first), 2);
        drop(pinned_first);
        assert_eq!(Arc::strong_count(&first), 1);
        drop(loaded_second);

        let mut clear = worker.begin().unwrap();
        table
            .put_inner(&mut clear, None, b"embedded/shared", Value::from(b"inline"))
            .unwrap();
        committed(clear.commit());
        assert!(record.state.shared.load_full().is_none());
        assert_eq!(Arc::strong_count(&second), 1);
    }

    #[test]
    fn fixed_point_batch_preserves_order_misses_duplicates_and_reuses_capacity() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let hit_a = [0x00, 0x11, 0x00, 0xff];
        let miss = [0x00, 0x22, 0x00, 0xff];
        let hit_b = [0xff, 0x00, 0x33, 0x00];
        seed(&table, &mut worker, &[(&hit_a, b"A"), (&hit_b, b"B")]);

        let keys = [hit_a, miss, miss, hit_b];
        let mut batch = PointReadBatch::with_capacity(keys.len());
        let retained_capacity = batch.capacity();

        // An empty, all-hit, exactly unique batch uses the contiguous typed
        // lane. A later scalar access must materialize that prefix into the
        // ordinary exact index and reuse the same logical item.
        let unique_hits = [hit_b, hit_a];
        let mut transaction = worker.begin().unwrap();
        let snapshots = table
            .get_fixed_inner(&mut transaction, None, &unique_hits, &mut batch, |batch| {
                for key in &unique_hits {
                    batch.push_record_id(table.shared().lookup(None, key)?);
                }
                Ok(())
            })
            .unwrap();
        assert_eq!(snapshots[0].as_deref(), Some(&b"B"[..]));
        assert_eq!(snapshots[1].as_deref(), Some(&b"A"[..]));
        assert_eq!(
            table
                .get_inner(&mut transaction, None, &hit_b)
                .unwrap()
                .as_deref(),
            Some(&b"B"[..])
        );
        committed(transaction.commit());

        let mut transaction = worker.begin().unwrap();
        let snapshots = table
            .get_fixed_inner(&mut transaction, None, &keys, &mut batch, |batch| {
                for key in &keys {
                    batch.push_record_id(table.shared().lookup(None, key)?);
                }
                Ok(())
            })
            .unwrap();
        assert_eq!(snapshots.len(), keys.len());
        assert_eq!(snapshots[0].as_deref(), Some(&b"A"[..]));
        assert_eq!(snapshots[1], None);
        assert_eq!(snapshots[2], None);
        assert_eq!(snapshots[3].as_deref(), Some(&b"B"[..]));
        assert_eq!(batch.capacity(), retained_capacity);
        committed(transaction.commit());

        // The first missing element interns one tombstone. Re-looking up the
        // duplicate at its sequential position must reuse that exact ID.
        assert_eq!(table.usage().consumed_record_ids(), 3);

        let mut staged = worker.begin().unwrap();
        table
            .put_inner(&mut staged, None, &miss, Value::from(&b"M"[..]))
            .unwrap();
        let repeated = [miss, hit_a];
        let snapshots = table
            .get_fixed_inner(&mut staged, None, &repeated, &mut batch, |batch| {
                for key in &repeated {
                    batch.push_record_id(table.shared().lookup(None, key)?);
                }
                Ok(())
            })
            .unwrap();
        assert_eq!(snapshots[0].as_deref(), Some(&b"M"[..]));
        assert_eq!(snapshots[1].as_deref(), Some(&b"A"[..]));
        assert_eq!(batch.capacity(), retained_capacity);
        staged.abort();

        batch.clear();
        assert!(batch.is_empty());
        assert_eq!(batch.capacity(), retained_capacity);
    }

    #[test]
    fn small_fixed_batch_capacity_uses_pairwise_proof_without_hash_scratch() {
        let mut batch = PointReadBatch::with_capacity(SMALL_UNIQUE_KEY_BATCH);
        assert_eq!(batch.unique_key_index.capacity(), 0);

        let retained_capacity = batch.capacity();
        assert!(retained_capacity >= SMALL_UNIQUE_KEY_BATCH);
        batch.prepare_read::<true>(retained_capacity).unwrap();
        assert_eq!(batch.unique_key_index.capacity(), 0);
        assert_eq!(batch.capacity(), retained_capacity);

        batch.prepare_modify::<false>(retained_capacity).unwrap();
        assert_eq!(batch.unique_key_index.capacity(), 0);
        assert_eq!(batch.capacity(), retained_capacity);
    }

    #[test]
    fn lazy_fixed_batch_allocates_hash_scratch_only_above_pairwise_limit() {
        let mut batch = PointReadBatch::new();
        batch.prepare_read::<true>(SMALL_UNIQUE_KEY_BATCH).unwrap();
        assert_eq!(batch.unique_key_index.capacity(), 0);

        let hashed_length = SMALL_UNIQUE_KEY_BATCH + 1;
        batch.prepare_read::<true>(hashed_length).unwrap();
        let retained_hash_capacity = batch.unique_key_index.capacity();
        assert!(retained_hash_capacity >= hashed_length);

        batch.clear();
        batch.prepare_read::<true>(hashed_length).unwrap();
        assert_eq!(batch.unique_key_index.capacity(), retained_hash_capacity);
    }

    #[test]
    fn fixed_insert_scratch_preparation_is_lazy_and_reuses_capacity() {
        let mut batch = PointReadBatch::new();

        // Preparing an ordinary all-hit mutation batch grows only the lookup,
        // item, alias, and optional value buffers. The five native-insert
        // buffers stay untouched until the caller has observed a real miss.
        batch.prepare_modify::<false>(4).unwrap();
        assert_eq!(fixed_insert_capacities(&batch), [0; 5]);

        batch.fixed_inserts.prepare(3).unwrap();
        let retained = fixed_insert_capacities(&batch);
        assert!(retained.into_iter().all(|capacity| capacity >= 3));

        batch.clear();
        assert_eq!(fixed_insert_capacities(&batch), retained);
        batch.prepare_modify::<false>(2).unwrap();
        assert_eq!(fixed_insert_capacities(&batch), retained);
        batch.fixed_inserts.prepare(2).unwrap();
        assert_eq!(fixed_insert_capacities(&batch), retained);
    }

    #[test]
    fn fixed_point_mutations_fuse_unique_hits_and_preserve_fallback_order() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let hit_a = [0x00, 0x11, 0xaa, 0x01];
        let hit_b = [0x00, 0x22, 0xbb, 0x02];
        let hit_c = [0x00, 0x33, 0xcc, 0x03];
        let miss = [0x00, 0x44, 0xdd, 0x04];
        seed(
            &table,
            &mut worker,
            &[(&hit_a, b"A"), (&hit_b, b"B"), (&hit_c, b"C")],
        );

        // An empty, all-hit, exactly unique transaction selects the direct
        // append lane while still exposing the ordinary Entry semantics.
        let keys = [hit_a, hit_b, hit_c];
        let mut batch = PointReadBatch::with_capacity(keys.len());
        let mut transaction = worker.begin().unwrap();
        let snapshots = table
            .modify_fixed_inner(
                &mut transaction,
                None,
                &keys,
                &mut batch,
                |index, current| match index {
                    0 => {
                        assert_eq!(current.map(Value::as_ref), Some(&b"A"[..]));
                        PointMutation::Put(Value::from(&b"A2"[..]))
                    }
                    1 => {
                        assert_eq!(current.map(Value::as_ref), Some(&b"B"[..]));
                        PointMutation::Remove
                    }
                    2 => {
                        assert_eq!(current.map(Value::as_ref), Some(&b"C"[..]));
                        PointMutation::Keep
                    }
                    _ => unreachable!(),
                },
                |batch| {
                    for key in &keys {
                        batch.push_record_id(table.shared().lookup(None, key)?);
                    }
                    Ok(())
                },
            )
            .unwrap();
        assert_eq!(snapshots[0].as_deref(), Some(&b"A"[..]));
        assert_eq!(snapshots[1].as_deref(), Some(&b"B"[..]));
        assert_eq!(snapshots[2].as_deref(), Some(&b"C"[..]));
        committed(transaction.commit());

        // A preexisting item and duplicate keys force the exact indexed
        // fallback. Mutations remain visible to later duplicate positions,
        // and a miss may become live within the same fused call.
        let repeated = [hit_a, hit_a, miss];
        let mut transaction = worker.begin().unwrap();
        assert_eq!(
            table
                .get_inner(&mut transaction, None, &hit_c)
                .unwrap()
                .as_deref(),
            Some(&b"C"[..])
        );
        let snapshots = table
            .modify_fixed_inner(
                &mut transaction,
                None,
                &repeated,
                &mut batch,
                |index, current| match index {
                    0 => {
                        assert_eq!(current.map(Value::as_ref), Some(&b"A2"[..]));
                        PointMutation::Put(Value::from(&b"A3"[..]))
                    }
                    1 => {
                        assert_eq!(current.map(Value::as_ref), Some(&b"A3"[..]));
                        PointMutation::Remove
                    }
                    2 => {
                        assert!(current.is_none());
                        PointMutation::Put(Value::from(&b"M"[..]))
                    }
                    _ => unreachable!(),
                },
                |batch| {
                    for key in &repeated {
                        batch.push_record_id(table.shared().lookup(None, key)?);
                    }
                    Ok(())
                },
            )
            .unwrap();
        assert_eq!(snapshots[0].as_deref(), Some(&b"A2"[..]));
        assert_eq!(snapshots[1].as_deref(), Some(&b"A3"[..]));
        assert_eq!(snapshots[2], None);
        committed(transaction.commit());

        let mut verify = worker.begin().unwrap();
        assert_eq!(table.get_inner(&mut verify, None, &hit_a).unwrap(), None);
        assert_eq!(table.get_inner(&mut verify, None, &hit_b).unwrap(), None);
        assert_eq!(
            table
                .get_inner(&mut verify, None, &hit_c)
                .unwrap()
                .as_deref(),
            Some(&b"C"[..])
        );
        assert_eq!(
            table
                .get_inner(&mut verify, None, &miss)
                .unwrap()
                .as_deref(),
            Some(&b"M"[..])
        );
        committed(verify.commit());
    }

    #[test]
    fn large_fixed_batches_reuse_hashed_proof_and_preserve_duplicate_order() {
        const KEY_COUNT: usize = 300;
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let mut keys: Vec<[u8; 8]> = (0..KEY_COUNT as u64)
            .map(|value| (value + 1).to_be_bytes())
            .collect();

        let mut load = worker.begin().unwrap();
        for key in &keys {
            assert_eq!(
                table
                    .put_inner(&mut load, None, key, Value::from(key))
                    .unwrap(),
                None
            );
        }
        committed(load.commit());

        let mut batch = PointReadBatch::with_capacity(KEY_COUNT + 1);
        let alias_allocation = batch.alias_order.as_ptr();
        let unique_index_capacity = batch.unique_key_index.capacity();
        let retained_capacity = batch.capacity();
        for _ in 0..2 {
            let mut visits = 0;
            let mut transaction = worker.begin().unwrap();
            assert_eq!(
                table
                    .visit_fixed_bytes_inner(
                        &mut transaction,
                        None,
                        &keys,
                        &mut batch,
                        |index, current| {
                            visits += 1;
                            assert_eq!(current, Some(&keys[index][..]));
                        },
                        |batch| {
                            for key in &keys {
                                batch.push_record_id(table.shared().lookup(None, key)?);
                            }
                            Ok(())
                        },
                    )
                    .unwrap(),
                KEY_COUNT
            );
            assert_eq!(visits, KEY_COUNT);
            assert!(batch.alias_order.is_empty());
            assert_eq!(batch.alias_order.as_ptr(), alias_allocation);
            assert_eq!(batch.unique_key_index.capacity(), unique_index_capacity);
            assert_eq!(batch.capacity(), retained_capacity);
            committed(transaction.commit());
            keys.reverse();
        }

        // A repeated key in a large batch rejects the direct unique lane and
        // retains ordinary sequential semantics: the later occurrence sees
        // the mutation staged by the first occurrence.
        let repeated_key = keys[0];
        keys.push(repeated_key);
        let mut callbacks = 0;
        let mut transaction = worker.begin().unwrap();
        assert_eq!(
            table
                .modify_fixed_visit_inner::<false, 8>(
                    &mut transaction,
                    None,
                    &keys,
                    &mut batch,
                    |index, current| {
                        callbacks += 1;
                        Ok(if index == 0 {
                            assert_eq!(current.map(Value::as_ref), Some(&repeated_key[..]));
                            PointMutation::Put(Value::from(&b"first"[..]))
                        } else if index == KEY_COUNT {
                            assert_eq!(current.map(Value::as_ref), Some(&b"first"[..]));
                            PointMutation::Put(Value::from(&b"last"[..]))
                        } else {
                            PointMutation::Keep
                        })
                    },
                    |batch| {
                        for key in &keys {
                            batch.push_record_id(table.shared().lookup(None, key)?);
                        }
                        Ok(())
                    },
                )
                .unwrap(),
            KEY_COUNT + 1
        );
        assert_eq!(callbacks, KEY_COUNT + 1);
        assert_eq!(batch.alias_order.as_ptr(), alias_allocation);
        assert_eq!(batch.unique_key_index.capacity(), unique_index_capacity);
        assert_eq!(batch.capacity(), retained_capacity);
        committed(transaction.commit());

        let mut verify = worker.begin().unwrap();
        assert_eq!(
            table
                .get_inner(&mut verify, None, &repeated_key)
                .unwrap()
                .as_deref(),
            Some(&b"last"[..])
        );
        committed(verify.commit());
    }

    #[test]
    fn large_hashed_fixed_alias_fault_is_callback_free() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let seeded = 1_u64.to_be_bytes();
        seed(&table, &mut worker, &[(&seeded, b"value")]);
        let record_id = table.shared().lookup(None, &seeded).unwrap().unwrap();
        let keys: Vec<[u8; 8]> = (1..=33_u64).map(u64::to_be_bytes).collect();
        let mut batch = PointReadBatch::with_capacity(keys.len());
        let mut callbacks = 0;
        let mut transaction = worker.begin().unwrap();
        let error = table
            .visit_fixed_inner::<false, 8>(
                &mut transaction,
                None,
                &keys,
                &mut batch,
                |_index, _current| callbacks += 1,
                |batch| {
                    for _ in &keys {
                        batch.push_record_id(Some(record_id));
                    }
                    Ok(())
                },
            )
            .unwrap_err();

        assert!(matches!(error, AccessError::Fault(_)));
        assert_eq!(callbacks, 0);
        assert!(batch.record_ids.is_empty());
        assert!(batch.item_keys.is_empty());
        assert!(batch.alias_order.is_empty());
        assert!(transaction.is_doomed());
        assert_eq!(table.health(), TableHealth::Poisoned);
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Poisoned);
        transaction.abort();
    }

    #[test]
    fn fixed_point_mutations_preintern_unique_misses_before_direct_batch_callbacks() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let hit = [0x00, 0x11, 0xaa, 0x01];
        let miss_a = [0x00, 0x22, 0xbb, 0x02];
        let miss_b = [0x00, 0x33, 0xcc, 0x03];
        seed(&table, &mut worker, &[(&hit, b"old")]);

        let keys = [miss_a, hit, miss_b];
        let mut batch = PointReadBatch::with_capacity(keys.len());
        let retained_capacity = batch.capacity();
        let mut observed = Vec::new();
        let mut transaction = worker.begin().unwrap();
        let visited = table
            .modify_fixed_visit_inner::<false, 4>(
                &mut transaction,
                None,
                &keys,
                &mut batch,
                |index, current| {
                    observed.push((index, current.map(|value| value.as_ref().to_vec())));
                    Ok(PointMutation::Put(Value::from(match index {
                        0 => &b"new-a"[..],
                        1 => &b"new-hit"[..],
                        2 => &b"new-b"[..],
                        _ => unreachable!(),
                    })))
                },
                |batch| {
                    for key in &keys {
                        batch.push_record_id(table.shared().lookup(None, key)?);
                    }
                    Ok(())
                },
            )
            .unwrap();
        assert_eq!(visited, keys.len());
        assert_eq!(
            observed,
            [(0, None), (1, Some(b"old".to_vec())), (2, None),]
        );
        assert_eq!(batch.capacity(), retained_capacity);
        assert!(batch.results().is_empty());
        committed(transaction.commit());

        let mut verify = worker.begin().unwrap();
        for (key, expected) in [
            (&miss_a[..], &b"new-a"[..]),
            (&hit[..], &b"new-hit"[..]),
            (&miss_b[..], &b"new-b"[..]),
        ] {
            assert_eq!(
                table.get_inner(&mut verify, None, key).unwrap().as_deref(),
                Some(expected)
            );
        }
        committed(verify.commit());
    }

    #[test]
    fn duplicate_expected_absent_mutations_use_sequential_read_your_writes() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let key = [0x10, 0x20, 0x30, 0x40];
        let keys = [key, key];
        let mut batch = PointReadBatch::new();
        let mut observed = Vec::new();
        let mut transaction = worker.begin().unwrap();

        let visited = table
            .modify_fixed_visit_inner::<false, 4>(
                &mut transaction,
                None,
                &keys,
                &mut batch,
                |index, current| {
                    observed.push((index, current.map(|value| value.as_ref().to_vec())));
                    Ok(PointMutation::Put(Value::from(match index {
                        0 => &b"first"[..],
                        1 => &b"second"[..],
                        _ => unreachable!(),
                    })))
                },
                |batch| {
                    batch.record_ids.resize(keys.len(), None);
                    Ok(())
                },
            )
            .unwrap();

        assert_eq!(visited, keys.len());
        assert_eq!(observed, [(0, None), (1, Some(b"first".to_vec()))]);
        assert_eq!(table.usage().consumed_record_ids(), 1);
        committed(transaction.commit());

        let mut verify = worker.begin().unwrap();
        assert_eq!(
            table.get_inner(&mut verify, None, &key).unwrap().as_deref(),
            Some(&b"second"[..])
        );
        committed(verify.commit());
    }

    #[test]
    fn fixed_point_unique_miss_preintern_failure_is_callback_free_and_retryable() {
        let config = TableConfig::new()
            .with_max_retained_records(2)
            .with_max_retained_key_bytes(8)
            .with_max_consumed_record_ids(2);
        let (runtime, table) = runtime_and_table(config);
        let mut worker = runtime.attach().unwrap();
        let prior = [0x01, 0x02, 0x03, 0x04];
        let first = [0x10, 0x20, 0x30, 0x40];
        let second = [0x50, 0x60, 0x70, 0x80];
        seed(&table, &mut worker, &[(&prior, b"prior")]);
        let keys = [first, second];
        let mut batch = PointReadBatch::with_capacity(keys.len());
        let retained_capacity = batch.capacity();
        let mut callbacks = 0;

        let mut transaction = worker.begin().unwrap();
        assert_eq!(
            table
                .get_inner(&mut transaction, None, &prior)
                .unwrap()
                .as_deref(),
            Some(&b"prior"[..])
        );
        let error = table
            .modify_fixed_visit_inner::<false, 4>(
                &mut transaction,
                None,
                &keys,
                &mut batch,
                |_index, _current| {
                    callbacks += 1;
                    Ok(PointMutation::Put(Value::from(&b"never-staged"[..])))
                },
                |batch| {
                    for key in &keys {
                        batch.push_record_id(table.shared().lookup(None, key)?);
                    }
                    Ok(())
                },
            )
            .unwrap_err();
        assert!(matches!(error, AccessError::Capacity(_)));
        assert_eq!(callbacks, 0);
        assert!(transaction.is_doomed());
        assert!(batch.record_ids.is_empty());
        assert!(batch.item_keys.is_empty());
        assert_eq!(batch.capacity(), retained_capacity);
        assert_eq!(
            table.usage(),
            TableUsage {
                retained_records: 2,
                retained_key_bytes: 8,
                consumed_record_ids: 2,
            }
        );
        transaction.abort();

        // The first physical binding survived as a tombstone. A fresh
        // transaction can reuse it without consuming another ID; the second
        // key remains over capacity and fails closed.
        let mut retry = worker.begin().unwrap();
        assert_eq!(
            table
                .put_inner(&mut retry, None, &first, Value::from(&b"retry"[..]))
                .unwrap(),
            None
        );
        committed(retry.commit());
        let mut exhausted = worker.begin().unwrap();
        assert!(matches!(
            table.put_inner(
                &mut exhausted,
                None,
                &second,
                Value::from(&b"still-full"[..])
            ),
            Err(AccessError::Capacity(_))
        ));
        exhausted.abort();
    }

    #[test]
    fn mixed_fixed_mutation_rejects_foreign_alias_before_intern_or_callbacks() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let first = [0x10, 0x20, 0x30, 0x40];
        let second = [0x50, 0x60, 0x70, 0x80];
        let missing = [0x90, 0xa0, 0xb0, 0xc0];
        seed(&table, &mut worker, &[(&first, b"value")]);
        let record_id = table.shared().lookup(None, &first).unwrap().unwrap();
        let usage_before = table.usage();
        let keys = [first, second, missing];
        let mut batch = PointReadBatch::with_capacity(keys.len());
        let mut callbacks = 0;
        let mut transaction = worker.begin().unwrap();
        let error = table
            .modify_fixed_visit_inner::<false, 4>(
                &mut transaction,
                None,
                &keys,
                &mut batch,
                |_index, _current| {
                    callbacks += 1;
                    Ok(PointMutation::Remove)
                },
                |batch| {
                    batch.push_record_id(Some(record_id));
                    batch.push_record_id(Some(record_id));
                    batch.push_record_id(None);
                    Ok(())
                },
            )
            .unwrap_err();

        assert!(matches!(error, AccessError::Fault(_)));
        assert_eq!(callbacks, 0);
        assert!(batch.record_ids.is_empty());
        assert!(transaction.is_doomed());
        assert_eq!(table.usage(), usage_before);
        assert_eq!(table.health(), TableHealth::Poisoned);
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Poisoned);
        transaction.abort();
    }

    #[test]
    fn fixed_point_visitors_preserve_order_without_retaining_snapshots() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let hit_a = [0x00, 0x11, 0xaa, 0x01];
        let hit_b = [0x00, 0x22, 0xbb, 0x02];
        let miss = [0x00, 0x33, 0xcc, 0x03];
        seed(&table, &mut worker, &[(&hit_a, b"A"), (&hit_b, b"B")]);

        // A fresh visitor batch must not allocate or retain the owning result
        // vector. The unique all-hit request still selects the typed batch.
        let mut batch = PointReadBatch::new();
        assert_eq!(batch.values.capacity(), 0);
        let unique = [hit_b, hit_a];
        let mut observed = Vec::new();
        let mut transaction = worker.begin().unwrap();
        let visited = table
            .visit_fixed_inner::<false, 4>(
                &mut transaction,
                None,
                &unique,
                &mut batch,
                |index, current| {
                    observed.push((index, current.map(|value| value.as_ref().to_vec())));
                },
                |batch| {
                    for key in &unique {
                        batch.push_record_id(table.shared().lookup(None, key)?);
                    }
                    Ok(())
                },
            )
            .unwrap();
        assert_eq!(visited, unique.len());
        assert_eq!(
            observed,
            vec![(0, Some(b"B".to_vec())), (1, Some(b"A".to_vec()))]
        );
        assert!(batch.results().is_empty());
        assert_eq!(batch.values.capacity(), 0);
        committed(transaction.commit());

        // Duplicate keys force the ordinary indexed fallback. Later positions
        // see earlier intents. The missing key is physically interned exactly
        // once even though its staged live state returns to a tombstone.
        let generation_before = table.shared().directory_generation.load(Ordering::Acquire);
        let hit_a_id = table.shared().lookup(None, &hit_a).unwrap().unwrap();
        let hit_b_id = table.shared().lookup(None, &hit_b).unwrap().unwrap();
        let hit_a_before = table
            .shared()
            .resolve_directory_record(hit_a_id)
            .unwrap()
            .version
            .observe()
            .unwrap();
        let hit_b_before = table
            .shared()
            .resolve_directory_record(hit_b_id)
            .unwrap()
            .version
            .observe()
            .unwrap();
        let repeated = [hit_a, hit_a, hit_b, hit_b, miss, miss];
        let mut observed = Vec::new();
        let mut transaction = worker.begin().unwrap();
        let visited = table
            .modify_fixed_visit_inner::<false, 4>(
                &mut transaction,
                None,
                &repeated,
                &mut batch,
                |index, current| {
                    observed.push((index, current.map(|value| value.as_ref().to_vec())));
                    Ok(match index {
                        0 => PointMutation::Put(Value::from(&b"A2"[..])),
                        1 => PointMutation::Put(Value::from(&b"A"[..])),
                        2 => PointMutation::Put(Value::from(&b"B2"[..])),
                        3 => PointMutation::Put(Value::from(&b"B3"[..])),
                        4 => PointMutation::Put(Value::from(&b"M"[..])),
                        5 => PointMutation::Remove,
                        _ => unreachable!(),
                    })
                },
                |batch| {
                    for key in &repeated {
                        batch.push_record_id(table.shared().lookup(None, key)?);
                    }
                    Ok(())
                },
            )
            .unwrap();
        assert_eq!(visited, repeated.len());
        assert_eq!(
            observed,
            vec![
                (0, Some(b"A".to_vec())),
                (1, Some(b"A2".to_vec())),
                (2, Some(b"B".to_vec())),
                (3, Some(b"B2".to_vec())),
                (4, None),
                (5, Some(b"M".to_vec())),
            ]
        );
        assert!(batch.results().is_empty());
        assert_eq!(batch.values.capacity(), 0);
        let miss_id = table.shared().lookup(None, &miss).unwrap().unwrap();
        let miss_before = table
            .shared()
            .resolve_directory_record(miss_id)
            .unwrap()
            .version
            .observe()
            .unwrap();
        committed(transaction.commit());
        assert_eq!(
            table.shared().directory_generation.load(Ordering::Acquire),
            generation_before + 1
        );

        let mut verify = worker.begin().unwrap();
        assert_eq!(
            table
                .get_inner(&mut verify, None, &hit_a)
                .unwrap()
                .as_deref(),
            Some(&b"A"[..])
        );
        assert_eq!(
            table
                .get_inner(&mut verify, None, &hit_b)
                .unwrap()
                .as_deref(),
            Some(&b"B3"[..])
        );
        assert_eq!(table.get_inner(&mut verify, None, &miss).unwrap(), None);
        committed(verify.commit());
        assert!(
            table
                .shared()
                .resolve_directory_record(hit_a_id)
                .unwrap()
                .version
                .observe()
                .unwrap()
                > hit_a_before
        );
        assert!(
            table
                .shared()
                .resolve_directory_record(hit_b_id)
                .unwrap()
                .version
                .observe()
                .unwrap()
                > hit_b_before
        );
        assert!(
            table
                .shared()
                .resolve_directory_record(miss_id)
                .unwrap()
                .version
                .observe()
                .unwrap()
                > miss_before
        );
    }

    #[test]
    fn fixed_byte_visitors_borrow_shared_storage_and_preserve_fallback_semantics() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let inline_key = [0x10, 0x20, 0x30, 0x40];
        let shared_key = [0x50, 0x60, 0x70, 0x80];
        let missing_key = [0x90, 0xa0, 0xb0, 0xc0];
        let shared_value = vec![0xa7; INLINE_VALUE_CAPACITY + 31];
        seed(
            &table,
            &mut worker,
            &[
                (&inline_key, &b"inline"[..]),
                (&shared_key, shared_value.as_slice()),
            ],
        );

        let shared_id = table.shared().lookup(None, &shared_key).unwrap().unwrap();
        let shared_record = table.shared().resolve_directory_record(shared_id).unwrap();
        let retained = shared_record.state.shared.load_full().unwrap();
        let retained_strong_count = Arc::strong_count(&retained);

        let keys = [inline_key, shared_key];
        let mut batch = PointReadBatch::with_capacity(3);
        let mut observed = Vec::new();
        let mut transaction = worker.begin().unwrap();
        let visited = table
            .visit_fixed_bytes_inner(
                &mut transaction,
                None,
                &keys,
                &mut batch,
                |index, current| {
                    if index == 1 {
                        assert_eq!(Arc::strong_count(&retained), retained_strong_count);
                    }
                    observed.push((index, current.map(<[u8]>::to_vec)));
                },
                |batch| {
                    for key in &keys {
                        batch.push_record_id(table.shared().lookup(None, key)?);
                    }
                    Ok(())
                },
            )
            .unwrap();
        assert_eq!(visited, keys.len());
        assert_eq!(
            observed,
            [
                (0, Some(b"inline".to_vec())),
                (1, Some(shared_value.clone())),
            ]
        );
        assert_eq!(Arc::strong_count(&retained), retained_strong_count);
        assert!(batch.results().is_empty());
        committed(transaction.commit());

        // A preexisting staged item and duplicate keys select the scalar
        // indexed fallback. Both duplicate positions observe the staged bytes,
        // while the absent key remains a null visitor value.
        let staged = vec![0x5c; INLINE_VALUE_CAPACITY + 7];
        let repeated = [shared_key, shared_key, missing_key];
        observed.clear();
        let mut transaction = worker.begin().unwrap();
        table
            .put_inner(
                &mut transaction,
                None,
                &shared_key,
                Value::from(staged.as_slice()),
            )
            .unwrap();
        assert_eq!(
            table
                .visit_fixed_bytes_inner(
                    &mut transaction,
                    None,
                    &repeated,
                    &mut batch,
                    |index, current| {
                        observed.push((index, current.map(<[u8]>::to_vec)));
                    },
                    |batch| {
                        for key in &repeated {
                            batch.push_record_id(table.shared().lookup(None, key)?);
                        }
                        Ok(())
                    },
                )
                .unwrap(),
            repeated.len()
        );
        assert_eq!(
            observed,
            [(0, Some(staged.clone())), (1, Some(staged)), (2, None),]
        );
        assert!(batch.results().is_empty());
        transaction.abort();
    }

    #[test]
    fn nonempty_fixed_visit_rejects_exact_foreign_directory_aliases() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let first = [0x10, 0x20, 0x30, 0x40];
        let second = [0x50, 0x60, 0x70, 0x80];
        seed(&table, &mut worker, &[(&first, b"value")]);
        let record_id = table.shared().lookup(None, &first).unwrap().unwrap();

        let mut transaction = worker.begin().unwrap();
        assert_eq!(
            table
                .get_inner(&mut transaction, None, &first)
                .unwrap()
                .as_deref(),
            Some(&b"value"[..])
        );
        let keys = [first, second];
        let mut batch = PointReadBatch::with_capacity(keys.len());
        let mut visits = 0;
        let error = table
            .visit_fixed_inner::<false, 4>(
                &mut transaction,
                None,
                &keys,
                &mut batch,
                |_index, _current| visits += 1,
                |batch| {
                    batch.push_record_id(Some(record_id));
                    batch.push_record_id(Some(record_id));
                    Ok(())
                },
            )
            .unwrap_err();

        assert!(matches!(error, AccessError::Fault(_)));
        assert_eq!(visits, 0);
        assert!(batch.record_ids.is_empty());
        assert!(batch.alias_order.is_empty());
        assert!(transaction.is_doomed());
        assert_eq!(table.health(), TableHealth::Poisoned);
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Poisoned);
        transaction.abort();
    }

    #[test]
    fn nonempty_fixed_mutation_rejects_foreign_alias_before_callbacks() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let first = [0x10, 0x20, 0x30, 0x40];
        let second = [0x50, 0x60, 0x70, 0x80];
        seed(&table, &mut worker, &[(&first, b"value")]);
        let record_id = table.shared().lookup(None, &first).unwrap().unwrap();

        let mut transaction = worker.begin().unwrap();
        assert_eq!(
            table
                .get_inner(&mut transaction, None, &first)
                .unwrap()
                .as_deref(),
            Some(&b"value"[..])
        );
        let keys = [first, second];
        let mut batch = PointReadBatch::with_capacity(keys.len());
        let mut callbacks = 0;
        let error = table
            .modify_fixed_visit_inner::<false, 4>(
                &mut transaction,
                None,
                &keys,
                &mut batch,
                |_index, _current| {
                    callbacks += 1;
                    Ok(PointMutation::Remove)
                },
                |batch| {
                    batch.push_record_id(Some(record_id));
                    batch.push_record_id(Some(record_id));
                    Ok(())
                },
            )
            .unwrap_err();

        assert!(matches!(error, AccessError::Fault(_)));
        assert_eq!(callbacks, 0);
        assert!(batch.record_ids.is_empty());
        assert!(batch.alias_order.is_empty());
        assert!(transaction.is_doomed());
        assert_eq!(table.health(), TableHealth::Poisoned);
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Poisoned);
        transaction.abort();
    }

    #[test]
    fn terminal_fixed_visit_handles_hits_duplicates_and_miss_retry() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let hit_a = [0x00, 0x11, 0xaa, 0x01];
        let hit_b = [0x00, 0x22, 0xbb, 0x02];
        let miss = [0x00, 0x33, 0xcc, 0x03];
        seed(&table, &mut worker, &[(&hit_a, b"A"), (&hit_b, b"B")]);

        let keys = [hit_b, hit_a, hit_b];
        let mut batch = PointReadBatch::with_capacity(keys.len());
        let retained_capacity = batch.capacity();
        let mut observed = Vec::new();
        let transaction = worker.begin_terminal_read_batch().unwrap();
        let outcome = table
            .visit_fixed_terminal_inner(
                transaction,
                &keys,
                &mut batch,
                |index, current| {
                    observed.push((index, current.map(|value| value.as_ref().to_vec())));
                },
                |batch| {
                    for key in &keys {
                        batch.push_record_id(table.shared().lookup(None, key)?);
                    }
                    Ok(())
                },
            )
            .unwrap();
        let (transaction, visited) = terminal_ready(outcome);
        committed(transaction.commit());
        assert_eq!(visited, keys.len());
        assert_eq!(
            observed,
            vec![
                (0, Some(b"B".to_vec())),
                (1, Some(b"A".to_vec())),
                (2, Some(b"B".to_vec())),
            ]
        );
        assert!(batch.results().is_empty());
        assert_eq!(batch.capacity(), retained_capacity);

        let keys = [hit_a, miss, hit_b];
        let mut visits = 0;
        let transaction = worker.begin_terminal_read_batch().unwrap();
        let outcome = table
            .visit_fixed_terminal_inner(
                transaction,
                &keys,
                &mut batch,
                |_index, _current| visits += 1,
                |batch| {
                    for key in &keys {
                        batch.push_record_id(table.shared().lookup(None, key)?);
                    }
                    Ok(())
                },
            )
            .unwrap();
        terminal_retry(outcome);
        assert_eq!(visits, 0);
        assert!(batch.results().is_empty());
        assert!(batch.record_ids.is_empty());

        // RetryOrdinary has definitely returned the terminal transaction's
        // scratch to the same worker.
        let mut ordinary = worker.begin().unwrap();
        assert_eq!(table.get_inner(&mut ordinary, None, &miss).unwrap(), None);
        ordinary.abort();
    }

    #[test]
    fn terminal_pre_core_retryable_errors_abort_without_poisoning_runtime() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let keys = [[0x10, 0x20]];
        let mut batch = PointReadBatch::with_capacity(keys.len());

        for expected in [
            AccessError::Conflict(Conflict::HiddenLockBusy),
            AccessError::Capacity(CapacityError::BufferLimit),
        ] {
            let transaction = worker.begin_terminal_read_batch().unwrap();
            let mut visits = 0;
            let error = table
                .visit_fixed_terminal_inner(
                    transaction,
                    &keys,
                    &mut batch,
                    |_index, _current| visits += 1,
                    |_batch| Err(expected),
                )
                .unwrap_err();

            assert_eq!(error, expected);
            assert_eq!(visits, 0);
            assert!(batch.is_empty());
            assert!(batch.record_ids.is_empty());
            assert_eq!(runtime.health(), sto_core::RuntimeHealth::Healthy);
            // The consuming error path must have definitely made this worker
            // available for another transaction.
            worker.begin().unwrap().abort();
        }
    }

    #[test]
    fn terminal_pre_core_resolution_fault_poisons_table_and_runtime() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let keys = [[0x30, 0x40]];
        let mut batch = PointReadBatch::with_capacity(keys.len());
        let transaction = worker.begin_terminal_read_batch().unwrap();
        let mut visits = 0;
        let error = table
            .visit_fixed_terminal_inner(
                transaction,
                &keys,
                &mut batch,
                |_index, _current| visits += 1,
                |batch| {
                    // A directory-returned ID whose registry segment was
                    // never allocated fails in the pre-core resolution phase.
                    batch.push_record_id(Some(RecordId::new(1).unwrap()));
                    Ok(())
                },
            )
            .unwrap_err();

        assert!(matches!(error, AccessError::Fault(_)));
        assert_eq!(visits, 0);
        assert!(batch.is_empty());
        assert!(batch.record_ids.is_empty());
        assert_eq!(table.health(), TableHealth::Poisoned);
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Poisoned);
    }

    #[test]
    fn terminal_pre_core_alias_fault_poisons_table_and_runtime() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let first = [0x50, 0x60];
        let second = [0x70, 0x80];
        seed(&table, &mut worker, &[(&first, b"value")]);
        let record_id = table.shared().lookup(None, &first).unwrap().unwrap();
        let keys = [first, second];
        let mut batch = PointReadBatch::with_capacity(keys.len());
        let transaction = worker.begin_terminal_read_batch().unwrap();
        let mut visits = 0;
        let error = table
            .visit_fixed_terminal_inner(
                transaction,
                &keys,
                &mut batch,
                |_index, _current| visits += 1,
                |batch| {
                    batch.push_record_id(Some(record_id));
                    batch.push_record_id(Some(record_id));
                    Ok(())
                },
            )
            .unwrap_err();

        assert!(matches!(error, AccessError::Fault(_)));
        assert_eq!(visits, 0);
        assert!(batch.is_empty());
        assert!(batch.record_ids.is_empty());
        assert_eq!(table.health(), TableHealth::Poisoned);
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Poisoned);
    }

    #[test]
    fn terminal_alias_fingerprint_collision_falls_back_to_exact_ids() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let mut record_ids = Vec::new();
        for index in 0_u64..65 {
            let mut candidate = table
                .shared()
                .registry
                .reserve_candidate(&index.to_le_bytes())
                .unwrap();
            table
                .shared()
                .registry
                .mark_published(&mut candidate)
                .unwrap();
            record_ids.push(candidate.id);
        }
        let first = record_ids[0];
        let second = record_ids[64];
        assert_ne!(first, second);
        assert_eq!((first.get() - 1) & 63, (second.get() - 1) & 63);

        let keys = [[0x81, 0x01], [0x82, 0x02]];
        let mut batch = PointReadBatch::with_capacity(keys.len());
        let transaction = worker.begin_terminal_read_batch().unwrap();
        let mut visits = 0;
        let outcome = table
            .visit_fixed_terminal_inner(
                transaction,
                &keys,
                &mut batch,
                |_index, current| {
                    visits += 1;
                    assert!(current.is_none());
                },
                |batch| {
                    batch.push_record_id(Some(first));
                    batch.push_record_id(Some(second));
                    Ok(())
                },
            )
            .unwrap();
        let (transaction, visited) = terminal_ready(outcome);
        assert_eq!(visited, 2);
        assert_eq!(visits, 2);
        committed(transaction.commit());
        assert_eq!(table.health(), TableHealth::Healthy);
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Healthy);
    }

    #[test]
    fn terminal_fixed_visit_drops_shared_snapshot_before_certification() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let key = [0x44, 0x00, 0x55, 0x00];
        let backing = SharedValue::from_slice(&[0x6d; INLINE_VALUE_CAPACITY + 1]);

        let mut seed = worker.begin().unwrap();
        table
            .put_inner(
                &mut seed,
                None,
                &key,
                Value {
                    repr: ValueRepr::Shared(Arc::clone(&backing)),
                },
            )
            .unwrap();
        committed(seed.commit());
        assert_eq!(Arc::strong_count(&backing), 2);

        let keys = [key];
        let mut batch = PointReadBatch::with_capacity(1);
        let mut visitor_count = 0;
        let transaction = worker.begin_terminal_read_batch().unwrap();
        let outcome = table
            .visit_fixed_terminal_inner(
                transaction,
                &keys,
                &mut batch,
                |_index, current| {
                    visitor_count += 1;
                    assert_eq!(current.map(Value::as_ref), Some(backing.as_bytes()));
                    assert_eq!(Arc::strong_count(&backing), 3);
                },
                |batch| {
                    batch.push_record_id(table.shared().lookup(None, &key)?);
                    Ok(())
                },
            )
            .unwrap();
        let (transaction, _) = terminal_ready(outcome);
        assert_eq!(visitor_count, 1);
        assert_eq!(Arc::strong_count(&backing), 2);
        committed(transaction.commit());
        assert_eq!(Arc::strong_count(&backing), 2);
    }

    #[test]
    fn terminal_fixed_visit_detects_a_write_before_final_validation() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut reader = runtime.attach().unwrap();
        let key = [0x66, 0x00, 0x77, 0x00];
        seed(&table, &mut reader, &[(&key, b"before")]);

        let keys = [key];
        let mut batch = PointReadBatch::with_capacity(1);
        let transaction = reader.begin_terminal_read_batch().unwrap();
        let outcome = table
            .visit_fixed_terminal_inner(
                transaction,
                &keys,
                &mut batch,
                |_index, current| {
                    assert_eq!(current.map(Value::as_ref), Some(&b"before"[..]));
                },
                |batch| {
                    batch.push_record_id(table.shared().lookup(None, &key)?);
                    Ok(())
                },
            )
            .unwrap();
        let (transaction, _) = terminal_ready(outcome);

        std::thread::scope(|scope| {
            scope
                .spawn(|| {
                    let mut writer = runtime.attach().unwrap();
                    let mut update = writer.begin().unwrap();
                    table
                        .put_inner(&mut update, None, &key, Value::from(&b"after"[..]))
                        .unwrap();
                    committed(update.commit());
                })
                .join()
                .unwrap();
        });

        assert!(matches!(
            transaction.commit(),
            Ok(CommitOutcome::Aborted(AbortReason::Conflict(
                Conflict::ReadValidation
            )))
        ));
        let ordinary = reader.begin().unwrap();
        ordinary.abort();
    }

    #[test]
    fn fixed_point_batch_error_clears_results_and_dooms_transaction() {
        let runtime = Runtime::new(RuntimeConfig::new().with_max_items_per_transaction(2)).unwrap();
        let table = Table::new_memory(&runtime, TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let keys = [[0x00, 0x10], [0xff, 0x00], [0x7f, 0x00]];
        for (index, key) in keys.iter().enumerate() {
            let value = [index as u8];
            let mut transaction = worker.begin().unwrap();
            table
                .put_inner(&mut transaction, None, key, Value::from(&value))
                .unwrap();
            committed(transaction.commit());
        }

        let mut batch = PointReadBatch::with_capacity(keys.len());
        let mut transaction = worker.begin().unwrap();
        let error = table
            .get_fixed_inner(&mut transaction, None, &keys, &mut batch, |batch| {
                for key in &keys {
                    batch.push_record_id(table.shared().lookup(None, key)?);
                }
                Ok(())
            })
            .unwrap_err();
        assert_eq!(error, AccessError::Capacity(CapacityError::ItemLimit));
        assert!(transaction.is_doomed());
        assert!(batch.is_empty());
        assert!(batch.record_ids.is_empty());
        assert!(batch.directory_results.is_empty());
        transaction.abort();

        // The unique typed lane checks whole-batch capacity before invoking a
        // visitor, and the no-capture path preserves the same doom/cleanup
        // boundary as the owning API.
        let mut visits = 0;
        let mut transaction = worker.begin().unwrap();
        let error = table
            .visit_fixed_inner::<false, 2>(
                &mut transaction,
                None,
                &keys,
                &mut batch,
                |_index, _current| visits += 1,
                |batch| {
                    for key in &keys {
                        batch.push_record_id(table.shared().lookup(None, key)?);
                    }
                    Ok(())
                },
            )
            .unwrap_err();
        assert_eq!(error, AccessError::Capacity(CapacityError::ItemLimit));
        assert_eq!(visits, 0);
        assert!(transaction.is_doomed());
        assert!(batch.is_empty());
        assert!(batch.record_ids.is_empty());
        assert!(batch.directory_results.is_empty());
        transaction.abort();
    }

    #[test]
    fn fixed_point_visitor_unwind_leaves_the_transaction_doomed() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let key = [0x42, 0x00];
        seed(&table, &mut worker, &[(&key, b"value")]);

        let mut batch = PointReadBatch::with_capacity(1);
        let mut transaction = worker.begin().unwrap();
        let unwind = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            let _ = table.visit_fixed_inner::<false, 2>(
                &mut transaction,
                None,
                &[key],
                &mut batch,
                |_index, _current| panic!("injected visitor panic"),
                |batch| {
                    batch.push_record_id(table.shared().lookup(None, &key)?);
                    Ok(())
                },
            );
        }));
        assert!(unwind.is_err());
        assert!(transaction.is_doomed());
        transaction.abort();
    }

    #[test]
    fn committed_state_transitions_inline_shared_and_tombstone_exactly() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let key = b"state/transitions";

        let mut intern = worker.begin().unwrap();
        assert_eq!(table.get_inner(&mut intern, None, key), Ok(None));
        committed(intern.commit());
        let record_id = table.shared().lookup(None, key).unwrap().unwrap();
        let record = table.shared().resolve_directory_record(record_id).unwrap();
        assert!(record.state.shared.load_full().is_none());
        assert_eq!(committed_record_snapshot(record), RecordState::tombstone());

        // A live empty value must not collapse into the tombstone descriptor.
        let mut write_empty = worker.begin().unwrap();
        table
            .put_inner(&mut write_empty, None, key, Value::default())
            .unwrap();
        committed(write_empty.commit());
        assert_eq!(
            record_state_descriptor(record.state.tail_and_descriptor.load(Ordering::Acquire)),
            RECORD_STATE_INLINE_BASE
        );
        let empty_state = committed_record_snapshot(record);
        assert_eq!(empty_state.value().unwrap().as_ref(), b"");

        let mut remove_empty = worker.begin().unwrap();
        assert_eq!(
            table.remove_inner(&mut remove_empty, None, key).unwrap(),
            Some(Value::default())
        );
        committed(remove_empty.commit());
        assert_eq!(committed_record_snapshot(record), RecordState::tombstone());

        let inline = [0x11_u8; INLINE_VALUE_CAPACITY];
        let mut write_inline = worker.begin().unwrap();
        table
            .put_inner(&mut write_inline, None, key, Value::from(&inline))
            .unwrap();
        committed(write_inline.commit());
        let inline_state = committed_record_snapshot(record);
        let RecordState::Live(inline_value) = &inline_state else {
            panic!("the committed inline value became a tombstone");
        };
        assert!(matches!(inline_value.repr, ValueRepr::Inline { .. }));
        assert_eq!(inline_value.as_ref(), inline);
        assert!(record.state.shared.load_full().is_none());

        let shared = vec![0x22_u8; INLINE_VALUE_CAPACITY + 1];
        let mut write_shared = worker.begin().unwrap();
        table
            .put_inner(&mut write_shared, None, key, Value::from(shared.clone()))
            .unwrap();
        committed(write_shared.commit());
        let shared_state = committed_record_snapshot(record);
        let RecordState::Live(shared_value) = &shared_state else {
            panic!("the committed shared value became a tombstone");
        };
        assert!(matches!(shared_value.repr, ValueRepr::Shared(_)));
        assert_eq!(shared_value.as_ref(), shared);
        assert!(record.state.shared.load_full().is_some());

        let mut remove = worker.begin().unwrap();
        assert_eq!(
            table
                .remove_inner(&mut remove, None, key)
                .unwrap()
                .as_deref(),
            Some(shared.as_slice())
        );
        committed(remove.commit());
        assert_eq!(committed_record_snapshot(record), RecordState::tombstone());
        assert!(record.state.shared.load_full().is_none());

        let mut write_inline_again = worker.begin().unwrap();
        table
            .put_inner(&mut write_inline_again, None, key, Value::from(&inline))
            .unwrap();
        committed(write_inline_again.commit());
        assert_eq!(
            committed_record_snapshot(record).value().unwrap().as_ref(),
            inline
        );
    }

    #[test]
    fn concurrent_inline_and_shared_publications_never_expose_torn_bytes() {
        const WRITES: usize = 2_000;
        const MIN_READS: usize = 2_000;
        const INLINE_A: [u8; INLINE_VALUE_CAPACITY] = [0x31; INLINE_VALUE_CAPACITY];
        const INLINE_B: [u8; INLINE_VALUE_CAPACITY] = [0xc7; INLINE_VALUE_CAPACITY];
        const SHARED: [u8; INLINE_VALUE_CAPACITY + 1] = [0x5a; INLINE_VALUE_CAPACITY + 1];

        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut setup_worker = runtime.attach().unwrap();
        let mut setup = setup_worker.begin().unwrap();
        table
            .put_inner(&mut setup, None, b"state/race", Value::from(&INLINE_A))
            .unwrap();
        committed(setup.commit());
        drop(setup_worker);

        let barrier = Arc::new(Barrier::new(2));
        let finished = Arc::new(AtomicBool::new(false));
        let mut reader_worker = runtime.attach().unwrap();
        std::thread::scope(|scope| {
            let writer_runtime = Arc::clone(&runtime);
            let writer_table = table.clone();
            let writer_barrier = Arc::clone(&barrier);
            let writer_finished = Arc::clone(&finished);
            let writer = scope.spawn(move || {
                let mut worker = writer_runtime.attach().unwrap();
                writer_barrier.wait();
                for iteration in 0..WRITES {
                    loop {
                        let mut txn = worker.begin().unwrap();
                        let operation = match iteration % 4 {
                            0 => writer_table
                                .put_inner(&mut txn, None, b"state/race", Value::from(&INLINE_A))
                                .map(|_| ()),
                            1 => writer_table
                                .put_inner(&mut txn, None, b"state/race", Value::from(&INLINE_B))
                                .map(|_| ()),
                            2 => writer_table
                                .put_inner(&mut txn, None, b"state/race", Value::from(&SHARED))
                                .map(|_| ()),
                            _ => writer_table
                                .remove_inner(&mut txn, None, b"state/race")
                                .map(|_| ()),
                        };
                        match operation {
                            Ok(()) => {}
                            Err(AccessError::Conflict(_)) => {
                                txn.abort();
                                continue;
                            }
                            Err(error) => panic!("concurrent writer access failed: {error:?}"),
                        }
                        match txn.commit().unwrap() {
                            CommitOutcome::Committed(_) => break,
                            CommitOutcome::Aborted(AbortReason::Conflict(_)) => continue,
                            CommitOutcome::Aborted(reason) => {
                                panic!("concurrent writer aborted: {reason:?}")
                            }
                        }
                    }
                }
                writer_finished.store(true, Ordering::Release);
            });

            barrier.wait();
            let mut reads = 0_usize;
            while !finished.load(Ordering::Acquire) || reads < MIN_READS {
                let mut txn = reader_worker.begin().unwrap();
                let valid = if reads.is_multiple_of(2) {
                    table.get_inner(&mut txn, None, b"state/race").map(|value| {
                        value.as_ref().is_none_or(|value| {
                            value.as_ref() == INLINE_A
                                || value.as_ref() == INLINE_B
                                || value.as_ref() == SHARED
                        })
                    })
                } else {
                    let mut output = [0_u8; INLINE_VALUE_CAPACITY + 1];
                    table
                        .copy_get_resolving_inner(&mut txn, None, b"state/race", &mut output)
                        .map(|(outcome, _resolved)| match outcome {
                            ValueCopyOutcome::Miss => true,
                            ValueCopyOutcome::Copied { len } => {
                                output[..len] == INLINE_A
                                    || output[..len] == INLINE_B
                                    || output[..len] == SHARED
                            }
                            ValueCopyOutcome::BufferTooSmall { .. } => false,
                        })
                };
                match valid {
                    Ok(valid) => {
                        assert!(valid, "reader observed torn committed bytes");
                        assert!(matches!(
                            txn.commit().unwrap(),
                            CommitOutcome::Committed(_)
                                | CommitOutcome::Aborted(AbortReason::Conflict(_))
                        ));
                    }
                    Err(AccessError::Conflict(_)) => {
                        txn.abort();
                    }
                    Err(error) => panic!("concurrent reader access failed: {error:?}"),
                }
                reads += 1;
            }
            writer.join().unwrap();
        });
    }

    #[test]
    fn concurrent_bounded_publications_never_expose_torn_bytes() {
        const WRITES: usize = 1_000;
        const MIN_READS: usize = 1_000;
        const STABLE_A: [u8; 93] = [0x2d; 93];
        const STABLE_B: [u8; STABLE_ATOMIC_VALUE_CAPACITY] = [0xd2; STABLE_ATOMIC_VALUE_CAPACITY];

        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let table = Table::new_memory_direct(&runtime, bounded_value_config());
        let mut setup_worker = runtime.attach().unwrap();
        let mut setup = setup_worker.begin().unwrap();
        table
            .put_inner(
                &mut setup,
                None,
                b"bounded/race",
                table.staging_value(&STABLE_A),
            )
            .unwrap();
        committed(setup.commit());
        drop(setup_worker);

        let barrier = Arc::new(Barrier::new(2));
        let finished = Arc::new(AtomicBool::new(false));
        let mut reader_worker = runtime.attach().unwrap();
        std::thread::scope(|scope| {
            let writer_runtime = Arc::clone(&runtime);
            let writer_table = table.clone();
            let writer_barrier = Arc::clone(&barrier);
            let writer_finished = Arc::clone(&finished);
            let writer = scope.spawn(move || {
                let mut worker = writer_runtime.attach().unwrap();
                writer_barrier.wait();
                for iteration in 0..WRITES {
                    loop {
                        let mut txn = worker.begin().unwrap();
                        let operation = match iteration % 3 {
                            0 => writer_table
                                .put_inner(
                                    &mut txn,
                                    None,
                                    b"bounded/race",
                                    writer_table.staging_value(&STABLE_A),
                                )
                                .map(|_| ()),
                            1 => writer_table
                                .put_inner(
                                    &mut txn,
                                    None,
                                    b"bounded/race",
                                    writer_table.staging_value(&STABLE_B),
                                )
                                .map(|_| ()),
                            _ => writer_table
                                .remove_inner(&mut txn, None, b"bounded/race")
                                .map(|_| ()),
                        };
                        match operation {
                            Ok(()) => {}
                            Err(AccessError::Conflict(_)) => {
                                txn.abort();
                                continue;
                            }
                            Err(error) => panic!("bounded writer access failed: {error:?}"),
                        }
                        match txn.commit().unwrap() {
                            CommitOutcome::Committed(_) => break,
                            CommitOutcome::Aborted(AbortReason::Conflict(_)) => continue,
                            CommitOutcome::Aborted(reason) => {
                                panic!("bounded writer aborted: {reason:?}")
                            }
                        }
                    }
                }
                writer_finished.store(true, Ordering::Release);
            });

            barrier.wait();
            let mut reads = 0;
            while !finished.load(Ordering::Acquire) || reads < MIN_READS {
                let mut txn = reader_worker.begin().unwrap();
                let mut output = [0_u8; STABLE_ATOMIC_VALUE_CAPACITY];
                let valid = table
                    .copy_get_resolving_inner(&mut txn, None, b"bounded/race", &mut output)
                    .map(|(outcome, _)| match outcome {
                        ValueCopyOutcome::Miss => true,
                        ValueCopyOutcome::Copied { len } => {
                            output[..len] == STABLE_A || output[..len] == STABLE_B
                        }
                        ValueCopyOutcome::BufferTooSmall { .. } => false,
                    });
                match valid {
                    Ok(valid) => {
                        assert!(valid, "reader observed torn bounded bytes");
                        assert!(matches!(
                            txn.commit().unwrap(),
                            CommitOutcome::Committed(_)
                                | CommitOutcome::Aborted(AbortReason::Conflict(_))
                        ));
                    }
                    Err(AccessError::Conflict(_)) => {
                        txn.abort();
                    }
                    Err(error) => panic!("bounded reader access failed: {error:?}"),
                }
                reads += 1;
            }
            writer.join().unwrap();
        });
    }

    #[test]
    fn wrong_runtime_access_does_not_intern_or_consume_foreign_table_quota() {
        let foreign_runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let table = Table::new_memory(&foreign_runtime, TableConfig::default());
        let before = table.usage();

        let transaction_runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let mut worker = transaction_runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        assert_eq!(
            table.get_inner(&mut transaction, None, b"foreign"),
            Err(AccessError::InvalidUse(InvalidUse::WrongRuntime))
        );
        assert_eq!(table.usage(), before);
        assert!(transaction.is_doomed());
        transaction.abort();
    }

    #[test]
    fn over_capacity_miss_does_not_intern_or_consume_table_quota() {
        let runtime = Runtime::new(
            RuntimeConfig::new()
                .with_max_items_per_transaction(1)
                .with_max_locks_per_transaction(1),
        )
        .unwrap();
        let table = Table::new_memory(&runtime, TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();

        assert_eq!(table.get_inner(&mut transaction, None, b"first"), Ok(None));
        let after_first = table.usage();
        assert_eq!(table.get_inner(&mut transaction, None, b"first"), Ok(None));
        assert_eq!(table.usage(), after_first);
        assert_eq!(
            table.get_inner(&mut transaction, None, b"over-capacity"),
            Err(AccessError::Capacity(CapacityError::ItemLimit))
        );
        assert_eq!(table.usage(), after_first);
        assert!(transaction.is_doomed());
        transaction.abort();
    }

    #[test]
    fn point_operations_compose_and_read_their_own_writes() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let mut txn = worker.begin().unwrap();

        assert_eq!(table.get_inner(&mut txn, None, b"k").unwrap(), None);
        assert_eq!(table.get_inner(&mut txn, None, b"k").unwrap(), None);
        assert_eq!(
            table
                .insert_inner(&mut txn, None, b"k", Arc::from(&b"one"[..]))
                .unwrap(),
            InsertOutcome::Inserted
        );
        assert_eq!(
            table.get_inner(&mut txn, None, b"k").unwrap().as_deref(),
            Some(&b"one"[..])
        );
        assert_eq!(
            table.get_inner(&mut txn, None, b"k").unwrap().as_deref(),
            Some(&b"one"[..])
        );
        assert_eq!(
            table
                .insert_inner(&mut txn, None, b"k", Arc::from(&b"ignored"[..]))
                .unwrap(),
            InsertOutcome::AlreadyPresent(Value::from(&b"one"[..]))
        );
        assert_eq!(
            table.remove_inner(&mut txn, None, b"k").unwrap().as_deref(),
            Some(&b"one"[..])
        );
        assert_eq!(table.get_inner(&mut txn, None, b"k").unwrap(), None);
        assert_eq!(
            table
                .put_inner(&mut txn, None, b"k", Arc::from(&b"two"[..]))
                .unwrap(),
            None
        );
        assert_eq!(
            table.get_inner(&mut txn, None, b"k").unwrap().as_deref(),
            Some(&b"two"[..])
        );
        committed(txn.commit());

        let mut verify = worker.begin().unwrap();
        assert_eq!(
            table.get_inner(&mut verify, None, b"k").unwrap().as_deref(),
            Some(&b"two"[..])
        );
        committed(verify.commit());
    }

    #[test]
    fn presence_only_operations_read_staged_liveness() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();

        assert!(!table
            .put_presence_inner(
                &mut transaction,
                None,
                b"presence/ryw",
                Value::from(&b"first"[..]),
            )
            .unwrap());
        assert!(table
            .remove_presence_inner(&mut transaction, None, b"presence/ryw")
            .unwrap());
        assert!(!table
            .remove_presence_inner(&mut transaction, None, b"presence/ryw")
            .unwrap());
        assert!(table
            .insert_expected_absent_inner(
                &mut transaction,
                None,
                b"presence/ryw",
                Value::from(&b"second"[..]),
            )
            .unwrap());
        assert!(table
            .put_presence_inner(
                &mut transaction,
                None,
                b"presence/ryw",
                Value::from(&b"final"[..]),
            )
            .unwrap());
        committed(transaction.commit());

        let mut verify = worker.begin().unwrap();
        assert_eq!(
            table
                .get_inner(&mut verify, None, b"presence/ryw")
                .unwrap()
                .as_deref(),
            Some(&b"final"[..])
        );
        committed(verify.commit());
    }

    #[test]
    fn abort_keeps_an_interned_tombstone_but_publishes_no_value() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let generation_before = table.shared().directory_generation.load(Ordering::Acquire);
        let mut txn = worker.begin().unwrap();
        table
            .put_inner(&mut txn, None, b"abort", Arc::from(&b"value"[..]))
            .unwrap();
        let generation_after_intern = table.shared().directory_generation.load(Ordering::Acquire);
        assert_eq!(generation_after_intern, generation_before + 1);
        assert_eq!(txn.abort().reason(), &AbortReason::Explicit);
        assert_eq!(
            table.shared().directory_generation.load(Ordering::Acquire),
            generation_after_intern
        );
        assert_eq!(table.usage().retained_records(), 1);
        assert_eq!(table.usage().consumed_record_ids(), 1);

        let mut verify = worker.begin().unwrap();
        assert_eq!(table.get_inner(&mut verify, None, b"abort").unwrap(), None);
        committed(verify.commit());
        assert_eq!(table.usage().consumed_record_ids(), 1);
    }

    #[test]
    fn stale_point_reader_aborts_after_publication() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut setup_worker = runtime.attach().unwrap();
        let mut setup = setup_worker.begin().unwrap();
        table
            .put_inner(&mut setup, None, b"shared", Arc::from(&b"old"[..]))
            .unwrap();
        committed(setup.commit());
        drop(setup_worker);

        let mut reader_worker = runtime.attach().unwrap();
        let mut stale = reader_worker.begin().unwrap();
        assert_eq!(
            table
                .get_inner(&mut stale, None, b"shared")
                .unwrap()
                .as_deref(),
            Some(&b"old"[..])
        );

        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            scope
                .spawn(move || {
                    let mut writer = runtime.attach().unwrap();
                    let mut txn = writer.begin().unwrap();
                    table
                        .put_inner(&mut txn, None, b"shared", Arc::from(&b"new"[..]))
                        .unwrap();
                    committed(txn.commit());
                })
                .join()
                .unwrap();
        });

        assert_eq!(
            stale.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );
    }

    #[test]
    fn repeated_point_read_reloads_and_rejects_a_changed_generation() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut setup_worker = runtime.attach().unwrap();
        seed(&table, &mut setup_worker, &[(b"repeat", b"before")]);
        drop(setup_worker);

        let mut reader_worker = runtime.attach().unwrap();
        let mut reader = reader_worker.begin().unwrap();
        assert_eq!(
            table
                .get_inner(&mut reader, None, b"repeat")
                .unwrap()
                .as_deref(),
            Some(&b"before"[..])
        );

        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            scope
                .spawn(move || {
                    let mut writer_worker = runtime.attach().unwrap();
                    let mut writer = writer_worker.begin().unwrap();
                    table
                        .put_inner(&mut writer, None, b"repeat", Value::from(&b"after"[..]))
                        .unwrap();
                    committed(writer.commit());
                })
                .join()
                .unwrap();
        });

        assert_eq!(
            table.get_inner(&mut reader, None, b"repeat"),
            Err(AccessError::Conflict(Conflict::ReadValidation))
        );
        assert!(reader.is_doomed());
        reader.abort();
        assert_eq!(table.health(), TableHealth::Healthy);
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Healthy);
    }

    #[test]
    fn presence_only_write_reuses_original_liveness_and_commit_validates() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut setup_worker = runtime.attach().unwrap();
        seed(&table, &mut setup_worker, &[(b"presence", b"before")]);
        drop(setup_worker);

        let mut stale_worker = runtime.attach().unwrap();
        let mut stale = stale_worker.begin().unwrap();
        assert_eq!(
            table
                .get_inner(&mut stale, None, b"presence")
                .unwrap()
                .as_deref(),
            Some(&b"before"[..])
        );

        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            scope
                .spawn(move || {
                    let mut writer_worker = runtime.attach().unwrap();
                    let mut writer = writer_worker.begin().unwrap();
                    table
                        .put_inner(&mut writer, None, b"presence", Value::from(&b"winner"[..]))
                        .unwrap();
                    committed(writer.commit());
                })
                .join()
                .unwrap();
        });

        // Presence-only repeat access needs no value reload. It uses the
        // liveness recorded by the first observation and stages normally;
        // certification still rejects the now-stale OCC generation.
        assert!(table
            .put_presence_inner(&mut stale, None, b"presence", Value::from(&b"staged"[..]),)
            .unwrap());
        assert_eq!(
            stale.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );

        let mut verify = stale_worker.begin().unwrap();
        assert_eq!(
            table
                .get_inner(&mut verify, None, b"presence")
                .unwrap()
                .as_deref(),
            Some(&b"winner"[..])
        );
        committed(verify.commit());
    }

    #[test]
    fn repeated_expected_absent_check_reuses_first_metadata_and_commit_validates() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut setup_worker = runtime.attach().unwrap();
        seed(
            &table,
            &mut setup_worker,
            &[(b"presence/expected", b"before")],
        );
        drop(setup_worker);

        let mut stale_worker = runtime.attach().unwrap();
        let mut stale = stale_worker.begin().unwrap();
        assert!(!table
            .insert_expected_absent_inner(
                &mut stale,
                None,
                b"presence/expected",
                Value::from(&b"ignored-a"[..]),
            )
            .unwrap());

        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            scope
                .spawn(move || {
                    let mut writer_worker = runtime.attach().unwrap();
                    let mut writer = writer_worker.begin().unwrap();
                    assert!(table
                        .remove_presence_inner(&mut writer, None, b"presence/expected")
                        .unwrap());
                    committed(writer.commit());
                })
                .join()
                .unwrap();
        });

        // The transaction's original logical snapshot remains present, so a
        // second conditional insert is still rejected during execution. The
        // preflight-free read validation then detects the concurrent removal.
        assert!(!table
            .insert_expected_absent_inner(
                &mut stale,
                None,
                b"presence/expected",
                Value::from(&b"ignored-b"[..]),
            )
            .unwrap());
        assert_eq!(
            stale.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );

        let mut verify = stale_worker.begin().unwrap();
        assert_eq!(
            table
                .get_inner(&mut verify, None, b"presence/expected")
                .unwrap(),
            None
        );
        committed(verify.commit());
        assert_eq!(table.health(), TableHealth::Healthy);
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Healthy);
    }

    #[test]
    fn scan_reloads_an_already_observed_record() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut setup_worker = runtime.attach().unwrap();
        seed(&table, &mut setup_worker, &[(b"scan/reload", b"before")]);
        drop(setup_worker);

        let mut scan_worker = runtime.attach().unwrap();
        let mut scan = scan_worker.begin().unwrap();
        assert_eq!(
            table
                .get_inner(&mut scan, None, b"scan/reload")
                .unwrap()
                .as_deref(),
            Some(&b"before"[..])
        );

        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            scope
                .spawn(move || {
                    let mut writer_worker = runtime.attach().unwrap();
                    let mut writer = writer_worker.begin().unwrap();
                    table
                        .put_inner(
                            &mut writer,
                            None,
                            b"scan/reload",
                            Value::from(&b"after"[..]),
                        )
                        .unwrap();
                    committed(writer.commit());
                })
                .join()
                .unwrap();
        });

        assert_eq!(
            table.scan_inner(&mut scan, None, ScanRequest::new(ScanDirection::Forward, 1),),
            Err(AccessError::Conflict(Conflict::ReadValidation))
        );
        assert!(scan.is_doomed());
        scan.abort();
        assert_eq!(table.health(), TableHealth::Healthy);
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Healthy);
    }

    #[test]
    fn staged_read_your_writes_bypasses_reload_but_commit_still_validates() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut setup_worker = runtime.attach().unwrap();
        seed(&table, &mut setup_worker, &[(b"ryw-race", b"old")]);
        drop(setup_worker);

        let mut stale_worker = runtime.attach().unwrap();
        let mut stale = stale_worker.begin().unwrap();
        table
            .put_inner(&mut stale, None, b"ryw-race", Value::from(&b"staged"[..]))
            .unwrap();

        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            scope
                .spawn(move || {
                    let mut writer_worker = runtime.attach().unwrap();
                    let mut writer = writer_worker.begin().unwrap();
                    table
                        .put_inner(&mut writer, None, b"ryw-race", Value::from(&b"winner"[..]))
                        .unwrap();
                    committed(writer.commit());
                })
                .join()
                .unwrap();
        });

        assert_eq!(
            table
                .get_inner(&mut stale, None, b"ryw-race")
                .unwrap()
                .as_deref(),
            Some(&b"staged"[..])
        );
        assert_eq!(
            stale.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );
        assert_eq!(table.health(), TableHealth::Healthy);
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Healthy);
    }

    #[test]
    fn concurrent_inline_to_shared_replacement_keeps_the_snapshot_and_rejects_the_reader() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let original = Value::from(&b"inline"[..]);
        let replacement_bytes = vec![0xa5; INLINE_VALUE_CAPACITY + 1];
        let mut setup_worker = runtime.attach().unwrap();
        let mut setup = setup_worker.begin().unwrap();
        table
            .put_inner(&mut setup, None, b"boundary", original.clone())
            .unwrap();
        committed(setup.commit());
        drop(setup_worker);

        let mut reader_worker = runtime.attach().unwrap();
        let mut stale = reader_worker.begin().unwrap();
        let retained_snapshot = table
            .get_inner(&mut stale, None, b"boundary")
            .unwrap()
            .unwrap();
        assert_eq!(retained_snapshot, original);

        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            let replacement_bytes = replacement_bytes.clone();
            scope
                .spawn(move || {
                    let mut writer_worker = runtime.attach().unwrap();
                    let mut writer = writer_worker.begin().unwrap();
                    table
                        .put_inner(
                            &mut writer,
                            None,
                            b"boundary",
                            Value::from(replacement_bytes),
                        )
                        .unwrap();
                    committed(writer.commit());
                })
                .join()
                .unwrap();
        });

        // Installation displaced the committed inline state, but snapshots
        // already returned to callers remain independent owned values.
        assert_eq!(retained_snapshot.as_ref(), b"inline");
        assert_eq!(
            stale.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );

        let mut verify = reader_worker.begin().unwrap();
        assert_eq!(
            table
                .get_inner(&mut verify, None, b"boundary")
                .unwrap()
                .unwrap()
                .as_ref(),
            replacement_bytes
        );
        committed(verify.commit());
    }

    #[test]
    fn interrupted_record_state_publication_quarantines_the_table_and_runtime() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let mut setup = worker.begin().unwrap();
        assert_eq!(
            table.put_inner(
                &mut setup,
                None,
                b"poison",
                vec![0xa5; INLINE_VALUE_CAPACITY + 1],
            ),
            Ok(None)
        );
        committed(setup.commit());

        let record_id = table.shared().lookup(None, b"poison").unwrap().unwrap();
        let record = table.shared().resolve_directory_record(record_id).unwrap();
        let poisoned = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            let _publication = record.state.begin_publication(table.shared(), None);
            panic!("deliberately interrupt committed-state publication");
        }));
        assert!(poisoned.is_err());
        assert_eq!(
            record_state_descriptor(record.state.tail_and_descriptor.load(Ordering::Acquire)),
            RECORD_STATE_POISONED
        );

        let mut transaction = worker.begin().unwrap();
        assert!(matches!(
            table.get_inner(&mut transaction, None, b"poison"),
            Err(AccessError::Fault(_))
        ));
        assert_eq!(table.health(), TableHealth::Poisoned);
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Poisoned);
        transaction.abort();
    }

    #[test]
    fn stable_incomplete_record_state_fails_closed_instead_of_becoming_a_snapshot() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let mut intern = worker.begin().unwrap();
        assert_eq!(table.get_inner(&mut intern, None, b"incomplete"), Ok(None));
        committed(intern.commit());

        let record_id = table.shared().lookup(None, b"incomplete").unwrap().unwrap();
        let access = table.shared().resolve_directory_access(record_id).unwrap();
        let record = access.record;
        // Simulate a publication marker that survived without the matching
        // version transition. A racing real publication instead changes or
        // locks the version and is reported as a retryable conflict.
        record.state.tail_and_descriptor.store(
            pack_record_state_word(RECORD_STATE_UPDATING, 0),
            Ordering::Release,
        );

        let mut transaction = worker.begin().unwrap();
        assert!(matches!(
            table.get_inner(&mut transaction, None, b"incomplete"),
            Err(AccessError::Fault(_))
        ));
        assert_eq!(table.health(), TableHealth::Poisoned);
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Poisoned);
        transaction.abort();
    }

    #[test]
    fn stable_incomplete_byte_lease_poisons_before_invoking_the_visitor() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let mut intern = worker.begin().unwrap();
        assert_eq!(
            table.get_inner(&mut intern, None, b"bytes/incomplete"),
            Ok(None)
        );
        committed(intern.commit());

        let record_id = table
            .shared()
            .lookup(None, b"bytes/incomplete")
            .unwrap()
            .unwrap();
        let record = table.shared().resolve_directory_record(record_id).unwrap();
        record.state.tail_and_descriptor.store(
            pack_record_state_word(RECORD_STATE_UPDATING, 0),
            Ordering::Release,
        );

        let mut invoked = false;
        let mut transaction = worker.begin().unwrap();
        assert!(matches!(
            table.visit_get_bytes_inner(&mut transaction, None, b"bytes/incomplete", |_| invoked =
                true,),
            Err(AccessError::Fault(_))
        ));
        assert!(!invoked);
        assert_eq!(table.health(), TableHealth::Poisoned);
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Poisoned);
        transaction.abort();
    }

    #[test]
    fn stable_invalid_packed_descriptor_fails_closed_and_poisons() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let mut intern = worker.begin().unwrap();
        assert_eq!(
            table.get_inner(&mut intern, None, b"invalid/descriptor"),
            Ok(None)
        );
        committed(intern.commit());

        let record_id = table
            .shared()
            .lookup(None, b"invalid/descriptor")
            .unwrap()
            .unwrap();
        let record = table.shared().resolve_directory_record(record_id).unwrap();
        record.state.tail_and_descriptor.store(
            pack_record_state_word(0x1234, 0x00ab_cdef_0123),
            Ordering::Release,
        );

        let mut transaction = worker.begin().unwrap();
        assert!(matches!(
            table.get_inner(&mut transaction, None, b"invalid/descriptor"),
            Err(AccessError::Fault(_))
        ));
        assert_eq!(table.health(), TableHealth::Poisoned);
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Poisoned);
        transaction.abort();
    }

    #[test]
    fn stable_incomplete_metadata_fails_closed_instead_of_becoming_presence() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let mut intern = worker.begin().unwrap();
        assert_eq!(
            table.get_inner(&mut intern, None, b"metadata/incomplete"),
            Ok(None)
        );
        committed(intern.commit());

        let record_id = table
            .shared()
            .lookup(None, b"metadata/incomplete")
            .unwrap()
            .unwrap();
        let record = table.shared().resolve_directory_record(record_id).unwrap();
        record.state.tail_and_descriptor.store(
            pack_record_state_word(RECORD_STATE_UPDATING, 0),
            Ordering::Release,
        );

        let mut transaction = worker.begin().unwrap();
        assert!(matches!(
            table.remove_presence_inner(&mut transaction, None, b"metadata/incomplete"),
            Err(AccessError::Fault(_))
        ));
        assert_eq!(table.health(), TableHealth::Poisoned);
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Poisoned);
        transaction.abort();
    }

    #[test]
    fn incomplete_state_inside_a_changed_version_sandwich_is_retryable() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let mut intern = worker.begin().unwrap();
        assert_eq!(table.get_inner(&mut intern, None, b"sandwich"), Ok(None));
        committed(intern.commit());

        let record_id = table.shared().lookup(None, b"sandwich").unwrap().unwrap();
        let access = table.shared().resolve_directory_access(record_id).unwrap();
        let record = access.record;
        let observed = record.version.observe().unwrap();
        let owner = sto_core::OwnerId::new(0).unwrap();
        let mut guard = record.version.try_acquire_detached(owner).unwrap();
        record.state.tail_and_descriptor.store(
            pack_record_state_word(RECORD_STATE_UPDATING, 0),
            Ordering::Release,
        );

        assert_eq!(
            table
                .record_resource
                .adapter()
                .snapshot_state(access, observed),
            Err(AccessError::Conflict(Conflict::ReadValidation))
        );
        assert_eq!(table.health(), TableHealth::Healthy);
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Healthy);
        assert!(matches!(
            table
                .record_resource
                .adapter()
                .snapshot_state_lease(access, observed),
            Err(AccessError::Conflict(Conflict::ReadValidation))
        ));
        assert_eq!(table.health(), TableHealth::Healthy);
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Healthy);
        assert_eq!(
            table
                .record_resource
                .adapter()
                .snapshot_metadata(access, observed),
            Err(AccessError::Conflict(Conflict::ReadValidation))
        );
        assert_eq!(table.health(), TableHealth::Healthy);
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Healthy);

        record.state.tail_and_descriptor.store(
            pack_record_state_word(RECORD_STATE_TOMBSTONE, 0),
            Ordering::Release,
        );
        guard.release_abort(&record.version).unwrap();
        assert_eq!(committed_record_snapshot(record), RecordState::Tombstone);
    }

    #[test]
    fn same_value_and_write_then_revert_advance_the_record_version() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let original = &b"original-value-beyond-inline"[..];
        seed(&table, &mut worker, &[(b"stable", original)]);

        let record_id = table.shared().lookup(None, b"stable").unwrap().unwrap();
        let record = table.shared().resolve_directory_record(record_id).unwrap();
        let original_version = record.version.observe().unwrap();

        // This replacement has equal bytes but independent shared storage.
        // Equality is abstract value equality, not Arc identity.
        let mut same_value = worker.begin().unwrap();
        assert_eq!(
            table
                .put_inner(&mut same_value, None, b"stable", Value::from(original))
                .unwrap()
                .as_deref(),
            Some(original)
        );
        assert_eq!(
            table
                .get_inner(&mut same_value, None, b"stable")
                .unwrap()
                .as_deref(),
            Some(original)
        );
        committed(same_value.commit());
        let same_value_version = record.version.observe().unwrap();
        assert!(same_value_version > original_version);

        let mut reverted = worker.begin().unwrap();
        assert_eq!(
            table
                .put_inner(
                    &mut reverted,
                    None,
                    b"stable",
                    Value::from(&b"temporary"[..]),
                )
                .unwrap()
                .as_deref(),
            Some(original)
        );
        assert_eq!(
            table
                .put_inner(&mut reverted, None, b"stable", Value::from(original))
                .unwrap()
                .as_deref(),
            Some(&b"temporary"[..])
        );
        assert_eq!(
            table
                .get_inner(&mut reverted, None, b"stable")
                .unwrap()
                .as_deref(),
            Some(original)
        );
        committed(reverted.commit());
        assert!(record.version.observe().unwrap() > same_value_version);
        assert_eq!(
            committed_record_snapshot(record).value().map(Value::as_ref),
            Some(original)
        );
    }

    #[test]
    fn stale_same_value_intent_still_validates_its_original_version() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut setup_worker = runtime.attach().unwrap();
        seed(&table, &mut setup_worker, &[(b"same", b"original")]);
        drop(setup_worker);

        let mut stale_worker = runtime.attach().unwrap();
        let mut stale = stale_worker.begin().unwrap();
        assert_eq!(
            table
                .put_inner(&mut stale, None, b"same", Value::from(&b"original"[..]),)
                .unwrap()
                .as_deref(),
            Some(&b"original"[..])
        );

        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            scope
                .spawn(move || {
                    let mut current_worker = runtime.attach().unwrap();
                    let mut current = current_worker.begin().unwrap();
                    table
                        .put_inner(&mut current, None, b"same", Value::from(&b"winner"[..]))
                        .unwrap();
                    committed(current.commit());
                })
                .join()
                .unwrap();
        });

        assert_eq!(
            stale.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Healthy);
    }

    #[test]
    fn stale_writer_is_a_retryable_validation_conflict_not_an_adapter_fault() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut setup_worker = runtime.attach().unwrap();
        let mut setup = setup_worker.begin().unwrap();
        table
            .put_inner(&mut setup, None, b"write", Arc::from(&b"initial"[..]))
            .unwrap();
        committed(setup.commit());
        drop(setup_worker);

        let mut stale_worker = runtime.attach().unwrap();
        let mut stale = stale_worker.begin().unwrap();
        table
            .put_inner(&mut stale, None, b"write", Arc::from(&b"stale"[..]))
            .unwrap();

        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            scope
                .spawn(move || {
                    let mut current_worker = runtime.attach().unwrap();
                    let mut current = current_worker.begin().unwrap();
                    table
                        .put_inner(&mut current, None, b"write", Arc::from(&b"winner"[..]))
                        .unwrap();
                    committed(current.commit());
                })
                .join()
                .unwrap();
        });

        assert_eq!(
            stale.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Healthy);
    }

    #[test]
    fn retained_record_and_key_quotas_are_enforced_before_publication() {
        let config = TableConfig::new()
            .with_max_retained_records(1)
            .with_max_retained_key_bytes(1)
            .with_max_consumed_record_ids(8);
        let (runtime, table) = runtime_and_table(config);
        let mut worker = runtime.attach().unwrap();

        let mut first = worker.begin().unwrap();
        assert_eq!(table.get_inner(&mut first, None, b"a").unwrap(), None);
        committed(first.commit());
        assert_eq!(
            table.usage(),
            TableUsage {
                retained_records: 1,
                retained_key_bytes: 1,
                consumed_record_ids: 1,
            }
        );

        let mut second = worker.begin().unwrap();
        assert_eq!(
            table.get_inner(&mut second, None, b"b"),
            Err(AccessError::Capacity(CapacityError::BufferLimit))
        );
        assert!(second.is_doomed());
        assert_eq!(second.abort().reason(), &AbortReason::Explicit);
        assert_eq!(table.usage().consumed_record_ids(), 1);
    }

    #[test]
    fn consumed_ids_never_reuse_proven_unpublished_slots() {
        let registry = isolated_registry(
            TableConfig::new()
                .with_max_retained_records(4)
                .with_max_retained_key_bytes(16)
                .with_max_consumed_record_ids(2),
        );
        let first = registry.reserve_candidate(b"a").unwrap();
        registry.prove_unpublished(&first).unwrap();
        assert!(matches!(
            registry.prove_unpublished(&first),
            Err(AccessError::Fault(_))
        ));
        assert_eq!(registry.usage().retained_records(), 0);
        assert_eq!(registry.usage().retained_key_bytes(), 0);
        let second = registry.reserve_candidate(b"b").unwrap();
        assert_eq!(first.id.get(), 1);
        assert_eq!(second.id.get(), 2);
        registry.prove_unpublished(&second).unwrap();
        assert_eq!(registry.usage().retained_records(), 0);
        assert_eq!(registry.usage().consumed_record_ids(), 2);
        assert!(matches!(
            registry.reserve_candidate(b"c"),
            Err(AccessError::Capacity(CapacityError::BufferLimit))
        ));
    }

    #[test]
    fn record_lock_targets_are_segmented_and_validate_exact_record_ids() {
        let registry = isolated_registry(
            TableConfig::new()
                .with_max_retained_records((REGISTRY_SEGMENT_SLOTS + 1) as u64)
                .with_max_retained_key_bytes((REGISTRY_SEGMENT_SLOTS + 1) as u64)
                .with_max_consumed_record_ids((REGISTRY_SEGMENT_SLOTS + 1) as u64),
        );
        let mut candidates = Vec::new();
        for _ in 0..=REGISTRY_SEGMENT_SLOTS {
            candidates.push(registry.reserve_candidate(b"x").unwrap());
        }

        let first_id = candidates[0].id;
        let second_id = candidates[1].id;
        let next_lock_segment_id = candidates[RECORD_LOCK_SEGMENT_SLOTS].id;
        let next_registry_segment_id = candidates[REGISTRY_SEGMENT_SLOTS].id;
        let (first_record, first_segment) = registry.resolve_with_segment(first_id).unwrap();
        let (second_record, second_segment) = registry.resolve_with_segment(second_id).unwrap();
        let (next_lock_record, next_lock_segment) =
            registry.resolve_with_segment(next_lock_segment_id).unwrap();
        let (next_registry_record, next_registry_segment) = registry
            .resolve_with_segment(next_registry_segment_id)
            .unwrap();

        assert!(Arc::ptr_eq(first_segment, second_segment));
        assert!(!Arc::ptr_eq(first_segment, next_lock_segment));
        assert!(first_segment
            .arena
            .shares_allocation_with(&next_lock_segment.arena));
        assert!(!first_segment
            .arena
            .shares_allocation_with(&next_registry_segment.arena));
        assert!(!std::ptr::eq(first_record, second_record));
        assert!(!std::ptr::eq(first_record, next_lock_record));
        assert!(!std::ptr::eq(first_record, next_registry_record));

        let first_identity = LockIdentity::new(
            first_segment.lock_domain.runtime_id,
            first_segment.lock_domain.namespace,
            first_segment.lock_domain.lock_class,
            first_id.get(),
        );
        let second_identity = LockIdentity::new(
            first_segment.lock_domain.runtime_id,
            first_segment.lock_domain.namespace,
            first_segment.lock_domain.lock_class,
            second_id.get(),
        );
        let wrong_segment_identity = LockIdentity::new(
            first_segment.lock_domain.runtime_id,
            first_segment.lock_domain.namespace,
            first_segment.lock_domain.lock_class,
            next_lock_segment_id.get(),
        );
        assert_eq!(
            first_segment
                .identity_record_slot(&first_identity, AdapterPhase::Acquire)
                .unwrap()
                .0,
            first_id
        );
        assert_eq!(
            first_segment
                .identity_record_slot(&second_identity, AdapterPhase::Acquire)
                .unwrap()
                .0,
            second_id
        );
        assert!(first_segment
            .identity_record_slot(&wrong_segment_identity, AdapterPhase::Acquire)
            .is_err());
    }

    #[test]
    fn two_record_writes_in_one_segment_retain_distinct_lock_guards() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        seed(&table, &mut worker, &[(b"a", b"old-a"), (b"b", b"old-b")]);

        let first_id = table.shared().lookup(None, b"a").unwrap().unwrap();
        let second_id = table.shared().lookup(None, b"b").unwrap().unwrap();
        let (first, first_segment) = table
            .shared()
            .registry
            .resolve_with_segment(first_id)
            .unwrap();
        let (second, second_segment) = table
            .shared()
            .registry
            .resolve_with_segment(second_id)
            .unwrap();
        assert!(Arc::ptr_eq(first_segment, second_segment));
        let first_before = first.version.observe().unwrap();
        let second_before = second.version.observe().unwrap();

        let mut txn = worker.begin().unwrap();
        table
            .put_inner(&mut txn, None, b"a", Value::from_slice(b"new-a"))
            .unwrap();
        table
            .put_inner(&mut txn, None, b"b", Value::from_slice(b"new-b"))
            .unwrap();
        committed(txn.commit());

        assert!(first.version.observe().unwrap() > first_before);
        assert!(second.version.observe().unwrap() > second_before);
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Healthy);
        assert_eq!(table.health(), TableHealth::Healthy);
    }

    #[test]
    fn liveness_writes_need_only_record_items_and_locks() {
        let runtime = Runtime::new(
            RuntimeConfig::new()
                .with_max_items_per_transaction(3)
                .with_max_locks_per_transaction(3),
        )
        .unwrap();
        let table = Table::new_memory(&runtime, TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        seed(
            &table,
            &mut worker,
            &[(b"existing-a", b"old-a"), (b"existing-c", b"old-c")],
        );
        let generation_before = table.shared().directory_generation.load(Ordering::Acquire);

        let mut txn = worker.begin().unwrap();
        assert_eq!(
            table
                .put_inner(&mut txn, None, b"existing-a", &b"new-a"[..])
                .unwrap()
                .as_deref(),
            Some(&b"old-a"[..])
        );
        assert_eq!(
            table
                .put_inner(&mut txn, None, b"inserted-b", &b"new-b"[..])
                .unwrap(),
            None
        );
        assert_eq!(
            table
                .put_inner(&mut txn, None, b"existing-c", &b"new-c"[..])
                .unwrap()
                .as_deref(),
            Some(&b"old-c"[..])
        );
        assert_eq!(
            table.shared().directory_generation.load(Ordering::Acquire),
            generation_before + 1
        );
        committed(txn.commit());
        let mut verify = worker.begin().unwrap();
        for (key, expected) in [
            (&b"existing-a"[..], &b"new-a"[..]),
            (&b"inserted-b"[..], &b"new-b"[..]),
            (&b"existing-c"[..], &b"new-c"[..]),
        ] {
            assert_eq!(
                table.get_inner(&mut verify, None, key).unwrap().as_deref(),
                Some(expected)
            );
        }
        committed(verify.commit());
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Healthy);
        assert_eq!(table.health(), TableHealth::Healthy);
    }

    #[test]
    fn unique_lock_request_tables_commit_all_record_locks() {
        let runtime = Runtime::new(
            RuntimeConfig::new()
                .with_max_items_per_transaction(3)
                .with_max_locks_per_transaction(3),
        )
        .unwrap();
        let config = TableConfig::new().with_unique_lock_requests(true);
        let first = Table::new_memory(&runtime, config);
        let second = Table::new_memory(&runtime, config);
        let mut worker = runtime.attach().unwrap();

        // The two first-table records normally share one physical lock target,
        // but their complete identities are distinct. Directory-generation
        // observations emit no lock request into the unique planning lane.
        let mut transaction = worker.begin().unwrap();
        for (table, key, value) in [
            (&first, &b"first/a"[..], &b"one"[..]),
            (&first, &b"first/b"[..], &b"two"[..]),
            (&second, &b"second/c"[..], &b"three"[..]),
        ] {
            assert_eq!(
                table
                    .put_inner(&mut transaction, None, key, Value::from(value))
                    .unwrap(),
                None
            );
        }
        committed(transaction.commit());

        let mut verify = worker.begin().unwrap();
        for (table, key, expected) in [
            (&first, &b"first/a"[..], &b"one"[..]),
            (&first, &b"first/b"[..], &b"two"[..]),
            (&second, &b"second/c"[..], &b"three"[..]),
        ] {
            assert_eq!(
                table.get_inner(&mut verify, None, key).unwrap().as_deref(),
                Some(expected)
            );
        }
        committed(verify.commit());
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Healthy);
        assert_eq!(first.health(), TableHealth::Healthy);
        assert_eq!(second.health(), TableHealth::Healthy);
    }

    #[test]
    fn physical_interning_advances_generation_despite_net_liveness_cancellation() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let before = table.shared().directory_generation.load(Ordering::Acquire);
        let mut txn = worker.begin().unwrap();
        table
            .put_inner(&mut txn, None, b"a", Arc::from(&b"one"[..]))
            .unwrap();
        table
            .put_inner(&mut txn, None, b"b", Arc::from(&b"two"[..]))
            .unwrap();
        table.remove_inner(&mut txn, None, b"a").unwrap();
        committed(txn.commit());
        let after = table.shared().directory_generation.load(Ordering::Acquire);
        assert_eq!(after, before + 2);

        let mut verify = worker.begin().unwrap();
        assert_eq!(table.get_inner(&mut verify, None, b"a").unwrap(), None);
        assert_eq!(
            table.get_inner(&mut verify, None, b"b").unwrap().as_deref(),
            Some(&b"two"[..])
        );
        committed(verify.commit());
    }

    #[test]
    fn existing_liveness_changes_skip_generation_but_first_intern_advances_it() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();

        let mut setup = worker.begin().unwrap();
        table
            .put_inner(&mut setup, None, b"live", Arc::from(&b"original"[..]))
            .unwrap();
        committed(setup.commit());

        let before_live = table.shared().directory_generation.load(Ordering::Acquire);
        let live_id = table.shared().lookup(None, b"live").unwrap().unwrap();
        let live_record = table.shared().resolve_directory_record(live_id).unwrap();
        let live_record_before = live_record.version.observe().unwrap();
        let mut live = worker.begin().unwrap();
        assert_eq!(
            table
                .remove_inner(&mut live, None, b"live")
                .unwrap()
                .as_deref(),
            Some(&b"original"[..])
        );
        assert_eq!(
            table
                .insert_inner(&mut live, None, b"live", Arc::from(&b"replacement"[..]))
                .unwrap(),
            InsertOutcome::Inserted
        );
        committed(live.commit());
        let after_live = table.shared().directory_generation.load(Ordering::Acquire);
        assert_eq!(after_live, before_live);
        assert!(live_record.version.observe().unwrap() > live_record_before);

        let before_absent = after_live;
        let mut absent = worker.begin().unwrap();
        assert_eq!(
            table
                .insert_inner(&mut absent, None, b"absent", Arc::from(&b"temporary"[..]))
                .unwrap(),
            InsertOutcome::Inserted
        );
        assert_eq!(
            table
                .remove_inner(&mut absent, None, b"absent")
                .unwrap()
                .as_deref(),
            Some(&b"temporary"[..])
        );
        let absent_id = table.shared().lookup(None, b"absent").unwrap().unwrap();
        let absent_record = table.shared().resolve_directory_record(absent_id).unwrap();
        let absent_record_before = absent_record.version.observe().unwrap();
        committed(absent.commit());
        let after_absent = table.shared().directory_generation.load(Ordering::Acquire);
        assert_eq!(after_absent, before_absent + 1);
        assert!(absent_record.version.observe().unwrap() > absent_record_before);

        let mut verify = worker.begin().unwrap();
        assert_eq!(
            table
                .get_inner(&mut verify, None, b"live")
                .unwrap()
                .as_deref(),
            Some(&b"replacement"[..])
        );
        assert_eq!(table.get_inner(&mut verify, None, b"absent").unwrap(), None);
        committed(verify.commit());
    }

    #[test]
    fn ordinary_updates_use_only_record_items_and_preserve_directory_generation() {
        let runtime = Runtime::new(
            RuntimeConfig::new()
                .with_max_items_per_transaction(2)
                .with_max_locks_per_transaction(2),
        )
        .unwrap();
        let table = Table::new_memory(&runtime, TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        for (key, value) in [(&b"a"[..], &b"old-a"[..]), (&b"b"[..], &b"old-b"[..])] {
            let mut setup = worker.begin().unwrap();
            table
                .put_inner(&mut setup, None, key, Arc::from(value))
                .unwrap();
            committed(setup.commit());
        }
        let usage_before = table.usage();
        let generation_before = table.shared().directory_generation.load(Ordering::Acquire);

        // Both item slots are record items. An ordinary live-to-live update
        // neither creates a directory item nor advances physical generation.
        let mut update = worker.begin().unwrap();
        table
            .put_inner(&mut update, None, b"a", Arc::from(&b"new-a"[..]))
            .unwrap();
        table
            .put_inner(&mut update, None, b"b", Arc::from(&b"new-b"[..]))
            .unwrap();
        committed(update.commit());

        assert_eq!(table.usage(), usage_before);
        assert_eq!(
            table.shared().directory_generation.load(Ordering::Acquire),
            generation_before
        );
    }

    #[test]
    fn multi_record_publication_is_accepted_or_rejected_as_one_occ_outcome() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut setup_worker = runtime.attach().unwrap();
        let mut setup = setup_worker.begin().unwrap();
        table
            .put_inner(&mut setup, None, b"a", Arc::from(&b"old-a"[..]))
            .unwrap();
        table
            .put_inner(&mut setup, None, b"b", Arc::from(&b"old-b"[..]))
            .unwrap();
        committed(setup.commit());
        drop(setup_worker);

        let mut reader_worker = runtime.attach().unwrap();
        let mut reader = reader_worker.begin().unwrap();
        assert_eq!(
            table.get_inner(&mut reader, None, b"a").unwrap().as_deref(),
            Some(&b"old-a"[..])
        );

        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            scope
                .spawn(move || {
                    let mut writer_worker = runtime.attach().unwrap();
                    let mut writer = writer_worker.begin().unwrap();
                    table
                        .put_inner(&mut writer, None, b"a", Arc::from(&b"new-a"[..]))
                        .unwrap();
                    table
                        .put_inner(&mut writer, None, b"b", Arc::from(&b"new-b"[..]))
                        .unwrap();
                    committed(writer.commit());
                })
                .join()
                .unwrap();
        });

        // Serializable (nonopaque) execution can momentarily see this mixed
        // pair, but final OCC certification must reject the whole history.
        assert_eq!(
            table.get_inner(&mut reader, None, b"b").unwrap().as_deref(),
            Some(&b"new-b"[..])
        );
        assert_eq!(
            reader.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );

        let mut current = reader_worker.begin().unwrap();
        assert_eq!(
            table
                .get_inner(&mut current, None, b"a")
                .unwrap()
                .as_deref(),
            Some(&b"new-a"[..])
        );
        assert_eq!(
            table
                .get_inner(&mut current, None, b"b")
                .unwrap()
                .as_deref(),
            Some(&b"new-b"[..])
        );
        committed(current.commit());
    }

    #[test]
    fn binary_and_empty_keys_are_distinct_and_count_exact_bytes() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let mut txn = worker.begin().unwrap();
        table
            .put_inner(&mut txn, None, b"", Arc::from(&b"empty"[..]))
            .unwrap();
        table
            .put_inner(&mut txn, None, b"a\0b", Arc::from(&b"binary"[..]))
            .unwrap();
        committed(txn.commit());
        assert_eq!(table.usage().retained_records(), 2);
        assert_eq!(table.usage().retained_key_bytes(), 3);

        let mut verify = worker.begin().unwrap();
        assert_eq!(
            table.get_inner(&mut verify, None, b"").unwrap().as_deref(),
            Some(&b"empty"[..])
        );
        assert_eq!(
            table
                .get_inner(&mut verify, None, b"a\0b")
                .unwrap()
                .as_deref(),
            Some(&b"binary"[..])
        );
        committed(verify.commit());
    }

    #[test]
    fn sealed_directory_preserves_existing_records_and_rejects_new_keys_without_quota() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        seed(&table, &mut worker, &[(b"live", b"before")]);

        let mut make_tombstone = worker.begin().unwrap();
        assert_eq!(
            table
                .get_inner(&mut make_tombstone, None, b"tombstone")
                .unwrap(),
            None
        );
        committed(make_tombstone.commit());
        let sealed_usage = table.usage();

        table.seal_directory_structure().unwrap();
        assert!(table.shared().structural.is_sealed());

        let mut read = worker.begin().unwrap();
        assert_eq!(
            table
                .get_inner(&mut read, None, b"live")
                .unwrap()
                .as_deref(),
            Some(&b"before"[..])
        );
        assert_eq!(
            table.get_inner(&mut read, None, b"tombstone").unwrap(),
            None
        );
        committed(read.commit());

        let mut update = worker.begin().unwrap();
        assert_eq!(
            table
                .put_inner(&mut update, None, b"live", Value::from(&b"updated"[..]))
                .unwrap()
                .as_deref(),
            Some(&b"before"[..])
        );
        committed(update.commit());

        let mut remove = worker.begin().unwrap();
        assert_eq!(
            table
                .remove_inner(&mut remove, None, b"live")
                .unwrap()
                .as_deref(),
            Some(&b"updated"[..])
        );
        committed(remove.commit());

        let mut resurrect = worker.begin().unwrap();
        assert_eq!(
            table
                .put_inner(
                    &mut resurrect,
                    None,
                    b"live",
                    Value::from(&b"resurrected"[..]),
                )
                .unwrap(),
            None
        );
        assert_eq!(
            table
                .put_inner(
                    &mut resurrect,
                    None,
                    b"tombstone",
                    Value::from(&b"also-live"[..]),
                )
                .unwrap(),
            None
        );
        committed(resurrect.commit());
        assert_eq!(table.usage(), sealed_usage);

        for operation in ["get", "put", "remove"] {
            let before = table.usage();
            let mut missing = worker.begin().unwrap();
            let error = match operation {
                "get" => table.get_inner(&mut missing, None, b"new").map(|_| ()),
                "put" => table
                    .put_inner(&mut missing, None, b"new", Value::from(&b"value"[..]))
                    .map(|_| ()),
                "remove" => table.remove_inner(&mut missing, None, b"new").map(|_| ()),
                _ => unreachable!(),
            }
            .unwrap_err();
            assert_eq!(error, directory_structure_sealed());
            assert!(missing.is_doomed());
            missing.abort();
            assert_eq!(table.usage(), before);
            assert_eq!(table.health(), TableHealth::Healthy);
        }
    }

    #[test]
    fn concurrent_seals_are_idempotent_and_unsealed_tables_still_grow() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        seed(&table, &mut worker, &[(b"before", b"B")]);
        seed(&table, &mut worker, &[(b"dynamic", b"D")]);
        assert_eq!(table.usage().retained_records(), 2);

        std::thread::scope(|scope| {
            let mut seals = Vec::new();
            for _ in 0..8 {
                let table = table.clone();
                seals.push(scope.spawn(move || table.seal_directory_structure()));
            }
            for seal in seals {
                seal.join().unwrap().unwrap();
            }
        });
        table.seal_directory_structure().unwrap();

        let seal_calls = match &table.shared().directory {
            Directory::Memory(directory) => directory.seal_calls.load(Ordering::Relaxed),
        };
        assert_eq!(seal_calls, 1);
        assert_eq!(table.health(), TableHealth::Healthy);
    }

    #[test]
    fn seal_after_scalar_reservation_proves_candidate_unpublished() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let reserved = Arc::new(Barrier::new(2));
        let resume = Arc::new(Barrier::new(2));
        let table = Table::new_memory_with_candidate_reservation_pause(
            &runtime,
            TableConfig::default(),
            Arc::clone(&reserved),
            Arc::clone(&resume),
        );

        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table_for_miss = table.clone();
            let miss = scope.spawn(move || {
                let mut worker = runtime.attach().unwrap();
                let mut transaction = worker.begin().unwrap();
                let error = table_for_miss
                    .get_inner(&mut transaction, None, b"raced")
                    .unwrap_err();
                assert!(transaction.is_doomed());
                transaction.abort();
                error
            });

            reserved.wait();
            table.seal_directory_structure().unwrap();
            resume.wait();
            assert_eq!(miss.join().unwrap(), directory_structure_sealed());
        });

        assert_eq!(
            table.usage(),
            TableUsage {
                retained_records: 0,
                retained_key_bytes: 0,
                consumed_record_ids: 1,
            }
        );
        assert_eq!(
            table
                .shared()
                .registry
                .entry(RecordId::new(1).unwrap())
                .unwrap()
                .state
                .load(Ordering::Acquire),
            SLOT_PROVEN_UNPUBLISHED
        );
        assert_eq!(table.health(), TableHealth::Healthy);

        let consumed = table.usage().consumed_record_ids();
        let mut worker = runtime.attach().unwrap();
        let mut ordinary_miss = worker.begin().unwrap();
        assert_eq!(
            table.get_inner(&mut ordinary_miss, None, b"later"),
            Err(directory_structure_sealed())
        );
        ordinary_miss.abort();
        assert_eq!(table.usage().consumed_record_ids(), consumed);
    }

    #[test]
    fn sealed_fixed_batch_cleanup_releases_every_retained_candidate() {
        const COUNT: usize = 4;
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let shared = table.shared();
        let mut candidates = Vec::with_capacity(COUNT);
        let mut directory_tokens = Vec::with_capacity(COUNT);
        assert_eq!(
            shared
                .registry
                .reserve_candidate_batch_with_mode(
                    COUNT,
                    8,
                    shared.record_token_mode,
                    &mut candidates,
                    &mut directory_tokens,
                )
                .unwrap(),
            CandidateBatchReservation::Reserved
        );

        let structural = shared.structural.write().unwrap();
        shared.directory.seal_structure().unwrap();
        shared.structural.mark_sealed();
        assert_eq!(
            shared
                .cleanup_fixed_candidates_if_sealed(&candidates)
                .unwrap_err(),
            directory_structure_sealed()
        );
        drop(structural);

        assert_eq!(
            table.usage(),
            TableUsage {
                retained_records: 0,
                retained_key_bytes: 0,
                consumed_record_ids: COUNT as u64,
            }
        );
        for candidate in candidates {
            assert_eq!(
                shared
                    .registry
                    .entry(candidate.id)
                    .unwrap()
                    .state
                    .load(Ordering::Acquire),
                SLOT_PROVEN_UNPUBLISHED
            );
        }

        let consumed = table.usage().consumed_record_ids();
        let keys = [[0x10; 8], [0x20; 8]];
        let mut batch = PointReadBatch::with_capacity(keys.len());
        let mut worker = runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        let mut callbacks = 0;
        let result = table.modify_fixed_visit_inner::<false, 8>(
            &mut transaction,
            None,
            &keys,
            &mut batch,
            |_index, _value| {
                callbacks += 1;
                Ok(PointMutation::Keep)
            },
            |batch| {
                for _ in &keys {
                    batch.push_record_id(None);
                }
                Ok(())
            },
        );
        assert_eq!(result, Err(directory_structure_sealed()));
        assert_eq!(callbacks, 0);
        assert!(batch.is_empty());
        transaction.abort();
        assert_eq!(table.usage().consumed_record_ids(), consumed);
        assert_eq!(table.health(), TableHealth::Healthy);
    }

    #[test]
    fn seal_waits_for_an_admitted_scan_then_scans_skip_the_rust_gate() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        seed(&table, &mut worker, &[(b"a", b"A"), (b"b", b"B")]);

        let admitted_scan = table
            .shared()
            .try_scan_structure()
            .unwrap()
            .expect("an unsealed scan must hold the structural read lock");
        let entered = Arc::new(Barrier::new(2));
        let finished = Arc::new(AtomicBool::new(false));
        std::thread::scope(|scope| {
            let table = table.clone();
            let entered_for_seal = Arc::clone(&entered);
            let finished_for_seal = Arc::clone(&finished);
            let seal = scope.spawn(move || {
                entered_for_seal.wait();
                table.seal_directory_structure().unwrap();
                finished_for_seal.store(true, Ordering::Release);
            });
            entered.wait();
            std::thread::yield_now();
            assert!(!finished.load(Ordering::Acquire));
            drop(admitted_scan);
            seal.join().unwrap();
        });
        assert!(finished.load(Ordering::Acquire));
        assert!(table.shared().try_scan_structure().unwrap().is_none());

        let structural_writer = table.shared().structural.write().unwrap();
        let mut scan = worker.begin().unwrap();
        assert_eq!(
            scan_rows(
                &table,
                &mut scan,
                ScanRequest::new(ScanDirection::Forward, 8),
            ),
            [
                (b"a".to_vec(), b"A".to_vec()),
                (b"b".to_vec(), b"B".to_vec())
            ]
        );
        committed(scan.commit());
        drop(structural_writer);
    }

    #[test]
    fn structural_scan_seam_never_blocks_a_miss_publisher() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let generation_before = table.shared().directory_generation.load(Ordering::Acquire);
        let scan_guard = table.shared().try_scan_structure().unwrap();
        let mut worker = runtime.attach().unwrap();
        let mut txn = worker.begin().unwrap();
        assert_eq!(
            table.get_inner(&mut txn, None, b"contended"),
            Err(AccessError::Conflict(Conflict::HiddenLockBusy))
        );
        assert!(txn.is_doomed());
        assert_eq!(table.health(), TableHealth::Healthy);
        assert_eq!(table.usage().retained_records(), 0);
        assert_eq!(table.usage().consumed_record_ids(), 1);
        assert_eq!(
            table.shared().directory_generation.load(Ordering::Acquire),
            generation_before
        );
        drop(scan_guard);
        txn.abort();
    }

    #[test]
    fn concurrent_first_misses_retain_one_winner_and_consume_both_candidates() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let miss_barrier = Arc::new(Barrier::new(2));
        let table = Table::new_memory_with_first_miss_barrier(
            &runtime,
            TableConfig::new()
                .with_max_retained_records(2)
                .with_max_retained_key_bytes(32)
                .with_max_consumed_record_ids(2),
            miss_barrier,
        );

        let outcomes: Vec<_> = std::thread::scope(|scope| {
            let mut handles = Vec::new();
            for _ in 0..2 {
                let runtime = Arc::clone(&runtime);
                let table = table.clone();
                handles.push(scope.spawn(move || {
                    let mut worker = runtime.attach().unwrap();
                    let mut txn = worker.begin().unwrap();
                    match table.get_inner(&mut txn, None, b"race") {
                        Ok(None) => {
                            committed(txn.commit());
                            Ok(())
                        }
                        Err(AccessError::Conflict(Conflict::HiddenLockBusy)) => {
                            txn.abort();
                            Err(Conflict::HiddenLockBusy)
                        }
                        outcome => panic!("unexpected first-miss outcome: {outcome:?}"),
                    }
                }));
            }
            handles
                .into_iter()
                .map(|handle| handle.join().unwrap())
                .collect()
        });
        assert!(outcomes.iter().any(Result::is_ok));
        assert_eq!(
            table.shared().directory_generation.load(Ordering::Acquire),
            outcomes.iter().filter(|outcome| outcome.is_ok()).count() as u64
        );
        assert_eq!(table.usage().retained_records(), 1);
        assert_eq!(table.usage().retained_key_bytes(), 4);
        assert_eq!(table.usage().consumed_record_ids(), 2);
        assert_eq!(table.health(), TableHealth::Healthy);

        let mut worker = runtime.attach().unwrap();
        let mut verify = worker.begin().unwrap();
        assert_eq!(table.get_inner(&mut verify, None, b"race").unwrap(), None);
        committed(verify.commit());
        assert_eq!(table.usage().consumed_record_ids(), 2);
    }

    #[test]
    fn ready_slots_are_resolvable_and_unknown_slots_retain_quota() {
        let registry = isolated_registry(
            TableConfig::new()
                .with_max_retained_records(2)
                .with_max_retained_key_bytes(8)
                .with_max_consumed_record_ids(2),
        );
        let ready = registry.reserve_candidate(b"ready").unwrap();
        let resolved = registry.resolve(ready.id).unwrap();
        assert!(std::ptr::eq(
            resolved,
            &registry.entry(ready.id).unwrap().record
        ));
        registry.mark_unknown(&ready).unwrap();
        assert!(matches!(
            registry.resolve(ready.id),
            Err(AccessError::Fault(_))
        ));
        assert_eq!(registry.usage().retained_records(), 1);
        assert_eq!(registry.usage().retained_key_bytes(), 5);

        let (runtime, table) = runtime_and_table(TableConfig::default());
        table.shared().mark_publication_unknown();
        assert_eq!(table.health(), TableHealth::PublicationUnknown);
        drop(runtime);
    }

    #[test]
    fn dropping_unclassified_candidate_retains_ready_slot_and_quota() {
        let registry = isolated_registry(
            TableConfig::new()
                .with_max_retained_records(1)
                .with_max_retained_key_bytes(8)
                .with_max_consumed_record_ids(2),
        );
        let record_id = {
            let candidate = registry.reserve_candidate(b"held").unwrap();
            candidate.id
        };

        assert_eq!(
            registry
                .entry(record_id)
                .unwrap()
                .state
                .load(Ordering::Acquire),
            SLOT_READY
        );
        assert_eq!(
            registry.usage(),
            TableUsage {
                retained_records: 1,
                retained_key_bytes: 4,
                consumed_record_ids: 1,
            }
        );
        assert!(matches!(
            registry.reserve_candidate(b"next"),
            Err(AccessError::Capacity(CapacityError::BufferLimit))
        ));
        assert_eq!(registry.usage().consumed_record_ids(), 1);
    }

    #[test]
    fn ready_and_published_resolution_borrow_the_same_stable_arena_record() {
        let registry = isolated_registry(
            TableConfig::new()
                .with_max_retained_records(1)
                .with_max_retained_key_bytes(8)
                .with_max_consumed_record_ids(1),
        );
        let mut candidate = registry.reserve_candidate(b"record").unwrap();
        let ready = registry.resolve(candidate.id).unwrap();
        assert!(std::ptr::eq(
            ready,
            &registry.entry(candidate.id).unwrap().record
        ));

        registry.mark_published(&mut candidate).unwrap();
        let published = registry.resolve(candidate.id).unwrap();
        assert!(std::ptr::eq(ready, published));
        assert_eq!(
            registry
                .entry(candidate.id)
                .unwrap()
                .state
                .load(Ordering::Acquire),
            SLOT_PUBLISHED
        );
    }

    #[test]
    fn concurrent_ready_resolution_survives_the_publication_transition_in_place() {
        const READERS: usize = 8;
        const RESOLVES_PER_READER: usize = 10_000;
        let registry = Arc::new(isolated_registry(
            TableConfig::new()
                .with_max_retained_records(1)
                .with_max_retained_key_bytes(8)
                .with_max_consumed_record_ids(1),
        ));
        let mut candidate = registry.reserve_candidate(b"race").unwrap();
        let record_id = candidate.id;
        let stable_address = registry.resolve(record_id).unwrap() as *const Record as usize;
        let start = Arc::new(Barrier::new(READERS + 1));

        std::thread::scope(|scope| {
            for _ in 0..READERS {
                let registry = Arc::clone(&registry);
                let start = Arc::clone(&start);
                scope.spawn(move || {
                    start.wait();
                    for _ in 0..RESOLVES_PER_READER {
                        let resolved = registry.resolve(record_id).unwrap();
                        assert_eq!(resolved as *const Record as usize, stable_address);
                    }
                });
            }

            start.wait();
            registry.mark_published(&mut candidate).unwrap();
        });

        assert_eq!(
            registry
                .entry(record_id)
                .unwrap()
                .state
                .load(Ordering::Acquire),
            SLOT_PUBLISHED
        );
    }

    #[test]
    fn concurrent_candidate_reservations_never_overcommit_retained_quota() {
        const THREADS: usize = 16;
        const LIMIT: u64 = 4;
        let registry = Arc::new(isolated_registry(
            TableConfig::new()
                .with_max_retained_records(LIMIT)
                .with_max_retained_key_bytes(1_024)
                .with_max_consumed_record_ids(100_000),
        ));
        let barrier = Arc::new(Barrier::new(THREADS));
        let mut handles = Vec::new();
        for index in 0..THREADS {
            let registry = Arc::clone(&registry);
            let barrier = Arc::clone(&barrier);
            handles.push(std::thread::spawn(move || {
                let key: Arc<[u8]> = Arc::from(format!("key-{index}").into_bytes());
                barrier.wait();
                loop {
                    match registry.reserve_candidate(key.as_ref()) {
                        Ok(candidate) => break Some(candidate),
                        Err(AccessError::Conflict(Conflict::HiddenLockBusy)) => {
                            std::thread::yield_now();
                        }
                        Err(AccessError::Capacity(CapacityError::BufferLimit)) => break None,
                        Err(error) => panic!("unexpected reservation failure: {error}"),
                    }
                }
            }));
        }
        let candidates: Vec<_> = handles
            .into_iter()
            .filter_map(|handle| handle.join().unwrap())
            .collect();
        assert_eq!(candidates.len(), LIMIT as usize);
        assert_eq!(registry.usage().retained_records(), LIMIT);
        for candidate in &candidates {
            registry.prove_unpublished(candidate).unwrap();
        }
        assert_eq!(registry.usage().retained_records(), 0);
        assert_eq!(registry.usage().retained_key_bytes(), 0);
        assert!(registry.usage().consumed_record_ids() >= LIMIT);
    }

    #[test]
    fn scans_apply_every_inclusive_and_exclusive_bound_pair() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        seed(
            &table,
            &mut worker,
            &[
                (b"a", b"A"),
                (b"b", b"B"),
                (b"c", b"C"),
                (b"d", b"D"),
                (b"e", b"E"),
            ],
        );

        let cases = [
            (
                ScanBound::Included(&b"b"[..]),
                ScanBound::Included(&b"d"[..]),
                vec![b"b".to_vec(), b"c".to_vec(), b"d".to_vec()],
            ),
            (
                ScanBound::Included(&b"b"[..]),
                ScanBound::Excluded(&b"d"[..]),
                vec![b"b".to_vec(), b"c".to_vec()],
            ),
            (
                ScanBound::Excluded(&b"b"[..]),
                ScanBound::Included(&b"d"[..]),
                vec![b"c".to_vec(), b"d".to_vec()],
            ),
            (
                ScanBound::Excluded(&b"b"[..]),
                ScanBound::Excluded(&b"d"[..]),
                vec![b"c".to_vec()],
            ),
        ];
        for (lower, upper, expected) in cases {
            let mut txn = worker.begin().unwrap();
            let rows = scan_rows(
                &table,
                &mut txn,
                ScanRequest::new(ScanDirection::Forward, 16)
                    .with_lower(lower)
                    .with_upper(upper),
            );
            assert_eq!(
                rows.into_iter().map(|(key, _)| key).collect::<Vec<_>>(),
                expected
            );
            committed(txn.commit());
        }
    }

    #[test]
    fn scans_preserve_empty_and_embedded_nul_keys_in_both_directions() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        seed(
            &table,
            &mut worker,
            &[
                (b"", b"empty"),
                (b"a", b"a"),
                (b"a\0", b"nul"),
                (b"a\0z", b"nul-z"),
                (b"b", b"b"),
            ],
        );

        let mut forward = worker.begin().unwrap();
        let forward_keys: Vec<_> = scan_rows(
            &table,
            &mut forward,
            ScanRequest::new(ScanDirection::Forward, 16),
        )
        .into_iter()
        .map(|(key, _)| key)
        .collect();
        assert_eq!(
            forward_keys,
            vec![
                b"".to_vec(),
                b"a".to_vec(),
                b"a\0".to_vec(),
                b"a\0z".to_vec(),
                b"b".to_vec(),
            ]
        );
        committed(forward.commit());

        let mut reverse = worker.begin().unwrap();
        let reverse_keys: Vec<_> = scan_rows(
            &table,
            &mut reverse,
            ScanRequest::new(ScanDirection::Reverse, 16),
        )
        .into_iter()
        .map(|(key, _)| key)
        .collect();
        assert_eq!(
            reverse_keys,
            vec![
                b"b".to_vec(),
                b"a\0z".to_vec(),
                b"a\0".to_vec(),
                b"a".to_vec(),
                b"".to_vec(),
            ]
        );
        committed(reverse.commit());
    }

    #[test]
    fn tiny_chunks_resume_without_duplicates_and_grow_the_key_arena() {
        let config = TableConfig::new()
            .with_scan_chunk_records(1)
            .with_scan_initial_key_arena_bytes(1)
            .with_scan_max_key_arena_bytes(32)
            .with_max_scan_chunks(32)
            .with_max_scan_physical_records(16);
        let (runtime, table) = runtime_and_table(config);
        let mut worker = runtime.attach().unwrap();
        seed(
            &table,
            &mut worker,
            &[
                (b"a", b"1"),
                (b"long-key", b"2"),
                (b"middle", b"3"),
                (b"z", b"4"),
            ],
        );

        for direction in [ScanDirection::Forward, ScanDirection::Reverse] {
            let mut txn = worker.begin().unwrap();
            let keys: Vec<_> = scan_rows(&table, &mut txn, ScanRequest::new(direction, 16))
                .into_iter()
                .map(|(key, _)| key)
                .collect();
            let mut expected = vec![
                b"a".to_vec(),
                b"long-key".to_vec(),
                b"middle".to_vec(),
                b"z".to_vec(),
            ];
            if direction == ScanDirection::Reverse {
                expected.reverse();
            }
            assert_eq!(keys, expected);
            committed(txn.commit());
        }
    }

    #[test]
    fn memory_scan_reports_the_exact_first_uncopied_key_size() {
        let directory = MemoryDirectory::default();
        {
            let mut entries = directory.entries.write().unwrap();
            entries.insert(b"a".to_vec(), RecordId::new(1).unwrap());
            entries.insert(b"long".to_vec(), RecordId::new(2).unwrap());
        }
        let chunk = directory
            .scan(DirectoryScanRequest {
                direction: ScanDirection::Forward,
                lower: ScanBound::Unbounded,
                upper: ScanBound::Unbounded,
                entry_capacity: 1,
                key_arena_capacity: 16,
            })
            .unwrap();
        assert_eq!(chunk.stop_reason(), ScanStopReason::EntryCapacity);
        assert_eq!(chunk.next_key_bytes_required(), b"long".len());
        assert!(matches!(
            chunk.resume(),
            DirectoryScanResumeRef::Exclusive(key) if key == b"a"
        ));
    }

    #[test]
    fn committed_scan_generation_advances_once_per_transaction_table() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let first_table = Table::new_memory_direct(&runtime, trusted_scan_config());
        let second_table = Table::new_memory_direct(&runtime, trusted_scan_config());
        let mut worker = runtime.attach().unwrap();
        seed(
            &first_table,
            &mut worker,
            &[(b"a", b"old-a"), (b"b", b"old-b")],
        );
        seed(
            &second_table,
            &mut worker,
            &[(b"c", b"old-c"), (b"d", b"old-d")],
        );
        let first_generation_before = first_table.shared().scan_generation.load(Ordering::Acquire);
        let second_generation_before = second_table
            .shared()
            .scan_generation
            .load(Ordering::Acquire);

        let mut transaction = worker.begin().unwrap();
        first_table
            .put_inner(&mut transaction, None, b"a", Value::from(&b"new-a"[..]))
            .unwrap();
        first_table
            .put_inner(&mut transaction, None, b"b", Value::from(&b"new-b"[..]))
            .unwrap();
        second_table
            .put_inner(&mut transaction, None, b"c", Value::from(&b"new-c"[..]))
            .unwrap();
        second_table
            .put_inner(&mut transaction, None, b"d", Value::from(&b"new-d"[..]))
            .unwrap();
        committed(transaction.commit());

        assert_eq!(
            first_table.shared().scan_generation.load(Ordering::Acquire),
            first_generation_before + 1
        );
        assert_eq!(
            second_table
                .shared()
                .scan_generation
                .load(Ordering::Acquire),
            second_generation_before + 1
        );
    }

    #[test]
    fn registry_token_commits_batch_scan_generation_per_table() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let table = Table::new_memory(&runtime, trusted_scan_config());
        let mut worker = runtime.attach().unwrap();
        seed(&table, &mut worker, &[(b"a", b"old-a"), (b"b", b"old-b")]);
        let generation_before = table.shared().scan_generation.load(Ordering::Acquire);

        let mut transaction = worker.begin().unwrap();
        table
            .put_inner(&mut transaction, None, b"a", Value::from(&b"new-a"[..]))
            .unwrap();
        table
            .put_inner(&mut transaction, None, b"b", Value::from(&b"new-b"[..]))
            .unwrap();
        committed(transaction.commit());

        assert_eq!(
            table.shared().scan_generation.load(Ordering::Acquire),
            generation_before + 1
        );
    }

    #[test]
    fn reused_owner_id_still_advances_each_new_commit() {
        let runtime = Runtime::new(RuntimeConfig::new().with_max_workers(1)).unwrap();
        let table = Table::new_memory_direct(&runtime, trusted_scan_config());
        let mut first_worker = runtime.attach().unwrap();
        seed(
            &table,
            &mut first_worker,
            &[(b"a", b"old-a"), (b"b", b"old-b")],
        );
        let reused_owner = first_worker.owner_id();
        let first_generation_before = table.shared().scan_generation.load(Ordering::Acquire);

        let mut first_transaction = first_worker.begin().unwrap();
        table
            .put_inner(
                &mut first_transaction,
                None,
                b"a",
                Value::from(&b"first-a"[..]),
            )
            .unwrap();
        table
            .put_inner(
                &mut first_transaction,
                None,
                b"b",
                Value::from(&b"first-b"[..]),
            )
            .unwrap();
        committed(first_transaction.commit());
        assert_eq!(
            table.shared().scan_generation.load(Ordering::Acquire),
            first_generation_before + 1
        );
        drop(first_worker);

        let mut second_worker = runtime.attach().unwrap();
        assert_eq!(second_worker.owner_id(), reused_owner);
        let second_generation_before = table.shared().scan_generation.load(Ordering::Acquire);
        let mut second_transaction = second_worker.begin().unwrap();
        table
            .put_inner(
                &mut second_transaction,
                None,
                b"a",
                Value::from(&b"second-a"[..]),
            )
            .unwrap();
        table
            .put_inner(
                &mut second_transaction,
                None,
                b"b",
                Value::from(&b"second-b"[..]),
            )
            .unwrap();
        committed(second_transaction.commit());
        assert_eq!(
            table.shared().scan_generation.load(Ordering::Acquire),
            second_generation_before + 1
        );
    }

    #[test]
    fn aborted_and_read_only_transactions_do_not_advance_scan_generation() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let table = Table::new_memory_direct(&runtime, trusted_scan_config());
        let mut worker = runtime.attach().unwrap();
        seed(&table, &mut worker, &[(b"a", b"old-a")]);
        let generation_before = table.shared().scan_generation.load(Ordering::Acquire);

        let mut read_only = worker.begin().unwrap();
        assert_eq!(
            table
                .get_inner(&mut read_only, None, b"a")
                .unwrap()
                .as_deref(),
            Some(&b"old-a"[..])
        );
        committed(read_only.commit());
        assert_eq!(
            table.shared().scan_generation.load(Ordering::Acquire),
            generation_before
        );

        let mut aborted = worker.begin().unwrap();
        table
            .put_inner(&mut aborted, None, b"a", Value::from(&b"new-a"[..]))
            .unwrap();
        aborted.abort();
        assert_eq!(
            table.shared().scan_generation.load(Ordering::Acquire),
            generation_before
        );
    }

    #[test]
    fn structural_scan_generation_remains_separate_from_commit_batch() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let table = Table::new_memory_direct(&runtime, trusted_scan_config());
        let mut worker = runtime.attach().unwrap();
        let generation_before = table.shared().scan_generation.load(Ordering::Acquire);

        let mut transaction = worker.begin().unwrap();
        table
            .put_inner(&mut transaction, None, b"a", Value::from(&b"new-a"[..]))
            .unwrap();
        table
            .put_inner(&mut transaction, None, b"b", Value::from(&b"new-b"[..]))
            .unwrap();
        assert_eq!(
            table.shared().scan_generation.load(Ordering::Acquire),
            generation_before + 2
        );
        committed(transaction.commit());

        assert_eq!(
            table.shared().scan_generation.load(Ordering::Acquire),
            generation_before + 3
        );
    }

    #[test]
    fn trusted_scan_reuses_one_generation_item_and_preserves_stop_and_tombstones() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let table = Table::new_memory_direct(
            &runtime,
            trusted_scan_config()
                .with_scan_chunk_records(1)
                .with_scan_initial_key_arena_bytes(1)
                .with_scan_max_key_arena_bytes(16),
        );
        let mut worker = runtime.attach().unwrap();
        seed(
            &table,
            &mut worker,
            &[(b"a", b"A"), (b"b", b"B"), (b"c", b"C")],
        );
        let mut remove = worker.begin().unwrap();
        table.remove_inner(&mut remove, None, b"b").unwrap();
        committed(remove.commit());

        let mut transaction = worker.begin().unwrap();
        let (outcome, rows) = trusted_scan_rows(
            &table,
            &mut transaction,
            ScanRequest::new(ScanDirection::Forward, 8),
        )
        .unwrap();
        assert_eq!(outcome.visited(), 2);
        assert!(!outcome.stopped());
        assert_eq!(
            rows,
            [
                (b"a".to_vec(), b"A".to_vec()),
                (b"c".to_vec(), b"C".to_vec()),
            ]
        );
        assert!(!transaction.has_items_for(&table.record_resource));
        assert!(transaction.has_items_for(&table.scan_resource));

        let mut scratch = ScanScratch::default();
        let mut stopped_rows = Vec::new();
        let stopped = table
            .visit_scan_bytes_trusted_inner(
                &mut transaction,
                None,
                ScanRequest::new(ScanDirection::Reverse, 8),
                &mut scratch,
                &mut |row| {
                    stopped_rows.push((row.key().to_vec(), row.value().to_vec()));
                    ScanControl::Stop
                },
            )
            .unwrap();
        assert_eq!(stopped.visited(), 1);
        assert!(stopped.stopped());
        assert_eq!(stopped_rows, [(b"c".to_vec(), b"C".to_vec())]);
        assert!(!transaction.has_items_for(&table.record_resource));
        committed(transaction.commit());
    }

    #[test]
    fn trusted_scan_falls_back_for_prior_local_state_and_reads_its_own_writes() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let table = Table::new_memory_direct(&runtime, trusted_scan_config());
        let mut worker = runtime.attach().unwrap();
        seed(&table, &mut worker, &[(b"a", b"old-a"), (b"b", b"old-b")]);

        let mut transaction = worker.begin().unwrap();
        table
            .put_inner(&mut transaction, None, b"a", Value::from(&b"new-a"[..]))
            .unwrap();
        table.remove_inner(&mut transaction, None, b"b").unwrap();
        let (outcome, rows) = trusted_scan_rows(
            &table,
            &mut transaction,
            ScanRequest::new(ScanDirection::Forward, 8),
        )
        .unwrap();
        assert_eq!(outcome.visited(), 1);
        assert_eq!(rows, [(b"a".to_vec(), b"new-a".to_vec())]);
        assert!(transaction.has_items_for(&table.record_resource));
        committed(transaction.commit());
    }

    #[test]
    fn trusted_scan_allows_a_later_own_update() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let table = Table::new_memory_direct(&runtime, trusted_scan_config());
        let mut worker = runtime.attach().unwrap();
        seed(&table, &mut worker, &[(b"a", b"before")]);

        let mut transaction = worker.begin().unwrap();
        let (_, rows) = trusted_scan_rows(
            &table,
            &mut transaction,
            ScanRequest::new(ScanDirection::Forward, 8),
        )
        .unwrap();
        assert_eq!(rows, [(b"a".to_vec(), b"before".to_vec())]);
        assert!(!transaction.has_items_for(&table.record_resource));
        table
            .put_inner(&mut transaction, None, b"a", Value::from(&b"after"[..]))
            .unwrap();
        committed(transaction.commit());

        let mut verify = worker.begin().unwrap();
        assert_eq!(
            table.get_inner(&mut verify, None, b"a").unwrap().as_deref(),
            Some(&b"after"[..])
        );
        committed(verify.commit());
    }

    #[test]
    fn trusted_scan_generation_rejects_updates_inserts_and_removes() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let table = Table::new_memory_direct(&runtime, trusted_scan_config());
        let mut setup_worker = runtime.attach().unwrap();
        seed(&table, &mut setup_worker, &[(b"a", b"A"), (b"b", b"B")]);
        drop(setup_worker);
        let mut reader_worker = runtime.attach().unwrap();

        for mutation in 0..3 {
            let mut reader = reader_worker.begin().unwrap();
            trusted_scan_rows(
                &table,
                &mut reader,
                ScanRequest::new(ScanDirection::Forward, 8),
            )
            .unwrap();
            assert!(!reader.has_items_for(&table.record_resource));

            std::thread::scope(|scope| {
                let runtime = Arc::clone(&runtime);
                let table = table.clone();
                scope
                    .spawn(move || {
                        let mut worker = runtime.attach().unwrap();
                        let mut writer = worker.begin().unwrap();
                        match mutation {
                            0 => {
                                table
                                    .put_inner(
                                        &mut writer,
                                        None,
                                        b"a",
                                        Value::from(&b"updated"[..]),
                                    )
                                    .unwrap();
                            }
                            1 => {
                                table
                                    .insert_inner(
                                        &mut writer,
                                        None,
                                        b"c",
                                        Value::from(&b"inserted"[..]),
                                    )
                                    .unwrap();
                            }
                            _ => {
                                table.remove_inner(&mut writer, None, b"b").unwrap();
                            }
                        }
                        committed(writer.commit());
                    })
                    .join()
                    .unwrap();
            });

            assert_eq!(
                reader.commit().unwrap(),
                CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
            );
        }
    }

    #[test]
    fn trusted_scan_generation_sandwich_rejects_an_update_between_rows() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let table = Table::new_memory_direct(&runtime, trusted_scan_config());
        let mut setup_worker = runtime.attach().unwrap();
        seed(&table, &mut setup_worker, &[(b"a", b"A"), (b"b", b"B")]);
        drop(setup_worker);

        let rendezvous = Arc::new(Barrier::new(2));
        let mut reader_worker = runtime.attach().unwrap();
        let mut reader = reader_worker.begin().unwrap();
        std::thread::scope(|scope| {
            let writer_runtime = Arc::clone(&runtime);
            let writer_table = table.clone();
            let writer_rendezvous = Arc::clone(&rendezvous);
            let writer = scope.spawn(move || {
                let mut worker = writer_runtime.attach().unwrap();
                writer_rendezvous.wait();
                let mut transaction = worker.begin().unwrap();
                writer_table
                    .put_inner(&mut transaction, None, b"b", Value::from(&b"updated"[..]))
                    .unwrap();
                committed(transaction.commit());
                writer_rendezvous.wait();
            });

            let mut scratch = ScanScratch::default();
            let mut callbacks = 0;
            let result = table.visit_scan_bytes_trusted_inner(
                &mut reader,
                None,
                ScanRequest::new(ScanDirection::Forward, 8),
                &mut scratch,
                &mut |_row| {
                    callbacks += 1;
                    if callbacks == 1 {
                        rendezvous.wait();
                        rendezvous.wait();
                    }
                    ScanControl::Continue
                },
            );
            assert_eq!(result, Err(AccessError::Conflict(Conflict::ReadValidation)));
            assert_eq!(callbacks, 2);
            writer.join().unwrap();
        });
        assert!(reader.is_doomed());
        reader.abort();
    }

    #[test]
    fn trusted_scan_generation_is_opt_in_for_direct_tables() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let table = Table::new_memory_direct(&runtime, TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        seed(&table, &mut worker, &[(b"a", b"A")]);
        assert_eq!(table.shared().scan_generation.load(Ordering::Acquire), 0);

        let mut transaction = worker.begin().unwrap();
        let (_, rows) = trusted_scan_rows(
            &table,
            &mut transaction,
            ScanRequest::new(ScanDirection::Forward, 8),
        )
        .unwrap();
        assert_eq!(rows, [(b"a".to_vec(), b"A".to_vec())]);
        assert!(transaction.has_items_for(&table.record_resource));
        committed(transaction.commit());
    }

    #[test]
    fn scan_reads_point_intents_for_insert_update_delete_and_cancellation() {
        let config = TableConfig::new()
            .with_scan_chunk_records(2)
            .with_scan_initial_key_arena_bytes(2)
            .with_scan_max_key_arena_bytes(32);
        let (runtime, table) = runtime_and_table(config);
        let mut worker = runtime.attach().unwrap();
        seed(
            &table,
            &mut worker,
            &[(b"a", b"old-a"), (b"b", b"old-b"), (b"d", b"old-d")],
        );

        let mut txn = worker.begin().unwrap();
        table
            .put_inner(&mut txn, None, b"a", Arc::from(&b"new-a"[..]))
            .unwrap();
        table.remove_inner(&mut txn, None, b"b").unwrap();
        table
            .insert_inner(&mut txn, None, b"c", Arc::from(&b"new-c"[..]))
            .unwrap();
        table
            .put_inner(&mut txn, None, b"e", Arc::from(&b"new-e"[..]))
            .unwrap();
        table
            .insert_inner(&mut txn, None, b"f", Arc::from(&b"temporary"[..]))
            .unwrap();
        table.remove_inner(&mut txn, None, b"f").unwrap();

        assert_eq!(
            scan_rows(
                &table,
                &mut txn,
                ScanRequest::new(ScanDirection::Forward, 16),
            ),
            vec![
                (b"a".to_vec(), b"new-a".to_vec()),
                (b"c".to_vec(), b"new-c".to_vec()),
                (b"d".to_vec(), b"old-d".to_vec()),
                (b"e".to_vec(), b"new-e".to_vec()),
            ]
        );
        committed(txn.commit());
    }

    #[test]
    fn streaming_scan_preserves_bounds_direction_tombstones_and_staged_rows() {
        let config = TableConfig::new()
            .with_scan_chunk_records(2)
            .with_scan_initial_key_arena_bytes(2)
            .with_scan_max_key_arena_bytes(32);
        let (runtime, table) = runtime_and_table(config);
        let mut worker = runtime.attach().unwrap();
        seed(
            &table,
            &mut worker,
            &[(b"a", b"old-a"), (b"b", b"old-b"), (b"d", b"old-d")],
        );

        let mut txn = worker.begin().unwrap();
        table
            .put_inner(&mut txn, None, b"a", Value::from(&b"new-a"[..]))
            .unwrap();
        table.remove_inner(&mut txn, None, b"b").unwrap();
        table
            .insert_inner(&mut txn, None, b"c", Value::from(&b"new-c"[..]))
            .unwrap();
        let mut scratch = ScanScratch::default();
        let mut rows = Vec::new();
        let outcome = table
            .visit_scan_inner(
                &mut txn,
                None,
                ScanRequest::new(ScanDirection::Forward, 8)
                    .with_lower(ScanBound::Included(b"a"))
                    .with_upper(ScanBound::Included(b"d")),
                &mut scratch,
                &mut |row| {
                    rows.push((row.key().to_vec(), row.value().to_vec()));
                    Ok(ScanControl::Continue)
                },
            )
            .unwrap();
        assert_eq!(outcome.visited(), 3);
        assert!(!outcome.stopped());
        assert_eq!(
            rows,
            [
                (b"a".to_vec(), b"new-a".to_vec()),
                (b"c".to_vec(), b"new-c".to_vec()),
                (b"d".to_vec(), b"old-d".to_vec()),
            ]
        );
        committed(txn.commit());

        let mut reverse = worker.begin().unwrap();
        rows.clear();
        let outcome = table
            .visit_scan_inner(
                &mut reverse,
                None,
                ScanRequest::new(ScanDirection::Reverse, 2)
                    .with_lower(ScanBound::Excluded(b"a"))
                    .with_upper(ScanBound::Included(b"d")),
                &mut scratch,
                &mut |row| {
                    rows.push((row.key().to_vec(), row.value().to_vec()));
                    Ok(ScanControl::Continue)
                },
            )
            .unwrap();
        assert_eq!(outcome.visited(), 2);
        assert!(!outcome.stopped());
        assert_eq!(rows[0].0, b"d");
        assert_eq!(rows[1].0, b"c");
        committed(reverse.commit());
    }

    #[test]
    fn byte_scan_streams_committed_inline_and_shared_values_plus_staged_rows() {
        let config = TableConfig::new()
            .with_scan_chunk_records(2)
            .with_scan_initial_key_arena_bytes(2)
            .with_scan_max_key_arena_bytes(64);
        let (runtime, table) = runtime_and_table(config);
        let mut worker = runtime.attach().unwrap();
        let shared = vec![0x5d; INLINE_VALUE_CAPACITY + 23];
        seed(
            &table,
            &mut worker,
            &[
                (b"a", b"inline"),
                (b"b", b"removed"),
                (b"c", shared.as_slice()),
            ],
        );
        let mut remove = worker.begin().unwrap();
        table.remove_inner(&mut remove, None, b"b").unwrap();
        committed(remove.commit());

        let mut transaction = worker.begin().unwrap();
        table
            .insert_inner(&mut transaction, None, b"d", Value::from(&b"staged"[..]))
            .unwrap();
        let mut scratch = ScanScratch::default();
        let mut rows = Vec::new();
        let outcome = table
            .visit_scan_bytes_inner(
                &mut transaction,
                None,
                ScanRequest::new(ScanDirection::Forward, 8),
                &mut scratch,
                &mut |row| {
                    rows.push((row.key().to_vec(), row.value().to_vec(), row.resolved()));
                    Ok(ScanControl::Continue)
                },
            )
            .unwrap();
        assert_eq!(outcome.visited(), 3);
        assert!(!outcome.stopped());
        assert_eq!(
            rows.iter()
                .map(|(key, value, _)| (key.as_slice(), value.as_slice()))
                .collect::<Vec<_>>(),
            [
                (&b"a"[..], &b"inline"[..]),
                (&b"c"[..], shared.as_slice()),
                (&b"d"[..], &b"staged"[..]),
            ]
        );
        assert!(rows
            .iter()
            .all(|(_, _, resolved)| table.owns_resolved(*resolved)));
        transaction.abort();
    }

    #[test]
    fn streaming_stop_observes_only_the_callback_visible_prefix() {
        let (runtime, table) = runtime_and_table(TableConfig::new().with_scan_chunk_records(16));
        let mut setup = runtime.attach().unwrap();
        seed(&table, &mut setup, &[(b"a", b"A"), (b"b", b"B")]);
        drop(setup);

        let mut scanner_worker = runtime.attach().unwrap();
        let mut scanner = scanner_worker.begin().unwrap();
        let mut scratch = ScanScratch::default();
        let mut delivered = Vec::new();
        let outcome = table
            .visit_scan_inner(
                &mut scanner,
                None,
                ScanRequest::new(ScanDirection::Forward, 16),
                &mut scratch,
                &mut |row| {
                    delivered.push(row.key().to_vec());
                    Ok(ScanControl::Stop)
                },
            )
            .unwrap();
        assert_eq!(outcome.visited(), 1);
        assert!(outcome.stopped());
        assert_eq!(delivered, [b"a".to_vec()]);
        assert_eq!(scratch.item_keys.len(), 2);

        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            scope
                .spawn(move || {
                    let mut writer_worker = runtime.attach().unwrap();
                    let mut writer = writer_worker.begin().unwrap();
                    table
                        .put_inner(&mut writer, None, b"b", Value::from(&b"changed"[..]))
                        .unwrap();
                    committed(writer.commit());
                })
                .join()
                .unwrap();
        });
        committed(scanner.commit());
    }

    #[test]
    fn streaming_batch_observes_tombstones_before_stop_but_not_its_copied_suffix() {
        let (runtime, table) = runtime_and_table(TableConfig::new().with_scan_chunk_records(16));
        let mut setup = runtime.attach().unwrap();
        seed(
            &table,
            &mut setup,
            &[(b"a", b"A"), (b"b", b"B"), (b"c", b"C")],
        );
        let mut remove = setup.begin().unwrap();
        table.remove_inner(&mut remove, None, b"a").unwrap();
        committed(remove.commit());
        drop(setup);

        let mut scanner_worker = runtime.attach().unwrap();
        let mut scanner = scanner_worker.begin().unwrap();
        let mut scratch = ScanScratch::default();
        let mut delivered = Vec::new();
        let outcome = table
            .visit_scan_inner(
                &mut scanner,
                None,
                ScanRequest::new(ScanDirection::Forward, 16),
                &mut scratch,
                &mut |row| {
                    delivered.push(row.key().to_vec());
                    Ok(ScanControl::Stop)
                },
            )
            .unwrap();
        assert_eq!(outcome.visited(), 1);
        assert!(outcome.stopped());
        assert_eq!(delivered, [b"b".to_vec()]);
        assert_eq!(scratch.item_keys.len(), 3);

        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            scope
                .spawn(move || {
                    let mut writer_worker = runtime.attach().unwrap();
                    let mut writer = writer_worker.begin().unwrap();
                    table
                        .put_inner(&mut writer, None, b"c", Value::from(&b"changed"[..]))
                        .unwrap();
                    committed(writer.commit());
                })
                .join()
                .unwrap();
        });
        committed(scanner.commit());

        let mut tombstone_scanner = scanner_worker.begin().unwrap();
        table
            .visit_scan_inner(
                &mut tombstone_scanner,
                None,
                ScanRequest::new(ScanDirection::Forward, 16),
                &mut scratch,
                &mut |_row| Ok(ScanControl::Stop),
            )
            .unwrap();
        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            scope
                .spawn(move || {
                    let mut writer_worker = runtime.attach().unwrap();
                    let mut writer = writer_worker.begin().unwrap();
                    table
                        .put_inner(&mut writer, None, b"a", Value::from(&b"resurrected"[..]))
                        .unwrap();
                    committed(writer.commit());
                })
                .join()
                .unwrap();
        });
        assert_eq!(
            tombstone_scanner.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );
    }

    #[test]
    fn scan_batch_scratch_reuses_capacity_and_repeated_scans_fall_back() {
        const RECORDS: usize = 40;
        let config = TableConfig::new().with_scan_chunk_records(RECORDS);
        let (runtime, table) = runtime_and_table(config);
        let mut worker = runtime.attach().unwrap();
        let mut seed = worker.begin().unwrap();
        for index in 0..RECORDS {
            let key = format!("key-{index:02}");
            table
                .put_inner(&mut seed, None, key.as_bytes(), Value::from(&b"value"[..]))
                .unwrap();
        }
        committed(seed.commit());

        let mut scratch = ScanScratch::default();
        let mut first = worker.begin().unwrap();
        let mut first_count = 0;
        table
            .visit_scan_inner(
                &mut first,
                None,
                ScanRequest::new(ScanDirection::Forward, RECORDS),
                &mut scratch,
                &mut |_row| {
                    first_count += 1;
                    Ok(ScanControl::Continue)
                },
            )
            .unwrap();
        assert_eq!(first_count, RECORDS);
        assert_eq!(scratch.item_keys.len(), RECORDS);
        assert_eq!(scratch.unique_order.len(), RECORDS);
        let item_keys_allocation = scratch.item_keys.as_ptr();
        let item_keys_capacity = scratch.item_keys.capacity();
        let order_allocation = scratch.unique_order.as_ptr();
        let order_capacity = scratch.unique_order.capacity();

        scratch.item_keys.clear();
        scratch.unique_order.clear();
        let mut second_count = 0;
        table
            .visit_scan_inner(
                &mut first,
                None,
                ScanRequest::new(ScanDirection::Forward, RECORDS),
                &mut scratch,
                &mut |_row| {
                    second_count += 1;
                    Ok(ScanControl::Continue)
                },
            )
            .unwrap();
        assert_eq!(second_count, RECORDS);
        assert!(scratch.item_keys.is_empty());
        assert!(scratch.unique_order.is_empty());
        committed(first.commit());

        let mut second = worker.begin().unwrap();
        table
            .visit_scan_inner(
                &mut second,
                None,
                ScanRequest::new(ScanDirection::Forward, RECORDS),
                &mut scratch,
                &mut |_row| Ok(ScanControl::Continue),
            )
            .unwrap();
        assert_eq!(scratch.item_keys.len(), RECORDS);
        assert_eq!(scratch.unique_order.len(), RECORDS);
        assert_eq!(scratch.item_keys.as_ptr(), item_keys_allocation);
        assert_eq!(scratch.item_keys.capacity(), item_keys_capacity);
        assert_eq!(scratch.unique_order.as_ptr(), order_allocation);
        assert_eq!(scratch.unique_order.capacity(), order_capacity);
        committed(second.commit());
    }

    #[test]
    fn scan_batch_uses_scalar_fallback_after_its_first_copied_chunk() {
        let config = TableConfig::new().with_scan_chunk_records(2);
        let (runtime, table) = runtime_and_table(config);
        let mut worker = runtime.attach().unwrap();
        seed(
            &table,
            &mut worker,
            &[
                (b"a", b"A"),
                (b"b", b"B"),
                (b"c", b"C"),
                (b"d", b"D"),
                (b"e", b"E"),
            ],
        );

        let mut transaction = worker.begin().unwrap();
        let mut scratch = ScanScratch::default();
        let mut keys = Vec::new();
        table
            .visit_scan_inner(
                &mut transaction,
                None,
                ScanRequest::new(ScanDirection::Forward, 5),
                &mut scratch,
                &mut |row| {
                    keys.push(row.key().to_vec());
                    Ok(ScanControl::Continue)
                },
            )
            .unwrap();

        assert_eq!(keys, [b"a", b"b", b"c", b"d", b"e"]);
        assert_eq!(scratch.item_keys.len(), 2);
        committed(transaction.commit());
    }

    #[test]
    fn streaming_scan_reports_later_errors_after_delivered_rows() {
        let config = TableConfig::new()
            .with_scan_chunk_records(1)
            .with_max_scan_chunks(8)
            .with_max_scan_physical_records(2);
        let (runtime, table) = runtime_and_table(config);
        let mut worker = runtime.attach().unwrap();
        seed(
            &table,
            &mut worker,
            &[(b"a", b"A"), (b"b", b"B"), (b"c", b"C")],
        );
        let mut remove = worker.begin().unwrap();
        table.remove_inner(&mut remove, None, b"b").unwrap();
        committed(remove.commit());

        let mut scan = worker.begin().unwrap();
        let mut scratch = ScanScratch::default();
        let mut delivered = Vec::new();
        assert_eq!(
            table.visit_scan_inner(
                &mut scan,
                None,
                ScanRequest::new(ScanDirection::Forward, 8),
                &mut scratch,
                &mut |row| {
                    delivered.push(row.key().to_vec());
                    Ok(ScanControl::Continue)
                },
            ),
            Err(AccessError::Capacity(CapacityError::BufferLimit))
        );
        assert_eq!(delivered, [b"a".to_vec()]);
        assert!(scan.is_doomed());
        scan.abort();
    }

    #[test]
    fn scan_limit_is_logical_and_zero_skips_physical_buffers() {
        let config = TableConfig::new()
            .with_scan_chunk_records(0)
            .with_scan_initial_key_arena_bytes(8)
            .with_scan_max_key_arena_bytes(8);
        let (runtime, table) = runtime_and_table(config);
        let mut worker = runtime.attach().unwrap();

        let structural_writer = table.shared().structural.try_write().unwrap();
        let mut zero = worker.begin().unwrap();
        assert!(scan_rows(
            &table,
            &mut zero,
            ScanRequest::new(ScanDirection::Forward, 0),
        )
        .is_empty());
        drop(structural_writer);
        committed(zero.commit());

        let config = TableConfig::new().with_scan_chunk_records(2);
        let (runtime, table) = runtime_and_table(config);
        let mut worker = runtime.attach().unwrap();
        seed(
            &table,
            &mut worker,
            &[(b"a", b"A"), (b"b", b"B"), (b"c", b"C")],
        );
        let mut limited = worker.begin().unwrap();
        let rows = scan_rows(
            &table,
            &mut limited,
            ScanRequest::new(ScanDirection::Reverse, 2),
        );
        assert_eq!(
            rows.into_iter().map(|(key, _)| key).collect::<Vec<_>>(),
            vec![b"c".to_vec(), b"b".to_vec()]
        );
        committed(limited.commit());
        drop(runtime);
    }

    #[test]
    fn scan_limit_does_not_consume_transaction_items_past_the_result() {
        // One item records the directory-generation observation and one
        // records the returned row. A bounded scan must not consume items for
        // rows that follow its completed logical result.
        let runtime = Runtime::new(
            RuntimeConfig::new()
                .with_max_items_per_transaction(2)
                .with_max_locks_per_transaction(2),
        )
        .unwrap();
        let table = Table::new_memory(&runtime, TableConfig::new().with_scan_chunk_records(16));
        let mut worker = runtime.attach().unwrap();
        for (key, value) in [(b"a", b"A"), (b"b", b"B"), (b"c", b"C")] {
            let mut insert = worker.begin().unwrap();
            table
                .put_inner(&mut insert, None, key, Arc::from(&value[..]))
                .unwrap();
            committed(insert.commit());
        }

        let mut forward = worker.begin().unwrap();
        assert_eq!(
            scan_rows(
                &table,
                &mut forward,
                ScanRequest::new(ScanDirection::Forward, 1),
            ),
            vec![(b"a".to_vec(), b"A".to_vec())]
        );
        committed(forward.commit());

        let mut reverse = worker.begin().unwrap();
        assert_eq!(
            scan_rows(
                &table,
                &mut reverse,
                ScanRequest::new(ScanDirection::Reverse, 1),
            ),
            vec![(b"c".to_vec(), b"C".to_vec())]
        );
        committed(reverse.commit());
    }

    #[test]
    fn aborted_read_only_miss_invalidates_an_earlier_scan() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut setup_worker = runtime.attach().unwrap();
        seed(&table, &mut setup_worker, &[(b"a", b"A")]);
        drop(setup_worker);

        let generation_before = table.shared().directory_generation.load(Ordering::Acquire);
        let mut scanner_worker = runtime.attach().unwrap();
        let mut scanner = scanner_worker.begin().unwrap();
        assert_eq!(
            scan_rows(
                &table,
                &mut scanner,
                ScanRequest::new(ScanDirection::Forward, 16),
            ),
            vec![(b"a".to_vec(), b"A".to_vec())]
        );

        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            scope
                .spawn(move || {
                    let mut miss_worker = runtime.attach().unwrap();
                    let mut miss = miss_worker.begin().unwrap();
                    assert_eq!(table.get_inner(&mut miss, None, b"tombstone"), Ok(None));
                    miss.abort();
                })
                .join()
                .unwrap();
        });
        assert_eq!(
            table.shared().directory_generation.load(Ordering::Acquire),
            generation_before + 1
        );
        assert_eq!(
            scanner.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );
    }

    #[test]
    fn existing_tombstone_resurrection_and_live_removal_use_record_validation() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut setup_worker = runtime.attach().unwrap();
        seed(&table, &mut setup_worker, &[(b"live", b"L")]);
        let mut tombstone = setup_worker.begin().unwrap();
        table
            .put_inner(
                &mut tombstone,
                None,
                b"tombstone",
                Value::from(&b"temporary"[..]),
            )
            .unwrap();
        tombstone.abort();
        drop(setup_worker);
        let generation = table.shared().directory_generation.load(Ordering::Acquire);

        let mut scanner_worker = runtime.attach().unwrap();
        let mut resurrection_scan = scanner_worker.begin().unwrap();
        assert_eq!(
            scan_rows(
                &table,
                &mut resurrection_scan,
                ScanRequest::new(ScanDirection::Forward, 16),
            ),
            vec![(b"live".to_vec(), b"L".to_vec())]
        );
        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            scope
                .spawn(move || {
                    let mut writer_worker = runtime.attach().unwrap();
                    let mut writer = writer_worker.begin().unwrap();
                    table
                        .put_inner(&mut writer, None, b"tombstone", Value::from(&b"T"[..]))
                        .unwrap();
                    committed(writer.commit());
                })
                .join()
                .unwrap();
        });
        assert_eq!(
            table.shared().directory_generation.load(Ordering::Acquire),
            generation
        );
        assert_eq!(
            resurrection_scan.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );

        let mut removal_scan = scanner_worker.begin().unwrap();
        assert_eq!(
            scan_rows(
                &table,
                &mut removal_scan,
                ScanRequest::new(ScanDirection::Forward, 16),
            ),
            vec![
                (b"live".to_vec(), b"L".to_vec()),
                (b"tombstone".to_vec(), b"T".to_vec()),
            ]
        );
        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            scope
                .spawn(move || {
                    let mut writer_worker = runtime.attach().unwrap();
                    let mut writer = writer_worker.begin().unwrap();
                    assert_eq!(
                        table
                            .remove_inner(&mut writer, None, b"live")
                            .unwrap()
                            .as_deref(),
                        Some(&b"L"[..])
                    );
                    committed(writer.commit());
                })
                .join()
                .unwrap();
        });
        assert_eq!(
            table.shared().directory_generation.load(Ordering::Acquire),
            generation
        );
        assert_eq!(
            removal_scan.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );
    }

    #[test]
    fn forward_bounded_scan_observes_preceding_tombstones_only() {
        let (runtime, table) = runtime_and_table(TableConfig::new().with_scan_chunk_records(2));
        let mut setup_worker = runtime.attach().unwrap();
        seed(&table, &mut setup_worker, &[(b"b", b"B"), (b"d", b"D")]);
        let mut intern = setup_worker.begin().unwrap();
        for key in [&b"a"[..], &b"c"[..], &b"e"[..]] {
            assert_eq!(table.get_inner(&mut intern, None, key), Ok(None));
        }
        committed(intern.commit());
        drop(setup_worker);
        let generation = table.shared().directory_generation.load(Ordering::Acquire);

        let mut scanner_worker = runtime.attach().unwrap();
        let mut unvisited_scan = scanner_worker.begin().unwrap();
        assert_eq!(
            scan_rows(
                &table,
                &mut unvisited_scan,
                ScanRequest::new(ScanDirection::Forward, 2),
            ),
            vec![
                (b"b".to_vec(), b"B".to_vec()),
                (b"d".to_vec(), b"D".to_vec())
            ]
        );
        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            scope
                .spawn(move || {
                    let mut writer_worker = runtime.attach().unwrap();
                    let mut writer = writer_worker.begin().unwrap();
                    table
                        .put_inner(&mut writer, None, b"e", Value::from(&b"E"[..]))
                        .unwrap();
                    committed(writer.commit());
                })
                .join()
                .unwrap();
        });
        assert_eq!(
            table.shared().directory_generation.load(Ordering::Acquire),
            generation
        );
        committed(unvisited_scan.commit());

        let mut preceding_scan = scanner_worker.begin().unwrap();
        assert_eq!(
            scan_rows(
                &table,
                &mut preceding_scan,
                ScanRequest::new(ScanDirection::Forward, 2),
            ),
            vec![
                (b"b".to_vec(), b"B".to_vec()),
                (b"d".to_vec(), b"D".to_vec())
            ]
        );
        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            scope
                .spawn(move || {
                    let mut writer_worker = runtime.attach().unwrap();
                    let mut writer = writer_worker.begin().unwrap();
                    table
                        .put_inner(&mut writer, None, b"c", Value::from(&b"C"[..]))
                        .unwrap();
                    committed(writer.commit());
                })
                .join()
                .unwrap();
        });
        assert_eq!(
            preceding_scan.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );
    }

    #[test]
    fn reverse_bounded_scan_observes_preceding_tombstones_only() {
        let (runtime, table) = runtime_and_table(TableConfig::new().with_scan_chunk_records(2));
        let mut setup_worker = runtime.attach().unwrap();
        seed(&table, &mut setup_worker, &[(b"b", b"B"), (b"d", b"D")]);
        let mut intern = setup_worker.begin().unwrap();
        for key in [&b"a"[..], &b"c"[..], &b"e"[..]] {
            assert_eq!(table.get_inner(&mut intern, None, key), Ok(None));
        }
        committed(intern.commit());
        drop(setup_worker);

        let mut scanner_worker = runtime.attach().unwrap();
        let mut unvisited_scan = scanner_worker.begin().unwrap();
        assert_eq!(
            scan_rows(
                &table,
                &mut unvisited_scan,
                ScanRequest::new(ScanDirection::Reverse, 2),
            ),
            vec![
                (b"d".to_vec(), b"D".to_vec()),
                (b"b".to_vec(), b"B".to_vec())
            ]
        );
        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            scope
                .spawn(move || {
                    let mut writer_worker = runtime.attach().unwrap();
                    let mut writer = writer_worker.begin().unwrap();
                    table
                        .put_inner(&mut writer, None, b"a", Value::from(&b"A"[..]))
                        .unwrap();
                    committed(writer.commit());
                })
                .join()
                .unwrap();
        });
        committed(unvisited_scan.commit());

        let mut preceding_scan = scanner_worker.begin().unwrap();
        assert_eq!(
            scan_rows(
                &table,
                &mut preceding_scan,
                ScanRequest::new(ScanDirection::Reverse, 2),
            ),
            vec![
                (b"d".to_vec(), b"D".to_vec()),
                (b"b".to_vec(), b"B".to_vec())
            ]
        );
        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            scope
                .spawn(move || {
                    let mut writer_worker = runtime.attach().unwrap();
                    let mut writer = writer_worker.begin().unwrap();
                    table
                        .put_inner(&mut writer, None, b"e", Value::from(&b"E"[..]))
                        .unwrap();
                    committed(writer.commit());
                })
                .join()
                .unwrap();
        });
        assert_eq!(
            preceding_scan.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );
    }

    #[test]
    fn scan_bounds_observe_only_included_existing_tombstones() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut setup_worker = runtime.attach().unwrap();
        seed(&table, &mut setup_worker, &[(b"b", b"B"), (b"d", b"D")]);
        let mut intern = setup_worker.begin().unwrap();
        for key in [&b"a"[..], &b"c"[..]] {
            assert_eq!(table.get_inner(&mut intern, None, key), Ok(None));
        }
        committed(intern.commit());
        drop(setup_worker);

        let request = || {
            ScanRequest::new(ScanDirection::Forward, 16)
                .with_lower(ScanBound::Included(b"b"))
                .with_upper(ScanBound::Excluded(b"d"))
        };
        let mut scanner_worker = runtime.attach().unwrap();
        let mut excluded_scan = scanner_worker.begin().unwrap();
        assert_eq!(
            scan_rows(&table, &mut excluded_scan, request()),
            vec![(b"b".to_vec(), b"B".to_vec())]
        );
        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            scope
                .spawn(move || {
                    let mut writer_worker = runtime.attach().unwrap();
                    let mut writer = writer_worker.begin().unwrap();
                    table
                        .put_inner(&mut writer, None, b"a", Value::from(&b"A"[..]))
                        .unwrap();
                    committed(writer.commit());
                })
                .join()
                .unwrap();
        });
        committed(excluded_scan.commit());

        let mut included_scan = scanner_worker.begin().unwrap();
        assert_eq!(
            scan_rows(&table, &mut included_scan, request()),
            vec![(b"b".to_vec(), b"B".to_vec())]
        );
        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            scope
                .spawn(move || {
                    let mut writer_worker = runtime.attach().unwrap();
                    let mut writer = writer_worker.begin().unwrap();
                    table
                        .put_inner(&mut writer, None, b"c", Value::from(&b"C"[..]))
                        .unwrap();
                    committed(writer.commit());
                })
                .join()
                .unwrap();
        });
        assert_eq!(
            included_scan.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );
    }

    #[test]
    fn scan_then_own_first_miss_conservatively_self_conflicts() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        seed(&table, &mut worker, &[(b"a", b"A")]);
        let generation = table.shared().directory_generation.load(Ordering::Acquire);

        let mut transaction = worker.begin().unwrap();
        assert_eq!(
            scan_rows(
                &table,
                &mut transaction,
                ScanRequest::new(ScanDirection::Forward, 16),
            ),
            vec![(b"a".to_vec(), b"A".to_vec())]
        );
        table
            .put_inner(&mut transaction, None, b"b", Value::from(&b"B"[..]))
            .unwrap();
        assert_eq!(
            table.shared().directory_generation.load(Ordering::Acquire),
            generation + 1
        );
        assert_eq!(
            transaction.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );

        let mut verify = worker.begin().unwrap();
        assert_eq!(table.get_inner(&mut verify, None, b"b"), Ok(None));
        committed(verify.commit());
    }

    #[test]
    fn physical_directory_publication_aborts_a_completed_scan() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut setup_worker = runtime.attach().unwrap();
        seed(&table, &mut setup_worker, &[(b"a", b"A")]);
        drop(setup_worker);

        let mut scanner_worker = runtime.attach().unwrap();
        let mut scanner = scanner_worker.begin().unwrap();
        assert_eq!(
            scan_rows(
                &table,
                &mut scanner,
                ScanRequest::new(ScanDirection::Forward, 16),
            ),
            vec![(b"a".to_vec(), b"A".to_vec())]
        );

        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            scope
                .spawn(move || {
                    let mut writer_worker = runtime.attach().unwrap();
                    let mut writer = writer_worker.begin().unwrap();
                    table
                        .put_inner(&mut writer, None, b"b", Arc::from(&b"B"[..]))
                        .unwrap();
                    committed(writer.commit());
                })
                .join()
                .unwrap();
        });
        assert_eq!(
            scanner.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );
    }

    #[test]
    fn direct_directory_publication_aborts_a_scan_with_a_record_write() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let table = Table::new_memory_direct(&runtime, TableConfig::default());
        let mut setup_worker = runtime.attach().unwrap();
        seed(
            &table,
            &mut setup_worker,
            &[(b"a", b"old-a"), (b"c", b"old-c")],
        );
        drop(setup_worker);

        let mut scanner_worker = runtime.attach().unwrap();
        let mut scanner = scanner_worker.begin().unwrap();
        assert_eq!(
            scan_rows(
                &table,
                &mut scanner,
                ScanRequest::new(ScanDirection::Forward, 16),
            ),
            vec![
                (b"a".to_vec(), b"old-a".to_vec()),
                (b"c".to_vec(), b"old-c".to_vec())
            ]
        );
        table
            .put_inner(&mut scanner, None, b"a", Value::from(&b"new-a"[..]))
            .unwrap();

        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            scope
                .spawn(move || {
                    let mut writer_worker = runtime.attach().unwrap();
                    let mut writer = writer_worker.begin().unwrap();
                    table
                        .put_inner(&mut writer, None, b"b", Value::from(&b"new-b"[..]))
                        .unwrap();
                    committed(writer.commit());
                })
                .join()
                .unwrap();
        });
        assert_eq!(
            scanner.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );

        let mut verify = scanner_worker.begin().unwrap();
        assert_eq!(
            table.get_inner(&mut verify, None, b"a").unwrap().as_deref(),
            Some(&b"old-a"[..])
        );
        assert_eq!(
            table.get_inner(&mut verify, None, b"b").unwrap().as_deref(),
            Some(&b"new-b"[..])
        );
        committed(verify.commit());
    }

    #[test]
    fn direct_record_removal_aborts_a_scan_with_an_unrelated_write() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let table = Table::new_memory_direct(&runtime, TableConfig::default());
        let mut setup_worker = runtime.attach().unwrap();
        seed(
            &table,
            &mut setup_worker,
            &[(b"a", b"old-a"), (b"c", b"old-c")],
        );
        drop(setup_worker);
        let generation = table.shared().directory_generation.load(Ordering::Acquire);

        let mut scanner_worker = runtime.attach().unwrap();
        let mut scanner = scanner_worker.begin().unwrap();
        assert_eq!(
            scan_rows(
                &table,
                &mut scanner,
                ScanRequest::new(ScanDirection::Forward, 16),
            )
            .len(),
            2
        );
        table
            .put_inner(&mut scanner, None, b"a", Value::from(&b"new-a"[..]))
            .unwrap();

        std::thread::scope(|scope| {
            let runtime = Arc::clone(&runtime);
            let table = table.clone();
            scope
                .spawn(move || {
                    let mut writer_worker = runtime.attach().unwrap();
                    let mut writer = writer_worker.begin().unwrap();
                    assert_eq!(
                        table
                            .remove_inner(&mut writer, None, b"c")
                            .unwrap()
                            .as_deref(),
                        Some(&b"old-c"[..])
                    );
                    committed(writer.commit());
                })
                .join()
                .unwrap();
        });
        assert_eq!(
            table.shared().directory_generation.load(Ordering::Acquire),
            generation
        );
        assert_eq!(
            scanner.commit().unwrap(),
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );

        let mut verify = scanner_worker.begin().unwrap();
        assert_eq!(
            table.get_inner(&mut verify, None, b"a").unwrap().as_deref(),
            Some(&b"old-a"[..])
        );
        assert_eq!(table.get_inner(&mut verify, None, b"c"), Ok(None));
        committed(verify.commit());
    }

    #[test]
    fn scan_structural_contention_is_retryable_and_dooms_the_transaction() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let structural_writer = table.shared().structural.try_write().unwrap();
        let mut worker = runtime.attach().unwrap();
        let mut txn = worker.begin().unwrap();
        assert_eq!(
            table.scan_inner(&mut txn, None, ScanRequest::new(ScanDirection::Forward, 1),),
            Err(AccessError::Conflict(Conflict::HiddenLockBusy))
        );
        assert!(txn.is_doomed());
        assert_eq!(table.health(), TableHealth::Healthy);
        drop(structural_writer);
        txn.abort();
    }

    #[test]
    fn physical_record_limit_counts_tombstones_and_dooms_on_overflow() {
        let config = TableConfig::new()
            .with_scan_chunk_records(2)
            .with_max_scan_physical_records(2);
        let (runtime, table) = runtime_and_table(config);
        let mut worker = runtime.attach().unwrap();
        for key in [&b"a"[..], &b"b"[..], &b"c"[..]] {
            let mut miss = worker.begin().unwrap();
            assert_eq!(table.get_inner(&mut miss, None, key).unwrap(), None);
            committed(miss.commit());
        }

        let mut scan = worker.begin().unwrap();
        assert_eq!(
            table.scan_inner(
                &mut scan,
                None,
                ScanRequest::new(ScanDirection::Forward, 16),
            ),
            Err(AccessError::Capacity(CapacityError::BufferLimit))
        );
        assert!(scan.is_doomed());
        scan.abort();
    }

    #[test]
    fn arena_growth_stops_at_the_configured_finite_limit() {
        let config = TableConfig::new()
            .with_scan_chunk_records(2)
            .with_scan_initial_key_arena_bytes(1)
            .with_scan_max_key_arena_bytes(2);
        let (runtime, table) = runtime_and_table(config);
        let mut worker = runtime.attach().unwrap();
        seed(&table, &mut worker, &[(b"long", b"value")]);

        let mut scan = worker.begin().unwrap();
        assert_eq!(
            table.scan_inner(&mut scan, None, ScanRequest::new(ScanDirection::Forward, 1),),
            Err(AccessError::Capacity(CapacityError::BufferLimit))
        );
        assert!(scan.is_doomed());
        scan.abort();
    }

    #[derive(Clone, Copy)]
    enum DifferentialBound {
        Unbounded,
        Included(&'static [u8]),
        Excluded(&'static [u8]),
    }

    impl DifferentialBound {
        fn as_scan_bound(self) -> ScanBound<'static> {
            match self {
                Self::Unbounded => ScanBound::Unbounded,
                Self::Included(key) => ScanBound::Included(key),
                Self::Excluded(key) => ScanBound::Excluded(key),
            }
        }
    }

    enum DifferentialOperation {
        Get(&'static [u8]),
        Put(&'static [u8], &'static [u8]),
        Insert(&'static [u8], &'static [u8]),
        Remove(&'static [u8]),
        Scan {
            direction: ScanDirection,
            lower: DifferentialBound,
            upper: DifferentialBound,
            limit: usize,
        },
    }

    struct DifferentialTransaction {
        commit: bool,
        operations: Vec<DifferentialOperation>,
    }

    #[derive(Default)]
    struct DifferentialCounts {
        transactions: usize,
        point_observations: usize,
        scan_observations: usize,
        commits: usize,
        aborts: usize,
        final_state_checks: usize,
    }

    fn reference_scan(
        map: &BTreeMap<Vec<u8>, Vec<u8>>,
        direction: ScanDirection,
        lower: DifferentialBound,
        upper: DifferentialBound,
        limit: usize,
    ) -> Vec<(Vec<u8>, Vec<u8>)> {
        let lower = lower.as_scan_bound();
        let upper = upper.as_scan_bound();
        let rows = map
            .iter()
            .filter(|(key, _)| key_in_bounds(key, lower, upper));
        match direction {
            ScanDirection::Forward => rows
                .take(limit)
                .map(|(key, value)| (key.clone(), value.clone()))
                .collect(),
            ScanDirection::Reverse => rows
                .rev()
                .take(limit)
                .map(|(key, value)| (key.clone(), value.clone()))
                .collect(),
        }
    }

    fn differential_transactions() -> Vec<DifferentialTransaction> {
        let mut transactions = vec![
            DifferentialTransaction {
                commit: false,
                operations: vec![
                    DifferentialOperation::Get(b""),
                    DifferentialOperation::Put(b"", b"abort-empty"),
                    DifferentialOperation::Get(b""),
                    DifferentialOperation::Insert(b"", b"ignored"),
                    DifferentialOperation::Scan {
                        direction: ScanDirection::Forward,
                        lower: DifferentialBound::Unbounded,
                        upper: DifferentialBound::Unbounded,
                        limit: 8,
                    },
                    DifferentialOperation::Remove(b""),
                    DifferentialOperation::Get(b""),
                    DifferentialOperation::Insert(b"", b"restored"),
                    DifferentialOperation::Put(b"a\0", b"abort-binary"),
                    DifferentialOperation::Scan {
                        direction: ScanDirection::Reverse,
                        lower: DifferentialBound::Included(b""),
                        upper: DifferentialBound::Unbounded,
                        limit: 8,
                    },
                ],
            },
            DifferentialTransaction {
                commit: true,
                operations: vec![
                    DifferentialOperation::Get(b""),
                    DifferentialOperation::Insert(b"", b"empty"),
                    DifferentialOperation::Put(b"a", b"A"),
                    DifferentialOperation::Insert(b"a\0", b"NUL"),
                    DifferentialOperation::Insert(b"b", b"B"),
                    DifferentialOperation::Put(b"\xff", b"FF"),
                    DifferentialOperation::Scan {
                        direction: ScanDirection::Forward,
                        lower: DifferentialBound::Unbounded,
                        upper: DifferentialBound::Included(b"b"),
                        limit: 3,
                    },
                ],
            },
            DifferentialTransaction {
                commit: false,
                operations: vec![
                    DifferentialOperation::Put(b"a", b"abort-A2"),
                    DifferentialOperation::Remove(b""),
                    DifferentialOperation::Insert(b"c", b"abort-C"),
                    DifferentialOperation::Remove(b"missing"),
                    DifferentialOperation::Scan {
                        direction: ScanDirection::Reverse,
                        lower: DifferentialBound::Excluded(b""),
                        upper: DifferentialBound::Excluded(b"\xff"),
                        limit: 32,
                    },
                ],
            },
            DifferentialTransaction {
                commit: true,
                operations: vec![
                    DifferentialOperation::Remove(b""),
                    DifferentialOperation::Put(b"a", b"A2"),
                    DifferentialOperation::Insert(b"c", b"C"),
                    DifferentialOperation::Insert(b"temp", b"temporary"),
                    DifferentialOperation::Scan {
                        direction: ScanDirection::Forward,
                        lower: DifferentialBound::Unbounded,
                        upper: DifferentialBound::Unbounded,
                        limit: 32,
                    },
                    DifferentialOperation::Remove(b"temp"),
                    DifferentialOperation::Put(b"a\0", b"NUL2"),
                    DifferentialOperation::Scan {
                        direction: ScanDirection::Reverse,
                        lower: DifferentialBound::Included(b"a"),
                        upper: DifferentialBound::Included(b"c"),
                        limit: 2,
                    },
                ],
            },
            DifferentialTransaction {
                commit: true,
                operations: vec![
                    DifferentialOperation::Insert(b"a", b"duplicate"),
                    DifferentialOperation::Remove(b"c"),
                    DifferentialOperation::Insert(b"c", b"C2"),
                    DifferentialOperation::Put(b"", b"resurrected"),
                    DifferentialOperation::Get(b"a\0"),
                    DifferentialOperation::Get(b"missing"),
                ],
            },
        ];

        let bound_pairs = [
            (DifferentialBound::Unbounded, DifferentialBound::Unbounded),
            (
                DifferentialBound::Included(b""),
                DifferentialBound::Unbounded,
            ),
            (
                DifferentialBound::Excluded(b""),
                DifferentialBound::Unbounded,
            ),
            (
                DifferentialBound::Unbounded,
                DifferentialBound::Included(b"b"),
            ),
            (
                DifferentialBound::Unbounded,
                DifferentialBound::Excluded(b"b"),
            ),
            (
                DifferentialBound::Included(b"a"),
                DifferentialBound::Included(b"\xff"),
            ),
            (
                DifferentialBound::Included(b"a"),
                DifferentialBound::Excluded(b"\xff"),
            ),
            (
                DifferentialBound::Excluded(b"a"),
                DifferentialBound::Included(b"\xff"),
            ),
            (
                DifferentialBound::Excluded(b"a"),
                DifferentialBound::Excluded(b"\xff"),
            ),
            (
                DifferentialBound::Included(b"z"),
                DifferentialBound::Excluded(b"a"),
            ),
        ];
        let mut scans = Vec::new();
        for (lower, upper) in bound_pairs {
            for direction in [ScanDirection::Forward, ScanDirection::Reverse] {
                for limit in [0, 1, 3, 64] {
                    scans.push(DifferentialOperation::Scan {
                        direction,
                        lower,
                        upper,
                        limit,
                    });
                }
            }
        }
        transactions.push(DifferentialTransaction {
            commit: true,
            operations: scans,
        });
        transactions
    }

    #[test]
    fn deterministic_sequential_histories_match_btree_reference() {
        let config = TableConfig::new()
            .with_scan_chunk_records(2)
            .with_scan_initial_key_arena_bytes(1)
            .with_scan_max_key_arena_bytes(32)
            .with_max_scan_chunks(256)
            .with_max_scan_physical_records(128);
        let (runtime, table) = runtime_and_table(config);
        let mut worker = runtime.attach().unwrap();
        let mut committed_model = BTreeMap::<Vec<u8>, Vec<u8>>::new();
        let mut counts = DifferentialCounts::default();

        for (transaction_index, specification) in
            differential_transactions().into_iter().enumerate()
        {
            counts.transactions += 1;
            let mut working_model = committed_model.clone();
            let mut txn = worker.begin().unwrap();
            for (operation_index, operation) in specification.operations.into_iter().enumerate() {
                match operation {
                    DifferentialOperation::Get(key) => {
                        counts.point_observations += 1;
                        let actual = table
                            .get_inner(&mut txn, None, key)
                            .unwrap()
                            .map(|value| value.to_vec());
                        let expected = working_model.get(key).cloned();
                        assert_eq!(
                            actual, expected,
                            "get at {transaction_index}:{operation_index}"
                        );
                    }
                    DifferentialOperation::Put(key, value) => {
                        counts.point_observations += 1;
                        let actual = table
                            .put_inner(&mut txn, None, key, Arc::from(value))
                            .unwrap()
                            .map(|snapshot| snapshot.to_vec());
                        let expected = working_model.insert(key.to_vec(), value.to_vec());
                        assert_eq!(
                            actual, expected,
                            "put at {transaction_index}:{operation_index}"
                        );
                    }
                    DifferentialOperation::Insert(key, value) => {
                        counts.point_observations += 1;
                        let actual = table
                            .insert_inner(&mut txn, None, key, Arc::from(value))
                            .unwrap();
                        let expected = match working_model.get(key) {
                            Some(existing) => {
                                InsertOutcome::AlreadyPresent(Value::from(existing.clone()))
                            }
                            None => {
                                working_model.insert(key.to_vec(), value.to_vec());
                                InsertOutcome::Inserted
                            }
                        };
                        assert_eq!(
                            actual, expected,
                            "insert at {transaction_index}:{operation_index}"
                        );
                    }
                    DifferentialOperation::Remove(key) => {
                        counts.point_observations += 1;
                        let actual = table
                            .remove_inner(&mut txn, None, key)
                            .unwrap()
                            .map(|snapshot| snapshot.to_vec());
                        let expected = working_model.remove(key);
                        assert_eq!(
                            actual, expected,
                            "remove at {transaction_index}:{operation_index}"
                        );
                    }
                    DifferentialOperation::Scan {
                        direction,
                        lower,
                        upper,
                        limit,
                    } => {
                        counts.scan_observations += 1;
                        let actual = scan_rows(
                            &table,
                            &mut txn,
                            ScanRequest::new(direction, limit)
                                .with_lower(lower.as_scan_bound())
                                .with_upper(upper.as_scan_bound()),
                        );
                        let expected =
                            reference_scan(&working_model, direction, lower, upper, limit);
                        assert_eq!(
                            actual, expected,
                            "scan at {transaction_index}:{operation_index}"
                        );
                    }
                }
            }

            if specification.commit {
                counts.commits += 1;
                committed(txn.commit());
                committed_model = working_model;
            } else {
                counts.aborts += 1;
                assert_eq!(txn.abort().reason(), &AbortReason::Explicit);
            }

            let mut verify = worker.begin().unwrap();
            let actual = scan_rows(
                &table,
                &mut verify,
                ScanRequest::new(ScanDirection::Forward, usize::MAX),
            );
            let expected: Vec<_> = committed_model
                .iter()
                .map(|(key, value)| (key.clone(), value.clone()))
                .collect();
            assert_eq!(
                actual, expected,
                "final state after transaction {transaction_index}"
            );
            committed(verify.commit());
            counts.final_state_checks += 1;
        }

        assert_eq!(counts.transactions, 6);
        assert_eq!(counts.point_observations, 30);
        assert_eq!(counts.scan_observations, 86);
        assert_eq!(counts.commits, 4);
        assert_eq!(counts.aborts, 2);
        assert_eq!(counts.final_state_checks, 6);
    }
}
