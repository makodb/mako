//! Linux implementation unit for [`super::legacy_epoll`].
//!
//! The four public functions back the interface owner's native Rust forwarding
//! bodies. Rusty-cpp emits ordinary module-attached C++ definitions and the
//! schema-v6 generator removes `export` from this implementation unit.

#![allow(dead_code, unsafe_code)]

use cpp::rrr::debugging as cpp_debugging;

const LINUX_CTL_EPOLLET: u32 = 0x8000_0000_u32;
const LINUX_CTL_EPOLLIN: u32 = 0x001_u32;
const LINUX_CTL_EPOLLOUT: u32 = 0x004_u32;
const LINUX_CTL_EPOLLRDHUP: u32 = 0x2000_u32;

const LINUX_EPOLL_CTL_ADD: i32 = 1_i32;
const LINUX_EPOLL_CTL_DEL: i32 = 2_i32;
const LINUX_EPOLL_CTL_MOD: i32 = 3_i32;

const LINUX_ERRNO_EBADF: i32 = 9_i32;
const LINUX_ERRNO_EEXIST: i32 = 17_i32;
const LINUX_ERRNO_ENOENT: i32 = 2_i32;

/// Private 12-byte carrier for Linux/x86-64's packed `epoll_event`.
#[repr(C)]
struct EpollCtlEvent {
    events: u32,
    fd: i32,
    padding: u32,
}

impl EpollCtlEvent {
    fn zeroed() -> EpollCtlEvent {
        EpollCtlEvent {
            events: 0_u32,
            fd: 0_i32,
            padding: 0_u32,
        }
    }
}

mod epoll_ffi {
    use super::EpollCtlEvent;

    unsafe extern "C" {
        pub(super) fn epoll_create(size: i32) -> i32;
        pub(super) fn epoll_ctl(
            epoll_fd: i32,
            operation: i32,
            fd: i32,
            event: *mut EpollCtlEvent,
        ) -> i32;
    }
}

fn last_errno() -> i32 {
    std::io::Error::last_os_error()
        .raw_os_error()
        .unwrap_or(0_i32)
}

pub fn epoll_add_impl(poll_fd: i32, fd: i32, poll_mode: i32) -> i32 {
    let mut event = EpollCtlEvent::zeroed();
    event.fd = fd;
    event.events = LINUX_CTL_EPOLLET | LINUX_CTL_EPOLLIN | LINUX_CTL_EPOLLRDHUP;
    if (poll_mode & crate::runtime::legacy_epoll::PollMode::WRITE) != 0_i32 {
        event.events |= LINUX_CTL_EPOLLOUT;
    }

    let mut result =
        unsafe { epoll_ffi::epoll_ctl(poll_fd, LINUX_EPOLL_CTL_ADD, fd, &raw mut event) };
    if result != 0_i32 && last_errno() == LINUX_ERRNO_EEXIST {
        unsafe {
            epoll_ffi::epoll_ctl(poll_fd, LINUX_EPOLL_CTL_DEL, fd, &raw mut event);
        }
        result = unsafe { epoll_ffi::epoll_ctl(poll_fd, LINUX_EPOLL_CTL_ADD, fd, &raw mut event) };
    }
    if result != 0_i32 && last_errno() == LINUX_ERRNO_EBADF {
        return -1_i32;
    }
    unsafe { cpp_debugging::verify(result == 0_i32) };
    0_i32
}

pub fn epoll_remove_impl(poll_fd: i32, fd: i32) -> i32 {
    crate::runtime::legacy_epoll::epoll_bump_remove_count();
    let mut event = EpollCtlEvent::zeroed();
    unsafe {
        epoll_ffi::epoll_ctl(poll_fd, LINUX_EPOLL_CTL_DEL, fd, &raw mut event);
    }
    0_i32
}

pub fn epoll_update_impl(poll_fd: i32, fd: i32, new_mode: i32, _old_mode: i32) -> i32 {
    let mut event = EpollCtlEvent::zeroed();
    event.fd = fd;
    event.events = LINUX_CTL_EPOLLET | LINUX_CTL_EPOLLRDHUP;
    if (new_mode & crate::runtime::legacy_epoll::PollMode::READ) != 0_i32 {
        event.events |= LINUX_CTL_EPOLLIN;
    }
    if (new_mode & crate::runtime::legacy_epoll::PollMode::WRITE) != 0_i32 {
        event.events |= LINUX_CTL_EPOLLOUT;
    }

    let result = unsafe { epoll_ffi::epoll_ctl(poll_fd, LINUX_EPOLL_CTL_MOD, fd, &raw mut event) };
    if result != 0_i32 {
        let error = last_errno();
        if error == LINUX_ERRNO_ENOENT || error == LINUX_ERRNO_EBADF {
            return 0_i32;
        }
        unsafe { cpp_debugging::verify(result == 0_i32) };
    }
    0_i32
}

pub fn epoll_open() -> i32 {
    let fd: i32 = unsafe { epoll_ffi::epoll_create(10_i32) };
    unsafe { cpp_debugging::verify(fd != -1_i32) };
    fd
}

// Cargo-only definition of the indexed legacy debugging import.
mod cpp {
    pub mod rrr {
        pub mod debugging {
            pub unsafe fn verify(value: bool) {
                assert!(value);
            }
        }
    }
}
