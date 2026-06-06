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
 */

#ifdef __cplusplus
extern "C" {
#endif

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
} TxnOpCode;

typedef enum {
    TXN_FLAG_NONE = 0,
    TXN_FLAG_SET_NX = 1u << 0,
    TXN_FLAG_SET_XX = 1u << 1,
    TXN_FLAG_SET_RETURN_OLD = 1u << 2,
    TXN_FLAG_SET_INTEGER_REPLY = 1u << 3,
    TXN_FLAG_SET_REQUIRE_ABSENT_GROUP = 1u << 4,
    TXN_FLAG_SET_KEEP_TTL = 1u << 5,
} TxnOpFlags;

/**
 * Single operation within a transaction request
 *
 * Memory layout is flat for easy FFI serialization:
 *   - op: operation code (GET=1, SET=2)
 *   - key_ptr/key_len: pointer to key bytes
 *   - val_ptr/val_len: pointer to value bytes (NULL for GET)
 *   - flags: command-specific flags; currently used by SET variants
 *   - expire_at_ms: absolute Unix millisecond expiry, or -1 for no expiry metadata
 *   - group_id: non-zero when ops belong to one all-or-nothing group, such as MSETNX
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
 * For integer-returning operations:
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
