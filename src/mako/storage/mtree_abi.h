/*
 * mtree_abi.h - stable C11 boundary for the Rust STO Masstree directory.
 *
 * The tree maps an arbitrary binary key to an immutable, nonzero RecordId.
 * Masstree nodes, values, cursors, and RCU guards never cross this boundary.
 */

#ifndef MAKO_STORAGE_MTREE_ABI_H
#define MAKO_STORAGE_MTREE_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define MT_NOEXCEPT noexcept
extern "C" {
#else
#define MT_NOEXCEPT
#endif

#define MT_ABI_VERSION UINT32_C(1)
#define MT_RECORD_ID_NONE UINT64_C(0)

/* The CMake Masstree configuration is deliberately capped at 1024 bytes. */
#define MT_CONFIGURED_MAX_KEY_LENGTH ((size_t)1024)

typedef struct mt_runtime mt_runtime;
typedef struct mt_thread mt_thread;
typedef struct mt_tree mt_tree;
typedef uint64_t mt_record_id;

/* Fixed-width status representation; values are part of ABI version 1. */
typedef int32_t mt_status;
enum {
  MT_OK = 0,
  MT_ERR_INVALID = 1,
  MT_ERR_KEY_TOO_LARGE = 2,
  MT_ERR_BUFFER_TOO_SMALL = 3,
  MT_ERR_NOT_ATTACHED = 4,
  MT_ERR_WRONG_THREAD = 5,
  MT_ERR_WRONG_RUNTIME = 6,
  MT_ERR_THREAD_LIMIT = 7,
  MT_ERR_OUT_OF_MEMORY = 8,
  MT_ERR_BUSY = 9,
  MT_ERR_ACTIVE_GUARDS = 10,
  MT_ERR_ABI_MISMATCH = 11,
  MT_ERR_CPP_EXCEPTION = 12,
  MT_ERR_INTERNAL = 13,
  MT_ERR_UNSUPPORTED = 14,
  MT_ERR_INCOMPATIBLE_RUNTIME = 15,
  MT_ERR_POISONED = 16,
  MT_ERR_CLOSED = 17
};

typedef uint64_t mt_feature_set;
#define MT_FEATURE_POINT_GET (UINT64_C(1) << 0)
#define MT_FEATURE_ATOMIC_GET_OR_INSERT (UINT64_C(1) << 1)
#define MT_FEATURE_EXPLICIT_HANDLES (UINT64_C(1) << 2)
#define MT_FEATURE_BINARY_KEYS (UINT64_C(1) << 3)
#define MT_FEATURE_INTEGRAL_RECORD_IDS (UINT64_C(1) << 4)
#define MT_FEATURE_RUNTIME_HEALTH (UINT64_C(1) << 5)
#define MT_FEATURE_SINGLETON_RUNTIME (UINT64_C(1) << 6)
#define MT_FEATURE_GRACEFUL_SHUTDOWN (UINT64_C(1) << 7)
#define MT_FEATURE_COPIED_RANGE_SCANS (UINT64_C(1) << 8)
#define MT_FEATURE_SCOPED_POINT_READS (UINT64_C(1) << 9)
#define MT_FEATURE_SCOPED_STRIDED_POINT_READS (UINT64_C(1) << 10)
#define MT_FEATURE_STRIDED_POINT_READS (UINT64_C(1) << 11)

typedef uint32_t mt_byte_order;
enum {
  MT_BYTE_ORDER_UNKNOWN = 0,
  MT_BYTE_ORDER_LITTLE_ENDIAN = 1,
  MT_BYTE_ORDER_BIG_ENDIAN = 2
};

typedef uint32_t mt_runtime_health_state;
enum { MT_RUNTIME_HEALTHY = 1, MT_RUNTIME_POISONED = 2 };

/*
 * A zero max_threads/max_key_length requests the native default. A nonzero
 * value may lower, but never raise, the corresponding native limit. Reserved
 * words must be zero. The first successful acquisition fixes these limits;
 * later incompatible acquisitions are rejected.
 */
typedef struct mt_runtime_config {
  uint32_t struct_size;
  uint32_t abi_version;
  mt_feature_set required_features;
  uint32_t max_threads;
  uint32_t max_key_length;
  uint64_t reserved[2];
} mt_runtime_config;

typedef struct mt_build_id {
  uint64_t low;
  uint64_t high;
} mt_build_id;

/*
 * Generation-tagged capability for one worker-affine, one-tree point-read
 * scope. Both words are implementation-owned. Callers must initialize a token
 * only through mt_read_scope_begin and must not modify or reuse it after a
 * successful mt_read_scope_end.
 */
typedef struct mt_read_scope {
  uintptr_t owner;
  uint64_t generation;
} mt_read_scope;

typedef uint32_t mt_publication_disposition;
enum {
  MT_PUBLICATION_FAILURE_BEFORE_PUBLICATION = 1,
  MT_PUBLICATION_CANDIDATE_INSERTED = 2,
  MT_PUBLICATION_CANDIDATE_PROVEN_UNPUBLISHED = 3,
  MT_PUBLICATION_UNKNOWN = 4
};

/*
 * winner is zero unless a winner is known. inserted is exactly 0 or 1.
 * reserved bytes are always zeroed by the implementation.
 */
typedef struct mt_get_or_insert_result {
  mt_record_id winner;
  mt_publication_disposition publication;
  uint8_t inserted;
  uint8_t reserved[3];
} mt_get_or_insert_result;

typedef uint32_t mt_scan_bound_kind;
enum {
  MT_SCAN_BOUND_ABSENT = 0,
  MT_SCAN_BOUND_INCLUSIVE = 1,
  MT_SCAN_BOUND_EXCLUSIVE = 2
};

/*
 * ABSENT requires key == NULL, key_length == 0, and denotes no bound.
 * INCLUSIVE/EXCLUSIVE make the bound present; a null key is then accepted
 * exactly when key_length is zero, so a present empty key is unambiguous.
 * reserved must be zero.
 */
typedef struct mt_scan_bound {
  const void *key;
  size_t key_length;
  mt_scan_bound_kind kind;
  uint32_t reserved;
} mt_scan_bound;

typedef uint32_t mt_scan_direction;
enum { MT_SCAN_FORWARD = 1, MT_SCAN_REVERSE = 2 };

/* Key bytes are copied into the caller's arena; no native pointer escapes. */
typedef struct mt_scan_entry {
  size_t key_offset;
  size_t key_length;
  mt_record_id record_id;
} mt_scan_entry;

typedef uint32_t mt_scan_stop_reason;
enum {
  MT_SCAN_STOP_END = 1,
  MT_SCAN_STOP_ENTRY_CAPACITY = 2,
  MT_SCAN_STOP_KEY_ARENA_CAPACITY = 3
};

typedef uint32_t mt_scan_resume_kind;
enum {
  MT_SCAN_RESUME_NONE = 0,
  MT_SCAN_RESUME_UNCHANGED_INPUT = 1,
  MT_SCAN_RESUME_EXCLUSIVE_LAST = 2
};

/*
 * On successful capacity stops, next_key_bytes_required is the exact length
 * of the first qualifying key that was not copied. If no entry was copied,
 * resume is UNCHANGED_INPUT: grow the limiting buffer and repeat the same
 * call. If progress was made, resume is EXCLUSIVE_LAST and
 * resume_key_offset/resume_key_length identify the last copied key in the
 * arena. Replace the lower bound (forward) or upper bound (reverse) with that
 * exclusive key and leave the opposite input bound unchanged.
 *
 * At END, resume is NONE and all resume/next-key fields are zero. Reserved
 * words are always zero. Entries and arena bytes beyond the reported counts
 * are unspecified.
 */
typedef struct mt_scan_result {
  size_t entries_written;
  size_t arena_bytes_used;
  size_t next_key_bytes_required;
  mt_scan_stop_reason stop_reason;
  mt_scan_resume_kind resume;
  size_t resume_key_offset;
  size_t resume_key_length;
  uint64_t reserved[2];
} mt_scan_result;

/* Build and ABI identity. These functions do not acquire a runtime. */
uint32_t mt_abi_version(void) MT_NOEXCEPT;
mt_feature_set mt_feature_bits(void) MT_NOEXCEPT;
mt_byte_order mt_endianness(void) MT_NOEXCEPT;
uint32_t mt_pointer_width(void) MT_NOEXCEPT;
size_t mt_max_key_length(void) MT_NOEXCEPT;
uint32_t mt_max_threads(void) MT_NOEXCEPT;
mt_record_id mt_record_id_limit(void) MT_NOEXCEPT;
size_t mt_runtime_config_size(void) MT_NOEXCEPT;
size_t mt_runtime_config_alignment(void) MT_NOEXCEPT;
size_t mt_build_id_size(void) MT_NOEXCEPT;
size_t mt_build_id_alignment(void) MT_NOEXCEPT;
size_t mt_read_scope_size(void) MT_NOEXCEPT;
size_t mt_read_scope_alignment(void) MT_NOEXCEPT;
size_t mt_get_or_insert_result_size(void) MT_NOEXCEPT;
size_t mt_get_or_insert_result_alignment(void) MT_NOEXCEPT;
size_t mt_scan_bound_size(void) MT_NOEXCEPT;
size_t mt_scan_bound_alignment(void) MT_NOEXCEPT;
size_t mt_scan_entry_size(void) MT_NOEXCEPT;
size_t mt_scan_entry_alignment(void) MT_NOEXCEPT;
size_t mt_scan_result_size(void) MT_NOEXCEPT;
size_t mt_scan_result_alignment(void) MT_NOEXCEPT;
uint64_t mt_exported_symbols_fingerprint(void) MT_NOEXCEPT;
mt_status mt_get_build_fingerprint(mt_build_id *out) MT_NOEXCEPT;
mt_status mt_runtime_config_init(mt_runtime_config *out) MT_NOEXCEPT;

/* Explicit singleton-runtime lifecycle and diagnostics. */
mt_status mt_runtime_acquire(const mt_runtime_config *config,
                             mt_runtime **out) MT_NOEXCEPT;
mt_status mt_runtime_health(const mt_runtime *runtime,
                            mt_runtime_health_state *out) MT_NOEXCEPT;
mt_status mt_runtime_max_key_length(const mt_runtime *runtime,
                                    size_t *out) MT_NOEXCEPT;
mt_status mt_runtime_max_threads(const mt_runtime *runtime,
                                 uint32_t *out) MT_NOEXCEPT;

/*
 * The inherited native implementation cannot safely tear down or recycle
 * global RCU/core-ID state. A valid request therefore returns UNSUPPORTED.
 */
mt_status mt_runtime_shutdown(mt_runtime *runtime,
                              mt_thread *shutdown_thread) MT_NOEXCEPT;

/* Worker handles are fixed, long-lived, and bound to the attaching OS thread.
 */
mt_status mt_thread_attach(mt_runtime *runtime, mt_thread **out) MT_NOEXCEPT;
mt_status mt_thread_quiesce(mt_thread *thread) MT_NOEXCEPT;

/*
 * Tree storage lives for the process. release only closes the facade handle;
 * it never performs thread-affine native destruction.
 */
mt_status mt_tree_create(mt_runtime *runtime, mt_thread *thread,
                         mt_tree **out) MT_NOEXCEPT;
mt_status mt_tree_release(mt_tree *tree) MT_NOEXCEPT;

/*
 * Keys are binary. A null key pointer is accepted only when key_length is 0;
 * a nonnull pointer with length 0 denotes the same valid empty key.
 * Absence is reported as MT_RECORD_ID_NONE with MT_OK.
 */
mt_status mt_get(mt_tree *tree, mt_thread *thread, const void *key,
                 size_t key_length, mt_record_id *out) MT_NOEXCEPT;

/*
 * Looks up key_count fixed-length keys in one operation. Handles, worker
 * affinity, runtime health, and the common key shape are validated once. For
 * a nonempty batch, one structural-reader admission and one native RCU region
 * cover the complete lookup loop and both are released before return. Unlike
 * mt_read_scope_get_strided, this call creates no persistent scope token and
 * leaves the worker immediately available for another ordinary operation.
 *
 * The first key begins at keys and each later key begins key_stride bytes
 * after the preceding one; key_stride must be at least key_length. The caller
 * must provide readable storage through the final key and exactly key_count
 * writable output elements; the key and output regions must not overlap.
 * Every output is initialized to MT_RECORD_ID_NONE before handle or key
 * validation and is reset to MT_RECORD_ID_NONE on failure. For key_count == 0,
 * keys and out may be null, but handles and the common key shape are still
 * validated.
 */
mt_status mt_get_strided(mt_tree *tree, mt_thread *thread, const void *keys,
                         size_t key_count, size_t key_length, size_t key_stride,
                         mt_record_id *out) MT_NOEXCEPT;

/*
 * Amortizes tree/thread validation, structural-reader admission, and native
 * RCU protection across multiple point lookups. A worker may own at most one
 * active scope, and that scope is bound to exactly one tree. Scoped reads are
 * not a snapshot: append-only directory publication may occur before begin or
 * after end, while publication on this tree waits for the scope to end.
 *
 * While a scope is active, ordinary operations using the same worker fail with
 * MT_ERR_ACTIVE_GUARDS. In particular, end the scope before mt_get_or_insert,
 * including after a miss. Keep the scope synchronous and short; do not retain
 * it across I/O, blocking waits, or reentrant native work. The ABI cannot
 * enforce that progress contract for a C caller. mt_read_scope_end invalidates
 * token even when the tree facade was concurrently closed or the runtime
 * became poisoned.
 */
mt_status mt_read_scope_begin(mt_tree *tree, mt_thread *thread,
                              mt_read_scope *token) MT_NOEXCEPT;
mt_status mt_read_scope_get(const mt_read_scope *token, const void *key,
                            size_t key_length, mt_record_id *out) MT_NOEXCEPT;

/*
 * Looks up key_count fixed-length keys in one validated scope boundary. The
 * first key begins at keys and each later key begins key_stride bytes after
 * the preceding one; key_stride must be at least key_length. This permits
 * both tightly packed key arrays and keys embedded at the same offset in a
 * fixed-size C record. The caller must provide readable storage through the
 * final key and exactly key_count writable output elements; the key and
 * output regions must not overlap.
 *
 * Token affinity, tree liveness, runtime health, and the common key shape are
 * checked once per call. Every output is initialized to MT_RECORD_ID_NONE
 * before native lookup. On success, absence remains MT_RECORD_ID_NONE; on
 * failure, all outputs are reset to MT_RECORD_ID_NONE. For key_count == 0,
 * keys and out may be null, but token and the common key shape are still
 * validated.
 */
mt_status mt_read_scope_get_strided(const mt_read_scope *token,
                                    const void *keys, size_t key_count,
                                    size_t key_length, size_t key_stride,
                                    mt_record_id *out) MT_NOEXCEPT;
mt_status mt_read_scope_end(mt_read_scope *token) MT_NOEXCEPT;

/*
 * Atomically publishes candidate when the key is absent and always reports
 * whether that candidate was inserted, proved unpublished, or may have been
 * published. candidate must be nonzero. The whole call owns exclusive native
 * structural access; concurrent point reads and scans drain before it enters
 * Masstree.
 */
mt_status mt_get_or_insert(mt_tree *tree, mt_thread *thread, const void *key,
                           size_t key_length, mt_record_id candidate,
                           mt_get_or_insert_result *out) MT_NOEXCEPT;

/*
 * Copies one bounded ascending or descending chunk into caller storage.
 * Point reads and scans may proceed concurrently. Each publishes structural
 * read activity in a cacheline-private worker slot rather than updating one
 * shared reader counter. get-or-insert excludes and drains those readers, so a
 * call never overlaps Masstree's plain structural writes. A single scan chunk
 * therefore sees stable native structure; insertion may occur between scan
 * calls.
 * `lower` and `upper` are required pointers; use MT_SCAN_BOUND_ABSENT for an
 * unbounded side. `entries`/`key_arena` may be null exactly when their
 * respective capacities are zero. `out` is required.
 *
 * Once all conditionally required output pointers are known nonnull, `out` is
 * initialized before handles, direction, bounds, lengths, or reserved fields
 * are validated. The bridge invokes only its own allocation-free collector
 * while native RCU is held.
 */
mt_status mt_scan(mt_tree *tree, mt_thread *thread, mt_scan_direction direction,
                  const mt_scan_bound *lower, const mt_scan_bound *upper,
                  mt_scan_entry *entries, size_t entry_capacity,
                  void *key_arena, size_t key_arena_capacity,
                  mt_scan_result *out) MT_NOEXCEPT;

#ifdef __cplusplus
} /* extern "C" */
#endif

#undef MT_NOEXCEPT

#endif /* MAKO_STORAGE_MTREE_ABI_H */
