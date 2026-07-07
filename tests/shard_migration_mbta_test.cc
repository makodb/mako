// Spike / gating test: the online data-migration DATA PLANE on the REAL
// mako storage engine (mbta_ordered_index / Silo), NOT the in-memory
// BTreeMap stub. It proves the range primitives that ShardManager's stub
// Shard provides -- copy_range_from, drop_range, range_count, checksum --
// reduce to scan + point ops on a real FullOrderedIndex, exactly as the
// storage recon concluded. This is the foundation for OrderedIndexShardData
// (Stage 2): swapping the stub Shard for masstree-backed storage under the
// same 2PC migration protocol.
//
// Bring-up mirrors tests/test_kv_backends.cc / test_silo_nontxn_api.cc:
// every thread touching MassTrans needs static_init + thread_init.

#include <stdlib.h>

#include "benchmarks/bench.h"
#include "storage/mbta_wrapper.hh"
#include "ordered_index_shard_data.h"   // the real shard-data class under test

#include <gtest/gtest.h>

import std;

namespace {

using mbta_type = mbta_table;

std::atomic<int> g_tid_counter{0};
std::atomic<long> g_table_id{7300};

void engine_thread_init() {
    static thread_local bool done = false;
    if (done) return;
    done = true;
    static std::once_flag once;
    std::call_once(once, [] { mbta_type::static_init(); });
    TThread::set_id(g_tid_counter.fetch_add(1));
    TThread::set_mode(0);
    TThread::readset_shard_bits = 0;
    TThread::writeset_shard_bits = 0;
    TThread::transget_without_throw = false;
    TThread::transget_without_stable = false;
    mbta_type::thread_init();
}

// Fresh mbta (Silo) table, leaked like the other Silo unit tests (MassTrans
// teardown wants an RCU quiescence a unit test can't provide; tiny tables).
::FullOrderedIndex* fresh_index(const std::string& name) {
    return mbta_index_build(name, g_table_id.fetch_add(1));
}

// ---------------------------------------------------------------------------
// Range primitives = the future OrderedIndexShardData methods, over the
// non-txn OrderedIndex surface (raw bytes; scan covers [start, *end)).
// ---------------------------------------------------------------------------

class Collect : public oi_scan_callback {
public:
    bool invoke(const char* keyp, size_t keylen, const std::string& value) override {
        pairs.emplace_back(std::string(keyp, keylen), value);
        return true;  // never stop early
    }
    std::vector<std::pair<std::string, std::string>> pairs;
};

std::vector<std::pair<std::string, std::string>>
range_scan(OrderedIndex* t, const std::string& lo, const std::string& hi) {
    Collect cb;
    t->scan(lo, &hi, cb, nullptr);
    return std::move(cb.pairs);
}

// Background bulk copy: scan the source range, put each pair into dest.
void copy_range(OrderedIndex* src, OrderedIndex* dst,
                const std::string& lo, const std::string& hi) {
    for (auto& kv : range_scan(src, lo, hi)) {
        dst->put(lcdf::Str(kv.first.data(), kv.first.size()), kv.second);
    }
}

size_t range_count(OrderedIndex* t, const std::string& lo, const std::string& hi) {
    return range_scan(t, lo, hi).size();
}

uint64_t fnv(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

// Order-independent u64 fold over live key->value pairs in [lo, hi). Deletes
// are hard-removes on the real engine, so a removed key simply drops out of
// the scan -- both sides exclude it and the checksums still match.
uint64_t range_checksum(OrderedIndex* t, const std::string& lo, const std::string& hi) {
    uint64_t sum = 0;
    for (auto& kv : range_scan(t, lo, hi)) {
        sum += fnv(kv.first) * 1000003ull + fnv(kv.second);
    }
    return sum;
}

// Post-commit shed: remove every key in the range from the source.
void drop_range(OrderedIndex* t, const std::string& lo, const std::string& hi) {
    for (auto& kv : range_scan(t, lo, hi)) {
        t->remove(lcdf::Str(kv.first.data(), kv.first.size()));
    }
}

class ShardMigrationMbta : public ::testing::Test {
protected:
    void SetUp() override { engine_thread_init(); }
    // Zero-padded so string order == numeric order: k00..k29.
    static std::string k(int i) {
        std::string s = std::to_string(i);
        return "k" + std::string(s.size() < 2 ? 2 - s.size() : 0, '0') + s;
    }
    static void seed30(OrderedIndex* t) {
        for (int i = 0; i < 30; i++)
            t->put(lcdf::Str(k(i)), "v" + std::to_string(i));
    }
};

// 1. Background copy reproduces the range EXACTLY (count + checksum), and
//    copies ONLY the range -- keys outside [lo,hi) do not move.
TEST_F(ShardMigrationMbta, CopyRangeReproducesRangeExactly) {
    OrderedIndex* src = fresh_index("mig_src_a");
    OrderedIndex* dst = fresh_index("mig_dst_a");
    seed30(src);

    const std::string lo = k(10), hi = k(20);   // [k10, k20) = 10 keys
    copy_range(src, dst, lo, hi);

    EXPECT_EQ(range_count(dst, lo, hi), 10u);
    EXPECT_EQ(range_checksum(src, lo, hi), range_checksum(dst, lo, hi));
    // Nothing outside the migrated range leaked to dst.
    EXPECT_EQ(range_count(dst, k(0), k(10)), 0u);
    EXPECT_EQ(range_count(dst, k(20), k(30)), 0u);
}

// 2. Writes-during-copy (the delta) -- overwrite, insert, AND delete --
//    replay to dst so no write is lost; final checksums match. Delete is a
//    hard-remove carried as an explicit delta fact (the mig_deleted model),
//    since the engine keeps no tombstone.
TEST_F(ShardMigrationMbta, DeltaReplayNoLostWrites) {
    OrderedIndex* src = fresh_index("mig_src_b");
    OrderedIndex* dst = fresh_index("mig_dst_b");
    seed30(src);

    const std::string lo = k(10), hi = k(20);
    copy_range(src, dst, lo, hi);   // snapshot at this point

    // Writes land on the range AFTER the snapshot (captured as the delta):
    src->put(lcdf::Str(k(15)), "v15-updated");   // overwrite
    src->put(lcdf::Str("k18b"), "inserted");     // new key, k18 < k18b < k19
    src->remove(lcdf::Str(k(12)));               // delete in range

    // final_sync: replay the delta to dst (puts then deletes).
    dst->put(lcdf::Str(k(15)), "v15-updated");
    dst->put(lcdf::Str("k18b"), "inserted");
    dst->remove(lcdf::Str(k(12)));

    EXPECT_EQ(range_checksum(src, lo, hi), range_checksum(dst, lo, hi))
        << "delta (overwrite + insert + delete) must transmit -- no lost write";
    EXPECT_EQ(range_count(src, lo, hi), 10u);   // 10 - 1 delete + 1 insert
}

// 3. drop_range sheds ONLY the migrated range from the source; keys below
//    and above the range are untouched; dst keeps the range.
TEST_F(ShardMigrationMbta, DropRangeShedsOnlyTheRange) {
    OrderedIndex* src = fresh_index("mig_src_c");
    OrderedIndex* dst = fresh_index("mig_dst_c");
    seed30(src);

    const std::string lo = k(10), hi = k(20);
    copy_range(src, dst, lo, hi);
    drop_range(src, lo, hi);   // COMMIT: the source drops the migrated range

    EXPECT_EQ(range_count(src, lo, hi), 0u);          // range gone from src
    EXPECT_EQ(range_count(src, k(0), k(10)), 10u);    // below untouched
    EXPECT_EQ(range_count(src, k(20), k(30)), 10u);   // above untouched
    EXPECT_EQ(range_count(dst, lo, hi), 10u);         // dst now serves it
}

// 4. The checksum genuinely gates the cutover: a divergent copy is caught,
//    so the 2PC prepare would vote no and the master would abort.
TEST_F(ShardMigrationMbta, ChecksumDetectsDivergence) {
    OrderedIndex* src = fresh_index("mig_src_d");
    OrderedIndex* dst = fresh_index("mig_dst_d");
    seed30(src);

    const std::string lo = k(10), hi = k(20);
    copy_range(src, dst, lo, hi);
    EXPECT_EQ(range_checksum(src, lo, hi), range_checksum(dst, lo, hi));

    dst->remove(lcdf::Str(k(14)));   // a dropped row in transfer
    EXPECT_NE(range_checksum(src, lo, hi), range_checksum(dst, lo, hi))
        << "checksum must catch a divergent copy so the cutover aborts";
}

// ---------------------------------------------------------------------------
// The FULL 2PC migration protocol, phase by phase, over OrderedIndexShardData
// (the real mbta-backed shard-data class) -- the stub ShardManager's protocol
// (docs/mako-book.md §3) run against real storage, single-process.
// ---------------------------------------------------------------------------

// Migrate [k10,k20) src->dst ONLINE: the source serves through the copy, a
// delta (overwrite + insert + delete) is captured, the range is frozen, the
// delta is synced, checksums match, and the cutover drops the range on src.
TEST_F(ShardMigrationMbta, FullProtocolCommitPath) {
    janus::OrderedIndexShardData src(fresh_index("fm_src"));
    janus::OrderedIndexShardData dst(fresh_index("fm_dst"));
    for (int i = 0; i < 30; i++) src.put(k(i), "v" + std::to_string(i));
    const std::string lo = k(10), hi = k(20);   // [k10,k20) = 10 keys

    // PHASE 1 -- BACKGROUND COPY (source live): snapshot bulk copy.
    dst.copy_range_from(src, lo, hi);

    // Writes land on the range AFTER the snapshot -> captured as the delta at
    // the coordinator; the source keeps serving the whole time.
    std::vector<janus::OrderedIndexShardData::KvPair> staged_puts;
    std::vector<std::string> staged_dels;
    src.put(k(15), "hot");   staged_puts.push_back({k(15), "hot"});    // overwrite
    src.put("k18b", "new");  staged_puts.push_back({"k18b", "new"});   // in-range insert
    src.remove(k(13));       staged_dels.push_back(k(13));             // delete

    // PHASE 2 -- LOCK: freeze [lo,hi) on the source (a control-plane gate; here
    // the point is simply that no new writes join the delta after this).

    // PHASE 3 -- FINAL SYNC + VERIFY: replay the delta to dst, then checksum.
    for (const auto& p : staged_puts) dst.put(p.first, p.second);
    for (const auto& d : staged_dels) dst.remove(d);
    ASSERT_EQ(src.checksum(lo, hi), dst.checksum(lo, hi))
        << "prepare must see byte-identical ranges before cutover";

    // PHASE 4 -- COMMIT: routing flips to dst; the source drops the range.
    src.drop_range(lo, hi);

    EXPECT_EQ(src.range_count(lo, hi), 0u);          // source shed the range
    EXPECT_EQ(dst.range_count(lo, hi), 10u);         // 10 - 1 delete + 1 insert
    EXPECT_EQ(src.range_count(k(0), k(10)), 10u);    // neighbors untouched
    EXPECT_EQ(src.range_count(k(20), k(30)), 10u);
    std::string got;                                 // dst holds the post-delta data
    ASSERT_TRUE(dst.get(k(15), got)); EXPECT_EQ(got, "hot");
    EXPECT_FALSE(dst.get(k(13), got));               // deleted key really gone
}

// A garbled transfer diverges the copy; the checksum gate fails, so the master
// ABORTS -- and because the source never dropped the range, it resumes intact.
TEST_F(ShardMigrationMbta, FullProtocolAbortLeavesSourceIntact) {
    janus::OrderedIndexShardData src(fresh_index("fa_src"));
    janus::OrderedIndexShardData dst(fresh_index("fa_dst"));
    for (int i = 0; i < 30; i++) src.put(k(i), "v" + std::to_string(i));
    const std::string lo = k(10), hi = k(20);

    dst.copy_range_from(src, lo, hi);
    dst.remove(k(14));   // a dropped row in transfer -> divergent copy

    // PREPARE fails the checksum-equality gate -> ABORT (no cutover happens).
    ASSERT_NE(src.checksum(lo, hi), dst.checksum(lo, hi));

    // ABORT: the source never ran drop_range, so it still serves the full
    // range; the destination's partial copy is simply discarded.
    EXPECT_EQ(src.range_count(lo, hi), 10u);
    std::string got;
    ASSERT_TRUE(src.get(k(14), got)); EXPECT_EQ(got, "v14");
}

}  // namespace
