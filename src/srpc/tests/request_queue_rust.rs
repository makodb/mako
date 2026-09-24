use std::cell::Cell;
use std::mem::{align_of, offset_of, size_of};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::rc::Rc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;

use srpc::request_queue::{
    kRequestQueueExpiredError, kRequestQueueRejectedError, overflow_strategy_to_string,
    queued_request_time_us, OverflowStrategy, QueuedRequest, QueuedRequestCallback, RequestQueue,
    RequestQueueConfig,
};

static NOW_US: AtomicU64 = AtomicU64::new(0);
static CLOCK_TEST_LOCK: Mutex<()> = Mutex::new(());

#[allow(unsafe_code)]
#[unsafe(no_mangle)]
pub extern "C" fn srpc_clock_monotonic_us() -> u64 {
    NOW_US.load(Ordering::SeqCst)
}

fn set_now(value: u64) {
    NOW_US.store(value, Ordering::SeqCst);
}

fn request_at(timestamp_us: u64) -> QueuedRequest {
    let mut request = QueuedRequest::new();
    request.timestamp_us = timestamp_us;
    request
}

fn callback<F>(callback: F) -> QueuedRequestCallback
where
    F: FnMut(i32) + 'static,
{
    QueuedRequestCallback::from_callable(callback)
}

#[test]
fn public_layout_discriminants_and_traits_match_the_cpp_surface() {
    assert_eq!(size_of::<OverflowStrategy>(), 4);
    assert_eq!(align_of::<OverflowStrategy>(), 4);
    assert_eq!(OverflowStrategy::DROP_OLDEST as i32, 0);
    assert_eq!(OverflowStrategy::DROP_NEWEST as i32, 1);
    assert_eq!(OverflowStrategy::FAIL_FAST as i32, 2);

    assert_eq!(size_of::<QueuedRequestCallback>(), 48);
    assert_eq!(align_of::<QueuedRequestCallback>(), 16);

    assert_eq!(size_of::<QueuedRequest>(), 96);
    assert_eq!(align_of::<QueuedRequest>(), 16);
    assert_eq!(offset_of!(QueuedRequest, xid), 0);
    assert_eq!(offset_of!(QueuedRequest, rpc_id), 8);
    assert_eq!(offset_of!(QueuedRequest, timestamp_us), 16);
    assert_eq!(offset_of!(QueuedRequest, retry_count), 24);
    assert_eq!(offset_of!(QueuedRequest, callback), 32);
    assert_eq!(offset_of!(QueuedRequest, ttl_ms), 80);

    assert_eq!(size_of::<RequestQueueConfig>(), 24);
    assert_eq!(align_of::<RequestQueueConfig>(), 8);
    assert_eq!(offset_of!(RequestQueueConfig, max_size), 0);
    assert_eq!(offset_of!(RequestQueueConfig, default_ttl_ms), 8);
    assert_eq!(offset_of!(RequestQueueConfig, overflow_strategy), 12);
    assert_eq!(offset_of!(RequestQueueConfig, enabled), 16);

    fn assert_copy<T: Copy>() {}
    fn assert_send_sync<T: Send + Sync>() {}
    assert_copy::<OverflowStrategy>();
    assert_copy::<RequestQueueConfig>();
    assert_send_sync::<RequestQueueConfig>();

    macro_rules! assert_not_auto_trait {
        ($type:ty, $auto_trait:ident) => {{
            trait AmbiguousIfImplemented<Marker> {
                fn marker() {}
            }
            impl<T: ?Sized> AmbiguousIfImplemented<()> for T {}
            impl<T: ?Sized + $auto_trait> AmbiguousIfImplemented<u8> for T {}
            let _ = <$type as AmbiguousIfImplemented<_>>::marker;
        }};
    }
    assert_not_auto_trait!(QueuedRequestCallback, Send);
    assert_not_auto_trait!(QueuedRequestCallback, Sync);
    assert_not_auto_trait!(QueuedRequest, Send);
    assert_not_auto_trait!(QueuedRequest, Sync);
    assert_not_auto_trait!(RequestQueue, Send);
    assert_not_auto_trait!(RequestQueue, Sync);
}

#[test]
fn constants_factories_and_names_preserve_legacy_values() {
    #[cfg(target_os = "macos")]
    let expected_errors = (35, 60);
    #[cfg(not(target_os = "macos"))]
    let expected_errors = (11, 110);
    assert_eq!(kRequestQueueRejectedError, expected_errors.0);
    assert_eq!(kRequestQueueExpiredError, expected_errors.1);
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
    let large = RequestQueueConfig::large();
    assert_eq!(large.max_size, 10_000);
    assert_eq!(large.default_ttl_ms, 60_000);
    let disabled = RequestQueueConfig::disabled();
    assert_eq!(disabled.max_size, 0);
    assert!(!disabled.enabled);
}

#[test]
fn request_time_expiry_age_and_unsigned_wrap_are_exact() {
    let _clock_guard = CLOCK_TEST_LOCK.lock().unwrap();
    set_now(1_000_000);
    assert_eq!(queued_request_time_us(), 1_000_000);

    let mut request = QueuedRequest::new();
    assert_eq!(request.xid, 0);
    assert_eq!(request.rpc_id, 0);
    assert_eq!(request.timestamp_us, 1_000_000);
    assert_eq!(request.retry_count, 0);
    assert!(request.callback.is_empty());
    assert_eq!(request.ttl_ms, 30_000);

    request.ttl_ms = 10;
    set_now(1_010_000);
    assert!(!request.is_expired());
    assert_eq!(request.age_ms(), 10);
    set_now(1_011_000);
    assert!(request.is_expired());
    assert_eq!(request.age_ms(), 11);

    request.timestamp_us = u64::MAX - 499;
    request.ttl_ms = 0;
    set_now(500);
    assert_eq!(request.age_ms(), 1);
    assert!(request.is_expired());
}

#[test]
fn fifo_capacity_default_ttl_and_config_updates_are_preserved() {
    let _clock_guard = CLOCK_TEST_LOCK.lock().unwrap();
    set_now(50);
    let mut queue = RequestQueue::with_config(RequestQueueConfig {
        max_size: 2,
        default_ttl_ms: 77,
        overflow_strategy: OverflowStrategy::DROP_OLDEST,
        enabled: true,
    });
    assert!(queue.empty());
    assert_eq!(queue.remaining_capacity(), 2);

    let mut first = request_at(10);
    first.xid = 1;
    first.ttl_ms = 0;
    let mut second = request_at(20);
    second.xid = 2;
    assert!(queue.enqueue(first));
    assert!(queue.enqueue(second));
    assert!(queue.full());
    assert_eq!(queue.remaining_capacity(), 0);

    let first = queue.dequeue().unwrap();
    assert_eq!(first.xid, 1);
    assert_eq!(first.ttl_ms, 77);
    assert_eq!(queue.dequeue().unwrap().xid, 2);
    assert!(queue.dequeue().is_none());

    queue.update_config(RequestQueueConfig::small());
    assert_eq!(queue.config(), RequestQueueConfig::small());
    assert!(queue.enabled());
    assert_eq!(queue.max_size(), 10);
}

#[test]
fn overflow_disabled_expiry_and_clear_callbacks_are_isolated() {
    let _clock_guard = CLOCK_TEST_LOCK.lock().unwrap();
    set_now(1_000_000);

    for strategy in [OverflowStrategy::DROP_NEWEST, OverflowStrategy::FAIL_FAST] {
        let observed = Rc::new(Cell::new(0));
        let sink = Rc::clone(&observed);
        let queue = RequestQueue::with_config(RequestQueueConfig {
            max_size: 1,
            default_ttl_ms: 30_000,
            overflow_strategy: strategy,
            enabled: true,
        });
        assert!(queue.enqueue(request_at(1_000_000)));
        let mut rejected = request_at(1_000_000);
        rejected.callback = callback(move |error| sink.set(error));
        assert!(!queue.enqueue(rejected));
        assert_eq!(observed.get(), kRequestQueueRejectedError);
        assert_eq!(queue.size(), 1);
    }

    let dropped = Rc::new(Cell::new(0));
    let dropped_sink = Rc::clone(&dropped);
    let queue = RequestQueue::with_config(RequestQueueConfig {
        max_size: 1,
        default_ttl_ms: 30_000,
        overflow_strategy: OverflowStrategy::DROP_OLDEST,
        enabled: true,
    });
    let mut oldest = request_at(1_000_000);
    oldest.callback = callback(move |error| dropped_sink.set(error));
    assert!(queue.enqueue(oldest));
    assert!(queue.enqueue(request_at(1_000_000)));
    assert_eq!(dropped.get(), kRequestQueueRejectedError);

    let mut panicking_oldest = RequestQueue::with_config(RequestQueueConfig {
        max_size: 1,
        default_ttl_ms: 30_000,
        overflow_strategy: OverflowStrategy::DROP_OLDEST,
        enabled: true,
    });
    let mut oldest = request_at(1_000_000);
    oldest.callback = callback(|_| panic!("held-lock callback panic must be isolated"));
    assert!(panicking_oldest.enqueue(oldest));
    assert!(catch_unwind(AssertUnwindSafe(|| {
        panicking_oldest.enqueue(request_at(1_000_000))
    }))
    .is_ok());
    assert_eq!(panicking_oldest.size(), 1);
    assert!(panicking_oldest.dequeue().is_some());

    let disabled_observed = Rc::new(Cell::new(0));
    let disabled_sink = Rc::clone(&disabled_observed);
    let disabled = RequestQueue::with_config(RequestQueueConfig::disabled());
    let mut rejected = request_at(1_000_000);
    rejected.callback = callback(move |error| disabled_sink.set(error));
    assert!(!disabled.enqueue(rejected));
    assert_eq!(disabled_observed.get(), kRequestQueueRejectedError);

    let expired_count = Rc::new(Cell::new(0));
    let expired_sink = Rc::clone(&expired_count);
    let queue = RequestQueue::new();
    let mut first_expired = request_at(900_000);
    first_expired.ttl_ms = 10;
    first_expired.callback = callback(|_| panic!("first expiry callback panic"));
    let mut second_expired = request_at(900_000);
    second_expired.ttl_ms = 10;
    second_expired.callback = callback(move |error| {
        assert_eq!(error, kRequestQueueExpiredError);
        expired_sink.set(expired_sink.get() + 1);
    });
    let mut live = request_at(999_000);
    live.ttl_ms = 10;
    assert!(queue.enqueue(first_expired));
    assert!(queue.enqueue(second_expired));
    assert!(queue.enqueue(live));
    assert_eq!(queue.expire_stale(), 2);
    assert_eq!(expired_count.get(), 1);
    assert_eq!(queue.size(), 1);

    let mut panicking_clear = request_at(1_000_000);
    panicking_clear.callback = callback(|_| panic!("first clear callback panic"));
    assert!(queue.enqueue(panicking_clear));

    let cleared_count = Rc::new(Cell::new(0));
    let clear_sink = Rc::clone(&cleared_count);
    let mut clear_request = request_at(1_000_000);
    clear_request.callback = callback(move |error| {
        assert_eq!(error, -7);
        clear_sink.set(clear_sink.get() + 1);
    });
    assert!(queue.enqueue(clear_request));
    assert!(catch_unwind(AssertUnwindSafe(|| queue.clear_all(-7))).is_ok());
    assert_eq!(cleared_count.get(), 1);
    assert!(queue.empty());

    let panicking = RequestQueue::with_config(RequestQueueConfig::disabled());
    let mut request = request_at(1_000_000);
    request.callback = callback(|_| panic!("callback panic must be isolated"));
    assert!(catch_unwind(AssertUnwindSafe(|| !panicking.enqueue(request))).is_ok());
}

#[test]
fn callbacks_observe_the_legacy_queue_lock_boundaries() {
    let _clock_guard = CLOCK_TEST_LOCK.lock().unwrap();
    set_now(1_000_000);

    let held_observed = Rc::new(Cell::new(false));
    let queue = Rc::new(RequestQueue::with_config(RequestQueueConfig {
        max_size: 1,
        default_ttl_ms: 30_000,
        overflow_strategy: OverflowStrategy::DROP_OLDEST,
        enabled: true,
    }));
    let queue_weak = Rc::downgrade(&queue);
    let held_sink = Rc::clone(&held_observed);
    let mut oldest = request_at(1_000_000);
    oldest.callback = callback(move |_| {
        let queue = queue_weak.upgrade().unwrap();
        held_sink.set(queue.queue_.try_lock().is_err());
    });
    assert!(queue.enqueue(oldest));
    assert!(queue.enqueue(request_at(1_000_000)));
    assert!(held_observed.get());

    let disabled_unlocked = Rc::new(Cell::new(false));
    let disabled = Rc::new(RequestQueue::with_config(RequestQueueConfig::disabled()));
    let disabled_weak = Rc::downgrade(&disabled);
    let disabled_sink = Rc::clone(&disabled_unlocked);
    let mut rejected = request_at(1_000_000);
    rejected.callback = callback(move |_| {
        let queue = disabled_weak.upgrade().unwrap();
        disabled_sink.set(queue.queue_.try_lock().is_ok());
    });
    assert!(!disabled.enqueue(rejected));
    assert!(disabled_unlocked.get());

    let expired_unlocked = Rc::new(Cell::new(false));
    let expiring = Rc::new(RequestQueue::new());
    let expiring_weak = Rc::downgrade(&expiring);
    let expired_sink = Rc::clone(&expired_unlocked);
    let mut expired = request_at(900_000);
    expired.ttl_ms = 10;
    expired.callback = callback(move |_| {
        let queue = expiring_weak.upgrade().unwrap();
        expired_sink.set(queue.queue_.try_lock().is_ok());
    });
    assert!(expiring.enqueue(expired));
    assert_eq!(expiring.expire_stale(), 1);
    assert!(expired_unlocked.get());

    let clear_unlocked = Rc::new(Cell::new(false));
    let clearing = Rc::new(RequestQueue::new());
    let clearing_weak = Rc::downgrade(&clearing);
    let clear_sink = Rc::clone(&clear_unlocked);
    let mut pending = request_at(1_000_000);
    pending.callback = callback(move |_| {
        let queue = clearing_weak.upgrade().unwrap();
        clear_sink.set(queue.queue_.try_lock().is_ok());
    });
    assert!(clearing.enqueue(pending));
    clearing.clear_all(-7);
    assert!(clear_unlocked.get());
}
