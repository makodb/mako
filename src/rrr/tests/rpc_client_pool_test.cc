/**
 * Unit tests for Enhanced ClientPool with Health Awareness
 * Tests pool configuration, health checking, idle cleanup, and connection management.
 */

#include <stdint.h>
#include <stddef.h>

#include <gtest/gtest.h>
#include <rusty/arc.hpp>
#include "../rrr.hpp"
#include "benchmark_service.h"
#include "rpc_test_ports.h"

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

// ============================================================================
// PoolConfig Tests
// ============================================================================

TEST(PoolConfigTest, DefaultValues) {
    PoolConfig config;
    EXPECT_EQ(config.min_connections, 1);
    EXPECT_EQ(config.max_connections, 4);
    EXPECT_EQ(config.idle_timeout_ms, 300000u);  // 5 minutes
    EXPECT_TRUE(config.health_check_enabled);
    EXPECT_EQ(config.unhealthy_threshold_percent, 50u);
    EXPECT_EQ(config.min_requests_for_health, 10u);
}

TEST(PoolConfigTest, DefaultsPreset) {
    auto config = PoolConfig::defaults();
    EXPECT_EQ(config.min_connections, 1);
    EXPECT_EQ(config.max_connections, 4);
    EXPECT_TRUE(config.health_check_enabled);
}

TEST(PoolConfigTest, AggressivePreset) {
    auto config = PoolConfig::aggressive();
    EXPECT_EQ(config.min_connections, 2);
    EXPECT_EQ(config.max_connections, 8);
    EXPECT_EQ(config.idle_timeout_ms, 60000u);  // 1 minute
    EXPECT_TRUE(config.health_check_enabled);
    EXPECT_EQ(config.unhealthy_threshold_percent, 70u);  // Stricter
    EXPECT_EQ(config.min_requests_for_health, 5u);  // Less data needed
}

TEST(PoolConfigTest, ConservativePreset) {
    auto config = PoolConfig::conservative();
    EXPECT_EQ(config.min_connections, 1);
    EXPECT_EQ(config.max_connections, 2);
    EXPECT_EQ(config.idle_timeout_ms, 600000u);  // 10 minutes
    EXPECT_TRUE(config.health_check_enabled);
    EXPECT_EQ(config.unhealthy_threshold_percent, 30u);  // More lenient
    EXPECT_EQ(config.min_requests_for_health, 20u);  // More data needed
}

TEST(PoolConfigTest, NoHealthCheckPreset) {
    auto config = PoolConfig::no_health_check();
    EXPECT_FALSE(config.health_check_enabled);
}

// ============================================================================
// ClientPool Tests with Real Server
// ============================================================================

class PoolTestService : public benchmark::BenchmarkService {
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

// @safe - Start a server with retries to avoid port collisions.
static Server* start_server_with_retry(const rusty::Arc<PollThread>& poll_thread, int* port_out) {
    const int kMaxAttempts = 10;
    for (int attempt = 0; attempt < kMaxAttempts; attempt++) {
        int port = (port_out != nullptr && *port_out > 0) ? *port_out : test_ports::get_port();
        auto server = new Server(rusty::Some(poll_thread.clone()));
        auto service_box = rusty::make_box<PoolTestService>();
        server->reg_service(std::move(service_box));
        if (server->start(("0.0.0.0:" + std::to_string(port)).c_str()) == 0) {
            if (port_out != nullptr) {
                *port_out = port;
            }
            return server;
        }
        delete server;
        if (port_out != nullptr) {
            *port_out = -1;
        }
        std::this_thread::sleep_for(milliseconds(5));
    }
    return nullptr;
}

class ClientPoolTest : public ::testing::Test {
protected:
    rusty::Option<rusty::Arc<PollThread>> poll_thread_;
    int test_port_;

    ClientPoolTest() : test_port_(test_ports::get_port()) {}

    void SetUp() override {
        poll_thread_ = rusty::Some(PollThread::create());
    }

    void TearDown() override {
        poll_thread_.as_ref().unwrap()->shutdown();
    }

    Server* start_server() {
        return start_server_with_retry(poll_thread_.as_ref().unwrap().clone(), &test_port_);
    }

    std::string server_addr() {
        return "127.0.0.1:" + std::to_string(test_port_);
    }
};

TEST_F(ClientPoolTest, CreateWithDefaultConfig) {
    ClientPool pool(rusty::Some(poll_thread_.as_ref().unwrap().clone()));

    auto config = pool.pool_config();
    EXPECT_EQ(config.min_connections, 1);
    EXPECT_EQ(config.max_connections, 4);
    EXPECT_TRUE(config.health_check_enabled);
}

TEST_F(ClientPoolTest, CreateWithCustomConfig) {
    auto custom_config = PoolConfig::aggressive();
    ClientPool pool(rusty::Some(poll_thread_.as_ref().unwrap().clone()), custom_config);

    auto config = pool.pool_config();
    EXPECT_EQ(config.min_connections, 2);
    EXPECT_EQ(config.max_connections, 8);
}

TEST_F(ClientPoolTest, SetPoolConfig) {
    ClientPool pool(rusty::Some(poll_thread_.as_ref().unwrap().clone()));

    auto new_config = PoolConfig::conservative();
    pool.set_pool_config(new_config);

    auto config = pool.pool_config();
    EXPECT_EQ(config.min_connections, 1);
    EXPECT_EQ(config.max_connections, 2);
}

TEST_F(ClientPoolTest, GetClientCreatesConnections) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    ClientPool pool(rusty::Some(poll_thread_.as_ref().unwrap().clone()));

    auto client = pool.get_client(server_addr());
    ASSERT_TRUE(client.is_some());
    EXPECT_TRUE(client.unwrap()->connected());

    EXPECT_EQ(pool.total_client_count(), 1u);
    EXPECT_EQ(pool.address_count(), 1u);

    delete server;
}

TEST_F(ClientPoolTest, GetClientReusesConnections) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    ClientPool pool(rusty::Some(poll_thread_.as_ref().unwrap().clone()));

    auto client1 = pool.get_client(server_addr());
    ASSERT_TRUE(client1.is_some());

    auto client2 = pool.get_client(server_addr());
    ASSERT_TRUE(client2.is_some());

    // Should still have only 1 connection (min_connections = 1)
    EXPECT_EQ(pool.total_client_count(), 1u);

    delete server;
}

TEST_F(ClientPoolTest, GetHealthyClientCount) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    ClientPool pool(rusty::Some(poll_thread_.as_ref().unwrap().clone()));

    auto client = pool.get_client(server_addr());
    ASSERT_TRUE(client.is_some());
    std::this_thread::sleep_for(milliseconds(50));

    // New connection should be healthy
    EXPECT_EQ(pool.get_healthy_client_count(server_addr()), 1u);

    delete server;
}

TEST_F(ClientPoolTest, TotalClientCount) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    ClientPool pool(rusty::Some(poll_thread_.as_ref().unwrap().clone()));

    EXPECT_EQ(pool.total_client_count(), 0u);

    auto client = pool.get_client(server_addr());
    ASSERT_TRUE(client.is_some());

    EXPECT_EQ(pool.total_client_count(), 1u);

    delete server;
}

TEST_F(ClientPoolTest, AddressCount) {
    auto server1 = start_server();
    ASSERT_NE(server1, nullptr);

    // Create second server on different port
    int port2 = test_ports::get_port();
    auto poll2 = PollThread::create();
    auto server2 = start_server_with_retry(poll2, &port2);
    ASSERT_NE(server2, nullptr);

    ClientPool pool(rusty::Some(poll_thread_.as_ref().unwrap().clone()));

    EXPECT_EQ(pool.address_count(), 0u);

    auto client1 = pool.get_client(server_addr());
    ASSERT_TRUE(client1.is_some());
    EXPECT_EQ(pool.address_count(), 1u);

    auto client2 = pool.get_client("127.0.0.1:" + std::to_string(port2));
    ASSERT_TRUE(client2.is_some());
    EXPECT_EQ(pool.address_count(), 2u);

    delete server1;
    delete server2;
    poll2->shutdown();
}

TEST_F(ClientPoolTest, CloseIdleClientsNoTimeout) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    // Config with no idle timeout
    PoolConfig config;
    config.idle_timeout_ms = 0;
    ClientPool pool(rusty::Some(poll_thread_.as_ref().unwrap().clone()), config);

    auto client = pool.get_client(server_addr());
    ASSERT_TRUE(client.is_some());

    // Should not close any (no timeout)
    EXPECT_EQ(pool.close_idle_clients(server_addr(), current_time_ms()), 0u);
    EXPECT_EQ(pool.total_client_count(), 1u);

    delete server;
}

TEST_F(ClientPoolTest, CloseIdleClientsRespectsMinConnections) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    // Config with min_connections = 1 and short idle timeout
    PoolConfig config;
    config.min_connections = 1;
    config.idle_timeout_ms = 10;  // 10ms timeout
    ClientPool pool(rusty::Some(poll_thread_.as_ref().unwrap().clone()), config);

    auto client = pool.get_client(server_addr());
    ASSERT_TRUE(client.is_some());

    // Wait for idle timeout
    std::this_thread::sleep_for(milliseconds(50));

    // Should not close - need to maintain min_connections
    EXPECT_EQ(pool.close_idle_clients(server_addr(), current_time_ms()), 0u);
    EXPECT_EQ(pool.total_client_count(), 1u);

    delete server;
}

TEST_F(ClientPoolTest, HealthCheckDisabled) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    auto config = PoolConfig::no_health_check();
    ClientPool pool(rusty::Some(poll_thread_.as_ref().unwrap().clone()), config);

    auto client = pool.get_client(server_addr());
    ASSERT_TRUE(client.is_some());

    // All clients considered healthy when health check disabled
    EXPECT_EQ(pool.get_healthy_client_count(server_addr()), 1u);

    delete server;
}

TEST_F(ClientPoolTest, RemoveUnhealthyRespectsMinConnections) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    PoolConfig config;
    config.min_connections = 1;
    ClientPool pool(rusty::Some(poll_thread_.as_ref().unwrap().clone()), config);

    auto client = pool.get_client(server_addr());
    ASSERT_TRUE(client.is_some());

    // Even if unhealthy, should keep min_connections
    size_t removed = pool.remove_unhealthy_clients(server_addr());
    EXPECT_EQ(removed, 0u);
    EXPECT_EQ(pool.total_client_count(), 1u);

    delete server;
}

TEST_F(ClientPoolTest, GetClientWithRealRequests) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    ClientPool pool(rusty::Some(poll_thread_.as_ref().unwrap().clone()));

    auto client_opt = pool.get_client(server_addr());
    ASSERT_TRUE(client_opt.is_some());
    auto client = client_opt.unwrap();

    // Make some requests
    for (int i = 0; i < 5; i++) {
        std::string input = "test_" + std::to_string(i);
        auto fu_result = client->request(
            benchmark::BenchmarkService::FAST_NOP, FutureAttr(),
            [&](BinaryWriteArchive& m) { m << input; }
        );
        ASSERT_TRUE(fu_result.is_ok());
        fu_result.unwrap()->wait();
    }

    // Client should still be healthy
    EXPECT_EQ(pool.get_healthy_client_count(server_addr()), 1u);

    delete server;
}

TEST_F(ClientPoolTest, RemoveAllUnhealthyWithMultipleAddresses) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    ClientPool pool(rusty::Some(poll_thread_.as_ref().unwrap().clone()));

    auto client = pool.get_client(server_addr());
    ASSERT_TRUE(client.is_some());

    // Should not remove any (all healthy or min_connections)
    size_t removed = pool.remove_all_unhealthy();
    EXPECT_EQ(removed, 0u);

    delete server;
}

TEST_F(ClientPoolTest, CloseAllIdleWithMultipleAddresses) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    PoolConfig config;
    config.idle_timeout_ms = 10;  // 10ms
    ClientPool pool(rusty::Some(poll_thread_.as_ref().unwrap().clone()), config);

    auto client = pool.get_client(server_addr());
    ASSERT_TRUE(client.is_some());

    std::this_thread::sleep_for(milliseconds(50));

    // Should not close - min_connections
    size_t closed = pool.close_all_idle(current_time_ms());
    EXPECT_EQ(closed, 0u);

    delete server;
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
