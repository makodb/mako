/* srpc_rand.c — the per-thread PRNG seed store as plain C (Goal-0 C
 * demotion). Contract mirrors srpc_net.c / srpc_timing.c: no C++ type
 * crosses this boundary, and all range/scale logic stays in the DSL
 * statics on the C++ side.
 *
 * Everything here was rrr::randgen_* in misc/rand.cpp: pthread_key
 * plumbing with a raw `free` destructor, pthread_getspecific returning
 * void*, malloc, a raw `unsigned int*` seed, and rand_r over it. None
 * of it can be inline-Rust DSL, and none of it needs C++.
 */

#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>

#include "srpc_rand.h"

/* The cycle-counter read lives in srpc_timing.c (also plain C). */
uint64_t srpc_rdtsc(void);

#if defined(__APPLE__) || defined(__clang__)

static pthread_key_t srpc_rand_seed_key;
static pthread_once_t srpc_rand_seed_key_once = PTHREAD_ONCE_INIT;
static pthread_once_t srpc_rand_delete_key_once = PTHREAD_ONCE_INIT;

static void srpc_rand_create_key(void) {
    pthread_key_create(&srpc_rand_seed_key, free);
}

static void srpc_rand_delete_key(void) {
    pthread_key_delete(srpc_rand_seed_key);
}

/* Returns this thread's seed slot, allocating and rdtsc-seeding it on
 * first use. The slot is freed by the key destructor at thread exit. */
static unsigned int* srpc_rand_get_seed(void) {
    unsigned int* seed;
    pthread_once(&srpc_rand_seed_key_once, srpc_rand_create_key);
    seed = (unsigned int*)pthread_getspecific(srpc_rand_seed_key);
    if (seed == NULL) {
        seed = (unsigned int*)malloc(sizeof(unsigned int));
        if (seed == NULL) {
            abort();
        }
        pthread_setspecific(srpc_rand_seed_key, (void*)seed);
        *seed = (unsigned int)srpc_rdtsc();
    }
    return seed;
}

int srpc_rand_raw(void) {
    return rand_r(srpc_rand_get_seed());
}

void srpc_rand_destroy(void) {
    pthread_once(&srpc_rand_delete_key_once, srpc_rand_delete_key);
}

#else

/* Non-clang fallback: a plain thread-local seed, lazily rdtsc-seeded.
 * (C11 _Thread_local; the C++ side used `thread_local`.) */
static _Thread_local unsigned int srpc_rand_seed;
static _Thread_local int srpc_rand_seeded;

int srpc_rand_raw(void) {
    if (!srpc_rand_seeded) {
        srpc_rand_seed = (unsigned int)srpc_rdtsc();
        srpc_rand_seeded = 1;
    }
    return rand_r(&srpc_rand_seed);
}

void srpc_rand_destroy(void) {
    /* Nothing to tear down: no pthread key in this configuration. */
}

#endif
