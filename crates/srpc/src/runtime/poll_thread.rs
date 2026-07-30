//! The poll thread — the port of `pollworker_poll_loop` and its command
//! channel in `src/rrr/reactor/reactor.cpp`.
//!
//! ## Why [`Pollable`] takes `&self`
//!
//! The single largest design decision in the transport port. A user
//! thread calls `send_frame` on the *same object* the poll thread is
//! reading from, so a `&mut self` interface would need external
//! synchronisation at every call site and would fight the borrow
//! checker at each one. The C++ resolves this by making everything
//! `const` and mutating through members; the Rust equivalent is `&self`
//! plus interior mutability inside the implementor, which keeps the
//! shared-object shape and lets each implementor pick its own locking.
//!
//! ## The loop, and why it looks wasteful
//!
//! ```text
//! loop {
//!     epoll_wait(1 ms)          // no wakeup fd exists
//!     dispatch READ, WRITE, ERROR   // in that order, per event
//!     drain commands (non-blocking)
//!     apply deferred write re-arms
//! }
//! ```
//!
//! There is no eventfd anywhere in rrr, so a command posted from
//! another thread waits up to a millisecond, and an idle process still
//! wakes a thousand times a second. Both are reproduced deliberately:
//! the baseline shows depth-1 throughput is *entirely* this path, so
//! adding a wakeup fd would change the number for a reason that has
//! nothing to do with Rust. See `docs/dev/srpc_rpcbench_baseline.md`.

use crate::runtime::epoll::{Epoll, PollMode, Readiness, POLL_TIMEOUT_MS};
use std::collections::HashMap;
use std::sync::mpsc::{channel, Receiver, Sender};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;

/// Something the poll thread watches.
///
/// Every method takes `&self` — see the module note. Implementors are
/// shared across threads, so they must be `Send + Sync`.
pub trait Pollable: Send + Sync {
    fn fd(&self) -> i32;

    /// Interest set at registration time.
    fn poll_mode(&self) -> PollMode;

    /// Readable. Under edge-triggering this MUST drain until the socket
    /// reports `EAGAIN`, or the edge is lost and the connection stalls
    /// with data pending.
    fn handle_read(&self);

    /// Writable. Returns the new interest set: [`PollMode::READ`] once
    /// the outbound queue has fully drained (dropping `EPOLLOUT`), or
    /// [`PollMode::NO_CHANGE`] while bytes remain.
    ///
    /// That return value plus the dedupe in [`Epoll::update_mode`] are
    /// jointly what re-arms the edge-triggered write.
    fn handle_write(&self) -> PollMode;

    /// Error or peer hangup. Dispatched AFTER read, so data that
    /// arrived alongside a half-close is parsed before teardown.
    fn handle_error(&self);
}

enum Command {
    Add(Arc<dyn Pollable>),
    UpdateMode(i32, PollMode),
    Remove(i32),
    Shutdown,
}

/// Handle to a running poll thread.
pub struct PollThread {
    tx: Mutex<Sender<Command>>,
    join: Mutex<Option<JoinHandle<()>>>,
}

impl PollThread {
    /// Start a poll thread.
    pub fn start() -> Arc<PollThread> {
        let (tx, rx) = channel();
        let join = std::thread::spawn(move || poll_loop(rx));
        Arc::new(PollThread {
            tx: Mutex::new(tx),
            join: Mutex::new(Some(join)),
        })
    }

    /// Register a pollable. Takes effect after the next `epoll_wait`
    /// returns — up to [`POLL_TIMEOUT_MS`] away.
    pub fn add(&self, p: Arc<dyn Pollable>) {
        self.post(Command::Add(p));
    }

    /// Change a registered fd's interest set.
    pub fn update_mode(&self, fd: i32, mode: PollMode) {
        self.post(Command::UpdateMode(fd, mode));
    }

    pub fn remove(&self, fd: i32) {
        self.post(Command::Remove(fd));
    }

    /// Stop the loop and join. Idempotent.
    pub fn shutdown(&self) {
        self.post(Command::Shutdown);
        let handle = self.join.lock().unwrap().take();
        if let Some(h) = handle {
            let _ = h.join();
        }
    }

    /// A send failure means the loop has already exited; the command is
    /// dropped rather than reported, matching the C++, where posting to
    /// a shut-down worker is a no-op.
    fn post(&self, cmd: Command) {
        let tx = self.tx.lock().unwrap();
        let _ = tx.send(cmd);
    }
}

impl Drop for PollThread {
    fn drop(&mut self) {
        // Best effort: a PollThread dropped without shutdown() would
        // otherwise leak the thread.
        let _ = self.tx.lock().map(|tx| tx.send(Command::Shutdown));
        if let Ok(mut g) = self.join.lock() {
            if let Some(h) = g.take() {
                let _ = h.join();
            }
        }
    }
}

fn poll_loop(rx: Receiver<Command>) {
    let mut ep = match Epoll::new() {
        Ok(e) => e,
        Err(_) => return,
    };
    let mut pollables: HashMap<i32, Arc<dyn Pollable>> = HashMap::new();
    // Mode changes discovered while dispatching, applied after the
    // sweep so the map is not mutated mid-iteration.
    let mut deferred: Vec<(i32, PollMode)> = Vec::new();

    loop {
        deferred.clear();
        let mut ready: Vec<(i32, Readiness)> = Vec::new();
        if ep
            .wait(POLL_TIMEOUT_MS, |fd, r| ready.push((fd, r)))
            .is_err()
        {
            break;
        }

        let mut i = 0;
        while i < ready.len() {
            let fd = ready[i].0;
            let r = ready[i].1;
            i += 1;
            // A stale event for an fd already removed resolves to
            // "no such pollable" rather than a dangling handle —
            // the reason the C++ keys by fd instead of by pointer.
            // Spelled as if-let rather than `match { None => continue }`:
            // a `continue` inside a match ARM leaves the loop it belongs
            // to once the match lowers to an expression.
            let found = pollables.get(&fd);
            if found.is_none() {
                continue;
            }
            let p = Arc::clone(found.unwrap());
            // READ, then WRITE, then ERROR: data delivered alongside a
            // half-close is parsed before the connection is torn down.
            if r.readable() {
                p.handle_read();
            }
            if r.writable() {
                let next = p.handle_write();
                if !next.is_no_change() {
                    deferred.push((fd, next));
                }
            }
            if r.error() {
                p.handle_error();
                deferred.retain(|(f, _)| *f != fd);
                let _ = ep.remove(fd);
                pollables.remove(&fd);
            }
        }

        let mut k = 0;
        while k < deferred.len() {
            let (fd, mode) = deferred[k];
            k += 1;
            if pollables.contains_key(&fd) {
                let _ = ep.update_mode(fd, mode);
            }
        }

        // Non-blocking drain, exactly as the C++ try_recv sweep: there
        // is no wakeup fd, so this is why the timeout above is 1 ms.
        loop {
            match rx.try_recv() {
                Ok(Command::Add(p)) => {
                    let fd = p.fd();
                    if ep.add(fd, p.poll_mode()).is_ok() {
                        pollables.insert(fd, p);
                    }
                }
                Ok(Command::UpdateMode(fd, mode)) => {
                    if pollables.contains_key(&fd) {
                        let _ = ep.update_mode(fd, mode);
                    }
                }
                Ok(Command::Remove(fd)) => {
                    let _ = ep.remove(fd);
                    pollables.remove(&fd);
                }
                Ok(Command::Shutdown) => return,
                // Disconnected means every sender is gone, so no
                // further work can arrive.
                Err(std::sync::mpsc::TryRecvError::Disconnected) => return,
                Err(std::sync::mpsc::TryRecvError::Empty) => break,
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Read, Write};
    use std::net::{TcpListener, TcpStream};
    use std::os::fd::AsRawFd;
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::time::{Duration, Instant};

    /// Records what the loop dispatched, and drains reads so the
    /// edge-triggered registration stays armed.
    struct Recorder {
        sock: TcpStream,
        reads: AtomicUsize,
        writes: AtomicUsize,
        errors: AtomicUsize,
        bytes: Mutex<Vec<u8>>,
        mode: PollMode,
    }

    impl Recorder {
        fn new(sock: TcpStream, mode: PollMode) -> Arc<Recorder> {
            sock.set_nonblocking(true).unwrap();
            Arc::new(Recorder {
                sock,
                reads: AtomicUsize::new(0),
                writes: AtomicUsize::new(0),
                errors: AtomicUsize::new(0),
                bytes: Mutex::new(Vec::new()),
                mode,
            })
        }
        fn collected(&self) -> Vec<u8> {
            self.bytes.lock().unwrap().clone()
        }
    }

    impl Pollable for Recorder {
        fn fd(&self) -> i32 {
            self.sock.as_raw_fd()
        }
        fn poll_mode(&self) -> PollMode {
            self.mode
        }
        fn handle_read(&self) {
            self.reads.fetch_add(1, Ordering::Relaxed);
            // Drain to EAGAIN — mandatory under edge-triggering.
            let mut buf = [0u8; 256];
            loop {
                match (&self.sock).read(&mut buf) {
                    Ok(0) => break,
                    Ok(n) => self.bytes.lock().unwrap().extend_from_slice(&buf[..n]),
                    Err(_) => break,
                }
            }
        }
        fn handle_write(&self) -> PollMode {
            self.writes.fetch_add(1, Ordering::Relaxed);
            // Nothing queued, so the queue is "drained": drop EPOLLOUT.
            PollMode::READ
        }
        fn handle_error(&self) {
            self.errors.fetch_add(1, Ordering::Relaxed);
        }
    }

    fn wait_until(mut cond: impl FnMut() -> bool, what: &str) {
        let deadline = Instant::now() + Duration::from_secs(5);
        while Instant::now() < deadline {
            if cond() {
                return;
            }
            std::thread::sleep(Duration::from_millis(2));
        }
        panic!("timed out waiting for {what}");
    }

    fn pair() -> (TcpStream, TcpStream) {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = l.local_addr().unwrap();
        let client = TcpStream::connect(addr).unwrap();
        let (server, _) = l.accept().unwrap();
        (client, server)
    }

    #[test]
    fn delivers_reads_to_the_registered_pollable() {
        let (client, server) = pair();
        let rec = Recorder::new(server, PollMode::READ);
        let pt = PollThread::start();
        pt.add(rec.clone());

        (&client).write_all(b"hello poll thread").unwrap();
        wait_until(
            || rec.collected() == b"hello poll thread",
            "the payload to arrive",
        );
        assert!(rec.reads.load(Ordering::Relaxed) >= 1);

        pt.shutdown();
    }

    #[test]
    fn dispatches_writable_and_applies_the_returned_mode() {
        let (_client, server) = pair();
        let rec = Recorder::new(server, PollMode::READ_WRITE);
        let pt = PollThread::start();
        pt.add(rec.clone());

        // A connected socket is immediately writable; handle_write
        // returns READ, which drops EPOLLOUT — so the writable
        // dispatch must not repeat forever.
        wait_until(|| rec.writes.load(Ordering::Relaxed) >= 1, "a write event");
        let after_first = rec.writes.load(Ordering::Relaxed);
        std::thread::sleep(Duration::from_millis(100));
        let later = rec.writes.load(Ordering::Relaxed);
        assert!(
            later - after_first <= 1,
            "EPOLLOUT should have been dropped, got {} more writes",
            later - after_first
        );

        pt.shutdown();
    }

    #[test]
    fn peer_hangup_reaches_handle_error_and_deregisters() {
        let (client, server) = pair();
        let rec = Recorder::new(server, PollMode::READ);
        let pt = PollThread::start();
        pt.add(rec.clone());

        drop(client);
        wait_until(
            || rec.errors.load(Ordering::Relaxed) >= 1,
            "the hangup to be reported",
        );

        // Deregistered: the error count stops climbing rather than
        // spinning on a permanently-ready fd.
        let seen = rec.errors.load(Ordering::Relaxed);
        std::thread::sleep(Duration::from_millis(100));
        assert_eq!(
            rec.errors.load(Ordering::Relaxed),
            seen,
            "a hung-up fd must be deregistered, not re-dispatched"
        );

        pt.shutdown();
    }

    #[test]
    fn data_arriving_with_a_hangup_is_read_before_teardown() {
        // The dispatch order (READ before ERROR) is why a peer that
        // writes and immediately closes does not lose its last message.
        let (client, server) = pair();
        let rec = Recorder::new(server, PollMode::READ);
        let pt = PollThread::start();

        (&client).write_all(b"last words").unwrap();
        drop(client);
        pt.add(rec.clone());

        wait_until(|| rec.errors.load(Ordering::Relaxed) >= 1, "the hangup");
        assert_eq!(
            rec.collected(),
            b"last words",
            "data must be parsed before teardown"
        );

        pt.shutdown();
    }

    #[test]
    fn remove_stops_delivery() {
        let (client, server) = pair();
        let rec = Recorder::new(server, PollMode::READ);
        let pt = PollThread::start();
        pt.add(rec.clone());

        (&client).write_all(b"first").unwrap();
        wait_until(|| !rec.collected().is_empty(), "the first payload");

        pt.remove(rec.fd());
        std::thread::sleep(Duration::from_millis(50));
        let before = rec.reads.load(Ordering::Relaxed);
        (&client).write_all(b"second").unwrap();
        std::thread::sleep(Duration::from_millis(100));
        assert_eq!(
            rec.reads.load(Ordering::Relaxed),
            before,
            "a removed pollable must stop receiving events"
        );

        pt.shutdown();
    }

    #[test]
    fn shutdown_is_idempotent_and_joins() {
        let pt = PollThread::start();
        pt.shutdown();
        pt.shutdown();
    }

    #[test]
    fn several_connections_are_multiplexed() {
        let pt = PollThread::start();
        let mut peers = Vec::new();
        let mut recs = Vec::new();
        let mut i = 0;
        while i < 8 {
            let (client, server) = pair();
            let rec = Recorder::new(server, PollMode::READ);
            pt.add(rec.clone());
            recs.push(rec);
            peers.push(client);
            i += 1;
        }
        let mut k = 0;
        while k < peers.len() {
            let msg = format!("conn{k}");
            (&peers[k]).write_all(msg.as_bytes()).unwrap();
            k += 1;
        }
        let mut j = 0;
        while j < recs.len() {
            let expect = format!("conn{j}").into_bytes();
            let rec = recs[j].clone();
            wait_until(
                move || rec.collected() == expect,
                "each connection's payload",
            );
            j += 1;
        }
        pt.shutdown();
    }
}
