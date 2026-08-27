//! TCP channel backend with all cross-thread connection state serialized by
//! the existing outbound mutex.  The poll worker remains the sole owner of
//! inbound decoder state; user-thread send/close operations never access it.

#![allow(
    non_camel_case_types,
    non_snake_case,
    unsafe_code,
    clippy::explicit_auto_deref
)]

use rusty::cpp_inherit;
use rusty::StdArcGetMutExt as _;
use std::cell::{RefCell, UnsafeCell};
use std::os::fd::IntoRawFd;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Weak as ArcWeak};

use crate::channel::{
    ChannelConnectionBase, ChannelConnectionProxy, ChannelError, ChannelFactoryBase,
    ChannelFactoryProxy, ChannelFrame, ChannelListenerBase, ChannelListenerProxy, ConnectResult,
    OnAcceptCallback, OnClosedCallback, OnErrorCallback, OnFrameCallback,
};
use crate::frame_codec::{FrameDecodeStatus, FrameHeader, FrameStreamReader, FrameView};
use crate::pollable_proxy::{PollableBase, PollableProxy};

use cpp::srpc::reactor as cpp_reactor;
use rusty as cpp;

type LegacyStdString = String;
type TcpOutBuf = rusty::StdVector<u8>;
type LegacyOwnedFd = cpp::RustcOwnedFd;
type LegacyTcpListener = cpp::RustcTcpListener;
type LegacySocketAddrV4 = cpp::RustcSocketAddrV4;
type LegacyIoErrorKind = cpp::RustcIoErrorKind;
type PollThread = cpp::ReactorPollThread;

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
    fn srpc_tcp_connect_socket(
        addr_be: u32,
        port_be: u16,
        timeout_ms: i32,
        out_errno: *mut i32,
    ) -> i32;
    fn srpc_tcp_recv_scratch() -> *mut u8;
    fn srpc_tcp_recv_bytes(fd: i32, data: *mut u8, size: usize) -> i64;
    fn srpc_tcp_send_bytes(fd: i32, data: *const u8, size: usize) -> i64;
    fn srpc_tcp_shutdown(fd: i32) -> i32;
    fn srpc_tcp_set_nonblocking(fd: i32) -> i32;
    fn srpc_tcp_last_errno() -> i32;
    fn srpc_tcp_current_thread_id() -> u32;
}

pub struct TcpConnection {
    // ABI note: these are deliberately the historical storage types.  The
    // already-present `outbound_` mutex is also the lifetime gate for `fd_`,
    // while `on_frame_` gates every access to `inbound_`.  Adding standalone
    // mutex wrappers around either field grew the public C++ object by 72
    // bytes and shifted every following field.
    fd_: UnsafeCell<LegacyOwnedFd>,
    peer_address_: LegacyStdString,
    outbound_high_water_: usize,
    outbound_: rusty::Mutex<TcpOutBuf>,
    inbound_: RefCell<FrameStreamReader>,
    closed_: AtomicBool,
    on_closed_fired_: AtomicBool,
    pending_write_update_: AtomicBool,
    poll_thread_: Option<Arc<PollThread>>,
    on_frame_: rusty::Mutex<OnFrameCallback>,
    on_closed_: rusty::Mutex<OnClosedCallback>,
    on_error_: rusty::Mutex<OnErrorCallback>,
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
    pub unsafe fn new(fd: i32, peer_address: LegacyStdString) -> TcpConnection {
        TcpConnection {
            // SAFETY: callers transfer a freshly connected descriptor.
            fd_: UnsafeCell::new(unsafe { LegacyOwnedFd::from_raw_fd(fd) }),
            peer_address_: peer_address,
            outbound_high_water_: kTcpConnectionOutboundHighWaterDefault,
            outbound_: rusty::Mutex::<TcpOutBuf>::new(Default::default()),
            inbound_: RefCell::new(FrameStreamReader::new()),
            closed_: AtomicBool::new(false),
            on_closed_fired_: AtomicBool::new(false),
            pending_write_update_: AtomicBool::new(false),
            poll_thread_: None,
            on_frame_: rusty::Mutex::<OnFrameCallback>::new(Default::default()),
            on_closed_: rusty::Mutex::<OnClosedCallback>::new(Default::default()),
            on_error_: rusty::Mutex::<OnErrorCallback>::new(Default::default()),
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

    pub fn peer_address(&self) -> LegacyStdString {
        self.peer_address_.clone()
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
        unsafe { (&*self.fd_.get()).as_raw_fd() }
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

#[cpp_inherit]
impl ChannelConnectionBase for TcpChannelShim {
    unsafe fn send_frame(&mut self, frame: &ChannelFrame) -> ChannelError {
        unsafe { self.conn_.send_frame(frame) }
    }
    fn flush(&mut self) {
        self.conn_.flush()
    }
    fn close(&mut self) {
        self.conn_.close()
    }
    fn is_closed(&self) -> bool {
        self.conn_.is_closed()
    }
    fn peer_address(&self) -> LegacyStdString {
        self.conn_.peer_address()
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
}

#[cpp_inherit]
impl PollableBase for TcpPollableShim {
    fn fd(&self) -> i32 {
        self.conn_.fd()
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

fn make_tcp_connection_pollable_proxy(conn: Arc<TcpConnection>) -> PollableProxy {
    Box::new(TcpPollableShim { conn_: conn })
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
    // These retain their historical RefCell representation and exact ABI.
    // `on_accept_` doubles as the lifecycle gate for both cells.  Callback
    // invocation always occurs after the gate has been released.
    listener_: RefCell<LegacyTcpListener>,
    bound_address_: RefCell<LegacyStdString>,
    closed_: AtomicBool,
    listened_: AtomicBool,
    // Reuses the historical padding bytes between the one-byte latches and
    // `poll_thread_`.  This is the owner latch for the whole accept driver,
    // not just its callback window: at most one `handle_read` may accept or
    // invoke `on_accept_` at a time.  `close` waits for that owner unless it
    // is called reentrantly by the owner itself.
    accept_callback_thread_: AtomicU32,
    poll_thread_: Option<Arc<PollThread>>,
    // Retained for exact historical layout/API compatibility. Registration
    // no longer depends on upgrading this weak pointer: the channel shim owns
    // the listener Arc and registers it immediately after a successful bind.
    self_weak_: Option<ArcWeak<TcpListener>>,
    on_accept_: rusty::Mutex<OnAcceptCallback>,
    on_error_: rusty::Mutex<OnErrorCallback>,
}

// SAFETY: `listener_` and `bound_address_` are only accessed while holding
// `on_accept_`; latches are atomic; callbacks have their own mutexes; and the
// Arc/Weak fields are initialized through `&mut self` before publication.
unsafe impl Send for TcpListener {}
unsafe impl Sync for TcpListener {}

impl TcpListener {
    pub fn new() -> TcpListener {
        TcpListener {
            listener_: RefCell::<LegacyTcpListener>::new(Default::default()),
            bound_address_: RefCell::<LegacyStdString>::new(Default::default()),
            closed_: AtomicBool::new(false),
            listened_: AtomicBool::new(false),
            accept_callback_thread_: AtomicU32::new(0),
            poll_thread_: None,
            self_weak_: None,
            on_accept_: rusty::Mutex::<OnAcceptCallback>::new(Default::default()),
            on_error_: rusty::Mutex::<OnErrorCallback>::new(Default::default()),
        }
    }

    // Bind path: rusty::net::TcpListener::bind + socket_addr_v4_from_str
    // + set_nonblocking — pure flow control over Results, all field
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
        let parse_result = cpp::rusty::net::socket_addr_v4_from_str(addr);
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
        let bound = match LegacyTcpListener::bind(parsed) {
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
        let address_string: LegacyStdString;
        match local_result {
            Ok(value) => {
                address_string = cpp::rusty::net::socket_addr_v4_to_string(value);
            }
            Err(_) => {
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
        *listener_guard = bound;
        ChannelError::None
    }

    // Sets the closed latch and replaces the owned listener with a
    // default one — the old value drops here, RAII-closing the fd.
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
            let _closed = core::mem::take(&mut *g);
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

    pub fn local_address(&self) -> LegacyStdString {
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
        (*g).as_owned_fd().as_raw_fd()
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

#[cpp_inherit]
#[allow(unsafe_code)]
unsafe impl ChannelListenerBase for TcpListenerChannelShim {
    fn listen(&mut self, a: &str) -> ChannelError {
        let result = self.listener_.listen(a);
        if result == ChannelError::None {
            if let Some(pt) = self.listener_.poll_thread_.as_ref() {
                // SAFETY: the proxy owns an Arc to this successfully bound
                // listener and is moved into the poll command queue.
                unsafe {
                    cpp_reactor::PollThread::add_proxy(
                        &**pt,
                        make_tcp_listener_pollable_proxy(self.listener_.clone()),
                    );
                }
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
    fn local_address(&self) -> LegacyStdString {
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
}

#[cpp_inherit]
impl PollableBase for TcpListenerPollableShim {
    fn fd(&self) -> i32 {
        self.listener_.fd()
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

fn make_tcp_listener_pollable_proxy(listener: Arc<TcpListener>) -> PollableProxy {
    Box::new(TcpListenerPollableShim {
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

    pub fn backend_name(&self) -> LegacyStdString {
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

#[cpp_inherit]
impl ChannelFactoryBase for TcpFactoryShim {
    fn connect(&mut self, addr: &str) -> ConnectResult {
        self.factory_.connect(addr)
    }
    fn make_listener(&mut self) -> Option<ChannelListenerProxy> {
        self.factory_.make_listener()
    }
    fn backend_name(&self) -> LegacyStdString {
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

    if let Some(pt) = conn.poll_thread_.as_ref() {
        // SAFETY: the predicate only reads reactor-owned TLS for this thread.
        if unsafe { cpp_reactor::pollworker_is_on_poll_thread() } {
            conn.pending_write_update_.store(true, Ordering::Release);
            return ChannelError::None;
        }
        // SAFETY: this connection's descriptor is registered with `pt`.
        unsafe {
            cpp_reactor::PollThread::update_mode(&**pt, conn.fd(), TCP_POLL_READ | TCP_POLL_WRITE);
        }
    } else {
        conn.pending_write_update_.store(true, Ordering::Release);
    }
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

fn tcpconn_close(conn: &TcpConnection) {
    if conn.closed_.swap(true, Ordering::AcqRel) {
        return;
    }
    {
        let _fd_gate = conn.outbound_.lock().unwrap();
        // SAFETY: `outbound_` serializes every descriptor access/mutation.
        let fd = unsafe { &mut *conn.fd_.get() };
        if fd.is_valid() {
            // SAFETY: the mutex keeps the descriptor live for the syscall.
            unsafe {
                let _ = srpc_tcp_shutdown(fd.as_raw_fd());
            }
            let _closed = core::mem::take(fd);
        }
    }
    tcpconn_deliver_on_closed_locked(conn, ChannelError::None);
}

fn tcpconn_reset_fd(conn: &TcpConnection) {
    let _fd_gate = conn.outbound_.lock().unwrap();
    // SAFETY: `outbound_` serializes every descriptor access and mutation.
    let fd = unsafe { &mut *conn.fd_.get() };
    let _closed = core::mem::take(fd);
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
    let fd = unsafe { &*conn.fd_.get() };
    if !fd.is_valid() {
        return TcpIoResult { count: 0, error: 0 };
    }
    // SAFETY: `s` is the current thread's full RecvScratch allocation and
    // the descriptor remains owned while the mutex guard is held.
    let count =
        unsafe { srpc_tcp_recv_bytes(fd.as_raw_fd(), (*s).arr.as_mut_ptr(), kRecvScratchBytes) };
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
    let fd = unsafe { &*conn.fd_.get() };
    if !fd.is_valid() {
        return TcpIoResult { count: 0, error: 0 };
    }
    let remaining = buf.len() - offset;
    let count = unsafe { srpc_tcp_send_bytes(fd.as_raw_fd(), buf.as_ptr().add(offset), remaining) };
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
    let rc = unsafe { srpc_tcp_set_nonblocking(fd) };
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
                callback.callable()(accepted);
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
    (*g).is_bound()
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
    let accept_result = listener_guard.accept();
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

    let peer_addr_str = cpp::rusty::net::socket_addr_v4_to_string(peer_addr);

    // Hand the accepted fd to TcpConnection.
    let conn_fd = stream.into_owned_fd().into_raw_fd();
    // SAFETY: accept transferred the freshly created descriptor into this
    // connection; no other owner remains after into_raw_fd above.
    let mut conn = Arc::new(unsafe { TcpConnection::new(conn_fd, peer_addr_str) });

    if let Some(pt) = lst.poll_thread_.as_ref() {
        // The Arc is still uniquely owned, so this is the safe minting
        // window for installing the worker before either proxy clones it.
        conn.get_mut().unwrap().set_poll_thread(pt.clone());
        // SAFETY: the proxy owns the registered connection Arc.
        unsafe {
            cpp_reactor::PollThread::add_proxy(
                &**pt,
                make_tcp_connection_pollable_proxy(conn.clone()),
            );
        }
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

fn tcp_factory_connect_socket(
    peer: LegacySocketAddrV4,
    connect_timeout_ms: i32,
    err_out: &mut ChannelError,
) -> i32 {
    let sa = cpp::rusty::net::sockaddr_in_from_socket_addr_v4(peer);
    let mut err_no: i32 = 0;
    // SAFETY: `err_no` is writable for the call and the address fields are
    // plain values copied into the C seam's local sockaddr.
    let fd = unsafe {
        srpc_tcp_connect_socket(
            sa.sin_addr.s_addr,
            sa.sin_port,
            connect_timeout_ms,
            &raw mut err_no,
        )
    };
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
    let parse_result = cpp::rusty::net::socket_addr_v4_from_str(addr);
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

    // SAFETY: srpc_tcp_connect_socket returned a fresh descriptor whose
    // ownership is transferred exactly once into TcpConnection.
    let mut conn = Arc::new(unsafe { TcpConnection::new(fd, addr.to_string()) });
    conn.get_mut()
        .unwrap()
        .set_poll_thread(fac.poll_thread_.clone());
    let pt: &Arc<PollThread> = &fac.poll_thread_;
    // SAFETY: the proxy owns the registered connection Arc.
    unsafe {
        cpp_reactor::PollThread::add_proxy(&**pt, make_tcp_connection_pollable_proxy(conn.clone()));
    }

    ConnectResult {
        connection: Some(make_tcp_connection_channel_proxy(conn)),
        error: ChannelError::None,
    }
}

pub fn tcp_factory_make_listener(self_: &TcpFactory) -> Option<ChannelListenerProxy> {
    let mut listener = Arc::new(TcpListener::new());
    listener
        .get_mut()
        .unwrap()
        .set_poll_thread(self_.poll_thread_.clone());
    Some(make_tcp_listener_channel_proxy(listener))
}
