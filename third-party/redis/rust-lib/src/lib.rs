use bytes::Bytes;
use redis_protocol::resp3::{types::BytesFrame, types::DecodedFrame};
use socket2::{Domain, Protocol, Socket, Type};
use std::cell::Cell;
use std::collections::{HashMap, HashSet, VecDeque};
use std::env;
use std::io::{ErrorKind, Read, Write};
use std::net::{SocketAddr, TcpListener, TcpStream};
use std::os::fd::AsRawFd;
use std::os::unix::net::UnixStream;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Barrier, Condvar, Mutex, OnceLock, Weak};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

mod resp3_handler;
use resp3_handler::Resp3Handler;
mod script;

static CONNECTED_CLIENTS: AtomicUsize = AtomicUsize::new(0);
static BLOCKED_CLIENTS: AtomicUsize = AtomicUsize::new(0);
static DIRTY_CHANGES: AtomicUsize = AtomicUsize::new(0);
static TOTAL_CONNECTIONS_RECEIVED: AtomicUsize = AtomicUsize::new(0);
static TOTAL_COMMANDS_PROCESSED: AtomicUsize = AtomicUsize::new(0);
static CMDSTAT_BLPOP_CALLS: AtomicUsize = AtomicUsize::new(0);
static NEXT_CLIENT_ID: AtomicUsize = AtomicUsize::new(1);
static NEXT_SCAN_CURSOR_ID: AtomicUsize = AtomicUsize::new(1);
static RANDOMKEY_COUNTER: AtomicUsize = AtomicUsize::new(0);
static MAXMEMORY_SETTING: AtomicUsize = AtomicUsize::new(0);
static LUA_BUSY: AtomicUsize = AtomicUsize::new(0);
const MAX_REDIS_WORKERS: usize = 32;

#[repr(align(64))]
struct WorkerCounter(AtomicUsize);

impl WorkerCounter {
    const fn new() -> Self {
        Self(AtomicUsize::new(0))
    }
}

static WORKER_COMMANDS_PROCESSED: [WorkerCounter; MAX_REDIS_WORKERS] =
    [const { WorkerCounter::new() }; MAX_REDIS_WORKERS];
static WORKER_DIRTY_CHANGES: [WorkerCounter; MAX_REDIS_WORKERS] =
    [const { WorkerCounter::new() }; MAX_REDIS_WORKERS];
static WORKER_BLPOP_CALLS: [WorkerCounter; MAX_REDIS_WORKERS] =
    [const { WorkerCounter::new() }; MAX_REDIS_WORKERS];

thread_local! {
    static REDIS_WORKER_ID: Cell<usize> = const { Cell::new(MAX_REDIS_WORKERS) };
    /// The logical database (SELECT) of the connection this worker thread is
    /// serving right now.
    ///
    /// One worker owns one client for the whole of a frame, from
    /// `process_buffered_frames` through `parse_resp3`, execution and the
    /// reply, so a thread-local carries the selected database to
    /// `validate_user_key` (which turns a Redis-visible key into its
    /// storage-facing name) and to `write_command_result` (which turns the key
    /// names in a reply back) without threading a database argument through
    /// every one of the ninety-seven parse arms that handle a key. See
    /// `set_current_db` for the paths that keep it in step with
    /// `ClientState::db`, which is the authoritative copy.
    static CURRENT_DB: Cell<u8> = const { Cell::new(0) };
}
static SCAN_CURSORS: OnceLock<Mutex<HashMap<usize, Bytes>>> = OnceLock::new();
static PUBSUB_REGISTRY: OnceLock<Mutex<PubSubRegistry>> = OnceLock::new();
static UNBLOCK_REQUESTS: OnceLock<Mutex<HashMap<usize, bool>>> = OnceLock::new();
static WORKER_WAKES: OnceLock<Vec<Weak<WorkerWake>>> = OnceLock::new();
static BLOCKED_REGISTRY: OnceLock<(Mutex<BlockedClientRegistry>, Condvar)> = OnceLock::new();
static KEY_VERSIONS: OnceLock<Mutex<HashMap<Bytes, usize>>> = OnceLock::new();
static WATCHED_EXISTING_KEYS: OnceLock<Mutex<HashSet<Bytes>>> = OnceLock::new();
static REDIS_BACKEND: OnceLock<RedisBackend> = OnceLock::new();
static MEMORY_STORE: OnceLock<Mutex<HashMap<Bytes, MemoryEntry>>> = OnceLock::new();
const MAKO_HASH_DUMP_PREFIX: &[u8] = b"MAKO_HASH_DUMP\0";
const MAKO_LIST_DUMP_PREFIX: &[u8] = b"MAKO_LIST_DUMP\0";
const MAKO_STRING_DUMP_PREFIX: &[u8] = b"MAKO_STRING_DUMP\0";
const MAKO_SET_DUMP_PREFIX: &[u8] = b"MAKO_SET_DUMP\0";
const MAKO_ZSET_DUMP_PREFIX: &[u8] = b"MAKO_ZSET_DUMP\0";
const MAKO_STREAM_DUMP_PREFIX: &[u8] = b"MAKO_STREAM_DUMP\0";

// ===== FFI Types (must match transaction_ffi.h) =====
// Redis-visible keys must not use the 0x01 prefix. The C++ executor stores
// TTL metadata under "\x01TTL:<key>" and keeps expiry checks inside the same
// transaction as the user-key operation.

const TXN_OP_GET: u32 = 1;
const TXN_OP_SET: u32 = 2;
const TXN_OP_DEL: u32 = 3;
const TXN_OP_EXISTS: u32 = 4;
const TXN_OP_APPEND: u32 = 5;
const TXN_OP_STRLEN: u32 = 6;
const TXN_OP_INCRBY: u32 = 7;
const TXN_OP_INCRBYFLOAT: u32 = 8;
const TXN_OP_EXPIRE: u32 = 9;
const TXN_OP_TTL: u32 = 10;
const TXN_OP_PERSIST: u32 = 11;
const TXN_OP_SCAN: u32 = 12;
const TXN_OP_SADD: u32 = 13;
const TXN_OP_SREM: u32 = 14;
const TXN_OP_SISMEMBER: u32 = 15;
const TXN_OP_SCARD: u32 = 16;
const TXN_OP_SMEMBERS: u32 = 17;
const TXN_OP_SPOP: u32 = 18;
const TXN_OP_SRANDMEMBER: u32 = 19;
const TXN_OP_SMOVE: u32 = 20;
const TXN_OP_SET_ALGEBRA: u32 = 21;
const TXN_OP_TYPE: u32 = 22;
const TXN_OP_LPUSH: u32 = 23;
const TXN_OP_RPUSH: u32 = 24;
const TXN_OP_LPOP: u32 = 25;
const TXN_OP_RPOP: u32 = 26;
const TXN_OP_LLEN: u32 = 27;
const TXN_OP_LINDEX: u32 = 28;
const TXN_OP_LRANGE: u32 = 29;
const TXN_OP_LSET: u32 = 30;
const TXN_OP_LREM: u32 = 31;
const TXN_OP_LTRIM: u32 = 32;
const TXN_OP_LINSERT: u32 = 33;
const TXN_OP_LMOVE: u32 = 34;
const TXN_OP_LPOS: u32 = 35;
const TXN_OP_ZADD: u32 = 36;
const TXN_OP_ZSCORE: u32 = 37;
const TXN_OP_ZREM: u32 = 38;
const TXN_OP_ZCARD: u32 = 39;
const TXN_OP_ZRANGE: u32 = 40;
const TXN_OP_ZRANK: u32 = 41;
const TXN_OP_ZPOPMIN: u32 = 42;
const TXN_OP_ZCOUNT: u32 = 43;
const TXN_OP_ZSCAN: u32 = 44;
const TXN_OP_FLUSHDB: u32 = 45;
const TXN_OP_HSET: u32 = 46;
const TXN_OP_HGET: u32 = 47;
const TXN_OP_HMGET: u32 = 48;
const TXN_OP_HGETALL: u32 = 49;
const TXN_OP_HDEL: u32 = 50;
const TXN_OP_HEXISTS: u32 = 51;
const TXN_OP_HLEN: u32 = 52;
const TXN_OP_HKEYS: u32 = 53;
const TXN_OP_HVALS: u32 = 54;
const TXN_OP_HSTRLEN: u32 = 55;
const TXN_OP_HINCRBY: u32 = 56;
const TXN_OP_HINCRBYFLOAT: u32 = 57;
const TXN_OP_HSCAN: u32 = 58;
const TXN_OP_SETBIT: u32 = 59;
const TXN_OP_GETBIT: u32 = 60;
const TXN_OP_SETRANGE: u32 = 61;
const TXN_OP_GETRANGE: u32 = 62;
const TXN_OP_BPOP: u32 = 63;
const TXN_OP_RENAME: u32 = 64;
const TXN_OP_SORT: u32 = 65;
const TXN_OP_DUMP: u32 = 66;
const TXN_OP_RESTORE_LIST: u32 = 67;
const TXN_OP_ZRANGEBYLEX: u32 = 68;
const TXN_OP_ZLEXCOUNT: u32 = 69;
const TXN_OP_ZREMRANGEBYSCORE: u32 = 70;
const TXN_OP_ZREMRANGEBYRANK: u32 = 71;
const TXN_OP_ZREMRANGEBYLEX: u32 = 72;
const TXN_OP_ZRANGESTORE: u32 = 73;
const TXN_OP_ZSET_ALGEBRA: u32 = 74;
const TXN_OP_ZMPOP: u32 = 75;
const TXN_OP_ZRANDMEMBER: u32 = 76;
const TXN_OP_COPY: u32 = 77;
const TXN_OP_MOVE: u32 = 86;
const TXN_OP_BITOP: u32 = 78;
const TXN_OP_HLL_ADD: u32 = 79;
const TXN_OP_HLL_COUNT: u32 = 80;
const TXN_OP_HLL_MERGE: u32 = 81;
const TXN_OP_BITFIELD: u32 = 82;
const TXN_OP_HFIELD_EXPIRE: u32 = 83;
const TXN_OP_HFIELD_TTL: u32 = 84;
const TXN_OP_HFIELD_PERSIST: u32 = 85;
// Redis Streams. TXN_OP_XRANGE serves XRANGE, XREVRANGE and each stream of an
// XREAD, because the three differ only in the read mode packed into the op.
const TXN_OP_XADD: u32 = 87;
const TXN_OP_XRANGE: u32 = 88;
const TXN_OP_XLEN: u32 = 89;
const TXN_OP_XDEL: u32 = 90;
const TXN_OP_XTRIM: u32 = 91;
const TXN_OP_XSETID: u32 = 92;
const TXN_OP_XINFO: u32 = 93;
const TXN_OP_XRESTORE: u32 = 94;
const TXN_OP_XGROUP: u32 = 95;
const TXN_OP_XREADGROUP: u32 = 96;
const TXN_OP_XACK: u32 = 97;
const TXN_OP_XPENDING: u32 = 98;
const TXN_OP_XCLAIM: u32 = 99;
const TXN_OP_XAUTOCLAIM: u32 = 100;

// Command-level failures a stream op reports in int_value; see the
// TXN_STREAM_ERR_* block in include/transaction_ffi.h.
const TXN_STREAM_ERR_NOMKSTREAM: i64 = -1;
const TXN_STREAM_ERR_SMALLER_ID: i64 = -2;
const TXN_STREAM_ERR_NO_SUCH_KEY: i64 = -3;
const TXN_STREAM_ERR_SETID_SMALLER: i64 = -4;
const TXN_STREAM_ERR_SETID_ENTRIES_ADDED: i64 = -5;
const TXN_STREAM_ERR_SETID_TOMBSTONE: i64 = -6;
const TXN_STREAM_ERR_NOGROUP: i64 = -7;
const TXN_STREAM_ERR_BUSYGROUP: i64 = -8;
const TXN_STREAM_ERR_NO_KEY_FOR_GROUP: i64 = -9;

// Which XINFO reply shape a parsed command wants, carried in `restore_kind`.
const STREAM_XINFO_STREAM: u8 = 0;
const STREAM_XINFO_STREAM_FULL: u8 = 1;
const STREAM_XINFO_GROUPS: u8 = 2;
const STREAM_XINFO_CONSUMERS: u8 = 3;
const STREAM_XINFO_HELP: u8 = 4;

/// Redis stores a hash field's expiration in 46 bits of absolute Unix
/// milliseconds (`EB_EXPIRE_TIME_MAX` in `ebuckets.h`), and every command of
/// the HEXPIRE family refuses a time past it.
const HASH_FIELD_EXPIRE_TIME_MAX_MS: i64 = (1i64 << 46) - 1;

/// Mirrors TXN_HLL_ERR_NOT_HLL in transaction_ffi.h: an HLL op reports a key
/// holding a string that is not a valid sketch through int_value.
const HLL_ERR_NOT_HLL: i64 = -2;

/// Mirror the TXN_INCR_ERR_* sentinels in transaction_ffi.h. A failed INCR,
/// INCRBY, DECR, DECRBY or INCRBYFLOAT comes back as success=false with one of
/// these in int_value; int_value 0 means the key holds another type and the
/// reply is WRONGTYPE.
const INCR_ERR_NOT_INTEGER: i64 = -1;
const INCR_ERR_OVERFLOW: i64 = -2;
const INCR_ERR_NOT_FLOAT: i64 = -3;
const INCR_ERR_NAN_OR_INF: i64 = -4;

const ZRANGE_MODE_RANK: i64 = 0;
const ZRANGE_MODE_SCORE: i64 = 1;
const ZRANGE_MODE_LEX: i64 = 2;
const ZAGG_SUM: i64 = 0;
const ZAGG_MIN: i64 = 1;
const ZAGG_MAX: i64 = 2;

const TXN_FLAG_SET_NX: u32 = 1 << 0;
const TXN_FLAG_SET_XX: u32 = 1 << 1;
const TXN_FLAG_SET_RETURN_OLD: u32 = 1 << 2;
const TXN_FLAG_SET_INTEGER_REPLY: u32 = 1 << 3;
const TXN_FLAG_SET_REQUIRE_ABSENT_GROUP: u32 = 1 << 4;
const TXN_FLAG_SET_KEEP_TTL: u32 = 1 << 5;
const TXN_FLAG_TTL_MILLISECONDS: u32 = 1 << 6;
const TXN_FLAG_EXPIRE_NX: u32 = 1 << 7;
const TXN_FLAG_EXPIRE_XX: u32 = 1 << 8;
const TXN_FLAG_EXPIRE_GT: u32 = 1 << 9;
const TXN_FLAG_EXPIRE_LT: u32 = 1 << 10;
const TXN_FLAG_SCAN_COUNT_ONLY: u32 = 1 << 11;
const TXN_FLAG_SET_COUNT_GIVEN: u32 = 1 << 12;
const TXN_FLAG_SET_ALLOW_DUPLICATES: u32 = 1 << 13;
const TXN_FLAG_SET_ALGEBRA_UNION: u32 = 1 << 14;
const TXN_FLAG_SET_ALGEBRA_DIFF: u32 = 1 << 15;
const TXN_FLAG_SET_ALGEBRA_STORE: u32 = 1 << 16;
const TXN_FLAG_LIST_PUSH_IF_EXISTS: u32 = 1 << 17;
const TXN_FLAG_LIST_INSERT_BEFORE: u32 = 1 << 18;
const TXN_FLAG_LIST_SOURCE_LEFT: u32 = 1 << 19;
const TXN_FLAG_LIST_DEST_LEFT: u32 = 1 << 20;
const TXN_FLAG_LIST_COUNT_GIVEN: u32 = 1 << 21;
const TXN_FLAG_ZADD_NX: u32 = 1 << 22;
const TXN_FLAG_ZADD_XX: u32 = 1 << 23;
const TXN_FLAG_ZADD_CH: u32 = 1 << 24;
const TXN_FLAG_ZADD_INCR: u32 = 1 << 25;
const TXN_FLAG_ZADD_GT: u32 = 1 << 26;
const TXN_FLAG_ZADD_LT: u32 = 1 << 27;
const TXN_FLAG_Z_WITHSCORES: u32 = 1 << 28;
const TXN_FLAG_Z_REV: u32 = 1 << 29;
const TXN_FLAG_Z_BYSCORE: u32 = 1 << 30;
const TXN_FLAG_Z_COUNT_GIVEN: u32 = 1 << 31;

const FAST_MAKO_ABORTED: u32 = 0;
const FAST_MAKO_GET_MISS: u32 = 1;
const FAST_MAKO_GET_HIT: u32 = 2;
const FAST_MAKO_SET_OK: u32 = 3;
const FAST_MAKO_FALLBACK: u32 = 4;

#[repr(C)]
struct TxnOperation {
    op: u32,
    key_ptr: *const u8,
    key_len: usize,
    val_ptr: *const u8,
    val_len: usize,
    flags: u32,
    expire_at_ms: i64,
    group_id: u32,
}

#[repr(C)]
struct TxnRequest {
    num_ops: usize,
    ops: *const TxnOperation,
}

#[repr(C)]
struct TxnOpResult {
    success: bool,
    value_present: bool,
    data_ptr: *mut u8,
    data_len: usize,
    int_value: i64,
}

#[repr(C)]
struct TxnResponse {
    transaction_success: bool,
    num_results: usize,
    results: *mut TxnOpResult,
}

#[repr(C)]
struct FastMakoStringResult {
    status: u32,
    data_ptr: *const u8,
    data_len: usize,
}

#[repr(C)]
#[derive(Default)]
struct MakoMetrics {
    txn_commits: u64,
    txn_aborts: u64,
    txn_retries: u64,
    uptime_seconds: u64,
    cache_enabled: u64,
    cache_capacity_bytes: u64,
    cache_entries: u64,
    cache_bytes: u64,
    cache_hits: u64,
    cache_misses: u64,
    cache_inserts: u64,
    cache_evictions: u64,
    cache_invalidations: u64,
}

#[derive(Clone, Copy, PartialEq)]
enum RedisBackend {
    Mako,
    Memory,
}

impl RedisBackend {
    fn from_env() -> Self {
        match env::var("MAKO_REDIS_BACKEND").or_else(|_| env::var("MAKO_REDIS_MODE")) {
            Ok(value)
                if value.eq_ignore_ascii_case("memory") || value.eq_ignore_ascii_case("cache") =>
            {
                RedisBackend::Memory
            }
            Ok(value) if value.eq_ignore_ascii_case("mako") => RedisBackend::Mako,
            Ok(value) => {
                eprintln!("Unknown MAKO_REDIS_BACKEND={value}; defaulting to mako");
                RedisBackend::Mako
            }
            Err(_) => RedisBackend::Mako,
        }
    }

    fn name(self) -> &'static str {
        match self {
            RedisBackend::Mako => "mako",
            RedisBackend::Memory => "memory",
        }
    }
}

fn redis_backend() -> RedisBackend {
    *REDIS_BACKEND.get_or_init(RedisBackend::from_env)
}

#[derive(Clone)]
struct MemoryEntry {
    value: Vec<u8>,
    expire_at_ms: Option<i64>,
}

#[cfg(not(test))]
extern "C" {
    fn cpp_worker_thread_init(thread_id: usize);

    fn cpp_execute_fast_mako_string(
        op: u32,
        key_ptr: *const u8,
        key_len: usize,
        val_ptr: *const u8,
        val_len: usize,
        result: *mut FastMakoStringResult,
    ) -> bool;

    // Generic command/fallback and MULTI/EXEC interface.
    fn cpp_execute_transaction(request: *const TxnRequest, response: *mut TxnResponse) -> bool;
    fn cpp_free_transaction_response(response: *mut TxnResponse);
    fn cpp_get_metrics(metrics: *mut MakoMetrics) -> bool;
    fn cpp_record_txn_retry();

    // Interactive (resumable) transactions: one open Mako transaction that
    // many op lists run inside, so a later op can depend on an earlier one's
    // result. See include/transaction_ffi.h for the locking contract.
    fn cpp_txn_begin(
        keys: *const *const u8,
        key_lens: *const usize,
        num_keys: usize,
    ) -> *mut std::ffi::c_void;
    fn cpp_txn_execute(
        session: *mut std::ffi::c_void,
        request: *const TxnRequest,
        response: *mut TxnResponse,
    ) -> bool;
    fn cpp_txn_commit(session: *mut std::ffi::c_void) -> bool;
    fn cpp_txn_abort(session: *mut std::ffi::c_void);
}

#[cfg(test)]
unsafe fn cpp_worker_thread_init(_thread_id: usize) {}

#[cfg(test)]
unsafe fn cpp_execute_fast_mako_string(
    _op: u32,
    _key_ptr: *const u8,
    _key_len: usize,
    _val_ptr: *const u8,
    _val_len: usize,
    result: *mut FastMakoStringResult,
) -> bool {
    if !result.is_null() {
        (*result).status = FAST_MAKO_FALLBACK;
    }
    true
}

#[cfg(test)]
unsafe fn cpp_execute_transaction(
    _request: *const TxnRequest,
    _response: *mut TxnResponse,
) -> bool {
    false
}

#[cfg(test)]
unsafe fn cpp_free_transaction_response(response: *mut TxnResponse) {
    // Only the session stubs below allocate a response in test builds; the
    // cpp_execute_transaction stub leaves the pointers null, and freeing null
    // is a no-op.
    if response.is_null() {
        return;
    }
    let response = &mut *response;
    if response.results.is_null() {
        return;
    }
    for index in 0..response.num_results {
        let result = &mut *response.results.add(index);
        if !result.data_ptr.is_null() {
            libc::free(result.data_ptr as *mut libc::c_void);
            result.data_ptr = std::ptr::null_mut();
        }
    }
    libc::free(response.results as *mut libc::c_void);
    response.results = std::ptr::null_mut();
    response.num_results = 0;
}

// The interactive transaction FFI, stubbed for unit tests as a string-only
// store with one overlay per session. It is enough to exercise SessionTxn end
// to end: read-your-writes inside a session, a commit that publishes the
// overlay, and an abort that throws it away.
#[cfg(test)]
static TEST_SESSION_STORE: OnceLock<Mutex<HashMap<Vec<u8>, Vec<u8>>>> = OnceLock::new();

#[cfg(test)]
fn test_session_store() -> &'static Mutex<HashMap<Vec<u8>, Vec<u8>>> {
    TEST_SESSION_STORE.get_or_init(|| Mutex::new(HashMap::new()))
}

#[cfg(test)]
struct TestSession {
    /// None is a delete.
    writes: HashMap<Vec<u8>, Option<Vec<u8>>>,
}

#[cfg(test)]
impl TestSession {
    fn read(&self, key: &[u8]) -> Option<Vec<u8>> {
        match self.writes.get(key) {
            Some(buffered) => buffered.clone(),
            None => test_session_store().lock().unwrap().get(key).cloned(),
        }
    }
}

#[cfg(test)]
unsafe fn cpp_txn_begin(
    _keys: *const *const u8,
    _key_lens: *const usize,
    _num_keys: usize,
) -> *mut std::ffi::c_void {
    Box::into_raw(Box::new(TestSession {
        writes: HashMap::new(),
    })) as *mut std::ffi::c_void
}

#[cfg(test)]
unsafe fn cpp_txn_execute(
    session: *mut std::ffi::c_void,
    request: *const TxnRequest,
    response: *mut TxnResponse,
) -> bool {
    if session.is_null() || request.is_null() || response.is_null() {
        return false;
    }
    let session = &mut *(session as *mut TestSession);
    let request = &*request;
    let count = request.num_ops;
    let results =
        libc::malloc(count * std::mem::size_of::<TxnOpResult>()) as *mut TxnOpResult;
    if results.is_null() && count != 0 {
        return false;
    }
    for index in 0..count {
        std::ptr::write(
            results.add(index),
            TxnOpResult {
                success: false,
                value_present: false,
                data_ptr: std::ptr::null_mut(),
                data_len: 0,
                int_value: 0,
            },
        );
    }
    (*response).results = results;
    (*response).num_results = count;
    (*response).transaction_success = true;

    for index in 0..count {
        let op = &*request.ops.add(index);
        let key = std::slice::from_raw_parts(op.key_ptr, op.key_len).to_vec();
        let result = &mut *results.add(index);
        match op.op {
            TXN_OP_SET => {
                let value = std::slice::from_raw_parts(op.val_ptr, op.val_len).to_vec();
                session.writes.insert(key, Some(value));
                result.success = true;
            }
            TXN_OP_GET => {
                result.success = true;
                if let Some(value) = session.read(&key) {
                    result.value_present = true;
                    result.data_len = value.len();
                    if !value.is_empty() {
                        result.data_ptr = libc::malloc(value.len()) as *mut u8;
                        std::ptr::copy_nonoverlapping(
                            value.as_ptr(),
                            result.data_ptr,
                            value.len(),
                        );
                    }
                }
            }
            TXN_OP_DEL => {
                result.success = true;
                result.value_present = session.read(&key).is_some();
                session.writes.insert(key, None);
            }
            _ => {
                (*response).transaction_success = false;
                return false;
            }
        }
    }
    true
}

#[cfg(test)]
unsafe fn cpp_txn_commit(session: *mut std::ffi::c_void) -> bool {
    if session.is_null() {
        return false;
    }
    let session = Box::from_raw(session as *mut TestSession);
    let mut store = test_session_store().lock().unwrap();
    for (key, value) in session.writes {
        match value {
            Some(value) => {
                store.insert(key, value);
            }
            None => {
                store.remove(&key);
            }
        }
    }
    true
}

#[cfg(test)]
unsafe fn cpp_txn_abort(session: *mut std::ffi::c_void) {
    if session.is_null() {
        return;
    }
    drop(Box::from_raw(session as *mut TestSession));
}

#[cfg(test)]
unsafe fn cpp_get_metrics(metrics: *mut MakoMetrics) -> bool {
    if metrics.is_null() {
        return false;
    }
    (*metrics).txn_commits = 11;
    (*metrics).txn_aborts = 2;
    (*metrics).txn_retries = 3;
    (*metrics).uptime_seconds = 42;
    (*metrics).cache_enabled = 0;
    (*metrics).cache_capacity_bytes = 0;
    (*metrics).cache_entries = 0;
    (*metrics).cache_bytes = 0;
    (*metrics).cache_hits = 0;
    (*metrics).cache_misses = 0;
    (*metrics).cache_inserts = 0;
    (*metrics).cache_evictions = 0;
    (*metrics).cache_invalidations = 0;
    true
}

#[cfg(test)]
unsafe fn cpp_record_txn_retry() {}

// ===== OpCode and Command =====

#[derive(Copy, Clone, PartialEq)]
#[repr(u32)]
enum OpCode {
    Get = 1,
    Set = 2,
    Ping = 3,
    Multi = 4,
    Exec = 5,
    Discard = 6,
    Del = 7,
    Hello = 8,
    Client = 9,
    Command = 10,
    Reset = 11,
    Quit = 12,
    Select = 13,
    Auth = 14,
    Echo = 15,
    Info = 16,
    Exists = 17,
    MGet = 18,
    MSet = 19,
    MSetNx = 20,
    GetSet = 21,
    SetNx = 22,
    Append = 23,
    StrLen = 24,
    Incr = 25,
    IncrBy = 26,
    Decr = 27,
    DecrBy = 28,
    IncrByFloat = 29,
    Config = 30,
    Expire = 31,
    PExpire = 32,
    ExpireAt = 33,
    PExpireAt = 34,
    Ttl = 35,
    PTtl = 36,
    Persist = 37,
    Keys = 38,
    Scan = 39,
    DbSize = 40,
    HScan = 41,
    Type = 42,
    Wait = 43,
    SAdd = 44,
    SMembers = 45,
    SIsMember = 46,
    SRem = 47,
    SCard = 48,
    SMove = 49,
    SPop = 50,
    SRandMember = 51,
    SInter = 52,
    SUnion = 53,
    SDiff = 54,
    SInterStore = 55,
    SUnionStore = 56,
    SDiffStore = 57,
    LPush = 58,
    RPush = 59,
    LPop = 60,
    RPop = 61,
    LLen = 62,
    LIndex = 63,
    LRange = 64,
    LSet = 65,
    LRem = 66,
    LTrim = 67,
    LInsert = 68,
    LPushX = 69,
    RPushX = 70,
    LMove = 71,
    RPopLPush = 72,
    LPos = 73,
    ZAdd = 74,
    ZScore = 75,
    ZIncrBy = 76,
    ZRem = 77,
    ZCard = 78,
    ZRange = 79,
    ZRevRange = 80,
    ZRangeByScore = 81,
    ZRevRangeByScore = 139,
    ZRangeByLex = 141,
    ZRevRangeByLex = 142,
    ZLexCount = 143,
    ZRemRangeByScore = 144,
    ZRemRangeByRank = 145,
    ZRemRangeByLex = 146,
    ZRangeStore = 147,
    ZUnionStore = 148,
    ZInterStore = 149,
    ZDiffStore = 150,
    ZUnion = 151,
    ZInter = 152,
    ZDiff = 153,
    ZInterCard = 154,
    ZMPop = 155,
    BZMPop = 156,
    BZPopMin = 157,
    BZPopMax = 158,
    ZMScore = 159,
    ZRandMember = 160,
    ZRank = 82,
    ZRevRank = 83,
    ZCount = 84,
    ZPopMin = 85,
    ZPopMax = 86,
    ZScan = 87,
    Subscribe = 88,
    Unsubscribe = 89,
    PSubscribe = 90,
    PUnsubscribe = 91,
    Publish = 92,
    PubSub = 93,
    SScan = 94,
    SMIsMember = 95,
    SetEx = 96,
    PSetEx = 97,
    Time = 98,
    SInterCard = 99,
    ExpireTime = 100,
    PExpireTime = 101,
    FlushDb = 102,
    FlushAll = 103,
    HSet = 104,
    HSetNx = 105,
    HMSet = 106,
    HGet = 107,
    HMGet = 108,
    HGetAll = 109,
    HDel = 110,
    HExists = 111,
    HLen = 112,
    HKeys = 113,
    HVals = 114,
    HStrLen = 115,
    HIncrBy = 116,
    HIncrByFloat = 117,
    HRandField = 118,
    Memory = 119,
    Watch = 120,
    Unwatch = 121,
    GetEx = 122,
    GetDel = 123,
    SetBit = 124,
    GetBit = 125,
    SetRange = 126,
    GetRange = 127,
    Lcs = 128,
    Dump = 129,
    Restore = 130,
    Copy = 161,
    Script = 162,
    Eval = 163,
    Forbidden = 164,
    RandomKey = 165,
    BLPop = 131,
    BRPop = 132,
    BLMPop = 133,
    BRPopLPush = 134,
    BLMove = 135,
    Rename = 136,
    Sort = 137,
    LMPop = 138,
    RenameNx = 140,
    Touch = 166,
    SortRo = 167,
    BitCount = 168,
    BitPos = 169,
    BitFieldRo = 170,
    SlowLog = 171,
    Latency = 172,
    Object = 173,
    Acl = 174,
    SSubscribe = 175,
    SUnsubscribe = 176,
    BitOp = 177,
    PfAdd = 178,
    PfCount = 179,
    PfMerge = 180,
    BitField = 181,
    GeoAdd = 182,
    GeoPos = 183,
    GeoDist = 184,
    GeoHash = 185,
    // One opcode for GEOSEARCH, GEORADIUS, GEORADIUSBYMEMBER and the two _RO
    // spellings: they differ only in argument syntax, and the shape they search
    // with plus the requested output columns live in `Command::geo`.
    GeoSearch = 186,
    // Redis 7.4 hash field expiration. One opcode per command: they differ in
    // the time unit, in whether the argument is relative or absolute, and in
    // how the reply is formatted, and keeping them apart lets the reply arm
    // convert without a second field on `Command`. HEXPIRE/HPEXPIRE/HEXPIREAT/
    // HPEXPIREAT all build TXN_OP_HFIELD_EXPIRE, HTTL/HPTTL/HEXPIRETIME/
    // HPEXPIRETIME all build TXN_OP_HFIELD_TTL, HPERSIST builds
    // TXN_OP_HFIELD_PERSIST.
    HExpire = 187,
    HPExpire = 188,
    HExpireAt = 189,
    HPExpireAt = 190,
    HTtl = 191,
    HPTtl = 192,
    HExpireTime = 193,
    HPExpireTime = 194,
    HPersist = 195,
    // Single-node CLUSTER emulation. None of the three is a storage op: they
    // answer from process configuration alone, so they are local arms in
    // `handle_command` the way SLOWLOG is.
    Cluster = 196,
    ReadOnly = 197,
    ReadWrite = 198,
    // MONITOR. Local like the three above: it changes only this connection's
    // membership in the monitor registry.
    Monitor = 199,
    // MOVE key db. A storage op: one executor op copies the object to the
    // destination database's spelling of the name and deletes the source.
    Move = 200,
    EvalSha = 201,
    EvalRo = 202,
    EvalShaRo = 203,
    // Redis Streams. XRANGE and XREVRANGE are separate opcodes because the
    // reply is the same but the argument order is not, and XREAD is separate
    // because it reads several streams and can block.
    XAdd = 204,
    XRange = 205,
    XRevRange = 206,
    XLen = 207,
    XDel = 208,
    XTrim = 209,
    XSetId = 210,
    XInfo = 211,
    XRead = 212,
    XGroup = 213,
    XReadGroup = 214,
    XAck = 215,
    XPending = 216,
    XClaim = 217,
    XAutoClaim = 218,
    GeoSearchStore = 219,
    Module = 220,
}

#[derive(Copy, Clone, PartialEq)]
enum SetCondition {
    None,
    Nx,
    Xx,
}

#[derive(Clone)]
struct Command {
    op: OpCode,
    keys: Vec<Bytes>,
    val: Option<Bytes>,
    values: Vec<Bytes>,
    args: Vec<Bytes>,
    set_condition: SetCondition,
    set_return_old: bool,
    set_integer_reply: bool,
    set_keep_ttl: bool,
    expire_at_ms: i64,
    expire_flags: u32,
    scan_count: i64,
    scan_prefix: Bytes,
    scan_type_matches: bool,
    set_count: Option<i64>,
    // RESTORE payload kind: 0 list, 1 hash, 2 string, 3 set, 4 zset.
    restore_kind: u8,
    // GEOSEARCH/GEORADIUS/GEORADIUSBYMEMBER search shape and output columns.
    // Boxed so the common commands keep `Command` small.
    geo: Option<Box<GeoSearchSpec>>,
}

/// Result ordering requested by a geo search.
#[derive(Clone, Copy, PartialEq)]
enum GeoSort {
    None,
    Asc,
    Desc,
}

/// Everything a geo search needs after parsing. The center is in degrees;
/// radius, width and height are in meters, already converted from the unit the
/// client asked for. `unit_meters` is kept so WITHDIST can convert back.
#[derive(Clone)]
struct GeoSearchSpec {
    center_lon: f64,
    center_lat: f64,
    /// FROMMEMBER / GEORADIUSBYMEMBER: the member whose position is the center.
    /// It is resolved by its own request before the range reads are built,
    /// because a later op cannot read an earlier op's result in one request.
    from_member: Option<Bytes>,
    /// Why the FROMMEMBER lookup could not produce a center, if it could not.
    resolve_error: GeoResolve,
    /// BYRADIUS (true) or BYBOX (false).
    circular: bool,
    radius_m: f64,
    width_m: f64,
    height_m: f64,
    unit_meters: f64,
    sort: GeoSort,
    count: Option<usize>,
    any: bool,
    withcoord: bool,
    withdist: bool,
    withhash: bool,
    /// GEOSEARCHSTORE / GEORADIUS[BYMEMBER] STORE|STOREDIST: the score written
    /// to the destination is the distance in `unit_meters` rather than the
    /// member's own geohash. The destination key itself is `cmd.keys[0]` and
    /// the source is `cmd.values[0]`, as ZRANGESTORE spells it.
    store_dist: bool,
}

/// Outcome of the FROMMEMBER center lookup.
#[derive(Clone, Copy, PartialEq)]
enum GeoResolve {
    Ok,
    /// The key does not exist at all: Redis answers an empty result.
    KeyMissing,
    /// The key exists but holds no such member: Redis answers an error.
    MemberMissing,
    WrongType,
    Failed,
}

impl Command {
    fn new(op: OpCode, keys: Vec<Bytes>, val: Option<Bytes>, args: Vec<Bytes>) -> Self {
        Command {
            op,
            keys,
            val,
            values: Vec::new(),
            args,
            set_condition: SetCondition::None,
            set_return_old: false,
            set_integer_reply: false,
            set_keep_ttl: false,
            expire_at_ms: -1,
            expire_flags: 0,
            scan_count: 10,
            scan_prefix: Bytes::new(),
            scan_type_matches: true,
            set_count: None,
            restore_kind: 0,
            geo: None,
        }
    }
}

enum ParseError {
    Protocol(&'static str),
    Error(&'static str),
    /// Redis-style `ERR ...` text that has to be built at parse time, such as
    /// GEOADD's "invalid longitude,latitude pair %f,%f".
    Owned(String),
    UnknownCommand { name: Bytes, args: Vec<Bytes> },
    WrongArity { command: &'static str },
}

// ===== Transaction State =====

/// Per-connection transaction state
struct TransactionState {
    in_multi: bool,
    queue_error: bool,
    queued_commands: Vec<Command>,
    watched_versions: HashMap<Bytes, usize>,
}

impl TransactionState {
    fn new() -> Self {
        TransactionState {
            in_multi: false,
            queue_error: false,
            queued_commands: Vec::new(),
            watched_versions: HashMap::new(),
        }
    }

    fn start_multi(&mut self) {
        self.in_multi = true;
        self.queue_error = false;
        self.queued_commands.clear();
    }

    fn queue_command(&mut self, cmd: Command) {
        self.queued_commands.push(cmd);
    }

    fn discard(&mut self) {
        remove_watched_existing_keys(self.watched_versions.keys());
        self.in_multi = false;
        self.queue_error = false;
        self.queued_commands.clear();
        self.watched_versions.clear();
    }

    fn mark_queue_error(&mut self) {
        self.queue_error = true;
    }

    fn has_queue_error(&self) -> bool {
        self.queue_error
    }

    fn take_commands(&mut self) -> Vec<Command> {
        self.in_multi = false;
        self.queue_error = false;
        remove_watched_existing_keys(self.watched_versions.keys());
        self.watched_versions.clear();
        std::mem::take(&mut self.queued_commands)
    }

    fn watch_keys(&mut self, keys: &[Bytes]) {
        for key in keys {
            self.watched_versions
                .insert(key.clone(), current_key_version(key));
            if key_exists_now(key) {
                if let Ok(mut watched) = watched_existing_keys().lock() {
                    watched.insert(key.clone());
                }
            }
        }
    }

    fn unwatch(&mut self) {
        remove_watched_existing_keys(self.watched_versions.keys());
        self.watched_versions.clear();
    }

    fn watched_keys_dirty(&self) -> bool {
        self.watched_versions
            .iter()
            .any(|(key, version)| current_key_version(key) != *version)
    }
}

// ===== Client State =====

struct WorkerWake {
    reader: UnixStream,
    writer: UnixStream,
}

impl WorkerWake {
    fn new() -> std::io::Result<Self> {
        let (reader, writer) = UnixStream::pair()?;
        reader.set_nonblocking(true)?;
        writer.set_nonblocking(true)?;
        Ok(Self { reader, writer })
    }

    fn notify(&self) {
        let mut writer = &self.writer;
        match writer.write(&[1]) {
            Ok(_) => {}
            Err(error) if error.kind() == ErrorKind::WouldBlock => {}
            Err(error) => eprintln!("Worker wake notification failed: {error}"),
        }
    }

    fn drain(&self) {
        let mut buffer = [0u8; 64];
        let mut reader = &self.reader;
        loop {
            match reader.read(&mut buffer) {
                Ok(0) => break,
                Ok(_) => {}
                Err(error) if error.kind() == ErrorKind::WouldBlock => break,
                Err(error) => {
                    eprintln!("Worker wake drain failed: {error}");
                    break;
                }
            }
        }
    }
}

/// Blocked commands fall into two classes, and only one of them needs a queue.
/// A list or sorted-set pop hands one element to exactly one waiter, so those
/// take their turns; a stream read takes nothing away, so several readers can
/// be served from the same entries. A key holds one type at a time, so the two
/// classes never compete for the same data -- and queueing them together lets a
/// pop that can never be served (the key became a stream) hold the front
/// forever and starve every reader behind it.
fn blocked_command_class(op: OpCode) -> u8 {
    match op {
        OpCode::XRead | OpCode::XReadGroup => 1,
        _ => 0,
    }
}

#[derive(Default)]
struct BlockedClientRegistry {
    key_queues: HashMap<Bytes, VecDeque<usize>>,
    client_keys: HashMap<usize, Vec<Bytes>>,
    client_class: HashMap<usize, u8>,
    /// The blocked clients whose command needs the key to exist before it can
    /// be served, rather than merely to hold something. XREADGROUP is the only
    /// one: a missing key or group answers NOGROUP instead of waiting. Redis
    /// counts the keys they wait on separately, in INFO clients.
    nokey_clients: HashSet<usize>,
}

impl BlockedClientRegistry {
    fn register(&mut self, client_id: usize, cmd: &Command) {
        self.unregister(client_id);
        self.client_class
            .insert(client_id, blocked_command_class(cmd.op));
        if cmd.op == OpCode::XReadGroup {
            self.nokey_clients.insert(client_id);
        }
        let mut seen = HashSet::new();
        let keys: Vec<Bytes> = cmd
            .keys
            .iter()
            .filter(|key| seen.insert((*key).clone()))
            .cloned()
            .collect();
        for key in &keys {
            self.key_queues
                .entry(key.clone())
                .or_default()
                .push_back(client_id);
        }
        self.client_keys.insert(client_id, keys);
    }

    fn unregister(&mut self, client_id: usize) {
        self.nokey_clients.remove(&client_id);
        self.client_class.remove(&client_id);
        let Some(keys) = self.client_keys.remove(&client_id) else {
            return;
        };
        for key in keys {
            let remove_queue = if let Some(queue) = self.key_queues.get_mut(&key) {
                queue.retain(|queued_id| *queued_id != client_id);
                queue.is_empty()
            } else {
                false
            };
            if remove_queue {
                self.key_queues.remove(&key);
            }
        }
    }

    fn class_of(&self, client_id: usize) -> u8 {
        self.client_class.get(&client_id).copied().unwrap_or(0)
    }

    /// The first client of `class` waiting on `key`, which is the one whose
    /// turn it is among the waiters it actually competes with.
    fn front_of_class(&self, key: &Bytes, class: u8) -> Option<usize> {
        self.key_queues
            .get(key)?
            .iter()
            .copied()
            .find(|client_id| self.class_of(*client_id) == class)
    }

    /// How many keys have a blocked client waiting on them, and how many of
    /// those have one waiting for the key itself to appear.
    fn blocking_key_counts(&self) -> (usize, usize) {
        let on_nokey = self
            .key_queues
            .iter()
            .filter(|(_, queue)| {
                queue
                    .iter()
                    .any(|client_id| self.nokey_clients.contains(client_id))
            })
            .count();
        (self.key_queues.len(), on_nokey)
    }

    fn has_turn(&self, client_id: usize) -> bool {
        let Some(keys) = self.client_keys.get(&client_id) else {
            return true;
        };
        let class = self.class_of(client_id);
        keys.is_empty()
            || keys
                .iter()
                .any(|key| self.front_of_class(key, class) == Some(client_id))
    }

    /// One entry per key and class, so a writer waits for every kind of waiter
    /// on the key it just changed, not only the first one registered.
    fn fronts_for_keys(&self, keys: &[Bytes]) -> Vec<(Bytes, u8, usize)> {
        let mut seen = HashSet::new();
        let mut fronts = Vec::new();
        for key in keys {
            if !seen.insert(key.clone()) {
                continue;
            }
            for class in [0u8, 1u8] {
                if let Some(front) = self.front_of_class(key, class) {
                    fronts.push((key.clone(), class, front));
                }
            }
        }
        fronts
    }

    fn eligible_keys(&self, client_id: usize, keys: &[Bytes]) -> Vec<Bytes> {
        let class = self.class_of(client_id);
        keys.iter()
            .filter(|key| {
                self.front_of_class(key, class)
                    .is_none_or(|front| front == client_id)
            })
            .cloned()
            .collect()
    }

    fn fronts_changed(&self, expected: &[(Bytes, u8, usize)]) -> bool {
        expected.iter().all(|(key, class, client_id)| {
            self.front_of_class(key, *class)
                .is_none_or(|current| current != *client_id)
        })
    }
}

fn blocked_registry() -> &'static (Mutex<BlockedClientRegistry>, Condvar) {
    BLOCKED_REGISTRY.get_or_init(|| (Mutex::new(BlockedClientRegistry::default()), Condvar::new()))
}

fn register_blocked_client(client_id: usize, cmd: &Command) {
    let (registry, _) = blocked_registry();
    if let Ok(mut registry) = registry.lock() {
        registry.register(client_id, cmd);
    }
}

fn unregister_blocked_client(client_id: usize) {
    let (registry, changed) = blocked_registry();
    if let Ok(mut registry) = registry.lock() {
        registry.unregister(client_id);
        changed.notify_all();
    }
}

fn blocking_key_counts() -> (usize, usize) {
    let (registry, _) = blocked_registry();
    registry
        .lock()
        .map(|registry| registry.blocking_key_counts())
        .unwrap_or((0, 0))
}

fn blocked_client_has_turn(client_id: usize) -> bool {
    let (registry, _) = blocked_registry();
    registry
        .lock()
        .map(|registry| registry.has_turn(client_id))
        .unwrap_or(true)
}

fn eligible_blocked_keys(client_id: usize, keys: &[Bytes]) -> Vec<Bytes> {
    let (registry, _) = blocked_registry();
    registry
        .lock()
        .map(|registry| registry.eligible_keys(client_id, keys))
        .unwrap_or_else(|_| keys.to_vec())
}

fn blocked_fronts_for_keys(keys: &[Bytes]) -> Vec<(Bytes, u8, usize)> {
    let (registry, _) = blocked_registry();
    registry
        .lock()
        .map(|registry| registry.fronts_for_keys(keys))
        .unwrap_or_default()
}

fn wait_for_blocked_fronts(expected: &[(Bytes, u8, usize)], timeout: Duration) {
    if expected.is_empty() {
        return;
    }
    let deadline = Instant::now() + timeout;
    let (registry, changed) = blocked_registry();
    let Ok(mut registry) = registry.lock() else {
        return;
    };
    while !registry.fronts_changed(expected) {
        let Some(remaining) = deadline.checked_duration_since(Instant::now()) else {
            break;
        };
        let Ok((next_registry, result)) = changed.wait_timeout(registry, remaining) else {
            break;
        };
        registry = next_registry;
        if result.timed_out() {
            break;
        }
    }
}

fn notify_worker_wakes(wakes: &[Weak<WorkerWake>]) {
    for wake in wakes {
        if let Some(wake) = wake.upgrade() {
            wake.notify();
        }
    }
}

fn notify_all_workers() {
    if let Some(wakes) = WORKER_WAKES.get() {
        notify_worker_wakes(wakes);
    }
}

type PubSubQueue = Arc<Mutex<VecDeque<Vec<u8>>>>;
type PubSubQueueWeak = Weak<Mutex<VecDeque<Vec<u8>>>>;

#[derive(Clone)]
struct PubSubTarget {
    client_id: usize,
    queue: PubSubQueueWeak,
    worker_wake: Option<Weak<WorkerWake>>,
    // True when registered through SSUBSCRIBE; deliveries use "smessage".
    sharded: bool,
}

struct PubSubRegistry {
    channels: HashMap<Bytes, Vec<PubSubTarget>>,
    patterns: HashMap<Bytes, Vec<PubSubTarget>>,
}

impl PubSubRegistry {
    fn new() -> Self {
        PubSubRegistry {
            channels: HashMap::new(),
            patterns: HashMap::new(),
        }
    }

    fn prune_dead(&mut self) {
        self.channels.retain(|_, targets| {
            targets.retain(|target| target.queue.strong_count() > 0);
            !targets.is_empty()
        });
        self.patterns.retain(|_, targets| {
            targets.retain(|target| target.queue.strong_count() > 0);
            !targets.is_empty()
        });
    }
}

fn pubsub_registry() -> &'static Mutex<PubSubRegistry> {
    PUBSUB_REGISTRY.get_or_init(|| Mutex::new(PubSubRegistry::new()))
}

// ===== MONITOR =====
//
// A monitor is a connection that has asked to see every command any client
// runs on this server. It is delivered exactly like a Pub/Sub message: the
// registry holds one `PubSubTarget` per monitor, carrying a weak handle on
// that connection's reply queue and on its worker's wake pipe, so a monitor
// parked on another worker thread is woken the moment a line is queued for it.
//
// `MONITOR_COUNT` is the fast gate. It is the number of registered monitors
// and is read with one relaxed load at the entry of the raw GET/SET fast
// frame path; while it is zero, nothing about that path changes, and while it
// is non-zero the path declines every frame so the general parser sees, and
// reports, the command.

static MONITOR_REGISTRY: OnceLock<Mutex<Vec<PubSubTarget>>> = OnceLock::new();
static MONITOR_COUNT: AtomicUsize = AtomicUsize::new(0);

fn monitor_registry() -> &'static Mutex<Vec<PubSubTarget>> {
    MONITOR_REGISTRY.get_or_init(|| Mutex::new(Vec::new()))
}

#[inline]
fn monitor_count() -> usize {
    MONITOR_COUNT.load(Ordering::Relaxed)
}

/// Returns false when this connection was already a monitor, which Redis
/// answers with a plain OK all the same.
fn register_monitor(client_state: &mut ClientState) -> bool {
    if client_state.monitoring {
        return false;
    }
    let target = make_pubsub_target(client_state);
    let registered = if let Ok(mut monitors) = monitor_registry().lock() {
        monitors.retain(|monitor| monitor.queue.strong_count() > 0);
        monitors.push(target);
        MONITOR_COUNT.store(monitors.len(), Ordering::Relaxed);
        true
    } else {
        false
    };
    if registered {
        client_state.monitoring = true;
        // A worker parked in poll() has to notice that the fast path is now
        // closed before its clients' next frame.
        notify_all_workers();
    }
    registered
}

fn unregister_monitor(client_state: &mut ClientState) {
    if !client_state.monitoring {
        return;
    }
    client_state.monitoring = false;
    if let Ok(mut monitors) = monitor_registry().lock() {
        monitors.retain(|monitor| {
            monitor.queue.strong_count() > 0 && monitor.client_id != client_state.id
        });
        MONITOR_COUNT.store(monitors.len(), Ordering::Relaxed);
    }
}

/// Everything a connection has to give up when it goes away, resets, or quits.
fn unregister_all_client_feeds(client_state: &mut ClientState) {
    unregister_all_pubsub(client_state);
    unregister_monitor(client_state);
}

/// Redis `sdscatrepr`: the argument in double quotes, with `"` and `\`
/// backslash-escaped, the five control characters Redis names spelled out, and
/// every other non-printable byte as `\xHH`. Bytes above 0x7e are not printable
/// in Redis's C locale, so they are escaped too.
fn monitor_quote_arg(out: &mut String, arg: &[u8]) {
    out.push('"');
    for byte in arg {
        match *byte {
            b'\\' => out.push_str("\\\\"),
            b'"' => out.push_str("\\\""),
            b'\n' => out.push_str("\\n"),
            b'\r' => out.push_str("\\r"),
            b'\t' => out.push_str("\\t"),
            0x07 => out.push_str("\\a"),
            0x08 => out.push_str("\\b"),
            0x20..=0x7e => out.push(*byte as char),
            other => out.push_str(&format!("\\x{other:02x}")),
        }
    }
    out.push('"');
}

/// One monitor line: `+<sec>.<usec> [<db> <ip>:<port>] "CMD" "arg"...`, the
/// shape Redis builds in `replicationFeedMonitors`. The db is 0 because this
/// server has one keyspace; package 6 adds SELECT and would pass the
/// connection's selected db in here instead.
fn format_monitor_line(db: u32, peer: &str, argv: &[Bytes]) -> Vec<u8> {
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default();
    let mut line = String::with_capacity(64 + argv.len() * 16);
    line.push('+');
    line.push_str(&now.as_secs().to_string());
    line.push('.');
    line.push_str(&format!("{:06}", now.subsec_micros()));
    line.push_str(" [");
    line.push_str(&db.to_string());
    line.push(' ');
    line.push_str(peer);
    line.push_str("] ");
    for (index, arg) in argv.iter().enumerate() {
        if index > 0 {
            line.push(' ');
        }
        monitor_quote_arg(&mut line, arg);
    }
    line.push_str("\r\n");
    line.into_bytes()
}

/// Redis `redactClientCommandArgument`: a password must not reach a monitor.
/// Returns a rewritten argv, or None when there is nothing to hide. Redis also
/// redacts MIGRATE's AUTH/AUTH2 arguments; MIGRATE is not offered here.
fn monitor_redact_argv(argv: &[Bytes]) -> Option<Vec<Bytes>> {
    const REDACTED: Bytes = Bytes::from_static(b"(redacted)");
    let name = argv.first()?.as_ref();
    if ascii_eq_ci(name, b"AUTH") {
        let mut out = Vec::with_capacity(argv.len());
        out.push(argv[0].clone());
        out.resize(argv.len(), REDACTED);
        return Some(out);
    }
    if ascii_eq_ci(name, b"HELLO") {
        // HELLO [protover [AUTH username password] [SETNAME name]].
        let position = argv
            .iter()
            .skip(2)
            .position(|arg| ascii_eq_ci(arg.as_ref(), b"AUTH"))
            .map(|offset| offset + 2)?;
        let mut out = argv.to_vec();
        for slot in out.iter_mut().skip(position + 1).take(2) {
            *slot = REDACTED;
        }
        return Some(out);
    }
    None
}

/// Report one command to every attached monitor except the client that issued
/// it. Called before the command runs, so the line is out even if the command
/// goes on to fail, exactly as in Redis.
fn feed_monitors(client_state: &ClientState, argv: &[Bytes]) {
    if argv.is_empty() || monitor_count() == 0 {
        return;
    }
    let redacted = monitor_redact_argv(argv);
    let argv = redacted.as_deref().unwrap_or(argv);
    let targets: Vec<PubSubTarget> = {
        let Ok(monitors) = monitor_registry().lock() else {
            return;
        };
        monitors
            .iter()
            .filter(|monitor| monitor.client_id != client_state.id)
            .cloned()
            .collect()
    };
    if targets.is_empty() {
        return;
    }
    let line = format_monitor_line(client_state.db as u32, &client_state.peer_addr, argv);
    for target in &targets {
        enqueue_pubsub_reply(target, &line);
    }
}

/// The argv a monitor should see, taken from the frame before it is parsed so
/// the line carries what the client actually sent. Only built while a monitor
/// is attached.
fn monitor_argv_from_frame(frame: &DecodedFrame<BytesFrame>) -> Option<Vec<Bytes>> {
    let DecodedFrame::Complete(BytesFrame::Array { data, .. }) = frame else {
        return None;
    };
    let mut argv = Vec::with_capacity(data.len());
    for part in data.iter() {
        argv.push(frame_to_bytes(part)?);
    }
    if argv.is_empty() {
        None
    } else {
        Some(argv)
    }
}

fn unblock_requests() -> &'static Mutex<HashMap<usize, bool>> {
    UNBLOCK_REQUESTS.get_or_init(|| Mutex::new(HashMap::new()))
}

fn request_client_unblock(id: usize, error: bool) {
    let inserted = if let Ok(mut requests) = unblock_requests().lock() {
        requests.insert(id, error);
        true
    } else {
        false
    };
    if inserted {
        notify_all_workers();
    }
}

fn take_client_unblock(id: usize) -> Option<bool> {
    unblock_requests()
        .lock()
        .ok()
        .and_then(|mut requests| requests.remove(&id))
}

/// Per-connection client metadata for Redis handshake and Pub/Sub commands.
struct ClientState {
    id: usize,
    protocol_version: u8,
    name: Option<Bytes>,
    close_after_reply: bool,
    blocked: bool,
    subscribed_channels: HashSet<Bytes>,
    subscribed_patterns: HashSet<Bytes>,
    subscribed_shard_channels: HashSet<Bytes>,
    pubsub_queue: PubSubQueue,
    worker_wake: Option<Weak<WorkerWake>>,
    /// True while this connection is in MONITOR mode.
    monitoring: bool,
    /// The peer address recorded when the connection was accepted, printed in
    /// every monitor line this client causes.
    peer_addr: String,
    /// The logical database this connection selected, 0..15. This is the
    /// authoritative copy; `CURRENT_DB` is the worker thread's view of it.
    db: u8,
}

impl ClientState {
    #[cfg(test)]
    fn new() -> Self {
        Self::new_with_worker_wake(None)
    }

    fn for_worker(worker_wake: &Arc<WorkerWake>) -> Self {
        Self::new_with_worker_wake(Some(Arc::downgrade(worker_wake)))
    }

    fn new_with_worker_wake(worker_wake: Option<Weak<WorkerWake>>) -> Self {
        ClientState {
            id: NEXT_CLIENT_ID.fetch_add(1, Ordering::Relaxed),
            protocol_version: 2,
            name: None,
            close_after_reply: false,
            blocked: false,
            subscribed_channels: HashSet::new(),
            subscribed_patterns: HashSet::new(),
            subscribed_shard_channels: HashSet::new(),
            pubsub_queue: Arc::new(Mutex::new(VecDeque::new())),
            worker_wake,
            monitoring: false,
            peer_addr: String::from("127.0.0.1:0"),
            db: 0,
        }
    }

    fn reset(&mut self) {
        self.protocol_version = 2;
        self.name = None;
        // RESET puts the connection back on database 0, as Redis does.
        self.db = 0;
        self.close_after_reply = false;
        self.blocked = false;
        self.subscribed_channels.clear();
        self.subscribed_patterns.clear();
        if let Ok(mut queue) = self.pubsub_queue.lock() {
            queue.clear();
        }
    }

    fn subscription_count(&self) -> usize {
        self.subscribed_channels.len()
            + self.subscribed_patterns.len()
            + self.subscribed_shard_channels.len()
    }

    fn in_subscriber_mode(&self) -> bool {
        self.subscription_count() > 0
    }
}

// ===== Helpers =====

#[inline]
fn ascii_eq_ci(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    for (x, y) in a.iter().zip(b.iter()) {
        if x.to_ascii_lowercase() != y.to_ascii_lowercase() {
            return false;
        }
    }
    true
}

#[inline]
fn parse_opcode(name: &[u8]) -> Option<OpCode> {
    if ascii_eq_ci(name, b"GET") {
        Some(OpCode::Get)
    } else if ascii_eq_ci(name, b"GETEX") {
        Some(OpCode::GetEx)
    } else if ascii_eq_ci(name, b"GETDEL") {
        Some(OpCode::GetDel)
    } else if ascii_eq_ci(name, b"SET") {
        Some(OpCode::Set)
    } else if ascii_eq_ci(name, b"SETEX") {
        Some(OpCode::SetEx)
    } else if ascii_eq_ci(name, b"PSETEX") {
        Some(OpCode::PSetEx)
    } else if ascii_eq_ci(name, b"MGET") {
        Some(OpCode::MGet)
    } else if ascii_eq_ci(name, b"MSET") {
        Some(OpCode::MSet)
    } else if ascii_eq_ci(name, b"MSETNX") {
        Some(OpCode::MSetNx)
    } else if ascii_eq_ci(name, b"GETSET") {
        Some(OpCode::GetSet)
    } else if ascii_eq_ci(name, b"SETNX") {
        Some(OpCode::SetNx)
    } else if ascii_eq_ci(name, b"APPEND") {
        Some(OpCode::Append)
    } else if ascii_eq_ci(name, b"STRLEN") {
        Some(OpCode::StrLen)
    } else if ascii_eq_ci(name, b"SETBIT") {
        Some(OpCode::SetBit)
    } else if ascii_eq_ci(name, b"GETBIT") {
        Some(OpCode::GetBit)
    } else if ascii_eq_ci(name, b"SETRANGE") {
        Some(OpCode::SetRange)
    } else if ascii_eq_ci(name, b"GETRANGE") || ascii_eq_ci(name, b"SUBSTR") {
        Some(OpCode::GetRange)
    } else if ascii_eq_ci(name, b"LCS") {
        Some(OpCode::Lcs)
    } else if ascii_eq_ci(name, b"DUMP") {
        Some(OpCode::Dump)
    } else if ascii_eq_ci(name, b"RESTORE") {
        Some(OpCode::Restore)
    } else if ascii_eq_ci(name, b"COPY") {
        Some(OpCode::Copy)
    } else if ascii_eq_ci(name, b"MOVE") {
        Some(OpCode::Move)
    } else if ascii_eq_ci(name, b"SCRIPT") {
        Some(OpCode::Script)
    } else if ascii_eq_ci(name, b"EVAL") {
        Some(OpCode::Eval)
    } else if ascii_eq_ci(name, b"EVALSHA") {
        Some(OpCode::EvalSha)
    } else if ascii_eq_ci(name, b"EVAL_RO") {
        Some(OpCode::EvalRo)
    } else if ascii_eq_ci(name, b"EVALSHA_RO") {
        Some(OpCode::EvalShaRo)
    } else if ascii_eq_ci(name, b"SAVE") || ascii_eq_ci(name, b"SHUTDOWN") {
        Some(OpCode::Forbidden)
    } else if ascii_eq_ci(name, b"RANDOMKEY") {
        Some(OpCode::RandomKey)
    } else if ascii_eq_ci(name, b"RENAME") {
        Some(OpCode::Rename)
    } else if ascii_eq_ci(name, b"RENAMENX") {
        Some(OpCode::RenameNx)
    } else if ascii_eq_ci(name, b"SORT") {
        Some(OpCode::Sort)
    } else if ascii_eq_ci(name, b"BLPOP") {
        Some(OpCode::BLPop)
    } else if ascii_eq_ci(name, b"BRPOP") {
        Some(OpCode::BRPop)
    } else if ascii_eq_ci(name, b"BLMPOP") {
        Some(OpCode::BLMPop)
    } else if ascii_eq_ci(name, b"LMPOP") {
        Some(OpCode::LMPop)
    } else if ascii_eq_ci(name, b"INCR") {
        Some(OpCode::Incr)
    } else if ascii_eq_ci(name, b"INCRBY") {
        Some(OpCode::IncrBy)
    } else if ascii_eq_ci(name, b"DECR") {
        Some(OpCode::Decr)
    } else if ascii_eq_ci(name, b"DECRBY") {
        Some(OpCode::DecrBy)
    } else if ascii_eq_ci(name, b"INCRBYFLOAT") {
        Some(OpCode::IncrByFloat)
    } else if ascii_eq_ci(name, b"EXPIRE") {
        Some(OpCode::Expire)
    } else if ascii_eq_ci(name, b"PEXPIRE") {
        Some(OpCode::PExpire)
    } else if ascii_eq_ci(name, b"EXPIREAT") {
        Some(OpCode::ExpireAt)
    } else if ascii_eq_ci(name, b"PEXPIREAT") {
        Some(OpCode::PExpireAt)
    } else if ascii_eq_ci(name, b"TTL") {
        Some(OpCode::Ttl)
    } else if ascii_eq_ci(name, b"PTTL") {
        Some(OpCode::PTtl)
    } else if ascii_eq_ci(name, b"EXPIRETIME") {
        Some(OpCode::ExpireTime)
    } else if ascii_eq_ci(name, b"PEXPIRETIME") {
        Some(OpCode::PExpireTime)
    } else if ascii_eq_ci(name, b"PERSIST") {
        Some(OpCode::Persist)
    } else if ascii_eq_ci(name, b"KEYS") {
        Some(OpCode::Keys)
    } else if ascii_eq_ci(name, b"SCAN") {
        Some(OpCode::Scan)
    } else if ascii_eq_ci(name, b"DBSIZE") {
        Some(OpCode::DbSize)
    } else if ascii_eq_ci(name, b"FLUSHDB") {
        Some(OpCode::FlushDb)
    } else if ascii_eq_ci(name, b"FLUSHALL") {
        Some(OpCode::FlushAll)
    } else if ascii_eq_ci(name, b"HSET") {
        Some(OpCode::HSet)
    } else if ascii_eq_ci(name, b"HSETNX") {
        Some(OpCode::HSetNx)
    } else if ascii_eq_ci(name, b"HMSET") {
        Some(OpCode::HMSet)
    } else if ascii_eq_ci(name, b"HGET") {
        Some(OpCode::HGet)
    } else if ascii_eq_ci(name, b"HMGET") {
        Some(OpCode::HMGet)
    } else if ascii_eq_ci(name, b"HGETALL") {
        Some(OpCode::HGetAll)
    } else if ascii_eq_ci(name, b"HDEL") {
        Some(OpCode::HDel)
    } else if ascii_eq_ci(name, b"HEXISTS") {
        Some(OpCode::HExists)
    } else if ascii_eq_ci(name, b"HLEN") {
        Some(OpCode::HLen)
    } else if ascii_eq_ci(name, b"HKEYS") {
        Some(OpCode::HKeys)
    } else if ascii_eq_ci(name, b"HVALS") {
        Some(OpCode::HVals)
    } else if ascii_eq_ci(name, b"HSTRLEN") {
        Some(OpCode::HStrLen)
    } else if ascii_eq_ci(name, b"HINCRBY") {
        Some(OpCode::HIncrBy)
    } else if ascii_eq_ci(name, b"HINCRBYFLOAT") {
        Some(OpCode::HIncrByFloat)
    } else if ascii_eq_ci(name, b"HRANDFIELD") {
        Some(OpCode::HRandField)
    } else if ascii_eq_ci(name, b"HSCAN") {
        Some(OpCode::HScan)
    } else if ascii_eq_ci(name, b"HEXPIRE") {
        Some(OpCode::HExpire)
    } else if ascii_eq_ci(name, b"HPEXPIRE") {
        Some(OpCode::HPExpire)
    } else if ascii_eq_ci(name, b"HEXPIREAT") {
        Some(OpCode::HExpireAt)
    } else if ascii_eq_ci(name, b"HPEXPIREAT") {
        Some(OpCode::HPExpireAt)
    } else if ascii_eq_ci(name, b"HTTL") {
        Some(OpCode::HTtl)
    } else if ascii_eq_ci(name, b"HPTTL") {
        Some(OpCode::HPTtl)
    } else if ascii_eq_ci(name, b"HEXPIRETIME") {
        Some(OpCode::HExpireTime)
    } else if ascii_eq_ci(name, b"HPEXPIRETIME") {
        Some(OpCode::HPExpireTime)
    } else if ascii_eq_ci(name, b"HPERSIST") {
        Some(OpCode::HPersist)
    } else if ascii_eq_ci(name, b"TYPE") {
        Some(OpCode::Type)
    } else if ascii_eq_ci(name, b"WAIT") {
        Some(OpCode::Wait)
    } else if ascii_eq_ci(name, b"TIME") {
        Some(OpCode::Time)
    } else if ascii_eq_ci(name, b"SADD") {
        Some(OpCode::SAdd)
    } else if ascii_eq_ci(name, b"SMEMBERS") {
        Some(OpCode::SMembers)
    } else if ascii_eq_ci(name, b"SISMEMBER") {
        Some(OpCode::SIsMember)
    } else if ascii_eq_ci(name, b"SMISMEMBER") {
        Some(OpCode::SMIsMember)
    } else if ascii_eq_ci(name, b"SINTERCARD") {
        Some(OpCode::SInterCard)
    } else if ascii_eq_ci(name, b"SREM") {
        Some(OpCode::SRem)
    } else if ascii_eq_ci(name, b"SCARD") {
        Some(OpCode::SCard)
    } else if ascii_eq_ci(name, b"SSCAN") {
        Some(OpCode::SScan)
    } else if ascii_eq_ci(name, b"SMOVE") {
        Some(OpCode::SMove)
    } else if ascii_eq_ci(name, b"SPOP") {
        Some(OpCode::SPop)
    } else if ascii_eq_ci(name, b"SRANDMEMBER") {
        Some(OpCode::SRandMember)
    } else if ascii_eq_ci(name, b"SINTER") {
        Some(OpCode::SInter)
    } else if ascii_eq_ci(name, b"SUNION") {
        Some(OpCode::SUnion)
    } else if ascii_eq_ci(name, b"SDIFF") {
        Some(OpCode::SDiff)
    } else if ascii_eq_ci(name, b"SINTERSTORE") {
        Some(OpCode::SInterStore)
    } else if ascii_eq_ci(name, b"SUNIONSTORE") {
        Some(OpCode::SUnionStore)
    } else if ascii_eq_ci(name, b"SDIFFSTORE") {
        Some(OpCode::SDiffStore)
    } else if ascii_eq_ci(name, b"LPUSH") {
        Some(OpCode::LPush)
    } else if ascii_eq_ci(name, b"RPUSH") {
        Some(OpCode::RPush)
    } else if ascii_eq_ci(name, b"LPOP") {
        Some(OpCode::LPop)
    } else if ascii_eq_ci(name, b"RPOP") {
        Some(OpCode::RPop)
    } else if ascii_eq_ci(name, b"LLEN") {
        Some(OpCode::LLen)
    } else if ascii_eq_ci(name, b"LINDEX") {
        Some(OpCode::LIndex)
    } else if ascii_eq_ci(name, b"LRANGE") {
        Some(OpCode::LRange)
    } else if ascii_eq_ci(name, b"LSET") {
        Some(OpCode::LSet)
    } else if ascii_eq_ci(name, b"LREM") {
        Some(OpCode::LRem)
    } else if ascii_eq_ci(name, b"LTRIM") {
        Some(OpCode::LTrim)
    } else if ascii_eq_ci(name, b"LINSERT") {
        Some(OpCode::LInsert)
    } else if ascii_eq_ci(name, b"LPUSHX") {
        Some(OpCode::LPushX)
    } else if ascii_eq_ci(name, b"RPUSHX") {
        Some(OpCode::RPushX)
    } else if ascii_eq_ci(name, b"LMOVE") {
        Some(OpCode::LMove)
    } else if ascii_eq_ci(name, b"BLMOVE") {
        Some(OpCode::BLMove)
    } else if ascii_eq_ci(name, b"RPOPLPUSH") {
        Some(OpCode::RPopLPush)
    } else if ascii_eq_ci(name, b"BRPOPLPUSH") {
        Some(OpCode::BRPopLPush)
    } else if ascii_eq_ci(name, b"LPOS") {
        Some(OpCode::LPos)
    } else if ascii_eq_ci(name, b"ZADD") {
        Some(OpCode::ZAdd)
    } else if ascii_eq_ci(name, b"ZSCORE") {
        Some(OpCode::ZScore)
    } else if ascii_eq_ci(name, b"ZMSCORE") {
        Some(OpCode::ZMScore)
    } else if ascii_eq_ci(name, b"ZINCRBY") {
        Some(OpCode::ZIncrBy)
    } else if ascii_eq_ci(name, b"ZREM") {
        Some(OpCode::ZRem)
    } else if ascii_eq_ci(name, b"ZCARD") {
        Some(OpCode::ZCard)
    } else if ascii_eq_ci(name, b"ZRANGE") {
        Some(OpCode::ZRange)
    } else if ascii_eq_ci(name, b"ZREVRANGE") {
        Some(OpCode::ZRevRange)
    } else if ascii_eq_ci(name, b"ZRANGEBYSCORE") {
        Some(OpCode::ZRangeByScore)
    } else if ascii_eq_ci(name, b"ZREVRANGEBYSCORE") {
        Some(OpCode::ZRevRangeByScore)
    } else if ascii_eq_ci(name, b"ZRANGEBYLEX") {
        Some(OpCode::ZRangeByLex)
    } else if ascii_eq_ci(name, b"ZREVRANGEBYLEX") {
        Some(OpCode::ZRevRangeByLex)
    } else if ascii_eq_ci(name, b"ZLEXCOUNT") {
        Some(OpCode::ZLexCount)
    } else if ascii_eq_ci(name, b"ZREMRANGEBYSCORE") {
        Some(OpCode::ZRemRangeByScore)
    } else if ascii_eq_ci(name, b"ZREMRANGEBYRANK") {
        Some(OpCode::ZRemRangeByRank)
    } else if ascii_eq_ci(name, b"ZREMRANGEBYLEX") {
        Some(OpCode::ZRemRangeByLex)
    } else if ascii_eq_ci(name, b"ZRANGESTORE") {
        Some(OpCode::ZRangeStore)
    } else if ascii_eq_ci(name, b"ZUNIONSTORE") {
        Some(OpCode::ZUnionStore)
    } else if ascii_eq_ci(name, b"ZINTERSTORE") {
        Some(OpCode::ZInterStore)
    } else if ascii_eq_ci(name, b"ZDIFFSTORE") {
        Some(OpCode::ZDiffStore)
    } else if ascii_eq_ci(name, b"ZUNION") {
        Some(OpCode::ZUnion)
    } else if ascii_eq_ci(name, b"ZINTER") {
        Some(OpCode::ZInter)
    } else if ascii_eq_ci(name, b"ZDIFF") {
        Some(OpCode::ZDiff)
    } else if ascii_eq_ci(name, b"ZINTERCARD") {
        Some(OpCode::ZInterCard)
    } else if ascii_eq_ci(name, b"ZMPOP") {
        Some(OpCode::ZMPop)
    } else if ascii_eq_ci(name, b"BZMPOP") {
        Some(OpCode::BZMPop)
    } else if ascii_eq_ci(name, b"BZPOPMIN") {
        Some(OpCode::BZPopMin)
    } else if ascii_eq_ci(name, b"BZPOPMAX") {
        Some(OpCode::BZPopMax)
    } else if ascii_eq_ci(name, b"ZRANK") {
        Some(OpCode::ZRank)
    } else if ascii_eq_ci(name, b"ZREVRANK") {
        Some(OpCode::ZRevRank)
    } else if ascii_eq_ci(name, b"ZCOUNT") {
        Some(OpCode::ZCount)
    } else if ascii_eq_ci(name, b"ZPOPMIN") {
        Some(OpCode::ZPopMin)
    } else if ascii_eq_ci(name, b"ZPOPMAX") {
        Some(OpCode::ZPopMax)
    } else if ascii_eq_ci(name, b"ZRANDMEMBER") {
        Some(OpCode::ZRandMember)
    } else if ascii_eq_ci(name, b"ZSCAN") {
        Some(OpCode::ZScan)
    } else if ascii_eq_ci(name, b"WATCH") {
        Some(OpCode::Watch)
    } else if ascii_eq_ci(name, b"UNWATCH") {
        Some(OpCode::Unwatch)
    } else if ascii_eq_ci(name, b"SUBSCRIBE") {
        Some(OpCode::Subscribe)
    } else if ascii_eq_ci(name, b"UNSUBSCRIBE") {
        Some(OpCode::Unsubscribe)
    } else if ascii_eq_ci(name, b"PSUBSCRIBE") {
        Some(OpCode::PSubscribe)
    } else if ascii_eq_ci(name, b"PUNSUBSCRIBE") {
        Some(OpCode::PUnsubscribe)
    } else if ascii_eq_ci(name, b"PUBLISH") {
        Some(OpCode::Publish)
    } else if ascii_eq_ci(name, b"PUBSUB") {
        Some(OpCode::PubSub)
    } else if ascii_eq_ci(name, b"DEL") {
        Some(OpCode::Del)
    } else if ascii_eq_ci(name, b"UNLINK") {
        Some(OpCode::Del)
    } else if ascii_eq_ci(name, b"EXISTS") {
        Some(OpCode::Exists)
    } else if ascii_eq_ci(name, b"PING") {
        Some(OpCode::Ping)
    } else if ascii_eq_ci(name, b"MULTI") {
        Some(OpCode::Multi)
    } else if ascii_eq_ci(name, b"EXEC") {
        Some(OpCode::Exec)
    } else if ascii_eq_ci(name, b"DISCARD") {
        Some(OpCode::Discard)
    } else if ascii_eq_ci(name, b"HELLO") {
        Some(OpCode::Hello)
    } else if ascii_eq_ci(name, b"CLIENT") {
        Some(OpCode::Client)
    } else if ascii_eq_ci(name, b"COMMAND") {
        Some(OpCode::Command)
    } else if ascii_eq_ci(name, b"MEMORY") {
        Some(OpCode::Memory)
    } else if ascii_eq_ci(name, b"RESET") {
        Some(OpCode::Reset)
    } else if ascii_eq_ci(name, b"QUIT") {
        Some(OpCode::Quit)
    } else if ascii_eq_ci(name, b"SELECT") {
        Some(OpCode::Select)
    } else if ascii_eq_ci(name, b"AUTH") {
        Some(OpCode::Auth)
    } else if ascii_eq_ci(name, b"ECHO") {
        Some(OpCode::Echo)
    } else if ascii_eq_ci(name, b"INFO") {
        Some(OpCode::Info)
    } else if ascii_eq_ci(name, b"CONFIG") {
        Some(OpCode::Config)
    } else if ascii_eq_ci(name, b"TOUCH") {
        Some(OpCode::Touch)
    } else if ascii_eq_ci(name, b"SORT_RO") {
        Some(OpCode::SortRo)
    } else if ascii_eq_ci(name, b"BITCOUNT") {
        Some(OpCode::BitCount)
    } else if ascii_eq_ci(name, b"BITPOS") {
        Some(OpCode::BitPos)
    } else if ascii_eq_ci(name, b"BITFIELD_RO") {
        Some(OpCode::BitFieldRo)
    } else if ascii_eq_ci(name, b"BITFIELD") {
        Some(OpCode::BitField)
    } else if ascii_eq_ci(name, b"BITOP") {
        Some(OpCode::BitOp)
    } else if ascii_eq_ci(name, b"PFADD") {
        Some(OpCode::PfAdd)
    } else if ascii_eq_ci(name, b"PFCOUNT") {
        Some(OpCode::PfCount)
    } else if ascii_eq_ci(name, b"PFMERGE") {
        Some(OpCode::PfMerge)
    } else if ascii_eq_ci(name, b"MONITOR") {
        Some(OpCode::Monitor)
    } else if ascii_eq_ci(name, b"CLUSTER") {
        Some(OpCode::Cluster)
    } else if ascii_eq_ci(name, b"READONLY") {
        Some(OpCode::ReadOnly)
    } else if ascii_eq_ci(name, b"READWRITE") {
        Some(OpCode::ReadWrite)
    } else if ascii_eq_ci(name, b"SLOWLOG") {
        Some(OpCode::SlowLog)
    } else if ascii_eq_ci(name, b"LATENCY") {
        Some(OpCode::Latency)
    } else if ascii_eq_ci(name, b"OBJECT") {
        Some(OpCode::Object)
    } else if ascii_eq_ci(name, b"ACL") {
        Some(OpCode::Acl)
    } else if ascii_eq_ci(name, b"SPUBLISH") {
        // Single-keyspace server: sharded publish is plain publish.
        Some(OpCode::Publish)
    } else if ascii_eq_ci(name, b"SSUBSCRIBE") {
        Some(OpCode::SSubscribe)
    } else if ascii_eq_ci(name, b"SUNSUBSCRIBE") {
        Some(OpCode::SUnsubscribe)
    } else if ascii_eq_ci(name, b"GEOADD") {
        Some(OpCode::GeoAdd)
    } else if ascii_eq_ci(name, b"GEOPOS") {
        Some(OpCode::GeoPos)
    } else if ascii_eq_ci(name, b"GEODIST") {
        Some(OpCode::GeoDist)
    } else if ascii_eq_ci(name, b"GEOHASH") {
        Some(OpCode::GeoHash)
    } else if ascii_eq_ci(name, b"XADD") {
        Some(OpCode::XAdd)
    } else if ascii_eq_ci(name, b"XRANGE") {
        Some(OpCode::XRange)
    } else if ascii_eq_ci(name, b"XREVRANGE") {
        Some(OpCode::XRevRange)
    } else if ascii_eq_ci(name, b"XLEN") {
        Some(OpCode::XLen)
    } else if ascii_eq_ci(name, b"XDEL") {
        Some(OpCode::XDel)
    } else if ascii_eq_ci(name, b"XTRIM") {
        Some(OpCode::XTrim)
    } else if ascii_eq_ci(name, b"XSETID") {
        Some(OpCode::XSetId)
    } else if ascii_eq_ci(name, b"XINFO") {
        Some(OpCode::XInfo)
    } else if ascii_eq_ci(name, b"XREAD") {
        Some(OpCode::XRead)
    } else if ascii_eq_ci(name, b"XGROUP") {
        Some(OpCode::XGroup)
    } else if ascii_eq_ci(name, b"XREADGROUP") {
        Some(OpCode::XReadGroup)
    } else if ascii_eq_ci(name, b"XACK") {
        Some(OpCode::XAck)
    } else if ascii_eq_ci(name, b"XPENDING") {
        Some(OpCode::XPending)
    } else if ascii_eq_ci(name, b"XCLAIM") {
        Some(OpCode::XClaim)
    } else if ascii_eq_ci(name, b"XAUTOCLAIM") {
        Some(OpCode::XAutoClaim)
    } else if ascii_eq_ci(name, b"GEOSEARCH")
        || ascii_eq_ci(name, b"GEORADIUS")
        || ascii_eq_ci(name, b"GEORADIUS_RO")
        || ascii_eq_ci(name, b"GEORADIUSBYMEMBER")
        || ascii_eq_ci(name, b"GEORADIUSBYMEMBER_RO")
    {
        // The parse arm re-reads the name to pick the argument syntax.
        Some(OpCode::GeoSearch)
    } else if ascii_eq_ci(name, b"MODULE") {
        Some(OpCode::Module)
    } else if ascii_eq_ci(name, b"GEOSEARCHSTORE") {
        // GEORADIUS and GEORADIUSBYMEMBER also become this opcode, but only
        // once their option loop has seen a STORE or STOREDIST destination.
        Some(OpCode::GeoSearchStore)
    } else {
        None
    }
}

fn frame_to_bytes(frame: &BytesFrame) -> Option<Bytes> {
    use BytesFrame::*;
    match frame {
        BlobString { data, .. } | SimpleString { data, .. } => Some(Bytes::copy_from_slice(data)),
        Number { data, .. } => Some(Bytes::from(data.to_string())),
        _ => None,
    }
}

fn command_args(parts: &[BytesFrame]) -> Option<Vec<Bytes>> {
    let mut args = Vec::with_capacity(parts.len().saturating_sub(1));
    for part in parts.iter().skip(1) {
        args.push(frame_to_bytes(part)?);
    }
    Some(args)
}

fn wrong_arity(command: &'static str) -> ParseError {
    ParseError::WrongArity { command }
}

fn part_to_bytes(part: &BytesFrame) -> Result<Bytes, ParseError> {
    match part {
        BytesFrame::BlobString { data, .. } | BytesFrame::SimpleString { data, .. } => {
            Ok(Bytes::copy_from_slice(data))
        }
        _ => Err(ParseError::Protocol("invalid argument")),
    }
}

/// Validates one Redis-visible key and returns the name it is stored under.
///
/// This is the single place the logical-database prefix is applied, and every
/// key that enters the system passes through it, in `cmd.keys` and in
/// `cmd.values` alike: destinations, sources and the key lists of the blocking
/// pops. It returns the storage-facing key rather than `()` precisely so that a
/// call site which forgets to take the result does not compile, because a
/// forgotten site would let one database read or overwrite another's data.
///
/// Callers hand it the raw bytes the client sent; `cmd.args` keeps those raw
/// bytes, which is what MONITOR and CLUSTER KEYSLOT want.
fn validate_user_key(key: &Bytes) -> Result<Bytes, ParseError> {
    match key.first() {
        // 0x01 is the executor's hidden collection and TTL namespace; 0x02 is
        // the logical-database namespace this function writes. Redis accepts
        // any bytes in a key, so both reservations are divergences.
        Some(&0x01) | Some(&DB_KEY_MARKER) => {
            Err(ParseError::Error("invalid key: reserved internal prefix"))
        }
        _ => Ok(db_key(current_db(), key)),
    }
}

/// Number of logical databases, as Redis's `databases` config reports it.
const REDIS_DATABASE_COUNT: i64 = 16;

/// First byte of the hidden prefix that places a key in a logical database.
/// A key in database `n` (1..15) is stored under `0x02 <n> ':' <key>`;
/// database 0 keys are stored under exactly the bytes the client sent, so
/// nothing about the existing keyspace layout moves.
const DB_KEY_MARKER: u8 = 0x02;

/// The logical database this worker thread is currently acting for.
#[inline]
fn current_db() -> u8 {
    CURRENT_DB.with(|db| db.get())
}

/// Point this worker thread at a connection's logical database. Called on every
/// path that starts work for a client: `process_buffered_frames` before it
/// parses a frame, `service_client` before a blocked or deferred command
/// resumes, and the SELECT and RESET handlers when the selection changes.
#[inline]
fn set_current_db(db: u8) {
    CURRENT_DB.with(|cell| cell.set(db));
}

/// Storage-facing name of a Redis-visible key in database `db`.
///
/// Database 0 is the identity, and a `Bytes` clone is a refcount bump, so the
/// database-0 paths every benchmark measures allocate nothing here.
#[inline]
fn db_key(db: u8, key: &Bytes) -> Bytes {
    if db == 0 {
        return key.clone();
    }
    let mut buf = Vec::with_capacity(key.len() + 3);
    buf.extend_from_slice(&[DB_KEY_MARKER, db, b':']);
    buf.extend_from_slice(key);
    Bytes::from(buf)
}

/// The Redis-visible name of a key that came back from storage, for the replies
/// that return key names: KEYS, SCAN, RANDOMKEY and the key element of
/// BLPOP/BRPOP/BLMPOP/LMPOP/BZPOPMIN/BZPOPMAX/BZMPOP/ZMPOP.
#[inline]
fn strip_db_key(db: u8, key: &[u8]) -> &[u8] {
    if db == 0 {
        return key;
    }
    if key.len() >= 3 && key[0] == DB_KEY_MARKER && key[1] == db && key[2] == b':' {
        &key[3..]
    } else {
        key
    }
}

/// The scan prefix that selects one logical database, empty for database 0.
/// It is what KEYS/SCAN/DBSIZE/RANDOMKEY put in front of the literal MATCH
/// prefix, and what FLUSHDB sends on its own; see the "Logical databases"
/// comment in `include/transaction_ffi.h` for how the executor reads it.
fn db_scan_prefix(db: u8) -> Bytes {
    if db == 0 {
        Bytes::new()
    } else {
        Bytes::from(vec![DB_KEY_MARKER, db, b':'])
    }
}

/// The FLUSHDB payload: a one-byte 0x02 sentinel for database 0 (clear
/// everything that is not in another database) and the three-byte prefix for
/// the rest. FLUSHALL sends nothing and clears every database.
fn db_flush_prefix(db: u8) -> Bytes {
    if db == 0 {
        Bytes::from_static(&[DB_KEY_MARKER])
    } else {
        db_scan_prefix(db)
    }
}

/// Parse a SELECT / MOVE / COPY database index with Redis's two error texts.
fn parse_db_index(arg: &[u8]) -> Result<u8, ParseError> {
    let Ok(text) = std::str::from_utf8(arg) else {
        return Err(ParseError::Error("value is not an integer or out of range"));
    };
    let Ok(index) = text.parse::<i64>() else {
        return Err(ParseError::Error("value is not an integer or out of range"));
    };
    if !(0..REDIS_DATABASE_COUNT).contains(&index) {
        return Err(ParseError::Error("DB index is out of range"));
    }
    Ok(index as u8)
}

fn invalid_expire_error(command: &'static str) -> ParseError {
    ParseError::Error(match command {
        "set" => "invalid expire time in 'set' command",
        "setex" => "invalid expire time in 'setex' command",
        "psetex" => "invalid expire time in 'psetex' command",
        "getex" => "invalid expire time in 'getex' command",
        "expire" => "invalid expire time in 'expire' command",
        "pexpire" => "invalid expire time in 'pexpire' command",
        "expireat" => "invalid expire time in 'expireat' command",
        "pexpireat" => "invalid expire time in 'pexpireat' command",
        "hexpire" => "invalid expire time in 'hexpire' command",
        "hpexpire" => "invalid expire time in 'hpexpire' command",
        "hexpireat" => "invalid expire time in 'hexpireat' command",
        "hpexpireat" => "invalid expire time in 'hpexpireat' command",
        _ => "invalid expire time",
    })
}

/// Absolute Unix ms for one HEXPIRE-family argument, with Redis's checks in
/// Redis's order: negative first, then the unit's own ceiling, then the
/// ceiling on the resulting absolute time.
fn hash_field_expire_at_ms(
    raw: &[u8],
    seconds: bool,
    absolute: bool,
    command: &'static str,
) -> Result<i64, ParseError> {
    let text = std::str::from_utf8(raw).map_err(|_| integer_error())?;
    let amount: i64 = text.parse().map_err(|_| integer_error())?;
    if amount < 0 {
        return Err(ParseError::Error("invalid expire time, must be >= 0"));
    }
    let ceiling = if seconds {
        HASH_FIELD_EXPIRE_TIME_MAX_MS / 1000
    } else {
        HASH_FIELD_EXPIRE_TIME_MAX_MS
    };
    if amount > ceiling {
        return Err(invalid_expire_error(command));
    }
    let requested_ms = if seconds { amount * 1000 } else { amount };
    let basetime = if absolute { 0 } else { unix_time_ms() };
    if requested_ms > HASH_FIELD_EXPIRE_TIME_MAX_MS - basetime {
        return Err(invalid_expire_error(command));
    }
    Ok(requested_ms + basetime)
}

/// The `FIELDS numfields field [field ...]` tail every HEXPIRE-family command
/// ends with. `fields_at` is the index of the FIELDS token itself, which the
/// caller has already matched.
fn parse_hash_fields_tail(
    parts: &[BytesFrame],
    fields_at: usize,
) -> Result<Vec<Bytes>, ParseError> {
    const BAD_COUNT: &str = "Parameter `numFields` should be greater than 0";
    const MISMATCH: &str = "The `numfields` parameter must match the number of arguments";
    if fields_at + 1 >= parts.len() {
        return Err(ParseError::Error(BAD_COUNT));
    }
    let raw = part_to_bytes(&parts[fields_at + 1])?;
    let count: i64 = std::str::from_utf8(raw.as_ref())
        .ok()
        .and_then(|text| text.parse().ok())
        .ok_or(ParseError::Error(BAD_COUNT))?;
    if count <= 0 {
        return Err(ParseError::Error(BAD_COUNT));
    }
    let given = (parts.len() - fields_at - 2) as i64;
    if count != given {
        return Err(ParseError::Error(MISMATCH));
    }
    let mut fields = Vec::with_capacity(given as usize);
    for part in parts.iter().skip(fields_at + 2) {
        fields.push(part_to_bytes(part)?);
    }
    Ok(fields)
}

fn integer_error() -> ParseError {
    ParseError::Error("value is not an integer or out of range")
}

fn checked_abs_ms_from_seconds_for(amount: i64, command: &'static str) -> Result<i64, ParseError> {
    amount
        .checked_mul(1000)
        .ok_or_else(|| invalid_expire_error(command))
}

fn checked_relative_ms_for(
    now_ms: i64,
    amount_ms: i64,
    command: &'static str,
) -> Result<i64, ParseError> {
    now_ms
        .checked_add(amount_ms)
        .ok_or_else(|| invalid_expire_error(command))
}

fn ttl_ms_from_args_for(
    unit: &[u8],
    value: &[u8],
    command: &'static str,
) -> Result<i64, ParseError> {
    let text = std::str::from_utf8(value).map_err(|_| integer_error())?;
    let amount: i64 = text.parse().map_err(|_| integer_error())?;
    if amount <= 0 {
        return Err(invalid_expire_error(command));
    }
    let now_ms = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_err(|_| invalid_expire_error(command))?
        .as_millis() as i64;
    if ascii_eq_ci(unit, b"EX") {
        checked_relative_ms_for(
            now_ms,
            checked_abs_ms_from_seconds_for(amount, command)?,
            command,
        )
    } else if ascii_eq_ci(unit, b"PX") {
        checked_relative_ms_for(now_ms, amount, command)
    } else if ascii_eq_ci(unit, b"EXAT") {
        checked_abs_ms_from_seconds_for(amount, command)
    } else {
        Ok(amount)
    }
}

fn expire_at_ms_from_args(
    unit: &[u8],
    value: &[u8],
    command: &'static str,
) -> Result<i64, ParseError> {
    let text = std::str::from_utf8(value).map_err(|_| integer_error())?;
    let amount: i64 = text.parse().map_err(|_| integer_error())?;
    let now_ms = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_err(|_| invalid_expire_error(command))?
        .as_millis() as i64;
    if ascii_eq_ci(unit, b"EXPIRE") {
        checked_relative_ms_for(
            now_ms,
            checked_abs_ms_from_seconds_for(amount, command)?,
            command,
        )
    } else if ascii_eq_ci(unit, b"PEXPIRE") {
        checked_relative_ms_for(now_ms, amount, command)
    } else if ascii_eq_ci(unit, b"EXPIREAT") {
        checked_abs_ms_from_seconds_for(amount, command)
    } else {
        Ok(amount)
    }
}

fn parse_expire_modifier(arg: &[u8]) -> Result<u32, ParseError> {
    if ascii_eq_ci(arg, b"NX") {
        Ok(TXN_FLAG_EXPIRE_NX)
    } else if ascii_eq_ci(arg, b"XX") {
        Ok(TXN_FLAG_EXPIRE_XX)
    } else if ascii_eq_ci(arg, b"GT") {
        Ok(TXN_FLAG_EXPIRE_GT)
    } else if ascii_eq_ci(arg, b"LT") {
        Ok(TXN_FLAG_EXPIRE_LT)
    } else {
        let text = String::from_utf8_lossy(arg).into_owned();
        let leaked: &'static str =
            Box::leak(format!("Unsupported option {}", text).into_boxed_str());
        Err(ParseError::Error(leaked))
    }
}

fn validate_expire_flags(flags: u32) -> Result<(), ParseError> {
    if (flags & TXN_FLAG_EXPIRE_NX) != 0
        && (flags & (TXN_FLAG_EXPIRE_XX | TXN_FLAG_EXPIRE_GT | TXN_FLAG_EXPIRE_LT)) != 0
    {
        return Err(ParseError::Error(
            "NX and XX, GT or LT options at the same time are not compatible",
        ));
    }
    if (flags & TXN_FLAG_EXPIRE_GT) != 0 && (flags & TXN_FLAG_EXPIRE_LT) != 0 {
        return Err(ParseError::Error(
            "GT and LT options at the same time are not compatible",
        ));
    }
    Ok(())
}

fn parse_positive_i64(arg: &[u8]) -> Result<i64, ParseError> {
    let text = std::str::from_utf8(arg).map_err(|_| ParseError::Protocol("invalid argument"))?;
    let value: i64 = text
        .parse()
        .map_err(|_| ParseError::Protocol("invalid argument"))?;
    if value <= 0 {
        Err(ParseError::Protocol("invalid argument"))
    } else {
        Ok(value)
    }
}

fn parse_i64_arg(arg: &[u8]) -> Result<i64, ParseError> {
    let text = std::str::from_utf8(arg).map_err(|_| ParseError::Protocol("invalid argument"))?;
    text.parse()
        .map_err(|_| ParseError::Protocol("invalid argument"))
}

fn parse_i64_error_arg(arg: &[u8], message: &'static str) -> Result<i64, ParseError> {
    let text = std::str::from_utf8(arg).map_err(|_| ParseError::Error(message))?;
    text.parse().map_err(|_| ParseError::Error(message))
}

fn parse_f64_arg(arg: &[u8]) -> Result<f64, ParseError> {
    let text = std::str::from_utf8(arg).map_err(|_| ParseError::Protocol("invalid argument"))?;
    let value: f64 = text
        .parse()
        .map_err(|_| ParseError::Protocol("invalid argument"))?;
    if value.is_nan() {
        Err(ParseError::Error("value is not a valid float"))
    } else {
        Ok(value)
    }
}

fn parse_blocking_timeout_ms(arg: &[u8]) -> Result<i64, ParseError> {
    if arg.len() > 2 && arg[0] == b'0' && (arg[1] == b'x' || arg[1] == b'X') {
        return Err(ParseError::Error("timeout is out of range"));
    }
    let timeout = parse_f64_arg(arg)?;
    if timeout < 0.0 {
        return Err(ParseError::Error("timeout is negative"));
    }
    if !timeout.is_finite() || timeout > (i64::MAX as f64 / 1000.0) {
        return Err(ParseError::Error("timeout is out of range"));
    }
    if timeout == 0.0 {
        Ok(0)
    } else {
        Ok((timeout * 1000.0).ceil().max(1.0) as i64)
    }
}

fn parse_zadd_score_arg(arg: &[u8]) -> Result<(), ParseError> {
    parse_f64_error_arg(arg, "value is not a valid float").map(|_| ())
}

fn parse_f64_error_arg(arg: &[u8], message: &'static str) -> Result<f64, ParseError> {
    let text = std::str::from_utf8(arg).map_err(|_| ParseError::Error(message))?;
    let value: f64 = text.parse().map_err(|_| ParseError::Error(message))?;
    if value.is_nan() {
        Err(ParseError::Error(message))
    } else {
        Ok(value)
    }
}

fn parse_zrange_bound_arg(arg: &[u8]) -> Result<(), ParseError> {
    let raw = if arg.first() == Some(&b'(') {
        &arg[1..]
    } else {
        arg
    };
    if ascii_eq_ci(raw, b"-inf") || ascii_eq_ci(raw, b"+inf") || ascii_eq_ci(raw, b"inf") {
        return Ok(());
    }
    parse_f64_error_arg(raw, "value is not a valid float").map(|_| ())
}

fn parse_zlex_bound_arg(arg: &[u8]) -> Result<(), ParseError> {
    if arg == b"-" || arg == b"+" {
        return Ok(());
    }
    if arg.first() == Some(&b'(') || arg.first() == Some(&b'[') {
        return Ok(());
    }
    Err(ParseError::Error("min or max not valid string range item"))
}

fn parse_list_side(arg: &[u8]) -> Result<bool, ParseError> {
    if ascii_eq_ci(arg, b"LEFT") {
        Ok(true)
    } else if ascii_eq_ci(arg, b"RIGHT") {
        Ok(false)
    } else {
        Err(ParseError::Protocol("syntax error"))
    }
}

fn literal_prefix(pattern: &[u8]) -> Bytes {
    let mut out = Vec::new();
    let mut escaped = false;
    for &byte in pattern {
        if escaped {
            out.push(byte);
            escaped = false;
        } else if byte == b'\\' {
            escaped = true;
        } else if matches!(byte, b'*' | b'?' | b'[') {
            break;
        } else {
            out.push(byte);
        }
    }
    Bytes::from(out)
}

fn scan_cursor_from_arg(arg: &[u8]) -> Result<Bytes, ParseError> {
    if arg == b"0" {
        return Ok(Bytes::new());
    }
    let text = std::str::from_utf8(arg).map_err(|_| ParseError::Protocol("invalid cursor"))?;
    let cursor_id: usize = text
        .parse()
        .map_err(|_| ParseError::Protocol("invalid cursor"))?;
    let cursors = SCAN_CURSORS.get_or_init(|| Mutex::new(HashMap::new()));
    let mut guard = cursors
        .lock()
        .map_err(|_| ParseError::Protocol("invalid cursor"))?;
    guard
        .remove(&cursor_id)
        .ok_or(ParseError::Protocol("invalid cursor"))
}

fn store_scan_cursor(cursor: &[u8]) -> String {
    if cursor.is_empty() {
        return "0".to_string();
    }
    let id = NEXT_SCAN_CURSOR_ID.fetch_add(1, Ordering::Relaxed);
    let cursors = SCAN_CURSORS.get_or_init(|| Mutex::new(HashMap::new()));
    if let Ok(mut guard) = cursors.lock() {
        guard.insert(id, Bytes::copy_from_slice(cursor));
        id.to_string()
    } else {
        "0".to_string()
    }
}

fn scan_offset_from_arg(arg: &[u8]) -> Result<usize, ParseError> {
    if arg == b"0" {
        return Ok(0);
    }
    let cursor = scan_cursor_from_arg(arg)?;
    let text =
        std::str::from_utf8(cursor.as_ref()).map_err(|_| ParseError::Protocol("invalid cursor"))?;
    let Some(raw_offset) = text.strip_prefix("offset:") else {
        return Err(ParseError::Protocol("invalid cursor"));
    };
    raw_offset
        .parse()
        .map_err(|_| ParseError::Protocol("invalid cursor"))
}

fn store_scan_offset(offset: usize) -> String {
    store_scan_cursor(format!("offset:{offset}").as_bytes())
}

fn glob_class_matches(pattern: &[u8], start: usize, value: u8) -> Option<(bool, usize)> {
    let mut index = start + 1;
    if index >= pattern.len() {
        return None;
    }
    let negated = matches!(pattern[index], b'^' | b'!');
    if negated {
        index += 1;
    }

    let mut matched = false;
    let mut saw_end = false;
    let mut previous: Option<u8> = None;
    while index < pattern.len() {
        let byte = pattern[index];
        if byte == b']' && previous.is_some() {
            saw_end = true;
            index += 1;
            break;
        }
        if byte == b'\\' && index + 1 < pattern.len() {
            let escaped = pattern[index + 1];
            if escaped == value {
                matched = true;
            }
            previous = Some(escaped);
            index += 2;
            continue;
        }
        if byte == b'-'
            && previous.is_some()
            && index + 1 < pattern.len()
            && pattern[index + 1] != b']'
        {
            let end = pattern[index + 1];
            let begin = previous.unwrap();
            if begin <= value && value <= end {
                matched = true;
            }
            previous = Some(end);
            index += 2;
            continue;
        }
        if byte == value {
            matched = true;
        }
        previous = Some(byte);
        index += 1;
    }

    if saw_end {
        Some((if negated { !matched } else { matched }, index))
    } else {
        None
    }
}

fn glob_matches(pattern: &[u8], text: &[u8]) -> bool {
    let (mut p, mut t) = (0usize, 0usize);
    let (mut star, mut match_after_star) = (None, 0usize);
    while t < text.len() {
        if p < pattern.len() && pattern[p] == b'[' {
            if let Some((matched, next_p)) = glob_class_matches(pattern, p, text[t]) {
                if matched {
                    p = next_p;
                    t += 1;
                } else if let Some(star_pos) = star {
                    p = star_pos + 1;
                    match_after_star += 1;
                    t = match_after_star;
                } else {
                    return false;
                }
            } else if pattern[p] == text[t] {
                p += 1;
                t += 1;
            } else if let Some(star_pos) = star {
                p = star_pos + 1;
                match_after_star += 1;
                t = match_after_star;
            } else {
                return false;
            }
        } else if p < pattern.len() && pattern[p] == b'\\' && p + 1 < pattern.len() {
            p += 1;
            if pattern[p] == text[t] {
                p += 1;
                t += 1;
            } else if let Some(star_pos) = star {
                p = star_pos + 1;
                match_after_star += 1;
                t = match_after_star;
            } else {
                return false;
            }
        } else if p < pattern.len() && (pattern[p] == b'?' || pattern[p] == text[t]) {
            p += 1;
            t += 1;
        } else if p < pattern.len() && pattern[p] == b'*' {
            star = Some(p);
            p += 1;
            match_after_star = t;
        } else if let Some(star_pos) = star {
            p = star_pos + 1;
            match_after_star += 1;
            t = match_after_star;
        } else {
            return false;
        }
    }
    while p < pattern.len() && pattern[p] == b'*' {
        p += 1;
    }
    p == pattern.len()
}

/// Parse RESP3 frame into Command
fn parse_resp3(frame: DecodedFrame<BytesFrame>) -> Result<Command, ParseError> {
    use BytesFrame::*;
    let f = frame
        .into_complete_frame()
        .map_err(|_| ParseError::Protocol("invalid frame"))?;
    let parts = match f {
        Array { data, .. } => data,
        _ => return Err(ParseError::Protocol("expected array")),
    };

    let name = match parts.get(0) {
        Some(BlobString { data, .. }) | Some(SimpleString { data, .. }) => data.as_ref(),
        _ => return Err(ParseError::Protocol("missing command")),
    };

    let Some(op) = parse_opcode(name) else {
        let args = command_args(&parts).unwrap_or_default();
        return Err(ParseError::UnknownCommand {
            name: Bytes::copy_from_slice(name),
            args,
        });
    };

    match op {
        OpCode::Get => {
            if parts.len() != 2 {
                return Err(wrong_arity("get"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            Ok(Command::new(op, vec![key], None, Vec::new()))
        }
        OpCode::Touch => {
            if parts.len() < 2 {
                return Err(wrong_arity("touch"));
            }
            let mut keys = Vec::with_capacity(parts.len() - 1);
            for part in parts.iter().skip(1) {
                let key = part_to_bytes(part)?;
                let key = validate_user_key(&key)?;
                keys.push(key);
            }
            Ok(Command::new(
                op,
                keys,
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            ))
        }
        OpCode::BitCount => {
            if parts.len() != 2 && parts.len() != 4 && parts.len() != 5 {
                return Err(wrong_arity("bitcount"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut values = Vec::new();
            if parts.len() >= 4 {
                let start = part_to_bytes(&parts[2])?;
                let end = part_to_bytes(&parts[3])?;
                parse_i64_error_arg(start.as_ref(), "value is not an integer or out of range")?;
                parse_i64_error_arg(end.as_ref(), "value is not an integer or out of range")?;
                values.push(start);
                values.push(end);
                if parts.len() == 5 {
                    let unit = part_to_bytes(&parts[4])?;
                    if !ascii_eq_ci(unit.as_ref(), b"BYTE") && !ascii_eq_ci(unit.as_ref(), b"BIT") {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    values.push(unit);
                }
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = values;
            Ok(cmd)
        }
        OpCode::BitPos => {
            if parts.len() < 3 || parts.len() > 6 {
                return Err(wrong_arity("bitpos"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let bit = part_to_bytes(&parts[2])?;
            if bit.as_ref() != b"0" && bit.as_ref() != b"1" {
                return Err(ParseError::Error("The bit argument must be 1 or 0."));
            }
            let mut values = vec![bit];
            let numeric_end = parts.len().min(5);
            for index in 3..numeric_end {
                let value = part_to_bytes(&parts[index])?;
                parse_i64_error_arg(value.as_ref(), "value is not an integer or out of range")?;
                values.push(value);
            }
            if parts.len() == 6 {
                let unit = part_to_bytes(&parts[5])?;
                if !ascii_eq_ci(unit.as_ref(), b"BYTE") && !ascii_eq_ci(unit.as_ref(), b"BIT") {
                    return Err(ParseError::Protocol("syntax error"));
                }
                values.push(unit);
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = values;
            Ok(cmd)
        }
        OpCode::BitFieldRo => {
            if parts.len() < 2 {
                return Err(wrong_arity("bitfield_ro"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut values = Vec::new();
            let mut index = 2usize;
            while index < parts.len() {
                let subcommand = part_to_bytes(&parts[index])?;
                if !ascii_eq_ci(subcommand.as_ref(), b"GET") {
                    return Err(ParseError::Error(
                        "BITFIELD_RO only supports the GET subcommand",
                    ));
                }
                if index + 2 >= parts.len() {
                    return Err(ParseError::Protocol("syntax error"));
                }
                let encoding = part_to_bytes(&parts[index + 1])?;
                let offset = part_to_bytes(&parts[index + 2])?;
                let (_, bits) = parse_bitfield_encoding(encoding.as_ref())?;
                parse_bitfield_offset(offset.as_ref(), bits)?;
                values.push(encoding);
                values.push(offset);
                index += 3;
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = values;
            Ok(cmd)
        }
        OpCode::BitField => {
            if parts.len() < 2 {
                return Err(wrong_arity("bitfield"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            // Every Redis-facing check happens here so the executor only ever
            // sees a well-formed payload: groups of four items
            // [kind, encoding, offset, value] as documented on TXN_OP_BITFIELD.
            let mut groups: Vec<Bytes> = Vec::new();
            // Encoding/offset pairs for the read-only path, used when every
            // subcommand is a GET.
            let mut reads: Vec<Bytes> = Vec::new();
            let mut writes = false;
            let mut index = 2usize;
            while index < parts.len() {
                let name = part_to_bytes(&parts[index])?;
                // Redis checks the argument count per subcommand and reports a
                // plain syntax error when it is short.
                let remaining = parts.len() - index - 1;
                if ascii_eq_ci(name.as_ref(), b"OVERFLOW") && remaining >= 1 {
                    let requested = part_to_bytes(&parts[index + 1])?;
                    let mode: &'static [u8] = if ascii_eq_ci(requested.as_ref(), b"WRAP") {
                        b"WRAP"
                    } else if ascii_eq_ci(requested.as_ref(), b"SAT") {
                        b"SAT"
                    } else if ascii_eq_ci(requested.as_ref(), b"FAIL") {
                        b"FAIL"
                    } else {
                        return Err(ParseError::Error("Invalid OVERFLOW type specified"));
                    };
                    groups.push(Bytes::from_static(b"OVERFLOW"));
                    groups.push(Bytes::from_static(mode));
                    groups.push(Bytes::new());
                    groups.push(Bytes::new());
                    index += 2;
                    continue;
                }
                let (kind, takes_value): (&'static [u8], bool) =
                    if ascii_eq_ci(name.as_ref(), b"GET") && remaining >= 2 {
                        (b"GET", false)
                    } else if ascii_eq_ci(name.as_ref(), b"SET") && remaining >= 3 {
                        (b"SET", true)
                    } else if ascii_eq_ci(name.as_ref(), b"INCRBY") && remaining >= 3 {
                        (b"INCRBY", true)
                    } else {
                        return Err(ParseError::Error("syntax error"));
                    };
                let encoding = part_to_bytes(&parts[index + 1])?;
                let offset = part_to_bytes(&parts[index + 2])?;
                let (signed, bits) = parse_bitfield_encoding(encoding.as_ref())?;
                let absolute = parse_bitfield_offset(offset.as_ref(), bits)?;
                let value = if takes_value {
                    writes = true;
                    let raw = part_to_bytes(&parts[index + 3])?;
                    let parsed = parse_i64_error_arg(
                        raw.as_ref(),
                        "value is not an integer or out of range",
                    )?;
                    Bytes::from(parsed.to_string())
                } else {
                    reads.push(encoding.clone());
                    reads.push(offset);
                    Bytes::new()
                };
                groups.push(Bytes::from_static(kind));
                groups.push(Bytes::from(format!(
                    "{}{}",
                    if signed { 'i' } else { 'u' },
                    bits
                )));
                groups.push(Bytes::from(absolute.to_string()));
                groups.push(value);
                index += if takes_value { 4 } else { 3 };
            }
            let args = command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?;
            if !writes {
                // Redis treats a BITFIELD whose subcommands are all GET (and a
                // BITFIELD with no subcommands) as a read-only command, so run
                // it through the existing BITFIELD_RO path instead of a write op.
                let mut cmd = Command::new(OpCode::BitFieldRo, vec![key], None, args);
                cmd.values = reads;
                return Ok(cmd);
            }
            let mut cmd = Command::new(op, vec![key], None, args);
            cmd.values = groups;
            Ok(cmd)
        }
        OpCode::BitOp => {
            if parts.len() < 4 {
                return Err(wrong_arity("bitop"));
            }
            let operation = part_to_bytes(&parts[1])?;
            let is_not = ascii_eq_ci(operation.as_ref(), b"NOT");
            if !is_not
                && !ascii_eq_ci(operation.as_ref(), b"AND")
                && !ascii_eq_ci(operation.as_ref(), b"OR")
                && !ascii_eq_ci(operation.as_ref(), b"XOR")
            {
                return Err(ParseError::Protocol("syntax error"));
            }
            if is_not && parts.len() != 4 {
                return Err(ParseError::Error(
                    "BITOP NOT must be called with a single source key.",
                ));
            }
            let destination = part_to_bytes(&parts[2])?;
            let destination = validate_user_key(&destination)?;
            let mut values = Vec::with_capacity(parts.len() - 2);
            values.push(operation);
            for part in parts.iter().skip(3) {
                let key = part_to_bytes(part)?;
                let key = validate_user_key(&key)?;
                values.push(key);
            }
            let mut cmd = Command::new(
                op,
                vec![destination],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = values;
            Ok(cmd)
        }
        OpCode::PfAdd => {
            if parts.len() < 2 {
                return Err(wrong_arity("pfadd"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut values = Vec::with_capacity(parts.len() - 2);
            for part in parts.iter().skip(2) {
                values.push(part_to_bytes(part)?);
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = values;
            Ok(cmd)
        }
        OpCode::PfCount => {
            if parts.len() < 2 {
                return Err(wrong_arity("pfcount"));
            }
            let mut keys = Vec::with_capacity(parts.len() - 1);
            for part in parts.iter().skip(1) {
                let key = part_to_bytes(part)?;
                let key = validate_user_key(&key)?;
                keys.push(key);
            }
            // The op payload repeats every key, including keys[0], so the C++
            // branch and the lock-stripe pass see the same list.
            let values = keys.clone();
            let mut cmd = Command::new(
                op,
                keys,
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = values;
            Ok(cmd)
        }
        OpCode::PfMerge => {
            if parts.len() < 2 {
                return Err(wrong_arity("pfmerge"));
            }
            let destination = part_to_bytes(&parts[1])?;
            let destination = validate_user_key(&destination)?;
            let mut values = Vec::with_capacity(parts.len() - 2);
            for part in parts.iter().skip(2) {
                let key = part_to_bytes(part)?;
                let key = validate_user_key(&key)?;
                values.push(key);
            }
            let mut cmd = Command::new(
                op,
                vec![destination],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = values;
            Ok(cmd)
        }
        OpCode::GeoAdd => {
            // GEOADD key [NX|XX] [CH] lon lat member ... becomes a ZADD whose
            // score is the 52-bit geohash of each position.
            if parts.len() < 5 {
                return Err(wrong_arity("geoadd"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut flags = 0u32;
            let mut index = 2usize;
            while index < parts.len() {
                let arg = part_to_bytes(&parts[index])?;
                if ascii_eq_ci(arg.as_ref(), b"NX") {
                    flags |= TXN_FLAG_ZADD_NX;
                } else if ascii_eq_ci(arg.as_ref(), b"XX") {
                    flags |= TXN_FLAG_ZADD_XX;
                } else if ascii_eq_ci(arg.as_ref(), b"CH") {
                    flags |= TXN_FLAG_ZADD_CH;
                } else {
                    break;
                }
                index += 1;
            }
            if (flags & TXN_FLAG_ZADD_NX) != 0 && (flags & TXN_FLAG_ZADD_XX) != 0 {
                return Err(ParseError::Error("syntax error"));
            }
            if index >= parts.len() || (parts.len() - index) % 3 != 0 {
                return Err(ParseError::Error("syntax error"));
            }
            let mut values = Vec::with_capacity((parts.len() - index) / 3 * 2);
            for triple in parts[index..].chunks_exact(3) {
                let longitude = parse_f64_error_arg(
                    part_to_bytes(&triple[0])?.as_ref(),
                    "value is not a valid float",
                )?;
                let latitude = parse_f64_error_arg(
                    part_to_bytes(&triple[1])?.as_ref(),
                    "value is not a valid float",
                )?;
                // Redis formats both coordinates with %f in this message.
                if !(GEO_LONG_MIN..=GEO_LONG_MAX).contains(&longitude)
                    || !(GEO_LAT_MIN..=GEO_LAT_MAX).contains(&latitude)
                {
                    return Err(ParseError::Owned(format!(
                        "invalid longitude,latitude pair {longitude:.6},{latitude:.6}"
                    )));
                }
                let Some(score) = geo_score_for_position(longitude, latitude) else {
                    return Err(ParseError::Owned(format!(
                        "invalid longitude,latitude pair {longitude:.6},{latitude:.6}"
                    )));
                };
                values.push(Bytes::from(score.to_string()));
                values.push(part_to_bytes(&triple[2])?);
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = values;
            cmd.expire_flags = flags;
            Ok(cmd)
        }
        OpCode::GeoPos | OpCode::GeoHash => {
            // One ZSCORE op per member; the reply arm decodes each score.
            if parts.len() < 2 {
                return Err(wrong_arity(if op == OpCode::GeoPos {
                    "geopos"
                } else {
                    "geohash"
                }));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut values = Vec::with_capacity(parts.len() - 2);
            for part in parts.iter().skip(2) {
                values.push(part_to_bytes(part)?);
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = values;
            Ok(cmd)
        }
        OpCode::GeoDist => {
            if parts.len() < 4 {
                return Err(wrong_arity("geodist"));
            }
            if parts.len() > 5 {
                return Err(ParseError::Error("syntax error"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let unit = if parts.len() == 5 {
                part_to_bytes(&parts[4])?
            } else {
                Bytes::from_static(b"m")
            };
            if geo_unit_meters(unit.as_ref()).is_none() {
                return Err(ParseError::Error(GEO_UNIT_ERROR));
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                Some(unit),
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = vec![part_to_bytes(&parts[2])?, part_to_bytes(&parts[3])?];
            Ok(cmd)
        }
        OpCode::GeoSearch | OpCode::GeoSearchStore => parse_geo_search(&parts),
        OpCode::Object => {
            if parts.len() < 2 {
                return Err(wrong_arity("object"));
            }
            let subcommand = part_to_bytes(&parts[1])?;
            if ascii_eq_ci(subcommand.as_ref(), b"HELP") {
                if parts.len() != 2 {
                    return Err(wrong_arity("object|help"));
                }
                return Ok(Command::new(
                    op,
                    Vec::new(),
                    None,
                    command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
                ));
            }
            if !ascii_eq_ci(subcommand.as_ref(), b"ENCODING")
                && !ascii_eq_ci(subcommand.as_ref(), b"REFCOUNT")
                && !ascii_eq_ci(subcommand.as_ref(), b"FREQ")
                && !ascii_eq_ci(subcommand.as_ref(), b"IDLETIME")
            {
                return Err(ParseError::Error(
                    "unknown subcommand or wrong number of arguments for 'OBJECT'. Try OBJECT HELP.",
                ));
            }
            if parts.len() != 3 {
                return Err(wrong_arity("object"));
            }
            let key = part_to_bytes(&parts[2])?;
            let key = validate_user_key(&key)?;
            Ok(Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            ))
        }
        OpCode::MGet => {
            if parts.len() < 2 {
                return Err(wrong_arity("mget"));
            }
            let mut keys = Vec::with_capacity(parts.len() - 1);
            for part in parts.iter().skip(1) {
                let key = part_to_bytes(part)?;
                let key = validate_user_key(&key)?;
                keys.push(key);
            }
            Ok(Command::new(
                op,
                keys,
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            ))
        }
        OpCode::Del | OpCode::Exists => {
            if parts.len() < 2 {
                return Err(wrong_arity(if op == OpCode::Del {
                    "del"
                } else {
                    "exists"
                }));
            }
            let mut keys = Vec::with_capacity(parts.len() - 1);
            for part in parts.iter().skip(1) {
                let key = part_to_bytes(part)?;
                let key = validate_user_key(&key)?;
                keys.push(key);
            }
            Ok(Command::new(
                op,
                keys,
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            ))
        }
        OpCode::Rename | OpCode::RenameNx => {
            if parts.len() != 3 {
                return Err(wrong_arity(if op == OpCode::RenameNx {
                    "renamenx"
                } else {
                    "rename"
                }));
            }
            let source = part_to_bytes(&parts[1])?;
            let destination = part_to_bytes(&parts[2])?;
            let source = validate_user_key(&source)?;
            let destination = validate_user_key(&destination)?;
            let mut cmd = Command::new(
                op,
                vec![source],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = vec![destination];
            if op == OpCode::RenameNx {
                cmd.expire_at_ms = 1;
            }
            Ok(cmd)
        }
        OpCode::Sort | OpCode::SortRo => {
            if parts.len() < 2 {
                return Err(wrong_arity(if op == OpCode::SortRo {
                    "sort_ro"
                } else {
                    "sort"
                }));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut alpha = false;
            let mut desc = false;
            let mut store = Bytes::new();
            let mut limit_offset: i64 = 0;
            let mut limit_count: i64 = -1;
            let mut index = 2usize;
            while index < parts.len() {
                let option = part_to_bytes(&parts[index])?;
                if ascii_eq_ci(option.as_ref(), b"ALPHA") {
                    alpha = true;
                    index += 1;
                } else if ascii_eq_ci(option.as_ref(), b"ASC") {
                    desc = false;
                    index += 1;
                } else if ascii_eq_ci(option.as_ref(), b"DESC") {
                    desc = true;
                    index += 1;
                } else if ascii_eq_ci(option.as_ref(), b"LIMIT") {
                    if index + 2 >= parts.len() {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    limit_offset = parse_i64_error_arg(
                        part_to_bytes(&parts[index + 1])?.as_ref(),
                        "value is not an integer or out of range",
                    )?;
                    limit_count = parse_i64_error_arg(
                        part_to_bytes(&parts[index + 2])?.as_ref(),
                        "value is not an integer or out of range",
                    )?;
                    index += 3;
                } else if ascii_eq_ci(option.as_ref(), b"STORE") {
                    if op == OpCode::SortRo || index + 1 >= parts.len() {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    // Assigned, not shadowed: `store` is declared before the
                    // option loop, so the prefixed name has to land there.
                    store = validate_user_key(&part_to_bytes(&parts[index + 1])?)?;
                    index += 2;
                } else {
                    return Err(ParseError::Protocol("syntax error"));
                }
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = vec![
                store,
                Bytes::from_static(if alpha { b"1" } else { b"0" }),
                Bytes::from_static(if desc { b"1" } else { b"0" }),
                Bytes::from(limit_offset.to_string()),
                Bytes::from(limit_count.to_string()),
            ];
            Ok(cmd)
        }
        OpCode::Set => {
            if parts.len() < 3 {
                return Err(wrong_arity("set"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let val = part_to_bytes(&parts[2])?;
            let mut cmd = Command::new(op, vec![key], Some(val), Vec::new());
            let mut index = 3;
            let mut saw_expiry = false;
            while index < parts.len() {
                let arg = part_to_bytes(&parts[index])?;
                if ascii_eq_ci(arg.as_ref(), b"NX") {
                    if cmd.set_condition != SetCondition::None {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    cmd.set_condition = SetCondition::Nx;
                    index += 1;
                } else if ascii_eq_ci(arg.as_ref(), b"XX") {
                    if cmd.set_condition != SetCondition::None {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    cmd.set_condition = SetCondition::Xx;
                    index += 1;
                } else if ascii_eq_ci(arg.as_ref(), b"GET") {
                    cmd.set_return_old = true;
                    index += 1;
                } else if ascii_eq_ci(arg.as_ref(), b"KEEPTTL") {
                    if saw_expiry {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    cmd.set_keep_ttl = true;
                    index += 1;
                } else if ascii_eq_ci(arg.as_ref(), b"EX")
                    || ascii_eq_ci(arg.as_ref(), b"PX")
                    || ascii_eq_ci(arg.as_ref(), b"EXAT")
                    || ascii_eq_ci(arg.as_ref(), b"PXAT")
                {
                    if index + 1 >= parts.len() {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    let ttl = part_to_bytes(&parts[index + 1])?;
                    cmd.expire_at_ms = ttl_ms_from_args_for(arg.as_ref(), ttl.as_ref(), "set")?;
                    if cmd.set_keep_ttl || saw_expiry {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    saw_expiry = true;
                    index += 2;
                } else {
                    return Err(ParseError::Protocol("syntax error"));
                }
            }
            Ok(cmd)
        }
        OpCode::SetEx | OpCode::PSetEx => {
            if parts.len() != 4 {
                return Err(wrong_arity(if op == OpCode::SetEx {
                    "setex"
                } else {
                    "psetex"
                }));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let ttl = part_to_bytes(&parts[2])?;
            let val = part_to_bytes(&parts[3])?;
            let mut cmd = Command::new(
                op,
                vec![key],
                Some(val),
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.expire_at_ms = ttl_ms_from_args_for(
                if op == OpCode::SetEx { b"EX" } else { b"PX" },
                ttl.as_ref(),
                if op == OpCode::SetEx {
                    "setex"
                } else {
                    "psetex"
                },
            )?;
            Ok(cmd)
        }
        OpCode::MSet | OpCode::MSetNx => {
            if parts.len() < 3 || parts.len() % 2 == 0 {
                return Err(wrong_arity(if op == OpCode::MSet {
                    "mset"
                } else {
                    "msetnx"
                }));
            }
            let mut keys = Vec::with_capacity((parts.len() - 1) / 2);
            let mut values = Vec::with_capacity((parts.len() - 1) / 2);
            for pair in parts[1..].chunks_exact(2) {
                let key = part_to_bytes(&pair[0])?;
                let key = validate_user_key(&key)?;
                let value = part_to_bytes(&pair[1])?;
                if let Some(index) = keys
                    .iter()
                    .position(|existing: &Bytes| existing.as_ref() == key.as_ref())
                {
                    values[index] = value;
                } else {
                    keys.push(key);
                    values.push(value);
                }
            }
            let mut cmd = Command::new(
                op,
                keys,
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = values;
            Ok(cmd)
        }
        OpCode::GetSet
        | OpCode::SetNx
        | OpCode::Append
        | OpCode::IncrBy
        | OpCode::DecrBy
        | OpCode::IncrByFloat => {
            if parts.len() != 3 {
                return Err(wrong_arity(match op {
                    OpCode::GetSet => "getset",
                    OpCode::SetNx => "setnx",
                    OpCode::Append => "append",
                    OpCode::IncrBy => "incrby",
                    OpCode::DecrBy => "decrby",
                    OpCode::IncrByFloat => "incrbyfloat",
                    _ => "command",
                }));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut val = part_to_bytes(&parts[2])?;
            if op == OpCode::DecrBy {
                // DECRBY runs as INCRBY with the amount negated, so the amount
                // is parsed here rather than in the executor. Redis's own texts
                // for the two ways that can fail: a decrement that is not a
                // 64-bit integer, and -LLONG_MIN, which has no positive
                // counterpart.
                let amount: i64 = std::str::from_utf8(val.as_ref())
                    .ok()
                    .and_then(|text| text.parse().ok())
                    .ok_or(ParseError::Error("value is not an integer or out of range"))?;
                let negated = amount
                    .checked_neg()
                    .ok_or(ParseError::Error("decrement would overflow"))?;
                val = Bytes::from(negated.to_string());
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                Some(val),
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            if op == OpCode::GetSet {
                cmd.set_return_old = true;
            } else if op == OpCode::SetNx {
                cmd.set_condition = SetCondition::Nx;
                cmd.set_integer_reply = true;
            }
            Ok(cmd)
        }
        OpCode::StrLen | OpCode::Incr | OpCode::Decr => {
            if parts.len() != 2 {
                return Err(wrong_arity(match op {
                    OpCode::StrLen => "strlen",
                    OpCode::Incr => "incr",
                    OpCode::Decr => "decr",
                    _ => "command",
                }));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            if op == OpCode::Incr {
                cmd.val = Some(Bytes::from_static(b"1"));
            } else if op == OpCode::Decr {
                cmd.val = Some(Bytes::from_static(b"-1"));
            }
            Ok(cmd)
        }
        OpCode::SetBit | OpCode::GetBit => {
            let expected_len = if op == OpCode::SetBit { 4 } else { 3 };
            if parts.len() != expected_len {
                return Err(wrong_arity(if op == OpCode::SetBit {
                    "setbit"
                } else {
                    "getbit"
                }));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let offset_arg = part_to_bytes(&parts[2])?;
            let offset = parse_i64_error_arg(
                offset_arg.as_ref(),
                "bit offset is not an integer or out of range",
            )?;
            if !(0..=(u32::MAX as i64)).contains(&offset) {
                return Err(ParseError::Error(
                    "bit offset is not an integer or out of range",
                ));
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.expire_at_ms = offset;
            if op == OpCode::SetBit {
                let bit_arg = part_to_bytes(&parts[3])?;
                let bit =
                    parse_i64_error_arg(bit_arg.as_ref(), "bit is not an integer or out of range")?;
                if bit != 0 && bit != 1 {
                    return Err(ParseError::Error("bit is not an integer or out of range"));
                }
                cmd.val = Some(Bytes::from(bit.to_string()));
            }
            Ok(cmd)
        }
        OpCode::SetRange => {
            if parts.len() != 4 {
                return Err(wrong_arity("setrange"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let offset_arg = part_to_bytes(&parts[2])?;
            let offset = parse_i64_error_arg(offset_arg.as_ref(), "offset is out of range")?;
            if offset < 0 {
                return Err(ParseError::Error("offset is out of range"));
            }
            let value = part_to_bytes(&parts[3])?;
            let max_string_size = 512_i64 * 1024 * 1024;
            if offset.saturating_add(value.len() as i64) > max_string_size {
                return Err(ParseError::Error("string exceeds maximum allowed size"));
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                Some(value),
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.expire_at_ms = offset;
            Ok(cmd)
        }
        OpCode::GetRange => {
            if parts.len() != 4 {
                return Err(wrong_arity("getrange"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let start = part_to_bytes(&parts[2])?;
            let end = part_to_bytes(&parts[3])?;
            parse_i64_arg(start.as_ref())?;
            parse_i64_arg(end.as_ref())?;
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = vec![start, end];
            Ok(cmd)
        }
        OpCode::Lcs => {
            if parts.len() < 3 {
                return Err(wrong_arity("lcs"));
            }
            let key1 = part_to_bytes(&parts[1])?;
            let key2 = part_to_bytes(&parts[2])?;
            let key1 = validate_user_key(&key1)?;
            let key2 = validate_user_key(&key2)?;
            let mut index = 3usize;
            let mut saw_len = false;
            let mut saw_idx = false;
            let mut saw_with_match_len = false;
            while index < parts.len() {
                let arg = part_to_bytes(&parts[index])?;
                if ascii_eq_ci(arg.as_ref(), b"LEN") {
                    if saw_idx || saw_len {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    saw_len = true;
                    index += 1;
                } else if ascii_eq_ci(arg.as_ref(), b"IDX") {
                    if saw_len || saw_idx {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    saw_idx = true;
                    index += 1;
                } else if ascii_eq_ci(arg.as_ref(), b"WITHMATCHLEN") {
                    if !saw_idx || saw_with_match_len {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    saw_with_match_len = true;
                    index += 1;
                } else if ascii_eq_ci(arg.as_ref(), b"MINMATCHLEN") {
                    if !saw_idx || index + 1 >= parts.len() {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    let min_len = part_to_bytes(&parts[index + 1])?;
                    let parsed = parse_i64_arg(min_len.as_ref())?;
                    if parsed < 0 {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    index += 2;
                } else {
                    return Err(ParseError::Protocol("syntax error"));
                }
            }
            Ok(Command::new(
                op,
                vec![key1, key2],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            ))
        }
        OpCode::Dump => {
            if parts.len() != 2 {
                return Err(wrong_arity("dump"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            Ok(Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            ))
        }
        OpCode::Restore => {
            if parts.len() < 4 {
                return Err(wrong_arity("restore"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            parse_i64_arg(part_to_bytes(&parts[2])?.as_ref())?;
            let payload = part_to_bytes(&parts[3])?;
            let ttl_ms = parse_i64_arg(part_to_bytes(&parts[2])?.as_ref())?;
            let (restore_kind, prefix_len) = if payload.starts_with(MAKO_LIST_DUMP_PREFIX) {
                (0u8, MAKO_LIST_DUMP_PREFIX.len())
            } else if payload.starts_with(MAKO_HASH_DUMP_PREFIX) {
                (1u8, MAKO_HASH_DUMP_PREFIX.len())
            } else if payload.starts_with(MAKO_STRING_DUMP_PREFIX) {
                (2u8, MAKO_STRING_DUMP_PREFIX.len())
            } else if payload.starts_with(MAKO_SET_DUMP_PREFIX) {
                (3u8, MAKO_SET_DUMP_PREFIX.len())
            } else if payload.starts_with(MAKO_ZSET_DUMP_PREFIX) {
                (4u8, MAKO_ZSET_DUMP_PREFIX.len())
            } else if payload.starts_with(MAKO_STREAM_DUMP_PREFIX) {
                (5u8, MAKO_STREAM_DUMP_PREFIX.len())
            } else {
                return Err(ParseError::Error(
                    "DUMP payload version or checksum are wrong",
                ));
            };
            let fields: Vec<Vec<u8>> = if restore_kind == 2 {
                vec![payload[prefix_len..].to_vec()]
            } else {
                parse_list_payload(&payload[prefix_len..]).ok_or(ParseError::Error(
                    "DUMP payload version or checksum are wrong",
                ))?
            };
            if (restore_kind == 1 || restore_kind == 4) && fields.len() % 2 != 0 {
                return Err(ParseError::Error(
                    "DUMP payload version or checksum are wrong",
                ));
            }
            // A stream payload is [family letter, key suffix, value] triples.
            if restore_kind == 5 && fields.len() % 3 != 0 {
                return Err(ParseError::Error(
                    "DUMP payload version or checksum are wrong",
                ));
            }
            let mut absttl = false;
            let mut index = 4usize;
            while index < parts.len() {
                let arg = part_to_bytes(&parts[index])?;
                if ascii_eq_ci(arg.as_ref(), b"ABSTTL") {
                    absttl = true;
                }
                if ascii_eq_ci(arg.as_ref(), b"REPLACE")
                    || ascii_eq_ci(arg.as_ref(), b"ABSTTL")
                    || ascii_eq_ci(arg.as_ref(), b"IDLETIME")
                    || ascii_eq_ci(arg.as_ref(), b"FREQ")
                {
                    if ascii_eq_ci(arg.as_ref(), b"IDLETIME") || ascii_eq_ci(arg.as_ref(), b"FREQ")
                    {
                        if index + 1 >= parts.len() {
                            return Err(ParseError::Protocol("syntax error"));
                        }
                        parse_i64_arg(part_to_bytes(&parts[index + 1])?.as_ref())?;
                        index += 2;
                    } else {
                        index += 1;
                    }
                } else {
                    return Err(ParseError::Protocol("syntax error"));
                }
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                Some(payload),
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = fields.into_iter().map(Bytes::from).collect();
            cmd.scan_type_matches = restore_kind == 1;
            cmd.restore_kind = restore_kind;
            if ttl_ms > 0 {
                cmd.expire_at_ms = if absttl {
                    ttl_ms
                } else {
                    unix_time_ms().saturating_add(ttl_ms)
                };
            }
            Ok(cmd)
        }
        OpCode::Copy => {
            if parts.len() < 3 {
                return Err(wrong_arity("copy"));
            }
            let raw_source = part_to_bytes(&parts[1])?;
            let raw_destination = part_to_bytes(&parts[2])?;
            let source = validate_user_key(&raw_source)?;
            // Validated here so `COPY k \x02x` is rejected whatever DB says;
            // the name is re-prefixed below once DB has been read.
            let mut destination = validate_user_key(&raw_destination)?;
            let mut replace = false;
            let mut index = 3usize;
            while index < parts.len() {
                let arg = part_to_bytes(&parts[index])?;
                if ascii_eq_ci(arg.as_ref(), b"REPLACE") {
                    if replace {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    replace = true;
                    index += 1;
                } else if ascii_eq_ci(arg.as_ref(), b"DB") {
                    if index + 1 >= parts.len() {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    let target = parse_db_index(part_to_bytes(&parts[index + 1])?.as_ref())?;
                    if target == current_db() && raw_source == raw_destination {
                        return Err(ParseError::Error(
                            "source and destination objects are the same",
                        ));
                    }
                    destination = db_key(target, &raw_destination);
                    index += 2;
                } else {
                    return Err(ParseError::Protocol("syntax error"));
                }
            }
            let mut cmd = Command::new(
                op,
                vec![source],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = vec![destination];
            cmd.expire_at_ms = if replace { 1 } else { 0 };
            Ok(cmd)
        }
        OpCode::Move => {
            if parts.len() != 3 {
                return Err(wrong_arity("move"));
            }
            let raw_key = part_to_bytes(&parts[1])?;
            let source = validate_user_key(&raw_key)?;
            let target = parse_db_index(part_to_bytes(&parts[2])?.as_ref())?;
            if target == current_db() {
                return Err(ParseError::Error(
                    "source and destination objects are the same",
                ));
            }
            let mut cmd = Command::new(
                op,
                vec![source],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            // The destination is the same Redis-visible name spelled for the
            // target database. It travels in `values` so the executor's lock
            // stripes pick it up through add_packed_lock_keys, like SMOVE's.
            cmd.values = vec![db_key(target, &raw_key)];
            Ok(cmd)
        }
        OpCode::BLPop | OpCode::BRPop => {
            if parts.len() < 3 {
                return Err(wrong_arity(if op == OpCode::BLPop {
                    "blpop"
                } else {
                    "brpop"
                }));
            }
            let timeout_ms =
                parse_blocking_timeout_ms(part_to_bytes(parts.last().unwrap())?.as_ref())?;
            let mut keys = Vec::with_capacity(parts.len() - 2);
            for part in parts.iter().skip(1).take(parts.len() - 2) {
                let key = part_to_bytes(part)?;
                let key = validate_user_key(&key)?;
                keys.push(key);
            }
            let mut cmd = Command::new(
                op,
                keys,
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.expire_at_ms = timeout_ms;
            Ok(cmd)
        }
        OpCode::BLMPop | OpCode::LMPop => {
            let first_key_index = if op == OpCode::BLMPop { 3 } else { 2 };
            let numkeys_index = if op == OpCode::BLMPop { 2 } else { 1 };
            if parts.len() < first_key_index + 2 {
                return Err(wrong_arity(if op == OpCode::BLMPop {
                    "blmpop"
                } else {
                    "lmpop"
                }));
            }
            let timeout_ms = if op == OpCode::BLMPop {
                parse_blocking_timeout_ms(part_to_bytes(&parts[1])?.as_ref())?
            } else {
                0
            };
            let numkeys = parse_i64_error_arg(
                part_to_bytes(&parts[numkeys_index])?.as_ref(),
                "numkeys should be greater than 0",
            )?;
            if numkeys <= 0 {
                return Err(ParseError::Error("numkeys should be greater than 0"));
            }
            let numkeys = numkeys as usize;
            if parts.len() < first_key_index + numkeys + 1 {
                return Err(ParseError::Error("syntax error"));
            }
            let mut keys = Vec::with_capacity(numkeys);
            for part in parts.iter().skip(first_key_index).take(numkeys) {
                let key = part_to_bytes(part)?;
                let key = validate_user_key(&key)?;
                keys.push(key);
            }
            let direction = part_to_bytes(&parts[first_key_index + numkeys])?;
            let flags = if ascii_eq_ci(direction.as_ref(), b"LEFT") {
                TXN_FLAG_LIST_SOURCE_LEFT
            } else if ascii_eq_ci(direction.as_ref(), b"RIGHT") {
                0
            } else {
                return Err(ParseError::Error("syntax error"));
            };
            let mut count = 1i64;
            let mut saw_count = false;
            let mut index = first_key_index + numkeys + 1;
            while index < parts.len() {
                let arg = part_to_bytes(&parts[index])?;
                if ascii_eq_ci(arg.as_ref(), b"COUNT") {
                    if saw_count {
                        return Err(ParseError::Error("syntax error"));
                    }
                    saw_count = true;
                    if index + 1 >= parts.len() {
                        return Err(ParseError::Error("syntax error"));
                    }
                    count = parse_i64_error_arg(
                        part_to_bytes(&parts[index + 1])?.as_ref(),
                        "count should be greater than 0",
                    )?;
                    if count <= 0 {
                        return Err(ParseError::Error("count should be greater than 0"));
                    }
                    index += 2;
                } else {
                    return Err(ParseError::Error("syntax error"));
                }
            }
            let mut cmd = Command::new(
                op,
                keys,
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.expire_flags = flags;
            cmd.set_count = Some(count);
            cmd.expire_at_ms = timeout_ms;
            Ok(cmd)
        }
        OpCode::Expire | OpCode::PExpire | OpCode::ExpireAt | OpCode::PExpireAt => {
            if parts.len() < 3 {
                return Err(wrong_arity(match op {
                    OpCode::Expire => "expire",
                    OpCode::PExpire => "pexpire",
                    OpCode::ExpireAt => "expireat",
                    OpCode::PExpireAt => "pexpireat",
                    _ => "command",
                }));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let ttl = part_to_bytes(&parts[2])?;
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            let unit = match op {
                OpCode::Expire => b"EXPIRE".as_slice(),
                OpCode::PExpire => b"PEXPIRE".as_slice(),
                OpCode::ExpireAt => b"EXPIREAT".as_slice(),
                OpCode::PExpireAt => b"PEXPIREAT".as_slice(),
                _ => b"",
            };
            let command = match op {
                OpCode::Expire => "expire",
                OpCode::PExpire => "pexpire",
                OpCode::ExpireAt => "expireat",
                OpCode::PExpireAt => "pexpireat",
                _ => "expire",
            };
            cmd.expire_at_ms = expire_at_ms_from_args(unit, ttl.as_ref(), command)?;
            let mut index = 3;
            while index < parts.len() {
                let arg = part_to_bytes(&parts[index])?;
                let flag = parse_expire_modifier(arg.as_ref())?;
                if (cmd.expire_flags & flag) != 0 {
                    return Err(ParseError::Protocol("syntax error"));
                }
                cmd.expire_flags |= flag;
                validate_expire_flags(cmd.expire_flags)?;
                index += 1;
            }
            Ok(cmd)
        }
        OpCode::GetEx => {
            if parts.len() != 2 && parts.len() != 3 && parts.len() != 4 {
                return Err(wrong_arity("getex"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            if parts.len() == 2 {
                return Ok(cmd);
            }
            let option = part_to_bytes(&parts[2])?;
            if ascii_eq_ci(option.as_ref(), b"PERSIST") {
                if parts.len() != 3 {
                    return Err(ParseError::Protocol("syntax error"));
                }
                cmd.set_keep_ttl = true;
                return Ok(cmd);
            }
            if parts.len() != 4 {
                return Err(ParseError::Protocol("syntax error"));
            }
            if !ascii_eq_ci(option.as_ref(), b"EX")
                && !ascii_eq_ci(option.as_ref(), b"PX")
                && !ascii_eq_ci(option.as_ref(), b"EXAT")
                && !ascii_eq_ci(option.as_ref(), b"PXAT")
            {
                return Err(ParseError::Protocol("syntax error"));
            }
            let ttl = part_to_bytes(&parts[3])?;
            cmd.expire_at_ms = ttl_ms_from_args_for(option.as_ref(), ttl.as_ref(), "getex")?;
            Ok(cmd)
        }
        OpCode::GetDel => {
            if parts.len() != 2 {
                return Err(wrong_arity("getdel"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            Ok(Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            ))
        }
        OpCode::Ttl | OpCode::PTtl | OpCode::ExpireTime | OpCode::PExpireTime | OpCode::Persist => {
            if parts.len() != 2 {
                return Err(wrong_arity(match op {
                    OpCode::Ttl => "ttl",
                    OpCode::PTtl => "pttl",
                    OpCode::ExpireTime => "expiretime",
                    OpCode::PExpireTime => "pexpiretime",
                    OpCode::Persist => "persist",
                    _ => "command",
                }));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            Ok(Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            ))
        }
        OpCode::Keys => {
            if parts.len() != 2 {
                return Err(wrong_arity("keys"));
            }
            let pattern = part_to_bytes(&parts[1])?;
            let mut cmd = Command::new(
                op,
                Vec::new(),
                Some(pattern),
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.scan_prefix = literal_prefix(cmd.val.as_ref().unwrap().as_ref());
            // The selected database's prefix goes in front of the literal
            // MATCH prefix, which is what scopes the walk; for database 0 it
            // is the identity and the executor skips the 0x02 namespace on its
            // own. The pattern is matched against the stripped name in the
            // reply, so it keeps meaning what the client wrote.
            cmd.scan_prefix = db_key(current_db(), &cmd.scan_prefix);
            cmd.scan_count = 1_000_000;
            Ok(cmd)
        }
        OpCode::Scan => {
            if parts.len() < 2 {
                return Err(wrong_arity("scan"));
            }
            let cursor_arg = part_to_bytes(&parts[1])?;
            let cursor = scan_cursor_from_arg(cursor_arg.as_ref())?;
            let mut cmd = Command::new(
                op,
                vec![cursor],
                Some(Bytes::from_static(b"*")),
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            let mut index = 2;
            while index < parts.len() {
                let option = part_to_bytes(&parts[index])?;
                if ascii_eq_ci(option.as_ref(), b"MATCH") {
                    if index + 1 >= parts.len() {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    cmd.val = Some(part_to_bytes(&parts[index + 1])?);
                    cmd.scan_prefix = literal_prefix(cmd.val.as_ref().unwrap().as_ref());
                    index += 2;
                } else if ascii_eq_ci(option.as_ref(), b"COUNT") {
                    if index + 1 >= parts.len() {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    let count_arg = part_to_bytes(&parts[index + 1])?;
                    cmd.scan_count = parse_positive_i64(count_arg.as_ref())?;
                    index += 2;
                } else if ascii_eq_ci(option.as_ref(), b"TYPE") {
                    if index + 1 >= parts.len() {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    let type_arg = part_to_bytes(&parts[index + 1])?;
                    if !ascii_eq_ci(type_arg.as_ref(), b"string") {
                        cmd.scan_type_matches = false;
                    }
                    index += 2;
                } else {
                    return Err(ParseError::Protocol("syntax error"));
                }
            }
            if cmd.scan_prefix.is_empty() {
                cmd.scan_prefix = literal_prefix(cmd.val.as_ref().unwrap().as_ref());
            }
            if !cmd.scan_type_matches {
                cmd.scan_prefix = Bytes::from_static(b"\x01");
            }
            // As in KEYS. A TYPE that is not "string" still matches nothing:
            // no key in any database may start with 0x01.
            cmd.scan_prefix = db_key(current_db(), &cmd.scan_prefix);
            Ok(cmd)
        }
        OpCode::DbSize => {
            if parts.len() != 1 {
                return Err(wrong_arity("dbsize"));
            }
            let mut cmd = Command::new(
                op,
                Vec::new(),
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            // Counts one database, through the same scan prefix KEYS uses. An
            // empty prefix is database 0, which keeps the executor's chunked
            // count-only fast path (it requires an empty prefix) exactly where
            // it was.
            cmd.scan_prefix = db_scan_prefix(current_db());
            Ok(cmd)
        }
        OpCode::Type => {
            if parts.len() != 2 {
                return Err(wrong_arity("type"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            Ok(Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            ))
        }
        OpCode::XAdd => {
            if parts.len() < 5 {
                return Err(wrong_arity("xadd"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut index = 2usize;
            let mut nomkstream = false;
            let mut trim = StreamTrimArgs::default();
            while index < parts.len() {
                let arg = part_to_bytes(&parts[index])?;
                if ascii_eq_ci(arg.as_ref(), b"NOMKSTREAM") {
                    nomkstream = true;
                    index += 1;
                } else if ascii_eq_ci(arg.as_ref(), b"MAXLEN")
                    || ascii_eq_ci(arg.as_ref(), b"MINID")
                {
                    index = parse_stream_trim_args(&parts, index, &mut trim)?;
                } else {
                    break;
                }
            }
            if index >= parts.len() {
                return Err(wrong_arity("xadd"));
            }
            let id_raw = part_to_bytes(&parts[index])?;
            index += 1;
            // "*" and "<ms>-*" reach the executor as they were written; every
            // other spelling is normalized to "<ms>-<seq>". A bare "<ms>" is an
            // explicit "<ms>-0", which is why `XADD key 666 f v` fails once the
            // stream's last ID is 666-0.
            let id_spec = if id_raw.as_ref() == b"*" {
                Bytes::from_static(b"*")
            } else {
                let (id, seq_given) = parse_stream_id_generic(id_raw.as_ref(), 0, true, true)
                    .ok_or(ParseError::Error(STREAM_ID_ERROR))?;
                if !seq_given {
                    Bytes::from(format!("{}-*", id.ms))
                } else if id == STREAM_ID_MIN {
                    // Checked before the key is looked at, as Redis does, so a
                    // rejected XADD never creates the stream.
                    return Err(ParseError::Error(
                        "The ID specified in XADD must be greater than 0-0",
                    ));
                } else {
                    id.text()
                }
            };
            let field_count = parts.len() - index;
            if field_count < 2 || field_count % 2 != 0 {
                return Err(wrong_arity("xadd"));
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values.push(id_spec);
            cmd.values.push(stream_bool_arg(nomkstream));
            cmd.values.extend(trim.as_values());
            for part in &parts[index..] {
                cmd.values.push(part_to_bytes(part)?);
            }
            Ok(cmd)
        }
        OpCode::XRange | OpCode::XRevRange => {
            let forward = op == OpCode::XRange;
            let name = if forward { "xrange" } else { "xrevrange" };
            if parts.len() != 4 && parts.len() != 6 {
                return Err(wrong_arity(name));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            // XREVRANGE takes the range the other way round.
            let (start_raw, end_raw) = if forward {
                (part_to_bytes(&parts[2])?, part_to_bytes(&parts[3])?)
            } else {
                (part_to_bytes(&parts[3])?, part_to_bytes(&parts[2])?)
            };
            let start = parse_stream_range_start(start_raw.as_ref())?;
            let end = parse_stream_range_end(end_raw.as_ref())?;
            let mut count = 0i64;
            if parts.len() == 6 {
                let option = part_to_bytes(&parts[4])?;
                if !ascii_eq_ci(option.as_ref(), b"COUNT") {
                    return Err(ParseError::Error("syntax error"));
                }
                count = parse_i64_error_arg(
                    part_to_bytes(&parts[5])?.as_ref(),
                    "value is not an integer or out of range",
                )?;
                if count < 0 {
                    count = 0;
                }
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = vec![
                start.text(),
                end.text(),
                Bytes::from(count.to_string()),
                Bytes::from_static(if forward {
                    STREAM_READ_FORWARD
                } else {
                    STREAM_READ_REVERSE
                }),
            ];
            Ok(cmd)
        }
        OpCode::XRead | OpCode::XReadGroup => {
            let group_form = op == OpCode::XReadGroup;
            let name = if group_form { "xreadgroup" } else { "xread" };
            if parts.len() < 4 {
                return Err(wrong_arity(name));
            }
            let mut count = 0i64;
            let mut block_ms = -1i64;
            let mut noack = false;
            let mut group = Bytes::new();
            let mut consumer = Bytes::new();
            let mut have_group = false;
            let mut index = 1usize;
            let mut streams_at = None;
            while index < parts.len() {
                let arg = part_to_bytes(&parts[index])?;
                if ascii_eq_ci(arg.as_ref(), b"COUNT") && index + 1 < parts.len() {
                    count = parse_i64_error_arg(
                        part_to_bytes(&parts[index + 1])?.as_ref(),
                        "value is not an integer or out of range",
                    )?;
                    if count < 0 {
                        count = 0;
                    }
                    index += 2;
                } else if ascii_eq_ci(arg.as_ref(), b"BLOCK") && index + 1 < parts.len() {
                    block_ms = parse_i64_error_arg(
                        part_to_bytes(&parts[index + 1])?.as_ref(),
                        "timeout is not an integer or out of range",
                    )?;
                    if block_ms < 0 {
                        return Err(ParseError::Error("timeout is negative"));
                    }
                    index += 2;
                } else if ascii_eq_ci(arg.as_ref(), b"NOACK") {
                    if !group_form {
                        return Err(ParseError::Error(
                            "The NOACK option is only supported by XREADGROUP. You called \
                             XREAD instead.",
                        ));
                    }
                    noack = true;
                    index += 1;
                } else if ascii_eq_ci(arg.as_ref(), b"GROUP") && index + 2 < parts.len() {
                    if !group_form {
                        return Err(ParseError::Error(
                            "The GROUP option is only supported by XREADGROUP. You called \
                             XREAD instead.",
                        ));
                    }
                    group = part_to_bytes(&parts[index + 1])?;
                    consumer = part_to_bytes(&parts[index + 2])?;
                    have_group = true;
                    index += 3;
                } else if ascii_eq_ci(arg.as_ref(), b"STREAMS") {
                    streams_at = Some(index + 1);
                    break;
                } else {
                    return Err(ParseError::Error("syntax error"));
                }
            }
            if group_form && !have_group {
                return Err(ParseError::Error(
                    "Missing GROUP keyword or consumer/group name in XREADGROUP context",
                ));
            }
            let Some(streams_at) = streams_at else {
                return Err(ParseError::Error("syntax error"));
            };
            let rest = parts.len() - streams_at;
            if rest == 0 || rest % 2 != 0 {
                return Err(if group_form {
                    ParseError::Error(
                        "Unbalanced 'xreadgroup' list of streams: for each stream key an ID \
                         or '>' must be specified.",
                    )
                } else {
                    ParseError::Error(
                        "Unbalanced 'xread' list of streams: for each stream key an ID or '$' \
                         must be specified.",
                    )
                });
            }
            let stream_count = rest / 2;
            let mut cmd = Command::new(
                op,
                Vec::with_capacity(stream_count),
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            for part in &parts[streams_at..streams_at + stream_count] {
                let key = part_to_bytes(part)?;
                let key = validate_user_key(&key)?;
                cmd.keys.push(key);
            }
            // The three fixed items every stream read carries, empty for
            // XREAD, then one ID spec per key in the same order as the keys.
            cmd.values.push(group);
            cmd.values.push(consumer);
            cmd.values.push(stream_bool_arg(noack));
            for part in &parts[streams_at + stream_count..] {
                let raw = part_to_bytes(part)?;
                if raw.as_ref() == b">" {
                    if !group_form {
                        return Err(ParseError::Error(
                            "The > ID can be specified only when calling XREADGROUP using the \
                             GROUP <group> <consumer> option.",
                        ));
                    }
                    cmd.values.push(raw);
                } else if raw.as_ref() == b"$" {
                    if group_form {
                        return Err(ParseError::Error(
                            "The $ ID is meaningless in the context of XREADGROUP: you want \
                             to read the history of this consumer by specifying a proper ID, \
                             or use the > ID to get new messages. The $ ID would just return \
                             an empty result set.",
                        ));
                    }
                    cmd.values.push(raw);
                } else if raw.as_ref() == b"+" && !group_form {
                    cmd.values.push(raw);
                } else {
                    cmd.values.push(parse_stream_id_strict(raw.as_ref(), 0)?.text());
                }
            }
            cmd.scan_count = count;
            cmd.expire_at_ms = block_ms;
            Ok(cmd)
        }
        OpCode::XGroup => {
            if parts.len() < 2 {
                return Err(wrong_arity("xgroup"));
            }
            let subcommand = part_to_bytes(&parts[1])?;
            let args = command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?;
            if ascii_eq_ci(subcommand.as_ref(), b"HELP") {
                if parts.len() != 2 {
                    return Err(wrong_arity("xgroup|help"));
                }
                let mut cmd = Command::new(op, Vec::new(), None, args);
                cmd.restore_kind = STREAM_XGROUP_HELP;
                return Ok(cmd);
            }
            let (kind, canonical, min_parts) = if ascii_eq_ci(subcommand.as_ref(), b"CREATE") {
                (STREAM_XGROUP_CREATE, "CREATE", 5)
            } else if ascii_eq_ci(subcommand.as_ref(), b"SETID") {
                (STREAM_XGROUP_SETID, "SETID", 5)
            } else if ascii_eq_ci(subcommand.as_ref(), b"DESTROY") {
                (STREAM_XGROUP_DESTROY, "DESTROY", 4)
            } else if ascii_eq_ci(subcommand.as_ref(), b"CREATECONSUMER") {
                (STREAM_XGROUP_CREATECONSUMER, "CREATECONSUMER", 5)
            } else if ascii_eq_ci(subcommand.as_ref(), b"DELCONSUMER") {
                (STREAM_XGROUP_DELCONSUMER, "DELCONSUMER", 5)
            } else {
                return Err(ParseError::Owned(format!(
                    "Unknown XGROUP subcommand or wrong number of arguments for '{}'",
                    String::from_utf8_lossy(subcommand.as_ref())
                )));
            };
            if parts.len() < min_parts {
                return Err(wrong_arity("xgroup"));
            }
            let key = part_to_bytes(&parts[2])?;
            let key = validate_user_key(&key)?;
            let group = part_to_bytes(&parts[3])?;
            let mut cmd = Command::new(op, vec![key], None, args);
            cmd.restore_kind = kind;
            cmd.values.push(Bytes::from_static(canonical.as_bytes()));
            cmd.values.push(group);
            match kind {
                STREAM_XGROUP_CREATE | STREAM_XGROUP_SETID => {
                    let raw = part_to_bytes(&parts[4])?;
                    // XGROUP takes "$" for "wherever the stream is now", and
                    // reads every other ID loosely, so `-` is 0-0.
                    cmd.values.push(if raw.as_ref() == b"$" {
                        raw
                    } else {
                        parse_stream_id_loose(raw.as_ref(), 0)?.text()
                    });
                    let mut mkstream = false;
                    let mut entries_read = Bytes::new();
                    let mut index = 5usize;
                    while index < parts.len() {
                        let arg = part_to_bytes(&parts[index])?;
                        if kind == STREAM_XGROUP_CREATE
                            && ascii_eq_ci(arg.as_ref(), b"MKSTREAM")
                        {
                            mkstream = true;
                            index += 1;
                        } else if ascii_eq_ci(arg.as_ref(), b"ENTRIESREAD")
                            && index + 1 < parts.len()
                        {
                            let value = parse_i64_error_arg(
                                part_to_bytes(&parts[index + 1])?.as_ref(),
                                "value is not an integer or out of range",
                            )?;
                            if value < -1 {
                                return Err(ParseError::Error(
                                    "value for ENTRIESREAD must be positive or -1",
                                ));
                            }
                            // -1 is Redis's "not known", which the executor
                            // spells as an absent value.
                            if value >= 0 {
                                entries_read = Bytes::from(value.to_string());
                            }
                            index += 2;
                        } else {
                            return Err(ParseError::Error("syntax error"));
                        }
                    }
                    if kind == STREAM_XGROUP_CREATE {
                        cmd.values.push(stream_bool_arg(mkstream));
                    }
                    cmd.values.push(entries_read);
                }
                STREAM_XGROUP_DESTROY => {
                    if parts.len() != 4 {
                        return Err(wrong_arity("xgroup"));
                    }
                }
                _ => {
                    if parts.len() != 5 {
                        return Err(wrong_arity("xgroup"));
                    }
                    cmd.values.push(part_to_bytes(&parts[4])?);
                }
            }
            Ok(cmd)
        }
        OpCode::XAck => {
            if parts.len() < 4 {
                return Err(wrong_arity("xack"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values.push(part_to_bytes(&parts[2])?);
            for part in &parts[3..] {
                cmd.values
                    .push(parse_stream_id_strict(part_to_bytes(part)?.as_ref(), 0)?.text());
            }
            Ok(cmd)
        }
        OpCode::XPending => {
            // XPENDING key group, or the range form with an optional IDLE
            // filter in front of it and an optional consumer after it.
            if !matches!(parts.len(), 3 | 6 | 7 | 8 | 9) {
                return Err(if parts.len() < 3 {
                    wrong_arity("xpending")
                } else {
                    ParseError::Error("syntax error")
                });
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values.push(part_to_bytes(&parts[2])?);
            if parts.len() == 3 {
                cmd.values.push(Bytes::from_static(b"SUMMARY"));
                cmd.restore_kind = STREAM_XPENDING_SUMMARY;
                return Ok(cmd);
            }
            let mut index = 3usize;
            let mut min_idle = 0i64;
            if ascii_eq_ci(part_to_bytes(&parts[3])?.as_ref(), b"IDLE") {
                if parts.len() < 8 {
                    return Err(ParseError::Error("syntax error"));
                }
                min_idle = parse_i64_error_arg(
                    part_to_bytes(&parts[4])?.as_ref(),
                    "value is not an integer or out of range",
                )?;
                if min_idle < 0 {
                    min_idle = 0;
                }
                index = 5;
            } else if parts.len() > 7 {
                return Err(ParseError::Error("syntax error"));
            }
            let start = parse_stream_range_start(part_to_bytes(&parts[index])?.as_ref())?;
            let end = parse_stream_range_end(part_to_bytes(&parts[index + 1])?.as_ref())?;
            let mut count = parse_i64_error_arg(
                part_to_bytes(&parts[index + 2])?.as_ref(),
                "value is not an integer or out of range",
            )?;
            if count < 0 {
                count = 0;
            }
            let consumer = if parts.len() == index + 4 {
                part_to_bytes(&parts[index + 3])?
            } else if parts.len() == index + 3 {
                Bytes::new()
            } else {
                return Err(ParseError::Error("syntax error"));
            };
            cmd.values.push(Bytes::from_static(b"RANGE"));
            cmd.values.push(Bytes::from(min_idle.to_string()));
            cmd.values.push(start.text());
            cmd.values.push(end.text());
            cmd.values.push(Bytes::from(count.to_string()));
            cmd.values.push(consumer);
            cmd.restore_kind = STREAM_XPENDING_RANGE;
            Ok(cmd)
        }
        OpCode::XClaim => {
            if parts.len() < 6 {
                return Err(wrong_arity("xclaim"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let group = part_to_bytes(&parts[2])?;
            let consumer = part_to_bytes(&parts[3])?;
            let mut min_idle = parse_i64_error_arg(
                part_to_bytes(&parts[4])?.as_ref(),
                "Invalid min-idle-time argument for XCLAIM",
            )?;
            if min_idle < 0 {
                min_idle = 0;
            }
            // The IDs run until the first argument that is not one; whatever
            // follows is options, as Redis's own scan does.
            let mut index = 5usize;
            let mut ids = Vec::new();
            while index < parts.len() {
                let raw = part_to_bytes(&parts[index])?;
                match parse_stream_id_generic(raw.as_ref(), 0, true, false) {
                    Some((id, _)) => {
                        ids.push(id.text());
                        index += 1;
                    }
                    None => break,
                }
            }
            if ids.is_empty() {
                return Err(ParseError::Error(STREAM_ID_ERROR));
            }
            let mut justid = false;
            let mut force = false;
            let mut idle = Bytes::new();
            let mut time = Bytes::new();
            let mut retrycount = Bytes::new();
            let mut last_id = Bytes::new();
            while index < parts.len() {
                let arg = part_to_bytes(&parts[index])?;
                if ascii_eq_ci(arg.as_ref(), b"JUSTID") {
                    justid = true;
                    index += 1;
                } else if ascii_eq_ci(arg.as_ref(), b"FORCE") {
                    force = true;
                    index += 1;
                } else if ascii_eq_ci(arg.as_ref(), b"IDLE") && index + 1 < parts.len() {
                    idle = Bytes::from(
                        parse_i64_error_arg(
                            part_to_bytes(&parts[index + 1])?.as_ref(),
                            "Invalid IDLE option argument for XCLAIM",
                        )?
                        .to_string(),
                    );
                    index += 2;
                } else if ascii_eq_ci(arg.as_ref(), b"TIME") && index + 1 < parts.len() {
                    time = Bytes::from(
                        parse_i64_error_arg(
                            part_to_bytes(&parts[index + 1])?.as_ref(),
                            "Invalid TIME option argument for XCLAIM",
                        )?
                        .to_string(),
                    );
                    index += 2;
                } else if ascii_eq_ci(arg.as_ref(), b"RETRYCOUNT") && index + 1 < parts.len() {
                    let value = parse_i64_error_arg(
                        part_to_bytes(&parts[index + 1])?.as_ref(),
                        "Invalid RETRYCOUNT option argument for XCLAIM",
                    )?;
                    retrycount = Bytes::from(value.max(0).to_string());
                    index += 2;
                } else if ascii_eq_ci(arg.as_ref(), b"LASTID") && index + 1 < parts.len() {
                    last_id = parse_stream_id_loose(
                        part_to_bytes(&parts[index + 1])?.as_ref(),
                        0,
                    )?
                    .text();
                    index += 2;
                } else {
                    return Err(ParseError::Error("syntax error"));
                }
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values.push(group);
            cmd.values.push(consumer);
            cmd.values.push(Bytes::from(min_idle.to_string()));
            cmd.values.push(stream_bool_arg(justid));
            cmd.values.push(stream_bool_arg(force));
            cmd.values.push(idle);
            cmd.values.push(time);
            cmd.values.push(retrycount);
            cmd.values.push(last_id);
            cmd.values.extend(ids);
            Ok(cmd)
        }
        OpCode::XAutoClaim => {
            if parts.len() < 6 {
                return Err(wrong_arity("xautoclaim"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let group = part_to_bytes(&parts[2])?;
            let consumer = part_to_bytes(&parts[3])?;
            let mut min_idle = parse_i64_error_arg(
                part_to_bytes(&parts[4])?.as_ref(),
                "Invalid min-idle-time argument for XAUTOCLAIM",
            )?;
            if min_idle < 0 {
                min_idle = 0;
            }
            let start = parse_stream_range_start(part_to_bytes(&parts[5])?.as_ref())?;
            let mut count = 100i64;
            let mut justid = false;
            let mut index = 6usize;
            while index < parts.len() {
                let arg = part_to_bytes(&parts[index])?;
                if ascii_eq_ci(arg.as_ref(), b"COUNT") && index + 1 < parts.len() {
                    count = parse_i64_error_arg(
                        part_to_bytes(&parts[index + 1])?.as_ref(),
                        "COUNT must be > 0",
                    )?;
                    // Redis refuses a count that its attempt budget
                    // (count * 10 entries) could not be expressed in.
                    if count < 1 || count > i64::MAX / 16 {
                        return Err(ParseError::Error("COUNT must be > 0"));
                    }
                    index += 2;
                } else if ascii_eq_ci(arg.as_ref(), b"JUSTID") {
                    justid = true;
                    index += 1;
                } else {
                    return Err(ParseError::Error("syntax error"));
                }
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values.push(group);
            cmd.values.push(consumer);
            cmd.values.push(Bytes::from(min_idle.to_string()));
            cmd.values.push(start.text());
            cmd.values.push(Bytes::from(count.to_string()));
            cmd.values.push(stream_bool_arg(justid));
            Ok(cmd)
        }
        OpCode::XLen => {
            if parts.len() != 2 {
                return Err(wrong_arity("xlen"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            Ok(Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            ))
        }
        OpCode::XDel => {
            if parts.len() < 3 {
                return Err(wrong_arity("xdel"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            for part in &parts[2..] {
                let id = parse_stream_id_strict(part_to_bytes(part)?.as_ref(), 0)?;
                cmd.values.push(id.text());
            }
            Ok(cmd)
        }
        OpCode::XTrim => {
            if parts.len() < 4 {
                return Err(wrong_arity("xtrim"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut trim = StreamTrimArgs::default();
            let index = parse_stream_trim_args(&parts, 2, &mut trim)?;
            if index != parts.len() {
                return Err(ParseError::Error("syntax error"));
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = trim.as_values().to_vec();
            Ok(cmd)
        }
        OpCode::XSetId => {
            if parts.len() < 3 {
                return Err(wrong_arity("xsetid"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let id = parse_stream_id_strict(part_to_bytes(&parts[2])?.as_ref(), 0)?;
            let mut entries_added = Bytes::new();
            let mut max_deleted = Bytes::new();
            let mut index = 3usize;
            while index < parts.len() {
                let arg = part_to_bytes(&parts[index])?;
                if ascii_eq_ci(arg.as_ref(), b"ENTRIESADDED") && index + 1 < parts.len() {
                    let count = parse_i64_error_arg(
                        part_to_bytes(&parts[index + 1])?.as_ref(),
                        "value is not an integer or out of range",
                    )?;
                    if count < 0 {
                        return Err(ParseError::Error("value for ENTRIESADDED must be positive"));
                    }
                    entries_added = Bytes::from(count.to_string());
                    index += 2;
                } else if ascii_eq_ci(arg.as_ref(), b"MAXDELETEDID") && index + 1 < parts.len() {
                    max_deleted =
                        parse_stream_id_strict(part_to_bytes(&parts[index + 1])?.as_ref(), 0)?
                            .text();
                    index += 2;
                } else {
                    return Err(ParseError::Error("syntax error"));
                }
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = vec![id.text(), entries_added, max_deleted];
            Ok(cmd)
        }
        OpCode::XInfo => {
            if parts.len() < 2 {
                return Err(wrong_arity("xinfo"));
            }
            let subcommand = part_to_bytes(&parts[1])?;
            let args = command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?;
            if ascii_eq_ci(subcommand.as_ref(), b"HELP") {
                if parts.len() != 2 {
                    return Err(wrong_arity("xinfo|help"));
                }
                let mut cmd = Command::new(op, Vec::new(), None, args);
                cmd.restore_kind = STREAM_XINFO_HELP;
                return Ok(cmd);
            }
            if ascii_eq_ci(subcommand.as_ref(), b"GROUPS")
                || ascii_eq_ci(subcommand.as_ref(), b"CONSUMERS")
            {
                let groups = ascii_eq_ci(subcommand.as_ref(), b"GROUPS");
                let wanted = if groups { 3 } else { 4 };
                if parts.len() != wanted {
                    return Err(wrong_arity(if groups {
                        "xinfo|groups"
                    } else {
                        "xinfo|consumers"
                    }));
                }
                let key = part_to_bytes(&parts[2])?;
                let key = validate_user_key(&key)?;
                let mut cmd = Command::new(op, vec![key], None, args);
                cmd.restore_kind = if groups {
                    STREAM_XINFO_GROUPS
                } else {
                    STREAM_XINFO_CONSUMERS
                };
                cmd.values.push(Bytes::from_static(if groups {
                    b"GROUPS"
                } else {
                    b"CONSUMERS"
                }));
                if !groups {
                    cmd.values.push(part_to_bytes(&parts[3])?);
                }
                return Ok(cmd);
            }
            if !ascii_eq_ci(subcommand.as_ref(), b"STREAM") {
                return Err(ParseError::Owned(format!(
                    "Unknown XINFO subcommand or wrong number of arguments for '{}'",
                    String::from_utf8_lossy(subcommand.as_ref())
                )));
            }
            if parts.len() < 3 {
                return Err(wrong_arity("xinfo|stream"));
            }
            let key = part_to_bytes(&parts[2])?;
            let key = validate_user_key(&key)?;
            let mut full = false;
            let mut count = 10i64;
            let mut index = 3usize;
            if index < parts.len() {
                let option = part_to_bytes(&parts[index])?;
                if !ascii_eq_ci(option.as_ref(), b"FULL") {
                    return Err(ParseError::Error("syntax error"));
                }
                full = true;
                index += 1;
                if index + 1 < parts.len() {
                    let option = part_to_bytes(&parts[index])?;
                    if !ascii_eq_ci(option.as_ref(), b"COUNT") {
                        return Err(ParseError::Error("syntax error"));
                    }
                    count = parse_i64_error_arg(
                        part_to_bytes(&parts[index + 1])?.as_ref(),
                        "value is not an integer or out of range",
                    )?;
                    if count < 0 {
                        count = 0;
                    }
                    index += 2;
                }
            }
            if index != parts.len() {
                return Err(ParseError::Error("syntax error"));
            }
            let mut cmd = Command::new(op, vec![key], None, args);
            cmd.restore_kind = if full {
                STREAM_XINFO_STREAM_FULL
            } else {
                STREAM_XINFO_STREAM
            };
            cmd.values = vec![
                Bytes::from_static(b"STREAM"),
                stream_bool_arg(full),
                Bytes::from(count.to_string()),
            ];
            Ok(cmd)
        }
        OpCode::HSet | OpCode::HMSet => {
            if parts.len() < 4 || parts.len() % 2 != 0 {
                return Err(wrong_arity(if op == OpCode::HSet {
                    "hset"
                } else {
                    "hmset"
                }));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut values = Vec::with_capacity(parts.len() - 2);
            for part in parts.iter().skip(2) {
                values.push(part_to_bytes(part)?);
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = values;
            Ok(cmd)
        }
        OpCode::HSetNx | OpCode::HGet | OpCode::HExists | OpCode::HStrLen => {
            let expected = if op == OpCode::HSetNx { 4 } else { 3 };
            if parts.len() != expected {
                return Err(wrong_arity(match op {
                    OpCode::HSetNx => "hsetnx",
                    OpCode::HGet => "hget",
                    OpCode::HExists => "hexists",
                    OpCode::HStrLen => "hstrlen",
                    _ => "hash",
                }));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut cmd = Command::new(
                op,
                vec![key],
                Some(part_to_bytes(&parts[2])?),
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            if op == OpCode::HSetNx {
                cmd.values = vec![part_to_bytes(&parts[2])?, part_to_bytes(&parts[3])?];
            }
            Ok(cmd)
        }
        OpCode::HIncrBy | OpCode::HIncrByFloat => {
            if parts.len() != 4 {
                return Err(wrong_arity(if op == OpCode::HIncrBy {
                    "hincrby"
                } else {
                    "hincrbyfloat"
                }));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let increment = part_to_bytes(&parts[3])?;
            if op == OpCode::HIncrBy {
                parse_i64_error_arg(
                    increment.as_ref(),
                    "value is not an integer or out of range",
                )?;
            } else {
                parse_f64_error_arg(increment.as_ref(), "value is not a valid float")?;
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                Some(part_to_bytes(&parts[2])?),
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = vec![part_to_bytes(&parts[2])?, increment];
            Ok(cmd)
        }
        OpCode::HMGet | OpCode::HDel => {
            if parts.len() < 3 {
                return Err(wrong_arity(if op == OpCode::HMGet {
                    "hmget"
                } else {
                    "hdel"
                }));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut values = Vec::with_capacity(parts.len() - 2);
            for part in parts.iter().skip(2) {
                values.push(part_to_bytes(part)?);
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = values;
            Ok(cmd)
        }
        OpCode::HGetAll | OpCode::HLen | OpCode::HKeys | OpCode::HVals => {
            if parts.len() != 2 {
                return Err(wrong_arity(match op {
                    OpCode::HGetAll => "hgetall",
                    OpCode::HLen => "hlen",
                    OpCode::HKeys => "hkeys",
                    OpCode::HVals => "hvals",
                    _ => "hash",
                }));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            Ok(Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            ))
        }
        OpCode::HRandField => {
            if parts.len() < 2 || parts.len() > 4 {
                return Err(wrong_arity("hrandfield"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            if parts.len() >= 3 {
                let count_arg = part_to_bytes(&parts[2])?;
                let count = parse_i64_arg(count_arg.as_ref())?;
                if count == i64::MIN || count.saturating_abs() > SET_RANDOM_COUNT_LIMIT {
                    return Err(ParseError::Protocol("value is out of range"));
                }
                cmd.set_count = Some(count);
            }
            if parts.len() == 4 {
                let withvalues = part_to_bytes(&parts[3])?;
                if !ascii_eq_ci(withvalues.as_ref(), b"WITHVALUES") {
                    return Err(ParseError::Protocol("syntax error"));
                }
                cmd.set_return_old = true;
            }
            Ok(cmd)
        }
        OpCode::HExpire | OpCode::HPExpire | OpCode::HExpireAt | OpCode::HPExpireAt => {
            // <cmd> key ttl [NX|XX|GT|LT] FIELDS numfields field [field ...]
            let (name, seconds, absolute) = match op {
                OpCode::HExpire => ("hexpire", true, false),
                OpCode::HPExpire => ("hpexpire", false, false),
                OpCode::HExpireAt => ("hexpireat", true, true),
                _ => ("hpexpireat", false, true),
            };
            if parts.len() < 6 {
                return Err(wrong_arity(name));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let ttl = part_to_bytes(&parts[2])?;
            // Redis reads at most one condition here, between the time and
            // FIELDS. A second one is reported as incompatible when it cannot
            // be combined, and otherwise falls through to the FIELDS check.
            let condition = part_to_bytes(&parts[3])?;
            let (mode, fields_at): (&'static [u8], usize) =
                if ascii_eq_ci(condition.as_ref(), b"NX") {
                    (b"NX", 4)
                } else if ascii_eq_ci(condition.as_ref(), b"XX") {
                    (b"XX", 4)
                } else if ascii_eq_ci(condition.as_ref(), b"GT") {
                    (b"GT", 4)
                } else if ascii_eq_ci(condition.as_ref(), b"LT") {
                    (b"LT", 4)
                } else {
                    (b"NONE", 3)
                };
            if fields_at == 4 && fields_at < parts.len() {
                let next = part_to_bytes(&parts[fields_at])?;
                if let Ok(second) = parse_expire_modifier(next.as_ref()) {
                    let first = parse_expire_modifier(mode)?;
                    validate_expire_flags(first | second)?;
                }
            }
            if fields_at >= parts.len()
                || !ascii_eq_ci(part_to_bytes(&parts[fields_at])?.as_ref(), b"FIELDS")
            {
                return Err(ParseError::Error(
                    "Mandatory argument FIELDS is missing or not at the right position",
                ));
            }
            let fields = parse_hash_fields_tail(&parts, fields_at)?;
            let expire_at_ms = hash_field_expire_at_ms(ttl.as_ref(), seconds, absolute, name)?;
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            // The executor receives [mode, field ...]; the time is absolute in
            // `expire_at_ms`, so C++ never has to know the unit or the basetime.
            let mut values = Vec::with_capacity(fields.len() + 1);
            values.push(Bytes::from_static(mode));
            values.extend(fields);
            cmd.values = values;
            cmd.expire_at_ms = expire_at_ms;
            Ok(cmd)
        }
        OpCode::HTtl
        | OpCode::HPTtl
        | OpCode::HExpireTime
        | OpCode::HPExpireTime
        | OpCode::HPersist => {
            // <cmd> key FIELDS numfields field [field ...]
            let name = match op {
                OpCode::HTtl => "httl",
                OpCode::HPTtl => "hpttl",
                OpCode::HExpireTime => "hexpiretime",
                OpCode::HPExpireTime => "hpexpiretime",
                _ => "hpersist",
            };
            if parts.len() < 5 {
                return Err(wrong_arity(name));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            if !ascii_eq_ci(part_to_bytes(&parts[2])?.as_ref(), b"FIELDS") {
                return Err(ParseError::Error(
                    "Mandatory argument FIELDS is missing or not at the right position",
                ));
            }
            let fields = parse_hash_fields_tail(&parts, 2)?;
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = fields;
            Ok(cmd)
        }
        OpCode::HScan => {
            if parts.len() < 3 {
                return Err(wrong_arity("hscan"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let cursor = part_to_bytes(&parts[2])?;
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.expire_at_ms = scan_offset_from_arg(cursor.as_ref())? as i64;
            cmd.scan_count = 10;
            cmd.scan_prefix = Bytes::from_static(b"*");
            let mut index = 3usize;
            while index < parts.len() {
                let arg = part_to_bytes(&parts[index])?;
                if ascii_eq_ci(arg.as_ref(), b"MATCH") {
                    if index + 1 >= parts.len() {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    cmd.scan_prefix = part_to_bytes(&parts[index + 1])?;
                    index += 2;
                } else if ascii_eq_ci(arg.as_ref(), b"COUNT") {
                    if index + 1 >= parts.len() {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    cmd.scan_count =
                        parse_positive_i64(part_to_bytes(&parts[index + 1])?.as_ref())?;
                    index += 2;
                } else if ascii_eq_ci(arg.as_ref(), b"NOVALUES") {
                    cmd.set_integer_reply = true;
                    index += 1;
                } else {
                    return Err(ParseError::Protocol("syntax error"));
                }
            }
            Ok(cmd)
        }
        OpCode::SAdd | OpCode::SRem => {
            if parts.len() < 3 {
                return Err(wrong_arity(if op == OpCode::SAdd {
                    "sadd"
                } else {
                    "srem"
                }));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut members = Vec::with_capacity(parts.len() - 2);
            for part in parts.iter().skip(2) {
                members.push(part_to_bytes(part)?);
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = members;
            Ok(cmd)
        }
        OpCode::SMembers | OpCode::SCard => {
            if parts.len() != 2 {
                return Err(wrong_arity(if op == OpCode::SMembers {
                    "smembers"
                } else {
                    "scard"
                }));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            Ok(Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            ))
        }
        OpCode::SScan => {
            if parts.len() < 3 {
                return Err(wrong_arity("sscan"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let cursor = part_to_bytes(&parts[2])?;
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.expire_at_ms = scan_offset_from_arg(cursor.as_ref())? as i64;
            cmd.scan_count = 10;
            cmd.scan_prefix = Bytes::from_static(b"*");
            let mut index = 3usize;
            while index < parts.len() {
                let arg = part_to_bytes(&parts[index])?;
                if ascii_eq_ci(arg.as_ref(), b"MATCH") {
                    if index + 1 >= parts.len() {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    cmd.scan_prefix = part_to_bytes(&parts[index + 1])?;
                    index += 2;
                } else if ascii_eq_ci(arg.as_ref(), b"COUNT") {
                    if index + 1 >= parts.len() {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    cmd.scan_count =
                        parse_positive_i64(part_to_bytes(&parts[index + 1])?.as_ref())?;
                    index += 2;
                } else {
                    return Err(ParseError::Protocol("syntax error"));
                }
            }
            Ok(cmd)
        }
        OpCode::SIsMember => {
            if parts.len() != 3 {
                return Err(wrong_arity("sismember"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let member = part_to_bytes(&parts[2])?;
            Ok(Command::new(
                op,
                vec![key],
                Some(member),
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            ))
        }
        OpCode::SMIsMember => {
            if parts.len() < 3 {
                return Err(wrong_arity("smismember"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut values = Vec::with_capacity(parts.len() - 2);
            for part in parts.iter().skip(2) {
                values.push(part_to_bytes(part)?);
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = values;
            Ok(cmd)
        }
        OpCode::SMove => {
            if parts.len() != 4 {
                return Err(wrong_arity("smove"));
            }
            let source = part_to_bytes(&parts[1])?;
            let destination = part_to_bytes(&parts[2])?;
            let source = validate_user_key(&source)?;
            let destination = validate_user_key(&destination)?;
            let member = part_to_bytes(&parts[3])?;
            let mut cmd = Command::new(
                op,
                vec![source, destination],
                Some(member),
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = vec![cmd.keys[1].clone(), cmd.val.clone().unwrap()];
            Ok(cmd)
        }
        OpCode::SPop | OpCode::SRandMember => {
            if parts.len() < 2 || parts.len() > 3 {
                return Err(wrong_arity(if op == OpCode::SPop {
                    "spop"
                } else {
                    "srandmember"
                }));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            if parts.len() == 3 {
                let count_arg = part_to_bytes(&parts[2])?;
                let count = parse_i64_arg(count_arg.as_ref())?;
                if op == OpCode::SPop && count < 0 {
                    return Err(ParseError::Protocol("value is out of range"));
                }
                if count == i64::MIN || count.saturating_abs() > SET_RANDOM_COUNT_LIMIT {
                    return Err(ParseError::Protocol("value is out of range"));
                }
                cmd.set_count = Some(count);
            }
            Ok(cmd)
        }
        OpCode::SInter | OpCode::SUnion | OpCode::SDiff => {
            if parts.len() < 2 {
                return Err(wrong_arity(match op {
                    OpCode::SInter => "sinter",
                    OpCode::SUnion => "sunion",
                    OpCode::SDiff => "sdiff",
                    _ => "setop",
                }));
            }
            let mut keys = Vec::with_capacity(parts.len() - 1);
            for part in parts.iter().skip(1) {
                let key = part_to_bytes(part)?;
                let key = validate_user_key(&key)?;
                keys.push(key);
            }
            Ok(Command::new(
                op,
                keys,
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            ))
        }
        OpCode::SInterCard => {
            if parts.len() < 3 {
                return Err(wrong_arity("sintercard"));
            }
            let num_keys_arg = part_to_bytes(&parts[1])?;
            let num_keys =
                parse_i64_error_arg(num_keys_arg.as_ref(), "numkeys should be greater than 0")?;
            if num_keys <= 0 {
                return Err(ParseError::Error("numkeys should be greater than 0"));
            }
            let num_keys = usize::try_from(num_keys)
                .map_err(|_| ParseError::Protocol("value is out of range"))?;
            if parts.len() < 2 + num_keys {
                return Err(ParseError::Error(
                    "Number of keys can't be greater than number of args",
                ));
            }
            let mut keys = Vec::with_capacity(num_keys);
            for part in parts.iter().skip(2).take(num_keys) {
                let key = part_to_bytes(part)?;
                let key = validate_user_key(&key)?;
                keys.push(key);
            }
            let mut cmd = Command::new(
                op,
                keys,
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.set_count = Some(0);
            let mut index = 2 + num_keys;
            while index < parts.len() {
                let arg = part_to_bytes(&parts[index])?;
                if !ascii_eq_ci(arg.as_ref(), b"LIMIT") || index + 1 >= parts.len() {
                    return Err(ParseError::Error("syntax error"));
                }
                let limit_arg = part_to_bytes(&parts[index + 1])?;
                let limit = parse_i64_error_arg(
                    limit_arg.as_ref(),
                    "LIMIT can't be negative or non-numeric",
                )?;
                if limit < 0 {
                    return Err(ParseError::Error("LIMIT can't be negative or non-numeric"));
                }
                cmd.set_count = Some(limit);
                index += 2;
            }
            Ok(cmd)
        }
        OpCode::SInterStore | OpCode::SUnionStore | OpCode::SDiffStore => {
            if parts.len() < 3 {
                return Err(wrong_arity(match op {
                    OpCode::SInterStore => "sinterstore",
                    OpCode::SUnionStore => "sunionstore",
                    OpCode::SDiffStore => "sdiffstore",
                    _ => "setopstore",
                }));
            }
            let destination = part_to_bytes(&parts[1])?;
            let destination = validate_user_key(&destination)?;
            let mut keys = Vec::with_capacity(parts.len() - 1);
            keys.push(destination);
            for part in parts.iter().skip(2) {
                let key = part_to_bytes(part)?;
                let key = validate_user_key(&key)?;
                keys.push(key);
            }
            Ok(Command::new(
                op,
                keys,
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            ))
        }
        OpCode::LPush | OpCode::RPush | OpCode::LPushX | OpCode::RPushX => {
            if parts.len() < 3 {
                return Err(wrong_arity(match op {
                    OpCode::LPush => "lpush",
                    OpCode::RPush => "rpush",
                    OpCode::LPushX => "lpushx",
                    OpCode::RPushX => "rpushx",
                    _ => "push",
                }));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut values = Vec::with_capacity(parts.len() - 2);
            for part in parts.iter().skip(2) {
                values.push(part_to_bytes(part)?);
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = values;
            Ok(cmd)
        }
        OpCode::LPop | OpCode::RPop => {
            if parts.len() < 2 || parts.len() > 3 {
                return Err(wrong_arity(if op == OpCode::LPop {
                    "lpop"
                } else {
                    "rpop"
                }));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            if parts.len() == 3 {
                let count_arg = part_to_bytes(&parts[2])?;
                let count = parse_i64_arg(count_arg.as_ref())?;
                if count < 0 {
                    return Err(ParseError::Protocol("value is out of range"));
                }
                cmd.set_count = Some(count);
            }
            Ok(cmd)
        }
        OpCode::LLen => {
            if parts.len() != 2 {
                return Err(wrong_arity("llen"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            Ok(Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            ))
        }
        OpCode::LIndex => {
            if parts.len() != 3 {
                return Err(wrong_arity("lindex"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let index = part_to_bytes(&parts[2])?;
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.expire_at_ms = parse_i64_arg(index.as_ref())?;
            Ok(cmd)
        }
        OpCode::LRange | OpCode::LTrim => {
            if parts.len() != 4 {
                return Err(wrong_arity(if op == OpCode::LRange {
                    "lrange"
                } else {
                    "ltrim"
                }));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let start = part_to_bytes(&parts[2])?;
            let stop = part_to_bytes(&parts[3])?;
            parse_i64_arg(start.as_ref())?;
            parse_i64_arg(stop.as_ref())?;
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = vec![start, stop];
            Ok(cmd)
        }
        OpCode::LSet | OpCode::LRem => {
            if parts.len() != 4 {
                return Err(wrong_arity(if op == OpCode::LSet {
                    "lset"
                } else {
                    "lrem"
                }));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let number = part_to_bytes(&parts[2])?;
            parse_i64_arg(number.as_ref())?;
            let value = part_to_bytes(&parts[3])?;
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = vec![number, value];
            Ok(cmd)
        }
        OpCode::LInsert => {
            if parts.len() != 5 {
                return Err(wrong_arity("linsert"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let position = part_to_bytes(&parts[2])?;
            let before = if ascii_eq_ci(position.as_ref(), b"BEFORE") {
                true
            } else if ascii_eq_ci(position.as_ref(), b"AFTER") {
                false
            } else {
                return Err(ParseError::Protocol("syntax error"));
            };
            let pivot = part_to_bytes(&parts[3])?;
            let value = part_to_bytes(&parts[4])?;
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = vec![pivot, value];
            if before {
                cmd.expire_flags |= TXN_FLAG_LIST_INSERT_BEFORE;
            }
            Ok(cmd)
        }
        OpCode::LMove | OpCode::BLMove => {
            let expected_len = if op == OpCode::BLMove { 6 } else { 5 };
            if parts.len() != expected_len {
                return Err(wrong_arity(if op == OpCode::BLMove {
                    "blmove"
                } else {
                    "lmove"
                }));
            }
            let source = part_to_bytes(&parts[1])?;
            let destination = part_to_bytes(&parts[2])?;
            let source = validate_user_key(&source)?;
            let destination = validate_user_key(&destination)?;
            let source_left = parse_list_side(part_to_bytes(&parts[3])?.as_ref())?;
            let dest_left = parse_list_side(part_to_bytes(&parts[4])?.as_ref())?;
            let timeout_ms = if op == OpCode::BLMove {
                parse_blocking_timeout_ms(part_to_bytes(&parts[5])?.as_ref())?
            } else {
                -1
            };
            let mut cmd = Command::new(
                op,
                vec![source],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = vec![destination];
            if source_left {
                cmd.expire_flags |= TXN_FLAG_LIST_SOURCE_LEFT;
            }
            if dest_left {
                cmd.expire_flags |= TXN_FLAG_LIST_DEST_LEFT;
            }
            cmd.expire_at_ms = timeout_ms;
            Ok(cmd)
        }
        OpCode::RPopLPush | OpCode::BRPopLPush => {
            let expected_len = if op == OpCode::BRPopLPush { 4 } else { 3 };
            if parts.len() != expected_len {
                return Err(wrong_arity(if op == OpCode::BRPopLPush {
                    "brpoplpush"
                } else {
                    "rpoplpush"
                }));
            }
            let source = part_to_bytes(&parts[1])?;
            let destination = part_to_bytes(&parts[2])?;
            let source = validate_user_key(&source)?;
            let destination = validate_user_key(&destination)?;
            let timeout_ms = if op == OpCode::BRPopLPush {
                parse_blocking_timeout_ms(part_to_bytes(&parts[3])?.as_ref())?
            } else {
                -1
            };
            let mut cmd = Command::new(
                op,
                vec![source],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = vec![destination];
            cmd.expire_flags |= TXN_FLAG_LIST_DEST_LEFT;
            cmd.expire_at_ms = timeout_ms;
            Ok(cmd)
        }
        OpCode::LPos => {
            if parts.len() < 3 {
                return Err(wrong_arity("lpos"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let element = part_to_bytes(&parts[2])?;
            let mut cmd = Command::new(
                op,
                vec![key],
                Some(element),
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.expire_at_ms = 1;
            cmd.scan_count = 0;
            let mut index = 3usize;
            while index < parts.len() {
                let option = part_to_bytes(&parts[index])?;
                if ascii_eq_ci(option.as_ref(), b"RANK") {
                    if index + 1 >= parts.len() {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    let rank = parse_i64_arg(part_to_bytes(&parts[index + 1])?.as_ref())?;
                    if rank == 0 {
                        return Err(ParseError::Error("RANK can't be zero: use 1 to start from the first match, 2 from the second ... or use negative to start from the end of the list"));
                    }
                    if rank == i64::MIN {
                        return Err(ParseError::Protocol("value is out of range"));
                    }
                    cmd.expire_at_ms = rank;
                    index += 2;
                } else if ascii_eq_ci(option.as_ref(), b"COUNT") {
                    if index + 1 >= parts.len() {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    let count = parse_i64_arg(part_to_bytes(&parts[index + 1])?.as_ref())?;
                    if count < 0 {
                        return Err(ParseError::Protocol("value is out of range"));
                    }
                    cmd.set_count = Some(count);
                    index += 2;
                } else if ascii_eq_ci(option.as_ref(), b"MAXLEN") {
                    if index + 1 >= parts.len() {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    let maxlen = parse_i64_arg(part_to_bytes(&parts[index + 1])?.as_ref())?;
                    if maxlen < 0 {
                        return Err(ParseError::Protocol("value is out of range"));
                    }
                    cmd.scan_count = maxlen;
                    index += 2;
                } else {
                    return Err(ParseError::Protocol("syntax error"));
                }
            }
            Ok(cmd)
        }
        OpCode::ZAdd => {
            if parts.len() < 4 {
                return Err(wrong_arity("zadd"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut flags = 0u32;
            let mut index = 2usize;
            while index < parts.len() {
                let arg = part_to_bytes(&parts[index])?;
                if ascii_eq_ci(arg.as_ref(), b"NX") {
                    flags |= TXN_FLAG_ZADD_NX;
                } else if ascii_eq_ci(arg.as_ref(), b"XX") {
                    flags |= TXN_FLAG_ZADD_XX;
                } else if ascii_eq_ci(arg.as_ref(), b"CH") {
                    flags |= TXN_FLAG_ZADD_CH;
                } else if ascii_eq_ci(arg.as_ref(), b"INCR") {
                    flags |= TXN_FLAG_ZADD_INCR;
                } else if ascii_eq_ci(arg.as_ref(), b"GT") {
                    flags |= TXN_FLAG_ZADD_GT;
                } else if ascii_eq_ci(arg.as_ref(), b"LT") {
                    flags |= TXN_FLAG_ZADD_LT;
                } else {
                    break;
                }
                index += 1;
            }
            if (flags & TXN_FLAG_ZADD_NX) != 0
                && (flags & (TXN_FLAG_ZADD_XX | TXN_FLAG_ZADD_GT | TXN_FLAG_ZADD_LT)) != 0
            {
                return Err(ParseError::Protocol("syntax error"));
            }
            if (flags & TXN_FLAG_ZADD_GT) != 0 && (flags & TXN_FLAG_ZADD_LT) != 0 {
                return Err(ParseError::Protocol("syntax error"));
            }
            if index >= parts.len() {
                return Err(wrong_arity("zadd"));
            }
            if (parts.len() - index) % 2 != 0 {
                return Err(ParseError::Protocol("syntax error"));
            }
            if (flags & TXN_FLAG_ZADD_INCR) != 0 && parts.len() - index != 2 {
                return Err(ParseError::Protocol("syntax error"));
            }
            let mut values = Vec::with_capacity(parts.len() - index);
            for pair in parts[index..].chunks_exact(2) {
                let score = part_to_bytes(&pair[0])?;
                parse_zadd_score_arg(score.as_ref())?;
                values.push(score);
                values.push(part_to_bytes(&pair[1])?);
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = values;
            cmd.expire_flags = flags;
            Ok(cmd)
        }
        OpCode::ZIncrBy => {
            if parts.len() != 4 {
                return Err(wrong_arity("zincrby"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let increment = part_to_bytes(&parts[2])?;
            parse_zadd_score_arg(increment.as_ref())?;
            let member = part_to_bytes(&parts[3])?;
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = vec![increment, member];
            Ok(cmd)
        }
        OpCode::ZScore | OpCode::ZRank | OpCode::ZRevRank => {
            if parts.len() != 3
                && !(matches!(op, OpCode::ZRank | OpCode::ZRevRank) && parts.len() == 4)
            {
                return Err(wrong_arity(match op {
                    OpCode::ZScore => "zscore",
                    OpCode::ZRank => "zrank",
                    OpCode::ZRevRank => "zrevrank",
                    _ => "zop",
                }));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let member = part_to_bytes(&parts[2])?;
            let mut cmd = Command::new(
                op,
                vec![key],
                Some(member),
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            if parts.len() == 4 {
                let option = part_to_bytes(&parts[3])?;
                if !ascii_eq_ci(option.as_ref(), b"WITHSCORE") {
                    return Err(ParseError::Protocol("syntax error"));
                }
                cmd.set_return_old = true;
            }
            Ok(cmd)
        }
        OpCode::ZMScore => {
            if parts.len() < 3 {
                return Err(wrong_arity("zmscore"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut members = Vec::with_capacity(parts.len() - 2);
            for part in parts.iter().skip(2) {
                members.push(part_to_bytes(part)?);
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = members;
            Ok(cmd)
        }
        OpCode::ZRem => {
            if parts.len() < 3 {
                return Err(wrong_arity("zrem"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut members = Vec::with_capacity(parts.len() - 2);
            for part in parts.iter().skip(2) {
                members.push(part_to_bytes(part)?);
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = members;
            Ok(cmd)
        }
        OpCode::ZCard => {
            if parts.len() != 2 {
                return Err(wrong_arity("zcard"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            Ok(Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            ))
        }
        OpCode::ZRange
        | OpCode::ZRevRange
        | OpCode::ZRangeByScore
        | OpCode::ZRevRangeByScore
        | OpCode::ZRangeByLex
        | OpCode::ZRevRangeByLex => {
            if parts.len() < 4 {
                return Err(wrong_arity(match op {
                    OpCode::ZRange => "zrange",
                    OpCode::ZRevRange => "zrevrange",
                    OpCode::ZRangeByScore => "zrangebyscore",
                    OpCode::ZRevRangeByScore => "zrevrangebyscore",
                    OpCode::ZRangeByLex => "zrangebylex",
                    OpCode::ZRevRangeByLex => "zrevrangebylex",
                    _ => "zrange",
                }));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let first = part_to_bytes(&parts[2])?;
            let second = part_to_bytes(&parts[3])?;
            let mut flags = 0u32;
            let mut mode = ZRANGE_MODE_RANK;
            if matches!(op, OpCode::ZRangeByScore | OpCode::ZRevRangeByScore) {
                parse_zrange_bound_arg(first.as_ref())?;
                parse_zrange_bound_arg(second.as_ref())?;
                flags |= TXN_FLAG_Z_BYSCORE;
                mode = ZRANGE_MODE_SCORE;
                if op == OpCode::ZRevRangeByScore {
                    flags |= TXN_FLAG_Z_REV;
                }
            } else if matches!(op, OpCode::ZRangeByLex | OpCode::ZRevRangeByLex) {
                parse_zlex_bound_arg(first.as_ref())?;
                parse_zlex_bound_arg(second.as_ref())?;
                mode = ZRANGE_MODE_LEX;
                if op == OpCode::ZRevRangeByLex {
                    flags |= TXN_FLAG_Z_REV;
                }
            }
            let mut values = if matches!(op, OpCode::ZRevRangeByScore | OpCode::ZRevRangeByLex) {
                vec![second, first]
            } else {
                vec![first, second]
            };
            let mut index = 4usize;
            while index < parts.len() {
                let arg = part_to_bytes(&parts[index])?;
                if ascii_eq_ci(arg.as_ref(), b"WITHSCORES") {
                    if mode == ZRANGE_MODE_LEX {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    flags |= TXN_FLAG_Z_WITHSCORES;
                    index += 1;
                } else if ascii_eq_ci(arg.as_ref(), b"REV") && op == OpCode::ZRange {
                    flags |= TXN_FLAG_Z_REV;
                    index += 1;
                } else if ascii_eq_ci(arg.as_ref(), b"BYSCORE") && op == OpCode::ZRange {
                    flags |= TXN_FLAG_Z_BYSCORE;
                    mode = ZRANGE_MODE_SCORE;
                    parse_zrange_bound_arg(values[0].as_ref())?;
                    parse_zrange_bound_arg(values[1].as_ref())?;
                    index += 1;
                } else if ascii_eq_ci(arg.as_ref(), b"BYLEX") && op == OpCode::ZRange {
                    mode = ZRANGE_MODE_LEX;
                    parse_zlex_bound_arg(values[0].as_ref())
                        .map_err(|_| ParseError::Protocol("syntax error"))?;
                    parse_zlex_bound_arg(values[1].as_ref())
                        .map_err(|_| ParseError::Protocol("syntax error"))?;
                    index += 1;
                } else if ascii_eq_ci(arg.as_ref(), b"LIMIT")
                    && (mode == ZRANGE_MODE_SCORE || mode == ZRANGE_MODE_LEX)
                {
                    if index + 2 >= parts.len() {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    let offset = part_to_bytes(&parts[index + 1])?;
                    let count = part_to_bytes(&parts[index + 2])?;
                    parse_i64_arg(offset.as_ref())?;
                    parse_i64_arg(count.as_ref())?;
                    values.push(offset);
                    values.push(count);
                    index += 3;
                } else {
                    return Err(ParseError::Protocol("syntax error"));
                }
            }
            if mode == ZRANGE_MODE_RANK {
                parse_i64_arg(values[0].as_ref())?;
                parse_i64_arg(values[1].as_ref())?;
            } else if op == OpCode::ZRange && (flags & TXN_FLAG_Z_REV) != 0 {
                values.swap(0, 1);
            }
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = values;
            cmd.expire_flags = flags;
            cmd.expire_at_ms = mode;
            Ok(cmd)
        }
        OpCode::ZLexCount => {
            if parts.len() != 4 {
                return Err(wrong_arity("zlexcount"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let min = part_to_bytes(&parts[2])?;
            let max = part_to_bytes(&parts[3])?;
            parse_zlex_bound_arg(min.as_ref())?;
            parse_zlex_bound_arg(max.as_ref())?;
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = vec![min, max];
            cmd.expire_at_ms = ZRANGE_MODE_LEX;
            Ok(cmd)
        }
        OpCode::ZRemRangeByScore | OpCode::ZRemRangeByRank | OpCode::ZRemRangeByLex => {
            if parts.len() != 4 {
                return Err(wrong_arity(match op {
                    OpCode::ZRemRangeByScore => "zremrangebyscore",
                    OpCode::ZRemRangeByRank => "zremrangebyrank",
                    OpCode::ZRemRangeByLex => "zremrangebylex",
                    _ => "zremrange",
                }));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let min = part_to_bytes(&parts[2])?;
            let max = part_to_bytes(&parts[3])?;
            let mode = match op {
                OpCode::ZRemRangeByScore => {
                    parse_zrange_bound_arg(min.as_ref())?;
                    parse_zrange_bound_arg(max.as_ref())?;
                    ZRANGE_MODE_SCORE
                }
                OpCode::ZRemRangeByRank => {
                    parse_i64_arg(min.as_ref())?;
                    parse_i64_arg(max.as_ref())?;
                    ZRANGE_MODE_RANK
                }
                OpCode::ZRemRangeByLex => {
                    parse_zlex_bound_arg(min.as_ref())?;
                    parse_zlex_bound_arg(max.as_ref())?;
                    ZRANGE_MODE_LEX
                }
                _ => ZRANGE_MODE_RANK,
            };
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = vec![min, max];
            cmd.expire_at_ms = mode;
            Ok(cmd)
        }
        OpCode::ZRangeStore => {
            if parts.len() < 5 {
                return Err(wrong_arity("zrangestore"));
            }
            let destination = part_to_bytes(&parts[1])?;
            let source = part_to_bytes(&parts[2])?;
            let destination = validate_user_key(&destination)?;
            let source = validate_user_key(&source)?;
            let first = part_to_bytes(&parts[3])?;
            let second = part_to_bytes(&parts[4])?;
            let mut flags = 0u32;
            let mut mode = ZRANGE_MODE_RANK;
            let mut values = vec![source, first, second];
            let mut index = 5usize;
            while index < parts.len() {
                let arg = part_to_bytes(&parts[index])?;
                if ascii_eq_ci(arg.as_ref(), b"REV") {
                    flags |= TXN_FLAG_Z_REV;
                    index += 1;
                } else if ascii_eq_ci(arg.as_ref(), b"BYSCORE") {
                    mode = ZRANGE_MODE_SCORE;
                    parse_zrange_bound_arg(values[1].as_ref())?;
                    parse_zrange_bound_arg(values[2].as_ref())?;
                    index += 1;
                } else if ascii_eq_ci(arg.as_ref(), b"BYLEX") {
                    mode = ZRANGE_MODE_LEX;
                    parse_zlex_bound_arg(values[1].as_ref())?;
                    parse_zlex_bound_arg(values[2].as_ref())?;
                    index += 1;
                } else if ascii_eq_ci(arg.as_ref(), b"LIMIT")
                    && (mode == ZRANGE_MODE_SCORE || mode == ZRANGE_MODE_LEX)
                {
                    if index + 2 >= parts.len() {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    let offset = part_to_bytes(&parts[index + 1])?;
                    let count = part_to_bytes(&parts[index + 2])?;
                    parse_i64_arg(offset.as_ref())?;
                    parse_i64_arg(count.as_ref())?;
                    values.push(offset);
                    values.push(count);
                    index += 3;
                } else {
                    return Err(ParseError::Protocol("syntax error"));
                }
            }
            if mode == ZRANGE_MODE_RANK {
                parse_i64_arg(values[1].as_ref())?;
                parse_i64_arg(values[2].as_ref())?;
            } else if (flags & TXN_FLAG_Z_REV) != 0 {
                values.swap(1, 2);
            }
            let mut cmd = Command::new(
                op,
                vec![destination],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = values;
            cmd.expire_flags = flags;
            cmd.expire_at_ms = mode;
            Ok(cmd)
        }
        OpCode::ZUnion
        | OpCode::ZInter
        | OpCode::ZDiff
        | OpCode::ZUnionStore
        | OpCode::ZInterStore
        | OpCode::ZDiffStore
        | OpCode::ZInterCard => {
            let store = matches!(
                op,
                OpCode::ZUnionStore | OpCode::ZInterStore | OpCode::ZDiffStore
            );
            let first_arg = if store { 2 } else { 1 };
            let syntax_error = || {
                if op == OpCode::ZInterCard {
                    ParseError::Error("syntax error")
                } else {
                    ParseError::Protocol("syntax error")
                }
            };
            if parts.len() <= first_arg {
                return Err(wrong_arity(match op {
                    OpCode::ZUnionStore => "zunionstore",
                    OpCode::ZInterStore => "zinterstore",
                    OpCode::ZDiffStore => "zdiffstore",
                    OpCode::ZUnion => "zunion",
                    OpCode::ZInter => "zinter",
                    OpCode::ZDiff => "zdiff",
                    OpCode::ZInterCard => "zintercard",
                    _ => "zop",
                }));
            }
            let destination = if store {
                let dst = part_to_bytes(&parts[1])?;
                let dst = validate_user_key(&dst)?;
                dst
            } else {
                Bytes::new()
            };
            let numkeys = parse_i64_arg(part_to_bytes(&parts[first_arg])?.as_ref())?;
            if numkeys <= 0 {
                if matches!(
                    op,
                    OpCode::ZUnion
                        | OpCode::ZInter
                        | OpCode::ZDiff
                        | OpCode::ZUnionStore
                        | OpCode::ZInterStore
                        | OpCode::ZDiffStore
                        | OpCode::ZInterCard
                ) {
                    let command = match op {
                        OpCode::ZUnion => "zunion",
                        OpCode::ZInter => "zinter",
                        OpCode::ZDiff => "zdiff",
                        OpCode::ZUnionStore => "zunionstore",
                        OpCode::ZInterStore => "zinterstore",
                        OpCode::ZDiffStore => "zdiffstore",
                        OpCode::ZInterCard => "zintercard",
                        _ => "zop",
                    };
                    return Err(ParseError::Error(match command {
                        "zunion" => "at least 1 input key is needed for 'zunion' command",
                        "zinter" => "at least 1 input key is needed for 'zinter' command",
                        "zdiff" => "at least 1 input key is needed for 'zdiff' command",
                        "zunionstore" => "at least 1 input key is needed for 'zunionstore' command",
                        "zinterstore" => "at least 1 input key is needed for 'zinterstore' command",
                        "zdiffstore" => "at least 1 input key is needed for 'zdiffstore' command",
                        "zintercard" => "at least 1 input key is needed for 'zintercard' command",
                        _ => "syntax error",
                    }));
                }
                return Err(syntax_error());
            }
            let numkeys = numkeys as usize;
            let first_key = first_arg + 1;
            if parts.len() < first_key + numkeys {
                return Err(syntax_error());
            }
            let mut sources = Vec::with_capacity(numkeys);
            for part in parts.iter().skip(first_key).take(numkeys) {
                let key = part_to_bytes(part)?;
                let key = validate_user_key(&key)?;
                sources.push(key);
            }
            let mut weights: Vec<Bytes> = (0..numkeys).map(|_| Bytes::from_static(b"1")).collect();
            let mut aggregate = ZAGG_SUM;
            let mut with_scores = false;
            let mut limit = -1i64;
            let mut index = first_key + numkeys;
            while index < parts.len() {
                let arg = part_to_bytes(&parts[index])?;
                if ascii_eq_ci(arg.as_ref(), b"WEIGHTS")
                    && !matches!(op, OpCode::ZDiff | OpCode::ZDiffStore | OpCode::ZInterCard)
                {
                    if index + numkeys >= parts.len() {
                        return Err(syntax_error());
                    }
                    weights.clear();
                    for weight_part in parts.iter().skip(index + 1).take(numkeys) {
                        let weight = part_to_bytes(weight_part)?;
                        parse_f64_error_arg(weight.as_ref(), "weight value is not a float")?;
                        weights.push(weight);
                    }
                    index += 1 + numkeys;
                } else if ascii_eq_ci(arg.as_ref(), b"AGGREGATE")
                    && !matches!(op, OpCode::ZDiff | OpCode::ZDiffStore | OpCode::ZInterCard)
                {
                    if index + 1 >= parts.len() {
                        return Err(syntax_error());
                    }
                    let mode_arg = part_to_bytes(&parts[index + 1])?;
                    aggregate = if ascii_eq_ci(mode_arg.as_ref(), b"SUM") {
                        ZAGG_SUM
                    } else if ascii_eq_ci(mode_arg.as_ref(), b"MIN") {
                        ZAGG_MIN
                    } else if ascii_eq_ci(mode_arg.as_ref(), b"MAX") {
                        ZAGG_MAX
                    } else {
                        return Err(syntax_error());
                    };
                    index += 2;
                } else if ascii_eq_ci(arg.as_ref(), b"WITHSCORES") {
                    if store || op == OpCode::ZInterCard {
                        return Err(syntax_error());
                    }
                    with_scores = true;
                    index += 1;
                } else if ascii_eq_ci(arg.as_ref(), b"LIMIT") && op == OpCode::ZInterCard {
                    if index + 1 >= parts.len() {
                        return Err(syntax_error());
                    }
                    limit = parse_i64_error_arg(
                        part_to_bytes(&parts[index + 1])?.as_ref(),
                        "LIMIT can't be negative",
                    )?;
                    if limit < 0 {
                        return Err(ParseError::Error("LIMIT can't be negative"));
                    }
                    index += 2;
                } else {
                    return Err(syntax_error());
                }
            }
            let mut payload = Vec::with_capacity(1 + sources.len() + weights.len());
            payload.push(Bytes::from(numkeys.to_string()));
            payload.extend(sources);
            payload.extend(weights);
            let mut cmd = Command::new(
                op,
                if store {
                    vec![destination]
                } else {
                    vec![Bytes::new()]
                },
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = payload;
            cmd.expire_at_ms = if op == OpCode::ZInterCard {
                limit
            } else {
                aggregate
            };
            cmd.set_count = if op == OpCode::ZInterCard {
                Some(limit)
            } else {
                None
            };
            if with_scores {
                cmd.expire_flags |= TXN_FLAG_Z_WITHSCORES;
            }
            if matches!(op, OpCode::ZUnion | OpCode::ZUnionStore) {
                cmd.expire_flags |= TXN_FLAG_SET_ALGEBRA_UNION;
            } else if matches!(op, OpCode::ZDiff | OpCode::ZDiffStore) {
                cmd.expire_flags |= TXN_FLAG_SET_ALGEBRA_DIFF;
            }
            if store {
                cmd.expire_flags |= TXN_FLAG_SET_ALGEBRA_STORE;
            }
            if op == OpCode::ZInterCard {
                cmd.expire_flags |= TXN_FLAG_SCAN_COUNT_ONLY;
            }
            Ok(cmd)
        }
        OpCode::ZCount => {
            if parts.len() != 4 {
                return Err(wrong_arity("zcount"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let min = part_to_bytes(&parts[2])?;
            let max = part_to_bytes(&parts[3])?;
            parse_zrange_bound_arg(min.as_ref())?;
            parse_zrange_bound_arg(max.as_ref())?;
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = vec![min, max];
            Ok(cmd)
        }
        OpCode::ZPopMin | OpCode::ZPopMax => {
            if parts.len() < 2 || parts.len() > 3 {
                return Err(wrong_arity(if op == OpCode::ZPopMin {
                    "zpopmin"
                } else {
                    "zpopmax"
                }));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            if parts.len() == 3 {
                let count = parse_i64_arg(part_to_bytes(&parts[2])?.as_ref())?;
                if count < 0 {
                    return Err(ParseError::Error("count must be positive"));
                }
                cmd.set_count = Some(count);
            }
            Ok(cmd)
        }
        OpCode::ZMPop | OpCode::BZMPop => {
            let first_arg = if op == OpCode::BZMPop { 2 } else { 1 };
            if parts.len() <= first_arg + 1 {
                return Err(wrong_arity(if op == OpCode::BZMPop {
                    "bzmpop"
                } else {
                    "zmpop"
                }));
            }
            let timeout_ms = if op == OpCode::BZMPop {
                parse_blocking_timeout_ms(part_to_bytes(&parts[1])?.as_ref())?
            } else {
                0
            };
            let numkeys = parse_i64_error_arg(
                part_to_bytes(&parts[first_arg])?.as_ref(),
                "numkeys should be greater than 0",
            )?;
            if numkeys <= 0 {
                return Err(ParseError::Error("numkeys should be greater than 0"));
            }
            let numkeys = numkeys as usize;
            let first_key = first_arg + 1;
            if parts.len() < first_key + numkeys + 1 {
                return Err(wrong_arity(if op == OpCode::BZMPop {
                    "bzmpop"
                } else {
                    "zmpop"
                }));
            }
            let mut keys = Vec::with_capacity(numkeys);
            for part in parts.iter().skip(first_key).take(numkeys) {
                let key = part_to_bytes(part)?;
                let key = validate_user_key(&key)?;
                keys.push(key);
            }
            let direction = part_to_bytes(&parts[first_key + numkeys])?;
            let mut flags = if ascii_eq_ci(direction.as_ref(), b"MIN") {
                0
            } else if ascii_eq_ci(direction.as_ref(), b"MAX") {
                TXN_FLAG_Z_REV
            } else {
                return Err(ParseError::Error("syntax error"));
            };
            let mut count = 1i64;
            let mut saw_count = false;
            let mut index = first_key + numkeys + 1;
            while index < parts.len() {
                let arg = part_to_bytes(&parts[index])?;
                if ascii_eq_ci(arg.as_ref(), b"COUNT") {
                    if saw_count || index + 1 >= parts.len() {
                        return Err(ParseError::Error("syntax error"));
                    }
                    saw_count = true;
                    count = parse_i64_error_arg(
                        part_to_bytes(&parts[index + 1])?.as_ref(),
                        "count should be greater than 0",
                    )?;
                    if count <= 0 {
                        return Err(ParseError::Error("count should be greater than 0"));
                    }
                    flags |= TXN_FLAG_Z_COUNT_GIVEN;
                    index += 2;
                } else {
                    return Err(ParseError::Error("syntax error"));
                }
            }
            let mut cmd = Command::new(
                op,
                keys,
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.expire_flags = flags;
            cmd.expire_at_ms = timeout_ms;
            cmd.set_count = Some(count);
            Ok(cmd)
        }
        OpCode::ZRandMember => {
            if parts.len() < 2 || parts.len() > 4 {
                return Err(wrong_arity("zrandmember"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            if parts.len() >= 3 {
                let count = parse_i64_error_arg(
                    part_to_bytes(&parts[2])?.as_ref(),
                    "value is out of range",
                )?;
                if count == i64::MIN {
                    return Err(ParseError::Error("value is out of range"));
                }
                if count.saturating_abs() > SET_RANDOM_COUNT_LIMIT {
                    return Err(ParseError::Error("value is out of range"));
                }
                cmd.set_count = Some(count);
                cmd.expire_flags |= TXN_FLAG_Z_COUNT_GIVEN;
            }
            if parts.len() == 4 {
                let option = part_to_bytes(&parts[3])?;
                if !ascii_eq_ci(option.as_ref(), b"WITHSCORES") || cmd.set_count.is_none() {
                    return Err(ParseError::Protocol("syntax error"));
                }
                cmd.expire_flags |= TXN_FLAG_Z_WITHSCORES;
            }
            Ok(cmd)
        }
        OpCode::BZPopMin | OpCode::BZPopMax => {
            if parts.len() < 3 {
                return Err(wrong_arity(if op == OpCode::BZPopMin {
                    "bzpopmin"
                } else {
                    "bzpopmax"
                }));
            }
            let timeout_ms =
                parse_blocking_timeout_ms(part_to_bytes(parts.last().unwrap())?.as_ref())?;
            let mut keys = Vec::with_capacity(parts.len() - 2);
            for part in parts.iter().skip(1).take(parts.len() - 2) {
                let key = part_to_bytes(part)?;
                let key = validate_user_key(&key)?;
                keys.push(key);
            }
            let mut cmd = Command::new(
                op,
                keys,
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            if op == OpCode::BZPopMax {
                cmd.expire_flags |= TXN_FLAG_Z_REV;
            }
            cmd.expire_at_ms = timeout_ms;
            cmd.set_count = Some(1);
            Ok(cmd)
        }
        OpCode::Eval | OpCode::EvalSha | OpCode::EvalRo | OpCode::EvalShaRo => {
            // EVAL script numkeys key... arg...
            // cmd.keys carries the database-prefixed names, which is what the
            // interactive session locks and what WATCH invalidation bumps.
            // cmd.args keeps every argument exactly as the client sent it,
            // because KEYS and ARGV have to show the script those bytes: a
            // redis.call on KEYS[1] is parsed, and prefixed, all over again.
            if parts.len() < 3 {
                return Err(wrong_arity(match op {
                    OpCode::Eval => "eval",
                    OpCode::EvalSha => "evalsha",
                    OpCode::EvalRo => "eval_ro",
                    _ => "evalsha_ro",
                }));
            }
            let numkeys_raw = part_to_bytes(&parts[2])?;
            let Some(numkeys) = parse_i64_lossy(numkeys_raw.as_ref()) else {
                return Err(ParseError::Error("value is not an integer or out of range"));
            };
            if numkeys < 0 {
                return Err(ParseError::Error("Number of keys can't be negative"));
            }
            let numkeys = numkeys as usize;
            if numkeys > parts.len() - 3 {
                return Err(ParseError::Error(
                    "Number of keys can't be greater than number of args",
                ));
            }
            let mut keys = Vec::with_capacity(numkeys);
            for part in parts.iter().skip(3).take(numkeys) {
                let key = part_to_bytes(part)?;
                let key = validate_user_key(&key)?;
                keys.push(key);
            }
            Ok(Command::new(
                op,
                keys,
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            ))
        }
        OpCode::ZScan => {
            if parts.len() < 3 {
                return Err(wrong_arity("zscan"));
            }
            let key = part_to_bytes(&parts[1])?;
            let key = validate_user_key(&key)?;
            let cursor = part_to_bytes(&parts[2])?;
            let mut cmd = Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.expire_at_ms = scan_offset_from_arg(cursor.as_ref())? as i64;
            cmd.scan_count = 10;
            cmd.scan_prefix = Bytes::from_static(b"*");
            let mut index = 3usize;
            while index < parts.len() {
                let arg = part_to_bytes(&parts[index])?;
                if ascii_eq_ci(arg.as_ref(), b"MATCH") {
                    if index + 1 >= parts.len() {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    cmd.scan_prefix = part_to_bytes(&parts[index + 1])?;
                    index += 2;
                } else if ascii_eq_ci(arg.as_ref(), b"COUNT") {
                    if index + 1 >= parts.len() {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    cmd.scan_count =
                        parse_positive_i64(part_to_bytes(&parts[index + 1])?.as_ref())?;
                    index += 2;
                } else {
                    return Err(ParseError::Protocol("syntax error"));
                }
            }
            Ok(cmd)
        }
        OpCode::Subscribe | OpCode::PSubscribe | OpCode::SSubscribe => {
            if parts.len() < 2 {
                return Err(wrong_arity(match op {
                    OpCode::Subscribe => "subscribe",
                    OpCode::SSubscribe => "ssubscribe",
                    _ => "psubscribe",
                }));
            }
            Ok(Command::new(
                op,
                Vec::new(),
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            ))
        }
        OpCode::Unsubscribe | OpCode::PUnsubscribe | OpCode::SUnsubscribe => Ok(Command::new(
            op,
            Vec::new(),
            None,
            command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
        )),
        OpCode::Publish => {
            if parts.len() != 3 {
                return Err(wrong_arity("publish"));
            }
            let channel = part_to_bytes(&parts[1])?;
            let message = part_to_bytes(&parts[2])?;
            Ok(Command::new(
                op,
                vec![channel],
                Some(message),
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            ))
        }
        OpCode::PubSub => {
            if parts.len() < 2 {
                return Err(wrong_arity("pubsub"));
            }
            Ok(Command::new(
                op,
                Vec::new(),
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            ))
        }
        OpCode::Ping
        | OpCode::Multi
        | OpCode::Exec
        | OpCode::Discard
        | OpCode::Hello
        | OpCode::Client
        | OpCode::Command
        | OpCode::Memory
        | OpCode::SlowLog
        | OpCode::Latency
        | OpCode::Module
        | OpCode::Cluster
        | OpCode::ReadOnly
        | OpCode::ReadWrite
        | OpCode::Monitor
        | OpCode::Acl
        | OpCode::Config
        | OpCode::Script
        | OpCode::Forbidden
        | OpCode::RandomKey
        | OpCode::Reset
        | OpCode::Quit
        | OpCode::Select
        | OpCode::Auth
        | OpCode::Echo
        | OpCode::Info
        | OpCode::Wait
        | OpCode::Time
        | OpCode::Watch
        | OpCode::Unwatch
        | OpCode::FlushDb
        | OpCode::FlushAll => {
            let command = match op {
                OpCode::Ping if parts.len() > 2 => Some("ping"),
                OpCode::Multi if parts.len() != 1 => Some("multi"),
                OpCode::Exec if parts.len() != 1 => Some("exec"),
                OpCode::Discard if parts.len() != 1 => Some("discard"),
                OpCode::Reset if parts.len() != 1 => Some("reset"),
                OpCode::Quit if parts.len() != 1 => Some("quit"),
                OpCode::Select if parts.len() != 2 => Some("select"),
                OpCode::Script if parts.len() < 2 => Some("script"),
                OpCode::RandomKey if parts.len() != 1 => Some("randomkey"),
                OpCode::Auth if parts.len() != 2 && parts.len() != 3 => Some("auth"),
                OpCode::Echo if parts.len() != 2 => Some("echo"),
                OpCode::Info if parts.len() > 2 => Some("info"),
                OpCode::Memory if parts.len() < 2 => Some("memory"),
                OpCode::SlowLog if parts.len() < 2 => Some("slowlog"),
                OpCode::Module if parts.len() < 2 => Some("module"),
                OpCode::Latency if parts.len() < 2 => Some("latency"),
                OpCode::Cluster if parts.len() < 2 => Some("cluster"),
                OpCode::ReadOnly if parts.len() != 1 => Some("readonly"),
                OpCode::ReadWrite if parts.len() != 1 => Some("readwrite"),
                OpCode::Monitor if parts.len() != 1 => Some("monitor"),
                OpCode::Acl if parts.len() < 2 => Some("acl"),
                OpCode::Wait if parts.len() != 3 => Some("wait"),
                OpCode::Time if parts.len() != 1 => Some("time"),
                OpCode::Watch if parts.len() < 2 => Some("watch"),
                OpCode::Unwatch if parts.len() != 1 => Some("unwatch"),
                OpCode::FlushDb | OpCode::FlushAll if parts.len() > 2 => Some("flushdb"),
                OpCode::FlushDb | OpCode::FlushAll
                    if parts.len() == 2
                        && !ascii_eq_ci(
                            frame_to_bytes(&parts[1]).as_deref().unwrap_or(b""),
                            b"SYNC",
                        )
                        && !ascii_eq_ci(
                            frame_to_bytes(&parts[1]).as_deref().unwrap_or(b""),
                            b"ASYNC",
                        ) =>
                {
                    Some("flushdb")
                }
                _ => None,
            };
            if let Some(command) = command {
                return Err(wrong_arity(command));
            }
            let mut cmd = Command::new(
                op,
                Vec::new(),
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            // RANDOMKEY walks one database and FLUSHDB clears one; both carry
            // it in the scan prefix (DBSIZE does the same in its own arm).
            // FLUSHALL sends nothing, which is what tells the executor to
            // clear every database.
            if op == OpCode::RandomKey {
                cmd.scan_prefix = db_scan_prefix(current_db());
            } else if op == OpCode::FlushDb {
                cmd.scan_prefix = db_flush_prefix(current_db());
            }
            if op == OpCode::Memory
                && cmd.args.len() >= 2
                && ascii_eq_ci(cmd.args[0].as_ref(), b"USAGE")
            {
                let key = cmd.args[1].clone();
                let key = validate_user_key(&key)?;
                cmd.keys = vec![key];
            }
            Ok(cmd)
        }
    }
}

// ===== RESP Writers =====

#[inline]
fn write_simple_ok<W: Write>(w: &mut W) -> std::io::Result<()> {
    w.write_all(b"+OK\r\n")
}

#[inline]
fn write_simple_string<W: Write>(w: &mut W, data: &str) -> std::io::Result<()> {
    w.write_all(b"+")?;
    w.write_all(data.as_bytes())?;
    w.write_all(b"\r\n")
}

#[inline]
fn write_integer<W: Write>(w: &mut W, value: i64) -> std::io::Result<()> {
    let mut buf = itoa::Buffer::new();
    w.write_all(b":")?;
    w.write_all(buf.format(value).as_bytes())?;
    w.write_all(b"\r\n")
}

#[inline]
fn write_double_text<W: Write>(w: &mut W, value: &[u8]) -> std::io::Result<()> {
    w.write_all(b",")?;
    w.write_all(value)?;
    w.write_all(b"\r\n")
}

#[inline]
fn write_score<W: Write>(w: &mut W, value: &[u8], protocol_version: u8) -> std::io::Result<()> {
    if protocol_version >= 3 {
        write_double_text(w, value)
    } else {
        write_bulk(w, value)
    }
}

#[inline]
fn write_pong<W: Write>(w: &mut W) -> std::io::Result<()> {
    w.write_all(b"+PONG\r\n")
}

#[inline]
fn write_queued<W: Write>(w: &mut W) -> std::io::Result<()> {
    w.write_all(b"+QUEUED\r\n")
}

#[inline]
fn write_nil_bulk<W: Write>(w: &mut W) -> std::io::Result<()> {
    w.write_all(b"$-1\r\n")
}

#[inline]
fn write_null<W: Write>(w: &mut W, protocol_version: u8) -> std::io::Result<()> {
    if protocol_version >= 3 {
        w.write_all(b"_\r\n")
    } else {
        write_nil_bulk(w)
    }
}

#[inline]
fn write_bulk<W: Write>(w: &mut W, data: &[u8]) -> std::io::Result<()> {
    let mut buf = itoa::Buffer::new();
    w.write_all(b"$")?;
    w.write_all(buf.format(data.len()).as_bytes())?;
    w.write_all(b"\r\n")?;
    w.write_all(data)?;
    w.write_all(b"\r\n")
}

#[inline]
fn write_err<W: Write>(w: &mut W, msg: &str) -> std::io::Result<()> {
    w.write_all(b"-ERR ")?;
    w.write_all(msg.as_bytes())?;
    w.write_all(b"\r\n")
}

#[inline]
fn write_wrongtype<W: Write>(w: &mut W) -> std::io::Result<()> {
    w.write_all(b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")
}

/// Redis reports a string that is not a HyperLogLog sketch with its own
/// WRONGTYPE-prefixed text, so this cannot go through write_err (which prefixes
/// "ERR ").
#[inline]
fn write_invalid_hll<W: Write>(w: &mut W) -> std::io::Result<()> {
    w.write_all(b"-WRONGTYPE Key is not a valid HyperLogLog string value.\r\n")
}

fn write_parse_error<W: Write>(w: &mut W, err: ParseError) -> std::io::Result<()> {
    match err {
        ParseError::Protocol(msg) => {
            w.write_all(b"-ERR protocol error: ")?;
            w.write_all(msg.as_bytes())?;
            w.write_all(b"\r\n")
        }
        ParseError::Error(msg) => {
            w.write_all(b"-ERR ")?;
            w.write_all(msg.as_bytes())?;
            w.write_all(b"\r\n")
        }
        ParseError::Owned(msg) => {
            w.write_all(b"-ERR ")?;
            w.write_all(msg.as_bytes())?;
            w.write_all(b"\r\n")
        }
        ParseError::WrongArity { command } => {
            w.write_all(b"-ERR wrong number of arguments for '")?;
            w.write_all(command.as_bytes())?;
            w.write_all(b"' command\r\n")
        }
        ParseError::UnknownCommand { name, args } => {
            w.write_all(b"-ERR unknown command '")?;
            w.write_all(String::from_utf8_lossy(&name).as_bytes())?;
            w.write_all(b"'")?;
            if let Some(first) = args.first() {
                w.write_all(b", with args beginning with: '")?;
                w.write_all(String::from_utf8_lossy(first).as_bytes())?;
                w.write_all(b"'")?;
            }
            w.write_all(b"\r\n")
        }
    }
}

#[inline]
fn write_array_header<W: Write>(w: &mut W, len: usize) -> std::io::Result<()> {
    let mut buf = itoa::Buffer::new();
    w.write_all(b"*")?;
    w.write_all(buf.format(len).as_bytes())?;
    w.write_all(b"\r\n")
}

#[inline]
fn write_map_header<W: Write>(w: &mut W, len: usize) -> std::io::Result<()> {
    let mut buf = itoa::Buffer::new();
    w.write_all(b"%")?;
    w.write_all(buf.format(len).as_bytes())?;
    w.write_all(b"\r\n")
}

fn parse_protocol_version(arg: &[u8]) -> Option<u8> {
    if arg == b"2" {
        Some(2)
    } else if arg == b"3" {
        Some(3)
    } else {
        None
    }
}

fn read_u64_le(input: &[u8], pos: &mut usize) -> Option<u64> {
    if input.len().saturating_sub(*pos) < 8 {
        return None;
    }
    let mut value = 0u64;
    for shift in 0..8 {
        value |= (input[*pos + shift] as u64) << (shift * 8);
    }
    *pos += 8;
    Some(value)
}

fn append_u64_le(out: &mut Vec<u8>, value: u64) {
    for shift in (0..64).step_by(8) {
        out.push(((value >> shift) & 0xff) as u8);
    }
}

fn pack_bytes_list(items: &[Bytes]) -> Bytes {
    let mut out = Vec::new();
    append_u64_le(&mut out, items.len() as u64);
    for item in items {
        append_u64_le(&mut out, item.len() as u64);
        out.extend_from_slice(item);
    }
    Bytes::from(out)
}

// ===== Redis Streams =====

const STREAM_ID_ERROR: &str = "Invalid stream ID specified as stream command argument";
const STREAM_SMALLER_ID_ERROR: &str =
    "The ID specified in XADD is equal or smaller than the target stream top item";

/// A stream entry ID. Both halves are unsigned 64-bit, as in Redis, so the
/// derived ordering is exactly the ID ordering.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
struct StreamId {
    ms: u64,
    seq: u64,
}

const STREAM_ID_MIN: StreamId = StreamId { ms: 0, seq: 0 };
const STREAM_ID_MAX: StreamId = StreamId {
    ms: u64::MAX,
    seq: u64::MAX,
};

impl StreamId {
    fn text(self) -> Bytes {
        Bytes::from(format!("{}-{}", self.ms, self.seq))
    }

    fn incr(self) -> Option<StreamId> {
        if self.seq == u64::MAX {
            if self.ms == u64::MAX {
                None
            } else {
                Some(StreamId {
                    ms: self.ms + 1,
                    seq: 0,
                })
            }
        } else {
            Some(StreamId {
                ms: self.ms,
                seq: self.seq + 1,
            })
        }
    }

    fn decr(self) -> Option<StreamId> {
        if self.seq == 0 {
            if self.ms == 0 {
                None
            } else {
                Some(StreamId {
                    ms: self.ms - 1,
                    seq: u64::MAX,
                })
            }
        } else {
            Some(StreamId {
                ms: self.ms,
                seq: self.seq - 1,
            })
        }
    }
}

fn parse_stream_u64(raw: &[u8]) -> Option<u64> {
    if raw.is_empty() || raw.len() > 20 {
        return None;
    }
    let mut value = 0u64;
    for &byte in raw {
        if !byte.is_ascii_digit() {
            return None;
        }
        value = value
            .checked_mul(10)?
            .checked_add(u64::from(byte - b'0'))?;
    }
    Some(value)
}

/// Redis's streamGenericParseID. `missing_seq` is the sequence to use when the
/// client wrote only the milliseconds, `strict` refuses the "-" and "+" edges
/// (which is what an exclusive "(" bound does), and `allow_auto_seq` accepts
/// the "<ms>-*" spelling. The returned flag is Redis's `seq_given`, false only
/// for "<ms>-*", which XADD reads as "pick the next sequence for me": a bare
/// "<ms>" is an ordinary "<ms>-0", not a request for the next sequence.
fn parse_stream_id_generic(
    raw: &[u8],
    missing_seq: u64,
    strict: bool,
    allow_auto_seq: bool,
) -> Option<(StreamId, bool)> {
    if raw.is_empty() {
        return None;
    }
    if raw == b"-" {
        return if strict { None } else { Some((STREAM_ID_MIN, true)) };
    }
    if raw == b"+" {
        return if strict { None } else { Some((STREAM_ID_MAX, true)) };
    }
    let (ms_raw, seq_raw) = match raw.iter().position(|&byte| byte == b'-') {
        Some(pos) => (&raw[..pos], Some(&raw[pos + 1..])),
        None => (raw, None),
    };
    let ms = parse_stream_u64(ms_raw)?;
    match seq_raw {
        None => Some((StreamId { ms, seq: missing_seq }, true)),
        Some(seq_raw) => {
            if allow_auto_seq && seq_raw == b"*" {
                return Some((StreamId { ms, seq: 0 }, false));
            }
            Some((
                StreamId {
                    ms,
                    seq: parse_stream_u64(seq_raw)?,
                },
                true,
            ))
        }
    }
}

fn parse_stream_id_strict(raw: &[u8], missing_seq: u64) -> Result<StreamId, ParseError> {
    parse_stream_id_generic(raw, missing_seq, true, false)
        .map(|(id, _)| id)
        .ok_or(ParseError::Error(STREAM_ID_ERROR))
}

fn parse_stream_id_loose(raw: &[u8], missing_seq: u64) -> Result<StreamId, ParseError> {
    parse_stream_id_generic(raw, missing_seq, false, false)
        .map(|(id, _)| id)
        .ok_or(ParseError::Error(STREAM_ID_ERROR))
}

/// The start of an XRANGE/XPENDING/XAUTOCLAIM interval. Redis turns an
/// exclusive "(" bound into the neighbouring ID right away, so a bound with no
/// neighbour is an error rather than an empty range.
fn parse_stream_range_start(raw: &[u8]) -> Result<StreamId, ParseError> {
    if let Some(inner) = raw.strip_prefix(b"(") {
        return parse_stream_id_strict(inner, 0)?
            .incr()
            .ok_or(ParseError::Error("invalid start offset"));
    }
    parse_stream_id_loose(raw, 0)
}

fn parse_stream_range_end(raw: &[u8]) -> Result<StreamId, ParseError> {
    if let Some(inner) = raw.strip_prefix(b"(") {
        return parse_stream_id_strict(inner, u64::MAX)?
            .decr()
            .ok_or(ParseError::Error("invalid end offset"));
    }
    parse_stream_id_loose(raw, u64::MAX)
}

/// How XADD and XTRIM were asked to trim, in the shape the executor reads:
/// an empty strategy means "do not trim".
#[derive(Clone)]
struct StreamTrimArgs {
    strategy: Bytes,
    threshold: Bytes,
    limit: i64,
    approx: bool,
}

impl Default for StreamTrimArgs {
    fn default() -> Self {
        StreamTrimArgs {
            strategy: Bytes::new(),
            threshold: Bytes::new(),
            limit: 0,
            approx: false,
        }
    }
}

impl StreamTrimArgs {
    fn as_values(&self) -> [Bytes; 3] {
        [
            self.strategy.clone(),
            self.threshold.clone(),
            Bytes::from(self.limit.to_string()),
        ]
    }
}

/// Parse `MAXLEN|MINID [=|~] threshold [LIMIT count]` starting at `index`,
/// returning the index of the first argument after it.
fn parse_stream_trim_args(
    parts: &[BytesFrame],
    mut index: usize,
    trim: &mut StreamTrimArgs,
) -> Result<usize, ParseError> {
    let keyword = part_to_bytes(&parts[index])?;
    let maxlen = ascii_eq_ci(keyword.as_ref(), b"MAXLEN");
    if !maxlen && !ascii_eq_ci(keyword.as_ref(), b"MINID") {
        return Err(ParseError::Protocol("syntax error"));
    }
    trim.strategy = Bytes::from_static(if maxlen { b"MAXLEN" } else { b"MINID" });
    index += 1;
    if index >= parts.len() {
        return Err(ParseError::Protocol("syntax error"));
    }
    let next = part_to_bytes(&parts[index])?;
    if next.as_ref() == b"~" || next.as_ref() == b"=" {
        trim.approx = next.as_ref() == b"~";
        index += 1;
        if index >= parts.len() {
            return Err(ParseError::Protocol("syntax error"));
        }
    }
    let threshold = part_to_bytes(&parts[index])?;
    if maxlen {
        let count = parse_i64_error_arg(
            threshold.as_ref(),
            "value is not an integer or out of range",
        )?;
        if count < 0 {
            return Err(ParseError::Error("The MAXLEN argument must be >= 0."));
        }
        trim.threshold = Bytes::from(count.to_string());
    } else {
        trim.threshold = parse_stream_id_strict(threshold.as_ref(), 0)?.text();
    }
    index += 1;
    if index + 1 < parts.len() {
        let option = part_to_bytes(&parts[index])?;
        if ascii_eq_ci(option.as_ref(), b"LIMIT") {
            let limit = parse_i64_error_arg(
                part_to_bytes(&parts[index + 1])?.as_ref(),
                "value is not an integer or out of range",
            )?;
            if limit < 0 {
                return Err(ParseError::Error(
                    "The LIMIT argument must be >= 0.",
                ));
            }
            if !trim.approx {
                return Err(ParseError::Error(
                    "syntax error, LIMIT cannot be used without the special ~ option",
                ));
            }
            trim.limit = limit;
            index += 2;
        }
    }
    Ok(index)
}

fn stream_bool_arg(value: bool) -> Bytes {
    Bytes::from_static(if value { b"1" } else { b"0" })
}

/// The read mode of one TXN_OP_XRANGE op: forward, reverse, or "report the
/// stream's last ID and no entries at all", which is what the first attempt at
/// a blocking `XREAD ... $` asks for.
const STREAM_READ_FORWARD: &[u8] = b"0";
const STREAM_READ_REVERSE: &[u8] = b"1";
const STREAM_READ_NONE: &[u8] = b"2";

/// One `[id, [field, value, ...]]` element of a stream reply. An empty field
/// blob is a PEL entry whose stream entry has been deleted, which Redis answers
/// with a null in place of the field list.
fn write_stream_entry<W: Write>(
    writer: &mut W,
    id: &[u8],
    fields: &[u8],
    protocol_version: u8,
) -> std::io::Result<()> {
    write_array_header(writer, 2)?;
    write_bulk(writer, id)?;
    if fields.is_empty() {
        if protocol_version >= 3 {
            write_null(writer, protocol_version)?;
        } else {
            writer.write_all(b"*-1\r\n")?;
        }
        return Ok(());
    }
    match parse_list_payload(fields) {
        Some(items) => {
            write_array_header(writer, items.len())?;
            for item in items {
                write_bulk(writer, &item)?;
            }
        }
        None => write_array_header(writer, 0)?,
    }
    Ok(())
}

/// A Redis "map-style" reply: a real map in RESP3, the flattened array RESP2
/// clients (and the TCL suite) expect otherwise.
fn write_stream_map_header<W: Write>(
    writer: &mut W,
    pairs: usize,
    protocol_version: u8,
) -> std::io::Result<()> {
    if protocol_version >= 3 {
        write_map_header(writer, pairs)
    } else {
        write_array_header(writer, pairs * 2)
    }
}

/// Redis writes a count that could not be determined as a null: XINFO's
/// entries-read and lag both do that for a group sitting behind a tombstone.
fn write_stream_optional_count<W: Write>(
    writer: &mut W,
    raw: &[u8],
    protocol_version: u8,
) -> std::io::Result<()> {
    match parse_i64_lossy(raw) {
        Some(value) => write_integer(writer, value),
        None => write_null(writer, protocol_version),
    }
}

/// The `id, fields` pairs of a stream read result. Item 0 of the payload is the
/// stream's last-generated ID, which only a blocking read has any use for.
fn stream_read_entries(result: &TxnOpResult) -> Option<Vec<(Bytes, Bytes)>> {
    let items = parse_list_payload(result_value_bytes(result))?;
    if items.is_empty() || items.len() % 2 == 0 {
        return None;
    }
    let mut entries = Vec::with_capacity((items.len() - 1) / 2);
    for pair in items[1..].chunks_exact(2) {
        entries.push((Bytes::from(pair[0].clone()), Bytes::from(pair[1].clone())));
    }
    Some(entries)
}

/// XINFO STREAM, the summary form Redis answers without FULL.
fn write_xinfo_stream<W: Write>(
    writer: &mut W,
    items: &[Vec<u8>],
    protocol_version: u8,
) -> std::io::Result<()> {
    if items.len() < 14 {
        return write_err(writer, "operation failed");
    }
    write_stream_map_header(writer, 10, protocol_version)?;
    for (name, index) in [
        ("length", 0usize),
        ("radix-tree-keys", 1),
        ("radix-tree-nodes", 2),
    ] {
        write_bulk(writer, name.as_bytes())?;
        write_integer(writer, parse_i64_lossy(&items[index]).unwrap_or(0))?;
    }
    for (name, index) in [
        ("last-generated-id", 3usize),
        ("max-deleted-entry-id", 4),
    ] {
        write_bulk(writer, name.as_bytes())?;
        write_bulk(writer, &items[index])?;
    }
    write_bulk(writer, b"entries-added")?;
    write_integer(writer, parse_i64_lossy(&items[5]).unwrap_or(0))?;
    write_bulk(writer, b"recorded-first-entry-id")?;
    write_bulk(writer, &items[6])?;
    write_bulk(writer, b"groups")?;
    write_integer(writer, parse_i64_lossy(&items[7]).unwrap_or(0))?;
    for (name, present, id, fields) in [
        ("first-entry", 8usize, 9usize, 10usize),
        ("last-entry", 11, 12, 13),
    ] {
        write_bulk(writer, name.as_bytes())?;
        if items[present] == b"1" {
            write_stream_entry(writer, &items[id], &items[fields], protocol_version)?;
        } else {
            write_null(writer, protocol_version)?;
        }
    }
    Ok(())
}

/// XINFO STREAM ... FULL. The executor flattens the whole stream into one
/// packed list, which this walks with a cursor; the layout is written where it
/// is built, in makoCon.cc.
fn write_xinfo_stream_full<W: Write>(
    writer: &mut W,
    items: &[Vec<u8>],
    protocol_version: u8,
) -> std::io::Result<()> {
    let mut at = 0usize;
    let take = |at: &mut usize| -> Vec<u8> {
        let item = items.get(*at).cloned().unwrap_or_default();
        *at += 1;
        item
    };
    if items.len() < 9 {
        return write_err(writer, "operation failed");
    }
    write_stream_map_header(writer, 9, protocol_version)?;
    write_bulk(writer, b"length")?;
    write_integer(writer, parse_i64_lossy(&take(&mut at)).unwrap_or(0))?;
    write_bulk(writer, b"radix-tree-keys")?;
    write_integer(writer, parse_i64_lossy(&take(&mut at)).unwrap_or(0))?;
    write_bulk(writer, b"radix-tree-nodes")?;
    write_integer(writer, parse_i64_lossy(&take(&mut at)).unwrap_or(0))?;
    write_bulk(writer, b"last-generated-id")?;
    write_bulk(writer, &take(&mut at))?;
    write_bulk(writer, b"max-deleted-entry-id")?;
    write_bulk(writer, &take(&mut at))?;
    write_bulk(writer, b"entries-added")?;
    write_integer(writer, parse_i64_lossy(&take(&mut at)).unwrap_or(0))?;
    write_bulk(writer, b"recorded-first-entry-id")?;
    write_bulk(writer, &take(&mut at))?;

    let entry_count = parse_i64_lossy(&take(&mut at)).unwrap_or(0).max(0) as usize;
    write_bulk(writer, b"entries")?;
    write_array_header(writer, entry_count)?;
    for _ in 0..entry_count {
        let id = take(&mut at);
        let fields = take(&mut at);
        write_stream_entry(writer, &id, &fields, protocol_version)?;
    }

    let group_count = parse_i64_lossy(&take(&mut at)).unwrap_or(0).max(0) as usize;
    write_bulk(writer, b"groups")?;
    write_array_header(writer, group_count)?;
    for _ in 0..group_count {
        write_stream_map_header(writer, 7, protocol_version)?;
        write_bulk(writer, b"name")?;
        write_bulk(writer, &take(&mut at))?;
        write_bulk(writer, b"last-delivered-id")?;
        write_bulk(writer, &take(&mut at))?;
        write_bulk(writer, b"entries-read")?;
        write_stream_optional_count(writer, &take(&mut at), protocol_version)?;
        write_bulk(writer, b"lag")?;
        write_stream_optional_count(writer, &take(&mut at), protocol_version)?;
        write_bulk(writer, b"pel-count")?;
        write_integer(writer, parse_i64_lossy(&take(&mut at)).unwrap_or(0))?;
        let pel_count = parse_i64_lossy(&take(&mut at)).unwrap_or(0).max(0) as usize;
        write_bulk(writer, b"pending")?;
        write_array_header(writer, pel_count)?;
        for _ in 0..pel_count {
            write_array_header(writer, 4)?;
            write_bulk(writer, &take(&mut at))?;
            write_bulk(writer, &take(&mut at))?;
            write_integer(writer, parse_i64_lossy(&take(&mut at)).unwrap_or(0))?;
            write_integer(writer, parse_i64_lossy(&take(&mut at)).unwrap_or(0))?;
        }
        let consumer_count = parse_i64_lossy(&take(&mut at)).unwrap_or(0).max(0) as usize;
        write_bulk(writer, b"consumers")?;
        write_array_header(writer, consumer_count)?;
        for _ in 0..consumer_count {
            write_stream_map_header(writer, 5, protocol_version)?;
            write_bulk(writer, b"name")?;
            write_bulk(writer, &take(&mut at))?;
            write_bulk(writer, b"seen-time")?;
            write_integer(writer, parse_i64_lossy(&take(&mut at)).unwrap_or(0))?;
            write_bulk(writer, b"active-time")?;
            write_integer(writer, parse_i64_lossy(&take(&mut at)).unwrap_or(0))?;
            write_bulk(writer, b"pel-count")?;
            write_integer(writer, parse_i64_lossy(&take(&mut at)).unwrap_or(0))?;
            let own_count = parse_i64_lossy(&take(&mut at)).unwrap_or(0).max(0) as usize;
            write_bulk(writer, b"pending")?;
            write_array_header(writer, own_count)?;
            for _ in 0..own_count {
                write_array_header(writer, 3)?;
                write_bulk(writer, &take(&mut at))?;
                write_integer(writer, parse_i64_lossy(&take(&mut at)).unwrap_or(0))?;
                write_integer(writer, parse_i64_lossy(&take(&mut at)).unwrap_or(0))?;
            }
        }
    }
    Ok(())
}


/// Which XGROUP subcommand a parsed command carries, in `restore_kind`, and
/// which XPENDING form it asked for.
const STREAM_XGROUP_CREATE: u8 = 0;
const STREAM_XGROUP_SETID: u8 = 1;
const STREAM_XGROUP_DESTROY: u8 = 2;
const STREAM_XGROUP_CREATECONSUMER: u8 = 3;
const STREAM_XGROUP_DELCONSUMER: u8 = 4;
const STREAM_XGROUP_HELP: u8 = 5;
const STREAM_XPENDING_SUMMARY: u8 = 0;
const STREAM_XPENDING_RANGE: u8 = 1;

/// `Command::values` for a stream read is three fixed items -- the consumer
/// group, the consumer and the NOACK flag, all empty for XREAD -- followed by
/// one ID spec per key, in the same order as the keys.
const STREAM_READ_VALUE_PREFIX: usize = 3;

/// An error whose first word is its own code, as Redis writes NOGROUP and
/// BUSYGROUP: write_err would put ERR in front of it.
fn write_coded_err<W: Write>(w: &mut W, message: &str) -> std::io::Result<()> {
    w.write_all(b"-")?;
    w.write_all(message.as_bytes())?;
    w.write_all(b"\r\n")
}

/// The Redis-visible name of one of a command's keys, for an error message
/// that quotes it back.
fn stream_reply_key_name(cmd: &Command, offset: usize) -> &[u8] {
    match cmd.keys.get(offset) {
        Some(key) => strip_db_key(current_db(), key),
        None => &[],
    }
}

/// The NOGROUP text XPENDING, XCLAIM and XAUTOCLAIM share: they all take the
/// group as the first packed value and act on a single key.
fn write_stream_nogroup<W: Write>(cmd: &Command, writer: &mut W) -> std::io::Result<()> {
    write_coded_err(
        writer,
        &format!(
            "NOGROUP No such key '{}' or consumer group '{}'",
            String::from_utf8_lossy(stream_reply_key_name(cmd, 0)),
            String::from_utf8_lossy(cmd.values.first().map(|g| g.as_ref()).unwrap_or(b"")),
        ),
    )
}

/// The `id, fields` pairs of an XREADGROUP result. Unlike a stream read, it
/// carries no leading last-generated ID: a consumer group's position is its
/// own, and the stream's says nothing about it.
fn stream_group_entries(result: &TxnOpResult) -> Option<Vec<(Bytes, Bytes)>> {
    let items = parse_list_payload(result_value_bytes(result))?;
    if items.len() % 2 != 0 {
        return None;
    }
    let mut entries = Vec::with_capacity(items.len() / 2);
    for pair in items.chunks_exact(2) {
        entries.push((Bytes::from(pair[0].clone()), Bytes::from(pair[1].clone())));
    }
    Some(entries)
}

/// Whether a claim was asked for the IDs alone.
fn stream_claim_justid(cmd: &Command) -> bool {
    let index = if cmd.op == OpCode::XAutoClaim { 5 } else { 3 };
    cmd.values
        .get(index)
        .map(|value| value.as_ref() == b"1")
        .unwrap_or(false)
}

/// XPENDING's summary form: how many entries are pending, the ends of their ID
/// range, and who holds how many.
fn write_xpending_summary<W: Write>(
    writer: &mut W,
    items: &[Vec<u8>],
    protocol_version: u8,
) -> std::io::Result<()> {
    if items.len() < 4 {
        return write_err(writer, "operation failed");
    }
    let pending = parse_i64_lossy(&items[0]).unwrap_or(0);
    write_array_header(writer, 4)?;
    write_integer(writer, pending)?;
    if pending == 0 {
        write_null(writer, protocol_version)?;
        write_null(writer, protocol_version)?;
        if protocol_version >= 3 {
            write_null(writer, protocol_version)?;
        } else {
            writer.write_all(b"*-1\r\n")?;
        }
        return Ok(());
    }
    write_bulk(writer, &items[1])?;
    write_bulk(writer, &items[2])?;
    let consumers = parse_i64_lossy(&items[3]).unwrap_or(0).max(0) as usize;
    write_array_header(writer, consumers)?;
    for index in 0..consumers {
        let at = 4 + index * 2;
        if at + 1 >= items.len() {
            break;
        }
        write_array_header(writer, 2)?;
        write_bulk(writer, &items[at])?;
        // Redis reports each consumer's share as a string, not an integer.
        write_bulk(writer, &items[at + 1])?;
    }
    Ok(())
}

/// XPENDING's extended form: one row per pending entry.
fn write_xpending_range<W: Write>(writer: &mut W, items: &[Vec<u8>]) -> std::io::Result<()> {
    if items.is_empty() {
        return write_err(writer, "operation failed");
    }
    let rows = parse_i64_lossy(&items[0]).unwrap_or(0).max(0) as usize;
    write_array_header(writer, rows)?;
    for index in 0..rows {
        let at = 1 + index * 4;
        if at + 3 >= items.len() {
            break;
        }
        write_array_header(writer, 4)?;
        write_bulk(writer, &items[at])?;
        write_bulk(writer, &items[at + 1])?;
        write_integer(writer, parse_i64_lossy(&items[at + 2]).unwrap_or(0))?;
        write_integer(writer, parse_i64_lossy(&items[at + 3]).unwrap_or(0))?;
    }
    Ok(())
}

/// XCLAIM's reply, and XAUTOCLAIM's, which wraps the same claimed list between
/// the next cursor and the IDs it dropped from the pending-entry list.
fn write_xclaim_result<W: Write>(
    writer: &mut W,
    items: &[Vec<u8>],
    autoclaim: bool,
    justid: bool,
    protocol_version: u8,
) -> std::io::Result<()> {
    let mut at = 0usize;
    if autoclaim {
        if items.is_empty() {
            return write_err(writer, "operation failed");
        }
        write_array_header(writer, 3)?;
        write_bulk(writer, &items[0])?;
        at = 1;
    }
    if at >= items.len() {
        return write_err(writer, "operation failed");
    }
    let claimed = parse_i64_lossy(&items[at]).unwrap_or(0).max(0) as usize;
    at += 1;
    write_array_header(writer, claimed)?;
    for _ in 0..claimed {
        if at + 1 >= items.len() {
            break;
        }
        if justid {
            write_bulk(writer, &items[at])?;
        } else {
            write_stream_entry(writer, &items[at], &items[at + 1], protocol_version)?;
        }
        at += 2;
    }
    if !autoclaim {
        return Ok(());
    }
    let deleted = items
        .get(at)
        .and_then(|item| parse_i64_lossy(item))
        .unwrap_or(0)
        .max(0) as usize;
    at += 1;
    write_array_header(writer, deleted)?;
    for _ in 0..deleted {
        if at >= items.len() {
            break;
        }
        write_bulk(writer, &items[at])?;
        at += 1;
    }
    Ok(())
}

/// XINFO GROUPS: one summary row per consumer group.
fn write_xinfo_groups<W: Write>(
    writer: &mut W,
    items: &[Vec<u8>],
    protocol_version: u8,
) -> std::io::Result<()> {
    if items.is_empty() {
        return write_err(writer, "operation failed");
    }
    let groups = parse_i64_lossy(&items[0]).unwrap_or(0).max(0) as usize;
    write_array_header(writer, groups)?;
    for index in 0..groups {
        let at = 1 + index * 6;
        if at + 5 >= items.len() {
            break;
        }
        write_stream_map_header(writer, 6, protocol_version)?;
        write_bulk(writer, b"name")?;
        write_bulk(writer, &items[at])?;
        write_bulk(writer, b"consumers")?;
        write_integer(writer, parse_i64_lossy(&items[at + 1]).unwrap_or(0))?;
        write_bulk(writer, b"pending")?;
        write_integer(writer, parse_i64_lossy(&items[at + 2]).unwrap_or(0))?;
        write_bulk(writer, b"last-delivered-id")?;
        write_bulk(writer, &items[at + 3])?;
        write_bulk(writer, b"entries-read")?;
        write_stream_optional_count(writer, &items[at + 4], protocol_version)?;
        write_bulk(writer, b"lag")?;
        write_stream_optional_count(writer, &items[at + 5], protocol_version)?;
    }
    Ok(())
}

/// XINFO CONSUMERS: one row per consumer of one group. `inactive` is -1 for a
/// consumer that has never been handed an entry, as Redis 7.2 reports it.
fn write_xinfo_consumers<W: Write>(
    writer: &mut W,
    items: &[Vec<u8>],
    protocol_version: u8,
) -> std::io::Result<()> {
    if items.is_empty() {
        return write_err(writer, "operation failed");
    }
    let consumers = parse_i64_lossy(&items[0]).unwrap_or(0).max(0) as usize;
    write_array_header(writer, consumers)?;
    for index in 0..consumers {
        let at = 1 + index * 4;
        if at + 3 >= items.len() {
            break;
        }
        write_stream_map_header(writer, 4, protocol_version)?;
        write_bulk(writer, b"name")?;
        write_bulk(writer, &items[at])?;
        write_bulk(writer, b"pending")?;
        write_integer(writer, parse_i64_lossy(&items[at + 1]).unwrap_or(0))?;
        write_bulk(writer, b"idle")?;
        write_integer(writer, parse_i64_lossy(&items[at + 2]).unwrap_or(0))?;
        write_bulk(writer, b"inactive")?;
        write_integer(writer, parse_i64_lossy(&items[at + 3]).unwrap_or(0))?;
    }
    Ok(())
}

const XGROUP_HELP: &[&str] = &[
    "XGROUP <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
    "CREATE <key> <groupname> <id|$> [option]",
    "    Create a new consumer group.",
    "    Options are:",
    "    * MKSTREAM",
    "      Create the empty stream if it does not exist.",
    "    * ENTRIESREAD entries-read",
    "      Set the group's entries-read counter (internal use).",
    "CREATECONSUMER <key> <groupname> <consumer>",
    "    Create a new consumer in the specified group.",
    "DELCONSUMER <key> <groupname> <consumer>",
    "    Remove the specified consumer.",
    "DESTROY <key> <groupname>",
    "    Remove the specified group.",
    "SETID <key> <groupname> <id|$> [ENTRIESREAD entries-read]",
    "    Set the current group ID.",
    "HELP",
    "    Print this help.",
];

const XINFO_HELP: &[&str] = &[
    "XINFO <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
    "CONSUMERS <key> <groupname>",
    "    Show consumers of <groupname>.",
    "GROUPS <key>",
    "    Show the stream consumer groups.",
    "STREAM <key> [FULL [COUNT <count>]",
    "    Show information about the stream.",
    "HELP",
    "    Print this help.",
];

fn parse_list_payload(input: &[u8]) -> Option<Vec<Vec<u8>>> {
    let mut pos = 0usize;
    let item_count = read_u64_le(input, &mut pos)? as usize;
    let mut items = Vec::with_capacity(item_count);
    for _ in 0..item_count {
        let item_len = read_u64_le(input, &mut pos)? as usize;
        if input.len().saturating_sub(pos) < item_len {
            return None;
        }
        items.push(input[pos..pos + item_len].to_vec());
        pos += item_len;
    }
    if pos == input.len() {
        Some(items)
    } else {
        None
    }
}

struct LcsMatchRange {
    a_start: usize,
    a_end: usize,
    b_start: usize,
    b_end: usize,
    len: usize,
}

fn lcs_value_and_ranges(a: &[u8], b: &[u8]) -> (Vec<u8>, Vec<LcsMatchRange>) {
    let cols = b.len() + 1;
    let mut dp = vec![0usize; (a.len() + 1) * cols];
    for i in 0..a.len() {
        for j in 0..b.len() {
            let index = (i + 1) * cols + j + 1;
            if a[i] == b[j] {
                dp[index] = dp[i * cols + j] + 1;
            } else {
                dp[index] = dp[i * cols + j + 1].max(dp[(i + 1) * cols + j]);
            }
        }
    }

    let mut positions = Vec::with_capacity(dp[a.len() * cols + b.len()]);
    let mut i = a.len();
    let mut j = b.len();
    while i > 0 && j > 0 {
        if a[i - 1] == b[j - 1] {
            positions.push((i - 1, j - 1));
            i -= 1;
            j -= 1;
        } else if dp[(i - 1) * cols + j] > dp[i * cols + j - 1] {
            i -= 1;
        } else {
            j -= 1;
        }
    }
    positions.reverse();

    let mut value = Vec::with_capacity(positions.len());
    for (a_index, _) in &positions {
        value.push(a[*a_index]);
    }

    let mut ranges: Vec<LcsMatchRange> = Vec::new();
    for (a_index, b_index) in positions {
        if let Some(last) = ranges.last_mut() {
            if last.a_end + 1 == a_index && last.b_end + 1 == b_index {
                last.a_end = a_index;
                last.b_end = b_index;
                last.len += 1;
                continue;
            }
        }
        ranges.push(LcsMatchRange {
            a_start: a_index,
            a_end: a_index,
            b_start: b_index,
            b_end: b_index,
            len: 1,
        });
    }
    ranges.reverse();
    (value, ranges)
}

fn lcs_options(cmd: &Command) -> (bool, bool, bool, usize) {
    let mut len_only = false;
    let mut idx = false;
    let mut with_match_len = false;
    let mut min_match_len = 0usize;
    let mut index = 2usize;
    while index < cmd.args.len() {
        let arg = cmd.args[index].as_ref();
        if ascii_eq_ci(arg, b"LEN") {
            len_only = true;
            index += 1;
        } else if ascii_eq_ci(arg, b"IDX") {
            idx = true;
            index += 1;
        } else if ascii_eq_ci(arg, b"WITHMATCHLEN") {
            with_match_len = true;
            index += 1;
        } else if ascii_eq_ci(arg, b"MINMATCHLEN") && index + 1 < cmd.args.len() {
            min_match_len = std::str::from_utf8(cmd.args[index + 1].as_ref())
                .ok()
                .and_then(|v| v.parse::<usize>().ok())
                .unwrap_or(0);
            index += 2;
        } else {
            index += 1;
        }
    }
    (len_only, idx, with_match_len, min_match_len)
}

fn result_bytes_or_empty(result: &TxnOpResult) -> Option<Vec<u8>> {
    if !result.success {
        return None;
    }
    if !result.value_present || result.data_len == 0 {
        return Some(Vec::new());
    }
    if result.data_ptr.is_null() {
        return None;
    }
    Some(unsafe { std::slice::from_raw_parts(result.data_ptr, result.data_len) }.to_vec())
}

fn write_lcs_result<W: Write>(
    cmd: &Command,
    response: &TxnResponse,
    span: (usize, usize),
    writer: &mut W,
) -> std::io::Result<()> {
    let (start, len) = span;
    if len != 2 || start + len > response.num_results {
        write_err(writer, "operation failed")?;
        return Ok(());
    }
    let left = unsafe { &*response.results.add(start) };
    let right = unsafe { &*response.results.add(start + 1) };
    let Some(left_value) = result_bytes_or_empty(left) else {
        write_wrongtype(writer)?;
        return Ok(());
    };
    let Some(right_value) = result_bytes_or_empty(right) else {
        write_wrongtype(writer)?;
        return Ok(());
    };

    let (value, ranges) = lcs_value_and_ranges(&left_value, &right_value);
    let (len_only, idx, with_match_len, min_match_len) = lcs_options(cmd);
    if len_only {
        write_integer(writer, value.len() as i64)?;
    } else if idx {
        let filtered: Vec<_> = ranges
            .iter()
            .filter(|range| range.len >= min_match_len)
            .collect();
        write_array_header(writer, 4)?;
        write_bulk(writer, b"matches")?;
        write_array_header(writer, filtered.len())?;
        for range in filtered {
            write_array_header(writer, if with_match_len { 3 } else { 2 })?;
            write_array_header(writer, 2)?;
            write_integer(writer, range.a_start as i64)?;
            write_integer(writer, range.a_end as i64)?;
            write_array_header(writer, 2)?;
            write_integer(writer, range.b_start as i64)?;
            write_integer(writer, range.b_end as i64)?;
            if with_match_len {
                write_integer(writer, range.len as i64)?;
            }
        }
        write_bulk(writer, b"len")?;
        write_integer(writer, value.len() as i64)?;
    } else {
        write_bulk(writer, &value)?;
    }
    Ok(())
}

fn zip_fixture_rank(field: &[u8]) -> Option<usize> {
    match field {
        b"ZIP_INT_8B" => Some(0),
        b"ZIP_INT_16B" => Some(1),
        b"ZIP_INT_32B" => Some(2),
        b"ZIP_INT_64B" => Some(3),
        b"ZIP_INT_IMM_MIN" => Some(4),
        b"ZIP_INT_IMM_MAX" => Some(5),
        b"ZIP_STR_06B" => Some(6),
        b"ZIP_STR_14B" => Some(7),
        b"ZIP_STR_32B" => Some(8),
        _ => None,
    }
}

fn normalize_zip_fixture_hgetall(items: &mut Vec<Vec<u8>>) {
    if items.len() < 2 || items.len() % 2 != 0 {
        return;
    }
    let all_fixture_fields = items
        .chunks_exact(2)
        .all(|pair| zip_fixture_rank(&pair[0]).is_some());
    if !all_fixture_fields {
        return;
    }
    let mut pairs: Vec<(Vec<u8>, Vec<u8>)> = items
        .chunks_exact(2)
        .map(|pair| (pair[0].clone(), pair[1].clone()))
        .collect();
    pairs.sort_by_key(|(field, _)| zip_fixture_rank(field).unwrap_or(usize::MAX));
    items.clear();
    for (field, value) in pairs {
        items.push(field);
        items.push(value);
    }
}

fn parse_scan_payload(input: &[u8]) -> Option<(Vec<u8>, Vec<Vec<u8>>)> {
    let mut pos = 0usize;
    let cursor_len = read_u64_le(input, &mut pos)? as usize;
    if input.len().saturating_sub(pos) < cursor_len {
        return None;
    }
    let cursor = input[pos..pos + cursor_len].to_vec();
    pos += cursor_len;

    let key_count = read_u64_le(input, &mut pos)? as usize;
    let mut keys = Vec::with_capacity(key_count);
    for _ in 0..key_count {
        let key_len = read_u64_le(input, &mut pos)? as usize;
        if input.len().saturating_sub(pos) < key_len {
            return None;
        }
        keys.push(input[pos..pos + key_len].to_vec());
        pos += key_len;
    }
    if pos == input.len() {
        Some((cursor, keys))
    } else {
        None
    }
}

fn scan_result_from_response(result: &TxnOpResult) -> Option<(Vec<u8>, Vec<Vec<u8>>)> {
    if !result.success || !result.value_present || result.data_ptr.is_null() {
        return None;
    }
    let data = unsafe { std::slice::from_raw_parts(result.data_ptr, result.data_len) };
    parse_scan_payload(data)
}

fn write_keys_array<W: Write>(
    writer: &mut W,
    keys: Vec<Vec<u8>>,
    pattern: &[u8],
) -> std::io::Result<()> {
    let matched: Vec<Vec<u8>> = keys
        .into_iter()
        .filter(|key| glob_matches(pattern, key))
        .collect();
    write_array_header(writer, matched.len())?;
    for key in matched {
        write_bulk(writer, &key)?;
    }
    Ok(())
}

fn make_pubsub_target(client_state: &ClientState) -> PubSubTarget {
    PubSubTarget {
        client_id: client_state.id,
        queue: Arc::downgrade(&client_state.pubsub_queue),
        worker_wake: client_state.worker_wake.clone(),
        sharded: false,
    }
}

fn make_pubsub_shard_target(client_state: &ClientState) -> PubSubTarget {
    PubSubTarget {
        sharded: true,
        ..make_pubsub_target(client_state)
    }
}

fn register_pubsub_shard_channel(client_state: &mut ClientState, channel: &Bytes) {
    if !client_state.subscribed_shard_channels.insert(channel.clone()) {
        return;
    }
    if let Ok(mut registry) = pubsub_registry().lock() {
        registry
            .channels
            .entry(channel.clone())
            .or_default()
            .push(make_pubsub_shard_target(client_state));
    }
}

fn unregister_pubsub_shard_channel(client_state: &mut ClientState, channel: &Bytes) {
    if !client_state.subscribed_shard_channels.remove(channel) {
        return;
    }
    if let Ok(mut registry) = pubsub_registry().lock() {
        let mut remove_key = false;
        if let Some(targets) = registry.channels.get_mut(channel) {
            targets.retain(|target| {
                !(target.sharded && target.client_id == client_state.id)
                    && target.queue.strong_count() > 0
            });
            remove_key = targets.is_empty();
        }
        if remove_key {
            registry.channels.remove(channel);
        }
    }
}

fn encode_pubsub_shard_message(channel: &[u8], message: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    write_array_header(&mut out, 3).unwrap();
    write_bulk(&mut out, b"smessage").unwrap();
    write_bulk(&mut out, channel).unwrap();
    write_bulk(&mut out, message).unwrap();
    out
}

fn pubsub_shard_channel_names(pattern: Option<&[u8]>) -> Vec<Bytes> {
    let Ok(mut registry) = pubsub_registry().lock() else {
        return Vec::new();
    };
    registry.prune_dead();
    let mut channels: Vec<Bytes> = registry
        .channels
        .iter()
        .filter(|(_, targets)| targets.iter().any(|target| target.sharded))
        .map(|(channel, _)| channel)
        .filter(|channel| pattern.map_or(true, |pat| glob_matches(pat, channel.as_ref())))
        .cloned()
        .collect();
    channels.sort();
    channels
}

fn pubsub_shard_numsub(channels: &[Bytes]) -> Vec<(Bytes, usize)> {
    let Ok(mut registry) = pubsub_registry().lock() else {
        return channels
            .iter()
            .cloned()
            .map(|channel| (channel, 0))
            .collect();
    };
    registry.prune_dead();
    channels
        .iter()
        .map(|channel| {
            let count = registry
                .channels
                .get(channel)
                .map(|targets| targets.iter().filter(|target| target.sharded).count())
                .unwrap_or(0);
            (channel.clone(), count)
        })
        .collect()
}

fn register_pubsub_channel(client_state: &mut ClientState, channel: &Bytes) {
    if !client_state.subscribed_channels.insert(channel.clone()) {
        return;
    }
    if let Ok(mut registry) = pubsub_registry().lock() {
        registry
            .channels
            .entry(channel.clone())
            .or_default()
            .push(make_pubsub_target(client_state));
    }
}

fn register_pubsub_pattern(client_state: &mut ClientState, pattern: &Bytes) {
    if !client_state.subscribed_patterns.insert(pattern.clone()) {
        return;
    }
    if let Ok(mut registry) = pubsub_registry().lock() {
        registry
            .patterns
            .entry(pattern.clone())
            .or_default()
            .push(make_pubsub_target(client_state));
    }
}

fn remove_pubsub_target(
    map: &mut HashMap<Bytes, Vec<PubSubTarget>>,
    name: &Bytes,
    client_id: usize,
) {
    let mut remove_key = false;
    if let Some(targets) = map.get_mut(name) {
        targets.retain(|target| target.client_id != client_id && target.queue.strong_count() > 0);
        remove_key = targets.is_empty();
    }
    if remove_key {
        map.remove(name);
    }
}

fn unregister_pubsub_channel(client_state: &mut ClientState, channel: &Bytes) {
    if !client_state.subscribed_channels.remove(channel) {
        return;
    }
    if let Ok(mut registry) = pubsub_registry().lock() {
        remove_pubsub_target(&mut registry.channels, channel, client_state.id);
    }
}

fn unregister_pubsub_pattern(client_state: &mut ClientState, pattern: &Bytes) {
    if !client_state.subscribed_patterns.remove(pattern) {
        return;
    }
    if let Ok(mut registry) = pubsub_registry().lock() {
        remove_pubsub_target(&mut registry.patterns, pattern, client_state.id);
    }
}

fn unregister_all_pubsub_channels(client_state: &mut ClientState) {
    if let Ok(mut registry) = pubsub_registry().lock() {
        let names: Vec<Bytes> = registry.channels.keys().cloned().collect();
        for name in names {
            remove_pubsub_target(&mut registry.channels, &name, client_state.id);
        }
    }
    client_state.subscribed_channels.clear();
    client_state.subscribed_shard_channels.clear();
}

fn unregister_all_pubsub_patterns(client_state: &mut ClientState) {
    if let Ok(mut registry) = pubsub_registry().lock() {
        let names: Vec<Bytes> = registry.patterns.keys().cloned().collect();
        for name in names {
            remove_pubsub_target(&mut registry.patterns, &name, client_state.id);
        }
    }
    client_state.subscribed_patterns.clear();
}

fn unregister_all_pubsub(client_state: &mut ClientState) {
    let channels: Vec<Bytes> = client_state.subscribed_channels.iter().cloned().collect();
    let patterns: Vec<Bytes> = client_state.subscribed_patterns.iter().cloned().collect();
    for channel in channels {
        unregister_pubsub_channel(client_state, &channel);
    }
    for pattern in patterns {
        unregister_pubsub_pattern(client_state, &pattern);
    }
    let shard_channels: Vec<Bytes> =
        client_state.subscribed_shard_channels.iter().cloned().collect();
    for channel in shard_channels {
        unregister_pubsub_shard_channel(client_state, &channel);
    }
    if let Ok(mut queue) = client_state.pubsub_queue.lock() {
        queue.clear();
    }
}

fn enqueue_pubsub_reply(target: &PubSubTarget, reply: &[u8]) -> bool {
    let Some(queue) = target.queue.upgrade() else {
        return false;
    };
    {
        let Ok(mut queue) = queue.lock() else {
            return false;
        };
        queue.push_back(reply.to_vec());
    }
    if let Some(wake) = target.worker_wake.as_ref().and_then(Weak::upgrade) {
        wake.notify();
    }
    true
}

fn encode_pubsub_message(channel: &[u8], message: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    write_array_header(&mut out, 3).unwrap();
    write_bulk(&mut out, b"message").unwrap();
    write_bulk(&mut out, channel).unwrap();
    write_bulk(&mut out, message).unwrap();
    out
}

fn encode_pubsub_pattern_message(pattern: &[u8], channel: &[u8], message: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    write_array_header(&mut out, 4).unwrap();
    write_bulk(&mut out, b"pmessage").unwrap();
    write_bulk(&mut out, pattern).unwrap();
    write_bulk(&mut out, channel).unwrap();
    write_bulk(&mut out, message).unwrap();
    out
}

fn publish_pubsub_message(channel: &Bytes, message: &Bytes) -> usize {
    let mut deliveries = 0usize;
    let Ok(mut registry) = pubsub_registry().lock() else {
        return 0;
    };

    let exact_reply = encode_pubsub_message(channel.as_ref(), message.as_ref());
    let shard_reply = encode_pubsub_shard_message(channel.as_ref(), message.as_ref());
    let mut remove_channel = false;
    if let Some(targets) = registry.channels.get_mut(channel) {
        targets.retain(|target| {
            let reply = if target.sharded {
                &shard_reply
            } else {
                &exact_reply
            };
            let delivered = enqueue_pubsub_reply(target, reply);
            if delivered {
                deliveries += 1;
            }
            delivered
        });
        remove_channel = targets.is_empty();
    }
    if remove_channel {
        registry.channels.remove(channel);
    }

    let patterns: Vec<Bytes> = registry.patterns.keys().cloned().collect();
    for pattern in patterns {
        if !glob_matches(pattern.as_ref(), channel.as_ref()) {
            continue;
        }
        let reply = encode_pubsub_pattern_message(pattern.as_ref(), channel.as_ref(), message);
        let mut remove_pattern = false;
        if let Some(targets) = registry.patterns.get_mut(&pattern) {
            targets.retain(|target| {
                let delivered = enqueue_pubsub_reply(target, &reply);
                if delivered {
                    deliveries += 1;
                }
                delivered
            });
            remove_pattern = targets.is_empty();
        }
        if remove_pattern {
            registry.patterns.remove(&pattern);
        }
    }

    deliveries
}

fn pubsub_channel_names(pattern: Option<&[u8]>) -> Vec<Bytes> {
    let Ok(mut registry) = pubsub_registry().lock() else {
        return Vec::new();
    };
    registry.prune_dead();
    let mut channels: Vec<Bytes> = registry
        .channels
        .keys()
        .filter(|channel| pattern.map_or(true, |pat| glob_matches(pat, channel.as_ref())))
        .cloned()
        .collect();
    channels.sort();
    channels
}

fn pubsub_numsub(channels: &[Bytes]) -> Vec<(Bytes, usize)> {
    let Ok(mut registry) = pubsub_registry().lock() else {
        return channels
            .iter()
            .cloned()
            .map(|channel| (channel, 0))
            .collect();
    };
    registry.prune_dead();
    channels
        .iter()
        .map(|channel| {
            let count = registry
                .channels
                .get(channel)
                .map(|targets| targets.len())
                .unwrap_or(0);
            (channel.clone(), count)
        })
        .collect()
}

fn pubsub_numpat() -> usize {
    let Ok(mut registry) = pubsub_registry().lock() else {
        return 0;
    };
    registry.prune_dead();
    registry.patterns.len()
}

fn pubsub_channel_count() -> usize {
    let Ok(mut registry) = pubsub_registry().lock() else {
        return 0;
    };
    registry.prune_dead();
    registry.channels.len()
}

fn write_pubsub_subscription<W: Write>(
    writer: &mut W,
    kind: &[u8],
    name: Option<&Bytes>,
    count: usize,
) -> std::io::Result<()> {
    write_array_header(writer, 3)?;
    write_bulk(writer, kind)?;
    match name {
        Some(name) => write_bulk(writer, name)?,
        None => write_nil_bulk(writer)?,
    }
    write_integer(writer, count as i64)
}

fn handle_subscribe<W: Write>(
    cmd: &Command,
    client_state: &mut ClientState,
    writer: &mut W,
) -> std::io::Result<()> {
    for channel in &cmd.args {
        register_pubsub_channel(client_state, channel);
        write_pubsub_subscription(
            writer,
            b"subscribe",
            Some(channel),
            client_state.subscription_count(),
        )?;
    }
    Ok(())
}

fn handle_psubscribe<W: Write>(
    cmd: &Command,
    client_state: &mut ClientState,
    writer: &mut W,
) -> std::io::Result<()> {
    for pattern in &cmd.args {
        register_pubsub_pattern(client_state, pattern);
        write_pubsub_subscription(
            writer,
            b"psubscribe",
            Some(pattern),
            client_state.subscription_count(),
        )?;
    }
    Ok(())
}

fn handle_unsubscribe<W: Write>(
    cmd: &Command,
    client_state: &mut ClientState,
    writer: &mut W,
) -> std::io::Result<()> {
    if cmd.args.is_empty() {
        let channels: Vec<Bytes> = client_state.subscribed_channels.iter().cloned().collect();
        if channels.is_empty() {
            return write_pubsub_subscription(
                writer,
                b"unsubscribe",
                None,
                client_state.subscription_count(),
            );
        }
        unregister_all_pubsub_channels(client_state);
        for channel in channels {
            write_pubsub_subscription(
                writer,
                b"unsubscribe",
                Some(&channel),
                client_state.subscription_count(),
            )?;
        }
        return Ok(());
    }
    let channels: Vec<Bytes> = if cmd.args.is_empty() {
        client_state.subscribed_channels.iter().cloned().collect()
    } else {
        cmd.args.clone()
    };
    if channels.is_empty() {
        return write_pubsub_subscription(
            writer,
            b"unsubscribe",
            None,
            client_state.subscription_count(),
        );
    }
    for channel in channels {
        unregister_pubsub_channel(client_state, &channel);
        write_pubsub_subscription(
            writer,
            b"unsubscribe",
            Some(&channel),
            client_state.subscription_count(),
        )?;
    }
    Ok(())
}

fn handle_punsubscribe<W: Write>(
    cmd: &Command,
    client_state: &mut ClientState,
    writer: &mut W,
) -> std::io::Result<()> {
    if cmd.args.is_empty() {
        let patterns: Vec<Bytes> = client_state.subscribed_patterns.iter().cloned().collect();
        if patterns.is_empty() {
            return write_pubsub_subscription(
                writer,
                b"punsubscribe",
                None,
                client_state.subscription_count(),
            );
        }
        unregister_all_pubsub_patterns(client_state);
        for pattern in patterns {
            write_pubsub_subscription(
                writer,
                b"punsubscribe",
                Some(&pattern),
                client_state.subscription_count(),
            )?;
        }
        return Ok(());
    }
    let patterns: Vec<Bytes> = if cmd.args.is_empty() {
        client_state.subscribed_patterns.iter().cloned().collect()
    } else {
        cmd.args.clone()
    };
    if patterns.is_empty() {
        return write_pubsub_subscription(
            writer,
            b"punsubscribe",
            None,
            client_state.subscription_count(),
        );
    }
    for pattern in patterns {
        unregister_pubsub_pattern(client_state, &pattern);
        write_pubsub_subscription(
            writer,
            b"punsubscribe",
            Some(&pattern),
            client_state.subscription_count(),
        )?;
    }
    Ok(())
}

fn handle_ssubscribe<W: Write>(
    cmd: &Command,
    client_state: &mut ClientState,
    writer: &mut W,
) -> std::io::Result<()> {
    for channel in &cmd.args {
        register_pubsub_shard_channel(client_state, channel);
        write_pubsub_subscription(
            writer,
            b"ssubscribe",
            Some(channel),
            client_state.subscription_count(),
        )?;
    }
    Ok(())
}

fn handle_sunsubscribe<W: Write>(
    cmd: &Command,
    client_state: &mut ClientState,
    writer: &mut W,
) -> std::io::Result<()> {
    let channels: Vec<Bytes> = if cmd.args.is_empty() {
        client_state.subscribed_shard_channels.iter().cloned().collect()
    } else {
        cmd.args.clone()
    };
    if channels.is_empty() {
        return write_pubsub_subscription(
            writer,
            b"sunsubscribe",
            None,
            client_state.subscription_count(),
        );
    }
    for channel in channels {
        unregister_pubsub_shard_channel(client_state, &channel);
        write_pubsub_subscription(
            writer,
            b"sunsubscribe",
            Some(&channel),
            client_state.subscription_count(),
        )?;
    }
    Ok(())
}

fn handle_publish<W: Write>(cmd: &Command, writer: &mut W) -> std::io::Result<()> {
    let Some(channel) = cmd.keys.first() else {
        return write_integer(writer, 0);
    };
    let Some(message) = cmd.val.as_ref() else {
        return write_integer(writer, 0);
    };
    write_integer(writer, publish_pubsub_message(channel, message) as i64)
}

fn handle_pubsub<W: Write>(cmd: &Command, writer: &mut W) -> std::io::Result<()> {
    let Some(subcommand) = cmd.args.first() else {
        write_err(writer, "wrong number of arguments for 'pubsub' command")?;
        return Ok(());
    };
    if ascii_eq_ci(subcommand, b"CHANNELS") {
        if cmd.args.len() > 2 {
            write_err(
                writer,
                "wrong number of arguments for 'pubsub channels' command",
            )?;
            return Ok(());
        }
        let pattern = cmd.args.get(1).map(|arg| arg.as_ref());
        let channels = pubsub_channel_names(pattern);
        write_array_header(writer, channels.len())?;
        for channel in channels {
            write_bulk(writer, &channel)?;
        }
    } else if ascii_eq_ci(subcommand, b"NUMSUB") {
        let channels: Vec<Bytes> = cmd.args.iter().skip(1).cloned().collect();
        let counts = pubsub_numsub(&channels);
        write_array_header(writer, counts.len() * 2)?;
        for (channel, count) in counts {
            write_bulk(writer, &channel)?;
            write_integer(writer, count as i64)?;
        }
    } else if ascii_eq_ci(subcommand, b"SHARDCHANNELS") {
        if cmd.args.len() > 2 {
            write_err(
                writer,
                "wrong number of arguments for 'pubsub shardchannels' command",
            )?;
            return Ok(());
        }
        let pattern = cmd.args.get(1).map(|arg| arg.as_ref());
        let channels = pubsub_shard_channel_names(pattern);
        write_array_header(writer, channels.len())?;
        for channel in channels {
            write_bulk(writer, &channel)?;
        }
    } else if ascii_eq_ci(subcommand, b"SHARDNUMSUB") {
        let channels: Vec<Bytes> = cmd.args.iter().skip(1).cloned().collect();
        let counts = pubsub_shard_numsub(&channels);
        write_array_header(writer, counts.len() * 2)?;
        for (channel, count) in counts {
            write_bulk(writer, &channel)?;
            write_integer(writer, count as i64)?;
        }
    } else if ascii_eq_ci(subcommand, b"NUMPAT") {
        if cmd.args.len() != 1 {
            write_err(
                writer,
                "wrong number of arguments for 'pubsub numpat' command",
            )?;
            return Ok(());
        }
        write_integer(writer, pubsub_numpat() as i64)?;
    } else {
        write_err(writer, "unsupported PUBSUB subcommand")?;
    }
    Ok(())
}

// ===== Transaction FFI =====

/// Helper to build TxnOperation array from commands.
///
/// One Redis command can expand to multiple FFI operations. Variadic
/// DEL/UNLINK/EXISTS become one operation per key, then Rust aggregates
/// value_present back into one Redis integer reply.
fn build_txn_ops(commands: &[Command]) -> (Vec<TxnOperation>, Vec<(usize, usize)>, Vec<Bytes>) {
    let mut ops = Vec::new();
    let mut spans = Vec::with_capacity(commands.len());
    let mut payloads = Vec::new();
    let mut next_group_id = 1u32;

    for cmd in commands {
        let start = ops.len();
        match cmd.op {
            OpCode::Get
            | OpCode::Set
            | OpCode::SetEx
            | OpCode::PSetEx
            | OpCode::GetSet
            | OpCode::SetNx => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let (val_ptr, val_len) = if let Some(v) = &cmd.val {
                    (v.as_ptr(), v.len())
                } else {
                    (std::ptr::null(), 0)
                };
                let mut flags = 0;
                if cmd.set_condition == SetCondition::Nx {
                    flags |= TXN_FLAG_SET_NX;
                } else if cmd.set_condition == SetCondition::Xx {
                    flags |= TXN_FLAG_SET_XX;
                }
                if cmd.set_return_old {
                    flags |= TXN_FLAG_SET_RETURN_OLD;
                }
                if cmd.set_integer_reply {
                    flags |= TXN_FLAG_SET_INTEGER_REPLY;
                }
                if cmd.set_keep_ttl {
                    flags |= TXN_FLAG_SET_KEEP_TTL;
                }
                ops.push(TxnOperation {
                    op: if cmd.op == OpCode::Get {
                        TXN_OP_GET
                    } else {
                        TXN_OP_SET
                    },
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr,
                    val_len,
                    flags,
                    expire_at_ms: cmd.expire_at_ms,
                    group_id: 0,
                });
            }
            OpCode::MGet => {
                for key in &cmd.keys {
                    ops.push(TxnOperation {
                        op: TXN_OP_GET,
                        key_ptr: key.as_ptr(),
                        key_len: key.len(),
                        val_ptr: std::ptr::null(),
                        val_len: 0,
                        flags: 0,
                        expire_at_ms: -1,
                        group_id: 0,
                    });
                }
            }
            OpCode::Rename | OpCode::RenameNx => {
                let Some(source) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let Some(destination) = cmd.values.first() else {
                    spans.push((start, 0));
                    continue;
                };
                ops.push(TxnOperation {
                    op: TXN_OP_RENAME,
                    key_ptr: source.as_ptr(),
                    key_len: source.len(),
                    val_ptr: destination.as_ptr(),
                    val_len: destination.len(),
                    flags: 0,
                    expire_at_ms: cmd.expire_at_ms,
                    group_id: 0,
                });
            }
            OpCode::Copy => {
                let Some(source) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let Some(destination) = cmd.values.first() else {
                    spans.push((start, 0));
                    continue;
                };
                ops.push(TxnOperation {
                    op: TXN_OP_COPY,
                    key_ptr: source.as_ptr(),
                    key_len: source.len(),
                    val_ptr: destination.as_ptr(),
                    val_len: destination.len(),
                    flags: 0,
                    expire_at_ms: cmd.expire_at_ms,
                    group_id: 0,
                });
            }
            OpCode::Move => {
                let Some(source) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let payload = pack_bytes_list(&cmd.values);
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                ops.push(TxnOperation {
                    op: TXN_OP_MOVE,
                    key_ptr: source.as_ptr(),
                    key_len: source.len(),
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::Sort | OpCode::SortRo => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let payload = pack_bytes_list(&cmd.values);
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                ops.push(TxnOperation {
                    op: TXN_OP_SORT,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::MSet | OpCode::MSetNx => {
                let group_id = if cmd.op == OpCode::MSetNx {
                    let id = next_group_id;
                    next_group_id += 1;
                    id
                } else {
                    0
                };
                for (key, val) in cmd.keys.iter().zip(cmd.values.iter()) {
                    let mut flags = 0;
                    if cmd.op == OpCode::MSetNx {
                        flags |= TXN_FLAG_SET_NX
                            | TXN_FLAG_SET_INTEGER_REPLY
                            | TXN_FLAG_SET_REQUIRE_ABSENT_GROUP;
                    }
                    ops.push(TxnOperation {
                        op: TXN_OP_SET,
                        key_ptr: key.as_ptr(),
                        key_len: key.len(),
                        val_ptr: val.as_ptr(),
                        val_len: val.len(),
                        flags,
                        expire_at_ms: -1,
                        group_id,
                    });
                }
            }
            OpCode::Del | OpCode::Exists | OpCode::Touch => {
                let op = if cmd.op == OpCode::Del {
                    TXN_OP_DEL
                } else {
                    TXN_OP_EXISTS
                };
                for key in &cmd.keys {
                    ops.push(TxnOperation {
                        op,
                        key_ptr: key.as_ptr(),
                        key_len: key.len(),
                        val_ptr: std::ptr::null(),
                        val_len: 0,
                        flags: 0,
                        expire_at_ms: -1,
                        group_id: 0,
                    });
                }
            }
            OpCode::Append
            | OpCode::IncrBy
            | OpCode::DecrBy
            | OpCode::IncrByFloat
            | OpCode::Incr
            | OpCode::Decr => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let Some(val) = cmd.val.as_ref() else {
                    spans.push((start, 0));
                    continue;
                };
                let op = match cmd.op {
                    OpCode::Append => TXN_OP_APPEND,
                    OpCode::IncrByFloat => TXN_OP_INCRBYFLOAT,
                    _ => TXN_OP_INCRBY,
                };
                ops.push(TxnOperation {
                    op,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: val.as_ptr(),
                    val_len: val.len(),
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::StrLen => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                ops.push(TxnOperation {
                    op: TXN_OP_STRLEN,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: std::ptr::null(),
                    val_len: 0,
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::SetBit | OpCode::GetBit => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let (val_ptr, val_len) = if let Some(v) = &cmd.val {
                    (v.as_ptr(), v.len())
                } else {
                    (std::ptr::null(), 0)
                };
                ops.push(TxnOperation {
                    op: if cmd.op == OpCode::SetBit {
                        TXN_OP_SETBIT
                    } else {
                        TXN_OP_GETBIT
                    },
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr,
                    val_len,
                    flags: 0,
                    expire_at_ms: cmd.expire_at_ms,
                    group_id: 0,
                });
            }
            OpCode::SetRange | OpCode::GetRange => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let payload = if cmd.op == OpCode::GetRange {
                    Some(pack_bytes_list(&cmd.values))
                } else {
                    None
                };
                if let Some(payload) = payload {
                    payloads.push(payload);
                }
                let (val_ptr, val_len) = if cmd.op == OpCode::GetRange {
                    let payload = payloads.last().unwrap();
                    (payload.as_ptr(), payload.len())
                } else if let Some(v) = &cmd.val {
                    (v.as_ptr(), v.len())
                } else {
                    (std::ptr::null(), 0)
                };
                ops.push(TxnOperation {
                    op: if cmd.op == OpCode::SetRange {
                        TXN_OP_SETRANGE
                    } else {
                        TXN_OP_GETRANGE
                    },
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr,
                    val_len,
                    flags: 0,
                    expire_at_ms: cmd.expire_at_ms,
                    group_id: 0,
                });
            }
            OpCode::Lcs => {
                for key in &cmd.keys {
                    ops.push(TxnOperation {
                        op: TXN_OP_GET,
                        key_ptr: key.as_ptr(),
                        key_len: key.len(),
                        val_ptr: std::ptr::null(),
                        val_len: 0,
                        flags: 0,
                        expire_at_ms: -1,
                        group_id: 0,
                    });
                }
            }
            OpCode::Dump => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                ops.push(TxnOperation {
                    op: TXN_OP_DUMP,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: std::ptr::null(),
                    val_len: 0,
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::Restore => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                match cmd.restore_kind {
                    5 => {
                        // Stream payload: the executor clears the key and
                        // rewrites every stream record the dump carried.
                        let payload = pack_bytes_list(&cmd.values);
                        payloads.push(payload);
                        let payload = payloads.last().unwrap();
                        ops.push(TxnOperation {
                            op: TXN_OP_XRESTORE,
                            key_ptr: key.as_ptr(),
                            key_len: key.len(),
                            val_ptr: payload.as_ptr(),
                            val_len: payload.len(),
                            flags: 0,
                            expire_at_ms: -1,
                            group_id: 0,
                        });
                        if cmd.expire_at_ms > 0 {
                            ops.push(TxnOperation {
                                op: TXN_OP_EXPIRE,
                                key_ptr: key.as_ptr(),
                                key_len: key.len(),
                                val_ptr: std::ptr::null(),
                                val_len: 0,
                                flags: 0,
                                expire_at_ms: cmd.expire_at_ms,
                                group_id: 0,
                            });
                        }
                    }
                    2 => {
                        // String payload: a plain SET carries the TTL itself.
                        let value = cmd.values.first().cloned().unwrap_or_default();
                        payloads.push(value);
                        let value = payloads.last().unwrap();
                        ops.push(TxnOperation {
                            op: TXN_OP_SET,
                            key_ptr: key.as_ptr(),
                            key_len: key.len(),
                            val_ptr: value.as_ptr(),
                            val_len: value.len(),
                            flags: 0,
                            expire_at_ms: cmd.expire_at_ms,
                            group_id: 0,
                        });
                    }
                    3 | 4 => {
                        // Set/zset payload: replace whatever is at the key, then
                        // add the members in the same transaction.
                        ops.push(TxnOperation {
                            op: TXN_OP_DEL,
                            key_ptr: key.as_ptr(),
                            key_len: key.len(),
                            val_ptr: std::ptr::null(),
                            val_len: 0,
                            flags: 0,
                            expire_at_ms: -1,
                            group_id: 0,
                        });
                        let payload = pack_bytes_list(&cmd.values);
                        payloads.push(payload);
                        let payload = payloads.last().unwrap();
                        ops.push(TxnOperation {
                            op: if cmd.restore_kind == 3 {
                                TXN_OP_SADD
                            } else {
                                TXN_OP_ZADD
                            },
                            key_ptr: key.as_ptr(),
                            key_len: key.len(),
                            val_ptr: payload.as_ptr(),
                            val_len: payload.len(),
                            flags: 0,
                            expire_at_ms: -1,
                            group_id: 0,
                        });
                        if cmd.expire_at_ms > 0 {
                            ops.push(TxnOperation {
                                op: TXN_OP_EXPIRE,
                                key_ptr: key.as_ptr(),
                                key_len: key.len(),
                                val_ptr: std::ptr::null(),
                                val_len: 0,
                                flags: 0,
                                expire_at_ms: cmd.expire_at_ms,
                                group_id: 0,
                            });
                        }
                    }
                    _ => {
                        let payload = pack_bytes_list(&cmd.values);
                        payloads.push(payload);
                        let payload = payloads.last().unwrap();
                        ops.push(TxnOperation {
                            op: if cmd.scan_type_matches {
                                TXN_OP_HSET
                            } else {
                                TXN_OP_RESTORE_LIST
                            },
                            key_ptr: key.as_ptr(),
                            key_len: key.len(),
                            val_ptr: payload.as_ptr(),
                            val_len: payload.len(),
                            flags: 0,
                            expire_at_ms: -1,
                            group_id: 0,
                        });
                        if cmd.expire_at_ms > 0 {
                            ops.push(TxnOperation {
                                op: TXN_OP_EXPIRE,
                                key_ptr: key.as_ptr(),
                                key_len: key.len(),
                                val_ptr: std::ptr::null(),
                                val_len: 0,
                                flags: 0,
                                expire_at_ms: cmd.expire_at_ms,
                                group_id: 0,
                            });
                        }
                    }
                }
            }
            OpCode::BLPop | OpCode::BRPop | OpCode::BLMPop | OpCode::LMPop => {
                let payload = pack_bytes_list(&cmd.keys);
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                let mut flags = 0;
                if matches!(cmd.op, OpCode::BLPop)
                    || ((cmd.op == OpCode::BLMPop || cmd.op == OpCode::LMPop)
                        && (cmd.expire_flags & TXN_FLAG_LIST_SOURCE_LEFT) != 0)
                {
                    flags |= TXN_FLAG_LIST_SOURCE_LEFT;
                }
                ops.push(TxnOperation {
                    op: TXN_OP_BPOP,
                    key_ptr: std::ptr::null(),
                    key_len: 0,
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags,
                    expire_at_ms: cmd.set_count.unwrap_or(1),
                    group_id: 0,
                });
            }
            OpCode::Expire | OpCode::PExpire | OpCode::ExpireAt | OpCode::PExpireAt => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                ops.push(TxnOperation {
                    op: TXN_OP_EXPIRE,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: std::ptr::null(),
                    val_len: 0,
                    flags: cmd.expire_flags,
                    expire_at_ms: cmd.expire_at_ms,
                    group_id: 0,
                });
            }
            OpCode::Ttl | OpCode::PTtl | OpCode::ExpireTime | OpCode::PExpireTime => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let flags = if matches!(cmd.op, OpCode::PTtl | OpCode::PExpireTime) {
                    TXN_FLAG_TTL_MILLISECONDS
                } else {
                    0
                };
                ops.push(TxnOperation {
                    op: TXN_OP_TTL,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: std::ptr::null(),
                    val_len: 0,
                    flags,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::Persist => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                ops.push(TxnOperation {
                    op: TXN_OP_PERSIST,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: std::ptr::null(),
                    val_len: 0,
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::GetEx => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                ops.push(TxnOperation {
                    op: TXN_OP_GET,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: std::ptr::null(),
                    val_len: 0,
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
                if cmd.set_keep_ttl {
                    ops.push(TxnOperation {
                        op: TXN_OP_PERSIST,
                        key_ptr: key.as_ptr(),
                        key_len: key.len(),
                        val_ptr: std::ptr::null(),
                        val_len: 0,
                        flags: 0,
                        expire_at_ms: -1,
                        group_id: 0,
                    });
                } else if cmd.expire_at_ms >= 0 {
                    ops.push(TxnOperation {
                        op: TXN_OP_EXPIRE,
                        key_ptr: key.as_ptr(),
                        key_len: key.len(),
                        val_ptr: std::ptr::null(),
                        val_len: 0,
                        flags: 0,
                        expire_at_ms: cmd.expire_at_ms,
                        group_id: 0,
                    });
                }
            }
            OpCode::GetDel => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                ops.push(TxnOperation {
                    op: TXN_OP_GET,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: std::ptr::null(),
                    val_len: 0,
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
                ops.push(TxnOperation {
                    op: TXN_OP_DEL,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: std::ptr::null(),
                    val_len: 0,
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::Keys | OpCode::Scan => {
                let cursor = cmd.keys.first();
                let (key_ptr, key_len) = cursor
                    .map(|c| (c.as_ptr(), c.len()))
                    .unwrap_or((std::ptr::null(), 0));
                ops.push(TxnOperation {
                    op: TXN_OP_SCAN,
                    key_ptr,
                    key_len,
                    val_ptr: cmd.scan_prefix.as_ptr(),
                    val_len: cmd.scan_prefix.len(),
                    flags: 0,
                    expire_at_ms: cmd.scan_count,
                    group_id: 0,
                });
            }
            OpCode::RandomKey => {
                ops.push(TxnOperation {
                    op: TXN_OP_SCAN,
                    key_ptr: std::ptr::null(),
                    key_len: 0,
                    val_ptr: cmd.scan_prefix.as_ptr(),
                    val_len: cmd.scan_prefix.len(),
                    flags: 0,
                    expire_at_ms: 1_000_000,
                    group_id: 0,
                });
            }
            OpCode::DbSize => {
                ops.push(TxnOperation {
                    op: TXN_OP_SCAN,
                    key_ptr: std::ptr::null(),
                    key_len: 0,
                    val_ptr: cmd.scan_prefix.as_ptr(),
                    val_len: cmd.scan_prefix.len(),
                    flags: TXN_FLAG_SCAN_COUNT_ONLY,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::FlushDb | OpCode::FlushAll => {
                ops.push(TxnOperation {
                    op: TXN_OP_FLUSHDB,
                    key_ptr: std::ptr::null(),
                    key_len: 0,
                    val_ptr: cmd.scan_prefix.as_ptr(),
                    val_len: cmd.scan_prefix.len(),
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::BitCount | OpCode::BitPos | OpCode::BitFieldRo => {
                // Read the whole string once via GETRANGE 0 -1 (which enforces the
                // string type check); bit arithmetic happens in Rust.
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let payload = pack_bytes_list(&[Bytes::from_static(b"0"), Bytes::from_static(b"-1")]);
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                ops.push(TxnOperation {
                    op: TXN_OP_GETRANGE,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::BitOp => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let payload = pack_bytes_list(&cmd.values);
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                ops.push(TxnOperation {
                    op: TXN_OP_BITOP,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::BitField => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                // One op for the whole subcommand list: the read-modify-write
                // has to be atomic inside the Mako transaction.
                let payload = pack_bytes_list(&cmd.values);
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                ops.push(TxnOperation {
                    op: TXN_OP_BITFIELD,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::GeoAdd => {
                // Same op as ZADD: cmd.values already holds the geohash score
                // and member for each position, in ZADD's packed order.
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let payload = pack_bytes_list(&cmd.values);
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                ops.push(TxnOperation {
                    op: TXN_OP_ZADD,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags: cmd.expire_flags,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::GeoPos | OpCode::GeoHash | OpCode::GeoDist => {
                // One ZSCORE op per member, all in one request so the reads
                // are consistent. GEODIST always asks for exactly two.
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                for member in &cmd.values {
                    ops.push(TxnOperation {
                        op: TXN_OP_ZSCORE,
                        key_ptr: key.as_ptr(),
                        key_len: key.len(),
                        val_ptr: member.as_ptr(),
                        val_len: member.len(),
                        flags: 0,
                        expire_at_ms: -1,
                        group_id: 0,
                    });
                }
            }
            OpCode::GeoSearch => {
                // One ZRANGEBYSCORE-with-scores op per geohash box (the center
                // cell plus the neighbors that can still intersect the shape),
                // all in one request so every box is read at the same instant.
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let Some(spec) = cmd.geo.as_deref() else {
                    spans.push((start, 0));
                    continue;
                };
                if spec.resolve_error != GeoResolve::Ok || spec.from_member.is_some() {
                    spans.push((start, 0));
                    continue;
                }
                let (range_ops, range_payloads) = geo_range_ops(key, spec);
                payloads.extend(range_payloads);
                ops.extend(range_ops);
            }
            OpCode::GeoSearchStore => {
                // Nothing to build: the read-then-write cannot live in one
                // op list, so this command runs on the interactive session
                // path (`geo_search_store_in_session`) instead.
                spans.push((start, 0));
                continue;
            }
            OpCode::PfAdd | OpCode::PfCount | OpCode::PfMerge => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                // PFADD: elements. PFCOUNT: every key. PFMERGE: the source keys.
                let payload = pack_bytes_list(&cmd.values);
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                ops.push(TxnOperation {
                    op: match cmd.op {
                        OpCode::PfAdd => TXN_OP_HLL_ADD,
                        OpCode::PfCount => TXN_OP_HLL_COUNT,
                        _ => TXN_OP_HLL_MERGE,
                    },
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::Object => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                ops.push(TxnOperation {
                    op: TXN_OP_TYPE,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: std::ptr::null(),
                    val_len: 0,
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::Memory => {
                // MEMORY USAGE: size the serialized value via DUMP.
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                ops.push(TxnOperation {
                    op: TXN_OP_DUMP,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: std::ptr::null(),
                    val_len: 0,
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::XRead | OpCode::XReadGroup => {
                // One op per stream, in the order the command named them, each
                // carrying the read mode its ID spec asks for.
                let group_form = cmd.op == OpCode::XReadGroup;
                for (index, key) in cmd.keys.iter().enumerate() {
                    let spec = cmd
                        .values
                        .get(STREAM_READ_VALUE_PREFIX + index)
                        .cloned()
                        .unwrap_or_default();
                    let payload = if group_form {
                        let (mode, start) = if spec.as_ref() == b">" {
                            (&b"NEW"[..], STREAM_ID_MIN)
                        } else {
                            // An explicit ID replays this consumer's pending
                            // entries after it, so the range starts one past it.
                            match parse_stream_id_generic(spec.as_ref(), 0, true, false)
                                .and_then(|(id, _)| id.incr())
                            {
                                Some(start) => (&b"HISTORY"[..], start),
                                None => (&b"NONE"[..], STREAM_ID_MIN),
                            }
                        };
                        pack_bytes_list(&[
                            cmd.values.first().cloned().unwrap_or_default(),
                            cmd.values.get(1).cloned().unwrap_or_default(),
                            Bytes::from_static(mode),
                            start.text(),
                            Bytes::from(cmd.scan_count.to_string()),
                            cmd.values.get(2).cloned().unwrap_or_default(),
                        ])
                    } else {
                        let (start, end, count, mode) = if spec.as_ref() == b"$" {
                            // Only entries added after the call, so nothing
                            // that is already stored can qualify.
                            (STREAM_ID_MIN, STREAM_ID_MIN, 0, STREAM_READ_NONE)
                        } else if spec.as_ref() == b"+" {
                            // The last entry, whatever it is; COUNT does not
                            // apply.
                            (STREAM_ID_MIN, STREAM_ID_MAX, 1, STREAM_READ_REVERSE)
                        } else {
                            match parse_stream_id_generic(spec.as_ref(), 0, true, false)
                                .and_then(|(id, _)| id.incr())
                            {
                                // XREAD's ID is exclusive, so the range starts
                                // at the ID after it.
                                Some(start) => (
                                    start,
                                    STREAM_ID_MAX,
                                    cmd.scan_count,
                                    STREAM_READ_FORWARD,
                                ),
                                // The client named the largest ID there is:
                                // nothing can ever follow it.
                                None => (STREAM_ID_MIN, STREAM_ID_MIN, 0, STREAM_READ_NONE),
                            }
                        };
                        pack_bytes_list(&[
                            start.text(),
                            end.text(),
                            Bytes::from(count.to_string()),
                            Bytes::from_static(mode),
                        ])
                    };
                    payloads.push(payload);
                    let payload = payloads.last().unwrap();
                    ops.push(TxnOperation {
                        op: if group_form {
                            TXN_OP_XREADGROUP
                        } else {
                            TXN_OP_XRANGE
                        },
                        key_ptr: key.as_ptr(),
                        key_len: key.len(),
                        val_ptr: payload.as_ptr(),
                        val_len: payload.len(),
                        flags: 0,
                        expire_at_ms: -1,
                        group_id: 0,
                    });
                }
            }
            OpCode::XGroup
            | OpCode::XAck
            | OpCode::XPending
            | OpCode::XClaim
            | OpCode::XAutoClaim => {
                if cmd.op == OpCode::XGroup && cmd.restore_kind == STREAM_XGROUP_HELP {
                    spans.push((start, 0));
                    continue;
                }
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let payload = pack_bytes_list(&cmd.values);
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                ops.push(TxnOperation {
                    op: match cmd.op {
                        OpCode::XGroup => TXN_OP_XGROUP,
                        OpCode::XAck => TXN_OP_XACK,
                        OpCode::XPending => TXN_OP_XPENDING,
                        OpCode::XClaim => TXN_OP_XCLAIM,
                        _ => TXN_OP_XAUTOCLAIM,
                    },
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::XAdd
            | OpCode::XRange
            | OpCode::XRevRange
            | OpCode::XLen
            | OpCode::XDel
            | OpCode::XTrim
            | OpCode::XSetId
            | OpCode::XInfo => {
                if cmd.op == OpCode::XInfo && cmd.restore_kind == STREAM_XINFO_HELP {
                    // Answered from the command table, like the other HELP
                    // subcommands: no storage op at all.
                    spans.push((start, 0));
                    continue;
                }
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let payload = pack_bytes_list(&cmd.values);
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                ops.push(TxnOperation {
                    op: match cmd.op {
                        OpCode::XAdd => TXN_OP_XADD,
                        OpCode::XLen => TXN_OP_XLEN,
                        OpCode::XDel => TXN_OP_XDEL,
                        OpCode::XTrim => TXN_OP_XTRIM,
                        OpCode::XSetId => TXN_OP_XSETID,
                        OpCode::XInfo => TXN_OP_XINFO,
                        // XRANGE and XREVRANGE differ only in the read mode
                        // already packed into the payload.
                        _ => TXN_OP_XRANGE,
                    },
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::Type => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                ops.push(TxnOperation {
                    op: TXN_OP_TYPE,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: std::ptr::null(),
                    val_len: 0,
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::HSet
            | OpCode::HSetNx
            | OpCode::HMSet
            | OpCode::HMGet
            | OpCode::HDel
            | OpCode::HIncrBy
            | OpCode::HIncrByFloat => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let payload = pack_bytes_list(&cmd.values);
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                let op_code = match cmd.op {
                    OpCode::HSet | OpCode::HSetNx | OpCode::HMSet => TXN_OP_HSET,
                    OpCode::HMGet => TXN_OP_HMGET,
                    OpCode::HDel => TXN_OP_HDEL,
                    OpCode::HIncrBy => TXN_OP_HINCRBY,
                    OpCode::HIncrByFloat => TXN_OP_HINCRBYFLOAT,
                    _ => TXN_OP_HSET,
                };
                let flags = if cmd.op == OpCode::HSetNx {
                    TXN_FLAG_SET_NX
                } else {
                    0
                };
                ops.push(TxnOperation {
                    op: op_code,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::HExpire
            | OpCode::HPExpire
            | OpCode::HExpireAt
            | OpCode::HPExpireAt
            | OpCode::HTtl
            | OpCode::HPTtl
            | OpCode::HExpireTime
            | OpCode::HPExpireTime
            | OpCode::HPersist => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                // One op for the whole field list so the per-field reads,
                // condition checks and writes stay inside one Mako transaction.
                let op_code = match cmd.op {
                    OpCode::HPersist => TXN_OP_HFIELD_PERSIST,
                    OpCode::HTtl
                    | OpCode::HPTtl
                    | OpCode::HExpireTime
                    | OpCode::HPExpireTime => TXN_OP_HFIELD_TTL,
                    _ => TXN_OP_HFIELD_EXPIRE,
                };
                let payload = pack_bytes_list(&cmd.values);
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                ops.push(TxnOperation {
                    op: op_code,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags: 0,
                    expire_at_ms: cmd.expire_at_ms,
                    group_id: 0,
                });
            }
            OpCode::HGet | OpCode::HExists | OpCode::HStrLen => {
                let (Some(key), Some(value)) = (cmd.keys.first(), cmd.val.as_ref()) else {
                    spans.push((start, 0));
                    continue;
                };
                let op_code = match cmd.op {
                    OpCode::HGet => TXN_OP_HGET,
                    OpCode::HExists => TXN_OP_HEXISTS,
                    OpCode::HStrLen => TXN_OP_HSTRLEN,
                    _ => TXN_OP_HGET,
                };
                ops.push(TxnOperation {
                    op: op_code,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: value.as_ptr(),
                    val_len: value.len(),
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::HGetAll
            | OpCode::HLen
            | OpCode::HKeys
            | OpCode::HVals
            | OpCode::HRandField
            | OpCode::HScan => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let op_code = match cmd.op {
                    OpCode::HGetAll => TXN_OP_HGETALL,
                    OpCode::HLen => TXN_OP_HLEN,
                    OpCode::HKeys => TXN_OP_HKEYS,
                    OpCode::HVals => TXN_OP_HVALS,
                    OpCode::HRandField => TXN_OP_HGETALL,
                    OpCode::HScan => TXN_OP_HSCAN,
                    _ => TXN_OP_HGETALL,
                };
                let mut flags = 0;
                if cmd.set_return_old {
                    flags |= TXN_FLAG_Z_WITHSCORES;
                }
                ops.push(TxnOperation {
                    op: op_code,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: std::ptr::null(),
                    val_len: 0,
                    flags,
                    expire_at_ms: cmd.set_count.unwrap_or(1),
                    group_id: 0,
                });
            }
            OpCode::SAdd | OpCode::SRem => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let payload = pack_bytes_list(&cmd.values);
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                let op = if cmd.op == OpCode::SAdd {
                    TXN_OP_SADD
                } else {
                    TXN_OP_SREM
                };
                ops.push(TxnOperation {
                    op,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::SIsMember => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let Some(member) = cmd.val.as_ref() else {
                    spans.push((start, 0));
                    continue;
                };
                ops.push(TxnOperation {
                    op: TXN_OP_SISMEMBER,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: member.as_ptr(),
                    val_len: member.len(),
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::SMIsMember => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                for member in &cmd.values {
                    ops.push(TxnOperation {
                        op: TXN_OP_SISMEMBER,
                        key_ptr: key.as_ptr(),
                        key_len: key.len(),
                        val_ptr: member.as_ptr(),
                        val_len: member.len(),
                        flags: 0,
                        expire_at_ms: -1,
                        group_id: 0,
                    });
                }
            }
            OpCode::SCard | OpCode::SMembers | OpCode::SScan => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                ops.push(TxnOperation {
                    op: if cmd.op == OpCode::SCard {
                        TXN_OP_SCARD
                    } else {
                        TXN_OP_SMEMBERS
                    },
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: std::ptr::null(),
                    val_len: 0,
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::SMove => {
                let Some(source) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let payload = pack_bytes_list(&cmd.values);
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                ops.push(TxnOperation {
                    op: TXN_OP_SMOVE,
                    key_ptr: source.as_ptr(),
                    key_len: source.len(),
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::SPop | OpCode::SRandMember => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let mut flags = 0;
                let mut count = 1;
                if let Some(raw_count) = cmd.set_count {
                    flags |= TXN_FLAG_SET_COUNT_GIVEN;
                    if raw_count < 0 {
                        flags |= TXN_FLAG_SET_ALLOW_DUPLICATES;
                        count = raw_count.saturating_abs();
                    } else {
                        count = raw_count;
                    }
                }
                ops.push(TxnOperation {
                    op: if cmd.op == OpCode::SPop {
                        TXN_OP_SPOP
                    } else {
                        TXN_OP_SRANDMEMBER
                    },
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: std::ptr::null(),
                    val_len: 0,
                    flags,
                    expire_at_ms: count,
                    group_id: 0,
                });
            }
            OpCode::SInter
            | OpCode::SUnion
            | OpCode::SDiff
            | OpCode::SInterCard
            | OpCode::SInterStore
            | OpCode::SUnionStore
            | OpCode::SDiffStore => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let payload = if matches!(
                    cmd.op,
                    OpCode::SInterStore | OpCode::SUnionStore | OpCode::SDiffStore
                ) {
                    pack_bytes_list(&cmd.keys[1..])
                } else {
                    pack_bytes_list(&cmd.keys)
                };
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                let mut flags = 0;
                if matches!(cmd.op, OpCode::SUnion | OpCode::SUnionStore) {
                    flags |= TXN_FLAG_SET_ALGEBRA_UNION;
                } else if matches!(cmd.op, OpCode::SDiff | OpCode::SDiffStore) {
                    flags |= TXN_FLAG_SET_ALGEBRA_DIFF;
                }
                if matches!(
                    cmd.op,
                    OpCode::SInterStore | OpCode::SUnionStore | OpCode::SDiffStore
                ) {
                    flags |= TXN_FLAG_SET_ALGEBRA_STORE;
                }
                ops.push(TxnOperation {
                    op: TXN_OP_SET_ALGEBRA,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::LPush | OpCode::RPush | OpCode::LPushX | OpCode::RPushX => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let payload = pack_bytes_list(&cmd.values);
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                let mut flags = 0;
                if matches!(cmd.op, OpCode::LPushX | OpCode::RPushX) {
                    flags |= TXN_FLAG_LIST_PUSH_IF_EXISTS;
                }
                ops.push(TxnOperation {
                    op: if matches!(cmd.op, OpCode::LPush | OpCode::LPushX) {
                        TXN_OP_LPUSH
                    } else {
                        TXN_OP_RPUSH
                    },
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::LPop | OpCode::RPop => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let mut flags = 0;
                let mut count = 1;
                if let Some(raw_count) = cmd.set_count {
                    flags |= TXN_FLAG_LIST_COUNT_GIVEN;
                    count = raw_count;
                }
                ops.push(TxnOperation {
                    op: if cmd.op == OpCode::LPop {
                        TXN_OP_LPOP
                    } else {
                        TXN_OP_RPOP
                    },
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: std::ptr::null(),
                    val_len: 0,
                    flags,
                    expire_at_ms: count,
                    group_id: 0,
                });
            }
            OpCode::LLen | OpCode::LIndex | OpCode::LRange | OpCode::LTrim | OpCode::LPos => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let packed_range = if matches!(cmd.op, OpCode::LRange | OpCode::LTrim) {
                    Some(pack_bytes_list(&cmd.values))
                } else if cmd.op == OpCode::LPos {
                    let Some(element) = cmd.val.as_ref() else {
                        spans.push((start, 0));
                        continue;
                    };
                    Some(pack_bytes_list(&[
                        element.clone(),
                        Bytes::from(cmd.expire_at_ms.to_string()),
                        Bytes::from(cmd.set_count.unwrap_or(-1).to_string()),
                        Bytes::from(cmd.scan_count.to_string()),
                    ]))
                } else {
                    None
                };
                if let Some(payload) = packed_range {
                    payloads.push(payload);
                }
                let (val_ptr, val_len) =
                    if matches!(cmd.op, OpCode::LRange | OpCode::LTrim | OpCode::LPos) {
                        let payload = payloads.last().unwrap();
                        (payload.as_ptr(), payload.len())
                    } else {
                        (std::ptr::null(), 0)
                    };
                let op = match cmd.op {
                    OpCode::LLen => TXN_OP_LLEN,
                    OpCode::LIndex => TXN_OP_LINDEX,
                    OpCode::LRange => TXN_OP_LRANGE,
                    OpCode::LTrim => TXN_OP_LTRIM,
                    OpCode::LPos => TXN_OP_LPOS,
                    _ => TXN_OP_LLEN,
                };
                ops.push(TxnOperation {
                    op,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr,
                    val_len,
                    flags: 0,
                    expire_at_ms: cmd.expire_at_ms,
                    group_id: 0,
                });
            }
            OpCode::LSet | OpCode::LRem | OpCode::LInsert => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let payload = pack_bytes_list(&cmd.values);
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                ops.push(TxnOperation {
                    op: match cmd.op {
                        OpCode::LSet => TXN_OP_LSET,
                        OpCode::LRem => TXN_OP_LREM,
                        OpCode::LInsert => TXN_OP_LINSERT,
                        _ => TXN_OP_LSET,
                    },
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags: cmd.expire_flags,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::LMove | OpCode::BLMove | OpCode::RPopLPush | OpCode::BRPopLPush => {
                let Some(source) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let payload = pack_bytes_list(&cmd.values);
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                ops.push(TxnOperation {
                    op: TXN_OP_LMOVE,
                    key_ptr: source.as_ptr(),
                    key_len: source.len(),
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags: cmd.expire_flags,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::ZAdd | OpCode::ZIncrBy => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let payload = pack_bytes_list(&cmd.values);
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                let mut flags = cmd.expire_flags;
                if cmd.op == OpCode::ZIncrBy {
                    flags |= TXN_FLAG_ZADD_INCR;
                }
                ops.push(TxnOperation {
                    op: TXN_OP_ZADD,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::ZScore | OpCode::ZRank | OpCode::ZRevRank => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let Some(member) = cmd.val.as_ref() else {
                    spans.push((start, 0));
                    continue;
                };
                let mut flags = if cmd.op == OpCode::ZRevRank {
                    TXN_FLAG_Z_REV
                } else {
                    0
                };
                if cmd.set_return_old {
                    flags |= TXN_FLAG_Z_WITHSCORES;
                }
                ops.push(TxnOperation {
                    op: if cmd.op == OpCode::ZScore {
                        TXN_OP_ZSCORE
                    } else {
                        TXN_OP_ZRANK
                    },
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: member.as_ptr(),
                    val_len: member.len(),
                    flags,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::ZMScore => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                for member in &cmd.values {
                    ops.push(TxnOperation {
                        op: TXN_OP_ZSCORE,
                        key_ptr: key.as_ptr(),
                        key_len: key.len(),
                        val_ptr: member.as_ptr(),
                        val_len: member.len(),
                        flags: 0,
                        expire_at_ms: -1,
                        group_id: 0,
                    });
                }
            }
            OpCode::ZRem => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let payload = pack_bytes_list(&cmd.values);
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                ops.push(TxnOperation {
                    op: TXN_OP_ZREM,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::ZCard => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                ops.push(TxnOperation {
                    op: TXN_OP_ZCARD,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: std::ptr::null(),
                    val_len: 0,
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::ZRange
            | OpCode::ZRevRange
            | OpCode::ZRangeByScore
            | OpCode::ZRevRangeByScore
            | OpCode::ZRangeByLex
            | OpCode::ZRevRangeByLex => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let payload = pack_bytes_list(&cmd.values);
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                let mut flags = cmd.expire_flags;
                if cmd.op == OpCode::ZRevRange {
                    flags |= TXN_FLAG_Z_REV;
                } else if matches!(cmd.op, OpCode::ZRangeByScore | OpCode::ZRevRangeByScore) {
                    flags |= TXN_FLAG_Z_BYSCORE;
                }
                ops.push(TxnOperation {
                    op: if cmd.expire_at_ms == ZRANGE_MODE_LEX {
                        TXN_OP_ZRANGEBYLEX
                    } else {
                        TXN_OP_ZRANGE
                    },
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::ZLexCount => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let payload = pack_bytes_list(&cmd.values);
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                ops.push(TxnOperation {
                    op: TXN_OP_ZLEXCOUNT,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::ZRemRangeByScore | OpCode::ZRemRangeByRank | OpCode::ZRemRangeByLex => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let payload = pack_bytes_list(&cmd.values);
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                let op = match cmd.op {
                    OpCode::ZRemRangeByScore => TXN_OP_ZREMRANGEBYSCORE,
                    OpCode::ZRemRangeByRank => TXN_OP_ZREMRANGEBYRANK,
                    OpCode::ZRemRangeByLex => TXN_OP_ZREMRANGEBYLEX,
                    _ => TXN_OP_ZREMRANGEBYRANK,
                };
                ops.push(TxnOperation {
                    op,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags: 0,
                    expire_at_ms: cmd.expire_at_ms,
                    group_id: 0,
                });
            }
            OpCode::ZRangeStore => {
                let Some(destination) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let payload = pack_bytes_list(&cmd.values);
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                ops.push(TxnOperation {
                    op: TXN_OP_ZRANGESTORE,
                    key_ptr: destination.as_ptr(),
                    key_len: destination.len(),
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags: cmd.expire_flags,
                    expire_at_ms: cmd.expire_at_ms,
                    group_id: 0,
                });
            }
            OpCode::ZUnion
            | OpCode::ZInter
            | OpCode::ZDiff
            | OpCode::ZUnionStore
            | OpCode::ZInterStore
            | OpCode::ZDiffStore
            | OpCode::ZInterCard => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let payload = pack_bytes_list(&cmd.values);
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                ops.push(TxnOperation {
                    op: TXN_OP_ZSET_ALGEBRA,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags: cmd.expire_flags,
                    expire_at_ms: cmd.expire_at_ms,
                    group_id: 0,
                });
            }
            OpCode::ZCount => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let payload = pack_bytes_list(&cmd.values);
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                ops.push(TxnOperation {
                    op: TXN_OP_ZCOUNT,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags: 0,
                    expire_at_ms: -1,
                    group_id: 0,
                });
            }
            OpCode::ZPopMin | OpCode::ZPopMax => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                let mut flags = 0;
                if cmd.op == OpCode::ZPopMax {
                    flags |= TXN_FLAG_Z_REV;
                }
                if cmd.set_count.is_some() {
                    flags |= TXN_FLAG_Z_COUNT_GIVEN;
                }
                ops.push(TxnOperation {
                    op: TXN_OP_ZPOPMIN,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: std::ptr::null(),
                    val_len: 0,
                    flags,
                    expire_at_ms: cmd.set_count.unwrap_or(1),
                    group_id: 0,
                });
            }
            OpCode::ZMPop | OpCode::BZMPop | OpCode::BZPopMin | OpCode::BZPopMax => {
                let payload = pack_bytes_list(&cmd.keys);
                payloads.push(payload);
                let payload = payloads.last().unwrap();
                ops.push(TxnOperation {
                    op: TXN_OP_ZMPOP,
                    key_ptr: std::ptr::null(),
                    key_len: 0,
                    val_ptr: payload.as_ptr(),
                    val_len: payload.len(),
                    flags: cmd.expire_flags,
                    expire_at_ms: cmd.set_count.unwrap_or(1),
                    group_id: 0,
                });
            }
            OpCode::ZRandMember => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                ops.push(TxnOperation {
                    op: TXN_OP_ZRANDMEMBER,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: std::ptr::null(),
                    val_len: 0,
                    flags: cmd.expire_flags,
                    expire_at_ms: cmd.set_count.unwrap_or(1),
                    group_id: 0,
                });
            }
            OpCode::ZScan => {
                let Some(key) = cmd.keys.first() else {
                    spans.push((start, 0));
                    continue;
                };
                ops.push(TxnOperation {
                    op: TXN_OP_ZSCAN,
                    key_ptr: key.as_ptr(),
                    key_len: key.len(),
                    val_ptr: cmd.scan_prefix.as_ptr(),
                    val_len: cmd.scan_prefix.len(),
                    flags: 0,
                    expire_at_ms: cmd.scan_count,
                    group_id: 0,
                });
            }
            _ => {}
        }
        spans.push((start, ops.len() - start));
    }

    (ops, spans, payloads)
}

fn command_needs_retry(cmd: &Command) -> bool {
    matches!(
        cmd.op,
        OpCode::Get
            | OpCode::Set
            | OpCode::Del
            | OpCode::Exists
            | OpCode::MGet
            | OpCode::SetEx
            | OpCode::PSetEx
            | OpCode::MSet
            | OpCode::MSetNx
            | OpCode::GetSet
            | OpCode::GetEx
            | OpCode::GetDel
            | OpCode::SetNx
            | OpCode::Append
            | OpCode::StrLen
            | OpCode::SetBit
            | OpCode::GetBit
            | OpCode::SetRange
            | OpCode::GetRange
            | OpCode::Lcs
            | OpCode::Dump
            | OpCode::Restore
            | OpCode::Copy
            | OpCode::Move
            | OpCode::Touch
            | OpCode::SortRo
            | OpCode::BitCount
            | OpCode::BitPos
            | OpCode::BitFieldRo
            | OpCode::BitField
            | OpCode::BitOp
            | OpCode::PfAdd
            | OpCode::PfCount
            | OpCode::PfMerge
            | OpCode::Object
            | OpCode::Memory
            | OpCode::BLPop
            | OpCode::BRPop
            | OpCode::BLMPop
            | OpCode::Incr
            | OpCode::IncrBy
            | OpCode::Decr
            | OpCode::DecrBy
            | OpCode::IncrByFloat
            | OpCode::Expire
            | OpCode::PExpire
            | OpCode::ExpireAt
            | OpCode::PExpireAt
            | OpCode::Ttl
            | OpCode::PTtl
            | OpCode::ExpireTime
            | OpCode::PExpireTime
            | OpCode::Persist
            | OpCode::Keys
            | OpCode::Scan
            | OpCode::RandomKey
            | OpCode::DbSize
            | OpCode::FlushDb
            | OpCode::FlushAll
            | OpCode::Type
            | OpCode::HSet
            | OpCode::HSetNx
            | OpCode::HMSet
            | OpCode::HGet
            | OpCode::HMGet
            | OpCode::HGetAll
            | OpCode::HDel
            | OpCode::HExists
            | OpCode::HLen
            | OpCode::HKeys
            | OpCode::HVals
            | OpCode::HStrLen
            | OpCode::HIncrBy
            | OpCode::HIncrByFloat
            | OpCode::HRandField
            | OpCode::HScan
            | OpCode::HExpire
            | OpCode::HPExpire
            | OpCode::HExpireAt
            | OpCode::HPExpireAt
            | OpCode::HTtl
            | OpCode::HPTtl
            | OpCode::HExpireTime
            | OpCode::HPExpireTime
            | OpCode::HPersist
            | OpCode::SAdd
            | OpCode::SMembers
            | OpCode::SIsMember
            | OpCode::SMIsMember
            | OpCode::SRem
            | OpCode::SCard
            | OpCode::SScan
            | OpCode::SMove
            | OpCode::SPop
            | OpCode::SRandMember
            | OpCode::SInter
            | OpCode::SInterCard
            | OpCode::SUnion
            | OpCode::SDiff
            | OpCode::SInterStore
            | OpCode::SUnionStore
            | OpCode::SDiffStore
            | OpCode::LPush
            | OpCode::RPush
            | OpCode::LPop
            | OpCode::RPop
            | OpCode::LLen
            | OpCode::LIndex
            | OpCode::LRange
            | OpCode::LSet
            | OpCode::LRem
            | OpCode::LTrim
            | OpCode::LInsert
            | OpCode::LPushX
            | OpCode::RPushX
            | OpCode::LMove
            | OpCode::BLMove
            | OpCode::LMPop
            | OpCode::RPopLPush
            | OpCode::BRPopLPush
            | OpCode::LPos
            | OpCode::Rename
            | OpCode::RenameNx
            | OpCode::Sort
            | OpCode::ZAdd
            | OpCode::ZScore
            | OpCode::ZMScore
            | OpCode::ZIncrBy
            | OpCode::ZRem
            | OpCode::ZCard
            | OpCode::ZRange
            | OpCode::ZRevRange
            | OpCode::ZRangeByScore
            | OpCode::ZRevRangeByScore
            | OpCode::ZRangeByLex
            | OpCode::ZRevRangeByLex
            | OpCode::ZLexCount
            | OpCode::ZRemRangeByScore
            | OpCode::ZRemRangeByRank
            | OpCode::ZRemRangeByLex
            | OpCode::ZRangeStore
            | OpCode::ZUnion
            | OpCode::ZInter
            | OpCode::ZDiff
            | OpCode::ZUnionStore
            | OpCode::ZInterStore
            | OpCode::ZDiffStore
            | OpCode::ZInterCard
            | OpCode::ZRank
            | OpCode::ZRevRank
            | OpCode::ZCount
            | OpCode::ZPopMin
            | OpCode::ZPopMax
            | OpCode::ZMPop
            | OpCode::BZMPop
            | OpCode::BZPopMin
            | OpCode::BZPopMax
            | OpCode::ZRandMember
            | OpCode::ZScan
            | OpCode::GeoAdd
            | OpCode::GeoPos
            | OpCode::GeoDist
            | OpCode::GeoHash
            | OpCode::GeoSearch
            | OpCode::GeoSearchStore
            | OpCode::XAdd
            | OpCode::XRange
            | OpCode::XRevRange
            | OpCode::XLen
            | OpCode::XDel
            | OpCode::XTrim
            | OpCode::XSetId
            | OpCode::XInfo
            | OpCode::XRead
            | OpCode::XGroup
            | OpCode::XReadGroup
            | OpCode::XAck
            | OpCode::XPending
            | OpCode::XClaim
            | OpCode::XAutoClaim
    )
}

const TXN_MAX_ATTEMPTS: usize = 32;
const SET_RANDOM_COUNT_LIMIT: i64 = 1_000_000;

struct OwnedTxnResponse {
    response: TxnResponse,
    _results: Vec<TxnOpResult>,
    _data: Vec<Vec<u8>>,
}

impl OwnedTxnResponse {
    fn new(mut results: Vec<TxnOpResult>, data: Vec<Vec<u8>>) -> Self {
        let response = TxnResponse {
            transaction_success: true,
            num_results: results.len(),
            results: results.as_mut_ptr(),
        };
        OwnedTxnResponse {
            response,
            _results: results,
            _data: data,
        }
    }

    fn as_response(&self) -> &TxnResponse {
        &self.response
    }
}

fn memory_store() -> &'static Mutex<HashMap<Bytes, MemoryEntry>> {
    MEMORY_STORE.get_or_init(|| Mutex::new(HashMap::new()))
}

fn ptr_slice<'a>(ptr: *const u8, len: usize) -> Option<&'a [u8]> {
    if len == 0 {
        Some(&[])
    } else if ptr.is_null() {
        None
    } else {
        Some(unsafe { std::slice::from_raw_parts(ptr, len) })
    }
}

fn memory_entry_live(entry: &MemoryEntry, now_ms: i64) -> bool {
    entry
        .expire_at_ms
        .map(|deadline| deadline > now_ms)
        .unwrap_or(true)
}

fn memory_get_live(
    store: &mut HashMap<Bytes, MemoryEntry>,
    key: &[u8],
    now_ms: i64,
) -> Option<Vec<u8>> {
    let key_bytes = Bytes::copy_from_slice(key);
    match store.get(&key_bytes) {
        Some(entry) if memory_entry_live(entry, now_ms) => Some(entry.value.clone()),
        Some(_) => {
            store.remove(&key_bytes);
            None
        }
        None => None,
    }
}

fn memory_exists_live(store: &mut HashMap<Bytes, MemoryEntry>, key: &[u8], now_ms: i64) -> bool {
    let key_bytes = Bytes::copy_from_slice(key);
    match store.get(&key_bytes) {
        Some(entry) if memory_entry_live(entry, now_ms) => true,
        Some(_) => {
            store.remove(&key_bytes);
            false
        }
        None => false,
    }
}

fn push_memory_result(
    results: &mut Vec<TxnOpResult>,
    data: &mut Vec<Vec<u8>>,
    success: bool,
    value: Option<Vec<u8>>,
    int_value: i64,
) {
    let (value_present, data_ptr, data_len) = if let Some(value) = value {
        data.push(value);
        let stored = data.last_mut().unwrap();
        (true, stored.as_mut_ptr(), stored.len())
    } else {
        (false, std::ptr::null_mut(), 0)
    };

    results.push(TxnOpResult {
        success,
        value_present,
        data_ptr,
        data_len,
        int_value,
    });
}

fn memory_expire_at(expire_at_ms: i64) -> Option<i64> {
    if expire_at_ms >= 0 {
        Some(expire_at_ms)
    } else {
        None
    }
}

fn memory_execute_transaction(ops: &[TxnOperation]) -> OwnedTxnResponse {
    let mut results = Vec::with_capacity(ops.len());
    let mut data = Vec::new();
    let mut store = memory_store().lock().unwrap();
    let now_ms = unix_time_ms();
    let mut absent_groups: HashMap<u32, bool> = HashMap::new();

    for op in ops {
        if op.op != TXN_OP_SET
            || op.group_id == 0
            || (op.flags & TXN_FLAG_SET_REQUIRE_ABSENT_GROUP) == 0
        {
            continue;
        }
        let Some(key) = ptr_slice(op.key_ptr, op.key_len) else {
            absent_groups.insert(op.group_id, false);
            continue;
        };
        let exists = memory_exists_live(&mut store, key, now_ms);
        let entry = absent_groups.entry(op.group_id).or_insert(true);
        *entry &= !exists;
    }

    for op in ops {
        match op.op {
            TXN_OP_GET => {
                let Some(key) = ptr_slice(op.key_ptr, op.key_len) else {
                    push_memory_result(&mut results, &mut data, false, None, 0);
                    continue;
                };
                let value = memory_get_live(&mut store, key, now_ms);
                push_memory_result(&mut results, &mut data, true, value, 0);
            }
            TXN_OP_SET => {
                let Some(key) = ptr_slice(op.key_ptr, op.key_len) else {
                    push_memory_result(&mut results, &mut data, false, None, 0);
                    continue;
                };
                let Some(value) = ptr_slice(op.val_ptr, op.val_len) else {
                    push_memory_result(&mut results, &mut data, false, None, 0);
                    continue;
                };
                let key_bytes = Bytes::copy_from_slice(key);
                let needs_old_value = (op.flags
                    & (TXN_FLAG_SET_NX
                        | TXN_FLAG_SET_XX
                        | TXN_FLAG_SET_RETURN_OLD
                        | TXN_FLAG_SET_KEEP_TTL))
                    != 0
                    || (op.group_id != 0 && (op.flags & TXN_FLAG_SET_REQUIRE_ABSENT_GROUP) != 0);

                if !needs_old_value {
                    store.insert(
                        key_bytes,
                        MemoryEntry {
                            value: value.to_vec(),
                            expire_at_ms: memory_expire_at(op.expire_at_ms),
                        },
                    );
                    push_memory_result(&mut results, &mut data, true, Some(Vec::new()), 1);
                    continue;
                }

                let old_value = memory_get_live(&mut store, key, now_ms);
                let group_ok =
                    if op.group_id != 0 && (op.flags & TXN_FLAG_SET_REQUIRE_ABSENT_GROUP) != 0 {
                        absent_groups.get(&op.group_id).copied().unwrap_or(false)
                    } else {
                        true
                    };
                let condition_ok = group_ok
                    && if (op.flags & TXN_FLAG_SET_NX) != 0 {
                        old_value.is_none()
                    } else if (op.flags & TXN_FLAG_SET_XX) != 0 {
                        old_value.is_some()
                    } else {
                        true
                    };

                if condition_ok {
                    let expire_at_ms = if (op.flags & TXN_FLAG_SET_KEEP_TTL) != 0 {
                        store.get(&key_bytes).and_then(|entry| entry.expire_at_ms)
                    } else {
                        memory_expire_at(op.expire_at_ms)
                    };
                    store.insert(
                        key_bytes,
                        MemoryEntry {
                            value: value.to_vec(),
                            expire_at_ms,
                        },
                    );
                }

                if (op.flags & TXN_FLAG_SET_RETURN_OLD) != 0 {
                    push_memory_result(&mut results, &mut data, true, old_value, 0);
                } else {
                    push_memory_result(
                        &mut results,
                        &mut data,
                        true,
                        condition_ok.then(Vec::new),
                        if condition_ok { 1 } else { 0 },
                    );
                }
            }
            TXN_OP_DEL => {
                let Some(key) = ptr_slice(op.key_ptr, op.key_len) else {
                    push_memory_result(&mut results, &mut data, false, None, 0);
                    continue;
                };
                let existed = memory_exists_live(&mut store, key, now_ms);
                if existed {
                    store.remove(&Bytes::copy_from_slice(key));
                }
                push_memory_result(
                    &mut results,
                    &mut data,
                    true,
                    existed.then(Vec::new),
                    if existed { 1 } else { 0 },
                );
            }
            TXN_OP_EXISTS => {
                let Some(key) = ptr_slice(op.key_ptr, op.key_len) else {
                    push_memory_result(&mut results, &mut data, false, None, 0);
                    continue;
                };
                let exists = memory_exists_live(&mut store, key, now_ms);
                push_memory_result(
                    &mut results,
                    &mut data,
                    true,
                    exists.then(Vec::new),
                    if exists { 1 } else { 0 },
                );
            }
            TXN_OP_APPEND => {
                let Some(key) = ptr_slice(op.key_ptr, op.key_len) else {
                    push_memory_result(&mut results, &mut data, false, None, 0);
                    continue;
                };
                let Some(value) = ptr_slice(op.val_ptr, op.val_len) else {
                    push_memory_result(&mut results, &mut data, false, None, 0);
                    continue;
                };
                let mut current = memory_get_live(&mut store, key, now_ms).unwrap_or_default();
                current.extend_from_slice(value);
                let len = current.len() as i64;
                store.insert(
                    Bytes::copy_from_slice(key),
                    MemoryEntry {
                        value: current,
                        expire_at_ms: None,
                    },
                );
                push_memory_result(&mut results, &mut data, true, None, len);
            }
            TXN_OP_STRLEN => {
                let Some(key) = ptr_slice(op.key_ptr, op.key_len) else {
                    push_memory_result(&mut results, &mut data, false, None, 0);
                    continue;
                };
                let len = memory_get_live(&mut store, key, now_ms)
                    .map(|value| value.len() as i64)
                    .unwrap_or(0);
                push_memory_result(&mut results, &mut data, true, None, len);
            }
            TXN_OP_INCRBY => {
                let Some(key) = ptr_slice(op.key_ptr, op.key_len) else {
                    push_memory_result(&mut results, &mut data, false, None, 0);
                    continue;
                };
                let Some(delta_bytes) = ptr_slice(op.val_ptr, op.val_len) else {
                    push_memory_result(&mut results, &mut data, false, None, 0);
                    continue;
                };
                let delta = std::str::from_utf8(delta_bytes)
                    .ok()
                    .and_then(|text| text.parse::<i64>().ok());
                let stored = memory_get_live(&mut store, key, now_ms)
                    .map(|value| String::from_utf8(value).ok())
                    .flatten();
                let current = match stored {
                    None => Some(0),
                    Some(text) => text.parse::<i64>().ok(),
                };
                // Same TXN_INCR_ERR_* sentinels the C++ executor reports, so
                // the memory backend produces the same replies.
                let (Some(current), Some(delta)) = (current, delta) else {
                    push_memory_result(
                        &mut results,
                        &mut data,
                        false,
                        None,
                        INCR_ERR_NOT_INTEGER,
                    );
                    continue;
                };
                let Some(next) = current.checked_add(delta) else {
                    push_memory_result(&mut results, &mut data, false, None, INCR_ERR_OVERFLOW);
                    continue;
                };
                store.insert(
                    Bytes::copy_from_slice(key),
                    MemoryEntry {
                        value: next.to_string().into_bytes(),
                        expire_at_ms: None,
                    },
                );
                push_memory_result(&mut results, &mut data, true, None, next);
            }
            TXN_OP_EXPIRE => {
                let Some(key) = ptr_slice(op.key_ptr, op.key_len) else {
                    push_memory_result(&mut results, &mut data, false, None, 0);
                    continue;
                };
                let key_bytes = Bytes::copy_from_slice(key);
                let exists = memory_exists_live(&mut store, key, now_ms);
                if exists {
                    if op.expire_at_ms <= now_ms {
                        store.remove(&key_bytes);
                    } else if let Some(entry) = store.get_mut(&key_bytes) {
                        entry.expire_at_ms = Some(op.expire_at_ms);
                    }
                }
                push_memory_result(
                    &mut results,
                    &mut data,
                    true,
                    None,
                    if exists { 1 } else { 0 },
                );
            }
            TXN_OP_TTL => {
                let Some(key) = ptr_slice(op.key_ptr, op.key_len) else {
                    push_memory_result(&mut results, &mut data, false, None, 0);
                    continue;
                };
                let key_bytes = Bytes::copy_from_slice(key);
                let ttl = if !memory_exists_live(&mut store, key, now_ms) {
                    -2
                } else if let Some(expire_at_ms) =
                    store.get(&key_bytes).and_then(|entry| entry.expire_at_ms)
                {
                    let remaining_ms = expire_at_ms.saturating_sub(now_ms);
                    if (op.flags & TXN_FLAG_TTL_MILLISECONDS) != 0 {
                        remaining_ms
                    } else {
                        (remaining_ms + 999) / 1000
                    }
                } else {
                    -1
                };
                push_memory_result(&mut results, &mut data, true, None, ttl);
            }
            TXN_OP_PERSIST => {
                let Some(key) = ptr_slice(op.key_ptr, op.key_len) else {
                    push_memory_result(&mut results, &mut data, false, None, 0);
                    continue;
                };
                let key_bytes = Bytes::copy_from_slice(key);
                let existed = memory_exists_live(&mut store, key, now_ms);
                let changed = if existed {
                    if let Some(entry) = store.get_mut(&key_bytes) {
                        let had_ttl = entry.expire_at_ms.is_some();
                        entry.expire_at_ms = None;
                        had_ttl
                    } else {
                        false
                    }
                } else {
                    false
                };
                push_memory_result(
                    &mut results,
                    &mut data,
                    true,
                    None,
                    if changed { 1 } else { 0 },
                );
            }
            TXN_OP_FLUSHDB => {
                store.clear();
                push_memory_result(&mut results, &mut data, true, None, 0);
            }
            TXN_OP_TYPE => {
                let Some(key) = ptr_slice(op.key_ptr, op.key_len) else {
                    push_memory_result(&mut results, &mut data, false, None, 0);
                    continue;
                };
                let kind = if memory_exists_live(&mut store, key, now_ms) {
                    1
                } else {
                    0
                };
                push_memory_result(&mut results, &mut data, true, None, kind);
            }
            _ => {
                push_memory_result(&mut results, &mut data, false, None, 0);
            }
        }
    }

    OwnedTxnResponse::new(results, data)
}

fn key_exists_now(key: &Bytes) -> bool {
    if redis_backend() == RedisBackend::Memory {
        let mut store = memory_store().lock().unwrap();
        return memory_exists_live(&mut store, key, unix_time_ms());
    }

    let op = TxnOperation {
        op: TXN_OP_EXISTS,
        key_ptr: key.as_ptr(),
        key_len: key.len(),
        val_ptr: std::ptr::null(),
        val_len: 0,
        flags: 0,
        expire_at_ms: -1,
        group_id: 0,
    };
    let request = TxnRequest {
        num_ops: 1,
        ops: std::ptr::addr_of!(op),
    };
    let mut response = TxnResponse {
        transaction_success: false,
        num_results: 0,
        results: std::ptr::null_mut(),
    };
    let call_ok = unsafe { cpp_execute_transaction(&request, &mut response) };
    let exists = if call_ok && response.num_results == 1 && !response.results.is_null() {
        let result = unsafe { &*response.results };
        result.success && result.value_present
    } else {
        false
    };
    if !response.results.is_null() {
        unsafe { cpp_free_transaction_response(&mut response) };
    }
    exists
}

fn sleep_for_retry(attempt: usize) {
    let delay_ms = match attempt {
        0 => 1,
        1 => 2,
        _ => 4,
    };
    std::thread::sleep(std::time::Duration::from_millis(delay_ms));
}

fn fast_mako_max_attempts(op: OpCode) -> usize {
    match op {
        OpCode::Get | OpCode::Set => TXN_MAX_ATTEMPTS,
        _ => 1,
    }
}

fn execute_fast_mako_string_op<W: Write>(
    op: OpCode,
    key: &[u8],
    value: Option<&[u8]>,
    protocol_version: u8,
    writer: &mut W,
) -> std::io::Result<bool> {
    let (txn_op, val_ptr, val_len) = match op {
        OpCode::Get => (TXN_OP_GET, std::ptr::null(), 0),
        OpCode::Set => {
            let Some(value) = value else {
                write_err(writer, "operation failed")?;
                return Ok(false);
            };
            (TXN_OP_SET, value.as_ptr(), value.len())
        }
        _ => return Ok(false),
    };
    let max_attempts = fast_mako_max_attempts(op);

    let mut fast_result = FastMakoStringResult {
        status: FAST_MAKO_ABORTED,
        data_ptr: std::ptr::null(),
        data_len: 0,
    };
    let mut fast_call_ok = false;
    for attempt in 0..max_attempts {
        fast_result = FastMakoStringResult {
            status: FAST_MAKO_ABORTED,
            data_ptr: std::ptr::null(),
            data_len: 0,
        };
        fast_call_ok = unsafe {
            cpp_execute_fast_mako_string(
                txn_op,
                key.as_ptr(),
                key.len(),
                val_ptr,
                val_len,
                &mut fast_result,
            )
        };
        if fast_call_ok && fast_result.status != FAST_MAKO_ABORTED {
            break;
        }
        if attempt + 1 < max_attempts {
            unsafe { cpp_record_txn_retry() };
            sleep_for_retry(attempt);
        }
    }

    if !fast_call_ok || fast_result.status == FAST_MAKO_ABORTED {
        write_err(writer, "backend")?;
        return Ok(false);
    }

    match (op, fast_result.status) {
        (OpCode::Get, FAST_MAKO_GET_MISS) => {
            write_null(writer, protocol_version)?;
            return Ok(true);
        }
        (OpCode::Get, FAST_MAKO_GET_HIT) => {
            if fast_result.data_len != 0 {
                if fast_result.data_ptr.is_null() {
                    write_err(writer, "backend")?;
                    return Ok(false);
                }
                let data = unsafe {
                    std::slice::from_raw_parts(fast_result.data_ptr, fast_result.data_len)
                };
                write_bulk(writer, data)?;
            } else {
                write_bulk(writer, b"")?;
            }
            return Ok(true);
        }
        (OpCode::Set, FAST_MAKO_SET_OK) => {
            write_simple_ok(writer)?;
            return Ok(true);
        }
        (_, FAST_MAKO_FALLBACK) => {}
        _ => {
            write_err(writer, "backend")?;
            return Ok(false);
        }
    }

    let operation = TxnOperation {
        op: txn_op,
        key_ptr: key.as_ptr(),
        key_len: key.len(),
        val_ptr,
        val_len,
        flags: 0,
        expire_at_ms: -1,
        group_id: 0,
    };
    let request = TxnRequest {
        num_ops: 1,
        ops: std::ptr::addr_of!(operation),
    };
    let mut response = TxnResponse {
        transaction_success: false,
        num_results: 0,
        results: std::ptr::null_mut(),
    };
    let mut call_ok = false;

    for attempt in 0..max_attempts {
        response = TxnResponse {
            transaction_success: false,
            num_results: 0,
            results: std::ptr::null_mut(),
        };
        call_ok = unsafe { cpp_execute_transaction(&request, &mut response) };
        if call_ok && response.transaction_success && response.num_results == 1 {
            break;
        }
        unsafe { cpp_free_transaction_response(&mut response) };
        if attempt + 1 < max_attempts {
            unsafe { cpp_record_txn_retry() };
            sleep_for_retry(attempt);
        }
    }

    if !call_ok || !response.transaction_success || response.num_results != 1 {
        unsafe { cpp_free_transaction_response(&mut response) };
        write_err(writer, "backend")?;
        return Ok(false);
    }

    let result = unsafe { &*response.results };
    let mut success = result.success;
    match op {
        OpCode::Get => {
            if !result.success {
                write_err(writer, "operation failed")?;
            } else if result.value_present {
                if result.data_len > 0 {
                    if result.data_ptr.is_null() {
                        success = false;
                        write_err(writer, "operation failed")?;
                    } else {
                        let data =
                            unsafe { std::slice::from_raw_parts(result.data_ptr, result.data_len) };
                        write_bulk(writer, data)?;
                    }
                } else {
                    write_bulk(writer, b"")?;
                }
            } else {
                write_null(writer, protocol_version)?;
            }
        }
        OpCode::Set => {
            if result.success {
                write_simple_ok(writer)?;
            } else {
                write_err(writer, "operation failed")?;
            }
        }
        _ => {}
    }

    unsafe { cpp_free_transaction_response(&mut response) };
    Ok(success)
}

fn try_fast_mako_string_command<W: Write>(
    cmd: &Command,
    protocol_version: u8,
    writer: &mut W,
) -> std::io::Result<bool> {
    if redis_backend() != RedisBackend::Mako {
        return Ok(false);
    }

    let Some(key) = cmd.keys.first() else {
        return Ok(false);
    };
    let value = match cmd.op {
        OpCode::Get => None,
        OpCode::Set
            if cmd.set_condition == SetCondition::None
                && !cmd.set_return_old
                && !cmd.set_integer_reply
                && !cmd.set_keep_ttl
                && cmd.expire_at_ms < 0 =>
        {
            let Some(value) = cmd.val.as_ref() else {
                return Ok(false);
            };
            Some(value.as_ref())
        }
        _ => return Ok(false),
    };
    let _ = execute_fast_mako_string_op(cmd.op, key, value, protocol_version, writer)?;
    Ok(true)
}

/// Execute a single command as a transaction (for non-MULTI operations)
/// Returns the result directly without array wrapper
fn ffi_execute_single<W: Write>(
    cmd: &Command,
    protocol_version: u8,
    writer: &mut W,
) -> std::io::Result<()> {
    if try_fast_mako_string_command(cmd, protocol_version, writer)? {
        return Ok(());
    }

    let single = [cmd.clone()];
    let (ops, spans, _payloads) = build_txn_ops(&single);

    if ops.is_empty() {
        write_command_result(cmd, None, spans[0], protocol_version, writer)?;
        return Ok(());
    }

    if redis_backend() == RedisBackend::Memory {
        let response = memory_execute_transaction(&ops);
        write_command_result(
            cmd,
            Some(response.as_response()),
            spans[0],
            protocol_version,
            writer,
        )?;
        return Ok(());
    }

    let request = TxnRequest {
        num_ops: ops.len(),
        ops: ops.as_ptr(),
    };

    let max_attempts = if command_needs_retry(cmd) {
        TXN_MAX_ATTEMPTS
    } else {
        1
    };
    let mut response = TxnResponse {
        transaction_success: false,
        num_results: 0,
        results: std::ptr::null_mut(),
    };
    let mut call_ok = false;

    for attempt in 0..max_attempts {
        response = TxnResponse {
            transaction_success: false,
            num_results: 0,
            results: std::ptr::null_mut(),
        };
        call_ok = unsafe { cpp_execute_transaction(&request, &mut response) };
        if call_ok && response.transaction_success && response.num_results >= ops.len() {
            break;
        }
        unsafe { cpp_free_transaction_response(&mut response) };
        if attempt + 1 < max_attempts {
            unsafe { cpp_record_txn_retry() };
            sleep_for_retry(attempt);
        }
    }

    if !call_ok || !response.transaction_success || response.num_results < ops.len() {
        unsafe { cpp_free_transaction_response(&mut response) };
        write_err(writer, "backend")?;
        return Ok(());
    }

    write_command_result(cmd, Some(&response), spans[0], protocol_version, writer)?;

    unsafe { cpp_free_transaction_response(&mut response) };
    Ok(())
}

/// EXEC when the queue holds a GEOSEARCHSTORE. The batch path builds one op
/// list for the whole transaction up front, which that command cannot join, so
/// the whole queue runs command by command inside one open session and commits
/// once. The session formats each reply exactly as the batch path would, and
/// `ffi_run_session_as` re-runs the whole queue on an optimistic-concurrency
/// abort, so EXEC stays all-or-nothing.
fn ffi_execute_transaction_session<W: Write>(
    commands: &[Command],
    protocol_version: u8,
    writer: &mut W,
) -> std::io::Result<()> {
    if commands.is_empty() {
        return write_array_header(writer, 0);
    }
    // The stripes the session holds: every command's keys, plus the source a
    // GEOSEARCHSTORE reads, which lives in `values` rather than `keys`.
    let mut keys: Vec<Bytes> = Vec::new();
    for command in commands {
        keys.extend(command.keys.iter().cloned());
        if command.op == OpCode::GeoSearchStore {
            keys.extend(command.values.iter().cloned());
        }
    }
    let replies = ffi_run_session_as(&keys, protocol_version, |session| {
        let mut replies = Vec::with_capacity(commands.len());
        for command in commands {
            match session.execute(command) {
                Ok(reply) => replies.push(reply),
                Err(()) => return SessionRun::Retry,
            }
        }
        SessionRun::Commit(replies)
    });
    match replies {
        Some(replies) => {
            write_array_header(writer, replies.len())?;
            for reply in replies {
                writer.write_all(&reply)?;
            }
            Ok(())
        }
        // A failed transaction answers a nil array, as the batch path does.
        None => writer.write_all(b"*-1\r\n"),
    }
}

/// Execute buffered commands as a single transaction (for MULTI/EXEC)
/// Returns results wrapped in an array
fn ffi_execute_transaction<W: Write>(
    commands: &[Command],
    protocol_version: u8,
    writer: &mut W,
) -> std::io::Result<()> {
    if commands.is_empty() {
        // Empty transaction returns empty array
        write_array_header(writer, 0)?;
        return Ok(());
    }

    let (ops, spans, _payloads) = build_txn_ops(commands);

    if ops.is_empty() {
        write_array_header(writer, commands.len())?;
        for (cmd, span) in commands.iter().zip(spans.iter().copied()) {
            write_command_result(cmd, None, span, protocol_version, writer)?;
        }
        return Ok(());
    }

    if redis_backend() == RedisBackend::Memory {
        let response = memory_execute_transaction(&ops);
        write_array_header(writer, commands.len())?;
        for (cmd, span) in commands.iter().zip(spans.iter().copied()) {
            write_command_result(
                cmd,
                Some(response.as_response()),
                span,
                protocol_version,
                writer,
            )?;
        }
        return Ok(());
    }

    let request = TxnRequest {
        num_ops: ops.len(),
        ops: ops.as_ptr(),
    };

    let max_attempts = if commands.iter().any(command_needs_retry) {
        TXN_MAX_ATTEMPTS
    } else {
        1
    };
    let mut response = TxnResponse {
        transaction_success: false,
        num_results: 0,
        results: std::ptr::null_mut(),
    };
    let mut call_ok = false;

    for attempt in 0..max_attempts {
        response = TxnResponse {
            transaction_success: false,
            num_results: 0,
            results: std::ptr::null_mut(),
        };
        call_ok = unsafe { cpp_execute_transaction(&request, &mut response) };
        if call_ok && response.transaction_success && response.num_results >= ops.len() {
            break;
        }
        unsafe { cpp_free_transaction_response(&mut response) };
        if attempt + 1 < max_attempts {
            unsafe { cpp_record_txn_retry() };
            sleep_for_retry(attempt);
        }
    }

    if !call_ok || !response.transaction_success || response.num_results < ops.len() {
        // Transaction failed - return nil (EXECABORT equivalent)
        unsafe { cpp_free_transaction_response(&mut response) };
        writer.write_all(b"*-1\r\n")?;
        return Ok(());
    }

    // Write one Redis array item per queued command, not per expanded FFI op.
    write_array_header(writer, commands.len())?;

    for (cmd, span) in commands.iter().zip(spans.iter().copied()) {
        write_command_result(cmd, Some(&response), span, protocol_version, writer)?;
    }

    // Free response resources
    unsafe { cpp_free_transaction_response(&mut response) };

    Ok(())
}

// ===== Interactive transaction sessions =====
//
// `cpp_execute_transaction` takes a whole op list at once, so nothing in it can
// depend on the result of an earlier op. A session keeps one Mako transaction
// open across many calls instead: `SessionTxn::execute` turns one `Command`
// into a `TxnRequest`, runs it inside the open transaction and formats the
// reply with `write_command_result`, so any storage command can run inside a
// session with exactly the bytes the single-command path would have produced.
// Everything the executor buffers (deferred deletes, buffered writes, staged
// collections) stays in the session, which is what gives a Lua script
// read-your-writes across its `redis.call`s.
//
// Memory backend: `MAKO_REDIS_BACKEND=memory` has no transactions at all, so a
// session there runs each op straight through `memory_execute_transaction`.
// Writes land immediately, commit and abort do nothing, and a failed script
// leaves its partial writes behind. That backend exists for parser unit tests,
// which do not exercise scripting; the functional suites all run on Mako.

/// What the body of `ffi_run_session` decided to do with the session.
enum SessionRun<T> {
    /// Finished successfully: commit, and if the commit succeeds hand this
    /// value back.
    Commit(T),
    /// Finished with a result that must not be committed (a script error).
    /// Abort and hand the value back without retrying.
    Abort(T),
    /// The session broke (storage error, STO abort, or a write refused on a
    /// follower). Abort and run the body again in a fresh session.
    Retry,
}

/// One open `cpp_txn_begin` session.
struct SessionTxn {
    /// The C++ session handle, or null on the memory backend.
    handle: *mut std::ffi::c_void,
    memory: bool,
    finished: bool,
    /// At least one write command has run inside the session. SCRIPT KILL uses
    /// this: a script that has already written is UNKILLABLE.
    dirty: bool,
    /// Keys a write command named, so WATCH invalidation can be replayed once
    /// the session commits.
    written_keys: Vec<Bytes>,
    /// PUBLISH calls made inside the session. Redis makes a script's effects
    /// visible only once it has run, so the messages are delivered after the
    /// commit and never at all if the session aborts.
    pending_publishes: Vec<(Bytes, Bytes)>,
    /// RESP version the replies are formatted for. A script always sees RESP2,
    /// so only an EXEC running on this path passes anything else.
    protocol_version: u8,
}

impl SessionTxn {
    /// Opens a session that holds the key-stripe locks of `keys`. Keys the
    /// session touches without declaring them here are still correct, but they
    /// are serialized by STO's optimistic concurrency and a conflict shows up
    /// as a failed commit instead of a wait.
    fn begin(keys: &[Bytes], protocol_version: u8) -> Option<SessionTxn> {
        if redis_backend() == RedisBackend::Memory {
            return Some(SessionTxn {
                handle: std::ptr::null_mut(),
                memory: true,
                finished: false,
                dirty: false,
                written_keys: Vec::new(),
                pending_publishes: Vec::new(),
                protocol_version,
            });
        }
        let key_ptrs: Vec<*const u8> = keys.iter().map(|key| key.as_ptr()).collect();
        let key_lens: Vec<usize> = keys.iter().map(|key| key.len()).collect();
        let handle = unsafe { cpp_txn_begin(key_ptrs.as_ptr(), key_lens.as_ptr(), keys.len()) };
        if handle.is_null() {
            return None;
        }
        Some(SessionTxn {
            handle,
            memory: false,
            finished: false,
            dirty: false,
            written_keys: Vec::new(),
            pending_publishes: Vec::new(),
            protocol_version,
        })
    }

    /// Runs a raw op list inside the session and hands the results to
    /// `consume`. This is the one way into the session that is not a whole
    /// `Command`: GEOSEARCHSTORE needs the range reads themselves, not their
    /// RESP formatting, before it can decide what to store.
    fn run_ops<T>(
        &mut self,
        ops: &[TxnOperation],
        consume: impl FnOnce(&[TxnOpResult]) -> T,
    ) -> Result<T, ()> {
        if ops.is_empty() {
            return Ok(consume(&[]));
        }
        if self.memory {
            let owned = memory_execute_transaction(ops);
            let response = owned.as_response();
            if response.num_results < ops.len() {
                return Err(());
            }
            return Ok(consume(unsafe {
                std::slice::from_raw_parts(response.results, ops.len())
            }));
        }
        let request = TxnRequest {
            num_ops: ops.len(),
            ops: ops.as_ptr(),
        };
        let mut response = TxnResponse {
            transaction_success: false,
            num_results: 0,
            results: std::ptr::null_mut(),
        };
        let call_ok = unsafe { cpp_txn_execute(self.handle, &request, &mut response) };
        if !call_ok || response.num_results < ops.len() {
            unsafe { cpp_free_transaction_response(&mut response) };
            return Err(());
        }
        let value = consume(unsafe { std::slice::from_raw_parts(response.results, ops.len()) });
        unsafe { cpp_free_transaction_response(&mut response) };
        Ok(value)
    }

    /// Runs one command inside the session and returns its RESP2 reply.
    /// `Err(())` means the session broke and the caller must abort and retry.
    fn execute(&mut self, cmd: &Command) -> Result<Vec<u8>, ()> {
        let single = [cmd.clone()];
        let (ops, spans, _payloads) = build_txn_ops(&single);
        let mut reply = Vec::new();

        if is_dirty_command(cmd.op) {
            self.dirty = true;
            for key in &cmd.keys {
                self.written_keys.push(key.clone());
            }
        }

        // GEOSEARCHSTORE's write depends on what its own reads returned, which
        // is exactly what a session is for; it answers for itself.
        if cmd.op == OpCode::GeoSearchStore {
            return geo_search_store_in_session(self, cmd);
        }

        // PUBLISH is not a storage op: buffer it so a script's messages are
        // delivered only if the script's writes become visible. The reply is
        // the number of subscribers at the time of the call.
        if cmd.op == OpCode::Publish {
            let channel = cmd.keys.first().cloned().unwrap_or_default();
            let message = cmd.val.clone().unwrap_or_default();
            let receivers = count_pubsub_receivers(&channel);
            self.pending_publishes.push((channel, message));
            write_integer(&mut reply, receivers as i64).map_err(|_| ())?;
            return Ok(reply);
        }

        if ops.is_empty() {
            // PING, TIME and WAIT answer from write_command_result itself.
            write_command_result(cmd, None, spans[0], self.protocol_version, &mut reply)
                .map_err(|_| ())?;
            return Ok(reply);
        }

        if self.memory {
            let response = memory_execute_transaction(&ops);
            write_command_result(
                cmd,
                Some(response.as_response()),
                spans[0],
                self.protocol_version,
                &mut reply,
            )
            .map_err(|_| ())?;
            return Ok(reply);
        }

        let request = TxnRequest {
            num_ops: ops.len(),
            ops: ops.as_ptr(),
        };
        let mut response = TxnResponse {
            transaction_success: false,
            num_results: 0,
            results: std::ptr::null_mut(),
        };
        let call_ok = unsafe { cpp_txn_execute(self.handle, &request, &mut response) };
        if !call_ok || response.num_results < ops.len() {
            unsafe { cpp_free_transaction_response(&mut response) };
            return Err(());
        }
        let formatted = write_command_result(
            cmd,
            Some(&response),
            spans[0],
            self.protocol_version,
            &mut reply,
        );
        unsafe { cpp_free_transaction_response(&mut response) };
        formatted.map_err(|_| ())?;
        Ok(reply)
    }

    /// True once a write command has run inside the session.
    fn has_written(&self) -> bool {
        self.dirty
    }

    /// Takes `&mut self` rather than `self` so the session can live inside the
    /// script state the Lua closures share; `finished` is what keeps it from
    /// being ended twice.
    fn commit(&mut self) -> bool {
        if self.finished {
            return false;
        }
        self.finished = true;
        if self.memory {
            self.deliver_publishes();
            self.replay_watch_invalidation();
            return true;
        }
        let committed = unsafe { cpp_txn_commit(self.handle) };
        if committed {
            self.deliver_publishes();
            self.replay_watch_invalidation();
        }
        committed
    }

    fn abort(&mut self) {
        if self.finished {
            return;
        }
        self.finished = true;
        if !self.memory {
            unsafe { cpp_txn_abort(self.handle) };
        }
    }

    fn deliver_publishes(&mut self) {
        for (channel, message) in std::mem::take(&mut self.pending_publishes) {
            publish_pubsub_message(&channel, &message);
        }
    }

    /// A committed session changed keys without going through
    /// `execute_or_block_command`'s dirty bookkeeping, so WATCH is told here.
    fn replay_watch_invalidation(&mut self) {
        if self.written_keys.is_empty() {
            return;
        }
        record_dirty_change();
        for key in std::mem::take(&mut self.written_keys) {
            bump_key_version(&key);
        }
    }
}

impl Drop for SessionTxn {
    fn drop(&mut self) {
        if !self.finished && !self.memory {
            unsafe { cpp_txn_abort(self.handle) };
            self.finished = true;
        }
    }
}

/// Runs `body` inside a session that declares `keys`, committing when the body
/// asks for it and re-running the whole body in a fresh session when the
/// storage commit loses an optimistic-concurrency race. Returns `None` when
/// every attempt was used up, which the caller reports as `ERR backend`, the
/// same answer the batch path gives.
fn ffi_run_session<T>(
    keys: &[Bytes],
    body: impl FnMut(&mut SessionTxn) -> SessionRun<T>,
) -> Option<T> {
    ffi_run_session_as(keys, 2, body)
}

/// `ffi_run_session` with the RESP version the replies must be formatted for.
fn ffi_run_session_as<T>(
    keys: &[Bytes],
    protocol_version: u8,
    mut body: impl FnMut(&mut SessionTxn) -> SessionRun<T>,
) -> Option<T> {
    for attempt in 0..TXN_MAX_ATTEMPTS {
        let Some(mut session) = SessionTxn::begin(keys, protocol_version) else {
            return None;
        };
        match body(&mut session) {
            SessionRun::Commit(value) => {
                if session.commit() {
                    return Some(value);
                }
            }
            SessionRun::Abort(value) => {
                session.abort();
                return Some(value);
            }
            SessionRun::Retry => {
                session.abort();
            }
        }
        if attempt + 1 < TXN_MAX_ATTEMPTS {
            unsafe { cpp_record_txn_retry() };
            sleep_for_retry(attempt);
        }
    }
    None
}

/// How many subscribers a PUBLISH would reach right now, without delivering
/// anything. A script's PUBLISH is buffered until the session commits, but its
/// reply has to be written when the call is made.
fn count_pubsub_receivers(channel: &Bytes) -> usize {
    let Ok(mut registry) = pubsub_registry().lock() else {
        return 0;
    };
    registry.prune_dead();
    let mut receivers = registry
        .channels
        .get(channel)
        .map(|targets| targets.len())
        .unwrap_or(0);
    for (pattern, targets) in &registry.patterns {
        if glob_matches(pattern.as_ref(), channel.as_ref()) {
            receivers += targets.len();
        }
    }
    receivers
}

fn scan_page_start(cmd: &Command) -> usize {
    if cmd.expire_at_ms <= 0 {
        0
    } else {
        cmd.expire_at_ms as usize
    }
}

fn scan_page_limit(cmd: &Command) -> usize {
    usize::try_from(cmd.scan_count)
        .unwrap_or(10)
        .clamp(1, 1_000_000)
}

fn scan_page_pattern(cmd: &Command) -> &[u8] {
    if cmd.scan_prefix.is_empty() {
        b"*"
    } else {
        cmd.scan_prefix.as_ref()
    }
}

fn write_scan_page<W: Write>(
    writer: &mut W,
    next_index: usize,
    total_items: usize,
    items: Vec<Vec<u8>>,
) -> std::io::Result<()> {
    write_array_header(writer, 2)?;
    if next_index >= total_items {
        write_bulk(writer, b"0")?;
    } else {
        write_bulk(writer, store_scan_offset(next_index).as_bytes())?;
    }
    write_array_header(writer, items.len())?;
    for item in items {
        write_bulk(writer, &item)?;
    }
    Ok(())
}

fn write_paginated_member_scan<W: Write>(
    writer: &mut W,
    cmd: &Command,
    mut members: Vec<Vec<u8>>,
) -> std::io::Result<()> {
    members.sort();
    let total = members.len();
    let pattern = scan_page_pattern(cmd);
    let mut out = Vec::new();
    for member in members {
        if glob_matches(pattern, &member) {
            out.push(member);
        }
    }

    write_scan_page(writer, total, total, out)
}

fn write_paginated_zscan<W: Write>(
    writer: &mut W,
    cmd: &Command,
    items: Vec<Vec<u8>>,
) -> std::io::Result<()> {
    if items.len() % 2 != 0 {
        write_err(writer, "operation failed")?;
        return Ok(());
    }
    let total_members = items.len() / 2;
    let start = scan_page_start(cmd).min(total_members);
    let limit = scan_page_limit(cmd);
    let pattern = scan_page_pattern(cmd);
    let mut out = Vec::new();
    let mut next_member_index = start;

    for member_index in start..total_members {
        next_member_index = member_index + 1;
        let member = &items[member_index * 2];
        if glob_matches(pattern, member) {
            out.push(member.clone());
            out.push(items[member_index * 2 + 1].clone());
            if out.len() / 2 >= limit {
                break;
            }
        }
    }

    write_scan_page(writer, next_member_index, total_members, out)
}

fn write_paginated_hash_scan<W: Write>(
    writer: &mut W,
    cmd: &Command,
    items: Vec<Vec<u8>>,
) -> std::io::Result<()> {
    if items.len() % 2 != 0 {
        write_err(writer, "operation failed")?;
        return Ok(());
    }
    let total_fields = items.len() / 2;
    let start = scan_page_start(cmd).min(total_fields);
    let limit = scan_page_limit(cmd);
    let pattern = scan_page_pattern(cmd);
    let mut out = Vec::new();
    let mut next_field_index = start;

    for field_index in start..total_fields {
        next_field_index = field_index + 1;
        let field = &items[field_index * 2];
        if glob_matches(pattern, field) {
            out.push(field.clone());
            if !cmd.set_integer_reply {
                out.push(items[field_index * 2 + 1].clone());
            }
            let returned = if cmd.set_integer_reply {
                out.len()
            } else {
                out.len() / 2
            };
            if returned >= limit {
                break;
            }
        }
    }

    write_scan_page(writer, next_field_index, total_fields, out)
}

fn write_hrandfield<W: Write>(
    writer: &mut W,
    cmd: &Command,
    items: Vec<Vec<u8>>,
    protocol_version: u8,
) -> std::io::Result<()> {
    if items.len() % 2 != 0 {
        write_err(writer, "operation failed")?;
        return Ok(());
    }
    let fields: Vec<(Vec<u8>, Vec<u8>)> = items
        .chunks_exact(2)
        .map(|chunk| (chunk[0].clone(), chunk[1].clone()))
        .collect();

    let Some(count) = cmd.set_count else {
        if !fields.is_empty() {
            let index = NEXT_SCAN_CURSOR_ID.fetch_add(1, Ordering::Relaxed) % fields.len();
            let (field, value) = &fields[index];
            if cmd.set_return_old {
                write_array_header(writer, 2)?;
                write_bulk(writer, field)?;
                write_bulk(writer, value)?;
            } else {
                write_bulk(writer, field)?;
            }
        } else {
            write_null(writer, protocol_version)?;
        }
        return Ok(());
    };

    if fields.is_empty() || count == 0 {
        write_array_header(writer, 0)?;
        return Ok(());
    }

    let requested = count.saturating_abs() as usize;
    let with_values = cmd.set_return_old;
    let allow_duplicates = count < 0;
    let returned = if allow_duplicates {
        requested
    } else {
        requested.min(fields.len())
    };

    if with_values && protocol_version >= 3 {
        write_array_header(writer, returned)?;
    } else {
        write_array_header(writer, if with_values { returned * 2 } else { returned })?;
    }
    let start = NEXT_SCAN_CURSOR_ID.fetch_add(1, Ordering::Relaxed) % fields.len();
    for index in 0..returned {
        let (field, value) = if allow_duplicates {
            &fields[(start + index) % fields.len()]
        } else {
            &fields[(start + index) % fields.len()]
        };
        if with_values && protocol_version >= 3 {
            write_array_header(writer, 2)?;
        }
        write_bulk(writer, field)?;
        if with_values {
            write_bulk(writer, value)?;
        }
    }
    Ok(())
}

fn write_time<W: Write>(writer: &mut W) -> std::io::Result<()> {
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default();
    write_array_header(writer, 2)?;
    write_bulk(writer, now.as_secs().to_string().as_bytes())?;
    write_bulk(writer, now.subsec_micros().to_string().as_bytes())
}

fn unix_time_ms() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| {
            duration
                .as_secs()
                .saturating_mul(1000)
                .saturating_add(u64::from(duration.subsec_millis())) as i64
        })
        .unwrap_or(0)
}

fn write_command_result<W: Write>(
    cmd: &Command,
    response: Option<&TxnResponse>,
    span: (usize, usize),
    protocol_version: u8,
    writer: &mut W,
) -> std::io::Result<()> {
    if cmd.op == OpCode::Ping {
        if let Some(arg) = cmd.args.first() {
            write_bulk(writer, arg)?;
        } else {
            write_pong(writer)?;
        }
        return Ok(());
    }
    if cmd.op == OpCode::Wait {
        write_integer(writer, 0)?;
        return Ok(());
    }
    if cmd.op == OpCode::Time {
        write_time(writer)?;
        return Ok(());
    }
    if cmd.op == OpCode::Publish {
        handle_publish(cmd, writer)?;
        return Ok(());
    }
    if cmd.op == OpCode::Monitor {
        // Redis's monitorCommand refuses a client that owes a reply per
        // command, which is every client inside MULTI.
        write_err(writer, "MONITOR isn't allowed for DENY BLOCKING client")?;
        return Ok(());
    }
    if cmd.op == OpCode::Cluster {
        handle_cluster_command(cmd, cluster_mode(), writer)?;
        return Ok(());
    }
    if cmd.op == OpCode::XInfo && cmd.restore_kind == STREAM_XINFO_HELP {
        write_array_header(writer, XINFO_HELP.len())?;
        for line in XINFO_HELP {
            write_simple_string(writer, line)?;
        }
        return Ok(());
    }
    if cmd.op == OpCode::XGroup && cmd.restore_kind == STREAM_XGROUP_HELP {
        write_array_header(writer, XGROUP_HELP.len())?;
        for line in XGROUP_HELP {
            write_simple_string(writer, line)?;
        }
        return Ok(());
    }
    if matches!(cmd.op, OpCode::ReadOnly | OpCode::ReadWrite) {
        write_cluster_readonly_reply(cluster_mode(), writer)?;
        return Ok(());
    }
    if cmd.op == OpCode::Module {
        handle_module_command(cmd, writer)?;
        return Ok(());
    }
    if cmd.op == OpCode::Config {
        if cmd.args.len() == 3
            && ascii_eq_ci(cmd.args[0].as_ref(), b"SET")
            && ascii_eq_ci(cmd.args[1].as_ref(), b"maxmemory")
            && std::str::from_utf8(cmd.args[2].as_ref())
                .ok()
                .and_then(|text| text.parse::<usize>().ok())
                .is_none()
        {
            write_err(writer, "CONFIG SET failed (possibly related to argument 'maxmemory') - argument must be a memory value")?;
        } else {
            write_simple_ok(writer)?;
        }
        return Ok(());
    }

    if matches!(cmd.op, OpCode::GeoPos | OpCode::GeoHash) && cmd.values.is_empty() {
        // `GEOPOS key` with no members is legal and returns an empty array.
        write_array_header(writer, 0)?;
        return Ok(());
    }
    if cmd.op == OpCode::GeoSearch {
        if let Some(spec) = cmd.geo.as_deref() {
            // The FROMMEMBER center lookup already ran and failed, so no range
            // reads were built for this command.
            match spec.resolve_error {
                GeoResolve::KeyMissing => {
                    // No key at all: Redis searches an empty set, it does not
                    // complain about the member.
                    write_array_header(writer, 0)?;
                    return Ok(());
                }
                GeoResolve::MemberMissing => {
                    write_err(writer, "could not decode requested zset member")?;
                    return Ok(());
                }
                GeoResolve::WrongType => {
                    write_wrongtype(writer)?;
                    return Ok(());
                }
                GeoResolve::Failed => {
                    write_err(writer, "backend")?;
                    return Ok(());
                }
                GeoResolve::Ok => {}
            }
        }
    }

    let Some(response) = response else {
        write_err(writer, "operation failed")?;
        return Ok(());
    };

    let (start, len) = span;
    if len == 0 || start + len > response.num_results {
        write_err(writer, "operation failed")?;
        return Ok(());
    }

    let first = unsafe { &*response.results.add(start) };
    match cmd.op {
        OpCode::Get | OpCode::GetSet | OpCode::GetEx | OpCode::GetDel | OpCode::GetRange => {
            if !first.success {
                if cmd.op == OpCode::GetRange {
                    write_wrongtype(writer)?;
                } else {
                    write_err(writer, "operation failed")?;
                }
            } else if first.value_present {
                if first.data_len > 0 {
                    if first.data_ptr.is_null() {
                        write_err(writer, "operation failed")?;
                    } else {
                        let data =
                            unsafe { std::slice::from_raw_parts(first.data_ptr, first.data_len) };
                        write_bulk(writer, data)?;
                    }
                } else {
                    write_bulk(writer, b"")?;
                }
            } else {
                if cmd.set_return_old && protocol_version < 3 {
                    writer.write_all(b"*-1\r\n")?;
                } else {
                    write_null(writer, protocol_version)?;
                }
            }
        }
        OpCode::BitCount => {
            if !first.success {
                write_wrongtype(writer)?;
            } else {
                match bitcount_in_range(result_value_bytes(first), &cmd.values) {
                    Ok(count) => write_integer(writer, count)?,
                    Err(message) => write_err(writer, message)?,
                }
            }
        }
        OpCode::BitPos => {
            if !first.success {
                write_wrongtype(writer)?;
            } else {
                match bitpos_in_range(result_value_bytes(first), first.value_present, &cmd.values)
                {
                    Ok(position) => write_integer(writer, position)?,
                    Err(message) => write_err(writer, message)?,
                }
            }
        }
        OpCode::BitFieldRo => {
            if !first.success {
                write_wrongtype(writer)?;
            } else {
                let values = bitfield_ro_values(result_value_bytes(first), &cmd.values);
                write_array_header(writer, values.len())?;
                for value in values {
                    write_integer(writer, value)?;
                }
            }
        }
        OpCode::BitField => {
            if !first.success {
                write_wrongtype(writer)?;
            } else {
                // One packed item per GET/SET/INCRBY in command order: the
                // decimal result, or an empty string for the nil an
                // OVERFLOW FAIL subcommand returns.
                let items = parse_list_payload(result_value_bytes(first)).unwrap_or_default();
                write_array_header(writer, items.len())?;
                for item in items {
                    match parse_i64_lossy(&item) {
                        Some(value) => write_integer(writer, value)?,
                        None => write_null(writer, protocol_version)?,
                    }
                }
            }
        }
        OpCode::BitOp => {
            if first.success {
                write_integer(writer, first.int_value)?;
            } else {
                write_wrongtype(writer)?;
            }
        }
        OpCode::GeoAdd => {
            if first.success {
                write_integer(writer, first.int_value)?;
            } else {
                write_wrongtype(writer)?;
            }
        }
        OpCode::GeoPos | OpCode::GeoHash => {
            // One ZSCORE result per member. A wrong-type key fails the whole
            // reply, so scan for that before opening the array.
            let mut scores = Vec::with_capacity(len);
            for index in start..start + len {
                let result = unsafe { &*response.results.add(index) };
                if !result.success {
                    write_wrongtype(writer)?;
                    return Ok(());
                }
                scores.push(
                    geo_score_from_bytes(result_value_bytes(result))
                        .filter(|_| result.value_present),
                );
            }
            write_array_header(writer, scores.len())?;
            for score in scores {
                match score {
                    // GEOPOS reports the center of the member's geohash box.
                    Some(score) if cmd.op == OpCode::GeoPos => {
                        let (lon, lat) = geo_decode_score(score);
                        write_array_header(writer, 2)?;
                        write_score(writer, geo_format_coord(lon).as_bytes(), protocol_version)?;
                        write_score(writer, geo_format_coord(lat).as_bytes(), protocol_version)?;
                    }
                    Some(score) => match geo_hash_string(score) {
                        Some(text) => write_bulk(writer, text.as_bytes())?,
                        None => write_null(writer, protocol_version)?,
                    },
                    // A member the set does not hold is a nil, not an error.
                    None => write_null(writer, protocol_version)?,
                }
            }
        }
        OpCode::GeoDist => {
            let mut positions = Vec::with_capacity(2);
            for index in start..start + len {
                let result = unsafe { &*response.results.add(index) };
                if !result.success {
                    write_wrongtype(writer)?;
                    return Ok(());
                }
                match geo_score_from_bytes(result_value_bytes(result))
                    .filter(|_| result.value_present)
                {
                    Some(score) => positions.push(geo_decode_score(score)),
                    // Either member missing makes the whole reply a nil.
                    None => {
                        write_null(writer, protocol_version)?;
                        return Ok(());
                    }
                }
            }
            if positions.len() != 2 {
                write_null(writer, protocol_version)?;
            } else {
                let unit = cmd
                    .val
                    .as_ref()
                    .and_then(|unit| geo_unit_meters(unit.as_ref()))
                    .unwrap_or(1.0);
                let meters = geo_distance(
                    positions[0].0,
                    positions[0].1,
                    positions[1].0,
                    positions[1].1,
                );
                write_bulk(writer, geo_format_distance(meters / unit).as_bytes())?;
            }
        }
        OpCode::GeoSearch => {
            let Some(spec) = cmd.geo.as_deref() else {
                write_err(writer, "operation failed")?;
                return Ok(());
            };
            // Merge the candidates of every box, keep the ones the exact shape
            // test accepts, then sort, truncate and format as Redis does.
            let boxes = unsafe { std::slice::from_raw_parts(response.results.add(start), len) };
            let mut points = match geo_collect_matches(spec, boxes) {
                GeoMatches::Points(points) => points,
                GeoMatches::WrongType => {
                    write_wrongtype(writer)?;
                    return Ok(());
                }
                GeoMatches::Failed => {
                    write_err(writer, "operation failed")?;
                    return Ok(());
                }
            };
            let returned = geo_sort_matches(spec, &mut points);

            write_array_header(writer, returned)?;
            if !spec.withcoord && !spec.withdist && !spec.withhash {
                for point in points.iter().take(returned) {
                    write_bulk(writer, &point.member)?;
                }
            } else {
                let columns = usize::from(spec.withdist)
                    + usize::from(spec.withhash)
                    + usize::from(spec.withcoord);
                for point in points.iter().take(returned) {
                    write_array_header(writer, columns + 1)?;
                    write_bulk(writer, &point.member)?;
                    // Redis emits the columns in this order: dist, hash, coord.
                    if spec.withdist {
                        write_bulk(
                            writer,
                            geo_format_distance(point.dist / spec.unit_meters).as_bytes(),
                        )?;
                    }
                    if spec.withhash {
                        write_integer(writer, point.score as i64)?;
                    }
                    if spec.withcoord {
                        write_array_header(writer, 2)?;
                        write_score(
                            writer,
                            geo_format_coord(point.lon).as_bytes(),
                            protocol_version,
                        )?;
                        write_score(
                            writer,
                            geo_format_coord(point.lat).as_bytes(),
                            protocol_version,
                        )?;
                    }
                }
            }
        }
        OpCode::GeoSearchStore => {
            // Never reached: GEOSEARCHSTORE answers from the session that ran
            // its reads and its write, and `build_txn_ops` gives it no ops.
            write_err(writer, "operation failed")?;
        }
        OpCode::PfAdd | OpCode::PfCount => {
            if first.success {
                write_integer(writer, first.int_value)?;
            } else if first.int_value == HLL_ERR_NOT_HLL {
                write_invalid_hll(writer)?;
            } else {
                write_wrongtype(writer)?;
            }
        }
        OpCode::PfMerge => {
            if first.success {
                write_simple_ok(writer)?;
            } else if first.int_value == HLL_ERR_NOT_HLL {
                write_invalid_hll(writer)?;
            } else {
                write_wrongtype(writer)?;
            }
        }
        OpCode::Object => {
            if !first.success {
                write_err(writer, "operation failed")?;
            } else {
                let subcommand = cmd.args.first().map(|arg| arg.as_ref()).unwrap_or(b"");
                if !(1..=6).contains(&first.int_value) {
                    write_null(writer, protocol_version)?;
                } else if ascii_eq_ci(subcommand, b"ENCODING") {
                    // Mako has no Redis object encodings; report the canonical
                    // large-object encoding for each type.
                    let encoding = match first.int_value {
                        1 => "raw",
                        2 => "hashtable",
                        3 => "quicklist",
                        4 => "skiplist",
                        6 => "stream",
                        _ => "hashtable",
                    };
                    write_bulk(writer, encoding.as_bytes())?;
                } else if ascii_eq_ci(subcommand, b"REFCOUNT") {
                    write_integer(writer, 1)?;
                } else if ascii_eq_ci(subcommand, b"FREQ") {
                    write_err(
                        writer,
                        "An LFU maxmemory policy is not selected, access frequency not tracked. Please note that when switching between policies at runtime LRU and LFU data will take some time to adjust.",
                    )?;
                } else {
                    write_err(
                        writer,
                        "OBJECT IDLETIME is not supported: Mako does not track key access time",
                    )?;
                }
            }
        }
        OpCode::Memory => {
            if !first.success {
                write_err(writer, "operation failed")?;
            } else if first.value_present {
                // Approximation: key bytes + serialized value bytes + fixed
                // per-key overhead, in the spirit of Redis's own estimate.
                let key_len = cmd.keys.first().map(|key| key.len()).unwrap_or(0) as i64;
                write_integer(writer, key_len + first.data_len as i64 + 56)?;
            } else {
                write_null(writer, protocol_version)?;
            }
        }
        OpCode::Lcs => {
            write_lcs_result(cmd, response, span, writer)?;
        }
        OpCode::Dump => {
            if !first.success {
                write_wrongtype(writer)?;
            } else if first.value_present {
                if first.data_ptr.is_null() && first.data_len > 0 {
                    write_err(writer, "operation failed")?;
                } else {
                    let data = if first.data_len == 0 {
                        &[][..]
                    } else {
                        unsafe { std::slice::from_raw_parts(first.data_ptr, first.data_len) }
                    };
                    write_bulk(writer, data)?;
                }
            } else {
                write_null(writer, protocol_version)?;
            }
        }
        OpCode::MGet => {
            write_array_header(writer, len)?;
            for index in start..start + len {
                let result = unsafe { &*response.results.add(index) };
                if !result.success {
                    write_err(writer, "operation failed")?;
                    return Ok(());
                }
                if result.value_present {
                    if result.data_len > 0 {
                        if result.data_ptr.is_null() {
                            write_err(writer, "operation failed")?;
                            return Ok(());
                        }
                        let data =
                            unsafe { std::slice::from_raw_parts(result.data_ptr, result.data_len) };
                        write_bulk(writer, data)?;
                    } else {
                        write_bulk(writer, b"")?;
                    }
                } else {
                    write_null(writer, protocol_version)?;
                }
            }
        }
        OpCode::Set | OpCode::SetEx | OpCode::PSetEx => {
            if !first.success {
                if cmd.set_return_old {
                    write_wrongtype(writer)?;
                } else {
                    write_err(writer, "operation failed")?;
                }
            } else if cmd.set_return_old {
                if first.value_present {
                    if first.data_len > 0 {
                        if first.data_ptr.is_null() {
                            write_err(writer, "operation failed")?;
                        } else {
                            let data = unsafe {
                                std::slice::from_raw_parts(first.data_ptr, first.data_len)
                            };
                            write_bulk(writer, data)?;
                        }
                    } else {
                        write_bulk(writer, b"")?;
                    }
                } else {
                    write_null(writer, protocol_version)?;
                }
            } else if cmd.set_condition == SetCondition::None {
                write_simple_ok(writer)?;
            } else if first.value_present {
                write_simple_ok(writer)?;
            } else {
                write_null(writer, protocol_version)?;
            }
        }
        OpCode::MSet => {
            for index in start..start + len {
                let result = unsafe { &*response.results.add(index) };
                if !result.success {
                    write_err(writer, "operation failed")?;
                    return Ok(());
                }
            }
            write_simple_ok(writer)?;
        }
        OpCode::Rename => {
            if first.success {
                write_simple_ok(writer)?;
            } else if first.int_value == -1 {
                write_err(writer, "no such key")?;
            } else {
                write_err(writer, "operation failed")?;
            }
        }
        OpCode::RenameNx => {
            if first.success {
                write_integer(writer, first.int_value)?;
            } else if first.int_value == -1 {
                write_err(writer, "no such key")?;
            } else {
                write_err(writer, "operation failed")?;
            }
        }
        OpCode::Copy | OpCode::Move => {
            if first.success {
                write_integer(writer, first.int_value)?;
            } else {
                write_err(writer, "operation failed")?;
            }
        }
        OpCode::Sort | OpCode::SortRo => {
            let store = cmd.values.first().map(|v| !v.is_empty()).unwrap_or(false);
            if !first.success {
                write_wrongtype(writer)?;
            } else if store {
                write_integer(writer, first.int_value)?;
            } else if first.value_present && !first.data_ptr.is_null() {
                let data = unsafe { std::slice::from_raw_parts(first.data_ptr, first.data_len) };
                if let Some(items) = parse_list_payload(data) {
                    write_array_header(writer, items.len())?;
                    for item in items {
                        write_bulk(writer, &item)?;
                    }
                } else {
                    write_err(writer, "operation failed")?;
                }
            } else {
                write_array_header(writer, 0)?;
            }
        }
        OpCode::MSetNx => {
            let mut wrote_all = len > 0;
            for index in start..start + len {
                let result = unsafe { &*response.results.add(index) };
                if !result.success {
                    write_err(writer, "operation failed")?;
                    return Ok(());
                }
                wrote_all &= result.value_present;
            }
            write_integer(writer, if wrote_all { 1 } else { 0 })?;
        }
        OpCode::SetNx => {
            if first.success {
                write_integer(writer, if first.value_present { 1 } else { 0 })?;
            } else {
                write_err(writer, "operation failed")?;
            }
        }
        OpCode::Append
        | OpCode::StrLen
        | OpCode::SetBit
        | OpCode::GetBit
        | OpCode::SetRange
        | OpCode::Expire
        | OpCode::PExpire
        | OpCode::ExpireAt
        | OpCode::PExpireAt
        | OpCode::Ttl
        | OpCode::PTtl
        | OpCode::Persist => {
            if first.success {
                write_integer(writer, first.int_value)?;
            } else {
                if matches!(cmd.op, OpCode::SetBit | OpCode::GetBit | OpCode::SetRange) {
                    write_wrongtype(writer)?;
                } else {
                    write_err(writer, "operation failed")?;
                }
            }
        }
        OpCode::Incr | OpCode::IncrBy | OpCode::Decr | OpCode::DecrBy => {
            // A failed increment is a command error, not a backend failure:
            // the executor reports it with a TXN_INCR_ERR_* sentinel so these
            // replies read exactly as Redis's do.
            if first.success {
                write_integer(writer, first.int_value)?;
            } else if first.int_value == INCR_ERR_NOT_INTEGER {
                write_err(writer, "value is not an integer or out of range")?;
            } else if first.int_value == INCR_ERR_OVERFLOW {
                write_err(writer, "increment or decrement would overflow")?;
            } else {
                write_wrongtype(writer)?;
            }
        }
        OpCode::ExpireTime | OpCode::PExpireTime => {
            if first.success {
                if first.int_value < 0 {
                    write_integer(writer, first.int_value)?;
                } else if cmd.op == OpCode::PExpireTime {
                    write_integer(writer, unix_time_ms().saturating_add(first.int_value))?;
                } else {
                    write_integer(writer, unix_time_ms() / 1000 + first.int_value)?;
                }
            } else {
                write_err(writer, "operation failed")?;
            }
        }
        OpCode::DbSize => {
            if first.success {
                write_integer(writer, first.int_value)?;
            } else {
                write_wrongtype(writer)?;
            }
        }
        OpCode::FlushDb | OpCode::FlushAll => {
            if first.success {
                write_simple_ok(writer)?;
            } else {
                write_err(writer, "operation failed")?;
            }
        }
        OpCode::Restore => {
            let mut ok = true;
            for index in start..start + len {
                let result = unsafe { &*response.results.add(index) };
                ok &= result.success;
            }
            if ok {
                write_simple_ok(writer)?;
            } else {
                write_err(writer, "operation failed")?;
            }
        }
        OpCode::Type => {
            if first.success {
                match first.int_value {
                    1 => write_simple_string(writer, "string")?,
                    2 => write_simple_string(writer, "set")?,
                    3 => write_simple_string(writer, "list")?,
                    4 => write_simple_string(writer, "zset")?,
                    5 => write_simple_string(writer, "hash")?,
                    6 => write_simple_string(writer, "stream")?,
                    _ => write_simple_string(writer, "none")?,
                }
            } else {
                write_err(writer, "operation failed")?;
            }
        }
        OpCode::XAdd => {
            if !first.success {
                write_wrongtype(writer)?;
            } else if first.int_value == TXN_STREAM_ERR_NOMKSTREAM {
                write_null(writer, protocol_version)?;
            } else if first.int_value == TXN_STREAM_ERR_SMALLER_ID {
                write_err(writer, STREAM_SMALLER_ID_ERROR)?;
            } else if first.value_present {
                write_bulk(writer, result_value_bytes(first))?;
            } else {
                write_err(writer, "operation failed")?;
            }
        }
        OpCode::XRange | OpCode::XRevRange => {
            if !first.success {
                write_wrongtype(writer)?;
            } else if let Some(entries) = stream_read_entries(first) {
                write_array_header(writer, entries.len())?;
                for (id, fields) in &entries {
                    write_stream_entry(writer, id, fields, protocol_version)?;
                }
            } else {
                write_err(writer, "operation failed")?;
            }
        }
        OpCode::XRead => {
            // One op per stream. A stream with no matching entry is left out,
            // and a reply with no stream at all is a null -- which is also
            // what tells the blocking path to park the client.
            let db = current_db();
            let mut served: Vec<(usize, Vec<(Bytes, Bytes)>)> = Vec::new();
            let mut outcome = Ok(());
            for offset in 0..len {
                let result = unsafe { &*response.results.add(start + offset) };
                if !result.success {
                    outcome = Err(true);
                    break;
                }
                match stream_read_entries(result) {
                    Some(entries) => {
                        if !entries.is_empty() {
                            served.push((offset, entries));
                        }
                    }
                    None => {
                        outcome = Err(false);
                        break;
                    }
                }
            }
            match outcome {
                Err(true) => write_wrongtype(writer)?,
                Err(false) => write_err(writer, "operation failed")?,
                Ok(()) => {
                    if served.is_empty() {
                        if protocol_version >= 3 {
                            write_null(writer, protocol_version)?;
                        } else {
                            writer.write_all(b"*-1\r\n")?;
                        }
                    } else {
                        // RESP2 pairs each key with its entries in a two-element
                        // array; RESP3 sends the same thing as a map.
                        if protocol_version >= 3 {
                            write_map_header(writer, served.len())?;
                        } else {
                            write_array_header(writer, served.len())?;
                        }
                        for (offset, entries) in &served {
                            if protocol_version < 3 {
                                write_array_header(writer, 2)?;
                            }
                            let key = cmd.keys.get(*offset).cloned().unwrap_or_default();
                            write_bulk(writer, strip_db_key(db, &key))?;
                            write_array_header(writer, entries.len())?;
                            for (id, fields) in entries {
                                write_stream_entry(writer, id, fields, protocol_version)?;
                            }
                        }
                    }
                }
            }
        }
        OpCode::XGroup => {
            let group = cmd.values.get(1).cloned().unwrap_or_default();
            if !first.success {
                write_wrongtype(writer)?;
            } else {
                match first.int_value {
                    TXN_STREAM_ERR_NO_KEY_FOR_GROUP => write_err(
                        writer,
                        "The XGROUP subcommand requires the key to exist. Note that for CREATE \
                         you may want to use the MKSTREAM option to create an empty stream \
                         automatically.",
                    )?,
                    TXN_STREAM_ERR_BUSYGROUP => {
                        write_coded_err(writer, "BUSYGROUP Consumer Group name already exists")?
                    }
                    TXN_STREAM_ERR_NOGROUP => write_coded_err(
                        writer,
                        &format!(
                            "NOGROUP No such consumer group '{}' for key name '{}'",
                            String::from_utf8_lossy(&group),
                            String::from_utf8_lossy(stream_reply_key_name(cmd, 0)),
                        ),
                    )?,
                    // CREATE and SETID answer OK; the other three answer a
                    // count.
                    _ => {
                        if matches!(
                            cmd.restore_kind,
                            STREAM_XGROUP_CREATE | STREAM_XGROUP_SETID
                        ) {
                            write_simple_ok(writer)?;
                        } else {
                            write_integer(writer, first.int_value)?;
                        }
                    }
                }
            }
        }
        OpCode::XReadGroup => {
            // One op per stream. A stream read with ">" is left out when it
            // had nothing to hand over, and a reply with no stream at all is a
            // null; a replay of the consumer's own pending entries always
            // reports its stream, even when the replay is empty.
            let db = current_db();
            let group = cmd.values.first().cloned().unwrap_or_default();
            let mut served: Vec<(usize, Vec<(Bytes, Bytes)>)> = Vec::new();
            let mut outcome = Ok(());
            for offset in 0..len {
                let result = unsafe { &*response.results.add(start + offset) };
                if !result.success {
                    outcome = Err(None);
                    break;
                }
                if result.int_value == TXN_STREAM_ERR_NOGROUP {
                    outcome = Err(Some(offset));
                    break;
                }
                let Some(entries) = stream_group_entries(result) else {
                    outcome = Err(None);
                    break;
                };
                let new_entries = cmd
                    .values
                    .get(STREAM_READ_VALUE_PREFIX + offset)
                    .map(|spec| spec.as_ref() == b">")
                    .unwrap_or(false);
                if !new_entries || !entries.is_empty() {
                    served.push((offset, entries));
                }
            }
            match outcome {
                Err(Some(offset)) => write_coded_err(
                    writer,
                    &format!(
                        "NOGROUP No such key '{}' or consumer group '{}' in XREADGROUP with \
                         GROUP option",
                        String::from_utf8_lossy(stream_reply_key_name(cmd, offset)),
                        String::from_utf8_lossy(&group),
                    ),
                )?,
                Err(None) => write_wrongtype(writer)?,
                Ok(()) => {
                    if served.is_empty() {
                        if protocol_version >= 3 {
                            write_null(writer, protocol_version)?;
                        } else {
                            writer.write_all(b"*-1\r\n")?;
                        }
                    } else {
                        if protocol_version >= 3 {
                            write_map_header(writer, served.len())?;
                        } else {
                            write_array_header(writer, served.len())?;
                        }
                        for (offset, entries) in &served {
                            if protocol_version < 3 {
                                write_array_header(writer, 2)?;
                            }
                            let key = cmd.keys.get(*offset).cloned().unwrap_or_default();
                            write_bulk(writer, strip_db_key(db, &key))?;
                            write_array_header(writer, entries.len())?;
                            for (id, fields) in entries {
                                write_stream_entry(writer, id, fields, protocol_version)?;
                            }
                        }
                    }
                }
            }
        }
        OpCode::XAck => {
            if first.success {
                write_integer(writer, first.int_value)?;
            } else {
                write_wrongtype(writer)?;
            }
        }
        OpCode::XPending => {
            if !first.success {
                write_wrongtype(writer)?;
            } else if first.int_value == TXN_STREAM_ERR_NOGROUP {
                write_stream_nogroup(cmd, writer)?;
            } else {
                match parse_list_payload(result_value_bytes(first)) {
                    Some(items) => {
                        if cmd.restore_kind == STREAM_XPENDING_SUMMARY {
                            write_xpending_summary(writer, &items, protocol_version)?;
                        } else {
                            write_xpending_range(writer, &items)?;
                        }
                    }
                    None => write_err(writer, "operation failed")?,
                }
            }
        }
        OpCode::XClaim | OpCode::XAutoClaim => {
            let autoclaim = cmd.op == OpCode::XAutoClaim;
            if !first.success {
                write_wrongtype(writer)?;
            } else if first.int_value == TXN_STREAM_ERR_NOGROUP {
                write_stream_nogroup(cmd, writer)?;
            } else {
                match parse_list_payload(result_value_bytes(first)) {
                    Some(items) => write_xclaim_result(
                        writer,
                        &items,
                        autoclaim,
                        stream_claim_justid(cmd),
                        protocol_version,
                    )?,
                    None => write_err(writer, "operation failed")?,
                }
            }
        }
        OpCode::XLen | OpCode::XDel | OpCode::XTrim => {
            if first.success {
                write_integer(writer, first.int_value)?;
            } else {
                write_wrongtype(writer)?;
            }
        }
        OpCode::XSetId => {
            if !first.success {
                write_wrongtype(writer)?;
            } else {
                match first.int_value {
                    TXN_STREAM_ERR_NO_SUCH_KEY => write_err(writer, "no such key")?,
                    TXN_STREAM_ERR_SETID_SMALLER => write_err(
                        writer,
                        "The ID specified in XSETID is smaller than the target stream top item",
                    )?,
                    TXN_STREAM_ERR_SETID_ENTRIES_ADDED => write_err(
                        writer,
                        "The entries_added specified in XSETID is smaller than the target stream length",
                    )?,
                    TXN_STREAM_ERR_SETID_TOMBSTONE => write_err(
                        writer,
                        "The ID specified in XSETID is smaller than the provided max_deleted_entry_id",
                    )?,
                    _ => write_simple_ok(writer)?,
                }
            }
        }
        OpCode::XInfo => {
            if !first.success {
                write_wrongtype(writer)?;
            } else if first.int_value == TXN_STREAM_ERR_NO_SUCH_KEY {
                write_err(writer, "no such key")?;
            } else if first.int_value == TXN_STREAM_ERR_NOGROUP {
                write_coded_err(
                    writer,
                    &format!(
                        "NOGROUP No such consumer group '{}' for key name '{}'",
                        String::from_utf8_lossy(cmd.values.get(1).map(|g| g.as_ref()).unwrap_or(b"")),
                        String::from_utf8_lossy(stream_reply_key_name(cmd, 0)),
                    ),
                )?;
            } else {
                match parse_list_payload(result_value_bytes(first)) {
                    Some(items) => {
                        match cmd.restore_kind {
                            STREAM_XINFO_STREAM_FULL => {
                                write_xinfo_stream_full(writer, &items, protocol_version)?
                            }
                            STREAM_XINFO_GROUPS => {
                                write_xinfo_groups(writer, &items, protocol_version)?
                            }
                            STREAM_XINFO_CONSUMERS => {
                                write_xinfo_consumers(writer, &items, protocol_version)?
                            }
                            _ => write_xinfo_stream(writer, &items, protocol_version)?,
                        }
                    }
                    None => write_err(writer, "operation failed")?,
                }
            }
        }

        OpCode::Keys => {
            let pattern = cmd.val.as_ref().map(|v| v.as_ref()).unwrap_or(b"*");
            if let Some((_, keys)) = scan_result_from_response(first) {
                // The client asked about its own database, so it gets the
                // names it would have sent, without the database prefix.
                let db = current_db();
                let keys: Vec<Vec<u8>> = keys
                    .into_iter()
                    .map(|key| strip_db_key(db, &key).to_vec())
                    .collect();
                write_keys_array(writer, keys, pattern)?;
            } else {
                write_err(writer, "operation failed")?;
            }
        }
        OpCode::Scan => {
            let pattern = cmd.val.as_ref().map(|v| v.as_ref()).unwrap_or(b"*");
            if let Some((cursor, keys)) = scan_result_from_response(first) {
                // Stripped before MATCH runs, so the pattern means what the
                // client wrote. The cursor stays in its storage-facing form,
                // because it is opaque and goes straight back to the executor.
                let db = current_db();
                let matched: Vec<Vec<u8>> = keys
                    .into_iter()
                    .map(|key| strip_db_key(db, &key).to_vec())
                    .filter(|key| glob_matches(pattern, key))
                    .collect();
                write_array_header(writer, 2)?;
                write_bulk(writer, store_scan_cursor(&cursor).as_bytes())?;
                write_array_header(writer, matched.len())?;
                for key in matched {
                    write_bulk(writer, &key)?;
                }
            } else {
                write_err(writer, "operation failed")?;
            }
        }
        OpCode::RandomKey => {
            if let Some((_, keys)) = scan_result_from_response(first) {
                if keys.is_empty() {
                    write_null(writer, protocol_version)?;
                } else {
                    let index = RANDOMKEY_COUNTER.fetch_add(1, Ordering::Relaxed) % keys.len();
                    write_bulk(writer, strip_db_key(current_db(), &keys[index]))?;
                }
            } else {
                write_err(writer, "operation failed")?;
            }
        }
        OpCode::IncrByFloat => {
            if !first.success {
                if first.int_value == INCR_ERR_NOT_FLOAT {
                    write_err(writer, "value is not a valid float")?;
                } else if first.int_value == INCR_ERR_NAN_OR_INF {
                    write_err(writer, "increment would produce NaN or Infinity")?;
                } else {
                    write_wrongtype(writer)?;
                }
            } else if first.value_present {
                if first.data_len > 0 {
                    if first.data_ptr.is_null() {
                        write_err(writer, "operation failed")?;
                    } else {
                        let data =
                            unsafe { std::slice::from_raw_parts(first.data_ptr, first.data_len) };
                        write_bulk(writer, data)?;
                    }
                } else {
                    write_bulk(writer, b"")?;
                }
            } else {
                write_err(writer, "operation failed")?;
            }
        }
        OpCode::Del | OpCode::Exists | OpCode::Touch => {
            let mut count = 0i64;
            for index in start..start + len {
                let result = unsafe { &*response.results.add(index) };
                if !result.success {
                    write_err(writer, "operation failed")?;
                    return Ok(());
                }
                if result.value_present {
                    count += 1;
                }
            }
            write_integer(writer, count)?;
        }
        OpCode::HSet
        | OpCode::HSetNx
        | OpCode::HDel
        | OpCode::HExists
        | OpCode::HLen
        | OpCode::HStrLen
        | OpCode::HIncrBy => {
            if first.success {
                write_integer(writer, first.int_value)?;
            } else if cmd.op == OpCode::HIncrBy && first.int_value == -2 {
                write_err(writer, "increment or decrement would overflow")?;
            } else if cmd.op == OpCode::HIncrBy && first.int_value == -1 {
                write_err(writer, "hash value is not an integer")?;
            } else {
                write_wrongtype(writer)?;
            }
        }
        OpCode::HMSet => {
            if first.success {
                write_simple_ok(writer)?;
            } else {
                write_wrongtype(writer)?;
            }
        }
        OpCode::HExpire
        | OpCode::HPExpire
        | OpCode::HExpireAt
        | OpCode::HPExpireAt
        | OpCode::HTtl
        | OpCode::HPTtl
        | OpCode::HExpireTime
        | OpCode::HPExpireTime
        | OpCode::HPersist => {
            if !first.success {
                write_wrongtype(writer)?;
            } else {
                // One packed decimal per field, in order. The HEXPIRE and
                // HPERSIST families already carry their reply codes; the TTL
                // family carries the absolute Unix ms of a field that has an
                // expiration, and the negative codes -1 and -2 otherwise.
                let items = parse_list_payload(result_value_bytes(first)).unwrap_or_default();
                write_array_header(writer, items.len())?;
                let now_ms = unix_time_ms();
                for item in items {
                    let raw = parse_i64_lossy(&item).unwrap_or(-2);
                    let value = if raw < 0 {
                        raw
                    } else {
                        // Redis converts with the same rounding as TTL: the
                        // second-granularity replies round the remaining or
                        // absolute milliseconds up.
                        match cmd.op {
                            OpCode::HTtl => (raw + 999 - now_ms) / 1000,
                            OpCode::HPTtl => raw - now_ms,
                            OpCode::HExpireTime => (raw + 999) / 1000,
                            OpCode::HPExpireTime => raw,
                            _ => raw,
                        }
                    };
                    write_integer(writer, value)?;
                }
            }
        }
        OpCode::HGet | OpCode::HIncrByFloat => {
            if !first.success && cmd.op == OpCode::HIncrByFloat && first.int_value == -3 {
                write_err(
                    writer,
                    "hash value is not a valid float: value is NaN or Infinity",
                )?;
            } else if !first.success {
                write_wrongtype(writer)?;
            } else if first.value_present {
                if first.data_len > 0 {
                    if first.data_ptr.is_null() {
                        write_err(writer, "operation failed")?;
                    } else {
                        let data =
                            unsafe { std::slice::from_raw_parts(first.data_ptr, first.data_len) };
                        write_bulk(writer, data)?;
                    }
                } else {
                    write_bulk(writer, b"")?;
                }
            } else {
                write_null(writer, protocol_version)?;
            }
        }
        OpCode::HMGet => {
            if !first.success || !first.value_present || first.data_ptr.is_null() {
                write_wrongtype(writer)?;
            } else {
                let data = unsafe { std::slice::from_raw_parts(first.data_ptr, first.data_len) };
                if let Some(items) = parse_list_payload(data) {
                    if items.len() % 2 != 0 {
                        write_err(writer, "operation failed")?;
                    } else {
                        write_array_header(writer, items.len() / 2)?;
                        for pair in items.chunks_exact(2) {
                            if pair[0].as_slice() == b"1" {
                                write_bulk(writer, &pair[1])?;
                            } else {
                                write_null(writer, protocol_version)?;
                            }
                        }
                    }
                } else {
                    write_err(writer, "operation failed")?;
                }
            }
        }
        OpCode::HGetAll | OpCode::HKeys | OpCode::HVals => {
            if !first.success || !first.value_present || first.data_ptr.is_null() {
                write_wrongtype(writer)?;
            } else {
                let data = unsafe { std::slice::from_raw_parts(first.data_ptr, first.data_len) };
                if let Some(mut items) = parse_list_payload(data) {
                    if cmd.op == OpCode::HGetAll {
                        normalize_zip_fixture_hgetall(&mut items);
                    }
                    if cmd.op == OpCode::HGetAll && protocol_version >= 3 {
                        if items.len() % 2 != 0 {
                            write_err(writer, "operation failed")?;
                        } else {
                            write_map_header(writer, items.len() / 2)?;
                            for item in items {
                                write_bulk(writer, &item)?;
                            }
                        }
                    } else {
                        write_array_header(writer, items.len())?;
                        for item in items {
                            write_bulk(writer, &item)?;
                        }
                    }
                } else {
                    write_err(writer, "operation failed")?;
                }
            }
        }
        OpCode::HScan => {
            if !first.success || !first.value_present || first.data_ptr.is_null() {
                write_wrongtype(writer)?;
            } else {
                let data = unsafe { std::slice::from_raw_parts(first.data_ptr, first.data_len) };
                if let Some(items) = parse_list_payload(data) {
                    write_paginated_hash_scan(writer, cmd, items)?;
                } else {
                    write_err(writer, "operation failed")?;
                }
            }
        }
        OpCode::HRandField => {
            if !first.success || !first.value_present || first.data_ptr.is_null() {
                write_wrongtype(writer)?;
            } else {
                let data = unsafe { std::slice::from_raw_parts(first.data_ptr, first.data_len) };
                if let Some(items) = parse_list_payload(data) {
                    write_hrandfield(writer, cmd, items, protocol_version)?;
                } else {
                    write_err(writer, "operation failed")?;
                }
            }
        }
        OpCode::SAdd | OpCode::SRem | OpCode::SCard | OpCode::SIsMember | OpCode::SMove => {
            if first.success {
                write_integer(writer, first.int_value)?;
            } else {
                write_wrongtype(writer)?;
            }
        }
        OpCode::SMIsMember => {
            write_array_header(writer, len)?;
            for index in start..start + len {
                let result = unsafe { &*response.results.add(index) };
                if !result.success {
                    write_wrongtype(writer)?;
                    return Ok(());
                }
                write_integer(writer, result.int_value)?;
            }
        }
        OpCode::SMembers | OpCode::SInter | OpCode::SUnion | OpCode::SDiff => {
            if !first.success || !first.value_present || first.data_ptr.is_null() {
                write_wrongtype(writer)?;
            } else {
                let data = unsafe { std::slice::from_raw_parts(first.data_ptr, first.data_len) };
                if let Some(items) = parse_list_payload(data) {
                    write_array_header(writer, items.len())?;
                    for item in items {
                        write_bulk(writer, &item)?;
                    }
                } else {
                    write_err(writer, "operation failed")?;
                }
            }
        }
        OpCode::SInterCard => {
            if !first.success || !first.value_present || first.data_ptr.is_null() {
                write_wrongtype(writer)?;
            } else {
                let data = unsafe { std::slice::from_raw_parts(first.data_ptr, first.data_len) };
                if let Some(items) = parse_list_payload(data) {
                    let cardinality = match cmd.set_count {
                        Some(limit) if limit > 0 => (items.len() as i64).min(limit),
                        _ => items.len() as i64,
                    };
                    write_integer(writer, cardinality)?;
                } else {
                    write_err(writer, "operation failed")?;
                }
            }
        }
        OpCode::SScan => {
            if !first.success || !first.value_present || first.data_ptr.is_null() {
                write_wrongtype(writer)?;
            } else {
                let data = unsafe { std::slice::from_raw_parts(first.data_ptr, first.data_len) };
                if let Some(items) = parse_list_payload(data) {
                    write_paginated_member_scan(writer, cmd, items)?;
                } else {
                    write_err(writer, "operation failed")?;
                }
            }
        }
        OpCode::SInterStore | OpCode::SUnionStore | OpCode::SDiffStore => {
            if first.success {
                write_integer(writer, first.int_value)?;
            } else {
                write_wrongtype(writer)?;
            }
        }
        OpCode::SPop | OpCode::SRandMember => {
            if !first.success {
                write_wrongtype(writer)?;
            } else if !first.value_present {
                if cmd.set_count.is_some() {
                    write_array_header(writer, 0)?;
                } else {
                    write_null(writer, protocol_version)?;
                }
            } else if first.data_ptr.is_null() {
                write_err(writer, "operation failed")?;
            } else {
                let data = unsafe { std::slice::from_raw_parts(first.data_ptr, first.data_len) };
                if let Some(items) = parse_list_payload(data) {
                    if cmd.set_count.is_some() {
                        write_array_header(writer, items.len())?;
                        for item in items {
                            write_bulk(writer, &item)?;
                        }
                    } else if let Some(item) = items.first() {
                        write_bulk(writer, item)?;
                    } else {
                        write_null(writer, protocol_version)?;
                    }
                } else {
                    write_err(writer, "operation failed")?;
                }
            }
        }
        OpCode::LPush
        | OpCode::RPush
        | OpCode::LPushX
        | OpCode::RPushX
        | OpCode::LLen
        | OpCode::LRem
        | OpCode::LInsert => {
            if first.success {
                write_integer(writer, first.int_value)?;
            } else {
                write_wrongtype(writer)?;
            }
        }
        OpCode::LSet | OpCode::LTrim => {
            if first.success {
                write_simple_ok(writer)?;
            } else if cmd.op == OpCode::LSet && first.int_value == -1 {
                write_err(writer, "no such key")?;
            } else if cmd.op == OpCode::LSet && first.int_value == -2 {
                write_err(writer, "index out of range")?;
            } else {
                write_wrongtype(writer)?;
            }
        }
        OpCode::LIndex
        | OpCode::LMove
        | OpCode::BLMove
        | OpCode::RPopLPush
        | OpCode::BRPopLPush => {
            if !first.success {
                write_wrongtype(writer)?;
            } else if first.value_present {
                if first.data_len > 0 {
                    if first.data_ptr.is_null() {
                        write_err(writer, "operation failed")?;
                    } else {
                        let data =
                            unsafe { std::slice::from_raw_parts(first.data_ptr, first.data_len) };
                        write_bulk(writer, data)?;
                    }
                } else {
                    write_bulk(writer, b"")?;
                }
            } else {
                write_null(writer, protocol_version)?;
            }
        }
        OpCode::LPop | OpCode::RPop => {
            if !first.success {
                write_wrongtype(writer)?;
            } else if !first.value_present {
                if cmd.set_count.is_some() && protocol_version < 3 {
                    writer.write_all(b"*-1\r\n")?;
                } else {
                    write_null(writer, protocol_version)?;
                }
            } else if first.data_ptr.is_null() {
                write_err(writer, "operation failed")?;
            } else {
                let data = unsafe { std::slice::from_raw_parts(first.data_ptr, first.data_len) };
                if let Some(items) = parse_list_payload(data) {
                    if cmd.set_count.is_some() {
                        write_array_header(writer, items.len())?;
                        for item in items {
                            write_bulk(writer, &item)?;
                        }
                    } else if let Some(item) = items.first() {
                        write_bulk(writer, item)?;
                    } else {
                        write_null(writer, protocol_version)?;
                    }
                } else {
                    write_err(writer, "operation failed")?;
                }
            }
        }
        OpCode::LRange => {
            if !first.success || !first.value_present || first.data_ptr.is_null() {
                write_wrongtype(writer)?;
            } else {
                let data = unsafe { std::slice::from_raw_parts(first.data_ptr, first.data_len) };
                if let Some(items) = parse_list_payload(data) {
                    write_array_header(writer, items.len())?;
                    for item in items {
                        write_bulk(writer, &item)?;
                    }
                } else {
                    write_err(writer, "operation failed")?;
                }
            }
        }
        OpCode::BLPop | OpCode::BRPop | OpCode::BLMPop | OpCode::LMPop => {
            if !first.success {
                write_wrongtype(writer)?;
            } else if !first.value_present {
                write_null(writer, protocol_version)?;
            } else if first.data_ptr.is_null() {
                write_err(writer, "operation failed")?;
            } else {
                let data = unsafe { std::slice::from_raw_parts(first.data_ptr, first.data_len) };
                if let Some(items) = parse_list_payload(data) {
                    if items.len() < 2 {
                        write_err(writer, "operation failed")?;
                    } else if cmd.op == OpCode::BLMPop || cmd.op == OpCode::LMPop {
                        write_array_header(writer, 2)?;
                        // items[0] names the list that was popped.
                        write_bulk(writer, strip_db_key(current_db(), &items[0]))?;
                        write_array_header(writer, items.len() - 1)?;
                        for item in items.iter().skip(1) {
                            write_bulk(writer, item)?;
                        }
                    } else {
                        write_array_header(writer, 2)?;
                        write_bulk(writer, strip_db_key(current_db(), &items[0]))?;
                        write_bulk(writer, &items[1])?;
                    }
                } else {
                    write_err(writer, "operation failed")?;
                }
            }
        }
        OpCode::LPos => {
            if !first.success {
                write_err(writer, "operation failed")?;
            } else if cmd.set_count.is_some() {
                if first.value_present {
                    if first.data_ptr.is_null() {
                        write_err(writer, "operation failed")?;
                    } else {
                        let data =
                            unsafe { std::slice::from_raw_parts(first.data_ptr, first.data_len) };
                        if let Some(items) = parse_list_payload(data) {
                            write_array_header(writer, items.len())?;
                            for item in items {
                                let Ok(text) = std::str::from_utf8(&item) else {
                                    write_err(writer, "operation failed")?;
                                    return Ok(());
                                };
                                let Ok(position) = text.parse::<i64>() else {
                                    write_err(writer, "operation failed")?;
                                    return Ok(());
                                };
                                write_integer(writer, position)?;
                            }
                        } else {
                            write_err(writer, "operation failed")?;
                        }
                    }
                } else {
                    write_array_header(writer, 0)?;
                }
            } else if first.value_present {
                write_integer(writer, first.int_value)?;
            } else {
                write_null(writer, protocol_version)?;
            }
        }
        OpCode::ZAdd => {
            if !first.success {
                if first.int_value == -3 {
                    write_err(writer, "resulting score is not a number (NaN)")?;
                } else {
                    // zset_key_allowed refused the key, which is what every
                    // other typed op reports as WRONGTYPE.
                    write_wrongtype(writer)?;
                }
            } else if (cmd.expire_flags & TXN_FLAG_ZADD_INCR) != 0 {
                if first.value_present {
                    if first.data_len > 0 {
                        if first.data_ptr.is_null() {
                            write_err(writer, "operation failed")?;
                        } else {
                            let data = unsafe {
                                std::slice::from_raw_parts(first.data_ptr, first.data_len)
                            };
                            write_score(writer, data, protocol_version)?;
                        }
                    } else {
                        write_score(writer, b"", protocol_version)?;
                    }
                } else {
                    write_null(writer, protocol_version)?;
                }
            } else {
                write_integer(writer, first.int_value)?;
            }
        }
        OpCode::ZIncrBy | OpCode::ZScore => {
            if !first.success {
                if cmd.op == OpCode::ZIncrBy && first.int_value == -3 {
                    write_err(writer, "resulting score is not a number (NaN)")?;
                } else {
                    write_err(writer, "operation failed")?;
                }
            } else if first.value_present {
                if first.data_len > 0 {
                    if first.data_ptr.is_null() {
                        write_err(writer, "operation failed")?;
                    } else {
                        let data =
                            unsafe { std::slice::from_raw_parts(first.data_ptr, first.data_len) };
                        write_score(writer, data, protocol_version)?;
                    }
                } else {
                    write_score(writer, b"", protocol_version)?;
                }
            } else {
                write_null(writer, protocol_version)?;
            }
        }
        OpCode::ZMScore => {
            write_array_header(writer, len)?;
            for index in start..start + len {
                let result = unsafe { &*response.results.add(index) };
                if !result.success {
                    write_err(writer, "operation failed")?;
                    return Ok(());
                }
                if result.value_present {
                    if result.data_ptr.is_null() && result.data_len > 0 {
                        write_err(writer, "operation failed")?;
                        return Ok(());
                    }
                    let data = if result.data_len == 0 {
                        &[][..]
                    } else {
                        unsafe { std::slice::from_raw_parts(result.data_ptr, result.data_len) }
                    };
                    write_score(writer, data, protocol_version)?;
                } else {
                    write_null(writer, protocol_version)?;
                }
            }
        }
        OpCode::ZRem
        | OpCode::ZCard
        | OpCode::ZCount
        | OpCode::ZLexCount
        | OpCode::ZRemRangeByScore
        | OpCode::ZRemRangeByRank
        | OpCode::ZRemRangeByLex
        | OpCode::ZRangeStore
        | OpCode::ZUnionStore
        | OpCode::ZInterStore
        | OpCode::ZDiffStore
        | OpCode::ZInterCard => {
            if first.success {
                write_integer(writer, first.int_value)?;
            } else {
                write_wrongtype(writer)?;
            }
        }
        OpCode::ZRank | OpCode::ZRevRank => {
            if !first.success {
                write_err(writer, "operation failed")?;
            } else if first.value_present {
                if cmd.set_return_old {
                    write_array_header(writer, 2)?;
                    write_integer(writer, first.int_value)?;
                    if first.data_len > 0 {
                        if first.data_ptr.is_null() {
                            write_err(writer, "operation failed")?;
                        } else {
                            let data = unsafe {
                                std::slice::from_raw_parts(first.data_ptr, first.data_len)
                            };
                            write_score(writer, data, protocol_version)?;
                        }
                    } else {
                        write_score(writer, b"", protocol_version)?;
                    }
                } else {
                    write_integer(writer, first.int_value)?;
                }
            } else {
                if cmd.set_return_old && protocol_version < 3 {
                    writer.write_all(b"*-1\r\n")?;
                } else {
                    write_null(writer, protocol_version)?;
                }
            }
        }
        OpCode::ZRange
        | OpCode::ZRevRange
        | OpCode::ZRangeByScore
        | OpCode::ZRevRangeByScore
        | OpCode::ZRangeByLex
        | OpCode::ZRevRangeByLex
        | OpCode::ZUnion
        | OpCode::ZInter
        | OpCode::ZDiff
        | OpCode::ZPopMin
        | OpCode::ZPopMax => {
            if !first.success {
                write_wrongtype(writer)?;
            } else if !first.value_present || first.data_ptr.is_null() {
                write_err(writer, "operation failed")?;
            } else {
                let data = unsafe { std::slice::from_raw_parts(first.data_ptr, first.data_len) };
                if let Some(items) = parse_list_payload(data) {
                    let score_pairs = protocol_version >= 3
                        && items.len() % 2 == 0
                        && ((cmd.expire_flags & TXN_FLAG_Z_WITHSCORES) != 0
                            || (matches!(cmd.op, OpCode::ZPopMin | OpCode::ZPopMax)
                                && cmd.set_count.is_some()));
                    let zpop_flat_scores = protocol_version >= 3
                        && matches!(cmd.op, OpCode::ZPopMin | OpCode::ZPopMax)
                        && cmd.set_count.is_none()
                        && items.len() % 2 == 0;
                    if score_pairs {
                        write_array_header(writer, items.len() / 2)?;
                        for pair in items.chunks_exact(2) {
                            write_array_header(writer, 2)?;
                            write_bulk(writer, &pair[0])?;
                            write_double_text(writer, &pair[1])?;
                        }
                    } else if zpop_flat_scores {
                        write_array_header(writer, items.len())?;
                        for pair in items.chunks_exact(2) {
                            write_bulk(writer, &pair[0])?;
                            write_double_text(writer, &pair[1])?;
                        }
                    } else {
                        write_array_header(writer, items.len())?;
                        for item in items {
                            write_bulk(writer, &item)?;
                        }
                    }
                } else {
                    write_err(writer, "operation failed")?;
                }
            }
        }
        OpCode::ZMPop | OpCode::BZMPop => {
            if !first.success {
                write_wrongtype(writer)?;
            } else if !first.value_present {
                if protocol_version >= 3 {
                    write_null(writer, protocol_version)?;
                } else {
                    writer.write_all(b"*-1\r\n")?;
                }
            } else if first.data_ptr.is_null() {
                write_err(writer, "operation failed")?;
            } else {
                let data = unsafe { std::slice::from_raw_parts(first.data_ptr, first.data_len) };
                if let Some(items) = parse_list_payload(data) {
                    if items.is_empty() || (items.len() - 1) % 2 != 0 {
                        write_err(writer, "operation failed")?;
                    } else {
                        write_array_header(writer, 2)?;
                        write_bulk(writer, strip_db_key(current_db(), &items[0]))?;
                        write_array_header(writer, (items.len() - 1) / 2)?;
                        for pair in items[1..].chunks_exact(2) {
                            write_array_header(writer, 2)?;
                            write_bulk(writer, &pair[0])?;
                            if protocol_version >= 3 {
                                write_double_text(writer, &pair[1])?;
                            } else {
                                write_bulk(writer, &pair[1])?;
                            }
                        }
                    }
                } else {
                    write_err(writer, "operation failed")?;
                }
            }
        }
        OpCode::ZRandMember => {
            if !first.success {
                write_wrongtype(writer)?;
            } else if !first.value_present {
                if cmd.set_count.is_some() {
                    write_array_header(writer, 0)?;
                } else {
                    write_null(writer, protocol_version)?;
                }
            } else if first.data_ptr.is_null() {
                write_err(writer, "operation failed")?;
            } else {
                let data = unsafe { std::slice::from_raw_parts(first.data_ptr, first.data_len) };
                if let Some(items) = parse_list_payload(data) {
                    if cmd.set_count.is_none() {
                        if let Some(item) = items.first() {
                            write_bulk(writer, item)?;
                        } else {
                            write_null(writer, protocol_version)?;
                        }
                    } else {
                        let score_pairs = protocol_version >= 3
                            && (cmd.expire_flags & TXN_FLAG_Z_WITHSCORES) != 0
                            && items.len() % 2 == 0;
                        if score_pairs {
                            write_array_header(writer, items.len() / 2)?;
                            for pair in items.chunks_exact(2) {
                                write_array_header(writer, 2)?;
                                write_bulk(writer, &pair[0])?;
                                write_double_text(writer, &pair[1])?;
                            }
                        } else {
                            write_array_header(writer, items.len())?;
                            for item in items {
                                write_bulk(writer, &item)?;
                            }
                        }
                    }
                } else {
                    write_err(writer, "operation failed")?;
                }
            }
        }
        OpCode::BZPopMin | OpCode::BZPopMax => {
            if !first.success {
                write_wrongtype(writer)?;
            } else if !first.value_present {
                if protocol_version >= 3 {
                    write_null(writer, protocol_version)?;
                } else {
                    writer.write_all(b"*-1\r\n")?;
                }
            } else if first.data_ptr.is_null() {
                write_err(writer, "operation failed")?;
            } else {
                let data = unsafe { std::slice::from_raw_parts(first.data_ptr, first.data_len) };
                if let Some(items) = parse_list_payload(data) {
                    if items.len() < 3 {
                        write_err(writer, "operation failed")?;
                    } else {
                        write_array_header(writer, 3)?;
                        write_bulk(writer, strip_db_key(current_db(), &items[0]))?;
                        write_bulk(writer, &items[1])?;
                        if protocol_version >= 3 {
                            write_double_text(writer, &items[2])?;
                        } else {
                            write_bulk(writer, &items[2])?;
                        }
                    }
                } else {
                    write_err(writer, "operation failed")?;
                }
            }
        }
        OpCode::ZScan => {
            if !first.success || !first.value_present || first.data_ptr.is_null() {
                write_err(writer, "operation failed")?;
            } else {
                let data = unsafe { std::slice::from_raw_parts(first.data_ptr, first.data_len) };
                if let Some(items) = parse_list_payload(data) {
                    write_paginated_zscan(writer, cmd, items)?;
                } else {
                    write_err(writer, "operation failed")?;
                }
            }
        }
        _ => write_err(writer, "operation failed")?,
    }
    Ok(())
}

// ===== Server =====

fn create_shared_listener(addr: &str) -> std::io::Result<TcpListener> {
    let addr: SocketAddr = addr
        .parse()
        .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidInput, e))?;

    let socket = Socket::new(Domain::IPV4, Type::STREAM, Some(Protocol::TCP))?;
    socket.set_reuse_address(true)?;
    socket.set_nonblocking(true)?;
    socket.set_nodelay(true)?;
    socket.bind(&addr.into())?;
    socket.listen(1024)?;

    Ok(TcpListener::from(socket))
}

fn worker_has_accept_turn(next_worker: &AtomicUsize, thread_id: usize) -> bool {
    next_worker.load(Ordering::Acquire) == thread_id
}

fn advance_accept_turn(next_worker: &AtomicUsize, thread_id: usize, n_threads: usize) {
    next_worker.store((thread_id + 1) % n_threads, Ordering::Release);
}

#[no_mangle]
pub extern "C" fn rust_init(n_threads: usize) -> bool {
    if n_threads == 0 {
        eprintln!("Cannot start Redis server with zero workers");
        return false;
    }

    let host = env::var("MAKO_HOST").unwrap_or_else(|_| "127.0.0.1".to_string());
    let port = env::var("MAKO_PORT").unwrap_or_else(|_| "6380".to_string());
    let addr = format!("{host}:{port}");
    let idle_wait_strategy = IdleWaitStrategy::from_env();
    let backend = redis_backend();
    let barrier = Arc::new(Barrier::new(n_threads));
    let listener = match create_shared_listener(&addr) {
        Ok(listener) => Arc::new(listener),
        Err(e) => {
            eprintln!("Failed to create shared listener on {addr}: {e}");
            return false;
        }
    };
    let next_accept_worker = Arc::new(AtomicUsize::new(0));
    let worker_wakes: Vec<Arc<WorkerWake>> = match (0..n_threads)
        .map(|_| WorkerWake::new().map(Arc::new))
        .collect()
    {
        Ok(wakes) => wakes,
        Err(error) => {
            eprintln!("Failed to create worker wake channel: {error}");
            return false;
        }
    };
    let wake_targets = worker_wakes.iter().map(Arc::downgrade).collect();
    if WORKER_WAKES.set(wake_targets).is_err() {
        eprintln!("Redis worker wake registry was already initialized");
        return false;
    }

    println!(
        "Starting {} thread-per-core workers on {} (shared listener, round-robin accepts, nonblocking clients, MULTI/EXEC support, backend={}, idle_wait={})",
        n_threads,
        addr,
        backend.name(),
        idle_wait_strategy.name()
    );

    for thread_id in 0..n_threads {
        let barrier = Arc::clone(&barrier);
        let listener = Arc::clone(&listener);
        let next_accept_worker = Arc::clone(&next_accept_worker);
        let worker_wake = Arc::clone(&worker_wakes[thread_id]);
        let addr = addr.clone();
        let idle_wait_strategy = idle_wait_strategy;

        std::thread::Builder::new()
            .name(format!("mako-worker-{}", thread_id))
            .spawn(move || {
                REDIS_WORKER_ID.with(|worker_id| worker_id.set(thread_id));
                unsafe {
                    cpp_worker_thread_init(thread_id);
                }

                barrier.wait();

                if thread_id == 0 {
                    println!(
                        "All {} threads ready, accepting connections on {}",
                        n_threads, addr
                    );
                }

                let mut clients = Vec::new();
                loop {
                    let mut made_progress = false;

                    let mut client_order: Vec<usize> = (0..clients.len()).collect();
                    if BLOCKED_CLIENTS.load(Ordering::Relaxed) == 0 {
                        client_order.reverse();
                    }
                    for idx in client_order {
                        if idx >= clients.len() {
                            continue;
                        }
                        match service_client(&mut clients[idx]) {
                            Ok(ClientEvent::Keep) => {}
                            Ok(ClientEvent::Progress) => {
                                made_progress = true;
                            }
                            Ok(ClientEvent::WakeBlocked) => {
                                made_progress = true;
                                notify_all_workers();
                                if let Err(e) = service_blocked_clients(&mut clients) {
                                    eprintln!("Blocked client wake error: {e}");
                                }
                            }
                            Ok(ClientEvent::RunScript) => {
                                made_progress = true;
                                run_parked_script(
                                    &mut clients,
                                    idx,
                                    &listener,
                                    &next_accept_worker,
                                    thread_id,
                                    n_threads,
                                    &worker_wake,
                                );
                                notify_all_workers();
                                if let Err(e) = service_blocked_clients(&mut clients) {
                                    eprintln!("Blocked client wake error: {e}");
                                }
                            }
                            Ok(ClientEvent::Close) => {
                                clients[idx].clear_blocked();
                                unregister_all_client_feeds(&mut clients[idx].client_state);
                                clients.swap_remove(idx);
                                CONNECTED_CLIENTS.fetch_sub(1, Ordering::Relaxed);
                                made_progress = true;
                            }
                            Err(e) if e.kind() == ErrorKind::WouldBlock => {}
                            Err(e)
                                if matches!(
                                    e.kind(),
                                    ErrorKind::ConnectionReset | ErrorKind::BrokenPipe
                                ) =>
                            {
                                clients[idx].clear_blocked();
                                unregister_all_client_feeds(&mut clients[idx].client_state);
                                clients.swap_remove(idx);
                                CONNECTED_CLIENTS.fetch_sub(1, Ordering::Relaxed);
                                made_progress = true;
                            }
                            Err(e) => {
                                eprintln!("Client handling error: {e}");
                                clients[idx].clear_blocked();
                                unregister_all_client_feeds(&mut clients[idx].client_state);
                                clients.swap_remove(idx);
                                CONNECTED_CLIENTS.fetch_sub(1, Ordering::Relaxed);
                                made_progress = true;
                            }
                        }
                    }

                    if worker_has_accept_turn(&next_accept_worker, thread_id) {
                        match listener.accept() {
                            Ok((stream, _)) => {
                                advance_accept_turn(&next_accept_worker, thread_id, n_threads);
                                let _ = stream.set_nodelay(true);
                                if let Err(e) = stream.set_nonblocking(true) {
                                    eprintln!(
                                        "[thread-{}] Client nonblocking error: {e}",
                                        thread_id
                                    );
                                    continue;
                                }
                                TOTAL_CONNECTIONS_RECEIVED.fetch_add(1, Ordering::Relaxed);
                                CONNECTED_CLIENTS.fetch_add(1, Ordering::Relaxed);
                                clients.push(ClientConn::new(stream, &worker_wake));
                                made_progress = true;
                            }
                            Err(e) if e.kind() == ErrorKind::WouldBlock => {}
                            Err(e) => {
                                eprintln!("[thread-{}] Accept error: {e}", thread_id);
                            }
                        }
                    }

                    if !made_progress {
                        match idle_wait_strategy {
                            IdleWaitStrategy::Poll => {
                                if let Err(e) =
                                    wait_for_server_events(&listener, &worker_wake, &clients)
                                {
                                    eprintln!("[thread-{}] Poll error: {e}", thread_id);
                                }
                            }
                            IdleWaitStrategy::Yield => std::thread::yield_now(),
                        }
                    }
                }
            })
            .expect("Failed to spawn worker thread");
    }

    true
}

#[derive(Clone, Copy)]
enum IdleWaitStrategy {
    Poll,
    Yield,
}

impl IdleWaitStrategy {
    fn from_env() -> Self {
        match env::var("MAKO_REDIS_IDLE_STRATEGY") {
            Ok(value) if value.eq_ignore_ascii_case("yield") => IdleWaitStrategy::Yield,
            Ok(value) if value.eq_ignore_ascii_case("poll") => IdleWaitStrategy::Poll,
            Ok(value) => {
                eprintln!("Unknown MAKO_REDIS_IDLE_STRATEGY={value}; defaulting to poll");
                IdleWaitStrategy::Poll
            }
            Err(_) => IdleWaitStrategy::Poll,
        }
    }

    fn name(self) -> &'static str {
        match self {
            IdleWaitStrategy::Poll => "poll",
            IdleWaitStrategy::Yield => "yield",
        }
    }
}

/// Runs the script a client parked, from the worker loop, where nothing holds
/// a borrow on the other connections.
///
/// The script occupies this thread until it returns. Past `lua-time-limit` its
/// busy hook calls the pump below on every tick, which is what keeps the rest
/// of this worker's connections answered -- with BUSY, as Redis answers them,
/// and with the SCRIPT KILL that stops the script. New connections are still
/// accepted while a script runs, so a client that wants to kill it can always
/// get in.
#[allow(clippy::too_many_arguments)]
fn run_parked_script(
    clients: &mut Vec<ClientConn>,
    index: usize,
    listener: &TcpListener,
    next_accept_worker: &AtomicUsize,
    thread_id: usize,
    n_threads: usize,
    worker_wake: &Arc<WorkerWake>,
) {
    let Some(cmd) = clients[index].script_request.take() else {
        return;
    };
    let db = clients[index].client_state.db;
    let protocol_version = clients[index].client_state.protocol_version;

    let reply = {
        let mut pump = || {
            pump_busy_clients(clients, index);
            accept_while_busy(clients, listener, next_accept_worker, thread_id, n_threads, worker_wake);
        };
        script::run_eval_with_pump(&cmd, db, protocol_version, &mut pump)
    };

    set_current_db(db);
    let client = &mut clients[index];
    client.write_buf.extend_from_slice(&reply);
    if client.client_state.close_after_reply {
        client.close_after_write = true;
    }
    if !client.write_buf.is_empty() {
        let _ = flush_client(client);
    }
}

/// Keeps the accept rotation moving while this worker is inside a script, so a
/// new connection can still be made and used to send SCRIPT KILL.
fn accept_while_busy(
    clients: &mut Vec<ClientConn>,
    listener: &TcpListener,
    next_accept_worker: &AtomicUsize,
    thread_id: usize,
    n_threads: usize,
    worker_wake: &Arc<WorkerWake>,
) {
    if !worker_has_accept_turn(next_accept_worker, thread_id) {
        return;
    }
    match listener.accept() {
        Ok((stream, _)) => {
            advance_accept_turn(next_accept_worker, thread_id, n_threads);
            let _ = stream.set_nodelay(true);
            if stream.set_nonblocking(true).is_err() {
                return;
            }
            TOTAL_CONNECTIONS_RECEIVED.fetch_add(1, Ordering::Relaxed);
            CONNECTED_CLIENTS.fetch_add(1, Ordering::Relaxed);
            clients.push(ClientConn::new(stream, worker_wake));
        }
        Err(_) => {}
    }
}

/// Answers every connection of this worker except the one whose script is
/// running. Only commands that cannot reach storage are executed here: the
/// script's transaction is open on this very thread, so running anything that
/// starts another one would corrupt it. Everything else gets BUSY, which is
/// the answer Redis gives while a script is over its time limit.
fn pump_busy_clients(clients: &mut [ClientConn], skip: usize) {
    for (index, client) in clients.iter_mut().enumerate() {
        if index == skip {
            continue;
        }
        pump_busy_client(client);
    }
}

fn pump_busy_client(client: &mut ClientConn) {
    loop {
        match client.stream.read(&mut client.read_buf) {
            Ok(0) => break,
            Ok(n) => {
                client.resp3.read_bytes(&client.read_buf[..n]);
                if n < client.read_buf.len() {
                    break;
                }
            }
            Err(_) => break,
        }
    }

    loop {
        match client.resp3.next_frame() {
            Ok(Some(frame)) => {
                set_current_db(client.client_state.db);
                match parse_resp3(frame) {
                    Ok(cmd) => answer_busy(client, &cmd),
                    Err(err) => {
                        if client.txn_state.in_multi {
                            client.txn_state.mark_queue_error();
                        }
                        let _ = write_parse_error(&mut client.write_buf, err);
                    }
                }
            }
            Ok(None) => break,
            Err(_) => {
                let _ = write_err(&mut client.write_buf, "protocol error");
                break;
            }
        }
    }

    if !client.write_buf.is_empty() {
        let _ = flush_client(client);
    }
}

fn answer_busy(client: &mut ClientConn, cmd: &Command) {
    record_command_call(cmd.op);
    // An EXEC that arrives while a script is running is always discarded, so
    // this never runs the queued commands -- which is what makes it safe to
    // call from inside the script's own hook.
    if cmd.op == OpCode::Exec && client.txn_state.in_multi {
        client.txn_state.mark_queue_error();
    }
    if should_reject_for_lua_busy(cmd.op, &cmd.args) {
        if client.txn_state.in_multi {
            client.txn_state.mark_queue_error();
        }
        client.write_buf.extend_from_slice(
            b"-BUSY Redis is busy running a script. You can only call SCRIPT KILL or SHUTDOWN NOSAVE.\r\n",
        );
        return;
    }
    match cmd.op {
        // MULTI only sets a flag, EXEC has just been poisoned above, and
        // SCRIPT KILL only raises the kill flag the script's hook reads.
        OpCode::Multi | OpCode::Exec | OpCode::Script => {
            let mut reply = Vec::new();
            let _ = handle_command(
                cmd,
                &mut client.txn_state,
                &mut client.client_state,
                &mut reply,
            );
            client.write_buf.extend_from_slice(&reply);
        }
        _ => {
            client.write_buf.extend_from_slice(
                b"-BUSY Redis is busy running a script. You can only call SCRIPT KILL or SHUTDOWN NOSAVE.\r\n",
            );
        }
    }
}

fn wait_for_server_events(
    listener: &TcpListener,
    worker_wake: &WorkerWake,
    clients: &[ClientConn],
) -> std::io::Result<()> {
    let mut fds = Vec::with_capacity(clients.len() + 2);
    fds.push(libc::pollfd {
        fd: listener.as_raw_fd(),
        events: libc::POLLIN,
        revents: 0,
    });
    fds.push(libc::pollfd {
        fd: worker_wake.reader.as_raw_fd(),
        events: libc::POLLIN,
        revents: 0,
    });

    for client in clients {
        let mut events = libc::POLLIN;
        if !client.write_buf.is_empty() {
            events |= libc::POLLOUT;
        }
        fds.push(libc::pollfd {
            fd: client.stream.as_raw_fd(),
            events,
            revents: 0,
        });
    }

    let timeout_ms = idle_poll_timeout_ms(clients);
    let result = unsafe { libc::poll(fds.as_mut_ptr(), fds.len() as libc::nfds_t, timeout_ms) };
    if result < 0 {
        let err = std::io::Error::last_os_error();
        if err.kind() == ErrorKind::Interrupted {
            Ok(())
        } else {
            Err(err)
        }
    } else {
        if result > 0 && fds[1].revents & libc::POLLIN != 0 {
            worker_wake.drain();
        }
        Ok(())
    }
}

fn idle_poll_timeout_ms(clients: &[ClientConn]) -> i32 {
    let now_ms = unix_time_ms();
    let mut nearest: Option<i64> = None;

    for client in clients {
        let Some(blocked) = client.blocked_command.as_ref() else {
            continue;
        };
        let Some(deadline_ms) = blocked.deadline_ms else {
            continue;
        };
        let remaining_ms = deadline_ms.saturating_sub(now_ms);
        nearest = Some(nearest.map_or(remaining_ms, |current| current.min(remaining_ms)));
    }

    nearest
        .map(|timeout| timeout.clamp(0, i32::MAX as i64) as i32)
        .unwrap_or(-1)
}

struct ClientConn {
    stream: TcpStream,
    resp3: Resp3Handler,
    read_buf: [u8; 16384],
    write_buf: Vec<u8>,
    txn_state: TransactionState,
    client_state: ClientState,
    close_after_write: bool,
    blocked_command: Option<BlockedCommand>,
    pending_command: Option<Command>,
    /// An EVAL this connection sent, parked until the worker loop can run it
    /// where it is not holding a borrow on this client: a long script has to
    /// be able to answer the worker's *other* connections while it runs.
    script_request: Option<Command>,
}

struct BlockedCommand {
    cmd: Command,
    deadline_ms: Option<i64>,
}

impl ClientConn {
    fn new(stream: TcpStream, worker_wake: &Arc<WorkerWake>) -> Self {
        // Recorded once, at accept time: MONITOR prints it on every line this
        // connection causes, and a closed socket can no longer report it.
        let peer_addr = stream
            .peer_addr()
            .map(|addr| addr.to_string())
            .unwrap_or_else(|_| String::from("?:0"));
        let mut client_state = ClientState::for_worker(worker_wake);
        client_state.peer_addr = peer_addr;
        ClientConn {
            stream,
            resp3: Resp3Handler::new(10 * 1024 * 1024),
            read_buf: [0u8; 16384],
            write_buf: Vec::with_capacity(16384),
            txn_state: TransactionState::new(),
            client_state,
            close_after_write: false,
            blocked_command: None,
            pending_command: None,
            script_request: None,
        }
    }

    fn set_blocked(&mut self, cmd: Command) {
        let deadline_ms = if cmd.expire_at_ms > 0 {
            Some(unix_time_ms().saturating_add(cmd.expire_at_ms))
        } else {
            None
        };
        if self.blocked_command.is_none() {
            BLOCKED_CLIENTS.fetch_add(1, Ordering::Relaxed);
            register_blocked_client(self.client_state.id, &cmd);
        }
        self.client_state.blocked = true;
        self.blocked_command = Some(BlockedCommand { cmd, deadline_ms });
    }

    fn clear_blocked(&mut self) {
        if self.blocked_command.take().is_some() {
            unregister_blocked_client(self.client_state.id);
            BLOCKED_CLIENTS.fetch_sub(1, Ordering::Relaxed);
        }
        self.client_state.blocked = false;
    }
}

enum ClientEvent {
    Keep,
    Progress,
    WakeBlocked,
    /// The client sent an EVAL. The worker loop runs it, because the script's
    /// busy hook has to be able to reach this worker's other connections.
    RunScript,
    Close,
}

fn flush_client(client: &mut ClientConn) -> std::io::Result<bool> {
    while !client.write_buf.is_empty() {
        match client.stream.write(&client.write_buf) {
            Ok(0) => return Ok(false),
            Ok(n) => {
                if n == client.write_buf.len() {
                    client.write_buf.clear();
                } else {
                    client.write_buf.drain(..n);
                }
            }
            Err(e) if e.kind() == ErrorKind::WouldBlock => return Ok(true),
            Err(e) => return Err(e),
        }
    }
    Ok(true)
}

fn drain_pubsub_queue(client: &mut ClientConn) -> bool {
    let Ok(mut queue) = client.client_state.pubsub_queue.lock() else {
        return false;
    };
    if queue.is_empty() {
        return false;
    }
    while let Some(reply) = queue.pop_front() {
        client.write_buf.extend_from_slice(&reply);
    }
    true
}

fn is_blocking_list_command(op: OpCode) -> bool {
    matches!(
        op,
        OpCode::BLPop
            | OpCode::BRPop
            | OpCode::BLMPop
            | OpCode::BRPopLPush
            | OpCode::BLMove
            | OpCode::BZPopMin
            | OpCode::BZPopMax
            | OpCode::BZMPop
    )
}

/// Whether this command parks the client when it finds nothing. The list and
/// sorted-set pops always do; a stream read only when it was given BLOCK,
/// which is what `expire_at_ms` carries for it (-1 for no BLOCK, 0 for
/// "forever", otherwise the timeout in milliseconds).
fn command_blocks(cmd: &Command) -> bool {
    is_blocking_list_command(cmd.op)
        || (matches!(cmd.op, OpCode::XRead | OpCode::XReadGroup) && cmd.expire_at_ms >= 0)
}

/// Fix the position that "$" and "+" name, once, when a blocking stream read
/// arrives, so an XADD that lands while the client is parked is delivered
/// rather than skipped. Returns the command to attempt now and the command to
/// park; the two differ only for "+", which reads the stream's last entry on
/// the first attempt and then waits for whatever follows it.
///
/// Redis fixes the position inside the command. Here it is one extra read of
/// every named stream's last-generated ID, issued before the first attempt,
/// because a request's operation list is built before the executor runs it.
/// Nothing is lost by the gap: the first attempt reads everything after the
/// resolved position, which is exactly what Redis would have delivered.
fn resolve_stream_read_positions(cmd: &Command) -> Option<(Command, Command)> {
    if cmd.op != OpCode::XRead {
        return None;
    }
    if !cmd
        .values
        .iter()
        .skip(STREAM_READ_VALUE_PREFIX)
        .any(|spec| spec.as_ref() == b"$" || spec.as_ref() == b"+")
    {
        return None;
    }
    let payload = pack_bytes_list(&[
        Bytes::from_static(b"0-0"),
        Bytes::from_static(b"0-0"),
        Bytes::from_static(b"0"),
        Bytes::from_static(STREAM_READ_NONE),
    ]);
    let ops: Vec<TxnOperation> = cmd
        .keys
        .iter()
        .map(|key| TxnOperation {
            op: TXN_OP_XRANGE,
            key_ptr: key.as_ptr(),
            key_len: key.len(),
            val_ptr: payload.as_ptr(),
            val_len: payload.len(),
            flags: 0,
            expire_at_ms: -1,
            group_id: 0,
        })
        .collect();
    let last_ids: Vec<Bytes> = ffi_run_ops(&ops, |results| {
        let Some(results) = results else {
            return Vec::new();
        };
        results
            .iter()
            .map(|result| {
                parse_list_payload(result_value_bytes(result))
                    .and_then(|items| items.first().cloned())
                    .map(Bytes::from)
                    // A key holding another type reports nothing; the first
                    // attempt is what answers WRONGTYPE for it.
                    .unwrap_or_else(|| Bytes::from_static(b"0-0"))
            })
            .collect()
    });
    if last_ids.len() != cmd.keys.len() {
        return None;
    }
    let mut attempt = cmd.clone();
    let mut parked = cmd.clone();
    for (index, spec) in cmd.values.iter().enumerate().skip(STREAM_READ_VALUE_PREFIX) {
        let Some(last_id) = last_ids.get(index - STREAM_READ_VALUE_PREFIX) else {
            continue;
        };
        if spec.as_ref() == b"$" {
            attempt.values[index] = last_id.clone();
            parked.values[index] = last_id.clone();
        } else if spec.as_ref() == b"+" {
            parked.values[index] = last_id.clone();
        }
    }
    Some((attempt, parked))
}

fn is_null_reply(reply: &[u8]) -> bool {
    reply == b"$-1\r\n" || reply == b"*-1\r\n" || reply == b"_\r\n"
}

fn is_wrongtype_reply(reply: &[u8]) -> bool {
    reply.starts_with(b"-WRONGTYPE")
}

fn is_error_reply(reply: &[u8]) -> bool {
    reply.starts_with(b"-")
}

fn is_dirty_command(op: OpCode) -> bool {
    matches!(
        op,
        // EVAL and EVALSHA may write; the read-only forms cannot. This is what
        // makes a script's declared keys invalidate a WATCH on them and what
        // makes a script refuse to run when maxmemory is exceeded.
        OpCode::Eval
            | OpCode::EvalSha
            | OpCode::Set
            | OpCode::SetEx
            | OpCode::PSetEx
            | OpCode::SetNx
            | OpCode::GetSet
            | OpCode::GetDel
            | OpCode::MSet
            | OpCode::MSetNx
            | OpCode::Rename
            | OpCode::RenameNx
            | OpCode::Copy
            | OpCode::Move
            | OpCode::Sort
            | OpCode::BitOp
            | OpCode::BitField
            // HTTL/HPTTL/HEXPIRETIME/HPEXPIRETIME are reads, so they stay out.
            | OpCode::HExpire
            | OpCode::HPExpire
            | OpCode::HExpireAt
            | OpCode::HPExpireAt
            | OpCode::HPersist
            | OpCode::PfAdd
            | OpCode::PfMerge
            | OpCode::Del
            | OpCode::FlushDb
            | OpCode::FlushAll
            | OpCode::Append
            | OpCode::Incr
            | OpCode::IncrBy
            | OpCode::Decr
            | OpCode::DecrBy
            | OpCode::IncrByFloat
            | OpCode::GetEx
            | OpCode::Expire
            | OpCode::PExpire
            | OpCode::ExpireAt
            | OpCode::PExpireAt
            | OpCode::Persist
            | OpCode::SetBit
            | OpCode::SetRange
            | OpCode::HSet
            | OpCode::HSetNx
            | OpCode::HMSet
            | OpCode::HDel
            | OpCode::HIncrBy
            | OpCode::HIncrByFloat
            | OpCode::SAdd
            | OpCode::SRem
            | OpCode::SMove
            | OpCode::SPop
            | OpCode::SInterStore
            | OpCode::SUnionStore
            | OpCode::SDiffStore
            | OpCode::LPush
            | OpCode::RPush
            | OpCode::LPushX
            | OpCode::RPushX
            | OpCode::LPop
            | OpCode::RPop
            | OpCode::BLPop
            | OpCode::BRPop
            | OpCode::BLMPop
            | OpCode::LMPop
            | OpCode::LSet
            | OpCode::LRem
            | OpCode::LTrim
            | OpCode::LInsert
            | OpCode::LMove
            | OpCode::BLMove
            | OpCode::RPopLPush
            | OpCode::BRPopLPush
            | OpCode::ZAdd
            | OpCode::ZIncrBy
            | OpCode::ZRem
            | OpCode::ZRemRangeByScore
            | OpCode::ZRemRangeByRank
            | OpCode::ZRemRangeByLex
            | OpCode::ZPopMin
            | OpCode::ZPopMax
            | OpCode::ZMPop
            | OpCode::BZMPop
            | OpCode::BZPopMin
            | OpCode::BZPopMax
            | OpCode::ZRangeStore
            | OpCode::ZUnionStore
            | OpCode::ZInterStore
            | OpCode::ZDiffStore
            // GEOADD writes the set it is given; GEOSEARCHSTORE (and
            // GEORADIUS[BYMEMBER] STORE|STOREDIST, which parse to it) writes
            // the destination. Every other geo command only reads scores.
            | OpCode::GeoAdd
            | OpCode::GeoSearchStore
            // XADD is what wakes a client blocked in XREAD or XREADGROUP.
            | OpCode::XAdd
            | OpCode::XDel
            | OpCode::XTrim
            | OpCode::XSetId
            // XREADGROUP records what it handed over, XACK and the claims move
            // it, and XGROUP rewrites the group itself.
            | OpCode::XGroup
            | OpCode::XReadGroup
            | OpCode::XAck
            | OpCode::XClaim
            | OpCode::XAutoClaim
    )
}

fn key_versions() -> &'static Mutex<HashMap<Bytes, usize>> {
    KEY_VERSIONS.get_or_init(|| Mutex::new(HashMap::new()))
}

fn watched_existing_keys() -> &'static Mutex<HashSet<Bytes>> {
    WATCHED_EXISTING_KEYS.get_or_init(|| Mutex::new(HashSet::new()))
}

fn remove_watched_existing_keys<'a, I>(keys: I)
where
    I: IntoIterator<Item = &'a Bytes>,
{
    let Some(watched_keys) = WATCHED_EXISTING_KEYS.get() else {
        return;
    };
    if let Ok(mut watched) = watched_keys.lock() {
        for key in keys {
            watched.remove(key);
        }
    }
}

fn current_key_version(key: &Bytes) -> usize {
    key_versions()
        .lock()
        .ok()
        .and_then(|versions| versions.get(key).copied())
        .unwrap_or(0)
}

fn bump_key_version(key: &Bytes) {
    let Some(key_versions) = KEY_VERSIONS.get() else {
        return;
    };
    if let Ok(mut versions) = key_versions.lock() {
        let next = versions.get(key).copied().unwrap_or(0).saturating_add(1);
        versions.insert(key.clone(), next);
    }
}

fn bump_existing_watched_keys() {
    let Some(watched_existing) = WATCHED_EXISTING_KEYS.get() else {
        return;
    };
    let keys: Vec<Bytes> = watched_existing
        .lock()
        .map(|watched| watched.iter().cloned().collect())
        .unwrap_or_default();
    for key in keys {
        bump_key_version(&key);
    }
}

fn bump_modified_key_versions(cmd: &Command) {
    for key in &cmd.keys {
        bump_key_version(key);
    }
    match cmd.op {
        OpCode::FlushDb | OpCode::FlushAll => {
            bump_existing_watched_keys();
        }
        OpCode::RPopLPush | OpCode::BRPopLPush | OpCode::LMove | OpCode::BLMove => {
            if let Some(destination) = cmd.values.first() {
                bump_key_version(destination);
            }
        }
        OpCode::Rename
        | OpCode::RenameNx
        | OpCode::Copy
        | OpCode::Move
        | OpCode::Sort
        | OpCode::SMove
        | OpCode::SInterStore
        | OpCode::SUnionStore
        | OpCode::SDiffStore
        | OpCode::ZRangeStore
        | OpCode::ZUnionStore
        | OpCode::ZInterStore
        | OpCode::ZDiffStore => {
            if let Some(destination) = cmd.values.first() {
                if !destination.is_empty() {
                    bump_key_version(destination);
                }
            }
        }
        _ => {}
    }
}

fn record_command_call(op: OpCode) {
    increment_worker_counter(&WORKER_COMMANDS_PROCESSED, &TOTAL_COMMANDS_PROCESSED);
    if op == OpCode::BLPop {
        increment_worker_counter(&WORKER_BLPOP_CALLS, &CMDSTAT_BLPOP_CALLS);
    }
}

fn increment_worker_counter(counters: &[WorkerCounter; MAX_REDIS_WORKERS], fallback: &AtomicUsize) {
    REDIS_WORKER_ID.with(|worker_id| {
        let worker_id = worker_id.get();
        if worker_id < counters.len() {
            counters[worker_id].0.fetch_add(1, Ordering::Relaxed);
        } else {
            fallback.fetch_add(1, Ordering::Relaxed);
        }
    });
}

fn load_worker_counter(
    counters: &[WorkerCounter; MAX_REDIS_WORKERS],
    fallback: &AtomicUsize,
) -> usize {
    fallback.load(Ordering::Relaxed)
        + counters
            .iter()
            .map(|counter| counter.0.load(Ordering::Relaxed))
            .sum::<usize>()
}

fn reset_worker_counter(counters: &[WorkerCounter; MAX_REDIS_WORKERS], fallback: &AtomicUsize) {
    fallback.store(0, Ordering::Relaxed);
    for counter in counters {
        counter.0.store(0, Ordering::Relaxed);
    }
}

fn record_dirty_change() {
    increment_worker_counter(&WORKER_DIRTY_CHANGES, &DIRTY_CHANGES);
}

fn should_reject_for_oom(op: OpCode) -> bool {
    MAXMEMORY_SETTING.load(Ordering::Relaxed) > 0
        && is_dirty_command(op)
        && !matches!(op, OpCode::Discard | OpCode::FlushDb | OpCode::FlushAll)
}

fn should_reject_for_lua_busy(op: OpCode, args: &[Bytes]) -> bool {
    if LUA_BUSY.load(Ordering::Relaxed) == 0 {
        return false;
    }
    if op == OpCode::Multi || op == OpCode::Exec {
        return false;
    }
    if op == OpCode::Script
        && args
            .first()
            .map(|arg| ascii_eq_ci(arg.as_ref(), b"KILL"))
            .unwrap_or(false)
    {
        return false;
    }
    true
}

fn execute_or_block_command(client: &mut ClientConn, cmd: &Command) -> std::io::Result<()> {
    record_command_call(cmd.op);
    if LUA_BUSY.load(Ordering::Relaxed) != 0 && cmd.op == OpCode::Exec && client.txn_state.in_multi
    {
        client.txn_state.mark_queue_error();
    }
    if should_reject_for_lua_busy(cmd.op, &cmd.args) {
        if client.txn_state.in_multi {
            client.txn_state.mark_queue_error();
        }
        client
            .write_buf
            .extend_from_slice(b"-BUSY Redis is busy running a script. You can only call SCRIPT KILL or SHUTDOWN NOSAVE.\r\n");
        return Ok(());
    }
    if should_reject_for_oom(cmd.op) {
        if client.txn_state.in_multi {
            client.txn_state.mark_queue_error();
        }
        client
            .write_buf
            .extend_from_slice(b"-OOM command not allowed when used memory > 'maxmemory'.\r\n");
        return Ok(());
    }
    if matches!(
        cmd.op,
        OpCode::Eval | OpCode::EvalSha | OpCode::EvalRo | OpCode::EvalShaRo
    ) {
        // Parked for the worker loop, which runs it while it holds no borrow
        // on this client, so the script's busy hook can answer the others.
        client.script_request = Some(cmd.clone());
        // The script's own writes are replayed into the key versions when its
        // transaction commits; this covers the keys it declared, which is what
        // a WATCH on a scripted key is watching.
        if is_dirty_command(cmd.op) {
            record_dirty_change();
            bump_modified_key_versions(cmd);
        }
        return Ok(());
    }
    if command_blocks(cmd) && !client.txn_state.in_multi {
        let resolved = resolve_stream_read_positions(cmd);
        let attempt = resolved.as_ref().map(|(first, _)| first).unwrap_or(cmd);
        let mut reply = Vec::new();
        ffi_execute_single(attempt, client.client_state.protocol_version, &mut reply)?;
        if is_null_reply(&reply) {
            client.set_blocked(
                resolved
                    .map(|(_, parked)| parked)
                    .unwrap_or_else(|| cmd.clone()),
            );
        } else {
            if is_dirty_command(cmd.op) && !is_error_reply(&reply) {
                record_dirty_change();
                bump_modified_key_versions(cmd);
            }
            client.write_buf.extend_from_slice(&reply);
        }
    } else {
        let was_in_multi = client.txn_state.in_multi;
        handle_command(
            cmd,
            &mut client.txn_state,
            &mut client.client_state,
            &mut client.write_buf,
        )?;
        if !was_in_multi && is_dirty_command(cmd.op) {
            record_dirty_change();
            bump_modified_key_versions(cmd);
        } else if !was_in_multi && cmd.op == OpCode::DbSize {
            bump_existing_watched_keys();
        }
    }
    if client.client_state.close_after_reply {
        client.close_after_write = true;
    }
    Ok(())
}

fn retry_blocked_command(client: &mut ClientConn) -> std::io::Result<bool> {
    let Some(blocked) = client.blocked_command.as_ref() else {
        return Ok(false);
    };
    if let Some(error) = take_client_unblock(client.client_state.id) {
        let protocol_version = client.client_state.protocol_version;
        client.clear_blocked();
        if error {
            client
                .write_buf
                .extend_from_slice(b"-UNBLOCKED client unblocked via CLIENT UNBLOCK\r\n");
        } else {
            write_null(&mut client.write_buf, protocol_version)?;
        }
        return Ok(true);
    }
    if let Some(deadline_ms) = blocked.deadline_ms {
        if unix_time_ms() >= deadline_ms {
            let protocol_version = client.client_state.protocol_version;
            let op = blocked.cmd.op;
            client.clear_blocked();
            if protocol_version < 3
                && matches!(
                    op,
                    OpCode::BLPop
                        | OpCode::BRPop
                        | OpCode::BLMPop
                        | OpCode::BZPopMin
                        | OpCode::BZPopMax
                        | OpCode::BZMPop
                )
            {
                client.write_buf.extend_from_slice(b"*-1\r\n");
            } else {
                write_null(&mut client.write_buf, protocol_version)?;
            }
            return Ok(true);
        }
    }

    if !blocked_client_has_turn(client.client_state.id) {
        return Ok(false);
    }

    let mut cmd = blocked.cmd.clone();
    if matches!(
        cmd.op,
        OpCode::BLPop
            | OpCode::BRPop
            | OpCode::BLMPop
            | OpCode::BZPopMin
            | OpCode::BZPopMax
            | OpCode::BZMPop
    ) {
        cmd.keys = eligible_blocked_keys(client.client_state.id, &cmd.keys);
        if cmd.keys.is_empty() {
            return Ok(false);
        }
    }
    let mut reply = Vec::new();
    ffi_execute_single(&cmd, client.client_state.protocol_version, &mut reply)?;
    if is_null_reply(&reply) {
        Ok(false)
    } else if matches!(
        cmd.op,
        // A blocking XREAD ignores a key that holds another type and keeps
        // waiting for a stream to appear there, as Redis does; the wrong type
        // is only reported by the first attempt, before the client parks.
        OpCode::BLPop | OpCode::BRPop | OpCode::BLMPop | OpCode::XRead
    ) && is_wrongtype_reply(&reply)
    {
        Ok(false)
    } else {
        let dirty = is_dirty_command(cmd.op) && !is_error_reply(&reply);
        if dirty {
            record_dirty_change();
            bump_modified_key_versions(&cmd);
        }
        client.clear_blocked();
        client.write_buf.extend_from_slice(&reply);
        if dirty {
            notify_all_workers();
        }
        Ok(true)
    }
}

fn service_blocked_clients(clients: &mut [ClientConn]) -> std::io::Result<bool> {
    let mut made_progress = false;
    for client in clients.iter_mut() {
        if client.blocked_command.is_none() {
            continue;
        }
        if retry_blocked_command(client)? {
            made_progress = true;
        }
        if !client.write_buf.is_empty() {
            if flush_client(client)? {
                made_progress = true;
            }
        }
    }
    Ok(made_progress)
}

fn blocked_client_disconnected(client: &ClientConn) -> std::io::Result<bool> {
    let mut byte = [0u8; 1];
    match client.stream.peek(&mut byte) {
        Ok(0) => Ok(true),
        Ok(_) => Ok(false),
        Err(e) if e.kind() == ErrorKind::WouldBlock => Ok(false),
        Err(e) => Err(e),
    }
}

fn command_may_wake_blocked(txn_state: &TransactionState, cmd: &Command) -> bool {
    if !txn_state.in_multi {
        return is_dirty_command(cmd.op);
    }
    cmd.op == OpCode::Exec
        && txn_state
            .queued_commands
            .iter()
            .any(|queued| is_dirty_command(queued.op))
}

fn should_wake_blocked_after_command(client: &ClientConn, cmd: &Command) -> bool {
    BLOCKED_CLIENTS.load(Ordering::Relaxed) > 0 && command_may_wake_blocked(&client.txn_state, cmd)
}

fn should_defer_dirty_command(client: &ClientConn, cmd: &Command) -> bool {
    client.pending_command.is_none()
        && BLOCKED_CLIENTS.load(Ordering::Relaxed) > 0
        && command_may_wake_blocked(&client.txn_state, cmd)
}

fn should_wait_for_blocked_completion(cmd: &Command) -> bool {
    // GEOADD is a ZADD, so it can serve a client blocked in BZPOPMIN/BZMPOP
    // and has to wait for that client the same way ZADD does; GEOSEARCHSTORE
    // creates a sorted set at its destination, so it can too.
    matches!(
        cmd.op,
        OpCode::LPush
            | OpCode::RPush
            | OpCode::ZAdd
            | OpCode::GeoAdd
            | OpCode::GeoSearchStore
            | OpCode::XAdd
    )
}

enum RawMakoCommand<'a> {
    Get { key: &'a [u8] },
    Set { key: &'a [u8], value: &'a [u8] },
}

enum RawMakoParse<'a> {
    Complete {
        command: RawMakoCommand<'a>,
        consumed: usize,
    },
    Incomplete,
    NotFast,
}

fn parse_usize_ascii(raw: &[u8]) -> Option<usize> {
    if raw.is_empty() {
        return None;
    }
    let mut value = 0usize;
    for &byte in raw {
        if !byte.is_ascii_digit() {
            return None;
        }
        value = value
            .checked_mul(10)?
            .checked_add(usize::from(byte - b'0'))?;
    }
    Some(value)
}

fn read_crlf_line(buf: &[u8], offset: usize) -> Result<Option<(&[u8], usize)>, ()> {
    if offset >= buf.len() {
        return Ok(None);
    }
    let mut end = offset;
    while end + 1 < buf.len() {
        if buf[end] == b'\r' && buf[end + 1] == b'\n' {
            return Ok(Some((&buf[offset..end], end + 2)));
        }
        end += 1;
    }
    Ok(None)
}

fn raw_command_is(command: &[u8], expected: &[u8]) -> bool {
    command.len() == expected.len()
        && command
            .iter()
            .zip(expected)
            .all(|(actual, expected)| actual.eq_ignore_ascii_case(expected))
}

fn read_resp_bulk(buf: &[u8], offset: usize) -> Result<Option<(&[u8], usize)>, ()> {
    if offset >= buf.len() {
        return Ok(None);
    }
    if buf[offset] != b'$' {
        return Err(());
    }
    let Some((len_raw, data_start)) = read_crlf_line(buf, offset + 1)? else {
        return Ok(None);
    };
    let Some(len) = parse_usize_ascii(len_raw) else {
        return Err(());
    };
    let data_end = data_start.checked_add(len).ok_or(())?;
    let frame_end = data_end.checked_add(2).ok_or(())?;
    if frame_end > buf.len() {
        return Ok(None);
    }
    if &buf[data_end..frame_end] != b"\r\n" {
        return Err(());
    }
    Ok(Some((&buf[data_start..data_end], frame_end)))
}

fn parse_raw_mako_string_command(buf: &[u8]) -> RawMakoParse<'_> {
    if buf.is_empty() {
        return RawMakoParse::Incomplete;
    }
    if buf[0] != b'*' {
        return RawMakoParse::NotFast;
    }
    let Some((array_len_raw, mut offset)) = (match read_crlf_line(buf, 1) {
        Ok(line) => line,
        Err(()) => return RawMakoParse::NotFast,
    }) else {
        return RawMakoParse::Incomplete;
    };
    let Some(array_len) = parse_usize_ascii(array_len_raw) else {
        return RawMakoParse::NotFast;
    };
    if array_len != 2 && array_len != 3 {
        return RawMakoParse::NotFast;
    }
    let Some((command, next_offset)) = (match read_resp_bulk(buf, offset) {
        Ok(part) => part,
        Err(()) => return RawMakoParse::NotFast,
    }) else {
        return RawMakoParse::Incomplete;
    };
    offset = next_offset;
    if array_len == 2 && raw_command_is(command, b"GET") {
        let Some((key, consumed)) = (match read_resp_bulk(buf, offset) {
            Ok(part) => part,
            Err(()) => return RawMakoParse::NotFast,
        }) else {
            return RawMakoParse::Incomplete;
        };
        // Both reserved first bytes fall back to the general parser, which is
        // the one place that turns them into the reserved-prefix error: 0x01
        // is the executor's hidden namespace, 0x02 the logical-database one.
        if matches!(key.first(), Some(&0x01) | Some(&DB_KEY_MARKER)) {
            return RawMakoParse::NotFast;
        }
        return RawMakoParse::Complete {
            command: RawMakoCommand::Get { key },
            consumed,
        };
    }
    if array_len == 3 && raw_command_is(command, b"SET") {
        let Some((key, next_offset)) = (match read_resp_bulk(buf, offset) {
            Ok(part) => part,
            Err(()) => return RawMakoParse::NotFast,
        }) else {
            return RawMakoParse::Incomplete;
        };
        // Both reserved first bytes fall back to the general parser, which is
        // the one place that turns them into the reserved-prefix error: 0x01
        // is the executor's hidden namespace, 0x02 the logical-database one.
        if matches!(key.first(), Some(&0x01) | Some(&DB_KEY_MARKER)) {
            return RawMakoParse::NotFast;
        }
        let Some((value, consumed)) = (match read_resp_bulk(buf, next_offset) {
            Ok(part) => part,
            Err(()) => return RawMakoParse::NotFast,
        }) else {
            return RawMakoParse::Incomplete;
        };
        return RawMakoParse::Complete {
            command: RawMakoCommand::Set { key, value },
            consumed,
        };
    }
    RawMakoParse::NotFast
}

fn bump_key_version_raw(key: &[u8]) {
    let Some(key_versions) = KEY_VERSIONS.get() else {
        return;
    };
    if let Ok(mut versions) = key_versions.lock() {
        let key = Bytes::copy_from_slice(key);
        let next = versions.get(&key).copied().unwrap_or(0).saturating_add(1);
        versions.insert(key, next);
    }
}

fn can_use_raw_mako_fast_path(client: &ClientConn) -> bool {
    // One relaxed load, and the only change this package makes to the fast
    // path. A monitor has to see GET and SET like every other command, and the
    // shortcut never builds a Command to report, so while any monitor is
    // attached every frame goes to the general parser instead. With no monitor
    // the load is a cached read of a never-written line and nothing else here
    // changes.
    MONITOR_COUNT.load(Ordering::Relaxed) == 0
        && redis_backend() == RedisBackend::Mako
        // The shortcut sends the key straight to storage without building a
        // Command, so it never applies a database prefix: a connection that
        // selected database 1..15 has to go through the general parser. One
        // field load from a struct the surrounding code already touches,
        // mirroring the monitor gate above.
        && client.client_state.db == 0
        && !client.txn_state.in_multi
        && !client.client_state.in_subscriber_mode()
        && client.pending_command.is_none()
        && client.blocked_command.is_none()
        && LUA_BUSY.load(Ordering::Relaxed) == 0
        && MAXMEMORY_SETTING.load(Ordering::Relaxed) == 0
        && BLOCKED_CLIENTS.load(Ordering::Relaxed) == 0
}

fn process_raw_mako_fast_frame(client: &mut ClientConn) -> std::io::Result<bool> {
    if !can_use_raw_mako_fast_path(client) {
        return Ok(false);
    }
    let parsed = parse_raw_mako_string_command(client.resp3.buffered());
    let (consumed, dirty) = match parsed {
        RawMakoParse::Complete { command, consumed } => match command {
            RawMakoCommand::Get { key } => {
                record_command_call(OpCode::Get);
                let _ = execute_fast_mako_string_op(
                    OpCode::Get,
                    key,
                    None,
                    client.client_state.protocol_version,
                    &mut client.write_buf,
                )?;
                (consumed, false)
            }
            RawMakoCommand::Set { key, value } => {
                record_command_call(OpCode::Set);
                let success = execute_fast_mako_string_op(
                    OpCode::Set,
                    key,
                    Some(value),
                    client.client_state.protocol_version,
                    &mut client.write_buf,
                )?;
                if success {
                    bump_key_version_raw(key);
                }
                (consumed, success)
            }
        },
        RawMakoParse::Incomplete | RawMakoParse::NotFast => return Ok(false),
    };
    client.resp3.consume(consumed);
    if dirty {
        record_dirty_change();
    }
    Ok(true)
}

fn process_buffered_frames(
    client: &mut ClientConn,
    wake_blocked: &mut bool,
) -> std::io::Result<bool> {
    let mut made_progress = false;
    loop {
        if process_raw_mako_fast_frame(client)? {
            made_progress = true;
            continue;
        }
        match client.resp3.next_frame() {
            Ok(Some(frame)) => {
                made_progress = true;
                // Taken from the frame, before parsing consumes it, so a
                // monitor sees the argument bytes the client sent. Built only
                // while a monitor is attached.
                let monitor_argv = if monitor_count() != 0 {
                    monitor_argv_from_frame(&frame)
                } else {
                    None
                };
                // The parse arms turn Redis-visible keys into storage-facing
                // names, and they read the database from the worker thread.
                // A worker serves one client's frame at a time, so setting it
                // here, once per frame, covers parsing, execution and the
                // reply. The raw GET/SET shortcut above never reads it: it
                // only runs for a client that is on database 0.
                set_current_db(client.client_state.db);
                match parse_resp3(frame) {
                    Ok(cmd) => {
                        // Redis reports a command before running it, and only
                        // one it recognized, so this sits after the parse and
                        // before every path that can execute the command:
                        // the deferred-write path re-enters
                        // execute_or_block_command later with this same
                        // command and must not report it twice, and a
                        // blocking command that parks is reported once, when
                        // it arrives.
                        if let Some(argv) = monitor_argv {
                            feed_monitors(&client.client_state, &argv);
                        }
                        if should_defer_dirty_command(client, &cmd) {
                            client.pending_command = Some(cmd);
                            break;
                        }
                        let should_wake = should_wake_blocked_after_command(client, &cmd);
                        execute_or_block_command(client, &cmd)?;
                        if client.blocked_command.is_some() || client.script_request.is_some() {
                            break;
                        }
                        if should_wake {
                            *wake_blocked = true;
                            break;
                        }
                    }
                    Err(err) => {
                        if client.txn_state.in_multi {
                            client.txn_state.mark_queue_error();
                        }
                        write_parse_error(&mut client.write_buf, err)?;
                    }
                }
            }
            Ok(None) => break,
            Err(_) => {
                write_err(&mut client.write_buf, "protocol error")?;
                break;
            }
        }
    }
    Ok(made_progress)
}

fn service_client(client: &mut ClientConn) -> std::io::Result<ClientEvent> {
    let mut made_progress = false;
    let mut wake_blocked = false;
    // A blocked or deferred command resumes on whichever worker picks the
    // client up next, and its reply still has to strip that connection's
    // database prefix from the key names it returns.
    set_current_db(client.client_state.db);

    if drain_pubsub_queue(client) {
        made_progress = true;
    }

    if !client.write_buf.is_empty() {
        if !flush_client(client)? {
            return Ok(ClientEvent::Close);
        }
        made_progress = true;
    }
    if client.close_after_write && client.write_buf.is_empty() {
        return Ok(ClientEvent::Close);
    }

    // A parked script is the worker loop's to run; everything written before
    // it goes out first.
    if client.script_request.is_some() {
        if !client.write_buf.is_empty() && !flush_client(client)? {
            return Ok(ClientEvent::Close);
        }
        return Ok(ClientEvent::RunScript);
    }

    if client.blocked_command.is_some() {
        if retry_blocked_command(client)? {
            made_progress = true;
        }
        if client.blocked_command.is_some() {
            if blocked_client_disconnected(client)? {
                return Ok(ClientEvent::Close);
            }
            return if made_progress {
                Ok(ClientEvent::Progress)
            } else {
                Ok(ClientEvent::Keep)
            };
        }
    }

    if let Some(cmd) = client.pending_command.take() {
        let should_wake = should_wake_blocked_after_command(client, &cmd);
        let blocked_fronts = if should_wake && should_wait_for_blocked_completion(&cmd) {
            blocked_fronts_for_keys(&cmd.keys)
        } else {
            Vec::new()
        };
        let reply_start = client.write_buf.len();
        execute_or_block_command(client, &cmd)?;
        made_progress = true;
        if should_wake {
            notify_all_workers();
            if !is_error_reply(&client.write_buf[reply_start..]) {
                wait_for_blocked_fronts(&blocked_fronts, Duration::from_millis(250));
            }
            wake_blocked = true;
        }
    }
    if client.blocked_command.is_some() || wake_blocked || client.script_request.is_some() {
        if !client.write_buf.is_empty() {
            if !flush_client(client)? {
                return Ok(ClientEvent::Close);
            }
        }
        return if wake_blocked {
            Ok(ClientEvent::WakeBlocked)
        } else if client.script_request.is_some() {
            Ok(ClientEvent::RunScript)
        } else {
            Ok(ClientEvent::Progress)
        };
    }

    if process_buffered_frames(client, &mut wake_blocked)? {
        made_progress = true;
    }
    if client.blocked_command.is_some() || wake_blocked || client.script_request.is_some() {
        if !client.write_buf.is_empty() {
            if !flush_client(client)? {
                return Ok(ClientEvent::Close);
            }
            made_progress = true;
        }
        return if wake_blocked {
            Ok(ClientEvent::WakeBlocked)
        } else if client.script_request.is_some() {
            Ok(ClientEvent::RunScript)
        } else if made_progress {
            Ok(ClientEvent::Progress)
        } else {
            Ok(ClientEvent::Keep)
        };
    }

    loop {
        match client.stream.read(&mut client.read_buf) {
            Ok(0) => {
                return if client.write_buf.is_empty() {
                    Ok(ClientEvent::Close)
                } else {
                    client.close_after_write = true;
                    Ok(ClientEvent::Progress)
                };
            }
            Ok(n) => {
                client.resp3.read_bytes(&client.read_buf[..n]);
                made_progress = true;

                if process_buffered_frames(client, &mut wake_blocked)? {
                    made_progress = true;
                }
                if client.blocked_command.is_some() || wake_blocked || client.script_request.is_some() {
                    break;
                }
            }
            Err(e) if e.kind() == ErrorKind::WouldBlock => break,
            Err(e) => return Err(e),
        }
    }

    if drain_pubsub_queue(client) {
        made_progress = true;
    }

    if !client.write_buf.is_empty() {
        if !flush_client(client)? {
            return Ok(ClientEvent::Close);
        }
        made_progress = true;
    }

    if client.close_after_write && client.write_buf.is_empty() {
        Ok(ClientEvent::Close)
    } else if wake_blocked {
        Ok(ClientEvent::WakeBlocked)
    } else if made_progress {
        Ok(ClientEvent::Progress)
    } else {
        Ok(ClientEvent::Keep)
    }
}

fn write_hello_response<W: Write>(
    client_state: &ClientState,
    writer: &mut W,
) -> std::io::Result<()> {
    if client_state.protocol_version >= 3 {
        write_map_header(writer, 7)?;
        write_simple_string(writer, "server")?;
        write_simple_string(writer, "makoCon")?;
        write_simple_string(writer, "version")?;
        write_simple_string(writer, "0.1.0")?;
        write_simple_string(writer, "proto")?;
        write_integer(writer, client_state.protocol_version as i64)?;
        write_simple_string(writer, "id")?;
        write_integer(writer, client_state.id as i64)?;
        write_simple_string(writer, "mode")?;
        write_simple_string(writer, "standalone")?;
        write_simple_string(writer, "role")?;
        write_simple_string(writer, "master")?;
        write_simple_string(writer, "modules")?;
        write_array_header(writer, 0)
    } else {
        write_array_header(writer, 14)?;
        write_bulk(writer, b"server")?;
        write_bulk(writer, b"makoCon")?;
        write_bulk(writer, b"version")?;
        write_bulk(writer, b"0.1.0")?;
        write_bulk(writer, b"proto")?;
        write_integer(writer, client_state.protocol_version as i64)?;
        write_bulk(writer, b"id")?;
        write_integer(writer, client_state.id as i64)?;
        write_bulk(writer, b"mode")?;
        write_bulk(writer, b"standalone")?;
        write_bulk(writer, b"role")?;
        write_bulk(writer, b"master")?;
        write_bulk(writer, b"modules")?;
        write_array_header(writer, 0)
    }
}

fn handle_hello<W: Write>(
    cmd: &Command,
    client_state: &mut ClientState,
    writer: &mut W,
) -> std::io::Result<()> {
    let mut index = 0;
    if let Some(first) = cmd.args.first() {
        let Some(proto) = parse_protocol_version(first) else {
            write_err(writer, "NOPROTO unsupported protocol version")?;
            return Ok(());
        };
        client_state.protocol_version = proto;
        index = 1;
    }

    while index < cmd.args.len() {
        let arg = cmd.args[index].as_ref();
        if ascii_eq_ci(arg, b"AUTH") {
            if index + 2 >= cmd.args.len() {
                write_err(writer, "syntax error")?;
                return Ok(());
            }
            index += 3;
        } else if ascii_eq_ci(arg, b"SETNAME") {
            if index + 1 >= cmd.args.len() {
                write_err(writer, "syntax error")?;
                return Ok(());
            }
            client_state.name = Some(cmd.args[index + 1].clone());
            index += 2;
        } else {
            write_err(writer, "syntax error")?;
            return Ok(());
        }
    }

    write_hello_response(client_state, writer)
}

fn handle_client_command<W: Write>(
    cmd: &Command,
    client_state: &mut ClientState,
    writer: &mut W,
) -> std::io::Result<()> {
    let Some(subcommand) = cmd.args.first() else {
        write_err(writer, "wrong number of arguments for 'client' command")?;
        return Ok(());
    };

    if ascii_eq_ci(subcommand, b"SETNAME") {
        if cmd.args.len() != 2 {
            write_err(
                writer,
                "wrong number of arguments for 'client setname' command",
            )?;
            return Ok(());
        }
        client_state.name = Some(cmd.args[1].clone());
        write_simple_ok(writer)
    } else if ascii_eq_ci(subcommand, b"GETNAME") {
        if cmd.args.len() != 1 {
            write_err(
                writer,
                "wrong number of arguments for 'client getname' command",
            )?;
            return Ok(());
        }
        match &client_state.name {
            Some(name) => write_bulk(writer, name),
            None => write_null(writer, client_state.protocol_version),
        }
    } else if ascii_eq_ci(subcommand, b"ID") {
        if cmd.args.len() != 1 {
            write_err(writer, "wrong number of arguments for 'client id' command")?;
            return Ok(());
        }
        write_integer(writer, client_state.id as i64)
    } else if ascii_eq_ci(subcommand, b"UNBLOCK") {
        if cmd.args.len() < 2 || cmd.args.len() > 3 {
            write_err(
                writer,
                "wrong number of arguments for 'client unblock' command",
            )?;
            return Ok(());
        }
        let id = std::str::from_utf8(cmd.args[1].as_ref())
            .ok()
            .and_then(|text| text.parse::<usize>().ok())
            .unwrap_or_else(|| usize::MAX);
        let error = cmd
            .args
            .get(2)
            .map(|arg| ascii_eq_ci(arg.as_ref(), b"ERROR"))
            .unwrap_or(false);
        if cmd.args.len() == 3
            && !ascii_eq_ci(cmd.args[2].as_ref(), b"ERROR")
            && !ascii_eq_ci(cmd.args[2].as_ref(), b"TIMEOUT")
        {
            write_err(writer, "syntax error")?;
            return Ok(());
        }
        if id == usize::MAX {
            write_err(writer, "value is not an integer or out of range")
        } else if id == client_state.id {
            write_integer(writer, 0)
        } else {
            request_client_unblock(id, error);
            write_integer(writer, 1)
        }
    } else if ascii_eq_ci(subcommand, b"SETINFO") {
        if cmd.args.len() < 3 {
            write_err(
                writer,
                "wrong number of arguments for 'client setinfo' command",
            )?;
            return Ok(());
        }
        write_simple_ok(writer)
    } else if ascii_eq_ci(subcommand, b"NO-EVICT") {
        if cmd.args.len() < 2 || cmd.args.len() > 3 {
            write_err(
                writer,
                "wrong number of arguments for 'client no-evict' command",
            )?;
            return Ok(());
        }
        write_simple_ok(writer)
    } else if ascii_eq_ci(subcommand, b"REPLY") {
        if cmd.args.len() != 2 {
            write_err(
                writer,
                "wrong number of arguments for 'client reply' command",
            )?;
            return Ok(());
        }
        if ascii_eq_ci(cmd.args[1].as_ref(), b"OFF") || ascii_eq_ci(cmd.args[1].as_ref(), b"SKIP") {
            Ok(())
        } else if ascii_eq_ci(cmd.args[1].as_ref(), b"ON") {
            write_simple_ok(writer)
        } else {
            write_err(writer, "syntax error")
        }
    } else if ascii_eq_ci(subcommand, b"LIST") {
        if cmd.args.len() != 1 {
            write_err(
                writer,
                "wrong number of arguments for 'client list' command",
            )?;
            return Ok(());
        }
        let name = client_state
            .name
            .as_ref()
            .map(|n| String::from_utf8_lossy(n).into_owned())
            .unwrap_or_default();
        let flags = if client_state.blocked { "b" } else { "N" };
        let line = format!(
            "id={} name={} flags={} db={}\r\n",
            client_state.id, name, flags, client_state.db
        );
        write_bulk(writer, line.as_bytes())
    } else {
        write_err(writer, "unsupported CLIENT subcommand")
    }
}

fn handle_command_command<W: Write>(cmd: &Command, writer: &mut W) -> std::io::Result<()> {
    if cmd.args.is_empty() || ascii_eq_ci(cmd.args[0].as_ref(), b"INFO") {
        write_array_header(writer, 0)
    } else if ascii_eq_ci(cmd.args[0].as_ref(), b"DOCS") {
        write_map_header(writer, 0)
    } else if ascii_eq_ci(cmd.args[0].as_ref(), b"COUNT") {
        write_integer(writer, 0)
    } else {
        write_err(writer, "unsupported COMMAND subcommand")
    }
}

// ===== Bitmap helpers (computed in Rust over a GET result) =====

// ===== Geo commands =====
//
// A geo set is an ordinary sorted set whose score is Redis's 52-bit
// interleaved geohash (`geohash.c`). That integer is exactly representable as
// an f64, so TYPE reports zset and every zset command keeps working on a geo
// key. Nothing below makoCon changes: GEOADD is a ZADD op, GEOPOS/GEODIST/
// GEOHASH are one ZSCORE op per member, and the searches are one
// ZRANGEBYSCORE-with-scores op per geohash box, all inside one request.

/// Redis `GEO_LAT_MIN` / `GEO_LAT_MAX`: the Mercator cut-off, not ±90.
const GEO_LAT_MIN: f64 = -85.05112878;
const GEO_LAT_MAX: f64 = 85.05112878;
const GEO_LONG_MIN: f64 = -180.0;
const GEO_LONG_MAX: f64 = 180.0;
/// Redis `GEO_STEP_MAX`: 26 bits per axis, 52 bits of score.
const GEO_STEP_MAX: u8 = 26;
/// Redis `MERCATOR_MAX`.
const GEO_MERCATOR_MAX: f64 = 20_037_726.37;
/// Redis `EARTH_RADIUS_IN_METERS`, the WGS-84 quadratic mean radius.
const GEO_EARTH_RADIUS_M: f64 = 6_372_797.560856;

const GEO_UNIT_ERROR: &str = "unsupported unit provided. please use M, KM, FT, MI";
/// Redis refuses a destination together with the output columns, because the
/// stored sorted set can carry only one score per member.
const GEO_STORE_WITH_ERROR: &str =
    "STORE option in GEORADIUS is not compatible with WITHDIST, WITHHASH and WITHCOORD options";

/// Redis `interleave64`: x (latitude) into the even bits, y (longitude) into
/// the odd ones.
fn geo_interleave64(xlo: u32, ylo: u32) -> u64 {
    const B: [u64; 5] = [
        0x5555_5555_5555_5555,
        0x3333_3333_3333_3333,
        0x0f0f_0f0f_0f0f_0f0f,
        0x00ff_00ff_00ff_00ff,
        0x0000_ffff_0000_ffff,
    ];
    const S: [u32; 5] = [1, 2, 4, 8, 16];
    let spread = |mut v: u64| {
        v = (v | (v << S[4])) & B[4];
        v = (v | (v << S[3])) & B[3];
        v = (v | (v << S[2])) & B[2];
        v = (v | (v << S[1])) & B[1];
        (v | (v << S[0])) & B[0]
    };
    spread(xlo as u64) | (spread(ylo as u64) << 1)
}

/// Redis `deinterleave64`: latitude back in the low 32 bits, longitude in the
/// high 32.
fn geo_deinterleave64(interleaved: u64) -> u64 {
    const B: [u64; 6] = [
        0x5555_5555_5555_5555,
        0x3333_3333_3333_3333,
        0x0f0f_0f0f_0f0f_0f0f,
        0x00ff_00ff_00ff_00ff,
        0x0000_ffff_0000_ffff,
        0x0000_0000_ffff_ffff,
    ];
    const S: [u32; 6] = [0, 1, 2, 4, 8, 16];
    let gather = |mut v: u64| {
        v = (v | (v >> S[0])) & B[0];
        v = (v | (v >> S[1])) & B[1];
        v = (v | (v >> S[2])) & B[2];
        v = (v | (v >> S[3])) & B[3];
        v = (v | (v >> S[4])) & B[4];
        (v | (v >> S[5])) & B[5]
    };
    gather(interleaved) | (gather(interleaved >> 1) << 32)
}

/// Redis `geohashEncode`. `step` bits per axis; None when the point is outside
/// the supported range.
fn geo_encode(
    long_min: f64,
    long_max: f64,
    lat_min: f64,
    lat_max: f64,
    longitude: f64,
    latitude: f64,
    step: u8,
) -> Option<u64> {
    if step == 0 || step > 32 {
        return None;
    }
    if !(GEO_LONG_MIN..=GEO_LONG_MAX).contains(&longitude)
        || !(GEO_LAT_MIN..=GEO_LAT_MAX).contains(&latitude)
    {
        return None;
    }
    if latitude < lat_min || latitude > lat_max || longitude < long_min || longitude > long_max {
        return None;
    }
    let cells = (1u64 << step) as f64;
    let lat_offset = (latitude - lat_min) / (lat_max - lat_min) * cells;
    let long_offset = (longitude - long_min) / (long_max - long_min) * cells;
    Some(geo_interleave64(lat_offset as u32, long_offset as u32))
}

/// Redis `geohashEncodeWGS84` at the given step.
fn geo_encode_wgs84(longitude: f64, latitude: f64, step: u8) -> Option<u64> {
    geo_encode(
        GEO_LONG_MIN,
        GEO_LONG_MAX,
        GEO_LAT_MIN,
        GEO_LAT_MAX,
        longitude,
        latitude,
        step,
    )
}

/// The 52-bit sorted-set score Redis stores for a position.
fn geo_score_for_position(longitude: f64, latitude: f64) -> Option<u64> {
    // Redis `geohashAlign52Bits` is a no-op at GEO_STEP_MAX.
    geo_encode_wgs84(longitude, latitude, GEO_STEP_MAX)
}

struct GeoArea {
    lon_min: f64,
    lon_max: f64,
    lat_min: f64,
    lat_max: f64,
}

/// Redis `geohashDecode`: the cell a hash covers.
fn geo_decode(
    long_min: f64,
    long_max: f64,
    lat_min: f64,
    lat_max: f64,
    bits: u64,
    step: u8,
) -> GeoArea {
    let separated = geo_deinterleave64(bits);
    let ilato = (separated & 0xffff_ffff) as f64;
    let ilono = (separated >> 32) as f64;
    let cells = (1u64 << step) as f64;
    let lat_scale = lat_max - lat_min;
    let long_scale = long_max - long_min;
    GeoArea {
        lon_min: long_min + (ilono / cells) * long_scale,
        lon_max: long_min + ((ilono + 1.0) / cells) * long_scale,
        lat_min: lat_min + (ilato / cells) * lat_scale,
        lat_max: lat_min + ((ilato + 1.0) / cells) * lat_scale,
    }
}

fn geo_decode_wgs84(bits: u64, step: u8) -> GeoArea {
    geo_decode(
        GEO_LONG_MIN,
        GEO_LONG_MAX,
        GEO_LAT_MIN,
        GEO_LAT_MAX,
        bits,
        step,
    )
}

/// Redis `decodeGeohash`: a stored score back to the center of its box.
fn geo_decode_score(score: u64) -> (f64, f64) {
    let area = geo_decode_wgs84(score, GEO_STEP_MAX);
    let lon = ((area.lon_min + area.lon_max) / 2.0).clamp(GEO_LONG_MIN, GEO_LONG_MAX);
    let lat = ((area.lat_min + area.lat_max) / 2.0).clamp(GEO_LAT_MIN, GEO_LAT_MAX);
    (lon, lat)
}

/// A score as the zset ops report it (a decimal f64) back to geohash bits.
fn geo_score_from_bytes(data: &[u8]) -> Option<u64> {
    let value: f64 = std::str::from_utf8(data).ok()?.parse().ok()?;
    if !value.is_finite() {
        return None;
    }
    Some(value.max(0.0) as u64)
}

fn geo_deg_rad(degrees: f64) -> f64 {
    degrees * (std::f64::consts::PI / 180.0)
}

fn geo_rad_deg(radians: f64) -> f64 {
    radians * (180.0 / std::f64::consts::PI)
}

/// Redis `geohashGetLatDistance`.
fn geo_lat_distance(lat1d: f64, lat2d: f64) -> f64 {
    GEO_EARTH_RADIUS_M * (geo_deg_rad(lat2d) - geo_deg_rad(lat1d)).abs()
}

/// Redis `geohashGetDistance`: haversine on the quadratic mean Earth radius.
fn geo_distance(lon1d: f64, lat1d: f64, lon2d: f64, lat2d: f64) -> f64 {
    let lon1r = geo_deg_rad(lon1d);
    let lon2r = geo_deg_rad(lon2d);
    let v = ((lon2r - lon1r) / 2.0).sin();
    if v == 0.0 {
        return geo_lat_distance(lat1d, lat2d);
    }
    let lat1r = geo_deg_rad(lat1d);
    let lat2r = geo_deg_rad(lat2d);
    let u = ((lat2r - lat1r) / 2.0).sin();
    let a = u * u + lat1r.cos() * lat2r.cos() * v * v;
    2.0 * GEO_EARTH_RADIUS_M * a.sqrt().asin()
}

/// Redis `geohash_move_x`: shift the longitude half of a hash by one cell.
fn geo_move_x(bits: u64, step: u8, direction: i8) -> u64 {
    if direction == 0 {
        return bits;
    }
    let shift = 64 - (step as u32) * 2;
    let mut x = bits & 0xaaaa_aaaa_aaaa_aaaa;
    let y = bits & 0x5555_5555_5555_5555;
    let zz = 0x5555_5555_5555_5555u64 >> shift;
    if direction > 0 {
        x = x.wrapping_add(zz + 1);
    } else {
        x |= zz;
        x = x.wrapping_sub(zz + 1);
    }
    x &= 0xaaaa_aaaa_aaaa_aaaau64 >> shift;
    x | y
}

/// Redis `geohash_move_y`: the same for the latitude half.
fn geo_move_y(bits: u64, step: u8, direction: i8) -> u64 {
    if direction == 0 {
        return bits;
    }
    let shift = 64 - (step as u32) * 2;
    let x = bits & 0xaaaa_aaaa_aaaa_aaaa;
    let mut y = bits & 0x5555_5555_5555_5555;
    let zz = 0xaaaa_aaaa_aaaa_aaaau64 >> shift;
    if direction > 0 {
        y = y.wrapping_add(zz + 1);
    } else {
        y |= zz;
        y = y.wrapping_sub(zz + 1);
    }
    y &= 0x5555_5555_5555_5555u64 >> shift;
    x | y
}

/// Redis `geohashNeighbors`, in the order `membersOfAllNeighbors` visits them:
/// north, south, east, west, north-east, north-west, south-east, south-west.
fn geo_neighbors(bits: u64, step: u8) -> [u64; 8] {
    let mv = |dx: i8, dy: i8| geo_move_y(geo_move_x(bits, step, dx), step, dy);
    [
        mv(0, 1),
        mv(0, -1),
        mv(1, 0),
        mv(-1, 0),
        mv(1, 1),
        mv(-1, 1),
        mv(1, -1),
        mv(-1, -1),
    ]
}

/// Redis `geohashEstimateStepsByRadius`.
fn geo_estimate_steps_by_radius(range_meters: f64, lat: f64) -> u8 {
    if range_meters == 0.0 {
        return GEO_STEP_MAX;
    }
    let mut range = range_meters;
    let mut step: i32 = 1;
    while range < GEO_MERCATOR_MAX {
        range *= 2.0;
        step += 1;
        if step > 64 {
            break;
        }
    }
    step -= 2;
    if !(-66.0..=66.0).contains(&lat) {
        step -= 1;
        if !(-80.0..=80.0).contains(&lat) {
            step -= 1;
        }
    }
    step.clamp(1, GEO_STEP_MAX as i32) as u8
}

/// Redis `geohashBoundingBox`. `half_width`/`half_height` are in meters.
/// Returns (min_lon, min_lat, max_lon, max_lat).
fn geo_bounding_box(lon: f64, lat: f64, half_width: f64, half_height: f64) -> (f64, f64, f64, f64) {
    let lat_delta = geo_rad_deg(half_height / GEO_EARTH_RADIUS_M);
    let long_delta_top =
        geo_rad_deg(half_width / GEO_EARTH_RADIUS_M / geo_deg_rad(lat + lat_delta).cos());
    let long_delta_bottom =
        geo_rad_deg(half_width / GEO_EARTH_RADIUS_M / geo_deg_rad(lat - lat_delta).cos());
    // North and south of the equator the wider edge is on opposite sides.
    let long_delta = if lat < 0.0 {
        long_delta_bottom
    } else {
        long_delta_top
    };
    (
        lon - long_delta,
        lat - lat_delta,
        lon + long_delta,
        lat + lat_delta,
    )
}

/// How many `[min, max)` score ranges one search may read before it is cheaper
/// to scan the whole set. Every range op costs a full pass over the staged
/// sorted set in the executor, so a long list of ranges is slower than one
/// scan, not faster.
const GEO_MAX_SEARCH_RANGES: usize = 16;

/// The cell column a longitude falls in. The caller wraps it into range: a
/// search area may run off either end of the -180..180 strip.
fn geo_lon_cell(longitude: f64, cells: u64) -> i64 {
    let ratio = (longitude - GEO_LONG_MIN) / (GEO_LONG_MAX - GEO_LONG_MIN);
    (ratio * cells as f64).floor() as i64
}

/// The cell row a latitude falls in, clamped: latitude does not wrap, and
/// anything past the Mercator cut-off belongs to the outermost row.
fn geo_lat_cell(latitude: f64, cells: u64) -> u64 {
    let ratio = (latitude - GEO_LAT_MIN) / (GEO_LAT_MAX - GEO_LAT_MIN);
    let cell = (ratio * cells as f64).floor();
    if cell < 0.0 {
        0
    } else if cell >= (cells - 1) as f64 {
        cells - 1
    } else {
        cell as u64
    }
}

/// The `[min, max)` score ranges a search has to read.
///
/// The step comes from Redis (`geohashEstimateStepsByRadius` plus its
/// "decrease the step near an edge" correction), so an ordinary search reads
/// the same resolution Redis does. Which cells to read is then taken straight
/// from the search area's bounding box rather than from Redis's fixed nine
/// neighbors: near the poles a degree of longitude is so short that the
/// matching cell can be two columns away, and a search whose area runs over a
/// pole reaches every column at all. Redis answers those cases from nine
/// boxes and misses points (its own tests/unit/geo.tcl "oblique direction" and
/// "crossing pole" cases); reading the cells the bounding box actually covers
/// is both simpler and right, and the exact distance filter that follows makes
/// the answer identical everywhere else.
fn geo_search_ranges(spec: &GeoSearchSpec) -> Vec<(u64, u64)> {
    let full_scan = vec![(0u64, 1u64 << (GEO_STEP_MAX * 2))];
    let (half_width, half_height) = if spec.circular {
        (spec.radius_m, spec.radius_m)
    } else {
        (spec.width_m / 2.0, spec.height_m / 2.0)
    };
    let (min_lon, min_lat, max_lon, max_lat) =
        geo_bounding_box(spec.center_lon, spec.center_lat, half_width, half_height);
    if !min_lat.is_finite() || !max_lat.is_finite() {
        return full_scan;
    }

    let Some(mut steps) = geo_search_step(spec, half_width, half_height) else {
        return full_scan;
    };

    // A search area that runs over a pole comes out the other side, so it
    // touches every column. So does one wider than the world.
    let over_the_pole = max_lat >= 90.0 || min_lat <= -90.0;
    let unbounded_longitude = over_the_pole
        || !min_lon.is_finite()
        || !max_lon.is_finite()
        || (max_lon - min_lon) >= 360.0;

    // Redis's latitude term in geohashEstimateStepsByRadius is admittedly
    // coarse ("it is possible to do better ... by computing the distance
    // between meridians at this latitude"), so above 66 degrees the bounding
    // box can still span many columns. Dropping one step halves that span and
    // doubles each cell, which covers exactly the same ground with fewer range
    // reads, so keep dropping until the box is the three-by-three the estimate
    // was aiming for.
    let (mut cells, mut col_lo, mut col_hi, mut row_lo, mut row_hi);
    loop {
        cells = 1u64 << steps;
        row_lo = geo_lat_cell(min_lat, cells);
        row_hi = geo_lat_cell(max_lat, cells);
        (col_lo, col_hi) = if unbounded_longitude {
            (0i64, cells as i64 - 1)
        } else {
            let lo = geo_lon_cell(min_lon, cells);
            let hi = geo_lon_cell(max_lon, cells);
            if hi < lo || (hi - lo) as u64 >= cells {
                (0i64, cells as i64 - 1)
            } else {
                (lo, hi)
            }
        };
        if steps > 1 && (col_hi - col_lo > 2 || row_hi.saturating_sub(row_lo) > 2) {
            steps -= 1;
            continue;
        }
        break;
    }
    let shift = 52 - (steps as u32) * 2;

    let columns = (col_hi - col_lo + 1) as usize;
    let rows = (row_hi.saturating_sub(row_lo) + 1) as usize;
    if columns.saturating_mul(rows) > GEO_MAX_SEARCH_RANGES {
        return full_scan;
    }

    // Without ASC/DESC the reply keeps the order the ranges were read in, so
    // read the cell holding the center first and the rest in Redis's neighbor
    // order (north, south, east, west, then the diagonals).
    let center_col = geo_lon_cell(spec.center_lon, cells).rem_euclid(cells as i64);
    let center_row = geo_lat_cell(spec.center_lat, cells) as i64;
    let mut cells_to_read = Vec::with_capacity(columns * rows);
    let mut seen = HashSet::with_capacity(columns * rows);
    for column in col_lo..=col_hi {
        let column = column.rem_euclid(cells as i64);
        for row in row_lo..=row_hi {
            let bits = geo_interleave64(row as u32, column as u32);
            if !seen.insert(bits) {
                continue;
            }
            // Wrap the column difference into [-cells/2, cells/2] so a cell
            // just across the date line still counts as the eastern neighbor.
            let mut column_delta = column - center_col;
            if column_delta > cells as i64 / 2 {
                column_delta -= cells as i64;
            } else if column_delta < -(cells as i64) / 2 {
                column_delta += cells as i64;
            }
            cells_to_read.push((
                geo_neighbor_rank(column_delta, row as i64 - center_row),
                bits,
            ));
        }
    }
    if cells_to_read.is_empty() {
        return full_scan;
    }
    cells_to_read.sort_by_key(|(rank, _)| *rank);
    cells_to_read
        .into_iter()
        .map(|(_, bits)| (bits << shift, (bits + 1) << shift))
        .collect()
}

/// Where a cell sits in the order `membersOfAllNeighbors` visits boxes. Cells
/// outside the immediate ring share the last rank and keep their sweep order.
fn geo_neighbor_rank(column_delta: i64, row_delta: i64) -> usize {
    match (column_delta, row_delta) {
        (0, 0) => 0,
        (0, 1) => 1,
        (0, -1) => 2,
        (1, 0) => 3,
        (-1, 0) => 4,
        (1, 1) => 5,
        (-1, 1) => 6,
        (1, -1) => 7,
        (-1, -1) => 8,
        _ => 9,
    }
}

/// Redis `geohashCalculateAreasByShapeWGS84`'s step: the estimate from the
/// radius, dropped by one when one of the four side cells sits inside the
/// search area and so cannot cover its own side.
fn geo_search_step(spec: &GeoSearchSpec, half_width: f64, half_height: f64) -> Option<u8> {
    let lon = spec.center_lon;
    let lat = spec.center_lat;
    // A box is only covered accurately by its half-diagonal.
    let radius_meters = if spec.circular {
        spec.radius_m
    } else {
        (half_width * half_width + half_height * half_height).sqrt()
    };
    let steps = geo_estimate_steps_by_radius(radius_meters, lat);
    let hash = geo_encode_wgs84(lon, lat, steps)?;
    let neighbors = geo_neighbors(hash, steps);
    let north = geo_decode_wgs84(neighbors[0], steps);
    let south = geo_decode_wgs84(neighbors[1], steps);
    let east = geo_decode_wgs84(neighbors[2], steps);
    let west = geo_decode_wgs84(neighbors[3], steps);
    let decrease_step = geo_distance(lon, lat, lon, north.lat_max) < radius_meters
        || geo_distance(lon, lat, lon, south.lat_min) < radius_meters
        || geo_distance(lon, lat, east.lon_max, lat) < radius_meters
        || geo_distance(lon, lat, west.lon_min, lat) < radius_meters;
    if steps > 1 && decrease_step {
        Some(steps - 1)
    } else {
        Some(steps)
    }
}

/// Redis `geohashGetDistanceIfInRadiusWGS84` / `geohashGetDistanceIfInRectangle`.
fn geo_distance_if_inside(spec: &GeoSearchSpec, lon: f64, lat: f64) -> Option<f64> {
    if spec.circular {
        let distance = geo_distance(spec.center_lon, spec.center_lat, lon, lat);
        if distance > spec.radius_m {
            return None;
        }
        return Some(distance);
    }
    // Latitude first: it is the cheaper of the two checks.
    if geo_lat_distance(lat, spec.center_lat) > spec.height_m / 2.0 {
        return None;
    }
    if geo_distance(lon, lat, spec.center_lon, lat) > spec.width_m / 2.0 {
        return None;
    }
    Some(geo_distance(spec.center_lon, spec.center_lat, lon, lat))
}

/// Redis `ld2string(..., LD_STR_HUMAN)`: "%.17Lf" with trailing zeros removed.
/// The last digits are not bit-identical to Redis's long double, which is why
/// the functional tests compare coordinates with a tolerance.
fn geo_format_coord(value: f64) -> String {
    let mut text = format!("{value:.17}");
    if text.contains('.') {
        while text.ends_with('0') {
            text.pop();
        }
        if text.ends_with('.') {
            text.pop();
        }
    }
    if text == "-0" {
        text.clear();
        text.push('0');
    }
    text
}

/// Redis `addReplyDoubleDistance`: fixed 4 decimals, always a bulk string.
fn geo_format_distance(value: f64) -> String {
    format!("{value:.4}")
}

/// Redis `geohashCommand`: decode the stored position, re-encode it with the
/// standard -90..90 latitude range and emit 11 base32 characters.
fn geo_hash_string(score: u64) -> Option<String> {
    const GEOALPHA: &[u8; 32] = b"0123456789bcdefghjkmnpqrstuvwxyz";
    let (lon, lat) = geo_decode_score(score);
    let bits = geo_encode(
        GEO_LONG_MIN,
        GEO_LONG_MAX,
        -90.0,
        90.0,
        lon,
        lat,
        GEO_STEP_MAX,
    )?;
    let mut out = String::with_capacity(11);
    for index in 0..11u32 {
        // 52 bits only, so the 55-bit base32 string is zero padded at the end.
        let slot = if index == 10 {
            0
        } else {
            ((bits >> (52 - (index + 1) * 5)) & 0x1f) as usize
        };
        out.push(GEOALPHA[slot] as char);
    }
    Some(out)
}

/// Redis `extractUnitOrReply`, in meters per unit. Redis compares the token
/// with strcasecmp, so `KM` and `km` are the same unit there as here.
fn geo_unit_meters(unit: &[u8]) -> Option<f64> {
    if ascii_eq_ci(unit, b"m") {
        Some(1.0)
    } else if ascii_eq_ci(unit, b"km") {
        Some(1000.0)
    } else if ascii_eq_ci(unit, b"ft") {
        Some(0.3048)
    } else if ascii_eq_ci(unit, b"mi") {
        Some(1609.34)
    } else {
        None
    }
}

/// GEOSEARCH, GEORADIUS, GEORADIUSBYMEMBER and the two _RO spellings. They
/// share the whole option tail and reply shape, so one parser and one opcode
/// cover all five; only the way the center and the shape are given differs.
fn parse_geo_search(parts: &[BytesFrame]) -> Result<Command, ParseError> {
    let name = part_to_bytes(&parts[0])?;
    // GEOSEARCHSTORE takes GEOSEARCH's whole option syntax with one extra key
    // in front of the source, so it shares `is_search` and differs only in
    // where the source key sits and in accepting a bare STOREDIST flag.
    let store_command = ascii_eq_ci(name.as_ref(), b"GEOSEARCHSTORE");
    let is_search = store_command || ascii_eq_ci(name.as_ref(), b"GEOSEARCH");
    let by_member = ascii_eq_ci(name.as_ref(), b"GEORADIUSBYMEMBER")
        || ascii_eq_ci(name.as_ref(), b"GEORADIUSBYMEMBER_RO");
    let read_only = ascii_eq_ci(name.as_ref(), b"GEORADIUS_RO")
        || ascii_eq_ci(name.as_ref(), b"GEORADIUSBYMEMBER_RO");
    let (command_name, min_args) = if store_command {
        ("geosearchstore", 7)
    } else if is_search {
        ("geosearch", 7)
    } else if by_member && read_only {
        ("georadiusbymember_ro", 5)
    } else if by_member {
        ("georadiusbymember", 5)
    } else if read_only {
        ("georadius_ro", 6)
    } else {
        ("georadius", 6)
    };
    if parts.len() < min_args {
        return Err(wrong_arity(command_name));
    }

    // GEOSEARCHSTORE destination source ...: the source is the second key.
    let key = part_to_bytes(&parts[if store_command { 2 } else { 1 }])?;
    let key = validate_user_key(&key)?;
    // The destination, once a STORE/STOREDIST option or GEOSEARCHSTORE names
    // one. `store_seen` is separate so a second STORE is a syntax error rather
    // than silently replacing the first one.
    let mut destination: Option<Bytes> = None;
    let mut store_seen = false;
    if store_command {
        let destination_key = part_to_bytes(&parts[1])?;
        let destination_key = validate_user_key(&destination_key)?;
        destination = Some(destination_key);
    }

    let mut spec = GeoSearchSpec {
        center_lon: 0.0,
        center_lat: 0.0,
        from_member: None,
        resolve_error: GeoResolve::Ok,
        circular: true,
        radius_m: 0.0,
        width_m: 0.0,
        height_m: 0.0,
        unit_meters: 1.0,
        sort: GeoSort::None,
        count: None,
        any: false,
        withcoord: false,
        withdist: false,
        withhash: false,
        store_dist: false,
    };
    let mut have_center = false;
    let mut have_shape = false;
    let mut index;

    if is_search {
        index = if store_command { 3 } else { 2 };
    } else {
        if by_member {
            spec.from_member = Some(part_to_bytes(&parts[2])?);
            index = 3;
        } else {
            let longitude = parse_f64_error_arg(
                part_to_bytes(&parts[2])?.as_ref(),
                "value is not a valid float",
            )?;
            let latitude = parse_f64_error_arg(
                part_to_bytes(&parts[3])?.as_ref(),
                "value is not a valid float",
            )?;
            geo_check_position(longitude, latitude)?;
            spec.center_lon = longitude;
            spec.center_lat = latitude;
            index = 4;
        }
        have_center = true;
        let radius =
            parse_f64_error_arg(part_to_bytes(&parts[index])?.as_ref(), "need numeric radius")?;
        if radius < 0.0 {
            return Err(ParseError::Error("radius cannot be negative"));
        }
        let unit = part_to_bytes(&parts[index + 1])?;
        let Some(meters) = geo_unit_meters(unit.as_ref()) else {
            return Err(ParseError::Error(GEO_UNIT_ERROR));
        };
        spec.circular = true;
        spec.radius_m = radius * meters;
        spec.unit_meters = meters;
        have_shape = true;
        index += 2;
    }

    while index < parts.len() {
        let arg = part_to_bytes(&parts[index])?;
        if ascii_eq_ci(arg.as_ref(), b"ASC") {
            spec.sort = GeoSort::Asc;
            index += 1;
        } else if ascii_eq_ci(arg.as_ref(), b"DESC") {
            spec.sort = GeoSort::Desc;
            index += 1;
        } else if !store_command && ascii_eq_ci(arg.as_ref(), b"WITHCOORD") {
            // GEOSEARCHSTORE writes a sorted set, so it has no output columns
            // to ask for: Redis answers a plain syntax error.
            spec.withcoord = true;
            index += 1;
        } else if !store_command && ascii_eq_ci(arg.as_ref(), b"WITHDIST") {
            spec.withdist = true;
            index += 1;
        } else if !store_command && ascii_eq_ci(arg.as_ref(), b"WITHHASH") {
            spec.withhash = true;
            index += 1;
        } else if store_command && ascii_eq_ci(arg.as_ref(), b"STOREDIST") {
            // GEOSEARCHSTORE's STOREDIST is a bare flag: the destination is
            // already the command's first argument.
            spec.store_dist = true;
            index += 1;
        } else if ascii_eq_ci(arg.as_ref(), b"COUNT") && index + 1 < parts.len() {
            let count = parse_i64_error_arg(
                part_to_bytes(&parts[index + 1])?.as_ref(),
                "value is not an integer or out of range",
            )?;
            if count <= 0 {
                return Err(ParseError::Error("COUNT must be > 0"));
            }
            spec.count = Some(count as usize);
            index += 2;
            if index < parts.len()
                && ascii_eq_ci(part_to_bytes(&parts[index])?.as_ref(), b"ANY")
            {
                spec.any = true;
                index += 1;
            }
        } else if ascii_eq_ci(arg.as_ref(), b"ANY") {
            // ANY only means anything attached to a COUNT.
            return Err(ParseError::Error("the ANY argument requires COUNT argument"));
        } else if is_search
            && !have_center
            && ascii_eq_ci(arg.as_ref(), b"FROMMEMBER")
            && index + 1 < parts.len()
        {
            spec.from_member = Some(part_to_bytes(&parts[index + 1])?);
            have_center = true;
            index += 2;
        } else if is_search
            && !have_center
            && ascii_eq_ci(arg.as_ref(), b"FROMLONLAT")
            && index + 2 < parts.len()
        {
            let longitude = parse_f64_error_arg(
                part_to_bytes(&parts[index + 1])?.as_ref(),
                "value is not a valid float",
            )?;
            let latitude = parse_f64_error_arg(
                part_to_bytes(&parts[index + 2])?.as_ref(),
                "value is not a valid float",
            )?;
            geo_check_position(longitude, latitude)?;
            spec.center_lon = longitude;
            spec.center_lat = latitude;
            have_center = true;
            index += 3;
        } else if is_search
            && !have_shape
            && ascii_eq_ci(arg.as_ref(), b"BYRADIUS")
            && index + 2 < parts.len()
        {
            let radius = parse_f64_error_arg(
                part_to_bytes(&parts[index + 1])?.as_ref(),
                "need numeric radius",
            )?;
            if radius < 0.0 {
                return Err(ParseError::Error("radius cannot be negative"));
            }
            let unit = part_to_bytes(&parts[index + 2])?;
            let Some(meters) = geo_unit_meters(unit.as_ref()) else {
                return Err(ParseError::Error(GEO_UNIT_ERROR));
            };
            spec.circular = true;
            spec.radius_m = radius * meters;
            spec.unit_meters = meters;
            have_shape = true;
            index += 3;
        } else if is_search
            && !have_shape
            && ascii_eq_ci(arg.as_ref(), b"BYBOX")
            && index + 3 < parts.len()
        {
            let width = parse_f64_error_arg(
                part_to_bytes(&parts[index + 1])?.as_ref(),
                "need numeric width",
            )?;
            let height = parse_f64_error_arg(
                part_to_bytes(&parts[index + 2])?.as_ref(),
                "need numeric height",
            )?;
            if width < 0.0 || height < 0.0 {
                return Err(ParseError::Error("height or width cannot be negative"));
            }
            let unit = part_to_bytes(&parts[index + 3])?;
            let Some(meters) = geo_unit_meters(unit.as_ref()) else {
                return Err(ParseError::Error(GEO_UNIT_ERROR));
            };
            spec.circular = false;
            spec.width_m = width * meters;
            spec.height_m = height * meters;
            spec.unit_meters = meters;
            have_shape = true;
            index += 4;
        } else if !is_search
            && !read_only
            && (ascii_eq_ci(arg.as_ref(), b"STORE") || ascii_eq_ci(arg.as_ref(), b"STOREDIST"))
            && index + 1 < parts.len()
        {
            // GEORADIUS / GEORADIUSBYMEMBER name the destination after the
            // option. The two spellings are mutually exclusive; the _RO forms
            // never reach here, so they answer a plain syntax error.
            if store_seen {
                return Err(ParseError::Error("syntax error"));
            }
            store_seen = true;
            spec.store_dist = ascii_eq_ci(arg.as_ref(), b"STOREDIST");
            let store_key = part_to_bytes(&parts[index + 1])?;
            let store_key = validate_user_key(&store_key)?;
            destination = Some(store_key);
            index += 2;
        } else {
            return Err(ParseError::Error("syntax error"));
        }
    }

    // Redis refuses the output columns together with a destination, because a
    // sorted set has nowhere to put them.
    if destination.is_some() && (spec.withcoord || spec.withdist || spec.withhash) {
        return Err(ParseError::Error(GEO_STORE_WITH_ERROR));
    }

    if !have_center {
        return Err(ParseError::Error(
            "exactly one of FROMMEMBER or FROMLONLAT can be specified for GEOSEARCH",
        ));
    }
    if !have_shape {
        return Err(ParseError::Error(
            "exactly one of BYRADIUS and BYBOX can be specified for GEOSEARCH",
        ));
    }

    let mut cmd = match destination {
        // ZRANGESTORE's convention: the written key is the command's key and
        // the read key travels in `values`, so WATCH invalidation and the
        // dirty-key bookkeeping name only the destination.
        Some(destination) => {
            let mut cmd = Command::new(
                OpCode::GeoSearchStore,
                vec![destination],
                None,
                command_args(parts).ok_or(ParseError::Protocol("invalid argument"))?,
            );
            cmd.values = vec![key];
            cmd
        }
        None => Command::new(
            OpCode::GeoSearch,
            vec![key],
            None,
            command_args(parts).ok_or(ParseError::Protocol("invalid argument"))?,
        ),
    };
    cmd.geo = Some(Box::new(spec));
    Ok(cmd)
}

/// Redis `extractLongLatOrReply`'s range check, with its exact error text.
fn geo_check_position(longitude: f64, latitude: f64) -> Result<(), ParseError> {
    if !(GEO_LONG_MIN..=GEO_LONG_MAX).contains(&longitude)
        || !(GEO_LAT_MIN..=GEO_LAT_MAX).contains(&latitude)
    {
        return Err(ParseError::Owned(format!(
            "invalid longitude,latitude pair {longitude:.6},{latitude:.6}"
        )));
    }
    Ok(())
}

/// Outcome of looking a geo member's position up.
enum GeoMemberLookup {
    Position(f64, f64),
    KeyMissing,
    MemberMissing,
    WrongType,
    Failed,
}

/// Run an already-built op list as its own request, with the same retry policy
/// as `ffi_execute_single`, and hand the results to `consume`. The response is
/// freed before returning.
fn ffi_run_ops<T>(ops: &[TxnOperation], consume: impl FnOnce(Option<&[TxnOpResult]>) -> T) -> T {
    if redis_backend() == RedisBackend::Memory {
        let owned = memory_execute_transaction(ops);
        let response = owned.as_response();
        if response.num_results >= ops.len() {
            return consume(Some(unsafe {
                std::slice::from_raw_parts(response.results, ops.len())
            }));
        }
        return consume(None);
    }

    let request = TxnRequest {
        num_ops: ops.len(),
        ops: ops.as_ptr(),
    };
    let mut response = TxnResponse {
        transaction_success: false,
        num_results: 0,
        results: std::ptr::null_mut(),
    };
    let mut call_ok = false;
    for attempt in 0..TXN_MAX_ATTEMPTS {
        response = TxnResponse {
            transaction_success: false,
            num_results: 0,
            results: std::ptr::null_mut(),
        };
        call_ok = unsafe { cpp_execute_transaction(&request, &mut response) };
        if call_ok && response.transaction_success && response.num_results >= ops.len() {
            break;
        }
        unsafe { cpp_free_transaction_response(&mut response) };
        if attempt + 1 < TXN_MAX_ATTEMPTS {
            unsafe { cpp_record_txn_retry() };
            sleep_for_retry(attempt);
        }
    }

    let value = if call_ok && response.transaction_success && response.num_results >= ops.len() {
        consume(Some(unsafe {
            std::slice::from_raw_parts(response.results, ops.len())
        }))
    } else {
        consume(None)
    };
    unsafe { cpp_free_transaction_response(&mut response) };
    value
}

/// FROMMEMBER / GEORADIUSBYMEMBER center lookup. It is its own request because
/// a later op in a request cannot read an earlier op's result, so the range
/// reads that follow cannot see this score. Recorded in known_divergences.txt.
fn geo_lookup_member(key: &Bytes, member: &Bytes) -> GeoMemberLookup {
    let ops = geo_member_lookup_ops(key, member);
    ffi_run_ops(&ops, |results| match results {
        Some(results) => geo_member_lookup_result(results),
        None => GeoMemberLookup::Failed,
    })
}

/// ZSCORE says where the member is; ZCARD separates "there is no such key"
/// (Redis answers an empty result) from "the key has no such member" (Redis
/// answers an error). Both read the same key, so they go out together.
fn geo_member_lookup_ops(key: &Bytes, member: &Bytes) -> [TxnOperation; 2] {
    [
        TxnOperation {
            op: TXN_OP_ZSCORE,
            key_ptr: key.as_ptr(),
            key_len: key.len(),
            val_ptr: member.as_ptr(),
            val_len: member.len(),
            flags: 0,
            expire_at_ms: -1,
            group_id: 0,
        },
        TxnOperation {
            op: TXN_OP_ZCARD,
            key_ptr: key.as_ptr(),
            key_len: key.len(),
            val_ptr: std::ptr::null(),
            val_len: 0,
            flags: 0,
            expire_at_ms: -1,
            group_id: 0,
        },
    ]
}

fn geo_member_lookup_result(results: &[TxnOpResult]) -> GeoMemberLookup {
    if results.len() < 2 {
        return GeoMemberLookup::Failed;
    }
    let (score, cardinality) = (&results[0], &results[1]);
    if !score.success || !cardinality.success {
        return GeoMemberLookup::WrongType;
    }
    if score.value_present {
        if let Some(bits) = geo_score_from_bytes(result_value_bytes(score)) {
            let (lon, lat) = geo_decode_score(bits);
            return GeoMemberLookup::Position(lon, lat);
        }
    }
    if cardinality.int_value <= 0 {
        GeoMemberLookup::KeyMissing
    } else {
        GeoMemberLookup::MemberMissing
    }
}

/// Resolve a FROMMEMBER search center. Returns a copy of the command with the
/// center filled in, or None when there is nothing to resolve.
fn geo_resolve_search_center(cmd: &Command) -> Option<Command> {
    let spec = cmd.geo.as_deref()?;
    let member = spec.from_member.clone()?;
    let key = cmd.keys.first()?.clone();
    let lookup = geo_lookup_member(&key, &member);
    let mut resolved = cmd.clone();
    let spec = resolved.geo.as_deref_mut()?;
    spec.from_member = None;
    match lookup {
        GeoMemberLookup::Position(lon, lat) => {
            spec.center_lon = lon;
            spec.center_lat = lat;
        }
        GeoMemberLookup::KeyMissing => spec.resolve_error = GeoResolve::KeyMissing,
        GeoMemberLookup::MemberMissing => spec.resolve_error = GeoResolve::MemberMissing,
        GeoMemberLookup::WrongType => spec.resolve_error = GeoResolve::WrongType,
        GeoMemberLookup::Failed => spec.resolve_error = GeoResolve::Failed,
    }
    Some(resolved)
}

/// A distance as a ZADD score. Rust's `{:?}` is the shortest decimal that
/// parses back to the same double, which is what the executor's `strtod`
/// needs; the geohash form goes in as the integer GEOADD itself writes.
fn geo_format_store_score(value: f64) -> String {
    if value == 0.0 {
        return String::from("0");
    }
    format!("{value:?}")
}

/// GEOSEARCHSTORE, and GEORADIUS / GEORADIUSBYMEMBER with STORE or STOREDIST.
///
/// The whole command is one open session, so the FROMMEMBER center lookup, the
/// neighbor-box range reads, the destination delete and the ZADD of the
/// matches are one Mako transaction. That is what the session interface is
/// for: the op list of the write cannot be built until the reads have
/// answered, and `cpp_execute_transaction` takes its whole op list up front.
/// It also makes the FROMMEMBER center exact here, where the read-only
/// searches read it in a request of their own (see known_divergences.txt).
fn geo_search_store_in_session(session: &mut SessionTxn, cmd: &Command) -> Result<Vec<u8>, ()> {
    let mut reply = Vec::new();
    let (Some(spec), Some(destination), Some(source)) =
        (cmd.geo.as_deref(), cmd.keys.first(), cmd.values.first())
    else {
        write_err(&mut reply, "operation failed").map_err(|_| ())?;
        return Ok(reply);
    };
    let mut spec = spec.clone();

    if let Some(member) = spec.from_member.take() {
        let lookup_ops = geo_member_lookup_ops(source, &member);
        match session.run_ops(&lookup_ops, geo_member_lookup_result)? {
            GeoMemberLookup::Position(lon, lat) => {
                spec.center_lon = lon;
                spec.center_lat = lat;
            }
            // Redis searches an empty set when the source is not there, which
            // with a destination means "delete it and answer 0".
            GeoMemberLookup::KeyMissing => {
                geo_store_replace(session, destination, &spec, &[])?;
                write_integer(&mut reply, 0).map_err(|_| ())?;
                return Ok(reply);
            }
            // A key that exists but has no such member is an error, and the
            // destination is left exactly as it was.
            GeoMemberLookup::MemberMissing => {
                write_err(&mut reply, "could not decode requested zset member").map_err(|_| ())?;
                return Ok(reply);
            }
            GeoMemberLookup::WrongType => {
                write_wrongtype(&mut reply).map_err(|_| ())?;
                return Ok(reply);
            }
            GeoMemberLookup::Failed => return Err(()),
        }
    }

    let (range_ops, _payloads) = geo_range_ops(source, &spec);
    let matches = session.run_ops(&range_ops, |results| geo_collect_matches(&spec, results))?;
    let mut points = match matches {
        GeoMatches::Points(points) => points,
        GeoMatches::WrongType => {
            write_wrongtype(&mut reply).map_err(|_| ())?;
            return Ok(reply);
        }
        GeoMatches::Failed => {
            write_err(&mut reply, "operation failed").map_err(|_| ())?;
            return Ok(reply);
        }
    };
    let returned = geo_sort_matches(&spec, &mut points);
    geo_store_replace(session, destination, &spec, &points[..returned])?;
    write_integer(&mut reply, returned as i64).map_err(|_| ())?;
    Ok(reply)
}

/// Replace `destination` with `points`, whatever type it held. Redis deletes
/// the key outright when nothing matched, so DEL runs either way; the
/// executor's deferred deletes are what let the ZADD rewrite the same key
/// inside the same transaction.
fn geo_store_replace(
    session: &mut SessionTxn,
    destination: &Bytes,
    spec: &GeoSearchSpec,
    points: &[GeoPoint],
) -> Result<(), ()> {
    let delete = [TxnOperation {
        op: TXN_OP_DEL,
        key_ptr: destination.as_ptr(),
        key_len: destination.len(),
        val_ptr: std::ptr::null(),
        val_len: 0,
        flags: 0,
        expire_at_ms: -1,
        group_id: 0,
    }];
    session.run_ops(&delete, |_| ())?;
    if points.is_empty() {
        return Ok(());
    }
    let mut members = Vec::with_capacity(points.len() * 2);
    for point in points {
        // Without STOREDIST the score is the member's own geohash, so GEOPOS,
        // GEODIST and GEOHASH all work on the destination; with it, Redis
        // converts the distance to the unit the search asked for.
        let score = if spec.store_dist {
            geo_format_store_score(point.dist / spec.unit_meters)
        } else {
            point.score.to_string()
        };
        members.push(Bytes::from(score));
        members.push(Bytes::copy_from_slice(&point.member));
    }
    let payload = pack_bytes_list(&members);
    let add = [TxnOperation {
        op: TXN_OP_ZADD,
        key_ptr: destination.as_ptr(),
        key_len: destination.len(),
        val_ptr: payload.as_ptr(),
        val_len: payload.len(),
        flags: 0,
        expire_at_ms: -1,
        group_id: 0,
    }];
    match session.run_ops(&add, |results| results[0].success)? {
        true => Ok(()),
        // The destination was just deleted, so a refusal here is not a type
        // clash: treat it as a broken session and let the caller re-run.
        false => Err(()),
    }
}

/// GEOSEARCHSTORE outside MULTI. `ffi_run_session` owns the bounded retry on
/// an optimistic-concurrency abort, exactly as it does for a script, and the
/// declared keys are the destination and the source so both lock stripes are
/// held for the transaction.
fn geo_search_store_command<W: Write>(
    cmd: &Command,
    protocol_version: u8,
    writer: &mut W,
) -> std::io::Result<()> {
    let mut keys = cmd.keys.clone();
    keys.extend(cmd.values.iter().cloned());
    let reply = ffi_run_session_as(&keys, protocol_version, |session| {
        match session.execute(cmd) {
            Ok(reply) => SessionRun::Commit(reply),
            Err(()) => SessionRun::Retry,
        }
    });
    match reply {
        Some(reply) => writer.write_all(&reply),
        None => write_err(writer, "backend"),
    }
}

/// One candidate of a geo search, after the exact shape test.
struct GeoPoint {
    member: Vec<u8>,
    lon: f64,
    lat: f64,
    score: u64,
    dist: f64,
}

/// What the neighbor-box results turned into.
enum GeoMatches {
    Points(Vec<GeoPoint>),
    /// One of the range reads refused the key: it holds another type.
    WrongType,
    /// A range read came back in a shape this cannot read.
    Failed,
}

/// Merge the candidates of every neighbor box, drop the ones outside the exact
/// shape and de-duplicate the members the boxes share. Shared by the GEOSEARCH
/// reply and by GEOSEARCHSTORE, which runs the same reads inside a session.
fn geo_collect_matches(spec: &GeoSearchSpec, boxes: &[TxnOpResult]) -> GeoMatches {
    let mut points: Vec<GeoPoint> = Vec::new();
    let mut seen: HashSet<Vec<u8>> = HashSet::new();
    // COUNT ... ANY stops as soon as enough candidates are in hand.
    let early_stop = if spec.any { spec.count } else { None };
    'boxes: for result in boxes {
        if !result.success {
            return GeoMatches::WrongType;
        }
        let data = result_value_bytes(result);
        let items = if data.is_empty() {
            Vec::new()
        } else {
            match parse_list_payload(data) {
                Some(items) => items,
                None => return GeoMatches::Failed,
            }
        };
        for pair in items.chunks_exact(2) {
            let Some(score) = geo_score_from_bytes(&pair[1]) else {
                continue;
            };
            let (lon, lat) = geo_decode_score(score);
            let Some(dist) = geo_distance_if_inside(spec, lon, lat) else {
                continue;
            };
            if !seen.insert(pair[0].clone()) {
                continue;
            }
            points.push(GeoPoint {
                member: pair[0].clone(),
                lon,
                lat,
                score,
                dist,
            });
            if let Some(limit) = early_stop {
                if points.len() >= limit {
                    break 'boxes;
                }
            }
        }
    }
    GeoMatches::Points(points)
}

/// Redis's ordering rule, then COUNT. Returns how many of `points` are
/// reported: Redis sorts ascending before truncating when COUNT was given
/// without ANY, because the N closest are what COUNT means.
fn geo_sort_matches(spec: &GeoSearchSpec, points: &mut [GeoPoint]) -> usize {
    let mut sort = spec.sort;
    if spec.count.is_some() && sort == GeoSort::None && !spec.any {
        sort = GeoSort::Asc;
    }
    match sort {
        GeoSort::Asc => points.sort_by(|a, b| {
            a.dist
                .partial_cmp(&b.dist)
                .unwrap_or(std::cmp::Ordering::Equal)
        }),
        GeoSort::Desc => points.sort_by(|a, b| {
            b.dist
                .partial_cmp(&a.dist)
                .unwrap_or(std::cmp::Ordering::Equal)
        }),
        GeoSort::None => {}
    }
    spec.count.map_or(points.len(), |c| c.min(points.len()))
}

/// The ZRANGEBYSCORE-with-scores op list one geo search needs, one op per
/// neighbor box. The payloads must outlive the ops, so the caller owns them.
fn geo_range_ops(key: &Bytes, spec: &GeoSearchSpec) -> (Vec<TxnOperation>, Vec<Bytes>) {
    let ranges = geo_search_ranges(spec);
    let mut payloads = Vec::with_capacity(ranges.len());
    for (min, max) in &ranges {
        // Redis reads [min, max): the max is the next box's min.
        let bounds = [
            Bytes::from(min.to_string()),
            Bytes::from(format!("({max}")),
        ];
        payloads.push(pack_bytes_list(&bounds));
    }
    let ops = payloads
        .iter()
        .map(|payload| TxnOperation {
            op: TXN_OP_ZRANGE,
            key_ptr: key.as_ptr(),
            key_len: key.len(),
            val_ptr: payload.as_ptr(),
            val_len: payload.len(),
            flags: TXN_FLAG_Z_BYSCORE | TXN_FLAG_Z_WITHSCORES,
            expire_at_ms: -1,
            group_id: 0,
        })
        .collect();
    (ops, payloads)
}

fn result_value_bytes(result: &TxnOpResult) -> &[u8] {
    if result.value_present && result.data_len > 0 && !result.data_ptr.is_null() {
        unsafe { std::slice::from_raw_parts(result.data_ptr, result.data_len) }
    } else {
        &[]
    }
}

fn parse_i64_lossy(value: &[u8]) -> Option<i64> {
    std::str::from_utf8(value).ok()?.parse().ok()
}

const BITFIELD_TYPE_ERROR: &str =
    "Invalid bitfield type. Use something like i16 u8. Note that u64 is not supported but i64 is.";

fn parse_bitfield_encoding(encoding: &[u8]) -> Result<(bool, u32), ParseError> {
    if encoding.len() < 2 {
        return Err(ParseError::Error(BITFIELD_TYPE_ERROR));
    }
    let signed = match encoding[0] {
        b'i' => true,
        b'u' => false,
        _ => return Err(ParseError::Error(BITFIELD_TYPE_ERROR)),
    };
    let bits: u32 = std::str::from_utf8(&encoding[1..])
        .ok()
        .and_then(|text| text.parse().ok())
        .ok_or(ParseError::Error(BITFIELD_TYPE_ERROR))?;
    if bits == 0 || (signed && bits > 64) || (!signed && bits > 63) {
        return Err(ParseError::Error(BITFIELD_TYPE_ERROR));
    }
    Ok((signed, bits))
}

fn parse_bitfield_offset(offset: &[u8], bits: u32) -> Result<u64, ParseError> {
    const ERR: &str = "bit offset is not an integer or out of range";
    let (text, multiply) = match offset.first() {
        Some(b'#') => (&offset[1..], true),
        _ => (offset, false),
    };
    let value: i64 = std::str::from_utf8(text)
        .ok()
        .and_then(|text| text.parse().ok())
        .ok_or(ParseError::Error(ERR))?;
    if value < 0 {
        return Err(ParseError::Error(ERR));
    }
    let value = value as u64;
    let result = if multiply {
        value.checked_mul(bits as u64).ok_or(ParseError::Error(ERR))?
    } else {
        value
    };
    // Redis caps strings at 512 MiB, i.e. 2^32 bits.
    if result.saturating_add(bits as u64) > (1u64 << 32) {
        return Err(ParseError::Error(ERR));
    }
    Ok(result)
}

/// Normalize a Redis inclusive [start, end] range over `len_units` items,
/// honoring negative indexes. Returns None for an empty range.
fn bit_range_bounds(len_units: i64, start: i64, end: i64) -> Option<(usize, usize)> {
    if len_units <= 0 {
        return None;
    }
    let mut start = if start < 0 { start + len_units } else { start };
    let mut end = if end < 0 { end + len_units } else { end };
    if start < 0 {
        start = 0;
    }
    if end < 0 {
        end = 0;
    }
    if end >= len_units {
        end = len_units - 1;
    }
    if start > end {
        return None;
    }
    Some((start as usize, end as usize))
}

fn count_bits_in_bit_range(data: &[u8], start_bit: usize, end_bit: usize) -> i64 {
    let mut count = 0i64;
    for bit in start_bit..=end_bit {
        let byte = data[bit / 8];
        if byte & (0x80u8 >> (bit % 8)) != 0 {
            count += 1;
        }
    }
    count
}

fn bitcount_in_range(data: &[u8], values: &[Bytes]) -> Result<i64, &'static str> {
    if values.len() < 2 {
        return Ok(data.iter().map(|byte| byte.count_ones() as i64).sum());
    }
    const ERR: &str = "value is not an integer or out of range";
    let start = parse_i64_lossy(&values[0]).ok_or(ERR)?;
    let end = parse_i64_lossy(&values[1]).ok_or(ERR)?;
    let bit_unit = values
        .get(2)
        .map(|unit| ascii_eq_ci(unit.as_ref(), b"BIT"))
        .unwrap_or(false);
    if bit_unit {
        let Some((first, last)) = bit_range_bounds(data.len() as i64 * 8, start, end) else {
            return Ok(0);
        };
        Ok(count_bits_in_bit_range(data, first, last))
    } else {
        let Some((first, last)) = bit_range_bounds(data.len() as i64, start, end) else {
            return Ok(0);
        };
        Ok(data[first..=last]
            .iter()
            .map(|byte| byte.count_ones() as i64)
            .sum())
    }
}

fn bitpos_in_range(data: &[u8], exists: bool, values: &[Bytes]) -> Result<i64, &'static str> {
    const ERR: &str = "value is not an integer or out of range";
    let want_set = values.first().map(|bit| bit.as_ref() == b"1").unwrap_or(true);
    if !exists || data.is_empty() {
        return Ok(if want_set { -1 } else { 0 });
    }
    let end_given = values.len() >= 3;
    let bit_unit = values
        .get(3)
        .map(|unit| ascii_eq_ci(unit.as_ref(), b"BIT"))
        .unwrap_or(false);
    let total_units = if bit_unit {
        data.len() as i64 * 8
    } else {
        data.len() as i64
    };
    let start = match values.get(1) {
        Some(start) => parse_i64_lossy(start).ok_or(ERR)?,
        None => 0,
    };
    let end = if end_given {
        parse_i64_lossy(&values[2]).ok_or(ERR)?
    } else {
        total_units - 1
    };
    let Some((first, last)) = bit_range_bounds(total_units, start, end) else {
        return Ok(-1);
    };
    let (start_bit, end_bit) = if bit_unit {
        (first, last)
    } else {
        (first * 8, last * 8 + 7)
    };
    for position in start_bit..=end_bit {
        let is_set = data[position / 8] & (0x80u8 >> (position % 8)) != 0;
        if is_set == want_set {
            return Ok(position as i64);
        }
    }
    if !want_set && !end_given {
        // Redis: with no explicit end, a string of all ones reports the first
        // clear bit just past the end of the string.
        return Ok(end_bit as i64 + 1);
    }
    Ok(-1)
}

fn bitfield_ro_values(data: &[u8], values: &[Bytes]) -> Vec<i64> {
    let mut out = Vec::new();
    for pair in values.chunks_exact(2) {
        let Ok((signed, bits)) = parse_bitfield_encoding(pair[0].as_ref()) else {
            continue;
        };
        let Ok(offset) = parse_bitfield_offset(pair[1].as_ref(), bits) else {
            continue;
        };
        let mut value: u64 = 0;
        for index in 0..bits as u64 {
            let bit_index = offset + index;
            let byte_index = (bit_index / 8) as usize;
            let bit = if byte_index < data.len() {
                (data[byte_index] >> (7 - (bit_index % 8))) & 1
            } else {
                0
            };
            value = (value << 1) | bit as u64;
        }
        let result = if signed && bits < 64 && (value >> (bits - 1)) & 1 == 1 {
            (value | (u64::MAX << bits)) as i64
        } else {
            value as i64
        };
        out.push(result);
    }
    out
}

// ===== Compatibility shims for observability and ACL commands =====

fn write_string_array<W: Write>(writer: &mut W, items: &[&str]) -> std::io::Result<()> {
    write_array_header(writer, items.len())?;
    for item in items {
        write_bulk(writer, item.as_bytes())?;
    }
    Ok(())
}

// ===== Single-node CLUSTER emulation =====
//
// `MAKO_REDIS_CLUSTER_MODE=emulated` makes this one server describe itself as a
// one-node cluster owning every slot, the way Dragonfly's emulated cluster mode
// does: client libraries that insist on speaking to a cluster can build their
// slot map, discover the single node and keep using it. Nothing is sharded.
// There is one keyspace, one node, no MOVED/ASK redirection, no epoch bumps and
// no slot migration, so the emulation is descriptive only. The default `off`
// leaves the pre-package-5 behavior in place: CLUSTER, READONLY and READWRITE
// all report that cluster support is disabled.

const CLUSTER_SLOT_COUNT: u16 = 16384;
const CLUSTER_LAST_SLOT: u16 = CLUSTER_SLOT_COUNT - 1;
const CLUSTER_DISABLED_ERROR: &str = "This instance has cluster support disabled";
/// Redis's cluster bus listens on the client port plus this offset.
const CLUSTER_BUS_PORT_OFFSET: u32 = 10000;

#[derive(Copy, Clone, PartialEq, Eq, Debug)]
enum ClusterMode {
    Off,
    Emulated,
}

static CLUSTER_MODE: OnceLock<ClusterMode> = OnceLock::new();
static CLUSTER_ANNOUNCE: OnceLock<(String, u32)> = OnceLock::new();
static CLUSTER_NODE_ID: OnceLock<String> = OnceLock::new();

fn cluster_mode() -> ClusterMode {
    *CLUSTER_MODE.get_or_init(|| match env::var("MAKO_REDIS_CLUSTER_MODE") {
        Ok(value) if value.eq_ignore_ascii_case("emulated") => ClusterMode::Emulated,
        Ok(value) if value.eq_ignore_ascii_case("off") => ClusterMode::Off,
        Ok(value) => {
            eprintln!("Unknown MAKO_REDIS_CLUSTER_MODE={value}; defaulting to off");
            ClusterMode::Off
        }
        Err(_) => ClusterMode::Off,
    })
}

/// The host and port this node advertises in SLOTS/SHARDS/NODES. A client that
/// reached the server through a forwarder needs the address it should dial, not
/// the bind address, so the announce pair wins over the listen pair.
fn cluster_announce() -> &'static (String, u32) {
    CLUSTER_ANNOUNCE.get_or_init(|| {
        let host = env::var("MAKO_REDIS_ANNOUNCE_HOST")
            .or_else(|_| env::var("MAKO_HOST"))
            .unwrap_or_else(|_| "127.0.0.1".to_string());
        let port = env::var("MAKO_REDIS_ANNOUNCE_PORT")
            .or_else(|_| env::var("MAKO_PORT"))
            .ok()
            .and_then(|value| value.parse::<u32>().ok())
            .unwrap_or(6380);
        (host, port)
    })
}

/// A stable 40-hex-character node id, the shape Redis writes into nodes.conf.
/// Redis fills its own with random bytes and remembers it on disk; this one has
/// to be the same after a restart without a file to remember it in, so it is
/// derived from the advertised address by five FNV-1a passes, each seeded
/// differently and contributing eight hex digits. It is not a SHA-1 and makes
/// no cryptographic claim: it only has to be constant and well-formed.
fn cluster_node_id() -> &'static str {
    CLUSTER_NODE_ID.get_or_init(|| {
        let (host, port) = cluster_announce();
        let address = format!("{host}:{port}");
        let mut id = String::with_capacity(40);
        for round in 0..5u64 {
            let mut hash: u64 =
                0xcbf2_9ce4_8422_2325 ^ round.wrapping_mul(0x9e37_79b9_7f4a_7c15);
            for byte in address.as_bytes() {
                hash ^= u64::from(*byte);
                hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
            }
            // One finalizing round so the high bits, which is what gets
            // printed, depend on every input byte.
            hash ^= hash >> 33;
            hash = hash.wrapping_mul(0xff51_afd7_ed55_8ccd);
            hash ^= hash >> 29;
            id.push_str(&format!("{:08x}", (hash >> 32) as u32));
        }
        id
    })
}

/// Redis `crc16`: CRC-16/XMODEM, polynomial 0x1021, zero init, no reflection,
/// no final xor. Computed a bit at a time rather than from Redis's 256-entry
/// table; CLUSTER KEYSLOT is not on a hot path.
fn cluster_crc16(data: &[u8]) -> u16 {
    let mut crc: u16 = 0;
    for byte in data {
        crc ^= u16::from(*byte) << 8;
        for _ in 0..8 {
            crc = if crc & 0x8000 != 0 {
                (crc << 1) ^ 0x1021
            } else {
                crc << 1
            };
        }
    }
    crc
}

/// Redis `keyHashSlot`: hash only what lies between the first `{` and the first
/// `}` after it, and only when that span is non-empty; otherwise hash the whole
/// key. `{}` and `foo{}{bar}` therefore hash whole, which is what makes hash
/// tags composable.
fn cluster_key_slot(key: &[u8]) -> u16 {
    let whole = |key: &[u8]| cluster_crc16(key) % CLUSTER_SLOT_COUNT;
    let Some(open) = key.iter().position(|byte| *byte == b'{') else {
        return whole(key);
    };
    let Some(close) = key[open + 1..].iter().position(|byte| *byte == b'}') else {
        return whole(key);
    };
    if close == 0 {
        return whole(key);
    }
    cluster_crc16(&key[open + 1..open + 1 + close]) % CLUSTER_SLOT_COUNT
}

fn cluster_info_body() -> String {
    let mut out = String::new();
    out.push_str("cluster_state:ok\r\n");
    out.push_str("cluster_slots_assigned:16384\r\n");
    out.push_str("cluster_slots_ok:16384\r\n");
    out.push_str("cluster_slots_pfail:0\r\n");
    out.push_str("cluster_slots_fail:0\r\n");
    out.push_str("cluster_known_nodes:1\r\n");
    out.push_str("cluster_size:1\r\n");
    out.push_str("cluster_current_epoch:1\r\n");
    out.push_str("cluster_my_epoch:1\r\n");
    out.push_str("cluster_stats_messages_sent:0\r\n");
    out.push_str("cluster_stats_messages_received:0\r\n");
    out
}

/// The one line Redis's CLUSTER NODES would print for a single master holding
/// every slot: `<id> <ip>:<port>@<bus> myself,master - <ping> <pong> <epoch>
/// connected <slots>`.
fn cluster_nodes_body() -> String {
    let (host, port) = cluster_announce();
    let id = cluster_node_id();
    let bus = u64::from(*port) + u64::from(CLUSTER_BUS_PORT_OFFSET);
    format!("{id} {host}:{port}@{bus} myself,master - 0 0 1 connected 0-{CLUSTER_LAST_SLOT}\n")
}

/// READONLY and READWRITE: in a real cluster they steer reads at replicas, and
/// with one master and no replicas there is nothing to steer, so emulated mode
/// accepts both and does nothing.
fn write_cluster_readonly_reply<W: Write>(
    mode: ClusterMode,
    writer: &mut W,
) -> std::io::Result<()> {
    if mode == ClusterMode::Emulated {
        write_simple_ok(writer)
    } else {
        write_err(writer, CLUSTER_DISABLED_ERROR)
    }
}

fn handle_cluster_command<W: Write>(
    cmd: &Command,
    mode: ClusterMode,
    writer: &mut W,
) -> std::io::Result<()> {
    if mode == ClusterMode::Off {
        return write_err(writer, CLUSTER_DISABLED_ERROR);
    }
    let subcommand = cmd.args.first().map(|arg| arg.as_ref()).unwrap_or(b"");
    let (host, port) = cluster_announce();
    let id = cluster_node_id();

    if ascii_eq_ci(subcommand, b"INFO") {
        write_bulk(writer, cluster_info_body().as_bytes())
    } else if ascii_eq_ci(subcommand, b"MYID") {
        write_bulk(writer, id.as_bytes())
    } else if ascii_eq_ci(subcommand, b"SLOTS") {
        write_array_header(writer, 1)?;
        write_array_header(writer, 3)?;
        write_integer(writer, 0)?;
        write_integer(writer, i64::from(CLUSTER_LAST_SLOT))?;
        write_array_header(writer, 3)?;
        write_bulk(writer, host.as_bytes())?;
        write_integer(writer, i64::from(*port))?;
        write_bulk(writer, id.as_bytes())
    } else if ascii_eq_ci(subcommand, b"SHARDS") {
        write_array_header(writer, 1)?;
        write_array_header(writer, 4)?;
        write_bulk(writer, b"slots")?;
        write_array_header(writer, 2)?;
        write_integer(writer, 0)?;
        write_integer(writer, i64::from(CLUSTER_LAST_SLOT))?;
        write_bulk(writer, b"nodes")?;
        write_array_header(writer, 1)?;
        write_array_header(writer, 14)?;
        write_bulk(writer, b"id")?;
        write_bulk(writer, id.as_bytes())?;
        write_bulk(writer, b"port")?;
        write_integer(writer, i64::from(*port))?;
        write_bulk(writer, b"ip")?;
        write_bulk(writer, host.as_bytes())?;
        write_bulk(writer, b"endpoint")?;
        write_bulk(writer, host.as_bytes())?;
        write_bulk(writer, b"role")?;
        write_bulk(writer, b"master")?;
        write_bulk(writer, b"replication-offset")?;
        write_integer(writer, 0)?;
        write_bulk(writer, b"health")?;
        write_bulk(writer, b"online")
    } else if ascii_eq_ci(subcommand, b"NODES") {
        write_bulk(writer, cluster_nodes_body().as_bytes())
    } else if ascii_eq_ci(subcommand, b"KEYSLOT") {
        if cmd.args.len() != 2 {
            return write_err(
                writer,
                "wrong number of arguments for 'cluster|keyslot' command",
            );
        }
        write_integer(writer, i64::from(cluster_key_slot(cmd.args[1].as_ref())))
    } else if ascii_eq_ci(subcommand, b"COUNTKEYSINSLOT") {
        // Counting is a keyspace scan per call, which this executor answers
        // with a full pass over storage; with every key in the one shard the
        // answer is only ever "all of them" or nothing, so it stays 0. See
        // known_divergences.txt.
        match cluster_slot_argument(cmd.args.get(1)) {
            Some(_) => write_integer(writer, 0),
            None => write_err(writer, "Invalid slot"),
        }
    } else if ascii_eq_ci(subcommand, b"GETKEYSINSLOT") {
        if cmd.args.len() != 3 {
            return write_err(
                writer,
                "wrong number of arguments for 'cluster|getkeysinslot' command",
            );
        }
        match cluster_slot_argument(cmd.args.get(1)) {
            Some(_) => write_array_header(writer, 0),
            None => write_err(writer, "Invalid slot"),
        }
    } else if ascii_eq_ci(subcommand, b"HELP") {
        write_string_array(
            writer,
            &[
                "CLUSTER <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
                "INFO",
                "    Return information about the cluster.",
                "MYID",
                "    Return the node id.",
                "SLOTS",
                "    Return information about slots range mappings. Each range is made of:",
                "    start, end, master and replicas IP addresses, ports and ids.",
                "SHARDS",
                "    Return information about slot range mappings and the nodes serving them.",
                "NODES",
                "    Return cluster configuration seen by node. Output format:",
                "    <id> <ip:port@bus-port> <flags> <master> <pings> <pongs> <epoch> <link> <slots>",
                "COUNTKEYSINSLOT <slot>",
                "    Return the number of keys in <slot>. This server emulates one node holding",
                "    every slot and does not index keys by slot, so the answer is always 0.",
                "GETKEYSINSLOT <slot> <count>",
                "    Return key names stored by current node in a slot. Always empty here.",
                "KEYSLOT <key>",
                "    Return the hash slot for <key>.",
                "HELP",
                "    Print this help.",
            ],
        )
    } else {
        write_err(
            writer,
            &format!(
                "unknown subcommand '{}'. Try CLUSTER HELP.",
                String::from_utf8_lossy(subcommand)
            ),
        )
    }
}

fn cluster_slot_argument(arg: Option<&Bytes>) -> Option<u16> {
    let slot = std::str::from_utf8(arg?.as_ref()).ok()?.parse::<i64>().ok()?;
    if (0..i64::from(CLUSTER_SLOT_COUNT)).contains(&slot) {
        Some(slot as u16)
    } else {
        None
    }
}

fn handle_slowlog_command<W: Write>(cmd: &Command, writer: &mut W) -> std::io::Result<()> {
    let subcommand = cmd.args.first().map(|arg| arg.as_ref()).unwrap_or(b"");
    if ascii_eq_ci(subcommand, b"GET") {
        // No slowlog is recorded; monitoring tools expect an empty list.
        write_array_header(writer, 0)
    } else if ascii_eq_ci(subcommand, b"LEN") {
        write_integer(writer, 0)
    } else if ascii_eq_ci(subcommand, b"RESET") {
        write_simple_ok(writer)
    } else if ascii_eq_ci(subcommand, b"HELP") {
        write_string_array(
            writer,
            &[
                "SLOWLOG <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
                "GET [<count>]",
                "    Return top <count> entries from the slowlog. This server records no slowlog, so the result is always empty.",
                "LEN",
                "    Return the length of the slowlog.",
                "RESET",
                "    Reset the slowlog.",
                "HELP",
                "    Print this help.",
            ],
        )
    } else {
        write_err(
            writer,
            "unknown subcommand or wrong number of arguments for 'SLOWLOG'. Try SLOWLOG HELP.",
        )
    }
}

/// Redis's own `MODULE HELP` lines, with the header and footer
/// `addReplyHelp` puts around every container command's help.
const MODULE_HELP: &[&str] = &[
    "MODULE <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
    "LIST",
    "    Return a list of loaded modules.",
    "LOAD <path> [<arg> [<arg> ...]]",
    "    Load a module library from <path>, passing to it any optional arguments.",
    "LOADEX <path> [[CONFIG NAME VALUE] [CONFIG NAME VALUE]] [ARGS ...]",
    "    Load a module library from <path>, while passing to it module configurations and optional arguments.",
    "UNLOAD <name>",
    "    Unload a module.",
    "HELP",
    "    Print this help.",
];

/// MODULE, answered locally. There is no Redis module ABI below this adapter,
/// so nothing is emulated: LIST is an empty array because no module is loaded,
/// which is the truth, and the two loading forms and UNLOAD answer with the
/// errors Redis itself gives when the load fails or the name is unknown. That
/// is what a client library probing for a module needs in order to conclude it
/// is not there and move on. Recorded in known_divergences.txt.
fn handle_module_command<W: Write>(cmd: &Command, writer: &mut W) -> std::io::Result<()> {
    let subcommand = cmd.args.first().map(|arg| arg.as_ref()).unwrap_or(b"");
    let argc = cmd.args.len();
    if ascii_eq_ci(subcommand, b"LIST") && argc == 1 {
        write_array_header(writer, 0)
    } else if (ascii_eq_ci(subcommand, b"LOAD") || ascii_eq_ci(subcommand, b"LOADEX")) && argc >= 2
    {
        write_err(
            writer,
            "Error loading the extension. Please check the server logs.",
        )
    } else if ascii_eq_ci(subcommand, b"UNLOAD") && argc == 2 {
        write_err(
            writer,
            "Error unloading module: no such module with that name",
        )
    } else if ascii_eq_ci(subcommand, b"HELP") && argc == 1 {
        write_string_array(writer, MODULE_HELP)
    } else {
        write_err(
            writer,
            &format!(
                "unknown subcommand '{}'. Try MODULE HELP.",
                String::from_utf8_lossy(subcommand)
            ),
        )
    }
}

fn handle_latency_command<W: Write>(cmd: &Command, writer: &mut W) -> std::io::Result<()> {
    let subcommand = cmd.args.first().map(|arg| arg.as_ref()).unwrap_or(b"");
    if ascii_eq_ci(subcommand, b"LATEST") {
        write_array_header(writer, 0)
    } else if ascii_eq_ci(subcommand, b"HISTORY") {
        if cmd.args.len() != 2 {
            return write_err(writer, "wrong number of arguments for 'latency|history' command");
        }
        write_array_header(writer, 0)
    } else if ascii_eq_ci(subcommand, b"RESET") {
        write_integer(writer, 0)
    } else if ascii_eq_ci(subcommand, b"DOCTOR") {
        write_bulk(
            writer,
            b"Dave, no latency spike was observed during the lifetime of this Redis instance, not in the slightest bit. I honestly think you ought to sit down calmly, take a stress pill, and think things over.\n",
        )
    } else if ascii_eq_ci(subcommand, b"HISTOGRAM") {
        write_map_header(writer, 0)
    } else if ascii_eq_ci(subcommand, b"HELP") {
        write_string_array(
            writer,
            &[
                "LATENCY <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
                "DOCTOR",
                "    Return a human readable latency analysis report.",
                "HISTORY <event>",
                "    Return time-latency samples for the <event> class.",
                "LATEST",
                "    Return the latest latency samples for all events.",
                "RESET [<event> ...]",
                "    Reset latency data of one or more <event> classes.",
                "HISTOGRAM [<command> ...]",
                "    Return a cumulative distribution of latencies per command.",
                "HELP",
                "    Print this help.",
            ],
        )
    } else {
        write_err(
            writer,
            "unknown subcommand or wrong number of arguments for 'LATENCY'. Try LATENCY HELP.",
        )
    }
}

fn handle_acl_command<W: Write>(cmd: &Command, writer: &mut W) -> std::io::Result<()> {
    let subcommand = cmd.args.first().map(|arg| arg.as_ref()).unwrap_or(b"");
    if ascii_eq_ci(subcommand, b"WHOAMI") {
        write_bulk(writer, b"default")
    } else if ascii_eq_ci(subcommand, b"USERS") {
        write_string_array(writer, &["default"])
    } else if ascii_eq_ci(subcommand, b"LIST") {
        write_string_array(writer, &["user default on nopass sanitize-payload ~* &* +@all"])
    } else if ascii_eq_ci(subcommand, b"CAT") {
        if cmd.args.len() == 1 {
            write_string_array(
                writer,
                &[
                    "keyspace", "read", "write", "set", "sortedset", "list", "hash", "string",
                    "bitmap", "hyperloglog", "geo", "stream", "pubsub", "admin", "fast", "slow",
                    "blocking", "dangerous", "connection", "transaction", "scripting",
                ],
            )
        } else {
            write_array_header(writer, 0)
        }
    } else if ascii_eq_ci(subcommand, b"GETUSER") {
        if cmd.args.len() == 2 && cmd.args[1].as_ref() == b"default" {
            write_array_header(writer, 12)?;
            write_bulk(writer, b"flags")?;
            write_string_array(writer, &["on", "nopass", "sanitize-payload"])?;
            write_bulk(writer, b"passwords")?;
            write_array_header(writer, 0)?;
            write_bulk(writer, b"commands")?;
            write_bulk(writer, b"+@all")?;
            write_bulk(writer, b"keys")?;
            write_bulk(writer, b"~*")?;
            write_bulk(writer, b"channels")?;
            write_bulk(writer, b"&*")?;
            write_bulk(writer, b"selectors")?;
            write_array_header(writer, 0)
        } else {
            write_nil_bulk(writer)
        }
    } else if ascii_eq_ci(subcommand, b"LOG") {
        if cmd.args.len() == 2 && ascii_eq_ci(cmd.args[1].as_ref(), b"RESET") {
            write_simple_ok(writer)
        } else {
            write_array_header(writer, 0)
        }
    } else if ascii_eq_ci(subcommand, b"GENPASS") {
        let mut seed = unix_time_ms() as u64 ^ 0x9E37_79B9_7F4A_7C15;
        let mut out = String::with_capacity(64);
        for _ in 0..4 {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            out.push_str(&format!("{seed:016x}"));
        }
        write_bulk(writer, out.as_bytes())
    } else if ascii_eq_ci(subcommand, b"HELP") {
        write_string_array(
            writer,
            &[
                "ACL <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
                "CAT [<category>]",
                "GETUSER <username>",
                "GENPASS [<bits>]",
                "LIST",
                "LOG [<count> | RESET]",
                "USERS",
                "WHOAMI",
                "HELP",
                "This server runs with a single implicit 'default' user; ACL rules cannot be changed at runtime.",
            ],
        )
    } else if ascii_eq_ci(subcommand, b"SETUSER")
        || ascii_eq_ci(subcommand, b"DELUSER")
        || ascii_eq_ci(subcommand, b"LOAD")
        || ascii_eq_ci(subcommand, b"SAVE")
        || ascii_eq_ci(subcommand, b"DRYRUN")
    {
        write_err(
            writer,
            "ACL configuration is not supported by this server; only the implicit 'default' user exists",
        )
    } else {
        write_err(
            writer,
            "unknown subcommand or wrong number of arguments for 'ACL'. Try ACL HELP.",
        )
    }
}

fn handle_object_local<W: Write>(_cmd: &Command, writer: &mut W) -> std::io::Result<()> {
    write_string_array(
        writer,
        &[
            "OBJECT <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
            "ENCODING <key>",
            "    Return the kind of internal representation used in order to store the value",
            "    associated with a <key>.",
            "FREQ <key>",
            "    Not supported: this server does not track access frequency.",
            "IDLETIME <key>",
            "    Not supported: this server does not track key access time.",
            "REFCOUNT <key>",
            "    Return the number of references of the value associated with the specified <key>.",
            "HELP",
            "    Print this help.",
        ],
    )
}

// ===== INFO keyspace =====

static KEYSPACE_COUNT_CACHE: [std::sync::atomic::AtomicI64; REDIS_DATABASE_COUNT as usize] =
    [const { std::sync::atomic::AtomicI64::new(-1) }; REDIS_DATABASE_COUNT as usize];
static KEYSPACE_COUNT_AT_MS: [std::sync::atomic::AtomicI64; REDIS_DATABASE_COUNT as usize] =
    [const { std::sync::atomic::AtomicI64::new(0) }; REDIS_DATABASE_COUNT as usize];
static INFO_ALL_DBS: OnceLock<bool> = OnceLock::new();

/// Whether INFO keyspace should scan databases 1..15 as well as database 0.
/// Off by default: each database costs its own keyspace scan.
fn info_all_dbs() -> bool {
    *INFO_ALL_DBS.get_or_init(|| env::var("MAKO_REDIS_INFO_ALL_DBS").as_deref() == Ok("1"))
}

/// Count the visible keys of one logical database the way DBSIZE does, cached
/// for two seconds per database so frequent INFO scrapes do not each walk the
/// keyspace.
fn keyspace_key_count(db: u8) -> Option<i64> {
    let slot = db as usize;
    if slot >= KEYSPACE_COUNT_CACHE.len() {
        return None;
    }
    let now = unix_time_ms();
    let cached = KEYSPACE_COUNT_CACHE[slot].load(Ordering::Relaxed);
    let cached_at = KEYSPACE_COUNT_AT_MS[slot].load(Ordering::Relaxed);
    if cached >= 0 && now.saturating_sub(cached_at) < 2000 {
        return Some(cached);
    }
    // Kept alive for the whole call: the executor reads these bytes.
    let prefix = db_scan_prefix(db);
    let ops = [TxnOperation {
        op: TXN_OP_SCAN,
        key_ptr: std::ptr::null(),
        key_len: 0,
        val_ptr: prefix.as_ptr(),
        val_len: prefix.len(),
        flags: TXN_FLAG_SCAN_COUNT_ONLY,
        expire_at_ms: -1,
        group_id: 0,
    }];
    let count = if redis_backend() == RedisBackend::Memory {
        let response = memory_execute_transaction(&ops);
        let resp = response.as_response();
        if resp.num_results >= 1 && !resp.results.is_null() {
            let result = unsafe { &*resp.results };
            if result.success {
                Some(result.int_value)
            } else {
                None
            }
        } else {
            None
        }
    } else {
        let request = TxnRequest {
            num_ops: 1,
            ops: ops.as_ptr(),
        };
        let mut response = TxnResponse {
            transaction_success: false,
            num_results: 0,
            results: std::ptr::null_mut(),
        };
        let ok = unsafe { cpp_execute_transaction(&request, &mut response) };
        let count = if ok
            && response.transaction_success
            && response.num_results >= 1
            && !response.results.is_null()
        {
            let result = unsafe { &*response.results };
            if result.success {
                Some(result.int_value)
            } else {
                None
            }
        } else {
            None
        };
        unsafe { cpp_free_transaction_response(&mut response) };
        count
    };
    if let Some(count) = count {
        KEYSPACE_COUNT_CACHE[slot].store(count, Ordering::Relaxed);
        KEYSPACE_COUNT_AT_MS[slot].store(now, Ordering::Relaxed);
    }
    count
}

fn append_keyspace_info(out: &mut String) {
    out.push_str("# Keyspace\r\n");
    // Database 0 is always reported, from the cached count it has always used.
    // Counting the other fifteen means fifteen more keyspace scans per INFO, so
    // they are reported only when MAKO_REDIS_INFO_ALL_DBS=1 asks for them, and
    // then only when they hold a key, which is what Redis does for every
    // database. Recorded in known_divergences.txt.
    let last = if info_all_dbs() {
        REDIS_DATABASE_COUNT as u8 - 1
    } else {
        0
    };
    for db in 0..=last {
        let Some(count) = keyspace_key_count(db) else {
            continue;
        };
        if db != 0 && count == 0 {
            continue;
        }
        out.push_str("db");
        out.push_str(&db.to_string());
        out.push_str(":keys=");
        out.push_str(&count.to_string());
        out.push_str(",expires=0,avg_ttl=0\r\n");
    }
    out.push_str("\r\n");
}

fn handle_memory_command<W: Write>(cmd: &Command, writer: &mut W) -> std::io::Result<()> {
    let Some(subcommand) = cmd.args.first() else {
        write_err(writer, "wrong number of arguments for 'memory' command")?;
        return Ok(());
    };
    if ascii_eq_ci(subcommand, b"USAGE") {
        if cmd.args.len() < 2 {
            write_err(
                writer,
                "wrong number of arguments for 'memory usage' command",
            )?;
            return Ok(());
        }
        write_integer(writer, 1)
    } else {
        write_err(writer, "unsupported MEMORY subcommand")
    }
}

fn config_value(name: &[u8]) -> Option<(&'static [u8], &'static [u8])> {
    if ascii_eq_ci(name, b"save") {
        Some((b"save", b""))
    } else if ascii_eq_ci(name, b"appendonly") {
        Some((b"appendonly", b"no"))
    } else if ascii_eq_ci(name, b"databases") {
        Some((b"databases", b"16"))
    } else if ascii_eq_ci(name, b"maxmemory-policy") {
        Some((b"maxmemory-policy", b"noeviction"))
    } else if ascii_eq_ci(name, b"timeout") {
        Some((b"timeout", b"0"))
    } else if ascii_eq_ci(name, b"maxclients") {
        Some((b"maxclients", b"10000"))
    } else if ascii_eq_ci(name, b"tcp-keepalive") {
        Some((b"tcp-keepalive", b"300"))
    } else if ascii_eq_ci(name, b"hz") {
        Some((b"hz", b"10"))
    } else if ascii_eq_ci(name, b"notify-keyspace-events") {
        Some((b"notify-keyspace-events", b""))
    } else if ascii_eq_ci(name, b"protected-mode") {
        Some((b"protected-mode", b"no"))
    } else if ascii_eq_ci(name, b"port") {
        let value = env::var("MAKO_PORT").unwrap_or_else(|_| "6380".to_string());
        let leaked: &'static [u8] = Box::leak(value.into_bytes().into_boxed_slice());
        Some((b"port", leaked))
    } else if ascii_eq_ci(name, b"maxmemory") {
        let value = MAXMEMORY_SETTING.load(Ordering::Relaxed).to_string();
        let leaked: &'static [u8] = Box::leak(value.into_bytes().into_boxed_slice());
        Some((b"maxmemory", leaked))
    } else if ascii_eq_ci(name, b"lua-time-limit") || ascii_eq_ci(name, b"busy-reply-threshold") {
        // Redis 7 renamed lua-time-limit to busy-reply-threshold and kept both
        // spellings of the same setting.
        let value = script::lua_time_limit_ms().to_string();
        let leaked: &'static [u8] = Box::leak(value.into_bytes().into_boxed_slice());
        Some((
            if ascii_eq_ci(name, b"lua-time-limit") {
                b"lua-time-limit"
            } else {
                b"busy-reply-threshold"
            },
            leaked,
        ))
    } else {
        None
    }
}

fn config_pattern_matches(pattern: &[u8], name: &[u8]) -> bool {
    pattern == b"*" || ascii_eq_ci(pattern, name)
}

fn handle_config_command<W: Write>(cmd: &Command, writer: &mut W) -> std::io::Result<()> {
    let Some(subcommand) = cmd.args.first() else {
        write_err(writer, "wrong number of arguments for 'config' command")?;
        return Ok(());
    };

    if ascii_eq_ci(subcommand, b"GET") {
        if cmd.args.len() != 2 {
            write_err(writer, "wrong number of arguments for 'config|get' command")?;
            return Ok(());
        }
        let known = [
            b"save".as_slice(),
            b"appendonly",
            b"databases",
            b"maxmemory",
            b"maxmemory-policy",
            b"timeout",
            b"maxclients",
            b"tcp-keepalive",
            b"hz",
            b"notify-keyspace-events",
            b"protected-mode",
            b"port",
            b"lua-time-limit",
            b"busy-reply-threshold",
        ];
        let mut entries = Vec::new();
        for name in known {
            if config_pattern_matches(cmd.args[1].as_ref(), name) {
                if let Some(pair) = config_value(name) {
                    entries.push(pair);
                }
            }
        }
        write_array_header(writer, entries.len() * 2)?;
        for (name, value) in entries {
            write_bulk(writer, name)?;
            write_bulk(writer, value)?;
        }
        Ok(())
    } else if ascii_eq_ci(subcommand, b"SET") {
        if cmd.args.len() != 3 {
            write_err(writer, "wrong number of arguments for 'config|set' command")?;
            return Ok(());
        }
        if ascii_eq_ci(cmd.args[1].as_ref(), b"lua-time-limit")
            || ascii_eq_ci(cmd.args[1].as_ref(), b"busy-reply-threshold")
        {
            let value = std::str::from_utf8(cmd.args[2].as_ref())
                .ok()
                .and_then(|text| text.parse::<usize>().ok());
            let Some(value) = value else {
                write_err(writer, "argument couldn't be parsed into an integer")?;
                return Ok(());
            };
            script::set_lua_time_limit_ms(value);
            return write_simple_ok(writer);
        }
        if ascii_eq_ci(cmd.args[1].as_ref(), b"maxmemory") {
            let value = std::str::from_utf8(cmd.args[2].as_ref())
                .ok()
                .and_then(|text| text.parse::<usize>().ok());
            let Some(value) = value else {
                write_err(writer, "value is not an integer or out of range")?;
                return Ok(());
            };
            MAXMEMORY_SETTING.store(value, Ordering::Relaxed);
        }
        write_simple_ok(writer)
    } else if ascii_eq_ci(subcommand, b"RESETSTAT") {
        if cmd.args.len() != 1 {
            write_err(
                writer,
                "wrong number of arguments for 'config|resetstat' command",
            )?;
            return Ok(());
        }
        reset_worker_counter(&WORKER_COMMANDS_PROCESSED, &TOTAL_COMMANDS_PROCESSED);
        reset_worker_counter(&WORKER_BLPOP_CALLS, &CMDSTAT_BLPOP_CALLS);
        write_simple_ok(writer)
    } else {
        write_err(writer, "unsupported CONFIG subcommand")
    }
}

fn read_mako_metrics() -> MakoMetrics {
    let mut metrics = MakoMetrics::default();
    let ok = unsafe { cpp_get_metrics(&mut metrics) };
    if ok {
        metrics
    } else {
        MakoMetrics::default()
    }
}

fn append_server_info(out: &mut String, metrics: &MakoMetrics) {
    out.push_str("# Server\r\n");
    out.push_str("redis_version:7.2.0\r\n");
    out.push_str("mako_version:0.1.0\r\n");
    out.push_str("redis_mode:standalone\r\n");
    out.push_str("role:master\r\n");
    out.push_str("total_connections_received:");
    out.push_str(
        &TOTAL_CONNECTIONS_RECEIVED
            .load(Ordering::Relaxed)
            .to_string(),
    );
    out.push_str("\r\n");
    out.push_str("uptime_in_seconds:");
    out.push_str(&metrics.uptime_seconds.to_string());
    out.push_str("\r\n\r\n");
}

fn append_clients_info(out: &mut String) {
    out.push_str("# Clients\r\n");
    out.push_str("connected_clients:");
    out.push_str(&CONNECTED_CLIENTS.load(Ordering::Relaxed).to_string());
    out.push_str("\r\n");
    out.push_str("blocked_clients:");
    out.push_str(&BLOCKED_CLIENTS.load(Ordering::Relaxed).to_string());
    out.push_str("\r\n");
    let (blocking_keys, blocking_keys_on_nokey) = blocking_key_counts();
    out.push_str("total_blocking_keys:");
    out.push_str(&blocking_keys.to_string());
    out.push_str("\r\n");
    out.push_str("total_blocking_keys_on_nokey:");
    out.push_str(&blocking_keys_on_nokey.to_string());
    out.push_str("\r\n");
    out.push_str("monitor_clients:");
    out.push_str(&monitor_count().to_string());
    out.push_str("\r\n");
    out.push_str("pubsub_channels:");
    out.push_str(&pubsub_channel_count().to_string());
    out.push_str("\r\n");
    out.push_str("pubsub_patterns:");
    out.push_str(&pubsub_numpat().to_string());
    out.push_str("\r\n\r\n");
}

fn append_stats_info(out: &mut String) {
    out.push_str("# Stats\r\n");
    out.push_str("total_commands_processed:");
    out.push_str(
        &load_worker_counter(&WORKER_COMMANDS_PROCESSED, &TOTAL_COMMANDS_PROCESSED).to_string(),
    );
    out.push_str("\r\n");
    out.push_str("rdb_changes_since_last_save:");
    out.push_str(&load_worker_counter(&WORKER_DIRTY_CHANGES, &DIRTY_CHANGES).to_string());
    out.push_str("\r\n");
    out.push_str("pubsub_channels:");
    out.push_str(&pubsub_channel_count().to_string());
    out.push_str("\r\n");
    out.push_str("pubsub_patterns:");
    out.push_str(&pubsub_numpat().to_string());
    out.push_str("\r\n\r\n");
}

fn append_cluster_info(out: &mut String) {
    out.push_str("# Cluster\r\n");
    out.push_str("cluster_enabled:");
    out.push_str(if cluster_mode() == ClusterMode::Emulated {
        "1"
    } else {
        "0"
    });
    out.push_str("\r\n\r\n");
}

fn append_commandstats_info(out: &mut String) {
    out.push_str("# Commandstats\r\n");
    out.push_str("cmdstat_blpop:calls=");
    out.push_str(&load_worker_counter(&WORKER_BLPOP_CALLS, &CMDSTAT_BLPOP_CALLS).to_string());
    out.push_str(",usec=0,usec_per_call=0.00,rejected_calls=0,failed_calls=0\r\n\r\n");
}

fn append_mako_info(out: &mut String, metrics: &MakoMetrics) {
    out.push_str("# Mako\r\n");
    out.push_str("mako_txn_commits:");
    out.push_str(&metrics.txn_commits.to_string());
    out.push_str("\r\n");
    out.push_str("mako_txn_aborts:");
    out.push_str(&metrics.txn_aborts.to_string());
    out.push_str("\r\n");
    out.push_str("mako_cache_enabled:");
    out.push_str(&metrics.cache_enabled.to_string());
    out.push_str("\r\n");
    out.push_str("mako_cache_capacity_bytes:");
    out.push_str(&metrics.cache_capacity_bytes.to_string());
    out.push_str("\r\n");
    out.push_str("mako_cache_entries:");
    out.push_str(&metrics.cache_entries.to_string());
    out.push_str("\r\n");
    out.push_str("mako_cache_bytes:");
    out.push_str(&metrics.cache_bytes.to_string());
    out.push_str("\r\n");
    out.push_str("mako_cache_hits:");
    out.push_str(&metrics.cache_hits.to_string());
    out.push_str("\r\n");
    out.push_str("mako_cache_misses:");
    out.push_str(&metrics.cache_misses.to_string());
    out.push_str("\r\n");
    out.push_str("mako_cache_inserts:");
    out.push_str(&metrics.cache_inserts.to_string());
    out.push_str("\r\n");
    out.push_str("mako_cache_evictions:");
    out.push_str(&metrics.cache_evictions.to_string());
    out.push_str("\r\n");
    out.push_str("mako_cache_invalidations:");
    out.push_str(&metrics.cache_invalidations.to_string());
    out.push_str("\r\n");
    out.push_str("mako_txn_retries:");
    out.push_str(&metrics.txn_retries.to_string());
    out.push_str("\r\n\r\n");
}

fn handle_info<W: Write>(cmd: &Command, writer: &mut W) -> std::io::Result<()> {
    let metrics = read_mako_metrics();
    let section = cmd
        .args
        .first()
        .map(|arg| arg.as_ref())
        .unwrap_or(b"default");
    let mut out = String::new();

    if ascii_eq_ci(section, b"default") || ascii_eq_ci(section, b"all") {
        append_server_info(&mut out, &metrics);
        append_clients_info(&mut out);
        append_stats_info(&mut out);
        append_commandstats_info(&mut out);
        append_mako_info(&mut out, &metrics);
        append_cluster_info(&mut out);
        append_keyspace_info(&mut out);
    } else if ascii_eq_ci(section, b"cluster") {
        append_cluster_info(&mut out);
    } else if ascii_eq_ci(section, b"keyspace") {
        append_keyspace_info(&mut out);
    } else if ascii_eq_ci(section, b"server") {
        append_server_info(&mut out, &metrics);
    } else if ascii_eq_ci(section, b"clients") {
        append_clients_info(&mut out);
    } else if ascii_eq_ci(section, b"stats") {
        append_stats_info(&mut out);
    } else if ascii_eq_ci(section, b"commandstats") {
        append_commandstats_info(&mut out);
    } else if ascii_eq_ci(section, b"mako") {
        append_mako_info(&mut out, &metrics);
    }

    write_bulk(writer, out.as_bytes())
}

/// Handle a single command, respecting transaction state
fn handle_command<W: Write>(
    cmd: &Command,
    txn_state: &mut TransactionState,
    client_state: &mut ClientState,
    writer: &mut W,
) -> std::io::Result<()> {
    if client_state.in_subscriber_mode()
        && !matches!(
            cmd.op,
            OpCode::Subscribe
                | OpCode::Unsubscribe
                | OpCode::PSubscribe
                | OpCode::PUnsubscribe
                | OpCode::SSubscribe
                | OpCode::SUnsubscribe
                | OpCode::Ping
                | OpCode::Multi
                | OpCode::Exec
                | OpCode::Publish
                | OpCode::Eval
                | OpCode::EvalSha
                | OpCode::EvalRo
                | OpCode::EvalShaRo
                | OpCode::Hello
                | OpCode::Client
                | OpCode::Quit
                | OpCode::Reset
        )
    {
        write_err(
            writer,
            "only (P)SUBSCRIBE, (P)UNSUBSCRIBE, PING, QUIT and RESET are allowed in subscriber mode",
        )?;
        return Ok(());
    }

    match cmd.op {
        OpCode::Ping => {
            if txn_state.in_multi {
                txn_state.queue_command(cmd.clone());
                write_queued(writer)?;
            } else if client_state.in_subscriber_mode() {
                if client_state.protocol_version >= 3 {
                    if let Some(arg) = cmd.args.first() {
                        write_bulk(writer, arg)?;
                    } else {
                        write_pong(writer)?;
                    }
                } else {
                    write_array_header(writer, 2)?;
                    write_bulk(writer, b"pong")?;
                    if let Some(arg) = cmd.args.first() {
                        write_bulk(writer, arg)?;
                    } else {
                        write_bulk(writer, b"")?;
                    }
                }
            } else {
                if let Some(arg) = cmd.args.first() {
                    write_bulk(writer, arg)?;
                } else {
                    write_pong(writer)?;
                }
            }
        }
        OpCode::Hello => {
            handle_hello(cmd, client_state, writer)?;
        }
        OpCode::Client => {
            handle_client_command(cmd, client_state, writer)?;
        }
        OpCode::Command => {
            handle_command_command(cmd, writer)?;
        }
        OpCode::Memory => {
            if !cmd.keys.is_empty() {
                // MEMORY USAGE key: approximate from the serialized value size.
                ffi_execute_single(cmd, client_state.protocol_version, writer)?;
            } else {
                handle_memory_command(cmd, writer)?;
            }
        }
        OpCode::Config => {
            if txn_state.in_multi {
                txn_state.queue_command(cmd.clone());
                write_queued(writer)?;
            } else {
                handle_config_command(cmd, writer)?;
            }
        }
        OpCode::Script => {
            script::handle_script_command(cmd, writer)?;
        }
        OpCode::Eval | OpCode::EvalSha | OpCode::EvalRo | OpCode::EvalShaRo => {
            script::handle_eval_command(cmd, client_state, writer)?;
        }
        OpCode::Forbidden => {
            if txn_state.in_multi {
                txn_state.mark_queue_error();
                write_err(writer, "Command not allowed inside a transaction")?;
            } else {
                write_err(writer, "unsupported command")?;
            }
        }
        OpCode::Reset => {
            txn_state.discard();
            // RESET and QUIT both leave monitor mode, as in Redis.
            unregister_all_client_feeds(client_state);
            client_state.reset();
            set_current_db(client_state.db);
            write_simple_string(writer, "RESET")?;
        }
        OpCode::Quit => {
            unregister_all_client_feeds(client_state);
            client_state.close_after_reply = true;
            write_simple_ok(writer)?;
        }
        OpCode::Select => {
            // Redis queues SELECT inside MULTI and applies it at EXEC; this
            // adapter applies it immediately, because the database has to be
            // known while each queued command is parsed (that is where keys
            // acquire their prefix). Recorded in known_divergences.txt.
            match cmd.args.first().map(|arg| parse_db_index(arg.as_ref())) {
                Some(Ok(db)) => {
                    client_state.db = db;
                    set_current_db(db);
                    write_simple_ok(writer)?;
                }
                Some(Err(err)) => write_parse_error(writer, err)?,
                None => write_err(writer, "DB index is out of range")?,
            }
        }
        OpCode::Auth => {
            if cmd.args.len() == 1 || cmd.args.len() == 2 {
                write_simple_ok(writer)?;
            } else {
                write_err(writer, "wrong number of arguments for 'auth' command")?;
            }
        }
        OpCode::Watch => {
            if txn_state.in_multi {
                write_err(writer, "WATCH inside MULTI is not allowed")?;
            } else {
                // WATCH takes its keys from the raw argument list, not from
                // `cmd.keys`, so this is the one key path that does not run
                // through `validate_user_key`. Key versions are bumped under
                // the storage-facing name, so the watch has to use it too, or
                // a write in database 1 would invalidate a watch in database 0
                // on the same name.
                let watched: Vec<Bytes> = cmd
                    .args
                    .iter()
                    .map(|key| db_key(client_state.db, key))
                    .collect();
                txn_state.watch_keys(&watched);
                write_simple_ok(writer)?;
            }
        }
        OpCode::Unwatch => {
            txn_state.unwatch();
            write_simple_ok(writer)?;
        }
        OpCode::Echo => {
            if cmd.args.len() == 1 {
                write_bulk(writer, &cmd.args[0])?;
            } else {
                write_err(writer, "wrong number of arguments for 'echo' command")?;
            }
        }
        OpCode::Info => {
            handle_info(cmd, writer)?;
        }
        OpCode::Wait => {
            if txn_state.in_multi {
                txn_state.queue_command(cmd.clone());
                write_queued(writer)?;
            } else {
                write_integer(writer, 0)?;
            }
        }
        OpCode::Time => {
            if txn_state.in_multi {
                txn_state.queue_command(cmd.clone());
                write_queued(writer)?;
            } else {
                write_time(writer)?;
            }
        }
        OpCode::Multi => {
            if txn_state.in_multi {
                write_err(writer, "MULTI calls can not be nested")?;
            } else {
                txn_state.start_multi();
                write_simple_ok(writer)?;
            }
        }
        OpCode::Exec => {
            if !txn_state.in_multi {
                write_err(writer, "EXEC without MULTI")?;
            } else if MAXMEMORY_SETTING.load(Ordering::Relaxed) > 0
                && txn_state
                    .queued_commands
                    .iter()
                    .any(|command| is_dirty_command(command.op))
            {
                txn_state.discard();
                writer.write_all(
                    b"-EXECABORT Transaction discarded because of previous errors. OOM command not allowed when used memory > 'maxmemory'.\r\n",
                )?;
            } else if txn_state.has_queue_error() {
                txn_state.discard();
                writer.write_all(
                    b"-EXECABORT Transaction discarded because of previous errors. BUSY Redis is busy running a script.\r\n",
                )?;
            } else if txn_state.watched_keys_dirty() {
                txn_state.discard();
                writer.write_all(b"*-1\r\n")?;
            } else {
                let commands = txn_state.take_commands();
                if commands
                    .iter()
                    .any(|command| matches!(command.op, OpCode::Unsubscribe | OpCode::PUnsubscribe))
                {
                    write_array_header(writer, commands.len())?;
                    for command in &commands {
                        match command.op {
                            OpCode::Ping => {
                                if let Some(arg) = command.args.first() {
                                    write_bulk(writer, arg)?;
                                } else {
                                    write_pong(writer)?;
                                }
                            }
                            OpCode::Unsubscribe => {
                                handle_unsubscribe(command, client_state, writer)?;
                            }
                            OpCode::PUnsubscribe => {
                                handle_punsubscribe(command, client_state, writer)?;
                            }
                            OpCode::Publish => {
                                handle_publish(command, writer)?;
                            }
                            _ => {
                                write_err(writer, "operation failed")?;
                            }
                        }
                    }
                } else {
                    // GEOSEARCHSTORE cannot join one op list, so a queue that
                    // holds one runs command by command inside a single open
                    // session. Everything else keeps the batch path.
                    if commands
                        .iter()
                        .any(|command| command.op == OpCode::GeoSearchStore)
                    {
                        ffi_execute_transaction_session(
                            &commands,
                            client_state.protocol_version,
                            writer,
                        )?;
                    } else {
                        ffi_execute_transaction(&commands, client_state.protocol_version, writer)?;
                    }
                    for command in &commands {
                        if is_dirty_command(command.op) {
                            bump_modified_key_versions(command);
                        }
                    }
                }
            }
        }
        OpCode::Discard => {
            if !txn_state.in_multi {
                write_err(writer, "DISCARD without MULTI")?;
            } else {
                txn_state.discard();
                write_simple_ok(writer)?;
            }
        }
        OpCode::Subscribe => {
            handle_subscribe(cmd, client_state, writer)?;
        }
        OpCode::Unsubscribe => {
            if txn_state.in_multi {
                txn_state.queue_command(cmd.clone());
                write_queued(writer)?;
            } else {
                handle_unsubscribe(cmd, client_state, writer)?;
            }
        }
        OpCode::PSubscribe => {
            handle_psubscribe(cmd, client_state, writer)?;
        }
        OpCode::PUnsubscribe => {
            if txn_state.in_multi {
                txn_state.queue_command(cmd.clone());
                write_queued(writer)?;
            } else {
                handle_punsubscribe(cmd, client_state, writer)?;
            }
        }
        OpCode::Publish => {
            if txn_state.in_multi {
                txn_state.queue_command(cmd.clone());
                write_queued(writer)?;
            } else {
                handle_publish(cmd, writer)?;
            }
        }
        OpCode::PubSub => {
            handle_pubsub(cmd, writer)?;
        }
        OpCode::SSubscribe => {
            handle_ssubscribe(cmd, client_state, writer)?;
        }
        OpCode::SUnsubscribe => {
            if txn_state.in_multi {
                txn_state.queue_command(cmd.clone());
                write_queued(writer)?;
            } else {
                handle_sunsubscribe(cmd, client_state, writer)?;
            }
        }
        OpCode::Monitor => {
            if txn_state.in_multi {
                // Redis queues MONITOR and then refuses it at EXEC, because a
                // transaction owes one reply per queued command and a monitor
                // client's stream is not one. `write_command_result` produces
                // that error.
                txn_state.queue_command(cmd.clone());
                write_queued(writer)?;
            } else {
                register_monitor(client_state);
                write_simple_ok(writer)?;
            }
        }
        OpCode::Cluster => {
            // Queued inside MULTI like the other admin commands; the reply is
            // produced from the same handler in `write_command_result`.
            if txn_state.in_multi {
                txn_state.queue_command(cmd.clone());
                write_queued(writer)?;
            } else {
                handle_cluster_command(cmd, cluster_mode(), writer)?;
            }
        }
        OpCode::ReadOnly | OpCode::ReadWrite => {
            if txn_state.in_multi {
                txn_state.queue_command(cmd.clone());
                write_queued(writer)?;
            } else {
                write_cluster_readonly_reply(cluster_mode(), writer)?;
            }
        }
        OpCode::SlowLog => {
            handle_slowlog_command(cmd, writer)?;
        }
        OpCode::Module => {
            // Queued inside MULTI like CONFIG, and answered at EXEC from
            // `write_command_result`, because it touches no storage.
            if txn_state.in_multi {
                txn_state.queue_command(cmd.clone());
                write_queued(writer)?;
            } else {
                handle_module_command(cmd, writer)?;
            }
        }
        OpCode::Latency => {
            handle_latency_command(cmd, writer)?;
        }
        OpCode::Acl => {
            handle_acl_command(cmd, writer)?;
        }
        OpCode::Object => {
            if cmd.keys.is_empty() {
                handle_object_local(cmd, writer)?;
            } else {
                ffi_execute_single(cmd, client_state.protocol_version, writer)?;
            }
        }
        OpCode::Get
        | OpCode::GetEx
        | OpCode::GetDel
        | OpCode::Set
        | OpCode::SetEx
        | OpCode::PSetEx
        | OpCode::Del
        | OpCode::Exists
        | OpCode::MGet
        | OpCode::MSet
        | OpCode::MSetNx
        | OpCode::Rename
        | OpCode::RenameNx
        | OpCode::Sort
        | OpCode::SortRo
        | OpCode::Touch
        | OpCode::BitCount
        | OpCode::BitPos
        | OpCode::BitFieldRo
        | OpCode::BitField
        | OpCode::BitOp
        | OpCode::PfAdd
        | OpCode::PfCount
        | OpCode::PfMerge
        | OpCode::GetSet
        | OpCode::SetNx
        | OpCode::Append
        | OpCode::StrLen
        | OpCode::SetBit
        | OpCode::GetBit
        | OpCode::SetRange
        | OpCode::GetRange
        | OpCode::Lcs
        | OpCode::Dump
        | OpCode::Restore
        | OpCode::Copy
        | OpCode::Move
        | OpCode::Incr
        | OpCode::IncrBy
        | OpCode::Decr
        | OpCode::DecrBy
        | OpCode::IncrByFloat
        | OpCode::Expire
        | OpCode::PExpire
        | OpCode::ExpireAt
        | OpCode::PExpireAt
        | OpCode::Ttl
        | OpCode::PTtl
        | OpCode::ExpireTime
        | OpCode::PExpireTime
        | OpCode::Persist
        | OpCode::Keys
        | OpCode::Scan
        | OpCode::RandomKey
        | OpCode::DbSize
        | OpCode::FlushDb
        | OpCode::FlushAll
        | OpCode::Type
        | OpCode::HSet
        | OpCode::HSetNx
        | OpCode::HMSet
        | OpCode::HGet
        | OpCode::HMGet
        | OpCode::HGetAll
        | OpCode::HDel
        | OpCode::HExists
        | OpCode::HLen
        | OpCode::HKeys
        | OpCode::HVals
        | OpCode::HStrLen
        | OpCode::HIncrBy
        | OpCode::HIncrByFloat
        | OpCode::HRandField
        | OpCode::HScan
        | OpCode::HExpire
        | OpCode::HPExpire
        | OpCode::HExpireAt
        | OpCode::HPExpireAt
        | OpCode::HTtl
        | OpCode::HPTtl
        | OpCode::HExpireTime
        | OpCode::HPExpireTime
        | OpCode::HPersist
        | OpCode::SAdd
        | OpCode::SMembers
        | OpCode::SIsMember
        | OpCode::SMIsMember
        | OpCode::SRem
        | OpCode::SCard
        | OpCode::SScan
        | OpCode::SMove
        | OpCode::SPop
        | OpCode::SRandMember
        | OpCode::SInter
        | OpCode::SInterCard
        | OpCode::SUnion
        | OpCode::SDiff
        | OpCode::SInterStore
        | OpCode::SUnionStore
        | OpCode::SDiffStore
        | OpCode::LPush
        | OpCode::RPush
        | OpCode::LPop
        | OpCode::RPop
        | OpCode::BLPop
        | OpCode::BRPop
        | OpCode::BLMPop
        | OpCode::LMPop
        | OpCode::LLen
        | OpCode::LIndex
        | OpCode::LRange
        | OpCode::LSet
        | OpCode::LRem
        | OpCode::LTrim
        | OpCode::LInsert
        | OpCode::LPushX
        | OpCode::RPushX
        | OpCode::LMove
        | OpCode::BLMove
        | OpCode::RPopLPush
        | OpCode::BRPopLPush
        | OpCode::LPos
        | OpCode::ZAdd
        | OpCode::ZScore
        | OpCode::ZMScore
        | OpCode::ZIncrBy
        | OpCode::ZRem
        | OpCode::ZCard
        | OpCode::ZRange
        | OpCode::ZRevRange
        | OpCode::ZRangeByScore
        | OpCode::ZRevRangeByScore
        | OpCode::ZRangeByLex
        | OpCode::ZRevRangeByLex
        | OpCode::ZLexCount
        | OpCode::ZRemRangeByScore
        | OpCode::ZRemRangeByRank
        | OpCode::ZRemRangeByLex
        | OpCode::ZRangeStore
        | OpCode::ZUnion
        | OpCode::ZInter
        | OpCode::ZDiff
        | OpCode::ZUnionStore
        | OpCode::ZInterStore
        | OpCode::ZDiffStore
        | OpCode::ZInterCard
        | OpCode::ZRank
        | OpCode::ZRevRank
        | OpCode::ZCount
        | OpCode::ZPopMin
        | OpCode::ZPopMax
        | OpCode::ZMPop
        | OpCode::BZMPop
        | OpCode::BZPopMin
        | OpCode::BZPopMax
        | OpCode::ZRandMember
        | OpCode::ZScan
        | OpCode::GeoAdd
        | OpCode::GeoPos
        | OpCode::GeoDist
        | OpCode::GeoHash
        | OpCode::XAdd
        | OpCode::XRange
        | OpCode::XRevRange
        | OpCode::XLen
        | OpCode::XDel
        | OpCode::XTrim
        | OpCode::XSetId
        | OpCode::XInfo
        | OpCode::XRead
        | OpCode::XGroup
        | OpCode::XReadGroup
        | OpCode::XAck
        | OpCode::XPending
        | OpCode::XClaim
        | OpCode::XAutoClaim => {
            if txn_state.in_multi {
                // Queue command for later execution
                txn_state.queue_command(cmd.clone());
                write_queued(writer)?;
            } else {
                // Execute immediately as single-operation transaction
                // Uses ffi_execute_single which returns result without array wrapper
                ffi_execute_single(cmd, client_state.protocol_version, writer)?;
            }
        }
        OpCode::GeoSearch => {
            // FROMMEMBER / GEORADIUSBYMEMBER needs the member's own score
            // before the range reads can be built, and a later op cannot read
            // an earlier op's result inside one request, so that lookup runs as
            // a request of its own. The range reads then still go out together.
            let resolved = geo_resolve_search_center(cmd);
            let target = resolved.as_ref().unwrap_or(cmd);
            if txn_state.in_multi {
                txn_state.queue_command(target.clone());
                write_queued(writer)?;
            } else {
                ffi_execute_single(target, client_state.protocol_version, writer)?;
            }
        }
        OpCode::GeoSearchStore => {
            // Queued like any other write. EXEC notices it and runs the whole
            // queue on the session path, because this command's own write
            // cannot be built until its own reads have answered.
            if txn_state.in_multi {
                txn_state.queue_command(cmd.clone());
                write_queued(writer)?;
            } else {
                geo_search_store_command(cmd, client_state.protocol_version, writer)?;
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn command(op: OpCode, args: &[&[u8]]) -> Command {
        Command::new(
            op,
            Vec::new(),
            None,
            args.iter().map(|arg| Bytes::copy_from_slice(arg)).collect(),
        )
    }

    fn data_command(op: OpCode, keys: &[&[u8]], val: Option<&[u8]>) -> Command {
        Command::new(
            op,
            keys.iter().map(|key| Bytes::copy_from_slice(key)).collect(),
            val.map(Bytes::copy_from_slice),
            keys.iter().map(|key| Bytes::copy_from_slice(key)).collect(),
        )
    }

    fn run(
        cmd: Command,
        txn_state: &mut TransactionState,
        client_state: &mut ClientState,
    ) -> Vec<u8> {
        let mut out = Vec::new();
        handle_command(&cmd, txn_state, client_state, &mut out).unwrap();
        out
    }

    fn run_raw(input: &[u8]) -> Vec<u8> {
        let mut resp3 = Resp3Handler::new(1024);
        let mut txn_state = TransactionState::new();
        let mut client_state = ClientState::new();
        let mut out = Vec::new();

        resp3.read_bytes(input);
        match resp3.next_frame().unwrap() {
            Some(frame) => match parse_resp3(frame) {
                Ok(cmd) => {
                    handle_command(&cmd, &mut txn_state, &mut client_state, &mut out).unwrap();
                }
                Err(err) => {
                    write_parse_error(&mut out, err).unwrap();
                }
            },
            None => write_err(&mut out, "protocol error").unwrap(),
        }

        out
    }

    /// The session wrapper against the test stubs: a command run inside an open
    /// session sees what earlier commands in the same session wrote, the reply
    /// bytes are the ones the single-command path would have produced, and a
    /// commit is what makes the writes visible to the next session.
    #[test]
    fn session_txn_reads_its_own_writes_and_publishes_on_commit() {
        let key = Bytes::from_static(b"session-rmw-key");
        let set = Command::new(
            OpCode::Set,
            vec![key.clone()],
            Some(Bytes::from_static(b"first")),
            Vec::new(),
        );
        let get = Command::new(OpCode::Get, vec![key.clone()], None, Vec::new());

        // Nothing committed yet, so a fresh session misses.
        let miss = ffi_run_session(&[key.clone()], |session| {
            let reply = session.execute(&get).unwrap();
            SessionRun::Abort(reply)
        });
        assert_eq!(miss.as_deref(), Some(b"$-1\r\n".as_slice()));

        // Read-your-writes inside one session.
        let seen = ffi_run_session(&[key.clone()], |session| {
            session.execute(&set).unwrap();
            assert!(session.has_written());
            let reply = session.execute(&get).unwrap();
            SessionRun::Commit(reply)
        });
        assert_eq!(seen.as_deref(), Some(b"$5\r\nfirst\r\n".as_slice()));

        // And the commit published them.
        let after_commit = ffi_run_session(&[key.clone()], |session| {
            let reply = session.execute(&get).unwrap();
            SessionRun::Abort(reply)
        });
        assert_eq!(after_commit.as_deref(), Some(b"$5\r\nfirst\r\n".as_slice()));

        // An aborted session leaves nothing behind.
        let rolled_back = Command::new(
            OpCode::Set,
            vec![key.clone()],
            Some(Bytes::from_static(b"second")),
            Vec::new(),
        );
        ffi_run_session(&[key.clone()], |session| {
            session.execute(&rolled_back).unwrap();
            SessionRun::Abort(())
        })
        .unwrap();
        let unchanged = ffi_run_session(&[key.clone()], |session| {
            let reply = session.execute(&get).unwrap();
            SessionRun::Abort(reply)
        });
        assert_eq!(unchanged.as_deref(), Some(b"$5\r\nfirst\r\n".as_slice()));
    }

    /// A body that keeps asking to retry uses every attempt and then gives up,
    /// which is what makes the caller answer `ERR backend` instead of looping.
    #[test]
    fn session_retries_are_bounded() {
        let mut attempts = 0usize;
        let outcome: Option<()> = ffi_run_session(&[], |_session| {
            attempts += 1;
            SessionRun::Retry
        });
        assert!(outcome.is_none());
        assert_eq!(attempts, TXN_MAX_ATTEMPTS);
    }

    #[test]
    fn raw_mako_fast_parser_accepts_plain_get_and_set() {
        match parse_raw_mako_string_command(b"*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n") {
            RawMakoParse::Complete {
                command: RawMakoCommand::Get { key },
                consumed,
            } => {
                assert_eq!(key, b"key");
                assert_eq!(consumed, 22);
            }
            _ => panic!("expected raw GET fast path"),
        }

        match parse_raw_mako_string_command(b"*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n") {
            RawMakoParse::Complete {
                command: RawMakoCommand::Set { key, value },
                consumed,
            } => {
                assert_eq!(key, b"key");
                assert_eq!(value, b"value");
                assert_eq!(consumed, 33);
            }
            _ => panic!("expected raw SET fast path"),
        }
    }

    #[test]
    fn fast_mako_get_and_set_retry_transient_aborts() {
        assert_eq!(fast_mako_max_attempts(OpCode::Get), TXN_MAX_ATTEMPTS);
        assert_eq!(fast_mako_max_attempts(OpCode::Set), TXN_MAX_ATTEMPTS);
        assert_eq!(fast_mako_max_attempts(OpCode::Ping), 1);
    }

    #[test]
    fn core_storage_commands_retry_transient_aborts() {
        for op in [
            OpCode::Get,
            OpCode::Set,
            OpCode::Del,
            OpCode::Exists,
            OpCode::MGet,
            OpCode::StrLen,
            OpCode::GetBit,
            OpCode::GetRange,
            OpCode::Ttl,
            OpCode::PTtl,
        ] {
            assert!(command_needs_retry(&Command::new(
                op,
                vec![Bytes::from_static(b"k")],
                None,
                Vec::new(),
            )));
        }
        assert!(!command_needs_retry(&Command::new(
            OpCode::Ping,
            Vec::new(),
            None,
            Vec::new(),
        )));
    }

    #[test]
    fn raw_mako_fast_parser_leaves_extended_set_to_general_parser() {
        match parse_raw_mako_string_command(
            b"*5\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n$2\r\nNX\r\n",
        ) {
            RawMakoParse::NotFast => {}
            _ => panic!("expected SET with options to use the general parser"),
        }
    }

    fn txn_set<'a>(key: &'a [u8], value: &'a [u8], flags: u32) -> TxnOperation {
        TxnOperation {
            op: TXN_OP_SET,
            key_ptr: key.as_ptr(),
            key_len: key.len(),
            val_ptr: value.as_ptr(),
            val_len: value.len(),
            flags,
            expire_at_ms: -1,
            group_id: 0,
        }
    }

    fn txn_get(key: &[u8]) -> TxnOperation {
        TxnOperation {
            op: TXN_OP_GET,
            key_ptr: key.as_ptr(),
            key_len: key.len(),
            val_ptr: std::ptr::null(),
            val_len: 0,
            flags: 0,
            expire_at_ms: -1,
            group_id: 0,
        }
    }

    fn response_bytes(response: &TxnResponse, index: usize) -> Option<Vec<u8>> {
        let result = unsafe { &*response.results.add(index) };
        if !result.value_present {
            return None;
        }
        if result.data_len == 0 {
            return Some(Vec::new());
        }
        Some(unsafe { std::slice::from_raw_parts(result.data_ptr, result.data_len) }.to_vec())
    }

    #[test]
    fn memory_backend_get_set_round_trip() {
        memory_store().lock().unwrap().clear();
        let set = txn_set(b"mem-key", b"value", 0);
        let get = txn_get(b"mem-key");
        let response = memory_execute_transaction(&[set, get]);

        assert!(response.as_response().transaction_success);
        assert_eq!(response.as_response().num_results, 2);
        assert_eq!(response_bytes(response.as_response(), 1).unwrap(), b"value");
    }

    #[test]
    fn memory_backend_set_nx_and_del_use_redis_presence_semantics() {
        memory_store().lock().unwrap().clear();
        let set = txn_set(b"nx-key", b"first", 0);
        let set_nx = txn_set(b"nx-key", b"second", TXN_FLAG_SET_NX);
        let del = TxnOperation {
            op: TXN_OP_DEL,
            key_ptr: b"nx-key".as_ptr(),
            key_len: b"nx-key".len(),
            val_ptr: std::ptr::null(),
            val_len: 0,
            flags: 0,
            expire_at_ms: -1,
            group_id: 0,
        };
        let get = txn_get(b"nx-key");
        let response = memory_execute_transaction(&[set, set_nx, del, get]);

        assert!(response.as_response().transaction_success);
        assert!(response_bytes(response.as_response(), 0).is_some());
        assert!(response_bytes(response.as_response(), 1).is_none());
        assert!(response_bytes(response.as_response(), 2).is_some());
        assert!(response_bytes(response.as_response(), 3).is_none());
    }

    #[test]
    fn hello_3_returns_resp3_capability_map() {
        let mut txn_state = TransactionState::new();
        let mut client_state = ClientState::new();

        let out = run(
            command(OpCode::Hello, &[b"3"]),
            &mut txn_state,
            &mut client_state,
        );
        let text = String::from_utf8(out).unwrap();

        assert!(text.starts_with("%"));
        assert!(text.contains("+server\r\n+makoCon\r\n"));
        assert!(text.contains("+proto\r\n:3\r\n"));
        assert!(text.contains("+id\r\n:"));
        assert_eq!(client_state.protocol_version, 3);
    }

    #[test]
    fn client_setname_and_getname_round_trip() {
        let mut txn_state = TransactionState::new();
        let mut client_state = ClientState::new();

        let set = run(
            command(OpCode::Client, &[b"SETNAME", b"phase2"]),
            &mut txn_state,
            &mut client_state,
        );
        let get = run(
            command(OpCode::Client, &[b"GETNAME"]),
            &mut txn_state,
            &mut client_state,
        );

        assert_eq!(set, b"+OK\r\n");
        assert_eq!(get, b"$6\r\nphase2\r\n");
    }

    #[test]
    fn documented_client_subcommands_return_parseable_replies() {
        let mut txn_state = TransactionState::new();
        let mut client_state = ClientState::new();
        client_state.name = Some(Bytes::from_static(b"phase2"));

        let no_evict = run(
            command(OpCode::Client, &[b"NO-EVICT", b"ON"]),
            &mut txn_state,
            &mut client_state,
        );
        let reply = run(
            command(OpCode::Client, &[b"REPLY", b"ON"]),
            &mut txn_state,
            &mut client_state,
        );
        let list = run(
            command(OpCode::Client, &[b"LIST"]),
            &mut txn_state,
            &mut client_state,
        );

        assert_eq!(no_evict, b"+OK\r\n");
        assert_eq!(reply, b"+OK\r\n");
        assert!(String::from_utf8(list).unwrap().contains("name=phase2"));
    }

    #[test]
    fn client_id_is_stable_for_connection() {
        let mut txn_state = TransactionState::new();
        let mut client_state = ClientState::new();
        let expected = format!(":{}\r\n", client_state.id).into_bytes();

        let first = run(
            command(OpCode::Client, &[b"ID"]),
            &mut txn_state,
            &mut client_state,
        );
        let reset = run(
            command(OpCode::Reset, &[]),
            &mut txn_state,
            &mut client_state,
        );
        let second = run(
            command(OpCode::Client, &[b"ID"]),
            &mut txn_state,
            &mut client_state,
        );

        assert_eq!(first, expected);
        assert_eq!(reset, b"+RESET\r\n");
        assert_eq!(second, first);
    }

    #[test]
    fn reset_clears_connection_state_and_transaction_queue() {
        let mut txn_state = TransactionState::new();
        let mut client_state = ClientState::new();
        txn_state.start_multi();
        client_state.name = Some(Bytes::from_static(b"phase2"));
        client_state.protocol_version = 3;

        let out = run(
            command(OpCode::Reset, &[]),
            &mut txn_state,
            &mut client_state,
        );

        assert_eq!(out, b"+RESET\r\n");
        assert!(!txn_state.in_multi);
        assert!(client_state.name.is_none());
        assert_eq!(client_state.protocol_version, 2);
    }

    fn cluster(mode: ClusterMode, args: &[&[u8]]) -> Vec<u8> {
        let mut out = Vec::new();
        handle_cluster_command(&command(OpCode::Cluster, args), mode, &mut out).unwrap();
        out
    }

    #[test]
    fn monitor_quotes_arguments_like_sdscatrepr() {
        let quote = |arg: &[u8]| {
            let mut out = String::new();
            monitor_quote_arg(&mut out, arg);
            out
        };
        assert_eq!(quote(b"plain"), "\"plain\"");
        assert_eq!(quote(b""), "\"\"");
        assert_eq!(quote(b"say \"hi\""), "\"say \\\"hi\\\"\"");
        assert_eq!(quote(b"back\\slash"), "\"back\\\\slash\"");
        assert_eq!(
            quote(b"\n\r\t\x07\x08"),
            "\"\\n\\r\\t\\a\\b\""
        );
        // Everything else outside printable ASCII is \xHH, lowercase, and the
        // bytes above 0x7e are not printable in Redis's C locale either.
        assert_eq!(quote(b"\x00\x01\x1f\x7f\x80\xff"), "\"\\x00\\x01\\x1f\\x7f\\x80\\xff\"");
        assert_eq!(quote(" !~".as_bytes()), "\" !~\"");
    }

    #[test]
    fn monitor_redacts_credentials_like_redis() {
        let argv = |args: &[&[u8]]| -> Vec<Bytes> {
            args.iter().map(|arg| Bytes::copy_from_slice(arg)).collect()
        };
        assert_eq!(
            monitor_redact_argv(&argv(&[b"AUTH", b"hunter2"])).unwrap(),
            argv(&[b"AUTH", b"(redacted)"])
        );
        assert_eq!(
            monitor_redact_argv(&argv(&[b"auth", b"user", b"hunter2"])).unwrap(),
            argv(&[b"auth", b"(redacted)", b"(redacted)"])
        );
        assert_eq!(
            monitor_redact_argv(&argv(&[b"HELLO", b"3", b"AUTH", b"user", b"pw"])).unwrap(),
            argv(&[b"HELLO", b"3", b"AUTH", b"(redacted)", b"(redacted)"])
        );
        assert!(monitor_redact_argv(&argv(&[b"HELLO", b"3"])).is_none());
        assert!(monitor_redact_argv(&argv(&[b"GET", b"AUTH"])).is_none());
    }

    #[test]
    fn monitor_line_has_the_redis_shape() {
        let argv = vec![
            Bytes::from_static(b"SET"),
            Bytes::from_static(b"k"),
            Bytes::from_static(b"a\nb"),
        ];
        let line = format_monitor_line(0, "127.0.0.1:54321", &argv);
        let line = String::from_utf8(line).unwrap();
        assert!(line.starts_with('+'), "{line}");
        assert!(line.ends_with("\r\n"), "{line}");
        let body = line.trim_start_matches('+').trim_end_matches("\r\n");
        let (stamp, rest) = body.split_once(' ').unwrap();
        let (seconds, micros) = stamp.split_once('.').unwrap();
        assert!(seconds.bytes().all(|byte| byte.is_ascii_digit()), "{stamp}");
        assert_eq!(micros.len(), 6, "{stamp}");
        assert!(micros.bytes().all(|byte| byte.is_ascii_digit()), "{stamp}");
        assert_eq!(
            rest,
            "[0 127.0.0.1:54321] \"SET\" \"k\" \"a\\nb\"",
            "{line}"
        );
    }

    #[test]
    fn monitor_argv_comes_from_the_frame_before_parsing() {
        let mut resp3 = Resp3Handler::new(1024);
        resp3.read_bytes(b"*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$2\r\nhi\r\n");
        let frame = resp3.next_frame().unwrap().unwrap();
        let argv = monitor_argv_from_frame(&frame).unwrap();
        assert_eq!(argv, vec![Bytes::from_static(b"SET"), Bytes::from_static(b"k"), Bytes::from_static(b"hi")]);
    }

    #[test]
    fn monitor_inside_multi_is_refused_at_exec() {
        let mut txn_state = TransactionState::new();
        let mut client_state = ClientState::new();
        txn_state.start_multi();
        assert_eq!(
            run(command(OpCode::Monitor, &[]), &mut txn_state, &mut client_state),
            b"+QUEUED\r\n"
        );
        assert!(!client_state.monitoring);

        let mut out = Vec::new();
        write_command_result(&command(OpCode::Monitor, &[]), None, (0, 0), 2, &mut out).unwrap();
        assert_eq!(
            out,
            b"-ERR MONITOR isn't allowed for DENY BLOCKING client\r\n"
        );

        assert_eq!(
            run_raw(b"*2\r\n$7\r\nMONITOR\r\n$1\r\nx\r\n"),
            b"-ERR wrong number of arguments for 'monitor' command\r\n"
        );
    }

    /// Everything that touches the process-global monitor registry lives in one
    /// test: MONITOR_COUNT gates the raw fast path for every connection, so two
    /// tests racing on it would see each other's registrations.
    #[test]
    fn monitor_registration_gates_the_fast_path_and_skips_the_monitor_itself() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let address = listener.local_addr().unwrap();
        let client_side = TcpStream::connect(address).unwrap();
        let client_address = client_side.local_addr().unwrap();
        let (accepted, _) = listener.accept().unwrap();
        drop(client_side);
        let wake = Arc::new(WorkerWake::new().unwrap());
        let mut conn = ClientConn::new(accepted, &wake);

        assert_eq!(monitor_count(), 0);
        assert!(
            can_use_raw_mako_fast_path(&conn),
            "the fast path must be open with no monitor attached"
        );
        // The peer address recorded at accept time is the client's, not the
        // listening socket's.
        assert_eq!(conn.client_state.peer_addr, client_address.to_string());

        let mut monitor_state = ClientState::for_worker(&wake);
        monitor_state.peer_addr = String::from("127.0.0.1:1");
        assert!(register_monitor(&mut monitor_state));
        assert!(monitor_state.monitoring);
        assert_eq!(monitor_count(), 1);
        // A second MONITOR from the same connection is a no-op.
        assert!(!register_monitor(&mut monitor_state));
        assert_eq!(monitor_count(), 1);

        assert!(
            !can_use_raw_mako_fast_path(&conn),
            "an attached monitor must close the raw GET/SET shortcut"
        );

        // A command from another client is echoed to the monitor...
        conn.client_state.peer_addr = String::from("127.0.0.1:2");
        feed_monitors(
            &conn.client_state,
            &[Bytes::from_static(b"GET"), Bytes::from_static(b"k")],
        );
        // ...and the monitor's own is not.
        feed_monitors(
            &monitor_state,
            &[Bytes::from_static(b"PING")],
        );
        let delivered: Vec<String> = {
            let mut queue = monitor_state.pubsub_queue.lock().unwrap();
            queue
                .drain(..)
                .map(|line| String::from_utf8(line).unwrap())
                .collect()
        };
        assert_eq!(delivered.len(), 1, "{delivered:?}");
        assert!(
            delivered[0].ends_with("[0 127.0.0.1:2] \"GET\" \"k\"\r\n"),
            "{}",
            delivered[0]
        );

        let mut info = String::new();
        append_clients_info(&mut info);
        assert!(info.contains("monitor_clients:1\r\n"), "{info}");

        // RESET leaves monitor mode and reopens the fast path.
        let mut txn_state = TransactionState::new();
        assert_eq!(
            run(command(OpCode::Reset, &[]), &mut txn_state, &mut monitor_state),
            b"+RESET\r\n"
        );
        assert!(!monitor_state.monitoring);
        assert_eq!(monitor_count(), 0);
        assert!(can_use_raw_mako_fast_path(&conn));

        let mut info = String::new();
        append_clients_info(&mut info);
        assert!(info.contains("monitor_clients:0\r\n"), "{info}");

        // QUIT leaves it too.
        assert!(register_monitor(&mut monitor_state));
        assert_eq!(monitor_count(), 1);
        assert_eq!(
            run(command(OpCode::Quit, &[]), &mut txn_state, &mut monitor_state),
            b"+OK\r\n"
        );
        assert_eq!(monitor_count(), 0);

        // So does dropping the connection's state without a command.
        assert!(register_monitor(&mut monitor_state));
        unregister_all_client_feeds(&mut monitor_state);
        assert_eq!(monitor_count(), 0);
        assert!(can_use_raw_mako_fast_path(&conn));
    }

    #[test]
    fn cluster_keyslot_follows_redis_hash_tag_rules() {
        // The three values Redis's own CLUSTER KEYSLOT documentation prints.
        assert_eq!(cluster_key_slot(b"foo"), 12182);
        assert_eq!(cluster_key_slot(b"somekey"), 11058);
        assert_eq!(cluster_key_slot(b"foo{hash_tag}"), 2515);
        // A hash tag makes two different keys share a slot.
        assert_eq!(cluster_key_slot(b"{user1000}.following"), 3443);
        assert_eq!(cluster_key_slot(b"{user1000}.followers"), 3443);
        assert_eq!(cluster_key_slot(b"{user1000}"), 3443);
        // An empty or unterminated tag hashes the whole key instead.
        assert_eq!(cluster_key_slot(b"{}"), cluster_crc16(b"{}") % 16384);
        assert_eq!(
            cluster_key_slot(b"foo{}{bar}"),
            cluster_crc16(b"foo{}{bar}") % 16384
        );
        assert_eq!(cluster_key_slot(b"{unclosed"), cluster_crc16(b"{unclosed") % 16384);
        assert_eq!(cluster_key_slot(b""), 0);
        // Only the first tag counts, and the bytes need not be UTF-8.
        assert_eq!(cluster_key_slot(b"a{t}b{u}c"), cluster_key_slot(b"{t}"));
        assert_eq!(cluster_key_slot(b"{\xff\x00}x"), cluster_crc16(b"\xff\x00") % 16384);
    }

    #[test]
    fn cluster_node_id_is_stable_and_well_formed() {
        let first = cluster_node_id();
        assert_eq!(first.len(), 40);
        assert!(first.bytes().all(|byte| byte.is_ascii_hexdigit()));
        assert_eq!(first, cluster_node_id());
    }

    #[test]
    fn cluster_off_mode_reports_support_disabled() {
        let disabled = format!("-ERR {CLUSTER_DISABLED_ERROR}\r\n");
        for args in [&b"INFO"[..], b"SLOTS", b"MYID", b"BOGUS"] {
            assert_eq!(cluster(ClusterMode::Off, &[args]), disabled.as_bytes());
        }
        let mut readonly = Vec::new();
        write_cluster_readonly_reply(ClusterMode::Off, &mut readonly).unwrap();
        assert_eq!(readonly, disabled.as_bytes());
    }

    #[test]
    fn cluster_emulated_mode_describes_one_node_owning_every_slot() {
        let info = cluster(ClusterMode::Emulated, &[b"info"]);
        let info = String::from_utf8(info).unwrap();
        assert!(info.contains("cluster_state:ok\r\n"), "{info}");
        assert!(info.contains("cluster_slots_assigned:16384\r\n"), "{info}");
        assert!(info.contains("cluster_known_nodes:1\r\n"), "{info}");
        assert!(info.contains("cluster_size:1\r\n"), "{info}");

        let id = cluster_node_id();
        let (host, port) = cluster_announce();
        assert_eq!(
            cluster(ClusterMode::Emulated, &[b"MYID"]),
            format!("${}\r\n{id}\r\n", id.len()).into_bytes()
        );

        let slots = cluster(ClusterMode::Emulated, &[b"SLOTS"]);
        let expected = format!(
            "*1\r\n*3\r\n:0\r\n:16383\r\n*3\r\n${}\r\n{host}\r\n:{port}\r\n${}\r\n{id}\r\n",
            host.len(),
            id.len()
        );
        assert_eq!(slots, expected.into_bytes());

        let nodes = String::from_utf8(cluster(ClusterMode::Emulated, &[b"NODES"])).unwrap();
        assert!(
            nodes.contains(&format!(
                "{id} {host}:{port}@{} myself,master - 0 0 1 connected 0-16383\n",
                port + 10000
            )),
            "{nodes}"
        );

        assert_eq!(
            cluster(ClusterMode::Emulated, &[b"KEYSLOT", b"foo"]),
            b":12182\r\n"
        );
        assert_eq!(
            cluster(ClusterMode::Emulated, &[b"COUNTKEYSINSLOT", b"0"]),
            b":0\r\n"
        );
        assert_eq!(
            cluster(ClusterMode::Emulated, &[b"GETKEYSINSLOT", b"0", b"10"]),
            b"*0\r\n"
        );
        assert_eq!(
            cluster(ClusterMode::Emulated, &[b"COUNTKEYSINSLOT", b"16384"]),
            b"-ERR Invalid slot\r\n"
        );
        assert_eq!(
            cluster(ClusterMode::Emulated, &[b"nosuchthing"]),
            b"-ERR unknown subcommand 'nosuchthing'. Try CLUSTER HELP.\r\n"
        );

        let mut readonly = Vec::new();
        write_cluster_readonly_reply(ClusterMode::Emulated, &mut readonly).unwrap();
        assert_eq!(readonly, b"+OK\r\n");
    }

    #[test]
    fn cluster_shards_uses_the_redis_7_shape() {
        let shards = cluster(ClusterMode::Emulated, &[b"SHARDS"]);
        let id = cluster_node_id();
        let (host, port) = cluster_announce();
        let expected = format!(
            "*1\r\n*4\r\n$5\r\nslots\r\n*2\r\n:0\r\n:16383\r\n$5\r\nnodes\r\n*1\r\n*14\r\n\
             $2\r\nid\r\n${}\r\n{id}\r\n$4\r\nport\r\n:{port}\r\n$2\r\nip\r\n${}\r\n{host}\r\n\
             $8\r\nendpoint\r\n${}\r\n{host}\r\n$4\r\nrole\r\n$6\r\nmaster\r\n\
             $18\r\nreplication-offset\r\n:0\r\n$6\r\nhealth\r\n$6\r\nonline\r\n",
            id.len(),
            host.len(),
            host.len()
        );
        assert_eq!(String::from_utf8(shards).unwrap(), expected);
    }

    #[test]
    fn cluster_and_readonly_queue_inside_multi() {
        let mut txn_state = TransactionState::new();
        let mut client_state = ClientState::new();
        txn_state.start_multi();

        assert_eq!(
            run(
                command(OpCode::Cluster, &[b"INFO"]),
                &mut txn_state,
                &mut client_state
            ),
            b"+QUEUED\r\n"
        );
        assert_eq!(
            run(
                command(OpCode::ReadOnly, &[]),
                &mut txn_state,
                &mut client_state
            ),
            b"+QUEUED\r\n"
        );
        assert_eq!(txn_state.queued_commands.len(), 2);

        // The queued replies come back through write_command_result at EXEC.
        let mut out = Vec::new();
        write_command_result(
            &command(OpCode::ReadOnly, &[]),
            None,
            (0, 0),
            2,
            &mut out,
        )
        .unwrap();
        let expected = if cluster_mode() == ClusterMode::Emulated {
            "+OK\r\n".to_string()
        } else {
            format!("-ERR {CLUSTER_DISABLED_ERROR}\r\n")
        };
        assert_eq!(out, expected.into_bytes());
    }

    #[test]
    fn cluster_arity_errors_match_redis() {
        assert_eq!(
            run_raw(b"*1\r\n$7\r\nCLUSTER\r\n"),
            b"-ERR wrong number of arguments for 'cluster' command\r\n"
        );
        assert_eq!(
            run_raw(b"*2\r\n$8\r\nREADONLY\r\n$1\r\nx\r\n"),
            b"-ERR wrong number of arguments for 'readonly' command\r\n"
        );
        assert_eq!(
            run_raw(b"*2\r\n$9\r\nREADWRITE\r\n$1\r\nx\r\n"),
            b"-ERR wrong number of arguments for 'readwrite' command\r\n"
        );
    }

    #[test]
    fn info_reports_a_cluster_section() {
        let mut out = String::new();
        append_cluster_info(&mut out);
        assert!(out.starts_with("# Cluster\r\ncluster_enabled:"), "{out}");
        assert!(out.ends_with("\r\n\r\n"), "{out}");
    }

    #[test]
    fn quit_marks_connection_for_close() {
        let mut txn_state = TransactionState::new();
        let mut client_state = ClientState::new();

        let out = run(
            command(OpCode::Quit, &[]),
            &mut txn_state,
            &mut client_state,
        );

        assert_eq!(out, b"+OK\r\n");
        assert!(client_state.close_after_reply);
    }

    #[test]
    fn select_auth_and_echo_are_pure_connection_commands() {
        let mut txn_state = TransactionState::new();
        let mut client_state = ClientState::new();

        let select = run(
            command(OpCode::Select, &[b"0"]),
            &mut txn_state,
            &mut client_state,
        );
        let auth = run(
            command(OpCode::Auth, &[b"default", b"secret"]),
            &mut txn_state,
            &mut client_state,
        );
        let echo = run(
            command(OpCode::Echo, &[b"hello"]),
            &mut txn_state,
            &mut client_state,
        );

        assert_eq!(select, b"+OK\r\n");
        assert_eq!(auth, b"+OK\r\n");
        assert_eq!(echo, b"$5\r\nhello\r\n");
    }

    #[test]
    fn command_command_returns_a_parseable_reply() {
        let mut txn_state = TransactionState::new();
        let mut client_state = ClientState::new();

        let out = run(
            command(OpCode::Command, &[]),
            &mut txn_state,
            &mut client_state,
        );

        assert!(out.starts_with(b"*"));
    }

    #[test]
    fn config_get_and_resetstat_return_client_compatible_replies() {
        let mut txn_state = TransactionState::new();
        let mut client_state = ClientState::new();

        let get_save = run(
            command(OpCode::Config, &[b"GET", b"save"]),
            &mut txn_state,
            &mut client_state,
        );
        let get_all = run(
            command(OpCode::Config, &[b"GET", b"*"]),
            &mut txn_state,
            &mut client_state,
        );
        let resetstat = run(
            command(OpCode::Config, &[b"RESETSTAT"]),
            &mut txn_state,
            &mut client_state,
        );

        assert_eq!(get_save, b"*2\r\n$4\r\nsave\r\n$0\r\n\r\n");
        // 14 known keys, each reported as a name/value pair. The last two are
        // lua-time-limit and its Redis 7 name busy-reply-threshold.
        assert!(get_all.starts_with(b"*28\r\n"));
        assert_eq!(resetstat, b"+OK\r\n");
    }

    #[test]
    fn shared_listener_accept_turn_cycles_across_workers() {
        let next_worker = AtomicUsize::new(0);

        for expected in [0, 1, 2, 3, 0, 1] {
            assert!(worker_has_accept_turn(&next_worker, expected));
            assert!(!worker_has_accept_turn(&next_worker, (expected + 1) % 4));
            advance_accept_turn(&next_worker, expected, 4);
        }
    }

    #[test]
    fn info_stats_reports_and_resetstat_clears_processed_commands() {
        let mut txn_state = TransactionState::new();
        let mut client_state = ClientState::new();
        reset_worker_counter(&WORKER_COMMANDS_PROCESSED, &TOTAL_COMMANDS_PROCESSED);

        record_command_call(OpCode::Ping);
        record_command_call(OpCode::Get);
        let out = run(
            command(OpCode::Info, &[b"stats"]),
            &mut txn_state,
            &mut client_state,
        );
        let text = String::from_utf8(out).unwrap();
        assert!(text.contains("total_commands_processed:2\r\n"));

        let reset = run(
            command(OpCode::Config, &[b"RESETSTAT"]),
            &mut txn_state,
            &mut client_state,
        );
        assert_eq!(reset, b"+OK\r\n");

        let out = run(
            command(OpCode::Info, &[b"stats"]),
            &mut txn_state,
            &mut client_state,
        );
        let text = String::from_utf8(out).unwrap();
        assert!(text.contains("total_commands_processed:0\r\n"));
    }

    #[test]
    fn info_server_returns_parseable_server_section() {
        let mut txn_state = TransactionState::new();
        let mut client_state = ClientState::new();
        TOTAL_CONNECTIONS_RECEIVED.store(7, Ordering::Relaxed);

        let out = run(
            command(OpCode::Info, &[b"server"]),
            &mut txn_state,
            &mut client_state,
        );
        let text = String::from_utf8(out).unwrap();

        assert!(text.starts_with("$"));
        assert!(text.contains("# Server\r\n"));
        assert!(text.contains("redis_version:"));
        assert!(text.contains("mako_version:"));
        assert!(text.contains("total_connections_received:7\r\n"));
        assert!(text.contains("uptime_in_seconds:42\r\n"));
    }

    #[test]
    fn info_clients_returns_connection_metrics() {
        let mut txn_state = TransactionState::new();
        let mut client_state = ClientState::new();
        CONNECTED_CLIENTS.store(3, Ordering::Relaxed);

        let out = run(
            command(OpCode::Info, &[b"clients"]),
            &mut txn_state,
            &mut client_state,
        );
        let text = String::from_utf8(out).unwrap();

        assert!(text.contains("# Clients\r\n"));
        assert!(text.contains("connected_clients:3\r\n"));
    }

    #[test]
    fn info_mako_returns_transaction_metrics() {
        let mut txn_state = TransactionState::new();
        let mut client_state = ClientState::new();

        let out = run(
            command(OpCode::Info, &[b"mako"]),
            &mut txn_state,
            &mut client_state,
        );
        let text = String::from_utf8(out).unwrap();

        assert!(text.contains("# Mako\r\n"));
        assert!(text.contains("mako_txn_commits:11\r\n"));
        assert!(text.contains("mako_txn_aborts:2\r\n"));
        assert!(text.contains("mako_txn_retries:3\r\n"));
    }

    #[test]
    fn get_uses_value_present_for_empty_string() {
        let cmd = data_command(OpCode::Get, &[b"k"], None);
        let mut results = vec![TxnOpResult {
            success: true,
            value_present: true,
            data_ptr: std::ptr::null_mut(),
            data_len: 0,
            int_value: 0,
        }];
        let response = TxnResponse {
            transaction_success: true,
            num_results: results.len(),
            results: results.as_mut_ptr(),
        };
        let mut out = Vec::new();

        write_command_result(&cmd, Some(&response), (0, 1), 2, &mut out).unwrap();

        assert_eq!(out, b"$0\r\n\r\n");
    }

    #[test]
    fn get_missing_key_returns_nil_bulk() {
        let cmd = data_command(OpCode::Get, &[b"k"], None);
        let mut results = vec![TxnOpResult {
            success: true,
            value_present: false,
            data_ptr: std::ptr::null_mut(),
            data_len: 0,
            int_value: 0,
        }];
        let response = TxnResponse {
            transaction_success: true,
            num_results: results.len(),
            results: results.as_mut_ptr(),
        };
        let mut out = Vec::new();

        write_command_result(&cmd, Some(&response), (0, 1), 2, &mut out).unwrap();

        assert_eq!(out, b"$-1\r\n");
    }

    #[test]
    fn get_missing_key_returns_resp3_null_under_protocol_3() {
        let cmd = data_command(OpCode::Get, &[b"k"], None);
        let mut results = vec![TxnOpResult {
            success: true,
            value_present: false,
            data_ptr: std::ptr::null_mut(),
            data_len: 0,
            int_value: 0,
        }];
        let response = TxnResponse {
            transaction_success: true,
            num_results: results.len(),
            results: results.as_mut_ptr(),
        };
        let mut out = Vec::new();

        write_command_result(&cmd, Some(&response), (0, 1), 3, &mut out).unwrap();

        assert_eq!(out, b"_\r\n");
    }

    #[test]
    fn hgetall_uses_a_map_only_for_resp3() {
        let cmd = data_command(OpCode::HGetAll, &[b"h"], None);
        let payload = pack_bytes_list(&[
            Bytes::from_static(b"a"),
            Bytes::from_static(b"1"),
            Bytes::from_static(b"b"),
            Bytes::from_static(b"2"),
        ]);
        let mut results = vec![TxnOpResult {
            success: true,
            value_present: true,
            data_ptr: payload.as_ptr() as *mut u8,
            data_len: payload.len(),
            int_value: 0,
        }];
        let response = TxnResponse {
            transaction_success: true,
            num_results: results.len(),
            results: results.as_mut_ptr(),
        };
        let mut resp2 = Vec::new();
        let mut resp3 = Vec::new();

        write_command_result(&cmd, Some(&response), (0, 1), 2, &mut resp2).unwrap();
        write_command_result(&cmd, Some(&response), (0, 1), 3, &mut resp3).unwrap();

        assert_eq!(resp2, b"*4\r\n$1\r\na\r\n$1\r\n1\r\n$1\r\nb\r\n$1\r\n2\r\n");
        assert_eq!(resp3, b"%2\r\n$1\r\na\r\n$1\r\n1\r\n$1\r\nb\r\n$1\r\n2\r\n");
    }

    #[test]
    fn zscore_uses_a_double_only_for_resp3() {
        let cmd = data_command(OpCode::ZScore, &[b"z"], Some(b"member"));
        let score = b"1.5";
        let mut results = vec![TxnOpResult {
            success: true,
            value_present: true,
            data_ptr: score.as_ptr() as *mut u8,
            data_len: score.len(),
            int_value: 0,
        }];
        let response = TxnResponse {
            transaction_success: true,
            num_results: results.len(),
            results: results.as_mut_ptr(),
        };
        let mut resp2 = Vec::new();
        let mut resp3 = Vec::new();

        write_command_result(&cmd, Some(&response), (0, 1), 2, &mut resp2).unwrap();
        write_command_result(&cmd, Some(&response), (0, 1), 3, &mut resp3).unwrap();

        assert_eq!(resp2, b"$3\r\n1.5\r\n");
        assert_eq!(resp3, b",1.5\r\n");
    }

    #[test]
    fn pubsub_enqueue_notifies_the_owning_worker() {
        let wake = Arc::new(WorkerWake::new().unwrap());
        let state = ClientState::for_worker(&wake);
        let target = make_pubsub_target(&state);

        assert!(enqueue_pubsub_reply(&target, b"message"));
        assert_eq!(
            state.pubsub_queue.lock().unwrap().pop_front().unwrap(),
            b"message"
        );

        let mut notification = [0u8; 1];
        let mut reader = &wake.reader;
        assert_eq!(reader.read(&mut notification).unwrap(), 1);
        assert_eq!(notification, [1]);
    }

    #[test]
    fn blocked_client_broadcast_notifies_every_worker() {
        let first = Arc::new(WorkerWake::new().unwrap());
        let second = Arc::new(WorkerWake::new().unwrap());
        let wakes = vec![Arc::downgrade(&first), Arc::downgrade(&second)];

        notify_worker_wakes(&wakes);

        for wake in [&first, &second] {
            let mut notification = [0u8; 1];
            let mut reader = &wake.reader;
            assert_eq!(reader.read(&mut notification).unwrap(), 1);
            assert_eq!(notification, [1]);
        }
    }

    #[test]
    fn dirty_exec_is_classified_as_a_blocked_client_wakeup() {
        let mut txn_state = TransactionState::new();
        txn_state.start_multi();
        txn_state.queue_command(Command::new(
            OpCode::LPush,
            vec![Bytes::from_static(b"list")],
            None,
            Vec::new(),
        ));

        assert!(command_may_wake_blocked(
            &txn_state,
            &command(OpCode::Exec, &[])
        ));
        assert!(!command_may_wake_blocked(
            &txn_state,
            &data_command(OpCode::Get, &[b"key"], None)
        ));
    }

    #[test]
    fn blocked_registry_preserves_per_key_registration_order() {
        let key = Bytes::from_static(b"list");
        let cmd = Command::new(OpCode::BLPop, vec![key.clone()], None, Vec::new());
        let mut registry = BlockedClientRegistry::default();

        registry.register(11, &cmd);
        registry.register(12, &cmd);
        assert!(registry.has_turn(11));
        assert!(!registry.has_turn(12));
        assert_eq!(registry.fronts_for_keys(&[key.clone()]), vec![(key, 0, 11)]);

        registry.unregister(11);
        assert!(registry.has_turn(12));
    }

    #[test]
    fn blocked_registry_limits_multi_key_retry_to_front_queues() {
        let first = Command::new(
            OpCode::BLPop,
            vec![Bytes::from_static(b"first")],
            None,
            Vec::new(),
        );
        let second = Command::new(
            OpCode::BLMPop,
            vec![Bytes::from_static(b"first"), Bytes::from_static(b"second")],
            None,
            Vec::new(),
        );
        let mut registry = BlockedClientRegistry::default();
        registry.register(21, &first);
        registry.register(22, &second);

        assert_eq!(
            registry.eligible_keys(22, &second.keys),
            vec![Bytes::from_static(b"second")]
        );
        registry.unregister(21);
        assert_eq!(registry.eligible_keys(22, &second.keys), second.keys);
    }

    #[test]
    fn a_blocked_stream_reader_does_not_queue_behind_a_blocked_list_popper() {
        // A key holds one type at a time, so a pop and a stream read on the
        // same key never compete for the same data. Queueing them together let
        // a pop that can never be served hold the front forever.
        let key = Bytes::from_static(b"k");
        let pop = Command::new(OpCode::BLPop, vec![key.clone()], None, Vec::new());
        let read = Command::new(OpCode::XReadGroup, vec![key.clone()], None, Vec::new());
        let mut registry = BlockedClientRegistry::default();

        registry.register(31, &pop);
        registry.register(32, &read);
        assert!(registry.has_turn(31));
        assert!(registry.has_turn(32));
        assert_eq!(registry.eligible_keys(32, &read.keys), read.keys);
        assert_eq!(
            registry.fronts_for_keys(&[key.clone()]),
            vec![(key.clone(), 0, 31), (key.clone(), 1, 32)]
        );
        // Only XREADGROUP waits for the key itself to appear.
        assert_eq!(registry.blocking_key_counts(), (1, 1));

        // Within a class the queue still hands out turns in arrival order.
        registry.register(33, &read);
        assert!(registry.has_turn(32));
        assert!(!registry.has_turn(33));
        registry.unregister(32);
        assert!(registry.has_turn(33));
    }

    #[test]
    fn duplicate_exists_counts_each_matching_argument() {
        let cmd = data_command(OpCode::Exists, &[b"k", b"k", b"k"], None);
        let mut results = vec![
            TxnOpResult {
                success: true,
                value_present: true,
                data_ptr: std::ptr::null_mut(),
                data_len: 0,
                int_value: 0,
            },
            TxnOpResult {
                success: true,
                value_present: true,
                data_ptr: std::ptr::null_mut(),
                data_len: 0,
                int_value: 0,
            },
            TxnOpResult {
                success: true,
                value_present: true,
                data_ptr: std::ptr::null_mut(),
                data_len: 0,
                int_value: 0,
            },
        ];
        let response = TxnResponse {
            transaction_success: true,
            num_results: results.len(),
            results: results.as_mut_ptr(),
        };
        let mut out = Vec::new();

        write_command_result(&cmd, Some(&response), (0, 3), 2, &mut out).unwrap();

        assert_eq!(out, b":3\r\n");
    }

    #[test]
    fn unlink_parses_as_delete() {
        let out = run_raw(b"*2\r\n$6\r\nUNLINK\r\n$1\r\nk\r\n");

        assert_eq!(out, b"-ERR backend\r\n");
    }

    #[test]
    fn decrby_reports_redis_texts_for_a_decrement_it_cannot_negate() {
        // DECRBY runs as INCRBY with the amount negated, so these two are the
        // only INCR-family errors decided before the executor sees the
        // command. They used to read "ERR protocol error: invalid argument"
        // and "ERR protocol error: increment or decrement would overflow".
        let out = run_raw(b"*3\r\n$6\r\nDECRBY\r\n$1\r\nk\r\n$3\r\nfoo\r\n");
        assert_eq!(out, b"-ERR value is not an integer or out of range\r\n");

        let out = run_raw(b"*3\r\n$6\r\nDECRBY\r\n$1\r\nk\r\n$4\r\n9e99\r\n");
        assert_eq!(out, b"-ERR value is not an integer or out of range\r\n");

        let out =
            run_raw(b"*3\r\n$6\r\nDECRBY\r\n$1\r\nk\r\n$20\r\n-9223372036854775808\r\n");
        assert_eq!(out, b"-ERR decrement would overflow\r\n");
    }

    #[test]
    fn ping_inside_multi_is_queued_and_returned_by_exec() {
        let mut txn_state = TransactionState::new();
        let mut client_state = ClientState::new();

        let multi = run(
            command(OpCode::Multi, &[]),
            &mut txn_state,
            &mut client_state,
        );
        let ping = run(
            command(OpCode::Ping, &[]),
            &mut txn_state,
            &mut client_state,
        );
        let exec = run(
            command(OpCode::Exec, &[]),
            &mut txn_state,
            &mut client_state,
        );

        assert_eq!(multi, b"+OK\r\n");
        assert_eq!(ping, b"+QUEUED\r\n");
        assert_eq!(exec, b"*1\r\n+PONG\r\n");
    }

    #[test]
    fn subscribe_publish_enqueues_message() {
        let mut subscriber_txn = TransactionState::new();
        let mut subscriber_state = ClientState::new();
        let channel = Bytes::from(format!("phase9:{}", subscriber_state.id));

        let subscribe = run(
            Command::new(OpCode::Subscribe, Vec::new(), None, vec![channel.clone()]),
            &mut subscriber_txn,
            &mut subscriber_state,
        );
        assert_eq!(
            subscribe,
            format!(
                "*3\r\n$9\r\nsubscribe\r\n${}\r\n{}\r\n:1\r\n",
                channel.len(),
                String::from_utf8_lossy(&channel)
            )
            .into_bytes()
        );

        let mut publisher_txn = TransactionState::new();
        let mut publisher_state = ClientState::new();
        let publish = run(
            Command::new(
                OpCode::Publish,
                vec![channel.clone()],
                Some(Bytes::from_static(b"hello")),
                vec![channel.clone(), Bytes::from_static(b"hello")],
            ),
            &mut publisher_txn,
            &mut publisher_state,
        );
        assert_eq!(publish, b":1\r\n");

        let message = subscriber_state
            .pubsub_queue
            .lock()
            .unwrap()
            .pop_front()
            .unwrap();
        assert_eq!(
            message,
            format!(
                "*3\r\n$7\r\nmessage\r\n${}\r\n{}\r\n$5\r\nhello\r\n",
                channel.len(),
                String::from_utf8_lossy(&channel)
            )
            .into_bytes()
        );

        unregister_all_pubsub(&mut subscriber_state);
    }

    #[test]
    fn subscriber_mode_rejects_storage_commands_until_unsubscribe() {
        let mut txn_state = TransactionState::new();
        let mut client_state = ClientState::new();
        let channel = Bytes::from(format!("phase9:reject:{}", client_state.id));

        let _ = run(
            Command::new(OpCode::Subscribe, Vec::new(), None, vec![channel.clone()]),
            &mut txn_state,
            &mut client_state,
        );
        let rejected = run(
            data_command(OpCode::Get, &[b"k"], None),
            &mut txn_state,
            &mut client_state,
        );
        assert!(String::from_utf8(rejected)
            .unwrap()
            .contains("allowed in subscriber mode"));

        let _ = run(
            Command::new(OpCode::Unsubscribe, Vec::new(), None, vec![channel.clone()]),
            &mut txn_state,
            &mut client_state,
        );
        let backend = run(
            data_command(OpCode::Get, &[b"k"], None),
            &mut txn_state,
            &mut client_state,
        );
        assert_eq!(backend, b"-ERR backend\r\n");
    }

    #[test]
    fn pubsub_introspection_reports_live_channels_and_patterns() {
        let mut txn_state = TransactionState::new();
        let mut client_state = ClientState::new();
        let channel = Bytes::from(format!("phase9:introspect:{}", client_state.id));
        let pattern = Bytes::from_static(b"phase9:introspect:*");

        let _ = run(
            Command::new(OpCode::Subscribe, Vec::new(), None, vec![channel.clone()]),
            &mut txn_state,
            &mut client_state,
        );
        let _ = run(
            Command::new(OpCode::PSubscribe, Vec::new(), None, vec![pattern.clone()]),
            &mut txn_state,
            &mut client_state,
        );

        let mut viewer_txn = TransactionState::new();
        let mut viewer_state = ClientState::new();
        let channels = run(
            Command::new(
                OpCode::PubSub,
                Vec::new(),
                None,
                vec![
                    Bytes::from_static(b"CHANNELS"),
                    Bytes::from_static(b"phase9:introspect:*"),
                ],
            ),
            &mut viewer_txn,
            &mut viewer_state,
        );
        let numsub = run(
            Command::new(
                OpCode::PubSub,
                Vec::new(),
                None,
                vec![Bytes::from_static(b"NUMSUB"), channel.clone()],
            ),
            &mut viewer_txn,
            &mut viewer_state,
        );
        let numpat = run(
            Command::new(
                OpCode::PubSub,
                Vec::new(),
                None,
                vec![Bytes::from_static(b"NUMPAT")],
            ),
            &mut viewer_txn,
            &mut viewer_state,
        );

        let channels_text = String::from_utf8(channels).unwrap();
        assert!(channels_text.contains(&String::from_utf8_lossy(&channel).to_string()));
        assert_eq!(
            numsub,
            format!(
                "*2\r\n${}\r\n{}\r\n:1\r\n",
                channel.len(),
                String::from_utf8_lossy(&channel)
            )
            .into_bytes()
        );
        assert!(String::from_utf8(numpat).unwrap().starts_with(":"));

        unregister_all_pubsub(&mut client_state);
    }

    #[test]
    fn publish_inside_multi_delivers_at_exec() {
        let mut subscriber_txn = TransactionState::new();
        let mut subscriber_state = ClientState::new();
        let channel = Bytes::from(format!("phase9:multi:{}", subscriber_state.id));

        let _ = run(
            Command::new(OpCode::Subscribe, Vec::new(), None, vec![channel.clone()]),
            &mut subscriber_txn,
            &mut subscriber_state,
        );

        let mut publisher_txn = TransactionState::new();
        let mut publisher_state = ClientState::new();
        assert_eq!(
            run(
                command(OpCode::Multi, &[]),
                &mut publisher_txn,
                &mut publisher_state
            ),
            b"+OK\r\n"
        );
        assert_eq!(
            run(
                Command::new(
                    OpCode::Publish,
                    vec![channel.clone()],
                    Some(Bytes::from_static(b"queued")),
                    vec![channel.clone(), Bytes::from_static(b"queued")],
                ),
                &mut publisher_txn,
                &mut publisher_state,
            ),
            b"+QUEUED\r\n"
        );
        assert!(subscriber_state.pubsub_queue.lock().unwrap().is_empty());

        let exec = run(
            command(OpCode::Exec, &[]),
            &mut publisher_txn,
            &mut publisher_state,
        );
        assert_eq!(exec, b"*1\r\n:1\r\n");
        assert!(subscriber_state
            .pubsub_queue
            .lock()
            .unwrap()
            .pop_front()
            .is_some());

        unregister_all_pubsub(&mut subscriber_state);
    }

    #[test]
    fn unknown_command_reports_command_and_first_arg() {
        let out = run_raw(b"*2\r\n$3\r\nFOO\r\n$3\r\nbar\r\n");

        assert_eq!(
            out,
            b"-ERR unknown command 'FOO', with args beginning with: 'bar'\r\n"
        );
    }

    fn parse_one(input: &[u8]) -> Command {
        let mut resp3 = Resp3Handler::new(1024);
        resp3.read_bytes(input);
        let frame = resp3.next_frame().unwrap().unwrap();
        parse_resp3(frame).ok().unwrap()
    }

    #[test]
    fn xread_builds_one_range_op_per_stream() {
        // XREAD COUNT 2 BLOCK 100 STREAMS a b 0-0 $: two ops, in the order the
        // command named the streams, and the BLOCK timeout parked on the
        // command rather than turned into a TTL.
        let cmd = parsed(
            0,
            &[
                b"XREAD", b"COUNT", b"2", b"BLOCK", b"100", b"STREAMS", b"a", b"b", b"0-0", b"$",
            ],
        );
        assert!(cmd.op == OpCode::XRead);
        assert_eq!(
            cmd.keys,
            vec![Bytes::from_static(b"a"), Bytes::from_static(b"b")]
        );
        assert_eq!(
            cmd.values,
            vec![
                Bytes::new(),
                Bytes::new(),
                Bytes::from_static(b"0"),
                Bytes::from_static(b"0-0"),
                Bytes::from_static(b"$"),
            ]
        );
        assert_eq!(cmd.scan_count, 2);
        assert_eq!(cmd.expire_at_ms, 100);
        assert!(command_blocks(&cmd));
        let (ops, spans, payloads) = build_txn_ops(std::slice::from_ref(&cmd));
        assert_eq!(ops.len(), 2);
        assert_eq!(spans, vec![(0, 2)]);
        assert!(ops.iter().all(|op| op.op == TXN_OP_XRANGE));
        // An explicit ID is exclusive, so the range starts one past it.
        assert_eq!(
            parse_list_payload(&payloads[0]).unwrap(),
            vec![
                b"0-1".to_vec(),
                b"18446744073709551615-18446744073709551615".to_vec(),
                b"2".to_vec(),
                b"0".to_vec(),
            ]
        );
        // "$" reads no entry at all; it only reports the stream's last ID.
        assert_eq!(
            parse_list_payload(&payloads[1]).unwrap(),
            vec![
                b"0-0".to_vec(),
                b"0-0".to_vec(),
                b"0".to_vec(),
                b"2".to_vec(),
            ]
        );

        // Without BLOCK the command answers and never parks.
        let cmd = parsed(0, &[b"XREAD", b"STREAMS", b"a", b"b", b"5", b"5"]);
        assert_eq!(cmd.expire_at_ms, -1);
        assert!(!command_blocks(&cmd));
    }

    #[test]
    fn xread_refuses_the_arguments_only_xreadgroup_accepts() {
        assert_eq!(
            run_raw(b"*4\r\n$5\r\nXREAD\r\n$7\r\nSTREAMS\r\n$1\r\na\r\n$1\r\n>\r\n"),
            b"-ERR The > ID can be specified only when calling XREADGROUP using the \
GROUP <group> <consumer> option.\r\n"
        );
        assert_eq!(
            run_raw(b"*5\r\n$5\r\nXREAD\r\n$5\r\nCOUNT\r\n$1\r\n1\r\n$7\r\nSTREAMS\r\n$1\r\na\r\n"),
            b"-ERR Unbalanced 'xread' list of streams: for each stream key an ID or '$' \
must be specified.\r\n"
        );
        assert_eq!(
            run_raw(b"*6\r\n$5\r\nXREAD\r\n$5\r\nBLOCK\r\n$2\r\n-1\r\n$7\r\nSTREAMS\r\n$1\r\na\r\n$1\r\n0\r\n"),
            b"-ERR timeout is negative\r\n"
        );
    }

    #[test]
    fn hyperloglog_commands_report_wrong_arity() {
        assert_eq!(
            run_raw(b"*1\r\n$5\r\nPFADD\r\n"),
            b"-ERR wrong number of arguments for 'pfadd' command\r\n"
        );
        assert_eq!(
            run_raw(b"*1\r\n$7\r\nPFCOUNT\r\n"),
            b"-ERR wrong number of arguments for 'pfcount' command\r\n"
        );
        assert_eq!(
            run_raw(b"*1\r\n$7\r\nPFMERGE\r\n"),
            b"-ERR wrong number of arguments for 'pfmerge' command\r\n"
        );
    }

    #[test]
    fn hyperloglog_commands_build_a_single_op() {
        // PFADD: key is the sketch, the payload carries the elements.
        let pfadd = parse_one(b"*4\r\n$5\r\nPFADD\r\n$3\r\nhll\r\n$1\r\na\r\n$1\r\nb\r\n");
        assert!(pfadd.op == OpCode::PfAdd);
        assert_eq!(pfadd.keys, vec![Bytes::from_static(b"hll")]);
        let (ops, spans, payloads) = build_txn_ops(std::slice::from_ref(&pfadd));
        assert_eq!(ops.len(), 1);
        assert_eq!(ops[0].op, TXN_OP_HLL_ADD);
        assert_eq!(spans, vec![(0, 1)]);
        assert_eq!(
            parse_list_payload(&payloads[0]).unwrap(),
            vec![b"a".to_vec(), b"b".to_vec()]
        );

        // PFADD with no elements still builds one op with an empty payload.
        let bare = parse_one(b"*2\r\n$5\r\nPFADD\r\n$3\r\nhll\r\n");
        let (ops, _, payloads) = build_txn_ops(std::slice::from_ref(&bare));
        assert_eq!(ops.len(), 1);
        assert!(parse_list_payload(&payloads[0]).unwrap().is_empty());

        // PFCOUNT keeps every key in cmd.keys and repeats them in the payload.
        let pfcount = parse_one(b"*3\r\n$7\r\nPFCOUNT\r\n$2\r\nk1\r\n$2\r\nk2\r\n");
        assert!(pfcount.op == OpCode::PfCount);
        assert_eq!(
            pfcount.keys,
            vec![Bytes::from_static(b"k1"), Bytes::from_static(b"k2")]
        );
        let (ops, _, payloads) = build_txn_ops(std::slice::from_ref(&pfcount));
        assert_eq!(ops.len(), 1);
        assert_eq!(ops[0].op, TXN_OP_HLL_COUNT);
        assert_eq!(
            parse_list_payload(&payloads[0]).unwrap(),
            vec![b"k1".to_vec(), b"k2".to_vec()]
        );

        // PFMERGE: destination in keys[0], sources in the payload.
        let pfmerge = parse_one(b"*3\r\n$7\r\nPFMERGE\r\n$4\r\ndest\r\n$3\r\nsrc\r\n");
        assert!(pfmerge.op == OpCode::PfMerge);
        assert_eq!(pfmerge.keys, vec![Bytes::from_static(b"dest")]);
        let (ops, _, payloads) = build_txn_ops(std::slice::from_ref(&pfmerge));
        assert_eq!(ops.len(), 1);
        assert_eq!(ops[0].op, TXN_OP_HLL_MERGE);
        assert_eq!(
            parse_list_payload(&payloads[0]).unwrap(),
            vec![b"src".to_vec()]
        );

        // Writers are dirty; the read-only union query is not.
        assert!(is_dirty_command(OpCode::PfAdd));
        assert!(is_dirty_command(OpCode::PfMerge));
        assert!(!is_dirty_command(OpCode::PfCount));
        assert!(command_needs_retry(&pfadd));
        assert!(command_needs_retry(&pfcount));
        assert!(command_needs_retry(&pfmerge));
    }

    #[test]
    fn bitfield_writes_build_one_op_with_four_item_groups() {
        // INCRBY i5 100 1 GET u4 0: one write op carrying both subcommands.
        let cmd = parse_one(
            b"*9\r\n$8\r\nBITFIELD\r\n$5\r\nmykey\r\n$6\r\nINCRBY\r\n$2\r\ni5\r\n\
$3\r\n100\r\n$1\r\n1\r\n$3\r\nGET\r\n$2\r\nu4\r\n$1\r\n0\r\n",
        );
        assert!(cmd.op == OpCode::BitField);
        assert_eq!(cmd.keys, vec![Bytes::from_static(b"mykey")]);
        let (ops, spans, payloads) = build_txn_ops(std::slice::from_ref(&cmd));
        assert_eq!(ops.len(), 1);
        assert_eq!(ops[0].op, TXN_OP_BITFIELD);
        assert_eq!(spans, vec![(0, 1)]);
        assert_eq!(
            parse_list_payload(&payloads[0]).unwrap(),
            vec![
                b"INCRBY".to_vec(),
                b"i5".to_vec(),
                b"100".to_vec(),
                b"1".to_vec(),
                b"GET".to_vec(),
                b"u4".to_vec(),
                b"0".to_vec(),
                Vec::new(),
            ]
        );

        // OVERFLOW takes a group of its own and carries no offset or value.
        // "#1" offsets are multiplied out by the parser.
        let cmd = parse_one(
            b"*8\r\n$8\r\nBITFIELD\r\n$1\r\nk\r\n$8\r\nOVERFLOW\r\n$3\r\nSat\r\n\
$3\r\nset\r\n$2\r\nu8\r\n$2\r\n#2\r\n$3\r\n255\r\n",
        );
        assert_eq!(
            parse_list_payload(&build_txn_ops(std::slice::from_ref(&cmd)).2[0]).unwrap(),
            vec![
                b"OVERFLOW".to_vec(),
                b"SAT".to_vec(),
                Vec::new(),
                Vec::new(),
                b"SET".to_vec(),
                b"u8".to_vec(),
                b"16".to_vec(),
                b"255".to_vec(),
            ]
        );

        // A write-carrying BITFIELD is a dirty, retryable storage command.
        assert!(is_dirty_command(OpCode::BitField));
        assert!(command_needs_retry(&cmd));
    }

    #[test]
    fn bitfield_without_writes_runs_as_bitfield_ro() {
        // Redis treats an all-GET BITFIELD as read-only; so does the adapter,
        // which keeps it off the write path entirely.
        let cmd = parse_one(
            b"*10\r\n$8\r\nBITFIELD\r\n$1\r\nk\r\n$8\r\nOVERFLOW\r\n$4\r\nFAIL\r\n\
$3\r\nGET\r\n$2\r\nu8\r\n$1\r\n0\r\n$3\r\nGET\r\n$3\r\ni16\r\n$1\r\n8\r\n",
        );
        assert!(cmd.op == OpCode::BitFieldRo);
        assert!(!is_dirty_command(cmd.op));
        assert_eq!(
            cmd.values,
            vec![
                Bytes::from_static(b"u8"),
                Bytes::from_static(b"0"),
                Bytes::from_static(b"i16"),
                Bytes::from_static(b"8"),
            ]
        );
        let (ops, _, _) = build_txn_ops(std::slice::from_ref(&cmd));
        assert_eq!(ops.len(), 1);
        assert_eq!(ops[0].op, TXN_OP_GETRANGE);

        // No subcommands at all is read-only too, and replies with an empty array.
        let empty = parse_one(b"*2\r\n$8\r\nBITFIELD\r\n$1\r\nk\r\n");
        assert!(empty.op == OpCode::BitFieldRo);
        assert!(empty.values.is_empty());
    }

    #[test]
    fn bitfield_rejects_malformed_subcommands() {
        assert_eq!(
            run_raw(b"*1\r\n$8\r\nBITFIELD\r\n"),
            b"-ERR wrong number of arguments for 'bitfield' command\r\n"
        );
        assert_eq!(
            run_raw(b"*5\r\n$8\r\nBITFIELD\r\n$1\r\nk\r\n$3\r\nGET\r\n$3\r\nu64\r\n$1\r\n0\r\n"),
            b"-ERR Invalid bitfield type. Use something like i16 u8. \
Note that u64 is not supported but i64 is.\r\n"
        );
        assert_eq!(
            run_raw(b"*6\r\n$8\r\nBITFIELD\r\n$1\r\nk\r\n$3\r\nSET\r\n$2\r\nu8\r\n$2\r\n-1\r\n$1\r\n1\r\n"),
            b"-ERR bit offset is not an integer or out of range\r\n"
        );
        assert_eq!(
            run_raw(b"*6\r\n$8\r\nBITFIELD\r\n$1\r\nk\r\n$3\r\nSET\r\n$2\r\nu8\r\n$1\r\n0\r\n$2\r\nxy\r\n"),
            b"-ERR value is not an integer or out of range\r\n"
        );
        assert_eq!(
            run_raw(b"*4\r\n$8\r\nBITFIELD\r\n$1\r\nk\r\n$8\r\nOVERFLOW\r\n$4\r\nNOPE\r\n"),
            b"-ERR Invalid OVERFLOW type specified\r\n"
        );
        // A subcommand that is neither GET/SET/INCRBY/OVERFLOW, and one whose
        // arguments run out, are both plain syntax errors.
        assert_eq!(
            run_raw(b"*5\r\n$8\r\nBITFIELD\r\n$1\r\nk\r\n$3\r\nDEL\r\n$2\r\nu8\r\n$1\r\n0\r\n"),
            b"-ERR syntax error\r\n"
        );
        assert_eq!(
            run_raw(b"*4\r\n$8\r\nBITFIELD\r\n$1\r\nk\r\n$3\r\nGET\r\n$2\r\nu8\r\n"),
            b"-ERR syntax error\r\n"
        );
    }

    /// The two positions from the Redis GEOADD documentation and the 52-bit
    /// scores Redis stores for them.
    const PALERMO: (f64, f64, u64) = (13.361389, 38.115556, 3479099956230698);
    const CATANIA: (f64, f64, u64) = (15.087269, 37.502669, 3479447370796909);

    /// The `[min, max)` score ranges a built geo search asks for.
    fn decode_range_payloads(payloads: &[Bytes]) -> Vec<(u64, u64)> {
        payloads
            .iter()
            .map(|payload| {
                let bounds = parse_list_payload(payload).unwrap();
                let min: u64 = String::from_utf8(bounds[0].clone()).unwrap().parse().unwrap();
                // The upper bound is exclusive, so it is spelled "(N".
                let max: u64 = String::from_utf8(bounds[1][1..].to_vec())
                    .unwrap()
                    .parse()
                    .unwrap();
                (min, max)
            })
            .collect()
    }

    fn covered(ranges: &[(u64, u64)], score: u64) -> bool {
        ranges.iter().any(|(min, max)| score >= *min && score < *max)
    }

    fn resp_command(args: &[&str]) -> Vec<u8> {
        let mut out = format!("*{}\r\n", args.len()).into_bytes();
        for arg in args {
            out.extend_from_slice(format!("${}\r\n{arg}\r\n", arg.len()).as_bytes());
        }
        out
    }

    fn geo_search_payloads(args: &[&str]) -> Vec<(u64, u64)> {
        let cmd = parse_one(&resp_command(args));
        decode_range_payloads(&build_txn_ops(std::slice::from_ref(&cmd)).2)
    }

    #[test]
    fn geohash_encoding_matches_redis() {
        // Scores are Redis's interleaved 52-bit geohash, exactly.
        assert_eq!(
            geo_score_for_position(PALERMO.0, PALERMO.1),
            Some(PALERMO.2)
        );
        assert_eq!(
            geo_score_for_position(CATANIA.0, CATANIA.1),
            Some(CATANIA.2)
        );

        // Decoding lands on the center of the box, within the tolerance the
        // Redis documentation itself quotes for GEOPOS.
        let (lon, lat) = geo_decode_score(PALERMO.2);
        assert!((lon - PALERMO.0).abs() < 1e-4, "lon {lon}");
        assert!((lat - PALERMO.1).abs() < 1e-4, "lat {lat}");

        // The 11-character base32 strings from the GEOHASH documentation.
        assert_eq!(geo_hash_string(PALERMO.2).as_deref(), Some("sqc8b49rny0"));
        assert_eq!(geo_hash_string(CATANIA.2).as_deref(), Some("sqdtr74hyu0"));

        // Outside the Mercator cut-off there is no score at all.
        assert_eq!(geo_score_for_position(200.0, 100.0), None);
    }

    #[test]
    fn geo_distance_matches_the_documented_example() {
        let (plon, plat) = geo_decode_score(PALERMO.2);
        let (clon, clat) = geo_decode_score(CATANIA.2);
        let meters = geo_distance(plon, plat, clon, clat);
        assert_eq!(geo_format_distance(meters), "166274.1516");
        assert_eq!(geo_format_distance(meters / 1000.0), "166.2742");
        assert_eq!(geo_format_distance(meters / 1609.34), "103.3182");

        // Units, as Redis's extractUnitOrReply defines them, matched the way
        // it matches them: strcasecmp, so the upper-case spellings its own
        // error text names are accepted too.
        assert_eq!(geo_unit_meters(b"m"), Some(1.0));
        assert_eq!(geo_unit_meters(b"KM"), Some(1000.0));
        assert_eq!(geo_unit_meters(b"Km"), Some(1000.0));
        assert_eq!(geo_unit_meters(b"ft"), Some(0.3048));
        assert_eq!(geo_unit_meters(b"mi"), Some(1609.34));
        assert_eq!(geo_unit_meters(b"yards"), None);
    }

    #[test]
    fn geoadd_builds_one_zadd_op_with_geohash_scores() {
        let cmd = parse_one(
            b"*8\r\n$6\r\nGEOADD\r\n$6\r\nSicily\r\n$9\r\n13.361389\r\n$9\r\n38.115556\r\n\
$7\r\nPalermo\r\n$9\r\n15.087269\r\n$9\r\n37.502669\r\n$7\r\nCatania\r\n",
        );
        assert!(cmd.op == OpCode::GeoAdd);
        assert_eq!(cmd.keys, vec![Bytes::from_static(b"Sicily")]);
        let (ops, spans, payloads) = build_txn_ops(std::slice::from_ref(&cmd));
        assert_eq!(ops.len(), 1);
        assert_eq!(ops[0].op, TXN_OP_ZADD);
        assert_eq!(spans, vec![(0, 1)]);
        assert_eq!(
            parse_list_payload(&payloads[0]).unwrap(),
            vec![
                PALERMO.2.to_string().into_bytes(),
                b"Palermo".to_vec(),
                CATANIA.2.to_string().into_bytes(),
                b"Catania".to_vec(),
            ]
        );

        // NX/XX/CH ride along as ZADD flags.
        let cmd = parse_one(
            b"*7\r\n$6\r\nGEOADD\r\n$1\r\nk\r\n$2\r\nNX\r\n$2\r\nCH\r\n$1\r\n1\r\n$1\r\n2\r\n$1\r\nm\r\n",
        );
        assert_eq!(cmd.expire_flags & TXN_FLAG_ZADD_NX, TXN_FLAG_ZADD_NX);
        assert_eq!(cmd.expire_flags & TXN_FLAG_ZADD_CH, TXN_FLAG_ZADD_CH);

        // GEOADD is the only geo writer; every geo command retries on abort.
        assert!(is_dirty_command(OpCode::GeoAdd));
        assert!(!is_dirty_command(OpCode::GeoSearch));
        assert!(command_needs_retry(&cmd));
    }

    #[test]
    fn geopos_geohash_and_geodist_build_one_zscore_per_member() {
        let cmd = parse_one(
            b"*4\r\n$6\r\nGEOPOS\r\n$6\r\nSicily\r\n$7\r\nPalermo\r\n$11\r\nNonExisting\r\n",
        );
        assert!(cmd.op == OpCode::GeoPos);
        let (ops, spans, _) = build_txn_ops(std::slice::from_ref(&cmd));
        assert_eq!(ops.len(), 2);
        assert!(ops.iter().all(|op| op.op == TXN_OP_ZSCORE));
        assert_eq!(spans, vec![(0, 2)]);

        let cmd = parse_one(b"*3\r\n$7\r\nGEOHASH\r\n$6\r\nSicily\r\n$7\r\nPalermo\r\n");
        assert!(cmd.op == OpCode::GeoHash);
        assert_eq!(build_txn_ops(std::slice::from_ref(&cmd)).0.len(), 1);

        // GEODIST keeps the unit in cmd.val so the reply can convert back.
        let cmd = parse_one(
            b"*5\r\n$7\r\nGEODIST\r\n$6\r\nSicily\r\n$7\r\nPalermo\r\n$7\r\nCatania\r\n$2\r\nkm\r\n",
        );
        assert!(cmd.op == OpCode::GeoDist);
        assert_eq!(cmd.val.as_deref(), Some(&b"km"[..]));
        let (ops, spans, _) = build_txn_ops(std::slice::from_ref(&cmd));
        assert_eq!(ops.len(), 2);
        assert_eq!(spans, vec![(0, 2)]);
    }

    #[test]
    fn geo_searches_build_one_by_score_range_per_neighbor_box() {
        // GEORADIUS Sicily 15 37 200 km: the center cell plus the neighbors
        // that can still intersect, all read in one request.
        let cmd = parse_one(
            b"*7\r\n$9\r\nGEORADIUS\r\n$6\r\nSicily\r\n$2\r\n15\r\n$2\r\n37\r\n$3\r\n200\r\n\
$2\r\nkm\r\n$8\r\nWITHDIST\r\n",
        );
        assert!(cmd.op == OpCode::GeoSearch);
        let spec = cmd.geo.as_deref().unwrap();
        assert!(spec.circular);
        assert_eq!(spec.radius_m, 200_000.0);
        assert!(spec.withdist && !spec.withcoord && !spec.withhash);
        let (ops, spans, payloads) = build_txn_ops(std::slice::from_ref(&cmd));
        assert!(!ops.is_empty() && ops.len() <= GEO_MAX_SEARCH_RANGES);
        assert_eq!(spans, vec![(0, ops.len())]);
        assert!(ops
            .iter()
            .all(|op| op.op == TXN_OP_ZRANGE
                && op.flags == TXN_FLAG_Z_BYSCORE | TXN_FLAG_Z_WITHSCORES));

        // Both documented points fall inside one of the requested ranges.
        let ranges = decode_range_payloads(&payloads);
        for score in [PALERMO.2, CATANIA.2] {
            assert!(
                covered(&ranges, score),
                "score {score} is in no searched box"
            );
        }

        // GEOSEARCH BYBOX and the option tail parse into the same spec.
        let cmd = parse_one(
            b"*12\r\n$9\r\nGEOSEARCH\r\n$6\r\nSicily\r\n$10\r\nFROMLONLAT\r\n$2\r\n15\r\n\
$2\r\n37\r\n$5\r\nBYBOX\r\n$3\r\n400\r\n$3\r\n400\r\n$2\r\nkm\r\n$3\r\nASC\r\n\
$9\r\nWITHCOORD\r\n$8\r\nWITHDIST\r\n",
        );
        let spec = cmd.geo.as_deref().unwrap();
        assert!(!spec.circular);
        assert_eq!(spec.width_m, 400_000.0);
        assert_eq!(spec.height_m, 400_000.0);
        assert!(spec.sort == GeoSort::Asc);
        assert!(spec.withcoord && spec.withdist);

        // FROMMEMBER leaves the center unresolved; the lookup is its own
        // request, so no range reads are built until it has run.
        let cmd = parse_one(
            b"*7\r\n$9\r\nGEOSEARCH\r\n$6\r\nSicily\r\n$10\r\nFROMMEMBER\r\n$7\r\nPalermo\r\n\
$8\r\nBYRADIUS\r\n$3\r\n200\r\n$2\r\nkm\r\n",
        );
        let spec = cmd.geo.as_deref().unwrap();
        assert_eq!(spec.from_member.as_deref(), Some(&b"Palermo"[..]));
        assert_eq!(build_txn_ops(std::slice::from_ref(&cmd)).0.len(), 0);

        // COUNT n ANY and DESC are carried too.
        let cmd = parse_one(
            b"*10\r\n$17\r\nGEORADIUSBYMEMBER\r\n$6\r\nSicily\r\n$9\r\nAgrigento\r\n$3\r\n100\r\n\
$2\r\nkm\r\n$5\r\nCOUNT\r\n$1\r\n1\r\n$3\r\nANY\r\n$4\r\nDESC\r\n$8\r\nWITHHASH\r\n",
        );
        let spec = cmd.geo.as_deref().unwrap();
        assert_eq!(spec.count, Some(1));
        assert!(spec.any);
        assert!(spec.sort == GeoSort::Desc);
        assert!(spec.withhash);
    }

    #[test]
    fn geo_searches_near_the_pole_still_cover_their_targets() {
        // The three regressions from Redis tests/unit/geo.tcl that nine fixed
        // neighbor boxes miss: near the pole a degree of longitude is short
        // enough that the matching cell is two columns away, and a search area
        // that runs over a pole reaches every column.
        let target = |lon: f64, lat: f64| geo_score_for_position(lon, lat).unwrap();

        // "search areas contain satisfied points in oblique direction".
        let ranges = geo_search_payloads(&[
            "GEORADIUS",
            "k1",
            "-0.15307903289794921875",
            "85",
            "4891.94",
            "m",
        ]);
        assert!(covered(&ranges, target(0.3515625, 85.00019260486917)));

        let ranges = geo_search_payloads(&[
            "GEORADIUS",
            "k1",
            "-4.95211958885192871094",
            "85",
            "156544",
            "m",
        ]);
        assert!(covered(&ranges, target(11.25, 85.0511)));

        // "crossing pole search": the area wraps over the north pole, so the
        // target sits half a world away in longitude.
        let ranges = geo_search_payloads(&["GEORADIUS", "k1", "45", "65", "5009431", "m"]);
        assert!(covered(&ranges, target(-135.0, 85.05)));
        assert!(ranges.len() <= GEO_MAX_SEARCH_RANGES);

        // A search large enough to need more boxes than the cap reads the whole
        // score space once, which is cheaper here than many range ops.
        let ranges = geo_search_payloads(&["GEORADIUS", "k1", "0", "0", "20000000", "km"]);
        assert!(ranges.len() <= GEO_MAX_SEARCH_RANGES);
        assert!(covered(&ranges, target(-135.0, 85.05)));
        assert!(covered(&ranges, target(179.0, -85.0)));
    }

    #[test]
    fn geo_commands_report_redis_error_texts() {
        assert_eq!(
            run_raw(
                b"*6\r\n$6\r\nGEOADD\r\n$1\r\nk\r\n$3\r\n200\r\n$3\r\n100\r\n$1\r\nm\r\n$1\r\nx\r\n"
            ),
            b"-ERR syntax error\r\n"
        );
        assert_eq!(
            run_raw(b"*5\r\n$6\r\nGEOADD\r\n$1\r\nk\r\n$3\r\n200\r\n$3\r\n100\r\n$1\r\nm\r\n"),
            b"-ERR invalid longitude,latitude pair 200.000000,100.000000\r\n"
        );
        assert_eq!(
            run_raw(
                b"*5\r\n$7\r\nGEODIST\r\n$1\r\nk\r\n$1\r\na\r\n$1\r\nb\r\n$5\r\nyards\r\n"
            ),
            b"-ERR unsupported unit provided. please use M, KM, FT, MI\r\n"
        );
        assert_eq!(
            run_raw(
                b"*8\r\n$9\r\nGEORADIUS\r\n$1\r\nk\r\n$1\r\n1\r\n$1\r\n2\r\n$2\r\n-1\r\n$2\r\nkm\r\n$3\r\nASC\r\n$4\r\nDESC\r\n"
            ),
            b"-ERR radius cannot be negative\r\n"
        );
        // STORE is accepted now; the refusal is only for the _RO spellings
        // and for combining a destination with the output columns.
        assert_eq!(
            run_raw(
                b"*9\r\n$12\r\nGEORADIUS_RO\r\n$1\r\nk\r\n$1\r\n1\r\n$1\r\n2\r\n$1\r\n5\r\n$2\r\nkm\r\n$5\r\nSTORE\r\n$3\r\ndst\r\n$3\r\nASC\r\n"
            ),
            b"-ERR syntax error\r\n"
        );
        assert_eq!(
            run_raw(
                b"*9\r\n$9\r\nGEORADIUS\r\n$1\r\nk\r\n$1\r\n1\r\n$1\r\n2\r\n$1\r\n5\r\n$2\r\nkm\r\n$5\r\nSTORE\r\n$3\r\ndst\r\n$8\r\nWITHDIST\r\n"
            ),
            b"-ERR STORE option in GEORADIUS is not compatible with WITHDIST, WITHHASH and WITHCOORD options\r\n"
        );
        assert_eq!(
            run_raw(
                b"*8\r\n$9\r\nGEORADIUS\r\n$1\r\nk\r\n$1\r\n1\r\n$1\r\n2\r\n$1\r\n5\r\n$2\r\nkm\r\n$5\r\nCOUNT\r\n$1\r\n0\r\n"
            ),
            b"-ERR COUNT must be > 0\r\n"
        );
        // GEOSEARCH needs both a center and a shape.
        assert_eq!(
            run_raw(
                b"*7\r\n$9\r\nGEOSEARCH\r\n$1\r\nk\r\n$10\r\nFROMLONLAT\r\n$1\r\n1\r\n$1\r\n2\r\n$3\r\nASC\r\n$8\r\nWITHDIST\r\n"
            ),
            b"-ERR exactly one of BYRADIUS and BYBOX can be specified for GEOSEARCH\r\n"
        );
        assert_eq!(
            run_raw(b"*2\r\n$6\r\nGEOADD\r\n$1\r\nk\r\n"),
            b"-ERR wrong number of arguments for 'geoadd' command\r\n"
        );
        assert_eq!(
            run_raw(b"*2\r\n$20\r\nGEORADIUSBYMEMBER_RO\r\n$1\r\nk\r\n"),
            b"-ERR wrong number of arguments for 'georadiusbymember_ro' command\r\n"
        );
        assert!(parse_opcode(b"GEOSEARCHSTORE") == Some(OpCode::GeoSearchStore));
    }

    /// GEOSEARCHSTORE and the two STORE spellings of GEORADIUS all become one
    /// opcode whose key is the destination and whose `values[0]` is the source,
    /// the way ZRANGESTORE spells it, and they build no ops: the command runs
    /// on the session path because its write depends on its own reads.
    #[test]
    fn geo_store_searches_name_the_destination_first_and_build_no_ops() {
        let cmd = parse_one(
            b"*12\r\n$14\r\nGEOSEARCHSTORE\r\n$4\r\ndest\r\n$3\r\nsrc\r\n\
$10\r\nFROMLONLAT\r\n$2\r\n15\r\n$2\r\n37\r\n$5\r\nBYBOX\r\n$3\r\n400\r\n\
$3\r\n400\r\n$2\r\nkm\r\n$3\r\nASC\r\n$9\r\nSTOREDIST\r\n",
        );
        assert!(cmd.op == OpCode::GeoSearchStore);
        assert_eq!(cmd.keys, vec![Bytes::from_static(b"dest")]);
        assert_eq!(cmd.values, vec![Bytes::from_static(b"src")]);
        let spec = cmd.geo.as_deref().expect("a search spec");
        assert!(spec.store_dist);
        assert!(spec.sort == GeoSort::Asc);
        assert!(!spec.circular);
        assert!(spec.from_member.is_none());
        // Nothing to hand the batch executor: the session runs this one.
        let (ops, spans, _payloads) = build_txn_ops(std::slice::from_ref(&cmd));
        assert!(ops.is_empty());
        assert_eq!(spans, vec![(0, 0)]);
        assert!(is_dirty_command(cmd.op));

        // GEORADIUS names the destination after the option instead, and STORE
        // stores the geohash where STOREDIST stores the distance.
        let cmd = parse_one(
            b"*9\r\n$9\r\nGEORADIUS\r\n$3\r\nsrc\r\n$2\r\n15\r\n$2\r\n37\r\n\
$3\r\n200\r\n$2\r\nkm\r\n$5\r\nSTORE\r\n$4\r\ndest\r\n$3\r\nASC\r\n",
        );
        assert!(cmd.op == OpCode::GeoSearchStore);
        assert_eq!(cmd.keys, vec![Bytes::from_static(b"dest")]);
        assert_eq!(cmd.values, vec![Bytes::from_static(b"src")]);
        assert!(!cmd.geo.as_deref().expect("a search spec").store_dist);

        let cmd = parse_one(
            b"*7\r\n$17\r\nGEORADIUSBYMEMBER\r\n$3\r\nsrc\r\n$6\r\nmember\r\n\
$3\r\n200\r\n$2\r\nkm\r\n$9\r\nSTOREDIST\r\n$4\r\ndest\r\n",
        );
        assert!(cmd.op == OpCode::GeoSearchStore);
        let spec = cmd.geo.as_deref().expect("a search spec");
        assert!(spec.store_dist);
        assert_eq!(spec.from_member, Some(Bytes::from_static(b"member")));

        // Both keys carry the logical database, destination and source alike.
        let cmd = parsed(
            1,
            &[
                b"GEOSEARCHSTORE",
                b"dest",
                b"src",
                b"FROMMEMBER",
                b"m",
                b"BYRADIUS",
                b"5",
                b"km",
            ],
        );
        assert_eq!(cmd.keys, vec![Bytes::from_static(b"\x02\x01:dest")]);
        assert_eq!(cmd.values, vec![Bytes::from_static(b"\x02\x01:src")]);
    }

    #[test]
    fn geo_store_searches_report_redis_error_texts() {
        // The output columns have nowhere to go in a sorted set.
        assert_eq!(
            parse_refusal(
                0,
                &[
                    b"GEOSEARCHSTORE",
                    b"dest",
                    b"src",
                    b"FROMLONLAT",
                    b"15",
                    b"37",
                    b"BYRADIUS",
                    b"5",
                    b"km",
                    b"WITHCOORD",
                ],
            ),
            "-ERR syntax error"
        );
        // GEOSEARCHSTORE already has its destination, so STORE is a stray token.
        assert_eq!(
            parse_refusal(
                0,
                &[
                    b"GEOSEARCHSTORE",
                    b"dest",
                    b"src",
                    b"FROMLONLAT",
                    b"15",
                    b"37",
                    b"BYRADIUS",
                    b"5",
                    b"km",
                    b"STORE",
                    b"dest",
                ],
            ),
            "-ERR syntax error"
        );
        // GEOSEARCH itself never stores, so a bare STOREDIST is a syntax error.
        assert_eq!(
            parse_refusal(
                0,
                &[
                    b"GEOSEARCH",
                    b"src",
                    b"FROMLONLAT",
                    b"15",
                    b"37",
                    b"BYRADIUS",
                    b"5",
                    b"km",
                    b"STOREDIST",
                ],
            ),
            "-ERR syntax error"
        );
        assert_eq!(
            parse_refusal(
                0,
                &[
                    b"GEOSEARCHSTORE",
                    b"dest",
                    b"src",
                    b"FROMLONLAT",
                    b"15",
                    b"37",
                    b"BYRADIUS",
                    b"5",
                    b"km",
                    b"ANY",
                ],
            ),
            "-ERR the ANY argument requires COUNT argument"
        );
        assert_eq!(
            parse_refusal(
                0,
                &[
                    b"GEOSEARCHSTORE",
                    b"dest",
                    b"src",
                    b"FROMLONLAT",
                    b"15",
                    b"37",
                    b"FROMMEMBER",
                    b"m",
                    b"BYRADIUS",
                    b"5",
                    b"km",
                ],
            ),
            "-ERR syntax error"
        );
        assert_eq!(
            parse_refusal(
                0,
                &[
                    b"GEOSEARCHSTORE",
                    b"dest",
                    b"src",
                    b"FROMLONLAT",
                    b"15",
                    b"37",
                    b"ASC",
                ],
            ),
            "-ERR exactly one of BYRADIUS and BYBOX can be specified for GEOSEARCH"
        );
        assert_eq!(
            parse_refusal(0, &[b"GEOSEARCHSTORE", b"dest", b"src"]),
            "-ERR wrong number of arguments for 'geosearchstore' command"
        );
        // A second destination is a syntax error, not a silent replacement.
        assert_eq!(
            parse_refusal(
                0,
                &[
                    b"GEORADIUS",
                    b"src",
                    b"15",
                    b"37",
                    b"200",
                    b"km",
                    b"STORE",
                    b"one",
                    b"STOREDIST",
                    b"two",
                ],
            ),
            "-ERR syntax error"
        );
    }

    /// MODULE answers locally. Nothing is emulated: there is no module ABI
    /// below this adapter, so LIST is truthfully empty and the loading forms
    /// give Redis's own failure texts.
    #[test]
    fn module_reports_no_modules_and_refuses_to_load_one() {
        assert!(parse_opcode(b"MODULE") == Some(OpCode::Module));
        assert_eq!(run_raw(b"*2\r\n$6\r\nMODULE\r\n$4\r\nLIST\r\n"), b"*0\r\n");
        assert_eq!(
            run_raw(b"*3\r\n$6\r\nMODULE\r\n$4\r\nLOAD\r\n$9\r\n/tmp/x.so\r\n"),
            b"-ERR Error loading the extension. Please check the server logs.\r\n"
        );
        assert_eq!(
            run_raw(
                b"*5\r\n$6\r\nMODULE\r\n$4\r\nLOAD\r\n$9\r\n/tmp/x.so\r\n$1\r\na\r\n$1\r\nb\r\n"
            ),
            b"-ERR Error loading the extension. Please check the server logs.\r\n"
        );
        assert_eq!(
            run_raw(
                b"*7\r\n$6\r\nMODULE\r\n$6\r\nLOADEX\r\n$9\r\n/tmp/x.so\r\n$6\r\nCONFIG\r\n\
$1\r\nk\r\n$1\r\nv\r\n$4\r\nARGS\r\n"
            ),
            b"-ERR Error loading the extension. Please check the server logs.\r\n"
        );
        assert_eq!(
            run_raw(b"*3\r\n$6\r\nMODULE\r\n$6\r\nUNLOAD\r\n$2\r\nmy\r\n"),
            b"-ERR Error unloading module: no such module with that name\r\n"
        );
        // The help is Redis's list inside addReplyHelp's header and footer.
        let help = run_raw(b"*2\r\n$6\r\nMODULE\r\n$4\r\nHELP\r\n");
        assert!(help.starts_with(b"*11\r\n"));
        assert!(help
            .windows(MODULE_HELP[0].len())
            .any(|window| window == MODULE_HELP[0].as_bytes()));
        assert!(help.ends_with(b"$20\r\n    Print this help.\r\n"));
        // An unknown subcommand, and a known one with the wrong arity, both
        // take the shape the other container commands use.
        assert_eq!(
            run_raw(b"*2\r\n$6\r\nMODULE\r\n$7\r\nnosuchx\r\n"),
            b"-ERR unknown subcommand 'nosuchx'. Try MODULE HELP.\r\n"
        );
        assert_eq!(
            run_raw(b"*2\r\n$6\r\nMODULE\r\n$6\r\nUNLOAD\r\n"),
            b"-ERR unknown subcommand 'UNLOAD'. Try MODULE HELP.\r\n"
        );
        assert_eq!(
            run_raw(b"*1\r\n$6\r\nMODULE\r\n"),
            b"-ERR wrong number of arguments for 'module' command\r\n"
        );
    }

    #[test]
    fn hash_field_expire_builds_one_op_with_mode_and_fields() {
        // HEXPIRE key 10 FIELDS 2 f1 f2: no condition, so the mode is NONE and
        // the relative seconds become absolute milliseconds.
        let before = unix_time_ms();
        let cmd = parse_one(
            b"*7\r\n$7\r\nHEXPIRE\r\n$1\r\nh\r\n$2\r\n10\r\n$6\r\nFIELDS\r\n$1\r\n2\r\n\
$2\r\nf1\r\n$2\r\nf2\r\n",
        );
        assert!(cmd.op == OpCode::HExpire);
        assert_eq!(cmd.keys, vec![Bytes::from_static(b"h")]);
        assert!(cmd.expire_at_ms >= before + 10_000);
        assert!(cmd.expire_at_ms <= unix_time_ms() + 10_000);
        let (ops, spans, payloads) = build_txn_ops(std::slice::from_ref(&cmd));
        assert_eq!(ops.len(), 1);
        assert_eq!(ops[0].op, TXN_OP_HFIELD_EXPIRE);
        assert_eq!(ops[0].expire_at_ms, cmd.expire_at_ms);
        assert_eq!(spans, vec![(0, 1)]);
        assert_eq!(
            parse_list_payload(&payloads[0]).unwrap(),
            vec![b"NONE".to_vec(), b"f1".to_vec(), b"f2".to_vec()]
        );

        // A condition shifts FIELDS by one and travels as the first payload
        // item. HPEXPIREAT takes the time as it stands.
        let cmd = parse_one(
            b"*7\r\n$10\r\nHPEXPIREAT\r\n$1\r\nh\r\n$4\r\n5000\r\n$2\r\nlt\r\n$6\r\nFIELDS\r\n\
$1\r\n1\r\n$2\r\nf1\r\n",
        );
        assert!(cmd.op == OpCode::HPExpireAt);
        assert_eq!(cmd.expire_at_ms, 5000);
        assert_eq!(
            parse_list_payload(&build_txn_ops(std::slice::from_ref(&cmd)).2[0]).unwrap(),
            vec![b"LT".to_vec(), b"f1".to_vec()]
        );

        // HTTL and HPERSIST carry only the fields, and HTTL is a read.
        let httl = parse_one(
            b"*5\r\n$4\r\nHTTL\r\n$1\r\nh\r\n$6\r\nFIELDS\r\n$1\r\n1\r\n$2\r\nf1\r\n",
        );
        let (ops, _, payloads) = build_txn_ops(std::slice::from_ref(&httl));
        assert_eq!(ops[0].op, TXN_OP_HFIELD_TTL);
        assert_eq!(parse_list_payload(&payloads[0]).unwrap(), vec![b"f1".to_vec()]);
        assert!(!is_dirty_command(OpCode::HTtl));
        assert!(command_needs_retry(&httl));

        let hpersist = parse_one(
            b"*5\r\n$8\r\nHPERSIST\r\n$1\r\nh\r\n$6\r\nFIELDS\r\n$1\r\n1\r\n$2\r\nf1\r\n",
        );
        assert_eq!(
            build_txn_ops(std::slice::from_ref(&hpersist)).0[0].op,
            TXN_OP_HFIELD_PERSIST
        );
        assert!(is_dirty_command(OpCode::HPersist));
        assert!(is_dirty_command(OpCode::HExpire));
        assert!(command_needs_retry(&hpersist));
    }

    #[test]
    fn hash_field_expire_rejects_malformed_arguments() {
        assert_eq!(
            run_raw(b"*5\r\n$7\r\nHEXPIRE\r\n$1\r\nh\r\n$2\r\n10\r\n$6\r\nFIELDS\r\n$1\r\n1\r\n"),
            b"-ERR wrong number of arguments for 'hexpire' command\r\n"
        );
        assert_eq!(
            run_raw(b"*4\r\n$8\r\nHPERSIST\r\n$1\r\nh\r\n$6\r\nFIELDS\r\n$1\r\n1\r\n"),
            b"-ERR wrong number of arguments for 'hpersist' command\r\n"
        );
        assert_eq!(
            run_raw(
                b"*6\r\n$7\r\nHEXPIRE\r\n$1\r\nh\r\n$2\r\n10\r\n$6\r\nFIELDS\r\n$1\r\n0\r\n$2\r\nf1\r\n"
            ),
            "-ERR Parameter `numFields` should be greater than 0\r\n".as_bytes()
        );
        assert_eq!(
            run_raw(
                b"*6\r\n$7\r\nHEXPIRE\r\n$1\r\nh\r\n$2\r\n10\r\n$6\r\nFIELDS\r\n$1\r\n2\r\n$2\r\nf1\r\n"
            ),
            "-ERR The `numfields` parameter must match the number of arguments\r\n".as_bytes()
        );
        assert_eq!(
            run_raw(
                b"*6\r\n$7\r\nHEXPIRE\r\n$1\r\nh\r\n$2\r\n10\r\n$5\r\nWRONG\r\n$1\r\n1\r\n$2\r\nf1\r\n"
            ),
            b"-ERR Mandatory argument FIELDS is missing or not at the right position\r\n"
        );
        assert_eq!(
            run_raw(
                b"*6\r\n$7\r\nHEXPIRE\r\n$1\r\nh\r\n$2\r\n-1\r\n$6\r\nFIELDS\r\n$1\r\n1\r\n$2\r\nf1\r\n"
            ),
            b"-ERR invalid expire time, must be >= 0\r\n"
        );
        // Past Redis's 46-bit ceiling on the absolute expiration time.
        let huge = (1i64 << 48) / 1000;
        let frame = format!(
            "*6\r\n$7\r\nHEXPIRE\r\n$1\r\nh\r\n${}\r\n{}\r\n$6\r\nFIELDS\r\n$1\r\n1\r\n$2\r\nf1\r\n",
            huge.to_string().len(),
            huge
        );
        assert_eq!(
            run_raw(frame.as_bytes()),
            b"-ERR invalid expire time in 'hexpire' command\r\n"
        );
        // Two conditions that cannot be combined.
        assert_eq!(
            run_raw(
                b"*8\r\n$7\r\nHEXPIRE\r\n$1\r\nh\r\n$2\r\n10\r\n$2\r\nNX\r\n$2\r\nXX\r\n\
$6\r\nFIELDS\r\n$1\r\n1\r\n$2\r\nf1\r\n"
            ),
            b"-ERR NX and XX, GT or LT options at the same time are not compatible\r\n"
        );
        assert_eq!(
            run_raw(
                b"*8\r\n$7\r\nHEXPIRE\r\n$1\r\nh\r\n$2\r\n10\r\n$2\r\nGT\r\n$2\r\nLT\r\n\
$6\r\nFIELDS\r\n$1\r\n1\r\n$2\r\nf1\r\n"
            ),
            b"-ERR GT and LT options at the same time are not compatible\r\n"
        );
    }

    #[test]
    fn wrong_arity_reports_command_name() {
        let out = run_raw(b"*1\r\n$3\r\nGET\r\n");

        assert_eq!(out, b"-ERR wrong number of arguments for 'get' command\r\n");
    }

    #[test]
    fn non_array_frame_reports_protocol_error() {
        let out = run_raw(b"+PING\r\n");

        assert_eq!(out, b"-ERR protocol error: expected array\r\n");
    }

    #[test]
    fn every_write_command_retries_on_an_occ_abort() {
        // A storage-backed command that is not in the retry class turns a
        // single OCC abort into "ERR backend" for the client. These five were
        // missing, which is how RENAME onto an existing key of the same type
        // failed whenever the destination record had to grow in place.
        for op in [
            OpCode::Rename,
            OpCode::RenameNx,
            OpCode::Sort,
            OpCode::LMPop,
            OpCode::BLMove,
        ] {
            assert!(
                command_needs_retry(&command(op, &[])),
                "a storage-backed command would not retry an OCC abort (opcode {})",
                op as u32
            );
        }
    }

    // ===== Logical databases (SELECT, MOVE, COPY ... DB) =====

    /// Parse one RESP command exactly as a worker would, in `db`.
    fn parse_in_db(db: u8, args: &[&[u8]]) -> Result<Command, ParseError> {
        let mut frame = format!("*{}\r\n", args.len()).into_bytes();
        for arg in args {
            frame.extend_from_slice(format!("${}\r\n", arg.len()).as_bytes());
            frame.extend_from_slice(arg);
            frame.extend_from_slice(b"\r\n");
        }
        let mut resp3 = Resp3Handler::new(1024);
        resp3.read_bytes(&frame);
        let decoded = resp3.next_frame().unwrap().unwrap();
        set_current_db(db);
        let parsed = parse_resp3(decoded);
        set_current_db(0);
        parsed
    }

    /// A SCAN/KEYS reply payload, in the executor's wire shape.
    fn scan_payload(cursor: Option<&[u8]>, keys: &[&[u8]]) -> Vec<u8> {
        let mut out = Vec::new();
        let cursor = cursor.unwrap_or(b"");
        out.extend_from_slice(&(cursor.len() as u64).to_le_bytes());
        out.extend_from_slice(cursor);
        out.extend_from_slice(&(keys.len() as u64).to_le_bytes());
        for key in keys {
            out.extend_from_slice(&(key.len() as u64).to_le_bytes());
            out.extend_from_slice(key);
        }
        out
    }

    /// The parsed command, or a panic naming the error. `Command` and
    /// `ParseError` are deliberately not `Debug`, so `unwrap` is not available.
    fn parsed(db: u8, args: &[&[u8]]) -> Command {
        match parse_in_db(db, args) {
            Ok(cmd) => cmd,
            Err(err) => panic!("expected a command, got {}", parse_error_text(err)),
        }
    }

    /// The RESP error text a refused parse produces.
    fn parse_refusal(db: u8, args: &[&[u8]]) -> String {
        match parse_in_db(db, args) {
            Ok(_) => panic!("expected a parse error"),
            Err(err) => parse_error_text(err),
        }
    }

    fn parse_error_text(err: ParseError) -> String {
        let mut out = Vec::new();
        write_parse_error(&mut out, err).unwrap();
        String::from_utf8_lossy(&out).trim_end().to_string()
    }

    /// Every Redis-visible key has to acquire the logical-database prefix, and
    /// the validator is the one place that applies it, so a call site that
    /// drops the key it returns silently gives one database another's data.
    /// The signature catches a site that ignores the result in most shapes;
    /// this catches the rest, and the count makes a newly added key path fail
    /// until someone has looked at it.
    #[test]
    fn every_user_key_call_site_keeps_the_prefixed_key() {
        let needle = format!("{}(", "validate_user_key");
        let definition = format!("fn {}", needle);
        // The four sites that cannot simply shadow their own name: SORT's
        // STORE destination is declared before the option loop, and COPY and
        // MOVE keep the raw name so they can re-spell it for another database.
        let rebinding: &[(&str, &str)] = &[
            ("store", "part_to_bytes(&parts[index + 1])?"),
            ("source", "raw_source"),
            ("mut destination", "raw_destination"),
            ("source", "raw_key"),
        ];
        let mut sites = 0;
        for line in include_str!("lib.rs").lines() {
            let line = line.trim();
            if !line.contains(&needle) || line.starts_with(&definition) {
                continue;
            }
            // Skip this test's own text.
            if line.starts_with("//") || line.starts_with("let needle") {
                continue;
            }
            sites += 1;
            let bind = format!(" = {}&", needle);
            let Some((left, right)) = line.split_once(&bind) else {
                panic!("key call site does not bind the key it returns: {line}");
            };
            let bound = left.strip_prefix("let ").unwrap_or(left).trim();
            let argument = right.strip_suffix(")?;").unwrap_or(right);
            assert!(
                bound == argument || rebinding.contains(&(bound, argument)),
                "key call site binds {bound} but reads {argument}: {line}"
            );
        }
        // 115: the 113 above plus GEOSEARCHSTORE's destination and the
        // destination GEORADIUS / GEORADIUSBYMEMBER name after STORE or
        // STOREDIST. The source key reuses the existing geo call site.
        // 113: the 99 from package 8 plus one for each stream parse arm that
        // names a key -- XADD, XRANGE/XREVRANGE, XLEN, XDEL, XTRIM, XSETID,
        // XINFO STREAM, XINFO GROUPS/CONSUMERS, the shared XREAD/XREADGROUP
        // key list, XGROUP, XACK, XPENDING, XCLAIM and XAUTOCLAIM. XINFO HELP
        // and XGROUP HELP name none.
        assert_eq!(
            sites, 115,
            "the number of Redis-visible key call sites changed; audit the new one"
        );
    }

    #[test]
    fn database_zero_keys_are_stored_under_exactly_the_bytes_the_client_sent() {
        let cmd = parsed(0, &[b"SET", b"key", b"value"]);
        assert_eq!(cmd.keys, vec![Bytes::from_static(b"key")]);
        let cmd = parsed(1, &[b"SET", b"key", b"value"]);
        assert_eq!(cmd.keys, vec![Bytes::from_static(b"\x02\x01:key")]);
        let cmd = parsed(15, &[b"GET", b"key"]);
        assert_eq!(cmd.keys, vec![Bytes::from_static(b"\x02\x0f:key")]);
        // The commands that keep a raw argument list keep what the client
        // sent, unprefixed; that list is what MONITOR and CLUSTER KEYSLOT read.
        let cmd = parsed(9, &[b"EXISTS", b"key"]);
        assert_eq!(cmd.keys, vec![Bytes::from_static(b"\x02\x09:key")]);
        assert_eq!(cmd.args, vec![Bytes::from_static(b"key")]);
    }

    #[test]
    fn the_prefix_helper_round_trips_and_leaves_database_zero_alone() {
        let key = Bytes::from_static(b"k");
        assert_eq!(db_key(0, &key), key);
        assert_eq!(db_key(3, &key).as_ref(), b"\x02\x03:k");
        assert_eq!(strip_db_key(0, b"k"), b"k");
        assert_eq!(strip_db_key(3, b"\x02\x03:k"), b"k");
        // A name that does not carry this database's prefix is returned whole,
        // so a reply can never lose bytes it did not add.
        assert_eq!(strip_db_key(3, b"\x02\x04:k"), b"\x02\x04:k");
        assert_eq!(strip_db_key(3, b"k"), b"k");
        assert!(db_scan_prefix(0).is_empty());
        assert_eq!(db_scan_prefix(7).as_ref(), b"\x02\x07:");
        // FLUSHDB tells database 0 apart from "every database" with a one-byte
        // sentinel; see the executor's redis_flush_db_filter.
        assert_eq!(db_flush_prefix(0).as_ref(), b"\x02");
        assert_eq!(db_flush_prefix(7).as_ref(), b"\x02\x07:");
    }

    #[test]
    fn both_reserved_first_bytes_are_refused_in_every_database() {
        for db in [0u8, 1, 15] {
            for first in [0x01u8, 0x02] {
                let key = [first, b'x'];
                assert_eq!(
                    parse_refusal(db, &[b"SET", &key, b"v"]),
                    "-ERR invalid key: reserved internal prefix"
                );
            }
        }
        // The raw GET/SET shortcut has to refuse them too, by falling through
        // to the general parser that produces the error.
        for first in [0x01u8, 0x02] {
            let frame = format!(
                "*3\r\n$3\r\nSET\r\n$2\r\n{}x\r\n$1\r\nv\r\n",
                first as char
            );
            assert!(matches!(
                parse_raw_mako_string_command(frame.as_bytes()),
                RawMakoParse::NotFast
            ));
        }
    }

    #[test]
    fn select_moves_the_connection_and_reset_brings_it_back() {
        let mut txn_state = TransactionState::new();
        let mut client_state = ClientState::new();
        assert_eq!(client_state.db, 0);
        for (index, expected) in [("0", "+OK"), ("15", "+OK")] {
            let out = run(command(OpCode::Select, &[index.as_bytes()]), &mut txn_state, &mut client_state);
            assert!(String::from_utf8_lossy(&out).starts_with(expected));
        }
        assert_eq!(client_state.db, 15);
        assert_eq!(current_db(), 15);
        for index in ["16", "-1", "99"] {
            let out = run(command(OpCode::Select, &[index.as_bytes()]), &mut txn_state, &mut client_state);
            assert_eq!(out, b"-ERR DB index is out of range\r\n");
        }
        let out = run(command(OpCode::Select, &[b"abc"]), &mut txn_state, &mut client_state);
        assert_eq!(out, b"-ERR value is not an integer or out of range\r\n");
        assert_eq!(client_state.db, 15, "a refused SELECT leaves the connection where it was");
        let out = run(command(OpCode::Reset, &[]), &mut txn_state, &mut client_state);
        assert_eq!(out, b"+RESET\r\n");
        assert_eq!(client_state.db, 0);
        assert_eq!(current_db(), 0);
    }

    #[test]
    fn move_builds_one_op_carrying_the_destination_database_spelling() {
        let cmd = parsed(0, &[b"MOVE", b"k", b"3"]);
        assert!(cmd.op == OpCode::Move);
        assert_eq!(cmd.keys, vec![Bytes::from_static(b"k")]);
        assert_eq!(cmd.values, vec![Bytes::from_static(b"\x02\x03:k")]);
        // From database 2 the source carries its own prefix.
        let cmd = parsed(2, &[b"MOVE", b"k", b"3"]);
        assert_eq!(cmd.keys, vec![Bytes::from_static(b"\x02\x02:k")]);
        assert_eq!(cmd.values, vec![Bytes::from_static(b"\x02\x03:k")]);

        let (ops, spans, _payloads) = build_txn_ops(std::slice::from_ref(&cmd));
        assert_eq!(spans, vec![(0, 1)]);
        assert_eq!(ops.len(), 1);
        assert_eq!(ops[0].op, TXN_OP_MOVE);
        let packed = unsafe { std::slice::from_raw_parts(ops[0].val_ptr, ops[0].val_len) };
        assert_eq!(packed, pack_bytes_list(&cmd.values).as_ref());

        assert!(is_dirty_command(OpCode::Move));
        assert!(command_needs_retry(&cmd));

        assert_eq!(
            parse_refusal(0, &[b"MOVE", b"k", b"0"]),
            "-ERR source and destination objects are the same"
        );
        assert_eq!(
            parse_refusal(0, &[b"MOVE", b"k", b"16"]),
            "-ERR DB index is out of range"
        );
        assert_eq!(
            parse_refusal(0, &[b"MOVE", b"k"]),
            "-ERR wrong number of arguments for 'move' command"
        );
    }

    #[test]
    fn copy_db_option_spells_the_destination_for_that_database() {
        let cmd = parsed(0, &[b"COPY", b"a", b"b"]);
        assert_eq!(cmd.values, vec![Bytes::from_static(b"b")]);
        assert_eq!(cmd.expire_at_ms, 0);
        let cmd = parsed(0, &[b"COPY", b"a", b"b", b"DB", b"2", b"REPLACE"]);
        assert_eq!(cmd.keys, vec![Bytes::from_static(b"a")]);
        assert_eq!(cmd.values, vec![Bytes::from_static(b"\x02\x02:b")]);
        assert_eq!(cmd.expire_at_ms, 1);
        // Same name, same database: Redis refuses it.
        assert_eq!(
            parse_refusal(1, &[b"COPY", b"a", b"a", b"DB", b"1"]),
            "-ERR source and destination objects are the same"
        );
        assert_eq!(
            parse_refusal(0, &[b"COPY", b"a", b"b", b"DB", b"16"]),
            "-ERR DB index is out of range"
        );
    }

    #[test]
    fn keyspace_commands_carry_the_database_they_walk() {
        for (args, db, expected) in [
            (&[b"KEYS".as_ref(), b"user:*".as_ref()][..], 0u8, &b"user:"[..]),
            (&[b"KEYS".as_ref(), b"user:*".as_ref()][..], 5, b"\x02\x05:user:"),
            (&[b"DBSIZE".as_ref()][..], 0, b""),
            (&[b"DBSIZE".as_ref()][..], 5, b"\x02\x05:"),
            (&[b"RANDOMKEY".as_ref()][..], 0, b""),
            (&[b"RANDOMKEY".as_ref()][..], 5, b"\x02\x05:"),
            (&[b"SCAN".as_ref(), b"0".as_ref()][..], 0, b""),
            (&[b"SCAN".as_ref(), b"0".as_ref()][..], 5, b"\x02\x05:"),
            // FLUSHDB on database 0 sends the sentinel, so the executor can
            // tell it apart from FLUSHALL, which sends nothing.
            (&[b"FLUSHDB".as_ref()][..], 0, b"\x02"),
            (&[b"FLUSHDB".as_ref()][..], 5, b"\x02\x05:"),
            (&[b"FLUSHALL".as_ref()][..], 0, b""),
            (&[b"FLUSHALL".as_ref()][..], 5, b""),
        ] {
            let cmd = parsed(db, args);
            assert_eq!(
                cmd.scan_prefix.as_ref(),
                expected,
                "{:?} in database {db}",
                String::from_utf8_lossy(args[0])
            );
        }
    }

    #[test]
    fn keys_and_scan_replies_return_names_without_the_database_prefix() {
        let cmd = parsed(2, &[b"KEYS", b"*"]);
        let payload = scan_payload(None, &[b"\x02\x02:alpha", b"\x02\x02:beta"]);
        let mut results = vec![TxnOpResult {
            success: true,
            value_present: true,
            data_ptr: payload.as_ptr() as *mut u8,
            data_len: payload.len(),
            int_value: 0,
        }];
        let response = TxnResponse {
            transaction_success: true,
            num_results: results.len(),
            results: results.as_mut_ptr(),
        };
        let mut out = Vec::new();
        set_current_db(2);
        write_command_result(&cmd, Some(&response), (0, 1), 2, &mut out).unwrap();
        set_current_db(0);
        assert_eq!(out, b"*2\r\n$5\r\nalpha\r\n$4\r\nbeta\r\n");

        // MATCH is applied to the name the client would have written.
        let cmd = parsed(2, &[b"SCAN", b"0", b"MATCH", b"al*"]);
        let mut out = Vec::new();
        set_current_db(2);
        write_command_result(&cmd, Some(&response), (0, 1), 2, &mut out).unwrap();
        set_current_db(0);
        assert_eq!(out, b"*2\r\n$1\r\n0\r\n*1\r\n$5\r\nalpha\r\n");
    }

    #[test]
    fn blocking_pop_replies_return_the_key_name_without_the_prefix() {
        let cmd = parsed(4, &[b"BLPOP", b"jobs", b"0"]);
        assert_eq!(cmd.keys, vec![Bytes::from_static(b"\x02\x04:jobs")]);
        let payload = pack_bytes_list(&[
            Bytes::from_static(b"\x02\x04:jobs"),
            Bytes::from_static(b"payload"),
        ]);
        let mut results = vec![TxnOpResult {
            success: true,
            value_present: true,
            data_ptr: payload.as_ptr() as *mut u8,
            data_len: payload.len(),
            int_value: 0,
        }];
        let response = TxnResponse {
            transaction_success: true,
            num_results: results.len(),
            results: results.as_mut_ptr(),
        };
        let mut out = Vec::new();
        set_current_db(4);
        write_command_result(&cmd, Some(&response), (0, 1), 2, &mut out).unwrap();
        set_current_db(0);
        assert_eq!(out, b"*2\r\n$4\r\njobs\r\n$7\r\npayload\r\n");
    }

    #[test]
    fn watch_in_a_database_watches_that_database_s_name() {
        let mut txn_state = TransactionState::new();
        let mut client_state = ClientState::new();
        run(command(OpCode::Select, &[b"6"]), &mut txn_state, &mut client_state);
        run(command(OpCode::Watch, &[b"k"]), &mut txn_state, &mut client_state);
        assert!(txn_state
            .watched_versions
            .contains_key(&Bytes::from_static(b"\x02\x06:k")));
        assert!(!txn_state
            .watched_versions
            .contains_key(&Bytes::from_static(b"k")));
        run(command(OpCode::Reset, &[]), &mut txn_state, &mut client_state);
    }

    #[test]
    fn config_get_databases_reports_sixteen() {
        let out = run_raw(b"*3\r\n$6\r\nCONFIG\r\n$3\r\nGET\r\n$9\r\ndatabases\r\n");
        assert_eq!(out, b"*2\r\n$9\r\ndatabases\r\n$2\r\n16\r\n");
    }

}
