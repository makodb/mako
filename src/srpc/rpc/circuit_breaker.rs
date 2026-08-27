// Canonical Rust source for the srpc.circuit_breaker module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.
use std::cell::Cell;

#[allow(unsafe_code)]
unsafe extern "C" {
    fn srpc_clock_monotonic_us() -> u64;
}

// @unsafe - thin wrapper over the terminal plain-C monotonic-clock kernel.
#[allow(unsafe_code)]
pub fn current_time_us() -> u64 {
    unsafe { srpc_clock_monotonic_us() }
}

#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, PartialEq, Eq))]
#[repr(i32)]
pub enum CircuitState {
    CLOSED = 0,
    OPEN = 1,
    HALF_OPEN = 2,
}

#[allow(unreachable_patterns)]
pub fn circuit_state_to_string(state: CircuitState) -> &'static str {
    match state {
        CircuitState::CLOSED => "CLOSED",
        CircuitState::OPEN => "OPEN",
        CircuitState::HALF_OPEN => "HALF_OPEN",
        _ => "UNKNOWN",
    }
}

#[cfg_attr(not(any()), derive(Clone, Copy, Debug, PartialEq, Eq))]
#[repr(C)]
pub struct CircuitBreakerConfig {
    pub failure_threshold: u32,
    pub success_threshold: u32,
    pub timeout_ms: u32,
    pub enabled: bool,
}

impl CircuitBreakerConfig {
    #[allow(clippy::new_without_default)]
    pub fn new() -> CircuitBreakerConfig {
        CircuitBreakerConfig {
            failure_threshold: 5u32,
            success_threshold: 3u32,
            timeout_ms: 30000u32,
            enabled: true,
        }
    }

    pub fn defaults() -> CircuitBreakerConfig {
        CircuitBreakerConfig::new()
    }

    pub fn sensitive() -> CircuitBreakerConfig {
        CircuitBreakerConfig {
            failure_threshold: 3u32,
            success_threshold: 5u32,
            timeout_ms: 60000u32,
            enabled: true,
        }
    }

    pub fn relaxed() -> CircuitBreakerConfig {
        CircuitBreakerConfig {
            failure_threshold: 10u32,
            success_threshold: 2u32,
            timeout_ms: 15000u32,
            enabled: true,
        }
    }

    pub fn disabled() -> CircuitBreakerConfig {
        CircuitBreakerConfig {
            failure_threshold: 0u32,
            success_threshold: 0u32,
            timeout_ms: 0u32,
            enabled: false,
        }
    }
}

#[repr(C)]
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
            config_field: Cell::<CircuitBreakerConfig>::new(config),
            state_field: Cell::<CircuitState>::new(CircuitState::CLOSED),
            failure_count_field: Cell::<u32>::new(0u32),
            success_count_field: Cell::<u32>::new(0u32),
            last_failure_time: Cell::<u64>::new(0u64),
            probe_in_progress: Cell::<bool>::new(false),
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

        let current: CircuitState = self.state_field.get();

        if (current as i32) == (CircuitState::CLOSED as i32) {
            return true;
        }
        if (current as i32) == (CircuitState::OPEN as i32) {
            let now: u64 = current_time_us();
            let last: u64 = self.last_failure_time.get();
            let timeout_us: u64 = (self.config_field.get().timeout_ms as u64) * 1000u64;

            if now.wrapping_sub(last) >= timeout_us {
                let next: CircuitState = CircuitState::HALF_OPEN;
                self.state_field.set(next);
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

        let current: CircuitState = self.state_field.get();

        if (current as i32) == (CircuitState::CLOSED as i32) {
            self.failure_count_field.set(0u32);
            return;
        }
        if (current as i32) == (CircuitState::HALF_OPEN as i32) {
            self.probe_in_progress.set(false);
            let count: u32 = self.success_count_field.get().wrapping_add(1u32);
            self.success_count_field.set(count);

            if count >= self.config_field.get().success_threshold {
                let closed: CircuitState = CircuitState::CLOSED;
                self.state_field.set(closed);
                self.failure_count_field.set(0u32);
                self.success_count_field.set(0u32);
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

        let current: CircuitState = self.state_field.get();

        if (current as i32) == (CircuitState::CLOSED as i32) {
            let count: u32 = self.failure_count_field.get().wrapping_add(1u32);
            self.failure_count_field.set(count);

            if count >= self.config_field.get().failure_threshold {
                let open: CircuitState = CircuitState::OPEN;
                self.state_field.set(open);
                self.last_failure_time.set(current_time_us());
                self.failure_count_field.set(0u32);
                self.success_count_field.set(0u32);
            }
            return;
        }
        if (current as i32) == (CircuitState::HALF_OPEN as i32) {
            self.probe_in_progress.set(false);
            let open: CircuitState = CircuitState::OPEN;
            self.state_field.set(open);
            self.last_failure_time.set(current_time_us());
            self.success_count_field.set(0u32);
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
        let closed: CircuitState = CircuitState::CLOSED;
        self.state_field.set(closed);
        self.failure_count_field.set(0u32);
        self.success_count_field.set(0u32);
        self.last_failure_time.set(0u64);
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
