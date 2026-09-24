//! Verus support for the REAL `rpc/frame_codec.rs` peek-header bound (T6).
//!
//! Lives in the verify/ harness, never transpiled. `FrameDecodeStatus` and
//! `FrameHeader` are declared outside `verus!`, so Verus needs external type
//! specifications to admit them into the contract that frame_codec_peek_header
//! carries. The bound itself (Complete => payload in [0, kMaxFramePayloadSize])
//! is discharged in place from the function's two guards -- it needs no proof
//! here, and it assumes nothing about what the leader bytes decode to. See
//! docs/verification.md.
#[allow(unused_imports)]
use vstd::prelude::*;

use crate::frame_codec::{
    frame_codec_peek_header, frame_codec_write_header, FrameDecodeStatus, FrameHeader,
};

#[verifier::external_type_specification]
#[allow(dead_code)]
pub struct ExFrameDecodeStatus(FrameDecodeStatus);

#[verifier::external_type_specification]
#[allow(dead_code)]
pub struct ExFrameHeader(FrameHeader);

verus! {

// T5: write_header -> peek_header recovers both the payload size and the
// extended-header flag, for every in-range size and either flag.
//
// The bit-packing half of this is already proven in internal_protocol_proofs;
// what this adds is the 4-byte marshalling, which rests on the trusted
// little-endian axiom on frame_codec's store/load helpers (see the SCOPE note
// there). kMaxFramePayloadSize is 64 MiB = 67_108_864.
#[allow(dead_code)]
pub fn roundtrip_header(payload_size: i32, extended_header_flag: bool)
    requires 0 <= payload_size <= 67_108_864,
{
    let mut buf = [0u8; 4];
    let ok = frame_codec_write_header(&mut buf[..], payload_size, extended_header_flag);
    assert(ok);
    let ghost b0 = buf@[0];
    let ghost b1 = buf@[1];
    let ghost b2 = buf@[2];
    let ghost b3 = buf@[3];
    // The leader decodes back inside the frame bound, so peek reports Complete.
    assert(0 <= ((((((b0 as u32) | ((b1 as u32) << 8) | ((b2 as u32) << 16)
            | ((b3 as u32) << 24))) as i32) as u32) & 0x7fffffffu32) as i32
        && ((((((b0 as u32) | ((b1 as u32) << 8) | ((b2 as u32) << 16)
            | ((b3 as u32) << 24))) as i32) as u32) & 0x7fffffffu32) as i32 <= 67_108_864)
        by (bit_vector)
        requires
            0 <= payload_size <= 67_108_864,
            b0 == ((((if extended_header_flag {
                ((payload_size as u32) & 0x7fffffffu32) | 0x80000000u32
            } else { (payload_size as u32) & 0x7fffffffu32 }) as i32) as u32) & 0xFF) as u8,
            b1 == (((((if extended_header_flag {
                ((payload_size as u32) & 0x7fffffffu32) | 0x80000000u32
            } else { (payload_size as u32) & 0x7fffffffu32 }) as i32) as u32) >> 8) & 0xFF) as u8,
            b2 == (((((if extended_header_flag {
                ((payload_size as u32) & 0x7fffffffu32) | 0x80000000u32
            } else { (payload_size as u32) & 0x7fffffffu32 }) as i32) as u32) >> 16) & 0xFF) as u8,
            b3 == (((((if extended_header_flag {
                ((payload_size as u32) & 0x7fffffffu32) | 0x80000000u32
            } else { (payload_size as u32) & 0x7fffffffu32 }) as i32) as u32) >> 24) & 0xFF) as u8;
    let mut h = FrameHeader { payload_size: 0, extended_header_flag: false };
    let st = frame_codec_peek_header(&buf[..], &mut h);
    assert(st == FrameDecodeStatus::Complete);
    // peek's T5 clause now gives h in terms of the leader word; close the loop.
    // bit_vector cannot see through struct field access, so bind the fields.
    let ghost hp = h.payload_size;
    let ghost hf = h.extended_header_flag;
    assert(hp == payload_size && hf == extended_header_flag)
        by (bit_vector)
        requires
            0 <= payload_size <= 67_108_864,
            b0 == ((((if extended_header_flag {
                ((payload_size as u32) & 0x7fffffffu32) | 0x80000000u32
            } else { (payload_size as u32) & 0x7fffffffu32 }) as i32) as u32) & 0xFF) as u8,
            b1 == (((((if extended_header_flag {
                ((payload_size as u32) & 0x7fffffffu32) | 0x80000000u32
            } else { (payload_size as u32) & 0x7fffffffu32 }) as i32) as u32) >> 8) & 0xFF) as u8,
            b2 == (((((if extended_header_flag {
                ((payload_size as u32) & 0x7fffffffu32) | 0x80000000u32
            } else { (payload_size as u32) & 0x7fffffffu32 }) as i32) as u32) >> 16) & 0xFF) as u8,
            b3 == (((((if extended_header_flag {
                ((payload_size as u32) & 0x7fffffffu32) | 0x80000000u32
            } else { (payload_size as u32) & 0x7fffffffu32 }) as i32) as u32) >> 24) & 0xFF) as u8,
            hp == ((((((b0 as u32) | ((b1 as u32) << 8) | ((b2 as u32) << 16)
                | ((b3 as u32) << 24))) as i32) as u32) & 0x7fffffffu32) as i32,
            hf == (((((((b0 as u32) | ((b1 as u32) << 8)
                | ((b2 as u32) << 16) | ((b3 as u32) << 24))) as i32) as u32)
                & 0x80000000u32) != 0);
}

}
