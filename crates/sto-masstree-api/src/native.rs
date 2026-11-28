//! Owned Rust API. Storage callbacks and bytes cross only the native row ABI.
use crate::owner::StoWorkerOwner;
use std::{fmt, marker::PhantomData, ops::Bound, rc::Rc, sync::Arc};
use sto_core::clock::distributed::{
    global_distributed, DistributedTerm, DistributedTimeTerm, DistributedTimestamp,
};
use sto_core::{
    AccessError, Active, CommitFailure, CommitOutcome, DefiniteOutcome, ParticipantError,
    ParticipantPhase,
};
use sto_masstree::{InsertOutcome, ReadVisibility, ScanBound, ScanDirection, ScanRequest, Value};

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
impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:?}: {}", self.kind, self.message)
    }
}
impl std::error::Error for Error {}
pub type Result<T> = std::result::Result<T, Error>;
pub type Row = (Vec<u8>, Vec<u8>);
fn error(kind: ErrorKind, message: impl fmt::Display) -> Error {
    Error {
        kind,
        message: message.to_string(),
    }
}
fn invalid(message: impl fmt::Display) -> Error {
    error(ErrorKind::InvalidUse, message)
}
fn access(e: AccessError) -> Error {
    error(
        match e {
            AccessError::Conflict(_) => ErrorKind::Conflict,
            AccessError::Capacity(c) if !c.is_terminal() => ErrorKind::Capacity,
            AccessError::InvalidUse(_) => ErrorKind::InvalidUse,
            _ => ErrorKind::Fatal,
        },
        e,
    )
}
fn failure(e: CommitFailure) -> Error {
    error(
        match e {
            CommitFailure::Indeterminate(_) => ErrorKind::UnknownOutcome,
            CommitFailure::Poisoned {
                outcome: DefiniteOutcome::Committed(_),
                ..
            } => ErrorKind::CommittedFailure,
            _ => ErrorKind::Fatal,
        },
        e,
    )
}
fn aborted(reason: sto_core::AbortReason) -> Error {
    use sto_core::AbortReason;
    let kind = match reason {
        AbortReason::Capacity(c) if !c.is_terminal() => ErrorKind::Capacity,
        AbortReason::Doomed | AbortReason::Conflict(_) => ErrorKind::Conflict,
        AbortReason::InvalidUse(_) | AbortReason::Unsupported(_) => ErrorKind::InvalidUse,
        _ => ErrorKind::Fatal,
    };
    error(kind, reason)
}
fn phase_error(e: ParticipantError) -> Error {
    match e {
        ParticipantError::Failure(e) => failure(e),
        ParticipantError::InvalidUse(e) => invalid(e),
        ParticipantError::Aborted(reason) => aborted(reason),
    }
}
fn committed(result: std::result::Result<CommitOutcome, CommitFailure>) -> Result<()> {
    match result {
        Ok(CommitOutcome::Committed(_)) => Ok(()),
        Ok(CommitOutcome::Aborted(reason)) => Err(aborted(reason)),
        Err(e) => Err(failure(e)),
    }
}
#[derive(Clone, Copy, Debug, Default)]
pub struct DatabaseConfig {
    pub max_threads: u32,
    pub max_key_length: u32,
    pub max_items_per_txn: usize,
    pub max_locks_per_txn: usize,
    pub max_native_row_bytes: u64,
}
#[derive(Clone, Copy, Debug, Default)]
pub struct TableConfig {
    pub max_retained_records: u64,
    pub max_retained_key_bytes: u64,
    pub scan_chunk_records: usize,
    pub scan_max_key_arena_bytes: usize,
    pub max_scan_chunks: usize,
    pub max_scan_physical_records: usize,
}
struct DatabaseInner {
    core: Arc<sto_core::Runtime>,
    native: masstree::Runtime,
    budget: masstree::rows::Budget,
}
#[derive(Clone)]
pub struct Database {
    inner: Arc<DatabaseInner>,
}
impl Database {
    pub fn new(config: DatabaseConfig) -> Result<Self> {
        let mut core = sto_core::RuntimeConfig::default();
        let mut native = masstree::RuntimeConfig::new();
        if config.max_threads != 0 {
            core = core.with_max_workers(config.max_threads as usize);
            native = native.with_max_threads(config.max_threads);
        }
        if config.max_key_length != 0 {
            native = native.with_max_key_length(config.max_key_length);
        }
        if config.max_items_per_txn != 0 {
            core = core.with_max_items_per_transaction(config.max_items_per_txn);
        }
        if config.max_locks_per_txn != 0 {
            core = core.with_max_locks_per_transaction(config.max_locks_per_txn);
        }
        Ok(Self {
            inner: Arc::new(DatabaseInner {
                core: sto_core::Runtime::new(core).map_err(invalid)?,
                native: masstree::Runtime::new(native).map_err(invalid)?,
                budget: masstree::rows::Budget::new(if config.max_native_row_bytes == 0 {
                    8 * 1024 * 1024 * 1024
                } else {
                    config.max_native_row_bytes
                })
                .map_err(invalid)?,
            }),
        })
    }
    pub fn native_row_bytes(&self) -> u64 {
        self.inner.budget.used()
    }
    pub fn worker(&self) -> Result<Worker> {
        Ok(Worker {
            owner: StoWorkerOwner::new(self.inner.core.attach().map_err(invalid)?),
            native: self.inner.native.attach().map_err(invalid)?,
            database: self.clone(),
            protocol: Protocol::new(DistributedTerm::new(0).unwrap()),
            affinity: PhantomData,
        })
    }
}
#[derive(Clone)]
pub struct Table {
    inner: sto_masstree::Table,
    database: Database,
}
impl Table {
    pub fn belongs_to(&self, db: &Database) -> bool {
        Arc::ptr_eq(&self.database.inner, &db.inner)
    }
    pub fn len(&self) -> u64 {
        self.inner.len()
    }
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
    pub fn seal_directory_structure(&self) -> Result<()> {
        self.inner.seal_directory_structure().map_err(access)
    }
    pub fn advance_history_retention_floor(&self, base: u32) -> Result<()> {
        self.inner
            .advance_history_retention_floor(DistributedTimestamp::new(base).map_err(invalid)?);
        Ok(())
    }
}
/// Workers and active transaction guards remain on their creating OS thread.
/// ```compile_fail
/// fn send<T:Send>() {} send::<sto_masstree_api::native::Worker>();
/// ```
/// ```compile_fail
/// fn sync<T:Sync>() {} sync::<sto_masstree_api::native::Worker>();
/// ```
pub struct Worker {
    // Drop the transaction owner before the native worker and database.
    owner: StoWorkerOwner,
    native: masstree::Worker,
    database: Database,
    protocol: Protocol,
    affinity: PhantomData<Rc<()>>,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CheckpointRow {
    pub key: Vec<u8>,
    pub value: Option<Vec<u8>>,
    pub time_term: u32,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CheckpointPage {
    pub rows: Vec<CheckpointRow>,
    pub end: bool,
}
impl Worker {
    pub fn belongs_to(&self, db: &Database) -> bool {
        Arc::ptr_eq(&self.database.inner, &db.inner)
    }
    fn table(&self, t: &Table) -> Result<()> {
        if t.belongs_to(&self.database) {
            Ok(())
        } else {
            Err(invalid("table belongs to another database"))
        }
    }
    fn idle(&self) -> Result<()> {
        if self.protocol.uncertain {
            return Err(error(
                ErrorKind::UnknownOutcome,
                "worker has an unresolved installed outcome",
            ));
        }
        if self.owner.is_active() {
            return Err(invalid("worker already has an active transaction"));
        }
        Ok(())
    }
    pub fn create_table(&mut self, c: TableConfig) -> Result<Table> {
        self.idle()?;
        let mut config = sto_masstree::TableConfig::new()
            .with_record_local_versions(true)
            .with_distributed_history(true);
        if c.max_retained_records != 0 {
            config = config.with_max_retained_records(c.max_retained_records);
        }
        if c.max_retained_key_bytes != 0 {
            config = config.with_max_retained_key_bytes(c.max_retained_key_bytes);
        }
        if c.scan_chunk_records != 0 {
            config = config.with_scan_chunk_records(c.scan_chunk_records);
        }
        if c.scan_max_key_arena_bytes != 0 {
            config = config.with_scan_max_key_arena_bytes(c.scan_max_key_arena_bytes);
        }
        if c.max_scan_chunks != 0 {
            config = config.with_max_scan_chunks(c.max_scan_chunks);
        }
        if c.max_scan_physical_records != 0 {
            config = config.with_max_scan_physical_records(c.max_scan_physical_records);
        }
        Ok(Table {
            inner: sto_masstree::Table::with_budget(
                &self.database.inner.core,
                &self.database.inner.native,
                &self.native,
                config,
                &self.database.inner.budget,
            )
            .map_err(|e| {
                let kind = if matches!(
                    &e,
                    sto_masstree::TableCreateError::Directory(masstree::Error::Native(
                        masstree::NativeStatus::OutOfMemory
                    ))
                ) {
                    ErrorKind::Capacity
                } else {
                    ErrorKind::InvalidUse
                };
                error(kind, e)
            })?,
            database: self.database.clone(),
        })
    }
    pub fn begin(&mut self) -> Result<Transaction<'_>> {
        self.idle()?;
        self.owner.begin_erased().map_err(invalid)?;
        Ok(Transaction {
            owner: &mut self.owner,
            native: &self.native,
            database: &self.database,
            finished: false,
        })
    }
    pub fn begin_participant(&mut self, term: u32) -> Result<Participant<'_>> {
        self.idle()?;
        let term = DistributedTerm::new(term).map_err(invalid)?;
        self.owner.begin_erased().map_err(invalid)?;
        assert!(self.owner.promote());
        self.protocol = Protocol::new(term);
        Ok(Participant {
            worker: self,
            finished: false,
        })
    }
    pub fn resume_participant(&mut self) -> Result<Participant<'_>> {
        if self.protocol.uncertain {
            return Err(error(ErrorKind::UnknownOutcome, "participant is fenced"));
        }
        if !self.owner.has_participant() {
            return Err(invalid("no retained participant"));
        }
        Ok(Participant {
            worker: self,
            finished: false,
        })
    }
    pub fn abort(&mut self) -> Result<()> {
        if self.owner.has_participant() {
            if self.owner.with_participant(|p| p.phase()) == Some(ParticipantPhase::Installed) {
                self.protocol.uncertain = true;
            }
            self.finish(false).map(|_| ())
        } else {
            self.owner.finish_active(|t| t.abort());
            if self.protocol.uncertain {
                Err(error(ErrorKind::UnknownOutcome, "worker is fenced"))
            } else {
                Ok(())
            }
        }
    }
    pub fn replay(
        &mut self,
        t: &Table,
        key: &[u8],
        value: Option<&[u8]>,
        time_term: u32,
    ) -> Result<()> {
        self.idle()?;
        self.table(t)?;
        let time = DistributedTimeTerm::decode_legacy(time_term).map_err(invalid)?;
        global_distributed().observe(time.timestamp());
        let value = value
            .map(Value::try_from_slice)
            .transpose()
            .map_err(access)?;
        let native = &self.native;
        let table = &t.inner;
        self.owner.begin_erased().map_err(invalid)?;
        let staged = self
            .owner
            .with_active_transaction(|txn| table.stage_replay(txn, native, key, value, time))
            .expect("active replay");
        if let Err(e) = staged {
            self.owner.finish_active(|t| t.abort());
            return Err(access(e));
        }
        committed(
            self.owner
                .finish_active(|t| t.commit())
                .expect("active replay"),
        )
    }
    pub fn checkpoint_page(
        &mut self,
        t: &Table,
        after: Option<&[u8]>,
        limit: usize,
    ) -> Result<CheckpointPage> {
        self.idle()?;
        self.table(t)?;
        let page = t
            .inner
            .checkpoint_page(&self.native, after, limit)
            .map_err(access)?;
        Ok(CheckpointPage {
            rows: page
                .rows
                .into_iter()
                .map(|r| CheckpointRow {
                    key: r.key,
                    value: r.value.map(|v| v.as_ref().to_vec()),
                    time_term: r.metadata.map_or(0, DistributedTimeTerm::encode_legacy),
                })
                .collect(),
            end: page.end,
        })
    }
    #[cfg(all(test, mtree_native_integration))]
    fn local<R>(
        &mut self,
        t: &Table,
        f: impl for<'a> FnOnce(
            &mut sto_core::Transaction<'a, Active>,
            &masstree::Worker,
        ) -> std::result::Result<R, AccessError>,
    ) -> Result<R> {
        self.table(t)?;
        let result = self
            .owner
            .with_active_transaction(|txn| f(txn, &self.native))
            .ok_or_else(|| invalid("no active transaction"))?;
        match result {
            Ok(v) => Ok(v),
            Err(e) => {
                self.owner.finish_active(|t| t.abort());
                Err(access(e))
            }
        }
    }
    fn participant<R>(
        &mut self,
        f: impl for<'a> FnOnce(
            &mut sto_core::Transaction<'a, Active>,
            &masstree::Worker,
        ) -> std::result::Result<R, AccessError>,
    ) -> Result<R> {
        self.owner
            .with_participant(|p| p.with_transaction(|txn| f(txn, &self.native)))
            .ok_or_else(|| invalid("no participant"))?
            .map_err(access)
    }
    fn phase(&mut self, expected: ParticipantPhase) -> Result<()> {
        if self.owner.with_participant(|p| p.phase()) == Some(expected) {
            Ok(())
        } else {
            Err(invalid("invalid participant phase"))
        }
    }
    fn choose(&mut self, base: u32) -> Result<DistributedTimeTerm> {
        let time = DistributedTimeTerm::new(
            DistributedTimestamp::new(base).map_err(invalid)?,
            self.protocol.term,
        );
        if base <= self.protocol.dependency {
            return Err(invalid("decision does not exceed read dependency"));
        }
        if self.protocol.decision.is_some_and(|old| old != time) {
            return Err(invalid("prepared decision changed"));
        }
        global_distributed().observe(time.timestamp());
        self.protocol.decision = Some(time);
        Ok(time)
    }
    fn finish(&mut self, commit: bool) -> Result<ParticipantReport> {
        let result = self
            .owner
            .finish_participant(|p| p.finish(commit))
            .ok_or_else(|| invalid("no participant"))?;
        let (disposition, err) = match result {
            Ok(CommitOutcome::Committed(_)) => (2, None),
            Ok(CommitOutcome::Aborted(reason)) => (1, commit.then(|| aborted(reason))),
            Err(e) => {
                let e = failure(e);
                (
                    match e.kind {
                        ErrorKind::UnknownOutcome => 3,
                        ErrorKind::CommittedFailure => 2,
                        _ => 1,
                    },
                    Some(e),
                )
            }
        };
        self.protocol.uncertain |= disposition == 3;
        let report = ParticipantReport {
            disposition: if self.protocol.uncertain {
                3
            } else {
                disposition
            },
            phase: 0,
            time_term: self
                .protocol
                .decision
                .map_or(0, DistributedTimeTerm::encode_legacy),
            read_dependency: self.protocol.dependency,
            empty: true,
        };
        self.protocol.tables.clear();
        self.protocol.prepared.clear();
        if self.protocol.uncertain {
            return Err(error(
                ErrorKind::UnknownOutcome,
                "installed participant outcome is unresolved",
            ));
        }
        if let Some(e) = err {
            Err(e)
        } else {
            Ok(report)
        }
    }
}
/// A transaction exclusively borrows its worker until finish or drop.
/// ```compile_fail
/// use sto_masstree_api::native::{Database,DatabaseConfig,TableConfig};
/// let db = Database::new(DatabaseConfig::default()).unwrap();
/// let mut worker = db.worker().unwrap();
/// let transaction = worker.begin().unwrap();
/// let table = worker.create_table(TableConfig::default()).unwrap();
/// transaction.commit().unwrap();
/// ```
pub struct Transaction<'a> {
    owner: &'a mut StoWorkerOwner,
    native: &'a masstree::Worker,
    database: &'a Database,
    finished: bool,
}
fn request<'a>(
    lo: Bound<&'a [u8]>,
    hi: Bound<&'a [u8]>,
    reverse: bool,
    limit: usize,
) -> ScanRequest<'a> {
    fn bound(b: Bound<&[u8]>) -> ScanBound<'_> {
        match b {
            Bound::Unbounded => ScanBound::Unbounded,
            Bound::Included(v) => ScanBound::Included(v),
            Bound::Excluded(v) => ScanBound::Excluded(v),
        }
    }
    ScanRequest::new(
        if reverse {
            ScanDirection::Reverse
        } else {
            ScanDirection::Forward
        },
        limit,
    )
    .with_lower(bound(lo))
    .with_upper(bound(hi))
}
impl Transaction<'_> {
    fn local<R>(
        &mut self,
        table: &Table,
        f: impl for<'a> FnOnce(
            &mut sto_core::Transaction<'a, Active>,
            &masstree::Worker,
        ) -> std::result::Result<R, AccessError>,
    ) -> Result<R> {
        if !table.belongs_to(self.database) {
            return Err(invalid("table belongs to another database"));
        }
        let result = self
            .owner
            .with_active_transaction(|tx| f(tx, self.native))
            .ok_or_else(|| invalid("no active transaction"))?;
        match result {
            Ok(value) => Ok(value),
            Err(e) => {
                self.owner.finish_active(|t| t.abort());
                Err(access(e))
            }
        }
    }
    /// Runs a short synchronous group of accesses in one native RCU region.
    /// The region ends before this method returns, including on errors/panics.
    /// Do not block, perform I/O, or await inside the callback. No region is
    /// retained while the transaction waits for commit or a participant vote.
    pub fn with_access<R>(&mut self, f: impl FnOnce(&mut Self) -> Result<R>) -> Result<R> {
        let scope = self.native.rcu_scope().map_err(invalid)?;
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| f(self)));
        let closed = scope.close().map_err(invalid);
        match result {
            Err(panic) => {
                self.owner.finish_active(|t| t.abort());
                std::panic::resume_unwind(panic)
            }
            Ok(result) => {
                let result = closed.and(result);
                if result.is_err() {
                    self.owner.finish_active(|t| t.abort());
                }
                result
            }
        }
    }
    pub fn get(&mut self, t: &Table, key: &[u8]) -> Result<Option<Vec<u8>>> {
        let mut value = Vec::new();
        self.get_into(t, key, &mut value)
            .map(|present| present.then_some(value))
    }
    /// Reads into a reusable buffer with ordinary transaction validation.
    pub fn get_into(&mut self, t: &Table, key: &[u8], buffer: &mut Vec<u8>) -> Result<bool> {
        self.local(t, |txn, w| t.inner.get_into(txn, w, key, buffer))
    }
    pub fn put(&mut self, t: &Table, key: &[u8], value: &[u8]) -> Result<()> {
        self.local(t, |txn, w| t.inner.set(txn, w, key, value))
            .map(|_| ())
    }
    pub fn insert(&mut self, t: &Table, key: &[u8], value: &[u8]) -> Result<bool> {
        self.local(t, |txn, w| t.inner.insert(txn, w, key, value))
            .map(|r| matches!(r, InsertOutcome::Inserted))
    }
    pub fn remove(&mut self, t: &Table, key: &[u8]) -> Result<bool> {
        self.local(t, |txn, w| t.inner.remove(txn, w, key))
            .map(|v| v.is_some())
    }
    pub fn scan(
        &mut self,
        t: &Table,
        lo: Bound<&[u8]>,
        hi: Bound<&[u8]>,
        reverse: bool,
        limit: usize,
    ) -> Result<Vec<Row>> {
        self.local(t, |txn, w| {
            t.inner.scan(txn, w, request(lo, hi, reverse, limit))
        })
        .map(|rows| {
            rows.into_iter()
                .map(|r| (r.key().to_vec(), r.value().to_vec()))
                .collect()
        })
    }
    pub fn commit(mut self) -> Result<()> {
        self.finished = true;
        committed(
            self.owner
                .finish_active(|t| t.commit())
                .ok_or_else(|| invalid("transaction already aborted"))?,
        )
    }
    pub fn abort(mut self) -> Result<()> {
        self.finished = true;
        self.owner.finish_active(|t| t.abort());
        Ok(())
    }
}
impl Drop for Transaction<'_> {
    fn drop(&mut self) {
        if !self.finished {
            self.owner.finish_active(|t| t.abort());
        }
    }
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
pub struct PreparedRow {
    pub table: u16,
    pub key: Vec<u8>,
    pub value: Option<Vec<u8>>,
    pub time_term: u32,
}
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ParticipantReport {
    pub disposition: u32,
    pub phase: u32,
    pub time_term: u32,
    pub read_dependency: u32,
    pub empty: bool,
}
struct FrozenRow {
    table: u16,
    key: Vec<u8>,
    value: Option<Value>,
}
struct Protocol {
    term: DistributedTerm,
    visibility: ReadVisibility,
    dependency: u32,
    decision: Option<DistributedTimeTerm>,
    uncertain: bool,
    tables: Vec<(u16, Table)>,
    prepared: Vec<FrozenRow>,
}
impl Protocol {
    fn new(term: DistributedTerm) -> Self {
        Self {
            term,
            visibility: ReadVisibility::latest(term),
            dependency: 0,
            decision: None,
            uncertain: false,
            tables: Vec::new(),
            prepared: Vec::new(),
        }
    }
    fn observe(&mut self, time: Option<DistributedTimeTerm>) {
        if let Some(time) = time {
            self.dependency = self.dependency.max(time.timestamp().get());
        }
    }
}
pub struct Participant<'a> {
    worker: &'a mut Worker,
    finished: bool,
}
impl Participant<'_> {
    pub fn retain(mut self) {
        self.finished = true;
    }
    pub fn report(&mut self) -> Result<ParticipantReport> {
        let (phase, empty) = self
            .worker
            .owner
            .with_participant(|p| (p.phase(), p.is_empty()))
            .ok_or_else(|| invalid("no participant"))?;
        Ok(ParticipantReport {
            disposition: if self.worker.protocol.uncertain {
                3
            } else if phase == ParticipantPhase::Installed {
                2
            } else {
                0
            },
            phase: match phase {
                ParticipantPhase::Collecting => 1,
                ParticipantPhase::Validated => 2,
                ParticipantPhase::Installed => 3,
                ParticipantPhase::Finished => 0,
            },
            time_term: self
                .worker
                .protocol
                .decision
                .map_or(0, DistributedTimeTerm::encode_legacy),
            read_dependency: self.worker.protocol.dependency,
            empty,
        })
    }
    pub fn visibility(&mut self, stable: [Option<u32>; 10], failed: Option<u32>) -> Result<()> {
        self.worker.phase(ParticipantPhase::Collecting)?;
        self.worker.protocol.visibility = ReadVisibility {
            current_term: self.worker.protocol.term,
            stable_through: stable,
            failed_shard_through: failed,
        };
        Ok(())
    }
    pub fn get(&mut self, t: &Table, key: &[u8]) -> Result<VersionedValue> {
        self.worker.table(t)?;
        let visibility = self.worker.protocol.visibility;
        let r = self
            .worker
            .participant(|txn, w| t.inner.get_versioned(txn, w, key, visibility))?;
        self.worker.protocol.observe(r.metadata);
        Ok(VersionedValue {
            value: r.value.map(|v| v.as_ref().to_vec()),
            time_term: r.metadata.map_or(0, DistributedTimeTerm::encode_legacy),
        })
    }
    pub fn mutate(
        &mut self,
        t: &Table,
        id: u16,
        mutation: Mutation,
        key: &[u8],
        value: &[u8],
        lock_now: bool,
    ) -> Result<bool> {
        self.worker.table(t)?;
        self.worker.phase(ParticipantPhase::Collecting)?;
        if id == 0 || id >= 0x8000 {
            return Err(invalid("invalid journal table ID"));
        }
        if self
            .worker
            .protocol
            .tables
            .iter()
            .any(|(old, table)| (*old == id) != (table.inner.object_id() == t.inner.object_id()))
        {
            return Err(invalid("journal table ID changed"));
        }
        if !self
            .worker
            .protocol
            .tables
            .iter()
            .any(|(old, _)| *old == id)
        {
            self.worker.protocol.tables.push((id, t.clone()));
        }
        let visibility = self.worker.protocol.visibility;
        let (changed, metadata) = self.worker.participant(|txn, w| {
            let prior = t.inner.get_versioned(txn, w, key, visibility)?;
            let present = prior.value.is_some();
            let changed = match mutation {
                Mutation::Put => {
                    t.inner.set(txn, w, key, value)?;
                    true
                }
                Mutation::Insert if !present => {
                    t.inner.set(txn, w, key, value)?;
                    true
                }
                Mutation::Remove if present => {
                    t.inner.stage_tombstone(txn, w, key)?;
                    true
                }
                _ => false,
            };
            Ok((changed, prior.metadata))
        })?;
        self.worker.protocol.observe(metadata);
        if lock_now {
            self.lock()?;
        }
        Ok(changed)
    }
    pub fn scan(
        &mut self,
        t: &Table,
        lo: Bound<&[u8]>,
        hi: Bound<&[u8]>,
        reverse: bool,
        limit: usize,
    ) -> Result<VersionedScan> {
        self.worker.table(t)?;
        let visibility = self.worker.protocol.visibility;
        let scan = self.worker.participant(|txn, w| {
            t.inner
                .scan_versioned(txn, w, request(lo, hi, reverse, limit), visibility)
        })?;
        if let Some(dependency) = scan.dependency {
            self.worker.protocol.dependency = self.worker.protocol.dependency.max(dependency.get());
        }
        Ok(VersionedScan {
            read_dependency: self.worker.protocol.dependency,
            rows: scan
                .rows
                .into_iter()
                .map(|r| VersionedRow {
                    key: r.key,
                    value: r.value.as_ref().to_vec(),
                    time_term: r.metadata.map_or(0, DistributedTimeTerm::encode_legacy),
                })
                .collect(),
        })
    }
    pub fn lock(&mut self) -> Result<()> {
        self.worker
            .owner
            .with_participant(|p| p.lock_writes())
            .ok_or_else(|| invalid("no participant"))?
            .map_err(phase_error)
    }
    pub fn timestamp(&mut self) -> Result<u32> {
        if self.worker.protocol.dependency != 0 {
            global_distributed().observe(
                DistributedTimestamp::new(self.worker.protocol.dependency).map_err(invalid)?,
            );
        }
        global_distributed()
            .reserve()
            .map(|t| t.get())
            .map_err(invalid)
    }
    pub fn validate(&mut self) -> Result<()> {
        self.worker.phase(ParticipantPhase::Collecting)?;
        let tables = self.worker.protocol.tables.clone();
        let frozen = self.worker.participant(|txn, _| {
            let mut frozen = Vec::new();
            for (id, t) in &tables {
                for snapshot in t.inner.intents(txn)? {
                    frozen.push(FrozenRow {
                        table: *id,
                        key: snapshot.key,
                        value: snapshot.value,
                    });
                }
            }
            Ok(frozen)
        })?;
        self.worker
            .owner
            .with_participant(|p| p.validate())
            .ok_or_else(|| invalid("no participant"))?
            .map_err(phase_error)?;
        self.worker.protocol.prepared = frozen;
        Ok(())
    }
    pub fn prepared_rows(&mut self, base: u32) -> Result<Vec<PreparedRow>> {
        self.worker.phase(ParticipantPhase::Validated)?;
        let time = self.worker.choose(base)?;
        Ok(self
            .worker
            .protocol
            .prepared
            .iter()
            .map(|r| PreparedRow {
                table: r.table,
                key: r.key.clone(),
                value: r.value.as_ref().map(|v| v.as_ref().to_vec()),
                time_term: time.encode_legacy(),
            })
            .collect())
    }
    pub fn install(&mut self, base: u32) -> Result<()> {
        self.worker.phase(ParticipantPhase::Validated)?;
        let time = self.worker.choose(base)?;
        self.worker.protocol.uncertain = true;
        self.worker
            .owner
            .with_participant(|p| p.install(time))
            .ok_or_else(|| invalid("no participant"))?
            .map_err(|e| error(ErrorKind::UnknownOutcome, e))?;
        self.worker.protocol.uncertain = false;
        Ok(())
    }
    pub fn finish(mut self, committed: bool) -> Result<ParticipantReport> {
        self.finished = true;
        self.worker.finish(committed)
    }
}
impl Drop for Participant<'_> {
    fn drop(&mut self) {
        if !self.finished {
            let _ = self.worker.abort();
        }
    }
}

#[cfg(all(test, mtree_native_integration))]
mod tests {
    use super::*;
    #[test]
    fn post_install_failure_marks_runtime_indeterminate() {
        struct BrokenHook;
        impl sto_core::CommitHook for BrokenHook {
            fn reserve_upper_metadata(
                &mut self,
            ) -> std::result::Result<(), sto_core::CommitHookError> {
                Ok(())
            }
            fn pre_install(&mut self) -> std::result::Result<(), sto_core::CommitHookError> {
                Ok(())
            }
            fn post_install(&mut self) {
                panic!("injected failure after native publication")
            }
        }
        let db = Database::new(DatabaseConfig::default()).unwrap();
        let mut worker = db.worker().unwrap();
        let table = worker.create_table(TableConfig::default()).unwrap();
        worker.owner.begin_erased().unwrap();
        worker
            .local(&table, |tx, w| table.inner.put(tx, w, b"row", b"installed"))
            .unwrap();
        let result = worker
            .owner
            .finish_active(|tx| tx.commit_with_hook(&mut BrokenHook))
            .unwrap();
        assert!(matches!(result, Err(CommitFailure::Indeterminate(_))));
        assert_eq!(
            db.inner.core.health(),
            sto_core::RuntimeHealth::Indeterminate
        );
        assert!(!worker.owner.is_active());
        assert!(worker.begin().is_err());
    }
}
