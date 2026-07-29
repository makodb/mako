//! Clocks and stopwatches — the `Time`/`Timer` layer of
//! `src/rrr/base/basetypes.cpp`.
//!
//! The C++ side reaches libc directly (`gettimeofday`,
//! `clock_gettime(CLOCK_REALTIME_COARSE)`, `nanosleep`) through
//! `rusty::sys::time`. Here that is `std::time`, which translates to
//! the same clocks: `Instant` is the monotonic clock and `SystemTime`
//! the wall clock. Nothing in this module needs `unsafe`.
//!
//! Two deliberate differences from the C++ original, both because Rust
//! says it better:
//!   * `Timer` is a small state machine over `Instant` instead of two
//!     raw `u64` microsecond stamps, so "stopped" is unrepresentable
//!     rather than encoded as `end_us == 0`;
//!   * `Timer::elapsed` returns a [`Duration`] rather than `f64`
//!     seconds — callers that want seconds ask for them
//!     ([`Duration::as_secs_f64`]).
//!
//! The `accurate` flag on the C++ `Time::now` selected between the
//! precise and coarse realtime clocks. Rust's `SystemTime` has no
//! coarse variant, and the coarse clock existed purely to dodge a
//! `gettimeofday` cost that no longer matters on vDSO systems, so
//! [`wall_us`] always reads the precise clock.

use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

/// Microseconds since the Unix epoch (the C++ `Time::now`).
///
/// Clamps rather than panicking if the wall clock is before the epoch:
/// a nonsensical clock should not take the process down.
pub fn wall_us() -> u64 {
    match SystemTime::now().duration_since(UNIX_EPOCH) {
        Ok(d) => d.as_micros() as u64,
        Err(_) => 0,
    }
}

/// Milliseconds since the Unix epoch.
pub fn wall_ms() -> u64 {
    wall_us() / 1000
}

/// Sleep for `us` microseconds (the C++ `Time::sleep`).
pub fn sleep_us(us: u64) {
    std::thread::sleep(Duration::from_micros(us));
}

/// A start/stop stopwatch over the monotonic clock.
///
/// Monotonic on purpose: the C++ original timed with the *wall* clock,
/// so an NTP step could produce a negative interval. Measuring elapsed
/// time against `Instant` cannot go backwards.
#[derive(Clone, Copy, Debug)]
pub struct Timer {
    started: Option<Instant>,
    stopped: Option<Instant>,
}

impl Timer {
    pub fn new() -> Timer {
        Timer {
            started: None,
            stopped: None,
        }
    }

    /// Start (or restart) timing.
    pub fn start(&mut self) {
        self.started = Some(Instant::now());
        self.stopped = None;
    }

    /// Freeze the elapsed interval. A `stop` without a `start` is
    /// ignored — there is no interval to freeze.
    pub fn stop(&mut self) {
        if self.started.is_some() {
            self.stopped = Some(Instant::now());
        }
    }

    pub fn reset(&mut self) {
        self.started = None;
        self.stopped = None;
    }

    pub fn is_running(&self) -> bool {
        self.started.is_some() && self.stopped.is_none()
    }

    /// Time between `start` and `stop`, or between `start` and now
    /// while still running. Zero before the first `start` (the C++
    /// version aborted; a stopwatch that was never started has an
    /// elapsed time of zero, and taking the process down for asking is
    /// not worth it).
    pub fn elapsed(&self) -> Duration {
        let start = match self.started {
            Some(s) => s,
            None => return Duration::ZERO,
        };
        match self.stopped {
            Some(e) => e.saturating_duration_since(start),
            None => start.elapsed(),
        }
    }

    pub fn elapsed_us(&self) -> u64 {
        self.elapsed().as_micros() as u64
    }

    pub fn elapsed_secs_f64(&self) -> f64 {
        self.elapsed().as_secs_f64()
    }
}

impl Default for Timer {
    fn default() -> Timer {
        Timer::new()
    }
}

/// A deadline: a fixed point on the monotonic clock.
///
/// The RPC layers all express timeouts as "is it past X yet", which the
/// C++ code open-codes with microsecond arithmetic at every call site.
pub struct Deadline {
    at: Instant,
}

impl Deadline {
    pub fn after(timeout: Duration) -> Deadline {
        Deadline {
            at: Instant::now() + timeout,
        }
    }

    pub fn after_us(us: u64) -> Deadline {
        Deadline::after(Duration::from_micros(us))
    }

    pub fn is_expired(&self) -> bool {
        Instant::now() >= self.at
    }

    /// Time left, saturating at zero once expired (never negative).
    pub fn remaining(&self) -> Duration {
        self.at.saturating_duration_since(Instant::now())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn wall_clock_is_after_2020() {
        // 2020-01-01 in microseconds — a sanity floor, not a precise
        // assertion.
        assert!(wall_us() > 1577836800000000);
        assert!(wall_ms() > 1577836800000);
    }

    #[test]
    fn timer_states() {
        let mut t = Timer::new();
        assert!(!t.is_running());
        assert_eq!(t.elapsed(), Duration::ZERO);

        t.start();
        assert!(t.is_running());
        std::thread::sleep(Duration::from_millis(12));
        let running = t.elapsed();
        assert!(running >= Duration::from_millis(10), "{running:?}");

        t.stop();
        assert!(!t.is_running());
        let frozen = t.elapsed();
        std::thread::sleep(Duration::from_millis(5));
        assert_eq!(t.elapsed(), frozen, "a stopped timer does not advance");
        assert!(t.elapsed_us() >= 10000);
        assert!(t.elapsed_secs_f64() >= 0.010);

        t.reset();
        assert_eq!(t.elapsed(), Duration::ZERO);
    }

    #[test]
    fn stop_without_start_is_ignored() {
        let mut t = Timer::new();
        t.stop();
        assert_eq!(t.elapsed(), Duration::ZERO);
        assert!(!t.is_running());
    }

    #[test]
    fn deadline_expiry() {
        let d = Deadline::after(Duration::from_millis(20));
        assert!(!d.is_expired());
        assert!(d.remaining() > Duration::ZERO);

        let past = Deadline::after_us(0);
        std::thread::sleep(Duration::from_millis(1));
        assert!(past.is_expired());
        assert_eq!(past.remaining(), Duration::ZERO, "remaining saturates");
    }

    #[test]
    fn sleep_advances_the_clock() {
        let t = Instant::now();
        sleep_us(2000);
        assert!(t.elapsed() >= Duration::from_millis(2));
    }
}
