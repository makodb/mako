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
#endif
/*RUSTYCPP:GEN-BEGIN id=stat.1 version=1 rust_sha256=516f78fcce6fbcc1a9568b2b00733af846649ccdf89a29809516613e2e1abdf2*/
struct AvgStat;

struct AvgStat {
    int64_t n_stat_;
    int64_t sum_;
    int64_t avg_;
    int64_t max_;
    int64_t min_;

    static AvgStat new_();
    void sample(int64_t s);
    void clear();
    AvgStat reset();
    AvgStat peek() const;
    int64_t avg() const;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};


AvgStat AvgStat::new_() {
    return AvgStat{.n_stat_ = static_cast<int64_t>(0), .sum_ = static_cast<int64_t>(0), .avg_ = static_cast<int64_t>(0), .max_ = static_cast<int64_t>(0), .min_ = static_cast<int64_t>(0)};
}

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
    AvgStat stat = AvgStat{.n_stat_ = this->n_stat_, .sum_ = this->sum_, .avg_ = this->avg_, .max_ = this->max_, .min_ = this->min_};
    this->clear();
    return std::move(stat);
}

AvgStat AvgStat::peek() const {
    return AvgStat{.n_stat_ = this->n_stat_, .sum_ = this->sum_, .avg_ = this->avg_, .max_ = this->max_, .min_ = this->min_};
}

int64_t AvgStat::avg() const {
    return this->avg_;
}
/*RUSTYCPP:GEN-END id=stat.1*/

} // export namespace rrr
