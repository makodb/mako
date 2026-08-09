//! Connection retry policy and exponential-backoff state.
//!
//! Jitter deliberately shares the legacy `srpc_rand_raw` C kernel rather
//! than the crate's xorshift generator.  See `request_options.rs` for the
//! compatibility rationale.

#![allow(unsafe_code)]

use std::cell::Cell;

const LEGACY_RAND_MAX: f64 = 2_147_483_647.0;

mod reconnect_policy_rand_ffi {
    extern "C" {
        pub(super) fn srpc_rand_raw() -> i32;
    }
}

fn legacy_rand_double(min: f64, max: f64) -> f64 {
    if max == min {
        return min;
    }
    assert!(max > min);
    let raw = unsafe { reconnect_policy_rand_ffi::srpc_rand_raw() };
    ((raw as f64) / (LEGACY_RAND_MAX / (max - min))) + min
}

pub struct ReconnectPolicy {
    pub auto_reconnect: bool,
    pub max_retries: u32,
    pub initial_delay_ms: u32,
    pub max_delay_ms: u32,
    pub backoff_multiplier: f64,
    pub jitter_enabled: bool,
}

impl Copy for ReconnectPolicy {}

impl Clone for ReconnectPolicy {
    fn clone(&self) -> ReconnectPolicy {
        ReconnectPolicy {
            auto_reconnect: self.auto_reconnect,
            max_retries: self.max_retries,
            initial_delay_ms: self.initial_delay_ms,
            max_delay_ms: self.max_delay_ms,
            backoff_multiplier: self.backoff_multiplier,
            jitter_enabled: self.jitter_enabled,
        }
    }
}

impl ReconnectPolicy {
    pub fn new() -> ReconnectPolicy {
        ReconnectPolicy {
            auto_reconnect: true,
            max_retries: 5_u32,
            initial_delay_ms: 1_000_u32,
            max_delay_ms: 30_000_u32,
            backoff_multiplier: 2.0_f64,
            jitter_enabled: true,
        }
    }

    pub fn aggressive() -> ReconnectPolicy {
        ReconnectPolicy {
            auto_reconnect: true,
            max_retries: 0_u32,
            initial_delay_ms: 100_u32,
            max_delay_ms: 5_000_u32,
            backoff_multiplier: 1.5_f64,
            jitter_enabled: true,
        }
    }

    pub fn conservative() -> ReconnectPolicy {
        ReconnectPolicy::new()
    }

    pub fn no_retry() -> ReconnectPolicy {
        ReconnectPolicy {
            auto_reconnect: false,
            max_retries: 0_u32,
            initial_delay_ms: 0_u32,
            max_delay_ms: 0_u32,
            backoff_multiplier: 1.0_f64,
            jitter_enabled: false,
        }
    }
}

pub struct ReconnectCalculator<'policy> {
    pub policy: &'policy ReconnectPolicy,
    pub retries: Cell<u32>,
}

impl<'policy> ReconnectCalculator<'policy> {
    pub fn new(policy: &'policy ReconnectPolicy) -> ReconnectCalculator<'policy> {
        ReconnectCalculator {
            policy,
            retries: Cell::new(0_u32),
        }
    }

    pub fn should_retry(&self) -> bool {
        if !self.policy.auto_reconnect {
            return false;
        }
        if self.policy.max_retries == 0 {
            return true;
        }
        self.retries.get() < self.policy.max_retries
    }

    pub fn next_delay_ms(&self) -> u32 {
        let count: u32 = self.retries.get();
        self.retries.set(count + 1_u32);

        let mut delay: f64 = self.policy.initial_delay_ms as f64;
        let mut index: u32 = 0_u32;
        while index < count {
            delay *= self.policy.backoff_multiplier;
            if delay >= self.policy.max_delay_ms as f64 {
                delay = self.policy.max_delay_ms as f64;
                break;
            }
            index += 1;
        }

        if delay > self.policy.max_delay_ms as f64 {
            delay = self.policy.max_delay_ms as f64;
        }

        if self.policy.jitter_enabled && delay > 0.0_f64 {
            delay *= legacy_rand_double(0.5_f64, 1.5_f64);
        }

        delay as u32
    }

    pub fn peek_delay_ms(&self) -> u32 {
        let count: u32 = self.retries.get();
        let mut delay: f64 = self.policy.initial_delay_ms as f64;
        let mut index: u32 = 0_u32;
        while index < count {
            delay *= self.policy.backoff_multiplier;
            if delay >= self.policy.max_delay_ms as f64 {
                delay = self.policy.max_delay_ms as f64;
                break;
            }
            index += 1;
        }

        if delay > self.policy.max_delay_ms as f64 {
            delay = self.policy.max_delay_ms as f64;
        }

        delay as u32
    }

    pub fn reset(&self) {
        self.retries.set(0_u32);
    }

    pub fn retry_count(&self) -> u32 {
        self.retries.get()
    }

    pub fn retries_exhausted(&self) -> bool {
        if !self.policy.auto_reconnect {
            return true;
        }
        if self.policy.max_retries == 0 {
            return false;
        }
        self.retries.get() >= self.policy.max_retries
    }
}
