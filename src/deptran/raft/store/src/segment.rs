//! A WAL segment's header and batch framing.
//!
//! ```text
//! segment  = header batch*
//! header   = magic "MKRFTWAL" | version u32 | format u32 | site u32
//!            | partition u32 | fingerprint u64 | first_seq u64
//!            | crc32c(previous 40 bytes) u32 | 0 u32           (48 bytes)
//! batch    = len u32 | crc32c(body) u32 | body
//! body     = first_seq u64 | count u32 | (rec_len u32 | record)*count
//! ```
//! All integers little-endian. A batch is all or nothing: one checksum covers
//! it, and the flusher writes and syncs it before writing the next.

use crate::bytes::{put_u32, put_u64, Reader};
use crate::crc::crc32c;

pub const MAGIC: &[u8; 8] = b"MKRFTWAL";
pub const VERSION: u32 = 1;
pub const HEADER_LEN: usize = 48;
const FRAME_LEN: usize = 8;

/// Which server's store this is. A segment whose header names another
/// identity fails recovery closed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Identity {
    pub site: u32,
    pub partition: u32,
    /// A fingerprint of the configured members (site ids).
    pub fingerprint: u64,
    /// The payload format (the command codec's version).
    pub format: u32,
}

/// The file name of the segment whose first record is `first_seq`.
pub fn name(first_seq: u64) -> String {
    format!("{first_seq:020}.seg")
}

/// The first sequence number a segment file name states, if it is one.
pub fn parse_name(name: &str) -> Option<u64> {
    let digits = name.strip_suffix(".seg")?;
    if digits.len() != 20 || !digits.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    digits.parse().ok()
}

pub fn header(id: &Identity, first_seq: u64) -> Vec<u8> {
    let mut h = Vec::with_capacity(HEADER_LEN);
    h.extend_from_slice(MAGIC);
    put_u32(&mut h, VERSION);
    put_u32(&mut h, id.format);
    put_u32(&mut h, id.site);
    put_u32(&mut h, id.partition);
    put_u64(&mut h, id.fingerprint);
    put_u64(&mut h, first_seq);
    let c = crc32c(&h);
    put_u32(&mut h, c);
    put_u32(&mut h, 0);
    debug_assert_eq!(h.len(), HEADER_LEN);
    h
}

/// Parses a header: `Ok((identity, first_seq))`, or why it is not one.
pub fn parse_header(b: &[u8]) -> Result<(Identity, u64), String> {
    if b.len() < HEADER_LEN {
        return Err(format!("short header ({} of {HEADER_LEN} bytes)", b.len()));
    }
    let stored = u32::from_le_bytes(b[40..44].try_into().unwrap());
    if crc32c(&b[..40]) != stored {
        return Err("header checksum mismatch".into());
    }
    let mut r = Reader::new(&b[..40]);
    if r.take(8)? != MAGIC {
        return Err("bad magic".into());
    }
    let version = r.u32()?;
    if version != VERSION {
        return Err(format!("unsupported version {version}"));
    }
    let format = r.u32()?;
    let site = r.u32()?;
    let partition = r.u32()?;
    let fingerprint = r.u64()?;
    let first = r.u64()?;
    Ok((Identity { site, partition, fingerprint, format }, first))
}

/// Frames encoded records as one batch.
pub fn frame(first_seq: u64, records: &[Vec<u8>]) -> Vec<u8> {
    let body_len = 12 + records.iter().map(|r| 4 + r.len()).sum::<usize>();
    let mut out = Vec::with_capacity(FRAME_LEN + body_len);
    put_u32(&mut out, body_len as u32);
    put_u32(&mut out, 0);
    put_u64(&mut out, first_seq);
    put_u32(&mut out, records.len() as u32);
    for r in records {
        put_u32(&mut out, r.len() as u32);
        out.extend_from_slice(r);
    }
    let c = crc32c(&out[FRAME_LEN..]);
    out[4..8].copy_from_slice(&c.to_le_bytes());
    out
}

/// A batch read back: its first sequence number and its records' bytes.
pub struct Batch<'a> {
    pub first_seq: u64,
    pub records: Vec<&'a [u8]>,
}

/// The batches of one segment's body (the bytes after its header).
pub struct Scan<'a> {
    pub batches: Vec<Batch<'a>>,
    /// Bytes of the body that hold whole, valid batches.
    pub good_len: usize,
    /// Why the body ends early, if it does: a torn last write.
    pub torn: Option<String>,
}

/// Reads a segment body. A bad batch is a torn write only if nothing but
/// zeros follows its declared end (or it runs past the end of the file):
/// the flusher syncs each batch before writing the next, so only the last
/// one can be torn. Anything else is corruption, an error.
pub fn scan(body: &[u8]) -> Result<Scan<'_>, String> {
    let mut batches = Vec::new();
    let mut pos = 0;
    while pos < body.len() {
        let rest = &body[pos..];
        let bad = match check_batch(rest) {
            Ok((batch, len)) => {
                batches.push(batch);
                pos += len;
                continue;
            }
            Err(why) => why,
        };
        // Torn if nothing but zeros follows the bad batch's declared end. A
        // length field running past the end of the file counts as torn too;
        // the residual risk, a synced batch whose length bits rot, would
        // drop the batches after it, which their checksums cannot detect.
        let declared_end = if rest.len() >= FRAME_LEN {
            FRAME_LEN.saturating_add(u32::from_le_bytes(rest[0..4].try_into().unwrap()) as usize)
        } else {
            usize::MAX
        };
        let after = if declared_end >= rest.len() { &[][..] } else { &rest[declared_end..] };
        if after.iter().all(|&b| b == 0) {
            return Ok(Scan { batches, good_len: pos, torn: Some(format!("at body offset {pos}: {bad}")) });
        }
        return Err(format!("corrupt batch at body offset {pos} with data after it: {bad}"));
    }
    Ok(Scan { batches, good_len: pos, torn: None })
}

fn check_batch(b: &[u8]) -> Result<(Batch<'_>, usize), String> {
    if b.len() < FRAME_LEN {
        return Err(format!("short frame ({} bytes)", b.len()));
    }
    let len = u32::from_le_bytes(b[0..4].try_into().unwrap()) as usize;
    let stored = u32::from_le_bytes(b[4..8].try_into().unwrap());
    if len < 12 {
        return Err(format!("batch length {len} below the minimum"));
    }
    if b.len() - FRAME_LEN < len {
        return Err(format!("batch of {len} bytes runs past the end ({} left)", b.len() - FRAME_LEN));
    }
    let body = &b[FRAME_LEN..FRAME_LEN + len];
    if crc32c(body) != stored {
        return Err("batch checksum mismatch".into());
    }
    let mut r = Reader::new(body);
    let first_seq = r.u64()?;
    let count = r.u32()? as usize;
    let mut records = Vec::with_capacity(count.min(1 << 16));
    for _ in 0..count {
        let n = r.u32()? as usize;
        records.push(r.take(n)?);
    }
    if r.remaining() != 0 {
        return Err(format!("batch has {} trailing bytes", r.remaining()));
    }
    if count == 0 {
        return Err("empty batch".into());
    }
    Ok((Batch { first_seq, records }, FRAME_LEN + len))
}

#[cfg(test)]
mod tests {
    use super::*;

    const ID: Identity = Identity { site: 3, partition: 1, fingerprint: 0xfeed, format: 1 };

    #[test]
    fn header_roundtrip_and_damage() {
        let h = header(&ID, 42);
        assert_eq!(parse_header(&h).unwrap(), (ID, 42));
        assert!(parse_header(&h[..47]).is_err());
        let mut bad = h.clone();
        bad[20] ^= 1;
        assert!(parse_header(&bad).is_err());
    }

    #[test]
    fn names() {
        assert_eq!(name(1), "00000000000000000001.seg");
        assert_eq!(parse_name(&name(77)), Some(77));
        assert_eq!(parse_name("1.seg"), None);
        assert_eq!(parse_name("0000000000000000000x.seg"), None);
    }

    #[test]
    fn torn_tail_versus_corruption() {
        let a = frame(1, &[b"one".to_vec(), b"two".to_vec()]);
        let b = frame(3, &[b"three".to_vec()]);
        let mut body = [a.clone(), b.clone()].concat();
        let s = scan(&body).unwrap();
        assert_eq!(s.batches.len(), 2);
        assert!(s.torn.is_none());
        // Every cut inside the second batch is a torn tail.
        for cut in a.len() + 1..body.len() {
            let s = scan(&body[..cut]).unwrap();
            assert_eq!((s.batches.len(), s.good_len), (1, a.len()), "cut {cut}");
            assert!(s.torn.is_some());
        }
        // Zeros after a good batch: torn (allocated, never written).
        let zeros = [a.clone(), vec![0; 100]].concat();
        assert_eq!(scan(&zeros).unwrap().good_len, a.len());
        // A damaged first batch followed by a good one: corruption.
        body[10] ^= 0xff;
        assert!(scan(&body).is_err());
    }
}
