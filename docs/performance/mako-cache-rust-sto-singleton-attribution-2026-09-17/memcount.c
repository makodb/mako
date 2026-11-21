#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

/* Count memmove/memcpy/memset calls and their size distribution per thread. */
static _Atomic long mm_calls, mm_bytes, mc_calls, mc_bytes, ms_calls, ms_bytes;
static _Atomic long mm_small, mm_mid, mm_big, mm_huge; /* <=16, <=64, <=256, >256 */
static void *(*real_mm)(void *, const void *, size_t);
static void *(*real_mc)(void *, const void *, size_t);
static void *(*real_ms)(void *, int, size_t);
static _Atomic int init;

static void resolve(void) {
    if (atomic_exchange(&init, 1)) return;
    real_mm = dlsym(RTLD_NEXT, "memmove");
    real_mc = dlsym(RTLD_NEXT, "memcpy");
    real_ms = dlsym(RTLD_NEXT, "memset");
    atomic_store(&init, 2);
}

static void note_size(size_t n) {
    if (n <= 16) atomic_fetch_add(&mm_small, 1);
    else if (n <= 64) atomic_fetch_add(&mm_mid, 1);
    else if (n <= 256) atomic_fetch_add(&mm_big, 1);
    else atomic_fetch_add(&mm_huge, 1);
}

void *memmove(void *d, const void *s, size_t n) {
    if (atomic_load(&init) != 2) resolve();
    atomic_fetch_add(&mm_calls, 1);
    atomic_fetch_add(&mm_bytes, (long)n);
    note_size(n);
    if (real_mm) return real_mm(d, s, n);
    unsigned char *dd = d; const unsigned char *ss = s;
    if (dd < ss) { for (size_t i = 0; i < n; i++) dd[i] = ss[i]; }
    else { for (size_t i = n; i > 0; i--) dd[i-1] = ss[i-1]; }
    return d;
}

void *memcpy(void *d, const void *s, size_t n) {
    if (atomic_load(&init) != 2) resolve();
    atomic_fetch_add(&mc_calls, 1);
    atomic_fetch_add(&mc_bytes, (long)n);
    note_size(n);
    if (real_mc) return real_mc(d, s, n);
    return memmove(d, s, n);
}

void *memset(void *d, int c, size_t n) {
    if (atomic_load(&init) != 2) resolve();
    atomic_fetch_add(&ms_calls, 1);
    atomic_fetch_add(&ms_bytes, (long)n);
    if (real_ms) return real_ms(d, c, n);
    unsigned char *dd = d;
    for (size_t i = 0; i < n; i++) dd[i] = (unsigned char)c;
    return d;
}

__attribute__((destructor)) static void memcount_report(void) {
    fprintf(stderr,
            "MEMCOUNT memmove=%ld/%ldB memcpy=%ld/%ldB memset=%ld/%ldB sizes<=16=%ld <=64=%ld <=256=%ld >256=%ld\n",
            (long)atomic_load(&mm_calls), (long)atomic_load(&mm_bytes),
            (long)atomic_load(&mc_calls), (long)atomic_load(&mc_bytes),
            (long)atomic_load(&ms_calls), (long)atomic_load(&ms_bytes),
            (long)atomic_load(&mm_small), (long)atomic_load(&mm_mid),
            (long)atomic_load(&mm_big), (long)atomic_load(&mm_huge));
}
