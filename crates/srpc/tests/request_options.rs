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

use srpc::rpc::request_options::{timeout_type_to_string, RequestOptions, TimeoutType};

#[test]
fn timeout_discriminants_and_names_match_the_legacy_surface() {
    assert_eq!(core::mem::size_of::<TimeoutType>(), 1);
    assert_eq!(core::mem::align_of::<TimeoutType>(), 1);
    let cases = [
        (TimeoutType::NONE, 0_u8, "NONE"),
        (TimeoutType::CONNECT_TIMEOUT, 1, "CONNECT_TIMEOUT"),
        (TimeoutType::REQUEST_TIMEOUT, 2, "REQUEST_TIMEOUT"),
        (TimeoutType::RESPONSE_TIMEOUT, 3, "RESPONSE_TIMEOUT"),
        (TimeoutType::TOTAL_TIMEOUT, 4, "TOTAL_TIMEOUT"),
    ];
    let mut index = 0;
    while index < cases.len() {
        assert_eq!(cases[index].0 as u8, cases[index].1);
        assert_eq!(timeout_type_to_string(cases[index].0), cases[index].2);
        index += 1;
    }
}

#[test]
fn presets_match_the_legacy_values() {
    let defaults = RequestOptions::defaults();
    assert_eq!(defaults.timeout_ms, 1_000);
    assert_eq!(defaults.total_timeout_ms, 0);
    assert_eq!(defaults.max_retries, 0);
    assert_eq!(defaults.base_delay_ms, 50);
    assert_eq!(defaults.max_delay_ms, 5_000);
    assert_eq!(defaults.jitter_factor, 0.1);
    assert!(!defaults.idempotent);

    let via_new = RequestOptions::new();
    assert_eq!(via_new.timeout_ms, defaults.timeout_ms);
    assert_eq!(via_new.base_delay_ms, defaults.base_delay_ms);

    let retry = RequestOptions::with_retry(7, 2_500);
    assert_eq!(retry.timeout_ms, 2_500);
    assert_eq!(retry.max_retries, 7);
    assert!(retry.idempotent);

    let idempotent = RequestOptions::idempotent_retry(3);
    assert_eq!(idempotent.timeout_ms, 1_000);
    assert_eq!(idempotent.max_retries, 3);
    assert!(idempotent.idempotent);

    let none = RequestOptions::no_timeout();
    assert_eq!(none.timeout_ms, 0);
    assert!(!none.idempotent);

    let fast = RequestOptions::fast();
    assert_eq!(fast.timeout_ms, 100);
    assert_eq!(fast.max_retries, 2);
    assert_eq!(fast.base_delay_ms, 10);
    assert_eq!(fast.max_delay_ms, 100);

    let patient = RequestOptions::patient();
    assert_eq!(patient.timeout_ms, 10_000);
    assert_eq!(patient.total_timeout_ms, 60_000);
    assert_eq!(patient.max_retries, 5);
    assert_eq!(patient.base_delay_ms, 500);
    assert_eq!(patient.max_delay_ms, 10_000);
}

#[test]
fn retry_boundary_is_idempotency_and_count_gated() {
    let mut options = RequestOptions::idempotent_retry(3);
    assert!(options.can_retry(0));
    assert!(options.can_retry(2));
    assert!(!options.can_retry(3));
    options.idempotent = false;
    assert!(!options.can_retry(0));
}

#[test]
fn exponential_delay_and_cap_are_exact_without_jitter() {
    let _random_guard = RANDOM_TEST_LOCK.lock().unwrap();
    let mut options = RequestOptions::defaults();
    options.base_delay_ms = 100;
    options.max_delay_ms = 500;
    options.jitter_factor = 0.0;
    reset_raw_calls();

    assert_eq!(options.calculate_delay_ms(0), 100);
    assert_eq!(options.calculate_delay_ms(1), 200);
    assert_eq!(options.calculate_delay_ms(2), 400);
    assert_eq!(options.calculate_delay_ms(3), 500);
    assert_eq!(options.calculate_delay_ms(1_000), 500);
    assert_eq!(raw_calls(), 0);

    options.base_delay_ms = 1_000;
    options.max_delay_ms = 500;
    assert_eq!(options.calculate_delay_ms(0), 500);
}

#[test]
fn jitter_uses_the_legacy_rand_r_scaling_endpoints() {
    let _random_guard = RANDOM_TEST_LOCK.lock().unwrap();
    let mut options = RequestOptions::defaults();
    options.base_delay_ms = 1_000;
    options.max_delay_ms = 5_000;
    options.jitter_factor = 0.2;
    reset_raw_calls();

    set_raw(0);
    // `jitter_factor` is stored as f32 before widening, so its 0.2 value is
    // slightly above exact 0.2; the negative endpoint therefore truncates
    // 899.999... to 899, exactly as the generated C++ does.
    assert_eq!(options.calculate_delay_ms(0), 899);
    set_raw(i32::MAX);
    assert_eq!(options.calculate_delay_ms(0), 1_100);
    assert_eq!(raw_calls(), 2);

    // The legacy request-options path consumes one PRNG value whenever
    // jitter is enabled, even when multiplying a zero delay by it.
    options.base_delay_ms = 0;
    assert_eq!(options.calculate_delay_ms(0), 0);
    assert_eq!(raw_calls(), 3);
}

#[test]
fn total_timeout_queries_match_the_legacy_boundaries() {
    let mut options = RequestOptions::defaults();
    assert!(!options.is_total_timeout_exceeded(u64::MAX));
    assert_eq!(options.remaining_time_ms(99), u64::MAX);

    options.total_timeout_ms = 5_000;
    assert!(!options.is_total_timeout_exceeded(4_999));
    assert!(options.is_total_timeout_exceeded(5_000));
    assert_eq!(options.remaining_time_ms(0), 5_000);
    assert_eq!(options.remaining_time_ms(4_999), 1);
    assert_eq!(options.remaining_time_ms(5_000), 0);
    assert_eq!(options.remaining_time_ms(9_000), 0);
}

#[test]
fn options_remain_plain_copyable_values() {
    let mut original = RequestOptions::patient();
    let copy = original;
    original.timeout_ms = 1;
    assert_eq!(original.timeout_ms, 1);
    assert_eq!(copy.timeout_ms, 10_000);
    assert_eq!(copy.max_retries, 5);
}
