#![deny(unsafe_code)]

//! Transactional binary-key records over the safe Masstree directory.
//!
//! Masstree owns only the append-only `key -> RecordId` index. This crate owns
//! stable registry slots, immutable value/tombstone snapshots, OCC versions,
//! physical locks, and the table membership resource. Point misses eagerly
//! intern a tombstone, but every abstract mutation remains deferred until the
//! native Rust STO commit protocol installs it.

use std::{
    collections::{BTreeMap, BTreeSet},
    fmt,
    sync::{
        atomic::{AtomicBool, AtomicU64, AtomicU8, Ordering},
        Arc, RwLock, RwLockReadGuard, RwLockWriteGuard, TryLockError,
    },
};

use arc_swap::{ArcSwap, ArcSwapOption};
#[cfg(not(test))]
use masstree::{
    Error as MasstreeError, InsertError as MasstreeInsertError, NativeStatus,
    PublicationDisposition, ScanRequest as NativeScanRequest, ScanResume as NativeScanResume, Tree,
};
use masstree::{InsertOutcome as DirectoryInsertOutcome, RecordId, ScanStopReason, Worker};
pub use masstree::{KeyBound as ScanBound, ScanDirection};
use sto_core::{
    AccessError, Active, AdapterFault, AdapterFaultKind, AdapterPhase, AtomicVersion,
    CapacityError, CheckError, Conflict, Entry, ExecutionCheckContext, FinishContext,
    FinishDisposition, FinishItem, InstallContext, InstallItem, LockClass, LockIdentity,
    LockNamespaceId, LockRequest, LockUse, NoPredicate, ObjectId, ObservationOrder, OccVersion,
    OpacityToken, PredicateContext, PreflightContext, PreflightItem, PrepareError,
    RegisteredResource, RegistrationError, ResourceClass, Runtime, Transaction,
    TransactionalResource, ValidationContext, VersionLock,
};
#[cfg(not(test))]
use sto_core::{FailurePhase, InvalidUse, PoisonInfo};

const RECORD_RESOURCE_CLASS_VALUE: u32 = 1;
const MEMBERSHIP_RESOURCE_CLASS_VALUE: u32 = 2;
const MEMBERSHIP_LOCK_CLASS_VALUE: u32 = 1;
const RECORD_LOCK_CLASS_VALUE: u32 = 2;
const MEMBERSHIP_LOCK_KEY: u64 = 0;

const SLOT_RESERVED: u8 = 0;
const SLOT_READY: u8 = 1;
const SLOT_PUBLISHED: u8 = 2;
const SLOT_PROVEN_UNPUBLISHED: u8 = 3;
const SLOT_PUBLICATION_UNKNOWN: u8 = 4;

const TABLE_HEALTHY: u8 = 0;
const TABLE_POISONED: u8 = 1;
const TABLE_PUBLICATION_UNKNOWN: u8 = 2;

/// An immutable binary value snapshot returned by point operations.
pub type Value = Arc<[u8]>;

/// Bounded append-only registry and retained-memory limits for one table.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TableConfig {
    max_retained_records: u64,
    max_retained_key_bytes: u64,
    max_consumed_record_ids: u64,
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
            registry: Registry::new(config),
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

    /// Reads the staged value or the first validated committed snapshot.
    pub fn get(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
    ) -> Result<Option<Value>, AccessError> {
        self.get_inner(txn, Some(worker), key)
    }

    /// Stages an unconditional live value and returns the prior abstract value.
    pub fn put(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        value: &[u8],
    ) -> Result<Option<Value>, AccessError> {
        self.put_inner(txn, Some(worker), key, Arc::from(value))
    }

    /// Stages a value only when the transaction-local record is absent.
    pub fn insert(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        value: &[u8],
    ) -> Result<InsertOutcome, AccessError> {
        self.insert_inner(txn, Some(worker), key, Arc::from(value))
    }

    /// Stages a logical tombstone and returns the prior abstract value.
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
    /// copied physical record as an ordinary STO read, and overlays all point
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

    fn get_inner(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
    ) -> Result<Option<Value>, AccessError> {
        let key = RecordKey::copy_from(key);
        let adapter = self.record_resource.adapter();
        txn.with_item(&self.record_resource, key.clone(), |entry| {
            let _record = adapter.prepare_access(&key, entry, worker)?;
            Ok(current_state(entry)
                .filter(|state| state.live)
                .map(|state| Arc::clone(&state.value)))
        })
    }

    fn put_inner(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        value: Value,
    ) -> Result<Option<Value>, AccessError> {
        let key = RecordKey::copy_from(key);
        let adapter = self.record_resource.adapter();
        let operation = txn.with_item(&self.record_resource, key.clone(), move |entry| {
            let record = adapter.prepare_access(&key, entry, worker)?;
            let previous = current_state(entry)
                .filter(|state| state.live)
                .map(|state| Arc::clone(&state.value));
            let replacement = Arc::new(RecordState { live: true, value });
            entry.stage(Arc::clone(&replacement))?;
            let changed = !committed_state(entry)?.live;
            Ok(RecordEffect::write(previous, &record, replacement, changed))
        })?;
        self.apply_membership_effect(txn, operation)
    }

    fn insert_inner(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
        value: Value,
    ) -> Result<InsertOutcome, AccessError> {
        let key = RecordKey::copy_from(key);
        let adapter = self.record_resource.adapter();
        let operation = txn.with_item(&self.record_resource, key.clone(), move |entry| {
            let record = adapter.prepare_access(&key, entry, worker)?;
            if let Some(current) = current_state(entry).filter(|state| state.live) {
                return Ok(RecordEffect::read(InsertOutcome::AlreadyPresent(
                    Arc::clone(&current.value),
                )));
            }
            let replacement = Arc::new(RecordState { live: true, value });
            entry.stage(Arc::clone(&replacement))?;
            let changed = !committed_state(entry)?.live;
            Ok(RecordEffect::write(
                InsertOutcome::Inserted,
                &record,
                replacement,
                changed,
            ))
        })?;
        self.apply_membership_effect(txn, operation)
    }

    fn remove_inner(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        key: &[u8],
    ) -> Result<Option<Value>, AccessError> {
        let key = RecordKey::copy_from(key);
        let adapter = self.record_resource.adapter();
        let operation = txn.with_item(&self.record_resource, key.clone(), |entry| {
            let record = adapter.prepare_access(&key, entry, worker)?;
            let Some(current) = current_state(entry).filter(|state| state.live) else {
                return Ok(RecordEffect::read(None));
            };
            let previous = Some(Arc::clone(&current.value));
            let replacement = Arc::new(RecordState::tombstone());
            entry.stage(Arc::clone(&replacement))?;
            let changed = committed_state(entry)?.live;
            Ok(RecordEffect::write(previous, &record, replacement, changed))
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
        let overlay = txn.with_item(&self.membership_resource, (), |entry| {
            membership.observe_and_overlay(entry)
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
        let result = self.scan_while_structurally_stable(txn, worker, request, overlay, config);
        drop(structural);
        result
    }

    fn scan_while_structurally_stable(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        request: ScanRequest<'_>,
        overlay: Vec<StagedRecord>,
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
                let snapshot = txn.with_item(&self.record_resource, key.clone(), |entry| {
                    let record = self.shared().resolve_verified(record_id, &key.0)?;
                    adapter.prepare_resolved_access(&key, entry, record)?;
                    committed_state(entry)
                })?;
                if base
                    .insert(
                        key,
                        ScannedRecord {
                            record_id,
                            state: snapshot,
                        },
                    )
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

        for staged in overlay {
            if !key_in_bounds(&staged.key.0, request.lower, request.upper) {
                continue;
            }
            let Some(scanned) = base.get_mut(&staged.key) else {
                return self.fail_scan(
                    txn,
                    table_fault("staged record is absent from a stable directory scan"),
                );
            };
            if scanned.record_id != staged.record_id {
                return self.fail_scan(
                    txn,
                    table_fault("staged record and directory RecordId disagree"),
                );
            }
            scanned.state = staged.state;
        }

        let result_capacity = request.limit.min(base.len());
        let mut result = Vec::new();
        if result.try_reserve_exact(result_capacity).is_err() {
            return self.fail_scan(txn, CapacityError::BufferLimit.into());
        }
        match request.direction {
            ScanDirection::Forward => {
                for (key, scanned) in base {
                    if scanned.state.live {
                        result.push(ScanRecord {
                            key: key.0,
                            value: Arc::clone(&scanned.state.value),
                        });
                        if result.len() == request.limit {
                            break;
                        }
                    }
                }
            }
            ScanDirection::Reverse => {
                for (key, scanned) in base.into_iter().rev() {
                    if scanned.state.live {
                        result.push(ScanRecord {
                            key: key.0,
                            value: Arc::clone(&scanned.state.value),
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

    fn write(result: R, record: &Arc<Record>, state: Arc<RecordState>, changed: bool) -> Self {
        Self {
            result,
            membership: Some(MembershipUpdate {
                staged: StagedRecord {
                    record_id: record.id,
                    key: RecordKey(Arc::clone(&record.key)),
                    state,
                },
                changed,
            }),
        }
    }
}

struct MembershipUpdate {
    staged: StagedRecord,
    changed: bool,
}

#[derive(Clone)]
struct StagedRecord {
    record_id: RecordId,
    key: RecordKey,
    state: Arc<RecordState>,
}

struct ScannedRecord {
    record_id: RecordId,
    state: Arc<RecordState>,
}

#[derive(Clone, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
struct RecordKey(Arc<[u8]>);

impl RecordKey {
    fn copy_from(key: &[u8]) -> Self {
        Self(Arc::from(key))
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
    fn health(&self) -> TableHealth {
        match self.health.load(Ordering::Acquire) {
            TABLE_HEALTHY => TableHealth::Healthy,
            TABLE_POISONED => TableHealth::Poisoned,
            _ => TableHealth::PublicationUnknown,
        }
    }

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

    fn lookup_or_intern(
        &self,
        worker: Option<&Worker>,
        key: &RecordKey,
    ) -> Result<Arc<Record>, AccessError> {
        self.ensure_healthy()?;
        let found = self
            .directory
            .get(worker, &key.0)
            .inspect_err(|error| self.note_access_error(error))?;
        if let Some(record_id) = found {
            return self.resolve_verified(record_id, &key.0);
        }

        let candidate = self
            .registry
            .reserve_candidate(
                Arc::clone(&key.0),
                self.runtime_id,
                self.namespace,
                self.record_lock_class,
            )
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

        let result = self.directory.get_or_insert(worker, &key.0, candidate.id);
        let resolved = match result {
            Ok(DirectoryInsertOutcome::Inserted(winner)) => {
                if winner != candidate.id {
                    self.poison();
                    return Err(table_fault("inserted winner differs from candidate"));
                }
                self.registry
                    .mark_published(&candidate)
                    .inspect_err(|error| self.note_access_error(error))?;
                self.resolve_verified(winner, &key.0)
            }
            Ok(DirectoryInsertOutcome::Existing(winner)) => {
                self.registry
                    .prove_unpublished(&candidate)
                    .inspect_err(|error| self.note_access_error(error))?;
                self.resolve_verified(winner, &key.0)
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
    ) -> Result<Arc<Record>, AccessError> {
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
    ) -> Result<Arc<Record>, AccessError> {
        match error {}
    }

    fn resolve_verified(
        &self,
        record_id: RecordId,
        expected_key: &[u8],
    ) -> Result<Arc<Record>, AccessError> {
        let record = self
            .registry
            .resolve(record_id)
            .inspect_err(|error| self.note_access_error(error))?;
        if record.key.as_ref() != expected_key || record.id != record_id {
            self.poison();
            return Err(table_fault("directory key and registry record disagree"));
        }
        Ok(record)
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

struct Registry {
    slots: RwLock<Vec<Option<Arc<RegistryEntry>>>>,
    consumed: AtomicU64,
    retained_records: AtomicU64,
    retained_key_bytes: AtomicU64,
    config: TableConfig,
    effective_id_limit: u64,
}

impl Registry {
    fn new(config: TableConfig) -> Self {
        let addressable = u64::try_from(isize::MAX).unwrap_or(u64::MAX);
        Self {
            slots: RwLock::new(Vec::new()),
            consumed: AtomicU64::new(0),
            retained_records: AtomicU64::new(0),
            retained_key_bytes: AtomicU64::new(0),
            effective_id_limit: config.max_consumed_record_ids.min(addressable),
            config,
        }
    }

    fn usage(&self) -> TableUsage {
        TableUsage {
            retained_records: self.retained_records.load(Ordering::Acquire),
            retained_key_bytes: self.retained_key_bytes.load(Ordering::Acquire),
            consumed_record_ids: self.consumed.load(Ordering::Acquire),
        }
    }

    fn reserve_candidate(
        &self,
        key: Arc<[u8]>,
        runtime_id: sto_core::RuntimeId,
        namespace: LockNamespaceId,
        record_lock_class: LockClass,
    ) -> Result<Candidate, AccessError> {
        self.reserve_retained(&key)?;
        let raw_id =
            match self
                .consumed
                .fetch_update(Ordering::AcqRel, Ordering::Acquire, |current| {
                    (current < self.effective_id_limit && current < u64::MAX).then_some(current + 1)
                }) {
                Ok(previous) => previous + 1,
                Err(_) => {
                    self.release_retained(key.len() as u64);
                    return Err(CapacityError::BufferLimit.into());
                }
            };
        let record_id = RecordId::new(raw_id).expect("checked allocation never produces zero");
        let version = Arc::new(AtomicVersion::default());
        let lock = Arc::new(VersionLock::new(Arc::clone(&version)));
        let lock_identity =
            LockIdentity::new(runtime_id, namespace, record_lock_class, record_id.get());
        let record = Arc::new(Record {
            id: record_id,
            key,
            version,
            lock,
            lock_identity,
            state: ArcSwap::from_pointee(RecordState::tombstone()),
        });
        let entry = Arc::new(RegistryEntry {
            state: AtomicU8::new(SLOT_RESERVED),
            record: ArcSwapOption::from(Some(Arc::clone(&record))),
            quota_retained: AtomicBool::new(true),
            key_bytes: record.key.len() as u64,
        });

        if let Err(error) = self.insert_slot(record_id, Arc::clone(&entry)) {
            self.release_entry_quota(&entry);
            return Err(error);
        }
        entry.state.store(SLOT_READY, Ordering::Release);
        Ok(Candidate {
            id: record_id,
            entry,
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

    fn insert_slot(
        &self,
        record_id: RecordId,
        entry: Arc<RegistryEntry>,
    ) -> Result<(), AccessError> {
        let index = usize::try_from(record_id.get() - 1)
            .map_err(|_| AccessError::from(CapacityError::KeyLimit))?;
        let mut slots = match self.slots.try_write() {
            Ok(slots) => slots,
            Err(TryLockError::WouldBlock) => return Err(Conflict::HiddenLockBusy.into()),
            Err(TryLockError::Poisoned(_)) => {
                return Err(table_fault("record registry is poisoned"));
            }
        };
        if index >= slots.len() {
            let additional = index + 1 - slots.len();
            slots
                .try_reserve(additional)
                .map_err(|_| CapacityError::BufferLimit)?;
            slots.resize_with(index + 1, || None);
        }
        if slots[index].is_some() {
            return Err(table_fault("record ID registry slot was reused"));
        }
        slots[index] = Some(entry);
        Ok(())
    }

    fn resolve(&self, record_id: RecordId) -> Result<Arc<Record>, AccessError> {
        let index = usize::try_from(record_id.get() - 1)
            .map_err(|_| AccessError::from(CapacityError::KeyLimit))?;
        let entry = match self.slots.try_read() {
            Ok(slots) => slots.get(index).and_then(Option::as_ref).cloned(),
            Err(TryLockError::WouldBlock) => return Err(Conflict::HiddenLockBusy.into()),
            Err(TryLockError::Poisoned(_)) => {
                return Err(table_fault("record registry is poisoned"));
            }
        }
        .ok_or_else(|| table_fault("directory returned an unallocated RecordId"))?;
        let state = entry.state.load(Ordering::Acquire);
        if !matches!(state, SLOT_READY | SLOT_PUBLISHED) {
            return Err(table_fault("directory returned an unusable registry slot"));
        }
        entry
            .record
            .load_full()
            .ok_or_else(|| table_fault("ready registry slot has no record"))
    }

    fn mark_published(&self, candidate: &Candidate) -> Result<(), AccessError> {
        transition_slot(&candidate.entry, SLOT_READY, SLOT_PUBLISHED)
    }

    fn prove_unpublished(&self, candidate: &Candidate) -> Result<(), AccessError> {
        transition_slot(&candidate.entry, SLOT_READY, SLOT_PROVEN_UNPUBLISHED)?;
        let _ = candidate.entry.record.swap(None);
        self.release_entry_quota(&candidate.entry);
        Ok(())
    }

    fn mark_unknown(&self, candidate: &Candidate) -> Result<(), AccessError> {
        transition_slot(&candidate.entry, SLOT_READY, SLOT_PUBLICATION_UNKNOWN)
    }

    fn release_entry_quota(&self, entry: &RegistryEntry) {
        if entry.quota_retained.swap(false, Ordering::AcqRel) {
            self.release_retained(entry.key_bytes);
        }
    }

    fn release_retained(&self, key_bytes: u64) {
        self.retained_key_bytes
            .fetch_sub(key_bytes, Ordering::AcqRel);
        self.retained_records.fetch_sub(1, Ordering::AcqRel);
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
    entry: Arc<RegistryEntry>,
}

struct RegistryEntry {
    state: AtomicU8,
    record: ArcSwapOption<Record>,
    quota_retained: AtomicBool,
    key_bytes: u64,
}

struct Record {
    id: RecordId,
    key: Arc<[u8]>,
    version: Arc<AtomicVersion>,
    lock: Arc<VersionLock>,
    lock_identity: LockIdentity,
    state: ArcSwap<RecordState>,
}

#[derive(Debug, Eq, PartialEq)]
struct RecordState {
    live: bool,
    value: Value,
}

impl RecordState {
    fn tombstone() -> Self {
        Self {
            live: false,
            value: Arc::from([]),
        }
    }
}

struct RecordAdapter {
    table: Arc<TableShared>,
}

#[derive(Default)]
struct RecordLocal {
    record: Option<Arc<Record>>,
    first_snapshot: Option<Arc<RecordState>>,
    displaced: Option<Arc<RecordState>>,
}

#[derive(Clone)]
struct RecordObservation {
    record: Arc<Record>,
    version: OccVersion,
}

impl OpacityToken for RecordObservation {
    fn observation_order(&self) -> ObservationOrder {
        ObservationOrder::Ordered(self.version)
    }
}

struct RecordPrepared {
    record: Arc<Record>,
    lock_use: Option<LockUse<VersionLock>>,
}

impl RecordAdapter {
    fn prepare_access(
        &self,
        key: &RecordKey,
        entry: &mut Entry<'_, Self>,
        worker: Option<&Worker>,
    ) -> Result<Arc<Record>, AccessError> {
        self.table.ensure_healthy()?;
        let record = match entry.local().record.as_ref() {
            Some(record) => Arc::clone(record),
            None => self.table.lookup_or_intern(worker, key)?,
        };
        self.prepare_resolved_access(key, entry, record)
    }

    fn prepare_resolved_access(
        &self,
        key: &RecordKey,
        entry: &mut Entry<'_, Self>,
        record: Arc<Record>,
    ) -> Result<Arc<Record>, AccessError> {
        self.table.ensure_healthy()?;
        if record.key.as_ref() != key.0.as_ref() {
            self.table.poison();
            return Err(table_fault("transaction item key and record disagree"));
        }
        if let Some(existing) = entry.local().record.as_ref() {
            if !Arc::ptr_eq(existing, &record) || existing.id != record.id {
                self.table.poison();
                return Err(table_fault("transaction item resolved to two records"));
            }
        } else {
            entry.local_mut().record = Some(Arc::clone(&record));
        }

        if entry.local().first_snapshot.is_none() {
            let observed = record
                .version
                .observe()
                .map_err(|_| AccessError::from(Conflict::LockBusy))?;
            let snapshot = record.state.load_full();
            if !record.version.validate(observed) {
                return Err(Conflict::ReadValidation.into());
            }
            entry.record_read(RecordObservation {
                record: Arc::clone(&record),
                version: observed,
            })?;
            entry.local_mut().first_snapshot = Some(snapshot);
        }
        Ok(record)
    }

    fn validate_observation(
        &self,
        observation: &RecordObservation,
        prepared: &RecordPrepared,
        cx: &ValidationContext<'_>,
    ) -> Result<(), CheckError> {
        if !Arc::ptr_eq(&observation.record, &prepared.record) {
            return Err(AdapterFault::new(
                AdapterPhase::Validation,
                AdapterFaultKind::LockIdentityMismatch,
            )
            .into());
        }
        let valid = if let Some(lock_use) = prepared.lock_use.as_ref() {
            let guard = cx.guard(lock_use)?;
            if !guard.is_for(&observation.record.version) || guard.owner() != cx.owner() {
                return Err(AdapterFault::new(
                    AdapterPhase::Validation,
                    AdapterFaultKind::LockIdentityMismatch,
                )
                .into());
            }
            observation
                .record
                .version
                .validate_own(observation.version, cx.owner())
        } else {
            observation.record.version.validate(observation.version)
        };
        if valid {
            Ok(())
        } else {
            Err(Conflict::ReadValidation.into())
        }
    }
}

impl TransactionalResource for RecordAdapter {
    type Key = RecordKey;
    type Local = RecordLocal;
    type Observation = RecordObservation;
    type Predicate = NoPredicate;
    type Intent = Arc<RecordState>;
    type Prepared = RecordPrepared;

    fn new_local(&self, _key: &Self::Key) -> Result<Self::Local, sto_core::ItemInitError> {
        Ok(RecordLocal::default())
    }

    fn preflight(
        &self,
        _key: &Self::Key,
        item: PreflightItem<'_, Self>,
        cx: &mut PreflightContext<'_>,
    ) -> Result<Self::Prepared, PrepareError> {
        let record = item
            .local()
            .record
            .as_ref()
            .cloned()
            .ok_or_else(|| AdapterFault::invariant(AdapterPhase::Preflight))?;
        let changed = match (item.local().first_snapshot.as_ref(), item.intent()) {
            (Some(original), Some(replacement)) => original.as_ref() != replacement.as_ref(),
            (_, None) => false,
            (None, Some(_)) => {
                return Err(AdapterFault::invariant(AdapterPhase::Preflight).into());
            }
        };
        let lock_use = if changed {
            Some(cx.require_lock(LockRequest::new(
                record.lock_identity.clone(),
                Arc::clone(&record.lock),
            ))?)
        } else {
            None
        };
        Ok(RecordPrepared { record, lock_use })
    }

    fn revalidate_read(
        &self,
        _key: &Self::Key,
        observation: &Self::Observation,
        _cx: &ExecutionCheckContext<'_>,
    ) -> Result<(), CheckError> {
        if observation.record.version.validate(observation.version) {
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
        self.validate_observation(observation, prepared, cx)
    }

    fn install(
        &self,
        _key: &Self::Key,
        mut item: InstallItem<'_, Self>,
        prepared: &mut Self::Prepared,
        cx: &mut InstallContext<'_>,
    ) {
        let Some(lock_use) = prepared.lock_use.as_ref() else {
            return;
        };
        let commit_id = cx
            .occ_commit_id()
            .expect("sto-masstree record write has no OCC commit ID");
        let guard = cx
            .guard_mut(lock_use)
            .unwrap_or_else(|error| panic!("sto-masstree record lock invariant: {error}"));
        if !guard.is_held()
            || !guard.is_for(&prepared.record.version)
            || commit_id.to_version() <= guard.before()
        {
            panic!("sto-masstree record install received the wrong version guard");
        }
        if item.local_mut().displaced.is_some() {
            panic!("sto-masstree record was installed more than once");
        }
        let displaced = prepared.record.state.swap(Arc::clone(item.intent()));
        item.local_mut().displaced = Some(displaced);
    }

    fn finish(
        &self,
        _key: &Self::Key,
        mut item: FinishItem<'_, Self>,
        _prepared: Option<&mut Self::Prepared>,
        _disposition: FinishDisposition,
        _cx: &mut FinishContext<'_>,
    ) {
        item.local_mut().record = None;
        item.local_mut().first_snapshot = None;
        item.local_mut().displaced = None;
        let _ = item.take_remaining_intent();
    }
}

fn current_state(entry: &Entry<'_, RecordAdapter>) -> Option<Arc<RecordState>> {
    entry
        .intent()
        .cloned()
        .or_else(|| entry.local().first_snapshot.as_ref().cloned())
}

fn committed_state(entry: &Entry<'_, RecordAdapter>) -> Result<Arc<RecordState>, AccessError> {
    entry
        .local()
        .first_snapshot
        .as_ref()
        .cloned()
        .ok_or_else(|| table_fault("record operation has no committed snapshot"))
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
    staged_records: BTreeMap<RecordKey, StagedRecord>,
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
            intent.changed_records.insert(update.staged.record_id);
        } else {
            intent.changed_records.remove(&update.staged.record_id);
        }
        intent
            .staged_records
            .insert(update.staged.key.clone(), update.staged);
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

    fn observe_and_overlay(
        &self,
        entry: &mut Entry<'_, Self>,
    ) -> Result<Vec<StagedRecord>, AccessError> {
        self.observe(entry)?;
        let staged_len = entry
            .intent()
            .map_or(0, |intent| intent.staged_records.len());
        let mut overlay = Vec::new();
        overlay
            .try_reserve_exact(staged_len)
            .map_err(|_| CapacityError::BufferLimit)?;
        if let Some(intent) = entry.intent() {
            overlay.extend(intent.staged_records.values().cloned());
        }
        Ok(overlay)
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
    use std::sync::Barrier;
    use sto_core::{AbortReason, CommitOutcome, RuntimeConfig};

    fn runtime_and_table(config: TableConfig) -> (Arc<Runtime>, Table) {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let table = Table::new_memory(&runtime, config);
        (runtime, table)
    }

    fn committed(outcome: Result<CommitOutcome, sto_core::CommitFailure>) {
        assert!(matches!(outcome, Ok(CommitOutcome::Committed(_))));
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
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let clone = table.clone();
        assert_eq!(table.object_id(), clone.object_id());
        assert_eq!(runtime.health(), sto_core::RuntimeHealth::Healthy);
    }

    #[test]
    fn point_operations_compose_and_read_their_own_writes() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let mut worker = runtime.attach().unwrap();
        let mut txn = worker.begin().unwrap();

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
            table
                .insert_inner(&mut txn, None, b"k", Arc::from(&b"ignored"[..]))
                .unwrap(),
            InsertOutcome::AlreadyPresent(Arc::from(&b"one"[..]))
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
        let registry = Registry::new(
            TableConfig::new()
                .with_max_retained_records(4)
                .with_max_retained_key_bytes(16)
                .with_max_consumed_record_ids(2),
        );
        let runtime = sto_core::RuntimeId::new(1).unwrap();
        let namespace = LockNamespaceId::new(1).unwrap();
        let class = LockClass::new(RECORD_LOCK_CLASS_VALUE).unwrap();
        let first = registry
            .reserve_candidate(Arc::from(&b"a"[..]), runtime, namespace, class)
            .unwrap();
        registry.prove_unpublished(&first).unwrap();
        let second = registry
            .reserve_candidate(Arc::from(&b"b"[..]), runtime, namespace, class)
            .unwrap();
        assert_eq!(first.id.get(), 1);
        assert_eq!(second.id.get(), 2);
        registry.prove_unpublished(&second).unwrap();
        assert_eq!(registry.usage().retained_records(), 0);
        assert_eq!(registry.usage().consumed_record_ids(), 2);
        assert!(matches!(
            registry.reserve_candidate(Arc::from(&b"c"[..]), runtime, namespace, class),
            Err(AccessError::Capacity(CapacityError::BufferLimit))
        ));
    }

    #[test]
    fn membership_lock_sorts_before_every_record_lock() {
        let (runtime, table) = runtime_and_table(TableConfig::default());
        let shared = table.shared();
        let candidate = shared
            .registry
            .reserve_candidate(
                Arc::from(&b"key"[..]),
                runtime.id(),
                shared.namespace,
                shared.record_lock_class,
            )
            .unwrap();
        let record = candidate.entry.record.load_full().unwrap();
        assert!(shared.membership_identity < record.lock_identity);
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
        committed(absent.commit());
        let after_absent = table.shared().membership_version.observe().unwrap();
        assert_eq!(after_absent, before_absent);

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
        let registry = Registry::new(
            TableConfig::new()
                .with_max_retained_records(2)
                .with_max_retained_key_bytes(8)
                .with_max_consumed_record_ids(2),
        );
        let runtime = sto_core::RuntimeId::new(1).unwrap();
        let namespace = LockNamespaceId::new(1).unwrap();
        let class = LockClass::new(RECORD_LOCK_CLASS_VALUE).unwrap();
        let ready = registry
            .reserve_candidate(Arc::from(&b"ready"[..]), runtime, namespace, class)
            .unwrap();
        assert_eq!(registry.resolve(ready.id).unwrap().key.as_ref(), b"ready");
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
    fn concurrent_candidate_reservations_never_overcommit_retained_quota() {
        const THREADS: usize = 16;
        const LIMIT: u64 = 4;
        let registry = Arc::new(Registry::new(
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
                let runtime = sto_core::RuntimeId::new(1).unwrap();
                let namespace = LockNamespaceId::new(1).unwrap();
                let class = LockClass::new(RECORD_LOCK_CLASS_VALUE).unwrap();
                let key: Arc<[u8]> = Arc::from(format!("key-{index}").into_bytes());
                barrier.wait();
                loop {
                    match registry.reserve_candidate(Arc::clone(&key), runtime, namespace, class) {
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
    fn scan_overlay_composes_insert_update_delete_and_cancellation() {
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
                                InsertOutcome::AlreadyPresent(Arc::from(existing.clone()))
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
