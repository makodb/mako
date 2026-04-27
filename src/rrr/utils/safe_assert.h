#pragma once

// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>





inline void SAFE_ASSERT(bool expr) {
#ifdef NDEBUG
    if (!expr) {
        std::abort();
    }
#else
    assert(expr);
#endif
}
