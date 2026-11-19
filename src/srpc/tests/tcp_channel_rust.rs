#![allow(unsafe_code)]

use srpc::callback_wrapper::detail::CallbackWrapper;
use srpc::channel::{
    ChannelError, ChannelFrame, ChannelListenerProxy, OnAcceptCallback, OnClosedCallback,
};
use srpc::reactor::PollThread;
use srpc::tcp_channel::{
    kTcpConnectionOutboundHighWaterDefault, make_tcp_listener_channel_proxy, TcpConnection,
    TcpListener,
};
use std::net::TcpStream;
use std::os::fd::IntoRawFd;
use std::os::unix::net::UnixStream;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{mpsc, Arc, Barrier};
use std::thread;
use std::time::Duration;

fn assert_send_sync<T: Send + Sync>() {}

#[test]
fn fresh_listener_preserves_the_invalid_fd_contract() {
    assert_send_sync::<TcpConnection>();
    assert_send_sync::<TcpListener>();

    let listener = TcpListener::new();

    assert_eq!(listener.fd(), -1);
    assert_eq!(listener.local_address(), "");
    assert!(!listener.is_closed());
}

#[test]
fn listener_bind_close_and_single_use_state_match_the_cpp_contract() {
    let listener = TcpListener::new();

    assert_eq!(
        listener.listen("not-an-address"),
        ChannelError::AddressInvalid
    );
    assert_eq!(listener.fd(), -1);
    assert_eq!(listener.listen("127.0.0.1:0"), ChannelError::None);
    assert!(listener.fd() >= 0);
    assert!(listener.local_address().starts_with("127.0.0.1:"));
    assert_eq!(listener.listen("127.0.0.1:0"), ChannelError::AddressInUse);

    listener.close();
    listener.close();
    assert!(listener.is_closed());
    assert_eq!(listener.fd(), -1);
    assert_eq!(listener.listen("127.0.0.1:0"), ChannelError::AddressInUse);
}

#[test]
fn connection_constructor_owns_the_fd_and_preserves_initial_state() {
    let (owned, _peer) = UnixStream::pair().unwrap();
    let raw_fd = owned.into_raw_fd();
    // SAFETY: into_raw_fd transferred the stream's unique descriptor here.
    let connection = unsafe { TcpConnection::new(raw_fd, "test-peer".to_string()) };

    assert_eq!(connection.fd(), raw_fd);
    assert_eq!(connection.peer_address(), "test-peer");
    assert!(!connection.is_closed());
    assert_eq!(kTcpConnectionOutboundHighWaterDefault, 4 * 1024 * 1024);
}

#[test]
fn concurrent_listener_bind_and_close_never_publish_a_live_fd_after_close() {
    for _ in 0..128 {
        let listener = Arc::new(TcpListener::new());
        let barrier = Arc::new(Barrier::new(3));

        let bind_listener = Arc::clone(&listener);
        let bind_barrier = Arc::clone(&barrier);
        let bind = thread::spawn(move || {
            bind_barrier.wait();
            bind_listener.listen("127.0.0.1:0")
        });

        let close_listener = Arc::clone(&listener);
        let close_barrier = Arc::clone(&barrier);
        let close = thread::spawn(move || {
            close_barrier.wait();
            close_listener.close();
        });

        barrier.wait();
        let bind_result = bind.join().unwrap();
        close.join().unwrap();

        assert!(matches!(
            bind_result,
            ChannelError::None | ChannelError::AddressInUse
        ));
        assert!(listener.is_closed());
        assert_eq!(listener.fd(), -1);
    }
}

#[test]
fn closed_callback_can_replace_itself_without_deadlocking() {
    let (owned, _peer) = UnixStream::pair().unwrap();
    let raw_fd = owned.into_raw_fd();
    // SAFETY: into_raw_fd transferred the stream's unique descriptor here.
    let connection =
        Arc::new(unsafe { TcpConnection::new(raw_fd, "reentrant-test-peer".to_string()) });
    let weak = Arc::downgrade(&connection);
    let (fired_tx, fired_rx) = mpsc::channel();
    let callback: OnClosedCallback =
        CallbackWrapper::from_callable(Box::new(move |reason: ChannelError| {
            if let Some(connection) = weak.upgrade() {
                connection.set_on_closed(OnClosedCallback::default());
            }
            fired_tx.send(reason).unwrap();
        }));
    connection.set_on_closed(callback);

    let closing_connection = Arc::clone(&connection);
    let close = thread::spawn(move || closing_connection.close());

    assert_eq!(
        fired_rx.recv_timeout(Duration::from_secs(2)).unwrap(),
        ChannelError::None
    );
    close.join().unwrap();
    assert!(connection.is_closed());
    assert_eq!(connection.fd(), -1);
}

// A listener attached to `pt`: its accept task runs the accept driver on the
// poll thread, as production listeners do (S5).  The returned proxy keeps
// the registration; the Arc is for closing from any thread.
fn attached_listener(pt: &Arc<PollThread>, on_accept: OnAcceptCallback) -> (Arc<TcpListener>, ChannelListenerProxy) {
    let mut listener = Arc::new(TcpListener::new());
    Arc::get_mut(&mut listener).unwrap().set_poll_thread(pt.clone());
    listener.set_on_accept(on_accept);
    let mut proxy = make_tcp_listener_channel_proxy(listener.clone());
    assert_eq!(proxy.listen("127.0.0.1:0"), ChannelError::None);
    (listener, proxy)
}

// Since S5 the accept driver runs only in the listener's accept task, on its
// PollThread; these tests used to drive it through the retired pollable
// `TcpListener::handle_read` from arbitrary threads.  The contract is the
// same: once close() returns, no accept callback starts.
#[test]
fn close_return_never_precedes_a_new_accept_callback() {
    let pt = PollThread::create();
    for _ in 0..64 {
        let close_returned = Arc::new(AtomicBool::new(false));
        let late_callback = Arc::new(AtomicBool::new(false));
        let close_returned_in_callback = Arc::clone(&close_returned);
        let late_callback_in_callback = Arc::clone(&late_callback);
        let callback: OnAcceptCallback = CallbackWrapper::from_callable(Box::new(move |_proxy| {
            if close_returned_in_callback.load(Ordering::SeqCst) {
                late_callback_in_callback.store(true, Ordering::SeqCst);
            }
        }));
        let (listener, proxy) = attached_listener(&pt, callback);

        let barrier = Arc::new(Barrier::new(3));
        let address = listener.local_address();
        let connect_barrier = Arc::clone(&barrier);
        let connect = thread::spawn(move || {
            connect_barrier.wait();
            TcpStream::connect(address)
        });

        let close_listener = Arc::clone(&listener);
        let close_barrier = Arc::clone(&barrier);
        let close_returned_in_thread = Arc::clone(&close_returned);
        let close = thread::spawn(move || {
            close_barrier.wait();
            close_listener.close();
            close_returned_in_thread.store(true, Ordering::SeqCst);
        });

        barrier.wait();
        close.join().unwrap();
        let _client = connect.join().unwrap();
        // Let the accept task run whatever edge the connect raised.
        thread::sleep(Duration::from_millis(2));
        assert!(!late_callback.load(Ordering::SeqCst));
        assert!(listener.is_closed());
        assert_eq!(listener.fd(), -1);
        drop(proxy);
    }
    pt.shutdown();
}

#[test]
fn close_waits_for_the_whole_accept_driver() {
    let pt = PollThread::create();
    let callbacks_entered = Arc::new(AtomicU32::new(0));
    let release_first = Arc::new(AtomicBool::new(false));
    let (first_entered_tx, first_entered_rx) = mpsc::channel();
    let callback: OnAcceptCallback = CallbackWrapper::from_callable(Box::new({
        let callbacks_entered = Arc::clone(&callbacks_entered);
        let release_first = Arc::clone(&release_first);
        move |_proxy| {
            let index = callbacks_entered.fetch_add(1, Ordering::SeqCst);
            if index == 0 {
                first_entered_tx.send(()).unwrap();
                while !release_first.load(Ordering::Acquire) {
                    thread::yield_now();
                }
            }
        }
    }));
    let (listener, proxy) = attached_listener(&pt, callback);

    // The first accept callback runs on the poll thread and blocks there.
    let client1 = TcpStream::connect(listener.local_address()).unwrap();
    first_entered_rx
        .recv_timeout(Duration::from_secs(5))
        .unwrap();
    // A second connection queues behind the blocked driver.
    let client2 = TcpStream::connect(listener.local_address()).unwrap();

    let (close_done_tx, close_done_rx) = mpsc::channel();
    let closer1_listener = Arc::clone(&listener);
    let close1_done_tx = close_done_tx.clone();
    let closer1 = thread::spawn(move || {
        closer1_listener.close();
        close1_done_tx.send(()).unwrap();
    });
    while !listener.is_closed() {
        thread::yield_now();
    }

    // Exercise the already-closed path too: every non-owner close must wait
    // for the live accept owner, not only the first caller that set `closed_`.
    let closer2_listener = Arc::clone(&listener);
    let closer2 = thread::spawn(move || {
        closer2_listener.close();
        close_done_tx.send(()).unwrap();
    });
    let a_close_returned_while_first_callback_was_live = close_done_rx
        .recv_timeout(Duration::from_millis(200))
        .is_ok();

    release_first.store(true, Ordering::Release);
    if !a_close_returned_while_first_callback_was_live {
        close_done_rx.recv_timeout(Duration::from_secs(5)).unwrap();
    }
    close_done_rx.recv_timeout(Duration::from_secs(5)).unwrap();
    closer1.join().unwrap();
    closer2.join().unwrap();
    // The driver saw close and accepted nothing more.
    thread::sleep(Duration::from_millis(20));
    drop((client1, client2));

    assert_eq!(callbacks_entered.load(Ordering::SeqCst), 1);
    assert!(
        !a_close_returned_while_first_callback_was_live,
        "a non-owner close returned while the whole accept driver was live"
    );
    assert_eq!(listener.fd(), -1);
    drop(proxy);
    pt.shutdown();
}

#[test]
fn accept_callback_can_close_its_listener_without_deadlocking() {
    let pt = PollThread::create();
    let slot: Arc<std::sync::Mutex<Option<std::sync::Weak<TcpListener>>>> =
        Arc::new(std::sync::Mutex::new(None));
    let callback_slot = Arc::clone(&slot);
    let (closed_tx, closed_rx) = mpsc::channel();
    let callback: OnAcceptCallback = CallbackWrapper::from_callable(Box::new(move |_proxy| {
        let weak = callback_slot.lock().unwrap().clone().unwrap();
        // The owner closing reentrantly, on the poll thread, must not wait
        // for itself.
        weak.upgrade().unwrap().close();
        closed_tx.send(()).unwrap();
    }));
    let (listener, proxy) = attached_listener(&pt, callback);
    *slot.lock().unwrap() = Some(Arc::downgrade(&listener));

    let _client = TcpStream::connect(listener.local_address()).unwrap();
    closed_rx.recv_timeout(Duration::from_secs(5)).unwrap();
    assert!(listener.is_closed());
    assert_eq!(listener.fd(), -1);
    drop(proxy);
    pt.shutdown();
}

#[test]
fn flush_failure_still_delivers_one_reentrant_close_callback() {
    let (owned, peer) = UnixStream::pair().unwrap();
    owned.set_nonblocking(true).unwrap();
    // SAFETY: into_raw_fd transfers the stream's sole descriptor ownership.
    let connection = Arc::new(unsafe {
        TcpConnection::new(owned.into_raw_fd(), "flush-failure".to_string())
    });
    let callbacks = Arc::new(AtomicU32::new(0));
    let observed = callbacks.clone();
    let weak = Arc::downgrade(&connection);
    connection.set_on_closed(CallbackWrapper::from_callable(Box::new(move |reason| {
        assert_eq!(reason, ChannelError::None);
        observed.fetch_add(1, Ordering::SeqCst);
        if let Some(connection) = weak.upgrade() {
            assert_eq!(connection.fd(), -1);
            connection.close();
            connection.set_on_closed(OnClosedCallback::default());
        }
    })));
    let bytes = [0x11, 0x22, 0x33];
    let frame = ChannelFrame { payload: bytes.as_ptr(), size: bytes.len() };
    // SAFETY: frame points at the live, immutable bytes for this call.
    assert_eq!(unsafe { connection.send_frame(&frame) }, ChannelError::None);
    drop(peer);
    connection.flush();
    assert!(connection.is_closed(), "peer closure must produce a real write failure");
    assert_eq!(callbacks.load(Ordering::SeqCst), 0);
    connection.close();
    connection.close();
    assert_eq!(connection.fd(), -1);
    assert_eq!(callbacks.load(Ordering::SeqCst), 1);
}
