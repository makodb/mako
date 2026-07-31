//! Server endpoint: listener, handler registry, and request dispatch.
//!
//! ## Teardown and registration are the risky part
//!
//! Accept races epoll: a peer can connect and vanish before the
//! connection is registered, and a connection can be closed by its own
//! read pump while the accept loop is still handing out fds. The four
//! errno tolerances in [`crate::runtime::epoll`] exist for exactly this
//! and are not defensive padding — they are the historical CI-flake
//! fixes, ported as-is.
//!
//! ## The reply envelope is not the request envelope
//!
//! A reply is `[v64 xid][v32 err][v64 server_instance_id][results]`.
//! The error is a **v32 varint**, and the `server_instance_id` has no
//! counterpart in the request. Both are easy to omit and neither fails
//! loudly — the client simply reads the following field as this one.

use crate::rpc::errors::ChannelError;
use crate::runtime::epoll::PollMode;
use crate::runtime::poll_thread::{PollThread, Pollable};
use crate::runtime::tcp::{errno_to_channel_error, parse_addr_v4, TcpConnection};
use crate::sys;
use crate::wire::frame::encode_into;
use crate::wire::varint;
use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicI32, Ordering};
use std::sync::{Arc, Mutex, Weak};

/// `ENOENT` — what the C++ server reports for an unregistered rpc_id.
/// Verified against the live server by the interop suite.
pub const ENOENT: i32 = 2;

/// Backlog passed to `listen(2)`. Matches the C++.
const LISTEN_BACKLOG: i32 = 128;

/// A request handler: takes the argument bytes, returns either result
/// bytes or an error code.
pub type Handler = Box<dyn Fn(&[u8]) -> Result<Vec<u8>, i32> + Send + Sync>;

/// rpc_id → handler.
#[derive(Default)]
pub struct Registry {
    handlers: HashMap<i32, Handler>,
}

impl Registry {
    pub fn new() -> Registry {
        Registry::default()
    }

    pub fn register(&mut self, rpc_id: i32, h: Handler) {
        self.handlers.insert(rpc_id, h);
    }

    pub fn get(&self, rpc_id: i32) -> Option<&Handler> {
        self.handlers.get(&rpc_id)
    }

    pub fn len(&self) -> usize {
        self.handlers.len()
    }

    pub fn is_empty(&self) -> bool {
        self.handlers.is_empty()
    }
}

/// Parse `[v64 xid][i32 rpc_id RAW LE]` off a request body.
///
/// `rpc_id` is a raw little-endian i32, NOT a varint — the asymmetry
/// with the xid immediately before it. Reading it as a varint yields a
/// plausible-looking id that matches no handler.
pub fn parse_request_head(body: &[u8]) -> Option<(i64, i32, usize)> {
    if body.is_empty() {
        return None;
    }
    let n = varint::buf_size(body[0]);
    if n > body.len() {
        return None;
    }
    // load64 reads a FIXED-WIDTH window, so decode through a padded one.
    let mut window = [0u8; varint::VARINT_BUF_LEN];
    window[..n].copy_from_slice(&body[..n]);
    let xid = varint::load64(&window);

    if n + 4 > body.len() {
        return None;
    }
    let rpc_id = i32::from_le_bytes([body[n], body[n + 1], body[n + 2], body[n + 3]]);
    Some((xid, rpc_id, n + 4))
}

/// Build a reply body: `[v64 xid][v32 err][v64 server_instance_id]
/// [results]`.
pub fn encode_reply(xid: i64, err: i32, server_instance_id: i64, results: &[u8]) -> Vec<u8> {
    let mut body = Vec::with_capacity(24 + results.len());
    let mut scratch = [0u8; varint::VARINT_BUF_LEN];
    let n = varint::dump64(xid, &mut scratch);
    body.extend_from_slice(&scratch[..n]);
    // v32, not a fixed i32.
    let n = varint::dump32(err, &mut scratch);
    body.extend_from_slice(&scratch[..n]);
    let n = varint::dump64(server_instance_id, &mut scratch);
    body.extend_from_slice(&scratch[..n]);
    body.extend_from_slice(results);
    body
}

/// Shared server state: the registry plus the connections it owns.
struct Shared {
    registry: Registry,
    instance_id: i64,
    /// Run each request in its own fiber instead of inline.
    ///
    /// The C++ distinguishes these as `reg_rpc` (fiber) vs
    /// `reg_fast_rpc` (inline), and the measured ~35% gap between them
    /// is exactly this choice — spawn, two context switches and the TLS
    /// save/restore, with no yield anywhere.
    fiber_dispatch: bool,
    /// Accepted connections, kept alive for the server's lifetime.
    /// Without this the only strong reference is the poll thread's, and
    /// a connection removed on close would drop mid-callback.
    conns: Mutex<Vec<Arc<TcpConnection>>>,
}

/// A listening socket. Its `handle_read` is an accept loop.
pub struct Listener {
    fd: AtomicI32,
    closed: AtomicBool,
    shared: Arc<Shared>,
    /// Weak, for the same reason as on `TcpConnection`: the poll thread
    /// holds an Arc to this listener, so a strong handle back is a
    /// cycle that never frees.
    poll: Mutex<Option<Weak<PollThread>>>,
}

impl Listener {
    fn accept_one(&self, fd: i32) -> Option<i32> {
        let rc = sys::accept_fd(fd);
        if rc >= 0 {
            return Some(rc);
        }
        // EAGAIN is the drain condition, not a failure. EINTR retries.
        None
    }
}

impl Pollable for Listener {
    fn fd(&self) -> i32 {
        self.fd.load(Ordering::Acquire)
    }

    fn poll_mode(&self) -> PollMode {
        PollMode::READ
    }

    fn handle_read(&self) {
        if self.closed.load(Ordering::Acquire) {
            return;
        }
        let lfd = self.fd.load(Ordering::Acquire);
        if lfd < 0 {
            return;
        }
        // Edge-triggered: accept until the queue is empty, or the
        // remaining connections sit unaccepted with no further edge.
        loop {
            let Some(cfd) = self.accept_one(lfd) else {
                return;
            };
            if sys::set_nonblocking(cfd) < 0 {
                sys::close_fd(cfd);
                continue;
            }

            // DIAGNOSTIC ONLY, off by default. Nagle is ON throughout
            // this port deliberately (nothing in src/rrr sets
            // TCP_NODELAY), so this must never become the default — it
            // would invalidate every comparison. It exists to test
            // whether the 1 KiB throughput cliff is a Nagle/delayed-ACK
            // interaction.
            if std::env::var("SRPC_DIAG_NODELAY").is_ok() {
                sys::setsockopt_int(cfd, sys::IPPROTO_TCP, sys::TCP_NODELAY, 1);
            }
            let conn = Arc::new(TcpConnection::from_fd(cfd));
            let shared = Arc::clone(&self.shared);
            let reply_to = Arc::downgrade(&conn);
            conn.set_on_frame(Box::new(move |_hdr, body| {
                let Some(conn) = reply_to.upgrade() else {
                    return;
                };
                if !shared.fiber_dispatch {
                    dispatch(&shared, &conn, body);
                    return;
                }
                // Fiber dispatch. The body is COPIED out of the
                // zero-copy view before the spawn — mandatory, and it is
                // parity: the C++ moves the request into the fiber's
                // closure for the same reason. A fiber can outlive this
                // callback, and the view aliases a buffer the reader is
                // free to compact the moment we return.
                let owned = body.to_vec();
                let fiber_shared = Arc::clone(&shared);
                let fiber_conn = Arc::clone(&conn);
                let spawned = crate::runtime::fiber::spawn_here(Box::new(move || {
                    dispatch(&fiber_shared, &fiber_conn, &owned);
                }));
                if spawned.is_none() {
                    // No runtime on this thread, or no stack available.
                    // Falling back inline keeps the server answering
                    // rather than dropping the request silently.
                    dispatch(&shared, &conn, body);
                }
            }));

            // Drop the connection from the server's table when it dies,
            // or a long-lived server accumulates every peer it has ever
            // seen.
            let table = Arc::clone(&self.shared);
            let forget = Arc::downgrade(&conn);
            conn.set_on_closed(Box::new(move |_| {
                let Some(dead) = forget.upgrade() else {
                    return;
                };
                table
                    .conns
                    .lock()
                    .unwrap()
                    .retain(|c| !Arc::ptr_eq(c, &dead));
            }));

            self.shared.conns.lock().unwrap().push(Arc::clone(&conn));

            let guard = self.poll.lock().unwrap();
            if let Some(poll) = guard.as_ref().and_then(|w| w.upgrade()) {
                conn.attach_poll_thread(&poll);
                poll.add(Arc::clone(&conn) as Arc<dyn Pollable>);
            }
        }
    }

    fn handle_write(&self) -> PollMode {
        PollMode::NO_CHANGE
    }

    fn handle_error(&self) {
        self.closed.store(true, Ordering::Release);
        let fd = self.fd.swap(-1, Ordering::AcqRel);
        if fd >= 0 {
            sys::close_fd(fd);
        }
    }
}

/// Decode one request, run its handler, write the reply.
fn dispatch(shared: &Arc<Shared>, conn: &Arc<TcpConnection>, body: &[u8]) {
    let Some((xid, rpc_id, args_at)) = parse_request_head(body) else {
        // Unattributable to any xid, so there is no one to reply to.
        return;
    };
    let args = &body[args_at..];

    let (err, results) = match shared.registry.get(rpc_id) {
        Some(h) => match h(args) {
            Ok(r) => (0, r),
            Err(e) => (e, Vec::new()),
        },
        // An unregistered id is an ERROR REPLY, not a dropped request:
        // dropping it hangs the caller instead of failing it.
        None => (ENOENT, Vec::new()),
    };

    let reply = encode_reply(xid, err, shared.instance_id, &results);
    let mut framed = Vec::with_capacity(reply.len() + 8);
    if !encode_into(&mut framed, &reply, false) {
        return;
    }
    conn.send_frame(framed);
}

/// A listening RPC server.
pub struct Server {
    shared: Arc<Shared>,
    listener: Mutex<Option<Arc<Listener>>>,
}

impl Server {
    /// Build a server over `registry`. `instance_id` is echoed in every
    /// reply; the C++ uses it to let a client detect a restarted peer.
    pub fn new(registry: Registry, instance_id: i64) -> Arc<Server> {
        Server::with_dispatch(registry, instance_id, false)
    }

    /// Build a server that runs every handler in a fiber.
    pub fn with_fibers(registry: Registry, instance_id: i64) -> Arc<Server> {
        Server::with_dispatch(registry, instance_id, true)
    }

    fn with_dispatch(registry: Registry, instance_id: i64, fiber_dispatch: bool) -> Arc<Server> {
        Arc::new(Server {
            shared: Arc::new(Shared {
                registry,
                instance_id,
                fiber_dispatch,
                conns: Mutex::new(Vec::new()),
            }),
            listener: Mutex::new(None),
        })
    }

    /// Bind, listen, and register the accept loop with `poll`.
    ///
    /// `SO_REUSEADDR` before `bind` is what lets a restart reclaim a
    /// port still in TIME_WAIT — without it a server that just exited
    /// cannot rebind for a minute or more.
    pub fn listen(&self, addr: &str, poll: &Arc<PollThread>) -> Result<u16, ChannelError> {
        let sa = parse_addr_v4(addr).ok_or(ChannelError::AddressInvalid)?;

        let fd = sys::socket_fd(sys::AF_INET, sys::SOCK_STREAM, 0);
        if fd < 0 {
            return Err(errno_to_channel_error(-fd));
        }
        sys::setsockopt_int(fd, sys::SOL_SOCKET, sys::SO_REUSEADDR, 1);

        let rc = sys::bind_fd(fd, &sa);
        if rc < 0 {
            sys::close_fd(fd);
            return Err(errno_to_channel_error(-rc));
        }
        let rc = sys::listen_fd(fd, LISTEN_BACKLOG);
        if rc < 0 {
            sys::close_fd(fd);
            return Err(errno_to_channel_error(-rc));
        }
        let rc = sys::set_nonblocking(fd);
        if rc < 0 {
            sys::close_fd(fd);
            return Err(errno_to_channel_error(-rc));
        }

        let listener = Arc::new(Listener {
            fd: AtomicI32::new(fd),
            closed: AtomicBool::new(false),
            shared: Arc::clone(&self.shared),
            poll: Mutex::new(Some(Arc::downgrade(poll))),
        });
        *self.listener.lock().unwrap() = Some(Arc::clone(&listener));
        poll.add(listener as Arc<dyn Pollable>);

        // The REQUESTED port, echoed back. An ephemeral (`:0`) bind
        // would need `getsockname(2)` to learn the real one; nothing
        // needs that yet, so it is not implemented rather than faked.
        Ok(u16::from_be(sa.port))
    }

    pub fn connection_count(&self) -> usize {
        self.shared.conns.lock().unwrap().len()
    }

    pub fn close(&self) {
        if let Some(l) = self.listener.lock().unwrap().take() {
            l.handle_error();
        }
        for c in self.shared.conns.lock().unwrap().drain(..) {
            c.close();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::rpc::client::{encode_request, parse_reply_head};

    #[test]
    fn request_head_reads_a_raw_rpc_id_after_a_varint_xid() {
        let body = encode_request(1_000_000, 0x0a0b_0c0d, b"args");
        let (xid, rpc_id, at) = parse_request_head(&body).expect("should parse");
        assert_eq!(xid, 1_000_000);
        assert_eq!(rpc_id, 0x0a0b_0c0d);
        assert_eq!(&body[at..], b"args");
    }

    #[test]
    fn request_head_rejects_a_truncated_body() {
        let body = encode_request(7, 42, b"");
        for cut in 0..body.len() {
            assert!(
                parse_request_head(&body[..cut]).is_none(),
                "a {cut}-byte body should not parse"
            );
        }
        assert!(parse_request_head(&body).is_some());
    }

    #[test]
    fn reply_round_trips_through_the_client_parser() {
        // Encoder and decoder are on opposite sides of the wire, so
        // checking them against each other is the point.
        let body = encode_reply(1234, 0, 99, b"results");
        let head = parse_reply_head(&body).expect("client should parse");
        assert_eq!(head.xid, 1234);
        assert_eq!(head.error, 0);
        assert_eq!(head.server_instance_id, 99);
        assert_eq!(&body[head.results_at..], b"results");
    }

    #[test]
    fn reply_round_trips_a_large_error_as_a_varint() {
        let body = encode_reply(1, 1_000_000, 2, b"");
        let head = parse_reply_head(&body).expect("client should parse");
        assert_eq!(head.error, 1_000_000);
        assert_eq!(head.server_instance_id, 2);
    }

    #[test]
    fn registry_looks_handlers_up_by_id() {
        let mut r = Registry::new();
        r.register(7, Box::new(|a| Ok(a.to_vec())));
        assert_eq!(r.len(), 1);
        assert!(r.get(7).is_some());
        assert!(r.get(8).is_none(), "an unregistered id must not resolve");
    }
}
