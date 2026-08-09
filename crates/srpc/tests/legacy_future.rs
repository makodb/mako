#![allow(unsafe_code)]

use srpc::runtime::legacy_future::{
    fiber_make_state, fiber_null_state, make_promise, make_ready_future, FiberFuture, FiberPromise,
};
use std::mem::{align_of, offset_of, size_of};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::thread;
use std::time::{Duration, Instant};

#[test]
fn default_construction_and_layout_preserve_the_two_field_handles() {
    let promise = FiberPromise::<u64>::default();
    let future = FiberFuture::<u64>::default();

    assert!(promise.state_.is_some());
    assert!(!promise.future_retrieved_.get());
    assert!(future.state_.is_none());
    assert!(!future.nc_.get());

    assert_eq!(offset_of!(FiberPromise<u64>, state_), 0_usize);
    assert_eq!(offset_of!(FiberFuture<u64>, state_), 0_usize);
    assert_eq!(
        size_of::<FiberPromise<u64>>(),
        size_of::<FiberFuture<u64>>()
    );
    assert_eq!(
        align_of::<FiberPromise<u64>>(),
        align_of::<FiberFuture<u64>>()
    );

    let owner = include_str!("../src/runtime/legacy_future.rs");
    assert_eq!(
        owner
            .matches("#[cfg_attr(any(), cpp_no_fieldwise_ctor)]")
            .count(),
        2_usize
    );
    assert_eq!(
        owner.matches("#[cfg_attr(any(), cpp_ctor)]").count(),
        2_usize
    );
    assert!(owner.contains("pub nc_: Cell<bool>"));
}

#[test]
fn future_retrieval_and_value_delivery_are_each_one_shot() {
    let mut promise = FiberPromise::<i32>::default();
    let mut future = promise.get_future();
    assert!(future.valid());
    assert!(!future.is_ready());

    let second_future = catch_unwind(AssertUnwindSafe(|| promise.get_future()));
    assert!(second_future.is_err());

    promise.set_value(&42_i32);
    assert!(promise.is_ready());
    assert!(future.is_ready());
    assert_eq!(future.get(), 42_i32);
    assert_eq!(future.get(), 42_i32);

    let second_value = catch_unwind(AssertUnwindSafe(|| promise.set_value(&7_i32)));
    assert!(second_value.is_err());
}

#[test]
fn wait_for_times_out_then_get_wakes_after_cross_thread_set() {
    let mut promise = FiberPromise::<String>::default();
    let mut future = promise.get_future();

    let start = Instant::now();
    assert!(!future.wait_for(2_000_u64));
    assert!(start.elapsed() >= Duration::from_micros(1_000_u64));

    let producer = thread::spawn(move || {
        thread::sleep(Duration::from_millis(10_u64));
        promise.set_value(&"ready".to_owned());
    });
    assert_eq!(future.get(), "ready");
    producer.join().unwrap();
}

#[test]
fn convenience_factories_and_state_helpers_retain_the_public_surface() {
    let (mut promise, mut future) = make_promise::<Vec<i32>>();
    assert!(future.valid());
    promise.set_value(&vec![1_i32, 2_i32, 3_i32]);
    assert_eq!(future.get(), [1_i32, 2_i32, 3_i32]);

    let mut ready = make_ready_future(String::from("done"));
    assert!(ready.wait_for(1_u64));
    assert_eq!(ready.get(), "done");

    assert!(fiber_null_state::<u8>().is_none());
    let state = fiber_make_state::<u8>();
    assert!(!unsafe { state.is_ready() });
}

#[test]
fn exact_current_api_and_foreign_event_boundary_are_pinned_in_source() {
    let owner = include_str!("../src/runtime/legacy_future.rs");

    for required in [
        "pub fn fiber_make_state<T>()",
        "pub fn fiber_null_state<T>()",
        "pub struct FiberPromise<T>",
        "pub struct FiberFuture<T>",
        "pub fn fiber_promise_get_future<T>",
        "pub fn make_promise<T>()",
        "pub fn make_ready_future<T: cpp_reactor::LegacyFutureValue>",
        "pub trait LegacyFutureValue: Clone",
        "impl<T: Clone> LegacyFutureValue for T",
        "cpp_reactor::BoxEvent::is_ready",
        "cpp_reactor::BoxEvent::set",
        "cpp_reactor::BoxEvent::wait_timeout",
        "cpp_reactor::BoxEvent::wait",
        "cpp_reactor::BoxEvent::get",
        "cpp_std::make_pair",
    ] {
        assert!(
            owner.contains(required),
            "missing owner surface: {required}"
        );
    }

    assert_eq!(owner.matches("pub fn get_future").count(), 1_usize);
    assert_eq!(owner.matches("pub fn set_value").count(), 1_usize);
    assert_eq!(owner.matches("pub fn get(&mut self)").count(), 1_usize);
    assert_eq!(owner.matches("pub fn wait_for").count(), 1_usize);
    assert_eq!(owner.matches("pub fn is_ready").count(), 2_usize);
    assert_eq!(owner.matches("pub fn valid").count(), 1_usize);
    assert_eq!(
        owner.matches("cpp_reactor::LegacyFutureValue").count(),
        3_usize
    );
}
