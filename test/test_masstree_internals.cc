/**
 * test_masstree_internals.cc
 *
 * Comprehensive tests for Masstree internals:
 * - threadinfo creation and lifecycle
 * - MasstreeContext epoch management
 * - RCU memory reclamation
 * - Multi-threaded operations
 *
 * These tests verify the current behavior with MasstreeContext
 * supporting multiple Masstree instances.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>
#include <set>

// Masstree headers
#include "masstree/masstree_context.h"
#include "masstree/kvthread.hh"

// Provide globalepoch definition for this test file
volatile mrcu_epoch_type globalepoch = 1;

class MasstreeInternalsTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Record initial state from context
        ctx_ = MasstreeContext::Current();
        initial_epoch_ = ctx_->get_epoch();
        initial_allthreads_ = ctx_->get_allthreads();
    }

    void TearDown() override {
        // Note: threadinfo objects are not freed (by design in Masstree)
        // Reset epoch if we changed it
        ctx_->set_epoch(initial_epoch_);
    }

    MasstreeContext* ctx_;
    mrcu_epoch_type initial_epoch_;
    threadinfo* initial_allthreads_;
};

// Test 1: ThreadInfo Creation
TEST_F(MasstreeInternalsTest, ThreadInfoCreation) {
    // Create a threadinfo
    threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 1000);
    ASSERT_NE(ti, nullptr);

    // Verify purpose and index
    EXPECT_EQ(ti->purpose(), threadinfo::TI_PROCESS);
    EXPECT_EQ(ti->index(), 1000);

    // Verify it's added to the context's allthreads list
    bool found = false;
    for (threadinfo* t = ctx_->get_allthreads(); t; t = t->next()) {
        if (t == ti) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "threadinfo should be in allthreads list";
}

// Test 2: Multiple ThreadInfo Creation
TEST_F(MasstreeInternalsTest, MultipleThreadInfoCreation) {
    const int COUNT = 5;
    std::vector<threadinfo*> infos;

    for (int i = 0; i < COUNT; ++i) {
        threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 2000 + i);
        ASSERT_NE(ti, nullptr);
        EXPECT_EQ(ti->index(), 2000 + i);
        infos.push_back(ti);
    }

    // Verify all are in the list
    std::set<threadinfo*> found_set;
    for (threadinfo* t = ctx_->get_allthreads(); t; t = t->next()) {
        found_set.insert(t);
    }

    for (auto* ti : infos) {
        EXPECT_TRUE(found_set.count(ti) > 0)
            << "threadinfo with index " << ti->index() << " not found in list";
    }
}

// Test 3: Context Epoch Basics
TEST_F(MasstreeInternalsTest, GlobalEpochBasics) {
    // context epoch should be at least 1
    EXPECT_GE(ctx_->get_epoch(), 1u);

    // Test increment
    mrcu_epoch_type before = ctx_->get_epoch();
    ctx_->increment_epoch(2);
    EXPECT_EQ(ctx_->get_epoch(), before + 2);

    // Restore
    ctx_->set_epoch(before);
}

// Test 4: RCU Start and Stop
TEST_F(MasstreeInternalsTest, RcuStartStop) {
    threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 3000);
    ASSERT_NE(ti, nullptr);

    // After creation, thread is not in RCU region
    // rcu_start() should sync with globalepoch
    ti->rcu_start();
    // No crash means success

    // rcu_stop() should work
    ti->rcu_stop();
    // No crash means success
}

// Test 5: RCU Quiesce
TEST_F(MasstreeInternalsTest, RcuQuiesce) {
    threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 3001);
    ASSERT_NE(ti, nullptr);

    ti->rcu_start();

    // Advance epoch and quiesce
    ctx_->increment_epoch(2);
    ti->rcu_quiesce();

    ctx_->increment_epoch(2);
    ti->rcu_quiesce();

    ti->rcu_stop();
}

// Test 6: Memory Allocation via threadinfo
TEST_F(MasstreeInternalsTest, MemoryAllocation) {
    threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 4000);
    ASSERT_NE(ti, nullptr);

    // Allocate memory
    void* p1 = ti->allocate(64, memtag_value);
    ASSERT_NE(p1, nullptr);

    void* p2 = ti->allocate(128, memtag_value);
    ASSERT_NE(p2, nullptr);
    EXPECT_NE(p1, p2);

    // Deallocate
    ti->deallocate(p1, 64, memtag_value);
    ti->deallocate(p2, 128, memtag_value);
}

// Test 7: Pool Allocation
TEST_F(MasstreeInternalsTest, PoolAllocation) {
    threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 4001);
    ASSERT_NE(ti, nullptr);

    // Pool allocate (cache-line aligned)
    void* p1 = ti->pool_allocate(64, memtag_masstree_leaf);
    ASSERT_NE(p1, nullptr);

    void* p2 = ti->pool_allocate(64, memtag_masstree_leaf);
    ASSERT_NE(p2, nullptr);
    EXPECT_NE(p1, p2);

    // Pool deallocate returns to freelist
    ti->pool_deallocate(p1, 64, memtag_masstree_leaf);

    // Next allocation of same size should reuse freed memory
    void* p3 = ti->pool_allocate(64, memtag_masstree_leaf);
    EXPECT_EQ(p3, p1) << "Pool should reuse freed memory";

    // Cleanup
    ti->pool_deallocate(p2, 64, memtag_masstree_leaf);
    ti->pool_deallocate(p3, 64, memtag_masstree_leaf);
}

// Test 8: RCU Deferred Deallocation
TEST_F(MasstreeInternalsTest, RcuDeferredDeallocation) {
    threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 5000);
    ASSERT_NE(ti, nullptr);

    ti->rcu_start();

    // Allocate memory
    void* p = ti->allocate(128, memtag_value);
    ASSERT_NE(p, nullptr);

    // Defer deallocation via RCU
    ti->deallocate_rcu(p, 128, memtag_value);

    // Memory is not immediately freed - it's in limbo
    // We need to advance epochs and quiesce to actually free it

    // Advance epoch multiple times to trigger cleanup
    for (int i = 0; i < 5; ++i) {
        ctx_->increment_epoch(2);
        ti->rcu_quiesce();
    }

    ti->rcu_stop();
}

// Test 9: Thread Purposes
TEST_F(MasstreeInternalsTest, ThreadPurposes) {
    // Test different thread purposes
    threadinfo* ti_main = threadinfo::make(threadinfo::TI_MAIN, -1);
    ASSERT_NE(ti_main, nullptr);
    EXPECT_EQ(ti_main->purpose(), threadinfo::TI_MAIN);

    threadinfo* ti_proc = threadinfo::make(threadinfo::TI_PROCESS, 0);
    ASSERT_NE(ti_proc, nullptr);
    EXPECT_EQ(ti_proc->purpose(), threadinfo::TI_PROCESS);

    threadinfo* ti_log = threadinfo::make(threadinfo::TI_LOG, 0);
    ASSERT_NE(ti_log, nullptr);
    EXPECT_EQ(ti_log->purpose(), threadinfo::TI_LOG);

    threadinfo* ti_ckpt = threadinfo::make(threadinfo::TI_CHECKPOINT, 0);
    ASSERT_NE(ti_ckpt, nullptr);
    EXPECT_EQ(ti_ckpt->purpose(), threadinfo::TI_CHECKPOINT);
}

// Test 10: Multi-threaded ThreadInfo Creation
TEST_F(MasstreeInternalsTest, MultithreadedThreadInfoCreation) {
    const int NUM_THREADS = 4;
    std::vector<std::thread> threads;
    std::atomic<int> created{0};
    std::vector<threadinfo*> thread_infos(NUM_THREADS, nullptr);

    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([i, &created, &thread_infos]() {
            threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 6000 + i);
            ASSERT_NE(ti, nullptr);
            EXPECT_EQ(ti->index(), 6000 + i);
            thread_infos[i] = ti;
            created++;
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(created.load(), NUM_THREADS);

    // Verify all threadinfos are in the context's list
    std::set<threadinfo*> found_set;
    for (threadinfo* t = ctx_->get_allthreads(); t; t = t->next()) {
        found_set.insert(t);
    }

    for (int i = 0; i < NUM_THREADS; ++i) {
        EXPECT_NE(thread_infos[i], nullptr);
        EXPECT_TRUE(found_set.count(thread_infos[i]) > 0)
            << "Thread " << i << "'s threadinfo not found in context's list";
    }
}

// Test 11: Multi-threaded RCU Operations
TEST_F(MasstreeInternalsTest, MultithreadedRcuOperations) {
    const int NUM_THREADS = 4;
    std::vector<std::thread> threads;
    std::atomic<int> completed{0};

    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([i, &completed]() {
            threadinfo* ti = threadinfo::make(threadinfo::TI_PROCESS, 7000 + i);
            ASSERT_NE(ti, nullptr);

            // Perform RCU operations
            ti->rcu_start();

            // Allocate and deallocate via RCU
            for (int j = 0; j < 10; ++j) {
                void* p = ti->allocate(64, memtag_value);
                ASSERT_NE(p, nullptr);
                ti->deallocate_rcu(p, 64, memtag_value);
            }

            // Quiesce
            ti->rcu_quiesce();
            ti->rcu_stop();

            completed++;
        });
    }

    // Main thread advances epoch periodically
    for (int i = 0; i < 10 && completed.load() < NUM_THREADS; ++i) {
        ctx_->increment_epoch(2);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(completed.load(), NUM_THREADS);
}

// Test 12: Context Epoch is shared across threadinfos in same context
TEST_F(MasstreeInternalsTest, GlobalEpochShared) {
    threadinfo* ti1 = threadinfo::make(threadinfo::TI_PROCESS, 8000);
    threadinfo* ti2 = threadinfo::make(threadinfo::TI_PROCESS, 8001);

    ti1->rcu_start();
    ti2->rcu_start();

    // Both should see the same context epoch
    mrcu_epoch_type epoch_before = ctx_->get_epoch();

    // Increment context epoch
    ctx_->increment_epoch(2);

    // Both threads should see the new epoch when they quiesce
    ti1->rcu_quiesce();
    ti2->rcu_quiesce();

    EXPECT_EQ(ctx_->get_epoch(), epoch_before + 2);

    ti1->rcu_stop();
    ti2->rcu_stop();
}

// Test 13: Allthreads list integrity
TEST_F(MasstreeInternalsTest, AllthreadsListIntegrity) {
    // Create several threadinfos
    std::vector<threadinfo*> infos;
    for (int i = 0; i < 10; ++i) {
        infos.push_back(threadinfo::make(threadinfo::TI_PROCESS, 9000 + i));
    }

    // Walk the list and count - should be consistent
    int count1 = 0;
    for (threadinfo* t = ctx_->get_allthreads(); t; t = t->next()) {
        count1++;
        // Prevent infinite loop
        if (count1 > 10000) {
            FAIL() << "allthreads list appears to have a cycle";
        }
    }

    // Walk again - should get same count
    int count2 = 0;
    for (threadinfo* t = ctx_->get_allthreads(); t; t = t->next()) {
        count2++;
        if (count2 > 10000) {
            FAIL() << "allthreads list appears to have a cycle";
        }
    }

    EXPECT_EQ(count1, count2) << "allthreads list should be stable";
}
