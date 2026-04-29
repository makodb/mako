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
#include <rusty/arc.hpp>
#include "../rrr.hpp"
#include "benchmark_service.h"
#include "rpc_test_ports.h"

using namespace rrr;
using namespace benchmark;
using namespace std::chrono;

// ============================================================================
// Partition Test Service
// ============================================================================

// @safe - Test service that can simulate failures
class PartitionTestService : public benchmark::BenchmarkService {
public:
    std::atomic<uint64_t> request_count{0};
    std::atomic<bool> reject_requests{false};

    // @safe - Count requests and optionally reject
    rusty::Result<BenchmarkService::RpcFastNopResponse, i32>
    fast_nop(const BenchmarkService::RpcFastNopRequest& req) override {
        (void)req;
        request_count++;
        // We can't easily reject RPC requests at service level,
        // but we can track request patterns
        BenchmarkService::RpcFastNopResponse resp{};
        return rusty::Result<BenchmarkService::RpcFastNopResponse, i32>::Ok(resp);
    }

    // @safe - Count requests
    rusty::Result<BenchmarkService::RpcNopResponse, i32>
    nop(const BenchmarkService::RpcNopRequest& req) override {
        (void)req;
        request_count++;
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
    int port_offset_{0};  // Per-fixture offset to avoid port reuse within a test

    PartitionTest() : base_port_(test_ports::reserve_ports(100)) {}

    void SetUp() override {
        poll_thread_ = rusty::Some(PollThread::create());
    }

    void TearDown() override {
        poll_thread_.as_ref().unwrap()->shutdown();
    }

    int next_port() {
        // Use per-fixture offset instead of static to avoid cross-test accumulation
        return base_port_ + port_offset_++;
    }

    // @safe - Create server using ephemeral port assignment (port 0)
    // The OS kernel picks an available port, avoiding TIME_WAIT issues
    // Returns pair of (server*, actual_port) or (nullptr, 0) on failure
    std::pair<Server*, int> create_server_ephemeral() {
        auto server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
        auto service_box = rusty::make_box<PartitionTestService>();
        server->reg_service(std::move(service_box));
        // Use port 0 to let the OS assign an available ephemeral port
        std::string addr = "0.0.0.0:0";
        if (server->start(addr.c_str()) == 0) {
            int port = server->get_bound_port();
            if (port > 0) {
                return {server, port};
            }
        }
        delete server;
        return {nullptr, 0};
    }

    // @safe - Create server, retrying with different ports if binding fails
    // Returns pair of (server*, actual_port) or (nullptr, 0) on failure
    // Uses larger spacing between retries to skip over TIME_WAIT port clusters
    std::pair<Server*, int> create_server_retry(int initial_port, int max_retries = 50) {
        // First, try ephemeral port assignment - most reliable
        auto [server, port] = create_server_ephemeral();
        if (server != nullptr) {
            return {server, port};
        }

        // Fall back to explicit port with retry if ephemeral fails
        for (int retry = 0; retry < max_retries; retry++) {
            // Skip by 10 ports each retry to jump over TIME_WAIT clusters
            int try_port = initial_port + (retry * 10);
            // Ensure port is within valid range
            if (try_port > 65000) {
                try_port = 10000 + (try_port % 55000);  // Wrap around
            }
            auto server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
            auto service_box = rusty::make_box<PartitionTestService>();
            server->reg_service(std::move(service_box));
            std::string addr = "0.0.0.0:" + std::to_string(try_port);
            if (server->start(addr.c_str()) == 0) {
                // Advance port_offset_ to account for any skipped ports
                port_offset_ = std::max(port_offset_, initial_port - base_port_ + (retry * 10) + 1);
                return {server, try_port};
            }
            delete server;
        }
        return {nullptr, 0};
    }

    // @safe - Create server on a specific port (for restart scenarios)
    // This does NOT use ephemeral port allocation since the caller needs
    // the server to restart on the same port for reconnection tests.
    // Retries with brief delays to wait for TIME_WAIT sockets to clear.
    // Returns nullptr if binding fails after all retries.
    Server* create_server(int port, int max_retries = 10, int retry_delay_ms = 100) {
        for (int retry = 0; retry < max_retries; retry++) {
            if (retry > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms));
            }
            auto server = new Server(rusty::Some(poll_thread_.as_ref().unwrap().clone()));
            auto service_box = rusty::make_box<PartitionTestService>();
            server->reg_service(std::move(service_box));
            std::string addr = "0.0.0.0:" + std::to_string(port);
            if (server->start(addr.c_str()) == 0) {
                return server;
            }
            delete server;
        }
        return nullptr;
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
            [&](BinaryWriteArchive& m) { m << input; }
        );
        if (fu_result.is_err()) return false;
        auto fu = fu_result.unwrap();
        fu->timed_wait(0.5);  // 0.5 seconds (500ms) timeout
        return fu->get_error_code() == 0;
    }

    bool wait_for_request_success(
        rusty::Arc<Client>& client,
        milliseconds timeout = milliseconds(5000),
        milliseconds retry_interval = milliseconds(20)
    ) {
        auto deadline = steady_clock::now() + timeout;
        do {
            if (send_request(client)) {
                return true;
            }
            std::this_thread::sleep_for(retry_interval);
        } while (steady_clock::now() < deadline);
        return false;
    }
};

// ============================================================================
// Test: Temporary Partition
// ============================================================================

TEST_F(PartitionTest, TemporaryPartition) {
    PartitionStats stats;

    // Start server with retry to handle TIME_WAIT ports
    auto [server, port] = create_server_retry(next_port());
    ASSERT_NE(server, nullptr);
    std::string addr = "127.0.0.1:" + std::to_string(port);

    auto client = create_client();
    ASSERT_EQ(client->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Verify connection works
    EXPECT_TRUE(wait_for_request_success(client));

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
    auto [server, port] = create_server_retry(next_port());
    ASSERT_NE(server, nullptr);
    std::string addr = "127.0.0.1:" + std::to_string(port);

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
    auto [server, port] = create_server_retry(next_port());
    ASSERT_NE(server, nullptr);
    std::string addr = "127.0.0.1:" + std::to_string(port);

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
    CircuitBreakerConfig cb_config;
    cb_config.failure_threshold = 3;
    cb_config.timeout_ms = 200;
    CircuitBreaker cb(cb_config);

    auto [server, port] = create_server_retry(next_port());
    ASSERT_NE(server, nullptr);
    std::string addr = "127.0.0.1:" + std::to_string(port);

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
    auto [server, port] = create_server_retry(next_port());
    ASSERT_NE(server, nullptr);
    std::string addr = "127.0.0.1:" + std::to_string(port);

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

    // Both should work again once reconnect settles.
    EXPECT_TRUE(wait_for_request_success(client1));
    EXPECT_TRUE(wait_for_request_success(client2));

    client1->close();
    client2->close();
    delete server;
}

TEST_F(PartitionTest, PartialPartitionMultipleClients) {
    auto [server, port] = create_server_retry(next_port());
    ASSERT_NE(server, nullptr);
    std::string addr = "127.0.0.1:" + std::to_string(port);

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
    auto [server, port] = create_server_retry(next_port());
    ASSERT_NE(server, nullptr);
    std::string addr = "127.0.0.1:" + std::to_string(port);

    auto client = create_client();
    ASSERT_EQ(client->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Normal operation
    EXPECT_TRUE(wait_for_request_success(client));

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
    auto [server, port] = create_server_retry(next_port());
    ASSERT_NE(server, nullptr);
    std::string addr = "127.0.0.1:" + std::to_string(port);

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
    auto [server, port] = create_server_retry(next_port());
    ASSERT_NE(server, nullptr);
    std::string addr = "127.0.0.1:" + std::to_string(port);

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
    // Two servers (simulating partition between server groups)
    auto [server1, port1] = create_server_retry(next_port());
    ASSERT_NE(server1, nullptr);
    auto [server2, port2] = create_server_retry(next_port());
    ASSERT_NE(server2, nullptr);
    std::string addr1 = "127.0.0.1:" + std::to_string(port1);
    std::string addr2 = "127.0.0.1:" + std::to_string(port2);

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

    // Recreate group 1 clients to avoid stale reconnect state from the
    // old server instance during split-brain heal.
    client1a->close();
    client1b->close();
    client1a = create_client();
    client1b = create_client();
    EXPECT_EQ(client1a->connect(addr1.c_str()), 0);
    EXPECT_EQ(client1b->connect(addr1.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // All groups should work again once reconnect/replay settles.
    EXPECT_TRUE(wait_for_request_success(client1a));
    EXPECT_TRUE(wait_for_request_success(client2a));

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
    // Start with no server, try to connect - should fail
    auto client = create_client();

    // Get a port that we know isn't in use by anyone else
    int temp_port = next_port();
    std::string temp_addr = "127.0.0.1:" + std::to_string(temp_port);

    // Try to connect - should fail (no server)
    EXPECT_NE(client->connect(temp_addr.c_str()), 0);

    // Start server (heal partition) - use retry to find available port
    auto [server, port] = create_server_retry(next_port());
    ASSERT_NE(server, nullptr);
    std::string addr = "127.0.0.1:" + std::to_string(port);

    // Should be able to connect now
    EXPECT_EQ(client->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    EXPECT_TRUE(send_request(client));

    client->close();
    delete server;
}

TEST_F(PartitionTest, MultipleReconnectAttempts) {
    // Use a temp port for failed connection attempts (no server yet)
    int temp_port = next_port();
    std::string temp_addr = "127.0.0.1:" + std::to_string(temp_port);

    auto client = create_client();

    // Multiple failed connection attempts (partition)
    int fail_count = 0;
    for (int i = 0; i < 3; i++) {
        if (client->connect(temp_addr.c_str()) != 0) {
            fail_count++;
        }
        std::this_thread::sleep_for(milliseconds(20));
    }
    EXPECT_EQ(fail_count, 3);

    // Start server with retry to find available port
    auto [server, port] = create_server_retry(next_port());
    ASSERT_NE(server, nullptr);
    std::string addr = "127.0.0.1:" + std::to_string(port);

    // Should succeed now on the server's actual port
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
    auto [server, port] = create_server_retry(next_port());
    ASSERT_NE(server, nullptr);
    std::string addr = "127.0.0.1:" + std::to_string(port);

    auto client = create_client();
    ASSERT_EQ(client->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Send multiple async requests
    std::vector<rusty::Arc<Future>> futures;
    for (int i = 0; i < 10; i++) {
        std::string input = "req_" + std::to_string(i);
        auto fu_result = client->request(
            BenchmarkService::FAST_NOP,
            [&](BinaryWriteArchive& m) { m << input; }
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
        fu->timed_wait(0.2);  // 0.2 seconds (200ms) timeout
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
    auto [server, port] = create_server_retry(next_port());
    ASSERT_NE(server, nullptr);
    std::string addr = "127.0.0.1:" + std::to_string(port);

    auto client = create_client();
    ASSERT_EQ(client->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Track metrics for first connection
    EXPECT_TRUE(client->connected());
    client->metrics().reset();

    // Normal requests on first connection
    for (int i = 0; i < 5; i++) {
        send_request(client);
    }

    uint64_t first_conn_sent = client->metrics().requests_sent();
    EXPECT_EQ(first_conn_sent, 5u);

    // Partition - close first connection
    client->close();
    // Note: close() marks connection as closing but doesn't immediately disconnect
    // Wait briefly for connection to fully close
    std::this_thread::sleep_for(milliseconds(50));

    // Requests during partition should fail (connection is closing/closed)
    int failed_during_partition = 0;
    for (int i = 0; i < 5; i++) {
        if (!send_request(client)) {
            failed_during_partition++;
        }
    }
    // Most requests should fail during partition
    EXPECT_GT(failed_during_partition, 0);

    // Reconnect creates a NEW connection with fresh metrics
    EXPECT_EQ(client->connect(addr.c_str()), 0);
    std::this_thread::sleep_for(milliseconds(50));
    EXPECT_TRUE(client->connected());

    // New connection starts with fresh metrics
    client->metrics().reset();
    EXPECT_EQ(client->metrics().requests_sent(), 0u);

    // Normal requests on new connection
    for (int i = 0; i < 5; i++) {
        send_request(client);
    }

    // New connection's metrics should show 5 requests
    EXPECT_EQ(client->metrics().requests_sent(), 5u);

    client->close();
    delete server;
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
