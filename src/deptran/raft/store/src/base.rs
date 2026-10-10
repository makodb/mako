//! The base: the saved state as of WAL record c (design §3 "The base and the
//! applier", §5; plan P7).
//!
//! The applier folds durable records into it in one atomic batch each, c
//! included, so the base is always the replay of records 1..=c. A checkpoint
//! makes it durable (a waiting flush) and then deletes the WAL segments it
//! covers. RocksDB is the production base (rocks.rs, its own WAL off: ours is
//! the log above it); [`MemBase`] models one for the tests, dropping what
//! was not flushed when it crashes.
//!
//! Keys: `h` hard state, `s` snapshot, `c` the last record folded in, `i` the
//! store's identity, `e<index, big-endian>` an entry (its term, then the
//! payload's codec bytes).

use std::collections::BTreeMap;
use std::io;
use std::sync::{Arc, Mutex};

use crate::bytes::{put_u16, put_u32, put_u64, Reader};
use crate::record::{Hard, Record, SnapRef};
use crate::segment::Identity;
use crate::state::SavedState;

/// One operation of an atomic batch.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Op {
    Put(Vec<u8>, Vec<u8>),
    /// Deletes keys in [from, to).
    DeleteRange(Vec<u8>, Vec<u8>),
}

/// A key-value store with atomic batches and a waiting flush.
pub trait Base: Send {
    /// Applies the batch atomically (all or nothing at a crash).
    fn write(&mut self, ops: &[Op]) -> io::Result<()>;
    /// Makes every write so far durable.
    fn flush(&mut self) -> io::Result<()>;
    fn get(&self, key: &[u8]) -> io::Result<Option<Vec<u8>>>;
    /// Every key starting with `prefix`, in key order.
    fn scan(&self, prefix: &[u8]) -> io::Result<Vec<(Vec<u8>, Vec<u8>)>>;
}

pub const KEY_HARD: &[u8] = b"h";
pub const KEY_SNAP: &[u8] = b"s";
pub const KEY_C: &[u8] = b"c";
pub const KEY_ID: &[u8] = b"i";

pub fn entry_key(index: u64) -> Vec<u8> {
    let mut k = Vec::with_capacity(9);
    k.push(b'e');
    k.extend_from_slice(&index.to_be_bytes());
    k
}

fn u64_bytes(v: u64) -> Vec<u8> {
    v.to_le_bytes().to_vec()
}

pub fn identity_bytes(id: &Identity) -> Vec<u8> {
    let mut b = Vec::new();
    put_u32(&mut b, id.site);
    put_u32(&mut b, id.partition);
    put_u64(&mut b, id.fingerprint);
    put_u32(&mut b, id.format);
    b
}

/// The batch that folds `rec` (numbered `seq`) into the base: the same
/// outcome `SavedState::apply` gives, as puts and range deletes, then c.
/// Records state values at fixed keys, so folding one twice is harmless.
///
/// `top` bounds the highest entry index the base may hold (`u64::MAX`:
/// unknown) and is kept up to date. A record that replaces from past it only
/// appends and gets no range delete: an append writes one, so a saturated
/// run put hundreds of thousands of overlapping tombstones in each memtable,
/// whose flush RocksDB fragments in time superlinear in their number (2
/// minutes of a flush thread's CPU at a G2 shutdown; 1-2 s write stalls).
pub fn ops_for(rec: &Record<Vec<u8>>, seq: u64, top: &mut u64, ops: &mut Vec<Op>) {
    if let Some(h) = &rec.hard {
        let mut v = Vec::new();
        put_u64(&mut v, h.term);
        put_u16(&mut v, h.vote);
        put_u64(&mut v, h.commit);
        ops.push(Op::Put(KEY_HARD.to_vec(), v));
    }
    if let Some(s) = &rec.snapshot {
        let mut v = Vec::new();
        put_u64(&mut v, s.index);
        put_u64(&mut v, s.term);
        v.push(u8::from(s.keep));
        let name = s.image.clone().unwrap_or_default();
        put_u16(&mut v, name.len() as u16);
        v.extend_from_slice(name.as_bytes());
        ops.push(Op::Put(KEY_SNAP.to_vec(), v));
        ops.push(Op::DeleteRange(entry_key(0), entry_key(s.index + 1)));
        if !s.keep {
            ops.push(Op::DeleteRange(entry_key(s.index + 1), entry_key(u64::MAX)));
            *top = s.index;
        }
    }
    if let Some(from) = rec.replace_from {
        if from <= *top {
            ops.push(Op::DeleteRange(entry_key(from), entry_key(u64::MAX)));
        }
        *top = match rec.entries.len() as u64 {
            0 => (*top).min(from.saturating_sub(1)),
            n => from + n - 1,
        };
        for (k, (term, payload)) in rec.entries.iter().enumerate() {
            let mut v = Vec::with_capacity(8 + payload.len());
            put_u64(&mut v, *term);
            v.extend_from_slice(payload);
            ops.push(Op::Put(entry_key(from + k as u64), v));
        }
    }
    ops.push(Op::Put(KEY_C.to_vec(), u64_bytes(seq)));
}

/// Reads the base: (c, the saved state with payloads as codec bytes). Fails
/// closed on another server's base or a log with a gap.
pub fn load(base: &dyn Base, id: &Identity, no_vote: u16) -> Result<(u64, SavedState<Vec<u8>>), String> {
    let e = |e: io::Error| format!("base: {e}");
    match base.get(KEY_ID).map_err(e)? {
        Some(b) if b == identity_bytes(id) => {}
        Some(_) => return Err(format!("base: belongs to another server, not {id:?}")),
        None => return Err("base: no identity (not a store's base)".into()),
    }
    let c = match base.get(KEY_C).map_err(e)? {
        Some(b) if b.len() == 8 => u64::from_le_bytes(b.try_into().unwrap()),
        _ => return Err("base: no record number".into()),
    };
    let mut state = SavedState::new(no_vote);
    if let Some(b) = base.get(KEY_HARD).map_err(e)? {
        let mut r = Reader::new(&b);
        state.hard = Hard { term: r.u64()?, vote: r.u16()?, commit: r.u64()? };
    }
    if let Some(b) = base.get(KEY_SNAP).map_err(e)? {
        let mut r = Reader::new(&b);
        let index = r.u64()?;
        let term = r.u64()?;
        let _keep = r.u8()?;
        let n = r.u16()? as usize;
        let name = String::from_utf8(r.take(n)?.to_vec()).map_err(|e| format!("base: image name: {e}"))?;
        let snap = SnapRef { index, term, image: if name.is_empty() { None } else { Some(name) }, keep: true };
        state.snap_index = snap.index;
        state.snap_term = snap.term;
        state.image = snap.image;
    }
    for (expect, (k, v)) in (state.snap_index + 1..).zip(base.scan(b"e").map_err(e)?) {
        if k.len() != 9 {
            return Err("base: malformed entry key".into());
        }
        let index = u64::from_be_bytes(k[1..9].try_into().unwrap());
        if index != expect {
            return Err(format!("base: entry {index} where {expect} was expected"));
        }
        let mut r = Reader::new(&v);
        let term = r.u64()?;
        state.entries.push((term, v[8..].to_vec()));
    }
    Ok((c, state))
}

// ------------------------------------------------------------------ MemBase

#[derive(Default)]
struct MemInner {
    current: BTreeMap<Vec<u8>, Vec<u8>>,
    durable: BTreeMap<Vec<u8>, Vec<u8>>,
    /// Batches allowed before the simulated failure (tests).
    budget: Option<u64>,
    failed: bool,
}

/// An in-memory base for the tests. Clones share one store; `crash` keeps
/// only what the last flush made durable.
#[derive(Clone, Default)]
pub struct MemBase {
    inner: Arc<Mutex<MemInner>>,
}

impl MemBase {
    pub fn new() -> Self {
        Self::default()
    }

    /// A power cut: unflushed batches are gone.
    pub fn crash(&self) {
        let mut g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        g.current = g.durable.clone();
        g.failed = false;
        g.budget = None;
    }

    /// Lets `n` more writes or flushes succeed; then every one fails.
    pub fn set_budget(&self, n: Option<u64>) {
        self.inner.lock().unwrap_or_else(|e| e.into_inner()).budget = n;
    }

    fn spend(g: &mut MemInner) -> io::Result<()> {
        if g.failed {
            return Err(io::Error::other("MemBase: failed"));
        }
        if let Some(b) = g.budget.as_mut() {
            if *b == 0 {
                g.failed = true;
                return Err(io::Error::other("MemBase: failed"));
            }
            *b -= 1;
        }
        Ok(())
    }
}

impl Base for MemBase {
    fn write(&mut self, ops: &[Op]) -> io::Result<()> {
        let mut g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        Self::spend(&mut g)?;
        for op in ops {
            match op {
                Op::Put(k, v) => {
                    g.current.insert(k.clone(), v.clone());
                }
                Op::DeleteRange(from, to) => {
                    let keys: Vec<Vec<u8>> = g.current.range(from.clone()..to.clone()).map(|(k, _)| k.clone()).collect();
                    for k in keys {
                        g.current.remove(&k);
                    }
                }
            }
        }
        Ok(())
    }

    fn flush(&mut self) -> io::Result<()> {
        let mut g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        Self::spend(&mut g)?;
        g.durable = g.current.clone();
        Ok(())
    }

    fn get(&self, key: &[u8]) -> io::Result<Option<Vec<u8>>> {
        Ok(self.inner.lock().unwrap_or_else(|e| e.into_inner()).current.get(key).cloned())
    }

    fn scan(&self, prefix: &[u8]) -> io::Result<Vec<(Vec<u8>, Vec<u8>)>> {
        let g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        Ok(g.current.range(prefix.to_vec()..).take_while(|(k, _)| k.starts_with(prefix))
            .map(|(k, v)| (k.clone(), v.clone())).collect())
    }
}
