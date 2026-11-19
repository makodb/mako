//! Verus round-trip proof for the REAL `base/basetypes.rs` sparse-int codec.
//!
//! Lives in the verify/ harness, never transpiled, so it can call the exec
//! encode/decode and use in-body `proof ... by (bit_vector)`. Each theorem takes
//! a value in one length class, encodes it with sparseint_dump64, decodes with
//! sparseint_load64, and proves the result equals the original -- the T4
//! round-trip, one class at a time. `bit_vector` cannot see through an array's
//! Seq view, so each leader byte is bound to a `ghost` first. See
//! docs/verification.md.
use vstd::prelude::*;

use crate::basetypes::{sparseint_dump64, sparseint_load64};

verus! {

// Length class 1: values in [-64, 63] encode to a single 7-bit byte.
#[allow(dead_code)]
pub fn roundtrip_class1(v: i64)
    requires -64 <= v <= 63,
{
    let mut buf = [0u8; 9];
    let _n = sparseint_dump64(v, &mut buf[..]);
    // dump64's contract pins buf[0] == encode(v); bind it for bit_vector.
    let ghost b0 = buf@[0];
    // Its high bit is clear (& 0x7F), which is load64's class-1 condition.
    assert(b0 & 0x80 == 0) by (bit_vector)
        requires b0 == (((v as u64) & 0xFF) as u8) & 0x7F;
    let back = sparseint_load64(&buf[..]);
    // load64's class-1 ensures now gives `back` as the class-1 decode of b0;
    // bit_vector closes decode(encode(v)) == v.
    assert(back == v) by (bit_vector)
        requires
            b0 == (((v as u64) & 0xFF) as u8) & 0x7F,
            -64 <= v <= 63,
            back == (if ((b0 & 0x7F) >> 6) & 1 == 1 {
                ((u64::MAX << 8) | (((b0 & 0x7F) | 0xC0u8) as u64)) as i64
            } else {
                (b0 & 0x7F) as i64
            });
}

// Length class 2: values in [-8192, 8191] \ [-64, 63], two bytes.
#[allow(dead_code)]
pub fn roundtrip_class2(v: i64)
    requires -8192 <= v <= 8191, !(-64 <= v <= 63),
{
    let mut buf = [0u8; 9];
    let _n = sparseint_dump64(v, &mut buf[..]);
    let ghost b0 = buf@[0];
    let ghost b1 = buf@[1];
    assert(b0 & 0xC0 == 0x80) by (bit_vector)
        requires b0 == (((((v as u64) >> 8) & 0xFF) as u8) & 0x3F) | 0x80u8;
    // Skip the class-1 arm of load64's nested-if ensures (needs bit7 set).
    assert(b0 & 0x80 != 0) by (bit_vector)
        requires b0 & 0xC0 == 0x80;
    let back = sparseint_load64(&buf[..]);
    assert(back == v) by (bit_vector)
        requires
            b0 == (((((v as u64) >> 8) & 0xFF) as u8) & 0x3F) | 0x80u8,
            b1 == (((v as u64) & 0xFF) as u8),
            -8192 <= v <= 8191,
            !(-64 <= v <= 63),
            back == (if ((b0 & 0x3F) >> 5) & 1 == 1 {
                ((u64::MAX << 16) | (b1 as u64) | ((((b0 & 0x3F) | 0xE0u8) as u64) << 8)) as i64
            } else {
                ((b1 as u64) | (((b0 & 0x3F) as u64) << 8)) as i64
            });
}

// Length class 3: values in [-1048576, 1048575] \ [-8192, 8191], three bytes.
#[allow(dead_code)]
pub fn roundtrip_class3(v: i64)
    requires -1_048_576 <= v <= 1_048_575, !(-8192 <= v <= 8191),
{
    let mut buf = [0u8; 9];
    let _n = sparseint_dump64(v, &mut buf[..]);
    let ghost b0 = buf@[0];
    let ghost b1 = buf@[1];
    let ghost b2 = buf@[2];
    // The marker classifies as class 3: skip the class-1/2 arms, match class 3.
    assert(b0 & 0x80 != 0 && b0 & 0xC0 != 0x80 && b0 & 0xE0 == 0xC0) by (bit_vector)
        requires b0 == (((((v as u64) >> 16) & 0xFF) as u8) & 0x1F) | 0xC0u8;
    let back = sparseint_load64(&buf[..]);
    assert(back == v) by (bit_vector)
        requires
            b0 == (((((v as u64) >> 16) & 0xFF) as u8) & 0x1F) | 0xC0u8,
            b1 == ((((v as u64) >> 8) & 0xFF) as u8),
            b2 == (((v as u64) & 0xFF) as u8),
            -1_048_576 <= v <= 1_048_575,
            !(-8192 <= v <= 8191),
            back == (if ((b0 & 0x1F) >> 4) & 1 == 1 {
                ((u64::MAX << 24) | ((b2 as u64) | ((b1 as u64) << 8))
                    | ((((b0 & 0x1F) | 0xF0u8) as u64) << 16)) as i64
            } else {
                (((b2 as u64) | ((b1 as u64) << 8)) | (((b0 & 0x1F) as u64) << 16)) as i64
            });
}

// Length class 4: [-134217728, 134217727] \ [-1048576, 1048575], four bytes.
#[allow(dead_code)]
pub fn roundtrip_class4(v: i64)
    requires -134_217_728 <= v <= 134_217_727, !(-1_048_576 <= v <= 1_048_575),
{
    let mut buf = [0u8; 9];
    let _n = sparseint_dump64(v, &mut buf[..]);
    let ghost b0 = buf@[0];
    let ghost b1 = buf@[1];
    let ghost b2 = buf@[2];
    let ghost b3 = buf@[3];
    assert(b0 & 0x80 != 0 && b0 & 0xC0 != 0x80 && b0 & 0xE0 != 0xC0 && b0 & 0xF0 == 0xE0)
        by (bit_vector)
        requires b0 == (((((v as u64) >> 24) & 0xFF) as u8) & 0x0F) | 0xE0u8;
    let back = sparseint_load64(&buf[..]);
    assert(back == v) by (bit_vector)
        requires
            b0 == (((((v as u64) >> 24) & 0xFF) as u8) & 0x0F) | 0xE0u8,
            b1 == ((((v as u64) >> 16) & 0xFF) as u8),
            b2 == ((((v as u64) >> 8) & 0xFF) as u8),
            b3 == (((v as u64) & 0xFF) as u8),
            -134_217_728 <= v <= 134_217_727,
            !(-1_048_576 <= v <= 1_048_575),
            back == (if ((b0 & 0x0F) >> 3) & 1 == 1 {
                ((u64::MAX << 32)
                    | ((b3 as u64) | ((b2 as u64) << 8) | ((b1 as u64) << 16))
                    | ((((b0 & 0x0F) | 0xF8u8) as u64) << 24)) as i64
            } else {
                (((b3 as u64) | ((b2 as u64) << 8) | ((b1 as u64) << 16))
                    | (((b0 & 0x0F) as u64) << 24)) as i64
            });
}

// Length class 5: [-17179869184, 17179869183] \ [-134217728, 134217727], five bytes.
#[allow(dead_code)]
pub fn roundtrip_class5(v: i64)
    requires -17_179_869_184 <= v <= 17_179_869_183, !(-134_217_728 <= v <= 134_217_727),
{
    let mut buf = [0u8; 9];
    let _n = sparseint_dump64(v, &mut buf[..]);
    let ghost b0 = buf@[0];
    let ghost b1 = buf@[1];
    let ghost b2 = buf@[2];
    let ghost b3 = buf@[3];
    let ghost b4 = buf@[4];
    assert(b0 & 0x80 != 0 && b0 & 0xC0 != 0x80 && b0 & 0xE0 != 0xC0 && b0 & 0xF0 != 0xE0
        && b0 & 0xF8 == 0xF0) by (bit_vector)
        requires b0 == (((((v as u64) >> 32) & 0xFF) as u8) & 0x07) | 0xF0u8;
    let back = sparseint_load64(&buf[..]);
    assert(back == v) by (bit_vector)
        requires
            b0 == (((((v as u64) >> 32) & 0xFF) as u8) & 0x07) | 0xF0u8,
            b1 == ((((v as u64) >> 24) & 0xFF) as u8),
            b2 == ((((v as u64) >> 16) & 0xFF) as u8),
            b3 == ((((v as u64) >> 8) & 0xFF) as u8),
            b4 == (((v as u64) & 0xFF) as u8),
            -17_179_869_184 <= v <= 17_179_869_183,
            !(-134_217_728 <= v <= 134_217_727),
            back == (if ((b0 & 0x07) >> 2) & 1 == 1 {
                ((u64::MAX << 40)
                    | ((b4 as u64) | ((b3 as u64) << 8) | ((b2 as u64) << 16) | ((b1 as u64) << 24))
                    | ((((b0 & 0x07) | 0xFCu8) as u64) << 32)) as i64
            } else {
                (((b4 as u64) | ((b3 as u64) << 8) | ((b2 as u64) << 16) | ((b1 as u64) << 24))
                    | (((b0 & 0x07) as u64) << 32)) as i64
            });
}

// Length class 6: [-2199023255552, 2199023255551] \ [-17179869184, 17179869183], six bytes.
#[allow(dead_code)]
pub fn roundtrip_class6(v: i64)
    requires
        -2_199_023_255_552 <= v <= 2_199_023_255_551,
        !(-17_179_869_184 <= v <= 17_179_869_183),
{
    let mut buf = [0u8; 9];
    let _n = sparseint_dump64(v, &mut buf[..]);
    let ghost b0 = buf@[0];
    let ghost b1 = buf@[1];
    let ghost b2 = buf@[2];
    let ghost b3 = buf@[3];
    let ghost b4 = buf@[4];
    let ghost b5 = buf@[5];
    assert(b0 & 0x80 != 0 && b0 & 0xC0 != 0x80 && b0 & 0xE0 != 0xC0 && b0 & 0xF0 != 0xE0
        && b0 & 0xF8 != 0xF0 && b0 & 0xFC == 0xF8) by (bit_vector)
        requires b0 == (((((v as u64) >> 40) & 0xFF) as u8) & 0x03) | 0xF8u8;
    let back = sparseint_load64(&buf[..]);
    assert(back == v) by (bit_vector)
        requires
            b0 == (((((v as u64) >> 40) & 0xFF) as u8) & 0x03) | 0xF8u8,
            b1 == ((((v as u64) >> 32) & 0xFF) as u8),
            b2 == ((((v as u64) >> 24) & 0xFF) as u8),
            b3 == ((((v as u64) >> 16) & 0xFF) as u8),
            b4 == ((((v as u64) >> 8) & 0xFF) as u8),
            b5 == (((v as u64) & 0xFF) as u8),
            -2_199_023_255_552 <= v <= 2_199_023_255_551,
            !(-17_179_869_184 <= v <= 17_179_869_183),
            back == (if ((b0 & 0x03) >> 1) & 1 == 1 {
                ((u64::MAX << 48)
                    | ((b5 as u64) | ((b4 as u64) << 8) | ((b3 as u64) << 16) | ((b2 as u64) << 24)
                        | ((b1 as u64) << 32))
                    | ((((b0 & 0x03) | 0xFEu8) as u64) << 40)) as i64
            } else {
                (((b5 as u64) | ((b4 as u64) << 8) | ((b3 as u64) << 16) | ((b2 as u64) << 24)
                    | ((b1 as u64) << 32))
                    | (((b0 & 0x03) as u64) << 40)) as i64
            });
}

// Length class 7: [-281474976710656, 281474976710655] \ [-2199023255552, 2199023255551], seven bytes.
#[allow(dead_code)]
pub fn roundtrip_class7(v: i64)
    requires
        -281_474_976_710_656 <= v <= 281_474_976_710_655,
        !(-2_199_023_255_552 <= v <= 2_199_023_255_551),
{
    let mut buf = [0u8; 9];
    let _n = sparseint_dump64(v, &mut buf[..]);
    let ghost b0 = buf@[0];
    let ghost b1 = buf@[1];
    let ghost b2 = buf@[2];
    let ghost b3 = buf@[3];
    let ghost b4 = buf@[4];
    let ghost b5 = buf@[5];
    let ghost b6 = buf@[6];
    assert(b0 & 0x80 != 0 && b0 & 0xC0 != 0x80 && b0 & 0xE0 != 0xC0 && b0 & 0xF0 != 0xE0
        && b0 & 0xF8 != 0xF0 && b0 & 0xFC != 0xF8 && b0 & 0xFE == 0xFC) by (bit_vector)
        requires b0 == (((((v as u64) >> 48) & 0xFF) as u8) & 0x01) | 0xFCu8;
    let back = sparseint_load64(&buf[..]);
    assert(back == v) by (bit_vector)
        requires
            b0 == (((((v as u64) >> 48) & 0xFF) as u8) & 0x01) | 0xFCu8,
            b1 == ((((v as u64) >> 40) & 0xFF) as u8),
            b2 == ((((v as u64) >> 32) & 0xFF) as u8),
            b3 == ((((v as u64) >> 24) & 0xFF) as u8),
            b4 == ((((v as u64) >> 16) & 0xFF) as u8),
            b5 == ((((v as u64) >> 8) & 0xFF) as u8),
            b6 == (((v as u64) & 0xFF) as u8),
            -281_474_976_710_656 <= v <= 281_474_976_710_655,
            !(-2_199_023_255_552 <= v <= 2_199_023_255_551),
            back == (if ((b0 & 0x01) & 1) == 1 {
                ((u64::MAX << 56)
                    | ((b6 as u64) | ((b5 as u64) << 8) | ((b4 as u64) << 16) | ((b3 as u64) << 24)
                        | ((b2 as u64) << 32) | ((b1 as u64) << 40))
                    | ((((b0 & 0x01) | 0xFFu8) as u64) << 48)) as i64
            } else {
                (((b6 as u64) | ((b5 as u64) << 8) | ((b4 as u64) << 16) | ((b3 as u64) << 24)
                    | ((b2 as u64) << 32) | ((b1 as u64) << 40))
                    | (((b0 & 0x01) as u64) << 48)) as i64
            });
}

// Length class 9: everything past the 7-byte range, eight payload bytes + 0xFF leader.
#[allow(dead_code)]
pub fn roundtrip_class9(v: i64)
    requires !(-281_474_976_710_656 <= v <= 281_474_976_710_655),
{
    let mut buf = [0u8; 9];
    let _n = sparseint_dump64(v, &mut buf[..]);
    let ghost b0 = buf@[0];
    let ghost b1 = buf@[1];
    let ghost b2 = buf@[2];
    let ghost b3 = buf@[3];
    let ghost b4 = buf@[4];
    let ghost b5 = buf@[5];
    let ghost b6 = buf@[6];
    let ghost b7 = buf@[7];
    let ghost b8 = buf@[8];
    // Marker is 0xFF: skip every class-1..7 arm, reach the else.
    assert(b0 & 0x80 != 0 && b0 & 0xC0 != 0x80 && b0 & 0xE0 != 0xC0 && b0 & 0xF0 != 0xE0
        && b0 & 0xF8 != 0xF0 && b0 & 0xFC != 0xF8 && b0 & 0xFE != 0xFC) by (bit_vector)
        requires b0 == 0xFFu8;
    let back = sparseint_load64(&buf[..]);
    assert(back == v) by (bit_vector)
        requires
            b1 == ((((v as u64) >> 56) & 0xFF) as u8),
            b2 == ((((v as u64) >> 48) & 0xFF) as u8),
            b3 == ((((v as u64) >> 40) & 0xFF) as u8),
            b4 == ((((v as u64) >> 32) & 0xFF) as u8),
            b5 == ((((v as u64) >> 24) & 0xFF) as u8),
            b6 == ((((v as u64) >> 16) & 0xFF) as u8),
            b7 == ((((v as u64) >> 8) & 0xFF) as u8),
            b8 == (((v as u64) & 0xFF) as u8),
            back == ((b8 as u64) | ((b7 as u64) << 8) | ((b6 as u64) << 16) | ((b5 as u64) << 24)
                | ((b4 as u64) << 32) | ((b3 as u64) << 40) | ((b2 as u64) << 48)
                | ((b1 as u64) << 56)) as i64;
}

}
