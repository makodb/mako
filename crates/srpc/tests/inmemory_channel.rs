#![allow(unsafe_code)]

use srpc::base::callback_wrapper::detail::CallbackWrapper;
use srpc::rpc::channel::{
    ChannelConnectionProxy, ChannelError, ChannelFactoryProxy, ChannelFrame, ChannelListenerProxy,
    OnAcceptCallback, OnClosedCallback, OnFrameCallback,
};
use srpc::rpc::inmemory_channel;
use srpc::rpc::inmemory_channel::{
    inmemory_channel_clear_fault_injection, inmemory_channel_inject_drop_next_sends,
    inmemory_channel_inject_send_error, make_channel_pair_for_testing, make_inmemory_factory_proxy,
    InMemoryFactory, InMemoryListener, InMemorySwitchboard,
};
use std::cell::{Cell, RefCell};
use std::rc::Rc;
use std::sync::Arc;

fn make_factory() -> ChannelFactoryProxy {
    let switchboard: Arc<InMemorySwitchboard> = Arc::new(InMemorySwitchboard::new());
    let factory: Arc<InMemoryFactory> = Arc::new(InMemoryFactory::new(switchboard));
    make_inmemory_factory_proxy(factory)
}

fn listen_and_connect(
    factory: &mut ChannelFactoryProxy,
    address: &str,
) -> (
    ChannelListenerProxy,
    ChannelConnectionProxy,
    Rc<RefCell<Option<ChannelConnectionProxy>>>,
) {
    let mut listener: ChannelListenerProxy = factory.make_listener().unwrap();
    let accepted: Rc<RefCell<Option<ChannelConnectionProxy>>> = Rc::new(RefCell::new(None));
    let accepted_for_callback = accepted.clone();
    let accept_callable: Box<dyn Fn(ChannelConnectionProxy)> =
        Box::new(move |connection: ChannelConnectionProxy| {
            *accepted_for_callback.borrow_mut() = Some(connection);
        });
    let accept_callback: OnAcceptCallback = CallbackWrapper::from_callable(accept_callable);
    listener.set_on_accept(accept_callback);
    assert_eq!(listener.listen(address), ChannelError::None);

    let mut result = factory.connect(address);
    assert_eq!(result.error, ChannelError::None);
    let client = result.connection.take().unwrap();
    assert!(accepted.borrow().is_some());
    (listener, client, accepted)
}

#[test]
fn listener_lifecycle_collision_and_refusal_match_the_facade() {
    let mut factory = make_factory();
    assert_eq!(factory.backend_name(), "inmemory");
    assert_eq!(
        factory.connect("inmemory://absent").error,
        ChannelError::ConnectionRefused
    );

    let mut first = factory.make_listener().unwrap();
    let mut second = factory.make_listener().unwrap();
    assert_eq!(first.listen("inmemory://service"), ChannelError::None);
    assert_eq!(first.listen("inmemory://service"), ChannelError::None);
    assert_eq!(first.listen("inmemory://other"), ChannelError::AddressInUse);
    assert_eq!(
        second.listen("inmemory://service"),
        ChannelError::AddressInUse
    );
    assert_eq!(first.local_address(), "inmemory://service");
    assert!(!first.is_closed());

    // A registered listener without an accept callback refuses connections.
    assert_eq!(
        factory.connect("inmemory://service").error,
        ChannelError::ConnectionRefused
    );
    first.close();
    first.close();
    assert!(first.is_closed());
    assert_eq!(first.listen("inmemory://service"), ChannelError::Internal);
    assert_eq!(
        factory.connect("inmemory://service").error,
        ChannelError::ConnectionRefused
    );
}

#[test]
fn direct_listener_without_self_weak_reports_internal() {
    let switchboard = Arc::new(InMemorySwitchboard::new());
    let listener = InMemoryListener::new(switchboard);
    assert_eq!(
        listener.listen("inmemory://unowned"),
        ChannelError::Internal
    );
}

#[test]
fn connect_delivers_copied_bytes_and_peer_addresses_synchronously() {
    let mut factory = make_factory();
    let (_listener, mut client, accepted) =
        listen_and_connect(&mut factory, "inmemory://copy-test");
    let mut server = accepted.borrow_mut().take().unwrap();

    assert_eq!(client.peer_address(), "inmemory://copy-test");
    assert!(server.peer_address().starts_with("inmemory://client-"));

    let seen: Rc<RefCell<Vec<u8>>> = Rc::new(RefCell::new(Vec::new()));
    let seen_in_callback = seen.clone();
    let frame_callable: Box<dyn Fn(&ChannelFrame)> = Box::new(move |frame: &ChannelFrame| {
        let bytes = unsafe { core::slice::from_raw_parts(frame.payload, frame.size) };
        seen_in_callback.borrow_mut().extend_from_slice(bytes);
    });
    let frame_callback: OnFrameCallback = CallbackWrapper::from_callable(frame_callable);
    server.set_on_frame(frame_callback);

    let mut payload = [1_u8, 2_u8, 3_u8, 4_u8];
    let frame = ChannelFrame {
        payload: payload.as_ptr(),
        size: payload.len(),
    };
    assert_eq!(client.send_frame(&frame), ChannelError::None);
    payload.fill(9_u8);
    assert_eq!(&*seen.borrow(), &[1_u8, 2_u8, 3_u8, 4_u8]);
}

#[test]
fn frame_callback_can_reenter_the_peer_without_holding_the_state_lock() {
    let mut factory = make_factory();
    let (_listener, mut client, accepted) =
        listen_and_connect(&mut factory, "inmemory://reentrant");
    let server_slot = accepted;

    let replies: Rc<Cell<u32>> = Rc::new(Cell::new(0_u32));
    let replies_in_callback = replies.clone();
    let reply_callable: Box<dyn Fn(&ChannelFrame)> = Box::new(move |frame: &ChannelFrame| {
        assert_eq!(frame.size, 1_usize);
        replies_in_callback.set(replies_in_callback.get() + 1_u32);
    });
    let reply_callback: OnFrameCallback = CallbackWrapper::from_callable(reply_callable);
    client.set_on_frame(reply_callback);

    let server_for_callback = server_slot.clone();
    server_slot.borrow_mut().as_mut().unwrap().set_on_frame({
        let reentrant_callable: Box<dyn Fn(&ChannelFrame)> =
            Box::new(move |_frame: &ChannelFrame| {
                let reply = [0xA5_u8];
                let reply_frame = ChannelFrame {
                    payload: reply.as_ptr(),
                    size: reply.len(),
                };
                assert_eq!(
                    server_for_callback
                        .borrow_mut()
                        .as_mut()
                        .unwrap()
                        .send_frame(&reply_frame),
                    ChannelError::None
                );
            });
        CallbackWrapper::from_callable(reentrant_callable)
    });

    let request = [0x5A_u8];
    let request_frame = ChannelFrame {
        payload: request.as_ptr(),
        size: request.len(),
    };
    assert_eq!(client.send_frame(&request_frame), ChannelError::None);
    assert_eq!(replies.get(), 1_u32);
}

#[test]
fn close_is_peer_only_idempotent_and_kills_both_directions() {
    let mut factory = make_factory();
    let (_listener, mut client, accepted) = listen_and_connect(&mut factory, "inmemory://close");
    let mut server = accepted.borrow_mut().take().unwrap();

    let client_closed: Rc<Cell<u32>> = Rc::new(Cell::new(0_u32));
    let client_closed_cb = client_closed.clone();
    let client_close_callable: Box<dyn Fn(ChannelError)> = Box::new(move |reason: ChannelError| {
        assert_eq!(reason, ChannelError::None);
        client_closed_cb.set(client_closed_cb.get() + 1_u32);
    });
    let client_close_callback: OnClosedCallback =
        CallbackWrapper::from_callable(client_close_callable);
    client.set_on_closed(client_close_callback);

    let server_closed: Rc<Cell<u32>> = Rc::new(Cell::new(0_u32));
    let server_closed_cb = server_closed.clone();
    let server_close_callable: Box<dyn Fn(ChannelError)> = Box::new(move |reason: ChannelError| {
        assert_eq!(reason, ChannelError::None);
        server_closed_cb.set(server_closed_cb.get() + 1_u32);
    });
    let server_close_callback: OnClosedCallback =
        CallbackWrapper::from_callable(server_close_callable);
    server.set_on_closed(server_close_callback);

    client.close();
    client.close();
    assert_eq!(client_closed.get(), 0_u32);
    assert_eq!(server_closed.get(), 1_u32);
    assert!(client.is_closed());
    assert!(server.is_closed());

    let byte = [1_u8];
    let frame = ChannelFrame {
        payload: byte.as_ptr(),
        size: byte.len(),
    };
    assert_eq!(client.send_frame(&frame), ChannelError::ConnectionReset);
    assert_eq!(server.send_frame(&frame), ChannelError::ConnectionReset);
    server.close();
    assert_eq!(client_closed.get(), 0_u32);
    assert_eq!(server_closed.get(), 1_u32);
}

#[test]
fn fault_injection_is_per_side_drop_first_and_clearable() {
    let (a, b) = make_channel_pair_for_testing("side-a".to_owned(), "side-b".to_owned());
    let mut a_proxy = inmemory_channel::make_inmemory_channel_proxy(a.clone());
    let mut b_proxy = inmemory_channel::make_inmemory_channel_proxy(b.clone());
    let delivered: Rc<Cell<u32>> = Rc::new(Cell::new(0_u32));
    let delivered_cb = delivered.clone();
    let delivered_callable: Box<dyn Fn(&ChannelFrame)> = Box::new(move |_frame: &ChannelFrame| {
        delivered_cb.set(delivered_cb.get() + 1_u32);
    });
    let delivered_callback: OnFrameCallback = CallbackWrapper::from_callable(delivered_callable);
    b_proxy.set_on_frame(delivered_callback);

    inmemory_channel_inject_drop_next_sends(&a, 2_i32);
    inmemory_channel_inject_send_error(&a, ChannelError::WouldBlock, 2_i32);

    let bytes = [8_u8, 9_u8];
    let frame = ChannelFrame {
        payload: bytes.as_ptr(),
        size: bytes.len(),
    };
    assert_eq!(a_proxy.send_frame(&frame), ChannelError::None);
    assert_eq!(a_proxy.send_frame(&frame), ChannelError::None);
    assert_eq!(a_proxy.send_frame(&frame), ChannelError::WouldBlock);
    assert_eq!(a_proxy.send_frame(&frame), ChannelError::WouldBlock);
    assert_eq!(a_proxy.send_frame(&frame), ChannelError::None);
    assert_eq!(delivered.get(), 1_u32);

    // B's state is independent of A's consumed counters.
    assert_eq!(b_proxy.send_frame(&frame), ChannelError::None);
    inmemory_channel_inject_drop_next_sends(&b, 1_i32);
    inmemory_channel_clear_fault_injection(&b);
    assert_eq!(b_proxy.send_frame(&frame), ChannelError::None);
}
