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

#![allow(unsafe_code)]

use std::cell::UnsafeCell;
use std::mem::MaybeUninit;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::OnceLock;

use crate::value::Entry;

/// Entries in segment 0. Later segments double.
const BASE: u32 = 1024;
const BASE_SHIFT: u32 = 10;

/// Enough to index every `u32`: segment 22 ends at `1024 * 2^22` > 2^32.
const MAX_SEGMENTS: usize = 32;

/// One slot: an entry written once, then read forever.
///
/// # Why not `OnceLock<Entry>`
///
/// It was, and `OnceLock::set` runs the full `Once` state machine —
/// callgrind put 90 of `push`'s 182 instructions per op in `Once::call`,
/// `initialize` and `call_once_force`. All of that exists to arbitrate
/// between racing initialisers, and here there are none: `next.fetch_add`
/// hands each index to exactly one caller, so a slot has exactly one
/// writer by construction. The arbitration was paid for on every insert
/// and never used.
///
/// What remains is the part that IS load-bearing: a `Release` store after
/// the write, paired with an `Acquire` load before any read. `get` is no
/// cheaper than before — it was already a single atomic load — but `push`
/// is.
struct Slot {
    /// Set `Release` once `entry` is initialised.
    ready: AtomicBool,
    entry: UnsafeCell<MaybeUninit<Entry>>,
}

// SAFETY: a slot is written by exactly one thread, before `ready` is set,
// and read only after an `Acquire` load observes `ready`. So the write and
// every read are ordered, and no two threads ever touch it concurrently in
// a conflicting way.
unsafe impl Sync for Slot {}

impl Drop for Slot {
    fn drop(&mut self) {
        // Entries are immortal for the life of the table, but the TABLE is
        // dropped with the store, and an uninitialised slot must not be.
        if *self.ready.get_mut() {
            // SAFETY: `ready` is true, so `entry` was initialised, and
            // `&mut self` means nothing else can reach it.
            unsafe { self.entry.get_mut().assume_init_drop() };
        }
    }
}

/// One segment: a fixed run of slots, each filled at most once.
type Segment = Box<[Slot]>;

fn new_segment(len: usize) -> Segment {
    (0..len)
        .map(|_| Slot {
            ready: AtomicBool::new(false),
            entry: UnsafeCell::new(MaybeUninit::uninit()),
        })
        .collect::<Vec<_>>()
        .into_boxed_slice()
}

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
        let seg = self.segments[s].get_or_init(|| new_segment(segment_len(s)));
        let slot = &seg[off];
        debug_assert!(
            !slot.ready.load(Ordering::Acquire),
            "entry index {idx} was allocated twice"
        );
        // SAFETY: `next.fetch_add` gave `idx` to this caller alone, and
        // nothing can read the slot until the `Release` store below, so
        // this write has no other party.
        unsafe {
            (*slot.entry.get()).write(entry);
        }
        slot.ready.store(true, Ordering::Release);
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
        let slot = &self.segments[s]
            .get()
            .expect("segment is initialised before its index is published")[off];
        // NOT a debug_assert: this Acquire load is what pairs with the
        // Release in `push`, so it has to happen in release builds too.
        // The check it performs is then free.
        assert!(
            slot.ready.load(Ordering::Acquire),
            "entry index {idx} read before it was published"
        );
        // SAFETY: `ready` was stored with Release after the entry was
        // written, and the Acquire load above pairs with it, so the entry
        // is initialised and visible. It is never written again, and the
        // table outlives every reference it hands out.
        unsafe { (*slot.entry.get()).assume_init_ref() }
    }

    /// Whether a slot has been published yet.
    ///
    /// Test-only, and it exists so the ordering test can SPIN on
    /// readiness rather than assert it: a reader that learned the index
    /// through a relaxed store may legitimately not see `ready` yet, and
    /// that is not the bug the test is hunting.
    #[cfg(test)]
    fn is_ready(&self, idx: u32) -> bool {
        let (s, off) = locate(idx);
        match self.segments[s].get() {
            None => false,
            Some(seg) => seg[off].ready.load(Ordering::Acquire),
        }
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
        // Miri interprets every instruction, so the full sweep never
        // finishes under it. It is here to prove the unsafe is sound, not
        // to sweep the whole index space.
        #[cfg(miri)]
        const N: u32 = 3_000;
        #[cfg(not(miri))]
        const N: u32 = 100_000;
        for idx in 0..N {
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
        #[cfg(miri)]
        const M: u32 = 200;
        #[cfg(not(miri))]
        const M: u32 = 5000;
        for i in 0..M {
            let idx = t.push(Entry::new(
                format!("k{i}").as_bytes(),
                Record::resident(u64::from(i) + 1, &[]),
            ));
            assert_eq!(idx, i);
        }
        for i in 0..M {
            assert_eq!(t.get(i).key(), format!("k{i}").as_bytes());
            assert_eq!(t.get(i).load().version(), u64::from(i) + 1);
        }
        assert_eq!(t.len(), M);
    }

    #[test]
    fn a_reader_racing_a_push_never_sees_an_uninitialised_slot() {
        // THE test for the Release/Acquire pair, and the reason the
        // `assert!` in `get` is not a `debug_assert!`.
        //
        // The index is handed to the reader through a RELAXED atomic on
        // purpose. Anything stronger — a channel, a mutex, an Acquire
        // load — would supply the happens-before edge itself and hide
        // whether the slot's own ordering is right. With relaxed
        // publication the only thing ordering the reader's view of the
        // entry against the writer's initialisation of it is `ready`.
        //
        // Reordering `push` to store `ready` before writing the entry
        // makes Miri report a data race here. Without this test that
        // reordering passes everything.
        use std::sync::atomic::AtomicU32;
        #[cfg(miri)]
        const ROUNDS: u32 = 30;
        #[cfg(not(miri))]
        const ROUNDS: u32 = 2_000;

        let t = std::sync::Arc::new(EntryTable::new());
        let published = std::sync::Arc::new(AtomicU32::new(u32::MAX));

        std::thread::scope(|scope| {
            {
                let t = std::sync::Arc::clone(&t);
                let published = std::sync::Arc::clone(&published);
                scope.spawn(move || {
                    for i in 0..ROUNDS {
                        let idx = t.push(Entry::new(
                            format!("k{i:06}").as_bytes(),
                            Record::resident(u64::from(i) + 1, &[0xAB; 24]),
                        ));
                        published.store(idx, Ordering::Relaxed);
                    }
                });
            }
            let t = std::sync::Arc::clone(&t);
            let published = std::sync::Arc::clone(&published);
            scope.spawn(move || {
                let mut seen = 0u32;
                while seen < ROUNDS - 1 {
                    let idx = published.load(Ordering::Relaxed);
                    // Not-yet-published is expected: the relaxed store
                    // above carries no ordering, so the index can arrive
                    // before `ready` does. Spin rather than assert — the
                    // bug being hunted is the opposite case, `ready` set
                    // while the entry is still uninitialised.
                    if idx == u32::MAX || !t.is_ready(idx) {
                        std::hint::spin_loop();
                        continue;
                    }
                    let e = t.get(idx);
                    assert!(e.key().starts_with(b"k"));
                    assert_eq!(e.load().bytes(), Some(&[0xAB; 24][..]));
                    seen = idx;
                }
            });
        });
    }

    #[test]
    fn dropping_the_table_drops_initialised_entries_only() {
        // Half a segment filled, the rest never written. Dropping must run
        // Entry's destructor for the first half and touch nothing in the
        // second -- Miri fails this if an uninitialised slot is dropped.
        let t = EntryTable::new();
        for i in 0..50u32 {
            t.push(Entry::new(
                format!("k{i}").as_bytes(),
                Record::resident(1, &[7u8; 32]),
            ));
        }
        drop(t);
    }

    #[test]
    fn concurrent_pushes_get_distinct_indices() {
        let t = std::sync::Arc::new(EntryTable::new());
        #[cfg(miri)]
        const PER: u32 = 20;
        #[cfg(not(miri))]
        const PER: u32 = 500;
        let got = std::sync::Mutex::new(Vec::new());
        std::thread::scope(|scope| {
            for _ in 0..8 {
                let t = std::sync::Arc::clone(&t);
                let got = &got;
                scope.spawn(move || {
                    let mut mine = Vec::new();
                    for i in 0..PER {
                        mine.push(
                            t.push(Entry::new(format!("k{i}").as_bytes(), Record::tombstone(0))),
                        );
                    }
                    got.lock().unwrap().extend(mine);
                });
            }
        });
        let mut all = got.into_inner().unwrap();
        all.sort_unstable();
        all.dedup();
        assert_eq!(
            all.len(),
            8 * PER as usize,
            "two threads got the same index"
        );
        // And every one of them resolves, which is what would break if a
        // segment were published before it was filled.
        for idx in all {
            let _ = t.get(idx).key();
        }
    }
}
