// Tests for the masstree-rocks-cache capability
// (openspec/specs/masstree-rocks-cache/spec.md).
//
// Every test names the requirement it covers. The cache's correctness
// arguments live in docs/masstree-rocks-cache.md; this file is what
// turns them from arguments into evidence.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "mako/storage/masstree_rocks_index.hh"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

// Collects a range into a vector so scans can be asserted on directly.
class collecting_callback : public oi_scan_callback {
 public:
  explicit collecting_callback(size_t stop_after = 0)
      : stop_after_(stop_after) {}

  bool invoke(const char *keyp, size_t keylen,
              const std::string &value) override {
    keys.push_back(std::string(keyp, keylen));
    values.push_back(value);
    if (stop_after_ != 0 && keys.size() >= stop_after_) return false;
    return true;
  }

  std::vector<std::string> keys;
  std::vector<std::string> values;

 private:
  size_t stop_after_;
};

class MasstreeRocksCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/mrx_test_XXXXXX";
    char *d = mkdtemp(tmpl);
    ASSERT_NE(d, nullptr);
    db_path_ = std::string(d);
    // mkdtemp made the directory; rocksdb wants to create it itself.
    rmdir(db_path_.c_str());
    Open();
  }

  void TearDown() override {
    Close();
    std::string cmd = "rm -rf " + db_path_;
    (void)system(cmd.c_str());
  }

  // Open (or reopen) a store over the same RocksDB directory.
  void Open(uint64_t capacity = 0) {
    tree_ = new concurrent_btree();
    store_ = mrx_store_open(tree_, db_path_, capacity);
    ASSERT_NE(store_, nullptr);
    idx_ = new masstree_rocks_index("mrx_test", 1, tree_, store_);
  }

  void ReopenWithCapacity(uint64_t capacity) {
    Close();
    Open(capacity);
  }

  // Is this key's value held in memory? Observable because the test
  // includes the header, which is also how eviction gets asserted at
  // all - residency is invisible through OrderedIndex by design.
  bool IsResident(const std::string &key) {
    const auto region = mrx_rcu_region();
    mrx_entry *e = mrx_lookup(tree_, S(key));
    if (e == nullptr) return false;
    mrx_val *v = e->val.load(std::memory_order_acquire);
    return v->resident != 0 && v->tombstone == 0;
  }

  // Poll until the value tier fits, or give up. Eviction is
  // asynchronous, so there is nothing to synchronize on.
  bool WaitForResidentBytesAtMost(uint64_t limit, int timeout_ms = 10000) {
    for (int waited = 0; waited < timeout_ms; waited += 10) {
      if (idx_->resident_bytes() <= limit) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return idx_->resident_bytes() <= limit;
  }

  void Close() {
    if (idx_ != nullptr) {
      delete idx_;
      idx_ = nullptr;
    }
    if (store_ != nullptr) {
      mrx_store_close(store_);
      store_ = nullptr;
    }
    if (tree_ != nullptr) {
      delete tree_;
      tree_ = nullptr;
    }
  }

  // Close and reopen, which is how the startup key load gets exercised.
  void Reopen() {
    Close();
    Open();
  }

  static lcdf::Str S(const std::string &s) {
    return lcdf::Str(s.data(), static_cast<int>(s.size()));
  }

  bool Get(const std::string &key, std::string &out) {
    return idx_->get(S(key), out, std::string::npos);
  }

  bool Put(const std::string &key, const std::string &value) {
    return idx_->put(S(key), value);
  }

  bool Insert(const std::string &key, const std::string &value) {
    return idx_->insert(S(key), value);
  }

  bool Remove(const std::string &key) { return idx_->remove(S(key)); }

  // Read straight from the system of record, bypassing the cache, to
  // prove a write actually landed rather than merely being visible.
  bool RawDbGet(const std::string &key, std::string &out) {
    const auto region = mrx_rcu_region();
    return mrx_db_get(store_, S(key), out, std::string::npos);
  }

  std::string db_path_;
  concurrent_btree *tree_{nullptr};
  mrx_store *store_{nullptr};
  masstree_rocks_index *idx_{nullptr};
};

// ---------------------------------------------------------------------------
// Requirement: Write-Back Acknowledgement
// Requirement: Durability Barrier
// ---------------------------------------------------------------------------

TEST_F(MasstreeRocksCacheTest, WriteIsVisibleImmediately) {
  EXPECT_TRUE(Put("alpha", "one"));
  std::string got;
  ASSERT_TRUE(Get("alpha", got));
  EXPECT_EQ(got, "one");
}

TEST_F(MasstreeRocksCacheTest, FlushMakesWritesDurable) {
  for (int i = 0; i < 64; i++) {
    ASSERT_TRUE(Put("k" + std::to_string(i), "v" + std::to_string(i)));
  }
  EXPECT_TRUE(idx_->flush());

  for (int i = 0; i < 64; i++) {
    std::string raw;
    ASSERT_TRUE(RawDbGet("k" + std::to_string(i), raw))
        << "key k" << i << " never reached RocksDB";
    EXPECT_EQ(raw, "v" + std::to_string(i));
  }
}

TEST_F(MasstreeRocksCacheTest, FlushOnIdleStoreSucceeds) {
  EXPECT_TRUE(idx_->flush());
}

TEST_F(MasstreeRocksCacheTest, OverwriteFlushesLatestValue) {
  ASSERT_TRUE(Put("k", "first"));
  ASSERT_FALSE(Put("k", "second"));  // not newly inserted
  ASSERT_TRUE(idx_->flush());

  std::string raw;
  ASSERT_TRUE(RawDbGet("k", raw));
  EXPECT_EQ(raw, "second");
}

// ---------------------------------------------------------------------------
// Requirement: Existence-Reporting Writes
// ---------------------------------------------------------------------------

TEST_F(MasstreeRocksCacheTest, PutReportsNewlyInserted) {
  EXPECT_TRUE(Put("fresh", "a"));
  EXPECT_FALSE(Put("fresh", "b"));
}

TEST_F(MasstreeRocksCacheTest, InsertRefusesLiveKey) {
  EXPECT_TRUE(Insert("k", "a"));
  EXPECT_FALSE(Insert("k", "b"));

  std::string got;
  ASSERT_TRUE(Get("k", got));
  EXPECT_EQ(got, "a") << "refused insert must not overwrite";
}

TEST_F(MasstreeRocksCacheTest, RemoveReportsExistence) {
  EXPECT_FALSE(Remove("never-existed"));
  ASSERT_TRUE(Put("k", "v"));
  EXPECT_TRUE(Remove("k"));
  EXPECT_FALSE(Remove("k")) << "second remove must report absent";
}

TEST_F(MasstreeRocksCacheTest, ConcurrentInsertsElectOneWinner) {
  const int kThreads = 8;
  std::atomic<int> winners{0};
  std::vector<std::thread> ts;
  for (int i = 0; i < kThreads; i++) {
    ts.emplace_back([&, i]() {
      if (idx_->insert(S("contested"), "v" + std::to_string(i))) {
        winners.fetch_add(1);
      }
    });
  }
  for (auto &t : ts) t.join();
  EXPECT_EQ(winners.load(), 1) << "put-if-absent must be atomic";
}

// ---------------------------------------------------------------------------
// Requirement: Deletion By Tombstone
// ---------------------------------------------------------------------------

TEST_F(MasstreeRocksCacheTest, ReadAfterDeleteIsNotFound) {
  ASSERT_TRUE(Put("k", "v"));
  ASSERT_TRUE(idx_->flush());  // the row is genuinely in RocksDB now
  ASSERT_TRUE(Remove("k"));

  std::string got;
  EXPECT_FALSE(Get("k", got))
      << "delete must not fall through to the stale RocksDB row";
}

TEST_F(MasstreeRocksCacheTest, DeleteReachesTheSystemOfRecord) {
  ASSERT_TRUE(Put("k", "v"));
  ASSERT_TRUE(idx_->flush());
  ASSERT_TRUE(Remove("k"));
  ASSERT_TRUE(idx_->flush());

  std::string raw;
  EXPECT_FALSE(RawDbGet("k", raw));
}

TEST_F(MasstreeRocksCacheTest, ReinsertAfterDeleteSucceeds) {
  ASSERT_TRUE(Put("k", "v"));
  ASSERT_TRUE(Remove("k"));
  EXPECT_TRUE(Insert("k", "again"));

  std::string got;
  ASSERT_TRUE(Get("k", got));
  EXPECT_EQ(got, "again");
}

TEST_F(MasstreeRocksCacheTest, DeletedKeySurvivesReopenAsAbsent) {
  ASSERT_TRUE(Put("k", "v"));
  ASSERT_TRUE(Remove("k"));
  ASSERT_TRUE(idx_->flush());
  Reopen();

  std::string got;
  EXPECT_FALSE(Get("k", got));
}

// ---------------------------------------------------------------------------
// Requirement: Authoritative Key Residency
// ---------------------------------------------------------------------------

TEST_F(MasstreeRocksCacheTest, MissingKeyIsAbsent) {
  std::string got;
  EXPECT_FALSE(Get("nothing-here", got));
}

TEST_F(MasstreeRocksCacheTest, ReopenLoadsEveryKey) {
  const int kKeys = 200;
  for (int i = 0; i < kKeys; i++) {
    ASSERT_TRUE(Put("key" + std::to_string(i), "val" + std::to_string(i)));
  }
  ASSERT_TRUE(idx_->flush());
  Reopen();

  for (int i = 0; i < kKeys; i++) {
    std::string got;
    ASSERT_TRUE(Get("key" + std::to_string(i), got))
        << "key" << i << " missing after reopen";
    EXPECT_EQ(got, "val" + std::to_string(i));
  }
}

TEST_F(MasstreeRocksCacheTest, ReopenedValuesStartNonResident) {
  for (int i = 0; i < 50; i++) {
    ASSERT_TRUE(Put("k" + std::to_string(i), std::string(512, 'x')));
  }
  ASSERT_TRUE(idx_->flush());
  const uint64_t hot = idx_->resident_bytes();
  Reopen();
  const uint64_t cold = idx_->resident_bytes();

  EXPECT_LT(cold, hot)
      << "a reopened store must not have loaded value bytes into memory";
}

TEST_F(MasstreeRocksCacheTest, ReadThroughFillServesReopenedValue) {
  ASSERT_TRUE(Put("k", "persisted-value"));
  ASSERT_TRUE(idx_->flush());
  Reopen();

  std::string got;
  ASSERT_TRUE(Get("k", got)) << "fill from the system of record failed";
  EXPECT_EQ(got, "persisted-value");

  // Second read is served from memory and must agree.
  std::string again;
  ASSERT_TRUE(Get("k", again));
  EXPECT_EQ(again, "persisted-value");
}

// ---------------------------------------------------------------------------
// Requirement: Ordered Range Iteration
// ---------------------------------------------------------------------------

TEST_F(MasstreeRocksCacheTest, ScanReturnsAscendingRange) {
  for (int i = 0; i < 10; i++) {
    ASSERT_TRUE(Put("k" + std::to_string(i), "v" + std::to_string(i)));
  }
  collecting_callback cb;
  std::string start = "k0";
  std::string end = "k5";
  idx_->scan(start, &end, cb, nullptr);

  ASSERT_EQ(cb.keys.size(), 5u);
  for (size_t i = 0; i < cb.keys.size(); i++) {
    EXPECT_EQ(cb.keys[i], "k" + std::to_string(i));
    EXPECT_EQ(cb.values[i], "v" + std::to_string(i));
  }
}

TEST_F(MasstreeRocksCacheTest, ScanOmitsDeletedKeys) {
  for (int i = 0; i < 6; i++) {
    ASSERT_TRUE(Put("k" + std::to_string(i), "v"));
  }
  ASSERT_TRUE(Remove("k3"));

  collecting_callback cb;
  std::string start = "k0";
  idx_->scan(start, nullptr, cb, nullptr);

  for (const auto &k : cb.keys) {
    EXPECT_NE(k, "k3") << "a tombstoned key must not be emitted";
  }
  EXPECT_EQ(cb.keys.size(), 5u);
}

TEST_F(MasstreeRocksCacheTest, ScanStopsWhenCallbackReturnsFalse) {
  for (int i = 0; i < 20; i++) {
    ASSERT_TRUE(Put("k" + std::to_string(i), "v"));
  }
  collecting_callback cb(3);
  std::string start = "k";
  idx_->scan(start, nullptr, cb, nullptr);
  EXPECT_EQ(cb.keys.size(), 3u);
}

TEST_F(MasstreeRocksCacheTest, ScanFillsNonResidentValues) {
  for (int i = 0; i < 20; i++) {
    ASSERT_TRUE(Put("k" + std::to_string(i), "value" + std::to_string(i)));
  }
  ASSERT_TRUE(idx_->flush());
  Reopen();  // every value is now non-resident

  collecting_callback cb;
  std::string start = "k";
  idx_->scan(start, nullptr, cb, nullptr);

  ASSERT_EQ(cb.keys.size(), 20u);
  for (size_t i = 0; i < cb.keys.size(); i++) {
    const std::string suffix = cb.keys[i].substr(1);
    EXPECT_EQ(cb.values[i], "value" + suffix);
  }
}

TEST_F(MasstreeRocksCacheTest, ScanSpansMultipleChunks) {
  // More keys than MRX_SCAN_CHUNK, so the chunked walk has to resume
  // correctly rather than stopping at the first boundary.
  const int kKeys = 1500;
  for (int i = 0; i < kKeys; i++) {
    char buf[16];
    snprintf(buf, sizeof(buf), "k%05d", i);
    ASSERT_TRUE(Put(buf, "v"));
  }
  collecting_callback cb;
  std::string start = "k";
  idx_->scan(start, nullptr, cb, nullptr);

  ASSERT_EQ(cb.keys.size(), static_cast<size_t>(kKeys));
  for (size_t i = 1; i < cb.keys.size(); i++) {
    EXPECT_LT(cb.keys[i - 1], cb.keys[i]) << "chunked scan lost ordering";
  }
}

TEST_F(MasstreeRocksCacheTest, RscanReturnsDescendingRange) {
  for (int i = 0; i < 10; i++) {
    ASSERT_TRUE(Put("k" + std::to_string(i), "v" + std::to_string(i)));
  }
  collecting_callback cb;
  std::string start = "k9";
  std::string end = "k5";
  idx_->rscan(start, &end, cb, nullptr);

  ASSERT_FALSE(cb.keys.empty());
  for (size_t i = 1; i < cb.keys.size(); i++) {
    EXPECT_GT(cb.keys[i - 1], cb.keys[i]) << "rscan must descend";
  }
  for (const auto &k : cb.keys) {
    EXPECT_GT(k, "k5") << "lower bound is exclusive";
  }
}

// ---------------------------------------------------------------------------
// Requirement: Truncation
// ---------------------------------------------------------------------------

TEST_F(MasstreeRocksCacheTest, ClearTruncatesBothTiers) {
  for (int i = 0; i < 20; i++) {
    ASSERT_TRUE(Put("k" + std::to_string(i), "v"));
  }
  ASSERT_TRUE(idx_->flush());
  idx_->clear();

  std::string got;
  EXPECT_FALSE(Get("k1", got));

  std::string raw;
  EXPECT_FALSE(RawDbGet("k1", raw))
      << "clear must truncate the system of record too";
}

// ---------------------------------------------------------------------------
// Requirement: Version-Exact Durability Marking
//
// Hammer overwrites while the flusher runs, then assert the final value
// is what the last writer wrote - both in memory and on disk. A flusher
// that credited durability to the wrong version would let a newer value
// be dropped.
// ---------------------------------------------------------------------------

TEST_F(MasstreeRocksCacheTest, OverwriteStormEndsConsistent) {
  const int kRounds = 500;
  for (int i = 0; i < kRounds; i++) {
    ASSERT_TRUE(Put("hot", "v" + std::to_string(i)) || true);
  }
  const std::string expected = "v" + std::to_string(kRounds - 1);

  std::string got;
  ASSERT_TRUE(Get("hot", got));
  EXPECT_EQ(got, expected);

  ASSERT_TRUE(idx_->flush());
  std::string raw;
  ASSERT_TRUE(RawDbGet("hot", raw));
  EXPECT_EQ(raw, expected) << "flusher persisted a stale version";
}

TEST_F(MasstreeRocksCacheTest, ConcurrentWritersAndReadersStayConsistent) {
  const int kWriters = 4;
  const int kKeys = 50;
  const int kRounds = 100;

  std::atomic<bool> corrupt{false};
  std::vector<std::thread> ts;

  for (int w = 0; w < kWriters; w++) {
    ts.emplace_back([&]() {
      for (int r = 0; r < kRounds; r++) {
        for (int k = 0; k < kKeys; k++) {
          const std::string key = "k" + std::to_string(k);
          idx_->put(S(key), "value-" + std::to_string(k));
        }
      }
    });
  }
  // Readers: every value for key k must be exactly "value-k" - any
  // other content means a torn or misattributed value.
  for (int rd = 0; rd < 2; rd++) {
    ts.emplace_back([&]() {
      for (int r = 0; r < kRounds; r++) {
        for (int k = 0; k < kKeys; k++) {
          const std::string key = "k" + std::to_string(k);
          std::string got;
          if (idx_->get(S(key), got, std::string::npos)) {
            if (got != "value-" + std::to_string(k)) corrupt.store(true);
          }
        }
      }
    });
  }
  for (auto &t : ts) t.join();

  EXPECT_FALSE(corrupt.load()) << "observed a value that was never written";
  EXPECT_TRUE(idx_->flush());

  for (int k = 0; k < kKeys; k++) {
    std::string raw;
    ASSERT_TRUE(RawDbGet("k" + std::to_string(k), raw));
    EXPECT_EQ(raw, "value-" + std::to_string(k));
  }
}

// ---------------------------------------------------------------------------
// Requirement: Bounded Value Memory
// Requirement: Durable-Only Eviction
// Requirement: Recency-Biased Reclamation
// ---------------------------------------------------------------------------

TEST_F(MasstreeRocksCacheTest, NoCapacityMeansNoEviction) {
  const uint64_t kCount = 200;
  const std::string value(1024, 'v');
  for (uint64_t i = 0; i < kCount; i++) {
    ASSERT_TRUE(Put("k" + std::to_string(i), value));
  }
  ASSERT_TRUE(idx_->flush());
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  for (uint64_t i = 0; i < kCount; i++) {
    EXPECT_TRUE(IsResident("k" + std::to_string(i)))
        << "unbounded store must never evict";
  }
}

TEST_F(MasstreeRocksCacheTest, EvictionBringsValueBytesUnderCapacity) {
  const uint64_t kCapacity = 64 * 1024;
  ReopenWithCapacity(kCapacity);

  const std::string value(1024, 'x');
  const uint64_t kCount = 400;  // ~400KB of values against a 64KB cap
  for (uint64_t i = 0; i < kCount; i++) {
    ASSERT_TRUE(Put("k" + std::to_string(i), value));
  }
  ASSERT_TRUE(idx_->flush());

  EXPECT_TRUE(WaitForResidentBytesAtMost(kCapacity))
      << "resident bytes stayed at " << idx_->resident_bytes()
      << " against a capacity of " << kCapacity;

  // Meeting the capacity is not by itself proof that reclamation
  // happened - assert directly that values left memory.
  int evicted = 0;
  for (uint64_t i = 0; i < kCount; i++) {
    if (!IsResident("k" + std::to_string(i))) evicted++;
  }
  EXPECT_GT(evicted, 0) << "capacity was satisfied without evicting anything";
}

TEST_F(MasstreeRocksCacheTest, EvictedValuesReadBackUnchanged) {
  const uint64_t kCapacity = 32 * 1024;
  ReopenWithCapacity(kCapacity);

  // Values must be big enough that they dominate resident_bytes, which
  // also counts per-key entry overhead - otherwise the capacity is
  // never exceeded and this test would silently exercise nothing.
  const uint64_t kCount = 300;
  auto value_for = [](uint64_t i) {
    return "value-" + std::to_string(i) + std::string(512, 'z');
  };
  for (uint64_t i = 0; i < kCount; i++) {
    ASSERT_TRUE(Put("k" + std::to_string(i), value_for(i)));
  }
  ASSERT_TRUE(idx_->flush());
  ASSERT_TRUE(WaitForResidentBytesAtMost(kCapacity))
      << "nothing was reclaimed, so the read-back below proves nothing";

  // Every key must still be readable and correct, whether its value is
  // resident or had to be fetched back from the system of record.
  for (uint64_t i = 0; i < kCount; i++) {
    std::string got;
    ASSERT_TRUE(Get("k" + std::to_string(i), got)) << "key " << i << " lost";
    EXPECT_EQ(got, value_for(i));
  }
}

// No acknowledged write may be dropped to satisfy the capacity.
//
// LIMITATION: this cannot force the exact interleaving it cares about.
// The flusher runs continuously, so values become durable on their own
// and the test cannot pin one in the non-durable state without a hook
// to pause the flusher. What it does prove is the observable
// consequence - under sustained pressure with a capacity far below the
// working set, nothing is ever lost.
TEST_F(MasstreeRocksCacheTest, PressureNeverLosesAnAcknowledgedWrite) {
  const uint64_t kCapacity = 16 * 1024;
  ReopenWithCapacity(kCapacity);

  const uint64_t kCount = 500;
  const std::string value(256, 'p');
  for (uint64_t i = 0; i < kCount; i++) {
    ASSERT_TRUE(Put("key" + std::to_string(i), value + std::to_string(i)));
  }
  ASSERT_TRUE(idx_->flush());

  for (uint64_t i = 0; i < kCount; i++) {
    std::string got;
    ASSERT_TRUE(Get("key" + std::to_string(i), got))
        << "eviction discarded key " << i;
    EXPECT_EQ(got, value + std::to_string(i));
  }
}

// Direct unit test of the eviction guard. Driving this through the
// store would be racy - the flusher marks values durable on its own
// schedule - so the entries here are built by hand and never published,
// which makes each guard deterministic.
TEST_F(MasstreeRocksCacheTest, EvictValueRefusesIneligibleValues) {
  const auto region = mrx_rcu_region();

  // Non-durable: memory holds the only copy of this write.
  mrx_val *dirty = mrx_val_new(1, /*tombstone=*/false, /*resident=*/true,
                               "payload", /*durable=*/false);
  mrx_entry *e_dirty = mrx_entry_alloc(dirty);
  EXPECT_FALSE(mrx_evict_value(store_, e_dirty))
      << "evicting a non-durable value would discard the only copy";

  // Durable and resident: the one evictable case.
  mrx_val *clean = mrx_val_new(2, /*tombstone=*/false, /*resident=*/true,
                               "payload", /*durable=*/true);
  mrx_entry *e_clean = mrx_entry_alloc(clean);
  EXPECT_TRUE(mrx_evict_value(store_, e_clean));

  // Already evicted: nothing left to reclaim.
  EXPECT_FALSE(mrx_evict_value(store_, e_clean))
      << "a non-resident value has nothing to evict";

  // Tombstone: carries no bytes.
  mrx_val *tomb = mrx_val_new(3, /*tombstone=*/true, /*resident=*/true, "",
                              /*durable=*/true);
  mrx_entry *e_tomb = mrx_entry_alloc(tomb);
  EXPECT_FALSE(mrx_evict_value(store_, e_tomb));
}

TEST_F(MasstreeRocksCacheTest, HotKeySurvivesReclamation) {
  const uint64_t kCapacity = 32 * 1024;
  ReopenWithCapacity(kCapacity);

  const std::string value(512, 'h');
  const uint64_t kCount = 400;
  ASSERT_TRUE(Put("hot", value));
  for (uint64_t i = 0; i < kCount; i++) {
    ASSERT_TRUE(Put("cold" + std::to_string(i), value));
  }
  ASSERT_TRUE(idx_->flush());

  // Keep touching the hot key while the sweeper runs, so its reference
  // bit is set every time the clock hand passes.
  std::atomic<bool> stop{false};
  std::thread toucher([&]() {
    std::string got;
    while (!stop.load()) {
      idx_->get(S("hot"), got, std::string::npos);
    }
  });
  WaitForResidentBytesAtMost(kCapacity, 5000);
  // One last read so the assertion is not racing a sweep that just
  // cleared the bit.
  std::string got;
  ASSERT_TRUE(Get("hot", got));
  const bool hot_resident = IsResident("hot");
  stop.store(true);
  toucher.join();

  EXPECT_TRUE(hot_resident)
      << "a continuously read value should survive the clock hand";
  EXPECT_EQ(got, value);
}

TEST_F(MasstreeRocksCacheTest, WritesAndSweeperConcurrently) {
  const uint64_t kCapacity = 16 * 1024;
  ReopenWithCapacity(kCapacity);

  const int kKeys = 100;
  const int kRounds = 200;
  std::atomic<bool> corrupt{false};
  std::vector<std::thread> ts;

  for (int w = 0; w < 3; w++) {
    ts.emplace_back([&]() {
      for (int r = 0; r < kRounds; r++) {
        for (int k = 0; k < kKeys; k++) {
          idx_->put(S("k" + std::to_string(k)), "value-" + std::to_string(k));
        }
      }
    });
  }
  for (int rd = 0; rd < 2; rd++) {
    ts.emplace_back([&]() {
      for (int r = 0; r < kRounds; r++) {
        for (int k = 0; k < kKeys; k++) {
          std::string got;
          if (idx_->get(S("k" + std::to_string(k)), got, std::string::npos)) {
            if (got != "value-" + std::to_string(k)) corrupt.store(true);
          }
        }
      }
    });
  }
  for (auto &t : ts) t.join();

  EXPECT_FALSE(corrupt.load())
      << "eviction/fill/write interleaving produced a value never written";

  ASSERT_TRUE(idx_->flush());
  for (int k = 0; k < kKeys; k++) {
    std::string got;
    ASSERT_TRUE(Get("k" + std::to_string(k), got));
    EXPECT_EQ(got, "value-" + std::to_string(k));
  }
}

}  // namespace
