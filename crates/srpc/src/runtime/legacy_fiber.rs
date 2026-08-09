//! Convenience operations on the currently executing reactor fiber.
//!
//! This is the valid-Rust owner prepared for `rrr.fiber`.  The actual `Fiber`
//! and scheduler remain owned by `rrr.reactor`; these wrappers retain the
//! small `this_fiber` namespace consumed by C++.  Time comes from the prepared
//! `crate::base::legacy_basetypes` owner so the eventual generated module has a
//! normal Rust-owned dependency on `rrr.basetypes` rather than a second time
//! implementation.

#![allow(unsafe_code, unused_unsafe)]

use cpp::rrr::reactor as cpp_reactor;
use std::rc::Rc;

// The profile maps this private alias to the exact imported reactor type.  It
// also gives nested `this_fiber` functions a valid Rust spelling without
// losing reserved-`cpp::` type projection during C++ emission.
type LegacyFiber = cpp_reactor::Fiber;

struct LegacyFiberOps;

impl LegacyFiberOps {
    fn current() -> Option<Rc<cpp_reactor::Fiber>> {
        unsafe { cpp_reactor::Fiber::current_fiber() }
    }

    fn yield_(fiber: &cpp_reactor::Fiber) {
        unsafe { cpp_reactor::Fiber::yield_(fiber) };
    }

    fn sleep(microseconds: u64) {
        unsafe { cpp_reactor::fiber_sleep(microseconds) };
    }
}

/// Operations on the reactor fiber currently installed on this thread.
pub mod this_fiber {
    use super::{LegacyFiber, LegacyFiberOps};
    use crate::base::legacy_basetypes::{Time, RRR_USEC_PER_SEC};
    use std::rc::Rc;

    /// Return the running fiber's id, or zero outside fiber context.
    pub fn get_id() -> u64 {
        let fiber: Option<Rc<LegacyFiber>> = LegacyFiberOps::current();
        if let Some(fiber) = fiber {
            return fiber.id.get();
        }
        0_u64
    }

    /// Return the running fiber, if this thread is in fiber context.
    pub fn current() -> Option<Rc<LegacyFiber>> {
        LegacyFiberOps::current()
    }

    /// Whether this thread is currently executing a reactor fiber.
    pub fn in_fiber_context() -> bool {
        LegacyFiberOps::current().is_some()
    }

    /// Cooperatively yield to another ready fiber; outside context this is a
    /// no-op.  The raw identifier retains the public C++ spelling `yield`.
    pub fn r#yield() {
        let fiber: Option<Rc<LegacyFiber>> = LegacyFiberOps::current();
        if let Some(fiber) = fiber {
            LegacyFiberOps::yield_(&*fiber);
        }
    }

    /// Suspend the running fiber for `microseconds`.
    pub fn sleep_us(microseconds: u64) {
        LegacyFiberOps::sleep(microseconds);
    }

    /// Suspend the running fiber for `milliseconds`.
    pub fn sleep_ms(milliseconds: u64) {
        LegacyFiberOps::sleep(milliseconds * 1_000_u64);
    }

    /// Suspend the running fiber for `seconds`.
    pub fn sleep_s(seconds: u64) {
        LegacyFiberOps::sleep(seconds * RRR_USEC_PER_SEC);
    }

    /// Suspend until an absolute microsecond deadline.  Past deadlines return
    /// immediately without entering the scheduler.
    pub fn sleep_until_us(abs_time_us: u64) {
        let now: u64 = Time::now(true);
        if abs_time_us > now {
            LegacyFiberOps::sleep(abs_time_us - now);
        }
    }
}

// Cargo-only implementation of the reserved reactor import.  The C++
// consumer suppresses this module and resolves Fiber plus its three indexed
// methods directly from `rrr.reactor`.
pub(crate) mod cpp {
    pub mod rrr {
        pub mod reactor {
            use std::cell::{Cell, RefCell};
            use std::rc::Rc;

            pub struct Fiber {
                pub id: Cell<u64>,
                yields: Cell<u64>,
            }

            thread_local! {
                static CURRENT: RefCell<Option<Rc<Fiber>>> = const { RefCell::new(None) };
                static SLEEP_CALLS: RefCell<Vec<u64>> = const { RefCell::new(Vec::new()) };
            }

            impl Fiber {
                pub unsafe fn current_fiber() -> Option<Rc<Fiber>> {
                    CURRENT.with(|slot| slot.borrow().clone())
                }

                pub unsafe fn yield_(&self) {
                    self.yields.set(self.yields.get() + 1_u64);
                }
            }

            pub unsafe fn fiber_sleep(microseconds: u64) {
                SLEEP_CALLS.with(|calls| calls.borrow_mut().push(microseconds));
            }

            #[cfg(test)]
            #[allow(dead_code)]
            pub fn with_test_fiber<R>(id: u64, body: impl FnOnce() -> R) -> (R, u64) {
                let fiber = Rc::new(Fiber {
                    id: Cell::new(id),
                    yields: Cell::new(0_u64),
                });
                let previous = CURRENT.with(|slot| slot.replace(Some(Rc::clone(&fiber))));
                let result = body();
                CURRENT.with(|slot| {
                    slot.replace(previous);
                });
                (result, fiber.yields.get())
            }

            #[cfg(test)]
            #[allow(dead_code)]
            pub fn take_test_sleep_calls() -> Vec<u64> {
                SLEEP_CALLS.with(|calls| core::mem::take(&mut *calls.borrow_mut()))
            }
        }
    }
}
