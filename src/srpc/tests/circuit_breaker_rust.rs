use std::mem::{align_of, offset_of, size_of};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;

use srpc::circuit_breaker::{
    circuit_state_to_string, current_time_us, CircuitBreaker, CircuitBreakerConfig, CircuitState,
};

static NOW_US: AtomicU64 = AtomicU64::new(0);
static CLOCK_TEST_LOCK: Mutex<()> = Mutex::new(());

fn test_now() -> u64 {
    NOW_US.load(Ordering::SeqCst)
}

fn set_now(value: u64) {
    NOW_US.store(value, Ordering::SeqCst);
}

fn assert_send<T: Send>() {}

#[test]
fn layouts_discriminants_and_thread_traits_match_the_cpp_surface() {
    assert_send::<CircuitBreaker>();

    fn assert_sync<T: Sync>() {}
    assert_sync::<CircuitBreaker>();

    assert_eq!(size_of::<CircuitState>(), 4);
    assert_eq!(align_of::<CircuitState>(), 4);
    assert_eq!(CircuitState::CLOSED as i32, 0);
    assert_eq!(CircuitState::OPEN as i32, 1);
    assert_eq!(CircuitState::HALF_OPEN as i32, 2);

    assert_eq!(size_of::<CircuitBreakerConfig>(), 16);
    assert_eq!(align_of::<CircuitBreakerConfig>(), 4);
    assert_eq!(offset_of!(CircuitBreakerConfig, failure_threshold), 0);
    assert_eq!(offset_of!(CircuitBreakerConfig, success_threshold), 4);
    assert_eq!(offset_of!(CircuitBreakerConfig, timeout_ms), 8);
    assert_eq!(offset_of!(CircuitBreakerConfig, enabled), 12);

}

#[test]
fn factories_and_names_preserve_the_legacy_values() {
    assert_eq!(
        CircuitBreakerConfig::new(),
        CircuitBreakerConfig::defaults()
    );
    assert_eq!(
        CircuitBreakerConfig::defaults(),
        CircuitBreakerConfig {
            failure_threshold: 5,
            success_threshold: 3,
            timeout_ms: 30_000,
            enabled: true,
        }
    );
    assert_eq!(CircuitBreakerConfig::sensitive().failure_threshold, 3);
    assert_eq!(CircuitBreakerConfig::sensitive().success_threshold, 5);
    assert_eq!(CircuitBreakerConfig::sensitive().timeout_ms, 60_000);
    assert_eq!(CircuitBreakerConfig::relaxed().failure_threshold, 10);
    assert_eq!(CircuitBreakerConfig::relaxed().success_threshold, 2);
    assert_eq!(CircuitBreakerConfig::relaxed().timeout_ms, 15_000);
    let disabled = CircuitBreakerConfig::disabled();
    assert_eq!(disabled.failure_threshold, 0);
    assert_eq!(disabled.success_threshold, 0);
    assert_eq!(disabled.timeout_ms, 0);
    assert!(!disabled.enabled);

    assert_eq!(circuit_state_to_string(CircuitState::CLOSED), "CLOSED");
    assert_eq!(circuit_state_to_string(CircuitState::OPEN), "OPEN");
    assert_eq!(
        circuit_state_to_string(CircuitState::HALF_OPEN),
        "HALF_OPEN"
    );
}

#[test]
fn closed_open_half_open_and_reset_transitions_are_exact() {
    let _clock_guard = CLOCK_TEST_LOCK.lock().unwrap();
    set_now(1_000_000);
    let mut config = CircuitBreakerConfig::defaults();
    config.failure_threshold = 2;
    config.success_threshold = 2;
    config.timeout_ms = 10;
    let breaker = CircuitBreaker::new(config);

    assert!(breaker.allow_request_at(test_now()));
    breaker.record_failure_at(test_now());
    assert_eq!(breaker.failure_count(), 1);
    assert!(breaker.is_closed());
    breaker.record_failure_at(test_now());
    assert!(breaker.is_open());
    assert_eq!(breaker.failure_count(), 0);
    assert_eq!(breaker.last_failure_time.get(), 1_000_000);

    set_now(1_009_999);
    assert!(!breaker.allow_request_at(test_now()));
    set_now(1_010_000);
    assert!(breaker.allow_request_at(test_now()));
    assert!(breaker.is_half_open());
    assert!(!breaker.allow_request_at(test_now()));

    breaker.record_success();
    assert_eq!(breaker.success_count(), 1);
    assert!(breaker.allow_request_at(test_now()));
    breaker.record_success();
    assert!(breaker.is_closed());
    assert_eq!(breaker.success_count(), 0);

    breaker.record_failure_at(test_now());
    breaker.reset();
    assert!(breaker.is_closed());
    assert_eq!(breaker.failure_count(), 0);
    assert_eq!(breaker.last_failure_time.get(), 0);
}

#[test]
fn disabled_and_wrapping_boundaries_match_unsigned_cpp() {
    let _clock_guard = CLOCK_TEST_LOCK.lock().unwrap();
    set_now(7);
    assert!(current_time_us() > 0);

    let disabled = CircuitBreaker::new(CircuitBreakerConfig::disabled());
    disabled.record_failure_at(test_now());
    assert!(disabled.allow_request_at(test_now()));
    assert!(disabled.is_closed());

    let mut config = CircuitBreakerConfig::defaults();
    config.failure_threshold = u32::MAX;
    let breaker = CircuitBreaker::new(config);
    breaker.failure_count_field.set(u32::MAX);
    breaker.record_failure_at(test_now());
    assert_eq!(breaker.failure_count(), 0);
    assert!(breaker.is_closed());

    breaker.state_field.set(CircuitState::OPEN);
    breaker.last_failure_time.set(u64::MAX - 5);
    breaker.config_field.set(CircuitBreakerConfig {
        failure_threshold: 1,
        success_threshold: 1,
        timeout_ms: 0,
        enabled: true,
    });
    set_now(4);
    assert!(breaker.allow_request_at(test_now()));
    assert!(breaker.is_half_open());
}
