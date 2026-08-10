//! RPC error codes — the port of `src/rrr/rpc/errors.cpp`.
//!
//! **The discriminants are wire-visible.** A client and a server may be
//! running different builds, or different languages during the strangle,
//! and they exchange these as raw `i32`. Every value is pinned by
//! [`tests::discriminants_are_wire_stable`]; changing one is a protocol
//! break, not a refactor.
//!
//! Codes are banded by category, and the classification helpers read the
//! band rather than listing members, so a new code lands in the right
//! category by construction:
//!
//! | band | category |
//! |------|----------|
//! | 0    | [`Category::None`] |
//! | 100s | [`Category::Connection`] |
//! | 200s | [`Category::Protocol`] |
//! | 300s | [`Category::Application`] |
//! | 400s | [`Category::Timeout`] |
//! | 500s | [`Category::Internal`] |

use std::fmt;

/// Coarse classification of an [`RpcError`].
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(i32)]
pub enum Category {
    None = 0,
    Connection = 1,
    Protocol = 2,
    Application = 3,
    Timeout = 4,
    Internal = 5,
}

impl Category {
    pub fn as_str(self) -> &'static str {
        match self {
            Category::None => "NONE",
            Category::Connection => "CONNECTION",
            Category::Protocol => "PROTOCOL",
            Category::Application => "APPLICATION",
            Category::Timeout => "TIMEOUT",
            Category::Internal => "INTERNAL",
        }
    }
}

impl fmt::Display for Category {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

/// An RPC failure code. `Ok` (0) means success.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(i32)]
pub enum RpcError {
    Ok = 0,

    // 100s — connection faults.
    NotConnected = 100,
    ConnectionRefused = 101,
    ConnectionReset = 102,
    NetworkUnreachable = 103,
    HostUnreachable = 104,
    ConnectionClosed = 105,
    CircuitOpen = 106,

    // 200s — protocol/wire faults.
    InvalidMessage = 200,
    UnknownRpcId = 201,
    MarshallingError = 202,
    VersionMismatch = 203,
    ChecksumError = 204,

    // 300s — application-level faults.
    RpcFailed = 300,
    ServiceUnavailable = 301,
    PermissionDenied = 302,
    InvalidArgument = 303,
    NotFound = 304,
    AlreadyExists = 305,

    // 400s — timeouts.
    ConnectTimeout = 400,
    RequestTimeout = 401,
    ResponseTimeout = 402,
    IdleTimeout = 403,
    HeartbeatTimeout = 404,

    // 500s — internal faults.
    UnknownError = 500,
    OutOfMemory = 501,
    InvalidState = 502,
    InternalError = 503,
}

impl RpcError {
    pub fn code(self) -> i32 {
        self as i32
    }

    /// Decode a code received off the wire. Unrecognized codes are
    /// `None` rather than silently becoming `UnknownError`: a peer
    /// sending a code this build does not know is worth distinguishing
    /// from one that deliberately said "unknown".
    pub fn from_code(code: i32) -> Option<RpcError> {
        let e = match code {
            0 => RpcError::Ok,
            100 => RpcError::NotConnected,
            101 => RpcError::ConnectionRefused,
            102 => RpcError::ConnectionReset,
            103 => RpcError::NetworkUnreachable,
            104 => RpcError::HostUnreachable,
            105 => RpcError::ConnectionClosed,
            106 => RpcError::CircuitOpen,
            200 => RpcError::InvalidMessage,
            201 => RpcError::UnknownRpcId,
            202 => RpcError::MarshallingError,
            203 => RpcError::VersionMismatch,
            204 => RpcError::ChecksumError,
            300 => RpcError::RpcFailed,
            301 => RpcError::ServiceUnavailable,
            302 => RpcError::PermissionDenied,
            303 => RpcError::InvalidArgument,
            304 => RpcError::NotFound,
            305 => RpcError::AlreadyExists,
            400 => RpcError::ConnectTimeout,
            401 => RpcError::RequestTimeout,
            402 => RpcError::ResponseTimeout,
            403 => RpcError::IdleTimeout,
            404 => RpcError::HeartbeatTimeout,
            500 => RpcError::UnknownError,
            501 => RpcError::OutOfMemory,
            502 => RpcError::InvalidState,
            503 => RpcError::InternalError,
            _ => return None,
        };
        Some(e)
    }

    /// Category from the code's band — a new code is classified by
    /// construction rather than by remembering to extend a list.
    pub fn category(self) -> Category {
        let code = self.code();
        if code == 0 {
            Category::None
        } else if (100..200).contains(&code) {
            Category::Connection
        } else if (200..300).contains(&code) {
            Category::Protocol
        } else if (300..400).contains(&code) {
            Category::Application
        } else if (400..500).contains(&code) {
            Category::Timeout
        } else {
            Category::Internal
        }
    }

    pub fn is_ok(self) -> bool {
        self == RpcError::Ok
    }

    pub fn is_connection_error(self) -> bool {
        self.category() == Category::Connection
    }

    pub fn is_timeout_error(self) -> bool {
        self.category() == Category::Timeout
    }

    /// Whether a client should retry. Deliberately NOT "every transient
    /// category": `NotConnected`/`ConnectionRefused` mean the peer is
    /// not there yet (reconnect, do not resend), `IdleTimeout` and
    /// `HeartbeatTimeout` are connection lifecycle rather than a failed
    /// call, and `CircuitOpen` is the breaker's whole point.
    pub fn is_retryable(self) -> bool {
        matches!(
            self,
            RpcError::ConnectionReset
                | RpcError::NetworkUnreachable
                | RpcError::HostUnreachable
                | RpcError::ConnectTimeout
                | RpcError::RequestTimeout
                | RpcError::ResponseTimeout
                | RpcError::ServiceUnavailable
        )
    }

    pub fn as_str(self) -> &'static str {
        match self {
            RpcError::Ok => "OK",
            RpcError::NotConnected => "NOT_CONNECTED",
            RpcError::ConnectionRefused => "CONNECTION_REFUSED",
            RpcError::ConnectionReset => "CONNECTION_RESET",
            RpcError::NetworkUnreachable => "NETWORK_UNREACHABLE",
            RpcError::HostUnreachable => "HOST_UNREACHABLE",
            RpcError::ConnectionClosed => "CONNECTION_CLOSED",
            RpcError::CircuitOpen => "CIRCUIT_OPEN",
            RpcError::InvalidMessage => "INVALID_MESSAGE",
            RpcError::UnknownRpcId => "UNKNOWN_RPC_ID",
            RpcError::MarshallingError => "MARSHALLING_ERROR",
            RpcError::VersionMismatch => "VERSION_MISMATCH",
            RpcError::ChecksumError => "CHECKSUM_ERROR",
            RpcError::RpcFailed => "RPC_FAILED",
            RpcError::ServiceUnavailable => "SERVICE_UNAVAILABLE",
            RpcError::PermissionDenied => "PERMISSION_DENIED",
            RpcError::InvalidArgument => "INVALID_ARGUMENT",
            RpcError::NotFound => "NOT_FOUND",
            RpcError::AlreadyExists => "ALREADY_EXISTS",
            RpcError::ConnectTimeout => "CONNECT_TIMEOUT",
            RpcError::RequestTimeout => "REQUEST_TIMEOUT",
            RpcError::ResponseTimeout => "RESPONSE_TIMEOUT",
            RpcError::IdleTimeout => "IDLE_TIMEOUT",
            RpcError::HeartbeatTimeout => "HEARTBEAT_TIMEOUT",
            RpcError::UnknownError => "UNKNOWN_ERROR",
            RpcError::OutOfMemory => "OUT_OF_MEMORY",
            RpcError::InvalidState => "INVALID_STATE",
            RpcError::InternalError => "INTERNAL_ERROR",
        }
    }
}

impl fmt::Display for RpcError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Every code this build knows, paired with the integer that goes
    /// on the wire.
    const ALL: [(RpcError, i32); 28] = [
        (RpcError::Ok, 0),
        (RpcError::NotConnected, 100),
        (RpcError::ConnectionRefused, 101),
        (RpcError::ConnectionReset, 102),
        (RpcError::NetworkUnreachable, 103),
        (RpcError::HostUnreachable, 104),
        (RpcError::ConnectionClosed, 105),
        (RpcError::CircuitOpen, 106),
        (RpcError::InvalidMessage, 200),
        (RpcError::UnknownRpcId, 201),
        (RpcError::MarshallingError, 202),
        (RpcError::VersionMismatch, 203),
        (RpcError::ChecksumError, 204),
        (RpcError::RpcFailed, 300),
        (RpcError::ServiceUnavailable, 301),
        (RpcError::PermissionDenied, 302),
        (RpcError::InvalidArgument, 303),
        (RpcError::NotFound, 304),
        (RpcError::AlreadyExists, 305),
        (RpcError::ConnectTimeout, 400),
        (RpcError::RequestTimeout, 401),
        (RpcError::ResponseTimeout, 402),
        (RpcError::IdleTimeout, 403),
        (RpcError::HeartbeatTimeout, 404),
        (RpcError::UnknownError, 500),
        (RpcError::OutOfMemory, 501),
        (RpcError::InvalidState, 502),
        (RpcError::InternalError, 503),
    ];

    /// These integers travel between peers that may be different builds
    /// or different languages. Changing one is a protocol break.
    #[test]
    fn discriminants_are_wire_stable() {
        let mut i = 0;
        while i < ALL.len() {
            let (err, code) = ALL[i];
            assert_eq!(err.code(), code, "{err} must stay {code}");
            assert_eq!(RpcError::from_code(code), Some(err), "decode {code}");
            i += 1;
        }
    }

    #[test]
    fn category_bands() {
        assert_eq!(RpcError::Ok.category(), Category::None);
        assert_eq!(RpcError::CircuitOpen.category(), Category::Connection);
        assert_eq!(RpcError::ChecksumError.category(), Category::Protocol);
        assert_eq!(RpcError::AlreadyExists.category(), Category::Application);
        assert_eq!(RpcError::HeartbeatTimeout.category(), Category::Timeout);
        assert_eq!(RpcError::InternalError.category(), Category::Internal);

        // Every known code lands in the band its number implies.
        let mut i = 0;
        while i < ALL.len() {
            let (err, code) = ALL[i];
            let expected = match code {
                0 => Category::None,
                100..=199 => Category::Connection,
                200..=299 => Category::Protocol,
                300..=399 => Category::Application,
                400..=499 => Category::Timeout,
                _ => Category::Internal,
            };
            assert_eq!(err.category(), expected, "{err} band");
            i += 1;
        }
    }

    #[test]
    fn predicates_agree_with_categories() {
        assert!(RpcError::ConnectionReset.is_connection_error());
        assert!(!RpcError::ConnectionReset.is_timeout_error());
        assert!(RpcError::IdleTimeout.is_timeout_error());
        assert!(!RpcError::IdleTimeout.is_connection_error());
        assert!(RpcError::Ok.is_ok());
        assert!(!RpcError::RpcFailed.is_ok());
    }

    #[test]
    fn retryable_set_is_exactly_the_transient_faults() {
        let retryable = [
            RpcError::ConnectionReset,
            RpcError::NetworkUnreachable,
            RpcError::HostUnreachable,
            RpcError::ConnectTimeout,
            RpcError::RequestTimeout,
            RpcError::ResponseTimeout,
            RpcError::ServiceUnavailable,
        ];
        let mut i = 0;
        while i < ALL.len() {
            let (err, _) = ALL[i];
            let want = retryable.contains(&err);
            assert_eq!(err.is_retryable(), want, "{err} retryable");
            i += 1;
        }
        // Guard the judgement calls specifically.
        assert!(
            !RpcError::NotConnected.is_retryable(),
            "reconnect, not resend"
        );
        assert!(!RpcError::CircuitOpen.is_retryable(), "the breaker's point");
        assert!(
            !RpcError::IdleTimeout.is_retryable(),
            "lifecycle, not a call"
        );
        assert!(!RpcError::InvalidArgument.is_retryable(), "will fail again");
    }

    #[test]
    fn unknown_codes_decode_to_none() {
        assert_eq!(RpcError::from_code(1), None);
        assert_eq!(RpcError::from_code(107), None);
        assert_eq!(RpcError::from_code(-1), None);
        assert_eq!(RpcError::from_code(504), None);
    }

    #[test]
    fn names_round_trip_and_are_unique() {
        assert_eq!(RpcError::ConnectionReset.to_string(), "CONNECTION_RESET");
        assert_eq!(Category::Timeout.to_string(), "TIMEOUT");
        let mut names: Vec<&str> = Vec::new();
        let mut i = 0;
        while i < ALL.len() {
            names.push(ALL[i].0.as_str());
            i += 1;
        }
        names.sort_unstable();
        let before = names.len();
        names.dedup();
        assert_eq!(names.len(), before, "names must be unique");
    }
}

/// Transport-level failure, distinct from [`RpcError`].
///
/// `RpcError` travels on the wire in a reply's error field; this never
/// does. It says what the *channel* did — so its discriminants are free
/// to be a local convention, unlike `RpcError`'s, which are pinned by
/// the C++ peer. The values still match the C++ `ChannelError` so the
/// two enums stay readable side by side.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(i32)]
pub enum ChannelError {
    /// Not an error: the operation completed, or the peer closed
    /// cleanly. Named as the C++ names it.
    None = 0,
    /// Bytes remain queued; the poll thread owns the rest.
    WouldBlock = 1,
    ConnectionRefused = 2,
    ConnectionReset = 3,
    Timeout = 4,
    AddressInUse = 5,
    AddressInvalid = 6,
    PermissionDenied = 7,
    TooManyOpenFiles = 8,
    Internal = 9,
}

impl ChannelError {
    pub fn is_ok(self) -> bool {
        self == ChannelError::None
    }

    pub fn as_str(self) -> &'static str {
        match self {
            ChannelError::None => "None",
            ChannelError::WouldBlock => "WouldBlock",
            ChannelError::ConnectionRefused => "ConnectionRefused",
            ChannelError::ConnectionReset => "ConnectionReset",
            ChannelError::Timeout => "Timeout",
            ChannelError::AddressInUse => "AddressInUse",
            ChannelError::AddressInvalid => "AddressInvalid",
            ChannelError::PermissionDenied => "PermissionDenied",
            ChannelError::TooManyOpenFiles => "TooManyOpenFiles",
            ChannelError::Internal => "Internal",
        }
    }
}

#[cfg(test)]
mod channel_error_tests {
    use super::*;

    #[test]
    fn discriminants_match_the_cpp_enum() {
        // Not wire-visible, but kept aligned so the two enums read the
        // same way side by side.
        assert_eq!(ChannelError::None as i32, 0);
        assert_eq!(ChannelError::WouldBlock as i32, 1);
        assert_eq!(ChannelError::ConnectionRefused as i32, 2);
        assert_eq!(ChannelError::ConnectionReset as i32, 3);
        assert_eq!(ChannelError::Timeout as i32, 4);
        assert_eq!(ChannelError::AddressInUse as i32, 5);
        assert_eq!(ChannelError::AddressInvalid as i32, 6);
        assert_eq!(ChannelError::PermissionDenied as i32, 7);
        assert_eq!(ChannelError::TooManyOpenFiles as i32, 8);
        assert_eq!(ChannelError::Internal as i32, 9);
        assert!(ChannelError::None.is_ok());
        assert!(!ChannelError::WouldBlock.is_ok());
        assert_eq!(ChannelError::Timeout.as_str(), "Timeout");
    }
}
