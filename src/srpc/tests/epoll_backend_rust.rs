// The Lion OS backend over srpc_epoll.c (reactor/epoll_wrapper.rs,
// `SrpcEpollBackend`), against the contract of `lion_reactor::os::OsBackend`.
//
// Every expectation below was recorded by running the same inputs through
// Lion's own reference backend, `lion_reactor::os::MioBackend` (Lion 3496113,
// mio 1.2.3), in a scratch harness that also ran SRPC's backend behind the
// forwarding `impl OsBackend` that S1 adds (reactor/epoll_wrapper.rs):
//
// * MIO_FLAG_TABLE is mio's `Event::is_*` accessors applied to every subset of
//   {IN, PRI, OUT, ERR, HUP, RDHUP} (mio's `Event` is `repr(transparent)` over
//   `libc::epoll_event`, so the harness ran mio's real accessor code).
// * MIO_TIMEOUTS is mio's `Selector::select` timeout rule
//   (`checked_add(999_999ns).unwrap_or(to).as_millis()`, clamped to
//   `c_int::MAX`; `None` is -1) over the same samples.
// * The scenario tests assert the transcript lines MioBackend produced on the
//   same kernel. One line differs, and its test says why.
#![allow(unsafe_code)]

use srpc::epoll_wrapper::{
    epoll_interest_flags, epoll_os_event, epoll_timeout_ms, SrpcEpollBackend, SrpcInterest,
    SrpcOsEvent,
};
use std::io::Write;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
use std::os::unix::net::UnixStream;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::mpsc;
use std::time::{Duration, Instant};

const R: SrpcInterest = SrpcInterest { readable: true, writable: false };
const W: SrpcInterest = SrpcInterest { readable: false, writable: true };
const RW: SrpcInterest = SrpcInterest { readable: true, writable: true };
const NEITHER: SrpcInterest = SrpcInterest { readable: false, writable: false };

// ---------------------------------------------------------------------------
// Pure mappings, exhaustively against mio.

// (kernel event word, [readable, writable, error, read_closed, write_closed]).
const MIO_FLAG_TABLE: [(u32, [bool; 5]); 64] = [
    (0x0000, [false, false, false, false, false]),
    (0x0001, [true, false, false, false, false]),
    (0x0002, [true, false, false, false, false]),
    (0x0003, [true, false, false, false, false]),
    (0x0004, [false, true, false, false, false]),
    (0x0005, [true, true, false, false, false]),
    (0x0006, [true, true, false, false, false]),
    (0x0007, [true, true, false, false, false]),
    (0x0008, [false, false, true, false, true]),
    (0x0009, [true, false, true, false, false]),
    (0x000a, [true, false, true, false, false]),
    (0x000b, [true, false, true, false, false]),
    (0x000c, [false, true, true, false, true]),
    (0x000d, [true, true, true, false, true]),
    (0x000e, [true, true, true, false, true]),
    (0x000f, [true, true, true, false, true]),
    (0x0010, [false, false, false, true, true]),
    (0x0011, [true, false, false, true, true]),
    (0x0012, [true, false, false, true, true]),
    (0x0013, [true, false, false, true, true]),
    (0x0014, [false, true, false, true, true]),
    (0x0015, [true, true, false, true, true]),
    (0x0016, [true, true, false, true, true]),
    (0x0017, [true, true, false, true, true]),
    (0x0018, [false, false, true, true, true]),
    (0x0019, [true, false, true, true, true]),
    (0x001a, [true, false, true, true, true]),
    (0x001b, [true, false, true, true, true]),
    (0x001c, [false, true, true, true, true]),
    (0x001d, [true, true, true, true, true]),
    (0x001e, [true, true, true, true, true]),
    (0x001f, [true, true, true, true, true]),
    (0x2000, [false, false, false, false, false]),
    (0x2001, [true, false, false, true, false]),
    (0x2002, [true, false, false, false, false]),
    (0x2003, [true, false, false, true, false]),
    (0x2004, [false, true, false, false, false]),
    (0x2005, [true, true, false, true, false]),
    (0x2006, [true, true, false, false, false]),
    (0x2007, [true, true, false, true, false]),
    (0x2008, [false, false, true, false, false]),
    (0x2009, [true, false, true, true, false]),
    (0x200a, [true, false, true, false, false]),
    (0x200b, [true, false, true, true, false]),
    (0x200c, [false, true, true, false, true]),
    (0x200d, [true, true, true, true, true]),
    (0x200e, [true, true, true, false, true]),
    (0x200f, [true, true, true, true, true]),
    (0x2010, [false, false, false, true, true]),
    (0x2011, [true, false, false, true, true]),
    (0x2012, [true, false, false, true, true]),
    (0x2013, [true, false, false, true, true]),
    (0x2014, [false, true, false, true, true]),
    (0x2015, [true, true, false, true, true]),
    (0x2016, [true, true, false, true, true]),
    (0x2017, [true, true, false, true, true]),
    (0x2018, [false, false, true, true, true]),
    (0x2019, [true, false, true, true, true]),
    (0x201a, [true, false, true, true, true]),
    (0x201b, [true, false, true, true, true]),
    (0x201c, [false, true, true, true, true]),
    (0x201d, [true, true, true, true, true]),
    (0x201e, [true, true, true, true, true]),
    (0x201f, [true, true, true, true, true]),
];

#[test]
fn flag_mapping_matches_mio_for_every_bit_combination() {
    for (word, [readable, writable, error, read_closed, write_closed]) in MIO_FLAG_TABLE {
        let expected = SrpcOsEvent { token: 42, readable, writable, error, read_closed, write_closed };
        assert_eq!(epoll_os_event(42, word), expected, "kernel word {word:#06x}");
    }
    // The words cover all 64 subsets of the six bits exactly once.
    let mut words: Vec<u32> = MIO_FLAG_TABLE.iter().map(|(word, _)| *word).collect();
    words.sort_unstable();
    words.dedup();
    assert_eq!(words.len(), 64);
    assert_eq!(words.iter().fold(0, |all, word| all | word), 0x201f);
}

#[test]
fn tokens_pass_through_the_mapping_verbatim() {
    for token in [1_usize, 7, 0xdead_beef, usize::MAX] {
        assert_eq!(epoll_os_event(token, 0x001).token, token);
    }
}

const MIO_TIMEOUTS: [(Option<Duration>, i32); 17] = [
    (None, -1),
    (Some(Duration::ZERO), 0),
    (Some(Duration::from_nanos(1)), 1),
    (Some(Duration::from_micros(500)), 1),
    (Some(Duration::from_nanos(999_999)), 1),
    (Some(Duration::from_millis(1)), 1),
    (Some(Duration::from_nanos(1_000_001)), 2),
    (Some(Duration::from_micros(1500)), 2),
    (Some(Duration::from_millis(250)), 250),
    (Some(Duration::new(1, 1)), 1001),
    (Some(Duration::from_millis(i32::MAX as u64)), i32::MAX),
    (Some(Duration::from_millis(i32::MAX as u64 + 1)), i32::MAX),
    (Some(Duration::new(2_147_483, 647_000_000)), i32::MAX),
    (Some(Duration::new(2_147_483, 647_000_001)), i32::MAX),
    (Some(Duration::new(2_147_484, 0)), i32::MAX),
    (Some(Duration::from_secs(u64::MAX / 2)), i32::MAX),
    (Some(Duration::MAX), i32::MAX),
];

#[test]
fn timeout_rounding_matches_mio() {
    for (timeout, expected) in MIO_TIMEOUTS {
        assert_eq!(epoll_timeout_ms(timeout), expected, "{timeout:?}");
    }
}

#[test]
fn registrations_are_edge_triggered_and_ask_for_rdhup() {
    const ET: u32 = 0x8000_0000;
    const RDHUP: u32 = 0x2000;
    assert_eq!(epoll_interest_flags(R), ET | RDHUP | 0x001);
    assert_eq!(epoll_interest_flags(W), ET | RDHUP | 0x004);
    assert_eq!(epoll_interest_flags(RW), ET | RDHUP | 0x001 | 0x004);
    // Lion: "an interest with neither is treated as readable".
    assert_eq!(epoll_interest_flags(NEITHER), ET | RDHUP | 0x001);
}

// ---------------------------------------------------------------------------
// Real-kernel scenarios. Each wait is recorded as
// "<label>: [<token>:<flags>, ...] <timing>", where timing is "zero" for a
// Some(0) wait, "none" for an unbounded one, "full" when a bounded wait lasted
// at least 80% of its timeout and "early" otherwise. Load only lengthens a
// wait, so "full" lines cannot flake; every "early" line is a 5 s wait on an
// event that is already pending, or on a signal sent 100 ms into it. (The
// mio run used 500 ms bounds for these; the class of each line is the same.)

fn flags(event: &SrpcOsEvent) -> String {
    let names: Vec<&str> = [
        (event.readable, "R"),
        (event.writable, "W"),
        (event.error, "E"),
        (event.read_closed, "RC"),
        (event.write_closed, "WC"),
    ]
    .iter()
    .filter(|(on, _)| *on)
    .map(|(_, name)| *name)
    .collect();
    if names.is_empty() {
        "-".to_string()
    } else {
        names.join("|")
    }
}

struct Transcript {
    backend: SrpcEpollBackend,
    lines: Vec<String>,
}

impl Transcript {
    fn new() -> Self {
        Transcript { backend: SrpcEpollBackend::new().unwrap(), lines: Vec::new() }
    }

    fn wait(&mut self, label: &str, timeout: Option<Duration>) {
        let mut events = Vec::with_capacity(64);
        let start = Instant::now();
        let result = self.backend.wait(&mut events, timeout);
        let elapsed = start.elapsed();
        events.sort_by_key(|event| event.token);
        let reported: Vec<String> = events.iter().map(|e| format!("{}:{}", e.token, flags(e))).collect();
        let timing = match timeout {
            None => "none",
            Some(t) if t.is_zero() => "zero",
            Some(t) if elapsed >= t.mul_f64(0.8) => "full",
            Some(_) => "early",
        };
        let status = match result {
            Ok(()) => "ok".to_string(),
            Err(error) => format!("err({:?})", error.raw_os_error()),
        };
        self.lines.push(format!("{label}: {status} [{}] {timing}", reported.join(", ")));
    }

    fn check(&self, expected: &[&str]) {
        assert_eq!(self.lines, expected, "transcript differs from MioBackend's");
    }
}

fn ms(millis: u64) -> Option<Duration> {
    Some(Duration::from_millis(millis))
}

fn socket_pair() -> (UnixStream, UnixStream) {
    let (a, b) = UnixStream::pair().unwrap();
    a.set_nonblocking(true).unwrap();
    b.set_nonblocking(true).unwrap();
    (a, b)
}

extern "C" {
    fn pipe2(descriptors: *mut i32, flags: i32) -> i32;
    fn socket(domain: i32, kind: i32, protocol: i32) -> i32;
    fn connect(fd: i32, address: *const SockAddrIn, length: u32) -> i32;
    fn signal(signal: i32, handler: usize) -> usize;
    fn siginterrupt(signal: i32, interrupt: i32) -> i32;
    fn pthread_self() -> usize;
    fn pthread_kill(thread: usize, signal: i32) -> i32;
    fn gettid() -> i32;
}

const O_NONBLOCK: i32 = 0o4000;
const O_CLOEXEC: i32 = 0o2000000;

fn pipe_pair() -> (OwnedFd, OwnedFd) {
    let mut descriptors = [-1; 2];
    assert_eq!(unsafe { pipe2(descriptors.as_mut_ptr(), O_NONBLOCK | O_CLOEXEC) }, 0);
    // SAFETY: pipe2 created two uniquely owned descriptors.
    unsafe { (OwnedFd::from_raw_fd(descriptors[0]), OwnedFd::from_raw_fd(descriptors[1])) }
}

fn fill(mut writer: impl Write) {
    let chunk = [0u8; 65536];
    loop {
        match writer.write(&chunk) {
            Ok(_) => {}
            Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => return,
            Err(error) => panic!("fill: {error}"),
        }
    }
}

#[test]
fn edge_triggered_reports_each_arrival_once() {
    let mut t = Transcript::new();
    let (a, mut b) = socket_pair();
    t.backend.register(a.as_raw_fd(), 7, R).unwrap();
    t.wait("fresh socket, readable interest", Some(Duration::ZERO));
    b.write_all(b"x").unwrap();
    t.wait("peer wrote 1 byte", ms(5000));
    // The byte stays unread: a level-triggered backend would report it again.
    t.wait("unread byte, no new data", ms(50));
    b.write_all(b"y").unwrap();
    t.wait("peer wrote again, first byte still unread", ms(5000));
    t.check(&[
        "fresh socket, readable interest: ok [] zero",
        "peer wrote 1 byte: ok [7:R] early",
        "unread byte, no new data: ok [] full",
        "peer wrote again, first byte still unread: ok [7:R] early",
    ]);
}

#[test]
fn a_ready_fd_is_reported_once_at_registration() {
    let mut t = Transcript::new();
    let (a, _b) = socket_pair();
    t.backend.register(a.as_raw_fd(), 8, RW).unwrap();
    t.wait("fresh socket, rw interest", ms(5000));
    t.wait("nothing changed", ms(50));
    t.check(&["fresh socket, rw interest: ok [8:W] early", "nothing changed: ok [] full"]);
}

#[test]
fn peer_half_close_is_read_closed_without_hang_up() {
    let mut t = Transcript::new();
    let (a, b) = socket_pair();
    let (c, d) = socket_pair();
    t.backend.register(a.as_raw_fd(), 9, RW).unwrap();
    t.backend.register(c.as_raw_fd(), 10, R).unwrap();
    t.wait("rw and r registrations", ms(5000));
    b.shutdown(std::net::Shutdown::Write).unwrap();
    d.shutdown(std::net::Shutdown::Write).unwrap();
    t.wait("both peers shutdown(Write)", ms(5000));
    // IN|RDHUP (plus OUT for the rw registration): read_closed, not write_closed.
    t.check(&[
        "rw and r registrations: ok [9:W] early",
        "both peers shutdown(Write): ok [9:R|W|RC, 10:R|RC] early",
    ]);
}

#[test]
fn peer_close_is_read_and_write_closed() {
    let mut t = Transcript::new();
    let (a, b) = socket_pair();
    t.backend.register(a.as_raw_fd(), 11, RW).unwrap();
    t.wait("rw registration", ms(5000));
    drop(b);
    t.wait("peer closed", ms(5000));
    t.check(&["rw registration: ok [11:W] early", "peer closed: ok [11:R|W|RC|WC] early"]);
}

#[test]
fn pipe_reader_gone_while_full_is_error_only() {
    let mut t = Transcript::new();
    let (reader, writer) = pipe_pair();
    let writer = std::fs::File::from(writer);
    fill(&writer);
    t.backend.register(writer.as_raw_fd(), 12, W).unwrap();
    t.wait("full pipe, write interest", Some(Duration::ZERO));
    drop(reader);
    // The kernel word is exactly EPOLLERR: mio's "ERR alone" write_closed rule.
    t.wait("reader closed while pipe full", ms(5000));
    t.check(&[
        "full pipe, write interest: ok [] zero",
        "reader closed while pipe full: ok [12:E|WC] early",
    ]);
}

#[test]
fn pipe_reader_gone_with_space_is_writable_error() {
    let mut t = Transcript::new();
    let (reader, writer) = pipe_pair();
    t.backend.register(writer.as_raw_fd(), 13, W).unwrap();
    t.wait("empty pipe, write interest", ms(5000));
    drop(reader);
    t.wait("reader closed with space", ms(5000));
    t.check(&[
        "empty pipe, write interest: ok [13:W] early",
        "reader closed with space: ok [13:W|E|WC] early",
    ]);
}

#[test]
fn pipe_writer_gone_is_hang_up() {
    let mut t = Transcript::new();
    let (reader, writer) = pipe_pair();
    t.backend.register(reader.as_raw_fd(), 14, R).unwrap();
    t.wait("empty pipe, read interest", Some(Duration::ZERO));
    drop(writer);
    t.wait("writer closed", ms(5000));
    t.check(&["empty pipe, read interest: ok [] zero", "writer closed: ok [14:RC|WC] early"]);
}

#[test]
fn an_interest_with_neither_direction_is_readable() {
    let mut t = Transcript::new();
    let (a, mut b) = socket_pair();
    t.backend.register(a.as_raw_fd(), 15, NEITHER).unwrap();
    t.wait("neither interest, fresh", Some(Duration::ZERO));
    b.write_all(b"x").unwrap();
    t.wait("neither interest, peer wrote", ms(5000));
    t.check(&[
        "neither interest, fresh: ok [] zero",
        "neither interest, peer wrote: ok [15:R] early",
    ]);
}

#[test]
fn a_bare_rdhup_on_a_write_only_registration_is_not_reported() {
    // The one line where SRPC and MioBackend differ. SRPC registers every fd
    // with EPOLLRDHUP; mio asks for it only with readable interest (Lion itself
    // always registers readable). With a write-only interest and a full send
    // buffer, a peer half-close gives SRPC the kernel word EPOLLRDHUP alone,
    // which carries none of the five flags. The backend drops such an event,
    // so the reported events match mio's; the wait still returns early
    // (MioBackend's line is "... ok [] full"), which the contract allows.
    let mut t = Transcript::new();
    let (a, b) = socket_pair();
    fill(&a);
    t.backend.register(a.as_raw_fd(), 17, W).unwrap();
    t.wait("full socket, w registration", Some(Duration::ZERO));
    b.shutdown(std::net::Shutdown::Write).unwrap();
    let mut events = Vec::with_capacity(8);
    t.backend.wait(&mut events, ms(100)).unwrap();
    assert_eq!(events, [], "a flagless RDHUP-only event must not be reported");
    t.check(&["full socket, w registration: ok [] zero"]);
}

// A nonblocking TCP connect to a closed loopback port: the kernel word is
// ERR|HUP plus IN and OUT, so every flag is set.
#[repr(C)]
struct SockAddrIn {
    family: u16,
    port_be: u16,
    address: [u8; 4],
    zero: [u8; 8],
}

const AF_INET: i32 = 2;
const SOCK_STREAM: i32 = 1;
const SOCK_NONBLOCK: i32 = 0o4000;
const SOCK_CLOEXEC: i32 = 0o2000000;

#[test]
fn refused_connect_reports_every_flag() {
    let mut t = Transcript::new();
    let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    drop(listener);
    let fd = unsafe { socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0) };
    assert!(fd >= 0);
    // SAFETY: socket returned a fresh descriptor.
    let socket_owner = unsafe { OwnedFd::from_raw_fd(fd) };
    let address = SockAddrIn { family: AF_INET as u16, port_be: port.to_be(), address: [127, 0, 0, 1], zero: [0; 8] };
    let result = unsafe { connect(fd, &address, std::mem::size_of::<SockAddrIn>() as u32) };
    let errno = std::io::Error::last_os_error().raw_os_error();
    // EINPROGRESS, or an immediate ECONNREFUSED; either way the error is
    // pending on the socket when it is registered or shortly after.
    assert!(result == -1 && matches!(errno, Some(115) | Some(111)), "connect: {result} {errno:?}");
    t.backend.register(socket_owner.as_raw_fd(), 18, RW).unwrap();
    t.wait("refused connect", ms(5000));
    t.check(&["refused connect: ok [18:R|W|E|RC|WC] early"]);
}

#[test]
fn deregister_drops_pending_and_later_events() {
    let mut t = Transcript::new();
    let (a, mut b) = socket_pair();
    t.backend.register(a.as_raw_fd(), 20, R).unwrap();
    b.write_all(b"x").unwrap();
    t.backend.deregister(a.as_raw_fd()).unwrap();
    t.wait("deregistered before wait", ms(50));
    b.write_all(b"y").unwrap();
    t.wait("new data after deregister", ms(50));
    t.check(&["deregistered before wait: ok [] full", "new data after deregister: ok [] full"]);
}

#[test]
fn reregister_replaces_token_and_interest() {
    let mut t = Transcript::new();
    let (a, mut b) = socket_pair();
    t.backend.register(a.as_raw_fd(), 21, R).unwrap();
    t.wait("r registration", Some(Duration::ZERO));
    t.backend.reregister(a.as_raw_fd(), 22, RW).unwrap();
    t.wait("reregistered rw under 22", ms(5000));
    b.write_all(b"x").unwrap();
    t.wait("peer wrote", ms(5000));
    t.check(&[
        "r registration: ok [] zero",
        "reregistered rw under 22: ok [22:W] early",
        "peer wrote: ok [22:R|W] early",
    ]);
}

#[test]
fn tokens_round_trip_through_the_kernel() {
    let mut backend = SrpcEpollBackend::new().unwrap();
    let tokens = [1_usize, 0x1234_5678_9abc, usize::MAX];
    let mut pairs = Vec::new();
    for token in tokens {
        let (a, mut b) = socket_pair();
        backend.register(a.as_raw_fd(), token, R).unwrap();
        b.write_all(b"x").unwrap();
        pairs.push((a, b));
    }
    let mut events = Vec::with_capacity(8);
    backend.wait(&mut events, ms(5000)).unwrap();
    let mut seen: Vec<usize> = events.iter().map(|event| event.token).collect();
    seen.sort_unstable();
    assert_eq!(seen, tokens);
}

#[test]
fn registration_errors_are_the_kernel_errno() {
    let mut backend = SrpcEpollBackend::new().unwrap();
    let (a, _b) = socket_pair();
    let fd = a.as_raw_fd();
    let errno = |result: std::io::Result<()>| result.unwrap_err().raw_os_error();
    backend.register(fd, 23, R).unwrap();
    assert_eq!(errno(backend.register(fd, 24, R)), Some(17)); // EEXIST
    backend.deregister(fd).unwrap();
    assert_eq!(errno(backend.deregister(fd)), Some(2)); // ENOENT
    assert_eq!(errno(backend.reregister(fd, 25, R)), Some(2)); // ENOENT
    assert_eq!(errno(backend.register(-1, 26, R)), Some(9)); // EBADF
    // Token 0 belongs to the interrupt; the backend refuses it (EINVAL), and
    // the fd stays unregistered.
    assert_eq!(errno(backend.register(fd, 0, R)), Some(22));
    assert_eq!(errno(backend.reregister(fd, 0, R)), Some(22));
    assert_eq!(errno(backend.deregister(fd)), Some(2));
}

#[test]
fn wait_appends_at_most_the_spare_capacity() {
    let mut backend = SrpcEpollBackend::new().unwrap();
    let mut pairs = Vec::new();
    for token in 100..103 {
        let (a, mut b) = socket_pair();
        backend.register(a.as_raw_fd(), token, R).unwrap();
        b.write_all(b"x").unwrap();
        pairs.push((a, b));
    }
    // MioBackend ignores the capacity (it reports all 3 here); the contract
    // says a backend should not exceed it, and the kernel keeps the rest.
    let mut first = Vec::with_capacity(2);
    backend.wait(&mut first, ms(5000)).unwrap();
    assert_eq!((first.len(), first.capacity()), (2, 2));
    let mut second = Vec::with_capacity(16);
    backend.wait(&mut second, ms(5000)).unwrap();
    assert_eq!(second.len(), 1);
    let mut all: Vec<usize> = first.iter().chain(second.iter()).map(|event| event.token).collect();
    all.sort_unstable();
    assert_eq!(all, [100, 101, 102]);
    // Events already in the vector are kept, and a full vector still waits.
    let mut kept = vec![SrpcOsEvent { token: 9, ..SrpcOsEvent::default() }];
    kept.shrink_to_fit();
    backend.wait(&mut kept, Some(Duration::ZERO)).unwrap();
    assert_eq!(kept[0].token, 9);
}

// ---------------------------------------------------------------------------
// The interrupt.

#[test]
fn a_signal_before_the_wait_ends_it_and_is_consumed() {
    let mut t = Transcript::new();
    let interrupt = t.backend.interrupt();
    interrupt.signal().unwrap();
    t.wait("signalled before wait, None timeout", None);
    t.wait("after the signalled wait", ms(100));
    t.check(&[
        "signalled before wait, None timeout: ok [] none",
        "after the signalled wait: ok [] full",
    ]);
}

#[test]
fn signals_coalesce_into_one_early_return() {
    let mut t = Transcript::new();
    let interrupt = t.backend.interrupt();
    interrupt.signal().unwrap();
    interrupt.signal().unwrap();
    interrupt.signal().unwrap();
    t.wait("three signals, None timeout", None);
    t.wait("after the coalesced wait", ms(100));
    t.check(&[
        "three signals, None timeout: ok [] none",
        "after the coalesced wait: ok [] full",
    ]);
}

#[test]
fn a_signal_from_another_thread_wakes_a_blocked_wait() {
    let mut t = Transcript::new();
    let (a, _b) = socket_pair();
    t.backend.register(a.as_raw_fd(), 30, R).unwrap();
    let remote = t.backend.interrupt();
    let signaller = std::thread::spawn(move || {
        std::thread::sleep(Duration::from_millis(100));
        remote.signal().unwrap();
    });
    t.wait("signal from another thread during a blocked wait", ms(5000));
    signaller.join().unwrap();
    t.wait("after the cross-thread wait", ms(100));
    t.check(&[
        "signal from another thread during a blocked wait: ok [] early",
        "after the cross-thread wait: ok [] full",
    ]);
}

#[test]
fn many_threads_signalling_never_fail_and_always_wake() {
    let mut backend = SrpcEpollBackend::new().unwrap();
    let interrupt = backend.interrupt();
    for _round in 0..50 {
        let signallers: Vec<_> = (0..4)
            .map(|_| {
                let interrupt = interrupt.clone();
                std::thread::spawn(move || {
                    for _ in 0..100 {
                        interrupt.signal().unwrap();
                    }
                })
            })
            .collect();
        let mut events = Vec::with_capacity(4);
        let start = Instant::now();
        backend.wait(&mut events, ms(5000)).unwrap();
        assert!(start.elapsed() < Duration::from_secs(4), "a signal did not end the wait");
        assert_eq!(events, []);
        for signaller in signallers {
            signaller.join().unwrap();
        }
    }
}

const USER_SIGNAL: i32 = 10; // SIGUSR1 on x86_64 and aarch64
static HANDLED: AtomicUsize = AtomicUsize::new(0);

extern "C" fn count_signal(_: i32) {
    HANDLED.fetch_add(1, Ordering::SeqCst);
}

#[test]
fn a_wait_cut_short_by_a_signal_is_an_empty_wait() {
    // SAFETY: the handler only bumps an atomic; the previous disposition is
    // restored before the test returns.
    let previous = unsafe { signal(USER_SIGNAL, count_signal as *const () as usize) };
    assert_ne!(previous, usize::MAX);
    assert_eq!(unsafe { siginterrupt(USER_SIGNAL, 1) }, 0);
    HANDLED.store(0, Ordering::SeqCst);

    let (started_tx, started_rx) = mpsc::channel();
    let (done_tx, done_rx) = mpsc::channel();
    let waiter = std::thread::spawn(move || {
        let mut backend = SrpcEpollBackend::new().unwrap();
        started_tx
            .send((unsafe { gettid() }, unsafe { pthread_self() }, backend.fd(), backend.interrupt()))
            .unwrap();
        let mut events = Vec::with_capacity(8);
        let result = backend.wait(&mut events, None);
        done_tx.send((result.map_err(|error| error.raw_os_error()), events.len())).unwrap();
    });
    let (tid, thread, poll_fd, interrupt) = started_rx.recv_timeout(Duration::from_secs(3)).unwrap();

    // Wait until the thread is inside epoll_wait/epoll_pwait/epoll_pwait2 on
    // this backend's epoll fd, then interrupt it.
    let calls: &[i64] = if cfg!(target_arch = "aarch64") { &[22, 441] } else { &[232, 281, 441] };
    let deadline = Instant::now() + Duration::from_secs(3);
    loop {
        let state = std::fs::read_to_string(format!("/proc/self/task/{tid}/syscall")).unwrap();
        let mut fields = state.split_whitespace();
        let call: i64 = fields.next().unwrap_or("").parse().unwrap_or(-1);
        let first = fields.next().unwrap_or("");
        if calls.contains(&call) && first == format!("{poll_fd:#x}") {
            break;
        }
        assert!(Instant::now() < deadline, "waiter never blocked in epoll_wait: {state}");
        std::thread::sleep(Duration::from_millis(1));
    }
    assert_eq!(unsafe { pthread_kill(thread, USER_SIGNAL) }, 0);
    let outcome = done_rx.recv_timeout(Duration::from_secs(3));
    if outcome.is_err() {
        // The wait did not return on the signal: unblock it before failing.
        interrupt.signal().unwrap();
    }
    waiter.join().unwrap();
    unsafe { signal(USER_SIGNAL, previous) };
    assert_eq!(outcome.expect("a signal must end the wait"), (Ok(()), 0));
    assert!(HANDLED.load(Ordering::SeqCst) >= 1);
}

// ---------------------------------------------------------------------------
// Timeouts.

#[test]
fn a_zero_timeout_does_not_block() {
    let mut backend = SrpcEpollBackend::new().unwrap();
    let mut fastest = Duration::MAX;
    for _ in 0..100 {
        let mut events = Vec::with_capacity(4);
        let start = Instant::now();
        backend.wait(&mut events, Some(Duration::ZERO)).unwrap();
        fastest = fastest.min(start.elapsed());
        assert_eq!(events, []);
    }
    // A wait that blocked even 1 ms would make every sample >= 1 ms; load can
    // stretch some samples but not the fastest of 100.
    assert!(fastest < Duration::from_micros(500), "{fastest:?}");
}

#[test]
fn a_sub_millisecond_timeout_blocks_instead_of_spinning() {
    let mut backend = SrpcEpollBackend::new().unwrap();
    for _ in 0..20 {
        let mut events = Vec::with_capacity(4);
        let start = Instant::now();
        backend.wait(&mut events, Some(Duration::from_micros(500))).unwrap();
        // Rounded up to 1 ms, as mio does; truncation would return at once.
        assert!(start.elapsed() >= Duration::from_micros(900), "{:?}", start.elapsed());
    }
}
