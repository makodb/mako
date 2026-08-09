//! Scheduler jobs and the one remaining formatting helper from legacy
//! `rrr.misc`.
//!
//! A closed-world caller audit deliberately removed two dead exports while
//! moving this file to valid Rust:
//!
//! - the heterogeneous C++ `clamp<T, T1, T2>` template cannot be expressed in
//!   stable Rust without imposing `PartialOrd`/`From` bounds that narrow the
//!   accepted C++ instantiations;
//! - `get_ncpu` had no callers and would require either a new C shim or an
//!   unsupported translation of `std::thread::available_parallelism`.
//! - `JobAdapter`, `JobAdapterRef`, and `JobAdapterRefMut` were unreferenced,
//!   incomplete declarations injected by the former inline-trait emitter;
//!   they were never part of the handwritten `Job` API.
//!
//! Neither helper is approximated here. The exported owner surface is exactly
//! `Job`, `OneTimeJob`, and `format_thousands`.

#![allow(non_snake_case)]

// Native Rust uses `String`. The C++ consumer profile maps this private alias
// to `std::string`, preserving the legacy `format_thousands` signature.
type LegacyStdString = String;

/// A unit of work scheduled by the reactor.
pub trait Job {
    fn Ready(&mut self) -> bool;
    fn Work(&mut self);
    fn Done(&mut self) -> bool;
}

/// A job that starts ready and records completion after invoking its callback.
pub struct OneTimeJob {
    pub done_: bool,
    pub ready_: bool,
    pub func_: Box<dyn FnMut()>,
}

impl OneTimeJob {
    pub fn new(func: Box<dyn FnMut()>) -> OneTimeJob {
        OneTimeJob {
            done_: false,
            ready_: true,
            func_: func,
        }
    }
}

// The permanently-disabled cfg_attr keeps this file valid under rustc while
// requesting direct C++ inheritance from the generated Job interface.
#[cfg_attr(any(), cpp_inherit)]
impl Job for OneTimeJob {
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
///
/// The sole production caller supplies finite, positive QPS values. Rust's
/// formatter spells a hypothetical NaN as `NaN`, whereas both the legacy and
/// generated C++ `std::format` path spell it `nan`; that unreachable
/// cross-language presentation difference is intentionally not normalized.
pub fn format_thousands(val: f64) -> LegacyStdString {
    let formatted: LegacyStdString = format!("{:.2}", val);
    let bytes = formatted.as_bytes();

    let mut dot = 0usize;
    while dot < bytes.len() {
        if bytes[dot] == b'.' {
            break;
        }
        dot += 1;
    }

    let mut out: LegacyStdString = format!("");
    let mut index = 0usize;
    while index < dot {
        if (dot - index) % 3 == 0 && index != 0 && bytes[index - 1] != b'-' {
            out = format!("{},", out);
        }
        out = format!("{}{}", out, bytes[index] as char);
        index += 1;
    }
    while index < bytes.len() {
        out = format!("{}{}", out, bytes[index] as char);
        index += 1;
    }

    // Match the legacy formatter's normalization of rounded negative zero.
    if out == "-0.00" {
        return format!("0.00");
    }
    out
}
