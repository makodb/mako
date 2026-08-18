//! Durability bookkeeping: version issuance, the per-writer floors, the
//! dirty-ticket log, and the watermark.
//!
//! This is the part of the cache that is easy to write and hard to write
//! *correctly*. Nearly every subtlety here corresponds to a specific way
//! an idiomatic translation loses a property silently.

use std::collections::VecDeque;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Condvar, Mutex};

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
                let taken = std::mem::take(&mut *b);
                let min = taken.iter().map(|x| x.version).min().unwrap_or(NO_FLOOR);
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

/// The shared dirty-ticket log.
///
/// A plain mutex-guarded deque: this is the *semantics* of the C++ ring,
/// with the lock-free machinery left for later. What matters here is the
/// contract, which the tests pin:
///
/// * appending **waits**, never drops. A dropped ticket is an acked write
///   the flusher never learns about, which is silent data loss under
///   exactly the overload the cache exists to absorb. There is
///   deliberately no `try_append`.
/// * the flusher's own append must never block on that backpressure —
///   see [`TicketLog::try_append`], used only by the steal path, which
///   would otherwise deadlock the flusher against the queue it exists to
///   drain.
#[derive(Debug)]
pub struct TicketLog {
    inner: Mutex<LogInner>,
    space: Condvar,
    cap: usize,
}

#[derive(Debug)]
struct LogInner {
    q: VecDeque<Ticket>,
    /// Monotonic non-decreasing deque of minimum candidates, so
    /// [`TicketLog::min_version`] is O(1) instead of a walk of `q`.
    ///
    /// The walk mattered: `min_version` runs once per flusher cycle while
    /// holding the same lock producers append under, and `q` holds up to
    /// `log_slots` (a million by default) tickets whenever writers are
    /// ahead of writeback — which is exactly the state the cache exists
    /// to be in. It was the write path's remaining scaling bottleneck
    /// after the entry table.
    ///
    /// The invariant: `mins` holds a non-decreasing subsequence of `q`'s
    /// versions whose front is the minimum over all of `q`. Pushing pops
    /// every strictly larger candidate (they can never be the minimum
    /// again, because this one outlives them); popping removes the front
    /// only when the value leaving `q` is the one `mins` is holding.
    /// Equal values are kept, so duplicates survive as many pops as
    /// there are copies. Amortised O(1) per ticket.
    mins: VecDeque<Version>,
    stopping: bool,
}

impl LogInner {
    fn push(&mut self, t: Ticket) {
        while self.mins.back().is_some_and(|b| *b > t.version) {
            self.mins.pop_back();
        }
        self.mins.push_back(t.version);
        self.q.push_back(t);
    }

    fn pop(&mut self) -> Option<Ticket> {
        let t = self.q.pop_front()?;
        if self.mins.front() == Some(&t.version) {
            self.mins.pop_front();
        }
        Some(t)
    }

    fn min(&self) -> u64 {
        self.mins.front().copied().unwrap_or(NO_FLOOR)
    }
}

impl TicketLog {
    /// A log holding at most `cap` un-drained tickets.
    pub fn new(cap: usize) -> Self {
        Self {
            inner: Mutex::new(LogInner {
                q: VecDeque::new(),
                mins: VecDeque::new(),
                stopping: false,
            }),
            space: Condvar::new(),
            cap,
        }
    }

    /// Append, waiting for room. Never drops.
    pub fn append(&self, tickets: &[Ticket]) {
        let mut g = self.inner.lock().expect("log mutex poisoned");
        for t in tickets {
            while g.q.len() >= self.cap && !g.stopping {
                g = self.space.wait(g).expect("log condvar poisoned");
            }
            g.push(*t);
        }
    }

    /// Append only if there is room right now.
    ///
    /// The flusher uses this: blocking here would deadlock it against the
    /// backpressure only it can relieve.
    pub fn try_append(&self, tickets: &[Ticket]) -> bool {
        let mut g = self.inner.lock().expect("log mutex poisoned");
        if g.q.len() + tickets.len() > self.cap {
            return false;
        }
        for t in tickets {
            g.push(*t);
        }
        true
    }

    /// Drain up to `budget` tickets.
    pub fn drain(&self, budget: usize, out: &mut Vec<Ticket>) -> usize {
        let mut g = self.inner.lock().expect("log mutex poisoned");
        let n = budget.min(g.q.len());
        for _ in 0..n {
            out.push(g.pop().expect("q holds at least n tickets"));
        }
        if n > 0 {
            self.space.notify_all();
        }
        n
    }

    /// The lowest version still sitting in the log.
    pub fn min_version(&self) -> u64 {
        self.inner.lock().expect("log mutex poisoned").min()
    }

    /// Number of un-drained tickets.
    pub fn len(&self) -> usize {
        self.inner.lock().expect("log mutex poisoned").q.len()
    }

    /// Whether the log is empty.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Release anyone blocked in [`TicketLog::append`] so shutdown can
    /// proceed.
    pub fn stop(&self) {
        let mut g = self.inner.lock().expect("log mutex poisoned");
        g.stopping = true;
        self.space.notify_all();
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
        assert!(log.try_append(&[Ticket { entry: 0, version: 1 }]));
        assert!(log.try_append(&[Ticket { entry: 0, version: 2 }]));
        assert!(
            !log.try_append(&[Ticket { entry: 0, version: 3 }]),
            "the flusher's own append must never block on backpressure"
        );
    }

    #[test]
    fn incremental_minimum_matches_a_brute_force_walk() {
        // The monotonic deque is the kind of optimisation that is right
        // for a thousand operations and wrong for one specific
        // interleaving, so it is checked against the definition rather
        // than against itself.
        let log = TicketLog::new(4096);
        let mut shadow: std::collections::VecDeque<u64> =
            std::collections::VecDeque::new();
        // Deterministic pseudo-random with plenty of duplicates and
        // descending runs, which is where a naive version breaks.
        let mut x: u64 = 0x2545_F491_4F6C_DD1D;
        for step in 0..20_000 {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            if step % 3 == 2 && !shadow.is_empty() {
                let n = (x as usize % shadow.len()) + 1;
                let mut out = Vec::new();
                log.drain(n, &mut out);
                for _ in 0..n {
                    shadow.pop_front();
                }
            } else {
                let v = (x % 50) + 1;
                assert!(log.try_append(&[Ticket { entry: 0, version: v }]));
                shadow.push_back(v);
            }
            let want = shadow.iter().copied().min().unwrap_or(NO_FLOOR);
            assert_eq!(log.min_version(), want, "diverged at step {step}");
        }
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
    }
}
