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
#include <utility>
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
static_assert(__atomic_always_lock_free(sizeof(uint64_t), nullptr),
              "the private Rust/native queue-tail seam requires lock-free u64 atomics");

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
  // Immutable from successful cache-order claim until close. A transaction
  // keeps the facade alive, so hot terminals can enforce mode locally without
  // loading the process-wide owner pointer.
  uint32_t cache_order_mode = 0;
  std::mutex tables_mu;
  std::unordered_map<std::string, std::unique_ptr<mako_local_table>> tables;
};

// One cache-order namespace may own the process-wide dense field at a time.
// Ordinary LocalDb facades and non-cache timestamp users do not claim it.
static std::atomic<mako_local_db *> active_cache_order_db{nullptr};
static std::mutex active_cache_order_mu;

// @safe - The claim publishes an immutable mode before any cache transaction
// can begin, and facade lifetime excludes close while a terminal is active.
bool packed_cache_order_allowed(const mako_local_db *db) noexcept {
  return db != nullptr &&
      db->cache_order_mode == MAKO_RUST_FAST_CACHE_ORDER_CONCURRENT;
}

// @safe - Unclaimed low-level facades retain their legacy test/embedding
// behavior. A claimed namespace admits Rust-sequence terminals only in the
// immutable single-producer mode.
bool rust_sequence_cache_order_allowed(const mako_local_db *db) noexcept {
  return db != nullptr &&
      (db->cache_order_mode == 0 ||
       db->cache_order_mode == MAKO_RUST_FAST_CACHE_ORDER_SINGLE_PRODUCER);
}

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
  // Trailing defaulted member preserves the existing aggregate initialization
  // and makes an omitted extension select the safe default.
  uint32_t record_plan_checksum_mode =
      MAKO_RUST_FAST_RECORD_CHECKSUM_CRC32C;
  // Unsafe private fast-put witness. Its spans point into the stable
  // TransItem/tree row created by the one and only mutation. A later mutation
  // or read which can grow/reorganize transaction state invalidates it before
  // entering MassTrans, after which preflight and serialization fall back to
  // a canonical transaction walk.
  size_t record_fast_mutation_bytes = 0;
  Transaction::canonical_write_view record_fast_write{};
  bool record_fast_path_eligible = true;
  // Nonzero only while the immediately preceding trusted fast Put remains a
  // valid unchecked-v4 fused-terminal candidate. This consumes existing tail
  // padding in the facade and lets the release terminal test one scalar; the
  // richer borrowed witness above remains available to general serialization.
  uint32_t record_fused_candidate_bytes = 0;
};

/* Queue-owned storage for the trusted callback-free one-Put terminal. The
 * fields are deliberately non-atomic. Rust's single producer exclusively owns
 * the sequence generation until it release-publishes the queue entry; the
 * consumer acquire-observes that publication, completes pool_release, then
 * release-publishes applied_tail; producer reuse follows an acquire of that
 * tail. Those external edges order every access to one holder. Calling these
 * entry points without that proof is a cross-language data race and undefined
 * behavior. */
struct mako_rust_fast_one_put_holder_pool {
  static constexpr size_t kInlineKeyBytes = 32;
  static constexpr size_t kHotInlineKeyBytes = 8;

  enum class holder_state : uint8_t { free, sealed };
  enum class key_storage : uint8_t { hot_inline, inline_key, overflow };

  struct alignas(64) holder {
    // A standard-library string object of at most 32 bytes plus the common
    // 8-byte-key metadata occupies one prefetched line. Correctness does not
    // depend on that implementation size; larger string implementations use
    // the same fields with a less compact layout.
    std::string encoded_value;
    uint64_t sequence = 0;
    uint64_t table_id = 0;
    uint32_t mako_timestamp = 0;
    uint16_t key_len = 0;
    key_storage key_location = key_storage::hot_inline;
    holder_state state = holder_state::free;
    std::array<uint8_t, kHotInlineKeyBytes> hot_inline_key{};
    std::string overflow_key;
    std::array<uint8_t, kInlineKeyBytes> inline_key{};
  };

  size_t capacity = 0;
  size_t mask = 0;
  std::unique_ptr<holder[]> holders;
};

static_assert(mako_rust_fast_one_put_holder_pool::kInlineKeyBytes >= 16);
static_assert(MAKO_LOCAL_MAX_KEY_BYTES <= UINT16_MAX);
static_assert(
    sizeof(std::string) > 32 ||
    offsetof(mako_rust_fast_one_put_holder_pool::holder, hot_inline_key) +
            mako_rust_fast_one_put_holder_pool::kHotInlineKeyBytes <=
        64);
static_assert(noexcept(std::declval<std::string &>().swap(
    std::declval<std::string &>())));

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

constexpr bool is_nonzero_power_of_two(size_t value) noexcept {
  return value != 0 && (value & (value - 1)) == 0;
}

using one_put_holder = mako_rust_fast_one_put_holder_pool::holder;
using one_put_holder_state =
    mako_rust_fast_one_put_holder_pool::holder_state;

// @unsafe - Exact generation ownership, not this masked lookup, prevents an
// old and a new sequence from concurrently naming the same holder.
one_put_holder &one_put_holder_for(
    mako_rust_fast_one_put_holder_pool *pool, uint64_t sequence) noexcept {
  assert(pool != nullptr);
  assert(sequence != 0);
  assert(is_nonzero_power_of_two(pool->capacity));
  return pool->holders[static_cast<size_t>(sequence - 1) & pool->mask];
}

const one_put_holder &one_put_holder_for(
    const mako_rust_fast_one_put_holder_pool *pool,
    uint64_t sequence) noexcept {
  assert(pool != nullptr);
  assert(sequence != 0);
  assert(is_nonzero_power_of_two(pool->capacity));
  return pool->holders[static_cast<size_t>(sequence - 1) & pool->mask];
}

constexpr uint64_t kFastPutRecordBytesMax = UINT64_MAX >> 33;

constexpr uint64_t pack_fast_put_result(
    int status, bool created = false,
    uint32_t unchecked_record_bytes = 0) noexcept {
  return static_cast<uint64_t>(static_cast<uint32_t>(status)) |
         (created ? MAKO_RUST_FAST_PUT_CREATED_BIT : UINT64_C(0)) |
         (static_cast<uint64_t>(unchecked_record_bytes) << 33);
}

constexpr uint64_t pack_fast_terminal_result(int status,
                                             int cleanup_status) noexcept {
  return static_cast<uint64_t>(static_cast<uint32_t>(status)) |
         (static_cast<uint64_t>(static_cast<uint32_t>(cleanup_status)) << 32);
}

constexpr uint64_t pack_preselected_record_state(uint32_t mako_timestamp,
                                                 bool record_written) noexcept {
  return static_cast<uint64_t>(mako_timestamp) |
         (record_written ? UINT64_C(1) << 32 : UINT64_C(0));
}

constexpr uint64_t
pack_fused_holder_control_word(uint32_t code, uint32_t payload = 0) noexcept {
  return static_cast<uint64_t>(code) | (static_cast<uint64_t>(payload) << 32);
}

constexpr mako_rust_fast_preselected_record_result
pack_fused_holder_cold_result(
    mako_rust_fast_preselected_record_result result,
    uint32_t exact_record_bytes) noexcept {
  assert(exact_record_bytes != 0);
  assert(exact_record_bytes <= kFastPutRecordBytesMax);
  assert(MAKO_RUST_FAST_PRESELECTED_RECORD_RESERVED(result) == 0);
  result.record_state |= static_cast<uint64_t>(exact_record_bytes) << 33;
  return result;
}

static_assert(MAKO_RUST_FAST_PUT_STATUS(pack_fast_put_result(
                  MAKO_LOCAL_VALUE_TOO_LARGE)) == MAKO_LOCAL_VALUE_TOO_LARGE);
static_assert(MAKO_RUST_FAST_PUT_CREATED(pack_fast_put_result(MAKO_LOCAL_OK,
                                                              true)) == 1);
static_assert(MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(
                  pack_fast_put_result(MAKO_LOCAL_OK, true, 12345)) == 12345);
static_assert(MAKO_RUST_FAST_TERMINAL_STATUS(pack_fast_terminal_result(
                  MAKO_LOCAL_COMMIT_HOOK_REJECTED, MAKO_LOCAL_OK)) ==
              MAKO_LOCAL_COMMIT_HOOK_REJECTED);
static_assert(MAKO_RUST_FAST_CLEANUP_STATUS(pack_fast_terminal_result(
                  MAKO_LOCAL_OK, MAKO_LOCAL_WORKER_POISONED)) ==
              MAKO_LOCAL_WORKER_POISONED);
static_assert(sizeof(mako_rust_fast_preselected_record_result) ==
              2 * sizeof(uint64_t));
static_assert(alignof(mako_rust_fast_preselected_record_result) ==
              alignof(uint64_t));
static_assert(sizeof(mako_rust_fast_native_ordered_arena_control) == 56);
static_assert(alignof(mako_rust_fast_native_ordered_arena_control) ==
              alignof(uint64_t));
static_assert(offsetof(mako_rust_fast_native_ordered_arena_control,
                       next_bound) == 0);
static_assert(offsetof(mako_rust_fast_native_ordered_arena_control,
                       unhealthy) == 8);
static_assert(offsetof(mako_rust_fast_native_ordered_arena_control,
                       publication_base) == 16);
static_assert(offsetof(mako_rust_fast_native_ordered_arena_control,
                       arena_base) == 24);
static_assert(offsetof(mako_rust_fast_native_ordered_arena_control,
                       publication_mask) == 32);
static_assert(offsetof(mako_rust_fast_native_ordered_arena_control,
                       publication_shift) == 40);
static_assert(offsetof(mako_rust_fast_native_ordered_arena_control,
                       publication_stride) == 44);
static_assert(offsetof(mako_rust_fast_native_ordered_arena_control,
                       arena_stride) == 48);
static_assert(offsetof(mako_rust_fast_native_ordered_arena_control,
                       arena_block_bytes) == 52);
static_assert(sizeof(mako_rust_fast_native_ordered_arena_result) == 24);
static_assert(alignof(mako_rust_fast_native_ordered_arena_result) ==
              alignof(uint64_t));
static_assert(offsetof(mako_rust_fast_native_ordered_arena_result, terminal) ==
              0);
static_assert(offsetof(mako_rust_fast_native_ordered_arena_result,
                       ordered_sequence) == 8);
static_assert(offsetof(mako_rust_fast_native_ordered_arena_result,
                       record_state) == 16);
static_assert(sizeof(mako_rust_fast_spsc_holder_control) == 72);
static_assert(alignof(mako_rust_fast_spsc_holder_control) == alignof(uint64_t));
static_assert(offsetof(mako_rust_fast_spsc_holder_control, pool) == 0);
static_assert(offsetof(mako_rust_fast_spsc_holder_control, holder_base) == 8);
static_assert(offsetof(mako_rust_fast_spsc_holder_control, holder_mask) == 16);
static_assert(offsetof(mako_rust_fast_spsc_holder_control, acknowledged) == 24);
static_assert(offsetof(mako_rust_fast_spsc_holder_control, unhealthy) == 32);
static_assert(offsetof(mako_rust_fast_spsc_holder_control, capacity) == 40);
static_assert(offsetof(mako_rust_fast_spsc_holder_control, max_record_bytes) ==
              48);
static_assert(offsetof(mako_rust_fast_spsc_holder_control, reserved) == 52);
static_assert(offsetof(mako_rust_fast_spsc_holder_control, cold_out) == 56);
static_assert(sizeof(mako_rust_fast_one_put_holder_view) == 48);
static_assert(alignof(mako_rust_fast_one_put_holder_view) == alignof(uint64_t));
static_assert(offsetof(mako_rust_fast_one_put_holder_view, sequence) == 0);
static_assert(offsetof(mako_rust_fast_one_put_holder_view, table_id) == 8);
static_assert(offsetof(mako_rust_fast_one_put_holder_view, key) == 16);
static_assert(offsetof(mako_rust_fast_one_put_holder_view, value) == 24);
static_assert(offsetof(mako_rust_fast_one_put_holder_view, key_len) == 32);
static_assert(offsetof(mako_rust_fast_one_put_holder_view, value_len) == 36);
static_assert(offsetof(mako_rust_fast_one_put_holder_view, mako_timestamp) ==
              40);
static_assert(offsetof(mako_rust_fast_one_put_holder_view, reserved) == 44);
constexpr mako_rust_fast_preselected_record_result
    kPreselectedRecordLayoutProbe{
        pack_fast_terminal_result(MAKO_LOCAL_OK, MAKO_LOCAL_WORKER_POISONED),
        pack_preselected_record_state(12345, true)};
static_assert(MAKO_RUST_FAST_TERMINAL_STATUS(
                  kPreselectedRecordLayoutProbe.terminal) == MAKO_LOCAL_OK);
static_assert(
    MAKO_RUST_FAST_CLEANUP_STATUS(kPreselectedRecordLayoutProbe.terminal) ==
    MAKO_LOCAL_WORKER_POISONED);
static_assert(MAKO_RUST_FAST_PRESELECTED_RECORD_TIMESTAMP(
                  kPreselectedRecordLayoutProbe) == 12345);
static_assert(MAKO_RUST_FAST_PRESELECTED_RECORD_WRITTEN(
                  kPreselectedRecordLayoutProbe) == 1);
static_assert(MAKO_RUST_FAST_PRESELECTED_RECORD_RESERVED(
                  kPreselectedRecordLayoutProbe) == 0);
static_assert(MAKO_RUST_FAST_FUSED_HOLDER_CODE(pack_fused_holder_control_word(
                  MAKO_RUST_FAST_FUSED_HOLDER_UNTOUCHED_NEED_SLOW, 12345)) ==
              MAKO_RUST_FAST_FUSED_HOLDER_UNTOUCHED_NEED_SLOW);
static_assert(
    MAKO_RUST_FAST_FUSED_HOLDER_PAYLOAD(pack_fused_holder_control_word(
        MAKO_RUST_FAST_FUSED_HOLDER_UNTOUCHED_NEED_SLOW, 12345)) == 12345);

// Same-build mirrors of the two Rust hot allocations. Native never constructs
// these types; their offsets and sizes make every raw access below explicit.
struct alignas(64) rust_publication_cell_layout {
  uint64_t turn;
  uint32_t mako_timestamp;
  uint32_t timestamp_padding;
  size_t record_bytes;
  std::array<uint8_t, 40> padding;
};
struct alignas(64) rust_record_arena_block_layout {
  std::array<uint8_t, 256> bytes;
};
static_assert(sizeof(rust_publication_cell_layout) == 64);
static_assert(alignof(rust_publication_cell_layout) == 64);
static_assert(offsetof(rust_publication_cell_layout, turn) == 0);
static_assert(offsetof(rust_publication_cell_layout, mako_timestamp) == 8);
static_assert(offsetof(rust_publication_cell_layout, record_bytes) == 16);
static_assert(sizeof(rust_record_arena_block_layout) == 256);
static_assert(alignof(rust_record_arena_block_layout) == 64);
static_assert(offsetof(rust_record_arena_block_layout, bytes) == 0);

constexpr std::array<uint8_t, 8> kCacheRecordCrc32cMagic{'M', 'A', 'K', 'O',
                                                         'C', 'M', 'T', '\0'};
constexpr std::array<uint8_t, 8> kCacheRecordUncheckedMagic{
    'M', 'A', 'K', 'O', 'N', 'O', 'C', '\0'};
constexpr uint16_t kCacheRecordCrc32cVersion = 3;
constexpr uint16_t kCacheRecordUncheckedVersion = 4;
constexpr uint8_t kCacheRecordPutTag = 1;
constexpr uint8_t kCacheRecordDeleteTag = 2;
constexpr size_t kCacheRecordHeaderBytes = 8 + 2 + 8 + 4 + 4;
constexpr size_t kCacheRecordOperationHeaderBytes = 1 + 8 + 4 + 4;
constexpr size_t kCacheRecordCrcBytes = 4;
static_assert(kCacheRecordHeaderBytes + kCacheRecordOperationHeaderBytes +
                  MAKO_LOCAL_MAX_KEY_BYTES + MAKO_LOCAL_MAX_VALUE_BYTES <=
              kFastPutRecordBytesMax);

bool valid_indexed_byte_layout(const void *base, size_t last_index,
                               size_t stride, size_t final_extent) noexcept {
  if (base == nullptr || stride == 0 || final_extent == 0 ||
      last_index > (std::numeric_limits<size_t>::max() - final_extent) / stride)
    return false;
  const size_t last_end = last_index * stride + final_extent;
  return reinterpret_cast<uintptr_t>(base) <=
         std::numeric_limits<uintptr_t>::max() - last_end;
}

bool valid_native_ordered_arena_control(
    const mako_rust_fast_native_ordered_arena_control *control,
    uint32_t expected_record_bytes) noexcept {
  if (control == nullptr ||
      reinterpret_cast<uintptr_t>(control) %
              alignof(mako_rust_fast_native_ordered_arena_control) !=
          0)
    return false;
  if (control->next_bound == nullptr || control->unhealthy == nullptr ||
      control->publication_base == nullptr || control->arena_base == nullptr ||
      reinterpret_cast<uintptr_t>(control->next_bound) % alignof(uint64_t) !=
          0 ||
      reinterpret_cast<uintptr_t>(control->publication_base) %
              alignof(rust_publication_cell_layout) !=
          0 ||
      reinterpret_cast<uintptr_t>(control->arena_base) %
              alignof(rust_record_arena_block_layout) !=
          0)
    return false;

  if (control->publication_mask == std::numeric_limits<size_t>::max())
    return false;
  const size_t capacity = control->publication_mask + 1;
  if (!is_nonzero_power_of_two(capacity) || capacity < 4 ||
      control->publication_shift >= std::numeric_limits<size_t>::digits ||
      (size_t{1} << control->publication_shift) != capacity)
    return false;
  if (control->publication_stride != sizeof(rust_publication_cell_layout) ||
      control->arena_stride != sizeof(rust_record_arena_block_layout) ||
      control->arena_block_bytes == 0 ||
      control->arena_block_bytes > sizeof(rust_record_arena_block_layout) ||
      expected_record_bytes == 0 ||
      expected_record_bytes > control->arena_block_bytes)
    return false;
  return valid_indexed_byte_layout(
             control->publication_base, control->publication_mask,
             control->publication_stride,
             sizeof(rust_publication_cell_layout)) &&
         valid_indexed_byte_layout(control->arena_base,
                                   control->publication_mask,
                                   control->arena_stride,
                                   control->arena_block_bytes);
}

constexpr bool valid_record_checksum_mode(uint32_t mode) noexcept {
  return mode == MAKO_RUST_FAST_RECORD_CHECKSUM_CRC32C ||
         mode == MAKO_RUST_FAST_RECORD_CHECKSUM_NONE;
}

constexpr size_t record_trailer_bytes(uint32_t checksum_mode) noexcept {
  return checksum_mode == MAKO_RUST_FAST_RECORD_CHECKSUM_CRC32C
             ? kCacheRecordCrcBytes
             : 0;
}
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

// @unsafe - Fixed-width stores intentionally accept unaligned record fields.
// memcpy gives them defined C++ aliasing/alignment semantics and compilers
// lower the constant extents to one unaligned store on supported targets.
void store_u16_be_unaligned(uint8_t *destination, uint16_t value) noexcept {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  value = __builtin_bswap16(value);
#elif __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
#error "unsupported byte order"
#endif
  std::memcpy(destination, &value, sizeof(value));
}

void store_u32_be_unaligned(uint8_t *destination, uint32_t value) noexcept {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  value = __builtin_bswap32(value);
#elif __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
#error "unsupported byte order"
#endif
  std::memcpy(destination, &value, sizeof(value));
}

void store_u64_be_unaligned(uint8_t *destination, uint64_t value) noexcept {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  value = __builtin_bswap64(value);
#elif __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
#error "unsupported byte order"
#endif
  std::memcpy(destination, &value, sizeof(value));
}

struct record_shape {
  size_t bytes = 0;
  uint32_t operations = 0;
  uint32_t checksum_mode = MAKO_RUST_FAST_RECORD_CHECKSUM_CRC32C;
};

void invalidate_fast_record_witness(mako_local_txn *txn) noexcept {
  assert(txn != nullptr);
  txn->record_fast_path_eligible = false;
  txn->record_fast_mutation_bytes = 0;
  txn->record_fast_write = Transaction::canonical_write_view{};
  txn->record_fused_candidate_bytes = 0;
}

// Reads which precede the first write do not hold a borrowed record view and
// may leave the one-Put shortcut eligible: the later Put captures spans only
// after that read-side transaction growth has completed. Once a Put has lent
// us spans, however, any further point/range read retires them conservatively
// before it can grow or reorganize transaction state.
void invalidate_borrowed_fast_record_witness(mako_local_txn *txn) noexcept {
  assert(txn != nullptr);
  if (txn->record_fast_mutation_bytes != 0 ||
      static_cast<uint8_t>(txn->record_fast_write.op) != 0)
    invalidate_fast_record_witness(txn);
}

// @unsafe - Validates the cached borrowed spans without dereferencing them.
// The private fast-put contract and transaction seal keep the underlying
// TransItem/tree row alive and immutable until terminal cleanup. Exact length
// checks tie this shortcut to the same framing that a canonical walk derives.
bool derive_fast_record_shape(const mako_local_txn *txn,
                              uint32_t checksum_mode,
                              record_shape *shape) noexcept {
  if (txn == nullptr || shape == nullptr ||
      !txn->record_fast_path_eligible ||
      !valid_record_checksum_mode(checksum_mode))
    return false;

  const size_t base =
      kCacheRecordHeaderBytes + record_trailer_bytes(checksum_mode);
  const auto &write = txn->record_fast_write;
  if (static_cast<uint8_t>(write.op) == 0) {
    if (txn->record_fast_mutation_bytes != 0)
      return false;
    *shape = record_shape{base, 0, checksum_mode};
    return true;
  }
  if (write.op != Transaction::canonical_write_view::operation::put ||
      write.key_length > UINT32_MAX || write.value_length > UINT32_MAX ||
      (write.key_length != 0 && write.key == nullptr) ||
      (write.value_length != 0 && write.value == nullptr))
    return false;
  if (write.key_length >
      SIZE_MAX - kCacheRecordOperationHeaderBytes)
    return false;
  const size_t key_end =
      kCacheRecordOperationHeaderBytes + write.key_length;
  if (write.value_length > SIZE_MAX - key_end)
    return false;
  const size_t mutation_bytes = key_end + write.value_length;
  if (mutation_bytes != txn->record_fast_mutation_bytes ||
      mutation_bytes > SIZE_MAX - base)
    return false;
  *shape = record_shape{base + mutation_bytes, 1, checksum_mode};
  return true;
}

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

bool derive_record_shape(const Transaction *native_txn, uint32_t checksum_mode,
                         record_shape *shape) noexcept {
  if (native_txn == nullptr || shape == nullptr ||
      !valid_record_checksum_mode(checksum_mode))
    return false;
  *shape = record_shape{kCacheRecordHeaderBytes +
                            record_trailer_bytes(checksum_mode),
                        0, checksum_mode};
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
                            uint8_t *record,
                            const Transaction::canonical_write_view
                                *direct_write = nullptr) noexcept {
  assert(native_txn != nullptr);
  assert(sequence != 0);
  assert(mako_timestamp != 0);
  assert(shape.operations != 0);
  assert(valid_record_checksum_mode(shape.checksum_mode));
  assert(shape.bytes >=
         kCacheRecordHeaderBytes + record_trailer_bytes(shape.checksum_mode));
  assert(record != nullptr);

  uint8_t *cursor = record;
  const auto &magic =
      shape.checksum_mode == MAKO_RUST_FAST_RECORD_CHECKSUM_CRC32C
          ? kCacheRecordCrc32cMagic
          : kCacheRecordUncheckedMagic;
  std::memcpy(cursor, magic.data(), magic.size());
  cursor += magic.size();
  write_u16_be(cursor,
               shape.checksum_mode == MAKO_RUST_FAST_RECORD_CHECKSUM_CRC32C
                   ? kCacheRecordCrc32cVersion
                   : kCacheRecordUncheckedVersion);
  write_u64_be(cursor, sequence);
  write_u32_be(cursor, mako_timestamp);
  write_u32_be(cursor, shape.operations);

  record_writer writer{
      cursor, record + shape.bytes - record_trailer_bytes(shape.checksum_mode)};
  uint32_t visited = 0;
  const bool wrote = direct_write == nullptr
      ? native_txn->visit_local_canonical_writes(
            write_record_mutation, &writer, &visited)
      : (write_record_mutation(&writer, *direct_write) &&
         (++visited != 0));
  if (!wrote ||
      visited != shape.operations || writer.cursor != writer.operations_end)
    return false;

  cursor = writer.operations_end;
  if (shape.checksum_mode == MAKO_RUST_FAST_RECORD_CHECKSUM_CRC32C) {
    const uint32_t checksum =
        crc32c(record, shape.bytes - kCacheRecordCrcBytes);
    write_u32_be(cursor, checksum);
  }
  return cursor == record + shape.bytes;
}

// @unsafe - Specialized allocation-free encoder for the fused private
// one-Put/NONE terminal. The terminal has already rederived and sealed this
// exact shape before attempting write locking, and bind_hook has supplied at
// least exact_record_bytes stable writable bytes. No visitor, callback, CRC,
// or cursor-by-cursor integer loop remains on this path.
bool serialize_unchecked_one_put_cache_record(
    uint64_t sequence, uint32_t mako_timestamp, size_t exact_record_bytes,
    uint8_t *record,
    const Transaction::canonical_write_view &write) noexcept {
  assert(sequence != 0);
  assert(mako_timestamp != 0);
  assert(record != nullptr);
  assert(write.op == Transaction::canonical_write_view::operation::put);
  assert(write.key_length <= UINT32_MAX);
  assert(write.value_length <= UINT32_MAX);
  assert(write.key_length == 0 || write.key != nullptr);
  assert(write.value_length == 0 || write.value != nullptr);
  assert(exact_record_bytes == kCacheRecordHeaderBytes +
                                     kCacheRecordOperationHeaderBytes +
                                     write.key_length + write.value_length);

  std::memcpy(record, kCacheRecordUncheckedMagic.data(),
              kCacheRecordUncheckedMagic.size());
  store_u16_be_unaligned(record + 8, kCacheRecordUncheckedVersion);
  store_u64_be_unaligned(record + 10, sequence);
  store_u32_be_unaligned(record + 18, mako_timestamp);
  store_u32_be_unaligned(record + 22, 1);
  record[26] = kCacheRecordPutTag;
  store_u64_be_unaligned(record + 27, write.table_id);
  store_u32_be_unaligned(record + 35,
                         static_cast<uint32_t>(write.key_length));
  store_u32_be_unaligned(record + 39,
                         static_cast<uint32_t>(write.value_length));
  uint8_t *payload = record + kCacheRecordHeaderBytes +
                     kCacheRecordOperationHeaderBytes;
  if (write.key_length != 0) {
    std::memcpy(payload, write.key, write.key_length);
    payload += write.key_length;
  }
  if (write.value_length != 0)
    std::memcpy(payload, write.value, write.value_length);
  return true;
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

[[gnu::cold, gnu::noinline]] void release_oversized_encoded_values(
    mako_local_txn *txn) noexcept {
  assert(txn->encoded_values_require_release);
  for (size_t index = 0; index != txn->encoded_values_used; ++index) {
    std::string &encoded = txn->encoded_values[index];
    if (encoded.capacity() <= kMaximumRetainedEncodedValueCapacity)
      continue;
    std::string{}.swap(encoded);
  }
  txn->encoded_values_require_release = false;
}

[[gnu::always_inline]] inline void finish_encoded_values(
    mako_local_txn *txn) noexcept {
  // The common small-transaction path retains both size and capacity. The
  // next payload copy and metadata initialization overwrite every byte before
  // the slot is lent to STO again, so no clearing scan is required.
  if (txn->encoded_values_require_release) [[unlikely]]
    release_oversized_encoded_values(txn);
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
  txn->record_plan_checksum_mode =
      MAKO_RUST_FAST_RECORD_CHECKSUM_CRC32C;
  txn->record_plan_sealed = false;
  txn->record_plan_ready = false;
  txn->record_fast_mutation_bytes = 0;
  txn->record_fast_write = Transaction::canonical_write_view{};
  txn->record_fast_path_eligible = true;
  txn->record_fused_candidate_bytes = 0;
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

[[gnu::cold, gnu::noinline]] void recycle_txn_overflow(
    mako_local_txn *txn) noexcept {
  delete txn;
}

[[gnu::always_inline]] inline void recycle_txn(mako_local_txn *txn) noexcept {
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
    recycle_txn_overflow(txn);
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
  assert(txn->record_plan_checksum_mode ==
         MAKO_RUST_FAST_RECORD_CHECKSUM_CRC32C);
  assert(!txn->encoded_values_require_release);
  assert(!txn->record_plan_sealed);
  assert(!txn->record_plan_ready);
  assert(txn->record_fast_mutation_bytes == 0);
  assert(static_cast<uint8_t>(txn->record_fast_write.op) == 0);
  assert(txn->record_fast_path_eligible);
  assert(txn->record_fused_candidate_bytes == 0);
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
  // Range reads can append row/predicate TransItems and grow their backing
  // transaction storage, invalidating borrowed key/value spans retained by a
  // preceding fast put. Forward and reverse scans share this implementation.
  invalidate_borrowed_fast_record_witness(txn);
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
  bool use_direct_write = false;
  bool use_unchecked_one_put_serializer = false;
  // Native-ordered concurrent terminals lend the queue health word. The
  // packed process state is the sole sequence allocator; the legacy Rust
  // queue tail remains in the compatibility ABI but is not part of concurrent
  // allocation or descriptor discovery.
  const uint8_t *native_unhealthy = nullptr;
  uint64_t *ordered_sequence_out = nullptr;
  uint32_t *ordered_timestamp_out = nullptr;
  bool assign_sequence_natively = false;
};

void record_validation_cpu_relax() noexcept {
#if defined(__i386__) || defined(__x86_64__)
  _mm_pause();
#else
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

// @unsafe - Legacy/general record terminals which have not opted into the
// claimed cache-order namespace retain their allocation-free FIFO gate.
void enter_record_validation_gate(void *opaque) noexcept {
  auto *bridge = static_cast<record_bind_bridge *>(opaque);
  assert(bridge != nullptr);
  assert(!bridge->validation_gate_held);
  mako_local_db *const db = bridge->txn->owner;
  const uint64_t ticket = db->record_validation_next.value.fetch_add(
      UINT64_C(1), std::memory_order_release);
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

// @unsafe - Claimed cache terminals use the packed general bit after the full
// STO write set is held. Restricted one-Put updates never own this bit.
void enter_packed_cache_order_gate(void *opaque) noexcept {
  auto *bridge = static_cast<record_bind_bridge *>(opaque);
  assert(bridge != nullptr);
  assert(!bridge->validation_gate_held);
  assert(packed_cache_order_allowed(bridge->txn->owner));
  Transaction::enter_cache_order_general();
  bridge->validation_gate_held = true;
}

// @unsafe - Paired only with enter_packed_cache_order_gate for this bridge;
// releasing the bit ends general certification before record serialization.
void leave_packed_cache_order_gate(void *opaque) noexcept {
  auto *bridge = static_cast<record_bind_bridge *>(opaque);
  assert(bridge != nullptr);
  assert(bridge->validation_gate_held);
  bridge->validation_gate_held = false;
  Transaction::leave_cache_order_general();
}

// @unsafe - This callback deliberately provides no mutual exclusion. The
// caller of the single-producer terminal must prove that no other cache-record
// terminal for this database is running or waiting for the whole call. Keeping
// a non-null Transaction gate is nevertheless essential: `held()` selects the
// repeated post-lock predicate validation, and release still separates the
// bind hook from after-leave serialization.
void enter_single_producer_record_validation_gate(void *opaque) noexcept {
  auto *bridge = static_cast<record_bind_bridge *>(opaque);
  assert(bridge != nullptr);
  assert(!bridge->validation_gate_held);
#ifndef NDEBUG
  mako_local_db *const db = bridge->txn->owner;
  assert(db->record_validation_next.value.load(std::memory_order_relaxed) ==
         db->record_validation_serving.value.load(std::memory_order_acquire));
#endif
  bridge->validation_gate_held = true;
}

void leave_single_producer_record_validation_gate(void *opaque) noexcept {
  auto *bridge = static_cast<record_bind_bridge *>(opaque);
  assert(bridge != nullptr);
  assert(bridge->validation_gate_held);
  bridge->validation_gate_held = false;
}

// Build-private state for the callback-free single-producer record terminal.
// The target is an invisible exact arena turn retained by Rust for the whole
// call. A nonzero timestamp is therefore also native's acceptance witness:
// after observing it, Rust must publish this sequence even on uncertainty.
struct preselected_one_put_bridge {
  mako_local_txn *txn;
  uint64_t sequence;
  uint8_t *record;
  size_t exact_record_bytes;
  uint32_t mako_timestamp = 0;
  bool record_written = false;
};

// Build-private state for the zero-copy holder terminal. Inline key bytes and
// every potentially allocating overflow-key operation are complete before
// validation. The post-validation hook therefore only captures Transaction's
// accepted timestamp. The holder remains FREE/invisible until the terminal
// transfers the staged value and seals all metadata after try_commit returns.
struct preselected_one_put_holder_bridge {
  mako_local_txn *txn;
  one_put_holder *holder;
  uint64_t sequence;
  uint64_t table_id;
  uint16_t key_len;
  mako_rust_fast_one_put_holder_pool::key_storage key_location;
  uint32_t mako_timestamp = 0;
  bool holder_sealed = false;
};

bool accept_preselected_one_put_holder(void *opaque,
                                       uint32_t timestamp) noexcept {
  auto *bridge = static_cast<preselected_one_put_holder_bridge *>(opaque);
  assert(bridge != nullptr);
  assert(bridge->txn != nullptr);
  assert(bridge->holder != nullptr);
  assert(timestamp != 0);
  assert(bridge->mako_timestamp == 0);
  bridge->mako_timestamp = timestamp;
  return true;
}

// @unsafe - The same-build one-Put witness proves a non-null stable key span
// through commit. Special-casing the overwhelmingly common eight-byte key
// makes this one unaligned load/store rather than a size-dispatched memcpy.
inline void copy_preselected_holder_inline_key(uint8_t *destination,
                                               const char *key,
                                               size_t key_len) noexcept {
  assert(destination != nullptr);
  assert(key_len == 0 || key != nullptr);
  if (key_len == sizeof(uint64_t)) {
    uint64_t word;
    std::memcpy(&word, key, sizeof(word));
    std::memcpy(destination, &word, sizeof(word));
  } else if (key_len != 0) {
    std::memcpy(destination, key, key_len);
  }
}

// @unsafe - Called only after try_commit_no_paxos has returned or unwound to
// its caller. Normal success has completed STO cleanup. The accepted cleanup
// failpoint throws at stop() entry, before it can observe or alter the stable
// staged StringWrapper; that transaction and worker are then quarantined and
// native cleanup is never re-entered. Thus no STO code can observe the
// allocation after this noexcept ownership transfer.
void seal_preselected_one_put_holder(
    preselected_one_put_holder_bridge *bridge) noexcept {
  assert(bridge != nullptr);
  mako_local_txn *const txn = bridge->txn;
  one_put_holder &holder = *bridge->holder;
#ifndef NDEBUG
  const auto &write = txn->record_fast_write;
  assert(bridge->mako_timestamp != 0);
  assert(!bridge->holder_sealed);
  assert(holder.state == one_put_holder_state::free);
  assert(holder.sequence == 0);
  assert(txn->encoded_values_used == 1);
  assert(txn->record_fast_path_eligible);
  assert(write.op == Transaction::canonical_write_view::operation::put);
  assert(write.table_id == bridge->table_id);
  assert(write.key_length == bridge->key_len);
  assert(write.value_length <= UINT32_MAX);
  assert(txn->encoded_values[0].size() ==
         write.value_length +
             static_cast<size_t>(mako::EXTRA_BITS_FOR_VALUE));
  assert(bridge->key_location !=
             mako_rust_fast_one_put_holder_pool::key_storage::overflow ||
         holder.overflow_key.size() == write.key_length);
#endif

  // Do not dereference record_fast_write.value here: inserts may point at a
  // tree row and successful cleanup has ended that borrow. The facade-owned
  // encoded string is the authoritative value allocation. Its exact shape was
  // established by the unsafe same-build candidate contract (and is asserted
  // above in diagnostic builds).
  holder.encoded_value.swap(txn->encoded_values[0]);
  // The swap rotates the holder's prior allocation into the pooled facade.
  // Retain that one bounded-by-public-max buffer so later holder generations
  // can rotate allocations without falling back to malloc on every large Put.
  txn->encoded_values_require_release = false;
  holder.sequence = bridge->sequence;
  holder.table_id = bridge->table_id;
  holder.key_len = bridge->key_len;
  holder.key_location = bridge->key_location;
  holder.mako_timestamp = bridge->mako_timestamp;
  holder.state = one_put_holder_state::sealed;
  bridge->holder_sealed = true;
}

// @unsafe - Runs only after the complete write set is locked and phase-2
// predicate/point-read validation succeeds. The direct one-Put shape and
// borrowed spans were rederived and sealed before commit. Serialization is
// complete before this hook returns true, hence before Transaction can enter
// phase 3. Only an accepted hook publishes its timestamp witness.
bool serialize_preselected_one_put_record(void *opaque,
                                          uint32_t timestamp) noexcept {
  auto *bridge = static_cast<preselected_one_put_bridge *>(opaque);
  assert(bridge != nullptr);
  assert(bridge->mako_timestamp == 0);
  assert(!bridge->record_written);
  const bool serialized = serialize_unchecked_one_put_cache_record(
      bridge->sequence, timestamp, bridge->exact_record_bytes,
      bridge->record, bridge->txn->record_fast_write);
  if (!serialized) return false;
  bridge->mako_timestamp = timestamp;
  bridge->record_written = true;
  return true;
}

// @unsafe - Binds Rust-owned uninitialized storage while the full native write
// set is locked and the ordered validation turn is held. Every potentially
// failing native shape check precedes the external dense-sequence assignment.
bool invoke_record_bind_hook(void *opaque, uint32_t timestamp) noexcept {
  auto *bridge = static_cast<record_bind_bridge *>(opaque);
  assert(bridge->validation_gate_held);
  // Preflight seals the transaction plan, and every subsequent operation is
  // rejected while `record_plan_sealed` is set. Validation changes only OCC
  // lock/version state; it cannot change the canonical write set. Rewalking
  // that write set here duplicated the complete preflight traversal on every
  // successful commit. Trust the sealed scalars on this private hot path.
  // `serialize_cache_record` still walks the canonical writes once and checks
  // both the visited operation count and exact ending cursor before STO may
  // install anything, so an internal mismatch remains a fail-closed unwritten
  // bound record rather than an uncovered commit.
  const record_shape final_shape{bridge->txn->record_plan_bytes,
                                 bridge->txn->record_plan_ops,
                                 bridge->txn->record_plan_checksum_mode};
  if (final_shape.operations == 0 ||
      !valid_record_checksum_mode(final_shape.checksum_mode) ||
      final_shape.bytes <
          kCacheRecordHeaderBytes +
              record_trailer_bytes(final_shape.checksum_mode))
    return false;

  if (bridge->use_unchecked_one_put_serializer) {
    // The fused terminal rederived this exact direct v4 shape immediately
    // before entering commit and sealed the transaction against later API
    // operations. STO validation changes lock/version state, not this view.
    assert(bridge->txn->record_fast_path_eligible);
    bridge->use_direct_write = true;
  } else if (bridge->txn->record_fast_path_eligible) {
    record_shape witnessed_shape;
    if (!derive_fast_record_shape(bridge->txn, final_shape.checksum_mode,
                                  &witnessed_shape) ||
        witnessed_shape.bytes != final_shape.bytes ||
        witnessed_shape.operations != final_shape.operations)
      return false;
    bridge->use_direct_write = true;
  }

  if (bridge->assign_sequence_natively) {
    assert(bridge->native_unhealthy != nullptr);
    assert(bridge->ordered_sequence_out != nullptr);
    assert(bridge->ordered_timestamp_out != nullptr);
    assert(packed_cache_order_allowed(bridge->txn->owner));
    // The packed general bit is the unique cache-sequence writer here.
    // Matching compiler atomics preserve the Rust queue's health ordering.
    if (__atomic_load_n(bridge->native_unhealthy, __ATOMIC_ACQUIRE) != 0)
      return false;
    uint64_t sequence = 0;
    if (!Transaction::try_allocate_locked_cache_sequence(sequence))
      return false;
    bridge->final_shape = final_shape;
    bridge->sequence = sequence;
    *bridge->ordered_timestamp_out = timestamp;
    *bridge->ordered_sequence_out = sequence;
    return true;
  }

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

// @unsafe - The direct one-Put shape was rederived before validation and then
// sealed. This restricted callback runs only after final validation, so its
// successful packed CAS is both the timestamp and dense-order commit point.
Transaction::ordered_accept_result accept_packed_cache_order(
    void *opaque, uint32_t *timestamp_out) noexcept {
  auto *bridge = static_cast<record_bind_bridge *>(opaque);
  assert(bridge != nullptr);
  assert(timestamp_out != nullptr);
  assert(!bridge->validation_gate_held);
  assert(bridge->assign_sequence_natively);
  assert(bridge->native_unhealthy != nullptr);
  assert(bridge->ordered_sequence_out != nullptr);
  assert(bridge->ordered_timestamp_out != nullptr);
  *timestamp_out = 0;

  const record_shape final_shape{bridge->txn->record_plan_bytes,
                                 bridge->txn->record_plan_ops,
                                 bridge->txn->record_plan_checksum_mode};
  const bool prepared = bridge->use_unchecked_one_put_serializer &&
      bridge->txn->record_fast_path_eligible &&
      final_shape.operations == 1 &&
      valid_record_checksum_mode(final_shape.checksum_mode) &&
      final_shape.bytes >=
          kCacheRecordHeaderBytes +
              record_trailer_bytes(final_shape.checksum_mode);
  if (!prepared ||
      !packed_cache_order_allowed(bridge->txn->owner) ||
      __atomic_load_n(bridge->native_unhealthy, __ATOMIC_ACQUIRE) != 0) {
    // Preserve scalar timestamp-before-hook precedence on cold rejection.
    uint32_t rejected_timestamp = 0;
    if (!Transaction::try_allocate_mako_timestamp(rejected_timestamp))
      return Transaction::ordered_accept_result::timestamp_exhausted;
    return Transaction::ordered_accept_result::hook_rejected;
  }

  for (;;) {
    uint64_t sequence = 0;
    uint32_t timestamp = 0;
    switch (Transaction::try_allocate_cache_order_pair(sequence,
                                                        timestamp)) {
    case Transaction::cache_order_allocation::accepted:
      bridge->final_shape = final_shape;
      bridge->use_direct_write = true;
      bridge->sequence = sequence;
      *bridge->ordered_sequence_out = sequence;
      *bridge->ordered_timestamp_out = timestamp;
      *timestamp_out = timestamp;
      return Transaction::ordered_accept_result::accepted;
    case Transaction::cache_order_allocation::general_locked:
      record_validation_cpu_relax();
      break;
    case Transaction::cache_order_allocation::timestamp_exhausted:
      return Transaction::ordered_accept_result::timestamp_exhausted;
    case Transaction::cache_order_allocation::sequence_exhausted: {
      // This cannot precede timestamp exhaustion for a valid recovered
      // history, but retain the old timestamp-before-tail rejection behavior.
      uint32_t rejected_timestamp = 0;
      if (!Transaction::try_allocate_mako_timestamp(rejected_timestamp))
        return Transaction::ordered_accept_result::timestamp_exhausted;
      return Transaction::ordered_accept_result::hook_rejected;
    }
    }
  }
}

constexpr uint64_t kRustPublicationPhaseBits = 2;
constexpr uint64_t kRustPublicationFree = 0;
constexpr uint64_t kRustPublicationBound = 1;

uint64_t rust_publication_turn(uint64_t sequence, uint32_t ring_shift,
                               uint64_t phase) noexcept {
  assert(ring_shift >= kRustPublicationPhaseBits);
  assert(phase < (UINT64_C(1) << kRustPublicationPhaseBits));
  return ((sequence >> ring_shift) << kRustPublicationPhaseBits) | phase;
}

// @unsafe - The terminal validated the immutable layout before STO could
// assign sequence. The queue's detached occupancy claim proves this exact ring
// generation has retired and cannot alias another live producer. Once the
// dense tail advances, returning without BOUND would leave an unfillable hole;
// any impossible generation therefore terminates instead of unwinding.
void bind_native_ordered_arena(record_bind_bridge *bridge) noexcept {
  assert(bridge != nullptr);
  assert(bridge->assign_sequence_natively);
  assert(bridge->hook == nullptr);
  assert(bridge->context != nullptr);
  assert(bridge->sequence != 0);
  assert(bridge->ordered_timestamp_out != nullptr);
  assert(*bridge->ordered_timestamp_out != 0);
  assert(bridge->record == nullptr);
  const auto &control =
      *static_cast<const mako_rust_fast_native_ordered_arena_control *>(
          bridge->context);

  if (bridge->final_shape.bytes == 0 ||
      bridge->final_shape.bytes > control.arena_block_bytes ||
      control.publication_shift < kRustPublicationPhaseBits ||
      control.publication_stride != sizeof(rust_publication_cell_layout) ||
      control.arena_stride != sizeof(rust_record_arena_block_layout))
    std::abort();

  const size_t index =
      static_cast<size_t>(bridge->sequence) & control.publication_mask;
  if (index > control.publication_mask)
    std::abort();
  uint8_t *const publication =
      control.publication_base + index * control.publication_stride;
  uint8_t *const record = control.arena_base + index * control.arena_stride;
  auto *const turn = reinterpret_cast<uint64_t *>(publication);
  const uint64_t free = rust_publication_turn(
      bridge->sequence, control.publication_shift, kRustPublicationFree);
  const uint64_t bound = rust_publication_turn(
      bridge->sequence, control.publication_shift, kRustPublicationBound);
  if (__atomic_load_n(turn, __ATOMIC_ACQUIRE) != free)
    std::abort();

  const size_t no_record = 0;
  std::memcpy(publication +
                  offsetof(rust_publication_cell_layout, record_bytes),
              &no_record, sizeof(no_record));
  __atomic_store_n(turn, bound, __ATOMIC_RELEASE);
  bridge->record = record;
}

// @unsafe - Completes the already-bound record after native releases its
// ordering exclusion, but while STO still owns the complete write set before
// any write is installed. This bounded walk and copy, plus optional CRC, performs
// no allocation or I/O; failure leaves Rust's ordered slot bound but unwritten,
// which pins the queue fail-closed.
bool serialize_bound_record_after_gate(void *opaque,
                                       uint32_t timestamp) noexcept {
  auto *bridge = static_cast<record_bind_bridge *>(opaque);
  assert(!bridge->validation_gate_held);
  assert(bridge->sequence != 0);
  if (bridge->assign_sequence_natively && bridge->hook == nullptr) {
    bind_native_ordered_arena(bridge);
  } else if (bridge->assign_sequence_natively) {
    uint64_t returned_sequence = bridge->sequence;
    uint8_t *record = nullptr;
    size_t capacity = 0;
    try {
      if (bridge->hook(bridge->context, timestamp,
                       bridge->final_shape.bytes, &returned_sequence, &record,
                       &capacity) == 0)
        return false;
    } catch (...) {
      return false;
    }
    if (returned_sequence != bridge->sequence || record == nullptr ||
        capacity < bridge->final_shape.bytes)
      return false;
    bridge->record = record;
  }
  assert(bridge->record != nullptr);
  const bool serialized = bridge->use_unchecked_one_put_serializer
      ? serialize_unchecked_one_put_cache_record(
            bridge->sequence, timestamp, bridge->final_shape.bytes,
            bridge->record, bridge->txn->record_fast_write)
      : serialize_cache_record(TThread::txn, bridge->sequence, timestamp,
                               bridge->final_shape, bridge->record,
                               bridge->use_direct_write
                                   ? &bridge->txn->record_fast_write
                                   : nullptr);
  if (!serialized)
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
    {
      std::lock_guard<std::mutex> claim_guard(active_cache_order_mu);
      if (active_cache_order_db.load(std::memory_order_relaxed) == db) {
        active_cache_order_db.store(nullptr, std::memory_order_release);
        db->cache_order_mode = 0;
      }
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
  // A point read may add or repack an observation in the transaction buffer.
  // Retire a preceding put's borrowed spans before entering MassTrans;
  // preflight will recover the authoritative canonical view by walking STO.
  invalidate_borrowed_fast_record_witness(txn);

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
  // A trusted transaction can deliberately call the public surface for a
  // second table. Invalidate its one-put borrowed witness before any native
  // mutation, including a same-key rewrite that normalizes in place.
  invalidate_fast_record_witness(txn);

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
  invalidate_fast_record_witness(txn);

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
  invalidate_fast_record_witness(txn);

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

// @unsafe - The build-private Rust wrapper supplies the live LocalDb which owns
// the process cache-order namespace. The Acquire RMW marks a modification-order
// cut after every preceding packed writer assignment or general-lock acquire.
MAKO_RUST_FAST_DEFINITION_HIDDEN void
mako_rust_fast_db_order_record_validation_prefix(
    mako_local_db *db) noexcept {
  if (db == nullptr || !packed_cache_order_allowed(db)) [[unlikely]]
    std::abort();
  assert(active_cache_order_db.load(std::memory_order_acquire) == db);
  (void)Transaction::order_cache_validation_prefix();
}

// @unsafe - Claims the one process-wide cache-order namespace for a live
// LocalDb. Cache construction calls this before recovery can admit foreground
// work. A second facade receives BUSY instead of interleaving gaps into two
// independent backend logs. foreground_mode is immutable for the claim and
// prevents Rust-sequence and packed-sequence terminals from mixing.
MAKO_RUST_FAST_DEFINITION_HIDDEN int
mako_rust_fast_db_claim_cache_order_namespace(
    mako_local_db *db, uint32_t foreground_mode) noexcept {
  if (db == nullptr ||
      (foreground_mode != MAKO_RUST_FAST_CACHE_ORDER_CONCURRENT &&
       foreground_mode != MAKO_RUST_FAST_CACHE_ORDER_SINGLE_PRODUCER))
    return MAKO_LOCAL_INVALID_ARGUMENT;
  std::lock_guard<std::mutex> claim_guard(active_cache_order_mu);
  if (active_cache_order_db.load(std::memory_order_acquire) != nullptr)
    return MAKO_LOCAL_BUSY;
  db->cache_order_mode = foreground_mode;
  if (!Transaction::reseed_cache_order_sequence(0)) {
    db->cache_order_mode = 0;
    return MAKO_LOCAL_BUSY;
  }
  active_cache_order_db.store(db, std::memory_order_release);
  return MAKO_LOCAL_OK;
}

// @unsafe - Recovery owns the claimed namespace exclusively and supplies the
// dense backend tail validated from records 1..N. The process timestamp field
// is deliberately preserved at its current or separately recovered floor.
MAKO_RUST_FAST_DEFINITION_HIDDEN int
mako_rust_fast_db_reseed_cache_order_namespace(
    mako_local_db *db, uint64_t recovered_sequence) noexcept {
  if (db == nullptr ||
      active_cache_order_db.load(std::memory_order_acquire) != db)
    return MAKO_LOCAL_INVALID_ARGUMENT;
  return Transaction::reseed_cache_order_sequence(recovered_sequence)
      ? MAKO_LOCAL_OK
      : MAKO_LOCAL_INVALID_ARGUMENT;
}

// @safe - Test and cold-wrapper snapshot of the packed process word.
MAKO_RUST_FAST_DEFINITION_HIDDEN uint64_t
mako_rust_fast_db_cache_order_snapshot(const mako_local_db *db) noexcept {
  if (db == nullptr ||
      active_cache_order_db.load(std::memory_order_acquire) != db)
    return 0;
  return Transaction::cache_order_snapshot();
}

MAKO_RUST_FAST_DEFINITION_HIDDEN int
mako_rust_fast_one_put_holder_pool_create(
    size_t capacity, uint32_t key_reserve_bytes,
    uint32_t value_reserve_bytes,
    mako_rust_fast_one_put_holder_pool **out) noexcept {
  if (out == nullptr) return MAKO_LOCAL_INVALID_ARGUMENT;
  *out = nullptr;
  if (!is_nonzero_power_of_two(capacity))
    return MAKO_LOCAL_INVALID_ARGUMENT;
  if (key_reserve_bytes > MAKO_LOCAL_MAX_KEY_BYTES ||
      value_reserve_bytes > MAKO_LOCAL_MAX_VALUE_BYTES)
    return MAKO_LOCAL_VALUE_TOO_LARGE;

  try {
    auto pool = std::make_unique<mako_rust_fast_one_put_holder_pool>();
    pool->capacity = capacity;
    pool->mask = capacity - 1;
    pool->holders = std::make_unique<one_put_holder[]>(capacity);
    if (key_reserve_bytes >
        mako_rust_fast_one_put_holder_pool::kInlineKeyBytes) {
      for (size_t index = 0; index != capacity; ++index)
        pool->holders[index].overflow_key.reserve(key_reserve_bytes);
    }
    if (value_reserve_bytes != 0) {
      const size_t encoded_reserve =
          static_cast<size_t>(value_reserve_bytes) +
          static_cast<size_t>(mako::EXTRA_BITS_FOR_VALUE);
      for (size_t index = 0; index != capacity; ++index)
        pool->holders[index].encoded_value.reserve(encoded_reserve);
    }
    *out = pool.release();
    return MAKO_LOCAL_OK;
  } catch (const std::bad_alloc &) {
    return MAKO_LOCAL_OUT_OF_MEMORY;
  } catch (...) {
    return MAKO_LOCAL_INTERNAL;
  }
}

MAKO_RUST_FAST_DEFINITION_HIDDEN int
mako_rust_fast_one_put_holder_pool_destroy(
    mako_rust_fast_one_put_holder_pool *pool) noexcept {
  if (pool == nullptr) return MAKO_LOCAL_OK;
  // External quiescence is required. This scan is only a cold diagnostic; it
  // does not make destruction race-safe against producer or consumer calls.
  for (size_t index = 0; index != pool->capacity; ++index) {
    if (pool->holders[index].state != one_put_holder_state::free)
      return MAKO_LOCAL_BUSY;
  }
  delete pool;
  return MAKO_LOCAL_OK;
}

MAKO_RUST_FAST_DEFINITION_HIDDEN int
mako_rust_fast_one_put_holder_pool_get_hot_layout(
    mako_rust_fast_one_put_holder_pool *pool, void **holder_base_out,
    size_t *holder_mask_out) noexcept {
  if (holder_base_out == nullptr || holder_mask_out == nullptr)
    return MAKO_LOCAL_INVALID_ARGUMENT;
  *holder_base_out = nullptr;
  *holder_mask_out = 0;
  if (pool == nullptr || !is_nonzero_power_of_two(pool->capacity) ||
      pool->holders == nullptr)
    return MAKO_LOCAL_INVALID_ARGUMENT;
  *holder_base_out = pool->holders.get();
  *holder_mask_out = pool->mask;
  return MAKO_LOCAL_OK;
}

MAKO_RUST_FAST_DEFINITION_HIDDEN int
mako_rust_fast_one_put_holder_pool_get_view(
    const mako_rust_fast_one_put_holder_pool *pool,
    uint64_t expected_sequence,
    mako_rust_fast_one_put_holder_view *out) noexcept {
  if (out == nullptr) return MAKO_LOCAL_INVALID_ARGUMENT;
  *out = mako_rust_fast_one_put_holder_view{};
  if (pool == nullptr || expected_sequence == 0 ||
      !is_nonzero_power_of_two(pool->capacity))
    return MAKO_LOCAL_INVALID_ARGUMENT;

  const one_put_holder &holder =
      one_put_holder_for(pool, expected_sequence);
  if (holder.state != one_put_holder_state::sealed ||
      holder.sequence != expected_sequence)
    return MAKO_LOCAL_BUSY;
  if (holder.mako_timestamp == 0 ||
      holder.encoded_value.size() <
          static_cast<size_t>(mako::EXTRA_BITS_FOR_VALUE))
    return MAKO_LOCAL_INTERNAL;
  const size_t value_len = holder.encoded_value.size() -
                           static_cast<size_t>(mako::EXTRA_BITS_FOR_VALUE);
  if (value_len > UINT32_MAX) return MAKO_LOCAL_INTERNAL;

  const uint8_t *key = nullptr;
  switch (holder.key_location) {
  case mako_rust_fast_one_put_holder_pool::key_storage::hot_inline:
    if (holder.key_len > holder.hot_inline_key.size())
      return MAKO_LOCAL_INTERNAL;
    key = holder.hot_inline_key.data();
    break;
  case mako_rust_fast_one_put_holder_pool::key_storage::inline_key:
    if (holder.key_len > holder.inline_key.size())
      return MAKO_LOCAL_INTERNAL;
    key = holder.inline_key.data();
    break;
  case mako_rust_fast_one_put_holder_pool::key_storage::overflow:
    if (holder.overflow_key.size() != holder.key_len)
      return MAKO_LOCAL_INTERNAL;
    key = reinterpret_cast<const uint8_t *>(holder.overflow_key.data());
    break;
  default:
    return MAKO_LOCAL_INTERNAL;
  }

  *out = mako_rust_fast_one_put_holder_view{
      holder.sequence,
      holder.table_id,
      key,
      reinterpret_cast<const uint8_t *>(holder.encoded_value.data()),
      holder.key_len,
      static_cast<uint32_t>(value_len),
      holder.mako_timestamp,
      0};
  return MAKO_LOCAL_OK;
}

MAKO_RUST_FAST_DEFINITION_HIDDEN int
mako_rust_fast_one_put_holder_pool_release(
    mako_rust_fast_one_put_holder_pool *pool,
    uint64_t expected_sequence) noexcept {
  if (pool == nullptr || expected_sequence == 0 ||
      !is_nonzero_power_of_two(pool->capacity))
    return MAKO_LOCAL_INVALID_ARGUMENT;
  one_put_holder &holder = one_put_holder_for(pool, expected_sequence);
  if (holder.state != one_put_holder_state::sealed ||
      holder.sequence != expected_sequence)
    return MAKO_LOCAL_BUSY;

  // Synchronous release ends the consumer's pointer borrow. Rust must perform
  // its Release applied-tail publication only after this call returns; the
  // producer's Acquire observation then orders these plain stores before
  // reuse. State becomes FREE last for cold diagnostics.
  holder.mako_timestamp = 0;
  holder.sequence = 0;
  holder.state = one_put_holder_state::free;
  return MAKO_LOCAL_OK;
}

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

  // Only a single private put can retain a direct canonical witness. Once a
  // mutation already exists, invalidate before touching STO so every same-key
  // composition and multi-key transaction takes the canonical fallback.
  const bool capture_fast_write =
      txn->record_fast_path_eligible &&
      static_cast<uint8_t>(txn->record_fast_write.op) == 0 &&
      txn->record_fast_mutation_bytes == 0;
  if (!capture_fast_write)
    invalidate_fast_record_witness(txn);

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
    Transaction::canonical_write_view write{};
    const bool existed = capture_fast_write
        ? table->transPutWithCanonicalWrite(
              as_key(key, key_len), StringWrapper(encoded), &write)
        : table->transPut(as_key(key, key_len), StringWrapper(encoded));
    uint32_t unchecked_record_bytes = 0;
    if (capture_fast_write) {
      const size_t mutation_bytes = kCacheRecordOperationHeaderBytes +
                                    static_cast<size_t>(key_len) +
                                    static_cast<size_t>(value_len);
      if (write.op == Transaction::canonical_write_view::operation::put &&
          write.key_length == key_len && write.value_length == value_len &&
          (write.key_length == 0 || write.key != nullptr) &&
          (write.value_length == 0 || write.value != nullptr)) {
        txn->record_fast_write = write;
        txn->record_fast_mutation_bytes = mutation_bytes;
        const size_t exact_record_bytes =
            kCacheRecordHeaderBytes + mutation_bytes;
        if (exact_record_bytes <= kFastPutRecordBytesMax) {
          unchecked_record_bytes =
              static_cast<uint32_t>(exact_record_bytes);
          txn->record_fused_candidate_bytes = unchecked_record_bytes;
        } else {
          // Current public key/value limits make this unreachable, but never
          // truncate a future larger representation into the packed witness.
          invalidate_fast_record_witness(txn);
        }
      } else {
        // A malformed or unexpected engine layout must never make the unsafe
        // shortcut authoritative. Canonical preflight will diagnose it.
        invalidate_fast_record_witness(txn);
      }
    }
#if !READ_MY_WRITES
    mutations.insert(mutation_key);
#endif
    return pack_fast_put_result(MAKO_LOCAL_OK, !existed,
                                unchecked_record_bytes);
  } catch (const Transaction::Abort &) {
    return pack_fast_put_result(operation_exception(txn, MAKO_LOCAL_CONFLICT));
  } catch (const std::bad_alloc &) {
    return pack_fast_put_result(
        operation_exception(txn, MAKO_LOCAL_OUT_OF_MEMORY));
  } catch (...) {
    return pack_fast_put_result(operation_exception(txn, MAKO_LOCAL_INTERNAL));
  }
}

namespace {
int record_preflight_with_checksum(
    mako_local_txn *txn, size_t max_record_bytes, uint32_t checksum_mode,
    size_t *exact_record_bytes_out, uint32_t *op_count_out) noexcept {
  if (exact_record_bytes_out == nullptr || op_count_out == nullptr)
    return MAKO_LOCAL_INVALID_ARGUMENT;
  *exact_record_bytes_out = 0;
  *op_count_out = 0;

  if (!valid_record_checksum_mode(checksum_mode))
    return MAKO_LOCAL_INVALID_ARGUMENT;
  const int checked = check_txn(txn);
  if (checked != MAKO_LOCAL_OK) return checked;
  if (txn->fast_table_impl == nullptr) return MAKO_LOCAL_INVALID_ARGUMENT;
  if (txn->record_plan_sealed) return MAKO_LOCAL_BUSY;

  record_shape shape;
  if (!derive_fast_record_shape(txn, checksum_mode, &shape)) {
    // Any uncertainty about the borrowed one-put witness falls back to the
    // complete canonical transaction walk before the plan becomes sealed.
    // Keep it invalid for serialization too, so the two phases cannot choose
    // different sources of truth.
    invalidate_fast_record_witness(txn);
    if (!derive_record_shape(TThread::txn, checksum_mode, &shape))
      return MAKO_LOCAL_INTERNAL;
  }
  if (shape.operations > MAKO_LOCAL_TXN_ITEM_BUDGET)
    return MAKO_LOCAL_INTERNAL;

  // Seal even on a cap rejection. Rust consumes that transaction by aborting;
  // allowing another operation or a differently capped second preflight would
  // make failure handling depend on a mutable write set.
  txn->record_plan_bytes = shape.bytes;
  txn->record_plan_ops = shape.operations;
  txn->record_plan_checksum_mode = checksum_mode;
  txn->record_plan_sealed = true;
  txn->record_plan_ready =
      shape.operations == 0 || shape.bytes <= max_record_bytes;
  // A sealed plan is no longer eligible for the no-preflight fused terminal,
  // while the fuller witness remains intact for the selected serializer.
  txn->record_fused_candidate_bytes = 0;
  *exact_record_bytes_out = shape.bytes;
  *op_count_out = shape.operations;
  return txn->record_plan_ready ? MAKO_LOCAL_OK
                                : MAKO_LOCAL_VALUE_TOO_LARGE;
}
}  // namespace

MAKO_RUST_FAST_DEFINITION_HIDDEN int
mako_rust_fast_txn_record_preflight(
    mako_local_txn *txn, size_t max_record_bytes,
    size_t *exact_record_bytes_out, uint32_t *op_count_out) noexcept {
  return record_preflight_with_checksum(
      txn, max_record_bytes, MAKO_RUST_FAST_RECORD_CHECKSUM_CRC32C,
      exact_record_bytes_out, op_count_out);
}

MAKO_RUST_FAST_DEFINITION_HIDDEN int
mako_rust_fast_txn_record_preflight_with_checksum(
    mako_local_txn *txn, size_t max_record_bytes, uint32_t checksum_mode,
    size_t *exact_record_bytes_out, uint32_t *op_count_out) noexcept {
  return record_preflight_with_checksum(txn, max_record_bytes, checksum_mode,
                                        exact_record_bytes_out, op_count_out);
}

namespace {
[[gnu::cold]] static uint64_t abort_fast_record_terminal(
    mako_local_txn *txn, int status) noexcept {
  if (!abort_and_finish(txn, cleanup_boundary::abort))
    return pack_fast_terminal_result(MAKO_LOCAL_WORKER_POISONED,
                                     MAKO_LOCAL_WORKER_POISONED);
  recycle_txn(txn);
  return pack_fast_terminal_result(status, MAKO_LOCAL_OK);
}

[[gnu::cold]] static uint64_t reject_fast_record_terminal(
    mako_local_txn *txn) noexcept {
  return abort_fast_record_terminal(txn, MAKO_LOCAL_INVALID_ARGUMENT);
}

constexpr mako_rust_fast_preselected_record_result
pack_preselected_record_result(
    uint64_t terminal,
    const preselected_one_put_bridge &bridge) noexcept {
  return mako_rust_fast_preselected_record_result{
      terminal,
      pack_preselected_record_state(bridge.mako_timestamp,
                                    bridge.record_written)};
}

[[gnu::cold]] static mako_rust_fast_preselected_record_result
reject_fast_preselected_record_terminal(mako_local_txn *txn) noexcept {
  return mako_rust_fast_preselected_record_result{
      reject_fast_record_terminal(txn), 0};
}

[[gnu::always_inline]] static inline uint64_t commit_ready_record_and_destroy(
    mako_local_txn *txn, mako_rust_fast_record_bind_hook bind_hook,
    void *context, uint8_t *record_written_out,
    bool unchecked_one_put, bool acquire_gate_after_validation = false,
    Transaction::commit_validation_gate::callback enter_validation_gate =
        enter_record_validation_gate,
    Transaction::commit_validation_gate::callback leave_validation_gate =
        leave_record_validation_gate,
    bool assign_sequence_natively = false,
    const uint8_t *native_unhealthy = nullptr,
    uint64_t *ordered_sequence_out = nullptr,
    uint32_t *ordered_timestamp_out = nullptr) noexcept {
  int status = MAKO_LOCAL_INTERNAL;
  try {
#if defined(MAKO_LOCAL_TEST_HOOKS)
    arm_native_cleanup_failure_if_requested(cleanup_boundary::commit);
#endif
    Transaction::preinstall_failure failure =
        Transaction::preinstall_failure::none;
    record_bind_bridge bridge{txn, bind_hook, context, record_written_out};
    bridge.use_unchecked_one_put_serializer = unchecked_one_put;
    bridge.native_unhealthy = native_unhealthy;
    bridge.ordered_sequence_out = ordered_sequence_out;
    bridge.ordered_timestamp_out = ordered_timestamp_out;
    bridge.assign_sequence_natively = assign_sequence_natively;
    const bool use_restricted_packed_accept =
        bridge.assign_sequence_natively && acquire_gate_after_validation;
    const Transaction::commit_validation_gate validation_gate{
        use_restricted_packed_accept ? nullptr : enter_validation_gate,
        use_restricted_packed_accept ? nullptr : leave_validation_gate,
        serialize_bound_record_after_gate, &bridge,
        acquire_gate_after_validation,
        use_restricted_packed_accept ? accept_packed_cache_order : nullptr};
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

// @safe: Pure scalar packing from a validated bridge and terminal result; no
// pointer is dereferenced and no ownership or lifetime is changed.
[[gnu::always_inline]] static inline
mako_rust_fast_native_ordered_arena_result pack_native_ordered_arena_result(
    uint64_t terminal, const record_bind_bridge &bridge,
    uint32_t accepted_timestamp, uint8_t record_written) noexcept {
  return mako_rust_fast_native_ordered_arena_result{
      terminal, bridge.sequence,
      pack_preselected_record_state(accepted_timestamp,
                                    record_written == 1)};
}

[[gnu::always_inline]] static inline
mako_rust_fast_native_ordered_arena_result
commit_native_ordered_arena_and_destroy(
    mako_local_txn *txn,
    const mako_rust_fast_native_ordered_arena_control &validated_control)
    noexcept {
  // Copy the caller-owned descriptor before commit. The post-ordering path
  // reads an immutable stack snapshot, so a nonzero result cannot depend on a
  // control block whose fields changed around sequence assignment.
  mako_rust_fast_native_ordered_arena_control control = validated_control;
  uint64_t ordered_sequence = 0;
  uint32_t accepted_timestamp = 0;
  uint8_t record_written = 0;
  record_bind_bridge bridge{txn, nullptr, &control, &record_written};
  bridge.use_unchecked_one_put_serializer = true;
  bridge.native_unhealthy = control.unhealthy;
  bridge.ordered_sequence_out = &ordered_sequence;
  bridge.ordered_timestamp_out = &accepted_timestamp;
  bridge.assign_sequence_natively = true;
  const bool can_accept_after_validation =
      TThread::txn->can_order_record_after_validation();

  int status = MAKO_LOCAL_INTERNAL;
  try {
#if defined(MAKO_LOCAL_TEST_HOOKS)
    arm_native_cleanup_failure_if_requested(cleanup_boundary::commit);
#endif
    Transaction::preinstall_failure failure =
        Transaction::preinstall_failure::none;
    const Transaction::commit_validation_gate validation_gate{
        can_accept_after_validation ? nullptr : enter_packed_cache_order_gate,
        can_accept_after_validation ? nullptr : leave_packed_cache_order_gate,
        serialize_bound_record_after_gate, &bridge,
        can_accept_after_validation,
        can_accept_after_validation ? accept_packed_cache_order : nullptr};
    const bool committed = Sto::try_commit_no_paxos(
        invoke_record_bind_hook, &bridge, &failure, &validation_gate);
    if (bridge.sequence != ordered_sequence ||
        (bridge.sequence != 0 && bridge.record == nullptr))
      std::abort();
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
        return pack_native_ordered_arena_result(
            pack_fast_terminal_result(MAKO_LOCAL_WORKER_POISONED,
                                      MAKO_LOCAL_WORKER_POISONED),
            bridge, accepted_timestamp, record_written);
      }
    }
  } catch (...) {
    // Assignment occurs in a noexcept hook and direct BOUND occurs in the
    // immediately following noexcept after-leave callback. Never expose a
    // sequence if an internal change breaks that adjacency.
    if (bridge.sequence != ordered_sequence ||
        (bridge.sequence != 0 && bridge.record == nullptr))
      std::abort();
    poison_transaction(txn);
    return pack_native_ordered_arena_result(
        pack_fast_terminal_result(MAKO_LOCAL_WORKER_POISONED,
                                  MAKO_LOCAL_WORKER_POISONED),
        bridge, accepted_timestamp, record_written);
  }

  recycle_txn(txn);
  return pack_native_ordered_arena_result(
      pack_fast_terminal_result(status, MAKO_LOCAL_OK), bridge,
      accepted_timestamp, record_written);
}

[[gnu::always_inline]] static inline
mako_rust_fast_preselected_record_result
commit_preselected_one_put_record_and_destroy(
    mako_local_txn *txn, uint64_t sequence, uint8_t *record,
    size_t exact_record_bytes) noexcept {
  preselected_one_put_bridge bridge{
      txn, sequence, record, exact_record_bytes};
  int status = MAKO_LOCAL_INTERNAL;
  try {
#if defined(MAKO_LOCAL_TEST_HOOKS)
    arm_native_cleanup_failure_if_requested(cleanup_boundary::commit);
#endif
    Transaction::preinstall_failure failure =
        Transaction::preinstall_failure::none;
    const Transaction::commit_validation_gate validation_gate{
        nullptr, nullptr, nullptr, &bridge};
    const bool committed = Sto::try_commit_no_paxos(
        serialize_preselected_one_put_record, &bridge, &failure,
        &validation_gate);
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
        return pack_preselected_record_result(
            pack_fast_terminal_result(MAKO_LOCAL_WORKER_POISONED,
                                      MAKO_LOCAL_WORKER_POISONED),
            bridge);
      }
    }
  } catch (...) {
    poison_transaction(txn);
    return pack_preselected_record_result(
        pack_fast_terminal_result(MAKO_LOCAL_WORKER_POISONED,
                                  MAKO_LOCAL_WORKER_POISONED),
        bridge);
  }

  recycle_txn(txn);
  return pack_preselected_record_result(
      pack_fast_terminal_result(status, MAKO_LOCAL_OK), bridge);
}

[[gnu::always_inline]] static inline mako_rust_fast_preselected_record_result
pack_preselected_holder_result(
    uint64_t terminal, const one_put_holder &holder) noexcept {
  const bool sealed = holder.state == one_put_holder_state::sealed;
  return mako_rust_fast_preselected_record_result{
      terminal,
      pack_preselected_record_state(sealed ? holder.mako_timestamp : 0,
                                    sealed)};
}

[[gnu::always_inline]] static inline uint64_t
commit_preselected_one_put_holder_and_destroy(
    mako_local_txn *txn, one_put_holder *holder, uint64_t sequence,
    uint64_t table_id, uint16_t key_len,
    mako_rust_fast_one_put_holder_pool::key_storage key_location) noexcept {
  preselected_one_put_holder_bridge bridge{
      txn, holder, sequence, table_id, key_len, key_location};
  int status = MAKO_LOCAL_INTERNAL;
  try {
#if defined(MAKO_LOCAL_TEST_HOOKS)
    arm_native_cleanup_failure_if_requested(cleanup_boundary::commit);
#endif
    Transaction::preinstall_failure failure =
        Transaction::preinstall_failure::none;
    const Transaction::commit_validation_gate validation_gate{
        nullptr, nullptr, nullptr, &bridge};
    const bool committed = Sto::try_commit_no_paxos(
        accept_preselected_one_put_holder, &bridge, &failure,
        &validation_gate);

    // The accepted hook only captures the assigned timestamp. Moving the
    // staged encoded string sooner would change STO's stable StringWrapper
    // target before install/cleanup. Transfer it only after try_commit returns.
    if (bridge.mako_timestamp != 0)
      seal_preselected_one_put_holder(&bridge);

    // The hook always accepts, and sealing above is noexcept after validation
    // has returned. These relationships are therefore construction
    // invariants, not fallible runtime input on this same-build unsafe path.
    // Keep their full diagnostic spelling without charging every ACK.
#ifndef NDEBUG
    assert(committed ==
           (bridge.mako_timestamp != 0 && bridge.holder_sealed));
    assert(bridge.mako_timestamp == 0 || bridge.holder_sealed);
#endif

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
    // Stack unwinding has left try_commit_no_paxos before this transfer. An
    // accepted timestamp owns the dense sequence even though cleanup status is
    // now unknown, so seal it before quarantining. A preaccept exception owns
    // no sequence and returns its invisible holder immediately.
    if (bridge.mako_timestamp != 0 && !bridge.holder_sealed)
      seal_preselected_one_put_holder(&bridge);
    poison_transaction(txn);
    return pack_fast_terminal_result(MAKO_LOCAL_WORKER_POISONED,
                                     MAKO_LOCAL_WORKER_POISONED);
  }

  recycle_txn(txn);
  return pack_fast_terminal_result(status, MAKO_LOCAL_OK);
}
}  // namespace

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
  if (!rust_sequence_cache_order_allowed(txn->owner) ||
      bind_hook == nullptr || record_written_out == nullptr ||
      !txn->record_plan_sealed || !txn->record_plan_ready ||
      txn->record_plan_ops == 0 ||
      txn->record_plan_bytes <
          kCacheRecordHeaderBytes +
              record_trailer_bytes(txn->record_plan_checksum_mode)) {
    return reject_fast_record_terminal(txn);
  }

  return commit_ready_record_and_destroy(txn, bind_hook, context,
                                         record_written_out, false);
}

MAKO_RUST_FAST_DEFINITION_HIDDEN uint64_t
mako_rust_fast_txn_commit_native_ordered_record_and_destroy(
    mako_local_txn *txn, const uint8_t *unhealthy,
    mako_rust_fast_record_bind_hook bind_hook, void *context,
    uint64_t *ordered_sequence_out, uint32_t *ordered_timestamp_out,
    uint8_t *record_written_out) noexcept {
  assert(txn != nullptr);
  assert(on_owner_thread(txn));
  assert(!txn->poisoned);
  assert(!local_worker_poisoned);
  assert(txn->active);
  assert(local_active_txn == txn);
  assert(txn->fast_table_impl != nullptr);

  if (ordered_sequence_out != nullptr) *ordered_sequence_out = 0;
  if (ordered_timestamp_out != nullptr) *ordered_timestamp_out = 0;
  if (record_written_out != nullptr) *record_written_out = 0;
  if (!packed_cache_order_allowed(txn->owner) ||
      unhealthy == nullptr || bind_hook == nullptr || context == nullptr ||
      ordered_sequence_out == nullptr || ordered_timestamp_out == nullptr ||
      record_written_out == nullptr || !txn->record_plan_sealed ||
      !txn->record_plan_ready || txn->record_plan_ops == 0 ||
      txn->record_plan_bytes <
          kCacheRecordHeaderBytes +
              record_trailer_bytes(txn->record_plan_checksum_mode)) {
    return reject_fast_record_terminal(txn);
  }

  // General transactions own the packed certification bit from before Mako
  // timestamp allocation through final read/predicate validation and dense
  // assignment. The callback adopts that exact sequence only after the bit is
  // released, while STO still owns every write lock.
  return commit_ready_record_and_destroy(
      txn, bind_hook, context, record_written_out, false, false,
      enter_packed_cache_order_gate, leave_packed_cache_order_gate, true,
      unhealthy, ordered_sequence_out, ordered_timestamp_out);
}

MAKO_RUST_FAST_DEFINITION_HIDDEN uint64_t
mako_rust_fast_txn_commit_unchecked_one_put_record_and_destroy(
    mako_local_txn *txn, uint32_t expected_record_bytes,
    mako_rust_fast_record_bind_hook bind_hook, void *context,
    uint8_t *record_written_out) noexcept {
  assert(txn != nullptr);
  assert(on_owner_thread(txn));
  assert(!txn->poisoned);
  assert(!local_worker_poisoned);
  assert(txn->active);
  assert(local_active_txn == txn);
  assert(txn->fast_table_impl != nullptr);

  if (record_written_out != nullptr) *record_written_out = 0;
  record_shape shape;
  if (!rust_sequence_cache_order_allowed(txn->owner) ||
      bind_hook == nullptr || record_written_out == nullptr ||
      expected_record_bytes == 0 ||
      expected_record_bytes > kFastPutRecordBytesMax ||
      txn->record_plan_sealed || txn->record_plan_ready ||
      txn->record_plan_bytes != 0 || txn->record_plan_ops != 0 ||
      !derive_fast_record_shape(
          txn, MAKO_RUST_FAST_RECORD_CHECKSUM_NONE, &shape) ||
      shape.operations != 1 || shape.bytes != expected_record_bytes) {
    return reject_fast_record_terminal(txn);
  }

  // This seal occurs only after exact revalidation and before STO can acquire
  // a write lock. The caller has already reserved the advertised extent;
  // bind_hook merely lends that stable storage after final validation.
  txn->record_plan_bytes = shape.bytes;
  txn->record_plan_ops = shape.operations;
  txn->record_plan_checksum_mode = MAKO_RUST_FAST_RECORD_CHECKSUM_NONE;
  txn->record_plan_sealed = true;
  txn->record_plan_ready = true;
  return commit_ready_record_and_destroy(txn, bind_hook, context,
                                         record_written_out, true,
                                         TThread::txn
                                             ->can_order_record_after_validation());
}

MAKO_RUST_FAST_DEFINITION_HIDDEN uint64_t
mako_rust_fast_txn_commit_native_ordered_unchecked_one_put_record_and_destroy(
    mako_local_txn *txn, uint32_t expected_record_bytes,
    uint64_t *next_bound, const uint8_t *unhealthy,
    mako_rust_fast_record_bind_hook bind_hook, void *context,
    uint64_t *ordered_sequence_out, uint32_t *ordered_timestamp_out,
    uint8_t *record_written_out) noexcept {
  assert(txn != nullptr);
  assert(on_owner_thread(txn));
  assert(!txn->poisoned);
  assert(!local_worker_poisoned);
  assert(txn->active);
  assert(local_active_txn == txn);
  assert(txn->fast_table_impl != nullptr);

  if (ordered_sequence_out != nullptr) *ordered_sequence_out = 0;
  if (ordered_timestamp_out != nullptr) *ordered_timestamp_out = 0;
  if (record_written_out != nullptr) *record_written_out = 0;
  record_shape shape;
  if (!packed_cache_order_allowed(txn->owner) ||
      next_bound == nullptr || unhealthy == nullptr || bind_hook == nullptr ||
      context == nullptr || ordered_sequence_out == nullptr ||
      ordered_timestamp_out == nullptr || record_written_out == nullptr ||
      reinterpret_cast<uintptr_t>(next_bound) % alignof(uint64_t) != 0 ||
      expected_record_bytes == 0 ||
      expected_record_bytes > kFastPutRecordBytesMax ||
      txn->record_plan_sealed || txn->record_plan_ready ||
      txn->record_plan_bytes != 0 || txn->record_plan_ops != 0 ||
      !derive_fast_record_shape(
          txn, MAKO_RUST_FAST_RECORD_CHECKSUM_NONE, &shape) ||
      shape.operations != 1 || shape.bytes != expected_record_bytes) {
    return reject_fast_record_terminal(txn);
  }

  txn->record_plan_bytes = shape.bytes;
  txn->record_plan_ops = shape.operations;
  txn->record_plan_checksum_mode = MAKO_RUST_FAST_RECORD_CHECKSUM_NONE;
  txn->record_plan_sealed = true;
  txn->record_plan_ready = true;
  return commit_ready_record_and_destroy(
      txn, bind_hook, context, record_written_out, true,
      TThread::txn->can_order_record_after_validation(),
      enter_packed_cache_order_gate, leave_packed_cache_order_gate, true,
      unhealthy, ordered_sequence_out, ordered_timestamp_out);
}

MAKO_RUST_FAST_DEFINITION_HIDDEN mako_rust_fast_native_ordered_arena_result
mako_rust_fast_txn_commit_native_ordered_unchecked_one_put_arena_and_destroy(
    mako_local_txn *txn, uint32_t expected_record_bytes,
    const mako_rust_fast_native_ordered_arena_control *control) noexcept {
  assert(txn != nullptr);
  assert(on_owner_thread(txn));
  assert(!txn->poisoned);
  assert(!local_worker_poisoned);
  assert(txn->active);
  assert(local_active_txn == txn);
  assert(txn->fast_table_impl != nullptr);

  record_shape shape;
  if (!packed_cache_order_allowed(txn->owner) ||
      expected_record_bytes == 0 ||
      expected_record_bytes > kFastPutRecordBytesMax ||
      !valid_native_ordered_arena_control(control, expected_record_bytes) ||
      txn->record_plan_sealed || txn->record_plan_ready ||
      txn->record_plan_bytes != 0 || txn->record_plan_ops != 0 ||
      !derive_fast_record_shape(
          txn, MAKO_RUST_FAST_RECORD_CHECKSUM_NONE, &shape) ||
      shape.operations != 1 || shape.bytes != expected_record_bytes) {
    return mako_rust_fast_native_ordered_arena_result{
        reject_fast_record_terminal(txn), 0, 0};
  }

  // Every fallible shape and layout check precedes packed pair assignment.
  // From the BOUND Release onward, native either returns that generation or
  // terminates on an impossible cell state.
  txn->record_plan_bytes = shape.bytes;
  txn->record_plan_ops = shape.operations;
  txn->record_plan_checksum_mode = MAKO_RUST_FAST_RECORD_CHECKSUM_NONE;
  txn->record_plan_sealed = true;
  txn->record_plan_ready = true;
  return commit_native_ordered_arena_and_destroy(txn, *control);
}

MAKO_RUST_FAST_DEFINITION_HIDDEN uint64_t
mako_rust_fast_txn_commit_unchecked_one_put_record_single_producer_and_destroy(
    mako_local_txn *txn, uint32_t expected_record_bytes,
    mako_rust_fast_record_bind_hook bind_hook, void *context,
    uint8_t *record_written_out) noexcept {
  // This entry intentionally cannot verify its global exclusion contract.
  // Its caller guarantees that no concurrent or already-ticketed cache-record
  // terminal for txn->owner overlaps this entire call. The ordinary fused
  // spelling above remains the safe default for concurrent producers.
  assert(txn != nullptr);
  assert(on_owner_thread(txn));
  assert(!txn->poisoned);
  assert(!local_worker_poisoned);
  assert(txn->active);
  assert(local_active_txn == txn);
  assert(txn->fast_table_impl != nullptr);

  if (record_written_out != nullptr) *record_written_out = 0;
  record_shape shape;
  if (!rust_sequence_cache_order_allowed(txn->owner) ||
      bind_hook == nullptr || record_written_out == nullptr ||
      expected_record_bytes == 0 ||
      expected_record_bytes > kFastPutRecordBytesMax ||
      txn->record_plan_sealed || txn->record_plan_ready ||
      txn->record_plan_bytes != 0 || txn->record_plan_ops != 0 ||
      !derive_fast_record_shape(
          txn, MAKO_RUST_FAST_RECORD_CHECKSUM_NONE, &shape) ||
      shape.operations != 1 || shape.bytes != expected_record_bytes) {
    return reject_fast_record_terminal(txn);
  }

  // Preserve the ordinary fused terminal's exact predicate recheck and
  // fail-closed plan seal. Only the validation gate's ticket operations are
  // replaced; Transaction still calls enter, bind, leave, and after_leave in
  // the same order while retaining the complete write set.
  txn->record_plan_bytes = shape.bytes;
  txn->record_plan_ops = shape.operations;
  txn->record_plan_checksum_mode = MAKO_RUST_FAST_RECORD_CHECKSUM_NONE;
  txn->record_plan_sealed = true;
  txn->record_plan_ready = true;
  return commit_ready_record_and_destroy(
      txn, bind_hook, context, record_written_out, true, false,
      enter_single_producer_record_validation_gate,
      leave_single_producer_record_validation_gate);
}

MAKO_RUST_FAST_DEFINITION_HIDDEN mako_rust_fast_preselected_record_result
mako_rust_fast_txn_commit_preselected_unchecked_one_put_record_single_producer_and_destroy(
    mako_local_txn *txn, uint32_t expected_record_bytes, uint64_t sequence,
    uint8_t *record, size_t record_capacity) noexcept {
  // Rust retains this exact sequence/arena generation invisibly for the whole
  // call and guarantees that no other record terminal for txn->owner runs or
  // waits. This entry cannot verify that global exclusion contract.
  assert(txn != nullptr);
  assert(on_owner_thread(txn));
  assert(!txn->poisoned);
  assert(!local_worker_poisoned);
  assert(txn->active);
  assert(local_active_txn == txn);
  assert(txn->fast_table_impl != nullptr);
#ifndef NDEBUG
  assert(txn->owner->record_validation_next.value.load(
             std::memory_order_relaxed) ==
         txn->owner->record_validation_serving.value.load(
             std::memory_order_acquire));
#endif

  record_shape shape;
  if (!rust_sequence_cache_order_allowed(txn->owner) || sequence == 0 ||
      record == nullptr || expected_record_bytes == 0 ||
      expected_record_bytes > kFastPutRecordBytesMax ||
      record_capacity < expected_record_bytes || txn->record_plan_sealed ||
      txn->record_plan_ready || txn->record_plan_bytes != 0 ||
      txn->record_plan_ops != 0 ||
      !derive_fast_record_shape(txn, MAKO_RUST_FAST_RECORD_CHECKSUM_NONE,
                                &shape) ||
      shape.operations != 1 || shape.bytes != expected_record_bytes) {
    return reject_fast_preselected_record_terminal(txn);
  }

  // The private caller already acquired this target's exact FREE arena turn.
  // Sealing after every scalar check and before lock acquisition prevents any
  // later operation from changing the spans serialized by the internal hook.
  txn->record_plan_bytes = shape.bytes;
  txn->record_plan_ops = shape.operations;
  txn->record_plan_checksum_mode = MAKO_RUST_FAST_RECORD_CHECKSUM_NONE;
  txn->record_plan_sealed = true;
  txn->record_plan_ready = true;
  return commit_preselected_one_put_record_and_destroy(txn, sequence, record,
                                                       shape.bytes);
}

[[gnu::always_inline]] static inline uint64_t
commit_preselected_unchecked_one_put_holder_single_producer_and_destroy(
    mako_local_txn *txn, uint32_t expected_record_bytes,
    one_put_holder *selected_holder, uint64_t sequence) noexcept {
  // This entry has the record terminal's same whole-database exclusivity
  // requirement plus the holder generation proof documented in the header.
  // Neither global invariant can be reconstructed from opaque native state.
  assert(txn != nullptr);
  assert(on_owner_thread(txn));
  assert(!txn->poisoned);
  assert(!local_worker_poisoned);
  assert(txn->active);
  assert(local_active_txn == txn);
  assert(txn->fast_table_impl != nullptr);
  if (!rust_sequence_cache_order_allowed(txn->owner))
    return abort_fast_record_terminal(txn, MAKO_LOCAL_INVALID_ARGUMENT);
  assert(txn->owner->record_validation_next.value.load(
             std::memory_order_relaxed) ==
         txn->owner->record_validation_serving.value.load(
             std::memory_order_acquire));
  assert(selected_holder != nullptr);
  assert(sequence != 0);
  one_put_holder &holder = *selected_holder;
#if defined(__GNUC__) || defined(__clang__)
  // The ring can exceed LLC. A future pointer-token ABI can issue this before
  // the terminal; until then start exclusive acquisition immediately after
  // masked selection and before the diagnostic shape assertions.
  __builtin_prefetch(&holder, 1, 3);
#endif

  const auto &write = txn->record_fast_write;
#ifndef NDEBUG
  record_shape shape;
  const size_t encoded_trailer =
      static_cast<size_t>(mako::EXTRA_BITS_FOR_VALUE);
  assert(expected_record_bytes != 0);
  assert(expected_record_bytes <= kFastPutRecordBytesMax);
  assert(!txn->record_plan_sealed);
  assert(!txn->record_plan_ready);
  assert(txn->record_plan_bytes == 0);
  assert(txn->record_plan_ops == 0);
  assert(txn->encoded_values_used == 1);
  assert(derive_fast_record_shape(txn, MAKO_RUST_FAST_RECORD_CHECKSUM_NONE,
                                  &shape));
  assert(shape.operations == 1);
  assert(shape.bytes == expected_record_bytes);
  assert(write.op == Transaction::canonical_write_view::operation::put);
  assert(write.key_length <= UINT16_MAX);
  assert(write.value_length <= UINT32_MAX);
  assert(txn->encoded_values[0].size() == write.value_length + encoded_trailer);
  assert(holder.state == one_put_holder_state::free);
  assert(holder.sequence == 0);
  assert(holder.mako_timestamp == 0);
#else
  // The private Rust wrapper obtained this exact value from the immediately
  // preceding fast Put and consumes the transaction without another operation.
  // Malformed calls violate the same-build unsafe ABI contract; deliberately
  // keep their diagnostics out of the production foreground path.
  (void)expected_record_bytes;
#endif

  const auto key_location =
      write.key_length <= mako_rust_fast_one_put_holder_pool::kHotInlineKeyBytes
          ? mako_rust_fast_one_put_holder_pool::key_storage::hot_inline
      : write.key_length <= mako_rust_fast_one_put_holder_pool::kInlineKeyBytes
          ? mako_rust_fast_one_put_holder_pool::key_storage::inline_key
          : mako_rust_fast_one_put_holder_pool::key_storage::overflow;
  if (key_location ==
      mako_rust_fast_one_put_holder_pool::key_storage::overflow) {
    try {
      holder.overflow_key.assign(write.key, write.key_length);
    } catch (const std::bad_alloc &) {
      return abort_fast_record_terminal(txn, MAKO_LOCAL_OUT_OF_MEMORY);
    } catch (...) {
      return abort_fast_record_terminal(txn, MAKO_LOCAL_INTERNAL);
    }
  } else if (key_location ==
             mako_rust_fast_one_put_holder_pool::key_storage::hot_inline) {
    copy_preselected_holder_inline_key(holder.hot_inline_key.data(), write.key,
                                       write.key_length);
  } else {
    copy_preselected_holder_inline_key(holder.inline_key.data(), write.key,
                                       write.key_length);
  }

  // The unsafe consuming contract is itself the seal: no transaction operation
  // can follow this call. Inline bytes and long-key allocation are complete,
  // so the post-validation hook need only capture the Mako timestamp.
  return commit_preselected_one_put_holder_and_destroy(
      txn, &holder, sequence, write.table_id,
      static_cast<uint16_t>(write.key_length), key_location);
}

MAKO_RUST_FAST_DEFINITION_HIDDEN mako_rust_fast_preselected_record_result
mako_rust_fast_txn_commit_preselected_unchecked_one_put_holder_single_producer_and_destroy(
    mako_local_txn *txn, uint32_t expected_record_bytes,
    mako_rust_fast_one_put_holder_pool *pool, uint64_t sequence) noexcept {
  assert(pool != nullptr);
  assert(sequence != 0);
  assert(is_nonzero_power_of_two(pool->capacity));
  if (!rust_sequence_cache_order_allowed(txn->owner)) [[unlikely]]
    return reject_fast_preselected_record_terminal(txn);
  one_put_holder &holder = one_put_holder_for(pool, sequence);
  const uint64_t terminal =
      commit_preselected_unchecked_one_put_holder_single_producer_and_destroy(
          txn, expected_record_bytes, &holder, sequence);
  return pack_preselected_holder_result(terminal, holder);
}

// Recover the exact compact candidate emitted by the immediately preceding
// private fast Put without walking STO's write set. The fused entry is itself
// the consuming seal, so the Rust facade's active-transaction ownership and
// this retained direct-write witness make these scalar reads authoritative.
// A miss selects the checked general terminal while leaving txn untouched.
[[gnu::always_inline]] static inline uint32_t
trusted_fused_one_put_record_bytes(const mako_local_txn *txn) noexcept {
  const uint32_t exact_record_bytes = txn->record_fused_candidate_bytes;
  if (exact_record_bytes == 0)
    return 0;

#ifndef NDEBUG
  assert(txn->record_fast_path_eligible);
  assert(!txn->record_plan_sealed);
  assert(!txn->record_plan_ready);
  assert(txn->record_plan_bytes == 0);
  assert(txn->record_plan_ops == 0);
  assert(txn->record_fast_write.op ==
         Transaction::canonical_write_view::operation::put);
  assert(txn->record_fast_mutation_bytes != 0);
  assert(txn->record_fast_mutation_bytes <=
         kFastPutRecordBytesMax - kCacheRecordHeaderBytes);
  assert(exact_record_bytes ==
         kCacheRecordHeaderBytes + txn->record_fast_mutation_bytes);
  record_shape shape;
  assert(derive_fast_record_shape(txn, MAKO_RUST_FAST_RECORD_CHECKSUM_NONE,
                                  &shape));
  assert(shape.operations == 1);
  assert(shape.bytes == exact_record_bytes);
#endif
  return exact_record_bytes;
}

MAKO_RUST_FAST_DEFINITION_HIDDEN uint64_t
mako_rust_fast_txn_try_commit_fused_one_put_holder_single_producer_and_destroy(
    mako_local_txn *txn, uint64_t *acknowledged, const uint8_t *unhealthy,
    mako_rust_fast_spsc_holder_control *control,
    uint64_t capacity_limit) noexcept {
  // Rust's thread-affine lease owns txn and the next holder generation for
  // this entire synchronous call. ACK and health name naturally aligned Rust
  // atomic storage; GCC/Clang builtins establish the exact cross-language
  // order without pretending these C ABI pointer types are C++ atomics.
  assert(txn != nullptr);
  assert(on_owner_thread(txn));
  assert(!txn->poisoned);
  assert(!local_worker_poisoned);
  assert(txn->active);
  assert(local_active_txn == txn);
  assert(txn->fast_table_impl != nullptr);
  assert(control != nullptr);
  assert(control->pool != nullptr);
  assert(control->holder_base != nullptr);
  assert(control->holder_base == control->pool->holders.get());
  assert(control->holder_mask == control->pool->mask);
  assert(control->acknowledged != nullptr);
  assert(control->unhealthy != nullptr);
  assert(control->capacity != 0);
  assert(control->capacity <= control->pool->capacity);
  assert(control->reserved == 0);
  assert(acknowledged != nullptr);
  assert(unhealthy != nullptr);
  assert(acknowledged == control->acknowledged);
  assert(unhealthy == control->unhealthy);
  assert(reinterpret_cast<uintptr_t>(acknowledged) % alignof(uint64_t) == 0);

  const uint32_t exact_record_bytes = trusted_fused_one_put_record_bytes(txn);
  if (!rust_sequence_cache_order_allowed(txn->owner)) [[unlikely]] {
    const uint64_t terminal =
        abort_fast_record_terminal(txn, MAKO_LOCAL_INVALID_ARGUMENT);
    control->cold_out = mako_rust_fast_preselected_record_result{terminal, 0};
    if (exact_record_bytes != 0) {
      control->cold_out =
          pack_fused_holder_cold_result(control->cold_out, exact_record_bytes);
    }
    return pack_fused_holder_control_word(
        MAKO_RUST_FAST_FUSED_HOLDER_CONSUMED_OUTCOME);
  }
  if (exact_record_bytes - UINT32_C(1) >= control->max_record_bytes) [[unlikely]] {
    return pack_fused_holder_control_word(
        MAKO_RUST_FAST_FUSED_HOLDER_UNTOUCHED_NEED_GENERAL);
  }

  if (__atomic_load_n(unhealthy, __ATOMIC_ACQUIRE) != 0) [[unlikely]] {
    return pack_fused_holder_control_word(
        MAKO_RUST_FAST_FUSED_HOLDER_UNTOUCHED_NEED_SLOW, exact_record_bytes);
  }
  const uint64_t tail = __atomic_load_n(acknowledged, __ATOMIC_RELAXED);
  if (tail >= capacity_limit) [[unlikely]] {
    return pack_fused_holder_control_word(
        MAKO_RUST_FAST_FUSED_HOLDER_UNTOUCHED_NEED_SLOW, exact_record_bytes);
  }

  const uint64_t sequence = tail + 1;
  auto *const holders = static_cast<one_put_holder *>(control->holder_base);
  one_put_holder &holder =
      holders[static_cast<size_t>(sequence - 1) & control->holder_mask];
  const uint64_t terminal =
      commit_preselected_unchecked_one_put_holder_single_producer_and_destroy(
          txn, exact_record_bytes, &holder, sequence);
  constexpr uint64_t kExactOk =
      pack_fast_terminal_result(MAKO_LOCAL_OK, MAKO_LOCAL_OK);
  if (terminal == kExactOk) [[likely]] {
    const uint32_t timestamp = holder.mako_timestamp;
#ifndef NDEBUG
    assert(holder.state == one_put_holder_state::sealed);
    assert(timestamp != 0);
#endif
    // ACK remains the canonical healthy tail. Rust reconstructs its local
    // cursor only if this post-commit fail-stop diversion is taken.
    if (__atomic_load_n(unhealthy, __ATOMIC_ACQUIRE) != 0) [[unlikely]] {
      control->cold_out = pack_fused_holder_cold_result(
          pack_preselected_holder_result(terminal, holder),
          exact_record_bytes);
      return pack_fused_holder_control_word(
          MAKO_RUST_FAST_FUSED_HOLDER_CONSUMED_COMMITTED_UNPUBLISHED,
          timestamp);
    }

    __atomic_store_n(acknowledged, sequence, __ATOMIC_RELEASE);
    return pack_fused_holder_control_word(
        MAKO_RUST_FAST_FUSED_HOLDER_CONSUMED_PUBLISHED);
  }

  // The full raw outcome is cold. Rust synchronizes its pre-acceptance cursor
  // before it inspects and possibly pins this generation.
  control->cold_out = pack_fused_holder_cold_result(
      pack_preselected_holder_result(terminal, holder), exact_record_bytes);
  return pack_fused_holder_control_word(
      MAKO_RUST_FAST_FUSED_HOLDER_CONSUMED_OUTCOME);
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

MAKO_RUST_FAST_DEFINITION_HIDDEN uint8_t
mako_rust_fast_test_txn_can_order_record_after_validation(
    const mako_local_txn *txn) noexcept {
  if (txn == nullptr || !txn->active || txn->poisoned ||
      !on_owner_thread(txn) || local_active_txn != txn ||
      TThread::txn == nullptr)
    return 0;
  return TThread::txn->can_order_record_after_validation() ? 1 : 0;
}

MAKO_RUST_FAST_DEFINITION_HIDDEN const uint8_t *
mako_rust_fast_test_txn_staged_one_put_value(
    const mako_local_txn *txn, uint32_t *length_out) noexcept {
  if (length_out == nullptr) return nullptr;
  *length_out = 0;
  if (txn == nullptr || txn->encoded_values_used != 1 ||
      txn->encoded_values[0].size() <
          static_cast<size_t>(mako::EXTRA_BITS_FOR_VALUE))
    return nullptr;
  const size_t length = txn->encoded_values[0].size() -
                        static_cast<size_t>(mako::EXTRA_BITS_FOR_VALUE);
  if (length > UINT32_MAX) return nullptr;
  *length_out = static_cast<uint32_t>(length);
  return reinterpret_cast<const uint8_t *>(
      txn->encoded_values[0].data());
}
#endif

#undef MAKO_RUST_FAST_DEFINITION_HIDDEN

}  // extern "C"
