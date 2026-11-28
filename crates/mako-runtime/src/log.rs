//! Shared local and Raft log application. The pluggable Masstree path derives
//! its protocol index and data from RocksDB plus the committed log.
//! A locally installed proposal is identified by owned bytes and is never
//! replayed into its own retained locks. Unknown outcomes fence the shard.
use crate::{
    application::Index,
    migration::Schema,
    redo,
    storage::{engine, CommitLog, CommitPermit, Store},
    wire::{self, Error, ErrorCode, Result},
};
use parking_lot::Mutex;
use raft_host::{Host, Position, ProposalStatus, StateMachine};
use serde::{Deserialize, Serialize};
use std::{
    collections::BTreeMap,
    io::Read,
    path::Path,
    sync::{
        atomic::{AtomicU64, Ordering},
        mpsc, Arc, OnceLock,
    },
    thread::JoinHandle,
    time::Duration,
};
use sto_core::protocol::journal::{self, Entry, Journal, Record, TransactionId};

mod materialization;
pub use materialization::Options as MaterializationOptions;

#[derive(Serialize, Deserialize)]
pub(crate) struct JournalCommand {
    pub(crate) version: u32,
    pub(crate) incarnation: u64,
    pub(crate) sequence: u64,
    pub(crate) record: Record,
}
fn storage(error: impl std::fmt::Display) -> Error {
    Error::new(ErrorCode::Fatal, error.to_string())
}

enum ReplayWork {
    Tables(Vec<u16>, mpsc::SyncSender<Result<()>>),
    Apply(Vec<redo::Row>, mpsc::SyncSender<Result<()>>),
    Bootstrap(
        Arc<dyn mako_storage_api::PersistentApplication>,
        mpsc::SyncSender<Result<()>>,
    ),
}
struct Replay {
    sender: Option<mpsc::SyncSender<ReplayWork>>,
    thread: Option<JoinHandle<()>>,
}
impl Replay {
    fn start(store: &Arc<Store>, schema: Arc<Schema>) -> Result<Self> {
        let weak = Arc::downgrade(store);
        let database = store.database.clone();
        let (send, recv) = mpsc::sync_channel::<ReplayWork>(16);
        let (ready, started) = mpsc::sync_channel(1);
        let thread = std::thread::Builder::new()
            .name("mako-replay".into())
            .spawn(move || {
                let mut worker = match database.worker() {
                    Ok(worker) => worker,
                    Err(error) => {
                        let _ = ready.send(Err(engine(error)));
                        return;
                    }
                };
                let _ = ready.send(Ok(()));
                while let Ok(work) = recv.recv() {
                    let (rows, reply) = match work {
                        ReplayWork::Tables(ids, reply) => {
                            let result = (|| {
                                let store = weak
                                    .upgrade()
                                    .ok_or_else(|| storage("shard store stopped"))?;
                                for id in ids {
                                    store.open(&mut worker, schema.key_for_log(id)?, id)?;
                                }
                                Ok(())
                            })();
                            let _ = reply.send(result);
                            continue;
                        }
                        ReplayWork::Apply(rows, reply) => (rows, reply),
                        ReplayWork::Bootstrap(application, reply) => {
                            let result = (|| {
                                let store = weak
                                    .upgrade()
                                    .ok_or_else(|| storage("shard store stopped"))?;
                                for (_, table) in store.tables() {
                                    application.ensure_tables(&[table.log_id]).map_err(engine)?;
                                    let mut after = None;
                                    loop {
                                        // 128 maximum-size protocol rows fit below
                                        // the native 16 MiB materialization bound.
                                        let page = worker
                                            .checkpoint_page(&table.tree, after.as_deref(), 128)
                                            .map_err(engine)?;
                                        if !page.end && page.rows.is_empty() {
                                            return Err(storage(
                                                "bootstrap export made no progress",
                                            ));
                                        }
                                        let mut rows = Vec::new();
                                        for row in page.rows {
                                            if after.as_ref().is_some_and(|prior| prior >= &row.key)
                                            {
                                                return Err(storage("bootstrap export key order"));
                                            }
                                            after = Some(row.key.clone());
                                            rows.push(redo::Row {
                                                table: table.log_id,
                                                key: row.key,
                                                value: row.value,
                                                time_term: row.time_term,
                                            });
                                        }
                                        application.bootstrap(&rows).map_err(engine)?;
                                        if page.end {
                                            break;
                                        }
                                    }
                                }
                                Ok(())
                            })();
                            let _ = reply.send(result);
                            continue;
                        }
                    };
                    let result = (|| {
                        let store = weak
                            .upgrade()
                            .ok_or_else(|| storage("shard store stopped"))?;
                        for row in rows {
                            let key = schema.key_for_log(row.table)?;
                            let table = store.open(&mut worker, key, row.table)?;
                            if row.time_term == 0 {
                                if let Some(value) = row.value {
                                    let mut transaction = worker.begin().map_err(engine)?;
                                    match transaction.get(&table.tree, &row.key).map_err(engine)? {
                                        None => transaction
                                            .put(&table.tree, &row.key, &value)
                                            .map_err(engine)?,
                                        Some(prior) if prior == value => {}
                                        Some(_) => {
                                            return Err(storage(
                                                "checkpoint bootstrap value changed",
                                            ))
                                        }
                                    }
                                    transaction.commit().map_err(engine)?;
                                }
                            } else {
                                worker
                                    .replay(
                                        &table.tree,
                                        &row.key,
                                        row.value.as_deref(),
                                        row.time_term,
                                    )
                                    .map_err(engine)?;
                            }
                        }
                        Ok(())
                    })();
                    let _ = reply.send(result);
                }
            })
            .map_err(storage)?;
        let mut replay = Self {
            sender: Some(send),
            thread: Some(thread),
        };
        if let Err(error) = started.recv().map_err(storage)? {
            replay.sender.take();
            return Err(error);
        }
        Ok(replay)
    }
    fn tables(&self, ids: Vec<u16>) -> Result<()> {
        if ids.is_empty() {
            return Ok(());
        }
        let (send, receive) = mpsc::sync_channel(1);
        self.sender
            .as_ref()
            .expect("live replay")
            .send(ReplayWork::Tables(ids, send))
            .map_err(storage)?;
        receive.recv().map_err(storage)?
    }
    fn bootstrap(
        &self,
        application: Arc<dyn mako_storage_api::PersistentApplication>,
    ) -> Result<()> {
        let (reply, receive) = mpsc::sync_channel(1);
        self.sender
            .as_ref()
            .expect("live replay")
            .send(ReplayWork::Bootstrap(application, reply))
            .map_err(storage)?;
        receive.recv().map_err(storage)?
    }
    fn apply(&self, rows: Vec<redo::Row>) -> Result<()> {
        let (send, recv) = mpsc::sync_channel(1);
        self.sender
            .as_ref()
            .expect("live replay")
            .send(ReplayWork::Apply(rows, send))
            .map_err(storage)?;
        recv.recv().map_err(storage)?
    }
}
impl Drop for Replay {
    fn drop(&mut self) {
        self.sender.take();
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}
#[derive(Default)]
struct Gate {
    active: usize,
    fault: Option<String>,
    pending: BTreeMap<(u64, u64), Vec<u8>>,
    materialization: materialization::Pending,
}
struct State {
    owner: u32,
    durable_transactions: bool,
    gate: Mutex<Gate>,
    persistent: Arc<dyn mako_storage_api::PersistentApplication>,
    index: Mutex<Index>,
    identity: Vec<u8>,
    accelerated: bool,
    limits: MaterializationOptions,
    replay: Replay,
    schema: Arc<Schema>,
}
impl State {
    fn load_persistent(&self) -> Result<()> {
        let application = &self.persistent;
        let index = Index::from_metadata(
            application.metadata().map_err(engine)?,
            application.position().map_err(engine)?,
            self.owner,
            self.durable_transactions,
        )?;
        let ids = application
            .table_ids()
            .map_err(engine)?
            .into_iter()
            .map(|id| u16::try_from(id).map_err(storage))
            .collect::<Result<Vec<_>>>()?;
        self.replay.tables(ids.clone())?;
        if self.accelerated {
            for table in ids {
                let mut after = None;
                loop {
                    let page = application
                        .page(table, after.as_deref(), 256)
                        .map_err(engine)?;
                    if !page.end && page.rows.is_empty() {
                        return Err(storage("RocksDB recovery made no progress"));
                    }
                    let mut rows = Vec::new();
                    for row in page.rows {
                        if after.as_ref().is_some_and(|prior| prior >= &row.key) {
                            return Err(storage("RocksDB recovery key order"));
                        }
                        after = Some(row.key.clone());
                        rows.push(redo::Row {
                            table,
                            key: row.key,
                            value: row.value,
                            time_term: row.time_term,
                        });
                    }
                    self.replay.apply(rows)?;
                    if page.end {
                        break;
                    }
                }
            }
        }
        *self.index.lock() = index;

        Ok(())
    }
    fn position(&self) -> Result<Position> {
        Ok(self.index.lock().position.into())
    }
    fn transaction(&self, id: TransactionId) -> Result<Option<Entry>> {
        Ok(self.index.lock().transactions.get(&id).cloned())
    }
    fn transactions(&self) -> Result<Vec<Entry>> {
        Ok(self.index.lock().transactions.values().cloned().collect())
    }
    fn gateway_receipt(
        &self,
        request: &wire::GatewayRequest,
    ) -> Result<Option<wire::GatewayResponse>> {
        let index = self.index.lock();
        match index.receipts.get(&(request.client, request.sequence)) {
            Some(receipt) if &receipt.request == request => Ok(Some(receipt.response.clone())),
            Some(_) => Err(storage("gateway request identity reused")),
            None => Ok(None),
        }
    }
    fn ensure(gate: &Gate) -> Result<()> {
        if let Some(error) = &gate.fault {
            Err(Error::new(ErrorCode::Unknown, error.clone()))
        } else {
            Ok(())
        }
    }
}
impl StateMachine for State {
    fn applied_position(&self) -> std::result::Result<Position, String> {
        self.position().map_err(|error| error.to_string())
    }
    fn apply(&self, p: Position, bytes: Option<&[u8]>) -> std::result::Result<(), String> {
        let mut gate = self.gate.lock();
        let result = (|| {
            Self::ensure(&gate)?;
            let Some(bytes) = bytes else {
                let mut index = self.index.lock();
                let change = index.no_op(p.into())?;
                self.persist(&mut gate, index.position, &change)?;
                index.publish(change);
                return Ok(());
            };
            let command: JournalCommand = wire::decode(bytes)?;
            if command.version != 2
                || command.incarnation == 0
                || command.sequence == 0
                || command.record.transaction().durable != self.durable_transactions
            {
                return Err(storage("invalid transaction log record"));
            }
            if let Record::Prepare { rows, .. } = &command.record {
                for row in rows {
                    self.schema.key_for_log(row.table)?;
                }
            }
            let local = gate
                .pending
                .get(&(command.incarnation, command.sequence))
                .is_some_and(|pending| pending == bytes);
            if !local && gate.active != 0 {
                return Err(storage(
                    "leadership changed with active prepared transactions; recovery required",
                ));
            }
            let mut index = self.index.lock();
            let change = index.journal(
                p.into(),
                bytes,
                self.owner,
                self.durable_transactions,
                &command.record,
            )?;
            self.persist(&mut gate, index.position, &change)?;
            if self.accelerated && !local && !change.rows.is_empty() {
                self.replay.apply(change.rows.clone())?;
            }
            index.publish(change);

            Ok(())
        })();
        let result = result.and_then(|()| {
            if gate.materialization.full(self.limits) {
                self.flush_locked(&mut gate, self.position()?)?;
            }
            Ok(())
        });
        if let Err(error) = &result {
            gate.fault = Some(error.to_string());
        }
        result.map_err(|error| error.to_string())
    }
    fn create_checkpoint(&self, path: &Path, p: Position) -> std::result::Result<(), String> {
        let mut gate = self.gate.lock();
        let result = (|| {
            Self::ensure(&gate)?;

            if self.position()? != p {
                return Err(storage("checkpoint cut differs from applied prefix"));
            }
            self.flush_locked(&mut gate, p)?;
            let application = &self.persistent;
            let durable = application.position().map_err(engine)?;
            if !sto_core::protocol::verified::recovery::materialized_checkpoint(
                p.index,
                durable.index,
                p.term == durable.term,
            ) {
                return Err(storage("checkpoint does not cover applied prefix"));
            }
            application
                .create_checkpoint(path, crate::application::Position::from(p).into())
                .map_err(engine)
        })();
        result.map_err(|e| e.to_string())
    }
    fn validate_checkpoint(&self, path: &Path, p: Position) -> std::result::Result<(), String> {
        let result: Result<()> = (|| {
            let application = &self.persistent;

            let (position, metadata) = application
                .checkpoint_metadata(
                    path,
                    crate::application::Position::from(p).into(),
                    &self.identity,
                )
                .map_err(engine)?;
            Index::from_metadata(metadata, position, self.owner, self.durable_transactions)?;
            Ok(())
        })();
        result.map_err(|e| e.to_string())
    }
    fn publish_checkpoint(&self, path: &Path, p: Position) -> std::result::Result<(), String> {
        let mut gate = self.gate.lock();
        let result = (|| {
            let application = &self.persistent;
            Self::ensure(&gate)?;
            if gate.active != 0 {
                return Err(storage(
                    "snapshot publication with active transactions requires recovery",
                ));
            }

            let (position, metadata) = application
                .checkpoint_metadata(
                    path,
                    crate::application::Position::from(p).into(),
                    &self.identity,
                )
                .map_err(engine)?;
            Index::from_metadata(metadata, position, self.owner, self.durable_transactions)?;
            application
                .publish_checkpoint(path, crate::application::Position::from(p).into())
                .map_err(engine)?;
            gate.materialization.clear();
            self.load_persistent()
        })();
        if let Err(error) = &result {
            gate.fault = Some(error.to_string());
        }
        result.map_err(|error| error.to_string())
    }
}
/// Both modes submit identical commands and apply them through the same state machine.
#[doc(hidden)]
pub enum Mode {
    Local(raft_host::LocalConfig),
    Raft(raft_host::Config),
}
impl From<raft_host::LocalConfig> for Mode {
    fn from(config: raft_host::LocalConfig) -> Self {
        Self::Local(config)
    }
}
impl From<raft_host::Config> for Mode {
    fn from(config: raft_host::Config) -> Self {
        Self::Raft(config)
    }
}

/// Single-disk RocksDB transaction log.
pub type LocalLog = Log<raft_host::LocalConfig>;
/// Raft-replicated RocksDB transaction log.
pub type ReplicatedLog = Log<raft_host::Config>;
pub use sto_core::protocol::journal::MemoryLog;

#[derive(Clone)]
enum Driver {
    Local(raft_host::LocalHost),
    Raft(Host),
}
impl Driver {
    fn status(&self) -> raft_host::Status {
        match self {
            Self::Local(host) => host.status(),
            Self::Raft(host) => host.status(),
        }
    }
    fn propose(
        &self,
        term: u64,
        bytes: Vec<u8>,
    ) -> std::result::Result<raft_host::Proposal, raft_host::AdmissionError> {
        match self {
            Self::Local(host) => host.propose(term, bytes),
            Self::Raft(host) => host.propose(term, bytes),
        }
    }
    fn read_barrier(
        &self,
        term: u64,
        timeout: Duration,
    ) -> std::result::Result<Position, raft_host::HostError> {
        match self {
            Self::Local(host) => host.read_barrier(term, timeout),
            Self::Raft(host) => host.read_barrier(term, timeout),
        }
    }
    fn shutdown(&self) {
        match self {
            Self::Local(host) => host.shutdown(),
            Self::Raft(host) => host.shutdown(),
        }
    }
}
struct Binding {
    host: Driver,
    state: Arc<State>,
    group: Option<u64>,
    flusher: Mutex<Option<materialization::Flusher>>,
}
/// Shared implementation behind the typed local and replicated logs.
pub struct Log<C> {
    backend: std::marker::PhantomData<fn() -> C>,
    binding: OnceLock<Binding>,
    timeout: Duration,
    incarnation: u64,
    next: AtomicU64,
}
impl<C: Into<Mode>> Log<C> {
    pub fn new(timeout: Duration) -> Result<Arc<Self>> {
        if timeout.is_zero() {
            return Err(storage("zero log deadline"));
        }
        let mut nonce = [0; 8];
        std::fs::File::open("/dev/urandom")
            .and_then(|mut file| file.read_exact(&mut nonce))
            .map_err(storage)?;
        Ok(Arc::new(Self {
            backend: std::marker::PhantomData,
            binding: OnceLock::new(),
            timeout,
            incarnation: u64::from_ne_bytes(nonce).max(1),
            next: AtomicU64::new(1),
        }))
    }
    pub fn start_engine(
        &self,
        config: C,
        store: &Arc<Store>,
        schema: Arc<Schema>,
        identity: Vec<u8>,
    ) -> Result<()> {
        self.start_backing(
            config,
            store,
            schema,
            identity,
            None,
            MaterializationOptions::default(),
        )
    }
    /// Use a shared RocksDB data directory with either transaction adapter.
    pub fn start_materialized(
        &self,
        config: C,
        store: &Arc<Store>,
        schema: Arc<Schema>,
        identity: Vec<u8>,
        backing: Arc<dyn mako_storage_api::PersistentApplication>,
        limits: MaterializationOptions,
    ) -> Result<()> {
        self.start_backing(config, store, schema, identity, Some(backing), limits)
    }
    fn start_backing(
        &self,
        config: C,
        store: &Arc<Store>,
        schema: Arc<Schema>,
        identity: Vec<u8>,
        backing: Option<Arc<dyn mako_storage_api::PersistentApplication>>,
        limits: MaterializationOptions,
    ) -> Result<()> {
        if self.binding.get().is_some() {
            return Err(storage("transaction log already started"));
        }
        let config = config.into();
        let sync_writes = match &config {
            Mode::Local(config) => config.sync_writes,
            Mode::Raft(config) => config.sync_writes,
        };
        let limits = limits.validate()?;
        let accelerated = store.database.persistent().is_none();
        let persistent = match backing.or_else(|| store.database.persistent()) {
            Some(application) => application,
            None => {
                let (log_path, create) = match &config {
                    Mode::Local(c) => (&c.directory, c.create),
                    Mode::Raft(c) => (&c.directory, c.create),
                };
                let data = log_path.with_extension("data");
                if !(data.join("CURRENT").exists()
                    || sto_rocksdb::control_path(&data).join("CURRENT").exists())
                    && (!create || log_path.exists())
                {
                    return Err(storage("native RocksDB data image missing; restore current-format data and log together"));
                }
                mako_storage_rocksdb::backing(&data, store.owner.into(), sync_writes)
                    .map_err(engine)?
            }
        };
        if persistent.durable() != sync_writes {
            return Err(storage("data engine and log durability policies differ"));
        }
        persistent.bind_identity(&identity).map_err(engine)?;
        let replay = Replay::start(store, schema.clone())?;
        let state = Arc::new(State {
            owner: store.owner,
            durable_transactions: sync_writes,
            index: Mutex::new(Index::default()),
            identity,
            accelerated,
            limits,
            gate: Mutex::new(Gate::default()),
            persistent,
            replay,
            schema,
        });
        if state.accelerated && state.persistent.position().map_err(engine)?.index == 0 {
            state.replay.bootstrap(state.persistent.clone())?;
        }
        state.load_persistent()?;
        let initial_state = state.persistent.initial_state_id().map_err(engine)?;
        let (host, group) = match config {
            Mode::Raft(mut config) => {
                config.initial_state = initial_state;
                let group = config.group;
                (
                    Driver::Raft(Host::start(config, state.clone()).map_err(storage)?),
                    Some(group),
                )
            }
            Mode::Local(mut config) => {
                config.initial_state = initial_state;
                (
                    Driver::Local(
                        raft_host::LocalHost::start(config, state.clone()).map_err(storage)?,
                    ),
                    None,
                )
            }
        };
        let application = &state.persistent;
        if let Err(error) = application.complete_bootstrap() {
            host.shutdown();
            return Err(engine(error));
        }
        let materialized = application.position().map_err(engine)?.index;
        let status = host.status();
        if sto_core::protocol::verified::recovery::materialized_replay(
            materialized,
            status.committed.index,
        )
        .is_none()
        {
            host.shutdown();
            return Err(storage("RocksDB image exceeds committed log"));
        }

        let flusher = if state.accelerated {
            Some(materialization::Flusher::start(&state)?)
        } else {
            None
        };
        self.binding
            .set(Binding {
                host,
                state,
                group,
                flusher: Mutex::new(flusher),
            })
            .map_err(|_| storage("transaction log already started"))
    }
    pub fn status(&self) -> Result<raft_host::Status> {
        self.binding
            .get()
            .map(|binding| binding.host.status())
            .ok_or_else(|| storage("log not started"))
    }
    pub fn host(&self) -> Result<&Host> {
        self.binding
            .get()
            .and_then(|binding| match &binding.host {
                Driver::Raft(host) => Some(host),
                Driver::Local(_) => None,
            })
            .ok_or_else(|| storage("Raft is not configured or started"))
    }
    /// Checkpoint the applied engine/protocol cut and durably retire its log
    /// prefix. Failure does not authorize discarding any recovery artifact.
    pub fn checkpoint(&self) -> Result<Position> {
        let binding = self
            .binding
            .get()
            .ok_or_else(|| storage("transaction log not started"))?;
        match &binding.host {
            Driver::Local(host) => host.checkpoint(),
            Driver::Raft(host) => host.checkpoint(),
        }
        .map_err(storage)
    }
    pub fn health(&self) -> Result<()> {
        let binding = self
            .binding
            .get()
            .ok_or_else(|| storage("transaction log not started"))?;
        State::ensure(&binding.state.gate.lock())?;
        if let Some(failure) = binding.host.status().failure {
            return Err(storage(failure));
        }
        Ok(())
    }
    pub fn shutdown(&self) {
        if let Some(binding) = self.binding.get() {
            binding.host.shutdown();
            binding.flusher.lock().take();
            let _ = binding.state.flush();
        }
    }
    /// Flush committed effects without changing the transaction decision or log retention.
    pub fn flush(&self) -> Result<()> {
        self.binding
            .get()
            .ok_or_else(|| storage("log not started"))?
            .state
            .flush()
    }
    pub fn materialized_position(&self) -> Result<Position> {
        let binding = self
            .binding
            .get()
            .ok_or_else(|| storage("log not started"))?;
        binding
            .state
            .persistent
            .position()
            .map(|p| Position {
                index: p.index,
                term: p.term,
            })
            .map_err(engine)
    }
}
struct Permit {
    state: Arc<State>,
    host: Driver,
    term: u64,
}
impl CommitPermit for Permit {
    fn authority(&self) -> Option<u64> {
        Some(self.term)
    }
    fn check(&self) -> Result<()> {
        State::ensure(&self.state.gate.lock())?;
        let status = self.host.status();
        if status.is_leader && status.term == self.term && status.failure.is_none() {
            Ok(())
        } else {
            Err(Error::new(
                ErrorCode::RouteChanged,
                "transaction log authority changed",
            ))
        }
    }
}
impl Drop for Permit {
    fn drop(&mut self) {
        self.state.gate.lock().active -= 1;
    }
}
impl<C: Into<Mode>> CommitLog for Log<C> {
    fn apply_at(&self, record: &Record, term: u64) -> Result<Entry> {
        self.apply_record(record, Some(term))
    }
    fn persistent(&self) -> bool {
        true
    }
    fn checkpoint(&self) -> Result<(u64, u64)> {
        Log::checkpoint(self).map(|p| (p.index, p.term))
    }
    fn publish(&self, _id: TransactionId, effect: &mut dyn FnMut() -> Result<()>) -> Result<()> {
        let binding = self
            .binding
            .get()
            .ok_or_else(|| storage("transaction log not started"))?;
        let mut gate = binding.state.gate.lock();
        State::ensure(&gate)?;
        match effect() {
            Ok(()) => Ok(()),
            Err(error) => {
                gate.fault = Some(error.to_string());
                Err(error)
            }
        }
    }

    fn ready(&self) -> bool {
        self.health().is_ok()
            && self.status().is_ok_and(|status| {
                status.leader.is_some()
                    && status.applied.index > 0
                    && status.applied.term == status.term
            })
    }
    fn health(&self) -> Result<()> {
        Log::health(self)
    }
    fn shutdown(&self) {
        Log::shutdown(self);
    }
    fn replica_status(&self, owner: u32) -> wire::ReplicaStatus {
        let Some(binding) = self.binding.get() else {
            return wire::ReplicaStatus {
                owner,
                group: None,
                leader: false,
                term: 0,
                healthy: false,
                durable_receipts: true,
            };
        };
        let status = binding.host.status();
        wire::ReplicaStatus {
            owner,
            group: binding.group,
            leader: status.is_leader,
            term: status.term,
            healthy: status.failure.is_none()
                && binding
                    .state
                    .gate
                    .try_lock()
                    .is_some_and(|gate| gate.fault.is_none()),
            durable_receipts: true,
        }
    }
    fn retains_gateway_receipts(&self) -> bool {
        true
    }
    fn gateway_receipt(
        &self,
        request: &wire::GatewayRequest,
    ) -> Result<Option<wire::GatewayResponse>> {
        let binding = self
            .binding
            .get()
            .ok_or_else(|| storage("transaction log not started"))?;
        State::ensure(&binding.state.gate.lock())?;
        binding.state.gateway_receipt(request)
    }
    fn append_gateway(
        &self,
        _bytes: &[u8],
        _time_term: u32,
        _receipt: &wire::GatewayReceipt,
    ) -> Result<()> {
        Err(Error::new(
            ErrorCode::Invalid,
            "transaction logs require Prepare/Complete records",
        ))
    }
    fn admit(&self) -> Result<Box<dyn CommitPermit>> {
        let binding = self
            .binding
            .get()
            .ok_or_else(|| Error::new(ErrorCode::Closed, "transaction log not started"))?;
        State::ensure(&binding.state.gate.lock())?;
        let status = binding.host.status();
        if !status.is_leader {
            return Err(Error::new(ErrorCode::RouteChanged, "replica is not leader"));
        }
        binding
            .host
            .read_barrier(status.term, self.timeout)
            .map_err(|error| Error::new(ErrorCode::RouteChanged, error.to_string()))?;
        let mut gate = binding.state.gate.lock();
        State::ensure(&gate)?;
        let now = binding.host.status();
        if !now.is_leader || now.term != status.term || now.failure.is_some() {
            return Err(Error::new(
                ErrorCode::RouteChanged,
                "transaction log authority changed",
            ));
        }
        gate.active = gate
            .active
            .checked_add(1)
            .ok_or_else(|| storage("replica admission count exhausted"))?;
        Ok(Box::new(Permit {
            state: binding.state.clone(),
            host: binding.host.clone(),
            term: status.term,
        }))
    }
    fn append(&self, _bytes: &[u8], _time_term: u32) -> Result<()> {
        Err(Error::new(
            ErrorCode::Invalid,
            "transaction logs require Prepare/Complete records",
        ))
    }
}

impl<C: Into<Mode>> Log<C> {
    fn apply_record(&self, record: &Record, expected: Option<u64>) -> Result<Entry> {
        let permit = <Self as CommitLog>::admit(self)?;
        let authority = permit.authority().expect("persistent log permit");
        if expected.is_some_and(|term| term != authority) {
            return Err(Error::new(
                ErrorCode::RouteChanged,
                "journal operation authority changed",
            ));
        }
        let binding = self
            .binding
            .get()
            .ok_or_else(|| storage("transaction log not started"))?;
        let prior = binding.state.transaction(record.transaction().id)?;
        journal::transition(binding.state.owner, self.durable(), prior.as_ref(), record)?;
        if let Record::Prepare { rows, .. } = record {
            for row in rows {
                binding.state.schema.key_for_log(row.table)?;
            }
        }
        let sequence = self
            .next
            .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |next| {
                next.checked_add(1)
            })
            .map_err(|_| storage("transaction log identity exhausted"))?;
        let command = wire::encode(&JournalCommand {
            version: 2,
            incarnation: self.incarnation,
            sequence,
            record: record.clone(),
        })?;
        let status = binding.host.status();
        if !status.is_leader || status.term != authority || status.failure.is_some() {
            return Err(Error::new(
                ErrorCode::RouteChanged,
                "journal authority changed",
            ));
        }
        {
            let mut gate = binding.state.gate.lock();
            State::ensure(&gate)?;
            gate.pending
                .insert((self.incarnation, sequence), command.clone());
        }
        let proposal = binding.host.propose(authority, command).map_err(|error| {
            Error::new(ErrorCode::Unknown, format!("journal admission: {error:?}"))
        })?;
        match proposal.wait(self.timeout) {
            ProposalStatus::Applied => {
                binding
                    .state
                    .gate
                    .lock()
                    .pending
                    .remove(&(self.incarnation, sequence));
                let entry = binding
                    .state
                    .transaction(record.transaction().id)?
                    .ok_or_else(|| storage("applied journal record has no transaction"))?;
                journal::acknowledge(record, &entry)?;
                Ok(entry)
            }
            outcome => Err(Error::new(
                ErrorCode::Unknown,
                format!("journal completion: {outcome:?}"),
            )),
        }
    }
}

impl<C: Into<Mode>> Journal for Log<C> {
    fn durable(&self) -> bool {
        self.binding
            .get()
            .is_some_and(|binding| binding.state.durable_transactions)
    }
    fn apply(&self, record: &Record) -> Result<Entry> {
        self.apply_record(record, None)
    }
    fn lookup(&self, id: TransactionId) -> Result<Option<Entry>> {
        let _permit = <Self as CommitLog>::admit(self)?;
        self.binding
            .get()
            .ok_or_else(|| storage("transaction log not started"))?
            .state
            .transaction(id)
    }
    fn entries(&self) -> Result<Vec<Entry>> {
        let _permit = <Self as CommitLog>::admit(self)?;
        self.binding
            .get()
            .ok_or_else(|| storage("transaction log not started"))?
            .state
            .transactions()
    }
}
