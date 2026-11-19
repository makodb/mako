// Canonical Rust source for the srpc.basetypes module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.
pub use std::sync::atomic::{AtomicI64, Ordering};

#[allow(unsafe_code)]
unsafe extern "C" {
    fn srpc_clock_monotonic_us() -> u64;
    fn srpc_clock_realtime_coarse_us() -> u64;
    fn srpc_gettimeofday_us() -> u64;
    fn srpc_sleep_us(microseconds: u64);
}

#[allow(non_camel_case_types)]
pub type i8 = ::core::primitive::i8;
#[allow(non_camel_case_types)]
pub type i16 = ::core::primitive::i16;
#[allow(non_camel_case_types)]
pub type i32 = ::core::primitive::i32;
#[allow(non_camel_case_types)]
pub type i64 = ::core::primitive::i64;

// Verus specs (behind #[cfg(verus)], invisible to rustc and rusty-cpp) pin the
// sparse-int length functions. They are FREE functions because the verus_spec
// return-binding macro mis-handles associated (impl) functions; SparseInt's
// methods delegate to them, so the shipped length logic is what is proven. See
// docs/verification.md.
#[cfg(verus)]
use vstd::prelude::*;

// The encoder's byte length for `val`. Extracted from SparseInt::val_size so it
// can carry a Verus contract; the method delegates. The body is the original
// inclusive-range logic verbatim (Verus discharges the bound over `.contains()`).
#[cfg_attr(verus, verus_spec(r =>
    ensures 1usize <= r <= 9usize && r != 8usize,
))]
pub fn sparseint_val_size(val: i64) -> usize {
    if (-64..=63).contains(&val) {
        1
    } else if (-8192..=8191).contains(&val) {
        2
    } else if (-1_048_576..=1_048_575).contains(&val) {
        3
    } else if (-134_217_728..=134_217_727).contains(&val) {
        4
    } else if (-17_179_869_184..=17_179_869_183).contains(&val) {
        5
    } else if (-2_199_023_255_552..=2_199_023_255_551).contains(&val) {
        6
    } else if (-281_474_976_710_656..=281_474_976_710_655).contains(&val) {
        7
    } else {
        // The historical 8-byte (0xFE) rung is retired (docs/testing-plan.md
        // 4.1): it budgeted 7 payload bytes but the encoder emitted 8, silently
        // dropping the low byte of any value in +-[2^48, 2^55). Everything past
        // the 7-byte range now uses the correct 9-byte (0xFF) encoding. The
        // `r != 8` postcondition is the machine-checked statement of that fix.
        9
    }
}

// The decoder's byte length from a leading byte. Extracted from
// SparseInt::buf_size for the same reason. Unlike the encoder this CAN return 8:
// the decoder still reads the historical 0xFE (length-8) leader for old data.
#[cfg_attr(verus, verus_spec(r =>
    ensures
        1usize <= r <= 9usize,
        r == (
            if byte0 & 0x80 == 0 { 1usize }
            else if byte0 & 0xC0 == 0x80 { 2 }
            else if byte0 & 0xE0 == 0xC0 { 3 }
            else if byte0 & 0xF0 == 0xE0 { 4 }
            else if byte0 & 0xF8 == 0xF0 { 5 }
            else if byte0 & 0xFC == 0xF8 { 6 }
            else if byte0 & 0xFE == 0xFC { 7 }
            else if byte0 == 0xFE { 8 }
            else { 9 }
        ),
))]
pub fn sparseint_buf_size(byte0: u8) -> usize {
    if (byte0 & 0x80) == 0 {
        1
    } else if (byte0 & 0xC0) == 0x80 {
        2
    } else if (byte0 & 0xE0) == 0xC0 {
        3
    } else if (byte0 & 0xF0) == 0xE0 {
        4
    } else if (byte0 & 0xF8) == 0xF0 {
        5
    } else if (byte0 & 0xFC) == 0xF8 {
        6
    } else if (byte0 & 0xFE) == 0xFC {
        7
    } else if byte0 == 0xFE {
        8
    } else {
        9
    }
}

// Encode `val` into `buf` in the sparse-int wire format; returns the byte count
// (== sparseint_val_size(val)). Slice-based free-function form of
// SparseInt::dump64 -- the raw-pointer method delegates here. Taking `&mut [u8]`
// (rather than `*mut u8`) makes the writes bounds-checked, so a mis-sized buffer
// is a panic rather than out-of-bounds memory, and lets Verus reason about the
// byte writes (T4, docs/testing-plan.md). `buf` must hold at least 9 bytes.
// Encode is UNROLLED (one straight-line branch per length), rather than a loop,
// so it can carry a Verus contract: canonical sources cannot hold loop
// invariants (those need a verus! block the transpiler rejects), but straight-
// line code proves definitionally. Each branch is exactly the loop's writes for
// that length -- big-endian payload bytes then the unary-length marker on byte 0.
// The length classes match sparseint_val_size; the 8-byte (0xFE) rung is retired.
//
// The #[cfg(verus)] contract pins the marker byte per length class -- enough for
// the round-trip theorem (verify/src/basetypes_proofs.rs) to see which decode
// branch load64 will take. It is built up one class at a time.
#[cfg_attr(verus, verus_spec(r =>
    requires buf.len() >= 9,
    ensures
        final(buf)@.len() == old(buf)@.len(),
        (-64 <= val <= 63) ==> final(buf)@[0] == (((val as u64) & 0xFF) as u8) & 0x7F,
        (!(-64 <= val <= 63) && -8192 <= val <= 8191) ==> (
            final(buf)@[0] == (((((val as u64) >> 8) & 0xFF) as u8) & 0x3F) | 0x80u8
            && final(buf)@[1] == (((val as u64) & 0xFF) as u8)
        ),
        (!(-8192 <= val <= 8191) && -1_048_576 <= val <= 1_048_575) ==> (
            final(buf)@[0] == (((((val as u64) >> 16) & 0xFF) as u8) & 0x1F) | 0xC0u8
            && final(buf)@[1] == ((((val as u64) >> 8) & 0xFF) as u8)
            && final(buf)@[2] == (((val as u64) & 0xFF) as u8)
        ),
        (!(-1_048_576 <= val <= 1_048_575) && -134_217_728 <= val <= 134_217_727) ==> (
            final(buf)@[0] == (((((val as u64) >> 24) & 0xFF) as u8) & 0x0F) | 0xE0u8
            && final(buf)@[1] == ((((val as u64) >> 16) & 0xFF) as u8)
            && final(buf)@[2] == ((((val as u64) >> 8) & 0xFF) as u8)
            && final(buf)@[3] == (((val as u64) & 0xFF) as u8)
        ),
        (!(-134_217_728 <= val <= 134_217_727) && -17_179_869_184 <= val <= 17_179_869_183) ==> (
            final(buf)@[0] == (((((val as u64) >> 32) & 0xFF) as u8) & 0x07) | 0xF0u8
            && final(buf)@[1] == ((((val as u64) >> 24) & 0xFF) as u8)
            && final(buf)@[2] == ((((val as u64) >> 16) & 0xFF) as u8)
            && final(buf)@[3] == ((((val as u64) >> 8) & 0xFF) as u8)
            && final(buf)@[4] == (((val as u64) & 0xFF) as u8)
        ),
        (!(-17_179_869_184 <= val <= 17_179_869_183)
            && -2_199_023_255_552 <= val <= 2_199_023_255_551) ==> (
            final(buf)@[0] == (((((val as u64) >> 40) & 0xFF) as u8) & 0x03) | 0xF8u8
            && final(buf)@[1] == ((((val as u64) >> 32) & 0xFF) as u8)
            && final(buf)@[2] == ((((val as u64) >> 24) & 0xFF) as u8)
            && final(buf)@[3] == ((((val as u64) >> 16) & 0xFF) as u8)
            && final(buf)@[4] == ((((val as u64) >> 8) & 0xFF) as u8)
            && final(buf)@[5] == (((val as u64) & 0xFF) as u8)
        ),
        (!(-2_199_023_255_552 <= val <= 2_199_023_255_551)
            && -281_474_976_710_656 <= val <= 281_474_976_710_655) ==> (
            final(buf)@[0] == (((((val as u64) >> 48) & 0xFF) as u8) & 0x01) | 0xFCu8
            && final(buf)@[1] == ((((val as u64) >> 40) & 0xFF) as u8)
            && final(buf)@[2] == ((((val as u64) >> 32) & 0xFF) as u8)
            && final(buf)@[3] == ((((val as u64) >> 24) & 0xFF) as u8)
            && final(buf)@[4] == ((((val as u64) >> 16) & 0xFF) as u8)
            && final(buf)@[5] == ((((val as u64) >> 8) & 0xFF) as u8)
            && final(buf)@[6] == (((val as u64) & 0xFF) as u8)
        ),
        (!(-281_474_976_710_656 <= val <= 281_474_976_710_655)) ==> (
            final(buf)@[0] == 0xFFu8
            && final(buf)@[1] == ((((val as u64) >> 56) & 0xFF) as u8)
            && final(buf)@[2] == ((((val as u64) >> 48) & 0xFF) as u8)
            && final(buf)@[3] == ((((val as u64) >> 40) & 0xFF) as u8)
            && final(buf)@[4] == ((((val as u64) >> 32) & 0xFF) as u8)
            && final(buf)@[5] == ((((val as u64) >> 24) & 0xFF) as u8)
            && final(buf)@[6] == ((((val as u64) >> 16) & 0xFF) as u8)
            && final(buf)@[7] == ((((val as u64) >> 8) & 0xFF) as u8)
            && final(buf)@[8] == (((val as u64) & 0xFF) as u8)
        ),
))]
pub fn sparseint_dump64(val: i64, buf: &mut [u8]) -> usize {
    let u = val as u64;
    if (-64..=63).contains(&val) {
        buf[0] = (u & 0xFF) as u8;
        buf[0] &= 0x7F;
        1
    } else if (-8192..=8191).contains(&val) {
        buf[0] = ((u >> 8) & 0xFF) as u8;
        buf[1] = (u & 0xFF) as u8;
        buf[0] &= 0x3F;
        buf[0] |= 0x80;
        2
    } else if (-1_048_576..=1_048_575).contains(&val) {
        buf[0] = ((u >> 16) & 0xFF) as u8;
        buf[1] = ((u >> 8) & 0xFF) as u8;
        buf[2] = (u & 0xFF) as u8;
        buf[0] &= 0x1F;
        buf[0] |= 0xC0;
        3
    } else if (-134_217_728..=134_217_727).contains(&val) {
        buf[0] = ((u >> 24) & 0xFF) as u8;
        buf[1] = ((u >> 16) & 0xFF) as u8;
        buf[2] = ((u >> 8) & 0xFF) as u8;
        buf[3] = (u & 0xFF) as u8;
        buf[0] &= 0x0F;
        buf[0] |= 0xE0;
        4
    } else if (-17_179_869_184..=17_179_869_183).contains(&val) {
        buf[0] = ((u >> 32) & 0xFF) as u8;
        buf[1] = ((u >> 24) & 0xFF) as u8;
        buf[2] = ((u >> 16) & 0xFF) as u8;
        buf[3] = ((u >> 8) & 0xFF) as u8;
        buf[4] = (u & 0xFF) as u8;
        buf[0] &= 0x07;
        buf[0] |= 0xF0;
        5
    } else if (-2_199_023_255_552..=2_199_023_255_551).contains(&val) {
        buf[0] = ((u >> 40) & 0xFF) as u8;
        buf[1] = ((u >> 32) & 0xFF) as u8;
        buf[2] = ((u >> 24) & 0xFF) as u8;
        buf[3] = ((u >> 16) & 0xFF) as u8;
        buf[4] = ((u >> 8) & 0xFF) as u8;
        buf[5] = (u & 0xFF) as u8;
        buf[0] &= 0x03;
        buf[0] |= 0xF8;
        6
    } else if (-281_474_976_710_656..=281_474_976_710_655).contains(&val) {
        buf[0] = ((u >> 48) & 0xFF) as u8;
        buf[1] = ((u >> 40) & 0xFF) as u8;
        buf[2] = ((u >> 32) & 0xFF) as u8;
        buf[3] = ((u >> 24) & 0xFF) as u8;
        buf[4] = ((u >> 16) & 0xFF) as u8;
        buf[5] = ((u >> 8) & 0xFF) as u8;
        buf[6] = (u & 0xFF) as u8;
        buf[0] &= 0x01;
        buf[0] |= 0xFC;
        7
    } else {
        buf[1] = ((u >> 56) & 0xFF) as u8;
        buf[2] = ((u >> 48) & 0xFF) as u8;
        buf[3] = ((u >> 40) & 0xFF) as u8;
        buf[4] = ((u >> 32) & 0xFF) as u8;
        buf[5] = ((u >> 24) & 0xFF) as u8;
        buf[6] = ((u >> 16) & 0xFF) as u8;
        buf[7] = ((u >> 8) & 0xFF) as u8;
        buf[8] = (u & 0xFF) as u8;
        buf[0] = 0xFF;
        9
    }
}

// Decode is UNROLLED per leader length for the same reason as encode. Each arm
// reads the big-endian payload, then reconstructs the sign from the marker byte:
// mask the marker bits off byte 0, and if its top payload bit is set, sign-extend
// (the high bytes become 0xFF, expressed loop-free as `u64::MAX << 8*bsize`). The
// 8-byte (0xFE) leader is decoded for historical data but never produced by
// encode. `buf` must hold at least 9 bytes.
//
// The #[cfg(verus)] contract pins the decoded value per leader length, built up
// one class at a time; the round-trip theorem composes it with dump64's.
#[cfg_attr(verus, verus_spec(r =>
    requires buf.len() >= 9,
    ensures
        // One nested-if mirroring buf_size's classification, so proving it per
        // body branch only needs the "easy" bit direction (short-circuit: once a
        // class condition holds, later ones are never evaluated). `else { r }`
        // leaves not-yet-proven classes unconstrained; classes are filled in one
        // at a time toward the final class-9 leaf.
        r == (
            if buf@[0] & 0x80 == 0 {
                // class 1
                if ((buf@[0] & 0x7F) >> 6) & 1 == 1 {
                    ((u64::MAX << 8) | (((buf@[0] & 0x7F) | 0xC0u8) as u64)) as i64
                } else {
                    (buf@[0] & 0x7F) as i64
                }
            } else if buf@[0] & 0xC0 == 0x80 {
                // class 2
                if ((buf@[0] & 0x3F) >> 5) & 1 == 1 {
                    ((u64::MAX << 16) | (buf@[1] as u64)
                        | ((((buf@[0] & 0x3F) | 0xE0u8) as u64) << 8)) as i64
                } else {
                    ((buf@[1] as u64) | (((buf@[0] & 0x3F) as u64) << 8)) as i64
                }
            } else if buf@[0] & 0xE0 == 0xC0 {
                // class 3
                if ((buf@[0] & 0x1F) >> 4) & 1 == 1 {
                    ((u64::MAX << 24) | ((buf@[2] as u64) | ((buf@[1] as u64) << 8))
                        | ((((buf@[0] & 0x1F) | 0xF0u8) as u64) << 16)) as i64
                } else {
                    (((buf@[2] as u64) | ((buf@[1] as u64) << 8))
                        | (((buf@[0] & 0x1F) as u64) << 16)) as i64
                }
            } else if buf@[0] & 0xF0 == 0xE0 {
                // class 4
                if ((buf@[0] & 0x0F) >> 3) & 1 == 1 {
                    ((u64::MAX << 32)
                        | ((buf@[3] as u64) | ((buf@[2] as u64) << 8) | ((buf@[1] as u64) << 16))
                        | ((((buf@[0] & 0x0F) | 0xF8u8) as u64) << 24)) as i64
                } else {
                    (((buf@[3] as u64) | ((buf@[2] as u64) << 8) | ((buf@[1] as u64) << 16))
                        | (((buf@[0] & 0x0F) as u64) << 24)) as i64
                }
            } else if buf@[0] & 0xF8 == 0xF0 {
                // class 5
                if ((buf@[0] & 0x07) >> 2) & 1 == 1 {
                    ((u64::MAX << 40)
                        | ((buf@[4] as u64) | ((buf@[3] as u64) << 8) | ((buf@[2] as u64) << 16)
                            | ((buf@[1] as u64) << 24))
                        | ((((buf@[0] & 0x07) | 0xFCu8) as u64) << 32)) as i64
                } else {
                    (((buf@[4] as u64) | ((buf@[3] as u64) << 8) | ((buf@[2] as u64) << 16)
                        | ((buf@[1] as u64) << 24))
                        | (((buf@[0] & 0x07) as u64) << 32)) as i64
                }
            } else if buf@[0] & 0xFC == 0xF8 {
                // class 6
                if ((buf@[0] & 0x03) >> 1) & 1 == 1 {
                    ((u64::MAX << 48)
                        | ((buf@[5] as u64) | ((buf@[4] as u64) << 8) | ((buf@[3] as u64) << 16)
                            | ((buf@[2] as u64) << 24) | ((buf@[1] as u64) << 32))
                        | ((((buf@[0] & 0x03) | 0xFEu8) as u64) << 40)) as i64
                } else {
                    (((buf@[5] as u64) | ((buf@[4] as u64) << 8) | ((buf@[3] as u64) << 16)
                        | ((buf@[2] as u64) << 24) | ((buf@[1] as u64) << 32))
                        | (((buf@[0] & 0x03) as u64) << 40)) as i64
                }
            } else if buf@[0] & 0xFE == 0xFC {
                // class 7
                if ((buf@[0] & 0x01) & 1) == 1 {
                    ((u64::MAX << 56)
                        | ((buf@[6] as u64) | ((buf@[5] as u64) << 8) | ((buf@[4] as u64) << 16)
                            | ((buf@[3] as u64) << 24) | ((buf@[2] as u64) << 32)
                            | ((buf@[1] as u64) << 40))
                        | ((((buf@[0] & 0x01) | 0xFFu8) as u64) << 48)) as i64
                } else {
                    (((buf@[6] as u64) | ((buf@[5] as u64) << 8) | ((buf@[4] as u64) << 16)
                        | ((buf@[3] as u64) << 24) | ((buf@[2] as u64) << 32)
                        | ((buf@[1] as u64) << 40))
                        | (((buf@[0] & 0x01) as u64) << 48)) as i64
                }
            } else {
                // classes 8 (historical 0xFE) and 9 (0xFF): eight payload bytes,
                // no sign bit in the marker.
                ((buf@[8] as u64) | ((buf@[7] as u64) << 8) | ((buf@[6] as u64) << 16)
                    | ((buf@[5] as u64) << 24) | ((buf@[4] as u64) << 32)
                    | ((buf@[3] as u64) << 40) | ((buf@[2] as u64) << 48)
                    | ((buf@[1] as u64) << 56)) as i64
            }
        ),
))]
pub fn sparseint_load64(buf: &[u8]) -> i64 {
    let bsize = sparseint_buf_size(buf[0]) as i32;
    if bsize == 1 {
        let top0 = buf[0] & 0x7F;
        if ((top0 >> 6) & 1) == 1 {
            ((u64::MAX << 8) | ((top0 | 0xC0) as u64)) as i64
        } else {
            top0 as i64
        }
    } else if bsize == 2 {
        let payload = buf[1] as u64;
        let top0 = buf[0] & 0x3F;
        (if ((top0 >> 5) & 1) == 1 {
            (u64::MAX << 16) | payload | (((top0 | 0xE0) as u64) << 8)
        } else {
            payload | ((top0 as u64) << 8)
        }) as i64
    } else if bsize == 3 {
        let payload = (buf[2] as u64) | ((buf[1] as u64) << 8);
        let top0 = buf[0] & 0x1F;
        (if ((top0 >> 4) & 1) == 1 {
            (u64::MAX << 24) | payload | (((top0 | 0xF0) as u64) << 16)
        } else {
            payload | ((top0 as u64) << 16)
        }) as i64
    } else if bsize == 4 {
        let payload = (buf[3] as u64) | ((buf[2] as u64) << 8) | ((buf[1] as u64) << 16);
        let top0 = buf[0] & 0x0F;
        (if ((top0 >> 3) & 1) == 1 {
            (u64::MAX << 32) | payload | (((top0 | 0xF8) as u64) << 24)
        } else {
            payload | ((top0 as u64) << 24)
        }) as i64
    } else if bsize == 5 {
        let payload = (buf[4] as u64)
            | ((buf[3] as u64) << 8)
            | ((buf[2] as u64) << 16)
            | ((buf[1] as u64) << 24);
        let top0 = buf[0] & 0x07;
        (if ((top0 >> 2) & 1) == 1 {
            (u64::MAX << 40) | payload | (((top0 | 0xFC) as u64) << 32)
        } else {
            payload | ((top0 as u64) << 32)
        }) as i64
    } else if bsize == 6 {
        let payload = (buf[5] as u64)
            | ((buf[4] as u64) << 8)
            | ((buf[3] as u64) << 16)
            | ((buf[2] as u64) << 24)
            | ((buf[1] as u64) << 32);
        let top0 = buf[0] & 0x03;
        (if ((top0 >> 1) & 1) == 1 {
            (u64::MAX << 48) | payload | (((top0 | 0xFE) as u64) << 40)
        } else {
            payload | ((top0 as u64) << 40)
        }) as i64
    } else if bsize == 7 {
        let payload = (buf[6] as u64)
            | ((buf[5] as u64) << 8)
            | ((buf[4] as u64) << 16)
            | ((buf[3] as u64) << 24)
            | ((buf[2] as u64) << 32)
            | ((buf[1] as u64) << 40);
        let top0 = buf[0] & 0x01;
        (if (top0 & 1) == 1 {
            (u64::MAX << 56) | payload | (((top0 | 0xFF) as u64) << 48)
        } else {
            payload | ((top0 as u64) << 48)
        }) as i64
    } else {
        // 8-byte (historical 0xFE) and 9-byte (0xFF) leaders: eight payload
        // bytes in buf[1..9], no sign bit in the marker.
        let u = (buf[8] as u64)
            | ((buf[7] as u64) << 8)
            | ((buf[6] as u64) << 16)
            | ((buf[5] as u64) << 24)
            | ((buf[4] as u64) << 32)
            | ((buf[3] as u64) << 40)
            | ((buf[2] as u64) << 48)
            | ((buf[1] as u64) << 56);
        u as i64
    }
}

pub struct SparseInt {}

impl SparseInt {
    pub fn buf_size(byte0: u8) -> usize {
        sparseint_buf_size(byte0)
    }

    /// Encodes `val` into the historical sparse-integer wire format.
    ///
    /// # Safety
    ///
    /// `buf` must point to writable storage for at least five bytes.
    #[allow(unsafe_code)]
    pub unsafe fn dump32(val: i32, buf: *mut u8) -> usize {
        let u = val as u32;
        unsafe {
            if (-64..=63).contains(&val) {
                *buf.add(0) = (u & 0xFF) as u8;
                *buf.add(0) &= 0x7F;
                return 1;
            } else if (-8192..=8191).contains(&val) {
                *buf.add(0) = ((u >> 8) & 0xFF) as u8;
                *buf.add(1) = (u & 0xFF) as u8;
                *buf.add(0) &= 0x3F;
                *buf.add(0) |= 0x80;
                return 2;
            } else if (-1_048_576..=1_048_575).contains(&val) {
                *buf.add(0) = ((u >> 16) & 0xFF) as u8;
                *buf.add(1) = ((u >> 8) & 0xFF) as u8;
                *buf.add(2) = (u & 0xFF) as u8;
                *buf.add(0) &= 0x1F;
                *buf.add(0) |= 0xC0;
                return 3;
            } else if (-134_217_728..=134_217_727).contains(&val) {
                *buf.add(0) = ((u >> 24) & 0xFF) as u8;
                *buf.add(1) = ((u >> 16) & 0xFF) as u8;
                *buf.add(2) = ((u >> 8) & 0xFF) as u8;
                *buf.add(3) = (u & 0xFF) as u8;
                *buf.add(0) &= 0x0F;
                *buf.add(0) |= 0xE0;
                return 4;
            }
            *buf.add(1) = ((u >> 24) & 0xFF) as u8;
            *buf.add(2) = ((u >> 16) & 0xFF) as u8;
            *buf.add(3) = ((u >> 8) & 0xFF) as u8;
            *buf.add(4) = (u & 0xFF) as u8;
            *buf.add(0) = if val < 0 { 0xF7 } else { 0xF0 };
        }
        5
    }

    /// Encodes `val` into the historical sparse-integer wire format.
    ///
    /// # Safety
    ///
    /// `buf` must point to writable storage for nine bytes. This includes the
    /// nine-byte (0xFF) encoding for magnitudes past the seven-byte range. The
    /// historical eight-byte (0xFE) rung, which lost the low byte, is retired
    /// on the write side; the decoder still reads 0xFE for historical data.
    #[allow(unsafe_code)]
    pub unsafe fn dump64(val: i64, buf: *mut u8) -> usize {
        // SAFETY: the historical contract is that `buf` points to writable
        // storage for at least nine bytes (the maximum encoding). Wrap it as a
        // 9-byte slice and delegate to the proven, bounds-checked free function.
        let out = unsafe { core::slice::from_raw_parts_mut(buf, 9) };
        sparseint_dump64(val, out)
    }

    /// Decodes an i32 from the historical sparse-integer wire format.
    ///
    /// # Safety
    ///
    /// `buf` must point to a complete encoded i32 value of the length selected
    /// by its first byte.
    #[allow(unsafe_code)]
    pub unsafe fn load32(buf: *const u8) -> i32 {
        unsafe {
            let bsize = SparseInt::buf_size(*buf.add(0)) as i32;
            let mut u = 0u32;
            if bsize < 5 {
                let mut i = 0i32;
                while i < bsize - 1 {
                    u |= (*buf.add((bsize - 1 - i) as usize) as u32) << (8 * (i as u32));
                    i += 1;
                }
                let mut top = *buf.add(0);
                top &= (0xFF >> bsize) as u8;
                if ((top >> (7 - bsize)) & 1) == 1 {
                    top |= ((0xFF << (7 - bsize)) & 0xFF) as u8;
                    let mut k = bsize;
                    while k < 4 {
                        u |= 0xFFu32 << (8 * (k as u32));
                        k += 1;
                    }
                }
                u |= (top as u32) << (8 * ((bsize - 1) as u32));
                return u as i32;
            }
            let mut i = 0i32;
            while i < 4 {
                u |= (*buf.add((4 - i) as usize) as u32) << (8 * (i as u32));
                i += 1;
            }
            u as i32
        }
    }

    /// Decodes an i64 from the historical sparse-integer wire format.
    ///
    /// # Safety
    ///
    /// `buf` must point to a complete encoded i64 value. Markers below `0xFE`
    /// require the length selected by the first byte; both `0xFE` and `0xFF`
    /// require nine accessible bytes because the legacy decoder reads the
    /// marker plus eight payload bytes even though `0xFE` reports length eight.
    #[allow(unsafe_code)]
    pub unsafe fn load64(buf: *const u8) -> i64 {
        // SAFETY: `buf` points to at least nine readable bytes (the reader
        // inspects up to buf[8]). Wrap as a 9-byte slice and delegate to the
        // proven, bounds-checked free function.
        let src = unsafe { core::slice::from_raw_parts(buf, 9) };
        sparseint_load64(src)
    }

    pub fn val_size(val: i64) -> usize {
        sparseint_val_size(val)
    }
}

#[repr(C)]
pub struct v32 {
    pub val_field: i32,
}

impl v32 {
    pub fn new(v: i32) -> v32 {
        v32 { val_field: v }
    }
    pub fn set(&mut self, v: i32) {
        self.val_field = v;
    }
    pub fn get(&self) -> i32 {
        self.val_field
    }
    pub fn val_size(&self) -> usize {
        SparseInt::val_size(self.val_field as i64)
    }
}

#[repr(C)]
pub struct v64 {
    pub val_field: i64,
}

impl v64 {
    pub fn new(v: i64) -> v64 {
        v64 { val_field: v }
    }
    pub fn set(&mut self, v: i64) {
        self.val_field = v;
    }
    pub fn get(&self) -> i64 {
        self.val_field
    }
    pub fn val_size(&self) -> usize {
        SparseInt::val_size(self.val_field)
    }
}

#[repr(C)]
pub struct Counter {
    pub next_field: AtomicI64,
}

impl Counter {
    pub fn new(start: i64) -> Counter {
        Counter {
            next_field: AtomicI64::new(start),
        }
    }

    pub fn peek_next(&self) -> i64 {
        self.next_field.load(Ordering::Relaxed)
    }

    pub fn next(&self, step: i64) -> i64 {
        self.next_field.fetch_add(step, Ordering::AcqRel)
    }

    pub fn reset(&self, start: i64) {
        self.next_field.store(start, Ordering::Relaxed);
    }
}

pub const SRPC_USEC_PER_SEC: u64 = 1_000_000;

pub fn abort_if_false(cond: bool) {
    if !cond {
        std::process::abort();
    }
}

pub fn time_now_us(accurate: bool) -> u64 {
    #[allow(unsafe_code)]
    unsafe {
        if accurate {
            srpc_clock_monotonic_us()
        } else {
            srpc_clock_realtime_coarse_us()
        }
    }
}

pub struct Time {}

impl Time {
    pub fn now(accurate: bool) -> u64 {
        time_now_us(accurate)
    }

    pub fn sleep(t: u64) {
        #[allow(unsafe_code)]
        unsafe {
            srpc_sleep_us(t);
        }
    }
}

#[repr(C)]
pub struct Timer {
    pub begin_us: u64,
    pub end_us: u64,
}

impl Timer {
    #[allow(clippy::new_without_default)]
    pub fn new() -> Timer {
        Timer {
            begin_us: 0,
            end_us: 0,
        }
    }

    pub fn start(&mut self) {
        #[allow(unsafe_code)]
        unsafe {
            self.begin_us = srpc_gettimeofday_us();
        }
        self.end_us = 0;
    }

    pub fn stop(&mut self) {
        #[allow(unsafe_code)]
        unsafe {
            self.end_us = srpc_gettimeofday_us();
        }
    }

    pub fn reset(&mut self) {
        self.begin_us = 0;
        self.end_us = 0;
    }

    pub fn elapsed(&self) -> f64 {
        abort_if_false(self.begin_us != 0);
        #[allow(unsafe_code)]
        let end = if self.end_us == 0 {
            unsafe { srpc_gettimeofday_us() }
        } else {
            self.end_us
        };
        (end.wrapping_sub(self.begin_us) as f64) / 1_000_000.0
    }
}
