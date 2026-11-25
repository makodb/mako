//! A documented subset of the Rust `rocksdb` 0.25 API over distributed Mako
//! transactions. Enable `rocksdb-engine` for opening a production database and
//! `masstree-engine` for the optional Masstree transaction adapter.
//!
//! All reads are OCC dependencies. Commit runs Mako's global prepare barrier
//! and durable 2PC. This crate does not open ordinary RocksDB directories.
#![forbid(unsafe_code)]

pub mod backend;
mod batch;
mod error;
mod options;
#[cfg(feature = "rocksdb-engine")]
mod runtime;

pub use batch::WriteBatch;
pub use error::{Error, ErrorKind, Result};
pub use mako_protocol::journal::{Outcome, TransactionId};
use mako_protocol::wire::{Bound, Row, MAX_KEY, MAX_VALUE};
#[cfg(feature = "rocksdb-engine")]
pub use mako_runtime::node::Config as MakoConfig;
pub use options::{
    Direction, IteratorMode, OptimisticTransactionOptions, Options, ReadOptions, WriteOptions,
};
use std::{
    cell::RefCell,
    collections::{BTreeMap, BTreeSet},
    marker::PhantomData,
    sync::{Arc, Mutex},
};

pub const DEFAULT_COLUMN_FAMILY_NAME: &str = "default";
pub const MAX_ITERATOR_ROWS: usize = 4096;
pub const MAX_ITERATOR_BYTES: usize = 4 * 1024 * 1024;
pub const MAX_BATCH_OPERATIONS: usize = 4096;
pub const MAX_BATCH_BYTES: usize = 4 * 1024 * 1024;

#[derive(Clone, Debug)]
pub struct ColumnFamily {
    database: Arc<()>,
    table: u64,
    name: String,
}
impl ColumnFamily {
    pub fn name(&self) -> &str {
        &self.name
    }
}

/// Shared database handle. Column families are fixed by the opening schema.
pub struct OptimisticTransactionDB {
    backend: Arc<dyn backend::Backend>,
    identity: Arc<()>,
    families: BTreeMap<String, ColumnFamily>,
    pending: Mutex<BTreeSet<TransactionId>>,
}
/// DB operations are single distributed transactions.
pub type DB = OptimisticTransactionDB;

impl OptimisticTransactionDB {
    /// Mako extension: this handle currently has recovered coordinator authority.
    /// Open replicated followers remain available for replication while false.
    pub fn ready(&self) -> bool {
        self.backend.ready()
    }
    /// Adapter entry point, primarily for isolated contract tests. Production
    /// callers use `open`, `open_cf` or `open_config` with the native feature.
    pub fn from_backend(backend: Arc<dyn backend::Backend>) -> Result<Self> {
        let identity = Arc::new(());
        let mut families = BTreeMap::new();
        let mut tables = BTreeSet::new();
        for (name, table) in backend.column_families() {
            if name.is_empty()
                || name.contains('\0')
                || table == 0
                || !tables.insert(table)
                || families.contains_key(&name)
            {
                return Err(Error::invalid("invalid or duplicate column family"));
            }
            families.insert(
                name.clone(),
                ColumnFamily {
                    database: identity.clone(),
                    table,
                    name,
                },
            );
        }
        if !families.contains_key(DEFAULT_COLUMN_FAMILY_NAME) {
            return Err(Error::invalid("schema needs the default column family"));
        }
        Ok(Self {
            backend,
            identity,
            families,
            pending: Mutex::new(BTreeSet::new()),
        })
    }
    pub fn cf_handle(&self, name: &str) -> Option<&ColumnFamily> {
        self.families.get(name)
    }
    pub fn transaction(&self) -> Transaction<'_, Self> {
        self.transaction_opt(
            &WriteOptions::default(),
            &OptimisticTransactionOptions::default(),
        )
    }
    pub fn transaction_opt(
        &self,
        write: &WriteOptions,
        options: &OptimisticTransactionOptions,
    ) -> Transaction<'_, Self> {
        let failure = write
            .validate()
            .and_then(|()| options.validate())
            .and_then(|()| self.backend.validate_write(write))
            .err();
        Transaction {
            db: self,
            state: RefCell::new(State {
                session: None,
                failure,
                options: write.clone(),
                transaction_options: options.clone(),
            }),
            marker: PhantomData,
        }
    }
    /// Resolve an identity returned by an unknown result, including after reopening
    /// this coordinator. Success means all participants acknowledged the decision.
    pub fn resolve(&self, id: TransactionId) -> Result<Outcome> {
        let result = self.backend.resolve(id);
        if result.is_ok() {
            self.pending
                .lock()
                .expect("pending transactions")
                .remove(&id);
        }
        result
    }
    /// Unknown commits and failed Drop cleanup observed by this handle. Persist
    /// returned IDs in the application if they must survive application restart.
    pub fn pending_transactions(&self) -> Vec<TransactionId> {
        self.pending
            .lock()
            .expect("pending transactions")
            .iter()
            .copied()
            .collect()
    }
    fn remember(&self, result: &Result<()>) {
        if let Err(error) = result {
            if error.kind() == ErrorKind::UnknownCommit {
                if let Some(id) = error.transaction_id() {
                    self.pending
                        .lock()
                        .expect("pending transactions")
                        .insert(id);
                }
            }
        }
    }
    fn table(&self, cf: Option<&ColumnFamily>) -> Result<u64> {
        let cf = cf.unwrap_or(&self.families[DEFAULT_COLUMN_FAMILY_NAME]);
        if !Arc::ptr_eq(&cf.database, &self.identity) {
            return Err(Error::invalid(
                "column family belongs to another database handle",
            ));
        }
        Ok(cf.table)
    }
    fn run<T>(
        &self,
        write: &WriteOptions,
        body: impl FnOnce(&Transaction<'_, Self>) -> Result<T>,
    ) -> Result<T> {
        let tx = self.transaction_opt(write, &OptimisticTransactionOptions::default());
        match body(&tx) {
            Ok(value) => {
                tx.commit()?;
                Ok(value)
            }
            Err(error) => {
                tx.rollback()?;
                Err(error)
            }
        }
    }
    pub fn get<K: AsRef<[u8]>>(&self, key: K) -> Result<Option<Vec<u8>>> {
        self.get_opt(key, &ReadOptions::default())
    }
    pub fn get_opt<K: AsRef<[u8]>>(
        &self,
        key: K,
        options: &ReadOptions,
    ) -> Result<Option<Vec<u8>>> {
        self.run(&WriteOptions::default(), |tx| tx.get_opt(key, options))
    }
    pub fn get_cf<K: AsRef<[u8]>>(&self, cf: &ColumnFamily, key: K) -> Result<Option<Vec<u8>>> {
        self.get_cf_opt(cf, key, &ReadOptions::default())
    }
    pub fn get_cf_opt<K: AsRef<[u8]>>(
        &self,
        cf: &ColumnFamily,
        key: K,
        options: &ReadOptions,
    ) -> Result<Option<Vec<u8>>> {
        self.run(&WriteOptions::default(), |tx| {
            tx.get_cf_opt(cf, key, options)
        })
    }
    pub fn put<K: AsRef<[u8]>, V: AsRef<[u8]>>(&self, key: K, value: V) -> Result<()> {
        self.put_opt(key, value, &WriteOptions::default())
    }
    pub fn put_opt<K: AsRef<[u8]>, V: AsRef<[u8]>>(
        &self,
        key: K,
        value: V,
        options: &WriteOptions,
    ) -> Result<()> {
        self.run(options, |tx| tx.put(key, value))
    }
    pub fn put_cf<K: AsRef<[u8]>, V: AsRef<[u8]>>(
        &self,
        cf: &ColumnFamily,
        key: K,
        value: V,
    ) -> Result<()> {
        self.put_cf_opt(cf, key, value, &WriteOptions::default())
    }
    pub fn put_cf_opt<K: AsRef<[u8]>, V: AsRef<[u8]>>(
        &self,
        cf: &ColumnFamily,
        key: K,
        value: V,
        options: &WriteOptions,
    ) -> Result<()> {
        self.run(options, |tx| tx.put_cf(cf, key, value))
    }
    pub fn delete<K: AsRef<[u8]>>(&self, key: K) -> Result<()> {
        self.delete_opt(key, &WriteOptions::default())
    }
    pub fn delete_opt<K: AsRef<[u8]>>(&self, key: K, options: &WriteOptions) -> Result<()> {
        self.run(options, |tx| tx.delete(key))
    }
    pub fn delete_cf<K: AsRef<[u8]>>(&self, cf: &ColumnFamily, key: K) -> Result<()> {
        self.delete_cf_opt(cf, key, &WriteOptions::default())
    }
    pub fn delete_cf_opt<K: AsRef<[u8]>>(
        &self,
        cf: &ColumnFamily,
        key: K,
        options: &WriteOptions,
    ) -> Result<()> {
        self.run(options, |tx| tx.delete_cf(cf, key))
    }
    pub fn multi_get<K: AsRef<[u8]>, I: IntoIterator<Item = K>>(
        &self,
        keys: I,
    ) -> Vec<Result<Option<Vec<u8>>>> {
        let tx = self.transaction();
        let results = tx.multi_get(keys);
        match tx.commit() {
            Ok(()) => results,
            Err(error) => results.into_iter().map(|_| Err(error.clone())).collect(),
        }
    }
    pub fn write(&self, batch: WriteBatch) -> Result<()> {
        self.write_opt(batch, &WriteOptions::default())
    }
    pub fn write_opt(&self, batch: WriteBatch, options: &WriteOptions) -> Result<()> {
        if let Some(error) = batch.error {
            return Err(error);
        }
        // Validate every handle before any backend work.
        for entry in &batch.entries {
            self.table(entry.cf.as_ref())?;
        }
        self.run(options, |tx| {
            for entry in batch.entries {
                tx.mutate(entry.cf.as_ref(), &entry.key, entry.value.as_deref())?;
            }
            Ok(())
        })
    }
    pub fn iterator(&self, mode: IteratorMode<'_>) -> DBIterator<'_> {
        self.iterator_opt(mode, ReadOptions::default())
    }
    pub fn iterator_opt(&self, mode: IteratorMode<'_>, options: ReadOptions) -> DBIterator<'_> {
        self.iterate(None, mode, options)
    }
    pub fn iterator_cf(&self, cf: &ColumnFamily, mode: IteratorMode<'_>) -> DBIterator<'_> {
        self.iterator_cf_opt(cf, ReadOptions::default(), mode)
    }
    pub fn iterator_cf_opt(
        &self,
        cf: &ColumnFamily,
        options: ReadOptions,
        mode: IteratorMode<'_>,
    ) -> DBIterator<'_> {
        self.iterate(Some(cf), mode, options)
    }
    fn iterate(
        &self,
        cf: Option<&ColumnFamily>,
        mode: IteratorMode<'_>,
        options: ReadOptions,
    ) -> DBIterator<'_> {
        DBIterator::new(self.run(&WriteOptions::default(), |tx| tx.collect(cf, mode, options)))
    }
}

struct State {
    session: Option<Box<dyn backend::Session>>,
    failure: Option<Error>,
    options: WriteOptions,
    transaction_options: OptimisticTransactionOptions,
}
/// Methods mirror the shared-reference Rust binding. Transactions are Send but
/// not Sync. Initialization is lazy; the first operation can return Busy.
///
/// ```compile_fail
/// fn require_sync<T: Sync>() {}
/// require_sync::<mako_rocksdb::Transaction<'_>>();
/// ```
pub struct Transaction<'db, D = OptimisticTransactionDB> {
    db: &'db OptimisticTransactionDB,
    state: RefCell<State>,
    marker: PhantomData<&'db D>,
}
impl Transaction<'_> {
    fn operation<T>(&self, body: impl FnOnce(&mut dyn backend::Session) -> Result<T>) -> Result<T> {
        let mut state = self.state.borrow_mut();
        if let Some(error) = &state.failure {
            return Err(error.clone());
        }
        let result = (|| {
            if state.session.is_none() {
                state.session = Some(self.db.backend.begin()?);
            }
            body(state.session.as_deref_mut().expect("started transaction"))
        })();
        if let Err(error) = &result {
            state.failure = Some(error.clone());
            if state.session.is_none() {
                self.db.remember(&Err(error.clone()));
            }
        }
        result
    }
    /// Available after the first operation has started an attempt.
    pub fn transaction_id(&self) -> Option<TransactionId> {
        self.state.borrow().session.as_ref().map(|s| s.id())
    }
    pub fn get<K: AsRef<[u8]>>(&self, key: K) -> Result<Option<Vec<u8>>> {
        self.read(None, key.as_ref())
    }
    pub fn get_opt<K: AsRef<[u8]>>(
        &self,
        key: K,
        _options: &ReadOptions,
    ) -> Result<Option<Vec<u8>>> {
        self.get(key)
    }
    pub fn get_cf<K: AsRef<[u8]>>(&self, cf: &ColumnFamily, key: K) -> Result<Option<Vec<u8>>> {
        self.read(Some(cf), key.as_ref())
    }
    pub fn get_cf_opt<K: AsRef<[u8]>>(
        &self,
        cf: &ColumnFamily,
        key: K,
        _options: &ReadOptions,
    ) -> Result<Option<Vec<u8>>> {
        self.get_cf(cf, key)
    }
    pub fn get_for_update<K: AsRef<[u8]>>(
        &self,
        key: K,
        _exclusive: bool,
    ) -> Result<Option<Vec<u8>>> {
        self.get(key)
    }
    pub fn get_for_update_cf<K: AsRef<[u8]>>(
        &self,
        cf: &ColumnFamily,
        key: K,
        _exclusive: bool,
    ) -> Result<Option<Vec<u8>>> {
        self.get_cf(cf, key)
    }
    fn read(&self, cf: Option<&ColumnFamily>, key: &[u8]) -> Result<Option<Vec<u8>>> {
        self.operation(|session| {
            validate_key(key)?;
            session.get(self.db.table(cf)?, key)
        })
    }
    pub fn put<K: AsRef<[u8]>, V: AsRef<[u8]>>(&self, key: K, value: V) -> Result<()> {
        self.mutate(None, key.as_ref(), Some(value.as_ref()))
    }
    pub fn put_cf<K: AsRef<[u8]>, V: AsRef<[u8]>>(
        &self,
        cf: &ColumnFamily,
        key: K,
        value: V,
    ) -> Result<()> {
        self.mutate(Some(cf), key.as_ref(), Some(value.as_ref()))
    }
    pub fn delete<K: AsRef<[u8]>>(&self, key: K) -> Result<()> {
        self.mutate(None, key.as_ref(), None)
    }
    pub fn delete_cf<K: AsRef<[u8]>>(&self, cf: &ColumnFamily, key: K) -> Result<()> {
        self.mutate(Some(cf), key.as_ref(), None)
    }
    fn mutate(&self, cf: Option<&ColumnFamily>, key: &[u8], value: Option<&[u8]>) -> Result<()> {
        self.operation(|session| {
            validate_key(key)?;
            let table = self.db.table(cf)?;
            if let Some(value) = value {
                validate_value(value)?;
                session.put(table, key, value)
            } else {
                session.delete(table, key)
            }
        })
    }
    pub fn multi_get<K: AsRef<[u8]>, I: IntoIterator<Item = K>>(
        &self,
        keys: I,
    ) -> Vec<Result<Option<Vec<u8>>>> {
        keys.into_iter().map(|key| self.get(key)).collect()
    }
    pub fn commit(self) -> Result<()> {
        let mut state = self.state.borrow_mut();
        let result = if let Some(error) = state.failure.take() {
            match state.session.take().map(|s| s.rollback()).transpose() {
                Ok(_) => Err(error),
                Err(cleanup) => Err(cleanup),
            }
        } else {
            state.session.take().map_or(Ok(()), |s| s.commit())
        };
        self.db.remember(&result);
        result
    }
    /// Discard staged work. Like the reference binding, successful rollback
    /// leaves this handle reusable; its next operation starts a new attempt.
    pub fn rollback(&self) -> Result<()> {
        let mut state = self.state.borrow_mut();
        if state.session.is_none() {
            if let Some(error) = &state.failure {
                if error.kind() == ErrorKind::UnknownCommit {
                    return Err(error.clone());
                }
            }
        }
        let result = state.session.take().map_or(Ok(()), |s| s.rollback());
        self.db.remember(&result);
        state.failure = result.as_ref().err().cloned().or_else(|| {
            state
                .options
                .validate()
                .and_then(|()| state.transaction_options.validate())
                .and_then(|()| self.db.backend.validate_write(&state.options))
                .err()
        });
        result
    }
    pub fn iterator(&self, mode: IteratorMode<'_>) -> DBIterator<'_> {
        self.iterator_opt(mode, ReadOptions::default())
    }
    pub fn iterator_opt(&self, mode: IteratorMode<'_>, options: ReadOptions) -> DBIterator<'_> {
        DBIterator::new(self.collect(None, mode, options))
    }
    pub fn iterator_cf(&self, cf: &ColumnFamily, mode: IteratorMode<'_>) -> DBIterator<'_> {
        self.iterator_cf_opt(cf, ReadOptions::default(), mode)
    }
    pub fn iterator_cf_opt(
        &self,
        cf: &ColumnFamily,
        options: ReadOptions,
        mode: IteratorMode<'_>,
    ) -> DBIterator<'_> {
        DBIterator::new(self.collect(Some(cf), mode, options))
    }
    fn collect(
        &self,
        cf: Option<&ColumnFamily>,
        mode: IteratorMode<'_>,
        options: ReadOptions,
    ) -> Result<Vec<Row>> {
        self.operation(|session| {
            let table = self.db.table(cf)?;
            let (lower, upper, reverse) = bounds(mode, options)?;
            let rows = session.scan(table, lower, upper, reverse, MAX_ITERATOR_ROWS + 1)?;
            if rows.len() > MAX_ITERATOR_ROWS
                || rows
                    .iter()
                    .map(|row| row.key.len() + row.value.len())
                    .sum::<usize>()
                    > MAX_ITERATOR_BYTES
            {
                return Err(Error::new(
                    ErrorKind::Incomplete,
                    "iterator exceeds 4096 rows or 4 MiB; use narrower bounds",
                ));
            }
            Ok(rows)
        })
    }
}
impl<D> Drop for Transaction<'_, D> {
    fn drop(&mut self) {
        if let Some(session) = self.state.get_mut().session.take() {
            self.db.remember(&session.rollback());
        }
    }
}

pub type KVBytes = (Box<[u8]>, Box<[u8]>);
/// Eager, bounded iterator. Errors are yielded once; an error never appears as EOF.
pub struct DBIterator<'a> {
    rows: std::vec::IntoIter<Result<KVBytes>>,
    marker: PhantomData<&'a ()>,
}
impl DBIterator<'_> {
    fn new(rows: Result<Vec<Row>>) -> Self {
        let rows = match rows {
            Ok(rows) => rows
                .into_iter()
                .map(|r| Ok((r.key.into_boxed_slice(), r.value.into_boxed_slice())))
                .collect(),
            Err(error) => vec![Err(error)],
        };
        Self {
            rows: rows.into_iter(),
            marker: PhantomData,
        }
    }
}
impl Iterator for DBIterator<'_> {
    type Item = Result<KVBytes>;
    fn next(&mut self) -> Option<Self::Item> {
        self.rows.next()
    }
    fn size_hint(&self) -> (usize, Option<usize>) {
        self.rows.size_hint()
    }
}
impl std::iter::FusedIterator for DBIterator<'_> {}

fn validate_key(key: &[u8]) -> Result<()> {
    if key.len() > MAX_KEY {
        Err(Error::invalid("key exceeds 1024 bytes"))
    } else {
        Ok(())
    }
}
fn validate_value(value: &[u8]) -> Result<()> {
    if value.len() > MAX_VALUE {
        Err(Error::invalid("value exceeds 65535 bytes"))
    } else {
        Ok(())
    }
}
fn bounds(mode: IteratorMode<'_>, options: ReadOptions) -> Result<(Bound, Bound, bool)> {
    if let Some(key) = &options.lower {
        validate_key(key)?;
    }
    if let Some(key) = &options.upper {
        validate_key(key)?;
    }
    let mut lower = options.lower.map_or(Bound::Unbounded, Bound::Included);
    let mut upper = options.upper.map_or(Bound::Unbounded, Bound::Excluded);
    let reverse = match mode {
        IteratorMode::Start => false,
        IteratorMode::End => true,
        IteratorMode::From(key, direction) => {
            validate_key(key)?;
            if direction == Direction::Forward {
                if !matches!(&lower, Bound::Included(v) if v.as_slice() >= key) {
                    lower = Bound::Included(key.to_vec());
                }
            } else if !matches!(&upper, Bound::Excluded(v) if v.as_slice() <= key) {
                upper = Bound::Included(key.to_vec());
            }
            direction == Direction::Reverse
        }
    };
    Ok((lower, upper, reverse))
}

#[cfg(test)]
mod tests;
