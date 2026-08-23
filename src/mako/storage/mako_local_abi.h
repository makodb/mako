/* mako_local_abi.h - pure C boundary over Mako's local STO/MassTrans engine.
 *
 * This is the first seam of the Rust Mako migration. The implementation is
 * still the existing C++ Silo/STO engine; callers see opaque handles, binary
 * slices, owned output bytes, and integer statuses only.
 *
 * Threading contract:
 *   - call mako_local_thread_attach() on every long-lived worker;
 *   - at most one active transaction may exist on an OS thread;
 *   - a transaction must be used, committed/aborted, and destroyed on the
 *     thread that began it;
 *   - a worker already attached to native Mako or the plain mtx_* Masstree
 *     ABI cannot switch to this adapter; use a separate fixed worker pool;
 *   - the 460 STO thread IDs are process-lifetime resources and are not
 *     recycled, so a thread-per-request design will exhaust the runtime.
 *
 * Lifetime contract:
 *   db > table and db/table > transaction. In this draft the underlying
 *   MassTrans tables are process-lifetime, matching native Mako.
 *   mako_local_db_close() frees the facade handles but deliberately does not
 *   pretend it can reclaim a live Masstree safely without a process-wide RCU
 *   quiescence protocol.
 */

#ifndef MAKO_LOCAL_ABI_H
#define MAKO_LOCAL_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define MAKO_LOCAL_NOEXCEPT noexcept
extern "C" {
#else
#define MAKO_LOCAL_NOEXCEPT
#endif

#define MAKO_LOCAL_ABI_VERSION 0u

/* Semantic guarantees of the linked draft engine. Point transactions are the
 * revision-0 baseline. A bit is absent until every exposed path implements the
 * guarantee; callers must not infer a capability from STO build flags. The
 * revision-0 READ_MY_WRITES bit covers the exposed point operations only; no
 * transactional scan API is exposed by this revision. */
#define MAKO_LOCAL_FEATURE_POINT_TRANSACTIONS (UINT64_C(1) << 0)
#define MAKO_LOCAL_FEATURE_READ_MY_WRITES (UINT64_C(1) << 1)
#define MAKO_LOCAL_FEATURE_OPACITY (UINT64_C(1) << 2)

/* Draft input and transaction limits. The weighted transaction budget is one
 * item for get/remove and 4 + ceil(key_len / 8) for put/insert. Keeping it at
 * STO's 512 embedded items deliberately prevents a transaction-set allocation
 * after MassTrans has begun mutating a missing-key insert. */
#define MAKO_LOCAL_MAX_TABLE_NAME_BYTES 1024u
#define MAKO_LOCAL_MAX_KEY_BYTES 1024u
#define MAKO_LOCAL_MAX_VALUE_BYTES 1048576u
#define MAKO_LOCAL_TXN_ITEM_BUDGET 512u
/* Mako's legacy u32 `timestamp * 10 + term` format reserves one decimal digit
 * for term. This is therefore the largest representable base timestamp when
 * that distributed-format contract is honored. */
#define MAKO_LOCAL_MAX_MAKO_TIMESTAMP \
  ((UINT32_MAX - UINT32_C(9)) / UINT32_C(10))

/* Draft status numbers. Assigned numbers are never renumbered within this
 * revision. A missing key is OK with found_out == 0; it is not a conflict. */
#define MAKO_LOCAL_OK 0
#define MAKO_LOCAL_CONFLICT 1
#define MAKO_LOCAL_NOT_ATTACHED 2
#define MAKO_LOCAL_WRONG_THREAD 3
#define MAKO_LOCAL_TXN_ALREADY_ACTIVE 4
#define MAKO_LOCAL_TXN_FINISHED 5
#define MAKO_LOCAL_WRONG_DB_OR_TABLE 6
#define MAKO_LOCAL_INVALID_ARGUMENT 7
#define MAKO_LOCAL_THREAD_LIMIT 8
#define MAKO_LOCAL_BUSY 9
#define MAKO_LOCAL_OUT_OF_MEMORY 10
#define MAKO_LOCAL_INTERNAL 11 /* catchable C++ failure; assertions may abort */
/* Retained at its assigned number for old/no-RYW engines. Current RYW builds
 * compose repeated same-key mutations and do not return this status. */
#define MAKO_LOCAL_DUPLICATE_WRITE 12
#define MAKO_LOCAL_TXN_TOO_LARGE 13
#define MAKO_LOCAL_VALUE_TOO_LARGE 14
#define MAKO_LOCAL_COMMIT_HOOK_REJECTED 15
#define MAKO_LOCAL_TIMESTAMP_EXHAUSTED 16

typedef struct mako_local_db mako_local_db;
typedef struct mako_local_table mako_local_table;
typedef struct mako_local_txn mako_local_txn;

/* Called synchronously after native validation succeeds and before any write
 * is installed. `mako_timestamp` is the nonzero 32-bit Mako logical timestamp
 * (`Transaction::tid_unique_`). Return nonzero to proceed or zero to abort
 * definitely. The callback runs while Silo write locks are held. It may enter
 * a bounded in-memory critical section, but must not perform I/O, wait for
 * capacity, allocate, or unwind. */
typedef int (*mako_local_post_validate_hook)(void *context,
                                             uint32_t mako_timestamp);

/* Identity and diagnostics. The returned status string is static. */
uint32_t mako_local_abi_version(void) MAKO_LOCAL_NOEXCEPT;
uint64_t mako_local_feature_bits(void) MAKO_LOCAL_NOEXCEPT;
const char *mako_local_status_string(int status) MAKO_LOCAL_NOEXCEPT;

/* Attach the calling OS thread to the shared native-Mako STO runtime.
 * Idempotent on a thread; returns BUSY if another adapter owns the worker. */
int mako_local_thread_attach(void) MAKO_LOCAL_NOEXCEPT;

/* Atomically ensure every subsequently minted Mako logical timestamp is
 * greater than `observed`. The argument must be a nonzero timestamp previously
 * supplied to a post-validation hook. This is a monotonic recovery operation:
 * smaller calls never move the clock backward. TIMESTAMP_EXHAUSTED means
 * advancing would not leave at least one representable timestamp for a
 * subsequent checked commit. Call during recovery before admitting workers. */
int mako_local_advance_mako_timestamp_past(uint32_t observed)
    MAKO_LOCAL_NOEXCEPT;

/* One local in-memory database facade. Multiple facades share the process STO
 * runtime but own disjoint tables. close returns BUSY while a transaction
 * belonging to this database is active. */
int mako_local_db_open(mako_local_db **out) MAKO_LOCAL_NOEXCEPT;
int mako_local_db_close(mako_local_db *db) MAKO_LOCAL_NOEXCEPT;

/* Find-or-create a local table. name is a binary identifier bounded by
 * MAKO_LOCAL_MAX_TABLE_NAME_BYTES; a repeated name must use the same numeric ID
 * and returns the same borrowed handle. Numeric IDs are unique within one
 * database facade. */
int mako_local_table_open(mako_local_db *db, const uint8_t *name,
                          size_t name_len, uint64_t table_id,
                          mako_local_table **out) MAKO_LOCAL_NOEXCEPT;
uint64_t mako_local_table_id(const mako_local_table *table)
    MAKO_LOCAL_NOEXCEPT;

/* Begin one ambient STO transaction on this thread. */
int mako_local_txn_begin(mako_local_db *db, mako_local_txn **out)
    MAKO_LOCAL_NOEXCEPT;

/* Point operations. Returned value bytes belong to the ABI and must be freed
 * with mako_local_bytes_free(). put reports whether it created the key;
 * insert is put-if-absent; remove reports whether a live key existed.
 *
 * Keys and values are bounded by MAKO_LOCAL_MAX_{KEY,VALUE}_BYTES. An oversized
 * input returns nonterminal MAKO_LOCAL_VALUE_TOO_LARGE. Exceeding the weighted
 * transaction budget aborts the transaction and returns terminal
 * MAKO_LOCAL_TXN_TOO_LARGE.
 *
 * Engines advertising MAKO_LOCAL_FEATURE_READ_MY_WRITES compose repeated
 * mutations of the same table/key. A linked legacy/no-RYW build rejects the
 * next mutation request with MAKO_LOCAL_DUPLICATE_WRITE; the rejection
 * does not end the transaction. Every accepted operation is charged against
 * the weighted budget, even when it targets an already-mutated key. */
int mako_local_txn_get(mako_local_txn *txn, mako_local_table *table,
                       const uint8_t *key, size_t key_len,
                       uint8_t **value_out, size_t *value_len_out,
                       uint8_t *found_out) MAKO_LOCAL_NOEXCEPT;
int mako_local_txn_put(mako_local_txn *txn, mako_local_table *table,
                       const uint8_t *key, size_t key_len,
                       const uint8_t *value, size_t value_len,
                       uint8_t *created_out) MAKO_LOCAL_NOEXCEPT;
int mako_local_txn_insert(mako_local_txn *txn, mako_local_table *table,
                          const uint8_t *key, size_t key_len,
                          const uint8_t *value, size_t value_len,
                          uint8_t *inserted_out) MAKO_LOCAL_NOEXCEPT;
int mako_local_txn_remove(mako_local_txn *txn, mako_local_table *table,
                          const uint8_t *key, size_t key_len,
                          uint8_t *existed_out) MAKO_LOCAL_NOEXCEPT;

/* Commit/abort end the transaction but retain the small opaque handle so the
 * caller can inspect the status safely. destroy frees it; destroying an active
 * transaction first aborts it. If native abort cleanup fails, the handle,
 * buffers, database accounting, and worker are permanently quarantined:
 * every later operation returns INTERNAL and nothing retries partial cleanup. */
int mako_local_txn_commit(mako_local_txn *txn) MAKO_LOCAL_NOEXCEPT;
/* The hook variant preserves the old commit lifecycle but provides the exact
 * Mako timestamp seam needed by a transactional write-back cache. Native code
 * assigns the timestamp after locking the complete write set and invokes the
 * hook exactly once after validation for a transaction with writes. It is not
 * called for a conflict or read-only transaction. A zero return (or a C++
 * exception contained by the bridge) returns COMMIT_HOOK_REJECTED after a
 * definite abort. */
int mako_local_txn_commit_with_hook(
    mako_local_txn *txn, mako_local_post_validate_hook hook, void *context)
    MAKO_LOCAL_NOEXCEPT;
int mako_local_txn_abort(mako_local_txn *txn) MAKO_LOCAL_NOEXCEPT;
int mako_local_txn_destroy(mako_local_txn *txn) MAKO_LOCAL_NOEXCEPT;

void mako_local_bytes_free(void *bytes) MAKO_LOCAL_NOEXCEPT;

#ifdef __cplusplus
}  /* extern "C" */
#endif

#undef MAKO_LOCAL_NOEXCEPT

#endif /* MAKO_LOCAL_ABI_H */
