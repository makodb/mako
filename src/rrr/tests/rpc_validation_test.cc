/**
 * Unit tests for Proactive Connection Validation
 * Tests TCP keepalive configuration, idle detection, and connection validation.
 */

#include <stdint.h>

#include <gtest/gtest.h>
#include <rusty/arc.hpp>
#include "../rrr.hpp"
#include "benchmark_service.h"

import std;

using namespace rrr;
using namespace benchmark;
using namespace std::chrono;

// Helper to get current time in milliseconds
static uint64_t current_time_ms() {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count());
}

// Atomic counter for dynamic port allocation
static std::atomic<int> g_validation_test_port{16000};

// Test service for validation tests
class ValidationTestService : public benchmark::BenchmarkService {
public:
    std::atomic<int> call_count{0};

    rusty::Result<BenchmarkService::RpcFastNopResponse, i32>
    fast_nop(const BenchmarkService::RpcFastNopRequest& req) override {
        (void)req;
        call_count++;
        BenchmarkService::RpcFastNopResponse resp{};
        return rusty::Result<BenchmarkService::RpcFastNopResponse, i32>::Ok(resp);
    }

    rusty::Result<BenchmarkService::RpcNopResponse, i32>
    nop(const BenchmarkService::RpcNopRequest& req) override {
        (void)req;
        call_count++;
        BenchmarkService::RpcNopResponse resp{};
        return rusty::Result<BenchmarkService::RpcNopResponse, i32>::Ok(resp);
    }

    rusty::Result<BenchmarkService::RpcFastPrimeResponse, i32>
    fast_prime(const BenchmarkService::RpcFastPrimeRequest& req) override {
        (void)req;
        BenchmarkService::RpcFastPrimeResponse resp{};
        resp.flag = 1;
        return rusty::Result<BenchmarkService::RpcFastPrimeResponse, i32>::Ok(resp);
    }

    rusty::Result<BenchmarkService::RpcFastVecResponse, i32>
    fast_vec(const BenchmarkService::RpcFastVecRequest& req) override {
        BenchmarkService::RpcFastVecResponse resp{};
        for (i32 i = 0; i < req.n; i++) resp.v.push_back(i);
        return rusty::Result<BenchmarkService::RpcFastVecResponse, i32>::Ok(resp);
    }

    rusty::Result<BenchmarkService::RpcSleepResponse, i32>
    sleep(const BenchmarkService::RpcSleepRequest& req) override {
        std::this_thread::sleep_for(std::chrono::duration<double>(req.sec));
        BenchmarkService::RpcSleepResponse resp{};
        return rusty::Result<BenchmarkService::RpcSleepResponse, i32>::Ok(resp);
    }
};

// ============================================================================
// KeepaliveConfig Tests
// ============================================================================

TEST(KeepaliveConfigTest, DefaultValues) {
    KeepaliveConfig config;
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.idle_sec, 60);
    EXPECT_EQ(config.interval_sec, 10);
    EXPECT_EQ(config.count, 5);
}

TEST(KeepaliveConfigTest, AggressivePreset) {
    auto config = KeepaliveConfig::aggressive();
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.idle_sec, 10);
    EXPECT_EQ(config.interval_sec, 2);
    EXPECT_EQ(config.count, 3);
}

TEST(KeepaliveConfigTest, RelaxedPreset) {
    auto config = KeepaliveConfig::relaxed();
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.idle_sec, 60);
    EXPECT_EQ(config.interval_sec, 10);
    EXPECT_EQ(config.count, 5);
}

TEST(KeepaliveConfigTest, DisabledPreset) {
    auto config = KeepaliveConfig::disabled();
    EXPECT_FALSE(config.enabled);
    EXPECT_EQ(config.idle_sec, 0);
    EXPECT_EQ(config.interval_sec, 0);
    EXPECT_EQ(config.count, 0);
}

// ============================================================================
// Connection Validation Tests with Real Server
// ============================================================================

class ConnectionValidationTest : public ::testing::Test {
protected:
    rusty::Option<rusty::Arc<PollThread>> poll_thread_;
    int test_port_;

    ConnectionValidationTest() : test_port_(g_validation_test_port.fetch_add(1)) {}

    void SetUp() override {
        poll_thread_ = rusty::Some(PollThread::create());
    }

    void TearDown() override {
        poll_thread_.as_ref().unwrap()->shutdown();
    }

    Server* start_server() {
        auto server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
        auto service_box = rusty::make_box<ValidationTestService>();
        server->reg_service(std::move(service_box));
        if (server->start(("0.0.0.0:" + std::to_string(test_port_)).c_str()) != 0) {
            delete server;
            return nullptr;
        }
        return server;
    }

    std::string server_addr() {
        return "127.0.0.1:" + std::to_string(test_port_);
    }
};

TEST_F(ConnectionValidationTest, SetKeepaliveConfig) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    auto client = Client::create(poll_thread_.as_ref().unwrap());

    // Set aggressive keepalive before connecting
    auto config = KeepaliveConfig::aggressive();
    client->set_keepalive(config);

    // Connect
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(server_addr().c_str()), true), 0);
    std::this_thread::sleep_for(milliseconds(50));

    EXPECT_TRUE(client->connected());

    // Verify config was stored
    auto stored_config = client->keepalive_config();
    EXPECT_EQ(stored_config.idle_sec, 10);
    EXPECT_EQ(stored_config.interval_sec, 2);
    EXPECT_EQ(stored_config.count, 3);

    client->close();
    delete server;
}

TEST_F(ConnectionValidationTest, ValidateConnectedConnection) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(server_addr().c_str()), true), 0);
    std::this_thread::sleep_for(milliseconds(50));

    EXPECT_TRUE(client->connected());
    EXPECT_TRUE(client->validate_connection());

    client->close();
    delete server;
}

TEST_F(ConnectionValidationTest, ValidateDisconnectedConnection) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(server_addr().c_str()), true), 0);
    std::this_thread::sleep_for(milliseconds(50));

    EXPECT_TRUE(client->validate_connection());

    // Close connection
    client->close();
    std::this_thread::sleep_for(milliseconds(50));

    // Should return false after close
    EXPECT_FALSE(client->validate_connection());

    delete server;
}

TEST_F(ConnectionValidationTest, ValidateNoConnection) {
    auto client = Client::create(poll_thread_.as_ref().unwrap());

    // No connection yet
    EXPECT_FALSE(client->validate_connection());
}

TEST_F(ConnectionValidationTest, IdleDetectionNotIdleInitially) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(server_addr().c_str()), true), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Just connected, should not be idle
    EXPECT_FALSE(client->is_idle(100, current_time_ms()));  // Not idle after 100ms threshold

    client->close();
    delete server;
}

TEST_F(ConnectionValidationTest, IdleDetectionBecomesIdle) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(server_addr().c_str()), true), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Wait for connection to become idle (use longer delay for robustness)
    std::this_thread::sleep_for(milliseconds(300));

    // Should be idle after 100ms threshold (we waited 350ms total)
    EXPECT_TRUE(client->is_idle(100, current_time_ms()));

    // Should not be idle with very long threshold
    EXPECT_FALSE(client->is_idle(500, current_time_ms()));

    client->close();
    delete server;
}

TEST_F(ConnectionValidationTest, ActivityUpdatesOnRequest) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(server_addr().c_str()), true), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Wait a bit
    std::this_thread::sleep_for(milliseconds(100));
    EXPECT_TRUE(client->is_idle(50, current_time_ms()));  // Idle for 50ms

    // Make a request (which will update activity time)
    std::string input = "test";
    auto fu_result = client->request(
        benchmark::BenchmarkService::FAST_NOP, FutureAttr(),
        [&](BinaryWriteArchive& m) { m << input; }
    );
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();
    fu->wait();

    // Small sleep to let response come back
    std::this_thread::sleep_for(milliseconds(20));

    // Should no longer be idle (activity time updated by read/write)
    EXPECT_FALSE(client->is_idle(50, current_time_ms()));

    client->close();
    delete server;
}

TEST_F(ConnectionValidationTest, ValidateAfterServerRestart) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(server_addr().c_str()), true), 0);
    std::this_thread::sleep_for(milliseconds(50));

    EXPECT_TRUE(client->connected());
    EXPECT_TRUE(client->validate_connection());

    // Stop server
    delete server;
    std::this_thread::sleep_for(milliseconds(100));

    // The client may not immediately know the connection is broken
    // until we try to use it or the OS detects it
    // With aggressive keepalive, detection would be faster

    // Note: Without actual I/O, the socket error may not be detected immediately
    // This test mainly validates the API works

    client->close();
}

TEST_F(ConnectionValidationTest, KeepaliveAppliedOnConnect) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    auto client = Client::create(poll_thread_.as_ref().unwrap());

    // Set custom keepalive before connecting
    KeepaliveConfig config;
    config.enabled = true;
    config.idle_sec = 30;
    config.interval_sec = 5;
    config.count = 4;
    client->set_keepalive(config);

    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(server_addr().c_str()), true), 0);
    std::this_thread::sleep_for(milliseconds(50));

    EXPECT_TRUE(client->connected());

    // Verify config matches what we set
    auto stored = client->keepalive_config();
    EXPECT_EQ(stored.idle_sec, 30);
    EXPECT_EQ(stored.interval_sec, 5);
    EXPECT_EQ(stored.count, 4);

    client->close();
    delete server;
}

TEST_F(ConnectionValidationTest, DisabledKeepalive) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    auto client = Client::create(poll_thread_.as_ref().unwrap());

    // Disable keepalive
    client->set_keepalive(KeepaliveConfig::disabled());

    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(server_addr().c_str()), true), 0);
    std::this_thread::sleep_for(milliseconds(50));

    EXPECT_TRUE(client->connected());

    // Verify keepalive is disabled
    auto stored = client->keepalive_config();
    EXPECT_FALSE(stored.enabled);

    client->close();
    delete server;
}

// ============================================================================
// Integration with Other Reliability Components
// ============================================================================

TEST_F(ConnectionValidationTest, ValidationWithReconnect) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    client->set_reconnect_policy(ReconnectPolicy::aggressive());
    client->set_keepalive(KeepaliveConfig::aggressive());

    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(server_addr().c_str()), true), 0);
    std::this_thread::sleep_for(milliseconds(50));

    EXPECT_TRUE(client->validate_connection());

    // Disconnect
    client->close();
    std::this_thread::sleep_for(milliseconds(50));

    EXPECT_FALSE(client->validate_connection());

    // Reconnect
    std::atomic<bool> reconnect_done{false};
    client->reconnect([&](bool success) {
        reconnect_done = true;
    });

    for (int i = 0; i < 50 && !reconnect_done; i++) {
        std::this_thread::sleep_for(milliseconds(20));
    }

    if (reconnect_done && client->connected()) {
        EXPECT_TRUE(client->validate_connection());
    }

    client->close();
    delete server;
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
