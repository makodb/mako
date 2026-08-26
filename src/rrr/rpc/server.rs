//! rrr.server — RPC server (formerly server.hpp + server.cpp).
//!
//! Hosts service implementations and dispatches inbound RPC frames to
//! the right handler. Sits above the channel layer (`tcp_channel`,
//! `inmemory_channel`) which surfaces a transport-agnostic
//! `ChannelListenerProxy` / `ChannelConnectionProxy` to this module.
//! The legacy listener/socket-path was retired in 5g3 — this module
//! is now purely the request/reply orchestration layer.

#![allow(non_camel_case_types, non_snake_case)]
#![allow(unsafe_code)]
#![allow(clippy::arc_with_non_send_sync)]

use rusty::cpp_inherit;
use rusty::RustyBoxGet as _;
use rusty::RustyFunctionIsEmpty as _;
use rusty::RustyHandleIsValid as _;
use rusty::StdArcGetMutExt as _;
use std::cell::{Cell, RefCell};
use std::sync::atomic::{AtomicBool, AtomicI32, Ordering};
use std::sync::{Arc, Weak as ArcWeak};

// The registration tables are the runtime's own hash containers, not the
// standard-library ones: the retired carrier's C++ surface is
// `rusty::HashMap` / `rusty::HashSet` and the checked type map keeps that
// exact spelling.
use rusty::{HashMap, HashSet};

use crate::channel::{
    channel_error_to_string, ChannelConnectionBase, ChannelConnectionProxy, ChannelError,
    ChannelFactoryBase, ChannelFactoryProxy, ChannelFrame, ChannelListenerBase,
    ChannelListenerProxy, OnAcceptCallback, OnClosedCallback, OnErrorCallback, OnFrameCallback,
};
use crate::misc::OneTimeJob;
use crate::serializable::{BinaryReadArchive, BinaryWriteArchive, BufferSink, BufferSource};
use crate::tcp_channel::{make_tcp_factory_proxy, TcpFactory};

use cpp::rrr::basetypes as cpp_basetypes;
use cpp::rrr::debugging as cpp_debugging;
// This otherwise-unused source-owned import keeps the exact
// `rrr.internal_protocol` provider visible to generated C++; the heartbeat
// rpc-id constant below is read through the crate path.
#[allow(unused_imports)]
use cpp::rrr::internal_protocol as cpp_internal_protocol;
use cpp::rrr::logging as cpp_logging;
use cpp::rrr::reactor as cpp_reactor;
use cpp::rrr::serializable as cpp_serializable;
use rusty as cpp;

// The consumer profile maps this private carrier to `std::string`, retaining
// the established C++ surface instead of exposing rusty-cpp's distinct
// `rusty::String` owner.
type LegacyStdString = String;

// `PollThread` and `Fiber` live in `rrr.reactor`, the last carrier that is
// still an inline module. They are reached through the checked
// cpp-module-index facade rather than a `crate::` path.
type PollThread = cpp::ReactorPollThread;

// The checked type map restores this alias to the exact legacy C++ spelling
// `rrr::ChannelConnectionBase`, so a raw pointer through it stays the thin
// polymorphic pointer the retired carrier returned rather than a Rust fat
// trait-object pointer.
type LegacyChannelConnectionBase = dyn ChannelConnectionBase;

// The length-prefixed integer carriers are module-owned aliases for exactly
// the reason serializable.cpp documents: pulling them through
// `use crate::serializable::{..}` makes the emitter invent a nested
// `rrr::serializable` namespace, while the real provider exports them
// straight out of `rrr`.
type v32 = rusty::SerializableV32;
type v64 = rusty::SerializableV64;



// The three errno values this module reports. Spelled as their numeric
// values (not libc macros) so the generated module stays valid after the
// runtime headers include errno.h — the same seam tcp_channel.cpp uses.
const SERVER_ERR_INVALID_ARGUMENT: i32 = 22;
const SERVER_ERR_ALREADY_EXISTS: i32 = 17;
const SERVER_ERR_NO_ENTRY: i32 = 2;
const SERVER_ERR_TRY_AGAIN: i32 = 11;

#[allow(unsafe_code)]
mod server_ffi {
    unsafe extern "C" {
        /// Parse a decimal port out of `text`, mirroring `std::stoi`'s
        /// accept/reject language exactly. Returns 0 on success and stores
        /// the value through `out`; returns -1 when the input has no
        /// conversion or falls outside the int32 range.
        pub(super) fn srpc_parse_port(text: *const u8, len: usize, out: *mut i32) -> i32;

        /// Length of a NUL-terminated C string.
        pub(super) fn srpc_cstr_len(text: *const u8) -> usize;

        /// Draw 64 random bits for the server instance id.
        pub(super) fn srpc_random_u64() -> u64;
    }
}

// ===========================================================================
// Class declarations (from former server.hpp)
// ===========================================================================

// Type alias for Arc weak reference (must be before Service for __dispatch__).
pub type WeakServerConnection = ArcWeak<ServerConnection>;
// @safe - PendingRequestGuard / Request / Service / RpcServiceContext are
// `// @safe` shells; ServerConnection is `// @safe` with per-method
// `// @unsafe` overrides on the marshal/raw-pointer paths. The
// `shutdown_phase_to_string` free function is `// @safe`. The
// `make_service_proxy_from_box` / `make_service_proxy_from_typed_box`
// helpers are pure Box adapters.

/// Server shutdown phases for graceful shutdown support.
/// Progression: RUNNING -> STOP_ACCEPTING -> DRAINING -> CLOSING -> STOPPED
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum ShutdownPhase {
    RUNNING,
    STOP_ACCEPTING,
    DRAINING,
    CLOSING,
    STOPPED,
}

pub fn shutdown_phase_to_string(phase: ShutdownPhase) -> &'static str {
    #[allow(unreachable_patterns)]
    match phase {
        ShutdownPhase::RUNNING => "RUNNING",
        ShutdownPhase::STOP_ACCEPTING => "STOP_ACCEPTING",
        ShutdownPhase::DRAINING => "DRAINING",
        ShutdownPhase::CLOSING => "CLOSING",
        ShutdownPhase::STOPPED => "STOPPED",
        _ => "UNKNOWN",
    }
}

// Shutdown hook callback type. rusty::Function is move-only; the
// hooks are stored in `Vec<ShutdownHook>` (no clone()), pushed via
// move in `add_shutdown_hook`, and invoked by reference inside the
// graceful-shutdown loop.
pub type ShutdownHook = Box<dyn FnMut()>;

/// The raw packet sent from client will be like this:
/// `<size> <xid> <rpc_id> <arg1> <arg2> ... <argN>`
/// NOTE: size does not include the size itself (`<xid>..<argN>`).
///
/// For the request object, the marshal only contains `<arg1>..<argN>`,
/// other fields are already consumed.
///
/// @safe - RAII guard for one in-flight request: decrements the shared
/// pending-request counter on drop. The matching increment is done at the
/// guard's single construction site (`Request::attach_pending_guard`).
pub struct PendingRequestGuard {
    pending_counter: Arc<AtomicI32>,
}

impl Drop for PendingRequestGuard {
    fn drop(&mut self) {
        if self.pending_counter.is_valid() {
            self.pending_counter.fetch_sub(1i32, Ordering::Relaxed);
        }
    }
}

/// `Request` — simple in-flight RPC request container.
pub struct Request {
    pub body: Vec<u8>,
    pub src: BufferSource,
    pub xid: i64,
    pub pending_guard: Option<Box<PendingRequestGuard>>,
}

impl Request {
    pub fn attach_pending_guard(&mut self, counter: &Arc<AtomicI32>) {
        if self.pending_guard.is_none() && counter.is_valid() {
            counter.fetch_add(1i32, Ordering::Relaxed);
            self.pending_guard = Some(Box::new(PendingRequestGuard {
                pending_counter: counter.clone(),
            }));
        }
    }
}

/// The single `Request` construction site: the retired carrier default-built
/// the box, so this mirrors that value-initialization exactly.
fn make_empty_request_box() -> Box<Request> {
    Box::new(Request {
        body: Vec::<u8>::new(),
        src: BufferSource::new(core::ptr::null(), 0usize),
        xid: 0i64,
        pending_guard: None,
    })
}


// The channel layer's callback aliases require `Send + Sync` captures because
// a transport may deliver on a poll thread. The two types the server captures
// carry `Cell` / `RefCell` interior mutability, so the assertions below are
// what makes those captures well-formed.
//
// SAFETY: this states the module's long-standing single-dispatch-thread
// contract, unchanged from the retired C++ carrier. `RpcServiceContext` is
// immutable after `Server::start()` except for the `RefCell<ServiceProxy>`
// slots, and every `__dispatch__` runs on the connection's own poll thread;
// `ServerConnection`'s `Cell` state (`status_`, `channel_mode_`) is written
// only from that thread and read elsewhere as a monotone latch, exactly as
// the C++ carrier read it.
unsafe impl Send for RpcServiceContext {}
unsafe impl Sync for RpcServiceContext {}
unsafe impl Send for ServerConnection {}
unsafe impl Sync for ServerConnection {}

/// @interface
/// @safe - Pure virtual interface. All declarations carry per-method `// @safe`.
pub trait Service {
    fn __reg_to__(&mut self, server: &mut Server, svc_index: usize) -> i32;
    fn __dispatch__(&mut self, rpc_id: i32, req: Box<Request>, sconn: WeakServerConnection);
}

pub type ServiceProxy = Box<dyn Service>;

/// Pass-through factory for services that already inherit Service.
/// @safe - Box move.
pub fn make_service_proxy_from_box(svc: Box<dyn Service>) -> ServiceProxy {
    svc
}

/// `ServiceBoxShim<T>` — the generic Box-holding Service implementor
/// (generic #[cpp_inherit]; Box gives owning mutable access, so no
/// constness dance at all).
pub struct ServiceBoxShim<T> {
    svc_: Box<T>,
}

#[cpp_inherit]
impl<T: Service> Service for ServiceBoxShim<T> {
    fn __reg_to__(&mut self, server: &mut Server, svc_index: usize) -> i32 {
        self.svc_.__reg_to__(server, svc_index)
    }

    fn __dispatch__(&mut self, rpc_id: i32, req: Box<Request>, sconn: ArcWeak<ServerConnection>) {
        self.svc_.__dispatch__(rpc_id, req, sconn)
    }
}

/// @safe - wraps a typed Box<T> in the ServiceBoxShim above; Box move
/// only. Merged into this block so the factory sits beside the shim it
/// builds.
pub fn make_service_proxy_from_typed_box<T: Service + 'static>(svc: Box<T>) -> ServiceProxy {
    Box::new(ServiceBoxShim::<T> { svc_: svc })
}

// The atomic carriers named by RpcServiceContext and Server. Kept as named
// aliases so one spelling drives both the Rust and the C++ surface.
pub type ServerPendingRequestsAtomic = AtomicI32;
pub type ServerDropHeartbeatRepliesAtomic = AtomicBool;
pub type ServerAdmissionReadyAtomic = AtomicBool;

/// Shared context for RPC service dispatch.
///
/// This struct is shared between Server and ServerConnection via
/// `Arc<RpcServiceContext>` to avoid raw pointer dependencies.
///
/// SAFETY: The struct is constructed once in `Server::start()` and shared via
/// Arc. All fields are immutable after construction. Services use RefCell for
/// interior mutability, allowing non-const `__dispatch__` calls through const
/// Arc access.
///
/// NOTE: RefCell is single-threaded. All RPC dispatches must occur on the same
/// thread.
///
/// @safe - All fields are const after construction; the factory just moves
/// owned containers into place. No syscalls, no raw pointers.
pub struct RpcServiceContext {
    pub rpc_to_service: HashMap<i32, usize>,
    pub fast_rpc_ids: HashSet<i32>,
    pub services: Vec<RefCell<ServiceProxy>>,
    pub addr: LegacyStdString,
    pub pending_requests: Arc<ServerPendingRequestsAtomic>,
    pub drop_heartbeat_replies: Arc<ServerDropHeartbeatRepliesAtomic>,
    pub admission_ready: Arc<ServerAdmissionReadyAtomic>,
    pub server_instance_id: u64,
}

impl RpcServiceContext {
    pub fn new(
        rpc_map: HashMap<i32, usize>,
        fast_rpc_set: HashSet<i32>,
        svcs: Vec<RefCell<ServiceProxy>>,
        address: LegacyStdString,
        pending_counter: Arc<ServerPendingRequestsAtomic>,
        drop_heartbeats: Arc<ServerDropHeartbeatRepliesAtomic>,
        instance_id: u64,
    ) -> RpcServiceContext {
        RpcServiceContext::new_with_admission(
            rpc_map,
            fast_rpc_set,
            svcs,
            address,
            pending_counter,
            drop_heartbeats,
            Arc::new(ServerAdmissionReadyAtomic::new(true)),
            instance_id,
        )
    }

    #[allow(clippy::too_many_arguments)]
    pub fn new_with_admission(
        rpc_map: HashMap<i32, usize>,
        fast_rpc_set: HashSet<i32>,
        svcs: Vec<RefCell<ServiceProxy>>,
        address: LegacyStdString,
        pending_counter: Arc<ServerPendingRequestsAtomic>,
        drop_heartbeats: Arc<ServerDropHeartbeatRepliesAtomic>,
        admission_ready: Arc<ServerAdmissionReadyAtomic>,
        instance_id: u64,
    ) -> RpcServiceContext {
        RpcServiceContext {
            rpc_to_service: rpc_map,
            fast_rpc_ids: fast_rpc_set,
            services: svcs,
            addr: address,
            pending_requests: pending_counter,
            drop_heartbeat_replies: drop_heartbeats,
            admission_ready,
            server_instance_id: instance_id,
        }
    }
}

// 5g1: legacy `ServerListener` class deleted. The channel layer's
// `TcpListener` (registered via `ChannelFactoryProxy::make_listener()`)
// is the sole accept-loop implementation; `Server::start(addr)`
// auto-installs a default TCP factory (5f) when no explicit factory
// is bound.

// Alias for the reply callback type.
pub type ServerReplyFn = Box<dyn FnMut(&mut BinaryWriteArchive)>;

/// The empty-reply writer. The retired carrier passed a default-constructed
/// `rusty::Function` here and `sconn_reply` bypassed the call; a writer that
/// writes nothing produces the identical reply bytes, and the emptiness guard
/// in `sconn_reply` still protects the empty Functions that reach this module
/// from its C++ callers.
fn no_reply_writer() -> ServerReplyFn {
    Box::new(|_ar: &mut BinaryWriteArchive| {})
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum ServerConnStatus {
    CONNECTED,
    CLOSED,
}

/// ServerConnection — one client connection's server-side state. All fields
/// are already rusty (Arc / Mutex / Cell / Weak), so the struct is
/// borrow-checked. The reply/dispatch/decode bodies live in the `sconn_*`
/// free fns the methods delegate to.
///
/// @safe - the delegating methods forward to the `sconn_*` free fns, which
/// carry their own `// @unsafe`.
pub struct ServerConnection {
    pub ctx_: Arc<RpcServiceContext>,
    // Cell, matching how Server already holds shutdown_phase_field: an
    // Arc<ServerConnection> is shared, so state changes go through interior
    // mutability rather than callers const_cast-ing to get a &mut.
    pub status_: Cell<ServerConnStatus>,
    pub weak_self_: WeakServerConnection,
    pub channel_proxy_: rusty::Mutex<Option<ChannelConnectionProxy>>,
    pub channel_mode_: Cell<bool>,
    pub count: i32,
}

impl ServerConnection {
    pub fn new(ctx: Arc<RpcServiceContext>, _socket: i32) -> ServerConnection {
        ServerConnection {
            ctx_: ctx,
            status_: Cell::new(ServerConnStatus::CONNECTED),
            weak_self_: Default::default(),
            channel_proxy_: rusty::Mutex::<Option<ChannelConnectionProxy>>::new(None),
            channel_mode_: Cell::new(false),
            count: 0i32,
        }
    }

    pub fn install_self_weak_for_testing(&mut self, weak: WeakServerConnection) {
        self.weak_self_ = weak;
    }

    pub fn is_channel_mode(&self) -> bool {
        self.channel_mode_.get()
    }

    pub fn connected(&self) -> bool {
        self.status_.get() == ServerConnStatus::CONNECTED
    }

    pub fn is_closed(&self) -> bool {
        self.status_.get() == ServerConnStatus::CLOSED
    }

    pub fn reply(&self, req: &Request, error_code: i32, write_fn: ServerReplyFn) {
        sconn_reply(self, req, error_code, write_fn)
    }

    pub fn close(&self) {
        if self.status_.get() == ServerConnStatus::CONNECTED {
            self.status_.set(ServerConnStatus::CLOSED);
            let message: LegacyStdString =
                format!("server@{} close ServerConnection", self.ctx_.addr);
            // SAFETY: the file pointer is null, so the logger performs no path scan.
            unsafe { cpp_logging::log_line(4, 0, core::ptr::null(), &message) };
            // Tear down the channel proxy. Idempotent per channel-layer contract.
            let mut guard = self.channel_proxy_.lock().unwrap();
            if (*guard).is_some() {
                let proxy: &mut Box<dyn ChannelConnectionBase> = (*guard).as_mut().unwrap();
                proxy.close();
            }
        }
    }

    // Mirrors ClientConnection::bind_channel_direct (client.cpp): weak
    // clones — one per closure — so the callbacks never cycle through
    // `channel_proxy_`; callbacks installed BEFORE the proxy moves into
    // the slot so none of them runs under the mutex.
    pub fn bind_channel(&mut self, mut proxy: ChannelConnectionProxy) {
        if !proxy.is_valid() {
            return;
        }
        let weak_frame: WeakServerConnection = self.weak_self_.clone();
        let weak_closed: WeakServerConnection = self.weak_self_.clone();
        let weak_error: WeakServerConnection = self.weak_self_.clone();
        {
            let ch: &mut Box<dyn ChannelConnectionBase> = &mut proxy;
            ch.set_on_frame(OnFrameCallback::from_callable(Box::new(
                // SAFETY: the channel-layer contract pins the frame payload
                // for the duration of this callback.
                move |f: &ChannelFrame| unsafe { sconn_on_channel_frame(&weak_frame, f) },
            )
                as Box<dyn Fn(&ChannelFrame) + Send + Sync>));
            // on_closed runs the existing close path so the connection
            // transitions to CLOSED. The channel-layer contract guarantees
            // on_closed fires exactly once; close() is itself idempotent
            // (status_ == CLOSED short-circuits).
            ch.set_on_closed(OnClosedCallback::from_callable(Box::new(
                move |_reason: ChannelError| sconn_on_channel_closed(&weak_closed),
            ) as Box<dyn Fn(ChannelError) + Send + Sync>));
            // on_error logs and force-closes. Per the channel-layer
            // contract, fatal errors are followed by on_closed, so the
            // close() here is also defensive — close() is idempotent.
            ch.set_on_error(OnErrorCallback::from_callable(Box::new(
                move |err: ChannelError, msg: &str| {
                    sconn_on_channel_error(&weak_error, err, msg)
                },
            ) as Box<dyn Fn(ChannelError, &str) + Send + Sync>));
        }
        {
            let mut guard = self.channel_proxy_.lock().unwrap();
            *guard = Some(proxy);
        }
        self.channel_mode_.set(true);
    }

    pub fn run_async(&self, mut f: Box<dyn FnMut()>) -> i32 {
        if f.is_empty() {
            let message: LegacyStdString =
                "rrr::ServerConnection::run_async called with empty callback".to_string();
            // SAFETY: the file pointer is null.
            unsafe { cpp_logging::log_line(2, 0, core::ptr::null(), &message) };
            return SERVER_ERR_INVALID_ARGUMENT;
        }
        f();
        0i32
    }
}

// @safe - DeferredReply (RAII wrapper for deferred RPC replies) and
// Server (which owns the channel listener + accepted ServerConnection
// Arcs). Both carry their own descriptive `// @safe` blocks with
// per-method `// @unsafe` overrides.

/// `DeferredReply` — once-fire async-reply handle handed to user
/// service handlers. Holds the inbound request, a weak handle on
/// the server connection, the write-side archive callback the
/// rcc_rpc-generated dispatcher built around the typed reply
/// struct, and a cleanup callback for any heap state the wrapper
/// allocated for the request.
pub struct DeferredReply {
    req_field: Box<Request>,
    weak_sconn_field: WeakServerConnection,
    // `ServerReplyFn` is this exact type, declared above; naming it here
    // keeps the field off `clippy::type_complexity` without inventing a
    // second spelling for one C++ type.
    archive_reply_field: Option<ServerReplyFn>,
    cleanup_field: Option<Box<dyn FnMut()>>,
}

impl DeferredReply {
    pub fn new(
        req: Box<Request>,
        weak_sconn: WeakServerConnection,
        archive_reply: Box<dyn FnMut(&mut BinaryWriteArchive)>,
        cleanup: Box<dyn FnMut()>,
    ) -> DeferredReply {
        DeferredReply {
            req_field: req,
            weak_sconn_field: weak_sconn,
            archive_reply_field: Some(archive_reply),
            cleanup_field: Some(cleanup),
        }
    }

    pub fn run_async(&mut self, mut f: Box<dyn FnMut()>) -> i32 {
        f();
        0i32
    }


    pub fn reply(&mut self) {
        let cb_opt = self.archive_reply_field.take();
        if cb_opt.is_none() {
            let message: LegacyStdString =
                "DeferredReply::reply() called multiple times, ignoring".to_string();
            // SAFETY: the file pointer is null.
            unsafe { cpp_logging::log_line(2, 0, core::ptr::null(), &message) };
            return;
        }
        let cb = cb_opt.unwrap();
        let sconn_opt = self.weak_sconn_field.upgrade();
        if let Some(sconn) = sconn_opt {
            (*sconn).reply(&self.req_field, 0i32, cb);
        } else {
            let message: LegacyStdString =
                "Connection closed before reply sent, dropping reply".to_string();
            // SAFETY: the file pointer is null.
            unsafe { cpp_logging::log_line(4, 0, core::ptr::null(), &message) };
        }
    }

    pub fn reply_error(&mut self, error_code: i32) {
        if self.archive_reply_field.take().is_none() {
            let message: LegacyStdString =
                "DeferredReply::reply_error() called multiple times, ignoring".to_string();
            // SAFETY: the file pointer is null.
            unsafe { cpp_logging::log_line(2, 0, core::ptr::null(), &message) };
            return;
        }
        let sconn_opt = self.weak_sconn_field.upgrade();
        if let Some(sconn) = sconn_opt {
            let no_writer: ServerReplyFn = no_reply_writer();
            (*sconn).reply(&self.req_field, error_code, no_writer);
        } else {
            let message: LegacyStdString =
                "Connection closed before error reply sent, dropping reply".to_string();
            // SAFETY: the file pointer is null.
            unsafe { cpp_logging::log_line(4, 0, core::ptr::null(), &message) };
        }
    }
}

impl Drop for DeferredReply {
    fn drop(&mut self) {
        if let Some(mut cleanup) = self.cleanup_field.take() {
            cleanup();
        }
    }
}

/// Default drain / graceful-shutdown timeout. Was the inline `= 30000`
/// default arg on `Server::drain()` and `Server::graceful_shutdown()`.
pub const kDefaultDrainTimeoutMs: u64 = 30000u64;

/// Shutdown coordination state — guarded by `Server::shutdown_state_field`.
pub struct ShutdownState {
    pub shutdown: bool,
}

/// Pick the PollThread to use (auto-create one if the caller did not supply
/// one). Used by the ctor.
///
/// # Safety
///
/// `unsafe` records the foreign named-module boundary: `rrr.reactor` is the
/// last inline carrier, so `PollThread::create` is reached through the
/// checked cpp-module-index facade.
pub unsafe fn server_resolve_poll_thread(
    poll_thread_worker: Option<Arc<PollThread>>,
) -> Option<Arc<PollThread>> {
    if poll_thread_worker.is_none() {
        return Some(unsafe { cpp_reactor::PollThread::create() });
    }
    poll_thread_worker
}

/// Copy a NUL-terminated C string into an owned `std::string`.
///
/// # Safety
///
/// `addr` must be a NUL-terminated readable C string for this call.
pub unsafe fn server_dsl_addr_to_string(addr: *const i8) -> LegacyStdString {
    let bytes: *const u8 = addr as *const u8;
    // SAFETY: the caller pins a NUL-terminated readable string, so the C
    // kernel's scan stays inside it and the slice below covers exactly the
    // characters before the terminator.
    let len: usize = unsafe { server_ffi::srpc_cstr_len(bytes) };
    // SAFETY: the scan above proved `len` bytes readable, and an address
    // string is ASCII by construction.
    let text: &str = unsafe { core::str::from_utf8_unchecked(core::slice::from_raw_parts(bytes, len)) };
    let out: LegacyStdString = text.to_string();
    out
}

/// Draw 64 random bits for the instance-id mix. The retired carrier used
/// `std::random_device`; the C kernel reads the same kernel entropy pool.
pub fn server_random_u64() -> u64 {
    // SAFETY: the C kernel writes a fully initialized 64-bit value and takes
    // no pointers from this side.
    unsafe { server_ffi::srpc_random_u64() }
}

/// rrr's own clock (Time::now microseconds, scaled to the nano range the
/// id-mix historically used; entropy comes from the random_u64 mix, not
/// clock granularity).
pub fn server_now_nanos() -> u64 {
    // SAFETY: `unsafe` records the `rrr.basetypes` named-module boundary.
    (unsafe { cpp_basetypes::Time::now(true) }) * 1000u64
}

/// Block until `do_shutdown()` flips the flag.
//
// MEASURED ABI PIN, not style. `clippy::borrowed_box` wants `&rusty::Condvar`
// here. The incumbent carrier's generated C++ (rpc/server.cpp at c6c55ba,
// line 1173) exports exactly
//     void server_wait_for_shutdown_impl(const rusty::Mutex<ShutdownState>&,
//                                        const rusty::Box<rusty::Condvar>&)
// and taking the lint regenerates that declaration as `const rusty::Condvar&`
// -- a different mangled symbol, i.e. one incumbent symbol removed and one
// added. The `&Box<..>` spelling is the ABI.
#[allow(clippy::borrowed_box)]
pub fn server_wait_for_shutdown_impl(
    state: &rusty::Mutex<ShutdownState>,
    cond: &Box<rusty::Condvar>,
) {
    let entering: LegacyStdString = "Server::wait_for_shutdown".to_string();
    // SAFETY: the file pointer is null.
    unsafe { cpp_logging::log_line(4, 0, core::ptr::null(), &entering) };
    let mut guard = state.lock().unwrap();
    guard = cond
        .wait_while(guard, |s: &mut ShutdownState| !s.shutdown)
        .unwrap();
    drop(guard);
    let leaving: LegacyStdString = "Server::wait_for_shutdown - done".to_string();
    // SAFETY: the file pointer is null.
    unsafe { cpp_logging::log_line(4, 0, core::ptr::null(), &leaving) };
}

pub fn server_generate_instance_id() -> u64 {
    let time_component: u64 = server_now_nanos();
    let random_component: u64 = server_random_u64();
    let pid_component: u64 = (rusty::sys::process::getpid() as u64) << 48;
    // 0x7fff_ffff_ffff_ffff is std::numeric_limits<int64_t>::max(): the id
    // is kept non-negative because it crosses the wire as a signed i64.
    let mut id: u64 =
        (time_component ^ random_component ^ pid_component) & 0x7fffffffffffffffu64;
    if id == 0u64 {
        id = 1u64;
    }
    let message: LegacyStdString = format!("Server: generated instance_id={}", id);
    // SAFETY: the file pointer is null.
    unsafe { cpp_logging::log_line(4, 0, core::ptr::null(), &message) };
    id
}

/// Drain phase-FSM + timed busy-wait.
pub fn server_drain_impl(
    phase: &Cell<ShutdownPhase>,
    pending: &Arc<ServerPendingRequestsAtomic>,
    timeout_ms: u64,
) -> bool {
    let current_phase = phase.get();
    if current_phase != ShutdownPhase::RUNNING && current_phase != ShutdownPhase::STOP_ACCEPTING {
        let message: LegacyStdString = format!(
            "Server::drain: already past the draining phases ({})",
            shutdown_phase_to_string(current_phase)
        );
        // SAFETY: the file pointer is null.
        unsafe { cpp_logging::log_line(4, 0, core::ptr::null(), &message) };
        return pending.load(Ordering::Relaxed) == 0i32;
    }
    let entering: LegacyStdString = format!(
        "Server::drain: transitioning to DRAINING, pending={}",
        pending.load(Ordering::Relaxed)
    );
    // SAFETY: the file pointer is null.
    unsafe { cpp_logging::log_line(3, 0, core::ptr::null(), &entering) };
    phase.set(ShutdownPhase::DRAINING);
    let start_us = rusty::sys::time::clock_monotonic_us();
    let timeout_us = timeout_ms * 1000u64;
    while pending.load(Ordering::Relaxed) > 0i32 {
        let elapsed_us = rusty::sys::time::clock_monotonic_us() - start_us;
        if elapsed_us >= timeout_us {
            let expired: LegacyStdString = format!(
                "Server::drain: timeout after {} ms, pending={}",
                timeout_ms,
                pending.load(Ordering::Relaxed)
            );
            // SAFETY: the file pointer is null.
            unsafe { cpp_logging::log_line(2, 0, core::ptr::null(), &expired) };
            return false;
        }
        rusty::sys::time::sleep_us(1000u64);
    }
    let done: LegacyStdString = "Server::drain: completed, all requests drained".to_string();
    // SAFETY: the file pointer is null.
    unsafe { cpp_logging::log_line(3, 0, core::ptr::null(), &done) };
    true
}

/// NOTE: hooks run WHILE the mutex is held. That is the pre-existing
/// behaviour and is preserved deliberately.
pub fn server_run_shutdown_hooks(hooks: &rusty::Mutex<Vec<ShutdownHook>>) {
    let message: LegacyStdString =
        "Server::graceful_shutdown: transitioning to CLOSING, executing hooks".to_string();
    // SAFETY: the file pointer is null.
    unsafe { cpp_logging::log_line(3, 0, core::ptr::null(), &message) };
    let mut guard = hooks.lock().unwrap();
    for hook in (*guard).iter_mut() {
        server_invoke_shutdown_hook_safely(hook);
    }
}

/// @unsafe - strtoll-equivalent parse behind a C shim.
///
/// `srpc_parse_port` reports "no conversion" and out-of-range saturation the
/// same way `std::stoi` does (invalid_argument -> None, out_of_range -> None)
/// with the throw removed rather than caught. Returning Option keeps the
/// failure signal distinct from a legitimately parsed value.
pub fn server_parse_port(text: &LegacyStdString) -> Option<i32> {
    let mut value: i32 = 0i32;
    // SAFETY: `text` is a NUL-terminated owner for the duration of the call
    // and `value` is a live, exclusively borrowed i32.
    let ok = unsafe { server_ffi::srpc_parse_port(text.as_ptr(), text.len(), &raw mut value) };
    if ok != 0i32 {
        return None;
    }
    Some(value)
}

/// The invoker catches a panicking hook and logs it, exactly as the old
/// two-arm try/catch did.
pub fn server_invoke_shutdown_hook_safely(hook: &mut ShutdownHook) {
    // MEASURED compile pin, not style. `clippy::redundant_closure` wants
    // `catch_unwind(hook)`. That regenerates as
    //     rusty::panic::catch_unwind(hook)   // hook: rusty::Function<void()>&
    // and rusty/panic.hpp's `AssertUnwindSafe(F f)` takes its callable BY
    // VALUE, while rusty::Function's copy constructor is `= delete`
    // (rusty/function.hpp:281) -- "call to deleted constructor of
    // rusty::Function<void ()>". The closure is what keeps it a by-reference
    // capture.
    #[allow(clippy::redundant_closure)]
    let r = rusty::panic::catch_unwind(|| hook());
    if r.is_ok() {
        return;
    }
    let msg = rusty::panic::payload_message(r.unwrap_err());
    if let Some(text) = msg {
        let message: LegacyStdString = format!(
            "Server::graceful_shutdown: hook threw exception: {}",
            text
        );
        // SAFETY: the file pointer is null.
        unsafe { cpp_logging::log_line(1, 0, core::ptr::null(), &message) };
    } else {
        let message: LegacyStdString =
            "Server::graceful_shutdown: hook threw unknown exception".to_string();
        // SAFETY: the file pointer is null.
        unsafe { cpp_logging::log_line(1, 0, core::ptr::null(), &message) };
    }
}

/// Accepted-connection registry shared between the Server and its
/// listener's on_accept closure. `closed` flips in ~Server; a late
/// accept observing it closes the connection instead of pushing.
pub struct ChannelSconns {
    pub closed: bool,
    pub conns: Vec<Arc<ServerConnection>>,
}

/// `Server` — RPC server facade.
pub struct Server {
    pending_services_field: Vec<ServiceProxy>,
    pending_rpc_to_service_field: HashMap<i32, usize>,
    pending_fast_rpc_ids_field: HashSet<i32>,
    ctx_field: Option<Arc<RpcServiceContext>>,
    poll_thread_field: Option<Arc<PollThread>>,
    shutdown_state_field: rusty::Mutex<ShutdownState>,
    shutdown_cond_field: Box<rusty::Condvar>,
    shutdown_phase_field: Cell<ShutdownPhase>,
    shutdown_hooks_field: rusty::Mutex<Vec<ShutdownHook>>,
    pending_requests_field: Arc<ServerPendingRequestsAtomic>,
    drop_heartbeat_replies_field: Arc<ServerDropHeartbeatRepliesAtomic>,
    admission_ready_field: Arc<ServerAdmissionReadyAtomic>,
    instance_id_field: u64,
    channel_factory_field: Option<ChannelFactoryProxy>,
    channel_listener_field: Option<ChannelListenerProxy>,
    // Arc-shared: the listener's on_accept closure runs on the poll
    // thread and can fire after ~Server on another thread (listener
    // close is an in-order poll-thread job, not a fence). Sharing the
    // state keeps a late accept writing into live memory, and the
    // `closed` marker makes it CORRECT too: an accept that loses the
    // race against ~Server must close the connection instead of parking
    // it in an orphaned vector nobody will ever close.
    channel_sconns_field: Arc<rusty::Mutex<ChannelSconns>>,
}

impl Drop for Server {
    // 5e/5f teardown. The channel-mode listener close is scheduled on
    // the poll thread via a OneTimeJob so commands are processed in
    // order; dropping the Box inside the job then releases the backend
    // listener, closing its fd. Accepted channel connections are closed
    // eagerly so peers see EOF immediately; close() is idempotent
    // everywhere here.
    fn drop(&mut self) {
        if self.channel_listener_field.is_some() {
            let listener_opt: Option<ChannelListenerProxy> =
                core::mem::take(&mut self.channel_listener_field);
            let mut listener_box: Box<dyn ChannelListenerBase> = listener_opt.unwrap();
            let close_job: Arc<OneTimeJob> = Arc::new(OneTimeJob::new(Box::new(move || {
                listener_box.close();
            })));
            let pt: &Arc<PollThread> = self.poll_thread_field.as_ref().unwrap();
            // SAFETY: `rrr.reactor` is a foreign named module; the job is a
            // well-formed owning handle the worker command queue takes over.
            unsafe { pt.add(close_job) };
        }
        {
            let mut guard = self.channel_sconns_field.lock().unwrap();
            guard.closed = true;
            let mut i: usize = 0usize;
            while i < guard.conns.len() {
                // MEASURED compile pin, not style. `clippy::explicit_auto_deref`
                // wants the outer `*` dropped. `conns[i]` is an
                // `Arc<ServerConnection>`, and without the deref the emitter
                // writes `...conns[i].close()` -- "no member named 'close' in
                // 'rusty::Arc<rrr::ServerConnection>'". The `*` is what emits
                // the `deref_if_pointer_like` the call needs.
                #[allow(clippy::explicit_auto_deref)]
                (*guard.conns[i]).close();
                i += 1usize;
            }
            guard.conns.clear();
        }
        self.ctx_field = None;
    }
}

impl Server {
    /// # Safety
    ///
    /// `unsafe` records the `rrr.reactor` named-module boundary crossed when
    /// no poll thread is supplied and one must be created.
    pub unsafe fn new(poll_thread_worker: Option<Arc<PollThread>>) -> Server {
        Server {
            pending_services_field: Vec::<ServiceProxy>::new(),
            pending_rpc_to_service_field: HashMap::<i32, usize>::new(),
            pending_fast_rpc_ids_field: HashSet::<i32>::new(),
            ctx_field: None,
            poll_thread_field: unsafe { server_resolve_poll_thread(poll_thread_worker) },
            shutdown_state_field: rusty::Mutex::<ShutdownState>::new(ShutdownState {
                shutdown: false,
            }),
            shutdown_cond_field: Box::new(rusty::Condvar::new()),
            shutdown_phase_field: Cell::new(ShutdownPhase::RUNNING),
            shutdown_hooks_field: rusty::Mutex::<Vec<ShutdownHook>>::new(Vec::<ShutdownHook>::new()),
            pending_requests_field: Arc::new(ServerPendingRequestsAtomic::new(0i32)),
            drop_heartbeat_replies_field: Arc::new(ServerDropHeartbeatRepliesAtomic::new(false)),
            admission_ready_field: Arc::new(ServerAdmissionReadyAtomic::new(true)),
            instance_id_field: server_generate_instance_id(),
            channel_factory_field: None,
            channel_listener_field: None,
            channel_sconns_field: Arc::new(rusty::Mutex::<ChannelSconns>::new(ChannelSconns {
                closed: false,
                conns: Vec::<Arc<ServerConnection>>::new(),
            })),
        }
    }

    pub fn set_channel_factory(&mut self, factory: ChannelFactoryProxy) {
        if !factory.is_valid() {
            return;
        }
        self.channel_factory_field = Some(factory);
    }

    pub fn is_channel_factory_bound(&self) -> bool {
        self.channel_factory_field.is_some()
    }

    // The retired carrier pushed first and then called `__reg_to__` through
    // the parked element. Registering before the push is the same observable
    // sequence -- `svc_index` is identical and `__reg_to__` only ever touches
    // the rpc-id tables -- and it is expressible without aliasing the vector
    // and `self` at once.
    pub fn reg_service(&mut self, svc: Box<dyn Service>) {
        let svc_index: usize = self.pending_services_field.len();
        let mut proxy: ServiceProxy = make_service_proxy_from_box(svc);
        {
            let registered: &mut Box<dyn Service> = &mut proxy;
            registered.__reg_to__(self, svc_index);
        }
        self.pending_services_field.push(proxy);
    }

    pub fn reg_service_proxy(&mut self, mut proxy: ServiceProxy) {
        let svc_index: usize = self.pending_services_field.len();
        {
            let registered: &mut Box<dyn Service> = &mut proxy;
            registered.__reg_to__(self, svc_index);
        }
        self.pending_services_field.push(proxy);
    }

    pub fn reg_rpc(&mut self, rpc_id: i32, svc_index: usize) -> i32 {
        if self.pending_rpc_to_service_field.contains_key(&rpc_id) {
            return SERVER_ERR_ALREADY_EXISTS;
        }
        self.pending_rpc_to_service_field.insert(rpc_id, svc_index);
        0i32
    }

    pub fn reg_fast_rpc(&mut self, rpc_id: i32, svc_index: usize) -> i32 {
        let ret: i32 = self.reg_rpc(rpc_id, svc_index);
        if ret != 0i32 {
            return ret;
        }
        self.pending_fast_rpc_ids_field.insert(rpc_id);
        0i32
    }

    pub fn unreg(&mut self, rpc_id: i32) {
        self.pending_rpc_to_service_field.remove(&rpc_id);
        self.pending_fast_rpc_ids_field.remove(&rpc_id);
    }

    pub fn do_shutdown(&self) {
        let mut guard = self.shutdown_state_field.lock().unwrap();
        guard.shutdown = true;
        self.shutdown_cond_field.notify_all();
    }

    pub fn wait_for_shutdown(&self) {
        server_wait_for_shutdown_impl(&self.shutdown_state_field, &self.shutdown_cond_field);
    }

    pub fn add_shutdown_hook(&self, hook: ShutdownHook) {
        let mut guard = self.shutdown_hooks_field.lock().unwrap();
        (*guard).push(hook);
    }

    pub fn stop_accepting(&mut self) {
        if self.shutdown_phase_field.get() != ShutdownPhase::RUNNING {
            return;
        }
        self.shutdown_phase_field.set(ShutdownPhase::STOP_ACCEPTING);
        if let Some(listener) = self.channel_listener_field.as_mut() {
            listener.close();
        }
    }

    pub fn drain(&self, timeout_ms: u64) -> bool {
        server_drain_impl(
            &self.shutdown_phase_field,
            &self.pending_requests_field,
            timeout_ms,
        )
    }

    pub fn graceful_shutdown(&mut self, drain_timeout_ms: u64) {
        self.stop_accepting();
        self.drain(drain_timeout_ms);
        self.shutdown_phase_field.set(ShutdownPhase::CLOSING);
        server_run_shutdown_hooks(&self.shutdown_hooks_field);
        self.do_shutdown();
        self.shutdown_phase_field.set(ShutdownPhase::STOPPED);
    }

    pub fn phase(&self) -> ShutdownPhase {
        self.shutdown_phase_field.get()
    }

    pub fn pending_request_count(&self) -> i32 {
        self.pending_requests_field.load(Ordering::Relaxed)
    }

    pub fn increment_pending(&self) {
        self.pending_requests_field.fetch_add(1i32, Ordering::Relaxed);
    }

    pub fn decrement_pending(&self) {
        self.pending_requests_field.fetch_sub(1i32, Ordering::Relaxed);
    }

    pub fn set_drop_heartbeat_replies(&self, drop_replies: bool) {
        self.drop_heartbeat_replies_field
            .store(drop_replies, Ordering::Release);
    }

    pub fn drop_heartbeat_replies(&self) -> bool {
        self.drop_heartbeat_replies_field.load(Ordering::Acquire)
    }

    /// Close or open non-heartbeat RPC admission without disturbing the
    /// listener. Raft uses this while durable recovery and committed replay
    /// establish a safe service boundary.
    pub fn set_admission_ready(&self, ready: bool) {
        self.admission_ready_field.store(ready, Ordering::Release);
    }

    pub fn admission_ready(&self) -> bool {
        self.admission_ready_field.load(Ordering::Acquire)
    }

    pub fn instance_id(&self) -> u64 {
        self.instance_id_field
    }

    pub fn service_count(&self) -> usize {
        if let Some(ctx) = self.ctx_field.as_ref() {
            return ctx.services.len();
        }
        self.pending_services_field.len()
    }

    pub fn addr(&self) -> LegacyStdString {
        self.ctx_field.as_ref().unwrap().addr.clone()
    }

    /// Freeze the pending registrations into an immutable
    /// RpcServiceContext, auto-install a TcpFactory when none is bound,
    /// make + wire the channel listener, and bind.
    ///
    /// # Safety
    ///
    /// `bind_addr` must be null or a NUL-terminated C string that stays
    /// readable for the duration of the call.
    pub unsafe fn start(&mut self, bind_addr: *const i8) -> i32 {
        if bind_addr.is_null() {
            let message: LegacyStdString = "rrr::Server::start: bind_addr is NULL!".to_string();
            // SAFETY: the file pointer is null.
            unsafe { cpp_logging::log_line(1, 0, core::ptr::null(), &message) };
            return -1i32;
        }
        // SAFETY: the caller guarantees a NUL-terminated readable string that
        // stays alive for this call.
        let addr_str: LegacyStdString = unsafe { server_dsl_addr_to_string(bind_addr) };

        // Wrap each service in RefCell for interior mutability. The pending
        // Vec is taken whole and drained in order.
        let mut pending: Vec<ServiceProxy> = core::mem::take(&mut self.pending_services_field);
        let mut wrapped_services: Vec<RefCell<ServiceProxy>> =
            Vec::<RefCell<ServiceProxy>>::new();
        // `mut` is redundant in Rust (a non-mut binding can still be moved
        // out of) but load-bearing for the C++ lowering: without it the
        // element binds as `const auto&&` and `std::move` selects Box's
        // deleted copy constructor.
        #[allow(unused_mut)]
        for mut svc in pending.drain(..) {
            wrapped_services.push(RefCell::<ServiceProxy>::new(svc));
        }

        // Create the immutable RpcServiceContext from the pending
        // registration data.
        self.ctx_field = Some(Arc::new(RpcServiceContext::new_with_admission(
            core::mem::take(&mut self.pending_rpc_to_service_field),
            core::mem::take(&mut self.pending_fast_rpc_ids_field),
            wrapped_services,
            addr_str.clone(),
            self.pending_requests_field.clone(),
            self.drop_heartbeat_replies_field.clone(),
            self.admission_ready_field.clone(),
            self.instance_id_field,
        )));

        // Auto-install a default TcpFactory if the caller has not bound one.
        if !self.is_channel_factory_bound() {
            let tcp_factory: Arc<TcpFactory> = Arc::new(TcpFactory::new(
                self.poll_thread_field.as_ref().unwrap().clone(),
            ));
            self.set_channel_factory(make_tcp_factory_proxy(tcp_factory));
        }

        if self.is_channel_factory_bound() {
            let listener_opt = {
                let factory: &mut Box<dyn ChannelFactoryBase> =
                    self.channel_factory_field.as_mut().unwrap();
                factory.make_listener()
            };
            if listener_opt.is_none() {
                let message: LegacyStdString = format!(
                    "rrr::Server::start: factory->make_listener() returned a null proxy (factory backend={})",
                    "unknown"
                );
                // SAFETY: the file pointer is null.
                unsafe { cpp_logging::log_line(1, 0, core::ptr::null(), &message) };
                self.ctx_field = None;
                return -1i32;
            }
            let mut listener: ChannelListenerProxy = listener_opt.unwrap();

            let sconns_arc: Arc<rusty::Mutex<ChannelSconns>> = self.channel_sconns_field.clone();
            let ctx_arc: Arc<RpcServiceContext> = self.ctx_field.as_ref().unwrap().clone();

            {
                let ch: &mut Box<dyn ChannelListenerBase> = &mut listener;
                ch.set_on_accept(OnAcceptCallback::from_callable(Box::new(
                    move |conn_proxy: ChannelConnectionProxy| {
                        if !conn_proxy.is_valid() {
                            return;
                        }
                        let mut sconn: Arc<ServerConnection> =
                            Arc::new(ServerConnection::new(ctx_arc.clone(), -1i32));
                        // get_mut, not const_cast: the Arc was just made and is
                        // still uniquely owned — exactly when Arc::get_mut
                        // yields a &mut.
                        {
                            let weak = Arc::downgrade(&sconn);
                            let mut_sconn: &mut ServerConnection = sconn.get_mut().unwrap();
                            mut_sconn.install_self_weak_for_testing(weak);
                        }
                        {
                            let mut_sconn2: &mut ServerConnection = sconn.get_mut().unwrap();
                            mut_sconn2.bind_channel(conn_proxy);
                        }
                        {
                            let mut guard = sconns_arc.lock().unwrap();
                            if guard.closed {
                                // This accept lost the race against ~Server:
                                // close the connection now or nobody ever will.
                                (*sconn).close();
                                return;
                            }
                            guard.conns.push(sconn);
                        }
                    },
                ) as Box<dyn Fn(ChannelConnectionProxy) + Send + Sync>));
                ch.set_on_error(OnErrorCallback::from_callable(Box::new(
                    move |err: ChannelError, msg: &str| {
                        let reason: &str = channel_error_to_string(err);
                        let message: LegacyStdString =
                            format!("rrr::Server: channel listener error {}: {}", reason, msg);
                        // SAFETY: the file pointer is null.
                        unsafe { cpp_logging::log_line(2, 0, core::ptr::null(), &message) };
                    },
                ) as Box<dyn Fn(ChannelError, &str) + Send + Sync>));
            }

            let listen_err = {
                let ch2: &mut Box<dyn ChannelListenerBase> = &mut listener;
                ch2.listen(&addr_str)
            };
            if listen_err != ChannelError::None {
                let reason: &str = channel_error_to_string(listen_err);
                let message: LegacyStdString = format!(
                    "rrr::Server::start: channel listener failed to bind {}: {}",
                    addr_str, reason
                );
                // SAFETY: the file pointer is null.
                unsafe { cpp_logging::log_line(1, 0, core::ptr::null(), &message) };
                self.ctx_field = None;
                return -1i32;
            }

            self.channel_listener_field = Some(listener);
            return 0i32;
        }

        // SAFETY: `unsafe` records the `rrr.debugging` named-module boundary.
        unsafe { cpp_debugging::verify(false) };
        -1i32
    }

    // MEASURED emitter pin, not style. `clippy::borrowed_box` wants the
    // `&Box<..>` annotation below dropped. Regenerating without it emits
    //     auto& listener = this->channel_listener_field.as_ref().unwrap();
    //     const std::string local = listener.local_address();
    // i.e. the emitter loses the pointer-like classification and calls through
    // `.` instead of `->`. `rusty::Box` (third-party/rusty-cpp/include/rusty/
    // box.hpp) exposes `operator*`/`operator->` and forwards no members, so
    // that C++ does not compile. The annotation is what produces the
    // incumbent's `listener->local_address()`.
    #[allow(clippy::borrowed_box)]
    pub fn get_bound_port(&self) -> i32 {
        if self.channel_listener_field.is_none() {
            return -1i32;
        }
        let listener: &Box<dyn ChannelListenerBase> =
            self.channel_listener_field.as_ref().unwrap();
        let local: LegacyStdString = listener.local_address();
        let colon = local.rfind(':');
        if colon.is_none() {
            let message: LegacyStdString =
                format!("Server::get_bound_port: malformed local_address {}", local);
            // SAFETY: the file pointer is null.
            unsafe { cpp_logging::log_line(1, 0, core::ptr::null(), &message) };
            return -1i32;
        }
        let tail: LegacyStdString = local[colon.unwrap() + 1usize..].to_string();
        let parsed = server_parse_port(&tail);
        if parsed.is_none() {
            let message: LegacyStdString = format!(
                "Server::get_bound_port: failed to parse port from {}",
                local
            );
            // SAFETY: the file pointer is null.
            unsafe { cpp_logging::log_line(1, 0, core::ptr::null(), &message) };
            return -1i32;
        }
        parsed.unwrap()
    }

    pub fn reg_service_typed<T: Service + 'static>(&mut self, svc: Box<T>) {
        let svc_index: usize = self.pending_services_field.len();
        let mut proxy: ServiceProxy = make_service_proxy_from_typed_box::<T>(svc);
        {
            let registered: &mut Box<dyn Service> = &mut proxy;
            registered.__reg_to__(self, svc_index);
        }
        self.pending_services_field.push(proxy);
    }

    pub fn for_each_service<F: FnMut(&mut Box<dyn Service>)>(&self, mut callback: F) {
        let ctx = self.ctx_field.as_ref().unwrap();
        let n: usize = ctx.services.len();
        let mut i: usize = 0usize;
        while i < n {
            let mut guard = ctx.services[i].borrow_mut();
            // MEASURED emitter pin, not style. `clippy::explicit_auto_deref` wants
// `&mut guard`. Regenerating without the `*` emits
//     rusty::Box<Service>& svc = guard;
// binding the guard itself, not the boxed service, to a `rusty::Box<Service>&`.
// The `*` is what produces the incumbent's `rusty::Box<Service>& svc = *guard;`.
            #[allow(clippy::explicit_auto_deref)]
            let svc: &mut Box<dyn Service> = &mut *guard;
            callback(svc);
            i += 1usize;
        }
    }
}

// ===========================================================================
// Implementation namespace — free functions the methods above delegate to.
// ===========================================================================

/// Build the reply body (header + user payload) into a BufferSink and
/// dispatch through the bound channel proxy. (Was the templated `reply<F>`;
/// de-templated to a `ServerReplyFn` — Function SBO keeps the `[&]` reply
/// lambdas inline, no per-reply alloc.) An empty `write_fn` (the former
/// 2-arg empty-reply overload) writes just the header.
pub fn sconn_reply(
    sconn: &ServerConnection,
    req: &Request,
    error_code: i32,
    write_fn: ServerReplyFn,
) {
    let mut body_sink: BufferSink = BufferSink {
        bytes: Vec::<u8>::new(),
    };
    let mut ar_store = BinaryWriteArchive {
        // SAFETY: `body_sink` is a live, exclusively borrowed local that
        // outlives the archive built over it.
        sink_: unsafe { cpp_serializable::make_sink_proxy_buffer(&raw mut body_sink) },
    };
    let ar: &mut BinaryWriteArchive = &mut ar_store;
    crate::serializable::Serialize_::serialize(&v64::new(req.xid), ar);
    crate::serializable::Serialize_::serialize(&v32::new(error_code), ar);
    crate::serializable::Serialize_::serialize(
        &v64::new(sconn.ctx_.server_instance_id as i64),
        ar,
    );
    if !write_fn.is_empty() {
        let mut write = write_fn;
        write(ar);
    }
    drop(ar_store);
    // SAFETY: the sink's byte vector is live and the length matches.
    unsafe {
        sconn_dispatch_response_frame_via_channel(
            sconn,
            body_sink.bytes.as_ptr(),
            body_sink.bytes.len(),
        );
    }
}

/// The three channel callbacks the connection installs, as free functions so
/// each closure stays the single-call shape the emitter lowers into a
/// const-callable `rusty::Function`.
///
/// Dispatch only READS the connection (`status_` via Cell, `ctx_` through the
/// Arc), so it takes a const&.
///
/// # Safety
///
/// If `frame.size` is nonzero, `frame.payload` must address that many
/// readable bytes for the duration of the call.
pub unsafe fn sconn_on_channel_frame(weak: &ArcWeak<ServerConnection>, frame: &ChannelFrame) {
    let sconn_opt = weak.upgrade();
    if sconn_opt.is_none() {
        return;
    }
    let sconn = sconn_opt.unwrap();
    // SAFETY: the channel-layer contract pins the payload for this callback.
    unsafe { sconn_decode_request_and_dispatch(&sconn, frame.payload, frame.size) };
}

/// `on_closed` runs the existing close path so the connection transitions to
/// CLOSED. The channel-layer contract guarantees `on_closed` fires exactly
/// once; `close()` is itself idempotent.
pub fn sconn_on_channel_closed(weak: &ArcWeak<ServerConnection>) {
    let sconn_opt = weak.upgrade();
    if sconn_opt.is_none() {
        return;
    }
    let sconn = sconn_opt.unwrap();
    (*sconn).close();
}

/// `on_error` logs and force-closes. Per the channel-layer contract, fatal
/// errors are followed by `on_closed`, so the close here is also defensive.
pub fn sconn_on_channel_error(weak: &ArcWeak<ServerConnection>, err: ChannelError, msg: &str) {
    let sconn_opt = weak.upgrade();
    if sconn_opt.is_none() {
        return;
    }
    let sconn = sconn_opt.unwrap();
    let reason: &str = channel_error_to_string(err);
    let message: LegacyStdString =
        format!("rrr::ServerConnection: channel error {}: {}", reason, msg);
    // SAFETY: the file pointer is null.
    unsafe { cpp_logging::log_line(2, 0, core::ptr::null(), &message) };
    (*sconn).close();
}

/// Fill the request body from the wire bytes, then point the read cursor
/// at the filled buffer. Must be called at most once per Request, before
/// any read.
pub fn request_fill_body(req: &mut Request, bytes: &[u8]) {
    req.body.clear();
    req.body.extend_from_slice(bytes);
    req.src = BufferSource::new(req.body.as_ptr(), req.body.len());
}

/// The "no handler for rpc_id" warning-dedup set. Hoisted out of
/// ServerConnection (a DSL struct carries no static data member) and now
/// module-scope: it emits an `extern` declaration plus an `inline`
/// definition. Linkage widens from `static` (internal) to inline/module,
/// which is benign — this is the non-exported `namespace rrr` and
/// server.cpp is the module's only TU.
static g_rpc_id_missing: rusty::Mutex<HashSet<i32>> =
    rusty::Mutex::<HashSet<i32>>::new(HashSet::<i32>::new());

pub fn sconn_dispatch_in_fiber(
    ctx: Arc<RpcServiceContext>,
    svc_index: usize,
    rpc_id: i32,
    req: Box<Request>,
    weak_this: ArcWeak<ServerConnection>,
) {
    let mut guard = ctx.services[svc_index].borrow_mut();
    // Concrete `Box<..>`, not the ServiceProxy alias: through the alias the
    // pointer-like check fails and the call lowers to `.` instead of `->`.
    // MEASURED emitter pin, not style. `clippy::explicit_auto_deref` wants
// `&mut guard`. Regenerating without the `*` emits
//     rusty::Box<Service>& svc = guard;
// binding the guard itself, not the boxed service, to a `rusty::Box<Service>&`.
// The `*` is what produces the incumbent's `rusty::Box<Service>& svc = *guard;`.
    #[allow(clippy::explicit_auto_deref)]
    let svc: &mut Box<dyn Service> = &mut *guard;
    svc.__dispatch__(rpc_id, req, weak_this);
}

/// Decode one channel-mode request frame and dispatch. The body is
/// `[xid:v64][rpc_id:i32][user-args]` (the channel layer already stripped
/// the size prefix). Bytes are copied into the Request BEFORE any path that
/// may yield — the channel-layer contract makes them valid only for this
/// callback. The archive is a view over `req.src`; the cursor advance
/// persists into the handler's reads.
///
/// # Safety
///
/// If `size` is nonzero, `bytes` must address `size` readable, unaliased
/// bytes for the duration of the call.
pub unsafe fn sconn_decode_request_and_dispatch(
    sconn: &ServerConnection,
    bytes: *const u8,
    size: usize,
) {
    if sconn.status_.get() == ServerConnStatus::CLOSED {
        return;
    }
    let mut req_box: Box<Request> = make_empty_request_box();
    if size > 0usize {
        // SAFETY: the caller pins `size` readable bytes at `bytes`.
        request_fill_body(&mut req_box, unsafe {
            core::slice::from_raw_parts(bytes, size)
        });
    }

    // Malformed frame (not enough bytes for an xid): drop it — there is no
    // valid xid to reply against. v64 is 1-8 bytes; an empty body means
    // there is no xid at all.
    if req_box.src.remaining() == 0usize {
        let message: LegacyStdString =
            "rrr::ServerConnection: empty channel-mode request frame, dropping".to_string();
        // SAFETY: the file pointer is null.
        unsafe { cpp_logging::log_line(2, 0, core::ptr::null(), &message) };
        return;
    }
    let mut header_ar = BinaryReadArchive {
        // SAFETY: `req_box.src` is owned by the live boxed request.
        source_: unsafe { cpp_serializable::make_source_proxy_buffer(&raw mut req_box.src) },
    };
    let mut v_xid = v64::new(0i64);
    crate::serializable::Deserialize_::deserialize(&mut v_xid, &mut header_ar);
    req_box.xid = v_xid.get();
    let pending_counter = sconn.ctx_.pending_requests.clone();
    req_box.attach_pending_guard(&pending_counter);

    // sizeof(i32) spelled as its value: not enough bytes for rpc_id.
    if req_box.src.remaining() < 4usize {
        let empty_fn1: ServerReplyFn = no_reply_writer();
        sconn_reply(sconn, &req_box, SERVER_ERR_INVALID_ARGUMENT, empty_fn1);
        return;
    }

    let mut rpc_id: i32 = 0i32;
    crate::serializable::Deserialize_::deserialize(&mut rpc_id, &mut header_ar);
    if rpc_id == crate::internal_protocol::kInternalHeartbeatRpcId {
        let hb: &Arc<ServerDropHeartbeatRepliesAtomic> = &sconn.ctx_.drop_heartbeat_replies;
        if !hb.load(Ordering::Acquire) {
            let empty_fn2: ServerReplyFn = no_reply_writer();
            sconn_reply(sconn, &req_box, 0i32, empty_fn2);
        }
        return;
    }

    if !sconn.ctx_.admission_ready.load(Ordering::Acquire) {
        let empty_fn3: ServerReplyFn = no_reply_writer();
        sconn_reply(sconn, &req_box, SERVER_ERR_TRY_AGAIN, empty_fn3);
        return;
    }

    let svc_index_opt = sconn.ctx_.rpc_to_service.get(&rpc_id);
    if svc_index_opt.is_none() {
        let mut surpress_warning = false;
        {
            let mut guard = g_rpc_id_missing.lock().unwrap();
            if !(*guard).contains(&rpc_id) {
                (*guard).insert(rpc_id);
            } else {
                surpress_warning = true;
            }
        }
        if !surpress_warning {
            let message: LegacyStdString = format!(
                "rrr::ServerConnection: no handler for rpc_id = {} (channel-mode dispatch)",
                rpc_id
            );
            // SAFETY: the file pointer is null.
            unsafe { cpp_logging::log_line(2, 0, core::ptr::null(), &message) };
        }
        let empty_fn4: ServerReplyFn = no_reply_writer();
        sconn_reply(sconn, &req_box, SERVER_ERR_NO_ENTRY, empty_fn4);
        return;
    }

    let svc_index: usize = *svc_index_opt.unwrap();
    let weak_this: WeakServerConnection = sconn.weak_self_.clone();
    if sconn.ctx_.fast_rpc_ids.contains(&rpc_id) {
        // Fast inline dispatch — no fiber spawn.
        let mut guard = sconn.ctx_.services[svc_index].borrow_mut();
        // MEASURED emitter pin, not style. `clippy::explicit_auto_deref` wants
// `&mut guard`. Regenerating without the `*` emits
//     rusty::Box<Service>& svc = guard;
// binding the guard itself, not the boxed service, to a `rusty::Box<Service>&`.
// The `*` is what produces the incumbent's `rusty::Box<Service>& svc = *guard;`.
        #[allow(clippy::explicit_auto_deref)]
        let svc: &mut Box<dyn Service> = &mut *guard;
        svc.__dispatch__(rpc_id, req_box, weak_this);
    } else {
        // Slow path — spawn a fiber so the handler can yield (e.g. for
        // nested RPC calls). The ctx Arc clone keeps the services alive
        // even if the connection is closed mid-flight.
        let ctx2 = sconn.ctx_.clone();
        // `FnMut` (not `FnOnce`) is what `rusty::Function<void()>` models, so
        // the moved-in request and weak handle are parked in `Option` slots
        // the first call takes. The fiber runs the job exactly once.
        let mut parked_req: Option<Box<Request>> = Some(req_box);
        let mut parked_weak: Option<WeakServerConnection> = Some(weak_this);
        // Same redundant-`mut` reason as the service loop above: the C++
        // lowering must move the Function into `fiber_create_run_impl`.
        #[allow(unused_mut)]
        let mut job_fn: Box<dyn FnMut()> = Box::new(move || {
            let taken_req = parked_req.take();
            let taken_weak = parked_weak.take();
            if taken_req.is_none() {
                return;
            }
            sconn_dispatch_in_fiber(
                ctx2.clone(),
                svc_index,
                rpc_id,
                taken_req.unwrap(),
                taken_weak.unwrap(),
            );
        });
        // `create_run_impl` is the non-generic entry `Fiber::create_run<Func>`
        // itself calls; naming it directly keeps this an ordinary imported
        // method rather than an imported member template.
        //
        // SAFETY: `rrr.reactor` is a foreign named module; the closure is a
        // well-formed owning job the fiber runtime takes over.
        unsafe { cpp_reactor::fiber_create_run_impl(job_fn, core::ptr::null(), 0i64) };
    }
}

/// @unsafe - Box raw extraction for the dispatch body below — the pointer
/// must OUTLIVE the guard (send happens without the lock, per the
/// channel-layer contract that send_frame is internally thread-safe).
pub fn sconn_proxy_ptr(
    slot: &Option<ChannelConnectionProxy>,
) -> *mut LegacyChannelConnectionBase {
    slot.as_ref().unwrap().get()
}

/// Dispatch a reply-frame body through the bound proxy. Locks the mutex
/// briefly to extract the proxy pointer, then drops the guard so the actual
/// `send_frame` happens without holding the lock. Errors are observable via
/// the proxy's installed on_error/on_closed callbacks; the return value is
/// deliberately discarded — the RPC layer mirrors the legacy fd path's
/// behavior of not surfacing send-side errors from `reply()`.
///
/// # Safety
///
/// If `size` is nonzero, `bytes` must address `size` readable, unaliased
/// bytes for the duration of the call.
pub unsafe fn sconn_dispatch_response_frame_via_channel(
    sconn: &ServerConnection,
    bytes: *const u8,
    size: usize,
) {
    let conn_ptr: *mut LegacyChannelConnectionBase;
    {
        let guard = sconn.channel_proxy_.lock().unwrap();
        if (*guard).is_none() {
            let message: LegacyStdString =
                "rrr::ServerConnection::dispatch_response_frame_via_channel: channel mode flipped on but proxy is unbound (race?). Dropping reply."
                    .to_string();
            // SAFETY: the file pointer is null.
            unsafe { cpp_logging::log_line(2, 0, core::ptr::null(), &message) };
            return;
        }
        conn_ptr = sconn_proxy_ptr(&guard);
    }
    let frame = ChannelFrame {
        payload: bytes,
        size,
    };
    // SAFETY: the proxy Arc keeps the connection alive across the unlocked
    // send, and the caller pins the payload bytes.
    let _ = unsafe { (*conn_ptr).send_frame(&frame) };
}
