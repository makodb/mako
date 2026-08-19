//! The entry table: append-only, lock-free to read, and never moves.
//!
//! # Why this is not a `RwLock<Vec<Arc<Entry>>>`
//!
//! It was, and that cost about three quarters of the cache's throughput
//! at 16 threads. Measured, not guessed: single-threaded the Rust cache
//! ran at 0.90–1.08x of the C++ one, and at 16 threads it fell to
//! 0.24–0.36x. A ratio that collapses with thread count and does so on
//! the **read** path is contention, and the read path's only shared
//! mutable state was this table.
//!
//! Every operation, read or write, did two things here that scale
//! badly:
//!
//! * took a read lock on one `RwLock` — a shared atomic read-modify-write
//!   on a single cache line, bounced between all 16 cores;
//! * cloned an `Arc<Entry>` out of it, then dropped it — two more atomic
//!   RMWs, and for a hot key on the *same* line across every thread.
//!
//! Both are gone. Lookup is now two relaxed atomic loads and an index,
//! with no lock and no reference counting, which is what the C++
//! implementation gets by storing a raw pointer to a leaked entry.
//!
//! # How
//!
//! Segments that double in size, allocated once and never freed or
//! moved, so a `&Entry` handed out today stays valid for the life of the
//! store. That is what makes borrowing sound without `unsafe`: entries
//! are immortal by design (the flusher's dirty map holds entry indices
//! across cycles, and `clear` deliberately does not stop the flusher), so
//! there is no reclamation to be raced.
//!
//! Doubling rather than fixed-size segments keeps the outer array tiny —
//! 32 slots reach past `u32::MAX` entries — where fixed 4096-entry
//! segments would need a 256 KiB outer array per store, allocated at open
//! whether or not a single key is ever written.

use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::OnceLock;

use crate::value::Entry;

/// Entries in segment 0. Later segments double.
const BASE: u32 = 1024;
const BASE_SHIFT: u32 = 10;

/// Enough to index every `u32`: segment 22 ends at `1024 * 2^22` > 2^32.
const MAX_SEGMENTS: usize = 32;

/// One segment: a fixed run of slots, each filled at most once.
type Segment = Box<[OnceLock<Entry>]>;

/// An append-only table of immortal entries.
pub struct EntryTable {
    segments: Box<[OnceLock<Segment>]>,
    next: AtomicU32,
}

impl Default for EntryTable {
    fn default() -> Self {
        Self::new()
    }
}

/// Which segment an index falls in, and where within it.
#[inline]
fn locate(idx: u32) -> (usize, usize) {
    if idx < BASE {
        return (0, idx as usize);
    }
    // Segment s covers [BASE << (s-1), BASE << s).
    let s = (u32::BITS - (idx >> BASE_SHIFT).leading_zeros()) as usize;
    let start = BASE << (s - 1);
    (s, (idx - start) as usize)
}

#[inline]
fn segment_len(s: usize) -> usize {
    if s == 0 {
        BASE as usize
    } else {
        (BASE as usize) << (s - 1)
    }
}

impl EntryTable {
    /// An empty table. Allocates only the outer array (a few hundred
    /// bytes); segments come on demand.
    pub fn new() -> Self {
        Self {
            segments: (0..MAX_SEGMENTS)
                .map(|_| OnceLock::new())
                .collect::<Vec<_>>()
                .into_boxed_slice(),
            next: AtomicU32::new(0),
        }
    }

    /// Append an entry, returning its index.
    ///
    /// The slot is fully initialised before this returns, and the caller
    /// publishes the index into the key directory only afterwards — so no
    /// reader can reach an index whose slot is still empty.
    pub fn push(&self, entry: Entry) -> u32 {
        let idx = self.next.fetch_add(1, Ordering::AcqRel);
        let (s, off) = locate(idx);
        assert!(
            s < MAX_SEGMENTS,
            "entry table exhausted: more than u32::MAX keys"
        );
        let seg = self.segments[s].get_or_init(|| {
            (0..segment_len(s))
                .map(|_| OnceLock::new())
                .collect::<Vec<_>>()
                .into_boxed_slice()
        });
        // Each index is handed to exactly one caller, so this never loses.
        seg[off]
            .set(entry)
            .unwrap_or_else(|_| panic!("entry index {idx} was allocated twice"));
        idx
    }

    /// Borrow an entry.
    ///
    /// Two relaxed loads and an index — no lock, no reference count. The
    /// returned reference is valid as long as the table is, because
    /// entries are never freed and segments never move.
    #[inline]
    pub fn get(&self, idx: u32) -> &Entry {
        let (s, off) = locate(idx);
        self.segments[s]
            .get()
            .expect("segment is initialised before its index is published")[off]
            .get()
            .expect("entry is initialised before its index is published")
    }

    /// How many entries have been appended.
    pub fn len(&self) -> u32 {
        self.next.load(Ordering::Acquire)
    }

    /// Whether nothing has been appended.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::record::Record;

    #[test]
    fn segments_tile_the_index_space_without_gaps_or_overlap() {
        // An off-by-one here aliases two indices onto one slot, which is
        // silent data corruption rather than a crash.
        let mut seen = std::collections::HashSet::new();
        for idx in 0..100_000u32 {
            let (s, off) = locate(idx);
            assert!(off < segment_len(s), "index {idx} overruns segment {s}");
            assert!(seen.insert((s, off)), "index {idx} collides at ({s},{off})");
        }
    }

    #[test]
    fn locate_matches_the_documented_boundaries() {
        assert_eq!(locate(0), (0, 0));
        assert_eq!(locate(1023), (0, 1023));
        assert_eq!(locate(1024), (1, 0));
        assert_eq!(locate(2047), (1, 1023));
        assert_eq!(locate(2048), (2, 0));
        assert_eq!(locate(4095), (2, 2047));
        assert_eq!(locate(4096), (3, 0));
    }

    #[test]
    fn thirty_two_segments_reach_past_u32() {
        let (s, _) = locate(u32::MAX);
        assert!(s < MAX_SEGMENTS, "u32::MAX lands in segment {s}");
    }

    #[test]
    fn entries_keep_their_identity_across_growth() {
        let t = EntryTable::new();
        for i in 0..5000u32 {
            let idx = t.push(Entry::new(
                format!("k{i}").as_bytes(),
                Record::resident(u64::from(i) + 1, &[]),
            ));
            assert_eq!(idx, i);
        }
        for i in 0..5000u32 {
            assert_eq!(t.get(i).key(), format!("k{i}").as_bytes());
            assert_eq!(t.get(i).load().version(), u64::from(i) + 1);
        }
        assert_eq!(t.len(), 5000);
    }

    #[test]
    fn concurrent_pushes_get_distinct_indices() {
        let t = std::sync::Arc::new(EntryTable::new());
        let got = std::sync::Mutex::new(Vec::new());
        std::thread::scope(|scope| {
            for _ in 0..8 {
                let t = std::sync::Arc::clone(&t);
                let got = &got;
                scope.spawn(move || {
                    let mut mine = Vec::new();
                    for i in 0..500u32 {
                        mine.push(t.push(Entry::new(
                            format!("k{i}").as_bytes(),
                            Record::tombstone(0),
                        )));
                    }
                    got.lock().unwrap().extend(mine);
                });
            }
        });
        let mut all = got.into_inner().unwrap();
        all.sort_unstable();
        all.dedup();
        assert_eq!(all.len(), 4000, "two threads got the same index");
        // And every one of them resolves, which is what would break if a
        // segment were published before it was filled.
        for idx in all {
            let _ = t.get(idx).key();
        }
    }
}
