//! Owned bindings for the existing verified migration storage verbs. A worker
//! retains the registry, and the registry retains every registered table until
//! all workers stop. No C++ registry or borrowed application index is involved.
use super::*;
use std::{
    collections::HashSet,
    ffi::c_void,
    sync::{LazyLock, Mutex},
};

pub type MigrationResult<T> = std::result::Result<T, u32>;
static OWNERS: LazyLock<Mutex<HashSet<u32>>> = LazyLock::new(|| Mutex::new(HashSet::new()));

struct Registered {
    catalog: u64,
    warehouse: Option<u32>,
    total: Option<u32>,
    log_id: u16,
    _table: Table,
}
pub struct Registry {
    database: Database,
    owner: u32,
    tables: Mutex<Vec<Registered>>,
}
impl Registry {
    pub fn new(database: Database, owner: u32) -> MigrationResult<Arc<Self>> {
        let mut owners = OWNERS.lock().expect("migration owners");
        if !owners.insert(owner) {
            return Err(shard_host_core::SHARD_INVALID);
        }
        Ok(Arc::new(Self {
            database,
            owner,
            tables: Mutex::new(Vec::new()),
        }))
    }
    pub fn register(
        &self,
        catalog: u64,
        warehouse: Option<(u32, u32)>,
        table: &Table,
        log_id: u16,
    ) -> MigrationResult<()> {
        if !Arc::ptr_eq(&self.database.inner, &table.database.inner) {
            return Err(shard_host_core::SHARD_INVALID);
        }
        let mut entries = self.tables.lock().expect("migration registrations");
        let number = warehouse.map(|(number, _)| number);
        if let Some(entry) = entries
            .iter()
            .find(|entry| entry.catalog == catalog && entry.warehouse == number)
        {
            return if Arc::ptr_eq(&entry._table.inner, &table.inner)
                && entry.log_id == log_id
                && entry.total == warehouse.map(|(_, total)| total)
            {
                Ok(())
            } else {
                Err(shard_host_core::SHARD_INVALID)
            };
        }
        // Allocate retention before publishing the borrowed storage handle.
        entries
            .try_reserve(1)
            .map_err(|_| shard_host_core::SHARD_EXHAUSTED)?;
        // SAFETY: this registry owns the table until all its worker Arcs drop.
        let status = unsafe {
            match warehouse {
                None => mako_sto_shard_table_register(
                    catalog,
                    self.owner,
                    Arc::as_ptr(&table.inner),
                    u32::from(log_id),
                ),
                Some((number, total)) => mako_sto_shard_warehouse_register(
                    catalog,
                    self.owner,
                    number,
                    total,
                    Arc::as_ptr(&table.inner),
                    u32::from(log_id),
                ),
            }
        };
        checked_shard(status)?;
        entries.push(Registered {
            catalog,
            warehouse: number,
            total: warehouse.map(|(_, total)| total),
            log_id,
            _table: table.clone(),
        });
        Ok(())
    }
    pub fn worker(self: &Arc<Self>) -> MigrationResult<MigrationWorker> {
        let mut worker = self
            .database
            .worker()
            .map_err(|_| shard_host_core::SHARD_IO)?;
        let context = Arc::as_ptr(self).cast_mut().cast::<c_void>();
        // SAFETY: the thread-affine handle and Arc context outlive registration;
        // Drop unregisters before either is destroyed.
        checked_shard(unsafe {
            mako_sto_shard_thread_register(context, self.owner, &mut *worker.raw)
        })?;
        Ok(MigrationWorker {
            worker,
            registry: self.clone(),
        })
    }
}
impl Drop for Registry {
    fn drop(&mut self) {
        // No MigrationWorker can exist when the final registry Arc drops.
        for entry in self
            .tables
            .get_mut()
            .expect("migration registrations")
            .drain(..)
        {
            match entry.warehouse {
                None => {
                    mako_sto_shard_table_unregister(entry.catalog, self.owner);
                }
                Some(warehouse) => {
                    mako_sto_shard_warehouse_unregister(entry.catalog, self.owner, warehouse);
                }
            }
        }
        OWNERS.lock().expect("migration owners").remove(&self.owner);
    }
}
fn checked_shard(status: u32) -> MigrationResult<()> {
    if status == shard_host_core::SHARD_OK {
        Ok(())
    } else {
        Err(status)
    }
}
fn bytes(value: &[u8]) -> MakoStoShardBytes {
    MakoStoShardBytes {
        data: value.as_ptr(),
        len: value.len(),
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Row {
    pub coordinate: Vec<u8>,
    pub key: Vec<u8>,
    pub value: Vec<u8>,
}
pub struct MigrationWorker {
    worker: Worker,
    registry: Arc<Registry>,
}
impl MigrationWorker {
    fn context(&self) -> *mut c_void {
        Arc::as_ptr(&self.registry).cast_mut().cast()
    }
    pub fn create_table(&mut self, config: TableConfig) -> Result<Table> {
        self.worker.create_table(config)
    }
    pub fn scan(
        &mut self,
        table: u64,
        lower: &[u8],
        upper: Option<&[u8]>,
        after: Option<(&[u8], &[u8])>,
    ) -> MigrationResult<Option<Row>> {
        let mut row = None;
        let (coordinate, key) = after.unwrap_or((&[], &[]));
        // SAFETY: the registry owns every possible selected table; all spans and
        // the unique sink are live for this synchronous no-unwind call.
        let status = unsafe {
            mako_sto_shard_scan_next(
                self.context(),
                table,
                bytes(lower),
                u32::from(upper.is_some()),
                bytes(upper.unwrap_or(&[])),
                u32::from(after.is_some()),
                bytes(coordinate),
                bytes(key),
                Some(collect),
                (&mut row as *mut Option<Row>).cast(),
            )
        };
        if status == shard_host_core::SHARD_NOT_FOUND {
            return Ok(None);
        }
        checked_shard(status)?;
        Ok(row)
    }
    pub fn put(
        &mut self,
        table: u64,
        coordinate: &[u8],
        key: &[u8],
        value: &[u8],
    ) -> MigrationResult<()> {
        // SAFETY: unique same-thread worker; immutable spans stay live throughout.
        checked_shard(unsafe {
            mako_sto_shard_put(
                self.context(),
                table,
                bytes(coordinate),
                bytes(key),
                bytes(value),
            )
        })
    }
    pub fn remove(&mut self, table: u64, coordinate: &[u8], key: &[u8]) -> MigrationResult<()> {
        // SAFETY: as for put.
        checked_shard(unsafe {
            mako_sto_shard_remove(self.context(), table, bytes(coordinate), bytes(key))
        })
    }
}
impl Drop for MigrationWorker {
    fn drop(&mut self) {
        // SAFETY: only this guard registered the current thread with this context.
        unsafe {
            mako_sto_shard_thread_unregister(self.context());
        }
    }
}
unsafe extern "C" fn collect(
    context: *mut c_void,
    coordinate: MakoStoShardBytes,
    key: MakoStoShardBytes,
    value: MakoStoShardBytes,
) {
    let copy = |span: MakoStoShardBytes| {
        if span.len == 0 {
            Vec::new()
        } else {
            // SAFETY: the storage verb supplies valid borrowed row spans.
            unsafe { slice::from_raw_parts(span.data, span.len) }.to_vec()
        }
    };
    // SAFETY: scan supplies this unique Option for the callback's duration.
    unsafe {
        *context.cast::<Option<Row>>() = Some(Row {
            coordinate: copy(coordinate),
            key: copy(key),
            value: copy(value),
        });
    }
}
