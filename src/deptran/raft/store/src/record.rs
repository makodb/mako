//! One step's changes to the saved state, and their encoding.
//!
//! A record states outcomes, never rules (design §3): the new hard state, a
//! snapshot boundary with whether the log after it was kept, and "replace the
//! log from index i with these entries". Replay applies it whole and decides
//! nothing. Its sequence number is not stored in it: a batch stores its
//! first number and records are numbered by position.

use crate::bytes::{put_u16, put_u32, put_u64, Reader};

/// currentTerm, votedFor and commitIndex after the step.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Hard {
    pub term: u64,
    pub vote: u16,
    pub commit: u64,
}

/// A snapshot at index `index` of term `term`. `keep`: the entries after
/// `index` were kept (the local entry `index` had term `term`); otherwise
/// the whole log was dropped. `image` names the image file (P8); `None`
/// while snapshots are memory-only.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SnapRef {
    pub index: u64,
    pub term: u64,
    pub image: Option<String>,
    pub keep: bool,
}

/// One step's record. `entries[k]` is at index `replace_from + k`; with
/// `replace_from` set, the log from that index on is exactly `entries`.
#[derive(Clone, Debug, PartialEq)]
pub struct Record<P> {
    pub hard: Option<Hard>,
    pub snapshot: Option<SnapRef>,
    pub replace_from: Option<u64>,
    pub entries: Vec<(u64, P)>,
}

impl<P> Default for Record<P> {
    fn default() -> Self {
        Record { hard: None, snapshot: None, replace_from: None, entries: Vec::new() }
    }
}

impl<P> Record<P> {
    /// The log's last index after this record, given the last index before.
    pub fn last_after(&self, last: u64) -> u64 {
        let mut last = last;
        if let Some(s) = &self.snapshot {
            if !s.keep || last < s.index {
                last = s.index;
            }
        }
        if let Some(i) = self.replace_from {
            last = i + self.entries.len() as u64 - 1;
        }
        last
    }
}

/// Encodes and decodes an entry's payload. The shell's codec writes a Raft
/// command with the existing codec kernels (plan P3).
pub trait Codec<P>: Send + Sync {
    fn encode(&self, p: &P, out: &mut Vec<u8>);
    fn decode(&self, bytes: &[u8]) -> Result<P, String>;
}

/// The identity codec, for byte payloads.
pub struct BytesCodec;

impl Codec<Vec<u8>> for BytesCodec {
    fn encode(&self, p: &Vec<u8>, out: &mut Vec<u8>) {
        out.extend_from_slice(p);
    }
    fn decode(&self, bytes: &[u8]) -> Result<Vec<u8>, String> {
        Ok(bytes.to_vec())
    }
}

const HAS_HARD: u8 = 1;
const HAS_SNAPSHOT: u8 = 2;
const HAS_REPLACE: u8 = 4;
const SNAP_KEEP: u8 = 1;
const SNAP_IMAGE: u8 = 2;

/// Appends `rec`'s encoding to `out`.
pub fn encode<P>(rec: &Record<P>, codec: &dyn Codec<P>, out: &mut Vec<u8>) {
    let mut flags = 0;
    if rec.hard.is_some() {
        flags |= HAS_HARD;
    }
    if rec.snapshot.is_some() {
        flags |= HAS_SNAPSHOT;
    }
    if rec.replace_from.is_some() {
        flags |= HAS_REPLACE;
    }
    out.push(flags);
    if let Some(h) = &rec.hard {
        put_u64(out, h.term);
        put_u16(out, h.vote);
        put_u64(out, h.commit);
    }
    if let Some(s) = &rec.snapshot {
        put_u64(out, s.index);
        put_u64(out, s.term);
        let mut sf = 0;
        if s.keep {
            sf |= SNAP_KEEP;
        }
        if s.image.is_some() {
            sf |= SNAP_IMAGE;
        }
        out.push(sf);
        if let Some(name) = &s.image {
            put_u16(out, name.len() as u16);
            out.extend_from_slice(name.as_bytes());
        }
    }
    if let Some(i) = rec.replace_from {
        put_u64(out, i);
        put_u32(out, rec.entries.len() as u32);
        for (term, p) in &rec.entries {
            put_u64(out, *term);
            let at = out.len();
            put_u32(out, 0);
            codec.encode(p, out);
            let len = (out.len() - at - 4) as u32;
            out[at..at + 4].copy_from_slice(&len.to_le_bytes());
        }
    }
}

/// Decodes one record; every inconsistency is an error with a reason.
pub fn decode<P>(bytes: &[u8], codec: &dyn Codec<P>) -> Result<Record<P>, String> {
    let mut r = Reader::new(bytes);
    let flags = r.u8()?;
    if flags & !(HAS_HARD | HAS_SNAPSHOT | HAS_REPLACE) != 0 {
        return Err(format!("record: unknown flags {flags:#x}"));
    }
    let mut rec = Record::default();
    if flags & HAS_HARD != 0 {
        rec.hard = Some(Hard { term: r.u64()?, vote: r.u16()?, commit: r.u64()? });
    }
    if flags & HAS_SNAPSHOT != 0 {
        let index = r.u64()?;
        let term = r.u64()?;
        let sf = r.u8()?;
        if sf & !(SNAP_KEEP | SNAP_IMAGE) != 0 {
            return Err(format!("record: unknown snapshot flags {sf:#x}"));
        }
        let image = if sf & SNAP_IMAGE != 0 {
            let n = r.u16()? as usize;
            let s = std::str::from_utf8(r.take(n)?).map_err(|e| format!("record: image name: {e}"))?;
            Some(s.to_string())
        } else {
            None
        };
        rec.snapshot = Some(SnapRef { index, term, image, keep: sf & SNAP_KEEP != 0 });
    }
    if flags & HAS_REPLACE != 0 {
        let from = r.u64()?;
        if from == 0 {
            return Err("record: replace from index 0".into());
        }
        let n = r.u32()? as usize;
        let mut entries = Vec::with_capacity(n.min(1 << 16));
        for _ in 0..n {
            let term = r.u64()?;
            let len = r.u32()? as usize;
            entries.push((term, codec.decode(r.take(len)?)?));
        }
        rec.replace_from = Some(from);
        rec.entries = entries;
    }
    if r.remaining() != 0 {
        return Err(format!("record: {} trailing bytes", r.remaining()));
    }
    Ok(rec)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn roundtrip(rec: Record<Vec<u8>>) {
        let mut b = Vec::new();
        encode(&rec, &BytesCodec, &mut b);
        assert_eq!(decode(&b, &BytesCodec).unwrap(), rec);
        // Every strict prefix fails to decode.
        for n in 0..b.len() {
            assert!(decode(&b[..n], &BytesCodec).is_err(), "prefix {n} of {}", b.len());
        }
    }

    #[test]
    fn roundtrips() {
        roundtrip(Record::default());
        roundtrip(Record { hard: Some(Hard { term: 5, vote: 2, commit: 8 }), ..Default::default() });
        roundtrip(Record {
            hard: Some(Hard { term: 5, vote: u16::MAX, commit: 8 }),
            replace_from: Some(6),
            entries: vec![(5, b"a".to_vec()), (5, vec![]), (5, vec![0; 300])],
            ..Default::default()
        });
        roundtrip(Record {
            snapshot: Some(SnapRef { index: 100, term: 4, image: Some("100-4.img".into()), keep: true }),
            ..Default::default()
        });
        roundtrip(Record {
            hard: Some(Hard { term: 7, vote: 1, commit: 100 }),
            snapshot: Some(SnapRef { index: 100, term: 4, image: None, keep: false }),
            ..Default::default()
        });
    }

    #[test]
    fn last_after() {
        let r: Record<Vec<u8>> = Record { replace_from: Some(6), entries: vec![(1, vec![]); 3], ..Default::default() };
        assert_eq!(r.last_after(10), 8); // a cut, then an append
        let cut: Record<Vec<u8>> = Record { replace_from: Some(6), ..Default::default() };
        assert_eq!(cut.last_after(10), 5);
        let snap = |keep| Record::<Vec<u8>> { snapshot: Some(SnapRef { index: 20, term: 1, image: None, keep }), ..Default::default() };
        assert_eq!(snap(true).last_after(30), 30);
        assert_eq!(snap(false).last_after(30), 20);
        assert_eq!(snap(false).last_after(10), 20);
        assert_eq!(Record::<Vec<u8>>::default().last_after(9), 9);
    }
}
