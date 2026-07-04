/**
 * @file tpcc_sharding_test.cc
 * @brief Unit tests for TPC-C sharding policy initialization.
 */

#include <math.h>

#include <gtest/gtest.h>
#include "mako/benchmarks/tpcc_sharding.h"
#include "cluster/sharding_policy_cache.h"

import std;

namespace mako {

class TpccShardingTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Clear any previous state from the singleton cache
        janus::get_sharding_policy_cache().clear();
    }

    void TearDown() override {
        // Clean up after test
        janus::get_sharding_policy_cache().clear();
    }
};

// =============================================================================
// Initialize TPC-C Sharding Policy Tests
// =============================================================================

TEST_F(TpccShardingTest, InitializeWithValidParams) {
    // 10 warehouses across 2 shards
    EXPECT_TRUE(initialize_tpcc_sharding_policy(10, 2));
    EXPECT_TRUE(is_tpcc_sharding_initialized());
}

TEST_F(TpccShardingTest, InitializeWithSingleWarehouse) {
    // Single warehouse, single shard
    EXPECT_TRUE(initialize_tpcc_sharding_policy(1, 1));
    EXPECT_TRUE(is_tpcc_sharding_initialized());
}

TEST_F(TpccShardingTest, InitializeWithManyShards) {
    // 100 warehouses across 10 shards
    EXPECT_TRUE(initialize_tpcc_sharding_policy(100, 10));
    EXPECT_TRUE(is_tpcc_sharding_initialized());
}

TEST_F(TpccShardingTest, InitializeWithInvalidParams) {
    // Invalid: zero warehouses
    EXPECT_FALSE(initialize_tpcc_sharding_policy(0, 2));
    EXPECT_FALSE(is_tpcc_sharding_initialized());
}

TEST_F(TpccShardingTest, InitializeWithZeroShards) {
    // Invalid: zero shards
    EXPECT_FALSE(initialize_tpcc_sharding_policy(10, 0));
    EXPECT_FALSE(is_tpcc_sharding_initialized());
}

TEST_F(TpccShardingTest, InitializeWithNegativeParams) {
    // Invalid: negative warehouses
    EXPECT_FALSE(initialize_tpcc_sharding_policy(-5, 2));
    EXPECT_FALSE(is_tpcc_sharding_initialized());

    // Invalid: negative shards
    EXPECT_FALSE(initialize_tpcc_sharding_policy(10, -3));
    EXPECT_FALSE(is_tpcc_sharding_initialized());
}

// =============================================================================
// Is Initialized Tests
// =============================================================================

TEST_F(TpccShardingTest, NotInitializedByDefault) {
    // Should not be initialized initially
    EXPECT_FALSE(is_tpcc_sharding_initialized());
}

TEST_F(TpccShardingTest, IsInitializedAfterInit) {
    EXPECT_FALSE(is_tpcc_sharding_initialized());
    initialize_tpcc_sharding_policy(10, 2);
    EXPECT_TRUE(is_tpcc_sharding_initialized());
}

// =============================================================================
// Get Shard For Warehouse Tests
// =============================================================================

TEST_F(TpccShardingTest, GetShardForWarehouseNotInitialized) {
    // Should return -1 when not initialized
    EXPECT_EQ(-1, get_shard_for_warehouse(1));
    EXPECT_EQ(-1, get_shard_for_warehouse(5));
}

TEST_F(TpccShardingTest, GetShardForWarehouseEvenDistribution) {
    // 10 warehouses across 2 shards (5 per shard)
    // Shard 0: w_id 1-5
    // Shard 1: w_id 6-10
    EXPECT_TRUE(initialize_tpcc_sharding_policy(10, 2));

    // First shard
    EXPECT_EQ(0, get_shard_for_warehouse(1));
    EXPECT_EQ(0, get_shard_for_warehouse(2));
    EXPECT_EQ(0, get_shard_for_warehouse(3));
    EXPECT_EQ(0, get_shard_for_warehouse(4));
    EXPECT_EQ(0, get_shard_for_warehouse(5));

    // Second shard
    EXPECT_EQ(1, get_shard_for_warehouse(6));
    EXPECT_EQ(1, get_shard_for_warehouse(7));
    EXPECT_EQ(1, get_shard_for_warehouse(8));
    EXPECT_EQ(1, get_shard_for_warehouse(9));
    EXPECT_EQ(1, get_shard_for_warehouse(10));
}

TEST_F(TpccShardingTest, GetShardForWarehouseUnevenDistribution) {
    // 7 warehouses across 3 shards (3, 3, 1 distribution)
    // warehouses_per_shard = ceil(7/3) = 3
    // Shard 0: w_id 1-3
    // Shard 1: w_id 4-6
    // Shard 2: w_id 7
    EXPECT_TRUE(initialize_tpcc_sharding_policy(7, 3));

    // First shard
    EXPECT_EQ(0, get_shard_for_warehouse(1));
    EXPECT_EQ(0, get_shard_for_warehouse(2));
    EXPECT_EQ(0, get_shard_for_warehouse(3));

    // Second shard
    EXPECT_EQ(1, get_shard_for_warehouse(4));
    EXPECT_EQ(1, get_shard_for_warehouse(5));
    EXPECT_EQ(1, get_shard_for_warehouse(6));

    // Third shard (only 1 warehouse)
    EXPECT_EQ(2, get_shard_for_warehouse(7));
}

TEST_F(TpccShardingTest, GetShardForWarehouseSingleShard) {
    // All warehouses on single shard
    EXPECT_TRUE(initialize_tpcc_sharding_policy(5, 1));

    for (int w_id = 1; w_id <= 5; ++w_id) {
        EXPECT_EQ(0, get_shard_for_warehouse(w_id));
    }
}

TEST_F(TpccShardingTest, GetShardForWarehouseOutOfRange) {
    // 10 warehouses across 2 shards
    EXPECT_TRUE(initialize_tpcc_sharding_policy(10, 2));

    // Out of range warehouse ID should return default shard (0)
    EXPECT_EQ(0, get_shard_for_warehouse(0));   // Invalid: w_id starts at 1
    EXPECT_EQ(0, get_shard_for_warehouse(11));  // Out of range
    EXPECT_EQ(0, get_shard_for_warehouse(100)); // Way out of range
}

// =============================================================================
// Print Policy Tests
// =============================================================================

TEST_F(TpccShardingTest, PrintPolicyNotInitialized) {
    std::ostringstream out;
    print_tpcc_sharding_policy(out);
    EXPECT_NE(std::string::npos, out.str().find("Not initialized"));
}

TEST_F(TpccShardingTest, PrintPolicyInitialized) {
    EXPECT_TRUE(initialize_tpcc_sharding_policy(10, 2));

    std::ostringstream out;
    print_tpcc_sharding_policy(out);

    // Should contain policy info
    std::string output = out.str();
    EXPECT_NE(std::string::npos, output.find("TPC-C Sharding Policy"));
    EXPECT_NE(std::string::npos, output.find("Num Shards: 2"));
    EXPECT_NE(std::string::npos, output.find("w_id="));
    EXPECT_NE(std::string::npos, output.find("shard"));
}

// =============================================================================
// Cache Interaction Tests
// =============================================================================

TEST_F(TpccShardingTest, PolicyCacheVersion) {
    EXPECT_TRUE(initialize_tpcc_sharding_policy(10, 2));

    auto& cache = janus::get_sharding_policy_cache();
    // Version should be non-zero (timestamp-based)
    EXPECT_GT(cache.get_version(), 0u);
}

TEST_F(TpccShardingTest, PolicyCacheTables) {
    EXPECT_TRUE(initialize_tpcc_sharding_policy(10, 2));

    auto& cache = janus::get_sharding_policy_cache();

    // Should have policies for all TPC-C tables
    EXPECT_TRUE(cache.has_policy_for_table("WAREHOUSE"));
    EXPECT_TRUE(cache.has_policy_for_table("DISTRICT"));
    EXPECT_TRUE(cache.has_policy_for_table("CUSTOMER"));
    EXPECT_TRUE(cache.has_policy_for_table("STOCK"));
    EXPECT_TRUE(cache.has_policy_for_table("ORDER"));
    EXPECT_TRUE(cache.has_policy_for_table("NEW_ORDER"));
    EXPECT_TRUE(cache.has_policy_for_table("ORDER_LINE"));
    EXPECT_TRUE(cache.has_policy_for_table("HISTORY"));
    EXPECT_TRUE(cache.has_policy_for_table("ITEM"));

    // Also lowercase versions
    EXPECT_TRUE(cache.has_policy_for_table("warehouse"));
    EXPECT_TRUE(cache.has_policy_for_table("district"));
    EXPECT_TRUE(cache.has_policy_for_table("customer"));
}

TEST_F(TpccShardingTest, PolicyCacheConsistentRouting) {
    EXPECT_TRUE(initialize_tpcc_sharding_policy(10, 2));

    auto& cache = janus::get_sharding_policy_cache();

    // All tables should route the same w_id to the same shard
    for (int w_id = 1; w_id <= 10; ++w_id) {
        int expected_shard = (w_id <= 5) ? 0 : 1;

        EXPECT_EQ(expected_shard, cache.get_shard_for_key("WAREHOUSE", w_id));
        EXPECT_EQ(expected_shard, cache.get_shard_for_key("DISTRICT", w_id));
        EXPECT_EQ(expected_shard, cache.get_shard_for_key("CUSTOMER", w_id));
        EXPECT_EQ(expected_shard, cache.get_shard_for_key("STOCK", w_id));
        EXPECT_EQ(expected_shard, cache.get_shard_for_key("ORDER", w_id));
    }
}

}  // namespace mako

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
