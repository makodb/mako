//! Native orchestration. Checked actors own all ownership/lease transitions;
//! this layer owns threads, fixed-peer transport and actual engine lifetimes.
//! Live handoff is deliberately unavailable with replication or node restart.
use std::collections::{HashMap, HashSet};
use std::sync::{Arc, LazyLock, mpsc};
use parking_lot::{Mutex, RwLock};
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread::{self, JoinHandle};
use std::time::Duration;
use crate::catalog::{Catalog, Coordinates, TableKind, warehouse_coordinate};
use crate::directory::RouteTable;
use crate::host::{Completion, Host, Opener};
use crate::migration::Coordinator;
use crate::participant::{Cleanup, ControlResult, Participant};
use crate::routing::{RoutingCache, Snapshot};
use crate::routing_codec::{self, Writer};
use crate::storage::Source;
use crate::transfer::{self, RawStore};
use crate::types::*;
use crate::warehouse::{HandleKey, WarehouseDirectory};
use crate::wire::{self, checked};

type Nonce = (u64,u64);
type RawHandle = (u64,u32,bool);
struct AdminRecord { generation: u64, request: Vec<u8> }
struct Master { actor: Coordinator, requests: HashMap<Nonce,AdminRecord> }
struct Local {
    participant: Participant,
    store: EngineStore,
    plans: HashMap<u64,Vec<u8>>,
    ready: HashSet<u64>,
}
struct Job { operation: u32, payload: Vec<u8>, completion: Completion }
pub struct Node {
    owner: u32, nodes: u32, migration: bool, host: Host, hello: Vec<u8>,
    local: Mutex<Local>, master: Option<Mutex<Master>>,
    queue: Mutex<Option<mpsc::Sender<Job>>>, migrations: Mutex<Option<mpsc::Sender<u64>>>,
    threads: Mutex<Vec<JoinHandle<()>>>, stopping: AtomicBool,
    data_ready: std::sync::atomic::AtomicU32,
    shutdown: Mutex<Option<(std::path::PathBuf,String)>>,
    quiescing: AtomicBool,
}
pub struct Runtime {
    pub catalog: RwLock<Catalog>,
    pub warehouses: Mutex<Option<WarehouseDirectory>>,
    openers: Mutex<HashMap<u32,Opener>>,
    raw: Mutex<HashMap<RawHandle,usize>>,
    // The legacy DB's tables_taken map is not synchronized. Serialize both
    // raw and warehouse native openers, not merely each handle cache.
    engine_open: Mutex<()>,
    nodes: RwLock<HashMap<u32,Arc<Node>>>,
    cache: Mutex<Option<RoutingCache>>,
    requested: bool,
    members: RwLock<Vec<u32>>,
    lifecycle: Mutex<()>,
    configured: AtomicBool,
    // Replication replay and live migration cannot share this fixed process.
    // The latch is synchronized with start(), including replay before bootstrap.
    replication_replay: AtomicBool,
}
static RUNTIME: LazyLock<Runtime> = LazyLock::new(|| Runtime {
    catalog: RwLock::new(Catalog::new()),
    warehouses: Mutex::new(None), openers: Mutex::new(HashMap::new()),
    raw: Mutex::new(HashMap::new()), nodes: RwLock::new(HashMap::new()),
    engine_open: Mutex::new(()),
    cache: Mutex::new(None), members: RwLock::new(Vec::new()),
    lifecycle: Mutex::new(()), configured: AtomicBool::new(false),
    replication_replay: AtomicBool::new(false),
    requested: std::env::var_os("MAKO_CLUSTER_CONFIG").as_deref() == Some(std::ffi::OsStr::new("1")),
});
pub fn runtime() -> &'static Runtime { &RUNTIME }
impl Runtime {
    pub fn enabled(&self) -> bool { self.requested || self.configured.load(Ordering::Acquire) }
    pub fn node(&self, owner: u32) -> Result<Arc<Node>,Status> {
        let nodes = self.nodes.read();
        let node = nodes.get(&owner).ok_or(Status::NotFound)?;
        if node.stopping.load(Ordering::Acquire) { return Err(Status::Retry); }
        Ok(Arc::clone(node))
    }
    pub fn replication_replay(&self, replicated: bool) -> Result<(),Status> {
        let _lifecycle = self.lifecycle.lock();
        if !replicated || self.nodes.read().values().any(|node| node.migration) {
            return Err(Status::Invalid);
        }
        self.replication_replay.store(true,Ordering::Release);
        Ok(())
    }
    fn handle_live(&self, owner: u32) -> Result<(),Status> {
        if self.nodes.read().get(&owner).is_some_and(|n| n.stopping.load(Ordering::Acquire)) {
            Err(Status::Retry)
        } else { Ok(()) }
    }
    pub fn catalog_register(&self, id: u64, name: &[u8], kind: TableKind, coordinates: Coordinates, owner: u32) -> Status {
        let status = self.catalog.write().register(id,name,kind,coordinates,owner);
        if status == Status::Ok { self.configured.store(true,Ordering::Release); }
        status
    }
    pub fn catalog_tpcc(&self, micro: bool) -> Result<(),Status> {
        let initial = Catalog::tpcc(micro)?;
        let mut catalog = self.catalog.write();
        for i in 0..initial.len() {
            let t = initial.at(i).ok_or(Status::Invalid)?;
            checked(catalog.register(t.id,&t.name,t.kind,t.coordinates,t.initial_owner))?;
        }
        self.configured.store(true,Ordering::Release);
        Ok(())
    }
    pub fn warehouse_init(&self, per_shard: u32, total: u32) -> Result<(),Status> {
        let catalog = self.catalog.read();
        let mut directory = self.warehouses.lock();
        if let Some(d) = &*directory {
            return if d.dimensions() == (per_shard,total) { Ok(()) } else { Err(Status::Invalid) };
        }
        let mut d = WarehouseDirectory::new(per_shard,total)?;
        for i in 0..catalog.len() {
            let table = catalog.at(i).ok_or(Status::Invalid)?;
            if table.coordinates == Coordinates::Warehouse { checked(d.add_table(table.id))?; }
        }
        *directory = Some(d);
        Ok(())
    }
    pub fn warehouse_opener(&self, owner: u32, opener: Opener) -> Result<(),Status> {
        self.handle_live(owner)?;
        let mut openers = self.openers.lock();
        if let Some(old) = openers.get(&owner) {
            return if old.same(&opener) { Ok(()) } else { Err(Status::Invalid) };
        }
        openers.insert(owner,opener);
        Ok(())
    }
    fn materialize(&self, d: &mut WarehouseDirectory, key: HandleKey) -> Result<usize,Status> {
        let handle = d.cached(key)?;
        if handle != 0 { return Ok(handle); }
        let opener = *self.openers.lock().get(&key.owner).ok_or(Status::NotFound)?;
        let _opening = self.engine_open.lock();
        let handle = opener.open(key.table,key.warehouse,key.owner,key.proxy)?;
        checked(d.register(key,handle))?;
        Ok(handle)
    }
    pub fn warehouse_local(&self, table: u64, warehouse: u32, owner: u32) -> Result<usize,Status> {
        self.handle_live(owner)?;
        let mut directory = self.warehouses.lock();
        let d = directory.as_mut().ok_or(Status::NotFound)?;
        let key = d.local(table,warehouse,owner)?;
        self.materialize(d,key)
    }
    pub fn warehouse_resolve(&self, table: u64, warehouse: u32, participant: u32) -> Result<(usize,Grant),Status> {
        self.handle_live(participant)?;
        let grant = self.route(table,&warehouse_coordinate(warehouse))?;
        let mut directory = self.warehouses.lock();
        let d = directory.as_mut().ok_or(Status::NotFound)?;
        let selection = d.select(table,warehouse,participant,grant)?;
        let handle = self.materialize(d,selection.key)?;
        Ok((handle,selection.grant))
    }
    pub fn raw_register(&self, table: u64, owner: u32, proxy: bool, handle: usize) -> Result<(),Status> {
        self.handle_live(owner)?;
        if handle == 0 { return Err(Status::Invalid); }
        let catalog = self.catalog.read();
        let t = catalog.by_id(table).ok_or(Status::NotFound)?;
        if t.coordinates != Coordinates::Raw { return Err(Status::Invalid); }
        let mut raw = self.raw.lock();
        let key = (table,owner,proxy);
        if let Some(old) = raw.get(&key) { return if *old == handle { Ok(()) } else { Err(Status::Invalid) }; }
        if raw.values().any(|old| *old == handle) { return Err(Status::Invalid); }
        raw.insert(key,handle);
        Ok(())
    }
    fn raw_handle(&self, table: u64, owner: u32, proxy: bool, host: Host) -> Result<usize,Status> {
        self.handle_live(owner)?;
        let kind = self.catalog.read().by_id(table).ok_or(Status::NotFound)?.kind as u32;
        let mut raw = self.raw.lock();
        let key = (table,owner,proxy);
        if let Some(handle) = raw.get(&key) { return Ok(*handle); }
        let name = format!("native_raw_{table}_owner_{owner}_{}",if proxy { "proxy" } else { "bytes" });
        let _opening = self.engine_open.lock();
        let handle = host.open(table,name.as_bytes(),owner,proxy,kind)?;
        if raw.values().any(|old| *old == handle) { return Err(Status::Invalid); }
        raw.insert(key,handle);
        Ok(handle)
    }
    pub fn local_table(&self, table: u64, coordinate: &[u8], fixed: bool, owner: u32) -> Result<usize,Status> {
        let coordinates = self.catalog.read().by_id(table).ok_or(Status::NotFound)?.coordinates;
        match (coordinates,fixed) {
            (Coordinates::Warehouse,true) => {
                let bytes: [u8;4] = coordinate.try_into().map_err(|_| Status::Invalid)?;
                self.warehouse_local(table,u32::from_be_bytes(bytes),owner)
            },
            (Coordinates::Raw,false) => {
                self.handle_live(owner)?;
                if let Some(handle) = self.raw.lock().get(&(table,owner,false)) { return Ok(*handle); }
                self.raw_handle(table,owner,false,self.node(owner)?.host)
            },
            _ => Err(Status::Invalid),
        }
    }
    pub fn raw_proxy(&self, table: u64, origin: u32) -> Result<usize,Status> {
        if self.catalog.read().by_id(table).ok_or(Status::NotFound)?.coordinates != Coordinates::Raw {
            return Err(Status::Invalid);
        }
        self.raw_handle(table,origin,true,self.node(origin)?.host)
    }
    pub fn route(&self, table: u64, coordinate: &[u8]) -> Result<Grant,Status> {
        if let Some(cache) = &*self.cache.lock() {
            return cache.lookup(table,coordinate).ok_or(Status::NotFound);
        }
        // Bootstrap loading only. Once a cache exists it never falls back here.
        let catalog = self.catalog.read();
        let t = catalog.by_id(table).ok_or(Status::NotFound)?;
        let owner = if t.coordinates == Coordinates::Warehouse {
            let bytes: [u8;4] = coordinate.try_into().map_err(|_| Status::Invalid)?;
            let warehouse = u32::from_be_bytes(bytes);
            let directory = self.warehouses.lock();
            let (per_shard,total) = directory.as_ref().ok_or(Status::NotFound)?.dimensions();
            if warehouse == 0 || warehouse > total { return Err(Status::Invalid); }
            (warehouse-1)/per_shard
        } else { t.initial_owner };
        Ok(Grant { owner, epoch: 0 })
    }
    pub fn snapshot(&self) -> Result<Arc<Snapshot>,Status> {
        self.cache.lock().as_ref().and_then(RoutingCache::snapshot_arc).ok_or(Status::NotFound)
    }
    fn install(&self, bytes: &[u8]) -> Result<(),Status> {
        let snapshot = routing_codec::decode(bytes,&self.members.read())?;
        checked(self.cache.lock().as_mut().ok_or(Status::NotFound)?.install(snapshot))
    }
    fn initial_tables(&self, nodes: u32) -> Result<Vec<RouteTable>,Status> {
        let catalog = self.catalog.read();
        let directory = self.warehouses.lock();
        let mut tables = Vec::with_capacity(catalog.len());
        for i in 0..catalog.len() {
            let t = catalog.at(i).ok_or(Status::Invalid)?;
            if t.initial_owner >= nodes { return Err(Status::Invalid); }
            let mut table = RouteTable::new(t.id,t.initial_owner);
            if t.coordinates == Coordinates::Warehouse {
                let (per_shard,total) = directory.as_ref().ok_or(Status::Invalid)?.dimensions();
                if total/per_shard != nodes { return Err(Status::Invalid); }
                for owner in 1..nodes {
                    let start = warehouse_coordinate(owner.checked_mul(per_shard).and_then(|x|x.checked_add(1)).ok_or(Status::Exhausted)?);
                    table = table.move_range(&start,None,owner-1,owner,0)?;
                }
            }
            tables.push(table);
        }
        Ok(tables)
    }
    fn hello(&self, nodes: u32, migration: bool) -> Result<Vec<u8>,Status> {
        let catalog = self.catalog.read();
        let dimensions = self.warehouses.lock().as_ref().map_or((0,0),WarehouseDirectory::dimensions);
        let mut w = Writer::new();
        checked(w.write_u32(1))?; checked(w.write_u32(nodes))?; checked(w.write_u32(u32::from(migration)))?;
        checked(w.write_u32(dimensions.0))?; checked(w.write_u32(dimensions.1))?;
        checked(w.write_u64(catalog.len() as u64))?;
        for i in 0..catalog.len() {
            let t = catalog.at(i).ok_or(Status::Invalid)?;
            checked(w.write_u64(t.id))?; checked(w.write_bytes(&t.name))?;
            checked(w.write_u32(t.kind as u32))?; checked(w.write_u32(if t.coordinates == Coordinates::Raw {0} else {1}))?;
            checked(w.write_u32(t.initial_owner))?;
        }
        Ok(w.into_bytes())
    }
    pub fn start(&self, owner: u32, count: u32, migration: bool, host: Host) -> Result<(),Status> {
        let _lifecycle = self.lifecycle.lock();
        if count == 0 || owner >= count || !self.configured.load(Ordering::Acquire) { return Err(Status::Invalid); }
        if migration && self.replication_replay.load(Ordering::Acquire) { return Err(Status::Invalid); }
        {
            let nodes = self.nodes.read();
            if nodes.contains_key(&owner) || nodes.values().any(|n|n.nodes != count || n.migration != migration) {
                return Err(Status::Invalid);
            }
        }
        // A source's loaded raw table must be registered, never replaced with an
        // empty, similarly named index. Non-owner private storage opens lazily.
        {
            let catalog = self.catalog.read();
            let raw = self.raw.lock();
            for i in 0..catalog.len() {
                let t = catalog.at(i).ok_or(Status::Invalid)?;
                if t.kind == TableKind::Governed && t.coordinates == Coordinates::Raw
                    && t.initial_owner == owner && !raw.contains_key(&(t.id,owner,false)) {
                    return Err(Status::Invalid);
                }
            }
        }
        self.catalog.write().seal();
        let members: Vec<u32> = (0..count).collect();
        {
            let mut cache = self.cache.lock();
            if cache.is_none() {
                *self.members.write() = members.clone();
                let mut initial = RoutingCache::new(members.clone())?;
                checked(initial.install(Snapshot { version: 0, tables: self.initial_tables(count)? }))?;
                *cache = Some(initial);
            }
        }
        let participant = Participant::new(owner,self.initial_tables(count)?)?;
        let master = if owner == 0 && count > 1 {
            Some(Mutex::new(Master { actor: Coordinator::new(members,self.initial_tables(count)?)?, requests: HashMap::new() }))
        } else { None };
        let (tx,rx) = mpsc::channel();
        let (migration_tx,migration_rx) = mpsc::channel();
        let node = Arc::new(Node { owner, nodes: count, migration, host, hello: self.hello(count,migration)?,
            local: Mutex::new(Local { participant,store: EngineStore {owner,host},plans: HashMap::new(),ready: HashSet::new() }),
            master, queue: Mutex::new(Some(tx)), migrations: Mutex::new(Some(migration_tx)),
            threads: Mutex::new(Vec::new()), stopping: AtomicBool::new(false),
            shutdown: Mutex::new(None), quiescing: AtomicBool::new(false),
            data_ready: std::sync::atomic::AtomicU32::new(Status::Retry as u32) });
        let (ready_tx,ready_rx) = mpsc::sync_channel(1);
        let worker = Arc::clone(&node);
        let thread = thread::Builder::new().name(format!("shard-native-{owner}")).spawn(move || {
            let entered = worker.host.enter(worker.owner);
            let success = entered.is_ok();
            let _ = ready_tx.send(entered);
            if !success { return; }
            while let Ok(job) = rx.recv() {
                let result = if worker.stopping.load(Ordering::Acquire) { Err(Status::Retry) }
                    else { worker.handle(job.operation,job.payload) };
                job.completion.complete(result);
            }
            worker.host.leave();
        }).map_err(|_|Status::Io)?;
        node.threads.lock().push(thread);
        match ready_rx.recv() { Ok(Ok(())) => {}, Ok(Err(e)) => { node.stop(); return Err(e); }, Err(_) => {node.stop();return Err(Status::Io);} }
        let running = Arc::clone(&node);
        let background = thread::Builder::new().name(format!("shard-control-{owner}")).spawn(move || {
            // HELLO denotes completed local loading, not peer data readiness:
            // requiring ready in HELLO would make all peers wait on each other.
            if let Err(status) = running.await_loaders() {
                running.data_ready.store(status as u32,Ordering::Release);
                return;
            }
            running.data_ready.store(Status::Ok as u32,Ordering::Release);
            if running.master.is_some() {
                while let Ok(generation) = migration_rx.recv() {
                    if !running.stopping.load(Ordering::Acquire) { running.drive(generation); }
                }
            } else {
                while !running.stopping.load(Ordering::Acquire) {
                    if !runtime().nodes.read().contains_key(&0) {
                        if let Ok(bytes) = running.host.peer(0,wire::SNAPSHOT,&[]) { let _ = runtime().install(&bytes); }
                    }
                    thread::sleep(Duration::from_millis(100));
                }
            }
        });
        match background { Ok(thread) => node.threads.lock().push(thread), Err(_) => {node.stop();return Err(Status::Io);} }
        self.nodes.write().insert(owner,node);
        Ok(())
    }
    pub fn stop(&self, owner: u32) {
        let _lifecycle = self.lifecycle.lock();
        let node = self.nodes.read().get(&owner).cloned();
        if let Some(node) = node { node.stop(); }
        // Retain the closed node/fences. Restarting it against surviving messages
        // is not a supported transition, even with the same numerical owner.
    }
    pub fn dispatch(&self, owner: u32, operation: u32, payload: Vec<u8>, completion: Completion) -> Result<(),Status> {
        let node = self.node(owner)?;
        let queue = node.queue.lock();
        queue.as_ref().ok_or(Status::Retry)?.send(Job {operation,payload,completion}).map_err(|_|Status::Retry)
    }
}
impl Node {
    fn stop(&self) {
        self.stopping.store(true,Ordering::Release);
        self.queue.lock().take(); self.migrations.lock().take();
        let threads = std::mem::take(&mut *self.threads.lock());
        for thread in threads { let _ = thread.join(); }
    }
    pub fn ready(&self) -> bool { self.data_ready.load(Ordering::Acquire) == Status::Ok as u32 }
    pub fn wait_ready(&self) -> Result<(),Status> {
        loop {
            if self.stopping.load(Ordering::Acquire) {return Err(Status::Retry);}
            let status = wire::status(self.data_ready.load(Ordering::Acquire));
            if status != Status::Retry {return checked(status);}
            thread::sleep(Duration::from_millis(20));
        }
    }
    fn await_loaders(&self) -> Result<(),Status> {
        loop {
            if self.stopping.load(Ordering::Acquire) {return Err(Status::Retry);}
            let mut loaded = true;
            for owner in 0..self.nodes {
                if owner == self.owner {continue;}
                match self.host.peer(owner,wire::HELLO,&[]) {
                    Ok(hello) => if hello != self.hello {return Err(Status::Invalid);},
                    Err(Status::Invalid) => return Err(Status::Invalid),
                    Err(_) => {loaded=false;break;},
                }
            }
            if loaded {return Ok(());}
            thread::sleep(Duration::from_millis(20));
        }
    }
    pub fn lease_begin(&self, id: TxnId) -> Status {
        let status = wire::status(self.data_ready.load(Ordering::Acquire));
        if status != Status::Ok {return status;}
        self.local.lock().participant.begin(id)
    }
    pub fn lease_acquire(&self, id: TxnId, table: u64, coordinate: &[u8], grant: Grant) -> Status {
        self.local.lock().participant.acquire(id,table,coordinate,grant)
    }
    pub fn lease_range(&self, id: TxnId, table: u64, lo: &[u8], hi: Option<&[u8]>, grant: Grant) -> Status {
        self.local.lock().participant.acquire_range(id,table,lo,hi,grant)
    }
    pub fn lease_finish(&self, id: TxnId) -> Status { self.local.lock().participant.finish(id) }
    /// Called only after this owner's ordinary coordinators joined, with their
    /// terminal RPCs acknowledged. The existing benchmark shared filesystem
    /// retains receipts after a peer closes its control service. These files
    /// are lifecycle evidence, never ownership or migration authorization.
    pub fn quiesce(&self, directory: &str) -> Result<(),Status> {
        let root = std::path::Path::new(directory);
        if directory.is_empty() {return Err(Status::Invalid);}
        self.quiescing.store(true,Ordering::Release);
        // No remote owner or migration actor exists in a one-owner deployment.
        if self.nodes == 1 {return Ok(());}
        let token = if self.owner == 0 {
            let mut shutdown = self.shutdown.lock();
            if let Some((previous,token)) = &*shutdown {
                if previous != root {return Err(Status::Invalid);}
                token.clone()
            } else {
                let stamp = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH)
                    .map_err(|_|Status::Io)?.as_nanos();
                let mut attempt = 0u64;
                let token = loop {
                    let token = format!("nfs_sync_native_shutdown_{}_{stamp}_{attempt}",std::process::id());
                    match std::fs::create_dir(root.join(&token)) {
                        Ok(()) => break token,
                        Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => {
                            attempt = attempt.checked_add(1).ok_or(Status::Exhausted)?;
                        },
                        Err(_) => return Err(Status::Io),
                    }
                };
                *shutdown = Some((root.to_path_buf(),token.clone()));
                token
            }
        } else {
            loop {
                match self.host.peer(0,wire::SHUTDOWN_TOKEN,&[]) {
                    Ok(bytes) => {
                        let token = String::from_utf8(bytes).map_err(|_|Status::Invalid)?;
                        if !token.starts_with("nfs_sync_native_shutdown_")
                            || !token.bytes().all(|byte|byte.is_ascii_alphanumeric() || byte == b'_') {
                            return Err(Status::Invalid);
                        }
                        break token;
                    },
                    Err(Status::Retry | Status::Io) => thread::sleep(Duration::from_millis(20)),
                    Err(error) => return Err(error),
                }
            }
        };
        let run = root.join(token);
        // Atomic, retained publication; only this owner creates its marker.
        match std::fs::OpenOptions::new().write(true).create_new(true)
            .open(run.join(format!("owner-{}",self.owner))) {
            Ok(_) => {},
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => {},
            Err(_) => return Err(Status::Io),
        }
        for owner in 0..self.nodes {
            wait_for_shutdown_marker(&run.join(format!("owner-{owner}")))?;
        }
        let control_done = run.join("control-done");
        if self.owner == 0 {
            // Native transfer jobs may still hold physical engine/peer borrows.
            // Close new admin admission, then let existing terminal decisions
            // collect both receipts while every data/control service is alive.
            while self.master.as_ref().is_some_and(|master|master.lock().actor.active_plan().is_some()) {
                thread::sleep(Duration::from_millis(20));
            }
            std::fs::File::create(&control_done).map_err(|_|Status::Io)?;
        }
        wait_for_shutdown_marker(&control_done)
    }
    fn handle(&self, operation: u32, payload: Vec<u8>) -> Result<Vec<u8>,Status> {
        if operation == wire::HELLO { return if payload.is_empty() {Ok(self.hello.clone())} else {Err(Status::Invalid)}; }
        if operation == wire::SHUTDOWN_TOKEN {
            if self.owner != 0 || !payload.is_empty() {return Err(Status::Invalid);}
            return self.shutdown.lock().as_ref().map(|(_,token)|token.as_bytes().to_vec())
                .ok_or(Status::Retry);
        }
        if operation == wire::SNAPSHOT {
            if !payload.is_empty() || self.owner != 0 { return Err(Status::Invalid); }
            return if let Some(master) = &self.master { wire::snapshot(&master.lock().actor) }
                else { routing_codec::encode(&*runtime().snapshot()?) };
        }
        if matches!(operation,wire::ADMIN_BEGIN | wire::ADMIN_POLL | wire::ADMIN_ABORT) { return self.admin(operation,payload); }
        if !self.migration { return Err(Status::Invalid); }
        if operation == wire::CAPTURE {
            let (plan,after,encoded) = wire::decode_capture(&payload,self.nodes)?;
            let local = self.local.lock();
            if local.plans.get(&plan.generation).map(Vec::as_slice) != Some(encoded) { return Err(Status::Retry); }
            let mut rows = Vec::new();
            let mut bytes = 20usize;
            let mut eof = false;
            while rows.len() < wire::COPY_ROWS && bytes < wire::COPY_BYTES {
                let row = transfer::capture(&local.participant,&local.store,&plan,rows.last().or(after.as_ref()))?;
                match row {
                    None => {eof=true;break;},
                    Some(row) => { bytes = bytes.checked_add(24).and_then(|x|x.checked_add(row.coordinate.len()))
                        .and_then(|x|x.checked_add(row.key.len())).and_then(|x|x.checked_add(row.value.len())).ok_or(Status::Exhausted)?;
                        rows.push(row); }
                }
            }
            return wire::encode_rows(plan.generation,&rows,eof);
        }
        let plan = wire::decode_plan(&payload,self.nodes)?;
        let mut local = self.local.lock();
        if local.plans.get(&plan.generation).is_some_and(|old|old != &payload) { return Err(Status::Invalid); }
        let mut result = if let Some(command) = wire::command(operation) {
            local.participant.deliver(&plan,command)
        } else if operation == wire::DRAIN {
            local.participant.drain(&plan)
        } else if operation == wire::FINAL_COPY {
            if local.ready.contains(&plan.generation) {
                ControlResult { status: Status::Ok, certificate: Some(Certificate::Ready),cleanup: Cleanup::None }
            } else {
                let source = RemoteSource::new(self.host,&payload);
                let Local {participant,store,..} = &mut *local;
                transfer::execute_final(participant,store,&plan,&source)
            }
        } else { return Err(Status::Invalid); };
        if result.status == Status::Ok {
            local.plans.entry(plan.generation).or_insert(payload);
            if result.certificate == Some(Certificate::Ready) {local.ready.insert(plan.generation);}
            if result.cleanup != Cleanup::None {
                // Delivery already changed metadata. An I/O failure here is NOT
                // a whole-handler stutter; its retained cleanup is resumed later.
                let Local {participant,store,..} = &mut *local;
                result = transfer::execute_cleanup(participant,store,&plan);
            }
        }
        checked(result.status)?;
        wire::encode_control(plan.generation,&result)
    }
    fn admin(&self, operation: u32, payload: Vec<u8>) -> Result<Vec<u8>,Status> {
        let master = self.master.as_ref().ok_or(Status::Invalid)?;
        if operation == wire::ADMIN_BEGIN {
            if !self.migration { return Err(Status::Invalid); }
            let request = wire::decode_begin(&payload)?;
            let nonce = (request.nonce.client,request.nonce.sequence);
            {
                let mut master = master.lock();
                if let Some(record) = master.requests.get(&nonce) {
                    if record.request != payload {return Err(Status::Invalid);}
                    let generation = record.generation;
                    return admin_result(&mut master,generation,request.nonce);
                }
                if master.actor.active_plan().is_some() {return Err(Status::Busy);}
            }
            // No self RPC on this worker. Every other fixed peer must have
            // completed its real load and published the identical catalog.
            for owner in 1..self.nodes {
                if self.host.peer(owner,wire::HELLO,&[])? != self.hello {return Err(Status::Invalid);}
            }
            let table = {
                let catalog = runtime().catalog.read();
                let table = catalog.by_name(&request.table_name).ok_or(Status::NotFound)?;
                if table.kind != TableKind::Governed {return Err(Status::Invalid);}
                table.id
            };
            let mut master = master.lock();
            if self.quiescing.load(Ordering::Acquire) {return Err(Status::Retry);}
            let generation = master.actor.begin(request.nonce,request.source,request.destination,
                KeyRange {table,lo:request.lo,hi:request.hi})?;
            master.requests.insert(nonce,AdminRecord {generation,request:payload});
            self.migrations.lock().as_ref().ok_or(Status::Retry)?.send(generation).map_err(|_|Status::Retry)?;
            admin_result(&mut master,generation,request.nonce)
        } else {
            let nonce = wire::decode_nonce(&payload)?;
            let mut master = master.lock();
            let generation = master.requests.get(&(nonce.client,nonce.sequence)).ok_or(Status::NotFound)?.generation;
            if operation == wire::ADMIN_ABORT && master.actor.active_plan().is_some_and(|p|p.generation == generation)
                && !matches!(master.actor.current_phase(),Some(Phase::Committed | Phase::Aborted)) {
                checked(master.actor.abort())?;
            }
            admin_result(&mut master,generation,nonce)
        }
    }
    fn abort_requested(&self) -> bool {
        self.stopping.load(Ordering::Acquire) || self.master.as_ref().is_some_and(|m|
            matches!(m.lock().actor.current_phase(),Some(Phase::Aborted)))
    }
    fn control(&self, generation: u64, owner: u32, command: Command, operation: u32,
               encoded: &[u8], expected: Option<Certificate>) -> Result<(),Status> {
        if !self.master.as_ref().ok_or(Status::Invalid)?.lock().actor.command_issued(generation,command) {
            return Err(Status::Invalid);
        }
        let reply = self.host.peer(owner,operation,encoded)?;
        let certificate = wire::decode_certificate(&reply,generation)?;
        if certificate != expected {return Err(Status::Invalid);}
        if let Some(certificate) = certificate {
            checked(self.master.as_ref().ok_or(Status::Invalid)?.lock().actor.receive(generation,owner,certificate))?;
        }
        Ok(())
    }
    fn drive_before_commit(&self, generation: u64, source: u32, destination: u32, encoded: &[u8]) -> Result<(),Status> {
        let master = self.master.as_ref().ok_or(Status::Invalid)?;
        if self.abort_requested() {return Err(Status::Retry);}
        self.control(generation,destination,Command::Start,wire::command_operation(Command::Start),encoded,None)?;
        checked(master.lock().actor.request_freeze())?;
        self.control(generation,source,Command::Freeze,wire::command_operation(Command::Freeze),encoded,None)?;
        loop {
            if self.abort_requested() {return Err(Status::Retry);}
            match self.control(generation,source,Command::Freeze,wire::DRAIN,encoded,Some(Certificate::Drained)) {
                Ok(()) => break,
                Err(Status::Busy | Status::Retry) => thread::sleep(Duration::from_millis(10)),
                Err(error) => return Err(error),
            }
        }
        checked(master.lock().actor.request_final())?;
        self.control(generation,destination,Command::Final,wire::command_operation(Command::Final),encoded,None)?;
        self.control(generation,destination,Command::Final,wire::FINAL_COPY,encoded,Some(Certificate::Ready))?;
        checked(master.lock().actor.request_retire())?;
        self.control(generation,source,Command::Retire,wire::command_operation(Command::Retire),encoded,Some(Certificate::Retired))?;
        let mut master = master.lock();
        checked(master.actor.commit())?;
        runtime().install(&wire::snapshot(&master.actor)?)
    }
    fn drive(&self, generation: u64) {
        let Some(master) = &self.master else {return;};
        let prepared = (|| {
            let master = master.lock();
            let plan = master.actor.plan(generation).ok_or(Status::NotFound)?;
            Ok::<_,Status>((plan.source,plan.destination,wire::encode_plan(plan)?))
        })();
        let Ok((source,destination,encoded)) = prepared else {return;};
        let result = self.drive_before_commit(generation,source,destination,&encoded);
        let command = {
            let mut master = master.lock();
            if matches!(master.actor.current_phase(),Some(Phase::Committed)) {Command::Commit}
            else {
                if result.is_err() && !matches!(master.actor.current_phase(),Some(Phase::Aborted)) {
                    if master.actor.abort() != Status::Ok {return;}
                }
                Command::Abort
            }
        };
        // Both decisions are retained. Abort source first to invalidate frozen
        // capture, then destination; commit never rolls back after publication.
        for (owner,certificate) in [(source,Certificate::SourceDone),(destination,Certificate::DestinationDone)] {
            while !self.stopping.load(Ordering::Acquire) {
                if self.control(generation,owner,command,wire::command_operation(command),&encoded,Some(certificate)).is_ok() {break;}
                thread::sleep(Duration::from_millis(20));
            }
            if self.stopping.load(Ordering::Acquire) {return;}
        }
        let _ = master.lock().actor.finish();
    }
}
fn wait_for_shutdown_marker(path: &std::path::Path) -> Result<(),Status> {
    loop {
        match std::fs::metadata(path) {
            Ok(metadata) => return if metadata.is_file() {Ok(())} else {Err(Status::Invalid)},
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                thread::sleep(Duration::from_millis(20));
            },
            Err(_) => return Err(Status::Io),
        }
    }
}
fn admin_result(master: &mut Master, generation: u64, nonce: TxnId) -> Result<Vec<u8>,Status> {
    // Report terminal completion only after both real cleanup/activation receipts.
    let active = master.actor.active_plan().is_some_and(|p|p.generation == generation);
    let outcome = if active {None} else {master.actor.reply(nonce)};
    wire::encode_outcome(generation,outcome)
}
struct EngineStore { owner: u32, host: Host }
impl EngineStore {
    fn materialize(&self, table: u64) -> Result<(),Status> {
        let coordinates = runtime().catalog.read().by_id(table).ok_or(Status::NotFound)?.coordinates;
        if coordinates == Coordinates::Raw {runtime().raw_handle(table,self.owner,false,self.host)?;}
        Ok(())
    }
}
// Exact ordered scans/atomic one-key effects are the stated engine kernel
// premise. Authority checks are NOT trusted here: transfer::GuardedStore owns
// them and holds the same participant borrow throughout every physical effect.
impl RawStore for EngineStore {
    fn scan(&self, range: &KeyRange, after: Option<&Row>) -> Result<Option<Row>,Status> {
        self.materialize(range.table)?;
        self.host.scan(range,after)
    }
    fn put(&mut self, range: &KeyRange, row: &Row) -> Status {self.host.put(range.table,row)}
    fn delete(&mut self, range: &KeyRange, row: &Row) -> Status {self.host.remove(range.table,row)}
}
struct RemoteSource<'a> { host: Host, encoded: &'a [u8], page: Mutex<(std::vec::IntoIter<Row>,bool)> }
impl<'a> RemoteSource<'a> {
    fn new(host: Host, encoded: &'a [u8]) -> Self {Self {host,encoded,page:Mutex::new((Vec::new().into_iter(),false))}}
}
impl Source for RemoteSource<'_> {
    fn scan(&self, plan: &MigrationPlan, after: Option<&Row>) -> Result<Option<Row>,Status> {
        let mut page = self.page.lock();
        if let Some(row) = page.0.next() {return Ok(Some(row));}
        if page.1 {return Ok(None);}
        let bytes = self.host.peer(plan.source,wire::CAPTURE,&wire::capture_request(self.encoded,after)?)?;
        let (rows,eof) = wire::decode_rows(&bytes,plan,after)?;
        *page = (rows.into_iter(),eof);
        Ok(page.0.next())
    }
}
