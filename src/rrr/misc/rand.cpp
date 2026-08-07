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
// Authored as inline Rust DSL — RAND_MAX widened to double, no kernel.
#if RUSTYCPP_RUST
fn randgen_rand_max() -> f64 {
    RAND_MAX as f64
}
#endif
/*RUSTYCPP:GEN-BEGIN id=rand.rand_max version=1 rust_sha256=e16936a0504c4b38cef24916eeb30d744f0e03cca557de7876579749f19eb0fc*/
double randgen_rand_max();

double randgen_rand_max() {
    return static_cast<double>(RAND_MAX);
}
/*RUSTYCPP:GEN-END id=rand.rand_max*/
int randgen_nu_constant_now();
// Left-pad (or right-truncate) `s` to exactly `length` chars. Authored as
// inline Rust DSL — pure std::string control flow, no kernel needed.
#if RUSTYCPP_RUST
fn randgen_zero_pad(s: std::string, length: i32) -> std::string {
    let mut ret: std::string = s;
    if (ret.length() as i32) < length {
        while (ret.length() as i32) < length {
            ret.insert(0, "0");
        }
        return ret;
    }
    if (ret.length() as i32) > length {
        let cur: usize = ret.length();
        return ret.substr(cur - (length as usize), length as usize);
    }
    ret
}
#endif
/*RUSTYCPP:GEN-BEGIN id=rand.zero_pad version=1 rust_sha256=093fc9bc66d0ac77ad4481006931018c15e0b61ba124eba2d1d7582b9c5295ab*/
std::string randgen_zero_pad(std::string s, int32_t length);

std::string randgen_zero_pad(std::string s, int32_t length) {
    std::string ret = s;
    if (((static_cast<int32_t>(ret.length()))) < rusty::detail::deref_if_pointer_like(length)) {
        while (((static_cast<int32_t>(ret.length()))) < rusty::detail::deref_if_pointer_like(length)) {
            ret.insert(0, "0");
        }
        return std::move(ret);
    }
    if (((static_cast<int32_t>(ret.length()))) > rusty::detail::deref_if_pointer_like(length)) {
        const size_t cur = ret.length();
        return ret.substr(rusty::detail::deref_if_pointer_like(cur) - ((static_cast<size_t>(length))), static_cast<size_t>(length));
    }
    return std::move(ret);
}
/*RUSTYCPP:GEN-END id=rand.zero_pad*/
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
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
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

int randgen_nu_constant = 0;

}  // namespace

// The per-thread PRNG seed store (pthread_key plumbing, the raw
// `unsigned int*` seed, rand_r over it, and the pthread_once teardown)
// lives in srpc_rand.c now — plain C, Goal-0 C demotion. None of it
// needed C++, and none of it could ever be inline-Rust DSL.
extern "C" int srpc_rand_raw(void);
extern "C" void srpc_rand_destroy(void);

// The last three shims are DSL too: two `unsafe {}` calls into the
// srpc_rand.c kernels and one accessor over the impl-namespace nu_rand
// constant. `i32` lowers to `int32_t`, which is the same type as the
// `int` in the export-namespace declarations above (same redeclaration
// pattern the sibling logging.cpp already uses for its kernels).
#if RUSTYCPP_RUST
// @unsafe - thin shim over the C kernel.
fn randgen_rand_raw() -> i32 {
    unsafe { return srpc_rand_raw(); }
}

// @safe - accessor over the impl-namespace nu_rand constant.
fn randgen_nu_constant_now() -> i32 {
    randgen_nu_constant
}

// @unsafe - thin shim over the C kernel (pthread teardown lives there).
fn randgen_destroy() {
    unsafe { srpc_rand_destroy(); }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=rand.4 version=1 rust_sha256=c7ab926c4aeff1c9c31719eadadae48c5b53c91e8544c8f6bd7e3a4cc6a758b4*/
int32_t randgen_rand_raw();
int32_t randgen_nu_constant_now();
void randgen_destroy();

int32_t randgen_rand_raw() {
    // @unsafe
    {
        return srpc_rand_raw();
    }
}

int32_t randgen_nu_constant_now() {
    return std::move(randgen_nu_constant);
}

void randgen_destroy() {
    // @unsafe
    {
        srpc_rand_destroy();
    }
}
/*RUSTYCPP:GEN-END id=rand.4*/


}
