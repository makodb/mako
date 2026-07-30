//! Edge-triggered epoll registration — the port of
//! `src/rrr/reactor/epoll_wrapper.cc`.
//!
//! Faithful to the C++ in the places where faithfulness is load-bearing:
//!
//! * **Edge-triggered throughout.** Every registration carries
//!   `EPOLLET`, so a handler must drain until `EAGAIN` or the edge is
//!   lost and the connection stalls.
//! * **ADD and MOD are asymmetric.** `ADD` sets `EPOLLIN`
//!   *unconditionally*, whatever the requested mode says; `MOD` sets it
//!   only when the mode asks for it. Making ADD conditional "for
//!   consistency" would silently stop arming reads for write-only
//!   registrations.
//! * **Mode changes are deduplicated.** [`Epoll::update_mode`] issues
//!   `epoll_ctl(MOD)` only when the mode actually differs. Under
//!   edge-triggering, that dedupe plus "return READ once the outbound
//!   queue drains" is jointly what re-arms the write edge.
//! * **Four errno tolerances are deliberate**, not defensive coding:
//!   they are the historical fixes for a family of CI flakes.
//!   - `ADD` → `EEXIST`: a stale registration for a recycled fd. Drop
//!     it and retry once.
//!   - `ADD` → `EBADF`: the fd closed underneath us; report it, do not
//!     abort.
//!   - `MOD`/`DEL` → `ENOENT`/`EBADF`: the fd is already gone, which is
//!     the desired end state, so treat it as success.
//!
//! Anything else is a programming error and surfaces as an `Err`.

use crate::sys;
use std::collections::HashMap;

/// What a pollable wants to be woken for. Values match the C++
/// `PollMode`, since they cross the same interfaces.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct PollMode(pub i32);

impl PollMode {
    pub const READ: PollMode = PollMode(0x1);
    pub const WRITE: PollMode = PollMode(0x2);
    pub const READ_WRITE: PollMode = PollMode(0x3);
    /// Sentinel meaning "leave the registration alone".
    pub const NO_CHANGE: PollMode = PollMode(-1);

    pub fn wants_read(self) -> bool {
        self.0 & PollMode::READ.0 != 0
    }

    pub fn wants_write(self) -> bool {
        self.0 & PollMode::WRITE.0 != 0
    }

    pub fn is_no_change(self) -> bool {
        self.0 == PollMode::NO_CHANGE.0
    }
}

/// Why a pollable became ready. Values match the C++ readiness bits.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Readiness(pub i32);

impl Readiness {
    pub const READABLE: i32 = 0x1;
    pub const WRITABLE: i32 = 0x2;
    pub const ERROR: i32 = 0x4;

    pub fn readable(self) -> bool {
        self.0 & Readiness::READABLE != 0
    }
    pub fn writable(self) -> bool {
        self.0 & Readiness::WRITABLE != 0
    }
    pub fn error(self) -> bool {
        self.0 & Readiness::ERROR != 0
    }
    pub fn is_empty(self) -> bool {
        self.0 == 0
    }

    /// Decode kernel event bits. `EPOLLRDHUP` counts as an error
    /// alongside `EPOLLERR`/`EPOLLHUP`, matching the C++.
    pub fn from_epoll(events: u32) -> Readiness {
        let mut r = 0;
        if events & sys::EPOLLIN != 0 {
            r |= Readiness::READABLE;
        }
        if events & sys::EPOLLOUT != 0 {
            r |= Readiness::WRITABLE;
        }
        if events & (sys::EPOLLERR | sys::EPOLLHUP | sys::EPOLLRDHUP) != 0 {
            r |= Readiness::ERROR;
        }
        Readiness(r)
    }
}

/// How many events one `epoll_wait` may return — the C++ array size.
const MAX_EVENTS: usize = 100;

/// `epoll_wait` timeout. There is no wakeup fd anywhere in rrr: the
/// command channel is polled with a non-blocking receive after each
/// wait, so this doubles as the command-delivery latency and costs a
/// wakeup every millisecond even when idle.
///
/// Kept for parity. An `eventfd` would improve both, which is exactly
/// why it must not be introduced silently — see
/// `docs/dev/srpc_rpcbench_baseline.md`.
pub const POLL_TIMEOUT_MS: i32 = 1;

#[derive(Debug)]
pub enum EpollError {
    Create(i32),
    Ctl(i32),
    Wait(i32),
}

pub struct Epoll {
    fd: i32,
    /// Last mode applied per fd, so `MOD` is issued only on a change.
    modes: HashMap<i32, i32>,
    events: Vec<sys::EpollEvent>,
}

impl Epoll {
    pub fn new() -> Result<Epoll, EpollError> {
        // 10, as the C++ passes. Linux has ignored the argument since
        // 2.6.8; it must merely be positive.
        let fd = sys::epoll_create_fd(10);
        if fd < 0 {
            return Err(EpollError::Create(-fd));
        }
        Ok(Epoll {
            fd,
            modes: HashMap::new(),
            events: vec![sys::EpollEvent::zeroed(); MAX_EVENTS],
        })
    }

    pub fn fd(&self) -> i32 {
        self.fd
    }

    /// Event mask for `ADD`: `EPOLLIN` unconditionally — see the
    /// module note.
    fn add_events(mode: PollMode) -> u32 {
        let mut e = sys::EPOLLET | sys::EPOLLIN | sys::EPOLLRDHUP;
        if mode.wants_write() {
            e |= sys::EPOLLOUT;
        }
        e
    }

    /// Event mask for `MOD`: both directions conditional.
    fn mod_events(mode: PollMode) -> u32 {
        let mut e = sys::EPOLLET | sys::EPOLLRDHUP;
        if mode.wants_read() {
            e |= sys::EPOLLIN;
        }
        if mode.wants_write() {
            e |= sys::EPOLLOUT;
        }
        e
    }

    /// Register `fd`.
    ///
    /// An `EEXIST` from a recycled fd is resolved by dropping the stale
    /// registration and retrying once.
    pub fn add(&mut self, fd: i32, mode: PollMode) -> Result<(), EpollError> {
        let events = Epoll::add_events(mode);
        let rc = sys::epoll_ctl_fd(self.fd, sys::EPOLL_CTL_ADD, fd, events);
        let rc = if rc == -sys::ERRNO_EEXIST {
            sys::epoll_ctl_fd(self.fd, sys::EPOLL_CTL_DEL, fd, 0);
            sys::epoll_ctl_fd(self.fd, sys::EPOLL_CTL_ADD, fd, events)
        } else {
            rc
        };
        if rc < 0 {
            // EBADF means the fd closed underneath us — report, do not
            // abort, and leave no stale mode behind.
            self.modes.remove(&fd);
            return Err(EpollError::Ctl(-rc));
        }
        self.modes.insert(fd, mode.0);
        Ok(())
    }

    /// Change `fd`'s interest set, if it actually changed.
    ///
    /// [`PollMode::NO_CHANGE`] and a repeat of the current mode are
    /// both no-ops. A vanished fd is success: gone is the goal.
    pub fn update_mode(&mut self, fd: i32, mode: PollMode) -> Result<(), EpollError> {
        if mode.is_no_change() {
            return Ok(());
        }
        if self.modes.get(&fd) == Some(&mode.0) {
            return Ok(());
        }
        let rc = sys::epoll_ctl_fd(self.fd, sys::EPOLL_CTL_MOD, fd, Epoll::mod_events(mode));
        if rc == -sys::ERRNO_ENOENT || rc == -sys::ERRNO_EBADF {
            self.modes.remove(&fd);
            return Ok(());
        }
        if rc < 0 {
            return Err(EpollError::Ctl(-rc));
        }
        self.modes.insert(fd, mode.0);
        Ok(())
    }

    /// Deregister `fd`. Already-gone is success.
    pub fn remove(&mut self, fd: i32) -> Result<(), EpollError> {
        self.modes.remove(&fd);
        let rc = sys::epoll_ctl_fd(self.fd, sys::EPOLL_CTL_DEL, fd, 0);
        if rc < 0 && rc != -sys::ERRNO_ENOENT && rc != -sys::ERRNO_EBADF {
            return Err(EpollError::Ctl(-rc));
        }
        Ok(())
    }

    pub fn registered(&self, fd: i32) -> Option<PollMode> {
        self.modes.get(&fd).copied().map(PollMode)
    }

    /// Wait once and hand each ready fd to `on_ready`.
    ///
    /// `EINTR` yields zero events rather than an error — a signal is
    /// not a failure. Events that decode to no readiness bits are not
    /// dispatched, matching the C++.
    pub fn wait(
        &mut self,
        timeout_ms: i32,
        mut on_ready: impl FnMut(i32, Readiness),
    ) -> Result<usize, EpollError> {
        let n = sys::epoll_wait_fd(self.fd, &mut self.events[..], timeout_ms);
        if n == -sys::ERRNO_EINTR {
            return Ok(0);
        }
        if n < 0 {
            return Err(EpollError::Wait(-n));
        }
        let n = n as usize;
        let mut i = 0;
        while i < n {
            let ev = self.events[i];
            let readiness = Readiness::from_epoll(ev.events());
            if !readiness.is_empty() {
                on_ready(ev.fd(), readiness);
            }
            i += 1;
        }
        Ok(n)
    }
}

impl Drop for Epoll {
    fn drop(&mut self) {
        if self.fd >= 0 {
            sys::close_fd(self.fd);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Read, Write};
    use std::net::{TcpListener, TcpStream};
    use std::os::fd::AsRawFd;

    #[test]
    fn poll_mode_bits_match_the_cpp_values() {
        assert_eq!(PollMode::READ.0, 1);
        assert_eq!(PollMode::WRITE.0, 2);
        assert_eq!(PollMode::READ_WRITE.0, 3);
        assert_eq!(PollMode::NO_CHANGE.0, -1);
        assert!(PollMode::READ_WRITE.wants_read() && PollMode::READ_WRITE.wants_write());
        assert!(!PollMode::READ.wants_write());
        assert!(PollMode::NO_CHANGE.is_no_change());
    }

    /// ADD arms EPOLLIN whatever the mode says; MOD does not.
    #[test]
    fn add_is_unconditional_where_mod_is_conditional() {
        let add_write_only = Epoll::add_events(PollMode::WRITE);
        assert!(
            add_write_only & sys::EPOLLIN != 0,
            "ADD must arm EPOLLIN even for a write-only mode"
        );
        assert!(add_write_only & sys::EPOLLOUT != 0);
        assert!(add_write_only & sys::EPOLLET != 0, "edge-triggered");

        let mod_write_only = Epoll::mod_events(PollMode::WRITE);
        assert!(
            mod_write_only & sys::EPOLLIN == 0,
            "MOD must NOT arm EPOLLIN unless asked"
        );
        assert!(mod_write_only & sys::EPOLLOUT != 0);

        let mod_read_only = Epoll::mod_events(PollMode::READ);
        assert!(mod_read_only & sys::EPOLLIN != 0);
        assert!(mod_read_only & sys::EPOLLOUT == 0);
    }

    #[test]
    fn readiness_decoding() {
        assert!(Readiness::from_epoll(sys::EPOLLIN).readable());
        assert!(Readiness::from_epoll(sys::EPOLLOUT).writable());
        for bit in [sys::EPOLLERR, sys::EPOLLHUP, sys::EPOLLRDHUP] {
            assert!(Readiness::from_epoll(bit).error(), "{bit:#x} is an error");
        }
        // A half-close delivers data AND error, and the data must still
        // be dispatched.
        let both = Readiness::from_epoll(sys::EPOLLIN | sys::EPOLLRDHUP);
        assert!(both.readable() && both.error());
        assert!(Readiness::from_epoll(0).is_empty());
    }

    #[test]
    fn registration_lifecycle_on_a_real_socket() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let client = TcpStream::connect(addr).unwrap();
        let (server, _) = listener.accept().unwrap();

        let mut ep = Epoll::new().unwrap();
        let fd = server.as_raw_fd();

        ep.add(fd, PollMode::READ).unwrap();
        assert_eq!(ep.registered(fd), Some(PollMode::READ));

        // Writing from the peer makes it readable.
        (&client).write_all(b"ping").unwrap();
        let mut hits = Vec::new();
        ep.wait(200, |f, r| hits.push((f, r))).unwrap();
        assert_eq!(hits.len(), 1, "expected one ready fd");
        assert_eq!(hits[0].0, fd);
        assert!(hits[0].1.readable());

        let mut buf = [0u8; 8];
        let n = (&server).read(&mut buf).unwrap();
        assert_eq!(&buf[..n], b"ping");

        ep.remove(fd).unwrap();
        assert_eq!(ep.registered(fd), None);
        // Removing twice is success, not an error.
        ep.remove(fd).unwrap();
    }

    #[test]
    fn update_mode_dedupes_and_tolerates_a_vanished_fd() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let client = TcpStream::connect(addr).unwrap();
        let (server, _) = listener.accept().unwrap();

        let mut ep = Epoll::new().unwrap();
        let fd = server.as_raw_fd();
        ep.add(fd, PollMode::READ).unwrap();

        // Same mode and NO_CHANGE are both no-ops.
        ep.update_mode(fd, PollMode::READ).unwrap();
        ep.update_mode(fd, PollMode::NO_CHANGE).unwrap();
        assert_eq!(ep.registered(fd), Some(PollMode::READ));

        // A real change is applied — this is the write-edge re-arm.
        ep.update_mode(fd, PollMode::READ_WRITE).unwrap();
        assert_eq!(ep.registered(fd), Some(PollMode::READ_WRITE));
        // …and an idle socket is immediately writable.
        let mut saw_writable = false;
        ep.wait(200, |_, r| saw_writable |= r.writable()).unwrap();
        assert!(saw_writable, "a connected socket should be writable");

        // An fd that vanished is not an error: gone is the goal.
        drop(server);
        drop(client);
        ep.update_mode(fd, PollMode::READ).unwrap();
        assert_eq!(ep.registered(fd), None, "stale mode is forgotten");
    }

    #[test]
    fn wait_with_nothing_registered_returns_no_events() {
        let mut ep = Epoll::new().unwrap();
        let mut count = 0;
        assert_eq!(ep.wait(0, |_, _| count += 1).unwrap(), 0);
        assert_eq!(count, 0);
    }

    #[test]
    fn add_after_a_stale_registration_recovers() {
        // Adding twice would be EEXIST; the wrapper drops the stale
        // registration and retries, which is how a recycled fd is
        // handled.
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap();
        let _client = TcpStream::connect(addr).unwrap();
        let (server, _) = listener.accept().unwrap();

        let mut ep = Epoll::new().unwrap();
        let fd = server.as_raw_fd();
        ep.add(fd, PollMode::READ).unwrap();
        ep.add(fd, PollMode::READ_WRITE)
            .expect("EEXIST is recovered");
        assert_eq!(ep.registered(fd), Some(PollMode::READ_WRITE));
    }
}
