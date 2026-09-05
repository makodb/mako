// Canonical Rust source for the srpc.future module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.
#![allow(clippy::explicit_auto_deref)]

use std::cell::Cell;
use std::sync::Arc;

// `rusty` is the rustc-only facade package. The emitter drops this alias and
// treats the following `cpp::` paths as checked C++ named-module imports.
use cpp::srpc::reactor as cpp_reactor;
use cpp::std as cpp_std;
use rusty as cpp;

/// Construct the `BoxEvent<T>` state owned by a new promise.
#[allow(unsafe_code)]
pub fn fiber_make_state<T>() -> Arc<rusty::ReactorBoxEvent<T>> {
    // SAFETY: `create_sp_box_event<T>` has no caller-side safety
    // precondition; the explicit block records the foreign C++ module call.
    unsafe { cpp_reactor::create_sp_box_event::<T>() }
}

/// Construct the empty state used by a default/moved-from future.
pub fn fiber_null_state<T>() -> Option<Arc<rusty::ReactorBoxEvent<T>>> {
    None
}

#[repr(C)]
pub struct FiberPromise<T> {
    pub state_: Option<Arc<rusty::ReactorBoxEvent<T>>>,
    pub future_retrieved_: Cell<bool>,
}

impl<T> Default for FiberPromise<T> {
    #[allow(unsafe_code)]
    fn default() -> FiberPromise<T> {
        FiberPromise {
            // SAFETY: the foreign factory has no caller-side precondition.
            state_: Some(unsafe { cpp_reactor::create_sp_box_event::<T>() }),
            future_retrieved_: Cell::new(false),
        }
    }
}

impl<T> FiberPromise<T> {
    pub fn get_future(&mut self) -> FiberFuture<T> {
        fiber_promise_get_future(self)
    }

    pub fn set_value(&mut self, value: &T)
    where
        T: Clone,
    {
        assert!(
            self.state_.is_some(),
            "FiberPromise has no state (moved-from?)"
        );
        let ev = self.state_.as_ref().unwrap();
        assert!(!(*ev).is_set_.get(), "FiberPromise value already set");
        (*ev).set(value);
    }

    pub fn is_ready(&self) -> bool {
        if self.state_.is_none() {
            return false;
        }
        let ev = self.state_.as_ref().unwrap();
        (*ev).is_set_.get()
    }
}

#[repr(C)]
pub struct FiberFuture<T> {
    pub state_: Option<Arc<rusty::ReactorBoxEvent<T>>>,
    pub nc_: Cell<bool>,
}

impl<T> Default for FiberFuture<T> {
    fn default() -> FiberFuture<T> {
        FiberFuture {
            state_: None,
            nc_: Cell::new(false),
        }
    }
}

impl<T> FiberFuture<T> {
    pub fn get(&mut self) -> T
    where
        T: Clone,
    {
        assert!(
            self.state_.is_some(),
            "FiberFuture has no state (invalid or moved-from?)"
        );
        let ev = self.state_.as_ref().unwrap();
        if !(*ev).is_set_.get() {
            (*ev).wait();
        }
        (*ev).get()
    }

    /// A timeout of zero waits indefinitely, matching `Event::wait`.
    pub fn wait_for(&mut self, timeout_us: u64) -> bool {
        if self.state_.is_none() {
            return false;
        }
        let ev = self.state_.as_ref().unwrap();
        if (*ev).is_set_.get() {
            return true;
        }
        (*ev).wait_timeout(timeout_us);
        (*ev).is_set_.get()
    }

    pub fn is_ready(&self) -> bool {
        if self.state_.is_none() {
            return false;
        }
        let ev = self.state_.as_ref().unwrap();
        (*ev).is_set_.get()
    }

    pub fn valid(&self) -> bool {
        self.state_.is_some()
    }
}

/// Retrieve the unique future and share the promise's event state with it.
#[allow(clippy::field_reassign_with_default)]
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

/// Create a promise/future pair sharing one event state.
#[allow(unsafe_code)]
#[allow(unused_mut)]
pub fn make_promise<T>() -> rusty::StdPair<FiberPromise<T>, FiberFuture<T>> {
    let mut promise: FiberPromise<T> = Default::default();
    // `mut` is load-bearing for C++: it prevents std::move from degrading to
    // a deleted copy when the move-only future enters std::make_pair.
    let mut future: FiberFuture<T> = promise.get_future();
    // SAFETY: `std::make_pair` has no caller-side safety precondition; the
    // explicit block records the checked foreign C++ module call.
    unsafe { cpp_std::make_pair(promise, future) }
}

/// Create a future whose value has already been delivered.
pub fn make_ready_future<T>(value: T) -> FiberFuture<T> {
    let mut promise: FiberPromise<T> = Default::default();
    let future: FiberFuture<T> = promise.get_future();
    let ev = promise.state_.as_ref().unwrap();
    (*ev).set(value);
    future
}
