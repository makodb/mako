module;

#include <cstdint>

#include <rusty/rusty.hpp>

export module rrr.stat;

import std;

// @safe - POD AvgStat: int64 counters + simple arithmetic. No raw
// pointers, syscalls, or operator-overload chains.
//
// Authored as inline Rust DSL; the transpiler regenerates the
// matching `/*RUSTYCPP:GEN-BEGIN ... END*/` block below. Fields stay
// plain `i64` (not `Cell<i64>`) because the original C++ used plain
// `int64_t` and mutated through non-const methods; `&mut self`
// methods preserve that exactly.
export namespace rrr {

#if RUSTYCPP_RUST
struct AvgStat {
    n_stat_: i64,
    sum_: i64,
    avg_: i64,
    max_: i64,
    min_: i64,
}

impl AvgStat {
    #[cpp_ctor]
    fn new() -> AvgStat {
        AvgStat {
            n_stat_: 0i64,
            sum_: 0i64,
            avg_: 0i64,
            max_: 0i64,
            min_: 0i64,
        }
    }

    fn sample(&mut self, s: i64) {
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

    fn clear(&mut self) {
        self.n_stat_ = 0i64;
        self.sum_ = 0i64;
        self.avg_ = 0i64;
        self.max_ = 0i64;
        self.min_ = 0i64;
    }

    // peek/reset construct snapshots via `AvgStat {}` (empty struct
    // literal, emits as value-init `AvgStat{}` which calls the
    // #[cpp_ctor]-generated default ctor) + field-assign. We avoid the
    // populated form `AvgStat { n_stat_: ..., ... }` because that maps
    // to a C++ *designated initializer* `AvgStat{.n_stat_ = ..}` which
    // requires the struct to be an aggregate; the cpp_ctor disqualifies
    // it. The literal form is only special-cased to initializer-list
    // inside the cpp_ctor body itself.
    fn reset(&mut self) -> AvgStat {
        let mut stat: AvgStat = AvgStat {};
        stat.n_stat_ = self.n_stat_;
        stat.sum_ = self.sum_;
        stat.avg_ = self.avg_;
        stat.max_ = self.max_;
        stat.min_ = self.min_;
        self.clear();
        stat
    }

    fn peek(&self) -> AvgStat {
        let mut result: AvgStat = AvgStat {};
        result.n_stat_ = self.n_stat_;
        result.sum_ = self.sum_;
        result.avg_ = self.avg_;
        result.max_ = self.max_;
        result.min_ = self.min_;
        result
    }

    fn avg(&self) -> i64 {
        self.avg_
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=stat.1 version=1 rust_sha256=2e414d520e6cd26e30af6339a10fa4007293ade2ce9a185da8851796987b3a2e*/
struct AvgStat;

struct AvgStat {
    int64_t n_stat_;
    int64_t sum_;
    int64_t avg_;
    int64_t max_;
    int64_t min_;

    AvgStat();
    void sample(int64_t s);
    void clear();
    AvgStat reset();
    AvgStat peek() const;
    int64_t avg() const;
};


AvgStat::AvgStat()
    : n_stat_(static_cast<int64_t>(0))
    , sum_(static_cast<int64_t>(0))
    , avg_(static_cast<int64_t>(0))
    , max_(static_cast<int64_t>(0))
    , min_(static_cast<int64_t>(0))
{}

void AvgStat::sample(int64_t s) {
    this->n_stat_ += static_cast<int64_t>(1);
    this->sum_ += s;
    this->avg_ = rusty::detail::deref_if_pointer_like(this->sum_) / rusty::detail::deref_if_pointer_like(this->n_stat_);
    if (rusty::detail::deref_if_pointer_like(s) > rusty::detail::deref_if_pointer_like(this->max_)) {
        this->max_ = std::move(s);
    }
    if (rusty::detail::deref_if_pointer_like(s) < rusty::detail::deref_if_pointer_like(this->min_)) {
        this->min_ = std::move(s);
    }
}

void AvgStat::clear() {
    this->n_stat_ = static_cast<int64_t>(0);
    this->sum_ = static_cast<int64_t>(0);
    this->avg_ = static_cast<int64_t>(0);
    this->max_ = static_cast<int64_t>(0);
    this->min_ = static_cast<int64_t>(0);
}

AvgStat AvgStat::reset() {
    AvgStat stat = AvgStat{};
    stat.n_stat_ = this->n_stat_;
    stat.sum_ = this->sum_;
    stat.avg_ = this->avg_;
    stat.max_ = this->max_;
    stat.min_ = this->min_;
    this->clear();
    return std::move(stat);
}

AvgStat AvgStat::peek() const {
    AvgStat result = AvgStat{};
    result.n_stat_ = this->n_stat_;
    result.sum_ = this->sum_;
    result.avg_ = this->avg_;
    result.max_ = this->max_;
    result.min_ = this->min_;
    return std::move(result);
}

int64_t AvgStat::avg() const {
    return this->avg_;
}
/*RUSTYCPP:GEN-END id=stat.1*/

} // export namespace rrr
