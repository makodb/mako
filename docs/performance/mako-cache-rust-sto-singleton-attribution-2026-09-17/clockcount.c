#define _GNU_SOURCE
#include <sys/time.h>
#include <time.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/syscall.h>

#define MAXT 64
struct th { pid_t tid; long gtod; long cgt; };
static struct th ths[MAXT];
static int nth;
static _Atomic int lock;
static int (*real_gtod)(struct timeval *, void *);
static int (*real_cgt)(clockid_t, struct timespec *);

static struct th *slot(void) {
    pid_t t = (pid_t)syscall(SYS_gettid);
    while (atomic_exchange(&lock, 1)) {}
    for (int i = 0; i < nth; i++) if (ths[i].tid == t) { atomic_store(&lock, 0); return &ths[i]; }
    if (nth < MAXT) { ths[nth].tid = t; struct th *r = &ths[nth++]; atomic_store(&lock, 0); return r; }
    atomic_store(&lock, 0);
    return &ths[MAXT - 1];
}

int gettimeofday(struct timeval *tv, void *tz) {
    if (!real_gtod) real_gtod = dlsym(RTLD_NEXT, "gettimeofday");
    slot()->gtod++;
    return real_gtod(tv, tz);
}
int clock_gettime(clockid_t c, struct timespec *ts) {
    if (!real_cgt) real_cgt = dlsym(RTLD_NEXT, "clock_gettime");
    slot()->cgt++;
    return real_cgt(c, ts);
}
__attribute__((destructor)) static void clockcount_report(void) {
    for (int i = 0; i < nth; i++)
        fprintf(stderr, "CLOCKCOUNT tid=%d gettimeofday=%ld clock_gettime=%ld\n",
                (int)ths[i].tid, ths[i].gtod, ths[i].cgt);
}
