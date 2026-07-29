//! The crate's entire syscall surface.
//!
//! Every `unsafe` block in srpc lives here, so the rest of the crate is
//! ordinary safe Rust and the FFI boundary is one file to audit.
//!
//! ## Declaring libc by hand
//!
//! The crate takes no dependencies, so the functions it needs are
//! declared directly. Two rules make that survive translation to C++,
//! both learned by probing rather than by reasoning:
//!
//! 1. **Never declare a function the C++ standard library also
//!    declares.** A name first declared inside a module purview cannot
//!    be redeclared in the global module, and `import std;` does
//!    exactly that for `__errno_location`. [`last_errno`] therefore
//!    goes through `std::io::Error`. `epoll_*`, `socket`, `close` and
//!    friends are not in `std`, so declaring them is fine — including
//!    with our own [`EpollEvent`], since `<sys/epoll.h>` is never
//!    reachable.
//! 2. **Never name an item after a libc MACRO.** A macro does not
//!    shadow an identifier, it textually replaces it: a function named
//!    `errno` becomes `int (*__errno_location())()`, and the
//!    diagnostic names neither. Hence `last_errno`, and `CLOCK_*_ID`
//!    rather than `CLOCK_*`.

#![allow(unsafe_code)]

/// `struct epoll_event`. **Packed on x86_64** — the layout is ABI, not
/// a choice: an unpacked version would put `data` at offset 8 and every
/// registration would read the wrong field.
#[repr(C, packed)]
#[derive(Clone, Copy)]
pub struct EpollEvent {
    pub events: u32,
    pub data: u64,
}

impl EpollEvent {
    pub fn zeroed() -> EpollEvent {
        EpollEvent { events: 0, data: 0 }
    }

    /// The fd this event refers to.
    ///
    /// rrr stores the fd in `data` and ignores `data.ptr`, looking the
    /// pollable up by fd — so a stale event for a closed fd resolves to
    /// "no such pollable" rather than a dangling pointer.
    pub fn fd(&self) -> i32 {
        let data = self.data;
        data as i32
    }

    pub fn events(&self) -> u32 {
        self.events
    }
}

pub const EPOLLIN: u32 = 0x001;
pub const EPOLLOUT: u32 = 0x004;
pub const EPOLLERR: u32 = 0x008;
pub const EPOLLHUP: u32 = 0x010;
pub const EPOLLRDHUP: u32 = 0x2000;
pub const EPOLLET: u32 = 0x8000_0000;

pub const EPOLL_CTL_ADD: i32 = 1;
pub const EPOLL_CTL_DEL: i32 = 2;
pub const EPOLL_CTL_MOD: i32 = 3;

// errno values the transport's tolerances are written against.
//
// Prefixed because the BARE names are libc MACROS: `pub const EAGAIN`
// emitted verbatim becomes `constexpr int32_t 11 = 11;`. Same rule as
// `last_errno` above.
pub const ERRNO_EINTR: i32 = 4;
pub const ERRNO_EBADF: i32 = 9;
pub const ERRNO_EAGAIN: i32 = 11;
pub const ERRNO_EEXIST: i32 = 17;
pub const ERRNO_EINVAL: i32 = 22;
pub const ERRNO_ENOENT: i32 = 2;

extern "C" {
    fn epoll_create(size: i32) -> i32;
    fn epoll_ctl(epfd: i32, op: i32, fd: i32, event: *mut EpollEvent) -> i32;
    fn epoll_wait(epfd: i32, events: *mut EpollEvent, maxevents: i32, timeout: i32) -> i32;
    fn close(fd: i32) -> i32;
}

/// The current `errno`.
///
/// Via `std::io::Error` rather than a self-declared
/// `__errno_location` — see the module note.
pub fn last_errno() -> i32 {
    std::io::Error::last_os_error().raw_os_error().unwrap_or(0)
}

/// `epoll_create(size)`, returning the fd or `-errno`.
///
/// Deliberately `epoll_create` rather than `epoll_create1`, matching
/// the C++ exactly. `epoll_create1(EPOLL_CLOEXEC)` would be an
/// improvement — and one that must not be smuggled in during a port
/// whose whole point is comparability. The `size` argument has been
/// ignored by Linux since 2.6.8; rrr passes 10.
pub fn epoll_create_fd(size: i32) -> i32 {
    // @unsafe { libc }
    let fd = unsafe { epoll_create(size) };
    if fd < 0 {
        -last_errno()
    } else {
        fd
    }
}

/// `epoll_ctl`, returning 0 or `-errno`. `fd` is stored in
/// `event.data`.
pub fn epoll_ctl_fd(epfd: i32, op: i32, fd: i32, events: u32) -> i32 {
    let mut ev = EpollEvent {
        events,
        data: fd as u64,
    };
    // @unsafe { libc }
    let rc = unsafe { epoll_ctl(epfd, op, fd, &mut ev) };
    if rc < 0 {
        -last_errno()
    } else {
        0
    }
}

/// `epoll_wait`, returning the event count or `-errno`.
pub fn epoll_wait_fd(epfd: i32, out: &mut [EpollEvent], timeout_ms: i32) -> i32 {
    // @unsafe { libc — `out` supplies both pointer and capacity }
    let n = unsafe { epoll_wait(epfd, out.as_mut_ptr(), out.len() as i32, timeout_ms) };
    if n < 0 {
        -last_errno()
    } else {
        n
    }
}

/// `close`, returning 0 or `-errno`.
pub fn close_fd(fd: i32) -> i32 {
    // @unsafe { libc }
    let rc = unsafe { close(fd) };
    if rc < 0 {
        -last_errno()
    } else {
        0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn epoll_event_layout_is_the_kernel_abi() {
        // Packed: 4-byte events immediately followed by 8-byte data.
        // If this ever becomes 16, every registration reads the wrong
        // field and the failure is silent.
        assert_eq!(std::mem::size_of::<EpollEvent>(), 12);
        let ev = EpollEvent {
            events: EPOLLIN | EPOLLET,
            data: 42,
        };
        assert_eq!(ev.fd(), 42);
        assert_eq!(ev.events(), EPOLLIN | EPOLLET);
    }

    #[test]
    fn create_ctl_wait_close_round_trip() {
        let ep = epoll_create_fd(10);
        assert!(ep > 0, "epoll_create failed: {ep}");

        // Nothing registered: wait returns 0 events, not an error.
        let mut evs = [EpollEvent::zeroed(); 8];
        assert_eq!(epoll_wait_fd(ep, &mut evs, 0), 0);

        assert_eq!(close_fd(ep), 0);
    }

    #[test]
    fn errors_come_back_as_negative_errno() {
        // Registering a non-epoll fd, and operating on a closed one.
        let ep = epoll_create_fd(10);
        assert!(ep > 0);
        assert_eq!(close_fd(ep), 0);

        let rc = epoll_ctl_fd(ep, EPOLL_CTL_ADD, 0, EPOLLIN);
        assert_eq!(rc, -ERRNO_EBADF, "a closed epoll fd should report EBADF");

        assert_eq!(close_fd(ep), -ERRNO_EBADF, "a double close reports EBADF");
    }

    #[test]
    fn last_errno_reads_the_real_errno() {
        // Provoke a known failure and read it back.
        assert_eq!(close_fd(-1), -ERRNO_EBADF);
        assert_eq!(last_errno(), ERRNO_EBADF);
    }
}
