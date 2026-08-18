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
use std::path::Path;
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
        rocksdb_writebatch_t,
        rocksdb_iterator_t,
    );

    extern "C" {
        pub fn rocksdb_options_create() -> *mut rocksdb_options_t;
        pub fn rocksdb_options_destroy(o: *mut rocksdb_options_t);
        pub fn rocksdb_options_set_create_if_missing(o: *mut rocksdb_options_t, v: c_uchar);

        pub fn rocksdb_readoptions_create() -> *mut rocksdb_readoptions_t;
        pub fn rocksdb_readoptions_destroy(o: *mut rocksdb_readoptions_t);

        pub fn rocksdb_writeoptions_create() -> *mut rocksdb_writeoptions_t;
        pub fn rocksdb_writeoptions_destroy(o: *mut rocksdb_writeoptions_t);
        pub fn rocksdb_writeoptions_set_sync(o: *mut rocksdb_writeoptions_t, v: c_uchar);
        pub fn rocksdb_writeoptions_disable_WAL(o: *mut rocksdb_writeoptions_t, v: c_int);

        pub fn rocksdb_open(
            o: *const rocksdb_options_t,
            name: *const c_char,
            err: *mut *mut c_char,
        ) -> *mut rocksdb_t;
        pub fn rocksdb_close(db: *mut rocksdb_t);
        pub fn rocksdb_flush(
            db: *mut rocksdb_t,
            o: *const rocksdb_options_t,
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
        pub fn rocksdb_iter_seek_to_first(it: *mut rocksdb_iterator_t);
        pub fn rocksdb_iter_valid(it: *const rocksdb_iterator_t) -> c_uchar;
        pub fn rocksdb_iter_next(it: *mut rocksdb_iterator_t);
        pub fn rocksdb_iter_key(
            it: *const rocksdb_iterator_t,
            klen: *mut usize,
        ) -> *const c_char;
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

/// How durable each writeback batch should be.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Durability {
    /// `fsync` every batch. The watermark then means "on the platter".
    Sync,
    /// WAL only, no fsync. The watermark means "handed to the OS" — a
    /// process crash is survived, a machine crash may not be.
    Wal,
    /// No WAL. Fastest and the weakest: only correct where the durable
    /// store is itself a cache of something else.
    None,
}

/// A RocksDB database, usable as the cache's system of record.
pub struct RocksBlobs {
    db: *mut sys::rocksdb_t,
    opts: *mut sys::rocksdb_options_t,
    read: *mut sys::rocksdb_readoptions_t,
    write: *mut sys::rocksdb_writeoptions_t,
}

// SAFETY: RocksDB's `rocksdb_t` is documented as safe for concurrent use
// from multiple threads, which is the entire basis of its API. The option
// objects are read-only after construction here — nothing mutates them
// after `open` returns.
unsafe impl Send for RocksBlobs {}
// SAFETY: as above.
unsafe impl Sync for RocksBlobs {}

impl RocksBlobs {
    /// Open (creating if needed) at `path`.
    pub fn open(path: &Path, durability: Durability) -> Result<Self, BlobError> {
        let cpath = CString::new(path.as_os_str().as_encoded_bytes())
            .map_err(|_| BlobError("database path contains a NUL byte".into()))?;

        // SAFETY: all of these are infallible constructors with no
        // preconditions; each is destroyed exactly once, in Drop or on
        // the early-return path below.
        unsafe {
            let opts = sys::rocksdb_options_create();
            sys::rocksdb_options_set_create_if_missing(opts, 1);

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
                db,
                opts,
                read: sys::rocksdb_readoptions_create(),
                write,
            })
        }
    }

    /// Force a memtable flush. Only meaningful for tests and shutdown.
    pub fn flush(&self) -> Result<(), BlobError> {
        let mut e = Err0::new();
        // SAFETY: `db` and `opts` are live for the life of `self`.
        unsafe { sys::rocksdb_flush(self.db, self.opts, e.as_mut()) };
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
        let mut e = Err0::new();
        // SAFETY: as above. A WriteBatch is applied atomically, which is
        // the all-or-nothing guarantee the cache relies on.
        unsafe { sys::rocksdb_write(self.db, self.write, batch, e.as_mut()) };
        // SAFETY: destroyed exactly once, after the write it describes.
        unsafe { sys::rocksdb_writebatch_destroy(batch) };
        e.check("rocksdb_write")
    }

    fn for_each_key(&self, f: &mut dyn FnMut(&[u8])) -> Result<(), BlobError> {
        // SAFETY: `db` and `read` are live; the iterator is destroyed on
        // every path below.
        let it = unsafe { sys::rocksdb_create_iterator(self.db, self.read) };
        // SAFETY: `it` is a valid iterator for the whole block.
        unsafe {
            sys::rocksdb_iter_seek_to_first(it);
            while sys::rocksdb_iter_valid(it) != 0 as c_uchar {
                let mut klen: usize = 0;
                let kp = sys::rocksdb_iter_key(it, &mut klen);
                // The key borrows iterator-owned memory that stays valid
                // only until the next `next`, so it is handed to `f`
                // inside the loop and never escapes.
                f(std::slice::from_raw_parts(kp as *const u8, klen));
                sys::rocksdb_iter_next(it);
            }
            let mut e = Err0::new();
            sys::rocksdb_iter_get_error(it, e.as_mut());
            sys::rocksdb_iter_destroy(it);
            e.check("rocksdb iteration")
        }
    }
}
