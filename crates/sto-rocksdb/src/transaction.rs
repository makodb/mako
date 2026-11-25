use crate::command::{encode_transaction, MutationRef};
use crate::database::{data_key, optional_u64, required_u64, table_key, version_key, GENERATION};
use crate::{core, Database, Error, Position, RequestId, Result, MAX_COMMAND_BYTES, MAX_KEY_BYTES};
use mrx_core::Blobs;
use mrx_rocks::Snapshot;
use std::collections::BTreeMap;
use std::sync::{
    atomic::{AtomicBool, AtomicU64, Ordering},
    Arc,
};
use sto_core::{
    AcquireContext, AcquireError, Active, CapacityError, CheckError, CleanupContext,
    CleanupDisposition, CleanupItem, CommitVersionRequirement, Conflict, ExecutionCheckContext,
    InstallContext, InstallItem, ItemInitError, LocalCommit, LockClass, LockContext,
    LockDisposition, LockIdentity, LockNamespaceId, LockRequest, LockUse, ObservationOrder,
    OpacityToken, PredicateContext, PreflightItem, PrepareError, ReleaseContext, RuntimeId,
    TObject, TransactionLock, ValidationContext, WorkerContext,
};

#[derive(Clone, Debug, Eq, PartialEq, Ord, PartialOrd, Hash)]
pub(crate) enum Key {
    Range,
    Row(u64, Vec<u8>),
}
pub(crate) struct KeyMeta {
    pub generation: AtomicU64,
    held: AtomicBool,
    identity: u64,
}
impl KeyMeta {
    pub fn new(generation: u64, identity: u64) -> Self {
        Self {
            generation: AtomicU64::new(generation),
            held: AtomicBool::new(false),
            identity,
        }
    }
}
pub(crate) struct KeyGuard {
    before: u64,
}
impl TransactionLock for KeyMeta {
    type Guard = KeyGuard;
    fn try_acquire(
        &self,
        _: &LockIdentity,
        _: &AcquireContext<'_>,
    ) -> std::result::Result<KeyGuard, AcquireError> {
        self.held
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .map_err(|_| AcquireError::from(Conflict::LockBusy))?;
        Ok(KeyGuard {
            before: self.generation.load(Ordering::Acquire),
        })
    }
    fn release(&self, _: &mut KeyGuard, _: LockDisposition, _: &ReleaseContext<'_>) {
        self.held.store(false, Ordering::Release);
    }
}
const EXCLUSIVE: u64 = 1 << 63;
pub(crate) struct RangeState {
    pub generation: AtomicU64,
    reservations: AtomicU64,
}
impl RangeState {
    pub fn new(generation: u64) -> Self {
        Self {
            generation: AtomicU64::new(generation),
            reservations: AtomicU64::new(0),
        }
    }
}
pub(crate) struct RangeLock {
    state: Arc<RangeState>,
    exclusive: bool,
}
impl TransactionLock for RangeLock {
    type Guard = ();
    fn try_acquire(
        &self,
        _: &LockIdentity,
        _: &AcquireContext<'_>,
    ) -> std::result::Result<(), AcquireError> {
        if self.exclusive {
            self.state
                .reservations
                .compare_exchange(0, EXCLUSIVE, Ordering::AcqRel, Ordering::Acquire)
                .map_err(|_| AcquireError::from(Conflict::LockBusy))?;
        } else {
            // One reservation per owning STO worker; OwnerId's checked 16-bit
            // domain bounds this count far below EXCLUSIVE, including failed
            // contenders while a scanner holds the high bit. Fetch-add avoids
            // spurious point-writer conflicts under concurrent admission.
            let before = self.state.reservations.fetch_add(1, Ordering::AcqRel);
            if before & EXCLUSIVE != 0 {
                self.state.reservations.fetch_sub(1, Ordering::Release);
                return Err(Conflict::LockBusy.into());
            }
            if self
                .state
                .generation
                .load(Ordering::Acquire)
                .checked_add(before + 1)
                .is_none()
            {
                self.state.reservations.fetch_sub(1, Ordering::Release);
                return Err(CapacityError::VersionExhausted.into());
            }
        }
        Ok(())
    }
    fn release(&self, _: &mut (), _: LockDisposition, _: &ReleaseContext<'_>) {
        self.state.reservations.fetch_sub(
            if self.exclusive { EXCLUSIVE } else { 1 },
            Ordering::Release,
        );
    }
}
pub(crate) struct Adapter {
    pub namespace: LockNamespaceId,
    pub runtime_id: RuntimeId,
    pub lock_class: LockClass,
    pub range: Arc<RangeState>,
    pub epoch: Arc<AtomicU64>,
    pub writers: Arc<RangeLock>,
    pub scanner: Arc<RangeLock>,
}
impl Adapter {
    pub(crate) fn range_locks(range: &Arc<RangeState>) -> (Arc<RangeLock>, Arc<RangeLock>) {
        (
            Arc::new(RangeLock {
                state: Arc::clone(range),
                exclusive: false,
            }),
            Arc::new(RangeLock {
                state: Arc::clone(range),
                exclusive: true,
            }),
        )
    }
    fn identity(&self, key: u64) -> LockIdentity {
        LockIdentity::new(self.runtime_id, self.namespace, self.lock_class, key)
    }
}
pub(crate) struct Local {
    snapshot: Option<Arc<Snapshot>>,
    meta: Option<Arc<KeyMeta>>,
    value: Option<Vec<u8>>,
    version: u64,
    epoch: u64,
    scan: bool,
}
pub(crate) struct Observation {
    version: u64,
    epoch: u64,
    meta: Option<Arc<KeyMeta>>,
}
impl OpacityToken for Observation {
    fn observation_order(&self) -> ObservationOrder {
        ObservationOrder::Unordered
    }
}
pub(crate) enum Intent {
    Row {
        value: Option<Vec<u8>>,
        generation: u64,
    },
    WriteRange,
}
pub(crate) enum Held {
    None,
    Key(LockUse<KeyMeta>),
    Range(LockUse<RangeLock>),
}
impl TObject for Adapter {
    type Key = Key;
    type Local = Local;
    type Observation = Observation;
    type Predicate = Observation;
    type Intent = Intent;
    type Prepared = Held;
    fn commit_version_requirement(&self) -> CommitVersionRequirement {
        CommitVersionRequirement::AdapterLocal
    }
    fn new_local(&self, _: &Key) -> std::result::Result<Local, ItemInitError> {
        Ok(Local {
            snapshot: None,
            meta: None,
            value: None,
            version: 0,
            epoch: 0,
            scan: false,
        })
    }
    fn lock(
        &self,
        key: &Key,
        mut item: PreflightItem<'_, Self>,
        cx: &mut LockContext<'_>,
    ) -> std::result::Result<Held, PrepareError> {
        if item.local().epoch != self.epoch.load(Ordering::Acquire) {
            return Err(Conflict::ReadValidation.into());
        }
        match key {
            Key::Range => {
                if item.intent().is_some()
                    && self.range.generation.load(Ordering::Acquire) == u64::MAX
                {
                    return Err(CapacityError::VersionExhausted.into());
                }
                if item.local().scan {
                    Ok(Held::Range(cx.try_lock(LockRequest::new(
                        self.identity(1),
                        Arc::clone(&self.scanner),
                    ))?))
                } else if item.intent().is_some() {
                    Ok(Held::Range(cx.try_lock(LockRequest::new(
                        self.identity(2),
                        Arc::clone(&self.writers),
                    ))?))
                } else {
                    Ok(Held::None)
                }
            }
            Key::Row(_, _) if item.intent().is_some() => {
                let meta = item
                    .local()
                    .meta
                    .as_ref()
                    .expect("initialized row metadata");
                let lock = cx.try_lock(LockRequest::new(
                    self.identity(meta.identity),
                    Arc::clone(meta),
                ))?;
                let next = item
                    .local()
                    .version
                    .checked_add(1)
                    .ok_or(CapacityError::VersionExhausted)?;
                if let Some(Intent::Row { generation, .. }) = item.intent_mut() {
                    *generation = next;
                }
                Ok(Held::Key(lock))
            }
            _ => Ok(Held::None),
        }
    }
    fn revalidate_read(
        &self,
        _: &Key,
        observed: &Observation,
        _: &ExecutionCheckContext<'_>,
    ) -> std::result::Result<(), CheckError> {
        self.validate_unlocked(observed)
    }
    fn revalidate_predicate(
        &self,
        _: &Key,
        observed: &Observation,
        _: &ExecutionCheckContext<'_>,
    ) -> std::result::Result<ObservationOrder, CheckError> {
        self.validate_unlocked(observed)?;
        Ok(ObservationOrder::Unordered)
    }
    fn check_predicate(
        &self,
        _: &Key,
        observed: &Observation,
        held: &Held,
        cx: &PredicateContext<'_>,
    ) -> std::result::Result<Observation, CheckError> {
        if let Held::Range(token) = held {
            cx.guard(token)?;
        }
        if observed.epoch != self.epoch.load(Ordering::Acquire)
            || self.range.generation.load(Ordering::Acquire) != observed.version
        {
            return Err(Conflict::PredicateValidation.into());
        }
        Ok(Observation {
            version: observed.version,
            epoch: observed.epoch,
            meta: None,
        })
    }
    fn check(
        &self,
        _: &Key,
        observed: &Observation,
        held: &Held,
        cx: &ValidationContext<'_>,
    ) -> std::result::Result<(), CheckError> {
        if observed.epoch != self.epoch.load(Ordering::Acquire) {
            return Err(Conflict::ReadValidation.into());
        }
        match (&observed.meta, held) {
            (Some(meta), Held::Key(token)) => {
                let guard = cx.guard(token)?;
                if guard.before != observed.version
                    || meta.generation.load(Ordering::Acquire) != observed.version
                {
                    return Err(Conflict::ReadValidation.into());
                }
                Ok(())
            }
            (None, Held::Range(token)) => {
                cx.guard(token)?;
                if self.range.generation.load(Ordering::Acquire) != observed.version {
                    return Err(Conflict::PredicateValidation.into());
                }
                Ok(())
            }
            _ => self.validate_unlocked(observed),
        }
    }
    fn install(
        &self,
        _: &Key,
        mut item: InstallItem<'_, Self>,
        _: &mut Held,
        _: &mut InstallContext<'_>,
    ) {
        if let Intent::Row { generation, .. } = item.intent() {
            let generation = *generation;
            // No storage I/O: the whole native batch has already succeeded.
            // Publication remains monotone even if a later replica apply ran.
            if let Some(meta) = &item.local_mut().meta {
                meta.generation.fetch_max(generation, Ordering::Release);
            }
        }
    }
    fn cleanup(
        &self,
        _: &Key,
        mut item: CleanupItem<'_, Self>,
        _: Option<&mut Held>,
        _: CleanupDisposition,
        _: &mut CleanupContext<'_>,
    ) {
        item.take_remaining_intent();
        item.local_mut().snapshot = None;
        item.local_mut().value = None;
        item.local_mut().meta = None;
    }
}
impl Adapter {
    fn validate_unlocked(&self, observed: &Observation) -> std::result::Result<(), CheckError> {
        let valid = observed.epoch == self.epoch.load(Ordering::Acquire)
            && if let Some(meta) = &observed.meta {
                !meta.held.load(Ordering::Acquire)
                    && meta.generation.load(Ordering::Acquire) == observed.version
                    && !meta.held.load(Ordering::Acquire)
            } else {
                self.range.reservations.load(Ordering::Acquire) == 0
                    && self.range.generation.load(Ordering::Acquire) == observed.version
            };
        if valid {
            Ok(())
        } else {
            Err(Conflict::ReadValidation.into())
        }
    }
}

pub struct Worker {
    pub(crate) db: Arc<Database>,
    pub(crate) core: WorkerContext,
}
impl Worker {
    pub fn begin(&mut self) -> Result<Transaction<'_>> {
        let (snapshot, position, epoch) = self.db.snapshot()?;
        let data_generation = required_u64(snapshot.get(GENERATION)?, "snapshot generation")?;
        let txn = self.core.begin().map_err(core)?;
        Ok(Transaction {
            db: Arc::clone(&self.db),
            core: txn,
            snapshot,
            position,
            epoch,
            data_generation,
            writes: false,
            failed: false,
        })
    }
}
pub struct Transaction<'worker> {
    db: Arc<Database>,
    core: sto_core::Transaction<'worker, Active>,
    snapshot: Arc<Snapshot>,
    position: Position,
    epoch: u64,
    data_generation: u64,
    writes: bool,
    failed: bool,
}
impl<'worker> Transaction<'worker> {
    pub fn position(&self) -> Position {
        self.position
    }
    fn ensure_row(&mut self, table: u64, key: &[u8]) -> Result<Key> {
        let result = self.load_row(table, key);
        if result.is_err() {
            self.failed = true;
        }
        result
    }
    fn load_row(&mut self, table: u64, key: &[u8]) -> Result<Key> {
        if self.failed {
            return Err(Error::InvalidState("poisoned transaction"));
        }
        if key.len() > MAX_KEY_BYTES {
            return Err(Error::TooLarge);
        }
        let logical = Key::Row(table, key.to_vec());
        let loaded = self
            .core
            .with_item(&self.db.resource, logical.clone(), |item| {
                Ok(item.local().snapshot.is_some())
            })
            .map_err(core)?;
        if loaded {
            return Ok(logical);
        }
        if self.snapshot.get(&table_key(table))?.is_none() {
            return Err(Error::UnknownTable(table));
        }
        let value = self.snapshot.get(&data_key(table, key))?;
        let version = optional_u64(
            self.snapshot.get(&version_key(table, key))?,
            "snapshot version",
        )?;
        if value.is_some() && version == 0 {
            return Err(Error::Corruption(
                "live row has no durable generation".into(),
            ));
        }
        let meta = self.db.key_meta(&logical)?;
        self.core
            .with_item(&self.db.resource, logical.clone(), |item| {
                item.add_read(Observation {
                    version,
                    epoch: self.epoch,
                    meta: Some(Arc::clone(&meta)),
                })?;
                *item.local_mut() = Local {
                    snapshot: Some(Arc::clone(&self.snapshot)),
                    meta: Some(meta),
                    value,
                    version,
                    epoch: self.epoch,
                    scan: false,
                };
                Ok(())
            })
            .map_err(core)?;
        Ok(logical)
    }
    fn range(&mut self, scan: bool, write: bool) -> Result<()> {
        let generation = self.data_generation;
        self.core
            .with_item(&self.db.resource, Key::Range, |item| {
                if item.local().snapshot.is_none() {
                    *item.local_mut() = Local {
                        snapshot: Some(Arc::clone(&self.snapshot)),
                        meta: None,
                        value: None,
                        version: generation,
                        epoch: self.epoch,
                        scan: false,
                    };
                }
                if scan && !item.local().scan {
                    item.set_predicate(Observation {
                        version: generation,
                        epoch: self.epoch,
                        meta: None,
                    })?;
                    item.local_mut().scan = true;
                }
                if write {
                    item.add_write(Intent::WriteRange)?;
                }
                Ok(())
            })
            .map_err(core)
    }
    pub fn get(&mut self, table: u64, key: &[u8]) -> Result<Option<Vec<u8>>> {
        let logical = self.ensure_row(table, key)?;
        self.core
            .with_item(&self.db.resource, logical, |item| {
                Ok(match item.intent() {
                    Some(Intent::Row { value, .. }) => value.clone(),
                    _ => item.local().value.clone(),
                })
            })
            .map_err(core)
    }
    /// Logical time comes from the same snapshot as the STO observation.
    pub fn distributed_time(&mut self, table: u64, key: &[u8]) -> Result<u32> {
        self.db.ensure_distributed()?;
        self.ensure_row(table, key)?;
        crate::distributed::decode_time(
            self.snapshot
                .get(&crate::distributed::time_key(table, key))?,
        )
    }
    /// Conservative predicate dependency includes tombstones and omitted rows.
    pub fn distributed_range_dependency(
        &mut self,
        table: u64,
        start: &[u8],
        end: Option<&[u8]>,
    ) -> Result<u32> {
        self.db.ensure_distributed()?;
        self.range(true, false)?;
        let start = crate::distributed::time_key(table, start);
        let end = end
            .map(|key| crate::distributed::time_key(table, key))
            .unwrap_or_else(|| {
                if table == u64::MAX {
                    vec![0, 10]
                } else {
                    crate::distributed::time_key(table + 1, b"")
                }
            });
        let mut dependency = 0;
        let mut failure = None;
        self.snapshot
            .scan(&start, Some(&end), false, usize::MAX, &mut |_, value| {
                match crate::distributed::decode_time(Some(value.to_vec())) {
                    Ok(time) => dependency = dependency.max(time / 10),
                    Err(error) => {
                        failure = Some(error);
                        return Ok(false);
                    }
                }
                Ok(true)
            })?;
        if let Some(error) = failure {
            self.failed = true;
            return Err(error);
        }
        Ok(dependency)
    }
    pub fn exists(&mut self, table: u64, key: &[u8]) -> Result<bool> {
        let logical = self.ensure_row(table, key)?;
        self.core
            .with_item(&self.db.resource, logical, |item| {
                Ok(match item.intent() {
                    Some(Intent::Row { value, .. }) => value.is_some(),
                    _ => item.local().value.is_some(),
                })
            })
            .map_err(core)
    }
    pub fn put(&mut self, table: u64, key: &[u8], value: &[u8]) -> Result<()> {
        if value.len() > MAX_COMMAND_BYTES {
            self.failed = true;
            return Err(Error::TooLarge);
        }
        self.write(table, key, Some(value.to_vec()))
    }
    pub fn delete(&mut self, table: u64, key: &[u8]) -> Result<()> {
        self.write(table, key, None)
    }
    pub fn insert(&mut self, table: u64, key: &[u8], value: &[u8]) -> Result<()> {
        if self.exists(table, key)? {
            self.failed = true;
            return Err(Error::AlreadyExists);
        }
        self.put(table, key, value)
    }
    fn write(&mut self, table: u64, key: &[u8], value: Option<Vec<u8>>) -> Result<()> {
        let logical = self.ensure_row(table, key)?;
        if let Err(error) = self.range(false, true) {
            self.failed = true;
            return Err(error);
        }
        self.core
            .with_item(&self.db.resource, logical, |item| {
                item.add_write(Intent::Row {
                    value,
                    generation: 0,
                })
            })
            .map_err(core)?;
        self.writes = true;
        Ok(())
    }
    pub fn scan(
        &mut self,
        table: u64,
        start: &[u8],
        end: Option<&[u8]>,
        limit: usize,
    ) -> Result<Vec<(Vec<u8>, Vec<u8>)>> {
        self.scan_impl(table, start, end, limit, false)
    }
    pub fn reverse_scan(
        &mut self,
        table: u64,
        start: &[u8],
        end: Option<&[u8]>,
        limit: usize,
    ) -> Result<Vec<(Vec<u8>, Vec<u8>)>> {
        self.scan_impl(table, start, end, limit, true)
    }
    fn scan_impl(
        &mut self,
        table: u64,
        start: &[u8],
        end: Option<&[u8]>,
        limit: usize,
        reverse: bool,
    ) -> Result<Vec<(Vec<u8>, Vec<u8>)>> {
        let result = self.scan_inner(table, start, end, limit, reverse);
        if result.is_err() {
            self.failed = true;
        }
        result
    }
    fn scan_inner(
        &mut self,
        table: u64,
        start: &[u8],
        end: Option<&[u8]>,
        limit: usize,
        reverse: bool,
    ) -> Result<Vec<(Vec<u8>, Vec<u8>)>> {
        if self.failed {
            return Err(Error::InvalidState("poisoned transaction"));
        }
        if start.len() > MAX_KEY_BYTES || end.is_some_and(|end| end.len() > MAX_KEY_BYTES) {
            return Err(Error::TooLarge);
        }
        if self.snapshot.get(&table_key(table))?.is_none() {
            return Err(Error::UnknownTable(table));
        }
        self.range(true, false)?;
        if limit == 0 || end.is_some_and(|end| end <= start) {
            return Ok(Vec::new());
        }
        // This is a disposable merge index of core-owned intents, never another
        // transaction write set. Reads and mutations remain solely in STO.
        let mut staged = BTreeMap::new();
        self.core
            .visit_items(&self.db.resource, |item| {
                if let (Key::Row(item_table, key), Some(Intent::Row { value, .. })) =
                    (item.key, item.intent)
                {
                    if *item_table == table
                        && key.as_slice() >= start
                        && end.is_none_or(|end| key.as_slice() < end)
                    {
                        staged.insert(key.clone(), value.clone());
                    }
                }
            })
            .map_err(core)?;
        let from = data_key(table, start);
        let to = end.map(|end| data_key(table, end)).unwrap_or_else(|| {
            if table == u64::MAX {
                vec![2]
            } else {
                data_key(table + 1, b"")
            }
        });
        let mut rows = BTreeMap::new();
        let mut bytes = 0usize;
        let mut too_large = false;
        self.snapshot
            .scan(&from, Some(&to), reverse, usize::MAX, &mut |key, value| {
                let key = &key[9..];
                if !staged.contains_key(key) {
                    bytes = bytes.saturating_add(key.len()).saturating_add(value.len());
                    if bytes > MAX_COMMAND_BYTES {
                        too_large = true;
                        return Ok(false);
                    }
                    rows.insert(key.to_vec(), value.to_vec());
                }
                // At most limit unstaged rows are needed; staged values can only
                // displace these, while tombstones have already been filtered.
                Ok(rows.len() < limit)
            })?;
        if too_large {
            return Err(Error::TooLarge);
        }
        for (key, value) in staged {
            if let Some(value) = value {
                rows.insert(key, value);
            }
        }
        let rows: Vec<_> = if reverse {
            rows.into_iter().rev().take(limit).collect()
        } else {
            rows.into_iter().take(limit).collect()
        };
        if rows
            .iter()
            .try_fold(0usize, |bytes, (key, value)| {
                bytes.checked_add(key.len())?.checked_add(value.len())
            })
            .is_none_or(|bytes| bytes > MAX_COMMAND_BYTES)
        {
            return Err(Error::TooLarge);
        }
        Ok(rows)
    }
    pub fn rollback(self) {
        self.core.abort();
    }
    pub fn prepare(self, request: RequestId) -> Result<Prepared<'worker>> {
        if self.failed {
            return Err(Error::InvalidState("poisoned transaction"));
        }
        self.db.ensure_serving()?;
        let mut retained = self.core.into_local_commit();
        retained.lock_writes().map_err(commit_error)?;
        Ok(Prepared {
            db: self.db,
            core: retained,
            request,
            position: self.position,
            read_only: !self.writes,
            bytes: None,
            command_hash: None,
            submitted: false,
            certified: false,
        })
    }
}
pub struct Prepared<'worker> {
    db: Arc<Database>,
    core: LocalCommit<'worker>,
    request: RequestId,
    position: Position,
    read_only: bool,
    bytes: Option<Vec<u8>>,
    submitted: bool,
    certified: bool,
    command_hash: Option<[u8; 32]>,
}
impl Prepared<'_> {
    pub fn is_read_only(&self) -> bool {
        self.read_only
    }
    /// The service holds its short certification/proposal gate across this call
    /// and local Raft admission, but never across quorum or application waiting.
    pub fn certify(&mut self) -> Result<()> {
        self.db.ensure_serving()?;
        self.core.certify().map_err(commit_error)?;
        if !self.read_only {
            let mut mutations = Vec::new();
            self.core
                .visit_sealed(&self.db.resource, |item| {
                    if let (Key::Row(table, key), Some(Intent::Row { value, generation })) =
                        (item.key, item.intent)
                    {
                        mutations.push(MutationRef {
                            table: *table,
                            key,
                            value: value.as_deref(),
                            generation: *generation,
                            was_present: item.local.value.is_some(),
                        });
                    }
                })
                .map_err(core)?;
            mutations.sort_unstable_by(|a, b| (a.table, a.key).cmp(&(b.table, b.key)));
            let bytes =
                encode_transaction(self.db.group, self.request, mutations.iter().copied(), None)?;
            self.command_hash = Some(crate::command::digest(&bytes));
            self.bytes = Some(bytes);
        }
        self.certified = true;
        Ok(())
    }
    pub fn bytes(&self) -> Result<&[u8]> {
        self.bytes
            .as_deref()
            .ok_or(Error::InvalidState("command unavailable"))
    }
    pub fn take_bytes(&mut self) -> Result<Vec<u8>> {
        self.bytes
            .take()
            .ok_or(Error::InvalidState("command unavailable"))
    }
    pub fn mark_submitted(&mut self) -> Result<()> {
        self.core.mark_submitted().map_err(core)?;
        self.submitted = true;
        Ok(())
    }
    pub fn mark_unknown(&mut self) -> Result<()> {
        self.core.mark_unknown().map_err(core)
    }
    pub fn mark_committed(&mut self) -> Result<()> {
        self.core.mark_committed().map_err(core)
    }
    pub fn mark_apply_failed(&mut self) -> Result<()> {
        self.db.fence();
        self.core.mark_apply_failed().map_err(core)
    }
    pub fn reject(&mut self) -> Result<()> {
        self.core.reject().map_err(core)?;
        self.submitted = false;
        Ok(())
    }
    pub fn complete_read_only(&mut self) -> Result<Position> {
        if !self.read_only || !self.certified {
            return Err(Error::InvalidState("not certified read-only"));
        }
        self.core.complete_read_only().map_err(core)?;
        Ok(self.position)
    }
    /// Finish a certified distributed write after the caller atomically applied
    /// the authoritative decision. This checks installation, never recertifies.
    pub fn complete_distributed(&mut self, time_term: u32) -> Result<()> {
        self.db.ensure_distributed()?;
        if !self.certified || self.read_only {
            return Err(Error::InvalidState("not a certified distributed write"));
        }
        let result = (|| {
            let _gate = crate::mutex(&self.db.apply_gate);
            let rocks = self.db.rocks();
            let mut failure = None;
            self.core
                .visit_sealed(&self.db.resource, |item| {
                    if let (Key::Row(table, key), Some(Intent::Row { value, generation })) =
                        (item.key, item.intent)
                    {
                        let check = (|| {
                            if rocks.get(&data_key(*table, key))? != *value
                                || crate::distributed::decode_time(
                                    rocks.get(&crate::distributed::time_key(*table, key))?,
                                )? != time_term
                                || optional_u64(
                                    rocks.get(&version_key(*table, key))?,
                                    "installed generation",
                                )? != *generation
                            {
                                return Err(Error::Corruption(
                                    "distributed installation mismatch".into(),
                                ));
                            }
                            Ok(())
                        })();
                        if let Err(error) = check {
                            failure = Some(error);
                        }
                    }
                })
                .map_err(core)?;
            if let Some(error) = failure {
                return Err(error);
            }
            self.core.mark_submitted().map_err(core)?;
            self.submitted = true;
            self.core.mark_committed().map_err(core)?;
            self.core.complete_applied().map_err(core)?;
            self.submitted = false;
            Ok(())
        })();
        if result.is_err() {
            self.db.fence();
        }
        result
    }
    pub fn complete_applied(&mut self) -> Result<Position> {
        let hash = self
            .command_hash
            .ok_or(Error::InvalidState("missing decided command"))?;
        let outcome = self
            .db
            .matching_outcome(self.request, hash)?
            .ok_or(Error::InvalidState("request not applied"))?;
        self.core.complete_applied().map_err(core)?;
        self.submitted = false;
        Ok(outcome.position)
    }
}
impl Drop for Prepared<'_> {
    fn drop(&mut self) {
        if self.submitted {
            self.db.fence();
        }
    }
}

fn commit_error(error: sto_core::LocalCommitError) -> Error {
    match error {
        sto_core::LocalCommitError::Aborted(sto_core::AbortReason::Conflict(_)) => Error::Conflict,
        sto_core::LocalCommitError::Aborted(sto_core::AbortReason::Capacity(
            CapacityError::VersionExhausted,
        )) => Error::VersionExhausted,
        _ => core(error),
    }
}
