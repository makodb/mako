// Canonical Rust source for the srpc.future module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.
#![allow(clippy::explicit_auto_deref)]

use std::cell::Cell;
use std::sync::Arc;

#[allow(unused_imports)]
use crate::reactor as _;

/// Construct the `BoxEvent<T>` state owned by a new promise.
pub fn fiber_make_state<T: Clone + Default + 'static>() -> Arc<crate::reactor::BoxEvent<T>> {
    crate::reactor::create_sp_box_event::<T>()
}

/// Construct the empty state used by a default/moved-from future.
pub fn fiber_null_state<T>() -> Option<Arc<crate::reactor::BoxEvent<T>>> {
    None
}

#[repr(C)]
pub struct FiberPromise<T> {
    pub state_: Option<Arc<crate::reactor::BoxEvent<T>>>,
    pub future_retrieved_: Cell<bool>,
}

impl<T: Clone + Default + 'static> Default for FiberPromise<T> {
    fn default() -> FiberPromise<T> {
        FiberPromise {
            state_: Some(crate::reactor::create_sp_box_event::<T>()),
            future_retrieved_: Cell::new(false),
        }
    }
}

impl<T: Clone + Default + 'static> FiberPromise<T> {
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
    pub state_: Option<Arc<crate::reactor::BoxEvent<T>>>,
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

impl<T: Clone + Default + 'static> FiberFuture<T> {
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
pub fn fiber_promise_get_future<T: Clone + Default + 'static>(self_: &mut FiberPromise<T>) -> FiberFuture<T> {
    assert!(
        !self_.future_retrieved_.get(),
        "FiberFuture already retrieved from FiberPromise"
    );
    self_.future_retrieved_.set(true);
    let mut future: FiberFuture<T> = FiberFuture::<T>::default();
    future.state_ = self_.state_.clone();
    future
}

/// The Rust tuple keeps the established std::pair C++ profile.
pub type PromisePair<Promise, Future> = (Promise, Future);

/// Create a promise/future pair sharing one event state.
#[allow(unused_mut)]
pub fn make_promise<T: Clone + Default + 'static>() -> PromisePair<FiberPromise<T>, FiberFuture<T>> {
    let mut promise: FiberPromise<T> = FiberPromise::<T>::default();
    // `mut` is load-bearing for C++: it prevents std::move from degrading to
    // a deleted copy when the move-only future enters the pair.
    let mut future: FiberFuture<T> = promise.get_future();
    (promise, future)
}

/// Create a future whose value has already been delivered.
pub fn make_ready_future<T: Clone + Default + 'static>(value: T) -> FiberFuture<T> {
    let mut promise: FiberPromise<T> = FiberPromise::<T>::default();
    let future: FiberFuture<T> = promise.get_future();
    let ev = promise.state_.as_ref().unwrap();
    (*ev).set(&value);
    future
}
