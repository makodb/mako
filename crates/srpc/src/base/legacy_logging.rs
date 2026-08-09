//! Legacy `rrr.logging` compatibility surface.
//!
//! The valid-Rust owner retains the final inline-DSL API exactly: the global
//! relaxed atomic level, the all-static `Log` facade, and the six free helpers
//! that filter, decorate, and emit a line.  The consumer-side variadic
//! `Log_*` templates remain in `src/rrr_log.h`; Rust has no variadic-generics
//! grammar, and those wrappers add only format-pack expansion before calling
//! this module's `log_line`.
//!
//! Two existing plain-C seams remain the unsafe substrate.  One returns the
//! basename pointer inside its input buffer, and the other fills the legacy
//! 24-byte local-time string.  This owner copies arbitrary basename bytes into
//! `std::string` one byte at a time, avoiding a UTF-8 assumption and preserving
//! the historical C++ behavior.

#![allow(non_camel_case_types)]
#![allow(non_upper_case_globals)]
#![allow(unsafe_code)]

use cpp::rrr::debugging as cpp_debugging;
use cpp::std as cpp_std;
use std::sync::atomic::{AtomicI32, Ordering};

// Use the portable byte carrier in Rust; the consumer profile maps this alias
// back to `std::string::value_type` (`char`).  Spelling `core::ffi::c_char`
// directly would emit a private `rusty::ffi::c_char` alias, which the `std`-
// only module does not import.
type LegacyCChar = i8;
type LegacyStdString = String;

/// Process-wide maximum enabled severity.  The default keeps DEBUG enabled.
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
pub fn log_line(level: i32, line: i32, file: *const i8, msg: &LegacyStdString) {
    if level > Log::DEBUG {
        unsafe { cpp_debugging::verify(&false) };
    }
    if level <= Log::level_now() {
        let mut out: cpp_std::string = Default::default();
        out.append(log_level_tag(level));
        out.append("[");
        out.append(&log_basename(file));
        out.append(":");
        out.append(&line.to_string());
        out.append("] ");
        out.append(&log_time_now());
        out.append(" | ");
        out.append(msg);
        log_sink_write(&out);
    }
}

/// Write the exact line bytes, append one newline, and flush `std::cout`.
pub fn log_sink_write(line: &cpp_std::string) {
    unsafe {
        cpp_std::cout.write(line.data(), line.size());
        cpp_std::cout.put(b'\n' as LegacyCChar);
        cpp_std::cout.flush();
    }
}

mod logging_ffi {
    use super::LegacyCChar;

    unsafe extern "C" {
        pub(super) fn srpc_path_basename(path: *const LegacyCChar) -> *const LegacyCChar;
        pub(super) fn srpc_time_now_str(now: *mut LegacyCChar);
    }
}

/// Return an owned byte-for-byte copy of the filename portion of `fpath`.
pub fn log_basename(fpath: *const i8) -> cpp_std::string {
    let mut out: cpp_std::string = Default::default();
    let base = unsafe { logging_ffi::srpc_path_basename(fpath as *const LegacyCChar) };
    if base.is_null() {
        out.append("<unknown>");
        return out;
    }
    let mut index: usize = 0_usize;
    while unsafe { *base.add(index) } != 0 as LegacyCChar {
        out.push_back(unsafe { *base.add(index) });
        index += 1_usize;
    }
    out
}

/// Produce the legacy 23-character local-time timestamp.
pub fn log_time_now() -> cpp_std::string {
    let mut now: cpp_std::string = Default::default();
    now.resize(24_usize);
    unsafe { logging_ffi::srpc_time_now_str(now.data()) };
    now.resize(23_usize);
    now
}

// Cargo-only definitions for reserved `cpp::` imports.  The C++ consumer
// suppresses this module and resolves the indexed `rrr.debugging` and `std`
// symbols directly.
#[allow(dead_code)]
pub(crate) mod cpp {
    pub mod rrr {
        pub mod debugging {
            pub unsafe fn verify(condition: &bool) {
                assert!(*condition);
            }
        }
    }

    pub mod std {
        use ::std::cell::UnsafeCell;
        use ::std::io::Write;

        #[allow(non_camel_case_types)]
        pub struct string(UnsafeCell<Vec<u8>>);

        impl Default for string {
            fn default() -> string {
                string(UnsafeCell::new(Vec::<u8>::new()))
            }
        }

        impl string {
            pub fn append<T: AsRef<[u8]>>(&mut self, value: T) {
                self.0.get_mut().extend_from_slice(value.as_ref());
            }

            pub fn push_back(&mut self, value: i8) {
                self.0.get_mut().push(value as u8);
            }

            pub fn resize(&mut self, size: usize) {
                self.0.get_mut().resize(size, 0_u8);
            }

            pub fn data(&self) -> *mut i8 {
                // The returned pointer is used only by an unsafe C/C++ call.
                // UnsafeCell makes that write legal through this shared
                // receiver; callers must not retain an AsRef slice across it.
                unsafe { (&mut *self.0.get()).as_mut_ptr() as *mut i8 }
            }

            pub fn size(&self) -> usize {
                unsafe { (&*self.0.get()).len() }
            }
        }

        impl AsRef<[u8]> for string {
            fn as_ref(&self) -> &[u8] {
                unsafe { (&*self.0.get()).as_slice() }
            }
        }

        pub struct Cout;
        #[allow(non_upper_case_globals)]
        pub static cout: Cout = Cout;

        impl Cout {
            pub unsafe fn write(&self, data: *mut i8, size: usize) {
                let bytes = unsafe { core::slice::from_raw_parts(data as *const u8, size) };
                let _ = ::std::io::stdout().write_all(bytes);
            }

            pub unsafe fn put(&self, value: i8) {
                let _ = ::std::io::stdout().write_all(&[value as u8]);
            }

            pub unsafe fn flush(&self) {
                let _ = ::std::io::stdout().flush();
            }
        }
    }
}
