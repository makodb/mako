// mako_local_abi.cc - exception-contained C facade over local STO/MassTrans.

#include "storage/mako_local_rust_fast_abi.h"

#include "lib/common.h"
#include "sto/MassTrans.hh"
#include "sto/StringWrapper.hh"
#include "sto/thread_registration.hh"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#if !READ_MY_WRITES
#include <unordered_set>
#endif

#if defined(__i386__) || defined(__x86_64__)
#include <nmmintrin.h>
#endif

#if STO_OPACITY
using mako_local_table_impl =
    MassTrans<std::string, versioned_str_struct, true /* opacity */>;
#else
using mako_local_table_impl =
    MassTrans<std::string, versioned_str_struct, false /* opacity */>;
#endif

constexpr size_t kMinimumWriteItemCharge = 4;
constexpr size_t kMaximumEncodedValues =
    MAKO_LOCAL_TXN_ITEM_BUDGET / kMinimumWriteItemCharge;
constexpr size_t kMaximumRetainedEncodedValueCapacity = 512;
constexpr size_t kMaximumRetainedEncodedValueBytes = 64 * 1024;
static_assert(kMaximumEncodedValues * kMinimumWriteItemCharge ==
              MAKO_LOCAL_TXN_ITEM_BUDGET);
static_assert(kMaximumEncodedValues * kMaximumRetainedEncodedValueCapacity ==
              kMaximumRetainedEncodedValueBytes);

struct mako_local_table {
  mako_local_db *owner;
  mako_local_table_impl *table;
  uint64_t id;
};

struct alignas(64) record_validation_counter {
  std::atomic<uint64_t> value{0};
};
static_assert(sizeof(record_validation_counter) == 64);

struct mako_local_db {
  // Record commits for one cache/database share this allocation-free ticket
  // gate. The turn is acquired only after STO owns the full write set, so a
  // waiter never holds the turn while acquiring another transaction lock.
  // Isolate the producer and consumer words: enqueue fetch_add traffic must
  // not invalidate the cache line polled by every waiter.
  record_validation_counter record_validation_next;
  record_validation_counter record_validation_serving;
#if defined(MAKO_LOCAL_TEST_HOOKS)
  std::atomic<uint64_t> record_validation_wait_observations{0};
#endif
  std::mutex tables_mu;
  std::unordered_map<std::string, std::unique_ptr<mako_local_table>> tables;
};

struct mako_local_txn {
  mako_local_db *owner;
  // Non-null only for a live transaction created through the private Rust
  // fast begin. The public point APIs ignore it; fast put treats the binding
  // and its lifetime as a trusted Rust invariant.
  mako_local_table_impl *fast_table_impl;
  size_t worker_slot;
  size_t item_budget_used;
  size_t encoded_values_used;
  size_t record_plan_bytes;
  uint32_t record_plan_ops;
  // Keep the booleans together so adding the private binding does not move the
  // hot stable-value pool or add padding to every pooled facade.
  bool active;
  bool poisoned;
  bool encoded_values_require_release;
  bool record_plan_sealed;
  bool record_plan_ready;

  // StringWrapper retains the std::string object's address until terminal
  // native cleanup. Every write consumes at least four item-budget credits,
  // so this fixed pool is large enough for every legal transaction and never
  // moves an earlier slot while a later write is staged. Retaining at most
  // 512 bytes of capacity in each slot bounds warmed payload storage to 64 KiB
  // while eliminating allocation churn for up to 128 short values.
  std::array<std::string, kMaximumEncodedValues> encoded_values;
#if !READ_MY_WRITES
  std::unordered_map<mako_local_table_impl *, std::unordered_set<std::string>>
      mutated_keys;
#endif
};

namespace {

static_assert(MAKO_LOCAL_MAX_MAKO_TIMESTAMP ==
              Transaction::max_mako_timestamp);
static_assert(MAKO_LOCAL_MAX_WORKERS == MAX_THREADS);

thread_local bool local_attached = false;
thread_local mako_local_txn *local_active_txn = nullptr;
thread_local bool local_worker_poisoned = false;
std::atomic<uint64_t> quarantined_worker_count{0};

// One worker can own at most one ambient STO transaction, regardless of the
// database facade it uses. Publish that database in a worker-private cache
// line so db_close can retain its BUSY diagnostic without making every
// transaction bounce one database-wide atomic counter between cores.
//
// db_close is still externally quiesced by contract; the atomics make its
// diagnostic observation well-defined, but do not turn close into a lifetime
// barrier against a concurrent new begin.
constexpr size_t kCacheLineBytes = 64;
struct alignas(kCacheLineBytes) active_database_slot {
  std::atomic<mako_local_db *> database{nullptr};
  // Only the process-lifetime owner of this non-recycled STO worker ID may
  // touch spare. Keeping it here avoids a destructible internal TLS object:
  // a caller's TLS transaction wrapper may run its destructor later during
  // thread exit and must still be able to return a finished facade safely.
  mako_local_txn *spare = nullptr;
};
static_assert(sizeof(active_database_slot) == kCacheLineBytes);
static_assert(MAKO_LOCAL_MAX_WORKERS <= MAX_THREADS);
std::array<active_database_slot, MAKO_LOCAL_MAX_WORKERS>
    active_database_slots{};

enum class cleanup_boundary : uint32_t {
  begin = MAKO_LOCAL_CLEANUP_BOUNDARY_BEGIN,
  operation = MAKO_LOCAL_CLEANUP_BOUNDARY_OPERATION,
  commit = MAKO_LOCAL_CLEANUP_BOUNDARY_COMMIT,
  abort = MAKO_LOCAL_CLEANUP_BOUNDARY_ABORT,
  destroy = MAKO_LOCAL_CLEANUP_BOUNDARY_DESTROY,
};

#if defined(MAKO_LOCAL_TEST_HOOKS)
thread_local uint32_t local_test_cleanup_failure = 0;

bool cleanup_failure_armed(cleanup_boundary boundary) noexcept {
  return local_test_cleanup_failure == static_cast<uint32_t>(boundary);
}

bool arm_native_cleanup_failure_if_requested(
    cleanup_boundary boundary) noexcept {
  if (!cleanup_failure_armed(boundary)) return false;
  local_test_cleanup_failure = 0;
  Transaction::test_fail_next_cleanup();
  return true;
}

class operation_cleanup_failure_scope {
 public:
  operation_cleanup_failure_scope() noexcept
      : armed_(arm_native_cleanup_failure_if_requested(
            cleanup_boundary::operation)) {}

  ~operation_cleanup_failure_scope() {
    if (armed_ && Transaction::test_cancel_fail_next_cleanup()) {
      // No native stop() was entered. Restore the public boundary arm so a
      // later operation that actually cleans up remains the matching call.
      local_test_cleanup_failure =
          static_cast<uint32_t>(cleanup_boundary::operation);
    }
  }

  operation_cleanup_failure_scope(const operation_cleanup_failure_scope &) =
      delete;
  operation_cleanup_failure_scope &operator=(
      const operation_cleanup_failure_scope &) = delete;

 private:
  bool armed_;
};
#endif

bool valid_slice(const uint8_t *p, size_t n) { return p != nullptr || n == 0; }

constexpr uint64_t pack_fast_put_result(int status,
                                        bool created = false) noexcept {
  return static_cast<uint64_t>(static_cast<uint32_t>(status)) |
         (created ? MAKO_RUST_FAST_PUT_CREATED_BIT : UINT64_C(0));
}

constexpr uint64_t pack_fast_terminal_result(int status,
                                             int cleanup_status) noexcept {
  return static_cast<uint64_t>(static_cast<uint32_t>(status)) |
         (static_cast<uint64_t>(static_cast<uint32_t>(cleanup_status)) << 32);
}

static_assert(MAKO_RUST_FAST_PUT_STATUS(pack_fast_put_result(
                  MAKO_LOCAL_VALUE_TOO_LARGE)) == MAKO_LOCAL_VALUE_TOO_LARGE);
static_assert(MAKO_RUST_FAST_PUT_CREATED(pack_fast_put_result(MAKO_LOCAL_OK,
                                                              true)) == 1);
static_assert(MAKO_RUST_FAST_TERMINAL_STATUS(pack_fast_terminal_result(
                  MAKO_LOCAL_COMMIT_HOOK_REJECTED, MAKO_LOCAL_OK)) ==
              MAKO_LOCAL_COMMIT_HOOK_REJECTED);
static_assert(MAKO_RUST_FAST_CLEANUP_STATUS(pack_fast_terminal_result(
                  MAKO_LOCAL_OK, MAKO_LOCAL_WORKER_POISONED)) ==
              MAKO_LOCAL_WORKER_POISONED);

constexpr std::array<uint8_t, 8> kCacheRecordMagic{
    'M', 'A', 'K', 'O', 'C', 'M', 'T', '\0'};
constexpr uint16_t kCacheRecordVersion = 3;
constexpr uint8_t kCacheRecordPutTag = 1;
constexpr uint8_t kCacheRecordDeleteTag = 2;
constexpr size_t kCacheRecordHeaderBytes = 8 + 2 + 8 + 4 + 4;
constexpr size_t kCacheRecordOperationHeaderBytes = 1 + 8 + 4 + 4;
constexpr size_t kCacheRecordCrcBytes = 4;
constexpr size_t kCacheRecordMinimumBytes =
    kCacheRecordHeaderBytes + kCacheRecordCrcBytes;
constexpr uint32_t kReversedCastagnoli = UINT32_C(0x82f63b78);

constexpr uint32_t make_crc32c_entry(uint32_t value) noexcept {
  for (unsigned bit = 0; bit != 8; ++bit) {
    const uint32_t low_mask = UINT32_C(0) - (value & UINT32_C(1));
    value = (value >> 1) ^ (kReversedCastagnoli & low_mask);
  }
  return value;
}

constexpr std::array<std::array<uint32_t, 256>, 8>
make_crc32c_slicing_tables() noexcept {
  std::array<std::array<uint32_t, 256>, 8> tables{};
  for (size_t value = 0; value != 256; ++value)
    tables[0][value] = make_crc32c_entry(static_cast<uint32_t>(value));
  for (size_t slice = 1; slice != tables.size(); ++slice) {
    for (size_t value = 0; value != 256; ++value) {
      const uint32_t prior = tables[slice - 1][value];
      tables[slice][value] =
          (prior >> 8) ^ tables[0][prior & UINT32_C(0xff)];
    }
  }
  return tables;
}

constexpr auto kCrc32cSlicingTables = make_crc32c_slicing_tables();

uint32_t crc32c_slicing_by_8(const uint8_t *bytes, size_t length) noexcept {
  uint32_t crc = UINT32_MAX;
  while (length >= 8) {
    uint64_t block = 0;
    for (unsigned byte = 0; byte != 8; ++byte)
      block |= static_cast<uint64_t>(bytes[byte]) << (byte * 8);
    block ^= crc;
    crc = kCrc32cSlicingTables[7][block & UINT64_C(0xff)] ^
          kCrc32cSlicingTables[6][(block >> 8) & UINT64_C(0xff)] ^
          kCrc32cSlicingTables[5][(block >> 16) & UINT64_C(0xff)] ^
          kCrc32cSlicingTables[4][(block >> 24) & UINT64_C(0xff)] ^
          kCrc32cSlicingTables[3][(block >> 32) & UINT64_C(0xff)] ^
          kCrc32cSlicingTables[2][(block >> 40) & UINT64_C(0xff)] ^
          kCrc32cSlicingTables[1][(block >> 48) & UINT64_C(0xff)] ^
          kCrc32cSlicingTables[0][(block >> 56) & UINT64_C(0xff)];
    bytes += 8;
    length -= 8;
  }
  while (length-- != 0)
    crc = (crc >> 8) ^ kCrc32cSlicingTables[0][(crc ^ *bytes++) & 0xff];
  return ~crc;
}

#if defined(__i386__) || defined(__x86_64__)
__attribute__((target("sse4.2")))
uint32_t crc32c_sse42(const uint8_t *bytes, size_t length) noexcept {
#if defined(__x86_64__)
  uint64_t crc = UINT32_MAX;
  while (length >= sizeof(uint64_t)) {
    uint64_t word;
    std::memcpy(&word, bytes, sizeof(word));
    crc = _mm_crc32_u64(crc, word);
    bytes += sizeof(word);
    length -= sizeof(word);
  }
#else
  uint32_t crc = UINT32_MAX;
  while (length >= sizeof(uint32_t)) {
    uint32_t word;
    std::memcpy(&word, bytes, sizeof(word));
    crc = _mm_crc32_u32(crc, word);
    bytes += sizeof(word);
    length -= sizeof(word);
  }
#endif
  while (length-- != 0)
    crc = _mm_crc32_u8(static_cast<uint32_t>(crc), *bytes++);
  return ~static_cast<uint32_t>(crc);
}
#endif

uint32_t crc32c(const uint8_t *bytes, size_t length) noexcept {
#if defined(__i386__) || defined(__x86_64__)
  if (__builtin_cpu_supports("sse4.2"))
    return crc32c_sse42(bytes, length);
#endif
  return crc32c_slicing_by_8(bytes, length);
}

void write_u16_be(uint8_t *&cursor, uint16_t value) noexcept {
  *cursor++ = static_cast<uint8_t>(value >> 8);
  *cursor++ = static_cast<uint8_t>(value);
}

void write_u32_be(uint8_t *&cursor, uint32_t value) noexcept {
  for (int shift = 24; shift >= 0; shift -= 8)
    *cursor++ = static_cast<uint8_t>(value >> shift);
}

void write_u64_be(uint8_t *&cursor, uint64_t value) noexcept {
  for (int shift = 56; shift >= 0; shift -= 8)
    *cursor++ = static_cast<uint8_t>(value >> shift);
}

struct record_shape {
  size_t bytes = kCacheRecordMinimumBytes;
  uint32_t operations = 0;
};

bool add_record_shape(
    void *opaque, const Transaction::canonical_write_view &write) noexcept {
  auto *shape = static_cast<record_shape *>(opaque);
  if (write.key_length > UINT32_MAX || write.value_length > UINT32_MAX)
    return false;
  if (shape->bytes > SIZE_MAX - kCacheRecordOperationHeaderBytes)
    return false;
  size_t next = shape->bytes + kCacheRecordOperationHeaderBytes;
  if (write.key_length > SIZE_MAX - next)
    return false;
  next += write.key_length;
  if (write.value_length > SIZE_MAX - next)
    return false;
  shape->bytes = next + write.value_length;
  return true;
}

bool derive_record_shape(const Transaction *native_txn,
                         record_shape *shape) noexcept {
  if (native_txn == nullptr || shape == nullptr) return false;
  *shape = record_shape{};
  uint32_t visited = 0;
  if (!native_txn->visit_local_canonical_writes(
          add_record_shape, shape, &visited))
    return false;
  shape->operations = visited;
  return true;
}

struct record_writer {
  uint8_t *cursor;
  uint8_t *operations_end;
};

bool write_record_mutation(
    void *opaque, const Transaction::canonical_write_view &write) noexcept {
  auto *writer = static_cast<record_writer *>(opaque);
  const size_t required = kCacheRecordOperationHeaderBytes +
                          write.key_length + write.value_length;
  if (writer->cursor > writer->operations_end ||
      required > static_cast<size_t>(writer->operations_end - writer->cursor))
    return false;

  *writer->cursor++ =
      write.op == Transaction::canonical_write_view::operation::put
          ? kCacheRecordPutTag
          : kCacheRecordDeleteTag;
  write_u64_be(writer->cursor, write.table_id);
  write_u32_be(writer->cursor, static_cast<uint32_t>(write.key_length));
  write_u32_be(writer->cursor, static_cast<uint32_t>(write.value_length));
  if (write.key_length != 0) {
    std::memcpy(writer->cursor, write.key, write.key_length);
    writer->cursor += write.key_length;
  }
  if (write.value_length != 0) {
    std::memcpy(writer->cursor, write.value, write.value_length);
    writer->cursor += write.value_length;
  }
  return true;
}

bool serialize_cache_record(const Transaction *native_txn,
                            uint64_t sequence, uint32_t mako_timestamp,
                            const record_shape &shape,
                            uint8_t *record) noexcept {
  assert(native_txn != nullptr);
  assert(sequence != 0);
  assert(mako_timestamp != 0);
  assert(shape.operations != 0);
  assert(shape.bytes >= kCacheRecordMinimumBytes);
  assert(record != nullptr);

  uint8_t *cursor = record;
  std::memcpy(cursor, kCacheRecordMagic.data(), kCacheRecordMagic.size());
  cursor += kCacheRecordMagic.size();
  write_u16_be(cursor, kCacheRecordVersion);
  write_u64_be(cursor, sequence);
  write_u32_be(cursor, mako_timestamp);
  write_u32_be(cursor, shape.operations);

  record_writer writer{cursor, record + shape.bytes - kCacheRecordCrcBytes};
  uint32_t visited = 0;
  if (!native_txn->visit_local_canonical_writes(
          write_record_mutation, &writer, &visited) ||
      visited != shape.operations || writer.cursor != writer.operations_end)
    return false;

  const uint32_t checksum =
      crc32c(record, shape.bytes - kCacheRecordCrcBytes);
  cursor = writer.operations_end;
  write_u32_be(cursor, checksum);
  return cursor == record + shape.bytes;
}

lcdf::Str as_key(const uint8_t *p, size_t n) {
  static const char empty = 0;
  const char *bytes = n == 0 ? &empty : reinterpret_cast<const char *>(p);
  return lcdf::Str(bytes, static_cast<int>(n));
}

bool on_owner_thread(const mako_local_txn *txn) {
  // Attachment assigns one process-lifetime, never-recycled STO worker ID to
  // this OS thread. Comparing that TLS integer is both a complete ownership
  // proof and substantially cheaper than calling pthread_self at every ABI
  // boundary. The attachment bit is required because uninitialised TThread
  // TLS is zero, which would otherwise alias worker slot zero.
  return local_attached &&
         TThread::id() == static_cast<int>(txn->worker_slot);
}

void finish_encoded_values(mako_local_txn *txn) noexcept {
  // The common small-transaction path retains both size and capacity. The
  // next payload copy and metadata initialization overwrite every byte before
  // the slot is lent to STO again, so no clearing scan is required.
  if (txn->encoded_values_require_release) {
    for (size_t index = 0; index != txn->encoded_values_used; ++index) {
      std::string &encoded = txn->encoded_values[index];
      if (encoded.capacity() <= kMaximumRetainedEncodedValueCapacity)
        continue;
      std::string{}.swap(encoded);
    }
    txn->encoded_values_require_release = false;
  }
  txn->encoded_values_used = 0;
}

template <bool TrustedOwnerBorrow>
[[gnu::always_inline]] inline void finish_txn_known(
    mako_local_txn *txn) noexcept {
  assert(txn->active);
  assert(!txn->poisoned);
  assert(local_active_txn == txn);
  if constexpr (TrustedOwnerBorrow)
    assert(txn->fast_table_impl != nullptr);
  else
    assert(txn->fast_table_impl == nullptr);
  txn->active = false;
  if constexpr (TrustedOwnerBorrow)
    txn->fast_table_impl = nullptr;
  txn->item_budget_used = 0;
  txn->record_plan_bytes = 0;
  txn->record_plan_ops = 0;
  txn->record_plan_sealed = false;
  txn->record_plan_ready = false;
  local_active_txn = nullptr;
  // Keep the database published for both surfaces. Safe Rust normally borrows
  // LocalDb through this point, but mem::forget is safe and ends that borrow
  // without native cleanup; db_close must still report BUSY rather than free
  // facade storage beneath the ambient transaction.
  active_database_slots[txn->worker_slot].database.store(
      nullptr, std::memory_order_release);
  finish_encoded_values(txn);
#if !READ_MY_WRITES
  txn->mutated_keys.clear();
#endif
}

// Public point operations can also be used by a trusted transaction and may
// terminate it on conflict or cleanup failure, so those shared cold paths
// select the matching lifetime publication dynamically. Known-success public
// and trusted terminal paths call the specialization directly.
void finish_txn(mako_local_txn *txn) noexcept {
  if (txn->fast_table_impl == nullptr)
    finish_txn_known<false>(txn);
  else
    finish_txn_known<true>(txn);
}

void recycle_txn(mako_local_txn *txn) noexcept {
  assert(!txn->active);
  assert(!txn->poisoned);
  // Finished handles can coexist briefly: callers may begin a new transaction
  // before destroying an older F-state facade. Retain at most one spare per
  // worker and delete any additional finished handle.
  const size_t worker_slot = txn->worker_slot;
  txn->worker_slot = active_database_slots.size();
  if (active_database_slots[worker_slot].spare == nullptr) {
    active_database_slots[worker_slot].spare = txn;
  } else {
    delete txn;
  }
}

[[gnu::noinline]] mako_local_txn *allocate_txn(mako_local_db *db,
                                               size_t worker_slot) {
#if READ_MY_WRITES
  return new mako_local_txn{db,   nullptr, worker_slot, 0,     0,    0,
                            0,    true,    false,       false, false, false,
                            {}};
#else
  return new mako_local_txn{db,   nullptr, worker_slot, 0,     0,    0,
                            0,    true,    false,       false, false, false,
                            {}, {}};
#endif
}

[[gnu::always_inline]] inline mako_local_txn *acquire_txn(mako_local_db *db,
                                                          size_t worker_slot) {
  active_database_slot &slot = active_database_slots[worker_slot];
  mako_local_txn *txn = slot.spare;
  if (txn == nullptr) [[unlikely]]
    return allocate_txn(db, worker_slot);
  slot.spare = nullptr;
  assert(!txn->active);
  assert(!txn->poisoned);
  assert(txn->worker_slot == active_database_slots.size());
  assert(txn->fast_table_impl == nullptr);
  assert(txn->item_budget_used == 0);
  assert(txn->encoded_values_used == 0);
  assert(txn->record_plan_bytes == 0);
  assert(txn->record_plan_ops == 0);
  assert(!txn->encoded_values_require_release);
  assert(!txn->record_plan_sealed);
  assert(!txn->record_plan_ready);
  txn->owner = db;
  txn->worker_slot = worker_slot;
  txn->active = true;
  // Only a successfully finished facade enters spare. finish_txn already
  // reset every cursor and bounded reusable state before recycle, so repeating
  // those checks here only lengthens every begin fast path.
  return txn;
}

void poison_worker(mako_local_txn *txn) noexcept {
  if (txn != nullptr) txn->poisoned = true;
  if (local_worker_poisoned) return;
  local_worker_poisoned = true;
  quarantined_worker_count.fetch_add(1, std::memory_order_relaxed);
}

int poison_transaction(mako_local_txn *txn) noexcept {
  poison_worker(txn);
  return MAKO_LOCAL_WORKER_POISONED;
}

bool abort_and_finish(mako_local_txn *txn,
                      cleanup_boundary boundary) noexcept {
  if (!txn->active) return true;
  if (txn->poisoned || local_worker_poisoned) return false;
  // The marker proves stop() was entered but did not publish terminal state.
  // It deliberately does not describe partial progress, so the only safe
  // action is quarantine; retrying could double-unlock or clean a tuple twice.
  if (Sto::cleanup_in_progress()) {
    poison_worker(txn);
    return false;
  }
  try {
#if defined(MAKO_LOCAL_TEST_HOOKS)
    // Some MassTrans conflict paths have already completed native abort before
    // returning control to the facade. Do not consume a stop-entry failpoint
    // when there is no native cleanup left to enter.
    if (Sto::in_progress())
      arm_native_cleanup_failure_if_requested(boundary);
#else
    (void)boundary;
#endif
    Sto::silent_abort();
  } catch (...) {
    // Native cleanup may still retain StringWrapper pointers. Quarantine the
    // transaction and worker rather than freeing those buffers or pretending
    // the TLS transaction can be reused safely.
    poison_worker(txn);
    return false;
  }
  finish_txn(txn);
  return true;
}

int check_txn(mako_local_txn *txn, mako_local_table *table = nullptr) {
  if (txn == nullptr) return MAKO_LOCAL_INVALID_ARGUMENT;
  // The active TLS pointer is an ownership and liveness proof on the common
  // path: only begin publishes it, and terminal cleanup clears it before the
  // facade can be recycled. Preserve the full diagnostic ordering for stale
  // and cross-thread handles on the cold mismatch path.
  if (local_active_txn != txn) [[unlikely]] {
    if (!on_owner_thread(txn)) return MAKO_LOCAL_WRONG_THREAD;
    if (txn->poisoned || local_worker_poisoned)
      return MAKO_LOCAL_WORKER_POISONED;
    return MAKO_LOCAL_TXN_FINISHED;
  }
  if (txn->poisoned) return MAKO_LOCAL_WORKER_POISONED;
  if (table != nullptr && table->owner != txn->owner)
    return MAKO_LOCAL_WRONG_DB_OR_TABLE;
  return MAKO_LOCAL_OK;
}

int check_txn_operation(mako_local_txn *txn,
                        mako_local_table *table = nullptr) {
  const int checked = check_txn(txn, table);
  if (checked != MAKO_LOCAL_OK) return checked;
  // Private record preflight is a seal: its byte count is derived from the
  // current canonical TransItems, so no subsequent read/write-set mutation is
  // permitted. Stable revision-0 transactions never set this flag.
  return txn->record_plan_sealed ? MAKO_LOCAL_BUSY : MAKO_LOCAL_OK;
}

int operation_abort(mako_local_txn *txn, int status) {
  return abort_and_finish(txn, cleanup_boundary::operation)
             ? status
             : MAKO_LOCAL_WORKER_POISONED;
}

int operation_exception(mako_local_txn *txn, int status) noexcept {
  if (Sto::cleanup_in_progress()) return poison_transaction(txn);
  if (!Sto::in_progress()) {
    // A native Sto::abort() completed and threw only its control-flow Abort.
    // Publish the facade/active-database transition without entering cleanup
    // again.
    finish_txn(txn);
    return status;
  }
  return operation_abort(txn, status);
}

template <bool TrustedOwnerBorrow>
[[gnu::always_inline]] inline void account_begin_txn_known(
    mako_local_txn *txn) noexcept {
  assert(local_active_txn == nullptr || local_active_txn == txn);
  if constexpr (TrustedOwnerBorrow)
    assert(txn->fast_table_impl != nullptr);
  else
    assert(txn->fast_table_impl == nullptr);
  local_active_txn = txn;
  active_database_slots[txn->worker_slot].database.store(
      txn->owner, std::memory_order_release);
}

void account_begin_txn(mako_local_txn *txn) noexcept {
  if (txn->fast_table_impl == nullptr)
    account_begin_txn_known<false>(txn);
  else
    account_begin_txn_known<true>(txn);
}

int cleanup_failed_begin(mako_local_txn *txn, int original_status) noexcept {
  if (txn == nullptr) return original_status;

  // Publish a private quarantine anchor before cleanup. On success finish_txn
  // clears this marker again; on failure the facade, active-database marker,
  // native TLS, and every potentially referenced allocation remain live even
  // though the public begin output stays null.
  account_begin_txn(txn);
  if (!abort_and_finish(txn, cleanup_boundary::begin))
    return MAKO_LOCAL_WORKER_POISONED;
  recycle_txn(txn);
  return original_status;
}

// Both exported begin surfaces perform their own contract checks, then inline
// this native transaction setup. In particular, the private Rust entry must
// not call the interposable public ABI and pay a second facade boundary on
// every short transaction.
template <bool TrustedOwnerBorrow>
[[gnu::always_inline]] inline int begin_txn_prevalidated(
    mako_local_db *db, mako_local_table_impl *fast_table_impl,
    mako_local_txn **out) noexcept {
  if (!local_attached) return MAKO_LOCAL_NOT_ATTACHED;
  if (local_worker_poisoned) return MAKO_LOCAL_WORKER_POISONED;
  if (local_active_txn != nullptr)
    return local_active_txn->poisoned ? MAKO_LOCAL_WORKER_POISONED
                                      : MAKO_LOCAL_TXN_ALREADY_ACTIVE;
  // Attachment permanently claims this worker for the local ABI, and every
  // native transaction it starts remains anchored by local_active_txn until
  // terminal cleanup. No independent STO transaction can therefore remain.
  assert(!Sto::in_progress());
  const int native_worker = TThread::id();
  if (native_worker < 0 ||
      static_cast<size_t>(native_worker) >= active_database_slots.size())
    return MAKO_LOCAL_INTERNAL;

  mako_local_txn *txn = nullptr;
  try {
    txn = acquire_txn(db, static_cast<size_t>(native_worker));
    if constexpr (TrustedOwnerBorrow)
      txn->fast_table_impl = fast_table_impl;
    else
      assert(fast_table_impl == nullptr);
    Sto::start_transaction();
    account_begin_txn_known<TrustedOwnerBorrow>(txn);
#if defined(MAKO_LOCAL_TEST_HOOKS)
    if (cleanup_failure_armed(cleanup_boundary::begin))
      return cleanup_failed_begin(txn, MAKO_LOCAL_INTERNAL);
#endif
    *out = txn;
    return MAKO_LOCAL_OK;
  } catch (const std::bad_alloc &) {
    return cleanup_failed_begin(txn, MAKO_LOCAL_OUT_OF_MEMORY);
  } catch (...) {
    return cleanup_failed_begin(txn, MAKO_LOCAL_INTERNAL);
  }
}

constexpr size_t kMasstreeSliceBytes = sizeof(uint64_t);

size_t write_item_charge(size_t key_len) {
  // A missing SET can observe the old leaf, create at most one new leaf per
  // eight-byte Masstree trie slice, and add its value item. Four fixed credits
  // also cover the existing-value resize path and cursor bookkeeping.
  return kMinimumWriteItemCharge +
         (key_len + kMasstreeSliceBytes - 1) / kMasstreeSliceBytes;
}

bool try_reserve_item_budget(mako_local_txn *txn, size_t charge) {
  static_assert(MAKO_LOCAL_TXN_ITEM_BUDGET <=
                Transaction::tset_initial_capacity);
  assert(txn->item_budget_used <= MAKO_LOCAL_TXN_ITEM_BUDGET);
  assert(charge <= MAKO_LOCAL_TXN_ITEM_BUDGET);
  const size_t next = txn->item_budget_used + charge;
  if (next > MAKO_LOCAL_TXN_ITEM_BUDGET) return false;
  txn->item_budget_used = next;
  return true;
}

// Every write stages one value before entering MassTrans. Keep this small
// bookkeeping path in its caller: avoiding the extra call/return removes
// repeated branches for short write-heavy transactions and lets the compiler
// reuse the already-validated lengths and transaction fields.
[[gnu::always_inline]] inline std::string &stage_encoded_value(
    mako_local_txn *txn, const uint8_t *value, size_t value_len) {
  // try_reserve_item_budget has already charged at least four credits for
  // this write, which proves a slot remains in the fixed pool.
  assert(txn->encoded_values_used < txn->encoded_values.size());
  const size_t index = txn->encoded_values_used++;
  std::string &encoded = txn->encoded_values[index];
  const size_t encoded_size =
      value_len + static_cast<size_t>(mako::EXTRA_BITS_FOR_VALUE);
  if (encoded.size() != encoded_size)
    encoded.resize(encoded_size, '\0');
  if (encoded.capacity() > kMaximumRetainedEncodedValueCapacity)
    txn->encoded_values_require_release = true;
  if (value_len != 0) std::memcpy(encoded.data(), value, value_len);
  // The buffer is private until transPut below. Initialize the entire tail,
  // including Node padding, once; then overwrite the pointer field with the
  // platform's actual null representation rather than assuming zero bits.
  std::array<unsigned char,
             static_cast<size_t>(mako::EXTRA_BITS_FOR_VALUE)> metadata{};
  char *null_data = nullptr;
  std::memcpy(metadata.data() + sizeof(uint32_t) + offsetof(mako::Node, data),
              &null_data, sizeof(null_data));
  std::memcpy(encoded.data() + value_len, metadata.data(), metadata.size());
  return encoded;
}

constexpr uint32_t kKnownScanFlags =
    MAKO_LOCAL_SCAN_HAS_UPPER | MAKO_LOCAL_SCAN_HAS_RESUME;

struct maximum_scan_key_storage {
  uint8_t bytes[MAKO_LOCAL_MAX_KEY_BYTES]{};

  constexpr maximum_scan_key_storage() {
    for (size_t i = 0; i != MAKO_LOCAL_MAX_KEY_BYTES; ++i)
      bytes[i] = UINT8_MAX;
  }
};

constexpr maximum_scan_key_storage kMaximumScanKey{};

struct scan_window {
  const uint8_t *begin;
  size_t begin_len;
  bool begin_inclusive;
  const uint8_t *end;
  size_t end_len;
  bool end_inclusive;
  bool empty;
};

// @safe - Bytewise Masstree key order without dereferencing empty slices.
int compare_slices(const uint8_t *left, size_t left_len,
                   const uint8_t *right, size_t right_len) {
  const size_t common = std::min(left_len, right_len);
  if (common != 0) {
    const int compared = std::memcmp(left, right, common);
    if (compared != 0) return compared;
  }
  if (left_len < right_len) return -1;
  if (left_len > right_len) return 1;
  return 0;
}

// @safe - Validates borrowed option slices and derives inclusive/exclusive
// MassTrans bounds. No input pointer is retained beyond the ABI call.
int make_scan_window(const mako_local_scan_options *options, bool reverse,
                     scan_window *window) {
  if (options == nullptr || window == nullptr ||
      options->struct_size < MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE ||
      (options->flags & ~kKnownScanFlags) != 0 ||
      !valid_slice(options->lower, options->lower_len))
    return MAKO_LOCAL_INVALID_ARGUMENT;

  const bool has_upper =
      (options->flags & MAKO_LOCAL_SCAN_HAS_UPPER) != 0;
  const bool has_resume =
      (options->flags & MAKO_LOCAL_SCAN_HAS_RESUME) != 0;
  if ((has_upper && !valid_slice(options->upper, options->upper_len)) ||
      (has_resume && !valid_slice(options->resume, options->resume_len)))
    return MAKO_LOCAL_INVALID_ARGUMENT;
  if (options->lower_len > MAKO_LOCAL_MAX_KEY_BYTES ||
      (has_upper && options->upper_len > MAKO_LOCAL_MAX_KEY_BYTES) ||
      (has_resume && options->resume_len > MAKO_LOCAL_MAX_KEY_BYTES))
    return MAKO_LOCAL_VALUE_TOO_LARGE;

  *window = scan_window{nullptr, 0, false, nullptr, 0, false, false};
  if (has_upper &&
      compare_slices(options->lower, options->lower_len,
                     options->upper, options->upper_len) >= 0) {
    window->empty = true;
    return MAKO_LOCAL_OK;
  }

  if (!reverse) {
    window->begin = options->lower;
    window->begin_len = options->lower_len;
    window->begin_inclusive = true;
    if (has_resume) {
      if (has_upper &&
          compare_slices(options->resume, options->resume_len,
                         options->upper, options->upper_len) >= 0) {
        window->empty = true;
        return MAKO_LOCAL_OK;
      }
      if (compare_slices(options->resume, options->resume_len,
                         options->lower, options->lower_len) >= 0) {
        window->begin = options->resume;
        window->begin_len = options->resume_len;
        window->begin_inclusive = false;
      }
    }
    if (has_upper) {
      window->end = options->upper;
      window->end_len = options->upper_len;
    }
    // Forward ranges always exclude their upper boundary.
    window->end_inclusive = false;
    return MAKO_LOCAL_OK;
  }

  // Reverse scans cover the same [lower, upper) set in descending order. Both
  // upper and resume are exclusive, so the smaller one is the traversal start.
  if (has_resume &&
      compare_slices(options->resume, options->resume_len,
                     options->lower, options->lower_len) <= 0) {
    window->empty = true;
    return MAKO_LOCAL_OK;
  }
  if (has_upper && has_resume) {
    if (compare_slices(options->upper, options->upper_len,
                       options->resume, options->resume_len) <= 0) {
      window->begin = options->upper;
      window->begin_len = options->upper_len;
    } else {
      window->begin = options->resume;
      window->begin_len = options->resume_len;
    }
  } else if (has_upper) {
    window->begin = options->upper;
    window->begin_len = options->upper_len;
  } else if (has_resume) {
    window->begin = options->resume;
    window->begin_len = options->resume_len;
  } else {
    // Masstree treats an empty reverse start as the minimum key, not +infinity.
    // Keys exposed by this ABI are bounded to 1024 bytes, so 1024 0xff bytes is
    // the actual maximum and must be included to make the upper bound open.
    window->begin = kMaximumScanKey.bytes;
    window->begin_len = MAKO_LOCAL_MAX_KEY_BYTES;
    window->begin_inclusive = true;
  }
  if (has_upper || has_resume)
    window->begin_inclusive = false;
  window->end = options->lower;
  window->end_len = options->lower_len;
  window->end_inclusive = true;
  return MAKO_LOCAL_OK;
}

struct scan_chunk_collector {
  mako_local_scan_entry *entries;
  size_t entries_capacity;
  uint8_t *arena;
  size_t arena_capacity;
  size_t count = 0;
  size_t used = 0;
  size_t required = 0;
  bool stopped_early = false;
  bool malformed_value = false;

  // @unsafe - Copies callback-borrowed MassTrans bytes into caller-owned
  // buffers after checking every offset and length.
  bool add(lcdf::Str key, const std::string &encoded_value) {
    if (count >= entries_capacity) {
      stopped_early = true;
      return false;
    }
    if (encoded_value.size() <
        static_cast<size_t>(mako::EXTRA_BITS_FOR_VALUE)) {
      malformed_value = true;
      return false;
    }

    const size_t key_len = static_cast<size_t>(key.length());
    const size_t value_len =
        encoded_value.size() - mako::EXTRA_BITS_FOR_VALUE;
    if (key_len > UINT32_MAX || value_len > UINT32_MAX ||
        key_len > SIZE_MAX - value_len) {
      malformed_value = true;
      return false;
    }
    const size_t entry_bytes = key_len + value_len;
    if (used > arena_capacity || entry_bytes > arena_capacity - used) {
      if (count == 0)
        required = entry_bytes;
      else
        stopped_early = true;
      return false;
    }

    const size_t key_offset = used;
    const size_t value_offset = used + key_len;
    if (value_offset > UINT32_MAX ||
        value_len > UINT32_MAX - value_offset) {
      malformed_value = true;
      return false;
    }
    if (key_len != 0)
      std::memcpy(arena + key_offset, key.data(), key_len);
    if (value_len != 0)
      std::memcpy(arena + value_offset, encoded_value.data(), value_len);
    entries[count] = mako_local_scan_entry{
        static_cast<uint32_t>(key_offset), static_cast<uint32_t>(key_len),
        static_cast<uint32_t>(value_offset), static_cast<uint32_t>(value_len)};
    used += entry_bytes;
    ++count;

    // Do not peek beyond a full descriptor buffer. Besides making `done`
    // conservative, this prevents a capacity boundary from consuming one more
    // OCC item and turning an otherwise valid chunk into TXN_TOO_LARGE.
    if (count == entries_capacity) {
      stopped_early = true;
      return false;
    }
    return true;
  }
};

// @unsafe - Bridges caller-owned C buffers to a synchronous native MassTrans
// callback. The callback is stack-bound and no pointer is retained.
int scan_chunk_impl(
    mako_local_txn *txn, mako_local_table *table,
    const mako_local_scan_options *options,
    mako_local_scan_entry *entries, size_t entries_capacity,
    uint8_t *arena, size_t arena_capacity,
    size_t *entry_count_out, size_t *arena_used_out,
    size_t *arena_required_out, uint8_t *done_out, bool reverse) {
  if (entry_count_out == nullptr || arena_used_out == nullptr ||
      arena_required_out == nullptr || done_out == nullptr)
    return MAKO_LOCAL_INVALID_ARGUMENT;
  *entry_count_out = 0;
  *arena_used_out = 0;
  *arena_required_out = 0;
  *done_out = 0;

  if (table == nullptr || entries == nullptr || entries_capacity == 0 ||
      (arena == nullptr && arena_capacity != 0) || arena_capacity > UINT32_MAX)
    return MAKO_LOCAL_INVALID_ARGUMENT;
  scan_window window{};
  const int bounds_status = make_scan_window(options, reverse, &window);
  if (bounds_status != MAKO_LOCAL_OK) return bounds_status;
  const int checked = check_txn_operation(txn, table);
  if (checked != MAKO_LOCAL_OK) return checked;
  if (window.empty) {
    *done_out = 1;
    return MAKO_LOCAL_OK;
  }

  try {
#if defined(MAKO_LOCAL_TEST_HOOKS)
    operation_cleanup_failure_scope cleanup_failure_scope;
#endif
    if (txn->item_budget_used > MAKO_LOCAL_TXN_ITEM_BUDGET)
      return operation_abort(txn, MAKO_LOCAL_TXN_TOO_LARGE);
    size_t remaining_items =
        MAKO_LOCAL_TXN_ITEM_BUDGET - txn->item_budget_used;
    scan_chunk_collector collector{entries, entries_capacity, arena,
                                   arena_capacity};
    auto collect = [&](lcdf::Str key, std::string &encoded_value) {
      return collector.add(key, encoded_value);
    };
    const bool within_budget = reverse
        ? table->table->transRQueryBounded(
              as_key(window.begin, window.begin_len),
              as_key(window.end, window.end_len), collect, remaining_items,
              window.begin_inclusive, window.end_inclusive)
        : table->table->transQueryBounded(
              as_key(window.begin, window.begin_len),
              as_key(window.end, window.end_len), collect, remaining_items,
              window.begin_inclusive, window.end_inclusive);
    txn->item_budget_used = MAKO_LOCAL_TXN_ITEM_BUDGET - remaining_items;

    if (!within_budget)
      return operation_abort(txn, MAKO_LOCAL_TXN_TOO_LARGE);
    if (TThread::transget_without_throw) {
      TThread::transget_without_throw = false;
      return operation_abort(txn, MAKO_LOCAL_CONFLICT);
    }
    if (collector.malformed_value)
      return operation_abort(txn, MAKO_LOCAL_INTERNAL);
    if (collector.required != 0) {
      *arena_required_out = collector.required;
      return MAKO_LOCAL_BUFFER_TOO_SMALL;
    }

    *entry_count_out = collector.count;
    *arena_used_out = collector.used;
    *done_out = collector.stopped_early ? 0 : 1;
    return MAKO_LOCAL_OK;
  } catch (const Transaction::Abort &) {
    return operation_exception(txn, MAKO_LOCAL_CONFLICT);
  } catch (const std::bad_alloc &) {
    return operation_exception(txn, MAKO_LOCAL_OUT_OF_MEMORY);
  } catch (...) {
    return operation_exception(txn, MAKO_LOCAL_INTERNAL);
  }
}

struct post_validate_bridge {
  mako_local_post_validate_hook hook;
  void *context;
};

bool invoke_post_validate_hook(void *opaque,
                               uint32_t timestamp) noexcept {
  auto *bridge = static_cast<post_validate_bridge *>(opaque);
  try {
    return bridge->hook(bridge->context, timestamp) != 0;
  } catch (...) {
    // Raw C++ callers can still supply a throwing function despite the C
    // declaration. Contain it before it can cross either the transaction core
    // or the C ABI. Rust's safe trampoline separately catches Rust panics in
    // unwind-enabled builds; panic=abort builds terminate before unwinding.
    return false;
  }
}

struct record_bind_bridge {
  mako_local_txn *txn;
  mako_rust_fast_record_bind_hook hook;
  void *context;
  uint8_t *record_written_out;
  record_shape final_shape;
  uint64_t sequence = 0;
  uint8_t *record = nullptr;
  uint64_t validation_ticket = 0;
  bool validation_gate_held = false;
};

void record_validation_cpu_relax() noexcept {
#if defined(__i386__) || defined(__x86_64__)
  _mm_pause();
#else
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

// @unsafe - Spins only after Transaction has acquired the complete write set.
// Ticket order therefore fixes Mako timestamp/final-validation order without
// assigning a persistent cache sequence to a transaction that may still
// abort. Unsigned ticket wrap remains correct while fewer than 2^64 turns are
// simultaneously outstanding, which is structurally guaranteed by workers.
void enter_record_validation_gate(void *opaque) noexcept {
  auto *bridge = static_cast<record_bind_bridge *>(opaque);
  assert(bridge != nullptr);
  assert(!bridge->validation_gate_held);
  mako_local_db *const db = bridge->txn->owner;
  const uint64_t ticket = db->record_validation_next.value.fetch_add(
      UINT64_C(1), std::memory_order_relaxed);
  bool reported_wait = false;
  while (db->record_validation_serving.value.load(std::memory_order_acquire) !=
         ticket) {
#if defined(MAKO_LOCAL_TEST_HOOKS)
    if (!reported_wait) {
      db->record_validation_wait_observations.fetch_add(
          UINT64_C(1), std::memory_order_release);
      reported_wait = true;
    }
#else
    (void)reported_wait;
#endif
    record_validation_cpu_relax();
  }
  bridge->validation_ticket = ticket;
  bridge->validation_gate_held = true;
}

void leave_record_validation_gate(void *opaque) noexcept {
  auto *bridge = static_cast<record_bind_bridge *>(opaque);
  assert(bridge != nullptr);
  assert(bridge->validation_gate_held);
  bridge->validation_gate_held = false;
  bridge->txn->owner->record_validation_serving.value.store(
      bridge->validation_ticket + UINT64_C(1), std::memory_order_release);
}

// @unsafe - Binds Rust-owned uninitialized storage while the full native write
// set is locked and the ordered validation turn is held. Every potentially
// failing native shape check precedes the external dense-sequence assignment.
bool invoke_record_bind_hook(void *opaque, uint32_t timestamp) noexcept {
  auto *bridge = static_cast<record_bind_bridge *>(opaque);
  assert(bridge->validation_gate_held);
  record_shape final_shape;
  if (!derive_record_shape(TThread::txn, &final_shape) ||
      final_shape.operations == 0 ||
      final_shape.operations != bridge->txn->record_plan_ops ||
      final_shape.bytes != bridge->txn->record_plan_bytes)
    return false;

  uint64_t sequence = 0;
  uint8_t *record = nullptr;
  size_t capacity = 0;
  try {
    if (bridge->hook(bridge->context, timestamp, final_shape.bytes,
                     &sequence, &record, &capacity) == 0)
      return false;
  } catch (...) {
    return false;
  }

  if (sequence == 0 || record == nullptr || capacity < final_shape.bytes)
    return false;
  bridge->final_shape = final_shape;
  bridge->sequence = sequence;
  bridge->record = record;
  return true;
}

// @unsafe - Completes the already-bound record after the ticket turn is
// retired, but while STO still owns the complete write set and before any
// write is installed. This bounded walk, copy, and CRC performs no allocation
// or I/O; failure leaves Rust's ordered slot bound but unwritten, which pins
// the queue fail-closed.
bool serialize_bound_record_after_gate(void *opaque,
                                       uint32_t timestamp) noexcept {
  auto *bridge = static_cast<record_bind_bridge *>(opaque);
  assert(!bridge->validation_gate_held);
  assert(bridge->sequence != 0);
  assert(bridge->record != nullptr);
  if (!serialize_cache_record(TThread::txn, bridge->sequence, timestamp,
                              bridge->final_shape, bridge->record))
    return false;
  *bridge->record_written_out = 1;
  return true;
}

#if defined(MAKO_LOCAL_TEST_HOOKS)
struct test_commit_observer_bridge {
  mako_local_test_commit_observer observer = nullptr;
  void *context = nullptr;
};

thread_local test_commit_observer_bridge local_test_commit_observer_bridge;

static_assert(static_cast<uint32_t>(
                  Transaction::test_commit_phase::writeset_locked) ==
              MAKO_LOCAL_TEST_COMMIT_WRITESET_LOCKED);
static_assert(static_cast<uint32_t>(
                  Transaction::test_commit_phase::mako_timestamp_allocated) ==
              MAKO_LOCAL_TEST_COMMIT_MAKO_TIMESTAMP_ALLOCATED);
static_assert(static_cast<uint32_t>(
                  Transaction::test_commit_phase::local_validation_complete) ==
              MAKO_LOCAL_TEST_COMMIT_LOCAL_VALIDATION_COMPLETE);
static_assert(static_cast<uint32_t>(
                  Transaction::test_commit_phase::preinstall_accepted) ==
              MAKO_LOCAL_TEST_COMMIT_PREINSTALL_ACCEPTED);
static_assert(static_cast<uint32_t>(
                  Transaction::test_commit_phase::first_write_installed) ==
              MAKO_LOCAL_TEST_COMMIT_FIRST_WRITE_INSTALLED);
static_assert(static_cast<uint32_t>(
                  Transaction::test_commit_phase::all_writes_installed) ==
              MAKO_LOCAL_TEST_COMMIT_ALL_WRITES_INSTALLED);

// @unsafe - Calls a caller-borrowed C test callback synchronously from STO's
// lock-held commit path. Exceptions are contained before reaching the noexcept
// transaction core; callers must keep the callback context alive until clear.
void invoke_test_commit_observer(
    void *opaque, Transaction::test_commit_phase phase,
    uint32_t mako_timestamp) noexcept {
  auto *bridge = static_cast<test_commit_observer_bridge *>(opaque);
  try {
    bridge->observer(bridge->context, static_cast<uint32_t>(phase),
                     mako_timestamp);
  } catch (...) {
    // A callback exception cannot alter commit semantics. Rust callbacks must
    // likewise contain panics before they cross their extern "C" trampoline.
  }
}
#endif

}  // namespace

extern "C" {

uint32_t mako_local_abi_version(void) noexcept {
  return MAKO_LOCAL_ABI_VERSION;
}

uint64_t mako_local_feature_bits(void) noexcept {
  uint64_t features = MAKO_LOCAL_FEATURE_POINT_TRANSACTIONS;
#if READ_MY_WRITES
  // Chunk retry also relies on local single-version item deduplication: without
  // it, revisiting a BUFFER_TOO_SMALL row can consume fresh transaction items.
  // Therefore the complete transactional-scan contract, not only scan RYW, is
  // advertised with the RYW profile.
  features |= MAKO_LOCAL_FEATURE_READ_MY_WRITES |
              MAKO_LOCAL_FEATURE_TRANSACTIONAL_SCANS |
              MAKO_LOCAL_FEATURE_SCAN_READ_MY_WRITES;
#endif
#if STO_OPACITY
  features |= MAKO_LOCAL_FEATURE_OPACITY;
#endif
#if defined(MAKO_LOCAL_TEST_HOOKS)
  features |= MAKO_LOCAL_FEATURE_TEST_COMMIT_OBSERVER |
              MAKO_LOCAL_FEATURE_TEST_CLEANUP_FAILURES;
#endif
  return features;
}

size_t mako_local_db_options_size(void) noexcept {
  return MAKO_LOCAL_DB_OPTIONS_V0_SIZE;
}

size_t mako_local_scan_options_size(void) noexcept {
  return MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE;
}

size_t mako_local_scan_entry_size(void) noexcept {
  return sizeof(mako_local_scan_entry);
}

const char *mako_local_status_string(int status) noexcept {
  switch (status) {
#define MAKO_LOCAL_STATUS_CASE(short_name, c_symbol, message) \
  case c_symbol: return message;
    MAKO_LOCAL_FOR_EACH_STATUS(MAKO_LOCAL_STATUS_CASE)
#undef MAKO_LOCAL_STATUS_CASE
    default: return "unknown mako-local status";
  }
}

int mako_local_worker_health(void) noexcept {
  if (local_worker_poisoned) return MAKO_LOCAL_WORKER_POISONED;
  return local_attached ? MAKO_LOCAL_OK : MAKO_LOCAL_NOT_ATTACHED;
}

uint64_t mako_local_quarantined_worker_count(void) noexcept {
  return quarantined_worker_count.load(std::memory_order_relaxed);
}

int mako_local_advance_mako_timestamp_past(uint32_t observed) noexcept {
  if (observed == 0) return MAKO_LOCAL_INVALID_ARGUMENT;
  return Transaction::advance_mako_timestamp_past(observed)
             ? MAKO_LOCAL_OK
             : MAKO_LOCAL_TIMESTAMP_EXHAUSTED;
}

int mako_local_thread_attach(void) noexcept {
  if (local_worker_poisoned) return MAKO_LOCAL_WORKER_POISONED;
  if (local_attached) return MAKO_LOCAL_OK;
  try {
    // Do not replace native thread state underneath a live Mako transaction.
    if (Sto::in_progress()) return MAKO_LOCAL_BUSY;
    if (!mako::silo::claim_thread_runtime(
            mako::silo::thread_runtime::local_abi))
      return MAKO_LOCAL_BUSY;

    const int id = mako::silo::try_allocate_thread_id();
    if (id < 0) return MAKO_LOCAL_THREAD_LIMIT;

    TThread::set_id(id);
    // Sto::transaction() is process TLS and may have been created by legacy
    // direct STO code before that code adopted the shared allocator. We proved
    // it idle above, so refresh its cached owner ID before it can be reused.
    Sto::update_threadid();
    TThread::set_mode(0);
    TThread::set_shard_index(0);
    TThread::set_nshards(1);
    TThread::set_warehouses(1);
    TThread::set_pid(0);
    TThread::set_is_micro(0);
    TThread::disable_multiversion();
    TThread::readset_shard_bits = 0;
    TThread::writeset_shard_bits = 0;
    TThread::transget_without_throw = false;
    TThread::transget_without_stable = false;
    TThread::trans_nosend_abort = 0;
    TThread::increment_id = 0;
    TThread::sclient = nullptr;

    if (!mako::silo::ensure_epoch_runtime()) return MAKO_LOCAL_INTERNAL;
    mako_local_table_impl::thread_init();
    local_attached = true;
    return MAKO_LOCAL_OK;
  } catch (const std::bad_alloc &) {
    return MAKO_LOCAL_OUT_OF_MEMORY;
  } catch (...) {
    return MAKO_LOCAL_INTERNAL;
  }
}

// @unsafe - Registers a non-owning callback/context pair in this attached
// worker's native TLS. The caller owns both until the matching clear call.
int mako_local_test_set_commit_observer(
    mako_local_test_commit_observer observer, void *context) noexcept {
#if defined(MAKO_LOCAL_TEST_HOOKS)
  if (observer == nullptr) return MAKO_LOCAL_INVALID_ARGUMENT;
  if (!local_attached) return MAKO_LOCAL_NOT_ATTACHED;
  if (local_test_commit_observer_bridge.observer != nullptr)
    return MAKO_LOCAL_BUSY;
  local_test_commit_observer_bridge.observer = observer;
  local_test_commit_observer_bridge.context = context;
  Transaction::set_test_commit_observer(
      invoke_test_commit_observer, &local_test_commit_observer_bridge);
  return MAKO_LOCAL_OK;
#else
  (void)observer;
  (void)context;
  return MAKO_LOCAL_FEATURE_UNAVAILABLE;
#endif
}

// @unsafe - Ends the native TLS borrow before the caller may release context.
int mako_local_test_clear_commit_observer(void) noexcept {
#if defined(MAKO_LOCAL_TEST_HOOKS)
  if (!local_attached) return MAKO_LOCAL_NOT_ATTACHED;
  Transaction::clear_test_commit_observer();
  local_test_commit_observer_bridge.observer = nullptr;
  local_test_commit_observer_bridge.context = nullptr;
  return MAKO_LOCAL_OK;
#else
  return MAKO_LOCAL_FEATURE_UNAVAILABLE;
#endif
}

int mako_local_test_arm_cleanup_failure(uint32_t boundary) noexcept {
#if defined(MAKO_LOCAL_TEST_HOOKS)
  if (boundary < MAKO_LOCAL_CLEANUP_BOUNDARY_BEGIN ||
      boundary > MAKO_LOCAL_CLEANUP_BOUNDARY_DESTROY)
    return MAKO_LOCAL_INVALID_ARGUMENT;
  if (!local_attached) return MAKO_LOCAL_NOT_ATTACHED;
  if (local_worker_poisoned) return MAKO_LOCAL_WORKER_POISONED;
  if (local_test_cleanup_failure != 0) return MAKO_LOCAL_BUSY;
  local_test_cleanup_failure = boundary;
  return MAKO_LOCAL_OK;
#else
  (void)boundary;
  return MAKO_LOCAL_FEATURE_UNAVAILABLE;
#endif
}

int mako_local_test_clear_cleanup_failure(void) noexcept {
#if defined(MAKO_LOCAL_TEST_HOOKS)
  if (!local_attached) return MAKO_LOCAL_NOT_ATTACHED;
  if (local_worker_poisoned) return MAKO_LOCAL_WORKER_POISONED;
  local_test_cleanup_failure = 0;
  return MAKO_LOCAL_OK;
#else
  return MAKO_LOCAL_FEATURE_UNAVAILABLE;
#endif
}

int mako_local_db_open_with_options(
    const mako_local_db_options *options, mako_local_db **out) noexcept {
  if (out == nullptr) return MAKO_LOCAL_INVALID_ARGUMENT;
  *out = nullptr;
  if (options == nullptr ||
      options->struct_size < MAKO_LOCAL_DB_OPTIONS_V0_SIZE ||
      options->flags != 0)
    return MAKO_LOCAL_INVALID_ARGUMENT;
  try {
    *out = new mako_local_db();
    return MAKO_LOCAL_OK;
  } catch (const std::bad_alloc &) {
    return MAKO_LOCAL_OUT_OF_MEMORY;
  } catch (...) {
    return MAKO_LOCAL_INTERNAL;
  }
}

int mako_local_db_open(mako_local_db **out) noexcept {
  const mako_local_db_options options{MAKO_LOCAL_DB_OPTIONS_V0_SIZE, 0};
  return mako_local_db_open_with_options(&options, out);
}

int mako_local_db_close(mako_local_db *db) noexcept {
  if (db == nullptr) return MAKO_LOCAL_OK;
  try {
    for (const active_database_slot &slot : active_database_slots) {
      if (slot.database.load(std::memory_order_acquire) == db)
        return MAKO_LOCAL_BUSY;
    }
    // Facade table handles are reclaimed here. Their MassTrans objects are
    // intentionally process-lifetime until Mako has a verified global RCU
    // quiescence protocol.
    delete db;
    return MAKO_LOCAL_OK;
  } catch (...) {
    return MAKO_LOCAL_INTERNAL;
  }
}

int mako_local_table_open(mako_local_db *db, const uint8_t *name,
                          size_t name_len, uint64_t table_id,
                          mako_local_table **out) noexcept {
  if (out == nullptr) return MAKO_LOCAL_INVALID_ARGUMENT;
  *out = nullptr;
  if (db == nullptr || !valid_slice(name, name_len))
    return MAKO_LOCAL_INVALID_ARGUMENT;
  if (name_len > MAKO_LOCAL_MAX_TABLE_NAME_BYTES)
    return MAKO_LOCAL_VALUE_TOO_LARGE;
  if (!local_attached) return MAKO_LOCAL_NOT_ATTACHED;

  try {
    const std::string table_name(
        name_len == 0 ? "" : reinterpret_cast<const char *>(name), name_len);
    std::lock_guard<std::mutex> guard(db->tables_mu);
    auto it = db->tables.find(table_name);
    if (it != db->tables.end()) {
      if (it->second->id != table_id) return MAKO_LOCAL_WRONG_DB_OR_TABLE;
      *out = it->second.get();
      return MAKO_LOCAL_OK;
    }
    for (const auto &[existing_name, existing] : db->tables) {
      (void)existing_name;
      if (existing->id == table_id) return MAKO_LOCAL_WRONG_DB_OR_TABLE;
    }

    // Retain ownership until every facade allocation and map insertion has
    // succeeded. On success MassTrans becomes intentionally process-lifetime.
    auto inner = std::make_unique<mako_local_table_impl>();
    inner->set_table_id(table_id);
    inner->set_is_remote(false);
    inner->set_table_name(table_name);

    auto table = std::make_unique<mako_local_table>();
    table->owner = db;
    table->table = inner.get();
    table->id = table_id;
    auto [inserted, did_insert] =
        db->tables.emplace(table_name, std::move(table));
    if (!did_insert) return MAKO_LOCAL_INTERNAL;
    inner.release();
    *out = inserted->second.get();
    return MAKO_LOCAL_OK;
  } catch (const std::bad_alloc &) {
    return MAKO_LOCAL_OUT_OF_MEMORY;
  } catch (...) {
    return MAKO_LOCAL_INTERNAL;
  }
}

uint64_t mako_local_table_id(const mako_local_table *table) noexcept {
  return table == nullptr ? 0 : table->id;
}

int mako_local_txn_begin(mako_local_db *db, mako_local_txn **out) noexcept {
  if (out == nullptr) return MAKO_LOCAL_INVALID_ARGUMENT;
  *out = nullptr;
  if (db == nullptr) return MAKO_LOCAL_INVALID_ARGUMENT;
  return begin_txn_prevalidated<false>(db, nullptr, out);
}

int mako_local_txn_get(mako_local_txn *txn, mako_local_table *table,
                       const uint8_t *key, size_t key_len,
                       uint8_t **value_out, size_t *value_len_out,
                       uint8_t *found_out) noexcept {
  if (value_out == nullptr || value_len_out == nullptr || found_out == nullptr)
    return MAKO_LOCAL_INVALID_ARGUMENT;
  *value_out = nullptr;
  *value_len_out = 0;
  *found_out = 0;
  if (table == nullptr || !valid_slice(key, key_len))
    return MAKO_LOCAL_INVALID_ARGUMENT;
  if (key_len > MAKO_LOCAL_MAX_KEY_BYTES)
    return MAKO_LOCAL_VALUE_TOO_LARGE;
  const int checked = check_txn_operation(txn, table);
  if (checked != MAKO_LOCAL_OK) return checked;

  try {
#if defined(MAKO_LOCAL_TEST_HOOKS)
    operation_cleanup_failure_scope cleanup_failure_scope;
#endif
    if (!try_reserve_item_budget(txn, 1))
      return operation_abort(txn, MAKO_LOCAL_TXN_TOO_LARGE);
    std::string value;
    const bool found = table->table->transGet(as_key(key, key_len), value);
    if (TThread::transget_without_throw) {
      TThread::transget_without_throw = false;
      return operation_abort(txn, MAKO_LOCAL_CONFLICT);
    }
    if (!found) return MAKO_LOCAL_OK;
    if (value.size() < static_cast<size_t>(mako::EXTRA_BITS_FOR_VALUE))
      return operation_abort(txn, MAKO_LOCAL_INTERNAL);

    value.resize(value.size() - mako::EXTRA_BITS_FOR_VALUE);
    void *bytes = std::malloc(value.empty() ? 1 : value.size());
    if (bytes == nullptr) return operation_abort(txn, MAKO_LOCAL_OUT_OF_MEMORY);
    if (!value.empty()) std::memcpy(bytes, value.data(), value.size());
    *value_out = static_cast<uint8_t *>(bytes);
    *value_len_out = value.size();
    *found_out = 1;
    return MAKO_LOCAL_OK;
  } catch (const Transaction::Abort &) {
    return operation_exception(txn, MAKO_LOCAL_CONFLICT);
  } catch (const std::bad_alloc &) {
    return operation_exception(txn, MAKO_LOCAL_OUT_OF_MEMORY);
  } catch (...) {
    return operation_exception(txn, MAKO_LOCAL_INTERNAL);
  }
}

int mako_local_txn_put(mako_local_txn *txn, mako_local_table *table,
                       const uint8_t *key, size_t key_len,
                       const uint8_t *value, size_t value_len,
                       uint8_t *created_out) noexcept {
  if (created_out == nullptr) return MAKO_LOCAL_INVALID_ARGUMENT;
  *created_out = 0;
  if (table == nullptr || !valid_slice(key, key_len) ||
      !valid_slice(value, value_len))
    return MAKO_LOCAL_INVALID_ARGUMENT;
  if (key_len > MAKO_LOCAL_MAX_KEY_BYTES ||
      value_len > MAKO_LOCAL_MAX_VALUE_BYTES)
    return MAKO_LOCAL_VALUE_TOO_LARGE;
  const int checked = check_txn_operation(txn, table);
  if (checked != MAKO_LOCAL_OK) return checked;

  try {
#if defined(MAKO_LOCAL_TEST_HOOKS)
    operation_cleanup_failure_scope cleanup_failure_scope;
#endif
#if !READ_MY_WRITES
    auto &mutations = txn->mutated_keys[table->table];
    const std::string mutation_key(
        key_len == 0 ? "" : reinterpret_cast<const char *>(key), key_len);
    if (mutations.contains(mutation_key))
      return MAKO_LOCAL_DUPLICATE_WRITE;
#endif
    if (!try_reserve_item_budget(txn, write_item_charge(key_len)))
      return operation_abort(txn, MAKO_LOCAL_TXN_TOO_LARGE);
    // Build directly in a stable transaction-owned slot. Clean terminal
    // paths retain a bounded set of small allocations for the next facade
    // generation instead of malloc/free on every short transaction.
    std::string &encoded = stage_encoded_value(txn, value, value_len);
    const bool existed = table->table->transPut(
        as_key(key, key_len), StringWrapper(encoded));
#if !READ_MY_WRITES
    mutations.insert(mutation_key);
#endif
    *created_out = existed ? 0 : 1;
    return MAKO_LOCAL_OK;
  } catch (const Transaction::Abort &) {
    return operation_exception(txn, MAKO_LOCAL_CONFLICT);
  } catch (const std::bad_alloc &) {
    return operation_exception(txn, MAKO_LOCAL_OUT_OF_MEMORY);
  } catch (...) {
    return operation_exception(txn, MAKO_LOCAL_INTERNAL);
  }
}

int mako_local_txn_insert(mako_local_txn *txn, mako_local_table *table,
                          const uint8_t *key, size_t key_len,
                          const uint8_t *value, size_t value_len,
                          uint8_t *inserted_out) noexcept {
  if (inserted_out == nullptr) return MAKO_LOCAL_INVALID_ARGUMENT;
  *inserted_out = 0;
  if (table == nullptr || !valid_slice(key, key_len) ||
      !valid_slice(value, value_len))
    return MAKO_LOCAL_INVALID_ARGUMENT;
  if (key_len > MAKO_LOCAL_MAX_KEY_BYTES ||
      value_len > MAKO_LOCAL_MAX_VALUE_BYTES)
    return MAKO_LOCAL_VALUE_TOO_LARGE;
  const int checked = check_txn_operation(txn, table);
  if (checked != MAKO_LOCAL_OK) return checked;

  try {
#if defined(MAKO_LOCAL_TEST_HOOKS)
    operation_cleanup_failure_scope cleanup_failure_scope;
#endif
#if !READ_MY_WRITES
    auto &mutations = txn->mutated_keys[table->table];
    const std::string mutation_key(
        key_len == 0 ? "" : reinterpret_cast<const char *>(key), key_len);
    if (mutations.contains(mutation_key))
      return MAKO_LOCAL_DUPLICATE_WRITE;
#endif
    if (!try_reserve_item_budget(txn, write_item_charge(key_len)))
      return operation_abort(txn, MAKO_LOCAL_TXN_TOO_LARGE);
    std::string &encoded = stage_encoded_value(txn, value, value_len);
    const bool existed = table->table->transInsert(
        as_key(key, key_len), StringWrapper(encoded));
#if !READ_MY_WRITES
    if (!existed) mutations.insert(mutation_key);
#endif
    *inserted_out = existed ? 0 : 1;
    return MAKO_LOCAL_OK;
  } catch (const Transaction::Abort &) {
    return operation_exception(txn, MAKO_LOCAL_CONFLICT);
  } catch (const std::bad_alloc &) {
    return operation_exception(txn, MAKO_LOCAL_OUT_OF_MEMORY);
  } catch (...) {
    return operation_exception(txn, MAKO_LOCAL_INTERNAL);
  }
}

int mako_local_txn_remove(mako_local_txn *txn, mako_local_table *table,
                          const uint8_t *key, size_t key_len,
                          uint8_t *existed_out) noexcept {
  if (existed_out == nullptr) return MAKO_LOCAL_INVALID_ARGUMENT;
  *existed_out = 0;
  if (table == nullptr || !valid_slice(key, key_len))
    return MAKO_LOCAL_INVALID_ARGUMENT;
  if (key_len > MAKO_LOCAL_MAX_KEY_BYTES)
    return MAKO_LOCAL_VALUE_TOO_LARGE;
  const int checked = check_txn_operation(txn, table);
  if (checked != MAKO_LOCAL_OK) return checked;

  try {
#if defined(MAKO_LOCAL_TEST_HOOKS)
    operation_cleanup_failure_scope cleanup_failure_scope;
#endif
#if !READ_MY_WRITES
    auto &mutations = txn->mutated_keys[table->table];
    const std::string mutation_key(
        key_len == 0 ? "" : reinterpret_cast<const char *>(key), key_len);
    if (mutations.contains(mutation_key))
      return MAKO_LOCAL_DUPLICATE_WRITE;
#endif
    if (!try_reserve_item_budget(txn, 1))
      return operation_abort(txn, MAKO_LOCAL_TXN_TOO_LARGE);
    const bool existed = table->table->transDelete(as_key(key, key_len));
#if !READ_MY_WRITES
    if (existed) mutations.insert(mutation_key);
#endif
    *existed_out = existed ? 1 : 0;
    return MAKO_LOCAL_OK;
  } catch (const Transaction::Abort &) {
    return operation_exception(txn, MAKO_LOCAL_CONFLICT);
  } catch (const std::bad_alloc &) {
    return operation_exception(txn, MAKO_LOCAL_OUT_OF_MEMORY);
  } catch (...) {
    return operation_exception(txn, MAKO_LOCAL_INTERNAL);
  }
}

int mako_local_txn_scan_chunk(
    mako_local_txn *txn, mako_local_table *table,
    const mako_local_scan_options *options,
    mako_local_scan_entry *entries, size_t entries_capacity,
    uint8_t *arena, size_t arena_capacity,
    size_t *entry_count_out, size_t *arena_used_out,
    size_t *arena_required_out, uint8_t *done_out) noexcept {
  return scan_chunk_impl(txn, table, options, entries, entries_capacity,
                         arena, arena_capacity, entry_count_out,
                         arena_used_out, arena_required_out, done_out,
                         false /* forward */);
}

int mako_local_txn_rscan_chunk(
    mako_local_txn *txn, mako_local_table *table,
    const mako_local_scan_options *options,
    mako_local_scan_entry *entries, size_t entries_capacity,
    uint8_t *arena, size_t arena_capacity,
    size_t *entry_count_out, size_t *arena_used_out,
    size_t *arena_required_out, uint8_t *done_out) noexcept {
  return scan_chunk_impl(txn, table, options, entries, entries_capacity,
                         arena, arena_capacity, entry_count_out,
                         arena_used_out, arena_required_out, done_out,
                         true /* reverse */);
}

int mako_local_txn_commit(mako_local_txn *txn) noexcept {
  const int checked = check_txn(txn);
  if (checked != MAKO_LOCAL_OK) return checked;
  try {
#if defined(MAKO_LOCAL_TEST_HOOKS)
    arm_native_cleanup_failure_if_requested(cleanup_boundary::commit);
#endif
    const bool committed = Sto::try_commit_no_paxos();
    finish_txn_known<false>(txn);
    return committed ? MAKO_LOCAL_OK : MAKO_LOCAL_CONFLICT;
  } catch (...) {
    // try_commit() may have thrown from Transaction::stop() after an unknown
    // amount of unlock/cleanup progress. Retrying abort here could double
    // unlock or clean an installed tuple twice.
    return poison_transaction(txn);
  }
}

int mako_local_txn_commit_with_hook(
    mako_local_txn *txn, mako_local_post_validate_hook hook,
    void *context) noexcept {
  if (hook == nullptr) return MAKO_LOCAL_INVALID_ARGUMENT;
  const int checked = check_txn(txn);
  if (checked != MAKO_LOCAL_OK) return checked;

  post_validate_bridge bridge{hook, context};
  Transaction::preinstall_failure failure =
      Transaction::preinstall_failure::none;
  try {
#if defined(MAKO_LOCAL_TEST_HOOKS)
    arm_native_cleanup_failure_if_requested(cleanup_boundary::commit);
#endif
    const bool committed = Sto::try_commit_no_paxos(
        invoke_post_validate_hook, &bridge, &failure);
    finish_txn_known<false>(txn);
    if (committed) return MAKO_LOCAL_OK;
    switch (failure) {
      case Transaction::preinstall_failure::hook_rejected:
        return MAKO_LOCAL_COMMIT_HOOK_REJECTED;
      case Transaction::preinstall_failure::timestamp_exhausted:
        return MAKO_LOCAL_TIMESTAMP_EXHAUSTED;
      case Transaction::preinstall_failure::none:
        return MAKO_LOCAL_CONFLICT;
    }
  } catch (...) {
    return poison_transaction(txn);
  }
  return poison_transaction(txn);
}

int mako_local_txn_abort(mako_local_txn *txn) noexcept {
  const int checked = check_txn(txn);
  if (checked != MAKO_LOCAL_OK) return checked;
  return abort_and_finish(txn, cleanup_boundary::abort)
             ? MAKO_LOCAL_OK
             : MAKO_LOCAL_WORKER_POISONED;
}

int mako_local_txn_destroy(mako_local_txn *txn) noexcept {
  if (txn == nullptr) return MAKO_LOCAL_OK;
  if (!on_owner_thread(txn)) return MAKO_LOCAL_WRONG_THREAD;
  try {
    if (txn->poisoned) return MAKO_LOCAL_WORKER_POISONED;
    if (txn->active) {
      const int checked = check_txn(txn);
      if (checked != MAKO_LOCAL_OK) return checked;
    }
    if (txn->active &&
        !abort_and_finish(txn, cleanup_boundary::destroy))
      return MAKO_LOCAL_WORKER_POISONED;
    recycle_txn(txn);
    return MAKO_LOCAL_OK;
  } catch (...) {
    return poison_transaction(txn);
  }
}

void mako_local_bytes_free(void *bytes) noexcept {
  std::free(bytes);
}

#if defined(__GNUC__) || defined(__clang__)
#define MAKO_RUST_FAST_DEFINITION_HIDDEN __attribute__((visibility("hidden")))
#else
#define MAKO_RUST_FAST_DEFINITION_HIDDEN
#endif

MAKO_RUST_FAST_DEFINITION_HIDDEN int
mako_rust_fast_txn_begin(mako_local_db *db, mako_local_table *bound_table,
                         mako_local_txn **out) noexcept {
  if (out == nullptr)
    return MAKO_LOCAL_INVALID_ARGUMENT;
  *out = nullptr;
  if (db == nullptr || bound_table == nullptr)
    return MAKO_LOCAL_INVALID_ARGUMENT;
  if (bound_table->owner != db)
    return MAKO_LOCAL_WRONG_DB_OR_TABLE;
  return begin_txn_prevalidated<true>(db, bound_table->table, out);
}

MAKO_RUST_FAST_DEFINITION_HIDDEN uint64_t mako_rust_fast_txn_put(
    mako_local_txn *txn, const uint8_t *key, uint32_t key_len,
    const uint8_t *value, uint32_t value_len) noexcept {
  if (key_len > MAKO_LOCAL_MAX_KEY_BYTES ||
      value_len > MAKO_LOCAL_MAX_VALUE_BYTES)
    return pack_fast_put_result(MAKO_LOCAL_VALUE_TOO_LARGE);
  if (txn->record_plan_sealed) [[unlikely]]
    return pack_fast_put_result(MAKO_LOCAL_BUSY);

  // Rust's private wrapper makes these proofs once at fast begin. Retaining
  // debug assertions documents the contract without putting opaque-handle,
  // owner-thread, table-owner, or slice validation on the release hot path.
  assert(txn != nullptr);
  assert(local_active_txn == txn);
  assert(on_owner_thread(txn));
  assert(txn->active);
  assert(!txn->poisoned);
  assert(txn->fast_table_impl != nullptr);
  assert(!txn->record_plan_sealed);
  assert(valid_slice(key, key_len));
  assert(valid_slice(value, value_len));
  mako_local_table_impl *const table = txn->fast_table_impl;

  try {
#if defined(MAKO_LOCAL_TEST_HOOKS)
    operation_cleanup_failure_scope cleanup_failure_scope;
#endif
#if !READ_MY_WRITES
    auto &mutations = txn->mutated_keys[table];
    const std::string mutation_key(
        key_len == 0 ? "" : reinterpret_cast<const char *>(key), key_len);
    if (mutations.contains(mutation_key))
      return pack_fast_put_result(MAKO_LOCAL_DUPLICATE_WRITE);
#endif
    if (!try_reserve_item_budget(txn, write_item_charge(key_len))) {
      return pack_fast_put_result(
          operation_abort(txn, MAKO_LOCAL_TXN_TOO_LARGE));
    }

    // StringWrapper retains this address through commit/abort. Use the same
    // fixed transaction-owned staging pool and retention bound as the public
    // ABI; bypassing validation must never shorten the value lifetime.
    std::string &encoded = stage_encoded_value(txn, value, value_len);
    const bool existed =
        table->transPut(as_key(key, key_len), StringWrapper(encoded));
#if !READ_MY_WRITES
    mutations.insert(mutation_key);
#endif
    return pack_fast_put_result(MAKO_LOCAL_OK, !existed);
  } catch (const Transaction::Abort &) {
    return pack_fast_put_result(operation_exception(txn, MAKO_LOCAL_CONFLICT));
  } catch (const std::bad_alloc &) {
    return pack_fast_put_result(
        operation_exception(txn, MAKO_LOCAL_OUT_OF_MEMORY));
  } catch (...) {
    return pack_fast_put_result(operation_exception(txn, MAKO_LOCAL_INTERNAL));
  }
}

MAKO_RUST_FAST_DEFINITION_HIDDEN int
mako_rust_fast_txn_record_preflight(
    mako_local_txn *txn, size_t max_record_bytes,
    size_t *exact_record_bytes_out, uint32_t *op_count_out) noexcept {
  if (exact_record_bytes_out == nullptr || op_count_out == nullptr)
    return MAKO_LOCAL_INVALID_ARGUMENT;
  *exact_record_bytes_out = 0;
  *op_count_out = 0;

  const int checked = check_txn(txn);
  if (checked != MAKO_LOCAL_OK) return checked;
  if (txn->fast_table_impl == nullptr) return MAKO_LOCAL_INVALID_ARGUMENT;
  if (txn->record_plan_sealed) return MAKO_LOCAL_BUSY;

  record_shape shape;
  if (!derive_record_shape(TThread::txn, &shape) ||
      shape.operations > MAKO_LOCAL_TXN_ITEM_BUDGET)
    return MAKO_LOCAL_INTERNAL;

  // Seal even on a cap rejection. Rust consumes that transaction by aborting;
  // allowing another operation or a differently capped second preflight would
  // make failure handling depend on a mutable write set.
  txn->record_plan_bytes = shape.bytes;
  txn->record_plan_ops = shape.operations;
  txn->record_plan_sealed = true;
  txn->record_plan_ready =
      shape.operations == 0 || shape.bytes <= max_record_bytes;
  *exact_record_bytes_out = shape.bytes;
  *op_count_out = shape.operations;
  return txn->record_plan_ready ? MAKO_LOCAL_OK
                                : MAKO_LOCAL_VALUE_TOO_LARGE;
}

MAKO_RUST_FAST_DEFINITION_HIDDEN uint64_t
mako_rust_fast_txn_commit_record_and_destroy(
    mako_local_txn *txn, mako_rust_fast_record_bind_hook bind_hook,
    void *context, uint8_t *record_written_out) noexcept {
  assert(txn != nullptr);
  assert(on_owner_thread(txn));
  assert(!txn->poisoned);
  assert(!local_worker_poisoned);
  assert(txn->active);
  assert(local_active_txn == txn);
  assert(txn->fast_table_impl != nullptr);

  if (record_written_out != nullptr) *record_written_out = 0;
  if (bind_hook == nullptr || record_written_out == nullptr ||
      !txn->record_plan_sealed || !txn->record_plan_ready ||
      txn->record_plan_ops == 0 ||
      txn->record_plan_bytes < kCacheRecordMinimumBytes) {
    if (!abort_and_finish(txn, cleanup_boundary::abort))
      return pack_fast_terminal_result(MAKO_LOCAL_WORKER_POISONED,
                                       MAKO_LOCAL_WORKER_POISONED);
    recycle_txn(txn);
    return pack_fast_terminal_result(MAKO_LOCAL_INVALID_ARGUMENT,
                                     MAKO_LOCAL_OK);
  }

  int status = MAKO_LOCAL_INTERNAL;
  try {
#if defined(MAKO_LOCAL_TEST_HOOKS)
    arm_native_cleanup_failure_if_requested(cleanup_boundary::commit);
#endif
    Transaction::preinstall_failure failure =
        Transaction::preinstall_failure::none;
    record_bind_bridge bridge{txn, bind_hook, context, record_written_out};
    const Transaction::commit_validation_gate validation_gate{
        enter_record_validation_gate, leave_record_validation_gate,
        serialize_bound_record_after_gate, &bridge};
    const bool committed = Sto::try_commit_no_paxos(
        invoke_record_bind_hook, &bridge, &failure, &validation_gate);
    finish_txn_known<true>(txn);

    if (committed) {
      status = MAKO_LOCAL_OK;
    } else {
      switch (failure) {
      case Transaction::preinstall_failure::hook_rejected:
        status = MAKO_LOCAL_COMMIT_HOOK_REJECTED;
        break;
      case Transaction::preinstall_failure::timestamp_exhausted:
        status = MAKO_LOCAL_TIMESTAMP_EXHAUSTED;
        break;
      case Transaction::preinstall_failure::none:
        status = MAKO_LOCAL_CONFLICT;
        break;
      default:
        poison_transaction(txn);
        return pack_fast_terminal_result(MAKO_LOCAL_WORKER_POISONED,
                                         MAKO_LOCAL_WORKER_POISONED);
      }
    }
  } catch (...) {
    poison_transaction(txn);
    return pack_fast_terminal_result(MAKO_LOCAL_WORKER_POISONED,
                                     MAKO_LOCAL_WORKER_POISONED);
  }

  recycle_txn(txn);
  return pack_fast_terminal_result(status, MAKO_LOCAL_OK);
}

MAKO_RUST_FAST_DEFINITION_HIDDEN uint64_t
mako_rust_fast_txn_commit_and_destroy(mako_local_txn *txn) noexcept {
  // This build-private terminal entry is unsafe by contract. Transaction's
  // !Send/!Sync marker and consuming Rust call prove these invariants;
  // repeating their opaque-handle diagnostics was more work than the checked
  // commit+destroy pair this entry replaces.
  assert(txn != nullptr);
  assert(on_owner_thread(txn));
  assert(!txn->poisoned);
  assert(!local_worker_poisoned);
  assert(txn->active);
  assert(local_active_txn == txn);
  assert(txn->fast_table_impl != nullptr);

  // A nonempty sealed record plan may only commit through the serializer. A
  // cap-rejected plan likewise has no valid caller storage. Definite abort is
  // safer than permitting a trusted-wrapper bug to commit without durability.
  if (txn->record_plan_sealed &&
      (!txn->record_plan_ready || txn->record_plan_ops != 0)) {
    if (!abort_and_finish(txn, cleanup_boundary::abort))
      return pack_fast_terminal_result(MAKO_LOCAL_WORKER_POISONED,
                                       MAKO_LOCAL_WORKER_POISONED);
    recycle_txn(txn);
    return pack_fast_terminal_result(MAKO_LOCAL_INVALID_ARGUMENT,
                                     MAKO_LOCAL_OK);
  }

  try {
#if defined(MAKO_LOCAL_TEST_HOOKS)
    arm_native_cleanup_failure_if_requested(cleanup_boundary::commit);
#endif
    const bool committed = Sto::try_commit_no_paxos();
    finish_txn_known<true>(txn);
    recycle_txn(txn);
    return pack_fast_terminal_result(
        committed ? MAKO_LOCAL_OK : MAKO_LOCAL_CONFLICT, MAKO_LOCAL_OK);
  } catch (...) {
    // try_commit may have entered Transaction::stop(). Never retry cleanup or
    // release the stable staging pool when its progress is uncertain.
    poison_transaction(txn);
    return pack_fast_terminal_result(MAKO_LOCAL_WORKER_POISONED,
                                     MAKO_LOCAL_WORKER_POISONED);
  }
}

MAKO_RUST_FAST_DEFINITION_HIDDEN uint64_t
mako_rust_fast_txn_commit_with_hook_and_destroy(
    mako_local_txn *txn, mako_local_post_validate_hook hook,
    void *context) noexcept {
  assert(txn != nullptr);
  assert(hook != nullptr);
  assert(on_owner_thread(txn));
  assert(!txn->poisoned);
  assert(!local_worker_poisoned);
  assert(txn->active);
  assert(local_active_txn == txn);
  assert(txn->fast_table_impl != nullptr);

  int status = MAKO_LOCAL_INTERNAL;
  try {
#if defined(MAKO_LOCAL_TEST_HOOKS)
    arm_native_cleanup_failure_if_requested(cleanup_boundary::commit);
#endif
    Transaction::preinstall_failure failure =
        Transaction::preinstall_failure::none;
    post_validate_bridge bridge{hook, context};
    const bool committed = Sto::try_commit_no_paxos(
        invoke_post_validate_hook, &bridge, &failure);
    finish_txn_known<true>(txn);

    if (committed) {
      status = MAKO_LOCAL_OK;
    } else {
      switch (failure) {
      case Transaction::preinstall_failure::hook_rejected:
        status = MAKO_LOCAL_COMMIT_HOOK_REJECTED;
        break;
      case Transaction::preinstall_failure::timestamp_exhausted:
        status = MAKO_LOCAL_TIMESTAMP_EXHAUSTED;
        break;
      case Transaction::preinstall_failure::none:
        status = MAKO_LOCAL_CONFLICT;
        break;
      default:
        poison_transaction(txn);
        return pack_fast_terminal_result(MAKO_LOCAL_WORKER_POISONED,
                                         MAKO_LOCAL_WORKER_POISONED);
      }
    }
  } catch (...) {
    // try_commit may have entered Transaction::stop(). Never retry cleanup or
    // release the stable staging pool when its progress is uncertain.
    poison_transaction(txn);
    return pack_fast_terminal_result(MAKO_LOCAL_WORKER_POISONED,
                                     MAKO_LOCAL_WORKER_POISONED);
  }

  recycle_txn(txn);
  return pack_fast_terminal_result(status, MAKO_LOCAL_OK);
}

MAKO_RUST_FAST_DEFINITION_HIDDEN uint64_t
mako_rust_fast_txn_abort_and_destroy(mako_local_txn *txn) noexcept {
  if (txn == nullptr) {
    return pack_fast_terminal_result(MAKO_LOCAL_INVALID_ARGUMENT,
                                     MAKO_LOCAL_OK);
  }
  if (!on_owner_thread(txn)) {
    return pack_fast_terminal_result(MAKO_LOCAL_WRONG_THREAD,
                                     MAKO_LOCAL_WRONG_THREAD);
  }
  if (txn->poisoned || local_worker_poisoned) {
    return pack_fast_terminal_result(MAKO_LOCAL_WORKER_POISONED,
                                     MAKO_LOCAL_WORKER_POISONED);
  }

  try {
    if (txn->active) {
      if (local_active_txn != txn) {
        // As in fast commit, neither abort nor recycling is safe once the
        // facade and ambient STO state disagree. Quarantine instead of
        // stranding an active transaction after the consuming Rust call.
        poison_worker(txn);
        return pack_fast_terminal_result(MAKO_LOCAL_WORKER_POISONED,
                                         MAKO_LOCAL_WORKER_POISONED);
      }
      if (!abort_and_finish(txn, cleanup_boundary::abort)) {
        return pack_fast_terminal_result(MAKO_LOCAL_WORKER_POISONED,
                                         MAKO_LOCAL_WORKER_POISONED);
      }
    }
    recycle_txn(txn);
    return pack_fast_terminal_result(MAKO_LOCAL_OK, MAKO_LOCAL_OK);
  } catch (...) {
    poison_transaction(txn);
    return pack_fast_terminal_result(MAKO_LOCAL_WORKER_POISONED,
                                     MAKO_LOCAL_WORKER_POISONED);
  }
}

#if defined(MAKO_LOCAL_TEST_HOOKS)
MAKO_RUST_FAST_DEFINITION_HIDDEN uint64_t
mako_rust_fast_test_record_validation_tickets(
    const mako_local_db *db) noexcept {
  return db == nullptr
             ? 0
             : db->record_validation_next.value.load(std::memory_order_acquire);
}

MAKO_RUST_FAST_DEFINITION_HIDDEN uint64_t
mako_rust_fast_test_record_validation_wait_observations(
    const mako_local_db *db) noexcept {
  return db == nullptr
             ? 0
             : db->record_validation_wait_observations.load(
                   std::memory_order_acquire);
}
#endif

#undef MAKO_RUST_FAST_DEFINITION_HIDDEN

}  // extern "C"
