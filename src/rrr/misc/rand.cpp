#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <rusty/rusty.hpp>

#include <pthread.h>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

#include "rand.hpp"
#include "../rrr.hpp"

// `import std;` after every textual `#include` — libc++ rejects
// textual STL emitted after the import. Conditional `<mach/...>` is
// platform-specific and doesn't pull in C++ STL, so it's safe.
import std;

// @external: {
//   pthread_key_create: [unsafe],
//   pthread_key_delete: [unsafe],
//   pthread_once: [unsafe],
//   pthread_getspecific: [unsafe],
//   pthread_setspecific: [unsafe],
//   malloc: [unsafe],
//   free: [unsafe],
//   rand_r: [unsafe],
//   std::to_string: [unsafe]
// }

// NOTE: This file is a random number generator. It uses pthread functions,
// malloc/free, inline assembly (rdtsc), and rand_r. All low-level operations
// are marked @unsafe.

namespace rrr {

#if defined(__APPLE__) || defined(__clang__)
pthread_key_t RandomGenerator::seed_key_;
pthread_once_t RandomGenerator::seed_key_once_ = PTHREAD_ONCE_INIT;
pthread_once_t RandomGenerator::delete_key_once_ = PTHREAD_ONCE_INIT;

// @unsafe - Uses pthread_key_create system call
void RandomGenerator::create_key() {
    pthread_key_create(&seed_key_, free);  // @unsafe
}

// @unsafe - Uses pthread_key_delete system call
void RandomGenerator::delete_key() {
    pthread_key_delete(seed_key_);  // @unsafe
}

// @unsafe - Uses pthread functions and malloc
unsigned int *RandomGenerator::get_seed() {
    pthread_once(&seed_key_once_, create_key);  // @unsafe
    unsigned int *seed = (unsigned int *)pthread_getspecific(seed_key_);  // @unsafe
    if (seed == NULL) {
        seed = (unsigned int *)malloc(sizeof(unsigned int));  // @unsafe
        pthread_setspecific(seed_key_, (void *)seed);  // @unsafe
        *seed = rdtsc();  // @unsafe
    }
    return seed;
}
#else // not __APPLE__
thread_local unsigned int RandomGenerator::seed_ = rdtsc();
#endif // __APPLE__

int RandomGenerator::nu_constant = 0;

// @unsafe - Uses rand_r system call
int RandomGenerator::rand(int min, int max) {
    verify(max >= min);  // @unsafe
#if defined(__APPLE__) || defined(__clang__)
    unsigned int *seed = get_seed();  // @unsafe
    int r = rand_r(seed);  // @unsafe
#else // not __APPLE__
    int r = rand_r(&seed_);  // @unsafe
#endif // __APPLE__
    return (r % (max - min + 1)) + min;
}

// @unsafe - Uses rand_r system call
double RandomGenerator::rand_double(double min, double max) {
    if (max == min)
        return min;
    verify(max > min);  // @unsafe
#if defined(__APPLE__) || defined(__clang__)
    unsigned int *seed = get_seed();  // @unsafe
    int r = rand_r(seed);  // @unsafe
#else // not __APPLE__
    int r = rand_r(&seed_);  // @unsafe
#endif // __APPLE__
    return ((double)r) / ((double)RAND_MAX / (max - min)) + min;
}

// @unsafe - Uses rand_r and std::to_string
std::string RandomGenerator::rand_str(int length) {
#if defined(__APPLE__) || defined(__clang__)
    unsigned int *seed = get_seed();  // @unsafe
    int r = rand_r(seed);  // @unsafe
#else // not __APPLE__
    int r = rand_r(&seed_);  // @unsafe
#endif // __APPLE__
    if (length <= 0)
        return std::to_string(r);  // @unsafe
    else
        return std::to_string(r).substr(0, length);  // @unsafe
}

// @unsafe - Uses std::to_string and std::string operations
std::string RandomGenerator::int2str_n(int i, int length) {
    std::string ret = std::to_string(i);  // @unsafe
    if (ret.length() < length) {
        while (ret.length() < length) {
            ret = std::string("0").append(ret);
        }
        return ret;
    }
    else if (ret.length() > length) {
        ret = ret.substr(ret.length() - length, length);
    }
    return ret;
}

// @unsafe - Calls rand_double which uses rand_r
bool RandomGenerator::percentage_true(double p) {
    if (rand_double((double)0, (double)100) <= p)  // @unsafe
        return true;
    else
        return false;
}

// @unsafe - Calls rand which uses rand_r
bool RandomGenerator::percentage_true(int p) {
    if (rand(0, 99) < p)  // @unsafe
        return true;
    else
        return false;
}

// @unsafe - Calls rand which uses rand_r
int RandomGenerator::nu_rand(int a, int x, int y) {
    int r1 = rand(0, a);  // @unsafe
    int r2 = rand(x, y);  // @unsafe
    return ((r1 | r2) + nu_constant) % (y - x + 1) + x;
}

// @unsafe - Uses inline assembly (rdtsc instruction)
unsigned long long RandomGenerator::rdtsc() {
#if defined(__APPLE__)
    // macOS (including Apple Silicon): mach_absolute_time is monotonic and fast.
    return static_cast<unsigned long long>(mach_absolute_time());
#elif defined(__x86_64__) || defined(__i386__)
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a" (lo), "=d" (hi));  // @unsafe
    return ((unsigned long long)hi << 32) | lo;
#elif defined(__clang__) && __has_builtin(__builtin_readcyclecounter)
    return static_cast<unsigned long long>(__builtin_readcyclecounter());
#else
    // Fallback: not a true cycle counter, but sufficient for per-thread seeding.
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (static_cast<unsigned long long>(ts.tv_sec) << 32) ^
           static_cast<unsigned long long>(ts.tv_nsec);
#endif
}

// @unsafe - Calls rand_double which uses rand_r
unsigned int RandomGenerator::weighted_select(const std::vector<double> &weight_vector) {
    double sum = 0, stage_sum = 0;
    unsigned int i = 0;
    while (i < weight_vector.size())
        sum += weight_vector[i++];
    double r = rand_double(0, sum);  // @unsafe
    i = 0;
    while (i < weight_vector.size())
        if (r <= (stage_sum += weight_vector[i]))
            return i;
        else
            i++;
    return --i;
}

// @unsafe - Uses pthread_once system call
void RandomGenerator::destroy() {
#if defined(__APPLE__) || defined(__clang__)
    pthread_once(&delete_key_once_, delete_key);  // @unsafe
#endif // __APPLE__
}

}
