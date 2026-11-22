//! Canonical epoll registration, error policy, and readiness dispatch.
//! Rust and generated C++ use the same syscall adapters in srpc_epoll.c.
//!
//! `SrpcEpollBackend` is the OS backend for the Lion runtime that every
//! `PollThread` runs (docs/dev/lion-runtime-plan.md, S2): it meets the
//! contract of `lion_reactor::os::OsBackend` method for method, so the trait
//! impls at the end of this module (S1) only forward.  `PollMode` and
//! `PollReady` are the interest and readiness masks of the `PollableBase`
//! registrations a `PollThread` drives (rpc/pollable_proxy.rs).  The fd-keyed
//! `Epoll` wrapper and the `Pollable` trait of the retired 1 ms epoll loop
//! were deleted in S7b.

use std::os::fd::{AsRawFd, FromRawFd};
use std::sync::Arc;
use std::time::Duration;

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

#[allow(unsafe_code)]
unsafe extern "C" {
    fn srpc_epoll_create() -> i32;
    fn srpc_epoll_ctl_token(poll_fd: i32, operation: i32, fd: i32, flags: u32, token: u64) -> i32;
    fn srpc_epoll_wait_tokens(poll_fd: i32, tokens: *mut u64, flags: *mut u32, capacity: i32, timeout_ms: i32) -> i32;
    fn srpc_epoll_eventfd_create() -> i32;
    fn srpc_epoll_eventfd_signal(event_fd: i32) -> i32;
    fn srpc_epoll_eventfd_drain(event_fd: i32) -> i32;
}

const LINUX_EPOLLIN: u32 = 0x001_u32;
const LINUX_EPOLLPRI: u32 = 0x002_u32;
const LINUX_EPOLLOUT: u32 = 0x004_u32;
const LINUX_EPOLLERR: u32 = 0x008_u32;
const LINUX_EPOLLHUP: u32 = 0x010_u32;
const LINUX_EPOLLRDHUP: u32 = 0x2000_u32;
const LINUX_EPOLLET: u32 = 0x8000_0000_u32;

// The checked type map restores the established C++ spelling
// `rusty::os::fd::OwnedFd`; direct Rust uses std's equivalent RAII owner.
type LegacyOwnedFd = std::os::fd::OwnedFd;

// ---------------------------------------------------------------------------
// The OS backend for the Lion runtime (plan item S2).
//
// Lion's reactor keeps a readiness flag per resource and direction and clears
// it only after an operation returned WouldBlock, so it needs edge-triggered
// reports: every registration carries EPOLLET. It registers each fd under a
// usize token and expects that token back, so the kernel stores the token in
// epoll_event.data (srpc_epoll_ctl_token) rather than the fd. The cross-thread
// interrupt is an eventfd registered under the reserved token 0, which the
// reactor never uses; wait consumes it and never reports it.
//
// The native leaves in srpc_epoll.c make one system call each and return
// -errno. Everything that is policy is here: the registration flags, the
// reserved token, the flag mapping, the timeout rounding, and what EINTR and
// EAGAIN mean.

// Errno values as raw numerics, so the generated module stays valid next to
// errno.h (see TCP_ERR_* in rpc/tcp_channel.rs).
const EPOLL_ERR_INTR: i32 = 4_i32;
const EPOLL_ERR_AGAIN: i32 = 11_i32;
const EPOLL_ERR_INVAL: i32 = 22_i32;

// The token the interrupt eventfd is registered under. Lion's contract: "The
// reactor never registers token 0 (a backend may reserve it for its
// interrupt)". Lion's own mio backend reserves the same token.
const EPOLL_INTERRUPT_TOKEN: u64 = 0_u64;

// The native batch holds at most 100 events per wait (srpc_epoll_wait_tokens).
const EPOLL_BATCH_CAPACITY: usize = 100_usize;

/// The directions a registration reports. Mirrors
/// `lion_reactor::types::Interest` field for field.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SrpcInterest {
    pub readable: bool,
    pub writable: bool,
}

/// One readiness report for a registered fd. Mirrors
/// `lion_reactor::os::OsEvent` field for field, with mio's meaning of each
/// flag (see `epoll_os_event`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SrpcOsEvent {
    /// The token the fd was registered under.
    pub token: usize,
    pub readable: bool,
    pub writable: bool,
    pub error: bool,
    pub read_closed: bool,
    pub write_closed: bool,
}

/// The epoll flags of a registration: always EPOLLET and EPOLLRDHUP, EPOLLIN
/// when the interest is readable, EPOLLOUT when it is writable. An interest
/// with neither direction is treated as readable, as Lion's contract says.
pub fn epoll_interest_flags(interest: SrpcInterest) -> u32 {
    let mut flags = LINUX_EPOLLET | LINUX_EPOLLRDHUP;
    if interest.readable || !interest.writable {
        flags |= LINUX_EPOLLIN;
    }
    if interest.writable {
        flags |= LINUX_EPOLLOUT;
    }
    flags
}

/// Maps one kernel event word to readiness exactly as mio does on Linux
/// (mio 1.x, sys/unix/selector/epoll.rs): readable is IN or PRI; writable is
/// OUT; error is ERR; read_closed is HUP, or IN together with RDHUP;
/// write_closed is HUP, or OUT together with ERR, or ERR alone (the word is
/// exactly EPOLLERR: a pipe whose reader went away while the writer waits for
/// space).
pub fn epoll_os_event(token: usize, kernel_events: u32) -> SrpcOsEvent {
    let input = (kernel_events & LINUX_EPOLLIN) != 0_u32;
    let output = (kernel_events & LINUX_EPOLLOUT) != 0_u32;
    let error = (kernel_events & LINUX_EPOLLERR) != 0_u32;
    let hang_up = (kernel_events & LINUX_EPOLLHUP) != 0_u32;
    let priority = (kernel_events & LINUX_EPOLLPRI) != 0_u32;
    let read_hang_up = (kernel_events & LINUX_EPOLLRDHUP) != 0_u32;
    SrpcOsEvent {
        token,
        readable: input || priority,
        writable: output,
        error,
        read_closed: hang_up || (input && read_hang_up),
        write_closed: hang_up || (output && error) || kernel_events == LINUX_EPOLLERR,
    }
}

/// The epoll_wait timeout for a Lion wait: -1 (block) for `None`, otherwise
/// whole milliseconds rounded up, clamped to `i32::MAX`. Rounding up is mio's
/// rule: truncating would turn a 0.5 ms timeout into a zero one and make a
/// short timer busy-spin; only `Some(Duration::ZERO)` does not block.
pub fn epoll_timeout_ms(timeout: Option<Duration>) -> i32 {
    match timeout {
        None => -1_i32,
        Some(duration) => {
            let seconds = duration.as_secs();
            // Past this many seconds the result exceeds i32::MAX anyway, and
            // the multiplication below could overflow u64.
            if seconds >= 2_147_484_u64 {
                return i32::MAX;
            }
            let nanos = duration.subsec_nanos() as u64;
            let whole_millis = nanos / 1_000_000_u64;
            let mut millis = seconds * 1_000_u64 + whole_millis;
            if whole_millis * 1_000_000_u64 < nanos {
                millis += 1_u64;
            }
            if millis > i32::MAX as u64 {
                i32::MAX
            } else {
                millis as i32
            }
        }
    }
}

fn epoll_result(result: i32) -> std::io::Result<()> {
    if result == 0_i32 {
        Ok(())
    } else {
        Err(std::io::Error::from_raw_os_error(-result))
    }
}

/// The cross-thread half of the backend: makes the owner's current or next
/// wait return promptly. Mirrors `lion_reactor::os::OsInterrupt`.
pub struct SrpcEpollInterrupt {
    event_fd_: LegacyOwnedFd,
}

impl SrpcEpollInterrupt {
    /// Adds one to the eventfd counter. Callable from any thread at any time.
    /// EINTR is retried and EAGAIN is success: the counter only refuses a
    /// write when it is about to saturate, and then it is already nonzero, so
    /// the eventfd is readable and a wait will see it. Lion's waker panics on
    /// an error return, so neither condition may surface.
    #[allow(unsafe_code)]
    pub fn signal(&self) -> std::io::Result<()> {
        loop {
            let result = unsafe { srpc_epoll_eventfd_signal(self.event_fd_.as_raw_fd()) };
            if result == 0_i32 || result == -EPOLL_ERR_AGAIN {
                return Ok(());
            }
            if result != -EPOLL_ERR_INTR {
                return Err(std::io::Error::from_raw_os_error(-result));
            }
        }
    }

    // Owner thread only, inside wait. One read of a non-semaphore eventfd
    // returns the whole counter and resets it to zero, so it drains every
    // signal made so far; EINTR is retried. A signal that lands after the read
    // leaves the counter nonzero, and because the eventfd is registered
    // edge-triggered its write also queued a fresh event, so the next wait
    // returns promptly: no signal is lost. EAGAIN (the counter was already
    // zero) and any other error end the drain without being reported: the
    // wait has already harvested io events, and an error return would make
    // the reactor treat the park as empty and drop those edges.
    #[allow(unsafe_code)]
    fn drain(&self) {
        loop {
            let result = unsafe { srpc_epoll_eventfd_drain(self.event_fd_.as_raw_fd()) };
            if result != -EPOLL_ERR_INTR {
                return;
            }
        }
    }
}

/// An edge-triggered epoll instance plus its eventfd interrupt: the OS backend
/// of the Lion runtime. Every method but `SrpcEpollInterrupt::signal` runs on
/// the thread that owns the reactor.
pub struct SrpcEpollBackend {
    poll_fd_: LegacyOwnedFd,
    interrupt_: Arc<SrpcEpollInterrupt>,
    // The Lion trait's wait batch, reused so a park allocates nothing. Only
    // `impl OsBackend` touches it; between waits it is empty.
    lion_batch_: Vec<SrpcOsEvent>,
}

impl SrpcEpollBackend {
    /// Creates the epoll instance and the interrupt eventfd (both close-on-
    /// exec) and registers the eventfd, edge-triggered, under token 0.
    // Spelled `Result<_, std::io::Error>` rather than `std::io::Result<_>`: the
    // same Rust type, but the emitter lowers the alias to rusty::io::Result,
    // whose error constructor default-constructs the value, and this struct
    // holds an Arc, which has no default. The plain Result lowers to the
    // tagged rusty::Result, which does not need one.
    #[allow(unsafe_code)]
    pub fn new() -> Result<SrpcEpollBackend, std::io::Error> {
        let poll_fd = unsafe { srpc_epoll_create() };
        if poll_fd < 0_i32 {
            return Err(std::io::Error::from_raw_os_error(-poll_fd));
        }
        // SAFETY: srpc_epoll_create returned a fresh descriptor this owner
        // closes exactly once.
        let poll_owner = unsafe { LegacyOwnedFd::from_raw_fd(poll_fd) };
        let event_fd = unsafe { srpc_epoll_eventfd_create() };
        if event_fd < 0_i32 {
            return Err(std::io::Error::from_raw_os_error(-event_fd));
        }
        // SAFETY: as above, for the eventfd.
        let event_owner = unsafe { LegacyOwnedFd::from_raw_fd(event_fd) };
        let registered = unsafe {
            srpc_epoll_ctl_token(poll_fd, 1_i32, event_fd, LINUX_EPOLLET | LINUX_EPOLLIN, EPOLL_INTERRUPT_TOKEN)
        };
        if registered != 0_i32 {
            return Err(std::io::Error::from_raw_os_error(-registered));
        }
        Ok(SrpcEpollBackend {
            poll_fd_: poll_owner,
            interrupt_: Arc::new(SrpcEpollInterrupt { event_fd_: event_owner }),
            lion_batch_: Vec::new(),
        })
    }

    /// The epoll descriptor, for diagnostics.
    pub fn fd(&self) -> i32 {
        self.poll_fd_.as_raw_fd()
    }

    /// Starts reporting `fd` under `token` (EPOLL_CTL_ADD). Token 0 is the
    /// interrupt's and is refused with EINVAL; an fd registered twice fails
    /// with EEXIST.
    pub fn register(&mut self, fd: i32, token: usize, interest: SrpcInterest) -> std::io::Result<()> {
        self.control(1_i32, fd, token, interest)
    }

    /// Replaces the token and interest of a registered fd (EPOLL_CTL_MOD).
    pub fn reregister(&mut self, fd: i32, token: usize, interest: SrpcInterest) -> std::io::Result<()> {
        self.control(3_i32, fd, token, interest)
    }

    #[allow(unsafe_code)]
    fn control(&mut self, operation: i32, fd: i32, token: usize, interest: SrpcInterest) -> std::io::Result<()> {
        if token as u64 == EPOLL_INTERRUPT_TOKEN {
            return Err(std::io::Error::from_raw_os_error(EPOLL_ERR_INVAL));
        }
        let result = unsafe {
            srpc_epoll_ctl_token(self.poll_fd_.as_raw_fd(), operation, fd, epoll_interest_flags(interest), token as u64)
        };
        epoll_result(result)
    }

    /// Stops all reports for `fd` (EPOLL_CTL_DEL), including events the kernel
    /// has queued but no wait has returned yet.
    #[allow(unsafe_code)]
    pub fn deregister(&mut self, fd: i32) -> std::io::Result<()> {
        let result = unsafe { srpc_epoll_ctl_token(self.poll_fd_.as_raw_fd(), 2_i32, fd, 0_u32, 0_u64) };
        epoll_result(result)
    }

    /// Blocks for at most `timeout` (`None`: no bound) until an event is ready
    /// or the interrupt is signalled, and appends the ready events to
    /// `events`. See `wait_timeout_ms`.
    pub fn wait(&mut self, events: &mut Vec<SrpcOsEvent>, timeout: Option<Duration>) -> std::io::Result<()> {
        self.wait_timeout_ms(events, epoll_timeout_ms(timeout))
    }

    /// `wait` with an epoll_wait timeout: -1 blocks, 0 does not.
    ///
    /// At most `events.capacity() - events.len()` events are appended, and at
    /// most 100 (or 100 when the vector has no spare capacity); the kernel
    /// keeps the rest queued for the next wait. The interrupt is consumed
    /// here and never reported. An event that carries none of the five flags
    /// is dropped: registrations always ask for EPOLLRDHUP, so a write-only
    /// registration can see a bare EPOLLRDHUP that mio, which asks for it only
    /// with readable interest, would never have reported. A wait cut short by
    /// a signal (EINTR) is an empty wait, not an error.
    #[allow(unsafe_code)]
    pub fn wait_timeout_ms(&mut self, events: &mut Vec<SrpcOsEvent>, timeout_ms: i32) -> std::io::Result<()> {
        let mut tokens: [u64; 100] = [0_u64; 100];
        let mut kernel_events: [u32; 100] = [0_u32; 100];
        let room = events.capacity() - events.len();
        let capacity: i32 = if room == 0_usize || room > EPOLL_BATCH_CAPACITY {
            EPOLL_BATCH_CAPACITY as i32
        } else {
            room as i32
        };
        let ready_count = unsafe {
            srpc_epoll_wait_tokens(
                self.poll_fd_.as_raw_fd(),
                tokens.as_mut_ptr(),
                kernel_events.as_mut_ptr(),
                capacity,
                timeout_ms,
            )
        };
        if ready_count == -EPOLL_ERR_INTR {
            return Ok(());
        }
        if ready_count < 0_i32 {
            return Err(std::io::Error::from_raw_os_error(-ready_count));
        }
        let mut interrupted = false;
        let mut index: usize = 0_usize;
        while index < ready_count as usize {
            if tokens[index] == EPOLL_INTERRUPT_TOKEN {
                interrupted = true;
            } else {
                let event = epoll_os_event(tokens[index] as usize, kernel_events[index]);
                if event.readable || event.writable || event.error || event.read_closed || event.write_closed {
                    events.push(event);
                }
            }
            index += 1_usize;
        }
        if interrupted {
            self.interrupt_.drain();
        }
        Ok(())
    }

    /// The backend's interrupt, shared with every thread that may need to
    /// wake the owner's wait.
    pub fn interrupt(&self) -> Arc<SrpcEpollInterrupt> {
        self.interrupt_.clone()
    }
}

// ---------------------------------------------------------------------------
// Lion's OS seam (plan item S1). These impls only forward: each converts
// Lion's type to its SRPC mirror field for field and calls the inherent method
// of the same name, which owns the policy above. Inherent methods win method
// resolution, so every call below names `SrpcEpollBackend::` or
// `SrpcEpollInterrupt::` to make the forwarding target explicit.

fn srpc_interest(interest: lion_reactor::Interest) -> SrpcInterest {
    SrpcInterest { readable: interest.readable, writable: interest.writable }
}

fn lion_os_event(event: &SrpcOsEvent) -> lion_reactor::os::OsEvent {
    lion_reactor::os::OsEvent {
        token: event.token,
        readable: event.readable,
        writable: event.writable,
        error: event.error,
        read_closed: event.read_closed,
        write_closed: event.write_closed,
    }
}

impl lion_reactor::os::OsBackend for SrpcEpollBackend {
    fn register(&mut self, fd: lion_reactor::os::RawFd, token: usize, interest: lion_reactor::Interest) -> std::io::Result<()> {
        SrpcEpollBackend::register(self, fd, token, srpc_interest(interest))
    }

    fn reregister(&mut self, fd: lion_reactor::os::RawFd, token: usize, interest: lion_reactor::Interest) -> std::io::Result<()> {
        SrpcEpollBackend::reregister(self, fd, token, srpc_interest(interest))
    }

    fn deregister(&mut self, fd: lion_reactor::os::RawFd) -> std::io::Result<()> {
        SrpcEpollBackend::deregister(self, fd)
    }

    // SrpcEpollBackend::wait takes as many events as its vector has room for,
    // at most 100 (100 when it has none), so the batch carries the room Lion's
    // vector has. Lion's reactor passes an empty vector with capacity 1024:
    // the batch is then the reused one, sized once for 100, and a park
    // allocates nothing. Only a caller with room for 1 to 99 events gets a
    // fresh batch, allocated for exactly that many. The mirror events are
    // converted field for field on the way out.
    fn wait(&mut self, events: &mut Vec<lion_reactor::os::OsEvent>, timeout: Option<Duration>) -> std::io::Result<()> {
        let room = events.capacity() - events.len();
        let mut batch = std::mem::take(&mut self.lion_batch_);
        if room != 0_usize && room < EPOLL_BATCH_CAPACITY {
            batch = Vec::with_capacity(room);
        } else if batch.capacity() < EPOLL_BATCH_CAPACITY {
            batch.reserve_exact(EPOLL_BATCH_CAPACITY);
        }
        let result = SrpcEpollBackend::wait(self, &mut batch, timeout);
        for event in batch.iter() {
            events.push(lion_os_event(event));
        }
        batch.clear();
        self.lion_batch_ = batch;
        result
    }

    fn interrupt(&self) -> Arc<dyn lion_reactor::os::OsInterrupt> {
        SrpcEpollBackend::interrupt(self)
    }
}

impl lion_reactor::os::OsInterrupt for SrpcEpollInterrupt {
    fn signal(&self) -> std::io::Result<()> {
        SrpcEpollInterrupt::signal(self)
    }
}
