use srpc::runtime::legacy_epoll::{
    epoll_remove_count, epoll_wait_impl, Epoll, PollMode, PollReady, Pollable,
};
use std::io::Write;
use std::os::fd::AsRawFd;
use std::os::unix::net::UnixStream;
use std::sync::atomic::Ordering;

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
        0_usize
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
        fd: 7_i32,
        mode: PollMode::READ,
        closed: false,
    };
    assert_eq!(pollable.fd(), 7_i32);
    assert_eq!(pollable.poll_mode(), PollMode::READ);
    assert_eq!(pollable.handle_write(), PollMode::NO_CHANGE);
    assert!(!pollable.is_closed());
    pollable.close();
    assert!(pollable.is_closed());
}

#[test]
fn real_epoll_add_wait_update_and_remove_preserve_linux_behavior() {
    let (reader, mut writer) = UnixStream::pair().unwrap();
    let fd = reader.as_raw_fd();
    let mut epoll = Epoll::new();

    assert_eq!(epoll.Add(fd, PollMode::READ), 0_i32);
    writer.write_all(b"x").unwrap();

    let mut observed = Vec::new();
    let mut attempts = 0_i32;
    while observed.is_empty() && attempts < 50_i32 {
        epoll.Wait(|ready_fd, readiness| observed.push((ready_fd, readiness)));
        attempts += 1_i32;
    }
    assert!(observed.iter().any(
        |(ready_fd, readiness)| *ready_fd == fd && (*readiness & PollReady::READABLE) != 0_i32
    ));

    assert_eq!(epoll.Update(fd, PollMode::WRITE, PollMode::READ), 0_i32);
    epoll_remove_count.store(0_i32, Ordering::SeqCst);
    assert_eq!(epoll.Remove(fd), 0_i32);
    assert_eq!(epoll_remove_count.load(Ordering::SeqCst), 1_i32);
}

#[test]
fn add_recovers_eexist_and_reports_ebadf() {
    let (reader, _writer) = UnixStream::pair().unwrap();
    let fd = reader.as_raw_fd();
    let mut epoll = Epoll::new();

    assert_eq!(epoll.Add(fd, PollMode::READ), 0_i32);
    assert_eq!(epoll.Add(fd, PollMode::WRITE), 0_i32);
    assert_eq!(epoll.Add(i32::MAX, PollMode::READ), -1_i32);
}

#[test]
fn stale_update_and_remove_are_success_and_negative_wait_dispatches_nothing() {
    let mut epoll = Epoll::new();
    assert_eq!(
        epoll.Update(i32::MAX, PollMode::READ, PollMode::WRITE),
        0_i32
    );

    epoll_remove_count.store(0_i32, Ordering::SeqCst);
    assert_eq!(epoll.Remove(i32::MAX), 0_i32);
    assert_eq!(epoll_remove_count.load(Ordering::SeqCst), 1_i32);

    let mut callbacks = 0_i32;
    epoll_wait_impl(-1_i32, |_, _| callbacks += 1_i32);
    assert_eq!(callbacks, 0_i32);
}
