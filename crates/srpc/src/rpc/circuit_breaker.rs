//! Three-state circuit breaker used by the legacy `rrr` client.
//!
//! Names and integer discriminants intentionally match `rrr.circuit_breaker`:
//! this file is also the source used to generate that C++ module.  The state
//! machine is single-threaded, like the legacy `Cell`-based implementation.
//!
//! `current_time_us` delegates to the crate's shared process-local monotonic
//! epoch. All breaker decisions use elapsed differences.

use crate::base::monotonic::monotonic_time_us;
use std::cell::Cell;

/// Monotonic microseconds from the crate's nonzero process-local epoch.
pub fn current_time_us() -> u64 {
    monotonic_time_us()
}

#[allow(non_camel_case_types)]
#[repr(i32)]
pub enum CircuitState {
    CLOSED = 0,
    OPEN = 1,
    HALF_OPEN = 2,
}

impl Copy for CircuitState {}

impl Clone for CircuitState {
    fn clone(&self) -> CircuitState {
        match self {
            CircuitState::CLOSED => CircuitState::CLOSED,
            CircuitState::OPEN => CircuitState::OPEN,
            CircuitState::HALF_OPEN => CircuitState::HALF_OPEN,
        }
    }
}

#[allow(unreachable_patterns)]
pub fn circuit_state_to_string(state: self::CircuitState) -> &'static str {
    match state {
        CircuitState::CLOSED => "CLOSED",
        CircuitState::OPEN => "OPEN",
        CircuitState::HALF_OPEN => "HALF_OPEN",
        _ => "UNKNOWN",
    }
}

pub struct CircuitBreakerConfig {
    pub failure_threshold: u32,
    pub success_threshold: u32,
    pub timeout_ms: u32,
    pub enabled: bool,
}

impl Copy for CircuitBreakerConfig {}

impl Clone for CircuitBreakerConfig {
    fn clone(&self) -> CircuitBreakerConfig {
        CircuitBreakerConfig {
            failure_threshold: self.failure_threshold,
            success_threshold: self.success_threshold,
            timeout_ms: self.timeout_ms,
            enabled: self.enabled,
        }
    }
}

impl CircuitBreakerConfig {
    pub fn new() -> CircuitBreakerConfig {
        CircuitBreakerConfig {
            failure_threshold: 5,
            success_threshold: 3,
            timeout_ms: 30_000,
            enabled: true,
        }
    }

    pub fn defaults() -> CircuitBreakerConfig {
        CircuitBreakerConfig::new()
    }

    pub fn sensitive() -> CircuitBreakerConfig {
        CircuitBreakerConfig {
            failure_threshold: 3,
            success_threshold: 5,
            timeout_ms: 60_000,
            enabled: true,
        }
    }

    pub fn relaxed() -> CircuitBreakerConfig {
        CircuitBreakerConfig {
            failure_threshold: 10,
            success_threshold: 2,
            timeout_ms: 15_000,
            enabled: true,
        }
    }

    pub fn disabled() -> CircuitBreakerConfig {
        CircuitBreakerConfig {
            failure_threshold: 0,
            success_threshold: 0,
            timeout_ms: 0,
            enabled: false,
        }
    }
}

/// Per-connection circuit-breaker state.
///
/// The fields remain public because the generated legacy C++ struct exposes
/// these exact names.  Callers should normally use the methods below.
pub struct CircuitBreaker {
    pub config_field: Cell<CircuitBreakerConfig>,
    pub state_field: Cell<CircuitState>,
    pub failure_count_field: Cell<u32>,
    pub success_count_field: Cell<u32>,
    pub last_failure_time: Cell<u64>,
    pub probe_in_progress: Cell<bool>,
}

impl CircuitBreaker {
    pub fn new(config: self::CircuitBreakerConfig) -> CircuitBreaker {
        CircuitBreaker {
            config_field: Cell::new(config),
            state_field: Cell::new(CircuitState::CLOSED),
            failure_count_field: Cell::new(0),
            success_count_field: Cell::new(0),
            last_failure_time: Cell::new(0),
            probe_in_progress: Cell::new(false),
        }
    }

    pub fn set_config(&self, config: self::CircuitBreakerConfig) {
        self.config_field.set(config);
        self.reset();
    }

    pub fn allow_request(&self) -> bool {
        if !self.config_field.get().enabled {
            return true;
        }

        let current = self.state_field.get();
        if (current as i32) == (CircuitState::CLOSED as i32) {
            return true;
        }
        if (current as i32) == (CircuitState::OPEN as i32) {
            let now = current_time_us();
            let last = self.last_failure_time.get();
            let timeout_us = (self.config_field.get().timeout_ms as u64) * 1_000;

            if now - last >= timeout_us {
                self.state_field.set(CircuitState::HALF_OPEN);
                self.probe_in_progress.set(true);
                return true;
            }
            return false;
        }
        if (current as i32) == (CircuitState::HALF_OPEN as i32) {
            if !self.probe_in_progress.get() {
                self.probe_in_progress.set(true);
                return true;
            }
            return false;
        }
        false
    }

    pub fn record_success(&self) {
        if !self.config_field.get().enabled {
            return;
        }

        let current = self.state_field.get();
        if (current as i32) == (CircuitState::CLOSED as i32) {
            self.failure_count_field.set(0);
            return;
        }
        if (current as i32) == (CircuitState::HALF_OPEN as i32) {
            self.probe_in_progress.set(false);
            let count = self.success_count_field.get() + 1;
            self.success_count_field.set(count);

            if count >= self.config_field.get().success_threshold {
                self.state_field.set(CircuitState::CLOSED);
                self.failure_count_field.set(0);
                self.success_count_field.set(0);
            }
            return;
        }
        if (current as i32) == (CircuitState::OPEN as i32) {
            self.probe_in_progress.set(false);
        }
    }

    pub fn record_failure(&self) {
        if !self.config_field.get().enabled {
            return;
        }

        let current = self.state_field.get();
        if (current as i32) == (CircuitState::CLOSED as i32) {
            let count = self.failure_count_field.get() + 1;
            self.failure_count_field.set(count);

            if count >= self.config_field.get().failure_threshold {
                self.state_field.set(CircuitState::OPEN);
                self.last_failure_time.set(current_time_us());
                self.failure_count_field.set(0);
                self.success_count_field.set(0);
            }
            return;
        }
        if (current as i32) == (CircuitState::HALF_OPEN as i32) {
            self.probe_in_progress.set(false);
            self.state_field.set(CircuitState::OPEN);
            self.last_failure_time.set(current_time_us());
            self.success_count_field.set(0);
            return;
        }
        if (current as i32) == (CircuitState::OPEN as i32) {
            self.last_failure_time.set(current_time_us());
        }
    }

    pub fn state(&self) -> CircuitState {
        self.state_field.get()
    }

    pub fn is_open(&self) -> bool {
        (self.state_field.get() as i32) == (CircuitState::OPEN as i32)
    }

    pub fn is_closed(&self) -> bool {
        (self.state_field.get() as i32) == (CircuitState::CLOSED as i32)
    }

    pub fn is_half_open(&self) -> bool {
        (self.state_field.get() as i32) == (CircuitState::HALF_OPEN as i32)
    }

    pub fn reset(&self) {
        self.state_field.set(CircuitState::CLOSED);
        self.failure_count_field.set(0);
        self.success_count_field.set(0);
        self.last_failure_time.set(0);
        self.probe_in_progress.set(false);
    }

    pub fn failure_count(&self) -> u32 {
        self.failure_count_field.get()
    }

    pub fn success_count(&self) -> u32 {
        self.success_count_field.get()
    }

    pub fn config(&self) -> CircuitBreakerConfig {
        self.config_field.get()
    }
}
