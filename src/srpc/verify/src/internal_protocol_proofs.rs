//! Verus proofs about the REAL `rpc/internal_protocol.rs` functions.
//!
//! This file lives in the verify/ harness, not in the srpc crate, so it is
//! never compiled into production and never transpiled to C++ -- which is what
//! lets it use in-body `proof! { assert(..) by (bit_vector) }`. It proves the
//! two header-codec theorems by calling the actual functions and reasoning from
//! their definitional contracts plus bit-vector facts about the 0x7fffffff /
//! 0x80000000 masks. See docs/verification.md.
use vstd::prelude::*;

use crate::internal_protocol::{
    encode_response_size, response_has_extended_header, response_payload_size,
};

// Property 1: decoding never yields a value outside the 31-bit size space.
// For every input, response_payload_size is non-negative and <= kResponseSizeMask.
#[verus_spec]
#[allow(dead_code)]
pub fn prove_payload_size_in_range(encoded_size: i32) {
    let r = response_payload_size(encoded_size);
    proof! {
        assert(((encoded_size as u32) & 0x7fffffffu32) < 0x80000000u32) by (bit_vector);
        assert(0i32 <= r);
        assert(r <= 0x7fffffffi32);
    }
}

// Property 2: encode then decode round-trips both the size and the flag, for any
// non-negative payload_size. The precondition is real: encode masks the top bit
// off, so a negative size could not round-trip.
#[verus_spec(
    requires payload_size >= 0i32,
)]
#[allow(dead_code)]
pub fn prove_roundtrip(payload_size: i32, extended_header: bool) {
    let e = encode_response_size(payload_size, extended_header);
    let size_back = response_payload_size(e);
    let flag_back = response_has_extended_header(e);
    proof! {
        // `e` equals encode's body word (from encode's contract); discharge both
        // round-trip goals from that hypothesis in one bit-vector step.
        assert(
            (((e as u32) & 0x7fffffffu32) as i32) == payload_size
            && (((e as u32) & 0x80000000u32) != 0) == extended_header
        ) by (bit_vector)
            requires
                payload_size >= 0i32,
                e == ((if extended_header {
                    ((payload_size as u32) & 0x7fffffffu32) | 0x80000000u32
                } else {
                    (payload_size as u32) & 0x7fffffffu32
                }) as i32);
        assert(size_back == payload_size);
        assert(flag_back == extended_header);
    }
}
