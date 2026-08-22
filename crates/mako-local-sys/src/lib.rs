//! Raw declarations for the `mako_local_*` C ABI.
//!
//! The implementation is compiled into CMake's `libmako.a`. This crate owns
//! no policy and performs no linking; [`mako_local`](https://docs.rs/mako-local)
//! supplies the safe types and locates the native archive.

#![allow(non_camel_case_types)]

use core::ffi::{c_char, c_int, c_void};

/// ABI version compiled into these declarations.
pub const MAKO_LOCAL_ABI_VERSION: u32 = 0;

pub const MAKO_LOCAL_FEATURE_POINT_TRANSACTIONS: u64 = 1 << 0;
pub const MAKO_LOCAL_FEATURE_READ_MY_WRITES: u64 = 1 << 1;
pub const MAKO_LOCAL_FEATURE_OPACITY: u64 = 1 << 2;

pub const MAKO_LOCAL_MAX_TABLE_NAME_BYTES: usize = 1024;
pub const MAKO_LOCAL_MAX_KEY_BYTES: usize = 1024;
pub const MAKO_LOCAL_MAX_VALUE_BYTES: usize = 1_048_576;
pub const MAKO_LOCAL_TXN_ITEM_BUDGET: usize = 512;
/// Largest base timestamp in Mako's legacy one-digit-term u32 encoding.
pub const MAKO_LOCAL_MAX_MAKO_TIMESTAMP: u32 = (u32::MAX - 9) / 10;

pub const MAKO_LOCAL_OK: c_int = 0;
pub const MAKO_LOCAL_CONFLICT: c_int = 1;
pub const MAKO_LOCAL_NOT_ATTACHED: c_int = 2;
pub const MAKO_LOCAL_WRONG_THREAD: c_int = 3;
pub const MAKO_LOCAL_TXN_ALREADY_ACTIVE: c_int = 4;
pub const MAKO_LOCAL_TXN_FINISHED: c_int = 5;
pub const MAKO_LOCAL_WRONG_DB_OR_TABLE: c_int = 6;
pub const MAKO_LOCAL_INVALID_ARGUMENT: c_int = 7;
pub const MAKO_LOCAL_THREAD_LIMIT: c_int = 8;
pub const MAKO_LOCAL_BUSY: c_int = 9;
pub const MAKO_LOCAL_OUT_OF_MEMORY: c_int = 10;
pub const MAKO_LOCAL_INTERNAL: c_int = 11;
pub const MAKO_LOCAL_DUPLICATE_WRITE: c_int = 12;
pub const MAKO_LOCAL_TXN_TOO_LARGE: c_int = 13;
pub const MAKO_LOCAL_VALUE_TOO_LARGE: c_int = 14;
pub const MAKO_LOCAL_COMMIT_HOOK_REJECTED: c_int = 15;
pub const MAKO_LOCAL_TIMESTAMP_EXHAUSTED: c_int = 16;

/// Synchronous post-validation, pre-install callback.
///
/// Returning zero definitely aborts the transaction; a nonzero result permits
/// native installation to proceed. The timestamp is Mako's nonzero 32-bit
/// logical timestamp. The callback runs while Silo write locks are held: it may
/// enter a bounded in-memory critical section, but must not perform I/O, wait
/// for capacity, allocate, or unwind.
pub type mako_local_post_validate_hook =
    Option<unsafe extern "C" fn(context: *mut c_void, mako_timestamp: u32) -> c_int>;

/// Opaque local database handle.
#[repr(C)]
pub struct mako_local_db {
    _private: [u8; 0],
}

/// Opaque transactional table handle.
#[repr(C)]
pub struct mako_local_table {
    _private: [u8; 0],
}

/// Opaque, thread-affine transaction handle.
#[repr(C)]
pub struct mako_local_txn {
    _private: [u8; 0],
}

extern "C" {
    pub fn mako_local_abi_version() -> u32;
    pub fn mako_local_feature_bits() -> u64;
    pub fn mako_local_status_string(status: c_int) -> *const c_char;
    pub fn mako_local_thread_attach() -> c_int;
    pub fn mako_local_advance_mako_timestamp_past(observed: u32) -> c_int;

    pub fn mako_local_db_open(out: *mut *mut mako_local_db) -> c_int;
    pub fn mako_local_db_close(db: *mut mako_local_db) -> c_int;

    pub fn mako_local_table_open(
        db: *mut mako_local_db,
        name: *const u8,
        name_len: usize,
        table_id: u64,
        out: *mut *mut mako_local_table,
    ) -> c_int;
    pub fn mako_local_table_id(table: *const mako_local_table) -> u64;

    pub fn mako_local_txn_begin(db: *mut mako_local_db, out: *mut *mut mako_local_txn) -> c_int;
    pub fn mako_local_txn_get(
        txn: *mut mako_local_txn,
        table: *mut mako_local_table,
        key: *const u8,
        key_len: usize,
        value_out: *mut *mut u8,
        value_len_out: *mut usize,
        found_out: *mut u8,
    ) -> c_int;
    pub fn mako_local_txn_put(
        txn: *mut mako_local_txn,
        table: *mut mako_local_table,
        key: *const u8,
        key_len: usize,
        value: *const u8,
        value_len: usize,
        created_out: *mut u8,
    ) -> c_int;
    pub fn mako_local_txn_insert(
        txn: *mut mako_local_txn,
        table: *mut mako_local_table,
        key: *const u8,
        key_len: usize,
        value: *const u8,
        value_len: usize,
        inserted_out: *mut u8,
    ) -> c_int;
    pub fn mako_local_txn_remove(
        txn: *mut mako_local_txn,
        table: *mut mako_local_table,
        key: *const u8,
        key_len: usize,
        existed_out: *mut u8,
    ) -> c_int;
    pub fn mako_local_txn_commit(txn: *mut mako_local_txn) -> c_int;
    pub fn mako_local_txn_commit_with_hook(
        txn: *mut mako_local_txn,
        hook: mako_local_post_validate_hook,
        context: *mut c_void,
    ) -> c_int;
    pub fn mako_local_txn_abort(txn: *mut mako_local_txn) -> c_int;
    pub fn mako_local_txn_destroy(txn: *mut mako_local_txn) -> c_int;

    pub fn mako_local_bytes_free(bytes: *mut c_void);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn status_values_are_distinct() {
        let values = [
            MAKO_LOCAL_OK,
            MAKO_LOCAL_CONFLICT,
            MAKO_LOCAL_NOT_ATTACHED,
            MAKO_LOCAL_WRONG_THREAD,
            MAKO_LOCAL_TXN_ALREADY_ACTIVE,
            MAKO_LOCAL_TXN_FINISHED,
            MAKO_LOCAL_WRONG_DB_OR_TABLE,
            MAKO_LOCAL_INVALID_ARGUMENT,
            MAKO_LOCAL_THREAD_LIMIT,
            MAKO_LOCAL_BUSY,
            MAKO_LOCAL_OUT_OF_MEMORY,
            MAKO_LOCAL_INTERNAL,
            MAKO_LOCAL_DUPLICATE_WRITE,
            MAKO_LOCAL_TXN_TOO_LARGE,
            MAKO_LOCAL_VALUE_TOO_LARGE,
            MAKO_LOCAL_COMMIT_HOOK_REJECTED,
            MAKO_LOCAL_TIMESTAMP_EXHAUSTED,
        ];
        for (i, a) in values.iter().enumerate() {
            for b in &values[i + 1..] {
                assert_ne!(a, b);
            }
        }
    }
}
