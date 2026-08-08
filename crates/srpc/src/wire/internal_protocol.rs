//! Internal RPC protocol constants and response-size encoding.
//!
//! The high bit of an encoded response size marks an extended response
//! header. The remaining 31 bits hold the payload size. These names are kept
//! identical to the legacy `rrr.internal_protocol` module because this file is
//! also transpiled to that C++ module.

#![allow(non_upper_case_globals)]

/// Reserved RPC id used for the internal heartbeat request.
pub const kInternalHeartbeatRpcId: i32 = i32::MIN;

/// High-bit flag indicating that a response has an extended header.
pub const kResponseHeaderExtFlag: u32 = 0x8000_0000;

/// Mask selecting the payload-size portion of an encoded response size.
pub const kResponseSizeMask: u32 = 0x7fff_ffff;

/// Whether `encoded_size` carries the extended-header flag.
pub fn response_has_extended_header(encoded_size: i32) -> bool {
    ((encoded_size as u32) & kResponseHeaderExtFlag) != 0
}

/// Extract the low 31-bit payload size from an encoded response size.
pub fn response_payload_size(encoded_size: i32) -> i32 {
    ((encoded_size as u32) & kResponseSizeMask) as i32
}

/// Encode a response payload size and optional extended-header flag.
pub fn encode_response_size(payload_size: i32, extended_header: bool) -> i32 {
    let base: u32 = (payload_size as u32) & kResponseSizeMask;
    let out: u32 = if extended_header {
        base | kResponseHeaderExtFlag
    } else {
        base
    };
    out as i32
}
