/**
 * Integration tests for CircuitBreaker with actual RPC operations.
 * Tests circuit breaker behavior during connection failures and recovery.
 */

#include <gtest/gtest.h>
#include <rusty/arc.hpp>
#include "../rrr.hpp"
#include "benchmark_service.h"

import std;

using namespace rrr;
using namespace benchmark;
using namespace std::chrono;

// Atomic counter for dynamic port allocation
static std::atomic<int> g_cb_test_port{13000};

// Test service for circuit breaker tests
class CircuitBreakerTestService : public benchmark::BenchmarkService {
public:
    std::atomic<int> call_count{0};
    std::atomic<bool> should_fail{false};

    rusty::Result<BenchmarkService::RpcFastNopResponse, i32>
    fast_nop(const BenchmarkService::RpcFastNopRequest& req) override {
        (void)req;
        call_count++;
        if (should_fail) {
            throw std::runtime_error("Simulated failure");
        }
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
// Circuit Breaker Configuration Tests
// ============================================================================

class CircuitBreakerIntegrationTest : public ::testing::Test {
protected:
    rusty::Option<rusty::Arc<PollThread>> poll_thread_;
    int test_port_;

    CircuitBreakerIntegrationTest() : test_port_(g_cb_test_port.fetch_add(1)) {}

    void SetUp() override {
        poll_thread_ = rusty::Some(PollThread::create());
    }

    void TearDown() override {
        poll_thread_.as_ref().unwrap()->shutdown();
    }

    Server* start_server() {
        auto server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
        auto service_box = rusty::make_box<CircuitBreakerTestService>();
        server->reg_service_typed(std::move(service_box));
        if (server->start(reinterpret_cast<const int8_t*>(("0.0.0.0:" + std::to_string(test_port_)).c_str())) != 0) {
            delete server;
            return nullptr;
        }
        return server;
    }

    std::string server_addr() {
        return "127.0.0.1:" + std::to_string(test_port_);
    }
};

TEST_F(CircuitBreakerIntegrationTest, InitialStateClosed) {
    CircuitBreaker cb(CircuitBreakerConfig{});
    EXPECT_EQ(cb.state(), CircuitState::CLOSED);
    EXPECT_TRUE(cb.is_closed());
    EXPECT_TRUE(cb.allow_request());
}

TEST_F(CircuitBreakerIntegrationTest, CircuitOpensAfterFailures) {
    CircuitBreakerConfig config;
    config.failure_threshold = 3;
    CircuitBreaker cb(config);

    // Record failures up to threshold
    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitState::CLOSED);

    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitState::CLOSED);

    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitState::OPEN);
    EXPECT_FALSE(cb.allow_request());
}

TEST_F(CircuitBreakerIntegrationTest, CircuitBreakerWithRpcFailures) {
    CircuitBreakerConfig config;
    config.failure_threshold = 3;
    config.timeout_ms = 100;  // Short timeout for testing
    CircuitBreaker cb(config);

    // Start server first
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(server_addr().c_str()), true), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Simulate checking circuit before each request
    EXPECT_TRUE(cb.allow_request());

    // Make a successful request
    std::string input = "test";
    auto fu_result = client->request(
        benchmark::BenchmarkService::FAST_NOP, FutureAttr(),
        [&](BinaryWriteArchive& m) { m << input; }
    );
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();
    fu->wait();

    if (fu->get_error_code() == 0) {
        cb.record_success();
    } else {
        cb.record_failure();
    }

    EXPECT_EQ(cb.state(), CircuitState::CLOSED);

    // Shutdown server to cause failures
    client->close();
    delete server;
    std::this_thread::sleep_for(milliseconds(100));

    // Try connecting to non-existent server to simulate failures
    for (int i = 0; i < 3 && cb.allow_request(); i++) {
        auto fail_client = Client::create(poll_thread_.as_ref().unwrap());
        int result = fail_client->connect(reinterpret_cast<const int8_t*>(server_addr().c_str()), true);
        if (result != 0) {
            cb.record_failure();
        }
        fail_client->close();
    }

    // Circuit should be open now
    EXPECT_EQ(cb.state(), CircuitState::OPEN);
    EXPECT_FALSE(cb.allow_request());
}

TEST_F(CircuitBreakerIntegrationTest, CircuitHalfOpenAfterTimeout) {
    CircuitBreakerConfig config;
    config.failure_threshold = 2;
    config.timeout_ms = 50;  // Short timeout
    CircuitBreaker cb(config);

    // Open the circuit
    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitState::OPEN);

    // Wait for timeout
    std::this_thread::sleep_for(milliseconds(100));

    // Should allow a probe request
    EXPECT_TRUE(cb.allow_request());
    EXPECT_EQ(cb.state(), CircuitState::HALF_OPEN);
}

TEST_F(CircuitBreakerIntegrationTest, CircuitClosesOnProbeSuccess) {
    CircuitBreakerConfig config;
    config.failure_threshold = 2;
    config.success_threshold = 2;
    config.timeout_ms = 20;
    CircuitBreaker cb(config);

    // Open the circuit
    cb.record_failure();
    cb.record_failure();

    // Wait for HALF_OPEN
    std::this_thread::sleep_for(milliseconds(50));
    cb.allow_request();  // Triggers HALF_OPEN
    EXPECT_EQ(cb.state(), CircuitState::HALF_OPEN);

    // Record successes
    cb.record_success();
    EXPECT_EQ(cb.state(), CircuitState::HALF_OPEN);

    cb.allow_request();  // Allow next probe
    cb.record_success();
    EXPECT_EQ(cb.state(), CircuitState::CLOSED);
}

TEST_F(CircuitBreakerIntegrationTest, CircuitReopensOnProbeFailure) {
    CircuitBreakerConfig config;
    config.failure_threshold = 2;
    config.timeout_ms = 20;
    CircuitBreaker cb(config);

    // Open the circuit
    cb.record_failure();
    cb.record_failure();

    // Wait for HALF_OPEN
    std::this_thread::sleep_for(milliseconds(50));
    cb.allow_request();
    EXPECT_EQ(cb.state(), CircuitState::HALF_OPEN);

    // Failure should reopen
    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitState::OPEN);
}

TEST_F(CircuitBreakerIntegrationTest, DisabledCircuitAlwaysAllows) {
    auto config = CircuitBreakerConfig::disabled();
    CircuitBreaker cb(config);

    // Record many failures
    for (int i = 0; i < 100; i++) {
        cb.record_failure();
    }

    // Should still allow requests
    EXPECT_TRUE(cb.allow_request());
    EXPECT_EQ(cb.state(), CircuitState::CLOSED);
}

TEST_F(CircuitBreakerIntegrationTest, CircuitBreakerReset) {
    CircuitBreakerConfig config;
    config.failure_threshold = 2;
    CircuitBreaker cb(config);

    // Open the circuit
    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitState::OPEN);

    // Reset
    cb.reset();
    EXPECT_EQ(cb.state(), CircuitState::CLOSED);
    EXPECT_EQ(cb.failure_count(), 0u);
    EXPECT_TRUE(cb.allow_request());
}

// ============================================================================
// Circuit Breaker with Real RPC Operations
// ============================================================================

TEST_F(CircuitBreakerIntegrationTest, CircuitBreakerProtectsRpcCalls) {
    CircuitBreakerConfig config;
    config.failure_threshold = 3;
    config.timeout_ms = 100;
    CircuitBreaker cb(config);

    // Start server
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(server_addr().c_str()), true), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Make some successful requests
    for (int i = 0; i < 5; i++) {
        if (!cb.allow_request()) {
            FAIL() << "Circuit should be closed";
        }

        std::string input = "test_" + std::to_string(i);
        auto fu_result = client->request(
            benchmark::BenchmarkService::FAST_NOP, FutureAttr(),
            [&](BinaryWriteArchive& m) { m << input; }
        );
        ASSERT_TRUE(fu_result.is_ok());
        auto fu = fu_result.unwrap();
        fu->wait();

        if (fu->get_error_code() == 0) {
            cb.record_success();
        } else {
            cb.record_failure();
        }
    }

    EXPECT_EQ(cb.state(), CircuitState::CLOSED);

    client->close();
    delete server;
}

TEST_F(CircuitBreakerIntegrationTest, CircuitBreakerFailFast) {
    CircuitBreakerConfig config;
    config.failure_threshold = 2;
    CircuitBreaker cb(config);

    // Open the circuit immediately
    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitState::OPEN);

    // Measure time for fail-fast response
    auto start = steady_clock::now();

    // Try to make requests - should be rejected immediately
    int rejected_count = 0;
    for (int i = 0; i < 100; i++) {
        if (!cb.allow_request()) {
            rejected_count++;
        }
    }

    auto end = steady_clock::now();
    auto duration_us = duration_cast<microseconds>(end - start).count();

    // All 100 requests should be rejected
    EXPECT_EQ(rejected_count, 100);

    // Should be very fast (less than 1ms for 100 checks)
    EXPECT_LT(duration_us, 1000);
}

// ============================================================================
// Config Presets
// ============================================================================

TEST_F(CircuitBreakerIntegrationTest, SensitivePreset) {
    auto config = CircuitBreakerConfig::sensitive();
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.failure_threshold, 3u);
    EXPECT_EQ(config.success_threshold, 5u);
    EXPECT_EQ(config.timeout_ms, 60000u);
}

TEST_F(CircuitBreakerIntegrationTest, RelaxedPreset) {
    auto config = CircuitBreakerConfig::relaxed();
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.failure_threshold, 10u);
    EXPECT_EQ(config.success_threshold, 2u);
    EXPECT_EQ(config.timeout_ms, 15000u);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
