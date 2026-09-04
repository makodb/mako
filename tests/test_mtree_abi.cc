#include "mako/storage/mtree_abi.h"

#include "mako/rcu.h"
#include "mako/silo_runtime.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

static_assert(std::is_standard_layout_v<mt_runtime_config>);
static_assert(std::is_standard_layout_v<mt_build_id>);
static_assert(std::is_standard_layout_v<mt_read_scope>);
static_assert(std::is_same_v<mt_rcu_scope, mt_read_scope>);
static_assert(std::is_standard_layout_v<mt_get_or_insert_result>);
static_assert(std::is_standard_layout_v<mt_scan_bound>);
static_assert(std::is_standard_layout_v<mt_scan_entry>);
static_assert(std::is_standard_layout_v<mt_scan_result>);
static_assert(sizeof(mt_record_id) == sizeof(uint64_t));
static_assert(sizeof(mt_get_or_insert_result) == 16);
static_assert(alignof(mt_get_or_insert_result) == alignof(uint64_t));
static_assert(sizeof(mt_read_scope) == 16);
static_assert(alignof(mt_read_scope) == alignof(uint64_t));

mt_runtime *default_runtime() {
  mt_runtime_config config{};
  EXPECT_EQ(mt_runtime_config_init(&config), MT_OK);
  config.required_features =
      MT_FEATURE_POINT_GET | MT_FEATURE_ATOMIC_GET_OR_INSERT |
      MT_FEATURE_EXPLICIT_HANDLES | MT_FEATURE_BINARY_KEYS |
      MT_FEATURE_INTEGRAL_RECORD_IDS | MT_FEATURE_RUNTIME_HEALTH |
      MT_FEATURE_SINGLETON_RUNTIME | MT_FEATURE_COPIED_RANGE_SCANS |
      MT_FEATURE_SCOPED_POINT_READS | MT_FEATURE_SCOPED_STRIDED_POINT_READS |
      MT_FEATURE_STRIDED_POINT_READS | MT_FEATURE_SCOPED_RCU |
      MT_FEATURE_STRUCTURE_SEAL;
  mt_runtime *runtime = nullptr;
  EXPECT_EQ(mt_runtime_acquire(&config, &runtime), MT_OK);
  EXPECT_NE(runtime, nullptr);
  return runtime;
}

mt_thread *current_worker(mt_runtime *runtime) {
  mt_thread *worker = nullptr;
  EXPECT_EQ(mt_thread_attach(runtime, &worker), MT_OK);
  EXPECT_NE(worker, nullptr);
  return worker;
}

mt_tree *new_tree(mt_runtime *runtime, mt_thread *worker) {
  mt_tree *tree = nullptr;
  EXPECT_EQ(mt_tree_create(runtime, worker, &tree), MT_OK);
  EXPECT_NE(tree, nullptr);
  return tree;
}

mt_scan_bound absent_bound() {
  return mt_scan_bound{nullptr, 0, MT_SCAN_BOUND_ABSENT, 0};
}

mt_scan_bound inclusive_bound(const void *key, size_t key_length) {
  return mt_scan_bound{key, key_length, MT_SCAN_BOUND_INCLUSIVE, 0};
}

mt_scan_bound exclusive_bound(const void *key, size_t key_length) {
  return mt_scan_bound{key, key_length, MT_SCAN_BOUND_EXCLUSIVE, 0};
}

std::array<uint8_t, 8> ordered_key(uint64_t value) {
  std::array<uint8_t, 8> key{};
  for (size_t index = 0; index != key.size(); ++index) {
    key[index] = static_cast<uint8_t>(value >> ((key.size() - index - 1) * 8));
  }
  return key;
}

std::array<uint8_t, 24> layered_key(uint64_t value) {
  std::array<uint8_t, 24> key{};
  key.fill(UINT8_C(0xa5));
  const auto tail = ordered_key(value);
  std::copy(tail.begin(), tail.end(), key.end() - tail.size());
  return key;
}

uint64_t decode_ordered_key(const uint8_t *key) {
  uint64_t value = 0;
  for (size_t index = 0; index != 8; ++index) {
    value = (value << 8) | key[index];
  }
  return value;
}

void insert_key(mt_tree *tree, mt_thread *worker, const void *key,
                size_t key_length, mt_record_id record_id) {
  mt_get_or_insert_result result{};
  ASSERT_EQ(mt_get_or_insert(tree, worker, key, key_length, record_id, &result),
            MT_OK);
  ASSERT_EQ(result.winner, record_id);
}

std::string copied_key(const mt_scan_entry &entry, const uint8_t *arena) {
  return std::string(reinterpret_cast<const char *>(arena + entry.key_offset),
                     entry.key_length);
}

std::vector<std::string> scan_keys(mt_tree *tree, mt_thread *worker,
                                   mt_scan_direction direction,
                                   const mt_scan_bound &lower,
                                   const mt_scan_bound &upper) {
  std::array<mt_scan_entry, 32> entries{};
  std::array<uint8_t, 8192> arena{};
  mt_scan_result result{};
  const mt_status status =
      mt_scan(tree, worker, direction, &lower, &upper, entries.data(),
              entries.size(), arena.data(), arena.size(), &result);
  EXPECT_EQ(status, MT_OK);
  EXPECT_EQ(result.stop_reason, MT_SCAN_STOP_END);
  EXPECT_EQ(result.resume, MT_SCAN_RESUME_NONE);
  EXPECT_EQ(result.next_key_bytes_required, 0u);
  EXPECT_EQ(result.resume_key_offset, 0u);
  EXPECT_EQ(result.resume_key_length, 0u);
  if (status != MT_OK || result.entries_written > entries.size()) {
    return {};
  }

  std::vector<std::string> keys;
  keys.reserve(result.entries_written);
  for (size_t index = 0; index != result.entries_written; ++index) {
    EXPECT_LE(entries[index].key_offset, result.arena_bytes_used);
    if (entries[index].key_offset > result.arena_bytes_used) {
      continue;
    }
    EXPECT_LE(entries[index].key_length,
              result.arena_bytes_used - entries[index].key_offset);
    if (entries[index].key_length >
        result.arena_bytes_used - entries[index].key_offset) {
      continue;
    }
    keys.push_back(copied_key(entries[index], arena.data()));
  }
  return keys;
}

void expect_initialized_scan_result(const mt_scan_result &result) {
  EXPECT_EQ(result.entries_written, 0u);
  EXPECT_EQ(result.arena_bytes_used, 0u);
  EXPECT_EQ(result.next_key_bytes_required, 0u);
  EXPECT_EQ(result.stop_reason, MT_SCAN_STOP_END);
  EXPECT_EQ(result.resume, MT_SCAN_RESUME_NONE);
  EXPECT_EQ(result.resume_key_offset, 0u);
  EXPECT_EQ(result.resume_key_length, 0u);
  EXPECT_EQ(result.reserved[0], 0u);
  EXPECT_EQ(result.reserved[1], 0u);
}

TEST(MtreeAbiIdentity, ReportsExactPublicLayoutsAndLimits) {
  EXPECT_EQ(mt_abi_version(), MT_ABI_VERSION);
  EXPECT_EQ(mt_feature_bits() & MT_FEATURE_POINT_GET, MT_FEATURE_POINT_GET);
  EXPECT_EQ(mt_feature_bits() & MT_FEATURE_ATOMIC_GET_OR_INSERT,
            MT_FEATURE_ATOMIC_GET_OR_INSERT);
  EXPECT_EQ(mt_feature_bits() & MT_FEATURE_EXPLICIT_HANDLES,
            MT_FEATURE_EXPLICIT_HANDLES);
  EXPECT_EQ(mt_feature_bits() & MT_FEATURE_SINGLETON_RUNTIME,
            MT_FEATURE_SINGLETON_RUNTIME);
  EXPECT_EQ(mt_feature_bits() & MT_FEATURE_COPIED_RANGE_SCANS,
            MT_FEATURE_COPIED_RANGE_SCANS);
  EXPECT_EQ(mt_feature_bits() & MT_FEATURE_SCOPED_POINT_READS,
            MT_FEATURE_SCOPED_POINT_READS);
  EXPECT_EQ(mt_feature_bits() & MT_FEATURE_SCOPED_STRIDED_POINT_READS,
            MT_FEATURE_SCOPED_STRIDED_POINT_READS);
  EXPECT_EQ(mt_feature_bits() & MT_FEATURE_STRIDED_POINT_READS,
            MT_FEATURE_STRIDED_POINT_READS);
  EXPECT_EQ(mt_feature_bits() & MT_FEATURE_SCOPED_RCU,
            MT_FEATURE_SCOPED_RCU);
  EXPECT_EQ(mt_feature_bits() & MT_FEATURE_STRUCTURE_SEAL,
            MT_FEATURE_STRUCTURE_SEAL);
  EXPECT_EQ(mt_feature_bits() & MT_FEATURE_GRACEFUL_SHUTDOWN, 0u);
  EXPECT_TRUE(mt_endianness() == MT_BYTE_ORDER_LITTLE_ENDIAN ||
              mt_endianness() == MT_BYTE_ORDER_BIG_ENDIAN);
  EXPECT_EQ(mt_pointer_width(), sizeof(void *) * 8);
  EXPECT_EQ(mt_max_key_length(), MT_CONFIGURED_MAX_KEY_LENGTH);
  EXPECT_GT(mt_max_threads(), 0u);
  EXPECT_EQ(mt_record_id_limit(), std::numeric_limits<mt_record_id>::max());

  EXPECT_EQ(mt_runtime_config_size(), sizeof(mt_runtime_config));
  EXPECT_EQ(mt_runtime_config_alignment(), alignof(mt_runtime_config));
  EXPECT_EQ(mt_build_id_size(), sizeof(mt_build_id));
  EXPECT_EQ(mt_build_id_alignment(), alignof(mt_build_id));
  EXPECT_EQ(mt_read_scope_size(), sizeof(mt_read_scope));
  EXPECT_EQ(mt_read_scope_alignment(), alignof(mt_read_scope));
  EXPECT_EQ(mt_get_or_insert_result_size(), sizeof(mt_get_or_insert_result));
  EXPECT_EQ(mt_get_or_insert_result_alignment(),
            alignof(mt_get_or_insert_result));
  EXPECT_EQ(mt_scan_bound_size(), sizeof(mt_scan_bound));
  EXPECT_EQ(mt_scan_bound_alignment(), alignof(mt_scan_bound));
  EXPECT_EQ(mt_scan_entry_size(), sizeof(mt_scan_entry));
  EXPECT_EQ(mt_scan_entry_alignment(), alignof(mt_scan_entry));
  EXPECT_EQ(mt_scan_result_size(), sizeof(mt_scan_result));
  EXPECT_EQ(mt_scan_result_alignment(), alignof(mt_scan_result));
  EXPECT_EQ(mt_exported_symbols_fingerprint(), UINT64_C(0x8275e6faa88a4fe0));

  mt_build_id first{};
  mt_build_id second{};
  EXPECT_EQ(mt_get_build_fingerprint(&first), MT_OK);
  EXPECT_EQ(mt_get_build_fingerprint(&second), MT_OK);
  EXPECT_NE(first.low, 0u);
  EXPECT_NE(first.high, 0u);
  EXPECT_EQ(first.low, second.low);
  EXPECT_EQ(first.high, second.high);
  EXPECT_EQ(mt_get_build_fingerprint(nullptr), MT_ERR_INVALID);
}

TEST(MtreeAbiLifecycle, SingletonIsExplicitAndIncompatibleConfigIsRejected) {
  mt_runtime *runtime = default_runtime();
  ASSERT_NE(runtime, nullptr);

  mt_runtime *same_runtime = nullptr;
  EXPECT_EQ(mt_runtime_acquire(nullptr, &same_runtime), MT_OK);
  EXPECT_EQ(same_runtime, runtime);

  mt_runtime_health_state health = 0;
  EXPECT_EQ(mt_runtime_health(runtime, &health), MT_OK);
  EXPECT_EQ(health, MT_RUNTIME_HEALTHY);

  size_t max_key = 0;
  uint32_t max_workers = 0;
  EXPECT_EQ(mt_runtime_max_key_length(runtime, &max_key), MT_OK);
  EXPECT_EQ(mt_runtime_max_threads(runtime, &max_workers), MT_OK);
  EXPECT_EQ(max_key, MT_CONFIGURED_MAX_KEY_LENGTH);
  EXPECT_EQ(max_workers, mt_max_threads());

  mt_runtime_config incompatible{};
  ASSERT_EQ(mt_runtime_config_init(&incompatible), MT_OK);
  ASSERT_GT(mt_max_threads(), 1u);
  incompatible.max_threads = mt_max_threads() - 1;
  mt_runtime *rejected = reinterpret_cast<mt_runtime *>(uintptr_t{1});
  EXPECT_EQ(mt_runtime_acquire(&incompatible, &rejected),
            MT_ERR_INCOMPATIBLE_RUNTIME);
  EXPECT_EQ(rejected, nullptr);

  mt_runtime_config unsupported{};
  ASSERT_EQ(mt_runtime_config_init(&unsupported), MT_OK);
  unsupported.required_features = MT_FEATURE_GRACEFUL_SHUTDOWN;
  EXPECT_EQ(mt_runtime_acquire(&unsupported, &rejected), MT_ERR_ABI_MISMATCH);
  EXPECT_EQ(rejected, nullptr);

  mt_thread *worker = current_worker(runtime);
  ASSERT_NE(worker, nullptr);
  EXPECT_EQ(mt_thread_quiesce(worker), MT_OK);
  EXPECT_EQ(mt_runtime_shutdown(runtime, worker), MT_ERR_UNSUPPORTED);
}

TEST(MtreeAbiPoint, EmptyAndBinaryKeysRoundTripWithoutPointerValues) {
  mt_runtime *runtime = default_runtime();
  mt_thread *worker = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, worker);
  ASSERT_NE(tree, nullptr);

  mt_record_id value = 99;
  EXPECT_EQ(mt_get(tree, worker, nullptr, 0, &value), MT_OK);
  EXPECT_EQ(value, MT_RECORD_ID_NONE);

  mt_get_or_insert_result inserted{};
  EXPECT_EQ(mt_get_or_insert(tree, worker, nullptr, 0, 41, &inserted), MT_OK);
  EXPECT_EQ(inserted.winner, 41u);
  EXPECT_EQ(inserted.inserted, 1u);
  EXPECT_EQ(inserted.publication, MT_PUBLICATION_CANDIDATE_INSERTED);

  const uint8_t arbitrary_nonnull_byte = 0xff;
  EXPECT_EQ(mt_get(tree, worker, &arbitrary_nonnull_byte, 0, &value), MT_OK);
  EXPECT_EQ(value, 41u);

  const std::array<uint8_t, 6> binary_key = {0x00, 0xff, 0x00,
                                             0x7f, 0x80, 0x01};
  EXPECT_EQ(mt_get_or_insert(tree, worker, binary_key.data(), binary_key.size(),
                             73, &inserted),
            MT_OK);
  EXPECT_EQ(inserted.winner, 73u);
  EXPECT_EQ(mt_get(tree, worker, binary_key.data(), binary_key.size(), &value),
            MT_OK);
  EXPECT_EQ(value, 73u);

  value = 99;
  EXPECT_EQ(mt_get(tree, worker, nullptr, 1, &value), MT_ERR_INVALID);
  EXPECT_EQ(value, MT_RECORD_ID_NONE);
}

TEST(MtreeAbiPoint, WinnerAndPublicationDispositionAreUnambiguous) {
  mt_runtime *runtime = default_runtime();
  mt_thread *worker = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, worker);
  const std::array<uint8_t, 4> key = {'w', 0, 'i', 'n'};

  mt_get_or_insert_result first{};
  ASSERT_EQ(
      mt_get_or_insert(tree, worker, key.data(), key.size(), 1001, &first),
      MT_OK);
  EXPECT_EQ(first.winner, 1001u);
  EXPECT_EQ(first.inserted, 1u);
  EXPECT_EQ(first.publication, MT_PUBLICATION_CANDIDATE_INSERTED);

  mt_get_or_insert_result loser{};
  ASSERT_EQ(
      mt_get_or_insert(tree, worker, key.data(), key.size(), 2002, &loser),
      MT_OK);
  EXPECT_EQ(loser.winner, 1001u);
  EXPECT_EQ(loser.inserted, 0u);
  EXPECT_EQ(loser.publication, MT_PUBLICATION_CANDIDATE_PROVEN_UNPUBLISHED);

  mt_get_or_insert_result invalid{};
  invalid.winner = 123;
  invalid.inserted = 1;
  invalid.publication = MT_PUBLICATION_UNKNOWN;
  EXPECT_EQ(mt_get_or_insert(tree, worker, key.data(), key.size(), 0, &invalid),
            MT_ERR_INVALID);
  EXPECT_EQ(invalid.winner, MT_RECORD_ID_NONE);
  EXPECT_EQ(invalid.inserted, 0u);
  EXPECT_EQ(invalid.publication, MT_PUBLICATION_FAILURE_BEFORE_PUBLICATION);
}

TEST(MtreeAbiPoint, StructureSealIsIdempotentAndPreservesReadsAndScans) {
  mt_runtime *runtime = default_runtime();
  mt_thread *worker = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, worker);
  const auto present = ordered_key(31);
  const auto missing = ordered_key(32);
  insert_key(tree, worker, present.data(), present.size(), 310);

  EXPECT_EQ(mt_tree_seal_structure(nullptr), MT_ERR_INVALID);
  EXPECT_EQ(mt_tree_seal_structure(
                reinterpret_cast<mt_tree *>(uintptr_t{1})),
            MT_ERR_INVALID);
  EXPECT_EQ(mt_tree_seal_structure(tree), MT_OK);
  EXPECT_EQ(mt_tree_seal_structure(tree), MT_OK);

  mt_record_id found = 99;
  EXPECT_EQ(mt_get(tree, worker, present.data(), present.size(), &found), MT_OK);
  EXPECT_EQ(found, 310u);
  std::array<std::array<uint8_t, 8>, 2> keys = {present, missing};
  std::array<mt_record_id, 2> found_batch = {99, 99};
  EXPECT_EQ(mt_get_strided(tree, worker, keys.data(), keys.size(),
                           keys[0].size(), sizeof(keys[0]), found_batch.data()),
            MT_OK);
  EXPECT_EQ(found_batch, (std::array<mt_record_id, 2>{310, 0}));

  const mt_scan_bound absent = absent_bound();
  std::array<mt_scan_entry, 2> entries{};
  std::array<uint8_t, 32> arena{};
  mt_scan_result scan{};
  EXPECT_EQ(mt_scan(tree, worker, MT_SCAN_FORWARD, &absent, &absent,
                    entries.data(), entries.size(), arena.data(), arena.size(),
                    &scan),
            MT_OK);
  ASSERT_EQ(scan.entries_written, 1u);
  EXPECT_EQ(entries[0].record_id, 310u);

  for (const auto &key : keys) {
    mt_get_or_insert_result rejected;
    std::memset(&rejected, 0xa5, sizeof(rejected));
    EXPECT_EQ(mt_get_or_insert(tree, worker, key.data(), key.size(), 311,
                               &rejected),
              MT_ERR_STRUCTURE_SEALED);
    EXPECT_EQ(rejected.winner, MT_RECORD_ID_NONE);
    EXPECT_EQ(rejected.inserted, 0u);
    EXPECT_EQ(rejected.publication,
              MT_PUBLICATION_FAILURE_BEFORE_PUBLICATION);
    EXPECT_EQ(rejected.reserved[0], 0u);
    EXPECT_EQ(rejected.reserved[1], 0u);
    EXPECT_EQ(rejected.reserved[2], 0u);
  }
  found = 99;
  EXPECT_EQ(mt_get(tree, worker, missing.data(), missing.size(), &found), MT_OK);
  EXPECT_EQ(found, MT_RECORD_ID_NONE);

  EXPECT_EQ(mt_tree_release(tree), MT_OK);
  EXPECT_EQ(mt_tree_seal_structure(tree), MT_ERR_CLOSED);
}

TEST(MtreeAbiPoint, StructureSealDrainsAnActiveReaderThenReadersBypassIt) {
  mt_runtime *runtime = default_runtime();
  mt_thread *reader = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, reader);
  const auto key = ordered_key(41);
  insert_key(tree, reader, key.data(), key.size(), 410);

  mt_read_scope scope{};
  ASSERT_EQ(mt_read_scope_begin(tree, reader, &scope), MT_OK);
  EXPECT_EQ(mt_tree_seal_structure(tree), MT_ERR_ACTIVE_GUARDS);

  std::barrier ready(2);
  std::atomic<bool> seal_started{false};
  std::atomic<bool> seal_returned{false};
  std::atomic<mt_status> seal_status{MT_ERR_INTERNAL};
  std::thread sealer([&]() {
    ready.arrive_and_wait();
    seal_started.store(true, std::memory_order_release);
    seal_status.store(mt_tree_seal_structure(tree), std::memory_order_release);
    seal_returned.store(true, std::memory_order_release);
  });

  ready.arrive_and_wait();
  while (!seal_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  for (size_t iteration = 0; iteration != 256; ++iteration) {
    mt_record_id found = 99;
    ASSERT_EQ(mt_read_scope_get(&scope, key.data(), key.size(), &found), MT_OK);
    ASSERT_EQ(found, 410u);
    std::this_thread::yield();
  }
  EXPECT_FALSE(seal_returned.load(std::memory_order_acquire));

  ASSERT_EQ(mt_read_scope_end(&scope), MT_OK);
  sealer.join();
  EXPECT_TRUE(seal_returned.load(std::memory_order_acquire));
  EXPECT_EQ(seal_status.load(std::memory_order_acquire), MT_OK);

  /* A post-seal scope keeps native RCU but needs no structural admission. */
  ASSERT_EQ(mt_read_scope_begin(tree, reader, &scope), MT_OK);
  mt_record_id found = 99;
  EXPECT_EQ(mt_read_scope_get(&scope, key.data(), key.size(), &found), MT_OK);
  EXPECT_EQ(found, 410u);
  EXPECT_EQ(mt_read_scope_end(&scope), MT_OK);
}

TEST(MtreeAbiPoint, StructureSealRacingWithInsertionHasOneLinearizedWinner) {
  mt_runtime *runtime = default_runtime();
  mt_thread *creator = current_worker(runtime);
  constexpr size_t kTreeCount = 128;
  const auto key = ordered_key(51);
  std::vector<mt_tree *> trees;
  trees.reserve(kTreeCount);
  for (size_t index = 0; index != kTreeCount; ++index) {
    trees.push_back(new_tree(runtime, creator));
  }

  std::barrier begin(2);
  std::barrier complete(2);
  std::atomic<mt_status> attach_status{MT_ERR_INTERNAL};
  std::atomic<mt_status> quiesce_status{MT_ERR_INTERNAL};
  std::array<mt_status, kTreeCount> insert_statuses{};
  std::array<mt_get_or_insert_result, kTreeCount> insert_results{};
  std::thread writer([&]() {
    mt_thread *worker = nullptr;
    attach_status.store(mt_thread_attach(runtime, &worker),
                        std::memory_order_release);
    for (size_t index = 0; index != kTreeCount; ++index) {
      begin.arrive_and_wait();
      if ((index & 1) == 0) {
        std::this_thread::yield();
      }
      if (attach_status.load(std::memory_order_acquire) == MT_OK) {
        insert_statuses[index] = mt_get_or_insert(
            trees[index], worker, key.data(), key.size(), index + 1,
            &insert_results[index]);
      }
      complete.arrive_and_wait();
    }
    if (attach_status.load(std::memory_order_acquire) == MT_OK) {
      quiesce_status.store(mt_thread_quiesce(worker),
                           std::memory_order_release);
    }
  });

  for (size_t index = 0; index != kTreeCount; ++index) {
    begin.arrive_and_wait();
    if ((index & 1) != 0) {
      std::this_thread::yield();
    }
    ASSERT_EQ(mt_tree_seal_structure(trees[index]), MT_OK);
    complete.arrive_and_wait();

    ASSERT_EQ(attach_status.load(std::memory_order_acquire), MT_OK);
    const mt_status insert_status = insert_statuses[index];
    EXPECT_TRUE(insert_status == MT_OK ||
                insert_status == MT_ERR_STRUCTURE_SEALED);
    if (insert_status == MT_OK) {
      EXPECT_EQ(insert_results[index].winner, index + 1);
      EXPECT_EQ(insert_results[index].inserted, 1u);
      EXPECT_EQ(insert_results[index].publication,
                MT_PUBLICATION_CANDIDATE_INSERTED);
    } else {
      EXPECT_EQ(insert_results[index].winner, MT_RECORD_ID_NONE);
      EXPECT_EQ(insert_results[index].inserted, 0u);
      EXPECT_EQ(insert_results[index].publication,
                MT_PUBLICATION_FAILURE_BEFORE_PUBLICATION);
    }

    mt_get_or_insert_result after{};
    EXPECT_EQ(mt_get_or_insert(trees[index], creator, key.data(), key.size(),
                               kTreeCount + index + 1, &after),
              MT_ERR_STRUCTURE_SEALED);
    EXPECT_EQ(after.winner, MT_RECORD_ID_NONE);
    EXPECT_EQ(after.inserted, 0u);
    EXPECT_EQ(after.publication,
              MT_PUBLICATION_FAILURE_BEFORE_PUBLICATION);

    mt_record_id found = 99;
    EXPECT_EQ(mt_get(trees[index], creator, key.data(), key.size(), &found),
              MT_OK);
    EXPECT_EQ(found, insert_status == MT_OK ? index + 1 : MT_RECORD_ID_NONE);
  }
  writer.join();
  EXPECT_EQ(quiesce_status.load(std::memory_order_acquire), MT_OK);
}

TEST(MtreeAbiPoint, KeyLimitIsCheckedBeforeMasstreeSeesTheInput) {
  mt_runtime *runtime = default_runtime();
  mt_thread *worker = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, worker);
  std::vector<uint8_t> maximum(mt_max_key_length(), 0xa5);
  std::vector<uint8_t> oversized(mt_max_key_length() + 1, 0x5a);

  mt_get_or_insert_result result{};
  ASSERT_EQ(mt_get_or_insert(tree, worker, maximum.data(), maximum.size(), 9,
                             &result),
            MT_OK);
  EXPECT_EQ(result.winner, 9u);

  result.winner = 99;
  result.inserted = 1;
  result.publication = MT_PUBLICATION_UNKNOWN;
  EXPECT_EQ(mt_get_or_insert(tree, worker, oversized.data(), oversized.size(),
                             10, &result),
            MT_ERR_KEY_TOO_LARGE);
  EXPECT_EQ(result.winner, MT_RECORD_ID_NONE);
  EXPECT_EQ(result.inserted, 0u);
  EXPECT_EQ(result.publication, MT_PUBLICATION_FAILURE_BEFORE_PUBLICATION);

  mt_record_id value = 99;
  EXPECT_EQ(mt_get(tree, worker, oversized.data(), oversized.size(), &value),
            MT_ERR_KEY_TOO_LARGE);
  EXPECT_EQ(value, MT_RECORD_ID_NONE);
}

TEST(MtreeAbiPoint, ScopedReadsRetainGuardsAndRejectStaleGenerations) {
  mt_runtime *runtime = default_runtime();
  mt_thread *worker = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, worker);
  const std::array<uint8_t, 4> present = {'s', 0, 'c', 'p'};
  const std::array<uint8_t, 4> missing = {'m', 'i', 's', 's'};
  insert_key(tree, worker, present.data(), present.size(), 91);

  mt_read_scope scope;
  std::memset(&scope, 0xa5, sizeof(scope));
  ASSERT_EQ(mt_read_scope_begin(tree, worker, &scope), MT_OK);
  ASSERT_NE(scope.owner, 0u);
  ASSERT_NE(scope.generation, 0u);
  const mt_read_scope stale = scope;

  mt_record_id found = 99;
  EXPECT_EQ(mt_read_scope_get(&scope, present.data(), present.size(), &found),
            MT_OK);
  EXPECT_EQ(found, 91u);
  EXPECT_EQ(mt_read_scope_get(&scope, missing.data(), missing.size(), &found),
            MT_OK);
  EXPECT_EQ(found, MT_RECORD_ID_NONE);

  std::vector<uint8_t> oversized(mt_max_key_length() + 1, 0xa5);
  found = 99;
  EXPECT_EQ(
      mt_read_scope_get(&scope, oversized.data(), oversized.size(), &found),
      MT_ERR_KEY_TOO_LARGE);
  EXPECT_EQ(found, MT_RECORD_ID_NONE);
  EXPECT_EQ(mt_read_scope_get(&scope, present.data(), present.size(), &found),
            MT_OK);
  EXPECT_EQ(found, 91u);

  mt_read_scope nested{uintptr_t{1}, UINT64_C(1)};
  EXPECT_EQ(mt_read_scope_begin(tree, worker, &nested), MT_ERR_ACTIVE_GUARDS);
  EXPECT_EQ(nested.owner, 0u);
  EXPECT_EQ(nested.generation, 0u);
  mt_rcu_scope blocked_rcu{uintptr_t{1}, UINT64_C(1)};
  EXPECT_EQ(mt_rcu_scope_begin(worker, &blocked_rcu), MT_ERR_ACTIVE_GUARDS);
  EXPECT_EQ(blocked_rcu.owner, 0u);
  EXPECT_EQ(blocked_rcu.generation, 0u);

  found = 99;
  EXPECT_EQ(mt_get(tree, worker, present.data(), present.size(), &found),
            MT_ERR_ACTIVE_GUARDS);
  EXPECT_EQ(found, MT_RECORD_ID_NONE);
  mt_get_or_insert_result blocked_insert;
  std::memset(&blocked_insert, 0xa5, sizeof(blocked_insert));
  EXPECT_EQ(mt_get_or_insert(tree, worker, missing.data(), missing.size(), 92,
                             &blocked_insert),
            MT_ERR_ACTIVE_GUARDS);
  EXPECT_EQ(blocked_insert.winner, MT_RECORD_ID_NONE);
  EXPECT_EQ(blocked_insert.inserted, 0u);
  EXPECT_EQ(blocked_insert.publication,
            MT_PUBLICATION_FAILURE_BEFORE_PUBLICATION);
  const mt_scan_bound absent = absent_bound();
  mt_scan_entry entry{};
  std::array<uint8_t, 8> arena{};
  mt_scan_result scan_result;
  std::memset(&scan_result, 0xa5, sizeof(scan_result));
  EXPECT_EQ(mt_scan(tree, worker, MT_SCAN_FORWARD, &absent, &absent, &entry, 1,
                    arena.data(), arena.size(), &scan_result),
            MT_ERR_ACTIVE_GUARDS);
  expect_initialized_scan_result(scan_result);
  mt_tree *blocked_tree = reinterpret_cast<mt_tree *>(uintptr_t{1});
  EXPECT_EQ(mt_tree_create(runtime, worker, &blocked_tree),
            MT_ERR_ACTIVE_GUARDS);
  EXPECT_EQ(blocked_tree, nullptr);
  mt_thread *blocked_attach = reinterpret_cast<mt_thread *>(uintptr_t{1});
  EXPECT_EQ(mt_thread_attach(runtime, &blocked_attach), MT_ERR_ACTIVE_GUARDS);
  EXPECT_EQ(blocked_attach, nullptr);
  EXPECT_EQ(mt_runtime_shutdown(runtime, worker), MT_ERR_ACTIVE_GUARDS);
  EXPECT_EQ(mt_tree_release(tree), MT_ERR_ACTIVE_GUARDS);
  EXPECT_EQ(mt_thread_quiesce(worker), MT_ERR_ACTIVE_GUARDS);

  EXPECT_EQ(mt_read_scope_end(&scope), MT_OK);
  EXPECT_EQ(scope.owner, 0u);
  EXPECT_EQ(scope.generation, 0u);
  found = 99;
  EXPECT_EQ(mt_read_scope_get(&stale, present.data(), present.size(), &found),
            MT_ERR_INVALID);
  EXPECT_EQ(found, MT_RECORD_ID_NONE);
  mt_read_scope stale_for_end = stale;
  EXPECT_EQ(mt_read_scope_end(&stale_for_end), MT_ERR_INVALID);

  ASSERT_EQ(mt_get_or_insert(tree, worker, missing.data(), missing.size(), 92,
                             &blocked_insert),
            MT_OK);
  EXPECT_EQ(blocked_insert.winner, 92u);

  mt_read_scope next{};
  ASSERT_EQ(mt_read_scope_begin(tree, worker, &next), MT_OK);
  EXPECT_EQ(next.owner, stale.owner);
  EXPECT_NE(next.generation, stale.generation);
  found = 99;
  EXPECT_EQ(mt_read_scope_get(&stale, present.data(), present.size(), &found),
            MT_ERR_INVALID);
  EXPECT_EQ(found, MT_RECORD_ID_NONE);
  EXPECT_EQ(mt_read_scope_get(&next, missing.data(), missing.size(), &found),
            MT_OK);
  EXPECT_EQ(found, 92u);
  EXPECT_EQ(mt_read_scope_end(&next), MT_OK);
  EXPECT_EQ(mt_thread_quiesce(worker), MT_OK);
}

TEST(MtreeAbiPoint, WorkerRcuScopeSpansTreesReadsWritesAndScans) {
  mt_runtime *runtime = default_runtime();
  mt_thread *worker = current_worker(runtime);
  mt_tree *first_tree = new_tree(runtime, worker);
  mt_tree *second_tree = new_tree(runtime, worker);
  const auto first_key = ordered_key(401);
  const auto second_key = ordered_key(402);
  insert_key(first_tree, worker, first_key.data(), first_key.size(), 401);

  mt_rcu_scope scope;
  std::memset(&scope, 0xa5, sizeof(scope));
  ASSERT_EQ(mt_rcu_scope_begin(worker, &scope), MT_OK);
  ASSERT_NE(scope.owner, 0u);
  ASSERT_NE(scope.generation, 0u);
  EXPECT_TRUE(rcu::s_instance.in_rcu_region());
  const mt_rcu_scope stale = scope;
  const mt_read_scope wrong_family = scope;

  mt_record_id wrong_family_output = 99;
  EXPECT_EQ(mt_read_scope_get(&wrong_family, first_key.data(), first_key.size(),
                              &wrong_family_output),
            MT_ERR_INVALID);
  EXPECT_EQ(wrong_family_output, MT_RECORD_ID_NONE);

  mt_record_id found = 99;
  EXPECT_EQ(mt_get(first_tree, worker, first_key.data(), first_key.size(),
                   &found),
            MT_OK);
  EXPECT_EQ(found, 401u);
  EXPECT_TRUE(rcu::s_instance.in_rcu_region());

  std::array<mt_record_id, 1> strided{MT_RECORD_ID_NONE};
  EXPECT_EQ(mt_get_strided(first_tree, worker, first_key.data(), strided.size(),
                           first_key.size(), first_key.size(), strided.data()),
            MT_OK);
  EXPECT_EQ(strided[0], 401u);
  EXPECT_TRUE(rcu::s_instance.in_rcu_region());

  mt_get_or_insert_result inserted{};
  EXPECT_EQ(mt_get_or_insert(second_tree, worker, second_key.data(),
                             second_key.size(), 402, &inserted),
            MT_OK);
  EXPECT_EQ(inserted.winner, 402u);
  EXPECT_EQ(inserted.inserted, 1u);
  EXPECT_TRUE(rcu::s_instance.in_rcu_region());

  mt_get_or_insert_result existing{};
  EXPECT_EQ(mt_get_or_insert(second_tree, worker, second_key.data(),
                             second_key.size(), 999, &existing),
            MT_OK);
  EXPECT_EQ(existing.winner, 402u);
  EXPECT_EQ(existing.inserted, 0u);
  EXPECT_EQ(existing.publication,
            MT_PUBLICATION_CANDIDATE_PROVEN_UNPUBLISHED);
  EXPECT_TRUE(rcu::s_instance.in_rcu_region());

  found = 99;
  EXPECT_EQ(mt_get(second_tree, worker, second_key.data(), second_key.size(),
                   &found),
            MT_OK);
  EXPECT_EQ(found, 402u);

  const mt_scan_bound absent = absent_bound();
  mt_scan_entry entry{};
  std::array<uint8_t, 16> arena{};
  mt_scan_result scan{};
  EXPECT_EQ(mt_scan(second_tree, worker, MT_SCAN_FORWARD, &absent, &absent,
                    &entry, 1, arena.data(), arena.size(), &scan),
            MT_OK);
  EXPECT_EQ(scan.entries_written, 1u);
  EXPECT_EQ(entry.record_id, 402u);
  EXPECT_TRUE(rcu::s_instance.in_rcu_region());

  scan = mt_scan_result{};
  EXPECT_EQ(mt_scan(second_tree, worker, MT_SCAN_REVERSE, &absent, &absent,
                    &entry, 1, arena.data(), arena.size(), &scan),
            MT_OK);
  EXPECT_EQ(scan.entries_written, 1u);
  EXPECT_EQ(entry.record_id, 402u);
  EXPECT_TRUE(rcu::s_instance.in_rcu_region());

  found = 99;
  EXPECT_EQ(mt_get(first_tree, worker, nullptr, 1, &found), MT_ERR_INVALID);
  EXPECT_EQ(found, MT_RECORD_ID_NONE);
  EXPECT_TRUE(rcu::s_instance.in_rcu_region());
  EXPECT_EQ(mt_get(first_tree, worker, first_key.data(), first_key.size(),
                   &found),
            MT_OK);
  EXPECT_EQ(found, 401u);

  mt_rcu_scope nested{uintptr_t{1}, UINT64_C(1)};
  EXPECT_EQ(mt_rcu_scope_begin(worker, &nested), MT_ERR_ACTIVE_GUARDS);
  EXPECT_EQ(nested.owner, 0u);
  EXPECT_EQ(nested.generation, 0u);
  mt_read_scope read_scope{uintptr_t{1}, UINT64_C(1)};
  EXPECT_EQ(mt_read_scope_begin(first_tree, worker, &read_scope),
            MT_ERR_ACTIVE_GUARDS);
  EXPECT_EQ(read_scope.owner, 0u);
  EXPECT_EQ(read_scope.generation, 0u);

  mt_tree *blocked_tree = reinterpret_cast<mt_tree *>(uintptr_t{1});
  EXPECT_EQ(mt_tree_create(runtime, worker, &blocked_tree),
            MT_ERR_ACTIVE_GUARDS);
  EXPECT_EQ(blocked_tree, nullptr);
  mt_thread *blocked_attach = reinterpret_cast<mt_thread *>(uintptr_t{1});
  EXPECT_EQ(mt_thread_attach(runtime, &blocked_attach), MT_ERR_ACTIVE_GUARDS);
  EXPECT_EQ(blocked_attach, nullptr);
  EXPECT_EQ(mt_runtime_shutdown(runtime, worker), MT_ERR_ACTIVE_GUARDS);
  EXPECT_EQ(mt_thread_quiesce(worker), MT_ERR_ACTIVE_GUARDS);

  EXPECT_EQ(mt_rcu_scope_end(&scope), MT_OK);
  EXPECT_EQ(scope.owner, 0u);
  EXPECT_EQ(scope.generation, 0u);
  EXPECT_FALSE(rcu::s_instance.in_rcu_region());

  /* Standalone calls still create and release their own local RCU region. */
  found = MT_RECORD_ID_NONE;
  EXPECT_EQ(mt_get(first_tree, worker, first_key.data(), first_key.size(),
                   &found),
            MT_OK);
  EXPECT_EQ(found, 401u);
  EXPECT_FALSE(rcu::s_instance.in_rcu_region());
  mt_rcu_scope stale_for_end = stale;
  EXPECT_EQ(mt_rcu_scope_end(&stale_for_end), MT_ERR_INVALID);

  mt_rcu_scope next{};
  ASSERT_EQ(mt_rcu_scope_begin(worker, &next), MT_OK);
  EXPECT_EQ(next.owner, stale.owner);
  EXPECT_NE(next.generation, stale.generation);
  EXPECT_EQ(mt_rcu_scope_end(&next), MT_OK);
  EXPECT_EQ(mt_thread_quiesce(worker), MT_OK);
}

TEST(MtreeAbiPoint, WorkerRcuScopeDoesNotRetainStructuralAdmission) {
  mt_runtime *runtime = default_runtime();
  mt_thread *reader = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, reader);
  const auto stable_key = layered_key(601);
  const auto inserted_key = layered_key(602);
  insert_key(tree, reader, stable_key.data(), stable_key.size(), 601);

  std::barrier attached(2);
  std::barrier start_write(2);
  std::atomic<mt_status> attach_status{MT_ERR_INTERNAL};
  std::atomic<mt_status> insert_status{MT_ERR_INTERNAL};
  std::atomic<mt_status> quiesce_status{MT_ERR_INTERNAL};
  std::atomic<bool> writer_returned{false};
  mt_get_or_insert_result inserted{};
  std::thread writer([&]() {
    mt_thread *worker = nullptr;
    attach_status.store(mt_thread_attach(runtime, &worker),
                        std::memory_order_release);
    attached.arrive_and_wait();
    start_write.arrive_and_wait();
    if (attach_status.load(std::memory_order_acquire) != MT_OK) {
      return;
    }
    insert_status.store(mt_get_or_insert(tree, worker, inserted_key.data(),
                                         inserted_key.size(), 602, &inserted),
                        std::memory_order_release);
    writer_returned.store(true, std::memory_order_release);
    quiesce_status.store(mt_thread_quiesce(worker), std::memory_order_release);
  });

  attached.arrive_and_wait();
  if (attach_status.load(std::memory_order_acquire) != MT_OK) {
    start_write.arrive_and_wait();
    writer.join();
    FAIL() << "writer worker attachment failed";
  }

  mt_rcu_scope scope{};
  const mt_status begin_status = mt_rcu_scope_begin(reader, &scope);
  if (begin_status != MT_OK) {
    start_write.arrive_and_wait();
    writer.join();
    FAIL() << "reader RCU scope begin failed: " << begin_status;
  }
  mt_record_id found = MT_RECORD_ID_NONE;
  const mt_status get_status =
      mt_get(tree, reader, stable_key.data(), stable_key.size(), &found);
  if (get_status != MT_OK || found != 601u) {
    EXPECT_EQ(mt_rcu_scope_end(&scope), MT_OK);
    start_write.arrive_and_wait();
    writer.join();
    FAIL() << "scoped reader setup failed: status=" << get_status
           << ", value=" << found;
  }
  start_write.arrive_and_wait();

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!writer_returned.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const bool returned_while_scope_active =
      writer_returned.load(std::memory_order_acquire);

  EXPECT_EQ(mt_rcu_scope_end(&scope), MT_OK);
  writer.join();
  EXPECT_TRUE(returned_while_scope_active)
      << "worker RCU scope retained per-operation structural admission";
  EXPECT_EQ(insert_status.load(std::memory_order_acquire), MT_OK);
  EXPECT_EQ(inserted.winner, 602u);
  EXPECT_EQ(inserted.inserted, 1u);
  EXPECT_EQ(quiesce_status.load(std::memory_order_acquire), MT_OK);
}

TEST(MtreeAbiPoint, WorkerRcuTokenCannotCrossAnOsThread) {
  mt_runtime *runtime = default_runtime();
  mt_thread *worker = current_worker(runtime);
  mt_rcu_scope scope{};
  ASSERT_EQ(mt_rcu_scope_begin(worker, &scope), MT_OK);
  mt_rcu_scope copied = scope;
  std::atomic<mt_status> status{MT_OK};
  std::thread other([&]() {
    status.store(mt_rcu_scope_end(&copied), std::memory_order_release);
  });
  other.join();
  EXPECT_EQ(status.load(std::memory_order_acquire), MT_ERR_INVALID);
  EXPECT_EQ(mt_rcu_scope_end(&scope), MT_OK);
}

TEST(MtreeAbiPoint, OneShotStridedReadsValidateOnceAndReleaseEveryGuard) {
  mt_runtime *runtime = default_runtime();
  mt_thread *worker = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, worker);
  const std::array<uint8_t, 4> first = {'o', 0, '0', '1'};
  const std::array<uint8_t, 4> missing = {'o', 0, '0', '2'};
  const std::array<uint8_t, 4> third = {'o', 0, '0', '3'};
  const std::array<uint8_t, 4> fourth = {'o', 0, '0', '4'};
  insert_key(tree, worker, first.data(), first.size(), 201);
  insert_key(tree, worker, third.data(), third.size(), 203);
  insert_key(tree, worker, nullptr, 0, 204);

  struct padded_key {
    std::array<uint8_t, 4> key;
    std::array<uint8_t, 5> padding;
  };
  static_assert(offsetof(padded_key, key) == 0);
  const std::array<padded_key, 3> keys = {
      padded_key{first, {}}, padded_key{missing, {}}, padded_key{third, {}}};

  std::array<mt_record_id, 3> found = {99, 99, 99};
  EXPECT_EQ(mt_get_strided(tree, worker, keys.front().key.data(), keys.size(),
                           first.size(), sizeof(padded_key), found.data()),
            MT_OK);
  EXPECT_EQ(found, (std::array<mt_record_id, 3>{201, 0, 203}));

  /* Both native guards end inside the one-shot boundary. */
  EXPECT_EQ(mt_thread_quiesce(worker), MT_OK);
  mt_get_or_insert_result inserted{};
  EXPECT_EQ(mt_get_or_insert(tree, worker, fourth.data(), fourth.size(), 205,
                             &inserted),
            MT_OK);
  EXPECT_EQ(inserted.winner, 205u);

  /* Empty keys need no input storage and retain one result per lookup. */
  found = {99, 99, 99};
  EXPECT_EQ(
      mt_get_strided(tree, worker, nullptr, found.size(), 0, 0, found.data()),
      MT_OK);
  EXPECT_EQ(found, (std::array<mt_record_id, 3>{204, 204, 204}));

  /* A zero-count call still validates handles and the common key shape. */
  EXPECT_EQ(mt_get_strided(tree, worker, nullptr, 0, first.size(), first.size(),
                           nullptr),
            MT_OK);
  EXPECT_EQ(mt_get_strided(nullptr, worker, nullptr, 0, 0, 0, nullptr),
            MT_ERR_INVALID);
  EXPECT_EQ(mt_get_strided(tree, worker, nullptr, 0, mt_max_key_length() + 1,
                           mt_max_key_length() + 1, nullptr),
            MT_ERR_KEY_TOO_LARGE);

  found = {99, 99, 99};
  EXPECT_EQ(mt_get_strided(tree, worker, nullptr, found.size(), first.size(),
                           first.size(), found.data()),
            MT_ERR_INVALID);
  EXPECT_EQ(found, (std::array<mt_record_id, 3>{0, 0, 0}));

  found = {99, 99, 99};
  EXPECT_EQ(mt_get_strided(tree, worker, keys.front().key.data(), found.size(),
                           first.size(), first.size() - 1, found.data()),
            MT_ERR_INVALID);
  EXPECT_EQ(found, (std::array<mt_record_id, 3>{0, 0, 0}));

  found = {99, 99, 99};
  EXPECT_EQ(mt_get_strided(tree, worker, keys.front().key.data(), found.size(),
                           mt_max_key_length() + 1, mt_max_key_length() + 1,
                           found.data()),
            MT_ERR_KEY_TOO_LARGE);
  EXPECT_EQ(found, (std::array<mt_record_id, 3>{0, 0, 0}));

  std::array<mt_record_id, 2> overflow_found = {99, 99};
  EXPECT_EQ(mt_get_strided(tree, worker, first.data(), overflow_found.size(), 1,
                           std::numeric_limits<size_t>::max(),
                           overflow_found.data()),
            MT_ERR_INVALID);
  EXPECT_EQ(overflow_found, (std::array<mt_record_id, 2>{0, 0}));

  found = {99, 99, 99};
  const void *wrapping_keys = reinterpret_cast<const void *>(
      std::numeric_limits<uintptr_t>::max() - first.size() + 2);
  EXPECT_EQ(mt_get_strided(tree, worker, wrapping_keys, found.size(),
                           first.size(), first.size(), found.data()),
            MT_ERR_INVALID);
  EXPECT_EQ(found, (std::array<mt_record_id, 3>{0, 0, 0}));

  auto *wrapping_output = reinterpret_cast<mt_record_id *>(
      std::numeric_limits<uintptr_t>::max() - sizeof(mt_record_id) + 2);
  EXPECT_EQ(mt_get_strided(tree, worker, first.data(), 1, first.size(),
                           first.size(), wrapping_output),
            MT_ERR_INVALID);
  EXPECT_EQ(mt_get_strided(tree, worker, first.data(), 1, first.size(),
                           first.size(), nullptr),
            MT_ERR_INVALID);
  EXPECT_EQ(mt_get_strided(
                tree, worker, first.data(),
                std::numeric_limits<size_t>::max() / sizeof(mt_record_id) + 1,
                first.size(), first.size(), found.data()),
            MT_ERR_INVALID);

  found = {99, 99, 99};
  EXPECT_EQ(mt_get_strided(reinterpret_cast<mt_tree *>(uintptr_t{1}), worker,
                           keys.front().key.data(), found.size(), first.size(),
                           sizeof(padded_key), found.data()),
            MT_ERR_INVALID);
  EXPECT_EQ(found, (std::array<mt_record_id, 3>{0, 0, 0}));

  mt_read_scope scope{};
  ASSERT_EQ(mt_read_scope_begin(tree, worker, &scope), MT_OK);
  found = {99, 99, 99};
  EXPECT_EQ(mt_get_strided(tree, worker, keys.front().key.data(), found.size(),
                           first.size(), sizeof(padded_key), found.data()),
            MT_ERR_ACTIVE_GUARDS);
  EXPECT_EQ(found, (std::array<mt_record_id, 3>{0, 0, 0}));
  ASSERT_EQ(mt_read_scope_end(&scope), MT_OK);
  EXPECT_EQ(mt_thread_quiesce(worker), MT_OK);
}

TEST(MtreeAbiPoint, ScopedStridedReadsValidateOnceAndClearEveryFailure) {
  mt_runtime *runtime = default_runtime();
  mt_thread *worker = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, worker);
  const std::array<uint8_t, 4> first = {'b', 0, '0', '1'};
  const std::array<uint8_t, 4> missing = {'b', 0, '0', '2'};
  const std::array<uint8_t, 4> third = {'b', 0, '0', '3'};
  insert_key(tree, worker, first.data(), first.size(), 101);
  insert_key(tree, worker, third.data(), third.size(), 103);
  insert_key(tree, worker, nullptr, 0, 104);

  struct padded_key {
    std::array<uint8_t, 4> key;
    std::array<uint8_t, 5> padding;
  };
  static_assert(offsetof(padded_key, key) == 0);
  const std::array<padded_key, 3> keys = {
      padded_key{first, {}}, padded_key{missing, {}}, padded_key{third, {}}};

  mt_read_scope scope{};
  ASSERT_EQ(mt_read_scope_begin(tree, worker, &scope), MT_OK);
  const mt_read_scope stale = scope;

  std::array<mt_record_id, 3> found = {99, 99, 99};
  EXPECT_EQ(mt_read_scope_get_strided(&scope, keys.front().key.data(),
                                      keys.size(), first.size(),
                                      sizeof(padded_key), found.data()),
            MT_OK);
  EXPECT_EQ(found, (std::array<mt_record_id, 3>{101, 0, 103}));

  /* Empty keys need no input storage and retain one result per lookup. */
  found = {99, 99, 99};
  EXPECT_EQ(mt_read_scope_get_strided(&scope, nullptr, found.size(), 0, 0,
                                      found.data()),
            MT_OK);
  EXPECT_EQ(found, (std::array<mt_record_id, 3>{104, 104, 104}));

  /* A zero-count call still validates both the capability and common shape. */
  EXPECT_EQ(mt_read_scope_get_strided(&scope, nullptr, 0, first.size(),
                                      first.size(), nullptr),
            MT_OK);
  EXPECT_EQ(mt_read_scope_get_strided(nullptr, nullptr, 0, 0, 0, nullptr),
            MT_ERR_INVALID);

  found = {99, 99, 99};
  EXPECT_EQ(mt_read_scope_get_strided(&scope, nullptr, found.size(),
                                      first.size(), first.size(), found.data()),
            MT_ERR_INVALID);
  EXPECT_EQ(found, (std::array<mt_record_id, 3>{0, 0, 0}));

  found = {99, 99, 99};
  EXPECT_EQ(mt_read_scope_get_strided(&scope, keys.front().key.data(),
                                      found.size(), first.size(),
                                      first.size() - 1, found.data()),
            MT_ERR_INVALID);
  EXPECT_EQ(found, (std::array<mt_record_id, 3>{0, 0, 0}));

  found = {99, 99, 99};
  EXPECT_EQ(mt_read_scope_get_strided(&scope, keys.front().key.data(),
                                      found.size(), mt_max_key_length() + 1,
                                      mt_max_key_length() + 1, found.data()),
            MT_ERR_KEY_TOO_LARGE);
  EXPECT_EQ(found, (std::array<mt_record_id, 3>{0, 0, 0}));

  std::array<mt_record_id, 2> overflow_found = {99, 99};
  EXPECT_EQ(mt_read_scope_get_strided(
                &scope, first.data(), overflow_found.size(), 1,
                std::numeric_limits<size_t>::max(), overflow_found.data()),
            MT_ERR_INVALID);
  EXPECT_EQ(overflow_found, (std::array<mt_record_id, 2>{0, 0}));

  found = {99, 99, 99};
  const void *wrapping_keys = reinterpret_cast<const void *>(
      std::numeric_limits<uintptr_t>::max() - first.size() + 2);
  EXPECT_EQ(mt_read_scope_get_strided(&scope, wrapping_keys, found.size(),
                                      first.size(), first.size(), found.data()),
            MT_ERR_INVALID);
  EXPECT_EQ(found, (std::array<mt_record_id, 3>{0, 0, 0}));

  auto *wrapping_output = reinterpret_cast<mt_record_id *>(
      std::numeric_limits<uintptr_t>::max() - sizeof(mt_record_id) + 2);
  EXPECT_EQ(mt_read_scope_get_strided(&scope, first.data(), 1, first.size(),
                                      first.size(), wrapping_output),
            MT_ERR_INVALID);

  EXPECT_EQ(mt_read_scope_get_strided(&scope, first.data(), 1, first.size(),
                                      first.size(), nullptr),
            MT_ERR_INVALID);
  ASSERT_EQ(mt_read_scope_end(&scope), MT_OK);

  found = {99, 99, 99};
  EXPECT_EQ(mt_read_scope_get_strided(&stale, keys.front().key.data(),
                                      found.size(), first.size(),
                                      sizeof(padded_key), found.data()),
            MT_ERR_INVALID);
  EXPECT_EQ(found, (std::array<mt_record_id, 3>{0, 0, 0}));
  EXPECT_EQ(mt_thread_quiesce(worker), MT_OK);
}

TEST(MtreeAbiPoint, ScopedReadTokenCannotCrossAnOsThread) {
  mt_runtime *runtime = default_runtime();
  mt_thread *worker = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, worker);
  const auto key = ordered_key(7);
  insert_key(tree, worker, key.data(), key.size(), 8);

  mt_read_scope scope{};
  ASSERT_EQ(mt_read_scope_begin(tree, worker, &scope), MT_OK);
  const mt_read_scope copied = scope;
  std::atomic<mt_status> status{MT_OK};
  std::atomic<mt_status> strided_status{MT_OK};
  std::atomic<mt_record_id> output{99};
  std::atomic<mt_record_id> strided_output{99};
  std::thread other([&]() {
    mt_record_id found = 99;
    status.store(mt_read_scope_get(&copied, key.data(), key.size(), &found),
                 std::memory_order_release);
    output.store(found, std::memory_order_release);
    found = 99;
    strided_status.store(mt_read_scope_get_strided(&copied, key.data(), 1,
                                                   key.size(), key.size(),
                                                   &found),
                         std::memory_order_release);
    strided_output.store(found, std::memory_order_release);
  });
  other.join();
  EXPECT_EQ(status.load(std::memory_order_acquire), MT_ERR_INVALID);
  EXPECT_EQ(output.load(std::memory_order_acquire), MT_RECORD_ID_NONE);
  EXPECT_EQ(strided_status.load(std::memory_order_acquire), MT_ERR_INVALID);
  EXPECT_EQ(strided_output.load(std::memory_order_acquire), MT_RECORD_ID_NONE);

  mt_record_id found = 99;
  EXPECT_EQ(mt_read_scope_get(&scope, key.data(), key.size(), &found), MT_OK);
  EXPECT_EQ(found, 8u);
  EXPECT_EQ(mt_read_scope_end(&scope), MT_OK);
}

TEST(MtreeAbiPoint, ScopedReaderDrainsBeforeStructuralWriterEnters) {
  mt_runtime *runtime = default_runtime();
  mt_thread *reader = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, reader);
  const auto stable_key = layered_key(1);
  const auto inserted_key = layered_key(2);
  insert_key(tree, reader, stable_key.data(), stable_key.size(), 1);

  mt_read_scope scope{};
  ASSERT_EQ(mt_read_scope_begin(tree, reader, &scope), MT_OK);
  std::barrier ready(2);
  std::atomic<bool> writer_started{false};
  std::atomic<bool> writer_returned{false};
  std::atomic<mt_status> attach_status{MT_ERR_INTERNAL};
  std::atomic<mt_status> insert_status{MT_ERR_INTERNAL};
  std::atomic<mt_status> quiesce_status{MT_ERR_INTERNAL};
  mt_get_or_insert_result inserted{};
  std::thread writer([&]() {
    mt_thread *worker = nullptr;
    attach_status.store(mt_thread_attach(runtime, &worker),
                        std::memory_order_release);
    ready.arrive_and_wait();
    if (attach_status.load(std::memory_order_acquire) != MT_OK) {
      return;
    }
    writer_started.store(true, std::memory_order_release);
    insert_status.store(mt_get_or_insert(tree, worker, inserted_key.data(),
                                         inserted_key.size(), 2, &inserted),
                        std::memory_order_release);
    writer_returned.store(true, std::memory_order_release);
    quiesce_status.store(mt_thread_quiesce(worker), std::memory_order_release);
  });

  ready.arrive_and_wait();
  ASSERT_EQ(attach_status.load(std::memory_order_acquire), MT_OK);
  while (!writer_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  for (size_t iteration = 0; iteration != 256; ++iteration) {
    mt_record_id found = 99;
    ASSERT_EQ(
        mt_read_scope_get(&scope, stable_key.data(), stable_key.size(), &found),
        MT_OK);
    ASSERT_EQ(found, 1u);
    std::this_thread::yield();
  }
  EXPECT_FALSE(writer_returned.load(std::memory_order_acquire));

  ASSERT_EQ(mt_read_scope_end(&scope), MT_OK);
  writer.join();
  EXPECT_TRUE(writer_returned.load(std::memory_order_acquire));
  EXPECT_EQ(insert_status.load(std::memory_order_acquire), MT_OK);
  EXPECT_EQ(inserted.winner, 2u);
  EXPECT_EQ(inserted.inserted, 1u);
  EXPECT_EQ(quiesce_status.load(std::memory_order_acquire), MT_OK);
}

TEST(MtreeAbiPoint, FirstReadRegistrationsRaceWritersOnFreshTrees) {
  mt_runtime *runtime = default_runtime();
  mt_thread *creator = current_worker(runtime);
  constexpr size_t kTreeCount = 256;
  const auto key = ordered_key(77);
  std::vector<mt_tree *> trees;
  trees.reserve(kTreeCount);
  for (size_t index = 0; index != kTreeCount; ++index) {
    trees.push_back(new_tree(runtime, creator));
  }

  std::array<std::atomic<mt_status>, 2> attach_statuses{
      MT_ERR_INTERNAL, MT_ERR_INTERNAL};
  std::array<std::atomic<mt_status>, 2> quiesce_statuses{
      MT_ERR_INTERNAL, MT_ERR_INTERNAL};
  std::atomic<size_t> violations{0};
  std::barrier phase(2);

  std::thread reader([&]() {
    mt_thread *worker = nullptr;
    attach_statuses[0].store(mt_thread_attach(runtime, &worker),
                             std::memory_order_release);
    phase.arrive_and_wait();
    if (attach_statuses[0].load(std::memory_order_acquire) != MT_OK ||
        attach_statuses[1].load(std::memory_order_acquire) != MT_OK) {
      return;
    }
    for (size_t index = 0; index != kTreeCount; ++index) {
      phase.arrive_and_wait();
      if ((index & 1) == 0) {
        std::this_thread::yield();
      }
      mt_record_id found = 99;
      const mt_status status =
          mt_get(trees[index], worker, key.data(), key.size(), &found);
      const mt_record_id expected = static_cast<mt_record_id>(index + 1);
      if (status != MT_OK ||
          (found != MT_RECORD_ID_NONE && found != expected)) {
        violations.fetch_add(1, std::memory_order_relaxed);
      }
    }
    quiesce_statuses[0].store(mt_thread_quiesce(worker),
                              std::memory_order_release);
  });

  std::thread writer([&]() {
    mt_thread *worker = nullptr;
    attach_statuses[1].store(mt_thread_attach(runtime, &worker),
                             std::memory_order_release);
    phase.arrive_and_wait();
    if (attach_statuses[0].load(std::memory_order_acquire) != MT_OK ||
        attach_statuses[1].load(std::memory_order_acquire) != MT_OK) {
      return;
    }
    for (size_t index = 0; index != kTreeCount; ++index) {
      phase.arrive_and_wait();
      if ((index & 1) != 0) {
        std::this_thread::yield();
      }
      const mt_record_id candidate = static_cast<mt_record_id>(index + 1);
      mt_get_or_insert_result inserted{};
      if (mt_get_or_insert(trees[index], worker, key.data(), key.size(),
                           candidate, &inserted) != MT_OK ||
          inserted.winner != candidate || inserted.inserted != 1 ||
          inserted.publication != MT_PUBLICATION_CANDIDATE_INSERTED) {
        violations.fetch_add(1, std::memory_order_relaxed);
      }
    }
    quiesce_statuses[1].store(mt_thread_quiesce(worker),
                              std::memory_order_release);
  });

  reader.join();
  writer.join();
  ASSERT_EQ(attach_statuses[0].load(std::memory_order_acquire), MT_OK);
  ASSERT_EQ(attach_statuses[1].load(std::memory_order_acquire), MT_OK);
  EXPECT_EQ(quiesce_statuses[0].load(std::memory_order_acquire), MT_OK);
  EXPECT_EQ(quiesce_statuses[1].load(std::memory_order_acquire), MT_OK);
  EXPECT_EQ(violations.load(std::memory_order_relaxed), 0u);

  for (size_t index = 0; index != kTreeCount; ++index) {
    mt_record_id found = MT_RECORD_ID_NONE;
    ASSERT_EQ(mt_get(trees[index], creator, key.data(), key.size(), &found),
              MT_OK);
    EXPECT_EQ(found, static_cast<mt_record_id>(index + 1));
  }
}

TEST(MtreeAbiPoint, ThreadExitClosesAnUnendedReadScope) {
  mt_runtime *runtime = default_runtime();
  mt_thread *creator = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, creator);
  const auto stable_key = ordered_key(31);
  const auto inserted_key = ordered_key(32);
  insert_key(tree, creator, stable_key.data(), stable_key.size(), 31);

  std::atomic<mt_status> attach_status{MT_ERR_INTERNAL};
  std::atomic<mt_status> begin_status{MT_ERR_INTERNAL};
  std::atomic<mt_status> get_status{MT_ERR_INTERNAL};
  std::thread exiting([&]() {
    mt_thread *worker = nullptr;
    attach_status.store(mt_thread_attach(runtime, &worker),
                        std::memory_order_release);
    if (attach_status.load(std::memory_order_acquire) != MT_OK) {
      return;
    }
    mt_read_scope leaked{};
    begin_status.store(mt_read_scope_begin(tree, worker, &leaked),
                       std::memory_order_release);
    mt_record_id found = 99;
    if (begin_status.load(std::memory_order_acquire) == MT_OK) {
      get_status.store(mt_read_scope_get(&leaked, stable_key.data(),
                                         stable_key.size(), &found),
                       std::memory_order_release);
    }
    /* Deliberately rely on TLS scope-state destruction at thread exit. */
  });
  exiting.join();
  ASSERT_EQ(attach_status.load(std::memory_order_acquire), MT_OK);
  ASSERT_EQ(begin_status.load(std::memory_order_acquire), MT_OK);
  ASSERT_EQ(get_status.load(std::memory_order_acquire), MT_OK);

  mt_get_or_insert_result inserted{};
  ASSERT_EQ(mt_get_or_insert(tree, creator, inserted_key.data(),
                             inserted_key.size(), 32, &inserted),
            MT_OK);
  EXPECT_EQ(inserted.winner, 32u);
  EXPECT_EQ(inserted.inserted, 1u);
}

TEST(MtreeAbiPoint, ThreadExitClosesAnUnendedWorkerRcuScope) {
  mt_runtime *runtime = default_runtime();
  mt_thread *creator = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, creator);
  const auto key = ordered_key(33);
  insert_key(tree, creator, key.data(), key.size(), 33);

  std::atomic<mt_status> attach_status{MT_ERR_INTERNAL};
  std::atomic<mt_status> begin_status{MT_ERR_INTERNAL};
  std::atomic<mt_status> get_status{MT_ERR_INTERNAL};
  std::thread exiting([&]() {
    mt_thread *worker = nullptr;
    attach_status.store(mt_thread_attach(runtime, &worker),
                        std::memory_order_release);
    if (attach_status.load(std::memory_order_acquire) != MT_OK) {
      return;
    }
    mt_rcu_scope leaked{};
    begin_status.store(mt_rcu_scope_begin(worker, &leaked),
                       std::memory_order_release);
    if (begin_status.load(std::memory_order_acquire) == MT_OK) {
      mt_record_id found = 99;
      get_status.store(mt_get(tree, worker, key.data(), key.size(), &found),
                       std::memory_order_release);
    }
    /* Deliberately rely on TLS scope-state destruction at thread exit. */
  });
  exiting.join();
  ASSERT_EQ(attach_status.load(std::memory_order_acquire), MT_OK);
  ASSERT_EQ(begin_status.load(std::memory_order_acquire), MT_OK);
  ASSERT_EQ(get_status.load(std::memory_order_acquire), MT_OK);
  EXPECT_EQ(mt_thread_quiesce(creator), MT_OK);
}

TEST(MtreeAbiPoint, RecordIdsPreserveEveryBitWithoutPointerInterpretation) {
  mt_runtime *runtime = default_runtime();
  mt_thread *worker = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, worker);

  std::vector<mt_record_id> patterns;
  patterns.reserve(64 * 3 + 4);
  for (unsigned bit = 0; bit != 64; ++bit) {
    patterns.push_back(UINT64_C(1) << bit);
    patterns.push_back(UINT64_MAX ^ (UINT64_C(1) << bit));
    patterns.push_back(bit == 63 ? UINT64_MAX
                                 : ((UINT64_C(1) << (bit + 1)) - 1));
  }
  patterns.push_back(UINT64_C(0xaaaaaaaaaaaaaaaa));
  patterns.push_back(UINT64_C(0x5555555555555555));
  patterns.push_back(UINT64_C(0x8000000000000001));
  patterns.push_back(UINT64_C(0xdeadbeef01234567));

  for (size_t index = 0; index != patterns.size(); ++index) {
    ASSERT_NE(patterns[index], MT_RECORD_ID_NONE);
    const auto key = ordered_key(index);
    mt_get_or_insert_result inserted{};
    ASSERT_EQ(mt_get_or_insert(tree, worker, key.data(), key.size(),
                               patterns[index], &inserted),
              MT_OK);
    EXPECT_EQ(inserted.winner, patterns[index]);
    EXPECT_EQ(inserted.publication, MT_PUBLICATION_CANDIDATE_INSERTED);
    mt_record_id found = MT_RECORD_ID_NONE;
    ASSERT_EQ(mt_get(tree, worker, key.data(), key.size(), &found), MT_OK);
    EXPECT_EQ(found, patterns[index]);
  }

  std::vector<mt_scan_entry> entries(patterns.size());
  std::vector<uint8_t> arena(patterns.size() * 8);
  const mt_scan_bound absent = absent_bound();
  mt_scan_result scanned{};
  ASSERT_EQ(mt_scan(tree, worker, MT_SCAN_FORWARD, &absent, &absent,
                    entries.data(), entries.size(), arena.data(), arena.size(),
                    &scanned),
            MT_OK);
  ASSERT_EQ(scanned.stop_reason, MT_SCAN_STOP_END);
  ASSERT_EQ(scanned.entries_written, patterns.size());
  for (size_t index = 0; index != patterns.size(); ++index) {
    const auto expected_key = ordered_key(index);
    ASSERT_EQ(entries[index].key_length, 8u);
    EXPECT_EQ(copied_key(entries[index], arena.data()),
              std::string(reinterpret_cast<const char *>(expected_key.data()),
                          expected_key.size()));
    EXPECT_EQ(entries[index].record_id, patterns[index]);
  }
}

TEST(MtreeAbiScan, EmptyBinaryAndAbsentBoundsRemainDistinct) {
  mt_runtime *runtime = default_runtime();
  mt_thread *worker = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, worker);
  const std::array<uint8_t, 1> zero = {0x00};
  const std::array<uint8_t, 2> zero_ff = {0x00, 0xff};
  const std::array<uint8_t, 2> one_zero = {0x01, 0x00};
  insert_key(tree, worker, nullptr, 0, 1);
  insert_key(tree, worker, zero.data(), zero.size(), 2);
  insert_key(tree, worker, zero_ff.data(), zero_ff.size(), 3);
  insert_key(tree, worker, one_zero.data(), one_zero.size(), 4);

  const mt_scan_bound absent = absent_bound();
  const std::vector<std::string> expected = {
      std::string(), std::string("\0", 1), std::string("\0\xff", 2),
      std::string("\x01\0", 2)};
  EXPECT_EQ(scan_keys(tree, worker, MT_SCAN_FORWARD, absent, absent), expected);
  EXPECT_EQ(scan_keys(tree, worker, MT_SCAN_REVERSE, absent, absent),
            (std::vector<std::string>{expected[3], expected[2], expected[1],
                                      expected[0]}));

  const mt_scan_bound empty_inclusive = inclusive_bound(nullptr, 0);
  EXPECT_EQ(scan_keys(tree, worker, MT_SCAN_FORWARD, absent, empty_inclusive),
            (std::vector<std::string>{std::string()}));
  const mt_scan_bound empty_exclusive = exclusive_bound(nullptr, 0);
  EXPECT_TRUE(scan_keys(tree, worker, MT_SCAN_FORWARD, absent, empty_exclusive)
                  .empty());
  EXPECT_EQ(scan_keys(tree, worker, MT_SCAN_FORWARD, empty_exclusive, absent),
            (std::vector<std::string>{expected[1], expected[2], expected[3]}));
  EXPECT_EQ(scan_keys(tree, worker, MT_SCAN_REVERSE, absent, empty_inclusive),
            (std::vector<std::string>{std::string()}));
}

TEST(MtreeAbiScan, EveryInclusivityCombinationWorksInBothDirections) {
  mt_runtime *runtime = default_runtime();
  mt_thread *worker = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, worker);
  for (const char key : {'a', 'b', 'c', 'd'}) {
    insert_key(tree, worker, &key, 1, static_cast<mt_record_id>(key - 'a' + 1));
  }

  mt_scan_bound lower = inclusive_bound("b", 1);
  mt_scan_bound upper = inclusive_bound("c", 1);
  EXPECT_EQ(scan_keys(tree, worker, MT_SCAN_FORWARD, lower, upper),
            (std::vector<std::string>{"b", "c"}));
  EXPECT_EQ(scan_keys(tree, worker, MT_SCAN_REVERSE, lower, upper),
            (std::vector<std::string>{"c", "b"}));

  upper.kind = MT_SCAN_BOUND_EXCLUSIVE;
  EXPECT_EQ(scan_keys(tree, worker, MT_SCAN_FORWARD, lower, upper),
            (std::vector<std::string>{"b"}));
  EXPECT_EQ(scan_keys(tree, worker, MT_SCAN_REVERSE, lower, upper),
            (std::vector<std::string>{"b"}));

  lower.kind = MT_SCAN_BOUND_EXCLUSIVE;
  upper.kind = MT_SCAN_BOUND_INCLUSIVE;
  EXPECT_EQ(scan_keys(tree, worker, MT_SCAN_FORWARD, lower, upper),
            (std::vector<std::string>{"c"}));
  EXPECT_EQ(scan_keys(tree, worker, MT_SCAN_REVERSE, lower, upper),
            (std::vector<std::string>{"c"}));

  upper.kind = MT_SCAN_BOUND_EXCLUSIVE;
  EXPECT_TRUE(scan_keys(tree, worker, MT_SCAN_FORWARD, lower, upper).empty());
  EXPECT_TRUE(scan_keys(tree, worker, MT_SCAN_REVERSE, lower, upper).empty());

  lower = inclusive_bound("b", 1);
  upper = inclusive_bound("b", 1);
  EXPECT_EQ(scan_keys(tree, worker, MT_SCAN_FORWARD, lower, upper),
            (std::vector<std::string>{"b"}));
  EXPECT_EQ(scan_keys(tree, worker, MT_SCAN_REVERSE, lower, upper),
            (std::vector<std::string>{"b"}));
  upper.kind = MT_SCAN_BOUND_EXCLUSIVE;
  EXPECT_TRUE(scan_keys(tree, worker, MT_SCAN_FORWARD, lower, upper).empty());
  EXPECT_TRUE(scan_keys(tree, worker, MT_SCAN_REVERSE, lower, upper).empty());

  lower = inclusive_bound("d", 1);
  upper = exclusive_bound("a", 1);
  EXPECT_TRUE(scan_keys(tree, worker, MT_SCAN_FORWARD, lower, upper).empty());
  EXPECT_TRUE(scan_keys(tree, worker, MT_SCAN_REVERSE, lower, upper).empty());
}

TEST(MtreeAbiScan, CapacityStopsReportExactNextKeyAndAuthoritativeResume) {
  mt_runtime *runtime = default_runtime();
  mt_thread *worker = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, worker);
  const std::string first(37, 'a');
  const std::string second(40, 'b');
  insert_key(tree, worker, first.data(), first.size(), 11);
  insert_key(tree, worker, second.data(), second.size(), 12);
  const mt_scan_bound absent = absent_bound();

  mt_scan_result no_entries{};
  ASSERT_EQ(mt_scan(tree, worker, MT_SCAN_FORWARD, &absent, &absent, nullptr, 0,
                    nullptr, 0, &no_entries),
            MT_OK);
  EXPECT_EQ(no_entries.stop_reason, MT_SCAN_STOP_ENTRY_CAPACITY);
  EXPECT_EQ(no_entries.resume, MT_SCAN_RESUME_UNCHANGED_INPUT);
  EXPECT_EQ(no_entries.next_key_bytes_required, first.size());
  EXPECT_EQ(no_entries.entries_written, 0u);
  EXPECT_EQ(no_entries.arena_bytes_used, 0u);

  mt_scan_entry entry{};
  std::array<uint8_t, 8> tiny_arena{};
  mt_scan_result no_arena{};
  ASSERT_EQ(mt_scan(tree, worker, MT_SCAN_FORWARD, &absent, &absent, &entry, 1,
                    tiny_arena.data(), tiny_arena.size(), &no_arena),
            MT_OK);
  EXPECT_EQ(no_arena.stop_reason, MT_SCAN_STOP_KEY_ARENA_CAPACITY);
  EXPECT_EQ(no_arena.resume, MT_SCAN_RESUME_UNCHANGED_INPUT);
  EXPECT_EQ(no_arena.next_key_bytes_required, first.size());
  EXPECT_EQ(no_arena.entries_written, 0u);

  std::array<uint8_t, 128> arena{};
  mt_scan_result entry_limited{};
  ASSERT_EQ(mt_scan(tree, worker, MT_SCAN_FORWARD, &absent, &absent, &entry, 1,
                    arena.data(), arena.size(), &entry_limited),
            MT_OK);
  EXPECT_EQ(entry_limited.stop_reason, MT_SCAN_STOP_ENTRY_CAPACITY);
  EXPECT_EQ(entry_limited.resume, MT_SCAN_RESUME_EXCLUSIVE_LAST);
  EXPECT_EQ(entry_limited.next_key_bytes_required, second.size());
  EXPECT_EQ(entry_limited.entries_written, 1u);
  EXPECT_EQ(entry_limited.resume_key_offset, entry.key_offset);
  EXPECT_EQ(entry_limited.resume_key_length, entry.key_length);
  EXPECT_EQ(copied_key(entry, arena.data()), first);

  std::array<mt_scan_entry, 2> entries{};
  std::array<uint8_t, 37> exact_arena{};
  mt_scan_result arena_limited{};
  ASSERT_EQ(mt_scan(tree, worker, MT_SCAN_FORWARD, &absent, &absent,
                    entries.data(), entries.size(), exact_arena.data(),
                    exact_arena.size(), &arena_limited),
            MT_OK);
  EXPECT_EQ(arena_limited.stop_reason, MT_SCAN_STOP_KEY_ARENA_CAPACITY);
  EXPECT_EQ(arena_limited.resume, MT_SCAN_RESUME_EXCLUSIVE_LAST);
  EXPECT_EQ(arena_limited.next_key_bytes_required, second.size());
  EXPECT_EQ(arena_limited.entries_written, 1u);
  EXPECT_EQ(arena_limited.arena_bytes_used, first.size());
  EXPECT_EQ(arena_limited.resume_key_offset, entries[0].key_offset);
  EXPECT_EQ(arena_limited.resume_key_length, entries[0].key_length);

  const mt_scan_bound through_first =
      inclusive_bound(first.data(), first.size());
  mt_scan_result exact_end{};
  ASSERT_EQ(mt_scan(tree, worker, MT_SCAN_FORWARD, &absent, &through_first,
                    &entry, 1, exact_arena.data(), exact_arena.size(),
                    &exact_end),
            MT_OK);
  EXPECT_EQ(exact_end.entries_written, 1u);
  EXPECT_EQ(exact_end.arena_bytes_used, first.size());
  EXPECT_EQ(exact_end.stop_reason, MT_SCAN_STOP_END);
  EXPECT_EQ(exact_end.resume, MT_SCAN_RESUME_NONE);
}

TEST(MtreeAbiScan, ExclusiveResumePartitionsForwardAndReverseScans) {
  mt_runtime *runtime = default_runtime();
  mt_thread *worker = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, worker);
  const std::vector<std::string> expected = {"", "a", "aa", "b", "c", "d"};
  for (size_t index = 0; index != expected.size(); ++index) {
    insert_key(tree, worker, expected[index].data(), expected[index].size(),
               index + 1);
  }

  auto collect_chunks = [&](mt_scan_direction direction) {
    mt_scan_bound lower = absent_bound();
    mt_scan_bound upper = absent_bound();
    std::string cursor;
    std::vector<std::string> seen;
    for (size_t call = 0; call != 10; ++call) {
      std::array<mt_scan_entry, 2> entries{};
      std::array<uint8_t, 16> arena{};
      mt_scan_result result{};
      EXPECT_EQ(mt_scan(tree, worker, direction, &lower, &upper, entries.data(),
                        entries.size(), arena.data(), arena.size(), &result),
                MT_OK);
      for (size_t index = 0; index != result.entries_written; ++index) {
        seen.push_back(copied_key(entries[index], arena.data()));
      }
      if (result.stop_reason == MT_SCAN_STOP_END) {
        EXPECT_EQ(result.resume, MT_SCAN_RESUME_NONE);
        return seen;
      }
      EXPECT_GT(result.entries_written, 0u);
      EXPECT_EQ(result.resume, MT_SCAN_RESUME_EXCLUSIVE_LAST);
      const mt_scan_entry &last = entries[result.entries_written - 1];
      EXPECT_EQ(result.resume_key_offset, last.key_offset);
      EXPECT_EQ(result.resume_key_length, last.key_length);
      cursor = copied_key(last, arena.data());
      if (direction == MT_SCAN_FORWARD) {
        lower = exclusive_bound(cursor.data(), cursor.size());
      } else {
        upper = exclusive_bound(cursor.data(), cursor.size());
      }
    }
    ADD_FAILURE() << "scan did not terminate after exclusive resumption";
    return seen;
  };

  EXPECT_EQ(collect_chunks(MT_SCAN_FORWARD), expected);
  EXPECT_EQ(collect_chunks(MT_SCAN_REVERSE),
            (std::vector<std::string>(expected.rbegin(), expected.rend())));
}

TEST(MtreeAbiScan, UnboundedReverseIncludesTheMaximumLengthAllFfKey) {
  mt_runtime *runtime = default_runtime();
  mt_thread *worker = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, worker);
  std::vector<uint8_t> shorter(MT_CONFIGURED_MAX_KEY_LENGTH - 1, UINT8_MAX);
  std::vector<uint8_t> maximum(MT_CONFIGURED_MAX_KEY_LENGTH, UINT8_MAX);
  insert_key(tree, worker, shorter.data(), shorter.size(), 21);
  insert_key(tree, worker, maximum.data(), maximum.size(), 22);
  const mt_scan_bound absent = absent_bound();

  mt_scan_entry entry{};
  std::vector<uint8_t> too_small(maximum.size() - 1);
  mt_scan_result retry{};
  ASSERT_EQ(mt_scan(tree, worker, MT_SCAN_REVERSE, &absent, &absent, &entry, 1,
                    too_small.data(), too_small.size(), &retry),
            MT_OK);
  EXPECT_EQ(retry.stop_reason, MT_SCAN_STOP_KEY_ARENA_CAPACITY);
  EXPECT_EQ(retry.resume, MT_SCAN_RESUME_UNCHANGED_INPUT);
  EXPECT_EQ(retry.next_key_bytes_required, maximum.size());

  std::vector<uint8_t> arena(maximum.size());
  mt_scan_result copied{};
  ASSERT_EQ(mt_scan(tree, worker, MT_SCAN_REVERSE, &absent, &absent, &entry, 1,
                    arena.data(), arena.size(), &copied),
            MT_OK);
  EXPECT_EQ(copied.stop_reason, MT_SCAN_STOP_ENTRY_CAPACITY);
  EXPECT_EQ(copied.resume, MT_SCAN_RESUME_EXCLUSIVE_LAST);
  ASSERT_EQ(entry.key_length, maximum.size());
  EXPECT_EQ(entry.record_id, 22u);
  EXPECT_TRUE(std::equal(maximum.begin(), maximum.end(), arena.begin()));
}

TEST(MtreeAbiScan, FixedWorkersPreserveStableKeysAcrossConcurrentSplits) {
  mt_runtime *runtime = default_runtime();
  mt_thread *creator = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, creator);
  constexpr size_t kStableKeys = 256;
  constexpr size_t kScanRounds = 256;
  for (size_t index = 0; index != kStableKeys; ++index) {
    const uint64_t coordinate = index * 2;
    const auto key = ordered_key(coordinate);
    insert_key(tree, creator, key.data(), key.size(), coordinate + 1);
  }

  std::array<std::atomic<mt_status>, 2> attach_statuses{};
  std::array<mt_status, 2> quiesce_statuses{};
  std::atomic<size_t> violations{0};
  std::barrier start(2);
  std::thread writer([&]() {
    mt_thread *worker = nullptr;
    attach_statuses[0].store(mt_thread_attach(runtime, &worker),
                             std::memory_order_release);
    start.arrive_and_wait();
    if (attach_statuses[0].load(std::memory_order_acquire) != MT_OK ||
        attach_statuses[1].load(std::memory_order_acquire) != MT_OK) {
      return;
    }
    for (size_t index = 0; index != kStableKeys; ++index) {
      const uint64_t coordinate = index * 2 + 1;
      const auto key = ordered_key(coordinate);
      mt_get_or_insert_result inserted{};
      if (mt_get_or_insert(tree, worker, key.data(), key.size(), coordinate + 1,
                           &inserted) != MT_OK ||
          inserted.winner != coordinate + 1 || inserted.inserted != 1) {
        violations.fetch_add(1, std::memory_order_relaxed);
      }
      if ((index & 7) == 0) {
        std::this_thread::yield();
      }
    }
    quiesce_statuses[0] = mt_thread_quiesce(worker);
  });
  std::thread scanner([&]() {
    mt_thread *worker = nullptr;
    attach_statuses[1].store(mt_thread_attach(runtime, &worker),
                             std::memory_order_release);
    start.arrive_and_wait();
    if (attach_statuses[0].load(std::memory_order_acquire) != MT_OK ||
        attach_statuses[1].load(std::memory_order_acquire) != MT_OK) {
      return;
    }
    const mt_scan_bound absent = absent_bound();
    std::array<mt_scan_entry, kStableKeys * 2> entries{};
    std::array<uint8_t, kStableKeys * 2 * 8> arena{};
    for (size_t round = 0; round != kScanRounds; ++round) {
      const mt_scan_direction direction =
          (round & 1) == 0 ? MT_SCAN_FORWARD : MT_SCAN_REVERSE;
      mt_scan_result result{};
      if (mt_scan(tree, worker, direction, &absent, &absent, entries.data(),
                  entries.size(), arena.data(), arena.size(),
                  &result) != MT_OK ||
          result.stop_reason != MT_SCAN_STOP_END ||
          result.entries_written > entries.size()) {
        violations.fetch_add(1, std::memory_order_relaxed);
        continue;
      }

      std::array<bool, kStableKeys> stable_seen{};
      uint64_t previous = 0;
      bool have_previous = false;
      for (size_t index = 0; index != result.entries_written; ++index) {
        const mt_scan_entry &entry = entries[index];
        if (entry.key_length != 8 ||
            entry.key_offset > result.arena_bytes_used ||
            entry.key_length > result.arena_bytes_used - entry.key_offset) {
          violations.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        const uint64_t coordinate =
            decode_ordered_key(arena.data() + entry.key_offset);
        const bool out_of_order =
            have_previous &&
            (direction == MT_SCAN_FORWARD ? coordinate <= previous
                                          : coordinate >= previous);
        if (out_of_order || entry.record_id != coordinate + 1) {
          violations.fetch_add(1, std::memory_order_relaxed);
        }
        previous = coordinate;
        have_previous = true;
        if ((coordinate & 1) == 0 && coordinate / 2 < kStableKeys) {
          const size_t stable_index = coordinate / 2;
          if (stable_seen[stable_index]) {
            violations.fetch_add(1, std::memory_order_relaxed);
          }
          stable_seen[stable_index] = true;
        }
      }
      for (const bool seen : stable_seen) {
        if (!seen) {
          violations.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
    quiesce_statuses[1] = mt_thread_quiesce(worker);
  });
  writer.join();
  scanner.join();

  ASSERT_EQ(attach_statuses[0].load(std::memory_order_acquire), MT_OK);
  ASSERT_EQ(attach_statuses[1].load(std::memory_order_acquire), MT_OK);
  EXPECT_EQ(quiesce_statuses[0], MT_OK);
  EXPECT_EQ(quiesce_statuses[1], MT_OK);
  EXPECT_EQ(violations.load(std::memory_order_relaxed), 0u);
  for (uint64_t coordinate = 0; coordinate != kStableKeys * 2; ++coordinate) {
    const auto key = ordered_key(coordinate);
    mt_record_id found = MT_RECORD_ID_NONE;
    ASSERT_EQ(mt_get(tree, creator, key.data(), key.size(), &found), MT_OK);
    EXPECT_EQ(found, coordinate + 1);
  }
}

TEST(MtreeAbiScan, MalformedInputsAreRejectedAfterResultInitialization) {
  mt_runtime *runtime = default_runtime();
  mt_thread *worker = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, worker);
  insert_key(tree, worker, "key", 3, 31);
  const mt_scan_bound absent = absent_bound();
  mt_scan_entry entry{};
  std::array<uint8_t, 32> arena{};

  auto expect_invalid_and_initialized = [&](mt_scan_direction direction,
                                            const mt_scan_bound *lower,
                                            const mt_scan_bound *upper) {
    mt_scan_result result;
    std::memset(&result, 0xa5, sizeof(result));
    EXPECT_EQ(mt_scan(tree, worker, direction, lower, upper, &entry, 1,
                      arena.data(), arena.size(), &result),
              MT_ERR_INVALID);
    expect_initialized_scan_result(result);
  };

  expect_invalid_and_initialized(99, &absent, &absent);
  expect_invalid_and_initialized(MT_SCAN_FORWARD, nullptr, &absent);
  expect_invalid_and_initialized(MT_SCAN_FORWARD, &absent, nullptr);

  mt_scan_bound malformed = absent;
  malformed.key = "hidden";
  expect_invalid_and_initialized(MT_SCAN_FORWARD, &malformed, &absent);
  malformed = absent;
  malformed.key_length = 1;
  expect_invalid_and_initialized(MT_SCAN_FORWARD, &malformed, &absent);
  malformed = inclusive_bound(nullptr, 1);
  expect_invalid_and_initialized(MT_SCAN_FORWARD, &malformed, &absent);
  malformed = absent;
  malformed.kind = 99;
  expect_invalid_and_initialized(MT_SCAN_FORWARD, &malformed, &absent);
  malformed = absent;
  malformed.reserved = 1;
  expect_invalid_and_initialized(MT_SCAN_FORWARD, &malformed, &absent);

  /* The private trusted scan must not bypass checks on the public path. */
  malformed = inclusive_bound(nullptr, 1);
  expect_invalid_and_initialized(MT_SCAN_FORWARD, &absent, &malformed);
  malformed = absent;
  malformed.kind = 99;
  expect_invalid_and_initialized(MT_SCAN_REVERSE, &absent, &malformed);
  malformed = absent;
  malformed.reserved = 1;
  expect_invalid_and_initialized(MT_SCAN_REVERSE, &absent, &malformed);

  std::vector<uint8_t> oversized(mt_max_key_length() + 1, 0);
  malformed = inclusive_bound(oversized.data(), oversized.size());
  mt_scan_result oversized_result;
  std::memset(&oversized_result, 0xa5, sizeof(oversized_result));
  EXPECT_EQ(mt_scan(tree, worker, MT_SCAN_FORWARD, &malformed, &absent, &entry,
                    1, arena.data(), arena.size(), &oversized_result),
            MT_ERR_KEY_TOO_LARGE);
  expect_initialized_scan_result(oversized_result);

  mt_scan_result untouched;
  std::memset(&untouched, 0xa5, sizeof(untouched));
  const size_t untouched_entries = untouched.entries_written;
  EXPECT_EQ(mt_scan(tree, worker, MT_SCAN_FORWARD, &absent, &absent, nullptr, 1,
                    arena.data(), arena.size(), &untouched),
            MT_ERR_INVALID);
  EXPECT_EQ(untouched.entries_written, untouched_entries);
  EXPECT_EQ(mt_scan(tree, worker, MT_SCAN_FORWARD, &absent, &absent, &entry, 1,
                    nullptr, 1, &untouched),
            MT_ERR_INVALID);
  EXPECT_EQ(mt_scan(tree, worker, MT_SCAN_FORWARD, &absent, &absent, &entry, 1,
                    arena.data(), arena.size(), nullptr),
            MT_ERR_INVALID);

  auto *foreign_tree = reinterpret_cast<mt_tree *>(uintptr_t{1});
  auto *foreign_thread = reinterpret_cast<mt_thread *>(uintptr_t{1});
  mt_scan_result result;
  std::memset(&result, 0xa5, sizeof(result));
  EXPECT_EQ(mt_scan(foreign_tree, worker, MT_SCAN_FORWARD, &absent, &absent,
                    &entry, 1, arena.data(), arena.size(), &result),
            MT_ERR_INVALID);
  expect_initialized_scan_result(result);
  std::memset(&result, 0xa5, sizeof(result));
  EXPECT_EQ(mt_scan(tree, foreign_thread, MT_SCAN_FORWARD, &absent, &absent,
                    &entry, 1, arena.data(), arena.size(), &result),
            MT_ERR_INVALID);
  expect_initialized_scan_result(result);
}

TEST(MtreeAbiThreading, WorkerCannotBeUsedFromAnotherOsThread) {
  mt_runtime *runtime = default_runtime();
  mt_thread *worker = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, worker);
  const uint8_t key = 1;
  const std::array<uint8_t, 2> batch_keys = {1, 2};

  std::atomic<mt_status> get_status{MT_OK};
  std::atomic<mt_status> strided_status{MT_OK};
  std::atomic<mt_status> insert_status{MT_OK};
  std::atomic<mt_status> scan_status{MT_OK};
  std::atomic<mt_status> quiesce_status{MT_OK};
  mt_get_or_insert_result insert_result;
  std::memset(&insert_result, 0xa5, sizeof(insert_result));
  const mt_scan_bound absent = absent_bound();
  mt_scan_entry scan_entry{};
  std::array<uint8_t, 8> scan_arena{};
  mt_scan_result scan_result;
  std::memset(&scan_result, 0xa5, sizeof(scan_result));
  std::array<mt_record_id, 2> strided_output = {99, 99};
  std::thread other([&]() {
    mt_record_id value = 99;
    get_status.store(mt_get(tree, worker, &key, 1, &value),
                     std::memory_order_release);
    EXPECT_EQ(value, MT_RECORD_ID_NONE);
    strided_status.store(mt_get_strided(tree, worker, batch_keys.data(),
                                        batch_keys.size(), 1, 1,
                                        strided_output.data()),
                         std::memory_order_release);
    insert_status.store(
        mt_get_or_insert(tree, worker, &key, 1, 1, &insert_result),
        std::memory_order_release);
    scan_status.store(mt_scan(tree, worker, MT_SCAN_FORWARD, &absent, &absent,
                              &scan_entry, 1, scan_arena.data(),
                              scan_arena.size(), &scan_result),
                      std::memory_order_release);
    quiesce_status.store(mt_thread_quiesce(worker), std::memory_order_release);
  });
  other.join();

  EXPECT_EQ(get_status.load(std::memory_order_acquire), MT_ERR_WRONG_THREAD);
  EXPECT_EQ(strided_status.load(std::memory_order_acquire),
            MT_ERR_WRONG_THREAD);
  EXPECT_EQ(strided_output, (std::array<mt_record_id, 2>{0, 0}));
  EXPECT_EQ(insert_status.load(std::memory_order_acquire), MT_ERR_WRONG_THREAD);
  EXPECT_EQ(insert_result.winner, MT_RECORD_ID_NONE);
  EXPECT_EQ(insert_result.inserted, 0u);
  EXPECT_EQ(insert_result.publication,
            MT_PUBLICATION_FAILURE_BEFORE_PUBLICATION);
  EXPECT_EQ(scan_status.load(std::memory_order_acquire), MT_ERR_WRONG_THREAD);
  expect_initialized_scan_result(scan_result);
  EXPECT_EQ(quiesce_status.load(std::memory_order_acquire),
            MT_ERR_WRONG_THREAD);
}

TEST(MtreeAbiThreading, FreshWorkerBindsTheRuntimeMasstreeContext) {
  mt_runtime *runtime = default_runtime();
  SiloRuntime *native_runtime = SiloRuntime::GlobalDefault();
  ASSERT_NE(native_runtime, nullptr);

  mt_status attach_status = MT_ERR_INTERNAL;
  mt_status quiesce_status = MT_ERR_INTERNAL;
  SiloRuntime *runtime_before = nullptr;
  SiloRuntime *runtime_after = nullptr;
  MasstreeContext *fallback_context = nullptr;
  MasstreeContext *bound_context = nullptr;
  std::thread fresh([&]() {
    runtime_before = SiloRuntime::Current();
    fallback_context = MasstreeContext::Current();
    mt_thread *worker = nullptr;
    attach_status = mt_thread_attach(runtime, &worker);
    runtime_after = SiloRuntime::Current();
    bound_context = MasstreeContext::Current();
    if (attach_status == MT_OK) {
      quiesce_status = mt_thread_quiesce(worker);
    }
  });
  fresh.join();

  EXPECT_EQ(runtime_before, native_runtime);
  EXPECT_NE(fallback_context, native_runtime->masstree_context());
  ASSERT_EQ(attach_status, MT_OK);
  EXPECT_EQ(runtime_after, native_runtime);
  EXPECT_EQ(bound_context, native_runtime->masstree_context());
  EXPECT_EQ(quiesce_status, MT_OK);
}

TEST(MtreeAbiThreading, RebindingAnAttachedWorkerRejectsTheWrongRuntime) {
  mt_runtime *runtime = default_runtime();
  mt_thread *creator = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, creator);
  rusty::Arc<SiloRuntime> foreign_runtime = SiloRuntime::Create();
  ASSERT_NE(foreign_runtime.as_ptr(), SiloRuntime::GlobalDefault());

  std::array<mt_status, 8> statuses{};
  mt_record_id found = 99;
  std::array<mt_record_id, 2> strided_found = {99, 99};
  mt_get_or_insert_result insert_result;
  std::memset(&insert_result, 0xa5, sizeof(insert_result));
  mt_scan_result scan_result;
  std::memset(&scan_result, 0xa5, sizeof(scan_result));
  mt_tree *rejected_tree = reinterpret_cast<mt_tree *>(uintptr_t{1});
  mt_thread *rejected = reinterpret_cast<mt_thread *>(uintptr_t{1});
  std::thread other([&]() {
    mt_thread *worker = nullptr;
    statuses[0] = mt_thread_attach(runtime, &worker);
    if (statuses[0] != MT_OK) {
      return;
    }
    foreign_runtime.as_ptr()->BindToCurrentThread();
    statuses[1] = mt_get(tree, worker, nullptr, 0, &found);
    statuses[7] = mt_get_strided(tree, worker, nullptr, strided_found.size(), 0,
                                 0, strided_found.data());
    statuses[2] = mt_get_or_insert(tree, worker, nullptr, 0, 1, &insert_result);

    const mt_scan_bound absent = absent_bound();
    mt_scan_entry entry{};
    std::array<uint8_t, 8> arena{};
    statuses[3] = mt_scan(tree, worker, MT_SCAN_FORWARD, &absent, &absent,
                          &entry, 1, arena.data(), arena.size(), &scan_result);
    statuses[4] = mt_tree_create(runtime, worker, &rejected_tree);
    statuses[5] = mt_thread_quiesce(worker);
    statuses[6] = mt_thread_attach(runtime, &rejected);
  });
  other.join();

  ASSERT_EQ(statuses[0], MT_OK);
  EXPECT_EQ(statuses[1], MT_ERR_WRONG_RUNTIME);
  EXPECT_EQ(found, MT_RECORD_ID_NONE);
  EXPECT_EQ(statuses[7], MT_ERR_WRONG_RUNTIME);
  EXPECT_EQ(strided_found, (std::array<mt_record_id, 2>{0, 0}));
  EXPECT_EQ(statuses[2], MT_ERR_WRONG_RUNTIME);
  EXPECT_EQ(insert_result.winner, MT_RECORD_ID_NONE);
  EXPECT_EQ(insert_result.inserted, 0u);
  EXPECT_EQ(insert_result.publication,
            MT_PUBLICATION_FAILURE_BEFORE_PUBLICATION);
  EXPECT_EQ(statuses[3], MT_ERR_WRONG_RUNTIME);
  expect_initialized_scan_result(scan_result);
  EXPECT_EQ(statuses[4], MT_ERR_WRONG_RUNTIME);
  EXPECT_EQ(rejected_tree, nullptr);
  EXPECT_EQ(statuses[5], MT_ERR_WRONG_RUNTIME);
  EXPECT_EQ(statuses[6], MT_ERR_WRONG_RUNTIME);
  EXPECT_EQ(rejected, nullptr);
}

TEST(MtreeAbiThreading, ConcurrentGetOrInsertReturnsOneWinner) {
  mt_runtime *runtime = default_runtime();
  mt_thread *creator = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, creator);
  constexpr size_t kRounds = 128;
  std::array<std::atomic<mt_status>, 2> attach_statuses{};
  std::array<mt_status, 2> quiesce_statuses{};
  std::array<std::vector<mt_status>, 2> operation_statuses = {
      std::vector<mt_status>(kRounds), std::vector<mt_status>(kRounds)};
  std::array<std::vector<mt_get_or_insert_result>, 2> results = {
      std::vector<mt_get_or_insert_result>(kRounds),
      std::vector<mt_get_or_insert_result>(kRounds)};
  std::barrier round_boundary(2);
  std::array<std::thread, 2> threads;
  for (size_t index = 0; index != threads.size(); ++index) {
    threads[index] = std::thread([&, index]() {
      mt_thread *worker = nullptr;
      attach_statuses[index].store(mt_thread_attach(runtime, &worker),
                                   std::memory_order_release);
      round_boundary.arrive_and_wait();
      if (attach_statuses[0].load(std::memory_order_acquire) != MT_OK ||
          attach_statuses[1].load(std::memory_order_acquire) != MT_OK) {
        return;
      }
      for (size_t round = 0; round != kRounds; ++round) {
        const auto key = ordered_key(UINT64_C(0x10000) + round);
        round_boundary.arrive_and_wait();
        operation_statuses[index][round] = mt_get_or_insert(
            tree, worker, key.data(), key.size(),
            UINT64_C(0x8000000000000001) + index, &results[index][round]);
        round_boundary.arrive_and_wait();
      }
      quiesce_statuses[index] = mt_thread_quiesce(worker);
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }

  ASSERT_EQ(attach_statuses[0].load(std::memory_order_acquire), MT_OK);
  ASSERT_EQ(attach_statuses[1].load(std::memory_order_acquire), MT_OK);
  EXPECT_EQ(quiesce_statuses[0], MT_OK);
  EXPECT_EQ(quiesce_statuses[1], MT_OK);
  for (size_t round = 0; round != kRounds; ++round) {
    ASSERT_EQ(operation_statuses[0][round], MT_OK);
    ASSERT_EQ(operation_statuses[1][round], MT_OK);
    EXPECT_EQ(results[0][round].winner, results[1][round].winner);
    EXPECT_TRUE(results[0][round].winner == UINT64_C(0x8000000000000001) ||
                results[0][round].winner == UINT64_C(0x8000000000000002));
    EXPECT_EQ(static_cast<unsigned>(results[0][round].inserted) +
                  static_cast<unsigned>(results[1][round].inserted),
              1u);
    for (size_t worker = 0; worker != 2; ++worker) {
      const auto &result = results[worker][round];
      const mt_record_id candidate = UINT64_C(0x8000000000000001) + worker;
      if (result.inserted != 0) {
        EXPECT_EQ(result.winner, candidate);
        EXPECT_EQ(result.publication, MT_PUBLICATION_CANDIDATE_INSERTED);
      } else {
        EXPECT_NE(result.winner, candidate);
        EXPECT_EQ(result.publication,
                  MT_PUBLICATION_CANDIDATE_PROVEN_UNPUBLISHED);
      }
    }
    const auto key = ordered_key(UINT64_C(0x10000) + round);
    mt_record_id persisted = MT_RECORD_ID_NONE;
    ASSERT_EQ(mt_get(tree, creator, key.data(), key.size(), &persisted), MT_OK);
    EXPECT_EQ(persisted, results[0][round].winner);
  }
}

TEST(MtreeAbiThreading,
     ValidOperationsSurviveConcurrentHandleRegistryPublication) {
  mt_runtime *runtime = default_runtime();
  mt_thread *creator = current_worker(runtime);
  mt_tree *stable_tree = new_tree(runtime, creator);
  const auto stable_key = ordered_key(UINT64_C(0x123456));
  constexpr mt_record_id kStableValue = UINT64_C(0xfeedface);
  insert_key(stable_tree, creator, stable_key.data(), stable_key.size(),
             kStableValue);

  constexpr size_t kPublishers = 4;
  constexpr size_t kTreesPerPublisher = 16;
  constexpr size_t kMinimumReaderPasses = 4096;
  std::array<std::atomic<mt_status>, kPublishers + 1> attach_statuses;
  for (auto &status : attach_statuses) {
    status.store(MT_ERR_INTERNAL, std::memory_order_relaxed);
  }
  std::array<mt_status, kPublishers + 1> quiesce_statuses{};
  std::atomic<size_t> publishers_done{0};
  std::atomic<size_t> violations{0};
  std::barrier start(kPublishers + 1);

  std::thread reader([&]() {
    mt_thread *worker = nullptr;
    attach_statuses[0].store(mt_thread_attach(runtime, &worker),
                             std::memory_order_release);
    start.arrive_and_wait();
    if (attach_statuses[0].load(std::memory_order_acquire) != MT_OK) {
      return;
    }

    size_t passes = 0;
    do {
      mt_record_id found = MT_RECORD_ID_NONE;
      if (mt_get(stable_tree, worker, stable_key.data(), stable_key.size(),
                 &found) != MT_OK ||
          found != kStableValue) {
        violations.fetch_add(1, std::memory_order_relaxed);
      }

      if ((passes & 63) == 0) {
        auto *foreign_thread = reinterpret_cast<mt_thread *>(uintptr_t{1});
        auto *foreign_tree = reinterpret_cast<mt_tree *>(uintptr_t{1});
        found = 99;
        if (mt_get(stable_tree, foreign_thread, stable_key.data(),
                   stable_key.size(), &found) != MT_ERR_INVALID ||
            found != MT_RECORD_ID_NONE) {
          violations.fetch_add(1, std::memory_order_relaxed);
        }
        found = 99;
        if (mt_get(foreign_tree, worker, stable_key.data(), stable_key.size(),
                   &found) != MT_ERR_INVALID ||
            found != MT_RECORD_ID_NONE) {
          violations.fetch_add(1, std::memory_order_relaxed);
        }
      }
      ++passes;
    } while (passes < kMinimumReaderPasses ||
             publishers_done.load(std::memory_order_acquire) != kPublishers);
    quiesce_statuses[0] = mt_thread_quiesce(worker);
  });

  std::array<std::thread, kPublishers> publishers;
  for (size_t publisher = 0; publisher != kPublishers; ++publisher) {
    publishers[publisher] = std::thread([&, publisher]() {
      start.arrive_and_wait();
      mt_thread *worker = nullptr;
      attach_statuses[publisher + 1].store(mt_thread_attach(runtime, &worker),
                                           std::memory_order_release);
      if (attach_statuses[publisher + 1].load(std::memory_order_acquire) !=
          MT_OK) {
        publishers_done.fetch_add(1, std::memory_order_release);
        return;
      }

      for (size_t index = 0; index != kTreesPerPublisher; ++index) {
        mt_tree *tree = nullptr;
        if (mt_tree_create(runtime, worker, &tree) != MT_OK ||
            tree == nullptr) {
          violations.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        const uint64_t coordinate =
            UINT64_C(0x200000) + publisher * kTreesPerPublisher + index;
        const auto key = ordered_key(coordinate);
        const mt_record_id candidate = coordinate + 1;
        mt_get_or_insert_result inserted{};
        if (mt_get_or_insert(tree, worker, key.data(), key.size(), candidate,
                             &inserted) != MT_OK ||
            inserted.winner != candidate || inserted.inserted != 1 ||
            inserted.publication != MT_PUBLICATION_CANDIDATE_INSERTED) {
          violations.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        mt_record_id found = MT_RECORD_ID_NONE;
        if (mt_get(tree, worker, key.data(), key.size(), &found) != MT_OK ||
            found != candidate) {
          violations.fetch_add(1, std::memory_order_relaxed);
        }

        /*
         * Exercise first reader-mask registration and structural writes on one
         * shared tree while peer handles are concurrently attached.
         */
        const uint64_t shared_coordinate = UINT64_C(0x300000) +
                                           publisher * kTreesPerPublisher +
                                           index;
        const auto shared_key = ordered_key(shared_coordinate);
        const mt_record_id shared_candidate = shared_coordinate + 1;
        mt_get_or_insert_result shared_inserted{};
        if (mt_get_or_insert(stable_tree, worker, shared_key.data(),
                             shared_key.size(), shared_candidate,
                             &shared_inserted) != MT_OK ||
            shared_inserted.winner != shared_candidate ||
            shared_inserted.inserted != 1 ||
            shared_inserted.publication !=
                MT_PUBLICATION_CANDIDATE_INSERTED) {
          violations.fetch_add(1, std::memory_order_relaxed);
        }
      }
      quiesce_statuses[publisher + 1] = mt_thread_quiesce(worker);
      publishers_done.fetch_add(1, std::memory_order_release);
    });
  }

  for (auto &publisher : publishers) {
    publisher.join();
  }
  reader.join();

  for (size_t index = 0; index != attach_statuses.size(); ++index) {
    EXPECT_EQ(attach_statuses[index].load(std::memory_order_acquire), MT_OK);
    EXPECT_EQ(quiesce_statuses[index], MT_OK);
  }
  EXPECT_EQ(violations.load(std::memory_order_relaxed), 0u);

  for (size_t publisher = 0; publisher != kPublishers; ++publisher) {
    for (size_t index = 0; index != kTreesPerPublisher; ++index) {
      const uint64_t coordinate = UINT64_C(0x300000) +
                                  publisher * kTreesPerPublisher + index;
      const auto key = ordered_key(coordinate);
      mt_record_id found = MT_RECORD_ID_NONE;
      ASSERT_EQ(mt_get(stable_tree, creator, key.data(), key.size(), &found),
                MT_OK);
      EXPECT_EQ(found, coordinate + 1);
    }
  }
}

TEST(MtreeAbiThreading, PointReadersRemainExactAcrossConcurrentSplits) {
  mt_runtime *runtime = default_runtime();
  mt_thread *creator = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, creator);
  constexpr size_t kStableKeys = 1024;
  constexpr size_t kReaders = 4;
  constexpr size_t kWriters = 2;
  constexpr size_t kMinimumReaderPasses = 8;
  for (size_t index = 0; index != kStableKeys; ++index) {
    const uint64_t coordinate = index * 2;
    const auto key = ordered_key(coordinate);
    insert_key(tree, creator, key.data(), key.size(), coordinate + 1);
  }

  constexpr size_t kWorkers = kReaders + kWriters;
  std::array<std::atomic<mt_status>, kWorkers> attach_statuses;
  for (auto &status : attach_statuses) {
    status.store(MT_ERR_INTERNAL, std::memory_order_relaxed);
  }
  std::array<mt_status, kWorkers> quiesce_statuses{};
  std::atomic<size_t> writers_done{0};
  std::atomic<size_t> violations{0};
  std::barrier start(kWorkers);
  std::array<std::thread, kWorkers> workers;

  for (size_t reader = 0; reader != kReaders; ++reader) {
    workers[reader] = std::thread([&, reader]() {
      mt_thread *worker = nullptr;
      attach_statuses[reader].store(mt_thread_attach(runtime, &worker),
                                    std::memory_order_release);
      start.arrive_and_wait();
      if (attach_statuses[reader].load(std::memory_order_acquire) != MT_OK) {
        return;
      }

      size_t passes = 0;
      do {
        for (size_t index = 0; index != kStableKeys; ++index) {
          const uint64_t coordinate = index * 2;
          const auto key = ordered_key(coordinate);
          mt_record_id found = MT_RECORD_ID_NONE;
          if (mt_get(tree, worker, key.data(), key.size(), &found) != MT_OK ||
              found != coordinate + 1) {
            violations.fetch_add(1, std::memory_order_relaxed);
          }
        }
        ++passes;
      } while (passes < kMinimumReaderPasses ||
               writers_done.load(std::memory_order_acquire) != kWriters);
      quiesce_statuses[reader] = mt_thread_quiesce(worker);
    });
  }

  for (size_t writer = 0; writer != kWriters; ++writer) {
    const size_t worker_index = kReaders + writer;
    workers[worker_index] = std::thread([&, writer, worker_index]() {
      mt_thread *worker = nullptr;
      attach_statuses[worker_index].store(mt_thread_attach(runtime, &worker),
                                          std::memory_order_release);
      start.arrive_and_wait();
      if (attach_statuses[worker_index].load(std::memory_order_acquire) !=
          MT_OK) {
        writers_done.fetch_add(1, std::memory_order_release);
        return;
      }

      for (size_t index = writer; index < kStableKeys; index += kWriters) {
        const uint64_t coordinate = index * 2 + 1;
        const auto key = ordered_key(coordinate);
        mt_get_or_insert_result inserted{};
        if (mt_get_or_insert(tree, worker, key.data(), key.size(),
                             coordinate + 1, &inserted) != MT_OK ||
            inserted.winner != coordinate + 1 || inserted.inserted != 1 ||
            inserted.publication != MT_PUBLICATION_CANDIDATE_INSERTED) {
          violations.fetch_add(1, std::memory_order_relaxed);
        }
        if ((index & 7) == 0) {
          std::this_thread::yield();
        }
      }
      quiesce_statuses[worker_index] = mt_thread_quiesce(worker);
      writers_done.fetch_add(1, std::memory_order_release);
    });
  }

  for (auto &worker : workers) {
    worker.join();
  }
  for (size_t index = 0; index != kWorkers; ++index) {
    EXPECT_EQ(attach_statuses[index].load(std::memory_order_acquire), MT_OK);
    EXPECT_EQ(quiesce_statuses[index], MT_OK);
  }
  EXPECT_EQ(violations.load(std::memory_order_relaxed), 0u);

  for (uint64_t coordinate = 0; coordinate != kStableKeys * 2; ++coordinate) {
    const auto key = ordered_key(coordinate);
    mt_record_id found = MT_RECORD_ID_NONE;
    ASSERT_EQ(mt_get(tree, creator, key.data(), key.size(), &found), MT_OK);
    EXPECT_EQ(found, coordinate + 1);
  }
}

TEST(MtreeAbiThreading, LayeredPointReadersRemainExactAcrossConcurrentSplits) {
  mt_runtime *runtime = default_runtime();
  mt_thread *creator = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, creator);
  constexpr size_t kStableKeys = 256;
  constexpr size_t kReaders = 3;
  constexpr size_t kWriters = 2;
  constexpr size_t kMinimumReaderPasses = 4;
  for (size_t index = 0; index != kStableKeys; ++index) {
    const uint64_t coordinate = index * 2;
    const auto key = layered_key(coordinate);
    insert_key(tree, creator, key.data(), key.size(), coordinate + 1);
  }

  constexpr size_t kWorkers = kReaders + kWriters;
  std::array<std::atomic<mt_status>, kWorkers> attach_statuses;
  for (auto &status : attach_statuses) {
    status.store(MT_ERR_INTERNAL, std::memory_order_relaxed);
  }
  std::array<mt_status, kWorkers> quiesce_statuses{};
  std::atomic<size_t> writers_done{0};
  std::atomic<size_t> violations{0};
  std::barrier start(kWorkers);
  std::array<std::thread, kWorkers> workers;

  for (size_t reader = 0; reader != kReaders; ++reader) {
    workers[reader] = std::thread([&, reader]() {
      mt_thread *worker = nullptr;
      attach_statuses[reader].store(mt_thread_attach(runtime, &worker),
                                    std::memory_order_release);
      start.arrive_and_wait();
      if (attach_statuses[reader].load(std::memory_order_acquire) != MT_OK) {
        return;
      }

      size_t passes = 0;
      do {
        for (size_t index = 0; index != kStableKeys; ++index) {
          const uint64_t coordinate = index * 2;
          const auto key = layered_key(coordinate);
          mt_record_id found = MT_RECORD_ID_NONE;
          if (mt_get(tree, worker, key.data(), key.size(), &found) != MT_OK ||
              found != coordinate + 1) {
            violations.fetch_add(1, std::memory_order_relaxed);
          }
        }
        ++passes;
      } while (passes < kMinimumReaderPasses ||
               writers_done.load(std::memory_order_acquire) != kWriters);
      quiesce_statuses[reader] = mt_thread_quiesce(worker);
    });
  }

  for (size_t writer = 0; writer != kWriters; ++writer) {
    const size_t worker_index = kReaders + writer;
    workers[worker_index] = std::thread([&, writer, worker_index]() {
      mt_thread *worker = nullptr;
      attach_statuses[worker_index].store(mt_thread_attach(runtime, &worker),
                                          std::memory_order_release);
      start.arrive_and_wait();
      if (attach_statuses[worker_index].load(std::memory_order_acquire) !=
          MT_OK) {
        writers_done.fetch_add(1, std::memory_order_release);
        return;
      }

      for (size_t index = writer; index < kStableKeys; index += kWriters) {
        const uint64_t coordinate = index * 2 + 1;
        const auto key = layered_key(coordinate);
        mt_get_or_insert_result inserted{};
        if (mt_get_or_insert(tree, worker, key.data(), key.size(),
                             coordinate + 1, &inserted) != MT_OK ||
            inserted.winner != coordinate + 1 || inserted.inserted != 1 ||
            inserted.publication != MT_PUBLICATION_CANDIDATE_INSERTED) {
          violations.fetch_add(1, std::memory_order_relaxed);
        }
        if ((index & 7) == 0) {
          std::this_thread::yield();
        }
      }
      quiesce_statuses[worker_index] = mt_thread_quiesce(worker);
      writers_done.fetch_add(1, std::memory_order_release);
    });
  }

  for (auto &worker : workers) {
    worker.join();
  }
  for (size_t index = 0; index != kWorkers; ++index) {
    EXPECT_EQ(attach_statuses[index].load(std::memory_order_acquire), MT_OK);
    EXPECT_EQ(quiesce_statuses[index], MT_OK);
  }
  EXPECT_EQ(violations.load(std::memory_order_relaxed), 0u);

  for (uint64_t coordinate = 0; coordinate != kStableKeys * 2; ++coordinate) {
    const auto key = layered_key(coordinate);
    mt_record_id found = MT_RECORD_ID_NONE;
    ASSERT_EQ(mt_get(tree, creator, key.data(), key.size(), &found), MT_OK);
    EXPECT_EQ(found, coordinate + 1);
  }
}

TEST(MtreeAbiLifecycle, CloseRacingWithGetIsFailClosedAndMemorySafe) {
  mt_runtime *runtime = default_runtime();
  mt_thread *creator = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, creator);
  const auto key = ordered_key(UINT64_C(0xabcdef));
  constexpr mt_record_id kValue = UINT64_C(0xdecafbad);
  insert_key(tree, creator, key.data(), key.size(), kValue);

  std::atomic<mt_status> attach_status{MT_ERR_INTERNAL};
  std::atomic<mt_status> quiesce_status{MT_ERR_INTERNAL};
  std::atomic<size_t> successful_gets{0};
  std::atomic<size_t> closed_gets{0};
  std::atomic<size_t> violations{0};
  std::barrier attached(2);
  std::thread reader([&]() {
    mt_thread *worker = nullptr;
    attach_status.store(mt_thread_attach(runtime, &worker),
                        std::memory_order_release);
    attached.arrive_and_wait();
    if (attach_status.load(std::memory_order_acquire) != MT_OK) {
      return;
    }
    for (;;) {
      mt_record_id found = 99;
      const mt_status status =
          mt_get(tree, worker, key.data(), key.size(), &found);
      if (status == MT_OK) {
        if (found != kValue) {
          violations.fetch_add(1, std::memory_order_relaxed);
        }
        successful_gets.fetch_add(1, std::memory_order_release);
      } else if (status == MT_ERR_CLOSED) {
        if (found != MT_RECORD_ID_NONE) {
          violations.fetch_add(1, std::memory_order_relaxed);
        }
        closed_gets.fetch_add(1, std::memory_order_relaxed);
        break;
      } else {
        violations.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
    quiesce_status.store(mt_thread_quiesce(worker), std::memory_order_release);
  });

  attached.arrive_and_wait();
  if (attach_status.load(std::memory_order_acquire) != MT_OK) {
    reader.join();
    FAIL() << "reader worker attachment failed";
  }
  while (successful_gets.load(std::memory_order_acquire) < 128) {
    std::this_thread::yield();
  }
  ASSERT_EQ(mt_tree_release(tree), MT_OK);
  reader.join();

  EXPECT_GE(successful_gets.load(std::memory_order_relaxed), 128u);
  EXPECT_EQ(closed_gets.load(std::memory_order_relaxed), 1u);
  EXPECT_EQ(violations.load(std::memory_order_relaxed), 0u);
  EXPECT_EQ(quiesce_status.load(std::memory_order_acquire), MT_OK);

  mt_record_id found = 99;
  EXPECT_EQ(mt_get(tree, creator, key.data(), key.size(), &found),
            MT_ERR_CLOSED);
  EXPECT_EQ(found, MT_RECORD_ID_NONE);
  EXPECT_EQ(mt_tree_release(tree), MT_ERR_CLOSED);
}

TEST(MtreeAbiHandles, UnknownPointersAreRejectedBeforeDereference) {
  mt_runtime *runtime = default_runtime();
  mt_thread *worker = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, worker);
  auto *foreign_runtime = reinterpret_cast<mt_runtime *>(uintptr_t{1});
  auto *foreign_thread = reinterpret_cast<mt_thread *>(uintptr_t{1});
  auto *foreign_tree = reinterpret_cast<mt_tree *>(uintptr_t{1});

  mt_runtime_health_state health = MT_RUNTIME_HEALTHY;
  EXPECT_EQ(mt_runtime_health(foreign_runtime, &health), MT_ERR_INVALID);
  EXPECT_EQ(health, 0u);

  mt_record_id value = 99;
  EXPECT_EQ(mt_get(foreign_tree, worker, nullptr, 0, &value), MT_ERR_INVALID);
  EXPECT_EQ(value, MT_RECORD_ID_NONE);
  EXPECT_EQ(mt_get(tree, foreign_thread, nullptr, 0, &value), MT_ERR_INVALID);
  EXPECT_EQ(value, MT_RECORD_ID_NONE);
  std::array<mt_record_id, 2> values = {99, 99};
  EXPECT_EQ(mt_get_strided(foreign_tree, worker, nullptr, values.size(), 0, 0,
                           values.data()),
            MT_ERR_INVALID);
  EXPECT_EQ(values, (std::array<mt_record_id, 2>{0, 0}));
  values = {99, 99};
  EXPECT_EQ(mt_get_strided(tree, foreign_thread, nullptr, values.size(), 0, 0,
                           values.data()),
            MT_ERR_INVALID);
  EXPECT_EQ(values, (std::array<mt_record_id, 2>{0, 0}));
  EXPECT_EQ(mt_thread_quiesce(foreign_thread), MT_ERR_INVALID);
}

TEST(MtreeAbiLifecycle, ReleasingFacadeNeverDestroysNativeTree) {
  mt_runtime *runtime = default_runtime();
  mt_thread *worker = current_worker(runtime);
  mt_tree *tree = new_tree(runtime, worker);

  EXPECT_EQ(mt_tree_release(tree), MT_OK);
  mt_record_id value = 99;
  EXPECT_EQ(mt_get(tree, worker, nullptr, 0, &value), MT_ERR_CLOSED);
  EXPECT_EQ(value, MT_RECORD_ID_NONE);
  std::array<mt_record_id, 2> values = {99, 99};
  EXPECT_EQ(
      mt_get_strided(tree, worker, nullptr, values.size(), 0, 0, values.data()),
      MT_ERR_CLOSED);
  EXPECT_EQ(values, (std::array<mt_record_id, 2>{0, 0}));

  mt_get_or_insert_result insert_result;
  std::memset(&insert_result, 0xa5, sizeof(insert_result));
  EXPECT_EQ(mt_get_or_insert(tree, worker, nullptr, 0, 1, &insert_result),
            MT_ERR_CLOSED);
  EXPECT_EQ(insert_result.winner, MT_RECORD_ID_NONE);
  EXPECT_EQ(insert_result.inserted, 0u);
  EXPECT_EQ(insert_result.publication,
            MT_PUBLICATION_FAILURE_BEFORE_PUBLICATION);

  const mt_scan_bound absent = absent_bound();
  mt_scan_entry entry{};
  std::array<uint8_t, 8> arena{};
  mt_scan_result scan_result;
  std::memset(&scan_result, 0xa5, sizeof(scan_result));
  EXPECT_EQ(mt_scan(tree, worker, MT_SCAN_FORWARD, &absent, &absent, &entry, 1,
                    arena.data(), arena.size(), &scan_result),
            MT_ERR_CLOSED);
  expect_initialized_scan_result(scan_result);
  EXPECT_EQ(mt_tree_release(tree), MT_ERR_CLOSED);
}

TEST(MtreeAbiZWorkerLimit, ExhaustionIsReportedWithoutAbort) {
  mt_runtime *runtime = default_runtime();
  uint32_t maximum = 0;
  ASSERT_EQ(mt_runtime_max_threads(runtime, &maximum), MT_OK);
  ASSERT_GT(maximum, 0u);

  bool exhausted = false;
  for (size_t attempt = 0; attempt != static_cast<size_t>(maximum) + 2;
       ++attempt) {
    mt_status status = MT_ERR_INTERNAL;
    mt_thread *handle = reinterpret_cast<mt_thread *>(uintptr_t{1});
    std::thread worker([&]() {
      status = mt_thread_attach(runtime, &handle);
      if (status == MT_OK) {
        status = mt_thread_quiesce(handle);
      }
    });
    worker.join();
    if (status == MT_ERR_THREAD_LIMIT) {
      EXPECT_EQ(handle, nullptr);
      exhausted = true;
      break;
    }
    ASSERT_EQ(status, MT_OK);
    ASSERT_NE(handle, nullptr);
  }
  ASSERT_TRUE(exhausted);

  mt_status repeated_status = MT_ERR_INTERNAL;
  mt_thread *rejected = reinterpret_cast<mt_thread *>(uintptr_t{1});
  std::thread after_exhaustion(
      [&]() { repeated_status = mt_thread_attach(runtime, &rejected); });
  after_exhaustion.join();
  EXPECT_EQ(repeated_status, MT_ERR_THREAD_LIMIT);
  EXPECT_EQ(rejected, nullptr);
}

} // namespace
