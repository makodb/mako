use std::mem::{align_of, offset_of, size_of};
use std::sync::{
    atomic::{AtomicI32, AtomicUsize, Ordering},
    Mutex,
};

use srpc::request_options::{timeout_type_to_string, RequestOptions, TimeoutType};

static RAW_VALUE: AtomicI32 = AtomicI32::new(0);
static RAW_DRAWS: AtomicUsize = AtomicUsize::new(0);
static RAND_STATE_TEST_GUARD: Mutex<()> = Mutex::new(());

#[allow(unsafe_code)]
#[unsafe(no_mangle)]
pub extern "C" fn srpc_rand_raw() -> i32 {
    RAW_DRAWS.fetch_add(1, Ordering::SeqCst);
    RAW_VALUE.load(Ordering::SeqCst)
}

#[allow(unsafe_code)]
#[unsafe(no_mangle)]
pub extern "C" fn srpc_rand_destroy() {}

fn install_raw(value: i32) {
    RAW_VALUE.store(value, Ordering::SeqCst);
    RAW_DRAWS.store(0, Ordering::SeqCst);
}

fn draws() -> usize {
    RAW_DRAWS.load(Ordering::SeqCst)
}

fn assert_send_sync_copy<T: Send + Sync + Copy>() {}

#[test]
fn layouts_discriminants_and_traits_match_the_flat_cpp_surface() {
    assert_send_sync_copy::<TimeoutType>();
    assert_send_sync_copy::<RequestOptions>();

    assert_eq!(size_of::<TimeoutType>(), 4);
    assert_eq!(align_of::<TimeoutType>(), 4);
    assert_eq!(TimeoutType::NONE as i32, 0);
    assert_eq!(TimeoutType::CONNECT_TIMEOUT as i32, 1);
    assert_eq!(TimeoutType::REQUEST_TIMEOUT as i32, 2);
    assert_eq!(TimeoutType::RESPONSE_TIMEOUT as i32, 3);
    assert_eq!(TimeoutType::TOTAL_TIMEOUT as i32, 4);

    assert_eq!(size_of::<RequestOptions>(), 32);
    assert_eq!(align_of::<RequestOptions>(), 8);
    assert_eq!(offset_of!(RequestOptions, timeout_ms), 0);
    assert_eq!(offset_of!(RequestOptions, total_timeout_ms), 8);
    assert_eq!(offset_of!(RequestOptions, max_retries), 16);
    assert_eq!(offset_of!(RequestOptions, base_delay_ms), 18);
    assert_eq!(offset_of!(RequestOptions, max_delay_ms), 20);
    assert_eq!(offset_of!(RequestOptions, jitter_factor), 24);
    assert_eq!(offset_of!(RequestOptions, idempotent), 28);
}

#[test]
fn factories_retry_and_timeout_boundaries_are_exact() {
    let defaults = RequestOptions::defaults();
    assert_eq!(defaults, RequestOptions::new());
    assert_eq!(defaults.timeout_ms, 1_000);
    assert_eq!(defaults.total_timeout_ms, 0);
    assert_eq!(defaults.max_retries, 0);
    assert_eq!(defaults.base_delay_ms, 50);
    assert_eq!(defaults.max_delay_ms, 5_000);
    assert_eq!(defaults.jitter_factor, 0.1);
    assert!(!defaults.idempotent);
    assert!(!defaults.can_retry(0));

    let retry = RequestOptions::with_retry(3, 2_000);
    assert_eq!(retry.timeout_ms, 2_000);
    assert_eq!(retry.max_retries, 3);
    assert!(retry.idempotent);
    assert!(retry.can_retry(0));
    assert!(retry.can_retry(2));
    assert!(!retry.can_retry(3));

    let idempotent = RequestOptions::idempotent_retry(10);
    assert_eq!(idempotent.timeout_ms, 1_000);
    assert_eq!(idempotent.max_retries, 10);
    assert!(idempotent.idempotent);

    assert_eq!(RequestOptions::no_timeout().timeout_ms, 0);
    assert_eq!(RequestOptions::fast().max_delay_ms, 100);
    assert_eq!(RequestOptions::patient().total_timeout_ms, 60_000);

    let mut limited = defaults;
    limited.total_timeout_ms = 5_000;
    assert!(!limited.is_total_timeout_exceeded(4_999));
    assert!(limited.is_total_timeout_exceeded(5_000));
    assert_eq!(limited.remaining_time_ms(0), 5_000);
    assert_eq!(limited.remaining_time_ms(4_999), 1);
    assert_eq!(limited.remaining_time_ms(5_000), 0);
    assert_eq!(limited.remaining_time_ms(u64::MAX), 0);
    assert_eq!(defaults.remaining_time_ms(u64::MAX), u64::MAX);
}

#[test]
fn backoff_caps_before_jitter_and_draw_counts_are_deterministic() {
    let _state_guard = RAND_STATE_TEST_GUARD
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());

    let mut options = RequestOptions::defaults();
    options.base_delay_ms = 100;
    options.max_delay_ms = 500;
    options.jitter_factor = 0.0;
    install_raw(17);
    assert_eq!(options.calculate_delay_ms(0), 100);
    assert_eq!(options.calculate_delay_ms(1), 200);
    assert_eq!(options.calculate_delay_ms(2), 400);
    assert_eq!(options.calculate_delay_ms(3), 500);
    assert_eq!(options.calculate_delay_ms(u16::MAX), 500);
    assert_eq!(draws(), 0);

    options.jitter_factor = -0.1;
    assert_eq!(options.calculate_delay_ms(0), 100);
    assert_eq!(draws(), 0);
    options.jitter_factor = f32::NAN;
    assert_eq!(options.calculate_delay_ms(0), 100);
    assert_eq!(draws(), 0);

    options.jitter_factor = 0.2;
    install_raw(0);
    let centered_low = (0f64 / (i32::MAX as f64)) - 0.5;
    let low = 100f64 + (100f64 * (options.jitter_factor as f64) * centered_low);
    assert_eq!(options.calculate_delay_ms(0), low as u64);
    assert_eq!(draws(), 1);

    install_raw(i32::MAX);
    let centered_high = (i32::MAX as f64 / i32::MAX as f64) - 0.5;
    let high = 100f64 + (100f64 * (options.jitter_factor as f64) * centered_high);
    assert_eq!(options.calculate_delay_ms(0), high as u64);
    assert_eq!(draws(), 1);

    options.base_delay_ms = 1_000;
    options.max_delay_ms = 500;
    install_raw(i32::MAX);
    let capped_then_jittered =
        500f64 + (500f64 * (options.jitter_factor as f64) * centered_high);
    assert_eq!(options.calculate_delay_ms(0), capped_then_jittered as u64);
    assert_eq!(draws(), 1);

    options.base_delay_ms = 0;
    install_raw(123);
    assert_eq!(options.calculate_delay_ms(u16::MAX), 0);
    assert_eq!(draws(), 1);

    options.base_delay_ms = 100;
    options.max_delay_ms = 500;
    options.jitter_factor = 10.0;
    install_raw(-i32::MAX);
    assert_eq!(options.calculate_delay_ms(0), 0);
    assert_eq!(draws(), 1);

    options.base_delay_ms = u16::MAX;
    options.max_delay_ms = u16::MAX;
    options.jitter_factor = f32::MAX;
    install_raw(i32::MAX);
    assert_eq!(options.calculate_delay_ms(0), u64::MAX);
    assert_eq!(draws(), 1);
}

#[test]
fn timeout_strings_cover_every_rust_constructible_variant() {
    assert_eq!(timeout_type_to_string(TimeoutType::NONE), "NONE");
    assert_eq!(
        timeout_type_to_string(TimeoutType::CONNECT_TIMEOUT),
        "CONNECT_TIMEOUT"
    );
    assert_eq!(
        timeout_type_to_string(TimeoutType::REQUEST_TIMEOUT),
        "REQUEST_TIMEOUT"
    );
    assert_eq!(
        timeout_type_to_string(TimeoutType::RESPONSE_TIMEOUT),
        "RESPONSE_TIMEOUT"
    );
    assert_eq!(
        timeout_type_to_string(TimeoutType::TOTAL_TIMEOUT),
        "TOTAL_TIMEOUT"
    );
}
