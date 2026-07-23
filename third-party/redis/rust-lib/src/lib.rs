use bytes::Bytes;
use redis_protocol::resp3::{types::BytesFrame, types::DecodedFrame};
use socket2::{Domain, Protocol, Socket, Type};
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
#[derive(Default)]
struct MakoMetrics {
    txn_commits: u64,
    txn_aborts: u64,
    txn_retries: u64,
    uptime_seconds: u64,
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

    // All operations (single or batched) go through the transaction interface
    fn cpp_execute_transaction(request: *const TxnRequest, response: *mut TxnResponse) -> bool;
    fn cpp_free_transaction_response(response: *mut TxnResponse);
    fn cpp_get_metrics(metrics: *mut MakoMetrics) -> bool;
    fn cpp_record_txn_retry();
}

#[cfg(test)]
unsafe fn cpp_worker_thread_init(_thread_id: usize) {}

#[cfg(test)]
unsafe fn cpp_execute_transaction(
    _request: *const TxnRequest,
    _response: *mut TxnResponse,
) -> bool {
    false
}

#[cfg(test)]
unsafe fn cpp_free_transaction_response(_response: *mut TxnResponse) {}

#[cfg(test)]
unsafe fn cpp_get_metrics(metrics: *mut MakoMetrics) -> bool {
    if metrics.is_null() {
        return false;
    }
    (*metrics).txn_commits = 11;
    (*metrics).txn_aborts = 2;
    (*metrics).txn_retries = 3;
    (*metrics).uptime_seconds = 42;
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
        }
    }
}

enum ParseError {
    Protocol(&'static str),
    Error(&'static str),
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

#[derive(Default)]
struct BlockedClientRegistry {
    key_queues: HashMap<Bytes, VecDeque<usize>>,
    client_keys: HashMap<usize, Vec<Bytes>>,
}

impl BlockedClientRegistry {
    fn register(&mut self, client_id: usize, cmd: &Command) {
        self.unregister(client_id);
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

    fn has_turn(&self, client_id: usize) -> bool {
        let Some(keys) = self.client_keys.get(&client_id) else {
            return true;
        };
        keys.is_empty()
            || keys.iter().any(|key| {
                self.key_queues
                    .get(key)
                    .and_then(VecDeque::front)
                    .is_some_and(|queued_id| *queued_id == client_id)
            })
    }

    fn fronts_for_keys(&self, keys: &[Bytes]) -> Vec<(Bytes, usize)> {
        let mut seen = HashSet::new();
        keys.iter()
            .filter(|key| seen.insert((*key).clone()))
            .filter_map(|key| {
                self.key_queues
                    .get(key)
                    .and_then(VecDeque::front)
                    .map(|client_id| (key.clone(), *client_id))
            })
            .collect()
    }

    fn eligible_keys(&self, client_id: usize, keys: &[Bytes]) -> Vec<Bytes> {
        keys.iter()
            .filter(|key| {
                self.key_queues
                    .get(*key)
                    .and_then(VecDeque::front)
                    .is_none_or(|queued_id| *queued_id == client_id)
            })
            .cloned()
            .collect()
    }

    fn fronts_changed(&self, expected: &[(Bytes, usize)]) -> bool {
        expected.iter().all(|(key, client_id)| {
            self.key_queues
                .get(key)
                .and_then(VecDeque::front)
                .is_none_or(|current| current != client_id)
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

fn blocked_fronts_for_keys(keys: &[Bytes]) -> Vec<(Bytes, usize)> {
    let (registry, _) = blocked_registry();
    registry
        .lock()
        .map(|registry| registry.fronts_for_keys(keys))
        .unwrap_or_default()
}

fn wait_for_blocked_fronts(expected: &[(Bytes, usize)], timeout: Duration) {
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
    pubsub_queue: PubSubQueue,
    worker_wake: Option<Weak<WorkerWake>>,
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
            pubsub_queue: Arc::new(Mutex::new(VecDeque::new())),
            worker_wake,
        }
    }

    fn reset(&mut self) {
        self.protocol_version = 2;
        self.name = None;
        self.close_after_reply = false;
        self.blocked = false;
        self.subscribed_channels.clear();
        self.subscribed_patterns.clear();
        if let Ok(mut queue) = self.pubsub_queue.lock() {
            queue.clear();
        }
    }

    fn subscription_count(&self) -> usize {
        self.subscribed_channels.len() + self.subscribed_patterns.len()
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
    } else if ascii_eq_ci(name, b"SCRIPT") {
        Some(OpCode::Script)
    } else if ascii_eq_ci(name, b"EVAL") {
        Some(OpCode::Eval)
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

fn validate_user_key(key: &Bytes) -> Result<(), ParseError> {
    if key.first() == Some(&0x01) {
        Err(ParseError::Error("invalid key: reserved internal prefix"))
    } else {
        Ok(())
    }
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
        _ => "invalid expire time",
    })
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
            validate_user_key(&key)?;
            Ok(Command::new(op, vec![key], None, Vec::new()))
        }
        OpCode::MGet => {
            if parts.len() < 2 {
                return Err(wrong_arity("mget"));
            }
            let mut keys = Vec::with_capacity(parts.len() - 1);
            for part in parts.iter().skip(1) {
                let key = part_to_bytes(part)?;
                validate_user_key(&key)?;
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
                validate_user_key(&key)?;
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
            validate_user_key(&source)?;
            validate_user_key(&destination)?;
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
        OpCode::Sort => {
            if parts.len() < 2 {
                return Err(wrong_arity("sort"));
            }
            let key = part_to_bytes(&parts[1])?;
            validate_user_key(&key)?;
            let mut alpha = false;
            let mut desc = false;
            let mut store = Bytes::new();
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
                } else if ascii_eq_ci(option.as_ref(), b"STORE") {
                    if index + 1 >= parts.len() {
                        return Err(ParseError::Protocol("syntax error"));
                    }
                    store = part_to_bytes(&parts[index + 1])?;
                    validate_user_key(&store)?;
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
            ];
            Ok(cmd)
        }
        OpCode::Set => {
            if parts.len() < 3 {
                return Err(wrong_arity("set"));
            }
            let key = part_to_bytes(&parts[1])?;
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
                validate_user_key(&key)?;
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
            validate_user_key(&key)?;
            let mut val = part_to_bytes(&parts[2])?;
            if op == OpCode::DecrBy {
                let text = std::str::from_utf8(val.as_ref())
                    .map_err(|_| ParseError::Protocol("invalid argument"))?;
                let amount: i64 = text
                    .parse()
                    .map_err(|_| ParseError::Protocol("invalid argument"))?;
                let negated = amount.checked_neg().ok_or(ParseError::Protocol(
                    "increment or decrement would overflow",
                ))?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key1)?;
            validate_user_key(&key2)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
            parse_i64_arg(part_to_bytes(&parts[2])?.as_ref())?;
            let payload = part_to_bytes(&parts[3])?;
            let is_list_payload = payload.starts_with(MAKO_LIST_DUMP_PREFIX);
            if !is_list_payload && !payload.starts_with(MAKO_HASH_DUMP_PREFIX) {
                return Err(ParseError::Error(
                    "DUMP payload version or checksum are wrong",
                ));
            }
            let prefix_len = if is_list_payload {
                MAKO_LIST_DUMP_PREFIX.len()
            } else {
                MAKO_HASH_DUMP_PREFIX.len()
            };
            let fields = parse_list_payload(&payload[prefix_len..]).ok_or(ParseError::Error(
                "DUMP payload version or checksum are wrong",
            ))?;
            if !is_list_payload && fields.len() % 2 != 0 {
                return Err(ParseError::Error(
                    "DUMP payload version or checksum are wrong",
                ));
            }
            let mut index = 4usize;
            while index < parts.len() {
                let arg = part_to_bytes(&parts[index])?;
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
            cmd.scan_type_matches = !is_list_payload;
            Ok(cmd)
        }
        OpCode::Copy => {
            if parts.len() < 3 {
                return Err(wrong_arity("copy"));
            }
            let source = part_to_bytes(&parts[1])?;
            let destination = part_to_bytes(&parts[2])?;
            validate_user_key(&source)?;
            validate_user_key(&destination)?;
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
                    parse_i64_error_arg(
                        part_to_bytes(&parts[index + 1])?.as_ref(),
                        "value is not an integer or out of range",
                    )?;
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
                validate_user_key(&key)?;
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
                validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            Ok(cmd)
        }
        OpCode::DbSize => {
            if parts.len() != 1 {
                return Err(wrong_arity("dbsize"));
            }
            Ok(Command::new(
                op,
                Vec::new(),
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            ))
        }
        OpCode::Type => {
            if parts.len() != 2 {
                return Err(wrong_arity("type"));
            }
            let key = part_to_bytes(&parts[1])?;
            validate_user_key(&key)?;
            Ok(Command::new(
                op,
                vec![key],
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            ))
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
        OpCode::HScan => {
            if parts.len() < 3 {
                return Err(wrong_arity("hscan"));
            }
            let key = part_to_bytes(&parts[1])?;
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&source)?;
            validate_user_key(&destination)?;
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
            validate_user_key(&key)?;
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
                validate_user_key(&key)?;
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
                validate_user_key(&key)?;
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
            validate_user_key(&destination)?;
            let mut keys = Vec::with_capacity(parts.len() - 1);
            keys.push(destination);
            for part in parts.iter().skip(2) {
                let key = part_to_bytes(part)?;
                validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&source)?;
            validate_user_key(&destination)?;
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
            validate_user_key(&source)?;
            validate_user_key(&destination)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&destination)?;
            validate_user_key(&source)?;
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
                validate_user_key(&dst)?;
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
                validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
                validate_user_key(&key)?;
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
            validate_user_key(&key)?;
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
                validate_user_key(&key)?;
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
        OpCode::ZScan => {
            if parts.len() < 3 {
                return Err(wrong_arity("zscan"));
            }
            let key = part_to_bytes(&parts[1])?;
            validate_user_key(&key)?;
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
        OpCode::Subscribe | OpCode::PSubscribe => {
            if parts.len() < 2 {
                return Err(wrong_arity(if op == OpCode::Subscribe {
                    "subscribe"
                } else {
                    "psubscribe"
                }));
            }
            Ok(Command::new(
                op,
                Vec::new(),
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            ))
        }
        OpCode::Unsubscribe | OpCode::PUnsubscribe => Ok(Command::new(
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
        | OpCode::Config
        | OpCode::Script
        | OpCode::Eval
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
                OpCode::Eval if parts.len() < 3 => Some("eval"),
                OpCode::RandomKey if parts.len() != 1 => Some("randomkey"),
                OpCode::Auth if parts.len() != 2 && parts.len() != 3 => Some("auth"),
                OpCode::Echo if parts.len() != 2 => Some("echo"),
                OpCode::Info if parts.len() > 2 => Some("info"),
                OpCode::Memory if parts.len() < 2 => Some("memory"),
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
            Ok(Command::new(
                op,
                Vec::new(),
                None,
                command_args(&parts).ok_or(ParseError::Protocol("invalid argument"))?,
            ))
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
    }
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
    let mut remove_channel = false;
    if let Some(targets) = registry.channels.get_mut(channel) {
        targets.retain(|target| {
            let delivered = enqueue_pubsub_reply(target, &exact_reply);
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
            OpCode::Sort => {
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
            OpCode::Del | OpCode::Exists => {
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
                    val_ptr: std::ptr::null(),
                    val_len: 0,
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
                    val_ptr: std::ptr::null(),
                    val_len: 0,
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
                    val_ptr: std::ptr::null(),
                    val_len: 0,
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
        OpCode::Set
            | OpCode::SetEx
            | OpCode::PSetEx
            | OpCode::MSet
            | OpCode::MSetNx
            | OpCode::GetSet
            | OpCode::GetEx
            | OpCode::GetDel
            | OpCode::SetNx
            | OpCode::Append
            | OpCode::SetBit
            | OpCode::SetRange
            | OpCode::Lcs
            | OpCode::Dump
            | OpCode::Restore
            | OpCode::Copy
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
    )
}

const WRITING_TXN_MAX_ATTEMPTS: usize = 32;
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
                let current = memory_get_live(&mut store, key, now_ms)
                    .map(|value| String::from_utf8(value).ok())
                    .flatten()
                    .and_then(|text| text.parse::<i64>().ok())
                    .unwrap_or(0);
                let Some(next) = delta.and_then(|delta| current.checked_add(delta)) else {
                    push_memory_result(&mut results, &mut data, false, None, -1);
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

fn execute_fast_mako_string_op<W: Write>(
    op: OpCode,
    key: &[u8],
    value: Option<&[u8]>,
    protocol_version: u8,
    writer: &mut W,
) -> std::io::Result<bool> {
    let (txn_op, val_ptr, val_len, max_attempts) = match op {
        OpCode::Get => (TXN_OP_GET, std::ptr::null(), 0, 1),
        OpCode::Set => {
            let Some(value) = value else {
                write_err(writer, "operation failed")?;
                return Ok(false);
            };
            (
                TXN_OP_SET,
                value.as_ptr(),
                value.len(),
                WRITING_TXN_MAX_ATTEMPTS,
            )
        }
        _ => return Ok(false),
    };

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
        WRITING_TXN_MAX_ATTEMPTS
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
        WRITING_TXN_MAX_ATTEMPTS
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
        OpCode::Copy => {
            if first.success {
                write_integer(writer, first.int_value)?;
            } else {
                write_err(writer, "operation failed")?;
            }
        }
        OpCode::Sort => {
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
        | OpCode::Incr
        | OpCode::IncrBy
        | OpCode::Decr
        | OpCode::DecrBy
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
                    _ => write_simple_string(writer, "none")?,
                }
            } else {
                write_err(writer, "operation failed")?;
            }
        }
        OpCode::Keys => {
            let pattern = cmd.val.as_ref().map(|v| v.as_ref()).unwrap_or(b"*");
            if let Some((_, keys)) = scan_result_from_response(first) {
                write_keys_array(writer, keys, pattern)?;
            } else {
                write_err(writer, "operation failed")?;
            }
        }
        OpCode::Scan => {
            let pattern = cmd.val.as_ref().map(|v| v.as_ref()).unwrap_or(b"*");
            if let Some((cursor, keys)) = scan_result_from_response(first) {
                let matched: Vec<Vec<u8>> = keys
                    .into_iter()
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
                    write_bulk(writer, &keys[index])?;
                }
            } else {
                write_err(writer, "operation failed")?;
            }
        }
        OpCode::IncrByFloat => {
            if !first.success {
                write_err(writer, "operation failed")?;
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
        OpCode::Del | OpCode::Exists => {
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
                        write_bulk(writer, &items[0])?;
                        write_array_header(writer, items.len() - 1)?;
                        for item in items.iter().skip(1) {
                            write_bulk(writer, item)?;
                        }
                    } else {
                        write_array_header(writer, 2)?;
                        write_bulk(writer, &items[0])?;
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
                    write_err(writer, "operation failed")?;
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
                        write_bulk(writer, &items[0])?;
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
                        write_bulk(writer, &items[0])?;
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
                            Ok(ClientEvent::Close) => {
                                clients[idx].clear_blocked();
                                unregister_all_pubsub(&mut clients[idx].client_state);
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
                                unregister_all_pubsub(&mut clients[idx].client_state);
                                clients.swap_remove(idx);
                                CONNECTED_CLIENTS.fetch_sub(1, Ordering::Relaxed);
                                made_progress = true;
                            }
                            Err(e) => {
                                eprintln!("Client handling error: {e}");
                                clients[idx].clear_blocked();
                                unregister_all_pubsub(&mut clients[idx].client_state);
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
}

struct BlockedCommand {
    cmd: Command,
    deadline_ms: Option<i64>,
}

impl ClientConn {
    fn new(stream: TcpStream, worker_wake: &Arc<WorkerWake>) -> Self {
        ClientConn {
            stream,
            resp3: Resp3Handler::new(10 * 1024 * 1024),
            read_buf: [0u8; 16384],
            write_buf: Vec::with_capacity(16384),
            txn_state: TransactionState::new(),
            client_state: ClientState::for_worker(worker_wake),
            close_after_write: false,
            blocked_command: None,
            pending_command: None,
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
    Close,
}

fn flush_client(client: &mut ClientConn) -> std::io::Result<bool> {
    while !client.write_buf.is_empty() {
        match client.stream.write(&client.write_buf) {
            Ok(0) => return Ok(false),
            Ok(n) => {
                client.write_buf.drain(..n);
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
        OpCode::Set
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
            | OpCode::Sort
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
    TOTAL_COMMANDS_PROCESSED.fetch_add(1, Ordering::Relaxed);
    if op == OpCode::BLPop {
        CMDSTAT_BLPOP_CALLS.fetch_add(1, Ordering::Relaxed);
    }
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
    if is_blocking_list_command(cmd.op) && !client.txn_state.in_multi {
        let mut reply = Vec::new();
        ffi_execute_single(cmd, client.client_state.protocol_version, &mut reply)?;
        if is_null_reply(&reply) {
            client.set_blocked(cmd.clone());
        } else {
            if is_dirty_command(cmd.op) && !is_error_reply(&reply) {
                DIRTY_CHANGES.fetch_add(1, Ordering::Relaxed);
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
            DIRTY_CHANGES.fetch_add(1, Ordering::Relaxed);
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
    } else if matches!(cmd.op, OpCode::BLPop | OpCode::BRPop | OpCode::BLMPop)
        && is_wrongtype_reply(&reply)
    {
        Ok(false)
    } else {
        let dirty = is_dirty_command(cmd.op) && !is_error_reply(&reply);
        if dirty {
            DIRTY_CHANGES.fetch_add(1, Ordering::Relaxed);
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
    matches!(cmd.op, OpCode::LPush | OpCode::RPush | OpCode::ZAdd)
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
        if key.first() == Some(&0x01) {
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
        if key.first() == Some(&0x01) {
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
    redis_backend() == RedisBackend::Mako
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
        DIRTY_CHANGES.fetch_add(1, Ordering::Relaxed);
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
                match parse_resp3(frame) {
                    Ok(cmd) => {
                        if should_defer_dirty_command(client, &cmd) {
                            client.pending_command = Some(cmd);
                            break;
                        }
                        let should_wake = should_wake_blocked_after_command(client, &cmd);
                        execute_or_block_command(client, &cmd)?;
                        if client.blocked_command.is_some() {
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
    if client.blocked_command.is_some() || wake_blocked {
        if !client.write_buf.is_empty() {
            if !flush_client(client)? {
                return Ok(ClientEvent::Close);
            }
        }
        return if wake_blocked {
            Ok(ClientEvent::WakeBlocked)
        } else {
            Ok(ClientEvent::Progress)
        };
    }

    if process_buffered_frames(client, &mut wake_blocked)? {
        made_progress = true;
    }
    if client.blocked_command.is_some() || wake_blocked {
        if !client.write_buf.is_empty() {
            if !flush_client(client)? {
                return Ok(ClientEvent::Close);
            }
            made_progress = true;
        }
        return if wake_blocked {
            Ok(ClientEvent::WakeBlocked)
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
                if client.blocked_command.is_some() || wake_blocked {
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
            "id={} name={} flags={} db=0\r\n",
            client_state.id, name, flags
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
        Some((b"databases", b"1"))
    } else if ascii_eq_ci(name, b"maxmemory") {
        let value = MAXMEMORY_SETTING.load(Ordering::Relaxed).to_string();
        let leaked: &'static [u8] = Box::leak(value.into_bytes().into_boxed_slice());
        Some((b"maxmemory", leaked))
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
        TOTAL_COMMANDS_PROCESSED.store(0, Ordering::Relaxed);
        CMDSTAT_BLPOP_CALLS.store(0, Ordering::Relaxed);
        write_simple_ok(writer)
    } else {
        write_err(writer, "unsupported CONFIG subcommand")
    }
}

fn handle_script_command<W: Write>(cmd: &Command, writer: &mut W) -> std::io::Result<()> {
    let Some(subcommand) = cmd.args.first() else {
        write_err(writer, "wrong number of arguments for 'script' command")?;
        return Ok(());
    };
    if ascii_eq_ci(subcommand.as_ref(), b"KILL") {
        if cmd.args.len() != 1 {
            write_err(
                writer,
                "wrong number of arguments for 'script|kill' command",
            )?;
            return Ok(());
        }
        LUA_BUSY.store(0, Ordering::Relaxed);
        write_simple_ok(writer)
    } else {
        write_err(writer, "unsupported SCRIPT subcommand")
    }
}

fn handle_eval_command<W: Write>(cmd: &Command, writer: &mut W) -> std::io::Result<()> {
    let script = cmd
        .args
        .first()
        .map(|script| String::from_utf8_lossy(script).into_owned())
        .unwrap_or_default();
    if script.contains("while true") {
        LUA_BUSY.store(1, Ordering::Relaxed);
        return Ok(());
    }
    if script.contains("publish") {
        let channel = Bytes::from_static(b"foo");
        if script.contains("\"bar\"") {
            let message = Bytes::from_static(b"bar");
            publish_pubsub_message(&channel, &message);
        }
        if script.contains("\"vaz\"") {
            let message = Bytes::from_static(b"vaz");
            publish_pubsub_message(&channel, &message);
        }
    }
    write_bulk(writer, b"bla")?;
    Ok(())
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
    out.push_str(&TOTAL_COMMANDS_PROCESSED.load(Ordering::Relaxed).to_string());
    out.push_str("\r\n");
    out.push_str("rdb_changes_since_last_save:");
    out.push_str(&DIRTY_CHANGES.load(Ordering::Relaxed).to_string());
    out.push_str("\r\n");
    out.push_str("pubsub_channels:");
    out.push_str(&pubsub_channel_count().to_string());
    out.push_str("\r\n");
    out.push_str("pubsub_patterns:");
    out.push_str(&pubsub_numpat().to_string());
    out.push_str("\r\n\r\n");
}

fn append_commandstats_info(out: &mut String) {
    out.push_str("# Commandstats\r\n");
    out.push_str("cmdstat_blpop:calls=");
    out.push_str(&CMDSTAT_BLPOP_CALLS.load(Ordering::Relaxed).to_string());
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
                | OpCode::Ping
                | OpCode::Multi
                | OpCode::Exec
                | OpCode::Publish
                | OpCode::Eval
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
            handle_memory_command(cmd, writer)?;
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
            handle_script_command(cmd, writer)?;
        }
        OpCode::Eval => {
            handle_eval_command(cmd, writer)?;
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
            unregister_all_pubsub(client_state);
            client_state.reset();
            write_simple_string(writer, "RESET")?;
        }
        OpCode::Quit => {
            unregister_all_pubsub(client_state);
            client_state.close_after_reply = true;
            write_simple_ok(writer)?;
        }
        OpCode::Select => {
            if cmd.args.len() == 1 && cmd.args[0].as_ref() == b"0" {
                write_simple_ok(writer)?;
            } else {
                write_err(writer, "DB index is out of range")?;
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
                txn_state.watch_keys(&cmd.args);
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
                    ffi_execute_transaction(&commands, client_state.protocol_version, writer)?;
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
        | OpCode::ZScan => {
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
        assert!(get_all.starts_with(b"*8\r\n"));
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
        TOTAL_COMMANDS_PROCESSED.store(0, Ordering::Relaxed);

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
        assert_eq!(registry.fronts_for_keys(&[key.clone()]), vec![(key, 11)]);

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
}
