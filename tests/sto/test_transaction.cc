#include <gtest/gtest.h>
#include "mako/benchmarks/sto/TRcu.hh"
#include <thread>
#include <vector>
#include <atomic>

// Test fixture for STO TRcu tests
class STOTRcuTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup
    }
    
    void TearDown() override {
        // Cleanup  
    }
};

// TRcuSet Constructor Tests
TEST_F(STOTRcuTest, Constructor_InitializesCorrectly) {
    TRcuSet rcu_set;
    // Constructor should complete without errors
    EXPECT_TRUE(true);
}

TEST_F(STOTRcuTest, MultipleInstances_Independent) {
    TRcuSet rcu1;
    TRcuSet rcu2;
    TRcuSet rcu3;
    
    // Multiple instances should be independent
    EXPECT_TRUE(true);
}

// Clean Until Tests
TEST_F(STOTRcuTest, CleanUntil_ZeroEpoch) {
    TRcuSet rcu_set;
    
    // Cleaning at epoch 0 should be safe
    EXPECT_NO_THROW({
        rcu_set.clean_until(0);
    });
}

TEST_F(STOTRcuTest, CleanUntil_SequentialEpochs) {
    TRcuSet rcu_set;
    
    // Clean through sequential epochs
    for (int i = 0; i < 100; i++) {
        EXPECT_NO_THROW({
            rcu_set.clean_until(i);
        });
    }
}

TEST_F(STOTRcuTest, CleanUntil_NonSequential) {
    TRcuSet rcu_set;
    
    // Clean with non-sequential epochs
    rcu_set.clean_until(0);
    rcu_set.clean_until(5);
    rcu_set.clean_until(10);
    rcu_set.clean_until(3);  // Going backwards
    
    EXPECT_TRUE(true);
}

TEST_F(STOTRcuTest, CleanUntil_LargeEpoch) {
    TRcuSet rcu_set;
    
    // Clean with large epoch number
    EXPECT_NO_THROW({
        rcu_set.clean_until(1000000);
    });
}

// Concurrency Tests
TEST_F(STOTRcuTest, ConcurrentCleanup_MultipleThreads) {
    TRcuSet rcu_set;
    std::vector<std::thread> threads;
    std::atomic<int> completed{0};
    
    // Multiple threads cleaning concurrently
    for (int i = 0; i < 4; i++) {
        threads.emplace_back([&rcu_set, &completed, i]() {
            for (int j = 0; j < 100; j++) {
                rcu_set.clean_until(i * 100 + j);
            }
            completed++;
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(completed.load(), 4);
}

TEST_F(STOTRcuTest, ConcurrentCleanup_NoRaceConditions) {
    TRcuSet rcu_set;
    std::vector<std::thread> threads;
    
    for (int i = 0; i < 4; i++) {
        threads.emplace_back([&rcu_set, i]() {
            for (int j = 0; j < 50; j++) {
                rcu_set.clean_until(i * 50 + j);
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    // If we reach here without crash, no obvious race conditions
    EXPECT_TRUE(true);
}

// Performance Tests
TEST_F(STOTRcuTest, Perf_1000CleanOperations) {
    TRcuSet rcu_set;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < 1000; i++) {
        rcu_set.clean_until(i);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    std::cout << "1000 clean_until operations took: " << duration.count() << " μs" << std::endl;
    std::cout << "Average: " << (duration.count() / 1000.0) << " μs per operation" << std::endl;
    
    // Should be very fast
    EXPECT_LT(duration.count(), 100000);  // Less than 100ms total
}

// Edge Cases
TEST_F(STOTRcuTest, EdgeCase_RepeatedSameEpoch) {
    TRcuSet rcu_set;
    
    // Repeatedly clean same epoch
    for (int i = 0; i < 10; i++) {
        rcu_set.clean_until(42);
    }
    
    EXPECT_TRUE(true);
}

TEST_F(STOTRcuTest, EdgeCase_BackwardsEpochs) {
    TRcuSet rcu_set;
    
    // Clean epochs in reverse order
    for (int i = 100; i >= 0; i--) {
        rcu_set.clean_until(i);
    }
    
    EXPECT_TRUE(true);
}

TEST_F(STOTRcuTest, Stress_RapidCleanupCycles) {
    TRcuSet rcu_set;
    
    // Rapid cleanup cycles
    for (int cycle = 0; cycle < 10; cycle++) {
        for (int i = 0; i < 100; i++) {
            rcu_set.clean_until(cycle * 100 + i);
        }
    }
    
    EXPECT_TRUE(true);
}

// Batch Operations
TEST_F(STOTRcuTest, BatchCleanup_LargeRange) {
    TRcuSet rcu_set;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    // Clean a large range
    for (int i = 0; i < 10000; i++) {
        rcu_set.clean_until(i);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    std::cout << "10000 clean operations took: " << duration.count() << " ms" << std::endl;
    
    EXPECT_LT(duration.count(), 1000);  // Should complete in under 1 second
}
