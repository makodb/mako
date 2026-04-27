/**
 * Unit tests for RPC error types.
 * Tests categories, codes, and helper functions.
 */

#include <gtest/gtest.h>
#include <string>
#include "../rrr.hpp"

using namespace rrr;

// ============================================================================
// Error Category Tests
// ============================================================================

TEST(RpcErrorCategoryTest, CategoryToString) {
    EXPECT_STREQ(rpc_error_category_to_string(RpcErrorCategory::NONE), "NONE");
    EXPECT_STREQ(rpc_error_category_to_string(RpcErrorCategory::CONNECTION), "CONNECTION");
    EXPECT_STREQ(rpc_error_category_to_string(RpcErrorCategory::PROTOCOL), "PROTOCOL");
    EXPECT_STREQ(rpc_error_category_to_string(RpcErrorCategory::APPLICATION), "APPLICATION");
    EXPECT_STREQ(rpc_error_category_to_string(RpcErrorCategory::TIMEOUT), "TIMEOUT");
    EXPECT_STREQ(rpc_error_category_to_string(RpcErrorCategory::INTERNAL), "INTERNAL");
}

// ============================================================================
// Error Code to String Tests
// ============================================================================

TEST(RpcErrorTest, ErrorToStringNoError) {
    EXPECT_STREQ(rpc_error_to_string(RpcError::OK), "OK");
}

TEST(RpcErrorTest, ErrorToStringConnection) {
    EXPECT_STREQ(rpc_error_to_string(RpcError::NOT_CONNECTED), "NOT_CONNECTED");
    EXPECT_STREQ(rpc_error_to_string(RpcError::CONNECTION_REFUSED), "CONNECTION_REFUSED");
    EXPECT_STREQ(rpc_error_to_string(RpcError::CONNECTION_RESET), "CONNECTION_RESET");
    EXPECT_STREQ(rpc_error_to_string(RpcError::NETWORK_UNREACHABLE), "NETWORK_UNREACHABLE");
    EXPECT_STREQ(rpc_error_to_string(RpcError::HOST_UNREACHABLE), "HOST_UNREACHABLE");
    EXPECT_STREQ(rpc_error_to_string(RpcError::CONNECTION_CLOSED), "CONNECTION_CLOSED");
    EXPECT_STREQ(rpc_error_to_string(RpcError::CIRCUIT_OPEN), "CIRCUIT_OPEN");
}

TEST(RpcErrorTest, ErrorToStringProtocol) {
    EXPECT_STREQ(rpc_error_to_string(RpcError::INVALID_MESSAGE), "INVALID_MESSAGE");
    EXPECT_STREQ(rpc_error_to_string(RpcError::UNKNOWN_RPC_ID), "UNKNOWN_RPC_ID");
    EXPECT_STREQ(rpc_error_to_string(RpcError::MARSHALLING_ERROR), "MARSHALLING_ERROR");
    EXPECT_STREQ(rpc_error_to_string(RpcError::VERSION_MISMATCH), "VERSION_MISMATCH");
    EXPECT_STREQ(rpc_error_to_string(RpcError::CHECKSUM_ERROR), "CHECKSUM_ERROR");
}

TEST(RpcErrorTest, ErrorToStringApplication) {
    EXPECT_STREQ(rpc_error_to_string(RpcError::RPC_FAILED), "RPC_FAILED");
    EXPECT_STREQ(rpc_error_to_string(RpcError::SERVICE_UNAVAILABLE), "SERVICE_UNAVAILABLE");
    EXPECT_STREQ(rpc_error_to_string(RpcError::PERMISSION_DENIED), "PERMISSION_DENIED");
    EXPECT_STREQ(rpc_error_to_string(RpcError::INVALID_ARGUMENT), "INVALID_ARGUMENT");
    EXPECT_STREQ(rpc_error_to_string(RpcError::NOT_FOUND), "NOT_FOUND");
    EXPECT_STREQ(rpc_error_to_string(RpcError::ALREADY_EXISTS), "ALREADY_EXISTS");
}

TEST(RpcErrorTest, ErrorToStringTimeout) {
    EXPECT_STREQ(rpc_error_to_string(RpcError::CONNECT_TIMEOUT), "CONNECT_TIMEOUT");
    EXPECT_STREQ(rpc_error_to_string(RpcError::REQUEST_TIMEOUT), "REQUEST_TIMEOUT");
    EXPECT_STREQ(rpc_error_to_string(RpcError::RESPONSE_TIMEOUT), "RESPONSE_TIMEOUT");
    EXPECT_STREQ(rpc_error_to_string(RpcError::IDLE_TIMEOUT), "IDLE_TIMEOUT");
    EXPECT_STREQ(rpc_error_to_string(RpcError::HEARTBEAT_TIMEOUT), "HEARTBEAT_TIMEOUT");
}

TEST(RpcErrorTest, ErrorToStringInternal) {
    EXPECT_STREQ(rpc_error_to_string(RpcError::UNKNOWN_ERROR), "UNKNOWN_ERROR");
    EXPECT_STREQ(rpc_error_to_string(RpcError::OUT_OF_MEMORY), "OUT_OF_MEMORY");
    EXPECT_STREQ(rpc_error_to_string(RpcError::INVALID_STATE), "INVALID_STATE");
    EXPECT_STREQ(rpc_error_to_string(RpcError::INTERNAL_ERROR), "INTERNAL_ERROR");
}

// ============================================================================
// Get Error Category Tests
// ============================================================================

TEST(RpcErrorTest, GetErrorCategoryNone) {
    EXPECT_EQ(get_error_category(RpcError::OK), RpcErrorCategory::NONE);
}

TEST(RpcErrorTest, GetErrorCategoryConnection) {
    EXPECT_EQ(get_error_category(RpcError::NOT_CONNECTED), RpcErrorCategory::CONNECTION);
    EXPECT_EQ(get_error_category(RpcError::CONNECTION_REFUSED), RpcErrorCategory::CONNECTION);
    EXPECT_EQ(get_error_category(RpcError::CONNECTION_RESET), RpcErrorCategory::CONNECTION);
    EXPECT_EQ(get_error_category(RpcError::CONNECTION_CLOSED), RpcErrorCategory::CONNECTION);
}

TEST(RpcErrorTest, GetErrorCategoryProtocol) {
    EXPECT_EQ(get_error_category(RpcError::INVALID_MESSAGE), RpcErrorCategory::PROTOCOL);
    EXPECT_EQ(get_error_category(RpcError::UNKNOWN_RPC_ID), RpcErrorCategory::PROTOCOL);
    EXPECT_EQ(get_error_category(RpcError::MARSHALLING_ERROR), RpcErrorCategory::PROTOCOL);
}

TEST(RpcErrorTest, GetErrorCategoryApplication) {
    EXPECT_EQ(get_error_category(RpcError::RPC_FAILED), RpcErrorCategory::APPLICATION);
    EXPECT_EQ(get_error_category(RpcError::SERVICE_UNAVAILABLE), RpcErrorCategory::APPLICATION);
    EXPECT_EQ(get_error_category(RpcError::PERMISSION_DENIED), RpcErrorCategory::APPLICATION);
}

TEST(RpcErrorTest, GetErrorCategoryTimeout) {
    EXPECT_EQ(get_error_category(RpcError::CONNECT_TIMEOUT), RpcErrorCategory::TIMEOUT);
    EXPECT_EQ(get_error_category(RpcError::REQUEST_TIMEOUT), RpcErrorCategory::TIMEOUT);
    EXPECT_EQ(get_error_category(RpcError::RESPONSE_TIMEOUT), RpcErrorCategory::TIMEOUT);
}

TEST(RpcErrorTest, GetErrorCategoryInternal) {
    EXPECT_EQ(get_error_category(RpcError::UNKNOWN_ERROR), RpcErrorCategory::INTERNAL);
    EXPECT_EQ(get_error_category(RpcError::OUT_OF_MEMORY), RpcErrorCategory::INTERNAL);
    EXPECT_EQ(get_error_category(RpcError::INTERNAL_ERROR), RpcErrorCategory::INTERNAL);
}

// ============================================================================
// Helper Function Tests
// ============================================================================

TEST(RpcErrorTest, IsConnectionError) {
    EXPECT_TRUE(is_connection_error(RpcError::NOT_CONNECTED));
    EXPECT_TRUE(is_connection_error(RpcError::CONNECTION_REFUSED));
    EXPECT_TRUE(is_connection_error(RpcError::CONNECTION_RESET));

    EXPECT_FALSE(is_connection_error(RpcError::OK));
    EXPECT_FALSE(is_connection_error(RpcError::REQUEST_TIMEOUT));
    EXPECT_FALSE(is_connection_error(RpcError::RPC_FAILED));
}

TEST(RpcErrorTest, IsTimeoutError) {
    EXPECT_TRUE(is_timeout_error(RpcError::CONNECT_TIMEOUT));
    EXPECT_TRUE(is_timeout_error(RpcError::REQUEST_TIMEOUT));
    EXPECT_TRUE(is_timeout_error(RpcError::RESPONSE_TIMEOUT));

    EXPECT_FALSE(is_timeout_error(RpcError::OK));
    EXPECT_FALSE(is_timeout_error(RpcError::NOT_CONNECTED));
    EXPECT_FALSE(is_timeout_error(RpcError::RPC_FAILED));
}

TEST(RpcErrorTest, IsRetryableError) {
    // Retryable errors
    EXPECT_TRUE(is_retryable_error(RpcError::CONNECTION_RESET));
    EXPECT_TRUE(is_retryable_error(RpcError::NETWORK_UNREACHABLE));
    EXPECT_TRUE(is_retryable_error(RpcError::HOST_UNREACHABLE));
    EXPECT_TRUE(is_retryable_error(RpcError::CONNECT_TIMEOUT));
    EXPECT_TRUE(is_retryable_error(RpcError::REQUEST_TIMEOUT));
    EXPECT_TRUE(is_retryable_error(RpcError::RESPONSE_TIMEOUT));
    EXPECT_TRUE(is_retryable_error(RpcError::SERVICE_UNAVAILABLE));

    // Non-retryable errors
    EXPECT_FALSE(is_retryable_error(RpcError::OK));
    EXPECT_FALSE(is_retryable_error(RpcError::PERMISSION_DENIED));
    EXPECT_FALSE(is_retryable_error(RpcError::INVALID_ARGUMENT));
    EXPECT_FALSE(is_retryable_error(RpcError::INVALID_MESSAGE));
    EXPECT_FALSE(is_retryable_error(RpcError::INTERNAL_ERROR));
}

TEST(RpcErrorTest, UnknownCategoryStringFallsBackToUnknown) {
    auto unknown = static_cast<RpcErrorCategory>(999);
    EXPECT_STREQ(rpc_error_category_to_string(unknown), "UNKNOWN");
}

TEST(RpcErrorTest, UnknownErrorStringFallsBackToUnknown) {
    auto unknown = static_cast<RpcError>(999999);
    EXPECT_STREQ(rpc_error_to_string(unknown), "UNKNOWN");
}

// ============================================================================
// Error Code Ranges Tests
// ============================================================================

TEST(RpcErrorTest, ErrorCodeRanges) {
    // Verify error codes are in expected ranges
    EXPECT_EQ(static_cast<int>(RpcError::OK), 0);

    // Connection: 100-199
    EXPECT_GE(static_cast<int>(RpcError::NOT_CONNECTED), 100);
    EXPECT_LT(static_cast<int>(RpcError::CIRCUIT_OPEN), 200);

    // Protocol: 200-299
    EXPECT_GE(static_cast<int>(RpcError::INVALID_MESSAGE), 200);
    EXPECT_LT(static_cast<int>(RpcError::CHECKSUM_ERROR), 300);

    // Application: 300-399
    EXPECT_GE(static_cast<int>(RpcError::RPC_FAILED), 300);
    EXPECT_LT(static_cast<int>(RpcError::ALREADY_EXISTS), 400);

    // Timeout: 400-499
    EXPECT_GE(static_cast<int>(RpcError::CONNECT_TIMEOUT), 400);
    EXPECT_LT(static_cast<int>(RpcError::HEARTBEAT_TIMEOUT), 500);

    // Internal: 500-599
    EXPECT_GE(static_cast<int>(RpcError::UNKNOWN_ERROR), 500);
    EXPECT_LT(static_cast<int>(RpcError::INTERNAL_ERROR), 600);
}
