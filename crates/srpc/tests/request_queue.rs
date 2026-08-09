mod base {
    pub mod monotonic {
        pub use srpc::base::monotonic::monotonic_time_us;
    }
}

#[allow(dead_code)]
#[path = "../src/rpc/request_queue.rs"]
mod request_queue;

use request_queue::{
    kRequestQueueExpiredError, kRequestQueueRejectedError, overflow_strategy_to_string,
    queued_request_time_us, OverflowStrategy, QueuedRequest, QueuedRequestCallback, RequestQueue,
    RequestQueueConfig,
};
use std::cell::RefCell;
use std::mem::{align_of, offset_of, size_of};
use std::rc::Rc;

fn request(xid: i64) -> QueuedRequest {
    let mut request = QueuedRequest::new();
    request.xid = xid;
    request
}

fn request_with_callback(xid: i64, callback: QueuedRequestCallback) -> QueuedRequest {
    let mut request = request(xid);
    request.callback = callback;
    request
}

#[test]
fn constants_enum_repr_layout_and_presets_match_history() {
    assert_eq!(kRequestQueueRejectedError, 11);
    assert_eq!(kRequestQueueExpiredError, 110);

    assert_eq!(OverflowStrategy::DROP_OLDEST as i32, 0);
    assert_eq!(OverflowStrategy::DROP_NEWEST as i32, 1);
    assert_eq!(OverflowStrategy::FAIL_FAST as i32, 2);
    assert_eq!(size_of::<OverflowStrategy>(), 4);
    assert_eq!(align_of::<OverflowStrategy>(), 4);
    assert_eq!(
        overflow_strategy_to_string(OverflowStrategy::DROP_OLDEST),
        "DROP_OLDEST"
    );
    assert_eq!(
        overflow_strategy_to_string(OverflowStrategy::DROP_NEWEST),
        "DROP_NEWEST"
    );
    assert_eq!(
        overflow_strategy_to_string(OverflowStrategy::FAIL_FAST),
        "FAIL_FAST"
    );

    let defaults = RequestQueueConfig::defaults();
    assert_eq!(defaults, RequestQueueConfig::new());
    assert_eq!(defaults.max_size, 1_000);
    assert_eq!(defaults.default_ttl_ms, 30_000);
    assert_eq!(defaults.overflow_strategy, OverflowStrategy::DROP_OLDEST);
    assert!(defaults.enabled);

    let small = RequestQueueConfig::small();
    assert_eq!(small.max_size, 10);
    assert_eq!(small.default_ttl_ms, 5_000);
    assert_eq!(small.overflow_strategy, OverflowStrategy::DROP_OLDEST);
    assert!(small.enabled);

    let large = RequestQueueConfig::large();
    assert_eq!(large.max_size, 10_000);
    assert_eq!(large.default_ttl_ms, 60_000);
    assert_eq!(large.overflow_strategy, OverflowStrategy::DROP_OLDEST);
    assert!(large.enabled);

    let disabled = RequestQueueConfig::disabled();
    assert_eq!(disabled.max_size, 0);
    assert_eq!(disabled.default_ttl_ms, 30_000);
    assert_eq!(disabled.overflow_strategy, OverflowStrategy::DROP_OLDEST);
    assert!(!disabled.enabled);

    assert_eq!(offset_of!(RequestQueueConfig, max_size), 0);
    assert_eq!(offset_of!(RequestQueueConfig, default_ttl_ms), 8);
    assert_eq!(offset_of!(RequestQueueConfig, overflow_strategy), 12);
    assert_eq!(offset_of!(RequestQueueConfig, enabled), 16);
    assert_eq!(size_of::<RequestQueueConfig>(), 24);
    assert_eq!(align_of::<RequestQueueConfig>(), 8);
}

#[test]
fn request_factory_field_order_age_and_unsigned_ttl_wrap_are_exact() {
    let before = queued_request_time_us();
    let mut request = QueuedRequest::new();
    let after = queued_request_time_us();

    assert_eq!(request.xid, 0);
    assert_eq!(request.rpc_id, 0);
    assert!(request.timestamp_us >= before && request.timestamp_us <= after);
    assert_eq!(request.retry_count, 0);
    assert!(request.callback.is_none());
    assert_eq!(request.ttl_ms, 30_000);
    assert!(!request.is_expired());

    assert_eq!(offset_of!(QueuedRequest, xid), 0);
    assert!(offset_of!(QueuedRequest, xid) < offset_of!(QueuedRequest, rpc_id));
    assert!(offset_of!(QueuedRequest, rpc_id) < offset_of!(QueuedRequest, timestamp_us));
    assert!(offset_of!(QueuedRequest, timestamp_us) < offset_of!(QueuedRequest, retry_count));
    assert!(offset_of!(QueuedRequest, retry_count) < offset_of!(QueuedRequest, callback));
    assert!(offset_of!(QueuedRequest, callback) < offset_of!(QueuedRequest, ttl_ms));

    request.timestamp_us = queued_request_time_us().wrapping_add(1_000_000);
    request.ttl_ms = u32::MAX;
    assert!(
        request.is_expired(),
        "unsigned subtraction must wrap when a timestamp is in the future"
    );
    assert_ne!(request.age_ms(), 0);
}

#[test]
fn constructors_fifo_default_ttl_and_config_update_are_observable() {
    let mut queue = RequestQueue::new();
    assert!(queue.empty());
    assert_eq!(queue.size(), 0);
    assert!(!queue.full());
    assert_eq!(queue.remaining_capacity(), 1_000);

    let mut first = request(10);
    first.ttl_ms = 0;
    assert!(queue.enqueue(first));
    assert!(queue.enqueue(request(11)));
    assert_eq!(queue.size(), 2);

    let first = queue.dequeue().unwrap();
    assert_eq!(first.xid, 10);
    assert_eq!(first.ttl_ms, 30_000);
    assert_eq!(queue.dequeue().unwrap().xid, 11);
    assert!(queue.dequeue().is_none());

    let mut replacement = RequestQueueConfig::small();
    replacement.enabled = false;
    queue.update_config(replacement);
    assert_eq!(queue.config(), replacement);
    assert_eq!(queue.max_size(), 10);
    assert!(!queue.enabled());

    let configured = RequestQueue::with_config(RequestQueueConfig::large());
    assert_eq!(configured.max_size(), 10_000);
    assert_eq!(configured.remaining_capacity(), 10_000);
}

#[test]
fn every_overflow_policy_preserves_fifo_rejection_and_zero_capacity_edges() {
    let mut drop_oldest = RequestQueueConfig::defaults();
    drop_oldest.max_size = 2;
    drop_oldest.overflow_strategy = OverflowStrategy::DROP_OLDEST;
    let mut queue = RequestQueue::with_config(drop_oldest);
    let observed = Rc::new(RefCell::new(Vec::<(i64, i32)>::new()));

    let sink = Rc::clone(&observed);
    assert!(queue.enqueue(request_with_callback(
        1,
        Some(Box::new(move |error| sink.borrow_mut().push((1, error))))
    )));
    let sink = Rc::clone(&observed);
    assert!(queue.enqueue(request_with_callback(
        2,
        Some(Box::new(move |error| sink.borrow_mut().push((2, error))))
    )));
    assert!(queue.enqueue(request(3)));
    assert_eq!(&*observed.borrow(), &[(1, kRequestQueueRejectedError)]);
    assert_eq!(queue.dequeue().unwrap().xid, 2);
    assert_eq!(queue.dequeue().unwrap().xid, 3);

    for strategy in [OverflowStrategy::DROP_NEWEST, OverflowStrategy::FAIL_FAST] {
        let mut config = RequestQueueConfig::defaults();
        config.max_size = 1;
        config.overflow_strategy = strategy;
        let queue = RequestQueue::with_config(config);
        assert!(queue.enqueue(request(7)));
        let error = Rc::new(RefCell::new(Vec::<i32>::new()));
        let sink = Rc::clone(&error);
        assert!(!queue.enqueue(request_with_callback(
            8,
            Some(Box::new(move |value| sink.borrow_mut().push(value)))
        )));
        assert_eq!(&*error.borrow(), &[kRequestQueueRejectedError]);
        assert_eq!(queue.size(), 1);
    }

    let mut zero_config = RequestQueueConfig::defaults();
    zero_config.max_size = 0;
    zero_config.overflow_strategy = OverflowStrategy::DROP_OLDEST;
    let mut zero = RequestQueue::with_config(zero_config);
    assert!(zero.enqueue(request(20)));
    assert_eq!(zero.size(), 1);
    assert!(zero.full());
    assert_eq!(zero.remaining_capacity(), 0);
    assert!(zero.enqueue(request(21)));
    assert_eq!(zero.size(), 1);
    assert_eq!(zero.dequeue().unwrap().xid, 21);
}

#[test]
fn overflow_callbacks_preserve_legacy_locking_while_clear_and_expire_release_it() {
    let mut config = RequestQueueConfig::defaults();
    config.max_size = 1;
    config.overflow_strategy = OverflowStrategy::DROP_OLDEST;
    let queue = Rc::new(RequestQueue::with_config(config));
    let calls = Rc::new(RefCell::new(Vec::<&'static str>::new()));

    let weak_queue = Rc::downgrade(&queue);
    let calls_from_panicking = Rc::clone(&calls);
    assert!(queue.enqueue(request_with_callback(
        1,
        Some(Box::new(move |_| {
            assert!(weak_queue.upgrade().unwrap().queue_.try_lock().is_err());
            calls_from_panicking.borrow_mut().push("drop-oldest-panic");
            panic!("expected callback panic");
        }))
    )));
    assert!(queue.enqueue(request(2)));
    assert_eq!(&*calls.borrow(), &["drop-oldest-panic"]);

    let weak_queue = Rc::downgrade(&queue);
    let calls_after_panic = Rc::clone(&calls);
    queue.clear_all(-77);
    assert!(queue.enqueue(request_with_callback(
        3,
        Some(Box::new(move |error| {
            assert_eq!(error, -77);
            assert!(weak_queue.upgrade().unwrap().queue_.try_lock().is_ok());
            calls_after_panic.borrow_mut().push("clear");
        }))
    )));
    queue.clear_all(-77);
    assert_eq!(&*calls.borrow(), &["drop-oldest-panic", "clear"]);
    assert!(queue.empty());

    let weak_queue = Rc::downgrade(&queue);
    let expired = Rc::new(RefCell::new(Vec::<i32>::new()));
    let expired_sink = Rc::clone(&expired);
    let mut stale = request_with_callback(
        4,
        Some(Box::new(move |error| {
            assert!(weak_queue.upgrade().unwrap().queue_.try_lock().is_ok());
            expired_sink.borrow_mut().push(error);
        })),
    );
    stale.timestamp_us = queued_request_time_us().wrapping_sub(2_000);
    stale.ttl_ms = 1;
    assert!(queue.enqueue(stale));
    assert_eq!(queue.expire_stale(), 1);
    assert_eq!(&*expired.borrow(), &[kRequestQueueExpiredError]);
}

#[test]
fn expiration_is_stable_ordered_strict_and_continues_after_panics() {
    let queue = RequestQueue::new();
    let calls = Rc::new(RefCell::new(Vec::<i64>::new()));
    let now = queued_request_time_us();

    for xid in 0_i64..4_i64 {
        let sink = Rc::clone(&calls);
        let mut queued = request_with_callback(
            xid,
            Some(Box::new(move |error| {
                assert_eq!(error, kRequestQueueExpiredError);
                sink.borrow_mut().push(xid);
                if xid == 0 {
                    panic!("expected expiration callback panic");
                }
            })),
        );
        queued.timestamp_us = if xid == 2 {
            now
        } else {
            now.wrapping_sub(2_000)
        };
        queued.ttl_ms = if xid == 2 { u32::MAX } else { 1 };
        assert!(queue.enqueue(queued));
    }

    assert_eq!(queue.expire_stale(), 3);
    assert_eq!(&*calls.borrow(), &[0, 1, 3]);
    assert_eq!(queue.size(), 1);
    let mut queue = queue;
    assert_eq!(queue.dequeue().unwrap().xid, 2);
}

#[test]
fn disabled_rejection_and_clear_preserve_callback_order() {
    let disabled = RequestQueue::with_config(RequestQueueConfig::disabled());
    let errors = Rc::new(RefCell::new(Vec::<i32>::new()));
    let sink = Rc::clone(&errors);
    assert!(!disabled.enqueue(request_with_callback(
        1,
        Some(Box::new(move |error| sink.borrow_mut().push(error)))
    )));
    assert_eq!(&*errors.borrow(), &[kRequestQueueRejectedError]);
    assert!(disabled.empty());

    let queue = RequestQueue::new();
    let order = Rc::new(RefCell::new(Vec::<i64>::new()));
    for xid in 10_i64..13_i64 {
        let sink = Rc::clone(&order);
        assert!(queue.enqueue(request_with_callback(
            xid,
            Some(Box::new(move |error| {
                assert_eq!(error, -99);
                sink.borrow_mut().push(xid);
            }))
        )));
    }
    queue.clear_all(-99);
    assert_eq!(&*order.borrow(), &[10, 11, 12]);
    assert!(queue.empty());
}

#[test]
fn owner_retains_the_cxx_only_invalid_enum_fallthrough_and_exact_receivers() {
    let source = include_str!("../src/rpc/request_queue.rs");
    assert!(source.contains("_ => {}"));
    assert!(source.contains("pub fn enqueue(&self"));
    assert!(source.contains("pub fn dequeue(&mut self"));
    assert!(source.contains("pub fn expire_stale(&self"));
    assert!(source.contains("pub fn full(&self"));
    assert!(source.contains("pub fn remaining_capacity(&self"));
    assert!(source.contains("pub fn clear_all(&self"));
    assert!(source.contains("pub fn update_config(&self"));
    assert!(source.contains("#[cfg_attr(any(), cpp_ctor)]"));
    assert!(source.contains("Option<Box<dyn FnMut(i32)>>"));
    assert!(source.contains("wrapping_sub"));
    assert!(source.contains("elapsed_us / 1_000_u64 > self.ttl_ms as u64"));
}
