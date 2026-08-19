// Canonical Rust source for the rrr.logging module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.
use cpp::rrr::debugging as cpp_debugging;
use cpp::std as cpp_std;
use rusty as cpp;
use std::sync::atomic::{AtomicI32, Ordering};

// Consumer type mappings restore the historical C++ spellings.
type LegacyCChar = i8;
type LegacyStdString = String;

/// Process-wide maximum enabled severity. The default keeps DEBUG enabled.
pub static LOG_LEVEL_S: AtomicI32 = AtomicI32::new(4_i32);

/// All-static compatibility facade used by C++ callers and `rrr_log.h`.
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
pub unsafe fn log_line(level: i32, line: i32, file: *const i8, msg: &LegacyStdString) {
    if level > Log::DEBUG {
        // SAFETY: the indexed verifier has no caller-side precondition.
        unsafe { cpp_debugging::verify(false) };
    }
    if level <= Log::level_now() {
        let mut out: rusty::LoggingString = Default::default();
        out.append(log_level_tag(level));
        out.append("[");
        // SAFETY: upheld by this function's contract.
        out.append(unsafe { log_basename(file) });
        out.append(":");
        out.append(&line.to_string());
        out.append("] ");
        out.append(log_time_now());
        out.append(" | ");
        out.append(msg);
        log_sink_write(&out);
    }
}

/// Write the exact line bytes, append one newline, and flush `std::cout`.
#[allow(unsafe_code)]
pub fn log_sink_write(line: &rusty::LoggingString) {
    // SAFETY: `line.data()` remains valid for `line.size()` bytes for the
    // duration of these synchronous output calls.
    unsafe {
        cpp_std::cout.write(line.data(), line.size());
        cpp_std::cout.put(b'\n' as LegacyCChar);
        cpp_std::cout.flush();
    }
}

#[allow(unsafe_code)]
mod logging_ffi {
    use super::LegacyCChar;

    unsafe extern "C" {
        pub(super) fn srpc_path_basename(path: *const LegacyCChar) -> *const LegacyCChar;
        pub(super) fn srpc_time_now_str(now: *mut LegacyCChar);
    }
}

/// Return an owned byte-for-byte copy of the filename portion of `fpath`.
///
/// # Safety
///
/// `fpath` must be null or point to a valid NUL-terminated path for the
/// duration of the call.
#[allow(unsafe_code)]
pub unsafe fn log_basename(fpath: *const i8) -> rusty::LoggingString {
    let mut out: rusty::LoggingString = Default::default();
    // SAFETY: upheld by this function's contract.
    let base = unsafe { logging_ffi::srpc_path_basename(fpath as *const LegacyCChar) };
    if base.is_null() {
        out.append("<unknown>");
        return out;
    }
    let mut index: usize = 0;
    // SAFETY: the C helper returns either null or a pointer into the same
    // valid NUL-terminated input string.
    while unsafe { *base.add(index) } != 0 as LegacyCChar {
        // SAFETY: `index` is advanced only until the first NUL byte.
        out.push_back(unsafe { *base.add(index) });
        index += 1;
    }
    out
}

/// Produce the legacy 23-character local-time timestamp.
#[allow(unsafe_code)]
pub fn log_time_now() -> rusty::LoggingString {
    let mut now: rusty::LoggingString = Default::default();
    now.resize(24);
    // SAFETY: resize created 24 writable bytes; the terminal C helper writes
    // exactly 23 timestamp bytes and a trailing NUL.
    unsafe { logging_ffi::srpc_time_now_str(now.data()) };
    now.resize(23);
    now
}
