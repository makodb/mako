//! One service owns local and remote participants through prepare and recovery.
use crate::{
    prepared::{self, NativeEngine},
    rpc::{Network, Respond, RpcServer},
    storage::Store,
    wire::*,
};
use mako_protocol::{contracts::Transport, journal::Entry, participant::Lane};
use parking_lot::Mutex;
use std::{
    collections::BTreeMap,
    sync::{
        atomic::{AtomicBool, Ordering},
        mpsc::{self, Receiver, SyncSender},
        Arc, Weak,
    },
    thread::{self, JoinHandle},
    time::Duration,
};

enum WorkKind {
    Participant(Request),
    Transaction(TransactionRequest),
    Open(u64),
    Restore(Entry),
}
struct Work {
    request: WorkKind,
    respond: Respond,
}
impl Work {
    fn reject(self, code: ErrorCode, message: &str) {
        (self.respond)(Reply::Error(Error::new(code, message)));
    }
}
struct Actor {
    sender: SyncSender<Work>,
    thread: Option<JoinHandle<()>>,
}
struct Dispatch {
    message: Option<Message>,
    respond: Respond,
}
struct Listener {
    stop: SyncSender<()>,
    thread: Option<JoinHandle<()>>,
}

pub(crate) struct ParticipantService {
    store: Weak<Store>,
    network: Arc<Network>,
    stopped: Arc<AtomicBool>,
    sender: Option<SyncSender<Dispatch>>,
    control: Mutex<Option<JoinHandle<()>>>,
    actors: Mutex<BTreeMap<(u32, u32), Actor>>,
    queue_capacity: usize,
    listener: Mutex<Option<Listener>>,
}
impl ParticipantService {
    pub(crate) fn for_store(store: &Arc<Store>, network: Arc<Network>) -> Result<Arc<Self>> {
        Self::with_capacity(store, network, 256)
    }
    fn with_capacity(
        store: &Arc<Store>,
        network: Arc<Network>,
        queue_capacity: usize,
    ) -> Result<Arc<Self>> {
        store
            .participants
            .get_or_init(|| Self::start(store, network, queue_capacity))
            .clone()
    }
    fn start(
        store: &Arc<Store>,
        network: Arc<Network>,
        queue_capacity: usize,
    ) -> Result<Arc<Self>> {
        let (sender, receive) = mpsc::sync_channel::<Dispatch>(queue_capacity);
        let service = Arc::new(Self {
            store: Arc::downgrade(store),
            network,
            stopped: Arc::new(AtomicBool::new(false)),
            sender: Some(sender),
            control: Mutex::new(None),
            actors: Mutex::new(BTreeMap::new()),
            queue_capacity,
            listener: Mutex::new(None),
        });
        let weak = Arc::downgrade(&service);
        let stopped = service.stopped.clone();
        let thread = thread::Builder::new()
            .name(format!("shard-{}-control", store.owner))
            .spawn(move || {
                let mut recovered_term = None;
                while !stopped.load(Ordering::Acquire) {
                    let work = match receive.recv_timeout(Duration::from_millis(100)) {
                        Ok(work) => work,
                        Err(mpsc::RecvTimeoutError::Timeout) => continue,
                        Err(mpsc::RecvTimeoutError::Disconnected) => break,
                    };
                    let Some(service) = weak.upgrade() else {
                        break;
                    };
                    let Some(store) = service.store.upgrade() else {
                        break;
                    };
                    let result = (|| {
                        store.health()?;
                        let status = store.log.replica_status(store.owner);
                        // Discovery's nonblocking `healthy` flag may be
                        // false while the apply mutex is merely occupied.
                        // This worker already performed the authoritative
                        // blocking health check above.
                        if !status.leader {
                            return Err(Error::new(
                                ErrorCode::RouteChanged,
                                "participant authority unavailable",
                            ));
                        }
                        if recovered_term != Some(status.term) {
                            service.restore(&store)?;
                            recovered_term = Some(status.term);
                        }
                        Ok(())
                    })();
                    if let Err(error) = result {
                        (work.respond)(Reply::Error(error));
                        continue;
                    }
                    let Some(message) = work.message else {
                        (work.respond)(Reply::OpenStream(Ok(0)));
                        continue;
                    };
                    match message {
                        Message::Checkpoint { owner } => {
                            let position = if owner == store.owner {
                                store.log.checkpoint()
                            } else {
                                Err(Error::new(
                                    ErrorCode::Invalid,
                                    "checkpoint sent to another shard",
                                ))
                            };
                            (work.respond)(Reply::Checkpoint {
                                owner: store.owner,
                                position,
                            });
                        }
                        Message::Participant(request) => service.enqueue(
                            request.stream,
                            Work {
                                request: WorkKind::Participant(request),
                                respond: work.respond,
                            },
                        ),
                        Message::Transaction(request) => service.enqueue(
                            Stream {
                                node: request.id.coordinator,
                                worker: request.id.worker,
                            },
                            Work {
                                request: WorkKind::Transaction(request),
                                respond: work.respond,
                            },
                        ),
                        Message::OpenStream { stream, nonce } => service.enqueue(
                            stream,
                            Work {
                                request: WorkKind::Open(nonce),
                                respond: work.respond,
                            },
                        ),
                        Message::TransactionOutcome {
                            transaction,
                            resolve,
                        } => {
                            let outcome = prepared::local_decision(&store, &transaction, resolve);
                            (work.respond)(Reply::TransactionOutcome {
                                id: transaction.id,
                                owner: store.owner,
                                outcome,
                            });
                        }
                        _ => (work.respond)(Reply::Error(Error::new(
                            ErrorCode::Invalid,
                            "unsupported participant service request",
                        ))),
                    }
                }
                for work in receive.try_iter() {
                    (work.respond)(Reply::Error(Error::new(ErrorCode::Closed, "shard stopped")));
                }
            })
            .map_err(|e| Error::new(ErrorCode::Fatal, e.to_string()))?;
        *service.control.lock() = Some(thread);
        Ok(service)
    }
    pub(crate) fn endpoint(self: &Arc<Self>, store: &Arc<Store>) -> Result<String> {
        if let Some(endpoint) = store.endpoint.read().clone() {
            return Ok(endpoint);
        }
        let mut listener = self.listener.lock();
        if let Some(endpoint) = store.endpoint.read().clone() {
            return Ok(endpoint);
        }
        let weak = Arc::downgrade(self);
        let network = self.network.clone();
        let (stop, stopped) = mpsc::sync_channel(1);
        let (ready, started) = mpsc::sync_channel(1);
        let thread = thread::Builder::new()
            .name(format!("shard-{}-listener", store.owner))
            .spawn(move || {
                let server = RpcServer::start(&network, "127.0.0.1:0", move |message, respond| {
                    if let Some(service) = weak.upgrade() {
                        if matches!(message, Message::ReplicaStatus) {
                            if let Some(store) = service.store.upgrade() {
                                respond(Reply::ReplicaStatus(
                                    store.log.replica_status(store.owner),
                                ));
                            } else {
                                respond(Reply::Error(Error::new(
                                    ErrorCode::Closed,
                                    "shard stopped",
                                )));
                            }
                        } else {
                            service.dispatch(message, respond);
                        }
                    } else {
                        respond(Reply::Error(Error::new(ErrorCode::Closed, "shard stopped")));
                    }
                });
                match server {
                    Ok(mut server) => {
                        if ready.send(Ok(server.address().to_owned())).is_ok() {
                            let _ = stopped.recv();
                        }
                        server.shutdown();
                    }
                    Err(error) => {
                        let _ = ready.send(Err(error));
                    }
                }
            })
            .map_err(|error| Error::new(ErrorCode::Fatal, error.to_string()))?;
        let endpoint = started
            .recv()
            .map_err(|_| Error::new(ErrorCode::Fatal, "local listener failed"))??;
        *store.endpoint.write() = Some(endpoint.clone());
        *listener = Some(Listener {
            stop,
            thread: Some(thread),
        });
        Ok(endpoint)
    }
    fn actor(&self, stream: Stream) -> Result<SyncSender<Work>> {
        let mut actors = self.actors.lock();
        if let Some(actor) = actors.get(&(stream.node, stream.worker)) {
            return Ok(actor.sender.clone());
        }
        if actors.len() >= 256 {
            return Err(Error::new(ErrorCode::Capacity, "participant worker limit"));
        }
        let store = self
            .store
            .upgrade()
            .ok_or_else(|| Error::new(ErrorCode::Closed, "shard stopped"))?;
        let (sender, receive) = mpsc::sync_channel(self.queue_capacity);
        let (ready, started) = mpsc::sync_channel(1);
        let network = self.network.clone();
        let stopped = self.stopped.clone();
        let thread = thread::Builder::new()
            .name(format!(
                "shard-{}-{}-{}",
                store.owner, stream.node, stream.worker
            ))
            .spawn(move || lane_loop(store, network, stopped, receive, ready))
            .map_err(|e| Error::new(ErrorCode::Fatal, e.to_string()))?;
        let startup = started.recv().map_err(|_| {
            Error::new(ErrorCode::Fatal, "participant worker initialization failed")
        })?;
        if let Err(error) = startup {
            let _ = thread.join();
            return Err(error);
        }
        actors.insert(
            (stream.node, stream.worker),
            Actor {
                sender: sender.clone(),
                thread: Some(thread),
            },
        );
        Ok(sender)
    }
    fn restore(&self, store: &Store) -> Result<()> {
        let entries = store.journal.entries()?;
        let mut streams = BTreeMap::new();
        // Restore every unresolved write lock before any foreground request.
        for entry in entries.iter().filter(|entry| entry.unresolved()) {
            let id = entry.transaction.id;
            if streams.insert((id.coordinator, id.worker), id).is_some() {
                return Err(Error::new(
                    ErrorCode::Unknown,
                    "multiple prepared attempts own one participant stream",
                ));
            }
            let sender = self.actor(Stream {
                node: id.coordinator,
                worker: id.worker,
            })?;
            let (send, receive) = mpsc::sync_channel(1);
            sender
                .send(Work {
                    request: WorkKind::Restore(entry.clone()),
                    respond: Box::new(move |reply| {
                        let _ = send.send(reply);
                    }),
                })
                .map_err(|_| Error::new(ErrorCode::Closed, "recovery worker stopped"))?;
            match receive
                .recv()
                .map_err(|_| Error::new(ErrorCode::Closed, "recovery worker stopped"))?
            {
                Reply::OpenStream(Ok(_)) => (),
                Reply::Error(error) | Reply::OpenStream(Err(error)) => return Err(error),
                _ => return Err(Error::new(ErrorCode::Fatal, "invalid recovery reply")),
            }
        }
        // A new authority may resolve a registration which never reached local
        // Prepare. CAS preserves an earlier Commit and fences its old writer.
        for entry in entries
            .iter()
            .filter(|entry| entry.begun && entry.decision.is_none())
        {
            prepared::local_decision(store, &entry.transaction, true)?;
        }
        Ok(())
    }
    fn enqueue(&self, stream: Stream, work: Work) {
        if self.stopped.load(Ordering::Acquire) {
            work.reject(ErrorCode::Closed, "participant service stopped");
            return;
        }
        let sender = match self.actor(stream) {
            Ok(sender) => sender,
            Err(error) => {
                (work.respond)(Reply::Error(error));
                return;
            }
        };
        if let Err(error) = sender.try_send(work) {
            let (work, code) = match error {
                mpsc::TrySendError::Full(work) => (work, ErrorCode::Busy),
                mpsc::TrySendError::Disconnected(work) => (work, ErrorCode::Closed),
            };
            work.reject(code, "participant lane unavailable");
        }
    }
    pub(crate) fn dispatch(&self, message: Message, respond: Respond) {
        self.send(Dispatch {
            message: Some(message),
            respond,
        });
    }
    pub(crate) fn recover(&self) -> Result<()> {
        let (send, receive) = mpsc::sync_channel(1);
        self.send(Dispatch {
            message: None,
            respond: Box::new(move |reply| {
                let _ = send.send(reply);
            }),
        });
        match receive
            .recv_timeout(Duration::from_secs(10))
            .map_err(|_| Error::new(ErrorCode::Unknown, "recovery admission deadline"))?
        {
            Reply::OpenStream(Ok(_)) => Ok(()),
            Reply::Error(error) => Err(error),
            _ => Err(Error::new(
                ErrorCode::Fatal,
                "invalid recovery admission reply",
            )),
        }
    }
    fn send(&self, work: Dispatch) {
        if self.stopped.load(Ordering::Acquire) {
            (work.respond)(Reply::Error(Error::new(ErrorCode::Closed, "shard stopped")));
            return;
        }
        if let Err(error) = self.sender.as_ref().expect("live service").try_send(work) {
            let (work, code) = match error {
                mpsc::TrySendError::Full(work) => (work, ErrorCode::Busy),
                mpsc::TrySendError::Disconnected(work) => (work, ErrorCode::Closed),
            };
            (work.respond)(Reply::Error(Error::new(
                code,
                "participant control unavailable",
            )));
        }
    }
    pub(crate) fn shutdown(&self) {
        self.stopped.store(true, Ordering::Release);
        if let Some(mut listener) = self.listener.lock().take() {
            drop(listener.stop);
            if let Some(thread) = listener.thread.take() {
                if thread.thread().id() != thread::current().id() {
                    let _ = thread.join();
                }
            }
        }
        let current = thread::current().id();
        if let Some(thread) = self.control.lock().take() {
            if thread.thread().id() != current {
                let _ = thread.join();
            }
        }
        let actors = std::mem::take(&mut *self.actors.lock());
        for (_, mut actor) in actors {
            drop(actor.sender);
            if let Some(thread) = actor.thread.take() {
                if thread.thread().id() != current {
                    let _ = thread.join();
                }
            }
        }
    }
}
impl Drop for ParticipantService {
    fn drop(&mut self) {
        self.shutdown();
    }
}

pub(crate) struct LocalTransport {
    pub(crate) service: Arc<ParticipantService>,
    pub(crate) network: Arc<Network>,
    pub(crate) endpoint: String,
    pub(crate) owner: u32,
}
impl Transport for LocalTransport {
    fn find_leader(&self, owner: u32, replicas: &ReplicaSet, timeout: Duration) -> Result<String> {
        if owner == self.owner {
            Ok(self.endpoint.clone())
        } else {
            self.network.find_leader(owner, replicas, timeout)
        }
    }
    fn call(&self, endpoint: &str, message: Message, timeout: Duration) -> Result<Reply> {
        if endpoint != self.endpoint {
            return self.network.call(endpoint, message, timeout);
        }
        let (send, receive) = mpsc::sync_channel(1);
        self.service.dispatch(
            message,
            Box::new(move |reply| {
                let _ = send.send(reply);
            }),
        );
        receive
            .recv_timeout(timeout)
            .map_err(|_| Error::new(ErrorCode::Transport, "local participant deadline elapsed"))
    }
}
fn fresh_lease() -> Result<u64> {
    use std::io::Read;
    let mut bytes = [0; 8];
    std::fs::File::open("/dev/urandom")
        .and_then(|mut file| file.read_exact(&mut bytes))
        .map_err(|error| Error::new(ErrorCode::Fatal, format!("stream lease entropy: {error}")))?;
    Ok(u64::from_ne_bytes(bytes).max(1))
}
fn lane_loop(
    store: Arc<Store>,
    network: Arc<Network>,
    stopped: Arc<AtomicBool>,
    receive: Receiver<Work>,
    ready: SyncSender<Result<()>>,
) {
    let engine = match NativeEngine::new(&store, network) {
        Ok(engine) => engine,
        Err(error) => {
            let _ = ready.send(Err(error));
            return;
        }
    };
    drop(store);
    let mut lane = Lane::new(engine);
    if ready.send(Ok(())).is_err() {
        return;
    }
    while !stopped.load(Ordering::Acquire) {
        match receive.recv_timeout(Duration::from_millis(100)) {
            Ok(work) => {
                let reply = match work.request {
                    WorkKind::Participant(request) => Reply::Participant(lane.process(request)),
                    WorkKind::Transaction(request) => {
                        Reply::Transaction(prepared::transaction(&mut lane, request))
                    }
                    WorkKind::Open(nonce) => {
                        Reply::OpenStream(fresh_lease().and_then(|lease| lane.claim(nonce, lease)))
                    }
                    WorkKind::Restore(entry) => {
                        let result = lane.engine_mut().restore(entry);
                        if result.is_ok() {
                            lane.seal_external();
                        }
                        Reply::OpenStream(result.map(|()| 0))
                    }
                };
                (work.respond)(reply);
            }
            Err(mpsc::RecvTimeoutError::Timeout) => prepared::resolve_idle(&mut lane),
            Err(mpsc::RecvTimeoutError::Disconnected) => break,
        }
    }
    for work in receive.try_iter() {
        work.reject(ErrorCode::Closed, "shard stopped");
    }
}

pub struct RunningShard {
    server: Option<RpcServer>,
    gateway: Option<crate::gateway::RunningGateway>,
    stopped: Arc<AtomicBool>,
    _service: Arc<ParticipantService>,
    _network: Arc<Network>,
    _store: Arc<Store>,
}
impl RunningShard {
    pub fn address(&self) -> &str {
        self.server.as_ref().expect("running listener").address()
    }
    pub fn start(
        store: Arc<Store>,
        network: Arc<Network>,
        address: &str,
        nodes: u32,
        workers_per_node: u32,
        queue_capacity: usize,
    ) -> Result<Self> {
        Self::start_inner(
            store,
            network,
            address,
            nodes,
            workers_per_node,
            queue_capacity,
            None,
        )
    }
    pub fn start_with_gateway(
        store: Arc<Store>,
        network: Arc<Network>,
        address: &str,
        nodes: u32,
        workers_per_node: u32,
        queue_capacity: usize,
        gateway: crate::gateway::Config,
    ) -> Result<Self> {
        Self::start_inner(
            store,
            network,
            address,
            nodes,
            workers_per_node,
            queue_capacity,
            Some(gateway),
        )
    }
    fn start_inner(
        store: Arc<Store>,
        network: Arc<Network>,
        address: &str,
        nodes: u32,
        workers_per_node: u32,
        queue_capacity: usize,
        gateway: Option<crate::gateway::Config>,
    ) -> Result<Self> {
        if nodes
            .checked_mul(workers_per_node)
            .is_none_or(|count| count == 0 || count > 256)
            || queue_capacity == 0
        {
            return Err(Error::new(
                ErrorCode::Invalid,
                "invalid participant worker/queue count",
            ));
        }
        let stopped = Arc::new(AtomicBool::new(false));
        let service = ParticipantService::with_capacity(&store, network.clone(), queue_capacity)?;
        let mut running = Self {
            server: None,
            gateway: None,
            stopped: stopped.clone(),
            _service: service.clone(),
            _network: network.clone(),
            _store: store.clone(),
        };
        if let Some(config) = gateway {
            running.gateway = Some(crate::gateway::RunningGateway::start(
                store.clone(),
                network.clone(),
                config,
                queue_capacity,
            )?);
        }
        let gateway = running.gateway.as_ref().map(|gateway| gateway.sender());
        let endpoint = store.clone();
        let weak = Arc::downgrade(&store);
        running.server = Some(RpcServer::start(
            &network,
            address,
            move |message, respond| {
                if stopped.load(Ordering::Acquire) {
                    respond(Reply::Error(Error::new(ErrorCode::Closed, "shard stopped")));
                    return;
                }
                let Some(store) = weak.upgrade() else {
                    respond(Reply::Error(Error::new(ErrorCode::Closed, "shard stopped")));
                    return;
                };
                match message {
                    Message::ReplicaStatus => {
                        respond(Reply::ReplicaStatus(store.log.replica_status(store.owner)))
                    }
                    message @ (Message::Gateway(_) | Message::Route(_)) => {
                        if let Some(gateway) = &gateway {
                            gateway.dispatch(message, respond);
                        } else {
                            respond(Reply::Error(Error::new(
                                ErrorCode::Invalid,
                                "gateway is not configured",
                            )));
                        }
                    }
                    Message::Migration {
                        owner,
                        operation,
                        payload,
                    } => {
                        if owner != store.owner {
                            respond(Reply::Error(Error::new(
                                ErrorCode::Invalid,
                                "wrong migration owner",
                            )));
                        } else {
                            crate::migration::dispatch(owner, operation, payload, respond);
                        }
                    }
                    message => {
                        let stream = match &message {
                            Message::Participant(request) => Some(request.stream),
                            Message::OpenStream { stream, .. } => Some(*stream),
                            Message::Transaction(request) => Some(Stream {
                                node: request.id.coordinator,
                                worker: request.id.worker,
                            }),
                            _ => None,
                        };
                        if stream.is_some_and(|stream| {
                            stream.node >= nodes || stream.worker >= workers_per_node
                        }) {
                            respond(Reply::Error(Error::new(
                                ErrorCode::Invalid,
                                "participant stream outside configured lanes",
                            )));
                        } else {
                            service.dispatch(message, respond);
                        }
                    }
                }
            },
        )?);
        *endpoint.endpoint.write() = Some(running.address().into());
        Ok(running)
    }
}
impl Drop for RunningShard {
    fn drop(&mut self) {
        self.stopped.store(true, Ordering::Release);
        if let Some(mut server) = self.server.take() {
            server.shutdown();
        }
        self.gateway.take();
        // The Store owns prepared workers. Closing a listener is not an Abort.
    }
}
