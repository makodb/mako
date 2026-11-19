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
#![allow(unsafe_code)]

use crate::durable_kv::{CommitResult, DurableKv, ReadResult, WriteOp};
use crate::types::Status;
use std::cell::Cell;
use std::ffi::{c_char, c_int, c_uchar, c_void, CStr, CString};
use std::fs::{self, File, OpenOptions, TryLockError};
use std::io::{self, Write};
use std::marker::PhantomData;
use std::os::unix::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::ptr::NonNull;

const WRITER_LOCK: &str = "MAKO_METADATA_WRITER_LOCK";

// Signatures and recovery-mode discriminant follow the installed rocksdb/c.h.
// Opaque native objects never cross this module's public API.
mod ffi {
    use super::{c_char, c_int, c_uchar, c_void};

    macro_rules! opaque {
        ($($name:ident),+ $(,)?) => { $(
            #[repr(C)]
            pub struct $name { _opaque: [u8; 0] }
        )+ };
    }
    opaque!(Db, Options, ReadOptions, WriteOptions, FlushOptions, Batch, Pinned);

    #[link(name = "rocksdb")]
    unsafe extern "C" {
        pub fn rocksdb_options_create() -> *mut Options;
        pub fn rocksdb_options_destroy(options: *mut Options);
        pub fn rocksdb_options_set_create_if_missing(options: *mut Options, value: c_uchar);
        pub fn rocksdb_options_set_error_if_exists(options: *mut Options, value: c_uchar);
        pub fn rocksdb_options_set_paranoid_checks(options: *mut Options, value: c_uchar);
        pub fn rocksdb_options_set_wal_recovery_mode(options: *mut Options, value: c_int);
        pub fn rocksdb_get_options_from_string(base: *const Options, text: *const c_char,
            options: *mut Options, error: *mut *mut c_char);
        pub fn rocksdb_open(options: *const Options, path: *const c_char, error: *mut *mut c_char) -> *mut Db;
        pub fn rocksdb_close(db: *mut Db);
        pub fn rocksdb_readoptions_create() -> *mut ReadOptions;
        pub fn rocksdb_readoptions_destroy(options: *mut ReadOptions);
        pub fn rocksdb_readoptions_set_verify_checksums(options: *mut ReadOptions, value: c_uchar);
        pub fn rocksdb_writeoptions_create() -> *mut WriteOptions;
        pub fn rocksdb_writeoptions_destroy(options: *mut WriteOptions);
        pub fn rocksdb_writeoptions_set_sync(options: *mut WriteOptions, value: c_uchar);
        pub fn rocksdb_writeoptions_disable_WAL(options: *mut WriteOptions, value: c_int);
        pub fn rocksdb_flushoptions_create() -> *mut FlushOptions;
        pub fn rocksdb_flushoptions_destroy(options: *mut FlushOptions);
        pub fn rocksdb_flushoptions_set_wait(options: *mut FlushOptions, value: c_uchar);
        pub fn rocksdb_flush(db: *mut Db, options: *const FlushOptions, error: *mut *mut c_char);
        pub fn rocksdb_flush_wal(db: *mut Db, sync: c_uchar, error: *mut *mut c_char);
        pub fn rocksdb_writebatch_create() -> *mut Batch;
        pub fn rocksdb_writebatch_destroy(batch: *mut Batch);
        pub fn rocksdb_writebatch_put(batch: *mut Batch, key: *const c_char, key_len: usize,
            value: *const c_char, value_len: usize);
        pub fn rocksdb_writebatch_delete(batch: *mut Batch, key: *const c_char, key_len: usize);
        pub fn rocksdb_write(db: *mut Db, options: *const WriteOptions, batch: *mut Batch,
            error: *mut *mut c_char);
        pub fn rocksdb_get_pinned(db: *mut Db, options: *const ReadOptions, key: *const c_char,
            key_len: usize, error: *mut *mut c_char) -> *mut Pinned;
        pub fn rocksdb_pinnableslice_value(value: *const Pinned, len: *mut usize) -> *const c_char;
        pub fn rocksdb_pinnableslice_destroy(value: *mut Pinned);
        pub fn rocksdb_free(pointer: *mut c_void);
    }
}

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

struct NativeError(*mut c_char);
impl NativeError {
    fn new() -> Self { Self(std::ptr::null_mut()) }
    fn output(&mut self) -> *mut *mut c_char { &mut self.0 }
    fn failed(&self, operation: &str) -> bool {
        if self.0.is_null() { return false; }
        // SAFETY: RocksDB error outputs are owned, NUL-terminated allocations.
        let reason = unsafe { CStr::from_ptr(self.0) };
        report(operation, reason.to_string_lossy());
        true
    }
}
impl Drop for NativeError {
    fn drop(&mut self) {
        if !self.0.is_null() {
            // SAFETY: This wrapper is the sole owner of the RocksDB allocation.
            unsafe { ffi::rocksdb_free(self.0.cast()) };
        }
    }
}

// Each native allocation has one statically dispatched destructor, including on
// early return. The wrappers are private and cannot export or clone ownership.
macro_rules! owned_handle {
    ($name:ident, $raw:ident, $create:ident, $destroy:ident) => {
        struct $name(NonNull<ffi::$raw>);
        impl $name {
            fn new() -> Result<Self, Status> {
                // SAFETY: Native factory takes no borrowed inputs.
                let pointer = unsafe { ffi::$create() };
                NonNull::new(pointer).map(Self).ok_or_else(|| {
                    report(stringify!($create), "native allocation failed");
                    Status::Exhausted
                })
            }
        }
        impl Drop for $name {
            fn drop(&mut self) {
                // SAFETY: Non-null factory result is destroyed exactly once.
                unsafe { ffi::$destroy(self.0.as_ptr()) };
            }
        }
    };
}
owned_handle!(Options, Options, rocksdb_options_create, rocksdb_options_destroy);
owned_handle!(ReadOptions, ReadOptions, rocksdb_readoptions_create, rocksdb_readoptions_destroy);
owned_handle!(WriteOptions, WriteOptions, rocksdb_writeoptions_create, rocksdb_writeoptions_destroy);
owned_handle!(FlushOptions, FlushOptions, rocksdb_flushoptions_create, rocksdb_flushoptions_destroy);
owned_handle!(WriteBatch, Batch, rocksdb_writebatch_create, rocksdb_writebatch_destroy);

struct Database(NonNull<ffi::Db>);
impl Database {
    fn open(options: &mut Options, path: &CStr, initialize: bool) -> Result<Self, Status> {
        let mut error = NativeError::new();
        // SAFETY: Options are exclusively owned; path and error output live for
        // this call. The DB owns its own option state after rocksdb_open returns.
        let pointer = unsafe {
            ffi::rocksdb_options_set_create_if_missing(options.0.as_ptr(), u8::from(initialize));
            ffi::rocksdb_options_set_error_if_exists(options.0.as_ptr(), u8::from(initialize));
            ffi::rocksdb_open(options.0.as_ptr(), path.as_ptr(), error.output())
        };
        let database = NonNull::new(pointer).map(Self);
        if error.failed("opening RocksDB metadata") { return Err(Status::Io); }
        database.ok_or_else(|| {
            report("opening RocksDB metadata", "native open returned no handle and no error");
            Status::Io
        })
    }
}
impl Drop for Database {
    fn drop(&mut self) {
        // SAFETY: No pinned read or native operation outlives its synchronous
        // call. Close joins the old DB's background work before returning.
        unsafe { ffi::rocksdb_close(self.0.as_ptr()) };
    }
}

struct PinnedValue(NonNull<ffi::Pinned>);
impl Drop for PinnedValue {
    fn drop(&mut self) {
        // SAFETY: Pinned result is uniquely owned and destroyed before its DB.
        unsafe { ffi::rocksdb_pinnableslice_destroy(self.0.as_ptr()) };
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

/// Owned, non-Clone metadata backend. All operations are serialized by `&mut`.
/// A write error fences reads and later submissions until a real recovery barrier
/// succeeds. This type deliberately does not implement Sync or expose its DB.
pub struct RocksKv {
    // Rust drops fields in declaration order: DB first, lifetime lock last.
    database: Option<Database>,
    options: Options,
    read_options: ReadOptions,
    write_options: WriteOptions,
    flush_options: FlushOptions,
    path: CString,
    directory: File,
    fenced: bool,
    _not_sync: PhantomData<Cell<()>>,
    _writer_lock: File,
}

// SAFETY: Native handles have no thread affinity and are exclusively owned.
// Moving the owner cannot race an operation: all accesses require &mut Self,
// no handles escape, and the independent File lock moves with this owner.
unsafe impl Send for RocksKv {}

impl RocksKv {
    /// Create only with explicit initialization of an absent namespace, or open
    /// an already retained store without creating missing directories/lock/DB.
    /// Busy identifies a live writer, NotFound missing filesystem state, Invalid
    /// invalid paths or attempted reinitialization, and Io native/durability errors.
    pub fn open(path: &Path, initialize: bool) -> Result<Self, Status> {
        let (root, writer_lock) = namespace_directory(path, initialize)?;
        let path = CString::new(root.as_os_str().as_bytes()).map_err(|_| Status::Invalid)?;
        let directory = File::open(&root)
            .map_err(|error| filesystem_error("opening metadata directory for sync", error))?;
        let mut options = {
            // RocksDB 8.9 exposes this DB option through its supported parser,
            // not a dedicated C setter. Reject unsupported options, never weaken
            // recovery silently to accommodate a different library version.
            let base = Options::new()?;
            let options = Options::new()?;
            let mut error = NativeError::new();
            // SAFETY: Distinct owned option objects and a static NUL-terminated
            // input remain live through the synchronous options parser.
            unsafe { ffi::rocksdb_get_options_from_string(base.0.as_ptr(),
                c"track_and_verify_wals_in_manifest=true".as_ptr(), options.0.as_ptr(), error.output()) };
            if error.failed("configuring metadata WAL verification") { return Err(Status::Io); }
            options
        };
        let read_options = ReadOptions::new()?;
        let write_options = WriteOptions::new()?;
        let flush_options = FlushOptions::new()?;
        // SAFETY: All option handles are uniquely owned, live factory results.
        unsafe {
            ffi::rocksdb_options_set_paranoid_checks(options.0.as_ptr(), 1);
            // rocksdb_absolute_consistency_recovery, not point-in-time salvage.
            ffi::rocksdb_options_set_wal_recovery_mode(options.0.as_ptr(), 1);
            ffi::rocksdb_readoptions_set_verify_checksums(read_options.0.as_ptr(), 1);
            ffi::rocksdb_writeoptions_set_sync(write_options.0.as_ptr(), 1);
            ffi::rocksdb_writeoptions_disable_WAL(write_options.0.as_ptr(), 0);
            ffi::rocksdb_flushoptions_set_wait(flush_options.0.as_ptr(), 1);
        }
        let database = Database::open(&mut options, &path, initialize)?;
        let mut kv = Self {
            database: Some(database), options, read_options, write_options, flush_options,
            path, directory, fenced: true, _not_sync: PhantomData, _writer_lock: writer_lock,
        };
        // Opening a retained WAL may resolve previously accepted work. Establish
        // durability before exposing any successful read, not only on settle().
        if !kv.durability_barrier() { return Err(Status::Io); }
        if initialize { sync_parents(&root)?; }
        kv.fenced = false;
        Ok(kv)
    }

    fn durability_barrier(&mut self) -> bool {
        let Some(database) = &self.database else { return false; };
        let mut wal_error = NativeError::new();
        // SAFETY: DB is live and exclusively borrowed; sync=1 requests a real
        // WAL fsync, not a user-space buffer flush or an empty success surrogate.
        unsafe { ffi::rocksdb_flush_wal(database.0.as_ptr(), 1, wal_error.output()) };
        if wal_error.failed("syncing metadata WAL") { return false; }
        let mut flush_error = NativeError::new();
        // SAFETY: Blocking flush uses owned options with wait=1. This persists
        // recovered memtables even when reopening rotated the current WAL.
        unsafe { ffi::rocksdb_flush(database.0.as_ptr(), self.flush_options.0.as_ptr(), flush_error.output()) };
        if flush_error.failed("flushing recovered metadata") { return false; }
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
        let mut error = NativeError::new();
        // SAFETY: Borrowed key bytes remain live for the synchronous call. The
        // pinned result is owned locally and cannot outlive this DB borrow.
        let pointer = unsafe {
            ffi::rocksdb_get_pinned(database.0.as_ptr(), self.read_options.0.as_ptr(),
                key.as_ptr().cast(), key.len(), error.output())
        };
        let pinned = NonNull::new(pointer).map(PinnedValue);
        if error.failed("reading metadata") {
            self.fenced = true;
            return ReadResult::Io;
        }
        // Only a successful native NotFound produces a null pinned handle.
        let Some(pinned) = pinned else { return ReadResult::Absent; };
        let mut length = 0;
        // SAFETY: The pin keeps the returned bytes alive until its destructor.
        let bytes = unsafe { ffi::rocksdb_pinnableslice_value(pinned.0.as_ptr(), &mut length) };
        if length == 0 { return ReadResult::Found(Vec::new()); }
        if bytes.is_null() || length > isize::MAX as usize {
            report("reading metadata", "native pinned value has an invalid span");
            self.fenced = true;
            return ReadResult::Io;
        }
        // SAFETY: RocksDB returned this initialized span, protected by the pin.
        // The result contract owns a Vec, so exactly one copy is necessary.
        ReadResult::Found(unsafe { std::slice::from_raw_parts(bytes.cast::<u8>(), length) }.to_vec())
    }

    fn submit(&mut self, operations: Vec<WriteOp>) -> CommitResult {
        if self.fenced { return CommitResult::NotCommitted; }
        let Some(database) = &self.database else { return CommitResult::NotCommitted; };
        let batch = match WriteBatch::new() {
            Ok(batch) => batch,
            Err(_) => return CommitResult::NotCommitted, // No submission occurred.
        };
        for operation in &operations {
            // SAFETY: Native WriteBatch copies borrowed spans before returning.
            // Preserve order, including repeated keys, puts and tombstones.
            unsafe {
                match &operation.value {
                    Some(value) => ffi::rocksdb_writebatch_put(batch.0.as_ptr(),
                        operation.key.as_ptr().cast(), operation.key.len(), value.as_ptr().cast(), value.len()),
                    None => ffi::rocksdb_writebatch_delete(batch.0.as_ptr(),
                        operation.key.as_ptr().cast(), operation.key.len()),
                }
            }
        }
        let mut error = NativeError::new();
        self.fenced = true;
        // SAFETY: One owned batch, one live DB, sync=true and WAL enabled. No
        // concurrent owner can submit or observe an intermediate batch state.
        unsafe { ffi::rocksdb_write(database.0.as_ptr(), self.write_options.0.as_ptr(), batch.0.as_ptr(), error.output()) };
        if error.failed("submitting metadata batch; outcome unknown, backend fenced") {
            // An arbitrary error cannot establish non-commit. The WAL, not a
            // volatile copy of this batch, is authoritative during settlement.
            CommitResult::Unknown
        } else {
            self.fenced = false;
            CommitResult::Committed
        }
    }

    fn settle(&mut self) -> bool {
        if !self.fenced { return true; }
        // The application lock is a separate File field and stays locked while
        // RocksDB releases/reacquires its own LOCK. Close finishes old work;
        // strict recovery either obtains a whole-batch outcome or refuses open.
        drop(self.database.take());
        let database = match Database::open(&mut self.options, &self.path, false) {
            Ok(database) => database,
            Err(_) => return false,
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
