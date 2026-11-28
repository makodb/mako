//! Storage boundary for the distributed STO protocol.
//!
//! Implementations own concurrency control and private transaction state. The
//! caller locks every participant before validation, durably saves prepared
//! intent, then installs only the authoritative decision. Log commitment is
//! not permission to expose a Prepare's rows. A worker and every borrowed
//! participant stay on their creating thread.
#![forbid(unsafe_code)]

#[cfg(any(test, feature = "test-support"))]
pub mod conformance;
#[cfg(any(test, feature = "test-support"))]
pub mod model;

use std::{
    any::Any,
    marker::PhantomData,
    ops::{Bound, Deref, DerefMut},
    rc::Rc,
    sync::Arc,
};

pub use sto_core::protocol::redo::Row as RedoRow;
/// Shared migration exclusion above either physical transaction backend.
pub use sto_core::protocol::range_lock;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ErrorKind {
    Conflict,
    Capacity,
    UnknownOutcome,
    CommittedFailure,
    InvalidUse,
    Fatal,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Error {
    pub kind: ErrorKind,
    pub message: String,
}
impl Error {
    pub fn invalid(message: impl Into<String>) -> Self {
        Self {
            kind: ErrorKind::InvalidUse,
            message: message.into(),
        }
    }
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{:?}: {}", self.kind, self.message)
    }
}
impl std::error::Error for Error {}
pub type Result<T> = std::result::Result<T, Error>;
pub type Row = (Vec<u8>, Vec<u8>);

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum EngineKind {
    Model,
    Masstree,
    RocksDb,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Mutation {
    Put,
    Insert,
    Remove,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct VersionedValue {
    pub value: Option<Vec<u8>>,
    pub time_term: u32,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct VersionedRow {
    pub key: Vec<u8>,
    pub value: Vec<u8>,
    pub time_term: u32,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct VersionedScan {
    pub rows: Vec<VersionedRow>,
    pub read_dependency: u32,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CheckpointRow {
    pub key: Vec<u8>,
    pub value: Option<Vec<u8>>,
    /// Zero is bootstrap data; nonzero values are distributed application time.
    pub time_term: u32,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CheckpointPage {
    pub rows: Vec<CheckpointRow>,
    pub end: bool,
}
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ParticipantReport {
    pub disposition: u32,
    pub phase: u32,
    pub time_term: u32,
    pub read_dependency: u32,
    pub empty: bool,
}
#[derive(Clone, Copy, Default)]
pub struct TableConfig {
    /// Stable schema ID for persistent engines. Zero allocates a test/local ID.
    pub id: u64,
}

/// Shared engine factory. `worker` constructs affine state on the calling thread.
pub trait Backend: Any + Send + Sync {
    fn kind(&self) -> EngineKind;
    fn as_any(&self) -> &dyn Any;
    fn worker(&self) -> Result<Box<dyn WorkerBackend>>;
    fn persistent(&self) -> Option<Arc<dyn PersistentApplication>> {
        None
    }
}
/// Common durable application image. It may back a direct transaction adapter
/// or a volatile accelerator. Application batches recover rows and protocol
/// metadata at the same exact prefix.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct LogPosition {
    pub index: u64,
    pub term: u64,
}
pub type Metadata = Vec<(Vec<u8>, Vec<u8>)>;
/// One already decided application effect. Several consecutive effects may be
/// materialized atomically. Prepare changes protocol metadata only.
#[derive(Clone, Debug)]
pub struct ApplicationBatch {
    pub position: LogPosition,
    pub fingerprint: [u8; 32],
    pub rows: Vec<RedoRow>,
    pub metadata: Metadata,
}
impl ApplicationBatch {
    pub fn bytes(&self) -> usize {
        self.rows
            .iter()
            .fold(64usize, |n, row| {
                n.saturating_add(row.key.len())
                    .saturating_add(row.value.as_ref().map_or(0, Vec::len))
                    .saturating_add(16)
            })
            .saturating_add(self.metadata.iter().fold(0usize, |n, (key, value)| {
                n.saturating_add(key.len())
                    .saturating_add(value.len())
                    .saturating_add(16)
            }))
    }
}

pub trait PersistentApplication: Send + Sync {
    /// Stable identity of pre-log data. Replicas must agree before voting or
    /// exchanging log entries. New empty databases return zero.
    fn initial_state_id(&self) -> Result<[u8; 32]> { Ok([0;32]) }
    /// Called only after the runtime establishes durable log ownership.
    fn complete_bootstrap(&self) -> Result<()> { Ok(()) }
    fn durable(&self) -> bool;
    fn bind_identity(&self, identity: &[u8]) -> Result<()>;
    fn position(&self) -> Result<LogPosition>;
    fn metadata(&self) -> Result<Metadata>;
    /// Stable physical IDs in the currently published durable generation.
    fn table_ids(&self) -> Result<Vec<u64>>;
    fn ensure_tables(&self, ids: &[u16]) -> Result<()>;
    /// Caller holds publication stable across all pages. Tombstones are included.
    fn page(&self, table: u16, after: Option<&[u8]>, limit: usize) -> Result<CheckpointPage>;
    /// Initial image only, before any authoritative log position is applied.
    fn bootstrap(&self, rows: &[RedoRow]) -> Result<()>;
    /// Atomic contiguous-prefix materialization. After a successful batch, every
    /// row, metadata item, replay identity and the final position recover together.
    /// An ambiguous failure must fence until recovery; it permits no log trimming.
    fn apply_batches(&self, batches: &[ApplicationBatch]) -> Result<()>;

    fn apply(
        &self,
        position: LogPosition,
        fingerprint: [u8; 32],
        rows: &[RedoRow],
        metadata: &Metadata,
    ) -> Result<()>;
    fn create_checkpoint(&self, path: &std::path::Path, position: LogPosition) -> Result<()>;
    /// Validate a private generation before publication. A previously selected
    /// generation may already contain a later prefix and must never regress.
    fn checkpoint_metadata(
        &self,
        path: &std::path::Path,
        position: LogPosition,
        identity: &[u8],
    ) -> Result<(LogPosition, Metadata)>;
    fn publish_checkpoint(&self, path: &std::path::Path, position: LogPosition) -> Result<()>;
}
pub trait TableBackend: Any + Send + Sync {
    fn as_any(&self) -> &dyn Any;
    fn len(&self) -> u64;
    fn is_empty(&self) -> bool {
        self.len() == 0
    }
}
/// A backend must reject foreign tables, even if their numeric IDs coincide.
/// Replay is for decided effects under the caller's publication gate; it must
/// preserve logical timestamps and tombstones and never revalidate a decision.
pub trait WorkerBackend {
    /// The caller holds the publication barrier for the entire multi-page cut.
    /// Export committed payloads, including tombstones, even behind prepare locks.
    fn checkpoint_page(
        &mut self,
        table: &Table,
        after: Option<&[u8]>,
        limit: usize,
    ) -> Result<CheckpointPage>;
    fn create_table(&mut self, config: TableConfig) -> Result<Arc<dyn TableBackend>>;
    fn begin(&mut self) -> Result<Box<dyn LocalTransaction + '_>>;
    fn begin_participant(&mut self, term: u32) -> Result<Box<dyn ParticipantBackend + '_>>;
    fn resume_participant(&mut self) -> Result<Box<dyn ParticipantBackend + '_>>;
    fn replay(
        &mut self,
        table: &Table,
        key: &[u8],
        value: Option<&[u8]>,
        time_term: u32,
    ) -> Result<()>;
    fn abort(&mut self) -> Result<()>;
}
/// Loading/component transactions. Distributed applications use Participant.
/// Persistent loading must be covered by a checkpoint or the authoritative log.
pub trait LocalTransaction {
    fn get(&mut self, table: &Table, key: &[u8]) -> Result<Option<Vec<u8>>>;
    fn put(&mut self, table: &Table, key: &[u8], value: &[u8]) -> Result<()>;
    fn insert(&mut self, table: &Table, key: &[u8], value: &[u8]) -> Result<bool>;
    fn remove(&mut self, table: &Table, key: &[u8]) -> Result<bool>;
    fn scan(
        &mut self,
        table: &Table,
        lower: Bound<&[u8]>,
        upper: Bound<&[u8]>,
        reverse: bool,
        limit: usize,
    ) -> Result<Vec<Row>>;
    fn commit(self: Box<Self>) -> Result<()>;
    fn abort(self: Box<Self>) -> Result<()>;
}
/// Owned borrowed guard; retain releases the borrow, not locks or private writes.
/// Drop before installation aborts. An uncertain/installed outcome must fence
/// instead of rolling back. After validate, writes and returned intent are frozen.
pub trait ParticipantBackend {
    fn get(&mut self, table: &Table, key: &[u8]) -> Result<VersionedValue>;
    #[allow(clippy::too_many_arguments)]
    fn mutate(
        &mut self,
        table: &Table,
        table_id: u16,
        mutation: Mutation,
        key: &[u8],
        value: &[u8],
        lock_now: bool,
    ) -> Result<bool>;
    /// Dependencies include traversed tombstones and omitted rows, not just output.
    fn scan(
        &mut self,
        table: &Table,
        lower: Bound<&[u8]>,
        upper: Bound<&[u8]>,
        reverse: bool,
        limit: usize,
    ) -> Result<VersionedScan>;
    fn lock(&mut self) -> Result<()>;
    fn timestamp(&mut self) -> Result<u32>;
    fn validate(&mut self) -> Result<()>;
    fn prepared_rows(&mut self, time_term: u32) -> Result<Vec<RedoRow>>;
    fn install(&mut self, timestamp: u32) -> Result<()>;
    fn finish(self: Box<Self>, committed: bool) -> Result<ParticipantReport>;
    fn retain(self: Box<Self>);
}

#[derive(Clone)]
pub struct Database(Arc<dyn Backend>);
impl Database {
    pub fn new(backend: impl Backend) -> Self {
        Self(Arc::new(backend))
    }
    pub fn kind(&self) -> EngineKind {
        self.0.kind()
    }
    pub fn as_any(&self) -> &dyn Any {
        self.0.as_any()
    }
    pub fn persistent(&self) -> Option<Arc<dyn PersistentApplication>> {
        self.0.persistent()
    }

    pub fn worker(&self) -> Result<Worker> {
        Ok(Worker {
            inner: self.0.worker()?,
            database: self.clone(),
            affine: PhantomData,
        })
    }
}
#[derive(Clone)]
pub struct Table {
    inner: Arc<dyn TableBackend>,
    database: Database,
}
impl Table {
    pub fn as_any(&self) -> &dyn Any {
        self.inner.as_any()
    }
    pub fn len(&self) -> u64 {
        self.inner.len()
    }
    pub fn is_empty(&self) -> bool {
        self.inner.is_empty()
    }
    pub fn belongs_to(&self, database: &Database) -> bool {
        Arc::ptr_eq(&self.database.0, &database.0)
    }
}
/// ```compile_fail
/// fn send<T: Send>() {}
/// send::<mako_storage_api::Worker>();
/// ```
pub struct Worker {
    inner: Box<dyn WorkerBackend>,
    database: Database,
    affine: PhantomData<Rc<()>>,
}
impl Worker {
    pub fn checkpoint_page(
        &mut self,
        table: &Table,
        after: Option<&[u8]>,
        limit: usize,
    ) -> Result<CheckpointPage> {
        if !table.belongs_to(&self.database) {
            return Err(Error::invalid("foreign checkpoint table"));
        }
        self.inner.checkpoint_page(table, after, limit)
    }
    pub fn belongs_to(&self, database: &Database) -> bool {
        Arc::ptr_eq(&self.database.0, &database.0)
    }
    pub fn create_table(&mut self, config: TableConfig) -> Result<Table> {
        Ok(Table {
            inner: self.inner.create_table(config)?,
            database: self.database.clone(),
        })
    }
    pub fn begin(&mut self) -> Result<Box<dyn LocalTransaction + '_>> {
        self.inner.begin()
    }
    pub fn begin_participant(&mut self, term: u32) -> Result<Participant<'_>> {
        self.inner.begin_participant(term).map(Participant)
    }
    pub fn resume_participant(&mut self) -> Result<Participant<'_>> {
        self.inner.resume_participant().map(Participant)
    }
    pub fn replay(
        &mut self,
        table: &Table,
        key: &[u8],
        value: Option<&[u8]>,
        time_term: u32,
    ) -> Result<()> {
        if !table.belongs_to(&self.database) {
            return Err(Error::invalid("table belongs to another engine"));
        }
        self.inner.replay(table, key, value, time_term)
    }
    pub fn abort(&mut self) -> Result<()> {
        self.inner.abort()
    }
}
pub struct Participant<'worker>(Box<dyn ParticipantBackend + 'worker>);
impl<'worker> Deref for Participant<'worker> {
    type Target = dyn ParticipantBackend + 'worker;
    fn deref(&self) -> &Self::Target {
        &*self.0
    }
}
impl DerefMut for Participant<'_> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut *self.0
    }
}
impl Participant<'_> {
    pub fn retain(self) {
        self.0.retain()
    }
    pub fn finish(self, committed: bool) -> Result<ParticipantReport> {
        self.0.finish(committed)
    }
}
