//! Canonical Rust owner for `rrr.pollable_proxy`.

use rusty::cpp_inherit;
use std::sync::Arc;

pub trait PollableBase: Send {
    fn fd(&self) -> i32;
    fn poll_mode(&self) -> i32;
    fn content_size(&mut self) -> usize;
    fn handle_read(&mut self) -> bool;
    fn handle_write(&mut self) -> i32;
    fn handle_error(&mut self);
    fn close(&mut self);
    fn check_pending_write_update(&self) -> bool;
    fn is_closed(&self) -> bool;
}

trait PollableSharedTarget: Send + Sync {
    fn fd(&self) -> i32;
    fn poll_mode(&self) -> i32;
    fn content_size(&self) -> usize;
    fn handle_read(&self) -> bool;
    fn handle_write(&self) -> i32;
    fn handle_error(&self);
    fn close(&self);
    fn check_pending_write_update(&self) -> bool;
    fn is_closed(&self) -> bool;
}

pub type PollableProxy = Box<dyn PollableBase>;

#[repr(C)]
pub struct PollableArcShim<T> {
    pub poll_: Arc<T>,
}

#[cpp_inherit]
impl<T: PollableSharedTarget> PollableBase for PollableArcShim<T> {
    fn fd(&self) -> i32 {
        self.poll_.fd()
    }

    fn poll_mode(&self) -> i32 {
        self.poll_.poll_mode()
    }

    fn content_size(&mut self) -> usize {
        self.poll_.content_size()
    }

    fn handle_read(&mut self) -> bool {
        self.poll_.handle_read()
    }

    fn handle_write(&mut self) -> i32 {
        self.poll_.handle_write()
    }

    fn handle_error(&mut self) {
        self.poll_.handle_error()
    }

    fn close(&mut self) {
        self.poll_.close()
    }

    fn check_pending_write_update(&self) -> bool {
        self.poll_.check_pending_write_update()
    }

    fn is_closed(&self) -> bool {
        self.poll_.is_closed()
    }
}

#[allow(private_bounds)]
pub fn make_pollable_proxy_from_typed_arc<T>(poll: Arc<T>) -> PollableProxy
where
    T: PollableSharedTarget + 'static,
{
    rusty::make_box::<PollableArcShim<T>>(PollableArcShim { poll_: poll })
}
