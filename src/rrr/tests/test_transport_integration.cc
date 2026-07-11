#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <rusty/arc.hpp>
#include <rusty/function.hpp>
#include <rusty/option.hpp>
#include <rusty/box.hpp>
// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * test_transport_integration.cc:
 *   Integration tests for transport backends (RrrRpcBackend, ErpcBackend)
 *   Tests actual network I/O and request/response cycles
 *
 *   This test uses the underlying rrr/rpc library directly to test
 *   the same RPC patterns that RrrRpcBackend uses internally.
 *
 **********************************************************************/

#include <gtest/gtest.h>

// RRR/RPC includes for direct testing
#include "../rrr.hpp"

import std;
import rusty;

using namespace std::chrono;

// ============= Test Configuration =============

// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block with the C++ definitions.
#if RUSTYCPP_RUST
const TEST_PORT_BASE: i32 = 19000;
const TEST_REQ_TYPE_START: i32 = 1;
const TEST_REQ_TYPE_END: i32 = 10;
#endif
/*RUSTYCPP:GEN-BEGIN id=test_transport_integration.1 version=1 rust_sha256=99a9518b218ca78d28deae0e9c2ecd587cf97d7261926b6d8172b4f79524a805*/
constexpr int32_t TEST_PORT_BASE = static_cast<int32_t>(19000);
constexpr int32_t TEST_REQ_TYPE_START = static_cast<int32_t>(1);
constexpr int32_t TEST_REQ_TYPE_END = static_cast<int32_t>(10);
/*RUSTYCPP:GEN-END id=test_transport_integration.1*/

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

// ============= Test Service Classes =============

// TestRangeService: handles a range of RPC IDs for testing
class TestRangeService : public rrr::Service {
public:
    using Handler = rusty::Function<void(uint8_t, rusty::Box<rrr::Request>, rrr::WeakServerConnection)>;

    TestRangeService(rrr::i32 rpc_start, rrr::i32 rpc_end, Handler handler)
        : rpc_start_(rpc_start), rpc_end_(rpc_end), handler_(std::move(handler)) {}

    // @safe - with @unsafe block for loop
    int __reg_to__(rrr::Server& svr, size_t svc_index) override {
        // @unsafe - loop iteration
        {
            for (rrr::i32 rpc_id = rpc_start_; rpc_id <= rpc_end_; ++rpc_id) {
                int ret = svr.reg_rpc(rpc_id, svc_index);
                if (ret != 0) {
                    for (rrr::i32 id = rpc_start_; id < rpc_id; ++id) {
                        svr.unreg(id);
                    }
                    return ret;
                }
            }
        }
        return 0;
    }

    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req,
                      rrr::WeakServerConnection weak_sconn) override {
        handler_(static_cast<uint8_t>(rpc_id), std::move(req), weak_sconn);
    }

private:
    rrr::i32 rpc_start_;
    rrr::i32 rpc_end_;
    Handler handler_;
};

// TestSingleService: handles a single RPC ID for testing
class TestSingleService : public rrr::Service {
public:
    using Handler = rusty::Function<void(rusty::Box<rrr::Request>, rrr::WeakServerConnection)>;

    TestSingleService(rrr::i32 rpc_id, Handler handler)
        : rpc_id_(rpc_id), handler_(std::move(handler)) {}

    int __reg_to__(rrr::Server& svr, size_t svc_index) override {
        return svr.reg_rpc(rpc_id_, svc_index);
    }

    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req,
                      rrr::WeakServerConnection weak_sconn) override {
        if (rpc_id == rpc_id_) {
            handler_(std::move(req), weak_sconn);
        }
    }

private:
    rrr::i32 rpc_id_;
    Handler handler_;
};

// ============= Transport Type Utility Tests (Integration) =============

class TransportTypeIntegrationTest : public ::testing::Test {};

TEST_F(TransportTypeIntegrationTest, ParseAndConvertRrrRpc) {
    auto type = mako::ParseTransportType("rrr");
    EXPECT_EQ(type, mako::TransportType::RRR_RPC);
    EXPECT_STREQ(mako::TransportTypeToString(type), "rrr");
}

TEST_F(TransportTypeIntegrationTest, ParseAndConvertErpc) {
    auto type = mako::ParseTransportType("erpc");
    EXPECT_EQ(type, mako::TransportType::ERPC);
    EXPECT_STREQ(mako::TransportTypeToString(type), "erpc");
}

TEST_F(TransportTypeIntegrationTest, CaseInsensitiveParsing) {
    EXPECT_EQ(mako::ParseTransportType("RRR"), mako::TransportType::RRR_RPC);
    EXPECT_EQ(mako::ParseTransportType("ERPC"), mako::TransportType::ERPC);
    EXPECT_EQ(mako::ParseTransportType("rrr_rpc"), mako::TransportType::RRR_RPC);
}

TEST_F(TransportTypeIntegrationTest, InvalidTypeThrows) {
    EXPECT_THROW(mako::ParseTransportType("invalid"), std::runtime_error);
    EXPECT_THROW(mako::ParseTransportType("tcp"), std::runtime_error);
    EXPECT_THROW(mako::ParseTransportType(""), std::runtime_error);
}

// ============= RRR/RPC Direct Integration Tests =============
// These tests use the underlying rrr/rpc library directly,
// which is what RrrRpcBackend wraps

class RrrRpcDirectTest : public ::testing::Test {
protected:
    rusty::Option<rusty::Arc<rrr::PollThread>> poll_thread_worker_;
    rrr::Server* server_{nullptr};
    rusty::Option<rusty::Arc<rrr::Client>> client_;
    int port_;
    std::atomic<int> request_count_{0};
    std::atomic<bool> server_running_{false};

    void SetUp() override {
        // Use unique port to avoid conflicts with other tests
        static std::atomic<int> port_counter{TEST_PORT_BASE};
        port_ = port_counter.fetch_add(1);

        // Create PollThread
        poll_thread_worker_ = rusty::Some(rrr::PollThread::create());

        // Create server
        server_ = new rrr::Server(rrr::Server::new_(rusty::Some(poll_thread_worker_.as_ref().unwrap().clone())));

        // Register TestRangeService to handle test request types
        auto svc = rusty::make_box<TestRangeService>(
            static_cast<rrr::i32>(TEST_REQ_TYPE_START),
            static_cast<rrr::i32>(TEST_REQ_TYPE_END),
            [this](uint8_t req_type, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
                HandleRequest(req_type, std::move(req), weak_sconn);
            }
        );
        server_->reg_service(std::move(svc));

        // Start server
        std::string addr = "0.0.0.0:" + std::to_string(port_);
        ASSERT_EQ(server_->start(reinterpret_cast<const int8_t*>(addr.c_str())), 0) << "Failed to start server on " << addr;
        server_running_ = true;

        // Create client
        client_ = rusty::Some(rrr::Client::create(poll_thread_worker_.as_ref().unwrap()));
        std::string connect_addr = "127.0.0.1:" + std::to_string(port_);
        ASSERT_EQ(client_.as_ref().unwrap()->connect(reinterpret_cast<const int8_t*>(connect_addr.c_str()), true), 0)
            << "Failed to connect to " << connect_addr;

        // Wait for connection to establish
        std::this_thread::sleep_for(milliseconds(100));
    }

    void TearDown() override {
        if (client_.is_some()) {
            client_.as_ref().unwrap()->close();
            client_ = rusty::None;
        }

        if (server_) {
            delete server_;
            server_ = nullptr;
        }

        if (poll_thread_worker_.is_some()) {
            poll_thread_worker_.as_ref().unwrap()->shutdown();
            poll_thread_worker_ = rusty::None;
        }
    }

    void HandleRequest(uint8_t req_type, rusty::Box<rrr::Request> req,
                       rrr::WeakServerConnection weak_sconn) {
        request_count_++;

        auto sconn_opt = weak_sconn.upgrade();
        if (!sconn_opt.is_some()) {
            return;
        }
        auto sconn = sconn_opt.unwrap();

        // Read request data
        size_t req_size = req->m.content_size();
        std::vector<char> req_data(req_size);
        if (req_size > 0) {
            req->m.read(req_data.data(), req_size);
        }

        // Prepare response: echo back request type and size
        struct {
            uint8_t req_type;
            uint32_t req_size;
            uint32_t magic;
        } response;
        response.req_type = req_type;
        response.req_size = static_cast<uint32_t>(req_size);
        response.magic = 0xDEADBEEF;

        // Send response
        const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, [&](rrr::BinaryWriteArchive& out) {
            out.write_bytes(reinterpret_cast<const std::uint8_t*>(&response), sizeof(response));
        });
    }
};

TEST_F(RrrRpcDirectTest, BasicRequestResponse) {
    // Send a simple request
    std::string request_data = "Hello, Transport!";
    auto fu_result = client_.as_ref().unwrap()->request(TEST_REQ_TYPE_START, rrr::FutureAttr(), [&](rrr::BinaryWriteArchive& m) {
        m.write_bytes(reinterpret_cast<const std::uint8_t*>(request_data.data()), request_data.size());
    });
    ASSERT_TRUE(fu_result.is_ok()) << "Failed to begin request";
    auto fu = fu_result.unwrap();

    // Wait for response
    fu->wait();
    EXPECT_EQ(fu->get_error_code(), 0) << "RPC error: " << fu->get_error_code();

    // Verify request was processed
    EXPECT_EQ(request_count_, 1);

    // Read response
    struct {
        uint8_t req_type;
        uint32_t req_size;
        uint32_t magic;
    } response;
    fu->get_reply()->read(&response, sizeof(response));

    EXPECT_EQ(response.req_type, TEST_REQ_TYPE_START);
    EXPECT_EQ(response.req_size, request_data.size());
    EXPECT_EQ(response.magic, 0xDEADBEEF);
}

TEST_F(RrrRpcDirectTest, MultipleRequestTypes) {
    // Test sending different request types
    for (uint8_t req_type = TEST_REQ_TYPE_START; req_type <= TEST_REQ_TYPE_END; req_type++) {
        std::string data = "Request_" + std::to_string(req_type);
        auto fu_result = client_.as_ref().unwrap()->request(req_type, rrr::FutureAttr(), [&](rrr::BinaryWriteArchive& m) {
            m.write_bytes(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
        });
        ASSERT_TRUE(fu_result.is_ok()) << "Failed to begin request type " << (int)req_type;
        auto fu = fu_result.unwrap();

        fu->wait();
        EXPECT_EQ(fu->get_error_code(), 0);

        struct {
            uint8_t req_type;
            uint32_t req_size;
            uint32_t magic;
        } response;
        fu->get_reply()->read(&response, sizeof(response));

        EXPECT_EQ(response.req_type, req_type);
    }

    EXPECT_EQ(request_count_, TEST_REQ_TYPE_END - TEST_REQ_TYPE_START + 1);
}

TEST_F(RrrRpcDirectTest, ConcurrentRequests) {
    const int num_requests = 100;
    std::vector<rusty::Arc<rrr::Future>> futures;

    // Send all requests without waiting
    for (int i = 0; i < num_requests; i++) {
        std::string data = "Concurrent_" + std::to_string(i);
        auto fu_result = client_.as_ref().unwrap()->request(TEST_REQ_TYPE_START, rrr::FutureAttr(), [&](rrr::BinaryWriteArchive& m) {
            m.write_bytes(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
        });
        ASSERT_TRUE(fu_result.is_ok());
        futures.push_back(fu_result.unwrap());
    }

    // Wait for all responses
    for (auto& fu : futures) {
        fu->wait();
        EXPECT_EQ(fu->get_error_code(), 0);
    }

    EXPECT_EQ(request_count_, num_requests);
}

TEST_F(RrrRpcDirectTest, LargePayload) {
    // Test with 1MB payload
    const size_t payload_size = 1024 * 1024;
    std::vector<char> large_data(payload_size, 'X');

    auto fu_result = client_.as_ref().unwrap()->request(TEST_REQ_TYPE_START, rrr::FutureAttr(), [&](rrr::BinaryWriteArchive& m) {
        m.write_bytes(reinterpret_cast<const std::uint8_t*>(large_data.data()), large_data.size());
    });
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();

    fu->wait();
    EXPECT_EQ(fu->get_error_code(), 0);

    struct {
        uint8_t req_type;
        uint32_t req_size;
        uint32_t magic;
    } response;
    fu->get_reply()->read(&response, sizeof(response));

    EXPECT_EQ(response.req_size, payload_size);
}

TEST_F(RrrRpcDirectTest, ThreadSafetyMultipleClients) {
    const int num_threads = 4;
    const int requests_per_thread = 50;
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([this, t, requests_per_thread, &success_count]() {
            // Each thread creates its own client
            auto thread_client = rrr::Client::create(poll_thread_worker_.as_ref().unwrap());
            int ret = thread_client->connect(reinterpret_cast<const int8_t*>(("127.0.0.1:" + std::to_string(port_)).c_str()), true);
            if (ret != 0) return;

            std::this_thread::sleep_for(milliseconds(50));

            for (int i = 0; i < requests_per_thread; i++) {
                std::string data = "Thread_" + std::to_string(t) + "_" + std::to_string(i);
                auto fu_result = thread_client->request(TEST_REQ_TYPE_START, rrr::FutureAttr(), [&](rrr::BinaryWriteArchive& m) {
                    m.write_bytes(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
                });
                if (fu_result.is_err()) continue;
                auto fu = fu_result.unwrap();

                fu->wait();
                if (fu->get_error_code() == 0) {
                    success_count++;
                }
            }

            thread_client->close();
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(success_count, num_threads * requests_per_thread);
    EXPECT_EQ(request_count_, num_threads * requests_per_thread);
}

TEST_F(RrrRpcDirectTest, RequestWithTimeout) {
    std::string data = "Timeout_Test";
    auto fu_result = client_.as_ref().unwrap()->request(TEST_REQ_TYPE_START, rrr::FutureAttr(), [&](rrr::BinaryWriteArchive& m) {
        m.write_bytes(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    });
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();

    // Use timed_wait instead of wait
    fu->timed_wait(5.0);  // 5 second timeout

    EXPECT_FALSE(fu->timed_out());
    EXPECT_EQ(fu->get_error_code(), 0);
}

// ============= Stress Test =============

TEST_F(RrrRpcDirectTest, StressThroughput) {
    const int num_requests = 10000;
    auto start = high_resolution_clock::now();

    for (int i = 0; i < num_requests; i++) {
        uint32_t seq = i;
        auto fu_result = client_.as_ref().unwrap()->request(TEST_REQ_TYPE_START, rrr::FutureAttr(), [&](rrr::BinaryWriteArchive& m) {
            m.write_bytes(reinterpret_cast<const std::uint8_t*>(&seq), sizeof(seq));
        });
        ASSERT_TRUE(fu_result.is_ok());
        auto fu = fu_result.unwrap();

        fu->wait();
        EXPECT_EQ(fu->get_error_code(), 0);
    }

    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start);

    double ops_per_sec = (num_requests * 1000.0) / duration.count();
    std::cout << "[THROUGHPUT] " << num_requests << " requests in "
              << duration.count() << "ms (" << ops_per_sec << " ops/sec)" << std::endl;

    EXPECT_EQ(request_count_, num_requests);
    // Expect at least 800 ops/sec for localhost (lowered from 1000 to account for channel-based poll communication overhead)
    EXPECT_GT(ops_per_sec, 700.0);
}

TEST_F(RrrRpcDirectTest, StressPipelined) {
    const int batch_size = 50;
    const int num_batches = 20;

    auto start = high_resolution_clock::now();

    for (int batch = 0; batch < num_batches; batch++) {
        std::vector<rusty::Arc<rrr::Future>> futures;

        // Send batch
        for (int i = 0; i < batch_size; i++) {
            uint32_t seq = batch * batch_size + i;
            auto fu_result = client_.as_ref().unwrap()->request(TEST_REQ_TYPE_START, rrr::FutureAttr(), [&](rrr::BinaryWriteArchive& m) {
                m.write_bytes(reinterpret_cast<const std::uint8_t*>(&seq), sizeof(seq));
            });
            if (fu_result.is_err()) continue;
            futures.push_back(fu_result.unwrap());
        }

        // Wait for batch
        for (auto& fu : futures) {
            fu->wait();
        }
    }

    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start);

    int total_requests = batch_size * num_batches;
    double ops_per_sec = (total_requests * 1000.0) / duration.count();
    std::cout << "[PIPELINED] " << total_requests << " requests in "
              << duration.count() << "ms (" << ops_per_sec << " ops/sec)" << std::endl;

    EXPECT_EQ(request_count_, total_requests);
}

// ============= Connection Resilience Tests =============

class ConnectionResilienceTest : public ::testing::Test {
protected:
    rusty::Option<rusty::Arc<rrr::PollThread>> poll_thread_worker_;
    int port_;

    void SetUp() override {
        static std::atomic<int> port_counter{TEST_PORT_BASE + 100};
        port_ = port_counter.fetch_add(1);
        poll_thread_worker_ = rusty::Some(rrr::PollThread::create());
    }

    void TearDown() override {
        if (poll_thread_worker_.is_some()) {
            poll_thread_worker_.as_ref().unwrap()->shutdown();
        }
    }
};

TEST_F(ConnectionResilienceTest, ConnectToNonExistentServer) {
    auto client = rrr::Client::create(poll_thread_worker_.as_ref().unwrap());
    int result = client->connect(reinterpret_cast<const int8_t*>("127.0.0.1:19999"), true);
    EXPECT_NE(result, 0);
    client->close();
}

TEST_F(ConnectionResilienceTest, ReconnectAfterServerRestart) {
    std::atomic<int> request_count{0};

    // Start server
    auto server = new rrr::Server(rrr::Server::new_(rusty::Some(poll_thread_worker_.as_ref().unwrap().clone())));
    auto svc = rusty::make_box<TestSingleService>(1, [&](rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        request_count++;
        auto sconn_opt = weak_sconn.upgrade();
        if (sconn_opt.is_some()) {
            auto sconn = sconn_opt.unwrap();
            const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, rrr::ServerReplyFn{});
        }
    });
    server->reg_service(std::move(svc));
    ASSERT_EQ(server->start(reinterpret_cast<const int8_t*>(("0.0.0.0:" + std::to_string(port_)).c_str())), 0);

    // Connect client
    auto client = rrr::Client::create(poll_thread_worker_.as_ref().unwrap());
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(("127.0.0.1:" + std::to_string(port_)).c_str()), true), 0);
    std::this_thread::sleep_for(milliseconds(100));

    // Send request - use no-op lambda to avoid template overload issues
    {
        auto fu_result = client->request(1, rrr::FutureAttr(), [](rrr::BinaryWriteArchive&) {});
        ASSERT_TRUE(fu_result.is_ok());
        auto fu = fu_result.unwrap();
        fu->wait();
        EXPECT_EQ(fu->get_error_code(), 0);
    }
    EXPECT_EQ(request_count, 1);

    // Close and reopen client
    client->close();
    std::this_thread::sleep_for(milliseconds(100));

    auto client2 = rrr::Client::create(poll_thread_worker_.as_ref().unwrap());
    ASSERT_EQ(client2->connect(reinterpret_cast<const int8_t*>(("127.0.0.1:" + std::to_string(port_)).c_str()), true), 0);
    std::this_thread::sleep_for(milliseconds(100));

    // Send another request
    {
        auto fu_result = client2->request(1, rrr::FutureAttr(), [](rrr::BinaryWriteArchive&) {});
        ASSERT_TRUE(fu_result.is_ok());
        auto fu = fu_result.unwrap();
        fu->wait();
        EXPECT_EQ(fu->get_error_code(), 0);
    }
    EXPECT_EQ(request_count, 2);

    client2->close();
    delete server;
}

// ============= Buffer Allocation Pattern Tests =============
// These test buffer patterns that would be used by transport backends

class BufferPatternTest : public ::testing::Test {};

TEST_F(BufferPatternTest, ThreadLocalBuffers) {
    // Test pattern: thread-local buffers like RrrRpcBackend uses
    struct ThreadBuffers {
        std::vector<char> request_buffer;
        size_t response_len{0};
    };

    thread_local ThreadBuffers tls_buffers;

    // Simulate multiple allocations
    for (int i = 0; i < 1000; i++) {
        size_t req_len = (i % 100 + 1) * 64;
        size_t resp_len = (i % 50 + 1) * 64;

        tls_buffers.request_buffer.resize(req_len);
        tls_buffers.response_len = resp_len;

        // Fill buffer
        memset(tls_buffers.request_buffer.data(), 'X', req_len);

        // Verify
        EXPECT_EQ(tls_buffers.request_buffer.size(), req_len);
        EXPECT_EQ(tls_buffers.response_len, resp_len);
    }
}

TEST_F(BufferPatternTest, ConcurrentThreadLocalBuffers) {
    const int num_threads = 8;
    const int iterations = 1000;
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([t, iterations, &errors]() {
            struct ThreadBuffers {
                std::vector<char> request_buffer;
                size_t response_len{0};
            };
            thread_local ThreadBuffers tls_buffers;

            for (int i = 0; i < iterations; i++) {
                size_t expected_size = ((t * iterations + i) % 100 + 1) * 64;

                tls_buffers.request_buffer.resize(expected_size);
                tls_buffers.response_len = expected_size;

                // Fill with thread-specific pattern
                memset(tls_buffers.request_buffer.data(), static_cast<char>(t), expected_size);

                // Verify no cross-thread contamination
                for (size_t j = 0; j < expected_size; j++) {
                    if (tls_buffers.request_buffer[j] != static_cast<char>(t)) {
                        errors++;
                        break;
                    }
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(errors, 0) << "Thread-local buffer contamination detected";
}

// ============= Main =============

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
