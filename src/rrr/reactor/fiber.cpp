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
#include <rusty/rc.hpp>

export module rrr.fiber;

import std;
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
 * @return Fiber ID (uint64_t), or 0 if called outside fiber context
 */
inline uint64_t get_id() noexcept {
    auto fiber = Fiber::current_fiber();
    if (fiber.is_some()) {
        // @unsafe { accessing Rc internals }
        return fiber.unwrap()->id;
    }
    return 0;
}

/**
 * Get the currently executing fiber as an Option<Rc<Fiber>>.
 */
inline rusty::Option<rusty::Rc<Fiber>> current() noexcept {
    return Fiber::current_fiber();
}

/**
 * Check if currently executing within a fiber context.
 */
inline bool in_fiber_context() noexcept {
    return Fiber::current_fiber().is_some();
}

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
 * Sleep for specified microseconds. Uses rrr::Time internally.
 */
inline void sleep_us(uint64_t microseconds) {
    Fiber::sleep(microseconds);
}

/**
 * Sleep for specified milliseconds. Uses rrr::Time internally.
 */
inline void sleep_ms(uint64_t milliseconds) {
    Fiber::sleep(milliseconds * 1000);
}

/**
 * Sleep for specified seconds. Uses rrr::Time internally.
 */
inline void sleep_s(uint64_t seconds) {
    Fiber::sleep(seconds * Time::RRR_USEC_PER_SEC);
}

/**
 * Sleep until specified absolute time (microseconds since epoch).
 * If the time has already passed, returns immediately.
 */
inline void sleep_until_us(uint64_t abs_time_us) {
    // @unsafe { Time::now }
    uint64_t now = Time::now(true);
    if (abs_time_us > now) {
        Fiber::sleep(abs_time_us - now);
    }
}

}  // namespace this_fiber

}  // export namespace rrr
