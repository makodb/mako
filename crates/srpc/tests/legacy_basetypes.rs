use srpc::base::legacy_basetypes::{
    self, abort_if_false, i16 as LegacyI16, i32 as LegacyI32, i64 as LegacyI64, i8 as LegacyI8,
    Counter, SparseInt, Time, Timer, RRR_USEC_PER_SEC,
};
use std::sync::Arc;
use std::time::Duration;

fn assert_send_sync<T: Send + Sync>() {}
fn assert_copy<T: Copy>() {}

#[test]
fn public_aliases_layouts_and_native_traits_are_pinned() {
    assert_eq!(core::mem::size_of::<LegacyI8>(), 1_usize);
    assert_eq!(core::mem::size_of::<LegacyI16>(), 2_usize);
    assert_eq!(core::mem::size_of::<LegacyI32>(), 4_usize);
    assert_eq!(core::mem::size_of::<LegacyI64>(), 8_usize);

    let atomic = legacy_basetypes::AtomicI64::new(17_i64);
    assert_eq!(atomic.load(legacy_basetypes::Ordering::Relaxed), 17_i64);

    assert_eq!(core::mem::size_of::<SparseInt>(), 0_usize);
    assert_eq!(core::mem::align_of::<SparseInt>(), 1_usize);
    assert_eq!(core::mem::size_of::<legacy_basetypes::v32>(), 4_usize);
    assert_eq!(core::mem::align_of::<legacy_basetypes::v32>(), 4_usize);
    assert_eq!(core::mem::size_of::<legacy_basetypes::v64>(), 8_usize);
    assert_eq!(core::mem::align_of::<legacy_basetypes::v64>(), 8_usize);
    assert_eq!(core::mem::size_of::<Counter>(), 8_usize);
    assert_eq!(core::mem::align_of::<Counter>(), 8_usize);
    assert_eq!(core::mem::size_of::<Time>(), 0_usize);
    assert_eq!(core::mem::align_of::<Time>(), 1_usize);
    assert_eq!(core::mem::size_of::<Timer>(), 16_usize);
    assert_eq!(core::mem::align_of::<Timer>(), 8_usize);

    assert_copy::<SparseInt>();
    assert_copy::<legacy_basetypes::v32>();
    assert_copy::<legacy_basetypes::v64>();
    assert_copy::<Time>();
    assert_copy::<Timer>();
    assert_send_sync::<SparseInt>();
    assert_send_sync::<legacy_basetypes::v32>();
    assert_send_sync::<legacy_basetypes::v64>();
    assert_send_sync::<Counter>();
    assert_send_sync::<Time>();
    assert_send_sync::<Timer>();
}

#[test]
fn sparseint_i32_boundaries_roundtrip_and_tags_agree() {
    let values: [i32; 19] = [
        0_i32,
        1_i32,
        -1_i32,
        63_i32,
        -64_i32,
        64_i32,
        -65_i32,
        8_191_i32,
        -8_192_i32,
        8_192_i32,
        -8_193_i32,
        1_048_575_i32,
        -1_048_576_i32,
        1_048_576_i32,
        -1_048_577_i32,
        134_217_727_i32,
        -134_217_728_i32,
        i32::MAX,
        i32::MIN,
    ];

    for value in values {
        let mut bytes = [0_u8; 9];
        let size = SparseInt::dump32(value, bytes.as_mut_ptr());
        assert_eq!(size, SparseInt::val_size(value as i64), "size for {value}");
        assert_eq!(size, SparseInt::buf_size(bytes[0]), "tag for {value}");
        assert_eq!(SparseInt::load32(bytes.as_ptr()), value, "value {value}");
    }
}

#[test]
fn sparseint_i64_boundaries_roundtrip_and_keep_the_size_eight_quirk() {
    let values: [i64; 35] = [
        0_i64,
        1_i64,
        -1_i64,
        63_i64,
        -64_i64,
        64_i64,
        -65_i64,
        8_191_i64,
        -8_192_i64,
        8_192_i64,
        -8_193_i64,
        1_048_575_i64,
        -1_048_576_i64,
        1_048_576_i64,
        -1_048_577_i64,
        134_217_727_i64,
        -134_217_728_i64,
        134_217_728_i64,
        -134_217_729_i64,
        17_179_869_183_i64,
        -17_179_869_184_i64,
        17_179_869_184_i64,
        -17_179_869_185_i64,
        2_199_023_255_551_i64,
        -2_199_023_255_552_i64,
        2_199_023_255_552_i64,
        -2_199_023_255_553_i64,
        281_474_976_710_655_i64,
        -281_474_976_710_656_i64,
        281_474_976_710_656_i64,
        -281_474_976_710_657_i64,
        36_028_797_018_963_967_i64,
        -36_028_797_018_963_968_i64,
        i64::MAX,
        i64::MIN,
    ];

    for value in values {
        let mut bytes = [0_u8; 9];
        let size = SparseInt::dump64(value, bytes.as_mut_ptr());
        assert_eq!(size, SparseInt::val_size(value), "size for {value}");
        assert_eq!(size, SparseInt::buf_size(bytes[0]), "tag for {value}");
        assert_eq!(SparseInt::load64(bytes.as_ptr()), value, "value {value}");
    }

    let mut positive = [0xA5_u8; 9];
    let positive_size = SparseInt::dump64(36_028_797_018_963_967_i64, positive.as_mut_ptr());
    assert_eq!(positive_size, 8_usize);
    assert_eq!(positive[0], 0xFE_u8);
    assert_eq!(
        positive[8], 0xFF_u8,
        "the ninth byte is written despite size 8"
    );

    // Reproduce the archive's zero-padded decode scratch: only the reported
    // eight bytes reach the wire, so the final payload byte decodes as zero.
    let mut wire_scratch = [0_u8; 9];
    wire_scratch[..positive_size].copy_from_slice(&positive[..positive_size]);
    assert_eq!(
        SparseInt::load64(wire_scratch.as_ptr()),
        36_028_797_018_963_712_i64
    );
}

#[test]
fn sparseint_golden_prefixes_are_byte_exact() {
    let cases: [(i64, &[u8]); 7] = [
        (0_i64, &[0x00_u8]),
        (1_i64, &[0x01_u8]),
        (-1_i64, &[0x7F_u8]),
        (63_i64, &[0x3F_u8]),
        (-64_i64, &[0x40_u8]),
        (64_i64, &[0x80_u8, 0x40_u8]),
        (-65_i64, &[0xBF_u8, 0xBF_u8]),
    ];

    for (value, expected) in cases {
        let mut bytes = [0_u8; 9];
        let size = SparseInt::dump64(value, bytes.as_mut_ptr());
        assert_eq!(size, expected.len());
        assert_eq!(&bytes[..size], expected, "bytes for {value}");
    }
}

#[test]
fn v32_v64_keep_value_construction_mutation_and_encoded_sizes() {
    let mut small = legacy_basetypes::v32::new(63_i32);
    assert_eq!(small.get(), 63_i32);
    assert_eq!(small.val_size(), 1_usize);
    small.set(64_i32);
    assert_eq!(small.get(), 64_i32);
    assert_eq!(small.val_size(), 2_usize);
    assert_eq!(legacy_basetypes::v32::default().get(), 0_i32);

    let mut wide = legacy_basetypes::v64::new(281_474_976_710_656_i64);
    assert_eq!(wide.get(), 281_474_976_710_656_i64);
    assert_eq!(wide.val_size(), 8_usize);
    wide.set(i64::MAX);
    assert_eq!(wide.get(), i64::MAX);
    assert_eq!(wide.val_size(), 9_usize);
    assert_eq!(legacy_basetypes::v64::default().get(), 0_i64);
}

#[test]
fn counter_preserves_fetch_add_return_value_reset_and_concurrency() {
    let counter = Counter::new(7_i64);
    assert_eq!(counter.peek_next(), 7_i64);
    assert_eq!(counter.next(3_i64), 7_i64);
    assert_eq!(counter.peek_next(), 10_i64);
    counter.reset(-2_i64);
    assert_eq!(counter.next(1_i64), -2_i64);

    let shared = Arc::new(Counter::new(0_i64));
    let mut workers = Vec::new();
    for _ in 0_usize..4_usize {
        let worker_counter = Arc::clone(&shared);
        workers.push(std::thread::spawn(move || {
            for _ in 0_usize..2_000_usize {
                worker_counter.next(1_i64);
            }
        }));
    }
    for worker in workers {
        worker.join().unwrap();
    }
    assert_eq!(shared.peek_next(), 8_000_i64);
}

#[test]
fn clock_constant_sleep_and_timer_state_machine_match_the_live_contract() {
    assert_eq!(RRR_USEC_PER_SEC, 1_000_000_u64);
    abort_if_false(true);

    let coarse = Time::now(false);
    assert!(coarse > 1_577_836_800_000_000_u64);
    let monotonic_before = Time::now(true);
    Time::sleep(2_000_u64);
    let monotonic_after = Time::now(true);
    assert!(monotonic_after >= monotonic_before + 1_000_u64);

    let mut timer = Timer::new();
    assert_eq!(timer.begin_us, 0_u64);
    assert_eq!(timer.end_us, 0_u64);
    timer.start();
    assert_ne!(timer.begin_us, 0_u64);
    assert_eq!(timer.end_us, 0_u64);
    std::thread::sleep(Duration::from_millis(2));
    let live = timer.elapsed();
    assert!(live >= 0.001_f64, "live elapsed={live}");
    timer.stop();
    assert_ne!(timer.end_us, 0_u64);
    let stopped = timer.elapsed();
    std::thread::sleep(Duration::from_millis(2));
    assert_eq!(timer.elapsed(), stopped, "a stopped timer must stay frozen");
    timer.reset();
    assert_eq!(timer, Timer::default());
}

#[test]
fn false_precondition_aborts_instead_of_panicking_or_returning() {
    const CHILD_ENV: &str = "SRPC_BASETYPES_ABORT_CHILD";
    if std::env::var_os(CHILD_ENV).is_some() {
        abort_if_false(false);
    }

    let status = std::process::Command::new(std::env::current_exe().unwrap())
        .arg("--exact")
        .arg("false_precondition_aborts_instead_of_panicking_or_returning")
        .arg("--nocapture")
        .env(CHILD_ENV, "1")
        .status()
        .unwrap();
    assert!(
        !status.success(),
        "false precondition unexpectedly returned"
    );
}

#[test]
fn historical_deletions_and_current_public_names_are_explicit() {
    let owner = include_str!("../src/base/legacy_basetypes.rs");
    for required in [
        "pub type i8",
        "pub type i16",
        "pub type i32",
        "pub type i64",
        "pub struct SparseInt",
        "pub struct v32",
        "pub struct v64",
        "pub struct Counter",
        "pub const RRR_USEC_PER_SEC",
        "pub fn abort_if_false",
        "pub fn time_now_us",
        "pub struct Time",
        "pub struct Timer",
    ] {
        assert!(
            owner.contains(required),
            "missing public surface: {required}"
        );
    }
    for retired in [
        "pub struct NoCopy",
        "pub struct Rand",
        "pub struct Enumerator",
        "pub struct MergedEnumerator",
        "pub fn atomic_store_relaxed",
        "pub fn atomic_load_relaxed",
        "pub fn atomic_fetch_add_acq_rel",
        "pub fn atomic_fetch_sub_acq_rel",
    ] {
        assert!(
            !owner.contains(retired),
            "retired API resurfaced: {retired}"
        );
    }
}
