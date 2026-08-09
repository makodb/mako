//! One-shot value delivery between reactor fibers.
//!
//! This is the valid-Rust owner prepared for the legacy `rrr.future` module.
//! The producer and consumer share one reactor `BoxEvent<T>` through an
//! `Arc`; `FiberPromise::get_future` and `FiberPromise::set_value` are each
//! one-shot operations, while `FiberFuture::get` may copy the stored value any
//! number of times.  A missing state is the observable moved-from/default
//! representation used by the existing C++ API.
//!
//! The two `Cell<bool>` fields are load-bearing at the C++ boundary.  Besides
//! storing the promise retrieval latch, they keep both generated templates
//! non-copyable and movable, matching the current module.  The future's
//! `nc_` cell is intentionally only a move-only marker.  The disabled
//! `cpp_no_fieldwise_ctor` markers keep the generated constructors as the only
//! construction surface, so callers cannot bypass these invariants.

#![allow(unsafe_code, unused_mut, unused_unsafe)]

use cpp::rrr::reactor as cpp_reactor;
use cpp::std as cpp_std;
use std::cell::Cell;
use std::sync::Arc;

// The Mako consumer profile maps this private alias back to `std::pair`, the
// established return type of `make_promise`.  Rust's native carrier is the
// layout-independent tuple returned by the Cargo-only `std::make_pair` shim.
type LegacyStdPair<A, B> = (A, B);

/// Allocate the shared reactor event used by a promise/future pair.
pub fn fiber_make_state<T>() -> Arc<cpp_reactor::BoxEvent<T>> {
    // SAFETY: the reactor factory returns a newly owned event in an Arc.
    unsafe { cpp_reactor::create_sp_box_event::<T>() }
}

/// Construct the empty state used by a default or moved-from future.
pub fn fiber_null_state<T>() -> Option<Arc<cpp_reactor::BoxEvent<T>>> {
    None
}

/// Producer side of a one-shot fiber value.
#[repr(C)]
#[cfg_attr(any(), cpp_no_fieldwise_ctor)]
pub struct FiberPromise<T> {
    pub state_: Option<Arc<cpp_reactor::BoxEvent<T>>>,
    pub future_retrieved_: Cell<bool>,
}

impl<T> Default for FiberPromise<T> {
    #[cfg_attr(any(), cpp_ctor)]
    fn default() -> FiberPromise<T> {
        FiberPromise {
            // SAFETY: the reactor factory returns a newly owned event in an
            // Arc.  Calling it directly here also keeps the generated
            // constructor independent of helper declaration order.
            state_: Some(unsafe { cpp_reactor::create_sp_box_event::<T>() }),
            future_retrieved_: Cell::new(false),
        }
    }
}

impl<T> FiberPromise<T> {
    /// Retrieve the unique consumer handle.
    pub fn get_future(&mut self) -> FiberFuture<T> {
        fiber_promise_get_future(self)
    }

    /// Whether this promise has already delivered its value.
    pub fn is_ready(&self) -> bool {
        if self.state_.is_none() {
            return false;
        }
        let event = self.state_.as_ref().unwrap();
        // SAFETY: the Arc keeps the imported event alive for this const query.
        unsafe { cpp_reactor::BoxEvent::is_ready(&**event) }
    }
}

impl<T: cpp_reactor::LegacyFutureValue> FiberPromise<T> {
    /// Fulfil the promise exactly once.
    pub fn set_value(&mut self, value: &T) {
        assert!(
            self.state_.is_some(),
            "FiberPromise has no state (moved-from?)"
        );
        let event = self.state_.as_ref().unwrap();
        assert!(
            !unsafe { cpp_reactor::BoxEvent::is_ready(&**event) },
            "FiberPromise value already set"
        );
        // SAFETY: `value` remains live for the call and BoxEvent copies it into
        // its owned slot before returning.
        unsafe { cpp_reactor::BoxEvent::set(&**event, value) };
    }
}

/// Consumer side of a one-shot fiber value.
#[repr(C)]
#[cfg_attr(any(), cpp_no_fieldwise_ctor)]
pub struct FiberFuture<T> {
    pub state_: Option<Arc<cpp_reactor::BoxEvent<T>>>,
    pub nc_: Cell<bool>,
}

impl<T> Default for FiberFuture<T> {
    #[cfg_attr(any(), cpp_ctor)]
    fn default() -> FiberFuture<T> {
        FiberFuture {
            state_: None,
            nc_: Cell::new(false),
        }
    }
}

impl<T> FiberFuture<T> {
    /// Wait for at most `timeout_us` microseconds (`0` means indefinitely).
    pub fn wait_for(&mut self, timeout_us: u64) -> bool {
        if self.state_.is_none() {
            return false;
        }
        let event = self.state_.as_ref().unwrap();
        if unsafe { cpp_reactor::BoxEvent::is_ready(&**event) } {
            return true;
        }
        // SAFETY: the Arc retains the event across the reactor suspension.
        unsafe { cpp_reactor::BoxEvent::wait_timeout(&**event, timeout_us) };
        unsafe { cpp_reactor::BoxEvent::is_ready(&**event) }
    }

    /// Whether the paired promise has delivered its value.
    pub fn is_ready(&self) -> bool {
        if self.state_.is_none() {
            return false;
        }
        let event = self.state_.as_ref().unwrap();
        unsafe { cpp_reactor::BoxEvent::is_ready(&**event) }
    }

    /// Whether this handle owns a shared event state.
    pub fn valid(&self) -> bool {
        self.state_.is_some()
    }
}

impl<T: cpp_reactor::LegacyFutureValue> FiberFuture<T> {
    /// Block until ready, then copy out the stored value.
    pub fn get(&mut self) -> T {
        assert!(
            self.state_.is_some(),
            "FiberFuture has no state (invalid or moved-from?)"
        );
        let event = self.state_.as_ref().unwrap();
        if !unsafe { cpp_reactor::BoxEvent::is_ready(&**event) } {
            // SAFETY: the Arc retains the event across the suspension.
            unsafe { cpp_reactor::BoxEvent::wait(&**event) };
        }
        // SAFETY: BoxEvent is ready, and `get` returns an owned copy.
        unsafe { cpp_reactor::BoxEvent::get(&**event) }
    }
}

/// Share a promise's event with its unique future.
pub fn fiber_promise_get_future<T>(self_: &mut FiberPromise<T>) -> FiberFuture<T> {
    assert!(
        !self_.future_retrieved_.get(),
        "FiberFuture already retrieved from FiberPromise"
    );
    self_.future_retrieved_.set(true);
    let mut future: FiberFuture<T> = Default::default();
    future.state_ = self_.state_.clone();
    future
}

/// Create a promise and its unique future in one operation.
pub fn make_promise<T>() -> LegacyStdPair<FiberPromise<T>, FiberFuture<T>> {
    let mut promise: FiberPromise<T> = Default::default();
    // `mut` is load-bearing for the C++ move-only carrier: an immutable Rust
    // local emits `const`, and moving from it would select a deleted copy.
    let mut future: FiberFuture<T> = promise.get_future();
    // SAFETY: the indexed `std::make_pair` consumes the two owned handles and
    // returns the exact legacy `std::pair` carrier.
    unsafe { cpp_std::make_pair(promise, future) }
}

/// Create an immediately-ready future containing `value`.
pub fn make_ready_future<T: cpp_reactor::LegacyFutureValue>(value: T) -> FiberFuture<T> {
    let mut promise: FiberPromise<T> = Default::default();
    let future: FiberFuture<T> = promise.get_future();
    promise.set_value(&value);
    future
}

// Cargo-only implementations of the reserved `cpp::` imports.  The C++
// consumer suppresses this module and resolves each named type/function/member
// directly through the module-local symbol index.
mod cpp {
    pub mod rrr {
        pub mod reactor {
            use std::sync::{Arc, Condvar, Mutex};
            use std::time::Duration;

            pub trait LegacyFutureValue: Clone {}

            impl<T: Clone> LegacyFutureValue for T {}

            pub struct BoxEvent<T> {
                value: Mutex<Option<T>>,
                ready: Condvar,
            }

            impl<T> BoxEvent<T> {
                pub unsafe fn is_ready(&self) -> bool {
                    self.value.lock().unwrap().is_some()
                }

                pub unsafe fn wait(&self) {
                    let mut value = self.value.lock().unwrap();
                    while value.is_none() {
                        value = self.ready.wait(value).unwrap();
                    }
                }

                pub unsafe fn wait_timeout(&self, timeout_us: u64) {
                    if timeout_us == 0_u64 {
                        self.wait();
                        return;
                    }
                    let value = self.value.lock().unwrap();
                    if value.is_none() {
                        let _ = self
                            .ready
                            .wait_timeout(value, Duration::from_micros(timeout_us))
                            .unwrap();
                    }
                }
            }

            impl<T: Clone> BoxEvent<T> {
                pub unsafe fn set(&self, value: &T) {
                    *self.value.lock().unwrap() = Some(value.clone());
                    self.ready.notify_all();
                }

                pub unsafe fn get(&self) -> T {
                    self.value.lock().unwrap().as_ref().unwrap().clone()
                }
            }

            pub unsafe fn create_sp_box_event<T>() -> Arc<BoxEvent<T>> {
                Arc::new(BoxEvent {
                    value: Mutex::new(None),
                    ready: Condvar::new(),
                })
            }
        }
    }

    pub mod std {
        pub unsafe fn make_pair<A, B>(first: A, second: B) -> (A, B) {
            (first, second)
        }
    }
}
