#![deny(unsafe_code)]

//! Transactional binary-key records over the safe Masstree directory.
//!
//! Masstree owns only the append-only `key -> RecordId` index. This crate owns
//! stable registry slots, immutable value/tombstone snapshots, OCC versions,
//! physical locks, and the table membership resource. Point misses eagerly
//! intern a tombstone, but every abstract mutation remains deferred until the
//! native Rust STO commit protocol installs it.

mod record_prefetch;

use arc_swap::ArcSwapOption;
use std::{
    borrow::Borrow,
    collections::{BTreeMap, BTreeSet},
    fmt,
    ops::Deref,
    sync::{
        atomic::{AtomicU64, AtomicU8, Ordering},
        Arc, OnceLock, RwLock, RwLockReadGuard, RwLockWriteGuard, TryLockError,
    },
};

#[cfg(not(test))]
use masstree::{
    Error as MasstreeError, InsertError as MasstreeInsertError, NativeStatus,
    PublicationDisposition, ReadScope as NativeReadScope, ScanRequest as NativeScanRequest,
    ScanResume as NativeScanResume, Tree,
};
use masstree::{
    InsertOutcome as DirectoryInsertOutcome, PointReadResult as DirectoryPointReadResult, RecordId,
    ScanStopReason, Worker,
};
pub use masstree::{KeyBound as ScanBound, ScanDirection};
use sto_core::{
    AccessError, AcquireContext, AcquireError, Active, AdapterFault, AdapterFaultKind,
    AdapterPhase, AtomicVersion, CapacityError, CheckError, Conflict, DetachedVersionGuard, Entry,
    ExecutionCheckContext, FinishContext, FinishDisposition, FinishItem, InstallContext,
    InstallItem, LockClass, LockDisposition, LockIdentity, LockNamespaceId, LockRequest, LockUse,
    NoPredicate, ObjectId, ObservationOrder, ObservationRef, OccVersion, OpacityToken,
    PredicateContext, PreflightContext, PreflightFreeReadCapability,
    PreflightFreeValidationContext, PreflightItem, PrepareError, RegisteredResource,
    RegistrationError, ReleaseContext, ResourceClass, Runtime, Transaction, TransactionLock,
    TransactionalResource, UniqueItemKeys, ValidationContext, VersionLock,
};
#[cfg(not(test))]
use sto_core::{FailurePhase, InvalidUse, PoisonInfo};

const RECORD_RESOURCE_CLASS_VALUE: u32 = 1;
const MEMBERSHIP_RESOURCE_CLASS_VALUE: u32 = 2;
const MEMBERSHIP_LOCK_CLASS_VALUE: u32 = 1;
const RECORD_LOCK_CLASS_VALUE: u32 = 2;
const MEMBERSHIP_LOCK_KEY: u64 = 0;

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
// tags only make the physical snapshot copy race-free without a read-side
// lock. Empty inline values and tombstones deliberately have distinct tags.
const RECORD_STATE_TOMBSTONE: u8 = 0;
const RECORD_STATE_SHARED: u8 = 1;
const RECORD_STATE_INLINE_BASE: u8 = 2;
const RECORD_STATE_UPDATING: u8 = u8::MAX - 1;
const RECORD_STATE_POISONED: u8 = u8::MAX;

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
const REGISTRY_ENTRY_SLOT_BYTES: usize = 48;

// Keep the benchmark's common u64-sized values entirely inside the STO item.
// Larger values retain cheap, immutable snapshot clones through a thin Arc;
// the Vec owns the allocation containing the immutable bytes.
const INLINE_VALUE_CAPACITY: usize = 8;

/// An immutable binary value snapshot returned by point operations.
///
/// Short values are stored inline and clone without allocation or shared
/// reference-count traffic. Larger values use immutable shared storage. Empty
/// and arbitrary binary values (including embedded NUL bytes) are preserved.
#[derive(Clone)]
pub struct Value {
    repr: ValueRepr,
}

#[derive(Clone)]
enum ValueRepr {
    Inline {
        len: u8,
        bytes: [u8; INLINE_VALUE_CAPACITY],
    },
    Shared(Arc<Vec<u8>>),
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
                repr: ValueRepr::Shared(Arc::new(bytes.to_vec())),
            }
        }
    }
}

impl AsRef<[u8]> for Value {
    #[inline]
    fn as_ref(&self) -> &[u8] {
        match &self.repr {
            ValueRepr::Inline { len, bytes } => &bytes[..usize::from(*len)],
            ValueRepr::Shared(bytes) => bytes.as_slice(),
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
                repr: ValueRepr::Shared(Arc::new(bytes)),
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
                repr: ValueRepr::Shared(Arc::new(bytes.into_vec())),
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
    unique_record_ids: Vec<RecordId>,
    values: Vec<Option<Value>>,
    membership_updates: Vec<MembershipUpdate>,
}

impl PointReadBatch {
    /// Creates empty scratch storage.
    pub const fn new() -> Self {
        Self {
            directory_results: Vec::new(),
            record_ids: Vec::new(),
            unique_record_ids: Vec::new(),
            values: Vec::new(),
            membership_updates: Vec::new(),
        }
    }

    /// Creates empty scratch storage sized for at least `capacity` reads.
    pub fn with_capacity(capacity: usize) -> Self {
        Self {
            directory_results: Vec::with_capacity(capacity),
            record_ids: Vec::with_capacity(capacity),
            unique_record_ids: Vec::with_capacity(capacity),
            values: Vec::with_capacity(capacity),
            membership_updates: Vec::with_capacity(capacity),
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
            .min(self.unique_record_ids.capacity())
            .min(self.values.capacity())
            .min(self.membership_updates.capacity())
    }

    /// Returns the snapshots from the last successful call in input order.
    pub fn results(&self) -> &[Option<Value>] {
        &self.values
    }

    /// Drops prior snapshots while retaining all scratch allocations.
    pub fn clear(&mut self) {
        self.directory_results.clear();
        self.record_ids.clear();
        self.unique_record_ids.clear();
        self.values.clear();
        self.membership_updates.clear();
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
        self.unique_record_ids
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
        self.prepare_read::<CAPTURE_VALUES>(length)?;
        self.membership_updates
            .try_reserve_exact(length)
            .map_err(|_| CapacityError::BufferLimit)?;
        Ok(())
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
    /// (currently 48 bytes each) and one shared lock target per 16 slots, even
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
    scan_chunk_records: usize,
    scan_initial_key_arena_bytes: usize,
    scan_max_key_arena_bytes: usize,
    max_scan_chunks: usize,
    max_scan_physical_records: usize,
}

impl TableConfig {
    /// Conservative, explicitly bounded defaults.
    pub const fn new() -> Self {
        Self {
            max_retained_records: 1_000_000,
            max_retained_key_bytes: 1 << 30,
            max_consumed_record_ids: 4_000_000,
            registry_layout: RegistryLayout::LazySegmented,
            scan_chunk_records: 128,
            scan_initial_key_arena_bytes: 16 * 1024,
            scan_max_key_arena_bytes: 64 * 1024,
            max_scan_chunks: 4_096,
            max_scan_physical_records: 1_000_000,
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

/// A cloneable transactional table backed by one safe Masstree tree.
///
/// The supplied native worker remains an operation-scoped capability. It is
/// never stored in the table, an STO item, or an adapter callback.
pub struct Table {
    record_resource: RegisteredResource<RecordAdapter>,
    membership_resource: RegisteredResource<MembershipAdapter>,
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

    fn with_directory(
        runtime: &Arc<Runtime>,
        directory: Directory,
        config: TableConfig,
    ) -> Result<Self, RegistrationError> {
        let object = runtime.register_object()?;
        let namespace = LockNamespaceId::new(object.object_id().get())
            .expect("nonzero ObjectId always forms a lock namespace");
        let membership_lock_class = LockClass::new(MEMBERSHIP_LOCK_CLASS_VALUE)
            .expect("the membership lock class is nonzero");
        let record_lock_class =
            LockClass::new(RECORD_LOCK_CLASS_VALUE).expect("the record lock class is nonzero");

        let membership_version = Arc::new(AtomicVersion::default());
        let membership_lock = Arc::new(VersionLock::new(Arc::clone(&membership_version)));
        let membership_identity = LockIdentity::new(
            object.runtime_id(),
            namespace,
            membership_lock_class,
            MEMBERSHIP_LOCK_KEY,
        );
        let shared = Arc::new(TableShared {
            directory,
            registry: Registry::new(config, object.runtime_id(), namespace, record_lock_class)?,
            structural: StructuralGate::default(),
            health: AtomicU8::new(TABLE_HEALTHY),
            runtime_id: object.runtime_id(),
            namespace,
            record_lock_class,
            membership_version,
            membership_lock,
            membership_identity,
        });

        let record_class = ResourceClass::new(RECORD_RESOURCE_CLASS_VALUE)
            .expect("the record resource class is nonzero");
        let membership_class = ResourceClass::new(MEMBERSHIP_RESOURCE_CLASS_VALUE)
            .expect("the membership resource class is nonzero");
        let record_resource = object.register_resource(
            record_class,
            RecordAdapter {
                table: Arc::clone(&shared),
            },
        )?;
        let membership_resource = object.register_resource(
            membership_class,
            MembershipAdapter {
                table: Arc::clone(&shared),
            },
        )?;

        Ok(Self {
            record_resource,
            membership_resource,
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

    /// Reads the staged value or the first validated committed snapshot.
    #[inline]
    pub fn get(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
    ) -> Result<Option<Value>, AccessError> {
        self.get_inner(txn, Some(worker), key)
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
        self.put_inner(txn, Some(worker), key, Value::from(value))
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
        self.insert_inner(txn, Some(worker), key, Value::from(value))
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

    /// Returns an owned, bounded transactional range snapshot.
    ///
    /// The scan observes the table membership resource, validates every
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

    pub fn object_id(&self) -> ObjectId {
        self.record_resource.object_id()
    }

    pub fn health(&self) -> TableHealth {
        self.shared().health()
    }

    pub fn usage(&self) -> TableUsage {
        self.shared().registry.usage()
    }

    #[inline]
    fn get_inner(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
    ) -> Result<Option<Value>, AccessError> {
        self.get_inner_with_lookup(txn, worker, key, || self.shared().lookup(worker, key))
    }

    #[inline]
    fn get_inner_with_lookup(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        lookup: impl FnOnce() -> Result<Option<RecordId>, AccessError>,
    ) -> Result<Option<Value>, AccessError> {
        let adapter = self.record_resource.adapter();
        self.with_key_record_lookup(txn, worker, key, lookup, |entry, record_id| {
            adapter.prepare_access(record_id, entry)?;
            Ok(current_state(entry).and_then(RecordState::value).cloned())
        })
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
            for record_id in batch.record_ids.iter().flatten().copied() {
                shared
                    .registry
                    .prefetch(record_id)
                    .inspect_err(|error| shared.note_access_error(error))?;
            }

            batch
                .unique_record_ids
                .extend(batch.record_ids.iter().flatten().copied());
            if batch.unique_record_ids.len() == batch.record_ids.len() {
                if let Some(unique) = UniqueItemKeys::try_new(&batch.unique_record_ids) {
                    let record_ids = &batch.unique_record_ids;
                    let values = &mut batch.values;
                    if items.try_with_unique_item_batch(unique, |index, entry| {
                        visit_fixed_value::<CAPTURE_VALUES>(
                            adapter,
                            entry,
                            record_ids[index],
                            index,
                            &mut visit,
                            values,
                        )
                    })? {
                        return Ok(());
                    }
                } else if keys
                    .iter()
                    .enumerate()
                    .all(|(index, key)| !keys[..index].contains(key))
                {
                    // Equal input keys legitimately resolve to one ID and use
                    // the sequential fallback. Distinct keys aliasing one ID
                    // would violate the private append-only directory binding.
                    shared.poison();
                    return Err(table_fault(
                        "distinct directory keys resolved to one record ID",
                    ));
                }
            }

            for (index, key) in keys.iter().enumerate() {
                let batched_record_id = batch.record_ids[index];
                items.with_resolved_item(
                    || {
                        let found = match batched_record_id {
                            Some(record_id) => Some(record_id),
                            None => shared.lookup(worker, key)?,
                        };
                        if batched_record_id.is_none() {
                            if let Some(record_id) = found {
                                shared.registry.prefetch(record_id)?;
                            }
                        }
                        Ok(found.map(|record_id| (record_id, record_id)))
                    },
                    || {
                        shared
                            .intern_missing(worker, key)
                            .map(|record_id| (record_id, record_id))
                    },
                    |entry, record_id| {
                        visit_fixed_value::<CAPTURE_VALUES>(
                            adapter,
                            entry,
                            record_id,
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
        mut modify: impl for<'value> FnMut(usize, Option<&'value Value>) -> PointMutation,
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
            // Resolve the stable registry addresses as one locality wave
            // before item-index work can evict the directory result set.
            for record_id in batch.record_ids.iter().flatten().copied() {
                shared
                    .registry
                    .prefetch(record_id)
                    .inspect_err(|error| shared.note_access_error(error))?;
            }
            batch
                .unique_record_ids
                .extend(batch.record_ids.iter().flatten().copied());
            if batch.unique_record_ids.len() == batch.record_ids.len() {
                if let Some(unique) = UniqueItemKeys::try_new(&batch.unique_record_ids) {
                    let record_ids = &batch.unique_record_ids;
                    let values = &mut batch.values;
                    let membership_updates = &mut batch.membership_updates;
                    if items.try_with_unique_item_batch(unique, |index, entry| {
                        apply_fixed_mutation::<CAPTURE_VALUES>(
                            adapter,
                            entry,
                            record_ids[index],
                            index,
                            &mut modify,
                            values,
                            membership_updates,
                        )
                    })? {
                        return Ok(());
                    }
                } else if keys
                    .iter()
                    .enumerate()
                    .all(|(index, key)| !keys[..index].contains(key))
                {
                    // Equal input keys legitimately resolve to one ID and use
                    // the sequential fallback. Distinct keys aliasing one ID
                    // would violate the private append-only directory binding.
                    shared.poison();
                    return Err(table_fault(
                        "distinct directory keys resolved to one record ID",
                    ));
                }
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
                        if batched_record_id.is_none() {
                            if let Some(record_id) = found {
                                shared.registry.prefetch(record_id)?;
                            }
                        }
                        Ok(found.map(|record_id| (record_id, record_id)))
                    },
                    || {
                        shared
                            .intern_missing(worker, key)
                            .map(|record_id| (record_id, record_id))
                    },
                    |entry, record_id| {
                        apply_fixed_mutation::<CAPTURE_VALUES>(
                            adapter,
                            entry,
                            record_id,
                            index,
                            &mut modify,
                            &mut batch.values,
                            &mut batch.membership_updates,
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

        for index in 0..batch.membership_updates.len() {
            let update = batch.membership_updates[index];
            if let Err(error) =
                self.apply_membership_effect(txn, RecordEffect::write((), Some(update)))
            {
                batch.clear();
                return Err(error);
            }
        }
        batch.membership_updates.clear();
        Ok(keys.len())
    }

    fn modify_fixed_inner<'batch, const KEY_LENGTH: usize>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        keys: &[[u8; KEY_LENGTH]],
        batch: &'batch mut PointReadBatch,
        modify: impl for<'value> FnMut(usize, Option<&'value Value>) -> PointMutation,
        lookup_batch: impl FnOnce(&mut PointReadBatch) -> Result<(), AccessError>,
    ) -> Result<&'batch [Option<Value>], AccessError> {
        self.modify_fixed_visit_inner::<true, KEY_LENGTH>(
            txn,
            worker,
            keys,
            batch,
            modify,
            lookup_batch,
        )?;
        Ok(batch.results())
    }

    #[inline]
    fn put_inner<V: Into<Value>>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        value: V,
    ) -> Result<Option<Value>, AccessError> {
        self.put_inner_with_lookup(txn, worker, key, value.into(), || {
            self.shared().lookup(worker, key)
        })
    }

    #[inline]
    fn put_inner_with_lookup(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        value: Value,
        lookup: impl FnOnce() -> Result<Option<RecordId>, AccessError>,
    ) -> Result<Option<Value>, AccessError> {
        let adapter = self.record_resource.adapter();
        let operation =
            self.with_key_record_lookup(txn, worker, key, lookup, move |entry, record_id| {
                adapter.prepare_access(record_id, entry)?;
                let previous = current_state(entry).and_then(RecordState::value).cloned();
                let membership = membership_transition(entry, record_id, true)?;
                let replacement = RecordState::Live(value);
                stage_record_state(entry, replacement)?;
                Ok(RecordEffect::write(previous, membership))
            })?;
        self.apply_membership_effect(txn, operation)
    }

    #[inline]
    fn insert_inner<V: Into<Value>>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        value: V,
    ) -> Result<InsertOutcome, AccessError> {
        self.insert_inner_with_lookup(txn, worker, key, value.into(), || {
            self.shared().lookup(worker, key)
        })
    }

    #[inline]
    fn insert_inner_with_lookup(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        value: Value,
        lookup: impl FnOnce() -> Result<Option<RecordId>, AccessError>,
    ) -> Result<InsertOutcome, AccessError> {
        let adapter = self.record_resource.adapter();
        let operation =
            self.with_key_record_lookup(txn, worker, key, lookup, move |entry, record_id| {
                adapter.prepare_access(record_id, entry)?;
                if let Some(current) = current_state(entry).and_then(RecordState::value) {
                    return Ok(RecordEffect::read(InsertOutcome::AlreadyPresent(
                        current.clone(),
                    )));
                }
                let membership = membership_transition(entry, record_id, true)?;
                let replacement = RecordState::Live(value);
                stage_record_state(entry, replacement)?;
                Ok(RecordEffect::write(InsertOutcome::Inserted, membership))
            })?;
        self.apply_membership_effect(txn, operation)
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
    fn remove_inner_with_lookup(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        lookup: impl FnOnce() -> Result<Option<RecordId>, AccessError>,
    ) -> Result<Option<Value>, AccessError> {
        let adapter = self.record_resource.adapter();
        let operation =
            self.with_key_record_lookup(txn, worker, key, lookup, |entry, record_id| {
                adapter.prepare_access(record_id, entry)?;
                let Some(current) = current_state(entry).and_then(RecordState::value) else {
                    return Ok(RecordEffect::read(None));
                };
                let previous = Some(current.clone());
                let membership = membership_transition(entry, record_id, false)?;
                let replacement = RecordState::tombstone();
                stage_record_state(entry, replacement)?;
                Ok(RecordEffect::write(previous, membership))
            })?;
        self.apply_membership_effect(txn, operation)
    }

    fn scan_inner(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        request: ScanRequest<'_>,
    ) -> Result<Vec<ScanRecord>, AccessError> {
        let membership = self.membership_resource.adapter();
        txn.with_item(&self.membership_resource, (), |entry| {
            membership.observe(entry).map(|_| ())
        })?;
        if request.limit == 0 || range_is_empty(request.lower, request.upper) {
            return Ok(Vec::new());
        }

        let config = self.shared().registry.config;
        if config.scan_chunk_records == 0
            || config.max_scan_chunks == 0
            || config.scan_initial_key_arena_bytes > config.scan_max_key_arena_bytes
        {
            return self.fail_scan(txn, CapacityError::BufferLimit.into());
        }

        let structural = txn.with_item(&self.membership_resource, (), |_entry| {
            self.shared().ensure_healthy()?;
            self.shared().try_scan_structure()
        })?;
        let result = self.scan_while_structurally_stable(txn, worker, request, config);
        drop(structural);
        result
    }

    fn scan_while_structurally_stable(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        request: ScanRequest<'_>,
        config: TableConfig,
    ) -> Result<Vec<ScanRecord>, AccessError> {
        let mut base = BTreeMap::<RecordKey, ScannedRecord>::new();
        let mut resume_key: Option<Box<[u8]>> = None;
        let mut previous_key: Option<Box<[u8]>> = None;
        let mut arena_capacity = config.scan_initial_key_arena_bytes;
        let mut physical_records = 0_usize;
        let mut chunks = 0_usize;

        loop {
            if chunks >= config.max_scan_chunks {
                return self.fail_scan(txn, CapacityError::BufferLimit.into());
            }
            chunks += 1;

            let (lower, upper) = resumed_bounds(request, resume_key.as_deref());
            let directory_request = DirectoryScanRequest {
                direction: request.direction,
                lower,
                upper,
                entry_capacity: config.scan_chunk_records,
                key_arena_capacity: arena_capacity,
            };
            let chunk = txn.with_item(&self.membership_resource, (), |_entry| {
                self.shared().ensure_healthy()?;
                let chunk = self
                    .shared()
                    .directory
                    .scan(worker, directory_request)
                    .inspect_err(|error| self.shared().note_access_error(error))?;
                validate_directory_chunk(
                    &chunk,
                    request.direction,
                    request.lower,
                    request.upper,
                    previous_key.as_deref(),
                )
                .inspect_err(|error| self.shared().note_access_error(error))?;
                Ok(chunk)
            })?;

            let next_physical = physical_records
                .checked_add(chunk.entries.len())
                .ok_or(CapacityError::BufferLimit);
            let next_physical = match next_physical {
                Ok(count) if count <= config.max_scan_physical_records => count,
                _ => return self.fail_scan(txn, CapacityError::BufferLimit.into()),
            };
            physical_records = next_physical;

            for copied in &chunk.entries {
                let key = RecordKey(Arc::from(copied.key.clone()));
                let record_id = copied.record_id;
                let adapter = self.record_resource.adapter();
                let snapshot = txn.with_resolved_item(
                    &self.record_resource,
                    || Ok(Some((record_id, record_id))),
                    || Err(table_fault("stable directory record disappeared")),
                    |entry, record_id| {
                        adapter.prepare_access(record_id, entry)?;
                        current_state_snapshot(entry).cloned()
                    },
                )?;
                if base
                    .insert(key, ScannedRecord { state: snapshot })
                    .is_some()
                {
                    return self
                        .fail_scan(txn, table_fault("directory scan returned a duplicate key"));
                }
            }

            if let Some(last) = chunk.entries.last() {
                previous_key = Some(last.key.clone());
            }

            match (&chunk.stop_reason, &chunk.resume) {
                (ScanStopReason::End, DirectoryScanResume::None) => break,
                (
                    ScanStopReason::EntryCapacity | ScanStopReason::KeyArenaCapacity,
                    DirectoryScanResume::Exclusive(key),
                ) => {
                    if physical_records >= config.max_scan_physical_records {
                        return self.fail_scan(txn, CapacityError::BufferLimit.into());
                    }
                    resume_key = Some(key.clone());
                }
                (ScanStopReason::KeyArenaCapacity, DirectoryScanResume::UnchangedInput) => {
                    if chunk.next_key_bytes_required == 0 {
                        return self.fail_scan(
                            txn,
                            table_fault("arena-limited scan reported no required size"),
                        );
                    }
                    let doubled = arena_capacity.saturating_mul(2);
                    let grown = doubled.max(chunk.next_key_bytes_required).max(1);
                    if grown > config.scan_max_key_arena_bytes || grown <= arena_capacity {
                        return self.fail_scan(txn, CapacityError::BufferLimit.into());
                    }
                    arena_capacity = grown;
                }
                (ScanStopReason::EntryCapacity, DirectoryScanResume::UnchangedInput) => {
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

        let result_capacity = request.limit.min(base.len());
        let mut result = Vec::new();
        if result.try_reserve_exact(result_capacity).is_err() {
            return self.fail_scan(txn, CapacityError::BufferLimit.into());
        }
        match request.direction {
            ScanDirection::Forward => {
                for (key, scanned) in base {
                    if let Some(value) = scanned.state.value() {
                        result.push(ScanRecord {
                            key: key.0,
                            value: value.clone(),
                        });
                        if result.len() == request.limit {
                            break;
                        }
                    }
                }
            }
            ScanDirection::Reverse => {
                for (key, scanned) in base.into_iter().rev() {
                    if let Some(value) = scanned.state.value() {
                        result.push(ScanRecord {
                            key: key.0,
                            value: value.clone(),
                        });
                        if result.len() == request.limit {
                            break;
                        }
                    }
                }
            }
        }
        Ok(result)
    }

    fn fail_scan<R>(
        &self,
        txn: &mut Transaction<'_, Active>,
        error: AccessError,
    ) -> Result<R, AccessError> {
        txn.with_item(&self.membership_resource, (), |_entry| Err(error))
    }

    #[inline]
    fn apply_membership_effect<R>(
        &self,
        txn: &mut Transaction<'_, Active>,
        effect: RecordEffect<R>,
    ) -> Result<R, AccessError> {
        let Some(update) = effect.membership else {
            return Ok(effect.result);
        };
        let adapter = self.membership_resource.adapter();
        txn.with_item(&self.membership_resource, (), |entry| {
            adapter.update(update, entry)
        })?;
        Ok(effect.result)
    }

    #[inline]
    fn with_key_record_lookup<R>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        lookup: impl FnOnce() -> Result<Option<RecordId>, AccessError>,
        operation: impl for<'entry> FnOnce(
            &mut Entry<'entry, RecordAdapter>,
            RecordId,
        ) -> Result<R, AccessError>,
    ) -> Result<R, AccessError> {
        let shared = self.shared();
        txn.with_resolved_item(
            &self.record_resource,
            || {
                let found = lookup()?;
                if let Some(record_id) = found {
                    shared.registry.prefetch(record_id)?;
                }
                Ok(found.map(|record_id| (record_id, record_id)))
            },
            || {
                self.shared()
                    .intern_missing(worker, key)
                    .map(|record_id| (record_id, record_id))
            },
            operation,
        )
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
}

impl<'session, 'context> PointSession<'session, 'context> {
    /// Reads the staged value or first validated committed snapshot.
    #[inline]
    pub fn get(&mut self, key: &[u8]) -> Result<Option<Value>, AccessError> {
        #[cfg(not(test))]
        {
            let table = self.table;
            let worker = self.worker;
            let read_scope = &mut self.read_scope;
            table.get_inner_with_lookup(&mut *self.transaction, Some(worker), key, || {
                table.lookup_in_read_scope(worker, read_scope, key)
            })
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
    /// Membership changes remain deferred until all record callbacks finish.
    /// On success, the returned count equals `keys.len()` and
    /// [`PointReadBatch::results`] is empty.
    ///
    /// If access fails after an input prefix was processed, callback side
    /// effects are not rolled back and the transaction is doomed. A callback
    /// unwind likewise leaves the transaction doomed.
    #[inline]
    pub fn modify_fixed_visit<const KEY_LENGTH: usize>(
        &mut self,
        keys: &[[u8; KEY_LENGTH]],
        batch: &mut PointReadBatch,
        modify: impl for<'value> FnMut(usize, Option<&'value Value>) -> PointMutation,
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
                modify,
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
                Value::from(value),
                || table.lookup_in_read_scope(worker, read_scope, key),
            )
        }
        #[cfg(test)]
        {
            self.table.put_inner(
                &mut *self.transaction,
                Some(self.worker),
                key,
                Value::from(value),
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
                Value::from(value),
                || table.lookup_in_read_scope(worker, read_scope, key),
            )
        }
        #[cfg(test)]
        {
            self.table.insert_inner(
                &mut *self.transaction,
                Some(self.worker),
                key,
                Value::from(value),
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
            membership_resource: self.membership_resource.clone(),
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

struct RecordEffect<R> {
    result: R,
    membership: Option<MembershipUpdate>,
}

impl<R> RecordEffect<R> {
    fn read(result: R) -> Self {
        Self {
            result,
            membership: None,
        }
    }

    fn write(result: R, membership: Option<MembershipUpdate>) -> Self {
        Self { result, membership }
    }
}

#[derive(Clone, Copy, Debug)]
struct MembershipUpdate {
    record_id: RecordId,
    changed: bool,
}

struct ScannedRecord {
    state: RecordState,
}

#[derive(Clone, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
struct RecordKey(Arc<[u8]>);

#[derive(Clone, Copy)]
struct DirectoryScanRequest<'key> {
    direction: ScanDirection,
    lower: ScanBound<'key>,
    upper: ScanBound<'key>,
    entry_capacity: usize,
    key_arena_capacity: usize,
}

struct DirectoryScanEntry {
    key: Box<[u8]>,
    record_id: RecordId,
}

enum DirectoryScanResume {
    None,
    UnchangedInput,
    Exclusive(Box<[u8]>),
}

struct DirectoryScanChunk {
    entries: Vec<DirectoryScanEntry>,
    stop_reason: ScanStopReason,
    resume: DirectoryScanResume,
    next_key_bytes_required: usize,
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

fn validate_directory_chunk(
    chunk: &DirectoryScanChunk,
    direction: ScanDirection,
    lower: ScanBound<'_>,
    upper: ScanBound<'_>,
    previous: Option<&[u8]>,
) -> Result<(), AccessError> {
    let mut prior = previous;
    for entry in &chunk.entries {
        if !key_in_bounds(&entry.key, lower, upper) {
            return Err(table_fault("directory scan returned an out-of-bounds key"));
        }
        if prior.is_some_and(|prior| match direction {
            ScanDirection::Forward => prior >= entry.key.as_ref(),
            ScanDirection::Reverse => prior <= entry.key.as_ref(),
        }) {
            return Err(table_fault(
                "directory scan did not make strict key progress",
            ));
        }
        prior = Some(&entry.key);
    }

    match (&chunk.stop_reason, &chunk.resume) {
        (ScanStopReason::End, DirectoryScanResume::None) => Ok(()),
        (
            ScanStopReason::EntryCapacity | ScanStopReason::KeyArenaCapacity,
            DirectoryScanResume::UnchangedInput,
        ) if chunk.entries.is_empty() => Ok(()),
        (
            ScanStopReason::EntryCapacity | ScanStopReason::KeyArenaCapacity,
            DirectoryScanResume::Exclusive(resume),
        ) if chunk.entries.last().is_some_and(|last| last.key == *resume) => Ok(()),
        _ => Err(table_fault("directory scan stop metadata is inconsistent")),
    }
}

struct TableShared {
    directory: Directory,
    registry: Registry,
    structural: StructuralGate,
    health: AtomicU8,
    runtime_id: sto_core::RuntimeId,
    namespace: LockNamespaceId,
    record_lock_class: LockClass,
    membership_version: Arc<AtomicVersion>,
    membership_lock: Arc<VersionLock>,
    membership_identity: LockIdentity,
}

impl TableShared {
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
        let candidate = self
            .registry
            .reserve_candidate(key)
            .inspect_err(|error| self.note_access_error(error))?;
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

        let result = self.directory.get_or_insert(worker, key, candidate.id);
        let resolved = match result {
            Ok(DirectoryInsertOutcome::Inserted(winner)) => {
                if winner != candidate.id {
                    self.poison();
                    return Err(table_fault("inserted winner differs from candidate"));
                }
                self.registry
                    .mark_published(&candidate)
                    .inspect_err(|error| self.note_access_error(error))?;
                Ok(winner)
            }
            Ok(DirectoryInsertOutcome::Existing(winner)) => {
                self.registry
                    .prove_unpublished(&candidate)
                    .inspect_err(|error| self.note_access_error(error))?;
                Ok(winner)
            }
            Err(error) => self.handle_insert_error(&candidate, error),
        };
        drop(structural);
        resolved
    }

    #[cfg(not(test))]
    fn handle_insert_error(
        &self,
        candidate: &Candidate,
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
        _candidate: &Candidate,
        error: MemoryInsertError,
    ) -> Result<RecordId, AccessError> {
        match error {}
    }

    #[inline(always)]
    fn resolve_directory_record(&self, record_id: RecordId) -> Result<&Record, AccessError> {
        // The directory is private to this table and append-only: its only
        // insertion path binds the exact lookup key to a candidate allocated
        // by this registry. Consequently the returned RecordId is the binding
        // capability; retaining and comparing a second copy of every key in
        // the Rust record would not strengthen the safe contract.
        self.registry
            .resolve(record_id)
            .inspect_err(|error| self.note_access_error(error))
    }

    #[inline]
    fn resolve_for_phase(
        &self,
        record_id: RecordId,
        phase: AdapterPhase,
    ) -> Result<&Record, AdapterFault> {
        self.registry.resolve(record_id).map_err(|error| {
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
        self.registry
            .resolve_with_segment(record_id)
            .map_err(|error| {
                self.note_access_error(&error);
                AdapterFault::invariant(phase)
            })
    }

    fn try_scan_structure(&self) -> Result<RwLockReadGuard<'_, ()>, AccessError> {
        self.structural.try_read()
    }
}

#[derive(Default)]
struct StructuralGate {
    lock: RwLock<()>,
}

impl StructuralGate {
    fn try_write(&self) -> Result<RwLockWriteGuard<'_, ()>, AccessError> {
        match self.lock.try_write() {
            Ok(guard) => Ok(guard),
            Err(TryLockError::WouldBlock) => Err(Conflict::HiddenLockBusy.into()),
            Err(TryLockError::Poisoned(_)) => {
                Err(table_fault("Masstree structural gate is poisoned"))
            }
        }
    }

    fn try_read(&self) -> Result<RwLockReadGuard<'_, ()>, AccessError> {
        match self.lock.try_read() {
            Ok(guard) => Ok(guard),
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

    fn scan(
        &self,
        _worker: Option<&Worker>,
        request: DirectoryScanRequest<'_>,
    ) -> Result<DirectoryScanChunk, AccessError> {
        match self {
            #[cfg(not(test))]
            Self::Native(directory) => directory.scan(_worker, request),
            #[cfg(test)]
            Self::Memory(directory) => directory.scan(request),
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

    fn scan(
        &self,
        worker: Option<&Worker>,
        request: DirectoryScanRequest<'_>,
    ) -> Result<DirectoryScanChunk, AccessError> {
        let worker = worker.ok_or_else(|| table_fault("native worker capability is missing"))?;
        let native_request = NativeScanRequest::new(request.direction)
            .with_lower(request.lower)
            .with_upper(request.upper)
            .with_entry_capacity(request.entry_capacity)
            .with_key_arena_capacity(request.key_arena_capacity);
        let chunk = self
            .tree
            .scan_chunk(worker, native_request)
            .map_err(map_masstree_error)?;
        let stop_reason = chunk.stop_reason();
        let resume = match chunk.resume() {
            NativeScanResume::None => DirectoryScanResume::None,
            NativeScanResume::UnchangedInput => DirectoryScanResume::UnchangedInput,
            NativeScanResume::Exclusive(key) => DirectoryScanResume::Exclusive(key.clone()),
        };
        let next_key_bytes_required = chunk.next_key_bytes_required();
        let entries = chunk
            .into_entries()
            .into_iter()
            .map(|entry| DirectoryScanEntry {
                key: entry.key().into(),
                record_id: entry.record_id(),
            })
            .collect();
        Ok(DirectoryScanChunk {
            entries,
            stop_reason,
            resume,
            next_key_bytes_required,
        })
    }
}

#[cfg(test)]
#[derive(Default)]
struct MemoryDirectory {
    entries: RwLock<std::collections::BTreeMap<Vec<u8>, RecordId>>,
    first_miss_barrier: Option<Arc<std::sync::Barrier>>,
    coordinated_misses: AtomicU64,
}

#[cfg(test)]
impl MemoryDirectory {
    fn with_first_miss_barrier(barrier: Arc<std::sync::Barrier>) -> Self {
        Self {
            entries: RwLock::new(std::collections::BTreeMap::new()),
            first_miss_barrier: Some(barrier),
            coordinated_misses: AtomicU64::new(0),
        }
    }

    fn get(&self, key: &[u8]) -> Result<Option<RecordId>, AccessError> {
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

    fn scan(&self, request: DirectoryScanRequest<'_>) -> Result<DirectoryScanChunk, AccessError> {
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
) -> Result<DirectoryScanChunk, AccessError> {
    let mut copied = Vec::new();
    let mut arena_used = 0_usize;
    for (key, record_id) in entries {
        if copied.len() == request.entry_capacity {
            let resume = copied.last().map_or(
                DirectoryScanResume::UnchangedInput,
                |last: &DirectoryScanEntry| DirectoryScanResume::Exclusive(last.key.clone()),
            );
            return Ok(DirectoryScanChunk {
                entries: copied,
                stop_reason: ScanStopReason::EntryCapacity,
                resume,
                next_key_bytes_required: key.len(),
            });
        }
        let remaining = request.key_arena_capacity.saturating_sub(arena_used);
        if key.len() > remaining {
            let resume = copied.last().map_or(
                DirectoryScanResume::UnchangedInput,
                |last: &DirectoryScanEntry| DirectoryScanResume::Exclusive(last.key.clone()),
            );
            return Ok(DirectoryScanChunk {
                entries: copied,
                stop_reason: ScanStopReason::KeyArenaCapacity,
                resume,
                next_key_bytes_required: key.len(),
            });
        }
        arena_used = arena_used
            .checked_add(key.len())
            .ok_or(CapacityError::BufferLimit)?;
        copied.push(DirectoryScanEntry {
            key: key.clone().into_boxed_slice(),
            record_id: *record_id,
        });
    }
    Ok(DirectoryScanChunk {
        entries: copied,
        stop_reason: ScanStopReason::End,
        resume: DirectoryScanResume::None,
        next_key_bytes_required: 0,
    })
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
        | MasstreeError::InvalidPublication => table_fault("Masstree boundary contract failed"),
    }
}

fn table_fault(reason: &'static str) -> AccessError {
    AdapterFault::new(AdapterPhase::Execute, AdapterFaultKind::Other(reason)).into()
}

#[derive(Clone, Copy)]
struct RecordLockDomain {
    runtime_id: sto_core::RuntimeId,
    namespace: LockNamespaceId,
    lock_class: LockClass,
}

enum RegistryStorage {
    LazySegmented(SegmentedRegistry),
    EagerContiguous(ContiguousRegistry),
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
            RegistryLayout::LazySegmented => {
                RegistryStorage::LazySegmented(SegmentedRegistry::new(effective_id_limit)?)
            }
            RegistryLayout::EagerContiguous { max_bytes } => RegistryStorage::EagerContiguous(
                ContiguousRegistry::new(effective_id_limit, max_bytes, lock_domain)?,
            ),
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

    fn reserve_candidate(&self, key: &[u8]) -> Result<Candidate, AccessError> {
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
        let entry = self.claim_slot(record_id).inspect_err(|_| {
            // The entry was never published into the registry, so no other
            // thread can release its retained-resource reservation.
            self.release_retained(key_bytes);
        })?;
        transition_slot(entry, SLOT_RESERVED, SLOT_READY).inspect_err(|_| {
            // No directory operation can observe this candidate before READY,
            // so this exact transition has sole ownership of the reservation.
            self.release_retained(key_bytes);
        })?;
        Ok(Candidate {
            id: record_id,
            key_bytes,
        })
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

    fn claim_slot(&self, record_id: RecordId) -> Result<&RegistryEntry, AccessError> {
        let index = record_index(record_id)?;
        let slot = match &self.storage {
            RegistryStorage::LazySegmented(storage) => {
                storage.claim_entry(index, self.lock_domain)?
            }
            RegistryStorage::EagerContiguous(storage) => storage.claim_entry(index)?,
        };
        slot.state
            .compare_exchange(
                SLOT_UNALLOCATED,
                SLOT_RESERVED,
                Ordering::AcqRel,
                Ordering::Acquire,
            )
            .map_err(|_| table_fault("record ID registry slot was reused"))?;
        Ok(slot)
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

    /// Issues a best-effort cache hint for a directory-proven stable slot
    /// before transaction-item lookup. Publication and slot initialization
    /// are still checked by `resolve`; the hint conveys no validity proof.
    #[inline(always)]
    fn prefetch(&self, record_id: RecordId) -> Result<(), AccessError> {
        let index = record_index(record_id)?;
        let entry = match &self.storage {
            RegistryStorage::LazySegmented(storage) => storage.entry(index)?,
            RegistryStorage::EagerContiguous(storage) => storage.entry(index)?,
        };
        record_prefetch::read(entry);
        Ok(())
    }

    #[inline(always)]
    fn resolve(&self, record_id: RecordId) -> Result<&Record, AccessError> {
        let entry = self.entry(record_id)?;
        match entry.state.load(Ordering::Acquire) {
            SLOT_READY | SLOT_PUBLISHED => Ok(&entry.record),
            _ => Err(table_fault("directory returned an unusable registry slot")),
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

    fn mark_published(&self, candidate: &Candidate) -> Result<(), AccessError> {
        let entry = self.entry(candidate.id)?;
        transition_slot(entry, SLOT_READY, SLOT_PUBLISHED)
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
}

fn record_index(record_id: RecordId) -> Result<usize, AccessError> {
    usize::try_from(record_id.get() - 1).map_err(|_| AccessError::from(CapacityError::KeyLimit))
}

struct SegmentedRegistry {
    segments: Box<[OnceLock<RegistrySegment>]>,
}

impl SegmentedRegistry {
    fn new(effective_id_limit: u64) -> Result<Self, CapacityError> {
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

        let candidate = RegistrySegment::new(segment_index, lock_domain)?;
        let _ = slot.set(candidate);
        slot.get()
            .ok_or_else(|| table_fault("record registry segment publication failed"))
    }

    fn claim_entry(
        &self,
        index: usize,
        lock_domain: RecordLockDomain,
    ) -> Result<&RegistryEntry, AccessError> {
        let segment = self.ensure_segment(index / REGISTRY_SEGMENT_SLOTS, lock_domain)?;
        Ok(&segment.slots[index % REGISTRY_SEGMENT_SLOTS])
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
        let (segment, slot_index) = self.segment_and_slot(index)?;
        Ok(&segment.slots[slot_index])
    }

    #[inline(always)]
    fn entry_with_lock(
        &self,
        index: usize,
    ) -> Result<(&RegistryEntry, &Arc<RecordLockSegment>), AccessError> {
        let (segment, slot_index) = self.segment_and_slot(index)?;
        Ok((&segment.slots[slot_index], segment.lock_segment(slot_index)))
    }
}

struct ContiguousRegistry {
    slots: Arc<[RegistryEntry]>,
    lock_segments: Box<[Arc<RecordLockSegment>]>,
}

impl ContiguousRegistry {
    fn new(
        effective_id_limit: u64,
        max_bytes: usize,
        lock_domain: RecordLockDomain,
    ) -> Result<Self, CapacityError> {
        let slot_count = checked_registry_slot_count(effective_id_limit)?;
        let accounted_bytes = eager_registry_accounted_bytes(slot_count)?;
        if accounted_bytes > max_bytes {
            return Err(CapacityError::BufferLimit);
        }

        // The budget and all size arithmetic are validated before either
        // proportional allocation begins. Failure never silently selects the
        // segmented backend because that would invalidate benchmark intent.
        let slots = allocate_registry_slots(slot_count)?;
        let lock_segments = build_record_lock_segments(&slots, 0, lock_domain)?;
        Ok(Self {
            slots,
            lock_segments,
        })
    }

    fn claim_entry(&self, index: usize) -> Result<&RegistryEntry, AccessError> {
        self.slots
            .get(index)
            .ok_or_else(|| CapacityError::BufferLimit.into())
    }

    #[inline(always)]
    fn entry(&self, index: usize) -> Result<&RegistryEntry, AccessError> {
        self.slots
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

fn checked_registry_slot_count(effective_id_limit: u64) -> Result<usize, CapacityError> {
    let slot_count = usize::try_from(effective_id_limit).map_err(|_| CapacityError::BufferLimit)?;
    let allocation_bytes = slot_count
        .checked_mul(std::mem::size_of::<RegistryEntry>())
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

fn eager_registry_accounted_bytes(slot_count: usize) -> Result<usize, CapacityError> {
    if slot_count == 0 {
        return Ok(0);
    }
    let lock_count = record_lock_segment_count(slot_count)?;
    let arc_header_bytes = 2_usize
        .checked_mul(std::mem::size_of::<usize>())
        .ok_or(CapacityError::BufferLimit)?;
    let slot_bytes = slot_count
        .checked_mul(std::mem::size_of::<RegistryEntry>())
        .and_then(|bytes| bytes.checked_add(arc_header_bytes))
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

fn allocate_registry_slots(slot_count: usize) -> Result<Arc<[RegistryEntry]>, CapacityError> {
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
    Ok(Arc::from(slots.into_boxed_slice()))
}

fn build_record_lock_segments(
    slots: &Arc<[RegistryEntry]>,
    logical_base: usize,
    lock_domain: RecordLockDomain,
) -> Result<Box<[Arc<RecordLockSegment>]>, CapacityError> {
    let lock_count = record_lock_segment_count(slots.len())?;
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
            slots: Arc::clone(slots),
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
    slots: Arc<[RegistryEntry]>,
    // Targets never point back to this owner: each owns only `slots`, so table
    // teardown cannot dangle a guard and the ownership graph has no cycle.
    lock_segments: Box<[Arc<RecordLockSegment>]>,
}

impl RegistrySegment {
    fn new(segment_index: usize, lock_domain: RecordLockDomain) -> Result<Self, AccessError> {
        debug_assert_eq!(REGISTRY_SEGMENT_SLOTS % RECORD_LOCK_SEGMENT_SLOTS, 0);
        let logical_base = segment_index
            .checked_mul(REGISTRY_SEGMENT_SLOTS)
            .ok_or(CapacityError::BufferLimit)?;
        let slots = allocate_registry_slots(REGISTRY_SEGMENT_SLOTS)?;
        let lock_segments = build_record_lock_segments(&slots, logical_base, lock_domain)?;
        debug_assert_eq!(
            lock_segments.len(),
            RECORD_LOCK_SEGMENTS_PER_REGISTRY_SEGMENT
        );
        Ok(Self {
            slots,
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
    slots: Arc<[RegistryEntry]>,
    // The same target shape serves both backends. `logical_base` identifies
    // the first global RecordId index accepted by this target, while
    // `physical_base` identifies that record inside its owning Arc arena.
    logical_base: usize,
    physical_base: usize,
    lock_domain: RecordLockDomain,
}

impl RecordLockSegment {
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
        let index = usize::try_from(record_id.get() - 1)
            .map_err(|_| AdapterFault::new(phase, AdapterFaultKind::LockIdentityMismatch))?;
        let offset = index
            .checked_sub(self.logical_base)
            .filter(|offset| *offset < RECORD_LOCK_SEGMENT_SLOTS)
            .ok_or_else(|| AdapterFault::new(phase, AdapterFaultKind::LockIdentityMismatch))?;
        let slot = self
            .physical_base
            .checked_add(offset)
            .filter(|slot| *slot < self.slots.len())
            .ok_or_else(|| AdapterFault::new(phase, AdapterFaultKind::LockIdentityMismatch))?;
        Ok((record_id, slot))
    }

    fn record_at(&self, slot: usize, phase: AdapterPhase) -> Result<&Record, AdapterFault> {
        let entry = self
            .slots
            .get(slot)
            .ok_or_else(|| AdapterFault::invariant(phase))?;
        match entry.state.load(Ordering::Acquire) {
            SLOT_READY | SLOT_PUBLISHED => Ok(&entry.record),
            _ => Err(AdapterFault::invariant(phase)),
        }
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

        let result = match disposition {
            LockDisposition::Aborted => {
                guard.detached.release_abort(&record.version).map(|()| None)
            }
            LockDisposition::Committed {
                occ_commit_id: Some(commit_id),
            } => guard
                .detached
                .release_commit(&record.version, commit_id)
                .map(Some),
            LockDisposition::Committed {
                occ_commit_id: None,
            } => panic!("sto-masstree committed record write has no OCC commit ID"),
            LockDisposition::Indeterminate { occ_commit_id } => guard
                .detached
                .release_indeterminate(&record.version, occ_commit_id)
                .map(Some),
        };
        if let Err(error) = result {
            panic!("sto-masstree record version release failed: {error}");
        }
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

struct Candidate {
    id: RecordId,
    key_bytes: u64,
}

const REGISTRY_ENTRY_PADDING_BYTES: usize =
    REGISTRY_ENTRY_SLOT_BYTES - std::mem::size_of::<Record>() - std::mem::size_of::<AtomicU8>();

#[repr(C)]
struct RegistryEntry {
    // Put the read-hot record first. Publication state is the only per-slot
    // metadata needed after candidate resolution; retained quota belongs to
    // the short-lived Candidate until its publication disposition is known.
    record: Record,
    state: AtomicU8,
    // Preserve the measured 48-byte arena stride after removing each slot's
    // OnceLock. Compacting these entries to 40 bytes regressed contended writes.
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

    #[inline(always)]
    fn uses_shared_storage(&self) -> bool {
        matches!(
            self,
            Self::Live(Value {
                repr: ValueRepr::Shared(_),
            })
        )
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
/// Large immutable values use ArcSwap's protected strong-reference load, so a
/// concurrent replacement cannot reclaim their bytes. The read-hot inline
/// path never enters ArcSwap and never performs a read-side RMW.
#[repr(C)]
struct CommittedRecordState {
    inline: AtomicU64,
    shared: ArcSwapOption<Vec<u8>>,
    tag: AtomicU8,
}

enum CommittedStateLoad {
    Complete(RecordState),
    Incomplete(&'static str),
}

impl CommittedRecordState {
    fn tombstone() -> Self {
        Self {
            inline: AtomicU64::new(0),
            shared: ArcSwapOption::from(None),
            tag: AtomicU8::new(RECORD_STATE_TOMBSTONE),
        }
    }

    #[inline(always)]
    fn load(&self) -> CommittedStateLoad {
        let tag = self.tag.load(Ordering::Acquire);
        let inline_length = tag.wrapping_sub(RECORD_STATE_INLINE_BASE);
        if usize::from(inline_length) <= INLINE_VALUE_CAPACITY {
            let bytes = self.inline.load(Ordering::Acquire).to_ne_bytes();
            return CommittedStateLoad::Complete(RecordState::Live(Value {
                repr: ValueRepr::Inline {
                    len: inline_length,
                    bytes,
                },
            }));
        }

        match tag {
            RECORD_STATE_TOMBSTONE => CommittedStateLoad::Complete(RecordState::Tombstone),
            RECORD_STATE_SHARED => match self.shared.load_full() {
                Some(bytes) => CommittedStateLoad::Complete(RecordState::Live(Value {
                    repr: ValueRepr::Shared(bytes),
                })),
                // A delayed reader can sample the old Shared tag before a
                // shared-to-inline publication clears the ArcSwap slot. Its
                // following version check must fail, so do not misclassify
                // this safe transient as corruption until after validation.
                None => CommittedStateLoad::Incomplete(
                    "committed shared record publication is incomplete",
                ),
            },
            RECORD_STATE_UPDATING => {
                CommittedStateLoad::Incomplete("committed record publication remained in progress")
            }
            RECORD_STATE_POISONED => {
                CommittedStateLoad::Incomplete("committed record state is poisoned")
            }
            _ => CommittedStateLoad::Incomplete("committed record state tag is invalid"),
        }
    }

    fn begin_publication<'state>(
        &'state self,
        table: &'state TableShared,
    ) -> CommittedStatePublication<'state> {
        // Once this marker is visible, all payload fields may be changed in
        // any order. A normal reader either observes the held OCC version or
        // rejects its final version check. The publication guard converts an
        // unwind into a permanent fail-closed marker.
        self.tag.store(RECORD_STATE_UPDATING, Ordering::Release);
        CommittedStatePublication {
            state: self,
            table,
            completed: false,
        }
    }
}

struct CommittedStatePublication<'state> {
    state: &'state CommittedRecordState,
    table: &'state TableShared,
    completed: bool,
}

impl CommittedStatePublication<'_> {
    fn publish(mut self, replacement: RecordState, old_was_shared: bool) {
        let final_tag = match replacement {
            RecordState::Tombstone => {
                if old_was_shared {
                    self.state.shared.store(None);
                }
                debug_assert!(self.state.shared.load().is_none());
                RECORD_STATE_TOMBSTONE
            }
            RecordState::Live(Value {
                repr: ValueRepr::Inline { len, bytes },
            }) => {
                debug_assert!(usize::from(len) <= INLINE_VALUE_CAPACITY);
                self.state
                    .inline
                    .store(u64::from_ne_bytes(bytes), Ordering::Release);
                if old_was_shared {
                    self.state.shared.store(None);
                }
                debug_assert!(self.state.shared.load().is_none());
                RECORD_STATE_INLINE_BASE
                    .checked_add(len)
                    .expect("inline record-state tag cannot overflow")
            }
            RecordState::Live(Value {
                repr: ValueRepr::Shared(bytes),
            }) => {
                self.state.shared.store(Some(bytes));
                RECORD_STATE_SHARED
            }
        };

        // This is the linear physical-state publication. The transaction's
        // logical publication follows when the core Release-unlocks version.
        self.state.tag.store(final_tag, Ordering::Release);
        self.completed = true;
    }
}

impl Drop for CommittedStatePublication<'_> {
    fn drop(&mut self) {
        if !self.completed {
            self.state
                .tag
                .store(RECORD_STATE_POISONED, Ordering::Release);
            self.table.poison();
        }
    }
}

struct RecordAdapter {
    table: Arc<TableShared>,
}

static RECORD_PREFLIGHT_FREE_READ: PreflightFreeReadCapability<RecordAdapter> =
    // A committed prepared-free record read has no intent or Prepared value.
    // Its only remaining work is dropping RecordLocal::first_snapshot after
    // certification; core-owned teardown does that while retaining the table
    // binding. Aborted reads still run RecordAdapter::finish.
    PreflightFreeReadCapability::new_drop_only(validate_record_preflight_free_read);

#[derive(Default)]
struct RecordLocal {
    first_snapshot: Option<RecordState>,
}

#[derive(Clone, Copy)]
struct RecordObservation {
    version: OccVersion,
}

impl OpacityToken for RecordObservation {
    fn observation_order(&self) -> ObservationOrder {
        ObservationOrder::Ordered(self.version)
    }
}

fn validate_record_preflight_free_read(
    adapter: &RecordAdapter,
    key: &RecordId,
    observation: &RecordObservation,
    _cx: &PreflightFreeValidationContext<'_>,
) -> Result<(), CheckError> {
    adapter.validate_read_only_observation(*key, observation)
}

enum RecordPrepared {
    ReadOnly,
    Write {
        lock_use: LockUse<RecordLockSegment>,
    },
}

impl RecordAdapter {
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
    fn prepare_access(
        &self,
        record_id: RecordId,
        entry: &mut Entry<'_, Self>,
    ) -> Result<(), AccessError> {
        self.table.ensure_healthy()?;
        match (entry.local().first_snapshot.is_some(), entry.observation()) {
            (true, ObservationRef::Read(_) | ObservationRef::UpgradedPredicate(_)) => return Ok(()),
            (false, ObservationRef::Unobserved) => {}
            _ => {
                self.table.poison();
                return Err(table_fault("record local snapshot state is inconsistent"));
            }
        }

        let record = self.table.resolve_directory_record(record_id)?;

        let observed = record
            .version
            .observe()
            .map_err(|_| AccessError::from(Conflict::LockBusy))?;
        let snapshot = self.snapshot_state(record, observed)?;
        entry.record_read(RecordObservation { version: observed })?;
        entry.local_mut().first_snapshot = Some(snapshot);
        Ok(())
    }

    #[inline(always)]
    fn snapshot_state(
        &self,
        record: &Record,
        observed: OccVersion,
    ) -> Result<RecordState, AccessError> {
        let candidate = record.state.load();
        // Classify intermediate field combinations only after closing the OCC
        // sandwich. They are expected when a writer acquired the version
        // after our first observation; they are corruption only if the exact
        // unlocked generation remained stable.
        if !record.version.validate(observed) {
            return Err(Conflict::ReadValidation.into());
        }
        match candidate {
            CommittedStateLoad::Complete(state) => Ok(state),
            CommittedStateLoad::Incomplete(reason) => {
                self.table.poison();
                Err(table_fault(reason))
            }
        }
    }

    fn validate_observation(
        &self,
        record_id: RecordId,
        observation: &RecordObservation,
        prepared: &RecordPrepared,
        cx: &ValidationContext<'_>,
    ) -> Result<(), CheckError> {
        if matches!(prepared, RecordPrepared::ReadOnly) {
            return self.validate_read_only_observation(record_id, observation);
        }
        let RecordPrepared::Write { lock_use } = prepared else {
            unreachable!("read-only records returned before lock resolution")
        };
        let guard = cx.guard(lock_use)?;
        if !guard.is_held() || guard.record_id != record_id || guard.owner() != cx.owner() {
            return Err(AdapterFault::new(
                AdapterPhase::Validation,
                AdapterFaultKind::LockIdentityMismatch,
            )
            .into());
        }

        // Successful acquisition recorded the exact unlocked version that
        // its compare-exchange replaced with this owner's lock word. The
        // private guard can only be constructed by RecordLockSegment for the
        // identity above, and the held lock excludes subsequent writers.
        // Equality with that acquisition snapshot is therefore the final
        // write-read certification; resolving the registry and loading the
        // same AtomicVersion again would add no information.
        if guard.before() == observation.version {
            Ok(())
        } else {
            Err(Conflict::ReadValidation.into())
        }
    }
}

impl TransactionalResource for RecordAdapter {
    type Key = RecordId;
    type Local = RecordLocal;
    type Observation = RecordObservation;
    type Predicate = NoPredicate;
    type Intent = RecordState;
    type Prepared = RecordPrepared;

    fn new_local(&self, _key: &Self::Key) -> Result<Self::Local, sto_core::ItemInitError> {
        Ok(RecordLocal::default())
    }

    fn preflight_free_read_capability(&self) -> Option<&'static PreflightFreeReadCapability<Self>> {
        Some(&RECORD_PREFLIGHT_FREE_READ)
    }

    fn preflight(
        &self,
        key: &Self::Key,
        item: PreflightItem<'_, Self>,
        cx: &mut PreflightContext<'_>,
    ) -> Result<Self::Prepared, PrepareError> {
        match item.observation() {
            ObservationRef::Read(_) | ObservationRef::UpgradedPredicate(_) => {}
            ObservationRef::Unobserved | ObservationRef::Predicate(_) => {
                return Err(AdapterFault::new(
                    AdapterPhase::Preflight,
                    AdapterFaultKind::LockIdentityMismatch,
                )
                .into());
            }
        };
        let record_id = *key;
        let changed = match (item.local().first_snapshot.as_ref(), item.intent()) {
            (Some(original), Some(replacement)) => original != replacement,
            (_, None) => false,
            (None, Some(_)) => {
                return Err(AdapterFault::invariant(AdapterPhase::Preflight).into());
            }
        };
        if changed {
            let (_record, lock_segment) = self
                .table
                .resolve_with_lock_segment_for_phase(record_id, AdapterPhase::Preflight)?;
            let lock_identity = LockIdentity::new(
                self.table.runtime_id,
                self.table.namespace,
                self.table.record_lock_class,
                record_id.get(),
            );
            let lock_use =
                cx.require_lock(LockRequest::new(lock_identity, Arc::clone(lock_segment)))?;
            Ok(RecordPrepared::Write { lock_use })
        } else {
            Ok(RecordPrepared::ReadOnly)
        }
    }

    fn revalidate_read(
        &self,
        key: &Self::Key,
        observation: &Self::Observation,
        _cx: &ExecutionCheckContext<'_>,
    ) -> Result<(), CheckError> {
        let record = self
            .table
            .resolve_for_phase(*key, AdapterPhase::ExecutionCheck)?;
        if record.version.validate(observation.version) {
            Ok(())
        } else {
            Err(Conflict::ReadValidation.into())
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
        self.validate_observation(*key, observation, prepared, cx)
    }

    fn install(
        &self,
        key: &Self::Key,
        mut item: InstallItem<'_, Self>,
        prepared: &mut Self::Prepared,
        cx: &mut InstallContext<'_>,
    ) {
        let RecordPrepared::Write { lock_use } = prepared else {
            return;
        };
        let commit_id = cx
            .occ_commit_id()
            .expect("sto-masstree record write has no OCC commit ID");
        let (lock_segment, guard) = cx
            .target_and_guard_mut(lock_use)
            .unwrap_or_else(|error| panic!("sto-masstree record lock invariant: {error}"));
        let record = lock_segment
            .record_at(guard.slot, AdapterPhase::Install)
            .unwrap_or_else(|error| panic!("sto-masstree record install invariant: {error}"));
        if !guard.is_held()
            || !guard.is_for(*key, &record.version)
            || commit_id.to_version() <= guard.before()
        {
            panic!("sto-masstree record install received the wrong version guard");
        }
        if item.local_mut().first_snapshot.is_none() {
            panic!("sto-masstree record install has no committed snapshot");
        }
        let old_was_shared = item
            .local_mut()
            .first_snapshot
            .as_ref()
            .is_some_and(RecordState::uses_shared_storage);

        // Clone before beginning publication. Inline values copy one word;
        // large immutable values increment one Arc that is moved into ArcSwap.
        // The first snapshot pins old shared bytes through the subsequent
        // version unlock and post-unlock `finish`.
        let replacement = item.intent().clone();
        record
            .state
            .begin_publication(self.table.as_ref())
            .publish(replacement, old_was_shared);
    }

    fn finish(
        &self,
        _key: &Self::Key,
        mut item: FinishItem<'_, Self>,
        _prepared: Option<&mut Self::Prepared>,
        _disposition: FinishDisposition,
        _cx: &mut FinishContext<'_>,
    ) {
        item.local_mut().first_snapshot = None;
        let _ = item.take_remaining_intent();
    }
}

#[inline(always)]
fn visit_fixed_value<const CAPTURE_VALUE: bool>(
    adapter: &RecordAdapter,
    entry: &mut Entry<'_, RecordAdapter>,
    record_id: RecordId,
    index: usize,
    visit: &mut impl for<'value> FnMut(usize, Option<&'value Value>),
    values: &mut Vec<Option<Value>>,
) -> Result<(), AccessError> {
    adapter.prepare_access(record_id, entry)?;
    let current = current_state(entry).and_then(RecordState::value);
    visit(index, current);
    if CAPTURE_VALUE {
        values.push(current.cloned());
    }
    Ok(())
}

#[inline(always)]
fn apply_fixed_mutation<const CAPTURE_VALUE: bool>(
    adapter: &RecordAdapter,
    entry: &mut Entry<'_, RecordAdapter>,
    record_id: RecordId,
    index: usize,
    modify: &mut impl for<'value> FnMut(usize, Option<&'value Value>) -> PointMutation,
    values: &mut Vec<Option<Value>>,
    membership_updates: &mut Vec<MembershipUpdate>,
) -> Result<(), AccessError> {
    adapter.prepare_access(record_id, entry)?;
    let previous = current_state(entry).and_then(RecordState::value);
    let mutation = modify(index, previous);
    if CAPTURE_VALUE {
        values.push(previous.cloned());
    }
    match mutation {
        PointMutation::Keep => {}
        PointMutation::Put(value) => {
            if let Some(update) = membership_transition(entry, record_id, true)? {
                membership_updates.push(update);
            }
            stage_record_state(entry, RecordState::Live(value))?;
        }
        PointMutation::Remove => {
            if previous.is_some() {
                if let Some(update) = membership_transition(entry, record_id, false)? {
                    membership_updates.push(update);
                }
                stage_record_state(entry, RecordState::tombstone())?;
            }
        }
    }
    Ok(())
}

#[inline(always)]
fn current_state<'entry>(entry: &'entry Entry<'_, RecordAdapter>) -> Option<&'entry RecordState> {
    entry
        .intent()
        .or_else(|| entry.local().first_snapshot.as_ref())
}

#[inline(always)]
fn current_state_snapshot<'entry>(
    entry: &'entry Entry<'_, RecordAdapter>,
) -> Result<&'entry RecordState, AccessError> {
    entry
        .intent()
        .or_else(|| entry.local().first_snapshot.as_ref())
        .ok_or_else(|| table_fault("record operation has no current snapshot"))
}

#[inline(always)]
fn stage_record_state(
    entry: &mut Entry<'_, RecordAdapter>,
    replacement: RecordState,
) -> Result<(), AccessError> {
    entry.stage(replacement)
}

#[inline(always)]
fn committed_state<'entry>(
    entry: &'entry Entry<'_, RecordAdapter>,
) -> Result<&'entry RecordState, AccessError> {
    entry
        .local()
        .first_snapshot
        .as_ref()
        .ok_or_else(|| table_fault("record operation has no committed snapshot"))
}

#[inline(always)]
fn membership_transition(
    entry: &Entry<'_, RecordAdapter>,
    record_id: RecordId,
    replacement_live: bool,
) -> Result<Option<MembershipUpdate>, AccessError> {
    let committed_live = committed_state(entry)?.is_live();
    let current_live = current_state(entry)
        .ok_or_else(|| table_fault("record operation has no current snapshot"))?
        .is_live();
    let changed_before = current_live != committed_live;
    let changed_after = replacement_live != committed_live;
    Ok(
        (changed_before != changed_after).then_some(MembershipUpdate {
            record_id,
            changed: changed_after,
        }),
    )
}

struct MembershipAdapter {
    table: Arc<TableShared>,
}

#[derive(Default)]
struct MembershipLocal {
    first_version: Option<OccVersion>,
}

#[derive(Clone, Copy)]
struct MembershipObservation {
    version: OccVersion,
}

impl OpacityToken for MembershipObservation {
    fn observation_order(&self) -> ObservationOrder {
        ObservationOrder::Ordered(self.version)
    }
}

#[derive(Default)]
struct MembershipIntent {
    changed_records: BTreeSet<RecordId>,
}

struct MembershipPrepared {
    lock_use: Option<LockUse<VersionLock>>,
}

impl MembershipAdapter {
    fn update(
        &self,
        update: MembershipUpdate,
        entry: &mut Entry<'_, Self>,
    ) -> Result<(), AccessError> {
        self.table.ensure_healthy()?;
        if entry.intent().is_none() {
            entry.stage(MembershipIntent::default())?;
        }
        let intent = entry
            .intent_mut()
            .ok_or_else(|| table_fault("membership intent disappeared"))?;
        if update.changed {
            intent.changed_records.insert(update.record_id);
        } else {
            intent.changed_records.remove(&update.record_id);
        }
        Ok(())
    }

    fn observe(&self, entry: &mut Entry<'_, Self>) -> Result<OccVersion, AccessError> {
        if let Some(version) = entry.local().first_version {
            return Ok(version);
        }
        let observed = self
            .table
            .membership_version
            .observe()
            .map_err(|_| AccessError::from(Conflict::LockBusy))?;
        if !self.table.membership_version.validate(observed) {
            return Err(Conflict::ReadValidation.into());
        }
        entry.record_read(MembershipObservation { version: observed })?;
        entry.local_mut().first_version = Some(observed);
        Ok(observed)
    }
}

impl TransactionalResource for MembershipAdapter {
    type Key = ();
    type Local = MembershipLocal;
    type Observation = MembershipObservation;
    type Predicate = NoPredicate;
    type Intent = MembershipIntent;
    type Prepared = MembershipPrepared;

    fn new_local(&self, _key: &Self::Key) -> Result<Self::Local, sto_core::ItemInitError> {
        Ok(MembershipLocal::default())
    }

    fn preflight(
        &self,
        _key: &Self::Key,
        item: PreflightItem<'_, Self>,
        cx: &mut PreflightContext<'_>,
    ) -> Result<Self::Prepared, PrepareError> {
        let changes_membership = item
            .intent()
            .is_some_and(|intent| !intent.changed_records.is_empty());
        let lock_use = if changes_membership {
            Some(cx.require_lock(LockRequest::new(
                self.table.membership_identity.clone(),
                Arc::clone(&self.table.membership_lock),
            ))?)
        } else {
            None
        };
        Ok(MembershipPrepared { lock_use })
    }

    fn revalidate_read(
        &self,
        _key: &Self::Key,
        observation: &Self::Observation,
        _cx: &ExecutionCheckContext<'_>,
    ) -> Result<(), CheckError> {
        if self.table.membership_version.validate(observation.version) {
            Ok(())
        } else {
            Err(Conflict::ReadValidation.into())
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
        _key: &Self::Key,
        observation: &Self::Observation,
        prepared: &Self::Prepared,
        cx: &ValidationContext<'_>,
    ) -> Result<(), CheckError> {
        let valid = if let Some(lock_use) = prepared.lock_use.as_ref() {
            let guard = cx.guard(lock_use)?;
            if !guard.is_for(&self.table.membership_version) || guard.owner() != cx.owner() {
                return Err(AdapterFault::new(
                    AdapterPhase::Validation,
                    AdapterFaultKind::LockIdentityMismatch,
                )
                .into());
            }
            self.table
                .membership_version
                .validate_own(observation.version, cx.owner())
        } else {
            self.table.membership_version.validate(observation.version)
        };
        if valid {
            Ok(())
        } else {
            Err(Conflict::ReadValidation.into())
        }
    }

    fn install(
        &self,
        _key: &Self::Key,
        item: InstallItem<'_, Self>,
        prepared: &mut Self::Prepared,
        cx: &mut InstallContext<'_>,
    ) {
        if item.intent().changed_records.is_empty() {
            return;
        }
        let lock_use = prepared
            .lock_use
            .as_ref()
            .expect("sto-masstree membership change has no planned lock");
        let commit_id = cx
            .occ_commit_id()
            .expect("sto-masstree membership change has no OCC commit ID");
        let guard = cx
            .guard_mut(lock_use)
            .unwrap_or_else(|error| panic!("sto-masstree membership lock invariant: {error}"));
        if !guard.is_held()
            || !guard.is_for(&self.table.membership_version)
            || commit_id.to_version() <= guard.before()
        {
            panic!("sto-masstree membership install received the wrong version guard");
        }
    }

    fn finish(
        &self,
        _key: &Self::Key,
        mut item: FinishItem<'_, Self>,
        _prepared: Option<&mut Self::Prepared>,
        _disposition: FinishDisposition,
        _cx: &mut FinishContext<'_>,
    ) {
        item.local_mut().first_version = None;
        let _ = item.take_remaining_intent();
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
        let max_bytes = eager_registry_accounted_bytes(maximum).unwrap();
        TableConfig::new()
            .with_max_retained_records(maximum as u64)
            .with_max_retained_key_bytes((maximum as u64).saturating_mul(16))
            .with_max_consumed_record_ids(maximum as u64)
            .with_registry_layout(RegistryLayout::EagerContiguous { max_bytes })
    }

    fn committed(outcome: Result<CommitOutcome, sto_core::CommitFailure>) {
        assert!(matches!(outcome, Ok(CommitOutcome::Committed(_))));
    }

    fn committed_record_snapshot(record: &Record) -> RecordState {
        let observed = record.version.observe().unwrap();
        let candidate = record.state.load();
        assert!(record.version.validate(observed));
        match candidate {
            CommittedStateLoad::Complete(state) => state,
            CommittedStateLoad::Incomplete(reason) => {
                panic!("stable committed record state is incomplete: {reason}")
            }
        }
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

    #[test]
    fn table_handles_are_send_sync_and_clones_preserve_identity() {
        fn assert_send_sync<T: Send + Sync>() {}
        assert_send_sync::<Table>();
        assert_send_sync::<Value>();
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let clone = table.clone();
        assert_eq!(table.object_id(), clone.object_id());
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Healthy);
    }

    #[test]
    fn registry_layout_configuration_is_explicit_and_lazy_by_default() {
        let default = TableConfig::new();
        assert_eq!(default.registry_layout(), RegistryLayout::LazySegmented);
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
    }

    #[test]
    #[cfg(target_pointer_width = "64")]
    fn record_transaction_types_stay_cache_compact() {
        assert_eq!(INLINE_VALUE_CAPACITY, 8);
        assert_eq!(std::mem::size_of::<ValueRepr>(), 16);
        assert_eq!(std::mem::size_of::<Value>(), 16);
        assert_eq!(std::mem::size_of::<RecordState>(), 16);
        assert_eq!(std::mem::size_of::<Option<RecordState>>(), 16);
        assert_eq!(std::mem::size_of::<ArcSwapOption<Vec<u8>>>(), 8);
        assert_eq!(std::mem::size_of::<CommittedRecordState>(), 24);
        assert_eq!(std::mem::size_of::<Record>(), 32);
        assert_eq!(REGISTRY_ENTRY_PADDING_BYTES, 15);
        assert_eq!(std::mem::size_of::<RegistryEntry>(), 48);
        assert_eq!(std::mem::size_of::<RecordLockSegment>(), 56);
        assert_eq!(std::mem::size_of::<Candidate>(), 16);
        assert_eq!(std::mem::size_of::<RecordLocal>(), 16);
        assert_eq!(std::mem::size_of::<RecordObservation>(), 8);
        assert_eq!(std::mem::size_of::<RegisteredResource<RecordAdapter>>(), 8);
        // LockUse retains runtime and plan identities while its nonzero
        // slot-plus-one encoding supplies the enum discriminant niche.
        assert_eq!(std::mem::size_of::<RecordPrepared>(), 24);
    }

    #[test]
    fn drop_only_read_finish_releases_shared_snapshots_in_core_teardown() {
        const KEY: [u8; 8] = *b"snapshot";
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        seed(
            &table,
            &mut worker,
            &[(&KEY, b"shared-value-beyond-inline")],
        );

        let record_id = table.shared().lookup(None, &KEY).unwrap().unwrap();
        let record = table.shared().resolve_directory_record(record_id).unwrap();
        let pinned = match committed_record_snapshot(record) {
            RecordState::Live(Value {
                repr: ValueRepr::Shared(bytes),
            }) => bytes,
            _ => panic!("fixture value must use shared storage"),
        };
        let baseline = Arc::strong_count(&pinned);

        // The scalar lane owns one transaction-local snapshot and returns one
        // clone. Committed drop-only finish must release the former through
        // core teardown even though RecordAdapter::finish is not called.
        let mut scalar = worker.begin().unwrap();
        let returned = table.get_inner(&mut scalar, None, &KEY).unwrap().unwrap();
        assert!(matches!(&returned.repr, ValueRepr::Shared(_)));
        assert_eq!(Arc::strong_count(&pinned), baseline + 2);
        drop(returned);
        assert_eq!(Arc::strong_count(&pinned), baseline + 1);
        committed(scalar.commit());
        assert_eq!(Arc::strong_count(&pinned), baseline);

        // The fixed visitor is the typed unique-batch lane used by the point
        // benchmark. It captures no result, leaving exactly the local pin for
        // core teardown to release.
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
                        assert_eq!(
                            current.map(Value::as_ref),
                            Some(&b"shared-value-beyond-inline"[..])
                        );
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
        assert_eq!(Arc::strong_count(&pinned), baseline + 1);
        committed(typed.commit());
        assert_eq!(Arc::strong_count(&pinned), baseline);

        // Drop-only is a committed policy only. Explicit abort still routes
        // through the adapter finish callback before core teardown.
        let mut aborted = worker.begin().unwrap();
        let returned = table.get_inner(&mut aborted, None, &KEY).unwrap().unwrap();
        drop(returned);
        assert_eq!(Arc::strong_count(&pinned), baseline + 1);
        aborted.abort();
        assert_eq!(Arc::strong_count(&pinned), baseline);
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Healthy);
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
        assert_eq!(segment.slots.len(), REGISTRY_SEGMENT_SLOTS);
        assert_eq!(
            std::mem::size_of::<RegistryEntry>(),
            REGISTRY_ENTRY_SLOT_BYTES
        );
        assert_eq!(
            (&segment.slots[1] as *const RegistryEntry as usize)
                - (&segment.slots[0] as *const RegistryEntry as usize),
            REGISTRY_ENTRY_SLOT_BYTES
        );
        assert!(segment
            .slots
            .iter()
            .all(|entry| entry.state.load(Ordering::Acquire) == SLOT_UNALLOCATED));
    }

    #[test]
    #[cfg(target_pointer_width = "64")]
    fn contiguous_registry_allocates_one_exact_stable_arena_and_all_lock_targets() {
        const SLOTS: usize = 33;
        let required_bytes = eager_registry_accounted_bytes(SLOTS).unwrap();
        let registry = isolated_registry(eager_registry_config(SLOTS));
        let RegistryStorage::EagerContiguous(storage) = &registry.storage else {
            panic!("explicit eager configuration must not fall back to segmented storage");
        };

        assert_eq!(storage.slots.len(), SLOTS);
        assert_eq!(
            std::mem::size_of::<RegistryEntry>(),
            REGISTRY_ENTRY_SLOT_BYTES
        );
        assert_eq!(
            (&storage.slots[1] as *const RegistryEntry as usize)
                - (&storage.slots[0] as *const RegistryEntry as usize),
            REGISTRY_ENTRY_SLOT_BYTES
        );
        assert_eq!(
            storage.lock_segments.len(),
            record_lock_segment_count(SLOTS).unwrap()
        );
        assert_eq!(
            eager_registry_accounted_bytes(storage.slots.len()).unwrap(),
            required_bytes
        );
        assert_eq!(eager_registry_accounted_bytes(100_000).unwrap(), 5_300_016);
        assert!(eager_registry_accounted_bytes(100_000).unwrap() <= 8 * 1024 * 1024);
        assert!(storage
            .slots
            .iter()
            .all(|entry| entry.state.load(Ordering::Acquire) == SLOT_UNALLOCATED));
        for (index, target) in storage.lock_segments.iter().enumerate() {
            assert!(Arc::ptr_eq(&storage.slots, &target.slots));
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

        let published = registry.reserve_candidate(b"a").unwrap();
        let ready = registry.resolve(published.id).unwrap();
        let RegistryStorage::EagerContiguous(storage) = &registry.storage else {
            panic!("explicit eager configuration must remain contiguous");
        };
        assert!(std::ptr::eq(ready, &storage.slots[0].record));
        let (with_lock, lock_segment) = registry.resolve_with_segment(published.id).unwrap();
        assert!(std::ptr::eq(ready, with_lock));
        assert!(Arc::ptr_eq(lock_segment, &storage.lock_segments[0]));
        registry.mark_published(&published).unwrap();
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
        let required_bytes = eager_registry_accounted_bytes(SLOTS).unwrap();
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
        assert!(storage.slots.is_empty());
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
                storage.slots[index].state.load(Ordering::Acquire),
                SLOT_READY
            );
            assert!(std::ptr::eq(
                registry.resolve(candidate.id).unwrap(),
                &storage.slots[index].record
            ));
        }
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
        let entry = &segment.slots[0];

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

        let candidate = registry.reserve_candidate(b"record").unwrap();
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

        registry.mark_published(&candidate).unwrap();
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
        assert_eq!(
            segment.slots[1].state.load(Ordering::Acquire),
            SLOT_UNALLOCATED
        );
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
        assert!(segment.slots[..THREADS]
            .iter()
            .all(|entry| entry.state.load(Ordering::Acquire) == SLOT_READY));
        assert_eq!(
            segment.slots[THREADS].state.load(Ordering::Acquire),
            SLOT_UNALLOCATED
        );
    }

    #[test]
    fn values_preserve_binary_bytes_across_the_inline_boundary() {
        for length in [0, 1, INLINE_VALUE_CAPACITY] {
            let bytes: Vec<_> = (0..length).map(|index| index as u8).collect();
            let value = Value::from(bytes.as_slice());
            assert!(matches!(value.repr, ValueRepr::Inline { .. }));
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
        assert_eq!(shared.as_slice(), bytes);
        let cloned = value.clone();
        let ValueRepr::Shared(cloned_shared) = &cloned.repr else {
            panic!("cloning a shared value must preserve shared storage");
        };
        assert!(Arc::ptr_eq(shared, cloned_shared));
        assert_eq!(cloned, value);

        let arc_slice: Arc<[u8]> = Arc::from(bytes.clone());
        assert_eq!(Value::from(arc_slice).as_ref(), bytes);

        let binary = Value::from(&b"\0a\0\xff"[..]);
        assert_eq!(binary.as_ref(), b"\0a\0\xff");
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
        // see earlier intents, and an absent -> live -> absent sequence has no
        // net membership effect.
        let membership_before = table.shared().membership_version.observe().unwrap();
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
                    match index {
                        0 => PointMutation::Put(Value::from(&b"A2"[..])),
                        1 => PointMutation::Put(Value::from(&b"A"[..])),
                        2 => PointMutation::Put(Value::from(&b"B2"[..])),
                        3 => PointMutation::Put(Value::from(&b"B3"[..])),
                        4 => PointMutation::Put(Value::from(&b"M"[..])),
                        5 => PointMutation::Remove,
                        _ => unreachable!(),
                    }
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
            table.shared().membership_version.observe().unwrap(),
            membership_before
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
        assert_eq!(
            table
                .shared()
                .resolve_directory_record(hit_a_id)
                .unwrap()
                .version
                .observe()
                .unwrap(),
            hit_a_before
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
        assert_eq!(
            table
                .shared()
                .resolve_directory_record(miss_id)
                .unwrap()
                .version
                .observe()
                .unwrap(),
            miss_before
        );
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
        assert_eq!(committed_record_snapshot(record), RecordState::tombstone());

        // A live empty value must not collapse into the tombstone tag.
        let mut write_empty = worker.begin().unwrap();
        table
            .put_inner(&mut write_empty, None, key, Value::default())
            .unwrap();
        committed(write_empty.commit());
        assert_eq!(
            record.state.tag.load(Ordering::Acquire),
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
                match table.get_inner(&mut txn, None, b"state/race") {
                    Ok(value) => {
                        let valid = value.as_ref().is_none_or(|value| {
                            value.as_ref() == INLINE_A
                                || value.as_ref() == INLINE_B
                                || value.as_ref() == SHARED
                        });
                        assert!(valid, "reader observed torn committed bytes: {value:?}");
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
    fn abort_keeps_an_interned_tombstone_but_publishes_no_value() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let mut txn = worker.begin().unwrap();
        table
            .put_inner(&mut txn, None, b"abort", Arc::from(&b"value"[..]))
            .unwrap();
        assert_eq!(txn.abort().reason(), &AbortReason::Explicit);
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
            let _publication = record.state.begin_publication(table.shared());
            panic!("deliberately interrupt committed-state publication");
        }));
        assert!(poisoned.is_err());
        assert_eq!(
            record.state.tag.load(Ordering::Acquire),
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
        let record = table.shared().resolve_directory_record(record_id).unwrap();
        // Simulate a publication marker that survived without the matching
        // version transition. A racing real publication instead changes or
        // locks the version and is reported as a retryable conflict.
        record
            .state
            .tag
            .store(RECORD_STATE_UPDATING, Ordering::Release);

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
    fn incomplete_state_inside_a_changed_version_sandwich_is_retryable() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let mut intern = worker.begin().unwrap();
        assert_eq!(table.get_inner(&mut intern, None, b"sandwich"), Ok(None));
        committed(intern.commit());

        let record_id = table.shared().lookup(None, b"sandwich").unwrap().unwrap();
        let record = table.shared().resolve_directory_record(record_id).unwrap();
        let observed = record.version.observe().unwrap();
        let owner = sto_core::OwnerId::new(0).unwrap();
        let mut guard = record.version.try_acquire_detached(owner).unwrap();
        record
            .state
            .tag
            .store(RECORD_STATE_UPDATING, Ordering::Release);

        assert_eq!(
            table
                .record_resource
                .adapter()
                .snapshot_state(record, observed),
            Err(AccessError::Conflict(Conflict::ReadValidation))
        );
        assert_eq!(table.health(), TableHealth::Healthy);
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Healthy);

        record
            .state
            .tag
            .store(RECORD_STATE_TOMBSTONE, Ordering::Release);
        guard.release_abort(&record.version).unwrap();
        assert_eq!(committed_record_snapshot(record), RecordState::Tombstone);
    }

    #[test]
    fn same_value_and_write_then_revert_are_record_version_no_ops() {
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
        assert_eq!(record.version.observe().unwrap(), original_version);

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
        assert_eq!(record.version.observe().unwrap(), original_version);
        assert_eq!(
            committed_record_snapshot(record).value().map(Value::as_ref),
            Some(original)
        );
    }

    #[test]
    fn stale_same_value_intent_still_validates_its_first_snapshot() {
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
        assert!(Arc::ptr_eq(&first_segment.slots, &next_lock_segment.slots));
        assert!(!Arc::ptr_eq(
            &first_segment.slots,
            &next_registry_segment.slots
        ));
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
    fn membership_lock_sorts_before_every_record_lock() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let shared = table.shared();
        let candidate = shared.registry.reserve_candidate(b"key").unwrap();
        let record_identity = LockIdentity::new(
            runtime.id(),
            shared.namespace,
            shared.record_lock_class,
            candidate.id.get(),
        );
        assert!(shared.membership_identity < record_identity);
        shared.registry.prove_unpublished(&candidate).unwrap();
    }

    #[test]
    fn membership_aggregation_preserves_other_changed_records() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let before = table.shared().membership_version.observe().unwrap();
        let mut txn = worker.begin().unwrap();
        table
            .put_inner(&mut txn, None, b"a", Arc::from(&b"one"[..]))
            .unwrap();
        table
            .put_inner(&mut txn, None, b"b", Arc::from(&b"two"[..]))
            .unwrap();
        table.remove_inner(&mut txn, None, b"a").unwrap();
        committed(txn.commit());
        let after = table.shared().membership_version.observe().unwrap();
        assert_eq!(after.get(), before.get() + 1);

        let mut verify = worker.begin().unwrap();
        assert_eq!(table.get_inner(&mut verify, None, b"a").unwrap(), None);
        assert_eq!(
            table.get_inner(&mut verify, None, b"b").unwrap().as_deref(),
            Some(&b"two"[..])
        );
        committed(verify.commit());
    }

    #[test]
    fn net_liveness_cancellation_does_not_advance_membership() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();

        let mut setup = worker.begin().unwrap();
        table
            .put_inner(&mut setup, None, b"live", Arc::from(&b"original"[..]))
            .unwrap();
        committed(setup.commit());

        let before_live = table.shared().membership_version.observe().unwrap();
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
        let after_live = table.shared().membership_version.observe().unwrap();
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
        let after_absent = table.shared().membership_version.observe().unwrap();
        assert_eq!(after_absent, before_absent);
        assert_eq!(
            absent_record.version.observe().unwrap(),
            absent_record_before
        );

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
    fn ordinary_updates_skip_membership_items_and_preserve_registry_usage() {
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
        let membership_before = table.shared().membership_version.observe().unwrap();

        // Both item slots are record items. An ordinary live-to-live update
        // must not try to construct a third, table-membership item.
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
            table.shared().membership_version.observe().unwrap(),
            membership_before
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
    fn structural_scan_seam_never_blocks_a_miss_publisher() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
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
    fn ready_and_published_resolution_borrow_the_same_stable_arena_record() {
        let registry = isolated_registry(
            TableConfig::new()
                .with_max_retained_records(1)
                .with_max_retained_key_bytes(8)
                .with_max_consumed_record_ids(1),
        );
        let candidate = registry.reserve_candidate(b"record").unwrap();
        let ready = registry.resolve(candidate.id).unwrap();
        assert!(std::ptr::eq(
            ready,
            &registry.entry(candidate.id).unwrap().record
        ));

        registry.mark_published(&candidate).unwrap();
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
        let candidate = registry.reserve_candidate(b"race").unwrap();
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
            registry.mark_published(&candidate).unwrap();
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
        assert_eq!(chunk.stop_reason, ScanStopReason::EntryCapacity);
        assert_eq!(chunk.next_key_bytes_required, b"long".len());
        assert!(matches!(
            chunk.resume,
            DirectoryScanResume::Exclusive(ref key) if key.as_ref() == b"a"
        ));
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
    fn phantom_membership_change_aborts_a_completed_scan() {
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
