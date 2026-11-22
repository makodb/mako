#define _GNU_SOURCE
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <string.h>

volatile long sink;

int main(int argc, char **argv) {
    long n = 3000000;
    if (argc < 2) return 2;
    if (!strcmp(argv[1], "gtod")) {
        struct timeval tv;
        for (long i = 0; i < n; i++) { gettimeofday(&tv, 0); sink += tv.tv_usec; }
    } else if (!strcmp(argv[1], "cgt_mono")) {
        struct timespec ts;
        for (long i = 0; i < n; i++) { clock_gettime(CLOCK_MONOTONIC, &ts); sink += ts.tv_nsec; }
    } else if (!strcmp(argv[1], "cgt_raw")) {
        struct timespec ts;
        for (long i = 0; i < n; i++) { clock_gettime(CLOCK_MONOTONIC_RAW, &ts); sink += ts.tv_nsec; }
    } else if (!strcmp(argv[1], "cgt_real")) {
        struct timespec ts;
        for (long i = 0; i < n; i++) { clock_gettime(CLOCK_REALTIME, &ts); sink += ts.tv_nsec; }
    } else {
        return 2;
    }
    printf("%ld %ld\n", n, sink);
    return 0;
}
