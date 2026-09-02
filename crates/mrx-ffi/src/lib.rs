//! The `mrxdb_*` C ABI: the Rust cache, callable from C and C++.
//!
//! Shaped after RocksDB's `rocksdb/c.h` so that a caller already written
//! against that API moves with `sed`, not a rewrite. See
//! `include/mrxdb.h` for the contract as C sees it, and
//! `include/mrxdb_rocksdb_compat.h` for the mechanical rename.
//!
//! # The three rules every function here follows
//!
//! 1. **`extern "C"`, never `extern "C-unwind"`, and no panic escapes.**
//!    Every entry point wraps its body in `catch_unwind`. A Rust panic
//!    unwinding into a C frame is undefined behaviour, and the release
//!    profile's `panic = "abort"` only makes it *usually* not happen —
//!    the dev profile unwinds, so the guard has to be real code rather
//!    than a profile setting.
//! 2. **Ownership at the boundary is explicit and one-directional.**
//!    Anything this library returns as a buffer was allocated with
//!    `malloc` and is freed with `free` (exposed as [`mrxdb_free`]), so a
//!    caller cannot be wrong about which allocator to use — the mistake
//!    that a Rust-allocated return value invites.
//! 3. **Errors are out-parameters, not return codes.** `char **errptr`,
//!    non-null on failure, owned by the caller. Same as RocksDB, for the
//!    same reason: a `get` needs to distinguish "absent" from "failed"
//!    without stealing a bit from the return value.

#![allow(clippy::missing_safety_doc)]
// The handle types are named the way the C header names them, on
// purpose: `mrxdb_t` in Rust and `mrxdb_t` in C is one name to search
// for, and these types are never used from Rust as Rust.
#![allow(non_camel_case_types)]

use std::ffi::{CStr, c_char, c_int, c_uchar, c_void};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::PathBuf;
use std::ptr;
use std::sync::Arc;

use mrx::{Db, Durability, MrxStore, Options, WriteBatch};

extern "C" {
    fn malloc(n: usize) -> *mut c_void;
    fn free(p: *mut c_void);
}

/// Bumped on any incompatible change to this ABI.
pub const MRXDB_ABI_VERSION: u32 = 1;

// ===================================================================
// Handles
// ===================================================================

/// Opaque database handle.
pub struct mrxdb_t {
    db: Db,
}

/// Opaque options handle.
pub struct mrxdb_options_t {
    opts: Options,
}

/// Opaque write-batch handle.
pub struct mrxdb_writebatch_t {
    batch: WriteBatch,
}

/// Opaque iterator handle.
pub struct mrxdb_iterator_t {
    store: Arc<MrxStore>,
    forward: bool,
    buf: std::collections::VecDeque<(Vec<u8>, Vec<u8>)>,
    cursor: Option<Vec<u8>>,
    skip_first: bool,
    done: bool,
    /// The pair `mrxdb_iter_key`/`_value` currently point into. Held so
    /// those pointers stay valid until the next `mrxdb_iter_next`, which
    /// is the contract RocksDB's iterator has.
    current: Option<(Vec<u8>, Vec<u8>)>,
    error: Option<String>,
}

// ===================================================================
// Boundary helpers
// ===================================================================

/// Copy a Rust string into a `malloc`'d C string for `*errptr`.
///
/// Silently does nothing when `errptr` is null, matching RocksDB: a
/// caller who passes null has said it does not want the detail, not that
/// it wants a crash.
unsafe fn set_err(errptr: *mut *mut c_char, msg: &str) {
    if errptr.is_null() {
        return;
    }
    let n = msg.len();
    let p = malloc(n + 1) as *mut c_char;
    if p.is_null() {
        *errptr = ptr::null_mut();
        return;
    }
    ptr::copy_nonoverlapping(msg.as_ptr(), p as *mut u8, n);
    *p.add(n) = 0;
    *errptr = p;
}

/// Run `f`, converting a panic into an error string.
///
/// Rule 1. The `AssertUnwindSafe` is sound here because every failure
/// path returns `fallback` and touches no state that a panic could have
/// left inconsistent — the handles are either untouched or about to be
/// reported as failed.
unsafe fn guard<R>(errptr: *mut *mut c_char, what: &str, fallback: R, f: impl FnOnce() -> R) -> R {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(r) => r,
        Err(_) => {
            set_err(errptr, &format!("mrxdb: panic in {what}"));
            fallback
        }
    }
}

unsafe fn slice<'a>(p: *const c_char, len: usize) -> &'a [u8] {
    if p.is_null() || len == 0 {
        &[]
    } else {
        std::slice::from_raw_parts(p as *const u8, len)
    }
}

/// Copy bytes into a `malloc`'d buffer the caller frees with
/// [`mrxdb_free`].
unsafe fn out_bytes(b: &[u8]) -> *mut c_char {
    // A zero-length value is still PRESENT, so it must not come back as
    // null — null means absent. malloc(0) may legitimately return null,
    // hence the max.
    let p = malloc(b.len().max(1)) as *mut c_char;
    if p.is_null() {
        return ptr::null_mut();
    }
    if !b.is_empty() {
        ptr::copy_nonoverlapping(b.as_ptr(), p as *mut u8, b.len());
    }
    p
}

// ===================================================================
// Version
// ===================================================================

/// This library's ABI version, to be compared against
/// `MRXDB_ABI_VERSION` in the header.
#[no_mangle]
pub extern "C" fn mrxdb_abi_version() -> u32 {
    MRXDB_ABI_VERSION
}

/// Free a buffer or error string returned by this library.
///
/// Plain `free`; provided so callers have one obvious name and never
/// have to know which allocator produced the pointer.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_free(p: *mut c_void) {
    if !p.is_null() {
        free(p);
    }
}

// ===================================================================
// Options
// ===================================================================

/// Create default options.
#[no_mangle]
pub extern "C" fn mrxdb_options_create() -> *mut mrxdb_options_t {
    match catch_unwind(|| {
        Box::into_raw(Box::new(mrxdb_options_t {
            opts: Options::default(),
        }))
    }) {
        Ok(p) => p,
        Err(_) => ptr::null_mut(),
    }
}

/// Destroy options. Null is a no-op.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_options_destroy(o: *mut mrxdb_options_t) {
    if !o.is_null() {
        drop(Box::from_raw(o));
    }
}

/// Accepted and ignored: this cache always creates the database if it is
/// missing. Present so a ported RocksDB caller compiles unchanged.
#[no_mangle]
pub extern "C" fn mrxdb_options_set_create_if_missing(_o: *mut mrxdb_options_t, _v: c_uchar) {}

/// Byte ceiling for the evictable value tier. 0 disables eviction.
///
/// This bounds *values*. Keys are always resident, so a workload with
/// enough keys exceeds any setting here — see the header.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_options_set_capacity_bytes(o: *mut mrxdb_options_t, bytes: u64) {
    if o.is_null() {
        return;
    }
    (*o).opts.capacity_bytes = if bytes == 0 { None } else { Some(bytes) };
}

/// Durability of each writeback batch: 0 = fsync, 1 = WAL only, 2 = none.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_options_set_durability(o: *mut mrxdb_options_t, level: c_int) {
    if o.is_null() {
        return;
    }
    (*o).opts.durability = match level {
        0 => Durability::Sync,
        2 => Durability::None,
        _ => Durability::Wal,
    };
}

// ===================================================================
// Open and close
// ===================================================================

/// Open, creating the database if needed. Null on failure.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_open(
    o: *const mrxdb_options_t,
    name: *const c_char,
    errptr: *mut *mut c_char,
) -> *mut mrxdb_t {
    guard(errptr, "mrxdb_open", ptr::null_mut(), || {
        if name.is_null() {
            set_err(errptr, "mrxdb_open: null path");
            return ptr::null_mut();
        }
        let path = match CStr::from_ptr(name).to_str() {
            Ok(s) => PathBuf::from(s),
            Err(_) => {
                set_err(errptr, "mrxdb_open: path is not valid UTF-8");
                return ptr::null_mut();
            }
        };
        let opts = if o.is_null() {
            Options::default()
        } else {
            (*o).opts.clone()
        };
        match Db::open(path, opts) {
            Ok(db) => Box::into_raw(Box::new(mrxdb_t { db })),
            Err(e) => {
                set_err(errptr, &format!("mrxdb_open: {e}"));
                ptr::null_mut()
            }
        }
    })
}

/// Close cleanly: make everything acked durable, then release.
///
/// **Sets `*errptr` if the drain failed**, and still frees the handle. A
/// caller that ignores the error has exited having lost acknowledged
/// writes, which is exactly the case worth reporting; there is nothing
/// left to retry against, so the handle goes either way.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_close(db: *mut mrxdb_t, errptr: *mut *mut c_char) {
    if db.is_null() {
        return;
    }
    guard(errptr, "mrxdb_close", (), || {
        let boxed = Box::from_raw(db);
        if let Err(e) = boxed.db.close() {
            set_err(errptr, &format!("mrxdb_close: {e}"));
        }
    })
}

// ===================================================================
// Point operations
// ===================================================================

/// Read a key. Null return means **absent**; check `*errptr` to
/// distinguish absent from failed.
///
/// A present-but-empty value returns a non-null pointer with
/// `*vallen == 0`.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_get(
    db: *mut mrxdb_t,
    key: *const c_char,
    keylen: usize,
    vallen: *mut usize,
    errptr: *mut *mut c_char,
) -> *mut c_char {
    guard(errptr, "mrxdb_get", ptr::null_mut(), || {
        if db.is_null() || vallen.is_null() {
            set_err(errptr, "mrxdb_get: null argument");
            return ptr::null_mut();
        }
        *vallen = 0;
        match (*db).db.get(slice(key, keylen)) {
            Ok(None) => ptr::null_mut(),
            Ok(Some(v)) => {
                *vallen = v.len();
                out_bytes(&v)
            }
            Err(e) => {
                set_err(errptr, &format!("mrxdb_get: {e}"));
                ptr::null_mut()
            }
        }
    })
}

/// Write a key. Returns once visible, **not** once durable.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_put(
    db: *mut mrxdb_t,
    key: *const c_char,
    keylen: usize,
    val: *const c_char,
    vallen: usize,
    errptr: *mut *mut c_char,
) {
    guard(errptr, "mrxdb_put", (), || {
        if db.is_null() {
            set_err(errptr, "mrxdb_put: null database");
            return;
        }
        if let Err(e) = (*db).db.put(slice(key, keylen), slice(val, vallen)) {
            set_err(errptr, &format!("mrxdb_put: {e}"));
        }
    })
}

/// Delete a key.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_delete(
    db: *mut mrxdb_t,
    key: *const c_char,
    keylen: usize,
    errptr: *mut *mut c_char,
) {
    guard(errptr, "mrxdb_delete", (), || {
        if db.is_null() {
            set_err(errptr, "mrxdb_delete: null database");
            return;
        }
        if let Err(e) = (*db).db.delete(slice(key, keylen)) {
            set_err(errptr, &format!("mrxdb_delete: {e}"));
        }
    })
}

/// Write only if absent. Returns 1 if written, 0 if the key was live.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_insert(
    db: *mut mrxdb_t,
    key: *const c_char,
    keylen: usize,
    val: *const c_char,
    vallen: usize,
    errptr: *mut *mut c_char,
) -> c_uchar {
    guard(errptr, "mrxdb_insert", 0, || {
        if db.is_null() {
            set_err(errptr, "mrxdb_insert: null database");
            return 0;
        }
        c_uchar::from((*db).db.insert(slice(key, keylen), slice(val, vallen)))
    })
}

/// Block until every write acked before this call is durable.
///
/// The real barrier, and the reason this ABI is not a drop-in for
/// RocksDB's: without it, a write that returned is not yet safe.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_flush(db: *mut mrxdb_t, errptr: *mut *mut c_char) {
    guard(errptr, "mrxdb_flush", (), || {
        if db.is_null() {
            set_err(errptr, "mrxdb_flush: null database");
            return;
        }
        if let Err(e) = (*db).db.flush() {
            set_err(errptr, &format!("mrxdb_flush: {e}"));
        }
    })
}

/// Delete every key and make that durable.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_clear(db: *mut mrxdb_t, errptr: *mut *mut c_char) {
    guard(errptr, "mrxdb_clear", (), || {
        if db.is_null() {
            set_err(errptr, "mrxdb_clear: null database");
            return;
        }
        if let Err(e) = (*db).db.clear() {
            set_err(errptr, &format!("mrxdb_clear: {e}"));
        }
    })
}

// ===================================================================
// Write batches
// ===================================================================

/// Create an empty batch.
#[no_mangle]
pub extern "C" fn mrxdb_writebatch_create() -> *mut mrxdb_writebatch_t {
    match catch_unwind(|| {
        Box::into_raw(Box::new(mrxdb_writebatch_t {
            batch: WriteBatch::new(),
        }))
    }) {
        Ok(p) => p,
        Err(_) => ptr::null_mut(),
    }
}

/// Destroy a batch. Null is a no-op.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_writebatch_destroy(b: *mut mrxdb_writebatch_t) {
    if !b.is_null() {
        drop(Box::from_raw(b));
    }
}

/// Queue a write.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_writebatch_put(
    b: *mut mrxdb_writebatch_t,
    key: *const c_char,
    keylen: usize,
    val: *const c_char,
    vallen: usize,
) {
    if b.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        (*b).batch.put(slice(key, keylen), slice(val, vallen));
    }));
}

/// Queue a delete.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_writebatch_delete(
    b: *mut mrxdb_writebatch_t,
    key: *const c_char,
    keylen: usize,
) {
    if b.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        (*b).batch.delete(slice(key, keylen));
    }));
}

/// Apply a batch. **Not atomic** — see the header.
///
/// The batch is consumed; the handle is still the caller's to destroy.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_write(
    db: *mut mrxdb_t,
    b: *mut mrxdb_writebatch_t,
    errptr: *mut *mut c_char,
) {
    guard(errptr, "mrxdb_write", (), || {
        if db.is_null() || b.is_null() {
            set_err(errptr, "mrxdb_write: null argument");
            return;
        }
        let batch = std::mem::take(&mut (*b).batch);
        if let Err(e) = (*db).db.write(batch) {
            set_err(errptr, &format!("mrxdb_write: {e}"));
        }
    })
}

// ===================================================================
// Iteration
// ===================================================================

/// Create an iterator. Position it with `seek` or `seek_to_first`.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_create_iterator(db: *mut mrxdb_t) -> *mut mrxdb_iterator_t {
    if db.is_null() {
        return ptr::null_mut();
    }
    match catch_unwind(AssertUnwindSafe(|| {
        Box::into_raw(Box::new(mrxdb_iterator_t {
            store: Arc::clone((*db).db.store()),
            forward: true,
            buf: Default::default(),
            cursor: None,
            skip_first: false,
            done: true,
            current: None,
            error: None,
        }))
    })) {
        Ok(p) => p,
        Err(_) => ptr::null_mut(),
    }
}

/// Destroy an iterator. Null is a no-op.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_iter_destroy(it: *mut mrxdb_iterator_t) {
    if !it.is_null() {
        drop(Box::from_raw(it));
    }
}

unsafe fn iter_reset(it: *mut mrxdb_iterator_t, from: &[u8], forward: bool) {
    let i = &mut *it;
    i.forward = forward;
    i.buf.clear();
    i.cursor = Some(from.to_vec());
    i.skip_first = false;
    i.done = false;
    i.current = None;
    i.error = None;
    iter_advance(it);
}

/// Pull the next pair into `current`, refilling from the store as needed.
unsafe fn iter_advance(it: *mut mrxdb_iterator_t) {
    let i = &mut *it;
    loop {
        if let Some(pair) = i.buf.pop_front() {
            i.current = Some(pair);
            return;
        }
        if i.done {
            i.current = None;
            return;
        }
        let Some(from) = i.cursor.take() else {
            i.done = true;
            i.current = None;
            return;
        };
        match i.store.chunk(&from, i.forward, i.skip_first) {
            Ok(chunk) => {
                i.buf.extend(chunk.pairs);
                match chunk.next_from {
                    // Loops rather than returns: a chunk can be empty
                    // while the range continues, when every key in it was
                    // deleted. Returning here would end the walk early.
                    Some(next) => {
                        i.cursor = Some(next);
                        i.skip_first = true;
                    }
                    None => i.done = true,
                }
            }
            Err(e) => {
                i.error = Some(format!("mrxdb iteration: {e}"));
                i.done = true;
                i.current = None;
                return;
            }
        }
    }
}

/// Position at the first key.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_iter_seek_to_first(it: *mut mrxdb_iterator_t) {
    if it.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| iter_reset(it, b"", true)));
}

/// Position at the first key at or after `key`.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_iter_seek(
    it: *mut mrxdb_iterator_t,
    key: *const c_char,
    keylen: usize,
) {
    if it.is_null() {
        return;
    }
    let k = slice(key, keylen).to_vec();
    let _ = catch_unwind(AssertUnwindSafe(|| iter_reset(it, &k, true)));
}

/// Position at the last key at or before `key`, walking backwards.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_iter_seek_for_prev(
    it: *mut mrxdb_iterator_t,
    key: *const c_char,
    keylen: usize,
) {
    if it.is_null() {
        return;
    }
    let k = slice(key, keylen).to_vec();
    let _ = catch_unwind(AssertUnwindSafe(|| iter_reset(it, &k, false)));
}

/// Whether the iterator is positioned on a pair.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_iter_valid(it: *const mrxdb_iterator_t) -> c_uchar {
    if it.is_null() {
        return 0;
    }
    c_uchar::from((*it).current.is_some())
}

/// Step in the iterator's direction.
///
/// There is no `prev`: an iterator here walks one way, fixed at seek
/// time. RocksDB's bidirectional cursor would need a real cursor into the
/// index rather than a chunk buffer, and offering a `prev` that silently
/// re-walked would be worse than not offering one.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_iter_next(it: *mut mrxdb_iterator_t) {
    if it.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| iter_advance(it)));
}

/// The current key. Valid until the next `next`/`seek`/`destroy`.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_iter_key(
    it: *const mrxdb_iterator_t,
    klen: *mut usize,
) -> *const c_char {
    if it.is_null() || klen.is_null() {
        return ptr::null();
    }
    match &(*it).current {
        None => {
            *klen = 0;
            ptr::null()
        }
        Some((k, _)) => {
            *klen = k.len();
            k.as_ptr() as *const c_char
        }
    }
}

/// The current value. Valid until the next `next`/`seek`/`destroy`.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_iter_value(
    it: *const mrxdb_iterator_t,
    vlen: *mut usize,
) -> *const c_char {
    if it.is_null() || vlen.is_null() {
        return ptr::null();
    }
    match &(*it).current {
        None => {
            *vlen = 0;
            ptr::null()
        }
        Some((_, v)) => {
            *vlen = v.len();
            v.as_ptr() as *const c_char
        }
    }
}

/// Report an iteration failure, if any.
///
/// A value may have to be fetched back from the durable store mid-walk,
/// and that can fail. Without this the iterator would just look like it
/// ended, turning a transient IO error into apparent data loss.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_iter_get_error(
    it: *const mrxdb_iterator_t,
    errptr: *mut *mut c_char,
) {
    if it.is_null() {
        return;
    }
    if let Some(e) = &(*it).error {
        set_err(errptr, e);
    }
}

// ===================================================================
// Introspection
// ===================================================================

/// The durability watermark: every version at or below it is durable.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_watermark(db: *const mrxdb_t) -> u64 {
    if db.is_null() {
        return 0;
    }
    (*db).db.watermark()
}

/// Key count, **including deleted keys**.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_len(db: *const mrxdb_t) -> usize {
    if db.is_null() {
        return 0;
    }
    (*db).db.len()
}

/// Whether a key's value is currently in memory: 1 resident, 0 evicted,
/// -1 unknown or deleted.
///
/// For tests. Nothing in the data path should branch on this.
#[no_mangle]
pub unsafe extern "C" fn mrxdb_is_resident(
    db: *const mrxdb_t,
    key: *const c_char,
    keylen: usize,
) -> c_int {
    if db.is_null() {
        return -1;
    }
    match (*db).db.store().is_resident(slice(key, keylen)) {
        Some(true) => 1,
        Some(false) => 0,
        None => -1,
    }
}
