//! Legacy `rrr.rand` random-number helpers.
//!
//! This owner deliberately delegates every draw to `srpc_rand_raw`, the
//! existing C `rand_r` kernel in `src/rrr/misc/srpc_rand.c`.  The crate's
//! `base::rand::Rng` is xorshift64* and is therefore not a compatible
//! substitute: using it here would change the per-thread stream consumed by
//! RPC load balancing, retry jitter, and the benchmark workloads.
//!
//! The pre-DSL class exposed three source conveniences that the valid-Rust
//! owner cannot express exactly: default arguments on `rand`/`rand_double`,
//! `rand_str`, and a `percentage_true(double)` overload.  The conversion that
//! preceded this owner rewrote every default-argument caller explicitly and
//! removed the latter two functions after a closed-world caller audit.  This
//! file preserves the resulting live seven-method surface; it does not invent
//! replacement overloads or silently select a different PRNG.
//!
//! The inline-DSL transition also exposed its `randgen_*` implementation
//! helpers and `RandWeightVec` alias from an `export namespace`. They have no
//! repository consumers and were absent from the original public class API,
//! so this owner deliberately does not perpetuate those conversion seams.

#![allow(unsafe_code)]

use cpp::std as cpp_std;

const LEGACY_RAND_MAX: f64 = 2_147_483_647.0_f64;

mod legacy_rand_ffi {
    extern "C" {
        pub(super) fn srpc_rand_raw() -> i32;
        pub(super) fn srpc_rand_destroy();
    }
}

fn next_raw() -> i32 {
    unsafe { legacy_rand_ffi::srpc_rand_raw() }
}

/// All-static compatibility surface for the historical C++ class.
///
/// A zero-field Rust struct is a ZST, while the generated empty C++ struct has
/// the required C++ object layout (`sizeof == alignof == 1`).  Adding a dummy
/// Rust field would alter the generated aggregate surface without improving
/// the C++ ABI.
pub struct RandomGenerator {}

impl RandomGenerator {
    /// Draw uniformly using the historical inclusive modulo reduction.
    pub fn rand(min: i32, max: i32) -> i32 {
        assert!(max >= min);
        let raw: i32 = next_raw();

        // `rand(0, RAND_MAX)` is a live call.  Its mathematical width is
        // 2^31, which the old signed-int expression represented through the
        // target's two's-complement wrap.  Spell that wrap explicitly so the
        // native debug build and generated C++ consume the stream identically.
        let width: i32 = max.wrapping_sub(min).wrapping_add(1_i32);
        (raw % width).wrapping_add(min)
    }

    /// Scale one raw draw to the historical inclusive floating-point range.
    pub fn rand_double(min: f64, max: f64) -> f64 {
        if max == min {
            return min;
        }
        assert!(max > min);
        let raw: i32 = next_raw();
        ((raw as f64) / (LEGACY_RAND_MAX / (max - min))) + min
    }

    /// Decimal formatting padded or right-truncated to exactly `length`.
    pub fn int2str_n(i: i32, length: i32) -> cpp_std::string {
        assert!(length >= 0_i32, "int2str_n length must be nonnegative");
        let text: cpp_std::string = format!("{}", i);
        let text_len: i32 = text.len() as i32;

        if text_len < length {
            let mut prefix: cpp_std::string = format!("");
            let mut remaining: i32 = length - text_len;
            while remaining > 0_i32 {
                prefix = format!("{}0", prefix);
                remaining -= 1_i32;
            }
            return format!("{}{}", prefix, text);
        }

        if text_len > length {
            let bytes = text.as_bytes();
            let mut index: usize = text.len() - (length as usize);
            let mut suffix: cpp_std::string = format!("");
            while index < text.len() {
                suffix = format!("{}{}", suffix, bytes[index] as char);
                index += 1_usize;
            }
            return suffix;
        }

        text
    }

    /// Integer percentage predicate (`p` successes in 100 draws).
    pub fn percentage_true(p: i32) -> bool {
        RandomGenerator::rand(0_i32, 99_i32) < p
    }

    /// TPC-C NURand with the historical process constant (which is zero).
    pub fn nu_rand(a: i32, x: i32, y: i32) -> i32 {
        let r1: i32 = RandomGenerator::rand(0_i32, a);
        let r2: i32 = RandomGenerator::rand(x, y);
        ((r1 | r2) % ((y - x) + 1_i32)) + x
    }

    /// Select an index using cumulative weights and the historical `<=`
    /// boundary rule.  An empty vector returns `UINT_MAX`, matching `--i` on
    /// the old unsigned-zero index without consuming a PRNG value.
    pub fn weighted_select(weight_vector: &cpp_std::vector<f64>) -> u32 {
        let mut sum: f64 = 0.0_f64;
        let mut index: u32 = 0_u32;
        while (index as usize) < weight_vector.len() {
            sum += weight_vector[index as usize];
            index = index.wrapping_add(1_u32);
        }

        let draw: f64 = RandomGenerator::rand_double(0.0_f64, sum);
        let mut stage_sum: f64 = 0.0_f64;
        let mut selected: u32 = 0_u32;
        while (selected as usize) < weight_vector.len() {
            stage_sum += weight_vector[selected as usize];
            if draw <= stage_sum {
                return selected;
            }
            selected = selected.wrapping_add(1_u32);
        }
        selected.wrapping_sub(1_u32)
    }

    /// Tear down the pthread-keyed seed store on platforms that use it.
    pub fn destroy() {
        unsafe { legacy_rand_ffi::srpc_rand_destroy() }
    }
}

// Cargo-only definitions for the reserved `cpp::std` import.  The C++
// consumer suppresses this shim and resolves `string` and `vector` through a
// fail-closed module index, retaining the exact historical STL signatures.
#[allow(dead_code)]
mod cpp {
    pub mod std {
        #[allow(non_camel_case_types)]
        pub type string = String;

        #[allow(non_camel_case_types)]
        pub type vector<T> = Vec<T>;
    }
}
