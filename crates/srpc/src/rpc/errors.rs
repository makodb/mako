//! RPC error codes shared by Rust and the legacy `rrr.errors` C++ module.
//!
//! The C++ names are intentional. This file is transpiled as a whole, so the
//! Rust declarations below must preserve the legacy enum names, variant names,
//! and free-function surface exactly. `RpcError` discriminants are wire-visible
//! and may not be renumbered.

/// Coarse classification of an [`RpcError`].
#[allow(non_camel_case_types)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(i32)]
pub enum RpcErrorCategory {
    NONE = 0,
    CONNECTION = 1,
    PROTOCOL = 2,
    APPLICATION = 3,
    TIMEOUT = 4,
    INTERNAL = 5,
}

/// An RPC failure code. `OK` (0) means success.
#[allow(non_camel_case_types)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(i32)]
pub enum RpcError {
    OK = 0,

    // 100s — connection faults.
    NOT_CONNECTED = 100,
    CONNECTION_REFUSED = 101,
    CONNECTION_RESET = 102,
    NETWORK_UNREACHABLE = 103,
    HOST_UNREACHABLE = 104,
    CONNECTION_CLOSED = 105,
    CIRCUIT_OPEN = 106,

    // 200s — protocol/wire faults.
    INVALID_MESSAGE = 200,
    UNKNOWN_RPC_ID = 201,
    MARSHALLING_ERROR = 202,
    VERSION_MISMATCH = 203,
    CHECKSUM_ERROR = 204,

    // 300s — application-level faults.
    RPC_FAILED = 300,
    SERVICE_UNAVAILABLE = 301,
    PERMISSION_DENIED = 302,
    INVALID_ARGUMENT = 303,
    NOT_FOUND = 304,
    ALREADY_EXISTS = 305,

    // 400s — timeouts.
    CONNECT_TIMEOUT = 400,
    REQUEST_TIMEOUT = 401,
    RESPONSE_TIMEOUT = 402,
    IDLE_TIMEOUT = 403,
    HEARTBEAT_TIMEOUT = 404,

    // 500s — internal faults.
    UNKNOWN_ERROR = 500,
    OUT_OF_MEMORY = 501,
    INVALID_STATE = 502,
    INTERNAL_ERROR = 503,
}

/// Integer-level helper used to test the behavior of invalid C++ enum casts.
///
/// Safe Rust cannot construct an invalid enum discriminant. Keeping the
/// fallback in this integer helper makes the generated C++ behavior explicit
/// and lets Rust tests pin it without invoking undefined behavior.
#[doc(hidden)]
fn rpc_error_category_string_for_code(code: i32) -> &'static str {
    match code {
        0 => "NONE",
        1 => "CONNECTION",
        2 => "PROTOCOL",
        3 => "APPLICATION",
        4 => "TIMEOUT",
        5 => "INTERNAL",
        _ => "UNKNOWN",
    }
}

/// Integer-level helper for the legacy error-name table.
#[doc(hidden)]
fn rpc_error_string_for_code(code: i32) -> &'static str {
    match code {
        0 => "OK",
        100 => "NOT_CONNECTED",
        101 => "CONNECTION_REFUSED",
        102 => "CONNECTION_RESET",
        103 => "NETWORK_UNREACHABLE",
        104 => "HOST_UNREACHABLE",
        105 => "CONNECTION_CLOSED",
        106 => "CIRCUIT_OPEN",
        200 => "INVALID_MESSAGE",
        201 => "UNKNOWN_RPC_ID",
        202 => "MARSHALLING_ERROR",
        203 => "VERSION_MISMATCH",
        204 => "CHECKSUM_ERROR",
        300 => "RPC_FAILED",
        301 => "SERVICE_UNAVAILABLE",
        302 => "PERMISSION_DENIED",
        303 => "INVALID_ARGUMENT",
        304 => "NOT_FOUND",
        305 => "ALREADY_EXISTS",
        400 => "CONNECT_TIMEOUT",
        401 => "REQUEST_TIMEOUT",
        402 => "RESPONSE_TIMEOUT",
        403 => "IDLE_TIMEOUT",
        404 => "HEARTBEAT_TIMEOUT",
        500 => "UNKNOWN_ERROR",
        501 => "OUT_OF_MEMORY",
        502 => "INVALID_STATE",
        503 => "INTERNAL_ERROR",
        _ => "UNKNOWN",
    }
}

/// Integer-level category classifier. Unknown values retain the legacy
/// `INTERNAL` fallback.
#[doc(hidden)]
fn rpc_error_category_for_code(code: i32) -> RpcErrorCategory {
    if code == 0 {
        RpcErrorCategory::NONE
    } else if code >= 100 && code < 200 {
        RpcErrorCategory::CONNECTION
    } else if code >= 200 && code < 300 {
        RpcErrorCategory::PROTOCOL
    } else if code >= 300 && code < 400 {
        RpcErrorCategory::APPLICATION
    } else if code >= 400 && code < 500 {
        RpcErrorCategory::TIMEOUT
    } else {
        RpcErrorCategory::INTERNAL
    }
}

/// Return the legacy spelling of an error category.
pub fn rpc_error_category_to_string(cat: self::RpcErrorCategory) -> &'static str {
    rpc_error_category_string_for_code(cat as i32)
}

/// Return the legacy spelling of an RPC error.
pub fn rpc_error_to_string(err: self::RpcError) -> &'static str {
    rpc_error_string_for_code(err as i32)
}

/// Classify an RPC error by its numeric band.
pub fn get_error_category(err: self::RpcError) -> RpcErrorCategory {
    rpc_error_category_for_code(err as i32)
}

/// Whether an error belongs to the connection-error band.
pub fn is_connection_error(err: self::RpcError) -> bool {
    let code = err as i32;
    code >= 100 && code < 200
}

/// Whether an error belongs to the timeout-error band.
pub fn is_timeout_error(err: self::RpcError) -> bool {
    let code = err as i32;
    code >= 400 && code < 500
}

/// Whether a request may be retried after this error.
pub fn is_retryable_error(err: self::RpcError) -> bool {
    if err == RpcError::CONNECTION_RESET {
        true
    } else if err == RpcError::NETWORK_UNREACHABLE {
        true
    } else if err == RpcError::HOST_UNREACHABLE {
        true
    } else if err == RpcError::CONNECT_TIMEOUT {
        true
    } else if err == RpcError::REQUEST_TIMEOUT {
        true
    } else if err == RpcError::RESPONSE_TIMEOUT {
        true
    } else {
        err == RpcError::SERVICE_UNAVAILABLE
    }
}
