//! Optional fixed-copy `u64` records over the existing Masstree directory.
//!
//! This deliberately restricted lane keeps the general binary-value [`super::Table`]
//! unchanged. It supports an explicit initial-load phase followed by transactions
//! over an all-present map. Inserts, removals, and scans remain the responsibility
//! of the general table.

use std::{
    fmt,
    sync::{
        atomic::{AtomicBool, AtomicU64, AtomicU8, Ordering},
        Arc,
    },
};

use masstree::{
    Error as MasstreeError, PointReadResult as DirectoryPointReadResult, RecordId, Worker,
};
#[cfg(not(test))]
use masstree::{PublicationDisposition, Runtime as MasstreeRuntime};
use sto_core::{
    AccessError, AcquireContext, AcquireError, Active, AdapterFault, AdapterFaultKind,
    AdapterPhase, AtomicVersion, CapacityError, CheckError, Conflict, DetachedVersionGuard, Entry,
    ExecutionCheckContext, FinishContext, FinishDisposition, FinishItem, InstallContext,
    InstallItem, LockClass, LockDisposition, LockIdentity, LockNamespaceId, LockRequest, LockUse,
    NoPredicate, ObjectId, ObservationOrder, ObservationRef, OccVersion, OpacityToken,
    PredicateContext, PreflightContext, PreflightFreeReadCapability,
    PreflightFreeValidationContext, PreflightItem, PrepareError, RegisteredResource,
    RegistrationError, ReleaseContext, ResourceClass, Runtime, TerminalReadBatchCapability,
    TerminalReadOpen, TerminalReadTransaction, Transaction, TransactionLock, TransactionalResource,
    UniqueItemKeys, ValidationContext,
};

#[cfg(not(test))]
use super::{map_masstree_error, NativeDirectory};
use super::{
    table_fault, Directory, RegistryLayout, StructuralGate, TableConfig, TableHealth, TableUsage,
    TerminalReadVisitOutcome, RECORD_LOCK_CLASS_VALUE, RECORD_LOCK_SEGMENT_SLOTS,
    RECORD_RESOURCE_CLASS_VALUE, SLOT_PROVEN_UNPUBLISHED, SLOT_PUBLISHED, SLOT_READY,
    SLOT_RESERVED, SLOT_UNALLOCATED, TABLE_HEALTHY, TABLE_POISONED,
};
#[cfg(not(test))]
use super::{SLOT_PUBLICATION_UNKNOWN, TABLE_PUBLICATION_UNKNOWN};

/// One deferred action in a fixed-copy mutation batch.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum FixedU64Mutation {
    /// Retain the observed value without staging a write.
    Keep,
    /// Stage an unconditional replacement.
    Put(u64),
}

/// Failure to create a fixed-copy table and its private native directory.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum FixedU64CreateError {
    /// Creating the fresh Masstree directory failed.
    Directory(MasstreeError),
    /// Registering or allocating the STO-side table failed.
    Registration(RegistrationError),
}

impl fmt::Display for FixedU64CreateError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Directory(error) => write!(formatter, "create fixed-u64 directory: {error}"),
            Self::Registration(error) => write!(formatter, "register fixed-u64 table: {error}"),
        }
    }
}

impl std::error::Error for FixedU64CreateError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Directory(error) => Some(error),
            Self::Registration(error) => Some(error),
        }
    }
}

impl From<MasstreeError> for FixedU64CreateError {
    fn from(error: MasstreeError) -> Self {
        Self::Directory(error)
    }
}

impl From<RegistrationError> for FixedU64CreateError {
    fn from(error: RegistrationError) -> Self {
        Self::Registration(error)
    }
}

/// Reusable directory and RecordId scratch for the fixed-copy lane.
#[derive(Debug, Default)]
pub struct FixedU64Batch {
    directory_results: Vec<DirectoryPointReadResult>,
    record_ids: Vec<RecordId>,
}

impl FixedU64Batch {
    /// Creates empty scratch storage.
    pub const fn new() -> Self {
        Self {
            directory_results: Vec::new(),
            record_ids: Vec::new(),
        }
    }

    /// Creates scratch storage sized for at least `capacity` fixed-width keys.
    pub fn with_capacity(capacity: usize) -> Self {
        Self {
            directory_results: Vec::with_capacity(capacity),
            record_ids: Vec::with_capacity(capacity),
        }
    }

    /// Returns the allocation-free capacity common to every internal buffer.
    pub fn capacity(&self) -> usize {
        self.directory_results
            .capacity()
            .min(self.record_ids.capacity())
    }

    /// Clears live scratch while retaining its allocations.
    pub fn clear(&mut self) {
        self.directory_results.clear();
        self.record_ids.clear();
    }

    fn prepare(&mut self, length: usize) -> Result<(), AccessError> {
        // Tree::get_fixed overwrites/resizes every result slot. Retain its
        // previous length so a same-sized call avoids redundant zero-filling.
        self.record_ids.clear();
        self.record_ids
            .try_reserve_exact(length)
            .map_err(|_| CapacityError::BufferLimit)?;
        Ok(())
    }
}

/// A restricted all-live `u64` table with a 16-byte hot record stride.
///
/// Masstree still owns only `key -> RecordId`. The bounded Rust arena owns one
/// adjacent OCC word and atomic `u64` value per ID. Initial loading is an
/// explicitly nontransactional, pre-worker phase; all post-load operations use the
/// ordinary `sto-core` commit protocol. This type intentionally exposes only
/// fixed-width point batches over keys published by the loader: it has no
/// transactional membership changes, variable-width values, or scans.
pub struct FixedU64Table {
    record_resource: RegisteredResource<FixedAdapter>,
}

impl FixedU64Table {
    /// Creates an eager fixed-copy table and its private native tree.
    ///
    /// `config.registry_layout()` must be [`RegistryLayout::EagerContiguous`].
    /// `native_worker` must belong to `native_runtime`. The newly created tree
    /// is never exposed, so safe callers cannot retain a clone that publishes
    /// RecordIds outside this table's loader.
    #[cfg(not(test))]
    pub fn new(
        runtime: &Arc<Runtime>,
        native_runtime: &MasstreeRuntime,
        native_worker: &Worker,
        config: TableConfig,
    ) -> Result<Self, FixedU64CreateError> {
        let tree = native_runtime.create_tree(native_worker)?;
        Self::with_directory(runtime, Directory::Native(NativeDirectory { tree }), config)
            .map_err(Into::into)
    }

    fn with_directory(
        runtime: &Arc<Runtime>,
        directory: Directory,
        config: TableConfig,
    ) -> Result<Self, RegistrationError> {
        let object = runtime.register_object()?;
        let namespace = LockNamespaceId::new(object.object_id().get())
            .expect("nonzero ObjectId always forms a lock namespace");
        let record_lock_class =
            LockClass::new(RECORD_LOCK_CLASS_VALUE).expect("record lock class is nonzero");
        let shared = Arc::new(FixedShared {
            directory,
            registry: FixedRegistry::new(
                config,
                object.runtime_id(),
                namespace,
                record_lock_class,
            )?,
            structural: StructuralGate::default(),
            health: AtomicU8::new(TABLE_HEALTHY),
            initial_load_finished: AtomicBool::new(false),
            runtime_id: object.runtime_id(),
            namespace,
            record_lock_class,
        });
        let record_class = ResourceClass::new(RECORD_RESOURCE_CLASS_VALUE)
            .expect("record resource class is nonzero");
        let record_resource = object.register_resource(
            record_class,
            FixedAdapter {
                table: Arc::clone(&shared),
            },
        )?;
        Ok(Self { record_resource })
    }

    /// Publishes one immutable key/ID binding during the untimed loading phase.
    ///
    /// Calls must finish before transactional workers begin. Repeating a key is
    /// accepted only when its already-published value equals `value`.
    pub fn insert_initial(
        &self,
        worker: &Worker,
        key: &[u8],
        value: u64,
    ) -> Result<(), AccessError> {
        self.shared().insert_initial(Some(worker), key, value)
    }

    /// Permanently closes the initial-load phase and enables transactions.
    ///
    /// The transition is synchronized with the table's publication gate. Once
    /// it succeeds, every RecordId reachable from the private directory names a
    /// fully initialized fixed record and no cold slot state can change again.
    pub fn finish_initial_load(&self) -> Result<(), AccessError> {
        self.shared().finish_initial_load()
    }

    /// Returns this table's STO object identity.
    pub fn object_id(&self) -> ObjectId {
        self.record_resource.object_id()
    }

    /// Returns the table-local fail-closed health state.
    pub fn health(&self) -> TableHealth {
        self.shared().health()
    }

    /// Returns bounded initial-load registry accounting.
    pub fn usage(&self) -> TableUsage {
        self.shared().registry.usage()
    }

    /// Visits one all-present read batch through the terminal-read typestate.
    ///
    /// A miss invokes no callback and returns `RetryOrdinary`; this restricted
    /// table has no ordinary insertion path, so the caller may report a workload
    /// violation or consult another table. Distinct directory keys aliasing one
    /// RecordId fail closed. Visitor side effects are not rolled back if final
    /// certification conflicts.
    pub fn visit_fixed_terminal<'worker, const KEY_LENGTH: usize>(
        &self,
        transaction: TerminalReadTransaction<'worker, TerminalReadOpen>,
        worker: &Worker,
        keys: &[[u8; KEY_LENGTH]],
        batch: &mut FixedU64Batch,
        visit: impl FnMut(usize, u64),
    ) -> Result<TerminalReadVisitOutcome<'worker>, AccessError> {
        self.visit_fixed_terminal_inner(transaction, Some(worker), keys, batch, visit)
    }

    fn visit_fixed_terminal_inner<'worker, const KEY_LENGTH: usize>(
        &self,
        transaction: TerminalReadTransaction<'worker, TerminalReadOpen>,
        worker: Option<&Worker>,
        keys: &[[u8; KEY_LENGTH]],
        batch: &mut FixedU64Batch,
        mut visit: impl FnMut(usize, u64),
    ) -> Result<TerminalReadVisitOutcome<'worker>, AccessError> {
        let lookup = (|| {
            batch.prepare(keys.len())?;
            if !self.resolve_fixed_ids(worker, keys, batch)? {
                return Ok(false);
            }
            self.prefetch_resolved_records(&batch.record_ids)?;
            Ok(true)
        })();
        match lookup {
            Ok(true) => {}
            Ok(false) => {
                batch.clear();
                drop(transaction);
                return Ok(TerminalReadVisitOutcome::RetryOrdinary);
            }
            Err(error) => {
                batch.clear();
                return Err(transaction.abort_with_access_error(error));
            }
        }

        let adapter = self.record_resource.adapter();
        let ready = transaction.with_terminal_read_batch(
            &self.record_resource,
            &batch.record_ids,
            |index, entry| {
                let (observation, value) = adapter.snapshot(*entry.key())?;
                entry.record_read(observation)?;
                visit(index, value);
                Ok(())
            },
        );
        match ready {
            Ok(transaction) => {
                batch.record_ids.clear();
                Ok(TerminalReadVisitOutcome::Ready {
                    transaction,
                    visited: keys.len(),
                })
            }
            Err(error) => {
                batch.clear();
                Err(error)
            }
        }
    }

    /// Applies one exactly unique, all-present fixed-width mutation batch.
    ///
    /// The transaction must be empty. `Ok(None)` means a key was absent or a
    /// repeated input key required a sequential lane; no callback or STO item
    /// was created. This restricted table deliberately supplies no insertion or
    /// sequential-duplicate fallback. Callback side effects are not rolled back
    /// if this access or the later transaction commit fails. A nonempty
    /// transaction is rejected and doomed before directory lookup, even when
    /// this batch would otherwise miss or contain duplicates.
    pub fn modify_fixed<const KEY_LENGTH: usize>(
        &self,
        transaction: &mut Transaction<'_, Active>,
        worker: &Worker,
        keys: &[[u8; KEY_LENGTH]],
        batch: &mut FixedU64Batch,
        modify: impl FnMut(usize, u64) -> FixedU64Mutation,
    ) -> Result<Option<usize>, AccessError> {
        self.modify_fixed_inner(transaction, Some(worker), keys, batch, modify)
    }

    fn modify_fixed_inner<const KEY_LENGTH: usize>(
        &self,
        transaction: &mut Transaction<'_, Active>,
        worker: Option<&Worker>,
        keys: &[[u8; KEY_LENGTH]],
        batch: &mut FixedU64Batch,
        mut modify: impl FnMut(usize, u64) -> FixedU64Mutation,
    ) -> Result<Option<usize>, AccessError> {
        transaction.with_item_session(&self.record_resource, |session| {
            let no_keys = [];
            let empty_batch = UniqueItemKeys::try_new(&no_keys)
                .expect("an empty fixed-u64 batch is exactly unique");
            if !session.try_with_unique_item_batch(empty_batch, |_, _| unreachable!())? {
                return Err(sto_core::InvalidUse::UniqueBatchRequiresEmptyTransaction.into());
            }
            batch.prepare(keys.len())?;
            if !self.resolve_fixed_ids(worker, keys, batch)? {
                batch.clear();
                return Ok(None);
            }
            let Some(unique) = UniqueItemKeys::try_new(&batch.record_ids) else {
                batch.clear();
                return Ok(None);
            };
            let adapter = self.record_resource.adapter();
            let appended = session.try_with_unique_item_batch(unique, |index, entry| {
                let record_id = unique.as_slice()[index];
                let current = adapter.prepare_access(record_id, entry)?;
                match modify(index, current) {
                    FixedU64Mutation::Keep => {}
                    FixedU64Mutation::Put(value) if value != current => {
                        entry.stage(())?;
                        *entry.local_mut() = value;
                    }
                    FixedU64Mutation::Put(_) => {}
                }
                Ok(())
            })?;
            if !appended {
                return Err(sto_core::InvalidUse::UniqueBatchRequiresEmptyTransaction.into());
            }
            batch.record_ids.clear();
            Ok(Some(keys.len()))
        })
    }

    #[cfg(not(test))]
    fn resolve_fixed_ids<const KEY_LENGTH: usize>(
        &self,
        worker: Option<&Worker>,
        keys: &[[u8; KEY_LENGTH]],
        batch: &mut FixedU64Batch,
    ) -> Result<bool, AccessError> {
        self.shared().ensure_transactional()?;
        let worker = worker.ok_or_else(|| table_fault("fixed-u64 native worker is missing"))?;
        let Directory::Native(directory) = &self.shared().directory;
        directory
            .tree
            .get_fixed(worker, keys, &mut batch.directory_results)
            .map_err(map_masstree_error)
            .inspect_err(|error| self.shared().note_access_error(error))?;
        self.convert_and_verify_ids(keys, batch)
    }

    #[cfg(test)]
    fn resolve_fixed_ids<const KEY_LENGTH: usize>(
        &self,
        _worker: Option<&Worker>,
        keys: &[[u8; KEY_LENGTH]],
        batch: &mut FixedU64Batch,
    ) -> Result<bool, AccessError> {
        self.shared().ensure_transactional()?;
        for key in keys {
            let Some(record_id) = self.shared().lookup(None, key)? else {
                return Ok(false);
            };
            batch.record_ids.push(record_id);
        }
        self.verify_directory_aliases(keys, &batch.record_ids)?;
        Ok(true)
    }

    #[cfg(not(test))]
    fn convert_and_verify_ids<const KEY_LENGTH: usize>(
        &self,
        keys: &[[u8; KEY_LENGTH]],
        batch: &mut FixedU64Batch,
    ) -> Result<bool, AccessError> {
        if batch.directory_results.len() != keys.len() {
            return Err(table_fault(
                "fixed-u64 directory lookup returned the wrong result count",
            ));
        }
        for result in batch.directory_results.iter().copied() {
            let Some(record_id) = result.record_id() else {
                return Ok(false);
            };
            batch.record_ids.push(record_id);
        }
        self.verify_directory_aliases(keys, &batch.record_ids)?;
        Ok(true)
    }

    #[inline]
    fn prefetch_resolved_records(&self, record_ids: &[RecordId]) -> Result<(), AccessError> {
        for record_id in record_ids {
            let record = self.shared().resolve_record(*record_id)?;
            super::record_prefetch::read(record);
        }
        Ok(())
    }

    fn verify_directory_aliases<const KEY_LENGTH: usize>(
        &self,
        keys: &[[u8; KEY_LENGTH]],
        record_ids: &[RecordId],
    ) -> Result<(), AccessError> {
        let mut fingerprints = 0_u64;
        for (index, record_id) in record_ids.iter().enumerate() {
            let record_id = *record_id;
            let fingerprint = 1_u64 << ((record_id.get() - 1) & 63);
            if fingerprints & fingerprint != 0 {
                for prior in 0..index {
                    if record_ids[prior] == record_id && keys[prior] != keys[index] {
                        self.shared().poison();
                        return Err(table_fault(
                            "distinct fixed-u64 directory keys resolved to one record ID",
                        ));
                    }
                }
            }
            fingerprints |= fingerprint;
        }
        Ok(())
    }

    fn shared(&self) -> &Arc<FixedShared> {
        &self.record_resource.adapter().table
    }
}

impl Clone for FixedU64Table {
    fn clone(&self) -> Self {
        Self {
            record_resource: self.record_resource.clone(),
        }
    }
}

impl fmt::Debug for FixedU64Table {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("FixedU64Table")
            .field("object_id", &self.object_id())
            .field("health", &self.health())
            .field("usage", &self.usage())
            .finish_non_exhaustive()
    }
}

struct FixedShared {
    directory: Directory,
    registry: FixedRegistry,
    structural: StructuralGate,
    health: AtomicU8,
    initial_load_finished: AtomicBool,
    runtime_id: sto_core::RuntimeId,
    namespace: LockNamespaceId,
    record_lock_class: LockClass,
}

impl FixedShared {
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
            TableHealth::Poisoned => Err(table_fault("fixed-u64 table is poisoned")),
            TableHealth::PublicationUnknown => {
                Err(table_fault("fixed-u64 publication outcome is unknown"))
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

    #[cfg(not(test))]
    fn mark_publication_unknown(&self) {
        self.health
            .store(TABLE_PUBLICATION_UNKNOWN, Ordering::Release);
    }

    fn note_access_error(&self, error: &AccessError) {
        if matches!(
            error,
            AccessError::Fault(_) | AccessError::Poisoned(_) | AccessError::Internal(_)
        ) {
            self.poison();
        }
    }

    #[inline]
    fn ensure_transactional(&self) -> Result<(), AccessError> {
        self.ensure_healthy()?;
        if self.initial_load_finished.load(Ordering::Acquire) {
            Ok(())
        } else {
            Err(sto_core::InvalidUse::IllegalItemState.into())
        }
    }

    fn finish_initial_load(&self) -> Result<(), AccessError> {
        self.ensure_healthy()?;
        let _structural = self
            .structural
            .try_write()
            .inspect_err(|error| self.note_access_error(error))?;
        self.initial_load_finished.store(true, Ordering::Release);
        Ok(())
    }

    #[inline]
    fn lookup(&self, worker: Option<&Worker>, key: &[u8]) -> Result<Option<RecordId>, AccessError> {
        self.ensure_healthy()?;
        self.directory
            .get(worker, key)
            .inspect_err(|error| self.note_access_error(error))
    }

    #[inline]
    fn resolve_initial_record(&self, id: RecordId) -> Result<&FixedRecord, AccessError> {
        self.registry
            .resolve_published(id)
            .inspect_err(|error| self.note_access_error(error))
    }

    #[inline(always)]
    fn resolve_record(&self, id: RecordId) -> Result<&FixedRecord, AccessError> {
        self.registry
            .resolve(id)
            .inspect_err(|error| self.note_access_error(error))
    }

    #[inline]
    fn resolve_for_phase(
        &self,
        id: RecordId,
        phase: AdapterPhase,
    ) -> Result<&FixedRecord, AdapterFault> {
        self.registry.resolve(id).map_err(|error| {
            self.note_access_error(&error);
            AdapterFault::invariant(phase)
        })
    }

    #[inline]
    fn resolve_with_lock_for_phase(
        &self,
        id: RecordId,
        phase: AdapterPhase,
    ) -> Result<(&FixedRecord, &Arc<FixedLockSegment>), AdapterFault> {
        self.registry.resolve_with_lock(id).map_err(|error| {
            self.note_access_error(&error);
            AdapterFault::invariant(phase)
        })
    }

    fn insert_initial(
        &self,
        worker: Option<&Worker>,
        key: &[u8],
        value: u64,
    ) -> Result<(), AccessError> {
        self.ensure_healthy()?;
        let structural = self
            .structural
            .try_write()
            .inspect_err(|error| self.note_access_error(error))?;
        if self.initial_load_finished.load(Ordering::Acquire) {
            return Err(sto_core::InvalidUse::IllegalItemState.into());
        }
        if let Some(existing) = self.lookup(worker, key)? {
            let record = self.resolve_initial_record(existing)?;
            let observed = record.version.observe().map_err(|_| Conflict::LockBusy)?;
            let current = record.value.load(Ordering::Relaxed);
            if record.version.validate(observed) && current == value {
                return Ok(());
            }
            return Err(table_fault("fixed-u64 initial value changed or mismatched"));
        }

        let candidate = self
            .registry
            .reserve_candidate(key, value)
            .inspect_err(|error| self.note_access_error(error))?;
        let result = self.directory.get_or_insert(worker, key, candidate.id);
        let resolved = match result {
            Ok(super::DirectoryInsertOutcome::Inserted(winner)) => {
                if winner != candidate.id {
                    self.poison();
                    Err(table_fault(
                        "fixed-u64 inserted winner differs from candidate",
                    ))
                } else {
                    self.registry
                        .mark_published(&candidate)
                        .inspect_err(|error| self.note_access_error(error))?;
                    Ok(winner)
                }
            }
            Ok(super::DirectoryInsertOutcome::Existing(winner)) => {
                self.registry
                    .prove_unpublished(&candidate)
                    .inspect_err(|error| self.note_access_error(error))?;
                Ok(winner)
            }
            Err(error) => self.handle_insert_error(&candidate, error),
        };
        drop(structural);
        let winner = resolved?;
        let record = self.resolve_initial_record(winner)?;
        let observed = record.version.observe().map_err(|_| Conflict::LockBusy)?;
        let current = record.value.load(Ordering::Relaxed);
        if record.version.validate(observed) && current == value {
            Ok(())
        } else {
            self.poison();
            Err(table_fault("fixed-u64 competing initial value mismatched"))
        }
    }

    #[cfg(not(test))]
    fn handle_insert_error(
        &self,
        candidate: &FixedCandidate,
        error: super::MasstreeInsertError,
    ) -> Result<RecordId, AccessError> {
        match error.publication() {
            PublicationDisposition::FailureBeforePublication
            | PublicationDisposition::CandidateProvenUnpublished => {
                self.registry
                    .prove_unpublished(candidate)
                    .inspect_err(|error| self.note_access_error(error))?;
                let mapped = map_masstree_error(error.error());
                self.note_access_error(&mapped);
                Err(mapped)
            }
            PublicationDisposition::CandidateInserted => {
                self.registry
                    .mark_published(candidate)
                    .inspect_err(|error| self.note_access_error(error))?;
                self.poison();
                Err(table_fault(
                    "fixed-u64 native insertion failed after publishing its candidate",
                ))
            }
            PublicationDisposition::Unknown => {
                self.registry
                    .mark_unknown(candidate)
                    .inspect_err(|error| self.note_access_error(error))?;
                self.mark_publication_unknown();
                Err(table_fault(
                    "fixed-u64 native insertion publication is unknown",
                ))
            }
        }
    }

    #[cfg(test)]
    fn handle_insert_error(
        &self,
        _candidate: &FixedCandidate,
        error: super::MemoryInsertError,
    ) -> Result<RecordId, AccessError> {
        match error {}
    }
}

#[repr(C)]
struct FixedRecord {
    // Adjacent fields deliberately match the physical shape of C++
    // versioned_value_struct<u64>. The OCC word is also the physical lock.
    version: AtomicVersion,
    value: AtomicU64,
}

impl FixedRecord {
    fn zeroed() -> Self {
        Self {
            version: AtomicVersion::default(),
            value: AtomicU64::new(0),
        }
    }
}

struct FixedCandidate {
    id: RecordId,
    key_bytes: u64,
}

struct FixedRegistry {
    records: Arc<[FixedRecord]>,
    // Publication metadata is cold after loading and has no effect on the
    // 16-byte hot arena stride.
    states: Arc<[AtomicU8]>,
    lock_segments: Box<[Arc<FixedLockSegment>]>,
    consumed: AtomicU64,
    retained_records: AtomicU64,
    retained_key_bytes: AtomicU64,
    config: TableConfig,
    effective_id_limit: u64,
}

impl FixedRegistry {
    fn new(
        config: TableConfig,
        runtime_id: sto_core::RuntimeId,
        namespace: LockNamespaceId,
        lock_class: LockClass,
    ) -> Result<Self, RegistrationError> {
        let RegistryLayout::EagerContiguous { max_bytes } = config.registry_layout else {
            return Err(CapacityError::BufferLimit.into());
        };
        let addressable = u64::try_from(isize::MAX).unwrap_or(u64::MAX);
        let effective_id_limit = config.max_consumed_record_ids.min(addressable);
        let slot_count = usize::try_from(effective_id_limit)
            .map_err(|_| RegistrationError::from(CapacityError::BufferLimit))?;
        if fixed_registry_accounted_bytes(slot_count)? > max_bytes {
            return Err(CapacityError::BufferLimit.into());
        }

        let mut records = Vec::new();
        records
            .try_reserve_exact(slot_count)
            .map_err(|_| CapacityError::BufferLimit)?;
        records.resize_with(slot_count, FixedRecord::zeroed);
        let records: Arc<[FixedRecord]> = Arc::from(records.into_boxed_slice());

        let mut states = Vec::new();
        states
            .try_reserve_exact(slot_count)
            .map_err(|_| CapacityError::BufferLimit)?;
        states.resize_with(slot_count, || AtomicU8::new(SLOT_UNALLOCATED));
        let states = Arc::from(states.into_boxed_slice());

        let lock_domain = FixedLockDomain {
            runtime_id,
            namespace,
            lock_class,
        };
        let lock_count = fixed_lock_segment_count(slot_count)?;
        let mut lock_segments = Vec::new();
        lock_segments
            .try_reserve_exact(lock_count)
            .map_err(|_| CapacityError::BufferLimit)?;
        for index in 0..lock_count {
            lock_segments.push(Arc::new(FixedLockSegment {
                records: Arc::clone(&records),
                logical_base: index
                    .checked_mul(RECORD_LOCK_SEGMENT_SLOTS)
                    .ok_or(CapacityError::BufferLimit)?,
                lock_domain,
            }));
        }

        Ok(Self {
            records,
            states,
            lock_segments: lock_segments.into_boxed_slice(),
            consumed: AtomicU64::new(0),
            retained_records: AtomicU64::new(0),
            retained_key_bytes: AtomicU64::new(0),
            config,
            effective_id_limit,
        })
    }

    fn usage(&self) -> TableUsage {
        TableUsage {
            retained_records: self.retained_records.load(Ordering::Acquire),
            retained_key_bytes: self.retained_key_bytes.load(Ordering::Acquire),
            consumed_record_ids: self.consumed.load(Ordering::Acquire),
        }
    }

    fn reserve_candidate(&self, key: &[u8], value: u64) -> Result<FixedCandidate, AccessError> {
        reserve_fixed_atomic(&self.retained_records, 1, self.config.max_retained_records)
            .map_err(|()| AccessError::from(CapacityError::BufferLimit))?;
        let key_bytes = key.len() as u64;
        if reserve_fixed_atomic(
            &self.retained_key_bytes,
            key_bytes,
            self.config.max_retained_key_bytes,
        )
        .is_err()
        {
            self.retained_records.fetch_sub(1, Ordering::AcqRel);
            return Err(CapacityError::KeyLimit.into());
        }

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
        let id = RecordId::new(raw_id).expect("checked fixed-u64 ID allocation is nonzero");
        let index = fixed_record_index(id)?;
        let state = self.states.get(index).ok_or(CapacityError::BufferLimit)?;
        state
            .compare_exchange(
                SLOT_UNALLOCATED,
                SLOT_RESERVED,
                Ordering::AcqRel,
                Ordering::Acquire,
            )
            .map_err(|_| table_fault("fixed-u64 registry slot was reused"))?;
        let record = self.records.get(index).ok_or(CapacityError::BufferLimit)?;
        record.value.store(value, Ordering::Relaxed);
        state
            .compare_exchange(
                SLOT_RESERVED,
                SLOT_READY,
                Ordering::Release,
                Ordering::Acquire,
            )
            .map_err(|_| table_fault("fixed-u64 registry initialization failed"))?;
        Ok(FixedCandidate { id, key_bytes })
    }

    fn resolve_published(&self, id: RecordId) -> Result<&FixedRecord, AccessError> {
        let index = fixed_record_index(id)?;
        let state = self
            .states
            .get(index)
            .ok_or_else(|| table_fault("initial-load RecordId is out of range"))?;
        if state.load(Ordering::Acquire) != SLOT_PUBLISHED {
            return Err(table_fault(
                "initial-load directory returned an unpublished RecordId",
            ));
        }
        self.records
            .get(index)
            .ok_or_else(|| table_fault("initial-load fixed-u64 record is missing"))
    }

    #[inline(always)]
    fn resolve(&self, id: RecordId) -> Result<&FixedRecord, AccessError> {
        // FixedAdapter is private and FixedU64Table constructs item keys only
        // from its exclusively owned, append-only directory. Initial-load
        // publication initializes the record before READY and directory
        // insertion, then `finish_initial_load` permanently excludes further
        // publication before any transaction may begin. A directory-derived ID
        // is therefore the capability; reloading the cold sidecar here would
        // add no validity proof. Bounds remain checked for fail-closed safety.
        self.records
            .get(fixed_record_index(id)?)
            .ok_or_else(|| table_fault("directory returned an out-of-range fixed-u64 RecordId"))
    }

    #[inline]
    fn resolve_with_lock(
        &self,
        id: RecordId,
    ) -> Result<(&FixedRecord, &Arc<FixedLockSegment>), AccessError> {
        let index = fixed_record_index(id)?;
        let record = self
            .records
            .get(index)
            .ok_or_else(|| table_fault("directory returned an out-of-range fixed-u64 RecordId"))?;
        let lock = self
            .lock_segments
            .get(index / RECORD_LOCK_SEGMENT_SLOTS)
            .ok_or_else(|| table_fault("fixed-u64 lock segment is missing"))?;
        Ok((record, lock))
    }

    fn mark_published(&self, candidate: &FixedCandidate) -> Result<(), AccessError> {
        self.transition(candidate.id, SLOT_READY, SLOT_PUBLISHED)
    }

    fn prove_unpublished(&self, candidate: &FixedCandidate) -> Result<(), AccessError> {
        self.transition(candidate.id, SLOT_READY, SLOT_PROVEN_UNPUBLISHED)?;
        self.release_retained(candidate.key_bytes);
        Ok(())
    }

    #[cfg(not(test))]
    fn mark_unknown(&self, candidate: &FixedCandidate) -> Result<(), AccessError> {
        self.transition(candidate.id, SLOT_READY, SLOT_PUBLICATION_UNKNOWN)
    }

    fn transition(&self, id: RecordId, from: u8, to: u8) -> Result<(), AccessError> {
        self.states
            .get(fixed_record_index(id)?)
            .ok_or_else(|| table_fault("fixed-u64 publication state is missing"))?
            .compare_exchange(from, to, Ordering::AcqRel, Ordering::Acquire)
            .map(|_| ())
            .map_err(|_| table_fault("illegal fixed-u64 publication transition"))
    }

    fn release_retained(&self, key_bytes: u64) {
        self.retained_key_bytes
            .fetch_sub(key_bytes, Ordering::AcqRel);
        self.retained_records.fetch_sub(1, Ordering::AcqRel);
    }
}

fn fixed_record_index(id: RecordId) -> Result<usize, AccessError> {
    usize::try_from(id.get() - 1).map_err(|_| CapacityError::KeyLimit.into())
}

fn fixed_lock_segment_count(slot_count: usize) -> Result<usize, CapacityError> {
    slot_count
        .checked_add(RECORD_LOCK_SEGMENT_SLOTS - 1)
        .map(|rounded| rounded / RECORD_LOCK_SEGMENT_SLOTS)
        .ok_or(CapacityError::BufferLimit)
}

fn fixed_registry_accounted_bytes(slot_count: usize) -> Result<usize, CapacityError> {
    let arc_header = 2_usize
        .checked_mul(std::mem::size_of::<usize>())
        .ok_or(CapacityError::BufferLimit)?;
    let records = slot_count
        .checked_mul(std::mem::size_of::<FixedRecord>())
        .and_then(|bytes| bytes.checked_add(arc_header))
        .ok_or(CapacityError::BufferLimit)?;
    let states = slot_count
        .checked_mul(std::mem::size_of::<AtomicU8>())
        .and_then(|bytes| bytes.checked_add(arc_header))
        .ok_or(CapacityError::BufferLimit)?;
    let lock_count = fixed_lock_segment_count(slot_count)?;
    let lock_pointers = lock_count
        .checked_mul(std::mem::size_of::<Arc<FixedLockSegment>>())
        .ok_or(CapacityError::BufferLimit)?;
    let lock_allocations = lock_count
        .checked_mul(
            std::mem::size_of::<FixedLockSegment>()
                .checked_add(arc_header)
                .ok_or(CapacityError::BufferLimit)?,
        )
        .ok_or(CapacityError::BufferLimit)?;
    records
        .checked_add(states)
        .and_then(|bytes| bytes.checked_add(lock_pointers))
        .and_then(|bytes| bytes.checked_add(lock_allocations))
        .filter(|bytes| *bytes <= isize::MAX as usize)
        .ok_or(CapacityError::BufferLimit)
}

fn reserve_fixed_atomic(counter: &AtomicU64, amount: u64, limit: u64) -> Result<(), ()> {
    counter
        .fetch_update(Ordering::AcqRel, Ordering::Acquire, |current| {
            current.checked_add(amount).filter(|next| *next <= limit)
        })
        .map(|_| ())
        .map_err(|_| ())
}

#[derive(Clone, Copy)]
struct FixedLockDomain {
    runtime_id: sto_core::RuntimeId,
    namespace: LockNamespaceId,
    lock_class: LockClass,
}

struct FixedLockSegment {
    records: Arc<[FixedRecord]>,
    logical_base: usize,
    lock_domain: FixedLockDomain,
}

impl FixedLockSegment {
    fn identity_record_index(
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
        index
            .checked_sub(self.logical_base)
            .filter(|offset| *offset < RECORD_LOCK_SEGMENT_SLOTS)
            .ok_or_else(|| AdapterFault::new(phase, AdapterFaultKind::LockIdentityMismatch))?;
        if index >= self.records.len() {
            return Err(AdapterFault::new(
                phase,
                AdapterFaultKind::LockIdentityMismatch,
            ));
        }
        Ok((record_id, index))
    }

    fn record_at(&self, index: usize, phase: AdapterPhase) -> Result<&FixedRecord, AdapterFault> {
        self.records
            .get(index)
            .ok_or_else(|| AdapterFault::invariant(phase))
    }
}

struct FixedLockGuard {
    record_id: RecordId,
    index: usize,
    detached: DetachedVersionGuard,
}

impl FixedLockGuard {
    fn before(&self) -> OccVersion {
        self.detached.before()
    }

    fn owner(&self) -> sto_core::OwnerId {
        self.detached.owner()
    }

    fn is_held(&self) -> bool {
        self.detached.is_held()
    }

    fn is_for(&self, id: RecordId, version: &AtomicVersion) -> bool {
        self.record_id == id && self.detached.is_for(version)
    }
}

impl TransactionLock for FixedLockSegment {
    type Guard = FixedLockGuard;

    fn try_acquire(
        &self,
        identity: &LockIdentity,
        cx: &AcquireContext<'_>,
    ) -> Result<Self::Guard, AcquireError> {
        let (record_id, index) = self.identity_record_index(identity, AdapterPhase::Acquire)?;
        let record = self.record_at(index, AdapterPhase::Acquire)?;
        let detached = record.version.try_acquire_detached(cx.owner())?;
        Ok(FixedLockGuard {
            record_id,
            index,
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
            .record_at(guard.index, AdapterPhase::Release)
            .unwrap_or_else(|error| panic!("fixed-u64 release invariant: {error}"));
        if guard.owner() != cx.owner() || !guard.detached.is_for(&record.version) {
            panic!("fixed-u64 lock received a mismatched detached guard");
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
            } => panic!("fixed-u64 committed write has no OCC commit ID"),
            LockDisposition::Indeterminate { occ_commit_id } => guard
                .detached
                .release_indeterminate(&record.version, occ_commit_id)
                .map(Some),
        };
        if let Err(error) = result {
            panic!("fixed-u64 record version release failed: {error}");
        }
    }
}

struct FixedAdapter {
    table: Arc<FixedShared>,
}

#[derive(Clone, Copy)]
struct FixedObservation {
    version: OccVersion,
}

impl OpacityToken for FixedObservation {
    fn observation_order(&self) -> ObservationOrder {
        ObservationOrder::Ordered(self.version)
    }
}

enum FixedPrepared {
    ReadOnly,
    Write { lock_use: LockUse<FixedLockSegment> },
}

static FIXED_PREFLIGHT_FREE_READ: PreflightFreeReadCapability<FixedAdapter> =
    PreflightFreeReadCapability::new_drop_only(validate_fixed_preflight_free_read);
static FIXED_TERMINAL_READ: TerminalReadBatchCapability<FixedAdapter> =
    TerminalReadBatchCapability::new_drop_only(validate_fixed_preflight_free_read);

fn validate_fixed_preflight_free_read(
    adapter: &FixedAdapter,
    key: &RecordId,
    observation: &FixedObservation,
    _cx: &PreflightFreeValidationContext<'_>,
) -> Result<(), CheckError> {
    adapter.validate_read_only(*key, observation)
}

impl FixedAdapter {
    #[inline(always)]
    fn snapshot(&self, id: RecordId) -> Result<(FixedObservation, u64), AccessError> {
        self.snapshot_with_copy(id, |record| record.value.load(Ordering::Relaxed))
    }

    #[inline(always)]
    fn snapshot_with_copy(
        &self,
        id: RecordId,
        mut copy: impl FnMut(&FixedRecord) -> u64,
    ) -> Result<(FixedObservation, u64), AccessError> {
        let record = self.table.resolve_record(id)?;
        loop {
            let observed = record
                .version
                .observe()
                .map_err(|_| AccessError::from(Conflict::LockBusy))?;
            let value = copy(record);
            if record.version.validate(observed) {
                return Ok((FixedObservation { version: observed }, value));
            }
        }
    }

    #[inline(always)]
    fn prepare_access(
        &self,
        id: RecordId,
        entry: &mut Entry<'_, Self>,
    ) -> Result<u64, AccessError> {
        match entry.observation() {
            ObservationRef::Unobserved => {
                let (observation, value) = self.snapshot(id)?;
                *entry.local_mut() = value;
                entry.record_read(observation)?;
                Ok(value)
            }
            ObservationRef::Read(_) | ObservationRef::UpgradedPredicate(_) => Ok(*entry.local()),
            ObservationRef::Predicate(_) => {
                self.table.poison();
                Err(table_fault(
                    "fixed-u64 record carried an impossible predicate",
                ))
            }
        }
    }

    fn validate_read_only(
        &self,
        id: RecordId,
        observation: &FixedObservation,
    ) -> Result<(), CheckError> {
        let record = self.table.resolve_for_phase(id, AdapterPhase::Validation)?;
        if record.version.validate(observation.version) {
            Ok(())
        } else {
            Err(Conflict::ReadValidation.into())
        }
    }

    fn validate_observation(
        &self,
        id: RecordId,
        observation: &FixedObservation,
        prepared: &FixedPrepared,
        cx: &ValidationContext<'_>,
    ) -> Result<(), CheckError> {
        let FixedPrepared::Write { lock_use } = prepared else {
            return self.validate_read_only(id, observation);
        };
        let guard = cx.guard(lock_use)?;
        if !guard.is_held() || guard.record_id != id || guard.owner() != cx.owner() {
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
}

impl TransactionalResource for FixedAdapter {
    type Key = RecordId;
    type Local = u64;
    type Observation = FixedObservation;
    type Predicate = NoPredicate;
    type Intent = ();
    type Prepared = FixedPrepared;

    fn new_local(&self, _key: &Self::Key) -> Result<Self::Local, sto_core::ItemInitError> {
        Ok(0)
    }

    fn preflight_free_read_capability(&self) -> Option<&'static PreflightFreeReadCapability<Self>> {
        Some(&FIXED_PREFLIGHT_FREE_READ)
    }

    fn terminal_read_batch_capability(&self) -> Option<&'static TerminalReadBatchCapability<Self>> {
        Some(&FIXED_TERMINAL_READ)
    }

    fn preflight(
        &self,
        key: &Self::Key,
        item: PreflightItem<'_, Self>,
        cx: &mut PreflightContext<'_>,
    ) -> Result<Self::Prepared, PrepareError> {
        if !matches!(
            item.observation(),
            ObservationRef::Read(_) | ObservationRef::UpgradedPredicate(_)
        ) {
            return Err(AdapterFault::new(
                AdapterPhase::Preflight,
                AdapterFaultKind::LockIdentityMismatch,
            )
            .into());
        }
        if item.intent().is_none() {
            return Ok(FixedPrepared::ReadOnly);
        }
        let (_record, lock_segment) = self
            .table
            .resolve_with_lock_for_phase(*key, AdapterPhase::Preflight)?;
        let identity = LockIdentity::new(
            self.table.runtime_id,
            self.table.namespace,
            self.table.record_lock_class,
            key.get(),
        );
        let lock_use = cx.require_lock(LockRequest::new(identity, Arc::clone(lock_segment)))?;
        Ok(FixedPrepared::Write { lock_use })
    }

    fn revalidate_read(
        &self,
        key: &Self::Key,
        observation: &Self::Observation,
        _cx: &ExecutionCheckContext<'_>,
    ) -> Result<(), CheckError> {
        self.validate_read_only(*key, observation)
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
        let FixedPrepared::Write { lock_use } = prepared else {
            return;
        };
        let commit_id = cx
            .occ_commit_id()
            .expect("fixed-u64 record write has no OCC commit ID");
        let (lock_segment, guard) = cx
            .target_and_guard_mut(lock_use)
            .unwrap_or_else(|error| panic!("fixed-u64 lock invariant: {error}"));
        let record = lock_segment
            .record_at(guard.index, AdapterPhase::Install)
            .unwrap_or_else(|error| panic!("fixed-u64 install invariant: {error}"));
        if !guard.is_held()
            || !guard.is_for(*key, &record.version)
            || commit_id.to_version() <= guard.before()
        {
            panic!("fixed-u64 install received the wrong version guard");
        }
        record.value.store(*item.local_mut(), Ordering::Relaxed);
    }

    fn finish(
        &self,
        _key: &Self::Key,
        _item: FinishItem<'_, Self>,
        _prepared: Option<&mut Self::Prepared>,
        _disposition: FinishDisposition,
        _cx: &mut FinishContext<'_>,
    ) {
    }
}

#[cfg(test)]
mod tests {
    use std::sync::{Arc, Barrier};

    use sto_core::{
        AbortReason, CommitOutcome, InvalidUse, OccCommitId, OwnerId, RuntimeConfig, RuntimeHealth,
    };

    use super::*;
    use crate::MemoryDirectory;

    fn table() -> (Arc<Runtime>, FixedU64Table) {
        let runtime = Runtime::new(
            RuntimeConfig::new()
                .with_max_items_per_transaction(16)
                .with_max_locks_per_transaction(16),
        )
        .unwrap();
        let table = FixedU64Table::with_directory(
            &runtime,
            Directory::Memory(MemoryDirectory::default()),
            TableConfig::new()
                .with_max_retained_records(16)
                .with_max_retained_key_bytes(128)
                .with_max_consumed_record_ids(16)
                .with_registry_layout(RegistryLayout::EagerContiguous {
                    max_bytes: 64 * 1024,
                }),
        )
        .unwrap();
        (runtime, table)
    }

    fn committed(outcome: CommitOutcome) {
        assert!(matches!(outcome, CommitOutcome::Committed(_)));
    }

    #[test]
    fn hot_record_and_batch_layout_are_pinned() {
        assert_eq!(std::mem::size_of::<FixedRecord>(), 16);
        assert_eq!(std::mem::align_of::<FixedRecord>(), 8);
        assert_eq!(std::mem::size_of::<FixedU64Batch>(), 48);
        assert_eq!(
            std::mem::size_of::<Option<<FixedAdapter as TransactionalResource>::Intent>>(),
            1
        );
    }

    #[test]
    fn initial_load_and_mixed_batch_use_exact_occ_publication() {
        let (runtime, table) = table();
        table.shared().insert_initial(None, b"key-a", 7).unwrap();
        table.shared().insert_initial(None, b"key-b", 9).unwrap();
        table.finish_initial_load().unwrap();
        assert_eq!(table.usage().retained_records(), 2);
        let unchanged_id = table.shared().lookup(None, b"key-b").unwrap().unwrap();
        let unchanged_before = table
            .shared()
            .registry
            .resolve(unchanged_id)
            .unwrap()
            .version
            .observe()
            .unwrap();

        let mut worker = runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        let keys = [*b"key-a", *b"key-b"];
        let mut batch = FixedU64Batch::with_capacity(2);
        let mut seen = Vec::new();
        assert_eq!(
            table
                .modify_fixed_inner(&mut transaction, None, &keys, &mut batch, |index, value| {
                    seen.push(value);
                    if index == 0 {
                        FixedU64Mutation::Put(value + 1)
                    } else {
                        FixedU64Mutation::Put(value)
                    }
                },)
                .unwrap(),
            Some(2)
        );
        committed(transaction.commit().unwrap());
        assert_eq!(seen, [7, 9]);
        let unchanged_after = table
            .shared()
            .registry
            .resolve(unchanged_id)
            .unwrap()
            .version
            .observe()
            .unwrap();
        assert_eq!(unchanged_after, unchanged_before);

        let transaction = worker.begin_terminal_read_batch().unwrap();
        let mut values = Vec::new();
        let outcome = table
            .visit_fixed_terminal_inner(transaction, None, &keys, &mut batch, |_index, value| {
                values.push(value)
            })
            .unwrap();
        let TerminalReadVisitOutcome::Ready { transaction, .. } = outcome else {
            panic!("preloaded fixed records must remain present");
        };
        committed(transaction.commit().unwrap());
        assert_eq!(values, [8, 9]);
        assert_eq!(runtime.health(), RuntimeHealth::Healthy);
    }

    #[test]
    fn snapshot_retries_an_unlocked_generation_change_and_rejects_a_held_lock() {
        let (_runtime, table) = table();
        table.shared().insert_initial(None, b"record", 7).unwrap();
        table.finish_initial_load().unwrap();
        let record_id = table.shared().lookup(None, b"record").unwrap().unwrap();
        let adapter = table.record_resource.adapter();
        let mut copies = 0;
        let (observation, value) = adapter
            .snapshot_with_copy(record_id, |record| {
                copies += 1;
                let copied = record.value.load(Ordering::Relaxed);
                if copies == 1 {
                    let mut guard = record
                        .version
                        .try_acquire_detached(OwnerId::new(7).unwrap())
                        .unwrap();
                    record.value.store(8, Ordering::Relaxed);
                    guard
                        .release_commit(&record.version, OccCommitId::new(2).unwrap())
                        .unwrap();
                }
                copied
            })
            .unwrap();
        assert_eq!(copies, 2);
        assert_eq!(value, 8);
        assert_eq!(
            observation.version,
            OccCommitId::new(2).unwrap().to_version()
        );

        let record = table.shared().registry.resolve(record_id).unwrap();
        let mut held = record
            .version
            .try_acquire_detached(OwnerId::new(8).unwrap())
            .unwrap();
        assert!(matches!(
            adapter.snapshot(record_id),
            Err(AccessError::Conflict(Conflict::LockBusy))
        ));
        held.release_abort(&record.version).unwrap();
    }

    #[test]
    fn poisoned_structural_gate_poisoning_is_reflected_in_table_health() {
        let (_runtime, table) = table();
        let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            let _guard = table.shared().structural.try_write().unwrap();
            panic!("poison the fixed-u64 structural gate");
        }));
        assert!(matches!(
            table.finish_initial_load(),
            Err(AccessError::Fault(_))
        ));
        assert_eq!(table.health(), TableHealth::Poisoned);
    }

    #[test]
    fn stale_fixed_writer_aborts_without_installing() {
        let (runtime, table) = table();
        table.shared().insert_initial(None, b"record", 1).unwrap();
        table.finish_initial_load().unwrap();
        let keys = [*b"record"];
        let staged = Arc::new(Barrier::new(2));
        let installed = Arc::new(Barrier::new(2));
        let first_outcome = std::thread::scope(|scope| {
            let first = scope.spawn(|| {
                let mut worker = runtime.attach().unwrap();
                let mut transaction = worker.begin().unwrap();
                let mut batch = FixedU64Batch::with_capacity(1);
                table
                    .modify_fixed_inner(&mut transaction, None, &keys, &mut batch, |_, value| {
                        FixedU64Mutation::Put(value + 10)
                    })
                    .unwrap();
                staged.wait();
                installed.wait();
                transaction.commit().unwrap()
            });
            let second = scope.spawn(|| {
                staged.wait();
                let mut worker = runtime.attach().unwrap();
                let mut transaction = worker.begin().unwrap();
                let mut batch = FixedU64Batch::with_capacity(1);
                table
                    .modify_fixed_inner(&mut transaction, None, &keys, &mut batch, |_, value| {
                        FixedU64Mutation::Put(value + 1)
                    })
                    .unwrap();
                committed(transaction.commit().unwrap());
                installed.wait();
            });
            second.join().unwrap();
            first.join().unwrap()
        });
        assert_eq!(
            first_outcome,
            CommitOutcome::Aborted(AbortReason::Conflict(Conflict::ReadValidation))
        );

        let mut worker = runtime.attach().unwrap();
        let transaction = worker.begin_terminal_read_batch().unwrap();
        let mut batch = FixedU64Batch::with_capacity(1);
        let mut value = None;
        let outcome = table
            .visit_fixed_terminal_inner(transaction, None, &keys, &mut batch, |_, observed| {
                value = Some(observed)
            })
            .unwrap();
        let TerminalReadVisitOutcome::Ready { transaction, .. } = outcome else {
            panic!("record remains present");
        };
        committed(transaction.commit().unwrap());
        assert_eq!(value, Some(2));
    }

    #[test]
    fn loader_seal_is_permanent_and_out_of_range_ids_fail_closed() {
        let (runtime, table) = table();
        assert!(table
            .shared()
            .registry
            .resolve(RecordId::new(17).unwrap())
            .is_err());

        let mut worker = runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();
        let mut batch = FixedU64Batch::with_capacity(1);
        assert_eq!(
            table
                .modify_fixed_inner(&mut transaction, None, &[*b"record"], &mut batch, |_, _| {
                    FixedU64Mutation::Keep
                },)
                .unwrap_err(),
            AccessError::InvalidUse(InvalidUse::IllegalItemState)
        );
        transaction.abort();
        drop(worker);

        table.shared().insert_initial(None, b"record", 1).unwrap();
        table.finish_initial_load().unwrap();
        assert_eq!(
            table
                .shared()
                .insert_initial(None, b"another", 2)
                .unwrap_err(),
            AccessError::InvalidUse(InvalidUse::IllegalItemState)
        );
    }

    #[test]
    fn duplicate_and_missing_mutation_batches_invoke_no_callback() {
        let (runtime, table) = table();
        table.shared().insert_initial(None, b"record", 5).unwrap();
        table.finish_initial_load().unwrap();

        let mut worker = runtime.attach().unwrap();
        let mut batch = FixedU64Batch::with_capacity(2);
        let mut transaction = worker.begin().unwrap();
        let mut calls = 0;
        assert_eq!(
            table
                .modify_fixed_inner(
                    &mut transaction,
                    None,
                    &[*b"record", *b"record"],
                    &mut batch,
                    |_, _| {
                        calls += 1;
                        FixedU64Mutation::Keep
                    },
                )
                .unwrap(),
            None
        );
        assert_eq!(calls, 0);
        committed(transaction.commit().unwrap());

        let mut transaction = worker.begin().unwrap();
        assert_eq!(
            table
                .modify_fixed_inner(&mut transaction, None, &[*b"absent"], &mut batch, |_, _| {
                    calls += 1;
                    FixedU64Mutation::Keep
                },)
                .unwrap(),
            None
        );
        assert_eq!(calls, 0);
        committed(transaction.commit().unwrap());

        let mut transaction = worker.begin().unwrap();
        assert_eq!(
            table
                .modify_fixed_inner(&mut transaction, None, &[*b"record"], &mut batch, |_, _| {
                    FixedU64Mutation::Keep
                },)
                .unwrap(),
            Some(1)
        );
        let mut nonempty_calls = 0;
        assert_eq!(
            table
                .modify_fixed_inner(&mut transaction, None, &[*b"absent"], &mut batch, |_, _| {
                    nonempty_calls += 1;
                    FixedU64Mutation::Keep
                },)
                .unwrap_err(),
            AccessError::InvalidUse(InvalidUse::UniqueBatchRequiresEmptyTransaction)
        );
        assert_eq!(nonempty_calls, 0);
        assert!(transaction.is_doomed());
        transaction.abort();

        let transaction = worker.begin_terminal_read_batch().unwrap();
        let mut observed = Vec::new();
        let outcome = table
            .visit_fixed_terminal_inner(
                transaction,
                None,
                &[*b"record", *b"record"],
                &mut batch,
                |index, value| observed.push((index, value)),
            )
            .unwrap();
        let TerminalReadVisitOutcome::Ready { transaction, .. } = outcome else {
            panic!("duplicate reads of a preloaded key must remain present");
        };
        committed(transaction.commit().unwrap());
        assert_eq!(observed, [(0, 5), (1, 5)]);
    }
}
