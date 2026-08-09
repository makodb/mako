//! Legacy `rrr.basetypes` compatibility surface.
//!
//! This is the whole-file owner for the small value types and utility classes
//! that remain in `src/rrr/base/basetypes.cpp`.  It preserves the live surface
//! produced by the preceding inline-DSL conversion: the four historical
//! signed-integer aliases, `SparseInt`, `v32`/`v64`, `Counter`, the
//! namespace-level microseconds constant and abort helper, and `Time`/`Timer`.
//! APIs retired before this owner (`NoCopy`, `Rand`, `Enumerator`,
//! `MergedEnumerator`, and the generic `std::atomic` helpers) stay retired.
//!
//! The raw-pointer `SparseInt` methods deliberately retain the legacy wire
//! contract.  In particular, an eight-byte `i64` encoding writes a marker plus
//! all eight payload bytes but reports a length of eight.  The archive copies
//! only those first eight bytes; decode observes a zero-filled ninth scratch
//! byte.  This long-standing quirk is compatibility behavior, not an error to
//! repair while changing ownership.
//!
//! `Timer` likewise keeps the final inline-DSL layout: two public `u64`
//! microsecond stamps (`0` means unset), rather than reintroducing the older
//! pair of `timeval` objects.  The generated C++ types consequently retain the
//! current aggregate layout and implicit copy/move traits.

#![allow(non_camel_case_types)]
#![allow(unsafe_code)]
#![allow(unused_unsafe)]

use cpp::rusty::sys::time as cpp_time;

// These aliases are public compatibility names used throughout generated RPC
// declarations and the memdb/benchmark consumers.  Primitive-qualified right
// hand sides keep this file valid Rust even though the aliases shadow Rust's
// primitive spellings.
pub type i8 = core::primitive::i8;
pub type i16 = core::primitive::i16;
pub type i32 = core::primitive::i32;
pub type i64 = core::primitive::i64;

// The legacy export namespace also made these two using-declarations visible
// as `rrr::AtomicI64` and `rrr::Ordering`.
pub use std::sync::atomic::{AtomicI64, Ordering};

/// Stateless owner of the legacy signed variable-length integer codec.
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default))]
pub struct SparseInt {}

impl SparseInt {
    /// Total encoded length implied by the first byte.
    pub fn buf_size(byte0: u8) -> usize {
        if (byte0 & 0x80_u8) == 0_u8 {
            1_usize
        } else if (byte0 & 0xC0_u8) == 0x80_u8 {
            2_usize
        } else if (byte0 & 0xE0_u8) == 0xC0_u8 {
            3_usize
        } else if (byte0 & 0xF0_u8) == 0xE0_u8 {
            4_usize
        } else if (byte0 & 0xF8_u8) == 0xF0_u8 {
            5_usize
        } else if (byte0 & 0xFC_u8) == 0xF8_u8 {
            6_usize
        } else if (byte0 & 0xFE_u8) == 0xFC_u8 {
            7_usize
        } else if byte0 == 0xFE_u8 {
            8_usize
        } else {
            9_usize
        }
    }

    /// Encode an `i32` into caller-owned storage of at least nine bytes.
    pub fn dump32(val: i32, buf: *mut u8) -> usize {
        let bits: u32 = val as u32;
        if (-64_i32..=63_i32).contains(&val) {
            unsafe {
                *buf.add(0_usize) = (bits & 0xFF_u32) as u8;
                *buf.add(0_usize) &= 0x7F_u8;
            }
            return 1_usize;
        }
        if (-8_192_i32..=8_191_i32).contains(&val) {
            unsafe {
                *buf.add(0_usize) = ((bits >> 8_usize) & 0xFF_u32) as u8;
                *buf.add(1_usize) = (bits & 0xFF_u32) as u8;
                *buf.add(0_usize) &= 0x3F_u8;
                *buf.add(0_usize) |= 0x80_u8;
            }
            return 2_usize;
        }
        if (-1_048_576_i32..=1_048_575_i32).contains(&val) {
            unsafe {
                *buf.add(0_usize) = ((bits >> 16_usize) & 0xFF_u32) as u8;
                *buf.add(1_usize) = ((bits >> 8_usize) & 0xFF_u32) as u8;
                *buf.add(2_usize) = (bits & 0xFF_u32) as u8;
                *buf.add(0_usize) &= 0x1F_u8;
                *buf.add(0_usize) |= 0xC0_u8;
            }
            return 3_usize;
        }
        if (-134_217_728_i32..=134_217_727_i32).contains(&val) {
            unsafe {
                *buf.add(0_usize) = ((bits >> 24_usize) & 0xFF_u32) as u8;
                *buf.add(1_usize) = ((bits >> 16_usize) & 0xFF_u32) as u8;
                *buf.add(2_usize) = ((bits >> 8_usize) & 0xFF_u32) as u8;
                *buf.add(3_usize) = (bits & 0xFF_u32) as u8;
                *buf.add(0_usize) &= 0x0F_u8;
                *buf.add(0_usize) |= 0xE0_u8;
            }
            return 4_usize;
        }

        unsafe {
            *buf.add(1_usize) = ((bits >> 24_usize) & 0xFF_u32) as u8;
            *buf.add(2_usize) = ((bits >> 16_usize) & 0xFF_u32) as u8;
            *buf.add(3_usize) = ((bits >> 8_usize) & 0xFF_u32) as u8;
            *buf.add(4_usize) = (bits & 0xFF_u32) as u8;
            *buf.add(0_usize) = if val < 0_i32 { 0xF7_u8 } else { 0xF0_u8 };
        }
        5_usize
    }

    /// Encode an `i64` into caller-owned storage of at least nine bytes.
    pub fn dump64(val: i64, buf: *mut u8) -> usize {
        let bits: u64 = val as u64;
        let size: usize = SparseInt::val_size(val);
        if size <= 7_usize {
            let mut index: usize = 0_usize;
            while index < size {
                unsafe {
                    *buf.add(index) =
                        ((bits >> (8_usize * ((size - 1_usize) - index))) & 0xFF_u64) as u8;
                }
                index += 1_usize;
            }

            unsafe {
                if size == 1_usize {
                    *buf.add(0_usize) &= 0x7F_u8;
                } else if size == 2_usize {
                    *buf.add(0_usize) &= 0x3F_u8;
                    *buf.add(0_usize) |= 0x80_u8;
                } else if size == 3_usize {
                    *buf.add(0_usize) &= 0x1F_u8;
                    *buf.add(0_usize) |= 0xC0_u8;
                } else if size == 4_usize {
                    *buf.add(0_usize) &= 0x0F_u8;
                    *buf.add(0_usize) |= 0xE0_u8;
                } else if size == 5_usize {
                    *buf.add(0_usize) &= 0x07_u8;
                    *buf.add(0_usize) |= 0xF0_u8;
                } else if size == 6_usize {
                    *buf.add(0_usize) &= 0x03_u8;
                    *buf.add(0_usize) |= 0xF8_u8;
                } else {
                    *buf.add(0_usize) &= 0x01_u8;
                    *buf.add(0_usize) |= 0xFC_u8;
                }
            }
            return size;
        }

        // The size-eight case intentionally touches byte eight, then reports
        // only eight bytes.  See the module-level compatibility note.
        let mut index: usize = 0_usize;
        while index < 8_usize {
            unsafe {
                *buf.add(1_usize + index) =
                    ((bits >> (8_usize * (7_usize - index))) & 0xFF_u64) as u8;
            }
            index += 1_usize;
        }
        unsafe {
            if size == 8_usize {
                *buf.add(0_usize) = 0xFE_u8;
                return 8_usize;
            }
            *buf.add(0_usize) = 0xFF_u8;
        }
        9_usize
    }

    /// Decode an `i32` from a zero-padded caller-owned nine-byte buffer.
    pub fn load32(buf: *const u8) -> i32 {
        let byte0: u8 = unsafe { *buf.add(0_usize) };
        let size: usize = SparseInt::buf_size(byte0);
        let mut bits: u32 = 0_u32;
        if size < 5_usize {
            let mut index: usize = 0_usize;
            while index < size - 1_usize {
                let byte: u8 = unsafe { *buf.add((size - 1_usize) - index) };
                bits |= (byte as u32) << (8_usize * index);
                index += 1_usize;
            }

            let mut top: u8 = byte0;
            top &= (0xFF_u32 >> size) as u8;
            if ((top >> (7_usize - size)) & 0x01_u8) == 0x01_u8 {
                top |= ((0xFF_u32 << (7_usize - size)) & 0xFF_u32) as u8;
                let mut fill: usize = size;
                while fill < 4_usize {
                    bits |= 0xFF_u32 << (8_usize * fill);
                    fill += 1_usize;
                }
            }
            bits |= (top as u32) << (8_usize * (size - 1_usize));
            return bits as i32;
        }

        let mut index: usize = 0_usize;
        while index < 4_usize {
            let byte: u8 = unsafe { *buf.add(4_usize - index) };
            bits |= (byte as u32) << (8_usize * index);
            index += 1_usize;
        }
        bits as i32
    }

    /// Decode an `i64` from a zero-padded caller-owned nine-byte buffer.
    pub fn load64(buf: *const u8) -> i64 {
        let byte0: u8 = unsafe { *buf.add(0_usize) };
        let size: usize = SparseInt::buf_size(byte0);
        let mut bits: u64 = 0_u64;
        if size < 8_usize {
            let mut index: usize = 0_usize;
            while index < size - 1_usize {
                let byte: u8 = unsafe { *buf.add((size - 1_usize) - index) };
                bits |= (byte as u64) << (8_usize * index);
                index += 1_usize;
            }

            let mut top: u8 = byte0;
            top &= (0xFF_u32 >> size) as u8;
            if ((top >> (7_usize - size)) & 0x01_u8) == 0x01_u8 {
                top |= ((0xFF_u32 << (7_usize - size)) & 0xFF_u32) as u8;
                let mut fill: usize = size;
                while fill < 8_usize {
                    bits |= 0xFF_u64 << (8_usize * fill);
                    fill += 1_usize;
                }
            }
            bits |= (top as u64) << (8_usize * (size - 1_usize));
            return bits as i64;
        }

        let mut index: usize = 0_usize;
        while index < 8_usize {
            let byte: u8 = unsafe { *buf.add(8_usize - index) };
            bits |= (byte as u64) << (8_usize * index);
            index += 1_usize;
        }
        bits as i64
    }

    /// Encoded length required for a signed value.
    pub fn val_size(val: i64) -> usize {
        if (-64_i64..=63_i64).contains(&val) {
            1_usize
        } else if (-8_192_i64..=8_191_i64).contains(&val) {
            2_usize
        } else if (-1_048_576_i64..=1_048_575_i64).contains(&val) {
            3_usize
        } else if (-134_217_728_i64..=134_217_727_i64).contains(&val) {
            4_usize
        } else if (-17_179_869_184_i64..=17_179_869_183_i64).contains(&val) {
            5_usize
        } else if (-2_199_023_255_552_i64..=2_199_023_255_551_i64).contains(&val) {
            6_usize
        } else if (-281_474_976_710_656_i64..=281_474_976_710_655_i64).contains(&val) {
            7_usize
        } else if (-36_028_797_018_963_968_i64..=36_028_797_018_963_967_i64).contains(&val) {
            8_usize
        } else {
            9_usize
        }
    }
}

/// Variable-length signed 32-bit wire value.
#[repr(C)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
pub struct v32 {
    pub val_field: i32,
}

impl v32 {
    pub fn new(value: i32) -> v32 {
        v32 { val_field: value }
    }

    pub fn set(&mut self, value: i32) {
        self.val_field = value;
    }

    pub fn get(&self) -> i32 {
        self.val_field
    }

    pub fn val_size(&self) -> usize {
        SparseInt::val_size(self.val_field as i64)
    }
}

/// Variable-length signed 64-bit wire value.
#[repr(C)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, Eq, PartialEq))]
pub struct v64 {
    pub val_field: i64,
}

impl v64 {
    pub fn new(value: i64) -> v64 {
        v64 { val_field: value }
    }

    pub fn set(&mut self, value: i64) {
        self.val_field = value;
    }

    pub fn get(&self) -> i64 {
        self.val_field
    }

    pub fn val_size(&self) -> usize {
        SparseInt::val_size(self.val_field)
    }
}

/// Atomic-backed monotonically increasing counter.
#[repr(C)]
pub struct Counter {
    pub next_field: AtomicI64,
}

impl Counter {
    pub fn new(start: i64) -> Counter {
        Counter {
            next_field: AtomicI64::new(start),
        }
    }

    pub fn peek_next(&self) -> i64 {
        self.next_field.load(Ordering::Relaxed)
    }

    pub fn next(&self, step: i64) -> i64 {
        self.next_field.fetch_add(step, Ordering::AcqRel)
    }

    pub fn reset(&self, start: i64) {
        self.next_field.store(start, Ordering::Relaxed);
    }
}

/// Microseconds per second, retained at namespace scope as in the current API.
pub const RRR_USEC_PER_SEC: u64 = 1_000_000_u64;

/// Abort the process when a compatibility precondition is false.
pub fn abort_if_false(condition: bool) {
    if !condition {
        std::process::abort();
    }
}

/// Dispatch to the accurate monotonic or coarse realtime legacy clock.
pub fn time_now_us(accurate: bool) -> u64 {
    if accurate {
        unsafe { cpp_time::clock_monotonic_us() }
    } else {
        unsafe { cpp_time::clock_realtime_coarse_us() }
    }
}

/// All-static clock and sleep facade.
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default))]
pub struct Time {}

impl Time {
    pub fn now(accurate: bool) -> u64 {
        time_now_us(accurate)
    }

    pub fn sleep(microseconds: u64) {
        unsafe { cpp_time::sleep_us(microseconds) };
    }
}

/// Wall-clock stopwatch with the current two-word C++ layout.
#[repr(C)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Default, PartialEq))]
pub struct Timer {
    pub begin_us: u64,
    pub end_us: u64,
}

impl Timer {
    pub fn new() -> Timer {
        Timer {
            begin_us: 0_u64,
            end_us: 0_u64,
        }
    }

    pub fn start(&mut self) {
        self.begin_us = unsafe { cpp_time::gettimeofday_us() };
        self.end_us = 0_u64;
    }

    pub fn stop(&mut self) {
        self.end_us = unsafe { cpp_time::gettimeofday_us() };
    }

    pub fn reset(&mut self) {
        self.begin_us = 0_u64;
        self.end_us = 0_u64;
    }

    pub fn elapsed(&self) -> f64 {
        abort_if_false(self.begin_us != 0_u64);
        let end: u64 = if self.end_us == 0_u64 {
            unsafe { cpp_time::gettimeofday_us() }
        } else {
            self.end_us
        };
        ((end - self.begin_us) as f64) / 1_000_000.0_f64
    }
}

// Cargo-only definitions for the reserved `cpp::rusty::sys::time` import.
// The C++ consumer suppresses this shim and resolves the four calls through a
// fail-closed module-local symbol index.
mod cpp {
    pub mod rusty {
        pub mod sys {
            pub mod time {
                use std::sync::OnceLock;
                use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

                fn realtime_us() -> u64 {
                    match SystemTime::now().duration_since(UNIX_EPOCH) {
                        Ok(duration) => duration.as_micros() as u64,
                        Err(_) => 0_u64,
                    }
                }

                pub fn clock_monotonic_us() -> u64 {
                    static ORIGIN: OnceLock<Instant> = OnceLock::new();
                    ORIGIN.get_or_init(Instant::now).elapsed().as_micros() as u64
                }

                pub fn clock_realtime_coarse_us() -> u64 {
                    realtime_us()
                }

                pub fn gettimeofday_us() -> u64 {
                    realtime_us()
                }

                pub fn sleep_us(microseconds: u64) {
                    std::thread::sleep(Duration::from_micros(microseconds));
                }
            }
        }
    }
}
