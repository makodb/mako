//! The database facade: one `sto-core` runtime, the shared Masstree runtime,
//! the table registry, and the process-wide cache-order namespace claim.

use std::collections::HashMap;
use std::fmt;
use std::marker::PhantomData;
use std::sync::atomic::{AtomicPtr, AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, RwLock};

use mako_engine_api::{CacheOrderMode, Error, Result, MAX_TABLE_NAME_BYTES, MAX_WORKERS};
use sto_core::{CapacityError, RegistrationError, RuntimeConfig, RuntimeId};
use sto_masstree::{RegistryBudget, TableConfig, TableCreateError};

use crate::txn::Transaction;
use crate::worker::{self, Attachment};

/// Items and locks one transaction may hold in `sto-core`. The public item
/// budget is 512 weighted credits; each credited operation touches at most
/// one record item plus directory bookkeeping, so this bound is generous.
const STO_MAX_ITEMS_PER_TRANSACTION: usize = 4 * mako_engine_api::TRANSACTION_ITEM_BUDGET;
const STO_MAX_LOCKS_PER_TRANSACTION: usize = 4 * mako_engine_api::TRANSACTION_ITEM_BUDGET;
/// Per-table registry bounds. Records and consumed ids are process-lifetime
/// in this revision, exactly as MassTrans rows are in the C++ engine.
const TABLE_MAX_RETAINED_RECORDS: u64 = 1 << 40;
const TABLE_MAX_RETAINED_KEY_BYTES: u64 = 1 << 44;
const TABLE_MAX_CONSUMED_RECORD_IDS: u64 = 1 << 32;

const MODE_UNCLAIMED: u32 = 0;

/// One registered table.
pub(crate) struct TableEntry {
    pub(crate) id: u64,
    pub(crate) runtime: RuntimeId,
    pub(crate) table: sto_masstree::Table,
}

/// A local in-memory Mako database over the native Rust STO.
///
/// The facade can be shared between fixed, long-lived workers. Tables are
/// process-lifetime in this revision.
pub struct LocalDb {
    sto: Arc<sto_core::Runtime>,
    directory: masstree::Runtime,
    tables: RwLock<Vec<Arc<TableEntry>>>,
    by_name: Mutex<HashMap<Vec<u8>, usize>>,
    trusted: AtomicPtr<TableEntry>,
    mode: AtomicU32,
    /// Legacy record-terminal FIFO ticket of unclaimed and single-producer
    /// facades, mirroring `record_validation_next/serving`.
    pub(crate) ticket_next: AtomicU64,
    pub(crate) ticket_serving: AtomicU64,
}

impl fmt::Debug for LocalDb {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("LocalDb")
            .field("runtime", &self.sto.id())
            .field("mode", &self.mode.load(Ordering::Relaxed))
            .finish_non_exhaustive()
    }
}

/// The one process-wide cache-order namespace holder.
static NAMESPACE_CLAIM: Mutex<Option<RuntimeId>> = Mutex::new(None);

fn recover<T>(lock: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    lock.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn map_registration(error: RegistrationError) -> Error {
    match error {
        RegistrationError::Capacity(
            CapacityError::ObjectIdExhausted | CapacityError::RuntimeIdExhausted,
        ) => Error::Busy,
        RegistrationError::Capacity(_) => Error::OutOfMemory,
        RegistrationError::InvalidUse(_) => Error::InvalidArgument,
        RegistrationError::Poisoned(_) => Error::WorkerPoisoned,
    }
}

impl LocalDb {
    /// Open a new database and attach the calling worker.
    pub fn open() -> Result<Self> {
        let config = RuntimeConfig::new()
            .with_max_workers(MAX_WORKERS)
            .with_max_items_per_transaction(STO_MAX_ITEMS_PER_TRANSACTION)
            .with_max_locks_per_transaction(STO_MAX_LOCKS_PER_TRANSACTION);
        let sto = sto_core::Runtime::new(config).map_err(|error| match error {
            sto_core::RuntimeError::Capacity(_) => Error::OutOfMemory,
            sto_core::RuntimeError::Poisoned(_) => Error::WorkerPoisoned,
            _ => Error::Internal,
        })?;
        let directory_config = masstree::RuntimeConfig::new()
            .with_max_threads(u32::try_from(MAX_WORKERS).expect("worker limit fits u32"));
        let directory = masstree::Runtime::new(directory_config).map_err(|_| Error::Internal)?;
        let db = Self {
            sto,
            directory,
            tables: RwLock::new(Vec::new()),
            by_name: Mutex::new(HashMap::new()),
            trusted: AtomicPtr::new(std::ptr::null_mut()),
            mode: AtomicU32::new(MODE_UNCLAIMED),
            ticket_next: AtomicU64::new(0),
            ticket_serving: AtomicU64::new(0),
        };
        db.attach()?;
        Ok(db)
    }

    pub(crate) fn runtime_id(&self) -> RuntimeId {
        self.sto.id()
    }

    pub(crate) fn attach(&self) -> Result<Attachment> {
        worker::attach(&self.sto, &self.directory)
    }

    /// Current immutable cache-order mode; zero while unclaimed.
    pub(crate) fn mode(&self) -> u32 {
        self.mode.load(Ordering::Acquire)
    }

    pub(crate) fn mode_is(&self, mode: CacheOrderMode) -> bool {
        self.mode() == mode as u32
    }

    /// `packed_cache_order_allowed`: claimed Concurrent.
    pub(crate) fn packed_allowed(&self) -> bool {
        self.mode_is(CacheOrderMode::Concurrent)
    }

    /// `rust_sequence_cache_order_allowed`: unclaimed, SingleProducer, or PerWorker.
    pub(crate) fn rust_sequence_allowed(&self) -> bool {
        !self.packed_allowed()
    }

    /// `single_producer_cache_order_allowed`: unclaimed or SingleProducer.
    pub(crate) fn single_producer_allowed(&self) -> bool {
        let mode = self.mode();
        mode == MODE_UNCLAIMED || mode == CacheOrderMode::SingleProducer as u32
    }

    pub(crate) fn per_worker_allowed(&self) -> bool {
        self.mode_is(CacheOrderMode::PerWorker)
    }

    /// Find or create a table with a stable numeric identifier.
    ///
    /// Reopening `name` with another identifier, or another name with the
    /// same identifier, is an error.
    pub fn open_table(&self, name: impl AsRef<[u8]>, table_id: u64) -> Result<Table<'_>> {
        let name = name.as_ref();
        if name.len() > MAX_TABLE_NAME_BYTES {
            return Err(Error::ValueTooLarge);
        }
        let attachment = self.attach()?;
        let mut by_name = recover(&self.by_name);
        if let Some(&index) = by_name.get(name) {
            let tables = self
                .tables
                .read()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            let entry = &tables[index];
            if entry.id != table_id {
                return Err(Error::WrongDatabaseOrTable);
            }
            return Ok(Table::from_entry(entry));
        }
        {
            let tables = self
                .tables
                .read()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            if tables.iter().any(|entry| entry.id == table_id) {
                return Err(Error::WrongDatabaseOrTable);
            }
        }
        let config = TableConfig::new()
            .with_max_retained_records(TABLE_MAX_RETAINED_RECORDS)
            .with_max_retained_key_bytes(TABLE_MAX_RETAINED_KEY_BYTES)
            .with_max_consumed_record_ids(TABLE_MAX_CONSUMED_RECORD_IDS)
            // The cache contract never surfaces a conflict for two inserters
            // of distinct keys; wait for structural admission like MassTrans.
            .with_blocking_structural_admission(true);
        let table = sto_masstree::Table::new_direct_with_budget(
            &self.sto,
            &self.directory,
            attachment.worker,
            config,
            RegistryBudget::new(u64::MAX),
        )
        .map_err(|error| match error {
            TableCreateError::Directory(_) => Error::Internal,
            TableCreateError::Registration(error) => map_registration(error),
        })?;
        let entry = Arc::new(TableEntry {
            id: table_id,
            runtime: self.sto.id(),
            table,
        });
        let mut tables = self
            .tables
            .write()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        by_name.try_reserve(1).map_err(|_| Error::OutOfMemory)?;
        tables.try_reserve(1).map_err(|_| Error::OutOfMemory)?;
        let index = tables.len();
        tables.push(entry);
        by_name.insert(name.to_vec(), index);
        Ok(Table::from_entry(&tables[index]))
    }

    /// Begin one transaction on the current OS thread.
    #[inline]
    pub fn transaction(&self) -> Result<Transaction<'_>> {
        let attachment = self.attach()?;
        Transaction::begin(self, attachment, None)
    }

    /// Begin a transaction bound to `bound_table` for the cache's fast path.
    #[doc(hidden)]
    pub fn trusted_transaction<'db>(
        &'db self,
        bound_table: &Table<'db>,
    ) -> Result<Transaction<'db>> {
        if bound_table.entry.runtime != self.sto.id() {
            return Err(Error::WrongDatabaseOrTable);
        }
        let attachment = self.attach()?;
        Transaction::begin(self, attachment, Some(bound_table.entry))
    }

    /// Bind the one table used by the trusted cache transaction path.
    #[doc(hidden)]
    pub fn bind_trusted_table(&self, name: impl AsRef<[u8]>, table_id: u64) -> Result<()> {
        let table = self.open_table(name, table_id)?;
        let raw = std::ptr::from_ref(table.entry).cast_mut();
        match self.trusted.compare_exchange(
            std::ptr::null_mut(),
            raw,
            Ordering::Release,
            Ordering::Acquire,
        ) {
            Ok(_) => Ok(()),
            Err(existing) if existing == raw => Ok(()),
            Err(_) => Err(Error::WrongDatabaseOrTable),
        }
    }

    /// Begin on the table installed by [`Self::bind_trusted_table`].
    #[doc(hidden)]
    pub fn trusted_bound_transaction<'db>(&'db self) -> Result<(Table<'db>, Transaction<'db>)> {
        let raw = self.trusted.load(Ordering::Acquire);
        if raw.is_null() {
            return Err(Error::FeatureUnavailable);
        }
        // SAFETY: the pointer names an `Arc<TableEntry>` payload held by
        // `self.tables` for the database's whole lifetime.
        let entry: &'db TableEntry = unsafe { &*raw };
        let table = Table {
            entry,
            _db: PhantomData,
        };
        let transaction = self.trusted_transaction(&table)?;
        Ok((table, transaction))
    }

    /// Claim the process-wide cache-order namespace in one immutable mode.
    ///
    /// # Safety
    ///
    /// The caller must exclusively own this fresh facade; no transaction or
    /// terminal may overlap the claim.
    #[doc(hidden)]
    pub unsafe fn claim_cache_order_namespace(&self, mode: CacheOrderMode) -> Result<()> {
        let mut claim = recover(&NAMESPACE_CLAIM);
        if claim.is_some() {
            return Err(Error::Busy);
        }
        self.mode.store(mode as u32, Ordering::Release);
        if !mako_clock::global().reseed_sequence(0) {
            self.mode.store(MODE_UNCLAIMED, Ordering::Release);
            return Err(Error::Busy);
        }
        *claim = Some(self.sto.id());
        Ok(())
    }

    /// Reseed the claimed namespace's dense sequence after recovery.
    ///
    /// # Safety
    ///
    /// No foreground transaction or ordering terminal may exist.
    #[doc(hidden)]
    pub unsafe fn reseed_cache_order_namespace(&self, recovered_sequence: u64) -> Result<()> {
        if *recover(&NAMESPACE_CLAIM) != Some(self.sto.id()) {
            return Err(Error::InvalidArgument);
        }
        if mako_clock::global().reseed_sequence(recovered_sequence) {
            Ok(())
        } else {
            Err(Error::InvalidArgument)
        }
    }

    /// Dense cache-sequence snapshot for diagnostics; zero when unclaimed.
    #[doc(hidden)]
    pub fn cache_order_snapshot(&self) -> u64 {
        if *recover(&NAMESPACE_CLAIM) == Some(self.sto.id()) {
            mako_clock::global().sequence_snapshot()
        } else {
            0
        }
    }

    /// Cut the cache-order modification order before a read-only outcome scan.
    #[doc(hidden)]
    #[inline]
    pub fn order_record_validation_prefix(&self) {
        let mode = self.mode();
        if mode != CacheOrderMode::Concurrent as u32 && mode != CacheOrderMode::PerWorker as u32 {
            std::process::abort();
        }
        let _ = mako_clock::global().order_validation_prefix();
    }

    /// Explicitly close and consume this facade.
    pub fn close(self) -> Result<()> {
        drop(self);
        Ok(())
    }
}

impl Drop for LocalDb {
    fn drop(&mut self) {
        let mut claim = recover(&NAMESPACE_CLAIM);
        if *claim == Some(self.sto.id()) {
            *claim = None;
        }
    }
}

impl mako_engine_api::EngineDb for LocalDb {
    type Engine = crate::MakoStoEngine;
    type Table<'db>
        = Table<'db>
    where
        Self: 'db;
    type Transaction<'db>
        = Transaction<'db>
    where
        Self: 'db;

    fn open() -> Result<Self> {
        Self::open()
    }
    unsafe fn claim_cache_order_namespace(&self, mode: CacheOrderMode) -> Result<()> {
        // SAFETY: forwarded unchanged.
        unsafe { Self::claim_cache_order_namespace(self, mode) }
    }
    unsafe fn reseed_cache_order_namespace(&self, recovered_sequence: u64) -> Result<()> {
        // SAFETY: forwarded unchanged.
        unsafe { Self::reseed_cache_order_namespace(self, recovered_sequence) }
    }
    fn open_table(&self, name: &[u8], table_id: u64) -> Result<Self::Table<'_>> {
        Self::open_table(self, name, table_id)
    }
    fn bind_trusted_table(&self, name: &[u8], table_id: u64) -> Result<()> {
        Self::bind_trusted_table(self, name, table_id)
    }
    fn transaction(&self) -> Result<Self::Transaction<'_>> {
        Self::transaction(self)
    }
    fn trusted_bound_transaction(&self) -> Result<(Self::Table<'_>, Self::Transaction<'_>)> {
        Self::trusted_bound_transaction(self)
    }
    fn order_record_validation_prefix(&self) {
        Self::order_record_validation_prefix(self)
    }
}

/// A table borrowed from a [`LocalDb`].
#[derive(Clone, Copy)]
pub struct Table<'db> {
    pub(crate) entry: &'db TableEntry,
    _db: PhantomData<&'db LocalDb>,
}

impl<'db> Table<'db> {
    fn from_entry(entry: &Arc<TableEntry>) -> Self {
        // The `Arc` payload lives in `LocalDb::tables` until the database is
        // dropped, which outlives every `'db` borrow.
        let entry: *const TableEntry = Arc::as_ptr(entry);
        Self {
            // SAFETY: see above; entries are never removed or replaced.
            entry: unsafe { &*entry },
            _db: PhantomData,
        }
    }

    /// Stable table identifier supplied at open.
    pub fn id(&self) -> u64 {
        self.entry.id
    }
}

impl fmt::Debug for Table<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Table").field("id", &self.entry.id).finish()
    }
}
