/**
 * @file shard_router_test.cc
 * @brief Unit tests for shard router integration.
 */

#include "gtest/gtest.h"
import cluster;   // config/sharding metadata module (was #include "cluster/...")
#include "mako/lib/table_registry.h"
#include "sharding_policy_test_util.h"  // janus::make_table_policy / make_policy_set

namespace mako {

class ShardRouterTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Clear registries before each test
        get_table_registry().clear();
        // Empty the process-global ClusterConfig so routing uses the
        // legacy ShardingPolicyCache path (shard_count 0 disables the
        // ClusterConfig branch). The ClusterConfig routing test opts in
        // explicitly by populating it.
        janus::get_cluster_config().set_shard_count(0);
    }

    void TearDown() override {
        get_table_registry().clear();
        janus::get_cluster_config().set_shard_count(0);
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
    auto policy = janus::make_policy_set(2, {
        janus::make_table_policy("WAREHOUSE", janus::KeyExtractor::by_field(0),
                                 {{0, 5, 0}, {5, 10, 1}}, 0),
        janus::make_table_policy("DISTRICT", janus::KeyExtractor::by_field(0),
                                 {{0, 5, 0}, {5, 10, 1}}, 0),
    });

    policy_cache.set_policy(std::move(policy));

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
    auto policy = janus::make_policy_set(3, {
        janus::make_table_policy("STOCK", janus::KeyExtractor::by_field(0),
                                 {{0, 10, 0}, {10, 20, 1}, {20, 30, 2}}, 0),
    });

    policy_cache.set_policy(std::move(policy));

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
    auto policy = janus::make_policy_set(2, {
        janus::make_table_policy("WAREHOUSE", janus::KeyExtractor::by_field(0),
                                 {{0, 5, 0}, {5, 10, 1}}),
    });

    policy_cache.set_policy(std::move(policy));

    // Unknown table should fall back to table-ID-based routing
    // table_id 1 → (1-1)/200 = 0
    EXPECT_EQ(0, compute_shard_for_key(1, "key"));
}

TEST_F(ShardRouterTest, HasPolicyRouting) {
    auto& policy_cache = janus::get_sharding_policy_cache();

    // Set policy with a unique table name for this test
    auto policy = janus::make_policy_set(2, {
        janus::make_table_policy("HAS_POLICY_TEST_TABLE", janus::KeyExtractor::by_field(0),
                                 {{0, 10, 0}}),
    });

    policy_cache.set_policy(std::move(policy));

    EXPECT_TRUE(has_policy_routing("HAS_POLICY_TEST_TABLE"));
    EXPECT_FALSE(has_policy_routing("NONEXISTENT_TABLE_XYZ"));
}

TEST_F(ShardRouterTest, GetPolicyNumShards) {
    auto& policy_cache = janus::get_sharding_policy_cache();

    // Set policy with 4 shards
    auto policy = janus::make_policy_set(4, {
        janus::make_table_policy("TEST", janus::KeyExtractor::by_field(0),
                                 {{0, 10, 0}}),
    });

    policy_cache.set_policy(std::move(policy));

    EXPECT_EQ(4, get_policy_num_shards());
}

// =============================================================================
// Routing consults the process-global ClusterConfig when populated
// =============================================================================

TEST_F(ShardRouterTest, ComputeShardConsultsClusterConfigWhenPopulated) {
    get_table_registry().register_table(1, "WAREHOUSE");

    auto& cc = janus::get_cluster_config();
    cc.set_shard_count(2);
    janus::ShardInfo s0; s0.id = 0; s0.status = "active"; cc.update_shard(0, s0);
    janus::ShardInfo s1; s1.id = 1; s1.status = "active"; cc.update_shard(1, s1);

    // With ClusterConfig populated, compute_shard_for_key routes through
    // it (hash-mod default here — no per-table policy). Stable + in range.
    const int a = compute_shard_for_key(1, "warehouse/42");
    const int b = compute_shard_for_key(1, "warehouse/42");
    EXPECT_EQ(a, b);
    EXPECT_GE(a, 0);
    EXPECT_LT(a, 2);
}

TEST_F(ShardRouterTest, ComputeShardFollowsDeadShardReplacementViaClusterConfig) {
    get_table_registry().register_table(1, "WAREHOUSE");

    auto& cc = janus::get_cluster_config();
    cc.set_shard_count(2);
    janus::ShardInfo s0; s0.id = 0; s0.status = "active"; cc.update_shard(0, s0);
    janus::ShardInfo s1; s1.id = 1; s1.status = "active"; cc.update_shard(1, s1);

    // Find a key that routes to shard 1.
    std::string probe;
    for (int i = 0; i < 64; ++i) {
        const std::string k = "k" + std::to_string(i);
        if (compute_shard_for_key(1, k) == 1) { probe = k; break; }
    }
    ASSERT_FALSE(probe.empty());

    // Kill shard 1 -> taker 0. The router must reroute the probe key.
    janus::ShardInfo dead; dead.id = 1; dead.status = "dead"; dead.replacement = 0;
    cc.update_shard(1, dead);
    EXPECT_EQ(compute_shard_for_key(1, probe), 0);
}

TEST_F(ShardRouterTest, EmptyClusterConfigFallsBackToLegacyPath) {
    // With an empty ClusterConfig (shard_count 0), routing must use the
    // legacy table-ID fallback, unchanged from before this wiring.
    // Table 1 with no policy -> (1-1)/NUM_TABLES_PER_SHARD == 0.
    EXPECT_EQ(0u, janus::get_cluster_config().get_shard_count());
    EXPECT_EQ(compute_shard_for_key(1, "anykey"),
              (1 - 1) / SHARD_ROUTER_NUM_TABLES_PER_SHARD);
}

}  // namespace mako

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
