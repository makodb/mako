//! Transport facade shared by the native Rust runtime and `rrr.channel`.
//!
//! The generated C++ contract is intentionally the established channel-mode
//! surface: `rusty::Box` proxies, nullable proxy results, `std::string`
//! address/backend values, and callback wrappers parameterized by
//! `rusty::Function<... const>`.  Those spellings are already consumed by the
//! TCP, in-memory, client, server, and fiber-channel implementations.

use crate::base::callback_wrapper::detail::CallbackWrapper;

// Native Rust owns a String.  The C++ consumer maps this private alias to
// std::string so trait return types remain byte-for-byte compatible with the
// existing channel implementations.
type LegacyStdString = String;

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

/// Callback aliases deliberately carry a boxed Rust trait object.  The C++
/// projection flattens `Box<dyn Fn>` to `rusty::Function<... const>`, which is
/// the callable type expected by the generated `CallbackWrapper<F>` owner.
pub type OnFrameCallback = CallbackWrapper<Box<dyn Fn(&self::ChannelFrame)>>;
pub type OnClosedCallback = CallbackWrapper<Box<dyn Fn(self::ChannelError)>>;
pub type OnErrorCallback = CallbackWrapper<Box<dyn Fn(self::ChannelError, &str)>>;

/// Abstract transport connection implemented by TCP and in-memory channels.
pub trait ChannelConnectionBase {
    fn send_frame(&mut self, frame: &self::ChannelFrame) -> self::ChannelError;
    fn flush(&mut self);
    fn close(&mut self);
    fn is_closed(&self) -> bool;
    fn peer_address(&self) -> LegacyStdString;
    fn set_on_frame(&mut self, callback: self::OnFrameCallback);
    fn set_on_closed(&mut self, callback: self::OnClosedCallback);
    fn set_on_error(&mut self, callback: self::OnErrorCallback);
}

/// Owned, non-nullable connection handle.
pub type ChannelConnectionProxy = Box<dyn ChannelConnectionBase>;

pub type OnAcceptCallback = CallbackWrapper<Box<dyn Fn(self::ChannelConnectionProxy)>>;

/// Abstract accept loop implemented by transport listeners.
pub trait ChannelListenerBase {
    fn listen(&mut self, address: &str) -> self::ChannelError;
    fn close(&mut self);
    fn is_closed(&self) -> bool;
    fn local_address(&self) -> LegacyStdString;
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
pub trait ChannelFactoryBase {
    fn connect(&mut self, address: &str) -> self::ConnectResult;
    fn make_listener(&mut self) -> Option<self::ChannelListenerProxy>;
    fn backend_name(&self) -> LegacyStdString;
}

/// Owned, non-nullable factory handle.
pub type ChannelFactoryProxy = Box<dyn ChannelFactoryBase>;
