//! Circuit breaker — the port of `src/rrr/rpc/circuit_breaker.cpp`.
//!
//! Three states, as in the C++ original:
//!
//! ```text
//!            failure_threshold consecutive failures
//!   Closed ──────────────────────────────────────▶ Open
//!     ▲                                              │
//!     │ success_threshold successes                  │ timeout elapsed
//!     │                                              ▼
//!     └────────────────────────────────────────  HalfOpen
//!                        (a failure sends it straight back to Open)
//! ```
//!
//! `HalfOpen` admits **one probe at a time**: the first caller through
//! takes the probe slot, everyone else is refused until that probe
//! reports back. That is what keeps a recovering peer from being hit by
//! the full load the instant its timeout expires.
//!
//! Every method that consults the clock has an `_at` twin taking the
//! instant explicitly. Tests drive those and never sleep; the C++
//! version could only be tested against the real clock.

use crate::base::time::wall_us;
use std::cell::Cell;
use std::time::{Duration, Instant};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(i32)]
pub enum CircuitState {
    Closed = 0,
    Open = 1,
    HalfOpen = 2,
}

impl CircuitState {
    pub fn as_str(self) -> &'static str {
        match self {
            CircuitState::Closed => "CLOSED",
            CircuitState::Open => "OPEN",
            CircuitState::HalfOpen => "HALF_OPEN",
        }
    }
}

impl std::fmt::Display for CircuitState {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.as_str())
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct CircuitBreakerConfig {
    /// Consecutive failures that trip the breaker.
    pub failure_threshold: u32,
    /// Consecutive probe successes that close it again.
    pub success_threshold: u32,
    /// How long to stay open before admitting a probe.
    pub timeout_ms: u32,
    pub enabled: bool,
}

impl CircuitBreakerConfig {
    pub fn new() -> CircuitBreakerConfig {
        CircuitBreakerConfig {
            failure_threshold: 5,
            success_threshold: 3,
            timeout_ms: 30000,
            enabled: true,
        }
    }

    pub fn defaults() -> CircuitBreakerConfig {
        CircuitBreakerConfig::new()
    }

    /// Trips early, recovers cautiously.
    pub fn sensitive() -> CircuitBreakerConfig {
        CircuitBreakerConfig {
            failure_threshold: 3,
            success_threshold: 5,
            timeout_ms: 60000,
            enabled: true,
        }
    }

    /// Tolerates more failures, retries sooner.
    pub fn relaxed() -> CircuitBreakerConfig {
        CircuitBreakerConfig {
            failure_threshold: 10,
            success_threshold: 2,
            timeout_ms: 15000,
            enabled: true,
        }
    }

    /// Never breaks the circuit; every request is allowed.
    pub fn disabled() -> CircuitBreakerConfig {
        CircuitBreakerConfig {
            failure_threshold: 0,
            success_threshold: 0,
            timeout_ms: 0,
            enabled: false,
        }
    }

    pub fn timeout(&self) -> Duration {
        Duration::from_millis(self.timeout_ms as u64)
    }
}

impl Default for CircuitBreakerConfig {
    fn default() -> CircuitBreakerConfig {
        CircuitBreakerConfig::new()
    }
}

/// Per-peer breaker state.
///
/// Interior mutability (`Cell`) rather than `&mut`: a breaker is shared
/// by every call on a connection and is consulted from the poll thread,
/// exactly as the C++ version is. Not thread-safe on its own — one
/// breaker per connection, owned by that connection.
pub struct CircuitBreaker {
    config: CircuitBreakerConfig,
    state: Cell<CircuitState>,
    failure_count: Cell<u32>,
    success_count: Cell<u32>,
    /// When the breaker last tripped. `None` until it first opens.
    opened_at: Cell<Option<Instant>>,
    probe_in_progress: Cell<bool>,
}

impl CircuitBreaker {
    pub fn new(config: CircuitBreakerConfig) -> CircuitBreaker {
        CircuitBreaker {
            config,
            state: Cell::new(CircuitState::Closed),
            failure_count: Cell::new(0),
            success_count: Cell::new(0),
            opened_at: Cell::new(None),
            probe_in_progress: Cell::new(false),
        }
    }

    pub fn config(&self) -> CircuitBreakerConfig {
        self.config
    }

    pub fn state(&self) -> CircuitState {
        self.state.get()
    }

    pub fn is_closed(&self) -> bool {
        self.state.get() == CircuitState::Closed
    }

    pub fn is_open(&self) -> bool {
        self.state.get() == CircuitState::Open
    }

    pub fn is_half_open(&self) -> bool {
        self.state.get() == CircuitState::HalfOpen
    }

    pub fn failure_count(&self) -> u32 {
        self.failure_count.get()
    }

    pub fn success_count(&self) -> u32 {
        self.success_count.get()
    }

    /// Whether a request may proceed, evaluated against `now`.
    ///
    /// Not a pure query: crossing the timeout moves `Open` to
    /// `HalfOpen` and claims the probe slot, so the caller that is told
    /// "yes" is the probe.
    pub fn allow_request_at(&self, now: Instant) -> bool {
        if !self.config.enabled {
            return true;
        }
        match self.state.get() {
            CircuitState::Closed => true,
            CircuitState::Open => {
                let elapsed = match self.opened_at.get() {
                    Some(t) => now.saturating_duration_since(t),
                    // Open with no timestamp cannot happen through the
                    // public API; treat it as expired rather than
                    // wedging the breaker shut forever.
                    None => self.config.timeout(),
                };
                if elapsed >= self.config.timeout() {
                    self.state.set(CircuitState::HalfOpen);
                    self.probe_in_progress.set(true);
                    true
                } else {
                    false
                }
            }
            CircuitState::HalfOpen => {
                if self.probe_in_progress.get() {
                    false
                } else {
                    self.probe_in_progress.set(true);
                    true
                }
            }
        }
    }

    pub fn allow_request(&self) -> bool {
        self.allow_request_at(Instant::now())
    }

    pub fn record_success(&self) {
        if !self.config.enabled {
            return;
        }
        match self.state.get() {
            CircuitState::Closed => {
                // Only CONSECUTIVE failures trip the breaker.
                self.failure_count.set(0);
            }
            CircuitState::HalfOpen => {
                self.probe_in_progress.set(false);
                let count = self.success_count.get() + 1;
                self.success_count.set(count);
                if count >= self.config.success_threshold {
                    self.state.set(CircuitState::Closed);
                    self.failure_count.set(0);
                    self.success_count.set(0);
                }
            }
            CircuitState::Open => {
                // A probe that reports back after the breaker re-opened.
                self.probe_in_progress.set(false);
            }
        }
    }

    pub fn record_failure_at(&self, now: Instant) {
        if !self.config.enabled {
            return;
        }
        match self.state.get() {
            CircuitState::Closed => {
                let count = self.failure_count.get() + 1;
                self.failure_count.set(count);
                if count >= self.config.failure_threshold {
                    self.trip(now);
                }
            }
            CircuitState::HalfOpen => {
                // The probe failed: straight back to Open, and the
                // timeout starts again from now.
                self.probe_in_progress.set(false);
                self.trip(now);
            }
            CircuitState::Open => {
                self.probe_in_progress.set(false);
            }
        }
    }

    pub fn record_failure(&self) {
        self.record_failure_at(Instant::now());
    }

    fn trip(&self, now: Instant) {
        self.state.set(CircuitState::Open);
        self.opened_at.set(Some(now));
        self.failure_count.set(0);
        self.success_count.set(0);
        self.probe_in_progress.set(false);
    }

    /// Force the breaker closed and forget all history.
    pub fn reset(&self) {
        self.state.set(CircuitState::Closed);
        self.failure_count.set(0);
        self.success_count.set(0);
        self.opened_at.set(None);
        self.probe_in_progress.set(false);
    }

    /// Wall-clock microseconds at which the breaker last tripped, or 0.
    /// For logging only — the state machine itself uses the monotonic
    /// clock, which cannot go backwards.
    pub fn last_trip_wall_us(&self) -> u64 {
        match self.opened_at.get() {
            Some(t) => wall_us().saturating_sub(t.elapsed().as_micros() as u64),
            None => 0,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn breaker() -> CircuitBreaker {
        CircuitBreaker::new(CircuitBreakerConfig::new())
    }

    #[test]
    fn presets_match_the_documented_values() {
        let d = CircuitBreakerConfig::new();
        assert_eq!(d, CircuitBreakerConfig::defaults());
        assert_eq!(
            (d.failure_threshold, d.success_threshold, d.timeout_ms),
            (5, 3, 30000)
        );

        let s = CircuitBreakerConfig::sensitive();
        assert_eq!(
            (s.failure_threshold, s.success_threshold, s.timeout_ms),
            (3, 5, 60000)
        );

        let r = CircuitBreakerConfig::relaxed();
        assert_eq!(
            (r.failure_threshold, r.success_threshold, r.timeout_ms),
            (10, 2, 15000)
        );

        assert!(!CircuitBreakerConfig::disabled().enabled);
        assert_eq!(d.timeout(), Duration::from_millis(30000));
    }

    #[test]
    fn starts_closed_and_allows() {
        let b = breaker();
        assert!(b.is_closed());
        assert!(b.allow_request());
        assert_eq!(b.state().to_string(), "CLOSED");
    }

    #[test]
    fn trips_after_consecutive_failures() {
        let b = breaker();
        let t = Instant::now();
        let mut i = 0;
        while i < 4 {
            b.record_failure_at(t);
            assert!(b.is_closed(), "still closed after {} failures", i + 1);
            i += 1;
        }
        b.record_failure_at(t);
        assert!(b.is_open(), "fifth failure trips it");
        assert!(!b.allow_request_at(t), "open breaker refuses");
    }

    #[test]
    fn a_success_clears_the_failure_streak() {
        let b = breaker();
        let t = Instant::now();
        b.record_failure_at(t);
        b.record_failure_at(t);
        b.record_failure_at(t);
        b.record_failure_at(t);
        assert_eq!(b.failure_count(), 4);
        b.record_success();
        assert_eq!(b.failure_count(), 0, "only CONSECUTIVE failures count");
        b.record_failure_at(t);
        assert!(b.is_closed(), "streak restarted");
    }

    #[test]
    fn opens_then_half_opens_after_the_timeout() {
        let b = breaker();
        let t0 = Instant::now();
        let mut i = 0;
        while i < 5 {
            b.record_failure_at(t0);
            i += 1;
        }
        assert!(b.is_open());

        // Just before the timeout: still refused, still open.
        let almost = t0 + Duration::from_millis(29_999);
        assert!(!b.allow_request_at(almost));
        assert!(b.is_open());

        // At the timeout: one probe is admitted.
        let after = t0 + Duration::from_millis(30_000);
        assert!(b.allow_request_at(after), "probe admitted");
        assert!(b.is_half_open());
    }

    #[test]
    fn half_open_admits_only_one_probe_at_a_time() {
        let b = breaker();
        let t0 = Instant::now();
        let mut i = 0;
        while i < 5 {
            b.record_failure_at(t0);
            i += 1;
        }
        let after = t0 + Duration::from_millis(30_000);
        assert!(b.allow_request_at(after), "first caller takes the probe");
        assert!(!b.allow_request_at(after), "second is refused");
        assert!(!b.allow_request_at(after), "and stays refused");

        // Once the probe reports, the next caller may probe again.
        b.record_success();
        assert!(b.allow_request_at(after), "slot freed");
    }

    #[test]
    fn closes_after_enough_probe_successes() {
        let b = breaker();
        let t0 = Instant::now();
        let mut i = 0;
        while i < 5 {
            b.record_failure_at(t0);
            i += 1;
        }
        let after = t0 + Duration::from_millis(30_000);
        assert!(b.allow_request_at(after));
        assert!(b.is_half_open());

        b.record_success();
        assert!(b.is_half_open(), "one success is not enough");
        assert!(b.allow_request_at(after));
        b.record_success();
        assert!(b.is_half_open(), "two is not enough");
        assert!(b.allow_request_at(after));
        b.record_success();
        assert!(b.is_closed(), "three closes it");
        assert_eq!(b.failure_count(), 0);
        assert_eq!(b.success_count(), 0);
    }

    #[test]
    fn a_failed_probe_reopens_and_restarts_the_timeout() {
        let b = breaker();
        let t0 = Instant::now();
        let mut i = 0;
        while i < 5 {
            b.record_failure_at(t0);
            i += 1;
        }
        let after = t0 + Duration::from_millis(30_000);
        assert!(b.allow_request_at(after));
        assert!(b.is_half_open());

        b.record_failure_at(after);
        assert!(b.is_open(), "failed probe reopens");

        // The timeout runs from the NEW trip, not the original one.
        assert!(!b.allow_request_at(after + Duration::from_millis(29_999)));
        assert!(b.allow_request_at(after + Duration::from_millis(30_000)));
    }

    #[test]
    fn a_single_probe_success_does_not_close_a_sensitive_breaker() {
        let b = CircuitBreaker::new(CircuitBreakerConfig::sensitive());
        let t0 = Instant::now();
        let mut i = 0;
        while i < 3 {
            b.record_failure_at(t0);
            i += 1;
        }
        assert!(b.is_open(), "sensitive trips at 3");
        let after = t0 + Duration::from_millis(60_000);
        let mut k = 0;
        while k < 4 {
            assert!(b.allow_request_at(after));
            b.record_success();
            assert!(b.is_half_open(), "still probing after {} successes", k + 1);
            k += 1;
        }
        assert!(b.allow_request_at(after));
        b.record_success();
        assert!(b.is_closed(), "fifth success closes it");
    }

    #[test]
    fn disabled_breaker_never_interferes() {
        let b = CircuitBreaker::new(CircuitBreakerConfig::disabled());
        let t = Instant::now();
        let mut i = 0;
        while i < 100 {
            assert!(b.allow_request_at(t));
            b.record_failure_at(t);
            i += 1;
        }
        assert!(b.is_closed(), "state never moves");
        assert_eq!(b.failure_count(), 0, "and nothing is counted");
    }

    #[test]
    fn reset_forgets_everything() {
        let b = breaker();
        let t0 = Instant::now();
        let mut i = 0;
        while i < 5 {
            b.record_failure_at(t0);
            i += 1;
        }
        assert!(b.is_open());
        b.reset();
        assert!(b.is_closed());
        assert_eq!(b.failure_count(), 0);
        assert_eq!(b.success_count(), 0);
        assert!(b.allow_request_at(t0), "usable immediately");
    }

    #[test]
    fn late_probe_result_after_reopen_frees_the_slot() {
        // A probe is admitted, the breaker reopens underneath it, and
        // the probe's result arrives afterwards. It must not leave the
        // probe slot claimed forever.
        let b = breaker();
        let t0 = Instant::now();
        let mut i = 0;
        while i < 5 {
            b.record_failure_at(t0);
            i += 1;
        }
        let after = t0 + Duration::from_millis(30_000);
        assert!(b.allow_request_at(after));
        b.record_failure_at(after); // probe fails -> Open
        assert!(b.is_open());
        b.record_success(); // late success from that same probe
        let later = after + Duration::from_millis(30_000);
        assert!(b.allow_request_at(later), "slot is free for the next probe");
    }
}
