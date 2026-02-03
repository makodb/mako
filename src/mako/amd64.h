#ifndef _AMD64_H_
#define _AMD64_H_

#include "macros.h"
#include <stdint.h>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

// The original codebase targeted x86_64 Linux. To support macOS (including
// Apple Silicon), provide portable fallbacks for cpu-relax and cycle counter
// operations. Linux/x86 keeps the original fast paths.

// @unsafe - emits raw pause instruction (no safety guarantees)
inline ALWAYS_INLINE void
nop_pause()
{
#if defined(__i386__) || defined(__x86_64__)
  __asm volatile("pause" : :);
#elif defined(__aarch64__) || defined(__arm__)
  __asm volatile("yield" : : : "memory");
#else
  __asm volatile("" : : : "memory");
#endif
}

// @unsafe - reads CPU timestamp counter directly
inline ALWAYS_INLINE uint64_t
rdtsc(void)
{
#if defined(__APPLE__)
  return static_cast<uint64_t>(mach_absolute_time());
#elif defined(__i386__) || defined(__x86_64__)
  uint32_t hi, lo;
  __asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)lo)|(((uint64_t)hi)<<32);
#elif defined(__clang__) && __has_builtin(__builtin_readcyclecounter)
  return static_cast<uint64_t>(__builtin_readcyclecounter());
#elif defined(__aarch64__) || defined(__arm__)
  uint64_t tsc;
  __asm volatile("mrs %0, cntvct_el0" : "=r"(tsc));
  return tsc;
#else
  return 0;
#endif
}

#endif /* _AMD64_H_ */
