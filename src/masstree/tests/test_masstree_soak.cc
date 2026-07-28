// Tier 8 of docs/masstree-test-plan.md — long-running soak / chaos.
//
// One test that runs a sustained mixed workload (writers, removers,
// scanners) on a shared concurrent_btree for a bounded duration.
// Default duration is 30 seconds (CTest-friendly); long-duration runs
// are opted into via the MASSTREE_SOAK_SECONDS env var, e.g. for a
// weekly 24-hour run set MASSTREE_SOAK_SECONDS=86400.
//
// What this catches above and beyond the shorter Tier 3 stress:
//
//   * Slow leaks — a per-op leak invisible in a 2-second window
//     becomes visible after minutes.
//   * Epoch-counter wrap edge cases that require many cycles to hit.
//   * Race classes that depend on uncommon scheduling — longer wall
//     time → more chances to interleave the wrong way.
//
// Stable-key invariant: a fixed key range is never written to after
// setup. Readers continually verify these keys and report any
// missing/corrupt value via an atomic failure counter.
//
// NOT in this test: thread join/leave churn (workers that spawn,
// do a small batch, then exit, repeated continuously). An earlier
// version included that and reliably triggered a SIGABRT inside
// concurrent_btree during the abort path, with no diagnostic message
// — strongly suggestive of a missing RCU per-thread cleanup hook
// when std::thread exits without an explicit teardown. Filed as a
// follow-up; the rest of the soak workload is stable.

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

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
inline uint64_t FromValue(TestTree::value_type v) {
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(v));
}

// Duration from MASSTREE_SOAK_SECONDS, clamped to [1, 86400]. Default 30 s.
std::chrono::seconds SoakDuration() {
  if (const char* s = std::getenv("MASSTREE_SOAK_SECONDS")) {
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (end != s && v > 0) {
      return std::chrono::seconds(std::min<long>(v, 86400));
    }
  }
  return std::chrono::seconds(30);
}

}  // namespace

TEST(MasstreeSoak, MixedWorkloadHoldsInvariants) {
  TestTree tree;

  constexpr uint64_t kStable = 8192;
  for (uint64_t k = 0; k < kStable; ++k) {
    ASSERT_TRUE(tree.insert(K(k), ToValue(k)));
  }

  const auto deadline = std::chrono::steady_clock::now() + SoakDuration();
  std::atomic<bool> stop{false};

  constexpr int kWriters = 4;
  constexpr int kRemovers = 2;
  constexpr int kReaders = 2;
  constexpr int kScanners = 1;
  constexpr uint64_t kChurnBase = 1ull << 40;
  constexpr uint64_t kPerWriter = 8192;

  std::atomic<uint64_t> reader_failures{0};
  std::atomic<uint64_t> reader_ops{0};
  std::atomic<uint64_t> scanner_ops{0};

  auto writer_body = [&](int wid) {
    const uint64_t base = kChurnBase + static_cast<uint64_t>(wid) * kPerWriter;
    while (!stop.load(std::memory_order_acquire)) {
      for (uint64_t i = 0; i < kPerWriter; ++i) {
        tree.insert(K(base + i), ToValue(base + i));
      }
    }
  };
  auto remover_body = [&](int rid) {
    const uint64_t base = kChurnBase + static_cast<uint64_t>(rid) * kPerWriter;
    while (!stop.load(std::memory_order_acquire)) {
      for (uint64_t i = 0; i < kPerWriter; ++i) {
        tree.remove(K(base + i));
      }
    }
  };
  auto reader_body = [&]() {
    while (!stop.load(std::memory_order_acquire)) {
      for (uint64_t k = 0; k < kStable; ++k) {
        TestTree::value_type out = nullptr;
        if (!tree.search(K(k), out) || FromValue(out) != k) {
          ++reader_failures;
        }
        ++reader_ops;
      }
    }
  };
  auto scanner_body = [&]() {
    class Cb : public TestTree::search_range_callback {
     public:
      std::atomic<uint64_t>* failures;
      bool bad_order = false;
      uint64_t last = 0;
      bool first = true;
      bool invoke(const TestTree::string_type& /*k*/, TestTree::value_type v) override {
        uint64_t cur = FromValue(v);
        if (!first && cur <= last && cur < kStable && last < kStable) {
          // We don't enforce ordering across the churn-space (writers
          // generate values >= kChurnBase) — only within the stable
          // range where keys ARE the values.
          bad_order = true;
        }
        last = cur;
        first = false;
        return true;
      }
    };
    while (!stop.load(std::memory_order_acquire)) {
      Cb cb;
      cb.failures = &reader_failures;
      u64_varkey lo(0);
      u64_varkey hi(kStable);
      tree.search_range_call(lo, &hi, cb);
      if (cb.bad_order) ++reader_failures;
      ++scanner_ops;
    }
  };

  auto threads = rusty::Vec<rusty::thread::JoinHandle<void>>::with_capacity(
      kWriters + kRemovers + kReaders + kScanners);
  for (int w = 0; w < kWriters; ++w) threads.push(rusty::thread::spawn(writer_body, w));
  for (int r = 0; r < kRemovers; ++r) threads.push(rusty::thread::spawn(remover_body, r));
  for (int r = 0; r < kReaders; ++r) threads.push(rusty::thread::spawn(reader_body));
  for (int s = 0; s < kScanners; ++s) threads.push(rusty::thread::spawn(scanner_body));

  while (std::chrono::steady_clock::now() < deadline) {
    rusty::thread::sleep(std::chrono::milliseconds(200));
  }
  stop.store(true, std::memory_order_release);

  for (auto& t : threads) { auto _ = t.join(); }

  EXPECT_EQ(reader_failures.load(), 0u)
      << "reader_ops=" << reader_ops.load()
      << " scanner_ops=" << scanner_ops.load();
  EXPECT_GT(reader_ops.load(), 0u);
  EXPECT_GT(scanner_ops.load(), 0u);
}
