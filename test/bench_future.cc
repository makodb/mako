#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <iostream>
#include <iomanip>
#include <rusty/arc.hpp>
#include <rusty/mutex.hpp>
#include "reactor/reactor.h"
#include "rpc/client.hpp"
#include "rpc/server.hpp"
#include "misc/marshal.hpp"

using namespace rrr;
using namespace std::chrono;

// Simple test service for benchmarking
class BenchService : public Service {
public:
    enum {
        ECHO = 0x2001,
    };

    std::atomic<int> call_count{0};

    int __reg_to__(Server* svr) {
        return svr->reg(ECHO, this, &BenchService::echo_wrapper);
    }

private:
    void echo_wrapper(rusty::Box<Request> req, WeakServerConnection weak_sconn) {
        call_count++;
        i32 input;
        req->m >> input;

        auto sconn_opt = weak_sconn.upgrade();
        if (sconn_opt.is_some()) {
            auto sconn = sconn_opt.unwrap();
            const_cast<ServerConnection&>(*sconn).begin_reply(*req);
            const_cast<ServerConnection&>(*sconn) << input;
            const_cast<ServerConnection&>(*sconn).end_reply();
        }
    }
};

class FutureBenchmark : public ::testing::Test {
protected:
    rusty::Option<rusty::Arc<PollThread>> poll_thread_worker_;
    Server* server;
    BenchService* service;
    rusty::Option<rusty::Arc<Client>> client;
    static constexpr int base_port = 8950;

    void SetUp() override {
        poll_thread_worker_ = rusty::Some(PollThread::create());
        // Clone the Arc to keep our copy for the client - use as_ref() to borrow
        server = new Server(rusty::Some(poll_thread_worker_.as_ref().unwrap().clone()));
        service = new BenchService();

        server->reg(service);
        ASSERT_EQ(server->start(("0.0.0.0:" + std::to_string(base_port)).c_str()), 0);

        client = rusty::Some(Client::create(poll_thread_worker_.as_ref().unwrap()));
        ASSERT_EQ(client.as_ref().unwrap()->connect(("127.0.0.1:" + std::to_string(base_port)).c_str()), 0);

        std::this_thread::sleep_for(milliseconds(100));
    }

    void TearDown() override {
        client.as_ref().unwrap()->close();
        delete service;
        delete server;
        poll_thread_worker_.as_ref().unwrap()->shutdown();
        std::this_thread::sleep_for(milliseconds(100));
    }

    // Helper to format numbers with thousands separator
    std::string format_number(long long num) {
        std::stringstream ss;
        ss.imbue(std::locale(""));
        ss << std::fixed << num;
        return ss.str();
    }

    // Helper to print benchmark results
    void print_result(const std::string& name, int iterations, double dur_sec) {
        double ops_per_sec = iterations / dur_sec;
        double ns_per_op = (dur_sec * 1e9) / iterations;

        std::cout << "\n[BENCHMARK] " << name << "\n";
        std::cout << "  Iterations:  " << format_number(iterations) << "\n";
        std::cout << "  Duration:    " << std::fixed << std::setprecision(3)
                  << dur_sec << " sec\n";
        std::cout << "  Throughput:  " << format_number((long long)ops_per_sec)
                  << " ops/sec\n";
        std::cout << "  Latency:     " << std::fixed << std::setprecision(2)
                  << ns_per_op << " ns/op\n";
    }
};

// Benchmark: Future creation and immediate release
TEST_F(FutureBenchmark, CreateReleaseThroughput) {
    const int iterations = 10000;

    auto start = high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        auto fu_result = client.as_ref().unwrap()->begin_request(BenchService::ECHO);
        ASSERT_TRUE(fu_result.is_ok());
        auto fu = fu_result.unwrap();
        i32 val = i;
        *client.as_ref().unwrap() << val;
        client.as_ref().unwrap()->end_request();
        // Arc auto-released (fire-and-forget)
    }

    auto end = high_resolution_clock::now();
    double dur = duration_cast<std::chrono::duration<double>>(end - start).count();

    print_result("Future Create/Release (fire-and-forget)", iterations, dur);
}

// Benchmark: Future creation, wait, and release
TEST_F(FutureBenchmark, CreateWaitReleaseThroughput) {
    const int iterations = 1000;  // Fewer iterations since we wait

    auto start = high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        auto fu_result = client.as_ref().unwrap()->begin_request(BenchService::ECHO);
        ASSERT_TRUE(fu_result.is_ok());
        auto fu = fu_result.unwrap();
        i32 val = i;
        *client.as_ref().unwrap() << val;
        client.as_ref().unwrap()->end_request();

        fu->wait();
        i32 result;
        fu->get_reply() >> result;
        // Arc auto-released
    }

    auto end = high_resolution_clock::now();
    double dur = duration_cast<std::chrono::duration<double>>(end - start).count();

    print_result("Future Create/Wait/Release (round-trip)", iterations, dur);
}

// Benchmark: Batch operations (create many, wait all, release all)
TEST_F(FutureBenchmark, BatchOperations) {
    const int batch_size = 100;
    const int num_batches = 50;
    const int total_ops = batch_size * num_batches;

    auto start = high_resolution_clock::now();

    for (int batch = 0; batch < num_batches; batch++) {
        std::vector<rusty::Arc<Future>> futures;

        // Create batch
        for (int i = 0; i < batch_size; i++) {
            auto fu_result = client.as_ref().unwrap()->begin_request(BenchService::ECHO);
            ASSERT_TRUE(fu_result.is_ok());
            auto fu = fu_result.unwrap();
            i32 val = batch * batch_size + i;
            *client.as_ref().unwrap() << val;
            client.as_ref().unwrap()->end_request();
            futures.push_back(fu);
        }

        // Wait and release all
        for (auto& fu : futures) {
            fu->wait();
            i32 result;
            fu->get_reply() >> result;
            // Arc auto-released
        }
    }

    auto end = high_resolution_clock::now();
    double dur = duration_cast<std::chrono::duration<double>>(end - start).count();

    print_result("Batch Operations (create/wait/release)", total_ops, dur);
}

// Benchmark: Arc copy overhead (simulates passing Future around)
TEST_F(FutureBenchmark, RefCopyOverhead) {
    const int iterations = 10000;

    auto start = high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        auto fu_result = client.as_ref().unwrap()->begin_request(BenchService::ECHO);
        ASSERT_TRUE(fu_result.is_ok());
        auto fu = fu_result.unwrap();
        i32 val = i;
        *client.as_ref().unwrap() << val;
        client.as_ref().unwrap()->end_request();

        // Simulate passing Future around (Arc copies)
        auto fu_copy1 = fu;
        auto fu_copy2 = fu;
        auto fu_copy3 = fu;

        // Arc auto-released when copies go out of scope
    }

    auto end = high_resolution_clock::now();
    double dur = duration_cast<std::chrono::duration<double>>(end - start).count();

    print_result("Arc copy Overhead (3 copies per Future)", iterations, dur);
}

// Benchmark: Callback overhead
TEST_F(FutureBenchmark, CallbackOverhead) {
    const int iterations = 1000;
    std::atomic<int> callback_count{0};

    auto start = high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        FutureAttr attr([&callback_count](rusty::Arc<Future> f) {
            callback_count++;
            i32 result;
            f->get_reply() >> result;
        });

        auto fu_result = client.as_ref().unwrap()->begin_request(BenchService::ECHO, attr);
        ASSERT_TRUE(fu_result.is_ok());
        auto fu = fu_result.unwrap();
        i32 val = i;
        *client.as_ref().unwrap() << val;
        client.as_ref().unwrap()->end_request();

        fu->wait();
        // Arc auto-released
    }

    auto end = high_resolution_clock::now();

    // Wait for all callbacks to complete
    std::this_thread::sleep_for(milliseconds(100));

    double dur = duration_cast<std::chrono::duration<double>>(end - start).count();

    print_result("Callback Execution Overhead", iterations, dur);

    std::cout << "  Callbacks executed: " << callback_count.load() << "\n";
}

// Benchmark: Memory allocation overhead (Future object creation)
TEST_F(FutureBenchmark, AllocationOverhead) {
    const int iterations = 100000;

    // Measure just the allocation/deallocation without RPC overhead
    auto start = high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        // Create Future using factory (Arc manages allocation)
        FutureAttr attr;
        auto fu = Future::create(i, attr);
        // Arc auto-released when fu goes out of scope
    }

    auto end = high_resolution_clock::now();
    double dur = duration_cast<std::chrono::duration<double>>(end - start).count();

    print_result("Future Allocation/Deallocation (Arc, no RPC)", iterations, dur);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);

    std::cout << "\n========================================\n";
    std::cout << "  Future Performance (Arc<Future>)\n";
    std::cout << "========================================\n";

    int result = RUN_ALL_TESTS();

    std::cout << "\n========================================\n";
    std::cout << "  Benchmark Complete\n";
    std::cout << "========================================\n\n";

    return result;
}
