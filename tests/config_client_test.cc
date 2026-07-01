#include <stdint.h>

#include <gtest/gtest.h>

#include "__dep__.h"
#include "procedure.h"
#include "rcc/tx.h"
#include "rcc_rpc.h"
#include "config_schema.h"
#include "cluster/config_store.h"
#include "config_service.h"
#include "config_client.h"

import std;

using namespace janus;

// Helper to create a test configuration
static PersistentConfig create_test_config(uint64_t version = 42) {
    PersistentConfig config;
    config.version = version;

    PersistentSiteInfo site;
    site.id = 1;
    site.name = "test-site";
    site.host = "127.0.0.1";
    site.port = 8080;
    config.sites.push_back(site);

    PersistentReplicaGroup group;
    group.partition_id = 0;
    group.replica_ids = {1};
    config.replica_groups.push_back(group);

    config.settings.tx_proto = 1;
    config.settings.replica_proto = 2;
    config.settings.txn_timeout_us = 30000000;

    return config;
}

// Test fixture that sets up a ConfigService server
class ConfigClientTest : public ::testing::Test {
protected:
    std::string test_db_path_;
    ConfigStore* store_ = nullptr;
    ConfigServiceImpl* service_ = nullptr;  // Owned by server via Box
    rrr::Server* server_ = nullptr;
    rusty::Option<rusty::Arc<rrr::PollThread>> poll_thread_;
    std::string server_addr_;
    static std::atomic<int> port_counter_;

    void SetUp() override {
        // Create unique test database path
        test_db_path_ = "/tmp/config_client_test_" +
                        std::to_string(::testing::UnitTest::GetInstance()->current_test_info()->line()) +
                        "_" + std::to_string(reinterpret_cast<uintptr_t>(this));

        // Create store and open it
        store_ = new ConfigStore(test_db_path_);
        ASSERT_TRUE(store_->open());

        // Create service (will be moved into Box and owned by server)
        service_ = new ConfigServiceImpl(*store_);

        // Create poll thread and server
        poll_thread_ = rusty::Some(rrr::PollThread::create());

        // Use unique port
        int port = 19100 + (port_counter_.fetch_add(1) % 900);
        server_addr_ = "127.0.0.1:" + std::to_string(port);

        server_ = new rrr::Server(poll_thread_.as_ref().unwrap().clone());
        // Note: service_ will be owned by the Box after this call
        server_->reg_service(rusty::Box<ConfigServiceImpl>(service_));

        // Start server
        int result = server_->start(server_addr_.c_str());
        ASSERT_EQ(result, 0) << "Failed to start server on " << server_addr_;

        // Give server time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    void TearDown() override {
        if (server_) {
            server_->graceful_shutdown(1000);  // 1 second timeout
            delete server_;
            server_ = nullptr;
        }
        // Note: service_ is owned by server's Box, don't delete it

        if (store_) {
            store_->close();
            delete store_;
            store_ = nullptr;
        }

        // Clean up test database
        std::filesystem::remove_all(test_db_path_);
    }
};

std::atomic<int> ConfigClientTest::port_counter_{0};

// ============================================================================
// Construction Tests
// ============================================================================

TEST_F(ConfigClientTest, ConstructionBasic) {
    ConfigClient client(server_addr_);
    EXPECT_FALSE(client.is_connected());
}

TEST_F(ConfigClientTest, ConstructionWithDefaultRetryConfig) {
    ConfigClient client(server_addr_);
    EXPECT_EQ(client.max_retries(), 10);
    EXPECT_EQ(client.retry_delay_ms(), 1000);
    EXPECT_EQ(client.max_retry_delay_ms(), 30000);
    EXPECT_EQ(client.connect_timeout_ms(), 5000);
}

TEST_F(ConfigClientTest, SetRetryConfiguration) {
    ConfigClient client(server_addr_);
    client.set_max_retries(5);
    client.set_retry_delay_ms(500);
    client.set_max_retry_delay_ms(10000);
    client.set_connect_timeout_ms(3000);

    EXPECT_EQ(client.max_retries(), 5);
    EXPECT_EQ(client.retry_delay_ms(), 500);
    EXPECT_EQ(client.max_retry_delay_ms(), 10000);
    EXPECT_EQ(client.connect_timeout_ms(), 3000);
}

// ============================================================================
// Connection Tests
// ============================================================================

TEST_F(ConfigClientTest, ConnectSuccess) {
    ConfigClient client(server_addr_);
    EXPECT_TRUE(client.connect());
    EXPECT_TRUE(client.is_connected());
}

TEST_F(ConfigClientTest, ConnectDisconnect) {
    ConfigClient client(server_addr_);
    EXPECT_TRUE(client.connect());
    EXPECT_TRUE(client.is_connected());

    client.disconnect();
    EXPECT_FALSE(client.is_connected());
}

TEST_F(ConfigClientTest, MultipleDisconnects) {
    ConfigClient client(server_addr_);
    EXPECT_TRUE(client.connect());

    // Multiple disconnects should be safe
    client.disconnect();
    client.disconnect();
    client.disconnect();

    EXPECT_FALSE(client.is_connected());
}

TEST_F(ConfigClientTest, ConnectFailsToInvalidAddress) {
    ConfigClient client("127.0.0.1:59999");  // Unlikely to have server
    client.set_max_retries(2);
    client.set_retry_delay_ms(100);

    EXPECT_FALSE(client.connect());
    EXPECT_FALSE(client.is_connected());
}

// ============================================================================
// HasConfig Tests
// ============================================================================

TEST_F(ConfigClientTest, HasConfigEmpty) {
    ConfigClient client(server_addr_);
    ASSERT_TRUE(client.connect());

    auto result = client.has_config();
    ASSERT_TRUE(result.is_some());
    EXPECT_FALSE(result.unwrap());  // No config stored yet
}

TEST_F(ConfigClientTest, HasConfigWithData) {
    // Store a config
    auto config = create_test_config();
    ASSERT_TRUE(store_->save(config));
    service_->invalidate_cache();

    ConfigClient client(server_addr_);
    ASSERT_TRUE(client.connect());

    auto result = client.has_config();
    ASSERT_TRUE(result.is_some());
    EXPECT_TRUE(result.unwrap());  // Config exists
}

// ============================================================================
// FetchVersion Tests
// ============================================================================

TEST_F(ConfigClientTest, FetchVersionEmpty) {
    ConfigClient client(server_addr_);
    ASSERT_TRUE(client.connect());

    auto result = client.fetch_version();
    ASSERT_TRUE(result.is_some());
    EXPECT_EQ(result.unwrap(), 0);  // No version when empty
}

TEST_F(ConfigClientTest, FetchVersionWithData) {
    auto config = create_test_config(123);
    ASSERT_TRUE(store_->save(config));
    service_->invalidate_cache();

    ConfigClient client(server_addr_);
    ASSERT_TRUE(client.connect());

    auto result = client.fetch_version();
    ASSERT_TRUE(result.is_some());
    EXPECT_EQ(result.unwrap(), 123);
}

// ============================================================================
// FetchConfig Tests
// ============================================================================

TEST_F(ConfigClientTest, FetchConfigEmpty) {
    ConfigClient client(server_addr_);
    ASSERT_TRUE(client.connect());

    auto result = client.fetch_config();
    EXPECT_TRUE(result.is_none());  // No config when empty
}

TEST_F(ConfigClientTest, FetchConfigSuccess) {
    auto original = create_test_config(42);
    ASSERT_TRUE(store_->save(original));
    service_->invalidate_cache();

    ConfigClient client(server_addr_);
    ASSERT_TRUE(client.connect());

    auto result = client.fetch_config();
    ASSERT_TRUE(result.is_some());

    auto fetched = result.unwrap();
    EXPECT_EQ(fetched.version, 42);
    ASSERT_EQ(fetched.sites.size(), 1);
    EXPECT_EQ(fetched.sites[0].name, "test-site");
    EXPECT_EQ(fetched.sites[0].host, "127.0.0.1");
    EXPECT_EQ(fetched.sites[0].port, 8080);
}

TEST_F(ConfigClientTest, FetchConfigMultipleSites) {
    PersistentConfig config;
    config.version = 100;

    for (int i = 0; i < 5; i++) {
        PersistentSiteInfo site;
        site.id = i;
        site.name = "site-" + std::to_string(i);
        site.host = "192.168.1." + std::to_string(i + 1);
        site.port = 8080 + i;
        config.sites.push_back(site);
    }

    ASSERT_TRUE(store_->save(config));
    service_->invalidate_cache();

    ConfigClient client(server_addr_);
    ASSERT_TRUE(client.connect());

    auto result = client.fetch_config();
    ASSERT_TRUE(result.is_some());

    auto fetched = result.unwrap();
    EXPECT_EQ(fetched.version, 100);
    ASSERT_EQ(fetched.sites.size(), 5);

    for (int i = 0; i < 5; i++) {
        EXPECT_EQ(fetched.sites[i].id, i);
        EXPECT_EQ(fetched.sites[i].name, "site-" + std::to_string(i));
    }
}

TEST_F(ConfigClientTest, FetchConfigNotConnected) {
    ConfigClient client(server_addr_);
    // Don't connect

    auto result = client.fetch_config();
    EXPECT_TRUE(result.is_none());
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST_F(ConfigClientTest, OperationsAfterDisconnect) {
    auto config = create_test_config();
    ASSERT_TRUE(store_->save(config));
    service_->invalidate_cache();

    ConfigClient client(server_addr_);
    ASSERT_TRUE(client.connect());
    client.disconnect();

    // All operations should return None after disconnect
    EXPECT_TRUE(client.has_config().is_none());
    EXPECT_TRUE(client.fetch_version().is_none());
    EXPECT_TRUE(client.fetch_config().is_none());
}

// ============================================================================
// Integration Tests
// ============================================================================

TEST_F(ConfigClientTest, FullWorkflow) {
    // Initially no config
    ConfigClient client1(server_addr_);
    ASSERT_TRUE(client1.connect());
    EXPECT_FALSE(client1.has_config().unwrap());

    // Save config on server
    auto config = create_test_config(42);
    ASSERT_TRUE(store_->save(config));
    service_->invalidate_cache();

    // New client should see the config
    ConfigClient client2(server_addr_);
    ASSERT_TRUE(client2.connect());

    EXPECT_TRUE(client2.has_config().unwrap());
    EXPECT_EQ(client2.fetch_version().unwrap(), 42);

    auto fetched = client2.fetch_config();
    ASSERT_TRUE(fetched.is_some());
    EXPECT_EQ(fetched.unwrap().version, 42);
}

TEST_F(ConfigClientTest, MultipleClients) {
    auto config = create_test_config(100);
    ASSERT_TRUE(store_->save(config));
    service_->invalidate_cache();

    // Multiple clients fetching simultaneously
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int i = 0; i < 5; i++) {
        threads.emplace_back([&, addr = server_addr_]() {
            ConfigClient client(addr);
            if (client.connect()) {
                auto result = client.fetch_config();
                if (result.is_some() && result.unwrap().version == 100) {
                    success_count++;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(success_count, 5);
}
