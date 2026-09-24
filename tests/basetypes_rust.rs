use std::mem::{align_of, offset_of, size_of};

use srpc::basetypes::{
    abort_if_false, i16, i32, i64, i8, time_now_us, v32, v64, Counter, SparseInt, Time, Timer,
    SRPC_USEC_PER_SEC,
};

#[test]
fn aliases_layouts_and_traits_match_the_cpp_surface() {
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<SparseInt>();
    assert_send_sync::<Counter>();
    assert_send_sync::<Timer>();

    assert_eq!(size_of::<i8>(), size_of::<::core::primitive::i8>());
    assert_eq!(size_of::<i16>(), size_of::<::core::primitive::i16>());
    assert_eq!(size_of::<i32>(), size_of::<::core::primitive::i32>());
    assert_eq!(size_of::<i64>(), size_of::<::core::primitive::i64>());
    assert_eq!(size_of::<SparseInt>(), 0);
    assert_eq!(align_of::<SparseInt>(), 1);
    assert_eq!(size_of::<v32>(), 4);
    assert_eq!(align_of::<v32>(), 4);
    assert_eq!(offset_of!(v32, val_field), 0);
    assert_eq!(size_of::<v64>(), 8);
    assert_eq!(align_of::<v64>(), 8);
    assert_eq!(offset_of!(v64, val_field), 0);
    assert_eq!(size_of::<Counter>(), 8);
    assert_eq!(align_of::<Counter>(), 8);
    assert_eq!(offset_of!(Counter, next_field), 0);
    assert_eq!(size_of::<Timer>(), 16);
    assert_eq!(align_of::<Timer>(), 8);
    assert_eq!(offset_of!(Timer, begin_us), 0);
    assert_eq!(offset_of!(Timer, end_us), 8);
}

#[allow(unsafe_code)]
fn round_trip_i64(value: i64) {
    let sentinel = (value as u8) ^ 0xff;
    let mut encoded = [sentinel; 12];
    // SAFETY: `encoded` supplies twelve writable bytes, exceeding the legacy
    // encoder's maximum nine-byte write, and remains alive for the decode.
    let size = unsafe { SparseInt::dump64(value, encoded.as_mut_ptr()) };
    assert_eq!(size, SparseInt::val_size(value));
    assert_eq!(SparseInt::buf_size(encoded[0]), size);
    // SAFETY: the preceding encoder produced a complete value in `encoded`.
    assert_eq!(unsafe { SparseInt::load64(encoded.as_ptr()) }, value);
    if size == 8 {
        assert_ne!(encoded[8], sentinel, "legacy case writes a ninth byte");
        assert_eq!(encoded[9], sentinel);
    } else {
        assert_eq!(encoded[size], sentinel);
    }
}

#[allow(unsafe_code)]
fn round_trip_i32(value: i32) {
    let sentinel = (value as u8) ^ 0xff;
    let mut encoded = [sentinel; 8];
    // SAFETY: `encoded` supplies eight writable bytes, exceeding the
    // encoder's maximum five-byte write, and remains alive for the decode.
    let size = unsafe { SparseInt::dump32(value, encoded.as_mut_ptr()) };
    assert_eq!(size, SparseInt::val_size(value as i64));
    assert_eq!(SparseInt::buf_size(encoded[0]), size);
    // SAFETY: the preceding encoder produced a complete value in `encoded`.
    assert_eq!(unsafe { SparseInt::load32(encoded.as_ptr()) }, value);
    assert_eq!(encoded[size], sentinel);
}

fn fnv1a_byte(hash: u64, byte: u8) -> u64 {
    (hash ^ u64::from(byte)).wrapping_mul(0x0000_0100_0000_01b3)
}

#[allow(unsafe_code)]
fn hash_i64_wire_record(mut hash: u64, value: i64) -> u64 {
    let mut encoded = [0u8; 9];
    // SAFETY: `encoded` is writable for the encoder's maximum nine bytes.
    let reported = unsafe { SparseInt::dump64(value, encoded.as_mut_ptr()) };
    hash = fnv1a_byte(hash, 64);
    hash = fnv1a_byte(hash, reported as u8);
    let written = if reported == 8 { 9 } else { reported };
    for byte in &encoded[..written] {
        hash = fnv1a_byte(hash, *byte);
    }
    hash
}

#[allow(unsafe_code)]
fn hash_i32_wire_record(mut hash: u64, value: i32) -> u64 {
    let mut encoded = [0u8; 5];
    // SAFETY: `encoded` is writable for the encoder's maximum five bytes.
    let reported = unsafe { SparseInt::dump32(value, encoded.as_mut_ptr()) };
    hash = fnv1a_byte(hash, 32);
    hash = fnv1a_byte(hash, reported as u8);
    for byte in &encoded[..reported] {
        hash = fnv1a_byte(hash, *byte);
    }
    hash
}

#[test]
#[allow(unsafe_code)]
fn sparse_int_boundaries_and_deterministic_wire_corpus() {
    let boundaries = [
        i64::MIN,
        -36_028_797_018_963_969,
        -36_028_797_018_963_968,
        -281_474_976_710_657,
        -281_474_976_710_656,
        -2_199_023_255_553,
        -2_199_023_255_552,
        -17_179_869_185,
        -17_179_869_184,
        -134_217_729,
        -134_217_728,
        -1_048_577,
        -1_048_576,
        -8_193,
        -8_192,
        -65,
        -64,
        -1,
        0,
        1,
        63,
        64,
        8_191,
        8_192,
        1_048_575,
        1_048_576,
        134_217_727,
        134_217_728,
        17_179_869_183,
        17_179_869_184,
        2_199_023_255_551,
        2_199_023_255_552,
        281_474_976_710_655,
        281_474_976_710_656,
        36_028_797_018_963_967,
        36_028_797_018_963_968,
        i64::MAX,
    ];
    for value in boundaries {
        round_trip_i64(value);
        if let Ok(value32) = i32::try_from(value) {
            round_trip_i32(value32);
        }
    }

    let mut state = 0x9e37_79b9_7f4a_7c15_u64;
    let mut wire_digest = 0xcbf2_9ce4_8422_2325_u64;
    for _ in 0..100_000 {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        let value = state.wrapping_mul(0x2545_f491_4f6c_dd1d) as i64;
        round_trip_i64(value);
        round_trip_i32(value as i32);
        wire_digest = hash_i64_wire_record(wire_digest, value);
        wire_digest = hash_i32_wire_record(wire_digest, value as i32);
    }
    // FNV-1a digest over the reported length and every byte written. Updated
    // for the length-8 fix (item 4.1): values past the 7-byte range now report
    // 9 and carry the 0xFF marker instead of the old lossy 0xFE/8.
    assert_eq!(wire_digest, 0xbbe9_520e_79d2_1a9c);

    // The former length-eight quirk is fixed (item 4.1): values past the
    // seven-byte range now report nine and carry the 0xFF marker, so a caller
    // that persists exactly the reported count round-trips losslessly. (The
    // decoder still reads the historical 0xFE form for old data; nothing
    // writes it any more.)
    unsafe fn archive_round_trip(value: i64) -> (i64, usize, u8) {
        let mut encoded = [0u8; 9];
        let reported = unsafe { SparseInt::dump64(value, encoded.as_mut_ptr()) };
        let marker = encoded[0];
        let mut persisted = [0u8; 9];
        persisted[..reported].copy_from_slice(&encoded[..reported]);
        (unsafe { SparseInt::load64(persisted.as_ptr()) }, reported, marker)
    }
    // The exact value that used to decode as ...712 now round-trips.
    assert_eq!(
        unsafe { archive_round_trip(36_028_797_018_963_967) },
        (36_028_797_018_963_967, 9, 0xff)
    );
    assert_eq!(
        unsafe { archive_round_trip(-36_028_797_018_963_967) },
        (-36_028_797_018_963_967, 9, 0xff)
    );
}

#[test]
fn values_counter_and_time_facades_preserve_behavior() {
    let mut x = v32::new(-8_192);
    assert_eq!(x.get(), -8_192);
    assert_eq!(x.val_size(), 2);
    x.set(8_192);
    assert_eq!(x.val_size(), 3);

    let mut y = v64::new(1);
    y.set(36_028_797_018_963_968);
    assert_eq!(y.get(), 36_028_797_018_963_968);
    assert_eq!(y.val_size(), 9);

    let counter = Counter::new(7);
    assert_eq!(counter.peek_next(), 7);
    assert_eq!(counter.next(5), 7);
    assert_eq!(counter.peek_next(), 12);
    counter.reset(-3);
    assert_eq!(counter.peek_next(), -3);
    counter.reset(i64::MAX);
    assert_eq!(counter.next(1), i64::MAX);
    assert_eq!(counter.peek_next(), i64::MIN);

    let shared = std::sync::Arc::new(Counter::new(0));
    let workers: Vec<_> = (0..8)
        .map(|_| {
            let shared = std::sync::Arc::clone(&shared);
            std::thread::spawn(move || {
                for _ in 0..10_000 {
                    shared.next(1);
                }
            })
        })
        .collect();
    for worker in workers {
        worker.join().unwrap();
    }
    assert_eq!(shared.peek_next(), 80_000);

    assert_eq!(SRPC_USEC_PER_SEC, 1_000_000);
    abort_if_false(true);
    let monotonic_before = time_now_us(true);
    assert!(monotonic_before > 0);
    assert!(Time::now(false) > 1_000_000_000_000);
    let sleep_started = std::time::Instant::now();
    Time::sleep(2_000);
    assert!(sleep_started.elapsed() >= std::time::Duration::from_millis(2));
    assert!(time_now_us(true) >= monotonic_before + 2_000);

    let mut timer = Timer::new();
    timer.start();
    assert!(timer.begin_us > 1_000_000_000_000);
    Time::sleep(2_000);
    assert!(timer.elapsed() >= 0.002);
    timer.stop();
    let stopped = timer.elapsed();
    Time::sleep(1_000);
    assert_eq!(timer.elapsed(), stopped);
    timer.begin_us = 3_000_000;
    timer.end_us = 5_250_000;
    assert_eq!(timer.elapsed(), 2.25);
    timer.begin_us = 10;
    timer.end_us = 5;
    assert_eq!(
        timer.elapsed(),
        (5_u64.wrapping_sub(10) as f64) / 1_000_000.0
    );
    timer.reset();
    assert_eq!((timer.begin_us, timer.end_us), (0, 0));
}
