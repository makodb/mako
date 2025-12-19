/**
 * test_silo_runtime.cc
 *
 * Tests for SiloRuntime - per-site runtime context.
 * Verifies that multiple sites can run independently in a single process.
 * Uses RustyCpp smart pointers for memory safety.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>
#include <memory>

#include "silo_runtime.h"
#include "core.h"
#include "macros.h"
#include "masstree/masstree_context.h"
#include "masstree/kvthread.hh"
#include "mako/masstree_btree.h"
#include "mako/varkey.h"

// Provide globalepoch definition for this test file
volatile mrcu_epoch_type globalepoch = 1;

using TestTree = single_threaded_btree;

class SiloRuntimeTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create two runtimes for two sites using Arc smart pointers
        // Use rusty::Some() to wrap Arc in Option
        site1_ = rusty::Some(SiloRuntime::Create());
        site2_ = rusty::Some(SiloRuntime::Create());

        ASSERT_TRUE(site1_.is_some());
        ASSERT_TRUE(site2_.is_some());
        ASSERT_NE(site1()->id(), site2()->id());
        ASSERT_NE(site1()->masstree_context(), site2()->masstree_context());
    }

    void TearDown() override {
        // Reset to None, Arc handles cleanup automatically
        site1_ = rusty::None;
        site2_ = rusty::None;
    }

    // Helper to get the runtime pointer (use as_ref to avoid moving the Arc)
    SiloRuntime* site1() { return site1_.as_ref().unwrap().get_mut(); }
    SiloRuntime* site2() { return site2_.as_ref().unwrap().get_mut(); }

    // Use Option<Arc> for nullable Arc members (gtest requires default-constructible fixtures)
    rusty::Option<rusty::Arc<SiloRuntime>> site1_;
    rusty::Option<rusty::Arc<SiloRuntime>> site2_;
};

// Test 1: Basic runtime creation and isolation

TEST_F(SiloRuntimeTest, BasicCreationAndIsolation) {
    // Each runtime should have its own MasstreeContext
    EXPECT_NE(site1()->masstree_context()->id(), site2()->masstree_context()->id());

    // Initial epochs should both be 1
    EXPECT_EQ(site1()->masstree_context()->get_epoch(), 1u);
    EXPECT_EQ(site2()->masstree_context()->get_epoch(), 1u);
}

// Test 2: Thread binding and Current() accessor

TEST_F(SiloRuntimeTest, ThreadBindingAndCurrent) {
    // Bind site1 to this thread
    SiloRuntime::BindCurrentThread(site1());
    EXPECT_EQ(SiloRuntime::Current(), site1());
    EXPECT_EQ(MasstreeContext::Current(), site1()->masstree_context());

    // Rebind to site2
    SiloRuntime::BindCurrentThread(site2());
    EXPECT_EQ(SiloRuntime::Current(), site2());
    EXPECT_EQ(MasstreeContext::Current(), site2()->masstree_context());
}

// Test 3: Concurrent operations on different sites

TEST_F(SiloRuntimeTest, ConcurrentSiteOperations) {
    const int NUM_THREADS_PER_SITE = 4;
    const int OPS_PER_THREAD = 100;

    std::atomic<int> site1_completed{0};
    std::atomic<int> site2_completed{0};

    // Get raw pointers for thread use (Arc ensures lifetime)
    SiloRuntime* site1_ptr = site1();
    SiloRuntime* site2_ptr = site2();

    // Worker function
    auto worker = [](SiloRuntime* site, int thread_id,
                     std::atomic<int>& completed, int ops) {
        site->BindToCurrentThread();
        threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, thread_id);

        ti->rcu_start();

        for (int i = 0; i < ops; ++i) {
            void* p = ti->allocate(64, memtag_value);
            ASSERT_NE(p, nullptr);
            ti->deallocate_rcu(p, 64, memtag_value);

            if (i % 10 == 0) {
                ti->rcu_quiesce();
            }
        }

        ti->rcu_stop();
        completed++;
    };

    // Spawn threads for site1
    std::vector<std::thread> site1_threads;
    for (int i = 0; i < NUM_THREADS_PER_SITE; ++i) {
        site1_threads.emplace_back(worker, site1_ptr, 1000 + i,
                                   std::ref(site1_completed), OPS_PER_THREAD);
    }

    // Spawn threads for site2
    std::vector<std::thread> site2_threads;
    for (int i = 0; i < NUM_THREADS_PER_SITE; ++i) {
        site2_threads.emplace_back(worker, site2_ptr, 2000 + i,
                                   std::ref(site2_completed), OPS_PER_THREAD);
    }

    // Epoch advancement threads
    std::atomic<bool> stop{false};

    std::thread epoch1([site1_ptr, &stop]() {
        while (!stop.load()) {
            site1_ptr->masstree_context()->increment_epoch(2);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    std::thread epoch2([site2_ptr, &stop]() {
        while (!stop.load()) {
            site2_ptr->masstree_context()->increment_epoch(2);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    // Wait for workers
    for (auto& t : site1_threads) t.join();
    for (auto& t : site2_threads) t.join();

    stop = true;
    epoch1.join();
    epoch2.join();

    EXPECT_EQ(site1_completed.load(), NUM_THREADS_PER_SITE);
    EXPECT_EQ(site2_completed.load(), NUM_THREADS_PER_SITE);
}

// Test 4: Two sites with independent Masstree instances

TEST_F(SiloRuntimeTest, IndependentMasstreeInstances) {
    const int NUM_KEYS = 5000;

    TestTree tree1;
    TestTree tree2;

    std::atomic<bool> done1{false};
    std::atomic<bool> done2{false};
    std::atomic<bool> stop_epoch{false};

    // Storage for values and keys
    std::vector<std::unique_ptr<uint64_t>> values1;
    std::vector<std::unique_ptr<uint64_t>> values2;
    std::vector<u64_varkey> keys1;
    std::vector<u64_varkey> keys2;

    values1.reserve(NUM_KEYS);
    values2.reserve(NUM_KEYS);
    keys1.reserve(NUM_KEYS);
    keys2.reserve(NUM_KEYS);

    for (int i = 0; i < NUM_KEYS; ++i) {
        keys1.emplace_back(static_cast<uint64_t>(i));
        keys2.emplace_back(static_cast<uint64_t>(i + 1000000));  // Different key space
    }

    auto make_value = [](std::vector<std::unique_ptr<uint64_t>>& storage, uint64_t v) -> TestTree::value_type {
        storage.emplace_back(std::make_unique<uint64_t>(v));
        return reinterpret_cast<TestTree::value_type>(storage.back().get());
    };

    // Get raw pointers for thread use
    SiloRuntime* site1_ptr = site1();
    SiloRuntime* site2_ptr = site2();

    // Worker for site1
    std::thread worker1([&, site1_ptr]() {
        site1_ptr->BindToCurrentThread();
        threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 3000);

        ti->rcu_start();

        // Insert keys
        for (int i = 0; i < NUM_KEYS; ++i) {
            tree1.insert(keys1[i], make_value(values1, i));
            if (i % 100 == 0) ti->rcu_quiesce();
        }

        // Verify keys
        for (int i = 0; i < NUM_KEYS; ++i) {
            TestTree::value_type found{};
            EXPECT_TRUE(tree1.search(keys1[i], found)) << "site1 tree missing key " << i;
        }

        ti->rcu_stop();
        done1 = true;
    });

    // Worker for site2
    std::thread worker2([&, site2_ptr]() {
        site2_ptr->BindToCurrentThread();
        threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 4000);

        ti->rcu_start();

        // Insert keys
        for (int i = 0; i < NUM_KEYS; ++i) {
            tree2.insert(keys2[i], make_value(values2, i + 1000000));
            if (i % 100 == 0) ti->rcu_quiesce();
        }

        // Verify keys
        for (int i = 0; i < NUM_KEYS; ++i) {
            TestTree::value_type found{};
            EXPECT_TRUE(tree2.search(keys2[i], found)) << "site2 tree missing key " << i;
        }

        ti->rcu_stop();
        done2 = true;
    });

    // Epoch threads
    std::thread epoch1([site1_ptr, &stop_epoch]() {
        while (!stop_epoch.load()) {
            site1_ptr->masstree_context()->increment_epoch(2);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    std::thread epoch2([site2_ptr, &stop_epoch]() {
        while (!stop_epoch.load()) {
            site2_ptr->masstree_context()->increment_epoch(2);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    worker1.join();
    worker2.join();

    stop_epoch = true;
    epoch1.join();
    epoch2.join();

    EXPECT_TRUE(done1.load());
    EXPECT_TRUE(done2.load());

    // Cross-check: tree1 should NOT have tree2 keys and vice versa
    for (int i = 0; i < 100; ++i) {
        TestTree::value_type found{};
        EXPECT_FALSE(tree1.search(keys2[i], found)) << "tree1 should not have tree2 key " << i;
        EXPECT_FALSE(tree2.search(keys1[i], found)) << "tree2 should not have tree1 key " << i;
    }
}

// Test 5: Global default runtime for backward compatibility

TEST_F(SiloRuntimeTest, GlobalDefaultRuntime) {
    // Without binding, Current() should return the global default
    tl_silo_runtime = nullptr;  // Clear any previous binding

    SiloRuntime* global = SiloRuntime::GlobalDefault();
    ASSERT_NE(global, nullptr);

    // Same global should be returned on subsequent calls
    EXPECT_EQ(SiloRuntime::GlobalDefault(), global);

    // Current() should also return global default when nothing bound
    EXPECT_EQ(SiloRuntime::Current(), global);
}

// Test 6: BindToCurrentThread convenience method

TEST_F(SiloRuntimeTest, BindToCurrentThreadConvenience) {
    // Use convenience method
    site1()->BindToCurrentThread();

    // Both SiloRuntime and MasstreeContext should be bound
    EXPECT_EQ(SiloRuntime::Current(), site1());
    EXPECT_EQ(MasstreeContext::Current(), site1()->masstree_context());

    // Switch to site2
    site2()->BindToCurrentThread();

    EXPECT_EQ(SiloRuntime::Current(), site2());
    EXPECT_EQ(MasstreeContext::Current(), site2()->masstree_context());
}

// Test 7: Per-runtime core ID allocation

TEST_F(SiloRuntimeTest, PerRuntimeCoreIdAllocation) {
    // Each runtime should have its own core ID counter starting at 0
    EXPECT_EQ(site1()->core_count(), 0u);
    EXPECT_EQ(site2()->core_count(), 0u);

    // Allocate core IDs from site1
    site1()->BindToCurrentThread();
    unsigned core1a = coreid::core_id();
    EXPECT_EQ(core1a, 0u);  // First core ID for site1
    EXPECT_EQ(site1()->core_count(), 1u);

    // Allocating again from same thread should return cached value
    unsigned core1b = coreid::core_id();
    EXPECT_EQ(core1b, core1a);
    EXPECT_EQ(site1()->core_count(), 1u);  // No new allocation

    // Switch to site2 - should get a NEW core ID from site2's space
    site2()->BindToCurrentThread();
    coreid::reset_core_id();  // Force re-allocation
    unsigned core2a = coreid::core_id();
    EXPECT_EQ(core2a, 0u);  // First core ID for site2 (starts at 0)
    EXPECT_EQ(site2()->core_count(), 1u);

    // site1 should still have count 1, site2 now has count 1
    EXPECT_EQ(site1()->core_count(), 1u);
    EXPECT_EQ(site2()->core_count(), 1u);
}

// Test 8: Core ID isolation across threads

TEST_F(SiloRuntimeTest, CoreIdIsolationAcrossThreads) {
    const int NUM_THREADS = 4;
    std::atomic<int> site1_count{0};
    std::atomic<int> site2_count{0};

    SiloRuntime* site1_ptr = site1();
    SiloRuntime* site2_ptr = site2();

    std::vector<std::thread> threads;

    // Spawn threads for site1
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([site1_ptr, &site1_count]() {
            site1_ptr->BindToCurrentThread();
            unsigned cid = coreid::core_id();
            EXPECT_LT(cid, NMAXCORES);
            site1_count++;
        });
    }

    // Spawn threads for site2
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([site2_ptr, &site2_count]() {
            site2_ptr->BindToCurrentThread();
            unsigned cid = coreid::core_id();
            EXPECT_LT(cid, NMAXCORES);
            site2_count++;
        });
    }

    for (auto& t : threads) t.join();

    EXPECT_EQ(site1_count.load(), NUM_THREADS);
    EXPECT_EQ(site2_count.load(), NUM_THREADS);

    // Each runtime should have allocated NUM_THREADS core IDs
    EXPECT_EQ(site1()->core_count(), NUM_THREADS);
    EXPECT_EQ(site2()->core_count(), NUM_THREADS);
}
