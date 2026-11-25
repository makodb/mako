//! Shared application tables and the immutable admission view held by a lane.
use crate::{
    epoch::{EpochDomain, Pin},
    wire::*,
};
use mako_sharding::{
    catalog::{warehouse_coordinate, Coordinates},
    epoch_admission::AdmissionSnapshot,
    runtime::runtime,
    types::{Grant, RoutedGrant, Status},
};
use parking_lot::RwLock;
use std::{
    collections::BTreeMap,
    sync::{atomic::AtomicU32, Arc, OnceLock},
};
use sto_tpcc_ffi::native as sto;

pub fn engine(error: sto::Error) -> Error {
    let code = match error.kind {
        sto::ErrorKind::Conflict => ErrorCode::Conflict,
        sto::ErrorKind::Capacity => ErrorCode::Capacity,
        sto::ErrorKind::UnknownOutcome => ErrorCode::Unknown,
        sto::ErrorKind::CommittedFailure => ErrorCode::CommittedFailure,
        sto::ErrorKind::InvalidUse => ErrorCode::Invalid,
        sto::ErrorKind::Fatal => ErrorCode::Fatal,
    };
    Error::new(code, error.message)
}
pub fn shard_error(status: Status) -> Error {
    Error::new(
        match status {
            Status::Retry | Status::NotFound => ErrorCode::RouteChanged,
            Status::Busy => ErrorCode::Busy,
            Status::Exhausted => ErrorCode::Capacity,
            Status::Invalid => ErrorCode::Invalid,
            _ => ErrorCode::Fatal,
        },
        format!("sharding: {status:?}"),
    )
}
fn check(status: Status) -> Result<()> {
    if status == Status::Ok {
        Ok(())
    } else {
        Err(shard_error(status))
    }
}

pub use mako_protocol::contracts::{CommitLog, CommitPermit};
pub use mako_protocol::journal::MemoryLog;
use mako_protocol::journal::{Entry, Journal, Record, TransactionId};

struct StoreJournal {
    log: Arc<dyn CommitLog>,
}
impl Journal for StoreJournal {
    fn durable(&self) -> bool {
        self.log.durable()
    }
    fn apply(&self, record: &Record) -> Result<Entry> {
        self.log.health()?;
        self.log.apply(record)
    }
    fn lookup(&self, id: TransactionId) -> Result<Option<Entry>> {
        self.log.health()?;
        self.log.lookup(id)
    }
    fn entries(&self) -> Result<Vec<Entry>> {
        self.log.health()?;
        self.log.entries()
    }
}

#[derive(Clone)]
pub struct Table {
    pub tree: sto::Table,
    pub log_id: u16,
}
pub struct Store {
    pub database: sto::Database,
    pub owner: u32,
    pub migration: bool,
    pub epochs: Arc<EpochDomain>,
    pub term: AtomicU32,
    pub log: Arc<dyn CommitLog>,
    pub journal: Arc<dyn Journal>,
    pub(crate) participants: OnceLock<Result<Arc<crate::receiver::ParticipantService>>>,
    pub(crate) endpoint: RwLock<Option<String>>,
    fault: Arc<RwLock<Option<Error>>>,
    tables: RwLock<BTreeMap<TableKey, Table>>,
    quarantine: parking_lot::Mutex<Vec<Protection>>,
    materializer: RwLock<Option<Arc<Materializer>>>,
}
type Materializer = dyn Fn(TableKey) -> Result<Table> + Send + Sync;
impl Store {
    pub fn new(owner: u32, migration: bool, log: Arc<dyn CommitLog>) -> Result<Arc<Self>> {
        Ok(Arc::new(Self {
            database: sto::Database::new(sto::DatabaseConfig::default()).map_err(engine)?,
            owner,
            migration,
            epochs: Arc::new(EpochDomain::default()),
            term: AtomicU32::new(0),
            journal: Arc::new(StoreJournal { log: log.clone() }),
            participants: OnceLock::new(),
            endpoint: RwLock::new(None),
            fault: Arc::new(RwLock::new(None)),
            log,
            tables: RwLock::new(BTreeMap::new()),
            quarantine: parking_lot::Mutex::new(Vec::new()),
            materializer: RwLock::new(None),
        }))
    }
    pub fn open(&self, worker: &mut sto::Worker, key: TableKey, log_id: u16) -> Result<Table> {
        if !worker.belongs_to(&self.database) {
            return Err(Error::new(
                ErrorCode::Invalid,
                "worker belongs to another database",
            ));
        }
        let mut tables = self.tables.write();
        if let Some(table) = tables.get(&key) {
            if table.log_id != log_id {
                return Err(Error::new(ErrorCode::Invalid, "table log ID changed"));
            }
            return Ok(table.clone());
        }
        if tables.values().any(|table| table.log_id == log_id) {
            return Err(Error::new(
                ErrorCode::Invalid,
                "duplicate physical table log ID",
            ));
        }
        let table = Table {
            tree: worker
                .create_table(sto::TableConfig::default())
                .map_err(engine)?,
            log_id,
        };
        tables.insert(key, table.clone());
        Ok(table)
    }
    pub fn table(&self, key: TableKey) -> Result<Table> {
        if let Some(table) = self.existing(key) {
            return Ok(table);
        }
        let materializer = self.materializer.read().clone();
        materializer
            .ok_or_else(|| Error::new(ErrorCode::Invalid, "table is not open on this shard"))?(
            key
        )
    }
    pub(crate) fn existing(&self, key: TableKey) -> Option<Table> {
        self.tables.read().get(&key).cloned()
    }
    pub(crate) fn set_materializer(&self, materializer: Arc<Materializer>) -> Result<()> {
        let mut slot = self.materializer.write();
        if slot.is_some() {
            return Err(Error::new(
                ErrorCode::Invalid,
                "table materializer already installed",
            ));
        }
        *slot = Some(materializer);
        Ok(())
    }
    pub(crate) fn quarantine(&self, pin: Protection) {
        self.quarantine.lock().push(pin);
    }
    pub fn tables(&self) -> Vec<(TableKey, Table)> {
        self.tables
            .read()
            .iter()
            .map(|(key, table)| (*key, table.clone()))
            .collect()
    }
    pub fn protect(&self) -> Result<Protection> {
        self.health()?;
        let permit = self.log.admit()?;
        let pin = self.epochs.enter();
        let snapshot = if self.migration {
            Some(
                runtime()
                    .node(self.owner)
                    .and_then(|node| node.admission_view())
                    .map_err(shard_error)?,
            )
        } else {
            None
        };
        Ok(Protection {
            fault: self.fault.clone(),
            permit,
            _pin: pin,
            snapshot,
            owner: self.owner,
        })
    }
    pub(crate) fn health(&self) -> Result<()> {
        self.fault
            .read()
            .clone()
            .map_or_else(|| self.log.health(), Err)
    }
    pub fn shutdown(&self) {
        self.fence(Error::new(ErrorCode::Closed, "shard shutdown"));
        if let Some(Ok(service)) = self.participants.get() {
            service.shutdown();
        }
    }
    pub(crate) fn fence(&self, error: Error) {
        self.fault.write().get_or_insert(Error::new(
            ErrorCode::Unknown,
            format!("shard requires recovery: {error}"),
        ));
    }
    pub(crate) fn table_for_log(&self, id: u16) -> Result<Table> {
        self.tables
            .read()
            .values()
            .find(|table| table.log_id == id)
            .cloned()
            .ok_or_else(|| Error::new(ErrorCode::Invalid, "prepared table is not open"))
    }
    pub(crate) fn gateway_receipt(
        &self,
        request: &GatewayRequest,
    ) -> Result<Option<GatewayResponse>> {
        self.health()?;
        // Also recognizes receipts written by the preceding row-log format.
        if let Some(response) = self.log.gateway_receipt(request)? {
            return Ok(Some(response));
        }
        for entry in self.journal.entries()? {
            let Some(participant) = entry.participant else {
                continue;
            };
            let Some(receipt) = participant.receipt else {
                continue;
            };
            if receipt.request.client != request.client
                || receipt.request.sequence != request.sequence
            {
                continue;
            }
            if !receipt.matches(request) {
                return Err(Error::new(
                    ErrorCode::Invalid,
                    "gateway identity reused with different intent",
                ));
            }
            match participant.outcome {
                Some(mako_protocol::journal::Outcome::Commit) => return Ok(Some(receipt.response)),
                Some(mako_protocol::journal::Outcome::Abort) => (),
                None => {
                    return Err(Error::new(
                        ErrorCode::Unknown,
                        "gateway transaction needs decision recovery",
                    ))
                }
            }
        }
        Ok(None)
    }
}
impl Drop for Store {
    fn drop(&mut self) {
        if let Some(Ok(service)) = self.participants.get() {
            service.shutdown();
        }
    }
}

pub struct Protection {
    fault: Arc<RwLock<Option<Error>>>,
    permit: Box<dyn CommitPermit>,
    _pin: Pin,
    snapshot: Option<Arc<AdmissionSnapshot>>,
    owner: u32,
}
pub(crate) struct ScanSegment {
    pub lower: Vec<u8>,
    pub upper: Option<Vec<u8>>,
    pub route: Route,
}
impl Protection {
    pub(crate) fn check(&self) -> Result<()> {
        if let Some(error) = self.fault.read().as_ref() {
            return Err(error.clone());
        }
        self.permit.check()
    }
    pub(crate) fn captured_route(&self, table: u64, coordinate: &[u8]) -> Result<Route> {
        let snapshot = self
            .snapshot
            .as_ref()
            .ok_or_else(|| Error::new(ErrorCode::Invalid, "no routing admission captured"))?;
        let route = snapshot.route(table, coordinate).map_err(shard_error)?;
        Ok(Route {
            owner: route.grant.owner,
            ownership_version: route.grant.ownership_version,
            snapshot_version: route.snapshot_version,
        })
    }
    pub(crate) fn segments(
        &self,
        table: u64,
        lower: &[u8],
        upper: Option<&[u8]>,
        reverse: bool,
    ) -> Result<Vec<ScanSegment>> {
        let snapshot = self
            .snapshot
            .as_ref()
            .ok_or_else(|| Error::new(ErrorCode::Invalid, "no routing admission captured"))?
            .routing();
        let mut segments = Vec::new();
        let mut push = |segment: mako_sharding::routing::ScanSegment<'_>| {
            segments.push(ScanSegment {
                lower: segment.lo.to_vec(),
                upper: segment.hi.map(<[u8]>::to_vec),
                route: Route {
                    owner: segment.grant.owner,
                    ownership_version: segment.grant.ownership_version,
                    snapshot_version: snapshot.version,
                },
            })
        };
        if reverse {
            let mut cursor = snapshot
                .reverse_segments(table, lower, upper)
                .map_err(shard_error)?;
            while let Some(segment) = cursor.next() {
                push(segment);
            }
        } else {
            let mut cursor = snapshot
                .segments(table, lower, upper)
                .map_err(shard_error)?;
            while let Some(segment) = cursor.next() {
                push(segment);
            }
        }
        Ok(segments)
    }
    fn route(&self, access: &Access) -> Result<Option<RoutedGrant>> {
        self.check()?;
        let governed = if self.snapshot.is_some() {
            self.coordinates(access)?;
            runtime()
                .catalog
                .read()
                .by_id(access.table.table)
                .is_some_and(|entry| entry.kind == mako_sharding::catalog::TableKind::Governed)
        } else {
            false
        };
        if let Some(route) = access.route {
            if route.owner != self.owner {
                return Err(Error::new(
                    ErrorCode::RouteChanged,
                    "request belongs to another shard",
                ));
            }
            Ok(governed.then_some(RoutedGrant {
                grant: Grant {
                    owner: route.owner,
                    ownership_version: route.ownership_version,
                },
                snapshot_version: route.snapshot_version,
            }))
        } else if governed {
            Err(Error::new(
                ErrorCode::RouteChanged,
                "routed admission is required",
            ))
        } else {
            Ok(None)
        }
    }
    fn coordinates(&self, access: &Access) -> Result<Coordinates> {
        let catalog = runtime().catalog.read();
        let entry = catalog
            .by_id(access.table.table)
            .ok_or_else(|| Error::new(ErrorCode::Invalid, "table is absent from catalog"))?;
        match entry.coordinates {
            Coordinates::Raw if access.table.warehouse != 0 => Err(Error::new(
                ErrorCode::Invalid,
                "warehouse supplied for raw table",
            )),
            Coordinates::Warehouse if access.table.warehouse == 0 => {
                Err(Error::new(ErrorCode::Invalid, "warehouse is required"))
            }
            coordinates => Ok(coordinates),
        }
    }
    pub fn point(&self, access: &Access, key: &[u8]) -> Result<()> {
        let route = self.route(access)?;
        if let (Some(snapshot), Some(route)) = (&self.snapshot, route) {
            let coordinate = match self.coordinates(access)? {
                Coordinates::Raw => key.to_vec(),
                Coordinates::Warehouse => warehouse_coordinate(access.table.warehouse).to_vec(),
            };
            check(snapshot.point(access.table.table, &coordinate, route))?;
        }
        Ok(())
    }
    pub fn range(&self, access: &Access, lower: &Bound, upper: &Bound) -> Result<()> {
        let route = self.route(access)?;
        if let (Some(snapshot), Some(route)) = (&self.snapshot, route) {
            if self.coordinates(access)? == Coordinates::Warehouse {
                return self.point(access, &[]);
            }
            let lo = match lower {
                Bound::Unbounded => &[][..],
                Bound::Included(key) | Bound::Excluded(key) => key,
            };
            // Include the final key by authorizing it separately. No invented
            // byte successor is safe for arbitrary binary keys.
            let hi = match upper {
                Bound::Unbounded => None,
                Bound::Included(key) => {
                    check(snapshot.point(access.table.table, key, route))?;
                    Some(key.as_slice())
                }
                Bound::Excluded(key) => Some(key.as_slice()),
            };
            if hi != Some(lo) {
                check(snapshot.range(access.table.table, lo, hi, route))?;
            } else {
                check(snapshot.point(access.table.table, lo, route))?;
            }
        }
        Ok(())
    }
}
