/* Native cycle-counter, clock, and scheduling operations shared by the
 * rustc and generated C++ builds. Canonical Rust owns timing policy.
 */

#include "srpc_timing.h"

#include <time.h>
#include <sys/time.h>

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
#if defined(__x86_64__) || defined(__i386__)
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

/* Microseconds since an unspecified monotonic origin, read through the same
 * C ABI operation by canonical Rust and its generated C++ modules. */
uint64_t srpc_clock_monotonic_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000) +
           (uint64_t)ts.tv_nsec / UINT64_C(1000);
}

uint64_t srpc_clock_realtime_coarse_us(void) {
    struct timespec ts;
#if defined(CLOCK_REALTIME_COARSE)
    clock_gettime(CLOCK_REALTIME_COARSE, &ts);
#else
    clock_gettime(CLOCK_REALTIME, &ts);
#endif
    return (uint64_t)ts.tv_sec * UINT64_C(1000000) +
           (uint64_t)ts.tv_nsec / UINT64_C(1000);
}

uint64_t srpc_gettimeofday_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * UINT64_C(1000000) +
           (uint64_t)tv.tv_usec;
}

void srpc_sleep_us(uint64_t microseconds) {
    struct timespec ts;
    ts.tv_sec = (time_t)(microseconds / UINT64_C(1000000));
    ts.tv_nsec = (long)((microseconds % UINT64_C(1000000)) * UINT64_C(1000));
    nanosleep(&ts, NULL);
}

/* Copy the platform calendar layout into six integer fields. Timestamp
 * digit formatting and separators are owned by canonical base/logging.rs. */
int32_t srpc_local_calendar_fields(int32_t* fields) {
    time_t seconds_since_epoch = time(NULL);
    struct tm local_calendar;
    if (localtime_r(&seconds_since_epoch, &local_calendar) == NULL) {
        return -1;
    }
    fields[0] = local_calendar.tm_year + 1900;
    fields[1] = local_calendar.tm_mon + 1;
    fields[2] = local_calendar.tm_mday;
    fields[3] = local_calendar.tm_hour;
    fields[4] = local_calendar.tm_min;
    fields[5] = local_calendar.tm_sec;
    return 0;
}

/* Spin-wait hint: x86 pause / arm yield / no-op. (Goal-0 C demotion;
 * was the inline-asm srpc::cpu_pause in base/threading.cpp.) */
void srpc_cpu_pause(void) {
#if defined(__i386__) || defined(__x86_64__)
    __asm__ __volatile__("pause");
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}
