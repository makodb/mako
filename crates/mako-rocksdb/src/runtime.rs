//! Affine coordinator lanes. Only owned commands and results cross threads.
use crate::{
    backend::{Backend, Session},
    *,
};
use mako_runtime::{
    client::{self, Client},
    migration::{Kind, TableSpec},
    node::{Config, EngineConfig, PersistenceConfig, RunningNode},
    storage, wire as w,
};
use std::{
    path::Path,
    sync::mpsc::{self, Receiver, SyncSender},
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

const TIMEOUT: Duration = Duration::from_secs(10);

impl DB {
    pub fn open_default<P: AsRef<Path>>(path: P) -> Result<Self> {
        let mut options = Options::default();
        options.create_if_missing(true);
        Self::open(&options, path)
    }
    pub fn open<P: AsRef<Path>>(options: &Options, path: P) -> Result<Self> {
        Self::open_cf(options, path, [DEFAULT_COLUMN_FAMILY_NAME])
    }
    pub fn open_cf<P, I, N>(options: &Options, path: P, families: I) -> Result<Self>
    where
        P: AsRef<Path>,
        I: IntoIterator<Item = N>,
        N: AsRef<str>,
    {
        let path = path.as_ref();
        let exists = path.join("data/CURRENT").is_file();
        if exists && options.error_if_exists {
            return Err(Error::invalid("database already exists"));
        }
        if !exists && !options.create {
            return Err(Error::invalid(
                "database is missing; enable create_if_missing",
            ));
        }
        if !exists && path.exists() && std::fs::read_dir(path).map_err(io_error)?.next().is_some() {
            return Err(Error::invalid(
                "nonempty directory is not a Mako RocksDB database",
            ));
        }
        let mut names = BTreeSet::new();
        for name in families {
            let name = name.as_ref();
            if name.is_empty() || name.contains('\0') || !names.insert(name.to_owned()) {
                return Err(Error::invalid("invalid or duplicate column family name"));
            }
        }
        names.remove(DEFAULT_COLUMN_FAMILY_NAME);
        let names = std::iter::once(DEFAULT_COLUMN_FAMILY_NAME.to_owned()).chain(names);
        let tables = names
            .enumerate()
            .map(|(i, name)| TableSpec {
                id: i as u64 + 1,
                name,
                kind: Kind::Governed,
                warehouse: false,
                initial_owner: 0,
            })
            .collect();
        Self::open_config(Config {
            engine: EngineConfig::RocksDb {
                directory: path.join("data"),
                masstree: options.masstree,
            },
            owner: 0,
            nodes: 1,
            listen: "127.0.0.1:0".into(),
            tables,
            workers: options.max_transactions,
            gateway_workers: 1,
            queue_capacity: 64,
            warehouses_per_shard: 0,
            peers: BTreeMap::new(),
            migration: false,
            initial: Vec::new(),
            replication: None,
            persistence: Some(PersistenceConfig {
                directory: path.to_path_buf(),
                create: !exists,
                sync_writes: true,
            }),
            volatile: false,
            replica_groups: BTreeMap::new(),
        })
    }
    /// Start one distributed shard. Every raw schema table is a column family.
    /// All shards use the same schema and worker count; peers supply routing.
    /// The returned DB owns listeners, workers and orderly shutdown.
    pub fn open_config(config: Config) -> Result<Self> {
        config.schema().map_err(convert)?;
        if config.volatile
            || config
                .tables
                .iter()
                .any(|t| t.warehouse || t.name.contains('\0') || t.kind == Kind::Replicated)
            || !config
                .tables
                .iter()
                .any(|t| t.name == DEFAULT_COLUMN_FAMILY_NAME)
            || config.persistence.as_ref().is_some_and(|c| !c.sync_writes)
            || config.replication.as_ref().is_some_and(|c| !c.sync_writes)
            || (0..config.nodes).any(|owner| {
                owner != config.owner
                    && !config.peers.contains_key(&owner)
                    && !config.replica_groups.contains_key(&owner)
            })
        {
            return Err(Error::invalid("RocksDB API requires durable synchronous logging, raw tables including default, and complete peer routing"));
        }
        let (owner, node) = NodeOwner::start(config.clone())?;
        let mut lanes = vec![Lane::Free; config.workers as usize];
        // Existing unresolved ownership is reserved until explicit resolution.
        for entry in node.store.journal.entries().map_err(convert)? {
            if entry.unresolved() && entry.transaction.id.coordinator == config.owner {
                let id = entry.transaction.id;
                let lane = lanes
                    .get_mut(id.worker as usize)
                    .ok_or_else(|| Error::invalid("recovered stream exceeds configured workers"))?;
                *lane = Lane::Pending(id);
            }
        }
        let shared = Arc::new(Shared {
            node,
            _owner: owner,
            config,
            lanes: Mutex::new(lanes),
        });
        Self::from_backend(Arc::new(Native(shared)))
    }
}

#[derive(Clone, Copy)]
enum Lane {
    Free,
    Starting,
    Active,
    Pending(TransactionId),
    Resolving,
}
struct Shared {
    node: mako_runtime::node::Handle,
    _owner: NodeOwner,
    config: Config,
    lanes: Mutex<Vec<Lane>>,
}
// SRPC's listener is affine as well. Construct and destroy RunningNode on its
// owner thread; only its already thread-safe Handle crosses that boundary.
struct NodeOwner {
    stop: SyncSender<()>,
    thread: Option<JoinHandle<()>>,
}
impl NodeOwner {
    fn start(config: Config) -> Result<(Self, mako_runtime::node::Handle)> {
        let (stop, stopped) = mpsc::sync_channel(1);
        let (ready, wait) = mpsc::sync_channel(1);
        let thread = thread::Builder::new()
            .name("rocks-api-node".into())
            .spawn(move || {
                let start = (|| {
                    let node = RunningNode::start(config).map_err(convert)?;
                    let deadline = Instant::now() + TIMEOUT;
                    while !node.ready() {
                        node.health().map_err(convert)?;
                        if Instant::now() >= deadline {
                            return Err(Error::new(
                                ErrorKind::TryAgain,
                                "coordinator did not become ready",
                            ));
                        }
                        thread::sleep(Duration::from_millis(10));
                    }
                    Ok(node)
                })();
                match start {
                    Ok(node) => {
                        if ready
                            .send(Ok(mako_runtime::node::Handle::from(&node)))
                            .is_ok()
                        {
                            let _ = stopped.recv();
                        }
                        drop(node);
                    }
                    Err(error) => {
                        let _ = ready.send(Err(error));
                    }
                }
            })
            .map_err(io_error)?;
        let owner = Self {
            stop,
            thread: Some(thread),
        };
        let node = wait
            .recv()
            .map_err(|_| Error::new(ErrorKind::IOError, "node owner failed to initialize"))??;
        Ok((owner, node))
    }
}
impl Drop for NodeOwner {
    fn drop(&mut self) {
        let _ = self.stop.send(());
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}
struct Native(Arc<Shared>);
impl Backend for Native {
    fn column_families(&self) -> Vec<(String, u64)> {
        self.0
            .config
            .tables
            .iter()
            .map(|t| (t.name.clone(), t.id))
            .collect()
    }
    fn validate_write(&self, _: &WriteOptions) -> Result<()> {
        Ok(())
    } // All configured logs sync.
    fn begin(&self) -> Result<Box<dyn Session>> {
        let index = {
            let mut lanes = self.0.lanes.lock().expect("coordinator lanes");
            let index = lanes
                .iter()
                .position(|lane| matches!(lane, Lane::Free))
                .ok_or_else(|| {
                    Error::new(
                        ErrorKind::Busy,
                        "all coordinator streams are occupied or awaiting resolution",
                    )
                })?;
            lanes[index] = Lane::Starting;
            index
        };
        let (send, receive) = mpsc::sync_channel(1);
        let (ready, wait) = mpsc::sync_channel(1);
        let shared = self.0.clone();
        let thread = match thread::Builder::new()
            .name(format!("rocks-api-{index}"))
            .spawn(move || lane(shared, index, receive, ready))
        {
            Ok(thread) => thread,
            Err(error) => {
                self.0.lanes.lock().expect("coordinator lanes")[index] = Lane::Free;
                return Err(io_error(error));
            }
        };
        match wait.recv() {
            Ok(Ok(id)) => Ok(Box::new(NativeSession {
                shared: self.0.clone(),
                index,
                id,
                send,
                thread: Some(thread),
            })),
            failure => {
                drop(send);
                let _ = thread.join();
                Err(failure
                    .unwrap_or_else(|_| {
                        Err(Error::new(
                            ErrorKind::IOError,
                            "coordinator failed to initialize",
                        ))
                    })
                    .unwrap_err())
            }
        }
    }
    fn resolve(&self, id: TransactionId) -> Result<Outcome> {
        if id.coordinator != self.0.config.owner
            || id.worker >= self.0.config.workers
            || id.incarnation == 0
            || id.attempt == 0
        {
            return Err(Error::invalid(
                "identity belongs to another coordinator or invalid stream",
            ));
        }
        {
            let mut lanes = self.0.lanes.lock().expect("coordinator lanes");
            match lanes[id.worker as usize] {
                Lane::Free => (),
                Lane::Pending(saved) if saved == id => (),
                _ => {
                    return Err(Error::new(
                        ErrorKind::Busy,
                        "stream has a live or different pending transaction",
                    ))
                }
            }
            lanes[id.worker as usize] = Lane::Resolving;
        }
        let result = client::resolve_transaction(
            self.0.node.network.clone(),
            self.0.node.store.clone(),
            id,
            TIMEOUT,
        )
        .map_err(|e| Error::new(ErrorKind::UnknownCommit, e.to_string()).with_transaction(id));
        self.0.lanes.lock().expect("coordinator lanes")[id.worker as usize] = if result.is_ok() {
            Lane::Free
        } else {
            Lane::Pending(id)
        };
        result
    }
}

enum Command {
    Get(u64, Vec<u8>),
    Put(u64, Vec<u8>, Vec<u8>),
    Delete(u64, Vec<u8>),
    Scan(u64, Bound, Bound, bool, usize),
    Commit,
    Abort,
}
enum Response {
    Unit,
    Value(Option<Vec<u8>>),
    Rows(Vec<Row>),
}
type Work = (Command, SyncSender<Result<Response>>);
struct NativeSession {
    shared: Arc<Shared>,
    index: usize,
    id: TransactionId,
    send: SyncSender<Work>,
    thread: Option<JoinHandle<()>>,
}
impl NativeSession {
    fn call(&self, command: Command) -> Result<Response> {
        let (send, receive) = mpsc::sync_channel(1);
        self.send
            .send((command, send))
            .map_err(|_| self.disconnected())?;
        receive.recv().map_err(|_| self.disconnected())?
    }
    fn disconnected(&self) -> Error {
        Error::new(
            ErrorKind::UnknownCommit,
            "coordinator lane stopped; resolve the saved identity",
        )
        .with_transaction(self.id)
    }
    fn finish(&mut self, command: Command) -> Result<()> {
        let result = self.call(command).map(|_| ());
        if let Some(thread) = self.thread.take() {
            if thread.join().is_err() {
                self.shared.lanes.lock().expect("coordinator lanes")[self.index] =
                    Lane::Pending(self.id);
                return Err(self.disconnected());
            }
        }
        // Release only after the caller received the terminal result and joined
        // the owner. A lost reply must never create a stream reuse window.
        self.shared.lanes.lock().expect("coordinator lanes")[self.index] = if matches!(&result, Err(e) if e.kind() == ErrorKind::UnknownCommit)
        {
            Lane::Pending(self.id)
        } else {
            Lane::Free
        };
        result
    }
}
impl Session for NativeSession {
    fn id(&self) -> TransactionId {
        self.id
    }
    fn get(&mut self, table: u64, key: &[u8]) -> Result<Option<Vec<u8>>> {
        match self.call(Command::Get(table, key.to_vec()))? {
            Response::Value(v) => Ok(v),
            _ => unreachable!(),
        }
    }
    fn put(&mut self, table: u64, key: &[u8], value: &[u8]) -> Result<()> {
        self.call(Command::Put(table, key.to_vec(), value.to_vec()))
            .map(|_| ())
    }
    fn delete(&mut self, table: u64, key: &[u8]) -> Result<()> {
        self.call(Command::Delete(table, key.to_vec())).map(|_| ())
    }
    fn scan(
        &mut self,
        table: u64,
        lower: Bound,
        upper: Bound,
        reverse: bool,
        limit: usize,
    ) -> Result<Vec<Row>> {
        match self.call(Command::Scan(table, lower, upper, reverse, limit))? {
            Response::Rows(v) => Ok(v),
            _ => unreachable!(),
        }
    }
    fn commit(mut self: Box<Self>) -> Result<()> {
        self.finish(Command::Commit)
    }
    fn rollback(mut self: Box<Self>) -> Result<()> {
        self.finish(Command::Abort)
    }
}
impl Drop for NativeSession {
    fn drop(&mut self) {
        if self.thread.is_some() {
            let _ = self.finish(Command::Abort);
        }
    }
}

fn lane(
    shared: Arc<Shared>,
    index: usize,
    receive: Receiver<Work>,
    ready: SyncSender<Result<TransactionId>>,
) {
    let setup = (|| {
        let mut client = Client::new(
            shared.node.network.clone(),
            shared.config.peers.clone(),
            w::Stream {
                node: shared.config.owner,
                worker: index as u32,
            },
            0,
            TIMEOUT,
        )?;
        for (owner, replicas) in &shared.config.replica_groups {
            client.set_replicas(*owner, replicas.clone())?;
        }
        let worker = shared
            .node
            .store
            .database
            .worker()
            .map_err(storage::engine)?;
        Ok::<_, w::Error>((client, worker))
    })();
    let (mut client, mut worker) = match setup {
        Ok(value) => value,
        Err(error) => {
            shared.lanes.lock().expect("coordinator lanes")[index] = Lane::Free;
            let _ = ready.send(Err(convert(error)));
            return;
        }
    };
    let (result, reply) = serve(&shared, index, &mut client, &mut worker, receive, &ready);
    let id = client.last_attempt_id();
    let fenced = client.is_fenced();
    {
        let mut lanes = shared.lanes.lock().expect("coordinator lanes");
        let started = matches!(lanes[index], Lane::Active);
        lanes[index] = if fenced || (started && reply.is_none()) {
            Lane::Pending(id)
        } else if started {
            Lane::Active
        } else {
            Lane::Free
        };
    }
    let result = result.map_err(|error| {
        if fenced {
            Error::new(ErrorKind::UnknownCommit, error.to_string()).with_transaction(id)
        } else {
            convert(error)
        }
    });
    if let Some(reply) = reply {
        let _ = reply.send(result.map(|()| Response::Unit));
    } else if let Err(error) = result {
        let _ = ready.send(Err(error));
    }
}
fn serve(
    shared: &Shared,
    index: usize,
    client: &mut Client,
    worker: &mut mako_storage_api::Worker,
    receive: Receiver<Work>,
    ready: &SyncSender<Result<TransactionId>>,
) -> (w::Result<()>, Option<SyncSender<Result<Response>>>) {
    let mut tx = match client.begin(shared.node.store.clone(), worker) {
        Ok(tx) => tx,
        Err(e) => return (Err(e), None),
    };
    shared.lanes.lock().expect("coordinator lanes")[index] = Lane::Active;
    if ready.send(Ok(tx.id())).is_err() {
        return (tx.abort(), None);
    }
    while let Ok((command, reply)) = receive.recv() {
        let table = |table| w::TableKey {
            table,
            warehouse: 0,
        };
        let result = match command {
            Command::Get(t, k) => tx
                .get_routed(&shared.node.schema, table(t), &k)
                .map(Response::Value),
            Command::Put(t, k, v) => tx
                .mutate_routed(&shared.node.schema, table(t), w::Mutation::Put, &k, &v)
                .map(|()| Response::Unit),
            Command::Delete(t, k) => {
                tx.get_routed(&shared.node.schema, table(t), &k)
                    .and_then(|value| {
                        // Native Remove has an existence precondition. RocksDB Delete
                        // also succeeds for absence; retain that absence dependency.
                        if value.is_some() {
                            tx.mutate_routed(
                                &shared.node.schema,
                                table(t),
                                w::Mutation::Remove,
                                &k,
                                b"",
                            )?;
                        }
                        Ok(Response::Unit)
                    })
            }
            Command::Scan(t, l, u, r, n) => tx
                .scan_routed(&shared.node.schema, table(t), l, u, r, n)
                .map(Response::Rows),
            Command::Commit => {
                let result = tx.commit();
                #[cfg(test)]
                if result.is_ok()
                    && DROP_COMMIT_REPLY.swap(false, std::sync::atomic::Ordering::Relaxed)
                {
                    return (result, None);
                }
                return (result, Some(reply));
            }
            Command::Abort => return (tx.abort(), Some(reply)),
        }
        .map_err(|error| convert(error).with_transaction(tx.id()));
        let _ = reply.send(result);
    }
    (tx.abort(), None)
}

fn convert(error: w::Error) -> Error {
    let kind = match error.code {
        w::ErrorCode::Conflict | w::ErrorCode::Busy => ErrorKind::Busy,
        w::ErrorCode::RouteChanged => ErrorKind::TryAgain,
        w::ErrorCode::Invalid | w::ErrorCode::Capacity => ErrorKind::InvalidArgument,
        w::ErrorCode::Unknown | w::ErrorCode::CommittedFailure => ErrorKind::UnknownCommit,
        _ => ErrorKind::IOError,
    };
    Error::new(kind, error.to_string())
}
fn io_error(error: std::io::Error) -> Error {
    Error::new(ErrorKind::IOError, error.to_string())
}

#[cfg(test)]
static DROP_COMMIT_REPLY: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn lost_commit_reply_fences_stream_and_resolves_saved_decision() {
        let root =
            std::env::temp_dir().join(format!("rocks-api-lost-reply-{}", std::process::id()));
        let mut options = Options::default();
        options.create_if_missing(true);
        options.set_max_transactions(1);
        let db = DB::open(&options, &root).unwrap();
        let tx = db.transaction();
        tx.put(b"committed", b"once").unwrap();
        let id = tx.transaction_id().unwrap();
        DROP_COMMIT_REPLY.store(true, std::sync::atomic::Ordering::Relaxed);
        let error = tx.commit().unwrap_err();
        assert_eq!(error.kind(), ErrorKind::UnknownCommit);
        assert_eq!(error.transaction_id(), Some(id));
        assert_eq!(db.pending_transactions(), [id]);
        assert_eq!(db.get(b"committed").unwrap_err().kind(), ErrorKind::Busy);
        assert_eq!(db.resolve(id).unwrap(), Outcome::Commit);
        assert!(db.pending_transactions().is_empty());
        assert_eq!(db.get(b"committed").unwrap(), Some(b"once".to_vec()));
        drop(db);
        let db = DB::open(&Options::default(), &root).unwrap();
        assert_eq!(db.resolve(id).unwrap(), Outcome::Commit);
        drop(db);
        std::fs::remove_dir_all(root).unwrap();
    }
}
