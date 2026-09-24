use srpc::epoll_wrapper::{
    epoll_bump_remove_count, epoll_remove_count, PollMode, PollReady, Pollable,
};
use std::sync::atomic::Ordering;

// Two tests below allocate real descriptors (socketpairs plus the epoll fd),
// and one of them asserts on a descriptor it has just CLOSED. Linux hands out
// the lowest free number, and the harness runs one binary's tests on parallel
// threads, so a socketpair opened by the other test can land on the closed
// number between the `drop` and the `epoll_ctl` -- the "closed" descriptor is
// live again and `Add` succeeds (seen once in a gate run as `left: 0,
// right: -1`). The fd-allocating tests hold this lock so no other descriptor
// allocation in the process can interleave. Poisoning is tolerated so that a
// failing test does not turn the other into a misleading PoisonError.
static FD_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

fn hold_fd_lock() -> std::sync::MutexGuard<'static, ()> {
    FD_LOCK.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

struct TestPollable {
    fd: i32,
    mode: i32,
    closed: bool,
}

impl Pollable for TestPollable {
    fn fd(&self) -> i32 {
        self.fd
    }

    fn poll_mode(&self) -> i32 {
        self.mode
    }

    fn content_size(&mut self) -> usize {
        0
    }

    fn handle_read(&mut self) -> bool {
        true
    }

    fn handle_write(&mut self) -> i32 {
        PollMode::NO_CHANGE
    }

    fn handle_error(&mut self) {}

    fn close(&mut self) {
        self.closed = true;
    }

    fn check_pending_write_update(&self) -> bool {
        false
    }

    fn is_closed(&self) -> bool {
        self.closed
    }
}

#[test]
fn constants_and_pollable_contract_match_the_cpp_surface() {
    assert_eq!(
        (PollMode::READ, PollMode::WRITE, PollMode::NO_CHANGE),
        (1, 2, -1)
    );
    assert_eq!(
        (PollReady::READABLE, PollReady::WRITABLE, PollReady::ERROR),
        (1, 2, 4)
    );

    let mut pollable = TestPollable {
        fd: 7,
        mode: PollMode::READ,
        closed: false,
    };
    assert_eq!(pollable.fd(), 7);
    assert_eq!(pollable.poll_mode(), PollMode::READ);
    assert_eq!(pollable.content_size(), 0);
    assert!(pollable.handle_read());
    assert_eq!(pollable.handle_write(), PollMode::NO_CHANGE);
    assert!(!pollable.check_pending_write_update());
    pollable.handle_error();
    pollable.close();
    assert!(pollable.is_closed());
}

#[test]
fn remove_counter_uses_the_established_atomic_increment() {
    epoll_remove_count.store(0, Ordering::SeqCst);
    epoll_bump_remove_count();
    epoll_bump_remove_count();
    assert_eq!(epoll_remove_count.load(Ordering::SeqCst), 2);
}

#[test]
fn kernel_batch_preserves_each_descriptor_and_interest_update() {
    use srpc::epoll_wrapper::Epoll;
    use std::io::{Read, Write};
    use std::os::fd::AsRawFd;
    use std::os::unix::net::UnixStream;

    let _fds = hold_fd_lock();
    let (mut first, mut first_peer) = UnixStream::pair().unwrap();
    let (mut second, mut second_peer) = UnixStream::pair().unwrap();
    let mut poll = Epoll::new();
    assert_eq!(poll.Add(first.as_raw_fd(), PollMode::READ), 0);
    assert_eq!(poll.Add(second.as_raw_fd(), PollMode::READ), 0);
    // EEXIST follows the shared delete/re-add policy.
    assert_eq!(poll.Add(first.as_raw_fd(), PollMode::READ), 0);
    first_peer.write_all(b"a").unwrap();
    second_peer.write_all(b"b").unwrap();
    let mut ready = Vec::new();
    poll.Wait(|fd, flags| {
        assert_ne!(flags & PollReady::READABLE, 0);
        ready.push(fd);
    });
    ready.sort();
    let mut expected = vec![first.as_raw_fd(), second.as_raw_fd()];
    expected.sort();
    assert_eq!(ready, expected);
    let mut byte = [0u8; 1];
    first.read_exact(&mut byte).unwrap();
    second.read_exact(&mut byte).unwrap();
    assert_eq!(poll.Update(first.as_raw_fd(), PollMode::WRITE, PollMode::READ), 0);
    ready.clear();
    poll.Wait(|fd, flags| {
        if flags & PollReady::WRITABLE != 0 { ready.push(fd); }
    });
    assert_eq!(ready, [first.as_raw_fd()]);
    drop(first_peer);
    let mut errors = Vec::new();
    poll.Wait(|fd, flags| {
        if flags & PollReady::ERROR != 0 { errors.push(fd); }
    });
    assert_eq!(errors, [first.as_raw_fd()]);
}

#[test]
fn closed_descriptor_registration_and_update_tolerate_teardown() {
    use srpc::epoll_wrapper::Epoll;
    use std::os::fd::AsRawFd;
    use std::os::unix::net::UnixStream;

    let _fds = hold_fd_lock();
    let mut poll = Epoll::new();
    let (stream, _peer) = UnixStream::pair().unwrap();
    let fd = stream.as_raw_fd();
    drop(stream);
    assert_eq!(poll.Add(fd, PollMode::READ), -1);
    assert_eq!(poll.Update(fd, PollMode::WRITE, PollMode::READ), 0);
}
