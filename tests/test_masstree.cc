#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mako/masstree_btree.h"
#include "mako/varkey.h"

using TestTree = single_threaded_btree;

class MasstreeTest : public ::testing::Test {
 protected:
  TestTree tree_;
  std::vector<std::unique_ptr<uint64_t>> storage_;

  TestTree::value_type MakeValue(uint64_t v) {
    storage_.emplace_back(std::make_unique<uint64_t>(v));
    auto* raw = reinterpret_cast<TestTree::value_type>(storage_.back().get());
    return raw;
  }
};

TEST_F(MasstreeTest, InsertSearchAndRemove) {
  constexpr size_t kCount = 256;
  std::vector<u64_varkey> keys;
  keys.reserve(kCount);

  for (size_t i = 0; i < kCount; ++i) {
    keys.emplace_back(static_cast<uint64_t>(i));
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
  std::vector<u64_varkey> keys;
  keys.reserve(kCount);

  for (size_t i = 0; i < kCount; ++i) {
    keys.emplace_back(static_cast<uint64_t>(i));
    EXPECT_TRUE(tree_.insert(keys.back(), MakeValue(i)));
  }

  class CollectCallback : public TestTree::search_range_callback {
   public:
    bool invoke(const TestTree::string_type& key, TestTree::value_type) override {
      results.emplace_back(key.data(), key.length());
      return true;
    }
    std::vector<std::string> results;
  };

  CollectCallback cb;
  TestTree::key_type lower = keys.front();
  tree_.search_range_call(lower, nullptr, cb);

  ASSERT_EQ(cb.results.size(), kCount);
  ASSERT_TRUE(std::is_sorted(cb.results.begin(), cb.results.end()));
}
