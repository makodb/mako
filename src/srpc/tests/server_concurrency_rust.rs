#![allow(unsafe_code)]

use srpc::channel::{ChannelConnectionBase, ChannelError, ChannelFrame, OnClosedCallback,
    OnErrorCallback, OnFrameCallback};
use srpc::server::{sconn_dispatch_response_frame_via_channel, RpcServiceContext, ServerConnection};
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicUsize, Ordering};
use std::sync::{Arc, Barrier};

struct TransportState {
    entered: Barrier,
    release: Barrier,
    dropped: AtomicBool,
    close_count: AtomicUsize,
}

struct BlockingTransport(Arc<TransportState>);

impl ChannelConnectionBase for BlockingTransport {
    unsafe fn send_frame(&self, _: &ChannelFrame) -> ChannelError {
        self.0.entered.wait();
        self.0.release.wait();
        ChannelError::None
    }
    fn flush(&self) {}
    fn close(&self) { self.0.close_count.fetch_add(1, Ordering::SeqCst); }
    fn is_closed(&self) -> bool { self.0.close_count.load(Ordering::SeqCst) != 0 }
    fn peer_address(&self) -> String { "concurrency-test".into() }
    fn set_on_frame(&mut self, _: OnFrameCallback) {}
    fn set_on_closed(&mut self, _: OnClosedCallback) {}
    fn set_on_error(&mut self, _: OnErrorCallback) {}
}

impl Drop for BlockingTransport {
    fn drop(&mut self) { self.0.dropped.store(true, Ordering::SeqCst); }
}

fn connection() -> (Arc<ServerConnection>, Arc<TransportState>) {
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<ServerConnection>();
    let context = Arc::new(RpcServiceContext::new(Default::default(), Default::default(),
        Vec::new(), String::new(), Arc::new(AtomicI32::new(0)), Arc::new(AtomicBool::new(false)), 1));
    let state = Arc::new(TransportState { entered: Barrier::new(2), release: Barrier::new(2),
        dropped: AtomicBool::new(false), close_count: AtomicUsize::new(0) });
    let mut connection = ServerConnection::new(context, -1);
    connection.bind_channel(Some(Box::new(BlockingTransport(state.clone()))));
    connection.bind_channel(None);
    assert!(!state.dropped.load(Ordering::SeqCst), "an absent binding must keep the installed transport");
    (Arc::new(connection), state)
}

#[test]
fn an_absent_request_counter_never_installs_a_pending_guard() {
    let counter = Arc::new(AtomicI32::new(0));
    let mut request = srpc::server::Request {
        body: Vec::new(),
        src: srpc::serializable::BufferSource::new(std::ptr::null(), 0),
        xid: 0,
        pending_guard: None,
    };
    request.attach_pending_guard(&None);
    assert!(request.pending_guard.is_none());
    request.attach_pending_guard(&Some(counter.clone()));
    request.attach_pending_guard(&None);
    assert_eq!(counter.load(Ordering::Relaxed), 1);
    drop(request);
    assert_eq!(counter.load(Ordering::Relaxed), 0);
}

#[test]
fn in_flight_send_pins_channel_after_its_slot_is_removed() {
    let (connection, state) = connection();
    let sender = connection.clone();
    let worker = std::thread::spawn(move || {
        // Empty payload requires no readable storage.
        unsafe { sconn_dispatch_response_frame_via_channel(&sender, std::ptr::null(), 0) };
    });
    state.entered.wait();
    drop(connection.channel_proxy_.lock().unwrap().take());
    assert!(!state.dropped.load(Ordering::SeqCst), "send must retain the channel owner");
    state.release.wait();
    worker.join().unwrap();
    assert!(state.dropped.load(Ordering::SeqCst));
}

#[test]
fn concurrent_close_transitions_and_closes_the_transport_once() {
    let (connection, state) = connection();
    let start = Arc::new(Barrier::new(9));
    let workers: Vec<_> = (0..8).map(|_| {
        let connection = connection.clone();
        let start = start.clone();
        std::thread::spawn(move || {
            start.wait();
            connection.close();
            assert!(connection.is_closed());
            assert!(!connection.connected());
            assert!(connection.is_channel_mode());
        })
    }).collect();
    start.wait();
    for worker in workers { worker.join().unwrap(); }
    assert_eq!(state.close_count.load(Ordering::SeqCst), 1);
}

#[test]
fn optional_async_callback_reports_empty_and_calls_present_once() {
    let (connection, _state) = connection();
    assert_ne!(connection.run_async(None), 0);
    let calls = Arc::new(AtomicUsize::new(0));
    let callback_calls = calls.clone();
    assert_eq!(connection.run_async(Some(Box::new(move || {
        callback_calls.fetch_add(1, Ordering::SeqCst);
    }))), 0);
    assert_eq!(calls.load(Ordering::SeqCst), 1);
}
