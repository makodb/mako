// Canonical Rust source for the srpc.logging module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.
use crate::debugging::verify_at;
use std::io::Write;
use std::sync::atomic::{AtomicI32, Ordering};

// Consumer type mappings restore the historical C++ spellings.
type LegacyCChar = i8;

/// Process-wide maximum enabled severity. The default keeps DEBUG enabled.
pub static LOG_LEVEL_S: AtomicI32 = AtomicI32::new(4_i32);

/// All-static compatibility facade used by C++ callers and `srpc_log.h`.
pub struct Log {}

impl Log {
    pub const FATAL: i32 = 0;
    pub const ERROR: i32 = 1;
    pub const WARN: i32 = 2;
    pub const INFO: i32 = 3;
    pub const DEBUG: i32 = 4;

    pub fn set_level(level: i32) {
        LOG_LEVEL_S.store(level, Ordering::Relaxed);
    }

    pub fn level_now() -> i32 {
        LOG_LEVEL_S.load(Ordering::Relaxed)
    }
}

/// Historical two-byte severity prefix, including its trailing space.
pub fn log_level_tag(level: i32) -> &'static str {
    match level {
        0 => "F ",
        1 => "E ",
        2 => "W ",
        3 => "I ",
        4 => "D ",
        _ => "? ",
    }
}

/// Filter, decorate, and synchronously emit one preformatted message.
///
/// # Safety
///
/// `file` must be null or point to a valid NUL-terminated path for the
/// duration of the call. The logger scans any non-null path.
#[allow(unsafe_code)]
pub unsafe fn log_line(level: i32, line: i32, file: *const i8, msg: &str) {
    if level > Log::DEBUG {
        // SAFETY: the indexed verifier has no caller-side precondition.
        verify_at(false, file!(), line!());
    }
    if level <= Log::level_now() {
        let mut out = String::new();
        out.push_str(log_level_tag(level));
        out.push('[');
        // SAFETY: upheld by this function's contract.
        out.push_str(&unsafe { log_basename(file) });
        out.push(':');
        out.push_str(&line.to_string());
        out.push_str("] ");
        out.push_str(&log_time_now());
        out.push_str(" | ");
        out.push_str(msg);
        log_sink_write(&out);
    }
}

/// Write the exact line bytes, append one newline, and flush stdout.
pub fn log_sink_write(line: &str) {
    let mut output = std::io::stdout();
    let _ = output.write_all(line.as_bytes());
    let _ = output.write_all(b"\n");
    let _ = output.flush();
}

#[allow(unsafe_code)]
mod logging_ffi {
    use super::LegacyCChar;

    unsafe extern "C" {
        pub(super) fn srpc_path_basename(path: *const LegacyCChar) -> *const LegacyCChar;
        pub(super) fn srpc_local_calendar_fields(fields: *mut i32) -> i32;
        pub(super) fn srpc_gettimeofday_us() -> u64;
    }
}

/// Return an owned copy of the filename portion of `fpath`. Valid UTF-8 is
/// preserved byte for byte; an invalid sequence becomes U+FFFD, as a Rust
/// `String` requires.
///
/// # Safety
///
/// `fpath` must be null or point to a valid NUL-terminated path for the
/// duration of the call.
#[allow(unsafe_code)]
pub unsafe fn log_basename(fpath: *const i8) -> String {
    // SAFETY: upheld by this function's contract.
    let base = unsafe { logging_ffi::srpc_path_basename(fpath as *const LegacyCChar) };
    if base.is_null() {
        return String::from("<unknown>");
    }
    let mut bytes: Vec<u8> = Vec::new();
    let mut index: usize = 0;
    // SAFETY: the C helper returns either null or a pointer into the same
    // valid NUL-terminated input string.
    while unsafe { *base.add(index) } != 0 as LegacyCChar {
        // SAFETY: `index` is advanced only until the first NUL byte.
        bytes.push(unsafe { *base.add(index) } as u8);
        index += 1;
    }
    String::from(String::from_utf8_lossy(bytes.as_slice()))
}

fn log_write_digits(bytes: &mut [u8], mut value: i32, offset: usize, digits: usize) {
    let mut index = offset + digits;
    while index > offset {
        index -= 1;
        bytes[index] = b'0' + (value % 10) as u8;
        value /= 10;
    }
}

fn log_format_time(fields: &[i32], milliseconds: i32) -> String {
    let mut bytes: [u8; 23] = [0; 23];
    log_write_digits(&mut bytes, fields[0], 0, 4);
    bytes[4] = b'-';
    log_write_digits(&mut bytes, fields[1], 5, 2);
    bytes[7] = b'-';
    log_write_digits(&mut bytes, fields[2], 8, 2);
    bytes[10] = b' ';
    log_write_digits(&mut bytes, fields[3], 11, 2);
    bytes[13] = b':';
    log_write_digits(&mut bytes, fields[4], 14, 2);
    bytes[16] = b':';
    log_write_digits(&mut bytes, fields[5], 17, 2);
    bytes[19] = b'.';
    log_write_digits(&mut bytes, milliseconds, 20, 3);
    // Every byte is an ASCII digit or separator, so this is lossless.
    String::from(String::from_utf8_lossy(bytes.as_slice()))
}

/// Produce the legacy 23-character local-time timestamp.
#[allow(unsafe_code)]
pub fn log_time_now() -> String {
    let mut fields: [i32; 6] = [0; 6];
    // SAFETY: the calendar operation writes exactly six integer fields.
    if unsafe { logging_ffi::srpc_local_calendar_fields(fields.as_mut_ptr()) } != 0 {
        std::process::abort();
    }
    let milliseconds = (unsafe { logging_ffi::srpc_gettimeofday_us() } % 1_000_000 / 1_000) as i32;
    log_format_time(&fields, milliseconds)
}

#[cfg(test)]
mod time_format_tests {
    #[test]
    fn timestamp_bytes_preserve_padding_and_leap_second() {
        assert_eq!(
            &*super::log_format_time(&[2026, 1, 2, 3, 4, 5], 6),
            "2026-01-02 03:04:05.006"
        );
        assert_eq!(
            &*super::log_format_time(&[2024, 12, 31, 23, 59, 60], 999),
            "2024-12-31 23:59:60.999"
        );
    }
}
