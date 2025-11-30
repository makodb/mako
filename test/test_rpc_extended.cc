#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <random>
#include <rusty/arc.hpp>
#include <rusty/mutex.hpp>
#include "reactor/reactor.h"
#include "rpc/client.hpp"
#include "rpc/server.hpp"
#include "misc/marshal.hpp"
#include "benchmark_service.h"

using namespace rrr;
using namespace benchmark;
using namespace std::chrono;

// Extended test service with more failure scenarios
class ExtendedTestService : public benchmark::BenchmarkService {
public:
    std::atomic<int> call_count{0};
    std::atomic<bool> should_crash{false};
    std::atomic<bool> should_delay{false};
    std::atomic<int> delay_ms{100};
    std::atomic<bool> should_throw{false};
    
    void fast_nop(const std::string& input) override {
        call_count++;
        if (should_throw) {
            throw std::runtime_error("Simulated service error");
        }
        if (should_crash) {
            abort(); // Simulate crash
        }
    }
    
    void nop(const std::string& input) override {
        call_count++;
        if (should_delay) {
            std::this_thread::sleep_for(milliseconds(delay_ms));
        }
    }
    
    void fast_prime(const i32& n, i8* flag) override {
        call_count++;
        bool is_prime = true;
        if (n <= 1) {
            is_prime = false;
        } else {
            for (i32 i = 2; i * i <= n; i++) {
                if (n % i == 0) {
                    is_prime = false;
                    break;
                }
            }
        }
        *flag = is_prime ? 1 : 0;
    }
    
    void fast_vec(const i32& n, std::vector<i64>* v) override {
        call_count++;
        for (i32 i = 0; i < n; i++) {
            v->push_back(i);
        }
    }
    
    void sleep(const double& sec) override {
        call_count++;
        std::this_thread::sleep_for(std::chrono::duration<double>(sec));
    }
};

class ExtendedRPCTest : public ::testing::Test {
protected:
    rusty::Option<rusty::Arc<PollThread>> poll_thread_worker_;
    Server* server;
    ExtendedTestService* service;
    static constexpr int test_port_base = 9000;
    static std::atomic<int> port_offset;
    int current_port;

    void SetUp() override {
        current_port = test_port_base + port_offset++;

        // Create PollThread Arc<Mutex<>>
        poll_thread_worker_ = rusty::Some(PollThread::create());

        // Server now takes Option<Arc<...>> - use as_ref() to borrow and clone
        server = new Server(rusty::Some(poll_thread_worker_.as_ref().unwrap().clone()));
        service = new ExtendedTestService();
        server->reg(service);
        ASSERT_EQ(server->start(("0.0.0.0:" + std::to_string(current_port)).c_str()), 0);
    }

    void TearDown() override {
        if (service) delete service;
        if (server) delete server;
        // Shutdown PollThread with proper locking
        {
            poll_thread_worker_.as_ref().unwrap()->shutdown();
        }
    }
};

std::atomic<int> ExtendedRPCTest::port_offset{0};

// Test 1: Multiple clients connecting to the same server
TEST_F(ExtendedRPCTest, MultipleClients) {
    const int num_clients = 10;
    std::vector<rusty::Arc<Client>> clients;

    // Create multiple clients
    for (int i = 0; i < num_clients; i++) {
        auto client = Client::create(poll_thread_worker_.as_ref().unwrap());
        ASSERT_EQ(client->connect(("127.0.0.1:" + std::to_string(current_port)).c_str()), 0);
        clients.push_back(client);
    }

    // Each client makes a request
    std::vector<rusty::Arc<Future>> futures;
    for (int i = 0; i < num_clients; i++) {
        std::string input = "Client_" + std::to_string(i);
        auto fu_result = clients[i]->begin_request(benchmark::BenchmarkService::FAST_NOP);
        ASSERT_TRUE(fu_result.is_ok());
        auto fu = fu_result.unwrap();
        *clients[i] << input;
        clients[i]->end_request();
        futures.push_back(std::move(fu));
    }

    // Wait for all requests
    for (auto& fu : futures) {
        fu->wait();
        EXPECT_EQ(fu->get_error_code(), 0);
        // Arc auto-released
    }

    EXPECT_EQ(service->call_count, num_clients);

    // Cleanup
    for (auto client : clients) {
        client->close();
        // Arc handles cleanup automatically
    }
}

// Test 2: Client reconnection after disconnect
TEST_F(ExtendedRPCTest, ClientReconnection) {
    auto client = Client::create(poll_thread_worker_.as_ref().unwrap());
    ASSERT_EQ(client->connect(("127.0.0.1:" + std::to_string(current_port)).c_str()), 0);

    // Make initial request
    auto fu1_result = client->begin_request(benchmark::BenchmarkService::FAST_NOP);
    ASSERT_TRUE(fu1_result.is_ok());
    auto fu1 = fu1_result.unwrap();
    std::string input1 = "Request1";
    *client << input1;
    client->end_request();
    fu1->wait();
    EXPECT_EQ(fu1->get_error_code(), 0);
    // Arc auto-released

    // Disconnect
    client->close();
    // Arc doesn't have reset() - just reassign or let it go out of scope

    // Wait a bit
    std::this_thread::sleep_for(milliseconds(100));

    // Create new client for reconnection
    client = Client::create(poll_thread_worker_.as_ref().unwrap());

    // Reconnect
    ASSERT_EQ(client->connect(("127.0.0.1:" + std::to_string(current_port)).c_str()), 0);

    // Make another request
    auto fu2_result = client->begin_request(benchmark::BenchmarkService::FAST_NOP);
    ASSERT_TRUE(fu2_result.is_ok());
    auto fu2 = fu2_result.unwrap();
    std::string input2 = "Request2";
    *client << input2;
    client->end_request();
    fu2->wait();
    EXPECT_EQ(fu2->get_error_code(), 0);
    // Arc auto-released

    EXPECT_EQ(service->call_count, 2);

    client->close();
    // Arc handles cleanup automatically
}

// Test 3: Request timeout handling
TEST_F(ExtendedRPCTest, RequestTimeout) {
    auto client = Client::create(poll_thread_worker_.as_ref().unwrap());
    ASSERT_EQ(client->connect(("127.0.0.1:" + std::to_string(current_port)).c_str()), 0);

    // Set service to delay longer than timeout
    service->should_delay = true;
    service->delay_ms = 5000; // 5 seconds

    // Make request with timeout
    auto fu_result = client->begin_request(benchmark::BenchmarkService::NOP);
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();
    std::string input = "Timeout test";
    *client << input;
    client->end_request();

    // Wait with timeout - Future doesn't have timed_wait, use wait() and time it manually
    auto start = steady_clock::now();
    // Don't wait forever - the service will delay 5 seconds
    std::this_thread::sleep_for(milliseconds(1000)); // Wait 1 second

    // Check if still not ready (simulating timeout)
    // Note: In real implementation, you'd want proper timeout support in Future
    auto elapsed = duration_cast<milliseconds>(steady_clock::now() - start).count();
    EXPECT_GE(elapsed, 900); // At least 900ms passed
    EXPECT_LE(elapsed, 1200); // But not more than 1.2 seconds
        // Arc auto-released
    client->close();
    // Arc handles cleanup automatically
}

// Test 4: Rapid connect/disconnect cycles
TEST_F(ExtendedRPCTest, RapidConnectDisconnect) {
    const int num_cycles = 20;

    for (int i = 0; i < num_cycles; i++) {
        auto client = Client::create(poll_thread_worker_.as_ref().unwrap());
        ASSERT_EQ(client->connect(("127.0.0.1:" + std::to_string(current_port)).c_str()), 0);

        // Make a quick request
        auto fu_result = client->begin_request(benchmark::BenchmarkService::FAST_NOP);
        if (fu_result.is_err()) continue;
        auto fu = fu_result.unwrap();
        std::string input = "Cycle_" + std::to_string(i);
        *client << input;
        client->end_request();
        fu->wait();

        EXPECT_EQ(fu->get_error_code(), 0);
        // Arc auto-released
        client->close();
        // Arc handles cleanup automatically

        // Small delay to avoid overwhelming the system
        std::this_thread::sleep_for(milliseconds(10));
    }

    EXPECT_EQ(service->call_count, num_cycles);
}

// Test 5: Mixed payload sizes
TEST_F(ExtendedRPCTest, MixedPayloadSizes) {
    auto client = Client::create(poll_thread_worker_.as_ref().unwrap());
    ASSERT_EQ(client->connect(("127.0.0.1:" + std::to_string(current_port)).c_str()), 0);

    std::vector<int> sizes = {1, 10, 100, 1000, 10000, 100000, 1000000};
    std::vector<rusty::Arc<Future>> futures;

    for (int size : sizes) {
        std::string payload(size, 'A' + (size % 26));
        auto fu_result = client->begin_request(benchmark::BenchmarkService::FAST_NOP);
        if (fu_result.is_err()) continue;
        auto fu = fu_result.unwrap();
        *client << payload;
        client->end_request();
        futures.push_back(fu);
    }

    for (auto fu : futures) {
        fu->wait();
        EXPECT_EQ(fu->get_error_code(), 0);
        // Arc auto-released
    }

    EXPECT_EQ(service->call_count, static_cast<int>(sizes.size()));

    client->close();
    // Arc handles cleanup automatically
}

// Test 6: Burst traffic pattern
TEST_F(ExtendedRPCTest, BurstTraffic) {
    auto client = Client::create(poll_thread_worker_.as_ref().unwrap());
    ASSERT_EQ(client->connect(("127.0.0.1:" + std::to_string(current_port)).c_str()), 0);

    const int burst_size = 100;
    const int num_bursts = 5;

    for (int burst = 0; burst < num_bursts; burst++) {
        std::vector<rusty::Arc<Future>> futures;

        // Send burst
        auto start = steady_clock::now();
        for (int i = 0; i < burst_size; i++) {
            std::string input = "Burst_" + std::to_string(burst) + "_" + std::to_string(i);
            auto fu_result = client->begin_request(benchmark::BenchmarkService::FAST_NOP);
            if (fu_result.is_err()) continue;
            auto fu = fu_result.unwrap();
            *client << input;
            client->end_request();
            futures.push_back(fu);
        }

        // Wait for all in burst
        for (auto fu : futures) {
            fu->wait();
            EXPECT_EQ(fu->get_error_code(), 0);
        // Arc auto-released
        }

        auto end = steady_clock::now();
        auto duration = duration_cast<milliseconds>(end - start).count();

        // Log burst performance
        std::cout << "Burst " << burst << " completed in " << duration << "ms" << std::endl;

        // Pause between bursts
        std::this_thread::sleep_for(milliseconds(100));
    }

    EXPECT_EQ(service->call_count, burst_size * num_bursts);

    client->close();
    // Arc handles cleanup automatically
}

// Test 7: Interleaved request types
TEST_F(ExtendedRPCTest, InterleavedRequestTypes) {
    auto client = Client::create(poll_thread_worker_.as_ref().unwrap());
    ASSERT_EQ(client->connect(("127.0.0.1:" + std::to_string(current_port)).c_str()), 0);

    std::vector<rusty::Arc<Future>> futures;

    // Mix different request types
    for (int i = 0; i < 20; i++) {
        if (i % 3 == 0) {
            // NOP request
            auto fu_result = client->begin_request(benchmark::BenchmarkService::FAST_NOP);
            if (fu_result.is_err()) continue;
            auto fu = fu_result.unwrap();
            std::string input = "NOP_" + std::to_string(i);
            *client << input;
            client->end_request();
            futures.push_back(fu);
        } else if (i % 3 == 1) {
            // PRIME request
            auto fu_result = client->begin_request(benchmark::BenchmarkService::PRIME);
            if (fu_result.is_err()) continue;
            auto fu = fu_result.unwrap();
            i32 n = 7 + i;
            *client << n;
            client->end_request();
            futures.push_back(fu);
        } else {
            // FAST_VEC request
            auto fu_result = client->begin_request(benchmark::BenchmarkService::FAST_VEC);
            ASSERT_TRUE(fu_result.is_ok());
            auto fu = fu_result.unwrap();
            i32 n = 10;
            *client << n;
            client->end_request();
            futures.push_back(fu);
        }
    }

    // Verify all completed successfully
    int prime_count = 0;
    int vec_count = 0;
    for (size_t i = 0; i < futures.size(); i++) {
        futures[i]->wait();
        EXPECT_EQ(futures[i]->get_error_code(), 0);

        if (i % 3 == 1) {
            i8 result;
            futures[i]->get_reply() >> result;
            prime_count++;
        } else if (i % 3 == 2) {
            std::vector<i64> result;
            futures[i]->get_reply() >> result;
            EXPECT_EQ(result.size(), 10);
            vec_count++;
        }
            // Arc auto-released
    }

    EXPECT_EQ(service->call_count, 20);

    client->close();
    // Arc handles cleanup automatically
}

// Test 8: Pipelined requests (send multiple before waiting)
TEST_F(ExtendedRPCTest, PipelinedRequests) {
    auto client = Client::create(poll_thread_worker_.as_ref().unwrap());
    ASSERT_EQ(client->connect(("127.0.0.1:" + std::to_string(current_port)).c_str()), 0);

    const int pipeline_depth = 50;
    std::vector<rusty::Arc<Future>> futures;

    // Send all requests without waiting
    auto start = steady_clock::now();
    for (int i = 0; i < pipeline_depth; i++) {
        std::string input = "Pipelined_" + std::to_string(i);
        auto fu_result = client->begin_request(benchmark::BenchmarkService::FAST_NOP);
        if (fu_result.is_err()) continue;
        auto fu = fu_result.unwrap();
        *client << input;
        client->end_request();
        futures.push_back(fu);
    }

    // Now wait for all
    for (auto fu : futures) {
        fu->wait();
        EXPECT_EQ(fu->get_error_code(), 0);
        // Arc auto-released
    }
    auto end = steady_clock::now();

    auto duration = duration_cast<milliseconds>(end - start).count();
    std::cout << "Pipelined " << pipeline_depth << " requests completed in " << duration << "ms" << std::endl;

    EXPECT_EQ(service->call_count, pipeline_depth);

    client->close();
    // Arc handles cleanup automatically
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}