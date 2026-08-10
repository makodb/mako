module;

#include <rusty/move.hpp>
#include <rusty/slice.hpp>
// The `match` arms below generate a `rusty::intrinsics::unreachable_panic()`
// fallthrough, and the GMF must include what its own GEN names (same rule
// that puts this include in channel.cpp).
#include <rusty/intrinsics.hpp>

#include <cstdint>

export module rrr.errors;

import std;
import rusty;

// @safe - RPC error enums + classification helpers. Pure switch tables
// + std::string formatting; no raw pointers, syscalls, or operator
// overload chains.
export namespace rrr {

// `RpcErrorCategory` — coarse classification of RpcError codes.
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
#if RUSTYCPP_RUST
#[repr(i32)]
enum RpcErrorCategory {
    NONE = 0,
    CONNECTION = 1,
    PROTOCOL = 2,
    APPLICATION = 3,
    TIMEOUT = 4,
    INTERNAL = 5,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=errors.error_category version=1 rust_sha256=0764a7d798d1c61ccbd2010d539ce49df144abf40538af1d419f855a4adac3af*/
enum class RpcErrorCategory;
constexpr RpcErrorCategory RpcErrorCategory_NONE();
constexpr RpcErrorCategory RpcErrorCategory_CONNECTION();
constexpr RpcErrorCategory RpcErrorCategory_PROTOCOL();
constexpr RpcErrorCategory RpcErrorCategory_APPLICATION();
constexpr RpcErrorCategory RpcErrorCategory_TIMEOUT();
constexpr RpcErrorCategory RpcErrorCategory_INTERNAL();

enum class RpcErrorCategory {
    NONE = 0,
    CONNECTION = 1,
    PROTOCOL = 2,
    APPLICATION = 3,
    TIMEOUT = 4,
    INTERNAL = 5
};
inline constexpr RpcErrorCategory RpcErrorCategory_NONE() { return RpcErrorCategory::NONE; }
inline constexpr RpcErrorCategory RpcErrorCategory_CONNECTION() { return RpcErrorCategory::CONNECTION; }
inline constexpr RpcErrorCategory RpcErrorCategory_PROTOCOL() { return RpcErrorCategory::PROTOCOL; }
inline constexpr RpcErrorCategory RpcErrorCategory_APPLICATION() { return RpcErrorCategory::APPLICATION; }
inline constexpr RpcErrorCategory RpcErrorCategory_TIMEOUT() { return RpcErrorCategory::TIMEOUT; }
inline constexpr RpcErrorCategory RpcErrorCategory_INTERNAL() { return RpcErrorCategory::INTERNAL; }
/*RUSTYCPP:GEN-END id=errors.error_category*/

// Returns `&'static str` (lowering to std::string_view), not `const char*`.
// Same shape as logging.cpp's log_level_tag. (The original reason -- "the
// DSL has no way to spell a literal as a raw pointer" -- overstates it:
// `core::ptr::null()` and casts do lower. `&'static str` is kept because
// a string_view return is the better API, not because char* is
// unreachable.) Callers are tests only, and EXPECT_STREQ
// (which needs a char*) becomes EXPECT_EQ against a string_view — the same
// assertion, since string_view compares equal to a string literal.
#if RUSTYCPP_RUST
fn rpc_error_category_to_string(cat: RpcErrorCategory) -> &'static str {
    match cat {
        RpcErrorCategory::NONE => "NONE",
        RpcErrorCategory::CONNECTION => "CONNECTION",
        RpcErrorCategory::PROTOCOL => "PROTOCOL",
        RpcErrorCategory::APPLICATION => "APPLICATION",
        RpcErrorCategory::TIMEOUT => "TIMEOUT",
        RpcErrorCategory::INTERNAL => "INTERNAL",
        _ => "UNKNOWN",
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=errors.2 version=1 rust_sha256=a4dce9e46029aff6d693510d0194f96abf0a97726d637cac59cf3788c9e7c49d*/
std::string_view rpc_error_category_to_string(RpcErrorCategory cat) {
    return ({ auto&& _m = cat; std::optional<std::string_view> _match_value; bool _m_matched = false; if (!_m_matched && (_m == RpcErrorCategory::NONE)) { _match_value.emplace(std::move(std::string_view("NONE"))); _m_matched = true; } if (!_m_matched && (_m == RpcErrorCategory::CONNECTION)) { _match_value.emplace(std::move(std::string_view("CONNECTION"))); _m_matched = true; } if (!_m_matched && (_m == RpcErrorCategory::PROTOCOL)) { _match_value.emplace(std::move(std::string_view("PROTOCOL"))); _m_matched = true; } if (!_m_matched && (_m == RpcErrorCategory::APPLICATION)) { _match_value.emplace(std::move(std::string_view("APPLICATION"))); _m_matched = true; } if (!_m_matched && (_m == RpcErrorCategory::TIMEOUT)) { _match_value.emplace(std::move(std::string_view("TIMEOUT"))); _m_matched = true; } if (!_m_matched && (_m == RpcErrorCategory::INTERNAL)) { _match_value.emplace(std::move(std::string_view("INTERNAL"))); _m_matched = true; } if (!_m_matched) { _match_value.emplace(std::move(std::string_view("UNKNOWN"))); _m_matched = true; } if (!_m_matched) { rusty::intrinsics::unreachable_panic(); } std::move(_match_value).value(); });
}
/*RUSTYCPP:GEN-END id=errors.2*/

// `RpcError` — categorical RPC error code; encoded as i32 over the
// wire. Code ranges 100/200/300/400/500 correspond to the matching
// `RpcErrorCategory` (see `get_error_category` below). Authored as
// inline Rust DSL: the `#if RUSTYCPP_RUST` block below is the source
// of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
#if RUSTYCPP_RUST
#[repr(i32)]
enum RpcError {
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
    INTERNAL_ERROR = 503,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=errors.rpc_error version=1 rust_sha256=4ab5aa6ba1d0a134f33836da246ce2b68115b5bda9668ef467e1d4b44863c4af*/
enum class RpcError;
constexpr RpcError RpcError_OK();
constexpr RpcError RpcError_NOT_CONNECTED();
constexpr RpcError RpcError_CONNECTION_REFUSED();
constexpr RpcError RpcError_CONNECTION_RESET();
constexpr RpcError RpcError_NETWORK_UNREACHABLE();
constexpr RpcError RpcError_HOST_UNREACHABLE();
constexpr RpcError RpcError_CONNECTION_CLOSED();
constexpr RpcError RpcError_CIRCUIT_OPEN();
constexpr RpcError RpcError_INVALID_MESSAGE();
constexpr RpcError RpcError_UNKNOWN_RPC_ID();
constexpr RpcError RpcError_MARSHALLING_ERROR();
constexpr RpcError RpcError_VERSION_MISMATCH();
constexpr RpcError RpcError_CHECKSUM_ERROR();
constexpr RpcError RpcError_RPC_FAILED();
constexpr RpcError RpcError_SERVICE_UNAVAILABLE();
constexpr RpcError RpcError_PERMISSION_DENIED();
constexpr RpcError RpcError_INVALID_ARGUMENT();
constexpr RpcError RpcError_NOT_FOUND();
constexpr RpcError RpcError_ALREADY_EXISTS();
constexpr RpcError RpcError_CONNECT_TIMEOUT();
constexpr RpcError RpcError_REQUEST_TIMEOUT();
constexpr RpcError RpcError_RESPONSE_TIMEOUT();
constexpr RpcError RpcError_IDLE_TIMEOUT();
constexpr RpcError RpcError_HEARTBEAT_TIMEOUT();
constexpr RpcError RpcError_UNKNOWN_ERROR();
constexpr RpcError RpcError_OUT_OF_MEMORY();
constexpr RpcError RpcError_INVALID_STATE();
constexpr RpcError RpcError_INTERNAL_ERROR();

enum class RpcError {
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
inline constexpr RpcError RpcError_OK() { return RpcError::OK; }
inline constexpr RpcError RpcError_NOT_CONNECTED() { return RpcError::NOT_CONNECTED; }
inline constexpr RpcError RpcError_CONNECTION_REFUSED() { return RpcError::CONNECTION_REFUSED; }
inline constexpr RpcError RpcError_CONNECTION_RESET() { return RpcError::CONNECTION_RESET; }
inline constexpr RpcError RpcError_NETWORK_UNREACHABLE() { return RpcError::NETWORK_UNREACHABLE; }
inline constexpr RpcError RpcError_HOST_UNREACHABLE() { return RpcError::HOST_UNREACHABLE; }
inline constexpr RpcError RpcError_CONNECTION_CLOSED() { return RpcError::CONNECTION_CLOSED; }
inline constexpr RpcError RpcError_CIRCUIT_OPEN() { return RpcError::CIRCUIT_OPEN; }
inline constexpr RpcError RpcError_INVALID_MESSAGE() { return RpcError::INVALID_MESSAGE; }
inline constexpr RpcError RpcError_UNKNOWN_RPC_ID() { return RpcError::UNKNOWN_RPC_ID; }
inline constexpr RpcError RpcError_MARSHALLING_ERROR() { return RpcError::MARSHALLING_ERROR; }
inline constexpr RpcError RpcError_VERSION_MISMATCH() { return RpcError::VERSION_MISMATCH; }
inline constexpr RpcError RpcError_CHECKSUM_ERROR() { return RpcError::CHECKSUM_ERROR; }
inline constexpr RpcError RpcError_RPC_FAILED() { return RpcError::RPC_FAILED; }
inline constexpr RpcError RpcError_SERVICE_UNAVAILABLE() { return RpcError::SERVICE_UNAVAILABLE; }
inline constexpr RpcError RpcError_PERMISSION_DENIED() { return RpcError::PERMISSION_DENIED; }
inline constexpr RpcError RpcError_INVALID_ARGUMENT() { return RpcError::INVALID_ARGUMENT; }
inline constexpr RpcError RpcError_NOT_FOUND() { return RpcError::NOT_FOUND; }
inline constexpr RpcError RpcError_ALREADY_EXISTS() { return RpcError::ALREADY_EXISTS; }
inline constexpr RpcError RpcError_CONNECT_TIMEOUT() { return RpcError::CONNECT_TIMEOUT; }
inline constexpr RpcError RpcError_REQUEST_TIMEOUT() { return RpcError::REQUEST_TIMEOUT; }
inline constexpr RpcError RpcError_RESPONSE_TIMEOUT() { return RpcError::RESPONSE_TIMEOUT; }
inline constexpr RpcError RpcError_IDLE_TIMEOUT() { return RpcError::IDLE_TIMEOUT; }
inline constexpr RpcError RpcError_HEARTBEAT_TIMEOUT() { return RpcError::HEARTBEAT_TIMEOUT; }
inline constexpr RpcError RpcError_UNKNOWN_ERROR() { return RpcError::UNKNOWN_ERROR; }
inline constexpr RpcError RpcError_OUT_OF_MEMORY() { return RpcError::OUT_OF_MEMORY; }
inline constexpr RpcError RpcError_INVALID_STATE() { return RpcError::INVALID_STATE; }
inline constexpr RpcError RpcError_INTERNAL_ERROR() { return RpcError::INTERNAL_ERROR; }
/*RUSTYCPP:GEN-END id=errors.rpc_error*/

// See rpc_error_category_to_string above for why this returns
// `&'static str` rather than `const char*`.
#if RUSTYCPP_RUST
fn rpc_error_to_string(err: RpcError) -> &'static str {
    match err {
        RpcError::OK => "OK",

        RpcError::NOT_CONNECTED => "NOT_CONNECTED",
        RpcError::CONNECTION_REFUSED => "CONNECTION_REFUSED",
        RpcError::CONNECTION_RESET => "CONNECTION_RESET",
        RpcError::NETWORK_UNREACHABLE => "NETWORK_UNREACHABLE",
        RpcError::HOST_UNREACHABLE => "HOST_UNREACHABLE",
        RpcError::CONNECTION_CLOSED => "CONNECTION_CLOSED",
        RpcError::CIRCUIT_OPEN => "CIRCUIT_OPEN",

        RpcError::INVALID_MESSAGE => "INVALID_MESSAGE",
        RpcError::UNKNOWN_RPC_ID => "UNKNOWN_RPC_ID",
        RpcError::MARSHALLING_ERROR => "MARSHALLING_ERROR",
        RpcError::VERSION_MISMATCH => "VERSION_MISMATCH",
        RpcError::CHECKSUM_ERROR => "CHECKSUM_ERROR",

        RpcError::RPC_FAILED => "RPC_FAILED",
        RpcError::SERVICE_UNAVAILABLE => "SERVICE_UNAVAILABLE",
        RpcError::PERMISSION_DENIED => "PERMISSION_DENIED",
        RpcError::INVALID_ARGUMENT => "INVALID_ARGUMENT",
        RpcError::NOT_FOUND => "NOT_FOUND",
        RpcError::ALREADY_EXISTS => "ALREADY_EXISTS",

        RpcError::CONNECT_TIMEOUT => "CONNECT_TIMEOUT",
        RpcError::REQUEST_TIMEOUT => "REQUEST_TIMEOUT",
        RpcError::RESPONSE_TIMEOUT => "RESPONSE_TIMEOUT",
        RpcError::IDLE_TIMEOUT => "IDLE_TIMEOUT",
        RpcError::HEARTBEAT_TIMEOUT => "HEARTBEAT_TIMEOUT",

        RpcError::UNKNOWN_ERROR => "UNKNOWN_ERROR",
        RpcError::OUT_OF_MEMORY => "OUT_OF_MEMORY",
        RpcError::INVALID_STATE => "INVALID_STATE",
        RpcError::INTERNAL_ERROR => "INTERNAL_ERROR",

        _ => "UNKNOWN",
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=errors.4 version=1 rust_sha256=5034d48b607add72bdebe540114c0bcade56c593c176060b52816d6bc01f12d3*/
std::string_view rpc_error_to_string(RpcError err) {
    return ({ auto&& _m = err; std::optional<std::string_view> _match_value; bool _m_matched = false; if (!_m_matched && (_m == RpcError::OK)) { _match_value.emplace(std::move(std::string_view("OK"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::NOT_CONNECTED)) { _match_value.emplace(std::move(std::string_view("NOT_CONNECTED"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::CONNECTION_REFUSED)) { _match_value.emplace(std::move(std::string_view("CONNECTION_REFUSED"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::CONNECTION_RESET)) { _match_value.emplace(std::move(std::string_view("CONNECTION_RESET"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::NETWORK_UNREACHABLE)) { _match_value.emplace(std::move(std::string_view("NETWORK_UNREACHABLE"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::HOST_UNREACHABLE)) { _match_value.emplace(std::move(std::string_view("HOST_UNREACHABLE"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::CONNECTION_CLOSED)) { _match_value.emplace(std::move(std::string_view("CONNECTION_CLOSED"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::CIRCUIT_OPEN)) { _match_value.emplace(std::move(std::string_view("CIRCUIT_OPEN"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::INVALID_MESSAGE)) { _match_value.emplace(std::move(std::string_view("INVALID_MESSAGE"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::UNKNOWN_RPC_ID)) { _match_value.emplace(std::move(std::string_view("UNKNOWN_RPC_ID"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::MARSHALLING_ERROR)) { _match_value.emplace(std::move(std::string_view("MARSHALLING_ERROR"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::VERSION_MISMATCH)) { _match_value.emplace(std::move(std::string_view("VERSION_MISMATCH"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::CHECKSUM_ERROR)) { _match_value.emplace(std::move(std::string_view("CHECKSUM_ERROR"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::RPC_FAILED)) { _match_value.emplace(std::move(std::string_view("RPC_FAILED"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::SERVICE_UNAVAILABLE)) { _match_value.emplace(std::move(std::string_view("SERVICE_UNAVAILABLE"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::PERMISSION_DENIED)) { _match_value.emplace(std::move(std::string_view("PERMISSION_DENIED"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::INVALID_ARGUMENT)) { _match_value.emplace(std::move(std::string_view("INVALID_ARGUMENT"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::NOT_FOUND)) { _match_value.emplace(std::move(std::string_view("NOT_FOUND"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::ALREADY_EXISTS)) { _match_value.emplace(std::move(std::string_view("ALREADY_EXISTS"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::CONNECT_TIMEOUT)) { _match_value.emplace(std::move(std::string_view("CONNECT_TIMEOUT"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::REQUEST_TIMEOUT)) { _match_value.emplace(std::move(std::string_view("REQUEST_TIMEOUT"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::RESPONSE_TIMEOUT)) { _match_value.emplace(std::move(std::string_view("RESPONSE_TIMEOUT"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::IDLE_TIMEOUT)) { _match_value.emplace(std::move(std::string_view("IDLE_TIMEOUT"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::HEARTBEAT_TIMEOUT)) { _match_value.emplace(std::move(std::string_view("HEARTBEAT_TIMEOUT"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::UNKNOWN_ERROR)) { _match_value.emplace(std::move(std::string_view("UNKNOWN_ERROR"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::OUT_OF_MEMORY)) { _match_value.emplace(std::move(std::string_view("OUT_OF_MEMORY"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::INVALID_STATE)) { _match_value.emplace(std::move(std::string_view("INVALID_STATE"))); _m_matched = true; } if (!_m_matched && (_m == RpcError::INTERNAL_ERROR)) { _match_value.emplace(std::move(std::string_view("INTERNAL_ERROR"))); _m_matched = true; } if (!_m_matched) { _match_value.emplace(std::move(std::string_view("UNKNOWN"))); _m_matched = true; } if (!_m_matched) { rusty::intrinsics::unreachable_panic(); } std::move(_match_value).value(); });
}
/*RUSTYCPP:GEN-END id=errors.4*/

// `get_error_category` — bucket an `RpcError` into its category by code
// range (NONE = 0, CONNECTION = 100–199, PROTOCOL = 200–299,
// APPLICATION = 300–399, TIMEOUT = 400–499, INTERNAL = otherwise).
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
#if RUSTYCPP_RUST
fn get_error_category(err: RpcError) -> RpcErrorCategory {
    let code: i32 = err as i32;
    if code == 0 { RpcErrorCategory::NONE }
    else if code >= 100 && code < 200 { RpcErrorCategory::CONNECTION }
    else if code >= 200 && code < 300 { RpcErrorCategory::PROTOCOL }
    else if code >= 300 && code < 400 { RpcErrorCategory::APPLICATION }
    else if code >= 400 && code < 500 { RpcErrorCategory::TIMEOUT }
    else { RpcErrorCategory::INTERNAL }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=errors.get_error_category version=1 rust_sha256=71b769d6fee42f5465b681fe532b9cd6124afb9272e918bf615f5cb30fa52a4c*/
RpcErrorCategory get_error_category(RpcError err) {
    const int32_t code = static_cast<int32_t>(err);
    if (rusty::detail::deref_if_pointer_like(code) == static_cast<int32_t>(0)) {
        return rusty::clone(rusty::clone(RpcErrorCategory_NONE()));
    } else if ((rusty::detail::deref_if_pointer_like(code) >= 100) && (rusty::detail::deref_if_pointer_like(code) < 200)) {
        return rusty::clone(rusty::clone(RpcErrorCategory_CONNECTION()));
    } else if ((rusty::detail::deref_if_pointer_like(code) >= 200) && (rusty::detail::deref_if_pointer_like(code) < 300)) {
        return rusty::clone(rusty::clone(RpcErrorCategory_PROTOCOL()));
    } else if ((rusty::detail::deref_if_pointer_like(code) >= 300) && (rusty::detail::deref_if_pointer_like(code) < 400)) {
        return rusty::clone(rusty::clone(RpcErrorCategory_APPLICATION()));
    } else if ((rusty::detail::deref_if_pointer_like(code) >= 400) && (rusty::detail::deref_if_pointer_like(code) < 500)) {
        return rusty::clone(rusty::clone(RpcErrorCategory_TIMEOUT()));
    } else {
        return rusty::clone(rusty::clone(RpcErrorCategory_INTERNAL()));
    }
}
/*RUSTYCPP:GEN-END id=errors.get_error_category*/

// `is_connection_error` / `is_timeout_error` — pure integer-range
// predicates over the `RpcError` code space (CONNECTION = 100–199,
// TIMEOUT = 400–499). Inlined here rather than delegating through
// `get_error_category(err) == RpcErrorCategory::Foo` so the DSL block
// doesn't need to cross-call a sibling free function (the transpiler
// currently prepends `::` to such calls, breaking namespace lookup).
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
#if RUSTYCPP_RUST
fn is_connection_error(err: RpcError) -> bool {
    let code: i32 = err as i32;
    code >= 100 && code < 200
}

fn is_timeout_error(err: RpcError) -> bool {
    let code: i32 = err as i32;
    code >= 400 && code < 500
}
#endif
/*RUSTYCPP:GEN-BEGIN id=errors.category_predicates version=1 rust_sha256=c5eddf66e526cd616e15478c813b595e710de432e6cc505ba0d9bd63c0e4c690*/
bool is_connection_error(RpcError err) {
    const int32_t code = static_cast<int32_t>(err);
    return (rusty::detail::deref_if_pointer_like(code) >= 100) && (rusty::detail::deref_if_pointer_like(code) < 200);
}

bool is_timeout_error(RpcError err) {
    const int32_t code = static_cast<int32_t>(err);
    return (rusty::detail::deref_if_pointer_like(code) >= 400) && (rusty::detail::deref_if_pointer_like(code) < 500);
}
/*RUSTYCPP:GEN-END id=errors.category_predicates*/

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
    if (rusty::detail::deref_if_pointer_like(err) == rusty::clone(RpcError_CONNECTION_RESET())) {
        return true;
    } else if (rusty::detail::deref_if_pointer_like(err) == rusty::clone(RpcError_NETWORK_UNREACHABLE())) {
        return true;
    } else if (rusty::detail::deref_if_pointer_like(err) == rusty::clone(RpcError_HOST_UNREACHABLE())) {
        return true;
    } else if (rusty::detail::deref_if_pointer_like(err) == rusty::clone(RpcError_CONNECT_TIMEOUT())) {
        return true;
    } else if (rusty::detail::deref_if_pointer_like(err) == rusty::clone(RpcError_REQUEST_TIMEOUT())) {
        return true;
    } else if (rusty::detail::deref_if_pointer_like(err) == rusty::clone(RpcError_RESPONSE_TIMEOUT())) {
        return true;
    } else if (rusty::detail::deref_if_pointer_like(err) == rusty::clone(RpcError_SERVICE_UNAVAILABLE())) {
        return true;
    } else {
        return false;
    }
}
/*RUSTYCPP:GEN-END id=errors.is_retryable_error*/

} // export namespace rrr
