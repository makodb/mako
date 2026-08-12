// Canonical Rust source for the rrr.reconnect_policy module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.
use std::cell::Cell;

#[cfg_attr(any(), cpp_import_namespace(rrr))]
use crate::rand::{randgen_rand_max, randgen_rand_raw};

#[cfg_attr(not(any()), derive(Clone, Copy, Debug, PartialEq))]
#[repr(C)]
pub struct ReconnectPolicy {
    pub auto_reconnect: bool,
    pub max_retries: u32,
    pub initial_delay_ms: u32,
    pub max_delay_ms: u32,
    pub backoff_multiplier: f64,
    pub jitter_enabled: bool,
}

impl ReconnectPolicy {
    #[allow(clippy::new_without_default)]
    pub fn new() -> ReconnectPolicy {
        ReconnectPolicy {
            auto_reconnect: true,
            max_retries: 5u32,
            initial_delay_ms: 1000u32,
            max_delay_ms: 30000u32,
            backoff_multiplier: 2.0,
            jitter_enabled: true,
        }
    }

    pub fn aggressive() -> ReconnectPolicy {
        ReconnectPolicy {
            auto_reconnect: true,
            max_retries: 0u32,
            initial_delay_ms: 100u32,
            max_delay_ms: 5000u32,
            backoff_multiplier: 1.5,
            jitter_enabled: true,
        }
    }

    pub fn conservative() -> ReconnectPolicy {
        ReconnectPolicy::new()
    }

    pub fn no_retry() -> ReconnectPolicy {
        ReconnectPolicy {
            auto_reconnect: false,
            max_retries: 0u32,
            initial_delay_ms: 0u32,
            max_delay_ms: 0u32,
            backoff_multiplier: 1.0,
            jitter_enabled: false,
        }
    }
}

#[repr(C)]
pub struct ReconnectCalculator<'p> {
    pub policy: &'p ReconnectPolicy,
    pub retries: Cell<u32>,
}

impl<'p> ReconnectCalculator<'p> {
    pub fn new(policy: &'p ReconnectPolicy) -> ReconnectCalculator<'p> {
        ReconnectCalculator {
            policy,
            retries: Cell::new(0u32),
        }
    }

    pub fn should_retry(&self) -> bool {
        if !self.policy.auto_reconnect {
            return false;
        }
        if self.policy.max_retries == 0u32 {
            return true;
        }
        self.retries.get() < self.policy.max_retries
    }

    pub fn next_delay_ms(&self) -> u32 {
        let count: u32 = self.retries.get();
        self.retries.set(count.wrapping_add(1u32));

        let mut delay: f64 = self.policy.initial_delay_ms as f64;
        let mut i: u32 = 0u32;
        while i < count {
            delay *= self.policy.backoff_multiplier;
            if delay >= (self.policy.max_delay_ms as f64) {
                delay = self.policy.max_delay_ms as f64;
                break;
            }
            i += 1u32;
        }

        if delay > (self.policy.max_delay_ms as f64) {
            delay = self.policy.max_delay_ms as f64;
        }

        if self.policy.jitter_enabled && delay > 0.0f64 {
            delay *= ((randgen_rand_raw() as f64) / randgen_rand_max()) + 0.5f64;
        }

        delay as u32
    }

    pub fn peek_delay_ms(&self) -> u32 {
        let count: u32 = self.retries.get();

        let mut delay: f64 = self.policy.initial_delay_ms as f64;
        let mut i: u32 = 0u32;
        while i < count {
            delay *= self.policy.backoff_multiplier;
            if delay >= (self.policy.max_delay_ms as f64) {
                delay = self.policy.max_delay_ms as f64;
                break;
            }
            i += 1u32;
        }

        if delay > (self.policy.max_delay_ms as f64) {
            delay = self.policy.max_delay_ms as f64;
        }

        delay as u32
    }

    pub fn reset(&self) {
        self.retries.set(0u32);
    }

    pub fn retry_count(&self) -> u32 {
        self.retries.get()
    }

    pub fn retries_exhausted(&self) -> bool {
        if !self.policy.auto_reconnect {
            return true;
        }
        if self.policy.max_retries == 0u32 {
            return false;
        }
        self.retries.get() >= self.policy.max_retries
    }
}
