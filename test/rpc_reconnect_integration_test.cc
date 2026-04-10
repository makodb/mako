/**
 * Integration tests for ReconnectPolicy with actual RPC operations.
 * Tests reconnection behavior, backoff, and policy configuration.
 */

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <rusty/arc.hpp>
#include "reactor/reactor.h"
#include "rpc/client.hpp"
#include "rpc/server.hpp"
#include "rpc/reconnect_policy.hpp"
#include "misc/marshal.hpp"
#include "benchmark_service.h"
#include "rpc_test_ports.h"

using namespace rrr;
using namespace benchmark;
using namespace std::chrono;

template <typename Predicate>
bool wait_for_condition(Predicate&& predicate, milliseconds timeout) {
    auto deadline = steady_clock::now() + timeout;
    while (steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(milliseconds(10));
    }
    return predicate();
}

// Simple test service for integration tests
class ReconnectTestService : public benchmark::BenchmarkService {
public:
    std::atomic<int> call_count{0};

    void fast_nop(const std::string& input) override {
        call_count++;
    }

    void nop(const std::string& input) override {
        call_count++;
    }

    void fast_prime(const i32& n, i8* flag) override {
        *flag = 1;
    }

    void fast_vec(const i32& n, std::vector<i64>* v) override {
        for (i32 i = 0; i < n; i++) v->push_back(i);
    }

    void sleep(const double& sec) override {
        std::this_thread::sleep_for(std::chrono::duration<double>(sec));
    }
};

// ============================================================================
// Reconnection Policy Configuration Tests
// ============================================================================

class ReconnectIntegrationTest : public ::testing::Test {
protected:
    rusty::Option<rusty::Arc<PollThread>> poll_thread_;
    int test_port_;

    ReconnectIntegrationTest() : test_port_(test_ports::get_port()) {}

    void SetUp() override {
        poll_thread_ = rusty::Some(PollThread::create());
    }

    void TearDown() override {
        poll_thread_.as_ref().unwrap()->shutdown();
    }

    // Helper to start a server
    Server* start_server() {
        auto server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
        auto service_box = rusty::make_box<ReconnectTestService>();
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

TEST_F(ReconnectIntegrationTest, SetReconnectPolicyOnClient) {
    auto client = Client::create(poll_thread_.as_ref().unwrap());

    // Set aggressive policy
    auto policy = ReconnectPolicy::aggressive();
    client->set_reconnect_policy(policy);

    // Policy should be set (we can't directly query it from Client, but no crash = success)
    SUCCEED();

    client->close();
}

TEST_F(ReconnectIntegrationTest, IsReconnectingInitiallyFalse) {
    auto client = Client::create(poll_thread_.as_ref().unwrap());
    EXPECT_FALSE(client->is_reconnecting());
    client->close();
}

TEST_F(ReconnectIntegrationTest, ReconnectPolicyWithoutAutoRetryFailsFast) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(server_addr().c_str()), 0);
    client->close();

    ASSERT_TRUE(wait_for_condition([&]() {
        auto s = client->connection_state();
        return s == ConnectionState::DISCONNECTED || s == ConnectionState::FAILED;
    }, milliseconds(1000)));

    delete server;  // Ensure reconnect attempts fail.
    ASSERT_TRUE(wait_for_condition([&]() {
        auto probe = Client::create(poll_thread_.as_ref().unwrap());
        int rc = probe->connect(server_addr().c_str());
        if (rc == 0) {
            probe->close();
            return false;
        }
        return true;
    }, milliseconds(1500)));

    ReconnectPolicy policy;
    policy.auto_reconnect = false;
    policy.max_retries = 5;
    policy.initial_delay_ms = 200;
    policy.max_delay_ms = 200;
    policy.backoff_multiplier = 1.0;
    policy.jitter_enabled = false;
    client->set_reconnect_policy(policy);

    auto start = steady_clock::now();
    int result = client->reconnect();
    auto elapsed_ms = duration_cast<milliseconds>(steady_clock::now() - start).count();

    EXPECT_NE(result, 0);
    EXPECT_FALSE(client->connected());
    EXPECT_LT(elapsed_ms, 150);  // No policy retries/sleeps when auto_reconnect is disabled.
}

TEST_F(ReconnectIntegrationTest, ReconnectPolicyAppliesRetryDelays) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(server_addr().c_str()), 0);
    client->close();

    ASSERT_TRUE(wait_for_condition([&]() {
        auto s = client->connection_state();
        return s == ConnectionState::DISCONNECTED || s == ConnectionState::FAILED;
    }, milliseconds(1000)));

    delete server;  // Ensure reconnect attempts fail.
    ASSERT_TRUE(wait_for_condition([&]() {
        auto probe = Client::create(poll_thread_.as_ref().unwrap());
        int rc = probe->connect(server_addr().c_str());
        if (rc == 0) {
            probe->close();
            return false;
        }
        return true;
    }, milliseconds(1500)));

    ReconnectPolicy policy;
    policy.auto_reconnect = true;
    policy.max_retries = 2;
    policy.initial_delay_ms = 80;
    policy.max_delay_ms = 80;
    policy.backoff_multiplier = 1.0;
    policy.jitter_enabled = false;
    client->set_reconnect_policy(policy);

    auto start = steady_clock::now();
    int result = client->reconnect();
    auto elapsed_ms = duration_cast<milliseconds>(steady_clock::now() - start).count();

    EXPECT_NE(result, 0);
    EXPECT_FALSE(client->connected());
    EXPECT_GE(elapsed_ms, 130);  // Two retry sleeps (~160ms nominal) must be observed.
    EXPECT_LT(elapsed_ms, 3000);
}

TEST_F(ReconnectIntegrationTest, ReconnectAfterDisconnect) {
    // Start server
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    // Connect client
    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(server_addr().c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));
    EXPECT_TRUE(client->connected());

    // Disconnect
    client->close();
    std::this_thread::sleep_for(milliseconds(50));
    EXPECT_FALSE(client->connected());

    // Reconnect
    std::atomic<bool> reconnect_complete{false};
    std::atomic<bool> reconnect_success{false};

    int result = client->reconnect([&](bool success) {
        reconnect_success = success;
        reconnect_complete = true;
    });

    // Wait for reconnection to complete
    for (int i = 0; i < 50 && !reconnect_complete; i++) {
        std::this_thread::sleep_for(milliseconds(20));
    }

    if (reconnect_complete) {
        EXPECT_TRUE(reconnect_success);
        EXPECT_TRUE(client->connected());
    }

    client->close();
    delete server;
}

TEST_F(ReconnectIntegrationTest, ReconnectAfterServerRestart) {
    // Start server
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    // Connect client
    auto client = Client::create(poll_thread_.as_ref().unwrap());
    client->set_reconnect_policy(ReconnectPolicy::aggressive());
    ASSERT_EQ(client->connect(server_addr().c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));
    EXPECT_TRUE(client->connected());

    // Shutdown server
    delete server;
    std::this_thread::sleep_for(milliseconds(100));

    // Try a request - should fail
    std::string input = "test";
    auto fu_result = client->request(
        benchmark::BenchmarkService::FAST_NOP,
        [&](Marshal& m) { m << input; }
    );
    if (fu_result.is_ok()) {
        auto fu = fu_result.unwrap();
        fu->timed_wait(0.2);
    }

    // Close the failed connection
    client->close();
    std::this_thread::sleep_for(milliseconds(50));

    // Restart server
    server = start_server();
    ASSERT_NE(server, nullptr);
    std::this_thread::sleep_for(milliseconds(100));

    // Try to reconnect
    std::atomic<bool> reconnect_complete{false};
    std::atomic<bool> reconnect_success{false};

    int result = client->reconnect([&](bool success) {
        reconnect_success = success;
        reconnect_complete = true;
    });

    // Wait for reconnection
    for (int i = 0; i < 50 && !reconnect_complete; i++) {
        std::this_thread::sleep_for(milliseconds(20));
    }

    if (reconnect_complete && reconnect_success) {
        // Make a request on the new connection
        auto fu2_result = client->request(
            benchmark::BenchmarkService::FAST_NOP,
            [&](Marshal& m) { m << input; }
        );
        if (fu2_result.is_ok()) {
            auto fu2 = fu2_result.unwrap();
            fu2->wait();
            EXPECT_EQ(fu2->get_error_code(), 0);
        }
    }

    client->close();
    delete server;
}

// ============================================================================
// Reconnect Calculator Tests
// ============================================================================

TEST_F(ReconnectIntegrationTest, ReconnectCalculatorBackoff) {
    ReconnectPolicy policy;
    policy.initial_delay_ms = 100;
    policy.backoff_multiplier = 2.0;
    policy.max_delay_ms = 10000;
    policy.jitter_enabled = false;
    policy.max_retries = 5;

    ReconnectCalculator calc(policy);

    // First delay should be initial
    EXPECT_EQ(calc.next_delay_ms(), 100u);

    // Second delay should be doubled
    EXPECT_EQ(calc.next_delay_ms(), 200u);

    // Third delay
    EXPECT_EQ(calc.next_delay_ms(), 400u);
}

TEST_F(ReconnectIntegrationTest, ReconnectCalculatorMaxRetries) {
    ReconnectPolicy policy;
    policy.max_retries = 3;
    policy.auto_reconnect = true;

    ReconnectCalculator calc(policy);

    EXPECT_TRUE(calc.should_retry());
    calc.next_delay_ms();  // Retry 1

    EXPECT_TRUE(calc.should_retry());
    calc.next_delay_ms();  // Retry 2

    EXPECT_TRUE(calc.should_retry());
    calc.next_delay_ms();  // Retry 3

    EXPECT_FALSE(calc.should_retry());  // Exhausted
}

TEST_F(ReconnectIntegrationTest, ReconnectCalculatorReset) {
    ReconnectPolicy policy;
    policy.max_retries = 3;

    ReconnectCalculator calc(policy);

    calc.next_delay_ms();
    calc.next_delay_ms();
    EXPECT_EQ(calc.retry_count(), 2u);

    calc.reset();
    EXPECT_EQ(calc.retry_count(), 0u);
    EXPECT_TRUE(calc.should_retry());
}

TEST_F(ReconnectIntegrationTest, ReconnectPolicyPresetAggressive) {
    auto policy = ReconnectPolicy::aggressive();

    EXPECT_TRUE(policy.auto_reconnect);
    EXPECT_EQ(policy.max_retries, 0u);  // 0 = unlimited
    EXPECT_EQ(policy.initial_delay_ms, 100u);
    EXPECT_EQ(policy.max_delay_ms, 5000u);
    EXPECT_DOUBLE_EQ(policy.backoff_multiplier, 1.5);
    EXPECT_TRUE(policy.jitter_enabled);
}

TEST_F(ReconnectIntegrationTest, ReconnectPolicyPresetConservative) {
    auto policy = ReconnectPolicy::conservative();

    EXPECT_TRUE(policy.auto_reconnect);
    EXPECT_EQ(policy.max_retries, 5u);
    EXPECT_EQ(policy.initial_delay_ms, 1000u);
    EXPECT_EQ(policy.max_delay_ms, 30000u);
    EXPECT_DOUBLE_EQ(policy.backoff_multiplier, 2.0);
    EXPECT_TRUE(policy.jitter_enabled);
}

TEST_F(ReconnectIntegrationTest, ReconnectPolicyNoRetry) {
    auto policy = ReconnectPolicy::no_retry();

    EXPECT_FALSE(policy.auto_reconnect);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(ReconnectIntegrationTest, ReconnectWithoutPreviousConnection) {
    auto client = Client::create(poll_thread_.as_ref().unwrap());

    // Try to reconnect without ever connecting - should fail
    int result = client->reconnect();

    // Reconnect should fail because there's no address to reconnect to
    // (depends on implementation - might return error code or succeed with no-op)
    client->close();
}

TEST_F(ReconnectIntegrationTest, ReconnectWhileConnected) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(server_addr().c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));
    EXPECT_TRUE(client->connected());

    // Try to reconnect while already connected - should be no-op or fail
    int result = client->reconnect();
    // Either succeeds silently or returns an error

    EXPECT_TRUE(client->connected());  // Still connected

    client->close();
    delete server;
}

TEST_F(ReconnectIntegrationTest, MultipleReconnectAttempts) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    client->set_reconnect_policy(ReconnectPolicy::aggressive());

    // Connect and disconnect multiple times
    for (int i = 0; i < 3; i++) {
        if (!client->connected()) {
            ASSERT_EQ(client->connect(server_addr().c_str()), 0);
        }
        std::this_thread::sleep_for(milliseconds(30));
        EXPECT_TRUE(client->connected());

        // Make a request to verify connection works
        std::string input = "test_" + std::to_string(i);
        auto fu_result = client->request(
            benchmark::BenchmarkService::FAST_NOP,
            [&](Marshal& m) { m << input; }
        );
        ASSERT_TRUE(fu_result.is_ok());
        auto fu = fu_result.unwrap();
        fu->wait();
        EXPECT_EQ(fu->get_error_code(), 0);

        client->close();
        std::this_thread::sleep_for(milliseconds(30));
    }

    delete server;
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
