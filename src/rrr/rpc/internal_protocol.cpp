module;

#include <stdint.h>
#include <rusty/rusty.hpp>

export module rrr.internal_protocol;

import std;

// @safe - Wire-protocol constants + pure bit-twiddling functions. All
// three constants and all three response-header functions are authored
// as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is the
// source of truth, and the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block immediately after it with the
// C++ implementation. The C++ compiler only sees the GEN block. No
// raw pointers, syscalls, or operator-overload chains.
export namespace rrr {

// The high bit of the encoded i32 marks "extended header" (response
// carries `<server_instance_id>` after `<error_code>`); the low 31
// bits hold the payload size.
#if RUSTYCPP_RUST
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
#endif
/*RUSTYCPP:GEN-BEGIN id=internal_protocol.1 version=1 rust_sha256=bd1d65d821abf33a30b8d3e4ac1b01c58c35d53b3bdb8a31772eb30b8b1d692c*/
constexpr int32_t kInternalHeartbeatRpcId = std::numeric_limits<int32_t>::min();
constexpr uint32_t kResponseHeaderExtFlag = static_cast<uint32_t>(2147483648);
constexpr uint32_t kResponseSizeMask = static_cast<uint32_t>(2147483647);
bool response_has_extended_header(int32_t encoded_size);
int32_t response_payload_size(int32_t encoded_size);
int32_t encode_response_size(int32_t payload_size, bool extended_header);




bool response_has_extended_header(int32_t encoded_size) {
    return ((((static_cast<uint32_t>(encoded_size))) & rusty::detail::deref_if_pointer_like(kResponseHeaderExtFlag))) != static_cast<uint32_t>(0);
}

int32_t response_payload_size(int32_t encoded_size) {
    return static_cast<int32_t>((((static_cast<uint32_t>(encoded_size))) & rusty::detail::deref_if_pointer_like(kResponseSizeMask)));
}

int32_t encode_response_size(int32_t payload_size, bool extended_header) {
    const uint32_t base = ((static_cast<uint32_t>(payload_size))) & rusty::detail::deref_if_pointer_like(kResponseSizeMask);
    const uint32_t out = (extended_header ? rusty::detail::deref_if_pointer_like(base) | rusty::detail::deref_if_pointer_like(kResponseHeaderExtFlag) : base);
    return static_cast<int32_t>(out);
}
/*RUSTYCPP:GEN-END id=internal_protocol.1*/

} // export namespace rrr
