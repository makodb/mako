// Canonical Rust source for the srpc.fiber module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.

#![allow(unsafe_code)]

#[allow(unused_imports)]
use crate::reactor as _;

/// Operations on the reactor fiber currently installed on this thread.
pub mod this_fiber {
    use crate::basetypes::Time;
    use std::rc::Rc;

    /// Return the running fiber's id, or zero outside fiber context.
    pub fn get_id() -> u64 {
        let fiber: Option<Rc<crate::reactor::Fiber>> = crate::reactor::Fiber::current_fiber();
        if let Some(fiber) = fiber {
            return fiber.id.get();
        }
        0_u64
    }

    /// Return the running fiber, if this thread is in fiber context.
    pub fn current() -> Option<Rc<crate::reactor::Fiber>> {
        crate::reactor::Fiber::current_fiber()
    }

    /// Whether this thread is currently executing in a fiber context.
    pub fn in_fiber_context() -> bool {
        crate::reactor::Fiber::current_fiber().is_some()
    }

    /// Suspend until the owner resumes this fiber; outside a fiber this is a
    /// no-op. The raw identifier retains the public C++ spelling `yield`.
    // clippy::explicit_auto_deref -- measured 2026-09-11 (clippy 0.1.97, rusty-cpp 3e1d9505): taking it lowers `fiber.yield_()` to `(&fiber)->yield_()` in place of `deref_if_pointer_like(fiber).yield_()` (2 emitted lines in srpc.fiber.cppm).
    #[allow(clippy::explicit_auto_deref)]
    pub fn r#yield() {
        let fiber: Option<Rc<crate::reactor::Fiber>> = crate::reactor::Fiber::current_fiber();
        if let Some(fiber) = fiber {
            crate::reactor::Fiber::yield_(&*fiber);
        }
    }

    /// Suspend the running fiber for `microseconds`.
    pub fn sleep_us(microseconds: u64) {
        crate::reactor::fiber_sleep(microseconds);
    }

    /// Suspend the running fiber for `milliseconds`.
    pub fn sleep_ms(milliseconds: u64) {
        crate::reactor::fiber_sleep(milliseconds.wrapping_mul(1_000_u64));
    }

    /// Suspend the running fiber for `seconds`.
    pub fn sleep_s(seconds: u64) {
        crate::reactor::fiber_sleep(seconds.wrapping_mul(1_000_000_u64));
    }

    /// Suspend until an absolute microsecond deadline. Past deadlines return
    /// immediately without entering the scheduler.
    pub fn sleep_until_us(abs_time_us: u64) {
        let now: u64 = Time::now(true);
        if abs_time_us > now {
            crate::reactor::fiber_sleep(abs_time_us - now);
        }
    }
}
