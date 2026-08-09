//! Per-request timeout and retry policy.
//!
//! The C++ consumer historically obtains jitter from
//! `rrr::RandomGenerator::rand_double`, whose state is the process-wide
//! per-thread `rand_r` stream in `src/rrr/misc/srpc_rand.c`.  Calling that C
//! kernel directly is the smallest source-level seam shared by rustc and the
//! transpiled C++: it preserves the same stream and the same scaling formula
//! without substituting this crate's unrelated xorshift generator.

#![allow(unsafe_code)]

const LEGACY_RAND_MAX: f64 = 2_147_483_647.0;

mod request_options_rand_ffi {
    extern "C" {
        pub(super) fn srpc_rand_raw() -> i32;
    }
}

fn legacy_rand_double(min: f64, max: f64) -> f64 {
    if max == min {
        return min;
    }
    assert!(max > min);
    let raw = unsafe { request_options_rand_ffi::srpc_rand_raw() };
    ((raw as f64) / (LEGACY_RAND_MAX / (max - min))) + min
}

#[allow(non_camel_case_types)]
#[derive(Clone, Copy)]
#[repr(u8)]
pub enum TimeoutType {
    NONE = 0,
    CONNECT_TIMEOUT,
    REQUEST_TIMEOUT,
    RESPONSE_TIMEOUT,
    TOTAL_TIMEOUT,
}

pub fn timeout_type_to_string(ty: TimeoutType) -> &'static str {
    #[allow(unreachable_patterns)]
    match ty {
        TimeoutType::NONE => "NONE",
        TimeoutType::CONNECT_TIMEOUT => "CONNECT_TIMEOUT",
        TimeoutType::REQUEST_TIMEOUT => "REQUEST_TIMEOUT",
        TimeoutType::RESPONSE_TIMEOUT => "RESPONSE_TIMEOUT",
        TimeoutType::TOTAL_TIMEOUT => "TOTAL_TIMEOUT",
        _ => "UNKNOWN",
    }
}

pub struct RequestOptions {
    pub timeout_ms: u64,
    pub total_timeout_ms: u64,
    pub max_retries: u16,
    pub base_delay_ms: u16,
    pub max_delay_ms: u16,
    pub jitter_factor: f32,
    pub idempotent: bool,
}

impl Copy for RequestOptions {}

impl Clone for RequestOptions {
    fn clone(&self) -> RequestOptions {
        RequestOptions {
            timeout_ms: self.timeout_ms,
            total_timeout_ms: self.total_timeout_ms,
            max_retries: self.max_retries,
            base_delay_ms: self.base_delay_ms,
            max_delay_ms: self.max_delay_ms,
            jitter_factor: self.jitter_factor,
            idempotent: self.idempotent,
        }
    }
}

impl RequestOptions {
    pub fn new() -> RequestOptions {
        RequestOptions {
            timeout_ms: 1_000_u64,
            total_timeout_ms: 0_u64,
            max_retries: 0_u16,
            base_delay_ms: 50_u16,
            max_delay_ms: 5_000_u16,
            jitter_factor: 0.1_f32,
            idempotent: false,
        }
    }

    pub fn defaults() -> RequestOptions {
        RequestOptions::new()
    }

    pub fn with_retry(max_retries: u16, timeout_ms: u64) -> RequestOptions {
        RequestOptions {
            timeout_ms,
            total_timeout_ms: 0_u64,
            max_retries,
            base_delay_ms: 50_u16,
            max_delay_ms: 5_000_u16,
            jitter_factor: 0.1_f32,
            idempotent: true,
        }
    }

    pub fn idempotent_retry(max_retries: u16) -> RequestOptions {
        RequestOptions {
            timeout_ms: 1_000_u64,
            total_timeout_ms: 0_u64,
            max_retries,
            base_delay_ms: 50_u16,
            max_delay_ms: 5_000_u16,
            jitter_factor: 0.1_f32,
            idempotent: true,
        }
    }

    pub fn no_timeout() -> RequestOptions {
        RequestOptions {
            timeout_ms: 0_u64,
            total_timeout_ms: 0_u64,
            max_retries: 0_u16,
            base_delay_ms: 50_u16,
            max_delay_ms: 5_000_u16,
            jitter_factor: 0.1_f32,
            idempotent: false,
        }
    }

    pub fn fast() -> RequestOptions {
        RequestOptions {
            timeout_ms: 100_u64,
            total_timeout_ms: 0_u64,
            max_retries: 2_u16,
            base_delay_ms: 10_u16,
            max_delay_ms: 100_u16,
            jitter_factor: 0.1_f32,
            idempotent: true,
        }
    }

    pub fn patient() -> RequestOptions {
        RequestOptions {
            timeout_ms: 10_000_u64,
            total_timeout_ms: 60_000_u64,
            max_retries: 5_u16,
            base_delay_ms: 500_u16,
            max_delay_ms: 10_000_u16,
            jitter_factor: 0.1_f32,
            idempotent: true,
        }
    }

    pub fn can_retry(&self, current_retry_count: u16) -> bool {
        self.idempotent && current_retry_count < self.max_retries
    }

    pub fn calculate_delay_ms(&self, attempt: u16) -> u64 {
        let mut delay: f64 = self.base_delay_ms as f64;
        let mut index: u16 = 0_u16;
        while index < attempt {
            delay *= 2.0_f64;
            if delay > self.max_delay_ms as f64 {
                delay = self.max_delay_ms as f64;
                break;
            }
            index += 1;
        }

        if delay > self.max_delay_ms as f64 {
            delay = self.max_delay_ms as f64;
        }

        if self.jitter_factor > 0.0_f32 {
            let jitter: f64 =
                delay * self.jitter_factor as f64 * legacy_rand_double(-0.5_f64, 0.5_f64);
            delay += jitter;
            if delay < 0.0_f64 {
                delay = 0.0_f64;
            }
        }

        delay as u64
    }

    pub fn is_total_timeout_exceeded(&self, elapsed_ms: u64) -> bool {
        self.total_timeout_ms > 0 && elapsed_ms >= self.total_timeout_ms
    }

    pub fn remaining_time_ms(&self, elapsed_ms: u64) -> u64 {
        if self.total_timeout_ms == 0 {
            return u64::MAX;
        }
        if elapsed_ms >= self.total_timeout_ms {
            return 0;
        }
        self.total_timeout_ms - elapsed_ms
    }
}
