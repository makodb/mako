// Regression: teardown must not destroy buffered request callbacks unfired.
//
// A request submitted while disconnected takes the QUEUE branch of
// clientconn_request_via_channel and is parked in `pending_queue_`. That path
// returns as soon as the enqueue succeeds and never registers the future in
// `pending_fu_`, so the queued callback is the ONLY holder of the future's
// notification path.
//
// close()/mark_closing()/Drop all funnel through invalidate_pending_futures(),
// which used to drain `pending_cb_slots_` and `pending_fu_` and nothing else.
// Every buffered entry was therefore dropped with the queue: a waiter got a 1s
// timeout and ETIMEDOUT rather than a connection error, and a callback-style
// caller -- which is what mako's generated proxies use -- was never called.
#![allow(unsafe_code)]

use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc as StdArc;

use srpc::request_queue::{QueuedRequest, QueuedRequestCallback, RequestQueue, RequestQueueConfig};

// The crate's clock is a C kernel (misc/srpc_timing.c) that rustc-only test
// binaries do not link; every other test here supplies the same stub.
#[allow(unsafe_code)]
#[unsafe(no_mangle)]
pub extern "C" fn srpc_clock_monotonic_us() -> u64 {
    0
}

fn queue_with_capacity(max: usize) -> RequestQueue {
    let mut cfg = RequestQueueConfig::defaults();
    cfg.max_size = max;
    cfg.enabled = true;
    RequestQueue::with_config(cfg)
}

fn park(q: &RequestQueue, xid: i64, fired: &StdArc<AtomicUsize>, codes: &StdArc<std::sync::Mutex<Vec<i32>>>) {
    let f = fired.clone();
    let c = codes.clone();
    let mut qr = QueuedRequest::new();
    qr.xid = xid;
    qr.rpc_id = 7;
    qr.callback = QueuedRequestCallback::from_callable(move |err: i32| {
        f.fetch_add(1, Ordering::SeqCst);
        c.lock().unwrap().push(err);
    });
    assert!(q.enqueue(qr), "enqueue of xid {xid} should succeed");
}

#[test]
fn clear_all_fires_every_queued_callback() {
    // The control: the drain path itself conserves callbacks. If this ever
    // fails, the fix below is built on sand.
    let q = queue_with_capacity(100);
    let fired = StdArc::new(AtomicUsize::new(0));
    let codes = StdArc::new(std::sync::Mutex::new(Vec::new()));
    for xid in 0..7i64 {
        park(&q, xid, &fired, &codes);
    }
    assert_eq!(q.size(), 7);

    q.clear_all(107);

    assert_eq!(
        fired.load(Ordering::SeqCst),
        7,
        "clear_all must invoke every queued callback"
    );
    assert_eq!(q.size(), 0, "clear_all must empty the queue");
    assert!(
        codes.lock().unwrap().iter().all(|&c| c == 107),
        "every callback must receive the error code it was drained with"
    );
}

#[test]
fn dropping_the_queue_without_draining_loses_every_callback() {
    // This is the shape of the bug, pinned so it cannot come back silently:
    // RequestQueue has no `impl Drop`, so letting one fall out of scope
    // destroys its callbacks. That is WHY teardown has to drain explicitly,
    // and why the fix belongs in invalidate_pending_futures() rather than in
    // a Drop impl on the queue (a Drop impl would also have to reach the
    // connection's `metrics_` through a raw pointer during field drop, which
    // is only sound today by declaration-order accident).
    let fired = StdArc::new(AtomicUsize::new(0));
    let codes = StdArc::new(std::sync::Mutex::new(Vec::new()));
    {
        let q = queue_with_capacity(100);
        for xid in 0..5i64 {
            park(&q, xid, &fired, &codes);
        }
        assert_eq!(q.size(), 5);
    } // dropped here, undrained

    assert_eq!(
        fired.load(Ordering::SeqCst),
        0,
        "documents that a bare drop fires nothing -- the reason teardown must drain"
    );
}
