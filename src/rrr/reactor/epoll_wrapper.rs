//! Canonical Rust owner for the `rrr.epoll_wrapper` C++ module.
//!
//! The historical `.cc` suffix is intentional: the generated `src/lib.rs`
//! names this file in a `#[path]` attribute, so Cargo reads these exact
//! bytes with nothing standing in between.  The Linux syscall
//! definitions remain in `reactor/epoll_platform_linux.cc`, an implementation
//! unit of the generated interface.

use cpp::rusty as cpp_rusty;
use rusty as cpp;
use std::os::fd::{AsRawFd, FromRawFd};
use std::sync::atomic::{AtomicI32, Ordering};

/// C++ consumers use this module as the `PollMode` namespace.
pub mod PollMode {
    pub const READ: i32 = 0x1_i32;
    pub const WRITE: i32 = 0x2_i32;
    pub const NO_CHANGE: i32 = -1_i32;
}

/// C++ consumers use this module as the `PollReady` namespace.
pub mod PollReady {
    pub const READABLE: i32 = 0x1_i32;
    pub const WRITABLE: i32 = 0x2_i32;
    pub const ERROR: i32 = 0x4_i32;
}

/// Abstract interface consumed by the reactor and transport modules.
pub trait Pollable {
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

/// Test instrumentation retained from the historical provider.
pub static epoll_remove_count: AtomicI32 = AtomicI32::new(0_i32);

pub fn epoll_bump_remove_count() {
    epoll_remove_count.fetch_add(1_i32, Ordering::SeqCst);
}

// The implementation unit supplies these four module-attached definitions.
// Rust's native ABI is deliberate: rusty-cpp emits ordinary C++ declarations,
// while direct rustc checking keeps calls explicit and unsafe.
#[allow(unsafe_code)]
unsafe extern "Rust" {
    pub fn epoll_open() -> i32;
    pub fn epoll_add_impl(poll_fd: i32, fd: i32, poll_mode: i32) -> i32;
    pub fn epoll_remove_impl(poll_fd: i32, fd: i32) -> i32;
    pub fn epoll_update_impl(poll_fd: i32, fd: i32, new_mode: i32, old_mode: i32) -> i32;
}

// Linux/x86-64's packed epoll_event has a 12-byte stride.  This private FFI
// carrier exposes only the union member the wait path consumes and keeps the
// platform header out of the public module surface.
#[repr(C)]
#[derive(Default)]
struct EpollWaitEvent {
    events: u32,
    fd: i32,
    padding: u32,
}

#[allow(unsafe_code)]
mod epoll_wait_ffi {
    use super::EpollWaitEvent;

    unsafe extern "C" {
        pub(super) fn epoll_wait(
            epoll_fd: i32,
            events: *mut EpollWaitEvent,
            max_events: i32,
            timeout_ms: i32,
        ) -> i32;
    }
}

const LINUX_EPOLLIN: u32 = 0x001_u32;
const LINUX_EPOLLOUT: u32 = 0x004_u32;
const LINUX_EPOLLERR: u32 = 0x008_u32;
const LINUX_EPOLLHUP: u32 = 0x010_u32;
const LINUX_EPOLLRDHUP: u32 = 0x2000_u32;

/// Run one one-millisecond poll pass and dispatch every meaningful event.
///
/// Keeping the loop counter signed preserves the legacy behavior on a failed
/// `epoll_wait`: a negative result performs zero callbacks.
#[allow(unsafe_code)]
pub fn epoll_wait_impl<F>(poll_fd: i32, mut on_ready: F)
where
    F: FnMut(i32, i32),
{
    let mut events: [EpollWaitEvent; 100] = std::array::from_fn(|_| EpollWaitEvent::default());
    let ready_count =
        unsafe { epoll_wait_ffi::epoll_wait(poll_fd, events.as_mut_ptr(), 100_i32, 1_i32) };
    let mut index: i32 = 0_i32;
    while index < ready_count {
        let event_index = index as usize;
        let kernel_events = events[event_index].events;
        let mut ready_events: i32 = 0_i32;
        if (kernel_events & LINUX_EPOLLIN) != 0_u32 {
            ready_events |= PollReady::READABLE;
        }
        if (kernel_events & LINUX_EPOLLOUT) != 0_u32 {
            ready_events |= PollReady::WRITABLE;
        }
        if (kernel_events & (LINUX_EPOLLERR | LINUX_EPOLLHUP | LINUX_EPOLLRDHUP)) != 0_u32 {
            ready_events |= PollReady::ERROR;
        }
        if ready_events != 0_i32 {
            on_ready(events[event_index].fd, ready_events);
        }
        index += 1_i32;
    }
}

// The checked type map restores the established C++ spelling
// `rusty::os::fd::OwnedFd`; direct Rust uses std's equivalent RAII owner.
type LegacyOwnedFd = cpp_rusty::os::fd::OwnedFd;

/// Move-only RAII owner of the platform poll descriptor.
#[repr(C)]
#[cfg_attr(any(), cpp_no_fieldwise_ctor)]
pub struct Epoll {
    pub poll_fd_: LegacyOwnedFd,
}

impl Epoll {
    /// Allocate the poll descriptor eagerly, as the historical default
    /// constructor did.
    #[allow(clippy::new_without_default, unsafe_code)]
    pub fn new() -> Epoll {
        Epoll {
            // SAFETY: epoll_open returns a fresh owned descriptor or aborts in
            // the platform implementation before returning an invalid value.
            poll_fd_: unsafe { cpp_rusty::os::fd::OwnedFd::from_raw_fd(epoll_open()) },
        }
    }

    pub fn fd(&self) -> i32 {
        self.poll_fd_.as_raw_fd()
    }

    #[allow(unsafe_code)]
    pub fn Add(&mut self, fd: i32, poll_mode: i32) -> i32 {
        unsafe { epoll_add_impl(self.poll_fd_.as_raw_fd(), fd, poll_mode) }
    }

    #[allow(unsafe_code)]
    pub fn Remove(&mut self, fd: i32) -> i32 {
        unsafe { epoll_remove_impl(self.poll_fd_.as_raw_fd(), fd) }
    }

    #[allow(unsafe_code)]
    pub fn Update(&mut self, fd: i32, new_mode: i32, old_mode: i32) -> i32 {
        unsafe { epoll_update_impl(self.poll_fd_.as_raw_fd(), fd, new_mode, old_mode) }
    }

    pub fn Wait<F>(&mut self, on_ready: F)
    where
        F: FnMut(i32, i32),
    {
        epoll_wait_impl(self.poll_fd_.as_raw_fd(), on_ready);
    }
}
