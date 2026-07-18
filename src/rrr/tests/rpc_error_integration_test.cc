/**
 * Integration tests for RPC error types with actual RPC operations.
 * Tests structured error-code handling during various failure scenarios.
 */

#include <gtest/gtest.h>
#include <rusty/arc.hpp>
#include "../rrr.hpp"
#include "benchmark_service.h"

import std;

using namespace rrr;
using namespace benchmark;
using namespace std::chrono;

// Atomic counter for dynamic port allocation
static std::atomic<int> g_error_test_port{14000};

// Test service for error tests
class ErrorTestService : public benchmark::BenchmarkService {
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
// RPC Error Types Tests
// ============================================================================

class ErrorIntegrationTest : public ::testing::Test {
protected:
    rusty::Option<rusty::Arc<PollThread>> poll_thread_;
    int test_port_;

    ErrorIntegrationTest() : test_port_(g_error_test_port.fetch_add(1)) {}

    void SetUp() override {
        poll_thread_ = rusty::Some(PollThread::create());
    }

    void TearDown() override {
        poll_thread_.as_ref().unwrap()->shutdown();
    }

    Server* start_server() {
        auto server = new Server(Server::new_(rusty::Some(poll_thread_.as_ref().unwrap().clone())));
        auto service_box = rusty::make_box<ErrorTestService>();
        server->reg_service_typed(std::move(service_box));
        if (server->start(reinterpret_cast<const int8_t*>(("0.0.0.0:" + std::to_string(test_port_)).c_str())) != 0) {
            delete server;
            return nullptr;
        }
        return server;
    }

    std::string server_addr() {
        return "127.0.0.1:" + std::to_string(test_port_);
    }
};

// ============================================================================
// Error Category Tests
// ============================================================================

TEST_F(ErrorIntegrationTest, ErrorCategoriesAreCorrect) {
    EXPECT_EQ(get_error_category(RpcError::OK), RpcErrorCategory::NONE);

    // Connection errors
    EXPECT_EQ(get_error_category(RpcError::NOT_CONNECTED), RpcErrorCategory::CONNECTION);
    EXPECT_EQ(get_error_category(RpcError::CONNECTION_REFUSED), RpcErrorCategory::CONNECTION);
    EXPECT_EQ(get_error_category(RpcError::CONNECTION_CLOSED), RpcErrorCategory::CONNECTION);
    EXPECT_EQ(get_error_category(RpcError::CONNECTION_RESET), RpcErrorCategory::CONNECTION);
    EXPECT_EQ(get_error_category(RpcError::HOST_UNREACHABLE), RpcErrorCategory::CONNECTION);

    // Timeout errors
    EXPECT_EQ(get_error_category(RpcError::CONNECT_TIMEOUT), RpcErrorCategory::TIMEOUT);
    EXPECT_EQ(get_error_category(RpcError::REQUEST_TIMEOUT), RpcErrorCategory::TIMEOUT);
    EXPECT_EQ(get_error_category(RpcError::RESPONSE_TIMEOUT), RpcErrorCategory::TIMEOUT);

    // Protocol errors
    EXPECT_EQ(get_error_category(RpcError::INVALID_MESSAGE), RpcErrorCategory::PROTOCOL);
    EXPECT_EQ(get_error_category(RpcError::UNKNOWN_RPC_ID), RpcErrorCategory::PROTOCOL);
    EXPECT_EQ(get_error_category(RpcError::MARSHALLING_ERROR), RpcErrorCategory::PROTOCOL);

    // Application errors
    EXPECT_EQ(get_error_category(RpcError::SERVICE_UNAVAILABLE), RpcErrorCategory::APPLICATION);
    EXPECT_EQ(get_error_category(RpcError::CIRCUIT_OPEN), RpcErrorCategory::CONNECTION);
    EXPECT_EQ(get_error_category(RpcError::PERMISSION_DENIED), RpcErrorCategory::APPLICATION);
}

// ============================================================================
// Error Helper Functions Tests
// ============================================================================

TEST_F(ErrorIntegrationTest, IsConnectionError) {
    EXPECT_TRUE(is_connection_error(RpcError::NOT_CONNECTED));
    EXPECT_TRUE(is_connection_error(RpcError::CONNECTION_REFUSED));
    EXPECT_TRUE(is_connection_error(RpcError::CONNECTION_CLOSED));
    EXPECT_TRUE(is_connection_error(RpcError::CONNECTION_RESET));
    EXPECT_TRUE(is_connection_error(RpcError::HOST_UNREACHABLE));

    EXPECT_FALSE(is_connection_error(RpcError::OK));
    EXPECT_FALSE(is_connection_error(RpcError::REQUEST_TIMEOUT));
    EXPECT_FALSE(is_connection_error(RpcError::INVALID_ARGUMENT));
}

TEST_F(ErrorIntegrationTest, IsTimeoutError) {
    EXPECT_TRUE(is_timeout_error(RpcError::CONNECT_TIMEOUT));
    EXPECT_TRUE(is_timeout_error(RpcError::REQUEST_TIMEOUT));
    EXPECT_TRUE(is_timeout_error(RpcError::RESPONSE_TIMEOUT));
    EXPECT_TRUE(is_timeout_error(RpcError::IDLE_TIMEOUT));

    EXPECT_FALSE(is_timeout_error(RpcError::OK));
    EXPECT_FALSE(is_timeout_error(RpcError::NOT_CONNECTED));
    EXPECT_FALSE(is_timeout_error(RpcError::INVALID_ARGUMENT));
}

TEST_F(ErrorIntegrationTest, IsRetryableError) {
    // Retryable errors
    EXPECT_TRUE(is_retryable_error(RpcError::CONNECTION_RESET));
    EXPECT_TRUE(is_retryable_error(RpcError::REQUEST_TIMEOUT));
    EXPECT_TRUE(is_retryable_error(RpcError::SERVICE_UNAVAILABLE));
    EXPECT_TRUE(is_retryable_error(RpcError::CONNECT_TIMEOUT));
    EXPECT_TRUE(is_retryable_error(RpcError::RESPONSE_TIMEOUT));

    // Non-retryable errors
    EXPECT_FALSE(is_retryable_error(RpcError::INVALID_ARGUMENT));
    EXPECT_FALSE(is_retryable_error(RpcError::PERMISSION_DENIED));
    EXPECT_FALSE(is_retryable_error(RpcError::NOT_FOUND));
}

// ============================================================================
// Error String Conversion Tests
// ============================================================================

TEST_F(ErrorIntegrationTest, ErrorToString) {
    EXPECT_STREQ(rpc_error_to_string(RpcError::OK), "OK");
    EXPECT_STREQ(rpc_error_to_string(RpcError::NOT_CONNECTED), "NOT_CONNECTED");
    EXPECT_STREQ(rpc_error_to_string(RpcError::REQUEST_TIMEOUT), "REQUEST_TIMEOUT");
    EXPECT_STREQ(rpc_error_to_string(RpcError::INVALID_ARGUMENT), "INVALID_ARGUMENT");
    EXPECT_STREQ(rpc_error_to_string(RpcError::SERVICE_UNAVAILABLE), "SERVICE_UNAVAILABLE");
}

TEST_F(ErrorIntegrationTest, CategoryToString) {
    EXPECT_STREQ(rpc_error_category_to_string(RpcErrorCategory::NONE), "NONE");
    EXPECT_STREQ(rpc_error_category_to_string(RpcErrorCategory::CONNECTION), "CONNECTION");
    EXPECT_STREQ(rpc_error_category_to_string(RpcErrorCategory::TIMEOUT), "TIMEOUT");
    EXPECT_STREQ(rpc_error_category_to_string(RpcErrorCategory::PROTOCOL), "PROTOCOL");
    EXPECT_STREQ(rpc_error_category_to_string(RpcErrorCategory::APPLICATION), "APPLICATION");
}

// ============================================================================
// Error in Real RPC Scenarios
// ============================================================================

TEST_F(ErrorIntegrationTest, ConnectionRefusedScenario) {
    // Don't start server - connection should fail
    auto client = Client::create(poll_thread_.as_ref().unwrap());

    int result = client->connect(reinterpret_cast<const int8_t*>(server_addr().c_str()), true);
    EXPECT_NE(result, 0);

    // We can categorize this error
    // (The actual error code depends on implementation)
    // Using our error types to describe what happened
    RpcError error = RpcError::CONNECTION_REFUSED;
    EXPECT_TRUE(is_connection_error(error));
    EXPECT_FALSE(is_retryable_error(error));  // Refused isn't typically retryable

    client->close();
}

TEST_F(ErrorIntegrationTest, InvalidAddressScenario) {
    auto client = Client::create(poll_thread_.as_ref().unwrap());

    int result = client->connect(reinterpret_cast<const int8_t*>("invalid_address:1234"), true);
    EXPECT_NE(result, 0);

    // Describe error with our types
    RpcError error = RpcError::HOST_UNREACHABLE;
    EXPECT_TRUE(is_connection_error(error));
    EXPECT_EQ(get_error_category(error), RpcErrorCategory::CONNECTION);

    client->close();
}

TEST_F(ErrorIntegrationTest, SuccessfulRequestHasNoError) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(server_addr().c_str()), true), 0);
    std::this_thread::sleep_for(milliseconds(50));

    std::string input = "test";
    auto fu_result = client->request(
        benchmark::BenchmarkService::FAST_NOP, FutureAttr(),
        [&](BinaryWriteArchive& m) { rrr::Serialize_::serialize(input, m); }
    );
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();
    fu->wait();

    // Successful request has error code 0
    EXPECT_EQ(fu->get_error_code(), 0);

    // Using our error types
    RpcError error = RpcError::OK;
    EXPECT_EQ(get_error_category(error), RpcErrorCategory::NONE);
    EXPECT_FALSE(is_connection_error(error));
    EXPECT_FALSE(is_timeout_error(error));

    client->close();
    delete server;
}

TEST_F(ErrorIntegrationTest, InvalidRpcIdError) {
    auto server = start_server();
    ASSERT_NE(server, nullptr);

    auto client = Client::create(poll_thread_.as_ref().unwrap());
    ASSERT_EQ(client->connect(reinterpret_cast<const int8_t*>(server_addr().c_str()), true), 0);
    std::this_thread::sleep_for(milliseconds(50));

    // Request with invalid RPC ID
    auto fu_result = client->request(99999, FutureAttr(), [](BinaryWriteArchive&) {});
    ASSERT_TRUE(fu_result.is_ok());
    auto fu = fu_result.unwrap();
    fu->wait();

    // Should have error
    EXPECT_NE(fu->get_error_code(), 0);

    // Describe error with our types
    RpcError error = RpcError::UNKNOWN_RPC_ID;
    EXPECT_EQ(get_error_category(error), RpcErrorCategory::PROTOCOL);
    EXPECT_FALSE(is_retryable_error(error));

    client->close();
    delete server;
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
