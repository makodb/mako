//! Masstree implementation of the distributed storage contract. STO keeps its
//! existing affine ownership and locks; this adapter introduces no raw lifetimes.
#![forbid(unsafe_code)]

use mako_storage_api as api;
use std::{any::Any, ops::Bound, sync::Arc};
use sto_tpcc_ffi::native;

pub fn error(e: native::Error) -> api::Error {
    api::Error {
        kind: match e.kind {
            native::ErrorKind::Conflict => api::ErrorKind::Conflict,
            native::ErrorKind::Capacity => api::ErrorKind::Capacity,
            native::ErrorKind::UnknownOutcome => api::ErrorKind::UnknownOutcome,
            native::ErrorKind::CommittedFailure => api::ErrorKind::CommittedFailure,
            native::ErrorKind::InvalidUse => api::ErrorKind::InvalidUse,
            native::ErrorKind::Fatal => api::ErrorKind::Fatal,
        },
        message: e.message,
    }
}
pub fn database() -> api::Result<api::Database> {
    Ok(api::Database::new(Masstree(
        native::Database::new(native::DatabaseConfig::default()).map_err(error)?,
    )))
}
struct Masstree(native::Database);
impl api::Backend for Masstree {
    fn kind(&self) -> api::EngineKind {
        api::EngineKind::Masstree
    }
    fn as_any(&self) -> &dyn Any {
        &self.0
    }
    fn worker(&self) -> api::Result<Box<dyn api::WorkerBackend>> {
        Ok(Box::new(Worker(self.0.worker().map_err(error)?)))
    }
}
struct Table(native::Table);
impl api::TableBackend for Table {
    fn as_any(&self) -> &dyn Any {
        &self.0
    }
    fn len(&self) -> u64 {
        self.0.len()
    }
}
/// Temporary bridge for the existing Masstree migration host. The common
/// transaction interface never downcasts and this bridge rejects other engines.
pub fn native_database(database: &api::Database) -> api::Result<native::Database> {
    database
        .as_any()
        .downcast_ref::<native::Database>()
        .cloned()
        .ok_or_else(|| api::Error::invalid("Masstree migration requires a Masstree engine"))
}
pub fn native_table(table: &api::Table) -> api::Result<&native::Table> {
    table
        .as_any()
        .downcast_ref()
        .ok_or_else(|| api::Error::invalid("foreign engine table"))
}
struct Worker(native::Worker);
impl api::WorkerBackend for Worker {
    fn checkpoint_page(
        &mut self,
        table: &api::Table,
        after: Option<&[u8]>,
        limit: usize,
    ) -> api::Result<api::CheckpointPage> {
        let page = self
            .0
            .checkpoint_page(native_table(table)?, after, limit)
            .map_err(error)?;
        Ok(api::CheckpointPage {
            rows: page
                .rows
                .into_iter()
                .map(|row| api::CheckpointRow {
                    key: row.key,
                    value: row.value,
                    time_term: row.time_term,
                })
                .collect(),
            end: page.end,
        })
    }
    fn create_table(&mut self) -> api::Result<Arc<dyn api::TableBackend>> {
        Ok(Arc::new(Table(
            self.0
                .create_table(native::TableConfig::default())
                .map_err(error)?,
        )))
    }
    fn begin(&mut self) -> api::Result<Box<dyn api::LocalTransaction + '_>> {
        Ok(Box::new(Local(self.0.begin().map_err(error)?)))
    }
    fn begin_participant(
        &mut self,
        term: u32,
    ) -> api::Result<Box<dyn api::ParticipantBackend + '_>> {
        Ok(Box::new(Participant(
            self.0.begin_participant(term).map_err(error)?,
        )))
    }
    fn resume_participant(&mut self) -> api::Result<Box<dyn api::ParticipantBackend + '_>> {
        Ok(Box::new(Participant(
            self.0.resume_participant().map_err(error)?,
        )))
    }
    fn replay(
        &mut self,
        table: &api::Table,
        key: &[u8],
        value: Option<&[u8]>,
        time_term: u32,
    ) -> api::Result<()> {
        self.0
            .replay(native_table(table)?, key, value, time_term)
            .map_err(error)
    }
    fn abort(&mut self) -> api::Result<()> {
        self.0.abort().map_err(error)
    }
}
struct Local<'a>(native::Transaction<'a>);
impl api::LocalTransaction for Local<'_> {
    fn get(&mut self, t: &api::Table, k: &[u8]) -> api::Result<Option<Vec<u8>>> {
        self.0.get(native_table(t)?, k).map_err(error)
    }
    fn put(&mut self, t: &api::Table, k: &[u8], v: &[u8]) -> api::Result<()> {
        self.0.put(native_table(t)?, k, v).map_err(error)
    }
    fn insert(&mut self, t: &api::Table, k: &[u8], v: &[u8]) -> api::Result<bool> {
        self.0.insert(native_table(t)?, k, v).map_err(error)
    }
    fn remove(&mut self, t: &api::Table, k: &[u8]) -> api::Result<bool> {
        self.0.remove(native_table(t)?, k).map_err(error)
    }
    fn scan(
        &mut self,
        t: &api::Table,
        lo: Bound<&[u8]>,
        hi: Bound<&[u8]>,
        rev: bool,
        limit: usize,
    ) -> api::Result<Vec<api::Row>> {
        self.0
            .scan(native_table(t)?, lo, hi, rev, limit)
            .map_err(error)
    }
    fn commit(self: Box<Self>) -> api::Result<()> {
        self.0.commit().map_err(error)
    }
    fn abort(self: Box<Self>) -> api::Result<()> {
        self.0.abort().map_err(error)
    }
}
struct Participant<'a>(native::Participant<'a>);
impl api::ParticipantBackend for Participant<'_> {
    fn get(&mut self, t: &api::Table, k: &[u8]) -> api::Result<api::VersionedValue> {
        let v = self.0.get(native_table(t)?, k).map_err(error)?;
        Ok(api::VersionedValue {
            value: v.value,
            time_term: v.time_term,
        })
    }
    fn mutate(
        &mut self,
        t: &api::Table,
        id: u16,
        m: api::Mutation,
        k: &[u8],
        v: &[u8],
        lock: bool,
    ) -> api::Result<bool> {
        let m = match m {
            api::Mutation::Put => native::Mutation::Put,
            api::Mutation::Insert => native::Mutation::Insert,
            api::Mutation::Remove => native::Mutation::Remove,
        };
        self.0
            .mutate(native_table(t)?, id, m, k, v, lock)
            .map_err(error)
    }
    fn scan(
        &mut self,
        t: &api::Table,
        lo: Bound<&[u8]>,
        hi: Bound<&[u8]>,
        rev: bool,
        limit: usize,
    ) -> api::Result<api::VersionedScan> {
        let scan = self
            .0
            .scan(native_table(t)?, lo, hi, rev, limit)
            .map_err(error)?;
        Ok(api::VersionedScan {
            rows: scan
                .rows
                .into_iter()
                .map(|r| api::VersionedRow {
                    key: r.key,
                    value: r.value,
                    time_term: r.time_term,
                })
                .collect(),
            read_dependency: scan.read_dependency,
        })
    }
    fn lock(&mut self) -> api::Result<()> {
        self.0.lock().map_err(error)
    }
    fn timestamp(&mut self) -> api::Result<u32> {
        self.0.timestamp().map_err(error)
    }
    fn validate(&mut self) -> api::Result<()> {
        self.0.validate().map_err(error)
    }
    fn prepared_rows(&mut self, time_term: u32) -> api::Result<Vec<api::RedoRow>> {
        let bytes = self.0.prepared_redo(time_term / 10).map_err(error)?;
        if bytes.is_empty() {
            return Ok(Vec::new());
        }
        // The runtime enforces its selected log's aggregate intent bound. This
        // boundary only decodes the local legacy frame into owned logical rows.
        mako_protocol::redo::decode_legacy_bounded(&bytes, time_term, bytes.len())
            .map_err(|e| api::Error::invalid(e.to_string()))
    }
    fn install(&mut self, timestamp: u32) -> api::Result<()> {
        self.0.install(timestamp).map_err(error)
    }
    fn finish(self: Box<Self>, committed: bool) -> api::Result<api::ParticipantReport> {
        let r = self.0.finish(committed).map_err(error)?;
        Ok(api::ParticipantReport {
            disposition: r.disposition,
            phase: r.phase,
            time_term: r.time_term,
            read_dependency: r.read_dependency,
            empty: r.empty,
        })
    }
    fn retain(self: Box<Self>) {
        self.0.retain()
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn masstree_contract() {
        mako_storage_api::conformance::run(&super::database().unwrap());
    }
}
