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

struct RandomGenerator;

// Hand-written kernels for the DSL statics below (rand_r on the
// pthread-keyed / thread_local seed, pthread teardown, foreign
// std::string / std::vector surgery). Definitions in the impl
// namespace; the seed plumbing lives in an anonymous namespace there.
int randgen_rand(int min, int max);
double randgen_rand_double(double min, double max);
std::string randgen_int2str_n(int i, int length);
int randgen_nu_rand(int a, int x, int y);
unsigned int randgen_weighted_select(const std::vector<double>& weight_vector);
void randgen_destroy();

// std::vector<double> spelled via an alias for the DSL param grammar.
using RandWeightVec = std::vector<double>;

// `RandomGenerator` — all-static PRNG helpers over a per-thread seed.
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block.
//
// Behavioral diffs from the original C++ class:
//   * Default args are gone (DSL fns have no defaults): the four
//     no-arg rand()/rand_double() call sites now pass the old
//     defaults explicitly.
//   * rand_str(length = 0) and the percentage_true(double) overload
//     are DROPPED — zero callers repo-wide (and a Rust impl cannot
//     hold two fns named percentage_true anyway).
//   * The private static seed machinery (pthread key/once state,
//     get_seed, rdtsc, nu_constant) moved to file-scope statics in the
//     impl namespace — a DSL struct cannot carry static data members.
#if RUSTYCPP_RUST
struct RandomGenerator {}

impl RandomGenerator {
    fn rand(min: i32, max: i32) -> i32 {
        randgen_rand(min, max)
    }

    fn rand_double(min: f64, max: f64) -> f64 {
        randgen_rand_double(min, max)
    }

    fn int2str_n(i: i32, length: i32) -> std::string {
        randgen_int2str_n(i, length)
    }

    fn percentage_true(p: i32) -> bool {
        RandomGenerator::rand(0, 99) < p
    }

    fn nu_rand(a: i32, x: i32, y: i32) -> i32 {
        randgen_nu_rand(a, x, y)
    }

    fn weighted_select(weight_vector: &RandWeightVec) -> u32 {
        randgen_weighted_select(weight_vector)
    }

    fn destroy() {
        randgen_destroy()
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=rand.generator version=1 rust_sha256=cd5d0e84be69529f8548c14b965f184e3a0d45ef91d76da380dfcafdc9e117c6*/
struct RandomGenerator;

struct RandomGenerator {

    static int32_t rand(int32_t min, int32_t max);
    static double rand_double(double min, double max);
    static std::string int2str_n(int32_t i, int32_t length);
    static bool percentage_true(int32_t p);
    static int32_t nu_rand(int32_t a, int32_t x, int32_t y);
    static uint32_t weighted_select(const RandWeightVec& weight_vector);
    static void destroy();
};


int32_t RandomGenerator::rand(int32_t min, int32_t max) {
    return randgen_rand(std::move(min), std::move(max));
}

double RandomGenerator::rand_double(double min, double max) {
    return randgen_rand_double(std::move(min), std::move(max));
}

std::string RandomGenerator::int2str_n(int32_t i, int32_t length) {
    return randgen_int2str_n(std::move(i), std::move(length));
}

bool RandomGenerator::percentage_true(int32_t p) {
    return RandomGenerator::rand(static_cast<int32_t>(0), static_cast<int32_t>(99)) < rusty::detail::deref_if_pointer_like(p);
}

int32_t RandomGenerator::nu_rand(int32_t a, int32_t x, int32_t y) {
    return randgen_nu_rand(std::move(a), std::move(x), std::move(y));
}

uint32_t RandomGenerator::weighted_select(const RandWeightVec& weight_vector) {
    return randgen_weighted_select(weight_vector);
}

void RandomGenerator::destroy() {
    randgen_destroy();
}
/*RUSTYCPP:GEN-END id=rand.generator*/

} // export namespace rrr

namespace rrr {

namespace {

#if defined(__APPLE__) || defined(__clang__)
pthread_key_t randgen_seed_key;
pthread_once_t randgen_seed_key_once = PTHREAD_ONCE_INIT;
pthread_once_t randgen_delete_key_once = PTHREAD_ONCE_INIT;
#endif

int randgen_nu_constant = 0;

// @unsafe - inline `rdtsc` asm + clock_gettime syscall.
unsigned long long randgen_rdtsc() {
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

#if defined(__APPLE__) || defined(__clang__)
// @unsafe - pthread_key_create with raw `free` function pointer.
void randgen_create_key() {
    pthread_key_create(&randgen_seed_key, free);
}

// @unsafe - pthread_key_delete on raw pthread key.
void randgen_delete_key() {
    pthread_key_delete(randgen_seed_key);
}

// @unsafe - returns raw `unsigned int*` from pthread_getspecific;
// malloc + C-style casts + pointer deref to seed the slot.
unsigned int *randgen_get_seed() {
    pthread_once(&randgen_seed_key_once, randgen_create_key);
    unsigned int *seed = (unsigned int *)pthread_getspecific(randgen_seed_key);
    if (seed == NULL) {
        seed = (unsigned int *)malloc(sizeof(unsigned int));
        pthread_setspecific(randgen_seed_key, (void *)seed);
        *seed = randgen_rdtsc();
    }
    return seed;
}
#else
thread_local unsigned int randgen_seed = randgen_rdtsc();
#endif

}  // namespace

int randgen_rand(int min, int max) {
    verify(max >= min);
    int r = 0;
    // @unsafe { get_seed returns raw unsigned int*; rand_r dereferences it }
    {
#if defined(__APPLE__) || defined(__clang__)
        unsigned int *seed = randgen_get_seed();
        r = rand_r(seed);
#else
        r = rand_r(&randgen_seed);
#endif
    }
    return (r % (max - min + 1)) + min;
}

double randgen_rand_double(double min, double max) {
    if (max == min)
        return min;
    verify(max > min);
    int r = 0;
    // @unsafe { get_seed returns raw unsigned int*; rand_r dereferences it }
    {
#if defined(__APPLE__) || defined(__clang__)
        unsigned int *seed = randgen_get_seed();
        r = rand_r(seed);
#else
        r = rand_r(&randgen_seed);
#endif
    }
    return (static_cast<double>(r)) / (static_cast<double>(RAND_MAX) / (max - min)) + min;
}

std::string randgen_int2str_n(int i, int length) {
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

int randgen_nu_rand(int a, int x, int y) {
    int r1 = randgen_rand(0, a);
    int r2 = randgen_rand(x, y);
    return ((r1 | r2) + randgen_nu_constant) % (y - x + 1) + x;
}

unsigned int randgen_weighted_select(const std::vector<double> &weight_vector) {
    double sum = 0, stage_sum = 0;
    unsigned int i = 0;
    while (i < weight_vector.size())
        sum += weight_vector[i++];
    double r = randgen_rand_double(0, sum);
    i = 0;
    while (i < weight_vector.size())
        if (r <= (stage_sum += weight_vector[i]))
            return i;
        else
            i++;
    return --i;
}

// @unsafe - pthread_once + raw pthread key teardown.
void randgen_destroy() {
#if defined(__APPLE__) || defined(__clang__)
    pthread_once(&randgen_delete_key_once, randgen_delete_key);
#endif
}


}
