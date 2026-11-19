//! Transport facade shared by the native Rust runtime and `srpc.channel`.
//!
//! The generated C++ contract is intentionally the established channel-mode
//! surface: `rusty::Box` proxies, nullable proxy results, `std::string`
//! address/backend values, and callback wrappers parameterized by
//! `rusty::Function<... const>`.  Those spellings are already consumed by the
//! TCP, in-memory, client, server, and fiber-channel implementations.

#[allow(unused_imports)]
use crate::callback_wrapper as _;

// Native Rust owns a String.  The C++ consumer maps this private alias to
// std::string so trait return types remain byte-for-byte compatible with the
// existing channel implementations.
type LegacyCallbackWrapper<F> = crate::callback_wrapper::detail::CallbackWrapper<F>;

/// Transport error returned by every channel tier.
#[repr(i32)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, PartialEq, Eq))]
pub enum ChannelError {
    None = 0,
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

/// Return the stable spelling used in diagnostics and tests.
pub fn channel_error_to_string(error: self::ChannelError) -> &'static str {
    #[allow(unreachable_patterns)]
    match error {
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
        _ => "Unknown",
    }
}

/// Non-owning view of one complete transport frame.
///
/// The payload remains owned by the sender and is valid for the duration of
/// the callback or `send_frame` call.
#[repr(C)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, PartialEq, Eq))]
pub struct ChannelFrame {
    pub payload: *const u8,
    pub size: usize,
}

/// Callback aliases deliberately carry a boxed Rust trait object.  Transport
/// callbacks cross the user-thread / poll-thread boundary, so captures must be
/// both `Send` and `Sync`. The C++ projection erases these Rust auto-trait
/// bounds and remains the established `rusty::Function<... const>` ABI.
pub type OnFrameCallback =
    LegacyCallbackWrapper<Box<dyn Fn(&self::ChannelFrame) + Send + Sync>>;
pub type OnClosedCallback =
    LegacyCallbackWrapper<Box<dyn Fn(self::ChannelError) + Send + Sync>>;
pub type OnErrorCallback =
    LegacyCallbackWrapper<Box<dyn Fn(self::ChannelError, &str) + Send + Sync>>;

/// Abstract transport connection implemented by TCP and in-memory channels.
pub trait ChannelConnectionBase: Send + Sync {
    /// Send one frame whose payload is described by a raw pointer.
    ///
    /// # Safety
    ///
    /// If `frame.size` is nonzero, `frame.payload` must be non-null, aligned,
    /// and readable for exactly `frame.size` bytes for this synchronous call.
    /// The range must not be concurrently mutated.
    #[allow(unsafe_code)]
    unsafe fn send_frame(&self, frame: &self::ChannelFrame) -> self::ChannelError;
    fn flush(&self);
    fn close(&self);
    fn is_closed(&self) -> bool;
    fn peer_address(&self) -> String;
    /// Apply TCP keepalive settings. Returns false when this transport has no
    /// TCP socket or the OS rejects an option. Non-TCP transports do not use
    /// TCP keepalive and inherit the unsupported result.
    fn set_keepalive(&self, _enabled: bool, _idle_sec: i32, _interval_sec: i32, _count: i32) -> bool {
        false
    }
    fn set_on_frame(&mut self, callback: self::OnFrameCallback);
    fn set_on_closed(&mut self, callback: self::OnClosedCallback);
    fn set_on_error(&mut self, callback: self::OnErrorCallback);
}

/// Owned, non-nullable connection handle.
pub type ChannelConnectionProxy = Box<dyn ChannelConnectionBase>;

/// A connection argument that may be absent at a transport boundary.
pub type NullableChannelConnectionProxy = Option<ChannelConnectionProxy>;

pub type OnAcceptCallback =
    LegacyCallbackWrapper<Box<dyn Fn(self::NullableChannelConnectionProxy) + Send + Sync>>;

/// Abstract accept loop implemented by transport listeners.
///
/// # Safety
///
/// Server teardown transfers listener ownership to a poll-thread job.
/// Implementors must keep listener storage and registered callbacks valid
/// throughout polling and close. Rust checks the `Send + Sync` bounds on
/// their shared state; canonical TCP and in-memory listeners use synchronized
/// ownership for callbacks and native resources.
#[allow(unsafe_code)]
pub unsafe trait ChannelListenerBase: Send + Sync {
    fn listen(&mut self, address: &str) -> self::ChannelError;
    fn close(&mut self);
    fn is_closed(&self) -> bool;
    fn local_address(&self) -> String;
    fn set_on_accept(&mut self, callback: self::OnAcceptCallback);
    fn set_on_error(&mut self, callback: self::OnErrorCallback);
}

/// Owned, non-nullable listener handle.
pub type ChannelListenerProxy = Box<dyn ChannelListenerBase>;

/// Result of a transport connection attempt.
#[repr(C)]
pub struct ConnectResult {
    pub connection: Option<self::ChannelConnectionProxy>,
    pub error: self::ChannelError,
}

/// Factory for transport connections and listeners.
pub trait ChannelFactoryBase: Send {
    fn connect(&mut self, address: &str) -> self::ConnectResult;
    fn make_listener(&mut self) -> Option<self::ChannelListenerProxy>;
    fn backend_name(&self) -> String;
}

/// Owned, non-nullable factory handle.
pub type ChannelFactoryProxy = Box<dyn ChannelFactoryBase>;

/// A factory argument that may be absent at a client or server boundary.
pub type NullableChannelFactoryProxy = Option<ChannelFactoryProxy>;
