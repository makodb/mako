#![allow(unsafe_code)]

#[allow(dead_code)]
#[path = "../src/base/legacy_logging.rs"]
mod legacy_logging;

use legacy_logging::{log_basename, log_level_tag, log_time_now, Log};
use std::sync::Mutex;

static LEVEL_TEST: Mutex<()> = Mutex::new(());

fn assert_not_sync<T: ?Sized>() {
    trait AmbiguousIfSync<Marker> {
        fn marker() {}
    }
    impl<T: ?Sized> AmbiguousIfSync<()> for T {}
    struct Invalid;
    impl<T: ?Sized + Sync> AmbiguousIfSync<Invalid> for T {}

    let _ = <T as AmbiguousIfSync<_>>::marker;
}

#[unsafe(no_mangle)]
unsafe extern "C" fn srpc_path_basename(
    path: *const core::ffi::c_char,
) -> *const core::ffi::c_char {
    if path.is_null() {
        return core::ptr::null();
    }
    let mut last = path;
    let mut cursor = path;
    while unsafe { *cursor } != 0 {
        if unsafe { *cursor } == b'/' as core::ffi::c_char {
            last = unsafe { cursor.add(1_usize) };
        }
        cursor = unsafe { cursor.add(1_usize) };
    }
    last
}

#[unsafe(no_mangle)]
unsafe extern "C" fn srpc_time_now_str(now: *mut core::ffi::c_char) {
    const FIXED: &[u8; 23] = b"2026-08-09 03:04:05.006";
    let mut index = 0_usize;
    while index < FIXED.len() {
        unsafe { *now.add(index) = FIXED[index] as core::ffi::c_char };
        index += 1_usize;
    }
    unsafe { *now.add(FIXED.len()) = 0 };
}

#[test]
fn level_surface_and_relaxed_global_preserve_legacy_values() {
    let _guard = LEVEL_TEST.lock().unwrap();
    assert_eq!(Log::FATAL, 0_i32);
    assert_eq!(Log::ERROR, 1_i32);
    assert_eq!(Log::WARN, 2_i32);
    assert_eq!(Log::INFO, 3_i32);
    assert_eq!(Log::DEBUG, 4_i32);
    assert_eq!(Log::level_now(), 4_i32);

    Log::set_level(Log::WARN);
    assert_eq!(Log::level_now(), 2_i32);
    Log::set_level(Log::DEBUG);
}

#[test]
fn level_tags_keep_the_exact_two_byte_strings() {
    assert_eq!(log_level_tag(0_i32), "F ");
    assert_eq!(log_level_tag(1_i32), "E ");
    assert_eq!(log_level_tag(2_i32), "W ");
    assert_eq!(log_level_tag(3_i32), "I ");
    assert_eq!(log_level_tag(4_i32), "D ");
    assert_eq!(log_level_tag(-1_i32), "? ");
    assert_eq!(log_level_tag(5_i32), "? ");
}

#[test]
fn basename_copies_null_normal_trailing_and_non_utf8_inputs_exactly() {
    assert_eq!(log_basename(core::ptr::null()).as_ref(), b"<unknown>");

    let ordinary = b"src/rrr/base/logging.cpp\0";
    assert_eq!(
        log_basename(ordinary.as_ptr() as *const i8).as_ref(),
        b"logging.cpp"
    );

    let trailing = b"some/path/\0";
    assert_eq!(log_basename(trailing.as_ptr() as *const i8).as_ref(), b"");

    let arbitrary = [b'x', b'/', 0xff_u8, 0_u8];
    assert_eq!(
        log_basename(arbitrary.as_ptr() as *const i8).as_ref(),
        &[0xff_u8]
    );
}

#[test]
fn timestamp_keeps_twenty_three_bytes_and_trims_only_the_c_nul() {
    assert_eq!(log_time_now().as_ref(), b"2026-08-09 03:04:05.006");

    // The Cargo-only std::string stand-in deliberately permits the C seam to
    // fill resized storage through data(), but must not claim shared-thread
    // access is safe while that interior mutation exists.
    assert_not_sync::<legacy_logging::cpp::std::string>();
    let owner = include_str!("../src/base/legacy_logging.rs");
    assert!(owner.contains("pub struct string(UnsafeCell<Vec<u8>>);"));
    assert!(!owner.contains("as_ptr() as *mut i8"));
}

#[test]
fn owner_keeps_the_exact_exported_compatibility_surface() {
    let owner = include_str!("../src/base/legacy_logging.rs");
    assert_eq!(owner.matches("pub static LOG_LEVEL_S").count(), 1_usize);
    assert_eq!(owner.matches("pub struct Log").count(), 1_usize);
    for function in [
        "pub fn log_level_tag",
        "pub fn log_line",
        "pub fn log_sink_write",
        "pub fn log_basename",
        "pub fn log_time_now",
    ] {
        assert_eq!(owner.matches(function).count(), 1_usize, "{function}");
    }
}
