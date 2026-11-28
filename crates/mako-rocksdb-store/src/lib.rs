//! Mako's byte-store contract over the maintained `rocksdb` crate.
//! Native ownership, error allocation and iterator lifetimes belong to upstream.
#![forbid(unsafe_code)]

use rocksdb::{DBRecoveryMode, DBWithThreadMode, MultiThreaded, FlushOptions, Options, WriteBatch, WriteOptions};
type DB = DBWithThreadMode<MultiThreaded>;
use std::path::{Path, PathBuf};
use std::sync::Arc;

mod bytes;
pub use bytes::{BlobError, BlobOp, Blobs, BoundedEntryVisitor, EntryVisitor};

fn error(error: rocksdb::Error) -> BlobError {
    BlobError(format!("rocksdb: {error}"))
}

/// Durability of each atomic batch.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Durability {
    /// WAL enabled, with an fsync before acknowledging the batch.
    Sync,
    /// WAL enabled, with asynchronous disk persistence.
    Wal,
    /// WAL disabled. Requires another durable authority for recovery.
    None,
}

/// Synchronous observations around a batch submission, not inside the native WAL.
#[cfg(feature = "test-hooks")]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WriteBatchHookPoint {
    BatchConstructed,
    BeforeWrite,
    AfterWrite,
}

/// Persistent rows, shared by the direct engine and optional Masstree checkpoint path.
pub struct RocksBlobs {
    path: PathBuf,
    db: DB,
    write: WriteOptions,
    #[cfg(feature = "test-hooks")]
    write_batch_observer: Option<Arc<dyn Fn(WriteBatchHookPoint) + Send + Sync + 'static>>,
}

impl RocksBlobs {
    /// Open or create with ordered writes and point-in-time WAL recovery.
    pub fn open(path: &Path, durability: Durability) -> Result<Self, BlobError> {
        Self::open_with_recovery(path, durability, false, true)
    }

    /// Open or create; reject damaged WALs rather than salvaging a prefix.
    pub fn open_strict(path: &Path, durability: Durability) -> Result<Self, BlobError> {
        Self::open_with_recovery(path, durability, true, true)
    }

    /// Reopen without replacing a missing durable authority.
    pub fn open_existing_strict(path: &Path, durability: Durability) -> Result<Self, BlobError> {
        Self::open_with_recovery(path, durability, true, false)
    }

    fn open_with_recovery(path: &Path, durability: Durability, strict: bool, create: bool)
        -> Result<Self, BlobError>
    {
        let path = std::path::absolute(path)
            .map_err(|error| BlobError(format!("database absolute path: {error}")))?;
        // Keep the existing buffer, cache and background-thread defaults.
        let mut options = Options::default();
        options.create_if_missing(create);
        options.set_wal_recovery_mode(if strict {
            DBRecoveryMode::AbsoluteConsistency
        } else {
            DBRecoveryMode::PointInTime
        });
        options.set_unordered_write(false);
        let mut write = WriteOptions::default();
        write.set_sync(durability == Durability::Sync);
        write.disable_wal(durability == Durability::None);
        let families = if path.join("CURRENT").exists() {
            DB::list_cf(&options, &path).map_err(error)?
        } else { vec!["default".to_owned()] };
        let db = DB::open_cf(&options, &path, families).map_err(error)?;
        Ok(Self {
            path, db, write,
            #[cfg(feature = "test-hooks")]
            write_batch_observer: None,
        })
    }

    /// Install a synchronous observer around this database's batch writes.
    ///
    /// This test-only facility is intentionally per database, avoiding global
    /// state that could intercept unrelated writers. The observer runs inline
    /// on the calling thread and may rendezvous with a crash-test controller;
    /// a write cannot advance past a point until the observer returns.
    /// Concurrent writes may invoke the observer concurrently.
    #[cfg(feature = "test-hooks")]
    pub fn set_write_batch_observer<F>(&mut self, observer: F)
    where
        F: Fn(WriteBatchHookPoint) + Send + Sync + 'static,
    {
        self.write_batch_observer = Some(std::sync::Arc::new(observer));
    }

    /// Remove this database's batch-write observer.
    #[cfg(feature = "test-hooks")]
    pub fn clear_write_batch_observer(&mut self) {
        self.write_batch_observer = None;
    }

    #[cfg(feature = "test-hooks")]
    #[inline]
    fn observe_write_batch(&self, point: WriteBatchHookPoint) {
        if let Some(observer) = &self.write_batch_observer {
            observer(point);
        }
    }

    /// Wait for memtables to reach SST files.
    pub fn flush(&self) -> Result<(), BlobError> {
        let mut options = FlushOptions::default();
        options.set_wait(true);
        self.db.flush_opt(&options).map_err(error)
    }

    /// Lease a native snapshot, retaining the database for the lease's lifetime.
    pub fn snapshot(self: &Arc<Self>) -> Result<Snapshot, BlobError> {
        Ok(Snapshot {
            lease: SnapshotLease::new(Arc::clone(self), |owner| owner.db.snapshot()),
        })
    }

    /// Create a consistent native checkpoint at a nonexistent destination.
    pub fn checkpoint(&self, destination: &Path) -> Result<(), BlobError> {
        rocksdb::checkpoint::Checkpoint::new(&self.db)
            .and_then(|checkpoint| checkpoint.create_checkpoint(destination))
            .map_err(error)
    }

    /// Discover native families without creating or modifying a database.
    pub fn list_families(path: &Path) -> Result<Vec<String>, BlobError> {
        DB::list_cf(&Options::default(), path).map_err(error)
    }

    pub fn has_family(&self, name: &str) -> bool { self.db.cf_handle(name).is_some() }

    /// Schema callers serialize creation. Existing families keep their options.
    pub fn ensure_family(&self, name: &str) -> Result<(), BlobError> {
        if !self.has_family(name) { self.db.create_cf(name, &Options::default()).map_err(error)?; }
        Ok(())
    }

    pub fn get_cf(&self, family: &str, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
        let cf = self.db.cf_handle(family).ok_or_else(|| BlobError(format!("unknown family {family}")))?;
        self.db.get_cf(&cf, key).map_err(error)
    }

    /// One atomic WAL batch, including operations in different column families.
    pub fn write_families(&self, ops: &[(&str, BlobOp<'_>)]) -> Result<(), BlobError> {
        let mut batch = WriteBatch::default();
        for (name, op) in ops {
            let cf = self.db.cf_handle(name).ok_or_else(|| BlobError(format!("unknown family {name}")))?;
            match op {
                BlobOp::Put { key, val } => batch.put_cf(&cf, key, val),
                BlobOp::Delete { key } => batch.delete_cf(&cf, key),
            }
        }
        #[cfg(feature = "test-hooks")]
        self.observe_write_batch(WriteBatchHookPoint::BatchConstructed);
        #[cfg(feature = "test-hooks")]
        self.observe_write_batch(WriteBatchHookPoint::BeforeWrite);
        let result = self.db.write_opt(batch, &self.write);
        #[cfg(feature = "test-hooks")]
        self.observe_write_batch(WriteBatchHookPoint::AfterWrite);
        result.map_err(error)
    }
}

impl Blobs for RocksBlobs {
    /// Sum logical lengths of regular files immediately inside the database
    /// directory, including WAL, SST, MANIFEST and diagnostic files. This does
    /// not measure allocated filesystem blocks or recurse into subdirectories.
    /// Compaction may create or remove files during this observation.
    fn disk_usage_bytes(&self) -> Result<Option<u64>, BlobError> {
        let entries = std::fs::read_dir(&self.path)
            .map_err(|error| BlobError(format!("database disk usage: {error}")))?;
        let mut bytes = 0_u64;
        for entry in entries {
            let entry =
                entry.map_err(|error| BlobError(format!("database disk usage: {error}")))?;
            let metadata = match entry.metadata() {
                Ok(metadata) => metadata,
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => continue,
                Err(error) => return Err(BlobError(format!("database disk usage: {error}"))),
            };
            if metadata.is_file() {
                bytes = bytes
                    .checked_add(metadata.len())
                    .ok_or_else(|| BlobError("database disk usage overflow".into()))?;
            }
        }
        Ok(Some(bytes))
    }

    fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
        self.db.get(key).map_err(error)
    }

    fn write_batch(&self, ops: &[BlobOp<'_>]) -> Result<(), BlobError> {
        if ops.is_empty() { return Ok(()); }
        let mut batch = WriteBatch::default();
        for op in ops {
            match op {
                BlobOp::Put { key, val } => batch.put(key, val),
                BlobOp::Delete { key } => batch.delete(key),
            }
        }
        #[cfg(feature = "test-hooks")]
        self.observe_write_batch(WriteBatchHookPoint::BatchConstructed);
        #[cfg(feature = "test-hooks")]
        self.observe_write_batch(WriteBatchHookPoint::BeforeWrite);
        let result = self.db.write_opt(batch, &self.write);
        #[cfg(feature = "test-hooks")]
        self.observe_write_batch(WriteBatchHookPoint::AfterWrite);
        result.map_err(error)
    }

    fn for_each_key(&self, f: &mut dyn FnMut(&[u8])) -> Result<(), BlobError> {
        self.for_each_entry(&mut |key, _| { f(key); Ok(()) })
    }

    fn for_each_entry(&self, f: &mut EntryVisitor<'_>) -> Result<(), BlobError> {
        self.for_each_entry_from(b"", usize::MAX, &mut |key, value| {
            f(key, value)?;
            Ok(true)
        })
    }

    fn for_each_entry_from(&self, start: &[u8], max_entries: usize,
        visitor: &mut BoundedEntryVisitor<'_>) -> Result<(), BlobError>
    {
        if max_entries == 0 { return Ok(()); }
        let mut iterator = self.db.raw_iterator();
        iterator.seek(start);
        let mut visited = 0;
        while let Some((key, value)) = iterator.item() {
            visited += 1;
            if !visitor(key, value)? || visited == max_entries { break; }
            iterator.next();
        }
        iterator.status().map_err(error)
    }
}

// Upstream snapshots borrow DB. The safe ownership cell drops that borrow before
// the Arc owner, allowing transactions to retain snapshots after their DB handle
// is dropped without hand-written lifetime extension or raw pointers.
type NativeSnapshot<'a> = rocksdb::SnapshotWithThreadMode<'a, DB>;
self_cell::self_cell! {
    struct SnapshotLease {
        owner: Arc<RocksBlobs>,
        #[covariant]
        dependent: NativeSnapshot,
    }
}

pub struct Snapshot {
    lease: SnapshotLease,
}

impl Snapshot {
    pub fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
        self.lease.borrow_dependent().get(key).map_err(error)
    }

    pub fn get_cf(&self, family: &str, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
        let cf = self.lease.borrow_owner().db.cf_handle(family)
            .ok_or_else(|| BlobError(format!("unknown family {family}")))?;
        self.lease.borrow_dependent().get_cf(&cf, key).map_err(error)
    }

    /// Visit `[start, end)` in either direction, stopping on limit or callback.
    pub fn scan(&self, start: &[u8], end: Option<&[u8]>, reverse: bool,
        limit: usize, visitor: &mut BoundedEntryVisitor<'_>) -> Result<(), BlobError>
    {
        self.scan_cf("default", start, end, reverse, limit, visitor)
    }

    pub fn scan_cf(&self, family: &str, start: &[u8], end: Option<&[u8]>, reverse: bool,
        limit: usize, visitor: &mut BoundedEntryVisitor<'_>) -> Result<(), BlobError>
    {
        if limit == 0 || end.is_some_and(|end| end <= start) { return Ok(()); }
        let cf = self.lease.borrow_owner().db.cf_handle(family)
            .ok_or_else(|| BlobError(format!("unknown family {family}")))?;
        let mut iterator = self.lease.borrow_dependent().raw_iterator_cf(&cf);
        if reverse {
            if let Some(end) = end { iterator.seek_for_prev(end); }
            else { iterator.seek_to_last(); }
        } else { iterator.seek(start); }
        let mut visited = 0;
        while let Some((key, value)) = iterator.item() {
            if key < start || visited == limit { break; }
            if end.is_some_and(|end| key >= end) {
                if !reverse { break; }
            } else {
                visited += 1;
                if !visitor(key, value)? { break; }
            }
            if reverse { iterator.prev(); } else { iterator.next(); }
        }
        iterator.status().map_err(error)
    }
}
