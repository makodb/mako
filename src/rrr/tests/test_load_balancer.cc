/**
 * Load Balancer Unit Tests
 * Part of Phase 5.2: Load Balancing Strategies
 *
 * Tests for:
 * - LoadBalancerState: round-robin index tracking
 * - LoadBalancer: selection strategies (RANDOM, ROUND_ROBIN, LEAST_CONNECTIONS, LEAST_LATENCY)
 * - ClientPool integration with load balancing
 */

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <unistd.h>
#include <vector>
#include <map>
#include <rusty/arc.hpp>
#include <rusty/mutex.hpp>
#include "../rrr.hpp"
#include "benchmark_service.h"

// Atomic counter for dynamic port allocation to avoid conflicts when tests run in parallel
static std::atomic<int> g_lb_next_port{12000};

// External safety annotations
// @external: {
//   std::function::function: [unsafe]
//   std::vector::push_back: [unsafe]
//   Log_error: [unsafe]
//   std::map::erase: [unsafe]
// }

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
        std::this_thread::sleep_for(milliseconds(5));
    }
    return predicate();
}

// ===========================================================================
// LoadBalancerState Unit Tests
// ===========================================================================

class LoadBalancerStateTest : public ::testing::Test {
protected:
    LoadBalancerState state_;
};

TEST_F(LoadBalancerStateTest, RoundRobinIndexStartsAtZero) {
    // First call should return 0
    size_t idx = state_.next_round_robin_index(5);
    EXPECT_EQ(idx, 0);
}

TEST_F(LoadBalancerStateTest, RoundRobinIndexCycles) {
    const size_t pool_size = 5;

    // First full cycle
    for (size_t i = 0; i < pool_size; i++) {
        size_t idx = state_.next_round_robin_index(pool_size);
        EXPECT_EQ(idx, i);
    }

    // Second cycle should start at 0 again
    size_t idx = state_.next_round_robin_index(pool_size);
    EXPECT_EQ(idx, 0);
}

TEST_F(LoadBalancerStateTest, RoundRobinWithPoolSizeOne) {
    // Always returns 0 for single-element pool
    for (int i = 0; i < 10; i++) {
        EXPECT_EQ(state_.next_round_robin_index(1), 0);
    }
}

TEST_F(LoadBalancerStateTest, RoundRobinWithPoolSizeZero) {
    // Returns 0 for empty pool
    EXPECT_EQ(state_.next_round_robin_index(0), 0);
}

TEST_F(LoadBalancerStateTest, ResetResetsIndex) {
    // Advance the index
    state_.next_round_robin_index(5);
    state_.next_round_robin_index(5);

    // Reset
    state_.reset();

    // Should be back to 0
    EXPECT_EQ(state_.next_round_robin_index(5), 0);
}

// ===========================================================================
// LoadBalancer Strategy Unit Tests with Mock Clients
// ===========================================================================

// Mock client for testing load balancer selection
class MockClientForLB {
    ConnectionMetrics metrics_;

public:
    MockClientForLB() = default;

    // Set pending requests (sent - completed)
    void set_pending(uint64_t pending) {
        // Record enough sent/completed to get the desired pending
        for (uint64_t i = 0; i < pending + 10; i++) {
            metrics_.record_request_sent();
        }
        for (uint64_t i = 0; i < 10; i++) {
            metrics_.record_request_completed();
        }
    }

    void add_failed_requests(uint64_t count) {
        for (uint64_t i = 0; i < count; i++) {
            metrics_.record_request_sent();
            metrics_.record_request_failed();
        }
    }

    // Set latency (average)
    void set_latency(uint64_t latency_us) {
        // Record a request with this latency
        metrics_.record_request_sent();
        metrics_.record_request_completed(latency_us);  // Records latency along with completion
    }

    const ConnectionMetrics& metrics() const {
        return metrics_;
    }
};

class LoadBalancerTest : public ::testing::Test {
protected:
    LoadBalancerState state_;
    std::vector<std::shared_ptr<MockClientForLB>> clients_;

    void SetUp() override {
        // Create 4 mock clients
        for (int i = 0; i < 4; i++) {
            clients_.push_back(std::make_shared<MockClientForLB>());
        }
    }

    void TearDown() override {
        clients_.clear();
    }
};

TEST_F(LoadBalancerTest, RandomSelectionDistributes) {
    // Track how many times each index is selected
    std::map<size_t, int> counts;
    const int iterations = 1000;

    for (int i = 0; i < iterations; i++) {
        size_t idx = LoadBalancer::select(
            LoadBalancingStrategy::RANDOM,
            clients_,
            state_,
            static_cast<size_t>(i * 12345 + 67890)  // Pseudo-random seed
        );
        ASSERT_LT(idx, clients_.size());
        counts[idx]++;
    }

    // All clients should have been selected at least once
    EXPECT_EQ(counts.size(), clients_.size());

    // Each client should have been selected roughly 25% of the time
    // Allow significant variance since distribution depends on rand values
    for (auto& pair : counts) {
        EXPECT_GT(pair.second, 0);
    }
}

TEST_F(LoadBalancerTest, RoundRobinCyclesInOrder) {
    const size_t pool_size = clients_.size();

    // First full cycle
    for (size_t i = 0; i < pool_size; i++) {
        size_t idx = LoadBalancer::select(
            LoadBalancingStrategy::ROUND_ROBIN,
            clients_,
            state_,
            0  // rand_value not used
        );
        EXPECT_EQ(idx, i);
    }

    // Second cycle
    for (size_t i = 0; i < pool_size; i++) {
        size_t idx = LoadBalancer::select(
            LoadBalancingStrategy::ROUND_ROBIN,
            clients_,
            state_,
            0
        );
        EXPECT_EQ(idx, i);
    }
}

TEST_F(LoadBalancerTest, LeastConnectionsSelectsClientWithFewestPending) {
    // Client 0: 5 pending
    clients_[0]->set_pending(5);
    // Client 1: 2 pending (lowest)
    clients_[1]->set_pending(2);
    // Client 2: 10 pending
    clients_[2]->set_pending(10);
    // Client 3: 7 pending
    clients_[3]->set_pending(7);

    size_t idx = LoadBalancer::select(
        LoadBalancingStrategy::LEAST_CONNECTIONS,
        clients_,
        state_,
        0
    );

    EXPECT_EQ(idx, 1);  // Client 1 has fewest pending
}

TEST_F(LoadBalancerTest, LeastConnectionsSelectsFirstWhenEqual) {
    // All clients have same pending count
    for (auto& client : clients_) {
        client->set_pending(5);
    }

    size_t idx = LoadBalancer::select(
        LoadBalancingStrategy::LEAST_CONNECTIONS,
        clients_,
        state_,
        0
    );

    EXPECT_EQ(idx, 0);  // First client when all equal
}

TEST_F(LoadBalancerTest, LeastConnectionsUsesExplicitInFlightCounter) {
    // Client 0 has history but no active requests.
    clients_[0]->add_failed_requests(50);
    // Client 1 has one active in-flight request.
    clients_[1]->set_pending(1);

    size_t idx = LoadBalancer::select(
        LoadBalancingStrategy::LEAST_CONNECTIONS,
        clients_,
        state_,
        0
    );

    EXPECT_EQ(idx, 0);
}

TEST_F(LoadBalancerTest, LeastLatencySelectsClientWithLowestLatency) {
    // Client 0: 1000us latency
    clients_[0]->set_latency(1000);
    // Client 1: 500us latency (lowest)
    clients_[1]->set_latency(500);
    // Client 2: 2000us latency
    clients_[2]->set_latency(2000);
    // Client 3: 750us latency
    clients_[3]->set_latency(750);

    size_t idx = LoadBalancer::select(
        LoadBalancingStrategy::LEAST_LATENCY,
        clients_,
        state_,
        0
    );

    EXPECT_EQ(idx, 1);  // Client 1 has lowest latency
}

TEST_F(LoadBalancerTest, LeastLatencySkipsClientsWithNoData) {
    // Client 0: no latency data (will be skipped)
    // Client 1: 1000us latency
    clients_[1]->set_latency(1000);
    // Client 2: 500us latency (lowest with data)
    clients_[2]->set_latency(500);
    // Client 3: 2000us latency
    clients_[3]->set_latency(2000);

    size_t idx = LoadBalancer::select(
        LoadBalancingStrategy::LEAST_LATENCY,
        clients_,
        state_,
        0
    );

    EXPECT_EQ(idx, 2);  // Client 2 has lowest latency among those with data
}

TEST_F(LoadBalancerTest, EmptyPoolReturnsZero) {
    std::vector<std::shared_ptr<MockClientForLB>> empty_clients;

    for (auto strategy : {LoadBalancingStrategy::RANDOM,
                          LoadBalancingStrategy::ROUND_ROBIN,
                          LoadBalancingStrategy::LEAST_CONNECTIONS,
                          LoadBalancingStrategy::LEAST_LATENCY}) {
        size_t idx = LoadBalancer::select(strategy, empty_clients, state_, 12345);
        EXPECT_EQ(idx, 0);
    }
}

TEST_F(LoadBalancerTest, SingleClientAlwaysSelected) {
    std::vector<std::shared_ptr<MockClientForLB>> single_client;
    single_client.push_back(std::make_shared<MockClientForLB>());
    single_client[0]->set_pending(5);
    single_client[0]->set_latency(1000);

    for (auto strategy : {LoadBalancingStrategy::RANDOM,
                          LoadBalancingStrategy::ROUND_ROBIN,
                          LoadBalancingStrategy::LEAST_CONNECTIONS,
                          LoadBalancingStrategy::LEAST_LATENCY}) {
        size_t idx = LoadBalancer::select(strategy, single_client, state_, 12345);
        EXPECT_EQ(idx, 0);
    }
}

// ===========================================================================
// Strategy String Conversion Tests
// ===========================================================================

TEST(LoadBalancingStrategyTest, ToStringConversion) {
    EXPECT_STREQ(load_balancing_strategy_to_string(LoadBalancingStrategy::RANDOM), "RANDOM");
    EXPECT_STREQ(load_balancing_strategy_to_string(LoadBalancingStrategy::ROUND_ROBIN), "ROUND_ROBIN");
    EXPECT_STREQ(load_balancing_strategy_to_string(LoadBalancingStrategy::LEAST_CONNECTIONS), "LEAST_CONNECTIONS");
    EXPECT_STREQ(load_balancing_strategy_to_string(LoadBalancingStrategy::LEAST_LATENCY), "LEAST_LATENCY");
    EXPECT_STREQ(load_balancing_strategy_to_string(static_cast<LoadBalancingStrategy>(99)), "UNKNOWN");
}

// ===========================================================================
// ClientPool Integration Tests with Load Balancing
// ===========================================================================

class TestServiceForLB : public benchmark::BenchmarkService {
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
        call_count++;
        BenchmarkService::RpcFastPrimeResponse resp{};
        resp.flag = (req.n > 1) ? 1 : 0;
        return rusty::Result<BenchmarkService::RpcFastPrimeResponse, i32>::Ok(resp);
    }

    rusty::Result<BenchmarkService::RpcFastVecResponse, i32>
    fast_vec(const BenchmarkService::RpcFastVecRequest& req) override {
        call_count++;
        BenchmarkService::RpcFastVecResponse resp{};
        (void)req;
        return rusty::Result<BenchmarkService::RpcFastVecResponse, i32>::Ok(resp);
    }

    rusty::Result<BenchmarkService::RpcSleepResponse, i32>
    sleep(const BenchmarkService::RpcSleepRequest& req) override {
        call_count++;
        std::this_thread::sleep_for(std::chrono::duration<double>(req.sec));
        BenchmarkService::RpcSleepResponse resp{};
        return rusty::Result<BenchmarkService::RpcSleepResponse, i32>::Ok(resp);
    }
};

class ClientPoolLoadBalancerTest : public ::testing::Test {
protected:
    rusty::Option<rusty::Arc<PollThread>> poll_thread_;
    Server* server_;
    TestServiceForLB* service_;
    int test_port_;
    std::string addr_;

    ClientPoolLoadBalancerTest()
        : test_port_(g_lb_next_port.fetch_add(1)) {
        addr_ = "127.0.0.1:" + std::to_string(test_port_);
    }

    void SetUp() override {
        // Create PollThread
        auto poll_arc = PollThread::create();
        poll_thread_ = rusty::Some(std::move(poll_arc));

        // Create server
        auto poll_clone = poll_thread_.as_ref().unwrap().clone();
        server_ = new Server(rusty::Some(std::move(poll_clone)));

        // Create service
        auto service_box = rusty::make_box<TestServiceForLB>();
        service_ = service_box.get();
        server_->reg_service(std::move(service_box));
        ASSERT_EQ(server_->start(("0.0.0.0:" + std::to_string(test_port_)).c_str()), 0);

        std::this_thread::sleep_for(milliseconds(100));
    }

    void TearDown() override {
        delete server_;
        poll_thread_.as_ref().unwrap()->shutdown();
    }
};

TEST_F(ClientPoolLoadBalancerTest, PoolConfigDefaultsToRandom) {
    PoolConfig config = PoolConfig::defaults();
    EXPECT_EQ(config.load_balancing, LoadBalancingStrategy::RANDOM);
}

TEST_F(ClientPoolLoadBalancerTest, PoolConfigCanBeSetToRoundRobin) {
    PoolConfig config;
    config.load_balancing = LoadBalancingStrategy::ROUND_ROBIN;
    config.min_connections = 2;

    ClientPool pool(poll_thread_.clone(), config);

    // Get clients multiple times
    std::vector<rusty::Arc<Client>> clients_obtained;
    for (int i = 0; i < 4; i++) {
        auto client_opt = pool.get_client(addr_);
        ASSERT_TRUE(client_opt.is_some());
        clients_obtained.push_back(client_opt.unwrap());
    }

    // All clients should work
    EXPECT_EQ(clients_obtained.size(), 4);
}

TEST_F(ClientPoolLoadBalancerTest, PoolConfigCanBeSetToLeastConnections) {
    PoolConfig config;
    config.load_balancing = LoadBalancingStrategy::LEAST_CONNECTIONS;
    config.min_connections = 2;

    ClientPool pool(poll_thread_.clone(), config);

    // Get a client
    auto client_opt = pool.get_client(addr_);
    ASSERT_TRUE(client_opt.is_some());
    auto client = client_opt.unwrap();

    // Make a request to verify it works
    std::string input = "test";
    auto fu_result = client->request(
        benchmark::BenchmarkService::FAST_NOP,
        [&](Marshal& m) { m << input; }
    );
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();
    fu->wait();

    EXPECT_EQ(fu->get_error_code(), 0);
    EXPECT_EQ(service_->call_count, 1);
}

TEST_F(ClientPoolLoadBalancerTest, LeastConnectionsPrefersClientWithLowerInFlightLoad) {
    PoolConfig config;
    config.load_balancing = LoadBalancingStrategy::LEAST_CONNECTIONS;
    config.min_connections = 2;
    config.max_connections = 2;

    ClientPool pool(poll_thread_.clone(), config);

    auto busy_client_opt = pool.get_client(addr_);
    ASSERT_TRUE(busy_client_opt.is_some());
    auto busy_client = busy_client_opt.unwrap();

    // Keep one request in-flight on the selected client.
    auto sleep_result = busy_client->request(
        benchmark::BenchmarkService::SLEEP,
        [&](Marshal& m) { m << 0.30; }
    );
    ASSERT_TRUE(sleep_result.is_ok());
    auto sleep_future = sleep_result.unwrap();

    ASSERT_TRUE(wait_for_condition(
        [&]() { return busy_client->metrics().in_flight_requests() > 0; },
        milliseconds(500)));

    // With one in-flight on busy_client and zero on peer, least-connections
    // should route to the other connection.
    auto selected_opt = pool.get_client(addr_);
    ASSERT_TRUE(selected_opt.is_some());
    auto selected = selected_opt.unwrap();

    EXPECT_NE(selected->fd(), busy_client->fd());
    EXPECT_EQ(selected->metrics().in_flight_requests(), 0u);

    sleep_future->wait();
    EXPECT_EQ(sleep_future->get_error_code(), 0);
}

TEST_F(ClientPoolLoadBalancerTest, PoolConfigCanBeSetToLeastLatency) {
    PoolConfig config;
    config.load_balancing = LoadBalancingStrategy::LEAST_LATENCY;
    config.min_connections = 2;

    ClientPool pool(poll_thread_.clone(), config);

    // Get a client
    auto client_opt = pool.get_client(addr_);
    ASSERT_TRUE(client_opt.is_some());
    auto client = client_opt.unwrap();

    // Make a request to verify it works
    std::string input = "test";
    auto fu_result = client->request(
        benchmark::BenchmarkService::FAST_NOP,
        [&](Marshal& m) { m << input; }
    );
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();
    fu->wait();

    EXPECT_EQ(fu->get_error_code(), 0);
    EXPECT_EQ(service_->call_count, 1);
}

TEST_F(ClientPoolLoadBalancerTest, PoolConfigCanBeChanged) {
    PoolConfig config1;
    config1.load_balancing = LoadBalancingStrategy::RANDOM;

    ClientPool pool(poll_thread_.clone(), config1);

    // Change to round-robin
    PoolConfig config2;
    config2.load_balancing = LoadBalancingStrategy::ROUND_ROBIN;
    pool.set_pool_config(config2);

    auto new_config = pool.pool_config();
    EXPECT_EQ(new_config.load_balancing, LoadBalancingStrategy::ROUND_ROBIN);
}

// ===========================================================================
// Main
// ===========================================================================

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
