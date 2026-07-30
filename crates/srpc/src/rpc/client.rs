//! Client endpoint: request framing, xid demultiplexing, and the
//! `Future` a caller waits on.
//!
//! ## The wire envelopes are NOT symmetric
//!
//! A request is `[i32 size LE][v64 xid][i32 rpc_id RAW LE][args]` and a
//! reply is `[i32 size LE][v64 xid][v32 err][v64 server_instance_id]
//! [results]`. Three asymmetries, each of which silently corrupts the
//! stream if got wrong rather than failing cleanly:
//!
//!   - `xid` is a varint but `rpc_id` is a RAW little-endian i32;
//!   - the reply's error is a **v32 varint**, not a fixed i32;
//!   - the reply carries a `server_instance_id` the request does not.
//!
//! ## One wakeup per reply, not per request
//!
//! The reply is parsed and its future completed on the poll thread,
//! inside the frame callback. There is no per-request wakeup syscall on
//! the send side: `send_frame` writes inline from the calling thread and
//! only defers to the poll thread when the socket pushes back. That is
//! the C++ send-path economics, and it is the thing a port most easily
//! gets wrong by routing every send through a queue.

use crate::base::sync::Counter;
use crate::rpc::errors::ChannelError;
use crate::runtime::poll_thread::{PollThread, Pollable};
use crate::runtime::tcp::{TcpConnection, TcpFactory};
use crate::wire::frame::encode_into;
use crate::wire::varint;
use std::collections::HashMap;
use std::sync::atomic::{AtomicI32, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::time::Duration;

/// `ENOTCONN`, the error a pending request is failed with when the
/// connection dies under it. Matches the C++, which uses the errno
/// directly rather than a channel-level code.
pub const ENOTCONN: i32 = 107;
/// `ETIMEDOUT`, for a request whose wait deadline expires.
pub const ETIMEDOUT: i32 = 110;

/// A reply the caller is waiting for.
///
/// Completed either by the poll thread (a reply arrived) or by whoever
/// tears the connection down (it never will). Both paths go through
/// [`Future::complete`], so a future is finished exactly once.
pub struct Future {
    xid: i64,
    error_code: AtomicI32,
    state: Mutex<FutureState>,
    ready: Condvar,
    /// Set when this future is awaited by a task rather than blocked
    /// on. Invoked by [`Future::complete`] on the POLL THREAD, so the
    /// continuation runs there instead of costing a park/unpark on a
    /// user thread — the whole point of the executor.
    waker: Mutex<Option<std::task::Waker>>,
}

#[derive(Default)]
struct FutureState {
    done: bool,
    reply: Vec<u8>,
}

impl Future {
    pub(crate) fn new(xid: i64) -> Future {
        Future {
            xid,
            error_code: AtomicI32::new(0),
            state: Mutex::new(FutureState::default()),
            ready: Condvar::new(),
            waker: Mutex::new(None),
        }
    }

    pub fn xid(&self) -> i64 {
        self.xid
    }

    pub fn error_code(&self) -> i32 {
        self.error_code.load(Ordering::Acquire)
    }

    /// Finish this future and wake every waiter. Idempotent: a reply
    /// that races a teardown must not double-complete.
    fn complete(&self, err: i32, reply: Vec<u8>) {
        let mut guard = self.state.lock().unwrap();
        if guard.done {
            return;
        }
        self.error_code.store(err, Ordering::Release);
        guard.reply = reply;
        guard.done = true;
        drop(guard);
        self.ready.notify_all();
        // AFTER releasing the state lock: waking re-polls the task
        // inline, and that continuation may touch this future again.
        let waker = self.waker.lock().unwrap().take();
        if let Some(w) = waker {
            w.wake();
        }
    }

    /// Register the waker to invoke on completion, and report whether
    /// the future is already done — the caller must not park if so, or
    /// it parks forever against a wake that already happened.
    pub(crate) fn register_waker(&self, w: &std::task::Waker) -> bool {
        let guard = self.state.lock().unwrap();
        if guard.done {
            return true;
        }
        *self.waker.lock().unwrap() = Some(w.clone());
        false
    }

    /// Take the completed result. Only valid once done.
    pub(crate) fn take_result(&self) -> Result<Vec<u8>, i32> {
        let mut guard = self.state.lock().unwrap();
        let err = self.error_code.load(Ordering::Acquire);
        if err != 0 {
            return Err(err);
        }
        Ok(std::mem::take(&mut guard.reply))
    }

    /// Block until the reply arrives. Returns the result payload, or the
    /// error code if the request failed.
    pub fn wait(&self) -> Result<Vec<u8>, i32> {
        let mut guard = self.state.lock().unwrap();
        while !guard.done {
            guard = self.ready.wait(guard).unwrap();
        }
        let err = self.error_code.load(Ordering::Acquire);
        if err != 0 {
            return Err(err);
        }
        Ok(std::mem::take(&mut guard.reply))
    }

    /// Block until the reply arrives or `timeout` elapses.
    ///
    /// A timeout leaves the future registered: the reply may still
    /// arrive, and unregistering here would leak the entry on the
    /// dispatch side. It is the connection's teardown that clears it.
    pub fn wait_timeout(&self, timeout: Duration) -> Result<Vec<u8>, i32> {
        let mut guard = self.state.lock().unwrap();
        let deadline = std::time::Instant::now() + timeout;
        while !guard.done {
            let now = std::time::Instant::now();
            if now >= deadline {
                return Err(ETIMEDOUT);
            }
            let (g, _) = self.ready.wait_timeout(guard, deadline - now).unwrap();
            guard = g;
        }
        let err = self.error_code.load(Ordering::Acquire);
        if err != 0 {
            return Err(err);
        }
        Ok(std::mem::take(&mut guard.reply))
    }

    pub fn is_done(&self) -> bool {
        self.state.lock().unwrap().done
    }

    /// Complete this future directly. Test-only: outside tests a future
    /// is completed by the reply path or by teardown, never by hand.
    #[cfg(test)]
    pub(crate) fn complete_for_test(&self, err: i32, reply: Vec<u8>) {
        self.complete(err, reply);
    }
}

/// Encode a request envelope: `[v64 xid][i32 rpc_id RAW LE][args]`.
///
/// `rpc_id` is deliberately NOT a varint — it is a raw little-endian
/// i32, unlike the xid immediately before it. Encoding it as a varint
/// produces a frame the C++ peer will misparse rather than reject.
pub fn encode_request(xid: i64, rpc_id: i32, args: &[u8]) -> Vec<u8> {
    let mut body = Vec::with_capacity(16 + args.len());
    let mut scratch = [0u8; 16];
    let n = varint::dump64(xid, &mut scratch);
    body.extend_from_slice(&scratch[..n]);
    body.extend_from_slice(&rpc_id.to_le_bytes());
    body.extend_from_slice(args);
    body
}

/// The parsed head of a reply body.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct ReplyHead {
    pub xid: i64,
    pub error: i32,
    pub server_instance_id: i64,
    /// Offset of the results within the body.
    pub results_at: usize,
}

/// Parse `[v64 xid][v32 err][v64 server_instance_id]` off the front of a
/// reply body. `None` if the body is truncated.
pub fn parse_reply_head(body: &[u8]) -> Option<ReplyHead> {
    // `load32`/`load64` read a FIXED-WIDTH window (VARINT_BUF_LEN),
    // not a slice sized to the encoded value — the C++ contract, where
    // the caller always has a full buffer behind the pointer. Near the
    // end of a body that is not true, so each field is decoded through
    // a zero-padded window. Slicing exactly to `buf_size` instead reads
    // past the end.
    fn take(body: &[u8], at: &mut usize) -> Option<[u8; varint::VARINT_BUF_LEN]> {
        if *at >= body.len() {
            return None;
        }
        let n = varint::buf_size(body[*at]);
        if *at + n > body.len() {
            return None;
        }
        let mut window = [0u8; varint::VARINT_BUF_LEN];
        window[..n].copy_from_slice(&body[*at..*at + n]);
        *at += n;
        Some(window)
    }

    let mut at = 0usize;
    let xid = varint::load64(&take(body, &mut at)?);
    // v32, not a fixed i32 — the single easiest field in this protocol
    // to encode wrongly, because it reads correctly for small values.
    let error = varint::load32(&take(body, &mut at)?);
    let server_instance_id = varint::load64(&take(body, &mut at)?);

    Some(ReplyHead {
        xid,
        error,
        server_instance_id,
        results_at: at,
    })
}

/// A connected client: frames requests, demultiplexes replies by xid.
pub struct ClientConnection {
    conn: Arc<TcpConnection>,
    xid_counter: Counter,
    pending: Arc<Mutex<HashMap<i64, Arc<Future>>>>,
}

impl ClientConnection {
    /// Dial `addr` and register with `poll`.
    pub fn connect(
        addr: &str,
        poll: &Arc<PollThread>,
    ) -> Result<Arc<ClientConnection>, ChannelError> {
        let factory = TcpFactory::new();
        let conn = Arc::new(factory.connect(addr)?);

        let pending: Arc<Mutex<HashMap<i64, Arc<Future>>>> = Arc::new(Mutex::new(HashMap::new()));

        // The reply path runs ON THE POLL THREAD, inside the frame
        // callback, which is what keeps a reply from costing a wakeup.
        let dispatch = Arc::clone(&pending);
        conn.set_on_frame(Box::new(move |_hdr, body| {
            let Some(head) = parse_reply_head(body) else {
                // A malformed head cannot be attributed to a request, so
                // there is nothing to fail; dropping it is the only
                // option that does not corrupt an unrelated future.
                return;
            };
            let fu = dispatch.lock().unwrap().remove(&head.xid);
            if let Some(fu) = fu {
                fu.complete(head.error, body[head.results_at..].to_vec());
            }
            // An unknown xid is a reply to something already failed or
            // timed out. Not an error.
        }));

        // Everything still waiting when the connection dies must be
        // failed, or its callers block forever.
        let orphans = Arc::clone(&pending);
        conn.set_on_closed(Box::new(move |_cause| {
            let drained: Vec<Arc<Future>> =
                orphans.lock().unwrap().drain().map(|(_, f)| f).collect();
            for fu in drained {
                fu.complete(ENOTCONN, Vec::new());
            }
        }));

        conn.attach_poll_thread(poll);
        poll.add(Arc::clone(&conn) as Arc<dyn Pollable>);

        Ok(Arc::new(ClientConnection {
            conn,
            xid_counter: Counter::new(0),
            pending,
        }))
    }

    /// Send a request and return the future its reply will complete.
    ///
    /// The future is registered BEFORE the bytes go out. Registering
    /// after would lose any reply that beat the registration — a real
    /// race on loopback, where the round trip can finish inside the
    /// send call.
    pub fn call(&self, rpc_id: i32, args: &[u8]) -> Result<Arc<Future>, ChannelError> {
        let xid = self.xid_counter.next_id();
        let fu = Arc::new(Future::new(xid));
        self.pending.lock().unwrap().insert(xid, Arc::clone(&fu));

        let body = encode_request(xid, rpc_id, args);
        let mut framed = Vec::with_capacity(body.len() + 8);
        if !encode_into(&mut framed, &body, false) {
            self.pending.lock().unwrap().remove(&xid);
            return Err(ChannelError::Internal);
        }

        let rc = self.conn.send_frame(framed);
        if rc != ChannelError::None && rc != ChannelError::WouldBlock {
            // The bytes never left; fail the future rather than leave
            // the caller waiting on a reply that cannot come.
            self.pending.lock().unwrap().remove(&xid);
            fu.complete(ENOTCONN, Vec::new());
            return Err(rc);
        }
        Ok(fu)
    }

    /// Requests still awaiting a reply.
    pub fn pending_count(&self) -> usize {
        self.pending.lock().unwrap().len()
    }

    pub fn is_closed(&self) -> bool {
        self.conn.is_closed()
    }

    pub fn close(&self) {
        self.conn.close();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn request_envelope_puts_a_raw_rpc_id_after_a_varint_xid() {
        // xid 1 encodes to one varint byte; rpc_id must then be four RAW
        // little-endian bytes, NOT a varint.
        let out = encode_request(1, 0x0102_0304, b"AB");
        let n = varint::buf_size(out[0]);
        let mut window = [0u8; varint::VARINT_BUF_LEN];
        window[..n].copy_from_slice(&out[..n]);
        assert_eq!(varint::load64(&window), 1);
        assert_eq!(&out[n..n + 4], &0x0102_0304i32.to_le_bytes());
        assert_eq!(&out[n + 4..], b"AB");
    }

    #[test]
    fn request_envelope_survives_a_multibyte_xid() {
        let xid = 1_000_000i64;
        let out = encode_request(xid, -7, b"");
        let n = varint::buf_size(out[0]);
        assert!(n > 1, "this xid should need more than one byte");
        let mut window = [0u8; varint::VARINT_BUF_LEN];
        window[..n].copy_from_slice(&out[..n]);
        assert_eq!(varint::load64(&window), xid);
        assert_eq!(&out[n..n + 4], &(-7i32).to_le_bytes());
    }

    fn reply_body(xid: i64, err: i32, instance: i64, results: &[u8]) -> Vec<u8> {
        let mut body = Vec::new();
        let mut scratch = [0u8; 16];
        let n = varint::dump64(xid, &mut scratch);
        body.extend_from_slice(&scratch[..n]);
        let n = varint::dump32(err, &mut scratch);
        body.extend_from_slice(&scratch[..n]);
        let n = varint::dump64(instance, &mut scratch);
        body.extend_from_slice(&scratch[..n]);
        body.extend_from_slice(results);
        body
    }

    #[test]
    fn reply_head_round_trips() {
        let body = reply_body(42, 0, 7, b"payload");
        let head = parse_reply_head(&body).expect("should parse");
        assert_eq!(head.xid, 42);
        assert_eq!(head.error, 0);
        assert_eq!(head.server_instance_id, 7);
        assert_eq!(&body[head.results_at..], b"payload");
    }

    #[test]
    fn reply_head_reads_a_multibyte_error_as_a_varint() {
        // A large error is where a fixed-i32 misreading diverges; small
        // ones parse the same either way, which is what makes this bug
        // survive casual testing.
        let body = reply_body(1, 1_000_000, 3, b"x");
        let head = parse_reply_head(&body).expect("should parse");
        assert_eq!(head.error, 1_000_000);
        assert_eq!(head.server_instance_id, 3);
        assert_eq!(&body[head.results_at..], b"x");
    }

    #[test]
    fn reply_head_rejects_a_truncated_body() {
        let body = reply_body(42, 0, 7, b"");
        for cut in 0..body.len() {
            assert!(
                parse_reply_head(&body[..cut]).is_none(),
                "a {cut}-byte body should not parse"
            );
        }
        assert!(parse_reply_head(&body).is_some());
    }

    #[test]
    fn a_future_completes_once() {
        let fu = Future::new(1);
        fu.complete(0, b"first".to_vec());
        fu.complete(5, b"second".to_vec());
        assert_eq!(fu.error_code(), 0);
        assert_eq!(fu.wait().expect("should be ok"), b"first");
    }

    #[test]
    fn a_failed_future_reports_its_error() {
        let fu = Future::new(1);
        fu.complete(ENOTCONN, Vec::new());
        assert_eq!(fu.wait().expect_err("should fail"), ENOTCONN);
    }

    #[test]
    fn wait_timeout_gives_up_without_a_reply() {
        let fu = Future::new(1);
        let err = fu
            .wait_timeout(Duration::from_millis(30))
            .expect_err("should time out");
        assert_eq!(err, ETIMEDOUT);
        assert!(!fu.is_done(), "a timeout must not complete the future");
    }

    #[test]
    fn wait_timeout_returns_a_reply_that_arrives_in_time() {
        let fu = Arc::new(Future::new(1));
        let bg = Arc::clone(&fu);
        std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(10));
            bg.complete(0, b"late".to_vec());
        });
        assert_eq!(
            fu.wait_timeout(Duration::from_secs(5))
                .expect("should arrive"),
            b"late"
        );
    }
}
