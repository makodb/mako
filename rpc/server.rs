//! srpc.server — RPC server (formerly server.hpp + server.cpp).
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

#[allow(unused_imports)]
use crate::reactor as _;
use crate::reactor::FiberFn;

use std::cell::Cell;
use std::sync::atomic::{AtomicBool, AtomicI32, Ordering};
use std::sync::{Arc, Weak as ArcWeak};

// The registration tables are the runtime's own hash containers, not the
// standard-library ones: the retired carrier's C++ surface is
// `rusty::HashMap` / `rusty::HashSet` and the checked type map keeps that
// exact spelling.
use std::collections::{HashMap, HashSet};

use crate::basetypes::Time;
use crate::channel::{
    channel_error_to_string, ChannelConnectionBase, ChannelConnectionProxy, ChannelError,
    ChannelFactoryBase, ChannelFactoryProxy, ChannelFrame, ChannelListenerBase,
    ChannelListenerProxy, OnAcceptCallback, OnClosedCallback, OnErrorCallback, OnFrameCallback,
    NullableChannelConnectionProxy, NullableChannelFactoryProxy,
};
use crate::logging::log_line;
use crate::misc::OneTimeJob;
use crate::serializable::{BinaryReadArchive, BinaryWriteArchive, BufferSink, BufferSource};
use crate::tcp_channel::{make_tcp_factory_proxy, TcpFactory};

use crate::debugging::verify_at;
// This otherwise-unused source-owned import keeps the exact
// `srpc.internal_protocol` provider visible to generated C++; the heartbeat
// rpc-id constant below is read through the crate path.
#[allow(unused_imports)]
use crate::internal_protocol as _;

// Use the canonical reactor worker in both Rust and generated C++.
type PollThread = crate::reactor::PollThread;

// Use the canonical sparse integer values for wire headers.
type v32 = crate::basetypes::v32;
type v64 = crate::basetypes::v64;



// The three errno values this module reports. Spelled as their numeric
// values (not libc macros) so the generated module stays valid after the
// runtime headers include errno.h — the same seam tcp_channel.cpp uses.
const SERVER_ERR_INVALID_ARGUMENT: i32 = 22;
const SERVER_ERR_ALREADY_EXISTS: i32 = 17;
pub const SERVER_ERR_NO_ENTRY: i32 = 2;

#[allow(unsafe_code)]
mod server_ffi {
    unsafe extern "C" {
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
type NullablePendingCounter = Option<Arc<AtomicI32>>;

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
    pending_counter: NullablePendingCounter,
}

impl Drop for PendingRequestGuard {
    fn drop(&mut self) {
        if let Some(counter) = &self.pending_counter {
            counter.fetch_sub(1i32, Ordering::Relaxed);
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
    pub fn attach_pending_guard(&mut self, counter: &NullablePendingCounter) {
        if self.pending_guard.is_none() {
            if let Some(counter) = counter.as_ref() {
                counter.fetch_add(1i32, Ordering::Relaxed);
                self.pending_guard = Some(Box::new(PendingRequestGuard {
                    pending_counter: Some(counter.clone()),
                }));
            }
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


/// Dispatch uses a shared receiver because a stackful handler may suspend
/// while another request enters the same service. Handlers synchronize any
/// mutable application state and release locks before yielding.
///
/// @interface
/// @safe - Pure virtual interface. All declarations carry per-method `// @safe`.
pub trait Service: Send + Sync {
    fn __reg_to__(&mut self, server: &mut Server, svc_index: usize) -> i32;
    fn __dispatch__(&self, rpc_id: i32, req: Box<Request>, sconn: WeakServerConnection);
}

pub type ServiceProxy = Box<dyn Service>;

/// Pass-through factory for services that already inherit Service.
/// @safe - Box move.
pub fn make_service_proxy_from_box(svc: Box<dyn Service>) -> ServiceProxy {
    svc
}

/// Own a typed service and forward registration and shared dispatch.
pub struct ServiceBoxShim<T> {
    svc_: Box<T>,
}

#[cfg_attr(any(), cpp_inherit)]
impl<T: Service> Service for ServiceBoxShim<T> {
    fn __reg_to__(&mut self, server: &mut Server, svc_index: usize) -> i32 {
        self.svc_.__reg_to__(server, svc_index)
    }

    fn __dispatch__(&self, rpc_id: i32, req: Box<Request>, sconn: ArcWeak<ServerConnection>) {
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

/// Shared context for RPC service dispatch.
///
/// This struct is shared between Server and ServerConnection via
/// `Arc<RpcServiceContext>` to avoid raw pointer dependencies.
///
/// The struct is constructed once in `Server::start()` and shared via
/// Arc. All fields are immutable after construction. Service dispatch uses a
/// shared receiver; handlers synchronize mutable application state themselves.
///
/// @safe - All fields are const after construction; the factory just moves
/// owned containers into place. No syscalls, no raw pointers.
pub struct RpcServiceContext {
    pub rpc_to_service: HashMap<i32, usize>,
    pub fast_rpc_ids: HashSet<i32>,
    pub services: Vec<ServiceProxy>,
    pub addr: String,
    pub pending_requests: Arc<ServerPendingRequestsAtomic>,
    pub drop_heartbeat_replies: Arc<ServerDropHeartbeatRepliesAtomic>,
    pub server_instance_id: u64,
}

impl RpcServiceContext {
    pub fn new(
        rpc_map: HashMap<i32, usize>,
        fast_rpc_set: HashSet<i32>,
        svcs: Vec<ServiceProxy>,
        address: String,
        pending_counter: Arc<ServerPendingRequestsAtomic>,
        drop_heartbeats: Arc<ServerDropHeartbeatRepliesAtomic>,
        instance_id: u64,
    ) -> RpcServiceContext {
        RpcServiceContext {
            rpc_to_service: rpc_map,
            fast_rpc_ids: fast_rpc_set,
            services: svcs,
            addr: address,
            pending_requests: pending_counter,
            drop_heartbeat_replies: drop_heartbeats,
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
pub type ServerReplyFn = Option<Box<dyn FnMut(&mut BinaryWriteArchive)>>;

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum ServerConnStatus {
    CONNECTED,
    CLOSED,
}

/// One client connection's server-side state. Status is shared across the
/// server teardown thread and transport callbacks; the context is immutable
/// after publication, and channel storage is protected by its mutex.
///
/// @safe - the delegating methods forward to the `sconn_*` free fns, which
/// carry their own `// @unsafe`.
pub struct ServerConnection {
    pub ctx_: Arc<RpcServiceContext>,
    pub status_: AtomicI32,
    pub weak_self_: WeakServerConnection,
    pub channel_proxy_: std::sync::Mutex<Option<Arc<ChannelConnectionProxy>>>,
    pub channel_mode_: AtomicBool,
    pub count: i32,
}

impl ServerConnection {
    pub fn new(ctx: Arc<RpcServiceContext>, _socket: i32) -> ServerConnection {
        ServerConnection {
            ctx_: ctx,
            status_: AtomicI32::new(ServerConnStatus::CONNECTED as i32),
            weak_self_: Default::default(),
            channel_proxy_: std::sync::Mutex::<Option<Arc<ChannelConnectionProxy>>>::new(None),
            channel_mode_: AtomicBool::new(false),
            count: 0i32,
        }
    }

    pub fn install_self_weak_for_testing(&mut self, weak: WeakServerConnection) {
        self.weak_self_ = weak;
    }

    pub fn is_channel_mode(&self) -> bool {
        self.channel_mode_.load(Ordering::Acquire)
    }

    pub fn connected(&self) -> bool {
        self.status_.load(Ordering::Acquire) == ServerConnStatus::CONNECTED as i32
    }

    pub fn is_closed(&self) -> bool {
        self.status_.load(Ordering::Acquire) == ServerConnStatus::CLOSED as i32
    }

    pub fn reply(&self, req: &Request, error_code: i32, write_fn: ServerReplyFn) {
        sconn_reply(self, req, error_code, write_fn)
    }

    pub fn close(&self) {
        if self.status_.swap(ServerConnStatus::CLOSED as i32, Ordering::AcqRel)
            == ServerConnStatus::CONNECTED as i32
        {
            let message: String =
                format!("server@{} close ServerConnection", self.ctx_.addr);
            // SAFETY: the file pointer is null, so the logger performs no path scan.
            unsafe { log_line(4, 0, core::ptr::null(), &message) };
            // Clone ownership before calling the transport: close may invoke
            // on_closed synchronously, and that callback may re-enter us.
            let channel: Option<Arc<ChannelConnectionProxy>> = self.channel_proxy_.lock().unwrap().clone();
            if let Some(proxy) = channel {
                let connection: &dyn ChannelConnectionBase = &**proxy;
                connection.close();
            }
        }
    }

    // Mirrors ClientConnection::bind_channel_direct (client.cpp): weak
    // clones — one per closure — so the callbacks never cycle through
    // `channel_proxy_`; callbacks installed BEFORE the proxy moves into
    // the slot so none of them runs under the mutex.
    pub fn bind_channel(&mut self, proxy: NullableChannelConnectionProxy) {
        if proxy.is_none() {
            return;
        }
        let mut proxy = proxy.unwrap();
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
            *guard = Some(Arc::new(proxy));
        }
        self.channel_mode_.store(true, Ordering::Release);
    }

    pub fn run_async(&self, f: Option<Box<dyn FnMut()>>) -> i32 {
        if f.is_none() {
            let message: String =
                "srpc::ServerConnection::run_async called with empty callback".to_string();
            // SAFETY: the file pointer is null.
            unsafe { log_line(2, 0, core::ptr::null(), &message) };
            return SERVER_ERR_INVALID_ARGUMENT;
        }
        f.unwrap()();
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
    archive_reply_field: ServerReplyFn,
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
            let message: String =
                "DeferredReply::reply() called multiple times, ignoring".to_string();
            // SAFETY: the file pointer is null.
            unsafe { log_line(2, 0, core::ptr::null(), &message) };
            return;
        }
        let cb = cb_opt.unwrap();
        let sconn_opt = self.weak_sconn_field.upgrade();
        if let Some(sconn) = sconn_opt {
            (*sconn).reply(&self.req_field, 0i32, Some(cb));
        } else {
            let message: String =
                "Connection closed before reply sent, dropping reply".to_string();
            // SAFETY: the file pointer is null.
            unsafe { log_line(4, 0, core::ptr::null(), &message) };
        }
    }

    pub fn reply_error(&mut self, error_code: i32) {
        if self.archive_reply_field.take().is_none() {
            let message: String =
                "DeferredReply::reply_error() called multiple times, ignoring".to_string();
            // SAFETY: the file pointer is null.
            unsafe { log_line(2, 0, core::ptr::null(), &message) };
            return;
        }
        let sconn_opt = self.weak_sconn_field.upgrade();
        if let Some(sconn) = sconn_opt {
            let no_writer: ServerReplyFn = None;
            (*sconn).reply(&self.req_field, error_code, no_writer);
        } else {
            let message: String =
                "Connection closed before error reply sent, dropping reply".to_string();
            // SAFETY: the file pointer is null.
            unsafe { log_line(4, 0, core::ptr::null(), &message) };
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

// Why this constant exists: every reply (sconn_reply) serializes into a fresh
// `BufferSink` whose Vec starts empty, and the header alone is three to four
// small `write_bytes` appends -- so an empty Vec re-allocates on nearly every
// append while it doubles its way up. Measured on the TCP fast-path benchmark,
// amortized Vec growth plus the allocator traffic it induces was 20-30% of
// BOTH servers' samples (`RawVecInner::grow_amortized` in the C++ lane's vec
// port, `finish_grow` under rustc). Seeding the hot sinks with one 64-byte
// allocation covers the whole header-plus-small-payload class in a single
// alloc; larger payloads grow amortized from 64 exactly as before. 64 is
// deliberately modest: these sinks are per-call locals, not pooled buffers.
// The same seeding is spelled kRequestSinkInitialCapacity in rpc/client.rs:
// per-module (the flat-import contract does not admit a cross-module
// root-level const), and per-NAME (two modules exporting one name into
// namespace srpc is an import-time ambiguity for any TU importing both --
// measured: the dual-compile importer and rpcbench both failed to compile).
// Like every module-scope const here (the SERVER_ERR_* block above), this is
// a strong `R` symbol in the module object (P1815 attaches it to the module;
// see EXPECTED_TOTAL_PROVIDER_SYMBOLS' comment in check_srpc_crate_mode.py),
// so it has a pinned row in ABI_SPECS["srpc.server"].
pub const kReplySinkInitialCapacity: usize = 64;


/// Shutdown coordination state — guarded by `Server::shutdown_state_field`.
pub struct ShutdownState {
    pub shutdown: bool,
}

/// Use the supplied worker or create a canonical poll thread.
pub fn server_resolve_poll_thread(
    poll_thread_worker: Option<Arc<PollThread>>,
) -> Option<Arc<PollThread>> {
    if poll_thread_worker.is_none() {
        return Some(crate::reactor::PollThread::create());
    }
    poll_thread_worker
}

/// Copy a NUL-terminated C string into an owned `std::string`.
///
/// # Safety
///
/// `addr` must be a NUL-terminated readable C string for this call.
pub unsafe fn server_dsl_addr_to_string(addr: *const i8) -> String {
    let bytes: *const u8 = addr as *const u8;
    // SAFETY: the caller pins a NUL-terminated readable string, so the C
    // kernel's scan stays inside it and the slice below covers exactly the
    // characters before the terminator.
    let len: usize = unsafe { server_ffi::srpc_cstr_len(bytes) };
    // SAFETY: the scan above proved `len` bytes readable, and an address
    // string is ASCII by construction.
    let text: &str = unsafe { core::str::from_utf8_unchecked(core::slice::from_raw_parts(bytes, len)) };
    let out: String = text.to_string();
    out
}

/// Draw 64 random bits for the instance-id mix. The retired carrier used
/// `std::random_device`; the C kernel reads the same kernel entropy pool.
pub fn server_random_u64() -> u64 {
    // SAFETY: the C kernel writes a fully initialized 64-bit value and takes
    // no pointers from this side.
    unsafe { server_ffi::srpc_random_u64() }
}

/// srpc's own clock (Time::now microseconds, scaled to the nano range the
/// id-mix historically used; entropy comes from the random_u64 mix, not
/// clock granularity).
pub fn server_now_nanos() -> u64 {
    Time::now(true) * 1000u64
}

/// Block until `do_shutdown()` flips the flag.
//
// MEASURED ABI PIN, not style. `clippy::borrowed_box` wants `&std::sync::Condvar`
// here. The incumbent carrier's generated C++ (rpc/server.cpp at c6c55ba,
// line 1173) exports exactly
//     void server_wait_for_shutdown_impl(const std::sync::Mutex<ShutdownState>&,
//                                        const rusty::Box<std::sync::Condvar>&)
// and taking the lint regenerates that declaration as `const std::sync::Condvar&`
// -- a different mangled symbol, i.e. one incumbent symbol removed and one
// added. The `&Box<..>` spelling is the ABI.
#[allow(clippy::borrowed_box)]
pub fn server_wait_for_shutdown_impl(
    state: &std::sync::Mutex<ShutdownState>,
    cond: &Box<std::sync::Condvar>,
) {
    let entering: String = "Server::wait_for_shutdown".to_string();
    // SAFETY: the file pointer is null.
    unsafe { log_line(4, 0, core::ptr::null(), &entering) };
    let mut guard = state.lock().unwrap();
    guard = cond
        .wait_while(guard, |s: &mut ShutdownState| !s.shutdown)
        .unwrap();
    drop(guard);
    let leaving: String = "Server::wait_for_shutdown - done".to_string();
    // SAFETY: the file pointer is null.
    unsafe { log_line(4, 0, core::ptr::null(), &leaving) };
}

pub fn server_generate_instance_id() -> u64 {
    let time_component: u64 = server_now_nanos();
    let random_component: u64 = server_random_u64();
    let pid_component: u64 = (std::process::id() as u64) << 48;
    // 0x7fff_ffff_ffff_ffff is std::numeric_limits<int64_t>::max(): the id
    // is kept non-negative because it crosses the wire as a signed i64.
    let mut id: u64 =
        (time_component ^ random_component ^ pid_component) & 0x7fffffffffffffffu64;
    if id == 0u64 {
        id = 1u64;
    }
    let message: String = format!("Server: generated instance_id={}", id);
    // SAFETY: the file pointer is null.
    unsafe { log_line(4, 0, core::ptr::null(), &message) };
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
        let message: String = format!(
            "Server::drain: already past the draining phases ({})",
            shutdown_phase_to_string(current_phase)
        );
        // SAFETY: the file pointer is null.
        unsafe { log_line(4, 0, core::ptr::null(), &message) };
        return pending.load(Ordering::Relaxed) == 0i32;
    }
    let entering: String = format!(
        "Server::drain: transitioning to DRAINING, pending={}",
        pending.load(Ordering::Relaxed)
    );
    // SAFETY: the file pointer is null.
    unsafe { log_line(3, 0, core::ptr::null(), &entering) };
    phase.set(ShutdownPhase::DRAINING);
    let start_us = crate::basetypes::Time::now(true);
    let timeout_us = timeout_ms * 1000u64;
    while pending.load(Ordering::Relaxed) > 0i32 {
        let elapsed_us = crate::basetypes::Time::now(true) - start_us;
        if elapsed_us >= timeout_us {
            let expired: String = format!(
                "Server::drain: timeout after {} ms, pending={}",
                timeout_ms,
                pending.load(Ordering::Relaxed)
            );
            // SAFETY: the file pointer is null.
            unsafe { log_line(2, 0, core::ptr::null(), &expired) };
            return false;
        }
        std::thread::sleep(std::time::Duration::from_micros(1000u64));
    }
    let done: String = "Server::drain: completed, all requests drained".to_string();
    // SAFETY: the file pointer is null.
    unsafe { log_line(3, 0, core::ptr::null(), &done) };
    true
}

/// NOTE: hooks run WHILE the mutex is held. That is the pre-existing
/// behaviour and is preserved deliberately.
pub fn server_run_shutdown_hooks(hooks: &std::sync::Mutex<Vec<ShutdownHook>>) {
    let message: String =
        "Server::graceful_shutdown: transitioning to CLOSING, executing hooks".to_string();
    // SAFETY: the file pointer is null.
    unsafe { log_line(3, 0, core::ptr::null(), &message) };
    let mut guard = hooks.lock().unwrap();
    for hook in (*guard).iter_mut() {
        server_invoke_shutdown_hook_safely(hook);
    }
}

/// Parse the decimal prefix accepted by the historical `std::stoi` path.
/// Leading ASCII whitespace and a sign are permitted; trailing text is ignored.
/// Keep the old 63-byte limit and reject missing digits or int32 overflow.
pub fn server_parse_port(text: &str) -> Option<i32> {
    let bytes = text.as_bytes();
    if bytes.len() > 63 {
        return None;
    }
    let mut offset: usize = 0;
    while offset < bytes.len() {
        let byte = bytes[offset];
        if byte != b' ' && byte != b'\t' && byte != b'\n'
            && byte != b'\r' && byte != 11u8 && byte != 12u8
        {
            break;
        }
        offset += 1;
    }
    let mut negative = false;
    if offset < bytes.len() && (bytes[offset] == b'+' || bytes[offset] == b'-') {
        negative = bytes[offset] == b'-';
        offset += 1;
    }
    let start = offset;
    let mut magnitude: i64 = 0;
    let limit: i64 = if negative { 2147483648 } else { 2147483647 };
    while offset < bytes.len() && bytes[offset] >= b'0' && bytes[offset] <= b'9' {
        let digit = (bytes[offset] - b'0') as i64;
        if magnitude > (limit - digit) / 10 {
            return None;
        }
        magnitude = magnitude * 10 + digit;
        offset += 1;
    }
    if offset == start {
        return None;
    }
    if negative {
        Some((-magnitude) as i32)
    } else {
        Some(magnitude as i32)
    }
}

/// The invoker catches a panicking hook and logs it, exactly as the old
/// two-arm try/catch did.
pub fn server_invoke_shutdown_hook_safely(hook: &mut ShutdownHook) {
    // Keep the move-only C++ hook borrowed by the closure.
    #[allow(clippy::redundant_closure)]
    let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| hook()));
    if r.is_ok() {
        return;
    }
    let payload = r.unwrap_err();
    let msg: Option<String>;
    if let Some(text) = payload.downcast_ref::<&str>() {
        msg = Some((*text).to_string());
    } else if let Some(text) = payload.downcast_ref::<String>() {
        msg = Some(text.clone());
    } else {
        msg = None;
    }
    if let Some(text) = msg {
        let message: String = format!(
            "Server::graceful_shutdown: hook threw exception: {}",
            text
        );
        // SAFETY: the file pointer is null.
        unsafe { log_line(1, 0, core::ptr::null(), &message) };
    } else {
        let message: String =
            "Server::graceful_shutdown: hook threw unknown exception".to_string();
        // SAFETY: the file pointer is null.
        unsafe { log_line(1, 0, core::ptr::null(), &message) };
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
    shutdown_state_field: std::sync::Mutex<ShutdownState>,
    shutdown_cond_field: Box<std::sync::Condvar>,
    shutdown_phase_field: Cell<ShutdownPhase>,
    shutdown_hooks_field: std::sync::Mutex<Vec<ShutdownHook>>,
    pending_requests_field: Arc<ServerPendingRequestsAtomic>,
    drop_heartbeat_replies_field: Arc<ServerDropHeartbeatRepliesAtomic>,
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
    channel_sconns_field: Arc<std::sync::Mutex<ChannelSconns>>,
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
            // Erase the job type for the worker command queue.
            let close_job_erased: Arc<dyn crate::misc::Job> = close_job;
            let pt: &Arc<PollThread> = self.poll_thread_field.as_ref().unwrap();
            pt.add(close_job_erased);
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
                // 'rusty::Arc<srpc::ServerConnection>'". The `*` is what emits
                // the `deref_if_pointer_like` the call needs.
                (*guard.conns[i]).close();
                i += 1usize;
            }
            guard.conns.clear();
        }
        self.ctx_field = None;
    }
}

impl Server {
    pub fn new(poll_thread_worker: Option<Arc<PollThread>>) -> Server {
        Server {
            pending_services_field: Vec::<ServiceProxy>::new(),
            pending_rpc_to_service_field: HashMap::<i32, usize>::new(),
            pending_fast_rpc_ids_field: HashSet::<i32>::new(),
            ctx_field: None,
            poll_thread_field: server_resolve_poll_thread(poll_thread_worker),
            shutdown_state_field: std::sync::Mutex::<ShutdownState>::new(ShutdownState {
                shutdown: false,
            }),
            shutdown_cond_field: Box::new(std::sync::Condvar::new()),
            shutdown_phase_field: Cell::new(ShutdownPhase::RUNNING),
            shutdown_hooks_field: std::sync::Mutex::<Vec<ShutdownHook>>::new(Vec::<ShutdownHook>::new()),
            pending_requests_field: Arc::new(ServerPendingRequestsAtomic::new(0i32)),
            drop_heartbeat_replies_field: Arc::new(ServerDropHeartbeatRepliesAtomic::new(false)),
            instance_id_field: server_generate_instance_id(),
            channel_factory_field: None,
            channel_listener_field: None,
            channel_sconns_field: Arc::new(std::sync::Mutex::<ChannelSconns>::new(ChannelSconns {
                closed: false,
                conns: Vec::<Arc<ServerConnection>>::new(),
            })),
        }
    }

    pub fn set_channel_factory(&mut self, factory: NullableChannelFactoryProxy) {
        if factory.is_none() {
            return;
        }
        let factory = factory.unwrap();
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

    pub fn instance_id(&self) -> u64 {
        self.instance_id_field
    }

    pub fn service_count(&self) -> usize {
        if let Some(ctx) = self.ctx_field.as_ref() {
            return ctx.services.len();
        }
        self.pending_services_field.len()
    }

    pub fn addr(&self) -> String {
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
            let message: String = "srpc::Server::start: bind_addr is NULL!".to_string();
            // SAFETY: the file pointer is null.
            unsafe { log_line(1, 0, core::ptr::null(), &message) };
            return -1i32;
        }
        // SAFETY: the caller guarantees a NUL-terminated readable string that
        // stays alive for this call.
        let addr_str: String = unsafe { server_dsl_addr_to_string(bind_addr) };

        // Registration is complete. Publish the owning service vector without
        // mutable borrow state so concurrent and suspended handlers share it.
        let services: Vec<ServiceProxy> = core::mem::take(&mut self.pending_services_field);

        // Create the immutable RpcServiceContext from the pending
        // registration data.
        self.ctx_field = Some(Arc::new(RpcServiceContext::new(
            core::mem::take(&mut self.pending_rpc_to_service_field),
            core::mem::take(&mut self.pending_fast_rpc_ids_field),
            services,
            addr_str.clone(),
            self.pending_requests_field.clone(),
            self.drop_heartbeat_replies_field.clone(),
            self.instance_id_field,
        )));

        // Auto-install a default TcpFactory if the caller has not bound one.
        if !self.is_channel_factory_bound() {
            let tcp_factory: Arc<TcpFactory> = Arc::new(TcpFactory::new(
                self.poll_thread_field.as_ref().unwrap().clone(),
            ));
            self.set_channel_factory(Some(make_tcp_factory_proxy(tcp_factory)));
        }

        if self.is_channel_factory_bound() {
            let listener_opt = {
                let factory: &mut Box<dyn ChannelFactoryBase> =
                    self.channel_factory_field.as_mut().unwrap();
                factory.make_listener()
            };
            if listener_opt.is_none() {
                let message: String = format!(
                    "srpc::Server::start: factory->make_listener() returned a null proxy (factory backend={})",
                    "unknown"
                );
                // SAFETY: the file pointer is null.
                unsafe { log_line(1, 0, core::ptr::null(), &message) };
                self.ctx_field = None;
                return -1i32;
            }
            let mut listener: ChannelListenerProxy = listener_opt.unwrap();

            let sconns_arc: Arc<std::sync::Mutex<ChannelSconns>> = self.channel_sconns_field.clone();
            let ctx_arc: Arc<RpcServiceContext> = self.ctx_field.as_ref().unwrap().clone();

            {
                let ch: &mut Box<dyn ChannelListenerBase> = &mut listener;
                ch.set_on_accept(OnAcceptCallback::from_callable(Box::new(
                    move |conn_proxy: NullableChannelConnectionProxy| {
                        if conn_proxy.is_none() {
                            return;
                        }
                        let sconn: Arc<ServerConnection> =
                            Arc::new(ServerConnection::new(ctx_arc.clone(), -1i32));
                        // as_ptr, not get_mut, and not const_cast: the Arc was
                        // just made and has not been published, so this is the
                        // unique minting window -- but `Arc::get_mut` cannot
                        // express it here.  Installing the self-weak below makes
                        // the weak count nonzero, and Rust's `Arc::get_mut`
                        // yields `None` unless strong == 1 AND weak == 0, so
                        // both `unwrap()`s below used to abort on every accepted
                        // connection.  The C++ `rusty::Arc::get_mut` tests only
                        // the strong count (third-party/rusty-cpp/include/rusty/
                        // arc.hpp), which is why the C++ lane never saw it.
                        // `Arc::as_ptr` is the house spelling for this window --
                        // reactor/reactor.rs:1965 does the same -- and it is
                        // documented in that header as working with live
                        // references.
                        {
                            let weak = Arc::downgrade(&sconn);
                            // SAFETY: sole strong owner, not yet published to any
                            // other thread; the weak handle above cannot observe
                            // the value, so this &mut is unaliased.
                            let mut_sconn: &mut ServerConnection =
                                unsafe { &mut *(Arc::as_ptr(&sconn) as *mut ServerConnection) };
                            mut_sconn.install_self_weak_for_testing(weak);
                        }
                        {
                            // SAFETY: same unpublished-minting-window contract.
                            let mut_sconn2: &mut ServerConnection =
                                unsafe { &mut *(Arc::as_ptr(&sconn) as *mut ServerConnection) };
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
                ) as Box<dyn Fn(NullableChannelConnectionProxy) + Send + Sync>));
                ch.set_on_error(OnErrorCallback::from_callable(Box::new(
                    move |err: ChannelError, msg: &str| {
                        let reason: &str = channel_error_to_string(err);
                        let message: String =
                            format!("srpc::Server: channel listener error {}: {}", reason, msg);
                        // SAFETY: the file pointer is null.
                        unsafe { log_line(2, 0, core::ptr::null(), &message) };
                    },
                ) as Box<dyn Fn(ChannelError, &str) + Send + Sync>));
            }

            let listen_err = {
                let ch2: &mut Box<dyn ChannelListenerBase> = &mut listener;
                ch2.listen(&addr_str)
            };
            if listen_err != ChannelError::None {
                let reason: &str = channel_error_to_string(listen_err);
                let message: String = format!(
                    "srpc::Server::start: channel listener failed to bind {}: {}",
                    addr_str, reason
                );
                // SAFETY: the file pointer is null.
                unsafe { log_line(1, 0, core::ptr::null(), &message) };
                self.ctx_field = None;
                return -1i32;
            }

            self.channel_listener_field = Some(listener);
            return 0i32;
        }

        verify_at(false, file!(), line!());
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
        let local: String = listener.local_address();
        let colon = local.rfind(':');
        if colon.is_none() {
            let message: String =
                format!("Server::get_bound_port: malformed local_address {}", local);
            // SAFETY: the file pointer is null.
            unsafe { log_line(1, 0, core::ptr::null(), &message) };
            return -1i32;
        }
        let tail: String = local[colon.unwrap() + 1usize..].to_string();
        let parsed = server_parse_port(&tail);
        if parsed.is_none() {
            let message: String = format!(
                "Server::get_bound_port: failed to parse port from {}",
                local
            );
            // SAFETY: the file pointer is null.
            unsafe { log_line(1, 0, core::ptr::null(), &message) };
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

    pub fn for_each_service<F: FnMut(&dyn Service)>(&self, mut callback: F) {
        let ctx = self.ctx_field.as_ref().unwrap();
        for service in ctx.services.iter() {
            callback(&**service);
        }
    }

}

// ===========================================================================
// Implementation namespace — free functions the methods above delegate to.
// ===========================================================================

/// Build the reply header and optional payload, then dispatch through the channel.
/// An absent writer sends only the header.
pub fn sconn_reply(
    sconn: &ServerConnection,
    req: &Request,
    error_code: i32,
    write_fn: ServerReplyFn,
) {
    let mut body_sink: BufferSink = BufferSink {
        bytes: Vec::<u8>::with_capacity(kReplySinkInitialCapacity),
    };
    let mut ar_store = BinaryWriteArchive {
        // SAFETY: `body_sink` is a live, exclusively borrowed local that
        // outlives the archive built over it.
        sink_: unsafe { crate::serializable::make_sink_proxy_buffer(&raw mut body_sink) },
    };
    let ar: &mut BinaryWriteArchive = &mut ar_store;
    // SAFETY: foreign named-module serialization boundary; both borrows
    // are held only for the duration of the call.
    crate::serializable::Serialize_::serialize(&v64::new(req.xid), ar);
    // SAFETY: foreign named-module serialization boundary; both borrows
    // are held only for the duration of the call.
    crate::serializable::Serialize_::serialize(&v32::new(error_code), ar);
    // SAFETY: foreign named-module serialization boundary; both borrows
    // are held only for the duration of the call.
    crate::serializable::Serialize_::serialize(
        &v64::new(sconn.ctx_.server_instance_id as i64),
        ar,
    );
    if let Some(mut write) = write_fn {
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

/// Reject a request whose typed arguments could not be decoded.
///
/// Generated service wrappers call this before invoking application code.
pub fn reject_malformed_request(req: &Request, weak_sconn: &ArcWeak<ServerConnection>) {
    let sconn = weak_sconn.upgrade();
    if let Some(sconn) = sconn {
        (*sconn).reply(req, SERVER_ERR_INVALID_ARGUMENT, None);
    }
}

/// The three channel callbacks the connection installs, as free functions so
/// each closure stays the single-call shape the emitter lowers into a
/// const-callable `rusty::Function`.
///
/// Dispatch reads atomic connection status and the immutable shared context.
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
    let message: String =
        format!("srpc::ServerConnection: channel error {}: {}", reason, msg);
    // SAFETY: the file pointer is null.
    unsafe { log_line(2, 0, core::ptr::null(), &message) };
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
/// which is benign — this is the non-exported `namespace srpc` and
/// server.cpp is the module's only TU.
//
// The set is constructed under its lock after the first missing RPC ID.
// An empty Option makes the process-wide static valid with std HashSet.
static g_rpc_id_missing: std::sync::Mutex<Option<HashSet<i32>>> =
    std::sync::Mutex::new(None);

pub fn sconn_dispatch_in_fiber(
    ctx: Arc<RpcServiceContext>,
    svc_index: usize,
    rpc_id: i32,
    req: Box<Request>,
    weak_this: ArcWeak<ServerConnection>,
) {
    let svc: &dyn Service = &*ctx.services[svc_index];
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
    if sconn.status_.load(Ordering::Acquire) == ServerConnStatus::CLOSED as i32 {
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
        let message: String =
            "srpc::ServerConnection: empty channel-mode request frame, dropping".to_string();
        // SAFETY: the file pointer is null.
        unsafe { log_line(2, 0, core::ptr::null(), &message) };
        return;
    }
    // SAFETY: `req_box.src` is owned by the live boxed request.
    let mut header_ar = BinaryReadArchive::new(unsafe {
        crate::serializable::make_source_proxy_buffer(&raw mut req_box.src)
    });
    let mut v_xid = v64::new(0i64);
    // SAFETY: foreign named-module serialization boundary; both borrows
    // are held only for the duration of the call.
    crate::serializable::Deserialize_::deserialize(&mut v_xid, &mut header_ar);
    req_box.xid = v_xid.get();
    let pending_counter: Arc<ServerPendingRequestsAtomic> = sconn.ctx_.pending_requests.clone();
    req_box.attach_pending_guard(&Some(pending_counter));

    // sizeof(i32) spelled as its value: not enough bytes for rpc_id.
    if req_box.src.remaining() < 4usize {
        let empty_fn1: ServerReplyFn = None;
        sconn_reply(sconn, &req_box, SERVER_ERR_INVALID_ARGUMENT, empty_fn1);
        return;
    }

    let mut rpc_id: i32 = 0i32;
    // SAFETY: foreign named-module serialization boundary; both borrows
    // are held only for the duration of the call.
    crate::serializable::Deserialize_::deserialize(&mut rpc_id, &mut header_ar);
    if rpc_id == crate::internal_protocol::kInternalHeartbeatRpcId {
        let hb: &Arc<ServerDropHeartbeatRepliesAtomic> = &sconn.ctx_.drop_heartbeat_replies;
        if !hb.load(Ordering::Acquire) {
            let empty_fn2: ServerReplyFn = None;
            sconn_reply(sconn, &req_box, 0i32, empty_fn2);
        }
        return;
    }

    let svc_index_opt = sconn.ctx_.rpc_to_service.get(&rpc_id);
    if svc_index_opt.is_none() {
        let mut surpress_warning = false;
        {
            let mut guard = g_rpc_id_missing.lock().unwrap();
            if guard.is_none() {
                *guard = Some(HashSet::new());
            }
            let missing = guard.as_mut().unwrap();
            if !missing.contains(&rpc_id) {
                missing.insert(rpc_id);
            } else {
                surpress_warning = true;
            }
        }
        if !surpress_warning {
            let message: String = format!(
                "srpc::ServerConnection: no handler for rpc_id = {} (channel-mode dispatch)",
                rpc_id
            );
            // SAFETY: the file pointer is null.
            unsafe { log_line(2, 0, core::ptr::null(), &message) };
        }
        let empty_fn3: ServerReplyFn = None;
        sconn_reply(sconn, &req_box, SERVER_ERR_NO_ENTRY, empty_fn3);
        return;
    }

    let svc_index: usize = *svc_index_opt.unwrap();
    let weak_this: WeakServerConnection = sconn.weak_self_.clone();
    if sconn.ctx_.fast_rpc_ids.contains(&rpc_id) {
        // Fast inline dispatch — no fiber spawn.
        let svc: &dyn Service = &*sconn.ctx_.services[svc_index];
        svc.__dispatch__(rpc_id, req_box, weak_this);
    } else {
        // Slow path — spawn a fiber so the handler can yield (e.g. for
        // nested RPC calls). The ctx Arc clone keeps the services alive
        // even if the connection is closed mid-flight.
        let ctx2: Arc<RpcServiceContext> = sconn.ctx_.clone();
        // `FnMut` (not `FnOnce`) is what `rusty::Function<void()>` models, so
        // the moved-in request and weak handle are parked in `Option` slots
        // the first call takes. The fiber runs the job exactly once.
        let mut parked_req: Option<Box<Request>> = Some(req_box);
        let mut parked_weak: Option<WeakServerConnection> = Some(weak_this);
        // Same redundant-`mut` reason as the service loop above: the C++
        // lowering must move the Function into `fiber_create_run_impl`.
        #[allow(unused_mut)]
        let mut job_fn: FiberFn = Some(Box::new(move || {
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
        }));
        // `create_run_impl` is the non-generic entry `Fiber::create_run<Func>`
        // itself calls; naming it directly keeps this an ordinary imported
        // method rather than an imported member template.
        //
        // The closure is a well-formed owning job the fiber runtime takes
        // over; `FiberFn` is the reactor's own erased-callable type.
        crate::reactor::fiber_create_run_impl(job_fn, "", 0i64);
    }
}

/// Clone the channel owner while holding the slot mutex, then release the
/// lock before sending. The clone pins the proxy even if teardown replaces
/// the slot, and shared dispatch permits synchronous callback re-entry.
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
    let channel: Option<Arc<ChannelConnectionProxy>> = sconn.channel_proxy_.lock().unwrap().clone();
    if let Some(proxy) = channel {
        let frame = ChannelFrame { payload: bytes, size };
        let connection: &dyn ChannelConnectionBase = &**proxy;
        // SAFETY: the caller pins the frame bytes, and this Arc pins the
        // shared transport until its synchronous send returns.
        let _ = unsafe { connection.send_frame(&frame) };
    }
}
