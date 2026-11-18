//! Native optimistic baseline, not the Rust STO adapter or a raw batch.
//!
//! Match the direct STO adapter's ordered writes, strict WAL recovery and
//! requested durability. Cache/write-buffer/compaction settings use the linked
//! library defaults; OptimisticTransactionDB additionally retains native
//! memtable history for tracked-key validation. Its indexed transaction batch,
//! begin-time snapshots and native OCC replace, rather than supplement, Rust STO.
//! There is no additional Rust value cache or conflict table.

use std::ffi::{CStr, CString};
use std::marker::PhantomData;
use std::path::Path;
use std::ptr;

use mrx_core::BlobError;

use crate::{database_options, sys, write_options, Durability, Err0};

/// Outcome of native optimistic commit.
#[derive(Debug)]
pub enum OptimisticCommitError {
    /// Native tracked-key validation found a concurrent write; nothing committed.
    Conflict,
    /// Any other native failure, including IO and insufficient validation history.
    /// This is not a conflict and must not be counted or retried as one. In
    /// particular, an IO failure does not establish whether a write took effect.
    Native(BlobError),
}

impl std::fmt::Display for OptimisticCommitError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Conflict => f.write_str("native optimistic transaction conflict"),
            Self::Native(error) => std::fmt::Display::fmt(error, f),
        }
    }
}

impl std::error::Error for OptimisticCommitError {}

/// RocksDB's actual OptimisticTransactionDB, shareable between worker threads.
///
/// Each transaction borrows this database, preventing closure while a snapshot,
/// transaction, or iterator remains alive. This API does not implement `Blobs`:
/// a transaction is not an interchangeable spelling of a raw `WriteBatch`.
pub struct OptimisticDatabase {
    db: *mut sys::rocksdb_optimistictransactiondb_t,
    options: *mut sys::rocksdb_options_t,
    write: *mut sys::rocksdb_writeoptions_t,
    transaction_options: *mut sys::rocksdb_optimistictransaction_options_t,
}

// SAFETY: RocksDB supports concurrent BeginTransaction/commit on one DB. All
// shared options are immutable after open; transaction handles are not shared.
unsafe impl Send for OptimisticDatabase {}
// SAFETY: as above; each worker owns its separate transaction and read options.
unsafe impl Sync for OptimisticDatabase {}

impl OptimisticDatabase {
    /// Open or create a native optimistic DB with strict WAL recovery.
    pub fn open(path: &Path, durability: Durability) -> Result<Self, BlobError> {
        let path = std::path::absolute(path)
            .map_err(|error| BlobError(format!("database absolute path: {error}")))?;
        let path = CString::new(path.as_os_str().as_encoded_bytes())
            .map_err(|_| BlobError("database path contains a NUL byte".into()))?;
        // SAFETY: newly owned options and DB handles are released by Drop on
        // both success and failure. The path remains alive throughout open.
        unsafe {
            let mut database = Self {
                db: ptr::null_mut(),
                options: database_options(true, true),
                write: write_options(durability),
                transaction_options: sys::rocksdb_optimistictransaction_options_create(),
            };
            sys::rocksdb_optimistictransaction_options_set_set_snapshot(
                database.transaction_options,
                1,
            );
            let mut error = Err0::new();
            database.db = sys::rocksdb_optimistictransactiondb_open(
                database.options,
                path.as_ptr(),
                error.as_mut(),
            );
            error.check("rocksdb_optimistictransactiondb_open")?;
            if database.db.is_null() {
                return Err(BlobError("optimistic database open returned null".into()));
            }
            Ok(database)
        }
    }

    /// Begin with an immediately established snapshot, not a first-read snapshot.
    pub fn begin(&self) -> Result<OptimisticTransaction<'_>, BlobError> {
        // SAFETY: the database and immutable options outlive the new transaction.
        // Passing no old transaction requests a fresh, exclusively owned handle.
        unsafe {
            let raw = sys::rocksdb_optimistictransaction_begin(
                self.db,
                self.write,
                self.transaction_options,
                ptr::null_mut(),
            );
            if raw.is_null() {
                return Err(BlobError("optimistic transaction begin returned null".into()));
            }
            let transaction = OptimisticTransaction {
                raw,
                read: sys::rocksdb_readoptions_create(),
                snapshot: sys::rocksdb_transaction_get_snapshot(raw),
                _database: self,
            };
            if transaction.snapshot.is_null() {
                return Err(BlobError("optimistic transaction has no begin snapshot".into()));
            }
            // SetSnapshot controls conflict validation, but read options must
            // also explicitly select that snapshot to obtain repeatable reads.
            sys::rocksdb_readoptions_set_snapshot(transaction.read, transaction.snapshot);
            Ok(transaction)
        }
    }
}

impl Drop for OptimisticDatabase {
    fn drop(&mut self) {
        // SAFETY: transactions borrow self and have already been destroyed.
        // Each handle is owned here, including during a failed open.
        unsafe {
            if !self.db.is_null() {
                sys::rocksdb_optimistictransactiondb_close(self.db);
            }
            sys::rocksdb_optimistictransaction_options_destroy(self.transaction_options);
            sys::rocksdb_writeoptions_destroy(self.write);
            sys::rocksdb_options_destroy(self.options);
        }
    }
}

/// A thread-local native optimistic transaction. Dropping it discards all
/// uncommitted writes. Reads (including absent keys) are tracked for commit-time
/// validation; pending writes/deletes are visible to the transaction itself.
pub struct OptimisticTransaction<'db> {
    raw: *mut sys::rocksdb_transaction_t,
    read: *mut sys::rocksdb_readoptions_t,
    snapshot: *const sys::rocksdb_snapshot_t,
    _database: &'db OptimisticDatabase,
}

/// Holds a C-allocated read result even when an error or panic leaves the call.
struct Value(*mut std::ffi::c_char);

impl Drop for Value {
    fn drop(&mut self) {
        // SAFETY: RocksDB owns the allocator; free(NULL) is supported.
        unsafe { sys::rocksdb_free(self.0.cast()) };
    }
}

/// An iterator cannot escape the transaction borrow or survive a batch mutation.
struct IteratorGuard<'tx> {
    raw: *mut sys::rocksdb_iterator_t,
    _transaction: PhantomData<&'tx mut sys::rocksdb_transaction_t>,
}

impl Drop for IteratorGuard<'_> {
    fn drop(&mut self) {
        // SAFETY: this guard exclusively owns the iterator.
        unsafe { sys::rocksdb_iter_destroy(self.raw) };
    }
}

impl OptimisticTransaction<'_> {
    /// Read the begin snapshot plus local writes and track the key for validation.
    /// An absent key is tracked too, so a concurrent insert conflicts at commit.
    pub fn get(&mut self, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
        let mut error = Err0::new();
        let mut length = 0;
        // SAFETY: native handles and key remain live for the call. Returned
        // memory is owned by Value and copied before it is freed.
        unsafe {
            let value = Value(sys::rocksdb_transaction_get_for_update(
                self.raw,
                self.read,
                key.as_ptr().cast(),
                key.len(),
                &mut length,
                1,
                error.as_mut(),
            ));
            error.check("rocksdb_transaction_get_for_update")?;
            if value.0.is_null() {
                Ok(None)
            } else {
                Ok(Some(std::slice::from_raw_parts(value.0.cast::<u8>(), length).to_vec()))
            }
        }
    }

    /// Stage a native tracked write, visible only to this transaction until commit.
    pub fn put(&mut self, key: &[u8], value: &[u8]) -> Result<(), BlobError> {
        let mut error = Err0::new();
        // SAFETY: the native indexed batch copies both borrowed byte slices.
        unsafe {
            sys::rocksdb_transaction_put(
                self.raw,
                key.as_ptr().cast(),
                key.len(),
                value.as_ptr().cast(),
                value.len(),
                error.as_mut(),
            );
        }
        error.check("rocksdb_transaction_put")
    }

    /// Stage a native tracked deletion.
    pub fn delete(&mut self, key: &[u8]) -> Result<(), BlobError> {
        let mut error = Err0::new();
        // SAFETY: the native indexed batch copies the borrowed key.
        unsafe {
            sys::rocksdb_transaction_delete(
                self.raw,
                key.as_ptr().cast(),
                key.len(),
                error.as_mut(),
            );
        }
        error.check("rocksdb_transaction_delete")
    }

    fn iterator(&mut self) -> Result<IteratorGuard<'_>, BlobError> {
        // SAFETY: iterator borrows the transaction; read options select its
        // snapshot. The guard must be destroyed before mutating the batch.
        let raw = unsafe { sys::rocksdb_transaction_create_iterator(self.raw, self.read) };
        if raw.is_null() {
            return Err(BlobError("optimistic transaction iterator returned null".into()));
        }
        Ok(IteratorGuard { raw, _transaction: PhantomData })
    }

    /// Return at most `limit` rows in bytewise order in `[lower, upper)`.
    ///
    /// The iterator sees the begin snapshot plus local writes. Returned keys are
    /// individually tracked with GetForUpdate, but the native optimistic engine
    /// does NOT certify the absent portions of a range: inserts into gaps or
    /// beyond a truncated result may commit without conflicting. This is NOT a
    /// phantom-safe serializable scan and must not be measured as one. A zero
    /// limit or empty/reversed interval returns no rows and tracks no keys.
    pub fn scan(
        &mut self,
        lower: &[u8],
        upper: &[u8],
        limit: usize,
    ) -> Result<Vec<(Vec<u8>, Vec<u8>)>, BlobError> {
        let mut rows = Vec::new();
        if limit == 0 || lower >= upper {
            return Ok(rows);
        }
        {
            let iterator = self.iterator()?;
            // SAFETY: the iterator and its buffers remain live; each key is
            // copied before advancement. Both key and row bounds stop traversal.
            unsafe {
                sys::rocksdb_iter_seek(iterator.raw, lower.as_ptr().cast(), lower.len());
                while rows.len() < limit && sys::rocksdb_iter_valid(iterator.raw) != 0 {
                    let mut length = 0;
                    let raw_key = sys::rocksdb_iter_key(iterator.raw, &mut length);
                    let key = std::slice::from_raw_parts(raw_key.cast::<u8>(), length);
                    if key >= upper {
                        break;
                    }
                    rows.push((key.to_vec(), Vec::new()));
                    if rows.len() < limit {
                        sys::rocksdb_iter_next(iterator.raw);
                    }
                }
                let mut error = Err0::new();
                sys::rocksdb_iter_get_error(iterator.raw, error.as_mut());
                error.check("rocksdb_transaction_iterator")?;
            }
        }
        // Destroy the iterator before further transaction calls. Fetch values
        // only once, through tracked reads of the same snapshot and local batch.
        for (key, value) in &mut rows {
            *value = self.get(key)?.ok_or_else(|| {
                BlobError("transaction snapshot iterator key disappeared".into())
            })?;
        }
        Ok(rows)
    }

    /// Validate native tracked keys and atomically commit the native write batch.
    /// Consumes the transaction on all outcomes; no implicit retries are made.
    pub fn commit(self) -> Result<(), OptimisticCommitError> {
        let mut error = Err0::new();
        // SAFETY: self exclusively owns a live transaction until this call ends.
        unsafe { sys::rocksdb_transaction_commit(self.raw, error.as_mut()) };
        if error.0.is_null() {
            return Ok(());
        }
        // RocksDB's C API exposes only Status::ToString, not a status code.
        // TransactionUtil::CheckKey returns exactly Status::Busy() on a key
        // conflict. Match only that complete bare status, never substrings or
        // general Busy/TryAgain/IO messages. Unknown library diagnostics remain
        // Native errors rather than silently inflating measured conflict rates.
        // SAFETY: a non-null errptr is an owned NUL-terminated RocksDB string.
        if unsafe { CStr::from_ptr(error.0) }.to_bytes() == b"Resource busy: " {
            return Err(OptimisticCommitError::Conflict);
        }
        error.check("rocksdb_transaction_commit").map_err(OptimisticCommitError::Native)
    }

    /// Explicitly discard pending writes and release this transaction's snapshot.
    pub fn rollback(self) -> Result<(), BlobError> {
        let mut error = Err0::new();
        // SAFETY: self exclusively owns the native transaction.
        unsafe { sys::rocksdb_transaction_rollback(self.raw, error.as_mut()) };
        error.check("rocksdb_transaction_rollback")
    }
}

impl Drop for OptimisticTransaction<'_> {
    fn drop(&mut self) {
        // SAFETY: no iterator can outlive its transaction borrow. The C snapshot
        // wrapper is separately allocated (c.h requires rocksdb_free), while the
        // underlying snapshot belongs to the transaction. Do not release it via
        // rocksdb_release_snapshot or delete the database through a base handle.
        unsafe {
            sys::rocksdb_readoptions_destroy(self.read);
            sys::rocksdb_free(self.snapshot.cast_mut().cast());
            sys::rocksdb_transaction_destroy(self.raw);
        }
    }
}
