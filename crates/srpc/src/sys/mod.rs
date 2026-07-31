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

// ## OPEN Goal-2 DEFECT: the socket family collides with libc
//
// The compile gate (scripts/srpc_cpp_gate.sh) fails this module with
// `conflicting types for 'connect' / 'bind' / 'accept' / 'recv'`. Our
// self-declared `extern "C"` socket fns collide with libc's real ones,
// because `<sys/socket.h>` IS reachable in the emitted TU — the same
// fact that made `AF_INET` a macro and forced the `SYS_` prefixes
// below. Signatures differ (`SockAddrIn*` vs `struct sockaddr*`), so it
// is a hard error, not a benign redeclaration.
//
// This is rule 1 above, violated for the whole socket family. epoll and
// mmap are fine: those headers genuinely are not reachable.
//
// Three ways out, and the choice is not obvious:
//
//  1. MATCH LIBC EXACTLY. Cannot be expressed: any Rust struct we
//     define lowers to a namespaced C++ type, so `srpc::sys::sockaddr*`
//     still conflicts with `::sockaddr*`.
//  2. `#[link_name]` to keep a distinct Rust name over the libc symbol.
//     NOT VIABLE TODAY — the transpiler does not read the attribute
//     (`grep -rn link_name transpiler/src/` is empty), so the emitted
//     C++ would call a symbol that does not exist.
//  3. A HAND-WRITTEN KERNEL exporting distinct names (`srpc_connect`,
//     `srpc_bind`, …), compiled by both toolchains. This is exactly the
//     pattern the fiber context switch already uses — one shared source
//     of truth, reached through `extern "C"`, with a link error if it
//     is absent. Consistent, and it sidesteps the collision by not
//     naming libc's symbols at all.
//
// STATUS 2026-07-31: thunks IMPLEMENTED (srpc_sys_x86_64.S). The
// collision is gone — no more "conflicting types for 'connect'". The
// module now fails one step further on, with:
//
//     no member named 'srpc_mmap' in the global namespace;
//     did you mean simply 'srpc_mmap'?
//
// The `extern "C"` block is emitted INSIDE `namespace srpc::sys` while
// the call sites are qualified `::srpc_mmap`. Declaration and use
// disagree about scope, and that is a transpiler bug, not a port one:
// C linkage does not imply global SCOPE in C++, so a block emitted
// inside a namespace really is namespace-scoped, and the `::` on the
// call is then wrong. The natural fix is to emit `extern "C"` blocks at
// global scope, which is also how every C header declares them.
//
// (The s8seam probe compiled because its extern block landed at file
// scope; that is why this did not surface there.)
//
// ORIGINAL ANALYSIS, still accurate:
//
// RECOMMENDED: (3) — and specifically as ASSEMBLY THUNKS, not a C
// file. `srpc_sys_x86_64.S` holding one `jmp connect@PLT` per symbol:
//
//     .globl srpc_connect
//     srpc_connect:
//         jmp connect@PLT
//
// Rust assembles it with `global_asm!(include_str!(...))` and the C++
// build assembles the same file — byte for byte the fiber seam, already
// proven end to end in probes/s8seam (links with it, `undefined
// reference` without it).
//
// Why thunks rather than a C kernel: a `.c` file would need a build
// script to compile for the Rust side, and the crate takes NO
// dependencies (so no `cc` crate) — a hand-rolled build.rs shelling to
// a compiler is exactly the kind of moving part this port has avoided.
// `global_asm!` needs none of that. The thunks are also trivially
// reviewable: one instruction each, no signature to get wrong, and the
// types stay entirely on the Rust side where they are already correct.
//
// The general fix remains teaching the transpiler `#[link_name]`, which
// would make (2) work and help every future FFI surface. That is a
// transpiler feature rather than a port change, so it is recorded
// rather than assumed.

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
            family: SYS_AF_INET as u16,
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

// SYS_-prefixed because the bare names are libc MACROS: `AF_INET`
// emitted verbatim becomes `constexpr int32_t 2 = 2;`. Same rule as
// `ERRNO_*` above and `last_errno` — and violated when these were
// added, which broke sys and every module importing it. Only the eight
// that actually collide are prefixed; EPOLL* and MAP*/PROT* do not,
// because <sys/epoll.h> and <sys/mman.h> are never reachable here.
pub const SYS_AF_INET: i32 = 2;
pub const SOCK_STREAM: i32 = 1;

pub const SYS_SOL_SOCKET: i32 = 1;
pub const IPPROTO_TCP: i32 = 6;
pub const SYS_TCP_NODELAY: i32 = 1;
pub const SYS_SO_REUSEADDR: i32 = 2;
pub const SYS_SO_ERROR: i32 = 4;

pub const SYS_F_GETFL: i32 = 3;
pub const SYS_F_SETFL: i32 = 4;
pub const SYS_O_NONBLOCK: i32 = 0o4000;

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

// Assembled by both toolchains; see the file header for why the
// symbols are renamed rather than declared directly.
core::arch::global_asm!(include_str!("srpc_sys_x86_64.S"), options(att_syntax));

extern "C" {
    fn srpc_epoll_create(size: i32) -> i32;
    fn srpc_epoll_ctl(epfd: i32, op: i32, fd: i32, event: *mut EpollEvent) -> i32;
    fn srpc_epoll_wait(epfd: i32, events: *mut EpollEvent, maxevents: i32, timeout: i32) -> i32;
    fn srpc_close(fd: i32) -> i32;

    fn srpc_socket(domain: i32, ty: i32, protocol: i32) -> i32;
    fn srpc_connect(sockfd: i32, addr: *const SockAddrIn, addrlen: u32) -> i32;
    fn srpc_bind(sockfd: i32, addr: *const SockAddrIn, addrlen: u32) -> i32;
    fn srpc_listen(sockfd: i32, backlog: i32) -> i32;
    fn srpc_accept(sockfd: i32, addr: *mut SockAddrIn, addrlen: *mut u32) -> i32;
    fn srpc_shutdown(sockfd: i32, how: i32) -> i32;
    fn srpc_recv(sockfd: i32, buf: *mut u8, len: usize, flags: i32) -> isize;
    fn srpc_send(sockfd: i32, buf: *const u8, len: usize, flags: i32) -> isize;
    fn srpc_setsockopt(sockfd: i32, level: i32, name: i32, val: *const i32, len: u32) -> i32;
    fn srpc_getsockopt(sockfd: i32, level: i32, name: i32, val: *mut i32, len: *mut u32) -> i32;
    fn srpc_fcntl(fd: i32, cmd: i32, arg: i32) -> i32;
    fn srpc_poll(fds: *mut PollFd, nfds: u64, timeout: i32) -> i32;
}

pub const PROT_NONE: i32 = 0;
pub const PROT_READ: i32 = 1;
pub const PROT_WRITE: i32 = 2;
pub const MAP_PRIVATE: i32 = 0x02;
pub const MAP_ANONYMOUS: i32 = 0x20;
/// `mmap` reports failure as `(void*)-1`, not null.
pub const MAP_FAILED: usize = usize::MAX;

extern "C" {
    fn srpc_mmap(addr: *mut u8, len: usize, prot: i32, flags: i32, fd: i32, off: i64) -> *mut u8;
    fn srpc_mprotect(addr: *mut u8, len: usize, prot: i32) -> i32;
    fn srpc_munmap(addr: *mut u8, len: usize) -> i32;
}

/// Anonymous private mapping of `len` bytes, readable and writable.
/// Returns the base address, or `MAP_FAILED`.
pub fn map_anonymous(len: usize) -> usize {
    // @unsafe { libc }
    let p = unsafe {
        srpc_mmap(
            core::ptr::null_mut(),
            len,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS,
            -1,
            0,
        )
    };
    p as usize
}

/// Make `[addr, addr+len)` inaccessible — the guard page. A stack
/// overflow then faults on a known address instead of quietly
/// scribbling on whatever was mapped below it.
pub fn protect_none(addr: usize, len: usize) -> i32 {
    // @unsafe { libc }
    let rc = unsafe { srpc_mprotect(addr as *mut u8, len, PROT_NONE) };
    if rc < 0 {
        -last_errno()
    } else {
        0
    }
}

pub fn unmap(addr: usize, len: usize) -> i32 {
    // @unsafe { libc }
    let rc = unsafe { srpc_munmap(addr as *mut u8, len) };
    if rc < 0 {
        -last_errno()
    } else {
        0
    }
}

/// `socket(2)`, returning the fd or `-errno`.
pub fn socket_fd(domain: i32, ty: i32, protocol: i32) -> i32 {
    // @unsafe { libc }
    let fd = unsafe { srpc_socket(domain, ty, protocol) };
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
    let rc = unsafe { srpc_connect(fd, addr, core::mem::size_of::<SockAddrIn>() as u32) };
    if rc < 0 {
        -last_errno()
    } else {
        0
    }
}

/// `bind(2)`, returning 0 or `-errno`.
pub fn bind_fd(fd: i32, addr: &SockAddrIn) -> i32 {
    // @unsafe { libc }
    let rc = unsafe { srpc_bind(fd, addr, core::mem::size_of::<SockAddrIn>() as u32) };
    if rc < 0 {
        -last_errno()
    } else {
        0
    }
}

/// `listen(2)`, returning 0 or `-errno`.
pub fn listen_fd(fd: i32, backlog: i32) -> i32 {
    // @unsafe { libc }
    let rc = unsafe { srpc_listen(fd, backlog) };
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
    let rc = unsafe { srpc_accept(fd, core::ptr::null_mut(), core::ptr::null_mut()) };
    if rc < 0 {
        -last_errno()
    } else {
        rc
    }
}

/// `shutdown(2)`, returning 0 or `-errno`.
pub fn shutdown_fd(fd: i32, how: i32) -> i32 {
    // @unsafe { libc }
    let rc = unsafe { srpc_shutdown(fd, how) };
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
    let n = unsafe { srpc_recv(fd, buf.as_mut_ptr(), buf.len(), 0) };
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
    let n = unsafe { srpc_send(fd, buf.as_ptr(), buf.len(), MSG_NOSIGNAL) };
    if n < 0 {
        -(last_errno() as isize)
    } else {
        n
    }
}

/// `setsockopt(2)` for an `int`-valued option, returning 0 or `-errno`.
pub fn setsockopt_int(fd: i32, level: i32, name: i32, value: i32) -> i32 {
    // @unsafe { libc }
    let rc =
        unsafe { srpc_setsockopt(fd, level, name, &value, core::mem::size_of::<i32>() as u32) };
    if rc < 0 {
        -last_errno()
    } else {
        0
    }
}

/// `getsockopt(2)` for an `int`-valued option. Returns the value, or
/// `-errno` on failure — used for `SYS_SO_ERROR`, where the VALUE is itself
/// an errno, so a caller must check the syscall's own failure first.
pub fn getsockopt_int(fd: i32, level: i32, name: i32) -> Result<i32, i32> {
    let mut value: i32 = 0;
    let mut len: u32 = core::mem::size_of::<i32>() as u32;
    // @unsafe { libc }
    let rc = unsafe { srpc_getsockopt(fd, level, name, &mut value, &mut len) };
    if rc < 0 {
        Err(last_errno())
    } else {
        Ok(value)
    }
}

/// Put `fd` into non-blocking mode. 0 or `-errno`.
pub fn set_nonblocking(fd: i32) -> i32 {
    // @unsafe { libc }
    let flags = unsafe { srpc_fcntl(fd, SYS_F_GETFL, 0) };
    if flags < 0 {
        return -last_errno();
    }
    // @unsafe { libc }
    let rc = unsafe { srpc_fcntl(fd, SYS_F_SETFL, flags | SYS_O_NONBLOCK) };
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
    let rc = unsafe { srpc_poll(&mut pfd, 1, timeout_ms) };
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
    let fd = unsafe { srpc_epoll_create(size) };
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
    let rc = unsafe { srpc_epoll_ctl(epfd, op, fd, &mut ev) };
    if rc < 0 {
        -last_errno()
    } else {
        0
    }
}

/// `epoll_wait`, returning the event count or `-errno`.
pub fn epoll_wait_fd(epfd: i32, out: &mut [EpollEvent], timeout_ms: i32) -> i32 {
    // @unsafe { libc — `out` supplies both pointer and capacity }
    let n = unsafe { srpc_epoll_wait(epfd, out.as_mut_ptr(), out.len() as i32, timeout_ms) };
    if n < 0 {
        -last_errno()
    } else {
        n
    }
}

/// `close`, returning 0 or `-errno`.
pub fn close_fd(fd: i32) -> i32 {
    // @unsafe { libc }
    let rc = unsafe { srpc_close(fd) };
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
