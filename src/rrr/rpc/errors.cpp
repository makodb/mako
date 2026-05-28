module;

#include <stdint.h>
#include <rusty/rusty.hpp>

export module rrr.errors;

import std;

// @safe - RPC error enums + classification helpers. Pure switch tables
// + std::string formatting; no raw pointers, syscalls, or operator
// overload chains.
export namespace rrr {

enum class RpcErrorCategory : int {
    NONE = 0,
    CONNECTION = 1,
    PROTOCOL = 2,
    APPLICATION = 3,
    TIMEOUT = 4,
    INTERNAL = 5
};

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

enum class RpcError : int {
    OK = 0,

    NOT_CONNECTED = 100,
    CONNECTION_REFUSED = 101,
    CONNECTION_RESET = 102,
    NETWORK_UNREACHABLE = 103,
    HOST_UNREACHABLE = 104,
    CONNECTION_CLOSED = 105,
    CIRCUIT_OPEN = 106,

    INVALID_MESSAGE = 200,
    UNKNOWN_RPC_ID = 201,
    MARSHALLING_ERROR = 202,
    VERSION_MISMATCH = 203,
    CHECKSUM_ERROR = 204,

    RPC_FAILED = 300,
    SERVICE_UNAVAILABLE = 301,
    PERMISSION_DENIED = 302,
    INVALID_ARGUMENT = 303,
    NOT_FOUND = 304,
    ALREADY_EXISTS = 305,

    CONNECT_TIMEOUT = 400,
    REQUEST_TIMEOUT = 401,
    RESPONSE_TIMEOUT = 402,
    IDLE_TIMEOUT = 403,
    HEARTBEAT_TIMEOUT = 404,

    UNKNOWN_ERROR = 500,
    OUT_OF_MEMORY = 501,
    INVALID_STATE = 502,
    INTERNAL_ERROR = 503
};

inline const char* rpc_error_to_string(RpcError err) {
    switch (err) {
        case RpcError::OK: return "OK";

        case RpcError::NOT_CONNECTED: return "NOT_CONNECTED";
        case RpcError::CONNECTION_REFUSED: return "CONNECTION_REFUSED";
        case RpcError::CONNECTION_RESET: return "CONNECTION_RESET";
        case RpcError::NETWORK_UNREACHABLE: return "NETWORK_UNREACHABLE";
        case RpcError::HOST_UNREACHABLE: return "HOST_UNREACHABLE";
        case RpcError::CONNECTION_CLOSED: return "CONNECTION_CLOSED";
        case RpcError::CIRCUIT_OPEN: return "CIRCUIT_OPEN";

        case RpcError::INVALID_MESSAGE: return "INVALID_MESSAGE";
        case RpcError::UNKNOWN_RPC_ID: return "UNKNOWN_RPC_ID";
        case RpcError::MARSHALLING_ERROR: return "MARSHALLING_ERROR";
        case RpcError::VERSION_MISMATCH: return "VERSION_MISMATCH";
        case RpcError::CHECKSUM_ERROR: return "CHECKSUM_ERROR";

        case RpcError::RPC_FAILED: return "RPC_FAILED";
        case RpcError::SERVICE_UNAVAILABLE: return "SERVICE_UNAVAILABLE";
        case RpcError::PERMISSION_DENIED: return "PERMISSION_DENIED";
        case RpcError::INVALID_ARGUMENT: return "INVALID_ARGUMENT";
        case RpcError::NOT_FOUND: return "NOT_FOUND";
        case RpcError::ALREADY_EXISTS: return "ALREADY_EXISTS";

        case RpcError::CONNECT_TIMEOUT: return "CONNECT_TIMEOUT";
        case RpcError::REQUEST_TIMEOUT: return "REQUEST_TIMEOUT";
        case RpcError::RESPONSE_TIMEOUT: return "RESPONSE_TIMEOUT";
        case RpcError::IDLE_TIMEOUT: return "IDLE_TIMEOUT";
        case RpcError::HEARTBEAT_TIMEOUT: return "HEARTBEAT_TIMEOUT";

        case RpcError::UNKNOWN_ERROR: return "UNKNOWN_ERROR";
        case RpcError::OUT_OF_MEMORY: return "OUT_OF_MEMORY";
        case RpcError::INVALID_STATE: return "INVALID_STATE";
        case RpcError::INTERNAL_ERROR: return "INTERNAL_ERROR";

        default: return "UNKNOWN";
    }
}

// Free helper backing `get_error_category`. Authored as inline Rust
// DSL: takes the raw integer code and returns the matching
// RpcErrorCategory's integer discriminant. The C++ wrapper casts at
// the boundary so the public RpcError / RpcErrorCategory surface
// is unchanged. See docs/dev/rrr_rust_dsl_migration_plan.md.
#if RUSTYCPP_RUST
fn rpc_error_category_code(code: i32) -> i32 {
    if code == 0 {
        return 0; // NONE
    }
    if code >= 100 && code < 200 {
        return 1; // CONNECTION
    }
    if code >= 200 && code < 300 {
        return 2; // PROTOCOL
    }
    if code >= 300 && code < 400 {
        return 3; // APPLICATION
    }
    if code >= 400 && code < 500 {
        return 4; // TIMEOUT
    }
    5 // INTERNAL
}
#endif
/*RUSTYCPP:GEN-BEGIN id=errors.1 version=1 rust_sha256=e8bd412072b41c9da1042a00663b430c2ce1f79c2cea4e10532efe09fbf1c5ed*/
int32_t rpc_error_category_code(int32_t code);

int32_t rpc_error_category_code(int32_t code) {
    if (rusty::detail::deref_if_pointer_like(code) == static_cast<int32_t>(0)) {
        return static_cast<int32_t>(0);
    }
    if ((rusty::detail::deref_if_pointer_like(code) >= 100) && (rusty::detail::deref_if_pointer_like(code) < 200)) {
        return static_cast<int32_t>(1);
    }
    if ((rusty::detail::deref_if_pointer_like(code) >= 200) && (rusty::detail::deref_if_pointer_like(code) < 300)) {
        return static_cast<int32_t>(2);
    }
    if ((rusty::detail::deref_if_pointer_like(code) >= 300) && (rusty::detail::deref_if_pointer_like(code) < 400)) {
        return static_cast<int32_t>(3);
    }
    if ((rusty::detail::deref_if_pointer_like(code) >= 400) && (rusty::detail::deref_if_pointer_like(code) < 500)) {
        return static_cast<int32_t>(4);
    }
    return static_cast<int32_t>(5);
}
/*RUSTYCPP:GEN-END id=errors.1*/

inline RpcErrorCategory get_error_category(RpcError err) {
    return static_cast<RpcErrorCategory>(
        rpc_error_category_code(static_cast<int32_t>(err)));
}

inline bool is_connection_error(RpcError err) {
    return get_error_category(err) == RpcErrorCategory::CONNECTION;
}

inline bool is_timeout_error(RpcError err) {
    return get_error_category(err) == RpcErrorCategory::TIMEOUT;
}

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

} // export namespace rrr
