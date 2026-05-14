#pragma once

// Compatibility shim for platforms that do not ship <immintrin.h> (e.g. Apple
// Silicon). Some third-party headers include <immintrin.h> unconditionally but
// guard actual intrinsic usage behind __SSE2__/__AVX__ checks.
//
// On x86/x86_64, defer to the system header.

#if defined(__i386__) || defined(__x86_64__)
#if defined(__has_include_next)
#if __has_include_next(<immintrin.h>)
#include_next <immintrin.h>
#endif
#elif defined(__has_include)
#if __has_include(<immintrin.h>)
#include <immintrin.h>
#endif
#endif
#endif

