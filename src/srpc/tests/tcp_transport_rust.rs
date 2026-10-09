// The TCP transport as Lion tasks (S5 of docs/dev/lion-runtime-plan.md): on
// its PollThread a connection is a reader task and a writer task over one
// AsyncFd, and a listener is an accept task. send_frame wakes the writer
// directly, from any thread, on the outbound buffer's empty->non-empty edge.
//
// These tests drive real PollThreads and real loopback sockets. A raw
// std::net peer plays the other end where a test needs to control it
// byte by byte (partial frames, half-close, reset, a peer that stops
// reading). Each wake and close path has a test that fails if that path is
// dropped; the negative controls are recorded in the plan's S5 note.
//
// Every test takes SERIAL: the descriptor counts must not see another test's
// sockets, and the latency test must not compete with the other tests.

#![allow(unsafe_code)]

use srpc::callback_wrapper::detail::CallbackWrapper;
use srpc::channel::{
    ChannelConnectionProxy, ChannelError, ChannelFrame,
    NullableChannelConnectionProxy, OnAcceptCallback, OnClosedCallback, OnErrorCallback,
    OnFrameCallback,
};
use srpc::misc::OneTimeJob;
use srpc::reactor::PollThread;
use srpc::tcp_channel::{make_tcp_listener_channel_proxy, TcpFactory, TcpListener};
use std::io::{Read, Write};
use std::net::{Shutdown, TcpStream};
use std::os::fd::AsRawFd;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{mpsc, Arc, Mutex, MutexGuard, PoisonError};
use std::time::{Duration, Instant};

static SERIAL: Mutex<()> = Mutex::new(());

fn serial() -> MutexGuard<'static, ()> {
    SERIAL.lock().unwrap_or_else(PoisonError::into_inner)
}

const LIMIT: Duration = Duration::from_secs(20);

extern "C" {
    fn fcntl(fd: i32, command: i32, ...) -> i32;
    fn setsockopt(fd: i32, level: i32, name: i32, value: *const core::ffi::c_void, length: u32) -> i32;
    fn getsockopt(fd: i32, level: i32, name: i32, value: *mut core::ffi::c_void, length: *mut u32) -> i32;
    fn getsockname(fd: i32, address: *mut core::ffi::c_void, length: *mut u32) -> i32;
}

fn open_fds() -> usize {
    std::fs::read_dir("/proc/self/fd").unwrap().count()
}

fn fd_is_open(fd: i32) -> bool {
    const F_GETFD: i32 = 1;
    unsafe { fcntl(fd, F_GETFD) >= 0 }
}

// Waits until `done` holds, polling; false on timeout.
fn eventually(limit: Duration, mut done: impl FnMut() -> bool) -> bool {
    let start = Instant::now();
    while start.elapsed() < limit {
        if done() {
            return true;
        }
        std::thread::sleep(Duration::from_millis(2));
    }
    done()
}

// Settles descriptors a previous test's threads may still be closing, then
// counts them.
fn settled_fds() -> usize {
    std::thread::sleep(Duration::from_millis(50));
    open_fds()
}

fn pattern(seed: usize, len: usize) -> Vec<u8> {
    (0..len).map(|i| (i.wrapping_mul(31) ^ seed.wrapping_mul(131) ^ (i >> 9)) as u8).collect()
}

fn frame_payload(frame: &ChannelFrame) -> Vec<u8> {
    if frame.size == 0 {
        return Vec::new();
    }
    unsafe { std::slice::from_raw_parts(frame.payload, frame.size) }.to_vec()
}

// The wire form the TCP send path open-codes: a 4-byte native-endian size,
// then the payload.
fn encode(payload: &[u8]) -> Vec<u8> {
    let mut bytes = (payload.len() as u32).to_ne_bytes().to_vec();
    bytes.extend_from_slice(payload);
    bytes
}

fn read_frame(stream: &mut TcpStream) -> std::io::Result<Vec<u8>> {
    let mut header = [0u8; 4];
    stream.read_exact(&mut header)?;
    let size = u32::from_ne_bytes(header) as usize;
    let mut payload = vec![0u8; size];
    stream.read_exact(&mut payload)?;
    Ok(payload)
}

fn send(proxy: &ChannelConnectionProxy, payload: &[u8]) -> ChannelError {
    let frame = ChannelFrame { payload: payload.as_ptr(), size: payload.len() };
    unsafe { proxy.send_frame(&frame) }
}

// Sets a flag when dropped: captured by a connection's callbacks, it shows
// that the connection itself -- and so both of its tasks, which own it --
// is gone.
struct DropFlag(Arc<AtomicBool>);

impl Drop for DropFlag {
    fn drop(&mut self) {
        self.0.store(true, Ordering::Release);
    }
}

// What a connection's callbacks observed.
struct Observed {
    frames: mpsc::Receiver<Vec<u8>>,
    closed: mpsc::Receiver<ChannelError>,
    errors: Arc<Mutex<Vec<(ChannelError, String)>>>,
    closes: Arc<AtomicUsize>,
    dropped: Arc<AtomicBool>,
}

// Installs recording callbacks on `proxy`. The frame callback also owns a
// DropFlag, so `dropped` reports the connection's destruction.
fn observe(proxy: &mut ChannelConnectionProxy) -> Observed {
    let (frame_tx, frames) = mpsc::channel();
    let (closed_tx, closed) = mpsc::channel();
    let errors = Arc::new(Mutex::new(Vec::new()));
    let closes = Arc::new(AtomicUsize::new(0));
    let dropped = Arc::new(AtomicBool::new(false));
    let flag = DropFlag(dropped.clone());
    let frame_tx = Mutex::new(frame_tx);
    proxy.set_on_frame(OnFrameCallback::from_callable(Box::new(move |frame: &ChannelFrame| {
        let _keep = &flag;
        let _ = frame_tx.lock().unwrap().send(frame_payload(frame));
    })));
    let closed_tx = Mutex::new(closed_tx);
    let close_count = closes.clone();
    proxy.set_on_closed(OnClosedCallback::from_callable(Box::new(move |reason: ChannelError| {
        close_count.fetch_add(1, Ordering::SeqCst);
        let _ = closed_tx.lock().unwrap().send(reason);
    })));
    let error_log = errors.clone();
    proxy.set_on_error(OnErrorCallback::from_callable(Box::new(move |error: ChannelError, what: &str| {
        error_log.lock().unwrap().push((error, what.to_string()));
    })));
    Observed { frames, closed, errors, closes, dropped }
}

// A connection made through TcpFactory on `pt` to a raw std peer.
fn connect_to_raw_peer(pt: &Arc<PollThread>) -> (ChannelConnectionProxy, TcpStream) {
    let peer = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let connected = TcpFactory::new(pt.clone()).connect(&peer.local_addr().unwrap().to_string());
    assert_eq!(connected.error, ChannelError::None);
    let (stream, _) = peer.accept().unwrap();
    (connected.connection.unwrap(), stream)
}

type ProxySlot = Arc<Mutex<Option<ChannelConnectionProxy>>>;
// Each accepted connection: its proxy, its drop flag, its on_closed count.
type Accepted = Arc<Mutex<Vec<(ProxySlot, Arc<AtomicBool>, Arc<AtomicUsize>)>>>;

// A TcpFactory listener on `pt` that echoes every frame back on the
// connection that carried it, from inside the reader task. Accepted
// connections are kept (with their observation) in the returned list.
struct EchoServer {
    listener: srpc::channel::ChannelListenerProxy,
    address: String,
    accepted: Accepted,
}

fn echo_server(pt: &Arc<PollThread>) -> EchoServer {
    let accepted: Accepted = Arc::new(Mutex::new(Vec::new()));
    let sink = accepted.clone();
    let mut listener = TcpFactory::new(pt.clone()).make_listener().unwrap();
    listener.set_on_accept(OnAcceptCallback::from_callable(Box::new(
        move |connection: NullableChannelConnectionProxy| {
            let mut proxy = connection.unwrap();
            let slot: ProxySlot = Arc::new(Mutex::new(None));
            let echo = slot.clone();
            let dropped = Arc::new(AtomicBool::new(false));
            let flag = DropFlag(dropped.clone());
            proxy.set_on_frame(OnFrameCallback::from_callable(Box::new(move |frame: &ChannelFrame| {
                let _keep = &flag;
                let guard = echo.lock().unwrap();
                if let Some(proxy) = guard.as_ref() {
                    let result = unsafe { proxy.send_frame(frame) };
                    assert_eq!(result, ChannelError::None, "the echo was refused");
                }
            })));
            let closes = Arc::new(AtomicUsize::new(0));
            let close_count = closes.clone();
            proxy.set_on_closed(OnClosedCallback::from_callable(Box::new(move |_reason: ChannelError| {
                close_count.fetch_add(1, Ordering::SeqCst);
            })));
            *slot.lock().unwrap() = Some(proxy);
            sink.lock().unwrap().push((slot, dropped, closes));
        },
    )));
    assert_eq!(listener.listen("127.0.0.1:0"), ChannelError::None);
    let address = listener.local_address();
    EchoServer { listener, address, accepted }
}

impl EchoServer {
    // Closes every accepted connection and drops the proxies; returns their
    // drop flags.
    fn release_accepted(&self) -> Vec<Arc<AtomicBool>> {
        let mut flags = Vec::new();
        for (slot, dropped, _) in self.accepted.lock().unwrap().drain(..) {
            if let Some(proxy) = slot.lock().unwrap().take() {
                proxy.close();
            }
            flags.push(dropped);
        }
        flags
    }
}

// ---------------------------------------------------------------------------
// Framing across partial writes and reads

// Frames far larger than the socket buffers leave the writer on EAGAIN over
// and over while the peer reads in small pieces; frames the peer dribbles in
// a few bytes at a time reach the reader in many partial reads; and a frame
// written in one burst makes the reader hit its per-poll read budget (it
// must wake itself, or it stalls with data queued and no edge to come).
#[test]
fn large_frames_cross_many_partial_writes_and_reads() {
    let _serial = serial();
    let pt = PollThread::create();
    let (mut proxy, mut peer) = connect_to_raw_peer(&pt);
    let observed = observe(&mut proxy);

    // Client -> peer: the writer stalls on a full socket many times. The
    // 5 MiB frame can take the buffer past the 4 MiB high-water mark; a send
    // refused there is retried once the writer has drained below it.
    let outbound: Vec<Vec<u8>> = vec![pattern(1, 3 << 20), pattern(2, 1), pattern(3, 5 << 20), Vec::new(), pattern(4, 1 << 20)];
    let expected_bytes: usize = outbound.iter().map(|p| 4 + p.len()).sum();
    peer.set_read_timeout(Some(LIMIT)).unwrap();
    let mut slow_peer = peer.try_clone().unwrap();
    let slow_reader = std::thread::spawn(move || {
        let mut chunk = vec![0u8; 1500];
        let mut stream_bytes = Vec::new();
        let mut reads = 0usize;
        while stream_bytes.len() < expected_bytes {
            let n = slow_peer.read(&mut chunk).expect("the writer stopped making progress");
            assert!(n > 0, "the client closed early");
            stream_bytes.extend_from_slice(&chunk[..n]);
            reads += 1;
            if reads.is_multiple_of(512) {
                // Let the socket fill, so the writer meets EAGAIN again.
                std::thread::sleep(Duration::from_millis(2));
            }
        }
        stream_bytes
    });
    let mut refused = 0usize;
    for payload in &outbound {
        loop {
            let result = send(&proxy, payload);
            if result == ChannelError::WouldBlock {
                refused += 1;
                std::thread::sleep(Duration::from_millis(1));
                continue;
            }
            assert_eq!(result, ChannelError::None);
            break;
        }
    }
    println!("sends refused at the high-water mark: {refused}");
    let stream_bytes = slow_reader.join().unwrap();
    let mut at = 0usize;
    for (i, payload) in outbound.iter().enumerate() {
        let size = u32::from_ne_bytes(stream_bytes[at..at + 4].try_into().unwrap()) as usize;
        assert_eq!(size, payload.len(), "frame {i} header");
        assert!(stream_bytes[at + 4..at + 4 + size] == payload[..], "frame {i} payload");
        at += 4 + size;
    }

    // Peer -> client in dribbles: many partial reads per frame.
    let inbound: Vec<Vec<u8>> = vec![pattern(5, 2 << 20), pattern(6, 7), pattern(7, 300_000)];
    let writer_peer = peer.try_clone().unwrap();
    let dribble = inbound.clone();
    let dribbler = std::thread::spawn(move || {
        let mut writer_peer = writer_peer;
        let mut sent = 0usize;
        for payload in &dribble {
            let bytes = encode(payload);
            let mut at = 0usize;
            while at < bytes.len() {
                let step = (1 + (at * 7 + sent) % 997).min(bytes.len() - at);
                writer_peer.write_all(&bytes[at..at + step]).unwrap();
                at += step;
                sent += 1;
                if sent.is_multiple_of(256) {
                    std::thread::sleep(Duration::from_micros(200));
                }
            }
        }
    });
    for (i, payload) in inbound.iter().enumerate() {
        let got = observed.frames.recv_timeout(LIMIT).expect("a dribbled frame never arrived");
        assert!(got == *payload, "dribbled frame {i} differs");
    }
    dribbler.join().unwrap();

    // Peer -> client in one burst: more than the reader's per-poll budget.
    let burst = pattern(8, 6 << 20);
    peer.write_all(&encode(&burst)).unwrap();
    let got = observed.frames.recv_timeout(LIMIT).expect("a burst frame stalled (the read budget must re-wake the reader)");
    assert!(got == burst, "burst frame differs");

    assert!(observed.errors.lock().unwrap().is_empty());
    assert_eq!(observed.closes.load(Ordering::SeqCst), 0);
    proxy.close();
    drop(proxy);
    assert!(eventually(LIMIT, || observed.dropped.load(Ordering::Acquire)), "a transport task outlived its closed connection");
    pt.shutdown();
}

// More bytes than the reader takes in one poll (16 reads of 64 KiB), all
// queued before it first runs and none arriving after: its readiness stays
// set and no edge is coming, so the reader must wake itself to go on. The
// listening socket's receive buffer is raised first, so an accepted socket
// can hold the whole burst, and the poll thread is held in a job while the
// peer writes it.
//
// This needs net.core.rmem_max of at least 4 MiB (SO_RCVBUF is clamped to
// it silently). Below that the burst does not fit before the reader runs,
// more data arrives on later edges, and the test passes without reaching
// the budget; it says so on stderr (shown with --nocapture).
#[test]
fn a_burst_beyond_the_read_budget_is_read_to_the_end() {
    let _serial = serial();
    const SOL_SOCKET: i32 = 1;
    const SO_RCVBUF: i32 = 8;
    let pt = PollThread::create();
    let mut listener = Arc::new(TcpListener::new());
    Arc::get_mut(&mut listener).unwrap().set_poll_thread(pt.clone());
    let (frame_tx, frame_rx) = mpsc::channel::<Vec<u8>>();
    let frame_tx = Mutex::new(frame_tx);
    let kept: Arc<Mutex<Vec<ChannelConnectionProxy>>> = Arc::new(Mutex::new(Vec::new()));
    let keep = kept.clone();
    listener.set_on_accept(CallbackWrapper::from_callable(Box::new(move |proxy: NullableChannelConnectionProxy| {
        let mut proxy = proxy.unwrap();
        let tx = frame_tx.lock().unwrap().clone();
        proxy.set_on_frame(OnFrameCallback::from_callable(Box::new(move |frame: &ChannelFrame| {
            let _ = tx.send(frame_payload(frame));
        })));
        keep.lock().unwrap().push(proxy);
    })));
    let mut shim = make_tcp_listener_channel_proxy(listener.clone());
    assert_eq!(shim.listen("127.0.0.1:0"), ChannelError::None);
    let rcvbuf: i32 = 4 << 20;
    assert_eq!(
        unsafe { setsockopt(listener.fd(), SOL_SOCKET, SO_RCVBUF, (&raw const rcvbuf).cast(), 4) },
        0
    );
    // Linux reports twice the granted size.
    let mut granted: i32 = 0;
    let mut length: u32 = 4;
    assert_eq!(
        unsafe { getsockopt(listener.fd(), SOL_SOCKET, SO_RCVBUF, (&raw mut granted).cast(), &raw mut length) },
        0
    );
    if granted < 2 * rcvbuf {
        eprintln!(
            "a_burst_beyond_the_read_budget_is_read_to_the_end: SO_RCVBUF clamped to {} bytes \
             (raise net.core.rmem_max to 4 MiB); the read budget is not exercised",
            granted / 2
        );
    }
    for round in 0..5usize {
        let (entered_tx, entered_rx) = mpsc::channel::<()>();
        let (release_tx, release_rx) = mpsc::channel::<()>();
        let entered_tx = Mutex::new(entered_tx);
        let release_rx = Mutex::new(release_rx);
        pt.add(Arc::new(OneTimeJob::new(Box::new(move || {
            entered_tx.lock().unwrap().send(()).unwrap();
            let _ = release_rx.lock().unwrap().recv_timeout(LIMIT);
        }))));
        entered_rx.recv_timeout(LIMIT).unwrap();
        let mut peer = TcpStream::connect(listener.local_address()).unwrap();
        let burst = pattern(round, (1 << 20) + (512 << 10));
        peer.write_all(&encode(&burst)).unwrap();
        // Let the last segment land in the accepted socket's queue.
        std::thread::sleep(Duration::from_millis(50));
        release_tx.send(()).unwrap();
        let got = frame_rx.recv_timeout(Duration::from_secs(5)).expect("the reader stalled at its read budget");
        assert!(got == burst, "burst {round} differs");
        drop(peer);
    }
    listener.close();
    drop(shim);
    kept.lock().unwrap().clear();
    pt.shutdown();
}

// ---------------------------------------------------------------------------
// Concurrency

#[test]
fn many_concurrent_connections_echo_in_parallel() {
    let _serial = serial();
    const CONNECTIONS: usize = 64;
    const FRAMES: usize = 200;
    let fds_before = settled_fds();
    let server_pt = PollThread::create();
    let client_pts: Vec<Arc<PollThread>> = (0..4).map(|_| PollThread::create()).collect();
    let server = echo_server(&server_pt);
    let mut drivers = Vec::new();
    let client_dropped: Arc<Mutex<Vec<Arc<AtomicBool>>>> = Arc::new(Mutex::new(Vec::new()));
    for c in 0..CONNECTIONS {
        let pt = client_pts[c % client_pts.len()].clone();
        let address = server.address.clone();
        let dropped_sink = client_dropped.clone();
        drivers.push(std::thread::spawn(move || {
            let connected = TcpFactory::new(pt).connect(&address);
            assert_eq!(connected.error, ChannelError::None);
            let mut proxy = connected.connection.unwrap();
            let observed = observe(&mut proxy);
            dropped_sink.lock().unwrap().push(observed.dropped.clone());
            let window = 8usize;
            let mut expected = std::collections::VecDeque::new();
            let mut sent = 0usize;
            let mut received = 0usize;
            while received < FRAMES {
                while sent < FRAMES && sent - received < window {
                    let payload = pattern(c * 1000 + sent, (sent * 37 + c) % 4096);
                    assert_eq!(send(&proxy, &payload), ChannelError::None);
                    expected.push_back(payload);
                    sent += 1;
                }
                let got = observed.frames.recv_timeout(LIMIT).expect("an echo was lost");
                assert!(got == expected.pop_front().unwrap(), "connection {c}: echo {received} differs");
                received += 1;
            }
            proxy.close();
            assert_eq!(observed.closed.recv_timeout(LIMIT).unwrap(), ChannelError::None);
            assert!(observed.errors.lock().unwrap().is_empty());
        }));
    }
    for driver in drivers {
        driver.join().unwrap();
    }
    // Every server-side connection reads its client's EOF and closes.
    assert!(
        eventually(LIMIT, || {
            let accepted = server.accepted.lock().unwrap();
            accepted.len() == CONNECTIONS && accepted.iter().all(|(_, _, closes)| closes.load(Ordering::SeqCst) == 1)
        }),
        "not every server connection saw its client close"
    );
    let server_flags = server.release_accepted();
    let flags: Vec<Arc<AtomicBool>> = client_dropped.lock().unwrap().iter().cloned().chain(server_flags).collect();
    assert_eq!(flags.len(), 2 * CONNECTIONS);
    assert!(
        eventually(LIMIT, || flags.iter().all(|f| f.load(Ordering::Acquire))),
        "{} of {} connections were not released after close",
        flags.iter().filter(|f| !f.load(Ordering::Acquire)).count(),
        flags.len()
    );
    let EchoServer { mut listener, .. } = server;
    listener.close();
    drop(listener);
    for pt in &client_pts {
        pt.shutdown();
    }
    server_pt.shutdown();
    drop(client_pts);
    drop(server_pt);
    assert!(eventually(LIMIT, || open_fds() == fds_before), "descriptors leaked: {} before, {} after", fds_before, open_fds());
}

// Frames sent concurrently from many threads arrive whole, and each
// sender's frames arrive in the order it sent them.
#[test]
fn sends_from_many_foreign_threads_keep_each_senders_order() {
    let _serial = serial();
    const SENDERS: usize = 8;
    const PER_SENDER: u32 = 3000;
    let pt = PollThread::create();
    let (proxy, mut peer) = connect_to_raw_peer(&pt);
    let proxy: Arc<ChannelConnectionProxy> = Arc::new(proxy);
    let reader = std::thread::spawn(move || {
        peer.set_read_timeout(Some(LIMIT)).unwrap();
        let mut next = [0u32; SENDERS];
        for _ in 0..(SENDERS as u32 * PER_SENDER) {
            let frame = read_frame(&mut peer).expect("a frame was lost");
            let sender = frame[0] as usize;
            let seq = u32::from_ne_bytes(frame[1..5].try_into().unwrap());
            assert_eq!(seq, next[sender], "sender {sender} out of order");
            assert_eq!(frame.len(), 5 + (seq as usize % 61), "sender {sender} frame {seq} truncated");
            next[sender] += 1;
        }
        next
    });
    let senders: Vec<_> = (0..SENDERS)
        .map(|sender| {
            let proxy = proxy.clone();
            std::thread::spawn(move || {
                for seq in 0..PER_SENDER {
                    let mut payload = vec![sender as u8];
                    payload.extend_from_slice(&seq.to_ne_bytes());
                    payload.resize(5 + (seq as usize % 61), 0xAB);
                    assert_eq!(send(&proxy, &payload), ChannelError::None);
                }
            })
        })
        .collect();
    for sender in senders {
        sender.join().unwrap();
    }
    let counts = reader.join().unwrap();
    assert!(counts.iter().all(|&n| n == PER_SENDER));
    proxy.close();
    drop(proxy);
    pt.shutdown();
}

// ---------------------------------------------------------------------------
// Peer close and error

// Half of a frame, then the peer's FIN: the complete frame before it is
// delivered, the partial one is not, and the connection closes with no
// error, as handle_read's EOF path does.
#[test]
fn a_peer_half_close_mid_frame_closes_without_an_error() {
    let _serial = serial();
    let pt = PollThread::create();
    let (mut proxy, mut peer) = connect_to_raw_peer(&pt);
    let observed = observe(&mut proxy);
    let whole = pattern(11, 5000);
    let partial = encode(&pattern(12, 9000));
    peer.write_all(&encode(&whole)).unwrap();
    peer.write_all(&partial[..partial.len() / 2]).unwrap();
    peer.shutdown(Shutdown::Write).unwrap();
    assert!(observed.frames.recv_timeout(LIMIT).unwrap() == whole);
    assert_eq!(observed.closed.recv_timeout(LIMIT).expect("EOF did not close the connection"), ChannelError::None);
    assert!(observed.frames.try_recv().is_err(), "a partial frame was delivered");
    assert!(observed.errors.lock().unwrap().is_empty(), "EOF reported an error: {:?}", observed.errors.lock().unwrap());
    assert!(proxy.is_closed());
    drop(proxy);
    assert!(eventually(LIMIT, || observed.dropped.load(Ordering::Acquire)), "the transport outlived an EOF close");
    assert_eq!(observed.closes.load(Ordering::SeqCst), 1);
    pt.shutdown();
}

// A frame and the FIN right behind it: the reader reads on to EAGAIN or EOF
// after a short read, so it delivers the frame and then sees the EOF, rather
// than consuming the one edge that carried both and missing the EOF.
#[test]
fn a_frame_followed_by_eof_is_delivered_and_then_closes() {
    let _serial = serial();
    let pt = PollThread::create();
    for round in 0..20usize {
        let (mut proxy, mut peer) = connect_to_raw_peer(&pt);
        let observed = observe(&mut proxy);
        // Let the tasks start and park first, so both arrive on one edge.
        std::thread::sleep(Duration::from_millis(5));
        let payload = pattern(round, 100 + round);
        let mut bytes = encode(&payload);
        bytes.extend_from_slice(&encode(&payload));
        peer.write_all(&bytes).unwrap();
        peer.shutdown(Shutdown::Write).unwrap();
        assert!(observed.frames.recv_timeout(LIMIT).unwrap() == payload);
        assert!(observed.frames.recv_timeout(LIMIT).unwrap() == payload);
        assert_eq!(observed.closed.recv_timeout(LIMIT).expect("the EOF behind a frame was missed"), ChannelError::None);
        drop(proxy);
        assert!(eventually(LIMIT, || observed.dropped.load(Ordering::Acquire)));
    }
    pt.shutdown();
}

// Half of a frame, then a reset: the receive error is reported and the
// connection closes with it.
#[test]
fn a_peer_reset_mid_frame_reports_the_error_and_closes() {
    let _serial = serial();
    let pt = PollThread::create();
    let (mut proxy, mut peer) = connect_to_raw_peer(&pt);
    let observed = observe(&mut proxy);
    let partial = encode(&pattern(13, 9000));
    peer.write_all(&partial[..partial.len() / 2]).unwrap();
    // SO_LINGER with a zero timeout: close sends a reset.
    #[repr(C)]
    struct Linger {
        onoff: i32,
        linger: i32,
    }
    let linger = Linger { onoff: 1, linger: 0 };
    const SOL_SOCKET: i32 = 1;
    const SO_LINGER: i32 = 13;
    assert_eq!(
        unsafe { setsockopt(peer.as_raw_fd(), SOL_SOCKET, SO_LINGER, (&raw const linger).cast(), 8) },
        0
    );
    drop(peer);
    let reason = observed.closed.recv_timeout(LIMIT).expect("a reset did not close the connection");
    assert_eq!(reason, ChannelError::ConnectionReset);
    let errors = observed.errors.lock().unwrap().clone();
    assert_eq!(errors, vec![(ChannelError::ConnectionReset, "socket receive failed".to_string())]);
    assert!(observed.frames.try_recv().is_err(), "a partial frame was delivered");
    drop(proxy);
    assert!(eventually(LIMIT, || observed.dropped.load(Ordering::Acquire)), "the transport outlived a reset");
    assert_eq!(observed.closes.load(Ordering::SeqCst), 1);
    pt.shutdown();
}

// ---------------------------------------------------------------------------
// Close from a foreign thread

// The writer waits on write readiness (the peer stopped reading) when
// another thread closes the connection: on_closed fires once, with no error,
// and both tasks retire.
#[test]
fn a_foreign_close_during_inflight_writes_releases_the_connection() {
    let _serial = serial();
    let pt = PollThread::create();
    let (mut proxy, mut peer) = connect_to_raw_peer(&pt);
    let observed = observe(&mut proxy);
    let payload = pattern(21, 1 << 20);
    for _ in 0..3 {
        assert_eq!(send(&proxy, &payload), ChannelError::None);
    }
    // Let the writer fill the socket and wait for its next write edge.
    std::thread::sleep(Duration::from_millis(100));
    let proxy = Arc::new(proxy);
    let closer_proxy = proxy.clone();
    std::thread::spawn(move || closer_proxy.close()).join().unwrap();
    assert_eq!(observed.closed.recv_timeout(LIMIT).unwrap(), ChannelError::None);
    assert!(observed.errors.lock().unwrap().is_empty(), "a local close reported {:?}", observed.errors.lock().unwrap());
    drop(proxy);
    assert!(eventually(LIMIT, || observed.dropped.load(Ordering::Acquire)), "a transport task outlived the close");
    assert_eq!(observed.closes.load(Ordering::SeqCst), 1);
    // The peer reads what the kernel had accepted, then the end of stream.
    peer.set_read_timeout(Some(LIMIT)).unwrap();
    let mut sink = Vec::new();
    let _ = peer.read_to_end(&mut sink);
    pt.shutdown();
}

// An idle connection's writer waits on the empty buffer, where no readiness
// edge reaches it: close must wake it, or it never retires and the
// connection is never released.
#[test]
fn a_foreign_close_of_an_idle_connection_retires_both_tasks() {
    let _serial = serial();
    let pt = PollThread::create();
    for _ in 0..20 {
        let (mut proxy, mut peer) = connect_to_raw_peer(&pt);
        let observed = observe(&mut proxy);
        // One frame out and read, so the writer has drained and parked.
        assert_eq!(send(&proxy, b"ping"), ChannelError::None);
        peer.set_read_timeout(Some(LIMIT)).unwrap();
        assert_eq!(read_frame(&mut peer).unwrap(), b"ping");
        std::thread::sleep(Duration::from_millis(2));
        let proxy = Arc::new(proxy);
        let closer_proxy = proxy.clone();
        std::thread::spawn(move || closer_proxy.close()).join().unwrap();
        assert_eq!(observed.closed.recv_timeout(LIMIT).unwrap(), ChannelError::None);
        drop(proxy);
        assert!(eventually(LIMIT, || observed.dropped.load(Ordering::Acquire)), "an idle writer was never retired");
    }
    pt.shutdown();
}

// Client::close closes its channel from a job on the PollThread. The writer,
// woken by that close on the thread's own ready queue, can run and retire
// the transport -- deregistering the descriptor -- before the next park
// reports the shutdown's hang-up edge, which deregistration discards. The
// retiring task must therefore wake the reader itself, or the reader never
// finishes.
#[test]
fn a_close_on_the_poll_thread_retires_both_tasks() {
    let _serial = serial();
    let pt = PollThread::create();
    for _ in 0..20 {
        let (mut proxy, mut peer) = connect_to_raw_peer(&pt);
        let observed = observe(&mut proxy);
        assert_eq!(send(&proxy, b"ping"), ChannelError::None);
        peer.set_read_timeout(Some(LIMIT)).unwrap();
        assert_eq!(read_frame(&mut peer).unwrap(), b"ping");
        std::thread::sleep(Duration::from_millis(2));
        let (done_tx, done_rx) = mpsc::channel();
        let done_tx = Mutex::new(done_tx);
        let slot = Mutex::new(Some(proxy));
        pt.add(Arc::new(OneTimeJob::new(Box::new(move || {
            if let Some(proxy) = slot.lock().unwrap().take() {
                proxy.close();
                drop(proxy);
                done_tx.lock().unwrap().send(()).unwrap();
            }
        }))));
        done_rx.recv_timeout(LIMIT).expect("the close job never ran");
        assert_eq!(observed.closed.recv_timeout(LIMIT).unwrap(), ChannelError::None);
        assert!(eventually(LIMIT, || observed.dropped.load(Ordering::Acquire)), "a task outlived a close made on its own poll thread");
        let mut rest = Vec::new();
        assert_eq!(peer.read_to_end(&mut rest).unwrap(), 0, "the peer did not see the close");
    }
    pt.shutdown();
}

// Frames sent before the PollThread has started the connection's tasks (the
// connect's hand-over job is still queued) go out on the writer's first
// poll: no edge wake reaches a writer that has not parked yet. (With
// write-through these small frames go out from the sending thread instead;
// a_hand_back_before_the_tasks_start_is_sent_on_the_first_poll covers the
// writer's first poll there.)
#[test]
fn frames_sent_before_the_tasks_start_go_out_on_the_first_poll() {
    let _serial = serial();
    let pt = PollThread::create();
    for round in 0..10usize {
        // Hold the poll thread inside a job, so the hand-over job waits.
        let (entered_tx, entered_rx) = mpsc::channel::<()>();
        let (release_tx, release_rx) = mpsc::channel::<()>();
        let entered_tx = Mutex::new(entered_tx);
        let release_rx = Mutex::new(release_rx);
        pt.add(Arc::new(OneTimeJob::new(Box::new(move || {
            entered_tx.lock().unwrap().send(()).unwrap();
            let _ = release_rx.lock().unwrap().recv_timeout(LIMIT);
        }))));
        entered_rx.recv_timeout(LIMIT).unwrap();
        let (proxy, mut peer) = connect_to_raw_peer(&pt);
        let payload = pattern(round, 10 + round);
        assert_eq!(send(&proxy, &payload), ChannelError::None);
        release_tx.send(()).unwrap();
        peer.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
        assert_eq!(read_frame(&mut peer).expect("a frame queued before the tasks started was never sent"), payload);
        proxy.close();
    }
    pt.shutdown();
}

// Connections opened and closed every way (client close, server close, peer
// EOF) leave no connection, task or descriptor behind.
#[test]
fn connections_leave_no_descriptor_or_task_behind() {
    let _serial = serial();
    let fds_before = settled_fds();
    let server_pt = PollThread::create();
    let client_pt = PollThread::create();
    let server = echo_server(&server_pt);
    let mut flags = Vec::new();
    for round in 0..30usize {
        match round % 3 {
            0 | 1 => {
                let connected = TcpFactory::new(client_pt.clone()).connect(&server.address);
                assert_eq!(connected.error, ChannelError::None);
                let mut proxy = connected.connection.unwrap();
                let observed = observe(&mut proxy);
                assert_eq!(send(&proxy, b"echo"), ChannelError::None);
                assert_eq!(observed.frames.recv_timeout(LIMIT).unwrap(), b"echo");
                if round % 3 == 0 {
                    proxy.close();
                } else {
                    // The server closes its side; the client reads EOF.
                    for (slot, _, _) in server.accepted.lock().unwrap().iter() {
                        if let Some(accepted) = slot.lock().unwrap().as_ref() {
                            accepted.close();
                        }
                    }
                }
                assert_eq!(observed.closed.recv_timeout(LIMIT).unwrap(), ChannelError::None);
                drop(proxy);
                flags.push(observed.dropped);
            }
            _ => {
                // A raw peer that sends one frame and hangs up.
                let (mut proxy, mut peer) = connect_to_raw_peer(&client_pt);
                let observed = observe(&mut proxy);
                peer.write_all(&encode(b"bye")).unwrap();
                drop(peer);
                assert_eq!(observed.frames.recv_timeout(LIMIT).unwrap(), b"bye");
                assert_eq!(observed.closed.recv_timeout(LIMIT).unwrap(), ChannelError::None);
                drop(proxy);
                flags.push(observed.dropped);
            }
        }
        flags.extend(server.release_accepted());
    }
    assert!(
        eventually(LIMIT, || flags.iter().all(|f| f.load(Ordering::Acquire))),
        "{} connections were not released",
        flags.iter().filter(|f| !f.load(Ordering::Acquire)).count()
    );
    let EchoServer { mut listener, .. } = server;
    listener.close();
    drop(listener);
    // With the PollThreads still running, only their own descriptors remain:
    // two epoll instances and two eventfds.
    assert!(
        eventually(LIMIT, || open_fds() == fds_before + 4),
        "descriptors leaked while the PollThreads run: {} before, {} now",
        fds_before,
        open_fds()
    );
    client_pt.shutdown();
    server_pt.shutdown();
    drop(client_pt);
    drop(server_pt);
    assert!(eventually(LIMIT, || open_fds() == fds_before), "descriptors leaked: {} before, {} after", fds_before, open_fds());
}

// A listener closed from another thread: its accept task retires and
// releases the descriptor lease, so the listening socket is closed.
#[test]
fn a_foreign_listener_close_releases_its_descriptor() {
    let _serial = serial();
    let pt = PollThread::create();
    let mut listener = Arc::new(TcpListener::new());
    Arc::get_mut(&mut listener).unwrap().set_poll_thread(pt.clone());
    let accepted = Arc::new(AtomicUsize::new(0));
    let accepted_count = accepted.clone();
    listener.set_on_accept(CallbackWrapper::from_callable(Box::new(move |_proxy: NullableChannelConnectionProxy| {
        accepted_count.fetch_add(1, Ordering::SeqCst);
    })));
    let mut shim = make_tcp_listener_channel_proxy(listener.clone());
    assert_eq!(shim.listen("127.0.0.1:0"), ChannelError::None);
    let fd = listener.fd();
    let address = listener.local_address();
    let _client = TcpStream::connect(&address).unwrap();
    assert!(eventually(LIMIT, || accepted.load(Ordering::SeqCst) == 1), "the accept task never accepted");
    let closer = listener.clone();
    std::thread::spawn(move || closer.close()).join().unwrap();
    assert!(eventually(LIMIT, || !fd_is_open(fd)), "the listening descriptor stayed open after close");
    assert!(TcpStream::connect(&address).is_err(), "a closed listener still accepts");
    drop(shim);
    pt.shutdown();
}

// ---------------------------------------------------------------------------
// Wake latency

// A frame sent from another thread reaches the peer promptly: send_frame
// wakes the parked writer directly, through its Lion waker.
#[test]
fn a_foreign_send_wakes_the_writer_promptly() {
    let _serial = serial();
    const FRAMES: usize = 300;
    let pt = PollThread::create();
    let (proxy, mut peer) = connect_to_raw_peer(&pt);
    let (arrived_tx, arrived_rx) = mpsc::channel();
    let reader = std::thread::spawn(move || {
        peer.set_read_timeout(Some(LIMIT)).unwrap();
        for _ in 0..FRAMES {
            let frame = match read_frame(&mut peer) {
                Ok(frame) => frame,
                Err(_) => return,
            };
            arrived_tx.send((frame, Instant::now())).unwrap();
        }
    });
    let mut samples = Vec::new();
    for i in 0..FRAMES {
        // Idle first, so the writer is parked on the empty buffer.
        std::thread::sleep(Duration::from_millis(1));
        let payload = (i as u32).to_ne_bytes();
        let start = Instant::now();
        assert_eq!(send(&proxy, &payload), ChannelError::None);
        let (frame, at) = arrived_rx.recv_timeout(Duration::from_secs(5)).expect("a foreign send did not wake the writer");
        assert_eq!(frame, payload);
        samples.push(at - start);
    }
    reader.join().unwrap();
    samples.sort();
    let at = |p: f64| samples[((samples.len() - 1) as f64 * p).round() as usize];
    println!(
        "foreign send_frame -> bytes at the peer: n={} min={:?} p50={:?} p90={:?} p99={:?} max={:?}",
        samples.len(),
        samples[0],
        at(0.5),
        at(0.9),
        at(0.99),
        samples[samples.len() - 1]
    );
    assert!(at(0.5) < Duration::from_millis(2), "median foreign send wake {:?}", at(0.5));
    proxy.close();
    drop(proxy);
    pt.shutdown();
}

// ---------------------------------------------------------------------------
// Write-through (lion/s5-writethrough): a sender on another thread that finds
// the outbound buffer empty writes its frame itself, and hands whatever
// send(2) did not take to the writer task.

// Holds `pt`'s thread inside a job until the returned sender is dropped or
// sent to, so its tasks cannot run meanwhile.
fn hold_poll_thread(pt: &Arc<PollThread>) -> mpsc::Sender<()> {
    let (entered_tx, entered_rx) = mpsc::channel::<()>();
    let (release_tx, release_rx) = mpsc::channel::<()>();
    let entered_tx = Mutex::new(entered_tx);
    let release_rx = Mutex::new(release_rx);
    pt.add(Arc::new(OneTimeJob::new(Box::new(move || {
        entered_tx.lock().unwrap().send(()).unwrap();
        let _ = release_rx.lock().unwrap().recv_timeout(LIMIT);
    }))));
    entered_rx.recv_timeout(LIMIT).unwrap();
    release_tx
}

// A frame sent from another thread reaches the peer while the connection's
// poll thread is busy: the sender wrote it itself.
#[test]
fn a_foreign_send_goes_out_while_the_poll_thread_is_busy() {
    let _serial = serial();
    let pt = PollThread::create();
    let (proxy, mut peer) = connect_to_raw_peer(&pt);
    // Let the tasks start and park.
    std::thread::sleep(Duration::from_millis(5));
    for round in 0..10usize {
        let release = hold_poll_thread(&pt);
        let payload = pattern(round, 64 + round);
        assert_eq!(send(&proxy, &payload), ChannelError::None);
        peer.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
        let got = read_frame(&mut peer).expect("a foreign send waited for the busy poll thread");
        assert_eq!(got, payload);
        drop(release);
    }
    proxy.close();
    pt.shutdown();
}

// A frame larger than the socket takes: the sender writes a prefix, and the
// writer -- woken by the hand-back, since no write edge will come for a
// writer parked on the buffer -- sends the rest once it runs.
#[test]
fn a_partial_write_hands_the_rest_to_the_writer() {
    let _serial = serial();
    let pt = PollThread::create();
    for round in 0..3usize {
        let (mut proxy, mut peer) = connect_to_raw_peer(&pt);
        let observed = observe(&mut proxy);
        std::thread::sleep(Duration::from_millis(5));
        let release = hold_poll_thread(&pt);
        let payload = pattern(round, 16 << 20);
        let expected = encode(&payload);
        assert_eq!(send(&proxy, &payload), ChannelError::None);
        // The prefix the sender wrote is readable now; the rest waits for
        // the writer, which cannot run while the poll thread is held.
        peer.set_read_timeout(Some(Duration::from_millis(300))).unwrap();
        let mut got = Vec::with_capacity(expected.len());
        let mut chunk = vec![0u8; 1 << 16];
        loop {
            match peer.read(&mut chunk) {
                Ok(0) => panic!("the connection closed"),
                Ok(n) => got.extend_from_slice(&chunk[..n]),
                Err(_) => break,
            }
        }
        let direct = got.len();
        assert!(direct > 0, "the sender wrote nothing itself");
        assert!(direct < expected.len(), "the socket took all of a 16 MiB frame at once");
        drop(release);
        peer.set_read_timeout(Some(LIMIT)).unwrap();
        while got.len() < expected.len() {
            let n = peer.read(&mut chunk).expect("the writer never sent the handed-back rest");
            assert!(n > 0, "the connection closed early");
            got.extend_from_slice(&chunk[..n]);
        }
        assert!(got == expected, "the frame arrived corrupted");
        println!("partial write: {direct} of {} bytes by the sender", expected.len());
        assert!(observed.errors.lock().unwrap().is_empty());
        proxy.close();
        drop(proxy);
        assert!(eventually(LIMIT, || observed.dropped.load(Ordering::Acquire)));
    }
    pt.shutdown();
}

// The hand-back before the connection's tasks have started (the connect's
// hand-over job waits behind a held poll thread): no writer waker exists yet,
// so the writer's first poll must send the rest.
#[test]
fn a_hand_back_before_the_tasks_start_is_sent_on_the_first_poll() {
    let _serial = serial();
    let pt = PollThread::create();
    for round in 0..3usize {
        let release = hold_poll_thread(&pt);
        let (proxy, mut peer) = connect_to_raw_peer(&pt);
        let payload = pattern(round, 16 << 20);
        let expected = encode(&payload);
        assert_eq!(send(&proxy, &payload), ChannelError::None);
        peer.set_read_timeout(Some(Duration::from_millis(300))).unwrap();
        let mut got = Vec::with_capacity(expected.len());
        let mut chunk = vec![0u8; 1 << 16];
        loop {
            match peer.read(&mut chunk) {
                Ok(0) => panic!("the connection closed"),
                Ok(n) => got.extend_from_slice(&chunk[..n]),
                Err(_) => break,
            }
        }
        assert!(!got.is_empty() && got.len() < expected.len(), "no partial write before the tasks started");
        drop(release);
        peer.set_read_timeout(Some(LIMIT)).unwrap();
        while got.len() < expected.len() {
            let n = peer.read(&mut chunk).expect("the writer's first poll never sent the handed-back rest");
            assert!(n > 0, "the connection closed early");
            got.extend_from_slice(&chunk[..n]);
        }
        assert!(got == expected, "the frame arrived corrupted");
        let small = pattern(round + 7, 33);
        assert_eq!(send(&proxy, &small), ChannelError::None);
        assert_eq!(read_frame(&mut peer).unwrap(), small);
        proxy.close();
    }
    pt.shutdown();
}

// Foreign senders racing each other, the poll thread's own sends and the
// writer's drains, with frames large enough to be written partly and a peer
// that stalls: every frame arrives whole, and each sender's in order.
#[test]
fn write_through_keeps_every_senders_order_under_contention() {
    let _serial = serial();
    const FOREIGN: usize = 6;
    const PER_SENDER: usize = 400;
    const POLL_SENDER: usize = FOREIGN;
    let pt = PollThread::create();
    let (proxy, mut peer) = connect_to_raw_peer(&pt);
    let proxy: Arc<ChannelConnectionProxy> = Arc::new(proxy);
    fn frame_for(sender: usize, seq: usize) -> Vec<u8> {
        let len = match seq % 7 {
            0 => 300_000 + seq * 13,
            1 => 70_000,
            _ => 9 + (seq * 31 + sender) % 2000,
        };
        let mut payload = pattern(sender * 100_000 + seq, len);
        payload[0] = sender as u8;
        payload[1..5].copy_from_slice(&(seq as u32).to_ne_bytes());
        payload
    }
    fn send_retrying(proxy: &ChannelConnectionProxy, payload: &[u8]) {
        let deadline = Instant::now() + LIMIT;
        loop {
            match send(proxy, payload) {
                ChannelError::None => return,
                ChannelError::WouldBlock => {
                    assert!(Instant::now() < deadline, "refused for good");
                    std::thread::sleep(Duration::from_micros(200));
                }
                other => panic!("send failed: {other:?}"),
            }
        }
    }
    let reader = std::thread::spawn(move || {
        peer.set_read_timeout(Some(LIMIT)).unwrap();
        let mut next = [0usize; FOREIGN + 1];
        let total = (FOREIGN + 1) * PER_SENDER;
        for n in 0..total {
            if n % 97 == 0 {
                // Stall, so the socket fills and senders meet EAGAIN.
                std::thread::sleep(Duration::from_millis(3));
            }
            let frame = read_frame(&mut peer).expect("a frame was lost");
            let sender = frame[0] as usize;
            let seq = u32::from_ne_bytes(frame[1..5].try_into().unwrap()) as usize;
            assert!(sender <= FOREIGN, "a frame from no sender: the stream is corrupt");
            assert_eq!(seq, next[sender], "sender {sender} out of order");
            assert!(frame == frame_for(sender, seq), "sender {sender} frame {seq} corrupt");
            next[sender] += 1;
        }
    });
    // The poll thread's own sender: a job that sends its frames from the
    // poll thread, where sends keep S5's path.
    let poll_proxy = proxy.clone();
    let (poll_done_tx, poll_done_rx) = mpsc::channel::<()>();
    let poll_done_tx = Mutex::new(poll_done_tx);
    pt.add(Arc::new(OneTimeJob::new(Box::new(move || {
        for seq in 0..PER_SENDER {
            let payload = frame_for(POLL_SENDER, seq);
            loop {
                match send(&poll_proxy, &payload) {
                    ChannelError::None => break,
                    ChannelError::WouldBlock => srpc::fiber::this_fiber::sleep_ms(1),
                    other => panic!("send failed: {other:?}"),
                }
            }
        }
        poll_done_tx.lock().unwrap().send(()).unwrap();
    }))));
    let senders: Vec<_> = (0..FOREIGN)
        .map(|sender| {
            let proxy = proxy.clone();
            std::thread::spawn(move || {
                for seq in 0..PER_SENDER {
                    send_retrying(&proxy, &frame_for(sender, seq));
                }
            })
        })
        .collect();
    for sender in senders {
        sender.join().unwrap();
    }
    poll_done_rx.recv_timeout(LIMIT).expect("the poll thread's sender never finished");
    reader.join().unwrap();
    proxy.close();
    drop(proxy);
    pt.shutdown();
}

// The descriptor in this process whose local address is `local`: the
// transport's side of a connection whose std peer is known.
fn find_local_socket(local: std::net::SocketAddr) -> i32 {
    #[repr(C)]
    struct SockaddrIn {
        family: u16,
        port: u16,
        address: u32,
        padding: [u8; 8],
    }
    let std::net::SocketAddr::V4(local) = local else { panic!("IPv4 expected") };
    for entry in std::fs::read_dir("/proc/self/fd").unwrap() {
        let Ok(fd) = entry.unwrap().file_name().to_string_lossy().parse::<i32>() else { continue };
        let mut address = SockaddrIn { family: 0, port: 0, address: 0, padding: [0; 8] };
        let mut length: u32 = std::mem::size_of::<SockaddrIn>() as u32;
        if unsafe { getsockname(fd, (&raw mut address).cast(), &raw mut length) } == 0
            && address.family == 2
            && u16::from_be(address.port) == local.port()
            && address.address == u32::from_ne_bytes(local.ip().octets())
        {
            return fd;
        }
    }
    panic!("no descriptor has local address {local}");
}

// The same under steady backpressure: small socket buffers on both sides
// and a peer that reads in small pieces, so the socket keeps filling and
// emptying and write-through sends are often partial, racing the writer's
// drains of the rest and each other. The stream is checked frame by frame.
#[test]
fn write_through_keeps_frames_whole_under_backpressure() {
    let _serial = serial();
    const SENDERS: usize = 8;
    const PER_SENDER: usize = 250;
    const SOL_SOCKET: i32 = 1;
    const SO_SNDBUF: i32 = 7;
    const SO_RCVBUF: i32 = 8;
    fn frame_for(sender: usize, seq: usize) -> Vec<u8> {
        let len = 5 + (seq * 7919 + sender * 104_729) % (24 << 10);
        let mut payload = pattern(sender * 100_000 + seq, len);
        payload[0] = sender as u8;
        payload[1..5].copy_from_slice(&(seq as u32).to_ne_bytes());
        payload
    }
    let pt = PollThread::create();
    for _round in 0..2 {
        let raw = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let small: i32 = 16 << 10;
        assert_eq!(unsafe { setsockopt(raw.as_raw_fd(), SOL_SOCKET, SO_RCVBUF, (&raw const small).cast(), 4) }, 0);
        let connected = TcpFactory::new(pt.clone()).connect(&raw.local_addr().unwrap().to_string());
        assert_eq!(connected.error, ChannelError::None);
        let (mut peer, client_address) = raw.accept().unwrap();
        let client_fd = find_local_socket(client_address);
        assert_eq!(unsafe { setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, (&raw const small).cast(), 4) }, 0);
        let proxy: Arc<ChannelConnectionProxy> = Arc::new(connected.connection.unwrap());
        let reader = std::thread::spawn(move || {
            peer.set_read_timeout(Some(LIMIT)).unwrap();
            let mut next = [0usize; SENDERS];
            let mut pending: Vec<u8> = Vec::new();
            let mut chunk = vec![0u8; 8 << 10];
            let mut frames = 0usize;
            let mut reads = 0usize;
            while frames < SENDERS * PER_SENDER {
                let n = peer.read(&mut chunk).expect("the stream stalled");
                assert!(n > 0, "the connection closed early");
                pending.extend_from_slice(&chunk[..n]);
                reads += 1;
                if reads.is_multiple_of(8) {
                    std::thread::sleep(Duration::from_micros(20));
                }
                loop {
                    if pending.len() < 4 {
                        break;
                    }
                    let size = u32::from_ne_bytes(pending[..4].try_into().unwrap()) as usize;
                    assert!(size < (1 << 20), "a torn frame: header claims {size} bytes");
                    if pending.len() < 4 + size {
                        break;
                    }
                    let frame: Vec<u8> = pending[4..4 + size].to_vec();
                    pending.drain(..4 + size);
                    let sender = frame[0] as usize;
                    assert!(sender < SENDERS, "a torn frame: no sender {sender}");
                    let seq = u32::from_ne_bytes(frame[1..5].try_into().unwrap()) as usize;
                    assert_eq!(seq, next[sender], "sender {sender} out of order");
                    assert!(frame == frame_for(sender, seq), "sender {sender} frame {seq} torn");
                    next[sender] += 1;
                    frames += 1;
                }
            }
        });
        let senders: Vec<_> = (0..SENDERS)
            .map(|sender| {
                let proxy = proxy.clone();
                std::thread::spawn(move || {
                    for seq in 0..PER_SENDER {
                        let payload = frame_for(sender, seq);
                        let deadline = Instant::now() + LIMIT;
                        loop {
                            match send(&proxy, &payload) {
                                ChannelError::None => break,
                                ChannelError::WouldBlock => {
                                    assert!(Instant::now() < deadline, "refused for good");
                                    std::thread::yield_now();
                                }
                                other => panic!("send failed: {other:?}"),
                            }
                        }
                    }
                })
            })
            .collect();
        for sender in senders {
            sender.join().unwrap();
        }
        reader.join().unwrap();
        proxy.close();
    }
    pt.shutdown();
}

// A connection reset that a foreign sender's own send(2) meets first -- the
// poll thread is held, so neither transport task can see it -- is reported
// as the writer always reported a send failure: once, as ConnectionReset,
// from the poll thread, never from the sender's thread. The sender's send
// consumes the socket's pending error, so the reader then reads a plain EOF.
// Even rounds start the tasks before the reset (the woken writer runs
// first); odd rounds only after it (the reader, spawned first, runs first
// and reads that EOF).
#[test]
fn a_reset_met_by_a_foreign_sender_is_reported_as_a_send_failure() {
    let _serial = serial();
    let pt = PollThread::create();
    for round in 0..10usize {
        let early_release = if round % 2 == 1 { Some(hold_poll_thread(&pt)) } else { None };
        let (mut proxy, peer) = connect_to_raw_peer(&pt);
        if early_release.is_none() {
            std::thread::sleep(Duration::from_millis(2));
        }
        let callback_threads: Arc<Mutex<Vec<std::thread::ThreadId>>> = Arc::new(Mutex::new(Vec::new()));
        let (closed_tx, closed_rx) = mpsc::channel::<ChannelError>();
        let closed_tx = Mutex::new(closed_tx);
        let on_closed_threads = callback_threads.clone();
        proxy.set_on_closed(OnClosedCallback::from_callable(Box::new(move |reason: ChannelError| {
            on_closed_threads.lock().unwrap().push(std::thread::current().id());
            let _ = closed_tx.lock().unwrap().send(reason);
        })));
        let on_error_threads = callback_threads.clone();
        let errors: Arc<Mutex<Vec<(ChannelError, String)>>> = Arc::new(Mutex::new(Vec::new()));
        let error_log = errors.clone();
        proxy.set_on_error(OnErrorCallback::from_callable(Box::new(move |error: ChannelError, what: &str| {
            on_error_threads.lock().unwrap().push(std::thread::current().id());
            error_log.lock().unwrap().push((error, what.to_string()));
        })));
        let release = match early_release {
            Some(release) => release,
            None => hold_poll_thread(&pt),
        };
        // Reset the connection from the peer's side (SO_LINGER, zero timeout).
        #[repr(C)]
        struct Linger {
            onoff: i32,
            linger: i32,
        }
        let linger = Linger { onoff: 1, linger: 0 };
        assert_eq!(unsafe { setsockopt(peer.as_raw_fd(), 1, 13, (&raw const linger).cast(), 8) }, 0);
        drop(peer);
        std::thread::sleep(Duration::from_millis(5));
        // The frame is accepted: the connection is not known to be closed.
        assert_eq!(send(&proxy, &pattern(round, 1000)), ChannelError::None);
        drop(release);
        assert_eq!(closed_rx.recv_timeout(LIMIT).unwrap(), ChannelError::ConnectionReset, "round {round}");
        assert!(closed_rx.recv_timeout(Duration::from_millis(50)).is_err(), "on_closed fired twice");
        let errors = errors.lock().unwrap().clone();
        assert_eq!(errors, vec![(ChannelError::ConnectionReset, "outbound write failed".to_string())], "round {round}");
        let threads = callback_threads.lock().unwrap().clone();
        assert!(!threads.contains(&std::thread::current().id()), "a callback ran on the sending thread");
        proxy.close();
    }
    pt.shutdown();
}

// ---------------------------------------------------------------------------
// The three dispatch modes over the transport

mod rpc_modes {
    use super::*;
    use srpc::client::{deserialize_from, Client, FutureAttr};
    use srpc::reactor::{reactor_spawn_stackless_task_with_result, Reactor};
    use srpc::serializable::{BinaryReadArchive, BinaryWriteArchive, Deserialize, Serialize};
    use srpc::server::{Request, Server, ServerReplyFn, Service, WeakServerConnection};
    use std::ffi::CString;
    use std::future::Future;
    use std::pin::Pin;
    use std::task::{Context, Poll, Waker};

    const FAST_RPC: i32 = 0x00E0_5501;
    const FIBER_RPC: i32 = 0x00E0_5502;
    const STACKLESS_RPC: i32 = 0x00E0_5503;

    // Completes once a helper thread has woken it: its reply is sent from a
    // Lion task, outside the reader's poll.
    struct Deferred {
        done: Arc<AtomicBool>,
        wakes: mpsc::Sender<(Arc<AtomicBool>, Waker)>,
        asked: bool,
    }

    impl Future for Deferred {
        type Output = ();
        fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
            let this = self.get_mut();
            if this.done.load(Ordering::Acquire) {
                return Poll::Ready(());
            }
            if !this.asked {
                this.asked = true;
                this.wakes.send((this.done.clone(), cx.waker().clone())).unwrap();
            }
            Poll::Pending
        }
    }

    struct Modes {
        wakes: Mutex<mpsc::Sender<(Arc<AtomicBool>, Waker)>>,
    }

    fn reply_plus_one(connection: &WeakServerConnection, request: &Request, value: i64) {
        let connection = connection.upgrade().expect("live server connection");
        let writer: ServerReplyFn = Some(Box::new(move |archive: &mut BinaryWriteArchive| {
            Serialize::serialize(&(value + 1), archive);
        }));
        connection.reply(request, 0, writer);
    }

    impl Service for Modes {
        fn __reg_to__(&mut self, server: &mut Server, index: usize) -> i32 {
            assert_eq!(server.reg_fast_rpc(FAST_RPC, index), 0);
            assert_eq!(server.reg_rpc(FIBER_RPC, index), 0);
            // Registered as fast: the handler itself hands off to a stackless
            // task, which is how the async IDL mode dispatches.
            server.reg_fast_rpc(STACKLESS_RPC, index)
        }

        fn __dispatch__(&self, rpc_id: i32, mut request: Box<Request>, connection: WeakServerConnection) {
            let mut value = 0i64;
            let mut archive = BinaryReadArchive::new(unsafe {
                srpc::serializable::make_source_proxy_buffer(&raw mut request.src)
            });
            Deserialize::deserialize(&mut value, &mut archive);
            if rpc_id == FAST_RPC {
                reply_plus_one(&connection, &request, value);
            } else if rpc_id == FIBER_RPC {
                // Resumed later by the driver: the reply wakes the writer
                // from outside the reader's poll.
                srpc::fiber::this_fiber::sleep_ms(1);
                reply_plus_one(&connection, &request, value);
            } else {
                let deferred = Deferred {
                    done: Arc::new(AtomicBool::new(false)),
                    wakes: self.wakes.lock().unwrap().clone(),
                    asked: false,
                };
                let mut parked = Some(request);
                reactor_spawn_stackless_task_with_result(&Reactor::get_reactor(), Box::pin(deferred), move |()| {
                    let request = parked.take().unwrap();
                    reply_plus_one(&connection, &request, value);
                });
            }
        }
    }

    #[test]
    fn fast_fiber_and_stackless_replies_all_reach_the_writer() {
        let _serial = serial();
        let (wakes_tx, wakes_rx) = mpsc::channel::<(Arc<AtomicBool>, Waker)>();
        let waker_thread = std::thread::spawn(move || {
            while let Ok((done, waker)) = wakes_rx.recv() {
                done.store(true, Ordering::Release);
                waker.wake();
            }
        });
        let server_pt = PollThread::create();
        let client_pt = PollThread::create();
        let mut server = Server::new(Some(server_pt.clone()));
        server.reg_service(Box::new(Modes { wakes: Mutex::new(wakes_tx) }));
        assert_eq!(unsafe { server.start(c"127.0.0.1:0".as_ptr()) }, 0);
        let address = CString::new(format!("127.0.0.1:{}", server.get_bound_port())).unwrap();
        let client = Client::create(client_pt.clone());
        assert_eq!(client.connect(address.as_ptr(), true), 0);
        let threads: Vec<_> = (0..4i64)
            .map(|t| {
                let client = client.clone();
                std::thread::spawn(move || {
                    let mut in_flight = std::collections::VecDeque::new();
                    for i in 0..600i64 {
                        let rpc = [FAST_RPC, FIBER_RPC, STACKLESS_RPC][(i % 3) as usize];
                        let value = t * 1_000_000 + i;
                        let future = client
                            .request(rpc, &FutureAttr::default(), |ar: &mut BinaryWriteArchive| Serialize::serialize(&value, ar))
                            .expect("request");
                        in_flight.push_back((future, value));
                        if in_flight.len() >= 32 {
                            let (future, value) = in_flight.pop_front().unwrap();
                            future.wait();
                            assert_eq!(future.get_error_code(), 0);
                            let mut reply = 0i64;
                            deserialize_from(future.get_reply(), &mut reply);
                            assert_eq!(reply, value + 1);
                        }
                    }
                    for (future, value) in in_flight {
                        future.wait();
                        assert_eq!(future.get_error_code(), 0);
                        let mut reply = 0i64;
                        deserialize_from(future.get_reply(), &mut reply);
                        assert_eq!(reply, value + 1);
                    }
                })
            })
            .collect();
        for thread in threads {
            thread.join().unwrap();
        }
        client.close();
        drop(client);
        drop(server);
        client_pt.shutdown();
        server_pt.shutdown();
        waker_thread.join().unwrap();
    }
}
