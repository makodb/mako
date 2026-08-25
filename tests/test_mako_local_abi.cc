// Contract tests for the pure-C local STO/MassTrans boundary.

#include "mako/storage/mako_local_abi.h"
#include "mako/sto/MassTrans.hh"
#include "mako/sto/Transaction.hh"
#include "mako/sto/common.hh"

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

  mako_local_db *db = nullptr;
  mako_local_table *primary = nullptr;
  mako_local_txn *txn_for_cleanup = nullptr;
  CommitPhaseObservation commit_observation;
};

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
  static_assert(sizeof(mako_local_scan_entry) == 4 * sizeof(uint32_t));
  EXPECT_EQ(mako_local_scan_options_size(),
            MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE);
  EXPECT_EQ(mako_local_scan_options_size(),
            offsetof(mako_local_scan_options, resume_len) + sizeof(size_t));
  EXPECT_EQ(mako_local_scan_entry_size(), sizeof(mako_local_scan_entry));
  EXPECT_EQ(mako_local_advance_mako_timestamp_past(0),
            MAKO_LOCAL_INVALID_ARGUMENT);
  EXPECT_EQ(mako_local_advance_mako_timestamp_past(UINT32_MAX),
            MAKO_LOCAL_TIMESTAMP_EXHAUSTED);
  static_assert(MAKO_LOCAL_MAX_MAKO_TIMESTAMP ==
                (std::numeric_limits<uint32_t>::max() - 9) / 10);
  EXPECT_EQ(mako_local_advance_mako_timestamp_past(
                MAKO_LOCAL_MAX_MAKO_TIMESTAMP),
            MAKO_LOCAL_TIMESTAMP_EXHAUSTED);
  EXPECT_EQ(mako_local_advance_mako_timestamp_past(
                MAKO_LOCAL_MAX_MAKO_TIMESTAMP + 1),
            MAKO_LOCAL_TIMESTAMP_EXHAUSTED);
  EXPECT_EQ(mako_local_db_open(nullptr), MAKO_LOCAL_INVALID_ARGUMENT);

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
      result.invalid_arm = mako_local_test_arm_cleanup_failure(0);
      result.arm = mako_local_test_arm_cleanup_failure(boundary);
      result.second_arm = mako_local_test_arm_cleanup_failure(boundary);
      txn = reinterpret_cast<mako_local_txn *>(uintptr_t{1});
      if (result.table_open == MAKO_LOCAL_OK)
        result.boundary_status = mako_local_txn_begin(db, &txn);
      result.boundary_begin_out_was_null = txn == nullptr;
      result.begin = result.boundary_status;
      result.setup = MAKO_LOCAL_OK;
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
  std::thread other([&] {
    uint8_t *value = nullptr;
    size_t value_len = 0;
    uint8_t found = 0;
    wrong_thread_status.store(mako_local_txn_get(
        txn, primary, reinterpret_cast<const uint8_t *>("key"), 3, &value,
        &value_len, &found));
    mako_local_bytes_free(value);
  });
  other.join();
  EXPECT_EQ(wrong_thread_status.load(), MAKO_LOCAL_WRONG_THREAD);

  ASSERT_EQ(mako_local_txn_commit(txn), MAKO_LOCAL_OK);
  EXPECT_EQ(put(txn, primary, "after", "commit"), MAKO_LOCAL_TXN_FINISHED);
  EXPECT_EQ(mako_local_txn_abort(txn), MAKO_LOCAL_TXN_FINISHED);
  destroy_tracked(txn);
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
