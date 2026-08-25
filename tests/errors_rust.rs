use srpc::errors::{
    get_error_category, is_connection_error, is_retryable_error, is_timeout_error,
    rpc_error_category_to_string, rpc_error_to_string, RpcError, RpcErrorCategory,
};

#[test]
fn discriminants_and_names_preserve_the_public_error_contract() {
    let categories = [
        (RpcErrorCategory::NONE, 0, "NONE"),
        (RpcErrorCategory::CONNECTION, 1, "CONNECTION"),
        (RpcErrorCategory::PROTOCOL, 2, "PROTOCOL"),
        (RpcErrorCategory::APPLICATION, 3, "APPLICATION"),
        (RpcErrorCategory::TIMEOUT, 4, "TIMEOUT"),
        (RpcErrorCategory::INTERNAL, 5, "INTERNAL"),
    ];
    for (category, discriminant, name) in categories {
        assert_eq!(category as i32, discriminant);
        assert_eq!(rpc_error_category_to_string(category), name);
    }
}

#[test]
fn categories_and_retry_predicates_match_the_numeric_ranges() {
    let rows = [
        (RpcError::OK, 0, "OK", RpcErrorCategory::NONE, false),
        (
            RpcError::NOT_CONNECTED,
            100,
            "NOT_CONNECTED",
            RpcErrorCategory::CONNECTION,
            false,
        ),
        (
            RpcError::CONNECTION_REFUSED,
            101,
            "CONNECTION_REFUSED",
            RpcErrorCategory::CONNECTION,
            false,
        ),
        (
            RpcError::CONNECTION_RESET,
            102,
            "CONNECTION_RESET",
            RpcErrorCategory::CONNECTION,
            true,
        ),
        (
            RpcError::NETWORK_UNREACHABLE,
            103,
            "NETWORK_UNREACHABLE",
            RpcErrorCategory::CONNECTION,
            true,
        ),
        (
            RpcError::HOST_UNREACHABLE,
            104,
            "HOST_UNREACHABLE",
            RpcErrorCategory::CONNECTION,
            true,
        ),
        (
            RpcError::CONNECTION_CLOSED,
            105,
            "CONNECTION_CLOSED",
            RpcErrorCategory::CONNECTION,
            false,
        ),
        (
            RpcError::CIRCUIT_OPEN,
            106,
            "CIRCUIT_OPEN",
            RpcErrorCategory::CONNECTION,
            false,
        ),
        (
            RpcError::INVALID_MESSAGE,
            200,
            "INVALID_MESSAGE",
            RpcErrorCategory::PROTOCOL,
            false,
        ),
        (
            RpcError::UNKNOWN_RPC_ID,
            201,
            "UNKNOWN_RPC_ID",
            RpcErrorCategory::PROTOCOL,
            false,
        ),
        (
            RpcError::MARSHALLING_ERROR,
            202,
            "MARSHALLING_ERROR",
            RpcErrorCategory::PROTOCOL,
            false,
        ),
        (
            RpcError::VERSION_MISMATCH,
            203,
            "VERSION_MISMATCH",
            RpcErrorCategory::PROTOCOL,
            false,
        ),
        (
            RpcError::CHECKSUM_ERROR,
            204,
            "CHECKSUM_ERROR",
            RpcErrorCategory::PROTOCOL,
            false,
        ),
        (
            RpcError::RPC_FAILED,
            300,
            "RPC_FAILED",
            RpcErrorCategory::APPLICATION,
            false,
        ),
        (
            RpcError::SERVICE_UNAVAILABLE,
            301,
            "SERVICE_UNAVAILABLE",
            RpcErrorCategory::APPLICATION,
            true,
        ),
        (
            RpcError::PERMISSION_DENIED,
            302,
            "PERMISSION_DENIED",
            RpcErrorCategory::APPLICATION,
            false,
        ),
        (
            RpcError::INVALID_ARGUMENT,
            303,
            "INVALID_ARGUMENT",
            RpcErrorCategory::APPLICATION,
            false,
        ),
        (
            RpcError::NOT_FOUND,
            304,
            "NOT_FOUND",
            RpcErrorCategory::APPLICATION,
            false,
        ),
        (
            RpcError::ALREADY_EXISTS,
            305,
            "ALREADY_EXISTS",
            RpcErrorCategory::APPLICATION,
            false,
        ),
        (
            RpcError::CONNECT_TIMEOUT,
            400,
            "CONNECT_TIMEOUT",
            RpcErrorCategory::TIMEOUT,
            true,
        ),
        (
            RpcError::REQUEST_TIMEOUT,
            401,
            "REQUEST_TIMEOUT",
            RpcErrorCategory::TIMEOUT,
            true,
        ),
        (
            RpcError::RESPONSE_TIMEOUT,
            402,
            "RESPONSE_TIMEOUT",
            RpcErrorCategory::TIMEOUT,
            true,
        ),
        (
            RpcError::IDLE_TIMEOUT,
            403,
            "IDLE_TIMEOUT",
            RpcErrorCategory::TIMEOUT,
            false,
        ),
        (
            RpcError::HEARTBEAT_TIMEOUT,
            404,
            "HEARTBEAT_TIMEOUT",
            RpcErrorCategory::TIMEOUT,
            false,
        ),
        (
            RpcError::UNKNOWN_ERROR,
            500,
            "UNKNOWN_ERROR",
            RpcErrorCategory::INTERNAL,
            false,
        ),
        (
            RpcError::OUT_OF_MEMORY,
            501,
            "OUT_OF_MEMORY",
            RpcErrorCategory::INTERNAL,
            false,
        ),
        (
            RpcError::INVALID_STATE,
            502,
            "INVALID_STATE",
            RpcErrorCategory::INTERNAL,
            false,
        ),
        (
            RpcError::INTERNAL_ERROR,
            503,
            "INTERNAL_ERROR",
            RpcErrorCategory::INTERNAL,
            false,
        ),
    ];

    for (error, discriminant, name, category, retryable) in rows {
        assert_eq!(error as i32, discriminant);
        assert_eq!(rpc_error_to_string(error), name);
        assert_eq!(get_error_category(error) as i32, category as i32);
        assert_eq!(
            is_connection_error(error),
            category == RpcErrorCategory::CONNECTION
        );
        assert_eq!(
            is_timeout_error(error),
            category == RpcErrorCategory::TIMEOUT
        );
        assert_eq!(is_retryable_error(error), retryable);
    }
}
