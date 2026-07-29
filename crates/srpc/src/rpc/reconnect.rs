//! Reconnect policy and backoff — the port of
//! `src/rrr/rpc/reconnect_policy.cpp`.
//!
//! [`ReconnectPolicy`] is the configuration; [`ReconnectCalculator`]
//! is the per-connection state that walks the backoff curve.
//!
//! Two C++ behaviours are preserved deliberately, because changing
//! them would change how a cluster reconnects under load:
//!   * `max_retries == 0` means **unlimited**, not "never retry" —
//!     [`ReconnectPolicy::no_retry`] expresses that by turning
//!     `auto_reconnect` off;
//!   * jitter is applied **after** clamping to `max_delay_ms`, so a
//!     jittered delay can exceed the maximum by up to 50%. Clamping
//!     after jitter instead would suppress the upper half of the
//!     jitter range at the ceiling, which is exactly where spreading
//!     a thundering herd matters most.

use crate::base::rand::Rng;
use std::cell::Cell;
use std::time::Duration;

/// How a connection should reconnect after a failure.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ReconnectPolicy {
    pub auto_reconnect: bool,
    /// `0` means unlimited attempts.
    pub max_retries: u32,
    pub initial_delay_ms: u32,
    pub max_delay_ms: u32,
    pub backoff_multiplier: f64,
    pub jitter_enabled: bool,
}

impl ReconnectPolicy {
    /// The documented defaults (identical to [`Self::conservative`]).
    pub fn new() -> ReconnectPolicy {
        ReconnectPolicy {
            auto_reconnect: true,
            max_retries: 5,
            initial_delay_ms: 1000,
            max_delay_ms: 30000,
            backoff_multiplier: 2.0,
            jitter_enabled: true,
        }
    }

    /// Retry forever, starting fast.
    pub fn aggressive() -> ReconnectPolicy {
        ReconnectPolicy {
            auto_reconnect: true,
            max_retries: 0,
            initial_delay_ms: 100,
            max_delay_ms: 5000,
            backoff_multiplier: 1.5,
            jitter_enabled: true,
        }
    }

    pub fn conservative() -> ReconnectPolicy {
        ReconnectPolicy::new()
    }

    /// Do not reconnect at all.
    pub fn no_retry() -> ReconnectPolicy {
        ReconnectPolicy {
            auto_reconnect: false,
            max_retries: 0,
            initial_delay_ms: 0,
            max_delay_ms: 0,
            backoff_multiplier: 1.0,
            jitter_enabled: false,
        }
    }
}

impl Default for ReconnectPolicy {
    fn default() -> ReconnectPolicy {
        ReconnectPolicy::new()
    }
}

/// Walks the backoff curve for one connection.
///
/// Owns its [`Rng`] so jitter is testable: [`Self::with_seed`] gives a
/// deterministic stream, while [`Self::new`] seeds from the clock.
pub struct ReconnectCalculator {
    policy: ReconnectPolicy,
    retries: Cell<u32>,
    rng: Rng,
}

impl ReconnectCalculator {
    pub fn new(policy: ReconnectPolicy) -> ReconnectCalculator {
        ReconnectCalculator {
            policy,
            retries: Cell::new(0),
            rng: Rng::from_clock(),
        }
    }

    /// Deterministic jitter — for tests and for reproducing a trace.
    pub fn with_seed(policy: ReconnectPolicy, seed: u64) -> ReconnectCalculator {
        ReconnectCalculator {
            policy,
            retries: Cell::new(0),
            rng: Rng::with_seed(seed),
        }
    }

    pub fn policy(&self) -> ReconnectPolicy {
        self.policy
    }

    pub fn attempts(&self) -> u32 {
        self.retries.get()
    }

    /// Whether another attempt is allowed. `max_retries == 0` is
    /// unlimited (the C++ meaning).
    pub fn should_retry(&self) -> bool {
        if !self.policy.auto_reconnect {
            return false;
        }
        if self.policy.max_retries == 0 {
            return true;
        }
        self.retries.get() < self.policy.max_retries
    }

    /// Exponential curve for a given attempt count, clamped to
    /// `max_delay_ms`. No jitter, no state change.
    fn base_delay_ms(&self, count: u32) -> f64 {
        let mut delay = self.policy.initial_delay_ms as f64;
        let max = self.policy.max_delay_ms as f64;
        let mut i = 0;
        while i < count {
            delay *= self.policy.backoff_multiplier;
            if delay >= max {
                delay = max;
                break;
            }
            i += 1;
        }
        if delay > max {
            delay = max;
        }
        delay
    }

    /// Delay for the NEXT attempt, advancing the curve.
    pub fn next_delay_ms(&self) -> u32 {
        let count = self.retries.get();
        self.retries.set(count + 1);
        let mut delay = self.base_delay_ms(count);
        if self.policy.jitter_enabled && delay > 0.0 {
            // Applied after clamping — see the module note.
            delay *= self.rng.next_f64_in(0.5, 1.5);
        }
        delay as u32
    }

    pub fn next_delay(&self) -> Duration {
        Duration::from_millis(self.next_delay_ms() as u64)
    }

    /// What [`Self::next_delay_ms`] would return, without jitter and
    /// without advancing.
    pub fn peek_delay_ms(&self) -> u32 {
        self.base_delay_ms(self.retries.get()) as u32
    }

    /// Back to the first attempt — call after a successful connect.
    pub fn reset(&self) {
        self.retries.set(0);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn presets_match_the_documented_values() {
        let d = ReconnectPolicy::new();
        assert_eq!(d, ReconnectPolicy::conservative());
        assert!(d.auto_reconnect);
        assert_eq!(d.max_retries, 5);
        assert_eq!(d.initial_delay_ms, 1000);
        assert_eq!(d.max_delay_ms, 30000);
        assert_eq!(d.backoff_multiplier, 2.0);
        assert!(d.jitter_enabled);

        let a = ReconnectPolicy::aggressive();
        assert_eq!(a.max_retries, 0, "unlimited");
        assert_eq!(a.initial_delay_ms, 100);
        assert_eq!(a.max_delay_ms, 5000);
        assert_eq!(a.backoff_multiplier, 1.5);

        let n = ReconnectPolicy::no_retry();
        assert!(!n.auto_reconnect);
        assert!(!n.jitter_enabled);
    }

    #[test]
    fn backoff_doubles_then_clamps() {
        let mut p = ReconnectPolicy::new();
        p.jitter_enabled = false;
        let c = ReconnectCalculator::with_seed(p, 1);
        assert_eq!(c.next_delay_ms(), 1000);
        assert_eq!(c.next_delay_ms(), 2000);
        assert_eq!(c.next_delay_ms(), 4000);
        assert_eq!(c.next_delay_ms(), 8000);
        assert_eq!(c.next_delay_ms(), 16000);
        assert_eq!(c.next_delay_ms(), 30000, "clamped at max");
        assert_eq!(c.next_delay_ms(), 30000, "stays clamped");
    }

    #[test]
    fn peek_does_not_advance_but_next_does() {
        let mut p = ReconnectPolicy::new();
        p.jitter_enabled = false;
        let c = ReconnectCalculator::with_seed(p, 1);
        assert_eq!(c.peek_delay_ms(), 1000);
        assert_eq!(c.peek_delay_ms(), 1000, "peek is idempotent");
        assert_eq!(c.attempts(), 0);
        assert_eq!(c.next_delay_ms(), 1000);
        assert_eq!(c.attempts(), 1);
        assert_eq!(c.peek_delay_ms(), 2000, "peek sees the advanced curve");
    }

    #[test]
    fn unlimited_means_zero_max_retries() {
        let c = ReconnectCalculator::with_seed(ReconnectPolicy::aggressive(), 1);
        let mut i = 0;
        while i < 1000 {
            assert!(c.should_retry(), "attempt {i} must be allowed");
            c.next_delay_ms();
            i += 1;
        }
    }

    #[test]
    fn bounded_retries_stop_at_the_limit() {
        let c = ReconnectCalculator::with_seed(ReconnectPolicy::new(), 1);
        let mut i = 0;
        while i < 5 {
            assert!(c.should_retry(), "attempt {i}");
            c.next_delay_ms();
            i += 1;
        }
        assert!(!c.should_retry(), "sixth attempt is refused");
        c.reset();
        assert!(c.should_retry(), "reset re-arms");
        assert_eq!(c.peek_delay_ms(), 1000, "and restarts the curve");
    }

    #[test]
    fn no_retry_never_retries() {
        let c = ReconnectCalculator::with_seed(ReconnectPolicy::no_retry(), 1);
        assert!(!c.should_retry());
        assert_eq!(c.next_delay_ms(), 0);
    }

    #[test]
    fn jitter_spreads_within_half_to_one_and_a_half() {
        let c = ReconnectCalculator::with_seed(ReconnectPolicy::new(), 12345);
        // First attempt: base 1000ms, jittered into [500, 1500).
        let mut lo_seen = false;
        let mut hi_seen = false;
        let mut i = 0;
        while i < 200 {
            c.reset();
            let d = c.next_delay_ms();
            assert!((500..1500).contains(&d), "{d} outside the jitter band");
            if d < 1000 {
                lo_seen = true;
            } else {
                hi_seen = true;
            }
            i += 1;
        }
        assert!(lo_seen && hi_seen, "jitter must vary in both directions");
    }

    #[test]
    fn jitter_is_reproducible_for_a_seed() {
        let a = ReconnectCalculator::with_seed(ReconnectPolicy::new(), 777);
        let b = ReconnectCalculator::with_seed(ReconnectPolicy::new(), 777);
        let mut i = 0;
        while i < 5 {
            assert_eq!(a.next_delay_ms(), b.next_delay_ms(), "step {i}");
            i += 1;
        }
    }

    #[test]
    fn jitter_can_exceed_the_ceiling_by_design() {
        // Clamping happens BEFORE jitter, so a jittered delay at the
        // ceiling can reach 1.5x max_delay_ms. Pinned so the ordering
        // is not "fixed" by accident.
        let c = ReconnectCalculator::with_seed(ReconnectPolicy::new(), 99);
        let mut i = 0;
        while i < 20 {
            c.next_delay_ms();
            i += 1;
        }
        // Deep into the curve every base delay is the 30000ms ceiling.
        let mut saw_over = false;
        let mut k = 0;
        while k < 200 {
            let d = c.next_delay_ms();
            assert!(d < 45000, "{d} beyond 1.5x the ceiling");
            if d > 30000 {
                saw_over = true;
            }
            k += 1;
        }
        assert!(
            saw_over,
            "jitter after clamping should sometimes exceed max"
        );
    }

    #[test]
    fn duration_form_agrees_with_millis() {
        let mut p = ReconnectPolicy::new();
        p.jitter_enabled = false;
        let c = ReconnectCalculator::with_seed(p, 1);
        assert_eq!(c.next_delay(), Duration::from_millis(1000));
        assert_eq!(c.next_delay(), Duration::from_millis(2000));
    }
}
