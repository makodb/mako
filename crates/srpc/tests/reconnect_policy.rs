use std::sync::atomic::{AtomicI32, AtomicU32, Ordering};
use std::sync::Mutex;

static NEXT_RAW: AtomicI32 = AtomicI32::new(0);
static RAW_CALLS: AtomicU32 = AtomicU32::new(0);
static RANDOM_TEST_LOCK: Mutex<()> = Mutex::new(());

#[allow(unsafe_code)]
#[no_mangle]
pub extern "C" fn srpc_rand_raw() -> i32 {
    RAW_CALLS.fetch_add(1, Ordering::Relaxed);
    NEXT_RAW.load(Ordering::Relaxed)
}

fn set_raw(raw: i32) {
    NEXT_RAW.store(raw, Ordering::Relaxed);
}

fn reset_raw_calls() {
    RAW_CALLS.store(0, Ordering::Relaxed);
}

fn raw_calls() -> u32 {
    RAW_CALLS.load(Ordering::Relaxed)
}

use srpc::rpc::reconnect_policy::{ReconnectCalculator, ReconnectPolicy};

#[test]
fn presets_match_the_legacy_values() {
    let defaults = ReconnectPolicy::new();
    assert!(defaults.auto_reconnect);
    assert_eq!(defaults.max_retries, 5);
    assert_eq!(defaults.initial_delay_ms, 1_000);
    assert_eq!(defaults.max_delay_ms, 30_000);
    assert_eq!(defaults.backoff_multiplier, 2.0);
    assert!(defaults.jitter_enabled);

    let conservative = ReconnectPolicy::conservative();
    assert_eq!(conservative.max_retries, defaults.max_retries);
    assert_eq!(conservative.initial_delay_ms, defaults.initial_delay_ms);

    let aggressive = ReconnectPolicy::aggressive();
    assert!(aggressive.auto_reconnect);
    assert_eq!(aggressive.max_retries, 0);
    assert_eq!(aggressive.initial_delay_ms, 100);
    assert_eq!(aggressive.max_delay_ms, 5_000);
    assert_eq!(aggressive.backoff_multiplier, 1.5);
    assert!(aggressive.jitter_enabled);

    let none = ReconnectPolicy::no_retry();
    assert!(!none.auto_reconnect);
    assert_eq!(none.max_retries, 0);
    assert_eq!(none.initial_delay_ms, 0);
    assert_eq!(none.max_delay_ms, 0);
    assert_eq!(none.backoff_multiplier, 1.0);
    assert!(!none.jitter_enabled);
}

#[test]
fn retry_state_and_reset_match_the_legacy_calculator() {
    let mut policy = ReconnectPolicy::new();
    policy.max_retries = 3;
    policy.jitter_enabled = false;
    let calculator = ReconnectCalculator::new(&policy);

    assert_eq!(calculator.retry_count(), 0);
    assert!(calculator.should_retry());
    assert!(!calculator.retries_exhausted());

    calculator.next_delay_ms();
    calculator.next_delay_ms();
    assert!(calculator.should_retry());
    calculator.next_delay_ms();
    assert!(!calculator.should_retry());
    assert!(calculator.retries_exhausted());

    calculator.reset();
    assert_eq!(calculator.retry_count(), 0);
    assert!(calculator.should_retry());
}

#[test]
fn disabled_and_unlimited_retry_modes_are_distinct() {
    let disabled = ReconnectPolicy::no_retry();
    let disabled_calculator = ReconnectCalculator::new(&disabled);
    assert!(!disabled_calculator.should_retry());
    assert!(disabled_calculator.retries_exhausted());

    let mut unlimited = ReconnectPolicy::aggressive();
    unlimited.jitter_enabled = false;
    let unlimited_calculator = ReconnectCalculator::new(&unlimited);
    let mut index = 0;
    while index < 100 {
        assert!(unlimited_calculator.should_retry());
        unlimited_calculator.next_delay_ms();
        index += 1;
    }
    assert!(unlimited_calculator.should_retry());
    assert!(!unlimited_calculator.retries_exhausted());
}

#[test]
fn exponential_backoff_cap_and_peek_are_exact_without_jitter() {
    let _random_guard = RANDOM_TEST_LOCK.lock().unwrap();
    let mut policy = ReconnectPolicy::new();
    policy.initial_delay_ms = 100;
    policy.max_delay_ms = 500;
    policy.backoff_multiplier = 2.0;
    policy.jitter_enabled = false;
    let calculator = ReconnectCalculator::new(&policy);
    reset_raw_calls();

    assert_eq!(calculator.peek_delay_ms(), 100);
    assert_eq!(calculator.peek_delay_ms(), 100);
    assert_eq!(calculator.retry_count(), 0);
    assert_eq!(calculator.next_delay_ms(), 100);
    assert_eq!(calculator.peek_delay_ms(), 200);
    assert_eq!(calculator.next_delay_ms(), 200);
    assert_eq!(calculator.next_delay_ms(), 400);
    assert_eq!(calculator.next_delay_ms(), 500);
    assert_eq!(calculator.next_delay_ms(), 500);
    assert_eq!(raw_calls(), 0);
}

#[test]
fn jitter_uses_the_legacy_rand_r_scaling_endpoints() {
    let _random_guard = RANDOM_TEST_LOCK.lock().unwrap();
    let mut policy = ReconnectPolicy::new();
    policy.initial_delay_ms = 1_000;
    policy.max_delay_ms = 10_000;
    policy.backoff_multiplier = 1.0;
    policy.jitter_enabled = true;
    reset_raw_calls();

    set_raw(0);
    let low = ReconnectCalculator::new(&policy);
    assert_eq!(low.next_delay_ms(), 500);

    set_raw(i32::MAX);
    let high = ReconnectCalculator::new(&policy);
    assert_eq!(high.next_delay_ms(), 1_500);
    assert_eq!(raw_calls(), 2);
}

#[test]
fn zero_delay_skips_jitter_but_still_advances_retry_count() {
    let _random_guard = RANDOM_TEST_LOCK.lock().unwrap();
    let mut policy = ReconnectPolicy::new();
    policy.initial_delay_ms = 0;
    policy.jitter_enabled = true;
    let calculator = ReconnectCalculator::new(&policy);
    reset_raw_calls();

    assert_eq!(calculator.next_delay_ms(), 0);
    assert_eq!(calculator.retry_count(), 1);
    assert_eq!(raw_calls(), 0);
}
