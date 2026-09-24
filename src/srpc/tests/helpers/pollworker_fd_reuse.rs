//! Real descriptor reuse between registration and the worker's closed sweep.

#![allow(unsafe_code)]

use super::*;
use crate::channel::{ChannelError, ChannelFrame, OnFrameCallback};
use crate::tcp_channel::{
    TcpConnection, TcpListener, make_tcp_connection_pollable_proxy,
    make_tcp_listener_pollable_proxy,
};
use std::io::{Read, Write};
use std::os::fd::{AsRawFd, FromRawFd, IntoRawFd, OwnedFd};
use std::os::unix::net::UnixStream;

unsafe extern "C" {
    fn fcntl(fd: i32, command: i32, ...) -> i32;
}

static FD_REUSE_TESTS: std::sync::Mutex<()> = std::sync::Mutex::new(());

fn duplicate_at_or_above(stream: &UnixStream, minimum: i32) -> OwnedFd {
    // F_DUPFD_CLOEXEC selects a free descriptor. Unlike dup2, it cannot close
    // an unrelated descriptor if another test allocates one concurrently.
    const F_DUPFD_CLOEXEC: i32 = 1030;
    let duplicated = unsafe { fcntl(stream.as_raw_fd(), F_DUPFD_CLOEXEC, minimum) };
    assert!(duplicated >= minimum, "{}", std::io::Error::last_os_error());
    // SAFETY: successful F_DUPFD_CLOEXEC transfers a fresh descriptor to us.
    unsafe { OwnedFd::from_raw_fd(duplicated) }
}

fn replacement_delivers_a_real_frame(deferred_removal: bool) {
    let _serial = FD_REUSE_TESTS
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let (_sender, receiver) = std::sync::mpsc::channel::<PollCommand>();
    let mut worker = pollworker_make(receiver);
    let (old_socket, mut old_peer) = UnixStream::pair().unwrap();
    old_socket.set_nonblocking(true).unwrap();
    // Keep the deliberate reuse away from ordinary low-number allocations.
    let old_fd = duplicate_at_or_above(&old_socket, 256).into_raw_fd();
    drop(old_socket);
    // SAFETY: the duplicate's sole ownership transfers to TcpConnection.
    let old = Arc::new(unsafe { TcpConnection::new(old_fd, "old".to_string()) });
    pollworker_do_add_pollable(&mut worker, make_tcp_connection_pollable_proxy(old.clone()));

    old_peer.write_all(&0_u32.to_ne_bytes()).unwrap();
    let mut initial_ready = false;
    worker.poll_.Wait(|fd, events| {
        initial_ready |= fd == old_fd && events & PollReady::READABLE != 0;
    });
    assert!(
        initial_ready,
        "the original socket must first be registered"
    );

    // A duplicate Add for a still-live owner must preserve its registration
    // and must not close its socket while retiring the redundant proxy.
    pollworker_do_add_pollable(&mut worker, make_tcp_connection_pollable_proxy(old.clone()));
    assert!(!old.is_closed());
    assert!(
        worker
            .fd_to_pollable_
            .get_mut(&old_fd)
            .unwrap()
            .handle_read()
    );

    let (replacement_socket, mut replacement_peer) = UnixStream::pair().unwrap();
    replacement_socket.set_nonblocking(true).unwrap();
    if deferred_removal {
        pollworker_do_remove_pollable(&mut worker, old_fd);
        assert!(worker.pending_remove_.contains(&old_fd));
    }
    old.close();
    assert_eq!(old.fd(), -1);
    assert!(worker.fd_to_pollable_.get(&old_fd).unwrap().is_closed());

    // The production proxy pins the original descriptor until unregister.
    // Explicit retirement must also cancel a pending old removal before the
    // same descriptor can be allocated to a replacement.
    assert_fd_live(old_fd);
    pollworker_do_close_pollable(&mut worker, old_fd);
    assert_fd_closed(old_fd);
    assert!(!worker.pending_remove_.contains(&old_fd));
    let replacement_fd = duplicate_at_or_above(&replacement_socket, old_fd).into_raw_fd();
    drop(replacement_socket);
    assert_eq!(
        replacement_fd, old_fd,
        "the fixture must reuse the exact descriptor"
    );
    // SAFETY: the new duplicate's sole ownership transfers to TcpConnection.
    let replacement =
        Arc::new(unsafe { TcpConnection::new(replacement_fd, "replacement".to_string()) });
    let received = Arc::new(std::sync::Mutex::new(Vec::<u8>::new()));
    let callback_received = received.clone();
    replacement.set_on_frame(OnFrameCallback::from_callable(Box::new(
        move |frame: &ChannelFrame| {
            // SAFETY: TcpConnection supplies this frame's readable payload for the
            // duration of the callback; copy it before returning.
            let payload = unsafe { std::slice::from_raw_parts(frame.payload, frame.size) };
            callback_received.lock().unwrap().extend_from_slice(payload);
        },
    )));
    pollworker_do_add_pollable(
        &mut worker,
        make_tcp_connection_pollable_proxy(replacement.clone()),
    );
    pollworker_process_pending_removals(&mut worker);

    replacement_peer.write_all(&3_u32.to_ne_bytes()).unwrap();
    replacement_peer.write_all(b"new").unwrap();
    let mut readable = Vec::new();
    worker.poll_.Wait(|fd, events| {
        if events & PollReady::READABLE != 0 {
            readable.push(fd);
        }
    });
    assert_eq!(
        readable,
        vec![replacement_fd],
        "the replacement must reach epoll"
    );
    for fd in readable {
        assert!(worker.fd_to_pollable_.get_mut(&fd).unwrap().handle_read());
    }
    assert_eq!(*received.lock().unwrap(), b"new");
    assert!(!replacement.is_closed());
}

#[test]
fn retired_registration_releases_fd_before_replacement_is_admitted() {
    replacement_delivers_a_real_frame(false);
}

#[test]
fn reused_fd_is_not_removed_by_the_retired_registrations_pending_removal() {
    replacement_delivers_a_real_frame(true);
}

fn assert_fd_live(fd: i32) {
    const F_GETFD: i32 = 1;
    // F_GETFD observes descriptor ownership without allocating another fd.
    assert!(unsafe { fcntl(fd, F_GETFD) } >= 0);
}

fn assert_fd_closed(fd: i32) {
    const F_GETFD: i32 = 1;
    assert_eq!(unsafe { fcntl(fd, F_GETFD) }, -1);
    assert_eq!(std::io::Error::last_os_error().raw_os_error(), Some(9));
}

fn controlled_connection() -> (Arc<TcpConnection>, UnixStream, i32) {
    let (socket, peer) = UnixStream::pair().unwrap();
    socket.set_nonblocking(true).unwrap();
    peer.set_nonblocking(true).unwrap();
    let fd = duplicate_at_or_above(&socket, 256).into_raw_fd();
    // SAFETY: the fresh duplicate's ownership transfers to this connection.
    let connection = Arc::new(unsafe { TcpConnection::new(fd, "leased".to_string()) });
    (connection, peer, fd)
}

#[test]
fn closed_queued_add_keeps_descriptor_until_command_retirement() {
    let _serial = FD_REUSE_TESTS
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let (sender, receiver) = std::sync::mpsc::channel::<PollCommand>();
    let mut worker = pollworker_make(receiver);
    let (connection, mut peer, fd) = controlled_connection();
    assert!(
        sender
            .send(PollCommand::AddPollable {
                pollable: make_tcp_connection_pollable_proxy(connection.clone()),
            })
            .is_ok()
    );

    // Deliberately leave ADD in the real command queue while close runs.
    connection.close();
    assert!(connection.is_closed());
    assert_eq!(connection.fd(), -1);
    assert_fd_live(fd);
    let mut byte = [0_u8; 1];
    assert_eq!(
        peer.read(&mut byte).unwrap(),
        0,
        "logical close must shut down the socket"
    );

    pollworker_process_commands(&mut worker);
    assert!(!worker.fd_to_pollable_.contains_key(&fd));
    assert!(!worker.mode_.contains_key(&fd));
    assert_fd_closed(fd);
}

#[test]
fn closed_active_registration_keeps_descriptor_until_deferred_removal() {
    let _serial = FD_REUSE_TESTS
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let (_sender, receiver) = std::sync::mpsc::channel::<PollCommand>();
    let mut worker = pollworker_make(receiver);
    let (connection, _peer, fd) = controlled_connection();
    pollworker_do_add_pollable(
        &mut worker,
        make_tcp_connection_pollable_proxy(connection.clone()),
    );
    assert!(worker.mode_.contains_key(&fd));

    let (unrelated, mut unrelated_peer, unrelated_fd) = controlled_connection();
    pollworker_do_add_pollable(
        &mut worker,
        make_tcp_connection_pollable_proxy(unrelated.clone()),
    );
    connection.close();
    assert!(connection.is_closed());
    assert_eq!(connection.fd(), -1);
    assert_fd_live(fd);
    assert_ne!(fd, unrelated_fd);
    pollworker_do_remove_pollable(&mut worker, fd);
    assert_fd_live(fd);
    pollworker_process_pending_removals(&mut worker);
    assert_fd_closed(fd);
    assert!(!worker.fd_to_pollable_.contains_key(&fd));
    assert!(!worker.mode_.contains_key(&fd));

    // Retirement of the old registration must leave another live socket alone.
    assert_fd_live(unrelated_fd);
    unrelated_peer.write_all(&0_u32.to_ne_bytes()).unwrap();
    let mut ready = Vec::new();
    worker.poll_.Wait(|ready_fd, events| {
        if events & PollReady::READABLE != 0 {
            ready.push(ready_fd);
        }
    });
    assert_eq!(ready, vec![unrelated_fd]);
    assert!(
        worker
            .fd_to_pollable_
            .get_mut(&unrelated_fd)
            .unwrap()
            .handle_read()
    );
    assert!(!unrelated.is_closed());
}

#[test]
fn closed_queued_listener_keeps_descriptor_until_command_retirement() {
    let _serial = FD_REUSE_TESTS
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let (sender, receiver) = std::sync::mpsc::channel::<PollCommand>();
    let mut worker = pollworker_make(receiver);
    let listener = Arc::new(TcpListener::new());
    assert!(listener.listen("127.0.0.1:0") == ChannelError::None);
    let fd = listener.fd();
    assert!(fd >= 0);
    assert!(
        sender
            .send(PollCommand::AddPollable {
                pollable: make_tcp_listener_pollable_proxy(listener.clone()),
            })
            .is_ok()
    );
    listener.close();
    assert!(listener.is_closed());
    assert_eq!(listener.fd(), -1);
    assert_fd_live(fd);
    pollworker_process_commands(&mut worker);
    assert!(!worker.fd_to_pollable_.contains_key(&fd));
    assert!(!worker.mode_.contains_key(&fd));
    assert_fd_closed(fd);
}
