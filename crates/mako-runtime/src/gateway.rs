//! External sequential auto-commit gateway. Session tokens retain the existing
//! API's grouping behavior; each storage operation commits independently.
//! The existing verified Gateway owns sequencing and terminal deduplication.
use crate::{
    migration::Schema,
    rpc::{Network, Respond},
    storage::{engine, LogCallbacks, Store},
    wire::*,
};
use mako_sharding::{gateway as core, types::NO_SNAPSHOT};
use parking_lot::Mutex;
use std::{
    collections::BTreeMap,
    sync::{
        atomic::{AtomicBool, Ordering},
        mpsc::{self, SyncSender},
        Arc,
    },
    thread::{self, JoinHandle},
    time::Duration,
};
use sto_tpcc_ffi::native as sto;

pub struct Config {
    pub schema: Arc<Schema>,
    pub endpoints: BTreeMap<u32, String>,
    pub workers: usize,
}
enum Work {
    Operation(GatewayRequest, Respond),
    Route(RouteRequest, Respond),
}
struct State {
    store: Arc<Store>,
    schema: Arc<Schema>,
    endpoints: BTreeMap<u32, String>,
    network: Arc<Network>,
    admission: Mutex<core::Gateway>,
}
#[derive(Clone)]
pub(crate) struct Sender(SyncSender<Work>);
impl Sender {
    pub(crate) fn dispatch(&self, message: Message, respond: Respond) {
        let work = match message {
            Message::Gateway(request) => Work::Operation(request, respond),
            Message::Route(request) => Work::Route(request, respond),
            _ => {
                respond(Reply::Error(Error::new(
                    ErrorCode::Invalid,
                    "not a gateway message",
                )));
                return;
            }
        };
        if let Err(error) = self.0.try_send(work) {
            let work = match error {
                mpsc::TrySendError::Full(work) | mpsc::TrySendError::Disconnected(work) => work,
            };
            let respond = match work {
                Work::Operation(_, respond) | Work::Route(_, respond) => respond,
            };
            respond(Reply::Error(Error::new(
                ErrorCode::Busy,
                "gateway queue unavailable",
            )));
        }
    }
}
pub(crate) struct RunningGateway {
    sender: Option<Sender>,
    stopped: Arc<AtomicBool>,
    threads: Vec<JoinHandle<()>>,
}
impl RunningGateway {
    pub(crate) fn start(
        store: Arc<Store>,
        network: Arc<Network>,
        config: Config,
        capacity: usize,
    ) -> Result<Self> {
        if config.workers == 0 || config.workers > 64 {
            return Err(Error::new(
                ErrorCode::Invalid,
                "invalid gateway worker count",
            ));
        }
        let (send, receive) = mpsc::sync_channel::<Work>(capacity);
        let receive = Arc::new(Mutex::new(receive));
        let stopped = Arc::new(AtomicBool::new(false));
        let state = Arc::new(State {
            admission: Mutex::new(core::Gateway::new(store.owner)),
            store,
            schema: config.schema,
            endpoints: config.endpoints,
            network,
        });
        let mut running = Self {
            sender: Some(Sender(send)),
            stopped: stopped.clone(),
            threads: Vec::new(),
        };
        for index in 0..config.workers {
            let state = state.clone();
            let stopped = stopped.clone();
            let receive = receive.clone();
            let (ready, wait) = mpsc::sync_channel(1);
            running.threads.push(
                thread::Builder::new()
                    .name(format!("gateway-{}-{index}", state.store.owner))
                    .spawn(move || {
                        let mut worker = match state.store.database.worker().map_err(engine) {
                            Ok(worker) => worker,
                            Err(error) => {
                                let _ = ready.send(Err(error));
                                return;
                            }
                        };
                        if ready.send(Ok(())).is_err() {
                            return;
                        }
                        while !stopped.load(Ordering::Acquire) {
                            let work = match receive.lock().recv_timeout(Duration::from_millis(50))
                            {
                                Ok(work) => work,
                                Err(mpsc::RecvTimeoutError::Timeout) => continue,
                                Err(_) => break,
                            };
                            match work {
                                Work::Route(request, respond) => {
                                    respond(Reply::Route(state.route(request)))
                                }
                                Work::Operation(request, respond) => {
                                    respond(Reply::Gateway(state.operation(&mut worker, request)))
                                }
                            }
                        }
                        for work in receive.lock().try_iter() {
                            let respond = match work {
                                Work::Operation(_, respond) | Work::Route(_, respond) => respond,
                            };
                            respond(Reply::Error(Error::new(
                                ErrorCode::Closed,
                                "gateway stopped",
                            )));
                        }
                    })
                    .map_err(|e| Error::new(ErrorCode::Fatal, e.to_string()))?,
            );
            wait.recv()
                .map_err(|_| Error::new(ErrorCode::Fatal, "gateway worker startup failed"))??;
        }
        Ok(running)
    }
    pub(crate) fn sender(&self) -> Sender {
        self.sender.as_ref().expect("running gateway").clone()
    }
}
impl Drop for RunningGateway {
    fn drop(&mut self) {
        self.stopped.store(true, Ordering::Release);
        self.sender.take();
        for thread in self.threads.drain(..) {
            let _ = thread.join();
        }
    }
}
fn response(request: &GatewayRequest, reply: core::Reply) -> GatewayResponse {
    GatewayResponse {
        client: request.client,
        sequence: request.sequence,
        snapshot_version: request
            .target
            .as_ref()
            .and_then(|target| target.access.route)
            .map_or(NO_SNAPSHOT, |route| route.snapshot_version),
        status: reply.status,
        outcome: reply.outcome,
        op_result: reply.op_result,
        value: reply.value,
    }
}
fn terminal(status: i32, outcome: u32, op_result: bool, value: Vec<u8>) -> core::Reply {
    core::Reply {
        status,
        outcome,
        op_result,
        value,
    }
}
fn rejected() -> core::Reply {
    terminal(2, core::REJECTED, false, Vec::new())
}
impl State {
    fn route(&self, request: RouteRequest) -> Result<Target> {
        if request.key.len() > 64 || request.name.as_ref().is_some_and(|name| name.len() > 128) {
            return Err(Error::new(
                ErrorCode::Invalid,
                "route request exceeds gateway bounds",
            ));
        }
        let table = match (request.table, request.name) {
            (Some(table), None) => table,
            (None, Some(name)) => self.schema.named(&name)?,
            _ => {
                return Err(Error::new(
                    ErrorCode::Invalid,
                    "route requires one table identity",
                ))
            }
        };
        self.schema.target(
            TableKey {
                table,
                warehouse: request.warehouse,
            },
            &request.key,
            self.store.owner,
            self.store.migration,
        )
    }
    fn operation(&self, worker: &mut sto::Worker, request: GatewayRequest) -> GatewayResponse {
        if request.key.len() > 64 || request.value.len() > 8000 {
            return response(&request, rejected());
        }
        let decoded = match crate::gateway_client::intent(&request) {
            Ok(decoded) => decoded,
            Err(_) => return response(&request, rejected()),
        };
        if request.target.as_ref().is_some_and(|target| {
            self.schema.log_id(target.access.table).ok() != Some(target.log_id)
        }) {
            return response(&request, rejected());
        }
        if request.forwarded && decoded.owner != self.store.owner {
            return response(&request, rejected());
        }
        let admission = self.admission.lock().admit(decoded);
        let reply = match admission {
            core::Admission::Reply(reply) => reply,
            core::Admission::Execute => {
                let reply = if request
                    .target
                    .as_ref()
                    .is_some_and(|target| target.owner != self.store.owner)
                {
                    self.forward(&request)
                } else {
                    self.local(worker, &request)
                };
                self.admission
                    .lock()
                    .finish(request.client, request.sequence, reply.clone());
                reply
            }
        };
        response(&request, reply)
    }
    fn forward(&self, request: &GatewayRequest) -> core::Reply {
        let owner = request
            .target
            .as_ref()
            .expect("storage request has route")
            .owner;
        let Some(endpoint) = self.endpoints.get(&owner) else {
            return terminal(2, core::ABORTED, false, Vec::new());
        };
        let mut forwarded = request.clone();
        forwarded.session = 0;
        forwarded.forwarded = true;
        match self.network.call(
            endpoint,
            Message::Gateway(forwarded),
            Duration::from_secs(3),
        ) {
            Ok(Reply::Gateway(reply))
                if reply.client == request.client
                    && reply.sequence == request.sequence
                    && reply.snapshot_version
                        == request
                            .target
                            .as_ref()
                            .and_then(|target| target.access.route)
                            .map_or(NO_SNAPSHOT, |route| route.snapshot_version)
                    && reply.value.len() <= 8000
                    && matches!(
                        reply.outcome,
                        core::UNKNOWN | core::COMMITTED | core::ABORTED | core::REJECTED
                    ) =>
            {
                terminal(reply.status, reply.outcome, reply.op_result, reply.value)
            }
            Ok(Reply::Error(error))
                if matches!(error.code, ErrorCode::Busy | ErrorCode::Closed) =>
            {
                terminal(4, core::ABORTED, false, Vec::new())
            }
            // The verified gateway permits repeating the exact forwarded
            // request after a lost reply. The destination retains its receipt.
            _ => terminal(1, core::UNKNOWN, false, Vec::new()),
        }
    }
    fn local(&self, worker: &mut sto::Worker, request: &GatewayRequest) -> core::Reply {
        let Some(target) = &request.target else {
            return rejected();
        };
        // Retry only a definite pre-install conflict. This is a single storage
        // operation and its retained gateway identity survives every attempt.
        for _ in 0..16 {
            let protection = match self.store.protect() {
                Ok(pin) => pin,
                Err(error) => return failed(error, false),
            };
            if let Err(error) = protection.point(&target.access, &request.key) {
                return failed(error, false);
            }
            let table = match self.store.table(target.access.table) {
                Ok(table) => table,
                Err(error) => return failed(error, false),
            };
            let mut participant =
                match worker.begin_participant(self.store.term.load(Ordering::Acquire)) {
                    Ok(p) => p,
                    Err(error) => return failed(engine(error), false),
                };
            let result: Result<(bool, Vec<u8>)> = match request.kind {
                core::GET => participant
                    .get(&table.tree, &request.key)
                    .map(|row| (row.value.is_some(), row.value.unwrap_or_default()))
                    .map_err(engine),
                core::PUT | core::INSERT | core::DELETE => {
                    let mutation = match request.kind {
                        core::PUT => sto::Mutation::Put,
                        core::INSERT => sto::Mutation::Insert,
                        _ => sto::Mutation::Remove,
                    };
                    participant
                        .mutate(
                            &table.tree,
                            table.log_id,
                            mutation,
                            &request.key,
                            &request.value,
                            false,
                        )
                        .map(|changed| (changed, Vec::new()))
                        .map_err(engine)
                }
                _ => Err(Error::new(
                    ErrorCode::Invalid,
                    "unsupported gateway operation",
                )),
            };
            let (changed, value) = match result {
                Ok(result) if result.1.len() <= 8000 => result,
                Ok(_) => {
                    drop(participant);
                    return terminal(2, core::ABORTED, false, Vec::new());
                }
                Err(error) => {
                    drop(participant);
                    if error.code == ErrorCode::Conflict {
                        continue;
                    }
                    return failed(error, false);
                }
            };
            match participant
                .commit(0, &mut LogCallbacks(&*self.store.log))
                .map_err(engine)
            {
                Ok(_) => {
                    return terminal(
                        if request.kind == core::GET && !changed {
                            3
                        } else {
                            0
                        },
                        core::COMMITTED,
                        changed,
                        value,
                    )
                }
                Err(error) if error.code == ErrorCode::Conflict => continue,
                Err(error) => {
                    let uncertain = matches!(
                        error.code,
                        ErrorCode::Unknown | ErrorCode::CommittedFailure | ErrorCode::Fatal
                    );
                    if uncertain {
                        self.store.quarantine(protection);
                    }
                    return failed(error, uncertain);
                }
            }
        }
        terminal(4, core::ABORTED, false, Vec::new())
    }
}
fn failed(error: Error, uncertain: bool) -> core::Reply {
    let unknown =
        uncertain || matches!(error.code, ErrorCode::Unknown | ErrorCode::CommittedFailure);
    terminal(
        match error.code {
            ErrorCode::Busy | ErrorCode::Conflict => 4,
            ErrorCode::RouteChanged => 3,
            _ => 2,
        },
        if unknown {
            core::UNKNOWN
        } else {
            core::ABORTED
        },
        false,
        Vec::new(),
    )
}
