#pragma once

// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>





namespace rrr {

/**
 * High-level RPC error categories.
 */
enum class RpcErrorCategory : int {
    NONE = 0,        // No error
    CONNECTION = 1,  // Network/connection issues
    PROTOCOL = 2,    // RPC protocol violations
    APPLICATION = 3, // Application-level errors
    TIMEOUT = 4,     // Timeout conditions
    INTERNAL = 5     // Internal/unexpected errors
};

// @safe - Convert category to string
inline const char* rpc_error_category_to_string(RpcErrorCategory cat) {
    switch (cat) {
        case RpcErrorCategory::NONE: return "NONE";
        case RpcErrorCategory::CONNECTION: return "CONNECTION";
        case RpcErrorCategory::PROTOCOL: return "PROTOCOL";
        case RpcErrorCategory::APPLICATION: return "APPLICATION";
        case RpcErrorCategory::TIMEOUT: return "TIMEOUT";
        case RpcErrorCategory::INTERNAL: return "INTERNAL";
        default: return "UNKNOWN";
    }
}

/**
 * Detailed RPC error codes.
 */
enum class RpcError : int {
    // No error (0)
    OK = 0,

    // Connection errors (100-199)
    NOT_CONNECTED = 100,
    CONNECTION_REFUSED = 101,
    CONNECTION_RESET = 102,
    NETWORK_UNREACHABLE = 103,
    HOST_UNREACHABLE = 104,
    CONNECTION_CLOSED = 105,
    CIRCUIT_OPEN = 106,

    // Protocol errors (200-299)
    INVALID_MESSAGE = 200,
    UNKNOWN_RPC_ID = 201,
    MARSHALLING_ERROR = 202,
    VERSION_MISMATCH = 203,
    CHECKSUM_ERROR = 204,

    // Application errors (300-399)
    RPC_FAILED = 300,
    SERVICE_UNAVAILABLE = 301,
    PERMISSION_DENIED = 302,
    INVALID_ARGUMENT = 303,
    NOT_FOUND = 304,
    ALREADY_EXISTS = 305,

    // Timeout errors (400-499)
    CONNECT_TIMEOUT = 400,
    REQUEST_TIMEOUT = 401,
    RESPONSE_TIMEOUT = 402,
    IDLE_TIMEOUT = 403,
    HEARTBEAT_TIMEOUT = 404,

    // Internal errors (500-599)
    UNKNOWN_ERROR = 500,
    OUT_OF_MEMORY = 501,
    INVALID_STATE = 502,
    INTERNAL_ERROR = 503
};

// @safe - Convert error code to string
inline const char* rpc_error_to_string(RpcError err) {
    switch (err) {
        // No error
        case RpcError::OK: return "OK";

        // Connection errors
        case RpcError::NOT_CONNECTED: return "NOT_CONNECTED";
        case RpcError::CONNECTION_REFUSED: return "CONNECTION_REFUSED";
        case RpcError::CONNECTION_RESET: return "CONNECTION_RESET";
        case RpcError::NETWORK_UNREACHABLE: return "NETWORK_UNREACHABLE";
        case RpcError::HOST_UNREACHABLE: return "HOST_UNREACHABLE";
        case RpcError::CONNECTION_CLOSED: return "CONNECTION_CLOSED";
        case RpcError::CIRCUIT_OPEN: return "CIRCUIT_OPEN";

        // Protocol errors
        case RpcError::INVALID_MESSAGE: return "INVALID_MESSAGE";
        case RpcError::UNKNOWN_RPC_ID: return "UNKNOWN_RPC_ID";
        case RpcError::MARSHALLING_ERROR: return "MARSHALLING_ERROR";
        case RpcError::VERSION_MISMATCH: return "VERSION_MISMATCH";
        case RpcError::CHECKSUM_ERROR: return "CHECKSUM_ERROR";

        // Application errors
        case RpcError::RPC_FAILED: return "RPC_FAILED";
        case RpcError::SERVICE_UNAVAILABLE: return "SERVICE_UNAVAILABLE";
        case RpcError::PERMISSION_DENIED: return "PERMISSION_DENIED";
        case RpcError::INVALID_ARGUMENT: return "INVALID_ARGUMENT";
        case RpcError::NOT_FOUND: return "NOT_FOUND";
        case RpcError::ALREADY_EXISTS: return "ALREADY_EXISTS";

        // Timeout errors
        case RpcError::CONNECT_TIMEOUT: return "CONNECT_TIMEOUT";
        case RpcError::REQUEST_TIMEOUT: return "REQUEST_TIMEOUT";
        case RpcError::RESPONSE_TIMEOUT: return "RESPONSE_TIMEOUT";
        case RpcError::IDLE_TIMEOUT: return "IDLE_TIMEOUT";
        case RpcError::HEARTBEAT_TIMEOUT: return "HEARTBEAT_TIMEOUT";

        // Internal errors
        case RpcError::UNKNOWN_ERROR: return "UNKNOWN_ERROR";
        case RpcError::OUT_OF_MEMORY: return "OUT_OF_MEMORY";
        case RpcError::INVALID_STATE: return "INVALID_STATE";
        case RpcError::INTERNAL_ERROR: return "INTERNAL_ERROR";

        default: return "UNKNOWN";
    }
}

// @safe - Get category for error code
inline RpcErrorCategory get_error_category(RpcError err) {
    int code = static_cast<int>(err);
    if (code == 0) return RpcErrorCategory::NONE;
    if (code >= 100 && code < 200) return RpcErrorCategory::CONNECTION;
    if (code >= 200 && code < 300) return RpcErrorCategory::PROTOCOL;
    if (code >= 300 && code < 400) return RpcErrorCategory::APPLICATION;
    if (code >= 400 && code < 500) return RpcErrorCategory::TIMEOUT;
    return RpcErrorCategory::INTERNAL;
}

// @safe - Check if error indicates connection issues
inline bool is_connection_error(RpcError err) {
    return get_error_category(err) == RpcErrorCategory::CONNECTION;
}

// @safe - Check if error indicates timeout
inline bool is_timeout_error(RpcError err) {
    return get_error_category(err) == RpcErrorCategory::TIMEOUT;
}

// @safe - Check if error is retryable
inline bool is_retryable_error(RpcError err) {
    switch (err) {
        case RpcError::CONNECTION_RESET:
        case RpcError::NETWORK_UNREACHABLE:
        case RpcError::HOST_UNREACHABLE:
        case RpcError::CONNECT_TIMEOUT:
        case RpcError::REQUEST_TIMEOUT:
        case RpcError::RESPONSE_TIMEOUT:
        case RpcError::SERVICE_UNAVAILABLE:
            return true;
        default:
            return false;
    }
}

} // namespace rrr
