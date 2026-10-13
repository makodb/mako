//! CRC32C (Castagnoli), the checksum of segment headers and WAL batches.
//!
//! SSE4.2's `crc32` instruction when the CPU has it (about 0.13 us/KiB on
//! zoo-003), else a slice-by-8 table (about 0.65 us/KiB). The plan's cost
//! model (plan §5) shows the table version costs G2 a further 8%, hence the
//! hardware path.

use std::sync::OnceLock;

const POLY: u32 = 0x82F6_3B78; // reflected Castagnoli polynomial

/// The CRC32C of `data`.
pub fn crc32c(data: &[u8]) -> u32 {
    crc32c_append(0, data)
}

/// Extends `crc` (a previous result, or 0) over `data`.
pub fn crc32c_append(crc: u32, data: &[u8]) -> u32 {
    #[cfg(target_arch = "x86_64")]
    {
        if std::is_x86_feature_detected!("sse4.2") {
            // SAFETY: the CPU supports SSE4.2, checked just above.
            return unsafe { hw(crc, data) };
        }
    }
    sw(crc, data)
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "sse4.2")]
unsafe fn hw(crc: u32, data: &[u8]) -> u32 {
    use std::arch::x86_64::{_mm_crc32_u64, _mm_crc32_u8};
    let mut c = u64::from(!crc);
    let mut chunks = data.chunks_exact(8);
    for ch in &mut chunks {
        c = _mm_crc32_u64(c, u64::from_le_bytes(ch.try_into().unwrap()));
    }
    let mut c32 = c as u32;
    for &b in chunks.remainder() {
        c32 = _mm_crc32_u8(c32, b);
    }
    !c32
}

fn tables() -> &'static [[u32; 256]; 8] {
    static T: OnceLock<[[u32; 256]; 8]> = OnceLock::new();
    T.get_or_init(|| {
        let mut t = [[0u32; 256]; 8];
        for i in 0..256u32 {
            let mut c = i;
            for _ in 0..8 {
                c = if c & 1 != 0 { (c >> 1) ^ POLY } else { c >> 1 };
            }
            t[0][i as usize] = c;
        }
        for k in 1..8 {
            for i in 0..256 {
                let p = t[k - 1][i];
                t[k][i] = (p >> 8) ^ t[0][(p & 0xff) as usize];
            }
        }
        t
    })
}

/// The portable slice-by-8 version; public for the tests that compare it
/// with the hardware path.
pub fn sw(crc: u32, data: &[u8]) -> u32 {
    let t = tables();
    let mut c = !crc;
    let mut chunks = data.chunks_exact(8);
    for ch in &mut chunks {
        let lo = u32::from_le_bytes(ch[0..4].try_into().unwrap()) ^ c;
        let hi = u32::from_le_bytes(ch[4..8].try_into().unwrap());
        c = t[7][(lo & 0xff) as usize]
            ^ t[6][((lo >> 8) & 0xff) as usize]
            ^ t[5][((lo >> 16) & 0xff) as usize]
            ^ t[4][(lo >> 24) as usize]
            ^ t[3][(hi & 0xff) as usize]
            ^ t[2][((hi >> 8) & 0xff) as usize]
            ^ t[1][((hi >> 16) & 0xff) as usize]
            ^ t[0][(hi >> 24) as usize];
    }
    for &b in chunks.remainder() {
        c = t[0][((c ^ u32::from(b)) & 0xff) as usize] ^ (c >> 8);
    }
    !c
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn check_value() {
        // The standard CRC32C check value.
        assert_eq!(crc32c(b"123456789"), 0xE306_9283);
        assert_eq!(sw(0, b"123456789"), 0xE306_9283);
    }

    #[test]
    fn hardware_matches_table() {
        let data: Vec<u8> = (0..10_000u32).map(|i| (i * 7 + i / 3) as u8).collect();
        for len in [0, 1, 7, 8, 9, 63, 64, 65, 1000, 10_000] {
            assert_eq!(crc32c(&data[..len]), sw(0, &data[..len]), "len {len}");
        }
        // Appending equals one pass.
        let (a, b) = data.split_at(777);
        assert_eq!(crc32c_append(crc32c(a), b), crc32c(&data));
    }
}
