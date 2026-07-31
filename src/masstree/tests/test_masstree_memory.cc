// Tier 4 of docs/masstree-test-plan.md — memory & resource correctness.
//
// These tests exercise allocation paths and RCU reclamation patterns
// that, when run under AddressSanitizer (with LeakSanitizer's
// default-on at-exit check) become real detectors:
//
//   * LSan reports any heap allocation still live at process exit.
//   * ASan poisons freed memory; any read of a node that RCU
//     reclaimed will trigger an immediate "heap-use-after-free".
//
// Without ASan/LSan the tests still pass — they assert the visible
// API contract (size, presence, no crash) — but they don't catch the
// invisible failure modes. Pair with the build_asan/ workflow from
// Tier 3.1.

#include <stdint.h>
#include <stddef.h>

#include <gtest/gtest.h>

#include <rusty/thread.hpp>
#include <rusty/vec.hpp>

#include "masstree/kvthread.hh"
#include "mako/masstree_btree.h"
#include "mako/varkey.h"

import std;
import rusty;

volatile mrcu_epoch_type globalepoch = 1;

using TestTree = concurrent_btree;

namespace {

inline u64_varkey K(uint64_t i) { return u64_varkey(i); }
inline TestTree::value_type ToValue(uint64_t v) {
  return reinterpret_cast<TestTree::value_type>(static_cast<uintptr_t>(v));
}

}  // namespace

// -----------------------------------------------------------------------------
// 1. Bulk insert then bulk remove returns the tree to empty.
//
// Detects: per-key leaks in the insert path, lost frees on remove.
// Under LSan this catches every leaked tree-node allocation at exit.
// -----------------------------------------------------------------------------
TEST(MasstreeMemory, MassiveInsertRemoveSizeReturnsToZero) {
  TestTree tree;
  constexpr size_t kCount = 100000;
  for (uint64_t k = 0; k < kCount; ++k) {
    ASSERT_TRUE(tree.insert(K(k), ToValue(k))) << "k=" << k;
  }
  ASSERT_EQ(tree.size(), kCount);
  for (uint64_t k = 0; k < kCount; ++k) {
    ASSERT_TRUE(tree.remove(K(k))) << "k=" << k;
  }
  EXPECT_EQ(tree.size(), 0u);
  // Tree dtor runs at scope exit; LSan checks the heap at process exit.
}

// -----------------------------------------------------------------------------
// 2. Repeated fill/empty cycles do not grow size unboundedly.
//
// Stresses RCU's deferred-reclamation queue: each cycle creates
// short-lived nodes that should be reclaimable. The test does not
// measure RSS (too noisy for CI) — instead, it asserts the visible
// invariant that `size()` always returns to zero after each cycle.
// Under LSan, any cycle whose deferred frees never run shows up as a
// leak at process exit. The cycle count is large enough that an
// O(1)-per-cycle leak would still report at process exit; an
// O(N)-per-cycle leak would balloon RSS noticeably.
// -----------------------------------------------------------------------------
TEST(MasstreeMemory, RepeatedFillEmptyCyclesAreStable) {
  TestTree tree;
  constexpr size_t kKeys = 1024;
  constexpr int kCycles = 1000;
  for (int c = 0; c < kCycles; ++c) {
    for (uint64_t k = 0; k < kKeys; ++k) {
      tree.insert(K(k), ToValue(k));
    }
    for (uint64_t k = 0; k < kKeys; ++k) {
      tree.remove(K(k));
    }
    ASSERT_EQ(tree.size(), 0u) << "cycle " << c;
  }
}

// -----------------------------------------------------------------------------
// 3. Readers run continuously while writers churn an unrelated range.
//
// Targeted RCU UAF probe. Writers allocate and (deferred-)free nodes
// at a high rate; readers traverse a stable range. If the RCU
// machinery frees a node a reader is mid-traversal of, ASan will
// trigger a heap-use-after-free at the dereference site. Without
// ASan this test still asserts that readers complete iterations and
// find their stable keys with the right values, but cannot catch
// the structural UAF.
// -----------------------------------------------------------------------------
TEST(MasstreeMemory, ReadersSurviveAggressiveWriterChurn) {
  TestTree tree;
  constexpr uint64_t kStable = 4096;
  for (uint64_t k = 0; k < kStable; ++k) {
    ASSERT_TRUE(tree.insert(K(k), ToValue(k)));
  }

  constexpr int kWriters = 4;
  constexpr int kReaders = 2;
  constexpr uint64_t kChurnBase = 1ull << 40;
  constexpr uint64_t kChurnPerWriter = 4096;

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> reader_failures{0};
  std::atomic<uint64_t> reader_ops{0};

  auto threads = rusty::Vec<rusty::thread::JoinHandle<rusty::thread::Unit>>::with_capacity(
      kWriters + kReaders);
  for (int w = 0; w < kWriters; ++w) {
    threads.push(rusty::thread::spawn([&, w]() {
      const uint64_t base = kChurnBase + static_cast<uint64_t>(w) * kChurnPerWriter;
      while (!stop.load(std::memory_order_acquire)) {
        for (uint64_t i = 0; i < kChurnPerWriter; ++i) {
          tree.insert(K(base + i), ToValue(base + i));
        }
        for (uint64_t i = 0; i < kChurnPerWriter; ++i) {
          tree.remove(K(base + i));
        }
      }
    }));
  }
  for (int r = 0; r < kReaders; ++r) {
    threads.push(rusty::thread::spawn([&]() {
      while (!stop.load(std::memory_order_acquire)) {
        for (uint64_t k = 0; k < kStable; ++k) {
          TestTree::value_type out = nullptr;
          if (!tree.search(K(k), out) ||
              static_cast<uint64_t>(reinterpret_cast<uintptr_t>(out)) != k) {
            ++reader_failures;
          }
          ++reader_ops;
        }
      }
    }));
  }
  rusty::thread::sleep(std::chrono::milliseconds(2000));
  stop.store(true, std::memory_order_release);
  for (auto& t : threads) { auto _ = t.join(); }

  EXPECT_EQ(reader_failures.load(), 0u);
  EXPECT_GT(reader_ops.load(), 0u);
}
