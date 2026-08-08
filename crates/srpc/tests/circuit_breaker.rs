use srpc::rpc::circuit_breaker::{
    circuit_state_to_string, current_time_us, CircuitBreaker, CircuitBreakerConfig, CircuitState,
};
use std::thread;
use std::time::Duration;

fn zero_config() -> CircuitBreakerConfig {
    CircuitBreakerConfig {
        failure_threshold: 0,
        success_threshold: 0,
        timeout_ms: 0,
        enabled: false,
    }
}

fn state_is(actual: CircuitState, expected: CircuitState) -> bool {
    (actual as i32) == (expected as i32)
}

#[test]
fn enum_discriminants_and_legacy_names_are_stable() {
    assert_eq!(CircuitState::CLOSED as i32, 0);
    assert_eq!(CircuitState::OPEN as i32, 1);
    assert_eq!(CircuitState::HALF_OPEN as i32, 2);
    assert_eq!(circuit_state_to_string(CircuitState::CLOSED), "CLOSED");
    assert_eq!(circuit_state_to_string(CircuitState::OPEN), "OPEN");
    assert_eq!(
        circuit_state_to_string(CircuitState::HALF_OPEN),
        "HALF_OPEN"
    );
}

#[test]
fn presets_exactly_match_the_legacy_values() {
    let config = CircuitBreakerConfig::defaults();
    assert!(config.enabled);
    assert_eq!(config.failure_threshold, 5);
    assert_eq!(config.success_threshold, 3);
    assert_eq!(config.timeout_ms, 30_000);

    let sensitive = CircuitBreakerConfig::sensitive();
    assert!(sensitive.enabled);
    assert_eq!(sensitive.failure_threshold, 3);
    assert_eq!(sensitive.success_threshold, 5);
    assert_eq!(sensitive.timeout_ms, 60_000);

    let relaxed = CircuitBreakerConfig::relaxed();
    assert!(relaxed.enabled);
    assert_eq!(relaxed.failure_threshold, 10);
    assert_eq!(relaxed.success_threshold, 2);
    assert_eq!(relaxed.timeout_ms, 15_000);

    let disabled = CircuitBreakerConfig::disabled();
    assert!(!disabled.enabled);
    assert_eq!(disabled.failure_threshold, 0);
    assert_eq!(disabled.success_threshold, 0);
    assert_eq!(disabled.timeout_ms, 0);

    let via_new = CircuitBreakerConfig::new();
    assert_eq!(via_new.failure_threshold, config.failure_threshold);
    assert_eq!(via_new.success_threshold, config.success_threshold);
    assert_eq!(via_new.timeout_ms, config.timeout_ms);
    assert_eq!(via_new.enabled, config.enabled);
}

#[test]
fn zero_initialized_config_is_disabled_like_cpp_brace_initialization() {
    let breaker = CircuitBreaker::new(zero_config());
    assert!(breaker.is_closed());
    assert!(breaker.allow_request());
    for _ in 0..100 {
        breaker.record_failure();
    }
    assert!(breaker.is_closed());
    assert_eq!(breaker.failure_count(), 0);
}

#[test]
fn consecutive_failures_open_and_success_clears_the_streak() {
    let mut config = CircuitBreakerConfig::defaults();
    config.failure_threshold = 3;
    let breaker = CircuitBreaker::new(config);

    breaker.record_failure();
    breaker.record_failure();
    assert_eq!(breaker.failure_count(), 2);
    breaker.record_success();
    assert_eq!(breaker.failure_count(), 0);

    breaker.record_failure();
    breaker.record_failure();
    assert!(breaker.is_closed());
    breaker.record_failure();
    assert!(breaker.is_open());
    assert_eq!(breaker.failure_count(), 0);
    assert!(!breaker.allow_request());
}

#[test]
fn open_timeout_admits_exactly_one_half_open_probe() {
    let mut config = CircuitBreakerConfig::defaults();
    config.failure_threshold = 1;
    config.timeout_ms = 5;
    let breaker = CircuitBreaker::new(config);

    breaker.record_failure();
    assert!(breaker.is_open());
    assert!(!breaker.allow_request());

    thread::sleep(Duration::from_millis(12));
    assert!(breaker.allow_request());
    assert!(breaker.is_half_open());
    assert!(breaker.probe_in_progress.get());
    assert!(!breaker.allow_request());
}

#[test]
fn enough_half_open_successes_close_and_reset_counts() {
    let mut config = CircuitBreakerConfig::defaults();
    config.failure_threshold = 1;
    config.success_threshold = 3;
    config.timeout_ms = 0;
    let breaker = CircuitBreaker::new(config);

    breaker.record_failure();
    assert!(breaker.allow_request());
    assert!(breaker.is_half_open());

    breaker.record_success();
    assert!(breaker.is_half_open());
    assert_eq!(breaker.success_count(), 1);
    assert!(breaker.allow_request());

    breaker.record_success();
    assert!(breaker.is_half_open());
    assert_eq!(breaker.success_count(), 2);
    assert!(breaker.allow_request());

    breaker.record_success();
    assert!(breaker.is_closed());
    assert_eq!(breaker.failure_count(), 0);
    assert_eq!(breaker.success_count(), 0);
}

#[test]
fn failed_probe_reopens_and_restarts_the_timeout() {
    let mut config = CircuitBreakerConfig::defaults();
    config.failure_threshold = 1;
    config.timeout_ms = 0;
    let breaker = CircuitBreaker::new(config);

    breaker.record_failure();
    assert!(breaker.allow_request());
    assert!(breaker.is_half_open());
    breaker.record_success();
    assert_eq!(breaker.success_count(), 1);
    assert!(breaker.allow_request());

    breaker.record_failure();
    assert!(breaker.is_open());
    assert_eq!(breaker.success_count(), 0);
    assert!(!breaker.probe_in_progress.get());
    assert!(breaker.last_failure_time.get() <= current_time_us());
}

#[test]
fn failure_while_open_refreshes_the_recovery_deadline() {
    let mut config = CircuitBreakerConfig::defaults();
    config.failure_threshold = 1;
    config.timeout_ms = 20;
    let breaker = CircuitBreaker::new(config);

    breaker.record_failure();
    let opened = breaker.last_failure_time.get();
    thread::sleep(Duration::from_millis(8));
    breaker.record_failure();
    let refreshed = breaker.last_failure_time.get();
    assert!(refreshed > opened);
    assert!(!breaker.allow_request(), "the refreshed timeout starts now");
}

#[test]
fn reset_and_set_config_clear_all_state() {
    let mut initial = CircuitBreakerConfig::defaults();
    initial.failure_threshold = 1;
    let breaker = CircuitBreaker::new(initial);
    breaker.record_failure();
    assert!(breaker.is_open());

    breaker.reset();
    assert!(breaker.is_closed());
    assert_eq!(breaker.failure_count(), 0);
    assert_eq!(breaker.success_count(), 0);
    assert_eq!(breaker.last_failure_time.get(), 0);
    assert!(!breaker.probe_in_progress.get());

    let replacement = CircuitBreakerConfig::relaxed();
    breaker.set_config(replacement);
    let observed = breaker.config();
    assert_eq!(observed.failure_threshold, 10);
    assert_eq!(observed.success_threshold, 2);
    assert_eq!(observed.timeout_ms, 15_000);
    assert!(observed.enabled);
    assert!(state_is(breaker.state(), CircuitState::CLOSED));
}

#[test]
fn monotonic_clock_never_moves_backwards() {
    let first = current_time_us();
    assert_ne!(first, 0, "zero is reserved as the never-recorded sentinel");
    thread::sleep(Duration::from_millis(1));
    let second = current_time_us();
    assert!(second >= first);
}
