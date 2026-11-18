//! Mako's local hybrid logical clock (HLC), the general certification gate,
//! and the dense cache sequence, in safe Rust.
//!
//! This is a port of the clock in `src/mako/sto/Transaction.cc`: one 64-bit
//! word holds `(stamp << 1) | general_gate`, where the stamp packs a 44-bit
//! Unix millisecond reading and a 19-bit logical counter, and a second word
//! holds the dense cache sequence. Both engines must produce identical
//! stamps for identical physical readings, so the port keeps every rule:
//!
//! - allocation takes `max(physical << 19, previous + 1)`, never reuses a
//!   stamp, and reports exhaustion at the 63-bit ceiling;
//! - the restricted profile refuses to allocate while the general gate is
//!   held (`GeneralLocked`) instead of waiting;
//! - the general gate is a spin-acquired bit that preserves the stamp;
//! - the order pair allocates the timestamp first and the sequence second so
//!   a failed sequence leaves a harmless timestamp gap, never a queue hole;
//! - `order_validation_prefix` is an acquire RMW cut on the packed word;
//! - reseeding the sequence is refused while the gate is held and never
//!   lowers the clock;
//! - `advance_past` raises the floor so the next allocation compares strictly
//!   after the observed full timestamp, including the origin tie-break.
//!
//! The clock is process-global in production ([`global`]) because one process
//! owns one cache-order namespace. Tests construct private [`Clock`]s.
//!
//! With the default `distributed` feature, distributed transaction bases and
//! decimal terms use an independent authority, never HLC stamps or cache sequences.

#![forbid(unsafe_code)]

#[cfg(feature = "distributed")]
pub mod distributed;

use std::num::NonZeroU32;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

pub use mako_engine_api::MakoTimestamp;

/// Allocator origin of every timestamp minted by this process's local clock.
pub const ORIGIN: NonZeroU32 = match NonZeroU32::new(1) {
    Some(origin) => origin,
    None => unreachable!(),
};

/// Number of logical-counter bits in a stamp.
pub const LOGICAL_BITS: u32 = MakoTimestamp::HOT_LOGICAL_BITS;
/// Mask of the logical counter.
pub const LOGICAL_MASK: u64 = MakoTimestamp::HOT_LOGICAL_MASK;
/// Largest representable physical millisecond reading.
pub const PHYSICAL_MS_MAX: u64 = MakoTimestamp::HOT_PHYSICAL_MS_MAX;
/// Largest valid stamp; the word's top bit is never part of a stamp.
pub const STAMP_MAX: u64 = MakoTimestamp::HOT_STAMP_MAX;
/// Largest dense cache sequence.
pub const SEQUENCE_MAX: u64 = (1u64 << 48) - 1;

const STAMP_SHIFT: u32 = 1;
const GENERAL_LOCK: u64 = 1;
const PHYSICAL_UNIT_US: u64 = 1_000;

/// Whether `stamp` is a valid allocated stamp (nonzero, within 63 bits).
#[inline]
pub const fn valid_stamp(stamp: u64) -> bool {
    stamp != 0 && stamp <= STAMP_MAX
}

/// Expand a private stamp into the public timestamp minted by this clock.
#[inline]
pub const fn expand(stamp: u64) -> Option<MakoTimestamp> {
    MakoTimestamp::from_hot_stamp(stamp, ORIGIN)
}

#[inline]
const fn stamp_of(word: u64) -> u64 {
    word >> STAMP_SHIFT
}

#[inline]
const fn with_stamp(word: u64, stamp: u64) -> u64 {
    (stamp << STAMP_SHIFT) | (word & GENERAL_LOCK)
}

/// Result of one stamp allocation.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StampAllocation {
    /// The clock advanced and minted this stamp.
    Accepted(u64),
    /// The restricted profile found the general gate held; retry later.
    GeneralLocked,
    /// The stamp space is exhausted or the physical clock is unreadable.
    TimestampExhausted,
}

/// Result of allocating a `(sequence, stamp)` cache-order pair.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OrderPairAllocation {
    /// Both values were minted; the sequence follows the timestamp.
    Accepted {
        /// Dense cache sequence.
        sequence: u64,
        /// Private HLC stamp.
        stamp: u64,
    },
    /// The general gate is held.
    GeneralLocked,
    /// No timestamp could be minted.
    TimestampExhausted,
    /// A timestamp was minted but the dense sequence is exhausted; the
    /// timestamp gap is harmless.
    SequenceExhausted,
}

/// Physical clock reading used as the HLC's wall input.
///
/// Monotonicity of the HLC never depends on this source being monotonic.
pub struct PhysicalClock {
    #[cfg(feature = "test-hooks")]
    override_ms: AtomicU64,
}

#[cfg(feature = "test-hooks")]
const NO_OVERRIDE: u64 = u64::MAX;

impl PhysicalClock {
    /// A clock reading the system's real-time clock.
    pub const fn new() -> Self {
        Self {
            #[cfg(feature = "test-hooks")]
            override_ms: AtomicU64::new(NO_OVERRIDE),
        }
    }

    /// Current Unix time in milliseconds, or `None` if unrepresentable.
    #[inline]
    pub fn read_unix_ms(&self) -> Option<u64> {
        #[cfg(feature = "test-hooks")]
        {
            let overridden = self.override_ms.load(Ordering::Acquire);
            if overridden != NO_OVERRIDE {
                return Some(overridden);
            }
        }
        let since_epoch = SystemTime::now().duration_since(UNIX_EPOCH).ok()?;
        let unix_ms = u64::try_from(since_epoch.as_millis()).ok()?;
        (unix_ms <= PHYSICAL_MS_MAX).then_some(unix_ms)
    }

    /// Pin the physical reading for tests. Returns false when unrepresentable.
    #[cfg(feature = "test-hooks")]
    pub fn set_test_unix_ms(&self, unix_ms: u64) -> bool {
        if unix_ms > PHYSICAL_MS_MAX {
            return false;
        }
        self.override_ms.store(unix_ms, Ordering::Release);
        true
    }

    /// Remove the test override.
    #[cfg(feature = "test-hooks")]
    pub fn clear_test_unix_ms(&self) {
        self.override_ms.store(NO_OVERRIDE, Ordering::Release);
    }
}

impl Default for PhysicalClock {
    fn default() -> Self {
        Self::new()
    }
}

/// One HLC/gate word plus one dense sequence word.
pub struct Clock {
    hlc: AtomicU64,
    sequence: AtomicU64,
    physical: PhysicalClock,
}

impl Default for Clock {
    fn default() -> Self {
        Self::new()
    }
}

impl Clock {
    /// A fresh clock: stamp zero (unassigned), gate free, sequence zero.
    pub const fn new() -> Self {
        Self {
            hlc: AtomicU64::new(0),
            sequence: AtomicU64::new(0),
            physical: PhysicalClock::new(),
        }
    }

    /// The physical clock feeding this HLC.
    pub fn physical(&self) -> &PhysicalClock {
        &self.physical
    }

    /// Opaque address of the packed HLC word for a non-binding cache hint.
    ///
    /// The address is valid only while this exact `Clock` remains alive and
    /// unmoved. It grants no permission to load, store, or dereference the
    /// word. Callers may only submit a non-faulting hardware prefetch hint;
    /// timestamp and gate operations must still use the clock methods.
    #[doc(hidden)]
    #[inline]
    pub fn prefetch_address(&self) -> *const u8 {
        std::ptr::from_ref(&self.hlc).cast()
    }

    /// Current stamp, or zero if nothing was ever allocated.
    pub fn current_stamp(&self) -> u64 {
        stamp_of(self.hlc.load(Ordering::Acquire))
    }

    /// Whether the general certification gate is currently held.
    pub fn general_gate_held(&self) -> bool {
        self.hlc.load(Ordering::Acquire) & GENERAL_LOCK != 0
    }

    fn allocate_stamp(&self, reject_general_gate: bool) -> StampAllocation {
        let Some(wall_ms) = self.physical.read_unix_ms() else {
            return StampAllocation::TimestampExhausted;
        };
        if wall_ms > PHYSICAL_MS_MAX {
            return StampAllocation::TimestampExhausted;
        }
        let physical_candidate = wall_ms << LOGICAL_BITS;
        let mut current = self.hlc.load(Ordering::Acquire);
        loop {
            if reject_general_gate && current & GENERAL_LOCK != 0 {
                return StampAllocation::GeneralLocked;
            }
            let previous = stamp_of(current);
            let mut next = physical_candidate;
            if next <= previous {
                if previous == STAMP_MAX {
                    return StampAllocation::TimestampExhausted;
                }
                next = previous + 1;
            }
            if !valid_stamp(next) {
                return StampAllocation::TimestampExhausted;
            }
            let desired = with_stamp(current, next);
            match self.hlc.compare_exchange_weak(
                current,
                desired,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) => return StampAllocation::Accepted(next),
                Err(observed) => current = observed,
            }
        }
    }

    /// Mint the next stamp for the general profile (may run under the gate).
    pub fn try_allocate_timestamp(&self) -> Option<u64> {
        match self.allocate_stamp(false) {
            StampAllocation::Accepted(stamp) => Some(stamp),
            _ => None,
        }
    }

    /// Mint the next stamp for the restricted profile: refuses while the
    /// general gate is held instead of waiting.
    pub fn try_allocate_restricted_timestamp(&self) -> StampAllocation {
        self.allocate_stamp(true)
    }

    /// Mint the next dense cache sequence.
    pub fn try_allocate_sequence(&self) -> Option<u64> {
        let mut current = self.sequence.load(Ordering::Acquire);
        loop {
            if current >= SEQUENCE_MAX {
                return None;
            }
            let desired = current + 1;
            match self.sequence.compare_exchange_weak(
                current,
                desired,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) => return Some(desired),
                Err(observed) => current = observed,
            }
        }
    }

    /// Mint the next dense sequence while the caller holds the general gate.
    pub fn try_allocate_locked_sequence(&self) -> Option<u64> {
        debug_assert!(self.general_gate_held());
        self.try_allocate_sequence()
    }

    /// Mint a `(stamp, sequence)` pair for a restricted one-put commit:
    /// timestamp first, then sequence.
    pub fn try_allocate_order_pair(&self) -> OrderPairAllocation {
        match self.allocate_stamp(true) {
            StampAllocation::Accepted(stamp) => match self.try_allocate_sequence() {
                Some(sequence) => OrderPairAllocation::Accepted { sequence, stamp },
                None => OrderPairAllocation::SequenceExhausted,
            },
            StampAllocation::GeneralLocked => OrderPairAllocation::GeneralLocked,
            StampAllocation::TimestampExhausted => OrderPairAllocation::TimestampExhausted,
        }
    }

    /// Acquire the general certification gate, spinning while it is held.
    ///
    /// Callers already hold their complete write set, so waiting here cannot
    /// create a lock-order cycle.
    pub fn enter_general(&self) {
        let mut current = self.hlc.load(Ordering::Acquire);
        loop {
            if current & GENERAL_LOCK != 0 {
                std::hint::spin_loop();
                current = self.hlc.load(Ordering::Acquire);
                continue;
            }
            match self.hlc.compare_exchange_weak(
                current,
                current | GENERAL_LOCK,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) => return,
                Err(observed) => current = observed,
            }
        }
    }

    /// Release the general gate, preserving the stamp.
    pub fn leave_general(&self) {
        let previous = self.hlc.fetch_and(!GENERAL_LOCK, Ordering::Release);
        debug_assert!(previous & GENERAL_LOCK != 0, "general gate was not held");
    }

    /// Acquire the gate for a scope.
    pub fn general(&self) -> GeneralGate<'_> {
        self.enter_general();
        GeneralGate { clock: self }
    }

    /// An acquire RMW that cuts the packed word's modification order; used by
    /// read-only commits to order themselves after every preceding writer.
    pub fn order_validation_prefix(&self) -> u64 {
        self.hlc.fetch_add(0, Ordering::Acquire)
    }

    /// Cold snapshot of the dense sequence.
    pub fn sequence_snapshot(&self) -> u64 {
        self.sequence.load(Ordering::Acquire)
    }

    /// Reset only the dense sequence during namespace admission. Refused
    /// above the maximum or while the gate is held. Never lowers the clock.
    pub fn reseed_sequence(&self, sequence: u64) -> bool {
        if sequence > SEQUENCE_MAX {
            return false;
        }
        if self.general_gate_held() {
            return false;
        }
        self.sequence.store(sequence, Ordering::Release);
        true
    }

    /// Catch the clock up to an observed private stamp.
    pub fn observe_stamp(&self, observed_stamp: u64) {
        if !valid_stamp(observed_stamp) {
            return;
        }
        let mut current = self.hlc.load(Ordering::Acquire);
        while stamp_of(current) < observed_stamp {
            let next = with_stamp(current, observed_stamp);
            match self
                .hlc
                .compare_exchange_weak(current, next, Ordering::AcqRel, Ordering::Acquire)
            {
                Ok(_) => return,
                Err(observed) => current = observed,
            }
        }
    }

    /// Raise the floor so the next minted timestamp compares strictly after
    /// `observed`. Returns false when no later timestamp remains mintable.
    pub fn advance_past(&self, observed: MakoTimestamp) -> bool {
        let physical_ms = observed.physical_us() / PHYSICAL_UNIT_US;
        let remainder = observed.physical_us() % PHYSICAL_UNIT_US;
        if physical_ms > PHYSICAL_MS_MAX {
            return false;
        }
        let logical = u64::from(observed.logical());
        let mut minimum_next;
        if remainder != 0 || logical > LOGICAL_MASK {
            if physical_ms == PHYSICAL_MS_MAX {
                return false;
            }
            minimum_next = (physical_ms + 1) << LOGICAL_BITS;
        } else {
            let base = physical_ms << LOGICAL_BITS;
            // A same-tuple stamp minted here is already after `observed` when
            // this clock's origin is greater.
            let same_tuple_is_after = ORIGIN.get() > observed.origin();
            if same_tuple_is_after {
                minimum_next = base | logical;
            } else if logical < LOGICAL_MASK {
                minimum_next = base | (logical + 1);
            } else {
                if physical_ms == PHYSICAL_MS_MAX {
                    return false;
                }
                minimum_next = (physical_ms + 1) << LOGICAL_BITS;
            }
        }
        // Public timestamps may validly be (0, 0, origin); private stamp zero
        // is the unassigned sentinel, so advance to the first hot stamp.
        if minimum_next == 0 {
            minimum_next = 1;
        }
        if !valid_stamp(minimum_next) {
            return false;
        }
        let floor = minimum_next - 1;
        let mut current = self.hlc.load(Ordering::Acquire);
        while stamp_of(current) < floor {
            let next = with_stamp(current, floor);
            match self
                .hlc
                .compare_exchange_weak(current, next, Ordering::AcqRel, Ordering::Acquire)
            {
                Ok(_) => return true,
                Err(observed) => current = observed,
            }
        }
        stamp_of(current) < STAMP_MAX
    }
}

/// RAII holder of the general certification gate.
#[must_use = "dropping the gate guard releases the gate"]
pub struct GeneralGate<'clock> {
    clock: &'clock Clock,
}

impl GeneralGate<'_> {
    /// Mint the dense sequence while holding the gate.
    pub fn try_allocate_locked_sequence(&self) -> Option<u64> {
        self.clock.try_allocate_locked_sequence()
    }

    /// Mint the general-profile stamp while holding the gate.
    pub fn try_allocate_timestamp(&self) -> Option<u64> {
        self.clock.try_allocate_timestamp()
    }
}

impl Drop for GeneralGate<'_> {
    fn drop(&mut self) {
        self.clock.leave_general();
    }
}

static GLOBAL: Clock = Clock::new();

/// The process-wide clock: one cache-order namespace per process.
pub fn global() -> &'static Clock {
    &GLOBAL
}

#[cfg(all(test, feature = "test-hooks"))]
mod tests {
    use super::*;
    use std::thread;

    fn clock_at(physical_ms: u64) -> Clock {
        let clock = Clock::new();
        assert!(clock.physical().set_test_unix_ms(physical_ms));
        clock
    }

    fn timestamp(physical_us: u64, logical: u32, origin: u32) -> MakoTimestamp {
        MakoTimestamp::new(physical_us, logical, origin).unwrap()
    }

    #[test]
    fn origin_is_one_and_constants_match_the_public_timestamp() {
        assert_eq!(ORIGIN.get(), 1);
        assert_eq!(LOGICAL_BITS, 19);
        assert_eq!(LOGICAL_MASK, (1 << 19) - 1);
        assert_eq!(PHYSICAL_MS_MAX, (1 << 44) - 1);
        assert_eq!(STAMP_MAX, u64::MAX >> 1);
        assert_eq!(SEQUENCE_MAX, (1 << 48) - 1);
    }

    #[test]
    fn first_allocation_takes_the_physical_reading() {
        let clock = clock_at(2000);
        assert_eq!(clock.try_allocate_timestamp(), Some(2000 << LOGICAL_BITS));
        assert_eq!(
            clock.try_allocate_timestamp(),
            Some((2000 << LOGICAL_BITS) | 1)
        );
        assert!(clock.physical().set_test_unix_ms(2001));
        assert_eq!(clock.try_allocate_timestamp(), Some(2001 << LOGICAL_BITS));
    }

    #[test]
    fn backward_physical_clock_cannot_move_hlc_backward() {
        let clock = clock_at(2000);
        let first = clock.try_allocate_timestamp().unwrap();
        assert!(clock.physical().set_test_unix_ms(1999));
        let second = clock.try_allocate_timestamp().unwrap();
        assert_eq!(second, first + 1);
    }

    #[test]
    fn recovery_floor_precedes_next_allocation() {
        let clock = clock_at(1000);
        assert!(clock.advance_past(timestamp(2000 * PHYSICAL_UNIT_US, 42, 1)));
        assert_eq!(
            clock.try_allocate_timestamp(),
            Some((2000 << LOGICAL_BITS) | 43)
        );

        let clock = clock_at(1000);
        assert!(clock.advance_past(timestamp(0, 0, 2)));
        let next = clock.try_allocate_timestamp().unwrap();
        assert_ne!(next, 0);
    }

    #[test]
    fn recovery_floor_is_not_raised_for_a_greater_origin_at_the_same_tuple() {
        // A stamp equal to the observed tuple minted by origin 1 already
        // compares after the same tuple from origin 0... but origin is
        // NonZero, so every peer origin is >= 1. With ORIGIN == 1 the
        // same-tuple-is-after branch is never taken for origin >= 1: an
        // observed (2000 ms, 42, origin 1) forces logical 43.
        let clock = clock_at(1000);
        assert!(clock.advance_past(timestamp(2000 * PHYSICAL_UNIT_US, 42, 7)));
        assert_eq!(
            clock.try_allocate_timestamp(),
            Some((2000 << LOGICAL_BITS) | 43)
        );
    }

    #[test]
    fn recovery_floor_carries_submillisecond_and_wide_logical_bounds() {
        let observed_ms = 2000;
        let cases = [
            (
                "sub-millisecond physical time",
                timestamp(observed_ms * PHYSICAL_UNIT_US + 1, 0, 1),
                (observed_ms + 1) << LOGICAL_BITS,
            ),
            (
                "logical value above the hot range",
                timestamp(observed_ms * PHYSICAL_UNIT_US, (LOGICAL_MASK + 1) as u32, 1),
                (observed_ms + 1) << LOGICAL_BITS,
            ),
            (
                "logical value at the hot ceiling rolls to the next millisecond",
                timestamp(observed_ms * PHYSICAL_UNIT_US, LOGICAL_MASK as u32, 1),
                (observed_ms + 1) << LOGICAL_BITS,
            ),
        ];
        for (name, observed, expected_next) in cases {
            let clock = clock_at(1000);
            assert!(clock.advance_past(observed), "{name}");
            assert_eq!(
                clock.try_allocate_timestamp(),
                Some(expected_next),
                "{name}"
            );
        }
    }

    #[test]
    fn recovery_floor_never_lowers_the_clock() {
        let clock = clock_at(5000);
        let first = clock.try_allocate_timestamp().unwrap();
        assert!(clock.advance_past(timestamp(1000 * PHYSICAL_UNIT_US, 0, 1)));
        assert_eq!(clock.try_allocate_timestamp(), Some(first + 1));
    }

    #[test]
    fn recovery_leaves_the_final_timestamp_mintable() {
        let clock = clock_at(1);
        clock.observe_stamp(STAMP_MAX - 1);
        assert_eq!(clock.try_allocate_timestamp(), Some(STAMP_MAX));
        assert_eq!(clock.try_allocate_timestamp(), None);
        assert_eq!(
            clock.try_allocate_restricted_timestamp(),
            StampAllocation::TimestampExhausted
        );
    }

    #[test]
    fn recovery_past_the_final_timestamp_reports_exhaustion() {
        let clock = clock_at(1);
        let last_ms = PHYSICAL_MS_MAX;
        assert!(!clock.advance_past(timestamp(last_ms * PHYSICAL_UNIT_US + 1, 0, 1)));
        assert!(!clock.advance_past(timestamp(
            last_ms * PHYSICAL_UNIT_US,
            LOGICAL_MASK as u32,
            1
        )));
        assert!(!clock.advance_past(timestamp((last_ms + 1) * PHYSICAL_UNIT_US, 0, 1)));
        // The last hot stamp itself is still mintable after observing the
        // one before it.
        assert!(clock.advance_past(timestamp(
            last_ms * PHYSICAL_UNIT_US,
            LOGICAL_MASK as u32 - 1,
            1
        )));
        assert_eq!(clock.try_allocate_timestamp(), Some(STAMP_MAX));
    }

    #[test]
    fn unreadable_physical_clock_is_reported_as_exhaustion() {
        let clock = Clock::new();
        assert!(!clock.physical().set_test_unix_ms(PHYSICAL_MS_MAX + 1));
        assert!(clock.physical().set_test_unix_ms(PHYSICAL_MS_MAX));
        assert_eq!(
            clock.try_allocate_timestamp(),
            Some(PHYSICAL_MS_MAX << LOGICAL_BITS)
        );
    }

    #[test]
    fn concurrent_hlc_allocation_is_unique_and_contiguous() {
        const THREADS: usize = 8;
        const PER_THREAD: usize = 4096;
        let clock = clock_at(3000);
        let mut merged: Vec<u64> = thread::scope(|scope| {
            let handles: Vec<_> = (0..THREADS)
                .map(|_| {
                    scope.spawn(|| {
                        (0..PER_THREAD)
                            .map(|_| clock.try_allocate_timestamp().unwrap())
                            .collect::<Vec<_>>()
                    })
                })
                .collect();
            handles
                .into_iter()
                .flat_map(|h| h.join().unwrap())
                .collect()
        });
        merged.sort_unstable();
        let first = 3000 << LOGICAL_BITS;
        for (index, stamp) in merged.iter().enumerate() {
            assert_eq!(*stamp, first + index as u64);
        }
    }

    #[test]
    fn validation_prefix_preserves_hlc_gate_and_sequence() {
        let clock = clock_at(1000);
        clock.observe_stamp(12345);
        assert!(clock.reseed_sequence(77));
        let gate = clock.general();
        let state = (12345u64 << 1) | 1;
        assert_eq!(clock.order_validation_prefix(), state);
        assert_eq!(clock.hlc.load(Ordering::Acquire), state);
        assert_eq!(clock.sequence_snapshot(), 77);
        drop(gate);
        assert_eq!(clock.hlc.load(Ordering::Acquire), 12345u64 << 1);
    }

    #[test]
    fn concurrent_sequence_stops_at_rust_lane_boundary() {
        let clock = Clock::new();
        assert!(clock.reseed_sequence(SEQUENCE_MAX - 1));
        assert_eq!(clock.try_allocate_sequence(), Some(SEQUENCE_MAX));
        assert_eq!(clock.try_allocate_sequence(), None);
        assert!(!clock.reseed_sequence(SEQUENCE_MAX + 1));
    }

    #[test]
    fn restricted_allocation_refuses_while_the_general_gate_is_held() {
        let clock = clock_at(1000);
        let gate = clock.general();
        assert_eq!(
            clock.try_allocate_restricted_timestamp(),
            StampAllocation::GeneralLocked
        );
        assert_eq!(
            clock.try_allocate_order_pair(),
            OrderPairAllocation::GeneralLocked
        );
        // The general profile still allocates under the gate and the gate
        // survives its CAS.
        assert_eq!(gate.try_allocate_timestamp(), Some(1000 << LOGICAL_BITS));
        assert!(clock.general_gate_held());
        assert_eq!(gate.try_allocate_locked_sequence(), Some(1));
        drop(gate);
        assert!(!clock.general_gate_held());
        assert_eq!(
            clock.try_allocate_restricted_timestamp(),
            StampAllocation::Accepted((1000 << LOGICAL_BITS) | 1)
        );
    }

    #[test]
    fn order_pair_allocates_the_timestamp_before_the_sequence() {
        let clock = clock_at(1000);
        assert!(clock.reseed_sequence(SEQUENCE_MAX - 1));
        assert_eq!(
            clock.try_allocate_order_pair(),
            OrderPairAllocation::Accepted {
                sequence: SEQUENCE_MAX,
                stamp: 1000 << LOGICAL_BITS
            }
        );
        assert_eq!(
            clock.try_allocate_order_pair(),
            OrderPairAllocation::SequenceExhausted
        );
        // The failed pair still consumed a timestamp: a harmless gap.
        assert_eq!(clock.current_stamp(), (1000 << LOGICAL_BITS) | 1);
    }

    #[test]
    fn reseed_is_refused_while_the_gate_is_held() {
        let clock = clock_at(1000);
        assert!(clock.reseed_sequence(5));
        let gate = clock.general();
        assert!(!clock.reseed_sequence(9));
        drop(gate);
        assert_eq!(clock.sequence_snapshot(), 5);
        assert!(clock.reseed_sequence(9));
        assert_eq!(clock.try_allocate_sequence(), Some(10));
    }

    #[test]
    fn observe_stamp_ignores_invalid_and_older_stamps() {
        let clock = clock_at(1000);
        clock.observe_stamp(0);
        assert_eq!(clock.current_stamp(), 0);
        clock.observe_stamp(STAMP_MAX + 1);
        assert_eq!(clock.current_stamp(), 0);
        clock.observe_stamp(500);
        clock.observe_stamp(400);
        assert_eq!(clock.current_stamp(), 500);
        assert_eq!(clock.try_allocate_timestamp(), Some(1000 << LOGICAL_BITS));
    }

    #[test]
    fn general_gate_is_mutually_exclusive_under_contention() {
        const THREADS: usize = 8;
        const ROUNDS: usize = 2000;
        let clock = clock_at(1000);
        // Every access happens under the gate with a plain load-then-store,
        // so a lost update proves the gate is broken.
        let counter = AtomicU64::new(0);
        thread::scope(|scope| {
            for _ in 0..THREADS {
                scope.spawn(|| {
                    for _ in 0..ROUNDS {
                        let _gate = clock.general();
                        let value = counter.load(Ordering::Relaxed);
                        std::hint::black_box(());
                        counter.store(value + 1, Ordering::Relaxed);
                    }
                });
            }
        });
        assert_eq!(counter.load(Ordering::Relaxed), (THREADS * ROUNDS) as u64);
        assert!(!clock.general_gate_held());
    }

    #[test]
    fn enter_general_waits_for_the_holder() {
        use std::sync::mpsc;
        use std::time::Duration;
        let clock = clock_at(1000);
        let held = clock.general();
        let (entered_tx, entered_rx) = mpsc::channel();
        thread::scope(|scope| {
            scope.spawn(|| {
                let _gate = clock.general();
                entered_tx.send(()).unwrap();
            });
            assert!(
                entered_rx.recv_timeout(Duration::from_millis(200)).is_err(),
                "a second holder entered the gate while it was held"
            );
            drop(held);
            entered_rx
                .recv_timeout(Duration::from_secs(10))
                .expect("the waiter enters once the holder leaves");
        });
        assert!(!clock.general_gate_held());
    }

    #[test]
    fn expand_round_trips_through_the_public_timestamp() {
        let stamp = (2000 << LOGICAL_BITS) | 42;
        let expanded = expand(stamp).unwrap();
        assert_eq!(expanded.physical_us(), 2000 * PHYSICAL_UNIT_US);
        assert_eq!(expanded.logical(), 42);
        assert_eq!(expanded.origin(), 1);
        assert_eq!(expanded.hot_stamp(), Some(stamp));
        assert!(expand(0).is_none());
        assert!(expand(STAMP_MAX + 1).is_none());
    }

    #[test]
    fn global_clock_is_shared_across_the_process() {
        let first = global().order_validation_prefix();
        let second = global().order_validation_prefix();
        assert_eq!(first, second);
    }
}
