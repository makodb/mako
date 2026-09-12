//! In-memory stand-ins for the two backends.
//!
//! These exist so the entire cache — write path, flusher, watermark,
//! eviction, barrier — can be tested with no C++, no RocksDB, and no
//! `unsafe`. They are also the substrate for mutation testing: a defect
//! injected into the cache must be caught here, not only against the real
//! backends where scheduling noise hides things.
//!
//! [`MemBlobs`] additionally models the two failure modes the durable
//! store really has: refusing writes, and refusing them for a bounded
//! number of attempts and then recovering.

use std::collections::BTreeMap;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::{Mutex, RwLock};

use crate::{BlobError, BlobOp, Blobs, EntryWord, KeyIndex};

/// A `BTreeMap`-backed [`KeyIndex`].
#[derive(Debug, Default)]
pub struct MemIndex {
    map: RwLock<BTreeMap<Vec<u8>, EntryWord>>,
}

impl MemIndex {
    /// An empty index.
    pub fn new() -> Self {
        Self::default()
    }
}

impl KeyIndex for MemIndex {
    fn get(&self, key: &[u8]) -> Option<EntryWord> {
        self.map.read().expect("index poisoned").get(key).copied()
    }

    fn get_or_insert(&self, key: &[u8], word: EntryWord) -> EntryWord {
        let mut m = self.map.write().expect("index poisoned");
        *m.entry(key.to_vec()).or_insert(word)
    }

    fn scan_chunk(&self, from: &[u8], budget: usize, out: &mut Vec<(Vec<u8>, EntryWord)>) -> usize {
        let m = self.map.read().expect("index poisoned");
        let mut n = 0;
        for (k, v) in m.range(from.to_vec()..) {
            if n >= budget {
                break;
            }
            out.push((k.clone(), *v));
            n += 1;
        }
        n
    }

    fn rscan_chunk(
        &self,
        from: &[u8],
        budget: usize,
        out: &mut Vec<(Vec<u8>, EntryWord)>,
    ) -> usize {
        let m = self.map.read().expect("index poisoned");
        let mut n = 0;
        for (k, v) in m.range(..=from.to_vec()).rev() {
            if n >= budget {
                break;
            }
            out.push((k.clone(), *v));
            n += 1;
        }
        n
    }

    fn len(&self) -> usize {
        self.map.read().expect("index poisoned").len()
    }
}

/// A `BTreeMap`-backed [`Blobs`] with fault injection and bounded iteration.
#[derive(Debug, Default)]
pub struct MemBlobs {
    rows: Mutex<BTreeMap<Vec<u8>, Vec<u8>>>,
    /// Remaining write attempts to fail. `usize::MAX` means "fail
    /// forever".
    fail_writes: AtomicUsize,
    /// Batches successfully applied, for asserting the flusher coalesces.
    batches: AtomicU64,
    /// Individual operations applied.
    ops: AtomicU64,
    /// Microseconds to stall inside each successful batch.
    ///
    /// Real durable stores are slow, and several properties are only
    /// *observable* when a backlog can exist. Without this, a test that
    /// shuts down and asserts everything is durable passes even with the
    /// shutdown barrier removed, simply because an in-memory insert
    /// outruns the race — a mutation this crate's suite let survive until
    /// this knob existed.
    write_delay_us: AtomicU64,
}

impl MemBlobs {
    /// An empty store.
    pub fn new() -> Self {
        Self::default()
    }

    /// Pre-populate, as if recovering an existing database.
    pub fn seeded(rows: impl IntoIterator<Item = (Vec<u8>, Vec<u8>)>) -> Self {
        let s = Self::default();
        {
            let mut m = s.rows.lock().expect("rows poisoned");
            for (k, v) in rows {
                m.insert(k, v);
            }
        }
        s
    }

    /// Fail the next `n` write batches.
    pub fn fail_next_writes(&self, n: usize) {
        self.fail_writes.store(n, Ordering::SeqCst);
    }

    /// Stall this long inside every successful batch.
    pub fn set_write_delay_us(&self, us: u64) {
        self.write_delay_us.store(us, Ordering::SeqCst);
    }

    /// Whether writes are currently being refused.
    pub fn is_failing(&self) -> bool {
        self.fail_writes.load(Ordering::SeqCst) > 0
    }

    /// Batches applied so far.
    pub fn batch_count(&self) -> u64 {
        self.batches.load(Ordering::SeqCst)
    }

    /// Operations applied so far.
    pub fn op_count(&self) -> u64 {
        self.ops.load(Ordering::SeqCst)
    }

    /// Read directly, bypassing the cache — the durable ground truth.
    pub fn peek(&self, key: &[u8]) -> Option<Vec<u8>> {
        self.rows.lock().expect("rows poisoned").get(key).cloned()
    }

    /// Every durable row, for whole-store comparisons.
    pub fn snapshot(&self) -> BTreeMap<Vec<u8>, Vec<u8>> {
        self.rows
            .lock()
            .expect("rows poisoned")
            .iter()
            .map(|(k, v)| (k.clone(), v.clone()))
            .collect()
    }
}

impl Blobs for MemBlobs {
    fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
        Ok(self.rows.lock().expect("rows poisoned").get(key).cloned())
    }

    fn write_batch(&self, ops: &[BlobOp<'_>]) -> Result<(), BlobError> {
        loop {
            let n = self.fail_writes.load(Ordering::SeqCst);
            if n == 0 {
                break;
            }
            let next = if n == usize::MAX { n } else { n - 1 };
            if self
                .fail_writes
                .compare_exchange(n, next, Ordering::SeqCst, Ordering::SeqCst)
                .is_ok()
            {
                return Err(BlobError("injected write failure".into()));
            }
        }
        let delay = self.write_delay_us.load(Ordering::Relaxed);
        if delay > 0 {
            std::thread::sleep(std::time::Duration::from_micros(delay));
        }
        // All-or-nothing: the map is only touched after the decision to
        // succeed, so a failed batch leaves no partial state.
        let mut m = self.rows.lock().expect("rows poisoned");
        for op in ops {
            match op {
                BlobOp::Put { key, val } => {
                    m.insert(key.to_vec(), val.to_vec());
                }
                BlobOp::Delete { key } => {
                    m.remove(*key);
                }
            }
        }
        self.batches.fetch_add(1, Ordering::SeqCst);
        self.ops.fetch_add(ops.len() as u64, Ordering::SeqCst);
        Ok(())
    }

    fn for_each_key(&self, f: &mut dyn FnMut(&[u8])) -> Result<(), BlobError> {
        self.for_each_entry(&mut |key, _| {
            f(key);
            Ok(())
        })
    }

    fn for_each_entry(
        &self,
        f: &mut dyn FnMut(&[u8], &[u8]) -> Result<(), BlobError>,
    ) -> Result<(), BlobError> {
        self.for_each_entry_from(b"", usize::MAX, &mut |key, value| {
            f(key, value)?;
            Ok(true)
        })
    }

    fn for_each_entry_from(
        &self,
        start: &[u8],
        max_entries: usize,
        visitor: &mut dyn FnMut(&[u8], &[u8]) -> Result<bool, BlobError>,
    ) -> Result<(), BlobError> {
        use std::ops::Bound::{Excluded, Included, Unbounded};
        let mut previous: Option<Vec<u8>> = None;
        for _ in 0..max_entries {
            // Copy one row and release the lock before invoking caller code.
            // Recovery may read another backend key in its callback. Holding
            // this mutex there would deadlock; a panic would also poison it.
            let entry = {
                let rows = self.rows.lock().expect("rows poisoned");
                let next = match &previous {
                    Some(key) => rows
                        .range::<[u8], _>((Excluded(key.as_slice()), Unbounded))
                        .next(),
                    None => rows.range::<[u8], _>((Included(start), Unbounded)).next(),
                };
                next.map(|(key, value)| (key.clone(), value.clone()))
            };
            let Some((key, value)) = entry else {
                return Ok(());
            };
            if !visitor(&key, &value)? {
                return Ok(());
            }
            previous = Some(key);
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bounded_entry_seek_is_inclusive_reentrant_and_stops_cleanly() {
        let backend = MemBlobs::seeded([
            (b"".to_vec(), b"".to_vec()),
            (b"a\0".to_vec(), b"first".to_vec()),
            (b"a\xff".to_vec(), b"second".to_vec()),
            (b"b".to_vec(), b"last".to_vec()),
        ]);
        backend
            .for_each_entry_from(b"", 0, &mut |_, _| panic!("zero limit"))
            .unwrap();
        let mut rows = Vec::new();
        backend
            .for_each_entry_from(b"a\0", 2, &mut |key, value| {
                assert_eq!(backend.get(key)?.as_deref(), Some(value));
                rows.push(key.to_vec());
                Ok(true)
            })
            .unwrap();
        assert_eq!(rows, vec![b"a\0".to_vec(), b"a\xff".to_vec()]);
        rows.clear();
        backend
            .for_each_entry_from(b"a\0\0", 9, &mut |key, _| {
                rows.push(key.to_vec());
                Ok(false)
            })
            .unwrap();
        assert_eq!(rows, vec![b"a\xff".to_vec()]);
        backend
            .for_each_entry_from(b"z", 1, &mut |_, _| panic!("past the end"))
            .unwrap();
        let panic = std::panic::catch_unwind(|| {
            let _ = backend.for_each_entry_from(b"a", 1, &mut |_, _| panic!("visitor"));
        });
        assert!(panic.is_err());
        assert_eq!(
            backend.get(b"b").unwrap().as_deref(),
            Some(b"last".as_slice())
        );
    }

    #[test]
    fn entry_visitor_is_ordered_reentrant_and_stops_on_error() {
        let backend = std::sync::Arc::new(MemBlobs::seeded([
            (b"b".to_vec(), b"value".to_vec()),
            (b"".to_vec(), b"".to_vec()),
            (b"a\0".to_vec(), b"\0\xff".to_vec()),
        ]));
        let mut observed = Vec::new();
        backend
            .for_each_entry(&mut |key, value| {
                assert_eq!(backend.get(key)?.as_deref(), Some(value));
                observed.push((key.to_vec(), value.to_vec()));
                Ok(())
            })
            .unwrap();
        assert_eq!(observed, backend.snapshot().into_iter().collect::<Vec<_>>());
        let mut visits = 0;
        let error = backend
            .for_each_entry(&mut |_, _| {
                visits += 1;
                Err(BlobError("stop here".into()))
            })
            .unwrap_err();
        assert_eq!(visits, 1);
        assert_eq!(error.0, "stop here");
        let unwind = std::panic::catch_unwind(|| {
            let _ = backend.for_each_entry(&mut |_, _| panic!("visitor panic"));
        });
        assert!(unwind.is_err());
        assert_eq!(
            backend.get(b"b").unwrap().as_deref(),
            Some(b"value".as_slice())
        );
    }

    #[test]
    fn a_failed_batch_leaves_nothing_behind() {
        let b = MemBlobs::new();
        b.fail_next_writes(1);
        let err = b.write_batch(&[BlobOp::Put {
            key: b"k",
            val: b"v",
        }]);
        assert!(err.is_err());
        assert_eq!(b.peek(b"k"), None, "all-or-nothing per batch");
        assert!(b
            .write_batch(&[BlobOp::Put {
                key: b"k",
                val: b"v"
            }])
            .is_ok());
        assert_eq!(b.peek(b"k").as_deref(), Some(&b"v"[..]));
    }

    #[test]
    fn scan_chunk_is_ordered_and_budgeted() {
        let i = MemIndex::new();
        for n in 0..10u8 {
            i.get_or_insert(&[b'k', n], n as u64 + 1);
        }
        let mut out = Vec::new();
        assert_eq!(i.scan_chunk(b"", 3, &mut out), 3);
        assert_eq!(out.len(), 3);
        assert_eq!(out[0].0, vec![b'k', 0]);
        assert_eq!(out[2].0, vec![b'k', 2]);
    }

    #[test]
    fn rscan_chunk_walks_backwards_from_the_bound() {
        let i = MemIndex::new();
        for n in 0..5u8 {
            i.get_or_insert(&[b'k', n], n as u64 + 1);
        }
        let mut out = Vec::new();
        i.rscan_chunk(&[b'k', 3], 2, &mut out);
        assert_eq!(out[0].0, vec![b'k', 3], "inclusive upper bound");
        assert_eq!(out[1].0, vec![b'k', 2]);
    }

    #[test]
    fn get_or_insert_reports_the_winner() {
        let i = MemIndex::new();
        assert_eq!(i.get_or_insert(b"k", 7), 7);
        assert_eq!(i.get_or_insert(b"k", 9), 7, "the first writer wins");
    }
}
