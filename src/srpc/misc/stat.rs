// Canonical Rust source for the srpc.stat module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.
pub struct AvgStat {
    pub n_stat_: i64,
    pub sum_: i64,
    pub avg_: i64,
    pub max_: i64,
    pub min_: i64,
}

impl AvgStat {
    pub fn new() -> AvgStat {
        AvgStat {
            n_stat_: 0i64,
            sum_: 0i64,
            avg_: 0i64,
            max_: 0i64,
            min_: 0i64,
        }
    }

    pub fn sample(&mut self, s: i64) {
        self.n_stat_ += 1i64;
        self.sum_ += s;
        self.avg_ = self.sum_ / self.n_stat_;
        if s > self.max_ {
            self.max_ = s;
        }
        if s < self.min_ {
            self.min_ = s;
        }
    }

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

    pub fn peek(&self) -> AvgStat {
        AvgStat {
            n_stat_: self.n_stat_,
            sum_: self.sum_,
            avg_: self.avg_,
            max_: self.max_,
            min_: self.min_,
        }
    }

    pub fn avg(&self) -> i64 {
        self.avg_
    }
}
