// Canonical Rust source for the srpc.stat module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.
//
// Verus verification annotations are gated behind `cfg(verus)` -- the same cfg
// the rusty-cpp transpiler strips, so under plain rustc and rusty-cpp they are
// absent and this module compiles and lowers exactly as before. The `verify/`
// package includes this real file in place (`#[path]`, no copy) and forces
// `--cfg verus`, so `cargo verus verify` (run via scripts/verify_srpc.sh)
// activates and checks the `sample` first-sample contract here.
#[cfg(verus)]
use vstd::prelude::*;

#[cfg_attr(verus, verus_verify)]
pub struct AvgStat {
    pub n_stat_: i64,
    pub sum_: i64,
    pub avg_: i64,
    pub max_: i64,
    pub min_: i64,
}

impl AvgStat {
    #[cfg_attr(verus, verus_verify(external_body))]
    pub fn new() -> AvgStat {
        AvgStat {
            n_stat_: 0i64,
            sum_: 0i64,
            avg_: 0i64,
            max_: 0i64,
            min_: 0i64,
        }
    }

    // The first sample of a fresh stat becomes both the running min and max.
    // The former body seeded `max_`/`min_` from the zero-initialised fields
    // and only moved them with `if s > max_` / `if s < min_`, so an all-positive
    // stream left `min_` stuck at 0 and an all-negative stream left `max_` stuck
    // at 0 -- neither the true extremum. Verus proves the first-sample contract
    // below holds for this body and fails for the old one.
    #[cfg_attr(verus, verus_spec(
        requires
            old(self).n_stat_ == 0,
            old(self).sum_ == 0,
            s > i64::MIN,
            s < i64::MAX,
        ensures
            final(self).min_ == s,
            final(self).max_ == s,
    ))]
    pub fn sample(&mut self, s: i64) {
        self.n_stat_ += 1i64;
        self.sum_ += s;
        self.avg_ = self.sum_ / self.n_stat_;
        if self.n_stat_ == 1i64 {
            self.max_ = s;
            self.min_ = s;
        } else {
            if s > self.max_ {
                self.max_ = s;
            }
            if s < self.min_ {
                self.min_ = s;
            }
        }
    }

    #[cfg_attr(verus, verus_verify(external_body))]
    pub fn clear(&mut self) {
        self.n_stat_ = 0i64;
        self.sum_ = 0i64;
        self.avg_ = 0i64;
        self.max_ = 0i64;
        self.min_ = 0i64;
    }

    // peek/reset snapshot self via a populated struct literal. Now that
    // the cpp_ctor is gone, AvgStat is a plain aggregate and the
    // populated form `AvgStat { n_stat_: ..., ... }` lowers to a clean
    // C++ designated initializer `AvgStat{.n_stat_ = ...}`.
    #[cfg_attr(verus, verus_verify(external_body))]
    pub fn reset(&mut self) -> AvgStat {
        let stat: AvgStat = AvgStat {
            n_stat_: self.n_stat_,
            sum_: self.sum_,
            avg_: self.avg_,
            max_: self.max_,
            min_: self.min_,
        };
        self.clear();
        stat
    }

    #[cfg_attr(verus, verus_verify(external_body))]
    pub fn peek(&self) -> AvgStat {
        AvgStat {
            n_stat_: self.n_stat_,
            sum_: self.sum_,
            avg_: self.avg_,
            max_: self.max_,
            min_: self.min_,
        }
    }

    #[cfg_attr(verus, verus_verify(external_body))]
    pub fn avg(&self) -> i64 {
        self.avg_
    }
}
