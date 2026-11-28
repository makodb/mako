use crate::command::{digest, put_u64, Decoder};
use crate::layout::{Snapshot, Store as RocksBlobs};
use crate::transaction::{Adapter, Key, KeyMeta, RangeState};
use crate::{
    core, mutex, ApplyOutcome, Command, CommandBody, Error, Outcome, OutcomeKind, Position,
    RequestId, Result, Worker,
};
use mako_rocksdb_store::Durability;
use mako_rocksdb_store::{BlobOp, Blobs};
use std::borrow::Cow;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::sync::{
    atomic::{AtomicBool, AtomicU64, Ordering},
    Arc, Mutex, RwLock, Weak,
};
use sto_core::{
    LockClass, LockNamespaceId, RegisteredResource, ResourceClass, Runtime, RuntimeConfig,
};

pub(crate) const FORMAT: &[u8] = &[0, 0];
pub(crate) const APPLIED: &[u8] = &[0, 1];
pub(crate) const GENERATION: &[u8] = &[0, 4];
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Table {
    pub id: u64,
    pub name: String,
}
pub(crate) struct Generation {
    pub name: String,
    pub rocks: Arc<RocksBlobs>,
}
/// Deterministic typed failure at the whole-batch native I/O boundary.
#[cfg(feature = "test-hooks")]
#[derive(Clone, Copy)]
pub enum ApplyFault {
    BeforeBatch = 1,
    AfterBatch = 2,
}
pub struct Database {
    pub(crate) root: PathBuf,
    pub(crate) public: PathBuf,
    pub(crate) group: u64,
    pub(crate) sync_writes: bool,
    pub(crate) distributed: bool,
    pub(crate) generation: RwLock<Generation>,
    pub(crate) retired: Mutex<Vec<Generation>>,
    pub(crate) apply_gate: Mutex<()>,
    pub(crate) epoch: Arc<AtomicU64>,
    pub(crate) fenced: AtomicBool,
    pub(crate) runtime: Arc<Runtime>,
    pub(crate) resource: RegisteredResource<Adapter>,
    pub(crate) metadata: Mutex<BTreeMap<Key, Weak<KeyMeta>>>,
    pub(crate) next_lock: AtomicU64,
    pub(crate) range: Arc<RangeState>,
    #[cfg(feature = "test-hooks")]
    pub(crate) apply_fault: std::sync::atomic::AtomicU8,
    // The logical DB root, not a replaceable native generation, owns the lease.
    _root_lock: std::fs::File,
}
impl Database {
    /// Open a WAL-enabled database with a persistent durability identity.
    /// `false` skips foreground write synchronization and can lose acknowledged
    /// writes on power/machine crash; reopening or installing the opposite mode
    /// is rejected rather than upgrading weak history to durable history.
    pub fn open(path: impl AsRef<Path>, group: u64, sync_writes: bool) -> Result<Arc<Self>> {
        Self::open_mode(path.as_ref(), group, sync_writes, false, None)
    }
    /// Separate format profile for externally decided distributed STO effects.
    pub fn open_distributed(
        path: impl AsRef<Path>,
        group: u64,
        sync_writes: bool,
    ) -> Result<Arc<Self>> {
        Self::open_mode(path.as_ref(), group, sync_writes, true, None)
    }
    /// Open or adopt native families with stable application table identities.
    /// Adoption scans existing rows and initializes metadata without changing
    /// user keys/values. The prior RocksDB writer must be stopped.
    pub fn open_distributed_with_tables(
        path: impl AsRef<Path>,
        group: u64,
        sync_writes: bool,
        tables: &[Table],
    ) -> Result<Arc<Self>> {
        let mut tables = tables.to_vec();
        tables.sort_by_key(|t| t.id);
        let db = Self::open_mode(path.as_ref(), group, sync_writes, true, Some(&tables))?;
        for table in &tables {
            if db.lookup_table(&table.name)?.as_ref() != Some(table) {
                return Err(Error::Corruption("column-family schema mismatch".into()));
            }
        }
        Ok(db)
    }

    fn open_mode(
        path: &Path,
        group: u64,
        sync_writes: bool,
        distributed: bool,
        tables: Option<&[Table]>,
    ) -> Result<Arc<Self>> {
        let crate::directory::Opened {
            root,
            public,
            name,
            rocks,
            lock: root_lock,
        } = crate::directory::open(path, group, sync_writes, distributed, tables)?;
        let expected = format_bytes(group, sync_writes);
        let opposite = format_bytes(group, !sync_writes);
        let format = rocks.get(FORMAT)?.ok_or_else(|| {
            Error::Corruption("selected generation has no format metadata".into())
        })?;
        if rocks.get(crate::distributed::PROFILE)?.as_deref()
            != distributed.then_some(crate::distributed::PROFILE_VALUE)
        {
            return Err(Error::Corruption(
                "storage application profile mismatch".into(),
            ));
        }
        if format == opposite {
            return Err(Error::Corruption(
                "database durability mode mismatch".into(),
            ));
        }
        if format != expected {
            return Err(Error::WrongGroup);
        }
        let position = read_position(&rocks)?;
        if (position.index == 0) != (position.term == 0) {
            return Err(Error::Corruption("invalid initial applied position".into()));
        }
        if position.index != 0 {
            let record = rocks
                .get(&entry_key(position.index))?
                .ok_or_else(|| Error::Corruption("missing applied-prefix receipt".into()))?;
            if record.len() != 40 || record[..8] != position.term.to_be_bytes() {
                return Err(Error::Corruption("applied-prefix receipt mismatch".into()));
            }
        }
        let data_generation = required_u64(rocks.get(GENERATION)?, "data generation")?;
        let runtime = Runtime::new(
            RuntimeConfig::new()
                .with_max_items_per_transaction(crate::MAX_OPERATIONS + 1)
                .with_max_locks_per_transaction(crate::MAX_OPERATIONS + 1),
        )
        .map_err(core)?;
        let object = runtime.register_object().map_err(core)?;
        let range = Arc::new(RangeState::new(data_generation));
        let epoch = Arc::new(AtomicU64::new(1));
        let (writers, scanner) = Adapter::range_locks(&range);
        let resource = object
            .register_resource(
                ResourceClass::new(501).unwrap(),
                Adapter {
                    namespace: LockNamespaceId::new(object.object_id().get()).unwrap(),
                    runtime_id: object.runtime_id(),
                    lock_class: LockClass::new(501).unwrap(),
                    range: Arc::clone(&range),
                    epoch: Arc::clone(&epoch),
                    writers,
                    scanner,
                },
            )
            .map_err(core)?;
        Ok(Arc::new(Self {
            root,
            public,
            group,
            sync_writes,
            distributed,
            generation: RwLock::new(Generation { name, rocks }),
            retired: Mutex::new(Vec::new()),
            apply_gate: Mutex::new(()),
            epoch,
            fenced: AtomicBool::new(false),
            runtime,
            resource,
            metadata: Mutex::new(BTreeMap::new()),
            next_lock: AtomicU64::new(3),
            range,
            #[cfg(feature = "test-hooks")]
            apply_fault: std::sync::atomic::AtomicU8::new(0),
            _root_lock: root_lock,
        }))
    }
    pub fn group(&self) -> u64 {
        self.group
    }
    pub fn attach_worker(self: &Arc<Self>) -> Result<Worker> {
        self.ensure_serving()?;
        Ok(Worker {
            db: Arc::clone(self),
            core: self.runtime.attach().map_err(core)?,
        })
    }
    #[cfg(feature = "test-hooks")]
    pub fn inject_apply_fault(&self, fault: ApplyFault) {
        self.apply_fault.store(fault as u8, Ordering::Release);
    }
    pub fn is_fenced(&self) -> bool {
        self.fenced.load(Ordering::Acquire)
    }
    pub fn fence(&self) {
        self.fenced.store(true, Ordering::Release);
    }
    pub(crate) fn ensure_serving(&self) -> Result<()> {
        if self.is_fenced() {
            Err(Error::Fenced)
        } else {
            Ok(())
        }
    }
    pub(crate) fn rocks(&self) -> Arc<RocksBlobs> {
        Arc::clone(
            &self
                .generation
                .read()
                .unwrap_or_else(|e| e.into_inner())
                .rocks,
        )
    }
    pub(crate) fn snapshot(&self) -> Result<(Arc<Snapshot>, Position, u64)> {
        let _gate = mutex(&self.apply_gate);
        self.ensure_serving()?;
        let snapshot = Arc::new(self.rocks().snapshot()?);
        let position = decode_position(
            &snapshot
                .get(APPLIED)?
                .ok_or_else(|| Error::Corruption("missing applied position".into()))?,
        )?;
        Ok((snapshot, position, self.epoch.load(Ordering::Acquire)))
    }
    pub(crate) fn key_meta(&self, key: &Key) -> Result<Arc<KeyMeta>> {
        let _gate = mutex(&self.apply_gate);
        let mut metadata = mutex(&self.metadata);
        if let Some(meta) = metadata.get(key).and_then(Weak::upgrade) {
            return Ok(meta);
        }
        let Key::Row(table, bytes) = key else {
            return Err(Error::InvalidState("range key metadata"));
        };
        let generation = optional_u64(
            self.rocks().get(&version_key(*table, bytes))?,
            "key generation",
        )?;
        let lock_id = self
            .next_lock
            .fetch_update(Ordering::AcqRel, Ordering::Acquire, |id| id.checked_add(1))
            .map_err(|_| Error::VersionExhausted)?;
        let meta = Arc::new(KeyMeta::new(generation, lock_id));
        // Dead weak handles are not a permanent key directory or value cache.
        if metadata.len() > 4096 && metadata.len().is_power_of_two() {
            metadata.retain(|_, value| value.strong_count() != 0);
        }
        metadata.insert(key.clone(), Arc::downgrade(&meta));
        Ok(meta)
    }
    pub fn applied(&self) -> Result<Position> {
        read_position(&self.rocks())
    }
    /// Exact retained replay identities, including no-ops. Conversion uses this
    /// under the exclusive database lease, never inferring terms from the head.
    pub fn entry_history(&self) -> Result<Vec<crate::distributed::Prefix>> {
        let _gate = mutex(&self.apply_gate);
        self.ensure_serving()?;
        let rocks = self.rocks();
        let head = read_position(&rocks)?;
        let mut history = Vec::new();
        let mut term = 0;
        for index in 1..=head.index {
            let bytes = rocks
                .get(&entry_key(index))?
                .ok_or_else(|| Error::Corruption("missing replay identity".into()))?;
            if bytes.len() != 40 {
                return Err(Error::Corruption("invalid replay identity length".into()));
            }
            let next = u64::from_be_bytes(bytes[..8].try_into().unwrap());
            if next == 0 || next < term || next > head.term {
                return Err(Error::Corruption("invalid replay identity term".into()));
            }
            term = next;
            history.push(crate::distributed::Prefix {
                position: Position { index, term },
                fingerprint: bytes[8..].try_into().unwrap(),
            });
        }
        if term != head.term {
            return Err(Error::Corruption("replay identity head mismatch".into()));
        }
        Ok(history)
    }
    pub fn lookup_table(&self, name: &str) -> Result<Option<Table>> {
        self.ensure_serving()?;
        self.rocks()
            .get(&catalog_key(name))?
            .map(|id| {
                Ok(Table {
                    id: decode_u64(&id, "table id")?,
                    name: name.to_owned(),
                })
            })
            .transpose()
    }
    pub fn list_tables(&self) -> Result<Vec<Table>> {
        self.ensure_serving()?;
        let snapshot = self.rocks().snapshot()?;
        let mut tables = Vec::new();
        let mut failure = None;
        snapshot.scan(
            &[0, 2],
            Some(&[0, 3]),
            false,
            usize::MAX,
            &mut |key, value| {
                match String::from_utf8(key[2..].to_vec())
                    .map_err(|_| Error::Corruption("catalog UTF-8".into()))
                    .and_then(|name| {
                        Ok(Table {
                            id: decode_u64(value, "table id")?,
                            name,
                        })
                    }) {
                    Ok(table) => tables.push(table),
                    Err(error) => {
                        failure = Some(error);
                        return Ok(false);
                    }
                }
                Ok(true)
            },
        )?;
        if let Some(error) = failure {
            return Err(error);
        }
        Ok(tables)
    }
    pub fn count(&self, table: u64) -> Result<u64> {
        self.ensure_serving()?;
        let rocks = self.rocks();
        if rocks.get(&table_key(table))?.is_none() {
            return Err(Error::UnknownTable(table));
        }
        required_u64(rocks.get(&count_key(table))?, "table count")
    }
    pub fn outcome(&self, request: RequestId) -> Result<Option<Outcome>> {
        self.rocks()
            .get(&outcome_key(request))?
            .map(|bytes| decode_outcome(request, &bytes).map(|(_, outcome)| outcome))
            .transpose()
    }
    pub(crate) fn matching_outcome(
        &self,
        request: RequestId,
        expected: [u8; 32],
    ) -> Result<Option<Outcome>> {
        self.rocks()
            .get(&outcome_key(request))?
            .map(|bytes| {
                let (actual, outcome) = decode_outcome(request, &bytes)?;
                if actual != expected {
                    return Err(Error::RequestReuse);
                }
                Ok(outcome)
            })
            .transpose()
    }
    pub fn apply(&self, position: Position, bytes: &[u8]) -> Result<ApplyOutcome> {
        if self.distributed {
            return Err(Error::InvalidState(
                "distributed profile requires external apply",
            ));
        }
        let command = match Command::decode(bytes) {
            Ok(command) => command,
            Err(error) => {
                self.fence();
                return Err(error);
            }
        };
        if command.group != self.group {
            self.fence();
            return Err(Error::WrongGroup);
        }
        self.apply_record(position, Some((&command, bytes)))
    }
    pub fn advance_applied(&self, position: Position) -> Result<ApplyOutcome> {
        if self.distributed {
            return Err(Error::InvalidState(
                "distributed profile requires external apply",
            ));
        }
        self.apply_record(position, None)
    }
    fn apply_record(
        &self,
        position: Position,
        command: Option<(&Command, &[u8])>,
    ) -> Result<ApplyOutcome> {
        let _gate = mutex(&self.apply_gate);
        self.ensure_serving()?;
        let result = self.apply_locked(position, command);
        if result.is_err() {
            self.fence();
        }
        result
    }
    fn apply_locked(
        &self,
        position: Position,
        command: Option<(&Command, &[u8])>,
    ) -> Result<ApplyOutcome> {
        if position.index == 0 || position.term == 0 {
            return Err(Error::InvalidCommand("zero Raft position"));
        }
        let rocks = self.rocks();
        let previous = read_position(&rocks)?;
        let hash = digest(command.map_or(&[][..], |(_, bytes)| bytes));
        let mut entry = position.term.to_be_bytes().to_vec();
        entry.extend_from_slice(&hash);
        if position.index <= previous.index {
            if rocks.get(&entry_key(position.index))?.as_deref() != Some(entry.as_slice()) {
                return Err(Error::Corruption("conflicting replay position".into()));
            }
            let outcome = command
                .map(|(command, _)| self.outcome(command.request))
                .transpose()?
                .flatten();
            return Ok(ApplyOutcome {
                outcome,
                duplicate: true,
                applied: previous,
            });
        }
        let expected = previous
            .index
            .checked_add(1)
            .ok_or(Error::VersionExhausted)?;
        if position.index != expected {
            return Err(Error::AppliedGap {
                expected,
                received: position.index,
            });
        }
        if position.term < previous.term {
            return Err(Error::Corruption("regressing applied term".into()));
        }
        let mut batch = Vec::<(Vec<u8>, Option<Cow<'_, [u8]>>)>::new();
        let mut duplicate = false;
        let mut published = Vec::<(Key, u64)>::new();
        let mut data_generation = None;
        let outcome = if let Some((command, _)) = command {
            if let Some(existing) = rocks.get(&outcome_key(command.request))? {
                let (old_hash, outcome) = decode_outcome(command.request, &existing)?;
                if hash != old_hash {
                    return Err(Error::RequestReuse);
                }
                duplicate = true;
                Some(outcome)
            } else {
                let kind = match &command.body {
                    CommandBody::CreateTable { name } => {
                        let id = if let Some(existing) = rocks.get(&catalog_key(name))? {
                            decode_u64(&existing, "table id")?
                        } else {
                            let id = position.index;
                            if rocks.get(&table_key(id))?.is_some() {
                                return Err(Error::Corruption("table ID collision".into()));
                            }
                            batch.push((catalog_key(name), Some(id.to_be_bytes().to_vec().into())));
                            batch.push((table_key(id), Some(Cow::Borrowed(name.as_bytes()))));
                            batch.push((count_key(id), Some(0u64.to_be_bytes().to_vec().into())));
                            id
                        };
                        OutcomeKind::Table(id)
                    }
                    CommandBody::Transaction {
                        mutations,
                        count_deltas,
                    } => {
                        for mutation in mutations {
                            if rocks.get(&table_key(mutation.table))?.is_none() {
                                return Err(Error::UnknownTable(mutation.table));
                            }
                            let old = optional_u64(
                                rocks.get(&version_key(mutation.table, &mutation.key))?,
                                "key generation",
                            )?;
                            if old.checked_add(1) != Some(mutation.generation) {
                                return Err(Error::Corruption(
                                    "decided key generation does not extend prefix".into(),
                                ));
                            }
                            if old == 0 && mutation.was_present {
                                return Err(Error::Corruption(
                                    "live row has no durable generation".into(),
                                ));
                            }
                            let key = data_key(mutation.table, &mutation.key);
                            if rocks.get(&key)?.is_some() != mutation.was_present {
                                return Err(Error::Corruption(
                                    "decided row presence disagrees with prefix".into(),
                                ));
                            }
                            batch.push((key, mutation.value.as_deref().map(Cow::Borrowed)));
                            batch.push((
                                version_key(mutation.table, &mutation.key),
                                Some(mutation.generation.to_be_bytes().to_vec().into()),
                            ));
                            published.push((
                                Key::Row(mutation.table, mutation.key.clone()),
                                mutation.generation,
                            ));
                        }
                        for (table, delta) in count_deltas {
                            let old = required_u64(rocks.get(&count_key(*table))?, "table count")?;
                            let count = old
                                .checked_add_signed(*delta)
                                .ok_or_else(|| Error::Corruption("table count overflow".into()))?;
                            batch.push((
                                count_key(*table),
                                Some(count.to_be_bytes().to_vec().into()),
                            ));
                        }
                        let generation = required_u64(rocks.get(GENERATION)?, "data generation")?
                            .checked_add(1)
                            .ok_or(Error::VersionExhausted)?;
                        batch.push((
                            GENERATION.to_vec(),
                            Some(generation.to_be_bytes().to_vec().into()),
                        ));
                        data_generation = Some(generation);
                        OutcomeKind::Transaction
                    }
                };
                let outcome = Outcome {
                    request: command.request,
                    position,
                    kind,
                };
                batch.push((
                    outcome_key(command.request),
                    Some(encode_outcome(&outcome, hash).into()),
                ));
                Some(outcome)
            }
        } else {
            None
        };
        batch.push((entry_key(position.index), Some(entry.into())));
        batch.push((APPLIED.to_vec(), Some(position_bytes(position).into())));
        let ops: Vec<_> = batch
            .iter()
            .map(|(key, value)| match value {
                Some(value) => BlobOp::Put { key, val: value },
                None => BlobOp::Delete { key },
            })
            .collect();
        // This is the one fallible whole-transaction storage boundary. An error
        // can be ambiguous: apply_record fences and recovery inspects this batch.
        #[cfg(feature = "test-hooks")]
        let fault = self.apply_fault.swap(0, Ordering::AcqRel);
        #[cfg(feature = "test-hooks")]
        if fault == ApplyFault::BeforeBatch as u8 {
            return Err(Error::Storage("injected before atomic batch".into()));
        }
        rocks.write_batch(&ops)?;
        #[cfg(feature = "test-hooks")]
        if fault == ApplyFault::AfterBatch as u8 {
            return Err(Error::Storage("injected ambiguous atomic batch".into()));
        }
        let metadata = mutex(&self.metadata);
        for (key, generation) in published {
            if let Some(meta) = metadata.get(&key).and_then(Weak::upgrade) {
                meta.generation.store(generation, Ordering::Release);
            }
        }
        if let Some(generation) = data_generation {
            self.range.generation.store(generation, Ordering::Release);
        }
        Ok(ApplyOutcome {
            outcome,
            duplicate,
            applied: position,
        })
    }
}
pub(crate) fn data_key(table: u64, key: &[u8]) -> Vec<u8> {
    let mut bytes = vec![1];
    put_u64(&mut bytes, table);
    bytes.extend_from_slice(key);
    bytes
}
pub(crate) fn version_key(table: u64, key: &[u8]) -> Vec<u8> {
    let mut bytes = vec![0, 6];
    put_u64(&mut bytes, table);
    bytes.extend_from_slice(key);
    bytes
}
pub(crate) fn table_key(table: u64) -> Vec<u8> {
    let mut bytes = vec![0, 3];
    put_u64(&mut bytes, table);
    bytes
}
pub(crate) fn count_key(table: u64) -> Vec<u8> {
    let mut bytes = vec![0, 5];
    put_u64(&mut bytes, table);
    bytes
}
pub(crate) fn catalog_key(name: &str) -> Vec<u8> {
    let mut bytes = vec![0, 2];
    bytes.extend_from_slice(name.as_bytes());
    bytes
}
fn outcome_key(request: RequestId) -> Vec<u8> {
    let mut bytes = vec![0, 7];
    put_u64(&mut bytes, request.client);
    put_u64(&mut bytes, request.sequence);
    bytes
}
pub(crate) fn entry_key(index: u64) -> Vec<u8> {
    let mut bytes = vec![0, 8];
    put_u64(&mut bytes, index);
    bytes
}
pub(crate) fn durability(sync_writes: bool) -> Durability {
    if sync_writes {
        Durability::Sync
    } else {
        Durability::Wal
    }
}
pub(crate) fn format_bytes(group: u64, sync_writes: bool) -> Vec<u8> {
    // Async is a distinct, incompatible durability identity.
    let mut bytes = if sync_writes {
        b"STORDB02".to_vec()
    } else {
        b"STORDBA2".to_vec()
    };
    put_u64(&mut bytes, group);
    bytes
}

pub(crate) fn position_bytes(position: Position) -> Vec<u8> {
    let mut bytes = position.index.to_be_bytes().to_vec();
    put_u64(&mut bytes, position.term);
    bytes
}
pub(crate) fn decode_position(bytes: &[u8]) -> Result<Position> {
    if bytes.len() != 16 {
        return Err(Error::Corruption("position length".into()));
    }
    Ok(Position {
        index: decode_u64(&bytes[..8], "position index")?,
        term: decode_u64(&bytes[8..], "position term")?,
    })
}
pub(crate) fn read_position(rocks: &RocksBlobs) -> Result<Position> {
    decode_position(
        &rocks
            .get(APPLIED)?
            .ok_or_else(|| Error::Corruption("missing applied position".into()))?,
    )
}
pub(crate) fn decode_u64(bytes: &[u8], what: &str) -> Result<u64> {
    Ok(u64::from_be_bytes(bytes.try_into().map_err(|_| {
        Error::Corruption(format!("invalid {what}"))
    })?))
}
pub(crate) fn required_u64(bytes: Option<Vec<u8>>, what: &str) -> Result<u64> {
    decode_u64(
        &bytes.ok_or_else(|| Error::Corruption(format!("missing {what}")))?,
        what,
    )
}
pub(crate) fn optional_u64(bytes: Option<Vec<u8>>, what: &str) -> Result<u64> {
    bytes.map_or(Ok(0), |bytes| {
        let value = decode_u64(&bytes, what)?;
        if value == 0 {
            return Err(Error::Corruption(format!("zero persisted {what}")));
        }
        Ok(value)
    })
}
fn encode_outcome(outcome: &Outcome, hash: [u8; 32]) -> Vec<u8> {
    let mut bytes = hash.to_vec();
    bytes.extend_from_slice(&position_bytes(outcome.position));
    match outcome.kind {
        OutcomeKind::Transaction => bytes.push(0),
        OutcomeKind::Table(table) => {
            bytes.push(1);
            put_u64(&mut bytes, table);
        }
    }
    bytes
}
fn decode_outcome(request: RequestId, bytes: &[u8]) -> Result<([u8; 32], Outcome)> {
    let mut input = Decoder(bytes);
    let hash = input.take(32)?.try_into().unwrap();
    let position = Position {
        index: input.u64()?,
        term: input.u64()?,
    };
    let kind = match input.byte()? {
        0 => OutcomeKind::Transaction,
        1 => OutcomeKind::Table(input.u64()?),
        _ => return Err(Error::Corruption("outcome tag".into())),
    };
    if !input.0.is_empty() {
        return Err(Error::Corruption("outcome trailing bytes".into()));
    }
    Ok((
        hash,
        Outcome {
            request,
            position,
            kind,
        },
    ))
}
