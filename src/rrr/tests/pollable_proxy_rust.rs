use rrr::pollable_proxy::{PollableArcShim, PollableBase, PollableProxy};
use std::mem::{align_of, size_of};
use std::sync::Arc;

struct RecordingPollable {
    fd: i32,
    mode: i32,
    content_size_calls: usize,
    read_calls: usize,
    write_calls: usize,
    error_calls: usize,
    close_calls: usize,
    pending: bool,
    closed: bool,
}

impl RecordingPollable {
    fn new(fd: i32, mode: i32) -> Self {
        Self {
            fd,
            mode,
            content_size_calls: 0,
            read_calls: 0,
            write_calls: 0,
            error_calls: 0,
            close_calls: 0,
            pending: false,
            closed: false,
        }
    }
}

impl PollableBase for RecordingPollable {
    fn fd(&self) -> i32 {
        self.fd
    }

    fn poll_mode(&self) -> i32 {
        self.mode
    }

    fn content_size(&mut self) -> usize {
        self.content_size_calls += 1;
        64
    }

    fn handle_read(&mut self) -> bool {
        self.read_calls += 1;
        true
    }

    fn handle_write(&mut self) -> i32 {
        self.write_calls += 1;
        -1
    }

    fn handle_error(&mut self) {
        self.error_calls += 1;
    }

    fn close(&mut self) {
        self.close_calls += 1;
        self.closed = true;
    }

    fn check_pending_write_update(&self) -> bool {
        self.pending
    }

    fn is_closed(&self) -> bool {
        self.closed
    }
}

fn assert_send_sync<T: Send + Sync>() {}

#[test]
fn proxy_trait_forwards_the_complete_surface() {
    let mut proxy: PollableProxy = Box::new(RecordingPollable::new(42, 3));

    assert_eq!(proxy.fd(), 42);
    assert_eq!(proxy.poll_mode(), 3);
    assert_eq!(proxy.content_size(), 64);
    assert!(proxy.handle_read());
    assert_eq!(proxy.handle_write(), -1);
    proxy.handle_error();
    assert!(!proxy.check_pending_write_update());
    assert!(!proxy.is_closed());
    proxy.close();
    assert!(proxy.is_closed());
}

#[test]
fn shim_is_exactly_one_send_sync_arc() {
    assert_eq!(size_of::<PollableArcShim<u8>>(), size_of::<Arc<u8>>());
    assert_eq!(align_of::<PollableArcShim<u8>>(), align_of::<Arc<u8>>());
    assert_send_sync::<PollableArcShim<u8>>();
}
