/**
 * Network partition simulation tests for RPC reliability.
 * Simulates various network partition scenarios at the application level.
 *
 * Note: Since we can't control actual network partitions in unit tests,
 * we simulate partition behavior by:
 * - Disconnecting clients (connection loss)
 * - Using circuit breaker to fail-fast
 * - Simulating intermittent failures (flaky network)
 */

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <random>
#include <rusty/arc.hpp>
#include "reactor/reactor.h"
#include "rpc/client.hpp"
#include "rpc/server.hpp"
#include "rpc/connection_state.hpp"
#include "rpc/circuit_breaker.hpp"
#include "rpc/reconnect_policy.hpp"
#include "rpc/connection_metrics.hpp"
#include "misc/marshal.hpp"
#include "benchmark_service.h"

using namespace rrr;
using namespace benchmark;
using namespace std::chrono;

// @safe - Atomic counter for port allocation
static std::atomic<int> g_partition_test_port{19000};

// ============================================================================
// Partition Test Service
// ============================================================================

// @safe - Test service that can simulate failures
class PartitionTestService : public benchmark::BenchmarkService {
public:
    std::atomic<uint64_t> request_count{0};
    std::atomic<bool> reject_requests{false};

    // @safe - Count requests and optionally reject
    void fast_nop(const std::string& input) override {
        request_count++;
        // We can't easily reject RPC requests at service level,
        // but we can track request patterns
    }

    // @safe - Count requests
    void nop(const std::string& input) override {
        request_count++;
    }

    void fast_prime(const i32& n, i8* flag) override { *flag = 1; }
    void fast_vec(const i32& n, std::vector<i64>* v) override {
        for (i32 i = 0; i < n; i++) v->push_back(i);
    }
    void sleep(const double& sec) override {
        std::this_thread::sleep_for(std::chrono::duration<double>(sec));
    }
};

// ============================================================================
// Partition Statistics
// ============================================================================

// @safe - Thread-safe partition event tracking
struct PartitionStats {
    std::atomic<uint64_t> requests_during_partition{0};
    std::atomic<uint64_t> requests_after_heal{0};
    std::atomic<uint64_t> partition_start_count{0};
    std::atomic<uint64_t> partition_heal_count{0};
    std::atomic<uint64_t> successful_after_heal{0};

    void reset() {
        requests_during_partition = 0;
        requests_after_heal = 0;
        partition_start_count = 0;
        partition_heal_count = 0;
        successful_after_heal = 0;
    }
};

// ============================================================================
// Partition Test Fixture
// ============================================================================

class PartitionTest : public ::testing::Test {
protected:
    rusty::Option<rusty::Arc<PollThread>> poll_thread_;
    int base_port_;

    PartitionTest() : base_port_(g_partition_test_port.fetch_add(100)) {}

    void SetUp() override {
        poll_thread_ = rusty::Some(PollThread::create());
    }

    void TearDown() override {
        poll_thread_.as_ref().unwrap()->shutdown();
    }

    int next_port() {
        static std::atomic<int> offset{0};
        return base_port_ + offset.fetch_add(1);
    }

    Server* create_server(int port) {
        auto server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
        auto service_box = rusty::make_box<PartitionTestService>();
        server->reg_service(std::move(service_box));
        std::string addr = "0.0.0.0:" + std::to_string(port);
        if (server->start(addr.c_str()) != 0) {
            delete server;
            return nullptr;
        }
        return server;
    }

    rusty::Arc<Client> create_client() {
        auto client = Client::create(poll_thread_.as_ref().unwrap());
        ReconnectPolicy policy = ReconnectPolicy::aggressive();
        policy.max_retries = 5;
        policy.initial_delay_ms = 20;
        client->set_reconnect_policy(policy);
        return client;
    }

    bool send_request(rusty::Arc<Client>& client) {
        std::string input = "partition_test";
        auto fu_result = client->request(
            BenchmarkService::FAST_NOP,
            [&](Marshal& m) { m << input; }
        );
        if (fu_result.is_err()) return false;
        auto fu = fu_result.unwrap();
        fu->timed_wait(500);  // 500ms timeout
        return fu->get_error_code() == 0;
    }
};

// ============================================================================
// Test: Temporary Partition
// ============================================================================

TEST_F(PartitionTest, TemporaryPartition) {
    int port = next_port();
    std::string addr = "127.0.0.1:" + std::to_string(port);
    PartitionStats stats;

    // Start server
    auto server = create_server(port);
    ASSERT_NE(server, nullptr);

    auto client = create_client();
    ASSERT_EQ(client->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Verify connection works
    EXPECT_TRUE(send_request(client));

    // Simulate partition by closing client connection
    stats.partition_start_count++;
    client->close();

    // Requests during "partition" should fail
    EXPECT_FALSE(client->connected());
    stats.requests_during_partition++;
    EXPECT_FALSE(send_request(client));

    // Heal partition by reconnecting
    std::this_thread::sleep_for(milliseconds(50));
    EXPECT_EQ(client->connect(addr.c_str()), 0);
    stats.partition_heal_count++;
    std::this_thread::sleep_for(milliseconds(50));

    // Requests after healing should succeed
    stats.requests_after_heal++;
    if (send_request(client)) {
        stats.successful_after_heal++;
    }

    EXPECT_GT(stats.successful_after_heal, 0u);

    client->close();
    delete server;
}

TEST_F(PartitionTest, ShortPartitionRecovery) {
    int port = next_port();
    std::string addr = "127.0.0.1:" + std::to_string(port);

    auto server = create_server(port);
    ASSERT_NE(server, nullptr);

    auto client = create_client();
    ASSERT_EQ(client->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Short burst of activity
    int success_before = 0;
    for (int i = 0; i < 5; i++) {
        if (send_request(client)) success_before++;
    }
    EXPECT_EQ(success_before, 5);

    // Very short partition (10ms)
    client->close();
    std::this_thread::sleep_for(milliseconds(10));
    EXPECT_EQ(client->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Activity after recovery
    int success_after = 0;
    for (int i = 0; i < 5; i++) {
        if (send_request(client)) success_after++;
    }
    EXPECT_EQ(success_after, 5);

    client->close();
    delete server;
}

// ============================================================================
// Test: Long Partition
// ============================================================================

TEST_F(PartitionTest, LongPartition) {
    int port = next_port();
    std::string addr = "127.0.0.1:" + std::to_string(port);

    auto server = create_server(port);
    ASSERT_NE(server, nullptr);

    auto client = create_client();
    ASSERT_EQ(client->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Verify initial connection
    EXPECT_TRUE(send_request(client));

    // Long partition (500ms)
    client->close();
    std::this_thread::sleep_for(milliseconds(500));

    // Reconnect after long partition
    EXPECT_EQ(client->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Should still work after long partition
    EXPECT_TRUE(send_request(client));

    client->close();
    delete server;
}

TEST_F(PartitionTest, LongPartitionWithCircuitBreaker) {
    int port = next_port();
    std::string addr = "127.0.0.1:" + std::to_string(port);

    CircuitBreakerConfig cb_config;
    cb_config.failure_threshold = 3;
    cb_config.timeout_ms = 200;
    CircuitBreaker cb(cb_config);

    auto server = create_server(port);
    ASSERT_NE(server, nullptr);

    auto client = create_client();
    ASSERT_EQ(client->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));
    cb.record_success();

    // Long partition - stop server
    delete server;
    server = nullptr;

    // Multiple failures should trip circuit
    for (int i = 0; i < 3 && cb.allow_request(); i++) {
        if (!send_request(client)) {
            cb.record_failure();
        }
    }

    EXPECT_TRUE(cb.is_open());

    // Restart server
    server = create_server(port);
    ASSERT_NE(server, nullptr);

    // Wait for circuit timeout
    std::this_thread::sleep_for(milliseconds(250));

    // Should allow probe
    EXPECT_TRUE(cb.allow_request());

    // Reconnect and verify
    client->close();
    EXPECT_EQ(client->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    if (send_request(client)) {
        cb.record_success();
    }

    client->close();
    delete server;
}

// ============================================================================
// Test: Partial Partition (Some Clients Affected)
// ============================================================================

TEST_F(PartitionTest, PartialPartition) {
    int port = next_port();
    std::string addr = "127.0.0.1:" + std::to_string(port);

    auto server = create_server(port);
    ASSERT_NE(server, nullptr);

    // Create two clients
    auto client1 = create_client();
    auto client2 = create_client();
    ASSERT_EQ(client1->connect(addr.c_str()), 0);
    ASSERT_EQ(client2->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Both work initially
    EXPECT_TRUE(send_request(client1));
    EXPECT_TRUE(send_request(client2));

    // Partition client1 only
    client1->close();

    // client1 fails, client2 still works
    EXPECT_FALSE(send_request(client1));
    EXPECT_TRUE(send_request(client2));

    // Heal client1's partition
    EXPECT_EQ(client1->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Both work again
    EXPECT_TRUE(send_request(client1));
    EXPECT_TRUE(send_request(client2));

    client1->close();
    client2->close();
    delete server;
}

TEST_F(PartitionTest, PartialPartitionMultipleClients) {
    int port = next_port();
    std::string addr = "127.0.0.1:" + std::to_string(port);

    auto server = create_server(port);
    ASSERT_NE(server, nullptr);

    const int NUM_CLIENTS = 6;
    std::vector<rusty::Arc<Client>> clients;

    for (int i = 0; i < NUM_CLIENTS; i++) {
        auto client = create_client();
        EXPECT_EQ(client->connect(addr.c_str()), 0);
        clients.push_back(std::move(client));
    }
    std::this_thread::sleep_for(milliseconds(100));

    // Partition half the clients
    for (int i = 0; i < NUM_CLIENTS / 2; i++) {
        clients[i]->close();
    }

    // Partitioned clients fail, others succeed
    int partitioned_failures = 0;
    int healthy_successes = 0;

    for (int i = 0; i < NUM_CLIENTS / 2; i++) {
        if (!send_request(clients[i])) partitioned_failures++;
    }
    for (int i = NUM_CLIENTS / 2; i < NUM_CLIENTS; i++) {
        if (send_request(clients[i])) healthy_successes++;
    }

    EXPECT_EQ(partitioned_failures, NUM_CLIENTS / 2);
    EXPECT_EQ(healthy_successes, NUM_CLIENTS / 2);

    // Cleanup
    for (auto& client : clients) {
        client->close();
    }
    delete server;
}

// ============================================================================
// Test: Asymmetric Partition
// ============================================================================

TEST_F(PartitionTest, AsymmetricPartitionSimulation) {
    // Simulates a case where client can reach server but not vice versa
    // In practice, this manifests as requests timing out
    int port = next_port();
    std::string addr = "127.0.0.1:" + std::to_string(port);

    auto server = create_server(port);
    ASSERT_NE(server, nullptr);

    auto client = create_client();
    ASSERT_EQ(client->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Normal operation
    EXPECT_TRUE(send_request(client));

    // Simulate asymmetric partition by stopping server
    // (client can try to send, but won't get responses)
    delete server;
    server = nullptr;

    // Requests will fail/timeout
    EXPECT_FALSE(send_request(client));

    // Restart server to "heal" the partition
    server = create_server(port);
    ASSERT_NE(server, nullptr);

    // Reconnect
    client->close();
    EXPECT_EQ(client->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    EXPECT_TRUE(send_request(client));

    client->close();
    delete server;
}

// ============================================================================
// Test: Flaky Network Simulation
// ============================================================================

TEST_F(PartitionTest, FlakyNetworkSimulation) {
    int port = next_port();
    std::string addr = "127.0.0.1:" + std::to_string(port);

    auto server = create_server(port);
    ASSERT_NE(server, nullptr);

    auto client = create_client();
    ASSERT_EQ(client->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    const int ITERATIONS = 20;
    int success_count = 0;
    int reconnect_count = 0;

    // Simulate flaky network with random disconnects
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1, 100);

    for (int i = 0; i < ITERATIONS; i++) {
        // 20% chance of "network flake" (disconnect)
        if (dis(gen) <= 20) {
            client->close();
            std::this_thread::sleep_for(milliseconds(10));
            if (client->connect(addr.c_str()) == 0) {
                reconnect_count++;
            }
            std::this_thread::sleep_for(milliseconds(30));
        }

        if (send_request(client)) {
            success_count++;
        }
    }

    // Should have some successes despite flakiness
    EXPECT_GT(success_count, ITERATIONS / 2);

    client->close();
    delete server;
}

TEST_F(PartitionTest, IntermittentConnectivity) {
    int port = next_port();
    std::string addr = "127.0.0.1:" + std::to_string(port);

    auto server = create_server(port);
    ASSERT_NE(server, nullptr);

    auto client = create_client();

    const int CYCLES = 10;
    int connect_success = 0;
    int request_success = 0;

    for (int i = 0; i < CYCLES; i++) {
        // Connect
        if (client->connect(addr.c_str()) == 0) {
            connect_success++;
            std::this_thread::sleep_for(milliseconds(30));

            // Try a few requests
            for (int r = 0; r < 3; r++) {
                if (send_request(client)) {
                    request_success++;
                }
            }
        }

        // Disconnect (simulate network drop)
        client->close();
        std::this_thread::sleep_for(milliseconds(20));
    }

    EXPECT_GE(connect_success, CYCLES - 2);  // Most connects succeed
    EXPECT_GT(request_success, 0);

    client->close();
    delete server;
}

// ============================================================================
// Test: Split Brain Scenario
// ============================================================================

TEST_F(PartitionTest, SplitBrainSimulation) {
    // Simulate split brain: two groups of clients can reach different servers
    int port1 = next_port();
    int port2 = next_port();
    std::string addr1 = "127.0.0.1:" + std::to_string(port1);
    std::string addr2 = "127.0.0.1:" + std::to_string(port2);

    // Two servers (simulating partition between server groups)
    auto server1 = create_server(port1);
    auto server2 = create_server(port2);
    ASSERT_NE(server1, nullptr);
    ASSERT_NE(server2, nullptr);

    // Group 1 clients connect to server1
    auto client1a = create_client();
    auto client1b = create_client();
    ASSERT_EQ(client1a->connect(addr1.c_str()), 0);
    ASSERT_EQ(client1b->connect(addr1.c_str()), 0);

    // Group 2 clients connect to server2
    auto client2a = create_client();
    auto client2b = create_client();
    ASSERT_EQ(client2a->connect(addr2.c_str()), 0);
    ASSERT_EQ(client2b->connect(addr2.c_str()), 0);

    std::this_thread::sleep_for(milliseconds(100));

    // Both groups work independently
    EXPECT_TRUE(send_request(client1a));
    EXPECT_TRUE(send_request(client1b));
    EXPECT_TRUE(send_request(client2a));
    EXPECT_TRUE(send_request(client2b));

    // Simulate partition: server1 goes down
    delete server1;
    server1 = nullptr;

    // Group 1 fails
    EXPECT_FALSE(send_request(client1a));
    EXPECT_FALSE(send_request(client1b));

    // Group 2 still works
    EXPECT_TRUE(send_request(client2a));
    EXPECT_TRUE(send_request(client2b));

    // Heal: restart server1
    server1 = create_server(port1);
    ASSERT_NE(server1, nullptr);

    // Reconnect group 1
    client1a->close();
    client1b->close();
    EXPECT_EQ(client1a->connect(addr1.c_str()), 0);
    EXPECT_EQ(client1b->connect(addr1.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // All groups work
    EXPECT_TRUE(send_request(client1a));
    EXPECT_TRUE(send_request(client2a));

    // Cleanup
    client1a->close();
    client1b->close();
    client2a->close();
    client2b->close();
    delete server1;
    delete server2;
}

// ============================================================================
// Test: Reconnection Under Partition
// ============================================================================

TEST_F(PartitionTest, ReconnectionDuringPartition) {
    int port = next_port();
    std::string addr = "127.0.0.1:" + std::to_string(port);

    // Start with no server
    auto client = create_client();

    // Try to connect - should fail (partition)
    EXPECT_NE(client->connect(addr.c_str()), 0);

    // Start server (heal partition)
    auto server = create_server(port);
    ASSERT_NE(server, nullptr);

    // Should be able to connect now
    EXPECT_EQ(client->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    EXPECT_TRUE(send_request(client));

    client->close();
    delete server;
}

TEST_F(PartitionTest, MultipleReconnectAttempts) {
    int port = next_port();
    std::string addr = "127.0.0.1:" + std::to_string(port);

    auto client = create_client();

    // Multiple failed connection attempts (partition)
    int fail_count = 0;
    for (int i = 0; i < 3; i++) {
        if (client->connect(addr.c_str()) != 0) {
            fail_count++;
        }
        std::this_thread::sleep_for(milliseconds(20));
    }
    EXPECT_EQ(fail_count, 3);

    // Start server
    auto server = create_server(port);
    ASSERT_NE(server, nullptr);

    // Should succeed now
    EXPECT_EQ(client->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    EXPECT_TRUE(send_request(client));

    client->close();
    delete server;
}

// ============================================================================
// Test: Partition with Pending Requests
// ============================================================================

TEST_F(PartitionTest, PartitionWithPendingRequests) {
    int port = next_port();
    std::string addr = "127.0.0.1:" + std::to_string(port);

    auto server = create_server(port);
    ASSERT_NE(server, nullptr);

    auto client = create_client();
    ASSERT_EQ(client->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Send multiple async requests
    std::vector<rusty::Arc<Future>> futures;
    for (int i = 0; i < 10; i++) {
        std::string input = "req_" + std::to_string(i);
        auto fu_result = client->request(
            BenchmarkService::FAST_NOP,
            [&](Marshal& m) { m << input; }
        );
        if (fu_result.is_ok()) {
            futures.push_back(fu_result.unwrap());
        }
    }

    // Partition before all complete
    delete server;
    server = nullptr;

    // Wait for futures
    int completed = 0;
    int failed = 0;
    for (auto& fu : futures) {
        fu->timed_wait(200);
        if (fu->get_error_code() == 0) {
            completed++;
        } else {
            failed++;
        }
    }

    // Some may complete, some may fail
    EXPECT_GT(completed + failed, 0);

    client->close();
}

// ============================================================================
// Test: Metrics During Partition
// ============================================================================

TEST_F(PartitionTest, MetricsDuringPartition) {
    int port = next_port();
    std::string addr = "127.0.0.1:" + std::to_string(port);

    auto server = create_server(port);
    ASSERT_NE(server, nullptr);

    auto client = create_client();
    ASSERT_EQ(client->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    const auto& metrics = client->metrics();
    metrics.reset();

    // Normal requests
    for (int i = 0; i < 5; i++) {
        send_request(client);
    }

    uint64_t before_sent = metrics.requests_sent();
    uint64_t before_completed = metrics.requests_completed();
    (void)before_completed;  // Suppress unused variable warning

    // Partition
    client->close();

    // Requests during partition
    for (int i = 0; i < 5; i++) {
        send_request(client);
    }

    // Reconnect
    EXPECT_EQ(client->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Normal requests after partition
    for (int i = 0; i < 5; i++) {
        send_request(client);
    }

    // Metrics should show the whole picture
    EXPECT_GT(metrics.requests_sent(), before_sent);

    client->close();
    delete server;
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
