// `impl lion_reactor::os::OsBackend for SrpcEpollBackend` and
// `impl lion_reactor::os::OsInterrupt for SrpcEpollInterrupt`
// (reactor/epoll_wrapper.rs), called through Lion's traits exactly as Lion's
// reactor calls them. The impls only forward to the inherent methods that
// tests/epoll_backend_rust.rs checks against mio; these tests pin what the
// forwarding itself adds: Lion's `Interest` and `OsEvent` converted field for
// field, the token passed through both ways, the caller's room honoured by the
// reused wait batch, and the interrupt reaching the eventfd.
//
// A runtime-level test cannot see a swapped readable/writable flag: an
// edge-triggered wait on a socket registered for both directions reports
// every ready bit at each edge, so the reactor still wakes the right task.
// These tests register one direction at a time.

use lion_reactor::os::{OsBackend, OsEvent};
use lion_reactor::Interest;
use srpc::epoll_wrapper::SrpcEpollBackend;
use std::io::Write;
use std::os::fd::AsRawFd;
use std::os::unix::net::UnixStream;
use std::time::{Duration, Instant};

fn backend() -> SrpcEpollBackend {
    SrpcEpollBackend::new().expect("SRPC epoll backend")
}

fn pair() -> (UnixStream, UnixStream) {
    let (a, b) = UnixStream::pair().expect("socketpair");
    a.set_nonblocking(true).unwrap();
    b.set_nonblocking(true).unwrap();
    (a, b)
}

// One Lion wait with a vector that has room for `room` events.
fn wait(backend: &mut SrpcEpollBackend, room: usize, timeout: Duration) -> Vec<OsEvent> {
    let mut events = Vec::with_capacity(room);
    OsBackend::wait(backend, &mut events, Some(timeout)).expect("wait");
    events
}

const READABLE: OsEvent =
    OsEvent { token: 0, readable: true, writable: false, error: false, read_closed: false, write_closed: false };
const WRITABLE: OsEvent =
    OsEvent { token: 0, readable: false, writable: true, error: false, read_closed: false, write_closed: false };

#[test]
fn trait_passes_interest_token_and_flags_field_for_field() {
    let mut backend = backend();
    let (a, mut b) = pair();

    // Read interest only: a fresh socket reports nothing until data arrives,
    // and then exactly a readable edge under the caller's token.
    OsBackend::register(&mut backend, a.as_raw_fd(), 7, Interest::READABLE).expect("register");
    assert_eq!(wait(&mut backend, 1024, Duration::ZERO), vec![]);
    b.write_all(b"x").unwrap();
    assert_eq!(wait(&mut backend, 1024, Duration::from_secs(1)), vec![OsEvent { token: 7, ..READABLE }]);

    // Write interest only: a fresh socket is writable at once.
    OsBackend::register(&mut backend, b.as_raw_fd(), 9, Interest::WRITABLE).expect("register");
    assert_eq!(wait(&mut backend, 1024, Duration::from_secs(1)), vec![OsEvent { token: 9, ..WRITABLE }]);

    // Reregister moves a's reports to a new token and direction.
    OsBackend::reregister(&mut backend, a.as_raw_fd(), 11, Interest::WRITABLE).expect("reregister");
    assert_eq!(wait(&mut backend, 1024, Duration::from_secs(1)), vec![OsEvent { token: 11, ..WRITABLE }]);

    // And back to read interest: a still holds the unread "x", so the
    // modification reports it at once.
    OsBackend::reregister(&mut backend, a.as_raw_fd(), 13, Interest::READABLE).expect("reregister");
    assert_eq!(wait(&mut backend, 1024, Duration::from_secs(1)), vec![OsEvent { token: 13, ..READABLE }]);

    // Deregistered, a reports nothing, even on a new read edge, and a second
    // deregistration fails with ENOENT.
    OsBackend::deregister(&mut backend, b.as_raw_fd()).expect("deregister");
    OsBackend::deregister(&mut backend, a.as_raw_fd()).expect("deregister");
    b.write_all(b"y").unwrap();
    assert_eq!(wait(&mut backend, 1024, Duration::ZERO), vec![]);
    let again = OsBackend::deregister(&mut backend, a.as_raw_fd());
    assert_eq!(again.unwrap_err().raw_os_error(), Some(2));

    // Token 0 is the interrupt's, through the trait as through the inherent API.
    let refused = OsBackend::register(&mut backend, a.as_raw_fd(), 0, Interest::READABLE);
    assert_eq!(refused.unwrap_err().raw_os_error(), Some(22));
}

#[test]
fn trait_reports_a_peer_hang_up() {
    let mut backend = backend();
    let (a, b) = pair();
    OsBackend::register(&mut backend, a.as_raw_fd(), 5, Interest::READABLE).expect("register");
    assert_eq!(wait(&mut backend, 1024, Duration::ZERO), vec![]);
    drop(b);
    let events = wait(&mut backend, 1024, Duration::from_secs(1));
    assert_eq!(events.len(), 1, "{events:?}");
    assert_eq!(events[0].token, 5);
    assert!(events[0].readable && events[0].read_closed, "{events:?}");
}

// Registers `count` socket pairs for reading under tokens 1..=count and makes
// every one readable.
fn readable_fds(backend: &mut SrpcEpollBackend, count: usize) -> Vec<(UnixStream, UnixStream)> {
    let mut pairs = Vec::new();
    for token in 1..=count {
        let (a, mut b) = pair();
        OsBackend::register(backend, a.as_raw_fd(), token, Interest::READABLE).expect("register");
        b.write_all(b"x").unwrap();
        pairs.push((a, b));
    }
    pairs
}

#[test]
fn trait_wait_takes_no_more_events_than_the_caller_has_room_for() {
    let mut backend = backend();
    let pairs = readable_fds(&mut backend, 3);
    let mut tokens = Vec::new();
    for room in [1, 1, 1] {
        let events = wait(&mut backend, room, Duration::from_secs(1));
        assert_eq!(events.len(), 1, "room {room}: {events:?}");
        tokens.push(events[0].token);
    }
    tokens.sort();
    assert_eq!(tokens, vec![1, 2, 3]);
    assert_eq!(wait(&mut backend, 1024, Duration::ZERO), vec![]);

    // Room for 2: two events, then the third.
    for (_, peer) in pairs.iter() {
        (&*peer).write_all(b"x").unwrap();
    }
    assert_eq!(wait(&mut backend, 2, Duration::from_secs(1)).len(), 2);
    assert_eq!(wait(&mut backend, 2, Duration::from_secs(1)).len(), 1);
    assert_eq!(wait(&mut backend, 1024, Duration::ZERO), vec![]);

    // No room at all: SRPC's rule is its full batch, so all three arrive.
    for (_, peer) in pairs.iter() {
        (&*peer).write_all(b"x").unwrap();
    }
    assert_eq!(wait(&mut backend, 0, Duration::from_secs(1)).len(), 3);
}

#[test]
fn trait_wait_returns_at_most_one_hundred_events() {
    let mut backend = backend();
    let _pairs = readable_fds(&mut backend, 120);
    // Lion's reactor passes room for 1024; SRPC's batch holds 100.
    let first = wait(&mut backend, 1024, Duration::from_secs(1));
    let second = wait(&mut backend, 1024, Duration::from_secs(1));
    assert_eq!((first.len(), second.len()), (100, 20));
    let mut tokens: Vec<usize> = first.iter().chain(second.iter()).map(|e| e.token).collect();
    tokens.sort();
    assert_eq!(tokens, (1..=120).collect::<Vec<_>>());
}

#[test]
fn trait_interrupt_cuts_a_wait_short_and_is_never_an_event() {
    let mut backend = backend();
    let interrupt = OsBackend::interrupt(&backend);
    let signaller = std::thread::spawn(move || {
        std::thread::sleep(Duration::from_millis(50));
        interrupt.signal().expect("signal");
    });
    let start = Instant::now();
    let events = wait(&mut backend, 1024, Duration::from_secs(10));
    let elapsed = start.elapsed();
    signaller.join().unwrap();
    assert_eq!(events, vec![]);
    assert!(elapsed < Duration::from_secs(2), "the interrupt took {elapsed:?} to end a 10 s wait");
    // Consumed: the next wait does not return early for it.
    let start = Instant::now();
    assert_eq!(wait(&mut backend, 1024, Duration::from_millis(50)), vec![]);
    assert!(start.elapsed() >= Duration::from_millis(40), "a consumed interrupt ended a wait again");
}
