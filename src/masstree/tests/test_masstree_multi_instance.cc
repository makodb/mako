/**
 * test_masstree_multi_instance.cc
 *
 * Tests for multiple independent Masstree instances.
 * Verifies that MasstreeContext properly isolates:
 * - Epoch counters
 * - Thread registries
 * - RCU operations
 *
 * Uses pure upstream Masstree only: Masstree::basic_table<P> with
 * raw tcursor / unlocked_tcursor calls, threadinfo from
 * masstree/kvthread.hh, and MasstreeContext for the per-instance
 * epoch / allthreads state. Deliberately NOT the Mako mbtree
 * wrapper, so the cap-bound coreid/SiloRuntime allocator path is
 * out of scope — those tests live in test_silo_runtime.cc.
 */

#include <stdint.h>

#include <gtest/gtest.h>

#include <rusty/box.hpp>
#include <rusty/mutex.hpp>
#include <rusty/thread.hpp>
#include <rusty/vec.hpp>

#include "masstree/masstree_context.h"
#include "masstree/kvthread.hh"
#include "masstree/masstree.hh"
#include "masstree/masstree_get.hh"
#include "masstree/masstree_insert.hh"
#include "masstree/masstree_print.hh"
#include "masstree/masstree_remove.hh"
#include "masstree/masstree_struct.hh"
#include "masstree/masstree_tcursor.hh"

import std;
import rusty;

// Provide globalepoch definition for this test file
volatile mrcu_epoch_type globalepoch = 1;

// ----------------------------------------------------------------------
// Pure-Masstree test plumbing.
//
// PureParams matches upstream Masstree::nodeparams<> with a uint64_t*
// value type and the kvthread.hh threadinfo. NO Mako symbols are
// referenced — no simple_threadinfo, no rcu::s_instance, no coreid,
// no mbtree wrapper.
// ----------------------------------------------------------------------

struct PureParams : public Masstree::nodeparams<> {
    typedef uint64_t* value_type;
    typedef Masstree::value_print<value_type> value_print_type;
    typedef threadinfo threadinfo_type;
};

using PureTable = Masstree::basic_table<PureParams>;

// Encode a uint64_t as an 8-byte big-endian key so lex order matches
// numeric order. Returned by value (stack-allocated string lives for
// the surrounding statement; cursors copy the bytes they need).
inline std::string be_u64(uint64_t v) {
    std::string s(8, '\0');
    for (int i = 7; i >= 0; --i) {
        s[i] = static_cast<char>(v & 0xff);
        v >>= 8;
    }
    return s;
}

// Insert via raw tcursor. Caller-managed RCU (must be inside
// ti.rcu_start() / ti.rcu_stop()). Returns true iff the key was new.
inline bool pure_insert(PureTable& t, threadinfo& ti, const std::string& k,
                        uint64_t* v) {
    Masstree::tcursor<PureParams> lp(t, k.data(), static_cast<int>(k.size()));
    bool found = lp.find_insert(ti);
    if (!found) ti.observe_phantoms(lp.node());
    lp.value() = v;
    lp.finish(1, ti);
    return !found;
}

// Search via raw unlocked_tcursor. Returns true if found and writes
// the value into *out.
inline bool pure_search(const PureTable& t, threadinfo& ti,
                        const std::string& k, uint64_t** out) {
    Masstree::unlocked_tcursor<PureParams> lp(t, k.data(),
                                              static_cast<int>(k.size()));
    bool found = lp.find_unlocked(ti);
    if (found) *out = lp.value();
    return found;
}

class MasstreeMultiInstanceTest : public ::testing::Test {
protected:
    ~MasstreeMultiInstanceTest() noexcept {}  // rusty::Vec member -> force noexcept dtor
    void SetUp() override {
        // Create two separate contexts
        ctx1_ = MasstreeContext::Create();
        ctx2_ = MasstreeContext::Create();

        ASSERT_NE(ctx1_, nullptr);
        ASSERT_NE(ctx2_, nullptr);
        ASSERT_NE(ctx1_->id(), ctx2_->id());
    }

    void TearDown() override {
        // Note: contexts are not deleted as threadinfos persist
    }

    uint64_t* StashValue(uint64_t v) {
        storage_.push(rusty::Box<uint64_t>::make(v));
        return storage_.back().get();
    }

    MasstreeContext* ctx1_;
    MasstreeContext* ctx2_;
    rusty::Vec<rusty::Box<uint64_t>> storage_;
};

// Test 1: Verify two contexts have separate epoch counters
TEST_F(MasstreeMultiInstanceTest, EpochIsolation) {
    // Initial epochs should both be 1
    EXPECT_EQ(ctx1_->get_epoch(), 1u);
    EXPECT_EQ(ctx2_->get_epoch(), 1u);

    // Increment ctx1's epoch
    ctx1_->increment_epoch(10);
    EXPECT_EQ(ctx1_->get_epoch(), 11u);
    EXPECT_EQ(ctx2_->get_epoch(), 1u);  // ctx2 unchanged

    // Increment ctx2's epoch differently
    ctx2_->increment_epoch(5);
    EXPECT_EQ(ctx1_->get_epoch(), 11u);  // ctx1 unchanged
    EXPECT_EQ(ctx2_->get_epoch(), 6u);
}

// Test 2: Verify two contexts have separate thread registries
TEST_F(MasstreeMultiInstanceTest, ThreadRegistryIsolation) {
    // Each per-context vector lives inside its own Mutex; only one
    // mutex per vector is needed since the writer threads partition
    // by destination.
    rusty::Mutex<rusty::Vec<threadinfo*>> ctx1_threads(rusty::Vec<threadinfo*>::new_());
    rusty::Mutex<rusty::Vec<threadinfo*>> ctx2_threads(rusty::Vec<threadinfo*>::new_());

    // Create threads bound to ctx1
    auto threads1 = rusty::Vec<rusty::thread::JoinHandle<void>>::with_capacity(3);
    for (int i = 0; i < 3; ++i) {
        threads1.push(rusty::thread::spawn([this, i, &ctx1_threads]() {
            MasstreeContext::BindCurrentThread(ctx1_);
            threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 1000 + i);
            ASSERT_NE(ti, nullptr);
            EXPECT_EQ(ti->context(), ctx1_);
            ctx1_threads.lock().unwrap()->push(ti);
        }));
    }

    // Create threads bound to ctx2
    auto threads2 = rusty::Vec<rusty::thread::JoinHandle<void>>::with_capacity(3);
    for (int i = 0; i < 3; ++i) {
        threads2.push(rusty::thread::spawn([this, i, &ctx2_threads]() {
            MasstreeContext::BindCurrentThread(ctx2_);
            threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 2000 + i);
            ASSERT_NE(ti, nullptr);
            EXPECT_EQ(ti->context(), ctx2_);
            ctx2_threads.lock().unwrap()->push(ti);
        }));
    }

    for (auto& t : threads1) { auto _ = t.join(); }
    for (auto& t : threads2) { auto _ = t.join(); }

    // Verify ctx1's thread list only contains ctx1 threads
    rusty::HashSet<threadinfo*> ctx1_set;
    for (threadinfo* ti = ctx1_->get_allthreads(); ti; ti = ti->next()) {
        ctx1_set.insert(ti);
        EXPECT_EQ(ti->context(), ctx1_);
    }

    // Verify ctx2's thread list only contains ctx2 threads
    rusty::HashSet<threadinfo*> ctx2_set;
    for (threadinfo* ti = ctx2_->get_allthreads(); ti; ti = ti->next()) {
        ctx2_set.insert(ti);
        EXPECT_EQ(ti->context(), ctx2_);
    }

    // Verify no overlap
    {
        auto g1 = ctx1_threads.lock().unwrap();
        for (auto* ti : *g1) {
            EXPECT_TRUE(ctx1_set.contains(ti));
            EXPECT_FALSE(ctx2_set.contains(ti));
        }
    }
    {
        auto g2 = ctx2_threads.lock().unwrap();
        for (auto* ti : *g2) {
            EXPECT_TRUE(ctx2_set.contains(ti));
            EXPECT_FALSE(ctx1_set.contains(ti));
        }
    }
}

// Test 3: Concurrent RCU operations on separate contexts
TEST_F(MasstreeMultiInstanceTest, ConcurrentRcuOperations) {
    const int NUM_THREADS_PER_CTX = 4;
    const int OPS_PER_THREAD = 100;

    std::atomic<int> ctx1_completed{0};
    std::atomic<int> ctx2_completed{0};

    // Worker function for RCU stress test
    auto rcu_worker = [](MasstreeContext* ctx, int thread_id,
                         std::atomic<int>& completed, int ops) {
        MasstreeContext::BindCurrentThread(ctx);
        threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, thread_id);

        ti->rcu_start();

        for (int i = 0; i < ops; ++i) {
            // Allocate and deallocate via RCU
            void* p = ti->allocate(64, memtag_value);
            ASSERT_NE(p, nullptr);
            ti->deallocate_rcu(p, 64, memtag_value);

            // Periodically quiesce
            if (i % 10 == 0) {
                ti->rcu_quiesce();
            }
        }

        ti->rcu_stop();
        completed++;
    };

    // Epoch advancement threads (one per context). Spawned before the
    // workers, and do-while so each context sees at least one increment
    // even when a loaded scheduler runs the short-lived workers to
    // completion (setting stop) before these threads first run.
    std::atomic<bool> stop{false};

    auto epoch1 = rusty::thread::spawn([this, &stop]() {
        do {
            ctx1_->increment_epoch(2);
            rusty::thread::sleep(std::chrono::milliseconds(5));
        } while (!stop.load());
    });

    auto epoch2 = rusty::thread::spawn([this, &stop]() {
        do {
            ctx2_->increment_epoch(2);
            rusty::thread::sleep(std::chrono::milliseconds(5));
        } while (!stop.load());
    });

    // Spawn threads for ctx1
    auto threads1 = rusty::Vec<rusty::thread::JoinHandle<void>>::with_capacity(NUM_THREADS_PER_CTX);
    for (int i = 0; i < NUM_THREADS_PER_CTX; ++i) {
        threads1.push(rusty::thread::spawn([this, i, &ctx1_completed, &rcu_worker]() {
            rcu_worker(ctx1_, 3000 + i, ctx1_completed, OPS_PER_THREAD);
        }));
    }

    // Spawn threads for ctx2
    auto threads2 = rusty::Vec<rusty::thread::JoinHandle<void>>::with_capacity(NUM_THREADS_PER_CTX);
    for (int i = 0; i < NUM_THREADS_PER_CTX; ++i) {
        threads2.push(rusty::thread::spawn([this, i, &ctx2_completed, &rcu_worker]() {
            rcu_worker(ctx2_, 4000 + i, ctx2_completed, OPS_PER_THREAD);
        }));
    }

    // Wait for workers
    for (auto& t : threads1) { auto _ = t.join(); }
    for (auto& t : threads2) { auto _ = t.join(); }

    stop = true;
    { auto _ = epoch1.join(); }
    { auto _ = epoch2.join(); }

    EXPECT_EQ(ctx1_completed.load(), NUM_THREADS_PER_CTX);
    EXPECT_EQ(ctx2_completed.load(), NUM_THREADS_PER_CTX);

    // Epochs should have advanced independently
    EXPECT_GT(ctx1_->get_epoch(), 1u);
    EXPECT_GT(ctx2_->get_epoch(), 1u);
}

// Test 4: Two Masstree instances running in parallel (single-threaded tree access)
// Each tree is accessed from a single thread, but both run concurrently with their
// own contexts and epoch advancement
TEST_F(MasstreeMultiInstanceTest, TwoMasstreeInstancesParallel) {
    const int NUM_KEYS = 10000;

    // Two pure-Masstree tables, one per MasstreeContext. The
    // initialization threadinfos are short-lived setup workers
    // bound to each context.
    PureTable tree1, tree2;
    {
        auto init1 = rusty::thread::spawn([this, &tree1]() {
            MasstreeContext::BindCurrentThread(ctx1_);
            threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 4001);
            tree1.initialize(*ti);
        });
        auto init2 = rusty::thread::spawn([this, &tree2]() {
            MasstreeContext::BindCurrentThread(ctx2_);
            threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 4002);
            tree2.initialize(*ti);
        });
        { auto _ = init1.join(); }
        { auto _ = init2.join(); }
    }

    std::atomic<bool> tree1_done{false};
    std::atomic<bool> tree2_done{false};
    std::atomic<bool> stop_epoch{false};

    // Value storage lives across both workers; each lambda's
    // captured back-reference holds raw pointers into it.
    rusty::Vec<rusty::Box<uint64_t>> values1, values2;
    auto make_v1 = [&](uint64_t v) { values1.push(rusty::Box<uint64_t>::make(v)); return values1.back().get(); };
    auto make_v2 = [&](uint64_t v) { values2.push(rusty::Box<uint64_t>::make(v)); return values2.back().get(); };

    auto worker1 = rusty::thread::spawn([&]() {
        MasstreeContext::BindCurrentThread(ctx1_);
        threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 5000);
        ti->rcu_start();

        for (int i = 0; i < NUM_KEYS; ++i) {
            const std::string k = be_u64(static_cast<uint64_t>(i));
            pure_insert(tree1, *ti, k, make_v1(i));
            if (i % 100 == 0) ti->rcu_quiesce();
        }
        for (int i = 0; i < NUM_KEYS; ++i) {
            const std::string k = be_u64(static_cast<uint64_t>(i));
            uint64_t* out = nullptr;
            EXPECT_TRUE(pure_search(tree1, *ti, k, &out)) << "tree1 missing key " << i;
        }

        ti->rcu_stop();
        tree1_done = true;
    });

    auto worker2 = rusty::thread::spawn([&]() {
        MasstreeContext::BindCurrentThread(ctx2_);
        threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 6000);
        ti->rcu_start();

        for (int i = 0; i < NUM_KEYS; ++i) {
            const std::string k = be_u64(static_cast<uint64_t>(i + 1000000));
            pure_insert(tree2, *ti, k, make_v2(i + 1000000));
            if (i % 100 == 0) ti->rcu_quiesce();
        }
        for (int i = 0; i < NUM_KEYS; ++i) {
            const std::string k = be_u64(static_cast<uint64_t>(i + 1000000));
            uint64_t* out = nullptr;
            EXPECT_TRUE(pure_search(tree2, *ti, k, &out)) << "tree2 missing key " << i;
        }

        ti->rcu_stop();
        tree2_done = true;
    });

    // Epoch advancement threads
    auto epoch1 = rusty::thread::spawn([this, &stop_epoch]() {
        while (!stop_epoch.load()) {
            ctx1_->increment_epoch(2);
            rusty::thread::sleep(std::chrono::milliseconds(2));
        }
    });
    auto epoch2 = rusty::thread::spawn([this, &stop_epoch]() {
        while (!stop_epoch.load()) {
            ctx2_->increment_epoch(2);
            rusty::thread::sleep(std::chrono::milliseconds(2));
        }
    });

    { auto _ = worker1.join(); }
    { auto _ = worker2.join(); }

    stop_epoch = true;
    { auto _ = epoch1.join(); }
    { auto _ = epoch2.join(); }

    EXPECT_TRUE(tree1_done.load());
    EXPECT_TRUE(tree2_done.load());

    // Cross-check: tree1 should NOT have tree2's keys and vice versa.
    // Need a threadinfo bound to *some* context for the cursor calls.
    threadinfo* check_ti = threadinfo::make(threadinfo::TI_PROCESS, 7000);
    check_ti->rcu_start();
    for (int i = 0; i < 100; ++i) {
        const std::string k1 = be_u64(static_cast<uint64_t>(i));
        const std::string k2 = be_u64(static_cast<uint64_t>(i + 1000000));
        uint64_t* out = nullptr;
        EXPECT_FALSE(pure_search(tree1, *check_ti, k2, &out)) << "tree1 should not have tree2 key " << i;
        EXPECT_FALSE(pure_search(tree2, *check_ti, k1, &out)) << "tree2 should not have tree1 key " << i;
    }
    check_ti->rcu_stop();
}

// Test 5: Stress test with multiple RCU operations per context
TEST_F(MasstreeMultiInstanceTest, RcuStressTest) {
    const int NUM_OPS = 10000;

    std::atomic<bool> stop{false};

    auto rcu_stress_worker = [](MasstreeContext* ctx, int num_ops) {
        MasstreeContext::BindCurrentThread(ctx);
        threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 7000);

        ti->rcu_start();

        std::mt19937 rng(12345);
        std::uniform_int_distribution<int> size_dist(32, 256);

        for (int i = 0; i < num_ops; ++i) {
            int size = size_dist(rng);

            // Allocate
            void* p = ti->allocate(size, memtag_value);
            ASSERT_NE(p, nullptr);

            // Sometimes deallocate immediately, sometimes via RCU
            if (i % 3 == 0) {
                ti->deallocate(p, size, memtag_value);
            } else {
                ti->deallocate_rcu(p, size, memtag_value);
            }

            // Quiesce periodically
            if (i % 50 == 0) {
                ti->rcu_quiesce();
            }
        }

        ti->rcu_stop();
    };

    // Run stress test on ctx1
    auto stress1 = rusty::thread::spawn([&]() {
        rcu_stress_worker(ctx1_, NUM_OPS);
    });

    // Run stress test on ctx2
    auto stress2 = rusty::thread::spawn([&]() {
        rcu_stress_worker(ctx2_, NUM_OPS);
    });

    // Epoch advancement threads
    auto epoch1 = rusty::thread::spawn([this, &stop]() {
        while (!stop.load()) {
            ctx1_->increment_epoch(2);
            rusty::thread::sleep(std::chrono::milliseconds(1));
        }
    });

    auto epoch2 = rusty::thread::spawn([this, &stop]() {
        while (!stop.load()) {
            ctx2_->increment_epoch(2);
            rusty::thread::sleep(std::chrono::milliseconds(1));
        }
    });

    { auto _ = stress1.join(); }
    { auto _ = stress2.join(); }

    stop = true;
    { auto _ = epoch1.join(); }
    { auto _ = epoch2.join(); }

    // Both epochs should have advanced (from initial value of 1)
    EXPECT_GT(ctx1_->get_epoch(), 1u);
    EXPECT_GT(ctx2_->get_epoch(), 1u);
}

// Test 6: Many-context scaling.
//
// Existing tests use exactly two contexts; this one spins up N=16 to
// stress: (a) the s_next_context_id_ atomic under burst creation,
// (b) MasstreeContext-bound worker threads running in parallel
// without cross-contamination, and (c) the per-context epoch
// counter at scale. Each context owns its own tree; each worker
// inserts a disjoint key stripe and then reads the whole tree back.
TEST_F(MasstreeMultiInstanceTest, ManyContextsScale) {
    constexpr int kContexts = 16;
    constexpr int kKeysPerWorker = 1000;

    auto contexts = rusty::Vec<MasstreeContext*>::with_capacity(kContexts);
    rusty::HashSet<int> seen_ids;
    for (int i = 0; i < kContexts; ++i) {
        MasstreeContext* ctx = MasstreeContext::Create();
        ASSERT_NE(ctx, nullptr) << "ctx " << i;
        // Context IDs must be unique across the burst creation.
        EXPECT_TRUE(seen_ids.insert(ctx->id())) << "duplicate id " << ctx->id();
        contexts.push(ctx);
    }

    // Each worker gets its own pure-Masstree table, its own key
    // space, and its own context. PureTable is default-constructible
    // but each instance must be initialize()'d under a threadinfo
    // bound to the right context — done inside the worker. We push
    // empty (root_=nullptr) tables and let workers initialize them
    // in place. basic_table's move ctor properly transfers ownership
    // of root_, so even if Vec ever reallocated post-initialize the
    // pointers would migrate correctly — but with_capacity guarantees
    // no reallocation here.
    auto trees = rusty::Vec<PureTable>::with_capacity(kContexts);
    for (int i = 0; i < kContexts; ++i) trees.push(PureTable{});
    std::atomic<int> failures{0};

    auto workers = rusty::Vec<rusty::thread::JoinHandle<void>>::with_capacity(kContexts);
    for (int i = 0; i < kContexts; ++i) {
        workers.push(rusty::thread::spawn([i, &trees, &contexts, &failures]() {
            MasstreeContext::BindCurrentThread(contexts[i]);
            threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 10000 + i);
            if (ti == nullptr) { ++failures; return; }
            if (ti->context() != contexts[i]) { ++failures; return; }

            trees[i].initialize(*ti);
            ti->rcu_start();

            rusty::Vec<rusty::Box<uint64_t>> value_storage;
            const uint64_t base = static_cast<uint64_t>(i) * 1'000'000ull;
            for (int k = 0; k < kKeysPerWorker; ++k) {
                const std::string key = be_u64(base + k);
                value_storage.push(rusty::Box<uint64_t>::make(base + k));
                pure_insert(trees[i], *ti, key, value_storage.back().get());
                if ((k & 0x7F) == 0) ti->rcu_quiesce();
            }
            for (int k = 0; k < kKeysPerWorker; ++k) {
                const std::string key = be_u64(base + k);
                uint64_t* out = nullptr;
                if (!pure_search(trees[i], *ti, key, &out)) ++failures;
            }

            ti->rcu_stop();
        }));
    }
    for (auto& t : workers) { auto _ = t.join(); }

    EXPECT_EQ(failures.load(), 0);

    // Cross-check: no tree contains keys from a different stripe.
    // Bind to ctx1 (arbitrarily) for a check-thread cursor pass.
    threadinfo* check_ti = nullptr;
    {
        auto setup = rusty::thread::spawn([this, &check_ti]() {
            MasstreeContext::BindCurrentThread(ctx1_);
            check_ti = threadinfo::make(threadinfo::TI_PROCESS, 11000);
        });
        auto _ = setup.join();
    }
    ASSERT_NE(check_ti, nullptr);
    check_ti->rcu_start();
    for (int i = 0; i < kContexts; ++i) {
        // Spot-check: the FIRST key of every OTHER stripe is absent.
        for (int j = 0; j < kContexts; ++j) {
            if (j == i) continue;
            const std::string other_key = be_u64(static_cast<uint64_t>(j) * 1'000'000ull);
            uint64_t* out = nullptr;
            EXPECT_FALSE(pure_search(trees[i], *check_ti, other_key, &out))
                << "tree " << i << " unexpectedly has key from stripe " << j;
        }
    }
    check_ti->rcu_stop();

    // Each context's allthreads list must contain at least the
    // threadinfo we registered above (one per worker).
    for (int i = 0; i < kContexts; ++i) {
        int seen = 0;
        for (threadinfo* ti = contexts[i]->get_allthreads(); ti; ti = ti->next()) {
            EXPECT_EQ(ti->context(), contexts[i]) << "ctx " << i << " contaminated";
            ++seen;
        }
        EXPECT_GE(seen, 1) << "ctx " << i << " has no threadinfos";
    }
}

// Test 7: Default-context fallback.
//
// Threads that never call BindCurrentThread should fall back to a
// process-wide singleton allocated lazily on first use. This test
// fires N threads that all call Current() roughly simultaneously
// and asserts they observe the same pointer — which exercises the
// rusty::Once init we migrated to in f4c05a99. Also asserts that a
// thread which subsequently binds an explicit context sees that
// context's pointer (not the singleton) until it unbinds back to
// nullptr.
TEST_F(MasstreeMultiInstanceTest, DefaultContextFallbackIsStable) {
    constexpr int kThreads = 16;
    auto observed = rusty::Vec<MasstreeContext*>::with_capacity(kThreads);
    for (int i = 0; i < kThreads; ++i) observed.push(nullptr);

    // Barrier so every thread races into Current() at roughly the
    // same moment; maximizes the chance of triggering any Once-init
    // race that would let two threads see different defaults.
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};

    auto workers = rusty::Vec<rusty::thread::JoinHandle<void>>::with_capacity(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        workers.push(rusty::thread::spawn([i, &observed, &ready, &go]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                rusty::thread::yield_now();
            }
            // No BindCurrentThread() — so Current() must fall back
            // to the lazily-created singleton.
            observed[i] = MasstreeContext::Current();
        }));
    }
    while (ready.load(std::memory_order_acquire) < kThreads) {
        rusty::thread::yield_now();
    }
    go.store(true, std::memory_order_release);
    for (auto& t : workers) { auto _ = t.join(); }

    // All threads must observe the same non-null singleton pointer.
    ASSERT_NE(observed[0], nullptr);
    for (int i = 1; i < kThreads; ++i) {
        EXPECT_EQ(observed[i], observed[0])
            << "thread " << i << " saw a different default context";
    }

    // Calling Current() from the test thread (also unbound) must
    // yield the same singleton.
    EXPECT_EQ(MasstreeContext::Current(), observed[0]);

    // After binding an explicit context the same thread must see
    // the explicit one; after unbinding it must fall back to the
    // singleton again.
    MasstreeContext* explicit_ctx = MasstreeContext::Create();
    ASSERT_NE(explicit_ctx, nullptr);
    MasstreeContext::BindCurrentThread(explicit_ctx);
    EXPECT_EQ(MasstreeContext::Current(), explicit_ctx);

    MasstreeContext::BindCurrentThread(nullptr);
    EXPECT_EQ(MasstreeContext::Current(), observed[0])
        << "unbinding did not fall back to the default singleton";
}

// Test 8: Rebind across contexts mid-thread.
//
// Existing tests bind a context once per thread and leave it. This
// one drives the bind/work/rebind/work/rebind-back pattern from a
// single thread and asserts both contexts end up with consistent
// state: independent epoch counters, threadinfo registered against
// the context that was bound at make() time, and the trees built
// while bound to each ctx contain only that ctx's keys.
TEST_F(MasstreeMultiInstanceTest, RebindAcrossContextsKeepsBothConsistent) {
    PureTable tree_a, tree_b;
    constexpr int kKeys = 200;
    rusty::Vec<rusty::Box<uint64_t>> values_a, values_b;
    auto make_value = [](rusty::Vec<rusty::Box<uint64_t>>& store, uint64_t v) -> uint64_t* {
        store.push(rusty::Box<uint64_t>::make(v));
        return store.back().get();
    };

    const uint64_t base_a = 0;
    const uint64_t base_b = 1ull << 20;

    // Phase 1: bind ctx1_, register a threadinfo, initialize and fill tree_a.
    MasstreeContext::BindCurrentThread(ctx1_);
    EXPECT_EQ(MasstreeContext::Current(), ctx1_);
    threadinfo* ti_a = threadinfo::make(threadinfo::TI_PROCESS, 20001);
    ASSERT_NE(ti_a, nullptr);
    EXPECT_EQ(ti_a->context(), ctx1_);
    tree_a.initialize(*ti_a);
    ti_a->rcu_start();
    for (int i = 0; i < kKeys; ++i) {
        const std::string k = be_u64(base_a + i);
        pure_insert(tree_a, *ti_a, k, make_value(values_a, base_a + i));
    }
    ti_a->rcu_stop();

    // Snapshot ctx1_'s epoch so we can assert ctx2_ work doesn't
    // touch it.
    const mrcu_epoch_type ctx1_epoch_before = ctx1_->get_epoch();
    ctx2_->increment_epoch(4);
    EXPECT_EQ(ctx1_->get_epoch(), ctx1_epoch_before);

    // Phase 2: REBIND to ctx2_, register a new threadinfo, initialize and fill tree_b.
    MasstreeContext::BindCurrentThread(ctx2_);
    EXPECT_EQ(MasstreeContext::Current(), ctx2_);
    threadinfo* ti_b = threadinfo::make(threadinfo::TI_PROCESS, 20002);
    ASSERT_NE(ti_b, nullptr);
    EXPECT_EQ(ti_b->context(), ctx2_)
        << "threadinfo created after rebind must belong to the newly-bound context";
    EXPECT_NE(ti_b, ti_a) << "threadinfo::make should not recycle across contexts";
    tree_b.initialize(*ti_b);
    ti_b->rcu_start();
    for (int i = 0; i < kKeys; ++i) {
        const std::string k = be_u64(base_b + i);
        pure_insert(tree_b, *ti_b, k, make_value(values_b, base_b + i));
    }
    ti_b->rcu_stop();

    // Phase 3: rebind back to ctx1_, confirm Current() switches back
    // and ctx1_'s pre-existing threadinfo is still on ctx1_'s list.
    MasstreeContext::BindCurrentThread(ctx1_);
    EXPECT_EQ(MasstreeContext::Current(), ctx1_);

    auto on_list = [](MasstreeContext* ctx, threadinfo* target) {
        for (threadinfo* ti = ctx->get_allthreads(); ti; ti = ti->next()) {
            if (ti == target) return true;
        }
        return false;
    };
    EXPECT_TRUE(on_list(ctx1_, ti_a));
    EXPECT_FALSE(on_list(ctx2_, ti_a));
    EXPECT_TRUE(on_list(ctx2_, ti_b));
    EXPECT_FALSE(on_list(ctx1_, ti_b));

    // Both trees retain only their own keys. Reuse ti_a for the
    // verification cursors — it's still bound to ctx1_ but cursors
    // are tree-local so reads against tree_b work too.
    ti_a->rcu_start();
    for (int i = 0; i < kKeys; ++i) {
        const std::string ka = be_u64(base_a + i);
        const std::string kb = be_u64(base_b + i);
        uint64_t* v = nullptr;
        EXPECT_TRUE (pure_search(tree_a, *ti_a, ka, &v));
        EXPECT_FALSE(pure_search(tree_a, *ti_a, kb, &v));
        EXPECT_TRUE (pure_search(tree_b, *ti_a, kb, &v));
        EXPECT_FALSE(pure_search(tree_b, *ti_a, ka, &v));
    }
    ti_a->rcu_stop();
}

// (Tests for SiloRuntime::try_register_current_thread — the Mako-side
// fix for sanitizer-findings.md Finding 6 — live in test_silo_runtime.cc.
// They belong with the SiloRuntime tests, not here, because the cap
// is a SiloRuntime concept, not a Masstree concept.)

// Counterpart to Finding 6: pure Masstree has no thread cap.
//
// The original Finding 6 abort happened in Mako's allocator layer
// (SiloRuntime::allocate_core_id → ALWAYS_ASSERT(id < NMaxCores))
// when an mbtree<masstree_params> (concurrent_btree) was used from
// ephemeral threads: each new thread lifetime consumed a core_id
// slot irreversibly. Pure upstream Masstree has no such cap;
// threadinfo::make() just prepends to a per-context linked list.
//
// This test proves the distinction: 5,000 ephemeral threads (well
// past Mako's 512 cap) each register a fresh threadinfo, perform a
// short batch of pure-Masstree tree ops, and exit. Cleanly
// completes — no abort. Same workload on concurrent_btree triggers
// the SIGABRT documented in docs/masstree-sanitizer-findings.md
// (Finding 6); see repro_finding6.cc.
TEST_F(MasstreeMultiInstanceTest, PureMasstreeAcceptsUnboundedEphemeralThreads) {
    PureTable tree;
    {
        // Initialize the tree under a setup threadinfo bound to ctx1_.
        auto init = rusty::thread::spawn([this, &tree]() {
            MasstreeContext::BindCurrentThread(ctx1_);
            threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 50000);
            tree.initialize(*ti);
        });
        auto _ = init.join();
    }

    constexpr int kEphemerals = 5000;       // ~10× Mako's NMAXCORES cap
    constexpr int kOpsPerThread = 16;

    std::atomic<int> completed{0};
    std::atomic<int> failures{0};

    // Sequential spawn/join — the exact pattern that aborts on
    // concurrent_btree. Each iteration is short, but the cumulative
    // thread count is what matters.
    for (int gen = 0; gen < kEphemerals; ++gen) {
        auto eph = rusty::thread::spawn([this, gen, &tree, &completed, &failures]() {
            MasstreeContext::BindCurrentThread(ctx1_);
            threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 50001 + gen);
            if (ti == nullptr) { ++failures; return; }

            ti->rcu_start();

            // Each ephemeral writes to its own key stripe so we
            // don't fight for the same leaf with previous gens.
            const uint64_t base = (1ull << 30) + static_cast<uint64_t>(gen) * kOpsPerThread;
            for (int i = 0; i < kOpsPerThread; ++i) {
                // Encode the value into the pointer itself — no
                // per-op heap allocation distorting the measurement.
                auto* v = reinterpret_cast<uint64_t*>(
                    static_cast<uintptr_t>(base + i));
                pure_insert(tree, *ti, be_u64(base + i), v);
            }
            for (int i = 0; i < kOpsPerThread; ++i) {
                uint64_t* out = nullptr;
                if (!pure_search(tree, *ti, be_u64(base + i), &out)) ++failures;
            }

            ti->rcu_stop();
            ++completed;
        });
        auto _ = eph.join();
    }

    EXPECT_EQ(completed.load(), kEphemerals);
    EXPECT_EQ(failures.load(), 0);
}

// Test 9: Context teardown / leak check.
//
// MasstreeContext intentionally leaks across the rest of this
// suite because the threadinfo list it points at outlives the
// process and threadinfos are not unregistered. That makes
// destroying a *populated* context dangerous (the linked list
// would dangle). But contexts that never had a thread registered
// CAN be destroyed safely — they own only their own scalars and
// the rusty::Mutex<MutPtr<threadinfo>> wrapper.
//
// This test documents that contract and locks it in: build a
// burst of contexts, advance their epochs, then destroy them
// without ever registering a threadinfo. Under ASan/LSan this
// becomes a real leak detector — if a context's destructor leaks
// the rusty::Mutex's internal pthread_mutex_t or any other state,
// LSan reports it at process exit.
TEST_F(MasstreeMultiInstanceTest, EmptyContextsAreDestructibleAndLeakFree) {
    constexpr int kContexts = 32;

    // Create, exercise, and destroy each context in turn.
    // Heap-allocate so we hit the operator-new / operator-delete
    // path (Create() uses `new`); raw delete mirrors what a future
    // teardown helper would call.
    for (int round = 0; round < 3; ++round) {
        auto batch = rusty::Vec<MasstreeContext*>::with_capacity(kContexts);
        for (int i = 0; i < kContexts; ++i) {
            MasstreeContext* ctx = MasstreeContext::Create();
            ASSERT_NE(ctx, nullptr);
            // Exercise: bump the epoch a few times so the std::atomic
            // path runs and the destructor has to release a counter
            // value other than the initial one.
            ctx->increment_epoch(2);
            ctx->set_epoch(ctx->get_epoch() + 10);
            // get_allthreads() under the new rusty::Mutex path must
            // work even with no threads registered.
            EXPECT_EQ(ctx->get_allthreads(), nullptr);
            batch.push(ctx);
        }
        for (MasstreeContext* ctx : batch) {
            delete ctx;
        }
    }
    // LSan at process exit checks for leaked allocations.
}
