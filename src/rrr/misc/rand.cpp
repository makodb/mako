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
int randgen_rand_raw();
double randgen_rand_max();
int randgen_nu_constant_now();
std::string randgen_zero_pad(const std::string& s, int length);
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
        verify(max >= min);
        let r = randgen_rand_raw();
        (r % ((max - min) + 1)) + min
    }

    fn rand_double(min: f64, max: f64) -> f64 {
        if max == min {
            return min;
        }
        verify(max > min);
        let r = randgen_rand_raw();
        ((r as f64) / (randgen_rand_max() / (max - min))) + min
    }

    fn int2str_n(i: i32, length: i32) -> std::string {
        let s = std::to_string(i);
        randgen_zero_pad(s, length)
    }

    fn percentage_true(p: i32) -> bool {
        RandomGenerator::rand(0, 99) < p
    }

    fn nu_rand(a: i32, x: i32, y: i32) -> i32 {
        let r1 = RandomGenerator::rand(0, a);
        let r2 = RandomGenerator::rand(x, y);
        (((r1 | r2) + randgen_nu_constant_now()) % ((y - x) + 1)) + x
    }

    fn weighted_select(weight_vector: &RandWeightVec) -> u32 {
        let mut sum: f64 = 0.0;
        let mut i: u32 = 0;
        while i < weight_vector.size() {
            sum += weight_vector[i];
            i += 1;
        }
        let r = RandomGenerator::rand_double(0.0, sum);
        let mut stage_sum: f64 = 0.0;
        let mut k: u32 = 0;
        while k < weight_vector.size() {
            stage_sum += weight_vector[k];
            if r <= stage_sum {
                return k;
            }
            k += 1;
        }
        k - 1
    }

    fn destroy() {
        randgen_destroy()
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=rand.generator version=1 rust_sha256=4603415f808960c109d21fa5e77e27254cbf95f5a76de89d5f2e818abff9a430*/
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
    verify(rusty::detail::deref_if_pointer_like(max) >= rusty::detail::deref_if_pointer_like(min));
    const auto r = randgen_rand_raw();
    return ((rusty::detail::deref_if_pointer_like(r) % ((((rusty::detail::deref_if_pointer_like(max) - rusty::detail::deref_if_pointer_like(min))) + static_cast<int32_t>(1))))) + rusty::detail::deref_if_pointer_like(min);
}

double RandomGenerator::rand_double(double min, double max) {
    if (rusty::detail::deref_if_pointer_like(max) == rusty::detail::deref_if_pointer_like(min)) {
        return std::move(min);
    }
    verify(rusty::detail::deref_if_pointer_like(max) > rusty::detail::deref_if_pointer_like(min));
    const auto r = randgen_rand_raw();
    return ((((static_cast<double>(r))) / ((randgen_rand_max() / ((rusty::detail::deref_if_pointer_like(max) - rusty::detail::deref_if_pointer_like(min))))))) + rusty::detail::deref_if_pointer_like(min);
}

std::string RandomGenerator::int2str_n(int32_t i, int32_t length) {
    const auto s = std::to_string(std::move(i));
    return randgen_zero_pad(std::move(s), std::move(length));
}

bool RandomGenerator::percentage_true(int32_t p) {
    return RandomGenerator::rand(static_cast<int32_t>(0), static_cast<int32_t>(99)) < rusty::detail::deref_if_pointer_like(p);
}

int32_t RandomGenerator::nu_rand(int32_t a, int32_t x, int32_t y) {
    const auto r1 = RandomGenerator::rand(static_cast<int32_t>(0), std::move(a));
    const auto r2 = RandomGenerator::rand(std::move(x), std::move(y));
    return ((((((rusty::detail::deref_if_pointer_like(r1) | rusty::detail::deref_if_pointer_like(r2))) + randgen_nu_constant_now())) % ((((rusty::detail::deref_if_pointer_like(y) - rusty::detail::deref_if_pointer_like(x))) + static_cast<int32_t>(1))))) + rusty::detail::deref_if_pointer_like(x);
}

uint32_t RandomGenerator::weighted_select(const RandWeightVec& weight_vector) {
    double sum = 0.0;
    uint32_t i = static_cast<uint32_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < weight_vector.size()) {
        sum += weight_vector[i];
        i += 1;
    }
    const auto r = RandomGenerator::rand_double(0.0, std::move(sum));
    double stage_sum = 0.0;
    uint32_t k = static_cast<uint32_t>(0);
    while (rusty::detail::deref_if_pointer_like(k) < weight_vector.size()) {
        stage_sum += weight_vector[k];
        if (rusty::detail::deref_if_pointer_like(r) <= rusty::detail::deref_if_pointer_like(stage_sum)) {
            return std::move(k);
        }
        k += 1;
    }
    return rusty::detail::deref_if_pointer_like(k) - static_cast<uint32_t>(1);
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

// @unsafe - the irreducible C surface: rand_r over the pthread-keyed /
// thread-local seed. All range/scale logic lives in the DSL statics.
int randgen_rand_raw() {
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
    return r;
}

// @safe - RAND_MAX as a double for the DSL's scale math (the macro has
// no DSL spelling).
double randgen_rand_max() {
    return static_cast<double>(RAND_MAX);
}

// @safe - accessor over the impl-namespace nu_rand constant.
int randgen_nu_constant_now() {
    return randgen_nu_constant;
}

// @unsafe - std::string surgery (substr/prepend) for int2str_n's
// fixed-width formatting; kept as a kernel for the substr call.
std::string randgen_zero_pad(const std::string& s, int length) {
    std::string ret = s;
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

// @unsafe - pthread_once + raw pthread key teardown.
void randgen_destroy() {
#if defined(__APPLE__) || defined(__clang__)
    pthread_once(&randgen_delete_key_once, randgen_delete_key);
#endif
}


}
