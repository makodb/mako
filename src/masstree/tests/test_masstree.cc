#include <stdint.h>
#include <stddef.h>

#include <gtest/gtest.h>

#include <rusty/box.hpp>
#include <rusty/vec.hpp>

#include "mako/masstree_btree.h"
#include "mako/varkey.h"

import std;

using TestTree = single_threaded_btree;

namespace {

inline varkey vk(const std::string& s) {
  return varkey(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

}  // namespace

class MasstreeTest : public ::testing::Test {
 protected:
  TestTree tree_;
  // Possible thanks to the rusty::Vec destructor being conditional
  // on T's noexcept-ness — uint64_t and rusty::Box<uint64_t> are
  // both noexcept-destructible, so Vec<Box<uint64_t>> is too, and
  // MasstreeTest's implicit ~MasstreeTest() remains noexcept and
  // does not violate ::testing::Test's noexcept virtual destructor.
  rusty::Vec<rusty::Box<uint64_t>> storage_;

  TestTree::value_type MakeValue(uint64_t v) {
    storage_.push(rusty::Box<uint64_t>::make(v));
    return reinterpret_cast<TestTree::value_type>(storage_.back().get());
  }

  static uint64_t Decode(TestTree::value_type v) {
    return *reinterpret_cast<const uint64_t*>(v);
  }
};

TEST_F(MasstreeTest, InsertSearchAndRemove) {
  constexpr size_t kCount = 256;
  auto keys = rusty::Vec<u64_varkey>::with_capacity(kCount);

  for (size_t i = 0; i < kCount; ++i) {
    keys.push(u64_varkey(static_cast<uint64_t>(i)));
    EXPECT_TRUE(tree_.insert(keys.back(), MakeValue(i))) << "insert failed for " << i;
  }
  EXPECT_EQ(tree_.size(), kCount);

  for (size_t i = 0; i < kCount; ++i) {
    TestTree::value_type found{};
    EXPECT_TRUE(tree_.search(keys[i], found));
    ASSERT_NE(found, nullptr);
    auto decoded = *reinterpret_cast<uint64_t*>(found);
    EXPECT_EQ(decoded, i);
  }

  for (size_t i = 0; i < kCount; i += 2) {
    EXPECT_TRUE(tree_.remove(keys[i]));
  }
  EXPECT_EQ(tree_.size(), kCount / 2);

  for (size_t i = 0; i < kCount; ++i) {
    TestTree::value_type found{};
    bool exists = tree_.search(keys[i], found);
    if (i % 2 == 0) {
      EXPECT_FALSE(exists);
    } else {
      EXPECT_TRUE(exists);
      auto decoded = *reinterpret_cast<uint64_t*>(found);
      EXPECT_EQ(decoded, i);
    }
  }
}

TEST_F(MasstreeTest, RangeScanReturnsSortedKeys) {
  constexpr size_t kCount = 128;
  auto keys = rusty::Vec<u64_varkey>::with_capacity(kCount);

  for (size_t i = 0; i < kCount; ++i) {
    keys.push(u64_varkey(static_cast<uint64_t>(i)));
    EXPECT_TRUE(tree_.insert(keys.back(), MakeValue(i)));
  }

  class CollectCallback : public TestTree::search_range_callback {
   public:
    bool invoke(const TestTree::string_type& key, TestTree::value_type) override {
      results.push(std::string(key.data(), key.length()));
      return true;
    }
    rusty::Vec<std::string> results;
  };

  CollectCallback cb;
  TestTree::key_type lower = keys.front();
  tree_.search_range_call_unbounded(lower, cb);

  ASSERT_EQ(cb.results.size(), kCount);
  ASSERT_TRUE(std::is_sorted(cb.results.begin(), cb.results.end()));
}

// =============================================================================
// Tier 1.1 / 1.2 — key-shape matrix × per-key operation matrix.
//
// For each interesting key shape (slice-boundary, layer-spanning,
// embedded-null, etc.) run the single-key op matrix: insert-new,
// insert-duplicate (overwrite), insert-if-absent, search-hit / miss,
// search-after-remove, remove-existing / missing, remove-then-reinsert.
// =============================================================================

struct KeyShape {
  std::string name;
  std::string key;
};

class MasstreeKeyShape
    : public MasstreeTest,
      public ::testing::WithParamInterface<KeyShape> {};

namespace {

rusty::Vec<KeyShape> AllShapes() {
  return rusty::Vec<KeyShape>({
      {"1byte",         "x"},
      {"7byte",         "1234567"},
      {"8byte_boundary","12345678"},
      {"9byte_2slices", "123456789"},
      {"16byte",        "ABCDEFGHabcdefgh"},
      {"17byte",        "ABCDEFGHabcdefgh!"},
      {"64byte",        std::string(64,  'b')},
      {"256byte",       std::string(256, 'c')},
      {"embedded_nulls",std::string("\x01\x00\x02\x00\x03\x00\x04", 7)},
  });
}

}  // namespace

INSTANTIATE_TEST_SUITE_P(
    KeyShapes, MasstreeKeyShape, ::testing::ValuesIn(AllShapes()),
    [](const ::testing::TestParamInfo<KeyShape>& info) {
      return info.param.name;
    });

TEST_P(MasstreeKeyShape, InsertNew) {
  const auto& key = GetParam().key;
  EXPECT_TRUE(tree_.insert(vk(key), MakeValue(42)));
  EXPECT_EQ(tree_.size(), 1u);
}

TEST_P(MasstreeKeyShape, InsertDuplicateOverwritesAndReturnsFalse) {
  const auto& key = GetParam().key;
  ASSERT_TRUE(tree_.insert(vk(key), MakeValue(1)));
  TestTree::value_type old = nullptr;
  EXPECT_FALSE(tree_.insert_with_old(vk(key), MakeValue(2), old));
  ASSERT_NE(old, nullptr);
  EXPECT_EQ(Decode(old), 1u);
  EXPECT_EQ(tree_.size(), 1u);
  TestTree::value_type now = nullptr;
  ASSERT_TRUE(tree_.search(vk(key), now));
  EXPECT_EQ(Decode(now), 2u);
}

TEST_P(MasstreeKeyShape, InsertIfAbsentReturnsFalseForExisting) {
  const auto& key = GetParam().key;
  ASSERT_TRUE(tree_.insert(vk(key), MakeValue(1)));
  EXPECT_FALSE(tree_.insert_if_absent(vk(key), MakeValue(2)));
  TestTree::value_type now = nullptr;
  ASSERT_TRUE(tree_.search(vk(key), now));
  EXPECT_EQ(Decode(now), 1u);
}

TEST_P(MasstreeKeyShape, SearchHit) {
  const auto& key = GetParam().key;
  ASSERT_TRUE(tree_.insert(vk(key), MakeValue(99)));
  TestTree::value_type v = nullptr;
  EXPECT_TRUE(tree_.search(vk(key), v));
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(Decode(v), 99u);
}

TEST_P(MasstreeKeyShape, SearchMissOnEmptyTree) {
  const auto& key = GetParam().key;
  TestTree::value_type v = nullptr;
  EXPECT_FALSE(tree_.search(vk(key), v));
}

TEST_P(MasstreeKeyShape, SearchMissForOtherKey) {
  const auto& present = GetParam().key;
  const std::string absent = present + std::string("\x7F", 1);  // suffix unlikely to collide
  ASSERT_TRUE(tree_.insert(vk(present), MakeValue(1)));
  TestTree::value_type v = nullptr;
  EXPECT_FALSE(tree_.search(vk(absent), v));
}

TEST_P(MasstreeKeyShape, RemoveExisting) {
  const auto& key = GetParam().key;
  ASSERT_TRUE(tree_.insert(vk(key), MakeValue(7)));
  EXPECT_TRUE(tree_.remove(vk(key)));
  EXPECT_EQ(tree_.size(), 0u);
  TestTree::value_type v = nullptr;
  EXPECT_FALSE(tree_.search(vk(key), v));
}

TEST_P(MasstreeKeyShape, RemoveMissingReturnsFalse) {
  const auto& key = GetParam().key;
  EXPECT_FALSE(tree_.remove(vk(key)));
}

TEST_P(MasstreeKeyShape, RemoveThenReinsert) {
  const auto& key = GetParam().key;
  ASSERT_TRUE(tree_.insert(vk(key), MakeValue(1)));
  ASSERT_TRUE(tree_.remove(vk(key)));
  EXPECT_TRUE(tree_.insert(vk(key), MakeValue(2)));
  TestTree::value_type v = nullptr;
  ASSERT_TRUE(tree_.search(vk(key), v));
  EXPECT_EQ(Decode(v), 2u);
}

// Empty key gets its own dedicated test rather than going through the
// parameterized matrix: if it is rejected by the implementation, that
// surfaces as one focused failure instead of nine.
TEST_F(MasstreeTest, EmptyKeyRoundTrip) {
  const std::string empty;
  EXPECT_TRUE(tree_.insert(vk(empty), MakeValue(42)));
  EXPECT_EQ(tree_.size(), 1u);
  TestTree::value_type v = nullptr;
  ASSERT_TRUE(tree_.search(vk(empty), v));
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(Decode(v), 42u);
  EXPECT_TRUE(tree_.remove(vk(empty)));
  EXPECT_EQ(tree_.size(), 0u);
}

// =============================================================================
// Tier 1.3 — structural triggers.
//
// Workloads designed to exercise B+tree mechanics: edge-direction splits,
// layer expansion when keys share long prefixes, and reduction back to
// empty on full removal. Default Masstree leaf_width = 15.
// =============================================================================

TEST_F(MasstreeTest, InsertAscendingForcesRightEdgeSplits) {
  constexpr size_t kCount = 1024;
  auto keys = rusty::Vec<u64_varkey>::with_capacity(kCount);
  for (size_t i = 0; i < kCount; ++i) {
    keys.push(u64_varkey(static_cast<uint64_t>(i)));
    ASSERT_TRUE(tree_.insert(keys.back(), MakeValue(i))) << "ascending insert " << i;
  }
  EXPECT_EQ(tree_.size(), kCount);
  for (size_t i = 0; i < kCount; ++i) {
    TestTree::value_type v = nullptr;
    ASSERT_TRUE(tree_.search(keys[i], v));
    EXPECT_EQ(Decode(v), i);
  }
}

TEST_F(MasstreeTest, InsertDescendingForcesLeftEdgeSplits) {
  constexpr size_t kCount = 1024;
  auto keys = rusty::Vec<u64_varkey>::with_capacity(kCount);
  for (size_t i = kCount; i > 0; --i) {
    keys.push(u64_varkey(static_cast<uint64_t>(i - 1)));
    ASSERT_TRUE(tree_.insert(keys.back(), MakeValue(i - 1)));
  }
  EXPECT_EQ(tree_.size(), kCount);
  for (auto& k : keys) {
    TestTree::value_type v = nullptr;
    ASSERT_TRUE(tree_.search(k, v));
  }
}

TEST_F(MasstreeTest, InsertRandomOrderHasAllKeys) {
  constexpr size_t kCount = 1024;
  auto idx = rusty::Vec<uint64_t>::with_capacity(kCount);
  for (uint64_t i = 0; i < kCount; ++i) idx.push(i);
  std::mt19937 rng(0xC0FFEEull);
  std::shuffle(idx.begin(), idx.end(), rng);
  auto keys = rusty::Vec<u64_varkey>::with_capacity(kCount);
  for (uint64_t i : idx) {
    keys.push(u64_varkey(i));
    ASSERT_TRUE(tree_.insert(keys.back(), MakeValue(i)));
  }
  EXPECT_EQ(tree_.size(), kCount);
  for (uint64_t i = 0; i < kCount; ++i) {
    u64_varkey k(i);
    TestTree::value_type v = nullptr;
    ASSERT_TRUE(tree_.search(k, v));
    EXPECT_EQ(Decode(v), i);
  }
}

TEST_F(MasstreeTest, LayerExpansionFromSharedPrefix) {
  // 64 keys sharing a 24-byte prefix (three full slices), differentiated
  // by trailing bytes — exercises Masstree layer creation.
  const std::string prefix(24, 'p');
  constexpr size_t kCount = 64;
  auto raws = rusty::Vec<std::string>::with_capacity(kCount);
  for (size_t i = 0; i < kCount; ++i) {
    std::string k = prefix;
    k.push_back(static_cast<char>('A' + (i % 26)));
    k.append(std::to_string(i));
    raws.push(std::move(k));
    ASSERT_TRUE(tree_.insert(vk(raws.back()), MakeValue(i)));
  }
  EXPECT_EQ(tree_.size(), kCount);
  for (size_t i = 0; i < kCount; ++i) {
    TestTree::value_type v = nullptr;
    ASSERT_TRUE(tree_.search(vk(raws[i]), v));
    EXPECT_EQ(Decode(v), i);
  }
}

TEST_F(MasstreeTest, RemoveAllReturnsToEmpty) {
  constexpr size_t kCount = 256;
  auto keys = rusty::Vec<u64_varkey>::with_capacity(kCount);
  for (size_t i = 0; i < kCount; ++i) {
    keys.push(u64_varkey(static_cast<uint64_t>(i)));
    ASSERT_TRUE(tree_.insert(keys.back(), MakeValue(i)));
  }
  for (size_t i = 0; i < kCount; ++i) {
    ASSERT_TRUE(tree_.remove(keys[i]));
  }
  EXPECT_EQ(tree_.size(), 0u);
  for (size_t i = 0; i < kCount; ++i) {
    TestTree::value_type v = nullptr;
    EXPECT_FALSE(tree_.search(keys[i], v));
  }
}

// =============================================================================
// Tier 1.4 — scan semantics.
//
// Verify the documented contract of search_range_call and
// rsearch_range_call: forward = [lower, *upper), reverse = (*lower, upper].
// =============================================================================

namespace {

class Collect : public TestTree::search_range_callback {
 public:
  rusty::Vec<std::string> keys;
  rusty::Vec<uint64_t> values;
  size_t limit = std::numeric_limits<size_t>::max();
  bool invoke(const TestTree::string_type& k, TestTree::value_type v) override {
    keys.push(std::string(k.data(), k.length()));
    values.push(*reinterpret_cast<const uint64_t*>(v));
    return keys.size() < limit;
  }
};

}  // namespace

TEST_F(MasstreeTest, ForwardScanRespectsExclusiveUpper) {
  constexpr size_t kCount = 100;
  auto keys = rusty::Vec<u64_varkey>::with_capacity(kCount);
  for (size_t i = 0; i < kCount; ++i) {
    keys.push(u64_varkey(static_cast<uint64_t>(i)));
    ASSERT_TRUE(tree_.insert(keys.back(), MakeValue(i)));
  }
  Collect cb;
  TestTree::key_type lo = keys[10];
  TestTree::key_type hi = keys[20];
  tree_.search_range_call_bounded(lo, hi, cb);
  ASSERT_EQ(cb.values.size(), 10u);
  EXPECT_EQ(cb.values.front(), 10u);
  EXPECT_EQ(cb.values.back(),  19u);
}

TEST_F(MasstreeTest, ForwardScanNullUpperIsUnbounded) {
  constexpr size_t kCount = 50;
  auto keys = rusty::Vec<u64_varkey>::with_capacity(kCount);
  for (size_t i = 0; i < kCount; ++i) {
    keys.push(u64_varkey(static_cast<uint64_t>(i)));
    ASSERT_TRUE(tree_.insert(keys.back(), MakeValue(i)));
  }
  Collect cb;
  TestTree::key_type lo = keys.front();
  tree_.search_range_call_unbounded(lo, cb);
  EXPECT_EQ(cb.values.size(), kCount);
}

TEST_F(MasstreeTest, ReverseScanRespectsInclusiveUpperAndExclusiveLower) {
  constexpr size_t kCount = 100;
  auto keys = rusty::Vec<u64_varkey>::with_capacity(kCount);
  for (size_t i = 0; i < kCount; ++i) {
    keys.push(u64_varkey(static_cast<uint64_t>(i)));
    ASSERT_TRUE(tree_.insert(keys.back(), MakeValue(i)));
  }
  Collect cb;
  TestTree::key_type up = keys[20];
  TestTree::key_type lo = keys[10];
  tree_.rsearch_range_call_bounded(up, lo, cb);
  ASSERT_EQ(cb.values.size(), 10u);
  EXPECT_EQ(cb.values.front(), 20u);
  EXPECT_EQ(cb.values.back(),  11u);
}

TEST_F(MasstreeTest, ReverseScanNullLowerIsUnbounded) {
  constexpr size_t kCount = 50;
  auto keys = rusty::Vec<u64_varkey>::with_capacity(kCount);
  for (size_t i = 0; i < kCount; ++i) {
    keys.push(u64_varkey(static_cast<uint64_t>(i)));
    ASSERT_TRUE(tree_.insert(keys.back(), MakeValue(i)));
  }
  Collect cb;
  TestTree::key_type up = keys.back();
  tree_.rsearch_range_call_unbounded(up, cb);
  EXPECT_EQ(cb.values.size(), kCount);
}

TEST_F(MasstreeTest, ScanOnEmptyTreeYieldsNothing) {
  Collect cb;
  u64_varkey lo(0);
  tree_.search_range_call_unbounded(lo, cb);
  EXPECT_TRUE(cb.values.is_empty());
}

TEST_F(MasstreeTest, ScanStopsWhenCallbackReturnsFalse) {
  constexpr size_t kCount = 100;
  auto keys = rusty::Vec<u64_varkey>::with_capacity(kCount);
  for (size_t i = 0; i < kCount; ++i) {
    keys.push(u64_varkey(static_cast<uint64_t>(i)));
    ASSERT_TRUE(tree_.insert(keys.back(), MakeValue(i)));
  }
  Collect cb;
  cb.limit = 7;
  TestTree::key_type lo = keys.front();
  tree_.search_range_call_unbounded(lo, cb);
  EXPECT_EQ(cb.values.size(), 7u);
}

TEST_F(MasstreeTest, ScanCrossesLayers) {
  // Two shared-prefix groups, 8 keys each — guaranteed to straddle Masstree
  // layer boundaries. Scan should see all 16 in sorted order.
  const std::string p1(16, 'a');
  const std::string p2(16, 'b');
  auto raws = rusty::Vec<std::string>::with_capacity(16);
  for (int i = 0; i < 8; ++i) {
    raws.push(p1 + std::to_string(i));
    ASSERT_TRUE(tree_.insert(vk(raws.back()), MakeValue(100 + i)));
  }
  for (int i = 0; i < 8; ++i) {
    raws.push(p2 + std::to_string(i));
    ASSERT_TRUE(tree_.insert(vk(raws.back()), MakeValue(200 + i)));
  }
  Collect cb;
  const std::string lower_raw(p1);
  varkey lo = vk(lower_raw);
  tree_.search_range_call_unbounded(lo, cb);
  EXPECT_EQ(cb.values.size(), 16u);
  EXPECT_TRUE(std::is_sorted(cb.keys.begin(), cb.keys.end()));
}

TEST_F(MasstreeTest, ScanStartKeyJustRemovedSkipsIt) {
  auto keys = rusty::Vec<u64_varkey>::with_capacity(10);
  for (size_t i = 0; i < 10; ++i) {
    keys.push(u64_varkey(static_cast<uint64_t>(i)));
    ASSERT_TRUE(tree_.insert(keys.back(), MakeValue(i)));
  }
  ASSERT_TRUE(tree_.remove(keys[3]));
  Collect cb;
  TestTree::key_type lo = keys[3];
  tree_.search_range_call_unbounded(lo, cb);
  ASSERT_FALSE(cb.values.is_empty());
  EXPECT_EQ(cb.values.front(), 4u);
  EXPECT_EQ(cb.values.size(), 6u);  // 4..9
}

// =============================================================================
// Tier 1.5 — iterator contract under callback-initiated mutation.
//
// single_threaded_btree has no concurrency, but the callback API can still
// observe a tree mutated by the callback itself. The documented guarantee
// is *weak consistency*: scan must not crash, must not return garbage, and
// keys it does emit must be in sorted order. Whether the mutation is
// observed in the same scan is unspecified.
// =============================================================================

TEST_F(MasstreeTest, InsertDuringScanIsWeaklyConsistent) {
  for (size_t i = 0; i < 50; ++i) {
    u64_varkey k(static_cast<uint64_t>(i));
    ASSERT_TRUE(tree_.insert(k, MakeValue(i)));
  }

  auto later_keys = rusty::Vec<u64_varkey>::with_capacity(10);
  auto later_values = rusty::Vec<TestTree::value_type>::with_capacity(10);
  for (size_t i = 100; i < 110; ++i) {
    later_keys.push(u64_varkey(static_cast<uint64_t>(i)));
    later_values.push(MakeValue(i));
  }

  class MutatingCallback : public TestTree::search_range_callback {
   public:
    TestTree* tree;
    const rusty::Vec<u64_varkey>* keys;
    const rusty::Vec<TestTree::value_type>* vals;
    size_t inserted = 0;
    rusty::Vec<std::string> observed;
    bool invoke(const TestTree::string_type& k, TestTree::value_type) override {
      observed.push(std::string(k.data(), k.length()));
      if (inserted < keys->size()) {
        tree->insert((*keys)[inserted], (*vals)[inserted]);
        ++inserted;
      }
      return true;
    }
  };
  MutatingCallback cb;
  cb.tree = &tree_;
  cb.keys = &later_keys;
  cb.vals = &later_values;

  u64_varkey lo(0);
  tree_.search_range_call_unbounded(lo, cb);

  EXPECT_GE(cb.observed.size(), 50u);
  EXPECT_TRUE(std::is_sorted(cb.observed.begin(), cb.observed.end()));

  for (size_t i = 0; i < 50; ++i) {
    u64_varkey k(static_cast<uint64_t>(i));
    TestTree::value_type v = nullptr;
    EXPECT_TRUE(tree_.search(k, v)) << "lost original key " << i;
  }
  for (size_t i = 0; i < cb.inserted; ++i) {
    u64_varkey k(static_cast<uint64_t>(100 + i));
    TestTree::value_type v = nullptr;
    EXPECT_TRUE(tree_.search(k, v)) << "lost in-scan insert " << (100 + i);
  }
}

TEST_F(MasstreeTest, RemoveDuringScanIsWeaklyConsistent) {
  auto keys = rusty::Vec<u64_varkey>::with_capacity(50);
  for (size_t i = 0; i < 50; ++i) {
    keys.push(u64_varkey(static_cast<uint64_t>(i)));
    ASSERT_TRUE(tree_.insert(keys.back(), MakeValue(i)));
  }
  class RemovingCallback : public TestTree::search_range_callback {
   public:
    TestTree* tree;
    const rusty::Vec<u64_varkey>* keys;
    rusty::Vec<std::string> observed;
    bool invoke(const TestTree::string_type& k, TestTree::value_type) override {
      observed.push(std::string(k.data(), k.length()));
      const size_t idx = observed.size() + 10;
      if (idx < keys->size()) {
        tree->remove((*keys)[idx]);
      }
      return true;
    }
  };
  RemovingCallback cb;
  cb.tree = &tree_;
  cb.keys = &keys;
  u64_varkey lo(0);
  tree_.search_range_call_unbounded(lo, cb);
  EXPECT_TRUE(std::is_sorted(cb.observed.begin(), cb.observed.end()));
}
