//! Transport facade shared by the native Rust runtime and `rrr.channel`.
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
type LegacyStdString = String;
type LegacyCallbackWrapper<F> = rusty::CallbackWrapper<F>;

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
pub trait ChannelConnectionBase {
    /// Send one frame whose payload is described by a raw pointer.
    ///
    /// # Safety
    ///
    /// If `frame.size` is nonzero, `frame.payload` must be non-null, aligned,
    /// and readable for exactly `frame.size` bytes for this synchronous call.
    /// The range must not be concurrently mutated.
    #[allow(unsafe_code)]
    unsafe fn send_frame(&mut self, frame: &self::ChannelFrame) -> self::ChannelError;
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

pub type OnAcceptCallback =
    LegacyCallbackWrapper<Box<dyn Fn(self::ChannelConnectionProxy) + Send + Sync>>;

/// Abstract accept loop implemented by transport listeners.
///
/// # Safety
///
/// `OneTimeJob`'s callable is `Box<dyn FnMut() + Send + Sync>` (see
/// `base/misc.cpp`), and `Server::drop` moves an owning
/// `Box<dyn ChannelListenerBase>` into the poll-thread close job, so the
/// handle itself must be able to cross threads.  `Send + Sync` records that.
/// The trait is `unsafe` because every implementor here reaches C++ backend
/// state (an fd, an in-memory registry) whose thread-safety the Rust type
/// system cannot see: `TcpListener` and `TcpConnection` already carry
/// hand-written `unsafe impl Send`/`unsafe impl Sync` for exactly that
/// reason.  An implementor asserts that its backend is safe to close from,
/// and accept on, a thread other than the one that created it.
///
/// This is the same shape the reactor promotion used for
/// `pub unsafe trait Job: Send + Sync`, and it is the form the emitter's
/// `cpp_import_namespace` leaf contract requires: a leaf trait carrying both
/// `Send` and `Sync` must be declared `unsafe` (a bare `Send` supertrait may
/// stay safe, which is why `PollableBase: Send` does).
#[allow(unsafe_code)]
pub unsafe trait ChannelListenerBase: Send + Sync {
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
