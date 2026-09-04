#ifndef STO_TPCC_FFI_H
#define STO_TPCC_FFI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define STO_TPCC_NOEXCEPT noexcept
extern "C" {
#else
#define STO_TPCC_NOEXCEPT
#endif

typedef struct sto_tpcc_db sto_tpcc_db;
typedef struct sto_tpcc_table sto_tpcc_table;
typedef struct sto_tpcc_thread sto_tpcc_thread;

/* Every exported operation uses one of these stable numeric results. */
typedef int32_t sto_tpcc_status;
enum {
  STO_TPCC_OK = 0,
  STO_TPCC_MISS = 1,
  STO_TPCC_DUPLICATE = 2,
  STO_TPCC_RETRY = 3,
  STO_TPCC_BUFFER_TOO_SMALL = 4,
  STO_TPCC_FATAL = 5,
};

/* Pointer safety contract for every function below: each non-NULL handle,
 * descriptor, input range, and output range must name live, correctly aligned
 * storage of the stated size for the complete synchronous call. Every mutable
 * handle/output and every location a callback may mutate through its context
 * must be exclusively accessible and disjoint from every other live handle,
 * input, output, and callback-reachable range. Immutable input ranges may
 * overlap one another unless an operation states a stricter rule. NULL is
 * accepted only where the operation explicitly permits it. Violating these
 * requirements is outside the C ABI contract. */

/* A zero field selects the Rust runtime's bounded default. */
typedef struct sto_tpcc_db_config {
  uint32_t max_threads;
  uint32_t max_key_length;
  size_t max_items_per_txn;
  size_t max_locks_per_txn;
} sto_tpcc_db_config;

/* A zero field selects sto-masstree's bounded default for that field. */
typedef struct sto_tpcc_table_config {
  uint64_t max_retained_records;
  uint64_t max_retained_key_bytes;
  uint64_t max_consumed_record_ids;
  size_t scan_chunk_records;
  size_t scan_initial_key_arena_bytes;
  size_t scan_max_key_arena_bytes;
  size_t max_scan_chunks;
  size_t max_scan_physical_records;
  /* Nonzero enables one table-wide value-generation RMW per committed row so
   * trusted scans can retain one conservative STO observation. Leave zero for
  * point-only tables. */
  uint32_t trusted_scan_value_generation;
  /* Nonzero selects a 192-byte registry entry whose atomic payload stores
   * committed values through 160 bytes without ArcSwap publication.
   *
   * ABI note: this field consumes the four bytes that were trailing padding
   * in the preceding definition. Its addition does not change sizeof on the
   * supported 64-bit ABI, but every caller must be rebuilt so those bytes are
   * initialized rather than carrying indeterminate former padding. */
  uint32_t bounded_atomic_values;
} sto_tpcc_table_config;

#if UINTPTR_MAX == UINT64_MAX
#ifdef __cplusplus
static_assert(offsetof(sto_tpcc_table_config, trusted_scan_value_generation) ==
              64);
static_assert(offsetof(sto_tpcc_table_config, bounded_atomic_values) == 68);
static_assert(sizeof(sto_tpcc_table_config) == 72);
#else
_Static_assert(offsetof(sto_tpcc_table_config,
                        trusted_scan_value_generation) == 64,
               "sto_tpcc_table_config trusted-scan offset changed");
_Static_assert(offsetof(sto_tpcc_table_config, bounded_atomic_values) == 68,
               "sto_tpcc_table_config bounded-value offset changed");
_Static_assert(sizeof(sto_tpcc_table_config) == 72,
               "sto_tpcc_table_config size changed");
#endif
#endif

/* Full retains the worker-local direct-mapped cache and scan population.
 * LastOnly probes and updates just the most recent exact point resolution;
 * scans never populate it. ReadThenWrite skips the lookup on get, but keeps
 * that get's resolution for a following put/remove of the same key. None
 * always resolves through Masstree and retains no point or scan resolutions.
 * DenseItem and DenseStock allocate a table-wide 100,000-slot cache for the
 * fused TPC-C Item and Stock paths. Ordinary scalar operations use None's
 * behavior for DenseItem and ReadThenWrite's behavior for DenseStock. Fused
 * operations do not populate the worker-local cache. */
typedef int32_t sto_tpcc_resolved_cache_policy;
enum {
  STO_TPCC_RESOLVED_CACHE_FULL = 0,
  STO_TPCC_RESOLVED_CACHE_LAST_ONLY = 1,
  STO_TPCC_RESOLVED_CACHE_READ_THEN_WRITE = 2,
  STO_TPCC_RESOLVED_CACHE_NONE = 3,
  STO_TPCC_RESOLVED_CACHE_DENSE_ITEM = 4,
  STO_TPCC_RESOLVED_CACHE_DENSE_STOCK = 5,
};

/* Receives each transaction-local value for a fixed-width key in input order.
 * current_value is NULL exactly for an absent key; otherwise the complete
 * stored value (including any application metadata suffix) is exposed for
 * this invocation. Return zero for success. A nonzero result stops further
 * callback delivery, aborts the active transaction, and returns
 * STO_TPCC_FATAL. The callback must not unwind, retain current_value, or
 * re-enter an operation on the same thread handle. The packed keys remain
 * immutable for the complete call, including every callback invocation. */
typedef int32_t (*sto_tpcc_fixed_read_callback)(
    void *context,
    size_t index,
    const uint8_t *current_value,
    size_t current_value_length);

/* A fixed-mutation callback selects exactly one action for each input key.
 * PUT replacement bytes are copied synchronously before the next callback;
 * NULL is valid for an empty replacement only. FAILED and unknown actions
 * stop delivery, abort the active attempt, and return STO_TPCC_FATAL. */
typedef int32_t sto_tpcc_fixed_modify_action;
enum {
  STO_TPCC_FIXED_MODIFY_KEEP = 0,
  STO_TPCC_FIXED_MODIFY_PUT = 1,
  STO_TPCC_FIXED_MODIFY_REMOVE = 2,
  STO_TPCC_FIXED_MODIFY_FAILED = 3,
};

typedef sto_tpcc_fixed_modify_action (*sto_tpcc_fixed_modify_callback)(
    void *context,
    size_t index,
    const uint8_t *current_value,
    size_t current_value_length,
    const uint8_t **out_replacement,
    size_t *out_replacement_length);

/* UPSERT replaces every transaction-local value. INSERT changes only absent
 * positions and reports STO_TPCC_DUPLICATE when any position was present. */
typedef int32_t sto_tpcc_fixed_put_mode;
enum {
  STO_TPCC_FIXED_PUT_UPSERT = 0,
  STO_TPCC_FIXED_PUT_INSERT = 1,
};

typedef struct sto_tpcc_fixed_value {
  /* NULL is valid exactly when length is zero. */
  const uint8_t *data;
  size_t length;
} sto_tpcc_fixed_value;

typedef struct sto_tpcc_fixed_put_result {
  /* Count of absent-to-live transitions staged by this call. */
  size_t inserted;
  /* First sequential duplicate in INSERT mode, or SIZE_MAX when none. */
  size_t first_duplicate;
} sto_tpcc_fixed_put_result;

/* One heterogeneous INSERT in a sequential transaction-local batch. Every
 * byte range remains caller-owned and immutable for the synchronous call. */
typedef struct sto_tpcc_insert_operation {
  const sto_tpcc_table *table;
  const uint8_t *key;
  size_t key_length;
  const uint8_t *value;
  size_t value_length;
} sto_tpcc_insert_operation;

typedef int32_t sto_tpcc_scan_direction;
enum {
  STO_TPCC_SCAN_FORWARD = 0,
  STO_TPCC_SCAN_REVERSE = 1,
};

typedef int32_t sto_tpcc_bound_kind;
enum {
  STO_TPCC_BOUND_UNBOUNDED = 0,
  STO_TPCC_BOUND_INCLUDED = 1,
  STO_TPCC_BOUND_EXCLUDED = 2,
};

/* Return zero to continue or nonzero to stop successfully after this row.
 * The callback must not unwind, retain row pointers, or re-enter an operation
 * on the same thread handle. */
typedef int32_t (*sto_tpcc_scan_callback)(void *context,
                                          const uint8_t *key,
                                          size_t key_length,
                                          const uint8_t *value,
                                          size_t value_length);

sto_tpcc_status sto_tpcc_db_create(const sto_tpcc_db_config *config,
                                    sto_tpcc_db **out_db) STO_TPCC_NOEXCEPT;
sto_tpcc_status sto_tpcc_db_destroy(sto_tpcc_db *db) STO_TPCC_NOEXCEPT;

/* Create every table before attaching a transaction thread on this OS thread.
 * The compatibility creator selects STO_TPCC_RESOLVED_CACHE_FULL. */
sto_tpcc_status sto_tpcc_table_create(sto_tpcc_db *db,
                                       const sto_tpcc_table_config *config,
                                       sto_tpcc_table **out_table)
    STO_TPCC_NOEXCEPT;
sto_tpcc_status sto_tpcc_table_create_with_cache_policy(
    sto_tpcc_db *db,
    const sto_tpcc_table_config *config,
    sto_tpcc_resolved_cache_policy cache_policy,
    sto_tpcc_table **out_table) STO_TPCC_NOEXCEPT;
/* Permanently reject new keys while preserving reads and updates of existing
 * records. Call after loader transactions finish and with no concurrent table
 * users. */
sto_tpcc_status
sto_tpcc_table_seal_directory_structure(sto_tpcc_table *table)
    STO_TPCC_NOEXCEPT;
sto_tpcc_status sto_tpcc_table_destroy(sto_tpcc_table *table)
    STO_TPCC_NOEXCEPT;
sto_tpcc_status sto_tpcc_table_size(const sto_tpcc_table *table,
                                     uint64_t *out_rows) STO_TPCC_NOEXCEPT;

/* Create, use, and destroy a thread handle on one OS thread. */
sto_tpcc_status sto_tpcc_thread_create(sto_tpcc_db *db,
                                        sto_tpcc_thread **out_thread)
    STO_TPCC_NOEXCEPT;
sto_tpcc_status sto_tpcc_thread_destroy(sto_tpcc_thread *thread)
    STO_TPCC_NOEXCEPT;

/*
 * A successful begin retains a worker-affine native RCU scope until commit,
 * abort, or thread destruction. Keep attempts synchronous and short; the
 * transaction may access several tables and perform reads, inserts, and scans.
 */
sto_tpcc_status sto_tpcc_txn_begin(sto_tpcc_thread *thread)
    STO_TPCC_NOEXCEPT;
sto_tpcc_status sto_tpcc_txn_commit(sto_tpcc_thread *thread)
    STO_TPCC_NOEXCEPT;
sto_tpcc_status sto_tpcc_txn_abort(sto_tpcc_thread *thread)
    STO_TPCC_NOEXCEPT;

/* After any transactional operation returns RETRY or FATAL, call the
 * idempotent sto_tpcc_txn_abort before reusing the thread handle. Some failure
 * paths have already closed the attempt while others leave it active or
 * doomed; abort safely normalizes all of them. */

/* out_actual is required. On BUFFER_TOO_SMALL it reports the required size;
 * otherwise a non-OK result leaves it zero. out_value is unchanged unless the
 * result is OK. key, out_value, and out_actual obey the disjoint mutable-range
 * rule above. */
sto_tpcc_status sto_tpcc_get(sto_tpcc_thread *thread,
                              const sto_tpcc_table *table,
                              const uint8_t *key,
                              size_t key_length,
                              uint8_t *out_value,
                              size_t value_capacity,
                              size_t *out_actual) STO_TPCC_NOEXCEPT;
/* keys contains key_count adjacent keys, each exactly key_width bytes. Only
 * widths 4, 8, 12, and 16 are supported. NULL keys is valid when key_count is
 * zero.
 * The raw callback receives the complete stored value so an adapter can
 * validate/remove any metadata suffix before applying a logical prefix limit.
 * The callback is invoked once per input key on OK. After successful argument
 * validation, out_visited reports the number of callback invocations that
 * occurred on every result. out_visited must be uniquely writable and must not
 * alias the immutable packed keys, callback context, or any handle. */
sto_tpcc_status sto_tpcc_visit_fixed(
    sto_tpcc_thread *thread,
    const sto_tpcc_table *table,
    const uint8_t *keys,
    size_t key_count,
    size_t key_width,
    sto_tpcc_fixed_read_callback callback,
    void *callback_context,
    size_t *out_visited) STO_TPCC_NOEXCEPT;
/* The packed-key and callback lifetime rules match sto_tpcc_visit_fixed;
 * fixed key widths 4, 8, 12, and 16 are supported.
 * current_value and PUT replacement bytes are complete stored values. A PUT
 * is copied before the next callback. Duplicate keys observe earlier actions
 * in the same call. On successful argument validation, out_visited reports
 * the delivered callback prefix on every result. */
sto_tpcc_status sto_tpcc_modify_fixed(
    sto_tpcc_thread *thread,
    const sto_tpcc_table *table,
    const uint8_t *keys,
    size_t key_count,
    size_t key_width,
    sto_tpcc_fixed_modify_callback callback,
    void *callback_context,
    size_t *out_visited) STO_TPCC_NOEXCEPT;
/* keys contains key_count adjacent 4-, 8-, 12-, or 16-byte keys. values
 * contains key_count adjacent descriptors. Empty values are valid. For
 * key_count zero, keys and values may both be NULL.
 * The packed keys, descriptor array, every described value range, out_result,
 * and live handles must be mutually non-aliasing.
 *
 * Input bytes are copied into Rust-owned transaction state synchronously.
 * Duplicate positions observe earlier positions in this call. INSERT visits
 * every position, leaves each duplicate unchanged, and returns DUPLICATE if
 * any was observed; nonduplicate positions remain staged and the transaction
 * remains active. out_result reports their aggregate insert count and first
 * duplicate. UPSERT always returns OK after a successful access and sets
 * first_duplicate to SIZE_MAX.
 *
 * Exactly unique batches may publish missing keys into the append-only
 * directory before the first logical write is staged. An access error or
 * later transaction abort can therefore leave reachable physical tombstones;
 * table_size changes only if the logical writes commit. */
sto_tpcc_status sto_tpcc_put_fixed(
    sto_tpcc_thread *thread,
    const sto_tpcc_table *table,
    const uint8_t *keys,
    size_t key_count,
    size_t key_width,
    const sto_tpcc_fixed_value *values,
    sto_tpcc_fixed_put_mode mode,
    sto_tpcc_fixed_put_result *out_result) STO_TPCC_NOEXCEPT;
sto_tpcc_status sto_tpcc_put(sto_tpcc_thread *thread,
                              const sto_tpcc_table *table,
                              const uint8_t *key,
                              size_t key_length,
                              const uint8_t *value,
                              size_t value_length) STO_TPCC_NOEXCEPT;
sto_tpcc_status sto_tpcc_insert(sto_tpcc_thread *thread,
                                 const sto_tpcc_table *table,
                                 const uint8_t *key,
                                 size_t key_length,
                                 const uint8_t *value,
                                 size_t value_length) STO_TPCC_NOEXCEPT;
/* Applies operations sequentially in input order. Every descriptor and byte
 * range is validated before the first mutation. The descriptor array, all
 * input bytes, and out_result must be mutually non-aliasing. Duplicate
 * positions are unchanged but do not stop later operations; DUPLICATE reports
 * that at least one was present. out_result uses inserted and first_duplicate
 * exactly like sto_tpcc_put_fixed. On an access failure, the delivered prefix
 * remains part of the doomed/abortable attempt just as if scalar calls had
 * stopped there. */
sto_tpcc_status sto_tpcc_insert_many(
    sto_tpcc_thread *thread,
    const sto_tpcc_insert_operation *operations,
    size_t operation_count,
    sto_tpcc_fixed_put_result *out_result) STO_TPCC_NOEXCEPT;
sto_tpcc_status sto_tpcc_remove(sto_tpcc_thread *thread,
                                 const sto_tpcc_table *table,
                                 const uint8_t *key,
                                 size_t key_length) STO_TPCC_NOEXCEPT;

/* Bounds are bytewise lexicographic. Pointers for unbounded bounds are ignored.
 * Rows are delivered as they are transactionally observed. If a later row or
 * chunk fails, the call returns that error and out_visited still counts every
 * earlier callback invocation; callback side effects are not rolled back. */
sto_tpcc_status sto_tpcc_scan(sto_tpcc_thread *thread,
                               const sto_tpcc_table *table,
                               sto_tpcc_scan_direction direction,
                               sto_tpcc_bound_kind lower_kind,
                               const uint8_t *lower_key,
                               size_t lower_key_length,
                               sto_tpcc_bound_kind upper_kind,
                               const uint8_t *upper_key,
                               size_t upper_key_length,
                               size_t limit,
                               sto_tpcc_scan_callback callback,
                               void *callback_context,
                               size_t *out_visited) STO_TPCC_NOEXCEPT;

/* The most recent diagnostic persists until another diagnostic on this
 * thread. It is thread-local UTF-8 and excludes its trailing NUL byte. */
size_t sto_tpcc_last_error_length(void) STO_TPCC_NOEXCEPT;
sto_tpcc_status sto_tpcc_last_error_copy(char *out_message,
                                          size_t message_capacity,
                                          size_t *out_actual)
    STO_TPCC_NOEXCEPT;

#ifdef __cplusplus
} /* extern "C" */
#endif

#undef STO_TPCC_NOEXCEPT
#endif /* STO_TPCC_FFI_H */
