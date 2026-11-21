//! One key's published value: an atomic pointer, CAS-replaced, with the
//! displaced record released through an epoch.
//!
//! # Why this replaced a `Mutex<Record>`
//!
//! The slot used to be a `std::sync::Mutex<Record>`, on the argument that
//! `mrx-core` is `forbid(unsafe_code)` and depends on nothing outside
//! std. That cost the *read* path most: a read took the lock (a store to
//! a shared word), cloned the record (two refcount atomics), and dropped
//! the clone. Measured at one thread on the write-scaling ladder
//! (`crates/mrx-masstree/examples/write_scaling.rs`, `rd_index` ->
//! `rd_entry` -> `rd_value`): masstree lookup alone 350 ns, +248 ns for
//! the load, +58 ns for the copy. The C++ implementation's read takes an
//! RCU guard and an atomic load, and no store to any shared word.
//!
//! # What is actually hard here
//!
//! Not the compare-and-swap — `AtomicPtr::compare_exchange` is the same
//! instruction the C++ `mrx_entry::val` uses. The hard part is
//! **reclamation**: a reader that loads the pointer and then increments
//! the refcount can be preempted in between, and a writer that frees the
//! displaced record in that window hands the reader a use-after-free.
//! C++ solves it by deferring the free to an RCU epoch (`mrx_rcu_region`
//! on the read side, `rcu::s_instance.dealloc` on the write side). This
//! module does the same with `crossbeam_epoch`, which is why the crate
//! now takes its one non-std dependency.
//!
//! # The protocol, and why it is sound
//!
//! 1. Every reader pins a guard **before** loading the pointer.
//! 2. Every writer that displaces a record gives up the slot's reference
//!    to it only inside `Guard::defer`, i.e. after every guard pinned at
//!    that moment has retired.
//! 3. So a reader that pinned first can always complete
//!    load-then-increment: the block is still allocated at the increment,
//!    because the release that would free it is still queued.
//!
//! Point 2 is the whole reason a plain refcount (or `ArcSwap`-shaped
//! `compare_exchange`) is not enough on its own, and point 1 is why the
//! `Atomic` API here takes a `&Guard`: it is impossible to load this
//! pointer without one.
//!
//! # Deferred, not freed late
//!
//! The deferred closure is `drop(record_handle)`, and dropping a handle
//! is what runs the refcount release. A record still referenced by a
//! reader that cloned it is not freed by that release — it is simply one
//! decrement. The epoch only has to cover the *window* between a reader's
//! load and its increment; everything after that is ordinary refcounting.
//!
//! # The read path this feeds
//!
//! A read never takes a handle from this slot: it pins a guard, copies the
//! bytes out under it, and drops the guard (`Entry::copy_into` and
//! `Entry::copy_into_slice`). What remains per read is the pin itself, and
//! that is where the money is at thread counts above one. Measured at 24
//! threads, pin + unpin with nothing in between runs at 178 M ops/s
//! aggregate against roughly 545 M at a single thread — 134.8 ns of thread
//! time per operation against 12.8 ns — because every pin reads and
//! sometimes advances the shared epoch.
//!
//! Three changes took that back off the hot read path:
//!
//! 1. the entry is resolved from its address (the word the index stores is
//!    a pointer, so there is no second dependent load through the entry
//!    table);
//! 2. the FFI read copies straight into the caller's buffer instead of
//!    allocating a `Vec` and a `malloc`'d copy per read (`mrxdb_get_into`);
//! 3. the read no longer opens a region of its own. The tree call opens
//!    the one it needs, and a second admission around it — one more
//!    ticker-guard/clock read per read — measured as pure cost.
//!
//! On the mako lane (`masstree_rocks_bench`, 2000-key hot set, medians of
//! three, rust/cpp:):
//!
//! | threads | mutex slot, old read | epoch slot, old read | epoch slot, new read |
//! | --- | ---: | ---: | ---: |
//! | 24 | 0.902 | 0.809 | 0.980 |
//! | 32 | 0.900 | 0.796 | 0.960 |
//!
//! Uniform reads are now above parity (1.050 at 24 threads, 1.018 at 32).
//! Writes moved the other way, in the Rust lane's favour: 3.151 -> 3.223 at
//! 24 threads and 3.753 -> 3.86 at 32 (rust/cpp).
//!
//! The residual few percent is the pin itself, and it is the price of the
//! lock-free slot: removing it needs a cheaper reclamation scheme, not
//! more copy-path work.
//!
//! # What this costs, measured
//!
//! One worker, zoo-002, boost off, pinned, 200k ops over a 200k keyspace
//! (`crates/mrx-masstree/examples/write_scaling.rs`, medians of 3):
//!
//! | rung | mutex slot | this slot |
//! | --- | ---: | ---: |
//! | `rd_entry` (load) | 589 ns | 572 ns |
//! | `rd_value` (load + copy) | 642 ns | 636 ns |
//! | `entry` (draw + publish) | 545 ns | 636 ns |
//! | `floors` (+ announce) | 548 ns | 638 ns |
//! | `full` (store + flusher) | 1366 ns | 1629 ns |
//!
//! The read side this was built for is a wash at ONE worker, where a
//! mutex is uncontended; its case is concurrent readers, which no
//! one-worker measurement can price. The write side pays, and the harness
//! has rungs that price the machinery directly (`epoch_pin` 12.8 ns,
//! `epoch_defer` 32.8 ns, both per op and touching no record) — about
//! 40 ns of the 91 ns.
//!
//! Batching retirements through a thread-local `Vec`, flushed with one
//! `defer` per 32, was tried to remove the per-write boxed closure. It
//! made the write path WORSE, not better: `entry` 636 -> 725 ns. The
//! batch's own machinery measured 21.6 ns/op, so the extra ~90 ns came
//! from somewhere else in the combination; whatever it was, the simple
//! form below is the one that measures better and it is the one kept.
//! Do not re-add batching without re-running that comparison.
//!
//! Per-write deferral also shows up end to end: the cache-ACK lane of
//! `masstree_rocks_bench` at one worker measured 382.9k -> 368.4k ops/s
//! (-3.8%) against the mutex slot, while the C++ lane was unchanged
//! (358.2k) — so the Rust lane's lead over C++ narrowed from 6.9% to
//! 2.9% and held.

//! # Running the tests under Miri
//!
//! ```text
//! MIRIFLAGS="-Zmiri-tree-borrows -Zmiri-permissive-provenance \
//!            -Zmiri-ignore-leaks" \
//!   cargo +nightly miri test -p mrx-core slot::
//! ```
//!
//! All three flags are needed and none of them hide this module's bugs:
//! `crossbeam-epoch` launders pointers through `usize` (permissive
//! provenance), its intrusive lists trip Stacked Borrows inside
//! `internal.rs` (Tree Borrows is the model its own CI uses), and it
//! leaves per-thread `Local` handles and queued deferred bags alive at
//! process exit (ignore-leaks). What Miri checks here is the part that
//! matters: that a reader never touches a record a writer has retired,
//! which the concurrent test below exercises.

#![allow(unsafe_code)]

use std::sync::atomic::Ordering;

use crossbeam_epoch::{self as epoch, Atomic, Shared};

use crate::record::{Header, Record};

/// One key's published value.
pub(crate) struct Slot {
    ptr: Atomic<Header>,
}

impl Slot {
    /// A slot holding `record`, taking over the caller's reference.
    pub(crate) fn new(record: Record) -> Self {
        Self {
            ptr: Atomic::from(record.into_raw_owned()),
        }
    }

    /// The published record, as an owning handle.
    pub(crate) fn load(&self) -> Record {
        let guard = epoch::pin();
        let p = self.ptr.load(Ordering::Acquire, &guard);
        // SAFETY: the slot publishes a non-null record for as long as it
        // is reachable, and the guard makes the load-then-increment
        // window safe — see the module docs.
        unsafe { Record::ref_from_ptr(p.as_raw()) }
    }

    /// Run `f` against the published record **without touching its
    /// refcount**.
    ///
    /// The read path the store uses on a resident value: no increment, no
    /// decrement, just the load and whatever `f` does with the bytes.
    pub(crate) fn with_value<R>(&self, f: impl FnOnce(&Record) -> R) -> R {
        let guard = epoch::pin();
        let p = self.ptr.load(Ordering::Acquire, &guard);
        // SAFETY: as `load`. The borrowed handle cannot escape — `f`
        // receives it by reference and returns an unrelated `R`.
        unsafe { Record::with_borrowed(p.as_raw(), f) }
    }

    /// Publish `new` only if the slot still holds exactly `expected`.
    ///
    /// Identity, not equality: two records can be equal in content and
    /// distinct in obligation, which is what the fill path's ABA guard
    /// turns on. Returns whether the swap happened.
    pub(crate) fn compare_publish(&self, expected: &Record, new: Record) -> bool {
        let guard = epoch::pin();
        let cur = self.ptr.load(Ordering::Acquire, &guard);
        if cur.as_raw() != expected.as_header_ptr() {
            // A different record is published; `new` was never installed,
            // so its reference goes back to the caller's stack here.
            drop(new);
            return false;
        }
        match self.ptr.compare_exchange(
            cur,
            Shared::from(new.into_raw_owned()),
            Ordering::AcqRel,
            Ordering::Acquire,
            &guard,
        ) {
            Ok(_) => {
                Self::retire(cur, &guard);
                true
            }
            Err(err) => {
                // Lost the race: the slot moved on between our load and
                // our CAS. Take our handle back and release it.
                let raw = err.new.as_raw();
                // SAFETY: `new`'s reference was transferred into the CAS
                // above and handed back here uninstalled.
                drop(unsafe { Record::from_owned_ptr(raw) });
                false
            }
        }
    }

    /// Run `f` against the published record, publish whatever it returns,
    /// and retry `f` if another writer publishes in between.
    ///
    /// This is how the write path stays atomic per key: the decision
    /// (does the key exist? may this write proceed?) is made against the
    /// exact record the CAS then compares, so no other writer can
    /// interleave between the decision and the publication. `f` is
    /// re-run on contention rather than being given a way to undo its
    /// decision, which is the same shape as the C++ `mrx_write` loop.
    pub(crate) fn with_slot<R>(&self, mut f: impl FnMut(&Record) -> (Option<Record>, R)) -> R {
        loop {
            let guard = epoch::pin();
            let cur = self.ptr.load(Ordering::Acquire, &guard);
            // SAFETY: as `load`; the borrow ends before the CAS below and
            // nothing derived from it is kept.
            let (new, r) = unsafe { Record::with_borrowed(cur.as_raw(), &mut f) };
            let Some(new) = new else {
                // `f` declined to publish; nothing to retry.
                return r;
            };
            match self.ptr.compare_exchange(
                cur,
                Shared::from(new.into_raw_owned()),
                Ordering::AcqRel,
                Ordering::Acquire,
                &guard,
            ) {
                Ok(_) => {
                    Self::retire(cur, &guard);
                    return r;
                }
                Err(err) => {
                    // Another writer won. `r` was computed against a
                    // record that is no longer current, so it is dropped
                    // along with `f`'s new record and the loop re-runs.
                    let raw = err.new.as_raw();
                    // SAFETY: as in `compare_publish`.
                    drop(unsafe { Record::from_owned_ptr(raw) });
                }
            }
        }
    }

    /// Release the slot's reference to a record it no longer publishes.
    ///
    /// The deferral is the point: a reader may have loaded `cur` and not
    /// yet incremented its refcount, and that window is not covered by
    /// anything the reader itself holds.
    fn retire(cur: Shared<'_, Header>, guard: &epoch::Guard) {
        // SAFETY: the CAS that called this transferred the slot's
        // reference to `cur` into our hands; it is released exactly once,
        // in the deferred drop below.
        let old = unsafe { Record::from_owned_ptr(cur.as_raw()) };
        guard.defer(move || drop(old));
    }
}

impl std::fmt::Debug for Slot {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        // The published record, not the pointer: that is what a reader of
        // a log line wants, and the pointer is unstable across runs.
        self.with_value(|v| write!(f, "{v:?}"))
    }
}

impl Drop for Slot {
    fn drop(&mut self) {
        // Immediate, not deferred: dropping a `Slot` proves exclusive
        // access to it, so no reader can be inside the load-then-
        // increment window. Deferring here would only risk never running,
        // e.g. at process exit.
        let guard = epoch::pin();
        let p = self.ptr.load(Ordering::Relaxed, &guard);
        // SAFETY: the slot's own reference, released here.
        drop(unsafe { Record::from_owned_ptr(p.as_raw()) });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicBool, AtomicUsize};
    use std::time::{Duration, Instant};

    /// Every version carries its own version number as the payload, so a
    /// reader can tell a torn or recycled record from a valid one.
    fn versioned(version: u64) -> Record {
        Record::resident(version, &version.to_be_bytes())
    }

    fn assert_intact(r: &Record) {
        let bytes = r.bytes().expect("resident");
        assert_eq!(
            r.version(),
            u64::from_be_bytes(bytes.try_into().expect("eight bytes")),
            "a record's payload must match its own version: this is what \
             catches a read of a freed or recycled block"
        );
    }

    #[test]
    fn load_sees_the_published_record() {
        let slot = Slot::new(versioned(7));
        assert_eq!(slot.load().version(), 7);
        assert_eq!(slot.load().bytes(), Some(&7u64.to_be_bytes()[..]));
    }

    #[test]
    fn with_value_borrows_without_taking_a_reference() {
        let slot = Slot::new(versioned(3));
        let (refs_inside, bytes) =
            slot.with_value(|v| (v.refcount(), v.bytes().map(<[u8]>::to_vec)));
        assert_eq!(
            refs_inside, 1,
            "the borrowed handle owns no reference: only the slot's is counted"
        );
        assert_eq!(bytes.as_deref(), Some(&3u64.to_be_bytes()[..]));
    }

    #[test]
    fn compare_publish_requires_identity() {
        let slot = Slot::new(versioned(1));
        let twin = versioned(1); // equal, not identical
        assert!(!slot.compare_publish(&twin, versioned(2)));
        assert_eq!(slot.load().version(), 1, "a failed CAS publishes nothing");
        let expected = slot.load();
        assert!(slot.compare_publish(&expected, versioned(2)));
        assert_eq!(slot.load().version(), 2);
    }

    #[test]
    fn with_slot_retries_against_the_current_record() {
        let slot = Slot::new(versioned(1));
        let mut attempts = 0;
        let seen = slot.with_slot(|cur| {
            attempts += 1;
            if cur.version() < 5 {
                // Stand in for a racing writer: publish between this
                // attempt's load and its CAS, through the same publish
                // path so the reference accounting stays exact. The CAS
                // below must then lose and the closure must run again.
                slot.with_slot(|inner| {
                    assert!(inner.version() < 5, "only the first attempt races");
                    (Some(versioned(5)), ())
                });
                return (Some(versioned(cur.version() + 1)), cur.version());
            }
            (None, cur.version())
        });
        assert_eq!(seen, 5, "the retry re-read the record that won");
        assert_eq!(attempts, 2, "the closure ran once per attempt");
    }

    /// The property the epoch exists for: readers never observe a freed or
    /// half-recycled record while writers displace them continuously.
    ///
    /// The writer keeps going until every reader has seen the published
    /// version change `MIN_CHANGES_SEEN` times. A fixed number of writes
    /// does not make the two sides overlap: on one busy CPU the writer can
    /// finish all of them before a reader is first scheduled.
    ///
    /// `GIVE_UP` is far longer than a starved reader needs. It is there so
    /// that a defect which hides the writes from a reader fails the test
    /// instead of hanging it.
    #[test]
    fn concurrent_readers_and_writers_never_see_a_broken_record() {
        const READERS: usize = 3;
        const WRITE_BURST: u64 = 512;
        const MIN_CHANGES_SEEN: usize = 4;
        const GIVE_UP: Duration = Duration::from_secs(60);
        let slot = Slot::new(versioned(0));
        let stop = AtomicBool::new(false);
        let changes_seen: [AtomicUsize; READERS] = std::array::from_fn(|_| AtomicUsize::new(0));
        let mut written = 0;

        std::thread::scope(|scope| {
            let readers: Vec<_> = changes_seen
                .iter()
                .map(|seen| {
                    let (slot, stop) = (&slot, &stop);
                    scope.spawn(move || {
                        let mut last = 0;
                        while !stop.load(Ordering::Relaxed) {
                            assert_intact(&slot.load());
                            let version = slot.with_value(|record| {
                                assert_intact(record);
                                record.version()
                            });
                            assert!(version >= last, "the published version went backwards");
                            if version != last {
                                last = version;
                                seen.fetch_add(1, Ordering::Relaxed);
                            }
                        }
                    })
                })
                .collect();
            let started = Instant::now();
            loop {
                // Back-to-back writes, with nothing else in the loop: a
                // reclamation defect shows only when a block is freed and
                // reused inside a reader's window.
                for _ in 0..WRITE_BURST {
                    written += 1;
                    slot.with_slot(|_| (Some(versioned(written)), ()));
                }
                // A reader returns before `stop` only by panicking. Stop
                // writing then, so the scope can report the panic.
                if readers.iter().any(|reader| reader.is_finished()) {
                    break;
                }
                let every_reader_overlapped = changes_seen
                    .iter()
                    .all(|seen| seen.load(Ordering::Relaxed) >= MIN_CHANGES_SEEN);
                if every_reader_overlapped || started.elapsed() > GIVE_UP {
                    break;
                }
            }
            stop.store(true, Ordering::Relaxed);
        });

        assert_eq!(slot.load().version(), written);
        for seen in &changes_seen {
            assert!(
                seen.load(Ordering::Relaxed) >= MIN_CHANGES_SEEN,
                "a reader did not see the writer's records change"
            );
        }
    }
}
