#[allow(dead_code)]
#[path = "../src/base/legacy_rand.rs"]
mod legacy_rand;

use legacy_rand::RandomGenerator;
use std::collections::VecDeque;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Mutex;

const RAND_MAX: i32 = 2_147_483_647_i32;

static RAW_VALUES: Mutex<VecDeque<i32>> = Mutex::new(VecDeque::new());
static RAW_CALLS: AtomicU32 = AtomicU32::new(0_u32);
static DESTROY_CALLS: AtomicU32 = AtomicU32::new(0_u32);
static TEST_LOCK: Mutex<()> = Mutex::new(());

#[allow(unsafe_code)]
#[no_mangle]
pub extern "C" fn srpc_rand_raw() -> i32 {
    RAW_CALLS.fetch_add(1_u32, Ordering::Relaxed);
    RAW_VALUES
        .lock()
        .unwrap()
        .pop_front()
        .expect("test exhausted the pinned rand_r stream")
}

#[allow(unsafe_code)]
#[no_mangle]
pub extern "C" fn srpc_rand_destroy() {
    DESTROY_CALLS.fetch_add(1_u32, Ordering::Relaxed);
}

fn seed_raw(values: &[i32]) {
    let mut stream = RAW_VALUES.lock().unwrap();
    stream.clear();
    stream.extend(values.iter().copied());
    RAW_CALLS.store(0_u32, Ordering::Relaxed);
}

fn raw_calls() -> u32 {
    RAW_CALLS.load(Ordering::Relaxed)
}

#[test]
fn empty_owner_and_current_method_surface_are_intentional() {
    assert_eq!(core::mem::size_of::<RandomGenerator>(), 0);
    assert_eq!(core::mem::align_of::<RandomGenerator>(), 1);

    let owner = include_str!("../src/base/legacy_rand.rs");
    assert!(!owner.contains("pub fn rand_str"));
    assert_eq!(owner.matches("pub fn percentage_true").count(), 1);
    assert!(!owner.contains("pub fn rand()"));
    assert!(!owner.contains("pub fn rand_double()"));
}

#[test]
fn integer_draws_consume_the_raw_stream_once_and_keep_inclusive_ranges() {
    let _guard = TEST_LOCK.lock().unwrap();
    seed_raw(&[0_i32, 1_i32, RAND_MAX]);

    assert_eq!(RandomGenerator::rand(5_i32, 8_i32), 5_i32);
    assert_eq!(RandomGenerator::rand(5_i32, 8_i32), 6_i32);
    assert_eq!(RandomGenerator::rand(0_i32, RAND_MAX), RAND_MAX);
    assert_eq!(raw_calls(), 3_u32);
}

#[test]
fn floating_draws_preserve_endpoints_and_degenerate_range_consumption() {
    let _guard = TEST_LOCK.lock().unwrap();
    seed_raw(&[0_i32, RAND_MAX]);

    assert_eq!(RandomGenerator::rand_double(-2.0_f64, 4.0_f64), -2.0_f64);
    assert_eq!(RandomGenerator::rand_double(-2.0_f64, 4.0_f64), 4.0_f64);
    assert_eq!(RandomGenerator::rand_double(7.5_f64, 7.5_f64), 7.5_f64);
    assert_eq!(raw_calls(), 2_u32);
}

#[test]
fn fixed_width_decimal_formatting_matches_legacy_string_surgery() {
    assert_eq!(RandomGenerator::int2str_n(42_i32, 5_i32), "00042");
    assert_eq!(RandomGenerator::int2str_n(12_345_i32, 3_i32), "345");
    assert_eq!(RandomGenerator::int2str_n(-123_i32, 3_i32), "123");
    assert_eq!(RandomGenerator::int2str_n(-7_i32, 3_i32), "0-7");
    assert_eq!(RandomGenerator::int2str_n(999_i32, 0_i32), "");
}

#[test]
#[should_panic(expected = "int2str_n length must be nonnegative")]
fn negative_format_width_is_outside_the_public_domain() {
    let _ = RandomGenerator::int2str_n(42_i32, -1_i32);
}

#[test]
fn percentage_and_nurand_use_the_same_ordered_raw_draws() {
    let _guard = TEST_LOCK.lock().unwrap();
    seed_raw(&[0_i32, 99_i32, 100_i32, 2_500_i32]);

    assert!(RandomGenerator::percentage_true(1_i32));
    assert!(!RandomGenerator::percentage_true(99_i32));
    assert_eq!(RandomGenerator::nu_rand(255_i32, 0_i32, 999_i32), 500_i32);
    assert_eq!(raw_calls(), 4_u32);
}

#[test]
fn weighted_selection_keeps_boundary_and_empty_vector_behavior() {
    let _guard = TEST_LOCK.lock().unwrap();
    let weights = vec![1.0_f64, 2.0_f64, 3.0_f64];
    seed_raw(&[0_i32, RAND_MAX, RAND_MAX / 3_i32]);

    assert_eq!(RandomGenerator::weighted_select(&weights), 0_u32);
    assert_eq!(RandomGenerator::weighted_select(&weights), 2_u32);
    assert_eq!(RandomGenerator::weighted_select(&weights), 1_u32);
    assert_eq!(raw_calls(), 3_u32);

    let empty = Vec::<f64>::new();
    assert_eq!(RandomGenerator::weighted_select(&empty), u32::MAX);
    assert_eq!(
        raw_calls(),
        3_u32,
        "an empty weight vector consumes no draw"
    );
}

#[test]
fn destroy_delegates_to_the_existing_c_kernel() {
    let _guard = TEST_LOCK.lock().unwrap();
    DESTROY_CALLS.store(0_u32, Ordering::Relaxed);
    RandomGenerator::destroy();
    assert_eq!(DESTROY_CALLS.load(Ordering::Relaxed), 1_u32);
}
