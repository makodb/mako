//! The RocksDB base (design §5; plan P7), through `rocksdb/c.h`, which Mako
//! already links. One instance per server, one column family. Every batch
//! is written with RocksDB's own WAL off -- legal exactly because the Raft
//! WAL is the log above it -- so a batch is durable only after a waiting
//! flush, and a crash leaves the base as of a whole batch at or after the
//! last one. Small write buffers: several servers may share a process.

use std::ffi::{c_char, c_int, c_uchar, c_void, CStr, CString};
use std::io;
use std::path::Path;

use crate::base::{Base, Op};

#[allow(non_camel_case_types)]
type rocksdb_t = c_void;
#[allow(non_camel_case_types)]
type opts_t = c_void;

extern "C" {
    fn rocksdb_options_create() -> *mut opts_t;
    fn rocksdb_options_destroy(o: *mut opts_t);
    fn rocksdb_options_set_create_if_missing(o: *mut opts_t, v: c_uchar);
    fn rocksdb_options_set_error_if_exists(o: *mut opts_t, v: c_uchar);
    fn rocksdb_options_set_write_buffer_size(o: *mut opts_t, s: usize);
    fn rocksdb_options_set_max_write_buffer_number(o: *mut opts_t, n: c_int);
    fn rocksdb_open(o: *const opts_t, name: *const c_char, err: *mut *mut c_char) -> *mut rocksdb_t;
    fn rocksdb_close(db: *mut rocksdb_t);
    fn rocksdb_free(p: *mut c_void);

    fn rocksdb_writebatch_create() -> *mut c_void;
    fn rocksdb_writebatch_destroy(b: *mut c_void);
    fn rocksdb_writebatch_put(b: *mut c_void, k: *const c_char, kl: usize, v: *const c_char, vl: usize);
    fn rocksdb_writebatch_delete_range(b: *mut c_void, s: *const c_char, sl: usize, e: *const c_char, el: usize);
    fn rocksdb_writeoptions_create() -> *mut c_void;
    fn rocksdb_writeoptions_destroy(o: *mut c_void);
    fn rocksdb_writeoptions_disable_WAL(o: *mut c_void, disable: c_int);
    fn rocksdb_write(db: *mut rocksdb_t, o: *const c_void, b: *mut c_void, err: *mut *mut c_char);

    fn rocksdb_flushoptions_create() -> *mut c_void;
    fn rocksdb_flushoptions_destroy(o: *mut c_void);
    fn rocksdb_flushoptions_set_wait(o: *mut c_void, v: c_uchar);
    fn rocksdb_flush(db: *mut rocksdb_t, o: *const c_void, err: *mut *mut c_char);

    fn rocksdb_readoptions_create() -> *mut c_void;
    fn rocksdb_readoptions_destroy(o: *mut c_void);
    fn rocksdb_get(db: *mut rocksdb_t, o: *const c_void, k: *const c_char, kl: usize, vl: *mut usize,
                   err: *mut *mut c_char) -> *mut c_char;
    fn rocksdb_create_iterator(db: *mut rocksdb_t, o: *const c_void) -> *mut c_void;
    fn rocksdb_iter_destroy(it: *mut c_void);
    fn rocksdb_iter_seek(it: *mut c_void, k: *const c_char, kl: usize);
    fn rocksdb_iter_valid(it: *const c_void) -> c_uchar;
    fn rocksdb_iter_next(it: *mut c_void);
    fn rocksdb_iter_key(it: *const c_void, kl: *mut usize) -> *const c_char;
    fn rocksdb_iter_value(it: *const c_void, vl: *mut usize) -> *const c_char;
}

fn check(err: *mut c_char) -> io::Result<()> {
    if err.is_null() {
        return Ok(());
    }
    // SAFETY: RocksDB hands back a malloc'd NUL-terminated message.
    let msg = unsafe { CStr::from_ptr(err) }.to_string_lossy().into_owned();
    unsafe { rocksdb_free(err as *mut c_void) };
    Err(io::Error::other(format!("rocksdb: {msg}")))
}

/// A RocksDB base. Not `Sync`; one owner (the applier, or recovery).
pub struct RocksBase {
    db: *mut rocksdb_t,
    wopts: *mut c_void,
    ropts: *mut c_void,
}

// SAFETY: a RocksDB handle may move between threads; this type has one owner.
unsafe impl Send for RocksBase {}

impl RocksBase {
    /// Opens the base at `path`. `create`: make a new one (only inside a
    /// store being created); otherwise a missing or damaged base is an error,
    /// never repaired or recreated (plan P7).
    pub fn open(path: &Path, create: bool) -> io::Result<RocksBase> {
        let name = CString::new(path.to_string_lossy().as_bytes()).map_err(io::Error::other)?;
        // SAFETY: plain C API calls on handles created here.
        unsafe {
            let o = rocksdb_options_create();
            rocksdb_options_set_create_if_missing(o, u8::from(create));
            rocksdb_options_set_error_if_exists(o, u8::from(create));
            rocksdb_options_set_write_buffer_size(o, 4 << 20);
            rocksdb_options_set_max_write_buffer_number(o, 2);
            let mut err: *mut c_char = std::ptr::null_mut();
            let db = rocksdb_open(o, name.as_ptr(), &mut err);
            rocksdb_options_destroy(o);
            check(err)?;
            if db.is_null() {
                return Err(io::Error::other("rocksdb: open returned no handle"));
            }
            let wopts = rocksdb_writeoptions_create();
            rocksdb_writeoptions_disable_WAL(wopts, 1);
            Ok(RocksBase { db, wopts, ropts: rocksdb_readoptions_create() })
        }
    }
}

impl Drop for RocksBase {
    fn drop(&mut self) {
        // SAFETY: handles created in open, destroyed once.
        unsafe {
            rocksdb_writeoptions_destroy(self.wopts);
            rocksdb_readoptions_destroy(self.ropts);
            rocksdb_close(self.db);
        }
    }
}

impl Base for RocksBase {
    fn write(&mut self, ops: &[Op]) -> io::Result<()> {
        // SAFETY: the batch borrows the slices only for each call.
        unsafe {
            let b = rocksdb_writebatch_create();
            for op in ops {
                match op {
                    Op::Put(k, v) => rocksdb_writebatch_put(b, k.as_ptr().cast(), k.len(), v.as_ptr().cast(), v.len()),
                    Op::DeleteRange(s, e) => {
                        rocksdb_writebatch_delete_range(b, s.as_ptr().cast(), s.len(), e.as_ptr().cast(), e.len())
                    }
                }
            }
            let mut err: *mut c_char = std::ptr::null_mut();
            rocksdb_write(self.db, self.wopts, b, &mut err);
            rocksdb_writebatch_destroy(b);
            check(err)
        }
    }

    fn flush(&mut self) -> io::Result<()> {
        // SAFETY: plain C API calls.
        unsafe {
            let o = rocksdb_flushoptions_create();
            rocksdb_flushoptions_set_wait(o, 1);
            let mut err: *mut c_char = std::ptr::null_mut();
            rocksdb_flush(self.db, o, &mut err);
            rocksdb_flushoptions_destroy(o);
            check(err)
        }
    }

    fn get(&self, key: &[u8]) -> io::Result<Option<Vec<u8>>> {
        // SAFETY: the returned value is malloc'd and freed here.
        unsafe {
            let mut len = 0usize;
            let mut err: *mut c_char = std::ptr::null_mut();
            let v = rocksdb_get(self.db, self.ropts, key.as_ptr().cast(), key.len(), &mut len, &mut err);
            check(err)?;
            if v.is_null() {
                return Ok(None);
            }
            let out = std::slice::from_raw_parts(v as *const u8, len).to_vec();
            rocksdb_free(v as *mut c_void);
            Ok(Some(out))
        }
    }

    fn scan(&self, prefix: &[u8]) -> io::Result<Vec<(Vec<u8>, Vec<u8>)>> {
        let mut out = Vec::new();
        // SAFETY: the iterator's key and value are valid until the next move.
        unsafe {
            let it = rocksdb_create_iterator(self.db, self.ropts);
            rocksdb_iter_seek(it, prefix.as_ptr().cast(), prefix.len());
            while rocksdb_iter_valid(it) != 0 {
                let (mut kl, mut vl) = (0usize, 0usize);
                let k = std::slice::from_raw_parts(rocksdb_iter_key(it, &mut kl) as *const u8, kl);
                if !k.starts_with(prefix) {
                    break;
                }
                let v = std::slice::from_raw_parts(rocksdb_iter_value(it, &mut vl) as *const u8, vl);
                out.push((k.to_vec(), v.to_vec()));
                rocksdb_iter_next(it);
            }
            rocksdb_iter_destroy(it);
        }
        Ok(out)
    }
}
