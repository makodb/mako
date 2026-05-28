module;

#include <stdint.h>
#include <rusty/rusty.hpp>

export module rrr.internal_protocol;

import std;

// @safe - Wire-protocol constants + pure bit-twiddling helpers.
// No raw pointers, syscalls, or operator-overload chains.
export namespace rrr {

constexpr int32_t kInternalHeartbeatRpcId = std::numeric_limits<int32_t>::min();

constexpr uint32_t kResponseHeaderExtFlag = 0x80000000u;
constexpr uint32_t kResponseSizeMask = 0x7fffffffu;

// Free helpers backing the three response-header bit-twiddling
// inlines. The high bit of the encoded i32 marks "extended header"
// (response carries `<server_instance_id>` after `<error_code>`);
// the low 31 bits hold the payload size. Authored as inline Rust DSL.
#if RUSTYCPP_RUST
fn internal_protocol_response_has_extended_header(encoded_size: i32) -> bool {
    ((encoded_size as u32) & 0x80000000) != 0
}

fn internal_protocol_response_payload_size(encoded_size: i32) -> i32 {
    ((encoded_size as u32) & 0x7fffffff) as i32
}

fn internal_protocol_encode_response_size(payload_size: i32, extended_header: bool) -> i32 {
    let base: u32 = (payload_size as u32) & 0x7fffffff;
    let out: u32 = if extended_header { base | 0x80000000 } else { base };
    out as i32
}
#endif
/*RUSTYCPP:GEN-BEGIN id=internal_protocol.1 version=1 rust_sha256=c89038524f01c6432929da2c60b1a9de977915d779ae3a676b173e910a67f128*/
bool internal_protocol_response_has_extended_header(int32_t encoded_size);
int32_t internal_protocol_response_payload_size(int32_t encoded_size);
int32_t internal_protocol_encode_response_size(int32_t payload_size, bool extended_header);

bool internal_protocol_response_has_extended_header(int32_t encoded_size) {
    return ((((static_cast<uint32_t>(encoded_size))) & static_cast<int32_t>(2147483648))) != static_cast<int32_t>(0);
}

int32_t internal_protocol_response_payload_size(int32_t encoded_size) {
    return static_cast<int32_t>((((static_cast<uint32_t>(encoded_size))) & 2147483647));
}

int32_t internal_protocol_encode_response_size(int32_t payload_size, bool extended_header) {
    const uint32_t base = ((static_cast<uint32_t>(payload_size))) & static_cast<uint32_t>(2147483647);
    const uint32_t out = (extended_header ? rusty::detail::deref_if_pointer_like(base) | static_cast<uint32_t>(2147483648) : base);
    return static_cast<int32_t>(out);
}
/*RUSTYCPP:GEN-END id=internal_protocol.1*/

inline bool response_has_extended_header(int32_t encoded_size) {
    return internal_protocol_response_has_extended_header(encoded_size);
}

inline int32_t response_payload_size(int32_t encoded_size) {
    return internal_protocol_response_payload_size(encoded_size);
}

inline int32_t encode_response_size(int32_t payload_size, bool extended_header) {
    return internal_protocol_encode_response_size(payload_size, extended_header);
}

} // export namespace rrr
