/**
 * @file config_store_test.cc
 * @brief Unit tests for ConfigStore RocksDB persistence.
 */

#include <stdint.h>
#include <stddef.h>

#include <gtest/gtest.h>
#include "../src/cluster/config_store.h"
#include "../src/cluster/sharding_policy_builder.h"

import std;

namespace janus {
namespace test {

namespace fs = std::filesystem;

// Test fixture that creates a temporary directory for each test
class ConfigStoreTest : public ::testing::Test {
protected:
    std::string test_db_path_;

    void SetUp() override {
        // Create a unique temporary directory for this test
        const char* tmpdir = std::getenv("TMPDIR");
        if (tmpdir == nullptr) {
            tmpdir = "/tmp";
        }
        std::string base_path = std::string(tmpdir) + "/config_store_test_";
        base_path += std::to_string(::testing::UnitTest::GetInstance()->random_seed());
        base_path += "_" + std::to_string(reinterpret_cast<uintptr_t>(this));
        test_db_path_ = base_path;

        // Ensure directory doesn't exist from a previous failed test
        fs::remove_all(test_db_path_);
    }

    void TearDown() override {
        // Clean up the test directory
        fs::remove_all(test_db_path_);
    }

    // Helper to create a sample configuration
    // @safe
    PersistentConfig create_sample_config(uint64_t version = 1) {
        PersistentConfig config;
        config.version = version;

        // Add some sites
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

        PersistentSiteInfo site2;
        site2.id = 2;
        site2.locale_id = 10;
        site2.name = "server2";
        site2.proc_name = "proc1";
        site2.role = 1;
        site2.host = "192.168.1.2";
        site2.port = 8081;
        site2.n_thread = 4;
        site2.type = 1;
        site2.partition_id = 0;

        PersistentSiteInfo site3;
        site3.id = 3;
        site3.locale_id = 20;
        site3.name = "server3";
        site3.proc_name = "proc2";
        site3.role = 0;
        site3.host = "192.168.2.1";
        site3.port = 8082;
        site3.n_thread = 2;
        site3.type = 1;
        site3.partition_id = 1;

        config.sites = {site1, site2, site3};

        // Add replica groups
        PersistentReplicaGroup group1;
        group1.partition_id = 0;
        group1.replica_ids = {1, 2};

        PersistentReplicaGroup group2;
        group2.partition_id = 1;
        group2.replica_ids = {3};

        config.replica_groups = {group1, group2};

        // Set protocol settings
        config.settings.tx_proto = 5;
        config.settings.replica_proto = 2;
        config.settings.benchmark = 1;
        config.settings.txn_timeout_us = 45000000;
        config.settings.scale_factor = 2;

        return config;
    }
};

// Test opening a new database
// @unsafe - RocksDB I/O
TEST_F(ConfigStoreTest, OpenNewDatabase) {
    ConfigStore store(test_db_path_);
    EXPECT_FALSE(store.get_is_open());
    EXPECT_TRUE(store.open());
    EXPECT_TRUE(store.get_is_open());
    store.close();
    EXPECT_FALSE(store.get_is_open());
}

// Test opening an existing database
// @unsafe - RocksDB I/O
TEST_F(ConfigStoreTest, OpenExistingDatabase) {
    // First create and close a database
    {
        ConfigStore store(test_db_path_);
        EXPECT_TRUE(store.open());
        store.close();
    }

    // Reopen it
    {
        ConfigStore store(test_db_path_);
        EXPECT_TRUE(store.open());
        EXPECT_TRUE(store.get_is_open());
    }
}

// Test double open
// @unsafe - RocksDB I/O
TEST_F(ConfigStoreTest, DoubleOpen) {
    ConfigStore store(test_db_path_);
    EXPECT_TRUE(store.open());
    EXPECT_TRUE(store.open());  // Should succeed (already open)
    EXPECT_TRUE(store.get_is_open());
}

// Test double close
// @unsafe - RocksDB I/O
TEST_F(ConfigStoreTest, DoubleClose) {
    ConfigStore store(test_db_path_);
    EXPECT_TRUE(store.open());
    store.close();
    store.close();  // Should not crash
    EXPECT_FALSE(store.get_is_open());
}

// Test save without open
// @unsafe - RocksDB I/O
TEST_F(ConfigStoreTest, SaveWithoutOpen) {
    ConfigStore store(test_db_path_);
    PersistentConfig config = create_sample_config();
    EXPECT_FALSE(store.save(config));  // Should fail - not open
}

// Test load without open
// @unsafe - RocksDB I/O
TEST_F(ConfigStoreTest, LoadWithoutOpen) {
    ConfigStore store(test_db_path_);
    auto result = store.load();
    EXPECT_TRUE(result.is_none());  // Should return None - not open
}

// Test save and load roundtrip
// @unsafe - RocksDB I/O
TEST_F(ConfigStoreTest, SaveAndLoadRoundtrip) {
    ConfigStore store(test_db_path_);
    ASSERT_TRUE(store.open());

    // Save configuration
    PersistentConfig original = create_sample_config(42);
    EXPECT_TRUE(store.save(original));

    // Load configuration
    auto result = store.load();
    EXPECT_TRUE(result.is_some());

    PersistentConfig loaded = result.unwrap();

    // Verify version
    EXPECT_EQ(original.version, loaded.version);

    // Verify sites
    ASSERT_EQ(original.sites.size(), loaded.sites.size());
    for (size_t i = 0; i < original.sites.size(); ++i) {
        EXPECT_EQ(original.sites[i].id, loaded.sites[i].id);
        EXPECT_EQ(original.sites[i].locale_id, loaded.sites[i].locale_id);
        EXPECT_EQ(original.sites[i].name, loaded.sites[i].name);
        EXPECT_EQ(original.sites[i].proc_name, loaded.sites[i].proc_name);
        EXPECT_EQ(original.sites[i].role, loaded.sites[i].role);
        EXPECT_EQ(original.sites[i].host, loaded.sites[i].host);
        EXPECT_EQ(original.sites[i].port, loaded.sites[i].port);
        EXPECT_EQ(original.sites[i].n_thread, loaded.sites[i].n_thread);
        EXPECT_EQ(original.sites[i].type, loaded.sites[i].type);
        EXPECT_EQ(original.sites[i].partition_id, loaded.sites[i].partition_id);
    }

    // Verify replica groups
    ASSERT_EQ(original.replica_groups.size(), loaded.replica_groups.size());
    for (size_t i = 0; i < original.replica_groups.size(); ++i) {
        EXPECT_EQ(original.replica_groups[i].partition_id, loaded.replica_groups[i].partition_id);
        ASSERT_EQ(original.replica_groups[i].replica_ids.size(),
                  loaded.replica_groups[i].replica_ids.size());
        for (size_t j = 0; j < original.replica_groups[i].replica_ids.size(); ++j) {
            EXPECT_EQ(original.replica_groups[i].replica_ids[j],
                      loaded.replica_groups[i].replica_ids[j]);
        }
    }

    // Verify settings
    EXPECT_EQ(original.settings.tx_proto, loaded.settings.tx_proto);
    EXPECT_EQ(original.settings.replica_proto, loaded.settings.replica_proto);
    EXPECT_EQ(original.settings.benchmark, loaded.settings.benchmark);
    EXPECT_EQ(original.settings.txn_timeout_us, loaded.settings.txn_timeout_us);
    EXPECT_EQ(original.settings.scale_factor, loaded.settings.scale_factor);
}

// Test get_version
// @unsafe - RocksDB I/O
TEST_F(ConfigStoreTest, GetVersion) {
    ConfigStore store(test_db_path_);
    ASSERT_TRUE(store.open());

    // No config saved yet
    EXPECT_EQ(0u, store.get_version());

    // Save config with version 42
    PersistentConfig config = create_sample_config(42);
    EXPECT_TRUE(store.save(config));
    EXPECT_EQ(42u, store.get_version());

    // Save config with version 100
    config.version = 100;
    EXPECT_TRUE(store.save(config));
    EXPECT_EQ(100u, store.get_version());
}

// Test has_config
// @unsafe - RocksDB I/O
TEST_F(ConfigStoreTest, HasConfig) {
    ConfigStore store(test_db_path_);
    ASSERT_TRUE(store.open());

    // No config saved yet
    EXPECT_FALSE(store.has_config());

    // Save config
    PersistentConfig config = create_sample_config();
    EXPECT_TRUE(store.save(config));
    EXPECT_TRUE(store.has_config());
}

// Test load from empty database
// @unsafe - RocksDB I/O
TEST_F(ConfigStoreTest, LoadFromEmptyDatabase) {
    ConfigStore store(test_db_path_);
    ASSERT_TRUE(store.open());

    auto result = store.load();
    EXPECT_TRUE(result.is_none());
}

// Test persistence across database reopen
// @unsafe - RocksDB I/O
TEST_F(ConfigStoreTest, PersistenceAcrossReopen) {
    // Save configuration
    {
        ConfigStore store(test_db_path_);
        ASSERT_TRUE(store.open());
        PersistentConfig config = create_sample_config(123);
        EXPECT_TRUE(store.save(config));
        store.close();
    }

    // Reopen and load
    {
        ConfigStore store(test_db_path_);
        ASSERT_TRUE(store.open());

        EXPECT_TRUE(store.has_config());
        EXPECT_EQ(123u, store.get_version());

        auto result = store.load();
        EXPECT_TRUE(result.is_some());
        EXPECT_EQ(123u, result.unwrap().version);
    }
}

// Test saving empty configuration
// @unsafe - RocksDB I/O
TEST_F(ConfigStoreTest, SaveEmptyConfig) {
    ConfigStore store(test_db_path_);
    ASSERT_TRUE(store.open());

    PersistentConfig config;
    config.version = 1;
    // Empty sites and replica_groups

    EXPECT_TRUE(store.save(config));

    auto result = store.load();
    EXPECT_TRUE(result.is_some());
    PersistentConfig loaded = result.unwrap();
    EXPECT_EQ(1u, loaded.version);
    EXPECT_TRUE(loaded.sites.empty());
    EXPECT_TRUE(loaded.replica_groups.empty());
}

// Test overwriting configuration
// @unsafe - RocksDB I/O
TEST_F(ConfigStoreTest, OverwriteConfig) {
    ConfigStore store(test_db_path_);
    ASSERT_TRUE(store.open());

    // Save first config
    PersistentConfig config1 = create_sample_config(1);
    config1.settings.tx_proto = 10;
    EXPECT_TRUE(store.save(config1));

    // Save second config (overwrite)
    PersistentConfig config2 = create_sample_config(2);
    config2.settings.tx_proto = 20;
    EXPECT_TRUE(store.save(config2));

    // Verify second config is stored
    auto result = store.load();
    EXPECT_TRUE(result.is_some());
    PersistentConfig loaded = result.unwrap();
    EXPECT_EQ(2u, loaded.version);
    EXPECT_EQ(20, loaded.settings.tx_proto);
}

// =============================================================================
// Sharding Policy Storage Tests
// =============================================================================

// Test saving and loading sharding policy
// @unsafe - RocksDB I/O
TEST_F(ConfigStoreTest, SaveAndLoadShardingPolicy) {
    ConfigStore store(test_db_path_);
    ASSERT_TRUE(store.open());

    // Create a sample sharding policy using the builder
    auto policy = ShardingPolicyBuilder(2)
        .table("WAREHOUSE")
            .shardByField(0)
            .addRange(0, 5, 0)
            .addRange(5, 10, 1)
            .defaultShard(0)
        .table("DISTRICT")
            .shardByField(0)
            .addRange(0, 5, 0)
            .addRange(5, 10, 1)
        .build();
    policy.version = 42;

    // Save the policy
    EXPECT_TRUE(store.save_sharding_policy(policy));

    // Load the policy
    auto result = store.load_sharding_policy();
    EXPECT_TRUE(result.is_some());

    ShardingPolicySet loaded = result.unwrap();
    EXPECT_EQ(42u, loaded.version);
    EXPECT_EQ(2, loaded.num_shards);
    EXPECT_EQ(2u, loaded.table_count());

    // Verify routing works correctly
    EXPECT_EQ(0, loaded.get_shard_for_key("WAREHOUSE", 2));
    EXPECT_EQ(1, loaded.get_shard_for_key("WAREHOUSE", 7));
    EXPECT_EQ(0, loaded.get_shard_for_key("DISTRICT", 3));
    EXPECT_EQ(1, loaded.get_shard_for_key("DISTRICT", 8));
}

// Test loading non-existent sharding policy
// @unsafe - RocksDB I/O
TEST_F(ConfigStoreTest, LoadNonExistentShardingPolicy) {
    ConfigStore store(test_db_path_);
    ASSERT_TRUE(store.open());

    auto result = store.load_sharding_policy();
    EXPECT_TRUE(result.is_none());
}

// Test has_sharding_policy
// @unsafe - RocksDB I/O
TEST_F(ConfigStoreTest, HasShardingPolicy) {
    ConfigStore store(test_db_path_);
    ASSERT_TRUE(store.open());

    // No policy saved yet
    EXPECT_FALSE(store.has_sharding_policy());

    // Save policy
    auto policy = ShardingPolicyBuilder(2)
        .table("TEST")
            .shardByField(0)
            .addRange(0, 100, 0)
            .addRange(100, 200, 1)
        .build();
    policy.version = 1;
    EXPECT_TRUE(store.save_sharding_policy(policy));

    // Now policy exists
    EXPECT_TRUE(store.has_sharding_policy());
}

// Test get_sharding_policy_version
// @unsafe - RocksDB I/O
TEST_F(ConfigStoreTest, GetShardingPolicyVersion) {
    ConfigStore store(test_db_path_);
    ASSERT_TRUE(store.open());

    // No policy saved yet
    EXPECT_EQ(0u, store.get_sharding_policy_version());

    // Save policy with version 100
    auto policy = ShardingPolicyBuilder(2)
        .table("TEST")
            .shardByField(0)
            .defaultShard(0)
        .build();
    policy.version = 100;
    EXPECT_TRUE(store.save_sharding_policy(policy));
    EXPECT_EQ(100u, store.get_sharding_policy_version());

    // Update to version 200
    policy.version = 200;
    EXPECT_TRUE(store.save_sharding_policy(policy));
    EXPECT_EQ(200u, store.get_sharding_policy_version());
}

// Test sharding policy persistence across database reopen
// @unsafe - RocksDB I/O
TEST_F(ConfigStoreTest, ShardingPolicyPersistenceAcrossReopen) {
    // Save sharding policy
    {
        ConfigStore store(test_db_path_);
        ASSERT_TRUE(store.open());

        auto policy = create_tpcc_sharding_policy(10, 2);
        policy.version = 55;
        EXPECT_TRUE(store.save_sharding_policy(policy));
        store.close();
    }

    // Reopen and verify
    {
        ConfigStore store(test_db_path_);
        ASSERT_TRUE(store.open());

        EXPECT_TRUE(store.has_sharding_policy());
        EXPECT_EQ(55u, store.get_sharding_policy_version());

        auto result = store.load_sharding_policy();
        EXPECT_TRUE(result.is_some());

        ShardingPolicySet loaded = result.unwrap();
        EXPECT_EQ(55u, loaded.version);
        EXPECT_EQ(2, loaded.num_shards);

        // Verify TPC-C tables are present
        EXPECT_TRUE(loaded.has_policy("WAREHOUSE"));
        EXPECT_TRUE(loaded.has_policy("DISTRICT"));
        EXPECT_TRUE(loaded.has_policy("CUSTOMER"));

        // Verify routing
        EXPECT_EQ(0, loaded.get_shard_for_key("WAREHOUSE", 2));
        EXPECT_EQ(1, loaded.get_shard_for_key("WAREHOUSE", 7));
    }
}

// Test saving sharding policy without open
// @unsafe - RocksDB I/O
TEST_F(ConfigStoreTest, SaveShardingPolicyWithoutOpen) {
    ConfigStore store(test_db_path_);

    auto policy = ShardingPolicyBuilder(2)
        .table("TEST")
            .shardByField(0)
            .defaultShard(0)
        .build();

    EXPECT_FALSE(store.save_sharding_policy(policy));
}

// Test loading sharding policy without open
// @unsafe - RocksDB I/O
TEST_F(ConfigStoreTest, LoadShardingPolicyWithoutOpen) {
    ConfigStore store(test_db_path_);
    auto result = store.load_sharding_policy();
    EXPECT_TRUE(result.is_none());
}

// Test both cluster config and sharding policy stored together
// @unsafe - RocksDB I/O
TEST_F(ConfigStoreTest, ClusterConfigAndShardingPolicyCoexist) {
    ConfigStore store(test_db_path_);
    ASSERT_TRUE(store.open());

    // Save cluster config
    PersistentConfig config = create_sample_config(10);
    EXPECT_TRUE(store.save(config));

    // Save sharding policy
    auto policy = create_tpcc_sharding_policy(10, 2);
    policy.version = 20;
    EXPECT_TRUE(store.save_sharding_policy(policy));

    // Both should exist
    EXPECT_TRUE(store.has_config());
    EXPECT_TRUE(store.has_sharding_policy());

    // Both should have correct versions
    EXPECT_EQ(10u, store.get_version());
    EXPECT_EQ(20u, store.get_sharding_policy_version());

    // Load and verify both
    auto config_result = store.load();
    EXPECT_TRUE(config_result.is_some());
    EXPECT_EQ(10u, config_result.unwrap().version);

    auto policy_result = store.load_sharding_policy();
    EXPECT_TRUE(policy_result.is_some());
    EXPECT_EQ(20u, policy_result.unwrap().version);
}

}  // namespace test
}  // namespace janus
