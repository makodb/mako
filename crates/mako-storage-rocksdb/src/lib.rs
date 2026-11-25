//! Direct RocksDB participant for the shared distributed STO protocol.
//! Borrowed STO transactions remain in one owner thread through prepare and
//! decision. Message passing needs neither raw lifetime extension nor unsafe code.
#![forbid(unsafe_code)]
use mako_storage_api as api;
use std::{
    any::Any,
    collections::BTreeMap,
    ops::Bound,
    path::Path,
    sync::{
        atomic::{AtomicU32, AtomicU64, Ordering},
        mpsc, Arc,
    },
    thread,
};
use sto_rocksdb as sto;
mod lane;
mod persistent;
use lane::{Envelope, Op, Response};

pub(crate) fn error(e: sto::Error) -> api::Error {
    api::Error {
        kind: match e {
            sto::Error::Conflict | sto::Error::AlreadyExists => api::ErrorKind::Conflict,
            sto::Error::TooLarge | sto::Error::VersionExhausted => api::ErrorKind::Capacity,
            sto::Error::InvalidCommand(_)
            | sto::Error::InvalidState(_)
            | sto::Error::UnknownTable(_) => api::ErrorKind::InvalidUse,
            _ => api::ErrorKind::Fatal,
        },
        message: e.to_string(),
    }
}
fn unavailable() -> api::Error {
    api::Error {
        kind: api::ErrorKind::Fatal,
        message: "RocksDB owner lane stopped".into(),
    }
}
struct Shared {
    db: Arc<sto::Database>,
    clock: AtomicU32,
    next_table: AtomicU64,
}
impl Shared {
    fn timestamp(&self, dependency: u32) -> api::Result<u32> {
        let floor = dependency.max(self.db.distributed_clock().map_err(error)?);
        self.clock
            .fetch_update(Ordering::AcqRel, Ordering::Acquire, |old| {
                old.max(floor)
                    .checked_add(1)
                    .filter(|next| *next < u32::MAX / 10)
            })
            .map(|old| old.max(floor) + 1)
            .map_err(|_| api::Error {
                kind: api::ErrorKind::Capacity,
                message: "logical clock exhausted".into(),
            })
    }
}
pub fn database(path: impl AsRef<Path>, owner: u64, durable: bool) -> api::Result<api::Database> {
    let db = sto::Database::open_distributed(path, owner, durable).map_err(error)?;
    let next_table = db
        .list_tables()
        .map_err(error)?
        .iter()
        .map(|t| t.id)
        .max()
        .unwrap_or(0)
        .checked_add(1)
        .ok_or_else(|| api::Error::invalid("table IDs exhausted"))?;
    let clock = db.distributed_clock().map_err(error)?;
    Ok(api::Database::new(Rocks(Arc::new(Shared {
        db,
        clock: AtomicU32::new(clock),
        next_table: AtomicU64::new(next_table),
    }))))
}
/// Open the common durable image without constructing RocksDB transaction
/// workers. A Masstree adapter can use this image as its batched backing store.
pub fn backing(
    path: impl AsRef<Path>,
    owner: u64,
    durable: bool,
) -> api::Result<Arc<dyn api::PersistentApplication>> {
    Ok(Arc::new(persistent::Application(
        sto::Database::open_distributed(path, owner, durable).map_err(error)?,
    )))
}
/// The native handle also owns atomic protocol metadata/checkpoint operations.
pub fn native_database(database: &api::Database) -> api::Result<Arc<sto::Database>> {
    database
        .as_any()
        .downcast_ref::<Arc<sto::Database>>()
        .cloned()
        .ok_or_else(|| api::Error::invalid("foreign engine"))
}
struct Rocks(Arc<Shared>);
impl api::Backend for Rocks {
    fn persistent(&self) -> Option<Arc<dyn api::PersistentApplication>> {
        Some(Arc::new(persistent::Application(self.0.db.clone())))
    }
    fn kind(&self) -> api::EngineKind {
        api::EngineKind::RocksDb
    }
    fn as_any(&self) -> &dyn Any {
        &self.0.db
    }
    fn worker(&self) -> api::Result<Box<dyn api::WorkerBackend>> {
        let (sender, receiver) = mpsc::channel();
        let (ready, startup) = mpsc::sync_channel(1);
        let shared = self.0.clone();
        let join = thread::Builder::new()
            .name("rocks-sto-owner".into())
            .spawn(move || match shared.db.attach_worker() {
                Ok(mut worker) => {
                    let _ = ready.send(Ok(()));
                    lane::run(&mut worker, &shared, &receiver);
                }
                Err(e) => {
                    let _ = ready.send(Err(error(e)));
                }
            })
            .map_err(|e| api::Error::invalid(e.to_string()))?;
        startup.recv().map_err(|_| unavailable())??;
        Ok(Box::new(Worker {
            shared: self.0.clone(),
            sender,
            join: Some(join),
            busy: false,
        }))
    }
}
struct Table {
    shared: Arc<Shared>,
    id: u64,
    logical: Option<u16>,
}
impl api::TableBackend for Table {
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn len(&self) -> u64 {
        self.shared.db.count(self.id).unwrap_or(0)
    }
}
struct Worker {
    shared: Arc<Shared>,
    sender: mpsc::Sender<Envelope>,
    join: Option<thread::JoinHandle<()>>,
    busy: bool,
}
impl Worker {
    fn call(&mut self, op: Op) -> api::Result<Response> {
        let (reply, receiver) = mpsc::sync_channel(1);
        self.sender
            .send(Envelope { op, reply })
            .map_err(|_| unavailable())?;
        receiver.recv().map_err(|_| unavailable())?
    }
    fn table(&self, table: &api::Table) -> api::Result<(u64, Option<u16>)> {
        let table = table
            .as_any()
            .downcast_ref::<Table>()
            .ok_or_else(|| api::Error::invalid("foreign table backend"))?;
        if !Arc::ptr_eq(&table.shared, &self.shared) {
            return Err(api::Error::invalid("foreign database table"));
        }
        Ok((table.id, table.logical))
    }
    fn finish(&mut self, committed: bool) -> api::Result<api::ParticipantReport> {
        let response = self.call(Op::Finish(committed));
        self.busy = false;
        match response? {
            Response::Report(report) => Ok(report),
            _ => Err(unavailable()),
        }
    }
}
impl Drop for Worker {
    fn drop(&mut self) {
        let _ = self.call(Op::Stop);
        if let Some(join) = self.join.take() {
            let _ = join.join();
        }
    }
}
impl api::WorkerBackend for Worker {
    fn create_table(
        &mut self,
        config: api::TableConfig,
    ) -> api::Result<Arc<dyn api::TableBackend>> {
        if self.busy {
            return Err(api::Error::invalid("busy worker"));
        }
        let id = if config.id == 0 {
            self.shared.next_table.fetch_add(1, Ordering::AcqRel)
        } else {
            config.id
        };
        self.shared
            .next_table
            .fetch_max(id.saturating_add(1), Ordering::AcqRel);
        let logical = if config.id == 0 {
            None
        } else {
            Some(u16::try_from(id).map_err(|_| api::Error::invalid("table ID overflow"))?)
        };
        self.shared.db.distributed_table(id).map_err(error)?;
        Ok(Arc::new(Table {
            shared: self.shared.clone(),
            id,
            logical,
        }))
    }
    fn begin(&mut self) -> api::Result<Box<dyn api::LocalTransaction + '_>> {
        self.start(0)?;
        Ok(Box::new(Guard {
            worker: self,
            retained: false,
        }))
    }
    fn begin_participant(
        &mut self,
        term: u32,
    ) -> api::Result<Box<dyn api::ParticipantBackend + '_>> {
        self.start(term)?;
        Ok(Box::new(Guard {
            worker: self,
            retained: false,
        }))
    }
    fn resume_participant(&mut self) -> api::Result<Box<dyn api::ParticipantBackend + '_>> {
        if !self.busy {
            return Err(api::Error::invalid("no retained participant"));
        }
        Ok(Box::new(Guard {
            worker: self,
            retained: false,
        }))
    }
    fn abort(&mut self) -> api::Result<()> {
        if self.busy {
            self.finish(false)?;
        }
        Ok(())
    }
    fn replay(
        &mut self,
        table: &api::Table,
        key: &[u8],
        value: Option<&[u8]>,
        time_term: u32,
    ) -> api::Result<()> {
        if self.busy {
            return Err(api::Error::invalid("replay on active worker"));
        }
        let (table, _) = self.table(table)?;
        self.shared
            .db
            .apply_distributed(
                None,
                &[sto::distributed::Row {
                    table,
                    key: key.to_vec(),
                    value: value.map(<[u8]>::to_vec),
                    time_term,
                }],
                &Vec::new(),
            )
            .map_err(error)
    }
    fn checkpoint_page(
        &mut self,
        table: &api::Table,
        after: Option<&[u8]>,
        limit: usize,
    ) -> api::Result<api::CheckpointPage> {
        if self.busy {
            return Err(api::Error::invalid("checkpoint on active worker"));
        }
        let (table, _) = self.table(table)?;
        let (rows, end) = self
            .shared
            .db
            .distributed_page(table, after, limit)
            .map_err(error)?;
        Ok(api::CheckpointPage {
            rows: rows
                .into_iter()
                .map(|row| api::CheckpointRow {
                    key: row.key,
                    value: row.value,
                    time_term: row.time_term,
                })
                .collect(),
            end,
        })
    }
}
impl Worker {
    fn start(&mut self, term: u32) -> api::Result<()> {
        if self.busy || term >= 10 {
            return Err(api::Error::invalid("busy worker or invalid term"));
        }
        self.call(Op::Begin(term))?;
        self.busy = true;
        Ok(())
    }
}
struct Guard<'a> {
    worker: &'a mut Worker,
    retained: bool,
}
impl Drop for Guard<'_> {
    fn drop(&mut self) {
        if !self.retained {
            let _ = self.worker.finish(false);
        }
    }
}
fn bounds(lower: Bound<&[u8]>, upper: Bound<&[u8]>) -> (Vec<u8>, Option<Vec<u8>>) {
    let successor = |k: &[u8]| {
        let mut k = k.to_vec();
        k.push(0);
        k
    };
    let lower = match lower {
        Bound::Unbounded => Vec::new(),
        Bound::Included(k) => k.to_vec(),
        Bound::Excluded(k) => successor(k),
    };
    let upper = match upper {
        Bound::Unbounded => None,
        Bound::Included(k) => Some(successor(k)),
        Bound::Excluded(k) => Some(k.to_vec()),
    };
    (lower, upper)
}
impl api::ParticipantBackend for Guard<'_> {
    fn get(&mut self, table: &api::Table, key: &[u8]) -> api::Result<api::VersionedValue> {
        let (table, _) = self.worker.table(table)?;
        match self.worker.call(Op::Get(table, key.to_vec()))? {
            Response::Value(v) => Ok(v),
            _ => Err(unavailable()),
        }
    }
    fn mutate(
        &mut self,
        table: &api::Table,
        table_id: u16,
        mutation: api::Mutation,
        key: &[u8],
        value: &[u8],
        lock_now: bool,
    ) -> api::Result<bool> {
        let (table, logical) = self.worker.table(table)?;
        if logical.is_some_and(|id| id != table_id) {
            return Err(api::Error::invalid("logical table ID mismatch"));
        }
        if lock_now {
            return Err(api::Error::invalid(
                "RocksDB distributed locks are acquired at prepare",
            ));
        }
        match self.worker.call(Op::Mutate(
            table,
            table_id,
            mutation,
            key.to_vec(),
            value.to_vec(),
        ))? {
            Response::Bool(v) => Ok(v),
            _ => Err(unavailable()),
        }
    }
    fn scan(
        &mut self,
        table: &api::Table,
        lower: Bound<&[u8]>,
        upper: Bound<&[u8]>,
        reverse: bool,
        limit: usize,
    ) -> api::Result<api::VersionedScan> {
        let (table, _) = self.worker.table(table)?;
        let (lower, upper) = bounds(lower, upper);
        match self
            .worker
            .call(Op::Scan(table, lower, upper, reverse, limit))?
        {
            Response::Scan(v) => Ok(v),
            _ => Err(unavailable()),
        }
    }
    fn lock(&mut self) -> api::Result<()> {
        self.worker.call(Op::Lock)?;
        Ok(())
    }
    fn timestamp(&mut self) -> api::Result<u32> {
        match self.worker.call(Op::Timestamp)? {
            Response::Time(v) => Ok(v),
            _ => Err(unavailable()),
        }
    }
    fn validate(&mut self) -> api::Result<()> {
        self.worker.call(Op::Validate)?;
        Ok(())
    }
    fn prepared_rows(&mut self, time_term: u32) -> api::Result<Vec<api::RedoRow>> {
        match self.worker.call(Op::Rows(time_term))? {
            Response::Rows(v) => Ok(v),
            _ => Err(unavailable()),
        }
    }
    fn install(&mut self, timestamp: u32) -> api::Result<()> {
        self.worker.call(Op::Install(timestamp))?;
        Ok(())
    }
    fn finish(mut self: Box<Self>, committed: bool) -> api::Result<api::ParticipantReport> {
        self.retained = true;
        self.worker.finish(committed)
    }
    fn retain(mut self: Box<Self>) {
        self.retained = true;
    }
}
impl api::LocalTransaction for Guard<'_> {
    fn get(&mut self, table: &api::Table, key: &[u8]) -> api::Result<Option<Vec<u8>>> {
        Ok(api::ParticipantBackend::get(self, table, key)?.value)
    }
    fn put(&mut self, table: &api::Table, key: &[u8], value: &[u8]) -> api::Result<()> {
        self.local_mutate(table, api::Mutation::Put, key, value)?;
        Ok(())
    }
    fn insert(&mut self, table: &api::Table, key: &[u8], value: &[u8]) -> api::Result<bool> {
        self.local_mutate(table, api::Mutation::Insert, key, value)
    }
    fn remove(&mut self, table: &api::Table, key: &[u8]) -> api::Result<bool> {
        self.local_mutate(table, api::Mutation::Remove, key, b"")
    }
    fn scan(
        &mut self,
        table: &api::Table,
        lower: Bound<&[u8]>,
        upper: Bound<&[u8]>,
        reverse: bool,
        limit: usize,
    ) -> api::Result<Vec<api::Row>> {
        Ok(
            api::ParticipantBackend::scan(self, table, lower, upper, reverse, limit)?
                .rows
                .into_iter()
                .map(|r| (r.key, r.value))
                .collect(),
        )
    }
    fn commit(mut self: Box<Self>) -> api::Result<()> {
        api::ParticipantBackend::lock(&mut *self)?;
        let time = api::ParticipantBackend::timestamp(&mut *self)?;
        api::ParticipantBackend::validate(&mut *self)?;
        api::ParticipantBackend::install(&mut *self, time)?;
        api::ParticipantBackend::finish(self, true)?;
        Ok(())
    }
    fn abort(self: Box<Self>) -> api::Result<()> {
        api::ParticipantBackend::finish(self, false)?;
        Ok(())
    }
}
impl Guard<'_> {
    fn local_mutate(
        &mut self,
        table: &api::Table,
        mutation: api::Mutation,
        key: &[u8],
        value: &[u8],
    ) -> api::Result<bool> {
        let (id, logical) = self.worker.table(table)?;
        let logical = logical
            .unwrap_or(u16::try_from(id).map_err(|_| api::Error::invalid("table ID overflow"))?);
        api::ParticipantBackend::mutate(self, table, logical, mutation, key, value, false)
    }
}
#[cfg(test)]
mod tests {
    #[test]
    fn shared_contract() {
        let directory = tempfile::tempdir().unwrap();
        let db = super::database(directory.path(), 1, true).unwrap();
        mako_storage_api::conformance::run(&db);
    }
}
