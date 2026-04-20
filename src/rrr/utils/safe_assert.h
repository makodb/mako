module;


export module rrr:utils.safe_assert;

import <cassert>;
import <cstdlib>;

export inline void SAFE_ASSERT(bool expr) {
#ifdef NDEBUG
    if (!expr) {
        std::abort();
    }
#else
    assert(expr);
#endif
}
