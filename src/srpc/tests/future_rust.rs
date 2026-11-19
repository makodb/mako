use srpc::future::{make_promise, make_ready_future, FiberFuture, FiberPromise};

#[test]
fn promise_future_delivery_is_one_shot_and_repeatable_to_read() {
    let mut promise = FiberPromise::<String>::default();
    assert!(!promise.is_ready());

    let mut future = promise.get_future();
    assert!(future.valid());
    assert!(!future.is_ready());

    promise.set_value(&"hello".to_owned());
    assert!(promise.is_ready());
    assert!(future.is_ready());
    assert!(future.wait_for(1));
    assert_eq!(future.get(), "hello");
    assert_eq!(future.get(), "hello");
}

#[test]
fn duplicate_handoffs_and_duplicate_sets_are_rejected() {
    let mut promise = FiberPromise::<i32>::default();
    let _future = promise.get_future();
    assert!(std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let _ = promise.get_future();
    }))
    .is_err());

    promise.set_value(&7);
    assert!(std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        promise.set_value(&9);
    }))
    .is_err());
}

#[test]
fn invalid_future_reports_invalid_without_waiting() {
    let mut future = FiberFuture::<i32>::default();
    assert!(!future.valid());
    assert!(!future.is_ready());
    assert!(!future.wait_for(1));
}

#[test]
fn factories_preserve_pair_and_ready_value_behavior() {
    let mut pair = make_promise::<Vec<i32>>();
    pair.0.set_value(&vec![1, 2, 3]);
    assert_eq!(pair.1.get(), vec![1, 2, 3]);

    let mut future = make_ready_future::<i32>(42);
    assert!(future.valid());
    assert!(future.is_ready());
    assert_eq!(future.get(), 42);
}

#[test]
fn promise_delivery_resumes_a_suspended_fiber() {
    let mut promise = FiberPromise::<String>::default();
    let mut future = promise.get_future();
    let answer = std::rc::Rc::new(std::cell::RefCell::new(None));
    let delivered = answer.clone();
    srpc::reactor::Fiber::create_run(move || {
        *delivered.borrow_mut() = Some(future.get());
    });
    assert!(answer.borrow().is_none());
    promise.set_value(&"delivered after suspension".to_owned());
    srpc::reactor::Reactor::get_reactor().run_loop(false, true);
    assert_eq!(answer.borrow().as_deref(), Some("delivered after suspension"));
}

#[test]
fn future_timeout_resumes_its_waiting_fiber_without_a_value() {
    let mut promise = FiberPromise::<i32>::default();
    let mut future = promise.get_future();
    let result = std::rc::Rc::new(std::cell::Cell::new(None));
    let completed = result.clone();
    let started = std::time::Instant::now();
    srpc::reactor::Fiber::create_run(move || completed.set(Some(future.wait_for(5_000))));
    assert_eq!(result.get(), None);
    let reactor = srpc::reactor::Reactor::get_reactor();
    while result.get().is_none() && started.elapsed() < std::time::Duration::from_secs(2) {
        reactor.run_loop(false, true);
        std::thread::yield_now();
    }
    assert_eq!(result.get(), Some(false));
    assert!(started.elapsed() >= std::time::Duration::from_millis(5));
    assert!(!promise.is_ready());
}
