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
    auto policy = janus::make_policy_set(3, {
        janus::make_table_policy("STOCK", janus::KeyExtractor::by_field(0),
                                 {{0, 10, 0}, {10, 20, 1}, {20, 30, 2}}, 0),
    });

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
    auto policy = janus::make_policy_set(2, {
        janus::make_table_policy("WAREHOUSE", janus::KeyExtractor::by_field(0),
                                 {{0, 5, 0}, {5, 10, 1}}),
    });

    policy_cache.set_policy(policy);

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

    policy_cache.set_policy(policy);

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

    policy_cache.set_policy(policy);

    EXPECT_EQ(4, get_policy_num_shards());
}

// =============================================================================
// Routing consults the process-global ClusterConfig when populated
// =============================================================================

// The ClusterConfig routes ONLY tables it explicitly governs (map mode + a
// partition table present); everything else keeps its legacy path even on a
// node whose config is populated. The negative half is the regression guard
// for the live incident: the moment the ConfigWatcher populated the config,
// ungoverned (TPC-C) tables were hijacked onto config routing (98% remote
// aborts on that node).
TEST_F(ShardRouterTest, ComputeShardConsultsClusterConfigOnlyForGovernedTables) {
    get_table_registry().register_table(1, "GOVERNED");
    get_table_registry().register_table(300, "UNGOVERNED");

    janus::InMemoryKvStore kv;
    janus::ConfigManager cm(&kv);
    ASSERT_TRUE(cm.add_shard(0, {"s0"}));
    ASSERT_TRUE(cm.add_shard(1, {"s1"}));
    ASSERT_TRUE(cm.set_sharding_mode("map"));
    ASSERT_TRUE(cm.seed_partition("GOVERNED", 1));   // whole keyspace -> shard 1
    ASSERT_TRUE(janus::get_cluster_config().load_from_config_manager(&cm));

    // Governed table: routed by the config's partition table.
    EXPECT_EQ(1, compute_shard_for_key(1, "anykey"));

    // Ungoverned table: falls through to the legacy table-ID fallback,
    // exactly as if the config were empty: (300-1)/NUM_TABLES_PER_SHARD.
    EXPECT_EQ((300 - 1) / SHARD_ROUTER_NUM_TABLES_PER_SHARD,
              compute_shard_for_key(300, "anykey"));
}

// =============================================================================
// Routing aliases: partition governance of per-warehouse physical indexes
// =============================================================================

// TPC-C opens one physical index per warehouse per table ("customer_0",
// "customer_remote_5"); the key bytes carry only the LOCAL warehouse id, so
// the index identity names the partition. The alias registered at open time
// (logical table + fixed encoded-global-warehouse key) lets the partition
// table govern those indexes: routing ignores the op's key bytes entirely.
TEST_F(ShardRouterTest, RoutingAliasGovernsPhysicalIndexesByGlobalWarehouse) {
    auto& reg = get_table_registry();
    // Shard 1's physical index for global warehouse 3 (names never seeded).
    reg.register_table(201, "customer_0");
    reg.register_route(201, "customer", warehouse_route_key(3));
    // Shard 0's remote proxy for the same warehouse: same logical route.
    reg.register_table(7, "customer_remote_3");
    reg.register_route(7, "customer", warehouse_route_key(3));
    // An aliased table whose LOGICAL table is not governed: legacy fallback.
    reg.register_table(9, "stock_remote_3");
    reg.register_route(9, "stock", warehouse_route_key(3));

    janus::InMemoryKvStore kv;
    janus::ConfigManager cm(&kv);
    ASSERT_TRUE(cm.add_shard(0, {"s0"}));
    ASSERT_TRUE(cm.add_shard(1, {"s1"}));
    ASSERT_TRUE(cm.set_sharding_mode("map"));
    // 4 warehouses, 2 shards: [1,2] -> 0, [3,4] -> 1.
    ASSERT_TRUE(seed_warehouse_partitions(&cm, "customer", 4, 2));
    ASSERT_TRUE(janus::get_cluster_config().load_from_config_manager(&cm));

    // Both physical indexes of warehouse 3 route to shard 1 -- REGARDLESS of
    // the op's key bytes (a local-wid key would say warehouse 1).
    EXPECT_EQ(1, compute_shard_for_key(201, std::string("\x00\x00\x00\x01", 4)));
    EXPECT_EQ(1, compute_shard_for_key(7, "unrelated-bytes"));
    // Ungoverned logical table: arithmetic fallback, alias dormant.
    EXPECT_EQ((9 - 1) / SHARD_ROUTER_NUM_TABLES_PER_SHARD,
              compute_shard_for_key(9, "unrelated-bytes"));
}

// A partition-table cutover (the migration commit's publish) flips both the
// physical-index routing AND the workload's warehouse oracle -- no re-open,
// no workload change.
TEST_F(ShardRouterTest, RoutingAliasFollowsWarehouseCutover) {
    auto& reg = get_table_registry();
    reg.register_table(210, "customer_0");   // shard 1's index, global wh 3
    reg.register_route(210, "customer", warehouse_route_key(3));

    janus::InMemoryKvStore kv;
    janus::ConfigManager cm(&kv);
    ASSERT_TRUE(cm.add_shard(0, {"s0"}));
    ASSERT_TRUE(cm.add_shard(1, {"s1"}));
    ASSERT_TRUE(cm.set_sharding_mode("map"));
    ASSERT_TRUE(seed_warehouse_partitions(&cm, "customer", 4, 2));
    ASSERT_TRUE(janus::get_cluster_config().load_from_config_manager(&cm));

    EXPECT_EQ(1, compute_shard_for_key(210, "any"));
    EXPECT_EQ(1, route_shard_for_warehouse("customer", 3));
    EXPECT_EQ(0, route_shard_for_warehouse("customer", 2));

    // Migrate warehouse 3 to shard 0: reassign [wrk(3), wrk(4)) and reload.
    ASSERT_TRUE(janus::cm_split_and_reassign(&cm, "customer",
                                             warehouse_route_key(3),
                                             warehouse_route_key(4), 0));
    ASSERT_TRUE(janus::get_cluster_config().load_from_config_manager(&cm));

    EXPECT_EQ(0, compute_shard_for_key(210, "any"));
    EXPECT_EQ(0, route_shard_for_warehouse("customer", 3));
    // Neighbors keep their owners.
    EXPECT_EQ(0, route_shard_for_warehouse("customer", 2));
    EXPECT_EQ(1, route_shard_for_warehouse("customer", 4));
}

// The seeded layout must reproduce the legacy static layout exactly --
// wps = ceil(nw/ns), shard s owns warehouses [s*wps+1, min((s+1)*wps, nw)] --
// for even, ragged, and degenerate splits. Seeding is also idempotent.
TEST_F(ShardRouterTest, SeedWarehousePartitionsMatchesLegacyLayout) {
    struct Case { int nw; int ns; };
    const Case cases[] = {{4, 2}, {5, 2}, {9, 3}, {1, 2}, {6, 1}};
    for (const auto& c : cases) {
        get_table_registry().clear();
        janus::InMemoryKvStore kv;
        janus::ConfigManager cm(&kv);
        for (int s = 0; s < c.ns; s++)
            ASSERT_TRUE(cm.add_shard(s, {"s" + std::to_string(s)}));
        ASSERT_TRUE(cm.set_sharding_mode("map"));
        ASSERT_TRUE(seed_warehouse_partitions(&cm, "customer", c.nw, c.ns));
        ASSERT_TRUE(janus::get_cluster_config().load_from_config_manager(&cm));

        const int wps = (c.nw + c.ns - 1) / c.ns;
        for (int w = 1; w <= c.nw; w++) {
            EXPECT_EQ((w - 1) / wps, route_shard_for_warehouse("customer", w))
                << "nw=" << c.nw << " ns=" << c.ns << " w=" << w;
        }
    }

    // Idempotence: a re-seed must not clobber a live cutover.
    janus::InMemoryKvStore kv;
    janus::ConfigManager cm(&kv);
    ASSERT_TRUE(cm.add_shard(0, {"s0"}));
    ASSERT_TRUE(cm.add_shard(1, {"s1"}));
    ASSERT_TRUE(cm.set_sharding_mode("map"));
    ASSERT_TRUE(seed_warehouse_partitions(&cm, "customer", 4, 2));
    ASSERT_TRUE(janus::cm_split_and_reassign(&cm, "customer",
                                             warehouse_route_key(3),
                                             warehouse_route_key(4), 0));
    ASSERT_TRUE(seed_warehouse_partitions(&cm, "customer", 4, 2));  // no-op
    ASSERT_TRUE(janus::get_cluster_config().load_from_config_manager(&cm));
    EXPECT_EQ(0, route_shard_for_warehouse("customer", 3));  // cutover survives
}

// Ungoverned (empty config, or populated hash-mode config): the warehouse
// oracle answers -1 and callers keep the legacy static layout.
TEST_F(ShardRouterTest, RouteShardForWarehouseUngovernedReturnsMinusOne) {
    EXPECT_EQ(-1, route_shard_for_warehouse("customer", 3));

    janus::InMemoryKvStore kv;
    janus::ConfigManager cm(&kv);
    ASSERT_TRUE(cm.add_shard(0, {"s0"}));
    ASSERT_TRUE(cm.add_shard(1, {"s1"}));
    ASSERT_TRUE(cm.set_sharding_mode("hash"));   // partition routing not chosen
    ASSERT_TRUE(janus::get_cluster_config().load_from_config_manager(&cm));
    EXPECT_EQ(-1, route_shard_for_warehouse("customer", 3));
}

TEST_F(ShardRouterTest, ComputeShardFollowsDeadShardReplacementViaClusterConfig) {
    get_table_registry().register_table(1, "GOVERNED");

    janus::InMemoryKvStore kv;
    janus::ConfigManager cm(&kv);
    ASSERT_TRUE(cm.add_shard(0, {"s0"}));
    ASSERT_TRUE(cm.add_shard(1, {"s1"}));
    ASSERT_TRUE(cm.set_sharding_mode("map"));
    ASSERT_TRUE(cm.seed_partition("GOVERNED", 1));   // whole keyspace -> shard 1
    ASSERT_TRUE(janus::get_cluster_config().load_from_config_manager(&cm));
    ASSERT_EQ(1, compute_shard_for_key(1, "probe"));

    // Kill shard 1 -> taker 0. The router must chase the replacement pointer
    // for the governed table's partition owner.
    auto& cc = janus::get_cluster_config();
    janus::ShardInfo dead; dead.id = 1; dead.status = "dead"; dead.replacement = 0;
    cc.update_shard(1, dead);
    EXPECT_EQ(compute_shard_for_key(1, "probe"), 0);
}

TEST_F(ShardRouterTest, EmptyClusterConfigFallsBackToLegacyPath) {
    // With an empty ClusterConfig (shard_count 0), routing must use the
    // legacy table-ID fallback, unchanged from before this wiring.
    // Table 1 with no policy -> (1-1)/NUM_TABLES_PER_SHARD == 0.
    EXPECT_EQ(0u, janus::get_cluster_config().get_shard_count());
    EXPECT_EQ(compute_shard_for_key(1, "anykey"),
              (1 - 1) / SHARD_ROUTER_NUM_TABLES_PER_SHARD);
}

// =============================================================================
// End-to-end reroute: a live migration reroutes the REAL runtime routing entry.
// =============================================================================

// A migration through the long-lived ShardMaster reassigns a range in the
// map-mode partition table and reloads the process-global ClusterConfig, so the
// runtime routing entry (compute_shard_for_key -> get_cluster_config) sends the
// migrated range to its new owner. Two in-process participants stand in for the
// shards' data planes; the routing plane is the real one.
TEST_F(ShardRouterTest, MigrationReroutesComputeShardForKey) {
    auto key = [](int i) {
        return std::string("k") + (i < 10 ? "0" : "") + std::to_string(i);
    };

    // Map-mode routing plane: the whole "" keyspace starts on shard 0 (source).
    janus::InMemoryKvStore kv;
    janus::ConfigManager cm(&kv);
    ASSERT_TRUE(cm.add_shard(0, {"s0"}));
    ASSERT_TRUE(cm.add_shard(1, {"s1"}));
    ASSERT_TRUE(cm.set_sharding_mode("map"));
    ASSERT_TRUE(cm.seed_partition("", 0));
    ASSERT_TRUE(janus::get_cluster_config().load_from_config_manager(&cm));

    // An unregistered table_id -> table_name "" -> the "" partition. The real
    // routing entry sends the whole range to shard 0 initially.
    EXPECT_EQ(0, compute_shard_for_key(999, key(15)));
    EXPECT_EQ(0, compute_shard_for_key(999, key(5)));

    // Seed the range's data on shard 0, then migrate [k10,k20) 0 -> 1 through the
    // master, which publishes the cutover to cm and reloads THIS ClusterConfig.
    janus::InMemoryShardData d0, d1;
    for (int i = 0; i < 30; i++) d0.put(key(i), "v");
    janus::ShardMaster master =
        janus::ShardMaster::new_(&cm, &janus::get_cluster_config());
    master.attach_shard(0, &d0);
    master.attach_shard(1, &d1);
    ASSERT_TRUE(master.begin_migration(0, 1, "", key(10), key(20)));
    master.background_copy();
    master.lock_range();
    master.final_sync();
    ASSERT_TRUE(master.both_prepared());
    ASSERT_TRUE(master.commit_migration());

    // The runtime routing entry now reroutes the migrated range to shard 1;
    // neighbors keep routing to shard 0.
    EXPECT_EQ(1, compute_shard_for_key(999, key(15)));  // inside -> destination
    EXPECT_EQ(0, compute_shard_for_key(999, key(5)));   // below -> source
    EXPECT_EQ(0, compute_shard_for_key(999, key(25)));  // above -> source

    // And the data really moved.
    EXPECT_EQ(10u, master.shard_range_count(1, key(10), key(20)));
    EXPECT_EQ(0u, master.shard_range_count(0, key(10), key(20)));
}

}  // namespace mako

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
