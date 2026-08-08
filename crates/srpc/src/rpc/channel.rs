//! Transport-level errors shared by the Rust channel implementations.
//!
//! This type belongs to the future `rrr.channel` generated module, not to
//! `rrr.errors`; keeping it in its own Rust owner prevents duplicate C++
//! exports when both legacy modules are generated from the crate.

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(i32)]
pub enum ChannelError {
    /// Not an error: the operation completed, or the peer closed cleanly.
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
