#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stddef.h>

/* Count allocator traffic per thread; forward to the real allocator (jemalloc). */
static _Atomic long n_malloc, n_calloc, n_realloc, n_free, bytes_malloc;
static void *(*real_malloc)(size_t);
static void *(*real_calloc)(size_t, size_t);
static void *(*real_realloc)(void *, size_t);
static void (*real_free)(void *);
static _Atomic int init;

static void resolve(void) {
    if (atomic_exchange(&init, 1)) return;
    real_malloc = dlsym(RTLD_NEXT, "malloc");
    real_calloc = dlsym(RTLD_NEXT, "calloc");
    real_realloc = dlsym(RTLD_NEXT, "realloc");
    real_free = dlsym(RTLD_NEXT, "free");
    atomic_store(&init, 2);
}

void *malloc(size_t n) {
    if (atomic_load(&init) != 2) resolve();
    atomic_fetch_add(&n_malloc, 1);
    atomic_fetch_add(&bytes_malloc, (long)n);
    if (!real_malloc) return NULL;
    return real_malloc(n);
}
void *calloc(size_t a, size_t b) {
    if (atomic_load(&init) != 2) resolve();
    atomic_fetch_add(&n_calloc, 1);
    atomic_fetch_add(&bytes_malloc, (long)(a * b));
    if (!real_calloc) return NULL;
    return real_calloc(a, b);
}
void *realloc(void *p, size_t n) {
    if (atomic_load(&init) != 2) resolve();
    atomic_fetch_add(&n_realloc, 1);
    if (!real_realloc) return NULL;
    return real_realloc(p, n);
}
void free(void *p) {
    if (atomic_load(&init) != 2) resolve();
    atomic_fetch_add(&n_free, 1);
    if (real_free) real_free(p);
}
__attribute__((destructor)) static void alloccount_report(void) {
    fprintf(stderr, "ALLOCCOUNT malloc=%ld/%ldB calloc=%ld realloc=%ld free=%ld\n",
            (long)atomic_load(&n_malloc), (long)atomic_load(&bytes_malloc),
            (long)atomic_load(&n_calloc), (long)atomic_load(&n_realloc),
            (long)atomic_load(&n_free));
}
