use std::mem::{align_of, offset_of, size_of};
use std::sync::{
    atomic::{AtomicI32, AtomicUsize, Ordering},
    Mutex,
};

use rrr::reconnect_policy::{ReconnectCalculator, ReconnectPolicy};

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
fn assert_send<T: Send>() {}

#[test]
fn repr_c_layouts_and_thread_traits_match_the_cpp_surface() {
    assert_send_sync_copy::<ReconnectPolicy>();
    assert_send::<ReconnectCalculator<'static>>();

    // Compile-time negative assertion: if ReconnectCalculator ever becomes
    // Sync, both implementations match and type inference is ambiguous.
    trait AmbiguousIfSync<Marker> {
        fn marker() {}
    }
    impl<T: ?Sized> AmbiguousIfSync<()> for T {}
    impl<T: ?Sized + Sync> AmbiguousIfSync<u8> for T {}
    let _ = <ReconnectCalculator<'static> as AmbiguousIfSync<_>>::marker;

    assert_eq!(size_of::<ReconnectPolicy>(), 32);
    assert_eq!(align_of::<ReconnectPolicy>(), 8);
    assert_eq!(offset_of!(ReconnectPolicy, auto_reconnect), 0);
    assert_eq!(offset_of!(ReconnectPolicy, max_retries), 4);
    assert_eq!(offset_of!(ReconnectPolicy, initial_delay_ms), 8);
    assert_eq!(offset_of!(ReconnectPolicy, max_delay_ms), 12);
    assert_eq!(offset_of!(ReconnectPolicy, backoff_multiplier), 16);
    assert_eq!(offset_of!(ReconnectPolicy, jitter_enabled), 24);

    assert_eq!(size_of::<ReconnectCalculator<'static>>(), 16);
    assert_eq!(align_of::<ReconnectCalculator<'static>>(), 8);
    assert_eq!(offset_of!(ReconnectCalculator<'static>, policy), 0);
    assert_eq!(offset_of!(ReconnectCalculator<'static>, retries), 8);
}

#[test]
fn factories_preserve_every_legacy_policy_value() {
    let defaults = ReconnectPolicy::new();
    assert!(defaults.auto_reconnect);
    assert_eq!(defaults.max_retries, 5);
    assert_eq!(defaults.initial_delay_ms, 1_000);
    assert_eq!(defaults.max_delay_ms, 30_000);
    assert_eq!(defaults.backoff_multiplier, 2.0);
    assert!(defaults.jitter_enabled);
    assert_eq!(ReconnectPolicy::conservative(), defaults);

    let aggressive = ReconnectPolicy::aggressive();
    assert!(aggressive.auto_reconnect);
    assert_eq!(aggressive.max_retries, 0);
    assert_eq!(aggressive.initial_delay_ms, 100);
    assert_eq!(aggressive.max_delay_ms, 5_000);
    assert_eq!(aggressive.backoff_multiplier, 1.5);
    assert!(aggressive.jitter_enabled);

    let no_retry = ReconnectPolicy::no_retry();
    assert!(!no_retry.auto_reconnect);
    assert_eq!(no_retry.max_retries, 0);
    assert_eq!(no_retry.initial_delay_ms, 0);
    assert_eq!(no_retry.max_delay_ms, 0);
    assert_eq!(no_retry.backoff_multiplier, 1.0);
    assert!(!no_retry.jitter_enabled);
}

#[test]
fn limited_disabled_and_unlimited_retry_boundaries_are_distinct() {
    let mut limited = ReconnectPolicy::new();
    limited.max_retries = 3;
    limited.jitter_enabled = false;
    let calculator = ReconnectCalculator::new(&limited);

    assert_eq!(calculator.retry_count(), 0);
    assert!(calculator.should_retry());
    assert!(!calculator.retries_exhausted());
    assert_eq!(calculator.next_delay_ms(), 1_000);
    assert!(calculator.should_retry());
    assert_eq!(calculator.next_delay_ms(), 2_000);
    assert!(calculator.should_retry());
    assert_eq!(calculator.next_delay_ms(), 4_000);
    assert_eq!(calculator.retry_count(), 3);
    assert!(!calculator.should_retry());
    assert!(calculator.retries_exhausted());

    calculator.reset();
    assert_eq!(calculator.retry_count(), 0);
    assert!(calculator.should_retry());
    assert!(!calculator.retries_exhausted());

    let disabled = ReconnectPolicy::no_retry();
    let disabled_calculator = ReconnectCalculator::new(&disabled);
    assert!(!disabled_calculator.should_retry());
    assert!(disabled_calculator.retries_exhausted());

    let mut unlimited = ReconnectPolicy::aggressive();
    unlimited.jitter_enabled = false;
    let unlimited_calculator = ReconnectCalculator::new(&unlimited);
    for _ in 0..100 {
        assert!(unlimited_calculator.should_retry());
        unlimited_calculator.next_delay_ms();
    }
    assert_eq!(unlimited_calculator.retry_count(), 100);
    assert!(unlimited_calculator.should_retry());
    assert!(!unlimited_calculator.retries_exhausted());
}

#[test]
fn exponential_backoff_caps_and_peek_does_not_advance_state() {
    let _state_guard = RAND_STATE_TEST_GUARD
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());

    let mut policy = ReconnectPolicy::new();
    policy.initial_delay_ms = 100;
    policy.max_delay_ms = 500;
    policy.backoff_multiplier = 2.0;
    policy.jitter_enabled = false;
    let calculator = ReconnectCalculator::new(&policy);
    install_raw(17);

    assert_eq!(calculator.peek_delay_ms(), 100);
    assert_eq!(calculator.peek_delay_ms(), 100);
    assert_eq!(calculator.retry_count(), 0);
    assert_eq!(calculator.next_delay_ms(), 100);
    assert_eq!(calculator.peek_delay_ms(), 200);
    assert_eq!(calculator.next_delay_ms(), 200);
    assert_eq!(calculator.next_delay_ms(), 400);
    assert_eq!(calculator.next_delay_ms(), 500);
    assert_eq!(calculator.next_delay_ms(), 500);
    assert_eq!(calculator.retry_count(), 5);
    assert_eq!(draws(), 0);
}

#[test]
fn jitter_uses_one_raw_draw_and_the_legacy_endpoints() {
    let _state_guard = RAND_STATE_TEST_GUARD
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());

    let mut policy = ReconnectPolicy::new();
    policy.initial_delay_ms = 1_000;
    policy.max_delay_ms = 10_000;
    policy.backoff_multiplier = 1.0;
    policy.jitter_enabled = true;

    install_raw(0);
    let low = ReconnectCalculator::new(&policy);
    assert_eq!(low.next_delay_ms(), 500);
    assert_eq!(draws(), 1);

    install_raw(i32::MAX);
    let high = ReconnectCalculator::new(&policy);
    assert_eq!(high.next_delay_ms(), 1_500);
    assert_eq!(draws(), 1);

    // The exponential result is capped first, then jitter is applied. The
    // legacy upper endpoint can therefore exceed max_delay_ms.
    policy.max_delay_ms = 500;
    install_raw(i32::MAX);
    let capped_then_jittered = ReconnectCalculator::new(&policy);
    assert_eq!(capped_then_jittered.next_delay_ms(), 750);
    assert_eq!(draws(), 1);

    policy.initial_delay_ms = 0;
    install_raw(123);
    let zero = ReconnectCalculator::new(&policy);
    assert_eq!(zero.next_delay_ms(), 0);
    assert_eq!(zero.retry_count(), 1);
    assert_eq!(draws(), 0);
}

#[test]
fn retry_counter_wraps_exactly_like_the_unsigned_cpp_cell() {
    let mut policy = ReconnectPolicy::new();
    policy.initial_delay_ms = 100;
    policy.max_delay_ms = 100;
    policy.backoff_multiplier = 1.0;
    policy.jitter_enabled = false;
    policy.max_retries = 0;
    let calculator = ReconnectCalculator::new(&policy);

    calculator.retries.set(u32::MAX);
    assert_eq!(calculator.retry_count(), u32::MAX);
    assert_eq!(calculator.next_delay_ms(), 100);
    assert_eq!(calculator.retry_count(), 0);
    assert!(calculator.should_retry());
    assert!(!calculator.retries_exhausted());
}
