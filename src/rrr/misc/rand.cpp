module;

#include <rusty/rusty.hpp>
#include <pthread.h>
#include <time.h>
#include "srpc_rand.h"

export module rrr.rand;

import std;
import rusty;

// Canonical Rust owns all range, formatting, and weighted-selection logic.
// Its only unsafe operations are the two one-call extern-C wrappers around
// the raw draw and teardown functions declared by srpc_rand.h; seed storage,
// pthread handling, rand_r, and cycle-counter access live in plain C.
export namespace rrr {

struct RandomGenerator;

// Declaration-order bridges for the Rust methods below. The generated ABI
// facades perform byte-string and vector/span conversion; the only external
// kernels are the plain-C raw draw and teardown functions.
int randgen_rand_raw();
// Authored as inline Rust DSL — i32::MAX matches the C kernel's RAND_MAX
// contract and is widened to double without a kernel call.
#if RUSTYCPP_RUST
pub fn randgen_rand_max() -> f64 {
    i32::MAX as f64
}
#endif
/*RUSTYCPP:GEN-BEGIN id=rand.rand_max version=1 rust_sha256=7d1c8d854739c082fde745f7bda9a748d5ce2954610377bfe2f76183a6682134*/
double randgen_rand_max();

double randgen_rand_max() {
    return static_cast<double>(std::numeric_limits<int32_t>::max());
}
/*RUSTYCPP:GEN-END id=rand.rand_max*/
int randgen_nu_constant_now();
// Left-pad (or right-truncate) `s` to exactly `length` chars. Authored as
// inline Rust DSL over bytes; the generated facade preserves std::string.
#if RUSTYCPP_RUST
#[cfg_attr(any(), cpp_abi(
    param(s, std_string_bytes),
    returns(std_string_bytes)
))]
pub fn randgen_zero_pad(s: Vec<u8>, length: i32) -> Vec<u8> {
    let mut ret = s;
    while (ret.len() as i32) < length {
        ret.insert(0usize, 48u8);
    }
    while (ret.len() as i32) > length {
        ret.remove(0usize);
    }
    ret
}
#endif
/*RUSTYCPP:GEN-BEGIN id=rand.zero_pad version=1 rust_sha256=986f35249ad982636c86cc47220f90da194dbf80906aa649428d19c3c31df700*/
namespace rusty_cpp_abi_detail_m_410e5b522c6dc9a94f23570cd0ebca4459da8b88f9a5170ba166350adf9474dd {
    inline rusty::Vec<uint8_t> bytes_from_std_string(const std::string& input) {
        auto output = rusty::Vec<uint8_t>::with_capacity(input.size());
        for (unsigned char byte : input) {
            output.push(static_cast<uint8_t>(byte));
        }
        return output;
    }
    inline std::string std_string_from_bytes(rusty::Vec<uint8_t> input) {
        if (input.size() == 0) {
            return {};
        }
        return std::string(reinterpret_cast<const char*>(input.data()), input.size());
    }
    inline std::span<const double> f64_span_from_std_vector(const std::vector<double>& input) {
        return std::span<const double>(input.data(), input.size());
    }
} // namespace rusty_cpp_abi_detail_m_410e5b522c6dc9a94f23570cd0ebca4459da8b88f9a5170ba166350adf9474dd

inline rusty::Vec<uint8_t> rusty_cpp_abi_sem_m_410e5b522c6dc9a94f23570cd0ebca4459da8b88f9a5170ba166350adf9474dd_randgen_zero_pad(rusty::Vec<uint8_t> s, int32_t length);
std::string randgen_zero_pad(std::string s, int32_t length);

inline rusty::Vec<uint8_t> rusty_cpp_abi_sem_m_410e5b522c6dc9a94f23570cd0ebca4459da8b88f9a5170ba166350adf9474dd_randgen_zero_pad(rusty::Vec<uint8_t> s, int32_t length) {
    auto ret = std::move(s);
    while (((static_cast<int32_t>(rusty::len(ret)))) < rusty::detail::deref_if_pointer_like(length)) {
        ret.insert(static_cast<size_t>(0), static_cast<uint8_t>(48));
    }
    while (((static_cast<int32_t>(rusty::len(ret)))) > rusty::detail::deref_if_pointer_like(length)) {
        ret.remove(static_cast<size_t>(0));
    }
    return std::move(ret);
}

std::string randgen_zero_pad(std::string s, int32_t length) {
    auto rusty_cpp_abi_arg_0 = rusty_cpp_abi_detail_m_410e5b522c6dc9a94f23570cd0ebca4459da8b88f9a5170ba166350adf9474dd::bytes_from_std_string(s);
    auto rusty_cpp_abi_result = rusty_cpp_abi_sem_m_410e5b522c6dc9a94f23570cd0ebca4459da8b88f9a5170ba166350adf9474dd_randgen_zero_pad(std::move(rusty_cpp_abi_arg_0), length);
    return rusty_cpp_abi_detail_m_410e5b522c6dc9a94f23570cd0ebca4459da8b88f9a5170ba166350adf9474dd::std_string_from_bytes(std::move(rusty_cpp_abi_result));
}
/*RUSTYCPP:GEN-END id=rand.zero_pad*/
void randgen_destroy();

// `RandomGenerator` — all-static PRNG helpers over the C kernel's per-thread
// seed.
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
//   * The private seed machinery moved to srpc_rand.c; this carrier retains
//     only generated C++ ABI facades and their declaration-order scaffolding.
#if RUSTYCPP_RUST
#[cfg_attr(any(), cpp_abi_alias(std_vector))]
pub type RandWeightVec = Vec<f64>;

pub struct RandomGenerator {}

impl RandomGenerator {
    pub fn rand(min: i32, max: i32) -> i32 {
        assert!(max >= min);
        let r = randgen_rand_raw();
        let width = max.wrapping_sub(min).wrapping_add(1i32);
        assert!(width != 0i32);
        (r % width).wrapping_add(min)
    }

    pub fn rand_double(min: f64, max: f64) -> f64 {
        if max == min {
            return min;
        }
        assert!(max > min);
        let r = randgen_rand_raw();
        ((r as f64) / (randgen_rand_max() / (max - min))) + min
    }

    #[cfg_attr(any(), cpp_abi(returns(std_string_bytes)))]
    pub fn int2str_n(i: i32, length: i32) -> Vec<u8> {
        let negative = i < 0i32;
        let mut magnitude: u32 = i.unsigned_abs();
        let mut reversed = Vec::<u8>::new();
        loop {
            reversed.push(((magnitude % 10u32) as u8).wrapping_add(48u8));
            magnitude /= 10u32;
            if magnitude == 0u32 {
                break;
            }
        }
        let mut s = Vec::<u8>::with_capacity(
            reversed.len() + if negative { 1usize } else { 0usize },
        );
        if negative {
            s.push(45u8);
        }
        let mut position = reversed.len();
        while position > 0usize {
            position -= 1usize;
            s.push(reversed[position]);
        }
        randgen_zero_pad(s, length)
    }

    pub fn percentage_true(p: i32) -> bool {
        RandomGenerator::rand(0, 99) < p
    }

    pub fn nu_rand(a: i32, x: i32, y: i32) -> i32 {
        let r1 = RandomGenerator::rand(0, a);
        let r2 = RandomGenerator::rand(x, y);
        let width = y.wrapping_sub(x).wrapping_add(1i32);
        assert!(width != 0i32);
        ((r1 | r2).wrapping_add(randgen_nu_constant_now()) % width)
            .wrapping_add(x)
    }

    #[cfg_attr(any(), cpp_abi(param(
        weight_vector,
        const_ref(RandWeightVec)
    )))]
    pub fn weighted_select(weight_vector: &[f64]) -> u32 {
        let mut sum: f64 = 0.0;
        let mut i: usize = 0usize;
        while i < weight_vector.len() {
            sum += weight_vector[i];
            i += 1usize;
        }
        let r = RandomGenerator::rand_double(0.0, sum);
        let mut stage_sum: f64 = 0.0;
        let mut k: usize = 0usize;
        while k < weight_vector.len() {
            stage_sum += weight_vector[k];
            if r <= stage_sum {
                return k as u32;
            }
            k += 1usize;
        }
        (k as u32).wrapping_sub(1u32)
    }

    pub fn destroy() {
        randgen_destroy()
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=rand.generator version=1 rust_sha256=f9aa0fb7e87f4131d094d571602ac3b768705c09324fb8c7e9ef4d98436d08ed*/
struct RandomGenerator;
using RandWeightVec = std::vector<double>;
inline rusty::Vec<uint8_t> rusty_cpp_abi_sem_m_410e5b522c6dc9a94f23570cd0ebca4459da8b88f9a5170ba166350adf9474dd_RandomGenerator_int2str_n(int32_t i, int32_t length);
inline uint32_t rusty_cpp_abi_sem_m_410e5b522c6dc9a94f23570cd0ebca4459da8b88f9a5170ba166350adf9474dd_RandomGenerator_weighted_select(std::span<const double> weight_vector);


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

inline rusty::Vec<uint8_t> rusty_cpp_abi_sem_m_410e5b522c6dc9a94f23570cd0ebca4459da8b88f9a5170ba166350adf9474dd_RandomGenerator_int2str_n(int32_t i, int32_t length) {
    const auto negative = rusty::detail::deref_if_pointer_like(i) < static_cast<int32_t>(0);
    uint32_t magnitude = ([&]() { auto&& _v = i; using _V = std::remove_cv_t<std::remove_reference_t<decltype(_v)>>; using _U = std::make_unsigned_t<_V>; if constexpr (std::is_signed_v<_V>) { auto _u = static_cast<_U>(_v); return (_v < 0) ? static_cast<_U>(static_cast<_U>(0) - _u) : _u; } else { return static_cast<_U>(_v); } })();
    auto reversed = rusty::Vec<uint8_t>::new_();
    while (true) {
        reversed.push(rusty::wrapping_add(((static_cast<uint8_t>((rusty::detail::deref_if_pointer_like(magnitude) % static_cast<uint32_t>(10))))), static_cast<std::remove_cvref_t<decltype(((static_cast<uint8_t>((rusty::detail::deref_if_pointer_like(magnitude) % static_cast<uint32_t>(10))))))>>(static_cast<uint8_t>(48))));
        magnitude /= static_cast<uint32_t>(10);
        if (rusty::detail::deref_if_pointer_like(magnitude) == static_cast<uint32_t>(0)) {
            break;
        }
    }
    auto s = rusty::Vec<uint8_t>::with_capacity(rusty::len(reversed) + (negative ? static_cast<size_t>(1) : static_cast<size_t>(0)));
    if (negative) {
        s.push(static_cast<uint8_t>(45));
    }
    auto position = rusty::len(reversed);
    while (rusty::detail::deref_if_pointer_like(position) > static_cast<size_t>(0)) {
        rusty::detail::deref_if_pointer_like(position) -= static_cast<size_t>(1);
        s.push(reversed[position]);
    }
    return rusty_cpp_abi_sem_m_410e5b522c6dc9a94f23570cd0ebca4459da8b88f9a5170ba166350adf9474dd_randgen_zero_pad(std::move(s), std::move(length));
}

inline uint32_t rusty_cpp_abi_sem_m_410e5b522c6dc9a94f23570cd0ebca4459da8b88f9a5170ba166350adf9474dd_RandomGenerator_weighted_select(std::span<const double> weight_vector) {
    double sum = 0.0;
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::len(weight_vector)) {
        sum += weight_vector[i];
        i += static_cast<size_t>(1);
    }
    const auto r = RandomGenerator::rand_double(0.0, std::move(sum));
    double stage_sum = 0.0;
    size_t k = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(k) < rusty::len(weight_vector)) {
        stage_sum += weight_vector[k];
        if (rusty::detail::deref_if_pointer_like(r) <= rusty::detail::deref_if_pointer_like(stage_sum)) {
            return static_cast<uint32_t>(k);
        }
        k += static_cast<size_t>(1);
    }
    return rusty::wrapping_sub(((static_cast<uint32_t>(k))), static_cast<std::remove_cvref_t<decltype(((static_cast<uint32_t>(k))))>>(static_cast<uint32_t>(1)));
}


int32_t RandomGenerator::rand(int32_t min, int32_t max) {
    if (!(rusty::detail::deref_if_pointer_like(max) >= rusty::detail::deref_if_pointer_like(min))) { rusty::panic::do_panic("assertion failed: max >= min"); }
    const auto r = randgen_rand_raw();
    const auto width = rusty::wrapping_add(rusty::wrapping_sub(max, static_cast<std::remove_cvref_t<decltype(max)>>(std::move(min))), static_cast<std::remove_cvref_t<decltype(rusty::wrapping_sub(max, static_cast<std::remove_cvref_t<decltype(max)>>(std::move(min))))>>(static_cast<int32_t>(1)));
    if (!(rusty::detail::deref_if_pointer_like(width) != static_cast<int32_t>(0))) { rusty::panic::do_panic("assertion failed: width != 0i32"); }
    return rusty::wrapping_add((rusty::detail::deref_if_pointer_like(r) % rusty::detail::deref_if_pointer_like(width)), static_cast<std::remove_cvref_t<decltype((rusty::detail::deref_if_pointer_like(r) % rusty::detail::deref_if_pointer_like(width)))>>(std::move(min)));
}

double RandomGenerator::rand_double(double min, double max) {
    if (rusty::detail::deref_if_pointer_like(max) == rusty::detail::deref_if_pointer_like(min)) {
        return std::move(min);
    }
    if (!(rusty::detail::deref_if_pointer_like(max) > rusty::detail::deref_if_pointer_like(min))) { rusty::panic::do_panic("assertion failed: max > min"); }
    const auto r = randgen_rand_raw();
    return ((((static_cast<double>(r))) / ((randgen_rand_max() / ((rusty::detail::deref_if_pointer_like(max) - rusty::detail::deref_if_pointer_like(min))))))) + rusty::detail::deref_if_pointer_like(min);
}

std::string RandomGenerator::int2str_n(int32_t i, int32_t length) {
    auto rusty_cpp_abi_result = rusty_cpp_abi_sem_m_410e5b522c6dc9a94f23570cd0ebca4459da8b88f9a5170ba166350adf9474dd_RandomGenerator_int2str_n(i, length);
    return rusty_cpp_abi_detail_m_410e5b522c6dc9a94f23570cd0ebca4459da8b88f9a5170ba166350adf9474dd::std_string_from_bytes(std::move(rusty_cpp_abi_result));
}

bool RandomGenerator::percentage_true(int32_t p) {
    return RandomGenerator::rand(static_cast<int32_t>(0), static_cast<int32_t>(99)) < rusty::detail::deref_if_pointer_like(p);
}

int32_t RandomGenerator::nu_rand(int32_t a, int32_t x, int32_t y) {
    const auto r1 = RandomGenerator::rand(static_cast<int32_t>(0), std::move(a));
    const auto r2 = RandomGenerator::rand(std::move(x), std::move(y));
    const auto width = rusty::wrapping_add(rusty::wrapping_sub(y, static_cast<std::remove_cvref_t<decltype(y)>>(std::move(x))), static_cast<std::remove_cvref_t<decltype(rusty::wrapping_sub(y, static_cast<std::remove_cvref_t<decltype(y)>>(std::move(x))))>>(static_cast<int32_t>(1)));
    if (!(rusty::detail::deref_if_pointer_like(width) != static_cast<int32_t>(0))) { rusty::panic::do_panic("assertion failed: width != 0i32"); }
    return rusty::wrapping_add((rusty::wrapping_add((rusty::detail::deref_if_pointer_like(r1) | rusty::detail::deref_if_pointer_like(r2)), static_cast<std::remove_cvref_t<decltype((rusty::detail::deref_if_pointer_like(r1) | rusty::detail::deref_if_pointer_like(r2)))>>(randgen_nu_constant_now())) % rusty::detail::deref_if_pointer_like(width)), static_cast<std::remove_cvref_t<decltype((rusty::wrapping_add((rusty::detail::deref_if_pointer_like(r1) | rusty::detail::deref_if_pointer_like(r2)), static_cast<std::remove_cvref_t<decltype((rusty::detail::deref_if_pointer_like(r1) | rusty::detail::deref_if_pointer_like(r2)))>>(randgen_nu_constant_now())) % rusty::detail::deref_if_pointer_like(width)))>>(std::move(x)));
}

uint32_t RandomGenerator::weighted_select(const RandWeightVec& weight_vector) {
    auto rusty_cpp_abi_arg_0 = rusty_cpp_abi_detail_m_410e5b522c6dc9a94f23570cd0ebca4459da8b88f9a5170ba166350adf9474dd::f64_span_from_std_vector(weight_vector);
    return rusty_cpp_abi_sem_m_410e5b522c6dc9a94f23570cd0ebca4459da8b88f9a5170ba166350adf9474dd_RandomGenerator_weighted_select(rusty_cpp_abi_arg_0);
}

void RandomGenerator::destroy() {
    randgen_destroy();
}
/*RUSTYCPP:GEN-END id=rand.generator*/

} // export namespace rrr

namespace rrr {

// The per-thread PRNG seed store (pthread_key plumbing, the raw
// `unsigned int*` seed, rand_r over it, and the pthread_once teardown)
// lives in srpc_rand.c now — plain C, Goal-0 C demotion. None of it
// needed C++, and none of it could ever be inline-Rust DSL.
// The last three shims are DSL too: two `unsafe {}` calls into the
// srpc_rand.c kernels and the historical nu_rand constant, which was
// never mutated and is therefore returned directly. `i32` lowers to
// `int32_t`, which is the same type as the
// `int` in the export-namespace declarations above (same redeclaration
// pattern the sibling logging.cpp already uses for its kernels).
#if RUSTYCPP_RUST
#[allow(unsafe_code)]
unsafe extern "C" {
    fn srpc_rand_raw() -> i32;
    fn srpc_rand_destroy();
}

// @unsafe - thin shim over the C kernel.
#[allow(unsafe_code)]
pub fn randgen_rand_raw() -> i32 {
    unsafe { srpc_rand_raw() }
}

// @safe - the historical nu_rand constant was always zero.
pub fn randgen_nu_constant_now() -> i32 {
    0
}

// @unsafe - thin shim over the C kernel (pthread teardown lives there).
#[allow(unsafe_code)]
pub fn randgen_destroy() {
    unsafe { srpc_rand_destroy(); }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=rand.4 version=1 rust_sha256=e5c4688a4961ef556b302e4c74693f8e92749d12c0a8cb1372659fe76eadc49e*/
int32_t randgen_rand_raw();
int32_t randgen_nu_constant_now();
void randgen_destroy();

extern "C" {
    int32_t srpc_rand_raw();
    void srpc_rand_destroy();
}

int32_t randgen_rand_raw() {
    // @unsafe
    {
        return srpc_rand_raw();
    }
}

int32_t randgen_nu_constant_now() {
    return static_cast<int32_t>(0);
}

void randgen_destroy() {
    // @unsafe
    {
        srpc_rand_destroy();
    }
}
/*RUSTYCPP:GEN-END id=rand.4*/


}
