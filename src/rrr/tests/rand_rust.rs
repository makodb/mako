use std::panic::catch_unwind;
use std::sync::{
    atomic::{AtomicI32, AtomicUsize, Ordering},
    Mutex,
};

use rrr::rand::{
    randgen_destroy, randgen_nu_constant_now, randgen_rand_max, randgen_rand_raw, randgen_zero_pad,
    RandWeightVec, RandomGenerator,
};

static RAW_VALUE: AtomicI32 = AtomicI32::new(0);
static RAW_DRAWS: AtomicUsize = AtomicUsize::new(0);
static DESTROYS: AtomicUsize = AtomicUsize::new(0);
static RAND_STATE_TEST_GUARD: Mutex<()> = Mutex::new(());

#[allow(unsafe_code)]
#[unsafe(no_mangle)]
pub extern "C" fn srpc_rand_raw() -> i32 {
    RAW_DRAWS.fetch_add(1, Ordering::SeqCst);
    RAW_VALUE.load(Ordering::SeqCst)
}

#[allow(unsafe_code)]
#[unsafe(no_mangle)]
pub extern "C" fn srpc_rand_destroy() {
    DESTROYS.fetch_add(1, Ordering::SeqCst);
}

fn install_raw(value: i32) {
    RAW_VALUE.store(value, Ordering::SeqCst);
    RAW_DRAWS.store(0, Ordering::SeqCst);
}

fn draws() -> usize {
    RAW_DRAWS.load(Ordering::SeqCst)
}

#[test]
fn byte_string_adapters_preserve_padding_truncation_and_binary_payloads() {
    assert_eq!(randgen_zero_pad(Vec::new(), 0), b"");
    assert_eq!(randgen_zero_pad(Vec::new(), 3), b"000");
    assert_eq!(randgen_zero_pad(b"7".to_vec(), 3), b"007");
    assert_eq!(randgen_zero_pad(b"123".to_vec(), 3), b"123");
    assert_eq!(randgen_zero_pad(b"1234".to_vec(), 3), b"234");
    assert_eq!(randgen_zero_pad(b"1234".to_vec(), 0), b"");

    let binary = vec![0x00, 0x80, 0xff];
    assert_eq!(
        randgen_zero_pad(binary.clone(), 5),
        vec![b'0', b'0', 0x00, 0x80, 0xff]
    );
    assert_eq!(randgen_zero_pad(binary, 2), vec![0x80, 0xff]);
}

#[test]
fn integer_formatting_matches_the_legacy_decimal_byte_contract() {
    assert_eq!(RandomGenerator::int2str_n(0, 1), b"0");
    assert_eq!(RandomGenerator::int2str_n(42, 5), b"00042");
    assert_eq!(RandomGenerator::int2str_n(-7, 4), b"00-7");
    assert_eq!(RandomGenerator::int2str_n(12_345, 3), b"345");
    assert_eq!(RandomGenerator::int2str_n(-12_345, 4), b"2345");
    assert_eq!(RandomGenerator::int2str_n(i32::MAX, 10), b"2147483647");
    assert_eq!(RandomGenerator::int2str_n(i32::MIN, 11), b"-2147483648");
    assert_eq!(RandomGenerator::int2str_n(i32::MIN, 10), b"2147483648");
}

#[test]
fn raw_kernel_range_math_and_destroy_have_exact_call_counts() {
    let _state_guard = RAND_STATE_TEST_GUARD
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());

    assert_eq!(randgen_rand_max(), i32::MAX as f64);
    assert_eq!(randgen_nu_constant_now(), 0);

    install_raw(17);
    assert_eq!(randgen_rand_raw(), 17);
    assert_eq!(draws(), 1);

    install_raw(5);
    assert_eq!(RandomGenerator::rand(-10, -5), -5);
    assert_eq!(draws(), 1);

    install_raw(i32::MAX);
    assert_eq!(RandomGenerator::rand(7, 7), 7);
    assert_eq!(draws(), 1);

    install_raw(i32::MAX);
    assert_eq!(RandomGenerator::rand(i32::MIN, -1), -1);
    assert_eq!(draws(), 1);

    // The legacy default range computes (MAX - 0 + 1) in signed i32.
    // Make that wrap explicit and deterministic in both debug and release.
    install_raw(i32::MAX);
    assert_eq!(RandomGenerator::rand(0, i32::MAX), i32::MAX);
    assert_eq!(draws(), 1);

    install_raw(i32::MAX);
    assert!(catch_unwind(|| RandomGenerator::rand(i32::MIN, i32::MAX)).is_err());
    assert_eq!(draws(), 1);

    install_raw(11);
    assert!(catch_unwind(|| RandomGenerator::rand(9, 8)).is_err());
    assert_eq!(draws(), 0);

    install_raw(123);
    assert_eq!(RandomGenerator::rand_double(4.5, 4.5), 4.5);
    assert_eq!(draws(), 0);
    let scaled = RandomGenerator::rand_double(-1.0, 1.0);
    let expected = (123f64 / ((i32::MAX as f64) / 2.0)) - 1.0;
    assert_eq!(scaled, expected);
    assert_eq!(draws(), 1);

    install_raw(123);
    assert!(catch_unwind(|| RandomGenerator::rand_double(2.0, 1.0)).is_err());
    assert_eq!(draws(), 0);

    install_raw(123);
    assert!(catch_unwind(|| RandomGenerator::rand_double(0.0, f64::NAN)).is_err());
    assert_eq!(draws(), 0);

    install_raw(0);
    assert!(!RandomGenerator::percentage_true(0));
    assert_eq!(draws(), 1);
    install_raw(0);
    assert!(RandomGenerator::percentage_true(1));
    assert_eq!(draws(), 1);

    install_raw(5);
    assert_eq!(RandomGenerator::nu_rand(1_022, 0, 999), 5);
    assert_eq!(draws(), 2);

    install_raw(i32::MAX);
    assert!(catch_unwind(|| RandomGenerator::nu_rand(0, i32::MIN, i32::MAX)).is_err());
    assert_eq!(draws(), 2);

    let before = DESTROYS.load(Ordering::SeqCst);
    randgen_destroy();
    RandomGenerator::destroy();
    assert_eq!(DESTROYS.load(Ordering::SeqCst), before + 2);
}

#[test]
fn weighted_selection_preserves_boundaries_empty_sentinel_and_draw_counts() {
    let _state_guard = RAND_STATE_TEST_GUARD
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());

    let empty: RandWeightVec = Vec::new();
    install_raw(99);
    assert_eq!(RandomGenerator::weighted_select(&empty), u32::MAX);
    assert_eq!(draws(), 0);

    install_raw(99);
    assert_eq!(RandomGenerator::weighted_select(&[0.0, 0.0]), 0);
    assert_eq!(draws(), 0);

    let weights: RandWeightVec = vec![1.0, 2.0, 3.0];
    install_raw(0);
    assert_eq!(RandomGenerator::weighted_select(&weights), 0);
    assert_eq!(draws(), 1);

    install_raw(i32::MAX / 2);
    assert_eq!(RandomGenerator::weighted_select(&weights), 1);
    assert_eq!(draws(), 1);

    install_raw(i32::MAX);
    assert_eq!(RandomGenerator::weighted_select(&weights), 2);
    assert_eq!(draws(), 1);

    let positive_boundary = vec![1.0, (i32::MAX - 1) as f64];
    install_raw(1);
    assert_eq!(RandomGenerator::weighted_select(&positive_boundary), 0);
    assert_eq!(draws(), 1);
}
