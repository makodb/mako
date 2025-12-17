#pragma once

// @safe - all operations are pure arithmetic
struct Rand {
    typedef uint32_t result_type;
    result_type x;
    // @safe - simple value assignment
    Rand(result_type a, result_type = 0)
        : x(a) {
    }
    // @safe - pure arithmetic operations
    result_type operator()() {
        x = 1103515245 * x + 12345;
        return x & 0x7fffffff;
    }
    // @safe - constexpr return
    static constexpr result_type min() {
        return 0;
    }
    // @safe - constexpr return
    static constexpr result_type max() {
        return 0x7fffffff;
    }
};
