/**
 * test_masstree_multi_instance.cc
 *
 * Stress tests for multiple independent Masstree instances.
 * Verifies that MasstreeContext properly isolates:
 * - Epoch counters
 * - Thread registries
 * - RCU operations
 */

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>
#include <set>
#include <random>
#include <chrono>
#include <memory>

#include "masstree/masstree_context.h"
#include "masstree/kvthread.hh"
#include "mako/masstree_btree.h"
#include "mako/varkey.h"

// Provide globalepoch definition for this test file
volatile mrcu_epoch_type globalepoch = 1;

using TestTree = single_threaded_btree;

class MasstreeMultiInstanceTest : public ::testing::Test {
protected:
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

    TestTree::value_type MakeValue(uint64_t v) {
        storage_.emplace_back(std::make_unique<uint64_t>(v));
        return reinterpret_cast<TestTree::value_type>(storage_.back().get());
    }

    MasstreeContext* ctx1_;
    MasstreeContext* ctx2_;
    std::vector<std::unique_ptr<uint64_t>> storage_;
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
    std::vector<threadinfo*> ctx1_threads;
    std::vector<threadinfo*> ctx2_threads;
    std::mutex mtx;

    // Create threads bound to ctx1
    std::vector<std::thread> threads1;
    for (int i = 0; i < 3; ++i) {
        threads1.emplace_back([this, i, &ctx1_threads, &mtx]() {
            MasstreeContext::BindCurrentThread(ctx1_);
            threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 1000 + i);
            ASSERT_NE(ti, nullptr);
            EXPECT_EQ(ti->context(), ctx1_);
            std::lock_guard<std::mutex> lock(mtx);
            ctx1_threads.push_back(ti);
        });
    }

    // Create threads bound to ctx2
    std::vector<std::thread> threads2;
    for (int i = 0; i < 3; ++i) {
        threads2.emplace_back([this, i, &ctx2_threads, &mtx]() {
            MasstreeContext::BindCurrentThread(ctx2_);
            threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 2000 + i);
            ASSERT_NE(ti, nullptr);
            EXPECT_EQ(ti->context(), ctx2_);
            std::lock_guard<std::mutex> lock(mtx);
            ctx2_threads.push_back(ti);
        });
    }

    for (auto& t : threads1) t.join();
    for (auto& t : threads2) t.join();

    // Verify ctx1's thread list only contains ctx1 threads
    std::set<threadinfo*> ctx1_set;
    for (threadinfo* ti = ctx1_->get_allthreads(); ti; ti = ti->next()) {
        ctx1_set.insert(ti);
        EXPECT_EQ(ti->context(), ctx1_);
    }

    // Verify ctx2's thread list only contains ctx2 threads
    std::set<threadinfo*> ctx2_set;
    for (threadinfo* ti = ctx2_->get_allthreads(); ti; ti = ti->next()) {
        ctx2_set.insert(ti);
        EXPECT_EQ(ti->context(), ctx2_);
    }

    // Verify no overlap
    for (auto* ti : ctx1_threads) {
        EXPECT_TRUE(ctx1_set.count(ti) > 0);
        EXPECT_TRUE(ctx2_set.count(ti) == 0);
    }
    for (auto* ti : ctx2_threads) {
        EXPECT_TRUE(ctx2_set.count(ti) > 0);
        EXPECT_TRUE(ctx1_set.count(ti) == 0);
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

    // Spawn threads for ctx1
    std::vector<std::thread> threads1;
    for (int i = 0; i < NUM_THREADS_PER_CTX; ++i) {
        threads1.emplace_back(rcu_worker, ctx1_, 3000 + i,
                              std::ref(ctx1_completed), OPS_PER_THREAD);
    }

    // Spawn threads for ctx2
    std::vector<std::thread> threads2;
    for (int i = 0; i < NUM_THREADS_PER_CTX; ++i) {
        threads2.emplace_back(rcu_worker, ctx2_, 4000 + i,
                              std::ref(ctx2_completed), OPS_PER_THREAD);
    }

    // Epoch advancement threads (one per context)
    std::atomic<bool> stop{false};

    std::thread epoch1([this, &stop]() {
        while (!stop.load()) {
            ctx1_->increment_epoch(2);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    std::thread epoch2([this, &stop]() {
        while (!stop.load()) {
            ctx2_->increment_epoch(2);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    // Wait for workers
    for (auto& t : threads1) t.join();
    for (auto& t : threads2) t.join();

    stop = true;
    epoch1.join();
    epoch2.join();

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

    // Create two Masstree instances
    TestTree tree1;
    TestTree tree2;

    std::atomic<bool> tree1_done{false};
    std::atomic<bool> tree2_done{false};
    std::atomic<bool> stop_epoch{false};

    // Storage for values
    std::vector<std::unique_ptr<uint64_t>> value_storage1;
    std::vector<std::unique_ptr<uint64_t>> value_storage2;
    auto make_value1 = [&](uint64_t v) -> TestTree::value_type {
        value_storage1.emplace_back(std::make_unique<uint64_t>(v));
        return reinterpret_cast<TestTree::value_type>(value_storage1.back().get());
    };
    auto make_value2 = [&](uint64_t v) -> TestTree::value_type {
        value_storage2.emplace_back(std::make_unique<uint64_t>(v));
        return reinterpret_cast<TestTree::value_type>(value_storage2.back().get());
    };

    // Storage for keys
    std::vector<u64_varkey> keys1;
    std::vector<u64_varkey> keys2;
    keys1.reserve(NUM_KEYS);
    keys2.reserve(NUM_KEYS);
    for (int i = 0; i < NUM_KEYS; ++i) {
        keys1.emplace_back(static_cast<uint64_t>(i));
        keys2.emplace_back(static_cast<uint64_t>(i + 1000000));  // Different key space
    }

    // Worker for tree1 (single-threaded tree access)
    std::thread worker1([&]() {
        MasstreeContext::BindCurrentThread(ctx1_);
        threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 5000);

        ti->rcu_start();

        // Insert all keys
        for (int i = 0; i < NUM_KEYS; ++i) {
            tree1.insert(keys1[i], make_value1(i));
            if (i % 100 == 0) {
                ti->rcu_quiesce();
            }
        }

        // Verify all keys are searchable
        for (int i = 0; i < NUM_KEYS; ++i) {
            TestTree::value_type found{};
            EXPECT_TRUE(tree1.search(keys1[i], found)) << "tree1 missing key " << i;
        }

        ti->rcu_stop();
        tree1_done = true;
    });

    // Worker for tree2 (single-threaded tree access)
    std::thread worker2([&]() {
        MasstreeContext::BindCurrentThread(ctx2_);
        threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 6000);

        ti->rcu_start();

        // Insert all keys
        for (int i = 0; i < NUM_KEYS; ++i) {
            tree2.insert(keys2[i], make_value2(i + 1000000));
            if (i % 100 == 0) {
                ti->rcu_quiesce();
            }
        }

        // Verify all keys are searchable
        for (int i = 0; i < NUM_KEYS; ++i) {
            TestTree::value_type found{};
            EXPECT_TRUE(tree2.search(keys2[i], found)) << "tree2 missing key " << i;
        }

        ti->rcu_stop();
        tree2_done = true;
    });

    // Epoch advancement threads
    std::thread epoch1([this, &stop_epoch]() {
        while (!stop_epoch.load()) {
            ctx1_->increment_epoch(2);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    std::thread epoch2([this, &stop_epoch]() {
        while (!stop_epoch.load()) {
            ctx2_->increment_epoch(2);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    // Wait for tree workers
    worker1.join();
    worker2.join();

    stop_epoch = true;
    epoch1.join();
    epoch2.join();

    EXPECT_TRUE(tree1_done.load());
    EXPECT_TRUE(tree2_done.load());

    // Cross-check: tree1 should NOT have tree2 keys and vice versa
    for (int i = 0; i < 100; ++i) {
        TestTree::value_type found{};
        EXPECT_FALSE(tree1.search(keys2[i], found)) << "tree1 should not have tree2 key " << i;
        EXPECT_FALSE(tree2.search(keys1[i], found)) << "tree2 should not have tree1 key " << i;
    }
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
    std::thread stress1([&]() {
        rcu_stress_worker(ctx1_, NUM_OPS);
    });

    // Run stress test on ctx2
    std::thread stress2([&]() {
        rcu_stress_worker(ctx2_, NUM_OPS);
    });

    // Epoch advancement threads
    std::thread epoch1([this, &stop]() {
        while (!stop.load()) {
            ctx1_->increment_epoch(2);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    std::thread epoch2([this, &stop]() {
        while (!stop.load()) {
            ctx2_->increment_epoch(2);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    stress1.join();
    stress2.join();

    stop = true;
    epoch1.join();
    epoch2.join();

    // Both epochs should have advanced (from initial value of 1)
    EXPECT_GT(ctx1_->get_epoch(), 1u);
    EXPECT_GT(ctx2_->get_epoch(), 1u);
}
