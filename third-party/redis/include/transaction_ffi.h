#ifndef _MAKO_TRANSACTION_FFI_H_
#define _MAKO_TRANSACTION_FFI_H_

#include <cstdint>
#include <cstddef>

/**
 * Transaction FFI Interface for Rust-C++ communication
 *
 * This header defines the data structures and functions for executing
 * batched transactions between the Rust Redis protocol layer and the
 * C++ Mako database layer.
 *
 * Flow:
 *   1. Client sends MULTI -> Rust starts buffering commands
 *   2. Client sends GET/SET commands -> Rust buffers them
 *   3. Client sends EXEC -> Rust calls cpp_execute_transaction()
 *   4. C++ executes all operations in a single database transaction
 *   5. C++ returns results for each operation
 *   6. Rust sends RESP array with all results to client
 *
 * Encoding boundary:
 *   Values stay encoded/decoded in the C++ Mako storage layer. Rust treats
 *   keys and values as opaque Redis bytes and uses value_present to distinguish
 *   an existing empty bulk string from a missing key.
 *
 * Reserved internal keys:
 *   Redis-visible keys must not use the 0x01 prefix. The Redis layer stores
 *   TTL metadata under "\x01TTL:<key>", set internals under "\x01S:" /
 *   "\x01S#:", list internals under "\x01L:" / "\x01L#:", and sorted-set
 *   internals under "\x01Z:" / "\x01ZS:" / "\x01Z#:". These records are
 *   hidden from Redis keyspace commands.
 *
 * Sorted-set score encoding:
 *   Sorted-set score indexes use order-preserving IEEE-754 double encoding:
 *   positives flip the sign bit, negatives flip all bits, then bytes are
 *   stored big-endian. NaN is rejected at command parse/execute time.
 */

#ifdef __cplusplus
extern "C" {
#endif

bool rust_init(size_t new_max);

/**
 * Operation codes for transaction operations
 */
typedef enum {
    TXN_OP_GET = 1,
    TXN_OP_SET = 2,
    TXN_OP_DELETE = 3,
    TXN_OP_DEL = TXN_OP_DELETE,
    TXN_OP_EXISTS = 4,
    TXN_OP_APPEND = 5,
    TXN_OP_STRLEN = 6,
    TXN_OP_INCRBY = 7,
    TXN_OP_INCRBYFLOAT = 8,
    TXN_OP_EXPIRE = 9,
    TXN_OP_TTL = 10,
    TXN_OP_PERSIST = 11,
    TXN_OP_SCAN = 12,
    TXN_OP_SADD = 13,
    TXN_OP_SREM = 14,
    TXN_OP_SISMEMBER = 15,
    TXN_OP_SCARD = 16,
    TXN_OP_SMEMBERS = 17,
    TXN_OP_SPOP = 18,
    TXN_OP_SRANDMEMBER = 19,
    TXN_OP_SMOVE = 20,
    TXN_OP_SET_ALGEBRA = 21,
    TXN_OP_TYPE = 22,
    TXN_OP_LPUSH = 23,
    TXN_OP_RPUSH = 24,
    TXN_OP_LPOP = 25,
    TXN_OP_RPOP = 26,
    TXN_OP_LLEN = 27,
    TXN_OP_LINDEX = 28,
    TXN_OP_LRANGE = 29,
    TXN_OP_LSET = 30,
    TXN_OP_LREM = 31,
    TXN_OP_LTRIM = 32,
    TXN_OP_LINSERT = 33,
    TXN_OP_LMOVE = 34,
    TXN_OP_LPOS = 35,
    TXN_OP_ZADD = 36,
    TXN_OP_ZSCORE = 37,
    TXN_OP_ZREM = 38,
    TXN_OP_ZCARD = 39,
    TXN_OP_ZRANGE = 40,
    TXN_OP_ZRANK = 41,
    TXN_OP_ZPOPMIN = 42,
    TXN_OP_ZCOUNT = 43,
    TXN_OP_ZSCAN = 44,
    TXN_OP_FLUSHDB = 45,
    TXN_OP_HSET = 46,
    TXN_OP_HGET = 47,
    TXN_OP_HMGET = 48,
    TXN_OP_HGETALL = 49,
    TXN_OP_HDEL = 50,
    TXN_OP_HEXISTS = 51,
    TXN_OP_HLEN = 52,
    TXN_OP_HKEYS = 53,
    TXN_OP_HVALS = 54,
    TXN_OP_HSTRLEN = 55,
    TXN_OP_HINCRBY = 56,
    TXN_OP_HINCRBYFLOAT = 57,
    TXN_OP_HSCAN = 58,
    TXN_OP_SETBIT = 59,
    TXN_OP_GETBIT = 60,
    TXN_OP_SETRANGE = 61,
    TXN_OP_GETRANGE = 62,
    TXN_OP_BPOP = 63,
    TXN_OP_RENAME = 64,
    TXN_OP_SORT = 65,
    TXN_OP_DUMP = 66,
    TXN_OP_RESTORE_LIST = 67,
    TXN_OP_ZRANGEBYLEX = 68,
    TXN_OP_ZLEXCOUNT = 69,
    TXN_OP_ZREMRANGEBYSCORE = 70,
    TXN_OP_ZREMRANGEBYRANK = 71,
    TXN_OP_ZREMRANGEBYLEX = 72,
    TXN_OP_ZRANGESTORE = 73,
    TXN_OP_ZSET_ALGEBRA = 74,
    TXN_OP_ZMPOP = 75,
    TXN_OP_ZRANDMEMBER = 76,
    TXN_OP_COPY = 77,
} TxnOpCode;

typedef enum {
    TXN_FLAG_NONE = 0,
    TXN_FLAG_SET_NX = 1u << 0,
    TXN_FLAG_SET_XX = 1u << 1,
    TXN_FLAG_SET_RETURN_OLD = 1u << 2,
    TXN_FLAG_SET_INTEGER_REPLY = 1u << 3,
    TXN_FLAG_SET_REQUIRE_ABSENT_GROUP = 1u << 4,
    TXN_FLAG_SET_KEEP_TTL = 1u << 5,
    TXN_FLAG_TTL_MILLISECONDS = 1u << 6,
    TXN_FLAG_EXPIRE_NX = 1u << 7,
    TXN_FLAG_EXPIRE_XX = 1u << 8,
    TXN_FLAG_EXPIRE_GT = 1u << 9,
    TXN_FLAG_EXPIRE_LT = 1u << 10,
    TXN_FLAG_SCAN_COUNT_ONLY = 1u << 11,
    TXN_FLAG_SET_COUNT_GIVEN = 1u << 12,
    TXN_FLAG_SET_ALLOW_DUPLICATES = 1u << 13,
    TXN_FLAG_SET_ALGEBRA_UNION = 1u << 14,
    TXN_FLAG_SET_ALGEBRA_DIFF = 1u << 15,
    TXN_FLAG_SET_ALGEBRA_STORE = 1u << 16,
    TXN_FLAG_LIST_PUSH_IF_EXISTS = 1u << 17,
    TXN_FLAG_LIST_INSERT_BEFORE = 1u << 18,
    TXN_FLAG_LIST_SOURCE_LEFT = 1u << 19,
    TXN_FLAG_LIST_DEST_LEFT = 1u << 20,
    TXN_FLAG_LIST_COUNT_GIVEN = 1u << 21,
    TXN_FLAG_ZADD_NX = 1u << 22,
    TXN_FLAG_ZADD_XX = 1u << 23,
    TXN_FLAG_ZADD_CH = 1u << 24,
    TXN_FLAG_ZADD_INCR = 1u << 25,
    TXN_FLAG_ZADD_GT = 1u << 26,
    TXN_FLAG_ZADD_LT = 1u << 27,
    TXN_FLAG_Z_WITHSCORES = 1u << 28,
    TXN_FLAG_Z_REV = 1u << 29,
    TXN_FLAG_Z_BYSCORE = 1u << 30,
    TXN_FLAG_Z_COUNT_GIVEN = 1u << 31,
} TxnOpFlags;

/**
 * Single operation within a transaction request
 *
 * Memory layout is flat for easy FFI serialization:
 *   - op: operation code (GET=1, SET=2)
 *   - key_ptr/key_len: pointer to key bytes
 *   - val_ptr/val_len: pointer to value bytes (NULL for GET)
 *   - flags: command-specific flags; currently used by SET variants
 *   - expire_at_ms: absolute Unix millisecond expiry for SET/EXPIRE, or -1
 *     for no expiry metadata
 *   - group_id: non-zero when ops belong to one all-or-nothing group, such as MSETNX
 *
 * For TXN_OP_SCAN:
 *   - key_ptr/key_len carries the decoded cursor user key, or empty for cursor 0
 *   - val_ptr/val_len carries the literal scan prefix derived from MATCH
 *   - expire_at_ms carries the COUNT work hint
 *   - TXN_FLAG_SCAN_COUNT_ONLY returns DBSIZE in int_value instead of key bytes
 *
 * For set operations:
 *   - SADD/SREM val_ptr carries a length-prefixed list of members
 *   - SISMEMBER val_ptr carries one member
 *   - SMOVE key carries the source set; val_ptr carries [destination, member]
 *   - SPOP/SRANDMEMBER expire_at_ms carries the requested count
 *   - SET_ALGEBRA key carries the destination for *STORE or the first source
 *     for non-store; val_ptr carries source set names
 *
 * For list operations:
 *   - LPUSH/RPUSH val_ptr carries a length-prefixed list of elements
 *   - LPOP/RPOP expire_at_ms carries count; LIST_COUNT_GIVEN controls
 *     bulk-vs-array response formatting in Rust
 *   - LINDEX expire_at_ms carries the signed list index
 *   - LRANGE/LTRIM val_ptr carries [start, stop] as byte strings
 *   - LSET/LREM/LINSERT val_ptr carries command-specific byte lists
 *   - LMOVE key carries source; val_ptr carries [destination]
 *   - The C++ executor stages list contents per transaction and flushes dirty
 *     lists once before commit.
 *
 * For sorted-set operations:
 *   - ZADD val_ptr carries [score, member, ...]
 *   - ZSCORE/ZRANK val_ptr carries one member
 *   - ZREM val_ptr carries members
 *   - ZRANGE val_ptr carries [start, stop] or [min, max, offset, count]
 *   - ZPOPMIN/ZPOPMAX expire_at_ms carries count; Z_COUNT_GIVEN controls
 *     whether the client supplied count
 */
typedef struct {
    uint32_t op;           // TxnOpCode
    const uint8_t* key_ptr;
    size_t key_len;
    const uint8_t* val_ptr;  // NULL for GET operations
    size_t val_len;          // 0 for GET operations
    uint32_t flags;          // TxnOpFlags
    int64_t expire_at_ms;    // -1 when no TTL metadata should be written
    uint32_t group_id;       // 0 for no command group
} TxnOperation;

/**
 * Transaction request: array of operations to execute atomically
 */
typedef struct {
    size_t num_ops;           // Number of operations
    const TxnOperation* ops;  // Array of operations
} TxnRequest;

/**
 * Result for a single operation
 *
 * For GET:
 *   - success=true, value_present=true: hit, data contains value bytes
 *     (data_len may be 0 for an empty Redis bulk string)
 *   - success=true, value_present=false: miss (key not found)
 * For SET:
 *   - success=true: write succeeded
 *   - success=false: write failed (conflict, etc.)
 * For DELETE / EXISTS:
 *   - success=true, value_present=true: key existed
 *   - success=true, value_present=false: key did not exist
 * For integer-returning operations, including TTL-family commands:
 *   - success=true, int_value carries the Redis integer reply
 */
typedef struct {
    bool success;
    bool value_present;
    uint8_t* data_ptr;   // malloc'd buffer for GET results, NULL for SET
    size_t data_len;
    int64_t int_value;
} TxnOpResult;

/**
 * Transaction response: results for all operations
 *
 * If transaction_success is false, the entire transaction was aborted
 * and individual results may not be meaningful.
 */
typedef struct {
    bool transaction_success;  // True if transaction committed
    size_t num_results;
    TxnOpResult* results;      // Array of results (malloc'd by C++)
} TxnResponse;

/**
 * Lightweight server metrics exposed to the Redis INFO formatter.
 */
typedef struct {
    uint64_t txn_commits;
    uint64_t txn_aborts;
    uint64_t txn_retries;
    uint64_t uptime_seconds;
} MakoMetrics;

/**
 * Execute a batch of operations as a single database transaction
 *
 * @param request  Pointer to transaction request
 * @param response Pointer to response struct (C++ fills this in)
 * @return true if the call succeeded (check response->transaction_success for commit status)
 *
 * Rust is responsible for:
 *   - Allocating and filling TxnRequest
 *   - Calling cpp_free_transaction_response() after processing results
 *
 * C++ is responsible for:
 *   - Executing all operations in a single DB transaction
 *   - Allocating response->results array
 *   - Allocating data_ptr buffers for GET results
 */
bool cpp_execute_transaction(const TxnRequest* request, TxnResponse* response);

/**
 * Free response resources allocated by cpp_execute_transaction
 */
void cpp_free_transaction_response(TxnResponse* response);

/**
 * Fill metrics for INFO server / INFO mako.
 */
bool cpp_get_metrics(MakoMetrics* metrics);

/**
 * Record one Redis-layer retry attempt. The retry loop lives in Rust, while
 * INFO mako metrics live in the C++ executor.
 */
void cpp_record_txn_retry(void);

#ifdef __cplusplus
}
#endif

#endif // _MAKO_TRANSACTION_FFI_H_
