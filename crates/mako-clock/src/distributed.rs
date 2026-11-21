//! Distributed transaction authority, independent of local OCC and the HLC.
//!
//! INSTALL carries an unencoded base. Legacy rows/logs carry `base * 10 + term`.
//! Zero denotes bootstrap/absence at boundaries, not a valid decision. Allocation
//! is unique within this process authority, not a cluster-wide transaction ID.
//! Observe the selected decision before publication, and recovered bases before
//! admitting foreground work. Observing the final valid base succeeds even though
//! no later allocation can succeed. There is deliberately no reset operation.

use std::fmt;
use std::sync::atomic::{AtomicU32, Ordering};

/// Largest base encodable with every supported decimal term.
pub const MAX_BASE: u32 = (u32::MAX - 9) / 10;
const EXHAUSTED: u32 = MAX_BASE + 1;

/// Checked boundary or allocation failure in the distributed namespace.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DistributedClockError {
    /// Zero or a base outside the legacy decimal encoding domain.
    InvalidTimestamp,
    /// The term does not fit one decimal digit.
    InvalidTerm,
    /// This authority cannot reserve another base; exhaustion is permanent.
    Exhausted,
}

impl fmt::Display for DistributedClockError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::InvalidTimestamp => "invalid distributed timestamp",
            Self::InvalidTerm => "distributed term must be in 0..=9",
            Self::Exhausted => "distributed timestamp authority exhausted",
        })
    }
}

impl std::error::Error for DistributedClockError {}

/// Nonzero, checked distributed base (never an OCC generation or HLC stamp).
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct DistributedTimestamp(u32);

impl DistributedTimestamp {
    pub const MIN: Self = Self(1);
    pub const MAX: Self = Self(MAX_BASE);

    pub const fn new(base: u32) -> Result<Self, DistributedClockError> {
        if base == 0 || base > MAX_BASE {
            Err(DistributedClockError::InvalidTimestamp)
        } else {
            Ok(Self(base))
        }
    }

    pub const fn get(self) -> u32 {
        self.0
    }
}

/// Checked decimal term. The wide constructor rejects rather than truncates.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct DistributedTerm(u8);

impl DistributedTerm {
    pub const fn new(term: u32) -> Result<Self, DistributedClockError> {
        if term > 9 {
            Err(DistributedClockError::InvalidTerm)
        } else {
            Ok(Self(term as u8))
        }
    }

    pub const fn get(self) -> u8 {
        self.0
    }
}

/// Coherent selected decision metadata belonging to one payload version.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct DistributedTimeTerm {
    timestamp: DistributedTimestamp,
    term: DistributedTerm,
}

impl DistributedTimeTerm {
    pub const fn new(timestamp: DistributedTimestamp, term: DistributedTerm) -> Self {
        Self { timestamp, term }
    }

    pub const fn timestamp(self) -> DistributedTimestamp {
        self.timestamp
    }

    pub const fn term(self) -> DistributedTerm {
        self.term
    }

    pub const fn encode_legacy(self) -> u32 {
        self.timestamp.0 * 10 + self.term.0 as u32
    }

    pub const fn decode_legacy(encoded: u32) -> Result<Self, DistributedClockError> {
        let timestamp = match DistributedTimestamp::new(encoded / 10) {
            Ok(timestamp) => timestamp,
            Err(error) => return Err(error),
        };
        // Remainder is provably a single digit, not a truncation of a wire term.
        Ok(Self::new(timestamp, DistributedTerm((encoded % 10) as u8)))
    }
}

/// Atomic next-to-return base with a permanent exhausted sentinel.
#[derive(Debug)]
pub struct DistributedClock {
    next: AtomicU32,
}

impl Default for DistributedClock {
    fn default() -> Self {
        Self::new()
    }
}

impl DistributedClock {
    pub const fn new() -> Self {
        Self {
            next: AtomicU32::new(1),
        }
    }

    pub fn reserve(&self) -> Result<DistributedTimestamp, DistributedClockError> {
        self.reserve_at_least(1)
    }

    /// Reserve atomically above the dependency, including concurrent callers.
    /// A terminal dependency permanently saturates the authority.
    pub fn reserve_after(
        &self,
        lower_bound: DistributedTimestamp,
    ) -> Result<DistributedTimestamp, DistributedClockError> {
        self.reserve_at_least(lower_bound.0 + 1)
    }

    fn reserve_at_least(
        &self,
        minimum: u32,
    ) -> Result<DistributedTimestamp, DistributedClockError> {
        let mut current = self.next.load(Ordering::Acquire);
        loop {
            let candidate = current.max(minimum);
            let next = if candidate >= EXHAUSTED {
                EXHAUSTED
            } else {
                candidate + 1
            };
            match self.next.compare_exchange_weak(
                current,
                next,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) if candidate < EXHAUSTED => return Ok(DistributedTimestamp(candidate)),
                Ok(_) => return Err(DistributedClockError::Exhausted),
                Err(observed) => current = observed,
            }
        }
    }

    /// Adopt a valid decision/recovery floor without allocating a new base.
    /// MAX is valid: adoption succeeds, while subsequent reserves fail.
    pub fn observe(&self, timestamp: DistributedTimestamp) {
        self.next.fetch_max(timestamp.0 + 1, Ordering::AcqRel);
    }

    pub fn is_exhausted(&self) -> bool {
        self.next.load(Ordering::Acquire) == EXHAUSTED
    }
}

static GLOBAL_DISTRIBUTED: DistributedClock = DistributedClock::new();

/// The single native process authority for coordinator, participant and replay.
pub fn global_distributed() -> &'static DistributedClock {
    &GLOBAL_DISTRIBUTED
}

/// Maximum selected-payload read base; absence remains explicit.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct DistributedDependencies {
    max_read: Option<DistributedTimestamp>,
}

impl DistributedDependencies {
    pub const fn new() -> Self {
        Self { max_read: None }
    }

    pub fn observe_read(&mut self, decision: DistributedTimeTerm) {
        self.max_read = Some(
            self.max_read
                .map_or(decision.timestamp, |old| old.max(decision.timestamp)),
        );
    }

    pub const fn max_read_timestamp(&self) -> Option<DistributedTimestamp> {
        self.max_read
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Barrier;
    use std::thread;

    fn base(value: u32) -> DistributedTimestamp {
        DistributedTimestamp::new(value).unwrap()
    }

    #[test]
    fn checked_decimal_boundaries_round_trip() {
        for value in [1, 2, MAX_BASE - 1, MAX_BASE] {
            for term in 0..=9 {
                let decision =
                    DistributedTimeTerm::new(base(value), DistributedTerm::new(term).unwrap());
                assert_eq!(decision.encode_legacy(), value * 10 + term);
                assert_eq!(
                    DistributedTimeTerm::decode_legacy(decision.encode_legacy()),
                    Ok(decision)
                );
            }
        }
        for invalid in [0, MAX_BASE + 1, u32::MAX] {
            assert_eq!(
                DistributedTimestamp::new(invalid),
                Err(DistributedClockError::InvalidTimestamp)
            );
        }
        for invalid in [10, 255, 256, u32::MAX] {
            assert_eq!(
                DistributedTerm::new(invalid),
                Err(DistributedClockError::InvalidTerm)
            );
        }
        let clock = DistributedClock::new();
        for invalid in (0..10).chain((MAX_BASE * 10 + 10)..=u32::MAX) {
            assert_eq!(
                DistributedTimeTerm::decode_legacy(invalid),
                Err(DistributedClockError::InvalidTimestamp)
            );
        }
        assert_eq!(clock.reserve(), Ok(DistributedTimestamp::MIN));
    }

    #[test]
    fn floors_never_lower_and_last_reservation_saturates() {
        let clock = DistributedClock::new();
        assert_eq!(clock.reserve(), Ok(base(1)));
        clock.observe(base(100));
        clock.observe(base(2));
        assert_eq!(clock.reserve(), Ok(base(101)));
        assert_eq!(clock.reserve_after(base(50)), Ok(base(102)));
        assert_eq!(
            clock.reserve_after(base(MAX_BASE - 1)),
            Ok(DistributedTimestamp::MAX)
        );
        assert!(clock.is_exhausted());
        clock.observe(base(1));
        assert_eq!(clock.reserve(), Err(DistributedClockError::Exhausted));
        assert_eq!(
            clock.reserve_after(base(1)),
            Err(DistributedClockError::Exhausted)
        );
    }

    #[test]
    fn terminal_decision_adoption_succeeds_without_allocation() {
        let clock = DistributedClock::new();
        let decision =
            DistributedTimeTerm::new(DistributedTimestamp::MAX, DistributedTerm::new(9).unwrap());
        clock.observe(decision.timestamp());
        clock.observe(decision.timestamp());
        assert!(clock.is_exhausted());
        assert_eq!(clock.reserve(), Err(DistributedClockError::Exhausted));
        let clock = DistributedClock::new();
        assert_eq!(
            clock.reserve_after(DistributedTimestamp::MAX),
            Err(DistributedClockError::Exhausted)
        );
        assert!(clock.is_exhausted());
        assert_eq!(clock.reserve(), Err(DistributedClockError::Exhausted));
    }

    #[test]
    fn concurrent_reservations_above_one_dependency_are_unique() {
        let clock = DistributedClock::new();
        let barrier = Barrier::new(8);
        let mut values = thread::scope(|scope| {
            let handles: Vec<_> = (0..8)
                .map(|_| {
                    scope.spawn(|| {
                        barrier.wait();
                        (0..128)
                            .map(|_| clock.reserve_after(base(1000)).unwrap().get())
                            .collect::<Vec<_>>()
                    })
                })
                .collect();
            handles
                .into_iter()
                .flat_map(|handle| handle.join().unwrap())
                .collect::<Vec<_>>()
        });
        values.sort_unstable();
        assert_eq!(values, (1001..2025).collect::<Vec<_>>());
    }

    #[test]
    fn concurrent_final_reservation_has_one_winner_and_observe_cannot_revive() {
        let clock = DistributedClock::new();
        clock.observe(base(MAX_BASE - 1));
        let barrier = Barrier::new(8);
        let successes = thread::scope(|scope| {
            let handles: Vec<_> = (0..8)
                .map(|_| {
                    scope.spawn(|| {
                        barrier.wait();
                        clock.observe(base(1));
                        match clock.reserve() {
                            Ok(value) => {
                                assert_eq!(value, DistributedTimestamp::MAX);
                                1
                            }
                            Err(error) => {
                                assert_eq!(error, DistributedClockError::Exhausted);
                                0
                            }
                        }
                    })
                })
                .collect();
            handles
                .into_iter()
                .map(|handle| handle.join().unwrap())
                .sum::<usize>()
        });
        assert_eq!(successes, 1);
        assert!(clock.is_exhausted());
    }

    #[test]
    fn dependency_maximum_uses_base_not_term() {
        let mut dependencies = DistributedDependencies::new();
        assert_eq!(dependencies.max_read_timestamp(), None);
        for (value, term) in [(20, 0), (19, 9), (20, 8), (1, 9)] {
            dependencies.observe_read(DistributedTimeTerm::new(
                base(value),
                DistributedTerm::new(term).unwrap(),
            ));
        }
        assert_eq!(dependencies.max_read_timestamp(), Some(base(20)));
    }
}
