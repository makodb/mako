use crate::{config::{check_scope, Config, GroupConfig}, rpc::Reply, wire::{Envelope, Error, ErrorCode, Handle, Operation, Position, Request, RequestId, Response, Result, PROTOCOL_VERSION, RPC_ID}};
use mako_sharding::routing::Snapshot;
use parking_lot::Mutex;
use raft_host::{AdmissionError, Host, HostError, Proposal, ProposalStatus, StateMachine};
use srpc::{reactor::PollThread, server::{Request as RpcRequest, Server, Service, WeakServerConnection}};
use std::{collections::{BTreeMap, HashMap}, ffi::CString, io::Write, path::Path,
    sync::{atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering}, mpsc::{self, Receiver, SyncSender, TrySendError}, Arc},
    thread::{self, JoinHandle}, time::{Duration, Instant, SystemTime, UNIX_EPOCH}};
use sto_rocksdb::{Database, Prepared, Transaction};

fn storage(error: sto_rocksdb::Error) -> Error {
    use sto_rocksdb::Error as E;
    let code = match error {
        E::Conflict | E::AlreadyExists => ErrorCode::Conflict,
        E::WrongGroup => ErrorCode::CrossShard,
        E::UnknownTable(_) => ErrorCode::UnknownTable,
        E::TooLarge => ErrorCode::TooLarge,
        E::InvalidCommand(_) | E::InvalidState(_) | E::RequestReuse => ErrorCode::Invalid,
        E::Storage(_) | E::Corruption(_) | E::Fenced | E::VersionExhausted | E::AppliedGap { .. } => ErrorCode::StorageFailure,
        E::Core(_) => ErrorCode::Closed,
    };
    Error::new(code, error.to_string())
}
fn committed_failure(error: sto_rocksdb::Error) -> Error {
    Error::new(ErrorCode::StorageFailure, format!("committed under configured durability; local completion failed: {error}"))
}
fn host_error(error: HostError) -> Error {
    let code = match &error { HostError::NotLeader => ErrorCode::NotLeader,
        HostError::Storage(_) => ErrorCode::StorageFailure, HostError::Unavailable(_) => ErrorCode::Closed,
        HostError::Timeout | HostError::Unknown => ErrorCode::UnknownOutcome };
    Error::new(code, error.to_string())
}
fn admission(error: AdmissionError) -> Error {
    let code = match &error { AdmissionError::NotLeader { .. } => ErrorCode::NotLeader,
        AdmissionError::Unavailable(_) => ErrorCode::Closed, AdmissionError::TooLarge => ErrorCode::TooLarge,
        AdmissionError::Unknown => ErrorCode::UnknownOutcome };
    Error::new(code, format!("{error:?}"))
}
fn adapter_position(p: raft_host::Position) -> sto_rocksdb::Position { sto_rocksdb::Position { index: p.index, term: p.term } }
fn host_position(p: sto_rocksdb::Position) -> raft_host::Position { raft_host::Position { index: p.index, term: p.term } }

struct Machine {
    db: Arc<Database>,
    #[cfg(feature = "test-hooks")]
    fault: std::sync::atomic::AtomicU8,
    #[cfg(feature = "test-hooks")]
    hooks: crate::test_hooks::Hooks,
}
impl StateMachine for Machine {
    fn applied_position(&self) -> std::result::Result<raft_host::Position, String> {
        self.db.applied().map(host_position).map_err(|e| e.to_string())
    }
    fn apply(&self, position: raft_host::Position, command: Option<&[u8]>) -> std::result::Result<(), String> {
        #[cfg(feature = "test-hooks")]
        if command.is_some() {
            match self.fault.swap(0, Ordering::AcqRel) {
                1 => self.db.inject_apply_fault(sto_rocksdb::ApplyFault::BeforeBatch),
                2 => self.db.inject_apply_fault(sto_rocksdb::ApplyFault::AfterBatch),
                _ => {}
            }
        }
        #[cfg(feature = "test-hooks")]
        if command.is_some() { self.hooks.hit(crate::wire::TestStage::BeforeApply, Some(position.into())); }
        let result = match command { Some(bytes) => self.db.apply(adapter_position(position), bytes).map(|_| ()),
            None => self.db.advance_applied(adapter_position(position)).map(|_| ()) }.map_err(|e| e.to_string());
        #[cfg(feature = "test-hooks")]
        if command.is_some() && result.is_ok() { self.hooks.hit(crate::wire::TestStage::AfterApply, Some(position.into())); }
        result
    }
    fn create_checkpoint(&self, path: &Path, position: raft_host::Position) -> std::result::Result<(), String> {
        self.db.create_checkpoint(path, adapter_position(position)).map_err(|e| e.to_string())
    }
    fn validate_checkpoint(&self, path: &Path, position: raft_host::Position) -> std::result::Result<(), String> {
        self.db.validate_checkpoint(path, adapter_position(position)).map_err(|e| e.to_string())
    }
    fn publish_checkpoint(&self, path: &Path, position: raft_host::Position) -> std::result::Result<(), String> {
        self.db.publish_checkpoint(path, adapter_position(position)).map_err(|e| e.to_string())
    }
}
struct Work { request: Request, reply: Reply }
struct Lane { send: SyncSender<Work>, busy: AtomicBool }
enum InFlight { Open, Proposed(Proposal), Unpositioned }
struct Group {
    config: GroupConfig, global: Arc<Config>, snapshot: Arc<Snapshot>, db: Arc<Database>, host: Host,
    certification: Mutex<()>, active: Mutex<HashMap<(u64, u64), InFlight>>,
    lanes: Vec<Lane>, next_lane: AtomicUsize, nonce: AtomicU64,
    stopped: Arc<AtomicBool>, fenced: AtomicBool,
    #[cfg(feature = "test-hooks")]
    machine: Arc<Machine>,
}
impl Group {
    fn term(&self) -> Result<u64> {
        if self.fenced.load(Ordering::Acquire) || self.stopped.load(Ordering::Acquire) || self.db.is_fenced() {
            return Err(Error::new(ErrorCode::StorageFailure, "group admission fenced; recover before serving"));
        }
        let status = self.host.status();
        if let Some(failure) = status.failure { return Err(Error::new(ErrorCode::StorageFailure, failure)); }
        if !status.is_leader { return Err(Error::new(ErrorCode::NotLeader, "group is not leader")); }
        Ok(status.term)
    }
    fn reserve(&self, request: RequestId) -> Result<ActiveRequest<'_>> {
        let mut requests = self.active.lock();
        requests.retain(|_, state| match state {
            InFlight::Proposed(proposal) => !matches!(proposal.status(), ProposalStatus::Applied | ProposalStatus::Discarded),
            _ => true,
        });
        let key = (request.client, request.sequence);
        if requests.contains_key(&key) || requests.len() >= self.lanes.len() + self.global.admin_capacity {
            return Err(Error::new(ErrorCode::Busy, "request identity unresolved or bounded attempt capacity exhausted"));
        }
        requests.insert(key, InFlight::Open);
        drop(requests);
        let active = ActiveRequest { group: self, request, retained: false };
        if self.db.outcome(request.into()).map_err(storage)?.is_some() {
            return Err(Error::new(ErrorCode::Invalid, "request already committed; query its recorded outcome"));
        }
        Ok(active)
    }
    fn timeout(&self) -> Duration { Duration::from_millis(self.global.limits.commit_ms) }
    fn submit(&self, work: Work) {
        match &work.request {
            Request::Begin { .. } => {
                let start = self.next_lane.fetch_add(1, Ordering::Relaxed);
                for offset in 0..self.lanes.len() {
                    let lane = &self.lanes[(start + offset) % self.lanes.len()];
                    if lane.busy.compare_exchange(false, true, Ordering::AcqRel, Ordering::Relaxed).is_ok() {
                        if let Err(error) = lane.send.try_send(work) {
                            lane.busy.store(false, Ordering::Release);
                            send_failure(error);
                        }
                        return;
                    }
                }
                work.reply.error(ErrorCode::Busy, "all owner-worker lanes are occupied");
            }
            Request::Execute { handle, .. } | Request::Commit { handle } | Request::Rollback { handle } => {
                if let Some(lane) = self.lanes.get(handle.lane) {
                    if let Err(error) = lane.send.try_send(work) { send_failure(error); }
                } else { work.reply.error(ErrorCode::InvalidHandle, "unknown owner lane"); }
            }
            _ => work.reply.error(ErrorCode::Invalid, "not a transaction lane operation"),
        }
    }
}
impl Drop for Group {
    fn drop(&mut self) { self.host.shutdown(); }
}
struct ActiveRequest<'a> { group: &'a Group, request: RequestId, retained: bool }
impl ActiveRequest<'_> {
    fn retain(&mut self, proposal: Option<Proposal>) {
        self.group.active.lock().insert((self.request.client, self.request.sequence),
            proposal.map(InFlight::Proposed).unwrap_or(InFlight::Unpositioned));
        self.retained = true;
    }
}
impl Drop for ActiveRequest<'_> {
    fn drop(&mut self) { if !self.retained { self.group.active.lock().remove(&(self.request.client, self.request.sequence)); } }
}
fn send_failure(error: TrySendError<Work>) {
    match error { TrySendError::Full(work) => work.reply.error(ErrorCode::Busy, "bounded command queue full"),
        TrySendError::Disconnected(work) => work.reply.error(ErrorCode::Closed, "owner worker stopped") }
}

fn lane_loop(group: Arc<Group>, lane: usize, receiver: Receiver<Work>, ready: SyncSender<Result<()>>) {
    let mut worker = match group.db.attach_worker() {
        Ok(worker) => { let _ = ready.send(Ok(())); worker }
        Err(error) => { let _ = ready.send(Err(storage(error))); return; }
    };
    while !group.stopped.load(Ordering::Acquire) {
        let work = match receiver.recv_timeout(Duration::from_millis(100)) { Ok(work) => work, Err(mpsc::RecvTimeoutError::Timeout) => continue, Err(_) => break };
        if let Request::Begin { request, .. } = work.request {
            run_attempt(&group, lane, &mut worker, &receiver, request, work.reply);
            group.lanes[lane].busy.store(false, Ordering::Release);
        } else { work.reply.error(ErrorCode::InvalidHandle, "transaction is closed or never existed"); }
    }
    while let Ok(work) = receiver.try_recv() { work.reply.error(ErrorCode::Closed, "worker shutdown"); }
}

fn run_attempt(group: &Group, lane: usize, worker: &mut sto_rocksdb::Worker, receiver: &Receiver<Work>, request: RequestId, reply: Reply) {
    let admitted = (|| {
        let term = group.term()?;
        group.host.read_barrier(term, group.timeout()).map_err(host_error)?;
        let active = group.reserve(request)?;
        Ok((term, active))
    })();
    let (term, _active) = match admitted {
        Ok(values) => values, Err(error) => { reply.send(Response::Error { error }); return; }
    };
    let mut transaction = match worker.begin() {
        Ok(transaction) => transaction,
        Err(error) => { reply.send(Response::Error { error: storage(error) }); return; }
    };
    let position = transaction.position();
    let nonce = group.nonce.fetch_add(1, Ordering::Relaxed);
    if nonce == u64::MAX { group.fenced.store(true, Ordering::Release); reply.error(ErrorCode::Closed, "handle identity exhausted"); return; }
    let handle = Handle { group: group.config.id, lane, nonce };
    reply.send(Response::Begun { handle, term, position: Position { index: position.index, term: position.term } });
    let mut poison: Option<Error> = None;
    // Only immutable catalog bindings are cached, never observations/intents.
    let mut scopes: HashMap<String, (u64, u64)> = HashMap::new();
    let mut operations = 0usize;
    let mut bytes = 0usize;
    let mut deadline = Instant::now() + Duration::from_millis(group.global.limits.idle_ms);
    loop {
        if group.stopped.load(Ordering::Acquire) { return; }
        let work = match receiver.recv_timeout(Duration::from_millis(100).min(deadline.saturating_duration_since(Instant::now()))) {
            Ok(work) => work,
            Err(mpsc::RecvTimeoutError::Timeout) if Instant::now() < deadline => continue,
            Err(_) => return,
        };
        let received_handle = match &work.request {
            Request::Execute { handle, .. } | Request::Commit { handle } | Request::Rollback { handle } => *handle,
            _ => { work.reply.error(ErrorCode::Busy, "owner lane already has an open transaction"); continue; }
        };
        if received_handle != handle { work.reply.error(ErrorCode::InvalidHandle, "stale transaction handle"); continue; }
        deadline = Instant::now() + Duration::from_millis(group.global.limits.idle_ms);
        match work.request {
            Request::Execute { group: requested_group, table, operation, .. } => {
                let result = if let Some(error) = &poison { Err(error.clone()) } else {
                    operations = operations.saturating_add(1);
                    bytes = bytes.saturating_add(operation.bytes()).saturating_add(table.len());
                    execute(group, &mut transaction, &mut scopes, requested_group, &table, &operation, operations, bytes)
                };
                if let Err(error) = &result { poison = Some(error.clone()); }
                work.reply.send(Response::from_result(result));
            }
            Request::Rollback { .. } => { transaction.rollback(); work.reply.send(Response::Ok); return; }
            Request::Commit { .. } => {
                if let Some(error) = poison { transaction.rollback(); work.reply.send(Response::Error { error }); return; }
                finish(group, transaction, term, request, handle, receiver, work.reply);
                return;
            }
            _ => unreachable!(),
        }
    }
}
fn execute(group: &Group, tx: &mut Transaction<'_>, scopes: &mut HashMap<String, (u64, u64)>, requested_group: u64, table: &str, operation: &Operation, operations: usize, bytes: usize) -> Result<Response> {
    if requested_group != group.config.id { return Err(Error::new(ErrorCode::CrossShard, "different user DB poisons the whole transaction")); }
    if operations > group.global.limits.transaction_operations || bytes > group.global.limits.transaction_bytes {
        return Err(Error::new(ErrorCode::TooLarge, "transaction operation/byte budget exceeded"));
    }
    group.global.limits.operation(operation)?;
    let (route, table) = match scopes.get(table) {
        Some(binding) => *binding,
        None => {
            let route = group.global.route_id(table)?;
            let id = group.db.lookup_table(table).map_err(storage)?
                .ok_or_else(|| Error::new(ErrorCode::UnknownTable, "table not created in this user DB"))?.id;
            scopes.insert(table.to_owned(), (route, id));
            (route, id)
        }
    };
    check_scope(&group.snapshot, route, group.config.id, operation)?;
    match operation {
        Operation::Get { key } => Ok(Response::Value { value: tx.get(table, key).map_err(storage)? }),
        Operation::Exists { key } => Ok(Response::Exists { exists: tx.exists(table, key).map_err(storage)? }),
        Operation::Put { key, value } => { tx.put(table, key, value).map_err(storage)?; Ok(Response::Ok) }
        Operation::Insert { key, value } => { tx.insert(table, key, value).map_err(storage)?; Ok(Response::Ok) }
        Operation::Delete { key } => { tx.delete(table, key).map_err(storage)?; Ok(Response::Ok) }
        Operation::Scan { start, end, limit } => Ok(Response::Rows { rows: tx.scan(table, start, end.as_deref(), *limit).map_err(storage)? }),
        Operation::ReverseScan { start, end, limit } => Ok(Response::Rows { rows: tx.reverse_scan(table, start, end.as_deref(), *limit).map_err(storage)? }),
    }
}

fn finish(group: &Group, transaction: Transaction<'_>, term: u64, request: RequestId, handle: Handle, receiver: &Receiver<Work>, reply: Reply) {
    // Acquiring the canonical write guards and hydrating storage happens before
    // the ordering gate. Only final certification + local admission are ordered.
    let mut prepared = match transaction.prepare(request.into()) { Ok(p) => p, Err(e) => { reply.send(Response::Error { error: storage(e) }); return; } };
    let proposal = {
        let _ordered = group.certification.lock();
        let certify = (|| {
            if group.term()? != term { return Err(Error::new(ErrorCode::NotLeader, "transaction leadership generation ended")); }
            prepared.certify().map_err(storage)?;
            Ok(())
        })();
        if let Err(error) = certify { drop(_ordered); reply.send(Response::Error { error }); return; }
        if prepared.is_read_only() {
            let result = prepared.complete_read_only().map(|position| Response::Committed { request, position: Position { index: position.index, term: position.term }, read_only: true }).map_err(storage);
            drop(_ordered);
            reply.send(Response::from_result(result)); return;
        }
        #[cfg(feature = "test-hooks")]
        group.machine.hooks.hit(crate::wire::TestStage::BeforeProposal, None);
        if let Err(error) = prepared.mark_submitted() { drop(_ordered); reply.send(Response::Error { error: storage(error) }); return; }
        let bytes = match prepared.take_bytes() {
            Ok(bytes) => bytes,
            Err(error) => {
                // No external append was attempted: authoritative local rejection.
                let _ = prepared.reject();
                drop(_ordered);
                reply.send(Response::Error { error: storage(error) }); return;
            }
        };
        group.host.propose(term, bytes)
    };
    match proposal {
        Ok(proposal) => {
            #[cfg(feature = "test-hooks")]
            group.machine.hooks.hit(crate::wire::TestStage::AfterAppend, Some(proposal.position.into()));
            resolve(group, prepared, Some(proposal), request, handle, receiver, reply);
        }
        Err(AdmissionError::Unknown) => {
            group.fenced.store(true, Ordering::Release);
            resolve(group, prepared, None, request, handle, receiver, reply);
        }
        Err(error) => {
            let rejection = prepared.reject();
            reply.send(Response::Error { error: rejection.err().map(storage).unwrap_or_else(|| admission(error)) });
        }
    }
}

fn resolve(group: &Group, mut prepared: Prepared<'_>, proposal: Option<Proposal>, request: RequestId, handle: Handle, receiver: &Receiver<Work>, reply: Reply) {
    let mut reply = Some(reply);
    let deadline = Instant::now() + group.timeout();
    let mut committed = false;
    let mut unknown = false;
    let mut apply_failed = false;
    loop {
        if group.stopped.load(Ordering::Acquire) { return; } // Drop quarantines possibly-submitted guards.
        let status = proposal.as_ref().map(|p| {
            if matches!(p.status(), ProposalStatus::Pending | ProposalStatus::Committed) {
                // Wake directly on the host's application notification. The
                // bounded timeout also services shutdown/cancellation mail.
                let _ = p.wait(Duration::from_millis(50));
            }
            p.status()
        }).unwrap_or(ProposalStatus::Unknown);
        match &status {
            ProposalStatus::Applied => {
                let result = (|| {
                    if !committed { prepared.mark_committed().map_err(committed_failure)?; }
                    let position = prepared.complete_applied().map_err(committed_failure)?;
                    Ok(Response::Committed { request, position: Position { index: position.index, term: position.term }, read_only: false })
                })();
                if result.is_err() { group.fenced.store(true, Ordering::Release); }
                #[cfg(feature = "test-hooks")]
                if result.is_ok() { group.machine.hooks.hit(crate::wire::TestStage::BeforeReply, proposal.as_ref().map(|p| p.position.into())); }
                if let Some(reply) = reply { reply.send(Response::from_result(result)); }
                return;
            }
            ProposalStatus::Discarded => {
                let result = prepared.reject().map_err(storage);
                if let Some(reply) = reply { reply.send(Response::Error { error: result.err().unwrap_or_else(|| Error::new(ErrorCode::Conflict, "uncommitted proposal was replaced")) }); }
                return;
            }
            ProposalStatus::Committed if !committed => {
                if let Err(error) = prepared.mark_committed() {
                    group.fenced.store(true, Ordering::Release);
                    if let Some(reply) = reply.take() { reply.send(Response::Error { error: committed_failure(error) }); }
                }
                committed = true;
            }
            ProposalStatus::StorageFailed(error) => {
                group.fenced.store(true, Ordering::Release);
                group.db.fence();
                if let Some(reply) = reply.take() { reply.error(ErrorCode::StorageFailure, error.clone()); }
                if !apply_failed {
                    // Generic host I/O failure does not itself prove a decision.
                    // The monotone per-proposal evidence survives a missed
                    // transient Committed notification.
                    if !committed && proposal.as_ref().is_some_and(Proposal::is_committed) {
                        committed = prepared.mark_committed().is_ok();
                    }
                    if committed { let _ = prepared.mark_apply_failed(); }
                    else { let _ = prepared.mark_unknown(); }
                    apply_failed = true;
                }
            }
            _ => {}
        }
        // Lost local admission results have no position handle. Only the
        // recorded outcome can resolve them; absence is never an abort proof.
        if proposal.is_none() {
            if let Ok(Some(outcome)) = group.db.outcome(request.into()) {
                let result = (|| {
                    prepared.mark_committed().map_err(committed_failure)?;
                    prepared.complete_applied().map_err(committed_failure)?;
                    Ok(Response::Committed { request, position: Position { index: outcome.position.index, term: outcome.position.term }, read_only: false })
                })();
                if let Some(reply) = reply { reply.send(Response::from_result(result)); }
                return;
            }
        }
        if (matches!(status, ProposalStatus::Unknown) || Instant::now() >= deadline) && !unknown {
            if !committed && !apply_failed { let _ = prepared.mark_unknown(); }
            unknown = true;
            if let Some(reply) = reply.take() { reply.error(ErrorCode::UnknownOutcome, "possibly submitted; use stable request identity for outcome lookup, never replay the body"); }
        }
        // The lane retains guards while host network/application threads make
        // progress. Reject cancellation but keep the outcome resolvable.
        let mail_timeout = if matches!(status, ProposalStatus::Pending | ProposalStatus::Committed) {
            Duration::ZERO
        } else { Duration::from_millis(50) };
        match receiver.recv_timeout(mail_timeout) {
            Ok(work) => {
                let matches = match &work.request { Request::Execute { handle: h, .. } | Request::Commit { handle: h } | Request::Rollback { handle: h } => *h == handle, _ => false };
                work.reply.error(if matches { ErrorCode::UnknownOutcome } else { ErrorCode::InvalidHandle }, "owner lane retains a possibly submitted transaction");
            }
            Err(mpsc::RecvTimeoutError::Disconnected) => return,
            Err(mpsc::RecvTimeoutError::Timeout) => {}
        }
    }
}

struct ServiceState { config: Arc<Config>, groups: BTreeMap<u64, Arc<Group>>, admin: SyncSender<Work>, stopped: Arc<AtomicBool> }
struct RpcService(Arc<ServiceState>);
impl Service for RpcService {
    fn __reg_to__(&mut self, server: &mut Server, index: usize) -> i32 { server.reg_fast_rpc(RPC_ID, index) }
    fn __dispatch__(&self, _: i32, request: Box<RpcRequest>, connection: WeakServerConnection) {
        let state = &self.0;
        // SRPC owns a whole [xid, rpc_id, args] frame; its archive cursor
        // already consumed the transport header before dispatching this service.
        let decoded = match request.body.get(request.src.pos()..) {
            Some(payload) if payload.len() <= state.config.limits.rpc_bytes =>
                serde_json::from_slice::<Envelope>(payload).map_err(|e| Error::new(ErrorCode::Invalid, e.to_string())),
            Some(_) => Err(Error::new(ErrorCode::TooLarge, "request exceeds RPC byte limit")),
            None => Err(Error::new(ErrorCode::Invalid, "invalid SRPC payload cursor")),
        };
        let reply = Reply::take(request, connection, state.config.limits.rpc_bytes);
        let envelope = match decoded { Ok(e) if e.version == PROTOCOL_VERSION => e,
            Ok(_) => { reply.error(ErrorCode::Invalid, "unsupported transaction protocol version"); return; }
            Err(error) => { reply.send(Response::Error { error }); return; } };
        let Some(group) = state.groups.get(&envelope.request.group()) else { reply.error(ErrorCode::UnknownGroup, "group is not hosted by this process"); return; };
        let work = Work { request: envelope.request, reply };
        match &work.request {
            Request::Begin { .. } | Request::Execute { .. } | Request::Commit { .. } | Request::Rollback { .. } => group.submit(work),
            Request::Status { .. } => work.reply.send(status_response(group)),
            #[cfg(feature = "test-hooks")]
            Request::TestGate { stage, paused, .. } => {
                let (paused, waiting, position) = group.machine.hooks.control(*stage, *paused);
                work.reply.send(Response::TestGate { paused, waiting, position });
            }
            #[cfg(feature = "test-hooks")]
            Request::TestIsolate { isolated, .. } => {
                group.host.set_isolated(*isolated);
                work.reply.send(Response::Ok);
            }
            _ => if let Err(error) = state.admin.try_send(work) { send_failure(error); },
        }
    }
}
fn status_response(group: &Group) -> Response {
    let status = group.host.status();
    let failure = status.failure.or_else(|| group.fenced.load(Ordering::Acquire).then(|| "service admission fenced".into()));
    Response::Status { group: group.config.id, node: group.global.node, term: status.term, leader: status.leader,
        leader_endpoint: status.leader.and_then(|id| group.config.peers.iter().find(|p| p.id == id).map(|p| p.client.clone())),
        is_leader: status.is_leader, committed: status.committed.index, applied: status.applied.index, failure,
        durability: group.global.durability, power_crash_durable: group.global.durability.sync_writes() }
}
fn admin(group: &Group, request: Request) -> Result<Response> {
    if let Request::Checkpoint { .. } = request { return group.host.checkpoint().map(|p| Response::Checkpoint { position: p.into() }).map_err(host_error); }
    #[cfg(feature = "test-hooks")]
    if let Request::TestLocalScan { table, start, end, limit, .. } = request {
        // Replica inspection only, never a production linearizable read API.
        // A real native snapshot lets recovery scenarios compare follower bytes.
        let operation = Operation::Scan { start, end: Some(end), limit };
        group.global.limits.operation(&operation)?;
        let Operation::Scan { start, end, limit } = &operation else { unreachable!() };
        let table = group.db.lookup_table(&table).map_err(storage)?.ok_or_else(|| Error::new(ErrorCode::UnknownTable, "table not found"))?;
        let mut worker = group.db.attach_worker().map_err(storage)?;
        let mut transaction = worker.begin().map_err(storage)?;
        let rows = transaction.scan(table.id, start, end.as_deref(), *limit).map_err(storage)?;
        transaction.rollback();
        return Ok(Response::Rows { rows });
    }
    let term = group.term()?;
    group.host.read_barrier(term, group.timeout()).map_err(host_error)?;
    match request {
        #[cfg(feature = "test-hooks")]
        Request::InjectApplyFailure { after_batch, .. } => {
            group.machine.fault.store(if after_batch { 2 } else { 1 }, Ordering::Release);
            Ok(Response::Ok)
        }
        Request::ListTables { .. } => Ok(Response::Tables { tables: group.db.list_tables().map_err(storage)?.into_iter().map(|t| (t.name, t.id)).collect() }),
        Request::LookupTable { name, .. } => Ok(Response::Table { table: group.db.lookup_table(&name).map_err(storage)?.map(|t| t.id) }),
        Request::Count { name, .. } => {
            let table = group.db.lookup_table(&name).map_err(storage)?.ok_or_else(|| Error::new(ErrorCode::UnknownTable, "table not found"))?;
            Ok(Response::Count { count: group.db.count(table.id).map_err(storage)? })
        }
        Request::Outcome { request, .. } => Ok(Response::Outcome { request, position: group.db.outcome(request.into()).map_err(storage)?.map(|o| Position { index: o.position.index, term: o.position.term }) }),
        Request::CreateTable { request, name, .. } => {
            group.global.route_id(&name)?;
            let mut active = group.reserve(request)?;
            let command = sto_rocksdb::Command::create_table(group.config.id, request.into(), name).map_err(storage)?;
            let bytes = command.encode().map_err(storage)?;
            let admitted = { let _gate = group.certification.lock();
                if group.term()? != term { return Err(Error::new(ErrorCode::NotLeader, "catalog leadership generation ended")); }
                group.host.propose(term, bytes) };
            let proposal = match admitted {
                Ok(proposal) => proposal,
                Err(AdmissionError::Unknown) => {
                    active.retain(None);
                    group.fenced.store(true, Ordering::Release);
                    return Err(Error::new(ErrorCode::UnknownOutcome, "catalog admission result lost; group fenced"));
                }
                Err(error) => return Err(admission(error)),
            };
            match proposal.wait(group.timeout()) {
                ProposalStatus::Applied => Ok(Response::Committed { request, position: proposal.position.into(), read_only: false }),
                ProposalStatus::Discarded => Err(Error::new(ErrorCode::Conflict, "catalog proposal discarded")),
                ProposalStatus::StorageFailed(e) => { active.retain(Some(proposal)); Err(Error::new(ErrorCode::StorageFailure, e)) }
                _ => { active.retain(Some(proposal)); Err(Error::new(ErrorCode::UnknownOutcome, "catalog proposal possibly accepted; query request outcome")) }
            }
        }
        _ => Err(Error::new(ErrorCode::Invalid, "invalid administrative operation")),
    }
}

/// Persists routing outside the Raft store, whose directory is atomically
/// created by raft-store. Recovery never invents a missing routing identity.
fn bind_routing(config: &Config, group: &GroupConfig, bootstrap: bool) -> Result<()> {
    let path = group.raft_path.with_added_extension("service-routing.json");
    let parent = path.parent().filter(|p| !p.as_os_str().is_empty()).unwrap_or_else(|| std::path::Path::new("."));
    let bytes = serde_json::to_vec(&config.routing).map_err(|e| Error::new(ErrorCode::Invalid, e.to_string()))?;
    if path.exists() {
        let previous = std::fs::read(&path).map_err(|e| Error::new(ErrorCode::StorageFailure, e.to_string()))?;
        if previous != bytes { return Err(Error::new(ErrorCode::Invalid, "persisted fixed routing identity differs; online migration is not enabled")); }
    } else {
        if !bootstrap { return Err(Error::new(ErrorCode::StorageFailure, "missing persisted routing identity; refusing recovery")); }
        std::fs::create_dir_all(parent).map_err(|e| Error::new(ErrorCode::StorageFailure, e.to_string()))?;
        let temporary = path.with_added_extension("tmp");
        let mut file = std::fs::File::create(&temporary).map_err(|e| Error::new(ErrorCode::StorageFailure, e.to_string()))?;
        file.write_all(&bytes).and_then(|_| file.sync_all()).map_err(|e| Error::new(ErrorCode::StorageFailure, e.to_string()))?;
        std::fs::rename(temporary, &path).and_then(|_| std::fs::File::open(parent)?.sync_all()).map_err(|e| Error::new(ErrorCode::StorageFailure, e.to_string()))?;
    }
    Ok(())
}

pub struct RunningServer { server: Option<Server>, poll: Arc<PollThread>, state: Arc<ServiceState>, threads: Vec<JoinHandle<()>> }
impl RunningServer {
    /// `bootstrap` is explicit. Normal restarts refuse to manufacture missing
    /// Raft recovery authority even if a user RocksDB happens to exist.
    pub fn start(config: Config, bootstrap: bool) -> Result<Self> {
        config.validate()?;
        let config = Arc::new(config);
        let stopped = Arc::new(AtomicBool::new(false));
        let snapshot = config.snapshot();
        if let Some(warning) = config.durability.warning() { eprintln!("{warning}"); }
        let (admin_send, admin_receive) = mpsc::sync_channel(config.admin_capacity);
        let mut groups = BTreeMap::new();
        let mut lane_receivers = Vec::new();
        let seed = SystemTime::now().duration_since(UNIX_EPOCH).map_err(|e| Error::new(ErrorCode::Invalid, e.to_string()))?.as_nanos() as u64;
        for gc in &config.groups {
            bind_routing(&config, gc, bootstrap)?;
            let db = Database::open(&gc.data_path, gc.id, config.durability.sync_writes()).map_err(storage)?;
            let machine = Arc::new(Machine { db: db.clone(),
                #[cfg(feature = "test-hooks")]
                fault: std::sync::atomic::AtomicU8::new(0),
                #[cfg(feature = "test-hooks")]
                hooks: crate::test_hooks::Hooks::default(),
            });
            let host = Host::start(raft_host::Config { group: gc.id, node: config.node,
                peers: gc.peers.iter().map(|p| raft_host::Peer { id: p.id, address: p.raft.clone() }).collect(),
                listen: gc.raft_listen.clone(), directory: gc.raft_path.clone(), create: bootstrap,
                sync_writes: config.durability.sync_writes(),
                max_command_bytes: raft_host::MAX_COMMAND_BYTES, ..Default::default() }, machine.clone()).map_err(host_error)?;
            db.reclaim_generations().map_err(storage)?;
            let mut lanes = Vec::new();
            for lane in 0..config.workers_per_group {
                let (send, receive) = mpsc::sync_channel(config.lane_capacity);
                lanes.push(Lane { send, busy: AtomicBool::new(false) });
                lane_receivers.push((gc.id, lane, receive));
            }
            groups.insert(gc.id, Arc::new(Group { config: gc.clone(), global: config.clone(), snapshot: snapshot.clone(), db, host,
                certification: Mutex::new(()), active: Mutex::new(HashMap::new()), lanes, next_lane: AtomicUsize::new(0),
                nonce: AtomicU64::new(seed), stopped: stopped.clone(), fenced: AtomicBool::new(false),
                #[cfg(feature = "test-hooks")]
                machine,
            }));
        }
        let state = Arc::new(ServiceState { config: config.clone(), groups, admin: admin_send, stopped });
        let poll = PollThread::create();
        let mut running = Self { server: None, poll: poll.clone(), state: state.clone(), threads: Vec::new() };
        for (id, lane, receive) in lane_receivers {
            let group = state.groups[&id].clone();
            let (ready, ready_receive) = mpsc::sync_channel(1);
            running.threads.push(thread::Builder::new().name(format!("txn-{id}-{lane}")).spawn(move || lane_loop(group, lane, receive, ready))
                .map_err(|e| Error::new(ErrorCode::Closed, e.to_string()))?);
            ready_receive.recv().map_err(|_| Error::new(ErrorCode::Closed, "owner-worker initialization failed"))??;
        }
        let receiver = Arc::new(Mutex::new(admin_receive));
        for index in 0..config.admin_workers {
            let state = state.clone(); let receiver = receiver.clone();
            running.threads.push(thread::Builder::new().name(format!("txn-admin-{index}")).spawn(move || {
                while !state.stopped.load(Ordering::Acquire) {
                    let work: Work = match receiver.lock().recv_timeout(Duration::from_millis(100)) {
                        Ok(work) => work, Err(mpsc::RecvTimeoutError::Timeout) => continue, Err(_) => break,
                    };
                    let result = admin(&state.groups[&work.request.group()], work.request);
                    work.reply.send(Response::from_result(result));
                }
            }).map_err(|e| Error::new(ErrorCode::Closed, e.to_string()))?);
        }
        let mut server = Server::new(Some(poll));
        server.reg_service(Box::new(RpcService(state)));
        let address = CString::new(config.listen.as_str()).map_err(|_| Error::new(ErrorCode::Invalid, "NUL in listen endpoint"))?;
        // SAFETY: the NUL-terminated address lives throughout synchronous start.
        let status = unsafe { server.start(address.as_ptr()) };
        if status != 0 { return Err(Error::new(ErrorCode::Transport, format!("SRPC listen failed: {status}"))); }
        running.server = Some(server);
        Ok(running)
    }
}
impl Drop for RunningServer {
    fn drop(&mut self) {
        self.state.stopped.store(true, Ordering::Release);
        #[cfg(feature = "test-hooks")]
        for group in self.state.groups.values() { group.machine.hooks.resume_all(); }
        self.server.take();
        for group in self.state.groups.values() { group.host.shutdown(); }
        for thread in self.threads.drain(..) { let _ = thread.join(); }
        self.poll.shutdown();
    }
}
