//! A small, self-contained PRNG.
//!
//! The C++ side used glibc's `rand_r`, whose sequence is neither
//! portable nor reproducible across libcs — fine there, useless as a
//! reference for a port that must behave identically under rustc and
//! under translation. This crate takes no dependencies, so rather than
//! reach for `rand`, it ships **xorshift64\***: a dozen lines of integer
//! math, good enough for the only things srpc randomises (backoff
//! jitter, load-balancer tie-breaks), and trivially translatable.
//!
//! The sequence is **frozen**: [`tests::sequence_is_pinned`] asserts
//! exact outputs for a known seed, so a change to the generator is a
//! deliberate act rather than an accident. This is not a
//! cryptographic generator and must not be used as one.

use super::time;
use std::cell::Cell;

/// xorshift64\* (Marsaglia/Vigna). State must never be zero — zero is a
/// fixed point that would emit zeros forever.
pub struct Rng {
    state: Cell<u64>,
}

/// Vigna's multiplier for the `*` variant.
const MULT: u64 = 0x2545_F491_4F6C_DD1D;

/// Substituted for a zero seed. Arbitrary but fixed (the golden-ratio
/// constant used by SplitMix/xoshiro seeding).
const NONZERO_FALLBACK: u64 = 0x9E37_79B9_7F4A_7C15;

impl Rng {
    /// Deterministic generator. Use this in tests and anywhere a
    /// reproducible sequence matters.
    pub fn with_seed(seed: u64) -> Rng {
        Rng {
            state: Cell::new(if seed == 0 { NONZERO_FALLBACK } else { seed }),
        }
    }

    /// Seeded from the wall clock, mixed so that two generators created
    /// in the same microsecond do not start in lockstep.
    pub fn from_clock() -> Rng {
        let us = time::wall_us();
        // SplitMix64 finalizer — spreads a low-entropy counter-ish seed
        // across all 64 bits.
        let mut z = us.wrapping_add(NONZERO_FALLBACK);
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^= z >> 31;
        Rng::with_seed(z)
    }

    /// Next raw 64-bit value.
    ///
    /// Takes `&self`: a generator is logically a source, and threading
    /// `&mut` through every caller (backoff calculators, tie-breakers)
    /// buys nothing here. Not thread-safe — give each thread its own.
    pub fn next_u64(&self) -> u64 {
        let mut x = self.state.get();
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        self.state.set(x);
        x.wrapping_mul(MULT)
    }

    pub fn next_u32(&self) -> u32 {
        (self.next_u64() >> 32) as u32
    }

    /// Uniform in `[0, 1)`. Uses the top 53 bits — the exact bits an
    /// f64 mantissa can hold, so every value is representable and the
    /// distribution has no gaps.
    pub fn next_f64(&self) -> f64 {
        ((self.next_u64() >> 11) as f64) / ((1u64 << 53) as f64)
    }

    /// Uniform in `[lo, hi)`. Returns `lo` for an empty, inverted or
    /// unordered (NaN) range rather than producing a nonsense value.
    pub fn next_f64_in(&self, lo: f64, hi: f64) -> f64 {
        // Spelled through partial_cmp so the NaN case is explicit: an
        // unordered bound yields `lo`, not a NaN delay.
        if hi.partial_cmp(&lo) != Some(std::cmp::Ordering::Greater) {
            return lo;
        }
        lo + self.next_f64() * (hi - lo)
    }

    /// Uniform in `[0, n)`. Zero for `n == 0`.
    ///
    /// Uses Lemire's multiply-shift rather than `%`, which biases the
    /// low values when `n` does not divide 2^64.
    pub fn next_below(&self, n: u64) -> u64 {
        if n == 0 {
            return 0;
        }
        ((self.next_u64() as u128 * n as u128) >> 64) as u64
    }
}

impl Default for Rng {
    fn default() -> Rng {
        Rng::from_clock()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The sequence is a frozen contract: a change to the generator
    /// must be deliberate, and the translated build must produce the
    /// same stream as the Rust one. These values were cross-checked
    /// against an independent implementation of the reference
    /// algorithm, so the test pins xorshift64* itself rather than
    /// whatever this file happens to compute.
    #[test]
    fn sequence_is_pinned() {
        let r = Rng::with_seed(1);
        let got = [r.next_u64(), r.next_u64(), r.next_u64(), r.next_u64()];
        assert_eq!(
            got,
            [
                5180492295206395165,
                12380297144915551517,
                13389498078930870103,
                5599127315341312413
            ],
            "xorshift64* stream for seed 1 changed"
        );
    }

    #[test]
    fn zero_seed_does_not_stick_at_zero() {
        let r = Rng::with_seed(0);
        let a = r.next_u64();
        let b = r.next_u64();
        assert_ne!(a, 0);
        assert_ne!(b, 0);
        assert_ne!(a, b);
    }

    #[test]
    fn f64_stays_in_unit_interval() {
        let r = Rng::with_seed(42);
        let mut i = 0;
        while i < 10000 {
            let v = r.next_f64();
            assert!((0.0..1.0).contains(&v), "{v} out of [0,1)");
            i += 1;
        }
    }

    #[test]
    fn f64_range_is_respected_and_degenerate_ranges_are_safe() {
        let r = Rng::with_seed(7);
        let mut i = 0;
        while i < 5000 {
            let v = r.next_f64_in(0.5, 1.5);
            assert!((0.5..1.5).contains(&v), "{v} out of [0.5,1.5)");
            i += 1;
        }
        assert_eq!(r.next_f64_in(2.0, 2.0), 2.0, "empty range yields lo");
        assert_eq!(r.next_f64_in(3.0, 1.0), 3.0, "inverted range yields lo");
    }

    #[test]
    fn next_below_is_bounded_and_covers_its_range() {
        let r = Rng::with_seed(99);
        let mut seen = [false; 8];
        let mut i = 0;
        while i < 4000 {
            let v = r.next_below(8);
            assert!(v < 8, "{v} out of range");
            seen[v as usize] = true;
            i += 1;
        }
        let mut k = 0;
        while k < 8 {
            assert!(seen[k], "value {k} never produced");
            k += 1;
        }
        assert_eq!(r.next_below(0), 0);
        assert_eq!(r.next_below(1), 0);
    }

    #[test]
    fn distinct_seeds_diverge() {
        let a = Rng::with_seed(1);
        let b = Rng::with_seed(2);
        assert_ne!(a.next_u64(), b.next_u64());
    }

    #[test]
    fn mean_is_roughly_centred() {
        // Not a statistical test — a smoke check that the stream is not
        // stuck in one half of the interval.
        let r = Rng::with_seed(2024);
        let mut sum = 0.0;
        let n = 20000;
        let mut i = 0;
        while i < n {
            sum += r.next_f64();
            i += 1;
        }
        let mean = sum / (n as f64);
        assert!((0.47..0.53).contains(&mean), "mean {mean} looks skewed");
    }
}
