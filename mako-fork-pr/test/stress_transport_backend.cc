// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * stress_transport_backend.cc:
 *   Stress tests for transport backend interface
 *   Tests throughput, concurrency, memory stability using mock implementations
 *
 *   NOTE: These tests use mock implementations to avoid pulling in
 *   full mako/eRPC dependencies. They test the interface contract and
 *   performance characteristics that backends must satisfy.
 *
 **********************************************************************/

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <memory>
#include <mutex>
#include <cstdlib>
#include <vector>
#include <random>
#include <map>
#include <string>
#include <stdexcept>

using namespace std::chrono;

// ============= Inline Transport Type Definitions =============

namespace mako {

enum class TransportType {
    ERPC = 0,
    RRR_RPC = 1
};

inline TransportType ParseTransportType(const std::string& type_str) {
    if (type_str == "erpc" || type_str == "ERPC") {
        return TransportType::ERPC;
    } else if (type_str == "rrr" || type_str == "RRR" || type_str == "rrr_rpc") {
        return TransportType::RRR_RPC;
    } else {
        throw std::runtime_error("Invalid transport type: " + type_str +
                                " (valid: erpc, rrr)");
    }
}

inline const char* TransportTypeToString(TransportType type) {
    switch (type) {
        case TransportType::ERPC: return "erpc";
        case TransportType::RRR_RPC: return "rrr";
        default: return "unknown";
    }
}

} // namespace mako

// ============= Mock TransportReceiver =============

class TransportReceiver {
public:
    virtual ~TransportReceiver() = default;
    virtual void ReceiveResponse(uint8_t reqType, char* respBuf) = 0;
    virtual bool Blocked() = 0;
};

// ============= StressMockBackend =============

class StressMockBackend {
public:
    std::atomic<uint64_t> op_count{0};
    std::atomic<uint64_t> bytes_allocated{0};
    std::atomic<bool> stopped{false};
    std::vector<char> buffer_;
    mutable std::mutex buffer_mutex_;
    mako::TransportType type_;

    explicit StressMockBackend(mako::TransportType type = mako::TransportType::RRR_RPC)
        : type_(type) {}

    virtual ~StressMockBackend() = default;

    virtual int Initialize(const std::string& local_uri,
                          uint8_t numa_node,
                          uint8_t phy_port,
                          uint8_t st_nr_req_types,
                          uint8_t end_nr_req_types) {
        op_count++;
        return 0;
    }

    virtual void Shutdown() {
        op_count++;
    }

    virtual char* AllocRequestBuffer(size_t req_len, size_t resp_len) {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        op_count++;
        bytes_allocated += req_len + resp_len;
        buffer_.resize(req_len);
        return buffer_.data();
    }

    virtual void FreeRequestBuffer() {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        op_count++;
        buffer_.clear();
    }

    virtual bool SendToShard(TransportReceiver* src,
                            uint8_t req_type,
                            uint8_t shard_idx,
                            uint16_t server_id,
                            size_t msg_len) {
        op_count++;
        return true;
    }

    virtual bool SendToAll(TransportReceiver* src,
                          uint8_t req_type,
                          int shards_bit_set,
                          uint16_t server_id,
                          size_t resp_len,
                          size_t req_len,
                          int force_center = -1) {
        op_count++;
        return true;
    }

    virtual bool SendBatchToAll(TransportReceiver* src,
                               uint8_t req_type,
                               uint16_t server_id,
                               size_t resp_len,
                               const std::map<int, std::pair<char*, size_t>>& data) {
        op_count += data.size();
        return true;
    }

    virtual void RunEventLoop() {
        while (!stopped) {
            std::this_thread::sleep_for(milliseconds(1));
            op_count++;
        }
    }

    virtual void Stop() {
        stopped = true;
    }

    virtual void PrintStats() {
        std::cout << "[StressMockBackend] Operations: " << op_count
                  << ", Bytes allocated: " << bytes_allocated << std::endl;
    }

    virtual mako::TransportType GetType() const {
        return type_;
    }

    void Reset() {
        op_count = 0;
        bytes_allocated = 0;
        stopped = false;
    }
};

// ============= StressTransportReceiver =============

class StressTransportReceiver : public TransportReceiver {
public:
    std::atomic<uint64_t> response_count{0};
    std::atomic<uint64_t> total_bytes{0};
    std::atomic<bool> blocked{true};

    void ReceiveResponse(uint8_t reqType, char* respBuf) override {
        response_count++;
        if (respBuf) {
            total_bytes += 64;
        }
    }

    bool Blocked() override {
        return blocked.load(std::memory_order_relaxed);
    }

    void Reset() {
        response_count = 0;
        total_bytes = 0;
        blocked = true;
    }
};

// ============= Stress Test Fixture =============

class TransportStressTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// ============= High Throughput Buffer Allocation Tests =============

TEST_F(TransportStressTest, RapidBufferAllocation) {
    StressMockBackend backend;
    backend.Initialize("127.0.0.1:29850", 0, 0, 1, 10);

    const int iterations = 100000;
    auto start = high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        char* buf = backend.AllocRequestBuffer(256, 256);
        ASSERT_NE(buf, nullptr) << "Allocation failed at iteration " << i;
        buf[0] = static_cast<char>(i & 0xFF);
        backend.FreeRequestBuffer();
    }

    auto end = high_resolution_clock::now();
    auto duration_ms = duration_cast<milliseconds>(end - start).count();

    std::cout << "[STRESS] Rapid buffer allocation: " << iterations
              << " iterations in " << duration_ms << "ms ("
              << (iterations * 1000.0 / std::max(1L, duration_ms)) << " ops/sec)" << std::endl;

    backend.Shutdown();
}

TEST_F(TransportStressTest, VaryingSizeBufferAllocation) {
    StressMockBackend backend;
    backend.Initialize("127.0.0.1:29850", 0, 0, 1, 10);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> size_dist(64, 65536);

    const int iterations = 10000;

    auto start = high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        size_t req_size = size_dist(gen);
        size_t resp_size = size_dist(gen);

        char* buf = backend.AllocRequestBuffer(req_size, resp_size);
        ASSERT_NE(buf, nullptr) << "Failed at iteration " << i;

        buf[0] = 'X';
        if (req_size > 1) buf[req_size - 1] = 'Y';

        backend.FreeRequestBuffer();
    }

    auto end = high_resolution_clock::now();
    auto duration_ms = duration_cast<milliseconds>(end - start).count();

    std::cout << "[STRESS] Varying size allocation: " << iterations
              << " iterations, " << (backend.bytes_allocated / (1024 * 1024)) << "MB total, "
              << duration_ms << "ms" << std::endl;

    backend.Shutdown();
}

// ============= Concurrent Access Tests =============

TEST_F(TransportStressTest, ConcurrentBackendOperations) {
    const int num_threads = 8;
    const int iterations_per_thread = 10000;
    std::vector<std::thread> threads;
    std::vector<std::unique_ptr<StressMockBackend>> backends;
    std::atomic<uint64_t> total_ops{0};

    for (int t = 0; t < num_threads; t++) {
        backends.push_back(std::make_unique<StressMockBackend>());
        backends[t]->Initialize("127.0.0.1:29850", 0, 0, 1, 10);
    }

    auto start = high_resolution_clock::now();

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([&backends, t, iterations_per_thread, &total_ops]() {
            StressTransportReceiver receiver;
            for (int i = 0; i < iterations_per_thread; i++) {
                char* buf = backends[t]->AllocRequestBuffer(256, 256);
                if (buf) {
                    buf[0] = 'X';
                    backends[t]->SendToShard(&receiver, 1, 0, 0, 256);
                    backends[t]->FreeRequestBuffer();
                    total_ops += 3;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = high_resolution_clock::now();
    auto duration_ms = duration_cast<milliseconds>(end - start).count();

    std::cout << "[STRESS] Concurrent operations: " << total_ops
              << " total operations across " << num_threads << " threads in "
              << duration_ms << "ms ("
              << (total_ops * 1000.0 / std::max(1L, duration_ms)) << " ops/sec)" << std::endl;

    for (auto& backend : backends) {
        backend->Shutdown();
    }
}

// ============= Memory Pressure Tests =============

TEST_F(TransportStressTest, MemoryPressureLargeBuffers) {
    StressMockBackend backend;
    backend.Initialize("127.0.0.1:29850", 0, 0, 1, 10);

    const size_t max_size = 64 * 1024 * 1024;  // 64MB
    size_t size = 1024;
    int successful_allocs = 0;

    while (size <= max_size) {
        char* buf = backend.AllocRequestBuffer(size, size);
        if (!buf) {
            std::cout << "[STRESS] Memory pressure: allocation failed at "
                      << (size / (1024 * 1024)) << "MB" << std::endl;
            break;
        }

        for (size_t i = 0; i < size; i += 4096) {
            buf[i] = 'X';
        }

        backend.FreeRequestBuffer();
        successful_allocs++;
        size *= 2;
    }

    std::cout << "[STRESS] Memory pressure: " << successful_allocs
              << " successful allocations up to "
              << (size / 2 / (1024 * 1024)) << "MB" << std::endl;

    backend.Shutdown();
}

TEST_F(TransportStressTest, RepeatedInitShutdown) {
    const int cycles = 100;
    auto start = high_resolution_clock::now();

    for (int i = 0; i < cycles; i++) {
        StressMockBackend backend;
        int result = backend.Initialize("127.0.0.1:29850", 0, 0, 1, 10);
        ASSERT_EQ(result, 0) << "Init failed at cycle " << i;

        char* buf = backend.AllocRequestBuffer(1024, 1024);
        ASSERT_NE(buf, nullptr);
        memset(buf, 'A', 1024);
        backend.FreeRequestBuffer();

        backend.Shutdown();
    }

    auto end = high_resolution_clock::now();
    auto duration_ms = duration_cast<milliseconds>(end - start).count();

    std::cout << "[STRESS] Init/Shutdown cycles: " << cycles
              << " cycles in " << duration_ms << "ms ("
              << (duration_ms / static_cast<double>(cycles)) << " ms/cycle)" << std::endl;
}

// ============= Long-Running Stability Tests =============

TEST_F(TransportStressTest, LongRunningOperation) {
    StressMockBackend backend;
    backend.Initialize("127.0.0.1:29850", 0, 0, 1, 10);

    const int duration_seconds = 3;
    auto end_time = steady_clock::now() + seconds(duration_seconds);
    uint64_t operation_count = 0;

    while (steady_clock::now() < end_time) {
        size_t size = (operation_count % 1000) * 64 + 64;
        char* buf = backend.AllocRequestBuffer(size, 1024);

        if (buf) {
            snprintf(buf, 64, "Op %lu", operation_count);
            backend.FreeRequestBuffer();
            operation_count++;
        }
    }

    std::cout << "[STRESS] Long-running: " << operation_count
              << " operations in " << duration_seconds << " seconds ("
              << (operation_count / static_cast<double>(duration_seconds))
              << " ops/sec)" << std::endl;

    backend.Shutdown();
}

// ============= Event Loop Stress Tests =============

TEST_F(TransportStressTest, EventLoopStartStopCycles) {
    StressMockBackend backend;
    backend.Initialize("127.0.0.1:29850", 0, 0, 1, 10);

    const int cycles = 50;

    for (int i = 0; i < cycles; i++) {
        backend.stopped = false;

        std::thread event_thread([&backend]() {
            backend.RunEventLoop();
        });

        std::this_thread::sleep_for(milliseconds(5));

        backend.Stop();
        event_thread.join();
    }

    std::cout << "[STRESS] Event loop start/stop: " << cycles
              << " cycles completed" << std::endl;

    backend.Shutdown();
}

// ============= Type Parsing Stress Tests =============

TEST_F(TransportStressTest, TypeParsingPerformance) {
    const int iterations = 1000000;
    auto start = high_resolution_clock::now();

    volatile int dummy = 0;
    for (int i = 0; i < iterations; i++) {
        mako::TransportType type;
        switch (i % 5) {
            case 0: type = mako::ParseTransportType("erpc"); break;
            case 1: type = mako::ParseTransportType("ERPC"); break;
            case 2: type = mako::ParseTransportType("rrr"); break;
            case 3: type = mako::ParseTransportType("RRR"); break;
            case 4: type = mako::ParseTransportType("rrr_rpc"); break;
        }
        dummy += static_cast<int>(type);
    }

    auto end = high_resolution_clock::now();
    auto duration_us = duration_cast<microseconds>(end - start).count();

    std::cout << "[STRESS] Type parsing: " << iterations
              << " iterations in " << duration_us << "μs ("
              << (iterations * 1000.0 / std::max(1L, duration_us)) << " ops/ms)" << std::endl;
}

// ============= Send Operation Stress Tests =============

TEST_F(TransportStressTest, HighThroughputSend) {
    StressMockBackend backend;
    StressTransportReceiver receiver;
    backend.Initialize("127.0.0.1:29850", 0, 0, 1, 10);

    const int iterations = 100000;
    auto start = high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        backend.SendToShard(&receiver, 1, 0, 0, 256);
    }

    auto end = high_resolution_clock::now();
    auto duration_ms = duration_cast<milliseconds>(end - start).count();

    std::cout << "[STRESS] High throughput send: " << iterations
              << " sends in " << duration_ms << "ms ("
              << (iterations * 1000.0 / std::max(1L, duration_ms)) << " ops/sec)" << std::endl;

    backend.Shutdown();
}

TEST_F(TransportStressTest, BroadcastStress) {
    StressMockBackend backend;
    StressTransportReceiver receiver;
    backend.Initialize("127.0.0.1:29850", 0, 0, 1, 10);

    const int iterations = 50000;
    auto start = high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        backend.SendToAll(&receiver, 1, 0x0F, 0, 512, 256);
    }

    auto end = high_resolution_clock::now();
    auto duration_ms = duration_cast<milliseconds>(end - start).count();

    std::cout << "[STRESS] Broadcast stress: " << iterations
              << " broadcasts in " << duration_ms << "ms ("
              << (iterations * 1000.0 / std::max(1L, duration_ms)) << " ops/sec)" << std::endl;

    backend.Shutdown();
}

TEST_F(TransportStressTest, BatchSendStress) {
    StressMockBackend backend;
    StressTransportReceiver receiver;
    backend.Initialize("127.0.0.1:29850", 0, 0, 1, 10);

    char data[4][256];
    std::map<int, std::pair<char*, size_t>> batch;
    for (int i = 0; i < 4; i++) {
        memset(data[i], 'X', 256);
        batch[i] = {data[i], 256};
    }

    const int iterations = 25000;
    auto start = high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        backend.SendBatchToAll(&receiver, 1, 0, 512, batch);
    }

    auto end = high_resolution_clock::now();
    auto duration_ms = duration_cast<milliseconds>(end - start).count();

    std::cout << "[STRESS] Batch send stress: " << iterations
              << " batches (" << (iterations * 4) << " individual sends) in "
              << duration_ms << "ms ("
              << (iterations * 4 * 1000.0 / std::max(1L, duration_ms)) << " ops/sec)" << std::endl;

    backend.Shutdown();
}

// ============= Throughput Benchmark =============

TEST_F(TransportStressTest, ThroughputBenchmark) {
    StressMockBackend backend;
    backend.Initialize("127.0.0.1:29850", 0, 0, 1, 10);

    // Warm up
    for (int i = 0; i < 1000; i++) {
        char* buf = backend.AllocRequestBuffer(1024, 1024);
        buf[0] = 'W';
        backend.FreeRequestBuffer();
    }

    std::vector<size_t> sizes = {64, 256, 1024, 4096, 16384, 65536};

    for (size_t size : sizes) {
        const int iterations = 50000;
        auto start = high_resolution_clock::now();

        for (int i = 0; i < iterations; i++) {
            char* buf = backend.AllocRequestBuffer(size, size);
            buf[0] = 'X';
            if (size > 1) buf[size - 1] = 'Y';
            backend.FreeRequestBuffer();
        }

        auto end = high_resolution_clock::now();
        auto duration_us = duration_cast<microseconds>(end - start).count();
        double throughput_mb = (size * iterations * 2.0) / (1024 * 1024) /
                               (duration_us / 1000000.0);

        std::cout << "[BENCHMARK] Size " << size << " bytes: "
                  << (iterations * 1000000.0 / std::max(1L, duration_us)) << " ops/sec, "
                  << throughput_mb << " MB/sec" << std::endl;
    }

    backend.Shutdown();
}

// ============= Mixed Workload Test =============

TEST_F(TransportStressTest, MixedWorkload) {
    const int num_backends = 4;
    std::vector<std::unique_ptr<StressMockBackend>> backends;
    std::vector<std::thread> threads;
    std::atomic<uint64_t> total_ops{0};

    for (int i = 0; i < num_backends; i++) {
        backends.push_back(std::make_unique<StressMockBackend>());
        backends[i]->Initialize("127.0.0.1:29850", 0, 0, 1, 10);
    }

    auto start = high_resolution_clock::now();
    const int duration_seconds = 2;

    for (int t = 0; t < num_backends; t++) {
        threads.emplace_back([&backends, t, &total_ops, duration_seconds]() {
            StressTransportReceiver receiver;
            auto end_time = steady_clock::now() + seconds(duration_seconds);
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> op_dist(0, 3);

            while (steady_clock::now() < end_time) {
                switch (op_dist(gen)) {
                    case 0: {
                        char* buf = backends[t]->AllocRequestBuffer(256, 256);
                        if (buf) {
                            buf[0] = 'X';
                            backends[t]->FreeRequestBuffer();
                        }
                        break;
                    }
                    case 1:
                        backends[t]->SendToShard(&receiver, 1, 0, 0, 256);
                        break;
                    case 2:
                        backends[t]->SendToAll(&receiver, 1, 0x0F, 0, 256, 256);
                        break;
                    case 3: {
                        char data[256];
                        std::map<int, std::pair<char*, size_t>> batch = {{0, {data, 256}}};
                        backends[t]->SendBatchToAll(&receiver, 1, 0, 256, batch);
                        break;
                    }
                }
                total_ops++;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = high_resolution_clock::now();
    auto duration_ms = duration_cast<milliseconds>(end - start).count();

    std::cout << "[STRESS] Mixed workload: " << total_ops
              << " operations across " << num_backends << " backends in "
              << duration_ms << "ms ("
              << (total_ops * 1000.0 / std::max(1L, duration_ms)) << " ops/sec)" << std::endl;

    for (auto& backend : backends) {
        backend->Shutdown();
    }
}

// ============= Main =============

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);

    std::cout << "=======================================" << std::endl;
    std::cout << "Transport Backend Stress Tests" << std::endl;
    std::cout << "=======================================" << std::endl;

    return RUN_ALL_TESTS();
}
