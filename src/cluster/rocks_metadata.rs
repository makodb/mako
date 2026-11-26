//! Production byte-map adapter for migration metadata only, not application rows.
//!
//! This is a trusted, unverified foreign-storage boundary. RocksDB supplies atomic
//! WriteBatch recovery, checksums and synchronous WAL persistence; the filesystem
//! and device must honor locking and fsync. A successful settlement closes the
//! old DB, reopens with strict WAL recovery, and durably flushes recovered state.
//! No uncertain batch is replayed by this adapter. Corruption can permanently
//! prevent settlement rather than silently roll acknowledged metadata back.
//!
//! The persistent writer-lock file is independent of RocksDB's own LOCK and stays
//! locked across close/reopen. All users of a namespace must obey this lock, and
//! operators must not unlink/replace its directory or lock file while in use.
//! Initialization requires an absent namespace directory; an incomplete earlier
//! initialization is retained for inspection, never erased or reinitialized.
#![cfg(not(verus_keep_ghost))]
#![forbid(unsafe_code)]

use crate::durable_kv::{CommitResult, DurableKv, ReadResult, WriteOp};
use crate::types::Status;
use std::cell::Cell;
use rocksdb::{DBRecoveryMode, FlushOptions, Options, ReadOptions, WriteBatch, WriteOptions, DB};
use std::fs::{self, File, OpenOptions, TryLockError};
use std::io::{self, Write};
use std::marker::PhantomData;
use std::os::unix::ffi::OsStrExt;
use std::path::{Path, PathBuf};

const WRITER_LOCK: &str = "MAKO_METADATA_WRITER_LOCK";

// Best-effort diagnostics must not panic after an uncertain native submission.
// Callers supply operation names and native I/O reasons, never keys or values.
fn report(operation: &str, error: impl std::fmt::Display) {
    let _ = writeln!(io::stderr().lock(), "[shard metadata] {operation}: {error}");
}

fn filesystem_error(operation: &str, error: io::Error) -> Status {
    report(operation, &error);
    match error.kind() {
        io::ErrorKind::NotFound => Status::NotFound,
        io::ErrorKind::InvalidInput => Status::Invalid,
        _ => Status::Io,
    }
}

fn namespace_directory(path: &Path, initialize: bool) -> Result<(PathBuf, File), Status> {
    if path.file_name().is_none() || path.as_os_str().as_bytes().contains(&0) {
        report("opening metadata namespace", "expected a directory name without NUL bytes");
        return Err(Status::Invalid);
    }
    if initialize {
        let parent = path.parent().filter(|p| !p.as_os_str().is_empty()).unwrap_or(Path::new("."));
        fs::create_dir_all(parent).map_err(|error| filesystem_error("creating metadata parents", error))?;
        fs::create_dir(path).map_err(|error| {
            if error.kind() == io::ErrorKind::AlreadyExists {
                report("initializing metadata namespace", "directory already exists; refusing replacement");
                Status::Invalid
            } else {
                filesystem_error("creating metadata namespace", error)
            }
        })?;
    }
    // Canonicalization makes aliases use the same lock and fixes relative paths
    // for a later reopen, even if the process changes its working directory.
    let directory = fs::canonicalize(path)
        .map_err(|error| filesystem_error("locating metadata namespace", error))?;
    let lock = OpenOptions::new().read(true).write(true).create_new(initialize)
        .open(directory.join(WRITER_LOCK))
        .map_err(|error| filesystem_error("opening metadata writer lock", error))?;
    lock.try_lock().map_err(|error| match error {
        TryLockError::WouldBlock => {
            report("locking metadata namespace", "another writer owns the namespace");
            Status::Busy
        }
        TryLockError::Error(error) => filesystem_error("locking metadata namespace", error),
    })?;
    if initialize {
        lock.sync_all().map_err(|error| filesystem_error("syncing metadata writer lock", error))?;
    }
    Ok((directory, lock))
}

fn sync_parents(directory: &Path) -> Result<(), Status> {
    // Sync every ancestor, not only the role directory: create_dir_all may have
    // introduced several parent entries. Existing ancestors are harmless to sync.
    if let Some(parent) = directory.parent() {
        for ancestor in parent.ancestors() {
            File::open(ancestor).and_then(|file| file.sync_all())
                .map_err(|error| filesystem_error("syncing metadata parent directory", error))?;
        }
    }
    Ok(())
}

/// Owned metadata backend. All operations are serialized by `&mut`.
/// A write error fences reads and submissions until a real recovery barrier.
pub struct RocksKv {
    // Drop the DB before releasing the independent lifetime writer lock.
    database: Option<DB>,
    options: Options,
    read_options: ReadOptions,
    write_options: WriteOptions,
    flush_options: FlushOptions,
    path: PathBuf,
    directory: File,
    fenced: bool,
    _not_sync: PhantomData<Cell<()>>,
    _writer_lock: File,
}

fn native_error(operation: &str, error: rocksdb::Error) -> Status {
    report(operation, error);
    Status::Io
}

impl RocksKv {
    /// Initialize an absent namespace, or retain an existing store without repair.
    pub fn open(path: &Path, initialize: bool) -> Result<Self, Status> {
        let (root, writer_lock) = namespace_directory(path, initialize)?;
        let directory = File::open(&root)
            .map_err(|error| filesystem_error("opening metadata directory for sync", error))?;
        // Reject unsupported WAL verification options; never silently weaken recovery.
        let mut options = Options::default()
            .get_options_from_string("track_and_verify_wals_in_manifest=true")
            .map_err(|error| native_error("configuring metadata WAL verification", error))?;
        options.create_if_missing(initialize);
        options.set_error_if_exists(initialize);
        options.set_paranoid_checks(true);
        options.set_wal_recovery_mode(DBRecoveryMode::AbsoluteConsistency);
        let mut read_options = ReadOptions::default();
        read_options.set_verify_checksums(true);
        let mut write_options = WriteOptions::default();
        write_options.set_sync(true);
        write_options.disable_wal(false);
        let mut flush_options = FlushOptions::default();
        flush_options.set_wait(true);
        let database = DB::open(&options, &root)
            .map_err(|error| native_error("opening RocksDB metadata", error))?;
        // Settlement must reopen, never recreate missing storage.
        options.create_if_missing(false);
        options.set_error_if_exists(false);
        let mut kv = Self {
            database: Some(database), options, read_options, write_options, flush_options,
            path: root.clone(), directory, fenced: true,
            _not_sync: PhantomData, _writer_lock: writer_lock,
        };
        if !kv.durability_barrier() { return Err(Status::Io); }
        if initialize { sync_parents(&root)?; }
        kv.fenced = false;
        Ok(kv)
    }

    fn durability_barrier(&mut self) -> bool {
        let Some(database) = &self.database else { return false; };
        if let Err(error) = database.flush_wal(true) {
            report("syncing metadata WAL", error);
            return false;
        }
        // A waiting flush persists recovered memtables even if reopen rotated WAL.
        if let Err(error) = database.flush_opt(&self.flush_options) {
            report("flushing recovered metadata", error);
            return false;
        }
        if let Err(error) = self.directory.sync_all() {
            report("syncing metadata directory", error);
            return false;
        }
        true
    }
}

impl DurableKv for RocksKv {
    fn read(&mut self, key: &[u8]) -> ReadResult {
        if self.fenced { return ReadResult::Io; }
        let Some(database) = &self.database else { return ReadResult::Io; };
        match database.get_pinned_opt(key, &self.read_options) {
            Ok(Some(value)) => ReadResult::Found(value.to_vec()),
            Ok(None) => ReadResult::Absent,
            Err(error) => {
                report("reading metadata", error);
                self.fenced = true;
                ReadResult::Io
            }
        }
    }

    fn submit(&mut self, operations: Vec<WriteOp>) -> CommitResult {
        if self.fenced { return CommitResult::NotCommitted; }
        let Some(database) = &self.database else { return CommitResult::NotCommitted; };
        let mut batch = WriteBatch::default();
        for operation in operations {
            match operation.value {
                Some(value) => batch.put(operation.key, value),
                None => batch.delete(operation.key),
            }
        }
        self.fenced = true;
        match database.write_opt(batch, &self.write_options) {
            Err(error) => {
                report("submitting metadata batch; outcome unknown, backend fenced", error);
                // Recover from the WAL, never replay a volatile copy of an uncertain batch.
                CommitResult::Unknown
            }
            Ok(()) => {
                self.fenced = false;
                CommitResult::Committed
            }
        }
    }

    fn settle(&mut self) -> bool {
        if !self.fenced { return true; }
        // The independent file lock stays held across the native close/reopen gap.
        drop(self.database.take());
        let database = match DB::open(&self.options, &self.path) {
            Ok(database) => database,
            Err(error) => {
                report("reopening RocksDB metadata", error);
                return false;
            }
        };
        self.database = Some(database);
        if !self.durability_barrier() { return false; }
        self.fenced = false;
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::FileExt;
    use std::sync::atomic::{AtomicU64, Ordering};

    static NEXT_DIRECTORY: AtomicU64 = AtomicU64::new(0);
    struct TestDirectory(PathBuf);
    impl TestDirectory {
        fn new() -> Self {
            loop {
                let path = std::env::temp_dir().join(format!("mako-rocks-metadata-{}-{}",
                    std::process::id(), NEXT_DIRECTORY.fetch_add(1, Ordering::Relaxed)));
                match fs::create_dir(&path) {
                    Ok(()) => return Self(path),
                    Err(error) if error.kind() == io::ErrorKind::AlreadyExists => continue,
                    Err(error) => panic!("creating RocksDB test directory: {error}"),
                }
            }
        }
        fn database(&self) -> PathBuf { self.0.join("owner-7").join("participant") }
    }
    impl Drop for TestDirectory {
        fn drop(&mut self) { let _ = fs::remove_dir_all(&self.0); }
    }
    fn put(key: &[u8], value: &[u8]) -> WriteOp {
        WriteOp { key: key.to_vec(), value: Some(value.to_vec()) }
    }
    fn expect_value(kv: &mut RocksKv, key: &[u8], expected: &[u8]) {
        match kv.read(key) {
            ReadResult::Found(value) => assert_eq!(value, expected),
            _ => panic!("expected a found metadata value"),
        }
    }

    #[test]
    fn strict_creation_recovery_and_writer_exclusion() {
        let temporary = TestDirectory::new();
        let path = temporary.database();
        assert!(matches!(RocksKv::open(&path, false), Err(Status::NotFound)));
        assert!(!path.parent().unwrap().exists());
        let kv = RocksKv::open(&path, true).unwrap();
        assert!(matches!(RocksKv::open(&path, true), Err(Status::Invalid)));
        assert!(matches!(RocksKv::open(&path, false), Err(Status::Busy)));
        drop(kv);
        let kv = RocksKv::open(&path, false).unwrap();
        drop(kv);
        fs::remove_file(path.join(WRITER_LOCK)).unwrap();
        assert!(matches!(RocksKv::open(&path, false), Err(Status::NotFound)));
        assert!(!path.join(WRITER_LOCK).exists());
        assert!(matches!(RocksKv::open(&path, true), Err(Status::Invalid)));
    }

    #[test]
    fn mixed_atomic_batch_retains_order_empty_values_and_deletes_on_reopen() {
        let temporary = TestDirectory::new();
        let path = temporary.database();
        let mut kv = RocksKv::open(&path, true).unwrap();
        assert_eq!(kv.submit(vec![put(b"removed", b"old")]), CommitResult::Committed);
        assert_eq!(kv.submit(vec![
            put(b"retained", b"value"),
            WriteOp { key: b"removed".to_vec(), value: None },
            put(b"repeated", b"first"),
            WriteOp { key: b"repeated".to_vec(), value: None },
            put(b"repeated", b"last"),
            put(b"", b""),
        ]), CommitResult::Committed);
        drop(kv);
        let mut kv = RocksKv::open(&path, false).unwrap();
        expect_value(&mut kv, b"retained", b"value");
        expect_value(&mut kv, b"repeated", b"last");
        expect_value(&mut kv, b"", b"");
        assert!(matches!(kv.read(b"removed"), ReadResult::Absent));
        assert!(matches!(kv.read(b"never-written"), ReadResult::Absent));
    }

    #[test]
    fn lifetime_lock_and_fence_survive_a_real_close_reopen_gap() {
        let temporary = TestDirectory::new();
        let path = temporary.database();
        let mut kv = RocksKv::open(&path, true).unwrap();
        assert_eq!(kv.submit(vec![put(b"retained", b"value")]), CommitResult::Committed);
        // Exercise the settlement close/reopen gap, not a simulated write result.
        kv.fenced = true;
        drop(kv.database.take());
        assert!(matches!(RocksKv::open(&path, false), Err(Status::Busy)));
        assert!(matches!(kv.read(b"retained"), ReadResult::Io));
        assert_eq!(kv.submit(vec![put(b"rejected", b"value")]), CommitResult::NotCommitted);
        assert!(kv.settle());
        expect_value(&mut kv, b"retained", b"value");
        assert!(matches!(kv.read(b"rejected"), ReadResult::Absent));

        kv.fenced = true;
        drop(kv.database.take());
        fs::remove_file(path.join("CURRENT")).unwrap();
        assert!(!kv.settle());
        assert!(matches!(kv.read(b"retained"), ReadResult::Io));
        assert!(matches!(RocksKv::open(&path, false), Err(Status::Busy)));
        assert!(!path.join("CURRENT").exists());
    }

    #[test]
    fn real_sst_corruption_is_io_not_absence() {
        let temporary = TestDirectory::new();
        let path = temporary.database();
        let mut kv = RocksKv::open(&path, true).unwrap();
        assert_eq!(kv.submit(vec![put(b"retained", b"checksum-protected-value")]), CommitResult::Committed);
        assert!(kv.durability_barrier());
        drop(kv);
        let mut kv = RocksKv::open(&path, false).unwrap();
        let mut corrupted = 0;
        for entry in fs::read_dir(&path).unwrap() {
            let path = entry.unwrap().path();
            if path.extension().is_some_and(|extension| extension == "sst") {
                let file = OpenOptions::new().read(true).write(true).open(path).unwrap();
                let mut first = [0];
                file.read_exact_at(&mut first, 0).unwrap();
                first[0] ^= 0x80;
                file.write_all_at(&first, 0).unwrap();
                file.sync_all().unwrap();
                corrupted += 1;
            }
        }
        assert!(corrupted > 0);
        assert!(matches!(kv.read(b"retained"), ReadResult::Io));
        assert!(matches!(kv.read(b"never-written"), ReadResult::Io));
    }
}
