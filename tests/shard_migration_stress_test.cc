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

// A migration-aware write path: captures concurrent writes to the migrating
// range (dual-write into the delta) and freezes the range at LOCK, so an online
// migration loses NO write even when the workload writes the range under it.
// Thread-safe via one mutex; only range writes contend (workload writes outside
// the range still go straight to the source).
class MigratingSource {
public:
    MigratingSource(janus::OrderedIndexShardData* src, janus::OrderedIndexShardData* dst)
        : src_(src), dst_(dst) {}

    // Workload write. Returns false if the key is frozen (LOCK) -> caller drops it.
    bool put(const std::string& k, const std::string& v) {
        std::lock_guard<std::mutex> g(mu_);
        if (active_ && k >= lo_ && k < hi_) {
            if (locked_) return false;          // frozen during LOCK
            src_->put(k, v);
            staged_[k] = v;                     // capture into the delta
            return true;
        }
        src_->put(k, v);                        // outside the migrating range
        return true;
    }

    // ---- coordinator side (single migration thread) ----
    void begin(std::string lo, std::string hi) {
        std::lock_guard<std::mutex> g(mu_);
        lo_ = std::move(lo); hi_ = std::move(hi);
        active_ = true; locked_ = false; staged_.clear();
    }
    void copy() { dst_->copy_range_from(*src_, lo_, hi_); }   // scans src concurrently
    // Chunked copy: scan the range in small windows so a concurrent-write OCC
    // conflict only re-scans one chunk (not the whole range) -> the cursor
    // always advances -> guaranteed progress under UNBOUNDED range writes.
    long copy_chunked(size_t chunk) {
        std::string cur = lo_;
        long scans = 0;
        while (cur < hi_) {
            auto batch = src_->scan_range_limited(cur, hi_, chunk);
            ++scans;
            if (batch.empty()) break;
            for (const auto& kv : batch) dst_->put(kv.first, kv.second);
            cur = batch.back().first; cur.push_back('\0');   // start strictly after last key
        }
        return scans;
    }
    void lock() { std::lock_guard<std::mutex> g(mu_); locked_ = true; }
    bool final_sync_and_verify() {
        std::lock_guard<std::mutex> g(mu_);
        for (const auto& kv : staged_) dst_->put(kv.first, kv.second);   // replay the delta
        return src_->checksum(lo_, hi_) == dst_->checksum(lo_, hi_);
    }
    void commit() {
        std::lock_guard<std::mutex> g(mu_);
        src_->drop_range(lo_, hi_); active_ = false;
    }

private:
    janus::OrderedIndexShardData* src_;
    janus::OrderedIndexShardData* dst_;
    std::mutex mu_;
    bool active_ = false, locked_ = false;
    std::string lo_, hi_;
    std::map<std::string, std::string> staged_;
};

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

// (3) The HARD case: the workload overwrites keys IN the migrating range while
// the migration runs. Proves the online migration captures those writes (no
// lost/stale write) via the dual-write delta + freeze. Each key is written by a
// single worker (key-partitioned), so "last accepted write" is well-defined.
TEST_F(Stress, MigrationCapturesConcurrentRangeWrites) {
    janus::OrderedIndexShardData src(fresh_index("cap_src"));
    janus::OrderedIndexShardData dst(fresh_index("cap_dst"));
    const std::string lo = g_keys[RANGE_LO], hi = g_keys[RANGE_HI];
    for (int i = RANGE_LO; i < RANGE_HI; i++) src.put(g_keys[i], "base");

    MigratingSource ms(&src, &dst);
    ms.begin(lo, hi);   // enable capture BEFORE the copy starts

    // Each worker overwrites its OWN partition of the range (keys where
    // idx % NUM_WORKERS == t), so the last write per key is unambiguous. Bounded
    // to one pass -- the non-txn OCC scan the copy uses retries while the range
    // is written, so an unbounded write loop starves the copy (measured ~25s);
    // one pass lets the copy converge once the writers finish.
    std::vector<std::map<std::string, std::string>> expect(NUM_WORKERS);
    std::vector<std::thread> ws;
    for (int t = 0; t < NUM_WORKERS; t++) {
        ws.emplace_back([&, t] {
            thread_init_once();
            for (int idx = RANGE_LO + t; idx < RANGE_HI; idx += NUM_WORKERS) {
                std::string val = "w" + std::to_string(t) + "_" + std::to_string(idx);
                if (ms.put(g_keys[idx], val)) expect[t][g_keys[idx]] = val;  // accepted -> remember
            }
        });
    }

    // The copy runs WHILE the workers write the range (concurrent stress + capture).
    ms.copy();
    for (auto& w : ws) w.join();   // workers finish their (bounded) range writes
    ms.lock();                     // freeze the range
    const bool ok = ms.final_sync_and_verify();  // replay the delta + checksum gate
    ASSERT_TRUE(ok) << "post-sync checksums must match (delta replayed correctly)";
    ms.commit();

    // No lost or stale writes: every accepted write's value is on the destination.
    long checked = 0;
    for (int t = 0; t < NUM_WORKERS; t++) {
        for (const auto& kv : expect[t]) {
            std::string out;
            ASSERT_TRUE(dst.get(kv.first, out)) << "lost write: " << kv.first;
            ASSERT_EQ(out, kv.second) << "stale write: " << kv.first;
            checked++;
        }
    }
    printf("[STRESS] concurrent range-write capture: %ld accepted writes, all present + latest on dst\n",
           checked);
    fflush(stdout);
    EXPECT_GT(checked, 0);
    EXPECT_EQ(src.range_count(lo, hi), 0u);   // source shed the range after commit
}

// (4) SUSTAINED: ping-pong a range between the two shards CONTINUOUSLY for a few
// seconds while the workers hammer disjoint keys -- a migration is ALWAYS in
// flight, so this measures the true steady-state throughput cost of continuous
// migration (vs the ~7M/s baseline), and correctness across hundreds of cutovers.
TEST_F(Stress, SustainedMigrationPingPong) {
    janus::OrderedIndexShardData shard0(fresh_index("pp_s0"));
    janus::OrderedIndexShardData shard1(fresh_index("pp_s1"));
    janus::OrderedIndexShardData* shards[2] = {&shard0, &shard1};
    for (int i = 0; i < KEYSPACE; i++) shard0.put(g_keys[i], "v");

    const int PP_LO = 50000, PP_HI = 52000;         // 2k-key range, frequent migrations
    const std::string lo = g_keys[PP_LO], hi = g_keys[PP_HI];

    std::atomic<int>  owner{0};                       // shard currently holding the range
    std::atomic<bool> stop{false};
    std::atomic<long> total_ops{0}, migrations{0};

    // Workers hammer keys OUTSIDE the ping-pong range (on shard0).
    std::vector<std::thread> ws;
    for (int t = 0; t < NUM_WORKERS; t++) {
        ws.emplace_back([&, t] {
            thread_init_once();
            uint64_t rng = 0xABCDEFull * (uint64_t)(t + 1);
            std::string out; long n = 0;
            while (!stop.load(std::memory_order_acquire)) {
                rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
                int k = (int)(rng % KEYSPACE);
                if (k >= PP_LO && k < PP_HI) continue;            // never touch the range
                if (n & 1) shard0.get(g_keys[k], out);
                else       shard0.put(g_keys[k], "v2");
                n++;
            }
            total_ops += n;
        });
    }

    // Migration thread: ping-pong the range back and forth until stop.
    std::thread mig([&] {
        thread_init_once();
        uint64_t gen = 0;
        while (!stop.load(std::memory_order_acquire)) {
            int from = owner.load(std::memory_order_acquire), to = 1 - from;
            janus::ShardMigrator m(shards[from], shards[to], lo, hi, ++gen);
            m.background_copy();
            m.lock();
            if (m.final_sync_and_verify()) {   // empty delta (no range writes) -> always matches
                m.commit();
                owner.store(to, std::memory_order_release);
                migrations += 1;
            } else {
                m.abort();
            }
        }
    });

    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(3));
    stop.store(true, std::memory_order_release);
    mig.join();
    for (auto& w : ws) w.join();
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    printf("[STRESS] SUSTAINED ping-pong: %.0f ops/sec  (%ld ops, %.1fs) with %ld migrations "
           "completed (%d-key range)\n",
           total_ops.load() / sec, total_ops.load(), sec, migrations.load(), PP_HI - PP_LO);
    fflush(stdout);

    // Correctness after hundreds of cutovers: the range is intact on its current
    // owner, absent on the other, and its data survived every ping-pong.
    const int fo = owner.load();
    EXPECT_EQ(shards[fo]->range_count(lo, hi), (size_t)(PP_HI - PP_LO));
    EXPECT_EQ(shards[1 - fo]->range_count(lo, hi), 0u);
    EXPECT_GT(migrations.load(), 10);
    std::string out;
    EXPECT_TRUE(shards[fo]->get(g_keys[PP_LO], out));     EXPECT_EQ(out, "v");
    EXPECT_TRUE(shards[fo]->get(g_keys[PP_HI - 1], out)); EXPECT_EQ(out, "v");
}

// (5) The starvation FIX (option B1): hammer the migrating range UNBOUNDED while
// a CHUNKED copy runs. A whole-range OCC scan starves (~25s+ / never advances,
// per the earlier commit); the chunked copy re-scans only a 64-key window per
// conflict, so the cursor always advances -> the copy COMPLETES, stays
// memory-safe (it's still an OCC read, just windowed), and -- because writes are
// captured in the delta -- loses nothing.
TEST_F(Stress, ChunkedCopyDoesNotStarveOnHotRange) {
    janus::OrderedIndexShardData src(fresh_index("chunk_src"));
    janus::OrderedIndexShardData dst(fresh_index("chunk_dst"));
    const std::string lo = g_keys[RANGE_LO], hi = g_keys[RANGE_HI];   // 10k-key range
    for (int i = RANGE_LO; i < RANGE_HI; i++) src.put(g_keys[i], "base");

    MigratingSource ms(&src, &dst);
    ms.begin(lo, hi);

    std::atomic<bool> stop{false};
    std::vector<std::map<std::string, std::string>> expect(NUM_WORKERS);
    std::vector<std::thread> ws;
    for (int t = 0; t < NUM_WORKERS; t++) {
        ws.emplace_back([&, t] {
            thread_init_once();
            uint64_t rng = 0xDEADull * (uint64_t)(t + 1);
            long ctr = 0;
            while (!stop.load(std::memory_order_acquire)) {          // UNBOUNDED hammer
                rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
                int idx = RANGE_LO + (int)(rng % (RANGE_HI - RANGE_LO));
                if (idx % NUM_WORKERS != t) continue;
                std::string val = "w" + std::to_string(t) + "_" + std::to_string(ctr++);
                if (ms.put(g_keys[idx], val)) expect[t][g_keys[idx]] = val;
            }
        });
    }

    // Chunked copy runs while the range is hammered UNBOUNDED. It must COMPLETE.
    auto t0 = std::chrono::steady_clock::now();
    long scans = ms.copy_chunked(/*chunk=*/64);
    double copy_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ms.lock();
    stop.store(true, std::memory_order_release);
    for (auto& w : ws) w.join();
    ASSERT_TRUE(ms.final_sync_and_verify());
    ms.commit();

    printf("[STRESS] chunked copy of a HOT range under UNBOUNDED writers: completed in %.3fs "
           "(%ld chunk-scans, chunk=64) -- no starvation\n", copy_sec, scans);
    fflush(stdout);

    long checked = 0;
    for (int t = 0; t < NUM_WORKERS; t++)
        for (const auto& kv : expect[t]) {
            std::string out;
            ASSERT_TRUE(dst.get(kv.first, out)) << "lost: " << kv.first;
            ASSERT_EQ(out, kv.second) << "stale: " << kv.first;
            checked++;
        }
    printf("[STRESS] ...and no lost/stale writes: %ld accepted writes verified on dst\n", checked);
    fflush(stdout);
    EXPECT_GT(checked, 0);
    EXPECT_EQ(src.range_count(lo, hi), 0u);
}

}  // namespace
