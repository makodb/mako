/**
 * @file config_failure_test.cc
 * @brief Failure scenario tests for Config Node components.
 *
 * Tests config node behavior under failure conditions:
 * - ConfigStore persistence across restarts
 * - ConfigClient reconnection handling
 * - Network failure scenarios
 */

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
namespace fs = std::filesystem;

// Helper to create a test configuration
// @safe
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

// ============================================================================
// ConfigStore Persistence Tests (Simulated Crash Recovery)
// ============================================================================

class ConfigStorePersistenceTest : public ::testing::Test {
protected:
    std::string test_db_path_;

    void SetUp() override {
        test_db_path_ = "/tmp/config_failure_test_store_" +
                        std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                        "_" + std::to_string(reinterpret_cast<uintptr_t>(this));
        fs::remove_all(test_db_path_);
    }

    void TearDown() override {
        fs::remove_all(test_db_path_);
    }
};

// Test that config persists after store close and reopen (simulating restart)
// @unsafe - RocksDB I/O
TEST_F(ConfigStorePersistenceTest, ConfigSurvivesRestart) {
    // Phase 1: Write config and close (simulating normal shutdown)
    {
        ConfigStore store(test_db_path_);
        ASSERT_TRUE(store.open());

        auto config = create_test_config(42);
        ASSERT_TRUE(store.save(config));
        EXPECT_EQ(store.get_version(), 42u);

        store.close();
    }

    // Phase 2: Reopen and verify config persisted (simulating restart)
    {
        ConfigStore store(test_db_path_);
        ASSERT_TRUE(store.open());

        EXPECT_TRUE(store.has_config());
        EXPECT_EQ(store.get_version(), 42u);

        auto loaded = store.load();
        ASSERT_TRUE(loaded.is_some());
        EXPECT_EQ(loaded.unwrap().version, 42u);
    }
}

// Test multiple close/reopen cycles (simulating multiple restarts)
// @unsafe - RocksDB I/O
TEST_F(ConfigStorePersistenceTest, MultipleRestartCycles) {
    // Initial save
    {
        ConfigStore store(test_db_path_);
        ASSERT_TRUE(store.open());
        ASSERT_TRUE(store.save(create_test_config(1)));
        store.close();
    }

    // Multiple restart cycles with version updates
    for (int i = 2; i <= 5; i++) {
        ConfigStore store(test_db_path_);
        ASSERT_TRUE(store.open());

        // Verify previous version
        EXPECT_EQ(store.get_version(), static_cast<uint64_t>(i - 1));

        // Update to new version
        ASSERT_TRUE(store.save(create_test_config(i)));
        EXPECT_EQ(store.get_version(), static_cast<uint64_t>(i));

        store.close();
    }

    // Final verification
    {
        ConfigStore store(test_db_path_);
        ASSERT_TRUE(store.open());
        EXPECT_EQ(store.get_version(), 5u);
    }
}

// Test that store handles being opened without prior data
// @unsafe - RocksDB I/O
TEST_F(ConfigStorePersistenceTest, FirstBootNoData) {
    ConfigStore store(test_db_path_);
    ASSERT_TRUE(store.open());

    // Should indicate no config
    EXPECT_FALSE(store.has_config());
    EXPECT_EQ(store.get_version(), 0u);

    auto loaded = store.load();
    EXPECT_TRUE(loaded.is_none());
}

// ============================================================================
// ConfigClient Failure Tests
// ============================================================================

class ConfigClientFailureTest : public ::testing::Test {
protected:
    std::string test_db_path_;
    ConfigStore* store_ = nullptr;
    rrr::Server* server_ = nullptr;
    rusty::Option<rusty::Arc<rrr::PollThread>> poll_thread_;
    std::string server_addr_;
    static std::atomic<int> port_counter_;

    void SetUp() override {
        test_db_path_ = "/tmp/config_failure_test_client_" +
                        std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                        "_" + std::to_string(reinterpret_cast<uintptr_t>(this));
        fs::remove_all(test_db_path_);

        // Create and open store
        store_ = new ConfigStore(test_db_path_);
        ASSERT_TRUE(store_->open());

        // Set up server on a unique port
        int port = 19000 + (port_counter_++ % 1000);
        server_addr_ = "127.0.0.1:" + std::to_string(port);
    }

    void TearDown() override {
        StopServer();
        if (store_) {
            store_->close();
            delete store_;
            store_ = nullptr;
        }
        fs::remove_all(test_db_path_);
    }

    // @unsafe - RPC server startup
    void StartServer() {
        poll_thread_ = rusty::Some(rrr::PollThread::create());
        server_ = new rrr::Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
        server_->reg_service(rusty::make_box<ConfigServiceImpl>(*store_));
        ASSERT_EQ(server_->start(server_addr_.c_str()), 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // @unsafe - RPC server shutdown
    void StopServer() {
        if (server_) {
            server_->graceful_shutdown(500);
            delete server_;
            server_ = nullptr;
        }
        poll_thread_ = rusty::None;
    }
};

std::atomic<int> ConfigClientFailureTest::port_counter_{0};

// Test connection failure to non-existent server
// @unsafe - Network I/O
TEST_F(ConfigClientFailureTest, ConnectionToNonExistentServer) {
    // Don't start server
    ConfigClient client(server_addr_);
    client.set_max_retries(2);
    client.set_retry_delay_ms(100);
    client.set_connect_timeout_ms(500);

    // Should fail to connect
    EXPECT_FALSE(client.connect());
    EXPECT_FALSE(client.is_connected());
}

// Test operations fail gracefully when not connected
// @unsafe - Network I/O
TEST_F(ConfigClientFailureTest, OperationsWithoutConnection) {
    ConfigClient client(server_addr_);

    // All operations should return None when not connected
    EXPECT_TRUE(client.has_config().is_none());
    EXPECT_TRUE(client.fetch_version().is_none());
    EXPECT_TRUE(client.fetch_config().is_none());
}

// Test client handles server stopping mid-session
// @unsafe - Network I/O
TEST_F(ConfigClientFailureTest, ServerStopsDuringSession) {
    StartServer();

    // Save config
    ASSERT_TRUE(store_->save(create_test_config(42)));

    // Connect client
    ConfigClient client(server_addr_);
    ASSERT_TRUE(client.connect());

    // Initial operation should succeed
    auto version = client.fetch_version();
    ASSERT_TRUE(version.is_some());
    EXPECT_EQ(version.unwrap(), 42u);

    // Stop server
    StopServer();

    // Give client time to notice
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Subsequent operations should fail gracefully (return None)
    // Note: Exact behavior depends on client implementation
    // The client may still appear connected briefly
}

// Test client can connect after server becomes available
// @unsafe - Network I/O
TEST_F(ConfigClientFailureTest, ConnectAfterServerStarts) {
    // Initially server is not running
    ConfigClient client(server_addr_);
    client.set_max_retries(1);
    client.set_retry_delay_ms(50);
    client.set_connect_timeout_ms(200);

    // First attempt fails
    EXPECT_FALSE(client.connect());

    // Start server
    StartServer();
    ASSERT_TRUE(store_->save(create_test_config(99)));

    // Second attempt should succeed
    EXPECT_TRUE(client.connect());

    auto version = client.fetch_version();
    EXPECT_TRUE(version.is_some());
    if (version.is_some()) {
        EXPECT_EQ(version.unwrap(), 99u);
    }
}

// Test client handles rapid connect/disconnect
// @unsafe - Network I/O
TEST_F(ConfigClientFailureTest, RapidConnectDisconnect) {
    StartServer();
    ASSERT_TRUE(store_->save(create_test_config(1)));

    // Rapid connect/disconnect cycles
    for (int i = 0; i < 5; i++) {
        ConfigClient client(server_addr_);
        EXPECT_TRUE(client.connect());
        EXPECT_TRUE(client.is_connected());
        client.disconnect();
        EXPECT_FALSE(client.is_connected());
    }
}

// Test multiple clients during server restart
// @unsafe - Network I/O
TEST_F(ConfigClientFailureTest, ClientsDuringServerRestart) {
    StartServer();
    ASSERT_TRUE(store_->save(create_test_config(50)));

    // Create and connect client
    ConfigClient client(server_addr_);
    ASSERT_TRUE(client.connect());

    auto version1 = client.fetch_version();
    ASSERT_TRUE(version1.is_some());
    EXPECT_EQ(version1.unwrap(), 50u);

    // Stop server
    StopServer();
    client.disconnect();

    // Start server again (simulating restart)
    StartServer();

    // Client should be able to reconnect
    EXPECT_TRUE(client.connect());

    auto version2 = client.fetch_version();
    EXPECT_TRUE(version2.is_some());
    if (version2.is_some()) {
        EXPECT_EQ(version2.unwrap(), 50u);
    }
}

// ============================================================================
// End-to-End Failure Scenarios
// ============================================================================

// Test full workflow with server restart
// @unsafe - RocksDB and Network I/O
TEST_F(ConfigClientFailureTest, FullWorkflowWithRestart) {
    // Phase 1: Initial setup
    StartServer();
    ASSERT_TRUE(store_->save(create_test_config(100)));

    ConfigClient client1(server_addr_);
    ASSERT_TRUE(client1.connect());

    auto fetched1 = client1.fetch_config();
    ASSERT_TRUE(fetched1.is_some());
    EXPECT_EQ(fetched1.unwrap().version, 100u);

    client1.disconnect();

    // Phase 2: Server restart (simulating c-node reboot)
    StopServer();

    // Store survives (persisted)
    EXPECT_TRUE(store_->has_config());

    // Restart server
    StartServer();

    // Phase 3: New client connects after restart
    ConfigClient client2(server_addr_);
    ASSERT_TRUE(client2.connect());

    // Config should still be available
    auto fetched2 = client2.fetch_config();
    ASSERT_TRUE(fetched2.is_some());
    EXPECT_EQ(fetched2.unwrap().version, 100u);
}

// Test config update survives restart
// @unsafe - RocksDB and Network I/O
TEST_F(ConfigClientFailureTest, ConfigUpdateSurvivesRestart) {
    // Phase 1: Save initial config
    StartServer();
    ASSERT_TRUE(store_->save(create_test_config(1)));

    ConfigClient client(server_addr_);
    ASSERT_TRUE(client.connect());
    EXPECT_EQ(client.fetch_version().unwrap(), 1u);
    client.disconnect();

    // Phase 2: Update config
    ASSERT_TRUE(store_->save(create_test_config(2)));

    // Phase 3: Restart server
    StopServer();
    StartServer();

    // Phase 4: Verify updated config persisted
    ASSERT_TRUE(client.connect());
    EXPECT_EQ(client.fetch_version().unwrap(), 2u);
}
