#include <stddef.h>
#include <stdlib.h>

#if defined(__linux__)
#include <numa.h>
#endif

#include <gtest/gtest.h>
#include "mako/rcu.h"

import std;

#if defined(__linux__)
namespace {

struct NumaCpuMaskDeleter {
    void operator()(bitmask* mask) const {
        if (mask != nullptr) {
            numa_free_cpumask(mask);
        }
    }
};

using NumaCpuMask = std::unique_ptr<bitmask, NumaCpuMaskDeleter>;

NumaCpuMask allocate_cpu_mask() {
    return NumaCpuMask(numa_allocate_cpumask());
}

struct AffinityProbeResult {
    std::string skip_reason;
    std::string error;
    bool singleton_pin_is_singleton{false};
    bool cross_node_available{false};
    bool first_cross_node_pin_is_singleton{false};
    bool second_cross_node_pin_is_singleton{false};
    unsigned int first_cpu{0};
    unsigned int second_cpu{0};
    int first_node{-1};
    int second_node{-1};
};

bool is_singleton_cpu(const bitmask* mask, unsigned int cpu) {
    return numa_bitmask_weight(mask) == 1
        && numa_bitmask_isbitset(mask, cpu);
}

AffinityProbeResult probe_affinity_pinning() {
    AffinityProbeResult result;
    std::thread worker([&result]() {
        if (numa_available() < 0) {
            result.skip_reason = "libnuma reports no NUMA support";
            return;
        }

        auto original = allocate_cpu_mask();
        auto requested = allocate_cpu_mask();
        auto observed = allocate_cpu_mask();
        if (!original || !requested || !observed) {
            result.error = "libnuma could not allocate a CPU mask";
            return;
        }
        if (numa_sched_getaffinity(0, original.get()) < 0) {
            result.error = "libnuma could not read the child thread affinity";
            return;
        }

        std::optional<std::pair<unsigned int, int>> first;
        std::optional<std::pair<unsigned int, int>> second;
        const auto max_cpu = static_cast<unsigned long>(
            std::numeric_limits<int>::max());
        for (unsigned long cpu = 0;
             cpu < original->size && cpu <= max_cpu; ++cpu) {
            const auto cpu_id = static_cast<unsigned int>(cpu);
            if (!numa_bitmask_isbitset(original.get(), cpu_id)) {
                continue;
            }
            const int node = numa_node_of_cpu(static_cast<int>(cpu));
            if (node < 0) {
                continue;
            }
            if (!first) {
                first = std::pair(cpu_id, node);
            } else if (node != first->second) {
                second = std::pair(cpu_id, node);
                break;
            }
        }
        if (!first) {
            result.skip_reason =
                "the child affinity has no CPU with a NUMA-node mapping";
            return;
        }
        result.first_cpu = first->first;
        result.first_node = first->second;
        if (second) {
            result.cross_node_available = true;
            result.second_cpu = second->first;
            result.second_node = second->second;
        }

        numa_bitmask_clearall(requested.get());
        numa_bitmask_setbit(requested.get(), result.first_cpu);
        if (numa_sched_setaffinity(0, requested.get()) < 0) {
            result.error = "libnuma could not install the singleton affinity";
            return;
        }

        rcu::s_instance.pin_current_thread(result.first_cpu);
        if (numa_sched_getaffinity(0, observed.get()) < 0) {
            result.error = "libnuma could not read affinity after the RCU pin";
        } else {
            result.singleton_pin_is_singleton =
                is_singleton_cpu(observed.get(), result.first_cpu);
        }

        if (result.error.empty() && second) {
            numa_bitmask_clearall(requested.get());
            numa_bitmask_setbit(requested.get(), result.first_cpu);
            numa_bitmask_setbit(requested.get(), result.second_cpu);
            if (numa_sched_setaffinity(0, requested.get()) < 0) {
                result.error =
                    "libnuma could not install the cross-node baseline";
            } else {
                rcu::s_instance.pin_current_thread(result.first_cpu);
                if (numa_sched_getaffinity(0, observed.get()) < 0) {
                    result.error =
                        "libnuma could not read the first cross-node pin";
                } else {
                    result.first_cross_node_pin_is_singleton =
                        is_singleton_cpu(observed.get(), result.first_cpu);
                    rcu::s_instance.pin_current_thread(result.second_cpu);
                    if (numa_sched_getaffinity(0, observed.get()) < 0) {
                        result.error =
                            "libnuma could not read the second cross-node pin";
                    } else {
                        result.second_cross_node_pin_is_singleton =
                            is_singleton_cpu(observed.get(), result.second_cpu);
                    }
                }
            }
        }

        if (numa_sched_setaffinity(0, original.get()) < 0) {
            if (!result.error.empty()) {
                result.error += "; ";
            }
            result.error += "libnuma could not restore the child affinity";
        }
    });
    worker.join();
    return result;
}

const AffinityProbeResult& affinity_probe_result() {
    static const AffinityProbeResult result = probe_affinity_pinning();
    return result;
}

}  // namespace
#endif

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

#if defined(__linux__)
TEST_F(SiloRCUTest, PinCurrentThreadDoesNotWidenCallerAffinity) {
    const auto& result = affinity_probe_result();
    if (!result.skip_reason.empty()) {
        GTEST_SKIP() << result.skip_reason;
    }
    ASSERT_TRUE(result.error.empty()) << result.error;
    ASSERT_TRUE(result.singleton_pin_is_singleton)
        << "pinning CPU " << result.first_cpu << " on NUMA node "
        << result.first_node << " widened the child thread affinity";
}

TEST_F(SiloRCUTest, RepeatedPinCanMoveAcrossPreservedAffinity) {
    const auto& result = affinity_probe_result();
    if (!result.skip_reason.empty()) {
        GTEST_SKIP() << result.skip_reason;
    }
    ASSERT_TRUE(result.error.empty()) << result.error;
    if (!result.cross_node_available) {
        GTEST_SKIP() << "the child affinity does not span two NUMA nodes";
    }
    EXPECT_TRUE(result.first_cross_node_pin_is_singleton)
        << "the first pin did not narrow the two-node baseline to CPU "
        << result.first_cpu << " on NUMA node " << result.first_node;
    EXPECT_TRUE(result.second_cross_node_pin_is_singleton)
        << "the repeated pin did not move from NUMA node "
        << result.first_node << " to CPU " << result.second_cpu
        << " on NUMA node " << result.second_node;
}
#endif

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
