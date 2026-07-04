/**
 * @file shard_router_test.cc
 * @brief Unit tests for shard router integration.
 */

#include "gtest/gtest.h"
#include "cluster/shard_router.h"
#include "mako/lib/table_registry.h"
#include "cluster/sharding_policy_cache.h"
#include "cluster/sharding_policy_builder.h"

namespace mako {

class ShardRouterTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Clear registries before each test
        get_table_registry().clear();
    }

    void TearDown() override {
        // Clear registries after each test
        get_table_registry().clear();
    }
};

// =============================================================================
// Table Registry Tests
// =============================================================================

TEST_F(ShardRouterTest, TableRegistryRegisterAndLookup) {
    auto& registry = get_table_registry();

    registry.register_table(1, "WAREHOUSE");
    registry.register_table(2, "DISTRICT");
    registry.register_table(201, "WAREHOUSE");  // Same table on different shard

    auto name1 = registry.get_table_name(1);
    ASSERT_TRUE(name1.is_some());
    EXPECT_EQ("WAREHOUSE", name1.unwrap());

    auto name2 = registry.get_table_name(2);
    ASSERT_TRUE(name2.is_some());
    EXPECT_EQ("DISTRICT", name2.unwrap());

    auto name201 = registry.get_table_name(201);
    ASSERT_TRUE(name201.is_some());
    EXPECT_EQ("WAREHOUSE", name201.unwrap());

    // Unknown table
    auto unknown = registry.get_table_name(999);
    EXPECT_TRUE(unknown.is_none());
}

TEST_F(ShardRouterTest, TableRegistryGetTableId) {
    auto& registry = get_table_registry();

    registry.register_table(1, "WAREHOUSE");
    registry.register_table(201, "WAREHOUSE");  // Same name, different ID

    // Should return first registered ID
    auto id = registry.get_table_id("WAREHOUSE");
    ASSERT_TRUE(id.is_some());
    EXPECT_EQ(1, id.unwrap());

    // Unknown table
    auto unknown = registry.get_table_id("UNKNOWN");
    EXPECT_TRUE(unknown.is_none());
}

TEST_F(ShardRouterTest, TableRegistryHasTable) {
    auto& registry = get_table_registry();

    EXPECT_FALSE(registry.has_table(1));

    registry.register_table(1, "WAREHOUSE");

    EXPECT_TRUE(registry.has_table(1));
    EXPECT_FALSE(registry.has_table(2));
}

TEST_F(ShardRouterTest, TableRegistryClear) {
    auto& registry = get_table_registry();

    registry.register_table(1, "WAREHOUSE");
    EXPECT_EQ(1u, registry.size());

    registry.clear();
    EXPECT_EQ(0u, registry.size());
    EXPECT_FALSE(registry.has_table(1));
}

// =============================================================================
// Shard Router Tests - Table-ID Based (No Policy)
// =============================================================================

TEST_F(ShardRouterTest, ComputeShardFallbackToTableId) {
    // Without policy, should fall back to table-ID-based routing
    // Formula: (table_id - 1) / NUM_TABLES_PER_SHARD
    // NUM_TABLES_PER_SHARD = 200

    // Table IDs 1-200 should map to shard 0
    EXPECT_EQ(0, compute_shard_for_key(1, "key"));
    EXPECT_EQ(0, compute_shard_for_key(100, "key"));
    EXPECT_EQ(0, compute_shard_for_key(200, "key"));

    // Table IDs 201-400 should map to shard 1
    EXPECT_EQ(1, compute_shard_for_key(201, "key"));
    EXPECT_EQ(1, compute_shard_for_key(300, "key"));
    EXPECT_EQ(1, compute_shard_for_key(400, "key"));

    // Table IDs 401-600 should map to shard 2
    EXPECT_EQ(2, compute_shard_for_key(401, "key"));
    EXPECT_EQ(2, compute_shard_for_key(500, "key"));
    EXPECT_EQ(2, compute_shard_for_key(600, "key"));
}

// =============================================================================
// Shard Router Tests - Policy Based
// =============================================================================

TEST_F(ShardRouterTest, ComputeShardWithPolicy) {
    auto& registry = get_table_registry();
    auto& policy_cache = janus::get_sharding_policy_cache();

    // Register tables
    registry.register_table(1, "WAREHOUSE");
    registry.register_table(2, "DISTRICT");

    // Create and set policy: 10 warehouses across 2 shards
    // w_id 0-4 → shard 0, w_id 5-9 → shard 1
    auto policy = janus::ShardingPolicyBuilder(2)
        .table("WAREHOUSE")
            .shardByField(0)
            .addRange(0, 5, 0)
            .addRange(5, 10, 1)
            .defaultShard(0)
        .table("DISTRICT")
            .shardByField(0)
            .addRange(0, 5, 0)
            .addRange(5, 10, 1)
            .defaultShard(0)
        .build();

    policy_cache.set_policy(policy);

    // With policy, routing should be based on key value
    // Key bytes are interpreted as big-endian int64

    // Key with value 0 → shard 0
    char key0[] = {0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_EQ(0, compute_shard_for_key(1, std::string(key0, 8)));

    // Key with value 3 → shard 0
    char key3[] = {0, 0, 0, 0, 0, 0, 0, 3};
    EXPECT_EQ(0, compute_shard_for_key(1, std::string(key3, 8)));

    // Key with value 5 → shard 1
    char key5[] = {0, 0, 0, 0, 0, 0, 0, 5};
    EXPECT_EQ(1, compute_shard_for_key(1, std::string(key5, 8)));

    // Key with value 7 → shard 1
    char key7[] = {0, 0, 0, 0, 0, 0, 0, 7};
    EXPECT_EQ(1, compute_shard_for_key(1, std::string(key7, 8)));

    // Clean up - reset policy cache (create new empty policy)
    // Note: There's no clear method, so we test is_initialized
    EXPECT_TRUE(policy_cache.is_initialized());
}

TEST_F(ShardRouterTest, ComputeShardWithPolicyKeyValue) {
    auto& policy_cache = janus::get_sharding_policy_cache();

    // Create and set policy
    auto policy = janus::ShardingPolicyBuilder(3)
        .table("STOCK")
            .shardByField(0)
            .addRange(0, 10, 0)
            .addRange(10, 20, 1)
            .addRange(20, 30, 2)
            .defaultShard(0)
        .build();

    policy_cache.set_policy(policy);

    // Using explicit key value
    EXPECT_EQ(0, compute_shard_for_key_value(1, "STOCK", 5));
    EXPECT_EQ(1, compute_shard_for_key_value(1, "STOCK", 15));
    EXPECT_EQ(2, compute_shard_for_key_value(1, "STOCK", 25));

    // Default shard for out-of-range
    EXPECT_EQ(0, compute_shard_for_key_value(1, "STOCK", 100));
}

TEST_F(ShardRouterTest, ComputeShardUnknownTableFallsBack) {
    auto& registry = get_table_registry();
    auto& policy_cache = janus::get_sharding_policy_cache();

    // Register table but with different name than in policy
    registry.register_table(1, "UNKNOWN_TABLE");

    // Create policy for WAREHOUSE only
    auto policy = janus::ShardingPolicyBuilder(2)
        .table("WAREHOUSE")
            .shardByField(0)
            .addRange(0, 5, 0)
            .addRange(5, 10, 1)
        .build();

    policy_cache.set_policy(policy);

    // Unknown table should fall back to table-ID-based routing
    // table_id 1 → (1-1)/200 = 0
    EXPECT_EQ(0, compute_shard_for_key(1, "key"));
}

TEST_F(ShardRouterTest, HasPolicyRouting) {
    auto& policy_cache = janus::get_sharding_policy_cache();

    // Set policy with a unique table name for this test
    auto policy = janus::ShardingPolicyBuilder(2)
        .table("HAS_POLICY_TEST_TABLE")
            .shardByField(0)
            .addRange(0, 10, 0)
        .build();

    policy_cache.set_policy(policy);

    EXPECT_TRUE(has_policy_routing("HAS_POLICY_TEST_TABLE"));
    EXPECT_FALSE(has_policy_routing("NONEXISTENT_TABLE_XYZ"));
}

TEST_F(ShardRouterTest, GetPolicyNumShards) {
    auto& policy_cache = janus::get_sharding_policy_cache();

    // Set policy with 4 shards
    auto policy = janus::ShardingPolicyBuilder(4)
        .table("TEST")
            .shardByField(0)
            .addRange(0, 10, 0)
        .build();

    policy_cache.set_policy(policy);

    EXPECT_EQ(4, get_policy_num_shards());
}

}  // namespace mako

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
