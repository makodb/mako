//! Shared local and Raft log application. The ordered application callback writes
//! committed RocksDB rows; one affine replay thread updates follower Masstrees.
//! A locally installed proposal is identified by owned bytes and is never
//! replayed into its own retained locks. Unknown outcomes fence the shard.
use crate::{
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

type ReplayWork = (Vec<redo::Row>, mpsc::SyncSender<Result<()>>);
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
                while let Ok((rows, reply)) = recv.recv() {
                    let result = (|| {
                        let store = weak
                            .upgrade()
                            .ok_or_else(|| storage("shard store stopped"))?;
                        for row in rows {
                            let key = schema.key_for_log(row.table)?;
                            let table = store.open(&mut worker, key, row.table)?;
                            worker
                                .replay(&table.tree, &row.key, row.value.as_deref(), row.time_term)
                                .map_err(engine)?;
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
    fn apply(&self, rows: Vec<redo::Row>) -> Result<()> {
        let (send, recv) = mpsc::sync_channel(1);
        self.sender
            .as_ref()
            .expect("live replay")
            .send((rows, send))
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
}
struct State {
    owner: u32,
    durable_transactions: bool,
    gate: Mutex<Gate>,
    durable: Arc<DurableRows>,
    replay: Replay,
    schema: Arc<Schema>,
}
impl State {
    fn restore(&self) -> Result<()> {
        let mut batch = Vec::new();
        self.durable.visit(|row| {
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
        self.durable
            .position()
            .map(host_position)
            .map_err(|error| error.to_string())
    }
    fn apply(&self, p: Position, bytes: Option<&[u8]>) -> std::result::Result<(), String> {
        let mut gate = self.gate.lock();
        let result = (|| {
            Self::ensure(&gate)?;
            let Some(bytes) = bytes else {
                return self.durable.advance(position(p));
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
                let (rows, _) =
                    self.durable
                        .apply_journal(position(p), bytes, self.owner, &command.record)?;
                if !local && !rows.is_empty() {
                    self.replay.apply(rows)?;
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
            let rows = self.durable.apply_gateway(
                position(p),
                bytes,
                &command.rows,
                command.receipt.as_ref(),
            )?;
            if !local {
                self.replay.apply(rows)?;
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
        Self::ensure(&gate).map_err(|error| error.to_string())?;
        self.durable
            .create_checkpoint(path, position(p))
            .map_err(|error| error.to_string())
    }
    fn validate_checkpoint(&self, path: &Path, p: Position) -> std::result::Result<(), String> {
        self.durable
            .validate_checkpoint(path, position(p))
            .map_err(|error| error.to_string())
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
            self.durable.publish_checkpoint(path, position(p))?;
            self.restore()
        })();
        if let Err(error) = &result {
            gate.fault = Some(error.to_string());
        }
        result.map_err(|error| error.to_string())
    }
}
/// Both modes submit identical commands and apply them through the same state machine.
pub enum Mode {
    Local(raft_host::LocalConfig),
    Raft(raft_host::Config),
}
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
pub struct Log {
    binding: OnceLock<Binding>,
    timeout: Duration,
    incarnation: u64,
    next: AtomicU64,
}
impl Log {
    pub fn new(timeout: Duration) -> Result<Arc<Self>> {
        if timeout.is_zero() {
            return Err(storage("zero log deadline"));
        }
        let mut nonce = [0; 8];
        std::fs::File::open("/dev/urandom")
            .and_then(|mut file| file.read_exact(&mut nonce))
            .map_err(storage)?;
        Ok(Arc::new(Self {
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
        config: Mode,
        store: &Arc<Store>,
        schema: Arc<Schema>,
        durable: Arc<DurableRows>,
    ) -> Result<()> {
        if self.binding.get().is_some() {
            return Err(storage("transaction log already started"));
        }
        let sync_writes = match &config {
            Mode::Local(config) => config.sync_writes,
            Mode::Raft(config) => config.sync_writes,
        };
        let replay = Replay::start(store, schema.clone())?;
        let state = Arc::new(State {
            owner: store.owner,
            durable_transactions: sync_writes && durable.durable(),
            gate: Mutex::new(Gate::default()),
            durable,
            replay,
            schema,
        });
        state.restore()?;
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
impl CommitLog for Log {
    fn transaction_journal(&self) -> Option<&dyn Journal> {
        Some(self)
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
        binding.state.durable.gateway_receipt(request)
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
        let result = self.append_inner(binding, bytes, time_term);
        if let Err(error) = result {
            binding.state.gate.lock().fault = Some(error.to_string());
            return Err(Error::new(ErrorCode::Unknown, error.to_string()));
        }
        Ok(())
    }
}

impl Journal for Log {
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
        let prior = binding.state.durable.transaction(record.transaction().id)?;
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
                    .durable
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
            .durable
            .transaction(id)
    }
    fn entries(&self) -> Result<Vec<Entry>> {
        let _permit = <Self as CommitLog>::admit(self)?;
        self.binding
            .get()
            .ok_or_else(|| storage("transaction log not started"))?
            .state
            .durable
            .transactions()
    }
}
