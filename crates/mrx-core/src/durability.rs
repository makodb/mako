//! Durability bookkeeping: version issuance, the per-writer floors, the
//! dirty-ticket log, and the watermark.
//!
//! # The one `unsafe` in this crate
//!
//! [`TicketLog`] is a lock-free ring, and its slots are `UnsafeCell`.
//! Three lines of `unsafe`, all in this file, all justified by the
//! sequence protocol documented on the type. Everything else in
//! `mrx-core` is `forbid(unsafe_code)`; this module is
//! `#![allow(unsafe_code)]` and nothing else is.
//!
//! This is the part of the cache that is easy to write and hard to write
//! *correctly*. Nearly every subtlety here corresponds to a specific way
//! an idiomatic translation loses a property silently.

#![allow(unsafe_code)]

use std::cell::UnsafeCell;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Mutex;

use crate::{Version, FIRST_VERSION};

/// A dirty ticket: "this entry changed at this version".
///
/// It deliberately carries **no payload**. Caching the bytes here — the
/// tempting `HashMap<Key, Arc<Val>>` — reintroduces verbatim the bug the
/// C++ original recorded: the drain confirms an obligation using bytes it
/// snapshotted, so a key overwritten faster than drain latency can be
/// reported durable while never reaching the store at all. Writeback must
/// re-read the entry's *current* value at write time.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Ticket {
    /// Which entry (an index into the store's entry table).
    pub entry: u32,
    /// The version that was published.
    pub version: Version,
}

/// Per-writer state.
///
/// The three floors exist to cover a version through every instant
/// between being drawn and being visible to the flusher. The hand-off
/// between them is ordered, and the order is load-bearing in both
/// directions — see [`Floors::min_over`] and [`WriterSlot::submit`].
///
/// # Deliberately NOT cache-line padded
///
/// The obvious move — `#[repr(align(128))]`, so sixteen threads storing
/// to their own `announce` stop invalidating each other's lines — was
/// tried and **measured worse** (median 3.19M vs 3.50M ops/s, losing
/// four interleaved pairs out of five).
///
/// The reason it backfires is [`Floors::min_over`]: the flusher reads
/// every slot's three floors on every cycle. Unpadded, the slot array is
/// about fourteen cache lines; padded, it is sixty-four. The scan's line
/// traffic grows faster than the producers' false sharing shrinks.
///
/// Restore the padding only together with a measurement, and only if
/// that scan has stopped touching every slot.
#[derive(Debug, Default)]
pub struct WriterSlot {
    /// Set **before** a version is drawn, cleared only once the ticket is
    /// safely in the local batch.
    ///
    /// This is the publish gap: a version exists from the moment it is
    /// drawn, but is invisible to both the batch and the log until it is
    /// submitted. Without this floor the watermark can sail past a
    /// version whose write has not landed — a mutation that escaped an
    /// entire crash-test suite, because the window is roughly one write
    /// in `batch` and a few instructions wide.
    pub announce: AtomicU64,
    /// Smallest version currently sitting in this writer's local batch.
    pub batch_min: AtomicU64,
    /// Smallest version in a full batch that has left `batch` but has not
    /// yet been appended to the log.
    pub staged_min: AtomicU64,
    /// The local batch itself.
    pub batch: Mutex<Vec<Ticket>>,
}

/// Sentinel meaning "this floor constrains nothing".
pub const NO_FLOOR: u64 = u64::MAX;

impl WriterSlot {
    /// A fresh slot with all floors disarmed.
    pub fn new() -> Self {
        Self {
            announce: AtomicU64::new(NO_FLOOR),
            batch_min: AtomicU64::new(NO_FLOOR),
            staged_min: AtomicU64::new(NO_FLOOR),
            batch: Mutex::new(Vec::new()),
        }
    }

    /// Arm the announce floor before drawing a version.
    ///
    /// Armed with the counter's value *before* the draw. Arming with the
    /// drawn version afterwards would leave exactly the window this
    /// exists to close.
    pub fn arm(&self, counter_now: Version) {
        self.announce.store(counter_now, Ordering::SeqCst);
    }

    /// Disarm the announce floor.
    ///
    /// Only ever called once the version is covered by something else —
    /// the batch — or once it is certain no version was published.
    ///
    /// **Not** a `Drop` guard, and that is deliberate. A guard that
    /// always clears on scope exit would disarm in the window between a
    /// successful publish and the ticket reaching the batch, leaving the
    /// published version covered by nothing: the watermark overshoots,
    /// `sync()` lies, and the sweeper may discard the only copy. Pinning
    /// the watermark forever is the fail-stop direction; overshooting is
    /// the data-loss direction. When in doubt this code leaves the floor
    /// armed.
    pub fn disarm(&self) {
        self.announce.store(NO_FLOOR, Ordering::Release);
    }

    /// Add a ticket to the local batch, returning a full batch to append.
    ///
    /// The hand-off order is the point: `batch_min` is lowered while the
    /// batch lock is held and **before** `announce` is cleared, and
    /// `staged_min` is set **before** `batch_min` is reset. At every
    /// instant some floor is at or below the version, so no published
    /// version is ever uncovered.
    pub fn submit(&self, t: Ticket, batch_cap: usize) -> Option<Vec<Ticket>> {
        let mut full: Option<Vec<Ticket>> = None;
        {
            let mut b = self.batch.lock().expect("batch mutex poisoned");
            b.push(t);
            // Lower batch_min FIRST: it must already cover this version
            // by the time announce is released below.
            let cur = self.batch_min.load(Ordering::Relaxed);
            if t.version < cur {
                self.batch_min.store(t.version, Ordering::Release);
            }
            if b.len() >= batch_cap {
                // `mem::replace` with a sized Vec, not `mem::take`: take
                // leaves the batch at ZERO capacity, so the next
                // `batch_cap` pushes re-grow it 1, 2, 4 ... 64 — six
                // reallocations per batch, every batch, inside this lock.
                let taken =
                    std::mem::replace(&mut *b, Vec::with_capacity(batch_cap));
                // `batch_min` already holds this; the O(batch_cap) rescan
                // it replaced was pure duplicated work under the lock.
                let min = self.batch_min.load(Ordering::Relaxed);
                // staged_min before batch_min is reset, so the handover
                // has no gap.
                self.staged_min.store(min, Ordering::Release);
                self.batch_min.store(NO_FLOOR, Ordering::Release);
                full = Some(taken);
            }
        }
        // Only now is the version covered by the batch (or by staged),
        // so only now may the announce floor be released.
        self.disarm();
        full
    }

    /// Take whatever is sitting in the local batch.
    ///
    /// The flusher calls this so an idle — or dead — writer's partial
    /// batch still reaches the log. Without it, one thread that writes
    /// once and then goes quiet pins the watermark for the whole process.
    pub fn steal(&self) -> Option<Vec<Ticket>> {
        // Check without locking first. The flusher stealing from every
        // slot on every cycle means this runs constantly against the
        // producers' own lock, and the overwhelming majority of slots are
        // empty — either never used by any thread, or already drained
        // this cycle. A relaxed load costs nothing; taking the lock to
        // discover the batch is empty costs the producer.
        //
        // Sound because `batch_min` is lowered while the batch lock is
        // held and BEFORE the ticket is visible anywhere else: a slot
        // reporting NO_FLOOR has nothing this steal could have taken, and
        // a ticket arriving immediately afterwards is taken next cycle.
        if self.batch_min.load(Ordering::Acquire) == NO_FLOOR {
            return None;
        }
        let mut b = self.batch.lock().expect("batch mutex poisoned");
        if b.is_empty() {
            return None;
        }
        let taken = std::mem::take(&mut *b);
        let min = taken.iter().map(|x| x.version).min().unwrap_or(NO_FLOOR);
        self.staged_min.store(min, Ordering::Release);
        self.batch_min.store(NO_FLOOR, Ordering::Release);
        Some(taken)
    }

    /// Release the staged floor once the tickets are in the log.
    pub fn clear_staged(&self) {
        self.staged_min.store(NO_FLOOR, Ordering::Release);
    }
}

/// The floors visible to the watermark computation, in the order they
/// must be read.
pub struct Floors;

impl Floors {
    /// The lowest version any writer might still owe.
    ///
    /// **THE READ ORDER BELOW IS LOAD-BEARING.** The counter is sampled
    /// first, then each writer's floors, then (by the caller) the log.
    /// Writers publish in the mirror order, so each adjacent pair is a
    /// release/acquire hand-off. Reorder any pair and a published version
    /// can slip between two floors and be covered by neither.
    ///
    /// Concretely, swapping `announce` and `batch_min` here gives: the
    /// flusher reads `batch_min = NO_FLOOR`; a writer draws v950,CASes,
    /// and *acks*; the writer then sets `batch_min` and clears
    /// `announce`; the flusher reads `announce = NO_FLOOR`. v950 is in no
    /// floor, the watermark passes it, and the sweeper discards bytes the
    /// durable store never received.
    ///
    /// For that reason this is written as explicit sequential statements.
    /// Do **not** rewrite it as `slots.iter().flat_map(...).min()` — that
    /// bakes in whatever field order was typed — and never parallelise
    /// it.
    /// `slots` must be only the **registered** prefix, not the whole
    /// array. The flusher runs this every cycle; walking 64 slots to
    /// read 16 live ones costs four times the cache-line traffic and
    /// pulls lines the producers own into shared state for nothing.
    pub fn min_over(slots: &[WriterSlot], counter: Version) -> u64 {
        let mut m = counter;
        for w in slots {
            // ORDER IS LOAD-BEARING
            let a = w.announce.load(Ordering::Acquire);
            if a < m {
                m = a;
            }
            // ORDER IS LOAD-BEARING
            let b = w.batch_min.load(Ordering::Acquire);
            if b < m {
                m = b;
            }
            // ORDER IS LOAD-BEARING
            let s = w.staged_min.load(Ordering::Acquire);
            if s < m {
                m = s;
            }
        }
        m
    }
}

/// The shared dirty-ticket log: a bounded MPSC ring, lock-free for
/// producers.
///
/// # Why this is not a mutex-guarded queue any more
///
/// It was, and it cost about a third of the write path. The mutex was not
/// the problem — a spinlock measured identically. The problem was that
/// producers and the consumer *excluded each other at all*: `drain` took
/// the same lock `append` needs and held it while it emptied the queue,
/// so every writer that filled a batch stopped dead for the length of a
/// drain. Here they never exclude each other. A producer reserves a
/// contiguous run with one `fetch_add` per BATCH and writes its slots; the
/// consumer walks the published prefix and recycles slots behind itself.
///
/// # The protocol
///
/// Vyukov's bounded MPMC ring with a single consumer, which is what the
/// C++ implementation uses (`mrx_log` in `masstree_rocks_index.hh`) and
/// mirroring it keeps the two comparable.
///
/// Each slot carries a sequence number. For a slot serving position `p`:
///
/// * `seq == p` — free, and reserved by whoever drew `p`. Only that
///   producer may write it.
/// * `seq == p + 1` — published. Only the consumer may read it.
/// * `seq == p + cap` — recycled, i.e. free for position `p + cap`, which
///   is the same slot one lap later.
///
/// Slots start at `seq[i] = i`, so lap zero is immediately reservable.
///
/// # Safety argument
///
/// The `unsafe` here is three lines, and rests on four facts:
///
/// 1. **`tail.fetch_add` hands each position to exactly one producer.**
///    Two producers can never hold the same `p`, so the write to
///    `slot.data` has a single writer by construction.
/// 2. **A producer waits for `seq == p` before writing.** That value is
///    published only by the consumer recycling the previous lap
///    (`seq = p_prev + cap`, and `p_prev + cap == p`), so the slot is not
///    being read while it is written.
/// 3. **The consumer reads only at `seq == p + 1`**, which the producer
///    stores with `Release` *after* writing the data. The consumer's
///    `Acquire` load pairs with it, so the data is visible.
/// 4. **`Ticket` is `Copy` and owns nothing** — two `u64`-ish scalars, no
///    pointers, no allocation. This is the fact that makes the whole
///    thing tractable: even a torn or duplicated read could only produce a
///    wrong number, never a leak, a double free, or a dangling pointer.
///    Do not put an owning type in a slot without revisiting all of this.
///
/// The consumer must be unique. That is enforced rather than assumed —
/// see [`TicketLog::drain`].
pub struct TicketLog {
    slots: Box<[Slot]>,
    mask: u64,
    cap: u64,
    /// Next position to reserve. Producers `fetch_add` this.
    tail: crate::CacheLine<AtomicU64>,
    /// Positions strictly below this have been drained and recycled.
    /// Only the consumer writes it.
    confirmed: crate::CacheLine<AtomicU64>,
    /// Set at shutdown so producers waiting for a slot give up.
    stopping: AtomicBool,
    /// Enforces the single-consumer invariant. Taken once per drain, not
    /// once per ticket, so it is off the producers' path entirely.
    consumer: Mutex<()>,
}

struct Slot {
    seq: AtomicU64,
    data: UnsafeCell<Ticket>,
}

// SAFETY: `Slot`'s data is only ever accessed under the sequence protocol
// described above, which gives it a single writer and then a single
// reader, never concurrently.
unsafe impl Sync for Slot {}

impl std::fmt::Debug for TicketLog {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("TicketLog")
            .field("cap", &self.cap)
            .field("len", &self.len())
            .finish()
    }
}

impl TicketLog {
    /// A ring holding at most `cap` un-drained tickets.
    ///
    /// `cap` is rounded up to a power of two: the index is a mask, and a
    /// modulo on the producer's hot path would be a division.
    ///
    /// **Depth is the coalescing multiplier.** A ticket sitting behind a
    /// deep backlog is usually superseded by drain time and costs nothing
    /// at RocksDB; a shallow ring degenerates into write-through. The C++
    /// version measured 65K slots pinning 16 writers to RocksDB's ingest
    /// rate.
    pub fn new(cap: usize) -> Self {
        let cap = cap.max(2).next_power_of_two() as u64;
        let slots = (0..cap)
            .map(|i| Slot {
                seq: AtomicU64::new(i),
                data: UnsafeCell::new(Ticket { entry: 0, version: 0 }),
            })
            .collect::<Vec<_>>()
            .into_boxed_slice();
        Self {
            slots,
            mask: cap - 1,
            cap,
            tail: crate::CacheLine(AtomicU64::new(0)),
            confirmed: crate::CacheLine(AtomicU64::new(0)),
            stopping: AtomicBool::new(false),
            consumer: Mutex::new(()),
        }
    }

    /// Append, waiting for room. Never drops a ticket.
    ///
    /// A dropped ticket is an acked write the flusher never learns about,
    /// which is silent data loss under exactly the overload this exists to
    /// absorb. Waiting is the only acceptable behaviour for a producer.
    pub fn append(&self, tickets: &[Ticket]) {
        if tickets.is_empty() {
            return;
        }
        let k = tickets.len() as u64;
        // One reservation for the whole batch.
        let pos = self.tail.fetch_add(k, Ordering::SeqCst);
        for (i, t) in tickets.iter().enumerate() {
            self.publish(pos + i as u64, *t);
        }
    }

    /// Write one ticket into its reserved slot, waiting for the slot's
    /// previous lap to be recycled.
    fn publish(&self, want: u64, t: Ticket) {
        let slot = &self.slots[(want & self.mask) as usize];
        let mut spins = 0u32;
        while slot.seq.load(Ordering::Acquire) != want {
            if self.stopping.load(Ordering::Relaxed) {
                // Shutting down and the consumer may be gone. Writing
                // anyway would race it; dropping the ticket is safe here
                // and only here, because nothing will read the log again.
                return;
            }
            spins += 1;
            if spins < 512 {
                std::hint::spin_loop();
            } else if spins < 4096 {
                std::thread::yield_now();
            } else {
                std::thread::sleep(std::time::Duration::from_micros(50));
            }
        }
        // SAFETY: `seq == want` means this slot is reserved for `want`,
        // and `tail.fetch_add` gave `want` to this thread alone. No other
        // thread may read or write the slot until the store below.
        unsafe {
            *slot.data.get() = t;
        }
        slot.seq.store(want + 1, Ordering::Release);
    }

    /// Append only if there is room right now.
    ///
    /// The flusher uses this on its steal path: blocking there would
    /// deadlock it against the backpressure only it can relieve.
    pub fn try_append(&self, tickets: &[Ticket]) -> bool {
        if tickets.is_empty() {
            return true;
        }
        let k = tickets.len() as u64;
        if k > self.cap {
            return false;
        }
        // Reserve by CAS rather than fetch_add: an unconditional
        // fetch_add cannot be taken back if there turns out to be no
        // room, and a reserved-but-unpublished position stalls the
        // consumer at that hole forever.
        let mut t = self.tail.load(Ordering::Relaxed);
        for _ in 0..64 {
            let conf = self.confirmed.load(Ordering::Acquire);
            if t + k > conf + self.cap {
                return false;
            }
            match self.tail.compare_exchange_weak(
                t,
                t + k,
                Ordering::SeqCst,
                Ordering::Relaxed,
            ) {
                Ok(_) => {
                    for (i, tk) in tickets.iter().enumerate() {
                        self.publish(t + i as u64, *tk);
                    }
                    return true;
                }
                Err(cur) => t = cur,
            }
        }
        false
    }

    /// Take the published prefix. Returns how many tickets were taken.
    ///
    /// **Single consumer**, enforced by a lock taken once per call — not
    /// once per ticket, so producers never touch it. `flush_cycle` can run
    /// on the flusher thread and on a caller inside `drain_fully` at the
    /// same time, and with a lock-free ring that would otherwise be two
    /// consumers racing on `confirmed` and on slot recycling.
    ///
    /// Stops at the first *hole* — a reserved position whose producer has
    /// not published yet. Those tickets are not lost; the next call picks
    /// them up. Their versions are still covered by the producer's
    /// `staged_min` floor until then.
    pub fn drain(&self, out: &mut Vec<Ticket>) -> usize {
        let _consumer = self.consumer.lock().expect("consumer lock poisoned");
        let base = self.confirmed.load(Ordering::Relaxed);
        let t0 = self.tail.load(Ordering::Acquire);
        let mut end = base;
        while end < t0 {
            let slot = &self.slots[(end & self.mask) as usize];
            if slot.seq.load(Ordering::Acquire) != end + 1 {
                break; // hole: reserved, not yet published
            }
            // SAFETY: `seq == end + 1` was stored with Release by the
            // producer after it wrote the data, and this Acquire load
            // pairs with it. The slot is not written again until it is
            // recycled below.
            let t = unsafe { *slot.data.get() };
            out.push(t);
            // Recycle for the next lap.
            slot.seq.store(end + self.cap, Ordering::Release);
            end += 1;
        }
        self.confirmed.store(end, Ordering::Release);
        (end - base) as usize
    }

    /// The lowest version still published-but-undrained.
    ///
    /// Scans the unconfirmed window, which is why the caller gates how
    /// often it runs (see `Store::watermark_due`). After a drain the
    /// window is normally empty, so this is normally free.
    pub fn min_version(&self) -> u64 {
        let base = self.confirmed.load(Ordering::Acquire);
        let t0 = self.tail.load(Ordering::Acquire);
        let mut m = NO_FLOOR;
        let mut p = base;
        while p < t0 {
            let slot = &self.slots[(p & self.mask) as usize];
            if slot.seq.load(Ordering::Acquire) == p + 1 {
                // SAFETY: published, as in `drain`. This only reads; the
                // slot is not recycled here, so the consumer will still
                // see it.
                let v = unsafe { (*slot.data.get()).version };
                if v < m {
                    m = v;
                }
            }
            p += 1;
        }
        m
    }

    /// Tickets reserved but not yet drained.
    pub fn len(&self) -> usize {
        let t = self.tail.load(Ordering::Acquire);
        let c = self.confirmed.load(Ordering::Acquire);
        (t - c) as usize
    }

    /// Whether the log is empty.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Release anyone waiting for a slot so shutdown can proceed.
    pub fn stop(&self) {
        self.stopping.store(true, Ordering::Release);
    }
}

/// Issues versions. Starts at [`FIRST_VERSION`], never 0.
#[derive(Debug)]
pub struct VersionCounter(AtomicU64);

impl Default for VersionCounter {
    fn default() -> Self {
        Self::new()
    }
}

impl VersionCounter {
    /// A counter positioned to issue [`FIRST_VERSION`] first.
    pub fn new() -> Self {
        Self(AtomicU64::new(FIRST_VERSION))
    }

    /// Draw the next version.
    ///
    /// `SeqCst` to pair with the announce store: downgrading this to
    /// `Relaxed` reads as harmless and is invisible on x86, then breaks
    /// on weaker orderings — green in CI, wrong in production.
    pub fn draw(&self) -> Version {
        self.0.fetch_add(1, Ordering::SeqCst)
    }

    /// The next version that *would* be drawn.
    pub fn peek(&self) -> Version {
        self.0.load(Ordering::SeqCst)
    }
}

/// The durability watermark: every version at or below it is in the
/// durable store.
///
/// A **low-water** mark. The natural misreading is "the newest version
/// written back", which is trivially wrong because writeback drains an
/// unordered chunk of the dirty map, never a version prefix.
#[derive(Debug, Default)]
pub struct Watermark(AtomicU64);

impl Watermark {
    /// A watermark covering nothing.
    pub fn new() -> Self {
        Self(AtomicU64::new(0))
    }

    /// The current value.
    pub fn get(&self) -> Version {
        self.0.load(Ordering::Acquire)
    }

    /// Advance to `floor - 1`, never backwards.
    ///
    /// Two hazards handled here, both of which are silent:
    ///
    /// * `saturating_sub`, because a floor of 0 would otherwise wrap to
    ///   `u64::MAX` **in release only** (Rust wraps in release, panics in
    ///   debug) and make every value instantly "durable".
    /// * `fetch_max`, because the lazy-recompute path can produce a
    ///   *stale smaller* floor, and a plain store would walk the
    ///   watermark backwards, un-durabling values the sweeper has already
    ///   evicted.
    pub fn advance_to_floor(&self, floor: u64) {
        debug_assert!(floor >= 1, "versions start at 1; floor 0 would wrap");
        let candidate = floor.saturating_sub(1);
        self.0.fetch_max(candidate, Ordering::AcqRel);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;

    #[test]
    fn watermark_never_wraps_on_an_empty_store() {
        let w = Watermark::new();
        w.advance_to_floor(FIRST_VERSION); // floor == 1
        assert_eq!(w.get(), 0, "must be 0, not u64::MAX");
    }

    #[test]
    fn watermark_is_monotone() {
        let w = Watermark::new();
        w.advance_to_floor(100);
        assert_eq!(w.get(), 99);
        w.advance_to_floor(10); // a stale, smaller floor
        assert_eq!(w.get(), 99, "must never walk backwards");
    }

    #[test]
    fn counter_starts_at_one() {
        let c = VersionCounter::new();
        assert_eq!(c.draw(), FIRST_VERSION);
        assert_eq!(c.draw(), FIRST_VERSION + 1);
    }

    #[test]
    fn announce_floor_bounds_the_minimum() {
        let slots = vec![WriterSlot::new(), WriterSlot::new()];
        assert_eq!(Floors::min_over(&slots, 500), 500, "no floors armed");
        slots[1].arm(42);
        assert_eq!(Floors::min_over(&slots, 500), 42);
        slots[1].disarm();
        assert_eq!(Floors::min_over(&slots, 500), 500);
    }

    #[test]
    fn submit_covers_the_version_at_every_instant() {
        let s = WriterSlot::new();
        s.arm(10);
        // Between arm and submit the announce floor is the only cover.
        assert_eq!(Floors::min_over(std::slice::from_ref(&s), 999), 10);
        let full = s.submit(Ticket { entry: 0, version: 10 }, 64);
        assert!(full.is_none(), "batch of 64 should not be full yet");
        // announce is released, but batch_min now covers it.
        assert_eq!(Floors::min_over(std::slice::from_ref(&s), 999), 10);
    }

    #[test]
    fn a_full_batch_hands_over_to_staged_without_a_gap() {
        let s = WriterSlot::new();
        for v in 1..=2u64 {
            s.arm(v);
            let full = s.submit(Ticket { entry: 0, version: v }, 2);
            if v == 2 {
                assert!(full.is_some(), "batch of 2 should be full");
            }
        }
        // batch_min has been reset, so staged_min must be holding v1.
        assert_eq!(Floors::min_over(std::slice::from_ref(&s), 999), 1);
        s.clear_staged();
        assert_eq!(Floors::min_over(std::slice::from_ref(&s), 999), 999);
    }

    #[test]
    fn steal_takes_a_partial_batch_and_keeps_it_covered() {
        let s = WriterSlot::new();
        s.arm(7);
        s.submit(Ticket { entry: 3, version: 7 }, 64);
        let taken = s.steal().expect("a partial batch is stealable");
        assert_eq!(taken.len(), 1);
        assert_eq!(
            Floors::min_over(std::slice::from_ref(&s), 999),
            7,
            "staged_min must still cover it after the steal"
        );
    }

    #[test]
    fn the_steal_fast_path_never_skips_a_nonempty_batch() {
        // The unlocked pre-check is only safe if "batch_min is NO_FLOOR"
        // really does imply "nothing to take". If those ever drift apart,
        // an acked write sits in a batch the flusher stops visiting and
        // the watermark is pinned for the life of the process.
        let s = WriterSlot::new();
        assert!(s.steal().is_none(), "a fresh slot has nothing");
        s.arm(5);
        s.submit(Ticket { entry: 1, version: 5 }, 64);
        assert!(
            s.steal().is_some(),
            "the fast path skipped a batch holding an acked write"
        );
        assert!(s.steal().is_none(), "and it is empty afterwards");
    }

    #[test]
    fn log_try_append_refuses_rather_than_blocking() {
        let log = TicketLog::new(2);
        // (the ring rounds capacity up to a power of two)
        assert!(log.try_append(&[Ticket { entry: 0, version: 1 }]));
        assert!(log.try_append(&[Ticket { entry: 0, version: 2 }]));
        assert!(
            !log.try_append(&[Ticket { entry: 0, version: 3 }]),
            "the flusher's own append must never block on backpressure"
        );
    }

    #[test]
    fn the_ring_never_loses_or_duplicates_a_ticket() {
        // The property the whole design rests on: producers never drop
        // and the consumer never double-takes. Run it with more producers
        // than cores and a ring far too small, so slots are recycled
        // under contention many times over.
        const PRODUCERS: u64 = 8;
        // Miri interprets every instruction, so the stress size that is
        // right for a real run never finishes under it. Miri is not here
        // to find the rare interleaving anyway — it is here to prove the
        // unsafe is sound on the interleavings it does explore.
        #[cfg(miri)]
        const PER: u64 = 40;
        #[cfg(not(miri))]
        const PER: u64 = 20_000;
        let log = Arc::new(TicketLog::new(64));
        let seen = Arc::new(Mutex::new(Vec::new()));
        let done = Arc::new(AtomicU64::new(0));

        let consumer = {
            let log = Arc::clone(&log);
            let seen = Arc::clone(&seen);
            let done = Arc::clone(&done);
            std::thread::spawn(move || {
                let mut out = Vec::new();
                loop {
                    let finished = done.load(Ordering::Acquire) == PRODUCERS;
                    log.drain(&mut out);
                    if finished && log.is_empty() {
                        // One last pass for anything published between
                        // the two checks.
                        log.drain(&mut out);
                        break;
                    }
                    std::hint::spin_loop();
                }
                seen.lock().unwrap().extend(out);
            })
        };

        let mut hs = Vec::new();
        for t in 0..PRODUCERS {
            let log = Arc::clone(&log);
            let done = Arc::clone(&done);
            hs.push(std::thread::spawn(move || {
                for i in 0..PER {
                    log.append(&[Ticket {
                        entry: t as u32,
                        version: t * PER + i + 1,
                    }]);
                }
                done.fetch_add(1, Ordering::Release);
            }));
        }
        for h in hs {
            h.join().unwrap();
        }
        consumer.join().unwrap();

        let mut got: Vec<u64> =
            seen.lock().unwrap().iter().map(|t| t.version).collect();
        let n = got.len();
        got.sort_unstable();
        got.dedup();
        assert_eq!(
            got.len(),
            (PRODUCERS * PER) as usize,
            "ring lost or duplicated tickets ({n} taken, {} distinct)",
            got.len()
        );
        assert_eq!(got[0], 1);
        assert_eq!(*got.last().unwrap(), PRODUCERS * PER);
    }

    #[test]
    fn batches_are_reserved_contiguously() {
        let log = TicketLog::new(1024);
        let batch: Vec<Ticket> = (1..=64)
            .map(|v| Ticket { entry: 7, version: v })
            .collect();
        log.append(&batch);
        let mut out = Vec::new();
        assert_eq!(log.drain(&mut out), 64);
        assert_eq!(out.len(), 64);
        // A single fetch_add per batch means the batch lands in order.
        assert!(out.windows(2).all(|w| w[0].version < w[1].version));
    }

    #[test]
    fn a_hole_stops_the_drain_without_losing_what_follows() {
        // A producer that has reserved but not published blocks the
        // prefix. The tickets behind it must NOT be skipped -- skipping
        // would drop an acked write -- and must arrive once it lands.
        let log = Arc::new(TicketLog::new(64));
        let started = Arc::new(AtomicU64::new(0));
        let release = Arc::new(AtomicBool::new(false));

        // Occupy position 0 without publishing, by holding a reservation
        // in another thread that waits before writing.
        let slow = {
            let log = Arc::clone(&log);
            let started = Arc::clone(&started);
            let release = Arc::clone(&release);
            std::thread::spawn(move || {
                let pos = log.tail.fetch_add(1, Ordering::SeqCst);
                started.store(1, Ordering::Release);
                while !release.load(Ordering::Acquire) {
                    std::hint::spin_loop();
                }
                log.publish(pos, Ticket { entry: 0, version: 1 });
            })
        };
        while started.load(Ordering::Acquire) == 0 {
            std::hint::spin_loop();
        }
        log.append(&[Ticket { entry: 1, version: 2 }]);

        let mut out = Vec::new();
        assert_eq!(log.drain(&mut out), 0, "the hole must stop the drain");
        assert!(out.is_empty());

        release.store(true, Ordering::Release);
        slow.join().unwrap();
        assert_eq!(log.drain(&mut out), 2, "and both arrive afterwards");
        assert_eq!(out[0].version, 1);
        assert_eq!(out[1].version, 2);
    }

    #[test]
    fn log_reports_its_minimum_for_the_watermark() {
        let log = TicketLog::new(8);
        assert_eq!(log.min_version(), NO_FLOOR);
        log.try_append(&[
            Ticket { entry: 0, version: 9 },
            Ticket { entry: 1, version: 4 },
        ]);
        assert_eq!(log.min_version(), 4);
        let mut out = Vec::new();
        log.drain(&mut out);
        assert_eq!(
            log.min_version(),
            NO_FLOOR,
            "a drained ring constrains the watermark with nothing"
        );
    }
}
