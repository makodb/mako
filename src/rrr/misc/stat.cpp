module;

#include <stdint.h>

#include <rusty/rusty.hpp>

export module rrr.stat;

import std;

// @safe - POD AvgStat: int64 counters + simple arithmetic. No raw
// pointers, syscalls, or operator-overload chains.
export namespace rrr {

// Free helpers backing the per-sample mutations in `AvgStat::sample`.
// `n_stat` is post-increment (always >= 1), so division is safe.
// Authored as inline Rust DSL.
#if RUSTYCPP_RUST
fn avg_stat_compute_avg(sum: i64, n_stat: i64) -> i64 {
    sum / n_stat
}

fn avg_stat_new_max(current_max: i64, sample: i64) -> i64 {
    if sample > current_max {
        return sample;
    }
    current_max
}

fn avg_stat_new_min(current_min: i64, sample: i64) -> i64 {
    if sample < current_min {
        return sample;
    }
    current_min
}
#endif
/*RUSTYCPP:GEN-BEGIN id=stat.1 version=1 rust_sha256=45644572e67a6951f3c7b29b4ba1e398c71307098fa0f7d09eda33004ddca0c8*/
int64_t avg_stat_compute_avg(int64_t sum, int64_t n_stat);
int64_t avg_stat_new_max(int64_t current_max, int64_t sample);
int64_t avg_stat_new_min(int64_t current_min, int64_t sample);

int64_t avg_stat_compute_avg(int64_t sum, int64_t n_stat) {
    return rusty::detail::deref_if_pointer_like(sum) / rusty::detail::deref_if_pointer_like(n_stat);
}

int64_t avg_stat_new_max(int64_t current_max, int64_t sample) {
    if (rusty::detail::deref_if_pointer_like(sample) > rusty::detail::deref_if_pointer_like(current_max)) {
        return std::move(sample);
    }
    return std::move(current_max);
}

int64_t avg_stat_new_min(int64_t current_min, int64_t sample) {
    if (rusty::detail::deref_if_pointer_like(sample) < rusty::detail::deref_if_pointer_like(current_min)) {
        return std::move(sample);
    }
    return std::move(current_min);
}
/*RUSTYCPP:GEN-END id=stat.1*/

class AvgStat {
public:
    int64_t n_stat_;
    int64_t sum_;
    int64_t avg_;
    int64_t max_;
    int64_t min_;

    AvgStat(): n_stat_(0), sum_(0), avg_(0), max_(0), min_(0) {}

    void sample(int64_t s = 1) {
        ++n_stat_;
        sum_ += s;
        avg_ = avg_stat_compute_avg(sum_, n_stat_);
        max_ = avg_stat_new_max(max_, s);
        min_ = avg_stat_new_min(min_, s);
    }

    void clear() {
        n_stat_ = 0;
        sum_ = 0;
        avg_ = 0;
        max_ = 0;
        min_ = 0;
    }

    AvgStat reset() {
        AvgStat stat;
        stat = *this;
        clear();
        return stat;
    }

    AvgStat peek() {
        AvgStat result = *this;
        return result;
    }

    int64_t avg() {
        return avg_;
    }
};

} // export namespace rrr
