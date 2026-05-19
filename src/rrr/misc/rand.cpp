module;

#include <rusty/rusty.hpp>
#include <pthread.h>
#include <time.h>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

export module rrr.rand;

import std;
import rrr.debugging;

// @safe - RandomGenerator: mostly pure helpers (int2str_n, formatting,
// percentage math). The pthread-keyed seed plumbing (create_key,
// delete_key, get_seed, rdtsc, destroy) and the rand_r entry points
// (rand/rand_double/rand_str) carry per-method `// @unsafe` overrides
// because they touch raw `unsigned int*` from pthread_getspecific,
// inline asm, malloc, and pthread C-API calls.
export namespace rrr {

// @safe - see file header.
class RandomGenerator {
private:
#if defined(__APPLE__) || defined(__clang__)
    static pthread_key_t seed_key_;
    static pthread_once_t seed_key_once_;
    static pthread_once_t delete_key_once_;

    static void create_key();
    static void delete_key();

    static unsigned int *get_seed();
#else
    static thread_local unsigned int seed_;
#endif

    static int nu_constant;
    static unsigned long long rdtsc();

public:

    static int rand(int min = 0, int max = RAND_MAX);

    static double rand_double(double min = 0.0,
                              double max = (double)RAND_MAX);

    static std::string rand_str(int length = 0);

    static std::string int2str_n(int i, int length);

    static int nu_rand(int a, int x, int y);

    static bool percentage_true(double p);

    static bool percentage_true(int p);

    static unsigned int weighted_select(const std::vector<double> &weight_vector);

    static void destroy();
};

} // export namespace rrr

namespace rrr {

#if defined(__APPLE__) || defined(__clang__)
pthread_key_t RandomGenerator::seed_key_;
pthread_once_t RandomGenerator::seed_key_once_ = PTHREAD_ONCE_INIT;
pthread_once_t RandomGenerator::delete_key_once_ = PTHREAD_ONCE_INIT;

// @unsafe - pthread_key_create with raw `free` function pointer.
void RandomGenerator::create_key() {
    pthread_key_create(&seed_key_, free);
}

// @unsafe - pthread_key_delete on raw pthread key.
void RandomGenerator::delete_key() {
    pthread_key_delete(seed_key_);
}

// @unsafe - returns raw `unsigned int*` from pthread_getspecific;
// malloc + C-style casts + pointer deref to seed the slot.
unsigned int *RandomGenerator::get_seed() {
    pthread_once(&seed_key_once_, create_key);
    unsigned int *seed = (unsigned int *)pthread_getspecific(seed_key_);
    if (seed == NULL) {
        seed = (unsigned int *)malloc(sizeof(unsigned int));
        pthread_setspecific(seed_key_, (void *)seed);
        *seed = rdtsc();
    }
    return seed;
}
#else
thread_local unsigned int RandomGenerator::seed_ = RandomGenerator::rdtsc();
#endif

int RandomGenerator::nu_constant = 0;

int RandomGenerator::rand(int min, int max) {
    verify(max >= min);
    int r = 0;
    // @unsafe { get_seed returns raw unsigned int*; rand_r dereferences it }
    {
#if defined(__APPLE__) || defined(__clang__)
        unsigned int *seed = get_seed();
        r = rand_r(seed);
#else
        r = rand_r(&seed_);
#endif
    }
    return (r % (max - min + 1)) + min;
}

double RandomGenerator::rand_double(double min, double max) {
    if (max == min)
        return min;
    verify(max > min);
    int r = 0;
    // @unsafe { get_seed returns raw unsigned int*; rand_r dereferences it }
    {
#if defined(__APPLE__) || defined(__clang__)
        unsigned int *seed = get_seed();
        r = rand_r(seed);
#else
        r = rand_r(&seed_);
#endif
    }
    return (static_cast<double>(r)) / (static_cast<double>(RAND_MAX) / (max - min)) + min;
}

std::string RandomGenerator::rand_str(int length) {
    int r = 0;
    // @unsafe { get_seed returns raw unsigned int*; rand_r dereferences it }
    {
#if defined(__APPLE__) || defined(__clang__)
        unsigned int *seed = get_seed();
        r = rand_r(seed);
#else
        r = rand_r(&seed_);
#endif
    }
    if (length <= 0)
        return std::to_string(r);
    else
        return std::to_string(r).substr(0, length);
}

std::string RandomGenerator::int2str_n(int i, int length) {
    std::string ret = std::to_string(i);
    if (static_cast<int>(ret.length()) < length) {
        while (static_cast<int>(ret.length()) < length) {
            ret = std::string("0").append(ret);
        }
        return ret;
    }
    else if (static_cast<int>(ret.length()) > length) {
        ret = ret.substr(ret.length() - length, length);
    }
    return ret;
}

bool RandomGenerator::percentage_true(double p) {
    if (rand_double(0.0, 100.0) <= p)
        return true;
    else
        return false;
}

bool RandomGenerator::percentage_true(int p) {
    if (rand(0, 99) < p)
        return true;
    else
        return false;
}

int RandomGenerator::nu_rand(int a, int x, int y) {
    int r1 = rand(0, a);
    int r2 = rand(x, y);
    return ((r1 | r2) + nu_constant) % (y - x + 1) + x;
}

// @unsafe - inline `rdtsc` asm + clock_gettime syscall.
unsigned long long RandomGenerator::rdtsc() {
#if defined(__APPLE__)
    return static_cast<unsigned long long>(mach_absolute_time());
#elif defined(__x86_64__) || defined(__i386__)
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a" (lo), "=d" (hi));
    return ((unsigned long long)hi << 32) | lo;
#elif defined(__clang__) && __has_builtin(__builtin_readcyclecounter)
    return static_cast<unsigned long long>(__builtin_readcyclecounter());
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (static_cast<unsigned long long>(ts.tv_sec) << 32) ^
           static_cast<unsigned long long>(ts.tv_nsec);
#endif
}

unsigned int RandomGenerator::weighted_select(const std::vector<double> &weight_vector) {
    double sum = 0, stage_sum = 0;
    unsigned int i = 0;
    while (i < weight_vector.size())
        sum += weight_vector[i++];
    double r = rand_double(0, sum);
    i = 0;
    while (i < weight_vector.size())
        if (r <= (stage_sum += weight_vector[i]))
            return i;
        else
            i++;
    return --i;
}

// @unsafe - pthread_once + raw pthread key teardown.
void RandomGenerator::destroy() {
#if defined(__APPLE__) || defined(__clang__)
    pthread_once(&delete_key_once_, delete_key);
#endif
}

}
