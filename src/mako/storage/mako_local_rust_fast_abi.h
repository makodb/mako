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
 * bit 32 is one exactly when an OK put created the key; bits 33..63 are zero.
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
 * computes the exact cache-v3 record size, and seals the transaction against
 * later operations. It performs no allocation and reports an op count of zero
 * for a read-only or net-empty write set; the exact size is still the 30-byte
 * empty v3 framing, but that no-record case succeeds regardless of the cap.
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
 * exactly the sealed byte count, including the Mako timestamp and CRC-32C,
 * without allocation or I/O, before any write is installed. The buffer is
 * borrowed only for the synchronous fill and is never retained.
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
#define MAKO_RUST_FAST_TERMINAL_STATUS(result) ((int32_t)(uint32_t)(result))
#define MAKO_RUST_FAST_CLEANUP_STATUS(result)                                  \
  ((int32_t)(uint32_t)(((uint64_t)(result)) >> 32))

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

typedef int (*mako_rust_fast_record_bind_hook)(
    void *context, uint32_t mako_timestamp, size_t exact_record_bytes,
    uint64_t *sequence_out, uint8_t **record_bytes_out,
    size_t *record_capacity_out);

MAKO_RUST_FAST_HIDDEN int mako_rust_fast_txn_record_preflight(
    mako_local_txn *txn, size_t max_record_bytes,
    size_t *exact_record_bytes_out,
    uint32_t *op_count_out) MAKO_RUST_FAST_NOEXCEPT;
MAKO_RUST_FAST_HIDDEN uint64_t
mako_rust_fast_txn_commit_record_and_destroy(
    mako_local_txn *txn, mako_rust_fast_record_bind_hook bind_hook,
    void *context, uint8_t *record_written_out) MAKO_RUST_FAST_NOEXCEPT;
MAKO_RUST_FAST_HIDDEN uint64_t
mako_rust_fast_txn_commit_and_destroy(mako_local_txn *txn)
    MAKO_RUST_FAST_NOEXCEPT;
MAKO_RUST_FAST_HIDDEN uint64_t mako_rust_fast_txn_commit_with_hook_and_destroy(
    mako_local_txn *txn, mako_local_post_validate_hook hook,
    void *context) MAKO_RUST_FAST_NOEXCEPT;
MAKO_RUST_FAST_HIDDEN uint64_t
mako_rust_fast_txn_abort_and_destroy(mako_local_txn *txn)
    MAKO_RUST_FAST_NOEXCEPT;

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
#endif

#undef MAKO_RUST_FAST_HIDDEN
#undef MAKO_RUST_FAST_NOEXCEPT

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* MAKO_LOCAL_RUST_FAST_ABI_H */
