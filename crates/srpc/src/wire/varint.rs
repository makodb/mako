//! SparseInt — the rrr wire varint codec.
//!
//! Byte-exact port of `SparseInt` in `src/rrr/base/basetypes.cpp`
//! (whose DSL block is itself Rust-syntax source; this is that code as
//! real Rust over slices instead of raw pointers).
//!
//! Layout: the first byte's tag encodes the total encoded length
//! (UTF-8 style): `0xxxxxxx`=1, `10..`=2, `110..`=3, `1110..`=4,
//! `11110..`=5, `111110..`=6, `1111110..`=7, `0xFE`=8, `0xFF`=9.
//! Payload bytes are big-endian; the value's top bits share the first
//! byte under the tag; decode sign-extends from the top payload bit.
//!
//! ## The legacy 8-length quirk (reproduced deliberately)
//!
//! `dump64` for a value whose `val_size` is 8 writes the `0xFE` marker
//! plus all **eight** payload bytes (nine bytes touched) but *returns
//! 8*, so the archive layer copies only the first eight — the lowest
//! payload byte never reaches the wire. `load64` on an `0xFE` first
//! byte reads eight payload bytes, the last of which the C++ decode
//! path leaves as the zero-initialized ninth buffer byte. The C++
//! implementation has behaved this way forever (values in that range
//! do not occur on rrr's wire in practice); byte parity requires
//! reproducing it, not fixing it.

/// Buffer large enough for any encoding (marker + 8 payload bytes).
pub const VARINT_BUF_LEN: usize = 9;

/// Total encoded length implied by the first byte.
pub fn buf_size(byte0: u8) -> usize {
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

/// Encoded length required for the value.
pub fn val_size(val: i64) -> usize {
    if (-64..=63).contains(&val) {
        1
    } else if (-8192..=8191).contains(&val) {
        2
    } else if (-1048576..=1048575).contains(&val) {
        3
    } else if (-134217728..=134217727).contains(&val) {
        4
    } else if (-17179869184..=17179869183).contains(&val) {
        5
    } else if (-2199023255552..=2199023255551).contains(&val) {
        6
    } else if (-281474976710656..=281474976710655).contains(&val) {
        7
    } else if (-36028797018963968..=36028797018963967).contains(&val) {
        8
    } else {
        9
    }
}

/// Encode `val` into `buf`; returns the number of bytes the caller
/// copies to the wire. (Not the number of bytes written for the
/// 8-length quirk case — see the module docs.)
pub fn dump32(val: i32, buf: &mut [u8]) -> usize {
    debug_assert!(buf.len() >= VARINT_BUF_LEN);
    let u = val as u32;
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
    } else if (-1048576..=1048575).contains(&val) {
        buf[0] = ((u >> 16) & 0xFF) as u8;
        buf[1] = ((u >> 8) & 0xFF) as u8;
        buf[2] = (u & 0xFF) as u8;
        buf[0] &= 0x1F;
        buf[0] |= 0xC0;
        3
    } else if (-134217728..=134217727).contains(&val) {
        buf[0] = ((u >> 24) & 0xFF) as u8;
        buf[1] = ((u >> 16) & 0xFF) as u8;
        buf[2] = ((u >> 8) & 0xFF) as u8;
        buf[3] = (u & 0xFF) as u8;
        buf[0] &= 0x0F;
        buf[0] |= 0xE0;
        4
    } else {
        buf[1] = ((u >> 24) & 0xFF) as u8;
        buf[2] = ((u >> 16) & 0xFF) as u8;
        buf[3] = ((u >> 8) & 0xFF) as u8;
        buf[4] = (u & 0xFF) as u8;
        buf[0] = if val < 0 { 0xF7 } else { 0xF0 };
        5
    }
}

/// Encode `val` into `buf`; returns the number of bytes the caller
/// copies to the wire (8-length quirk: nine bytes are written but 8
/// is returned — see the module docs).
pub fn dump64(val: i64, buf: &mut [u8]) -> usize {
    debug_assert!(buf.len() >= VARINT_BUF_LEN);
    let u = val as u64;
    let n = val_size(val);
    if n <= 7 {
        // Payload big-endian: buf[j] = byte (n-1-j) of u, then mask/tag.
        let mut j: usize = 0;
        while j < n {
            buf[j] = ((u >> (8 * ((n - 1) - j))) & 0xFF) as u8;
            j += 1;
        }
        if n == 1 {
            buf[0] &= 0x7F;
        } else if n == 2 {
            buf[0] &= 0x3F;
            buf[0] |= 0x80;
        } else if n == 3 {
            buf[0] &= 0x1F;
            buf[0] |= 0xC0;
        } else if n == 4 {
            buf[0] &= 0x0F;
            buf[0] |= 0xE0;
        } else if n == 5 {
            buf[0] &= 0x07;
            buf[0] |= 0xF0;
        } else if n == 6 {
            buf[0] &= 0x03;
            buf[0] |= 0xF8;
        } else {
            buf[0] &= 0x01;
            buf[0] |= 0xFC;
        }
        return n;
    }
    // n == 8 or 9: marker byte + all eight payload bytes at buf[1..=8]
    // (legacy quirk: n == 8 also writes nine bytes but reports 8).
    let mut j: usize = 0;
    while j < 8 {
        buf[1 + j] = ((u >> (8 * (7 - j))) & 0xFF) as u8;
        j += 1;
    }
    if n == 8 {
        buf[0] = 0xFE;
        return 8;
    }
    buf[0] = 0xFF;
    9
}

/// Decode an i32 from `buf` (first byte determines the length; the
/// buffer must hold at least [`VARINT_BUF_LEN`] bytes, zero-padded
/// past the encoded length like the C++ decode path's buffer).
/// Slice params (not `&[u8; N]`): the idiomatic Rust surface, and the
/// uniform lowering shape for the C++ translation (rusty-cpp #44).
pub fn load32(buf: &[u8]) -> i32 {
    debug_assert!(buf.len() >= VARINT_BUF_LEN);
    let bsize = buf_size(buf[0]);
    let mut u: u32 = 0;
    if bsize < 5 {
        let mut i: usize = 0;
        while i < bsize - 1 {
            u |= (buf[(bsize - 1) - i] as u32) << (8 * i);
            i += 1;
        }
        let mut top = buf[0];
        top &= (0xFFu32 >> bsize) as u8;
        if ((top >> (7 - bsize)) & 0x1) == 1 {
            top |= ((0xFFu32 << (7 - bsize)) & 0xFF) as u8;
            let mut k = bsize;
            while k < 4 {
                u |= 0xFFu32 << (8 * k);
                k += 1;
            }
        }
        u |= (top as u32) << (8 * (bsize - 1));
        return u as i32;
    }
    let mut i: usize = 0;
    while i < 4 {
        u |= (buf[4 - i] as u32) << (8 * i);
        i += 1;
    }
    u as i32
}

/// Decode an i64 from `buf` (same buffer contract as [`load32`]).
pub fn load64(buf: &[u8]) -> i64 {
    debug_assert!(buf.len() >= VARINT_BUF_LEN);
    let bsize = buf_size(buf[0]);
    let mut u: u64 = 0;
    if bsize < 8 {
        let mut i: usize = 0;
        while i < bsize - 1 {
            u |= (buf[(bsize - 1) - i] as u64) << (8 * i);
            i += 1;
        }
        let mut top = buf[0];
        top &= (0xFFu32 >> bsize) as u8;
        if ((top >> (7 - bsize)) & 0x1) == 1 {
            top |= ((0xFFu32 << (7 - bsize)) & 0xFF) as u8;
            let mut k = bsize;
            while k < 8 {
                u |= 0xFFu64 << (8 * k);
                k += 1;
            }
        }
        u |= (top as u64) << (8 * (bsize - 1));
        return u as i64;
    }
    let mut i: usize = 0;
    while i < 8 {
        u |= (buf[8 - i] as u64) << (8 * i);
        i += 1;
    }
    u as i64
}

#[cfg(test)]
mod tests {
    use super::*;

    fn roundtrip32(val: i32) {
        let mut buf = [0u8; VARINT_BUF_LEN];
        let n = dump32(val, &mut buf);
        assert_eq!(n, val_size(val as i64), "size for {val}");
        assert_eq!(n, buf_size(buf[0]), "tag/len agree for {val}");
        assert_eq!(load32(&buf), val, "roundtrip {val}");
    }

    fn roundtrip64(val: i64) {
        let mut buf = [0u8; VARINT_BUF_LEN];
        let n = dump64(val, &mut buf);
        let vs = val_size(val);
        if vs == 8 {
            // Quirk case: reported 8, wrote 9 (see module docs).
            assert_eq!(n, 8, "quirk reported size for {val}");
            return;
        }
        assert_eq!(n, vs, "size for {val}");
        assert_eq!(n, buf_size(buf[0]), "tag/len agree for {val}");
        assert_eq!(load64(&buf), val, "roundtrip {val}");
    }

    #[test]
    fn boundaries_32() {
        let cases = [
            0,
            1,
            -1,
            63,
            64,
            -64,
            -65,
            8191,
            8192,
            -8192,
            -8193,
            1048575,
            1048576,
            -1048576,
            -1048577,
            134217727,
            134217728,
            -134217728,
            -134217729,
            i32::MAX,
            i32::MIN,
        ];
        let mut i = 0;
        while i < cases.len() {
            roundtrip32(cases[i]);
            i += 1;
        }
    }

    #[test]
    fn boundaries_64() {
        let cases: [i64; 20] = [
            0,
            1,
            -1,
            63,
            -64,
            8191,
            -8192,
            1048575,
            -1048576,
            134217727,
            -134217728,
            17179869183,
            -17179869184,
            2199023255551,
            -2199023255552,
            281474976710655,
            -281474976710656,
            36028797018963967, // val_size 8 — quirk case, encode-only
            i64::MAX,
            i64::MIN,
        ];
        let mut i = 0;
        while i < cases.len() {
            roundtrip64(cases[i]);
            i += 1;
        }
    }

    #[test]
    fn known_encodings() {
        // Hand-derived from the layout (locked against the golden
        // corpus from the C++ implementation as well).
        let mut buf = [0u8; VARINT_BUF_LEN];

        assert_eq!(dump32(0, &mut buf), 1);
        assert_eq!(buf[0], 0x00);

        assert_eq!(dump32(1, &mut buf), 1);
        assert_eq!(buf[0], 0x01);

        // -1 = 0x7F under the 1-byte tag (sign-extends back).
        assert_eq!(dump32(-1, &mut buf), 1);
        assert_eq!(buf[0], 0x7F);

        // 64 needs 2 bytes: tag 10, payload big-endian 0x00 0x40.
        assert_eq!(dump32(64, &mut buf), 2);
        assert_eq!(&buf[..2], &[0x80, 0x40]);

        // i32::MAX: 5-byte form, positive marker 0xF0.
        assert_eq!(dump32(i32::MAX, &mut buf), 5);
        assert_eq!(&buf[..5], &[0xF0, 0x7F, 0xFF, 0xFF, 0xFF]);

        // i32::MIN: 5-byte form, negative marker 0xF7.
        assert_eq!(dump32(i32::MIN, &mut buf), 5);
        assert_eq!(&buf[..5], &[0xF7, 0x80, 0x00, 0x00, 0x00]);

        // i64::MIN: 9-byte form, marker 0xFF.
        assert_eq!(dump64(i64::MIN, &mut buf), 9);
        assert_eq!(
            &buf[..9],
            &[0xFF, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]
        );
    }
}
