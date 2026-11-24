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
    sync::{atomic::AtomicU32, Arc},
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

pub trait CommitLog: Send + Sync {
    /// Return success only after the configured durability contract is met.
    /// A failure occurs after local installation and therefore fences the lane.
    fn append(&self, bytes: &[u8], time_term: u32) -> Result<()>;
}
/// Explicit policy for a non-replicated, non-persistent shard.
pub struct VolatileLog;
impl CommitLog for VolatileLog {
    fn append(&self, _: &[u8], _: u32) -> Result<()> {
        Ok(())
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
    tables: RwLock<BTreeMap<TableKey, Table>>,
    quarantine: parking_lot::Mutex<Vec<Protection>>,
}
impl Store {
    pub fn new(owner: u32, migration: bool, log: Arc<dyn CommitLog>) -> Result<Arc<Self>> {
        Ok(Arc::new(Self {
            database: sto::Database::new(sto::DatabaseConfig::default()).map_err(engine)?,
            owner,
            migration,
            epochs: Arc::new(EpochDomain::default()),
            term: AtomicU32::new(0),
            log,
            tables: RwLock::new(BTreeMap::new()),
            quarantine: parking_lot::Mutex::new(Vec::new()),
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
        self.tables
            .read()
            .get(&key)
            .cloned()
            .ok_or_else(|| Error::new(ErrorCode::Invalid, "table is not open on this shard"))
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
            _pin: pin,
            snapshot,
            owner: self.owner,
        })
    }
}

pub struct Protection {
    _pin: Pin,
    snapshot: Option<Arc<AdmissionSnapshot>>,
    owner: u32,
}
impl Protection {
    fn route(&self, access: &Access) -> Result<Option<RoutedGrant>> {
        if let Some(route) = access.route {
            if route.owner != self.owner {
                return Err(Error::new(
                    ErrorCode::RouteChanged,
                    "request belongs to another shard",
                ));
            }
            Ok(Some(RoutedGrant {
                grant: Grant {
                    owner: route.owner,
                    ownership_version: route.ownership_version,
                },
                snapshot_version: route.snapshot_version,
            }))
        } else if self.snapshot.is_some() {
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

pub(crate) struct LogCallbacks<'a>(pub &'a dyn CommitLog);
impl sto::ProtocolCallbacks for LogCallbacks<'_> {
    fn stage(&mut self, _: u32, _: u16, _: sto::Mutation, _: &[u8], _: &[u8]) -> sto::Result<()> {
        Err(sto::Error {
            kind: sto::ErrorKind::InvalidUse,
            message: "remote work on participant log callback".into(),
        })
    }
    fn timestamp(&mut self) -> sto::Result<u32> {
        Err(sto::Error {
            kind: sto::ErrorKind::InvalidUse,
            message: "remote timestamp on participant".into(),
        })
    }
    fn validate(&mut self) -> sto::Result<()> {
        Err(sto::Error {
            kind: sto::ErrorKind::InvalidUse,
            message: "remote validation on participant".into(),
        })
    }
    fn install(&mut self, _: u32) -> sto::Result<()> {
        Err(sto::Error {
            kind: sto::ErrorKind::InvalidUse,
            message: "remote install on participant".into(),
        })
    }
    fn abort(&mut self) -> sto::Result<()> {
        Ok(())
    }
    fn log(&mut self, bytes: &[u8], time_term: u32) -> sto::Result<()> {
        self.0.append(bytes, time_term).map_err(|e| sto::Error {
            kind: sto::ErrorKind::UnknownOutcome,
            message: e.to_string(),
        })
    }
}
