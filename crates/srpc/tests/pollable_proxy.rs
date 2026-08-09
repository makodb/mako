use srpc::rpc::pollable_proxy::{
    make_pollable_proxy_from_typed_arc, PollableArcShim, PollableProxy, PollableSharedTarget,
};
use std::mem::{align_of, size_of};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Weak};

struct RecordingPollable {
    fd: i32,
    mode: i32,
    content_size_calls: AtomicUsize,
    read_calls: AtomicUsize,
    write_calls: AtomicUsize,
    error_calls: AtomicUsize,
    close_calls: AtomicUsize,
    pending: AtomicBool,
    closed: AtomicBool,
}

impl RecordingPollable {
    fn new(fd: i32, mode: i32) -> RecordingPollable {
        RecordingPollable {
            fd,
            mode,
            content_size_calls: AtomicUsize::new(0),
            read_calls: AtomicUsize::new(0),
            write_calls: AtomicUsize::new(0),
            error_calls: AtomicUsize::new(0),
            close_calls: AtomicUsize::new(0),
            pending: AtomicBool::new(false),
            closed: AtomicBool::new(false),
        }
    }
}

impl PollableSharedTarget for RecordingPollable {
    fn fd(&self) -> i32 {
        self.fd
    }

    fn poll_mode(&self) -> i32 {
        self.mode
    }

    fn content_size(&self) -> usize {
        self.content_size_calls.fetch_add(1, Ordering::Relaxed);
        64
    }

    fn handle_read(&self) -> bool {
        self.read_calls.fetch_add(1, Ordering::Relaxed);
        true
    }

    fn handle_write(&self) -> i32 {
        self.write_calls.fetch_add(1, Ordering::Relaxed);
        -1
    }

    fn handle_error(&self) {
        self.error_calls.fetch_add(1, Ordering::Relaxed);
    }

    fn close(&self) {
        self.close_calls.fetch_add(1, Ordering::Relaxed);
        self.closed.store(true, Ordering::Release);
    }

    fn check_pending_write_update(&self) -> bool {
        self.pending.load(Ordering::Acquire)
    }

    fn is_closed(&self) -> bool {
        self.closed.load(Ordering::Acquire)
    }
}

fn assert_send_sync<T: Send + Sync>() {}

#[test]
fn proxy_forwards_the_complete_legacy_surface() {
    let target = Arc::new(RecordingPollable::new(42, 3));
    let mut proxy = make_pollable_proxy_from_typed_arc(Arc::clone(&target));

    assert_eq!(proxy.fd(), 42);
    assert_eq!(proxy.poll_mode(), 3);
    assert_eq!(proxy.content_size(), 64);
    assert!(proxy.handle_read());
    assert_eq!(proxy.handle_write(), -1);
    proxy.handle_error();

    assert_eq!(target.content_size_calls.load(Ordering::Relaxed), 1);
    assert_eq!(target.read_calls.load(Ordering::Relaxed), 1);
    assert_eq!(target.write_calls.load(Ordering::Relaxed), 1);
    assert_eq!(target.error_calls.load(Ordering::Relaxed), 1);

    assert!(!proxy.check_pending_write_update());
    target.pending.store(true, Ordering::Release);
    assert!(proxy.check_pending_write_update());
    assert!(!proxy.is_closed());
    proxy.close();
    assert!(proxy.is_closed());
    assert_eq!(target.close_calls.load(Ordering::Relaxed), 1);
}

#[test]
fn proxy_owns_the_target_after_the_callers_arc_is_released() {
    let target = Arc::new(RecordingPollable::new(77, 1));
    let weak: Weak<RecordingPollable> = Arc::downgrade(&target);
    let proxy = make_pollable_proxy_from_typed_arc(Arc::clone(&target));
    assert_eq!(Arc::strong_count(&target), 2);

    drop(target);
    assert_eq!(proxy.fd(), 77);
    assert!(weak.upgrade().is_some());

    drop(proxy);
    assert!(weak.upgrade().is_none());
}

#[test]
fn shim_is_exactly_one_arc_and_preserves_thread_traits() {
    assert_eq!(
        size_of::<PollableArcShim<RecordingPollable>>(),
        size_of::<Arc<RecordingPollable>>()
    );
    assert_eq!(
        align_of::<PollableArcShim<RecordingPollable>>(),
        align_of::<Arc<RecordingPollable>>()
    );
    assert_send_sync::<PollableArcShim<RecordingPollable>>();
    assert_send_sync::<PollableProxy>();
}
