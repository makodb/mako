use crate::{
    admission::{Admission, Phase, Session},
    rpc::{Network, Respond, RpcServer},
    storage::{engine, LogCallbacks, Protection, Store},
    wire::*,
};
use std::{
    sync::{
        atomic::{AtomicBool, Ordering},
        mpsc::{self, Receiver, SyncSender},
        Arc,
    },
    thread::{self, JoinHandle},
    time::Duration,
};
use sto_tpcc_ffi::native as sto;

struct Work {
    request: Request,
    respond: Respond,
}
struct Lane {
    worker: sto::Worker,
    store: Arc<Store>,
    session: Session,
    protection: Option<Protection>,
    retained: bool,
}
impl Lane {
    fn process(&mut self, request: Request) -> Response {
        let admission = self
            .session
            .admit(&request, self.store.term.load(Ordering::Acquire));
        if let Admission::Reply(response) = admission {
            return response;
        }
        let result = self.execute(&request, matches!(admission, Admission::Begin));
        let response = Response {
            identity: request.identity,
            result,
        };
        let response = self.session.record(request, response);
        // Unknown holds its epoch even after local native cleanup. Migration
        // cannot mistake an unresolved INSTALL for a drained participant.
        if matches!(self.session.phase(), Phase::Aborted | Phase::Committed) {
            self.protection.take();
        }
        response
    }
    fn execute(&mut self, request: &Request, begin: bool) -> Result<Value> {
        if begin {
            if self.retained {
                self.worker.abort().map_err(engine)?;
                self.retained = false;
            }
            self.protection.take();
            self.store
                .term
                .fetch_max(request.identity.term, Ordering::AcqRel);
            if request.action == Action::Abort {
                return Ok(Value::Done);
            }
            self.protection = Some(self.store.protect()?);
            self.worker
                .begin_participant(request.identity.term)
                .map_err(engine)?
                .retain();
            self.retained = true;
        }
        if request.action == Action::Abort {
            let result = self.worker.abort().map_err(engine);
            self.retained = false;
            return result.map(|_| Value::Done);
        }
        let mut participant = self.worker.resume_participant().map_err(engine)?;
        let protection = self
            .protection
            .as_ref()
            .expect("admitted participant has epoch");
        let result = execute(&self.store, protection, &mut participant, &request.action);
        match (&request.action, result) {
            (Action::Install { .. }, Ok(value)) => {
                let log = participant.submit_log(0, &mut LogCallbacks(&*self.store.log));
                self.retained = false;
                if let Err(error) = log {
                    drop(participant);
                    return Err(engine(error));
                }
                participant.finish(true).map_err(engine)?;
                Ok(value)
            }
            (_, Ok(value)) => {
                participant.retain();
                Ok(value)
            }
            (_, Err(error)) => {
                // Drop contains local cleanup. INSTALL failures are quarantined
                // by Session even when cleanup finds no installed local write.
                drop(participant);
                self.retained = false;
                Err(error)
            }
        }
    }
}
impl Drop for Lane {
    fn drop(&mut self) {
        let _ = self.worker.abort();
        self.protection.take();
    }
}
fn execute(
    store: &Store,
    protection: &Protection,
    participant: &mut sto::Participant<'_>,
    action: &Action,
) -> Result<Value> {
    match action {
        Action::Get { access, key } => {
            protection.point(access, key)?;
            let table = store.table(access.table)?;
            let result = participant.get(&table.tree, key).map_err(engine)?;
            Ok(Value::Point {
                value: result.value,
                time_term: result.time_term,
            })
        }
        Action::Scan {
            access,
            lower,
            upper,
            reverse,
            limit,
        } => {
            protection.range(access, lower, upper)?;
            let table = store.table(access.table)?;
            let result = participant
                .scan(
                    &table.tree,
                    lower.as_ref(),
                    upper.as_ref(),
                    *reverse,
                    *limit,
                )
                .map_err(engine)?;
            let value = Value::Scan {
                rows: result
                    .rows
                    .into_iter()
                    .map(|r| Row {
                        key: r.key,
                        value: r.value,
                        time_term: r.time_term,
                    })
                    .collect(),
                read_dependency: result.read_dependency,
            };
            // Refuse before terminal effects; the complete response adds a small
            // fixed identity/envelope overhead to the encoded value.
            if encode(&value)?.len() > MAX_BYTES - 256 {
                return Err(Error::new(
                    ErrorCode::Capacity,
                    "scan response exceeds RPC limit",
                ));
            }
            Ok(value)
        }
        Action::Stage { writes } => {
            for write in writes {
                protection.point(&write.access, &write.key)?;
                let table = store.table(write.access.table)?;
                let mutation = match write.mutation {
                    Mutation::Put => sto::Mutation::Put,
                    Mutation::Insert => sto::Mutation::Insert,
                    Mutation::Remove => sto::Mutation::Remove,
                };
                if !participant
                    .mutate(
                        &table.tree,
                        table.log_id,
                        mutation,
                        &write.key,
                        &write.value,
                        false,
                    )
                    .map_err(engine)?
                {
                    return Err(Error::new(
                        ErrorCode::Conflict,
                        "remote insert/delete precondition failed",
                    ));
                }
            }
            participant.lock().map_err(engine)?;
            Ok(Value::Done)
        }
        Action::Timestamp => participant
            .timestamp()
            .map(Value::Timestamp)
            .map_err(engine),
        Action::Validate => {
            participant.validate().map_err(engine)?;
            Ok(Value::Done)
        }
        Action::Install { timestamp } => {
            participant.install(*timestamp).map_err(engine)?;
            Ok(Value::Done)
        }
        Action::Abort => unreachable!("abort handled before borrowing participant"),
    }
}

pub struct RunningShard {
    server: Option<RpcServer>,
    stopped: Arc<AtomicBool>,
    lanes: Vec<SyncSender<Work>>,
    threads: Vec<JoinHandle<()>>,
    // Own the network until the server and every affine worker have stopped.
    _network: Arc<Network>,
}
impl RunningShard {
    pub fn start(
        store: Arc<Store>,
        network: Arc<Network>,
        address: &str,
        nodes: u32,
        workers_per_node: u32,
        queue_capacity: usize,
    ) -> Result<Self> {
        let lane_count = nodes
            .checked_mul(workers_per_node)
            .filter(|n| *n > 0 && *n <= 256)
            .ok_or_else(|| Error::new(ErrorCode::Invalid, "invalid participant lane count"))?;
        if queue_capacity == 0 {
            return Err(Error::new(ErrorCode::Invalid, "empty participant queue"));
        }
        let stopped = Arc::new(AtomicBool::new(false));
        let mut running = Self {
            server: None,
            stopped: stopped.clone(),
            lanes: Vec::new(),
            threads: Vec::new(),
            _network: network.clone(),
        };
        for lane in 0..lane_count {
            let (send, receive) = mpsc::sync_channel(queue_capacity);
            let (ready, wait) = mpsc::sync_channel(1);
            let store = store.clone();
            let stopped = stopped.clone();
            running.threads.push(
                thread::Builder::new()
                    .name(format!("shard-{}-{lane}", store.owner))
                    .spawn(move || lane_loop(store, stopped, receive, ready))
                    .map_err(|e| Error::new(ErrorCode::Fatal, e.to_string()))?,
            );
            running.lanes.push(send);
            wait.recv().map_err(|_| {
                Error::new(ErrorCode::Fatal, "participant worker initialization failed")
            })??;
        }
        let lanes = running.lanes.clone();
        running.server = Some(RpcServer::start(
            &network,
            address,
            move |message, respond| match message {
                Message::Participant(request) => {
                    if stopped.load(Ordering::Acquire) {
                        respond(Reply::Participant(Response::error(
                            request.identity,
                            ErrorCode::Closed,
                            "shard is stopping",
                        )));
                        return;
                    }
                    let stream = request.stream;
                    if stream.node >= nodes || stream.worker >= workers_per_node {
                        respond(Reply::Participant(Response::error(
                            request.identity,
                            ErrorCode::Invalid,
                            "participant stream outside configured lanes",
                        )));
                        return;
                    }
                    let lane = (stream.node * workers_per_node + stream.worker) as usize;
                    if let Err(error) = lanes[lane].try_send(Work { request, respond }) {
                        let (work, code) = match error {
                            mpsc::TrySendError::Full(work) => (work, ErrorCode::Busy),
                            mpsc::TrySendError::Disconnected(work) => (work, ErrorCode::Closed),
                        };
                        (work.respond)(Reply::Participant(Response::error(
                            work.request.identity,
                            code,
                            "participant lane unavailable",
                        )));
                    }
                }
                Message::Migration { .. } => respond(Reply::Error(Error::new(
                    ErrorCode::Invalid,
                    "migration host has not started",
                ))),
            },
        )?);
        Ok(running)
    }
}
fn lane_loop(
    store: Arc<Store>,
    stopped: Arc<AtomicBool>,
    receive: Receiver<Work>,
    ready: SyncSender<Result<()>>,
) {
    let worker = match store.database.worker().map_err(engine) {
        Ok(worker) => worker,
        Err(error) => {
            let _ = ready.send(Err(error));
            return;
        }
    };
    let mut lane = Lane {
        worker,
        store,
        session: Session::default(),
        protection: None,
        retained: false,
    };
    if ready.send(Ok(())).is_err() {
        return;
    }
    while !stopped.load(Ordering::Acquire) {
        match receive.recv_timeout(Duration::from_millis(50)) {
            Ok(work) => (work.respond)(Reply::Participant(lane.process(work.request))),
            Err(mpsc::RecvTimeoutError::Timeout) => (),
            Err(mpsc::RecvTimeoutError::Disconnected) => break,
        }
    }
    for work in receive.try_iter() {
        (work.respond)(Reply::Participant(Response::error(
            work.request.identity,
            ErrorCode::Closed,
            "shard stopped",
        )));
    }
}
impl Drop for RunningShard {
    fn drop(&mut self) {
        self.stopped.store(true, Ordering::Release);
        if let Some(mut server) = self.server.take() {
            server.shutdown();
        }
        self.lanes.clear();
        for thread in self.threads.drain(..) {
            let _ = thread.join();
        }
    }
}
