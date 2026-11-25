//! Shared local and Raft log application. The pluggable Masstree path derives
//! its protocol index and data from checkpoints plus the committed log.
//! A locally installed proposal is identified by owned bytes and is never
//! replayed into its own retained locks. Unknown outcomes fence the shard.
use crate::{
    application::Index,
    checkpoint,
    durable::DurableRows,
    migration::Schema,
    redo,
    storage::{engine, CommitLog, CommitPermit, Store},
    wire::{self, Error, ErrorCode, Result},
};
use mako_protocol::journal::{self, Entry, Journal, Record, TransactionId};
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

#[derive(Serialize, Deserialize)]
struct Command {
    version: u32,
    incarnation: u64,
    sequence: u64,
    rows: Vec<redo::Row>,
    receipt: Option<wire::GatewayReceipt>,
}

#[derive(Serialize, Deserialize)]
struct JournalCommand {
    version: u32,
    incarnation: u64,
    sequence: u64,
    record: Record,
}
fn storage(error: impl std::fmt::Display) -> Error {
    Error::new(ErrorCode::Fatal, error.to_string())
}
fn position(p: Position) -> sto_rocksdb::Position {
    sto_rocksdb::Position {
        index: p.index,
        term: p.term,
    }
}
fn host_position(p: sto_rocksdb::Position) -> Position {
    Position {
        index: p.index,
        term: p.term,
    }
}

enum ReplayWork {
    Apply(Vec<redo::Row>, mpsc::SyncSender<Result<()>>),
    Export {
        path: std::path::PathBuf,
        index: Index,
        overlay: Vec<redo::Row>,
        identity: Vec<u8>,
        durable: bool,
        reply: mpsc::SyncSender<Result<()>>,
    },
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
                        ReplayWork::Apply(rows, reply) => (rows, reply),
                        ReplayWork::Export {
                            path,
                            index,
                            overlay,
                            identity,
                            durable,
                            reply,
                        } => {
                            let result = (|| {
                                let store = weak
                                    .upgrade()
                                    .ok_or_else(|| storage("shard store stopped"))?;
                                let engine_name = format!("{:?}", store.database.kind());
                                let mut overlay: BTreeMap<_, _> = overlay
                                    .into_iter()
                                    .map(|r| ((r.table, r.key.clone()), r))
                                    .collect();
                                checkpoint::write(
                                    &path,
                                    checkpoint::Identity {
                                        owner: store.owner,
                                        engine: &engine_name,
                                        schema: &identity,
                                        durable,
                                    },
                                    &index,
                                    |write| {
                                        for (_, table) in store.tables() {
                                            let mut after: Option<Vec<u8>> = None;
                                            loop {
                                                let page = worker
                                                    .checkpoint_page(
                                                        &table.tree,
                                                        after.as_deref(),
                                                        256,
                                                    )
                                                    .map_err(engine)?;
                                                if !page.end && page.rows.is_empty() {
                                                    return Err(storage(
                                                        "checkpoint export made no progress",
                                                    ));
                                                }
                                                for row in page.rows {
                                                    if after
                                                        .as_ref()
                                                        .is_some_and(|prior| prior >= &row.key)
                                                    {
                                                        return Err(storage(
                                                            "checkpoint export key order",
                                                        ));
                                                    }
                                                    after = Some(row.key.clone());
                                                    let row = overlay
                                                        .remove(&(table.log_id, row.key.clone()))
                                                        .unwrap_or(redo::Row {
                                                            table: table.log_id,
                                                            key: row.key,
                                                            value: row.value,
                                                            time_term: row.time_term,
                                                        });
                                                    write(row)?;
                                                }
                                                if page.end {
                                                    break;
                                                }
                                            }
                                        }
                                        for (_, row) in overlay {
                                            schema.key_for_log(row.table)?;
                                            write(row)?;
                                        }
                                        Ok(())
                                    },
                                )
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
    fn export(
        &self,
        path: &Path,
        index: Index,
        overlay: Vec<redo::Row>,
        identity: &[u8],
        durable: bool,
    ) -> Result<()> {
        let (reply, receive) = mpsc::sync_channel(1);
        self.sender
            .as_ref()
            .expect("live replay")
            .send(ReplayWork::Export {
                path: path.into(),
                index,
                overlay,
                identity: identity.to_vec(),
                durable,
                reply,
            })
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
    // Complete is durable before the affine participant installs. Checkpoints
    // overlay these exact committed rows while publication is paused.
    installing: BTreeMap<TransactionId, Vec<redo::Row>>,
}
struct State {
    owner: u32,
    durable_transactions: bool,
    gate: Mutex<Gate>,
    durable: Option<Arc<DurableRows>>,
    persistent: Option<Arc<dyn mako_storage_api::PersistentApplication>>,
    index: Mutex<Index>,
    identity: Vec<u8>,
    engine_name: String,
    replay: Replay,
    schema: Arc<Schema>,
}
impl State {
    fn restore(&self) -> Result<()> {
        let Some(durable) = &self.durable else {
            return Ok(());
        };
        let mut batch = Vec::new();
        durable.visit(|row| {
            batch.push(row);
            if batch.len() == 256 {
                self.replay.apply(std::mem::take(&mut batch))?;
            }
            Ok(())
        })?;
        if !batch.is_empty() {
            self.replay.apply(batch)?;
        }
        Ok(())
    }
    fn persist(&self, change: &crate::application::Change) -> Result<()> {
        if let Some(application) = &self.persistent {
            application
                .apply(
                    change.position.into(),
                    change.fingerprint(),
                    &change.rows,
                    &change.metadata()?,
                )
                .map_err(engine)?;
        }
        Ok(())
    }
    fn load_persistent(&self) -> Result<()> {
        if let Some(application) = &self.persistent {
            *self.index.lock() = Index::from_metadata(
                application.metadata().map_err(engine)?,
                application.position().map_err(engine)?,
                self.owner,
                self.durable_transactions,
            )?;
        }
        Ok(())
    }
    fn position(&self) -> Result<Position> {
        match &self.durable {
            Some(durable) => durable.position().map(host_position),
            None => Ok(self.index.lock().position.into()),
        }
    }
    fn transaction(&self, id: TransactionId) -> Result<Option<Entry>> {
        match &self.durable {
            Some(durable) => durable.transaction(id),
            None => Ok(self.index.lock().transactions.get(&id).cloned()),
        }
    }
    fn transactions(&self) -> Result<Vec<Entry>> {
        match &self.durable {
            Some(durable) => durable.transactions(),
            None => Ok(self.index.lock().transactions.values().cloned().collect()),
        }
    }
    fn gateway_receipt(
        &self,
        request: &wire::GatewayRequest,
    ) -> Result<Option<wire::GatewayResponse>> {
        if let Some(durable) = &self.durable {
            return durable.gateway_receipt(request);
        }
        let index = self.index.lock();
        match index.receipts.get(&(request.client, request.sequence)) {
            Some(receipt) if &receipt.request == request => Ok(Some(receipt.response.clone())),
            Some(_) => Err(storage("gateway request identity reused")),
            None => Ok(None),
        }
    }
    fn checkpoint_identity(&self) -> checkpoint::Identity<'_> {
        checkpoint::Identity {
            owner: self.owner,
            engine: &self.engine_name,
            schema: &self.identity,
            durable: self.durable_transactions,
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
                if let Some(durable) = &self.durable {
                    return durable.advance(position(p));
                }
                let mut index = self.index.lock();
                let change = index.no_op(p.into())?;
                self.persist(&change)?;
                index.publish(change);
                return Ok(());
            };
            // wire::encode prefixes its own u32 version; the application
            // command version follows it. Keep old row-command bytes readable.
            if bytes.get(4..8) == Some(2u32.to_le_bytes().as_slice()) {
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
                if let Some(durable) = &self.durable {
                    let (rows, _) =
                        durable.apply_journal(position(p), bytes, self.owner, &command.record)?;
                    if !local && !rows.is_empty() {
                        self.replay.apply(rows)?;
                    }
                } else {
                    let mut index = self.index.lock();
                    let change = index.journal(
                        p.into(),
                        bytes,
                        self.owner,
                        self.durable_transactions,
                        &command.record,
                    )?;
                    self.persist(&change)?;
                    if self.persistent.is_none() && !change.rows.is_empty() {
                        if local {
                            gate.installing
                                .insert(command.record.transaction().id, change.rows.clone());
                        } else {
                            self.replay.apply(change.rows.clone())?;
                        }
                    }
                    index.publish(change);
                }
                return Ok(());
            }
            let command: Command = wire::decode(bytes)?;
            if command.version != 1
                || command.incarnation == 0
                || command.sequence == 0
                || (command.rows.is_empty() && command.receipt.is_none())
            {
                return Err(storage("invalid log command"));
            }
            for row in &command.rows {
                row.validate()?;
                self.schema.key_for_log(row.table)?;
            }
            let local = gate
                .pending
                .get(&(command.incarnation, command.sequence))
                .is_some_and(|pending| pending == bytes);
            if !local && gate.active != 0 {
                // A former leader still owns speculative transactions. Refuse
                // to mix a different leader's committed prefix into that RAM;
                // restart rebuilds it from durable authority before serving.
                return Err(storage(
                    "leadership changed with active local transactions; recovery required",
                ));
            }
            if let Some(durable) = &self.durable {
                let rows = durable.apply_gateway(
                    position(p),
                    bytes,
                    &command.rows,
                    command.receipt.as_ref(),
                )?;
                if !local {
                    self.replay.apply(rows)?;
                }
            } else {
                let mut index = self.index.lock();
                let change =
                    index.gateway(p.into(), bytes, &command.rows, command.receipt.as_ref())?;
                self.persist(&change)?;
                if self.persistent.is_none() && !local {
                    self.replay.apply(change.rows.clone())?;
                }
                index.publish(change);
            }
            Ok(())
        })();
        if let Err(error) = &result {
            gate.fault = Some(error.to_string());
        }
        result.map_err(|error| error.to_string())
    }
    fn create_checkpoint(&self, path: &Path, p: Position) -> std::result::Result<(), String> {
        let gate = self.gate.lock();
        let result = (|| {
            Self::ensure(&gate)?;
            if let Some(durable) = &self.durable {
                return durable.create_checkpoint(path, position(p));
            }
            if let Some(application) = &self.persistent {
                return application
                    .create_checkpoint(path, crate::application::Position::from(p).into())
                    .map_err(engine);
            }
            let index = self.index.lock().clone();
            if index.position != p.into() {
                return Err(storage("checkpoint cut differs from applied prefix"));
            }
            let overlay = gate.installing.values().flatten().cloned().collect();
            self.replay.export(
                path,
                index,
                overlay,
                &self.identity,
                self.durable_transactions,
            )
        })();
        result.map_err(|e| e.to_string())
    }
    fn validate_checkpoint(&self, path: &Path, p: Position) -> std::result::Result<(), String> {
        let result = (|| {
            if let Some(durable) = &self.durable {
                return durable.validate_checkpoint(path, position(p));
            }
            if let Some(application) = &self.persistent {
                let (position, metadata) = application
                    .checkpoint_metadata(
                        path,
                        crate::application::Position::from(p).into(),
                        &self.identity,
                    )
                    .map_err(engine)?;
                Index::from_metadata(metadata, position, self.owner, self.durable_transactions)?;
                return Ok(());
            }
            checkpoint::validate(path, self.checkpoint_identity(), p.into())?;
            checkpoint::visit(path, |row| self.schema.key_for_log(row.table).map(|_| ()))
        })();
        result.map_err(|e| e.to_string())
    }
    fn publish_checkpoint(&self, path: &Path, p: Position) -> std::result::Result<(), String> {
        let mut gate = self.gate.lock();
        let result = (|| {
            Self::ensure(&gate)?;
            if gate.active != 0 {
                return Err(storage(
                    "snapshot publication with active transactions requires recovery",
                ));
            }
            if let Some(durable) = &self.durable {
                durable.publish_checkpoint(path, position(p))?;
                return self.restore();
            }
            if let Some(application) = &self.persistent {
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
                return self.load_persistent();
            }
            let index = checkpoint::validate(path, self.checkpoint_identity(), p.into())?;
            let replay_start = mako_protocol_core::recovery::replay_start(
                self.index.lock().position.index,
                p.index,
            );
            if index.position.index != replay_start {
                return Err(storage("checkpoint replay start mismatch"));
            }
            let mut batch = Vec::new();
            checkpoint::visit(path, |row| {
                self.schema.key_for_log(row.table)?;
                batch.push(row);
                if batch.len() == 256 {
                    self.replay.apply(std::mem::take(&mut batch))?;
                }
                Ok(())
            })?;
            if !batch.is_empty() {
                self.replay.apply(batch)?;
            }
            *self.index.lock() = index;
            gate.installing.clear();
            Ok(())
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
pub use mako_protocol::journal::MemoryLog;

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
    /// Run before starting application listeners. Durable rows are replayed
    /// first, then the ordering driver replays its committed prefix.
    pub fn start(
        &self,
        config: C,
        store: &Arc<Store>,
        schema: Arc<Schema>,
        durable: Arc<DurableRows>,
    ) -> Result<()> {
        self.start_backing(config, store, schema, Some(durable), Vec::new())
    }
    pub fn start_engine(
        &self,
        config: C,
        store: &Arc<Store>,
        schema: Arc<Schema>,
        identity: Vec<u8>,
    ) -> Result<()> {
        self.start_backing(config, store, schema, None, identity)
    }
    fn start_backing(
        &self,
        config: C,
        store: &Arc<Store>,
        schema: Arc<Schema>,
        durable: Option<Arc<DurableRows>>,
        identity: Vec<u8>,
    ) -> Result<()> {
        if self.binding.get().is_some() {
            return Err(storage("transaction log already started"));
        }
        let config = config.into();
        let sync_writes = match &config {
            Mode::Local(config) => config.sync_writes,
            Mode::Raft(config) => config.sync_writes,
        };
        let persistent = if durable.is_none() {
            store.database.persistent()
        } else {
            None
        };
        if let Some(application) = &persistent {
            if application.durable() != sync_writes {
                return Err(storage("data engine and log durability policies differ"));
            }
            application.bind_identity(&identity).map_err(engine)?;
        }
        let replay = Replay::start(store, schema.clone())?;
        let state = Arc::new(State {
            owner: store.owner,
            durable_transactions: sync_writes && durable.as_ref().is_none_or(|d| d.durable()),
            index: Mutex::new(Index::default()),
            identity,
            engine_name: format!("{:?}", store.database.kind()),
            gate: Mutex::new(Gate::default()),
            durable,
            persistent,
            replay,
            schema,
        });
        if state.persistent.is_some() {
            state.load_persistent()?;
        } else if state.durable.is_none() {
            let (log_path, create) = match &config {
                Mode::Local(c) => (&c.directory, c.create),
                Mode::Raft(c) => (&c.directory, c.create),
            };
            let bootstrap = log_path.with_extension("bootstrap");
            if !bootstrap.exists() {
                if !create || log_path.exists() {
                    return Err(storage(
                        "engine bootstrap checkpoint missing; existing state requires conversion",
                    ));
                }
                if let Some(parent) = bootstrap.parent() {
                    std::fs::create_dir_all(parent).map_err(storage)?;
                }
                let staging =
                    bootstrap.with_extension(format!("bootstrap-{:x}.creating", self.incarnation));
                state
                    .create_checkpoint(&staging, Position::default())
                    .map_err(storage)?;
                std::fs::rename(&staging, &bootstrap).map_err(storage)?;
                if let Some(parent) = bootstrap.parent() {
                    std::fs::File::open(parent)
                        .and_then(|f| f.sync_all())
                        .map_err(storage)?;
                }
            }
            state
                .publish_checkpoint(&bootstrap, Position::default())
                .map_err(storage)?;
        } else {
            state.restore()?;
        }
        let (host, group) = match config {
            Mode::Raft(config) => {
                let group = config.group;
                (
                    Driver::Raft(Host::start(config, state.clone()).map_err(storage)?),
                    Some(group),
                )
            }
            Mode::Local(config) => (
                Driver::Local(raft_host::LocalHost::start(config, state.clone()).map_err(storage)?),
                None,
            ),
        };
        self.binding
            .set(Binding { host, state, group })
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
        }
    }
    fn append_inner(&self, binding: &Binding, bytes: &[u8], time_term: u32) -> Result<()> {
        self.append_record(binding, bytes, time_term, None)
    }
    fn append_record(
        &self,
        binding: &Binding,
        bytes: &[u8],
        time_term: u32,
        receipt: Option<&wire::GatewayReceipt>,
    ) -> Result<()> {
        let rows = if bytes.is_empty() && receipt.is_some() {
            Vec::new()
        } else {
            redo::decode_legacy(bytes, time_term)?
        };
        let sequence = self
            .next
            .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |next| {
                next.checked_add(1)
            })
            .map_err(|_| storage("transaction log identity exhausted"))?;
        let command = wire::encode(&Command {
            version: 1,
            incarnation: self.incarnation,
            sequence,
            rows,
            receipt: receipt.cloned(),
        })?;
        let status = binding.host.status();
        if !status.is_leader || status.failure.is_some() {
            return Err(storage("leadership lost after local install"));
        }
        {
            let mut gate = binding.state.gate.lock();
            State::ensure(&gate)?;
            gate.pending
                .insert((self.incarnation, sequence), command.clone());
        }
        let proposal = binding
            .host
            .propose(status.term, command)
            .map_err(|error| storage(format!("log admission: {error:?}")))?;
        match proposal.wait(self.timeout) {
            ProposalStatus::Applied => {
                binding
                    .state
                    .gate
                    .lock()
                    .pending
                    .remove(&(self.incarnation, sequence));
                Ok(())
            }
            outcome => Err(storage(format!("log completion: {outcome:?}"))),
        }
    }
}
struct Permit {
    state: Arc<State>,
    host: Driver,
    term: u64,
}
impl CommitPermit for Permit {
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
    fn checkpoint(&self) -> Result<(u64, u64)> {
        Log::checkpoint(self).map(|p| (p.index, p.term))
    }
    fn publish(&self, id: TransactionId, effect: &mut dyn FnMut() -> Result<()>) -> Result<()> {
        let binding = self
            .binding
            .get()
            .ok_or_else(|| storage("transaction log not started"))?;
        let mut gate = binding.state.gate.lock();
        State::ensure(&gate)?;
        match effect() {
            Ok(()) => {
                gate.installing.remove(&id);
                Ok(())
            }
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
        bytes: &[u8],
        time_term: u32,
        receipt: &wire::GatewayReceipt,
    ) -> Result<()> {
        let binding = self.binding.get().ok_or_else(|| {
            Error::new(
                ErrorCode::Unknown,
                "transaction log not started after install",
            )
        })?;
        if binding.state.durable.is_none() {
            return Err(Error::new(
                ErrorCode::Invalid,
                "pluggable engines require transaction Prepare/Complete records",
            ));
        }
        if let Err(error) = self.append_record(binding, bytes, time_term, Some(receipt)) {
            binding.state.gate.lock().fault = Some(error.to_string());
            return Err(Error::new(ErrorCode::Unknown, error.to_string()));
        }
        Ok(())
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
    fn append(&self, bytes: &[u8], time_term: u32) -> Result<()> {
        let binding = self.binding.get().ok_or_else(|| {
            Error::new(
                ErrorCode::Unknown,
                "transaction log not started after install",
            )
        })?;
        if binding.state.durable.is_none() {
            return Err(Error::new(
                ErrorCode::Invalid,
                "pluggable engines require transaction Prepare/Complete records",
            ));
        }
        let result = self.append_inner(binding, bytes, time_term);
        if let Err(error) = result {
            binding.state.gate.lock().fault = Some(error.to_string());
            return Err(Error::new(ErrorCode::Unknown, error.to_string()));
        }
        Ok(())
    }
}

impl<C: Into<Mode>> Journal for Log<C> {
    fn durable(&self) -> bool {
        self.binding
            .get()
            .is_some_and(|binding| binding.state.durable_transactions)
    }
    fn apply(&self, record: &Record) -> Result<Entry> {
        let _permit = <Self as CommitLog>::admit(self)?;
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
        if !status.is_leader || status.failure.is_some() {
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
        let proposal = binding
            .host
            .propose(status.term, command)
            .map_err(|error| {
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
