// Tier 7 of docs/masstree-test-plan.md — Masstree-specific edge cases
// around its trie-of-B+trees design.
//
// Masstree indexes keys by 8-byte "slices". When N keys share a
// complete 8-byte slice that isn't the trailing slice, Masstree
// creates a sub-tree (a "layer") rooted at the diverging slice.
// Each additional shared 8-byte chunk adds another layer. This file
// pokes at the code paths that arise from that design:
//
//   1. Deep layers from long shared prefixes.
//   2. Layer collapse when the keys that created a layer are removed.
//   3. Wildly different key lengths sharing the same first slice.
//   4. Every key length from 0..32 under a common prefix (slice
//      boundary cross-product).
//
// Under ASan/LSan the layer-collapse test doubles as a leak detector
// for any layer whose teardown is skipped.

#include <stdint.h>
#include <stddef.h>

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

class StorageBank {
 public:
  TestTree::value_type Make(uint64_t v) {
    slots_.push(rusty::Box<uint64_t>::make(v));
    return reinterpret_cast<TestTree::value_type>(slots_.back().get());
  }
  static uint64_t Decode(TestTree::value_type v) {
    return *reinterpret_cast<const uint64_t*>(v);
  }
 private:
  rusty::Vec<rusty::Box<uint64_t>> slots_;
};

}  // namespace

// -----------------------------------------------------------------------------
// 1. DeepLayerKeysRoundTrip — long shared prefix forces a deep layer
// chain, must still round-trip every key.
//
// A 64-byte shared prefix is 8 full slices; the 9th slice contains
// the per-key distinguishing bytes. Default leaf_width = 15, so 16+
// distinct distinguishing bytes also force a split inside the
// deepest layer's leaf.
// -----------------------------------------------------------------------------
TEST(MasstreeLayered, DeepLayerKeysRoundTrip) {
  TestTree tree;
  StorageBank bank;
  const std::string prefix(64, 'p');  // 8 slices of 'p'
  constexpr size_t kCount = 32;
  auto raws = rusty::Vec<std::string>::with_capacity(kCount);
  for (size_t i = 0; i < kCount; ++i) {
    std::string k = prefix;
    k.push_back(static_cast<char>(i));      // 65th byte differs
    k.push_back(static_cast<char>(i ^ 0x5A)); // 66th byte differs
    raws.push(std::move(k));
    ASSERT_TRUE(tree.insert(vk(raws.back()), bank.Make(i))) << "i=" << i;
  }
  ASSERT_EQ(tree.size(), kCount);
  for (size_t i = 0; i < kCount; ++i) {
    TestTree::value_type out = nullptr;
    ASSERT_TRUE(tree.search(vk(raws[i]), out)) << "i=" << i;
    EXPECT_EQ(StorageBank::Decode(out), i) << "i=" << i;
  }
}

// -----------------------------------------------------------------------------
// 2. LayerCollapseOnRemoval — insert enough deep-prefix keys to
// create multiple layers, then remove all of them, and verify the
// tree returns to empty. Under ASan/LSan this catches a missing
// layer-free in masstree_remove.hh.
// -----------------------------------------------------------------------------
TEST(MasstreeLayered, LayerCollapseOnRemoval) {
  TestTree tree;
  StorageBank bank;
  // 24-byte (3 slice) shared prefix, plus 9 bytes of differentiating
  // suffix per key — total 33 bytes, forces 3 layers of nesting.
  const std::string prefix(24, 'L');
  constexpr size_t kCount = 64;
  auto raws = rusty::Vec<std::string>::with_capacity(kCount);
  for (size_t i = 0; i < kCount; ++i) {
    std::string k = prefix;
    for (int j = 0; j < 9; ++j) {
      k.push_back(static_cast<char>((i * 7 + j) & 0xFF));
    }
    raws.push(std::move(k));
    ASSERT_TRUE(tree.insert(vk(raws.back()), bank.Make(i)));
  }
  ASSERT_EQ(tree.size(), kCount);

  for (size_t i = 0; i < kCount; ++i) {
    ASSERT_TRUE(tree.remove(vk(raws[i]))) << "i=" << i;
  }
  EXPECT_EQ(tree.size(), 0u);

  for (size_t i = 0; i < kCount; ++i) {
    TestTree::value_type out = nullptr;
    EXPECT_FALSE(tree.search(vk(raws[i]), out)) << "stale i=" << i;
  }
  // LSan checks at process exit that the layer nodes were freed.
}

// -----------------------------------------------------------------------------
// 3. MixedLengthKeysInOneLeaf — keys of wildly different lengths
// sharing the same first slice. They live in (or descend from) the
// same leaf cluster; the leaf's keylenx packing has to accommodate
// every length without truncating or aliasing.
// -----------------------------------------------------------------------------
TEST(MasstreeLayered, MixedLengthKeysInOneLeaf) {
  TestTree tree;
  StorageBank bank;
  // Common 8-byte first slice (one full slice). Suffixes of varied
  // length encode the test value.
  const std::string head = "ABCDEFGH";  // 8 bytes
  struct Spec { size_t suffix_len; char fill; uint64_t val; };
  auto specs = rusty::Vec<Spec>::with_capacity(7);
  specs.push({0,    '?', 1});     // exactly 8 bytes — no suffix, sits at root
  specs.push({1,    'a', 2});     // 9 bytes — second slice partial
  specs.push({7,    'b', 3});     // 15 bytes — second slice partial
  specs.push({8,    'c', 4});     // 16 bytes — full second slice
  specs.push({50,   'd', 5});     // multi-slice
  specs.push({200,  'e', 6});     // forces layered descent
  specs.push({1000, 'f', 7});     // near MASSTREE_MAX_KEY_LEN
  auto raws = rusty::Vec<std::string>::with_capacity(specs.size());
  for (const auto& s : specs) {
    std::string k = head + std::string(s.suffix_len, s.fill);
    raws.push(std::move(k));
    ASSERT_TRUE(tree.insert(vk(raws.back()), bank.Make(s.val)))
        << "suffix_len=" << s.suffix_len;
  }
  ASSERT_EQ(tree.size(), specs.size());

  // Verify every spec round-trips.
  for (size_t i = 0; i < specs.size(); ++i) {
    TestTree::value_type out = nullptr;
    ASSERT_TRUE(tree.search(vk(raws[i]), out))
        << "suffix_len=" << specs[i].suffix_len;
    EXPECT_EQ(StorageBank::Decode(out), specs[i].val);
  }

  // Forward scan over the cluster must return keys in lex order. By
  // construction the suffixes' fill bytes increase with suffix
  // length, so lex order matches insertion order (suffix '?' (0x3F)
  // for len 0, 'a'..'f' for the rest).
  class Cb : public TestTree::search_range_callback {
   public:
    rusty::Vec<std::string> keys;
    bool invoke(const TestTree::string_type& k, TestTree::value_type) override {
      keys.push(std::string(k.data(), k.length()));
      return true;
    }
  };
  Cb cb;
  const std::string lo_str = head;  // start at "ABCDEFGH"
  varkey lo = vk(lo_str);
  tree.search_range_call(lo, nullptr, cb);
  ASSERT_EQ(cb.keys.len(), specs.len());
  EXPECT_TRUE(std::is_sorted(cb.keys.begin(), cb.keys.end()));
}

// -----------------------------------------------------------------------------
// 4. SliceBoundaryFuzz — for every length L in 1..32, insert a key
// of length L filled with deterministic bytes. The cross-product hits
// "just under 8B", "exactly 8B", "8B + 1B", "exactly 16B", etc., in
// the same tree. Inserts/removes happen in a permuted order so we
// don't accidentally always end at the right edge.
// -----------------------------------------------------------------------------
TEST(MasstreeLayered, SliceBoundaryFuzz) {
  TestTree tree;
  StorageBank bank;

  auto keys = rusty::Vec<std::string>::with_capacity(32);
  for (int len = 1; len <= 32; ++len) {
    // Deterministic content tied to length so lookups by length
    // resolve unambiguously.
    std::string k;
    for (int j = 0; j < len; ++j) {
      k.push_back(static_cast<char>('A' + ((len + j) % 26)));
    }
    keys.push(std::move(k));
  }

  // Build a 0..N-1 permutation and shuffle it deterministically.
  auto order = rusty::Vec<size_t>::with_capacity(keys.size());
  for (size_t i = 0; i < keys.size(); ++i) order.push(i);
  std::mt19937 rng(0xBADC0FFEEull);
  std::shuffle(order.begin(), order.end(), rng);

  for (size_t i : order) {
    ASSERT_TRUE(tree.insert(vk(keys[i]), bank.Make(i)))
        << "len=" << keys[i].size();
  }
  ASSERT_EQ(tree.size(), keys.size());

  for (size_t i = 0; i < keys.size(); ++i) {
    TestTree::value_type out = nullptr;
    ASSERT_TRUE(tree.search(vk(keys[i]), out)) << "len=" << keys[i].size();
    EXPECT_EQ(StorageBank::Decode(out), i);
  }

  // Remove half (the odd-indexed keys), verify the other half stays.
  for (size_t i = 0; i < keys.size(); ++i) {
    if (i % 2 == 1) {
      ASSERT_TRUE(tree.remove(vk(keys[i]))) << "len=" << keys[i].size();
    }
  }
  EXPECT_EQ(tree.size(), 16u);
  for (size_t i = 0; i < keys.size(); ++i) {
    TestTree::value_type out = nullptr;
    const bool found = tree.search(vk(keys[i]), out);
    EXPECT_EQ(found, (i % 2 == 0)) << "i=" << i << " len=" << keys[i].size();
  }

  // Reinsert the removed keys; the tree must accept them and
  // re-establish the original size.
  for (size_t i = 0; i < keys.size(); ++i) {
    if (i % 2 == 1) {
      ASSERT_TRUE(tree.insert(vk(keys[i]), bank.Make(i)));
    }
  }
  EXPECT_EQ(tree.size(), keys.size());
}
