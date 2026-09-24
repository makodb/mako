// Canonical Rust source for the srpc.internal_protocol module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.
//
// Each function carries a `#[cfg(verus)]`-gated Verus contract that pins its
// result to the exact wire-bit expression it computes. These are definitional,
// so they need no in-body proof (which the C++ transpiler could not carry) and
// they stay invisible to plain rustc and rusty-cpp. The interesting theorems --
// that decoded sizes are in range, and that encode/decode round-trips -- are
// proven from these contracts in verify/src/internal_protocol_proofs.rs, which
// runs only under `cargo verus verify`. See docs/verification.md.
#[cfg(verus)]
use vstd::prelude::*;

pub const kInternalHeartbeatRpcId: i32 = i32::MIN;
#[cfg_attr(verus, verus_verify)]
pub const kResponseHeaderExtFlag: u32 = 0x80000000;
#[cfg_attr(verus, verus_verify)]
pub const kResponseSizeMask: u32 = 0x7fffffff;

#[cfg_attr(verus, verus_spec(r =>
    ensures r == (((encoded_size as u32) & kResponseHeaderExtFlag) != 0),
))]
pub fn response_has_extended_header(encoded_size: i32) -> bool {
    ((encoded_size as u32) & kResponseHeaderExtFlag) != 0
}

#[cfg_attr(verus, verus_spec(r =>
    ensures r == (((encoded_size as u32) & kResponseSizeMask) as i32),
))]
pub fn response_payload_size(encoded_size: i32) -> i32 {
    ((encoded_size as u32) & kResponseSizeMask) as i32
}

#[cfg_attr(verus, verus_spec(r =>
    ensures r == ((if extended_header {
        ((payload_size as u32) & kResponseSizeMask) | kResponseHeaderExtFlag
    } else {
        (payload_size as u32) & kResponseSizeMask
    }) as i32),
))]
pub fn encode_response_size(payload_size: i32, extended_header: bool) -> i32 {
    let base: u32 = (payload_size as u32) & kResponseSizeMask;
    let out: u32 = if extended_header { base | kResponseHeaderExtFlag } else { base };
    out as i32
}
