//! Crate-internal check of the adaptive cork (lion/s5-cork): foreign sends
//! that follow the connection's last send(2) within kTcpWriteThroughIdleUs
//! are queued for the writer task, not written through, so a burst leaves in
//! fewer send(2) calls than frames. It reads the connection's recorded send
//! time, which the public surface cannot see, so each round can tell exactly
//! whether its burst stayed inside the interval, whatever the interval.

#![allow(unsafe_code)]

use super::*;
use crate::misc::OneTimeJob;
use std::io::Read;
use std::os::fd::IntoRawFd;
use std::time::Duration;

unsafe extern "C" {
    fn setsockopt(fd: i32, level: i32, name: i32, value: *const core::ffi::c_void, length: u32) -> i32;
    fn getsockopt(fd: i32, level: i32, name: i32, value: *mut core::ffi::c_void, length: *mut u32) -> i32;
}

const LIMIT: Duration = Duration::from_secs(20);

// Data segments the socket has sent (tcp_info.tcpi_segs_out). With
// TCP_NODELAY set and no data flowing the other way, every send(2) that
// carries bytes emits at least one segment, so the count bounds the number
// of such sends from above.
fn segments_out(fd: i32) -> u32 {
    const IPPROTO_TCP: i32 = 6;
    const TCP_INFO: i32 = 11;
    let mut info = [0u8; 256];
    let mut length: u32 = info.len() as u32;
    assert_eq!(unsafe { getsockopt(fd, IPPROTO_TCP, TCP_INFO, info.as_mut_ptr().cast(), &raw mut length) }, 0);
    assert!(length >= 144, "tcp_info too short: {length}");
    u32::from_ne_bytes(info[136..140].try_into().unwrap())
}

// Runs `f` on `pt`'s thread (a job) and waits for it.
fn run_on_poll_thread(pt: &Arc<PollThread>, f: impl FnOnce() + Send + 'static) {
    let (done_tx, done_rx) = std::sync::mpsc::channel::<()>();
    let slot = std::sync::Mutex::new(Some((f, done_tx)));
    pt.add(Arc::new(OneTimeJob::new(Box::new(move || {
        if let Some((f, done_tx)) = slot.lock().unwrap().take() {
            f();
            done_tx.send(()).unwrap();
        }
    }))));
    done_rx.recv_timeout(LIMIT).expect("the poll thread never ran the job");
}

// Holds `pt`'s thread inside a job until the returned sender is dropped.
fn hold_poll_thread(pt: &Arc<PollThread>) -> std::sync::mpsc::Sender<()> {
    let (entered_tx, entered_rx) = std::sync::mpsc::channel::<()>();
    let (release_tx, release_rx) = std::sync::mpsc::channel::<()>();
    let slot = std::sync::Mutex::new((entered_tx, release_rx));
    pt.add(Arc::new(OneTimeJob::new(Box::new(move || {
        let guard = slot.lock().unwrap();
        guard.0.send(()).unwrap();
        let _ = guard.1.recv_timeout(LIMIT);
    }))));
    entered_rx.recv_timeout(LIMIT).unwrap();
    release_tx
}

fn read_frame(stream: &mut std::net::TcpStream) -> std::io::Result<Vec<u8>> {
    let mut header = [0u8; 4];
    stream.read_exact(&mut header)?;
    let mut payload = vec![0u8; u32::from_ne_bytes(header) as usize];
    stream.read_exact(&mut payload)?;
    Ok(payload)
}

// The bytes still queued in the outbound buffer (the retired pollable
// content_size counted these plus the undecoded inbound bytes, which the
// cork's sends never touch).
fn queued_bytes(conn: &TcpConnection) -> usize {
    conn.outbound_.lock().unwrap().len()
}

fn send(conn: &TcpConnection, payload: &[u8]) -> ChannelError {
    let frame = ChannelFrame { payload: payload.as_ptr(), size: payload.len() };
    // SAFETY: the frame points at the live payload for this call.
    unsafe { conn.send_frame(&frame) }
}

// With the poll thread held: the first frame of a burst on an idle
// connection is written through by the sender; the rest, sent while the
// connection's last send is younger than the interval, wait in the outbound
// buffer and leave in the writer's one drain after the release.
#[test]
fn a_burst_inside_the_cork_interval_is_batched() {
    const FRAMES: usize = 4;
    const IPPROTO_TCP: i32 = 6;
    const TCP_NODELAY: i32 = 1;
    let pt = PollThread::create();
    let mut batched_rounds = 0usize;
    let mut attempts = 0usize;
    while batched_rounds < 5 {
        attempts += 1;
        assert!(attempts <= 200, "no burst stayed inside {kTcpWriteThroughIdleUs} us in 200 attempts");
        let raw = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let client = std::net::TcpStream::connect(raw.local_addr().unwrap()).unwrap();
        let (mut peer, _) = raw.accept().unwrap();
        client.set_nonblocking(true).unwrap();
        let one: i32 = 1;
        let fd = client.into_raw_fd();
        assert_eq!(unsafe { setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (&raw const one).cast(), 4) }, 0);
        // SAFETY: into_raw_fd transferred the connected descriptor's ownership.
        let mut conn = Arc::new(unsafe { TcpConnection::new(fd, "cork-peer".to_string()) });
        Arc::get_mut(&mut conn).unwrap().set_poll_thread(pt.clone());
        // Start the transport tasks (the hand-over job) and let them park,
        // then stay idle past the interval.
        tcpconn_attach(&conn);
        run_on_poll_thread(&pt, || ());
        std::thread::sleep(Duration::from_millis(2));
        let release = hold_poll_thread(&pt);
        let payloads: Vec<Vec<u8>> = (0..FRAMES).map(|i| vec![(attempts * 8 + i) as u8; 16 + i]).collect();
        let segments_before = segments_out(fd);
        assert_eq!(send(&conn, &payloads[0]), ChannelError::None);
        let first_send_us: u64 = conn.last_send_us_.load(Ordering::Relaxed);
        assert!(first_send_us != 0, "the first frame's send(2) recorded no send time");
        assert_eq!(queued_bytes(&conn), 0, "the idle connection's first frame was not written through");
        // Each later frame's cork decision read the clock before `now` below,
        // so a frame whose call returned inside the interval was decided
        // inside it, and must have been queued.
        let mut queued_so_far: usize = 0;
        let mut inside: bool = true;
        let mut burst_end_us: u64 = first_send_us;
        for payload in &payloads[1..] {
            assert_eq!(send(&conn, payload), ChannelError::None);
            burst_end_us = crate::basetypes::Time::now(true);
            if burst_end_us < first_send_us + kTcpWriteThroughIdleUs {
                queued_so_far += 4 + payload.len();
                assert_eq!(queued_bytes(&conn), queued_so_far, "a frame inside the interval was written through");
            } else {
                inside = false;
            }
        }
        peer.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
        assert_eq!(read_frame(&mut peer).unwrap(), payloads[0]);
        let queued: usize = payloads[1..].iter().map(|p| 4 + p.len()).sum();
        if inside {
            assert_eq!(queued_bytes(&conn), queued, "a frame inside the interval was written through");
            peer.set_read_timeout(Some(Duration::from_millis(50))).unwrap();
            let mut probe = [0u8; 1];
            assert!(peer.read(&mut probe).is_err(), "a corked frame reached the peer before the writer ran");
            assert_eq!(segments_out(fd) - segments_before, 1);
        }
        drop(release);
        peer.set_read_timeout(Some(LIMIT)).unwrap();
        for payload in &payloads[1..] {
            assert_eq!(read_frame(&mut peer).expect("a corked frame was never sent"), *payload);
        }
        let segments: u32 = segments_out(fd) - segments_before;
        if inside {
            assert!(segments < FRAMES as u32, "{FRAMES} frames left in {segments} segments: not batched");
            println!(
                "cork: {FRAMES} frames, burst {} us after the first send, {segments} segments",
                burst_end_us - first_send_us
            );
            batched_rounds += 1;
        }
        conn.close();
    }
    pt.shutdown();
}
