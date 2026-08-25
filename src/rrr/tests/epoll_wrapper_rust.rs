use rrr::epoll_wrapper::{
    epoll_bump_remove_count, epoll_remove_count, PollMode, PollReady, Pollable,
};
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
