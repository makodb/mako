/* srpc_timing.c — cycle-counter read as plain C (Goal-0 C demotion:
 * inline asm will never be Rust DSL; the crate's fiber seam already
 * uses the shared-kernel pattern this mirrors).
 */

#include <stdint.h>
#include <time.h>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

/* The base/misc.cpp variant: same x86 read, aarch64 counter via mrs,
 * and a plain 0 fallback (its callers only mix the value into stats
 * seeds — behavior preserved exactly from the C++ original). */
uint64_t srpc_rdtsc_raw(void) {
#if defined(__i386__) || defined(__x86_64__)
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
    uint64_t val;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
    return 0;
#endif
}

uint64_t srpc_rdtsc(void) {
#if defined(__APPLE__)
    return (uint64_t)mach_absolute_time();
#elif defined(__x86_64__) || defined(__i386__)
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__clang__) && __has_builtin(__builtin_readcyclecounter)
    return (uint64_t)__builtin_readcyclecounter();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec << 32) ^ (uint64_t)ts.tv_nsec;
#endif
}
