#![allow(unsafe_code)]

use rrr::channel::{ChannelError, OnAcceptCallback, OnClosedCallback};
use rrr::tcp_channel::{kTcpConnectionOutboundHighWaterDefault, TcpConnection, TcpListener};
use rusty::CallbackWrapper;
use std::net::TcpStream;
use std::os::fd::IntoRawFd;
use std::os::unix::net::UnixStream;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{mpsc, Arc, Barrier};
use std::thread;
use std::time::Duration;

fn assert_send_sync<T: Send + Sync>() {}

static NEXT_TEST_THREAD_ID: AtomicU32 = AtomicU32::new(1);

thread_local! {
    static TEST_THREAD_ID: u32 = NEXT_TEST_THREAD_ID.fetch_add(1, Ordering::Relaxed);
}

// Cargo's Rust lane does not compile the production plain-C syscall seam.
// These inert definitions satisfy references in unexercised TCP I/O helpers;
// the CMake/Clang runtime lane links and exercises rpc/srpc_connect.c itself.
#[no_mangle]
pub extern "C" fn srpc_tcp_recv_scratch() -> *mut u8 {
    core::ptr::null_mut()
}

#[no_mangle]
pub extern "C" fn srpc_tcp_recv_bytes(_fd: i32, _data: *mut u8, _size: usize) -> i64 {
    -1
}

#[no_mangle]
pub extern "C" fn srpc_tcp_send_bytes(_fd: i32, _data: *const u8, _size: usize) -> i64 {
    -1
}

#[no_mangle]
pub extern "C" fn srpc_tcp_shutdown(_fd: i32) -> i32 {
    0
}

#[no_mangle]
pub extern "C" fn srpc_tcp_set_nonblocking(_fd: i32) -> i32 {
    0
}

#[no_mangle]
pub extern "C" fn srpc_tcp_last_errno() -> i32 {
    0
}

#[no_mangle]
pub extern "C" fn srpc_tcp_current_thread_id() -> u32 {
    TEST_THREAD_ID.with(|thread_id| *thread_id)
}

#[test]
fn fresh_listener_preserves_the_invalid_fd_and_basic_pollable_contract() {
    assert_send_sync::<TcpConnection>();
    assert_send_sync::<TcpListener>();

    let listener = TcpListener::new();

    assert_eq!(listener.fd(), -1);
    assert_eq!(listener.local_address(), "");
    assert!(!listener.is_closed());
    assert_eq!(listener.poll_mode(), 1);
    assert_eq!(listener.content_size(), 0);
    assert_eq!(listener.handle_write(), -1);
    assert!(!listener.check_pending_write_update());
    assert!(!listener.handle_read());
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
    assert_eq!(connection.poll_mode(), 1);
    assert_eq!(connection.content_size(), 0);
    assert!(!connection.check_pending_write_update());
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

#[test]
fn close_return_never_precedes_a_new_accept_callback() {
    for _ in 0..128 {
        let listener = Arc::new(TcpListener::new());
        assert_eq!(listener.listen("127.0.0.1:0"), ChannelError::None);

        let close_returned = Arc::new(AtomicBool::new(false));
        let late_callback = Arc::new(AtomicBool::new(false));
        let close_returned_in_callback = Arc::clone(&close_returned);
        let late_callback_in_callback = Arc::clone(&late_callback);
        let callback: OnAcceptCallback = CallbackWrapper::from_callable(Box::new(move |_proxy| {
            if close_returned_in_callback.load(Ordering::SeqCst) {
                late_callback_in_callback.store(true, Ordering::SeqCst);
            }
        }));
        listener.set_on_accept(callback);

        let _client = TcpStream::connect(listener.local_address()).unwrap();
        let barrier = Arc::new(Barrier::new(3));

        let read_listener = Arc::clone(&listener);
        let read_barrier = Arc::clone(&barrier);
        let read = thread::spawn(move || {
            read_barrier.wait();
            read_listener.handle_read()
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
        let _ = read.join().unwrap();
        assert!(!late_callback.load(Ordering::SeqCst));
        assert!(listener.is_closed());
        assert_eq!(listener.fd(), -1);
    }
}

#[test]
fn close_waits_for_the_whole_accept_driver_with_two_readers() {
    let listener = Arc::new(TcpListener::new());
    assert_eq!(listener.listen("127.0.0.1:0"), ChannelError::None);

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
    listener.set_on_accept(callback);

    let client1 = TcpStream::connect(listener.local_address()).unwrap();
    let reader1_listener = Arc::clone(&listener);
    let reader1 = thread::spawn(move || reader1_listener.handle_read());
    first_entered_rx
        .recv_timeout(Duration::from_secs(2))
        .unwrap();

    // A second safe reader must not overwrite the first reader's owner TID.
    // It returns without accepting the second queued connection.
    let client2 = TcpStream::connect(listener.local_address()).unwrap();
    let reader2_listener = Arc::clone(&listener);
    let reader2 = thread::spawn(move || reader2_listener.handle_read());
    let reader2_result = reader2.join().unwrap();

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
    let _ = reader1.join().unwrap();
    if !a_close_returned_while_first_callback_was_live {
        close_done_rx.recv_timeout(Duration::from_secs(2)).unwrap();
    }
    close_done_rx.recv_timeout(Duration::from_secs(2)).unwrap();
    closer1.join().unwrap();
    closer2.join().unwrap();
    drop((client1, client2));

    assert!(!reader2_result);
    assert_eq!(callbacks_entered.load(Ordering::SeqCst), 1);
    assert!(
        !a_close_returned_while_first_callback_was_live,
        "a non-owner close returned while the whole accept driver was live"
    );
    assert_eq!(listener.fd(), -1);
}

#[test]
fn accept_callback_can_close_its_listener_without_deadlocking() {
    let listener = Arc::new(TcpListener::new());
    assert_eq!(listener.listen("127.0.0.1:0"), ChannelError::None);

    let weak_listener = Arc::downgrade(&listener);
    let (closed_tx, closed_rx) = mpsc::channel();
    let callback: OnAcceptCallback = CallbackWrapper::from_callable(Box::new(move |_proxy| {
        let listener = weak_listener.upgrade().unwrap();
        listener.close();
        closed_tx.send(()).unwrap();
    }));
    listener.set_on_accept(callback);

    let _client = TcpStream::connect(listener.local_address()).unwrap();
    assert!(listener.handle_read());
    closed_rx.recv_timeout(Duration::from_secs(2)).unwrap();
    assert!(listener.is_closed());
    assert_eq!(listener.fd(), -1);
}
