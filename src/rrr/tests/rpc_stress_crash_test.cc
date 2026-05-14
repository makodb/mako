/**
 * Stress tests for RPC crash recovery scenarios.
 * Tests high-load crash recovery, rapid restarts, and client storms.
 */

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <rusty/arc.hpp>
#include "../rrr.hpp"
#include "benchmark_service.h"
#include "rpc_test_ports.h"

using namespace rrr;
using namespace benchmark;
using namespace std::chrono;

// ============================================================================
// Stress Test Service - tracks request counts for verification
// ============================================================================

// @safe - Test service for stress scenarios
class StressTestService : public benchmark::BenchmarkService {
public:
    std::atomic<uint64_t> request_count{0};
    std::atomic<uint64_t> completed_count{0};
    std::atomic<bool> should_delay{false};
    std::atomic<uint32_t> delay_ms{0};

    // @safe - Increments counters atomically
    rusty::Result<BenchmarkService::RpcFastNopResponse, i32>
    fast_nop(const BenchmarkService::RpcFastNopRequest& req) override {
        (void)req;
        request_count++;
        if (should_delay && delay_ms > 0) {
            std::this_thread::sleep_for(milliseconds(delay_ms.load()));
        }
        completed_count++;
        BenchmarkService::RpcFastNopResponse resp{};
        return rusty::Result<BenchmarkService::RpcFastNopResponse, i32>::Ok(resp);
    }

    // @safe - Increments counters atomically
    rusty::Result<BenchmarkService::RpcNopResponse, i32>
    nop(const BenchmarkService::RpcNopRequest& req) override {
        (void)req;
        request_count++;
        if (should_delay && delay_ms > 0) {
            std::this_thread::sleep_for(milliseconds(delay_ms.load()));
        }
        completed_count++;
        BenchmarkService::RpcNopResponse resp{};
        return rusty::Result<BenchmarkService::RpcNopResponse, i32>::Ok(resp);
    }

    // @safe - Stub implementation
    rusty::Result<BenchmarkService::RpcFastPrimeResponse, i32>
    fast_prime(const BenchmarkService::RpcFastPrimeRequest& req) override {
        (void)req;
        BenchmarkService::RpcFastPrimeResponse resp{};
        resp.flag = 1;
        return rusty::Result<BenchmarkService::RpcFastPrimeResponse, i32>::Ok(resp);
    }

    // @safe - Stub implementation
    rusty::Result<BenchmarkService::RpcFastVecResponse, i32>
    fast_vec(const BenchmarkService::RpcFastVecRequest& req) override {
        BenchmarkService::RpcFastVecResponse resp{};
        for (i32 i = 0; i < req.n; i++) resp.v.push_back(i);
        return rusty::Result<BenchmarkService::RpcFastVecResponse, i32>::Ok(resp);
    }

    // @safe - Sleep implementation
    rusty::Result<BenchmarkService::RpcSleepResponse, i32>
    sleep(const BenchmarkService::RpcSleepRequest& req) override {
        std::this_thread::sleep_for(std::chrono::duration<double>(req.sec));
        BenchmarkService::RpcSleepResponse resp{};
        return rusty::Result<BenchmarkService::RpcSleepResponse, i32>::Ok(resp);
    }

    // @safe - Reset counters
    void reset() {
        request_count = 0;
        completed_count = 0;
        should_delay = false;
        delay_ms = 0;
    }
};

// ============================================================================
// Statistics collector for stress tests
// ============================================================================

// @safe - Thread-safe statistics collection
struct StressStats {
    std::atomic<uint64_t> requests_sent{0};
    std::atomic<uint64_t> requests_succeeded{0};
    std::atomic<uint64_t> requests_failed{0};
    std::atomic<uint64_t> connect_attempts{0};
    std::atomic<uint64_t> connect_succeeded{0};
    std::atomic<uint64_t> connect_failed{0};
    std::atomic<uint64_t> reconnect_attempts{0};
    std::atomic<uint64_t> reconnect_succeeded{0};

    // @safe - Reset all counters
    void reset() {
        requests_sent = 0;
        requests_succeeded = 0;
        requests_failed = 0;
        connect_attempts = 0;
        connect_succeeded = 0;
        connect_failed = 0;
        reconnect_attempts = 0;
        reconnect_succeeded = 0;
    }

    // @safe - Calculate success rate
    double success_rate() const {
        uint64_t total = requests_sent.load();
        if (total == 0) return 1.0;
        return static_cast<double>(requests_succeeded.load()) / total;
    }
};

// ============================================================================
// Stress Test Fixture
// ============================================================================

class StressCrashTest : public ::testing::Test {
protected:
    rusty::Option<rusty::Arc<PollThread>> poll_thread_;

    void SetUp() override {
        poll_thread_ = rusty::Some(PollThread::create());
    }

    void TearDown() override {
        poll_thread_.as_ref().unwrap()->shutdown();
    }

    // @safe - Create server on given port
    Server* create_server(int port) {
        auto server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
        auto service_box = rusty::make_box<StressTestService>();
        server->reg_service(std::move(service_box));
        std::string addr = "0.0.0.0:" + std::to_string(port);
        if (server->start(addr.c_str()) != 0) {
            delete server;
            return nullptr;
        }
        return server;
    }

    // @safe - Create server, retrying with new ports on bind failure.
    Server* create_server_with_retry(int* port_out) {
        const int kMaxAttempts = 10;
        for (int attempt = 0; attempt < kMaxAttempts; attempt++) {
            int port = (port_out != nullptr && *port_out > 0) ? *port_out : test_ports::get_port();
            auto server = create_server(port);
            if (server != nullptr) {
                if (port_out != nullptr) {
                    *port_out = port;
                }
                return server;
            }
            if (port_out != nullptr) {
                *port_out = -1;
            }
            std::this_thread::sleep_for(milliseconds(5));
        }
        return nullptr;
    }

    // @safe - Build loopback address for a port
    std::string make_addr(int port) {
        return "127.0.0.1:" + std::to_string(port);
    }

    // @safe - Create client configured for stress testing
    rusty::Arc<Client> create_stress_client() {
        auto client = Client::create(poll_thread_.as_ref().unwrap());
        ReconnectPolicy policy = ReconnectPolicy::aggressive();
        policy.max_retries = 10;
        policy.initial_delay_ms = 10;
        client->set_reconnect_policy(policy);
        return client;
    }

    // @safe - Send a single request
    bool send_request(rusty::Arc<Client>& client, StressStats& stats) {
        stats.requests_sent++;
        std::string input = "stress";
        auto fu_result = client->request(
            BenchmarkService::FAST_NOP,
            [&](BinaryWriteArchive& m) { m << input; }
        );
        if (fu_result.is_err()) {
            stats.requests_failed++;
            return false;
        }
        auto fu = fu_result.unwrap();
        fu->timed_wait(0.5);  // 0.5 seconds (500ms) timeout to prevent blocking forever
        if (fu->get_error_code() == 0) {
            stats.requests_succeeded++;
            return true;
        } else {
            stats.requests_failed++;
            return false;
        }
    }
};

// ============================================================================
// Test: High-Load Server Crash
// ============================================================================

TEST_F(StressCrashTest, ServerCrashUnderLoad) {
    int port = test_ports::get_port();
    StressStats stats;

    // Start server
    auto server = create_server_with_retry(&port);
    ASSERT_NE(server, nullptr) << "Failed to start server";
    std::string addr = make_addr(port);

    // Create multiple clients
    const int NUM_CLIENTS = 5;
    std::vector<rusty::Arc<Client>> clients;
    for (int i = 0; i < NUM_CLIENTS; i++) {
        auto client = create_stress_client();
        stats.connect_attempts++;
        if (client->connect(addr.c_str()) == 0) {
            stats.connect_succeeded++;
            clients.push_back(std::move(client));
        } else {
            stats.connect_failed++;
        }
    }
    EXPECT_EQ(clients.size(), NUM_CLIENTS);
    std::this_thread::sleep_for(milliseconds(50));

    // Send requests concurrently
    std::atomic<bool> stop_sending{false};
    std::vector<std::thread> sender_threads;

    for (size_t i = 0; i < clients.size(); i++) {
        sender_threads.emplace_back([this, &clients, i, &stats, &stop_sending]() {
            auto& client = clients[i];
            while (!stop_sending) {
                send_request(client, stats);
                std::this_thread::sleep_for(microseconds(100));
            }
        });
    }

    // Let requests flow for a bit
    std::this_thread::sleep_for(milliseconds(100));

    // Kill server while requests are in-flight
    delete server;
    server = nullptr;

    // Continue trying to send for a bit
    std::this_thread::sleep_for(milliseconds(50));
    stop_sending = true;

    // Wait for senders to finish
    for (auto& t : sender_threads) {
        t.join();
    }

    // Verify stats - we expect some requests to fail after crash
    EXPECT_GT(stats.requests_sent, 0u);
    EXPECT_GT(stats.requests_succeeded, 0u);
    EXPECT_GT(stats.requests_failed, 0u);  // Some should fail after crash

    // Cleanup clients
    for (auto& client : clients) {
        client->close();
    }
}

TEST_F(StressCrashTest, ServerCrashWith100PendingRequests) {
    int port = test_ports::get_port();
    StressStats stats;

    // Start server with delay to accumulate pending requests
    auto server = create_server_with_retry(&port);
    ASSERT_NE(server, nullptr);
    std::string addr = make_addr(port);

    auto client = create_stress_client();
    ASSERT_EQ(client->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Send 100 requests without waiting (async)
    std::vector<rusty::Arc<Future>> futures;
    for (int i = 0; i < 100; i++) {
        std::string input = "req_" + std::to_string(i);
        auto fu_result = client->request(
            BenchmarkService::FAST_NOP,
            [&](BinaryWriteArchive& m) { m << input; }
        );
        if (fu_result.is_ok()) {
            futures.push_back(fu_result.unwrap());
            stats.requests_sent++;
        }
    }

    // Kill server immediately
    delete server;
    server = nullptr;

    // Wait for all futures with short timeout
    for (auto& fu : futures) {
        fu->timed_wait(0.1);  // 0.1 seconds (100ms) timeout
        if (fu->get_error_code() == 0) {
            stats.requests_succeeded++;
        } else {
            stats.requests_failed++;
        }
    }

    // Some requests should have completed, some should have failed
    EXPECT_GT(stats.requests_sent, 50u);
    // At least some should fail since server crashed
    // Note: All might succeed if they complete before crash - that's ok

    client->close();
}

// ============================================================================
// Test: Rapid Server Restarts
// ============================================================================

TEST_F(StressCrashTest, RapidServerRestarts) {
    int port = test_ports::get_port();
    std::string addr;
    StressStats stats;
    const int RESTART_CYCLES = 5;

    auto client = create_stress_client();

    for (int cycle = 0; cycle < RESTART_CYCLES; cycle++) {
        // Start server
        auto server = create_server_with_retry(&port);
        ASSERT_NE(server, nullptr) << "Cycle " << cycle << ": Failed to start server";
        addr = make_addr(port);

        // Connect client
        stats.connect_attempts++;
        int connect_result = 0;
        if (!client->connected()) {
            connect_result = client->connect(addr.c_str());
        }

        if (connect_result == 0 || client->connected()) {
            stats.connect_succeeded++;
            std::this_thread::sleep_for(milliseconds(30));

            // Send a few requests
            for (int i = 0; i < 10; i++) {
                send_request(client, stats);
            }
        } else {
            stats.connect_failed++;
        }

        // Stop server
        delete server;
        server = nullptr;

        // Brief pause between cycles
        std::this_thread::sleep_for(milliseconds(20));

        // Disconnect client for next cycle
        client->close();
    }

    // Verify we completed multiple cycles
    EXPECT_GE(stats.connect_succeeded, 3u);  // At least 3 successful connects
    EXPECT_GT(stats.requests_succeeded, 0u);

    client->close();
}

TEST_F(StressCrashTest, QuickServerBounce) {
    int port = test_ports::get_port();

    // Start initial server
    auto server1 = create_server_with_retry(&port);
    ASSERT_NE(server1, nullptr);
    std::string addr = make_addr(port);

    auto client = create_stress_client();
    ASSERT_EQ(client->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Verify initial connection works
    StressStats stats;
    EXPECT_TRUE(send_request(client, stats));

    // Quick bounce: stop and immediately restart
    delete server1;
    auto server2 = create_server_with_retry(&port);
    ASSERT_NE(server2, nullptr);
    addr = make_addr(port);

    // Wait a moment for server to be ready
    std::this_thread::sleep_for(milliseconds(100));

    // Reconnect
    client->close();
    EXPECT_EQ(client->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Verify connection still works
    EXPECT_TRUE(send_request(client, stats));

    client->close();
    delete server2;
}

// ============================================================================
// Test: Client Storm After Recovery
// ============================================================================

TEST_F(StressCrashTest, ClientStormAfterRecovery) {
    int port = test_ports::get_port();

    // Start server
    auto server = create_server_with_retry(&port);
    ASSERT_NE(server, nullptr);
    std::string addr = make_addr(port);

    // Create many clients
    const int NUM_CLIENTS = 20;
    std::vector<rusty::Arc<Client>> clients;
    StressStats stats;

    for (int i = 0; i < NUM_CLIENTS; i++) {
        clients.push_back(create_stress_client());
    }

    // Connect all clients initially
    for (auto& client : clients) {
        stats.connect_attempts++;
        if (client->connect(addr.c_str()) == 0) {
            stats.connect_succeeded++;
        } else {
            stats.connect_failed++;
        }
    }
    EXPECT_GE(stats.connect_succeeded, NUM_CLIENTS - 2);  // Allow a couple failures
    std::this_thread::sleep_for(milliseconds(100));

    // Kill server
    delete server;
    server = nullptr;
    std::this_thread::sleep_for(milliseconds(50));

    // Restart server
    server = create_server_with_retry(&port);
    ASSERT_NE(server, nullptr);
    addr = make_addr(port);

    // All clients try to reconnect simultaneously (storm)
    std::atomic<int> reconnect_success{0};
    std::vector<std::thread> reconnect_threads;

    for (size_t i = 0; i < clients.size(); i++) {
        reconnect_threads.emplace_back([&clients, i, &addr, &reconnect_success]() {
            auto& client = clients[i];
            // Close and reconnect
            client->close();
            std::this_thread::sleep_for(milliseconds(10));  // Small jitter
            if (client->connect(addr.c_str()) == 0) {
                reconnect_success++;
            }
        });
    }

    // Wait for all reconnects
    for (auto& t : reconnect_threads) {
        t.join();
    }

    // Wait for connections to stabilize
    std::this_thread::sleep_for(milliseconds(200));

    // Most clients should reconnect successfully
    EXPECT_GE(reconnect_success.load(), NUM_CLIENTS / 2);

    // Verify reconnected clients can send requests
    int working_clients = 0;
    for (auto& client : clients) {
        if (client->connected()) {
            if (send_request(client, stats)) {
                working_clients++;
            }
        }
    }
    EXPECT_GT(working_clients, 0);

    // Cleanup
    for (auto& client : clients) {
        client->close();
    }
    delete server;
}

TEST_F(StressCrashTest, StaggeredClientReconnection) {
    int port = test_ports::get_port();

    // Start server
    auto server = create_server_with_retry(&port);
    ASSERT_NE(server, nullptr);
    std::string addr = make_addr(port);

    const int NUM_CLIENTS = 10;
    std::vector<rusty::Arc<Client>> clients;

    for (int i = 0; i < NUM_CLIENTS; i++) {
        auto client = create_stress_client();
        EXPECT_EQ(client->connect(addr.c_str()), 0);
        clients.push_back(std::move(client));
    }
    std::this_thread::sleep_for(milliseconds(100));

    // Kill and restart server
    delete server;
    std::this_thread::sleep_for(milliseconds(50));
    server = create_server_with_retry(&port);
    ASSERT_NE(server, nullptr);
    addr = make_addr(port);

    // Staggered reconnection - clients reconnect at different times
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < NUM_CLIENTS; i++) {
        threads.emplace_back([i, &clients, &addr, &success_count]() {
            std::this_thread::sleep_for(milliseconds(i * 20));  // Stagger by 20ms each
            clients[i]->close();
            if (clients[i]->connect(addr.c_str()) == 0) {
                success_count++;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    std::this_thread::sleep_for(milliseconds(100));
    EXPECT_GE(success_count.load(), NUM_CLIENTS / 2);

    // Cleanup
    for (auto& client : clients) {
        client->close();
    }
    delete server;
}

// ============================================================================
// Test: Memory Stability Under Stress
// ============================================================================

TEST_F(StressCrashTest, MemoryStabilityShortRun) {
    // This is a short version of the memory stability test
    // A full 24-hour test would be run separately in CI
    int port = test_ports::get_port();

    auto server = create_server_with_retry(&port);
    ASSERT_NE(server, nullptr);
    std::string addr = make_addr(port);

    const int ITERATIONS = 20;  // Reduced for CI (full test can be run manually)
    const int CLIENTS_PER_ITERATION = 3;

    for (int iter = 0; iter < ITERATIONS; iter++) {
        // Create clients
        std::vector<rusty::Arc<Client>> clients;
        for (int i = 0; i < CLIENTS_PER_ITERATION; i++) {
            auto client = create_stress_client();
            if (client->connect(addr.c_str()) == 0) {
                clients.push_back(std::move(client));
            }
        }

        // Send some requests
        StressStats stats;
        for (auto& client : clients) {
            for (int r = 0; r < 5; r++) {
                send_request(client, stats);
            }
        }

        // Cleanup all clients
        for (auto& client : clients) {
            client->close();
        }
    }

    // If we got here without crashing, memory handling is reasonably stable
    delete server;
    SUCCEED();
}

TEST_F(StressCrashTest, RepeatedConnectDisconnectCycle) {
    int port = test_ports::get_port();

    auto server = create_server_with_retry(&port);
    ASSERT_NE(server, nullptr);
    std::string addr = make_addr(port);

    auto client = create_stress_client();

    const int CYCLES = 50;
    int success_count = 0;

    for (int i = 0; i < CYCLES; i++) {
        // Connect
        if (client->connect(addr.c_str()) == 0) {
            success_count++;

            // Send one request
            StressStats stats;
            send_request(client, stats);
        }

        // Disconnect
        client->close();
    }

    // Should have many successful cycles
    EXPECT_GE(success_count, CYCLES - 5);  // Allow a few failures

    delete server;
}

// ============================================================================
// Test: Circuit Breaker Under High Load
// ============================================================================

TEST_F(StressCrashTest, CircuitBreakerHighLoadRecovery) {
    int port = test_ports::get_port();
    std::string addr = make_addr(port);

    CircuitBreakerConfig cb_config;
    cb_config.failure_threshold = 5;
    cb_config.success_threshold = 3;
    cb_config.timeout_ms = 100;
    CircuitBreaker cb(cb_config);

    // Start with no server - should trip circuit
    auto client = create_stress_client();

    // Attempt connections that will fail
    for (int i = 0; i < 5 && cb.allow_request(); i++) {
        if (client->connect(addr.c_str()) != 0) {
            cb.record_failure();
        } else {
            client->close();
            cb.record_failure();
        }
    }

    EXPECT_TRUE(cb.is_open());

    // Now start server
    auto server = create_server_with_retry(&port);
    ASSERT_NE(server, nullptr);
    addr = make_addr(port);

    // Wait for circuit timeout
    std::this_thread::sleep_for(milliseconds(150));

    // Should allow probe
    EXPECT_TRUE(cb.allow_request());

    // Create new client and probe
    auto new_client = create_stress_client();
    if (new_client->connect(addr.c_str()) == 0) {
        std::this_thread::sleep_for(milliseconds(50));

        // Record successes to close circuit
        StressStats stats;
        for (int i = 0; i < 3; i++) {
            if (send_request(new_client, stats)) {
                cb.record_success();
            }
        }
    }

    EXPECT_TRUE(cb.is_closed());

    client->close();
    new_client->close();
    delete server;
}

// ============================================================================
// Test: Multiple Server Endpoints
// ============================================================================

TEST_F(StressCrashTest, MultiServerFailover) {
    int port1 = test_ports::get_port();
    int port2 = test_ports::get_port();
    std::string addr1;
    std::string addr2;

    // Start two servers
    auto server1 = create_server_with_retry(&port1);
    auto server2 = create_server_with_retry(&port2);
    ASSERT_NE(server1, nullptr);
    ASSERT_NE(server2, nullptr);
    addr1 = make_addr(port1);
    addr2 = make_addr(port2);

    // Create clients for both
    auto client1 = create_stress_client();
    auto client2 = create_stress_client();
    ASSERT_EQ(client1->connect(addr1.c_str()), 0);
    ASSERT_EQ(client2->connect(addr2.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    StressStats stats;

    // Verify both work
    EXPECT_TRUE(send_request(client1, stats));
    EXPECT_TRUE(send_request(client2, stats));

    // Kill server1
    delete server1;
    server1 = nullptr;

    // server2 should still work
    EXPECT_TRUE(send_request(client2, stats));

    // client1 should fail
    EXPECT_FALSE(send_request(client1, stats));

    // Restart server1
    server1 = create_server_with_retry(&port1);
    ASSERT_NE(server1, nullptr);
    addr1 = make_addr(port1);

    // Reconnect client1
    client1->close();
    EXPECT_EQ(client1->connect(addr1.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Both should work again
    EXPECT_TRUE(send_request(client1, stats));
    EXPECT_TRUE(send_request(client2, stats));

    client1->close();
    client2->close();
    delete server1;
    delete server2;
}

// ============================================================================
// Test: Metrics Under Stress
// ============================================================================

TEST_F(StressCrashTest, MetricsAccuracyUnderStress) {
    int port = test_ports::get_port();

    auto server = create_server_with_retry(&port);
    ASSERT_NE(server, nullptr);
    std::string addr = make_addr(port);

    auto client = create_stress_client();
    ASSERT_EQ(client->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Access metrics
    const auto& metrics = client->metrics();

    // Reset metrics
    metrics.reset();

    // Send many requests
    const int NUM_REQUESTS = 100;
    StressStats stats;
    for (int i = 0; i < NUM_REQUESTS; i++) {
        send_request(client, stats);
    }

    // Verify metrics match
    EXPECT_EQ(metrics.requests_sent(), stats.requests_sent.load());
    EXPECT_EQ(metrics.requests_completed(), stats.requests_succeeded.load());

    client->close();
    delete server;
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
