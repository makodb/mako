// Tier 2 of docs/masstree-test-plan.md — property-based test driving a
// rusty::BTreeMap<std::string,uint64_t> oracle.
//
// Each iteration draws an op from {insert, insert_if_absent, remove,
// search, range-scan} weighted by realistic mix, applies it to both
// the tree and the oracle, and asserts equivalent return values. Every
// ~5 % of the schedule a full forward scan is compared element-wise.
//
// Seeds are listed explicitly and printed on failure so the failing
// run can be replayed deterministically.

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include <gtest/gtest.h>

#include <rusty/box.hpp>
#include <rusty/vec.hpp>

#include "mako/masstree_btree.h"
#include "mako/varkey.h"

import std;
import rusty;

using TestTree = single_threaded_btree;

namespace {

inline varkey vk(const std::string& s) {
  return varkey(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// Builds a key pool whose members cover the cases that exercise
// Masstree's slice / layer machinery: short keys, slice-boundary
// keys, multi-slice keys, and clusters that share a long prefix.
rusty::Vec<std::string> BuildKeyPool(std::mt19937_64& rng, size_t count) {
  auto pool = rusty::Vec<std::string>::with_capacity(count);

  const std::array<std::string, 4> prefixes = {
      std::string(),
      std::string(8, 'p'),
      std::string(16, 'q'),
      std::string(24, 'r'),
  };

  std::uniform_int_distribution<size_t> pick_prefix(0, prefixes.size() - 1);
  std::uniform_int_distribution<int> suffix_len(0, 32);
  std::uniform_int_distribution<int> byte(0, 255);

  for (size_t i = 0; i < count; ++i) {
    std::string k = prefixes[pick_prefix(rng)];
    const int len = suffix_len(rng);
    for (int j = 0; j < len; ++j) {
      k.push_back(static_cast<char>(byte(rng)));
    }
    if (k.empty()) {
      // The empty key is exercised by Tier 1 — keep the pool non-empty
      // so any oracle divergence here points at non-trivial key paths.
      k.push_back(static_cast<char>(byte(rng)));
    }
    pool.push(std::move(k));
  }
  return pool;
}

struct PropertyState {
  TestTree tree;
  rusty::BTreeMap<std::string, uint64_t> oracle;
  rusty::Vec<rusty::Box<uint64_t>> value_storage;

  TestTree::value_type Make(uint64_t v) {
    value_storage.push(rusty::Box<uint64_t>::make(v));
    return reinterpret_cast<TestTree::value_type>(value_storage.back().get());
  }

  static uint64_t Decode(TestTree::value_type v) {
    return *reinterpret_cast<const uint64_t*>(v);
  }

  void FullScanMatchesOracle(uint64_t seed, size_t step) const {
    class Cb : public TestTree::search_range_callback {
     public:
      rusty::Vec<std::pair<std::string, uint64_t>> seen;
      bool invoke(const TestTree::string_type& k, TestTree::value_type v) override {
        seen.push(std::pair<std::string, uint64_t>(
            std::string(k.data(), k.length()),
            *reinterpret_cast<const uint64_t*>(v)));
        return true;
      }
    };
    Cb cb;
    const std::string empty;
    varkey lo = vk(empty);
    tree.search_range_call(lo, nullptr, cb);

    ASSERT_EQ(cb.seen.len(), oracle.len())
        << "seed=" << std::hex << seed << " step=" << std::dec << step;

    size_t i = 0;
    for (const auto& [k, v] : oracle) {
      ASSERT_EQ(cb.seen[i].first, k)
          << "seed=" << std::hex << seed << " step=" << std::dec
          << step << " i=" << i;
      ASSERT_EQ(cb.seen[i].second, v)
          << "seed=" << std::hex << seed << " step=" << std::dec
          << step << " i=" << i;
      ++i;
    }
  }
};

void RunPropertySession(uint64_t seed, size_t iterations) {
  std::mt19937_64 rng(seed);
  PropertyState s;
  auto pool = BuildKeyPool(rng, /*count=*/200);

  std::uniform_int_distribution<size_t> pick_key(0, pool.len() - 1);
  std::uniform_int_distribution<int> pick_op(0, 99);
  std::uniform_int_distribution<uint64_t> any_val(
      0, std::numeric_limits<uint64_t>::max());

  const size_t check_every = std::max<size_t>(1, iterations / 20);

  for (size_t step = 0; step < iterations; ++step) {
    const auto& key = pool[pick_key(rng)];
    const int op = pick_op(rng);

    if (op < 45) {
      // insert (45 %): always succeeds; returns true iff key was new.
      const uint64_t v = any_val(rng);
      const bool was_new_oracle = !s.oracle.contains_key(key);
      s.oracle.insert(key, v);
      const bool was_new_tree = s.tree.insert(vk(key), s.Make(v));
      ASSERT_EQ(was_new_tree, was_new_oracle)
          << "seed=" << std::hex << seed << " step=" << std::dec << step;
    } else if (op < 55) {
      // insert_if_absent (10 %)
      const uint64_t v = any_val(rng);
      const bool exists_oracle = s.oracle.contains_key(key);
      const bool inserted_tree = s.tree.insert_if_absent(vk(key), s.Make(v));
      ASSERT_EQ(inserted_tree, !exists_oracle)
          << "seed=" << std::hex << seed << " step=" << std::dec << step;
      if (!exists_oracle) {
        s.oracle.insert(key, v);
      }
    } else if (op < 75) {
      // remove (20 %)
      const bool existed_oracle = s.oracle.remove(key).is_some();
      const bool removed_tree = s.tree.remove(vk(key));
      ASSERT_EQ(removed_tree, existed_oracle)
          << "seed=" << std::hex << seed << " step=" << std::dec << step;
    } else if (op < 90) {
      // search (15 %)
      auto oracle_val = s.oracle.get(key);
      TestTree::value_type v = nullptr;
      const bool found = s.tree.search(vk(key), v);
      ASSERT_EQ(found, oracle_val.is_some())
          << "seed=" << std::hex << seed << " step=" << std::dec << step;
      if (found) {
        ASSERT_EQ(PropertyState::Decode(v), oracle_val.unwrap())
            << "seed=" << std::hex << seed << " step=" << std::dec << step;
      }
    } else {
      // range scan (10 %)
      const std::string& a = pool[pick_key(rng)];
      const std::string& b = pool[pick_key(rng)];
      const std::string& lo_s = (a <= b ? a : b);
      const std::string& hi_s = (a <= b ? b : a);

      class CollectAll : public TestTree::search_range_callback {
       public:
        rusty::Vec<std::string> seen;
        bool invoke(const TestTree::string_type& k, TestTree::value_type) override {
          seen.push(std::string(k.data(), k.length()));
          return true;
        }
      };
      CollectAll cb;
      varkey lo = vk(lo_s);
      varkey hi = vk(hi_s);
      s.tree.search_range_call(lo, &hi, cb);

      auto expected = s.oracle.range_rusty(lo_s, hi_s);
      ASSERT_EQ(cb.seen.len(), expected.len())
          << "seed=" << std::hex << seed << " step=" << std::dec << step
          << " lo.size=" << lo_s.size() << " hi.size=" << hi_s.size();
      for (size_t i = 0; i < expected.len(); ++i) {
        ASSERT_EQ(cb.seen[i], expected[i].first)
            << "seed=" << std::hex << seed << " step=" << std::dec
            << step << " i=" << i;
      }
    }

    if ((step + 1) % check_every == 0) {
      s.FullScanMatchesOracle(seed, step);
      if (::testing::Test::HasFailure()) return;
    }
  }
  s.FullScanMatchesOracle(seed, iterations);
}

std::string FormatSeed(uint64_t s) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "seed_%016llx",
                static_cast<unsigned long long>(s));
  return std::string(buf);
}

}  // namespace

class MasstreeProperty : public ::testing::TestWithParam<uint64_t> {};

INSTANTIATE_TEST_SUITE_P(
    Seeds, MasstreeProperty,
    ::testing::Values<uint64_t>(0xC0FFEEull, 0xDEADBEEFull, 0xFEEDFACEull,
                                0xCAFEBABEull, 0xBADDCAFEull, 0xABCDEFull,
                                0x5EED1234ull, 0x1234567890ABCDEFull),
    [](const ::testing::TestParamInfo<uint64_t>& info) {
      return FormatSeed(info.param);
    });

TEST_P(MasstreeProperty, MatchesStdMapOracle) {
  RunPropertySession(GetParam(), /*iterations=*/5000);
}

// One longer soak session on a fixed seed — same property, more rounds.
TEST(MasstreePropertySoak, MatchesStdMapOracle50k) {
  RunPropertySession(0xA11C0DEull, /*iterations=*/50000);
}
