// Contract tests for the pure-C local STO/MassTrans boundary.

#include "mako/storage/mako_local_abi.h"
#include "mako/storage/mako_local_rust_fast_abi.h"
#include "mako/sto/MassTrans.hh"
#include "mako/sto/Transaction.hh"
#include "mako/sto/common.hh"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

std::atomic<uint64_t> next_table_id{10000};

struct HookObservation {
  int calls = 0;
  uint32_t timestamp = 0;
};

struct CommitPhaseObservation {
  std::array<uint32_t, 8> phases{};
  std::array<uint32_t, 8> timestamps{};
  size_t calls = 0;

  void reset() noexcept {
    phases.fill(0);
    timestamps.fill(0);
    calls = 0;
  }
};

void record_commit_phase(void *context, uint32_t phase,
                         uint32_t timestamp) noexcept {
  auto *observation = static_cast<CommitPhaseObservation *>(context);
  if (observation->calls < observation->phases.size()) {
    observation->phases[observation->calls] = phase;
    observation->timestamps[observation->calls] = timestamp;
  }
  ++observation->calls;
}

uint32_t read_u32_be_unchecked(const uint8_t *bytes) noexcept {
  uint32_t value = 0;
  for (unsigned index = 0; index != 4; ++index)
    value = (value << 8) | bytes[index];
  return value;
}

uint64_t read_u64_be_unchecked(const uint8_t *bytes) noexcept {
  uint64_t value = 0;
  for (unsigned index = 0; index != 8; ++index)
    value = (value << 8) | bytes[index];
  return value;
}

struct PreselectedCommitObservation {
  CommitPhaseObservation phases;
  const uint8_t *record = nullptr;
  size_t record_bytes = 0;
  uint64_t sequence = 0;
  uint64_t table_id = 0;
  const std::string *key = nullptr;
  const std::string *value = nullptr;
  bool complete_at_preinstall = false;
};

void observe_preselected_commit(void *context, uint32_t phase,
                                uint32_t timestamp) noexcept {
  auto *observation = static_cast<PreselectedCommitObservation *>(context);
  record_commit_phase(&observation->phases, phase, timestamp);
  if (phase != MAKO_LOCAL_TEST_COMMIT_PREINSTALL_ACCEPTED) return;
  constexpr std::array<uint8_t, 8> magic{
      'M', 'A', 'K', 'O', 'N', 'O', 'C', '\0'};
  constexpr size_t header_bytes = 26;
  constexpr size_t operation_header_bytes = 17;
  if (observation->record == nullptr || observation->key == nullptr ||
      observation->value == nullptr ||
      observation->record_bytes != header_bytes + operation_header_bytes +
                                       observation->key->size() +
                                       observation->value->size()) {
    return;
  }
  const uint8_t *const record = observation->record;
  const uint8_t *const payload = record + header_bytes + operation_header_bytes;
  observation->complete_at_preinstall =
      std::equal(magic.begin(), magic.end(), record) && record[8] == 0 &&
      record[9] == 4 &&
      read_u64_be_unchecked(record + 10) == observation->sequence &&
      read_u32_be_unchecked(record + 18) == timestamp &&
      read_u32_be_unchecked(record + 22) == 1 && record[26] == 1 &&
      read_u64_be_unchecked(record + 27) == observation->table_id &&
      read_u32_be_unchecked(record + 35) == observation->key->size() &&
      read_u32_be_unchecked(record + 39) == observation->value->size() &&
      std::memcmp(payload, observation->key->data(),
                  observation->key->size()) == 0 &&
      std::memcmp(payload + observation->key->size(),
                  observation->value->data(), observation->value->size()) == 0;
}

struct FusedHolderCommitObservation {
  uint64_t *producer_next = nullptr;
  uint64_t *acknowledged = nullptr;
  uint8_t *unhealthy = nullptr;
  uint64_t producer_at_install = UINT64_MAX;
  uint64_t acknowledged_at_install = UINT64_MAX;
  uint32_t timestamp_at_install = 0;
  bool make_unhealthy_at_install = false;
};

void observe_fused_holder_commit(void *context, uint32_t phase,
                                 uint32_t timestamp) noexcept {
  if (phase != MAKO_LOCAL_TEST_COMMIT_ALL_WRITES_INSTALLED)
    return;
  auto *observation = static_cast<FusedHolderCommitObservation *>(context);
  observation->producer_at_install =
      __atomic_load_n(observation->producer_next, __ATOMIC_RELAXED);
  observation->acknowledged_at_install =
      __atomic_load_n(observation->acknowledged, __ATOMIC_RELAXED);
  observation->timestamp_at_install = timestamp;
  if (observation->make_unhealthy_at_install)
    __atomic_store_n(observation->unhealthy, UINT8_C(1), __ATOMIC_RELEASE);
}

struct ParkingCommitObserver {
  CommitPhaseObservation observation;
  std::atomic<bool> *parked;
  std::atomic<bool> *release;
};

void park_after_writeset_lock(void *context, uint32_t phase,
                              uint32_t timestamp) noexcept {
  auto *parking = static_cast<ParkingCommitObserver *>(context);
  record_commit_phase(&parking->observation, phase, timestamp);
  if (phase == MAKO_LOCAL_TEST_COMMIT_WRITESET_LOCKED) {
    parking->parked->store(true, std::memory_order_release);
    while (!parking->release->load(std::memory_order_acquire))
      std::this_thread::yield();
  }
}

#if defined(MAKO_LOCAL_TEST_HOOKS)
struct ParkingPhaseCommitObserver {
  uint32_t target_phase = 0;
  std::atomic<bool> *parked = nullptr;
  std::atomic<bool> *release = nullptr;
};

void park_at_commit_phase(void *context, uint32_t phase,
                          uint32_t) noexcept {
  auto *parking = static_cast<ParkingPhaseCommitObserver *>(context);
  if (phase != parking->target_phase) return;
  parking->parked->store(true, std::memory_order_release);
  while (!parking->release->load(std::memory_order_acquire))
    std::this_thread::yield();
}

struct SignalPhaseCommitObserver {
  uint32_t target_phase = 0;
  std::atomic<bool> *reached = nullptr;
};

void signal_commit_phase(void *context, uint32_t phase,
                         uint32_t) noexcept {
  auto *signal = static_cast<SignalPhaseCommitObserver *>(context);
  if (phase == signal->target_phase)
    signal->reached->store(true, std::memory_order_release);
}

struct PayloadCopyPause {
  std::atomic<int> calls{0};
  std::atomic<bool> parked{false};
  std::atomic<bool> release{false};
  std::atomic<bool> timed_out{false};
};

void park_at_payload_copy_midpoint(void *context) noexcept {
  auto *pause = static_cast<PayloadCopyPause *>(context);
  pause->calls.fetch_add(1, std::memory_order_relaxed);
  pause->parked.store(true, std::memory_order_release);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(5);
  while (!pause->release.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  if (!pause->release.load(std::memory_order_acquire))
    pause->timed_out.store(true, std::memory_order_relaxed);
}

#if STO_OPACITY
using DirectComparatorTable =
    MassTrans<std::string, versioned_str_struct, true>;
#else
using DirectComparatorTable =
    MassTrans<std::string, versioned_str_struct, false>;
#endif

DirectComparatorTable &direct_comparator_table() {
  // MassTrans has process-lifetime RCU ownership. Keep the pointer rooted for
  // the test process just like the local ABI's table registry.
  static auto *table = new DirectComparatorTable();
  return *table;
}

std::atomic<int> direct_comparator_calls{0};

bool direct_comparator_false(const std::string &, const std::string &) {
  direct_comparator_calls.fetch_add(1, std::memory_order_relaxed);
  return false;
}

bool direct_comparator_true(const std::string &, const std::string &) {
  direct_comparator_calls.fetch_add(1, std::memory_order_relaxed);
  return true;
}
#endif

int accept_hook(void *context, uint32_t timestamp) {
  auto *observation = static_cast<HookObservation *>(context);
  observation->calls++;
  observation->timestamp = timestamp;
  return 1;
}

int reject_hook(void *context, uint32_t timestamp) {
  accept_hook(context, timestamp);
  return 0;
}

int throwing_hook(void *, uint32_t) {
  throw 7;
}

class LocalAbiTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(mako_local_thread_attach(), MAKO_LOCAL_OK);
    ASSERT_EQ(mako_local_db_open(&db), MAKO_LOCAL_OK);
    ASSERT_NE(db, nullptr);
    open_table("primary", &primary);
  }

  void TearDown() override {
#if defined(MAKO_LOCAL_TEST_HOOKS)
    EXPECT_EQ(mako_local_test_clear_commit_observer(), MAKO_LOCAL_OK);
    EXPECT_EQ(mako_local_test_clear_cleanup_failure(), MAKO_LOCAL_OK);
#endif
    if (txn_for_cleanup != nullptr) {
      EXPECT_EQ(mako_local_txn_destroy(txn_for_cleanup), MAKO_LOCAL_OK);
      txn_for_cleanup = nullptr;
    }
    if (holder_pool != nullptr) {
      EXPECT_EQ(mako_rust_fast_one_put_holder_pool_destroy(holder_pool),
                MAKO_LOCAL_OK);
      holder_pool = nullptr;
    }
    if (db != nullptr) EXPECT_EQ(mako_local_db_close(db), MAKO_LOCAL_OK);
  }

  void open_table(const std::string &name, mako_local_table **out) {
    ASSERT_EQ(mako_local_table_open(
                  db, reinterpret_cast<const uint8_t *>(name.data()),
                  name.size(), next_table_id.fetch_add(1), out),
              MAKO_LOCAL_OK);
    ASSERT_NE(*out, nullptr);
  }

  mako_local_txn *begin() {
    mako_local_txn *txn = nullptr;
    EXPECT_EQ(mako_local_txn_begin(db, &txn), MAKO_LOCAL_OK);
    EXPECT_NE(txn, nullptr);
    txn_for_cleanup = txn;
    return txn;
  }

  static int put(mako_local_txn *txn, mako_local_table *table,
                 const std::string &key, const std::string &value,
                 uint8_t *created = nullptr) {
    uint8_t local_created = 0;
    return mako_local_txn_put(
        txn, table, reinterpret_cast<const uint8_t *>(key.data()), key.size(),
        reinterpret_cast<const uint8_t *>(value.data()), value.size(),
        created == nullptr ? &local_created : created);
  }

  static int insert(mako_local_txn *txn, mako_local_table *table,
                    const std::string &key, const std::string &value,
                    uint8_t *inserted = nullptr) {
    uint8_t local_inserted = 0;
    return mako_local_txn_insert(
        txn, table, reinterpret_cast<const uint8_t *>(key.data()), key.size(),
        reinterpret_cast<const uint8_t *>(value.data()), value.size(),
        inserted == nullptr ? &local_inserted : inserted);
  }

  static int remove(mako_local_txn *txn, mako_local_table *table,
                    const std::string &key, uint8_t *existed = nullptr) {
    uint8_t local_existed = 0;
    return mako_local_txn_remove(
        txn, table, reinterpret_cast<const uint8_t *>(key.data()), key.size(),
        existed == nullptr ? &local_existed : existed);
  }

  static std::pair<int, std::optional<std::string>> get(
      mako_local_txn *txn, mako_local_table *table, const std::string &key) {
    uint8_t *bytes = nullptr;
    size_t len = 0;
    uint8_t found = 0;
    int status = mako_local_txn_get(
        txn, table, reinterpret_cast<const uint8_t *>(key.data()), key.size(),
        &bytes, &len, &found);
    std::optional<std::string> value;
    if (status == MAKO_LOCAL_OK && found)
      value.emplace(reinterpret_cast<const char *>(bytes), len);
    mako_local_bytes_free(bytes);
    return {status, std::move(value)};
  }

  void destroy_tracked(mako_local_txn *txn) {
    const int status = mako_local_txn_destroy(txn);
    EXPECT_EQ(status, MAKO_LOCAL_OK);
    if (status == MAKO_LOCAL_OK && txn_for_cleanup == txn)
      txn_for_cleanup = nullptr;
  }

  void abort_and_destroy(mako_local_txn *txn) {
    EXPECT_EQ(mako_local_txn_abort(txn), MAKO_LOCAL_OK);
    destroy_tracked(txn);
  }

  void commit_and_destroy(mako_local_txn *txn) {
    EXPECT_EQ(mako_local_txn_commit(txn), MAKO_LOCAL_OK);
    destroy_tracked(txn);
  }

  void create_holder_pool(size_t capacity, uint32_t key_reserve = 0,
                          uint32_t value_reserve = 0) {
    ASSERT_EQ(holder_pool, nullptr);
    ASSERT_EQ(mako_rust_fast_one_put_holder_pool_create(
                  capacity, key_reserve, value_reserve, &holder_pool),
              MAKO_LOCAL_OK);
    ASSERT_NE(holder_pool, nullptr);
  }

  mako_local_db *db = nullptr;
  mako_local_table *primary = nullptr;
  mako_local_txn *txn_for_cleanup = nullptr;
  mako_rust_fast_one_put_holder_pool *holder_pool = nullptr;
  CommitPhaseObservation commit_observation;
};

uint64_t fast_put(mako_local_txn *txn, const std::string &key,
                  const std::string &value) {
  EXPECT_LE(key.size(), std::numeric_limits<uint32_t>::max());
  EXPECT_LE(value.size(), std::numeric_limits<uint32_t>::max());
  return mako_rust_fast_txn_put(txn,
                                reinterpret_cast<const uint8_t *>(key.data()),
                                static_cast<uint32_t>(key.size()),
                                reinterpret_cast<const uint8_t *>(value.data()),
                                static_cast<uint32_t>(value.size()));
}

mako_rust_fast_spsc_holder_control make_fused_holder_control(
    mako_rust_fast_one_put_holder_pool *pool, uint64_t *acknowledged,
    const uint8_t *unhealthy, uint64_t capacity,
    uint32_t max_record_bytes = UINT32_MAX) {
  void *holder_base = nullptr;
  size_t holder_mask = 0;
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_get_hot_layout(
                pool, &holder_base, &holder_mask),
            MAKO_LOCAL_OK);
  EXPECT_NE(holder_base, nullptr);
  return mako_rust_fast_spsc_holder_control{
      pool, holder_base, holder_mask, acknowledged, unhealthy,
      capacity, max_record_bytes, 0, {0, 0}};
}

struct alignas(64) TestRustPublicationCell {
  uint64_t turn = UINT64_MAX;
  uint32_t mako_timestamp = 0;
  uint32_t timestamp_padding = 0;
  size_t record_bytes = SIZE_MAX;
  std::array<uint8_t, 40> padding{};
};

struct alignas(64) TestRustArenaBlock {
  std::array<uint8_t, 256> bytes{};
};

static_assert(sizeof(TestRustPublicationCell) == 64);
static_assert(alignof(TestRustPublicationCell) == 64);
static_assert(offsetof(TestRustPublicationCell, turn) == 0);
static_assert(offsetof(TestRustPublicationCell, mako_timestamp) == 8);
static_assert(offsetof(TestRustPublicationCell, record_bytes) == 16);
static_assert(sizeof(TestRustArenaBlock) == 256);
static_assert(alignof(TestRustArenaBlock) == 64);

mako_rust_fast_native_ordered_arena_control make_native_arena_control(
    uint64_t *next_bound, const uint8_t *unhealthy,
    TestRustPublicationCell *publications, TestRustArenaBlock *arena,
    size_t mask = 3, uint32_t block_bytes = 256) {
  return mako_rust_fast_native_ordered_arena_control{
      next_bound,
      unhealthy,
      reinterpret_cast<uint8_t *>(publications),
      reinterpret_cast<uint8_t *>(arena),
      mask,
      2,
      static_cast<uint32_t>(sizeof(TestRustPublicationCell)),
      static_cast<uint32_t>(sizeof(TestRustArenaBlock)),
      block_bytes};
}

struct ThinRecordBinding {
  std::vector<uint8_t> *storage = nullptr;
  uint64_t sequence = 0;
  int calls = 0;
  uint32_t timestamp = 0;
  size_t exact_bytes = 0;
  bool accept = true;
  bool invalidate_sequence = false;
  std::atomic<uint64_t> *next_sequence = nullptr;
  std::atomic<int> *published_calls = nullptr;
};

int bind_thin_record(void *context, uint32_t timestamp, size_t exact_bytes,
                     uint64_t *sequence_out, uint8_t **record_bytes_out,
                     size_t *record_capacity_out) {
  auto *binding = static_cast<ThinRecordBinding *>(context);
  ++binding->calls;
  binding->timestamp = timestamp;
  binding->exact_bytes = exact_bytes;
  if (binding->published_calls != nullptr)
    binding->published_calls->fetch_add(1, std::memory_order_release);
  if (!binding->accept) return 0;
  if (binding->next_sequence != nullptr)
    binding->sequence = binding->next_sequence->fetch_add(
        UINT64_C(1), std::memory_order_relaxed);
  *sequence_out = binding->invalidate_sequence ? 0 : binding->sequence;
  *record_bytes_out = binding->storage == nullptr
      ? nullptr
      : binding->storage->data();
  *record_capacity_out = binding->storage == nullptr
      ? 0
      : binding->storage->size();
  return 1;
}

int bind_native_ordered_thin_record(void *context, uint32_t timestamp,
                                    size_t exact_bytes,
                                    uint64_t *sequence_in_out,
                                    uint8_t **record_bytes_out,
                                    size_t *record_capacity_out) {
  auto *binding = static_cast<ThinRecordBinding *>(context);
  ++binding->calls;
  binding->timestamp = timestamp;
  binding->exact_bytes = exact_bytes;
  binding->sequence = *sequence_in_out;
  if (binding->published_calls != nullptr)
    binding->published_calls->fetch_add(1, std::memory_order_release);
  if (!binding->accept) return 0;
  *sequence_in_out = binding->invalidate_sequence ? 0 : binding->sequence;
  *record_bytes_out = binding->storage == nullptr
      ? nullptr
      : binding->storage->data();
  *record_capacity_out = binding->storage == nullptr
      ? 0
      : binding->storage->size();
  return 1;
}

uint32_t test_crc32c(const uint8_t *bytes, size_t length) {
  constexpr uint32_t polynomial = UINT32_C(0x82f63b78);
  uint32_t crc = UINT32_MAX;
  for (size_t index = 0; index != length; ++index) {
    crc ^= bytes[index];
    for (unsigned bit = 0; bit != 8; ++bit) {
      const uint32_t low_mask = UINT32_C(0) - (crc & UINT32_C(1));
      crc = (crc >> 1) ^ (polynomial & low_mask);
    }
  }
  return ~crc;
}

struct DecodedThinMutation {
  uint8_t tag = 0;
  uint64_t table_id = 0;
  std::string key;
  std::string value;
};

struct DecodedThinRecord {
  uint64_t sequence = 0;
  uint32_t timestamp = 0;
  std::vector<DecodedThinMutation> mutations;
};

bool decode_thin_record(const std::vector<uint8_t> &bytes,
                        DecodedThinRecord *record) {
  constexpr size_t minimum_bytes = 26;
  constexpr std::array<uint8_t, 8> crc32c_magic{
      'M', 'A', 'K', 'O', 'C', 'M', 'T', '\0'};
  constexpr std::array<uint8_t, 8> unchecked_magic{
      'M', 'A', 'K', 'O', 'N', 'O', 'C', '\0'};
  if (record == nullptr || bytes.size() < minimum_bytes) return false;

  const uint16_t encoded_version =
      static_cast<uint16_t>(bytes[8]) << 8 | static_cast<uint16_t>(bytes[9]);
  if (encoded_version != 3 && encoded_version != 4) return false;
  const auto &expected_magic =
      encoded_version == 3 ? crc32c_magic : unchecked_magic;
  if (!std::equal(expected_magic.begin(), expected_magic.end(), bytes.begin()))
    return false;
  if (encoded_version == 3 && bytes.size() < minimum_bytes + 4) return false;
  const size_t operations_end =
      encoded_version == 3 ? bytes.size() - 4 : bytes.size();
  auto read_u16 = [&](size_t *cursor, uint16_t *out) {
    if (*cursor > operations_end || operations_end - *cursor < 2)
      return false;
    *out = static_cast<uint16_t>(bytes[*cursor]) << 8 |
           static_cast<uint16_t>(bytes[*cursor + 1]);
    *cursor += 2;
    return true;
  };
  auto read_u32 = [&](size_t *cursor, uint32_t *out) {
    if (*cursor > operations_end || operations_end - *cursor < 4)
      return false;
    *out = 0;
    for (unsigned index = 0; index != 4; ++index)
      *out = (*out << 8) | bytes[(*cursor)++];
    return true;
  };
  auto read_u64 = [&](size_t *cursor, uint64_t *out) {
    if (*cursor > operations_end || operations_end - *cursor < 8)
      return false;
    *out = 0;
    for (unsigned index = 0; index != 8; ++index)
      *out = (*out << 8) | bytes[(*cursor)++];
    return true;
  };

  if (encoded_version == 3) {
    uint32_t stored_checksum = 0;
    for (unsigned index = 0; index != 4; ++index)
      stored_checksum = (stored_checksum << 8) |
                        bytes[operations_end + index];
    if (stored_checksum != test_crc32c(bytes.data(), operations_end))
      return false;
  }

  size_t cursor = expected_magic.size();
  uint16_t version = 0;
  uint32_t operation_count = 0;
  if (!read_u16(&cursor, &version) || version != encoded_version ||
      !read_u64(&cursor, &record->sequence) ||
      !read_u32(&cursor, &record->timestamp) ||
      !read_u32(&cursor, &operation_count))
    return false;

  record->mutations.clear();
  record->mutations.reserve(operation_count);
  for (uint32_t operation = 0; operation != operation_count; ++operation) {
    if (cursor >= operations_end) return false;
    DecodedThinMutation mutation;
    mutation.tag = bytes[cursor++];
    uint32_t key_length = 0;
    uint32_t value_length = 0;
    if ((mutation.tag != 1 && mutation.tag != 2) ||
        !read_u64(&cursor, &mutation.table_id) ||
        !read_u32(&cursor, &key_length) ||
        !read_u32(&cursor, &value_length) ||
        key_length > operations_end - cursor)
      return false;
    mutation.key.assign(reinterpret_cast<const char *>(bytes.data() + cursor),
                        key_length);
    cursor += key_length;
    if (value_length > operations_end - cursor ||
        (mutation.tag == 2 && value_length != 0))
      return false;
    mutation.value.assign(
        reinterpret_cast<const char *>(bytes.data() + cursor), value_length);
    cursor += value_length;
    record->mutations.push_back(std::move(mutation));
  }
  return cursor == operations_end;
}

struct AbiScanChunk {
  int status = MAKO_LOCAL_INTERNAL;
  std::vector<std::pair<std::string, std::string>> entries;
  size_t arena_used = 0;
  size_t arena_required = 0;
  uint8_t done = 0;
};

AbiScanChunk scan_chunk(mako_local_txn *txn, mako_local_table *table,
                        const mako_local_scan_options &options, bool reverse,
                        size_t entry_capacity, size_t arena_capacity) {
  std::vector<mako_local_scan_entry> descriptors(entry_capacity);
  std::vector<uint8_t> arena(arena_capacity);
  size_t entry_count = std::numeric_limits<size_t>::max();
  size_t arena_used = std::numeric_limits<size_t>::max();
  size_t arena_required = std::numeric_limits<size_t>::max();
  uint8_t done = 99;
  auto *descriptor_data =
      descriptors.empty() ? nullptr : descriptors.data();
  auto *arena_data = arena.empty() ? nullptr : arena.data();
  const int status = reverse
      ? mako_local_txn_rscan_chunk(
            txn, table, &options, descriptor_data, entry_capacity, arena_data,
            arena_capacity, &entry_count, &arena_used, &arena_required, &done)
      : mako_local_txn_scan_chunk(
            txn, table, &options, descriptor_data, entry_capacity, arena_data,
            arena_capacity, &entry_count, &arena_used, &arena_required, &done);

  AbiScanChunk result{status, {}, arena_used, arena_required, done};
  if (status != MAKO_LOCAL_OK) return result;
  if (entry_count > descriptors.size() || arena_used > arena.size()) {
    ADD_FAILURE() << "scan ABI reported output beyond caller capacity";
    return result;
  }
  for (size_t i = 0; i != entry_count; ++i) {
    const auto &entry = descriptors[i];
    const size_t key_offset = entry.key_offset;
    const size_t key_length = entry.key_length;
    const size_t value_offset = entry.value_offset;
    const size_t value_length = entry.value_length;
    if (key_offset > arena_used || key_length > arena_used - key_offset ||
        value_offset > arena_used || value_length > arena_used - value_offset) {
      ADD_FAILURE() << "scan ABI reported a slice outside arena_used";
      return result;
    }
    const std::string key = key_length == 0
        ? std::string()
        : std::string(
              reinterpret_cast<const char *>(arena.data() + key_offset),
              key_length);
    const std::string value = value_length == 0
        ? std::string()
        : std::string(
              reinterpret_cast<const char *>(arena.data() + value_offset),
              value_length);
    result.entries.emplace_back(key, value);
  }
  return result;
}

TEST(MakoLocalAbiIdentity, VersionAndStatusStringsAreStable) {
  EXPECT_EQ(mako_local_abi_version(), MAKO_LOCAL_ABI_VERSION);
  const uint64_t features = mako_local_feature_bits();
  EXPECT_NE(features & MAKO_LOCAL_FEATURE_POINT_TRANSACTIONS, 0U);
#if READ_MY_WRITES
  EXPECT_NE(features & MAKO_LOCAL_FEATURE_READ_MY_WRITES, 0U);
  EXPECT_NE(features & MAKO_LOCAL_FEATURE_TRANSACTIONAL_SCANS, 0U);
  EXPECT_NE(features & MAKO_LOCAL_FEATURE_SCAN_READ_MY_WRITES, 0U);
#else
  EXPECT_EQ(features & MAKO_LOCAL_FEATURE_READ_MY_WRITES, 0U);
  EXPECT_EQ(features & MAKO_LOCAL_FEATURE_TRANSACTIONAL_SCANS, 0U);
  EXPECT_EQ(features & MAKO_LOCAL_FEATURE_SCAN_READ_MY_WRITES, 0U);
#endif
#if STO_OPACITY
  EXPECT_NE(features & MAKO_LOCAL_FEATURE_OPACITY, 0U);
#else
  EXPECT_EQ(features & MAKO_LOCAL_FEATURE_OPACITY, 0U);
#endif
#if defined(MAKO_LOCAL_TEST_HOOKS)
  EXPECT_NE(features & MAKO_LOCAL_FEATURE_TEST_COMMIT_OBSERVER, 0U);
  EXPECT_NE(features & MAKO_LOCAL_FEATURE_TEST_CLEANUP_FAILURES, 0U);
#else
  EXPECT_EQ(features & MAKO_LOCAL_FEATURE_TEST_COMMIT_OBSERVER, 0U);
  EXPECT_EQ(features & MAKO_LOCAL_FEATURE_TEST_CLEANUP_FAILURES, 0U);
  EXPECT_EQ(mako_local_test_set_commit_observer(record_commit_phase, nullptr),
            MAKO_LOCAL_FEATURE_UNAVAILABLE);
  EXPECT_EQ(mako_local_test_clear_commit_observer(),
            MAKO_LOCAL_FEATURE_UNAVAILABLE);
  EXPECT_EQ(mako_local_test_arm_cleanup_failure(
                MAKO_LOCAL_CLEANUP_BOUNDARY_BEGIN),
            MAKO_LOCAL_FEATURE_UNAVAILABLE);
  EXPECT_EQ(mako_local_test_clear_cleanup_failure(),
            MAKO_LOCAL_FEATURE_UNAVAILABLE);
#endif
  EXPECT_STREQ(mako_local_status_string(MAKO_LOCAL_OK), "ok");
  EXPECT_STREQ(mako_local_status_string(MAKO_LOCAL_CONFLICT),
               "transaction conflict");
  static_assert(MAKO_LOCAL_DUPLICATE_WRITE == 12);
  EXPECT_STREQ(mako_local_status_string(MAKO_LOCAL_DUPLICATE_WRITE),
               "second mutation of one key is not supported");
  EXPECT_STREQ(mako_local_status_string(MAKO_LOCAL_TXN_TOO_LARGE),
               "transaction exceeds the draft item budget");
  EXPECT_STREQ(mako_local_status_string(MAKO_LOCAL_VALUE_TOO_LARGE),
               "table name, key, or value exceeds the draft byte limit");
  EXPECT_STREQ(mako_local_status_string(MAKO_LOCAL_COMMIT_HOOK_REJECTED),
               "post-validation commit hook rejected transaction");
  EXPECT_STREQ(mako_local_status_string(MAKO_LOCAL_TIMESTAMP_EXHAUSTED),
               "Mako logical timestamp exhausted");
  static_assert(MAKO_LOCAL_BUFFER_TOO_SMALL == 17);
  EXPECT_STREQ(mako_local_status_string(MAKO_LOCAL_BUFFER_TOO_SMALL),
               "caller scan arena is too small for the next entry");
  static_assert(MAKO_LOCAL_FEATURE_UNAVAILABLE == 18);
  EXPECT_STREQ(mako_local_status_string(MAKO_LOCAL_FEATURE_UNAVAILABLE),
               "requested native feature is unavailable");
  static_assert(MAKO_LOCAL_WORKER_POISONED == 19);
  EXPECT_STREQ(mako_local_status_string(MAKO_LOCAL_WORKER_POISONED),
               "worker poisoned by uncertain native cleanup");
  static_assert(sizeof(mako_rust_fast_preselected_record_result) == 16);
  static_assert(alignof(mako_rust_fast_preselected_record_result) == 8);
  static_assert(offsetof(mako_rust_fast_preselected_record_result, terminal) ==
                0);
  static_assert(
      offsetof(mako_rust_fast_preselected_record_result, record_state) == 8);
  constexpr mako_rust_fast_preselected_record_result preselected_probe{
      UINT64_C(0), UINT64_C(1) | (UINT64_C(1) << 32)};
  static_assert(
      MAKO_RUST_FAST_PRESELECTED_RECORD_TIMESTAMP(preselected_probe) == 1);
  static_assert(MAKO_RUST_FAST_PRESELECTED_RECORD_WRITTEN(preselected_probe) ==
                1);
  static_assert(MAKO_RUST_FAST_PRESELECTED_RECORD_RESERVED(preselected_probe) ==
                0);
  static_assert(MAKO_RUST_FAST_PRESELECTED_HOLDER_SEALED(preselected_probe) ==
                1);
  static_assert(sizeof(mako_rust_fast_native_ordered_arena_control) == 56);
  static_assert(alignof(mako_rust_fast_native_ordered_arena_control) == 8);
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
  static_assert(alignof(mako_rust_fast_native_ordered_arena_result) == 8);
  static_assert(offsetof(mako_rust_fast_native_ordered_arena_result,
                         terminal) == 0);
  static_assert(offsetof(mako_rust_fast_native_ordered_arena_result,
                         ordered_sequence) == 8);
  static_assert(offsetof(mako_rust_fast_native_ordered_arena_result,
                         record_state) == 16);
  constexpr mako_rust_fast_native_ordered_arena_result arena_probe{
      0, 9, UINT64_C(7) | (UINT64_C(1) << 32)};
  static_assert(MAKO_RUST_FAST_NATIVE_ORDERED_ARENA_TIMESTAMP(arena_probe) ==
                7);
  static_assert(MAKO_RUST_FAST_NATIVE_ORDERED_ARENA_WRITTEN(arena_probe) == 1);
  static_assert(MAKO_RUST_FAST_NATIVE_ORDERED_ARENA_RESERVED(arena_probe) ==
                0);
  static_assert(sizeof(mako_rust_fast_spsc_holder_control) == 72);
  static_assert(alignof(mako_rust_fast_spsc_holder_control) == 8);
  static_assert(offsetof(mako_rust_fast_spsc_holder_control, pool) == 0);
  static_assert(offsetof(mako_rust_fast_spsc_holder_control, holder_base) ==
                8);
  static_assert(offsetof(mako_rust_fast_spsc_holder_control, holder_mask) ==
                16);
  static_assert(offsetof(mako_rust_fast_spsc_holder_control, acknowledged) ==
                24);
  static_assert(offsetof(mako_rust_fast_spsc_holder_control, unhealthy) == 32);
  static_assert(offsetof(mako_rust_fast_spsc_holder_control, capacity) == 40);
  static_assert(
      offsetof(mako_rust_fast_spsc_holder_control, max_record_bytes) == 48);
  static_assert(offsetof(mako_rust_fast_spsc_holder_control, reserved) == 52);
  static_assert(offsetof(mako_rust_fast_spsc_holder_control, cold_out) == 56);
  constexpr uint64_t fused_slow =
      (UINT64_C(12345) << 32) | MAKO_RUST_FAST_FUSED_HOLDER_UNTOUCHED_NEED_SLOW;
  static_assert(MAKO_RUST_FAST_FUSED_HOLDER_CODE(fused_slow) ==
                MAKO_RUST_FAST_FUSED_HOLDER_UNTOUCHED_NEED_SLOW);
  static_assert(MAKO_RUST_FAST_FUSED_HOLDER_PAYLOAD(fused_slow) == 12345);
  static_assert(sizeof(mako_rust_fast_one_put_holder_view) == 48);
  static_assert(alignof(mako_rust_fast_one_put_holder_view) == 8);
  static_assert(offsetof(mako_rust_fast_one_put_holder_view, sequence) == 0);
  static_assert(offsetof(mako_rust_fast_one_put_holder_view, table_id) == 8);
  static_assert(offsetof(mako_rust_fast_one_put_holder_view, key) == 16);
  static_assert(offsetof(mako_rust_fast_one_put_holder_view, value) == 24);
  static_assert(offsetof(mako_rust_fast_one_put_holder_view, key_len) == 32);
  static_assert(offsetof(mako_rust_fast_one_put_holder_view, value_len) == 36);
  static_assert(offsetof(mako_rust_fast_one_put_holder_view, mako_timestamp) ==
                40);
  static_assert(offsetof(mako_rust_fast_one_put_holder_view, reserved) == 44);
  static_assert(sizeof(mako_local_scan_entry) == 4 * sizeof(uint32_t));
  EXPECT_EQ(mako_local_db_options_size(), MAKO_LOCAL_DB_OPTIONS_V0_SIZE);
  EXPECT_EQ(mako_local_db_options_size(),
            offsetof(mako_local_db_options, flags) + sizeof(uint32_t));
  EXPECT_EQ(mako_local_scan_options_size(), MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE);
  EXPECT_EQ(mako_local_scan_options_size(),
            offsetof(mako_local_scan_options, resume_len) + sizeof(size_t));
  EXPECT_EQ(mako_local_scan_entry_size(), sizeof(mako_local_scan_entry));
  EXPECT_EQ(mako_local_advance_mako_timestamp_past(0),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(mako_local_advance_mako_timestamp_past(UINT32_MAX),
            MAKO_LOCAL_TIMESTAMP_EXHAUSTED);
  static_assert(MAKO_LOCAL_MAX_MAKO_TIMESTAMP ==
                (std::numeric_limits<uint32_t>::max() - 9) / 10);
  EXPECT_EQ(
      mako_local_advance_mako_timestamp_past(MAKO_LOCAL_MAX_MAKO_TIMESTAMP),
      MAKO_LOCAL_TIMESTAMP_EXHAUSTED);
  EXPECT_EQ(mako_local_advance_mako_timestamp_past(
                MAKO_LOCAL_MAX_MAKO_TIMESTAMP + 1),
            MAKO_LOCAL_TIMESTAMP_EXHAUSTED);
  EXPECT_EQ(mako_local_db_open(nullptr), MAKO_LOCAL_INVALID_ARGUMENT);
  mako_local_db_options db_options{MAKO_LOCAL_DB_OPTIONS_V0_SIZE, 0};
  EXPECT_EQ(mako_local_db_open_with_options(&db_options, nullptr),
            MAKO_LOCAL_INVALID_ARGUMENT);
  auto *poison_db = reinterpret_cast<mako_local_db *>(uintptr_t{1});
  EXPECT_EQ(mako_local_db_open_with_options(nullptr, &poison_db),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(poison_db, nullptr);
  db_options.struct_size = MAKO_LOCAL_DB_OPTIONS_V0_SIZE - 1;
  poison_db = reinterpret_cast<mako_local_db *>(uintptr_t{1});
  EXPECT_EQ(mako_local_db_open_with_options(&db_options, &poison_db),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(poison_db, nullptr);
  db_options = mako_local_db_options{MAKO_LOCAL_DB_OPTIONS_V0_SIZE, 1};
  poison_db = reinterpret_cast<mako_local_db *>(uintptr_t{1});
  EXPECT_EQ(mako_local_db_open_with_options(&db_options, &poison_db),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(poison_db, nullptr);
  db_options = mako_local_db_options{MAKO_LOCAL_DB_OPTIONS_V0_SIZE + 8, 0};
  mako_local_db *option_db = nullptr;
  ASSERT_EQ(mako_local_db_open_with_options(&db_options, &option_db),
            MAKO_LOCAL_OK);
  ASSERT_NE(option_db, nullptr);
  EXPECT_EQ(mako_local_db_close(option_db), MAKO_LOCAL_OK);

  auto *poison_table = reinterpret_cast<mako_local_table *>(uintptr_t{1});
  EXPECT_EQ(mako_local_table_open(nullptr, nullptr, 0, 0, &poison_table),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(poison_table, nullptr);
  auto *poison_txn = reinterpret_cast<mako_local_txn *>(uintptr_t{1});
  EXPECT_EQ(mako_local_txn_begin(nullptr, &poison_txn),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(poison_txn, nullptr);
  uint8_t result = 99;
  EXPECT_EQ(mako_local_txn_put(nullptr, nullptr, nullptr, 0, nullptr, 0,
                               &result),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(result, 0);
}

TEST(MakoLocalAbiIdentity, FullStatusCatalogIsDenseAndStable) {
  struct StatusEntry {
    const char *name;
    int value;
    const char *message;
  };

#define MAKO_LOCAL_STATUS_ENTRY(short_name, c_symbol, message)                 \
  {#short_name, c_symbol, message},
  constexpr StatusEntry catalog[] = {
      MAKO_LOCAL_FOR_EACH_STATUS(MAKO_LOCAL_STATUS_ENTRY)};
#undef MAKO_LOCAL_STATUS_ENTRY

  constexpr size_t catalog_size = sizeof(catalog) / sizeof(catalog[0]);
  static_assert(catalog_size == 20);
  std::array<bool, catalog_size> seen{};

  for (size_t index = 0; index != catalog_size; ++index) {
    const auto &entry = catalog[index];
    EXPECT_EQ(entry.value, static_cast<int>(index))
        << entry.name << " changed its assigned status number";
    ASSERT_GE(entry.value, 0) << entry.name;
    ASSERT_LT(static_cast<size_t>(entry.value), catalog_size) << entry.name;
    EXPECT_FALSE(seen[entry.value])
        << entry.name << " duplicates status " << entry.value;
    seen[entry.value] = true;
    EXPECT_STREQ(mako_local_status_string(entry.value), entry.message)
        << entry.name;
  }
  for (size_t value = 0; value != seen.size(); ++value)
    EXPECT_TRUE(seen[value]) << "missing status " << value;

  EXPECT_STREQ(mako_local_status_string(-1), "unknown mako-local status");
  EXPECT_STREQ(mako_local_status_string(static_cast<int>(catalog_size)),
               "unknown mako-local status");
  EXPECT_STREQ(mako_local_status_string(INT_MAX), "unknown mako-local status");
}

TEST(MakoLocalAbiIdentity, WorkerHealthIsThreadLocalAndNonMutating) {
  const uint64_t quarantined_before =
      mako_local_quarantined_worker_count();
  int health_before = MAKO_LOCAL_INTERNAL;
  int attach = MAKO_LOCAL_INTERNAL;
  int health_after = MAKO_LOCAL_INTERNAL;
  int attach_again = MAKO_LOCAL_INTERNAL;
  std::thread worker([&] {
    health_before = mako_local_worker_health();
    attach = mako_local_thread_attach();
    health_after = mako_local_worker_health();
    attach_again = mako_local_thread_attach();
  });
  worker.join();

  EXPECT_EQ(health_before, MAKO_LOCAL_NOT_ATTACHED);
  EXPECT_EQ(attach, MAKO_LOCAL_OK);
  EXPECT_EQ(health_after, MAKO_LOCAL_OK);
  EXPECT_EQ(attach_again, MAKO_LOCAL_OK);
  EXPECT_EQ(mako_local_quarantined_worker_count(), quarantined_before);
}

TEST(MakoLocalAbiIdentity, RecoveryLeavesTheFinalTimestampMintable) {
  auto &clock = sync_util::sync_logger::local_replica_id;
  const uint32_t saved = clock.exchange(1, std::memory_order_acq_rel);

  EXPECT_EQ(mako_local_advance_mako_timestamp_past(
                MAKO_LOCAL_MAX_MAKO_TIMESTAMP - 1),
            MAKO_LOCAL_OK);
  uint32_t timestamp = 0;
  EXPECT_TRUE(Transaction::try_allocate_mako_timestamp(timestamp));
  EXPECT_EQ(timestamp, MAKO_LOCAL_MAX_MAKO_TIMESTAMP);
  EXPECT_FALSE(Transaction::try_allocate_mako_timestamp(timestamp));
  EXPECT_EQ(timestamp, 0U);

  // This test owns the clock exclusively; restore the suite's real progress.
  clock.store(saved, std::memory_order_release);
}

TEST(MakoLocalAbiIdentity, VersionWordContentionUsesAtomicLockTransitions) {
  using Version = TransactionTid::type;
  constexpr int kIterations = 10000;
  alignas(std::atomic_ref<Version>::required_alignment) Version version =
      TransactionTid::increment_value;
  std::atomic<bool> invalid_transition{false};
  std::atomic<bool> timed_out{false};
  std::barrier ready(3);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(30);

  auto contend = [&](int worker_id) {
    ready.arrive_and_wait();
    for (int i = 0; i != kIterations; ++i) {
      while (!TransactionTid::try_lock(version, worker_id)) {
        if (std::chrono::steady_clock::now() >= deadline) {
          timed_out.store(true, std::memory_order_relaxed);
          return;
        }
        std::this_thread::yield();
      }
      const Version locked = TransactionTid::atomic_load(
          version, std::memory_order_relaxed);
      if (!TransactionTid::is_locked_here(locked, worker_id))
        invalid_transition.store(true, std::memory_order_relaxed);
      const Version next =
          TransactionTid::unlocked(locked) + TransactionTid::increment_value |
          TransactionTid::nonopaque_bit;
      TransactionTid::set_version_unlock(version, next, worker_id);
    }
  };

  std::thread first(contend, 1);
  std::thread second(contend, 2);
  ready.arrive_and_wait();
  first.join();
  second.join();

  ASSERT_FALSE(timed_out.load(std::memory_order_relaxed));
  EXPECT_FALSE(invalid_transition.load(std::memory_order_relaxed));
  EXPECT_EQ(TransactionTid::atomic_load(version),
            Version(1 + 2 * kIterations) * TransactionTid::increment_value |
                TransactionTid::nonopaque_bit);
}

TEST_F(LocalAbiTest, MultiKeyMultiTableCommitIsVisibleTogether) {
  mako_local_table *secondary = nullptr;
  open_table("secondary", &secondary);

  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "a", "one"), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, "b", "two"), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, secondary, "c", "three"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "a"),
            (std::pair<int, std::optional<std::string>>{
                MAKO_LOCAL_OK, std::string("one")}));
  EXPECT_EQ(get(txn, primary, "b"),
            (std::pair<int, std::optional<std::string>>{
                MAKO_LOCAL_OK, std::string("two")}));
  EXPECT_EQ(get(txn, secondary, "c"),
            (std::pair<int, std::optional<std::string>>{
                MAKO_LOCAL_OK, std::string("three")}));
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, TrustedRustFastPathBindsPacksAndConsumesTransaction) {
  mako_local_table *secondary = nullptr;
  open_table("fast-secondary", &secondary);

  mako_local_txn *txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  ASSERT_NE(txn, nullptr);
  txn_for_cleanup = txn;

  const uint64_t created = fast_put(txn, "fast-primary", "first");
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(created), MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_PUT_CREATED(created), 1U);
  EXPECT_EQ(MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(created),
            26U + 17U + std::string("fast-primary").size() +
                std::string("first").size());
  ASSERT_EQ(put(txn, secondary, "safe-secondary", "mixed"), MAKO_LOCAL_OK);

  const uint64_t committed = mako_rust_fast_txn_commit_and_destroy(txn);
  txn_for_cleanup = nullptr;
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(committed), MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(committed), MAKO_LOCAL_OK);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "fast-primary").second,
            std::optional<std::string>("first"));
  EXPECT_EQ(get(txn, secondary, "safe-secondary").second,
            std::optional<std::string>("mixed"));
  commit_and_destroy(txn);

  HookObservation hook;
  txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  const uint64_t updated = fast_put(txn, "fast-primary", "second");
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(updated), MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_PUT_CREATED(updated), 0U);
  const uint64_t hooked =
      mako_rust_fast_txn_commit_with_hook_and_destroy(txn, accept_hook, &hook);
  txn_for_cleanup = nullptr;
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(hooked), MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(hooked), MAKO_LOCAL_OK);
  EXPECT_EQ(hook.calls, 1);
  EXPECT_NE(hook.timestamp, 0U);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "fast-primary").second,
            std::optional<std::string>("second"));
  commit_and_destroy(txn);

  txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  ASSERT_EQ(
      MAKO_RUST_FAST_PUT_STATUS(fast_put(txn, "fast-aborted", "invisible")),
      MAKO_LOCAL_OK);
  const uint64_t aborted = mako_rust_fast_txn_abort_and_destroy(txn);
  txn_for_cleanup = nullptr;
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(aborted), MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(aborted), MAKO_LOCAL_OK);

  txn = begin();
  EXPECT_FALSE(get(txn, primary, "fast-aborted").second.has_value());
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, TrustedThinRecordSerializesCanonicalV3WriteSet) {
  auto *seed = begin();
  ASSERT_EQ(put(seed, primary, "update", "old"), MAKO_LOCAL_OK);
  ASSERT_EQ(put(seed, primary, "delete", "old"), MAKO_LOCAL_OK);
  commit_and_destroy(seed);

  const std::string binary_key("\0key\xff", 5);
  const std::string binary_value("new\0\xff", 5);
  mako_local_txn *txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(fast_put(txn, binary_key, "")),
            MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, "update", binary_value), MAKO_LOCAL_OK);
  ASSERT_EQ(remove(txn, primary, "delete"), MAKO_LOCAL_OK);

  size_t exact_bytes = 0;
  uint32_t operation_count = 0;
  ASSERT_EQ(mako_rust_fast_txn_record_preflight(
                txn, 1 << 20, &exact_bytes, &operation_count),
            MAKO_LOCAL_OK);
  EXPECT_EQ(operation_count, 3U);
  EXPECT_EQ(exact_bytes,
            30U + (17U + binary_key.size()) +
                (17U + std::string("update").size() + binary_value.size()) +
                (17U + std::string("delete").size()));
  EXPECT_EQ(get(txn, primary, "update").first, MAKO_LOCAL_BUSY)
      << "preflight seals the final transaction set";

  std::vector<uint8_t> storage(exact_bytes, 0xa5);
  ThinRecordBinding binding{&storage, UINT64_C(0x0102030405060708)};
  uint8_t record_written = 99;
  const uint64_t result = mako_rust_fast_txn_commit_record_and_destroy(
      txn, bind_thin_record, &binding, &record_written);
  txn_for_cleanup = nullptr;
  ASSERT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(result), MAKO_LOCAL_OK);
  ASSERT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(result), MAKO_LOCAL_OK);
  ASSERT_EQ(record_written, 1U);
  ASSERT_EQ(binding.calls, 1);
  ASSERT_EQ(binding.exact_bytes, exact_bytes);
  ASSERT_NE(binding.timestamp, 0U);

  DecodedThinRecord decoded;
  ASSERT_TRUE(decode_thin_record(storage, &decoded));
  EXPECT_EQ(decoded.sequence, binding.sequence);
  EXPECT_EQ(decoded.timestamp, binding.timestamp);
  ASSERT_EQ(decoded.mutations.size(), operation_count);
  auto find_mutation = [&](const std::string &key)
      -> const DecodedThinMutation * {
    auto found = std::find_if(
        decoded.mutations.begin(), decoded.mutations.end(),
        [&](const DecodedThinMutation &mutation) {
          return mutation.key == key;
        });
    return found == decoded.mutations.end() ? nullptr : &*found;
  };
  const uint64_t table_id = mako_local_table_id(primary);
  const auto *inserted = find_mutation(binary_key);
  ASSERT_NE(inserted, nullptr);
  EXPECT_EQ(inserted->tag, 1U);
  EXPECT_EQ(inserted->table_id, table_id);
  EXPECT_TRUE(inserted->value.empty());
  const auto *updated = find_mutation("update");
  ASSERT_NE(updated, nullptr);
  EXPECT_EQ(updated->tag, 1U);
  EXPECT_EQ(updated->table_id, table_id);
  EXPECT_EQ(updated->value, binary_value);
  const auto *removed = find_mutation("delete");
  ASSERT_NE(removed, nullptr);
  EXPECT_EQ(removed->tag, 2U);
  EXPECT_EQ(removed->table_id, table_id);
  EXPECT_TRUE(removed->value.empty());

  txn = begin();
  EXPECT_EQ(get(txn, primary, binary_key).second,
            std::optional<std::string>(""));
  EXPECT_EQ(get(txn, primary, "update").second,
            std::optional<std::string>(binary_value));
  EXPECT_FALSE(get(txn, primary, "delete").second.has_value());
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, TrustedThinRecordCanExplicitlySkipCrcInV4) {
  mako_local_txn *txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(fast_put(txn, "unchecked", "payload")),
            MAKO_LOCAL_OK);

  size_t exact_bytes = 99;
  uint32_t operation_count = 99;
  EXPECT_EQ(mako_rust_fast_txn_record_preflight_with_checksum(
                txn, 1 << 20, 99, &exact_bytes, &operation_count),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(exact_bytes, 0U);
  EXPECT_EQ(operation_count, 0U);

  ASSERT_EQ(mako_rust_fast_txn_record_preflight_with_checksum(
                txn, 1 << 20, MAKO_RUST_FAST_RECORD_CHECKSUM_NONE,
                &exact_bytes, &operation_count),
            MAKO_LOCAL_OK);
  ASSERT_EQ(operation_count, 1U);
  EXPECT_EQ(exact_bytes, 26U + 17U + std::string("unchecked").size() +
                             std::string("payload").size());

  std::vector<uint8_t> storage(exact_bytes, 0xa5);
  ThinRecordBinding binding{&storage, 405};
  uint8_t written = 0;
  const uint64_t result = mako_rust_fast_txn_commit_record_and_destroy(
      txn, bind_thin_record, &binding, &written);
  txn_for_cleanup = nullptr;
  ASSERT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(result), MAKO_LOCAL_OK);
  ASSERT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(result), MAKO_LOCAL_OK);
  ASSERT_EQ(written, 1U);
  ASSERT_GE(storage.size(), 10U);
  EXPECT_EQ(storage[8], 0U);
  EXPECT_EQ(storage[9], 4U);

  DecodedThinRecord decoded;
  ASSERT_TRUE(decode_thin_record(storage, &decoded));
  EXPECT_EQ(decoded.sequence, binding.sequence);
  EXPECT_EQ(decoded.timestamp, binding.timestamp);
  ASSERT_EQ(decoded.mutations.size(), 1U);
  EXPECT_EQ(decoded.mutations[0].key, "unchecked");
  EXPECT_EQ(decoded.mutations[0].value, "payload");
}

TEST_F(LocalAbiTest, TrustedUncheckedOnePutFusesV4PreflightAndCommit) {
  const std::string key("fused\0key\xff", 10);
  const std::string value("direct\0payload\xff", 15);
  mako_local_txn *txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  const uint64_t put_result = fast_put(txn, key, value);
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(put_result), MAKO_LOCAL_OK);
  const uint32_t exact_bytes =
      MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(put_result);
  ASSERT_EQ(exact_bytes, 26U + 17U + key.size() + value.size());

  std::vector<uint8_t> storage(exact_bytes, 0xa5);
  ThinRecordBinding binding{&storage, 601};
  uint8_t written = 99;
  const uint64_t commit =
      mako_rust_fast_txn_commit_unchecked_one_put_record_and_destroy(
          txn, exact_bytes, bind_thin_record, &binding, &written);
  txn_for_cleanup = nullptr;
  ASSERT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(commit), MAKO_LOCAL_OK);
  ASSERT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(commit), MAKO_LOCAL_OK);
  ASSERT_EQ(binding.calls, 1);
  ASSERT_EQ(binding.exact_bytes, exact_bytes);
  ASSERT_EQ(written, 1U);

  DecodedThinRecord decoded;
  ASSERT_TRUE(decode_thin_record(storage, &decoded));
  EXPECT_EQ(decoded.sequence, binding.sequence);
  EXPECT_EQ(decoded.timestamp, binding.timestamp);
  ASSERT_EQ(decoded.mutations.size(), 1U);
  EXPECT_EQ(decoded.mutations[0].tag, 1U);
  EXPECT_EQ(decoded.mutations[0].table_id, mako_local_table_id(primary));
  EXPECT_EQ(decoded.mutations[0].key, key);
  EXPECT_EQ(decoded.mutations[0].value, value);

  auto *verify = begin();
  EXPECT_EQ(get(verify, primary, key).second,
            std::optional<std::string>(value));
  commit_and_destroy(verify);
}

TEST_F(LocalAbiTest, NativeOrderedOnePutAssignsBeforePostGateBinding) {
  const std::string key = "native-ordered-existing";
  auto *seed = begin();
  ASSERT_EQ(put(seed, primary, key, "old"), MAKO_LOCAL_OK);
  commit_and_destroy(seed);

  mako_local_txn *txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  const uint64_t put_result = fast_put(txn, key, "new");
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(put_result), MAKO_LOCAL_OK);
  const uint32_t exact_bytes =
      MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(put_result);
  ASSERT_NE(exact_bytes, 0U);

  alignas(uint64_t) uint64_t next_bound = 700;
  const uint8_t unhealthy = 0;
  std::vector<uint8_t> storage(exact_bytes, 0xa5);
  ThinRecordBinding binding{&storage};
  uint64_t ordered_sequence = UINT64_MAX;
  uint32_t ordered_timestamp = UINT32_MAX;
  uint8_t written = 99;
  const uint64_t commit =
      mako_rust_fast_txn_commit_native_ordered_unchecked_one_put_record_and_destroy(
          txn, exact_bytes, &next_bound, &unhealthy,
          bind_native_ordered_thin_record, &binding, &ordered_sequence,
          &ordered_timestamp, &written);
  txn_for_cleanup = nullptr;

  ASSERT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(commit), MAKO_LOCAL_OK);
  ASSERT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(commit), MAKO_LOCAL_OK);
  EXPECT_EQ(next_bound, 701U);
  EXPECT_EQ(ordered_sequence, 701U);
  EXPECT_NE(ordered_timestamp, 0U);
  EXPECT_EQ(binding.sequence, ordered_sequence);
  EXPECT_EQ(binding.timestamp, ordered_timestamp);
  EXPECT_EQ(binding.calls, 1);
  EXPECT_EQ(written, 1U);
  DecodedThinRecord decoded;
  ASSERT_TRUE(decode_thin_record(storage, &decoded));
  EXPECT_EQ(decoded.sequence, ordered_sequence);
  EXPECT_EQ(decoded.timestamp, ordered_timestamp);
  ASSERT_EQ(decoded.mutations.size(), 1U);
  EXPECT_EQ(decoded.mutations[0].key, key);
  EXPECT_EQ(decoded.mutations[0].value, "new");

  txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  const uint64_t rejected_put = fast_put(txn, key, "rejected");
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(rejected_put), MAKO_LOCAL_OK);
  const uint32_t rejected_bytes =
      MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(rejected_put);
  std::vector<uint8_t> rejected_storage(rejected_bytes, 0xa5);
  ThinRecordBinding rejected{&rejected_storage};
  rejected.accept = false;
  ordered_sequence = UINT64_MAX;
  ordered_timestamp = UINT32_MAX;
  written = 99;
  const uint64_t rejected_commit =
      mako_rust_fast_txn_commit_native_ordered_unchecked_one_put_record_and_destroy(
          txn, rejected_bytes, &next_bound, &unhealthy,
          bind_native_ordered_thin_record, &rejected, &ordered_sequence,
          &ordered_timestamp, &written);
  txn_for_cleanup = nullptr;
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(rejected_commit),
            MAKO_LOCAL_COMMIT_HOOK_REJECTED);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(rejected_commit), MAKO_LOCAL_OK);
  EXPECT_EQ(next_bound, 702U);
  EXPECT_EQ(ordered_sequence, 702U);
  EXPECT_NE(ordered_timestamp, 0U);
  EXPECT_EQ(rejected.sequence, ordered_sequence);
  EXPECT_EQ(rejected.calls, 1);
  EXPECT_EQ(written, 0U);

  auto *verify = begin();
  EXPECT_EQ(get(verify, primary, key).second,
            std::optional<std::string>("new"));
  commit_and_destroy(verify);
}

TEST_F(LocalAbiTest, NativeOrderedArenaBindsAndSerializesWithoutCallback) {
  const std::string key = "native-ordered-arena";
  const std::string value = "direct-record";
  mako_local_txn *txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  const uint64_t put_result = fast_put(txn, key, value);
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(put_result), MAKO_LOCAL_OK);
  const uint32_t exact_bytes =
      MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(put_result);
  ASSERT_NE(exact_bytes, 0U);
  ASSERT_LE(exact_bytes, 256U);

  alignas(uint64_t) uint64_t next_bound = 900;
  const uint8_t unhealthy = 0;
  std::array<TestRustPublicationCell, 4> publications{};
  std::array<TestRustArenaBlock, 4> arena{};
  constexpr uint64_t sequence = 901;
  const size_t index = static_cast<size_t>(sequence) & 3;
  const uint64_t free_turn = (sequence >> 2) << 2;
  __atomic_store_n(&publications[index].turn, free_turn, __ATOMIC_RELAXED);
  auto control = make_native_arena_control(
      &next_bound, &unhealthy, publications.data(), arena.data());

  const mako_rust_fast_native_ordered_arena_result commit =
      mako_rust_fast_txn_commit_native_ordered_unchecked_one_put_arena_and_destroy(
          txn, exact_bytes, &control);
  txn_for_cleanup = nullptr;
  ASSERT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(commit.terminal), MAKO_LOCAL_OK);
  ASSERT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(commit.terminal), MAKO_LOCAL_OK);
  EXPECT_EQ(next_bound, sequence);
  EXPECT_EQ(commit.ordered_sequence, sequence);
  EXPECT_NE(MAKO_RUST_FAST_NATIVE_ORDERED_ARENA_TIMESTAMP(commit), 0U);
  EXPECT_EQ(MAKO_RUST_FAST_NATIVE_ORDERED_ARENA_WRITTEN(commit), 1U);
  EXPECT_EQ(MAKO_RUST_FAST_NATIVE_ORDERED_ARENA_RESERVED(commit), 0U);
  EXPECT_EQ(__atomic_load_n(&publications[index].turn, __ATOMIC_ACQUIRE),
            free_turn | UINT64_C(1));
  EXPECT_EQ(publications[index].record_bytes, 0U);

  const std::vector<uint8_t> storage(
      arena[index].bytes.begin(), arena[index].bytes.begin() + exact_bytes);
  DecodedThinRecord decoded;
  ASSERT_TRUE(decode_thin_record(storage, &decoded));
  EXPECT_EQ(decoded.sequence, sequence);
  EXPECT_EQ(decoded.timestamp,
            MAKO_RUST_FAST_NATIVE_ORDERED_ARENA_TIMESTAMP(commit));
  ASSERT_EQ(decoded.mutations.size(), 1U);
  EXPECT_EQ(decoded.mutations[0].key, key);
  EXPECT_EQ(decoded.mutations[0].value, value);

  auto *verify = begin();
  EXPECT_EQ(get(verify, primary, key).second,
            std::optional<std::string>(value));
  commit_and_destroy(verify);
}

TEST_F(LocalAbiTest, NativeOrderedArenaRejectsLayoutBeforeAssigningOrder) {
  const std::string key = "native-arena-invalid-layout";
  mako_local_txn *txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  const uint64_t put_result = fast_put(txn, key, "must-not-install");
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(put_result), MAKO_LOCAL_OK);
  const uint32_t exact_bytes =
      MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(put_result);
  ASSERT_NE(exact_bytes, 0U);

  alignas(uint64_t) uint64_t next_bound = 920;
  const uint8_t unhealthy = 0;
  std::array<TestRustPublicationCell, 4> publications{};
  std::array<TestRustArenaBlock, 4> arena{};
  auto control = make_native_arena_control(
      &next_bound, &unhealthy, publications.data(), arena.data());
  control.publication_stride = 63;
  const mako_rust_fast_native_ordered_arena_result commit =
      mako_rust_fast_txn_commit_native_ordered_unchecked_one_put_arena_and_destroy(
          txn, exact_bytes, &control);
  txn_for_cleanup = nullptr;
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(commit.terminal),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(commit.terminal), MAKO_LOCAL_OK);
  EXPECT_EQ(commit.ordered_sequence, 0U);
  EXPECT_EQ(commit.record_state, 0U);
  EXPECT_EQ(next_bound, 920U);

  auto *verify = begin();
  EXPECT_FALSE(get(verify, primary, key).second.has_value());
  commit_and_destroy(verify);
}

#if defined(MAKO_LOCAL_TEST_HOOKS)
TEST_F(LocalAbiTest, TrustedOnePutLateGateRequiresWriteCoveredValidation) {
  auto *seed = begin();
  ASSERT_EQ(put(seed, primary, "late-existing", "old"), MAKO_LOCAL_OK);
  ASSERT_EQ(put(seed, primary, "late-other", "observed"), MAKO_LOCAL_OK);
  commit_and_destroy(seed);

  auto abort_fast = [&](mako_local_txn *txn) {
    const uint64_t aborted = mako_rust_fast_txn_abort_and_destroy(txn);
    txn_for_cleanup = nullptr;
    EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(aborted), MAKO_LOCAL_OK);
    EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(aborted), MAKO_LOCAL_OK);
  };

  mako_local_txn *txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(
                fast_put(txn, "late-existing", "pure-update")),
            MAKO_LOCAL_OK);
  EXPECT_EQ(
      mako_rust_fast_test_txn_can_order_record_after_validation(txn), 1U);
  abort_fast(txn);

  txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  ASSERT_EQ(get(txn, primary, "late-existing").first, MAKO_LOCAL_OK);
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(
                fast_put(txn, "late-existing", "same-key-read")),
            MAKO_LOCAL_OK);
#if READ_MY_WRITES
  EXPECT_EQ(
      mako_rust_fast_test_txn_can_order_record_after_validation(txn), 1U);
#else
  EXPECT_EQ(
      mako_rust_fast_test_txn_can_order_record_after_validation(txn), 0U);
#endif
  abort_fast(txn);

  txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  ASSERT_EQ(get(txn, primary, "late-other").first, MAKO_LOCAL_OK);
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(
                fast_put(txn, "late-existing", "external-read")),
            MAKO_LOCAL_OK);
  EXPECT_EQ(
      mako_rust_fast_test_txn_can_order_record_after_validation(txn), 0U);
  abort_fast(txn);

  txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(
                fast_put(txn, "late-absent", "insert")),
            MAKO_LOCAL_OK);
  EXPECT_EQ(
      mako_rust_fast_test_txn_can_order_record_after_validation(txn), 0U);
  abort_fast(txn);
}

TEST_F(LocalAbiTest, TrustedOnePutValidationAbortConsumesNoLateGateTicket) {
  auto *seed = begin();
  ASSERT_EQ(put(seed, primary, "late-conflict", "old"), MAKO_LOCAL_OK);
  commit_and_destroy(seed);

  mako_local_txn *loser = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &loser), MAKO_LOCAL_OK);
  txn_for_cleanup = loser;
  const uint64_t loser_put =
      fast_put(loser, "late-conflict", "must-not-install");
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(loser_put), MAKO_LOCAL_OK);
  const uint32_t exact_bytes =
      MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(loser_put);
  ASSERT_NE(exact_bytes, 0U);
  ASSERT_EQ(
      mako_rust_fast_test_txn_can_order_record_after_validation(loser), 1U);

  struct WinnerResult {
    int attach = MAKO_LOCAL_INTERNAL;
    int begin = MAKO_LOCAL_INTERNAL;
    int put = MAKO_LOCAL_INTERNAL;
    int commit = MAKO_LOCAL_INTERNAL;
    int destroy = MAKO_LOCAL_INTERNAL;
  } winner;
  std::thread winning_worker([&] {
    winner.attach = mako_local_thread_attach();
    mako_local_txn *txn = nullptr;
    if (winner.attach == MAKO_LOCAL_OK)
      winner.begin = mako_local_txn_begin(db, &txn);
    if (winner.begin == MAKO_LOCAL_OK)
      winner.put = put(txn, primary, "late-conflict", "winner");
    if (winner.put == MAKO_LOCAL_OK)
      winner.commit = mako_local_txn_commit(txn);
    if (txn != nullptr)
      winner.destroy = mako_local_txn_destroy(txn);
  });
  winning_worker.join();
  ASSERT_EQ(winner.attach, MAKO_LOCAL_OK);
  ASSERT_EQ(winner.begin, MAKO_LOCAL_OK);
  ASSERT_EQ(winner.put, MAKO_LOCAL_OK);
  ASSERT_EQ(winner.commit, MAKO_LOCAL_OK);
  ASSERT_EQ(winner.destroy, MAKO_LOCAL_OK);

  const uint64_t tickets_before =
      mako_rust_fast_test_record_validation_tickets(db);
  std::vector<uint8_t> storage(exact_bytes, 0xa5);
  const std::vector<uint8_t> untouched = storage;
  ThinRecordBinding binding{&storage, 901};
  uint8_t written = 99;
  const uint64_t result =
      mako_rust_fast_txn_commit_unchecked_one_put_record_and_destroy(
          loser, exact_bytes, bind_thin_record, &binding, &written);
  txn_for_cleanup = nullptr;

  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(result), MAKO_LOCAL_CONFLICT);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(result), MAKO_LOCAL_OK);
  EXPECT_EQ(binding.calls, 0);
  EXPECT_EQ(written, 0U);
  EXPECT_EQ(storage, untouched);
  EXPECT_EQ(mako_rust_fast_test_record_validation_tickets(db),
            tickets_before);

  auto *verify = begin();
  EXPECT_EQ(get(verify, primary, "late-conflict").second,
            std::optional<std::string>("winner"));
  commit_and_destroy(verify);
}

TEST_F(LocalAbiTest, TrustedOnePutLateGateKeepsSameKeyRecordOrder) {
  const std::string key = "late-same-key-order";
  auto *seed = begin();
  ASSERT_EQ(put(seed, primary, key, "initial"), MAKO_LOCAL_OK);
  commit_and_destroy(seed);

  struct ExactUpdateResult {
    std::string value;
    int attach = MAKO_LOCAL_INTERNAL;
    int begin = MAKO_LOCAL_INTERNAL;
    int put_status = MAKO_LOCAL_INTERNAL;
    int retry_destroy = MAKO_LOCAL_OK;
    uint64_t terminal = UINT64_MAX;
    unsigned attempts = 0;
    unsigned conflicts = 0;
    bool conflict_bound_record = false;
    uint8_t written = 0;
    std::vector<uint8_t> storage;
    ThinRecordBinding binding;
    DecodedThinRecord decoded;
  } first{"first"}, second{"second"};
  std::atomic<uint64_t> next_sequence{31};
  const uint64_t tickets_before =
      mako_rust_fast_test_record_validation_tickets(db);
  std::barrier staged(3);

  auto commit_update = [&](ExactUpdateResult *result) {
    result->attach = mako_local_thread_attach();
    if (result->attach != MAKO_LOCAL_OK) {
      staged.arrive_and_wait();
      return;
    }
    bool initial_attempt = true;
    constexpr unsigned kMaxAttempts = 10000;
    while (result->attach == MAKO_LOCAL_OK &&
           result->attempts != kMaxAttempts) {
      ++result->attempts;
      mako_local_txn *txn = nullptr;
      result->begin = mako_rust_fast_txn_begin(db, primary, &txn);
      if (result->begin != MAKO_LOCAL_OK) {
        if (initial_attempt) staged.arrive_and_wait();
        return;
      }

      const uint64_t put_result = fast_put(txn, key, result->value);
      result->put_status = MAKO_RUST_FAST_PUT_STATUS(put_result);
      const uint32_t exact_bytes =
          MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(put_result);
      if (initial_attempt) {
        staged.arrive_and_wait();
        initial_attempt = false;
      }
      if (result->put_status == MAKO_LOCAL_CONFLICT) {
        result->retry_destroy = mako_local_txn_destroy(txn);
        if (result->retry_destroy != MAKO_LOCAL_OK) return;
        ++result->conflicts;
        std::this_thread::yield();
        continue;
      }
      if (result->put_status != MAKO_LOCAL_OK || exact_bytes == 0) {
        if (result->put_status == MAKO_LOCAL_OK)
          (void)mako_rust_fast_txn_abort_and_destroy(txn);
        return;
      }

      result->storage.assign(exact_bytes, 0xa5);
      result->binding.storage = &result->storage;
      result->binding.next_sequence = &next_sequence;
      result->written = 99;
      result->terminal =
          mako_rust_fast_txn_commit_unchecked_one_put_record_and_destroy(
              txn, exact_bytes, bind_thin_record, &result->binding,
              &result->written);
      const int terminal_status =
          MAKO_RUST_FAST_TERMINAL_STATUS(result->terminal);
      if (terminal_status == MAKO_LOCAL_OK) return;
      if (terminal_status != MAKO_LOCAL_CONFLICT) return;
      ++result->conflicts;
      if (result->binding.calls != 0 || result->written != 0)
        result->conflict_bound_record = true;
      std::this_thread::yield();
    }
  };

  std::thread first_worker(commit_update, &first);
  std::thread second_worker(commit_update, &second);
  staged.arrive_and_wait();
  first_worker.join();
  second_worker.join();

  for (ExactUpdateResult *result : {&first, &second}) {
    EXPECT_EQ(result->attach, MAKO_LOCAL_OK);
    EXPECT_EQ(result->begin, MAKO_LOCAL_OK);
    EXPECT_EQ(result->put_status, MAKO_LOCAL_OK);
    EXPECT_EQ(result->retry_destroy, MAKO_LOCAL_OK);
    ASSERT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(result->terminal),
              MAKO_LOCAL_OK);
    EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(result->terminal),
              MAKO_LOCAL_OK);
    EXPECT_FALSE(result->conflict_bound_record);
    EXPECT_EQ(result->binding.calls, 1);
    EXPECT_EQ(result->written, 1U);
    ASSERT_TRUE(decode_thin_record(result->storage, &result->decoded));
    ASSERT_EQ(result->decoded.mutations.size(), 1U);
    EXPECT_EQ(result->decoded.mutations[0].key, key);
    EXPECT_EQ(result->decoded.mutations[0].value, result->value);
  }
  EXPECT_GE(first.conflicts + second.conflicts, 1U);

  EXPECT_EQ(mako_rust_fast_test_record_validation_tickets(db),
            tickets_before + 2);
  std::array<ExactUpdateResult *, 2> ordered{&first, &second};
  std::sort(ordered.begin(), ordered.end(),
            [](const ExactUpdateResult *left,
               const ExactUpdateResult *right) {
              return left->binding.sequence < right->binding.sequence;
            });
  EXPECT_EQ(ordered[0]->binding.sequence, 31U);
  EXPECT_EQ(ordered[1]->binding.sequence, 32U);
  EXPECT_LT(ordered[0]->binding.timestamp,
            ordered[1]->binding.timestamp);
  for (const ExactUpdateResult *result : ordered) {
    EXPECT_EQ(result->decoded.sequence, result->binding.sequence);
    EXPECT_EQ(result->decoded.timestamp, result->binding.timestamp);
  }

  auto *verify = begin();
  EXPECT_EQ(get(verify, primary, key).second,
            std::optional<std::string>(ordered[1]->value));
  commit_and_destroy(verify);
}
#endif

TEST_F(LocalAbiTest,
       TrustedSingleProducerUncheckedOnePutSkipsOnlyValidationTicket) {
  const std::string key("single\0producer\xff", 16);
  const std::string value("direct\0record\xff", 14);
#if defined(MAKO_LOCAL_TEST_HOOKS)
  const uint64_t tickets_before =
      mako_rust_fast_test_record_validation_tickets(db);
  const uint64_t waits_before =
      mako_rust_fast_test_record_validation_wait_observations(db);
  ASSERT_EQ(mako_local_test_set_commit_observer(record_commit_phase,
                                                &commit_observation),
            MAKO_LOCAL_OK);
#endif

  mako_local_txn *txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  const uint64_t put_result = fast_put(txn, key, value);
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(put_result), MAKO_LOCAL_OK);
  const uint32_t exact_bytes =
      MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(put_result);
  ASSERT_EQ(exact_bytes, 26U + 17U + key.size() + value.size());

  std::vector<uint8_t> storage(exact_bytes, 0xa5);
  ThinRecordBinding binding{&storage, 701};
  uint8_t written = 99;
  const uint64_t commit =
      mako_rust_fast_txn_commit_unchecked_one_put_record_single_producer_and_destroy(
          txn, exact_bytes, bind_thin_record, &binding, &written);
  txn_for_cleanup = nullptr;
  ASSERT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(commit), MAKO_LOCAL_OK);
  ASSERT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(commit), MAKO_LOCAL_OK);
  ASSERT_EQ(binding.calls, 1);
  ASSERT_EQ(binding.exact_bytes, exact_bytes);
  ASSERT_EQ(written, 1U);

#if defined(MAKO_LOCAL_TEST_HOOKS)
  EXPECT_EQ(mako_rust_fast_test_record_validation_tickets(db),
            tickets_before);
  EXPECT_EQ(mako_rust_fast_test_record_validation_wait_observations(db),
            waits_before);
  // A one-write commit has no meaningful mid-install seam; STO reports the
  // terminal all-writes-installed phase directly after preinstall acceptance.
  constexpr std::array<uint32_t, 5> expected_phases{
      MAKO_LOCAL_TEST_COMMIT_WRITESET_LOCKED,
      MAKO_LOCAL_TEST_COMMIT_MAKO_TIMESTAMP_ALLOCATED,
      MAKO_LOCAL_TEST_COMMIT_LOCAL_VALIDATION_COMPLETE,
      MAKO_LOCAL_TEST_COMMIT_PREINSTALL_ACCEPTED,
      MAKO_LOCAL_TEST_COMMIT_ALL_WRITES_INSTALLED,
  };
  ASSERT_EQ(commit_observation.calls, expected_phases.size());
  for (size_t index = 0; index != expected_phases.size(); ++index)
    EXPECT_EQ(commit_observation.phases[index], expected_phases[index])
        << index;
  EXPECT_EQ(commit_observation.timestamps[0], 0U);
  for (size_t index = 1; index != expected_phases.size(); ++index)
    EXPECT_EQ(commit_observation.timestamps[index], binding.timestamp)
        << index;
  ASSERT_EQ(mako_local_test_clear_commit_observer(), MAKO_LOCAL_OK);
#endif

  DecodedThinRecord decoded;
  ASSERT_TRUE(decode_thin_record(storage, &decoded));
  EXPECT_EQ(decoded.sequence, binding.sequence);
  EXPECT_EQ(decoded.timestamp, binding.timestamp);
  ASSERT_EQ(decoded.mutations.size(), 1U);
  EXPECT_EQ(decoded.mutations[0].tag, 1U);
  EXPECT_EQ(decoded.mutations[0].table_id, mako_local_table_id(primary));
  EXPECT_EQ(decoded.mutations[0].key, key);
  EXPECT_EQ(decoded.mutations[0].value, value);

  auto *verify = begin();
  EXPECT_EQ(get(verify, primary, key).second,
            std::optional<std::string>(value));
  commit_and_destroy(verify);
}

TEST_F(LocalAbiTest,
       TrustedPreselectedSingleProducerSerializesBeforeInstall) {
  const std::string key("preselected\0key\xff", 16);
  const std::string value("direct\0target\xff", 14);
  constexpr uint64_t sequence = 801;

  mako_local_txn *txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  const uint64_t put_result = fast_put(txn, key, value);
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(put_result), MAKO_LOCAL_OK);
  const uint32_t exact_bytes =
      MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(put_result);
  ASSERT_EQ(exact_bytes, 26U + 17U + key.size() + value.size());

  std::vector<uint8_t> storage(exact_bytes, 0xa5);
#if defined(MAKO_LOCAL_TEST_HOOKS)
  const uint64_t tickets_before =
      mako_rust_fast_test_record_validation_tickets(db);
  const uint64_t waits_before =
      mako_rust_fast_test_record_validation_wait_observations(db);
  PreselectedCommitObservation observation;
  observation.record = storage.data();
  observation.record_bytes = storage.size();
  observation.sequence = sequence;
  observation.table_id = mako_local_table_id(primary);
  observation.key = &key;
  observation.value = &value;
  ASSERT_EQ(mako_local_test_set_commit_observer(
                observe_preselected_commit, &observation),
            MAKO_LOCAL_OK);
#endif

  const mako_rust_fast_preselected_record_result result =
      mako_rust_fast_txn_commit_preselected_unchecked_one_put_record_single_producer_and_destroy(
          txn, exact_bytes, sequence, storage.data(), storage.size());
  txn_for_cleanup = nullptr;
  ASSERT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(result.terminal),
            MAKO_LOCAL_OK);
  ASSERT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(result.terminal),
            MAKO_LOCAL_OK);
  const uint32_t timestamp =
      MAKO_RUST_FAST_PRESELECTED_RECORD_TIMESTAMP(result);
  ASSERT_NE(timestamp, 0U);
  ASSERT_LE(timestamp, MAKO_LOCAL_MAX_MAKO_TIMESTAMP);
  EXPECT_EQ(MAKO_RUST_FAST_PRESELECTED_RECORD_WRITTEN(result), 1U);
  EXPECT_EQ(MAKO_RUST_FAST_PRESELECTED_RECORD_RESERVED(result), 0U);

#if defined(MAKO_LOCAL_TEST_HOOKS)
  EXPECT_EQ(mako_rust_fast_test_record_validation_tickets(db),
            tickets_before);
  EXPECT_EQ(mako_rust_fast_test_record_validation_wait_observations(db),
            waits_before);
  constexpr std::array<uint32_t, 5> expected_phases{
      MAKO_LOCAL_TEST_COMMIT_WRITESET_LOCKED,
      MAKO_LOCAL_TEST_COMMIT_MAKO_TIMESTAMP_ALLOCATED,
      MAKO_LOCAL_TEST_COMMIT_LOCAL_VALIDATION_COMPLETE,
      MAKO_LOCAL_TEST_COMMIT_PREINSTALL_ACCEPTED,
      MAKO_LOCAL_TEST_COMMIT_ALL_WRITES_INSTALLED,
  };
  ASSERT_EQ(observation.phases.calls, expected_phases.size());
  for (size_t index = 0; index != expected_phases.size(); ++index) {
    EXPECT_EQ(observation.phases.phases[index], expected_phases[index])
        << index;
    EXPECT_EQ(observation.phases.timestamps[index],
              index == 0 ? 0U : timestamp)
        << index;
  }
  EXPECT_TRUE(observation.complete_at_preinstall);
  ASSERT_EQ(mako_local_test_clear_commit_observer(), MAKO_LOCAL_OK);
#endif

  DecodedThinRecord decoded;
  ASSERT_TRUE(decode_thin_record(storage, &decoded));
  EXPECT_EQ(decoded.sequence, sequence);
  EXPECT_EQ(decoded.timestamp, timestamp);
  ASSERT_EQ(decoded.mutations.size(), 1U);
  EXPECT_EQ(decoded.mutations[0].tag, 1U);
  EXPECT_EQ(decoded.mutations[0].table_id, mako_local_table_id(primary));
  EXPECT_EQ(decoded.mutations[0].key, key);
  EXPECT_EQ(decoded.mutations[0].value, value);

  auto *verify = begin();
  EXPECT_EQ(get(verify, primary, key).second,
            std::optional<std::string>(value));
  commit_and_destroy(verify);
}

TEST_F(LocalAbiTest, OnePutHolderPoolValidatesConfigurationAndLayout) {
  mako_rust_fast_one_put_holder_pool *pool =
      reinterpret_cast<mako_rust_fast_one_put_holder_pool *>(uintptr_t{1});
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_create(0, 0, 0, &pool),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(pool, nullptr);
  pool = reinterpret_cast<mako_rust_fast_one_put_holder_pool *>(uintptr_t{1});
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_create(3, 0, 0, &pool),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(pool, nullptr);
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_create(4, 0, 0, nullptr),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_create(
                4, UINT32_MAX, 0, &pool),
            MAKO_LOCAL_VALUE_TOO_LARGE);
  EXPECT_EQ(pool, nullptr);
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_create(
                4, 0, UINT32_MAX, &pool),
            MAKO_LOCAL_VALUE_TOO_LARGE);
  EXPECT_EQ(pool, nullptr);
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_destroy(nullptr),
            MAKO_LOCAL_OK);

  ASSERT_EQ(mako_rust_fast_one_put_holder_pool_create(4, 64, 256, &pool),
            MAKO_LOCAL_OK);
  ASSERT_NE(pool, nullptr);
  mako_rust_fast_one_put_holder_view view{
      1, 2, reinterpret_cast<const uint8_t *>(uintptr_t{3}),
      reinterpret_cast<const uint8_t *>(uintptr_t{4}), 5, 6, 7, 8};
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_get_view(pool, 1, &view),
            MAKO_LOCAL_BUSY);
  EXPECT_EQ(view.sequence, 0U);
  EXPECT_EQ(view.key, nullptr);
  EXPECT_EQ(view.value, nullptr);
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_get_view(pool, 0, &view),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_get_view(pool, 1, nullptr),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_release(pool, 0),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_release(pool, 1),
            MAKO_LOCAL_BUSY);
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_destroy(pool), MAKO_LOCAL_OK);
}

TEST_F(LocalAbiTest, FusedOnePutHolderLeavesCandidateAndSizeMissesUntouched) {
  create_holder_pool(4);
  alignas(8) uint64_t acknowledged = 5;
  alignas(8) uint64_t producer_next = 5;
  alignas(8) uint64_t capacity_limit = 8;
  uint8_t unhealthy = 0;
  mako_rust_fast_spsc_holder_control control = make_fused_holder_control(
      holder_pool, &acknowledged, &unhealthy, 4);
  constexpr mako_rust_fast_preselected_record_result kColdSentinel{
      UINT64_MAX, UINT64_MAX - 1};
  control.cold_out = kColdSentinel;
  auto &cold = control.cold_out;

  mako_local_txn *txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  uint64_t fused =
      mako_rust_fast_txn_try_commit_fused_one_put_holder_single_producer_and_destroy(
          txn, &acknowledged, &unhealthy, &control, capacity_limit);
  EXPECT_EQ(MAKO_RUST_FAST_FUSED_HOLDER_CODE(fused),
            MAKO_RUST_FAST_FUSED_HOLDER_UNTOUCHED_NEED_GENERAL);
  EXPECT_EQ(MAKO_RUST_FAST_FUSED_HOLDER_PAYLOAD(fused), 0U);
  EXPECT_EQ(producer_next, 5U);
  EXPECT_EQ(acknowledged, 5U);
  EXPECT_EQ(cold.terminal, kColdSentinel.terminal);
  EXPECT_EQ(cold.record_state, kColdSentinel.record_state);
  const uint64_t abort_empty = mako_rust_fast_txn_abort_and_destroy(txn);
  txn_for_cleanup = nullptr;
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(abort_empty), MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(abort_empty), MAKO_LOCAL_OK);

  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  const uint64_t put_result = fast_put(txn, "fused-size", "candidate");
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(put_result), MAKO_LOCAL_OK);
  const uint32_t exact_bytes =
      MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(put_result);
  ASSERT_GT(exact_bytes, 1U);
  control.max_record_bytes = exact_bytes - 1;
  fused =
      mako_rust_fast_txn_try_commit_fused_one_put_holder_single_producer_and_destroy(
          txn, &acknowledged, &unhealthy, &control, capacity_limit);
  EXPECT_EQ(MAKO_RUST_FAST_FUSED_HOLDER_CODE(fused),
            MAKO_RUST_FAST_FUSED_HOLDER_UNTOUCHED_NEED_GENERAL);
  EXPECT_EQ(MAKO_RUST_FAST_FUSED_HOLDER_PAYLOAD(fused), 0U);
  EXPECT_EQ(producer_next, 5U);
  EXPECT_EQ(acknowledged, 5U);
  EXPECT_EQ(cold.terminal, kColdSentinel.terminal);
  EXPECT_EQ(cold.record_state, kColdSentinel.record_state);
  const uint64_t abort_oversized = mako_rust_fast_txn_abort_and_destroy(txn);
  txn_for_cleanup = nullptr;
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(abort_oversized), MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(abort_oversized), MAKO_LOCAL_OK);

  mako_rust_fast_one_put_holder_view view{};
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_get_view(holder_pool, 6, &view),
            MAKO_LOCAL_BUSY);
}

TEST_F(LocalAbiTest, FusedOnePutHolderSlowMissesRetainTxnAndExactCandidate) {
  create_holder_pool(4);
  alignas(8) uint64_t acknowledged = 7;
  alignas(8) uint64_t producer_next = 7;
  alignas(8) uint64_t capacity_limit = 7;
  uint8_t unhealthy = 0;
  mako_rust_fast_spsc_holder_control control = make_fused_holder_control(
      holder_pool, &acknowledged, &unhealthy, 2);
  constexpr mako_rust_fast_preselected_record_result kColdSentinel{
      UINT64_MAX - 2, UINT64_MAX - 3};
  control.cold_out = kColdSentinel;
  auto &cold = control.cold_out;

  mako_local_txn *txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  const uint64_t put_result = fast_put(txn, "fused-slow", "candidate");
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(put_result), MAKO_LOCAL_OK);
  const uint32_t exact_bytes =
      MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(put_result);
  ASSERT_NE(exact_bytes, 0U);

  uint64_t fused =
      mako_rust_fast_txn_try_commit_fused_one_put_holder_single_producer_and_destroy(
          txn, &acknowledged, &unhealthy, &control, capacity_limit);
  EXPECT_EQ(MAKO_RUST_FAST_FUSED_HOLDER_CODE(fused),
            MAKO_RUST_FAST_FUSED_HOLDER_UNTOUCHED_NEED_SLOW);
  EXPECT_EQ(MAKO_RUST_FAST_FUSED_HOLDER_PAYLOAD(fused), exact_bytes);
  EXPECT_EQ(producer_next, 7U);
  EXPECT_EQ(acknowledged, 7U);
  EXPECT_EQ(cold.terminal, kColdSentinel.terminal);
  EXPECT_EQ(cold.record_state, kColdSentinel.record_state);

  capacity_limit = 8;
  __atomic_store_n(&unhealthy, UINT8_C(1), __ATOMIC_RELEASE);
  fused =
      mako_rust_fast_txn_try_commit_fused_one_put_holder_single_producer_and_destroy(
          txn, &acknowledged, &unhealthy, &control, capacity_limit);
  EXPECT_EQ(MAKO_RUST_FAST_FUSED_HOLDER_CODE(fused),
            MAKO_RUST_FAST_FUSED_HOLDER_UNTOUCHED_NEED_SLOW);
  EXPECT_EQ(MAKO_RUST_FAST_FUSED_HOLDER_PAYLOAD(fused), exact_bytes);
  EXPECT_EQ(producer_next, 7U);
  EXPECT_EQ(acknowledged, 7U);
  EXPECT_EQ(cold.terminal, kColdSentinel.terminal);
  EXPECT_EQ(cold.record_state, kColdSentinel.record_state);

  // Both slow returns left txn and generation 8 untouched, so the same call
  // can succeed after Rust's refresh/health resolver clears the test barrier.
  __atomic_store_n(&unhealthy, UINT8_C(0), __ATOMIC_RELEASE);
  fused =
      mako_rust_fast_txn_try_commit_fused_one_put_holder_single_producer_and_destroy(
          txn, &acknowledged, &unhealthy, &control, capacity_limit);
  txn_for_cleanup = nullptr;
  EXPECT_EQ(MAKO_RUST_FAST_FUSED_HOLDER_CODE(fused),
            MAKO_RUST_FAST_FUSED_HOLDER_CONSUMED_PUBLISHED);
  EXPECT_EQ(MAKO_RUST_FAST_FUSED_HOLDER_PAYLOAD(fused), 0U);
  EXPECT_EQ(producer_next, 7U);
  EXPECT_EQ(acknowledged, 8U);
  EXPECT_EQ(cold.terminal, kColdSentinel.terminal);
  EXPECT_EQ(cold.record_state, kColdSentinel.record_state);

  mako_rust_fast_one_put_holder_view view{};
  ASSERT_EQ(mako_rust_fast_one_put_holder_pool_get_view(holder_pool, 8, &view),
            MAKO_LOCAL_OK);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(view.key), view.key_len),
            "fused-slow");
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_release(holder_pool, 8),
            MAKO_LOCAL_OK);
}

TEST_F(LocalAbiTest, FusedOnePutHolderPublishesOnlyAfterNativeInstallAndSeal) {
  create_holder_pool(4);
  alignas(8) uint64_t acknowledged = 0;
  alignas(8) uint64_t producer_next = 0;
  alignas(8) uint64_t capacity_limit = 4;
  uint8_t unhealthy = 0;
  mako_rust_fast_spsc_holder_control control = make_fused_holder_control(
      holder_pool, &acknowledged, &unhealthy, 4);
  constexpr mako_rust_fast_preselected_record_result kColdSentinel{
      UINT64_MAX - 4, UINT64_MAX - 5};
  control.cold_out = kColdSentinel;
  auto &cold = control.cold_out;
  FusedHolderCommitObservation observation{&producer_next, &acknowledged,
                                           &unhealthy};
#if defined(MAKO_LOCAL_TEST_HOOKS)
  ASSERT_EQ(mako_local_test_set_commit_observer(observe_fused_holder_commit,
                                                &observation),
            MAKO_LOCAL_OK);
#endif

  mako_local_txn *txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  const uint64_t put_result = fast_put(txn, "fused-publish", "visible");
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(put_result), MAKO_LOCAL_OK);
  const uint64_t fused =
      mako_rust_fast_txn_try_commit_fused_one_put_holder_single_producer_and_destroy(
          txn, &acknowledged, &unhealthy, &control, capacity_limit);
  txn_for_cleanup = nullptr;
#if defined(MAKO_LOCAL_TEST_HOOKS)
  ASSERT_EQ(mako_local_test_clear_commit_observer(), MAKO_LOCAL_OK);
#endif

  EXPECT_EQ(MAKO_RUST_FAST_FUSED_HOLDER_CODE(fused),
            MAKO_RUST_FAST_FUSED_HOLDER_CONSUMED_PUBLISHED);
  EXPECT_EQ(MAKO_RUST_FAST_FUSED_HOLDER_PAYLOAD(fused), 0U);
  EXPECT_EQ(producer_next, 0U);
  EXPECT_EQ(acknowledged, 1U);
  EXPECT_EQ(cold.terminal, kColdSentinel.terminal);
  EXPECT_EQ(cold.record_state, kColdSentinel.record_state);
#if defined(MAKO_LOCAL_TEST_HOOKS)
  EXPECT_EQ(observation.producer_at_install, 0U);
  EXPECT_EQ(observation.acknowledged_at_install, 0U);
  EXPECT_NE(observation.timestamp_at_install, 0U);
#endif

  mako_rust_fast_one_put_holder_view view{};
  ASSERT_EQ(mako_rust_fast_one_put_holder_pool_get_view(holder_pool, 1, &view),
            MAKO_LOCAL_OK);
  EXPECT_EQ(view.sequence, 1U);
#if defined(MAKO_LOCAL_TEST_HOOKS)
  EXPECT_EQ(view.mako_timestamp, observation.timestamp_at_install);
#endif
  EXPECT_EQ(
      std::string(reinterpret_cast<const char *>(view.value), view.value_len),
      "visible");
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_release(holder_pool, 1),
            MAKO_LOCAL_OK);
}

TEST_F(LocalAbiTest,
       FusedOnePutHolderPostCommitHealthBarrierDefersColdCursorToRust) {
#if !defined(MAKO_LOCAL_TEST_HOOKS)
  GTEST_SKIP() << "post-commit health race requires native commit observer";
#else
  create_holder_pool(4);
  alignas(8) uint64_t acknowledged = 0;
  alignas(8) uint64_t producer_next = 0;
  alignas(8) uint64_t capacity_limit = 4;
  uint8_t unhealthy = 0;
  mako_rust_fast_spsc_holder_control control = make_fused_holder_control(
      holder_pool, &acknowledged, &unhealthy, 4);
  constexpr mako_rust_fast_preselected_record_result kColdSentinel{
      UINT64_MAX - 6, UINT64_MAX - 7};
  control.cold_out = kColdSentinel;
  auto &cold = control.cold_out;
  FusedHolderCommitObservation observation{&producer_next,
                                           &acknowledged,
                                           &unhealthy,
                                           UINT64_MAX,
                                           UINT64_MAX,
                                           0,
                                           true};
  ASSERT_EQ(mako_local_test_set_commit_observer(observe_fused_holder_commit,
                                                &observation),
            MAKO_LOCAL_OK);

  mako_local_txn *txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  const uint64_t put_result = fast_put(txn, "fused-unhealthy", "committed");
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(put_result), MAKO_LOCAL_OK);
  const uint64_t fused =
      mako_rust_fast_txn_try_commit_fused_one_put_holder_single_producer_and_destroy(
          txn, &acknowledged, &unhealthy, &control, capacity_limit);
  txn_for_cleanup = nullptr;
  ASSERT_EQ(mako_local_test_clear_commit_observer(), MAKO_LOCAL_OK);

  EXPECT_EQ(MAKO_RUST_FAST_FUSED_HOLDER_CODE(fused),
            MAKO_RUST_FAST_FUSED_HOLDER_CONSUMED_COMMITTED_UNPUBLISHED);
  const uint32_t timestamp = MAKO_RUST_FAST_FUSED_HOLDER_PAYLOAD(fused);
  EXPECT_NE(timestamp, 0U);
  EXPECT_EQ(timestamp, observation.timestamp_at_install);
  EXPECT_EQ(observation.producer_at_install, 0U);
  EXPECT_EQ(observation.acknowledged_at_install, 0U);
  EXPECT_EQ(producer_next, 0U);
  EXPECT_EQ(acknowledged, 0U);
  EXPECT_EQ(__atomic_load_n(&unhealthy, __ATOMIC_ACQUIRE), 1U);
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(cold.terminal), MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(cold.terminal), MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_PRESELECTED_RECORD_TIMESTAMP(cold), timestamp);
  EXPECT_EQ(MAKO_RUST_FAST_PRESELECTED_HOLDER_SEALED(cold), 1U);
  EXPECT_EQ(MAKO_RUST_FAST_PRESELECTED_RECORD_RESERVED(cold),
            MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(put_result));

  mako_rust_fast_one_put_holder_view view{};
  ASSERT_EQ(mako_rust_fast_one_put_holder_pool_get_view(holder_pool, 1, &view),
            MAKO_LOCAL_OK);
  EXPECT_EQ(view.mako_timestamp, timestamp);
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_release(holder_pool, 1),
            MAKO_LOCAL_OK);
#endif
}

TEST_F(LocalAbiTest, OnePutHolderTransfersExactEncodedValueAllocation) {
  create_holder_pool(4);
  const std::string key = "12345678";
  const std::string value(2048, 'v');
  constexpr uint64_t sequence = 1;

  mako_local_txn *txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  const uint64_t put_result = fast_put(txn, key, value);
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(put_result), MAKO_LOCAL_OK);
  const uint32_t exact_bytes =
      MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(put_result);
  ASSERT_NE(exact_bytes, 0U);
#if defined(MAKO_LOCAL_TEST_HOOKS)
  uint32_t staged_len = 0;
  const uint8_t *const staged_value =
      mako_rust_fast_test_txn_staged_one_put_value(txn, &staged_len);
  ASSERT_NE(staged_value, nullptr);
  ASSERT_EQ(staged_len, value.size());
#endif

  const mako_rust_fast_preselected_record_result commit =
      mako_rust_fast_txn_commit_preselected_unchecked_one_put_holder_single_producer_and_destroy(
          txn, exact_bytes, holder_pool, sequence);
  txn_for_cleanup = nullptr;
  ASSERT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(commit.terminal), MAKO_LOCAL_OK);
  ASSERT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(commit.terminal), MAKO_LOCAL_OK);
  const uint32_t timestamp =
      MAKO_RUST_FAST_PRESELECTED_RECORD_TIMESTAMP(commit);
  ASSERT_NE(timestamp, 0U);
  EXPECT_EQ(MAKO_RUST_FAST_PRESELECTED_HOLDER_SEALED(commit), 1U);
  EXPECT_EQ(MAKO_RUST_FAST_PRESELECTED_RECORD_RESERVED(commit), 0U);

  mako_rust_fast_one_put_holder_view view{};
  ASSERT_EQ(mako_rust_fast_one_put_holder_pool_get_view(
                holder_pool, sequence, &view),
            MAKO_LOCAL_OK);
  EXPECT_EQ(view.sequence, sequence);
  EXPECT_EQ(view.table_id, mako_local_table_id(primary));
  EXPECT_EQ(view.key_len, key.size());
  EXPECT_EQ(view.value_len, value.size());
  EXPECT_EQ(view.mako_timestamp, timestamp);
  EXPECT_EQ(view.reserved, 0U);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(view.key),
                        view.key_len),
            key);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(view.value),
                        view.value_len),
            value);
#if defined(MAKO_LOCAL_TEST_HOOKS)
  // This is the core no-second-copy witness: pool view points at the exact
  // heap allocation staged before STO validation, not a reconstructed record.
  EXPECT_EQ(view.value, staged_value);
#endif

  mako_rust_fast_one_put_holder_view stale{};
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_get_view(
                holder_pool, sequence + 4, &stale),
            MAKO_LOCAL_BUSY);
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_release(
                holder_pool, sequence + 4),
            MAKO_LOCAL_BUSY);
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_destroy(holder_pool),
            MAKO_LOCAL_BUSY);
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_release(holder_pool, sequence),
            MAKO_LOCAL_OK);
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_release(holder_pool, sequence),
            MAKO_LOCAL_BUSY);

  auto *verify = begin();
  EXPECT_EQ(get(verify, primary, key).second,
            std::optional<std::string>(value));
  commit_and_destroy(verify);
}

TEST_F(LocalAbiTest, OnePutHolderSupportsInlineOverflowAndExactReuse) {
  create_holder_pool(2);
  const std::array<std::string, 3> keys{
      std::string{}, std::string(20, 'i'), std::string(40, 'o')};
  for (size_t index = 0; index != keys.size(); ++index) {
    const uint64_t sequence = index + 1;
    const std::string value = "value-" + std::to_string(sequence);
    mako_local_txn *txn = nullptr;
    ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
    txn_for_cleanup = txn;
    const uint64_t put_result = fast_put(txn, keys[index], value);
    ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(put_result), MAKO_LOCAL_OK);
    const uint32_t exact_bytes =
        MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(put_result);
    ASSERT_NE(exact_bytes, 0U);
    const mako_rust_fast_preselected_record_result commit =
        mako_rust_fast_txn_commit_preselected_unchecked_one_put_holder_single_producer_and_destroy(
            txn, exact_bytes, holder_pool, sequence);
    txn_for_cleanup = nullptr;
    ASSERT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(commit.terminal), MAKO_LOCAL_OK);
    ASSERT_EQ(MAKO_RUST_FAST_PRESELECTED_HOLDER_SEALED(commit), 1U);

    mako_rust_fast_one_put_holder_view view{};
    ASSERT_EQ(mako_rust_fast_one_put_holder_pool_get_view(
                  holder_pool, sequence, &view),
              MAKO_LOCAL_OK);
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(view.key),
                          view.key_len),
              keys[index]);
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(view.value),
                          view.value_len),
              value);
    EXPECT_EQ(mako_rust_fast_one_put_holder_pool_release(holder_pool,
                                                          sequence),
              MAKO_LOCAL_OK);
  }
}

TEST_F(LocalAbiTest, OnePutHolderReleasedGenerationCanPublishNextLap) {
  create_holder_pool(2);
  constexpr uint64_t sequence = 11;
  constexpr uint64_t next_generation = sequence + 2;
  const std::array<std::pair<uint64_t, std::string>, 2> generations{{
      {sequence, "first-generation"},
      {next_generation, "next-generation"},
  }};

  for (const auto &[generation, value] : generations) {
    mako_local_txn *txn = nullptr;
    ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
    txn_for_cleanup = txn;
    const uint64_t put_result = fast_put(txn, "same-key", value);
    const uint32_t exact_bytes =
        MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(put_result);
    ASSERT_NE(exact_bytes, 0U);
    const mako_rust_fast_preselected_record_result commit =
        mako_rust_fast_txn_commit_preselected_unchecked_one_put_holder_single_producer_and_destroy(
            txn, exact_bytes, holder_pool, generation);
    txn_for_cleanup = nullptr;
    ASSERT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(commit.terminal), MAKO_LOCAL_OK);
    ASSERT_EQ(MAKO_RUST_FAST_PRESELECTED_HOLDER_SEALED(commit), 1U);

    mako_rust_fast_one_put_holder_view view{};
    ASSERT_EQ(mako_rust_fast_one_put_holder_pool_get_view(
                  holder_pool, generation, &view),
              MAKO_LOCAL_OK);
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(view.value),
                          view.value_len),
              value);
    EXPECT_EQ(
        mako_rust_fast_one_put_holder_pool_release(holder_pool, generation),
        MAKO_LOCAL_OK);
  }
}

TEST_F(LocalAbiTest,
       FusedOnePutHolderConflictConsumesTxnButNotProducerGeneration) {
  create_holder_pool(2);
  constexpr uint64_t kTail = 20;
  constexpr uint64_t kSequence = kTail + 1;
  const std::string contested_key = "fused-holder-conflict";
  auto *seed = begin();
  ASSERT_EQ(put(seed, primary, contested_key, "seed"), MAKO_LOCAL_OK);
  commit_and_destroy(seed);

  struct WorkerResult {
    int attach = MAKO_LOCAL_INTERNAL;
    int begin = MAKO_LOCAL_INTERNAL;
    uint64_t put = UINT64_MAX;
    uint64_t fused = UINT64_MAX;
    mako_rust_fast_preselected_record_result cold{UINT64_MAX, UINT64_MAX};
    alignas(8) uint64_t acknowledged = 20;
    alignas(8) uint64_t producer_next = 20;
    alignas(8) uint64_t capacity_limit = 22;
    uint8_t unhealthy = 0;
  } result;
  std::barrier staged(2);
  std::barrier overwritten(2);
  std::thread worker([&] {
    result.attach = mako_local_thread_attach();
    mako_local_txn *txn = nullptr;
    if (result.attach == MAKO_LOCAL_OK)
      result.begin = mako_rust_fast_txn_begin(db, primary, &txn);
    if (result.begin == MAKO_LOCAL_OK)
      result.put = fast_put(txn, contested_key, "stale");
    staged.arrive_and_wait();
    overwritten.arrive_and_wait();
    if (MAKO_RUST_FAST_PUT_STATUS(result.put) == MAKO_LOCAL_OK) {
      mako_rust_fast_spsc_holder_control control =
          make_fused_holder_control(holder_pool, &result.acknowledged,
                                    &result.unhealthy, 2);
      result.fused =
          mako_rust_fast_txn_try_commit_fused_one_put_holder_single_producer_and_destroy(
              txn, &result.acknowledged, &result.unhealthy, &control,
              result.capacity_limit);
      result.cold = control.cold_out;
      txn = nullptr;
    }
    if (txn != nullptr)
      (void)mako_rust_fast_txn_abort_and_destroy(txn);
  });

  staged.arrive_and_wait();
  mako_local_txn *overwrite = nullptr;
  const int overwrite_begin = mako_local_txn_begin(db, &overwrite);
  const int overwrite_put =
      overwrite_begin == MAKO_LOCAL_OK
          ? put(overwrite, primary, contested_key, "newer")
          : MAKO_LOCAL_INTERNAL;
  const int overwrite_commit = overwrite_put == MAKO_LOCAL_OK
                                   ? mako_local_txn_commit(overwrite)
                                   : MAKO_LOCAL_INTERNAL;
  const int overwrite_destroy = overwrite == nullptr
                                    ? MAKO_LOCAL_INTERNAL
                                    : mako_local_txn_destroy(overwrite);
  overwritten.arrive_and_wait();
  worker.join();

  EXPECT_EQ(overwrite_begin, MAKO_LOCAL_OK);
  EXPECT_EQ(overwrite_put, MAKO_LOCAL_OK);
  EXPECT_EQ(overwrite_commit, MAKO_LOCAL_OK);
  EXPECT_EQ(overwrite_destroy, MAKO_LOCAL_OK);
  EXPECT_EQ(result.attach, MAKO_LOCAL_OK);
  EXPECT_EQ(result.begin, MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_PUT_STATUS(result.put), MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_FUSED_HOLDER_CODE(result.fused),
            MAKO_RUST_FAST_FUSED_HOLDER_CONSUMED_OUTCOME);
  EXPECT_EQ(MAKO_RUST_FAST_FUSED_HOLDER_PAYLOAD(result.fused), 0U);
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(result.cold.terminal),
            MAKO_LOCAL_CONFLICT);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(result.cold.terminal), MAKO_LOCAL_OK);
  EXPECT_EQ(result.cold.record_state & ((UINT64_C(1) << 33) - 1), 0U);
  EXPECT_EQ(MAKO_RUST_FAST_PRESELECTED_RECORD_RESERVED(result.cold),
            MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(result.put));
  EXPECT_EQ(result.producer_next, kTail);
  EXPECT_EQ(result.acknowledged, kTail);

  mako_rust_fast_one_put_holder_view view{};
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_get_view(holder_pool, kSequence,
                                                        &view),
            MAKO_LOCAL_BUSY);
  auto *verify = begin();
  EXPECT_EQ(get(verify, primary, contested_key).second,
            std::optional<std::string>("newer"));
  commit_and_destroy(verify);
}

TEST_F(LocalAbiTest, OnePutHolderConflictLeavesInvisibleGenerationReusable) {
  create_holder_pool(2);
  constexpr uint64_t sequence = 21;
  const std::string contested_key = "holder-conflict";
  auto *seed = begin();
  ASSERT_EQ(put(seed, primary, contested_key, "seed"), MAKO_LOCAL_OK);
  commit_and_destroy(seed);

  struct WorkerResult {
    int attach = MAKO_LOCAL_INTERNAL;
    int begin = MAKO_LOCAL_INTERNAL;
    uint64_t put = UINT64_MAX;
    mako_rust_fast_preselected_record_result conflict{UINT64_MAX, UINT64_MAX};
    int retry_begin = MAKO_LOCAL_INTERNAL;
    uint64_t retry_put = UINT64_MAX;
    mako_rust_fast_preselected_record_result retry{UINT64_MAX, UINT64_MAX};
  } result;
  std::barrier staged(2);
  std::barrier overwritten(2);
  std::thread worker([&] {
    result.attach = mako_local_thread_attach();
    mako_local_txn *txn = nullptr;
    if (result.attach == MAKO_LOCAL_OK)
      result.begin = mako_rust_fast_txn_begin(db, primary, &txn);
    if (result.begin == MAKO_LOCAL_OK)
      result.put = fast_put(txn, contested_key, "stale");
    staged.arrive_and_wait();
    overwritten.arrive_and_wait();
    if (MAKO_RUST_FAST_PUT_STATUS(result.put) == MAKO_LOCAL_OK) {
      const uint32_t exact_bytes =
          MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(result.put);
      result.conflict =
          mako_rust_fast_txn_commit_preselected_unchecked_one_put_holder_single_producer_and_destroy(
              txn, exact_bytes, holder_pool, sequence);
      txn = nullptr;
    }
    if (txn != nullptr)
      (void)mako_rust_fast_txn_abort_and_destroy(txn);

    if (MAKO_RUST_FAST_TERMINAL_STATUS(result.conflict.terminal) ==
        MAKO_LOCAL_CONFLICT) {
      result.retry_begin = mako_rust_fast_txn_begin(db, primary, &txn);
      if (result.retry_begin == MAKO_LOCAL_OK)
        result.retry_put = fast_put(txn, "holder-retry", "visible");
      if (MAKO_RUST_FAST_PUT_STATUS(result.retry_put) == MAKO_LOCAL_OK) {
        const uint32_t exact_bytes =
            MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(result.retry_put);
        result.retry =
            mako_rust_fast_txn_commit_preselected_unchecked_one_put_holder_single_producer_and_destroy(
                txn, exact_bytes, holder_pool, sequence);
        txn = nullptr;
      }
      if (txn != nullptr)
        (void)mako_rust_fast_txn_abort_and_destroy(txn);
    }
  });

  staged.arrive_and_wait();
  mako_local_txn *overwrite = nullptr;
  const int overwrite_begin = mako_local_txn_begin(db, &overwrite);
  const int overwrite_put = overwrite_begin == MAKO_LOCAL_OK
      ? put(overwrite, primary, contested_key, "newer")
      : MAKO_LOCAL_INTERNAL;
  const int overwrite_commit = overwrite_put == MAKO_LOCAL_OK
      ? mako_local_txn_commit(overwrite)
      : MAKO_LOCAL_INTERNAL;
  const int overwrite_destroy = overwrite == nullptr
      ? MAKO_LOCAL_INTERNAL
      : mako_local_txn_destroy(overwrite);
  overwritten.arrive_and_wait();
  worker.join();

  EXPECT_EQ(overwrite_begin, MAKO_LOCAL_OK);
  EXPECT_EQ(overwrite_put, MAKO_LOCAL_OK);
  EXPECT_EQ(overwrite_commit, MAKO_LOCAL_OK);
  EXPECT_EQ(overwrite_destroy, MAKO_LOCAL_OK);
  EXPECT_EQ(result.attach, MAKO_LOCAL_OK);
  EXPECT_EQ(result.begin, MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_PUT_STATUS(result.put), MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(result.conflict.terminal),
            MAKO_LOCAL_CONFLICT);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(result.conflict.terminal),
            MAKO_LOCAL_OK);
  EXPECT_EQ(result.conflict.record_state, 0U);
  EXPECT_EQ(result.retry_begin, MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_PUT_STATUS(result.retry_put), MAKO_LOCAL_OK);
  ASSERT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(result.retry.terminal),
            MAKO_LOCAL_OK);
  ASSERT_EQ(MAKO_RUST_FAST_PRESELECTED_HOLDER_SEALED(result.retry), 1U);

  mako_rust_fast_one_put_holder_view view{};
  ASSERT_EQ(mako_rust_fast_one_put_holder_pool_get_view(
                holder_pool, sequence, &view),
            MAKO_LOCAL_OK);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(view.key),
                        view.key_len),
            "holder-retry");
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(view.value),
                        view.value_len),
            "visible");
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_release(holder_pool, sequence),
            MAKO_LOCAL_OK);

  auto *verify = begin();
  EXPECT_EQ(get(verify, primary, contested_key).second,
            std::optional<std::string>("newer"));
  EXPECT_EQ(get(verify, primary, "holder-retry").second,
            std::optional<std::string>("visible"));
  commit_and_destroy(verify);
}

TEST_F(LocalAbiTest,
       TrustedSingleProducerUncheckedOnePutRejectsMalformedAndHookError) {
#if defined(MAKO_LOCAL_TEST_HOOKS)
  const uint64_t tickets_before =
      mako_rust_fast_test_record_validation_tickets(db);
#endif
  mako_local_txn *txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  const uint64_t malformed_put =
      fast_put(txn, "single-malformed", "invisible");
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(malformed_put), MAKO_LOCAL_OK);
  const uint32_t malformed_bytes =
      MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(malformed_put);
  ASSERT_NE(malformed_bytes, 0U);
  std::vector<uint8_t> malformed_storage(malformed_bytes, 0xa5);
  const std::vector<uint8_t> malformed_untouched = malformed_storage;
  ThinRecordBinding malformed_binding{&malformed_storage, 702};
  uint8_t written = 99;
  const uint64_t malformed =
      mako_rust_fast_txn_commit_unchecked_one_put_record_single_producer_and_destroy(
          txn, malformed_bytes + 1, bind_thin_record, &malformed_binding,
          &written);
  txn_for_cleanup = nullptr;
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(malformed),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(malformed), MAKO_LOCAL_OK);
  EXPECT_EQ(malformed_binding.calls, 0);
  EXPECT_EQ(written, 0U);
  EXPECT_EQ(malformed_storage, malformed_untouched);

  txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  const uint64_t rejected_put =
      fast_put(txn, "single-rejected", "also-invisible");
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(rejected_put), MAKO_LOCAL_OK);
  const uint32_t rejected_bytes =
      MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(rejected_put);
  ASSERT_NE(rejected_bytes, 0U);
  std::vector<uint8_t> rejected_storage(rejected_bytes, 0xa5);
  const std::vector<uint8_t> rejected_untouched = rejected_storage;
  ThinRecordBinding rejected_binding{&rejected_storage, 703};
  rejected_binding.accept = false;
  written = 99;
  const uint64_t rejected =
      mako_rust_fast_txn_commit_unchecked_one_put_record_single_producer_and_destroy(
          txn, rejected_bytes, bind_thin_record, &rejected_binding, &written);
  txn_for_cleanup = nullptr;
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(rejected),
            MAKO_LOCAL_COMMIT_HOOK_REJECTED);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(rejected), MAKO_LOCAL_OK);
  EXPECT_EQ(rejected_binding.calls, 1);
  EXPECT_EQ(written, 0U);
  EXPECT_EQ(rejected_storage, rejected_untouched);

#if defined(MAKO_LOCAL_TEST_HOOKS)
  EXPECT_EQ(mako_rust_fast_test_record_validation_tickets(db),
            tickets_before);
#endif
  auto *verify = begin();
  EXPECT_FALSE(get(verify, primary, "single-malformed").second.has_value());
  EXPECT_FALSE(get(verify, primary, "single-rejected").second.has_value());
  commit_and_destroy(verify);
}

TEST_F(LocalAbiTest,
       TrustedPreselectedSingleProducerRejectsStaleSizeAndTarget) {
  enum class InvalidCase {
    wrong_expected_size,
    short_target,
    null_target,
    zero_sequence,
    later_mutation,
  };
  const std::array<InvalidCase, 5> cases{
      InvalidCase::wrong_expected_size,
      InvalidCase::short_target,
      InvalidCase::null_target,
      InvalidCase::zero_sequence,
      InvalidCase::later_mutation,
  };
  std::vector<std::string> keys;

  for (size_t index = 0; index != cases.size(); ++index) {
    const std::string key = "preselected-invalid-" + std::to_string(index);
    keys.push_back(key);
    mako_local_txn *txn = nullptr;
    ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
    txn_for_cleanup = txn;
    const uint64_t put_result = fast_put(txn, key, "must-not-install");
    ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(put_result), MAKO_LOCAL_OK);
    const uint32_t exact_bytes =
        MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(put_result);
    ASSERT_NE(exact_bytes, 0U);

    std::vector<uint8_t> storage(exact_bytes + 1, 0xa5);
    const std::vector<uint8_t> untouched = storage;
    uint32_t expected_bytes = exact_bytes;
    uint64_t sequence = 810 + index;
    uint8_t *target = storage.data();
    size_t capacity = exact_bytes;
    switch (cases[index]) {
    case InvalidCase::wrong_expected_size:
      ++expected_bytes;
      capacity = expected_bytes;
      break;
    case InvalidCase::short_target:
      --capacity;
      break;
    case InvalidCase::null_target:
      target = nullptr;
      break;
    case InvalidCase::zero_sequence:
      sequence = 0;
      break;
    case InvalidCase::later_mutation: {
      const std::string later = key + "-later";
      keys.push_back(later);
      const uint64_t later_put = fast_put(txn, later, "also-invisible");
      ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(later_put), MAKO_LOCAL_OK);
      EXPECT_EQ(MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(later_put), 0U);
      break;
    }
    }

    const mako_rust_fast_preselected_record_result result =
        mako_rust_fast_txn_commit_preselected_unchecked_one_put_record_single_producer_and_destroy(
            txn, expected_bytes, sequence, target, capacity);
    txn_for_cleanup = nullptr;
    EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(result.terminal),
              MAKO_LOCAL_INVALID_ARGUMENT)
        << index;
    EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(result.terminal),
              MAKO_LOCAL_OK)
        << index;
    EXPECT_EQ(MAKO_RUST_FAST_PRESELECTED_RECORD_TIMESTAMP(result), 0U)
        << index;
    EXPECT_EQ(MAKO_RUST_FAST_PRESELECTED_RECORD_WRITTEN(result), 0U)
        << index;
    EXPECT_EQ(MAKO_RUST_FAST_PRESELECTED_RECORD_RESERVED(result), 0U)
        << index;
    EXPECT_EQ(storage, untouched) << index;
  }

  auto *verify = begin();
  for (const std::string &key : keys)
    EXPECT_FALSE(get(verify, primary, key).second.has_value()) << key;
  commit_and_destroy(verify);
}

#if defined(MAKO_LOCAL_TEST_HOOKS)
TEST_F(LocalAbiTest,
       TrustedSingleProducerUncheckedOnePutStillRechecksPredicates) {
  auto *seed = begin();
  ASSERT_EQ(put(seed, primary, "single-range-h", "seed"), MAKO_LOCAL_OK);
  commit_and_destroy(seed);

  struct WorkerResult {
    int attach = MAKO_LOCAL_INTERNAL;
    int observer_set = MAKO_LOCAL_INTERNAL;
    int begin = MAKO_LOCAL_INTERNAL;
    int scan = MAKO_LOCAL_INTERNAL;
    uint64_t put = UINT64_MAX;
    uint32_t exact_bytes = 0;
    uint64_t commit = UINT64_MAX;
    int observer_clear = MAKO_LOCAL_INTERNAL;
    uint8_t written = 0;
    std::vector<uint8_t> storage;
    ThinRecordBinding binding;
  } worker_result;

  std::atomic<bool> writeset_locked{false};
  std::atomic<bool> release_worker{false};
  ParkingPhaseCommitObserver parking{
      MAKO_LOCAL_TEST_COMMIT_WRITESET_LOCKED, &writeset_locked,
      &release_worker};
  const uint64_t tickets_before =
      mako_rust_fast_test_record_validation_tickets(db);

  std::thread worker([&] {
    worker_result.attach = mako_local_thread_attach();
    mako_local_txn *txn = nullptr;
    if (worker_result.attach == MAKO_LOCAL_OK)
      worker_result.observer_set = mako_local_test_set_commit_observer(
          park_at_commit_phase, &parking);
    if (worker_result.observer_set == MAKO_LOCAL_OK)
      worker_result.begin = mako_rust_fast_txn_begin(db, primary, &txn);
    if (worker_result.begin == MAKO_LOCAL_OK) {
      const std::string lower = "single-range-a";
      const std::string upper = "single-range-z";
      const mako_local_scan_options options{
          MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE, MAKO_LOCAL_SCAN_HAS_UPPER,
          reinterpret_cast<const uint8_t *>(lower.data()), lower.size(),
          reinterpret_cast<const uint8_t *>(upper.data()), upper.size(),
          nullptr, 0};
      worker_result.scan =
          scan_chunk(txn, primary, options, false, 8, 1024).status;
    }
    if (worker_result.scan == MAKO_LOCAL_OK) {
      worker_result.put = fast_put(txn, "single-side", "must-abort");
      worker_result.exact_bytes =
          MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(worker_result.put);
    }
    if (MAKO_RUST_FAST_PUT_STATUS(worker_result.put) == MAKO_LOCAL_OK &&
        worker_result.exact_bytes != 0) {
      worker_result.storage.assign(worker_result.exact_bytes, 0xa5);
      worker_result.binding.storage = &worker_result.storage;
      worker_result.binding.sequence = 704;
      worker_result.commit =
          mako_rust_fast_txn_commit_unchecked_one_put_record_single_producer_and_destroy(
              txn, worker_result.exact_bytes, bind_thin_record,
              &worker_result.binding, &worker_result.written);
      txn = nullptr;
    }
    if (txn != nullptr)
      (void)mako_rust_fast_txn_abort_and_destroy(txn);
    worker_result.observer_clear = mako_local_test_clear_commit_observer();
  });

  const auto lock_deadline = std::chrono::steady_clock::now() +
                             std::chrono::seconds(5);
  while (!writeset_locked.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < lock_deadline)
    std::this_thread::yield();
  if (!writeset_locked.load(std::memory_order_acquire)) {
    release_worker.store(true, std::memory_order_release);
    worker.join();
    FAIL() << "single-producer record commit did not lock its write set";
    return;
  }

  // This ordinary STO commit is not a competing cache-record terminal. It
  // changes the already-checked range after phase 1; the non-ticketed but
  // non-null validation gate must make phase 2 reject the stale predicate.
  mako_local_txn *phantom = nullptr;
  const int phantom_begin = mako_local_txn_begin(db, &phantom);
  const int phantom_put = phantom_begin == MAKO_LOCAL_OK
      ? put(phantom, primary, "single-range-m", "phantom")
      : MAKO_LOCAL_INTERNAL;
  int phantom_commit = MAKO_LOCAL_INTERNAL;
  int phantom_destroy = MAKO_LOCAL_INTERNAL;
  if (phantom_put == MAKO_LOCAL_OK)
    phantom_commit = mako_local_txn_commit(phantom);
  if (phantom != nullptr)
    phantom_destroy = mako_local_txn_destroy(phantom);
  release_worker.store(true, std::memory_order_release);
  worker.join();

  EXPECT_EQ(phantom_begin, MAKO_LOCAL_OK);
  EXPECT_EQ(phantom_put, MAKO_LOCAL_OK);
  EXPECT_EQ(phantom_commit, MAKO_LOCAL_OK);
  EXPECT_EQ(phantom_destroy, MAKO_LOCAL_OK);
  EXPECT_EQ(worker_result.attach, MAKO_LOCAL_OK);
  EXPECT_EQ(worker_result.observer_set, MAKO_LOCAL_OK);
  EXPECT_EQ(worker_result.begin, MAKO_LOCAL_OK);
  EXPECT_EQ(worker_result.scan, MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_PUT_STATUS(worker_result.put), MAKO_LOCAL_OK);
  EXPECT_NE(worker_result.exact_bytes, 0U);
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(worker_result.commit),
            MAKO_LOCAL_CONFLICT);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(worker_result.commit),
            MAKO_LOCAL_OK);
  EXPECT_EQ(worker_result.binding.calls, 0);
  EXPECT_EQ(worker_result.written, 0U);
  EXPECT_EQ(worker_result.observer_clear, MAKO_LOCAL_OK);
  EXPECT_EQ(mako_rust_fast_test_record_validation_tickets(db),
            tickets_before);

  auto *verify = begin();
  EXPECT_FALSE(get(verify, primary, "single-side").second.has_value());
  EXPECT_EQ(get(verify, primary, "single-range-m").second,
            std::optional<std::string>("phantom"));
  commit_and_destroy(verify);
}

TEST_F(LocalAbiTest,
       TrustedPreselectedSingleProducerStillRechecksPredicates) {
  auto *seed = begin();
  ASSERT_EQ(put(seed, primary, "preselected-range-h", "seed"),
            MAKO_LOCAL_OK);
  commit_and_destroy(seed);

  struct WorkerResult {
    int attach = MAKO_LOCAL_INTERNAL;
    int observer_set = MAKO_LOCAL_INTERNAL;
    int begin = MAKO_LOCAL_INTERNAL;
    int scan = MAKO_LOCAL_INTERNAL;
    uint64_t put = UINT64_MAX;
    uint32_t exact_bytes = 0;
    mako_rust_fast_preselected_record_result commit{UINT64_MAX, UINT64_MAX};
    int observer_clear = MAKO_LOCAL_INTERNAL;
    std::vector<uint8_t> storage;
  } worker_result;

  std::atomic<bool> writeset_locked{false};
  std::atomic<bool> release_worker{false};
  ParkingPhaseCommitObserver parking{
      MAKO_LOCAL_TEST_COMMIT_WRITESET_LOCKED, &writeset_locked,
      &release_worker};
  const uint64_t tickets_before =
      mako_rust_fast_test_record_validation_tickets(db);

  std::thread worker([&] {
    worker_result.attach = mako_local_thread_attach();
    mako_local_txn *txn = nullptr;
    if (worker_result.attach == MAKO_LOCAL_OK)
      worker_result.observer_set = mako_local_test_set_commit_observer(
          park_at_commit_phase, &parking);
    if (worker_result.observer_set == MAKO_LOCAL_OK)
      worker_result.begin = mako_rust_fast_txn_begin(db, primary, &txn);
    if (worker_result.begin == MAKO_LOCAL_OK) {
      const std::string lower = "preselected-range-a";
      const std::string upper = "preselected-range-z";
      const mako_local_scan_options options{
          MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE, MAKO_LOCAL_SCAN_HAS_UPPER,
          reinterpret_cast<const uint8_t *>(lower.data()), lower.size(),
          reinterpret_cast<const uint8_t *>(upper.data()), upper.size(),
          nullptr, 0};
      worker_result.scan =
          scan_chunk(txn, primary, options, false, 8, 1024).status;
    }
    if (worker_result.scan == MAKO_LOCAL_OK) {
      worker_result.put =
          fast_put(txn, "preselected-side", "must-abort");
      worker_result.exact_bytes =
          MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(worker_result.put);
    }
    if (MAKO_RUST_FAST_PUT_STATUS(worker_result.put) == MAKO_LOCAL_OK &&
        worker_result.exact_bytes != 0) {
      worker_result.storage.assign(worker_result.exact_bytes, 0xa5);
      worker_result.commit =
          mako_rust_fast_txn_commit_preselected_unchecked_one_put_record_single_producer_and_destroy(
              txn, worker_result.exact_bytes, 820,
              worker_result.storage.data(), worker_result.storage.size());
      txn = nullptr;
    }
    if (txn != nullptr)
      (void)mako_rust_fast_txn_abort_and_destroy(txn);
    worker_result.observer_clear = mako_local_test_clear_commit_observer();
  });

  const auto lock_deadline = std::chrono::steady_clock::now() +
                             std::chrono::seconds(5);
  while (!writeset_locked.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < lock_deadline)
    std::this_thread::yield();
  if (!writeset_locked.load(std::memory_order_acquire)) {
    release_worker.store(true, std::memory_order_release);
    worker.join();
    FAIL() << "preselected record commit did not lock its write set";
    return;
  }

  // The ordinary transaction creates a range anti-dependency after phase 1.
  // The callback-free terminal still supplies a non-null validation gate, so
  // phase 2 must reject before serializing or accepting the retained target.
  mako_local_txn *phantom = nullptr;
  const int phantom_begin = mako_local_txn_begin(db, &phantom);
  const int phantom_put = phantom_begin == MAKO_LOCAL_OK
      ? put(phantom, primary, "preselected-range-m", "phantom")
      : MAKO_LOCAL_INTERNAL;
  int phantom_commit = MAKO_LOCAL_INTERNAL;
  int phantom_destroy = MAKO_LOCAL_INTERNAL;
  if (phantom_put == MAKO_LOCAL_OK)
    phantom_commit = mako_local_txn_commit(phantom);
  if (phantom != nullptr)
    phantom_destroy = mako_local_txn_destroy(phantom);
  release_worker.store(true, std::memory_order_release);
  worker.join();

  EXPECT_EQ(phantom_begin, MAKO_LOCAL_OK);
  EXPECT_EQ(phantom_put, MAKO_LOCAL_OK);
  EXPECT_EQ(phantom_commit, MAKO_LOCAL_OK);
  EXPECT_EQ(phantom_destroy, MAKO_LOCAL_OK);
  EXPECT_EQ(worker_result.attach, MAKO_LOCAL_OK);
  EXPECT_EQ(worker_result.observer_set, MAKO_LOCAL_OK);
  EXPECT_EQ(worker_result.begin, MAKO_LOCAL_OK);
  EXPECT_EQ(worker_result.scan, MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_PUT_STATUS(worker_result.put), MAKO_LOCAL_OK);
  EXPECT_NE(worker_result.exact_bytes, 0U);
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(worker_result.commit.terminal),
            MAKO_LOCAL_CONFLICT);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(worker_result.commit.terminal),
            MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_PRESELECTED_RECORD_TIMESTAMP(
                worker_result.commit),
            0U);
  EXPECT_EQ(MAKO_RUST_FAST_PRESELECTED_RECORD_WRITTEN(worker_result.commit),
            0U);
  EXPECT_EQ(MAKO_RUST_FAST_PRESELECTED_RECORD_RESERVED(worker_result.commit),
            0U);
  EXPECT_TRUE(std::all_of(worker_result.storage.begin(),
                          worker_result.storage.end(),
                          [](uint8_t byte) { return byte == 0xa5; }));
  EXPECT_EQ(worker_result.observer_clear, MAKO_LOCAL_OK);
  EXPECT_EQ(mako_rust_fast_test_record_validation_tickets(db),
            tickets_before);

  auto *verify = begin();
  EXPECT_FALSE(get(verify, primary, "preselected-side").second.has_value());
  EXPECT_EQ(get(verify, primary, "preselected-range-m").second,
            std::optional<std::string>("phantom"));
  commit_and_destroy(verify);
}
#endif

TEST_F(LocalAbiTest, TrustedUncheckedOnePutRejectsStaleExactSizeBeforeBind) {
  mako_local_txn *txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  const uint64_t put_result = fast_put(txn, "fused-stale", "invisible");
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(put_result), MAKO_LOCAL_OK);
  const uint32_t exact_bytes =
      MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(put_result);
  ASSERT_NE(exact_bytes, 0U);

  std::vector<uint8_t> storage(exact_bytes, 0xa5);
  ThinRecordBinding binding{&storage, 602};
  uint8_t written = 99;
  const uint64_t commit =
      mako_rust_fast_txn_commit_unchecked_one_put_record_and_destroy(
          txn, exact_bytes + 1, bind_thin_record, &binding, &written);
  txn_for_cleanup = nullptr;
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(commit),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(commit), MAKO_LOCAL_OK);
  EXPECT_EQ(binding.calls, 0);
  EXPECT_EQ(written, 0U);

  auto *verify = begin();
  EXPECT_FALSE(get(verify, primary, "fused-stale").second.has_value());
  commit_and_destroy(verify);
}

#if READ_MY_WRITES
TEST_F(LocalAbiTest, TrustedUncheckedOnePutRejectsLaterReadOrMutation) {
  mako_local_txn *txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  const uint64_t first = fast_put(txn, "fused-first", "one");
  const uint32_t stale_bytes =
      MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(first);
  ASSERT_NE(stale_bytes, 0U);
  const uint64_t second = fast_put(txn, "fused-second", "two");
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(second), MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(second), 0U);

  std::vector<uint8_t> storage(stale_bytes, 0xa5);
  ThinRecordBinding binding{&storage, 603};
  uint8_t written = 99;
  uint64_t commit =
      mako_rust_fast_txn_commit_unchecked_one_put_record_and_destroy(
          txn, stale_bytes, bind_thin_record, &binding, &written);
  txn_for_cleanup = nullptr;
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(commit),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(binding.calls, 0);
  EXPECT_EQ(written, 0U);

  txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  const uint64_t before_read = fast_put(txn, "fused-read", "staged");
  const uint32_t read_stale_bytes =
      MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(before_read);
  ASSERT_NE(read_stale_bytes, 0U);
  EXPECT_EQ(get(txn, primary, "fused-read").second,
            std::optional<std::string>("staged"));
  storage.assign(read_stale_bytes, 0xa5);
  binding = ThinRecordBinding{&storage, 604};
  written = 99;
  commit = mako_rust_fast_txn_commit_unchecked_one_put_record_and_destroy(
      txn, read_stale_bytes, bind_thin_record, &binding, &written);
  txn_for_cleanup = nullptr;
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(commit),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(binding.calls, 0);
  EXPECT_EQ(written, 0U);

  // A read before the sole Put is safe: the direct spans are captured only
  // after read-side transaction growth has completed.
  txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  EXPECT_FALSE(get(txn, primary, "fused-probe").second.has_value());
  const uint64_t after_read = fast_put(txn, "fused-after-read", "visible");
  const uint32_t after_read_bytes =
      MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(after_read);
  ASSERT_NE(after_read_bytes, 0U);
  storage.assign(after_read_bytes, 0xa5);
  binding = ThinRecordBinding{&storage, 605};
  written = 0;
  commit = mako_rust_fast_txn_commit_unchecked_one_put_record_and_destroy(
      txn, after_read_bytes, bind_thin_record, &binding, &written);
  txn_for_cleanup = nullptr;
  ASSERT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(commit), MAKO_LOCAL_OK);
  ASSERT_EQ(written, 1U);

  auto *verify = begin();
  EXPECT_FALSE(get(verify, primary, "fused-first").second.has_value());
  EXPECT_FALSE(get(verify, primary, "fused-second").second.has_value());
  EXPECT_FALSE(get(verify, primary, "fused-read").second.has_value());
  EXPECT_EQ(get(verify, primary, "fused-after-read").second,
            std::optional<std::string>("visible"));
  commit_and_destroy(verify);
}
#endif

#if READ_MY_WRITES
TEST_F(LocalAbiTest, TrustedThinRecordFallsBackAfterPointAndRangeReads) {
  auto *seed = begin();
  ASSERT_EQ(put(seed, primary, "witness-a", "left"), MAKO_LOCAL_OK);
  ASSERT_EQ(put(seed, primary, "witness-z", "right"), MAKO_LOCAL_OK);
  commit_and_destroy(seed);

  const std::string key = "witness-middle";
  const std::string value("new\0value\xff", 10);
  mako_local_txn *txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  const uint64_t put_result = fast_put(txn, key, value);
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(put_result), MAKO_LOCAL_OK);
  ASSERT_EQ(MAKO_RUST_FAST_PUT_CREATED(put_result), 1U);

  // Point and range RYW paths may grow/reorganize transaction state. They
  // invalidate the borrowed one-put witness, so record production must use the
  // canonical transaction walk and still preserve the same final write set.
  ASSERT_EQ(get(txn, primary, key).second,
            std::optional<std::string>(value));
  const std::string lower = "witness-a";
  const std::string upper = "witness-z";
  const mako_local_scan_options options{
      MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE, MAKO_LOCAL_SCAN_HAS_UPPER,
      reinterpret_cast<const uint8_t *>(lower.data()), lower.size(),
      reinterpret_cast<const uint8_t *>(upper.data()), upper.size(),
      nullptr, 0};
  const auto scan = scan_chunk(txn, primary, options, false, 8, 1024);
  ASSERT_EQ(scan.status, MAKO_LOCAL_OK);
  ASSERT_EQ(scan.done, 1U);
  const auto staged = std::find(
      scan.entries.begin(), scan.entries.end(),
      std::pair<std::string, std::string>{key, value});
  ASSERT_NE(staged, scan.entries.end());

  size_t exact_bytes = 0;
  uint32_t operation_count = 0;
  ASSERT_EQ(mako_rust_fast_txn_record_preflight_with_checksum(
                txn, 1 << 20, MAKO_RUST_FAST_RECORD_CHECKSUM_NONE,
                &exact_bytes, &operation_count),
            MAKO_LOCAL_OK);
  ASSERT_EQ(operation_count, 1U);
  EXPECT_EQ(exact_bytes, 26U + 17U + key.size() + value.size());

  std::vector<uint8_t> storage(exact_bytes, 0xa5);
  ThinRecordBinding binding{&storage, 406};
  uint8_t written = 0;
  const uint64_t commit = mako_rust_fast_txn_commit_record_and_destroy(
      txn, bind_thin_record, &binding, &written);
  txn_for_cleanup = nullptr;
  ASSERT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(commit), MAKO_LOCAL_OK);
  ASSERT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(commit), MAKO_LOCAL_OK);
  ASSERT_EQ(written, 1U);

  DecodedThinRecord decoded;
  ASSERT_TRUE(decode_thin_record(storage, &decoded));
  ASSERT_EQ(decoded.mutations.size(), 1U);
  EXPECT_EQ(decoded.mutations[0].tag, 1U);
  EXPECT_EQ(decoded.mutations[0].key, key);
  EXPECT_EQ(decoded.mutations[0].value, value);

  auto *verify = begin();
  EXPECT_EQ(get(verify, primary, key).second,
            std::optional<std::string>(value));
  commit_and_destroy(verify);
}
#endif

TEST_F(LocalAbiTest, TrustedThinRecordReadOnlyNeedsNoCapacity) {
  mako_local_txn *txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;

  size_t exact_bytes = 0;
  uint32_t operation_count = 99;
  ASSERT_EQ(mako_rust_fast_txn_record_preflight(
                txn, 0, &exact_bytes, &operation_count),
            MAKO_LOCAL_OK);
  EXPECT_EQ(exact_bytes, 30U);
  EXPECT_EQ(operation_count, 0U);

  const uint64_t result = mako_rust_fast_txn_commit_and_destroy(txn);
  txn_for_cleanup = nullptr;
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(result), MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(result), MAKO_LOCAL_OK);
}

#if READ_MY_WRITES
TEST_F(LocalAbiTest, TrustedThinRecordNormalizesSameKeyAndNetCancellation) {
  mako_local_txn *txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(fast_put(txn, "temporary", "value")),
            MAKO_LOCAL_OK);
  uint8_t existed = 0;
  ASSERT_EQ(remove(txn, primary, "temporary", &existed), MAKO_LOCAL_OK);
  ASSERT_EQ(existed, 1U);

  size_t exact_bytes = 0;
  uint32_t operation_count = 99;
  ASSERT_EQ(mako_rust_fast_txn_record_preflight(
                txn, 30, &exact_bytes, &operation_count),
            MAKO_LOCAL_OK);
  EXPECT_EQ(exact_bytes, 30U);
  EXPECT_EQ(operation_count, 0U);
  const uint64_t empty_result =
      mako_rust_fast_txn_commit_and_destroy(txn);
  txn_for_cleanup = nullptr;
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(empty_result), MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(empty_result), MAKO_LOCAL_OK);

  auto *seed = begin();
  ASSERT_EQ(put(seed, primary, "chain", "seed"), MAKO_LOCAL_OK);
  commit_and_destroy(seed);

  txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(fast_put(txn, "chain", "first")),
            MAKO_LOCAL_OK);
  ASSERT_EQ(remove(txn, primary, "chain", &existed), MAKO_LOCAL_OK);
  ASSERT_EQ(existed, 1U);
  const std::string final_value("final\0bytes", 11);
  uint8_t inserted = 0;
  ASSERT_EQ(insert(txn, primary, "chain", final_value, &inserted),
            MAKO_LOCAL_OK);
  ASSERT_EQ(inserted, 1U);

  ASSERT_EQ(mako_rust_fast_txn_record_preflight(
                txn, 1 << 20, &exact_bytes, &operation_count),
            MAKO_LOCAL_OK);
  ASSERT_EQ(operation_count, 1U);
  std::vector<uint8_t> storage(exact_bytes, 0);
  ThinRecordBinding binding{&storage, 73};
  uint8_t written = 0;
  const uint64_t result = mako_rust_fast_txn_commit_record_and_destroy(
      txn, bind_thin_record, &binding, &written);
  txn_for_cleanup = nullptr;
  ASSERT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(result), MAKO_LOCAL_OK);
  ASSERT_EQ(written, 1U);
  DecodedThinRecord decoded;
  ASSERT_TRUE(decode_thin_record(storage, &decoded));
  ASSERT_EQ(decoded.mutations.size(), 1U);
  EXPECT_EQ(decoded.mutations[0].tag, 1U);
  EXPECT_EQ(decoded.mutations[0].key, "chain");
  EXPECT_EQ(decoded.mutations[0].value, final_value);
}

TEST_F(LocalAbiTest, TrustedThinRecordFallsBackAfterSecondFastPut) {
  auto *seed = begin();
  ASSERT_EQ(put(seed, primary, "fast-chain", "seed"), MAKO_LOCAL_OK);
  commit_and_destroy(seed);

  mako_local_txn *txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(
                fast_put(txn, "fast-chain", "intermediate")),
            MAKO_LOCAL_OK);
  const std::string final_value("final\0fast", 10);
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(
                fast_put(txn, "fast-chain", final_value)),
            MAKO_LOCAL_OK);

  size_t exact_bytes = 0;
  uint32_t operation_count = 0;
  ASSERT_EQ(mako_rust_fast_txn_record_preflight_with_checksum(
                txn, 1 << 20, MAKO_RUST_FAST_RECORD_CHECKSUM_NONE,
                &exact_bytes, &operation_count),
            MAKO_LOCAL_OK);
  ASSERT_EQ(operation_count, 1U);
  EXPECT_EQ(exact_bytes, 26U + 17U + std::string("fast-chain").size() +
                             final_value.size());

  std::vector<uint8_t> storage(exact_bytes, 0);
  ThinRecordBinding binding{&storage, 501};
  uint8_t written = 0;
  const uint64_t result = mako_rust_fast_txn_commit_record_and_destroy(
      txn, bind_thin_record, &binding, &written);
  txn_for_cleanup = nullptr;
  ASSERT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(result), MAKO_LOCAL_OK);
  ASSERT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(result), MAKO_LOCAL_OK);
  ASSERT_EQ(written, 1U);
  DecodedThinRecord decoded;
  ASSERT_TRUE(decode_thin_record(storage, &decoded));
  ASSERT_EQ(decoded.mutations.size(), 1U);
  EXPECT_EQ(decoded.mutations[0].key, "fast-chain");
  EXPECT_EQ(decoded.mutations[0].value, final_value);

  txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(fast_put(txn, "fast-a", "value-a")),
            MAKO_LOCAL_OK);
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(fast_put(txn, "fast-b", "value-b")),
            MAKO_LOCAL_OK);
  ASSERT_EQ(mako_rust_fast_txn_record_preflight_with_checksum(
                txn, 1 << 20, MAKO_RUST_FAST_RECORD_CHECKSUM_NONE,
                &exact_bytes, &operation_count),
            MAKO_LOCAL_OK);
  ASSERT_EQ(operation_count, 2U);
  storage.assign(exact_bytes, 0);
  binding = ThinRecordBinding{&storage, 502};
  written = 0;
  const uint64_t multi_result =
      mako_rust_fast_txn_commit_record_and_destroy(
          txn, bind_thin_record, &binding, &written);
  txn_for_cleanup = nullptr;
  ASSERT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(multi_result), MAKO_LOCAL_OK);
  ASSERT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(multi_result), MAKO_LOCAL_OK);
  ASSERT_EQ(written, 1U);
  ASSERT_TRUE(decode_thin_record(storage, &decoded));
  ASSERT_EQ(decoded.mutations.size(), 2U);
  EXPECT_EQ(decoded.mutations[0].key, "fast-a");
  EXPECT_EQ(decoded.mutations[0].value, "value-a");
  EXPECT_EQ(decoded.mutations[1].key, "fast-b");
  EXPECT_EQ(decoded.mutations[1].value, "value-b");
}
#endif

TEST_F(LocalAbiTest, TrustedThinRecordCapAndInvalidBindingAbortDefinitely) {
  mako_local_txn *txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(fast_put(txn, "capped", "value")),
            MAKO_LOCAL_OK);
  constexpr size_t expected_bytes = 30 + 17 + 6 + 5;
  size_t exact_bytes = 0;
  uint32_t operation_count = 0;
  EXPECT_EQ(mako_rust_fast_txn_record_preflight(
                txn, expected_bytes - 1, &exact_bytes, &operation_count),
            MAKO_LOCAL_VALUE_TOO_LARGE);
  EXPECT_EQ(exact_bytes, expected_bytes);
  EXPECT_EQ(operation_count, 1U);
  size_t second_exact = 99;
  uint32_t second_count = 99;
  EXPECT_EQ(mako_rust_fast_txn_record_preflight(
                txn, expected_bytes, &second_exact, &second_count),
            MAKO_LOCAL_BUSY);
  EXPECT_EQ(second_exact, 0U);
  EXPECT_EQ(second_count, 0U);
  EXPECT_EQ(put(txn, primary, "late", "write"), MAKO_LOCAL_BUSY);
  EXPECT_EQ(MAKO_RUST_FAST_PUT_STATUS(
                fast_put(txn, "late-fast", "write")),
            MAKO_LOCAL_BUSY);

  std::vector<uint8_t> storage(exact_bytes, 0xa5);
  ThinRecordBinding never_called{&storage, 1};
  uint8_t written = 99;
  const uint64_t capped = mako_rust_fast_txn_commit_record_and_destroy(
      txn, bind_thin_record, &never_called, &written);
  txn_for_cleanup = nullptr;
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(capped),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(capped), MAKO_LOCAL_OK);
  EXPECT_EQ(never_called.calls, 0);
  EXPECT_EQ(written, 0U);

  txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(fast_put(txn, "bad-bind", "value")),
            MAKO_LOCAL_OK);
  ASSERT_EQ(mako_rust_fast_txn_record_preflight(
                txn, 1 << 20, &exact_bytes, &operation_count),
            MAKO_LOCAL_OK);
  storage.assign(exact_bytes, 0xa5);
  ThinRecordBinding invalid{&storage, 99};
  invalid.invalidate_sequence = true;
  written = 99;
  const uint64_t invalid_result =
      mako_rust_fast_txn_commit_record_and_destroy(
          txn, bind_thin_record, &invalid, &written);
  txn_for_cleanup = nullptr;
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(invalid_result),
            MAKO_LOCAL_COMMIT_HOOK_REJECTED);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(invalid_result), MAKO_LOCAL_OK);
  EXPECT_EQ(invalid.calls, 1);
  EXPECT_EQ(written, 0U);

  txn = begin();
  EXPECT_FALSE(get(txn, primary, "capped").second.has_value());
  EXPECT_FALSE(get(txn, primary, "bad-bind").second.has_value());
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, TrustedThinRecordConflictNeverBindsOrWritesBuffer) {
  auto *seed = begin();
  ASSERT_EQ(put(seed, primary, "thin-conflict", "seed"), MAKO_LOCAL_OK);
  commit_and_destroy(seed);

  mako_local_txn *loser = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &loser), MAKO_LOCAL_OK);
  txn_for_cleanup = loser;
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(
                fast_put(loser, "thin-conflict", "loser")),
            MAKO_LOCAL_OK);

  struct WinnerResult {
    int attach = MAKO_LOCAL_INTERNAL;
    int begin = MAKO_LOCAL_INTERNAL;
    uint64_t put = UINT64_MAX;
    uint64_t commit = UINT64_MAX;
  } winner;
  std::thread worker([&] {
    winner.attach = mako_local_thread_attach();
    mako_local_txn *txn = nullptr;
    if (winner.attach == MAKO_LOCAL_OK)
      winner.begin = mako_rust_fast_txn_begin(db, primary, &txn);
    if (winner.begin == MAKO_LOCAL_OK) {
      winner.put = fast_put(txn, "thin-conflict", "winner");
      winner.commit = mako_rust_fast_txn_commit_and_destroy(txn);
    }
  });
  worker.join();
  ASSERT_EQ(winner.attach, MAKO_LOCAL_OK);
  ASSERT_EQ(winner.begin, MAKO_LOCAL_OK);
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(winner.put), MAKO_LOCAL_OK);
  ASSERT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(winner.commit), MAKO_LOCAL_OK);

  size_t exact_bytes = 0;
  uint32_t operation_count = 0;
  ASSERT_EQ(mako_rust_fast_txn_record_preflight(
                loser, 1 << 20, &exact_bytes, &operation_count),
            MAKO_LOCAL_OK);
  ASSERT_EQ(operation_count, 1U);
  std::vector<uint8_t> storage(exact_bytes, 0xa5);
  const std::vector<uint8_t> untouched = storage;
  ThinRecordBinding binding{&storage, 404};
  uint8_t written = 99;
  const uint64_t result = mako_rust_fast_txn_commit_record_and_destroy(
      loser, bind_thin_record, &binding, &written);
  txn_for_cleanup = nullptr;
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(result), MAKO_LOCAL_CONFLICT);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(result), MAKO_LOCAL_OK);
  EXPECT_EQ(binding.calls, 0);
  EXPECT_EQ(written, 0U);
  EXPECT_EQ(storage, untouched);

  auto *verify = begin();
  EXPECT_EQ(get(verify, primary, "thin-conflict").second,
            std::optional<std::string>("winner"));
  commit_and_destroy(verify);
}

#if defined(MAKO_LOCAL_TEST_HOOKS)
TEST_F(LocalAbiTest,
       TrustedThinRecordEarlyAndLateGatesPreserveReadWriteSerializationPrefix) {
  auto *seed = begin();
  ASSERT_EQ(put(seed, primary, "ordered-x", "old"), MAKO_LOCAL_OK);
  commit_and_destroy(seed);

  struct RecordWorkerResult {
    int attach = MAKO_LOCAL_INTERNAL;
    int observer_set = MAKO_LOCAL_INTERNAL;
    int begin = MAKO_LOCAL_INTERNAL;
    int read = MAKO_LOCAL_INTERNAL;
    std::optional<std::string> read_value;
    uint64_t put = UINT64_MAX;
    int preflight = MAKO_LOCAL_INTERNAL;
    size_t exact_bytes = 0;
    uint32_t operation_count = 0;
    uint64_t commit = UINT64_MAX;
    int observer_clear = MAKO_LOCAL_INTERNAL;
    uint8_t written = 0;
    std::vector<uint8_t> storage;
    ThinRecordBinding binding;
  } first, second;

  std::atomic<bool> first_validated{false};
  std::atomic<bool> release_first{false};
  std::atomic<bool> second_locked{false};
  std::atomic<int> first_bind_calls{0};
  std::atomic<int> second_bind_calls{0};
  std::atomic<uint64_t> next_sequence{1};
  ParkingPhaseCommitObserver first_parking{
      MAKO_LOCAL_TEST_COMMIT_LOCAL_VALIDATION_COMPLETE,
      &first_validated, &release_first};
  SignalPhaseCommitObserver second_signal{
      MAKO_LOCAL_TEST_COMMIT_WRITESET_LOCKED, &second_locked};

  std::thread first_worker([&] {
    first.attach = mako_local_thread_attach();
    mako_local_txn *txn = nullptr;
    if (first.attach == MAKO_LOCAL_OK)
      first.observer_set = mako_local_test_set_commit_observer(
          park_at_commit_phase, &first_parking);
    if (first.observer_set == MAKO_LOCAL_OK)
      first.begin = mako_rust_fast_txn_begin(db, primary, &txn);
    if (first.begin == MAKO_LOCAL_OK) {
      auto read = get(txn, primary, "ordered-x");
      first.read = read.first;
      first.read_value = std::move(read.second);
    }
    if (first.read == MAKO_LOCAL_OK)
      first.put = fast_put(txn, "ordered-a", "first");
    if (MAKO_RUST_FAST_PUT_STATUS(first.put) == MAKO_LOCAL_OK)
      first.preflight = mako_rust_fast_txn_record_preflight(
          txn, 1 << 20, &first.exact_bytes, &first.operation_count);
    if (first.preflight == MAKO_LOCAL_OK) {
      first.storage.assign(first.exact_bytes, 0xa5);
      first.binding.storage = &first.storage;
      first.binding.next_sequence = &next_sequence;
      first.binding.published_calls = &first_bind_calls;
      first.commit = mako_rust_fast_txn_commit_record_and_destroy(
          txn, bind_thin_record, &first.binding, &first.written);
      txn = nullptr;
    }
    if (txn != nullptr)
      (void)mako_rust_fast_txn_abort_and_destroy(txn);
    first.observer_clear = mako_local_test_clear_commit_observer();
  });

  const auto first_deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(5);
  while (!first_validated.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < first_deadline)
    std::this_thread::yield();
  if (!first_validated.load(std::memory_order_acquire)) {
    release_first.store(true, std::memory_order_release);
    first_worker.join();
    FAIL() << "first record commit did not reach final validation";
    return;
  }

  std::thread second_worker([&] {
    second.attach = mako_local_thread_attach();
    mako_local_txn *txn = nullptr;
    if (second.attach == MAKO_LOCAL_OK)
      second.observer_set = mako_local_test_set_commit_observer(
          signal_commit_phase, &second_signal);
    if (second.observer_set == MAKO_LOCAL_OK)
      second.begin = mako_rust_fast_txn_begin(db, primary, &txn);
    if (second.begin == MAKO_LOCAL_OK) {
      second.put = fast_put(txn, "ordered-x", "new");
      second.exact_bytes =
          MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(second.put);
      if (MAKO_RUST_FAST_PUT_STATUS(second.put) == MAKO_LOCAL_OK &&
          second.exact_bytes != 0) {
        second.preflight = MAKO_LOCAL_OK;
        second.operation_count = 1;
      }
    }
    if (second.preflight == MAKO_LOCAL_OK) {
      second.storage.assign(second.exact_bytes, 0xa5);
      second.binding.storage = &second.storage;
      second.binding.next_sequence = &next_sequence;
      second.binding.published_calls = &second_bind_calls;
      second.commit =
          mako_rust_fast_txn_commit_unchecked_one_put_record_and_destroy(
              txn, second.exact_bytes, bind_thin_record,
              &second.binding, &second.written);
      txn = nullptr;
    }
    if (txn != nullptr)
      (void)mako_rust_fast_txn_abort_and_destroy(txn);
    second.observer_clear = mako_local_test_clear_commit_observer();
  });

  const auto second_deadline = std::chrono::steady_clock::now() +
                               std::chrono::seconds(5);
  while (!second_locked.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < second_deadline)
    std::this_thread::yield();
  if (!second_locked.load(std::memory_order_acquire)) {
    release_first.store(true, std::memory_order_release);
    second_worker.join();
    first_worker.join();
    FAIL() << "second record commit did not lock its write set";
    return;
  }

  // The first transaction has read old X and validated, so it must precede
  // the exact unchecked update of X. The latter validates under X's write
  // lock before requesting its late ticket. Waiting for both fetched tickets
  // proves that the optimized late path still queues behind the parked
  // general transaction before it can bind.
  const auto queued_deadline = std::chrono::steady_clock::now() +
                               std::chrono::seconds(5);
  while ((mako_rust_fast_test_record_validation_tickets(db) != 2 ||
          mako_rust_fast_test_record_validation_wait_observations(db) == 0) &&
         std::chrono::steady_clock::now() < queued_deadline)
    std::this_thread::yield();
  if (mako_rust_fast_test_record_validation_tickets(db) != 2 ||
      mako_rust_fast_test_record_validation_wait_observations(db) == 0) {
    release_first.store(true, std::memory_order_release);
    second_worker.join();
    first_worker.join();
    FAIL() << "second record commit did not observe the occupied validation gate";
    return;
  }
  EXPECT_EQ(second_bind_calls.load(std::memory_order_acquire), 0)
      << "a later anti-dependent transaction passed final validation order";

  release_first.store(true, std::memory_order_release);
  first_worker.join();
  second_worker.join();

  EXPECT_EQ(first.attach, MAKO_LOCAL_OK);
  EXPECT_EQ(first.observer_set, MAKO_LOCAL_OK);
  EXPECT_EQ(first.begin, MAKO_LOCAL_OK);
  EXPECT_EQ(first.read, MAKO_LOCAL_OK);
  EXPECT_EQ(first.read_value, std::optional<std::string>("old"));
  EXPECT_EQ(MAKO_RUST_FAST_PUT_STATUS(first.put), MAKO_LOCAL_OK);
  EXPECT_EQ(first.preflight, MAKO_LOCAL_OK);
  EXPECT_EQ(first.operation_count, 1U);
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(first.commit), MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(first.commit), MAKO_LOCAL_OK);
  EXPECT_EQ(first.written, 1U);
  EXPECT_EQ(first.observer_clear, MAKO_LOCAL_OK);

  EXPECT_EQ(second.attach, MAKO_LOCAL_OK);
  EXPECT_EQ(second.observer_set, MAKO_LOCAL_OK);
  EXPECT_EQ(second.begin, MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_PUT_STATUS(second.put), MAKO_LOCAL_OK);
  EXPECT_EQ(second.preflight, MAKO_LOCAL_OK);
  EXPECT_EQ(second.operation_count, 1U);
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(second.commit), MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(second.commit), MAKO_LOCAL_OK);
  EXPECT_EQ(second.written, 1U);
  EXPECT_EQ(second.observer_clear, MAKO_LOCAL_OK);

  EXPECT_EQ(first_bind_calls.load(std::memory_order_acquire), 1);
  EXPECT_EQ(second_bind_calls.load(std::memory_order_acquire), 1);
  EXPECT_EQ(first.binding.sequence, 1U);
  EXPECT_EQ(second.binding.sequence, 2U);
  EXPECT_LT(first.binding.timestamp, second.binding.timestamp);
  DecodedThinRecord first_record;
  DecodedThinRecord second_record;
  ASSERT_TRUE(decode_thin_record(first.storage, &first_record));
  ASSERT_TRUE(decode_thin_record(second.storage, &second_record));
  EXPECT_EQ(first_record.sequence, 1U);
  EXPECT_EQ(second_record.sequence, 2U);
  EXPECT_EQ(first_record.timestamp, first.binding.timestamp);
  EXPECT_EQ(second_record.timestamp, second.binding.timestamp);

  auto *verify = begin();
  EXPECT_EQ(get(verify, primary, "ordered-a").second,
            std::optional<std::string>("first"));
  EXPECT_EQ(get(verify, primary, "ordered-x").second,
            std::optional<std::string>("new"));
  commit_and_destroy(verify);
}

TEST_F(LocalAbiTest,
       TrustedThinRecordGatePreservesPredicateSerializationPrefix) {
  auto *seed = begin();
  ASSERT_EQ(put(seed, primary, "ordered-range-h", "seed"), MAKO_LOCAL_OK);
  commit_and_destroy(seed);

  struct RecordWorkerResult {
    int attach = MAKO_LOCAL_INTERNAL;
    int observer_set = MAKO_LOCAL_INTERNAL;
    int begin = MAKO_LOCAL_INTERNAL;
    int scan = MAKO_LOCAL_INTERNAL;
    std::vector<std::pair<std::string, std::string>> scan_entries;
    uint64_t put = UINT64_MAX;
    int preflight = MAKO_LOCAL_INTERNAL;
    size_t exact_bytes = 0;
    uint32_t operation_count = 0;
    uint64_t commit = UINT64_MAX;
    int observer_clear = MAKO_LOCAL_INTERNAL;
    uint8_t written = 0;
    std::vector<uint8_t> storage;
    ThinRecordBinding binding;
  } first, second;

  std::atomic<bool> first_validated{false};
  std::atomic<bool> release_first{false};
  std::atomic<bool> second_locked{false};
  std::atomic<int> first_bind_calls{0};
  std::atomic<int> second_bind_calls{0};
  std::atomic<uint64_t> next_sequence{1};
  ParkingPhaseCommitObserver first_parking{
      MAKO_LOCAL_TEST_COMMIT_LOCAL_VALIDATION_COMPLETE,
      &first_validated, &release_first};
  SignalPhaseCommitObserver second_signal{
      MAKO_LOCAL_TEST_COMMIT_WRITESET_LOCKED, &second_locked};

  std::thread first_worker([&] {
    first.attach = mako_local_thread_attach();
    mako_local_txn *txn = nullptr;
    if (first.attach == MAKO_LOCAL_OK)
      first.observer_set = mako_local_test_set_commit_observer(
          park_at_commit_phase, &first_parking);
    if (first.observer_set == MAKO_LOCAL_OK)
      first.begin = mako_rust_fast_txn_begin(db, primary, &txn);
    if (first.begin == MAKO_LOCAL_OK) {
      const std::string lower = "ordered-range-a";
      const std::string upper = "ordered-range-z";
      const mako_local_scan_options options{
          MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE, MAKO_LOCAL_SCAN_HAS_UPPER,
          reinterpret_cast<const uint8_t *>(lower.data()), lower.size(),
          reinterpret_cast<const uint8_t *>(upper.data()), upper.size(),
          nullptr, 0};
      auto scan = scan_chunk(txn, primary, options, false, 8, 1024);
      first.scan = scan.status;
      first.scan_entries = std::move(scan.entries);
    }
    if (first.scan == MAKO_LOCAL_OK)
      first.put = fast_put(txn, "ordered-range-side", "first");
    if (MAKO_RUST_FAST_PUT_STATUS(first.put) == MAKO_LOCAL_OK)
      first.preflight = mako_rust_fast_txn_record_preflight(
          txn, 1 << 20, &first.exact_bytes, &first.operation_count);
    if (first.preflight == MAKO_LOCAL_OK) {
      first.storage.assign(first.exact_bytes, 0xa5);
      first.binding.storage = &first.storage;
      first.binding.next_sequence = &next_sequence;
      first.binding.published_calls = &first_bind_calls;
      first.commit = mako_rust_fast_txn_commit_record_and_destroy(
          txn, bind_thin_record, &first.binding, &first.written);
      txn = nullptr;
    }
    if (txn != nullptr)
      (void)mako_rust_fast_txn_abort_and_destroy(txn);
    first.observer_clear = mako_local_test_clear_commit_observer();
  });

  const auto first_deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(5);
  while (!first_validated.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < first_deadline)
    std::this_thread::yield();
  if (!first_validated.load(std::memory_order_acquire)) {
    release_first.store(true, std::memory_order_release);
    first_worker.join();
    FAIL() << "predicate reader did not reach final validation";
    return;
  }

  std::thread second_worker([&] {
    second.attach = mako_local_thread_attach();
    mako_local_txn *txn = nullptr;
    if (second.attach == MAKO_LOCAL_OK)
      second.observer_set = mako_local_test_set_commit_observer(
          signal_commit_phase, &second_signal);
    if (second.observer_set == MAKO_LOCAL_OK)
      second.begin = mako_rust_fast_txn_begin(db, primary, &txn);
    if (second.begin == MAKO_LOCAL_OK)
      second.put = fast_put(txn, "ordered-range-m", "inserted");
    if (MAKO_RUST_FAST_PUT_STATUS(second.put) == MAKO_LOCAL_OK)
      second.preflight = mako_rust_fast_txn_record_preflight(
          txn, 1 << 20, &second.exact_bytes, &second.operation_count);
    if (second.preflight == MAKO_LOCAL_OK) {
      second.storage.assign(second.exact_bytes, 0xa5);
      second.binding.storage = &second.storage;
      second.binding.next_sequence = &next_sequence;
      second.binding.published_calls = &second_bind_calls;
      second.commit = mako_rust_fast_txn_commit_record_and_destroy(
          txn, bind_thin_record, &second.binding, &second.written);
      txn = nullptr;
    }
    if (txn != nullptr)
      (void)mako_rust_fast_txn_abort_and_destroy(txn);
    second.observer_clear = mako_local_test_clear_commit_observer();
  });

  const auto second_deadline = std::chrono::steady_clock::now() +
                               std::chrono::seconds(5);
  while (!second_locked.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < second_deadline)
    std::this_thread::yield();
  if (!second_locked.load(std::memory_order_acquire)) {
    release_first.store(true, std::memory_order_release);
    second_worker.join();
    first_worker.join();
    FAIL() << "range insert did not lock its write set";
    return;
  }

  const auto queued_deadline = std::chrono::steady_clock::now() +
                               std::chrono::seconds(5);
  while ((mako_rust_fast_test_record_validation_tickets(db) != 2 ||
          mako_rust_fast_test_record_validation_wait_observations(db) == 0) &&
         std::chrono::steady_clock::now() < queued_deadline)
    std::this_thread::yield();
  if (mako_rust_fast_test_record_validation_tickets(db) != 2 ||
      mako_rust_fast_test_record_validation_wait_observations(db) == 0) {
    release_first.store(true, std::memory_order_release);
    second_worker.join();
    first_worker.join();
    FAIL() << "range insert did not observe the occupied validation gate";
    return;
  }
  EXPECT_EQ(second_bind_calls.load(std::memory_order_acquire), 0)
      << "a phantom-producing transaction passed its predicate reader";

  release_first.store(true, std::memory_order_release);
  first_worker.join();
  second_worker.join();

  EXPECT_EQ(first.attach, MAKO_LOCAL_OK);
  EXPECT_EQ(first.observer_set, MAKO_LOCAL_OK);
  EXPECT_EQ(first.begin, MAKO_LOCAL_OK);
  EXPECT_EQ(first.scan, MAKO_LOCAL_OK);
  EXPECT_EQ(first.scan_entries,
            (std::vector<std::pair<std::string, std::string>>{
                {"ordered-range-h", "seed"}}));
  EXPECT_EQ(MAKO_RUST_FAST_PUT_STATUS(first.put), MAKO_LOCAL_OK);
  EXPECT_EQ(first.preflight, MAKO_LOCAL_OK);
  EXPECT_EQ(first.operation_count, 1U);
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(first.commit), MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(first.commit), MAKO_LOCAL_OK);
  EXPECT_EQ(first.written, 1U);
  EXPECT_EQ(first.observer_clear, MAKO_LOCAL_OK);

  EXPECT_EQ(second.attach, MAKO_LOCAL_OK);
  EXPECT_EQ(second.observer_set, MAKO_LOCAL_OK);
  EXPECT_EQ(second.begin, MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_PUT_STATUS(second.put), MAKO_LOCAL_OK);
  EXPECT_EQ(second.preflight, MAKO_LOCAL_OK);
  EXPECT_EQ(second.operation_count, 1U);
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(second.commit), MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(second.commit), MAKO_LOCAL_OK);
  EXPECT_EQ(second.written, 1U);
  EXPECT_EQ(second.observer_clear, MAKO_LOCAL_OK);

  EXPECT_EQ(first_bind_calls.load(std::memory_order_acquire), 1);
  EXPECT_EQ(second_bind_calls.load(std::memory_order_acquire), 1);
  EXPECT_EQ(first.binding.sequence, 1U);
  EXPECT_EQ(second.binding.sequence, 2U);
  EXPECT_LT(first.binding.timestamp, second.binding.timestamp);

  auto *verify = begin();
  EXPECT_EQ(get(verify, primary, "ordered-range-side").second,
            std::optional<std::string>("first"));
  EXPECT_EQ(get(verify, primary, "ordered-range-m").second,
            std::optional<std::string>("inserted"));
  commit_and_destroy(verify);
}
#endif

TEST_F(LocalAbiTest, TrustedRustFastPathPreservesBoundsBudgetAndBinding) {
  mako_local_db *other_db = nullptr;
  ASSERT_EQ(mako_local_db_open(&other_db), MAKO_LOCAL_OK);
  mako_local_table *other_table = nullptr;
  const std::string other_name = "fast-other-db";
  ASSERT_EQ(mako_local_table_open(
                other_db, reinterpret_cast<const uint8_t *>(other_name.data()),
                other_name.size(), next_table_id.fetch_add(1), &other_table),
            MAKO_LOCAL_OK);

  mako_local_txn *txn = reinterpret_cast<mako_local_txn *>(uintptr_t{1});
  EXPECT_EQ(mako_rust_fast_txn_begin(db, other_table, &txn),
            MAKO_LOCAL_WRONG_DB_OR_TABLE);
  EXPECT_EQ(txn, nullptr);
  ASSERT_EQ(mako_local_db_close(other_db), MAKO_LOCAL_OK);

  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  ASSERT_NE(txn, nullptr);
  txn_for_cleanup = txn;

  const uint8_t byte = 0;
  const uint64_t oversized = mako_rust_fast_txn_put(
      txn, &byte, MAKO_LOCAL_MAX_KEY_BYTES + 1, &byte, 1);
  EXPECT_EQ(MAKO_RUST_FAST_PUT_STATUS(oversized), MAKO_LOCAL_VALUE_TOO_LARGE);
  EXPECT_EQ(MAKO_RUST_FAST_PUT_CREATED(oversized), 0U);

  // Two-byte keys each charge 4 + ceil(2 / 8) == 5 credits. Exactly 102 fit
  // in the 512-credit transaction; the next put atomically aborts it.
  for (uint16_t index = 0; index != 102; ++index) {
    std::string key(2, '\0');
    key[0] = static_cast<char>(index >> 8);
    key[1] = static_cast<char>(index);
    const uint64_t result = fast_put(txn, key, "v");
    ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(result), MAKO_LOCAL_OK)
        << "write " << index;
    EXPECT_EQ(MAKO_RUST_FAST_PUT_CREATED(result), 1U);
  }
  const std::string over_budget_key("\0\x66", 2);
  const uint64_t over_budget = fast_put(txn, over_budget_key, "v");
  EXPECT_EQ(MAKO_RUST_FAST_PUT_STATUS(over_budget), MAKO_LOCAL_TXN_TOO_LARGE);
  EXPECT_EQ(MAKO_RUST_FAST_PUT_CREATED(over_budget), 0U);

  // Put's terminal budget failure has already completed native abort. The
  // consuming abort spelling must only recycle the finished facade.
  const uint64_t reset = mako_rust_fast_txn_abort_and_destroy(txn);
  txn_for_cleanup = nullptr;
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(reset), MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(reset), MAKO_LOCAL_OK);

  txn = begin();
  const std::string first_key("\0\0", 2);
  EXPECT_FALSE(get(txn, primary, first_key).second.has_value());
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, TrustedRustFastHookRejectionAndExceptionStayContained) {
  HookObservation rejected;
  mako_local_txn *txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  ASSERT_EQ(
      MAKO_RUST_FAST_PUT_STATUS(fast_put(txn, "fast-hook-reject", "invisible")),
      MAKO_LOCAL_OK);
  const uint64_t rejected_result =
      mako_rust_fast_txn_commit_with_hook_and_destroy(txn, reject_hook,
                                                      &rejected);
  txn_for_cleanup = nullptr;
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(rejected_result),
            MAKO_LOCAL_COMMIT_HOOK_REJECTED);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(rejected_result), MAKO_LOCAL_OK);
  EXPECT_EQ(rejected.calls, 1);
  EXPECT_NE(rejected.timestamp, 0U);

  txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  txn_for_cleanup = txn;
  ASSERT_EQ(MAKO_RUST_FAST_PUT_STATUS(
                fast_put(txn, "fast-hook-throw", "also-invisible")),
            MAKO_LOCAL_OK);
  const uint64_t thrown = mako_rust_fast_txn_commit_with_hook_and_destroy(
      txn, throwing_hook, nullptr);
  txn_for_cleanup = nullptr;
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(thrown),
            MAKO_LOCAL_COMMIT_HOOK_REJECTED);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(thrown), MAKO_LOCAL_OK);

  txn = begin();
  EXPECT_FALSE(get(txn, primary, "fast-hook-reject").second.has_value());
  EXPECT_FALSE(get(txn, primary, "fast-hook-throw").second.has_value());
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, PostValidationHookCarriesMonotonicMakoTimestamp) {
  constexpr uint32_t recovered_max = UINT32_C(1) << 24;
  ASSERT_EQ(mako_local_advance_mako_timestamp_past(recovered_max),
            MAKO_LOCAL_OK);

  HookObservation first;
  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "hook-first", "one"), MAKO_LOCAL_OK);
  EXPECT_EQ(mako_local_txn_commit_with_hook(txn, accept_hook, &first),
            MAKO_LOCAL_OK);
  destroy_tracked(txn);
  EXPECT_EQ(first.calls, 1);
  EXPECT_GT(first.timestamp, recovered_max);
  EXPECT_LE(first.timestamp, MAKO_LOCAL_MAX_MAKO_TIMESTAMP);

  HookObservation second;
  txn = begin();
  ASSERT_EQ(put(txn, primary, "hook-second", "two"), MAKO_LOCAL_OK);
  EXPECT_EQ(mako_local_txn_commit_with_hook(txn, accept_hook, &second),
            MAKO_LOCAL_OK);
  destroy_tracked(txn);
  EXPECT_EQ(second.calls, 1);
  EXPECT_GT(second.timestamp, first.timestamp);

  // Advancing to a smaller observed value is monotonic and harmless.
  EXPECT_EQ(mako_local_advance_mako_timestamp_past(first.timestamp),
            MAKO_LOCAL_OK);

  HookObservation read_only;
  txn = begin();
  EXPECT_EQ(get(txn, primary, "hook-first").second,
            std::optional<std::string>("one"));
  EXPECT_EQ(mako_local_txn_commit_with_hook(txn, accept_hook, &read_only),
            MAKO_LOCAL_OK);
  destroy_tracked(txn);
  EXPECT_EQ(read_only.calls, 0);
}

TEST_F(LocalAbiTest, HookRejectionAndExceptionDefinitelyAbortBeforeInstall) {
  HookObservation rejected;
  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "hook-reject", "invisible"), MAKO_LOCAL_OK);
  EXPECT_EQ(mako_local_txn_commit_with_hook(txn, reject_hook, &rejected),
            MAKO_LOCAL_COMMIT_HOOK_REJECTED);
  destroy_tracked(txn);
  EXPECT_EQ(rejected.calls, 1);
  EXPECT_NE(rejected.timestamp, 0U);

  txn = begin();
  EXPECT_FALSE(get(txn, primary, "hook-reject").second.has_value());
  commit_and_destroy(txn);

  txn = begin();
  ASSERT_EQ(put(txn, primary, "hook-throw", "also-invisible"), MAKO_LOCAL_OK);
  EXPECT_EQ(mako_local_txn_commit_with_hook(txn, throwing_hook, nullptr),
            MAKO_LOCAL_COMMIT_HOOK_REJECTED);
  destroy_tracked(txn);

  txn = begin();
  EXPECT_FALSE(get(txn, primary, "hook-throw").second.has_value());
  ASSERT_EQ(put(txn, primary, "after-hook-reject", "works"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);
}

#if defined(MAKO_LOCAL_TEST_HOOKS)
TEST(MakoLocalAbiCleanupFailure,
     PreselectedTerminalRetainsAcceptedRecordWitnessOnUncertainty) {
  struct Result {
    int attach = MAKO_LOCAL_INTERNAL;
    int db_open = MAKO_LOCAL_INTERNAL;
    int table_open = MAKO_LOCAL_INTERNAL;
    int begin = MAKO_LOCAL_INTERNAL;
    uint64_t put = UINT64_MAX;
    int arm = MAKO_LOCAL_INTERNAL;
    mako_rust_fast_preselected_record_result commit{UINT64_MAX, UINT64_MAX};
    int health = MAKO_LOCAL_INTERNAL;
    int close = MAKO_LOCAL_INTERNAL;
    uint64_t table_id = 0;
    std::vector<uint8_t> storage;
  } result;
  constexpr uint64_t sequence = 830;
  const uint64_t quarantined_before =
      mako_local_quarantined_worker_count();

  std::thread worker([&] {
    result.attach = mako_local_thread_attach();
    mako_local_db *db = nullptr;
    if (result.attach == MAKO_LOCAL_OK)
      result.db_open = mako_local_db_open(&db);
    mako_local_table *table = nullptr;
    const std::string table_name = "preselected-cleanup-uncertainty";
    result.table_id = next_table_id.fetch_add(1);
    if (result.db_open == MAKO_LOCAL_OK) {
      result.table_open = mako_local_table_open(
          db, reinterpret_cast<const uint8_t *>(table_name.data()),
          table_name.size(), result.table_id, &table);
    }
    mako_local_txn *txn = nullptr;
    if (result.table_open == MAKO_LOCAL_OK)
      result.begin = mako_rust_fast_txn_begin(db, table, &txn);
    if (result.begin == MAKO_LOCAL_OK)
      result.put = fast_put(txn, "preselected-cleanup", "installed-or-unknown");
    const uint32_t exact_bytes =
        MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(result.put);
    if (MAKO_RUST_FAST_PUT_STATUS(result.put) == MAKO_LOCAL_OK &&
        exact_bytes != 0) {
      result.storage.assign(exact_bytes, 0xa5);
      result.arm = mako_local_test_arm_cleanup_failure(
          MAKO_LOCAL_CLEANUP_BOUNDARY_COMMIT);
    }
    if (result.arm == MAKO_LOCAL_OK) {
      result.commit =
          mako_rust_fast_txn_commit_preselected_unchecked_one_put_record_single_producer_and_destroy(
              txn, exact_bytes, sequence, result.storage.data(),
              result.storage.size());
      txn = nullptr;
    }
    if (txn != nullptr)
      (void)mako_rust_fast_txn_abort_and_destroy(txn);
    result.health = mako_local_worker_health();
    result.close = mako_local_db_close(db);

    // Cleanup uncertainty quarantines this process-lifetime worker and its
    // facade storage. Do not attempt another native operation on this thread.
  });
  worker.join();

  EXPECT_EQ(result.attach, MAKO_LOCAL_OK);
  EXPECT_EQ(result.db_open, MAKO_LOCAL_OK);
  EXPECT_EQ(result.table_open, MAKO_LOCAL_OK);
  EXPECT_EQ(result.begin, MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_PUT_STATUS(result.put), MAKO_LOCAL_OK);
  EXPECT_EQ(result.arm, MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(result.commit.terminal),
            MAKO_LOCAL_WORKER_POISONED);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(result.commit.terminal),
            MAKO_LOCAL_WORKER_POISONED);
  const uint32_t timestamp =
      MAKO_RUST_FAST_PRESELECTED_RECORD_TIMESTAMP(result.commit);
  EXPECT_NE(timestamp, 0U);
  EXPECT_EQ(MAKO_RUST_FAST_PRESELECTED_RECORD_WRITTEN(result.commit), 1U);
  EXPECT_EQ(MAKO_RUST_FAST_PRESELECTED_RECORD_RESERVED(result.commit), 0U);
  EXPECT_EQ(result.health, MAKO_LOCAL_WORKER_POISONED);
  EXPECT_EQ(result.close, MAKO_LOCAL_BUSY);
  EXPECT_EQ(mako_local_quarantined_worker_count(), quarantined_before + 1);

  DecodedThinRecord decoded;
  ASSERT_TRUE(decode_thin_record(result.storage, &decoded));
  EXPECT_EQ(decoded.sequence, sequence);
  EXPECT_EQ(decoded.timestamp, timestamp);
  ASSERT_EQ(decoded.mutations.size(), 1U);
  EXPECT_EQ(decoded.mutations[0].table_id, result.table_id);
  EXPECT_EQ(decoded.mutations[0].key, "preselected-cleanup");
  EXPECT_EQ(decoded.mutations[0].value, "installed-or-unknown");
}

TEST(MakoLocalAbiCleanupFailure,
     OnePutHolderSealsTransferredAllocationOnAcceptedUncertainty) {
  mako_rust_fast_one_put_holder_pool *pool = nullptr;
  ASSERT_EQ(mako_rust_fast_one_put_holder_pool_create(2, 0, 0, &pool),
            MAKO_LOCAL_OK);
  ASSERT_NE(pool, nullptr);
  struct Result {
    int attach = MAKO_LOCAL_INTERNAL;
    int db_open = MAKO_LOCAL_INTERNAL;
    int table_open = MAKO_LOCAL_INTERNAL;
    int begin = MAKO_LOCAL_INTERNAL;
    uint64_t put = UINT64_MAX;
    const uint8_t *staged_value = nullptr;
    uint32_t staged_len = 0;
    int arm = MAKO_LOCAL_INTERNAL;
    mako_rust_fast_preselected_record_result commit{UINT64_MAX, UINT64_MAX};
    int health = MAKO_LOCAL_INTERNAL;
    int close = MAKO_LOCAL_INTERNAL;
    uint64_t table_id = 0;
  } result;
  constexpr uint64_t sequence = 841;
  // Exercise the allocating overflow-key preparation on the accepted cleanup
  // uncertainty path, where the staged value still must be sealed exactly once
  // after try_commit unwinds.
  const std::string key(40, 'k');
  const std::string value(1024, 'u');
  const uint64_t quarantined_before =
      mako_local_quarantined_worker_count();

  std::thread worker([&] {
    result.attach = mako_local_thread_attach();
    mako_local_db *db = nullptr;
    if (result.attach == MAKO_LOCAL_OK)
      result.db_open = mako_local_db_open(&db);
    mako_local_table *table = nullptr;
    const std::string table_name = "holder-cleanup-uncertainty";
    result.table_id = next_table_id.fetch_add(1);
    if (result.db_open == MAKO_LOCAL_OK) {
      result.table_open = mako_local_table_open(
          db, reinterpret_cast<const uint8_t *>(table_name.data()),
          table_name.size(), result.table_id, &table);
    }
    mako_local_txn *txn = nullptr;
    if (result.table_open == MAKO_LOCAL_OK)
      result.begin = mako_rust_fast_txn_begin(db, table, &txn);
    if (result.begin == MAKO_LOCAL_OK) {
      result.put = fast_put(txn, key, value);
      result.staged_value = mako_rust_fast_test_txn_staged_one_put_value(
          txn, &result.staged_len);
    }
    const uint32_t exact_bytes =
        MAKO_RUST_FAST_PUT_UNCHECKED_RECORD_BYTES(result.put);
    if (MAKO_RUST_FAST_PUT_STATUS(result.put) == MAKO_LOCAL_OK &&
        exact_bytes != 0) {
      result.arm = mako_local_test_arm_cleanup_failure(
          MAKO_LOCAL_CLEANUP_BOUNDARY_COMMIT);
    }
    if (result.arm == MAKO_LOCAL_OK) {
      result.commit =
          mako_rust_fast_txn_commit_preselected_unchecked_one_put_holder_single_producer_and_destroy(
              txn, exact_bytes, pool, sequence);
      txn = nullptr;
    }
    if (txn != nullptr)
      (void)mako_rust_fast_txn_abort_and_destroy(txn);
    result.health = mako_local_worker_health();
    result.close = mako_local_db_close(db);
  });
  worker.join();

  EXPECT_EQ(result.attach, MAKO_LOCAL_OK);
  EXPECT_EQ(result.db_open, MAKO_LOCAL_OK);
  EXPECT_EQ(result.table_open, MAKO_LOCAL_OK);
  EXPECT_EQ(result.begin, MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_PUT_STATUS(result.put), MAKO_LOCAL_OK);
  EXPECT_EQ(result.staged_len, value.size());
  ASSERT_NE(result.staged_value, nullptr);
  EXPECT_EQ(result.arm, MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(result.commit.terminal),
            MAKO_LOCAL_WORKER_POISONED);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(result.commit.terminal),
            MAKO_LOCAL_WORKER_POISONED);
  const uint32_t timestamp =
      MAKO_RUST_FAST_PRESELECTED_RECORD_TIMESTAMP(result.commit);
  EXPECT_NE(timestamp, 0U);
  EXPECT_EQ(MAKO_RUST_FAST_PRESELECTED_HOLDER_SEALED(result.commit), 1U);
  EXPECT_EQ(MAKO_RUST_FAST_PRESELECTED_RECORD_RESERVED(result.commit), 0U);
  EXPECT_EQ(result.health, MAKO_LOCAL_WORKER_POISONED);
  EXPECT_EQ(result.close, MAKO_LOCAL_BUSY);
  EXPECT_EQ(mako_local_quarantined_worker_count(), quarantined_before + 1);

  mako_rust_fast_one_put_holder_view view{};
  ASSERT_EQ(mako_rust_fast_one_put_holder_pool_get_view(
                pool, sequence, &view),
            MAKO_LOCAL_OK);
  EXPECT_EQ(view.sequence, sequence);
  EXPECT_EQ(view.table_id, result.table_id);
  EXPECT_EQ(view.mako_timestamp, timestamp);
  EXPECT_EQ(view.value, result.staged_value);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(view.key),
                        view.key_len),
            key);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(view.value),
                        view.value_len),
            value);
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_destroy(pool), MAKO_LOCAL_BUSY);
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_release(pool, sequence),
            MAKO_LOCAL_OK);
  EXPECT_EQ(mako_rust_fast_one_put_holder_pool_destroy(pool), MAKO_LOCAL_OK);
}

struct CleanupFailureResult {
  int health_before = MAKO_LOCAL_INTERNAL;
  int attach = MAKO_LOCAL_INTERNAL;
  int db_open = MAKO_LOCAL_INTERNAL;
  int table_open = MAKO_LOCAL_INTERNAL;
  int begin = MAKO_LOCAL_INTERNAL;
  int setup = MAKO_LOCAL_INTERNAL;
  int invalid_arm = MAKO_LOCAL_INTERNAL;
  int arm = MAKO_LOCAL_INTERNAL;
  int second_arm = MAKO_LOCAL_INTERNAL;
  int boundary_status = MAKO_LOCAL_INTERNAL;
  int destroy_probe = MAKO_LOCAL_INTERNAL;
  int health_after = MAKO_LOCAL_INTERNAL;
  int health_again = MAKO_LOCAL_INTERNAL;
  int attach_after = MAKO_LOCAL_INTERNAL;
  int arm_after = MAKO_LOCAL_INTERNAL;
  int begin_after = MAKO_LOCAL_INTERNAL;
  int clear = MAKO_LOCAL_INTERNAL;
  int close = MAKO_LOCAL_INTERNAL;
  bool boundary_begin_out_was_null = false;
  bool operation_outputs_were_zeroed = false;
  bool begin_after_out_was_null = false;
};

CleanupFailureResult run_cleanup_failure(uint32_t boundary) {
  CleanupFailureResult result;
  std::thread worker([&] {
    result.health_before = mako_local_worker_health();
    result.attach = mako_local_thread_attach();

    mako_local_db *db = nullptr;
    if (result.attach == MAKO_LOCAL_OK)
      result.db_open = mako_local_db_open(&db);
    mako_local_table *table = nullptr;
    if (result.db_open == MAKO_LOCAL_OK) {
      const std::string name = "cleanup-failure-" +
                               std::to_string(boundary);
      result.table_open = mako_local_table_open(
          db, reinterpret_cast<const uint8_t *>(name.data()), name.size(),
          next_table_id.fetch_add(1), &table);
    }

    mako_local_txn *txn = nullptr;
    std::atomic<bool> conflict_writer_parked{false};
    std::atomic<bool> conflict_writer_release{false};
    std::atomic<bool> conflict_writer_done{false};
    struct ConflictWriterResult {
      int attach = MAKO_LOCAL_INTERNAL;
      int observer = MAKO_LOCAL_INTERNAL;
      int begin = MAKO_LOCAL_INTERNAL;
      int put = MAKO_LOCAL_INTERNAL;
      int commit = MAKO_LOCAL_INTERNAL;
      int destroy = MAKO_LOCAL_INTERNAL;
      int clear_observer = MAKO_LOCAL_INTERNAL;
    } conflict_writer_result;
    ParkingCommitObserver conflict_writer_observer{
        {}, &conflict_writer_parked, &conflict_writer_release};
    std::thread conflict_writer;

    if (boundary == MAKO_LOCAL_CLEANUP_BOUNDARY_BEGIN) {
      // Warm the worker's facade-storage cache first. The injected failure
      // must quarantine the reused facade and retain its database marker,
      // rather than return it to the cache as though cleanup succeeded.
      result.setup = result.table_open;
      mako_local_txn *warm = nullptr;
      if (result.setup == MAKO_LOCAL_OK)
        result.setup = mako_local_txn_begin(db, &warm);
      if (result.setup == MAKO_LOCAL_OK)
        result.setup = mako_local_txn_abort(warm);
      if (warm != nullptr) {
        const int warm_destroy = mako_local_txn_destroy(warm);
        if (result.setup == MAKO_LOCAL_OK)
          result.setup = warm_destroy;
      }

      result.invalid_arm = mako_local_test_arm_cleanup_failure(0);
      result.arm = mako_local_test_arm_cleanup_failure(boundary);
      result.second_arm = mako_local_test_arm_cleanup_failure(boundary);
      txn = reinterpret_cast<mako_local_txn *>(uintptr_t{1});
      if (result.setup == MAKO_LOCAL_OK)
        result.boundary_status = mako_local_txn_begin(db, &txn);
      result.boundary_begin_out_was_null = txn == nullptr;
      result.begin = result.boundary_status;
    } else {
      result.setup = result.table_open;
      if (boundary == MAKO_LOCAL_CLEANUP_BOUNDARY_OPERATION &&
          result.setup == MAKO_LOCAL_OK) {
        // Seed a stable value before the victim transaction observes it.
        // The second writer then parks with this key's native write lock held,
        // forcing MassTrans::transGet -> Sto::abort_without_throw -> stop().
        // This exercises engine-internal cleanup, not a facade budget check.
        mako_local_txn *seed = nullptr;
        result.setup = mako_local_txn_begin(db, &seed);
        uint8_t created = 0;
        if (result.setup == MAKO_LOCAL_OK)
          result.setup = mako_local_txn_put(
              seed, table,
              reinterpret_cast<const uint8_t *>("operation-conflict"), 18,
              reinterpret_cast<const uint8_t *>("initial"), 7, &created);
        if (result.setup == MAKO_LOCAL_OK)
          result.setup = mako_local_txn_commit(seed);
        if (seed != nullptr) {
          const int seed_destroy = mako_local_txn_destroy(seed);
          if (result.setup == MAKO_LOCAL_OK) result.setup = seed_destroy;
        }
      }

      if (result.setup == MAKO_LOCAL_OK)
        result.begin = mako_local_txn_begin(db, &txn);
      if (result.begin == MAKO_LOCAL_OK) {
        if (boundary == MAKO_LOCAL_CLEANUP_BOUNDARY_OPERATION) {
          uint8_t *value = nullptr;
          size_t value_len = 0;
          uint8_t found = 0;
          result.setup = mako_local_txn_get(
              txn, table,
              reinterpret_cast<const uint8_t *>("operation-conflict"), 18,
              &value, &value_len, &found);
          if (result.setup == MAKO_LOCAL_OK &&
              (found != 1 || value == nullptr || value_len != 7 ||
               std::string(reinterpret_cast<const char *>(value), value_len) !=
                   "initial")) {
            result.setup = MAKO_LOCAL_INTERNAL;
          }
          mako_local_bytes_free(value);

          if (result.setup == MAKO_LOCAL_OK) {
            conflict_writer = std::thread([&] {
              conflict_writer_result.attach = mako_local_thread_attach();
              if (conflict_writer_result.attach == MAKO_LOCAL_OK) {
                conflict_writer_result.observer =
                    mako_local_test_set_commit_observer(
                        park_after_writeset_lock, &conflict_writer_observer);
              }
              mako_local_txn *writer_txn = nullptr;
              if (conflict_writer_result.observer == MAKO_LOCAL_OK) {
                conflict_writer_result.begin =
                    mako_local_txn_begin(db, &writer_txn);
              }
              uint8_t writer_created = 0;
              if (conflict_writer_result.begin == MAKO_LOCAL_OK) {
                conflict_writer_result.put = mako_local_txn_put(
                    writer_txn, table,
                    reinterpret_cast<const uint8_t *>("operation-conflict"),
                    18, reinterpret_cast<const uint8_t *>("updated"), 7,
                    &writer_created);
              }
              if (conflict_writer_result.put == MAKO_LOCAL_OK) {
                conflict_writer_result.commit =
                    mako_local_txn_commit(writer_txn);
              }
              if (writer_txn != nullptr) {
                conflict_writer_result.destroy =
                    mako_local_txn_destroy(writer_txn);
              }
              if (conflict_writer_result.observer == MAKO_LOCAL_OK) {
                conflict_writer_result.clear_observer =
                    mako_local_test_clear_commit_observer();
              }
              conflict_writer_done.store(true, std::memory_order_release);
            });
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(5);
            while (!conflict_writer_parked.load(std::memory_order_acquire) &&
                   !conflict_writer_done.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
              std::this_thread::yield();
            }
            if (!conflict_writer_parked.load(std::memory_order_acquire)) {
              result.setup = MAKO_LOCAL_INTERNAL;
              conflict_writer_release.store(true, std::memory_order_release);
            }
          }
        } else {
          uint8_t created = 0;
          result.setup = mako_local_txn_put(
              txn, table, reinterpret_cast<const uint8_t *>("key"), 3,
              reinterpret_cast<const uint8_t *>("value"), 5, &created);
        }
      }

      result.invalid_arm = mako_local_test_arm_cleanup_failure(0);
      result.arm = mako_local_test_arm_cleanup_failure(boundary);
      result.second_arm = mako_local_test_arm_cleanup_failure(boundary);
      if (result.setup == MAKO_LOCAL_OK) {
        switch (boundary) {
          case MAKO_LOCAL_CLEANUP_BOUNDARY_OPERATION: {
            auto *const sentinel =
                reinterpret_cast<uint8_t *>(uintptr_t{1});
            uint8_t *value = sentinel;
            size_t value_len = std::numeric_limits<size_t>::max();
            uint8_t found = 99;
            result.boundary_status = mako_local_txn_get(
                txn, table,
                reinterpret_cast<const uint8_t *>("operation-conflict"), 18,
                &value, &value_len, &found);
            result.operation_outputs_were_zeroed =
                value == nullptr && value_len == 0 && found == 0;
            if (value != sentinel) mako_local_bytes_free(value);
            break;
          }
          case MAKO_LOCAL_CLEANUP_BOUNDARY_COMMIT:
            result.boundary_status = mako_local_txn_commit(txn);
            break;
          case MAKO_LOCAL_CLEANUP_BOUNDARY_ABORT:
            result.boundary_status = mako_local_txn_abort(txn);
            break;
          case MAKO_LOCAL_CLEANUP_BOUNDARY_DESTROY:
            result.boundary_status = mako_local_txn_destroy(txn);
            break;
          default:
            result.boundary_status = MAKO_LOCAL_INVALID_ARGUMENT;
            break;
        }
      }

      if (conflict_writer.joinable()) {
        conflict_writer_release.store(true, std::memory_order_release);
        conflict_writer.join();
        constexpr int expected_writer_statuses[] = {
            MAKO_LOCAL_OK, MAKO_LOCAL_OK, MAKO_LOCAL_OK, MAKO_LOCAL_OK,
            MAKO_LOCAL_OK, MAKO_LOCAL_OK, MAKO_LOCAL_OK};
        const int actual_writer_statuses[] = {
            conflict_writer_result.attach,
            conflict_writer_result.observer,
            conflict_writer_result.begin,
            conflict_writer_result.put,
            conflict_writer_result.commit,
            conflict_writer_result.destroy,
            conflict_writer_result.clear_observer};
        for (size_t i = 0; i != std::size(expected_writer_statuses); ++i) {
          if (result.setup == MAKO_LOCAL_OK &&
              actual_writer_statuses[i] != expected_writer_statuses[i]) {
            result.setup = actual_writer_statuses[i];
          }
        }
      }
      if (boundary != MAKO_LOCAL_CLEANUP_BOUNDARY_DESTROY && txn != nullptr)
        result.destroy_probe = mako_local_txn_destroy(txn);
    }

    result.health_after = mako_local_worker_health();
    result.health_again = mako_local_worker_health();
    result.attach_after = mako_local_thread_attach();
    result.arm_after = mako_local_test_arm_cleanup_failure(boundary);
    auto *after = reinterpret_cast<mako_local_txn *>(uintptr_t{1});
    result.begin_after = mako_local_txn_begin(db, &after);
    result.begin_after_out_was_null = after == nullptr;
    result.clear = mako_local_test_clear_cleanup_failure();
    result.close = mako_local_db_close(db);

    // `db`, its facade transaction, and native TLS are intentionally retained:
    // this sacrificial worker has demonstrated quarantine and must never run
    // native cleanup again.
  });
  worker.join();
  return result;
}

TEST(MakoLocalAbiCleanupFailure,
     EveryNativeCleanupBoundaryQuarantinesExactlyOnce) {
  constexpr std::array<uint32_t, 5> boundaries{
      MAKO_LOCAL_CLEANUP_BOUNDARY_BEGIN,
      MAKO_LOCAL_CLEANUP_BOUNDARY_OPERATION,
      MAKO_LOCAL_CLEANUP_BOUNDARY_COMMIT,
      MAKO_LOCAL_CLEANUP_BOUNDARY_ABORT,
      MAKO_LOCAL_CLEANUP_BOUNDARY_DESTROY,
  };
  const uint64_t quarantined_before =
      mako_local_quarantined_worker_count();

  for (size_t i = 0; i != boundaries.size(); ++i) {
    const uint32_t boundary = boundaries[i];
    SCOPED_TRACE(boundary);
    const CleanupFailureResult result = run_cleanup_failure(boundary);
    EXPECT_EQ(result.health_before, MAKO_LOCAL_NOT_ATTACHED);
    EXPECT_EQ(result.attach, MAKO_LOCAL_OK);
    EXPECT_EQ(result.db_open, MAKO_LOCAL_OK);
    EXPECT_EQ(result.table_open, MAKO_LOCAL_OK);
    EXPECT_EQ(result.setup, MAKO_LOCAL_OK);
    EXPECT_EQ(result.invalid_arm, MAKO_LOCAL_INVALID_ARGUMENT);
    EXPECT_EQ(result.arm, MAKO_LOCAL_OK);
    EXPECT_EQ(result.second_arm, MAKO_LOCAL_BUSY);
    EXPECT_EQ(result.boundary_status, MAKO_LOCAL_WORKER_POISONED);
    if (boundary == MAKO_LOCAL_CLEANUP_BOUNDARY_BEGIN) {
      EXPECT_TRUE(result.boundary_begin_out_was_null);
    } else {
      EXPECT_EQ(result.begin, MAKO_LOCAL_OK);
    }
    if (boundary == MAKO_LOCAL_CLEANUP_BOUNDARY_OPERATION)
      EXPECT_TRUE(result.operation_outputs_were_zeroed);
    if (boundary != MAKO_LOCAL_CLEANUP_BOUNDARY_BEGIN &&
        boundary != MAKO_LOCAL_CLEANUP_BOUNDARY_DESTROY) {
      EXPECT_EQ(result.destroy_probe, MAKO_LOCAL_WORKER_POISONED);
    }
    EXPECT_EQ(result.health_after, MAKO_LOCAL_WORKER_POISONED);
    EXPECT_EQ(result.health_again, MAKO_LOCAL_WORKER_POISONED);
    EXPECT_EQ(result.attach_after, MAKO_LOCAL_WORKER_POISONED);
    EXPECT_EQ(result.arm_after, MAKO_LOCAL_WORKER_POISONED);
    EXPECT_EQ(result.begin_after, MAKO_LOCAL_WORKER_POISONED);
    EXPECT_TRUE(result.begin_after_out_was_null);
    EXPECT_EQ(result.clear, MAKO_LOCAL_WORKER_POISONED);
    EXPECT_EQ(result.close, MAKO_LOCAL_BUSY);
    EXPECT_EQ(mako_local_quarantined_worker_count(),
              quarantined_before + i + 1);
  }
}

TEST_F(LocalAbiTest, CleanupFailureArmCanBeClearedWithoutPoisoning) {
  const uint64_t quarantined_before = mako_local_quarantined_worker_count();
  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "clear-cleanup-failure", "value"),
            MAKO_LOCAL_OK);
  ASSERT_EQ(mako_local_test_arm_cleanup_failure(
                MAKO_LOCAL_CLEANUP_BOUNDARY_ABORT),
            MAKO_LOCAL_OK);
  ASSERT_EQ(mako_local_test_clear_cleanup_failure(), MAKO_LOCAL_OK);
  abort_and_destroy(txn);

  EXPECT_EQ(mako_local_worker_health(), MAKO_LOCAL_OK);
  EXPECT_EQ(mako_local_quarantined_worker_count(), quarantined_before);
}

TEST_F(LocalAbiTest,
       TestCommitObserverReportsOrderedSeamsAndCanBeCleared) {
  ASSERT_NE(mako_local_feature_bits() &
                MAKO_LOCAL_FEATURE_TEST_COMMIT_OBSERVER,
            0U);
  ASSERT_EQ(mako_local_test_set_commit_observer(record_commit_phase,
                                                &commit_observation),
            MAKO_LOCAL_OK);
  EXPECT_EQ(mako_local_test_set_commit_observer(record_commit_phase,
                                                &commit_observation),
            MAKO_LOCAL_BUSY);
  EXPECT_EQ(mako_local_test_set_commit_observer(nullptr, nullptr),
            MAKO_LOCAL_INVALID_ARGUMENT);

  HookObservation hook;
  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "observer-a", "one"), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, "observer-b", "two"), MAKO_LOCAL_OK);
  EXPECT_EQ(mako_local_txn_commit_with_hook(txn, accept_hook, &hook),
            MAKO_LOCAL_OK);
  destroy_tracked(txn);

  constexpr std::array<uint32_t, 6> expected_phases{
      MAKO_LOCAL_TEST_COMMIT_WRITESET_LOCKED,
      MAKO_LOCAL_TEST_COMMIT_MAKO_TIMESTAMP_ALLOCATED,
      MAKO_LOCAL_TEST_COMMIT_LOCAL_VALIDATION_COMPLETE,
      MAKO_LOCAL_TEST_COMMIT_PREINSTALL_ACCEPTED,
      MAKO_LOCAL_TEST_COMMIT_FIRST_WRITE_INSTALLED,
      MAKO_LOCAL_TEST_COMMIT_ALL_WRITES_INSTALLED,
  };
  ASSERT_EQ(commit_observation.calls, expected_phases.size());
  for (size_t i = 0; i != expected_phases.size(); ++i)
    EXPECT_EQ(commit_observation.phases[i], expected_phases[i]) << i;
  EXPECT_EQ(commit_observation.timestamps[0], 0U);
  ASSERT_NE(hook.timestamp, 0U);
  for (size_t i = 1; i != expected_phases.size(); ++i)
    EXPECT_EQ(commit_observation.timestamps[i], hook.timestamp) << i;

  // The observer itself requests the same checked Mako timestamp even when the
  // caller uses ordinary local commit without a durability hook. A single
  // write omits only the mid-install phase.
  commit_observation.reset();
  txn = begin();
  ASSERT_EQ(put(txn, primary, "observer-single", "single"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);
  constexpr std::array<uint32_t, 5> expected_single_write_phases{
      MAKO_LOCAL_TEST_COMMIT_WRITESET_LOCKED,
      MAKO_LOCAL_TEST_COMMIT_MAKO_TIMESTAMP_ALLOCATED,
      MAKO_LOCAL_TEST_COMMIT_LOCAL_VALIDATION_COMPLETE,
      MAKO_LOCAL_TEST_COMMIT_PREINSTALL_ACCEPTED,
      MAKO_LOCAL_TEST_COMMIT_ALL_WRITES_INSTALLED,
  };
  ASSERT_EQ(commit_observation.calls, expected_single_write_phases.size());
  for (size_t i = 0; i != expected_single_write_phases.size(); ++i)
    EXPECT_EQ(commit_observation.phases[i], expected_single_write_phases[i])
        << i;
  EXPECT_EQ(commit_observation.timestamps[0], 0U);
  EXPECT_NE(commit_observation.timestamps[1], 0U);
  for (size_t i = 2; i != expected_single_write_phases.size(); ++i)
    EXPECT_EQ(commit_observation.timestamps[i],
              commit_observation.timestamps[1]) << i;

  // A read-only transaction reaches none of the write-commit seams, and its
  // durability hook remains uncalled as well.
  commit_observation.reset();
  HookObservation read_only_hook;
  txn = begin();
  EXPECT_EQ(get(txn, primary, "observer-a").second,
            std::optional<std::string>("one"));
  EXPECT_EQ(mako_local_txn_commit_with_hook(
                txn, accept_hook, &read_only_hook),
            MAKO_LOCAL_OK);
  destroy_tracked(txn);
  EXPECT_EQ(commit_observation.calls, 0U);
  EXPECT_EQ(read_only_hook.calls, 0);

  ASSERT_EQ(mako_local_test_clear_commit_observer(), MAKO_LOCAL_OK);
  EXPECT_EQ(mako_local_test_clear_commit_observer(), MAKO_LOCAL_OK);

  txn = begin();
  ASSERT_EQ(put(txn, primary, "observer-cleared", "three"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);
  EXPECT_EQ(commit_observation.calls, 0U);
}

TEST_F(LocalAbiTest, TestCommitObserverStopsBeforeRejectedPreinstall) {
  ASSERT_EQ(mako_local_test_set_commit_observer(record_commit_phase,
                                                &commit_observation),
            MAKO_LOCAL_OK);
  HookObservation hook;
  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "observer-reject", "invisible"), MAKO_LOCAL_OK);
  EXPECT_EQ(mako_local_txn_commit_with_hook(txn, reject_hook, &hook),
            MAKO_LOCAL_COMMIT_HOOK_REJECTED);
  destroy_tracked(txn);

  ASSERT_EQ(commit_observation.calls, 3U);
  EXPECT_EQ(commit_observation.phases[0],
            MAKO_LOCAL_TEST_COMMIT_WRITESET_LOCKED);
  EXPECT_EQ(commit_observation.phases[1],
            MAKO_LOCAL_TEST_COMMIT_MAKO_TIMESTAMP_ALLOCATED);
  EXPECT_EQ(commit_observation.phases[2],
            MAKO_LOCAL_TEST_COMMIT_LOCAL_VALIDATION_COMPLETE);
  EXPECT_EQ(commit_observation.timestamps[0], 0U);
  EXPECT_NE(commit_observation.timestamps[1], 0U);
  EXPECT_EQ(commit_observation.timestamps[1],
            commit_observation.timestamps[2]);
  EXPECT_EQ(commit_observation.timestamps[2], hook.timestamp);
  EXPECT_EQ(mako_local_test_clear_commit_observer(), MAKO_LOCAL_OK);
}

TEST_F(LocalAbiTest, LockedWriteConflictDoesNotReportACommitSeam) {
  auto *seed = begin();
  ASSERT_EQ(put(seed, primary, "observer-contended", "zero"), MAKO_LOCAL_OK);
  commit_and_destroy(seed);

  struct WorkerResult {
    int attach = MAKO_LOCAL_INTERNAL;
    int observer_set = MAKO_LOCAL_INTERNAL;
    int begin = MAKO_LOCAL_INTERNAL;
    int put = MAKO_LOCAL_INTERNAL;
    int commit = MAKO_LOCAL_INTERNAL;
    int destroy = MAKO_LOCAL_INTERNAL;
    int observer_clear = MAKO_LOCAL_INTERNAL;
    HookObservation hook;
    CommitPhaseObservation phases;
  } winner, loser;

  std::atomic<bool> parked{false};
  std::atomic<bool> release{false};
  ParkingCommitObserver parking{{}, &parked, &release};

  std::thread locking_worker([&] {
    winner.attach = mako_local_thread_attach();
    if (winner.attach == MAKO_LOCAL_OK)
      winner.observer_set = mako_local_test_set_commit_observer(
          park_after_writeset_lock, &parking);
    mako_local_txn *txn = nullptr;
    if (winner.observer_set == MAKO_LOCAL_OK)
      winner.begin = mako_local_txn_begin(db, &txn);
    if (winner.begin == MAKO_LOCAL_OK)
      winner.put = put(txn, primary, "observer-contended", "winner");
    if (winner.put == MAKO_LOCAL_OK)
      winner.commit = mako_local_txn_commit_with_hook(
          txn, accept_hook, &winner.hook);
    if (txn != nullptr)
      winner.destroy = mako_local_txn_destroy(txn);
    winner.observer_clear = mako_local_test_clear_commit_observer();
  });

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(5);
  while (!parked.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  if (!parked.load(std::memory_order_acquire)) {
    release.store(true, std::memory_order_release);
    locking_worker.join();
    FAIL() << "winning commit did not reach the write-set-locked seam";
    return;
  }

  std::thread conflicting_worker([&] {
    loser.attach = mako_local_thread_attach();
    if (loser.attach == MAKO_LOCAL_OK)
      loser.observer_set = mako_local_test_set_commit_observer(
          record_commit_phase, &loser.phases);
    mako_local_txn *txn = nullptr;
    if (loser.observer_set == MAKO_LOCAL_OK)
      loser.begin = mako_local_txn_begin(db, &txn);
    if (loser.begin == MAKO_LOCAL_OK)
      loser.put = put(txn, primary, "observer-contended", "loser");
    if (loser.put == MAKO_LOCAL_OK)
      loser.commit = mako_local_txn_commit_with_hook(
          txn, accept_hook, &loser.hook);
    if (txn != nullptr)
      loser.destroy = mako_local_txn_destroy(txn);
    loser.observer_clear = mako_local_test_clear_commit_observer();
  });
  conflicting_worker.join();
  release.store(true, std::memory_order_release);
  locking_worker.join();

  EXPECT_EQ(winner.attach, MAKO_LOCAL_OK);
  EXPECT_EQ(winner.observer_set, MAKO_LOCAL_OK);
  EXPECT_EQ(winner.begin, MAKO_LOCAL_OK);
  EXPECT_EQ(winner.put, MAKO_LOCAL_OK);
  EXPECT_EQ(winner.commit, MAKO_LOCAL_OK);
  EXPECT_EQ(winner.destroy, MAKO_LOCAL_OK);
  EXPECT_EQ(winner.observer_clear, MAKO_LOCAL_OK);
  EXPECT_EQ(parking.observation.calls, 5U);

  EXPECT_EQ(loser.attach, MAKO_LOCAL_OK);
  EXPECT_EQ(loser.observer_set, MAKO_LOCAL_OK);
  EXPECT_EQ(loser.begin, MAKO_LOCAL_OK);
  // MassTrans detects this exact held-version conflict while staging the put,
  // so the worker deliberately never enters commit at all.
  EXPECT_EQ(loser.put, MAKO_LOCAL_CONFLICT);
  EXPECT_EQ(loser.commit, MAKO_LOCAL_INTERNAL);
  EXPECT_EQ(loser.destroy, MAKO_LOCAL_OK);
  EXPECT_EQ(loser.observer_clear, MAKO_LOCAL_OK);
  EXPECT_EQ(loser.phases.calls, 0U);
  EXPECT_EQ(loser.hook.calls, 0);
}

TEST_F(LocalAbiTest, LockedRowsMakeBothScanDirectionsReportConflict) {
  const std::string key = "scan-locked-row";
  auto *seed = begin();
  ASSERT_EQ(put(seed, primary, key, "before"), MAKO_LOCAL_OK);
  commit_and_destroy(seed);

  struct WriterResult {
    int attach = MAKO_LOCAL_INTERNAL;
    int observer_set = MAKO_LOCAL_INTERNAL;
    int begin = MAKO_LOCAL_INTERNAL;
    int put = MAKO_LOCAL_INTERNAL;
    int commit = MAKO_LOCAL_INTERNAL;
    int destroy = MAKO_LOCAL_INTERNAL;
    int observer_clear = MAKO_LOCAL_INTERNAL;
  } writer_result;
  std::atomic<bool> parked{false};
  std::atomic<bool> release{false};
  ParkingCommitObserver parking{{}, &parked, &release};

  std::thread writer([&] {
    writer_result.attach = mako_local_thread_attach();
    if (writer_result.attach == MAKO_LOCAL_OK) {
      writer_result.observer_set = mako_local_test_set_commit_observer(
          park_after_writeset_lock, &parking);
    }
    mako_local_txn *txn = nullptr;
    if (writer_result.observer_set == MAKO_LOCAL_OK)
      writer_result.begin = mako_local_txn_begin(db, &txn);
    if (writer_result.begin == MAKO_LOCAL_OK)
      writer_result.put = put(txn, primary, key, "after");
    if (writer_result.put == MAKO_LOCAL_OK)
      writer_result.commit = mako_local_txn_commit(txn);
    if (txn != nullptr)
      writer_result.destroy = mako_local_txn_destroy(txn);
    writer_result.observer_clear = mako_local_test_clear_commit_observer();
  });

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(5);
  while (!parked.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  if (!parked.load(std::memory_order_acquire)) {
    release.store(true, std::memory_order_release);
    writer.join();
    FAIL() << "writer did not reach the write-set-locked seam";
    return;
  }

  std::string upper = key;
  upper.push_back('\0');
  const mako_local_scan_options options{
      MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE, MAKO_LOCAL_SCAN_HAS_UPPER,
      reinterpret_cast<const uint8_t *>(key.data()), key.size(),
      reinterpret_cast<const uint8_t *>(upper.data()), upper.size(), nullptr,
      0};
  for (const bool reverse : {false, true}) {
    SCOPED_TRACE(reverse ? "reverse" : "forward");
    auto *txn = begin();
    const auto result = scan_chunk(txn, primary, options, reverse, 2, 128);
    EXPECT_EQ(result.status, MAKO_LOCAL_CONFLICT);
    EXPECT_TRUE(result.entries.empty());
    EXPECT_EQ(result.arena_used, 0U);
    EXPECT_EQ(result.arena_required, 0U);
    EXPECT_EQ(result.done, 0U);
    destroy_tracked(txn);
  }

  // Both exception paths completed native cleanup, so this worker can commit
  // a disjoint transaction even while the writer remains parked.
  auto *recovery = begin();
  const int recovery_put = recovery == nullptr
      ? MAKO_LOCAL_INTERNAL
      : put(recovery, primary, "scan-conflict-recovery", "ok");
  const int recovery_commit = recovery_put == MAKO_LOCAL_OK
      ? mako_local_txn_commit(recovery)
      : MAKO_LOCAL_INTERNAL;
  const int recovery_destroy = recovery == nullptr
      ? MAKO_LOCAL_INTERNAL
      : mako_local_txn_destroy(recovery);
  if (recovery_destroy == MAKO_LOCAL_OK && txn_for_cleanup == recovery)
    txn_for_cleanup = nullptr;

  release.store(true, std::memory_order_release);
  writer.join();
  EXPECT_EQ(recovery_put, MAKO_LOCAL_OK);
  EXPECT_EQ(recovery_commit, MAKO_LOCAL_OK);
  EXPECT_EQ(recovery_destroy, MAKO_LOCAL_OK);
  EXPECT_EQ(writer_result.attach, MAKO_LOCAL_OK);
  EXPECT_EQ(writer_result.observer_set, MAKO_LOCAL_OK);
  EXPECT_EQ(writer_result.begin, MAKO_LOCAL_OK);
  EXPECT_EQ(writer_result.put, MAKO_LOCAL_OK);
  EXPECT_EQ(writer_result.commit, MAKO_LOCAL_OK);
  EXPECT_EQ(writer_result.destroy, MAKO_LOCAL_OK);
  EXPECT_EQ(writer_result.observer_clear, MAKO_LOCAL_OK);

  auto *verify = begin();
  EXPECT_EQ(get(verify, primary, key).second,
            std::optional<std::string>("after"));
  EXPECT_EQ(get(verify, primary, "scan-conflict-recovery").second,
            std::optional<std::string>("ok"));
  commit_and_destroy(verify);
}

TEST_F(LocalAbiTest, DirectComparatorPreservesReadsAndPropagatesConflicts) {
  auto &table = direct_comparator_table();
  const std::string suffix = "-" +
      std::to_string(next_table_id.fetch_add(1, std::memory_order_relaxed));
  const std::string compared_key = "direct-comparator-key" + suffix;
  const std::string side_key = "direct-comparator-side" + suffix;
  const std::string insert_key = "direct-comparator-insert" + suffix;

  auto direct_put = [&](const std::string &key,
                        const std::string &value) {
    Sto::start_transaction();
    (void)table.transPut(lcdf::Str(key), mako::Encode(value));
    return Sto::try_commit_no_paxos();
  };
  auto direct_get = [&](const std::string &key) {
    Sto::start_transaction();
    std::string value;
    const bool found = table.transGet(lcdf::Str(key), value);
    const bool committed = Sto::try_commit_no_paxos();
    return std::tuple{found, committed, std::move(value)};
  };

  ASSERT_TRUE(direct_put(compared_key, "initial"));
  ASSERT_TRUE(direct_put(side_key, "side-initial"));

  // Predicate-false is a transactional read. A concurrent update after the
  // comparison must invalidate the transaction and keep its disjoint write
  // invisible.
  direct_comparator_calls.store(0, std::memory_order_relaxed);
  Sto::start_transaction();
  const bool predicate_result = table.transPutMbta(
      lcdf::Str(compared_key), mako::Encode("ignored"),
      direct_comparator_false);
  (void)table.transPut(lcdf::Str(side_key), mako::Encode("must-abort"));
  struct DirectWriterResult {
    int attach = MAKO_LOCAL_INTERNAL;
    bool staged = false;
    bool committed = false;
    int observer_set = MAKO_LOCAL_INTERNAL;
    int observer_clear = MAKO_LOCAL_INTERNAL;
  } concurrent_writer;
  std::thread updater([&] {
    concurrent_writer.attach = mako_local_thread_attach();
    if (concurrent_writer.attach != MAKO_LOCAL_OK) return;
    try {
      Sto::start_transaction();
      concurrent_writer.staged = table.transPut(
          lcdf::Str(compared_key), mako::Encode("concurrent"));
      concurrent_writer.committed = Sto::try_commit_no_paxos();
    } catch (...) {
      Sto::silent_abort();
    }
  });
  updater.join();
  const bool stale_commit = Sto::try_commit_no_paxos();
  EXPECT_FALSE(predicate_result);
  EXPECT_EQ(direct_comparator_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(concurrent_writer.attach, MAKO_LOCAL_OK);
  EXPECT_TRUE(concurrent_writer.staged);
  EXPECT_TRUE(concurrent_writer.committed);
  EXPECT_FALSE(stale_commit);
  {
    const auto [found, committed, value] = direct_get(side_key);
    EXPECT_TRUE(found);
    EXPECT_TRUE(committed);
    EXPECT_EQ(value, mako::Encode("side-initial"));
  }

  // Without an intervening writer, false remains a normal no-op and the same
  // transaction can commit another key.
  direct_comparator_calls.store(0, std::memory_order_relaxed);
  Sto::start_transaction();
  EXPECT_FALSE(table.transPutMbta(
      lcdf::Str(compared_key), mako::Encode("still-ignored"),
      direct_comparator_false));
  (void)table.transPut(lcdf::Str(side_key), mako::Encode("side-committed"));
  EXPECT_TRUE(Sto::try_commit_no_paxos());
  EXPECT_EQ(direct_comparator_calls.load(std::memory_order_relaxed), 1);

  // An insert followed by the replay comparator reads the transaction's own
  // private invalid row instead of treating its invalid bit as a conflict.
  direct_comparator_calls.store(0, std::memory_order_relaxed);
  Sto::start_transaction();
  EXPECT_FALSE(table.transInsert(lcdf::Str(insert_key),
                                 mako::Encode("private")));
  EXPECT_TRUE(table.transPutMbta(
      lcdf::Str(insert_key), mako::Encode("updated"),
      direct_comparator_true));
  EXPECT_TRUE(Sto::try_commit_no_paxos());
  EXPECT_EQ(direct_comparator_calls.load(std::memory_order_relaxed), 1);

  // Park a writer with the record lock held. The comparator must not run, the
  // soft-abort marker must be consumed by direct conflict propagation, and the
  // caller's worker must remain immediately reusable.
  std::atomic<bool> parked{false};
  std::atomic<bool> release{false};
  ParkingCommitObserver parking{{}, &parked, &release};
  DirectWriterResult locking_writer;
  std::thread locker([&] {
    locking_writer.attach = mako_local_thread_attach();
    if (locking_writer.attach == MAKO_LOCAL_OK) {
      locking_writer.observer_set = mako_local_test_set_commit_observer(
          park_after_writeset_lock, &parking);
    }
    if (locking_writer.observer_set == MAKO_LOCAL_OK) {
      try {
        Sto::start_transaction();
        locking_writer.staged = table.transPut(
            lcdf::Str(compared_key), mako::Encode("lock-winner"));
        locking_writer.committed = Sto::try_commit_no_paxos();
      } catch (...) {
        Sto::silent_abort();
      }
    }
    locking_writer.observer_clear = mako_local_test_clear_commit_observer();
  });

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(5);
  while (!parked.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  if (!parked.load(std::memory_order_acquire)) {
    release.store(true, std::memory_order_release);
    locker.join();
    FAIL() << "direct writer did not reach the write-set-locked seam";
    return;
  }

  direct_comparator_calls.store(0, std::memory_order_relaxed);
  bool conflict_threw = false;
  Sto::start_transaction();
  try {
    (void)table.transPutMbta(lcdf::Str(compared_key),
                             mako::Encode("must-not-stage"),
                             direct_comparator_true);
  } catch (const Transaction::Abort &) {
    conflict_threw = true;
  }
  if (Sto::in_progress()) Sto::silent_abort();
  const bool marker_cleared = !TThread::transget_without_throw;

  bool recovery_commit = false;
  bool recovery_threw = false;
  try {
    recovery_commit = direct_put(side_key, "side-recovery");
  } catch (...) {
    recovery_threw = true;
    Sto::silent_abort();
  }
  release.store(true, std::memory_order_release);
  locker.join();
  EXPECT_TRUE(conflict_threw);
  EXPECT_TRUE(marker_cleared);
  EXPECT_EQ(direct_comparator_calls.load(std::memory_order_relaxed), 0);
  EXPECT_FALSE(recovery_threw);
  EXPECT_TRUE(recovery_commit);
  EXPECT_EQ(locking_writer.attach, MAKO_LOCAL_OK);
  EXPECT_EQ(locking_writer.observer_set, MAKO_LOCAL_OK);
  EXPECT_TRUE(locking_writer.staged);
  EXPECT_TRUE(locking_writer.committed);
  EXPECT_EQ(locking_writer.observer_clear, MAKO_LOCAL_OK);

  direct_comparator_calls.store(0, std::memory_order_relaxed);
  Sto::start_transaction();
  EXPECT_TRUE(table.transPutMbta(
      lcdf::Str(compared_key), mako::Encode("retry-winner"),
      direct_comparator_true));
  EXPECT_TRUE(Sto::try_commit_no_paxos());
  EXPECT_EQ(direct_comparator_calls.load(std::memory_order_relaxed), 1);

  {
    const auto [found, committed, value] = direct_get(compared_key);
    EXPECT_TRUE(found);
    EXPECT_TRUE(committed);
    EXPECT_EQ(value, mako::Encode("retry-winner"));
  }
  {
    const auto [found, committed, value] = direct_get(insert_key);
    EXPECT_TRUE(found);
    EXPECT_TRUE(committed);
    EXPECT_EQ(value, mako::Encode("updated"));
  }
}
#endif

TEST_F(LocalAbiTest, TableNamesAndNumericIdsAreBothUnique) {
  const std::string name = "identity";
  const std::string other_name = "identity-other";
  const uint64_t id = next_table_id.fetch_add(1);
  mako_local_table *first = nullptr;
  ASSERT_EQ(mako_local_table_open(
                db, reinterpret_cast<const uint8_t *>(name.data()), name.size(),
                id, &first),
            MAKO_LOCAL_OK);
  ASSERT_NE(first, nullptr);

  mako_local_table *same = nullptr;
  EXPECT_EQ(mako_local_table_open(
                db, reinterpret_cast<const uint8_t *>(name.data()), name.size(),
                id, &same),
            MAKO_LOCAL_OK);
  EXPECT_EQ(same, first);

  auto *poison = reinterpret_cast<mako_local_table *>(uintptr_t{1});
  EXPECT_EQ(mako_local_table_open(
                db, reinterpret_cast<const uint8_t *>(name.data()), name.size(),
                id + 1, &poison),
            MAKO_LOCAL_WRONG_DB_OR_TABLE);
  EXPECT_EQ(poison, nullptr);

  poison = reinterpret_cast<mako_local_table *>(uintptr_t{1});
  EXPECT_EQ(mako_local_table_open(
                db, reinterpret_cast<const uint8_t *>(other_name.data()),
                other_name.size(), id, &poison),
            MAKO_LOCAL_WRONG_DB_OR_TABLE);
  EXPECT_EQ(poison, nullptr);
}

TEST_F(LocalAbiTest, ConcurrentTableCreationSerializesIdentityMapping) {
  struct OpenResult {
    int attach = MAKO_LOCAL_INTERNAL;
    int open = MAKO_LOCAL_INTERNAL;
    mako_local_table *table = nullptr;
  };

  auto race = [&](const std::array<std::string, 2> &names,
                  const std::array<uint64_t, 2> &ids) {
    std::array<OpenResult, 2> results;
    std::barrier ready(3);
    std::array<std::thread, 2> workers;
    for (size_t worker = 0; worker != workers.size(); ++worker) {
      workers[worker] = std::thread([&, worker] {
        results[worker].attach = mako_local_thread_attach();
        ready.arrive_and_wait();
        if (results[worker].attach == MAKO_LOCAL_OK) {
          results[worker].open = mako_local_table_open(
              db, reinterpret_cast<const uint8_t *>(names[worker].data()),
              names[worker].size(), ids[worker], &results[worker].table);
        }
      });
    }
    ready.arrive_and_wait();
    for (auto &worker : workers) worker.join();
    return results;
  };

  const uint64_t same_id = next_table_id.fetch_add(1);
  auto same = race({"concurrent-same", "concurrent-same"},
                   {same_id, same_id});
  EXPECT_EQ(same[0].attach, MAKO_LOCAL_OK);
  EXPECT_EQ(same[1].attach, MAKO_LOCAL_OK);
  EXPECT_EQ(same[0].open, MAKO_LOCAL_OK);
  EXPECT_EQ(same[1].open, MAKO_LOCAL_OK);
  EXPECT_NE(same[0].table, nullptr);
  EXPECT_EQ(same[0].table, same[1].table);

  const uint64_t name_conflict_id = next_table_id.fetch_add(2);
  auto name_conflict = race(
      {"concurrent-name-conflict", "concurrent-name-conflict"},
      {name_conflict_id, name_conflict_id + 1});
  EXPECT_EQ(name_conflict[0].attach, MAKO_LOCAL_OK);
  EXPECT_EQ(name_conflict[1].attach, MAKO_LOCAL_OK);
  const int name_successes =
      (name_conflict[0].open == MAKO_LOCAL_OK ? 1 : 0) +
      (name_conflict[1].open == MAKO_LOCAL_OK ? 1 : 0);
  const int name_conflicts =
      (name_conflict[0].open == MAKO_LOCAL_WRONG_DB_OR_TABLE ? 1 : 0) +
      (name_conflict[1].open == MAKO_LOCAL_WRONG_DB_OR_TABLE ? 1 : 0);
  EXPECT_EQ(name_successes, 1);
  EXPECT_EQ(name_conflicts, 1);

  const uint64_t id_conflict = next_table_id.fetch_add(1);
  auto numeric_conflict = race(
      {"concurrent-id-left", "concurrent-id-right"},
      {id_conflict, id_conflict});
  EXPECT_EQ(numeric_conflict[0].attach, MAKO_LOCAL_OK);
  EXPECT_EQ(numeric_conflict[1].attach, MAKO_LOCAL_OK);
  const int numeric_successes =
      (numeric_conflict[0].open == MAKO_LOCAL_OK ? 1 : 0) +
      (numeric_conflict[1].open == MAKO_LOCAL_OK ? 1 : 0);
  const int numeric_conflicts =
      (numeric_conflict[0].open == MAKO_LOCAL_WRONG_DB_OR_TABLE ? 1 : 0) +
      (numeric_conflict[1].open == MAKO_LOCAL_WRONG_DB_OR_TABLE ? 1 : 0);
  EXPECT_EQ(numeric_successes, 1);
  EXPECT_EQ(numeric_conflicts, 1);
}

TEST_F(LocalAbiTest, ExactMaximumValueCommitsAndAbortsWithoutResizeLeakage) {
  const std::string maximum(MAKO_LOCAL_MAX_VALUE_BYTES, 'm');

  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "max-value-abort", maximum), MAKO_LOCAL_OK);
#if READ_MY_WRITES
  ASSERT_EQ(get(txn, primary, "max-value-abort").second,
            std::optional<std::string>(maximum));
#endif
  abort_and_destroy(txn);

  txn = begin();
  EXPECT_FALSE(get(txn, primary, "max-value-abort").second.has_value());
  commit_and_destroy(txn);

  txn = begin();
  ASSERT_EQ(put(txn, primary, "max-value-commit", maximum), MAKO_LOCAL_OK);
  commit_and_destroy(txn);

  txn = begin();
  ASSERT_EQ(get(txn, primary, "max-value-commit").second,
            std::optional<std::string>(maximum));
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, OversizedInputsAreNonterminalAndLeaveOutputsInitialized) {
  const std::string oversized_name(MAKO_LOCAL_MAX_TABLE_NAME_BYTES + 1, 'n');
  auto *table_poison = reinterpret_cast<mako_local_table *>(uintptr_t{1});
  EXPECT_EQ(mako_local_table_open(
                db, reinterpret_cast<const uint8_t *>(oversized_name.data()),
                oversized_name.size(), next_table_id.fetch_add(1),
                &table_poison),
            MAKO_LOCAL_VALUE_TOO_LARGE);
  EXPECT_EQ(table_poison, nullptr);

  const std::string oversized_key(MAKO_LOCAL_MAX_KEY_BYTES + 1, 'k');
  const std::string oversized_value(MAKO_LOCAL_MAX_VALUE_BYTES + 1, 'v');
  auto *txn = begin();
  uint8_t created = 99;
  EXPECT_EQ(put(txn, primary, oversized_key, "value", &created),
            MAKO_LOCAL_VALUE_TOO_LARGE);
  EXPECT_EQ(created, 0);
  created = 99;
  EXPECT_EQ(put(txn, primary, "large-value", oversized_value, &created),
            MAKO_LOCAL_VALUE_TOO_LARGE);
  EXPECT_EQ(created, 0);

  // VALUE_TOO_LARGE is validation-only: no native operation ran and the
  // transaction remains usable.
  EXPECT_EQ(put(txn, primary, "after-large", "works", &created),
            MAKO_LOCAL_OK);
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, PointFailuresOverwriteEveryValidOutputSentinel) {
  auto *txn = begin();
  const auto *key = reinterpret_cast<const uint8_t *>("key");
  const auto *value = reinterpret_cast<const uint8_t *>("value");

  auto *bytes = reinterpret_cast<uint8_t *>(uintptr_t{1});
  size_t value_len = std::numeric_limits<size_t>::max();
  uint8_t found = UINT8_MAX;
  EXPECT_EQ(
      mako_local_txn_get(txn, nullptr, key, 3, &bytes, &value_len, &found),
      MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(bytes, nullptr);
  EXPECT_EQ(value_len, 0U);
  EXPECT_EQ(found, 0U);

  uint8_t changed = UINT8_MAX;
  EXPECT_EQ(mako_local_txn_put(txn, nullptr, key, 3, value, 5, &changed),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(changed, 0U);

  changed = UINT8_MAX;
  EXPECT_EQ(mako_local_txn_insert(txn, nullptr, key, 3, value, 5, &changed),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(changed, 0U);

  changed = UINT8_MAX;
  EXPECT_EQ(mako_local_txn_remove(txn, nullptr, key, 3, &changed),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(changed, 0U);

  // Invalid-argument failures are nonterminal when native state was not
  // touched, so the same transaction must remain usable.
  ASSERT_EQ(put(txn, primary, "after-output-failures", "works"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, PartiallyNullOutputSetsLeaveEveryOtherSentinelUntouched) {
  auto *txn = begin();
  const auto *key = reinterpret_cast<const uint8_t *>("key");

  for (size_t missing = 0; missing != 3; ++missing) {
    SCOPED_TRACE(::testing::Message() << "get missing output " << missing);
    auto *bytes = reinterpret_cast<uint8_t *>(uintptr_t{1});
    size_t value_len = std::numeric_limits<size_t>::max();
    uint8_t found = UINT8_MAX;
    EXPECT_EQ(mako_local_txn_get(txn, primary, key, 3,
                                 missing == 0 ? nullptr : &bytes,
                                 missing == 1 ? nullptr : &value_len,
                                 missing == 2 ? nullptr : &found),
              MAKO_LOCAL_INVALID_ARGUMENT);
    if (missing != 0)
      EXPECT_EQ(bytes, reinterpret_cast<uint8_t *>(uintptr_t{1}));
    if (missing != 1)
      EXPECT_EQ(value_len, std::numeric_limits<size_t>::max());
    if (missing != 2)
      EXPECT_EQ(found, UINT8_MAX);
  }

  const mako_local_scan_options options{
      MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE, 0, nullptr, 0,
      nullptr, 0, nullptr, 0};
  mako_local_scan_entry descriptor{};
  uint8_t arena[1]{};
  for (const bool reverse : {false, true}) {
    for (size_t missing = 0; missing != 4; ++missing) {
      SCOPED_TRACE(::testing::Message()
                   << (reverse ? "reverse" : "forward")
                   << " scan missing output " << missing);
      size_t count = std::numeric_limits<size_t>::max();
      size_t used = std::numeric_limits<size_t>::max();
      size_t required = std::numeric_limits<size_t>::max();
      uint8_t done = UINT8_MAX;
      const int status =
          reverse
              ? mako_local_txn_rscan_chunk(
                    txn, primary, &options, &descriptor, 1, arena,
                    sizeof(arena), missing == 0 ? nullptr : &count,
                    missing == 1 ? nullptr : &used,
                    missing == 2 ? nullptr : &required,
                    missing == 3 ? nullptr : &done)
              : mako_local_txn_scan_chunk(
                    txn, primary, &options, &descriptor, 1, arena,
                    sizeof(arena), missing == 0 ? nullptr : &count,
                    missing == 1 ? nullptr : &used,
                    missing == 2 ? nullptr : &required,
                    missing == 3 ? nullptr : &done);
      EXPECT_EQ(status, MAKO_LOCAL_INVALID_ARGUMENT);
      if (missing != 0)
        EXPECT_EQ(count, std::numeric_limits<size_t>::max());
      if (missing != 1)
        EXPECT_EQ(used, std::numeric_limits<size_t>::max());
      if (missing != 2)
        EXPECT_EQ(required, std::numeric_limits<size_t>::max());
      if (missing != 3)
        EXPECT_EQ(done, UINT8_MAX);
    }
  }

  ASSERT_EQ(put(txn, primary, "after-partial-null-outputs", "works"),
            MAKO_LOCAL_OK);
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, BothScanDirectionsOverwriteEveryValidOutputSentinel) {
  auto *txn = begin();
  const mako_local_scan_options invalid_options{};
  mako_local_scan_entry descriptor{UINT32_MAX, UINT32_MAX, UINT32_MAX,
                                   UINT32_MAX};
  uint8_t arena[1]{UINT8_MAX};

  for (const bool reverse : {false, true}) {
    size_t count = std::numeric_limits<size_t>::max();
    size_t used = std::numeric_limits<size_t>::max();
    size_t required = std::numeric_limits<size_t>::max();
    uint8_t done = UINT8_MAX;
    const int status =
        reverse
            ? mako_local_txn_rscan_chunk(txn, primary, &invalid_options,
                                         &descriptor, 1, arena, sizeof(arena),
                                         &count, &used, &required, &done)
            : mako_local_txn_scan_chunk(txn, primary, &invalid_options,
                                        &descriptor, 1, arena, sizeof(arena),
                                        &count, &used, &required, &done);
    EXPECT_EQ(status, MAKO_LOCAL_INVALID_ARGUMENT)
        << (reverse ? "reverse" : "forward");
    EXPECT_EQ(count, 0U);
    EXPECT_EQ(used, 0U);
    EXPECT_EQ(required, 0U);
    EXPECT_EQ(done, 0U);
  }

  ASSERT_EQ(put(txn, primary, "after-scan-output-failures", "works"),
            MAKO_LOCAL_OK);
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, ScanChunksUseSymmetricBoundsAndExclusiveResume) {
  const std::string nul_key("b\0", 2);
  const std::string nul_value("nul\0value", 9);
  const std::string max_key(MAKO_LOCAL_MAX_KEY_BYTES, 'z');
  const std::string absolute_max_key(MAKO_LOCAL_MAX_KEY_BYTES,
                                     static_cast<char>(UINT8_MAX));
  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "", "empty-key"), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, "a", "va"), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, "b", "vb"), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, nul_key, nul_value), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, "c", ""), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, "d", "vd"), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, max_key, "maximum"), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, absolute_max_key, "absolute-maximum"),
            MAKO_LOCAL_OK);
  commit_and_destroy(txn);

  const std::string lower = "a";
  const std::string upper = "d";
  const std::vector<std::pair<std::string, std::string>> ascending = {
      {"a", "va"}, {"b", "vb"}, {nul_key, nul_value}, {"c", ""}};

  txn = begin();
  mako_local_scan_options options{
      MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE, MAKO_LOCAL_SCAN_HAS_UPPER,
      reinterpret_cast<const uint8_t *>(lower.data()), lower.size(),
      reinterpret_cast<const uint8_t *>(upper.data()), upper.size(),
      nullptr, 0};
  std::vector<std::pair<std::string, std::string>> seen;
  std::string resume;
  for (size_t guard = 0; guard != 10; ++guard) {
    const auto chunk = scan_chunk(txn, primary, options, false, 2, 64);
    ASSERT_EQ(chunk.status, MAKO_LOCAL_OK);
    EXPECT_EQ(chunk.arena_required, 0U);
    seen.insert(seen.end(), chunk.entries.begin(), chunk.entries.end());
    if (chunk.done) break;
    ASSERT_FALSE(chunk.entries.empty());
    resume = chunk.entries.back().first;
    options.flags |= MAKO_LOCAL_SCAN_HAS_RESUME;
    options.resume = reinterpret_cast<const uint8_t *>(resume.data());
    options.resume_len = resume.size();
  }
  EXPECT_EQ(seen, ascending);

  options.flags = MAKO_LOCAL_SCAN_HAS_UPPER;
  options.resume = nullptr;
  options.resume_len = 0;
  seen.clear();
  resume.clear();
  for (size_t guard = 0; guard != 10; ++guard) {
    const auto chunk = scan_chunk(txn, primary, options, true, 2, 64);
    ASSERT_EQ(chunk.status, MAKO_LOCAL_OK);
    seen.insert(seen.end(), chunk.entries.begin(), chunk.entries.end());
    if (chunk.done) break;
    ASSERT_FALSE(chunk.entries.empty());
    resume = chunk.entries.back().first;
    options.flags |= MAKO_LOCAL_SCAN_HAS_RESUME;
    options.resume = reinterpret_cast<const uint8_t *>(resume.data());
    options.resume_len = resume.size();
  }
  auto descending = ascending;
  std::reverse(descending.begin(), descending.end());
  EXPECT_EQ(seen, descending);
  commit_and_destroy(txn);

  // A maximum-sized resume key needs no appended byte. The next chunk is the
  // end of range rather than VALUE_TOO_LARGE or a duplicate.
  txn = begin();
  options = mako_local_scan_options{
      MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE, MAKO_LOCAL_SCAN_HAS_UPPER,
      reinterpret_cast<const uint8_t *>(max_key.data()), max_key.size(),
      reinterpret_cast<const uint8_t *>(absolute_max_key.data()),
      absolute_max_key.size(), nullptr, 0};
  auto chunk = scan_chunk(txn, primary, options, false, 1, 2048);
  ASSERT_EQ(chunk.status, MAKO_LOCAL_OK);
  ASSERT_EQ(chunk.entries.size(), 1U);
  EXPECT_EQ(chunk.entries[0],
            (std::pair<std::string, std::string>{max_key, "maximum"}));
  EXPECT_EQ(chunk.done, 0U);
  options.flags = MAKO_LOCAL_SCAN_HAS_UPPER |
                  MAKO_LOCAL_SCAN_HAS_RESUME;
  options.resume = reinterpret_cast<const uint8_t *>(max_key.data());
  options.resume_len = max_key.size();
  chunk = scan_chunk(txn, primary, options, false, 1, 2048);
  EXPECT_EQ(chunk.status, MAKO_LOCAL_OK);
  EXPECT_TRUE(chunk.entries.empty());
  EXPECT_EQ(chunk.done, 1U);
  commit_and_destroy(txn);

  // No upper bound means +infinity, not the empty/minimum key. The synthetic
  // native start must include the exact maximum key in the bounded ABI domain.
  txn = begin();
  options = mako_local_scan_options{
      MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE, 0, nullptr, 0,
      nullptr, 0, nullptr, 0};
  chunk = scan_chunk(txn, primary, options, true, 16, 4096);
  ASSERT_EQ(chunk.status, MAKO_LOCAL_OK);
  ASSERT_FALSE(chunk.entries.empty());
  EXPECT_EQ(chunk.entries.front(),
            (std::pair<std::string, std::string>{absolute_max_key,
                                                 "absolute-maximum"}));
  EXPECT_EQ(chunk.entries.back(),
            (std::pair<std::string, std::string>{"", "empty-key"}));
  EXPECT_EQ(chunk.done, 1U);
  for (size_t i = 1; i != chunk.entries.size(); ++i)
    EXPECT_GT(chunk.entries[i - 1].first, chunk.entries[i].first);
  commit_and_destroy(txn);

  // Empty is a real key and an inclusive lower bound. Reverse resume can
  // exclude it explicitly; it must not be mistaken for Masstree's +infinity
  // sentinel.
  txn = begin();
  const std::string empty_upper = "a";
  options = mako_local_scan_options{
      MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE, MAKO_LOCAL_SCAN_HAS_UPPER,
      nullptr, 0,
      reinterpret_cast<const uint8_t *>(empty_upper.data()),
      empty_upper.size(), nullptr, 0};
  chunk = scan_chunk(txn, primary, options, true, 1, 64);
  ASSERT_EQ(chunk.status, MAKO_LOCAL_OK);
  ASSERT_EQ(chunk.entries.size(), 1U);
  EXPECT_EQ(chunk.entries[0].first, "");
  options.flags |= MAKO_LOCAL_SCAN_HAS_RESUME;
  options.resume = nullptr;
  options.resume_len = 0;
  chunk = scan_chunk(txn, primary, options, true, 1, 64);
  EXPECT_EQ(chunk.status, MAKO_LOCAL_OK);
  EXPECT_TRUE(chunk.entries.empty());
  EXPECT_EQ(chunk.done, 1U);
  commit_and_destroy(txn);
}

#if READ_MY_WRITES
TEST_F(LocalAbiTest, TransactionalScansReadOwnPointMutationsWithoutChangingThem) {
  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "a", "old-a"), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, "c", "old-c"), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, "e", "old-e"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);

  const std::string binary("new\0binary", 10);
  txn = begin();
  ASSERT_EQ(put(txn, primary, "a", "new-a"), MAKO_LOCAL_OK);
  ASSERT_EQ(insert(txn, primary, "b", binary), MAKO_LOCAL_OK);
  ASSERT_EQ(remove(txn, primary, "c"), MAKO_LOCAL_OK);
  ASSERT_EQ(insert(txn, primary, "d", std::string(4096, 'd')),
            MAKO_LOCAL_OK);

  const std::string lower = "a";
  const std::string upper = "e";
  const mako_local_scan_options options{
      MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE, MAKO_LOCAL_SCAN_HAS_UPPER,
      reinterpret_cast<const uint8_t *>(lower.data()), lower.size(),
      reinterpret_cast<const uint8_t *>(upper.data()), upper.size(),
      nullptr, 0};
  const auto forward = scan_chunk(txn, primary, options, false, 8, 8192);
  ASSERT_EQ(forward.status, MAKO_LOCAL_OK);
  const std::vector<std::pair<std::string, std::string>> expected = {
      {"a", "new-a"}, {"b", binary}, {"d", std::string(4096, 'd')}};
  EXPECT_EQ(forward.entries, expected);
  EXPECT_EQ(forward.done, 1U);

  const auto reverse = scan_chunk(txn, primary, options, true, 8, 8192);
  ASSERT_EQ(reverse.status, MAKO_LOCAL_OK);
  auto reverse_expected = expected;
  std::reverse(reverse_expected.begin(), reverse_expected.end());
  EXPECT_EQ(reverse.entries, reverse_expected);

  // Scan callbacks receive copies. Stripping Mako's trailer for the ABI must
  // not truncate the transaction's actual staged write buffers.
  EXPECT_EQ(get(txn, primary, "a").second, std::optional<std::string>("new-a"));
  EXPECT_EQ(get(txn, primary, "b").second, std::optional<std::string>(binary));
  commit_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "a").second, std::optional<std::string>("new-a"));
  EXPECT_EQ(get(txn, primary, "b").second, std::optional<std::string>(binary));
  EXPECT_FALSE(get(txn, primary, "c").second.has_value());
  EXPECT_EQ(get(txn, primary, "d").second,
            std::optional<std::string>(std::string(4096, 'd')));
  commit_and_destroy(txn);
}
#endif

TEST_F(LocalAbiTest, ScanArenaRetryIsExactNonterminalAndPartialChunksProgress) {
  const std::string long_value(128, 'v');
  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "long", long_value), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, "p1", "11111"), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, "p2", "22222"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);

  const std::string lower = "long";
  const std::string upper("long\0", 5);
  mako_local_scan_options options{
      MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE, MAKO_LOCAL_SCAN_HAS_UPPER,
      reinterpret_cast<const uint8_t *>(lower.data()), lower.size(),
      reinterpret_cast<const uint8_t *>(upper.data()), upper.size(),
      nullptr, 0};
  txn = begin();
  auto chunk = scan_chunk(txn, primary, options, false, 4, 4);
  EXPECT_EQ(chunk.status, MAKO_LOCAL_BUFFER_TOO_SMALL);
  EXPECT_TRUE(chunk.entries.empty());
  EXPECT_EQ(chunk.arena_used, 0U);
  EXPECT_EQ(chunk.arena_required, lower.size() + long_value.size());
  EXPECT_EQ(chunk.done, 0U);

  chunk = scan_chunk(txn, primary, options, false, 4,
                     lower.size() + long_value.size());
  ASSERT_EQ(chunk.status, MAKO_LOCAL_OK);
  ASSERT_EQ(chunk.entries.size(), 1U);
  EXPECT_EQ(chunk.entries[0],
            (std::pair<std::string, std::string>{lower, long_value}));
  EXPECT_EQ(chunk.done, 1U);

  const std::string partial_lower = "p1";
  const std::string partial_upper = "q";
  options = mako_local_scan_options{
      MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE, MAKO_LOCAL_SCAN_HAS_UPPER,
      reinterpret_cast<const uint8_t *>(partial_lower.data()),
      partial_lower.size(),
      reinterpret_cast<const uint8_t *>(partial_upper.data()),
      partial_upper.size(), nullptr, 0};
  chunk = scan_chunk(txn, primary, options, false, 4, 7);
  ASSERT_EQ(chunk.status, MAKO_LOCAL_OK);
  ASSERT_EQ(chunk.entries.size(), 1U);
  EXPECT_EQ(chunk.entries[0],
            (std::pair<std::string, std::string>{"p1", "11111"}));
  EXPECT_EQ(chunk.done, 0U);
  EXPECT_EQ(chunk.arena_required, 0U);

  options.flags |= MAKO_LOCAL_SCAN_HAS_RESUME;
  options.resume = reinterpret_cast<const uint8_t *>(partial_lower.data());
  options.resume_len = partial_lower.size();
  chunk = scan_chunk(txn, primary, options, false, 4, 7);
  ASSERT_EQ(chunk.status, MAKO_LOCAL_OK);
  ASSERT_EQ(chunk.entries.size(), 1U);
  EXPECT_EQ(chunk.entries[0],
            (std::pair<std::string, std::string>{"p2", "22222"}));

  // BUFFER_TOO_SMALL did not finish the transaction.
  ASSERT_EQ(put(txn, primary, "after-scan-retry", "works"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, ScanFailuresInitializeOutputsAndBudgetFailureIsTerminal) {
  auto *txn = begin();
  mako_local_scan_entry descriptor{99, 99, 99, 99};
  uint8_t arena[8]{};
  size_t count = 99;
  size_t used = 99;
  size_t required = 99;
  uint8_t done = 99;
  mako_local_scan_options bad_options{};
  EXPECT_EQ(mako_local_txn_scan_chunk(
                txn, primary, &bad_options, &descriptor, 1, arena,
                sizeof(arena), &count, &used, &required, &done),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(count, 0U);
  EXPECT_EQ(used, 0U);
  EXPECT_EQ(required, 0U);
  EXPECT_EQ(done, 0U);

  const std::string oversized(MAKO_LOCAL_MAX_KEY_BYTES + 1, 'x');
  bad_options = mako_local_scan_options{
      MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE, 0,
      reinterpret_cast<const uint8_t *>(oversized.data()), oversized.size(),
      nullptr, 0, nullptr, 0};
  count = used = required = 99;
  done = 99;
  EXPECT_EQ(mako_local_txn_scan_chunk(
                txn, primary, &bad_options, &descriptor, 1, arena,
                sizeof(arena), &count, &used, &required, &done),
            MAKO_LOCAL_VALUE_TOO_LARGE);
  EXPECT_EQ(count, 0U);
  EXPECT_EQ(used, 0U);
  EXPECT_EQ(required, 0U);
  EXPECT_EQ(done, 0U);
  abort_and_destroy(txn);

  // Seed more row/predicate observations than the bounded 512-item profile can
  // admit, using several small write transactions so setup itself stays below
  // the point-operation budget.
  for (size_t base = 0; base != 560; base += 70) {
    txn = begin();
    for (size_t i = base; i != base + 70; ++i) {
      ASSERT_EQ(put(txn, primary, "budget-" + std::to_string(i), "v"),
                MAKO_LOCAL_OK);
    }
    commit_and_destroy(txn);
  }

  txn = begin();
  const mako_local_scan_options all{
      MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE, 0, nullptr, 0,
      nullptr, 0, nullptr, 0};
  const auto exhausted = scan_chunk(txn, primary, all, false, 600, 32768);
  EXPECT_EQ(exhausted.status, MAKO_LOCAL_TXN_TOO_LARGE);
  EXPECT_TRUE(exhausted.entries.empty());
  EXPECT_EQ(exhausted.arena_used, 0U);
  EXPECT_EQ(exhausted.arena_required, 0U);
  EXPECT_EQ(exhausted.done, 0U);
  EXPECT_EQ(get(txn, primary, "budget-0").first,
            MAKO_LOCAL_TXN_FINISHED);
  destroy_tracked(txn);

  txn = begin();
  ASSERT_EQ(put(txn, primary, "after-scan-budget", "works"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, WeightedItemBudgetAbortsBeforeTheStoHardLimit) {
  auto *txn = begin();
  for (size_t i = 0; i != MAKO_LOCAL_TXN_ITEM_BUDGET; ++i) {
    const auto read = get(txn, primary, "same-missing-key");
    ASSERT_EQ(read.first, MAKO_LOCAL_OK) << "read " << i;
    ASSERT_FALSE(read.second.has_value());
  }
  EXPECT_EQ(get(txn, primary, "same-missing-key").first,
            MAKO_LOCAL_TXN_TOO_LARGE);
  EXPECT_EQ(get(txn, primary, "same-missing-key").first,
            MAKO_LOCAL_TXN_FINISHED);
  destroy_tracked(txn);

  // A budget failure is terminal for that transaction, not for its worker.
  txn = begin();
  ASSERT_EQ(put(txn, primary, "after-budget", "works"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, AttachRefreshesAnIdleLegacyStoTransactionThreadId) {
  struct Result {
    bool legacy_commit = false;
    int attach = MAKO_LOCAL_INTERNAL;
    int attached_id = -1;
    int begin = MAKO_LOCAL_INTERNAL;
    int commit = MAKO_LOCAL_INTERNAL;
    int destroy = MAKO_LOCAL_INTERNAL;
  } result;

  std::thread worker([&] {
    // Simulate old direct STO code that left a reusable transaction object in
    // TLS without claiming the new shared runtime.
    TThread::set_mode(0);
    TThread::set_id(MAX_THREADS - 1);
    Sto::start_transaction();
    result.legacy_commit = Sto::try_commit_no_paxos();
    Transaction::rcu_quiesce();

    result.attach = mako_local_thread_attach();
    result.attached_id = TThread::id();
    mako_local_txn *txn = nullptr;
    if (result.attach == MAKO_LOCAL_OK)
      result.begin = mako_local_txn_begin(db, &txn);
    if (result.begin == MAKO_LOCAL_OK)
      result.commit = mako_local_txn_commit(txn);
    if (txn != nullptr)
      result.destroy = mako_local_txn_destroy(txn);
  });
  worker.join();

  EXPECT_TRUE(result.legacy_commit);
  EXPECT_EQ(result.attach, MAKO_LOCAL_OK);
  EXPECT_NE(result.attached_id, MAX_THREADS - 1);
  EXPECT_EQ(result.begin, MAKO_LOCAL_OK);
  EXPECT_EQ(result.commit, MAKO_LOCAL_OK);
  EXPECT_EQ(result.destroy, MAKO_LOCAL_OK);
}

TEST_F(LocalAbiTest, AbortAndDestroyOfActiveTransactionBothRollBack) {
  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "abort", "invisible"), MAKO_LOCAL_OK);
  abort_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "abort"),
            (std::pair<int, std::optional<std::string>>{MAKO_LOCAL_OK,
                                                        std::nullopt}));
  commit_and_destroy(txn);

  txn = begin();
  ASSERT_EQ(put(txn, primary, "drop", "also-invisible"), MAKO_LOCAL_OK);
  destroy_tracked(txn);
  txn = begin();
  EXPECT_FALSE(get(txn, primary, "drop").second.has_value());
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, MissingPresentEmptyAndBinaryValuesStayDistinct) {
  const std::string binary_key("k\0y", 3);
  const std::string binary_value("v\0x\xff", 4);

  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "empty", ""), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, binary_key, binary_value), MAKO_LOCAL_OK);
  commit_and_destroy(txn);

  txn = begin();
  auto missing = get(txn, primary, "missing");
  EXPECT_EQ(missing.first, MAKO_LOCAL_OK);
  EXPECT_FALSE(missing.second.has_value());
  auto empty = get(txn, primary, "empty");
  ASSERT_TRUE(empty.second.has_value());
  EXPECT_TRUE(empty.second->empty());
  EXPECT_EQ(get(txn, primary, binary_key).second,
            std::optional<std::string>(binary_value));
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, EveryStagedWriteOwnsADistinctStableBuffer) {
  // Regression for the old Redis FFI, where every StringWrapper pointed at
  // one reusable std::string and all keys committed with the last value.
  const std::string short_value = "first";  // exercises SSO
  const std::string long_value(4096, 'z');
  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "short", "old-short"), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, "long", "old-long"), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, "third", "old-third"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);

  txn = begin();
  ASSERT_EQ(put(txn, primary, "short", short_value), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, "long", long_value), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, "third", "different"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "short").second,
            std::optional<std::string>(short_value));
  EXPECT_EQ(get(txn, primary, "long").second,
            std::optional<std::string>(long_value));
  EXPECT_EQ(get(txn, primary, "third").second,
            std::optional<std::string>("different"));
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest,
       RecycledValueSlotsSurviveRetentionBoundaryGrowthAndAbort) {
  constexpr size_t kWrites = 20;
  std::array<std::string, kWrites> keys;
  std::array<std::string, kWrites> original;
  std::array<std::string, kWrites> committed;
  for (size_t index = 0; index != kWrites; ++index) {
    keys[index] = "value-pool-" + std::to_string(index);
    original[index] = std::string(index + 1, static_cast<char>('a' + index));
    committed[index] =
        std::string(32 + index, static_cast<char>('A' + index));
  }

  auto *txn = begin();
  for (size_t index = 0; index != kWrites; ++index)
    ASSERT_EQ(put(txn, primary, keys[index], original[index]), MAKO_LOCAL_OK);
  commit_and_destroy(txn);

  // Exercise many reusable slots and cross the per-slot capacity ceiling,
  // then abort. Releasing those buffers is only safe after native rollback
  // has stopped borrowing every std::string object.
  txn = begin();
  for (size_t index = 0; index != kWrites; ++index) {
    const std::string value = index == 0 ? std::string(8192, 'x')
                                         : std::string(index + 3, 'y');
    ASSERT_EQ(put(txn, primary, keys[index], value), MAKO_LOCAL_OK);
  }
  abort_and_destroy(txn);

  txn = begin();
  for (size_t index = 0; index != kWrites; ++index)
    EXPECT_EQ(get(txn, primary, keys[index]).second,
              std::optional<std::string>(original[index]));
  commit_and_destroy(txn);

  // Reuse the same facade pool after the oversized slot was discarded.
  txn = begin();
  for (size_t index = 0; index != kWrites; ++index)
    ASSERT_EQ(put(txn, primary, keys[index], committed[index]), MAKO_LOCAL_OK);
  commit_and_destroy(txn);

  txn = begin();
  for (size_t index = 0; index != kWrites; ++index)
    EXPECT_EQ(get(txn, primary, keys[index]).second,
              std::optional<std::string>(committed[index]));
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, ExactMaximumEncodedValueSlotsRemainStable) {
  constexpr size_t kMinimumWriteCharge = 4;
  constexpr size_t kMaximumWrites =
      MAKO_LOCAL_TXN_ITEM_BUDGET / kMinimumWriteCharge;
  static_assert(kMaximumWrites == 128);
  std::array<mako_local_table *, kMaximumWrites + 1> tables{};
  for (size_t index = 0; index != tables.size(); ++index)
    open_table("value-pool-limit-" + std::to_string(index), &tables[index]);

  // Empty keys are the only way to reach the four-credit minimum. Distinct
  // tables keep all 128 StringWrapper borrows live through one commit.
  auto *txn = begin();
  for (size_t index = 0; index != kMaximumWrites; ++index) {
    const std::string value(1, static_cast<char>(index));
    ASSERT_EQ(put(txn, tables[index], "", value), MAKO_LOCAL_OK)
        << "write " << index;
  }
  commit_and_destroy(txn);

  txn = begin();
  for (size_t index = 0; index != kMaximumWrites; ++index) {
    const std::string expected(1, static_cast<char>(index));
    EXPECT_EQ(get(txn, tables[index], "").second,
              std::optional<std::string>(expected));
  }
  commit_and_destroy(txn);

  // The next minimum-charge write must terminate the transaction before it
  // can index one element beyond the fixed pool, including in release builds
  // where the assertion in stage_encoded_value is compiled out.
  txn = begin();
  for (size_t index = 0; index != kMaximumWrites; ++index) {
    const std::string value(1, static_cast<char>(index));
    ASSERT_EQ(put(txn, tables[index], "", value), MAKO_LOCAL_OK)
        << "write " << index;
  }
  EXPECT_EQ(put(txn, tables[kMaximumWrites], "", "overflow"),
            MAKO_LOCAL_TXN_TOO_LARGE);
  destroy_tracked(txn);
}

TEST_F(LocalAbiTest, AbortingALargerValueResizePreservesTheOldValue) {
  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "resize-abort", "old"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);

  txn = begin();
  ASSERT_EQ(put(txn, primary, "resize-abort", std::string(8192, 'n')),
            MAKO_LOCAL_OK);
  abort_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "resize-abort").second,
            std::optional<std::string>("old"));
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, PutInsertAndRemoveReportExistence) {
  uint8_t result = 0;
  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "k", "one", &result), MAKO_LOCAL_OK);
  EXPECT_EQ(result, 1);
  commit_and_destroy(txn);

  txn = begin();
  ASSERT_EQ(put(txn, primary, "k", "two", &result), MAKO_LOCAL_OK);
  EXPECT_EQ(result, 0);
  commit_and_destroy(txn);

  txn = begin();
  ASSERT_EQ(mako_local_txn_insert(
                txn, primary, reinterpret_cast<const uint8_t *>("k"), 1,
                reinterpret_cast<const uint8_t *>("ignored"), 7, &result),
            MAKO_LOCAL_OK);
  EXPECT_EQ(result, 0);
  commit_and_destroy(txn);

  txn = begin();
  ASSERT_EQ(mako_local_txn_remove(
                txn, primary, reinterpret_cast<const uint8_t *>("k"), 1,
                &result),
            MAKO_LOCAL_OK);
  EXPECT_EQ(result, 1);
  commit_and_destroy(txn);

  txn = begin();
  EXPECT_FALSE(get(txn, primary, "k").second.has_value());
  commit_and_destroy(txn);
}

#if READ_MY_WRITES
TEST_F(LocalAbiTest, PointReadYourWritesSeesNewAndExistingPuts) {
  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "ryw-put-existing", "old"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);

  txn = begin();
  uint8_t created = 99;
  ASSERT_EQ(put(txn, primary, "ryw-put-existing", "updated", &created),
            MAKO_LOCAL_OK);
  EXPECT_EQ(created, 0);
  EXPECT_EQ(get(txn, primary, "ryw-put-existing"),
            (std::pair<int, std::optional<std::string>>{
                MAKO_LOCAL_OK, std::string("updated")}));

  created = 99;
  ASSERT_EQ(put(txn, primary, "ryw-put-new", "created", &created),
            MAKO_LOCAL_OK);
  EXPECT_EQ(created, 1);
  EXPECT_EQ(get(txn, primary, "ryw-put-new"),
            (std::pair<int, std::optional<std::string>>{
                MAKO_LOCAL_OK, std::string("created")}));
  commit_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "ryw-put-existing").second,
            std::optional<std::string>("updated"));
  EXPECT_EQ(get(txn, primary, "ryw-put-new").second,
            std::optional<std::string>("created"));
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, PointReadYourWritesNewPutAbortsCleanly) {
  auto *txn = begin();
  uint8_t created = 99;
  ASSERT_EQ(put(txn, primary, "ryw-put-abort", "temporary", &created),
            MAKO_LOCAL_OK);
  EXPECT_EQ(created, 1);
  EXPECT_EQ(get(txn, primary, "ryw-put-abort"),
            (std::pair<int, std::optional<std::string>>{
                MAKO_LOCAL_OK, std::string("temporary")}));
  abort_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "ryw-put-abort"),
            (std::pair<int, std::optional<std::string>>{MAKO_LOCAL_OK,
                                                        std::nullopt}));
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, PointReadYourWritesSeesInsertedValuesBeforeCommitOrAbort) {
  uint8_t inserted = 99;
  auto *txn = begin();
  ASSERT_EQ(mako_local_txn_insert(
                txn, primary,
                reinterpret_cast<const uint8_t *>("ryw-insert-abort"), 16,
                reinterpret_cast<const uint8_t *>("temporary"), 9, &inserted),
            MAKO_LOCAL_OK);
  EXPECT_EQ(inserted, 1);
  EXPECT_EQ(get(txn, primary, "ryw-insert-abort").second,
            std::optional<std::string>("temporary"));
  abort_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "ryw-insert-abort"),
            (std::pair<int, std::optional<std::string>>{MAKO_LOCAL_OK,
                                                        std::nullopt}));
  commit_and_destroy(txn);

  inserted = 99;
  txn = begin();
  ASSERT_EQ(mako_local_txn_insert(
                txn, primary,
                reinterpret_cast<const uint8_t *>("ryw-insert-commit"), 17,
                reinterpret_cast<const uint8_t *>("durable"), 7, &inserted),
            MAKO_LOCAL_OK);
  EXPECT_EQ(inserted, 1);
  EXPECT_EQ(get(txn, primary, "ryw-insert-commit").second,
            std::optional<std::string>("durable"));
  commit_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "ryw-insert-commit").second,
            std::optional<std::string>("durable"));
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, PointReadYourWritesHidesRemovedValuesBeforeCommitOrAbort) {
  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "ryw-remove-abort", "keep"), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, "ryw-remove-commit", "delete"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);

  uint8_t existed = 99;
  txn = begin();
  ASSERT_EQ(mako_local_txn_remove(
                txn, primary,
                reinterpret_cast<const uint8_t *>("ryw-remove-abort"), 16,
                &existed),
            MAKO_LOCAL_OK);
  EXPECT_EQ(existed, 1);
  EXPECT_EQ(get(txn, primary, "ryw-remove-abort"),
            (std::pair<int, std::optional<std::string>>{MAKO_LOCAL_OK,
                                                        std::nullopt}));
  abort_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "ryw-remove-abort").second,
            std::optional<std::string>("keep"));
  commit_and_destroy(txn);

  existed = 99;
  txn = begin();
  ASSERT_EQ(mako_local_txn_remove(
                txn, primary,
                reinterpret_cast<const uint8_t *>("ryw-remove-commit"), 17,
                &existed),
            MAKO_LOCAL_OK);
  EXPECT_EQ(existed, 1);
  EXPECT_EQ(get(txn, primary, "ryw-remove-commit"),
            (std::pair<int, std::optional<std::string>>{MAKO_LOCAL_OK,
                                                        std::nullopt}));
  commit_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "ryw-remove-commit"),
            (std::pair<int, std::optional<std::string>>{MAKO_LOCAL_OK,
                                                        std::nullopt}));
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, ExactLengthCallerKeyRequiresNoMasstreePadding) {
  constexpr std::array<uint8_t, 17> key = {
      'e', 'x', 'a', 'c', 't', '-', 'l', 'e', 'n', 'g', 't', 'h', '-', 'k', 'e',
      'y', '!'};
  auto free_bytes = [](uint8_t *bytes) { std::free(bytes); };
  std::unique_ptr<uint8_t, decltype(free_bytes)> exact_key(
      static_cast<uint8_t *>(std::malloc(key.size())), free_bytes);
  ASSERT_NE(exact_key, nullptr);
  std::memcpy(exact_key.get(), key.data(), key.size());

  uint8_t created = 99;
  auto *txn = begin();
  ASSERT_EQ(mako_local_txn_put(
                txn, primary, exact_key.get(), key.size(),
                reinterpret_cast<const uint8_t *>("value"), 5, &created),
            MAKO_LOCAL_OK);
  EXPECT_EQ(created, 1);
  commit_and_destroy(txn);

  uint8_t existed = 99;
  txn = begin();
  ASSERT_EQ(mako_local_txn_remove(txn, primary, exact_key.get(), key.size(),
                                  &existed),
            MAKO_LOCAL_OK);
  EXPECT_EQ(existed, 1);
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, PointReadYourWritesPreservesEmptyAndBinaryValues) {
  const std::string binary_key("ryw\0key", 7);
  const std::string binary_value("value\0\xff", 7);

  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "ryw-empty", ""), MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, binary_key, binary_value), MAKO_LOCAL_OK);

  auto empty = get(txn, primary, "ryw-empty");
  ASSERT_EQ(empty.first, MAKO_LOCAL_OK);
  ASSERT_TRUE(empty.second.has_value());
  EXPECT_TRUE(empty.second->empty());
  EXPECT_EQ(get(txn, primary, binary_key).second,
            std::optional<std::string>(binary_value));
  commit_and_destroy(txn);

  txn = begin();
  empty = get(txn, primary, "ryw-empty");
  ASSERT_TRUE(empty.second.has_value());
  EXPECT_TRUE(empty.second->empty());
  EXPECT_EQ(get(txn, primary, binary_key).second,
            std::optional<std::string>(binary_value));
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, PointReadYourWritesHandlesLargeResizeOnCommitAndAbort) {
  const std::string aborted_value(8192, 'a');
  const std::string committed_value(16384, 'c');

  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "ryw-resize-abort", "old-abort"),
            MAKO_LOCAL_OK);
  ASSERT_EQ(put(txn, primary, "ryw-resize-commit", "old-commit"),
            MAKO_LOCAL_OK);
  commit_and_destroy(txn);

  txn = begin();
  ASSERT_EQ(put(txn, primary, "ryw-resize-abort", aborted_value),
            MAKO_LOCAL_OK);
  EXPECT_EQ(get(txn, primary, "ryw-resize-abort").second,
            std::optional<std::string>(aborted_value));
  abort_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "ryw-resize-abort").second,
            std::optional<std::string>("old-abort"));
  commit_and_destroy(txn);

  txn = begin();
  ASSERT_EQ(put(txn, primary, "ryw-resize-commit", committed_value),
            MAKO_LOCAL_OK);
  EXPECT_EQ(get(txn, primary, "ryw-resize-commit").second,
            std::optional<std::string>(committed_value));
  commit_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "ryw-resize-commit").second,
            std::optional<std::string>(committed_value));
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, PointReadThenGrowingPutPreservesValidation) {
  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "ryw-read-resize", "old"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);

  const std::string grown_value(8192, 'g');
  txn = begin();
  EXPECT_EQ(get(txn, primary, "ryw-read-resize"),
            (std::pair<int, std::optional<std::string>>{
                MAKO_LOCAL_OK, std::string("old")}));
  EXPECT_EQ(get(txn, primary, "ryw-read-resize"),
            (std::pair<int, std::optional<std::string>>{
                MAKO_LOCAL_OK, std::string("old")}));
  ASSERT_EQ(put(txn, primary, "ryw-read-resize", grown_value),
            MAKO_LOCAL_OK);
  EXPECT_EQ(get(txn, primary, "ryw-read-resize").second,
            std::optional<std::string>(grown_value));
  commit_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "ryw-read-resize").second,
            std::optional<std::string>(grown_value));
  commit_and_destroy(txn);
}
#endif

TEST_F(LocalAbiTest, NestedBeginAndWrongDatabaseTableAreRejected) {
  auto *txn = begin();
  mako_local_txn *nested = nullptr;
  EXPECT_EQ(mako_local_txn_begin(db, &nested),
            MAKO_LOCAL_TXN_ALREADY_ACTIVE);
  EXPECT_EQ(nested, nullptr);

  mako_local_db *other_db = nullptr;
  mako_local_table *other_table = nullptr;
  ASSERT_EQ(mako_local_db_open(&other_db), MAKO_LOCAL_OK);
  const std::string name = "other";
  ASSERT_EQ(mako_local_table_open(
                other_db, reinterpret_cast<const uint8_t *>(name.data()),
                name.size(), next_table_id.fetch_add(1), &other_table),
            MAKO_LOCAL_OK);
  uint8_t created = 0;
  EXPECT_EQ(put(txn, other_table, "x", "y", &created),
            MAKO_LOCAL_WRONG_DB_OR_TABLE);
  abort_and_destroy(txn);
  EXPECT_EQ(mako_local_db_close(other_db), MAKO_LOCAL_OK);
}

TEST_F(LocalAbiTest, WrongThreadAndFinishedHandlesAreRejected) {
  auto *txn = begin();
  std::atomic<int> wrong_thread_status{MAKO_LOCAL_INTERNAL};
  std::atomic<int> wrong_thread_destroy{MAKO_LOCAL_INTERNAL};
  std::atomic<int> wrong_thread_attach{MAKO_LOCAL_INTERNAL};
  std::atomic<int> attached_wrong_thread_status{MAKO_LOCAL_INTERNAL};
  std::atomic<int> attached_wrong_thread_destroy{MAKO_LOCAL_INTERNAL};
  std::thread other([&] {
    uint8_t *value = nullptr;
    size_t value_len = 0;
    uint8_t found = 0;
    wrong_thread_status.store(mako_local_txn_get(
        txn, primary, reinterpret_cast<const uint8_t *>("key"), 3, &value,
        &value_len, &found));
    mako_local_bytes_free(value);
    wrong_thread_destroy.store(mako_local_txn_destroy(txn));

    // Ownership must remain tied to the creator's non-recycled STO worker ID
    // even after the calling thread has valid ABI TLS of its own.
    wrong_thread_attach.store(mako_local_thread_attach());
    value = nullptr;
    value_len = 0;
    found = 0;
    attached_wrong_thread_status.store(mako_local_txn_get(
        txn, primary, reinterpret_cast<const uint8_t *>("key"), 3, &value,
        &value_len, &found));
    mako_local_bytes_free(value);
    attached_wrong_thread_destroy.store(mako_local_txn_destroy(txn));
  });
  other.join();
  EXPECT_EQ(wrong_thread_status.load(), MAKO_LOCAL_WRONG_THREAD);
  EXPECT_EQ(wrong_thread_destroy.load(), MAKO_LOCAL_WRONG_THREAD);
  EXPECT_EQ(wrong_thread_attach.load(), MAKO_LOCAL_OK);
  EXPECT_EQ(attached_wrong_thread_status.load(), MAKO_LOCAL_WRONG_THREAD);
  EXPECT_EQ(attached_wrong_thread_destroy.load(), MAKO_LOCAL_WRONG_THREAD);

  ASSERT_EQ(mako_local_txn_commit(txn), MAKO_LOCAL_OK);
  EXPECT_EQ(put(txn, primary, "after", "commit"), MAKO_LOCAL_TXN_FINISHED);
  EXPECT_EQ(mako_local_txn_abort(txn), MAKO_LOCAL_TXN_FINISHED);
  destroy_tracked(txn);

  auto *next = begin();
  ASSERT_EQ(put(next, primary, "after-wrong-thread-destroy", "works"),
            MAKO_LOCAL_OK);
  commit_and_destroy(next);
}

#if READ_MY_WRITES
TEST_F(LocalAbiTest, EverySameKeyMutationPairComposesInTransactionOrder) {
  enum class Mutation { Put, Insert, Remove };
  const std::vector<Mutation> mutations = {
      Mutation::Put, Mutation::Insert, Mutation::Remove};
  auto mutation_name = [](Mutation mutation) {
    switch (mutation) {
      case Mutation::Put: return "put";
      case Mutation::Insert: return "insert";
      case Mutation::Remove: return "remove";
    }
    return "unknown";
  };

  size_t sequence = 0;
  for (const bool initially_present : {false, true}) {
    for (const Mutation first : mutations) {
      for (const Mutation second : mutations) {
        const std::string key = "same-key-pair-" + std::to_string(sequence++);
        SCOPED_TRACE(std::string(initially_present ? "present:" : "absent:") +
                     mutation_name(first) + ":" + mutation_name(second));

        if (initially_present) {
          auto *seed = begin();
          ASSERT_EQ(put(seed, primary, key, "seed"), MAKO_LOCAL_OK);
          commit_and_destroy(seed);
        }

        std::optional<std::string> expected =
            initially_present ? std::optional<std::string>("seed")
                              : std::nullopt;
        auto *txn = begin();
        auto apply = [&](Mutation mutation, const std::string &value) {
          const bool expected_changed = expected.has_value();
          uint8_t changed = 99;
          switch (mutation) {
            case Mutation::Put:
              ASSERT_EQ(put(txn, primary, key, value, &changed),
                        MAKO_LOCAL_OK);
              EXPECT_EQ(changed != 0, !expected_changed);
              expected = value;
              break;
            case Mutation::Insert:
              ASSERT_EQ(insert(txn, primary, key, value, &changed),
                        MAKO_LOCAL_OK);
              EXPECT_EQ(changed != 0, !expected_changed);
              if (!expected_changed) expected = value;
              break;
            case Mutation::Remove:
              ASSERT_EQ(remove(txn, primary, key, &changed), MAKO_LOCAL_OK);
              EXPECT_EQ(changed != 0, expected_changed);
              expected.reset();
              break;
          }
          EXPECT_EQ(get(txn, primary, key),
                    (std::pair<int, std::optional<std::string>>{
                        MAKO_LOCAL_OK, expected}));
        };

        apply(first, "first");
        apply(second, "second");
        commit_and_destroy(txn);

        txn = begin();
        EXPECT_EQ(get(txn, primary, key),
                  (std::pair<int, std::optional<std::string>>{
                      MAKO_LOCAL_OK, expected}));
        commit_and_destroy(txn);
      }
    }
  }
}

TEST_F(LocalAbiTest, SameKeyGrowthDeleteReinsertChainCommitsAndAborts) {
  const std::string medium(4096, 'm');
  const std::string large(32768, 'l');
  const std::string binary("final\0\xff", 7);

  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "same-key-chain", "seed"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);

  txn = begin();
  uint8_t changed = 99;
  ASSERT_EQ(put(txn, primary, "same-key-chain", "short", &changed),
            MAKO_LOCAL_OK);
  EXPECT_EQ(changed, 0);
  ASSERT_EQ(put(txn, primary, "same-key-chain", medium, &changed),
            MAKO_LOCAL_OK);
  EXPECT_EQ(changed, 0);
  EXPECT_EQ(get(txn, primary, "same-key-chain").second,
            std::optional<std::string>(medium));
  ASSERT_EQ(put(txn, primary, "same-key-chain", large, &changed),
            MAKO_LOCAL_OK);
  EXPECT_EQ(changed, 0);
  EXPECT_EQ(get(txn, primary, "same-key-chain").second,
            std::optional<std::string>(large));
  ASSERT_EQ(remove(txn, primary, "same-key-chain", &changed), MAKO_LOCAL_OK);
  EXPECT_EQ(changed, 1);
  EXPECT_FALSE(get(txn, primary, "same-key-chain").second.has_value());
  ASSERT_EQ(insert(txn, primary, "same-key-chain", binary, &changed),
            MAKO_LOCAL_OK);
  EXPECT_EQ(changed, 1);
  EXPECT_EQ(get(txn, primary, "same-key-chain").second,
            std::optional<std::string>(binary));
  abort_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "same-key-chain").second,
            std::optional<std::string>("seed"));
  commit_and_destroy(txn);

  txn = begin();
  ASSERT_EQ(remove(txn, primary, "same-key-chain", &changed), MAKO_LOCAL_OK);
  EXPECT_EQ(changed, 1);
  ASSERT_EQ(put(txn, primary, "same-key-chain", "recreated", &changed),
            MAKO_LOCAL_OK);
  EXPECT_EQ(changed, 1);
  ASSERT_EQ(insert(txn, primary, "same-key-chain", "ignored", &changed),
            MAKO_LOCAL_OK);
  EXPECT_EQ(changed, 0);
  ASSERT_EQ(put(txn, primary, "same-key-chain", large, &changed),
            MAKO_LOCAL_OK);
  EXPECT_EQ(changed, 0);
  EXPECT_EQ(get(txn, primary, "same-key-chain").second,
            std::optional<std::string>(large));
  commit_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "same-key-chain").second,
            std::optional<std::string>(large));
  commit_and_destroy(txn);
}

TEST_F(LocalAbiTest, RepeatedSameKeyWritesRemainChargedToTheItemBudget) {
  constexpr size_t key_len = 1;
  constexpr size_t write_charge = 4 + (key_len + 7) / 8;
  constexpr size_t accepted = MAKO_LOCAL_TXN_ITEM_BUDGET / write_charge;
  static_assert(accepted * write_charge <= MAKO_LOCAL_TXN_ITEM_BUDGET);

  auto *txn = begin();
  uint8_t created = 99;
  for (size_t i = 0; i != accepted; ++i) {
    const std::string value(1, static_cast<char>('a' + i % 26));
    ASSERT_EQ(put(txn, primary, "b", value, &created), MAKO_LOCAL_OK)
        << "write " << i;
    EXPECT_EQ(created, i == 0 ? 1 : 0);
  }
  EXPECT_EQ(put(txn, primary, "b", "over-budget", &created),
            MAKO_LOCAL_TXN_TOO_LARGE);
  EXPECT_EQ(put(txn, primary, "b", "after-terminal", &created),
            MAKO_LOCAL_TXN_FINISHED);
  destroy_tracked(txn);

  txn = begin();
  EXPECT_FALSE(get(txn, primary, "b").second.has_value());
  commit_and_destroy(txn);
}
#else
TEST_F(LocalAbiTest, DuplicateWritesAreRejectedWithoutCorruptingTheFirst) {
  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "repeat", "first"), MAKO_LOCAL_OK);
  EXPECT_EQ(put(txn, primary, "repeat", "second"),
            MAKO_LOCAL_DUPLICATE_WRITE);
  commit_and_destroy(txn);

  txn = begin();
  EXPECT_EQ(get(txn, primary, "repeat").second,
            std::optional<std::string>("first"));
  commit_and_destroy(txn);

  txn = begin();
  uint8_t existed = 0;
  ASSERT_EQ(mako_local_txn_remove(
                txn, primary, reinterpret_cast<const uint8_t *>("repeat"), 6,
                &existed),
            MAKO_LOCAL_OK);
  ASSERT_EQ(existed, 1);
  EXPECT_EQ(put(txn, primary, "repeat", "unsafe-composition"),
            MAKO_LOCAL_DUPLICATE_WRITE);
  abort_and_destroy(txn);
}
#endif

TEST_F(LocalAbiTest, DatabaseCloseReportsBusyUntilTransactionEnds) {
  auto *txn = begin();
  EXPECT_EQ(mako_local_db_close(db), MAKO_LOCAL_BUSY);
  abort_and_destroy(txn);
}

TEST_F(LocalAbiTest, TrustedFastTransactionAlsoKeepsDatabaseBusy) {
  mako_local_txn *txn = nullptr;
  ASSERT_EQ(mako_rust_fast_txn_begin(db, primary, &txn), MAKO_LOCAL_OK);
  ASSERT_NE(txn, nullptr);
  txn_for_cleanup = txn;

  // Rust normally borrows LocalDb for the transaction lifetime, but
  // mem::forget is safe and ends that borrow without running Drop. Preserve
  // the public ABI's BUSY/leak-safe close contract for that case too.
  EXPECT_EQ(mako_local_db_close(db), MAKO_LOCAL_BUSY);

  const uint64_t aborted = mako_rust_fast_txn_abort_and_destroy(txn);
  txn_for_cleanup = nullptr;
  EXPECT_EQ(MAKO_RUST_FAST_TERMINAL_STATUS(aborted), MAKO_LOCAL_OK);
  EXPECT_EQ(MAKO_RUST_FAST_CLEANUP_STATUS(aborted), MAKO_LOCAL_OK);
}

TEST_F(LocalAbiTest, ActiveTransactionOnAnotherWorkerKeepsDatabaseBusy) {
  struct WorkerResult {
    int attach = MAKO_LOCAL_INTERNAL;
    int begin = MAKO_LOCAL_INTERNAL;
    int abort = MAKO_LOCAL_INTERNAL;
    int destroy = MAKO_LOCAL_INTERNAL;
  } result;
  std::barrier phase(2);
  std::atomic<int> close_status{MAKO_LOCAL_INTERNAL};

  std::thread worker([&] {
    result.attach = mako_local_thread_attach();
    mako_local_txn *txn = nullptr;
    if (result.attach == MAKO_LOCAL_OK)
      result.begin = mako_local_txn_begin(db, &txn);
    phase.arrive_and_wait();
    phase.arrive_and_wait();
    // If close unexpectedly consumed the database, retaining the leaked
    // facade is safer than dereferencing its now-invalid owner in cleanup.
    if (txn != nullptr &&
        close_status.load(std::memory_order_acquire) != MAKO_LOCAL_OK) {
      result.abort = mako_local_txn_abort(txn);
      result.destroy = mako_local_txn_destroy(txn);
    }
  });

  phase.arrive_and_wait();
  const int close = mako_local_db_close(db);
  if (close == MAKO_LOCAL_OK)
    db = nullptr;
  close_status.store(close, std::memory_order_release);
  phase.arrive_and_wait();
  worker.join();

  EXPECT_EQ(result.attach, MAKO_LOCAL_OK);
  EXPECT_EQ(result.begin, MAKO_LOCAL_OK);
  EXPECT_EQ(close, MAKO_LOCAL_BUSY);
  EXPECT_EQ(result.abort, MAKO_LOCAL_OK);
  EXPECT_EQ(result.destroy, MAKO_LOCAL_OK);
}

TEST_F(LocalAbiTest, RecycledHandleChangesDatabaseWithoutStaleBusyState) {
  auto *first = begin();
  ASSERT_EQ(put(first, primary, "first-db", "committed"), MAKO_LOCAL_OK);
  commit_and_destroy(first);

  mako_local_db *other_db = nullptr;
  mako_local_table *other_table = nullptr;
  ASSERT_EQ(mako_local_db_open(&other_db), MAKO_LOCAL_OK);
  const std::string other_name = "recycled-owner";
  ASSERT_EQ(mako_local_table_open(
                other_db, reinterpret_cast<const uint8_t *>(other_name.data()),
                other_name.size(), next_table_id.fetch_add(1), &other_table),
            MAKO_LOCAL_OK);

  mako_local_txn *other_txn = nullptr;
  ASSERT_EQ(mako_local_txn_begin(other_db, &other_txn), MAKO_LOCAL_OK);
  ASSERT_NE(other_txn, nullptr);
  ASSERT_EQ(put(other_txn, other_table, "second-db", "active"), MAKO_LOCAL_OK);

  // The worker slot follows the active owner, not the database retained by a
  // previously recycled facade.
  const int first_close = mako_local_db_close(db);
  EXPECT_EQ(first_close, MAKO_LOCAL_OK);
  if (first_close == MAKO_LOCAL_OK)
    db = nullptr;
  EXPECT_EQ(mako_local_db_close(other_db), MAKO_LOCAL_BUSY);

  EXPECT_EQ(mako_local_txn_abort(other_txn), MAKO_LOCAL_OK);
  EXPECT_EQ(mako_local_txn_destroy(other_txn), MAKO_LOCAL_OK);
  EXPECT_EQ(mako_local_db_close(other_db), MAKO_LOCAL_OK);
}

TEST_F(LocalAbiTest, FinishedHandlesCanBeDestroyedOutOfOrderAndReused) {
  auto *first = begin();
  ASSERT_EQ(put(first, primary, "finished-first", "one"), MAKO_LOCAL_OK);
  ASSERT_EQ(mako_local_txn_commit(first), MAKO_LOCAL_OK);

  mako_local_txn *second = nullptr;
  ASSERT_EQ(mako_local_txn_begin(db, &second), MAKO_LOCAL_OK);
  ASSERT_NE(second, nullptr);
  ASSERT_EQ(put(second, primary, "finished-second", "two"), MAKO_LOCAL_OK);

  // Destroying the older facade while the next transaction is active must
  // neither clear nor alias the worker's active-database slot.
  EXPECT_EQ(mako_local_txn_destroy(first), MAKO_LOCAL_OK);
  if (txn_for_cleanup == first)
    txn_for_cleanup = second;
  EXPECT_EQ(mako_local_db_close(db), MAKO_LOCAL_BUSY);
  ASSERT_EQ(mako_local_txn_commit(second), MAKO_LOCAL_OK);

  // The older facade occupies the one-slot worker cache, so consuming this
  // second facade must remain safe even though it cannot itself be retained.
  EXPECT_EQ(mako_local_txn_destroy(second), MAKO_LOCAL_OK);
  txn_for_cleanup = nullptr;

  auto *third = begin();
  EXPECT_EQ(get(third, primary, "finished-first").second,
            std::optional<std::string>("one"));
  EXPECT_EQ(get(third, primary, "finished-second").second,
            std::optional<std::string>("two"));
  commit_and_destroy(third);
}

#if !READ_MY_WRITES
TEST_F(LocalAbiTest, UnadvertisedReadYourWritesConflictLeavesWorkerReusable) {
  ASSERT_EQ(mako_local_feature_bits() & MAKO_LOCAL_FEATURE_READ_MY_WRITES, 0U);

  auto *txn = begin();
  ASSERT_EQ(put(txn, primary, "own-write", "value"), MAKO_LOCAL_OK);
  EXPECT_EQ(get(txn, primary, "own-write").first, MAKO_LOCAL_CONFLICT);
  destroy_tracked(txn);

  txn = begin();
  EXPECT_FALSE(get(txn, primary, "own-write").second.has_value());
  ASSERT_EQ(put(txn, primary, "after-conflict", "works"), MAKO_LOCAL_OK);
  commit_and_destroy(txn);
}
#endif

#if defined(MAKO_LOCAL_TEST_HOOKS)
TEST_F(LocalAbiTest, MidCopyUpdateRetriesWithoutReturningTornPayload) {
  constexpr size_t kPayloadBytes = 64 * 1024;
  const std::string first(kPayloadBytes, 'A');
  const std::string second(kPayloadBytes, 'B');

  auto *seed = begin();
  ASSERT_EQ(put(seed, primary, "payload-midpoint", first), MAKO_LOCAL_OK);
  commit_and_destroy(seed);

  PayloadCopyPause pause;
  struct ReaderResult {
    int attach = MAKO_LOCAL_INTERNAL;
    int begin = MAKO_LOCAL_INTERNAL;
    int get = MAKO_LOCAL_INTERNAL;
    std::optional<std::string> value;
    int commit = MAKO_LOCAL_INTERNAL;
    int destroy = MAKO_LOCAL_INTERNAL;
    int recovery_begin = MAKO_LOCAL_INTERNAL;
    int recovery_put = MAKO_LOCAL_INTERNAL;
    int recovery_commit = MAKO_LOCAL_INTERNAL;
    int recovery_destroy = MAKO_LOCAL_INTERNAL;
  } reader_result;

  std::thread reader([&] {
    reader_result.attach = mako_local_thread_attach();
    mako_local_txn *txn = nullptr;
    if (reader_result.attach == MAKO_LOCAL_OK)
      reader_result.begin = mako_local_txn_begin(db, &txn);
    if (reader_result.begin == MAKO_LOCAL_OK) {
      versioned_str::test_set_copy_midpoint_hook(
          park_at_payload_copy_midpoint, &pause);
      auto read = get(txn, primary, "payload-midpoint");
      versioned_str::test_clear_copy_midpoint_hook();
      reader_result.get = read.first;
      reader_result.value = std::move(read.second);
    }
    if (reader_result.get == MAKO_LOCAL_OK)
      reader_result.commit = mako_local_txn_commit(txn);
    if (txn != nullptr)
      reader_result.destroy = mako_local_txn_destroy(txn);

    txn = nullptr;
    reader_result.recovery_begin = mako_local_txn_begin(db, &txn);
    if (reader_result.recovery_begin == MAKO_LOCAL_OK) {
      reader_result.recovery_put =
          put(txn, primary, "payload-midpoint-recovery", "ok");
    }
    if (reader_result.recovery_put == MAKO_LOCAL_OK)
      reader_result.recovery_commit = mako_local_txn_commit(txn);
    if (txn != nullptr)
      reader_result.recovery_destroy = mako_local_txn_destroy(txn);
  });

  const auto park_deadline = std::chrono::steady_clock::now() +
                             std::chrono::seconds(5);
  while (!pause.parked.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < park_deadline) {
    std::this_thread::yield();
  }

  const bool reader_parked = pause.parked.load(std::memory_order_acquire);
  auto *writer = begin();
  EXPECT_EQ(put(writer, primary, "payload-midpoint", second), MAKO_LOCAL_OK);
  commit_and_destroy(writer);
  pause.release.store(true, std::memory_order_release);
  reader.join();

  EXPECT_TRUE(reader_parked);
  EXPECT_FALSE(pause.timed_out.load(std::memory_order_relaxed));
  EXPECT_EQ(pause.calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(reader_result.attach, MAKO_LOCAL_OK);
  EXPECT_EQ(reader_result.begin, MAKO_LOCAL_OK);
  EXPECT_EQ(reader_result.get, MAKO_LOCAL_OK);
  EXPECT_EQ(reader_result.value, std::optional<std::string>(second));
  EXPECT_EQ(reader_result.commit, MAKO_LOCAL_OK);
  EXPECT_EQ(reader_result.destroy, MAKO_LOCAL_OK);
  EXPECT_EQ(reader_result.recovery_begin, MAKO_LOCAL_OK);
  EXPECT_EQ(reader_result.recovery_put, MAKO_LOCAL_OK);
  EXPECT_EQ(reader_result.recovery_commit, MAKO_LOCAL_OK);
  EXPECT_EQ(reader_result.recovery_destroy, MAKO_LOCAL_OK);

  auto *verify = begin();
  EXPECT_EQ(get(verify, primary, "payload-midpoint").second,
            std::optional<std::string>(second));
  EXPECT_EQ(get(verify, primary, "payload-midpoint-recovery").second,
            std::optional<std::string>("ok"));
  commit_and_destroy(verify);
}
#endif

TEST_F(LocalAbiTest, ConcurrentPublishedPayloadCopiesAreNeverTorn) {
  constexpr size_t kPayloadBytes = 64 * 1024;
  constexpr int kWriterIterations = 64;
  constexpr int kReaderIterations = 96;
  constexpr int kReaderCount = 4;
  const std::string first(kPayloadBytes, 'A');
  // Alternate both bytes and published length while staying within the large
  // seed allocation, so readers stress in-place grow/shrink snapshots too.
  const std::string second(1023, 'B');

  auto *seed = begin();
  ASSERT_EQ(put(seed, primary, "payload-race", first), MAKO_LOCAL_OK);
  commit_and_destroy(seed);

  std::barrier start(kReaderCount + 1);
  std::barrier updates_done(kReaderCount + 1);
  std::atomic<int> writer_failures{0};
  std::atomic<int> reader_failures{0};
  std::atomic<int> successful_reads{0};
  std::vector<std::thread> workers;

  workers.emplace_back([&] {
    if (mako_local_thread_attach() != MAKO_LOCAL_OK) {
      ++writer_failures;
      start.arrive_and_wait();
      updates_done.arrive_and_wait();
      return;
    }
    start.arrive_and_wait();
    for (int iteration = 0; iteration != kWriterIterations; ++iteration) {
      mako_local_txn *txn = nullptr;
      int status = mako_local_txn_begin(db, &txn);
      if (status == MAKO_LOCAL_OK)
        status = put(txn, primary, "payload-race",
                     iteration % 2 == 0 ? second : first);
      if (status == MAKO_LOCAL_OK)
        status = mako_local_txn_commit(txn);
      if (status != MAKO_LOCAL_OK) ++writer_failures;
      if (txn != nullptr && mako_local_txn_destroy(txn) != MAKO_LOCAL_OK)
        ++writer_failures;
      std::this_thread::yield();
    }
    updates_done.arrive_and_wait();
  });

  for (int reader = 0; reader != kReaderCount; ++reader) {
    workers.emplace_back([&] {
      if (mako_local_thread_attach() != MAKO_LOCAL_OK) {
        ++reader_failures;
        start.arrive_and_wait();
        updates_done.arrive_and_wait();
        return;
      }
      start.arrive_and_wait();
      for (int iteration = 0; iteration != kReaderIterations; ++iteration) {
        mako_local_txn *txn = nullptr;
        int status = mako_local_txn_begin(db, &txn);
        std::optional<std::string> value;
        if (status == MAKO_LOCAL_OK) {
          auto read = get(txn, primary, "payload-race");
          status = read.first;
          value = std::move(read.second);
        }
        if (status == MAKO_LOCAL_OK) {
          if (value != std::optional<std::string>(first) &&
              value != std::optional<std::string>(second)) {
            ++reader_failures;
          } else {
            ++successful_reads;
          }
          status = mako_local_txn_commit(txn);
        }
        if (status != MAKO_LOCAL_OK && status != MAKO_LOCAL_CONFLICT)
          ++reader_failures;
        if (txn != nullptr && mako_local_txn_destroy(txn) != MAKO_LOCAL_OK)
          ++reader_failures;
        std::this_thread::yield();
      }

      // Prove the reader remains usable after the contended phase, and make
      // at least one successful exact-value observation deterministic.
      updates_done.arrive_and_wait();
      mako_local_txn *txn = nullptr;
      int status = mako_local_txn_begin(db, &txn);
      std::optional<std::string> value;
      if (status == MAKO_LOCAL_OK) {
        auto read = get(txn, primary, "payload-race");
        status = read.first;
        value = std::move(read.second);
      }
      if (status == MAKO_LOCAL_OK && value == std::optional<std::string>(first)) {
        ++successful_reads;
        status = mako_local_txn_commit(txn);
      } else {
        ++reader_failures;
      }
      if (status != MAKO_LOCAL_OK) ++reader_failures;
      if (txn != nullptr && mako_local_txn_destroy(txn) != MAKO_LOCAL_OK)
        ++reader_failures;
    });
  }

  for (auto &worker : workers) worker.join();
  EXPECT_EQ(writer_failures.load(), 0);
  EXPECT_EQ(reader_failures.load(), 0);
  EXPECT_GT(successful_reads.load(), 0);

  auto *verify = begin();
  EXPECT_EQ(get(verify, primary, "payload-race").second,
            std::optional<std::string>(first));
  commit_and_destroy(verify);
}

TEST_F(LocalAbiTest, ConcurrentReadWriteConflictAbortsExactlyOneCommit) {
  auto *seed = begin();
  ASSERT_EQ(put(seed, primary, "contended", "0"), MAKO_LOCAL_OK);
  commit_and_destroy(seed);

  struct WorkerResult {
    int attach = MAKO_LOCAL_INTERNAL;
    int begin = MAKO_LOCAL_INTERNAL;
    int read = MAKO_LOCAL_INTERNAL;
    std::optional<std::string> read_value;
    int contended_put = MAKO_LOCAL_INTERNAL;
    int side_put = MAKO_LOCAL_INTERNAL;
    int commit = MAKO_LOCAL_INTERNAL;
    int destroy = MAKO_LOCAL_INTERNAL;
    HookObservation hook;
  } results[2];

  std::barrier ready(2);
  std::vector<std::thread> workers;
  for (int i = 0; i != 2; ++i) {
    workers.emplace_back([&, i] {
      WorkerResult &result = results[i];
      result.attach = mako_local_thread_attach();
      mako_local_txn *txn = nullptr;
      if (result.attach == MAKO_LOCAL_OK)
        result.begin = mako_local_txn_begin(db, &txn);
      if (result.begin == MAKO_LOCAL_OK) {
        auto read = get(txn, primary, "contended");
        result.read = read.first;
        result.read_value = std::move(read.second);
      }
      if (result.read == MAKO_LOCAL_OK &&
          result.read_value == std::optional<std::string>("0")) {
        result.contended_put =
            put(txn, primary, "contended", i == 0 ? "1" : "2");
      }
      if (result.contended_put == MAKO_LOCAL_OK) {
        result.side_put = put(txn, primary, "side-" + std::to_string(i),
                              i == 0 ? "left" : "right");
      }

      // Never return before rendezvous: a failed setup must not strand the
      // peer forever inside the barrier.
      ready.arrive_and_wait();

      if (result.side_put == MAKO_LOCAL_OK)
        result.commit =
            mako_local_txn_commit_with_hook(txn, accept_hook, &result.hook);
      if (txn != nullptr)
        result.destroy = mako_local_txn_destroy(txn);
    });
  }
  for (auto &worker : workers) worker.join();

  for (int i = 0; i != 2; ++i) {
    EXPECT_EQ(results[i].attach, MAKO_LOCAL_OK) << "worker " << i;
    EXPECT_EQ(results[i].begin, MAKO_LOCAL_OK) << "worker " << i;
    EXPECT_EQ(results[i].read, MAKO_LOCAL_OK) << "worker " << i;
    EXPECT_EQ(results[i].read_value, std::optional<std::string>("0"))
        << "worker " << i;
    EXPECT_EQ(results[i].contended_put, MAKO_LOCAL_OK) << "worker " << i;
    EXPECT_EQ(results[i].side_put, MAKO_LOCAL_OK) << "worker " << i;
    EXPECT_EQ(results[i].destroy, MAKO_LOCAL_OK) << "worker " << i;
  }

  const int commits = (results[0].commit == MAKO_LOCAL_OK) +
                      (results[1].commit == MAKO_LOCAL_OK);
  const int conflicts = (results[0].commit == MAKO_LOCAL_CONFLICT) +
                        (results[1].commit == MAKO_LOCAL_CONFLICT);
  EXPECT_EQ(commits, 1);
  EXPECT_EQ(conflicts, 1);
  EXPECT_EQ(results[0].hook.calls + results[1].hook.calls, 1);
  const int winner = results[0].commit == MAKO_LOCAL_OK ? 0 : 1;
  ASSERT_TRUE(winner == 0 || winner == 1);

  auto *verify = begin();
  EXPECT_EQ(get(verify, primary, "contended").second,
            std::optional<std::string>(winner == 0 ? "1" : "2"));
  EXPECT_EQ(get(verify, primary, "side-0").second.has_value(),
            winner == 0);
  EXPECT_EQ(get(verify, primary, "side-1").second.has_value(),
            winner == 1);
  commit_and_destroy(verify);
}

}  // namespace
