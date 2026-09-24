//! TCP channel backend with all cross-thread connection state serialized by
//! the existing outbound mutex.  The poll worker remains the sole owner of
//! inbound decoder state; user-thread send/close operations never access it.

#![allow(
    non_camel_case_types,
    non_snake_case,
    unsafe_code,
    clippy::explicit_auto_deref
)]

#[allow(unused_imports)]
use crate::reactor as _;

use std::cell::{RefCell, UnsafeCell};
use std::os::fd::{AsRawFd, FromRawFd, IntoRawFd};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Weak as ArcWeak};

use crate::channel::{
    ChannelConnectionBase, ChannelConnectionProxy, ChannelError, ChannelFactoryBase,
    ChannelFactoryProxy, ChannelFrame, ChannelListenerBase, ChannelListenerProxy, ConnectResult,
    NullableChannelConnectionProxy, OnAcceptCallback, OnClosedCallback, OnErrorCallback, OnFrameCallback,
};
use crate::frame_codec::{FrameDecodeStatus, FrameHeader, FrameStreamReader, FrameView};
use crate::pollable_proxy::{PollableBase, PollableProxy};


type TcpOutBuf = Vec<u8>;
type LegacyOwnedFd = std::os::fd::OwnedFd;
type LegacyTcpListener = std::net::TcpListener;
type LegacySocketAddrV4 = std::net::SocketAddrV4;
type LegacyIoErrorKind = std::io::ErrorKind;
type PollThread = crate::reactor::PollThread;

pub const kTcpConnectionOutboundHighWaterDefault: usize = 4 * 1024 * 1024; // 4 MiB

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
const TCP_POLL_READ: i32 = 1;
const TCP_POLL_WRITE: i32 = 2;
const TCP_POLL_NO_CHANGE: i32 = -1;
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
    // The outbound mutex gates the descriptor slot. Pollable registrations
    // clone its owner before enqueue and retain it through epoll removal.
    // Logical close clears this slot and shuts down the socket immediately;
    // the last registration lease releases the actual descriptor.
    fd_: UnsafeCell<Option<Arc<LegacyOwnedFd>>>,
    peer_address_: String,
    outbound_high_water_: usize,
    outbound_: std::sync::Mutex<TcpOutBuf>,
    inbound_: RefCell<FrameStreamReader>,
    closed_: AtomicBool,
    on_closed_fired_: AtomicBool,
    pending_write_update_: AtomicBool,
    poll_thread_: Option<Arc<PollThread>>,
    on_frame_: std::sync::Mutex<OnFrameCallback>,
    on_closed_: std::sync::Mutex<OnClosedCallback>,
    on_error_: std::sync::Mutex<OnErrorCallback>,
}

// SAFETY: all state reachable through shared references is either immutable
// after publication, atomic, or protected by an existing mutex:
//
// * `fd_` and `outbound_` are protected by `outbound_`;
// * `inbound_` is poll-worker-owned and every access (including the safe
//   `content_size` observer) is protected by `on_frame_`;
// * callbacks are protected by their corresponding mutexes; and
// * `poll_thread_` is installed through `&mut self` before the Arc is shared.
//
// The sole mutating inbound entry point is unsafe and documents the reactor's
// single-poll-worker precondition, so safe Rust cannot create two competing
// decoder operations.
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
            pending_write_update_: AtomicBool::new(false),
            poll_thread_: None,
            on_frame_: std::sync::Mutex::<OnFrameCallback>::new(Default::default()),
            on_closed_: std::sync::Mutex::<OnClosedCallback>::new(Default::default()),
            on_error_: std::sync::Mutex::<OnErrorCallback>::new(Default::default()),
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

    // READ always; WRITE only while the outbound buffer is non-empty.
    pub fn poll_mode(&self) -> i32 {
        let mut mode: i32 = TCP_POLL_READ;
        let guard = self.outbound_.lock().unwrap();
        if !(*guard).is_empty() {
            mode |= TCP_POLL_WRITE;
        }
        mode
    }

    pub fn content_size(&self) -> usize {
        let inbound_size = {
            let _gate = self.on_frame_.lock().unwrap();
            self.inbound_.borrow().buffered_bytes()
        };
        let guard = self.outbound_.lock().unwrap();
        (*guard).len() + inbound_size
    }

    /// Drive this connection's receive decoder.
    ///
    /// # Safety
    ///
    /// The caller must be this connection's registered poll worker, and no
    /// other call to `handle_read` may overlap it.  The reactor satisfies this
    /// contract by serializing pollable callbacks on one worker.
    pub unsafe fn handle_read(&self) -> bool {
        unsafe { tcpconn_handle_read(self) }
    }

    pub fn handle_write(&self) -> i32 {
        tcpconn_handle_write(self)
    }

    pub fn handle_error(&self) {
        tcpconn_handle_error(self)
    }

    pub fn check_pending_write_update(&self) -> bool {
        self.pending_write_update_.swap(false, Ordering::AcqRel)
    }

    // Retained for the historical C++ surface. Production creation uses the
    // atomic new_registered path so the poll thread is installed before the
    // Arc is shared.
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

struct TcpPollableShim {
    conn_: Arc<TcpConnection>,
    fd_lease_: Option<Arc<LegacyOwnedFd>>,
}

#[cfg_attr(any(), cpp_inherit)]
impl PollableBase for TcpPollableShim {
    fn fd(&self) -> i32 {
        match self.fd_lease_.as_ref() {
            // Measured lowering requirement -- the same rule reactor.rs's
            // `old` and `poll_ref` rebinds satisfy. The emitter writes `->`
            // only when the receiver's DECLARED type is literally `&Arc<..>`
            // or `&Box<..>`; it keeps that through a typed local (see the
            // `retired` binding in tcpconn_close) and loses it through an
            // untyped `.as_ref()` match arm, which lowered this call to
            // `owner.as_raw_fd()` on a `rusty::Arc<OwnedFd>` -- "no member
            // named 'as_raw_fd'" -- and srpc.tcp_channel failed to compile.
            // Four more arms below carry the same one-line rebind.
            Some(owner) => {
                let owner: &Arc<LegacyOwnedFd> = owner;
                owner.as_raw_fd()
            }
            None => -1,
        }
    }
    fn poll_mode(&self) -> i32 {
        self.conn_.poll_mode()
    }
    fn content_size(&mut self) -> usize {
        self.conn_.content_size()
    }
    fn handle_read(&mut self) -> bool {
        // SAFETY: Pollable callbacks are serialized by the owning poll worker.
        unsafe { self.conn_.handle_read() }
    }
    fn handle_write(&mut self) -> i32 {
        self.conn_.handle_write()
    }
    fn handle_error(&mut self) {
        self.conn_.handle_error()
    }
    fn close(&mut self) {
        self.conn_.close()
    }
    fn check_pending_write_update(&self) -> bool {
        self.conn_.check_pending_write_update()
    }
    fn is_closed(&self) -> bool {
        self.conn_.is_closed()
    }
}

pub fn make_tcp_connection_channel_proxy(conn: Arc<TcpConnection>) -> ChannelConnectionProxy {
    Box::new(TcpChannelShim { conn_: conn })
}

pub(crate) fn make_tcp_connection_pollable_proxy(conn: Arc<TcpConnection>) -> PollableProxy {
    let fd_lease: Option<Arc<LegacyOwnedFd>> = {
        let _gate = conn.outbound_.lock().unwrap();
        // SAFETY: the outbound gate serializes this clone with logical close.
        unsafe { (&*conn.fd_.get()).clone() }
    };
    Box::new(TcpPollableShim { conn_: conn, fd_lease_: fd_lease })
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
    // The poll worker reads the listener while user threads may close it.
    // Mutex/atomics make that ownership boundary explicit and remove the
    // historical RefCell/Cell cross-thread race.
    // `on_accept_` gates both cells. Registrations retain a cloned listener
    // owner until unregister, so close cannot race epoll through a reused fd.
    // Callback invocation always occurs after the gate has been released.
    listener_: RefCell<Option<Arc<LegacyTcpListener>>>,
    bound_address_: RefCell<String>,
    closed_: AtomicBool,
    listened_: AtomicBool,
    // Reuses the historical padding bytes between the one-byte latches and
    // `poll_thread_`.  This is the owner latch for the whole accept driver,
    // not just its callback window: at most one `handle_read` may accept or
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

    // Stop accepting immediately and release the listener slot. A pollable
    // registration keeps the descriptor live only until it unregisters.
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
                // descriptor number reserved through epoll removal.
                unsafe { let _ = srpc_tcp_shutdown(owner.as_raw_fd()); }
            }
        }
        // An accept callback may itself call close(); that call must not wait
        // for its own `handle_read` invocation to return. Other threads wait
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
            // Typed rebind: measured lowering requirement, see TcpPollableShim::fd.
            Some(owner) => {
                let owner: &Arc<LegacyTcpListener> = owner;
                owner.as_raw_fd()
            }
            None => -1,
        }
    }

    pub fn poll_mode(&self) -> i32 {
        TCP_POLL_READ
    }

    pub fn content_size(&self) -> usize {
        0usize
    }

    pub fn handle_read(&self) -> bool {
        tcplistener_handle_read(self)
    }

    pub fn handle_write(&self) -> i32 {
        TCP_POLL_NO_CHANGE
    }

    pub fn handle_error(&self) {
        tcplistener_handle_error(self)
    }

    pub fn check_pending_write_update(&self) -> bool {
        false
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
            if let Some(pt) = self.listener_.poll_thread_.as_ref() {
                // SAFETY: the proxy owns an Arc to this successfully bound
                // listener and is moved into the poll command queue.
                                    crate::reactor::PollThread::add_proxy(
                        &**pt,
                        make_tcp_listener_pollable_proxy(self.listener_.clone()),
                    );
            }
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

struct TcpListenerPollableShim {
    listener_: Arc<TcpListener>,
    fd_lease_: Option<Arc<LegacyTcpListener>>,
}

#[cfg_attr(any(), cpp_inherit)]
impl PollableBase for TcpListenerPollableShim {
    fn fd(&self) -> i32 {
        match self.fd_lease_.as_ref() {
            // Typed rebind: measured lowering requirement, see TcpPollableShim::fd.
            Some(owner) => {
                let owner: &Arc<LegacyTcpListener> = owner;
                owner.as_raw_fd()
            }
            None => -1,
        }
    }
    fn poll_mode(&self) -> i32 {
        self.listener_.poll_mode()
    }
    fn content_size(&mut self) -> usize {
        self.listener_.content_size()
    }
    fn handle_read(&mut self) -> bool {
        self.listener_.handle_read()
    }
    fn handle_write(&mut self) -> i32 {
        self.listener_.handle_write()
    }
    fn handle_error(&mut self) {
        self.listener_.handle_error()
    }
    fn close(&mut self) {
        self.listener_.close()
    }
    fn check_pending_write_update(&self) -> bool {
        self.listener_.check_pending_write_update()
    }
    fn is_closed(&self) -> bool {
        self.listener_.is_closed()
    }
}

pub fn make_tcp_listener_channel_proxy(listener: Arc<TcpListener>) -> ChannelListenerProxy {
    Box::new(TcpListenerChannelShim {
        listener_: listener,
    })
}

pub(crate) fn make_tcp_listener_pollable_proxy(listener: Arc<TcpListener>) -> PollableProxy {
    let fd_lease: Option<Arc<LegacyTcpListener>> = {
        let _gate = listener.on_accept_.lock().unwrap();
        let slot = listener.listener_.borrow();
        (*slot).clone()
    };
    Box::new(TcpListenerPollableShim {
        listener_: listener,
        fd_lease_: fd_lease,
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
    if offset == 0 && !buf.is_empty() {
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

    {
        let mut guard = conn.outbound_.lock().unwrap();

        // Reject when the queue is already past the high water — we never
        // append to a buffer that's already over budget so backpressure is
        // strictly bounded.
        if (*guard).len() >= conn.outbound_high_water_ {
            return ChannelError::WouldBlock;
        }

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
    }

    // Publish against the connection itself. The worker reads this flag
    // through its owned registration; a delayed raw-fd command could instead
    // update an unrelated socket after this connection's descriptor is reused.
    conn.pending_write_update_.store(true, Ordering::Release);
    ChannelError::None
}

fn tcpconn_flush(conn: &TcpConnection) {
    if conn.closed_.load(Ordering::Acquire) {
        return;
    }
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
    }
}

// Callers hold outbound_, which protects this descriptor slot.
fn tcpconn_fd_locked(conn: &TcpConnection) -> i32 {
    // SAFETY: this helper is called only inside the outbound gate.
    let slot = unsafe { &*conn.fd_.get() };
    match slot.as_ref() {
        // Typed rebind: measured lowering requirement, see TcpPollableShim::fd.
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

/// # Safety
///
/// Calls for one connection must be serialized by its poll worker.
unsafe fn tcpconn_handle_read(conn: &TcpConnection) -> bool {
    if conn.closed_.load(Ordering::Acquire) {
        return false;
    }
    let mut any_progress = false;
    let mut draining = true;
    while draining {
        let scratch = tcpconn_scratch();
        let io = tcpconn_recv_bytes(conn, scratch);
        if io.count > 0 {
            tcpconn_append_inbound(conn, io.count as usize);
            any_progress = true;
            if (io.count as usize) < kRecvScratchBytes {
                draining = false;
            }
        } else if io.count == 0 {
            // Peer closed cleanly: no on_error, just the close latch.
            conn.closed_.store(true, Ordering::Release);
            tcpconn_reset_fd(conn);
            tcpconn_deliver_on_closed_locked(conn, ChannelError::None);
            return false;
        } else {
            let err = io.error;
            if err == TCP_ERR_AGAIN || err == TCP_ERR_WOULD_BLOCK {
                draining = false;
            } else if err == TCP_ERR_INTERRUPTED {
                // retry — loop continues
            } else {
                let ch = tcpconn_errno_to_channel_error(err);
                {
                    let callback = {
                        let guard = conn.on_error_.lock().unwrap();
                        (*guard).clone()
                    };
                    if callback.has_value() {
                        callback.callable()(ch, "socket receive failed");
                    }
                }
                conn.closed_.store(true, Ordering::Release);
                tcpconn_reset_fd(conn);
                tcpconn_deliver_on_closed_locked(conn, ch);
                return false;
            }
        }
    }

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
            {
                let callback = {
                    let guard = conn.on_error_.lock().unwrap();
                    (*guard).clone()
                };
                if callback.has_value() {
                    callback.callable()(
                        ChannelError::Internal,
                        "malformed frame on inbound stream",
                    );
                }
            }
            conn.closed_.store(true, Ordering::Release);
            tcpconn_reset_fd(conn);
            tcpconn_reset_inbound(conn);
            tcpconn_deliver_on_closed_locked(conn, ChannelError::Internal);
            return false;
        }
    }
    any_progress
}

fn tcpconn_scratch() -> *mut RecvScratch {
    // SAFETY: the C seam returns this thread's 64-KiB aligned byte storage.
    unsafe { srpc_tcp_recv_scratch() as *mut RecvScratch }
}

struct TcpIoResult {
    count: i64,
    error: i32,
}

fn tcpconn_recv_bytes(conn: &TcpConnection, s: *mut RecvScratch) -> TcpIoResult {
    let _fd_gate = conn.outbound_.lock().unwrap();
    // SAFETY: `outbound_` serializes every descriptor access/mutation.
    let fd = tcpconn_fd_locked(conn);
    if fd < 0 {
        return TcpIoResult { count: 0, error: 0 };
    }
    // SAFETY: `s` is the current thread's full RecvScratch allocation and
    // the descriptor remains owned while the mutex guard is held.
    let count =
        unsafe { srpc_tcp_recv_bytes(fd, (*s).arr.as_mut_ptr(), kRecvScratchBytes) };
    // Capture the seam's thread-local errno before this scope unlocks the fd
    // gate or another transport call can replace the snapshot.
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

fn tcpconn_handle_write(conn: &TcpConnection) -> i32 {
    if conn.closed_.load(Ordering::Acquire) {
        return TCP_POLL_NO_CHANGE;
    }
    let result: ChannelError;
    {
        let mut guard = conn.outbound_.lock().unwrap();
        if (*guard).is_empty() {
            return TCP_POLL_READ;
        }
        result = tcpconn_drain_outbound_locked(conn, &mut *guard);
        if result == ChannelError::None {
            if (*guard).is_empty() {
                return TCP_POLL_READ;
            }
            return TCP_POLL_NO_CHANGE;
        }
        if result == ChannelError::WouldBlock {
            return TCP_POLL_NO_CHANGE;
        }
    }
    {
        let callback = {
            let guard = conn.on_error_.lock().unwrap();
            (*guard).clone()
        };
        if callback.has_value() {
            callback.callable()(result, "outbound write failed");
        }
    }
    conn.closed_.store(true, Ordering::Release);
    tcpconn_reset_fd(conn);
    tcpconn_deliver_on_closed_locked(conn, result);
    TCP_POLL_READ
}

fn tcpconn_handle_error(conn: &TcpConnection) {
    if conn.closed_.load(Ordering::Acquire) {
        return;
    }
    {
        let callback = {
            let guard = conn.on_error_.lock().unwrap();
            (*guard).clone()
        };
        if callback.has_value() {
            callback.callable()(ChannelError::Internal, "epoll/poll signaled error");
        }
    }
    tcpconn_close(conn);
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
            // `handle_read` invocation containing this scope guard.
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

fn tcplistener_handle_read(lst: &TcpListener) -> bool {
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
                // active owner TID and waits for this handle_read to return.
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
    // Typed local: measured lowering requirement, see TcpPollableShim::fd.
    let listener: &Arc<LegacyTcpListener> = match listener_guard.as_ref() {
        Some(owner) => owner,
        None => return 0,
    };
    let accept_result = listener.accept();
    if accept_result.is_err() {
        let err = accept_result.unwrap_err();
        let kind = err.kind();
        // Retriable / "no work" -- the loop breaks without spinning.
        if kind == LegacyIoErrorKind::WouldBlock
            || kind == LegacyIoErrorKind::Interrupted
            || kind == LegacyIoErrorKind::ConnectionAborted
        {
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
        // SAFETY: the proxy owns the registered connection Arc.
                    crate::reactor::PollThread::add_proxy(
                &**pt,
                make_tcp_connection_pollable_proxy(conn.clone()),
            );
    }

    out.connection = Some(conn.clone());
    out.proxy = Some(make_tcp_connection_channel_proxy(conn));
    1
}

fn tcplistener_handle_error(listener: &TcpListener) {
    if listener.closed_.load(Ordering::Acquire) {
        return;
    }
    {
        let callback = {
            let guard = listener.on_error_.lock().unwrap();
            (*guard).clone()
        };
        if callback.has_value() {
            callback.callable()(ChannelError::Internal, "epoll/poll signaled error");
        }
    }
    listener.close();
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
    let pt: &Arc<PollThread> = &fac.poll_thread_;
    // SAFETY: the proxy owns the registered connection Arc.
            crate::reactor::PollThread::add_proxy(&**pt, make_tcp_connection_pollable_proxy(conn.clone()));

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
        let listener = NativeListener::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
        let (address, port) = native_address(&listener);
        drop(listener);
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
