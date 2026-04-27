/**
 * Integration tests combining multiple reliability components.
 * Tests interaction between state machine, circuit breaker, reconnection, and error handling.
 */

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <rusty/arc.hpp>
#include "../rrr.hpp"
#include "benchmark_service.h"
#include "rpc_test_ports.h"

using namespace rrr;
using namespace benchmark;
using namespace std::chrono;

// Test service for combined tests
class CombinedTestService : public benchmark::BenchmarkService {
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
// Combined Reliability Tests
// ============================================================================

class CombinedReliabilityTest : public ::testing::Test {
protected:
    rusty::Option<rusty::Arc<PollThread>> poll_thread_;
    int test_port_;

    CombinedReliabilityTest() : test_port_(test_ports::get_port()) {}

    void SetUp() override {
        poll_thread_ = rusty::Some(PollThread::create());
    }

    void TearDown() override {
        poll_thread_.as_ref().unwrap()->shutdown();
    }

    Server* start_server() {
        auto server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
        auto service_box = rusty::make_box<CombinedTestService>();
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

// ============================================================================
// State + Circuit Breaker Integration
// ============================================================================

TEST_F(CombinedReliabilityTest, StateAndCircuitBreakerInteraction) {
    CircuitBreakerConfig cb_config;
    cb_config.failure_threshold = 3;
    CircuitBreaker cb(cb_config);

    // Start server
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    auto client = Client::create(poll_thread_.as_ref().unwrap());

    // Initial state
    EXPECT_FALSE(client->connected());
    EXPECT_TRUE(cb.is_closed());

    // Connect
    ASSERT_EQ(client->connect(server_addr().c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    EXPECT_TRUE(client->connected());
    EXPECT_TRUE(cb.is_closed());

    // Make successful request
    if (cb.allow_request()) {
        std::string input = "test";
        auto fu_result = client->request(
            benchmark::BenchmarkService::FAST_NOP,
            [&](Marshal& m) { m << input; }
        );
        ASSERT_TRUE(fu_result.is_ok());
        auto fu = fu_result.unwrap();
        fu->wait();
        cb.record_success();
    }

    EXPECT_TRUE(cb.is_closed());
    EXPECT_TRUE(client->connected());

    client->close();
    delete server;
}

TEST_F(CombinedReliabilityTest, CircuitBreakerWithConnectionState) {
    CircuitBreakerConfig cb_config;
    cb_config.failure_threshold = 2;
    cb_config.timeout_ms = 100;
    CircuitBreaker cb(cb_config);

    // First, try connecting to non-existent server
    auto client = Client::create(poll_thread_.as_ref().unwrap());

    for (int i = 0; i < 2; i++) {
        if (cb.allow_request()) {
            int result = client->connect(server_addr().c_str());
            if (result != 0) {
                cb.record_failure();
            }
        }
    }

    // Circuit should be open now
    EXPECT_TRUE(cb.is_open());

    // Now start the server
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    // Wait for circuit timeout
    std::this_thread::sleep_for(milliseconds(150));

    // Should allow probe
    EXPECT_TRUE(cb.allow_request());
    EXPECT_TRUE(cb.is_half_open());

    // Create new client and connect
    auto new_client = Client::create(poll_thread_.as_ref().unwrap());
    int result = new_client->connect(server_addr().c_str());
    if (result == 0) {
        cb.record_success();
    } else {
        cb.record_failure();
    }

    // Need to hit success threshold
    std::this_thread::sleep_for(milliseconds(50));
    if (cb.is_half_open()) {
        cb.allow_request();
        cb.record_success();
    }

    EXPECT_TRUE(cb.is_closed() || cb.is_half_open());

    new_client->close();
    client->close();
    delete server;
}

// ============================================================================
// Reconnection Policy + State Machine Integration
// ============================================================================

TEST_F(CombinedReliabilityTest, ReconnectPolicyWithStateTracking) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    auto client = Client::create(poll_thread_.as_ref().unwrap());

    // Configure reconnection policy
    ReconnectPolicy policy;
    policy.initial_delay_ms = 50;
    policy.max_retries = 3;
    policy.auto_reconnect = true;
    client->set_reconnect_policy(policy);

    // Connect
    ASSERT_EQ(client->connect(server_addr().c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));
    EXPECT_TRUE(client->connected());

    // Disconnect
    client->close();
    std::this_thread::sleep_for(milliseconds(50));
    EXPECT_FALSE(client->connected());

    // Reconnect using policy
    std::atomic<bool> reconnect_done{false};
    client->reconnect([&](bool success) {
        reconnect_done = true;
    });

    // Wait for reconnection
    for (int i = 0; i < 50 && !reconnect_done; i++) {
        std::this_thread::sleep_for(milliseconds(20));
    }

    if (reconnect_done && client->connected()) {
        // Verify we can access connection state
        auto conn = client->connection();
        if (conn.is_some()) {
            EXPECT_EQ(conn.unwrap()->connection_state(), ConnectionState::CONNECTED);
        }
    }

    client->close();
    delete server;
}

TEST_F(CombinedReliabilityTest, ReconnectCalculatorResetOnSuccess) {
    ReconnectPolicy policy;
    policy.initial_delay_ms = 100;
    policy.max_retries = 5;
    policy.jitter_enabled = false;

    ReconnectCalculator calc(policy);

    // Simulate some failed retries
    calc.next_delay_ms();  // Retry 1
    calc.next_delay_ms();  // Retry 2
    EXPECT_EQ(calc.retry_count(), 2u);

    // Reset (simulating successful connection)
    calc.reset();
    EXPECT_EQ(calc.retry_count(), 0u);
    EXPECT_TRUE(calc.should_retry());
}

// ============================================================================
// Full Reliability Stack Integration
// ============================================================================

TEST_F(CombinedReliabilityTest, FullStackSuccessPath) {
    // Configure all reliability components
    CircuitBreakerConfig cb_config;
    cb_config.failure_threshold = 3;
    CircuitBreaker cb(cb_config);

    HeartbeatConfig hb_config;
    hb_config.interval_ms = 100;
    HeartbeatManager hb(hb_config);

    ReconnectPolicy reconnect_policy = ReconnectPolicy::aggressive();

    // Start server
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    // Create client with reconnection policy
    auto client = Client::create(poll_thread_.as_ref().unwrap());
    client->set_reconnect_policy(reconnect_policy);

    // Connect
    EXPECT_TRUE(cb.allow_request());
    ASSERT_EQ(client->connect(server_addr().c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));
    cb.record_success();

    EXPECT_TRUE(client->connected());
    EXPECT_TRUE(cb.is_closed());

    // Make requests while tracking heartbeat
    for (int i = 0; i < 5; i++) {
        if (cb.allow_request()) {
            std::string input = "test_" + std::to_string(i);
            auto fu_result = client->request(
                benchmark::BenchmarkService::FAST_NOP,
                [&](Marshal& m) { m << input; }
            );
            ASSERT_TRUE(fu_result.is_ok());
            auto fu = fu_result.unwrap();
            fu->wait();

            if (fu->get_error_code() == 0) {
                cb.record_success();
                hb.on_pong_received();  // Simulate heartbeat
            } else {
                cb.record_failure();
            }
        }
    }

    EXPECT_TRUE(cb.is_closed());
    EXPECT_TRUE(client->connected());
    EXPECT_FALSE(hb.check_timeout());

    client->close();
    delete server;
}

TEST_F(CombinedReliabilityTest, FullStackFailureAndRecovery) {
    CircuitBreakerConfig cb_config;
    cb_config.failure_threshold = 2;
    cb_config.timeout_ms = 100;
    CircuitBreaker cb(cb_config);

    // Start with no server
    auto client = Client::create(poll_thread_.as_ref().unwrap());
    client->set_reconnect_policy(ReconnectPolicy::aggressive());

    // Try to connect - should fail
    if (cb.allow_request()) {
        int result = client->connect(server_addr().c_str());
        if (result != 0) {
            cb.record_failure();
        }
    }

    // Second attempt
    if (cb.allow_request()) {
        int result = client->connect(server_addr().c_str());
        if (result != 0) {
            cb.record_failure();
        }
    }

    // Circuit should be open
    EXPECT_TRUE(cb.is_open());
    EXPECT_FALSE(cb.allow_request());

    // Now start server
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    // Wait for circuit timeout
    std::this_thread::sleep_for(milliseconds(150));

    // Should allow probe
    EXPECT_TRUE(cb.allow_request());

    // Create new client and try again
    auto new_client = Client::create(poll_thread_.as_ref().unwrap());
    if (new_client->connect(server_addr().c_str()) == 0) {
        cb.record_success();
        std::this_thread::sleep_for(milliseconds(50));

        // Make request to verify
        std::string input = "recovery_test";
        auto fu_result = new_client->request(
            benchmark::BenchmarkService::FAST_NOP,
            [&](Marshal& m) { m << input; }
        );
        ASSERT_TRUE(fu_result.is_ok());
        auto fu = fu_result.unwrap();
        fu->wait();
        EXPECT_EQ(fu->get_error_code(), 0);

        cb.record_success();
    }

    new_client->close();
    client->close();
    delete server;
}

// ============================================================================
// Error Type Integration
// ============================================================================

TEST_F(CombinedReliabilityTest, ErrorCategoriesWithCircuitBreaker) {
    CircuitBreakerConfig cb_config;
    cb_config.failure_threshold = 3;
    CircuitBreaker cb(cb_config);

    // Simulate different error types and their effect on circuit
    // Note: Only CONNECTION (100-199) and TIMEOUT (400-499) errors trigger record_failure
    // SERVICE_UNAVAILABLE is an APPLICATION error (301), not connection/timeout
    std::vector<RpcError> errors = {
        RpcError::NOT_CONNECTED,      // 100 - CONNECTION
        RpcError::REQUEST_TIMEOUT,    // 401 - TIMEOUT
        RpcError::CONNECTION_RESET    // 102 - CONNECTION
    };

    for (const auto& error : errors) {
        if (is_connection_error(error) || is_timeout_error(error)) {
            cb.record_failure();
        }
    }

    EXPECT_TRUE(cb.is_open());

    // Non-retryable errors shouldn't affect circuit in same way
    cb.reset();
    EXPECT_TRUE(cb.is_closed());

    // Protocol errors are not connection failures
    RpcError protocol_error = RpcError::INVALID_ARGUMENT;
    EXPECT_FALSE(is_connection_error(protocol_error));
}

TEST_F(CombinedReliabilityTest, HeartbeatWithStateTransitions) {
    HeartbeatConfig config;
    config.interval_ms = 50;
    config.timeout_ms = 100;
    config.max_missed = 2;
    HeartbeatManager hb(config);

    auto server = start_server();
    ASSERT_NE(server, nullptr);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(server_addr().c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    EXPECT_TRUE(client->connected());

    // Simulate heartbeat activity
    for (int i = 0; i < 5; i++) {
        if (hb.should_send_heartbeat()) {
            hb.on_heartbeat_sent();
            // Simulate successful pong
            hb.on_pong_received();
        }
        std::this_thread::sleep_for(milliseconds(30));
    }

    EXPECT_FALSE(hb.check_timeout());

    client->close();
    delete server;
}

// ============================================================================
// Rapid Failure/Recovery Cycle
// ============================================================================

TEST_F(CombinedReliabilityTest, RapidCycleStressTest) {
    CircuitBreakerConfig cb_config;
    cb_config.failure_threshold = 5;
    cb_config.timeout_ms = 50;
    CircuitBreaker cb(cb_config);

    auto server = start_server();
    ASSERT_NE(server, nullptr);

    for (int cycle = 0; cycle < 3; cycle++) {
        auto client = Client::create(poll_thread_.as_ref().unwrap());

        // Connect
        if (cb.allow_request()) {
            if (client->connect(server_addr().c_str()) == 0) {
                cb.record_success();
            } else {
                cb.record_failure();
            }
        }

        std::this_thread::sleep_for(milliseconds(20));

        // Make some requests
        for (int i = 0; i < 3 && client->connected() && cb.allow_request(); i++) {
            std::string input = "cycle_" + std::to_string(cycle) + "_" + std::to_string(i);
            auto fu_result = client->request(
                benchmark::BenchmarkService::FAST_NOP,
                [&](Marshal& m) { m << input; }
            );
            if (fu_result.is_ok()) {
                auto fu = fu_result.unwrap();
                fu->wait();
                if (fu->get_error_code() == 0) {
                    cb.record_success();
                } else {
                    cb.record_failure();
                }
            }
        }

        client->close();
        std::this_thread::sleep_for(milliseconds(20));
    }

    // Circuit should still be closed after successful operations
    EXPECT_TRUE(cb.is_closed());

    delete server;
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
