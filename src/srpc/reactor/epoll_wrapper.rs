//! Canonical epoll registration, error policy, and readiness dispatch.
//! Rust and generated C++ use the same syscall adapters in srpc_epoll.c.

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

// Native epoll_event layout differs between supported architectures. The C
// boundary normalizes the kernel record; scheduling and error handling stay
// in these canonical Rust functions.
#[repr(C)]
#[derive(Default)]
struct EpollWaitEvent {
    events: u32,
    fd: i32,
}

#[allow(unsafe_code)]
unsafe extern "C" {
    fn srpc_epoll_open() -> i32;
    fn srpc_epoll_ctl(poll_fd: i32, operation: i32, fd: i32, flags: u32) -> i32;
    fn srpc_epoll_wait(poll_fd: i32, events: *mut core::ffi::c_void, capacity: i32, timeout_ms: i32) -> i32;
}

#[allow(unsafe_code)]
pub fn epoll_open() -> i32 {
    let fd = unsafe { srpc_epoll_open() };
    assert!(fd >= 0);
    fd
}

#[allow(unsafe_code)]
pub fn epoll_add_impl(poll_fd: i32, fd: i32, poll_mode: i32) -> i32 {
    let mut flags = 0x8000_0000_u32 | LINUX_EPOLLIN | LINUX_EPOLLRDHUP;
    if (poll_mode & PollMode::WRITE) != 0 {
        flags |= LINUX_EPOLLOUT;
    }
    let mut result = unsafe { srpc_epoll_ctl(poll_fd, 1_i32, fd, flags) };
    if result == -17_i32 {
        unsafe { srpc_epoll_ctl(poll_fd, 2_i32, fd, 0_u32); }
        result = unsafe { srpc_epoll_ctl(poll_fd, 1_i32, fd, flags) };
    }
    if result == -9_i32 {
        return -1_i32;
    }
    assert!(result == 0);
    0_i32
}

#[allow(unsafe_code)]
pub fn epoll_remove_impl(poll_fd: i32, fd: i32) -> i32 {
    epoll_bump_remove_count();
    unsafe { srpc_epoll_ctl(poll_fd, 2_i32, fd, 0_u32); }
    0_i32
}

#[allow(unsafe_code)]
pub fn epoll_update_impl(poll_fd: i32, fd: i32, new_mode: i32, _old_mode: i32) -> i32 {
    let mut flags = 0x8000_0000_u32 | LINUX_EPOLLRDHUP;
    if (new_mode & PollMode::READ) != 0 {
        flags |= LINUX_EPOLLIN;
    }
    if (new_mode & PollMode::WRITE) != 0 {
        flags |= LINUX_EPOLLOUT;
    }
    let result = unsafe { srpc_epoll_ctl(poll_fd, 3_i32, fd, flags) };
    if result == -2_i32 || result == -9_i32 {
        return 0_i32;
    }
    assert!(result == 0);
    0_i32
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
        unsafe { srpc_epoll_wait(poll_fd, events.as_mut_ptr() as *mut core::ffi::c_void, 100_i32, 1_i32) };
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
type LegacyOwnedFd = std::os::fd::OwnedFd;

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
            poll_fd_: unsafe { LegacyOwnedFd::from_raw_fd(epoll_open()) },
        }
    }

    pub fn fd(&self) -> i32 {
        self.poll_fd_.as_raw_fd()
    }

    #[allow(unsafe_code)]
    pub fn Add(&mut self, fd: i32, poll_mode: i32) -> i32 {
        epoll_add_impl(self.poll_fd_.as_raw_fd(), fd, poll_mode)
    }

    #[allow(unsafe_code)]
    pub fn Remove(&mut self, fd: i32) -> i32 {
        epoll_remove_impl(self.poll_fd_.as_raw_fd(), fd)
    }

    #[allow(unsafe_code)]
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
