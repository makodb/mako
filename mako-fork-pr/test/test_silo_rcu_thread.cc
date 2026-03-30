#include <gtest/gtest.h>
#include "mako/rcu.h"
#include <vector>
#include <thread>
#include <atomic>

// ============================================================================
// SILO RCU TESTS - Testing real RCU memory management
// ============================================================================

class SiloRCUTest : public ::testing::Test {
protected:
    void SetUp() override {
        // RCU is a singleton, already initialized
    }
};

// Test RCU region entry/exit
TEST_F(SiloRCUTest, RCURegion_BasicEnterExit) {
    EXPECT_FALSE(rcu::s_instance.in_rcu_region());
    
    {
        scoped_rcu_region guard;
        EXPECT_TRUE(rcu::s_instance.in_rcu_region());
    }
    
    EXPECT_FALSE(rcu::s_instance.in_rcu_region());
}

// Test nested RCU regions
TEST_F(SiloRCUTest, RCURegion_Nested) {
    {
        scoped_rcu_region guard1;
        EXPECT_TRUE(rcu::s_instance.in_rcu_region());
        
        {
            scoped_rcu_region guard2;
            EXPECT_TRUE(rcu::s_instance.in_rcu_region());
        }
        
        EXPECT_TRUE(rcu::s_instance.in_rcu_region());
    }
    
    EXPECT_FALSE(rcu::s_instance.in_rcu_region());
}

// Test RCU allocation
TEST_F(SiloRCUTest, Allocation_Basic) {
    scoped_rcu_region guard;
    
    void* ptr = rcu::s_instance.alloc(1024);
    ASSERT_NE(ptr, nullptr);
    
    // Write to verify allocation
    char* data = static_cast<char*>(ptr);
    data[0] = 'A';
    data[1023] = 'Z';
    
    EXPECT_EQ(data[0], 'A');
    EXPECT_EQ(data[1023], 'Z');
}

// Test RCU deallocation
TEST_F(SiloRCUTest, Deallocation_Basic) {
    scoped_rcu_region guard;
    
    void* ptr = rcu::s_instance.alloc(512);
    ASSERT_NE(ptr, nullptr);
    
    // Dealloc should not crash
    EXPECT_NO_THROW({
        rcu::s_instance.dealloc(ptr, 512);
    });
}

// Test concurrent RCU regions
TEST_F(SiloRCUTest, ConcurrentRegions_MultipleThreads) {
    std::vector<std::thread> threads;
    std::atomic<int> region_count{0};
    
    for (int i = 0; i < 4; i++) {
        threads.emplace_back([&region_count]() {
            for (int j = 0; j < 100; j++) {
                scoped_rcu_region guard;
                if (rcu::s_instance.in_rcu_region()) {
                    region_count++;
                }
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(region_count.load(), 400);
}

// Test RCU allocation/deallocation patterns
TEST_F(SiloRCUTest, AllocationPattern_MultipleAllocations) {
    scoped_rcu_region guard;
    
    std::vector<void*> ptrs;
    for (int i = 0; i < 50; i++) {
        void* ptr = rcu::s_instance.alloc(128);
        ASSERT_NE(ptr, nullptr);
        ptrs.push_back(ptr);
    }
    
    EXPECT_EQ(ptrs.size(), 50);
    
    // Deallocate all
    for (size_t i = 0; i < ptrs.size(); i++) {
        rcu::s_instance.dealloc(ptrs[i], 128);
    }
}

// Test RCU static allocation
TEST_F(SiloRCUTest, StaticAllocation_Basic) {
    scoped_rcu_region guard;
    
    void* ptr = rcu::s_instance.alloc_static(4096);
    ASSERT_NE(ptr, nullptr);
    
    char* data = static_cast<char*>(ptr);
    data[0] = 'S';
    data[4095] = 'E';
    
    EXPECT_EQ(data[0], 'S');
    EXPECT_EQ(data[4095], 'E');
}

// Test RCU cleanup
TEST_F(SiloRCUTest, Cleanup_DoCleanup) {
    {
        scoped_rcu_region guard;
        void* ptr = rcu::s_instance.alloc(256);
        rcu::s_instance.dealloc(ptr, 256);
    }
    
    // Cleanup happens on region exit
    EXPECT_NO_THROW({
        rcu::s_instance.do_cleanup();
    });
}

// Test RCU performance
TEST_F(SiloRCUTest, Performance_1000Allocations) {
    scoped_rcu_region guard;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    std::vector<void*> ptrs;
    for (int i = 0; i < 1000; i++) {
        void* ptr = rcu::s_instance.alloc(64);
        if (ptr) ptrs.push_back(ptr);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    std::cout << "1000 RCU allocations took: " << duration.count() << " μs" << std::endl;
    std::cout << "Average: " << (duration.count() / 1000.0) << " μs per allocation" << std::endl;
    
    EXPECT_EQ(ptrs.size(), 1000);
    
    // Cleanup
    for (auto* ptr : ptrs) {
        rcu::s_instance.dealloc(ptr, 64);
    }
}
