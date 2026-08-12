/* srpc_timing.h -- plain-C timing kernel boundary. */
#ifndef SRPC_TIMING_H
#define SRPC_TIMING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t srpc_rdtsc_raw(void);
uint64_t srpc_rdtsc(void);
uint64_t srpc_clock_monotonic_us(void);
void srpc_time_now_str(char* now);
void srpc_cpu_pause(void);

#ifdef __cplusplus
}
#endif

#endif
