/**
 * @file config_service_test.cc
 * @brief Unit tests for ConfigService RPC interface.
 */

#include <stdint.h>

#include <gtest/gtest.h>
#include "../src/deptran/config_service.h"
#include "../src/cluster/config_store.h"
#include "../src/deptran/config_schema.h"
#include "../src/cluster/sharding_policy.h"
#include "../src/cluster/sharding_policy_builder.h"

import std;

namespace janus {
namespace test {

namespace fs = std::filesystem;

// Test fixture for ConfigService tests
class ConfigServiceTest : public ::testing::Test {
protected:
    std::string test_db_path_;
    ConfigStore* store_ = nullptr;
    ConfigServiceImpl* service_ = nullptr;

    void SetUp() override {
        // Create unique temp directory for test database
        const char* tmpdir = std::getenv("TMPDIR");
        if (tmpdir == nullptr) {
            tmpdir = "/tmp";
        }
        test_db_path_ = std::string(tmpdir) + "/config_service_test_";
        test_db_path_ += std::to_string(::testing::UnitTest::GetInstance()->random_seed());
        test_db_path_ += "_" + std::to_string(reinterpret_cast<uintptr_t>(this));

        // Clean up from previous runs
        fs::remove_all(test_db_path_);

        // Create and open store
        store_ = new ConfigStore(test_db_path_);
        ASSERT_TRUE(store_->open());

        // Create service
        service_ = new ConfigServiceImpl(*store_);
    }

    void TearDown() override {
        if (service_) {
            delete service_;
            service_ = nullptr;
        }
        if (store_) {
            store_->close();
            delete store_;
            store_ = nullptr;
        }
        fs::remove_all(test_db_path_);
    }

    // Helper to create sample config
    // @safe
    PersistentConfig create_sample_config(uint64_t version = 1) {
        PersistentConfig config;
        config.version = version;

        PersistentSiteInfo site1;
        site1.id = 1;
        site1.locale_id = 10;
        site1.name = "server1";
        site1.proc_name = "proc1";
        site1.role = 0;
        site1.host = "192.168.1.1";
        site1.port = 8080;
        site1.n_thread = 4;
        site1.type = 1;
        site1.partition_id = 0;

        config.sites = {site1};

        PersistentReplicaGroup group1;
        group1.partition_id = 0;
        group1.replica_ids = {1};
        config.replica_groups = {group1};

        config.settings.tx_proto = 5;
        config.settings.replica_proto = 2;
        config.settings.benchmark = 1;

        return config;
    }
};

// Test HasConfig when no config exists
// @unsafe - RocksDB I/O
TEST_F(ConfigServiceTest, HasConfigEmpty) {
    EXPECT_FALSE(store_->has_config());
}

// Test HasConfig when config exists
// @unsafe - RocksDB I/O
TEST_F(ConfigServiceTest, HasConfigWithData) {
    // Save a config
    PersistentConfig config = create_sample_config(42);
    EXPECT_TRUE(store_->save(config));

    // Service should now report config exists
    EXPECT_TRUE(store_->has_config());
}

// Test GetConfigVersion when no config exists
// @unsafe - RocksDB I/O
TEST_F(ConfigServiceTest, GetVersionEmpty) {
    EXPECT_EQ(0u, store_->get_version());
}

// Test GetConfigVersion when config exists
// @unsafe - RocksDB I/O
TEST_F(ConfigServiceTest, GetVersionWithData) {
    PersistentConfig config = create_sample_config(123);
    EXPECT_TRUE(store_->save(config));
    EXPECT_EQ(123u, store_->get_version());
}

// Test serialize_config helper produces valid data
// @unsafe - Marshal I/O
TEST_F(ConfigServiceTest, SerializeConfigRoundtrip) {
    PersistentConfig original = create_sample_config(42);
    EXPECT_TRUE(store_->save(original));

    // Load and verify
    auto loaded_opt = store_->load();
    EXPECT_TRUE(loaded_opt.is_some());

    PersistentConfig loaded = loaded_opt.unwrap();
    EXPECT_EQ(original.version, loaded.version);
    EXPECT_EQ(original.sites.size(), loaded.sites.size());
    EXPECT_EQ(original.sites[0].id, loaded.sites[0].id);
    EXPECT_EQ(original.sites[0].name, loaded.sites[0].name);
    EXPECT_EQ(original.sites[0].host, loaded.sites[0].host);
}

// Test cache invalidation
// @safe
TEST_F(ConfigServiceTest, CacheInvalidation) {
    // Save initial config
    PersistentConfig config1 = create_sample_config(1);
    EXPECT_TRUE(store_->save(config1));

    // First access should load from store
    EXPECT_EQ(1u, store_->get_version());

    // Save new config
    PersistentConfig config2 = create_sample_config(2);
    EXPECT_TRUE(store_->save(config2));

    // Invalidate cache
    service_->invalidate_cache();

    // Should see new version
    EXPECT_EQ(2u, store_->get_version());
}

// Test multiple config updates
// @unsafe - RocksDB I/O
TEST_F(ConfigServiceTest, MultipleUpdates) {
    for (uint64_t v = 1; v <= 5; ++v) {
        PersistentConfig config = create_sample_config(v);
        EXPECT_TRUE(store_->save(config));
        EXPECT_EQ(v, store_->get_version());

        // Invalidate cache between updates
        service_->invalidate_cache();
    }
}

// Test config data deserialization
// @unsafe - Marshal I/O
TEST_F(ConfigServiceTest, ConfigDataDeserialization) {
    PersistentConfig original = create_sample_config(42);
    original.sites[0].name = "test_server";
    original.sites[0].host = "10.0.0.1";
    original.sites[0].port = 9999;

    EXPECT_TRUE(store_->save(original));

    auto loaded_opt = store_->load();
    EXPECT_TRUE(loaded_opt.is_some());

    PersistentConfig loaded = loaded_opt.unwrap();
    EXPECT_EQ("test_server", loaded.sites[0].name);
    EXPECT_EQ("10.0.0.1", loaded.sites[0].host);
    EXPECT_EQ(9999u, loaded.sites[0].port);
}

// Test empty config sites
// @unsafe - RocksDB I/O
TEST_F(ConfigServiceTest, EmptyConfigSites) {
    PersistentConfig config;
    config.version = 1;
    // Empty sites and replica_groups

    EXPECT_TRUE(store_->save(config));

    auto loaded_opt = store_->load();
    EXPECT_TRUE(loaded_opt.is_some());

    PersistentConfig loaded = loaded_opt.unwrap();
    EXPECT_EQ(1u, loaded.version);
    EXPECT_TRUE(loaded.sites.empty());
    EXPECT_TRUE(loaded.replica_groups.empty());
}

// Test multiple sites
// @unsafe - RocksDB I/O
TEST_F(ConfigServiceTest, MultipleSites) {
    PersistentConfig config;
    config.version = 10;

    for (uint32_t i = 0; i < 5; ++i) {
        PersistentSiteInfo site;
        site.id = i;
        site.name = "server" + std::to_string(i);
        site.host = "192.168.1." + std::to_string(i + 1);
        site.port = 8080 + i;
        site.type = 1;
        config.sites.push_back(site);
    }

    for (uint32_t i = 0; i < 3; ++i) {
        PersistentReplicaGroup group;
        group.partition_id = i;
        group.replica_ids = {i, (i + 1) % 5};
        config.replica_groups.push_back(group);
    }

    EXPECT_TRUE(store_->save(config));

    auto loaded_opt = store_->load();
    EXPECT_TRUE(loaded_opt.is_some());

    PersistentConfig loaded = loaded_opt.unwrap();
    EXPECT_EQ(5u, loaded.sites.size());
    EXPECT_EQ(3u, loaded.replica_groups.size());

    for (uint32_t i = 0; i < 5; ++i) {
        EXPECT_EQ(i, loaded.sites[i].id);
        EXPECT_EQ("server" + std::to_string(i), loaded.sites[i].name);
    }
}

// Test protocol settings persistence
// @unsafe - RocksDB I/O
TEST_F(ConfigServiceTest, ProtocolSettingsPersistence) {
    PersistentConfig config = create_sample_config(1);
    config.settings.tx_proto = 7;
    config.settings.replica_proto = 3;
    config.settings.benchmark = 5;
    config.settings.txn_timeout_us = 60000000;
    config.settings.scale_factor = 4;

    EXPECT_TRUE(store_->save(config));

    auto loaded_opt = store_->load();
    EXPECT_TRUE(loaded_opt.is_some());

    PersistentConfig loaded = loaded_opt.unwrap();
    EXPECT_EQ(7, loaded.settings.tx_proto);
    EXPECT_EQ(3, loaded.settings.replica_proto);
    EXPECT_EQ(5, loaded.settings.benchmark);
    EXPECT_EQ(60000000u, loaded.settings.txn_timeout_us);
    EXPECT_EQ(4u, loaded.settings.scale_factor);
}

// ============================================================================
// Sharding Policy Tests
// ============================================================================

// Test HasShardingPolicy when no policy exists
// @unsafe - RocksDB I/O
TEST_F(ConfigServiceTest, HasShardingPolicyEmpty) {
    EXPECT_FALSE(store_->has_sharding_policy());
}

// Test HasShardingPolicy when policy exists
// @unsafe - RocksDB I/O
TEST_F(ConfigServiceTest, HasShardingPolicyWithData) {
    auto policy = ShardingPolicyBuilder(2)
        .table("TEST")
            .shardByField(0)
            .addRange(0, 50, 0)
            .addRange(50, 100, 1)
        .build();

    EXPECT_TRUE(store_->save_sharding_policy(policy));
    EXPECT_TRUE(store_->has_sharding_policy());
}

// Test GetShardingPolicyVersion when no policy exists
// @unsafe - RocksDB I/O
TEST_F(ConfigServiceTest, GetShardingPolicyVersionEmpty) {
    EXPECT_EQ(0u, store_->get_sharding_policy_version());
}

// Test GetShardingPolicyVersion when policy exists
// @unsafe - RocksDB I/O
TEST_F(ConfigServiceTest, GetShardingPolicyVersionWithData) {
    auto policy = ShardingPolicyBuilder(2)
        .table("TEST")
            .shardByField(0)
            .addRange(0, 50, 0)
            .addRange(50, 100, 1)
        .build();
    policy.version = 456;

    EXPECT_TRUE(store_->save_sharding_policy(policy));
    EXPECT_EQ(456u, store_->get_sharding_policy_version());
}

// Test sharding policy save and load roundtrip via service
// @unsafe - RocksDB I/O
TEST_F(ConfigServiceTest, ShardingPolicySaveLoadRoundtrip) {
    auto original = ShardingPolicyBuilder(3)
        .table("WAREHOUSE")
            .shardByField(0)
            .addRange(1, 4, 0)
            .addRange(4, 7, 1)
            .addRange(7, 10, 2)
        .table("DISTRICT")
            .shardByField(0)
            .addRange(1, 4, 0)
            .addRange(4, 7, 1)
            .addRange(7, 10, 2)
        .build();
    original.version = 789;

    EXPECT_TRUE(store_->save_sharding_policy(original));

    auto loaded_opt = store_->load_sharding_policy();
    EXPECT_TRUE(loaded_opt.is_some());

    ShardingPolicySet loaded = loaded_opt.unwrap();
    EXPECT_EQ(original.version, loaded.version);
    EXPECT_EQ(original.num_shards, loaded.num_shards);
    EXPECT_EQ(original.table_count(), loaded.table_count());
    EXPECT_TRUE(loaded.has_policy("WAREHOUSE"));
    EXPECT_TRUE(loaded.has_policy("DISTRICT"));

    // Verify routing
    const auto* warehouse_policy = loaded.get_policy("WAREHOUSE");
    ASSERT_NE(nullptr, warehouse_policy);
    EXPECT_EQ(0, warehouse_policy->get_shard(2));  // w_id 2 -> shard 0
    EXPECT_EQ(1, warehouse_policy->get_shard(5));  // w_id 5 -> shard 1
    EXPECT_EQ(2, warehouse_policy->get_shard(8));  // w_id 8 -> shard 2
}

// Test sharding policy cache invalidation
// @safe
TEST_F(ConfigServiceTest, ShardingPolicyCacheInvalidation) {
    auto policy1 = ShardingPolicyBuilder(2)
        .table("TEST")
            .shardByField(0)
            .addRange(0, 50, 0)
            .addRange(50, 100, 1)
        .build();
    policy1.version = 100;
    EXPECT_TRUE(store_->save_sharding_policy(policy1));

    EXPECT_EQ(100u, store_->get_sharding_policy_version());

    // Save new policy
    auto policy2 = ShardingPolicyBuilder(2)
        .table("TEST")
            .shardByField(0)
            .addRange(0, 50, 0)
            .addRange(50, 100, 1)
        .build();
    policy2.version = 200;
    EXPECT_TRUE(store_->save_sharding_policy(policy2));

    // Invalidate cache
    service_->invalidate_sharding_cache();

    // Should see new version
    EXPECT_EQ(200u, store_->get_sharding_policy_version());
}

// Test sharding policy multiple updates
// @unsafe - RocksDB I/O
TEST_F(ConfigServiceTest, ShardingPolicyMultipleUpdates) {
    for (uint64_t v = 1; v <= 5; ++v) {
        auto policy = ShardingPolicyBuilder(2)
            .table("TEST")
                .shardByField(0)
                .addRange(0, 50, 0)
                .addRange(50, 100, 1)
            .build();
        policy.version = v;
        EXPECT_TRUE(store_->save_sharding_policy(policy));
        EXPECT_EQ(v, store_->get_sharding_policy_version());

        // Invalidate cache between updates
        service_->invalidate_sharding_cache();
    }
}

// Test cluster config and sharding policy coexist in service
// @unsafe - RocksDB I/O
TEST_F(ConfigServiceTest, ConfigAndShardingPolicyCoexistInService) {
    // Save cluster config
    PersistentConfig config = create_sample_config(10);
    EXPECT_TRUE(store_->save(config));

    // Save sharding policy
    auto policy = ShardingPolicyBuilder(2)
        .table("WAREHOUSE")
            .shardByField(0)
            .addRange(1, 6, 0)
            .addRange(6, 11, 1)
        .build();
    policy.version = 20;
    EXPECT_TRUE(store_->save_sharding_policy(policy));

    // Verify both exist
    EXPECT_TRUE(store_->has_config());
    EXPECT_TRUE(store_->has_sharding_policy());
    EXPECT_EQ(10u, store_->get_version());
    EXPECT_EQ(20u, store_->get_sharding_policy_version());

    // Load and verify both
    auto config_opt = store_->load();
    EXPECT_TRUE(config_opt.is_some());
    EXPECT_EQ(10u, config_opt.unwrap().version);

    auto policy_opt = store_->load_sharding_policy();
    EXPECT_TRUE(policy_opt.is_some());
    EXPECT_EQ(20u, policy_opt.unwrap().version);
}

// Test TPC-C style sharding policy via service
// @unsafe - RocksDB I/O
TEST_F(ConfigServiceTest, TpccShardingPolicyViaService) {
    // Create TPC-C policy: 10 warehouses across 2 shards
    auto policy = create_tpcc_sharding_policy(10, 2);
    policy.version = 1000;

    EXPECT_TRUE(store_->save_sharding_policy(policy));

    auto loaded_opt = store_->load_sharding_policy();
    EXPECT_TRUE(loaded_opt.is_some());

    ShardingPolicySet loaded = loaded_opt.unwrap();
    EXPECT_EQ(1000u, loaded.version);
    EXPECT_EQ(2, loaded.num_shards);

    // Verify all TPC-C tables exist
    EXPECT_TRUE(loaded.has_policy("WAREHOUSE"));
    EXPECT_TRUE(loaded.has_policy("DISTRICT"));
    EXPECT_TRUE(loaded.has_policy("CUSTOMER"));
    EXPECT_TRUE(loaded.has_policy("STOCK"));
    EXPECT_TRUE(loaded.has_policy("ORDER"));

    // Verify routing: w_id 1-5 -> shard 0, w_id 6-10 -> shard 1
    const auto* warehouse_policy = loaded.get_policy("WAREHOUSE");
    ASSERT_NE(nullptr, warehouse_policy);
    EXPECT_EQ(0, warehouse_policy->get_shard(1));
    EXPECT_EQ(0, warehouse_policy->get_shard(5));
    EXPECT_EQ(1, warehouse_policy->get_shard(6));
    EXPECT_EQ(1, warehouse_policy->get_shard(10));
}

}  // namespace test
}  // namespace janus
