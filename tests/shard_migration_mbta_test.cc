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
#include "ordered_index_shard_data.h"   // the real shard-data class under test (import cluster: ShardMaster)

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

// A ShardMaster over an isolated in-memory config store, driving two real
// mbta-backed participants (shard 0 = src, shard 1 = dst). This is the ONE
// migration coordinator the whole cluster uses -- here over real storage. Held
// in place (the master borrows cm_/cfg_), so it is non-movable; construct it as
// a local in each test.
struct MasterRig {
    janus::InMemoryKvStore kv_;
    janus::ConfigManager cm_{&kv_};
    janus::ClusterConfig cfg_ = janus::ClusterConfig::new_();
    janus::ShardMaster master = janus::ShardMaster::new_(&cm_, &cfg_);
    MasterRig(janus::ShardData* s0, janus::ShardData* s1) {
        master.register_shard({"s0"}, s0);   // id 0
        master.register_shard({"s1"}, s1);   // id 1
    }
    MasterRig(const MasterRig&) = delete;
    MasterRig& operator=(const MasterRig&) = delete;
};

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
    dst.copy_range_from(&src, lo, hi);

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

    dst.copy_range_from(&src, lo, hi);
    dst.remove(k(14));   // a dropped row in transfer -> divergent copy

    // PREPARE fails the checksum-equality gate -> ABORT (no cutover happens).
    ASSERT_NE(src.checksum(lo, hi), dst.checksum(lo, hi));

    // ABORT: the source never ran drop_range, so it still serves the full
    // range; the destination's partial copy is simply discarded.
    EXPECT_EQ(src.range_count(lo, hi), 10u);
    std::string got;
    ASSERT_TRUE(src.get(k(14), got)); EXPECT_EQ(got, "v14");
}

// ---------------------------------------------------------------------------
// The same protocol driven through the ShardMigrator COORDINATOR (the real
// counterpart to ShardManager's migration methods) instead of inline steps --
// this is the reusable coordinator the RPC (Stage 3) and runtime (Stage 4)
// paths will drive; the participants just become RPC proxies.
// ---------------------------------------------------------------------------

TEST_F(ShardMigrationMbta, MigratorCommitPath) {
    janus::OrderedIndexShardData src(fresh_index("mg_src"));
    janus::OrderedIndexShardData dst(fresh_index("mg_dst"));
    for (int i = 0; i < 30; i++) src.put(k(i), "v" + std::to_string(i));

    MasterRig rig(&src, &dst);
    auto& m = rig.master;
    ASSERT_TRUE(m.begin_migration(/*source=*/0, /*dest=*/1, /*table=*/"", k(10), k(20)));
    m.background_copy();                       // Phase 1 (source live)
    m.client_put(k(15), "hot");                // delta: overwrite
    m.client_remove(k(13));                    // delta: delete
    EXPECT_FALSE(m.shard_frozen_for(0, k(15))); // not locked yet -> not frozen
    m.lock_range();                            // Phase 2
    EXPECT_TRUE(m.shard_frozen_for(0, k(15)));  // in-range key now frozen
    EXPECT_FALSE(m.shard_frozen_for(0, k(25))); // out-of-range key never frozen
    m.final_sync();                            // Phase 3: delta replay + checksum
    ASSERT_TRUE(m.both_prepared());
    ASSERT_TRUE(m.commit_migration());         // Phase 4

    EXPECT_EQ(src.range_count(k(10), k(20)), 0u);   // source shed the range
    EXPECT_EQ(dst.range_count(k(10), k(20)), 9u);   // 10 - 1 delete
    std::string got;
    ASSERT_TRUE(dst.get(k(15), got)); EXPECT_EQ(got, "hot");
    EXPECT_FALSE(dst.get(k(13), got));
}

TEST_F(ShardMigrationMbta, MigratorAbortOnDivergenceLeavesSourceIntact) {
    janus::OrderedIndexShardData src(fresh_index("ma_src"));
    janus::OrderedIndexShardData dst(fresh_index("ma_dst"));
    for (int i = 0; i < 30; i++) src.put(k(i), "v" + std::to_string(i));

    MasterRig rig(&src, &dst);
    auto& m = rig.master;
    ASSERT_TRUE(m.begin_migration(0, 1, "", k(10), k(20)));
    m.background_copy();
    dst.remove(k(14));                         // garbled transfer -> divergence
    m.lock_range();
    m.final_sync();
    EXPECT_FALSE(m.both_prepared());            // checksum gate fails -> no cutover
    m.abort_migration();

    EXPECT_EQ(src.range_count(k(10), k(20)), 10u);  // source keeps the full range
    EXPECT_EQ(dst.range_count(k(10), k(20)), 0u);   // dest partial copy discarded
    std::string got;
    ASSERT_TRUE(src.get(k(14), got)); EXPECT_EQ(got, "v14");
}

// ---------------------------------------------------------------------------
// END TO END: a multi-shard WORKLOAD that dynamically re-shards by MIGRATING a
// key-range between two real mbta shards mid-run, then routes to the new owner
// -- dynamic sharding INCLUDING migration, on the real engine, single-process.
// The lex range map is the routing "assignment" a migration updates -- the
// plain-C++ stand-in for the runtime's eventual ClusterConfig lex-range routing.
// ---------------------------------------------------------------------------

// A contiguous lex-range -> owner assignment ([lo,hi); hi=="" means +inf).
struct RangeMap {
    struct R { std::string lo, hi; int owner; };
    std::vector<R> ranges;
    int route(const std::string& key) const {
        for (const auto& r : ranges)
            if (key >= r.lo && (r.hi.empty() || key < r.hi)) return r.owner;
        return -1;
    }
    // Reassign [lo,hi) (assumed fully within one existing range) to new_owner,
    // splitting that range -- the routing effect of a committed migration.
    void reassign(const std::string& lo, const std::string& hi, int new_owner) {
        std::vector<R> out;
        for (const auto& r : ranges) {
            const bool contains = lo >= r.lo && (r.hi.empty() || hi <= r.hi);
            if (contains) {
                if (lo > r.lo)       out.push_back({r.lo, lo, r.owner});
                out.push_back({lo, hi, new_owner});
                if (r.hi.empty() || hi < r.hi) out.push_back({hi, r.hi, r.owner});
            } else {
                out.push_back(r);
            }
        }
        ranges = std::move(out);
    }
};

TEST_F(ShardMigrationMbta, EndToEndDynamicReshardWithLiveMigration) {
    janus::OrderedIndexShardData shard0(fresh_index("e2e_s0"));
    janus::OrderedIndexShardData shard1(fresh_index("e2e_s1"));
    janus::OrderedIndexShardData* shards[2] = {&shard0, &shard1};

    // Initial assignment: shard0 owns [,k50), shard1 owns [k50,).
    RangeMap rmap;
    rmap.ranges = {{"", k(50), 0}, {k(50), "", 1}};

    // The workload routes every op through the current assignment.
    auto W = [&](const std::string& key, const std::string& v) {
        shards[rmap.route(key)]->put(key, v);
    };
    auto Rd = [&](const std::string& key) {
        std::string out; shards[rmap.route(key)]->get(key, out); return out;
    };

    // Phase 1 -- steady state: keys land on their owner shard.
    for (int i = 0; i < 50; i++) W(k(i), "v" + std::to_string(i));   // -> shard0
    for (int i = 50; i < 70; i++) W(k(i), "v" + std::to_string(i));  // -> shard1
    ASSERT_EQ(Rd(k(15)), "v15");   // served by shard0
    ASSERT_EQ(Rd(k(60)), "v60");   // served by shard1

    // Phase 2 -- RE-SHARD: migrate [k10,k20) from shard0 to shard1, ONLINE.
    MasterRig rig(&shard0, &shard1);
    auto& m = rig.master;
    ASSERT_TRUE(m.begin_migration(0, 1, "", k(10), k(20)));
    m.background_copy();                     // source keeps serving during copy
    // Writes to the migrating range during the copy are captured by the source
    // (dual-write into the delta) so none are lost across the cutover:
    m.client_put(k(15), "v15-hot");          // a hot overwrite
    m.client_remove(k(13));                  // a delete
    m.lock_range();                          // freeze the range
    m.final_sync();                          // replay delta + checksum-equality gate
    ASSERT_TRUE(m.both_prepared());
    ASSERT_TRUE(m.commit_migration());       // source drops the range
    rmap.reassign(k(10), k(20), 1);          // cutover: routing flips to shard1

    // Phase 3 -- after cutover: the migrated range is served by shard1, with the
    // live writes applied, and NO write was lost; everything else is unchanged.
    EXPECT_EQ(rmap.route(k(15)), 1);
    EXPECT_EQ(Rd(k(15)), "v15-hot");         // hot write survived + rerouted
    EXPECT_EQ(Rd(k(18)), "v18");             // migrated normally
    EXPECT_EQ(Rd(k(13)), "");                // deleted key is gone
    EXPECT_EQ(rmap.route(k(5)),  0); EXPECT_EQ(Rd(k(5)),  "v5");   // below range: shard0
    EXPECT_EQ(rmap.route(k(25)), 0); EXPECT_EQ(Rd(k(25)), "v25");  // above range: shard0
    EXPECT_EQ(rmap.route(k(60)), 1); EXPECT_EQ(Rd(k(60)), "v60");  // shard1 untouched
    // Data physically moved: shard0 shed the range, shard1 holds it.
    EXPECT_EQ(shard0.range_count(k(10), k(20)), 0u);
    EXPECT_EQ(shard1.range_count(k(10), k(20)), 9u);   // 10 - 1 delete
}

}  // namespace
