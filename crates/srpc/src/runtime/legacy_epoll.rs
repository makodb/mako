//! Exact Rust owner for the legacy `rrr.epoll_wrapper` interface unit.
//!
//! This module deliberately stays separate from [`super::epoll`].  The latter
//! is the native Rust event-loop abstraction (tracked registrations, `Result`
//! errors, and configurable waits); this owner preserves the established C++
//! module surface byte-for-byte: integer constant namespaces, the abstract
//! `Pollable` base, the remove counter, the four platform declarations, and
//! the one-millisecond `Epoll::Wait` template.

#![allow(dead_code, non_snake_case, non_upper_case_globals, unsafe_code)]

use cpp::rusty::os::fd as cpp_fd;
use std::os::fd::{AsRawFd, FromRawFd};
use std::sync::atomic::{AtomicI32, Ordering};

/// C++ consumers historically use this as the `PollMode` namespace.
pub mod PollMode {
    pub const READ: i32 = 0x1_i32;
    pub const WRITE: i32 = 0x2_i32;
    pub const NO_CHANGE: i32 = -1_i32;
}

/// C++ consumers historically use this as the `PollReady` namespace.
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

/// Test instrumentation retained from the legacy module.
pub static epoll_remove_count: AtomicI32 = AtomicI32::new(0_i32);

pub fn epoll_bump_remove_count() {
    epoll_remove_count.fetch_add(1_i32, Ordering::SeqCst);
}

// These native Rust forwarding bodies keep Cargo tests executable.  The
// C++-only marker lowers each item to an exported declaration, whose matching
// definition lives in the Linux implementation unit of this named module.
#[cfg_attr(any(), cpp_declaration)]
pub fn epoll_open() -> i32 {
    crate::runtime::legacy_epoll_linux::epoll_open()
}

#[cfg_attr(any(), cpp_declaration)]
pub fn epoll_add_impl(poll_fd: i32, fd: i32, poll_mode: i32) -> i32 {
    crate::runtime::legacy_epoll_linux::epoll_add_impl(poll_fd, fd, poll_mode)
}

#[cfg_attr(any(), cpp_declaration)]
pub fn epoll_remove_impl(poll_fd: i32, fd: i32) -> i32 {
    crate::runtime::legacy_epoll_linux::epoll_remove_impl(poll_fd, fd)
}

#[cfg_attr(any(), cpp_declaration)]
pub fn epoll_update_impl(poll_fd: i32, fd: i32, new_mode: i32, old_mode: i32) -> i32 {
    crate::runtime::legacy_epoll_linux::epoll_update_impl(poll_fd, fd, new_mode, old_mode)
}

// `epoll_event` is packed on the supported Linux/x86-64 ABI.  Spelling its
// only consumed union member directly gives both Rust and generated C++ the
// exact 12-byte layout without making a libc type part of the public module.
#[repr(C)]
struct EpollWaitEvent {
    events: u32,
    fd: i32,
    padding: u32,
}

impl Default for EpollWaitEvent {
    fn default() -> EpollWaitEvent {
        EpollWaitEvent {
            events: 0_u32,
            fd: 0_i32,
            padding: 0_u32,
        }
    }
}

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
/// A negative `epoll_wait` result performs zero callbacks, matching the old
/// signed loop exactly.  The generic remains a real C++ function template.
pub fn epoll_wait_impl<F>(poll_fd: i32, mut on_ready: F)
where
    F: FnMut(i32, i32),
{
    let mut events: [EpollWaitEvent; 100] = std::array::from_fn(|_| EpollWaitEvent::default());
    let ready_count: i32 =
        unsafe { epoll_wait_ffi::epoll_wait(poll_fd, events.as_mut_ptr(), 100_i32, 1_i32) };
    let mut index: i32 = 0_i32;
    while index < ready_count {
        let event_index: usize = index as usize;
        let kernel_events: u32 = events[event_index].events;
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

type LegacyOwnedFd = cpp_fd::OwnedFd;

/// Move-only RAII owner of the platform poll descriptor.
#[repr(C)]
#[cfg_attr(any(), cpp_no_fieldwise_ctor)]
pub struct Epoll {
    pub poll_fd_: LegacyOwnedFd,
}

impl Epoll {
    /// Allocate the poll descriptor eagerly, as the legacy default constructor
    /// does.
    #[cfg_attr(any(), cpp_ctor)]
    pub fn new() -> Epoll {
        Epoll {
            poll_fd_: unsafe { cpp_fd::OwnedFd::from_raw_fd(epoll_open()) },
        }
    }

    pub fn fd(&self) -> i32 {
        self.poll_fd_.as_raw_fd()
    }

    pub fn Add(&mut self, fd: i32, poll_mode: i32) -> i32 {
        epoll_add_impl(self.poll_fd_.as_raw_fd(), fd, poll_mode)
    }

    pub fn Remove(&mut self, fd: i32) -> i32 {
        epoll_remove_impl(self.poll_fd_.as_raw_fd(), fd)
    }

    pub fn Update(&mut self, fd: i32, new_mode: i32, old_mode: i32) -> i32 {
        epoll_update_impl(self.poll_fd_.as_raw_fd(), fd, new_mode, old_mode)
    }

    pub fn Wait<F>(&mut self, on_ready: F)
    where
        F: FnMut(i32, i32),
    {
        epoll_wait_impl(self.poll_fd_.as_raw_fd(), on_ready);
    }
}

// Cargo-only definition of the reserved C++ runtime import.  The generated
// module suppresses this tree and resolves the two indexed symbols from the
// `rusty` module instead.
mod cpp {
    pub mod rusty {
        pub mod os {
            pub mod fd {
                pub type OwnedFd = std::os::fd::OwnedFd;
            }
        }
    }
}
