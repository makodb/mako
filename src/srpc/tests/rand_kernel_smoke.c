#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "misc/srpc_rand.h"
#include "misc/srpc_timing.h"

#define SMOKE "srpc_goal0_rand_kernel_smoke"

#if defined(__linux__) && defined(__GLIBC__)

/* Mako-local regression (upstream to srpc with misc/srpc_rand.c):
 * srpc_rand_destroy() must not delete a pthread key it never created. The old
 * kernel deleted its seed key even before any draw, while that key was still
 * its zero initializer, so it deleted pthread key 0. In Mako key 0 is
 * jemalloc's thread-state key; the next draw re-created key 0 with `free` as
 * its destructor, and a thread that registered its jemalloc state after that
 * freed jemalloc's static TLS at exit.
 *
 * This runs in a fork()ed child before any draw in the process. It makes key 0
 * live (creating it unless something such as jemalloc already owns it), calls
 * destroy, and checks that key 0 still works. glibc makes the old failure
 * deterministic: pthread_setspecific on a deleted key returns EINVAL, and
 * pthread_key_create hands out the lowest free index. */

static const pthread_key_t key0 = 0;
static int key0_owned;
static void* key0_value;
static volatile int key0_destructor_runs;
static void* volatile malloc_sink;

static void key0_destructor(void* value) {
    key0_destructor_runs += 1;
    free(value);
}

static void* draw_thread(void* result) {
    *(int*)result = srpc_rand_raw();
    return NULL;
}

/* Mallocs and exits. When this test owns key 0 it also stores per-thread
 * state there, as jemalloc does, and expects its own destructor at exit. */
static void* exiting_thread(void* result) {
    malloc_sink = malloc(64);
    free(malloc_sink);
    if (key0_owned) {
        void* state = malloc(32);
        *(int*)result = pthread_setspecific(key0, state);
        if (*(int*)result != 0) {
            free(state);
        }
    }
    return NULL;
}

static int make_key0_live(void) {
    pthread_key_t key;
    void* current = pthread_getspecific(key0);
    if (pthread_setspecific(key0, current) == 0) {
        /* Already live (jemalloc, a sanitizer runtime): keep its value. */
        key0_value = current;
        return 0;
    }
    if (pthread_key_create(&key, key0_destructor) != 0 || key != key0) {
        fprintf(stderr, SMOKE ": FAIL: could not create pthread key 0\n");
        return -1;
    }
    key0_owned = 1;
    key0_value = &key0_owned;
    if (pthread_setspecific(key0, key0_value) != 0) {
        fprintf(stderr, SMOKE ": FAIL: could not set the pthread key 0 it created\n");
        return -1;
    }
    return 0;
}

static int destroy_keeps_key0(void) {
    int failures = 0;
    int draw = -1;
    int set_result = 0;
    int rc;
    pthread_t thread;

    if (make_key0_live() != 0) {
        return 1;
    }
    printf(SMOKE ": pthread key 0 %s; checking srpc_rand_destroy() keeps it\n",
           key0_owned ? "created by this test" : "was already live");
    fflush(stdout);
    srpc_rand_destroy(); /* before any draw in this process */
    rc = pthread_setspecific(key0, key0_value);
    if (rc != 0) {
        fprintf(stderr,
                SMOKE ": FAIL: pthread_setspecific(key 0) after srpc_rand_destroy() "
                "returned %d (%s): destroy deleted a key srpc never created\n",
                rc, strerror(rc));
        failures += 1;
    } else if (pthread_getspecific(key0) != key0_value) {
        fprintf(stderr, SMOKE ": FAIL: pthread key 0 lost its value after srpc_rand_destroy()\n");
        failures += 1;
    }

    if (pthread_create(&thread, NULL, draw_thread, &draw) != 0 ||
        pthread_join(thread, NULL) != 0) {
        fprintf(stderr, SMOKE ": FAIL: could not run the draw thread\n");
        return 1;
    }
    if (draw < 0 || draw > RAND_MAX) {
        fprintf(stderr, SMOKE ": FAIL: draw after srpc_rand_destroy() returned %d\n", draw);
        failures += 1;
    }
    if (pthread_getspecific(key0) != key0_value) {
        fprintf(stderr,
                SMOKE ": FAIL: after a draw, pthread key 0 no longer holds its owner's value: "
                "the draw re-created key 0 for srpc's seed\n");
        failures += 1;
    }

    if (pthread_create(&thread, NULL, exiting_thread, &set_result) != 0 ||
        pthread_join(thread, NULL) != 0) {
        fprintf(stderr, SMOKE ": FAIL: could not run the exiting thread\n");
        return 1;
    }
    if (key0_owned && (set_result != 0 || key0_destructor_runs != 1)) {
        fprintf(stderr,
                SMOKE ": FAIL: at thread exit key 0's own destructor ran %d times "
                "(pthread_setspecific returned %d), expected once: key 0 has another owner\n",
                key0_destructor_runs, set_result);
        failures += 1;
    }
    return failures == 0 ? 0 : 1;
}

/* The second hazard of the old kernel: destroy after a draw deleted srpc's
 * own seed key while the key stayed in use. A key created afterwards got the
 * same index (glibc hands out the lowest free one), and the next draw on a
 * fresh thread stored srpc's seed under that key. A kernel that deletes the
 * key only once it was created still fails this check. */
static pthread_key_t probe_key;

static void* draw_and_read_probe(void* seen) {
    (void)srpc_rand_raw();
    if (seen != NULL) {
        *(void**)seen = pthread_getspecific(probe_key);
    }
    return NULL;
}

static int destroy_after_draw_keeps_key_index(void) {
    pthread_t thread;
    void* seen = NULL;
    if (pthread_create(&thread, NULL, draw_and_read_probe, NULL) != 0 ||
        pthread_join(thread, NULL) != 0) {
        fprintf(stderr, SMOKE ": FAIL: could not run the first draw thread\n");
        return 1;
    }
    srpc_rand_destroy(); /* after a draw: srpc's key exists now */
    if (pthread_key_create(&probe_key, NULL) != 0) {
        fprintf(stderr, SMOKE ": FAIL: could not create the probe key\n");
        return 1;
    }
    if (pthread_create(&thread, NULL, draw_and_read_probe, &seen) != 0 ||
        pthread_join(thread, NULL) != 0) {
        fprintf(stderr, SMOKE ": FAIL: could not run the second draw thread\n");
        return 1;
    }
    if (seen != NULL) {
        fprintf(stderr,
                SMOKE ": FAIL: a key created after srpc_rand_destroy() (index %u) received "
                "srpc's seed %p from a draw: destroy deleted srpc's key while it was in use\n",
                (unsigned)probe_key, seen);
        return 1;
    }
    return 0;
}

/* Runs one check in a fork()ed child, so each starts before any draw in the
 * process. The child must exit normally: a crash at thread exit is a failure. */
static int run_in_child(int (*check)(void), const char* what) {
    int status;
    pid_t child;
    fflush(stdout);
    fflush(stderr);
    child = fork();
    if (child < 0) {
        perror(SMOKE ": fork");
        return -1;
    }
    if (child == 0) {
        int result = check();
        fflush(stdout);
        exit(result);
    }
    if (waitpid(child, &status, 0) != child) {
        perror(SMOKE ": waitpid");
        return -1;
    }
    if (WIFSIGNALED(status)) {
        fprintf(stderr, SMOKE ": FAIL: %s child killed by signal %d\n", what, WTERMSIG(status));
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, SMOKE ": FAIL: %s child exited with status %d\n", what,
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return -1;
    }
    return 0;
}

/* Runs both checks, so a failure of the first still reports the second. */
static int check_destroy_in_child(void) {
    int before_draw = run_in_child(destroy_keeps_key0, "destroy check");
    int after_draw = run_in_child(destroy_after_draw_keeps_key_index, "destroy-after-draw check");
    return before_draw == 0 && after_draw == 0 ? 0 : -1;
}

#else

static int check_destroy_in_child(void) {
    printf(SMOKE ": skipped the pthread key checks (need glibc key semantics)\n");
    return 0;
}

#endif

int main(void) {
    int i;
    if (check_destroy_in_child() != 0) {
        return EXIT_FAILURE;
    }
    for (i = 0; i < 256; ++i) {
        int value = srpc_rand_raw();
        if (value < 0 || value > RAND_MAX) {
            return EXIT_FAILURE;
        }
    }
    srpc_rand_destroy();

    {
        uint64_t first = srpc_clock_monotonic_us();
        uint64_t realtime = srpc_clock_realtime_coarse_us();
        uint64_t wall = srpc_gettimeofday_us();
        srpc_sleep_us(1);
        uint64_t second = srpc_clock_monotonic_us();
        if (first == 0 || realtime == 0 || wall == 0 || second < first) {
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}
