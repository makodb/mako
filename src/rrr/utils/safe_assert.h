module;

#include <cassert>
#include <cstdlib>

export module rrr:utils.safe_assert;

export inline void SAFE_ASSERT(bool expr) {
#ifdef NDEBUG
    if (!expr) {
        std::abort();
    }
#else
    assert(expr);
#endif
}
