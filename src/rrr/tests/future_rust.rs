use rrr::future::{make_promise, make_ready_future, FiberFuture, FiberPromise};

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
    pair.first.set_value(&vec![1, 2, 3]);
    assert_eq!(pair.second.get(), vec![1, 2, 3]);

    let mut future = make_ready_future::<i32>(42);
    assert!(future.valid());
    assert!(future.is_ready());
    assert_eq!(future.get(), 42);
}
