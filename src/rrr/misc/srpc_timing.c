/* srpc_timing.c — cycle-counter read as plain C (Goal-0 C demotion:
 * inline asm will never be Rust DSL; the crate's fiber seam already
 * uses the shared-kernel pattern this mirrors).
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

/* Microseconds since an unspecified monotonic origin.  This is the plain-C
 * kernel seam used by canonical Rust modules that cannot call the C++
 * rusty::sys::time wrapper while they are also compiled by rustc. */
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

/* Wall-clock timestamp "YYYY-MM-DD HH:MM:SS.mmm" into a caller buffer of
 * at least 24 bytes (23 chars + NUL). Plain C: time/localtime_r/
 * gettimeofday syscalls + raw byte writing (Goal-0 C demotion; was
 * rrr::time_now_str + its make_int digit writer in base/misc.cpp). */
static void srpc_write_int(char* str, int val, int digits) {
    char* p = str + digits;
    for (int i = 0; i < digits; i++) {
        int d = val % 10;
        val /= 10;
        p--;
        *p = (char)('0' + d);
    }
}

void srpc_time_now_str(char* now) {
    time_t seconds_since_epoch = time(NULL);
    struct tm local_calendar;
    localtime_r(&seconds_since_epoch, &local_calendar);
    srpc_write_int(now, local_calendar.tm_year + 1900, 4);
    now[4] = '-';
    srpc_write_int(now + 5, local_calendar.tm_mon + 1, 2);
    now[7] = '-';
    srpc_write_int(now + 8, local_calendar.tm_mday, 2);
    now[10] = ' ';
    srpc_write_int(now + 11, local_calendar.tm_hour, 2);
    now[13] = ':';
    srpc_write_int(now + 14, local_calendar.tm_min, 2);
    now[16] = ':';
    srpc_write_int(now + 17, local_calendar.tm_sec, 2);
    now[19] = '.';
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        srpc_write_int(now + 20, (int)(tv.tv_usec / 1000), 3);
    }
    now[23] = '\0';
}

/* Spin-wait hint: x86 pause / arm yield / no-op. (Goal-0 C demotion;
 * was the inline-asm rrr::cpu_pause in base/threading.cpp.) */
void srpc_cpu_pause(void) {
#if defined(__i386__) || defined(__x86_64__)
    __asm__ __volatile__("pause");
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}
