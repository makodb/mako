// Non-transactional STRESS test: raw non-txn throughput on the real mbta engine,
// and an online range migration running UNDER heavy concurrent load. Everything
// stays on the non-txn OrderedIndex surface -- exactly what OrderedIndexShardData
// and ShardMigrator use. The migrating range is disjoint from the keys the
// workers hammer, so the copy/checksum/drop is correct while the engine is under
// load; this measures (a) how fast the non-txn path goes and (b) migration's
// throughput impact + correctness under stress.
//
// Bring-up mirrors tests/test_kv_backends.cc (mbta static_init + thread_init);
// every worker thread gets a distinct, valid mbta id (< MAX_THREADS=460).

#include <stdlib.h>

#include "benchmarks/bench.h"
#include "storage/mbta_wrapper.hh"
#include "ordered_index_shard_data.h"
#include "shard_migrator.h"

#include <gtest/gtest.h>

import std;

namespace {

using mbta_type = mbta_table;

std::atomic<int>  g_tid{0};
std::atomic<long> g_table_id{7700};

// Per-thread Silo/mbta bring-up (distinct id per thread).
void thread_init_once() {
    static thread_local bool done = false;
    if (done) return;
    done = true;
    static std::once_flag once;
    std::call_once(once, [] { mbta_type::static_init(); });
    TThread::set_id(g_tid.fetch_add(1));   // distinct + valid (< 460)
    TThread::set_mode(0);
    TThread::readset_shard_bits = 0;
    TThread::writeset_shard_bits = 0;
    TThread::transget_without_throw = false;
    TThread::transget_without_stable = false;
    mbta_type::thread_init();
}

::FullOrderedIndex* fresh_index(const std::string& name) {
    return mbta_index_build(name, g_table_id.fetch_add(1));
}

constexpr int KEYSPACE   = 100000;
constexpr int RANGE_LO   = 50000;
constexpr int RANGE_HI   = 60000;   // migrate [50000,60000): 10k keys
constexpr int NUM_WORKERS = 8;
constexpr int OPS_PER_WORKER = 300000;

std::vector<std::string> g_keys;    // precomputed so the loop measures the engine

void build_keys() {
    if (!g_keys.empty()) return;
    g_keys.reserve(KEYSPACE);
    for (int i = 0; i < KEYSPACE; i++) {
        char b[16]; snprintf(b, sizeof b, "s%07d", i);
        g_keys.emplace_back(b);
    }
}

class Stress : public ::testing::Test {
protected:
    void SetUp() override { thread_init_once(); build_keys(); }
};

// A worker hammering random non-txn get/put; if avoid_range, it stays out of
// [RANGE_LO,RANGE_HI). Returns the op count.
long hammer(janus::OrderedIndexShardData* shard, int seed, bool avoid_range,
            std::atomic<bool>* go) {
    thread_init_once();
    if (go) while (!go->load(std::memory_order_acquire)) { /* spin to sync start */ }
    uint64_t rng = 0x9e3779b97f4a7c15ull * (uint64_t)(seed + 1);
    std::string out;
    long n = 0;
    for (int i = 0; i < OPS_PER_WORKER; i++) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        int k = (int)(rng % KEYSPACE);
        if (avoid_range && k >= RANGE_LO && k < RANGE_HI) k = (k + RANGE_HI) % KEYSPACE;
        if (i & 1) shard->get(g_keys[k], out);
        else       shard->put(g_keys[k], "v2");
        n++;
    }
    return n;
}

// (1) How fast is the raw non-txn path under concurrency?
TEST_F(Stress, NonTxnThroughputBaseline) {
    janus::OrderedIndexShardData shard(fresh_index("stress_base"));
    for (int i = 0; i < KEYSPACE; i++) shard.put(g_keys[i], "v");

    std::atomic<long> total{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> ws;
    for (int t = 0; t < NUM_WORKERS; t++)
        ws.emplace_back([&, t] { total += hammer(&shard, t, /*avoid_range=*/false, &go); });

    auto t0 = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& w : ws) w.join();
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    printf("[STRESS] non-txn BASELINE: %.0f ops/sec  (%ld ops, %d threads, %.3fs)\n",
           total.load() / sec, total.load(), NUM_WORKERS, sec);
    fflush(stdout);
    EXPECT_GT(total.load(), 0);
}

// (2) Online migration UNDER that load: workers hammer keys outside the range
// while a migration moves [RANGE_LO,RANGE_HI) to a second shard. Measures the
// throughput with a migration in flight and verifies the migration is correct.
TEST_F(Stress, MigrationUnderLoad) {
    janus::OrderedIndexShardData src(fresh_index("stress_src"));
    janus::OrderedIndexShardData dst(fresh_index("stress_dst"));
    for (int i = 0; i < KEYSPACE; i++) src.put(g_keys[i], "v");

    const std::string lo = g_keys[RANGE_LO], hi = g_keys[RANGE_HI];

    std::atomic<long> total{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> ws;
    for (int t = 0; t < NUM_WORKERS; t++)
        ws.emplace_back([&, t] { total += hammer(&src, t + 100, /*avoid_range=*/true, &go); });

    bool migrated_ok = false;
    auto t0 = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    std::thread mig([&] {
        thread_init_once();
        janus::ShardMigrator m(&src, &dst, lo, hi, /*generation=*/1);
        m.background_copy();                 // copy the range while the workers pound the engine
        m.lock();
        migrated_ok = m.final_sync_and_verify();
        if (migrated_ok) m.commit();
        else             m.abort();
    });
    mig.join();
    for (auto& w : ws) w.join();
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    printf("[STRESS] under MIGRATION: %.0f ops/sec  (%ld ops, %d threads, %.3fs); "
           "migrated %d keys [%d,%d)\n",
           total.load() / sec, total.load(), NUM_WORKERS, sec,
           RANGE_HI - RANGE_LO, RANGE_LO, RANGE_HI);
    fflush(stdout);

    EXPECT_TRUE(migrated_ok);
    EXPECT_EQ(src.range_count(lo, hi), 0u);                              // source shed the range
    EXPECT_EQ(dst.range_count(lo, hi), (size_t)(RANGE_HI - RANGE_LO));   // dest holds it
    std::string out;
    EXPECT_TRUE(src.get(g_keys[100], out));    // a workload key outside the range survived
    EXPECT_TRUE(src.get(g_keys[90000], out));
}

}  // namespace
