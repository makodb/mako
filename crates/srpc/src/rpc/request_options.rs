//! Per-request policy — the port of `src/rrr/rpc/request_options.cpp`.
//!
//! [`RequestOptions`] carries the timeout and retry policy for a single
//! call. The retry rule is the important one: **only idempotent
//! requests are retried**, no matter what `max_retries` says. A retry
//! of a non-idempotent call can execute the operation twice — the
//! request may well have reached the server and only the reply been
//! lost — so idempotence, not the retry budget, is the gate.
//!
//! `timeout_ms == 0` means "no timeout", matching the C++ meaning.

use crate::base::rand::Rng;
use std::time::Duration;

/// Which deadline elapsed. Wire-visible only via the corresponding
/// [`crate::rpc::errors::RpcError`] codes; kept for diagnostics.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u8)]
pub enum TimeoutType {
    None = 0,
    Connect = 1,
    Request = 2,
    Response = 3,
    Total = 4,
}

impl TimeoutType {
    pub fn as_str(self) -> &'static str {
        match self {
            TimeoutType::None => "NONE",
            TimeoutType::Connect => "CONNECT_TIMEOUT",
            TimeoutType::Request => "REQUEST_TIMEOUT",
            TimeoutType::Response => "RESPONSE_TIMEOUT",
            TimeoutType::Total => "TOTAL_TIMEOUT",
        }
    }
}

impl std::fmt::Display for TimeoutType {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.as_str())
    }
}

#[derive(Clone, Copy, PartialEq, Debug)]
pub struct RequestOptions {
    /// Per-attempt timeout. `0` means no timeout.
    pub timeout_ms: u64,
    /// Deadline across all attempts. `0` means unbounded.
    pub total_timeout_ms: u64,
    pub max_retries: u16,
    pub base_delay_ms: u16,
    pub max_delay_ms: u16,
    /// Fraction of the delay to jitter by, e.g. `0.1` = ±10%.
    pub jitter_factor: f32,
    /// Whether re-executing this request is safe. Retries require it.
    pub idempotent: bool,
}

impl RequestOptions {
    pub fn new() -> RequestOptions {
        RequestOptions {
            timeout_ms: 1000,
            total_timeout_ms: 0,
            max_retries: 0,
            base_delay_ms: 50,
            max_delay_ms: 5000,
            jitter_factor: 0.1,
            idempotent: false,
        }
    }

    pub fn defaults() -> RequestOptions {
        RequestOptions::new()
    }

    /// Retry an idempotent call with an explicit per-attempt timeout.
    pub fn with_retry(max_retries: u16, timeout_ms: u64) -> RequestOptions {
        RequestOptions {
            timeout_ms,
            max_retries,
            idempotent: true,
            ..RequestOptions::new()
        }
    }

    pub fn idempotent_retry(max_retries: u16) -> RequestOptions {
        RequestOptions {
            max_retries,
            idempotent: true,
            ..RequestOptions::new()
        }
    }

    /// Wait indefinitely for a reply.
    pub fn no_timeout() -> RequestOptions {
        RequestOptions {
            timeout_ms: 0,
            ..RequestOptions::new()
        }
    }

    /// Short deadline, quick retries — for latency-sensitive calls.
    pub fn fast() -> RequestOptions {
        RequestOptions {
            timeout_ms: 100,
            total_timeout_ms: 0,
            max_retries: 2,
            base_delay_ms: 10,
            max_delay_ms: 100,
            jitter_factor: 0.1,
            idempotent: true,
        }
    }

    /// Long deadlines and a total budget — for expensive calls.
    pub fn patient() -> RequestOptions {
        RequestOptions {
            timeout_ms: 10000,
            total_timeout_ms: 60000,
            max_retries: 5,
            base_delay_ms: 500,
            max_delay_ms: 10000,
            jitter_factor: 0.1,
            idempotent: true,
        }
    }

    pub fn has_timeout(&self) -> bool {
        self.timeout_ms != 0
    }

    pub fn timeout(&self) -> Option<Duration> {
        if self.timeout_ms == 0 {
            None
        } else {
            Some(Duration::from_millis(self.timeout_ms))
        }
    }

    pub fn total_timeout(&self) -> Option<Duration> {
        if self.total_timeout_ms == 0 {
            None
        } else {
            Some(Duration::from_millis(self.total_timeout_ms))
        }
    }

    /// Whether another attempt is permitted after `current_retry_count`
    /// retries. Non-idempotent requests are never retried, whatever
    /// `max_retries` says — a lost reply is indistinguishable from a
    /// lost request, so a retry can execute the operation twice.
    pub fn can_retry(&self, current_retry_count: u16) -> bool {
        self.idempotent && current_retry_count < self.max_retries
    }

    /// Exponential backoff for `attempt`, clamped to `max_delay_ms`.
    /// No jitter — see [`Self::delay_with_jitter_ms`].
    pub fn calculate_delay_ms(&self, attempt: u16) -> u64 {
        let max = self.max_delay_ms as f64;
        let mut delay = self.base_delay_ms as f64;
        let mut i = 0;
        while i < attempt {
            delay *= 2.0;
            if delay > max {
                delay = max;
                break;
            }
            i += 1;
        }
        if delay > max {
            delay = max;
        }
        delay as u64
    }

    /// Backoff with `±jitter_factor` applied, drawn from `rng`.
    ///
    /// Unlike the reconnect policy — which jitters after clamping on
    /// purpose — this one is bounded by `max_delay_ms`, because a
    /// per-request delay feeds a caller's total deadline.
    pub fn delay_with_jitter_ms(&self, attempt: u16, rng: &Rng) -> u64 {
        let base = self.calculate_delay_ms(attempt) as f64;
        if self.jitter_factor <= 0.0 || base <= 0.0 {
            return base as u64;
        }
        let f = self.jitter_factor as f64;
        let jittered = base * rng.next_f64_in(1.0 - f, 1.0 + f);
        let capped = jittered.min(self.max_delay_ms as f64).max(0.0);
        capped as u64
    }
}

impl Default for RequestOptions {
    fn default() -> RequestOptions {
        RequestOptions::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_do_not_retry() {
        let o = RequestOptions::new();
        assert_eq!(o, RequestOptions::defaults());
        assert_eq!(o.timeout_ms, 1000);
        assert_eq!(o.max_retries, 0);
        assert!(!o.idempotent, "a call is not assumed safe to repeat");
        assert!(!o.can_retry(0));
    }

    #[test]
    fn presets_match_the_documented_values() {
        let f = RequestOptions::fast();
        assert_eq!(
            (f.timeout_ms, f.max_retries, f.base_delay_ms, f.max_delay_ms),
            (100, 2, 10, 100)
        );
        assert!(f.idempotent);

        let p = RequestOptions::patient();
        assert_eq!(
            (p.timeout_ms, p.total_timeout_ms, p.max_retries),
            (10000, 60000, 5)
        );
        assert_eq!((p.base_delay_ms, p.max_delay_ms), (500, 10000));

        let r = RequestOptions::with_retry(3, 250);
        assert_eq!((r.timeout_ms, r.max_retries), (250, 3));
        assert!(r.idempotent);

        let i = RequestOptions::idempotent_retry(4);
        assert_eq!((i.timeout_ms, i.max_retries), (1000, 4));
        assert!(i.idempotent);

        let n = RequestOptions::no_timeout();
        assert_eq!(n.timeout_ms, 0);
        assert!(!n.has_timeout());
    }

    #[test]
    fn retries_require_idempotence() {
        let mut o = RequestOptions::idempotent_retry(3);
        assert!(o.can_retry(0) && o.can_retry(2));
        assert!(!o.can_retry(3), "budget exhausted");

        // The budget alone is not enough: a non-idempotent call is
        // never retried, because a lost reply looks exactly like a lost
        // request and the operation could run twice.
        o.idempotent = false;
        assert!(!o.can_retry(0), "non-idempotent must not retry");
        assert!(!o.can_retry(1));
    }

    #[test]
    fn timeouts_of_zero_mean_none() {
        let o = RequestOptions::new();
        assert_eq!(o.timeout(), Some(Duration::from_millis(1000)));
        assert_eq!(o.total_timeout(), None, "0 total means unbounded");
        assert_eq!(RequestOptions::no_timeout().timeout(), None);
        assert_eq!(
            RequestOptions::patient().total_timeout(),
            Some(Duration::from_millis(60000))
        );
    }

    #[test]
    fn backoff_doubles_then_clamps() {
        let o = RequestOptions::idempotent_retry(10);
        assert_eq!(o.calculate_delay_ms(0), 50);
        assert_eq!(o.calculate_delay_ms(1), 100);
        assert_eq!(o.calculate_delay_ms(2), 200);
        assert_eq!(o.calculate_delay_ms(3), 400);
        assert_eq!(o.calculate_delay_ms(7), 5000, "clamped at max_delay_ms");
        assert_eq!(o.calculate_delay_ms(50), 5000, "stays clamped");
    }

    #[test]
    fn fast_preset_clamps_at_its_own_ceiling() {
        let o = RequestOptions::fast();
        assert_eq!(o.calculate_delay_ms(0), 10);
        assert_eq!(o.calculate_delay_ms(1), 20);
        assert_eq!(o.calculate_delay_ms(4), 100, "ceiling is 100ms here");
    }

    #[test]
    fn jitter_stays_within_the_factor_and_the_ceiling() {
        let o = RequestOptions::idempotent_retry(10);
        let rng = Rng::with_seed(4242);
        let mut below = false;
        let mut above = false;
        let mut i = 0;
        while i < 500 {
            // attempt 2 -> base 200ms, +/-10% -> [180, 220]
            let d = o.delay_with_jitter_ms(2, &rng);
            assert!((180..=220).contains(&d), "{d} outside the jitter band");
            if d < 200 {
                below = true;
            }
            if d > 200 {
                above = true;
            }
            i += 1;
        }
        assert!(below && above, "jitter must vary both ways");

        // At the ceiling the result is capped, unlike the reconnect
        // policy which deliberately jitters past it.
        let mut k = 0;
        while k < 200 {
            assert!(o.delay_with_jitter_ms(20, &rng) <= 5000, "capped");
            k += 1;
        }
    }

    #[test]
    fn zero_jitter_factor_returns_the_base_delay() {
        let mut o = RequestOptions::idempotent_retry(5);
        o.jitter_factor = 0.0;
        let rng = Rng::with_seed(1);
        assert_eq!(o.delay_with_jitter_ms(3, &rng), 400);
        assert_eq!(o.delay_with_jitter_ms(3, &rng), 400, "deterministic");
    }

    #[test]
    fn timeout_type_names() {
        assert_eq!(TimeoutType::None as u8, 0);
        assert_eq!(TimeoutType::Total as u8, 4);
        assert_eq!(TimeoutType::Request.to_string(), "REQUEST_TIMEOUT");
    }
}
