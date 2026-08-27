//! Scheduler jobs and formatting/process helpers from legacy `rrr.misc`.
//!
//! This remains at its historical `.cpp` path so Git preserves the C++ to
//! Rust lineage. Cargo reads this exact file: the generated `src/lib.rs`
//! declares `#[path = "../base/misc.rs"] pub mod misc;`.

#![allow(non_snake_case)]

use rusty::cpp_inherit;

// Consumer type mappings restore the historical `std::string` spelling. The
// rustc-only byte model also exposes the C++-spelled `push_back` operation.
type LegacyStdString = rusty::LoggingString;

/// Clamp a value between potentially heterogeneous bounds.
//
// Reference conversion bounds make this valid stable Rust without adding C++
// constraints: rusty-cpp emits the exact historical
// `clamp<T, T1, T2>(const T&, const T1&, const T2&)` template and its
// `from_into<T>` lowering preserves C++ implicit conversions.
pub fn clamp<T, T1, T2>(value: &T, lower: &T1, upper: &T2) -> T
where
    T: PartialOrd<T1> + PartialOrd<T2>,
    for<'a> &'a T: Into<T>,
    for<'a> &'a T1: Into<T>,
    for<'a> &'a T2: Into<T>,
{
    if value < lower {
        return lower.into();
    }
    if value > upper {
        return upper.into();
    }
    value.into()
}

#[allow(unsafe_code)]
mod misc_ffi {
    unsafe extern "C" {
        pub(super) fn srpc_get_ncpu() -> i32;
        pub(super) fn srpc_format_fixed_2(value: f64, output: *mut i8, capacity: usize) -> i32;
    }
}

/// Return the number of online processors reported by `sysconf`.
#[allow(unsafe_code)]
pub fn get_ncpu() -> i32 {
    // SAFETY: the plain-C seam takes no arguments or caller-owned storage.
    unsafe { misc_ffi::srpc_get_ncpu() }
}

/// A unit of work scheduled by the reactor.
///
/// # Safety
///
/// The reactor owns mutable execution of a job after it is submitted. An
/// implementation must not expose aliases that concurrently mutate the state
/// reached by `Ready`, `Work`, or `Done`. `Send + Sync` proves that the shared
/// handle itself can cross into the poll worker; this unsafe trait records the
/// remaining single-worker mutation invariant.
///
/// Thread-confined state is rejected even when an implementation explicitly
/// acknowledges the worker-ownership invariant:
///
/// ```compile_fail
/// use rrr::misc::Job;
/// use std::cell::Cell;
/// use std::rc::Rc;
///
/// struct ThreadConfined(Rc<Cell<u32>>);
///
/// unsafe impl Job for ThreadConfined {
///     fn Ready(&mut self) -> bool { true }
///     fn Work(&mut self) { self.0.set(self.0.get() + 1); }
///     fn Done(&mut self) -> bool { false }
/// }
/// ```
#[allow(unsafe_code)]
pub unsafe trait Job: Send + Sync {
    fn Ready(&mut self) -> bool;
    fn Work(&mut self);
    fn Done(&mut self) -> bool;
}

/// A job that starts ready and records completion after invoking its callback.
pub struct OneTimeJob {
    // Kept private in Rust so a caller retaining an Arc cannot race the
    // worker's exclusive mutable dispatch. rusty-cpp still emits the
    // historical public C++ aggregate fields and unchanged layout.
    done_: bool,
    ready_: bool,
    func_: Box<dyn FnMut() + Send + Sync>,
}

impl OneTimeJob {
    pub fn new(func: Box<dyn FnMut() + Send + Sync>) -> OneTimeJob {
        OneTimeJob {
            done_: false,
            ready_: true,
            func_: func,
        }
    }
}

// Direct inheritance is required by existing Arc<OneTimeJob> -> Arc<Job>
// upcasts. The crate root imports a rustc-only no-op macro with this name;
// rusty-cpp consumes the retained attribute during production generation.
#[cpp_inherit]
#[allow(unsafe_code)]
unsafe impl Job for OneTimeJob {
    fn Ready(&mut self) -> bool {
        self.ready_
    }

    fn Work(&mut self) {
        self.ready_ = false;
        (self.func_)();
        self.done_ = true;
    }

    fn Done(&mut self) -> bool {
        self.done_
    }
}

/// Format a number with two fractional digits and comma-separated thousands.
#[allow(clippy::manual_is_multiple_of)]
#[allow(unsafe_code)]
pub fn format_thousands(val: f64) -> LegacyStdString {
    // A fixed buffer covers the longest finite f64 rendered with two decimal
    // places (sign + 309 integer digits + separator + two fraction digits).
    let mut bytes = [0_i8; 384];
    // SAFETY: `bytes` supplies `bytes.len()` writable elements for the
    // duration of the synchronous C formatting call.
    let byte_count = unsafe { misc_ffi::srpc_format_fixed_2(val, bytes.as_mut_ptr(), bytes.len()) };
    assert!(
        byte_count >= 0 && (byte_count as usize) < bytes.len(),
        "fixed decimal representation exceeds its complete f64 buffer"
    );
    let formatted_len = byte_count as usize;

    let mut dot = 0usize;
    while dot < formatted_len {
        if bytes[dot] == b'.' as i8 {
            break;
        }
        dot += 1;
    }

    let negative_zero = formatted_len == 5
        && bytes[0] == b'-' as i8
        && bytes[1] == b'0' as i8
        && bytes[2] == b'.' as i8
        && bytes[3] == b'0' as i8
        && bytes[4] == b'0' as i8;
    let mut out: LegacyStdString = Default::default();
    let mut index = if negative_zero { 1usize } else { 0usize };
    while index < dot {
        if (dot - index) % 3 == 0 && index != 0 && bytes[index - 1] != b'-' as i8 {
            out.push_back(b',' as i8);
        }
        out.push_back(bytes[index]);
        index += 1;
    }
    while index < formatted_len {
        out.push_back(bytes[index]);
        index += 1;
    }
    out
}
