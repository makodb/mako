// Tests for the mtx_* C ABI (src/mako/storage/mtree_abi.h).
//
// The load-bearing one is WordsRoundTripBitIdentical. The whole Rust port
// rests on masstree treating the stored word as opaque, and that is a
// property of a third-party data structure we do not control. If a vendor
// bump ever starts stealing tag bits from the value slot, or interpreting
// it, this test is what says so -- everything else would just corrupt
// quietly.
//
// (masstree does *touch* the word: masstree.hh sets prefetch = true and
// leafvalue::prefetch() issues __builtin_prefetch on it during lookups and
// scans. That is non-faulting, so an arbitrary word is architecturally
// safe -- but "safe today" is exactly the kind of assumption that deserves
// a tripwire.)

#include <stdint.h>
#include <string.h>

#include <gtest/gtest.h>

#include "mako/storage/mtree_abi.h"

#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

class MtreeAbiTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(mtx_thread_attach(), MTX_OK);
    t_ = mtx_create();
    ASSERT_NE(t_, nullptr);
  }
  void TearDown() override { mtx_destroy(t_); }

  int Put(const std::string &k, uint64_t w) {
    uint64_t got = 0;
    const int rc = mtx_get_or_insert(t_, k.data(), k.size(), w, &got);
    return rc;
  }
  uint64_t Get(const std::string &k) {
    uint64_t got = 0;
    EXPECT_EQ(mtx_get(t_, k.data(), k.size(), &got), MTX_OK);
    return got;
  }

  mtx_tree *t_{nullptr};
};

TEST_F(MtreeAbiTest, AbiIdentityMatchesTheHeader) {
  EXPECT_EQ(mtx_abi_version(), MTX_ABI_VERSION);
  EXPECT_EQ(mtx_kv_size(), sizeof(mtx_kv));
}

TEST_F(MtreeAbiTest, MissingKeyReportsNullWordNotAnError) {
  uint64_t w = 12345;
  EXPECT_EQ(mtx_get(t_, "nope", 4, &w), MTX_OK);
  EXPECT_EQ(w, MTX_WORD_NULL) << "absence is reported in the out-value";
}

// THE TRIPWIRE.
TEST_F(MtreeAbiTest, WordsRoundTripBitIdentical) {
  const uint64_t words[] = {
      1ull,
      2ull,
      0xFFFFFFFFFFFFFFFFull,          // all ones
      0x8000000000000000ull,          // top bit only
      0x7FFFFFFFFFFFFFFFull,          // all but top bit
      0x0000000000000001ull,          // low bit only - tag-bit canary
      0x0000000000000003ull,          // low two bits - tag-bit canary
      0xDEADBEEFDEADBEEFull,          // unaligned-looking
      0x0000000000001000ull,          // page-ish
      0xFFFFFFFF00000000ull,          // high half only
      0x00000000FFFFFFFFull,          // low half only
      0xAAAAAAAAAAAAAAAAull,          // alternating
      0x5555555555555555ull,          // alternating, inverted
  };
  const size_t n = sizeof(words) / sizeof(words[0]);

  for (size_t i = 0; i < n; i++) {
    const std::string k = "w" + std::to_string(i);
    uint64_t out = 0;
    ASSERT_EQ(mtx_get_or_insert(t_, k.data(), k.size(), words[i], &out),
              MTX_OK);
    EXPECT_EQ(out, words[i]) << "get_or_insert returned a different word";
  }
  for (size_t i = 0; i < n; i++) {
    const std::string k = "w" + std::to_string(i);
    EXPECT_EQ(Get(k), words[i])
        << "word " << i << " did not survive storage bit-identical; masstree "
           "may have started interpreting the value slot";
  }

  // And through a scan, which is a different code path (and the one that
  // prefetches the word on every visited key).
  std::vector<mtx_kv> kvs(n + 4);
  std::vector<char> arena(4096);
  size_t got_n = 0, used = 0;
  ASSERT_EQ(mtx_scan_chunk(t_, "", 0, kvs.data(), kvs.size(), arena.data(),
                           arena.size(), &got_n, &used),
            MTX_OK);
  ASSERT_EQ(got_n, n);
  for (size_t i = 0; i < got_n; i++) {
    const std::string key(arena.data() + kvs[i].key_off, kvs[i].key_len);
    const size_t idx = static_cast<size_t>(std::stoul(key.substr(1)));
    ASSERT_LT(idx, n);
    EXPECT_EQ(kvs[i].word, words[idx]) << "scan returned a mangled word";
  }
}

TEST_F(MtreeAbiTest, ReservedNullWordIsRejected) {
  uint64_t out = 0;
  EXPECT_EQ(mtx_get_or_insert(t_, "k", 1, MTX_WORD_NULL, &out),
            MTX_ERR_INVALID)
      << "MTX_WORD_NULL must stay reserved, or absence becomes ambiguous";
}

TEST_F(MtreeAbiTest, GetOrInsertReportsTheWinner) {
  uint64_t out = 0;
  ASSERT_EQ(mtx_get_or_insert(t_, "k", 1, 111, &out), MTX_OK);
  EXPECT_EQ(out, 111u);
  // Second call must NOT overwrite, and must report the incumbent.
  ASSERT_EQ(mtx_get_or_insert(t_, "k", 1, 222, &out), MTX_OK);
  EXPECT_EQ(out, 111u) << "the directory is immutable once written";
  EXPECT_EQ(Get("k"), 111u);
}

TEST_F(MtreeAbiTest, ConcurrentGetOrInsertElectsExactlyOneWinner) {
  const int kThreads = 8;
  std::vector<uint64_t> reported(kThreads, 0);
  std::vector<std::thread> ts;
  for (int i = 0; i < kThreads; i++) {
    ts.emplace_back([&, i]() {
      ASSERT_EQ(mtx_thread_attach(), MTX_OK);
      uint64_t out = 0;
      ASSERT_EQ(mtx_get_or_insert(t_, "contested", 9,
                                  static_cast<uint64_t>(i + 1), &out),
                MTX_OK);
      reported[i] = out;
    });
  }
  for (auto &t : ts) t.join();

  const uint64_t winner = Get("contested");
  EXPECT_NE(winner, MTX_WORD_NULL);
  for (int i = 0; i < kThreads; i++) {
    EXPECT_EQ(reported[i], winner)
        << "thread " << i << " was told a different word than is stored";
  }
}

TEST_F(MtreeAbiTest, ScanIsAscendingAndResumable) {
  for (int i = 0; i < 50; i++) {
    char k[16];
    snprintf(k, sizeof(k), "k%03d", i);
    ASSERT_EQ(Put(k, static_cast<uint64_t>(i + 1)), MTX_OK);
  }

  // Deliberately tiny chunks, so resumption is exercised rather than
  // assumed.
  std::vector<std::string> seen;
  std::string cursor;
  for (;;) {
    mtx_kv kvs[7];
    char arena[256];
    size_t n = 0, used = 0;
    ASSERT_EQ(mtx_scan_chunk(t_, cursor.data(), cursor.size(), kvs, 7, arena,
                             sizeof(arena), &n, &used),
              MTX_OK);
    if (n == 0) break;
    for (size_t i = 0; i < n; i++) {
      seen.push_back(std::string(arena + kvs[i].key_off, kvs[i].key_len));
    }
    cursor = seen.back();
    cursor.push_back('\0');  // strictly after the last key
    if (n < 7) break;
  }

  ASSERT_EQ(seen.size(), 50u);
  for (size_t i = 1; i < seen.size(); i++) {
    EXPECT_LT(seen[i - 1], seen[i]) << "scan lost ordering across chunks";
  }
}

TEST_F(MtreeAbiTest, ScanStopsCleanlyWhenTheArenaIsTooSmall) {
  for (int i = 0; i < 20; i++) {
    char k[32];
    snprintf(k, sizeof(k), "longishkey%08d", i);
    ASSERT_EQ(Put(k, static_cast<uint64_t>(i + 1)), MTX_OK);
  }
  mtx_kv kvs[20];
  char arena[40];  // room for only a couple of keys
  size_t n = 0, used = 0;
  EXPECT_EQ(mtx_scan_chunk(t_, "", 0, kvs, 20, arena, sizeof(arena), &n, &used),
            MTX_OK);
  EXPECT_GT(n, 0u) << "a too-small arena must still make progress";
  EXPECT_LT(n, 20u) << "it should have stopped early, not overrun";
  // The arena-full signal: `used` exceeds the capacity and states what one
  // more key would have needed. Reporting bytes-consumed here instead makes
  // this outcome indistinguishable from end-of-range.
  EXPECT_GT(used, sizeof(arena))
      << "an arena-limited stop must be distinguishable from end-of-range";
}

TEST_F(MtreeAbiTest, ArenaSignalLetsACallerRecoverEveryKey) {
  // The contract as a caller actually uses it. Long keys against a small
  // arena is the case that silently truncated the Rust adapter's scan --
  // 980 of 1000 keys lost -- because a partly-filled arena reads exactly
  // like end-of-range without this signal.
  const std::string prefix(400, 'x');
  const int kCount = 200;
  for (int i = 0; i < kCount; i++) {
    char suffix[16];
    snprintf(suffix, sizeof(suffix), "%04d", i);
    ASSERT_EQ(Put((prefix + suffix).c_str(), static_cast<uint64_t>(i + 1)),
              MTX_OK);
  }

  std::set<std::string> seen;
  std::string from;
  std::vector<char> arena(256);  // deliberately far too small
  std::vector<mtx_kv> kvs(16);
  bool first = true;
  for (int guard = 0; guard < 10000; guard++) {
    size_t n = 0, used = 0;
    const int st = mtx_scan_chunk(t_, from.data(), from.size(), kvs.data(),
                                  kvs.size(), arena.data(), arena.size(), &n,
                                  &used);
    if (st == MTX_ERR_NO_SPACE) {
      ASSERT_GT(used, arena.size());
      arena.resize(used);
      continue;
    }
    ASSERT_EQ(st, MTX_OK);
    if (used > arena.size()) {  // arena-limited: grow and re-walk
      arena.resize(used);
      continue;
    }
    if (n == 0) break;
    for (size_t i = first ? 0 : 1; i < n; i++) {
      seen.insert(std::string(arena.data() + kvs[i].key_off, kvs[i].key_len));
    }
    const std::string last(arena.data() + kvs[n - 1].key_off,
                           kvs[n - 1].key_len);
    first = false;
    if (n < kvs.size() || last == from) break;
    from = last;
  }
  EXPECT_EQ(seen.size(), static_cast<size_t>(kCount))
      << "a caller honouring the arena signal must still see every key";
}

TEST_F(MtreeAbiTest, RscanDescends) {
  for (int i = 0; i < 10; i++) {
    char k[16];
    snprintf(k, sizeof(k), "k%02d", i);
    ASSERT_EQ(Put(k, static_cast<uint64_t>(i + 1)), MTX_OK);
  }
  mtx_kv kvs[10];
  char arena[256];
  size_t n = 0, used = 0;
  ASSERT_EQ(mtx_rscan_chunk(t_, "k99", 3, kvs, 10, arena, sizeof(arena), &n,
                            &used),
            MTX_OK);
  ASSERT_GT(n, 1u);
  for (size_t i = 1; i < n; i++) {
    const std::string a(arena + kvs[i - 1].key_off, kvs[i - 1].key_len);
    const std::string b(arena + kvs[i].key_off, kvs[i].key_len);
    EXPECT_GT(a, b) << "rscan must descend";
  }
}

TEST_F(MtreeAbiTest, NullAndBadArgumentsAreRejectedNotCrashed) {
  uint64_t out = 0;
  EXPECT_EQ(mtx_get(nullptr, "k", 1, &out), MTX_ERR_INVALID);
  EXPECT_EQ(mtx_get(t_, nullptr, 1, &out), MTX_ERR_INVALID);
  EXPECT_EQ(mtx_get(t_, "k", 1, nullptr), MTX_ERR_INVALID);
  size_t n = 0, used = 0;
  EXPECT_EQ(mtx_scan_chunk(t_, "", 0, nullptr, 1, nullptr, 0, &n, &used),
            MTX_ERR_INVALID);
  mtx_destroy(nullptr);  // must be a no-op, not a crash
}

TEST_F(MtreeAbiTest, EmptyKeyIsAValidKey) {
  uint64_t out = 0;
  ASSERT_EQ(mtx_get_or_insert(t_, "", 0, 7, &out), MTX_OK);
  EXPECT_EQ(out, 7u);
  EXPECT_EQ(Get(""), 7u);
}

}  // namespace
