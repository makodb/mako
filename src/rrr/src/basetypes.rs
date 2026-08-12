// Canonical Rust source for the rrr.basetypes module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.
pub use std::sync::atomic::{AtomicI64, Ordering};

#[allow(unsafe_code)]
unsafe extern "C" {
    fn srpc_clock_monotonic_us() -> u64;
    fn srpc_clock_realtime_coarse_us() -> u64;
    fn srpc_gettimeofday_us() -> u64;
    fn srpc_sleep_us(microseconds: u64);
}

#[allow(non_camel_case_types)]
pub type i8 = ::core::primitive::i8;
#[allow(non_camel_case_types)]
pub type i16 = ::core::primitive::i16;
#[allow(non_camel_case_types)]
pub type i32 = ::core::primitive::i32;
#[allow(non_camel_case_types)]
pub type i64 = ::core::primitive::i64;

pub struct SparseInt {}

impl SparseInt {
    pub fn buf_size(byte0: u8) -> usize {
        if (byte0 & 0x80) == 0 {
            1
        } else if (byte0 & 0xC0) == 0x80 {
            2
        } else if (byte0 & 0xE0) == 0xC0 {
            3
        } else if (byte0 & 0xF0) == 0xE0 {
            4
        } else if (byte0 & 0xF8) == 0xF0 {
            5
        } else if (byte0 & 0xFC) == 0xF8 {
            6
        } else if (byte0 & 0xFE) == 0xFC {
            7
        } else if byte0 == 0xFE {
            8
        } else {
            9
        }
    }

    /// Encodes `val` into the historical sparse-integer wire format.
    ///
    /// # Safety
    ///
    /// `buf` must point to writable storage for at least five bytes.
    #[allow(unsafe_code)]
    pub unsafe fn dump32(val: i32, buf: *mut u8) -> usize {
        let u = val as u32;
        unsafe {
            if (-64..=63).contains(&val) {
                *buf.add(0) = (u & 0xFF) as u8;
                *buf.add(0) &= 0x7F;
                return 1;
            } else if (-8192..=8191).contains(&val) {
                *buf.add(0) = ((u >> 8) & 0xFF) as u8;
                *buf.add(1) = (u & 0xFF) as u8;
                *buf.add(0) &= 0x3F;
                *buf.add(0) |= 0x80;
                return 2;
            } else if (-1_048_576..=1_048_575).contains(&val) {
                *buf.add(0) = ((u >> 16) & 0xFF) as u8;
                *buf.add(1) = ((u >> 8) & 0xFF) as u8;
                *buf.add(2) = (u & 0xFF) as u8;
                *buf.add(0) &= 0x1F;
                *buf.add(0) |= 0xC0;
                return 3;
            } else if (-134_217_728..=134_217_727).contains(&val) {
                *buf.add(0) = ((u >> 24) & 0xFF) as u8;
                *buf.add(1) = ((u >> 16) & 0xFF) as u8;
                *buf.add(2) = ((u >> 8) & 0xFF) as u8;
                *buf.add(3) = (u & 0xFF) as u8;
                *buf.add(0) &= 0x0F;
                *buf.add(0) |= 0xE0;
                return 4;
            }
            *buf.add(1) = ((u >> 24) & 0xFF) as u8;
            *buf.add(2) = ((u >> 16) & 0xFF) as u8;
            *buf.add(3) = ((u >> 8) & 0xFF) as u8;
            *buf.add(4) = (u & 0xFF) as u8;
            *buf.add(0) = if val < 0 { 0xF7 } else { 0xF0 };
        }
        5
    }

    /// Encodes `val` into the historical sparse-integer wire format.
    ///
    /// # Safety
    ///
    /// `buf` must point to writable storage for nine bytes. This includes the
    /// legacy length-eight encoding, which reports eight but writes nine bytes.
    #[allow(unsafe_code)]
    pub unsafe fn dump64(val: i64, buf: *mut u8) -> usize {
        let u = val as u64;
        let n = SparseInt::val_size(val) as i32;
        unsafe {
            if n <= 7 {
                let mut j = 0i32;
                while j < n {
                    *buf.add(j as usize) = ((u >> (8 * ((n - 1 - j) as u32))) & 0xFF) as u8;
                    j += 1;
                }
                if n == 1 {
                    *buf.add(0) &= 0x7F;
                } else if n == 2 {
                    *buf.add(0) &= 0x3F;
                    *buf.add(0) |= 0x80;
                } else if n == 3 {
                    *buf.add(0) &= 0x1F;
                    *buf.add(0) |= 0xC0;
                } else if n == 4 {
                    *buf.add(0) &= 0x0F;
                    *buf.add(0) |= 0xE0;
                } else if n == 5 {
                    *buf.add(0) &= 0x07;
                    *buf.add(0) |= 0xF0;
                } else if n == 6 {
                    *buf.add(0) &= 0x03;
                    *buf.add(0) |= 0xF8;
                } else {
                    *buf.add(0) &= 0x01;
                    *buf.add(0) |= 0xFC;
                }
                return n as usize;
            }
            let mut j = 0i32;
            while j < 8 {
                *buf.add((1 + j) as usize) = ((u >> (8 * ((7 - j) as u32))) & 0xFF) as u8;
                j += 1;
            }
            if n == 8 {
                *buf.add(0) = 0xFE;
                return 8;
            }
            *buf.add(0) = 0xFF;
        }
        9
    }

    /// Decodes an i32 from the historical sparse-integer wire format.
    ///
    /// # Safety
    ///
    /// `buf` must point to a complete encoded i32 value of the length selected
    /// by its first byte.
    #[allow(unsafe_code)]
    pub unsafe fn load32(buf: *const u8) -> i32 {
        unsafe {
            let bsize = SparseInt::buf_size(*buf.add(0)) as i32;
            let mut u = 0u32;
            if bsize < 5 {
                let mut i = 0i32;
                while i < bsize - 1 {
                    u |= (*buf.add((bsize - 1 - i) as usize) as u32) << (8 * (i as u32));
                    i += 1;
                }
                let mut top = *buf.add(0);
                top &= (0xFF >> bsize) as u8;
                if ((top >> (7 - bsize)) & 1) == 1 {
                    top |= ((0xFF << (7 - bsize)) & 0xFF) as u8;
                    let mut k = bsize;
                    while k < 4 {
                        u |= 0xFFu32 << (8 * (k as u32));
                        k += 1;
                    }
                }
                u |= (top as u32) << (8 * ((bsize - 1) as u32));
                return u as i32;
            }
            let mut i = 0i32;
            while i < 4 {
                u |= (*buf.add((4 - i) as usize) as u32) << (8 * (i as u32));
                i += 1;
            }
            u as i32
        }
    }

    /// Decodes an i64 from the historical sparse-integer wire format.
    ///
    /// # Safety
    ///
    /// `buf` must point to a complete encoded i64 value. Markers below `0xFE`
    /// require the length selected by the first byte; both `0xFE` and `0xFF`
    /// require nine accessible bytes because the legacy decoder reads the
    /// marker plus eight payload bytes even though `0xFE` reports length eight.
    #[allow(unsafe_code)]
    pub unsafe fn load64(buf: *const u8) -> i64 {
        unsafe {
            let bsize = SparseInt::buf_size(*buf.add(0)) as i32;
            let mut u = 0u64;
            if bsize < 8 {
                let mut i = 0i32;
                while i < bsize - 1 {
                    u |= (*buf.add((bsize - 1 - i) as usize) as u64) << (8 * (i as u32));
                    i += 1;
                }
                let mut top = *buf.add(0);
                top &= (0xFF >> bsize) as u8;
                if ((top >> (7 - bsize)) & 1) == 1 {
                    top |= ((0xFF << (7 - bsize)) & 0xFF) as u8;
                    let mut k = bsize;
                    while k < 8 {
                        u |= 0xFFu64 << (8 * (k as u32));
                        k += 1;
                    }
                }
                u |= (top as u64) << (8 * ((bsize - 1) as u32));
                return u as i64;
            }
            let mut i = 0i32;
            while i < 8 {
                u |= (*buf.add((8 - i) as usize) as u64) << (8 * (i as u32));
                i += 1;
            }
            u as i64
        }
    }

    pub fn val_size(val: i64) -> usize {
        if (-64..=63).contains(&val) {
            1
        } else if (-8192..=8191).contains(&val) {
            2
        } else if (-1_048_576..=1_048_575).contains(&val) {
            3
        } else if (-134_217_728..=134_217_727).contains(&val) {
            4
        } else if (-17_179_869_184..=17_179_869_183).contains(&val) {
            5
        } else if (-2_199_023_255_552..=2_199_023_255_551).contains(&val) {
            6
        } else if (-281_474_976_710_656..=281_474_976_710_655).contains(&val) {
            7
        } else if (-36_028_797_018_963_968..=36_028_797_018_963_967).contains(&val) {
            8
        } else {
            9
        }
    }
}

#[repr(C)]
pub struct v32 {
    pub val_field: i32,
}

impl v32 {
    pub fn new(v: i32) -> v32 {
        v32 { val_field: v }
    }
    pub fn set(&mut self, v: i32) {
        self.val_field = v;
    }
    pub fn get(&self) -> i32 {
        self.val_field
    }
    pub fn val_size(&self) -> usize {
        SparseInt::val_size(self.val_field as i64)
    }
}

#[repr(C)]
pub struct v64 {
    pub val_field: i64,
}

impl v64 {
    pub fn new(v: i64) -> v64 {
        v64 { val_field: v }
    }
    pub fn set(&mut self, v: i64) {
        self.val_field = v;
    }
    pub fn get(&self) -> i64 {
        self.val_field
    }
    pub fn val_size(&self) -> usize {
        SparseInt::val_size(self.val_field)
    }
}

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

pub const RRR_USEC_PER_SEC: u64 = 1_000_000;

pub fn abort_if_false(cond: bool) {
    if !cond {
        std::process::abort();
    }
}

pub fn time_now_us(accurate: bool) -> u64 {
    #[allow(unsafe_code)]
    unsafe {
        if accurate {
            srpc_clock_monotonic_us()
        } else {
            srpc_clock_realtime_coarse_us()
        }
    }
}

pub struct Time {}

impl Time {
    pub fn now(accurate: bool) -> u64 {
        time_now_us(accurate)
    }

    pub fn sleep(t: u64) {
        #[allow(unsafe_code)]
        unsafe {
            srpc_sleep_us(t);
        }
    }
}

#[repr(C)]
pub struct Timer {
    pub begin_us: u64,
    pub end_us: u64,
}

impl Timer {
    #[allow(clippy::new_without_default)]
    pub fn new() -> Timer {
        Timer {
            begin_us: 0,
            end_us: 0,
        }
    }

    pub fn start(&mut self) {
        #[allow(unsafe_code)]
        unsafe {
            self.begin_us = srpc_gettimeofday_us();
        }
        self.end_us = 0;
    }

    pub fn stop(&mut self) {
        #[allow(unsafe_code)]
        unsafe {
            self.end_us = srpc_gettimeofday_us();
        }
    }

    pub fn reset(&mut self) {
        self.begin_us = 0;
        self.end_us = 0;
    }

    pub fn elapsed(&self) -> f64 {
        abort_if_false(self.begin_us != 0);
        #[allow(unsafe_code)]
        let end = if self.end_us == 0 {
            unsafe { srpc_gettimeofday_us() }
        } else {
            self.end_us
        };
        (end.wrapping_sub(self.begin_us) as f64) / 1_000_000.0
    }
}
