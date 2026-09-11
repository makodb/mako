//! [`Blobs`] over RocksDB, through its stable C API.
//!
//! The C API rather than a Rust crate on purpose. `rust-rocksdb` would
//! vendor and build its own RocksDB, which then coexists with the one
//! mako already links — two copies of the same library in one process,
//! with two sets of global state, is a class of bug worth avoiding
//! outright. `rocksdb/c.h` is the ABI the existing build already
//! provides.
//!
//! # Errors are strings that must be freed
//!
//! Every fallible `rocksdb_*` call takes a `char **errptr`. Non-null
//! means failure and the caller owns the string. Forgetting to free it
//! leaks on exactly the path where things are already going wrong, so
//! every call here goes through [`Err0`], which frees on drop.

use std::ffi::{c_char, c_uchar, CStr, CString};
use std::path::{Path, PathBuf};
use std::ptr;

use mrx_core::{BlobError, BlobOp, Blobs};

mod sys {
    #![allow(non_camel_case_types)]
    use std::ffi::{c_char, c_int, c_uchar};

    macro_rules! opaque {
        ($($n:ident),* $(,)?) => {$(
            #[repr(C)]
            pub struct $n {
                _private: [u8; 0],
            }
        )*};
    }
    opaque!(
        rocksdb_t,
        rocksdb_options_t,
        rocksdb_readoptions_t,
        rocksdb_writeoptions_t,
        rocksdb_flushoptions_t,
        rocksdb_writebatch_t,
        rocksdb_iterator_t,
    );

    extern "C" {
        pub fn rocksdb_options_create() -> *mut rocksdb_options_t;
        pub fn rocksdb_options_destroy(o: *mut rocksdb_options_t);
        pub fn rocksdb_options_set_create_if_missing(o: *mut rocksdb_options_t, v: c_uchar);
        pub fn rocksdb_options_set_wal_recovery_mode(o: *mut rocksdb_options_t, v: c_int);
        pub fn rocksdb_options_set_unordered_write(o: *mut rocksdb_options_t, v: c_uchar);

        pub fn rocksdb_readoptions_create() -> *mut rocksdb_readoptions_t;
        pub fn rocksdb_readoptions_destroy(o: *mut rocksdb_readoptions_t);

        pub fn rocksdb_writeoptions_create() -> *mut rocksdb_writeoptions_t;
        pub fn rocksdb_writeoptions_destroy(o: *mut rocksdb_writeoptions_t);
        pub fn rocksdb_writeoptions_set_sync(o: *mut rocksdb_writeoptions_t, v: c_uchar);
        pub fn rocksdb_writeoptions_disable_WAL(o: *mut rocksdb_writeoptions_t, v: c_int);

        pub fn rocksdb_flushoptions_create() -> *mut rocksdb_flushoptions_t;
        pub fn rocksdb_flushoptions_destroy(o: *mut rocksdb_flushoptions_t);
        pub fn rocksdb_flushoptions_set_wait(o: *mut rocksdb_flushoptions_t, v: c_uchar);

        pub fn rocksdb_open(
            o: *const rocksdb_options_t,
            name: *const c_char,
            err: *mut *mut c_char,
        ) -> *mut rocksdb_t;
        pub fn rocksdb_close(db: *mut rocksdb_t);
        pub fn rocksdb_flush(
            db: *mut rocksdb_t,
            o: *const rocksdb_flushoptions_t,
            err: *mut *mut c_char,
        );

        pub fn rocksdb_get(
            db: *mut rocksdb_t,
            o: *const rocksdb_readoptions_t,
            key: *const c_char,
            keylen: usize,
            vallen: *mut usize,
            err: *mut *mut c_char,
        ) -> *mut c_char;

        pub fn rocksdb_write(
            db: *mut rocksdb_t,
            o: *const rocksdb_writeoptions_t,
            batch: *mut rocksdb_writebatch_t,
            err: *mut *mut c_char,
        );

        pub fn rocksdb_writebatch_create() -> *mut rocksdb_writebatch_t;
        pub fn rocksdb_writebatch_destroy(b: *mut rocksdb_writebatch_t);
        pub fn rocksdb_writebatch_put(
            b: *mut rocksdb_writebatch_t,
            key: *const c_char,
            klen: usize,
            val: *const c_char,
            vlen: usize,
        );
        pub fn rocksdb_writebatch_delete(
            b: *mut rocksdb_writebatch_t,
            key: *const c_char,
            klen: usize,
        );

        pub fn rocksdb_create_iterator(
            db: *mut rocksdb_t,
            o: *const rocksdb_readoptions_t,
        ) -> *mut rocksdb_iterator_t;
        pub fn rocksdb_iter_destroy(it: *mut rocksdb_iterator_t);
        pub fn rocksdb_iter_seek(it: *mut rocksdb_iterator_t, key: *const c_char, klen: usize);
        pub fn rocksdb_iter_valid(it: *const rocksdb_iterator_t) -> c_uchar;
        pub fn rocksdb_iter_next(it: *mut rocksdb_iterator_t);
        pub fn rocksdb_iter_key(it: *const rocksdb_iterator_t, klen: *mut usize) -> *const c_char;
        pub fn rocksdb_iter_value(it: *const rocksdb_iterator_t, vlen: *mut usize)
            -> *const c_char;
        pub fn rocksdb_iter_get_error(it: *const rocksdb_iterator_t, err: *mut *mut c_char);

        pub fn rocksdb_free(p: *mut std::ffi::c_void);
    }
}

/// An owned RocksDB error string.
///
/// Exists so the free cannot be forgotten on the error path.
struct Err0(*mut c_char);

impl Err0 {
    fn new() -> Self {
        Self(ptr::null_mut())
    }

    fn as_mut(&mut self) -> *mut *mut c_char {
        &mut self.0
    }

    /// Convert a non-null error into `Err`, freeing the string.
    fn check(&mut self, what: &str) -> Result<(), BlobError> {
        if self.0.is_null() {
            return Ok(());
        }
        // SAFETY: RocksDB guarantees a NUL-terminated string here.
        let msg = unsafe { CStr::from_ptr(self.0) }
            .to_string_lossy()
            .into_owned();
        // SAFETY: the string was allocated by RocksDB's allocator.
        unsafe { sys::rocksdb_free(self.0.cast()) };
        self.0 = ptr::null_mut();
        Err(BlobError(format!("{what}: {msg}")))
    }
}

impl Drop for Err0 {
    fn drop(&mut self) {
        if !self.0.is_null() {
            // SAFETY: as above.
            unsafe { sys::rocksdb_free(self.0.cast()) };
        }
    }
}

/// Keeps iterator-owned key/value buffers and native resources scoped to the
/// database borrow, including callback errors and unwinding panics.
struct Entries<'a> {
    raw: *mut sys::rocksdb_iterator_t,
    _database: &'a RocksBlobs,
}

impl Drop for Entries<'_> {
    fn drop(&mut self) {
        // SAFETY: created once by for_each_entry, never transferred elsewhere.
        unsafe { sys::rocksdb_iter_destroy(self.raw) };
    }
}

/// How durable each writeback batch should be.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Durability {
    /// Request an `fsync` for every batch.
    Sync,
    /// WAL enabled with `sync=false`; disk persistence is asynchronous.
    Wal,
    /// No WAL. Fastest and the weakest: only correct where the durable
    /// store is itself a cache of something else.
    None,
}

/// A synchronous observation point around one RocksDB batch write.
///
/// Available only with the `test-hooks` feature. These points bracket the
/// opaque `rocksdb_write` C call; they do not run inside RocksDB's WAL append,
/// sync, or memtable installation. The linked C API exposes no such internal
/// callback seam.
#[cfg(feature = "test-hooks")]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WriteBatchHookPoint {
    /// Every operation has been copied into the native `WriteBatch`.
    BatchConstructed,
    /// Immediately before entering `rocksdb_write`.
    BeforeWrite,
    /// Immediately after `rocksdb_write` returns, before error handling.
    AfterWrite,
}

/// A RocksDB database, usable as the cache's system of record.
pub struct RocksBlobs {
    path: PathBuf,
    db: *mut sys::rocksdb_t,
    opts: *mut sys::rocksdb_options_t,
    read: *mut sys::rocksdb_readoptions_t,
    write: *mut sys::rocksdb_writeoptions_t,
    #[cfg(feature = "test-hooks")]
    write_batch_observer:
        Option<std::sync::Arc<dyn Fn(WriteBatchHookPoint) + Send + Sync + 'static>>,
}

// SAFETY: RocksDB's `rocksdb_t` is documented as safe for concurrent use
// from multiple threads, which is the entire basis of its API. The option
// objects are read-only after construction here — nothing mutates them
// after `open` returns.
unsafe impl Send for RocksBlobs {}
// SAFETY: as above.
unsafe impl Sync for RocksBlobs {}

impl RocksBlobs {
    /// Open (creating if needed) at `path` with ordered writes and point-in-time
    /// WAL recovery. Corruption stops WAL recovery at a consistent point rather
    /// than skipping arbitrary records that may contain checkpoint prerequisites.
    pub fn open(path: &Path, durability: Durability) -> Result<Self, BlobError> {
        // Keep file observations tied to the opened directory if the process
        // later changes its working directory. RocksDB stores WAL and SST files
        // in this directory with the adapter's fixed options.
        let path = std::path::absolute(path)
            .map_err(|error| BlobError(format!("database absolute path: {error}")))?;
        let cpath = CString::new(path.as_os_str().as_encoded_bytes())
            .map_err(|_| BlobError("database path contains a NUL byte".into()))?;

        // SAFETY: all of these are infallible constructors with no
        // preconditions; each is destroyed exactly once, in Drop or on
        // the early-return path below.
        unsafe {
            let opts = sys::rocksdb_options_create();
            sys::rocksdb_options_set_create_if_missing(opts, 1);
            // rocksdb/c.h defines rocksdb_point_in_time_recovery as 2. Explicitly
            // select the coherent-history contract required by log reclamation,
            // rather than inheriting a library default or salvaging arbitrary
            // later records after a damaged prerequisite batch.
            sys::rocksdb_options_set_wal_recovery_mode(opts, 2);
            sys::rocksdb_options_set_unordered_write(opts, 0);

            // DELIBERATELY NOT `increase_parallelism` OR
            // `optimize_level_style_compaction`.
            //
            // Both look like free wins and neither is, here. RocksDB sits
            // BEHIND a cache: it takes coalesced batches from one flusher
            // thread, not the application's write rate, so the thing to
            // optimise is how little CPU it steals from the foreground —
            // not how fast it can ingest.
            //
            // `increase_parallelism(available_parallelism())` spawned 64
            // background threads on this machine, to compete with the
            // application's writers for the same cores, and
            // `optimize_level_style_compaction(512 MiB)` bought more
            // compaction work on top. Measured against the C++ cache,
            // which sets neither, this made the Rust arm look far slower
            // than it is — the benchmark was comparing two different
            // RocksDB configurations and calling the difference a
            // language gap.
            //
            // Callers who genuinely want a heavier RocksDB should say so
            // explicitly rather than inherit it from a default.

            let write = sys::rocksdb_writeoptions_create();
            match durability {
                Durability::Sync => sys::rocksdb_writeoptions_set_sync(write, 1),
                Durability::Wal => sys::rocksdb_writeoptions_set_sync(write, 0),
                Durability::None => sys::rocksdb_writeoptions_disable_WAL(write, 1),
            }

            let mut e = Err0::new();
            let db = sys::rocksdb_open(opts, cpath.as_ptr(), e.as_mut());
            if let Err(err) = e.check("rocksdb_open") {
                sys::rocksdb_writeoptions_destroy(write);
                sys::rocksdb_options_destroy(opts);
                return Err(err);
            }
            if db.is_null() {
                sys::rocksdb_writeoptions_destroy(write);
                sys::rocksdb_options_destroy(opts);
                return Err(BlobError("rocksdb_open returned null".into()));
            }

            Ok(Self {
                path,
                db,
                opts,
                read: sys::rocksdb_readoptions_create(),
                write,
                #[cfg(feature = "test-hooks")]
                write_batch_observer: None,
            })
        }
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

    /// Force a memtable flush. Only meaningful for tests and shutdown.
    pub fn flush(&self) -> Result<(), BlobError> {
        // RocksDB's flush entry point takes a dedicated flush-options
        // handle, not the database options used at open. Waiting makes this
        // method's shutdown/testing contract literal: the memtable flush has
        // completed before it returns.
        // SAFETY: created here and destroyed exactly once below.
        let flush = unsafe { sys::rocksdb_flushoptions_create() };
        // SAFETY: `flush` is live for both calls and `db` remains live for
        // the duration of `self`.
        unsafe { sys::rocksdb_flushoptions_set_wait(flush, 1) };
        let mut e = Err0::new();
        // SAFETY: `db` and `flush` are live for this call.
        unsafe { sys::rocksdb_flush(self.db, flush, e.as_mut()) };
        // SAFETY: the call above has returned and this is the unique handle.
        unsafe { sys::rocksdb_flushoptions_destroy(flush) };
        e.check("rocksdb_flush")
    }
}

impl Drop for RocksBlobs {
    fn drop(&mut self) {
        // SAFETY: each handle was created once and is destroyed once.
        // Order matters: the DB must close before its options.
        unsafe {
            sys::rocksdb_readoptions_destroy(self.read);
            sys::rocksdb_writeoptions_destroy(self.write);
            sys::rocksdb_close(self.db);
            sys::rocksdb_options_destroy(self.opts);
        }
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
        let mut len: usize = 0;
        let mut e = Err0::new();
        // SAFETY: `key` is a valid slice; RocksDB copies it. The returned
        // pointer is owned by us and freed below.
        let p = unsafe {
            sys::rocksdb_get(
                self.db,
                self.read,
                key.as_ptr() as *const c_char,
                key.len(),
                &mut len,
                e.as_mut(),
            )
        };
        e.check("rocksdb_get")?;
        if p.is_null() {
            return Ok(None);
        }
        // SAFETY: `p` points at `len` initialised bytes allocated by
        // RocksDB, which we copy and then free.
        let v = unsafe { std::slice::from_raw_parts(p as *const u8, len) }.to_vec();
        // SAFETY: allocated by RocksDB's allocator.
        unsafe { sys::rocksdb_free(p.cast()) };
        Ok(Some(v))
    }

    fn write_batch(&self, ops: &[BlobOp<'_>]) -> Result<(), BlobError> {
        if ops.is_empty() {
            return Ok(());
        }
        // SAFETY: created here, destroyed on every path below.
        let batch = unsafe { sys::rocksdb_writebatch_create() };
        // SAFETY: `batch` is live; every slice is valid and is copied by
        // RocksDB before these calls return.
        unsafe {
            for op in ops {
                match op {
                    BlobOp::Put { key, val } => sys::rocksdb_writebatch_put(
                        batch,
                        key.as_ptr() as *const c_char,
                        key.len(),
                        val.as_ptr() as *const c_char,
                        val.len(),
                    ),
                    BlobOp::Delete { key } => sys::rocksdb_writebatch_delete(
                        batch,
                        key.as_ptr() as *const c_char,
                        key.len(),
                    ),
                }
            }
        }
        #[cfg(feature = "test-hooks")]
        self.observe_write_batch(WriteBatchHookPoint::BatchConstructed);

        let mut e = Err0::new();
        // SAFETY: as above. A WriteBatch is applied atomically, which is
        // the all-or-nothing guarantee the cache relies on.
        #[cfg(feature = "test-hooks")]
        self.observe_write_batch(WriteBatchHookPoint::BeforeWrite);
        unsafe { sys::rocksdb_write(self.db, self.write, batch, e.as_mut()) };
        #[cfg(feature = "test-hooks")]
        self.observe_write_batch(WriteBatchHookPoint::AfterWrite);
        // SAFETY: destroyed exactly once, after the write it describes.
        unsafe { sys::rocksdb_writebatch_destroy(batch) };
        e.check("rocksdb_write")
    }

    fn for_each_key(&self, f: &mut dyn FnMut(&[u8])) -> Result<(), BlobError> {
        self.for_each_entry(&mut |key, _| {
            f(key);
            Ok(())
        })
    }

    fn for_each_entry(
        &self,
        f: &mut dyn FnMut(&[u8], &[u8]) -> Result<(), BlobError>,
    ) -> Result<(), BlobError> {
        self.for_each_entry_from(b"", usize::MAX, &mut |key, value| {
            f(key, value)?;
            Ok(true)
        })
    }

    fn for_each_entry_from(
        &self,
        start: &[u8],
        max_entries: usize,
        visitor: &mut dyn FnMut(&[u8], &[u8]) -> Result<bool, BlobError>,
    ) -> Result<(), BlobError> {
        if max_entries == 0 {
            return Ok(());
        }
        // SAFETY: both handles remain live through the database borrow.
        let raw = unsafe { sys::rocksdb_create_iterator(self.db, self.read) };
        if raw.is_null() {
            return Err(BlobError("rocksdb_create_iterator returned null".into()));
        }
        let iterator = Entries {
            raw,
            _database: self,
        };
        // SAFETY: the iterator remains live for the block and Drop releases it
        // on normal completion, errors, and callback panics. Buffers belong to
        // the iterator and remain valid until next/seek/destruction. The callback
        // borrows them only for its call; it cannot retain a borrowed reference.
        unsafe {
            sys::rocksdb_iter_seek(iterator.raw, start.as_ptr().cast::<c_char>(), start.len());
            let mut visited = 0;
            while visited < max_entries && sys::rocksdb_iter_valid(iterator.raw) != 0 as c_uchar {
                let mut klen: usize = 0;
                let mut vlen: usize = 0;
                let kp = sys::rocksdb_iter_key(iterator.raw, &mut klen);
                let vp = sys::rocksdb_iter_value(iterator.raw, &mut vlen);
                if (kp.is_null() && klen != 0)
                    || (vp.is_null() && vlen != 0)
                    || klen > isize::MAX as usize
                    || vlen > isize::MAX as usize
                {
                    return Err(BlobError(
                        "rocksdb iterator returned an invalid buffer".into(),
                    ));
                }
                // Empty slices do not require a native pointer. This avoids
                // constructing a Rust slice from a possible null empty buffer.
                let key = if klen == 0 {
                    &[]
                } else {
                    std::slice::from_raw_parts(kp.cast::<u8>(), klen)
                };
                let value = if vlen == 0 {
                    &[]
                } else {
                    std::slice::from_raw_parts(vp.cast::<u8>(), vlen)
                };
                visited += 1;
                if !visitor(key, value)? || visited == max_entries {
                    break;
                }
                sys::rocksdb_iter_next(iterator.raw);
            }
            let mut e = Err0::new();
            sys::rocksdb_iter_get_error(iterator.raw, e.as_mut());
            e.check("rocksdb iteration")
        }
    }
}
