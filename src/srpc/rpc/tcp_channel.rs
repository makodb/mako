//! TCP channel backend with all cross-thread connection state serialized by
//! the existing outbound mutex.  The poll thread remains the sole owner of
//! inbound decoder state; user-thread send/close operations never access it.
//!
//! On its PollThread each connection runs as two Lion tasks over one
//! `AsyncFd`, a reader and a writer, and each listener as an accept task
//! (S5 of docs/dev/lion-runtime-plan.md; see "Transport tasks" below).

#![allow(
    non_camel_case_types,
    non_snake_case,
    unsafe_code,
    clippy::explicit_auto_deref
)]

#[allow(unused_imports)]
use crate::reactor as _;

use std::cell::{RefCell, UnsafeCell};
use std::future::Future;
use std::os::fd::{AsRawFd, FromRawFd, IntoRawFd};
use std::pin::Pin;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Weak as ArcWeak};
use std::task::{Context, Poll, Waker};

use crate::channel::{
    ChannelConnectionBase, ChannelConnectionProxy, ChannelError, ChannelFactoryBase,
    ChannelFactoryProxy, ChannelFrame, ChannelListenerBase, ChannelListenerProxy, ConnectResult,
    NullableChannelConnectionProxy, OnAcceptCallback, OnClosedCallback, OnErrorCallback, OnFrameCallback,
};
use crate::frame_codec::{FrameDecodeStatus, FrameHeader, FrameStreamReader, FrameView};
use crate::misc::OneTimeJob;


type TcpOutBuf = Vec<u8>;
type LegacyOwnedFd = std::os::fd::OwnedFd;
type LegacyTcpListener = std::net::TcpListener;
type LegacySocketAddrV4 = std::net::SocketAddrV4;
type LegacyIoErrorKind = std::io::ErrorKind;
type PollThread = crate::reactor::PollThread;

pub const kTcpConnectionOutboundHighWaterDefault: usize = 4 * 1024 * 1024; // 4 MiB

// The adaptive cork (experiment on lion/s5-cork): a send from another thread
// writes through an empty outbound buffer only when the connection's last
// send(2), by any path, is at least this many microseconds old.  A busier
// connection leaves the frame queued for the writer task, which then sends
// every frame queued meanwhile in one drain.  0 writes through always.
pub const kTcpWriteThroughIdleUs: u64 = 20;

// Private numeric seams deliberately avoid libc's macro spellings so the
// generated module remains valid after the runtime headers include errno.h.
const TCP_ERR_ACCES: i32 = 13;
const TCP_ERR_ADDR_IN_USE: i32 = 98;
const TCP_ERR_ADDR_NOT_AVAILABLE: i32 = 99;
const TCP_ERR_AGAIN: i32 = 11;
const TCP_ERR_CONNECTION_REFUSED: i32 = 111;
const TCP_ERR_CONNECTION_RESET: i32 = 104;
const TCP_ERR_HOST_UNREACHABLE: i32 = 113;
const TCP_ERR_INTERRUPTED: i32 = 4;
const TCP_ERR_PROCESS_FD_LIMIT: i32 = 24;
const TCP_ERR_SYSTEM_FD_LIMIT: i32 = 23;
const TCP_ERR_NETWORK_UNREACHABLE: i32 = 101;
const TCP_ERR_NOT_CONNECTED: i32 = 107;
const TCP_ERR_OPERATION_NOT_PERMITTED: i32 = 1;
const TCP_ERR_BROKEN_PIPE: i32 = 32;
const TCP_ERR_TIMED_OUT: i32 = 110;
const TCP_ERR_WOULD_BLOCK: i32 = TCP_ERR_AGAIN;
// Derived from the decoder's bound so the two can never drift: a frame this
// side is willing to send must be one the peer's decoder will accept.
const TCP_MAX_FRAME_PAYLOAD_SIZE: usize = crate::frame_codec::kMaxFramePayloadSize as usize;

extern "C" {
    fn srpc_tcp_socket_open() -> i32;
    fn srpc_tcp_get_flags(fd: i32) -> i32;
    fn srpc_tcp_set_nonblocking_flags(fd: i32, flags: i32) -> i32;
    fn srpc_tcp_connect_once(fd: i32, addr_be: u32, port_be: u16) -> i32;
    fn srpc_tcp_wait_writable_once(fd: i32, timeout_ms: i32) -> i32;
    fn srpc_tcp_socket_error(fd: i32, socket_error: *mut i32) -> i32;
    fn srpc_tcp_local_endpoint(fd: i32, addr_be: *mut u32, port_be: *mut u16) -> i32;
    fn srpc_tcp_close(fd: i32) -> i32;
    fn srpc_tcp_in_progress_errno() -> i32;
    fn srpc_tcp_is_connected_errno() -> i32;
    fn srpc_tcp_recv_scratch() -> *mut u8;
    fn srpc_tcp_recv_bytes(fd: i32, data: *mut u8, size: usize) -> i64;
    fn srpc_tcp_send_bytes(fd: i32, data: *const u8, size: usize) -> i64;
    fn srpc_tcp_shutdown(fd: i32) -> i32;
    fn srpc_tcp_last_errno() -> i32;
    fn srpc_tcp_current_thread_id() -> u32;
    fn srpc_tcp_set_keepalive(fd: i32, enabled: i32) -> i32;
    fn srpc_tcp_set_keepalive_idle(fd: i32, seconds: i32) -> i32;
    fn srpc_tcp_set_keepalive_interval(fd: i32, seconds: i32) -> i32;
    fn srpc_tcp_set_keepalive_count(fd: i32, count: i32) -> i32;
}

pub struct TcpConnection {
    // The outbound mutex gates the descriptor slot. The transport tasks clone
    // its owner when they start and keep that lease until they retire.
    // Logical close clears this slot and shuts down the socket immediately;
    // the last lease releases the actual descriptor.
    fd_: UnsafeCell<Option<Arc<LegacyOwnedFd>>>,
    peer_address_: String,
    outbound_high_water_: usize,
    outbound_: std::sync::Mutex<TcpOutBuf>,
    inbound_: RefCell<FrameStreamReader>,
    closed_: AtomicBool,
    on_closed_fired_: AtomicBool,
    poll_thread_: Option<Arc<PollThread>>,
    on_frame_: std::sync::Mutex<OnFrameCallback>,
    on_closed_: std::sync::Mutex<OnClosedCallback>,
    on_error_: std::sync::Mutex<OnErrorCallback>,
    // The writer task's Lion waker, published whenever that task waits: for
    // the outbound buffer's empty->non-empty edge, which send_frame wakes it
    // on, or for write readiness.  Close and a failed flush take it too, so
    // the task retires.  Gated by `outbound_`, like `fd_`: an append and the
    // writer's emptiness check never interleave, so no edge is lost.
    writer_: UnsafeCell<Option<Waker>>,
    // A hard send error met by a write-through sender (lion/s5-writethrough),
    // as an errno; 0 when none.  That send consumed the socket's pending
    // error, so the transport tasks report this one instead: the writer on
    // its next drain, the reader if it reads the EOF that follows first.
    send_error_: std::sync::atomic::AtomicI32,
    // When send(2) last wrote bytes on this connection, in monotonic
    // microseconds (Time::now(true)); 0 before the first.  Written by every
    // send, all under `outbound_`, and read by the cork.
    last_send_us_: std::sync::atomic::AtomicU64,
}

// SAFETY: all state reachable through shared references is either immutable
// after publication, atomic, or protected by an existing mutex:
//
// * `fd_`, `writer_` and `outbound_` are protected by `outbound_`;
// * `inbound_` is owned by the connection's reader task, the one decoder,
//   and every access is protected by `on_frame_`;
// * callbacks are protected by their corresponding mutexes; and
// * `poll_thread_` is installed through `&mut self` before the Arc is shared.
//
// Only the reader task mutates the inbound decoder, and no public method
// reaches it, so safe Rust cannot create two competing decoder operations.
unsafe impl Send for TcpConnection {}
unsafe impl Sync for TcpConnection {}

impl TcpConnection {
    /// Construct a connection by taking unique ownership of `fd`.
    ///
    /// # Safety
    ///
    /// `fd` must be a live connected descriptor whose ownership is transferred
    /// exactly once. The caller must not close or otherwise use it afterward.
    pub unsafe fn new(fd: i32, peer_address: String) -> TcpConnection {
        TcpConnection {
            // SAFETY: callers transfer a freshly connected descriptor.
            fd_: UnsafeCell::new(Some(Arc::new(unsafe { LegacyOwnedFd::from_raw_fd(fd) }))),
            peer_address_: peer_address,
            outbound_high_water_: kTcpConnectionOutboundHighWaterDefault,
            outbound_: std::sync::Mutex::<TcpOutBuf>::new(Default::default()),
            inbound_: RefCell::new(FrameStreamReader::new()),
            closed_: AtomicBool::new(false),
            on_closed_fired_: AtomicBool::new(false),
            poll_thread_: None,
            on_frame_: std::sync::Mutex::<OnFrameCallback>::new(Default::default()),
            on_closed_: std::sync::Mutex::<OnClosedCallback>::new(Default::default()),
            on_error_: std::sync::Mutex::<OnErrorCallback>::new(Default::default()),
            writer_: UnsafeCell::new(None),
            send_error_: std::sync::atomic::AtomicI32::new(0),
            last_send_us_: std::sync::atomic::AtomicU64::new(0),
        }
    }

    pub fn set_outbound_high_water(&mut self, bytes: usize) {
        self.outbound_high_water_ = bytes;
    }

    /// # Safety
    ///
    /// `frame` must satisfy the raw payload validity contract on
    /// `ChannelConnectionBase::send_frame`.
    pub unsafe fn send_frame(&self, frame: &ChannelFrame) -> ChannelError {
        unsafe { tcpconn_send_frame(self, frame) }
    }

    pub fn flush(&self) {
        tcpconn_flush(self)
    }

    pub fn close(&self) {
        tcpconn_close(self)
    }

    pub fn is_closed(&self) -> bool {
        self.closed_.load(Ordering::Acquire)
    }

    pub fn peer_address(&self) -> String {
        self.peer_address_.clone()
    }

    pub fn set_keepalive(&self, enabled: bool, idle_sec: i32, interval_sec: i32, count: i32) -> bool {
        let _fd_gate = self.outbound_.lock().unwrap();
        // SAFETY: the same gate protects close and descriptor replacement.
        let fd = tcpconn_fd_locked(self);
        if fd < 0 || self.closed_.load(Ordering::Acquire) {
            return false;
        }
        // Disabling does not alter the saved idle/interval/count parameters.
        if unsafe { srpc_tcp_set_keepalive(fd, enabled as i32) } != 0 {
            return false;
        }
        if !enabled {
            return true;
        }
        // Preserve the original policy: attempt every tuning option even if
        // an earlier one fails, then report whether the full update applied.
        let idle_ok = unsafe { srpc_tcp_set_keepalive_idle(fd, idle_sec) } == 0;
        let interval_ok = unsafe { srpc_tcp_set_keepalive_interval(fd, interval_sec) } == 0;
        let count_ok = unsafe { srpc_tcp_set_keepalive_count(fd, count) } == 0;
        idle_ok && interval_ok && count_ok
    }

    pub fn set_on_frame(&self, cb: OnFrameCallback) {
        let mut guard = self.on_frame_.lock().unwrap();
        *guard = cb;
    }

    pub fn set_on_closed(&self, cb: OnClosedCallback) {
        let mut guard = self.on_closed_.lock().unwrap();
        *guard = cb;
    }

    pub fn set_on_error(&self, cb: OnErrorCallback) {
        let mut guard = self.on_error_.lock().unwrap();
        *guard = cb;
    }

    pub fn fd(&self) -> i32 {
        let _guard = self.outbound_.lock().unwrap();
        // SAFETY: `outbound_` serializes access to the descriptor slot.
        tcpconn_fd_locked(self)
    }

    // Retained for the historical C++ surface. Production creation (the
    // factory's connect and the accept driver) installs the poll thread on
    // the owned value, before the Arc is shared and the transport attached.
    pub fn set_poll_thread(&mut self, pt: Arc<PollThread>) {
        self.poll_thread_ = Some(pt);
    }
}

struct TcpChannelShim {
    conn_: Arc<TcpConnection>,
}

#[cfg_attr(any(), cpp_inherit)]
impl ChannelConnectionBase for TcpChannelShim {
    unsafe fn send_frame(&self, frame: &ChannelFrame) -> ChannelError {
        unsafe { self.conn_.send_frame(frame) }
    }
    fn flush(&self) {
        self.conn_.flush()
    }
    fn close(&self) {
        self.conn_.close()
    }
    fn is_closed(&self) -> bool {
        self.conn_.is_closed()
    }
    fn peer_address(&self) -> String {
        self.conn_.peer_address()
    }
    fn set_keepalive(&self, enabled: bool, idle_sec: i32, interval_sec: i32, count: i32) -> bool {
        self.conn_.set_keepalive(enabled, idle_sec, interval_sec, count)
    }
    fn set_on_frame(&mut self, cb: OnFrameCallback) {
        self.conn_.set_on_frame(cb)
    }
    fn set_on_closed(&mut self, cb: OnClosedCallback) {
        self.conn_.set_on_closed(cb)
    }
    fn set_on_error(&mut self, cb: OnErrorCallback) {
        self.conn_.set_on_error(cb)
    }
}

pub fn make_tcp_connection_channel_proxy(conn: Arc<TcpConnection>) -> ChannelConnectionProxy {
    Box::new(TcpChannelShim { conn_: conn })
}

fn io_kind_to_channel_error(kind: LegacyIoErrorKind) -> ChannelError {
    let k_refused = LegacyIoErrorKind::ConnectionRefused;
    let k_reset = LegacyIoErrorKind::ConnectionReset;
    let k_aborted = LegacyIoErrorKind::ConnectionAborted;
    let k_not_connected = LegacyIoErrorKind::NotConnected;
    let k_broken_pipe = LegacyIoErrorKind::BrokenPipe;
    let k_timed_out = LegacyIoErrorKind::TimedOut;
    let k_addr_in_use = LegacyIoErrorKind::AddrInUse;
    let k_addr_not_avail = LegacyIoErrorKind::AddrNotAvailable;
    let k_invalid_input = LegacyIoErrorKind::InvalidInput;
    let k_perm_denied = LegacyIoErrorKind::PermissionDenied;
    let k_would_block = LegacyIoErrorKind::WouldBlock;

    let e_refused = ChannelError::ConnectionRefused;
    let e_reset = ChannelError::ConnectionReset;
    let e_timeout = ChannelError::Timeout;
    let e_addr_in_use = ChannelError::AddressInUse;
    let e_addr_invalid = ChannelError::AddressInvalid;
    let e_perm_denied = ChannelError::PermissionDenied;
    let e_would_block = ChannelError::WouldBlock;
    let e_internal = ChannelError::Internal;

    if kind == k_refused {
        return e_refused;
    }
    if kind == k_reset || kind == k_aborted || kind == k_not_connected || kind == k_broken_pipe {
        return e_reset;
    }
    if kind == k_timed_out {
        return e_timeout;
    }
    if kind == k_addr_in_use {
        return e_addr_in_use;
    }
    if kind == k_addr_not_avail || kind == k_invalid_input {
        return e_addr_invalid;
    }
    if kind == k_perm_denied {
        return e_perm_denied;
    }
    if kind == k_would_block {
        return e_would_block;
    }
    e_internal
}

pub struct TcpListener {
    // The accept task reads the listener while user threads may close it.
    // Mutex/atomics make that ownership boundary explicit and remove the
    // historical RefCell/Cell cross-thread race.
    // `on_accept_` gates both cells. The accept task retains a cloned
    // listener owner until it retires, so close cannot race the reactor
    // through a reused fd.
    // Callback invocation always occurs after the gate has been released.
    listener_: RefCell<Option<Arc<LegacyTcpListener>>>,
    bound_address_: RefCell<String>,
    closed_: AtomicBool,
    listened_: AtomicBool,
    // Reuses the historical padding bytes between the one-byte latches and
    // `poll_thread_`.  This is the owner latch for the whole accept driver,
    // not just its callback window: at most one accept driver may accept or
    // invoke `on_accept_` at a time.  `close` waits for that owner unless it
    // is called reentrantly by the owner itself.
    accept_callback_thread_: AtomicU32,
    poll_thread_: Option<Arc<PollThread>>,
    // Retains the existing set_self_weak API. Registration no longer depends
    // on upgrading this weak pointer: the channel shim owns the listener Arc
    // and registers it immediately after a successful bind.
    self_weak_: Option<ArcWeak<TcpListener>>,
    on_accept_: std::sync::Mutex<OnAcceptCallback>,
    on_error_: std::sync::Mutex<OnErrorCallback>,
}

// SAFETY: `listener_` and `bound_address_` are only accessed while holding
// `on_accept_`; latches are atomic; callbacks have their own mutexes; and the
// Arc/Weak fields are initialized through `&mut self` before publication.
unsafe impl Send for TcpListener {}
unsafe impl Sync for TcpListener {}

impl TcpListener {
    pub fn new() -> TcpListener {
        TcpListener {
            listener_: RefCell::new(None),
            bound_address_: RefCell::<String>::new(Default::default()),
            closed_: AtomicBool::new(false),
            listened_: AtomicBool::new(false),
            accept_callback_thread_: AtomicU32::new(0),
            poll_thread_: None,
            self_weak_: None,
            on_accept_: std::sync::Mutex::<OnAcceptCallback>::new(Default::default()),
            on_error_: std::sync::Mutex::<OnErrorCallback>::new(Default::default()),
        }
    }

    // Bind, parse the IPv4 address, and set nonblocking mode. All field
    // writes through the RefCells (listen runs once; the RefCell
    // replaces the old setup-time const_cast pattern).
    pub fn listen(&self, addr: &str) -> ChannelError {
        if self.closed_.load(Ordering::Acquire) {
            return ChannelError::AddressInUse;
        }
        if self
            .listened_
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return ChannelError::AddressInUse;
        }
        let parse_result = addr.parse::<std::net::SocketAddrV4>();
        if parse_result.is_err() {
            self.listened_.store(false, Ordering::Release);
            return ChannelError::AddressInvalid;
        }
        let parsed = match parse_result {
            Ok(value) => value,
            Err(_) => {
                self.listened_.store(false, Ordering::Release);
                return ChannelError::AddressInvalid;
            }
        };
        let bound: LegacyTcpListener = match LegacyTcpListener::bind(parsed) {
            Ok(value) => value,
            Err(error) => {
                self.listened_.store(false, Ordering::Release);
                return io_kind_to_channel_error(error.kind());
            }
        };
        let nonblock_result = bound.set_nonblocking(true);
        if let Err(error) = nonblock_result {
            let ch = io_kind_to_channel_error(error.kind());
            self.listened_.store(false, Ordering::Release);
            return ch;
        }
        // Discover actual bound address (port may have been 0).
        let local_result = bound.local_addr();
        // Keep the result match as a statement: rusty-cpp otherwise lets the
        // following MutexGuard assignment context leak into the match lambda's
        // return type.
        #[allow(clippy::needless_late_init)]
        let address_string: String;
        match local_result {
            Ok(std::net::SocketAddr::V4(value)) => {
                address_string = value.to_string();
            }
            _ => {
                address_string = addr.to_string();
            }
        }
        // Publish the fully configured listener while holding its lifecycle
        // mutex. `close()` sets the closed latch first and then takes this same
        // mutex, so a concurrent close either wins before publication or
        // waits and removes the newly published fd before it returns.
        let _lifecycle_gate = self.on_accept_.lock().unwrap();
        if self.closed_.load(Ordering::Acquire) {
            return ChannelError::AddressInUse;
        }
        let mut listener_guard = self.listener_.borrow_mut();
        let mut address_guard = self.bound_address_.borrow_mut();
        *address_guard = address_string;
        *listener_guard = Some(Arc::new(bound));
        ChannelError::None
    }

    // Stop accepting immediately and release the listener slot. The accept
    // task's lease keeps the descriptor live only until the task retires.
    pub fn close(&self) {
        // This store and the post-CAS `closed_` recheck are sequentially
        // consistent with the owner CAS/load pair.  That rules out the
        // store-buffering outcome where close misses a new owner while that
        // owner simultaneously misses `closed_` on a weak-memory target.
        self.closed_.store(true, Ordering::SeqCst);
        // Every caller crosses the lifecycle gate and idempotently takes the
        // descriptor.  Otherwise a second close could observe the closed bit
        // set by a first closer and return before that first closer removed
        // the fd or before an active accept driver released ownership.
        {
            let _lifecycle_gate = self.on_accept_.lock().unwrap();
            let mut g = self.listener_.borrow_mut();
            let retired: Option<Arc<LegacyTcpListener>> = g.take();
            if let Some(owner) = retired.as_ref() {
                // Stop new connections while the registration lease keeps the
                // descriptor number reserved through epoll removal.  On a
                // listening socket the shutdown also moves it to CLOSE, which
                // the kernel reports as a hang-up edge: that is what wakes the
                // accept task (S5) to retire and release its lease.
                unsafe { let _ = srpc_tcp_shutdown(owner.as_raw_fd()); }
            }
        }
        // An accept callback may itself call close(); that call must not wait
        // for its own accept-driver invocation to return. Other threads wait
        // until the whole accept-driver scope clears its Linux TID.
        let current_thread = unsafe { srpc_tcp_current_thread_id() };
        while {
            let callback_thread = self.accept_callback_thread_.load(Ordering::SeqCst);
            callback_thread != 0 && callback_thread != current_thread
        } {
            core::hint::spin_loop();
        }
    }

    pub fn is_closed(&self) -> bool {
        self.closed_.load(Ordering::Acquire)
    }

    pub fn local_address(&self) -> String {
        let _lifecycle_gate = self.on_accept_.lock().unwrap();
        let g = self.bound_address_.borrow();
        (*g).clone()
    }

    pub fn set_on_accept(&self, cb: OnAcceptCallback) {
        let mut guard = self.on_accept_.lock().unwrap();
        *guard = cb;
    }

    pub fn set_on_error(&self, cb: OnErrorCallback) {
        let mut guard = self.on_error_.lock().unwrap();
        *guard = cb;
    }

    pub fn fd(&self) -> i32 {
        let _lifecycle_gate = self.on_accept_.lock().unwrap();
        let g = self.listener_.borrow();
        match g.as_ref() {
            // Measured lowering requirement (the same rule reactor.rs's `old`
            // and `poll_ref` rebinds satisfy): the emitter writes `->` only
            // when the receiver's DECLARED type is literally `&Arc<..>` or
            // `&Box<..>`.  It keeps that through a typed local and loses it
            // through an untyped `.as_ref()` match arm, which lowered this
            // call to `owner.as_raw_fd()` on a `rusty::Arc<..>` -- "no member
            // named 'as_raw_fd'" -- and srpc.tcp_channel failed to compile.
            // The other "typed rebind" sites in this file follow the same rule.
            Some(owner) => {
                let owner: &Arc<LegacyTcpListener> = owner;
                owner.as_raw_fd()
            }
            None => -1,
        }
    }

    pub fn set_poll_thread(&mut self, pt: Arc<PollThread>) {
        self.poll_thread_ = Some(pt);
    }

    pub fn set_self_weak(&mut self, self_weak: ArcWeak<TcpListener>) {
        self.self_weak_ = Some(self_weak);
    }
}

struct TcpListenerChannelShim {
    listener_: Arc<TcpListener>,
}

#[cfg_attr(any(), cpp_inherit)]
#[allow(unsafe_code)]
unsafe impl ChannelListenerBase for TcpListenerChannelShim {
    fn listen(&mut self, a: &str) -> ChannelError {
        let result = self.listener_.listen(a);
        if result == ChannelError::None {
            // Start the accept task on the listener's PollThread (S5).
            tcplistener_attach(&self.listener_);
        }
        result
    }
    fn close(&mut self) {
        self.listener_.close()
    }
    fn is_closed(&self) -> bool {
        self.listener_.is_closed()
    }
    fn local_address(&self) -> String {
        self.listener_.local_address()
    }
    fn set_on_accept(&mut self, cb: OnAcceptCallback) {
        self.listener_.set_on_accept(cb)
    }
    fn set_on_error(&mut self, cb: OnErrorCallback) {
        self.listener_.set_on_error(cb)
    }
}

pub fn make_tcp_listener_channel_proxy(listener: Arc<TcpListener>) -> ChannelListenerProxy {
    Box::new(TcpListenerChannelShim {
        listener_: listener,
    })
}

pub struct TcpFactory {
    poll_thread_: Arc<PollThread>,
    connect_timeout_ms_: i32,
}

impl TcpFactory {
    pub fn new(poll_thread: Arc<PollThread>) -> TcpFactory {
        TcpFactory {
            poll_thread_: poll_thread,
            connect_timeout_ms_: 5000i32,
        }
    }

    pub fn backend_name(&self) -> String {
        "tcp".to_string()
    }

    // Socket + connect path (kernel does the syscalls).
    pub fn connect(&self, addr: &str) -> ConnectResult {
        tcp_factory_connect(self, addr)
    }

    pub fn make_listener(&self) -> Option<ChannelListenerProxy> {
        tcp_factory_make_listener(self)
    }

    pub fn set_connect_timeout_ms(&mut self, timeout_ms: i32) {
        self.connect_timeout_ms_ = timeout_ms;
    }
}

struct TcpFactoryShim {
    factory_: Arc<TcpFactory>,
}

#[cfg_attr(any(), cpp_inherit)]
impl ChannelFactoryBase for TcpFactoryShim {
    fn connect(&mut self, addr: &str) -> ConnectResult {
        self.factory_.connect(addr)
    }
    fn make_listener(&mut self) -> Option<ChannelListenerProxy> {
        self.factory_.make_listener()
    }
    fn backend_name(&self) -> String {
        self.factory_.backend_name()
    }
}

pub fn make_tcp_factory_proxy(factory: Arc<TcpFactory>) -> ChannelFactoryProxy {
    Box::new(TcpFactoryShim { factory_: factory })
}

const kRecvScratchBytes: usize = 64 * 1024;

struct RecvScratch {
    arr: [u8; kRecvScratchBytes],
}

// These four helpers precede their first callers because rusty-cpp only emits
// automatic forward declarations for signatures whose local types can be
// reconstructed without an imported enum return. Their leaf dependencies do
// receive declarations in the generated module.
fn tcpconn_errno_to_channel_error(err: i32) -> ChannelError {
    if err == TCP_ERR_CONNECTION_REFUSED {
        return ChannelError::ConnectionRefused;
    }
    if err == TCP_ERR_CONNECTION_RESET || err == TCP_ERR_BROKEN_PIPE || err == TCP_ERR_NOT_CONNECTED
    {
        return ChannelError::ConnectionReset;
    }
    if err == TCP_ERR_TIMED_OUT {
        return ChannelError::Timeout;
    }
    if err == TCP_ERR_ADDR_IN_USE {
        return ChannelError::AddressInUse;
    }
    if err == TCP_ERR_ADDR_NOT_AVAILABLE {
        return ChannelError::AddressInvalid;
    }
    if err == TCP_ERR_ACCES || err == TCP_ERR_OPERATION_NOT_PERMITTED {
        return ChannelError::PermissionDenied;
    }
    if err == TCP_ERR_PROCESS_FD_LIMIT || err == TCP_ERR_SYSTEM_FD_LIMIT {
        return ChannelError::TooManyOpenFiles;
    }
    ChannelError::Internal
}

fn tcpconn_drain_outbound_locked(conn: &TcpConnection, buf: &mut TcpOutBuf) -> ChannelError {
    let mut offset: usize = 0;
    let mut blocked = false;
    while !blocked && offset < buf.len() {
        let io = tcpconn_send_bytes(conn, buf, offset);
        if io.count > 0 {
            offset += io.count as usize;
        } else if io.count == 0 {
            // send returning 0 with bytes remaining = transport reset.
            return ChannelError::ConnectionReset;
        } else {
            let err = io.error;
            if err == TCP_ERR_AGAIN || err == TCP_ERR_WOULD_BLOCK {
                blocked = true;
            } else if err == TCP_ERR_INTERRUPTED {
                // retry — loop continues
            } else {
                // Hard error: drop what we couldn't send (dead anyway).
                tcpconn_drop_after_error(buf, offset);
                return tcpconn_errno_to_channel_error(err);
            }
        }
    }
    tcpconn_trim_sent(buf, offset);
    // WouldBlock whenever send(2) stopped on EAGAIN, even after partial
    // progress: the writer task then consumes its write readiness and waits
    // for the next edge.  (flush treats a partial drain the same either
    // way.)
    if blocked {
        return ChannelError::WouldBlock;
    }
    ChannelError::None
}

fn tcpconn_deliver_on_closed_locked(conn: &TcpConnection, reason: ChannelError) {
    if conn.on_closed_fired_.swap(true, Ordering::AcqRel) {
        return;
    }
    let callback = {
        let guard = conn.on_closed_.lock().unwrap();
        (*guard).clone()
    };
    if callback.has_value() {
        callback.callable()(reason);
    }
}

fn tcpconn_next_frame(conn: &TcpConnection, v: &mut FrameView) -> FrameDecodeStatus {
    let _gate = conn.on_frame_.lock().unwrap();
    let g = conn.inbound_.borrow();
    (*g).next_frame(v)
}

#[allow(unused_unsafe)]
unsafe fn tcpconn_send_frame(conn: &TcpConnection, frame: &ChannelFrame) -> ChannelError {
    if conn.closed_.load(Ordering::Acquire) {
        return ChannelError::ConnectionReset;
    }
    if frame.size > 0usize && frame.payload.is_null() {
        return ChannelError::Internal;
    }
    if frame.size > TCP_MAX_FRAME_PAYLOAD_SIZE {
        return ChannelError::Internal;
    }
    let extended_header_flag = false;
    // Decided before the gate: the TLS read needs no lock.
    let foreign: bool = tcpconn_is_foreign_sender(conn);

    let edge_waker: Option<Waker>;
    {
        let mut guard = conn.outbound_.lock().unwrap();

        // Reject when the queue is already past the high water — we never
        // append to a buffer that's already over budget so backpressure is
        // strictly bounded.
        if (*guard).len() >= conn.outbound_high_water_ {
            return ChannelError::WouldBlock;
        }
        let was_empty: bool = (*guard).is_empty();

        let encoded_size = if extended_header_flag {
            (frame.size as u32 | 0x8000_0000u32) as i32
        } else {
            frame.size as i32
        };
        let header = encoded_size.to_ne_bytes();
        let start = guard.len();
        guard.resize(start + 4usize + frame.size, 0u8);
        guard[start] = header[0];
        guard[start + 1usize] = header[1];
        guard[start + 2usize] = header[2];
        guard[start + 3usize] = header[3];
        if frame.size > 0usize {
            // SAFETY: ChannelFrame promises a readable range of `size` bytes.
            let payload = unsafe { core::slice::from_raw_parts(frame.payload, frame.size) };
            let mut i: usize = 0;
            while i < frame.size {
                guard[start + 4usize + i] = payload[i];
                i += 1usize;
            }
        }
        // The empty->non-empty edge wakes the writer task directly (S5 of
        // docs/dev/lion-runtime-plan.md): from any thread, with no hop
        // through the driver, and at most once per edge, so every frame
        // queued before the writer runs goes out in one drain.  The waker is
        // taken under the gate that published it; a writer not parked on
        // the edge is already woken, draining, or waiting for write
        // readiness, and a writer not yet started drains on its first poll.
        // On the poll thread a wake is a push onto its local ready queue.
        //
        // Write-through (experiment on lion/s5-writethrough): a sender on
        // another thread that finds the buffer empty writes the frame
        // itself, here under the gate, and wakes the writer only when
        // send(2) did not take all of it.  Nothing is queued ahead of the
        // frame (the buffer was empty) and every other send, drain and
        // flush holds the same gate, so bytes leave in gate order and each
        // sender's frames stay in order.  The writer stays the only user of
        // write readiness: this path never polls or consumes it, and hands
        // an EAGAIN, a short write or an error back as queued bytes, which
        // the writer then sends, waits on or fails with, as before.
        //
        // The cork (lion/s5-cork): only a connection idle for
        // kTcpWriteThroughIdleUs writes through.  A busier one keeps the
        // frame queued and wakes the writer, as S5 does, so the frames that
        // follow within the interval ride one drain.
        let mut handed_back: bool = was_empty;
        if was_empty && foreign && tcpconn_idle_for_write_through(conn) {
            handed_back = !tcpconn_write_through_locked(conn, &mut *guard);
        }
        edge_waker = if handed_back { tcpconn_take_writer_locked(conn) } else { None };
        drop(guard);
    }
    if let Some(waker) = edge_waker {
        waker.wake();
    }
    ChannelError::None
}

// Whether a send on this thread may write through: the connection has a
// PollThread (so a writer task exists or will) and this is not that thread.
// On the PollThread a send keeps S5's path: a reply inside the reader's poll
// wakes the writer on the local ready queue, and one drain carries them all.
fn tcpconn_is_foreign_sender(conn: &TcpConnection) -> bool {
    match conn.poll_thread_.as_ref() {
        Some(pt) => {
            let pt: &Arc<PollThread> = pt;
            !pt.is_current_thread()
        }
        None => false,
    }
}

// Whether the connection's last send(2) is at least kTcpWriteThroughIdleUs
// old (or there was none).  One monotonic clock read, only for a foreign
// sender that found the buffer empty.
fn tcpconn_idle_for_write_through(conn: &TcpConnection) -> bool {
    if kTcpWriteThroughIdleUs == 0 {
        return true;
    }
    let last: u64 = conn.last_send_us_.load(Ordering::Relaxed);
    if last == 0 {
        return true;
    }
    let now: u64 = crate::basetypes::Time::now(true);
    now >= last + kTcpWriteThroughIdleUs
}

// Under the outbound gate: send what the socket takes from the buffer, the
// sender's own frame (the buffer was empty before it).  True when all of it
// went out.  Otherwise the sent prefix is trimmed and the rest stays queued
// for the writer: after EAGAIN it waits for write readiness as always.  A
// hard error (or a zero-byte send) is recorded in send_error_ for the tasks
// to report, since this send consumed the socket's pending error; the bytes
// stay queued, not dropped as tcpconn_drain_outbound_locked drops them for
// callers that report the error themselves.  EINTR retries.
fn tcpconn_write_through_locked(conn: &TcpConnection, buf: &mut TcpOutBuf) -> bool {
    let mut offset: usize = 0;
    let mut stopped: bool = false;
    while !stopped && offset < buf.len() {
        let io = tcpconn_send_bytes(conn, buf, offset);
        if io.count > 0 {
            offset += io.count as usize;
        } else if io.count < 0 && io.error == TCP_ERR_INTERRUPTED {
            // retry
        } else {
            stopped = true;
            let fd_open: bool = tcpconn_fd_locked(conn) >= 0;
            let hard: bool = io.count == 0 || (io.error != TCP_ERR_AGAIN && io.error != TCP_ERR_WOULD_BLOCK);
            if fd_open && hard {
                let err: i32 = if io.count == 0 { TCP_ERR_CONNECTION_RESET } else { io.error };
                let _first = conn.send_error_.compare_exchange(0, err, Ordering::AcqRel, Ordering::Acquire);
            }
        }
    }
    tcpconn_trim_sent(buf, offset);
    buf.is_empty()
}

// The hard send error a write-through sender recorded, as a ChannelError, or
// None.
fn tcpconn_recorded_send_error(conn: &TcpConnection) -> ChannelError {
    let err: i32 = conn.send_error_.load(Ordering::Acquire);
    if err == 0 {
        return ChannelError::None;
    }
    tcpconn_errno_to_channel_error(err)
}

fn tcpconn_flush(conn: &TcpConnection) {
    if conn.closed_.load(Ordering::Acquire) {
        return;
    }
    let writer: Option<Waker>;
    {
        let mut guard = conn.outbound_.lock().unwrap();
        if (*guard).is_empty() {
            return;
        }
        // Best-effort immediate drain; errors are reported via the
        // connection callbacks on the next poll cycle (see the pre-DSL
        // comment history for the poll-thread hand-off rationale).
        let result = tcpconn_drain_outbound_locked(conn, &mut *guard);
        if result != ChannelError::None && result != ChannelError::WouldBlock {
            conn.closed_.store(true, Ordering::Release);
            // The transport tasks retire a closed connection and close it,
            // which delivers on_closed; wake the writer so that happens now.
            writer = tcpconn_take_writer_locked(conn);
        } else {
            writer = None;
        }
    }
    if let Some(waker) = writer {
        waker.wake();
    }
}

// Callers hold outbound_, which gates the writer slot.
fn tcpconn_take_writer_locked(conn: &TcpConnection) -> Option<Waker> {
    // SAFETY: this helper is called only inside the outbound gate.
    let slot = unsafe { &mut *conn.writer_.get() };
    slot.take()
}

// Callers hold outbound_, which protects this descriptor slot.
fn tcpconn_fd_locked(conn: &TcpConnection) -> i32 {
    // SAFETY: this helper is called only inside the outbound gate.
    let slot = unsafe { &*conn.fd_.get() };
    match slot.as_ref() {
        // Typed rebind: measured lowering requirement, see TcpListener::fd.
        Some(owner) => {
            let owner: &Arc<LegacyOwnedFd> = owner;
            owner.as_raw_fd()
        }
        None => -1,
    }
}

fn tcpconn_close(conn: &TcpConnection) {
    conn.closed_.store(true, Ordering::Release);
    // Every closer crosses the descriptor gate. Observing the closed bit
    // alone does not prove a concurrent closer has removed the slot yet.
    let writer: Option<Waker>;
    {
        let _fd_gate = conn.outbound_.lock().unwrap();
        // SAFETY: `outbound_` serializes every descriptor access/mutation.
        let slot = unsafe { &mut *conn.fd_.get() };
        let retired: Option<Arc<LegacyOwnedFd>> = slot.take();
        if let Some(owner) = retired.as_ref() {
            // The local owner keeps the descriptor live for shutdown, even
            // when a worker releases its registration lease concurrently.
            unsafe { let _ = srpc_tcp_shutdown(owner.as_raw_fd()); }
        }
        writer = tcpconn_take_writer_locked(conn);
    }
    // Wake the writer task so the transport retires from any thread's close.
    // A writer parked on an empty buffer holds no readiness waker, so the
    // shutdown's hang-up edge alone reaches only the reader.
    if let Some(waker) = writer {
        waker.wake();
    }
    // A failed flush may already have set closed_ without notifying. The
    // callback's independent fired latch makes this reentrant and exactly once.
    tcpconn_deliver_on_closed_locked(conn, ChannelError::None);
}

fn tcpconn_reset_fd(conn: &TcpConnection) {
    let _fd_gate = conn.outbound_.lock().unwrap();
    // SAFETY: `outbound_` serializes every descriptor access and mutation.
    let slot = unsafe { &mut *conn.fd_.get() };
    let retired: Option<Arc<LegacyOwnedFd>> = slot.take();
    if let Some(owner) = retired.as_ref() {
        unsafe { let _ = srpc_tcp_shutdown(owner.as_raw_fd()); }
    }
}

// Decode and deliver every complete buffered frame to on_frame, in order.
// False when the inbound stream is malformed: the connection is then failed
// and closed.  Called by the reader task (S5).
fn tcpconn_deliver_frames(conn: &TcpConnection) -> bool {
    // Foreign-enum variants hoisted into UNTYPED `let`s: dropping the
    // expected type routes them through ordinary PATH emission, so the
    // comparison is a plain `s == st_complete`. (The old `(s as i32) ==
    // (X as i32)` double-cast dodged the same variant-ACCESSOR
    // mis-lowering, but hid what was being compared.)
    let st_complete = FrameDecodeStatus::Complete;
    let st_need_more = FrameDecodeStatus::NeedMoreBytes;
    let mut decoding = true;
    while decoding {
        let mut v = FrameView {
            header: FrameHeader {
                payload_size: 0,
                extended_header_flag: false,
            },
            payload: core::ptr::null(),
            payload_size: 0,
        };
        let s = tcpconn_next_frame(conn, &mut v);
        if s == st_complete {
            let cf = ChannelFrame {
                payload: v.payload,
                size: v.payload_size,
            };
            {
                let callback = {
                    let guard = conn.on_frame_.lock().unwrap();
                    (*guard).clone()
                };
                if callback.has_value() {
                    callback.callable()(&cf);
                }
            }
            tcpconn_consume_inbound(conn);
        } else if s == st_need_more {
            decoding = false;
        } else {
            // Malformed inbound stream.
            tcpconn_report_error(conn, ChannelError::Internal, "malformed frame on inbound stream");
            conn.closed_.store(true, Ordering::Release);
            tcpconn_reset_fd(conn);
            tcpconn_reset_inbound(conn);
            tcpconn_deliver_on_closed_locked(conn, ChannelError::Internal);
            return false;
        }
    }
    true
}

// Invoke on_error, if installed, outside its mutex.
fn tcpconn_report_error(conn: &TcpConnection, ch: ChannelError, what: &str) {
    let callback = {
        let guard = conn.on_error_.lock().unwrap();
        (*guard).clone()
    };
    if callback.has_value() {
        callback.callable()(ch, what);
    }
}

// A fatal transport error: on_error, then the close latch, the descriptor
// slot, and on_closed with the same reason.
fn tcpconn_fail(conn: &TcpConnection, ch: ChannelError, what: &str) {
    tcpconn_report_error(conn, ch, what);
    conn.closed_.store(true, Ordering::Release);
    tcpconn_reset_fd(conn);
    tcpconn_deliver_on_closed_locked(conn, ch);
}

// The peer closed cleanly (recv read EOF): no on_error, just the close.
fn tcpconn_peer_closed(conn: &TcpConnection) {
    conn.closed_.store(true, Ordering::Release);
    tcpconn_reset_fd(conn);
    tcpconn_deliver_on_closed_locked(conn, ChannelError::None);
}

fn tcpconn_scratch() -> *mut RecvScratch {
    // SAFETY: the C seam returns this thread's 64-KiB aligned byte storage.
    unsafe { srpc_tcp_recv_scratch() as *mut RecvScratch }
}

struct TcpIoResult {
    count: i64,
    error: i32,
}

// One recv(2) of up to a scratch buffer from `fd`, which the caller keeps
// open (the outbound gate, or a transport task's descriptor lease).
fn tcpconn_recv_fd(fd: i32, s: *mut RecvScratch) -> TcpIoResult {
    // SAFETY: `s` is the current thread's full RecvScratch allocation.
    let count =
        unsafe { srpc_tcp_recv_bytes(fd, (*s).arr.as_mut_ptr(), kRecvScratchBytes) };
    // Capture the seam's thread-local errno before another transport call
    // can replace the snapshot.
    let error = if count < 0 {
        unsafe { srpc_tcp_last_errno() }
    } else {
        0
    };
    TcpIoResult { count, error }
}

fn tcpconn_append_inbound(conn: &TcpConnection, n: usize) {
    let s = tcpconn_scratch();
    let _gate = conn.on_frame_.lock().unwrap();
    let mut guard = conn.inbound_.borrow_mut();
    // SAFETY: `s` remains the current thread's scratch and `n` is the
    // nonnegative byte count returned by the immediately preceding recv.
    unsafe { guard.append((*s).arr.as_ptr(), n) };
}

fn tcpconn_consume_inbound(conn: &TcpConnection) {
    let _gate = conn.on_frame_.lock().unwrap();
    let mut guard = conn.inbound_.borrow_mut();
    guard.consume_frame();
}

fn tcpconn_reset_inbound(conn: &TcpConnection) {
    let _gate = conn.on_frame_.lock().unwrap();
    let mut guard = conn.inbound_.borrow_mut();
    guard.reset();
}

fn tcpconn_send_bytes(conn: &TcpConnection, buf: &mut TcpOutBuf, offset: usize) -> TcpIoResult {
    // The only callers hold `outbound_`, which is also the fd lifetime gate.
    // SAFETY: the caller holds `outbound_`, which gates this UnsafeCell.
    let fd = tcpconn_fd_locked(conn);
    if fd < 0 {
        return TcpIoResult { count: 0, error: 0 };
    }
    let remaining = buf.len() - offset;
    let count = unsafe { srpc_tcp_send_bytes(fd, buf.as_ptr().add(offset), remaining) };
    if count > 0 && kTcpWriteThroughIdleUs != 0 {
        // For the cork: the connection just sent.
        conn.last_send_us_.store(crate::basetypes::Time::now(true), Ordering::Relaxed);
    }
    let error = if count < 0 {
        unsafe { srpc_tcp_last_errno() }
    } else {
        0
    };
    TcpIoResult { count, error }
}

// Drop the prefix that send(2) actually accepted.
fn tcpconn_trim_sent(buf: &mut TcpOutBuf, offset: usize) {
    if offset == 0 {
        return;
    }
    if offset == buf.len() {
        buf.clear();
    } else {
        let remaining = buf.len() - offset;
        unsafe {
            core::ptr::copy(buf.as_ptr().add(offset), buf.as_mut_ptr(), remaining);
        }
        buf.resize(remaining, 0u8);
    }
}

// Hard-error cleanup: drop the sent prefix, or everything when nothing
// was sent (the connection is dead).
fn tcpconn_drop_after_error(buf: &mut TcpOutBuf, offset: usize) {
    if offset > 0 {
        tcpconn_trim_sent(buf, offset);
    } else {
        buf.clear();
    }
}

fn set_nonblocking_fd(fd: i32) -> i32 {
    // SAFETY: the caller keeps `fd` live for this operation.
    let flags = unsafe { srpc_tcp_get_flags(fd) };
    if flags < 0 {
        return tcpconn_last_errno();
    }
    let rc = unsafe { srpc_tcp_set_nonblocking_flags(fd, flags) };
    if rc < 0 {
        return tcpconn_last_errno();
    }
    0
}

fn tcpconn_last_errno() -> i32 {
    // SAFETY: this reads the C seam's thread-local errno snapshot.
    unsafe { srpc_tcp_last_errno() }
}

struct AcceptStep {
    ch: ChannelError,
    proxy: Option<ChannelConnectionProxy>,
    connection: Option<Arc<TcpConnection>>,
    // Set when accept(2) reported EAGAIN: the backlog is drained.
    would_block: bool,
}

struct TcpListenerHandleReadScope {
    owner_thread_: *const AtomicU32,
    acquired_: bool,
}

impl TcpListenerHandleReadScope {
    fn new(listener: &TcpListener) -> TcpListenerHandleReadScope {
        let thread_id = unsafe { srpc_tcp_current_thread_id() };
        let acquired = listener
            .accept_callback_thread_
            .compare_exchange(0, thread_id, Ordering::SeqCst, Ordering::SeqCst)
            .is_ok();
        TcpListenerHandleReadScope {
            owner_thread_: &raw const listener.accept_callback_thread_,
            acquired_: acquired,
        }
    }

    fn acquired(&self) -> bool {
        self.acquired_
    }
}

impl Drop for TcpListenerHandleReadScope {
    fn drop(&mut self) {
        if self.acquired_ {
            // SAFETY: the listener remains borrowed for the full synchronous
            // accept-driver invocation containing this scope guard.
            unsafe {
                (*self.owner_thread_).store(0, Ordering::Release);
            }
        }
    }
}

// The DSL has no default field initializers, so the old `ch =
// ChannelError::None` member init moves into the factory — both
// construction sites already go through it.
fn tcplistener_accept_step_new() -> AcceptStep {
    AcceptStep {
        ch: ChannelError::None,
        proxy: None,
        connection: None,
        would_block: false,
    }
}

fn tcplistener_take_proxy(s: &mut AcceptStep) -> ChannelConnectionProxy {
    s.proxy.take().unwrap()
}

fn tcplistener_close_accepted(s: &mut AcceptStep) {
    if let Some(connection) = s.connection.take() {
        tcpconn_close(&connection);
    }
}

// The accept driver the accept task runs on each read edge (the retired
// pollable `handle_read` ran it too).  `drained` is set when it stopped because
// accept(2) reported EAGAIN, the only evidence that lets the accept task
// consume its read readiness (S5).
fn tcplistener_accept_until_blocked(lst: &TcpListener, drained: &mut bool) -> bool {
    if lst.closed_.load(Ordering::SeqCst) {
        return false;
    }
    // Serialize the entire accept driver, including callback invocation.
    // A contending call and same-thread callback recursion both return
    // without touching the listener.  After acquiring, recheck `closed_` to
    // close the race in which `close` observed no owner before this CAS.
    let _owner_scope = TcpListenerHandleReadScope::new(lst);
    if !_owner_scope.acquired() {
        return false;
    }
    if lst.closed_.load(Ordering::SeqCst) {
        return false;
    }
    if !tcplistener_is_bound(lst) {
        return false;
    }
    let mut any_progress = false;
    let mut accepting = true;
    while accepting {
        if lst.closed_.load(Ordering::Acquire) {
            break;
        }
        let mut step = tcplistener_accept_step_new();
        let rc = tcplistener_accept_step(lst, &raw mut step);
        if rc == 1 {
            #[allow(unused_mut)]
            let mut accepted = tcplistener_take_proxy(&mut step);
            if lst.closed_.load(Ordering::Acquire) {
                tcplistener_close_accepted(&mut step);
                accepting = false;
                continue;
            }
            any_progress = true;
            let callback = {
                let guard = lst.on_accept_.lock().unwrap();
                (*guard).clone()
            };
            if callback.has_value() {
                // Close either set the latch before the whole-driver owner
                // was installed (and is observed above/here) or sees the
                // active owner TID and waits for this driver to return.
                // Thus the callback cannot begin after a concurrent close
                // has returned.
                if lst.closed_.load(Ordering::Acquire) {
                    tcplistener_close_accepted(&mut step);
                    accepting = false;
                    continue;
                }
                let connection: NullableChannelConnectionProxy = Some(accepted);
                callback.callable()(connection);
            }
        } else if rc == 0 {
            if step.would_block {
                *drained = true;
            }
            accepting = false;
        } else if rc == 2 {
            let callback = {
                let guard = lst.on_error_.lock().unwrap();
                (*guard).clone()
            };
            if callback.has_value() {
                callback.callable()(step.ch, "accept: failed to set non-blocking");
            }
        } else {
            {
                let callback = {
                    let guard = lst.on_error_.lock().unwrap();
                    (*guard).clone()
                };
                if callback.has_value() {
                    callback.callable()(step.ch, "socket accept failed");
                }
            }
            lst.close();
            return any_progress;
        }
    }
    any_progress
}

fn tcplistener_is_bound(lst: &TcpListener) -> bool {
    let _lifecycle_gate = lst.on_accept_.lock().unwrap();
    let g = lst.listener_.borrow();
    g.is_some()
}

#[allow(clippy::unnecessary_unwrap)]
fn tcplistener_accept_step(lst: &TcpListener, out: *mut AcceptStep) -> i32 {
    // SAFETY: every caller passes its live stack-local AcceptStep for the
    // duration of this synchronous accept attempt.
    let out: &mut AcceptStep = unsafe { &mut *out };
    // `on_accept_` is also the lifecycle gate.  It keeps the RefCell and its
    // descriptor stable against concurrent listen/close while accept runs.
    let _lifecycle_gate = lst.on_accept_.lock().unwrap();
    if lst.closed_.load(Ordering::Acquire) {
        return 0;
    }
    let listener_guard = lst.listener_.borrow();
    // Typed local: measured lowering requirement, see TcpListener::fd.
    let listener: &Arc<LegacyTcpListener> = match listener_guard.as_ref() {
        Some(owner) => owner,
        None => return 0,
    };
    let accept_result = listener.accept();
    if accept_result.is_err() {
        let err = accept_result.unwrap_err();
        let kind = err.kind();
        // Retriable / "no work" -- the loop breaks without spinning.
        if kind == LegacyIoErrorKind::WouldBlock {
            out.would_block = true;
            return 0;
        }
        if kind == LegacyIoErrorKind::Interrupted || kind == LegacyIoErrorKind::ConnectionAborted {
            return 0;
        }
        out.ch = io_kind_to_channel_error(kind);
        return -1;
    }
    let (stream, peer_addr) = accept_result.unwrap();

    let nonblock_result = stream.set_nonblocking(true);
    if let Err(error) = nonblock_result {
        out.ch = io_kind_to_channel_error(error.kind());
        return 2; // stream drops here, closing the accepted fd.
    }

    // Keep the early return in the accept function during C++ lowering.
    #[allow(clippy::needless_late_init)]
    let peer_v4: LegacySocketAddrV4;
    match peer_addr {
        std::net::SocketAddr::V4(value) => {
            peer_v4 = value;
        }
        _ => {
            out.ch = ChannelError::AddressInvalid;
            return 2;
        }
    }
    let peer_addr_str = peer_v4.to_string();

    // Hand the accepted fd to TcpConnection.
    let conn_fd = stream.into_raw_fd();
    // SAFETY: accept transferred the freshly created descriptor into this
    // connection; no other owner remains after into_raw_fd above.
    let mut conn = Arc::new(unsafe { TcpConnection::new(conn_fd, peer_addr_str) });

    if let Some(pt) = lst.poll_thread_.as_ref() {
        // The Arc is still uniquely owned, so this is the safe minting
        // window for installing the worker before either proxy clones it.
        Arc::get_mut(&mut conn).unwrap().set_poll_thread(pt.clone());
        // Start its transport tasks: in place when this accept runs on that
        // PollThread (the accept task), else through a job.  They first run
        // after this accept driver returns, by which time on_accept has
        // installed the connection's callbacks.
        tcpconn_attach(&conn);
    }

    out.connection = Some(conn.clone());
    out.proxy = Some(make_tcp_connection_channel_proxy(conn));
    1
}

fn connect_errno_to_channel_error(err: i32) -> ChannelError {
    if err == TCP_ERR_CONNECTION_REFUSED {
        return ChannelError::ConnectionRefused;
    }
    if err == TCP_ERR_CONNECTION_RESET || err == TCP_ERR_BROKEN_PIPE {
        return ChannelError::ConnectionReset;
    }
    if err == TCP_ERR_TIMED_OUT {
        return ChannelError::Timeout;
    }
    if err == TCP_ERR_HOST_UNREACHABLE
        || err == TCP_ERR_NETWORK_UNREACHABLE
        || err == TCP_ERR_ADDR_NOT_AVAILABLE
    {
        return ChannelError::AddressInvalid;
    }
    if err == TCP_ERR_ACCES || err == TCP_ERR_OPERATION_NOT_PERMITTED {
        return ChannelError::PermissionDenied;
    }
    if err == TCP_ERR_PROCESS_FD_LIMIT || err == TCP_ERR_SYSTEM_FD_LIMIT {
        return ChannelError::TooManyOpenFiles;
    }
    ChannelError::Internal
}

// Own the entire connection attempt here so Rust and C++ execute the same
// timeout, descriptor cleanup, and self-connect decisions. Address values
// retain their socket representation, including network byte order.
fn tcp_connect_socket(addr_be: u32, port_be: u16, timeout_ms: i32, out_errno: &mut i32) -> i32 {
    let fd = unsafe { srpc_tcp_socket_open() };
    if fd < 0 {
        *out_errno = tcpconn_last_errno();
        return -1;
    }
    let nonblocking_error = set_nonblocking_fd(fd);
    if nonblocking_error != 0 {
        *out_errno = nonblocking_error;
        unsafe { srpc_tcp_close(fd) };
        return -1;
    }
    let result = unsafe { srpc_tcp_connect_once(fd, addr_be, port_be) };
    if result < 0 {
        let error = tcpconn_last_errno();
        if error == unsafe { srpc_tcp_in_progress_errno() } && timeout_ms > 0 {
            let ready = unsafe { srpc_tcp_wait_writable_once(fd, timeout_ms) };
            if ready == 0 {
                unsafe { srpc_tcp_close(fd) };
                return -2;
            }
            if ready < 0 {
                *out_errno = tcpconn_last_errno();
                unsafe { srpc_tcp_close(fd) };
                return -1;
            }
            let mut socket_error: i32 = 0;
            let status = unsafe { srpc_tcp_socket_error(fd, &raw mut socket_error) };
            if status < 0 || socket_error != 0 {
                if socket_error != 0 {
                    *out_errno = socket_error;
                } else {
                    *out_errno = tcpconn_last_errno();
                }
                unsafe { srpc_tcp_close(fd) };
                return -1;
            }
        } else if error != unsafe { srpc_tcp_is_connected_errno() } {
            *out_errno = error;
            unsafe { srpc_tcp_close(fd) };
            return -1;
        }
    }
    if tcp_socket_is_self_connected(fd, addr_be, port_be) {
        unsafe { srpc_tcp_close(fd) };
        return -3;
    }
    fd
}

fn tcp_socket_is_self_connected(fd: i32, addr_be: u32, port_be: u16) -> bool {
    let mut local_address: u32 = 0;
    let mut local_port: u16 = 0;
    let local_status = unsafe {
        srpc_tcp_local_endpoint(fd, &raw mut local_address, &raw mut local_port)
    };
    local_status == 0 && local_address == addr_be && local_port == port_be
}

fn tcp_factory_connect_socket(
    peer: LegacySocketAddrV4,
    connect_timeout_ms: i32,
    err_out: &mut ChannelError,
) -> i32 {
    let octets = peer.ip().octets();
    // Preserve the address octets as network-order bytes in the native integer.
    let addr_be = u32::from_ne_bytes(octets);
    let port_be = peer.port().to_be();
    let mut err_no: i32 = 0;
    let fd = tcp_connect_socket(
            addr_be,
            port_be,
            connect_timeout_ms,
            &mut err_no,
        );
    if fd >= 0i32 {
        return fd;
    }
    // Each ChannelError is hoisted into a local instead of being written
    // straight into the deref: a factory call whose assignment target is
    // `*err_out` mis-resolves as `ChannelError::ChannelError::Timeout`.
    if fd == -2i32 {
        let ch = ChannelError::Timeout;
        *err_out = ch;
    } else if fd == -3i32 {
        let ch = ChannelError::ConnectionRefused;
        *err_out = ch;
    } else {
        let ch = connect_errno_to_channel_error(err_no);
        *err_out = ch;
    }
    -1i32
}

pub fn tcp_factory_connect(fac: &TcpFactory, addr: &str) -> ConnectResult {
    let parse_result = addr.parse::<std::net::SocketAddrV4>();
    if parse_result.is_err() {
        return ConnectResult {
            connection: None,
            error: ChannelError::AddressInvalid,
        };
    }
    let mut err: ChannelError = ChannelError::None;
    let parsed = match parse_result {
        Ok(value) => value,
        Err(_) => {
            return ConnectResult {
                connection: None,
                error: ChannelError::AddressInvalid,
            };
        }
    };
    let fd = tcp_factory_connect_socket(parsed, fac.connect_timeout_ms_, &mut err);
    if fd < 0i32 {
        return ConnectResult {
            connection: None,
            error: err,
        };
    }

    // SAFETY: tcp_connect_socket returned a fresh descriptor whose
    // ownership is transferred exactly once into TcpConnection.
    let mut conn = Arc::new(unsafe { TcpConnection::new(fd, addr.to_string()) });
    Arc::get_mut(&mut conn)
        .unwrap()
        .set_poll_thread(fac.poll_thread_.clone());
    // The connect above blocked the calling thread, as it always has; the
    // connected socket now goes to the PollThread, which starts its
    // transport tasks (through a job, unless this is that thread).
    tcpconn_attach(&conn);

    ConnectResult {
        connection: Some(make_tcp_connection_channel_proxy(conn)),
        error: ChannelError::None,
    }
}

pub fn tcp_factory_make_listener(self_: &TcpFactory) -> Option<ChannelListenerProxy> {
    let mut listener = Arc::new(TcpListener::new());
    Arc::get_mut(&mut listener)
        .unwrap()
        .set_poll_thread(self_.poll_thread_.clone());
    Some(make_tcp_listener_channel_proxy(listener))
}

// ---------------------------------------------------------------------------
// Transport tasks (S5 of docs/dev/lion-runtime-plan.md)
// ---------------------------------------------------------------------------
//
// On its PollThread a connection is two Lion local tasks over one AsyncFd,
// one waiter per direction (U8):
//
// * The reader waits for read readiness, reads into the FrameStreamReader and
//   delivers every complete frame to on_frame: fast RPCs
//   still run inline here, fiber RPCs start their fiber here, and stackless
//   RPCs make their first poll here.  It reads until recv(2) reports EAGAIN,
//   the only evidence that consumes readiness: a short read delivers and then
//   reads again, which also picks up whatever arrived meanwhile.  A poll
//   reads at most a budget of times, then yields.
// * The writer drains the outbound buffer while the socket takes it.  It
//   waits either on the buffer's empty->non-empty edge, which send_frame
//   wakes from any thread through the waker published under the outbound
//   gate (no driver hop, no latch sweep), or on write readiness after
//   send(2) reported EAGAIN.  A reply sent inside the reader's poll wakes it
//   on this thread's own ready queue, so it runs in the same tick, and one
//   drain carries every reply that poll produced.
//
// A listener is one accept task, which runs the accept driver on
// each read edge and consumes the readiness only when accept(2) reported
// EAGAIN.  close() needs no waker for it: shutting a listening socket down
// moves it to CLOSE, which the kernel reports as a hang-up edge, and Lion
// wakes the task's read wait for it.
//
// Close and error semantics are those the retired pollable shims had.  A task that
// finds the connection closed (by any thread's close, a failed flush, or its
// own EOF or error path) retires the transport: it closes the connection
// (idempotent; on_closed fires once), drops the AsyncFd and then the
// descriptor lease (Lion requires the fd to outlive its AsyncFd), and wakes
// the other task, which then finishes.  close() wakes a parked writer
// itself, and its shutdown(2) raises a hang-up edge, which Lion reports to
// both directions.  When the PollThread shuts down, its runtime drops the
// tasks; the registration is released without closing the connection, as
// the epoll loop's shutdown cleanup did.
//
// Connections accepted by the accept task start their tasks in place; a
// connect made on another thread, and a listen, hand over through a job.

// A connection's registration, shared by its reader and writer tasks.
struct TcpTransport {
    conn_: Arc<TcpConnection>,
    // Dropped before `lease_`, explicitly (tcp_transport_release): Lion
    // requires the descriptor to stay open until its AsyncFd is dropped, and
    // the generated C++ destroys fields in the reverse of Rust's order.
    async_fd_: RefCell<Option<lion_reactor::AsyncFd>>,
    lease_: RefCell<Option<Arc<LegacyOwnedFd>>>,
    // The reader task's waker, for the writer's retirement to wake it.
    reader_: RefCell<Option<Waker>>,
}

impl Drop for TcpTransport {
    fn drop(&mut self) {
        tcp_transport_release(self);
    }
}

// Hand a connection to its PollThread's runtime: in place when this is that
// thread, else through a job that starts the tasks there.
fn tcpconn_attach(conn: &Arc<TcpConnection>) {
    let pt: &Arc<PollThread> = match conn.poll_thread_.as_ref() {
        Some(pt) => pt,
        None => return,
    };
    if pt.is_current_thread() {
        tcpconn_start_transport(conn.clone());
        return;
    }
    // FnMut, run once: the connection is parked in an Option the call takes.
    let mut parked: Option<Arc<TcpConnection>> = Some(conn.clone());
    let start: Arc<OneTimeJob> = Arc::<OneTimeJob>::new(OneTimeJob::new(Box::new(move || {
        let taken: Option<Arc<TcpConnection>> = parked.take();
        if let Some(connection) = taken {
            tcpconn_start_transport(connection);
        }
    })));
    // Erase the job type for the worker command queue.
    let start_erased: Arc<dyn crate::misc::Job> = start;
    pt.add(start_erased);
}

// On the PollThread: register the connection's descriptor with Lion and
// spawn its reader and writer tasks.  A connection closed before this runs
// has no descriptor left and needs nothing more.
fn tcpconn_start_transport(conn: Arc<TcpConnection>) {
    let lease: Option<Arc<LegacyOwnedFd>> = {
        let _gate = conn.outbound_.lock().unwrap();
        // SAFETY: the outbound gate serializes this clone with logical close.
        unsafe { (&*conn.fd_.get()).clone() }
    };
    if lease.is_none() {
        return;
    }
    let lease: Arc<LegacyOwnedFd> = lease.unwrap();
    // Typed rebind: measured lowering requirement, see TcpListener::fd.
    let fd: i32 = {
        let owner: &Arc<LegacyOwnedFd> = &lease;
        owner.as_raw_fd()
    };
    let registered = lion_reactor::AsyncFd::new(fd);
    if registered.is_err() {
        // No Lion reactor on this thread, or the backend refused the fd: fail
        // the connection rather than leave it open and never read.
        drop(lease);
        tcpconn_fail(&conn, ChannelError::Internal, "poll registration failed");
        return;
    }
    let transport: Rc<TcpTransport> = Rc::new(TcpTransport {
        conn_: conn,
        async_fd_: RefCell::new(Some(registered.unwrap())),
        lease_: RefCell::new(Some(lease)),
        reader_: RefCell::new(None),
    });
    // Detached: each task ends by retiring the transport, and the runtime's
    // drop takes whatever is left at shutdown.
    let reader = TcpReaderTask { transport_: transport.clone() };
    let writer = TcpWriterTask { transport_: transport };
    let reader_handle: lion_executor::JoinHandle<()> = lion_executor::spawn_local(reader);
    drop(reader_handle);
    let writer_handle: lion_executor::JoinHandle<()> = lion_executor::spawn_local(writer);
    drop(writer_handle);
}

fn tcp_transport_is_retired(t: &TcpTransport) -> bool {
    let fd_guard = t.async_fd_.borrow();
    (*fd_guard).is_none()
}

// The leased descriptor, or -1 once released.
fn tcp_transport_fd(t: &TcpTransport) -> i32 {
    let lease_guard = t.lease_.borrow();
    match (*lease_guard).as_ref() {
        // Typed rebind: measured lowering requirement, see TcpListener::fd.
        Some(owner) => {
            let owner: &Arc<LegacyOwnedFd> = owner;
            owner.as_raw_fd()
        }
        None => -1,
    }
}

// Whether the descriptor may be ready in one direction; when not, `cx`'s
// waker is registered for that direction's next edge.
fn tcp_transport_ready(t: &TcpTransport, cx: &mut Context<'_>, write: bool) -> bool {
    let fd_guard = t.async_fd_.borrow();
    match (*fd_guard).as_ref() {
        Some(async_fd) => crate::reactor::lion_fd_poll_ready(async_fd, cx, write),
        None => false,
    }
}

// Consume one direction's readiness right after its operation reported
// EAGAIN, in the same poll (U8 invariant 2).
fn tcp_transport_consume(t: &TcpTransport, cx: &mut Context<'_>, write: bool) {
    let fd_guard = t.async_fd_.borrow();
    if let Some(async_fd) = (*fd_guard).as_ref() {
        let _consumed: bool = crate::reactor::lion_fd_consume_ready(async_fd, cx, write);
    }
}

// Deregister the descriptor from Lion, then release the lease, in that order,
// and forget the writer's waker so a late send_frame finds none.  Idempotent.
fn tcp_transport_release(t: &TcpTransport) {
    let async_fd: Option<lion_reactor::AsyncFd> = {
        let mut fd_guard = t.async_fd_.borrow_mut();
        (*fd_guard).take()
    };
    drop(async_fd);
    let lease: Option<Arc<LegacyOwnedFd>> = {
        let mut lease_guard = t.lease_.borrow_mut();
        (*lease_guard).take()
    };
    drop(lease);
    let writer: Option<Waker> = {
        let _gate = t.conn_.outbound_.lock().unwrap();
        tcpconn_take_writer_locked(&t.conn_)
    };
    drop(writer);
}

// Retire the transport: close the connection (idempotent; on_closed fires at
// most once, and close wakes a parked writer), release the registration and
// wake the reader.  The calling task has dropped its own waker first.
fn tcp_transport_retire(t: &TcpTransport) {
    tcpconn_close(&t.conn_);
    let reader: Option<Waker> = {
        let mut reader_guard = t.reader_.borrow_mut();
        (*reader_guard).take()
    };
    tcp_transport_release(t);
    if let Some(waker) = reader {
        waker.wake();
    }
}

// The reader task.  Unpin, so poll can reach its fields through get_mut.
struct TcpReaderTask {
    transport_: Rc<TcpTransport>,
}

impl Future for TcpReaderTask {
    type Output = ();

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        let this: &mut TcpReaderTask = self.get_mut();
        let mut unwind = crate::reactor::PollTaskUnwindAbort { armed: true };
        let result: Poll<()> = tcp_reader_poll(&this.transport_, cx);
        unwind.armed = false;
        result
    }
}

fn tcp_reader_poll(t: &TcpTransport, cx: &mut Context<'_>) -> Poll<()> {
    if tcp_transport_is_retired(t) {
        tcp_reader_unpark(t);
        return Poll::Ready(());
    }
    let conn: &TcpConnection = &t.conn_;
    if conn.closed_.load(Ordering::Acquire) {
        tcp_reader_finish(t);
        return Poll::Ready(());
    }
    {
        let mut reader_guard = t.reader_.borrow_mut();
        *reader_guard = Some(cx.waker().clone());
    }
    let fd: i32 = tcp_transport_fd(t);
    // Reads per poll before the reader yields to the thread's other tasks.
    let budget: u32 = 16u32;
    let mut reads: u32 = 0u32;
    // Whether full reads appended bytes that are not yet delivered.
    let mut undelivered: bool = false;
    loop {
        if !tcp_transport_ready(t, cx, false) {
            return Poll::Pending;
        }
        let io = tcpconn_recv_fd(fd, tcpconn_scratch());
        reads += 1u32;
        if io.count > 0 {
            tcpconn_append_inbound(conn, io.count as usize);
            if (io.count as usize) == kRecvScratchBytes && reads < budget {
                // More is probably queued: read it before decoding.
                undelivered = true;
                continue;
            }
            undelivered = false;
            if !tcp_reader_deliver(t) {
                return Poll::Ready(());
            }
            if reads >= budget {
                // The readiness is still set, so nothing else would wake
                // this task: come back after the thread's other tasks.
                cx.waker().wake_by_ref();
                return Poll::Pending;
            }
            continue;
        }
        if io.count == 0 {
            // EOF.  Deliver the frames received whole before it, then close
            // cleanly: no on_error, and on_closed(None).
            if undelivered && !tcp_reader_deliver(t) {
                return Poll::Ready(());
            }
            // A write-through sender's send consumed a reset that this EOF
            // stands for: report that send failure, as the writer would.
            let recorded: ChannelError = tcpconn_recorded_send_error(conn);
            if recorded != ChannelError::None {
                tcpconn_fail(conn, recorded, "outbound write failed");
            } else {
                tcpconn_peer_closed(conn);
            }
            tcp_reader_finish(t);
            return Poll::Ready(());
        }
        let err: i32 = io.error;
        if err == TCP_ERR_AGAIN || err == TCP_ERR_WOULD_BLOCK {
            // recv(2) disproved the readiness: consume it now, in the poll
            // that saw the EAGAIN (U8), then deliver and wait for an edge.
            tcp_transport_consume(t, cx, false);
            if undelivered {
                undelivered = false;
                if !tcp_reader_deliver(t) {
                    return Poll::Ready(());
                }
            }
            continue;
        }
        if err == TCP_ERR_INTERRUPTED {
            continue;
        }
        tcpconn_fail(conn, tcpconn_errno_to_channel_error(err), "socket receive failed");
        tcp_reader_finish(t);
        return Poll::Ready(());
    }
}

// Deliver the buffered frames.  False when the reader is done: the stream
// was malformed (the connection is then failed and closed), or a callback
// closed the connection.
fn tcp_reader_deliver(t: &TcpTransport) -> bool {
    let delivered: bool = tcpconn_deliver_frames(&t.conn_);
    if !delivered || t.conn_.closed_.load(Ordering::Acquire) {
        tcp_reader_finish(t);
        return false;
    }
    true
}

fn tcp_reader_unpark(t: &TcpTransport) {
    let mut reader_guard = t.reader_.borrow_mut();
    *reader_guard = None;
}

fn tcp_reader_finish(t: &TcpTransport) {
    tcp_reader_unpark(t);
    tcp_transport_retire(t);
}

// The writer task.  Unpin, so poll can reach its fields through get_mut.
struct TcpWriterTask {
    transport_: Rc<TcpTransport>,
}

impl Future for TcpWriterTask {
    type Output = ();

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        let this: &mut TcpWriterTask = self.get_mut();
        let mut unwind = crate::reactor::PollTaskUnwindAbort { armed: true };
        let result: Poll<()> = tcp_writer_poll(&this.transport_, cx);
        unwind.armed = false;
        result
    }
}

fn tcp_writer_poll(t: &TcpTransport, cx: &mut Context<'_>) -> Poll<()> {
    if tcp_transport_is_retired(t) {
        return Poll::Ready(());
    }
    let conn: &TcpConnection = &t.conn_;
    // Drains per poll before the writer yields, for a sender that refills
    // the buffer as fast as the socket empties it.
    let budget: u32 = 16u32;
    let mut drains: u32 = 0u32;
    loop {
        let mut closed: bool = false;
        let has_output: bool = tcp_writer_park(conn, cx.waker(), &mut closed);
        if closed {
            tcp_writer_finish(t);
            return Poll::Ready(());
        }
        if !has_output {
            // Waiting for the buffer's empty->non-empty edge.
            return Poll::Pending;
        }
        if !tcp_transport_ready(t, cx, true) {
            // Waiting for write readiness.
            return Poll::Pending;
        }
        if drains >= budget {
            // Still writable, so nothing else would wake this task.
            cx.waker().wake_by_ref();
            return Poll::Pending;
        }
        drains += 1u32;
        let mut closed_now: bool = false;
        let result: ChannelError = tcp_writer_drain(conn, &mut closed_now);
        if closed_now {
            tcp_writer_finish(t);
            return Poll::Ready(());
        }
        if result == ChannelError::WouldBlock {
            // send(2) disproved the readiness: consume it now (U8); the next
            // round waits for the next write edge.
            tcp_transport_consume(t, cx, true);
            continue;
        }
        if result != ChannelError::None {
            tcpconn_fail(conn, result, "outbound write failed");
            tcp_writer_finish(t);
            return Poll::Ready(());
        }
        // Drained: look again, the buffer may have refilled meanwhile.
    }
}

// Under the outbound gate: publish the writer's waker and report whether
// output is queued; `closed` when the connection is closed.
fn tcp_writer_park(conn: &TcpConnection, waker: &Waker, closed: &mut bool) -> bool {
    let guard = conn.outbound_.lock().unwrap();
    if conn.closed_.load(Ordering::Acquire) {
        *closed = true;
        return false;
    }
    // SAFETY: the outbound gate is held.
    let slot = unsafe { &mut *conn.writer_.get() };
    *slot = Some(waker.clone());
    !(*guard).is_empty()
}

// Under the outbound gate: write what the socket takes.  The closed check
// shares the gate with the send, so a concurrent close never turns into a
// spurious write error; `closed` reports it instead.
fn tcp_writer_drain(conn: &TcpConnection, closed: &mut bool) -> ChannelError {
    let mut guard = conn.outbound_.lock().unwrap();
    if conn.closed_.load(Ordering::Acquire) {
        *closed = true;
        return ChannelError::None;
    }
    if (*guard).is_empty() {
        return ChannelError::None;
    }
    // A write-through sender already met a hard error on this socket.
    let recorded: ChannelError = tcpconn_recorded_send_error(conn);
    if recorded != ChannelError::None {
        return recorded;
    }
    tcpconn_drain_outbound_locked(conn, &mut *guard)
}

fn tcp_writer_finish(t: &TcpTransport) {
    let own: Option<Waker> = {
        let _gate = t.conn_.outbound_.lock().unwrap();
        tcpconn_take_writer_locked(&t.conn_)
    };
    drop(own);
    tcp_transport_retire(t);
}

// Hand a bound listener to its PollThread's runtime, as tcpconn_attach does
// for a connection.
fn tcplistener_attach(listener: &Arc<TcpListener>) {
    let pt: &Arc<PollThread> = match listener.poll_thread_.as_ref() {
        Some(pt) => pt,
        None => return,
    };
    if pt.is_current_thread() {
        tcplistener_start_accept(listener.clone());
        return;
    }
    let mut parked: Option<Arc<TcpListener>> = Some(listener.clone());
    let start: Arc<OneTimeJob> = Arc::<OneTimeJob>::new(OneTimeJob::new(Box::new(move || {
        let taken: Option<Arc<TcpListener>> = parked.take();
        if let Some(bound) = taken {
            tcplistener_start_accept(bound);
        }
    })));
    // Erase the job type for the worker command queue.
    let start_erased: Arc<dyn crate::misc::Job> = start;
    pt.add(start_erased);
}

// On the PollThread: register the listener's descriptor with Lion and spawn
// its accept task.  A listener closed before this runs needs nothing more.
fn tcplistener_start_accept(listener: Arc<TcpListener>) {
    let lease: Option<Arc<LegacyTcpListener>> = {
        let _gate = listener.on_accept_.lock().unwrap();
        let slot = listener.listener_.borrow();
        (*slot).clone()
    };
    if lease.is_none() {
        return;
    }
    let lease: Arc<LegacyTcpListener> = lease.unwrap();
    // Typed rebind: measured lowering requirement, see TcpListener::fd.
    let fd: i32 = {
        let owner: &Arc<LegacyTcpListener> = &lease;
        owner.as_raw_fd()
    };
    let registered = lion_reactor::AsyncFd::new(fd);
    if registered.is_err() {
        drop(lease);
        let callback = {
            let guard = listener.on_error_.lock().unwrap();
            (*guard).clone()
        };
        if callback.has_value() {
            callback.callable()(ChannelError::Internal, "poll registration failed");
        }
        listener.close();
        return;
    }
    let task = TcpAcceptTask {
        listener_: listener,
        async_fd_: Some(registered.unwrap()),
        lease_: Some(lease),
    };
    let handle: lion_executor::JoinHandle<()> = lion_executor::spawn_local(task);
    drop(handle);
}

// The accept task.  Unpin, so poll can reach its fields through get_mut.
struct TcpAcceptTask {
    listener_: Arc<TcpListener>,
    // Dropped before `lease_`, explicitly (tcp_accept_release).
    async_fd_: Option<lion_reactor::AsyncFd>,
    lease_: Option<Arc<LegacyTcpListener>>,
}

impl Future for TcpAcceptTask {
    type Output = ();

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        let this: &mut TcpAcceptTask = self.get_mut();
        let mut unwind = crate::reactor::PollTaskUnwindAbort { armed: true };
        let result: Poll<()> = tcp_accept_poll(this, cx);
        unwind.armed = false;
        result
    }
}

impl Drop for TcpAcceptTask {
    fn drop(&mut self) {
        tcp_accept_release(self);
    }
}

fn tcp_accept_poll(task: &mut TcpAcceptTask, cx: &mut Context<'_>) -> Poll<()> {
    if task.async_fd_.is_none() {
        return Poll::Ready(());
    }
    let listener: Arc<TcpListener> = task.listener_.clone();
    let lst: &TcpListener = &listener;
    if lst.closed_.load(Ordering::SeqCst) {
        tcp_accept_release(task);
        return Poll::Ready(());
    }
    loop {
        let ready: bool = match task.async_fd_.as_ref() {
            Some(async_fd) => crate::reactor::lion_fd_poll_ready(async_fd, cx, false),
            None => false,
        };
        if !ready {
            return Poll::Pending;
        }
        let mut drained: bool = false;
        tcplistener_accept_until_blocked(lst, &mut drained);
        if lst.closed_.load(Ordering::SeqCst) {
            tcp_accept_release(task);
            return Poll::Ready(());
        }
        if !drained {
            // Stopped before EAGAIN (interrupted, an aborted connection, or
            // another accept driver still running): the readiness stays set,
            // so come back after the thread's other tasks.
            cx.waker().wake_by_ref();
            return Poll::Pending;
        }
        // accept(2) disproved the readiness: consume it now (U8).
        if let Some(async_fd) = task.async_fd_.as_ref() {
            let _consumed: bool = crate::reactor::lion_fd_consume_ready(async_fd, cx, false);
        }
    }
}

// Deregister the descriptor from Lion, then release the lease, in that order.
// Idempotent.
fn tcp_accept_release(task: &mut TcpAcceptTask) {
    let async_fd: Option<lion_reactor::AsyncFd> = task.async_fd_.take();
    drop(async_fd);
    let lease: Option<Arc<LegacyTcpListener>> = task.lease_.take();
    drop(lease);
}

#[cfg(test)]
#[path = "../tests/helpers/tcp_cork.rs"]
mod tcp_cork_tests;

#[cfg(test)]
mod native_connect_tests {
    use super::*;
    use std::io::{Read, Write};
    use std::net::{Ipv4Addr, SocketAddr, TcpListener as NativeListener, TcpStream};
    use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};

    extern "C" {
        fn listen(fd: i32, backlog: i32) -> i32;
        fn bind(fd: i32, address: *const SockaddrIn, length: u32) -> i32;
    }

    #[repr(C)]
    struct SockaddrIn {
        family: u16,
        port: u16,
        address: u32,
        padding: [u8; 8],
    }

    fn native_address(listener: &NativeListener) -> (u32, u16) {
        let SocketAddr::V4(address) = listener.local_addr().unwrap() else {
            panic!("IPv4 listener expected");
        };
        (u32::from_ne_bytes(address.ip().octets()), address.port().to_be())
    }

    #[test]
    fn canonical_connect_transfers_a_working_nonblocking_socket() {
        let listener = NativeListener::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
        let (address, port) = native_address(&listener);
        let mut error = 0;
        let fd = tcp_connect_socket(address, port, 1000, &mut error);
        assert!(fd >= 0, "connect error {error}");
        // SAFETY: the successful attempt transfers a unique owned descriptor.
        let mut stream = unsafe { TcpStream::from_raw_fd(fd) };
        let (mut accepted, _) = listener.accept().unwrap();
        assert!(!tcp_socket_is_self_connected(fd, address, port));
        assert_eq!(stream.read(&mut [0u8; 1]).unwrap_err().kind(), std::io::ErrorKind::WouldBlock);
        stream.write_all(b"canonical").unwrap();
        let mut received = [0u8; 9];
        accepted.read_exact(&mut received).unwrap();
        assert_eq!(&received, b"canonical");
    }

    #[test]
    fn canonical_connect_reports_refusal_from_socket_error() {
        // Keep the port bound, but not listening, for the whole attempt: the
        // kernel still refuses the SYN, and no concurrently running test can
        // bind(0) the port. A dropped listener frees it, and with a reserved
        // port band (as in mako's CI container) bind(0) hands that same first
        // free port to most callers.
        let raw = unsafe { srpc_tcp_socket_open() };
        assert!(raw >= 0);
        // SAFETY: socket_open returns a unique descriptor on success.
        let socket = unsafe { OwnedFd::from_raw_fd(raw) };
        let any_port = SockaddrIn {
            family: 2,
            port: 0,
            address: u32::from_ne_bytes(Ipv4Addr::LOCALHOST.octets()),
            padding: [0; 8],
        };
        assert_eq!(unsafe { bind(socket.as_raw_fd(), &any_port, std::mem::size_of::<SockaddrIn>() as u32) }, 0);
        let mut address = 0;
        let mut port = 0;
        assert_eq!(unsafe { srpc_tcp_local_endpoint(raw, &raw mut address, &raw mut port) }, 0);
        let mut error = 0;
        assert_eq!(tcp_connect_socket(address, port, 1000, &mut error), -1);
        assert_eq!(error, TCP_ERR_CONNECTION_REFUSED);
    }

    #[test]
    fn canonical_connect_times_out_when_the_accept_queue_is_full() {
        let listener = NativeListener::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
        // Linux permits one completed connection for a backlog of zero.
        // Keep it queued so the next handshake cannot complete.
        assert_eq!(unsafe { listen(listener.as_raw_fd(), 0) }, 0);
        let queued = TcpStream::connect(listener.local_addr().unwrap()).unwrap();
        let (address, port) = native_address(&listener);
        let mut error = 0;
        assert_eq!(tcp_connect_socket(address, port, 30, &mut error), -2);
        drop(queued);
    }

    #[test]
    fn self_connect_guard_recognizes_a_real_simultaneous_open() {
        let raw = unsafe { srpc_tcp_socket_open() };
        assert!(raw >= 0);
        // SAFETY: socket_open returns a unique descriptor on success.
        let socket = unsafe { OwnedFd::from_raw_fd(raw) };
        let address = SockaddrIn {
            family: 2,
            port: 0,
            address: u32::from_ne_bytes(Ipv4Addr::LOCALHOST.octets()),
            padding: [0; 8],
        };
        assert_eq!(unsafe { bind(socket.as_raw_fd(), &address, std::mem::size_of::<SockaddrIn>() as u32) }, 0);
        let mut local_address = 0;
        let mut local_port = 0;
        assert_eq!(unsafe { srpc_tcp_local_endpoint(raw, &raw mut local_address, &raw mut local_port) }, 0);
        assert_eq!(set_nonblocking_fd(raw), 0);
        let status = unsafe { srpc_tcp_connect_once(raw, local_address, local_port) };
        assert!(status == 0 || tcpconn_last_errno() == unsafe { srpc_tcp_in_progress_errno() });
        assert!(unsafe { srpc_tcp_wait_writable_once(raw, 1000) } > 0);
        let mut socket_error = 0;
        assert_eq!(unsafe { srpc_tcp_socket_error(raw, &raw mut socket_error) }, 0);
        assert_eq!(socket_error, 0);
        assert!(tcp_socket_is_self_connected(raw, local_address, local_port));
    }
}
