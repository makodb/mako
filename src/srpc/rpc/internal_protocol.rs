// Canonical Rust source for the srpc.internal_protocol module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.
pub const kInternalHeartbeatRpcId: i32 = i32::MIN;
pub const kResponseHeaderExtFlag: u32 = 0x80000000;
pub const kResponseSizeMask: u32 = 0x7fffffff;

pub fn response_has_extended_header(encoded_size: i32) -> bool {
    ((encoded_size as u32) & kResponseHeaderExtFlag) != 0
}

pub fn response_payload_size(encoded_size: i32) -> i32 {
    ((encoded_size as u32) & kResponseSizeMask) as i32
}

pub fn encode_response_size(payload_size: i32, extended_header: bool) -> i32 {
    let base: u32 = (payload_size as u32) & kResponseSizeMask;
    let out: u32 = if extended_header { base | kResponseHeaderExtFlag } else { base };
    out as i32
}
