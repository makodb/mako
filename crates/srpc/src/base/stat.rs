//! Running integer statistics.
//!
//! This is the Rust source of truth for the legacy `rrr.stat` module. Its
//! zero-biased minimum and maximum are intentional compatibility behavior:
//! positive-only samples retain a minimum of zero, and negative-only samples
//! retain a maximum of zero.

/// Running integer average and extrema, matching the legacy `AvgStat` layout.
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
            n_stat_: 0,
            sum_: 0,
            avg_: 0,
            max_: 0,
            min_: 0,
        }
    }

    pub fn sample(&mut self, s: i64) {
        self.n_stat_ += 1;
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
        self.n_stat_ = 0;
        self.sum_ = 0;
        self.avg_ = 0;
        self.max_ = 0;
        self.min_ = 0;
    }

    pub fn reset(&mut self) -> AvgStat {
        let stat = self.peek();
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
