// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * test_erpc_integration.cc:
 *   Integration tests for eRPC transport backend using socket-based
 *   "fake" transport (no RDMA hardware required)
 *
 *   Tests actual eRPC network I/O and request/response cycles
 *
 **********************************************************************/

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <cstring>
#include <cassert>

// eRPC includes
#include "rpc.h"
#include "rpc_constants.h"

using namespace std::chrono;

// ============= Test Configuration =============

// eRPC ports must be between kBaseSmUdpPort (31000) and kBaseSmUdpPort + kMaxNumERpcProcesses (41000)
static constexpr int ERPC_TEST_PORT_BASE = 31100;
static constexpr int ERPC_MSG_SIZE = 256;
static constexpr int ERPC_REQ_TYPE_START = 1;
static constexpr int ERPC_REQ_TYPE_END = 5;

// ============= Test Structures =============

struct ErpcTestRequest {
    uint64_t req_id;
    uint32_t data_size;
    uint32_t magic;
    char data[240];
};

struct ErpcTestResponse {
    uint64_t req_id;
    uint32_t result;
    uint32_t magic;
    uint8_t req_type;
    char echo[240];
};

struct ErpcReqTag {
    erpc::MsgBuffer req_msgbuf;
    erpc::MsgBuffer resp_msgbuf;
    std::atomic<bool> completed{false};
    int error_code{0};
};

struct ErpcClientContext {
    erpc::Rpc<erpc::CTransport>* rpc{nullptr};
    std::atomic<int> responses_received{0};
};

struct ErpcServerContext {
    erpc::Rpc<erpc::CTransport>* rpc{nullptr};
    std::atomic<int> requests_received{0};
};

// ============= Global Test State =============

static ErpcServerContext* g_server_ctx = nullptr;

// ============= Callbacks =============

static void sm_handler(int session_num, erpc::SmEventType sm_event_type,
                       erpc::SmErrType sm_err_type, void* context) {
    // Session management handler - just log events
    (void)session_num;
    (void)sm_event_type;
    (void)sm_err_type;
    (void)context;
}

static void server_request_handler(erpc::ReqHandle* req_handle, void* context) {
    auto* ctx = static_cast<ErpcServerContext*>(context);
    ctx->requests_received++;

    // Get request data
    auto* req = reinterpret_cast<ErpcTestRequest*>(req_handle->get_req_msgbuf()->buf_);
    uint8_t req_type = req_handle->get_req_msgbuf()->get_req_type();

    // Prepare response in pre-allocated buffer
    auto& resp_buf = req_handle->pre_resp_msgbuf_;
    auto* resp = reinterpret_cast<ErpcTestResponse*>(resp_buf.buf_);

    resp->req_id = req->req_id;
    resp->result = 0;  // Success
    resp->magic = 0xBEEFCAFE;
    resp->req_type = req_type;
    memcpy(resp->echo, req->data, std::min(req->data_size, (uint32_t)sizeof(resp->echo)));

    ctx->rpc->resize_msg_buffer(&resp_buf, sizeof(ErpcTestResponse));
    ctx->rpc->enqueue_response(req_handle, &resp_buf);
}

static void client_response_handler(void* context, void* tag) {
    auto* ctx = static_cast<ErpcClientContext*>(context);
    auto* req_tag = static_cast<ErpcReqTag*>(tag);

    ctx->responses_received++;
    req_tag->completed = true;

    // Free message buffers
    ctx->rpc->free_msg_buffer(req_tag->req_msgbuf);
    ctx->rpc->free_msg_buffer(req_tag->resp_msgbuf);
}

// ============= eRPC Direct Integration Tests =============

class ErpcDirectTest : public ::testing::Test {
protected:
    std::unique_ptr<erpc::Nexus> server_nexus_;
    std::unique_ptr<erpc::Nexus> client_nexus_;
    ErpcServerContext server_ctx_;
    ErpcClientContext client_ctx_;
    std::thread server_thread_;
    std::atomic<bool> server_running_{false};
    std::atomic<bool> stop_server_{false};
    int server_port_;
    int client_port_;
    int session_num_{-1};

    void SetUp() override {
        // Use unique ports to avoid conflicts
        static std::atomic<int> port_counter{ERPC_TEST_PORT_BASE};
        server_port_ = port_counter.fetch_add(10);
        client_port_ = server_port_ + 5;

        // Start server in background thread
        server_thread_ = std::thread([this]() { RunServer(); });

        // Wait for server to start
        while (!server_running_) {
            std::this_thread::sleep_for(milliseconds(10));
        }

        // Create client
        std::string client_uri = "127.0.0.1:" + std::to_string(client_port_);
        client_nexus_ = std::make_unique<erpc::Nexus>(client_uri);
        client_ctx_.rpc = new erpc::Rpc<erpc::CTransport>(
            client_nexus_.get(),
            static_cast<void*>(&client_ctx_),
            0,  // rpc_id
            sm_handler,
            0   // phy_port
        );

        // Connect to server
        std::string server_uri = "127.0.0.1:" + std::to_string(server_port_);
        session_num_ = client_ctx_.rpc->create_session(server_uri, 100);

        // Wait for connection - SM packets are processed asynchronously
        // Need to give time for the SM thread to process
        int wait_count = 0;
        while (!client_ctx_.rpc->is_connected(session_num_) && wait_count < 500) {
            client_ctx_.rpc->run_event_loop(1);  // 1ms timeout with event processing
            std::this_thread::sleep_for(milliseconds(1));
            wait_count++;
        }
        ASSERT_TRUE(client_ctx_.rpc->is_connected(session_num_))
            << "Failed to connect to server after " << wait_count << " iterations";
    }

    void TearDown() override {
        // Stop server
        stop_server_ = true;

        // Clean up client
        if (client_ctx_.rpc) {
            delete client_ctx_.rpc;
            client_ctx_.rpc = nullptr;
        }

        // Wait for server thread
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }

    void RunServer() {
        std::string server_uri = "127.0.0.1:" + std::to_string(server_port_);
        server_nexus_ = std::make_unique<erpc::Nexus>(server_uri);

        // Register request handlers
        for (uint8_t req_type = ERPC_REQ_TYPE_START; req_type <= ERPC_REQ_TYPE_END; req_type++) {
            server_nexus_->register_req_func(req_type, server_request_handler,
                                             erpc::ReqFuncType::kForeground);
        }

        server_ctx_.rpc = new erpc::Rpc<erpc::CTransport>(
            server_nexus_.get(),
            static_cast<void*>(&server_ctx_),
            100,  // rpc_id for server
            sm_handler,
            0     // phy_port
        );

        server_running_ = true;

        // Run event loop until stopped
        while (!stop_server_) {
            server_ctx_.rpc->run_event_loop(10);  // 10ms timeout
        }

        delete server_ctx_.rpc;
        server_ctx_.rpc = nullptr;
    }

    // Helper to send a request and wait for response
    bool SendRequestAndWait(uint8_t req_type, const char* data, size_t data_len,
                           ErpcTestResponse* out_resp = nullptr) {
        ErpcReqTag tag;
        tag.completed = false;

        // Allocate buffers
        tag.req_msgbuf = client_ctx_.rpc->alloc_msg_buffer_or_die(sizeof(ErpcTestRequest));
        tag.resp_msgbuf = client_ctx_.rpc->alloc_msg_buffer_or_die(sizeof(ErpcTestResponse));

        // Fill request
        auto* req = reinterpret_cast<ErpcTestRequest*>(tag.req_msgbuf.buf_);
        static std::atomic<uint64_t> req_counter{0};
        req->req_id = req_counter++;
        req->magic = 0xDEADBEEF;
        req->data_size = std::min(data_len, sizeof(req->data));
        memcpy(req->data, data, req->data_size);

        // Send request
        client_ctx_.rpc->enqueue_request(
            session_num_, req_type,
            &tag.req_msgbuf, &tag.resp_msgbuf,
            client_response_handler, &tag
        );

        // Wait for response
        int wait_count = 0;
        while (!tag.completed && wait_count < 100000) {
            client_ctx_.rpc->run_event_loop_once();
            wait_count++;
        }

        if (!tag.completed) {
            return false;
        }

        // Copy response if requested
        if (out_resp) {
            memcpy(out_resp, tag.resp_msgbuf.buf_, sizeof(ErpcTestResponse));
        }

        return true;
    }
};

TEST_F(ErpcDirectTest, BasicRequestResponse) {
    const char* test_data = "Hello, eRPC!";
    ErpcTestResponse resp;

    bool success = SendRequestAndWait(ERPC_REQ_TYPE_START, test_data, strlen(test_data), &resp);
    ASSERT_TRUE(success) << "Request timed out";

    EXPECT_EQ(resp.magic, 0xBEEFCAFE);
    EXPECT_EQ(resp.req_type, ERPC_REQ_TYPE_START);
    EXPECT_EQ(resp.result, 0);
    EXPECT_EQ(server_ctx_.requests_received, 1);
    EXPECT_EQ(client_ctx_.responses_received, 1);
}

TEST_F(ErpcDirectTest, MultipleRequestTypes) {
    for (uint8_t req_type = ERPC_REQ_TYPE_START; req_type <= ERPC_REQ_TYPE_END; req_type++) {
        std::string data = "Request_Type_" + std::to_string(req_type);
        ErpcTestResponse resp;

        bool success = SendRequestAndWait(req_type, data.c_str(), data.size(), &resp);
        ASSERT_TRUE(success) << "Request type " << (int)req_type << " timed out";

        EXPECT_EQ(resp.req_type, req_type);
        EXPECT_EQ(resp.result, 0);
    }

    EXPECT_EQ(server_ctx_.requests_received, ERPC_REQ_TYPE_END - ERPC_REQ_TYPE_START + 1);
}

TEST_F(ErpcDirectTest, SequentialRequests) {
    const int num_requests = 50;

    for (int i = 0; i < num_requests; i++) {
        std::string data = "Sequential_" + std::to_string(i);
        ErpcTestResponse resp;

        bool success = SendRequestAndWait(ERPC_REQ_TYPE_START, data.c_str(), data.size(), &resp);
        ASSERT_TRUE(success) << "Request " << i << " timed out";

        EXPECT_EQ(resp.result, 0);
    }

    EXPECT_EQ(server_ctx_.requests_received, num_requests);
    EXPECT_EQ(client_ctx_.responses_received, num_requests);
}

TEST_F(ErpcDirectTest, LargePayload) {
    // Use maximum data size that fits in our struct
    char large_data[240];
    memset(large_data, 'X', sizeof(large_data));

    ErpcTestResponse resp;
    bool success = SendRequestAndWait(ERPC_REQ_TYPE_START, large_data, sizeof(large_data), &resp);
    ASSERT_TRUE(success) << "Large payload request timed out";

    EXPECT_EQ(resp.result, 0);
    EXPECT_EQ(resp.echo[0], 'X');
    EXPECT_EQ(resp.echo[239], 'X');
}

TEST_F(ErpcDirectTest, ThroughputBenchmark) {
    const int num_requests = 500;
    auto start = high_resolution_clock::now();

    for (int i = 0; i < num_requests; i++) {
        std::string data = "Bench_" + std::to_string(i);
        bool success = SendRequestAndWait(ERPC_REQ_TYPE_START, data.c_str(), data.size());
        ASSERT_TRUE(success) << "Request " << i << " timed out"; // It timeouts
    }

    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start);

    double ops_per_sec = (num_requests * 1000.0) / duration.count();
    std::cout << "[eRPC THROUGHPUT] " << num_requests << " requests in "
              << duration.count() << "ms (" << ops_per_sec << " ops/sec)" << std::endl;

    EXPECT_EQ(server_ctx_.requests_received, num_requests);
    // eRPC with fake transport should achieve at least 100 ops/sec
    EXPECT_GT(ops_per_sec, 100.0);
}

// ============= Connection Tests =============

class ErpcConnectionTest : public ::testing::Test {
protected:
    int port_;

    void SetUp() override {
        static std::atomic<int> port_counter{ERPC_TEST_PORT_BASE + 500};
        port_ = port_counter.fetch_add(10);
    }
};

TEST_F(ErpcConnectionTest, ServerStartsSuccessfully) {
    std::string uri = "127.0.0.1:" + std::to_string(port_);
    erpc::Nexus nexus(uri);

    // Register a simple handler
    nexus.register_req_func(1, [](erpc::ReqHandle*, void*) {}, erpc::ReqFuncType::kForeground);

    ErpcServerContext ctx;
    ctx.rpc = new erpc::Rpc<erpc::CTransport>(&nexus, &ctx, 100, sm_handler, 0);

    EXPECT_NE(ctx.rpc, nullptr);

    delete ctx.rpc;
}

TEST_F(ErpcConnectionTest, ClientCreatesSession) {
    // Start server
    std::string server_uri = "127.0.0.1:" + std::to_string(port_);
    erpc::Nexus server_nexus(server_uri);
    server_nexus.register_req_func(1, server_request_handler, erpc::ReqFuncType::kForeground);

    ErpcServerContext server_ctx;
    server_ctx.rpc = new erpc::Rpc<erpc::CTransport>(&server_nexus, &server_ctx, 100, sm_handler, 0);

    // Run server in background
    std::atomic<bool> stop{false};
    std::thread server_thread([&]() {
        while (!stop) {
            server_ctx.rpc->run_event_loop(10);
        }
    });

    // Give server time to start
    std::this_thread::sleep_for(milliseconds(100));

    // Create client
    std::string client_uri = "127.0.0.1:" + std::to_string(port_ + 1);
    erpc::Nexus client_nexus(client_uri);

    ErpcClientContext client_ctx;
    client_ctx.rpc = new erpc::Rpc<erpc::CTransport>(&client_nexus, &client_ctx, 0, sm_handler, 0);

    // Create session
    int session = client_ctx.rpc->create_session(server_uri, 100);
    EXPECT_GE(session, 0);

    // Wait for connection - SM packets are processed asynchronously
    int wait_count = 0;
    while (!client_ctx.rpc->is_connected(session) && wait_count < 500) {
        client_ctx.rpc->run_event_loop(1);  // 1ms timeout with event processing
        std::this_thread::sleep_for(milliseconds(1));
        wait_count++;
    }

    EXPECT_TRUE(client_ctx.rpc->is_connected(session));

    // Cleanup
    stop = true;
    delete client_ctx.rpc;
    server_thread.join();
    delete server_ctx.rpc;
}

// ============= Main =============

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
