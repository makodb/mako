//! Live loopback proof for the S3 transport: a real socket, a real
//! poll thread, real frames.
//!
//! The unit tests around `TcpConnection` check the ladders in
//! isolation, which cannot catch the two failures that actually matter
//! here — a read pump that stops draining (losing the edge, so the
//! connection stalls with data pending) and a frame decode that hands
//! back the wrong bytes. Both need a socket with data on it.
//!
//! The peer is `std::net`, deliberately: it is `cfg(test)` code that
//! never translates, so using it here costs nothing on the C++ side
//! while giving an independent implementation to check against. A bug
//! in our own listener cannot mask a bug in our own connection.

use srpc::rpc::ChannelError;
use srpc::runtime::poll_thread::PollThread;
use srpc::runtime::tcp::{TcpConnection, TcpFactory};
use srpc::wire::frame::encode_into;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

/// Frames collected by the poll thread, as (declared payload size,
/// payload) — the size is kept so a header/body disagreement shows up
/// rather than being silently papered over by `payload.len()`.
type Collected = Arc<Mutex<Vec<(i32, Vec<u8>)>>>;

fn frame_bytes(payload: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    assert!(encode_into(&mut out, payload, false), "encode failed");
    out
}

/// Spin until `cond` holds or the deadline passes. Returns whether it
/// held — never panics itself, so callers can report what was actually
/// seen.
fn wait_until(timeout: Duration, mut cond: impl FnMut() -> bool) -> bool {
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        if cond() {
            return true;
        }
        std::thread::sleep(Duration::from_millis(2));
    }
    cond()
}

/// Accept one connection on an ephemeral port; returns (addr, join).
fn serve_once(
    handler: impl FnOnce(TcpStream) + Send + 'static,
) -> (String, std::thread::JoinHandle<()>) {
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind");
    let addr = listener.local_addr().expect("local_addr").to_string();
    let join = std::thread::spawn(move || {
        let (stream, _) = listener.accept().expect("accept");
        handler(stream);
    });
    (addr, join)
}

fn connect_and_register(addr: &str) -> (Arc<PollThread>, Arc<TcpConnection>, Collected) {
    let factory = TcpFactory::new();
    let conn = Arc::new(factory.connect(addr).expect("connect should succeed"));

    let collected: Collected = Arc::new(Mutex::new(Vec::new()));
    let sink = Arc::clone(&collected);
    conn.set_on_frame(Box::new(move |hdr, payload| {
        sink.lock()
            .unwrap()
            .push((hdr.payload_size, payload.to_vec()));
    }));

    let poll = PollThread::start();
    conn.attach_poll_thread(&poll);
    poll.add(Arc::clone(&conn) as Arc<dyn srpc::runtime::poll_thread::Pollable>);
    (poll, conn, collected)
}

#[test]
fn connects_sends_and_receives_a_frame() {
    let payload = b"hello frame".to_vec();
    let echo = payload.clone();
    let (addr, join) = serve_once(move |mut stream| {
        // Read the client's frame, then write one back.
        let mut buf = [0u8; 256];
        let n = stream.read(&mut buf).expect("server read");
        assert!(n > 0);
        stream.write_all(&frame_bytes(&echo)).expect("server write");
        stream.flush().ok();
        // Hold the connection open until the client has had time to
        // read; dropping here would race the reply with a FIN.
        std::thread::sleep(Duration::from_millis(200));
    });

    let (poll, conn, collected) = connect_and_register(&addr);

    assert_eq!(conn.send_frame(frame_bytes(b"ping")), ChannelError::None);

    let got = wait_until(Duration::from_secs(5), || {
        !collected.lock().unwrap().is_empty()
    });
    let frames = collected.lock().unwrap().clone();
    assert!(got, "no frame arrived; collected {frames:?}");
    assert_eq!(frames.len(), 1);
    assert_eq!(frames[0].1, payload, "payload round-tripped wrong");

    conn.close();
    poll.shutdown();
    join.join().expect("server thread");
}

#[test]
fn drains_a_burst_larger_than_one_recv() {
    // The edge-triggered read must keep draining past a single recv().
    // A pump that stops early leaves frames buffered in the kernel with
    // no further edge to wake it, and the test hangs rather than
    // failing loudly — which is exactly why the deadline is here.
    const FRAMES: usize = 4000;
    let (addr, join) = serve_once(move |mut stream| {
        let mut blob = Vec::new();
        for i in 0..FRAMES {
            // ~200 bytes each => ~800 KB total, far past the 64 KiB
            // recv scratch, so the drain loop must go around many times.
            let body = vec![(i % 251) as u8; 200];
            blob.extend_from_slice(&frame_bytes(&body));
        }
        stream.write_all(&blob).expect("server write burst");
        stream.flush().ok();
        std::thread::sleep(Duration::from_millis(500));
    });

    let (poll, conn, collected) = connect_and_register(&addr);

    let all = wait_until(Duration::from_secs(20), || {
        collected.lock().unwrap().len() >= FRAMES
    });
    let n = collected.lock().unwrap().len();
    assert!(
        all,
        "drained only {n} of {FRAMES} frames — the read edge was lost"
    );

    // Spot-check that framing stayed aligned across the whole burst:
    // a single mis-sized frame would desynchronize everything after it.
    let frames = collected.lock().unwrap();
    for (i, (declared, body)) in frames.iter().enumerate().take(FRAMES) {
        assert_eq!(body.len(), 200, "frame {i} had the wrong length");
        assert_eq!(
            *declared as usize,
            body.len(),
            "frame {i} header/body disagree"
        );
        assert_eq!(body[0], (i % 251) as u8, "frame {i} carried the wrong body");
    }
    drop(frames);

    conn.close();
    poll.shutdown();
    join.join().expect("server thread");
}

#[test]
fn peer_close_reports_once_with_no_error() {
    let (addr, join) = serve_once(|stream| {
        // Close immediately: an orderly FIN with no data.
        drop(stream);
    });

    let factory = TcpFactory::new();
    let conn = Arc::new(factory.connect(&addr).expect("connect"));
    let hits = Arc::new(AtomicUsize::new(0));
    let cause = Arc::new(Mutex::new(None));
    let seen = Arc::clone(&hits);
    let seen_cause = Arc::clone(&cause);
    conn.set_on_closed(Box::new(move |c| {
        seen.fetch_add(1, Ordering::SeqCst);
        *seen_cause.lock().unwrap() = Some(c);
    }));

    let poll = PollThread::start();
    poll.add(Arc::clone(&conn) as Arc<dyn srpc::runtime::poll_thread::Pollable>);

    let closed = wait_until(Duration::from_secs(5), || conn.is_closed());
    assert!(closed, "peer close was not observed");
    assert_eq!(hits.load(Ordering::SeqCst), 1, "close fired more than once");
    assert_eq!(
        *cause.lock().unwrap(),
        Some(ChannelError::None),
        "an orderly peer close is not an error"
    );

    poll.shutdown();
    join.join().expect("server thread");
}

#[test]
fn a_large_write_completes_across_partial_sends() {
    // Backpressure, made adversarial on purpose: the peer accepts the
    // connection and then REFUSES to read for a while, so the socket
    // buffer fills and `send` genuinely returns EAGAIN with bytes still
    // queued. Finishing then depends entirely on the write interest
    // being re-armed — the failure the C++ records as a 100-thread
    // wedge. An always-draining peer never reaches this state, so the
    // stall would go unnoticed.
    const BODY: usize = 4 * 1024 * 1024;
    let received = Arc::new(AtomicUsize::new(0));
    let counter = Arc::clone(&received);
    let (addr, join) = serve_once(move |mut stream| {
        // Let the sender fill the pipe and block.
        std::thread::sleep(Duration::from_millis(400));
        let mut buf = vec![0u8; 64 * 1024];
        let mut total = 0usize;
        loop {
            match stream.read(&mut buf) {
                Ok(0) => break,
                Ok(n) => {
                    total += n;
                    counter.store(total, Ordering::SeqCst);
                    if total >= BODY {
                        break;
                    }
                }
                Err(_) => break,
            }
        }
        counter.store(total, Ordering::SeqCst);
    });

    let (poll, conn, _collected) = connect_and_register(&addr);

    let body = vec![7u8; BODY];
    let framed = frame_bytes(&body);
    let framed_len = framed.len();
    let rc = conn.send_frame(framed);
    assert!(
        rc == ChannelError::None || rc == ChannelError::WouldBlock,
        "unexpected send result {rc:?}"
    );

    let done = wait_until(Duration::from_secs(30), || {
        received.load(Ordering::SeqCst) >= framed_len
    });
    assert!(
        done,
        "peer saw {} of {} bytes — the write never finished draining, \
         so the write interest was not re-armed",
        received.load(Ordering::SeqCst),
        framed_len
    );

    conn.close();
    poll.shutdown();
    join.join().expect("server thread");
}
