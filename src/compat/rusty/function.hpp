#pragma once

// Compatibility wrapper for third-party rusty-cpp.
// Some upstream versions use std::abort() without including <cstdlib>.
// Ensure the declaration is available, then include the upstream header.

#include <cstdlib>

#if defined(__has_include_next)
#if __has_include_next(<rusty/function.hpp>)
#include_next <rusty/function.hpp>
#else
#error "Expected upstream <rusty/function.hpp> after compat include path"
#endif
#else
#include <rusty/function.hpp>
#endif

