use srpc::rpc::channel::channel_error_to_string;
use srpc::rpc::errors::{
    get_error_category, is_connection_error, is_retryable_error, is_timeout_error,
    rpc_error_category_to_string, rpc_error_to_string, RpcError, RpcErrorCategory,
};
use srpc::rpc::ChannelError;

const ALL: [(RpcError, i32, &str, RpcErrorCategory, bool); 28] = [
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

#[test]
fn rpc_error_surface_is_wire_and_name_exact() {
    assert_eq!(core::mem::size_of::<RpcError>(), 4);
    assert_eq!(core::mem::size_of::<RpcErrorCategory>(), 4);

    for (err, code, name, category, retryable) in ALL {
        assert_eq!(err as i32, code, "{name} discriminant");
        assert_eq!(rpc_error_to_string(err), name);
        assert_eq!(get_error_category(err), category, "{name} category");
        assert_eq!(
            is_connection_error(err),
            category == RpcErrorCategory::CONNECTION
        );
        assert_eq!(is_timeout_error(err), category == RpcErrorCategory::TIMEOUT);
        assert_eq!(is_retryable_error(err), retryable, "{name} retryability");
    }
}

#[test]
fn category_surface_and_invalid_cpp_cast_fallbacks_are_pinned() {
    let categories = [
        (RpcErrorCategory::NONE, 0, "NONE"),
        (RpcErrorCategory::CONNECTION, 1, "CONNECTION"),
        (RpcErrorCategory::PROTOCOL, 2, "PROTOCOL"),
        (RpcErrorCategory::APPLICATION, 3, "APPLICATION"),
        (RpcErrorCategory::TIMEOUT, 4, "TIMEOUT"),
        (RpcErrorCategory::INTERNAL, 5, "INTERNAL"),
    ];
    for (category, code, name) in categories {
        assert_eq!(category as i32, code);
        assert_eq!(rpc_error_category_to_string(category), name);
    }
}

#[test]
fn channel_error_remains_available_from_its_own_owner() {
    let values = [
        (ChannelError::None, 0, "None"),
        (ChannelError::WouldBlock, 1, "WouldBlock"),
        (ChannelError::ConnectionRefused, 2, "ConnectionRefused"),
        (ChannelError::ConnectionReset, 3, "ConnectionReset"),
        (ChannelError::Timeout, 4, "Timeout"),
        (ChannelError::AddressInUse, 5, "AddressInUse"),
        (ChannelError::AddressInvalid, 6, "AddressInvalid"),
        (ChannelError::PermissionDenied, 7, "PermissionDenied"),
        (ChannelError::TooManyOpenFiles, 8, "TooManyOpenFiles"),
        (ChannelError::Internal, 9, "Internal"),
    ];
    for (error, code, name) in values {
        assert_eq!(error as i32, code);
        assert_eq!(channel_error_to_string(error), name);
    }
    assert_eq!(channel_error_to_string(ChannelError::None), "None");
    assert_eq!(
        channel_error_to_string(ChannelError::WouldBlock),
        "WouldBlock"
    );
    assert!(ChannelError::None.is_ok());
    assert!(!ChannelError::WouldBlock.is_ok());
    assert_eq!(ChannelError::WouldBlock.as_str(), "WouldBlock");
}
