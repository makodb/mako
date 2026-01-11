/**
 * @file config_service_test.cc
 * @brief Unit tests for ConfigService RPC interface.
 */

#include <gtest/gtest.h>
#include <filesystem>
#include <cstdlib>
#include <thread>
#include <chrono>
#include "../src/deptran/config_service.h"
#include "../src/deptran/config_store.h"
#include "../src/deptran/config_schema.h"

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

}  // namespace test
}  // namespace janus
