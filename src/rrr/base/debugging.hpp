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




namespace rrr {

// @unsafe - Uses backtrace functions and raw memory operations
// SAFETY: FILE* must be valid; backtrace functions are thread-safe
void print_stack_trace(FILE* fp = stderr) __attribute__((noinline));

[[nodiscard]] inline bool likely(bool value) noexcept {
  return __builtin_expect(value, true);
}

[[nodiscard]] inline bool unlikely(bool value) noexcept {
  return __builtin_expect(value, false);
}

/**
 * Use assert() when the test is only intended for debugging.
 * Use verify() when the test is crucial for both debug and release binary.
 */
template <typename Expr>
inline void verify(const Expr& expr,
                   const std::source_location& loc = std::source_location::current()) {
  const bool ok = static_cast<bool>(expr);
#ifdef NDEBUG
  if (unlikely(!ok)) {
    fprintf(stderr, "  *** verify failed at %s, line %u\n", loc.file_name(), loc.line());
    print_stack_trace(stderr);
    std::abort();
  }
#else
  assert(ok);
#endif
}

} // namespace rrr
