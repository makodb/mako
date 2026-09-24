#![allow(unsafe_code)]
#![allow(clippy::arc_with_non_send_sync)]

use srpc::channel::{
    ChannelConnectionBase, ChannelConnectionProxy, ChannelError, ChannelFrame, OnClosedCallback,
    OnErrorCallback, OnFrameCallback,
};
use srpc::fiber_channel::{FiberChannel, OwnedFrame};
use rusty::CallbackWrapper;
use std::marker::PhantomPinned;
use std::mem::{align_of, offset_of, size_of};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

struct StubState {
    closed: bool,
    sent: Vec<Vec<u8>>,
    send_result: ChannelError,
    on_frame: OnFrameCallback,
    on_closed: OnClosedCallback,
    on_error: OnErrorCallback,
}

impl StubState {
    fn new() -> StubState {
        StubState {
            closed: false,
            sent: Vec::new(),
            send_result: ChannelError::None,
            on_frame: CallbackWrapper::default(),
            on_closed: CallbackWrapper::default(),
            on_error: CallbackWrapper::default(),
        }
    }
}

#[derive(Clone)]
struct StubHandle(Arc<Mutex<StubState>>);

// Production callbacks run on the reactor thread. The parked-wait test uses a
// helper OS thread only because the Cargo-only Condvar event shim cannot pump
// reactor fibers; this unsafe test handle keeps the wrapper pinned and joins
// that helper before teardown.
unsafe impl Send for StubHandle {}
unsafe impl Sync for StubHandle {}

impl StubHandle {
    fn new() -> StubHandle {
        StubHandle(Arc::new(Mutex::new(StubState::new())))
    }

    fn deliver(&self, payload: &[u8]) {
        let callback = {
            let state = self.0.lock().unwrap();
            state.on_frame.clone()
        };
        if callback.has_value() {
            let frame = ChannelFrame {
                payload: payload.as_ptr(),
                size: payload.len(),
            };
            (callback.callable())(&frame);
        }
    }

    fn deliver_closed(&self, reason: ChannelError) {
        let callback = {
            let mut state = self.0.lock().unwrap();
            state.closed = true;
            state.on_closed.clone()
        };
        if callback.has_value() {
            (callback.callable())(reason);
        }
    }

    fn mark_proxy_closed_without_callback(&self) {
        self.0.lock().unwrap().closed = true;
    }

    fn callbacks_bound(&self) -> (bool, bool, bool) {
        let state = self.0.lock().unwrap();
        (
            state.on_frame.has_value(),
            state.on_closed.has_value(),
            state.on_error.has_value(),
        )
    }
}

struct StubConnection {
    state: StubHandle,
}

impl ChannelConnectionBase for StubConnection {
    unsafe fn send_frame(&mut self, frame: &ChannelFrame) -> ChannelError {
        let mut state = self.state.0.lock().unwrap();
        let bytes = if frame.size == 0 || frame.payload.is_null() {
            Vec::new()
        } else {
            unsafe { core::slice::from_raw_parts(frame.payload, frame.size) }.to_vec()
        };
        state.sent.push(bytes);
        state.send_result
    }

    fn flush(&mut self) {}

    fn close(&mut self) {
        self.state.deliver_closed(ChannelError::None);
    }

    fn is_closed(&self) -> bool {
        self.state.0.lock().unwrap().closed
    }

    fn peer_address(&self) -> String {
        "fiber-test".to_owned()
    }

    fn set_on_frame(&mut self, callback: OnFrameCallback) {
        self.state.0.lock().unwrap().on_frame = callback;
    }

    fn set_on_closed(&mut self, callback: OnClosedCallback) {
        self.state.0.lock().unwrap().on_closed = callback;
    }

    fn set_on_error(&mut self, callback: OnErrorCallback) {
        self.state.0.lock().unwrap().on_error = callback;
    }
}

fn make_channel() -> (ChannelConnectionProxy, StubHandle) {
    let handle = StubHandle::new();
    let proxy: ChannelConnectionProxy = Box::new(StubConnection {
        state: handle.clone(),
    });
    (proxy, handle)
}

fn bind(channel: ChannelConnectionProxy) -> std::pin::Pin<Box<FiberChannel>> {
    let mut wrapper = Box::pin(FiberChannel::new(channel));
    unsafe { wrapper.as_mut().get_unchecked_mut() }.bind_callbacks();
    wrapper
}

fn wrapper_mut(wrapper: &mut std::pin::Pin<Box<FiberChannel>>) -> &mut FiberChannel {
    unsafe { wrapper.as_mut().get_unchecked_mut() }
}

#[test]
fn owned_frame_layout_and_pin_contract_are_explicit() {
    assert_eq!(offset_of!(OwnedFrame, bytes), 0);
    assert_eq!(size_of::<OwnedFrame>(), size_of::<Vec<u8>>());
    assert_eq!(align_of::<OwnedFrame>(), align_of::<Vec<u8>>());

    fn requires_unpin<T: Unpin>() {}
    requires_unpin::<OwnedFrame>();

    // Compile-time ownership marker: the wrapper contains PhantomPinned.
    // Factory-only construction: the sole C++ construction path is the
    // FiberChannel::new_ factory (no #[cpp_ctor], so no C++ ctor and no
    // cpp_explicit); the cpp_no_fieldwise_ctor marker keeps the synthesized
    // fieldwise ctor private.
    let source = include_str!("../rpc/fiber_channel.rs");
    assert!(source.contains("_pin: PhantomPinned"));
    assert!(source.contains("cpp_no_fieldwise_ctor"));
    assert!(!source.contains("cpp_ctor)"));
    assert!(!source.contains("cpp_explicit"));
    let _marker = PhantomPinned;
}

#[test]
fn callbacks_copy_frames_fifo_and_drain_before_close() {
    let (channel, handle) = make_channel();
    let mut wrapper = bind(channel);
    assert_eq!(handle.callbacks_bound(), (true, true, true));

    handle.deliver(&[0xa0]);
    handle.deliver(&[0xb0, 0xb1]);
    handle.deliver(&[0xc0, 0xc1, 0xc2]);
    handle.deliver_closed(ChannelError::ConnectionReset);

    let wrapper = wrapper_mut(&mut wrapper);
    assert_eq!(wrapper.recv_frame().unwrap().bytes, [0xa0]);
    assert_eq!(wrapper.recv_frame().unwrap().bytes, [0xb0, 0xb1]);
    assert_eq!(wrapper.recv_frame().unwrap().bytes, [0xc0, 0xc1, 0xc2]);
    assert!(wrapper.recv_frame().is_none());
    assert!(wrapper.is_closed());
}

#[test]
fn frame_copy_owns_payload_and_null_nonempty_input_is_guarded() {
    let (channel, handle) = make_channel();
    let mut wrapper = bind(channel);

    let mut payload = [1_u8, 2, 3, 4];
    handle.deliver(&payload);
    payload.fill(0xff);
    assert_eq!(
        wrapper_mut(&mut wrapper).recv_frame().unwrap().bytes,
        [1, 2, 3, 4]
    );

    let callback = {
        let state = handle.0.lock().unwrap();
        state.on_frame.clone()
    };
    (callback.callable())(&ChannelFrame {
        payload: core::ptr::null(),
        size: 8,
    });
    assert!(wrapper_mut(&mut wrapper)
        .recv_frame()
        .unwrap()
        .bytes
        .is_empty());
}

#[test]
fn send_close_proxy_state_and_test_access_match_the_facade() {
    let (channel, handle) = make_channel();
    let mut wrapper = bind(channel);
    let payload = [9_u8, 8, 7];
    let frame = ChannelFrame {
        payload: payload.as_ptr(),
        size: payload.len(),
    };
    assert_eq!(
        unsafe { wrapper_mut(&mut wrapper).send_frame(&frame) },
        ChannelError::None
    );
    assert_eq!(handle.0.lock().unwrap().sent, [payload.to_vec()]);
    assert_eq!(
        wrapper_mut(&mut wrapper).channel_for_test().peer_address(),
        "fiber-test"
    );

    handle.mark_proxy_closed_without_callback();
    assert!(wrapper.as_ref().get_ref().is_closed());

    wrapper_mut(&mut wrapper).close();
    assert!(wrapper.as_ref().get_ref().is_closed());
}

#[test]
fn parked_receive_wakes_after_cross_thread_delivery() {
    let (channel, handle) = make_channel();
    let mut wrapper = bind(channel);
    let delivery = thread::spawn(move || {
        thread::sleep(Duration::from_millis(20));
        handle.deliver(&[0x11, 0x22, 0x33]);
    });

    let frame = wrapper_mut(&mut wrapper).recv_frame().unwrap();
    delivery.join().unwrap();
    assert_eq!(frame.bytes, [0x11, 0x22, 0x33]);
}

#[test]
fn drop_detaches_all_callbacks_before_proxy_teardown() {
    let (channel, handle) = make_channel();
    {
        let wrapper = bind(channel);
        assert_eq!(handle.callbacks_bound(), (true, true, true));
        drop(wrapper);
    }
    assert_eq!(handle.callbacks_bound(), (false, false, false));
}

#[test]
fn source_retains_the_load_bearing_lock_and_foreign_event_boundaries() {
    let source = include_str!("../rpc/fiber_channel.rs");
    assert!(source.contains("let held: Option<Arc<rusty::ReactorIntEvent>> = {"));
    assert!(source.contains("cpp_reactor::IntEvent::set(&*event, 1_i32)"));
    assert!(source.contains("cpp_reactor::IntEvent::wait(&*event)"));
    assert!(source.contains("cpp_reactor::create_sp_int_event(1_i32)"));
    assert!(source.contains("let ch: &Box<LegacyChannelConnectionBase>"));
    assert!(source.contains("core::mem::take(&mut guard[0])"));
}
