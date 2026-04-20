module;
/**
 * @file fiber.h
 * @brief Modern fiber API following Boost.Fiber conventions.
 *
 * This file provides a cleaner API for working with stackful fibers,
 * using "fiber" terminology that aligns with industry conventions.
 *
 * Key differences from C++20 coroutines:
 *   - C++20 coroutines are stackless (state machines)
 *   - Our implementation uses a custom C++ runtime with assembly context switching
 *   - Stackful execution contexts are properly called "fibers"
 *
 * Example usage:
 *   // Create and run a fiber
 *   auto fiber = Fiber::create_run([]{
 *       this_fiber::sleep_ms(100);
 *       this_fiber::yield();
 *   });
 *
 *   // Get current fiber ID
 *   auto id = this_fiber::get_id();
 *
 * Note: This header uses rrr::Time for timing, NOT std::chrono.
 */


#include <rusty/option.hpp>
#include <rusty/rc.hpp>
#include <cstdint>

export module rrr:reactor.fiber;

import :reactor.fiber_impl;
import :reactor.event;
import :reactor.future;
import :base.basetypes;

export namespace rrr {

// =============================================================================
// Fiber is the primary class for stackful fibers
// =============================================================================
// Fiber class is defined in fiber_impl.h.
// WaitAll, WaitAny, WaitN are defined in event.h

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
 *
 * @safe - Uses only safe rusty::Option operations
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
 *
 * @return Some(fiber) if in fiber context, None otherwise
 *
 * @safe - Delegates to Fiber::current_fiber()
 */
inline rusty::Option<rusty::Rc<Fiber>> current() noexcept {
    return Fiber::current_fiber();
}

/**
 * Check if currently executing within a fiber context.
 *
 * @return true if in fiber context, false otherwise
 *
 * @safe - Uses only safe Option operations
 */
inline bool in_fiber_context() noexcept {
    return Fiber::current_fiber().is_some();
}

/**
 * Yield execution to other fibers.
 *
 * This allows the reactor to run other ready fibers before returning
 * to the current fiber.
 *
 * @note No-op if called outside fiber context
 *
 * @unsafe - Delegates to fiber context switch yield
 */
inline void yield() noexcept {
    auto fiber = Fiber::current_fiber();
    if (fiber.is_some()) {
        // @unsafe { fiber context switch yield }
        fiber.unwrap()->yield_();
    }
}

/**
 * Sleep for specified microseconds.
 *
 * Uses rrr::Time internally (NOT std::chrono).
 *
 * @param microseconds Duration to sleep in microseconds
 *
 * @unsafe - Calls Fiber::sleep which uses Time internally
 */
inline void sleep_us(uint64_t microseconds) {
    // @unsafe { Fiber::sleep uses Time internally }
    Fiber::sleep(microseconds);
}

/**
 * Sleep for specified milliseconds.
 *
 * Convenience wrapper - uses rrr::Time internally (NOT std::chrono).
 *
 * @param milliseconds Duration to sleep in milliseconds
 *
 * @unsafe - Calls Fiber::sleep which uses Time internally
 */
inline void sleep_ms(uint64_t milliseconds) {
    // @unsafe { Fiber::sleep }
    Fiber::sleep(milliseconds * 1000);
}

/**
 * Sleep for specified seconds.
 *
 * Convenience wrapper - uses rrr::Time internally (NOT std::chrono).
 *
 * @param seconds Duration to sleep in seconds
 *
 * @unsafe - Calls Fiber::sleep which uses Time internally
 */
inline void sleep_s(uint64_t seconds) {
    // @unsafe { Fiber::sleep }
    Fiber::sleep(seconds * Time::RRR_USEC_PER_SEC);
}

/**
 * Sleep until specified absolute time (microseconds since epoch).
 *
 * If the specified time has already passed, returns immediately.
 * Uses rrr::Time::now() for current time (NOT std::chrono).
 *
 * @param abs_time_us Absolute time in microseconds since epoch
 *
 * @unsafe - Calls Time::now() and Fiber::sleep
 */
inline void sleep_until_us(uint64_t abs_time_us) {
    // @unsafe { Time::now }
    uint64_t now = Time::now(true);
    if (abs_time_us > now) {
        // @unsafe { Fiber::sleep }
        Fiber::sleep(abs_time_us - now);
    }
}

}  // namespace this_fiber

}  // namespace rrr
