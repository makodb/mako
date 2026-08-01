// rrr.fiber — `this_fiber` namespace helpers (formerly fiber.h).
//
// Thin inline wrappers around `Fiber::current_fiber()` / `Fiber::sleep()`,
// mirroring `std::this_thread`'s shape. The Fiber class itself lives in
// rrr.reactor; consumers that just need the `this_fiber::*` helpers
// import rrr.fiber and avoid pulling the rest of the reactor BMI.
module;

#include <cstddef>
#include <cstdint>

#include <rusty/option.hpp>
#include <rusty/slice.hpp>

export module rrr.fiber;

import std;
import rusty;
import rrr.basetypes;
import rrr.reactor;

export namespace rrr {

// =============================================================================
// this_fiber Namespace (like std::this_thread for threads)
// =============================================================================

/**
 * Namespace for operations on the currently executing fiber.
 *
 * These functions operate on the current fiber context and should only be
 * called from within a fiber (not from the main thread).
 *
 * Note: All time-based functions use rrr::Time internally, NOT std::chrono.
 */
namespace this_fiber {

/**
 * Get the ID of the currently executing fiber.
 *
 * Authored as inline Rust DSL. The comment here used to say this could
 * not be migrated because the transpiler emitted `.id` rather than
 * `->id` for `Rc<T>` field access — that lowering exists now
 * (`rc.field` -> `(*rc).field`), provided the binding's type is known,
 * hence the annotation below.
 *
 * @return Fiber ID (uint64_t), or 0 if called outside fiber context
 */
#if RUSTYCPP_RUST
fn get_id() -> u64 {
    // Annotated: current_fiber() is a C++ static, so the transpiler cannot
    // infer Option<Rc<Fiber>> and would emit `.id` on the Rc (playbook §7.13).
    let fiber: rusty::Option<rusty::Rc<Fiber>> = Fiber::current_fiber();
    if fiber.is_some() {
        return fiber.unwrap().id.get();
    }
    0u64
}
#endif
/*RUSTYCPP:GEN-BEGIN id=fiber.get_id version=1 rust_sha256=a403e0ef3bfeefa596ae162f66f6b116433aaa1105fe3fe412c5b7ca0419dc0e*/
uint64_t get_id();

uint64_t get_id() {
    rusty::Option<rusty::Rc<Fiber>> fiber = Fiber::current_fiber();
    if (fiber.is_some()) {
        return (*fiber.unwrap()).id.get();
    }
    return static_cast<uint64_t>(0);
}
/*RUSTYCPP:GEN-END id=fiber.get_id*/

/**
 * Get the currently executing fiber as an Option<Rc<Fiber>>.
 *
 * Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
 * the source of truth; the transpiler regenerates the matching
 * `RUSTYCPP:GEN-BEGIN ... END` block at the current `this_fiber`
 * namespace scope.
 */
#if RUSTYCPP_RUST
fn current() -> rusty::Option<rusty::Rc<Fiber>> {
    Fiber::current_fiber()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=fiber.current version=1 rust_sha256=1f556ee94fd36c0e239e7d574a31fac11bca2ba749073c1221605616e2db6adc*/
rusty::Option<rusty::Rc<Fiber>> current() {
    return Fiber::current_fiber();
}
/*RUSTYCPP:GEN-END id=fiber.current*/

/**
 * Check if currently executing within a fiber context.
 *
 * Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
 * the source of truth; the transpiler regenerates the matching
 * `RUSTYCPP:GEN-BEGIN ... END` block at the current `this_fiber`
 * namespace scope.
 */
#if RUSTYCPP_RUST
fn in_fiber_context() -> bool {
    Fiber::current_fiber().is_some()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=fiber.in_fiber_context version=1 rust_sha256=27ba5af1244c52d2db2455a7c0f1dfe62e8808ac84aa099afaa3af2b7d7f6c47*/
bool in_fiber_context();

bool in_fiber_context() {
    return Fiber::current_fiber().is_some();
}
/*RUSTYCPP:GEN-END id=fiber.in_fiber_context*/

/**
 * Yield execution to other fibers.
 *
 * This allows the reactor to run other ready fibers before returning
 * to the current fiber. No-op if called outside fiber context.
 */
inline void yield() noexcept {
    auto fiber = Fiber::current_fiber();
    if (fiber.is_some()) {
        fiber.unwrap()->yield_();
    }
}

/**
 * Sleep helpers (sleep_us / sleep_ms / sleep_s / sleep_until_us).
 *
 * Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
 * the source of truth; the transpiler regenerates the matching
 * `RUSTYCPP:GEN-BEGIN ... END` block at the current `this_fiber`
 * namespace scope.
 *
 * Note: All time-based helpers route through rrr::Time / Fiber::sleep,
 * which themselves flow through `rusty::sys::time::clock_monotonic_us`
 * (@safe) — no raw `clock_gettime` or `std::chrono` usage here.
 */
#if RUSTYCPP_RUST
fn sleep_us(microseconds: u64) {
    Fiber::sleep(microseconds);
}

fn sleep_ms(milliseconds: u64) {
    Fiber::sleep(milliseconds * 1000);
}

fn sleep_s(seconds: u64) {
    Fiber::sleep(seconds * RRR_USEC_PER_SEC);
}

fn sleep_until_us(abs_time_us: u64) {
    let now: u64 = Time::now(true);
    if abs_time_us > now {
        Fiber::sleep(abs_time_us - now);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=fiber.sleep_helpers version=1 rust_sha256=dd2308e48ef406df63c0e8381aea6ece1a37660fb85055b6941454468052f75c*/
void sleep_us(uint64_t microseconds);
void sleep_ms(uint64_t milliseconds);
void sleep_s(uint64_t seconds);
void sleep_until_us(uint64_t abs_time_us);

void sleep_us(uint64_t microseconds) {
    Fiber::sleep(std::move(microseconds));
}

void sleep_ms(uint64_t milliseconds) {
    Fiber::sleep(rusty::detail::deref_if_pointer_like(milliseconds) * 1000);
}

void sleep_s(uint64_t seconds) {
    Fiber::sleep(rusty::detail::deref_if_pointer_like(seconds) * rusty::detail::deref_if_pointer_like(RRR_USEC_PER_SEC));
}

void sleep_until_us(uint64_t abs_time_us) {
    const uint64_t now = Time::now(true);
    if (rusty::detail::deref_if_pointer_like(abs_time_us) > rusty::detail::deref_if_pointer_like(now)) {
        Fiber::sleep(rusty::detail::deref_if_pointer_like(abs_time_us) - rusty::detail::deref_if_pointer_like(now));
    }
}
/*RUSTYCPP:GEN-END id=fiber.sleep_helpers*/

}  // namespace this_fiber

}  // export namespace rrr
