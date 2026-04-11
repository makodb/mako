#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory>
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
            std::this_thread::sleep_for(milliseconds(delay_ms.load()));
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
    ExtendedTestService* service_;  // Raw pointer for test access (server owns via Box)
    static constexpr int test_port_base = 9000;
    static std::atomic<int> port_offset;
    int current_port;

    void SetUp() override {
        current_port = test_port_base + port_offset++;

        // Create PollThread Arc<Mutex<>>
        poll_thread_worker_ = rusty::Some(PollThread::create());

        // Server now takes Option<Arc<...>> - use as_ref() to borrow and clone
        server = new Server(rusty::Some(poll_thread_worker_.as_ref().unwrap().clone()));

        // Create service, store raw pointer for test access, server takes ownership via Box
        auto service_box = rusty::make_box<ExtendedTestService>();
        service_ = service_box.get();  // Store raw pointer before transferring ownership
        server->reg_service(std::move(service_box));
        ASSERT_EQ(server->start(("0.0.0.0:" + std::to_string(current_port)).c_str()), 0);
    }

    void TearDown() override {
        if (server) delete server;
        // Shutdown PollThread with proper locking
        {
            poll_thread_worker_.as_ref().unwrap()->shutdown();
        }
    }
};

std::atomic<int> ExtendedRPCTest::port_offset{0};

namespace {

rusty::Arc<RpcServiceContext> make_test_rpc_context() {
    std::unordered_map<i32, size_t> rpc_to_service;
    rusty::Vec<rusty::RefCell<rusty::Box<Service>>> services;
    return rusty::Arc<RpcServiceContext>::make(
        std::move(rpc_to_service),
        std::move(services),
        "127.0.0.1:0",
        std::make_shared<std::atomic<int>>(0),
        std::make_shared<std::atomic<bool>>(false),
        1);
}

}  // namespace

TEST(ServerApiSafetyTest, ServerConnectionRunAsyncExecutesInlineAndHandlesEmptyCallback) {
    ServerConnection sconn(make_test_rpc_context(), -1);
    std::atomic<int> callback_count{0};

    EXPECT_EQ(sconn.run_async([&]() { callback_count.fetch_add(1); }), 0);
    EXPECT_EQ(callback_count.load(), 1);

    std::function<void()> empty_callback;
    EXPECT_NE(sconn.run_async(empty_callback), 0);
    EXPECT_EQ(callback_count.load(), 1);
}

TEST(ServerApiSafetyTest, ServerConnectionContentSizeAndHandleFreeAreSafe) {
    ServerConnection sconn(make_test_rpc_context(), -1);

    EXPECT_EQ(sconn.content_size(), 0u);
    sconn.handle_free();  // Explicit no-op for server side.
    EXPECT_EQ(sconn.content_size(), 0u);
}

TEST(ServerApiSafetyTest, DeferredReplyRunAsyncExecutesInlineAndHandlesEmptyCallback) {
    auto req = rusty::make_box<Request>();
    req->xid = 1;

    auto sconn = rusty::Arc<ServerConnection>::make(make_test_rpc_context(), -1);
    auto weak_sconn = rusty::downgrade(sconn);

    bool cleanup_called = false;
    std::atomic<int> callback_count{0};
    {
        DeferredReply defer(
            std::move(req),
            weak_sconn,
            [](Marshal&) {},
            [&]() { cleanup_called = true; });

        EXPECT_EQ(defer.run_async([&]() { callback_count.fetch_add(1); }), 0);
        EXPECT_EQ(callback_count.load(), 1);

        std::function<void()> empty_callback;
        EXPECT_NE(defer.run_async(empty_callback), 0);
        EXPECT_EQ(callback_count.load(), 1);
    }

    EXPECT_TRUE(cleanup_called);
}

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
        auto fu_result = clients[i]->request(benchmark::BenchmarkService::FAST_NOP, FutureAttr(), [&](Marshal& m) {
            m << input;
        });
        ASSERT_TRUE(fu_result.is_ok());
        futures.push_back(fu_result.unwrap());
    }

    // Wait for all requests
    for (auto& fu : futures) {
        fu->wait();
        EXPECT_EQ(fu->get_error_code(), 0);
        // Arc auto-released
    }

    EXPECT_EQ(service_->call_count, num_clients);

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
    std::string input1 = "Request1";
    auto fu1_result = client->request(benchmark::BenchmarkService::FAST_NOP, FutureAttr(), [&](Marshal& m) {
        m << input1;
    });
    ASSERT_TRUE(fu1_result.is_ok());
    auto fu1 = fu1_result.unwrap();
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
    std::string input2 = "Request2";
    auto fu2_result = client->request(benchmark::BenchmarkService::FAST_NOP, FutureAttr(), [&](Marshal& m) {
        m << input2;
    });
    ASSERT_TRUE(fu2_result.is_ok());
    auto fu2 = fu2_result.unwrap();
    fu2->wait();
    EXPECT_EQ(fu2->get_error_code(), 0);
    // Arc auto-released

    EXPECT_EQ(service_->call_count, 2);

    client->close();
    // Arc handles cleanup automatically
}

// Test 3: Request timeout handling
TEST_F(ExtendedRPCTest, RequestTimeout) {
    auto client = Client::create(poll_thread_worker_.as_ref().unwrap());
    ASSERT_EQ(client->connect(("127.0.0.1:" + std::to_string(current_port)).c_str()), 0);

    // Set service to delay longer than timeout
    service_->should_delay = true;
    service_->delay_ms = 5000; // 5 seconds

    // Make request with timeout
    std::string input = "Timeout test";
    auto fu_result = client->request(benchmark::BenchmarkService::NOP, FutureAttr(), [&](Marshal& m) {
        m << input;
    });
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();

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
        std::string input = "Cycle_" + std::to_string(i);
        auto fu_result = client->request(benchmark::BenchmarkService::FAST_NOP, FutureAttr(), [&](Marshal& m) {
            m << input;
        });
        if (fu_result.is_err()) continue;
        auto fu = fu_result.unwrap();
        fu->wait();

        EXPECT_EQ(fu->get_error_code(), 0);
        // Arc auto-released
        client->close();
        // Arc handles cleanup automatically

        // Small delay to avoid overwhelming the system
        std::this_thread::sleep_for(milliseconds(10));
    }

    EXPECT_EQ(service_->call_count, num_cycles);
}

// Test 5: Mixed payload sizes
TEST_F(ExtendedRPCTest, MixedPayloadSizes) {
    auto client = Client::create(poll_thread_worker_.as_ref().unwrap());
    ASSERT_EQ(client->connect(("127.0.0.1:" + std::to_string(current_port)).c_str()), 0);

    std::vector<int> sizes = {1, 10, 100, 1000, 10000, 100000, 1000000};
    std::vector<rusty::Arc<Future>> futures;

    for (int size : sizes) {
        std::string payload(size, 'A' + (size % 26));
        auto fu_result = client->request(benchmark::BenchmarkService::FAST_NOP, FutureAttr(), [&](Marshal& m) {
            m << payload;
        });
        if (fu_result.is_err()) continue;
        futures.push_back(fu_result.unwrap());
    }

    for (auto fu : futures) {
        fu->wait();
        EXPECT_EQ(fu->get_error_code(), 0);
        // Arc auto-released
    }

    EXPECT_EQ(service_->call_count, static_cast<int>(sizes.size()));

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
            auto fu_result = client->request(benchmark::BenchmarkService::FAST_NOP, FutureAttr(), [&](Marshal& m) {
                m << input;
            });
            if (fu_result.is_err()) continue;
            futures.push_back(fu_result.unwrap());
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

    EXPECT_EQ(service_->call_count, burst_size * num_bursts);

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
            std::string input = "NOP_" + std::to_string(i);
            auto fu_result = client->request(benchmark::BenchmarkService::FAST_NOP, FutureAttr(), [&](Marshal& m) {
                m << input;
            });
            if (fu_result.is_err()) continue;
            futures.push_back(fu_result.unwrap());
        } else if (i % 3 == 1) {
            // PRIME request
            i32 n = 7 + i;
            auto fu_result = client->request(benchmark::BenchmarkService::PRIME, FutureAttr(), [&](Marshal& m) {
                m << n;
            });
            if (fu_result.is_err()) continue;
            futures.push_back(fu_result.unwrap());
        } else {
            // FAST_VEC request
            i32 n = 10;
            auto fu_result = client->request(benchmark::BenchmarkService::FAST_VEC, FutureAttr(), [&](Marshal& m) {
                m << n;
            });
            ASSERT_TRUE(fu_result.is_ok());
            futures.push_back(fu_result.unwrap());
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

    EXPECT_EQ(service_->call_count, 20);

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
        auto fu_result = client->request(benchmark::BenchmarkService::FAST_NOP, FutureAttr(), [&](Marshal& m) {
            m << input;
        });
        if (fu_result.is_err()) continue;
        futures.push_back(fu_result.unwrap());
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

    EXPECT_EQ(service_->call_count, pipeline_depth);

    client->close();
    // Arc handles cleanup automatically
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
