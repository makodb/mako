#pragma once

#include <stdio.h>
#include <assert.h>

// External safety annotations for system functions used in this module
// @external: {
//   backtrace: [unsafe, (void**, int) -> int]
//   backtrace_symbols: [unsafe, (void* const*, int) -> char**]
//   popen: [unsafe, (const char*, const char*) -> FILE*]
//   pclose: [unsafe, (FILE*) -> int]
//   memset: [unsafe, (void*, int, size_t) -> void*]
//   fprintf: [safe, (FILE*, const char*, ...) -> int]
//   fputc: [safe, (int, FILE*) -> int]
//   abort: [safe, () -> void]
// }

#ifndef likely
#define likely(x)   __builtin_expect((x), 1)
#endif // likely

#ifndef unlikely
#define unlikely(x)   __builtin_expect((x), 0)
#endif // unlikely

/**
 * Use assert() when the test is only intended for debugging.
 * Use verify() when the test is crucial for both debug and release binary.
 */
#ifdef NDEBUG
#define verify(expr) do { if (unlikely(!(expr))) { printf("  *** verify failed: %s at %s, line %d\n", #expr, __FILE__, __LINE__); ::rrr::print_stack_trace(); ::abort(); } } while (0)
#else
#define verify(expr) assert(expr)
#endif

namespace rrr {

// @unsafe - Uses backtrace functions and raw memory operations
// SAFETY: FILE* must be valid; backtrace functions are thread-safe
void print_stack_trace(FILE* fp = stderr) __attribute__((noinline));

} // namespace base
