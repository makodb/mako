// Canonical Rust source for the rrr.fiber module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.

#![allow(unsafe_code)]

/// Operations on the reactor fiber currently installed on this thread.
pub mod this_fiber {
    use cpp::rrr::basetypes as cpp_basetypes;
    use cpp::rrr::reactor as cpp_reactor;
    use rusty as cpp;
    use std::rc::Rc;

    /// Return the running fiber's id, or zero outside fiber context.
    pub fn get_id() -> u64 {
        // SAFETY: reading the reactor's thread-local current-fiber handle has
        // no caller-side precondition.
        let fiber: Option<Rc<rusty::ReactorFiber>> = unsafe { cpp_reactor::Fiber::current_fiber() };
        if let Some(fiber) = fiber {
            return fiber.id.get();
        }
        0_u64
    }

    /// Return the running fiber, if this thread is in fiber context.
    pub fn current() -> Option<Rc<rusty::ReactorFiber>> {
        // SAFETY: reading the reactor's thread-local current-fiber handle has
        // no caller-side precondition.
        unsafe { cpp_reactor::Fiber::current_fiber() }
    }

    /// Whether this thread is currently executing in a fiber context.
    pub fn in_fiber_context() -> bool {
        // SAFETY: reading the reactor's thread-local current-fiber handle has
        // no caller-side precondition.
        unsafe { cpp_reactor::Fiber::current_fiber() }.is_some()
    }

    /// Cooperatively yield to another ready fiber; outside a fiber this is a
    /// no-op. The raw identifier retains the public C++ spelling `yield`.
    #[allow(clippy::explicit_auto_deref)]
    pub fn r#yield() {
        // SAFETY: reading the reactor's thread-local current-fiber handle has
        // no caller-side precondition.
        let fiber: Option<Rc<rusty::ReactorFiber>> = unsafe { cpp_reactor::Fiber::current_fiber() };
        if let Some(fiber) = fiber {
            // SAFETY: `fiber` is held alive by the current-fiber `Rc`.
            unsafe { cpp_reactor::Fiber::yield_(&*fiber) };
        }
    }

    /// Suspend the running fiber for `microseconds`.
    pub fn sleep_us(microseconds: u64) {
        // SAFETY: the reactor accepts every microsecond duration.
        unsafe { cpp_reactor::fiber_sleep(microseconds) };
    }

    /// Suspend the running fiber for `milliseconds`.
    pub fn sleep_ms(milliseconds: u64) {
        // SAFETY: the reactor accepts every microsecond duration.
        unsafe { cpp_reactor::fiber_sleep(milliseconds.wrapping_mul(1_000_u64)) };
    }

    /// Suspend the running fiber for `seconds`.
    pub fn sleep_s(seconds: u64) {
        // SAFETY: the reactor accepts every microsecond duration.
        unsafe { cpp_reactor::fiber_sleep(seconds.wrapping_mul(1_000_000_u64)) };
    }

    /// Suspend until an absolute microsecond deadline. Past deadlines return
    /// immediately without entering the scheduler.
    pub fn sleep_until_us(abs_time_us: u64) {
        // SAFETY: the monotonic-clock selector has no caller precondition.
        let now: u64 = unsafe { cpp_basetypes::Time::now(true) };
        if abs_time_us > now {
            // SAFETY: the subtraction is guarded and yields a valid duration.
            unsafe { cpp_reactor::fiber_sleep(abs_time_us - now) };
        }
    }
}
