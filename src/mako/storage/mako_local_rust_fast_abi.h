#ifndef MAKO_LOCAL_RUST_FAST_ABI_H
#define MAKO_LOCAL_RUST_FAST_ABI_H

#include "mako_local_abi.h"

#ifdef __cplusplus
extern "C" {
#define MAKO_RUST_FAST_NOEXCEPT noexcept
#else
#define MAKO_RUST_FAST_NOEXCEPT
#endif

/* Private trusted Rust fast path. These mako_rust_fast_* entry points are not
 * part of revision 0, are not covered by its compatibility promise, and must
 * not be called by general C clients. The safe Rust wrapper proves the
 * thread, lifetime, slice, active-transaction, and bound-table invariants that
 * the hot put and consuming commit entries deliberately do not recheck in a
 * release build. Violating those private preconditions is undefined behavior;
 * debug builds retain assertions.
 *
 * begin validates the database/table relationship once and binds that table
 * into the pooled transaction facade. Public get/scan/insert/remove entry
 * points may still be used on the returned transaction; fast put always uses
 * the bound table. Key/value maxima and the weighted transaction budget remain
 * enforced natively. All entry points contain C++ exceptions before
 * returning.
 *
 * put has no output pointer. Its low 32 bits are the status bit pattern and
 * bit 32 is one exactly when an OK put created the key. Bits 33..63 carry the
 * exact unchecked-v4 record size when this is the transaction's one direct
 * canonical Put and zero otherwise. A later operation may retire that
 * candidate; callers must use the fused terminal below, which revalidates it
 * natively before acquiring write locks.
 * commit/abort consume the transaction pointer on every owner-thread call.
 * Their low 32 bits are the operation status and high 32 bits are the cleanup
 * status. A definite terminal outcome is recycled with cleanup status OK.
 * Cleanup uncertainty reports WORKER_POISONED in both halves and quarantines
 * the native facade instead of freeing storage that STO may still reference.
 * commit_and_destroy selects ordinary commit. The with_hook spelling requires
 * a non-null hook and has exactly the public commit_with_hook timing,
 * rejection, and exception-containment rules.
 *
 * The thin-record extension is likewise private. After all operations,
 * record_preflight walks STO's final normalized MassTrans write set directly,
 * computes the exact cache-record size, and seals the transaction against
 * later operations. The original spelling always selects the checksummed v3
 * format. The with_checksum spelling additionally accepts one of the explicit
 * MAKO_RUST_FAST_RECORD_CHECKSUM_* modes below: CRC32C produces v3, while NONE
 * produces self-describing v4 without a checksum trailer. It performs no
 * allocation and reports an op count of zero for a read-only or net-empty
 * write set; the exact diagnostic size is the selected format's empty framing,
 * but that no-record case succeeds regardless of the cap.
 * A cap rejection for a nonempty plan returns VALUE_TOO_LARGE and leaves the
 * transaction sealed for consuming abort.
 *
 * commit_record_and_destroy requires a successful, nonempty preflight. After
 * STO has locked the complete write set, a per-database native ticket gate
 * orders Mako timestamp assignment, repeated/final predicate validation,
 * point-read validation and bind_hook. The gate prevents a later
 * anti-dependent commit from binding an earlier dense sequence. It is retired
 * after an accepted binding; native then serializes the record while retaining
 * all write locks but allowing the next validation turn to proceed. On an
 * abort before binding, the turn is retired only after cleanup has released
 * native locks. General public hooks do not use this gate.
 *
 * Native code verifies the sealed scalar plan before invoking bind_hook. The
 * hook may bind one externally serialized dense sequence and returns stable
 * caller-owned storage. A true return must supply a nonzero sequence,
 * non-null buffer, and capacity at least exact_record_bytes. Native then fills
 * exactly the sealed byte count, including the Mako timestamp and, for v3,
 * CRC-32C, without allocation or I/O, before any write is installed. The
 * buffer is borrowed only for the synchronous fill and is never retained.
 *
 * record_written_out is zeroed before commit and becomes one only after every
 * record byte is initialized. Rust may publish a bound sequence only when that
 * witness is one. If binding assigned a sequence but later serialization left
 * the witness zero, Rust must pin that dense slot fail-closed even though
 * native definitely aborts before install; an unbound false/unreached hook
 * leaves no slot to pin. An internally inconsistent unbound terminal result
 * (for example success or a written witness without binding) is ABI corruption:
 * the Rust cache has no serialization position it can cover and must terminate
 * instead of admitting later work. As with the other terminal fast calls, the
 * low/high result halves are disposition/cleanup and the txn pointer is
 * consumed on every valid owner-thread call.
 *
 * commit_unchecked_one_put_record_and_destroy fuses NONE/v4 preflight with
 * the record terminal. The caller obtains expected_record_bytes from the
 * immediately preceding fast-put result and must reserve stable output
 * storage before this call. Native requires the direct one-Put witness to
 * still be current, derives its exact v4 shape again before taking write
 * locks, and requires an exact size match. A stale/malformed candidate, a
 * later read or mutation, or an already sealed plan definitely aborts without
 * invoking bind_hook. On a valid candidate it retains the same ordered
 * post-validation bind, serialization-before-install, and completion-witness
 * protocol as commit_record_and_destroy.
 *
 * commit_unchecked_one_put_record_single_producer_and_destroy is a still more
 * restricted opt-in spelling of that fused terminal. It preserves write-set
 * lock acquisition, Mako timestamp assignment, the repeated ordered predicate
 * checks, final point-read validation, bind_hook timing, and after-leave record
 * serialization before install. It bypasses only the per-database validation
 * ticket fetch/add and wait: the non-null gate callbacks retain Transaction's
 * validation and after-leave control flow without touching the ticket words.
 *
 * This entry point does not provide mutual exclusion. From before invocation
 * until it returns, the caller must guarantee that no other cache-record
 * commit terminal for txn's database is running or waiting, whether it uses
 * the ordinary concurrent gate or this single-producer spelling. In
 * particular, there must be no outstanding ticket already issued by a
 * concurrent record terminal. Sequential calls may freely alternate between
 * the two spellings. Violating this same-database exclusivity precondition is
 * undefined behavior: timestamp order and dense cache-sequence order can
 * diverge even though STO's data locks remain memory-safe. All other fused
 * one-Put preconditions and fail-closed terminal/witness rules above apply
 * unchanged.
 *
 * commit_preselected_unchecked_one_put_record_single_producer_and_destroy
 * removes the synchronous C-to-Rust bind callback from that same exclusive
 * profile. Before the call, Rust privately retains the exact next dense
 * sequence and its FREE arena generation without publishing either one.
 * sequence, record, and record_capacity must describe that stable exclusive
 * target for the whole call. Native revalidates and seals the one-Put shape,
 * retains a non-null no-ticket validation gate, and serializes the canonical
 * v4 record in an internal post-validation hook while still holding the
 * complete write set. Only after serialization succeeds can phase 3 install
 * a write.
 *
 * The two-word result keeps the ordinary packed terminal status in terminal.
 * record_state bits 0..31 contain the Mako timestamp exactly when the internal
 * hook accepted the record; bit 32 is one exactly when every record byte was
 * initialized; bits 33..63 are zero. A nonzero timestamp transfers an
 * unconditional obligation to Rust: it must publish the already-retained
 * sequence after this call even if terminal reports uncertainty or write-back
 * health changed concurrently. A zero timestamp leaves the invisible
 * reservation reusable. Success or a written witness with zero timestamp is
 * ABI corruption. The same whole-call database exclusion contract as the
 * callback-based single-producer terminal applies.
 *
 * The one-put holder extension removes the remaining value copy from that
 * callback-free profile. Rust owns an independent fixed-capacity holder pool
 * for exactly the lifetime of its write-back queue. Capacity is a nonzero
 * power of two, and sequence N exclusively leases holder
 * (N - 1) & (capacity - 1). The caller's queue-capacity/applied-tail proof
 * must prevent reuse until the consumer has called pool_release for the old
 * generation. key_reserve_bytes and value_reserve_bytes are optional cold
 * allocation hints; zero leaves each holder payload unallocated until used.
 *
 * The holder terminal preserves the transaction-owned encoded std::string as
 * STO's stable write target through validation and install. Once native has
 * accepted a Mako timestamp, it transfers that exact string allocation into
 * the preselected holder with a noexcept swap. Short keys use holder-inline
 * storage and are copied before validation; an OCC abort leaves those bytes
 * invisible and the retained generation immediately reusable. Long keys are
 * likewise staged before validation because their allocation can fail. The
 * post-validation hook therefore only captures the accepted timestamp.
 *
 * This symbol is a same-build unsafe terminal, not a checked holder API. The
 * immediately preceding fast Put's exact nonzero record-size witness, the
 * consuming transaction call, and the unique SPSC generation lease prove the
 * one-Put shape, stable spans, FREE target, power-of-two pool, and nonzero
 * sequence. Diagnostic builds assert those invariants; production deliberately
 * does not rederive the record shape or reread holder lifecycle state. A caller
 * which violates any of them has undefined behavior. Pool create/get/release
 * remain checked cold APIs. No foreground atomic read-modify-write protects
 * the holder: the single-producer lease is the ownership proof.
 *
 * The result uses the same two-word representation as the record terminal,
 * but bit 32 means that the holder is sealed. A nonzero timestamp always has
 * a sealed witness, including cleanup uncertainty, and unconditionally
 * transfers the sequence-publication obligation to Rust. A zero timestamp
 * leaves the slot reusable without pool_release. Background write-back may
 * call pool_get_view only after acquiring Rust's publication; returned spans
 * remain pool-owned and immutable until pool_release. Release must complete
 * before Rust release-publishes applied_tail, and producer reuse must follow
 * an acquire observation of that tail. Those cross-language happens-before
 * edges are required to make the deliberately non-atomic holder state and
 * payload access data-race-free. Pool destruction additionally requires
 * external quiescence and rejects any holder which remains sealed.
 *
 * The fused SPSC holder terminal extends that unsafe same-build contract
 * across the remaining Rust reservation/publication bookkeeping. A persistent
 * control block lends the queue-global holder pool, acknowledged frontier,
 * monotonic unhealthy flag, logical capacity, and record-size bound. Each
 * synchronous call passes the acknowledged and unhealthy pointers directly in
 * registers plus a Relaxed snapshot of the producer's exclusive capacity
 * limit. Both atomic pointers must be naturally aligned and must match the
 * stable addresses cached in the control. The capacity limit must be a
 * snapshot of applied_frontier.saturating_add(control.capacity) for this same
 * queue. It may lag the current frontier, which rejects capacity
 * conservatively, but must never exceed the limit derived from the current
 * frontier. Native Relaxed-loads ACK, Release-stores ACK after sealing the
 * holder, and Acquire-loads unhealthy through GCC/Clang __atomic operations.
 * It plain-writes control-owned cold_out only for the two consumed cold codes.
 * No terminal call or cold_out read may overlap another terminal using the
 * same control block.
 *
 * Before changing either the transaction or an external word, native checks
 * for a live direct one-Put candidate within max_record_bytes, an initially
 * healthy queue, a representable successor, and producer-local capacity. It
 * returns UNTOUCHED_NEED_GENERAL for a candidate/size miss and
 * UNTOUCHED_NEED_SLOW for a health/capacity miss; the latter carries the exact
 * candidate extent in the return payload. Both leave the transaction active,
 * the retained holder generation FREE, ACK unchanged, and cold_out untouched,
 * so Rust may synchronize its cold cursor, retry after refreshing capacity, or
 * select its general terminal.
 *
 * Otherwise native chooses acknowledged + 1 and runs the existing holder
 * terminal. A following unhealthy Acquire either diverts the sealed generation to
 * CONSUMED_COMMITTED_UNPUBLISHED, carrying its Mako timestamp, or permits an
 * acknowledged Release store and CONSUMED_PUBLISHED. Any other terminal result
 * copies the result to cold_out and returns CONSUMED_OUTCOME. Rust's cold
 * decoder synchronizes its producer-local cursor from ACK, or from ACK + 1 for
 * a committed-unpublished result, before it advances or pins an accepted
 * anomaly fail-closed. The transaction has been consumed for every CONSUMED_*
 * return.
 *
 * Future work: production representative PGO is complementary to this seam.
 * An ABI-only synthetic profile brought point-write throughput to within about
 * one percent of an unprofiled native control, mainly by deeply inlining put.
 * That comparison was an optimization ceiling, not fair closure: applying the
 * same representative profile to both paths retained about a ten-percent
 * dynamic-instruction gap. No generated profile is shippable yet because the
 * workload weights are synthetic and compiler/profile provenance is absent.
 * Reevaluate PGO on both native and trusted paths only with a reviewed
 * workload, pinned compiler, fail-closed stale-profile checks, and repeatable
 * provenance.
 */
#define MAKO_RUST_FAST_PUT_CREATED_BIT (UINT64_C(1) << 32)
#define MAKO_RUST_FAST_PUT_STATUS(result) ((int32_t)(uint32_t)(result))
#define MAKO_RUST_FAST_PUT_CREATED(result)                                     \
  ((uint8_t)((((uint64_t)(result)) >> 32) & UINT64_C(1)))
#define MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(result)                      \
  ((uint32_t)(((uint64_t)(result)) >> 33))
#define MAKO_RUST_FAST_TERMINAL_STATUS(result) ((int32_t)(uint32_t)(result))
#define MAKO_RUST_FAST_CLEANUP_STATUS(result)                                  \
  ((int32_t)(uint32_t)(((uint64_t)(result)) >> 32))

typedef struct mako_rust_fast_preselected_record_result {
  uint64_t terminal;
  uint64_t record_state;
} mako_rust_fast_preselected_record_result;

typedef struct mako_rust_fast_one_put_holder_pool
    mako_rust_fast_one_put_holder_pool;

/* Persistent queue-global inputs for the fused SPSC holder terminal. The
 * pointer targets and this control block must outlive every synchronous call
 * and its cold decode. Calls and cold decodes using one control must not
 * overlap because cold_out is non-atomic scratch owned by the unique producer.
 * holder_base and holder_mask are the immutable hot layout returned by
 * pool_get_hot_layout; caching them avoids a dependent pool lookup per ACK.
 * acknowledged names an aligned AtomicU64, unhealthy names an AtomicBool's
 * byte storage, capacity is the logical queue bound (not necessarily the
 * power-of-two holder-ring length), and reserved must be zero. Native
 * Relaxed-loads and Release-stores acknowledged, Acquire-loads unhealthy, and
 * writes cold_out only before returning code 3 or 4. */
typedef struct mako_rust_fast_spsc_holder_control {
  mako_rust_fast_one_put_holder_pool *pool;
  void *holder_base;
  size_t holder_mask;
  uint64_t *acknowledged;
  const uint8_t *unhealthy;
  uint64_t capacity;
  uint32_t max_record_bytes;
  uint32_t reserved;
  mako_rust_fast_preselected_record_result cold_out;
} mako_rust_fast_spsc_holder_control;

/* Snapshot of one sealed pool-owned holder. key and value remain valid and
 * immutable until the matching pool_release. value excludes STO's private
 * encoded-value trailer. reserved is always zero. */
typedef struct mako_rust_fast_one_put_holder_view {
  uint64_t sequence;
  uint64_t table_id;
  const uint8_t *key;
  const uint8_t *value;
  uint32_t key_len;
  uint32_t value_len;
  uint32_t mako_timestamp;
  uint32_t reserved;
} mako_rust_fast_one_put_holder_view;

#define MAKO_RUST_FAST_PRESELECTED_RECORD_TIMESTAMP(result)                    \
  ((uint32_t)((result).record_state))
#define MAKO_RUST_FAST_PRESELECTED_RECORD_WRITTEN(result)                      \
  ((uint8_t)((((result).record_state) >> 32) & UINT64_C(1)))
#define MAKO_RUST_FAST_PRESELECTED_HOLDER_SEALED(result)                       \
  MAKO_RUST_FAST_PRESELECTED_RECORD_WRITTEN(result)
#define MAKO_RUST_FAST_PRESELECTED_RECORD_RESERVED(result)                     \
  ((uint64_t)((result).record_state >> 33))

/* Register-sized fused-terminal control word. Low 32 bits select one explicit
 * lifecycle state. The high 32 bits are zero except for NEED_SLOW's exact
 * unchecked-v4 record extent and COMMITTED_UNPUBLISHED's accepted Mako
 * timestamp. Both consumed cold codes initialize cold_out. Its record_state
 * bits 33..63 carry native's exact extent; bits 0..32 retain the ordinary
 * timestamp/sealed state. Rust masks the extent before decoding the ordinary
 * holder outcome. No untouched or published code initializes cold_out. */
#define MAKO_RUST_FAST_FUSED_HOLDER_CONSUMED_PUBLISHED UINT32_C(0)
#define MAKO_RUST_FAST_FUSED_HOLDER_UNTOUCHED_NEED_GENERAL UINT32_C(1)
#define MAKO_RUST_FAST_FUSED_HOLDER_UNTOUCHED_NEED_SLOW UINT32_C(2)
#define MAKO_RUST_FAST_FUSED_HOLDER_CONSUMED_COMMITTED_UNPUBLISHED UINT32_C(3)
#define MAKO_RUST_FAST_FUSED_HOLDER_CONSUMED_OUTCOME UINT32_C(4)
#define MAKO_RUST_FAST_FUSED_HOLDER_CODE(result) ((uint32_t)(result))
#define MAKO_RUST_FAST_FUSED_HOLDER_PAYLOAD(result)                            \
  ((uint32_t)(((uint64_t)(result)) >> 32))

/* The default and legacy record-preflight spelling use CRC32C. NONE is an
 * explicitly unsafe durability/performance choice: v4 remains structurally
 * validated on replay but cannot detect arbitrary payload corruption. */
#define MAKO_RUST_FAST_RECORD_CHECKSUM_NONE UINT32_C(0)
#define MAKO_RUST_FAST_RECORD_CHECKSUM_CRC32C UINT32_C(1)

#if defined(__GNUC__) || defined(__clang__)
#define MAKO_RUST_FAST_HIDDEN __attribute__((visibility("hidden")))
#else
#define MAKO_RUST_FAST_HIDDEN
#endif

MAKO_RUST_FAST_HIDDEN int
mako_rust_fast_txn_begin(mako_local_db *db, mako_local_table *bound_table,
                         mako_local_txn **out) MAKO_RUST_FAST_NOEXCEPT;
MAKO_RUST_FAST_HIDDEN uint64_t mako_rust_fast_txn_put(
    mako_local_txn *txn, const uint8_t *key, uint32_t key_len,
    const uint8_t *value, uint32_t value_len) MAKO_RUST_FAST_NOEXCEPT;

typedef int (*mako_rust_fast_record_bind_hook)(void *context,
                                               uint32_t mako_timestamp,
                                               size_t exact_record_bytes,
                                               uint64_t *sequence_out,
                                               uint8_t **record_bytes_out,
                                               size_t *record_capacity_out);

MAKO_RUST_FAST_HIDDEN int mako_rust_fast_txn_record_preflight(
    mako_local_txn *txn, size_t max_record_bytes,
    size_t *exact_record_bytes_out,
    uint32_t *op_count_out) MAKO_RUST_FAST_NOEXCEPT;
MAKO_RUST_FAST_HIDDEN int mako_rust_fast_txn_record_preflight_with_checksum(
    mako_local_txn *txn, size_t max_record_bytes, uint32_t checksum_mode,
    size_t *exact_record_bytes_out,
    uint32_t *op_count_out) MAKO_RUST_FAST_NOEXCEPT;
MAKO_RUST_FAST_HIDDEN uint64_t mako_rust_fast_txn_commit_record_and_destroy(
    mako_local_txn *txn, mako_rust_fast_record_bind_hook bind_hook,
    void *context, uint8_t *record_written_out) MAKO_RUST_FAST_NOEXCEPT;
MAKO_RUST_FAST_HIDDEN uint64_t
mako_rust_fast_txn_commit_unchecked_one_put_record_and_destroy(
    mako_local_txn *txn, uint32_t expected_record_bytes,
    mako_rust_fast_record_bind_hook bind_hook, void *context,
    uint8_t *record_written_out) MAKO_RUST_FAST_NOEXCEPT;
MAKO_RUST_FAST_HIDDEN uint64_t
mako_rust_fast_txn_commit_unchecked_one_put_record_single_producer_and_destroy(
    mako_local_txn *txn, uint32_t expected_record_bytes,
    mako_rust_fast_record_bind_hook bind_hook, void *context,
    uint8_t *record_written_out) MAKO_RUST_FAST_NOEXCEPT;
MAKO_RUST_FAST_HIDDEN mako_rust_fast_preselected_record_result
mako_rust_fast_txn_commit_preselected_unchecked_one_put_record_single_producer_and_destroy(
    mako_local_txn *txn, uint32_t expected_record_bytes, uint64_t sequence,
    uint8_t *record, size_t record_capacity) MAKO_RUST_FAST_NOEXCEPT;
MAKO_RUST_FAST_HIDDEN int mako_rust_fast_one_put_holder_pool_create(
    size_t capacity, uint32_t key_reserve_bytes, uint32_t value_reserve_bytes,
    mako_rust_fast_one_put_holder_pool **out) MAKO_RUST_FAST_NOEXCEPT;
MAKO_RUST_FAST_HIDDEN int mako_rust_fast_one_put_holder_pool_destroy(
    mako_rust_fast_one_put_holder_pool *pool) MAKO_RUST_FAST_NOEXCEPT;
MAKO_RUST_FAST_HIDDEN int mako_rust_fast_one_put_holder_pool_get_hot_layout(
    mako_rust_fast_one_put_holder_pool *pool, void **holder_base_out,
    size_t *holder_mask_out) MAKO_RUST_FAST_NOEXCEPT;
MAKO_RUST_FAST_HIDDEN mako_rust_fast_preselected_record_result
mako_rust_fast_txn_commit_preselected_unchecked_one_put_holder_single_producer_and_destroy(
    mako_local_txn *txn, uint32_t expected_record_bytes,
    mako_rust_fast_one_put_holder_pool *pool,
    uint64_t sequence) MAKO_RUST_FAST_NOEXCEPT;
MAKO_RUST_FAST_HIDDEN uint64_t
mako_rust_fast_txn_try_commit_fused_one_put_holder_single_producer_and_destroy(
    mako_local_txn *txn, uint64_t *acknowledged, const uint8_t *unhealthy,
    mako_rust_fast_spsc_holder_control *control,
    uint64_t capacity_limit) MAKO_RUST_FAST_NOEXCEPT;
MAKO_RUST_FAST_HIDDEN int mako_rust_fast_one_put_holder_pool_get_view(
    const mako_rust_fast_one_put_holder_pool *pool, uint64_t expected_sequence,
    mako_rust_fast_one_put_holder_view *out) MAKO_RUST_FAST_NOEXCEPT;
MAKO_RUST_FAST_HIDDEN int mako_rust_fast_one_put_holder_pool_release(
    mako_rust_fast_one_put_holder_pool *pool,
    uint64_t expected_sequence) MAKO_RUST_FAST_NOEXCEPT;
MAKO_RUST_FAST_HIDDEN uint64_t mako_rust_fast_txn_commit_and_destroy(
    mako_local_txn *txn) MAKO_RUST_FAST_NOEXCEPT;
MAKO_RUST_FAST_HIDDEN uint64_t mako_rust_fast_txn_commit_with_hook_and_destroy(
    mako_local_txn *txn, mako_local_post_validate_hook hook,
    void *context) MAKO_RUST_FAST_NOEXCEPT;
MAKO_RUST_FAST_HIDDEN uint64_t mako_rust_fast_txn_abort_and_destroy(
    mako_local_txn *txn) MAKO_RUST_FAST_NOEXCEPT;

#if defined(MAKO_LOCAL_TEST_HOOKS)
/* Test-only observation of the number of record-validation tickets issued by
 * one database. A test can wait for a contender to enqueue before asserting
 * that it has not passed the current gate owner. */
MAKO_RUST_FAST_HIDDEN uint64_t
mako_rust_fast_test_record_validation_tickets(
    const mako_local_db *db) MAKO_RUST_FAST_NOEXCEPT;
MAKO_RUST_FAST_HIDDEN uint64_t
mako_rust_fast_test_record_validation_wait_observations(
    const mako_local_db *db) MAKO_RUST_FAST_NOEXCEPT;
/* Returns the exact transaction-owned encoded value allocation used by the
 * one-put holder transfer, excluding STO's trailer from length_out. */
MAKO_RUST_FAST_HIDDEN const uint8_t *
mako_rust_fast_test_txn_staged_one_put_value(
    const mako_local_txn *txn,
    uint32_t *length_out) MAKO_RUST_FAST_NOEXCEPT;
#endif

#undef MAKO_RUST_FAST_HIDDEN
#undef MAKO_RUST_FAST_NOEXCEPT

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* MAKO_LOCAL_RUST_FAST_ABI_H */
