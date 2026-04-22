module;

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>


export module rrr:utils.safe_assert;

import std;


export inline void SAFE_ASSERT(bool expr) {
#ifdef NDEBUG
    if (!expr) {
        std::abort();
    }
#else
    assert(expr);
#endif
}
