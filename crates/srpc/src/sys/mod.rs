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

/// `struct sockaddr_in`. Layout is ABI, and both `port` and `addr` are
/// NETWORK byte order — the two fields most easily got wrong, since a
/// host-order port silently connects somewhere else rather than failing.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SockAddrIn {
    pub family: u16,
    pub port: u16,
    pub addr: u32,
    pub zero: [u8; 8],
}

impl SockAddrIn {
    /// `addr` and `port` in HOST order; converted here.
    pub fn new(addr: u32, port: u16) -> SockAddrIn {
        SockAddrIn {
            family: AF_INET as u16,
            port: port.to_be(),
            addr: addr.to_be(),
            zero: [0; 8],
        }
    }
}

/// `struct pollfd`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PollFd {
    pub fd: i32,
    pub events: i16,
    pub revents: i16,
}

pub const AF_INET: i32 = 2;
pub const SOCK_STREAM: i32 = 1;

pub const SOL_SOCKET: i32 = 1;
pub const SO_REUSEADDR: i32 = 2;
pub const SO_ERROR: i32 = 4;

pub const F_GETFL: i32 = 3;
pub const F_SETFL: i32 = 4;
pub const O_NONBLOCK: i32 = 0o4000;

/// Suppress SIGPIPE on `send` to a closed peer. Linux-only; macOS uses
/// `setsockopt(SO_NOSIGPIPE)` at socket-creation time instead, which is
/// what the C++ does under `#ifdef __APPLE__`.
pub const MSG_NOSIGNAL: i32 = 0x4000;

pub const POLLOUT: i16 = 0x004;

pub const SHUT_RDWR: i32 = 2;

// errno values the transport's ladders are written against.
pub const ERRNO_EWOULDBLOCK: i32 = 11; // == EAGAIN on Linux
pub const ERRNO_EINPROGRESS: i32 = 115;
pub const ERRNO_EISCONN: i32 = 106;
pub const ERRNO_ECONNREFUSED: i32 = 111;
pub const ERRNO_ECONNRESET: i32 = 104;
pub const ERRNO_EPIPE: i32 = 32;
pub const ERRNO_ENOTCONN: i32 = 107;
pub const ERRNO_ETIMEDOUT: i32 = 110;
pub const ERRNO_EADDRINUSE: i32 = 98;
pub const ERRNO_EADDRNOTAVAIL: i32 = 99;
pub const ERRNO_EACCES: i32 = 13;
pub const ERRNO_EPERM: i32 = 1;
pub const ERRNO_EMFILE: i32 = 24;
pub const ERRNO_ENFILE: i32 = 23;

extern "C" {
    fn epoll_create(size: i32) -> i32;
    fn epoll_ctl(epfd: i32, op: i32, fd: i32, event: *mut EpollEvent) -> i32;
    fn epoll_wait(epfd: i32, events: *mut EpollEvent, maxevents: i32, timeout: i32) -> i32;
    fn close(fd: i32) -> i32;

    fn socket(domain: i32, ty: i32, protocol: i32) -> i32;
    fn connect(sockfd: i32, addr: *const SockAddrIn, addrlen: u32) -> i32;
    fn bind(sockfd: i32, addr: *const SockAddrIn, addrlen: u32) -> i32;
    fn listen(sockfd: i32, backlog: i32) -> i32;
    fn accept(sockfd: i32, addr: *mut SockAddrIn, addrlen: *mut u32) -> i32;
    fn shutdown(sockfd: i32, how: i32) -> i32;
    fn recv(sockfd: i32, buf: *mut u8, len: usize, flags: i32) -> isize;
    fn send(sockfd: i32, buf: *const u8, len: usize, flags: i32) -> isize;
    fn setsockopt(sockfd: i32, level: i32, name: i32, val: *const i32, len: u32) -> i32;
    fn getsockopt(sockfd: i32, level: i32, name: i32, val: *mut i32, len: *mut u32) -> i32;
    fn fcntl(fd: i32, cmd: i32, arg: i32) -> i32;
    fn poll(fds: *mut PollFd, nfds: u64, timeout: i32) -> i32;
}

/// `socket(2)`, returning the fd or `-errno`.
pub fn socket_fd(domain: i32, ty: i32, protocol: i32) -> i32 {
    // @unsafe { libc }
    let fd = unsafe { socket(domain, ty, protocol) };
    if fd < 0 {
        -last_errno()
    } else {
        fd
    }
}

/// `connect(2)`, returning 0 or `-errno`. `-EINPROGRESS` on a
/// non-blocking socket is the normal path, not a failure.
pub fn connect_fd(fd: i32, addr: &SockAddrIn) -> i32 {
    // @unsafe { libc }
    let rc = unsafe { connect(fd, addr, core::mem::size_of::<SockAddrIn>() as u32) };
    if rc < 0 {
        -last_errno()
    } else {
        0
    }
}

/// `bind(2)`, returning 0 or `-errno`.
pub fn bind_fd(fd: i32, addr: &SockAddrIn) -> i32 {
    // @unsafe { libc }
    let rc = unsafe { bind(fd, addr, core::mem::size_of::<SockAddrIn>() as u32) };
    if rc < 0 {
        -last_errno()
    } else {
        0
    }
}

/// `listen(2)`, returning 0 or `-errno`.
pub fn listen_fd(fd: i32, backlog: i32) -> i32 {
    // @unsafe { libc }
    let rc = unsafe { listen(fd, backlog) };
    if rc < 0 {
        -last_errno()
    } else {
        0
    }
}

/// `accept(2)`, returning the fd or `-errno`. The peer address is
/// discarded, matching the C++ (which passes null).
pub fn accept_fd(fd: i32) -> i32 {
    // @unsafe { libc }
    let rc = unsafe { accept(fd, core::ptr::null_mut(), core::ptr::null_mut()) };
    if rc < 0 {
        -last_errno()
    } else {
        rc
    }
}

/// `shutdown(2)`, returning 0 or `-errno`.
pub fn shutdown_fd(fd: i32, how: i32) -> i32 {
    // @unsafe { libc }
    let rc = unsafe { shutdown(fd, how) };
    if rc < 0 {
        -last_errno()
    } else {
        0
    }
}

/// `recv(2)`. Returns the byte count, 0 for orderly peer close, or
/// `-errno`. Zero is NOT an error and must stay distinguishable.
pub fn recv_fd(fd: i32, buf: &mut [u8]) -> isize {
    // @unsafe { libc — `buf` supplies both pointer and capacity }
    let n = unsafe { recv(fd, buf.as_mut_ptr(), buf.len(), 0) };
    if n < 0 {
        -(last_errno() as isize)
    } else {
        n
    }
}

/// `send(2)` with `MSG_NOSIGNAL`, returning the byte count or `-errno`.
///
/// The flag matters: without it, writing to a peer that has closed
/// raises SIGPIPE and kills the process rather than returning EPIPE.
pub fn send_fd(fd: i32, buf: &[u8]) -> isize {
    // @unsafe { libc }
    let n = unsafe { send(fd, buf.as_ptr(), buf.len(), MSG_NOSIGNAL) };
    if n < 0 {
        -(last_errno() as isize)
    } else {
        n
    }
}

/// `setsockopt(2)` for an `int`-valued option, returning 0 or `-errno`.
pub fn setsockopt_int(fd: i32, level: i32, name: i32, value: i32) -> i32 {
    // @unsafe { libc }
    let rc = unsafe { setsockopt(fd, level, name, &value, core::mem::size_of::<i32>() as u32) };
    if rc < 0 {
        -last_errno()
    } else {
        0
    }
}

/// `getsockopt(2)` for an `int`-valued option. Returns the value, or
/// `-errno` on failure — used for `SO_ERROR`, where the VALUE is itself
/// an errno, so a caller must check the syscall's own failure first.
pub fn getsockopt_int(fd: i32, level: i32, name: i32) -> Result<i32, i32> {
    let mut value: i32 = 0;
    let mut len: u32 = core::mem::size_of::<i32>() as u32;
    // @unsafe { libc }
    let rc = unsafe { getsockopt(fd, level, name, &mut value, &mut len) };
    if rc < 0 {
        Err(last_errno())
    } else {
        Ok(value)
    }
}

/// Put `fd` into non-blocking mode. 0 or `-errno`.
pub fn set_nonblocking(fd: i32) -> i32 {
    // @unsafe { libc }
    let flags = unsafe { fcntl(fd, F_GETFL, 0) };
    if flags < 0 {
        return -last_errno();
    }
    // @unsafe { libc }
    let rc = unsafe { fcntl(fd, F_SETFL, flags | O_NONBLOCK) };
    if rc < 0 {
        -last_errno()
    } else {
        0
    }
}

/// Wait up to `timeout_ms` for `fd` to become writable. Returns 1 when
/// it did, 0 on timeout, `-errno` on failure.
///
/// The C++ uses `select(2)` here. `poll(2)` has identical semantics for
/// a single descriptor and avoids modelling `fd_set`'s bitmask ABI —
/// which is also a latent bug on the C++ side, since `FD_SET` on a
/// descriptor at or above `FD_SETSIZE` (1024) writes past the end of
/// the `fd_set`. Nothing measurable rides on this: it runs once per
/// connect, never on the request path.
pub fn wait_writable(fd: i32, timeout_ms: i32) -> i32 {
    let mut pfd = PollFd {
        fd,
        events: POLLOUT,
        revents: 0,
    };
    // @unsafe { libc }
    let rc = unsafe { poll(&mut pfd, 1, timeout_ms) };
    if rc < 0 {
        -last_errno()
    } else {
        rc
    }
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
