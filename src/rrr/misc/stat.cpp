module;

#include <cstdint>

export module rrr.stat;

import std;

// @safe - POD AvgStat: int64 counters + simple arithmetic. No raw
// pointers, syscalls, or operator-overload chains.
export namespace rrr {

class AvgStat {
public:
    // Aggregate-style POD: no user-declared default constructor; the
    // implicit default zero-inits every field via the in-class
    // defaults below. This matches what the inline-Rust DSL emits and
    // mirrors `AvgStat::new_()` semantics (the named factory is just
    // a thin wrapper that returns `AvgStat{}`).
    int64_t n_stat_ = 0;
    int64_t sum_ = 0;
    int64_t avg_ = 0;
    int64_t max_ = 0;
    int64_t min_ = 0;

    // @safe - Rust-style factory matching the DSL `fn new() -> Self`
    // form. Existing `AvgStat stat;` callers continue to work via the
    // implicit aggregate default; new code should prefer `new_()`.
    static AvgStat new_() {
        return AvgStat{};
    }

    void sample(int64_t s = 1) {
        ++n_stat_;
        sum_ += s;
        avg_ = sum_ / n_stat_;
        max_ = s > max_ ? s : max_;
        min_ = s < min_ ? s : min_;
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
