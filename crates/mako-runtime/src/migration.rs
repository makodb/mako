//! Native host for the existing sharding actors and verified storage verbs.
//! The actor owns routing/control decisions; this module owns tables, affine
//! storage workers, admission epochs and SRPC completion lifetimes.
use crate::{
    client::Target,
    rpc::{Network, Respond},
    storage::{engine, shard_error, Store, Table},
    wire::{Access, Error, ErrorCode, Message, Reply, Result, Route, TableKey},
};
use mako_sharding::{
    catalog::{Coordinates, TableKind},
    host::{Bytes, Completion, Host, HostAbi, Opener, ReplySink, RowSink},
    runtime::runtime,
    types::Status,
    wire,
};
use parking_lot::Mutex;
use std::{
    cell::RefCell,
    collections::{BTreeMap, BTreeSet},
    ffi::c_void,
    panic::{catch_unwind, AssertUnwindSafe},
    sync::{
        atomic::{AtomicUsize, Ordering},
        mpsc::{self, SyncSender},
        Arc,
    },
    thread::{self, JoinHandle},
    time::Duration,
};
use sto_tpcc_ffi::native::migration::{MigrationWorker, Registry};

#[derive(Clone, Copy, Debug, Eq, PartialEq, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Kind {
    Governed,
    Replicated,
    Static,
}
#[derive(Clone, Debug, Eq, PartialEq, serde::Serialize, serde::Deserialize)]
#[serde(deny_unknown_fields)]
pub struct TableSpec {
    pub id: u64,
    pub name: String,
    pub kind: Kind,
    pub warehouse: bool,
    pub initial_owner: u32,
}
pub struct Schema {
    tables: BTreeMap<u64, (TableSpec, u16)>,
    pub nodes: u32,
    pub per_shard: u32,
    pub total: u32,
}
impl Schema {
    pub fn new(mut tables: Vec<TableSpec>, nodes: u32, per_shard: u32) -> Result<Arc<Self>> {
        if nodes == 0 {
            return Err(Error::new(ErrorCode::Invalid, "schema has no shards"));
        }
        let total = nodes
            .checked_mul(per_shard)
            .ok_or_else(|| Error::new(ErrorCode::Capacity, "warehouse count overflow"))?;
        tables.sort_by_key(|table| table.id);
        let mut names = BTreeSet::new();
        let mut indexed = BTreeMap::new();
        let mut next = 1u32;
        for table in tables {
            if table.id == 0
                || table.name.is_empty()
                || !names.insert(table.name.clone())
                || table.initial_owner >= nodes
                || (table.warehouse && total == 0)
                || indexed.contains_key(&table.id)
            {
                return Err(Error::new(
                    ErrorCode::Invalid,
                    "invalid or duplicate schema table",
                ));
            }
            let first = u16::try_from(next)
                .map_err(|_| Error::new(ErrorCode::Capacity, "physical table log ID exhausted"))?;
            next = next
                .checked_add(if table.warehouse { total } else { 1 })
                // The retained log format reserves the high bit for deletes.
                .filter(|next| *next <= 0x8000)
                .ok_or_else(|| {
                    Error::new(ErrorCode::Capacity, "physical table log ID exhausted")
                })?;
            indexed.insert(table.id, (table, first));
        }
        if indexed.is_empty() {
            return Err(Error::new(ErrorCode::Invalid, "schema has no tables"));
        }
        Ok(Arc::new(Self {
            tables: indexed,
            nodes,
            per_shard,
            total,
        }))
    }
    pub fn install(&self) -> Result<()> {
        for (table, _) in self.tables.values() {
            checked(runtime().catalog_register(
                table.id,
                table.name.as_bytes(),
                kind(table.kind),
                if table.warehouse {
                    Coordinates::Warehouse
                } else {
                    Coordinates::Raw
                },
                table.initial_owner,
            ))?;
        }
        if self.tables.values().any(|(table, _)| table.warehouse) {
            runtime()
                .warehouse_init(self.per_shard, self.total)
                .map_err(shard_error)?;
        }
        Ok(())
    }
    pub fn spec(&self, table: u64) -> Result<&TableSpec> {
        self.tables
            .get(&table)
            .map(|(table, _)| table)
            .ok_or_else(|| Error::new(ErrorCode::Invalid, "table is absent from schema"))
    }
    pub fn named(&self, name: &str) -> Result<u64> {
        self.tables
            .values()
            .find(|(table, _)| table.name == name)
            .map(|(table, _)| table.id)
            .ok_or_else(|| Error::new(ErrorCode::Invalid, "unknown table name"))
    }
    pub fn log_id(&self, key: TableKey) -> Result<u16> {
        let (table, base) = self
            .tables
            .get(&key.table)
            .ok_or_else(|| Error::new(ErrorCode::Invalid, "table is absent from schema"))?;
        if table.warehouse {
            if key.warehouse == 0 || key.warehouse > self.total {
                return Err(Error::new(
                    ErrorCode::Invalid,
                    "warehouse is outside schema",
                ));
            }
            u16::try_from(u32::from(*base) + key.warehouse - 1)
                .map_err(|_| Error::new(ErrorCode::Capacity, "table log ID exhausted"))
        } else if key.warehouse == 0 {
            Ok(*base)
        } else {
            Err(Error::new(ErrorCode::Invalid, "warehouse on raw table"))
        }
    }
    pub fn key_for_log(&self, log_id: u16) -> Result<TableKey> {
        for (table, base) in self.tables.values() {
            let width = if table.warehouse { self.total } else { 1 };
            if u32::from(log_id) >= u32::from(*base) && u32::from(log_id) < u32::from(*base) + width
            {
                return Ok(TableKey {
                    table: table.id,
                    warehouse: if table.warehouse {
                        u32::from(log_id - base) + 1
                    } else {
                        0
                    },
                });
            }
        }
        Err(Error::new(
            ErrorCode::Invalid,
            "unknown physical table log ID",
        ))
    }
    pub fn target(
        &self,
        key: TableKey,
        lookup_key: &[u8],
        origin: u32,
        moving: bool,
    ) -> Result<Target> {
        let log_id = self.log_id(key)?;
        let spec = self.spec(key.table)?;
        let route = if moving && spec.kind == Kind::Governed {
            let warehouse = key.warehouse.to_be_bytes();
            let coordinate = if spec.warehouse {
                warehouse.as_slice()
            } else {
                lookup_key
            };
            let captured = runtime()
                .route(key.table, coordinate)
                .map_err(shard_error)?;
            Some(Route {
                owner: captured.grant.owner,
                ownership_version: captured.grant.ownership_version,
                snapshot_version: captured.snapshot_version,
            })
        } else {
            None
        };
        let owner = route.map_or_else(
            || {
                if spec.kind == Kind::Replicated {
                    origin
                } else if spec.warehouse {
                    (key.warehouse - 1) / self.per_shard
                } else {
                    spec.initial_owner
                }
            },
            |route| route.owner,
        );
        if owner >= self.nodes {
            return Err(Error::new(
                ErrorCode::Invalid,
                "target is outside shard topology",
            ));
        }
        Ok(Target {
            owner,
            access: Access { table: key, route },
            log_id,
        })
    }
}
fn kind(kind: Kind) -> TableKind {
    match kind {
        Kind::Governed => TableKind::Governed,
        Kind::Replicated => TableKind::Replicated,
        Kind::Static => TableKind::Static,
    }
}
fn checked(status: Status) -> Result<()> {
    if status == Status::Ok {
        Ok(())
    } else {
        Err(shard_error(status))
    }
}
fn status(error: Error) -> Status {
    match error.code {
        ErrorCode::RouteChanged | ErrorCode::Conflict => Status::Retry,
        ErrorCode::Busy => Status::Busy,
        ErrorCode::Invalid => Status::Invalid,
        ErrorCode::Capacity => Status::Exhausted,
        _ => Status::Io,
    }
}
fn storage_status(code: u32) -> Result<()> {
    checked(wire::status(code))
}

struct Open {
    key: TableKey,
    log_id: u16,
    reply: SyncSender<Result<Table>>,
}
struct Factory {
    send: Option<SyncSender<Open>>,
    thread: Option<JoinHandle<()>>,
}
impl Factory {
    fn new(store: Arc<Store>) -> Result<Self> {
        let (send, receive) = mpsc::sync_channel::<Open>(64);
        let (ready, wait) = mpsc::sync_channel(1);
        let thread = thread::Builder::new()
            .name(format!("shard-tables-{}", store.owner))
            .spawn(move || {
                let mut worker = match store.database.worker().map_err(engine) {
                    Ok(worker) => worker,
                    Err(error) => {
                        let _ = ready.send(Err(error));
                        return;
                    }
                };
                if ready.send(Ok(())).is_err() {
                    return;
                }
                while let Ok(open) = receive.recv() {
                    let _ = open
                        .reply
                        .send(store.open(&mut worker, open.key, open.log_id));
                }
            })
            .map_err(|e| Error::new(ErrorCode::Fatal, e.to_string()))?;
        let factory = Self {
            send: Some(send),
            thread: Some(thread),
        };
        wait.recv()
            .map_err(|_| Error::new(ErrorCode::Fatal, "table factory stopped during startup"))??;
        Ok(factory)
    }
    fn open(&self, key: TableKey, log_id: u16) -> Result<Table> {
        let (reply, wait) = mpsc::sync_channel(1);
        self.send
            .as_ref()
            .expect("live table factory")
            .try_send(Open { key, log_id, reply })
            .map_err(|_| Error::new(ErrorCode::Busy, "table factory unavailable"))?;
        wait.recv()
            .map_err(|_| Error::new(ErrorCode::Closed, "table factory stopped"))?
    }
}
impl Drop for Factory {
    fn drop(&mut self) {
        self.send.take();
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

struct State {
    store: Arc<Store>,
    registry: Arc<Registry>,
    schema: Arc<Schema>,
    network: Arc<Network>,
    endpoints: BTreeMap<u32, String>,
    factory: Factory,
    tokens: Mutex<BTreeMap<(TableKey, bool), usize>>,
}
static TOKENS: AtomicUsize = AtomicUsize::new(0);
impl State {
    fn materialize(&self, key: TableKey) -> Result<Table> {
        let log_id = self.schema.log_id(key)?;
        let table = if let Some(table) = self.store.existing(key) {
            table
        } else {
            self.factory.open(key, log_id)?
        };
        if table.log_id != log_id {
            return Err(Error::new(
                ErrorCode::Invalid,
                "physical table differs from schema",
            ));
        }
        let warehouse = if self.schema.spec(key.table)?.warehouse {
            Some((key.warehouse, self.schema.total))
        } else {
            None
        };
        self.registry
            .register(
                key.table,
                warehouse,
                mako_storage_masstree::native_table(&table.tree).map_err(engine)?,
                log_id,
            )
            .map_err(|code| shard_error(wire::status(code)))?;
        Ok(table)
    }
    fn token(&self, key: TableKey, proxy: bool) -> Result<usize> {
        self.materialize(key)?;
        let mut tokens = self.tokens.lock();
        if let Some(token) = tokens.get(&(key, proxy)) {
            return Ok(*token);
        }
        let token = TOKENS
            .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |last| {
                last.checked_add(1)
            })
            .map_err(|_| Error::new(ErrorCode::Capacity, "table handle identity exhausted"))?
            + 1;
        tokens.insert((key, proxy), token);
        Ok(token)
    }
}
pub struct RunningMigration {
    state: Arc<State>,
}
impl RunningMigration {
    /// Start after the RPC listener and initial loaders. Node readiness is
    /// asynchronous, so multiple colocated shards can start without deadlock.
    pub fn start(
        store: Arc<Store>,
        schema: Arc<Schema>,
        network: Arc<Network>,
        endpoints: BTreeMap<u32, String>,
    ) -> Result<Self> {
        if !store.migration || store.owner >= schema.nodes {
            return Err(Error::new(
                ErrorCode::Invalid,
                "migration host requires routed store",
            ));
        }
        let registry = Registry::new(
            mako_storage_masstree::native_database(&store.database).map_err(engine)?,
            store.owner,
        )
        .map_err(|code| shard_error(wire::status(code)))?;
        let factory = Factory::new(store.clone())?;
        let state = Arc::new(State {
            store,
            registry,
            schema,
            network,
            endpoints,
            factory,
            tokens: Mutex::new(BTreeMap::new()),
        });
        for (spec, _) in state.schema.tables.values() {
            if !spec.warehouse {
                let key = TableKey {
                    table: spec.id,
                    warehouse: 0,
                };
                for proxy in [false, true] {
                    runtime()
                        .raw_register(spec.id, state.store.owner, proxy, state.token(key, proxy)?)
                        .map_err(shard_error)?;
                }
            }
        }
        for (key, _) in state.store.tables() {
            state.materialize(key)?;
        }
        let anchor = Arc::new(Arc::downgrade(&state));
        let context = Arc::as_ptr(&anchor).cast_mut().cast::<c_void>();
        // SAFETY: Host and Opener own this stable Weak anchor. Every callback
        // upgrades it before access, including callbacks after a failed start.
        let host = unsafe {
            Host::from_owned_abi(
                &HostAbi {
                    context,
                    thread_enter: Some(enter),
                    thread_leave: Some(leave),
                    open: Some(open),
                    scan_next: Some(scan),
                    put: Some(put),
                    remove: Some(remove),
                    peer_call: Some(peer),
                    epoch_cut: Some(cut),
                    epoch_drained: Some(drained),
                },
                anchor.clone(),
            )
        }
        .map_err(shard_error)?;
        if state.schema.total > 0 {
            // SAFETY: same retained State lifetime and owner checks as Host.
            runtime()
                .warehouse_opener(state.store.owner, unsafe {
                    Opener::new_owned(context, open_warehouse, anchor)
                })
                .map_err(shard_error)?;
        }
        let weak = Arc::downgrade(&state);
        state.store.set_materializer(Arc::new(move |key| {
            weak.upgrade()
                .ok_or_else(|| Error::new(ErrorCode::Closed, "migration table owner stopped"))?
                .materialize(key)
        }))?;
        runtime()
            .start(state.store.owner, state.schema.nodes, true, host)
            .map_err(shard_error)?;
        Ok(Self { state })
    }
    pub fn ready(&self) -> bool {
        runtime()
            .node(self.state.store.owner)
            .is_ok_and(|node| node.ready())
    }
    pub fn wait_ready(&self) -> Result<()> {
        runtime()
            .node(self.state.store.owner)
            .and_then(|node| node.wait_ready())
            .map_err(shard_error)
    }
}
impl Drop for RunningMigration {
    fn drop(&mut self) {
        runtime().stop(self.state.store.owner);
    }
}

thread_local! { static WORKER: RefCell<Option<(usize, MigrationWorker)>> = const { RefCell::new(None) }; }
fn boundary(call: impl FnOnce() -> Result<()>) -> u32 {
    match catch_unwind(AssertUnwindSafe(call)) {
        Ok(Ok(())) => Status::Ok as u32,
        Ok(Err(error)) => status(error) as u32,
        Err(_) => Status::Io as u32,
    }
}
unsafe fn state(context: *mut c_void) -> Result<Arc<State>> {
    // SAFETY: Host/Opener own this stable Weak anchor. Upgrading retains the
    // application state through a concurrent shutdown or rejects a dead owner.
    unsafe { &*context.cast::<std::sync::Weak<State>>() }
        .upgrade()
        .ok_or_else(|| Error::new(ErrorCode::Closed, "migration owner stopped"))
}
fn with_worker<T>(
    context: *mut c_void,
    call: impl FnOnce(&mut MigrationWorker) -> Result<T>,
) -> Result<T> {
    WORKER.with(|slot| {
        let mut slot = slot
            .try_borrow_mut()
            .map_err(|_| Error::new(ErrorCode::Busy, "nested migration worker use"))?;
        let (owner, worker) = slot
            .as_mut()
            .ok_or_else(|| Error::new(ErrorCode::Invalid, "migration thread is not entered"))?;
        if *owner != context as usize {
            return Err(Error::new(ErrorCode::Invalid, "foreign migration thread"));
        }
        call(worker)
    })
}
unsafe extern "C" fn enter(context: *mut c_void, owner: u32) -> u32 {
    boundary(|| {
        let state = unsafe { state(context) }?;
        if state.store.owner != owner {
            return Err(Error::new(ErrorCode::Invalid, "foreign shard owner"));
        }
        WORKER.with(|slot| {
            let mut slot = slot
                .try_borrow_mut()
                .map_err(|_| Error::new(ErrorCode::Busy, "migration worker borrowed"))?;
            if slot.is_some() {
                return Err(Error::new(
                    ErrorCode::Invalid,
                    "migration worker already entered",
                ));
            }
            let worker = state
                .registry
                .worker()
                .map_err(|code| shard_error(wire::status(code)))?;
            *slot = Some((context as usize, worker));
            Ok(())
        })
    })
}
unsafe extern "C" fn leave(context: *mut c_void) {
    let _ = boundary(|| {
        WORKER.with(|slot| {
            let mut slot = slot.borrow_mut();
            if slot
                .as_ref()
                .is_some_and(|(owner, _)| *owner == context as usize)
            {
                slot.take();
            }
        });
        Ok(())
    });
}
unsafe extern "C" fn open(
    context: *mut c_void,
    table: u64,
    _: Bytes,
    owner: u32,
    proxy: u32,
    table_kind: u32,
    output: *mut usize,
) -> u32 {
    boundary(|| {
        let state = unsafe { state(context) }?;
        if owner != state.store.owner
            || proxy > 1
            || output.is_null()
            || kind(state.schema.spec(table)?.kind) as u32 != table_kind
        {
            return Err(Error::new(ErrorCode::Invalid, "invalid raw table open"));
        }
        let token = state.token(
            TableKey {
                table,
                warehouse: 0,
            },
            proxy != 0,
        )?;
        unsafe {
            output.write(token);
        }
        Ok(())
    })
}
unsafe extern "C" fn open_warehouse(
    context: *mut c_void,
    table: u64,
    warehouse: u32,
    owner: u32,
    proxy: u32,
    output: *mut usize,
) -> u32 {
    boundary(|| {
        let state = unsafe { state(context) }?;
        if owner != state.store.owner || proxy > 1 || output.is_null() || warehouse == 0 {
            return Err(Error::new(ErrorCode::Invalid, "invalid warehouse open"));
        }
        let token = state.token(TableKey { table, warehouse }, proxy != 0)?;
        unsafe {
            output.write(token);
        }
        Ok(())
    })
}
unsafe extern "C" fn scan(
    context: *mut c_void,
    table: u64,
    lo: Bytes,
    has_hi: u32,
    hi: Bytes,
    has_after: u32,
    after_coordinate: Bytes,
    after_key: Bytes,
    sink: RowSink,
    sink_context: *mut c_void,
) -> u32 {
    let mut found = false;
    let code = boundary(|| {
        if has_hi > 1 || has_after > 1 {
            return Err(Error::new(ErrorCode::Invalid, "invalid scan flags"));
        }
        let lo = unsafe { lo.slice() }.map_err(shard_error)?;
        let hi = if has_hi != 0 {
            Some(unsafe { hi.slice() }.map_err(shard_error)?)
        } else {
            None
        };
        let after = if has_after != 0 {
            Some((
                unsafe { after_coordinate.slice() }.map_err(shard_error)?,
                unsafe { after_key.slice() }.map_err(shard_error)?,
            ))
        } else {
            None
        };
        let row = with_worker(context, |worker| {
            worker
                .scan(table, lo, hi, after)
                .map_err(|code| shard_error(wire::status(code)))
        })?;
        if let Some(row) = row {
            found = true;
            unsafe {
                sink(
                    sink_context,
                    Bytes::from_slice(&row.coordinate),
                    Bytes::from_slice(&row.key),
                    Bytes::from_slice(&row.value),
                );
            }
        }
        Ok(())
    });
    if code == Status::Ok as u32 && !found {
        Status::NotFound as u32
    } else {
        code
    }
}
unsafe fn write(
    context: *mut c_void,
    table: u64,
    coordinate: Bytes,
    key: Bytes,
    value: Option<Bytes>,
) -> u32 {
    boundary(|| {
        let state = unsafe { state(context) }?;
        let coordinate = unsafe { coordinate.slice() }.map_err(shard_error)?;
        let key = unsafe { key.slice() }.map_err(shard_error)?;
        let warehouse = if state.schema.spec(table)?.warehouse {
            u32::from_be_bytes(
                coordinate
                    .try_into()
                    .map_err(|_| Error::new(ErrorCode::Invalid, "invalid warehouse coordinate"))?,
            )
        } else {
            0
        };
        state.materialize(TableKey { table, warehouse })?;
        with_worker(context, |worker| {
            storage_status(
                match value {
                    Some(value) => worker.put(
                        table,
                        coordinate,
                        key,
                        unsafe { value.slice() }.map_err(shard_error)?,
                    ),
                    None => worker.remove(table, coordinate, key),
                }
                .err()
                .unwrap_or(Status::Ok as u32),
            )
        })
    })
}
unsafe extern "C" fn put(
    context: *mut c_void,
    table: u64,
    coordinate: Bytes,
    key: Bytes,
    value: Bytes,
) -> u32 {
    unsafe { write(context, table, coordinate, key, Some(value)) }
}
unsafe extern "C" fn remove(
    context: *mut c_void,
    table: u64,
    coordinate: Bytes,
    key: Bytes,
) -> u32 {
    unsafe { write(context, table, coordinate, key, None) }
}
unsafe extern "C" fn peer(
    context: *mut c_void,
    owner: u32,
    operation: u32,
    payload: Bytes,
    sink: ReplySink,
    sink_context: *mut c_void,
) -> u32 {
    boundary(|| {
        let state = unsafe { state(context) }?;
        let payload = unsafe { payload.slice() }.map_err(shard_error)?.to_vec();
        let endpoint = state
            .endpoints
            .get(&owner)
            .ok_or_else(|| Error::new(ErrorCode::Invalid, "missing migration peer"))?;
        match state.network.call(
            endpoint,
            Message::Migration {
                owner,
                operation,
                payload,
            },
            Duration::from_secs(3),
        )? {
            Reply::Migration { status, payload } => {
                unsafe {
                    sink(sink_context, status, Bytes::from_slice(&payload));
                }
                Ok(())
            }
            Reply::Error(error) => Err(error),
            _ => Err(Error::new(
                ErrorCode::Transport,
                "unexpected migration peer reply",
            )),
        }
    })
}
unsafe extern "C" fn cut(context: *mut c_void) -> u64 {
    catch_unwind(AssertUnwindSafe(|| {
        unsafe { state(context) }
            .and_then(|state| state.store.epochs.cut())
            .unwrap_or(0)
    }))
    .unwrap_or(0)
}
unsafe extern "C" fn drained(context: *mut c_void, cut: u64) -> u32 {
    boundary(|| {
        if unsafe { state(context) }?.store.epochs.drained(cut)? {
            Ok(())
        } else {
            Err(Error::new(
                ErrorCode::Busy,
                "prior admission epochs are active",
            ))
        }
    })
}

pub(crate) fn dispatch(owner: u32, operation: u32, payload: Vec<u8>, respond: Respond) {
    let pointer = Box::into_raw(Box::new(respond));
    // SAFETY: a successful enqueue transfers this Box exactly once to complete;
    // a failed enqueue never invokes complete, so this function reclaims it.
    let completion = unsafe { Completion::new(complete, pointer.cast()) };
    if let Err(error) = runtime().dispatch(owner, operation, payload, completion) {
        let respond = unsafe { Box::from_raw(pointer) };
        respond(Reply::Migration {
            status: error as u32,
            payload: Vec::new(),
        });
    }
}
unsafe extern "C" fn complete(context: *mut c_void, status: u32, payload: Bytes) {
    // SAFETY: the sharding runtime fires its transferred completion once.
    let respond = unsafe { Box::from_raw(context.cast::<Respond>()) };
    let _ = catch_unwind(AssertUnwindSafe(|| match unsafe { payload.slice() } {
        Ok(payload) => respond(Reply::Migration {
            status,
            payload: payload.to_vec(),
        }),
        Err(error) => respond(Reply::Migration {
            status: error as u32,
            payload: Vec::new(),
        }),
    }));
}
