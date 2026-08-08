//! TCP connections: the read/write pumps and the connect ladder.
//!
//! A [`TcpConnection`] is a [`Pollable`] owned by the poll thread and
//! shared with user threads, which call [`TcpConnection::send_frame`]
//! concurrently. Everything therefore hangs off `&self` with interior
//! mutability — see the note on [`Pollable`].
//!
//! ## The read pump drains, then decodes
//!
//! Under edge triggering a partial read loses the edge, so
//! [`TcpConnection::handle_read`] recv()s until the socket reports
//! `EAGAIN` (or returns short, which means the same thing without the
//! extra syscall), and only then decodes whole frames out of the
//! buffer. That order matters: decoding between recv()s would hand
//! frames to the callback while the socket still has data pending, and
//! a callback that blocks would strand it.
//!
//! ## Frames are handed out by reference
//!
//! The frame callback receives a `&[u8]` aliasing the reader's own
//! buffer — no copy per frame. That is why it is a callback rather than
//! a returned value: the borrow cannot outlive the decode step, and the
//! reader must be free to compact afterwards. The obvious alternative,
//! returning an owned `Vec` per frame, is a per-request allocation on
//! the hottest path in the system.

use crate::rpc::ChannelError;
use crate::runtime::epoll::PollMode;
use crate::runtime::poll_thread::{PollThread, Pollable};
use crate::sys;
use crate::wire::frame::{FrameHeader, FrameReader};
use std::sync::atomic::{AtomicBool, AtomicI32, Ordering};
use std::sync::{Arc, Mutex, Weak};

/// Bytes per `recv(2)`. Matches the C++ `kRecvScratchBytes`; a short
/// read against this is the drain-loop's exit condition, so changing it
/// changes how many syscalls a burst costs.
pub const RECV_SCRATCH_BYTES: usize = 64 * 1024;

/// Map an errno onto the channel-level error. Mirrors the C++
/// `tcpconn_errno_to_channel_error` case for case.
pub fn errno_to_channel_error(err: i32) -> ChannelError {
    if err == sys::ERRNO_ECONNREFUSED {
        return ChannelError::ConnectionRefused;
    }
    if err == sys::ERRNO_ECONNRESET || err == sys::ERRNO_EPIPE || err == sys::ERRNO_ENOTCONN {
        return ChannelError::ConnectionReset;
    }
    if err == sys::ERRNO_ETIMEDOUT {
        return ChannelError::Timeout;
    }
    if err == sys::ERRNO_EADDRINUSE {
        return ChannelError::AddressInUse;
    }
    if err == sys::ERRNO_EADDRNOTAVAIL {
        return ChannelError::AddressInvalid;
    }
    if err == sys::ERRNO_EACCES || err == sys::ERRNO_EPERM {
        return ChannelError::PermissionDenied;
    }
    if err == sys::ERRNO_EMFILE || err == sys::ERRNO_ENFILE {
        return ChannelError::TooManyOpenFiles;
    }
    ChannelError::Internal
}

/// Parse `"a.b.c.d:port"`. Deliberately narrow: the C++ takes the same
/// dotted-quad form and does no name resolution, so neither does this.
pub fn parse_addr_v4(addr: &str) -> Option<sys::SockAddrIn> {
    let (host, port) = addr.rsplit_once(':')?;
    let port: u16 = port.parse().ok()?;
    let mut octets = [0u8; 4];
    let mut seen = 0usize;
    for part in host.split('.') {
        if seen == 4 {
            return None;
        }
        octets[seen] = part.parse().ok()?;
        seen += 1;
    }
    if seen != 4 {
        return None;
    }
    let quad = ((octets[0] as u32) << 24)
        | ((octets[1] as u32) << 16)
        | ((octets[2] as u32) << 8)
        | (octets[3] as u32);
    Some(sys::SockAddrIn::new(quad, port))
}

/// Callback invoked once per decoded frame, on the poll thread.
pub type FrameHandler = Box<dyn Fn(FrameHeader, &[u8]) + Send + Sync>;
/// Callback invoked when the connection closes, with the cause
/// (`ChannelError::None` for an orderly peer close).
pub type CloseHandler = Box<dyn Fn(ChannelError) + Send + Sync>;

/// A connected TCP socket driven by the poll thread.
pub struct TcpConnection {
    fd: AtomicI32,
    closed: AtomicBool,
    /// Inbound bytes and the frame decoder over them.
    reader: Mutex<FrameReader>,
    /// Bytes queued for `send`, and how far into the head we got. A
    /// partial write is the normal case under backpressure, so the
    /// offset is part of the state, not an edge case.
    outbound: Mutex<Outbound>,
    /// Serializes the recv scratch buffer, which is per-connection
    /// rather than per-call to keep 64 KiB off the stack.
    scratch: Mutex<Vec<u8>>,
    on_frame: Mutex<Option<FrameHandler>>,
    on_closed: Mutex<Option<CloseHandler>>,
    /// Fires the close handler exactly once, however the connection
    /// ends — peer close, read error, write error, or an explicit
    /// `close()`.
    closed_delivered: AtomicBool,
    /// Whether to accumulate replies and flush once per poll iteration.
    ///
    /// On by default (the server shape, and the C++'s). A CLIENT wants
    /// it off: its sends are spread over time rather than produced in a
    /// burst, so holding a request until the read batch finishes is a
    /// pipeline bubble the coalescing does not repay.
    coalesce: AtomicBool,
    /// The poll thread driving this connection, for re-arming the write
    /// interest after a partial send.
    ///
    /// WEAK on purpose: the poll thread holds an `Arc` to every pollable
    /// it drives, so a strong handle back would be a cycle that never
    /// frees. `None` is a supported state — the unit tests drive a
    /// connection with no poll thread at all.
    poll_thread: Mutex<Option<Weak<PollThread>>>,
}

#[derive(Default)]
struct Outbound {
    /// ONE contiguous buffer, not a queue of frames — the C++ shape
    /// (`frame_codec_encode_into(buf, …)` appends into a single
    /// `outbound_`).
    ///
    /// This is what lets N replies produced in one poll iteration leave
    /// as ONE `send`. A queue of separate frames cannot coalesce, so it
    /// costs a syscall and a small TCP segment per reply — which Nagle
    /// then punishes. At depth 400 that is 400 syscalls where the C++
    /// does one, and it was most of a 16x throughput collapse at 1 KiB.
    buf: Vec<u8>,
    /// Bytes of `buf` already written.
    offset: usize,
}

impl Outbound {
    fn pending(&self) -> usize {
        self.buf.len() - self.offset
    }

    /// Drop the written prefix once it dominates the buffer, so the
    /// offset cannot grow without bound on a long-lived connection.
    fn compact(&mut self) {
        if self.offset == self.buf.len() {
            self.buf.clear();
            self.offset = 0;
        } else if self.offset >= 64 * 1024 {
            self.buf.drain(..self.offset);
            self.offset = 0;
        }
    }
}

impl TcpConnection {
    /// Take ownership of an already-connected, already-non-blocking fd.
    pub fn from_fd(fd: i32) -> TcpConnection {
        TcpConnection {
            fd: AtomicI32::new(fd),
            closed: AtomicBool::new(false),
            reader: Mutex::new(FrameReader::new()),
            outbound: Mutex::new(Outbound::default()),
            scratch: Mutex::new(vec![0u8; RECV_SCRATCH_BYTES]),
            on_frame: Mutex::new(None),
            on_closed: Mutex::new(None),
            closed_delivered: AtomicBool::new(false),
            coalesce: AtomicBool::new(true),
            poll_thread: Mutex::new(None),
        }
    }

    pub fn set_on_frame(&self, f: FrameHandler) {
        *self.on_frame.lock().unwrap() = Some(f);
    }

    pub fn set_on_closed(&self, f: CloseHandler) {
        *self.on_closed.lock().unwrap() = Some(f);
    }

    /// Tell the connection which poll thread drives it, so a partial
    /// write can re-arm the write interest. Without this a `send_frame`
    /// that returns `WouldBlock` leaves bytes queued with nothing to
    /// flush them.
    pub fn attach_poll_thread(&self, poll: &Arc<PollThread>) {
        *self.poll_thread.lock().unwrap() = Some(Arc::downgrade(poll));
    }

    /// Write each frame as it is queued instead of batching. See
    /// [`Self::coalesce`].
    pub fn set_write_immediate(&self) {
        self.coalesce.store(false, Ordering::Release);
    }

    pub fn is_closed(&self) -> bool {
        self.closed.load(Ordering::Acquire)
    }

    /// Ask the poll thread for `EPOLLOUT` as well as `EPOLLIN`.
    ///
    /// Safe to call from any thread, including the poll thread itself:
    /// `update_mode` posts a command, which the loop drains at the
    /// bottom of its current iteration rather than acting on inline.
    fn arm_write_interest(&self) {
        let fd = self.fd.load(Ordering::Acquire);
        if fd < 0 {
            return;
        }
        let guard = self.poll_thread.lock().unwrap();
        if let Some(weak) = guard.as_ref() {
            if let Some(poll) = weak.upgrade() {
                poll.update_mode(fd, PollMode::READ_WRITE);
            }
        }
    }

    /// Queue a frame and try to write it immediately.
    ///
    /// Returns `WouldBlock` when bytes remain queued — the caller's
    /// signal that the poll thread now owns the rest, which is what
    /// makes the write edge-triggered rather than spin.
    pub fn send_frame(&self, bytes: Vec<u8>) -> ChannelError {
        if self.is_closed() {
            return ChannelError::ConnectionReset;
        }
        let result = {
            let mut guard = self.outbound.lock().unwrap();
            guard.buf.extend_from_slice(&bytes);
            if self.coalesce.load(Ordering::Acquire)
                && crate::runtime::poll_thread::on_poll_thread()
            {
                // On the poll thread the caller is inside a frame
                // callback, and more replies are likely to follow in
                // this same iteration. Accumulate and let the read pump
                // flush once — that is the coalescing.
                ChannelError::WouldBlock
            } else {
                // Off the poll thread there is no iteration to
                // piggyback on, so write now; deferring would cost a
                // wakeup per request and the poll loop has no eventfd
                // to make that cheap.
                self.drain_outbound_locked(&mut guard)
            }
        };
        if result == ChannelError::WouldBlock {
            // Bytes remain: hand the rest to the poll thread. Dropping
            // this is the classic wedge — the queue never drains and the
            // caller waits forever for a reply that was never sent.
            self.arm_write_interest();
        }
        result
    }

    /// Write from the head of the queue until the socket says stop.
    /// `Ok`-equivalent is `ChannelError::None` (queue empty) or
    /// `WouldBlock` (bytes remain).
    fn drain_outbound_locked(&self, out: &mut Outbound) -> ChannelError {
        let fd = self.fd.load(Ordering::Acquire);
        if fd < 0 {
            return ChannelError::ConnectionReset;
        }
        while out.pending() > 0 {
            let n = sys::send_fd(fd, &out.buf[out.offset..]);
            if n > 0 {
                out.offset += n as usize;
                out.compact();
                continue;
            }
            // send() returning 0 on a stream socket means it accepted
            // nothing; treating it as progress would spin.
            if n == 0 {
                return ChannelError::WouldBlock;
            }
            let err = (-n) as i32;
            if err == sys::ERRNO_EAGAIN || err == sys::ERRNO_EWOULDBLOCK {
                return ChannelError::WouldBlock;
            }
            if err == sys::ERRNO_EINTR {
                continue;
            }
            return errno_to_channel_error(err);
        }
        out.compact();
        ChannelError::None
    }

    /// Write whatever has accumulated. Called by the read pump once per
    /// iteration, after every frame in the batch has been dispatched.
    pub fn flush(&self) {
        let mut guard = self.outbound.lock().unwrap();
        if guard.pending() == 0 {
            return;
        }
        let rc = self.drain_outbound_locked(&mut guard);
        drop(guard);
        if rc == ChannelError::WouldBlock {
            self.arm_write_interest();
        } else if rc != ChannelError::None {
            self.close_with(rc);
        }
    }

    /// Deliver the close callback at most once.
    fn deliver_closed(&self, cause: ChannelError) {
        if self
            .closed_delivered
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return;
        }
        let guard = self.on_closed.lock().unwrap();
        if let Some(f) = guard.as_ref() {
            f(cause);
        }
    }

    /// Mark dead and release the fd. Idempotent.
    fn reset_fd(&self) {
        let fd = self.fd.swap(-1, Ordering::AcqRel);
        if fd >= 0 {
            sys::close_fd(fd);
        }
    }

    /// Shut the connection down and report `cause` to the close handler.
    pub fn close_with(&self, cause: ChannelError) {
        if self.closed.swap(true, Ordering::AcqRel) {
            return;
        }
        let fd = self.fd.load(Ordering::Acquire);
        if fd >= 0 {
            sys::shutdown_fd(fd, sys::SHUT_RDWR);
        }
        self.reset_fd();
        self.deliver_closed(cause);
    }

    pub fn close(&self) {
        self.close_with(ChannelError::None);
    }

    /// Decode and dispatch every complete frame currently buffered.
    fn decode_buffered(&self) {
        loop {
            let mut reader = self.reader.lock().unwrap();
            // The handler runs while the reader lock is held, because
            // the payload it receives borrows the reader's buffer. That
            // is the cost of not copying: a handler must not re-enter
            // this connection's read path. It may freely `send_frame`,
            // which takes a different lock.
            let guard = self.on_frame.lock().unwrap();
            let dispatched = match guard.as_ref() {
                Some(f) => reader.with_next_frame(|hdr, payload| f(hdr, payload)),
                // No handler: still consume, or the buffer grows without
                // bound and the connection wedges.
                None => reader.with_next_frame(|_, _| {}),
            };
            match dispatched {
                Ok(true) => {}
                Ok(false) => return,
                Err(()) => {
                    drop(guard);
                    drop(reader);
                    self.close_with(ChannelError::Internal);
                    return;
                }
            }
        }
    }
}

impl Pollable for TcpConnection {
    fn fd(&self) -> i32 {
        self.fd.load(Ordering::Acquire)
    }

    fn poll_mode(&self) -> PollMode {
        // READ always; WRITE only while bytes are queued. Registration
        // reads this, so a connection registered with a backlog already
        // pending comes up with EPOLLOUT armed.
        let queued = self.outbound.lock().unwrap().pending() > 0;
        if queued {
            PollMode::READ_WRITE
        } else {
            PollMode::READ
        }
    }

    fn handle_read(&self) {
        if self.is_closed() {
            return;
        }
        let fd = self.fd.load(Ordering::Acquire);
        if fd < 0 {
            return;
        }

        // Drain first: under edge triggering, stopping early loses the
        // edge and the connection stalls with data pending.
        let mut scratch = self.scratch.lock().unwrap();
        loop {
            let n = sys::recv_fd(fd, &mut scratch[..]);
            if n > 0 {
                let got = n as usize;
                self.reader.lock().unwrap().append(&scratch[..got]);
                if got < RECV_SCRATCH_BYTES {
                    // A short read means the socket is drained; skip the
                    // extra syscall that would return EAGAIN.
                    break;
                }
                continue;
            }
            if n == 0 {
                // Orderly peer close.
                drop(scratch);
                self.closed.store(true, Ordering::Release);
                self.reset_fd();
                self.deliver_closed(ChannelError::None);
                return;
            }
            let err = (-n) as i32;
            if err == sys::ERRNO_EAGAIN || err == sys::ERRNO_EWOULDBLOCK {
                break;
            }
            if err == sys::ERRNO_EINTR {
                continue;
            }
            drop(scratch);
            let cause = errno_to_channel_error(err);
            self.closed.store(true, Ordering::Release);
            self.reset_fd();
            self.deliver_closed(cause);
            return;
        }
        drop(scratch);

        self.decode_buffered();
        // ONE write for every reply this iteration produced.
        self.flush();
    }

    fn handle_write(&self) -> PollMode {
        if self.is_closed() {
            return PollMode::NO_CHANGE;
        }
        let mut guard = self.outbound.lock().unwrap();
        if guard.pending() == 0 {
            return PollMode::READ;
        }
        let result = self.drain_outbound_locked(&mut guard);
        if result == ChannelError::None {
            // Drop EPOLLOUT only once the buffer is actually empty.
            if guard.pending() == 0 {
                return PollMode::READ;
            }
            return PollMode::NO_CHANGE;
        }
        if result == ChannelError::WouldBlock {
            return PollMode::NO_CHANGE;
        }
        drop(guard);
        self.close_with(result);
        PollMode::READ
    }

    fn handle_error(&self) {
        if self.is_closed() {
            return;
        }
        self.close_with(ChannelError::Internal);
    }
}

/// Creates connections. Holds the connect timeout, which is the only
/// knob the C++ factory carries.
pub struct TcpFactory {
    connect_timeout_ms: i32,
}

impl Default for TcpFactory {
    fn default() -> TcpFactory {
        TcpFactory::new()
    }
}

impl TcpFactory {
    pub fn new() -> TcpFactory {
        TcpFactory {
            connect_timeout_ms: 5000,
        }
    }

    pub fn set_connect_timeout_ms(&mut self, timeout_ms: i32) {
        self.connect_timeout_ms = timeout_ms;
    }

    /// `socket` + non-blocking + `connect`, with a bounded wait.
    ///
    /// Non-blocking BEFORE the connect is what makes the timeout
    /// possible at all — a blocking `connect(2)` ignores it and hangs
    /// for the kernel's own SYN retry budget.
    pub fn connect(&self, addr: &str) -> Result<TcpConnection, ChannelError> {
        let sa = parse_addr_v4(addr).ok_or(ChannelError::AddressInvalid)?;

        let fd = sys::socket_fd(sys::SYS_AF_INET, sys::SOCK_STREAM, 0);
        if fd < 0 {
            return Err(errno_to_channel_error(-fd));
        }

        let rc = sys::set_nonblocking(fd);
        if rc < 0 {
            sys::close_fd(fd);
            return Err(errno_to_channel_error(-rc));
        }

        let rc = sys::connect_fd(fd, &sa);
        if rc < 0 {
            let err = -rc;
            if err == sys::ERRNO_EINPROGRESS && self.connect_timeout_ms > 0 {
                match self.await_connect(fd) {
                    Ok(()) => {}
                    Err(e) => {
                        sys::close_fd(fd);
                        return Err(e);
                    }
                }
            } else if err != sys::ERRNO_EISCONN {
                // EISCONN means it already completed — success, not
                // failure.
                sys::close_fd(fd);
                return Err(errno_to_channel_error(err));
            }
        }

        Ok(TcpConnection::from_fd(fd))
    }

    /// Wait for an in-progress connect, then ask the socket how it
    /// actually went.
    ///
    /// Writability alone does NOT mean success: a refused connection
    /// also reports writable, and the real outcome only appears in
    /// `SYS_SO_ERROR`. Skipping that check yields a "connected" socket whose
    /// first write fails.
    fn await_connect(&self, fd: i32) -> Result<(), ChannelError> {
        let rc = sys::wait_writable(fd, self.connect_timeout_ms);
        if rc == 0 {
            return Err(ChannelError::Timeout);
        }
        if rc < 0 {
            return Err(errno_to_channel_error(-rc));
        }
        match sys::getsockopt_int(fd, sys::SYS_SOL_SOCKET, sys::SYS_SO_ERROR) {
            Err(e) => Err(errno_to_channel_error(e)),
            Ok(0) => Ok(()),
            Ok(so_err) => Err(errno_to_channel_error(so_err)),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicUsize;
    use std::sync::Arc;

    #[test]
    fn parses_dotted_quad_and_port() {
        let sa = parse_addr_v4("127.0.0.1:8080").expect("should parse");
        assert_eq!(sa.family, sys::SYS_AF_INET as u16);
        // Both fields are network order on the wire.
        assert_eq!(sa.port, 8080u16.to_be());
        assert_eq!(sa.addr, 0x7f00_0001u32.to_be());
    }

    #[test]
    fn rejects_malformed_addresses() {
        for bad in [
            "127.0.0.1",       // no port
            "127.0.0:8080",    // three octets
            "127.0.0.1.5:80",  // five octets
            "127.0.0.256:80",  // octet out of range
            "localhost:8080",  // no name resolution, by design
            "127.0.0.1:99999", // port out of range
            "",
        ] {
            assert!(parse_addr_v4(bad).is_none(), "should reject {bad:?}");
        }
    }

    #[test]
    fn errno_mapping_matches_the_cpp_table() {
        assert_eq!(
            errno_to_channel_error(sys::ERRNO_ECONNREFUSED),
            ChannelError::ConnectionRefused
        );
        for e in [sys::ERRNO_ECONNRESET, sys::ERRNO_EPIPE, sys::ERRNO_ENOTCONN] {
            assert_eq!(errno_to_channel_error(e), ChannelError::ConnectionReset);
        }
        assert_eq!(
            errno_to_channel_error(sys::ERRNO_ETIMEDOUT),
            ChannelError::Timeout
        );
        assert_eq!(
            errno_to_channel_error(sys::ERRNO_EADDRINUSE),
            ChannelError::AddressInUse
        );
        assert_eq!(
            errno_to_channel_error(sys::ERRNO_EADDRNOTAVAIL),
            ChannelError::AddressInvalid
        );
        for e in [sys::ERRNO_EACCES, sys::ERRNO_EPERM] {
            assert_eq!(errno_to_channel_error(e), ChannelError::PermissionDenied);
        }
        for e in [sys::ERRNO_EMFILE, sys::ERRNO_ENFILE] {
            assert_eq!(errno_to_channel_error(e), ChannelError::TooManyOpenFiles);
        }
        // Anything unmapped is Internal, not a panic.
        assert_eq!(errno_to_channel_error(1234), ChannelError::Internal);
    }

    #[test]
    fn connect_to_a_closed_port_is_refused_not_hung() {
        let f = TcpFactory::new();
        // Port 1 on loopback: nothing listens, and the kernel refuses
        // immediately rather than dropping the SYN.
        let err = match f.connect("127.0.0.1:1") {
            Ok(_) => panic!("nothing should be listening on port 1"),
            Err(e) => e,
        };
        assert!(
            err == ChannelError::ConnectionRefused || err == ChannelError::PermissionDenied,
            "unexpected {err:?}"
        );
    }

    #[test]
    fn connect_rejects_a_malformed_address_without_a_socket() {
        let f = TcpFactory::new();
        match f.connect("not-an-address") {
            Ok(_) => panic!("should reject a malformed address"),
            Err(e) => assert_eq!(e, ChannelError::AddressInvalid),
        }
    }

    #[test]
    fn send_frame_on_a_closed_connection_reports_reset() {
        let conn = TcpConnection::from_fd(-1);
        conn.close();
        assert!(conn.is_closed());
        assert_eq!(
            conn.send_frame(vec![1, 2, 3]),
            ChannelError::ConnectionReset
        );
    }

    #[test]
    fn close_delivers_its_callback_exactly_once() {
        let conn = TcpConnection::from_fd(-1);
        let hits = Arc::new(AtomicUsize::new(0));
        let seen = Arc::clone(&hits);
        conn.set_on_closed(Box::new(move |_| {
            seen.fetch_add(1, Ordering::SeqCst);
        }));
        conn.close();
        conn.close();
        conn.close_with(ChannelError::Timeout);
        assert_eq!(hits.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn handle_write_reports_read_when_nothing_is_queued() {
        let conn = TcpConnection::from_fd(-1);
        // Empty queue drops EPOLLOUT; that plus the epoll dedupe is what
        // re-arms the edge-triggered write.
        assert_eq!(conn.handle_write(), PollMode::READ);
    }
}
