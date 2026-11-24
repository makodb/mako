//! Owned Rust application API over the existing STO implementation.
//!
//! Database and table handles can be shared. A worker and its active transaction
//! stay on the OS thread that created them. Transactions retain every accessed
//! table until cleanup, so dropping an application table handle cannot invalidate
//! an outstanding STO observation. No C++ transaction adapter is involved.

use super::*;
use std::{marker::PhantomData, ops::Bound, rc::Rc};

pub use super::{StoTpccDbConfig as DatabaseConfig, StoTpccTableConfig as TableConfig};

pub mod migration;
mod participant;
pub use participant::{
    Coordinator, Mutation, Participant, ParticipantReport, ProtocolCallbacks, VersionedRow,
    VersionedScan, VersionedValue,
};

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

fn error(code: i32) -> Error {
    let kind = match code {
        3 => ErrorKind::Conflict,
        6 => ErrorKind::Capacity,
        7 => ErrorKind::UnknownOutcome,
        8 => ErrorKind::CommittedFailure,
        _ => ErrorKind::Fatal,
    };
    let message =
        LAST_ERROR.with(|buffer| String::from_utf8_lossy(buffer.borrow().as_bytes()).into_owned());
    Error { kind, message }
}

fn checked(code: i32) -> Result<()> {
    if code == Status::Ok.code() {
        Ok(())
    } else {
        Err(error(code))
    }
}

fn invalid(message: &str) -> Error {
    Error {
        kind: ErrorKind::InvalidUse,
        message: message.into(),
    }
}

#[derive(Clone)]
pub struct Database {
    inner: Arc<StoTpccDb>,
}

impl Database {
    pub fn new(config: DatabaseConfig) -> Result<Self> {
        let mut raw = ptr::null_mut();
        // SAFETY: config and output are distinct live stack allocations.
        checked(unsafe { sto_tpcc_db_create(&config, &mut raw) })?;
        // SAFETY: successful creation transfers one Box allocation to us.
        Ok(Self {
            inner: Arc::from(unsafe { Box::from_raw(raw) }),
        })
    }

    pub fn worker(&self) -> Result<Worker> {
        let mut raw = ptr::null_mut();
        // SAFETY: creation only reads the database. Arc keeps it alive, and
        // the private !Send worker owns the returned same-thread allocation.
        checked(unsafe { sto_tpcc_thread_create(Arc::as_ptr(&self.inner).cast_mut(), &mut raw) })?;
        Ok(Worker {
            raw: unsafe { Box::from_raw(raw) },
            database: self.clone(),
            pinned: Vec::new(),
            thread_affinity: PhantomData,
        })
    }
}

#[derive(Clone)]
pub struct Table {
    inner: Arc<StoTpccTable>,
    database: Database,
}

impl Table {
    pub fn belongs_to(&self, database: &Database) -> bool {
        Arc::ptr_eq(&self.database.inner, &database.inner)
    }

    pub fn len(&self) -> u64 {
        self.inner.state.logical_rows.load(Ordering::Acquire)
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

/// A thread-affine application worker. The compiler prevents moving it between
/// threads, including before its first transaction.
///
/// ```compile_fail
/// fn send<T: Send>() {}
/// send::<sto_tpcc_ffi::native::Worker>();
/// ```
pub struct Worker {
    // Drop the active transaction/native scope before its pinned tables and DB.
    raw: Box<StoTpccThread>,
    pinned: Vec<Table>,
    database: Database,
    thread_affinity: PhantomData<Rc<()>>,
}

impl Worker {
    pub fn belongs_to(&self, database: &Database) -> bool {
        Arc::ptr_eq(&self.database.inner, &database.inner)
    }

    pub fn create_table(&mut self, config: TableConfig) -> Result<Table> {
        if self.raw.active.is_some() || self.raw.protocol.is_uncertain() {
            return Err(invalid("cannot create a table during a transaction"));
        }
        let mut raw = ptr::null_mut();
        // SAFETY: the worker exclusively belongs to this thread and database;
        // each argument is a disjoint live allocation for the synchronous call.
        checked(unsafe {
            sto_tpcc_table_create_on_thread(
                Arc::as_ptr(&self.database.inner).cast_mut(),
                &*self.raw,
                &config,
                STO_TPCC_RESOLVED_CACHE_FULL,
                &mut raw,
            )
        })?;
        Ok(Table {
            inner: Arc::from(unsafe { Box::from_raw(raw) }),
            database: self.database.clone(),
        })
    }

    pub fn begin(&mut self) -> Result<Transaction<'_>> {
        // SAFETY: &mut self excludes other operations on this affine handle.
        checked(unsafe { sto_tpcc_txn_begin(&mut *self.raw) })?;
        self.pinned.clear();
        Ok(Transaction {
            worker: self,
            finished: false,
        })
    }

    /// Explicit cleanup is also available after an application forgets a
    /// transaction guard. An uncertain distributed outcome is never called an
    /// abort; the underlying protocol returns its corresponding error.
    pub fn abort(&mut self) -> Result<()> {
        let result = if self.raw.sto_worker.has_participant() {
            checked(boundary("native participant abandon", || {
                protocol::abandon(&mut self.raw)
            }))
        } else {
            checked(unsafe { sto_tpcc_txn_abort(&mut *self.raw) })
        };
        if self.raw.active.is_none() {
            self.pinned.clear();
        }
        result
    }

    fn pin(&mut self, table: &Table) -> Result<()> {
        if !Arc::ptr_eq(&self.database.inner, &table.database.inner) {
            return Err(invalid("table belongs to a different database"));
        }
        if !self
            .pinned
            .iter()
            .any(|t| Arc::ptr_eq(&t.inner, &table.inner))
        {
            self.pinned.push(table.clone());
        }
        Ok(())
    }
}

pub struct Transaction<'worker> {
    worker: &'worker mut Worker,
    finished: bool,
}

impl Transaction<'_> {
    pub fn get(&mut self, table: &Table, key: &[u8]) -> Result<Option<Vec<u8>>> {
        self.worker.pin(table)?;
        let mut value = vec![0; 256];
        loop {
            let mut actual = 0;
            // SAFETY: worker is exclusive, table is retained, and all spans
            // are disjoint Rust borrows valid for this synchronous call.
            let code = unsafe {
                sto_tpcc_get(
                    &mut *self.worker.raw,
                    &*table.inner,
                    key.as_ptr(),
                    key.len(),
                    value.as_mut_ptr(),
                    value.len(),
                    &mut actual,
                )
            };
            match code {
                0 => {
                    value.truncate(actual);
                    return Ok(Some(value));
                }
                1 => return Ok(None),
                4 => value.resize(actual, 0),
                _ => return Err(error(code)),
            }
        }
    }

    pub fn put(&mut self, table: &Table, key: &[u8], value: &[u8]) -> Result<()> {
        self.worker.pin(table)?;
        // SAFETY: retained table and exclusive affine worker; immutable input
        // slices remain borrowed throughout the call and are copied by STO.
        checked(unsafe {
            sto_tpcc_put(
                &mut *self.worker.raw,
                &*table.inner,
                key.as_ptr(),
                key.len(),
                value.as_ptr(),
                value.len(),
            )
        })
    }

    /// Returns false when this transaction already observes the key.
    pub fn insert(&mut self, table: &Table, key: &[u8], value: &[u8]) -> Result<bool> {
        self.worker.pin(table)?;
        let code = unsafe {
            sto_tpcc_insert(
                &mut *self.worker.raw,
                &*table.inner,
                key.as_ptr(),
                key.len(),
                value.as_ptr(),
                value.len(),
            )
        };
        match code {
            0 => Ok(true),
            2 => Ok(false),
            _ => Err(error(code)),
        }
    }

    pub fn remove(&mut self, table: &Table, key: &[u8]) -> Result<bool> {
        self.worker.pin(table)?;
        let code = unsafe {
            sto_tpcc_remove(
                &mut *self.worker.raw,
                &*table.inner,
                key.as_ptr(),
                key.len(),
            )
        };
        match code {
            0 => Ok(true),
            1 => Ok(false),
            _ => Err(error(code)),
        }
    }

    pub fn scan(
        &mut self,
        table: &Table,
        lower: Bound<&[u8]>,
        upper: Bound<&[u8]>,
        reverse: bool,
        limit: usize,
    ) -> Result<Vec<Row>> {
        self.worker.pin(table)?;
        let (lo_kind, lo) = bound(lower);
        let (hi_kind, hi) = bound(upper);
        let mut rows = Vec::<Row>::new();
        let mut visited = 0;
        // SAFETY: the callback owns a unique borrow of rows only while this
        // synchronous scan runs. STO supplies valid immutable row spans.
        checked(unsafe {
            sto_tpcc_scan(
                &mut *self.worker.raw,
                &*table.inner,
                i32::from(reverse),
                lo_kind,
                lo.as_ptr(),
                lo.len(),
                hi_kind,
                hi.as_ptr(),
                hi.len(),
                limit,
                Some(collect_row),
                (&mut rows as *mut Vec<Row>).cast(),
                &mut visited,
            )
        })?;
        Ok(rows)
    }

    pub fn commit(mut self) -> Result<()> {
        let result = checked(unsafe { sto_tpcc_txn_commit(&mut *self.worker.raw) });
        // A failed terminal may still need cleanup. Drop calls the existing
        // idempotent abort path and never changes the reported commit outcome.
        if self.worker.raw.active.is_none() {
            self.worker.pinned.clear();
            self.finished = true;
        }
        result
    }

    pub fn abort(mut self) -> Result<()> {
        let result = self.worker.abort();
        self.finished = self.worker.raw.active.is_none();
        result
    }
}

impl Drop for Transaction<'_> {
    fn drop(&mut self) {
        if !self.finished {
            let _ = self.worker.abort();
        }
    }
}

fn bound(value: Bound<&[u8]>) -> (i32, &[u8]) {
    match value {
        Bound::Unbounded => (0, &[]),
        Bound::Included(key) => (1, key),
        Bound::Excluded(key) => (2, key),
    }
}

unsafe extern "C" fn collect_row(
    context: *mut std::ffi::c_void,
    key: *const u8,
    key_len: usize,
    value: *const u8,
    value_len: usize,
) -> i32 {
    // SAFETY: scan supplies disjoint immutable row spans and our unique output
    // Vec, all live for this callback. Zero-length spans need no valid pointer.
    let rows = unsafe { &mut *context.cast::<Vec<Row>>() };
    let key = if key_len == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(key, key_len) }
    };
    let value = if value_len == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(value, value_len) }
    };
    rows.push((key.to_vec(), value.to_vec()));
    0
}
