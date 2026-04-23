// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * test_transport_backend.cc:
 *   Comprehensive tests for transport backend interface
 *   Tests type utilities and mock implementations
 *
 *   NOTE: These tests use mock implementations to avoid pulling in
 *   full mako/eRPC dependencies. The mock tests verify the interface
 *   contract that all backends must satisfy.
 *
 **********************************************************************/

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <fstream>
#include <cstdlib>
#include <map>
#include <vector>
#include <string>
#include <stdexcept>

using namespace std::chrono;

// ============= Inline Transport Type Definitions =============
// (Matching mako/lib/transport_backend.h to avoid header dependencies)

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

// ============= Mock TransportBackend =============

class MockTransportBackend {
public:
    std::atomic<int> initialize_count{0};
    std::atomic<int> shutdown_count{0};
    std::atomic<int> alloc_count{0};
    std::atomic<int> free_count{0};
    std::atomic<int> send_count{0};
    std::atomic<bool> stopped{false};
    std::vector<char> buffer_;
    mako::TransportType type_;

    explicit MockTransportBackend(mako::TransportType type = mako::TransportType::RRR_RPC)
        : type_(type) {}

    virtual ~MockTransportBackend() = default;

    virtual int Initialize(const std::string& local_uri,
                          uint8_t numa_node,
                          uint8_t phy_port,
                          uint8_t st_nr_req_types,
                          uint8_t end_nr_req_types) {
        initialize_count++;
        return 0;
    }

    virtual void Shutdown() {
        shutdown_count++;
    }

    virtual char* AllocRequestBuffer(size_t req_len, size_t resp_len) {
        alloc_count++;
        buffer_.resize(req_len);
        return buffer_.data();
    }

    virtual void FreeRequestBuffer() {
        free_count++;
        buffer_.clear();
    }

    virtual bool SendToShard(TransportReceiver* src,
                            uint8_t req_type,
                            uint8_t shard_idx,
                            uint16_t server_id,
                            size_t msg_len) {
        send_count++;
        return true;
    }

    virtual bool SendToAll(TransportReceiver* src,
                          uint8_t req_type,
                          int shards_bit_set,
                          uint16_t server_id,
                          size_t resp_len,
                          size_t req_len,
                          int force_center = -1) {
        send_count++;
        return true;
    }

    virtual bool SendBatchToAll(TransportReceiver* src,
                               uint8_t req_type,
                               uint16_t server_id,
                               size_t resp_len,
                               const std::map<int, std::pair<char*, size_t>>& data) {
        send_count += data.size();
        return true;
    }

    virtual void RunEventLoop() {
        while (!stopped) {
            std::this_thread::sleep_for(milliseconds(10));
        }
    }

    virtual void Stop() {
        stopped = true;
    }

    virtual void PrintStats() {
        // No-op for mock
    }

    virtual mako::TransportType GetType() const {
        return type_;
    }

    virtual const char* GetName() const {
        switch (GetType()) {
            case mako::TransportType::ERPC: return "eRPC";
            case mako::TransportType::RRR_RPC: return "rrr/rpc";
            default: return "unknown";
        }
    }
};

// ============= Mock TransportReceiver Implementation =============

class MockTransportReceiver : public TransportReceiver {
public:
    std::atomic<int> response_count{0};
    std::atomic<bool> blocked{true};
    std::vector<std::pair<uint8_t, std::vector<char>>> responses;
    std::mutex responses_mutex;

    void ReceiveResponse(uint8_t reqType, char* respBuf) override {
        std::lock_guard<std::mutex> lock(responses_mutex);
        std::vector<char> resp(64);
        if (respBuf) {
            memcpy(resp.data(), respBuf, 64);
        }
        responses.emplace_back(reqType, std::move(resp));
        response_count++;
    }

    bool Blocked() override {
        return blocked.load();
    }

    void Reset() {
        std::lock_guard<std::mutex> lock(responses_mutex);
        response_count = 0;
        responses.clear();
        blocked = true;
    }
};

// ============= Parse Transport Type Tests =============

TEST(TransportTypeTest, ParseErpc) {
    EXPECT_EQ(mako::ParseTransportType("erpc"), mako::TransportType::ERPC);
    EXPECT_EQ(mako::ParseTransportType("ERPC"), mako::TransportType::ERPC);
}

TEST(TransportTypeTest, ParseRrr) {
    EXPECT_EQ(mako::ParseTransportType("rrr"), mako::TransportType::RRR_RPC);
    EXPECT_EQ(mako::ParseTransportType("RRR"), mako::TransportType::RRR_RPC);
    EXPECT_EQ(mako::ParseTransportType("rrr_rpc"), mako::TransportType::RRR_RPC);
}

TEST(TransportTypeTest, ParseInvalid) {
    EXPECT_THROW(mako::ParseTransportType("invalid"), std::runtime_error);
    EXPECT_THROW(mako::ParseTransportType(""), std::runtime_error);
    EXPECT_THROW(mako::ParseTransportType("tcp"), std::runtime_error);
    EXPECT_THROW(mako::ParseTransportType("udp"), std::runtime_error);
}

TEST(TransportTypeTest, TypeToString) {
    EXPECT_STREQ(mako::TransportTypeToString(mako::TransportType::ERPC), "erpc");
    EXPECT_STREQ(mako::TransportTypeToString(mako::TransportType::RRR_RPC), "rrr");
}

TEST(TransportTypeTest, RoundTrip) {
    auto type1 = mako::ParseTransportType("erpc");
    EXPECT_STREQ(mako::TransportTypeToString(type1), "erpc");

    auto type2 = mako::ParseTransportType("rrr");
    EXPECT_STREQ(mako::TransportTypeToString(type2), "rrr");
}

// ============= Mock Backend Tests =============

TEST(MockBackendTest, Initialize) {
    MockTransportBackend backend;

    int result = backend.Initialize("127.0.0.1:8080", 0, 0, 1, 10);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(backend.initialize_count, 1);
}

TEST(MockBackendTest, Shutdown) {
    MockTransportBackend backend;

    backend.Initialize("127.0.0.1:8080", 0, 0, 1, 10);
    backend.Shutdown();

    EXPECT_EQ(backend.shutdown_count, 1);
}

TEST(MockBackendTest, AllocFreeBuffer) {
    MockTransportBackend backend;
    backend.Initialize("127.0.0.1:8080", 0, 0, 1, 10);

    char* buf = backend.AllocRequestBuffer(1024, 512);
    EXPECT_NE(buf, nullptr);
    EXPECT_EQ(backend.alloc_count, 1);

    memset(buf, 'X', 1024);
    EXPECT_EQ(buf[0], 'X');
    EXPECT_EQ(buf[1023], 'X');

    backend.FreeRequestBuffer();
    EXPECT_EQ(backend.free_count, 1);
}

TEST(MockBackendTest, SendToShard) {
    MockTransportBackend backend;
    MockTransportReceiver receiver;
    backend.Initialize("127.0.0.1:8080", 0, 0, 1, 10);

    bool result = backend.SendToShard(&receiver, 1, 0, 0, 100);
    EXPECT_TRUE(result);
    EXPECT_EQ(backend.send_count, 1);
}

TEST(MockBackendTest, SendToAll) {
    MockTransportBackend backend;
    MockTransportReceiver receiver;
    backend.Initialize("127.0.0.1:8080", 0, 0, 1, 10);

    bool result = backend.SendToAll(&receiver, 1, 0x07, 0, 512, 256);
    EXPECT_TRUE(result);
    EXPECT_EQ(backend.send_count, 1);
}

TEST(MockBackendTest, SendBatchToAll) {
    MockTransportBackend backend;
    MockTransportReceiver receiver;
    backend.Initialize("127.0.0.1:8080", 0, 0, 1, 10);

    char data1[100], data2[100], data3[100];
    std::map<int, std::pair<char*, size_t>> batch = {
        {0, {data1, 100}},
        {1, {data2, 100}},
        {2, {data3, 100}}
    };

    bool result = backend.SendBatchToAll(&receiver, 1, 0, 512, batch);
    EXPECT_TRUE(result);
    EXPECT_EQ(backend.send_count, 3);
}

TEST(MockBackendTest, EventLoopStartStop) {
    MockTransportBackend backend;
    backend.Initialize("127.0.0.1:8080", 0, 0, 1, 10);

    std::thread event_thread([&backend]() {
        backend.RunEventLoop();
    });

    std::this_thread::sleep_for(milliseconds(50));

    backend.Stop();
    EXPECT_TRUE(backend.stopped);

    event_thread.join();
}

TEST(MockBackendTest, GetType) {
    MockTransportBackend rrr_backend(mako::TransportType::RRR_RPC);
    EXPECT_EQ(rrr_backend.GetType(), mako::TransportType::RRR_RPC);

    MockTransportBackend erpc_backend(mako::TransportType::ERPC);
    EXPECT_EQ(erpc_backend.GetType(), mako::TransportType::ERPC);
}

TEST(MockBackendTest, GetName) {
    MockTransportBackend rrr_backend(mako::TransportType::RRR_RPC);
    EXPECT_STREQ(rrr_backend.GetName(), "rrr/rpc");

    MockTransportBackend erpc_backend(mako::TransportType::ERPC);
    EXPECT_STREQ(erpc_backend.GetName(), "eRPC");
}

// ============= MockTransportReceiver Tests =============

TEST(MockReceiverTest, BasicReceive) {
    MockTransportReceiver receiver;

    char response[64] = "Test Response";
    receiver.ReceiveResponse(1, response);

    EXPECT_EQ(receiver.response_count, 1);
    EXPECT_EQ(receiver.responses.size(), 1);
    EXPECT_EQ(receiver.responses[0].first, 1);
}

TEST(MockReceiverTest, MultipleReceive) {
    MockTransportReceiver receiver;

    for (int i = 0; i < 10; i++) {
        char response[64];
        snprintf(response, sizeof(response), "Response %d", i);
        receiver.ReceiveResponse(static_cast<uint8_t>(i), response);
    }

    EXPECT_EQ(receiver.response_count, 10);
    EXPECT_EQ(receiver.responses.size(), 10);
}

TEST(MockReceiverTest, BlockedState) {
    MockTransportReceiver receiver;

    EXPECT_TRUE(receiver.Blocked());
    receiver.blocked = false;
    EXPECT_FALSE(receiver.Blocked());
}

TEST(MockReceiverTest, Reset) {
    MockTransportReceiver receiver;

    char response[64] = "Test";
    receiver.ReceiveResponse(1, response);
    EXPECT_EQ(receiver.response_count, 1);

    receiver.Reset();
    EXPECT_EQ(receiver.response_count, 0);
    EXPECT_TRUE(receiver.responses.empty());
    EXPECT_TRUE(receiver.Blocked());
}

TEST(MockReceiverTest, ThreadSafety) {
    MockTransportReceiver receiver;
    const int num_threads = 10;
    const int responses_per_thread = 100;
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([&receiver, t, responses_per_thread]() {
            for (int i = 0; i < responses_per_thread; i++) {
                char response[64];
                snprintf(response, sizeof(response), "Thread %d Response %d", t, i);
                receiver.ReceiveResponse(static_cast<uint8_t>(t), response);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(receiver.response_count, num_threads * responses_per_thread);
}

// ============= Interface Polymorphism Tests =============

TEST(PolymorphismTest, MultipleBackends) {
    std::vector<std::unique_ptr<MockTransportBackend>> backends;

    backends.push_back(std::make_unique<MockTransportBackend>(mako::TransportType::RRR_RPC));
    backends.push_back(std::make_unique<MockTransportBackend>(mako::TransportType::ERPC));

    EXPECT_EQ(backends[0]->GetType(), mako::TransportType::RRR_RPC);
    EXPECT_EQ(backends[1]->GetType(), mako::TransportType::ERPC);

    for (auto& backend : backends) {
        int result = backend->Initialize("127.0.0.1:8080", 0, 0, 1, 10);
        EXPECT_EQ(result, 0);
    }

    for (auto& backend : backends) {
        backend->Shutdown();
    }
}

// ============= Edge Cases =============

TEST(EdgeCaseTest, EmptyBuffer) {
    MockTransportBackend backend;
    backend.Initialize("127.0.0.1:8080", 0, 0, 1, 10);

    char* buf = backend.AllocRequestBuffer(0, 0);
    // Should still return a valid pointer (to empty vector)

    backend.FreeRequestBuffer();
    backend.Shutdown();
}

TEST(EdgeCaseTest, LargeBuffer) {
    MockTransportBackend backend;
    backend.Initialize("127.0.0.1:8080", 0, 0, 1, 10);

    size_t large_size = 10 * 1024 * 1024;  // 10 MB
    char* buf = backend.AllocRequestBuffer(large_size, large_size);
    EXPECT_NE(buf, nullptr);

    buf[0] = 'A';
    buf[large_size - 1] = 'Z';

    backend.FreeRequestBuffer();
    backend.Shutdown();
}

TEST(EdgeCaseTest, RepeatedInitShutdown) {
    MockTransportBackend backend;

    for (int i = 0; i < 10; i++) {
        backend.Initialize("127.0.0.1:8080", 0, 0, 1, 10);
        backend.Shutdown();
    }

    EXPECT_EQ(backend.initialize_count, 10);
    EXPECT_EQ(backend.shutdown_count, 10);
}

TEST(EdgeCaseTest, MultipleAllocWithoutFree) {
    MockTransportBackend backend;
    backend.Initialize("127.0.0.1:8080", 0, 0, 1, 10);

    for (int i = 0; i < 100; i++) {
        char* buf = backend.AllocRequestBuffer(256, 256);
        EXPECT_NE(buf, nullptr);
        memset(buf, static_cast<char>(i), 256);
    }

    EXPECT_EQ(backend.alloc_count, 100);

    backend.FreeRequestBuffer();
    backend.Shutdown();
}

TEST(EdgeCaseTest, NullReceiver) {
    MockTransportBackend backend;
    backend.Initialize("127.0.0.1:8080", 0, 0, 1, 10);

    // Null receiver should still work (mock doesn't use it)
    bool result = backend.SendToShard(nullptr, 1, 0, 0, 100);
    EXPECT_TRUE(result);

    backend.Shutdown();
}

// ============= Main =============

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
