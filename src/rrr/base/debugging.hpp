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




// Defensive: erpc's `third-party/erpc/src/common.h` defines `likely(x)`
// and `unlikely(x)` as preprocessor macros.  When that header is
// transitively included before this one (e.g.
// `mako/lib/rrr_rpc_backend.h` pulls in `lib/configuration.h` →
// `common.h` → erpc, then `rrr/rrr.hpp` → us), the macro replacement
// turns our `inline bool likely(bool)` function into a syntax error.
// Stash the prior definitions, undef them while we declare our
// namespace-scoped functions, then restore on exit.
#pragma push_macro("likely")
#pragma push_macro("unlikely")
#undef likely
#undef unlikely

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

#pragma pop_macro("unlikely")
#pragma pop_macro("likely")
