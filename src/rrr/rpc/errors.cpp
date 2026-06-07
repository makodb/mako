module;

#include <rusty/move.hpp>
#include <rusty/slice.hpp>

#include <cstdint>

export module rrr.errors;

import std;
import rusty;

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

inline RpcErrorCategory get_error_category(RpcError err) {
    int code = static_cast<int>(err);
    if (code == 0) return RpcErrorCategory::NONE;
    if (code >= 100 && code < 200) return RpcErrorCategory::CONNECTION;
    if (code >= 200 && code < 300) return RpcErrorCategory::PROTOCOL;
    if (code >= 300 && code < 400) return RpcErrorCategory::APPLICATION;
    if (code >= 400 && code < 500) return RpcErrorCategory::TIMEOUT;
    return RpcErrorCategory::INTERNAL;
}

inline bool is_connection_error(RpcError err) {
    return get_error_category(err) == RpcErrorCategory::CONNECTION;
}

inline bool is_timeout_error(RpcError err) {
    return get_error_category(err) == RpcErrorCategory::TIMEOUT;
}

// `is_retryable_error` — pure classification of which `RpcError` codes
// the client should retry on (transient connection/timeout faults).
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
//
// Avoids `match err { … }` — the transpiler currently emits the entire
// rusty cmp/Ord standard-library scaffolding (~130 KB of inlined Rust-
// std code) when it sees a match arm pattern over an enum. The
// equivalent if/else-if chain emits cleanly. Switch back to `match`
// once the transpiler stops shipping cmp/Ord adapters for enum-match.
#if RUSTYCPP_RUST
fn is_retryable_error(err: RpcError) -> bool {
    if err == RpcError::CONNECTION_RESET { true }
    else if err == RpcError::NETWORK_UNREACHABLE { true }
    else if err == RpcError::HOST_UNREACHABLE { true }
    else if err == RpcError::CONNECT_TIMEOUT { true }
    else if err == RpcError::REQUEST_TIMEOUT { true }
    else if err == RpcError::RESPONSE_TIMEOUT { true }
    else if err == RpcError::SERVICE_UNAVAILABLE { true }
    else { false }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=errors.is_retryable_error version=1 rust_sha256=20628b5cb37cb5cf5202b6560f0b7edeb76f104ebacbe703565a1812cad0a44e*/
bool is_retryable_error(RpcError err) {
    if (rusty::detail::deref_if_pointer_like(err) == rusty::clone(RpcError::CONNECTION_RESET)) {
        return true;
    } else if (rusty::detail::deref_if_pointer_like(err) == rusty::clone(RpcError::NETWORK_UNREACHABLE)) {
        return true;
    } else if (rusty::detail::deref_if_pointer_like(err) == rusty::clone(RpcError::HOST_UNREACHABLE)) {
        return true;
    } else if (rusty::detail::deref_if_pointer_like(err) == rusty::clone(RpcError::CONNECT_TIMEOUT)) {
        return true;
    } else if (rusty::detail::deref_if_pointer_like(err) == rusty::clone(RpcError::REQUEST_TIMEOUT)) {
        return true;
    } else if (rusty::detail::deref_if_pointer_like(err) == rusty::clone(RpcError::RESPONSE_TIMEOUT)) {
        return true;
    } else if (rusty::detail::deref_if_pointer_like(err) == rusty::clone(RpcError::SERVICE_UNAVAILABLE)) {
        return true;
    } else {
        return false;
    }
}
/*RUSTYCPP:GEN-END id=errors.is_retryable_error*/

} // export namespace rrr
