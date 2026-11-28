#![deny(unsafe_code)]
//! STO callbacks over native-owned Masstree byte rows.
//!
//! C++ owns committed buffers, histories, lock words and publication. Rust owns
//! transaction-local native handles and STO observations. There is no Rust row
//! registry, value mirror, or alternative committed-value representation.
use masstree::{rows, Error as NativeError, NativeStatus, Runtime as NativeRuntime, Worker};
pub use masstree::{KeyBound as ScanBound, ScanDirection};
use std::{fmt, ops::Deref, sync::Arc};
use sto_core::clock::distributed::{DistributedTerm, DistributedTimeTerm, DistributedTimestamp};
use sto_core::*;

#[derive(Clone, Debug)]
pub struct Value(rows::Value);
impl Value {
    #[inline]
    pub fn try_from_slice(bytes: &[u8]) -> Result<Self, AccessError> {
        rows::Value::new(Some(bytes)).map(Self).map_err(access)
    }
}
impl From<&[u8]> for Value {
    fn from(v: &[u8]) -> Self {
        Self::try_from_slice(v).expect("native value allocation")
    }
}
impl From<Vec<u8>> for Value {
    fn from(v: Vec<u8>) -> Self {
        Self::from(v.as_slice())
    }
}
impl AsRef<[u8]> for Value {
    fn as_ref(&self) -> &[u8] {
        self.0.bytes().expect("live native value")
    }
}
impl Deref for Value {
    type Target = [u8];
    fn deref(&self) -> &[u8] {
        self.as_ref()
    }
}
impl PartialEq for Value {
    fn eq(&self, v: &Self) -> bool {
        self.as_ref() == v.as_ref()
    }
}
impl Eq for Value {}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TableConfig {
    max_retained_records: u64,
    max_retained_key_bytes: u64,
    scan_chunk_records: usize,
    scan_max_key_arena_bytes: usize,
    max_scan_chunks: usize,
    max_scan_physical_records: usize,
    record_local_versions: bool,
    distributed_history: bool,
}
impl Default for TableConfig {
    fn default() -> Self {
        Self::new()
    }
}
impl TableConfig {
    pub const fn new() -> Self {
        Self {
            max_retained_records: 1_000_000,
            max_retained_key_bytes: 1 << 30,
            scan_chunk_records: 128,
            scan_max_key_arena_bytes: 64 * 1024,
            max_scan_chunks: 4096,
            max_scan_physical_records: 1_000_000,
            record_local_versions: false,
            distributed_history: false,
        }
    }
    pub const fn with_max_retained_records(mut self, n: u64) -> Self {
        self.max_retained_records = n;
        self
    }
    pub const fn with_max_retained_key_bytes(mut self, n: u64) -> Self {
        self.max_retained_key_bytes = n;
        self
    }
    pub const fn with_scan_chunk_records(mut self, n: usize) -> Self {
        self.scan_chunk_records = n;
        self
    }
    pub const fn with_scan_max_key_arena_bytes(mut self, n: usize) -> Self {
        self.scan_max_key_arena_bytes = n;
        self
    }
    pub const fn with_max_scan_chunks(mut self, n: usize) -> Self {
        self.max_scan_chunks = n;
        self
    }
    pub const fn with_max_scan_physical_records(mut self, n: usize) -> Self {
        self.max_scan_physical_records = n;
        self
    }
    pub const fn with_record_local_versions(mut self, v: bool) -> Self {
        self.record_local_versions = v;
        self
    }
    pub const fn with_distributed_history(mut self, v: bool) -> Self {
        self.distributed_history = v;
        self
    }
}
#[derive(Debug)]
pub enum TableCreateError {
    Directory(NativeError),
    Registration(RegistrationError),
}
impl fmt::Display for TableCreateError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{self:?}")
    }
}
impl std::error::Error for TableCreateError {}
impl From<RegistrationError> for TableCreateError {
    fn from(e: RegistrationError) -> Self {
        Self::Registration(e)
    }
}
impl From<NativeError> for TableCreateError {
    fn from(e: NativeError) -> Self {
        Self::Directory(e)
    }
}
#[derive(Clone)]
pub struct Table {
    resource: RegisteredResource<Adapter>,
    config: TableConfig,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum InsertOutcome {
    Inserted,
    AlreadyPresent(Value),
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ScanRequest<'a> {
    direction: ScanDirection,
    lower: ScanBound<'a>,
    upper: ScanBound<'a>,
    limit: usize,
}
impl<'a> ScanRequest<'a> {
    pub const fn new(direction: ScanDirection, limit: usize) -> Self {
        Self {
            direction,
            lower: ScanBound::Unbounded,
            upper: ScanBound::Unbounded,
            limit,
        }
    }
    pub const fn with_lower(mut self, b: ScanBound<'a>) -> Self {
        self.lower = b;
        self
    }
    pub const fn with_upper(mut self, b: ScanBound<'a>) -> Self {
        self.upper = b;
        self
    }
    pub const fn direction(self) -> ScanDirection {
        self.direction
    }
    pub const fn lower(self) -> ScanBound<'a> {
        self.lower
    }
    pub const fn upper(self) -> ScanBound<'a> {
        self.upper
    }
    pub const fn limit(self) -> usize {
        self.limit
    }
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ScanRecord {
    key: Vec<u8>,
    value: Value,
}
impl ScanRecord {
    pub fn key(&self) -> &[u8] {
        &self.key
    }
    pub fn value(&self) -> &[u8] {
        self.value.as_ref()
    }
    pub fn value_snapshot(&self) -> &Value {
        &self.value
    }
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ReadVisibility {
    pub current_term: DistributedTerm,
    pub stable_through: [Option<u32>; 10],
    pub failed_shard_through: Option<u32>,
}
impl ReadVisibility {
    pub const fn latest(current_term: DistributedTerm) -> Self {
        Self {
            current_term,
            stable_through: [None; 10],
            failed_shard_through: None,
        }
    }
    fn native(self) -> rows::Visibility {
        rows::Visibility {
            term: u32::from(self.current_term.get()),
            stable: self.stable_through.map(|v| v.unwrap_or(u32::MAX)),
            failed: self.failed_shard_through.unwrap_or(u32::MAX),
        }
    }
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct VersionedRead {
    pub value: Option<Value>,
    pub metadata: Option<DistributedTimeTerm>,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct VersionedScanRecord {
    pub key: Vec<u8>,
    pub value: Value,
    pub metadata: Option<DistributedTimeTerm>,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct VersionedScan {
    pub rows: Vec<VersionedScanRecord>,
    pub dependency: Option<DistributedTimestamp>,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CheckpointRecord {
    pub key: Vec<u8>,
    pub value: Option<Value>,
    pub metadata: Option<DistributedTimeTerm>,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CheckpointPage {
    pub rows: Vec<CheckpointRecord>,
    pub end: bool,
}
/// An immutable native buffer reference collected for a journal prepare.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct WriteSnapshot {
    pub key: Vec<u8>,
    pub value: Option<Value>,
}
#[inline]
fn metadata(raw: u32) -> Option<DistributedTimeTerm> {
    (raw != 0).then(|| {
        DistributedTimeTerm::new(
            DistributedTimestamp::new(raw / 10).expect("native timestamp"),
            DistributedTerm::new(raw % 10).expect("native term"),
        )
    })
}
#[inline]
fn selected(snapshot: rows::Snapshot) -> VersionedRead {
    VersionedRead {
        value: snapshot
            .value
            .bytes()
            .is_some()
            .then_some(Value(snapshot.value)),
        metadata: metadata(snapshot.time_term),
    }
}
fn access(e: NativeError) -> AccessError {
    match e {
        NativeError::WrongThread => InvalidUse::WrongThread.into(),
        NativeError::WrongRuntime => InvalidUse::WrongRuntime.into(),
        NativeError::DuplicateWorker => InvalidUse::WorkerBusy.into(),
        NativeError::KeyTooLarge { .. } | NativeError::Native(NativeStatus::KeyTooLarge) => {
            CapacityError::KeyLimit.into()
        }
        NativeError::AllocationLimit { .. }
        | NativeError::Native(NativeStatus::OutOfMemory | NativeStatus::BufferTooSmall) => {
            CapacityError::BufferLimit.into()
        }
        NativeError::Native(NativeStatus::ThreadLimit) => CapacityError::WorkerLimit.into(),
        NativeError::Native(NativeStatus::Busy | NativeStatus::ActiveGuards) => {
            Conflict::LockBusy.into()
        }
        NativeError::Native(NativeStatus::StructureSealed) => {
            Unsupported::Capability("Masstree directory structure is sealed").into()
        }
        NativeError::Native(NativeStatus::WrongThread | NativeStatus::NotAttached) => {
            InvalidUse::WrongThread.into()
        }
        NativeError::Native(NativeStatus::WrongRuntime) => InvalidUse::WrongRuntime.into(),
        NativeError::Native(NativeStatus::Poisoned) => AccessError::Poisoned(PoisonInfo::new(
            FailurePhase::Execution,
            "native Masstree row storage is poisoned",
        )),
        _ => AdapterFault::new(AdapterPhase::Execute, AdapterFaultKind::InvariantViolation).into(),
    }
}

// The native word and shared core must agree before accepting any owner/version.
const _: () = assert!(OccVersion::MAX_VALUE == u64::MAX >> 16 && OwnerId::MAX_VALUE == 65534);
fn prepare_error(error: NativeError) -> PrepareError {
    match error {
        NativeError::Native(NativeStatus::OutOfMemory) => CapacityError::BufferLimit.into(),
        _ => fault(AdapterPhase::Preflight).into(),
    }
}
fn fault(phase: AdapterPhase) -> AdapterFault {
    AdapterFault::new(phase, AdapterFaultKind::LockIdentityMismatch)
}
#[derive(Clone, Copy, Debug, Eq, PartialEq, Ord, PartialOrd)]
enum Key {
    Directory,
    Leaf(rows::LeafToken),
    Row(rows::RowToken),
}
impl std::hash::Hash for Key {
    #[inline]
    fn hash<H: std::hash::Hasher>(&self, h: &mut H) {
        // The STO item index checks full equality, including token incarnation
        // and key variant. One native address supplies its advisory hash.
        match self {
            Self::Directory => h.write_u64(0),
            Self::Row(token) => std::hash::Hash::hash(token, h),
            Self::Leaf(token) => std::hash::Hash::hash(token, h),
        }
    }
}

struct Adapter {
    rows: rows::Rows,
    runtime: RuntimeId,
    namespace: LockNamespaceId,
    local_versions: bool,
}
#[derive(Clone, Copy)]
struct Observation {
    version: u64,
    ordered: bool,
}
impl OpacityToken for Observation {
    fn observation_order(&self) -> ObservationOrder {
        if self.ordered {
            ObservationOrder::Ordered(OccVersion::new(self.version).expect("native OCC version"))
        } else {
            ObservationOrder::Unordered
        }
    }
}
struct Intent {
    value: rows::Value,
    source: Option<DistributedTimeTerm>,
}
struct Local {
    lock: std::cell::OnceCell<Arc<RowLock>>,
    direct: Option<(rows::Write, u64)>,
}
struct Prepared {
    lock: Option<LockUse<RowLock>>,
    write: Option<rows::Write>,
    next: u64,
}
struct RowLock(rows::Row);
impl TransactionLock for RowLock {
    type Guard = rows::Guard;
    fn try_acquire(
        &self,
        _: &LockIdentity,
        cx: &AcquireContext<'_>,
    ) -> Result<Self::Guard, AcquireError> {
        self.0.lock(cx.owner().get()).map_err(|e| match e {
            NativeError::Native(NativeStatus::Busy) => AcquireError::from(Conflict::LockBusy),
            _ => AcquireError::from(fault(AdapterPhase::Preflight)),
        })
    }
    fn release(&self, g: &mut Self::Guard, d: LockDisposition, _: &ReleaseContext<'_>) {
        let mode = match d {
            LockDisposition::Aborted => 0,
            LockDisposition::Committed { .. } => 1,
            LockDisposition::Indeterminate { .. } => 2,
        };
        g.release(mode).expect("native row release invariant");
    }
}
impl Table {
    pub fn new(
        runtime: &Arc<Runtime>,
        native: &NativeRuntime,
        worker: &Worker,
        config: TableConfig,
    ) -> Result<Self, TableCreateError> {
        Self::with_budget(
            runtime,
            native,
            worker,
            config,
            &rows::Budget::new(u64::MAX)?,
        )
    }
    pub fn with_budget(
        runtime: &Arc<Runtime>,
        native: &NativeRuntime,
        worker: &Worker,
        config: TableConfig,
        budget: &rows::Budget,
    ) -> Result<Self, TableCreateError> {
        if config.scan_chunk_records == 0
            || config.scan_max_key_arena_bytes == 0
            || config.max_scan_chunks == 0
        {
            return Err(TableCreateError::Directory(NativeError::Native(
                NativeStatus::Invalid,
            )));
        }
        let object = runtime.register_object()?;
        let namespace = LockNamespaceId::new(object.object_id().get()).expect("object ID");
        let rows = rows::Rows::with_budget(
            native,
            worker,
            config.max_retained_records,
            config.max_retained_key_bytes,
            budget,
        )?;
        let resource = object.register_resource(
            ResourceClass::new(1).unwrap(),
            Adapter {
                rows,
                runtime: object.runtime_id(),
                namespace,
                local_versions: config.record_local_versions,
            },
        )?;
        Ok(Self { resource, config })
    }
    /// Frozen redo references collected from STO's authoritative write set.
    /// The buffers remain native-owned; byte copies occur at journal encoding.
    pub fn intents(
        &self,
        txn: &Transaction<'_, Active>,
    ) -> Result<Vec<WriteSnapshot>, AccessError> {
        let mut writes = Vec::new();
        txn.visit_items(&self.resource, |item| {
            if let (Key::Row(row), Some(intent)) = (item.key, item.intent) {
                writes.push(WriteSnapshot {
                    key: self
                        .resource
                        .adapter()
                        .rows
                        .borrow(*row)
                        .expect("registered native row")
                        .key()
                        .to_vec(),
                    value: intent
                        .value
                        .bytes()
                        .is_some()
                        .then(|| Value(intent.value.clone())),
                });
            }
        })?;
        Ok(writes)
    }
    pub fn object_id(&self) -> ObjectId {
        self.resource.object_id()
    }
    pub fn len(&self) -> u64 {
        self.resource.adapter().rows.len()
    }
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
    pub fn seal_directory_structure(&self) -> Result<(), AccessError> {
        self.resource.adapter().rows.seal().map_err(access)
    }
    fn observe_leaf(
        &self,
        txn: &mut Transaction<'_, Active>,
        observed: rows::LeafObservation,
    ) -> Result<(), AccessError> {
        txn.with_item(
            &self.resource,
            Key::Leaf(observed.leaf.token()),
            |entry| match entry.observation() {
                ObservationRef::Unobserved => entry.add_read(Observation {
                    version: observed.version,
                    ordered: false,
                }),
                // Keep the first leaf generation even if it is now stale.
                // Serializable access may continue; final certification must
                // reject it. Refreshing it here would hide a foreign phantom.
                ObservationRef::Read(_) => Ok(()),
                _ => Err(Conflict::ReadValidation.into()),
            },
        )
    }
    fn lookup(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
    ) -> Result<Option<rows::RowRef<'_>>, AccessError> {
        let lookup = txn.with_item_session(&self.resource, |_| {
            self.resource
                .adapter()
                .rows
                .lookup(worker, key)
                .map_err(access)
        })?;
        if let Some(observed) = lookup.observed {
            self.observe_leaf(txn, observed)?;
        }
        Ok(lookup.row)
    }
    fn resolve(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
    ) -> Result<rows::RowRef<'_>, AccessError> {
        let (row, change) = txn.with_item_session(&self.resource, |_| {
            self.resource
                .adapter()
                .rows
                .resolve_observed(worker, key)
                .map_err(access)
        })?;
        self.observe_insertion(txn, change)?;
        Ok(row)
    }
    fn observe_insertion(
        &self,
        txn: &mut Transaction<'_, Active>,
        change: Option<rows::LeafChange>,
    ) -> Result<(), AccessError> {
        if let Some(change) = change {
            // MassTrans::updateNodeVersion: advance only the observation that
            // exactly preceded our insertion. A foreign change must still fail.
            let observed = txn.with_item(
                &self.resource,
                Key::Leaf(change.before.leaf.token()),
                |entry| match entry.observation() {
                    ObservationRef::Unobserved => Ok(false),
                    ObservationRef::Read(old) if old.version == change.before.version => {
                        entry.replace_read(Observation {
                            version: change.after,
                            ordered: false,
                        })?;
                        Ok(true)
                    }
                    // A foreign change invalidated this predicate. Preserve
                    // it unchanged so a later own insertion cannot bless it.
                    ObservationRef::Read(_) => Ok(false),
                    _ => Err(Conflict::ReadValidation.into()),
                },
            )?;
            if observed {
                for leaf in change.created {
                    self.observe_leaf(txn, leaf)?;
                }
            }
        }
        Ok(())
    }
    fn with_mutation_row<R>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        operation: impl FnOnce(
            &rows::RowRef<'_>,
            &mut TransProxy<'_, Adapter>,
        ) -> Result<R, AccessError>,
    ) -> Result<R, AccessError> {
        // Keep split metadata in the rare insertion closure. Returning that
        // aggregate through core on every existing-key write duplicates moves
        // of data that the normal path never needs.
        let mut insertion = None;
        let result = txn.with_resolved_item(
            &self.resource,
            || {
                Ok(
                    match self
                        .resource
                        .adapter()
                        .rows
                        .locate(worker, key)
                        .map_err(access)?
                    {
                        rows::Located::Row(row) => Some((Key::Row(row), row)),
                        rows::Located::Missing(_, _) => None,
                    },
                )
            },
            || {
                let (row, change) = self
                    .resource
                    .adapter()
                    .rows
                    .resolve_observed(worker, key)
                    .map_err(access)?;
                insertion = change;
                let token = row.token();
                Ok((Key::Row(token), token))
            },
            |entry, token| {
                let row = self.resource.adapter().rows.borrow(token).map_err(access)?;
                operation(&row, entry)
            },
        )?;
        self.observe_insertion(txn, insertion)?;
        Ok(result)
    }

    fn read_row(
        &self,
        txn: &mut Transaction<'_, Active>,
        row: rows::RowRef<'_>,
        visibility: Option<ReadVisibility>,
    ) -> Result<VersionedRead, AccessError> {
        txn.with_item(&self.resource, Key::Row(row.token()), |entry| {
            self.resource.adapter().read(&row, entry, visibility)
        })
    }
    fn with_read_key<R>(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        operation: impl FnOnce(
            &mut TransProxy<'_, Adapter>,
            Option<rows::RowRef<'_>>,
            u64,
        ) -> Result<R, AccessError>,
    ) -> Result<R, AccessError> {
        txn.with_resolved_item(
            &self.resource,
            || {
                Ok(Some(
                    match self
                        .resource
                        .adapter()
                        .rows
                        .locate(worker, key)
                        .map_err(access)?
                    {
                        rows::Located::Row(row) => (Key::Row(row), (Some(row), 0)),
                        rows::Located::Missing(leaf, version) => (Key::Leaf(leaf), (None, version)),
                    },
                ))
            },
            || Err(fault(AdapterPhase::Execute).into()),
            |entry, (token, version)| {
                let row = token
                    .map(|token| self.resource.adapter().rows.borrow(token).map_err(access))
                    .transpose()?;
                operation(entry, row, version)
            },
        )
    }
    fn read_key(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        visibility: Option<ReadVisibility>,
    ) -> Result<VersionedRead, AccessError> {
        self.with_read_key(txn, worker, key, |entry, row, version| {
            if let Some(row) = row {
                return self.resource.adapter().read(&row, entry, visibility);
            }
            match entry.observation() {
                ObservationRef::Unobserved => entry.add_read(Observation {
                    version,
                    ordered: false,
                })?,
                // The first absence predicate remains authoritative at commit.
                ObservationRef::Read(_) => {}
                _ => return Err(Conflict::ReadValidation.into()),
            }
            Ok(VersionedRead {
                value: None,
                metadata: None,
            })
        })
    }
    #[inline]
    pub fn get(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
    ) -> Result<Option<Value>, AccessError> {
        self.read_key(txn, worker, key, None).map(|read| read.value)
    }
    /// Copies a point value into reusable caller storage. Missing values clear
    /// the buffer. This has the same read-your-writes and certification contract
    /// as get; it avoids pinning an owned native snapshot when a copy suffices.
    pub fn get_into(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        buffer: &mut Vec<u8>,
    ) -> Result<bool, AccessError> {
        self.with_read_key(txn, worker, key, |entry, row, leaf_version| {
            buffer.clear();
            if let Some(intent) = entry.intent() {
                return match intent.value.bytes() {
                    None => Ok(false),
                    Some(bytes) => {
                        buffer
                            .try_reserve(bytes.len())
                            .map_err(|_| CapacityError::BufferLimit)?;
                        buffer.extend_from_slice(bytes);
                        Ok(true)
                    }
                };
            }
            let (present, version) = if let Some(row) = &row {
                row.read_into(buffer).map_err(access)?
            } else {
                (false, leaf_version)
            };
            match entry.observation() {
                ObservationRef::Unobserved => entry.add_read(Observation {
                    version,
                    ordered: row.is_some() && !self.resource.adapter().local_versions,
                })?,
                ObservationRef::Read(old) if old.version == version => {}
                ObservationRef::Read(_) if row.is_none() => {}
                _ => return Err(Conflict::ReadValidation.into()),
            }
            Ok(present)
        })
    }
    #[inline]
    pub fn get_versioned(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        visibility: ReadVisibility,
    ) -> Result<VersionedRead, AccessError> {
        self.read_key(txn, worker, key, Some(visibility))
    }
    pub fn put(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        value: &[u8],
    ) -> Result<Option<Value>, AccessError> {
        self.with_mutation_row(txn, worker, key, |row, entry| {
            let previous = self.resource.adapter().read(row, entry, None)?.value;
            entry.add_write(Intent {
                value: Value::try_from_slice(value)?.0,
                source: None,
            })?;
            Ok(previous)
        })
    }
    /// Stages a value without fetching a previous value for the caller.
    /// An earlier read remains authoritative and is certified at commit.
    #[inline]
    pub fn set(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        value: &[u8],
    ) -> Result<(), AccessError> {
        self.with_mutation_row(txn, worker, key, |row, entry| {
            if matches!(entry.observation(), ObservationRef::Unobserved) {
                self.resource.adapter().read(row, entry, None)?;
            }
            entry.add_write(Intent {
                value: Value::try_from_slice(value)?.0,
                source: None,
            })
        })
    }
    pub fn insert(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        value: &[u8],
    ) -> Result<InsertOutcome, AccessError> {
        self.with_mutation_row(txn, worker, key, |row, entry| {
            if let Some(old) = self.resource.adapter().read(row, entry, None)?.value {
                return Ok(InsertOutcome::AlreadyPresent(old));
            }
            entry.add_write(Intent {
                value: Value::try_from_slice(value)?.0,
                source: None,
            })?;
            Ok(InsertOutcome::Inserted)
        })
    }
    pub fn remove(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
    ) -> Result<Option<Value>, AccessError> {
        self.remove_inner(txn, worker, key, false)
    }
    pub fn stage_tombstone(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
    ) -> Result<Option<Value>, AccessError> {
        self.remove_inner(txn, worker, key, true)
    }
    fn remove_inner(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        force: bool,
    ) -> Result<Option<Value>, AccessError> {
        let row = if force {
            self.resolve(txn, worker, key)?
        } else if let Some(row) = self.lookup(txn, worker, key)? {
            row
        } else {
            return Ok(None);
        };
        txn.with_item(&self.resource, Key::Row(row.token()), |entry| {
            let old = self.resource.adapter().read(&row, entry, None)?.value;
            if force || old.is_some() {
                entry.add_write(Intent {
                    value: rows::Value::new(None).map_err(access)?,
                    source: None,
                })?;
            }
            Ok(old)
        })
    }
    pub fn stage_replay(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        key: &[u8],
        value: Option<Value>,
        source: DistributedTimeTerm,
    ) -> Result<bool, AccessError> {
        if !self.config.distributed_history {
            return Err(fault(AdapterPhase::Execute).into());
        }
        let row = self.resolve(txn, worker, key)?;
        txn.with_item(&self.resource, Key::Row(row.token()), |entry| {
            let old = self.resource.adapter().read(&row, entry, None)?;
            if old
                .metadata
                .is_some_and(|m| source.term() <= m.term() && source.timestamp() <= m.timestamp())
            {
                return Ok(false);
            }
            let value = match value {
                Some(v) => v.0,
                None => rows::Value::new(None).map_err(access)?,
            };
            entry.add_write(Intent {
                value,
                source: Some(source),
            })?;
            Ok(true)
        })
    }
    pub fn advance_history_retention_floor(&self, floor: DistributedTimestamp) {
        self.resource.adapter().rows.advance_floor(floor.get())
    }
    fn scan_inner(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        request: ScanRequest<'_>,
        visibility: Option<ReadVisibility>,
    ) -> Result<VersionedScan, AccessError> {
        let mut result = VersionedScan {
            rows: Vec::new(),
            dependency: None,
        };
        if request.limit == 0 {
            return Ok(result);
        }
        let mut after: Option<rows::Row> = None;
        let mut walked = 0usize;
        for _ in 0..self.config.max_scan_chunks {
            let mut page = masstree::ScanRequest::new(request.direction)
                .with_lower(request.lower)
                .with_upper(request.upper)
                .with_entry_capacity(self.config.scan_chunk_records)
                .with_key_arena_capacity(self.config.scan_max_key_arena_bytes);
            if let Some(key) = &after {
                page = match request.direction {
                    ScanDirection::Forward => page.with_lower(ScanBound::Excluded(key.key())),
                    ScanDirection::Reverse => page.with_upper(ScanBound::Excluded(key.key())),
                };
            }
            let chunk = self
                .resource
                .adapter()
                .rows
                .scan(worker, page)
                .map_err(access)?;
            if chunk.entries.is_empty() && !chunk.end {
                return Err(CapacityError::BufferLimit.into());
            }
            for observed in chunk.observations {
                self.observe_leaf(txn, observed)?;
            }
            after = chunk.entries.last().cloned();
            for row in chunk.entries {
                walked += 1;
                if walked > self.config.max_scan_physical_records {
                    return Err(CapacityError::BufferLimit.into());
                }
                let value = self.read_row(txn, row.clone(), visibility)?;
                if let Some(m) = value.metadata {
                    result.dependency = Some(
                        result
                            .dependency
                            .map_or(m.timestamp(), |v| v.max(m.timestamp())),
                    );
                }
                if let Some(v) = value.value {
                    result.rows.push(VersionedScanRecord {
                        key: row.key().to_vec(),
                        value: v,
                        metadata: value.metadata,
                    });
                }
                if result.rows.len() == request.limit {
                    return Ok(result);
                }
            }
            if chunk.end {
                return Ok(result);
            }
        }
        Err(CapacityError::BufferLimit.into())
    }
    pub fn scan(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        request: ScanRequest<'_>,
    ) -> Result<Vec<ScanRecord>, AccessError> {
        let result = self.scan_inner(txn, worker, request, None);
        self.finish_scan(txn, result).map(|scan| {
            scan.rows
                .into_iter()
                .map(|r| ScanRecord {
                    key: r.key,
                    value: r.value,
                })
                .collect()
        })
    }
    pub fn scan_versioned(
        &self,
        txn: &mut Transaction<'_, Active>,
        worker: &Worker,
        request: ScanRequest<'_>,
        visibility: ReadVisibility,
    ) -> Result<VersionedScan, AccessError> {
        let result = self.scan_inner(txn, worker, request, Some(visibility));
        self.finish_scan(txn, result)
    }
    fn finish_scan<T>(
        &self,
        txn: &mut Transaction<'_, Active>,
        result: Result<T, AccessError>,
    ) -> Result<T, AccessError> {
        if let Err(error) = result {
            if !txn.is_doomed() {
                let _: Result<(), _> =
                    txn.with_item(&self.resource, Key::Directory, |_| Err(error));
            }
            return Err(error);
        }
        result
    }
    pub fn checkpoint_page(
        &self,
        worker: &Worker,
        after: Option<&[u8]>,
        limit: usize,
    ) -> Result<CheckpointPage, AccessError> {
        if limit == 0 || limit > 4096 {
            return Err(CapacityError::BufferLimit.into());
        }
        let request = masstree::ScanRequest::new(ScanDirection::Forward)
            .with_lower(after.map_or(ScanBound::Unbounded, ScanBound::Excluded))
            .with_entry_capacity(limit)
            .with_key_arena_capacity(self.config.scan_max_key_arena_bytes);
        let chunk = self
            .resource
            .adapter()
            .rows
            .scan(worker, request)
            .map_err(access)?;
        let mut records = Vec::new();
        for row in chunk.entries {
            let key = row.key().to_vec();
            let read = selected(row.read(None, None, true).map_err(access)?);
            records.push(CheckpointRecord {
                key,
                value: read.value,
                metadata: read.metadata,
            });
        }
        if records.is_empty() && !chunk.end {
            return Err(CapacityError::BufferLimit.into());
        }
        Ok(CheckpointPage {
            rows: records,
            end: chunk.end,
        })
    }
}
impl Adapter {
    #[inline]
    fn read(
        &self,
        row: &rows::RowRef<'_>,
        entry: &mut TransProxy<'_, Self>,
        visibility: Option<ReadVisibility>,
    ) -> Result<VersionedRead, AccessError> {
        if let Some(intent) = entry.intent() {
            return Ok(VersionedRead {
                value: intent
                    .value
                    .bytes()
                    .is_some()
                    .then(|| Value(intent.value.clone())),
                metadata: intent.source,
            });
        }
        // Re-read selected history under the native row latch. The covering OCC
        // observation remains the current row generation, even for old values.
        let snapshot = row
            .read(None, visibility.map(ReadVisibility::native), false)
            .map_err(access)?;
        match entry.observation() {
            ObservationRef::Unobserved => entry.add_read(Observation {
                version: snapshot.version,
                ordered: !self.local_versions,
            })?,
            ObservationRef::Read(old) if old.version == snapshot.version => {}
            _ => return Err(Conflict::ReadValidation.into()),
        }
        Ok(selected(snapshot))
    }
    #[inline]
    fn check_native(
        &self,
        key: &Key,
        observation: &Observation,
        owner: Option<u32>,
    ) -> Result<(), CheckError> {
        match key {
            Key::Directory => Err(Conflict::ReadValidation.into()),
            Key::Leaf(leaf) => self
                .rows
                .check_leaf(*leaf, observation.version)
                .map_err(|_| Conflict::ReadValidation.into()),
            Key::Row(row) => self
                .rows
                .borrow(*row)
                .and_then(|row| row.check(observation.version, owner))
                .map_err(|_| Conflict::ReadValidation.into()),
        }
    }
}
// Reuse the core-owned one-lock-per-write plan for homogeneous local work.
// Core retains lock ordering, final certification, irreversible installation
// and quarantine. Retained participants keep the ordinary phased callbacks.
fn direct_prepare(
    adapter: &Adapter,
    key: &Key,
    mut item: PreflightItem<'_, Adapter>,
) -> Result<Option<BorrowedLockToken<rows::RowToken>>, PrepareError> {
    let Some(intent) = item.intent() else {
        return Ok(None);
    };
    let Key::Row(token) = key else {
        return Err(fault(AdapterPhase::Preflight).into());
    };
    let row = adapter
        .rows
        .borrow(*token)
        .map_err(|_| fault(AdapterPhase::Preflight))?;
    let observed = match item.observation() {
        ObservationRef::Read(o) => o.version,
        _ => return Err(fault(AdapterPhase::Preflight).into()),
    };
    let next = OccVersion::new(observed)
        .map_err(|_| fault(AdapterPhase::Preflight))?
        .checked_next()
        .ok_or(CapacityError::VersionExhausted)?
        .get();
    let write = row.prepare(&intent.value).map_err(prepare_error)?;
    item.local_mut().direct = Some((write, next));
    Ok(Some(BorrowedLockToken::new(adapter.runtime, *token)))
}
fn direct_validate(
    adapter: &Adapter,
    key: &Key,
    item: DirectValidationItem<'_, Adapter>,
    lock: Option<DirectLockRef<'_, Adapter>>,
    cx: &DirectValidationContext,
) -> Result<(), CheckError> {
    match item.observation() {
        ObservationRef::Unobserved if item.intent().is_none() => Ok(()),
        ObservationRef::Read(o) => adapter.check_native(key, o, lock.map(|_| cx.owner().get())),
        _ => Err(fault(AdapterPhase::Validation).into()),
    }
}
fn direct_install(
    adapter: &Adapter,
    _: &Key,
    mut item: InstallItem<'_, Adapter>,
    mut lock: DirectLockMut<'_, Adapter>,
    cx: &mut DirectInstallContext,
) {
    let time = item
        .intent()
        .source
        .or(cx.distributed_time_term())
        .map_or(0, DistributedTimeTerm::encode_legacy);
    let (mut write, next) = item
        .local_mut()
        .direct
        .take()
        .expect("prepared native write");
    let version = if adapter.local_versions {
        next
    } else {
        cx.occ_commit_id().expect("ordered row commit").get()
    };
    adapter
        .rows
        .install_ticket(lock.guard_mut(), &mut write, version, time)
        .expect("native publication invariant");
}
// The adapter itself owns the one native table for every token. Core borrows
// that target through release; no per-write Arc<RowLock> allocation is needed.
impl TransactionLock for Adapter {
    type Guard = rows::LockTicket;
    fn try_acquire(
        &self,
        _: &LockIdentity,
        _: &AcquireContext<'_>,
    ) -> Result<Self::Guard, AcquireError> {
        Err(fault(AdapterPhase::Preflight).into())
    }
    fn release(&self, g: &mut Self::Guard, d: LockDisposition, _: &ReleaseContext<'_>) {
        let mode = match d {
            LockDisposition::Aborted => 0,
            LockDisposition::Committed { .. } => 1,
            LockDisposition::Indeterminate { .. } => 2,
        };
        self.rows
            .release_ticket(g, mode)
            .expect("native row release invariant");
    }
}
impl DirectBorrowedLockTarget<Adapter> for Adapter {
    fn direct_borrowed_lock_target(&self) -> &Adapter {
        self
    }
}
// SAFETY: RowToken has private fields and can only originate in a live Rows
// owner. Rows::borrow checks the monotone table incarnation before dereferencing
// it. This adapter never replaces that owner; its address and the row address
// identify the same native lock through release. A guard is returned only after
// native acquisition succeeds. Wrong runtimes/tables fail before acquisition.
#[allow(unsafe_code)]
unsafe impl DirectTokenLock for Adapter {
    type Token = rows::RowToken;
    fn try_acquire_token(
        &self,
        runtime: RuntimeId,
        token: Self::Token,
        cx: &AcquireContext<'_>,
    ) -> Result<Self::Guard, AcquireError> {
        if runtime != self.runtime {
            return Err(fault(AdapterPhase::Preflight).into());
        }
        self.rows
            .lock_ticket(token, cx.owner().get())
            .map_err(|e| match e {
                NativeError::Native(NativeStatus::Busy) => AcquireError::from(Conflict::LockBusy),
                _ => AcquireError::from(fault(AdapterPhase::Preflight)),
            })
    }
}
struct DirectOperations;
impl BorrowedTokenCommitOperations<Adapter, Adapter> for DirectOperations {
    fn prepare(
        &self,
        a: &Adapter,
        key: &Key,
        item: PreflightItem<'_, Adapter>,
    ) -> Result<Option<BorrowedLockToken<rows::RowToken>>, PrepareError> {
        direct_prepare(a, key, item)
    }
    fn validate(
        &self,
        a: &Adapter,
        key: &Key,
        item: DirectValidationItem<'_, Adapter>,
        lock: Option<DirectLockRef<'_, Adapter>>,
        cx: &DirectValidationContext,
    ) -> Result<(), CheckError> {
        direct_validate(a, key, item, lock, cx)
    }
    fn install(
        &self,
        a: &Adapter,
        key: &Key,
        item: InstallItem<'_, Adapter>,
        lock: DirectLockMut<'_, Adapter>,
        cx: &mut DirectInstallContext,
    ) {
        direct_install(a, key, item, lock, cx)
    }
}
static DIRECT_LOCKS: BorrowedInjectiveLockCommitCapability<Adapter, Adapter, DirectOperations> =
    BorrowedInjectiveLockCommitCapability::new(DirectOperations);
// SAFETY: a Table creates exactly one private, non-Clone Adapter/Rows binding.
// Cloning Table preserves that exact registered binding. Core deduplicates its
// complete Key::Row token before planning. Distinct bindings own distinct Rows;
// distinct row tokens within a binding name distinct append-only native rows.
// Leaf/directory items never emit locks. Thus writing item identities map
// injectively to (adapter address, row token), including across tables. Final
// certification still checks every read/write after all write locks are held.
#[allow(unsafe_code)]
static DIRECT: DirectCommitCapability<Adapter> = unsafe {
    DirectCommitCapability::borrowed_injective_token_lock(&DIRECT_LOCKS)
        .with_drop_only_committed_cleanup()
};

impl TObject for Adapter {
    type Key = Key;
    type Local = Local;
    type Observation = Observation;
    type Predicate = NoPredicate;
    type Intent = Intent;
    type Prepared = Prepared;
    fn commit_version_requirement(&self) -> CommitVersionRequirement {
        if self.local_versions {
            CommitVersionRequirement::AdapterLocal
        } else {
            CommitVersionRequirement::Ordered
        }
    }
    fn new_local(&self, _: &Key) -> Result<Local, ItemInitError> {
        Ok(Local {
            lock: std::cell::OnceCell::new(),
            direct: None,
        })
    }
    fn preflight_free_read_capability(&self) -> Option<&'static PreflightFreeReadCapability<Self>> {
        static READ: PreflightFreeReadCapability<Adapter> =
            PreflightFreeReadCapability::new_drop_only(|adapter, key, observed, _| {
                adapter.check_native(key, observed, None)
            });
        Some(&READ)
    }
    fn direct_commit_capability(&self) -> Option<&'static DirectCommitCapability<Self>> {
        Some(&DIRECT)
    }
    fn lock(
        &self,
        key: &Key,
        item: PreflightItem<'_, Self>,
        cx: &mut LockContext<'_>,
    ) -> Result<Prepared, PrepareError> {
        let Some(intent) = item.intent() else {
            return Ok(Prepared {
                lock: None,
                write: None,
                next: 0,
            });
        };
        let Key::Row(row) = key else {
            return Err(fault(AdapterPhase::Preflight).into());
        };
        let row = self
            .rows
            .borrow(*row)
            .map_err(|_| fault(AdapterPhase::Preflight))?;
        let lock = cx.try_lock(LockRequest::new(
            LockIdentity::new(
                self.runtime,
                self.namespace,
                LockClass::new(1).unwrap(),
                row.identity(),
            ),
            item.local()
                .lock
                .get_or_init(|| Arc::new(RowLock(row.clone().into_owned())))
                .clone(),
        ))?;
        let guard = cx.guard(&lock)?;
        let write = guard.prepare(&intent.value).map_err(prepare_error)?;
        let version = match item.observation() {
            ObservationRef::Read(observed) | ObservationRef::UpgradedPredicate(observed) => {
                observed.version
            }
            _ => return Err(fault(AdapterPhase::Preflight).into()),
        };
        let next = OccVersion::new(version)
            .map_err(|_| fault(AdapterPhase::Preflight))?
            .checked_next()
            .ok_or(CapacityError::VersionExhausted)?
            .get();
        Ok(Prepared {
            lock: Some(lock),
            write: Some(write),
            next,
        })
    }
    fn revalidate_read(
        &self,
        key: &Key,
        o: &Observation,
        _: &ExecutionCheckContext<'_>,
    ) -> Result<(), CheckError> {
        self.check_native(key, o, None)
    }
    fn revalidate_predicate(
        &self,
        _: &Key,
        p: &NoPredicate,
        _: &ExecutionCheckContext<'_>,
    ) -> Result<ObservationOrder, CheckError> {
        match *p {}
    }
    fn check_predicate(
        &self,
        _: &Key,
        p: &NoPredicate,
        _: &Prepared,
        _: &PredicateContext<'_>,
    ) -> Result<Observation, CheckError> {
        match *p {}
    }
    fn check(
        &self,
        key: &Key,
        o: &Observation,
        p: &Prepared,
        cx: &ValidationContext<'_>,
    ) -> Result<(), CheckError> {
        if let Some(lock) = &p.lock {
            cx.guard(lock)?;
        }
        self.check_native(key, o, p.lock.as_ref().map(|_| cx.owner().get()))
    }
    fn install(
        &self,
        _: &Key,
        item: InstallItem<'_, Self>,
        p: &mut Prepared,
        cx: &mut InstallContext<'_>,
    ) {
        let version = if self.local_versions {
            p.next
        } else {
            cx.occ_commit_id().expect("ordered row commit").get()
        };
        let time = item
            .intent()
            .source
            .or(cx.distributed_time_term())
            .map_or(0, DistributedTimeTerm::encode_legacy);
        cx.guard_mut(p.lock.as_ref().expect("row lock"))
            .expect("row guard")
            .install(
                p.write.as_mut().expect("preallocated row version"),
                version,
                time,
            )
            .expect("native publication invariant");
    }
    fn cleanup(
        &self,
        _: &Key,
        mut item: CleanupItem<'_, Self>,
        _: Option<&mut Prepared>,
        _: CleanupDisposition,
        _: &mut CleanupContext<'_>,
    ) {
        let _ = item.take_remaining_intent();
    }
}
