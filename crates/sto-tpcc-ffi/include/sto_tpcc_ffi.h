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
} sto_tpcc_table_config;

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

/* Create every table before attaching a transaction thread on this OS thread. */
sto_tpcc_status sto_tpcc_table_create(sto_tpcc_db *db,
                                       const sto_tpcc_table_config *config,
                                       sto_tpcc_table **out_table)
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

sto_tpcc_status sto_tpcc_txn_begin(sto_tpcc_thread *thread)
    STO_TPCC_NOEXCEPT;
sto_tpcc_status sto_tpcc_txn_commit(sto_tpcc_thread *thread)
    STO_TPCC_NOEXCEPT;
sto_tpcc_status sto_tpcc_txn_abort(sto_tpcc_thread *thread)
    STO_TPCC_NOEXCEPT;

/* out_actual is required. On BUFFER_TOO_SMALL it reports the required size. */
sto_tpcc_status sto_tpcc_get(sto_tpcc_thread *thread,
                              const sto_tpcc_table *table,
                              const uint8_t *key,
                              size_t key_length,
                              uint8_t *out_value,
                              size_t value_capacity,
                              size_t *out_actual) STO_TPCC_NOEXCEPT;
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
sto_tpcc_status sto_tpcc_remove(sto_tpcc_thread *thread,
                                 const sto_tpcc_table *table,
                                 const uint8_t *key,
                                 size_t key_length) STO_TPCC_NOEXCEPT;

/* Bounds are bytewise lexicographic. Pointers for unbounded bounds are ignored. */
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
