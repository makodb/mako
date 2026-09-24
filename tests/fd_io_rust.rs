#![allow(unsafe_code)]

use srpc::serializable::{FdSink, FdSource, SinkBase, SourceBase};
use std::fs::File;
use std::io::{Read, Write};
use std::os::fd::{AsRawFd, FromRawFd};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{mpsc, Mutex};
use std::time::{Duration, Instant};

const USER_SIGNAL: i32 = 10;
static SIGNAL_COUNT: AtomicUsize = AtomicUsize::new(0);
static SIGNAL_TEST_LOCK: Mutex<()> = Mutex::new(());

extern "C" {
    fn pipe(descriptors: *mut i32) -> i32;
    fn signal(signal: i32, handler: usize) -> usize;
    fn siginterrupt(signal: i32, interrupt: i32) -> i32;
    fn pthread_self() -> usize;
    fn pthread_kill(thread: usize, signal: i32) -> i32;
    fn gettid() -> i32;
}

extern "C" fn record_signal(_: i32) {
    SIGNAL_COUNT.fetch_add(1, Ordering::Relaxed);
}

struct SignalGuard(usize);

impl SignalGuard {
    fn install() -> Self {
        SIGNAL_COUNT.store(0, Ordering::Relaxed);
        // SAFETY: the handler only updates a lock-free atomic and remains live
        // until every receiving worker has joined.
        let previous = unsafe { signal(USER_SIGNAL, record_signal as *const () as usize) };
        assert_ne!(previous, usize::MAX);
        assert_eq!(unsafe { siginterrupt(USER_SIGNAL, 1) }, 0);
        Self(previous)
    }
}

impl Drop for SignalGuard {
    fn drop(&mut self) {
        unsafe { signal(USER_SIGNAL, self.0) };
    }
}

fn pipe_pair() -> (File, File) {
    let mut descriptors = [-1; 2];
    assert_eq!(unsafe { pipe(descriptors.as_mut_ptr()) }, 0);
    // SAFETY: pipe created two uniquely owned descriptors.
    unsafe {
        (
            File::from_raw_fd(descriptors[0]),
            File::from_raw_fd(descriptors[1]),
        )
    }
}

fn wait_for_io(thread: i32, fd: i32, write: bool) {
    let syscall = if cfg!(target_arch = "aarch64") {
        if write {
            64
        } else {
            63
        }
    } else if write {
        1
    } else {
        0
    };
    let prefix = format!("{syscall} {:#x} ", fd);
    let deadline = Instant::now() + Duration::from_secs(3);
    loop {
        let state = std::fs::read_to_string(format!("/proc/self/task/{thread}/syscall"))
            .expect("worker must remain alive until its blocked I/O completes");
        if state.starts_with(&prefix) {
            return;
        }
        assert!(
            Instant::now() < deadline,
            "worker did not block in expected I/O: {state}"
        );
        std::thread::sleep(Duration::from_millis(1));
    }
}

fn interrupt_thread(thread: usize) {
    assert_eq!(unsafe { pthread_kill(thread, USER_SIGNAL) }, 0);
    let deadline = Instant::now() + Duration::from_secs(3);
    while SIGNAL_COUNT.load(Ordering::Relaxed) == 0 {
        assert!(
            Instant::now() < deadline,
            "worker did not receive the signal"
        );
        std::thread::yield_now();
    }
}

#[test]
fn canonical_fd_read_retries_eintr_and_combines_partial_reads() {
    let _lock = SIGNAL_TEST_LOCK.lock().unwrap();
    let _signal = SignalGuard::install();
    let (reader, mut writer) = pipe_pair();
    let (started_tx, started_rx) = mpsc::channel();
    let worker = std::thread::spawn(move || {
        let fd = reader.as_raw_fd();
        started_tx
            .send((unsafe { gettid() }, unsafe { pthread_self() }, fd))
            .unwrap();
        let mut source = FdSource::new(fd);
        let mut bytes = [0u8; 7];
        let count = unsafe { source.read_bytes(bytes.as_mut_ptr(), bytes.len()) };
        (count, bytes)
    });
    let (tid, thread, fd) = started_rx.recv_timeout(Duration::from_secs(3)).unwrap();
    wait_for_io(tid, fd, false);
    interrupt_thread(thread);
    wait_for_io(tid, fd, false);
    writer.write_all(b"abc").unwrap();
    // With only three of the requested seven bytes available, the next read
    // must block again instead of returning the partial result to the caller.
    std::thread::sleep(Duration::from_millis(10));
    wait_for_io(tid, fd, false);
    writer.write_all(b"defg").unwrap();
    let (count, bytes) = worker.join().unwrap();
    assert_eq!(count, 7);
    assert_eq!(&bytes, b"abcdefg");
}

#[test]
fn canonical_fd_write_continues_after_an_interrupted_partial_write() {
    let _lock = SIGNAL_TEST_LOCK.lock().unwrap();
    let _signal = SignalGuard::install();
    let (mut reader, writer) = pipe_pair();
    let payload: Vec<u8> = (0..1024 * 1024).map(|index| (index % 251) as u8).collect();
    let expected = payload.clone();
    let (started_tx, started_rx) = mpsc::channel();
    let worker = std::thread::spawn(move || {
        let fd = writer.as_raw_fd();
        started_tx
            .send((unsafe { gettid() }, unsafe { pthread_self() }, fd))
            .unwrap();
        let mut sink = FdSink::new(fd);
        unsafe { sink.write_bytes(payload.as_ptr(), payload.len()) };
    });
    let (tid, thread, fd) = started_rx.recv_timeout(Duration::from_secs(3)).unwrap();
    wait_for_io(tid, fd, true);
    // The pipe contains a prefix, and its capacity is smaller than the write.
    // Interrupting this blocked syscall returns that positive partial count.
    interrupt_thread(thread);
    let mut received = Vec::new();
    reader.read_to_end(&mut received).unwrap();
    worker.join().unwrap();
    assert_eq!(received, expected);
}

#[test]
fn canonical_fd_read_returns_the_short_count_at_eof() {
    let (reader, mut writer) = pipe_pair();
    writer.write_all(b"short").unwrap();
    drop(writer);
    let mut source = FdSource::new(reader.as_raw_fd());
    let mut bytes = [0u8; 12];
    assert_eq!(
        unsafe { source.read_bytes(bytes.as_mut_ptr(), bytes.len()) },
        5
    );
    assert_eq!(&bytes[..5], b"short");
    assert_eq!(
        unsafe { source.read_bytes(bytes.as_mut_ptr(), bytes.len()) },
        0
    );
}
