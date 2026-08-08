// rrr.server — RPC server (formerly server.hpp + server.cpp).
//
// Hosts service implementations and dispatches inbound RPC frames to
// the right handler. Sits above the channel layer (`tcp_channel`,
// `inmemory_channel`) which surfaces a transport-agnostic
// `ChannelListenerProxy` / `ChannelConnectionProxy` to this module.
// The legacy listener/socket-path was retired in 5g3 — this module
// is now purely the request/reply orchestration layer.
module;

#include <cstddef>
#include <cstdint>
#include <stdlib.h>

#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/cell.hpp>
#include <rusty/function.hpp>
#include <rusty/option.hpp>
#include <rusty/unsafe_cell.hpp>
#include <rusty/rusty.hpp>

export module rrr.server;

import std;
import rusty;
import rrr.basetypes;
import rrr.callback_wrapper;
import rrr.channel;
import rrr.debugging;
import rrr.epoll_wrapper;
import rrr.internal_protocol;
import rrr.logging;
import rrr.misc;
import rrr.reactor;
import rrr.serializable;
import rrr.tcp_channel;
import rrr.threading;
import rrr.utils;

// ===========================================================================
// Class declarations (from former server.hpp)
// ===========================================================================
// @safe - Class declarations live in this export namespace. The
// classes and helpers are individually annotated below: PendingRequestGuard
// / Request / Service / ServiceTypedBoxAdapter / RpcServiceContext are
// `// @safe` shells; ServerConnection is `// @safe` with per-method
// `// @unsafe` overrides on the socket/marshal/raw-pointer paths. The
// `shutdown_phase_to_string` free function is `// @safe`. The
// `make_service_proxy_from_box` / `make_service_proxy_from_typed_box`
// helpers are pure Box adapters.
// One-line scaffolding for the inline-Rust DSL: the grammar has no
// spelling for C's `char` (DSL `char` is Rust's 4-byte scalar and
// lowers to `char32_t`; `core::ffi::c_char` lowers to an undefined
// `rusty::ffi::c_char`). Declared outside the export namespace so it
// stays module-local -- it is only ever named inside generated bodies.
/*RUSTYCPP:GEN-DISPATCH-BEGIN*/
namespace rusty { namespace detail {
RUSTY_METHOD_DISPATCH(unwrap)
} } // namespace rusty::detail (issue #31 deref_call dispatch)
/*RUSTYCPP:GEN-DISPATCH-END*/

using c_char = char;

export namespace rrr {

class Server;
class ServerConnection;
struct RpcServiceContext;

/**
 * Server shutdown phases for graceful shutdown support.
 * Progression: RUNNING -> STOP_ACCEPTING -> DRAINING -> CLOSING -> STOPPED
 */
// `ShutdownPhase` — server-shutdown FSM state. Authored as inline Rust
// DSL: the `#if RUSTYCPP_RUST` block below is the source of truth;
// the transpiler regenerates the matching `RUSTYCPP:GEN-BEGIN ... END`
// block.
#if RUSTYCPP_RUST
enum ShutdownPhase {
    RUNNING,
    STOP_ACCEPTING,
    DRAINING,
    CLOSING,
    STOPPED,
}

fn shutdown_phase_to_string(phase: ShutdownPhase) -> &'static str {
    match phase {
        ShutdownPhase::RUNNING => "RUNNING",
        ShutdownPhase::STOP_ACCEPTING => "STOP_ACCEPTING",
        ShutdownPhase::DRAINING => "DRAINING",
        ShutdownPhase::CLOSING => "CLOSING",
        ShutdownPhase::STOPPED => "STOPPED",
        _ => "UNKNOWN",
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=server.shutdown_phase version=1 rust_sha256=03547ba09ca4503ae51b144ca0a574c58b13aed97af72b34726b70428ad7d8dc*/
enum class ShutdownPhase;
constexpr ShutdownPhase ShutdownPhase_RUNNING();
constexpr ShutdownPhase ShutdownPhase_STOP_ACCEPTING();
constexpr ShutdownPhase ShutdownPhase_DRAINING();
constexpr ShutdownPhase ShutdownPhase_CLOSING();
constexpr ShutdownPhase ShutdownPhase_STOPPED();
std::string_view shutdown_phase_to_string(ShutdownPhase phase);

enum class ShutdownPhase {
    RUNNING,
    STOP_ACCEPTING,
    DRAINING,
    CLOSING,
    STOPPED
};
inline constexpr ShutdownPhase ShutdownPhase_RUNNING() { return ShutdownPhase::RUNNING; }
inline constexpr ShutdownPhase ShutdownPhase_STOP_ACCEPTING() { return ShutdownPhase::STOP_ACCEPTING; }
inline constexpr ShutdownPhase ShutdownPhase_DRAINING() { return ShutdownPhase::DRAINING; }
inline constexpr ShutdownPhase ShutdownPhase_CLOSING() { return ShutdownPhase::CLOSING; }
inline constexpr ShutdownPhase ShutdownPhase_STOPPED() { return ShutdownPhase::STOPPED; }

std::string_view shutdown_phase_to_string(ShutdownPhase phase) {
    return ({ auto&& _m = phase; std::optional<std::string_view> _match_value; bool _m_matched = false; if (!_m_matched && (_m == ShutdownPhase::RUNNING)) { _match_value.emplace(std::move(std::string_view("RUNNING"))); _m_matched = true; } if (!_m_matched && (_m == ShutdownPhase::STOP_ACCEPTING)) { _match_value.emplace(std::move(std::string_view("STOP_ACCEPTING"))); _m_matched = true; } if (!_m_matched && (_m == ShutdownPhase::DRAINING)) { _match_value.emplace(std::move(std::string_view("DRAINING"))); _m_matched = true; } if (!_m_matched && (_m == ShutdownPhase::CLOSING)) { _match_value.emplace(std::move(std::string_view("CLOSING"))); _m_matched = true; } if (!_m_matched && (_m == ShutdownPhase::STOPPED)) { _match_value.emplace(std::move(std::string_view("STOPPED"))); _m_matched = true; } if (!_m_matched) { _match_value.emplace(std::move(std::string_view("UNKNOWN"))); _m_matched = true; } if (!_m_matched) { rusty::intrinsics::unreachable_panic(); } std::move(_match_value).value(); });
}
/*RUSTYCPP:GEN-END id=server.shutdown_phase*/

// Shutdown hook callback type. rusty::Function is move-only; the
// hooks are stored in `Vec<ShutdownHook>` (no clone()), pushed via
// move in `add_shutdown_hook`, and invoked by reference inside the
// graceful-shutdown loop — see server.cpp.
using ShutdownHook = rusty::Function<void()>;

/**
 * The raw packet sent from client will be like this:
 * <size> <xid> <rpc_id> <arg1> <arg2> ... <argN>
 * NOTE: size does not include the size itself (<xid>..<argN>).
 *
 * For the request object, the marshal only contains <arg1>..<argN>,
 * other fields are already consumed.
 */
// @safe - RAII guard for one in-flight request: decrements the shared
// pending-request counter on drop. The matching increment is done at the
// guard's single construction site (`Request::attach_pending_guard`).
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block is the
// source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block with the C++ struct + destructor.
//
// The counter is an Arc-shared `rusty::sync::atomic::AtomicI32`; atomic
// ops are const (Rust's `&self`), so the shared Arc view is enough. The
// two hand-written `pending_guard_acquire` / `pending_guard_release`
// shims that used to live here are GONE — probe-verified, the DSL keeps
// `self.pending_counter.is_valid()` an Arc dot-call while
// `self.pending_counter.fetch_sub(..)` auto-arrows to
// `pending_counter->fetch_sub(..)`, exactly the lowering
// `Server::decrement_pending` already relies on. So the "&self.field
// lowers to a pointer, hence a pointer-taking release shim" workaround
// dissolves: nothing needs to be passed at all.
//
// `impl Drop` makes the emitted struct move-only (copy ctor deleted),
// matching how the guard is used — only ever boxed and moved.
#if RUSTYCPP_RUST
struct PendingRequestGuard {
    pending_counter: rusty::Arc<rusty::sync::atomic::AtomicI32>,
}

impl Drop for PendingRequestGuard {
    fn drop(&mut self) {
        if self.pending_counter.is_valid() {
            self.pending_counter.fetch_sub(1i32, rusty::sync::atomic::Ordering::Relaxed);
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=server.pending_guard version=1 rust_sha256=8f165530f62974a82b335fb863241fca728d4612a8f1601cada63c402c5e6104*/
struct PendingRequestGuard;

struct PendingRequestGuard {
    rusty::Arc<rusty::sync::atomic::AtomicI32> pending_counter;
    mutable bool _rusty_forgotten = false;
    PendingRequestGuard(rusty::Arc<rusty::sync::atomic::AtomicI32> pending_counter_init) : pending_counter(std::move(pending_counter_init)) {}
    PendingRequestGuard(const PendingRequestGuard&) = delete;
    PendingRequestGuard(PendingRequestGuard&& other) noexcept : pending_counter(std::move(other.pending_counter)) {
        this->_rusty_forgotten = other._rusty_forgotten;
        other._rusty_forgotten = true;
    }
    PendingRequestGuard& operator=(const PendingRequestGuard&) = delete;
    PendingRequestGuard& operator=(PendingRequestGuard&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        this->~PendingRequestGuard();
        new (this) PendingRequestGuard(std::move(other));
        return *this;
    }
    void rusty_mark_forgotten() const noexcept { _rusty_forgotten = true; rusty::detail::mark_forgotten_if_supported(this->pending_counter); }


    ~PendingRequestGuard() noexcept(false);
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};


PendingRequestGuard::~PendingRequestGuard() noexcept(false) {
    if (_rusty_forgotten) { return; }
    if (this->pending_counter.is_valid()) {
        this->pending_counter->fetch_sub(static_cast<int32_t>(1), rusty::sync::atomic::Ordering::Relaxed);
    }
}
/*RUSTYCPP:GEN-END id=server.pending_guard*/

// `Request` — simple in-flight RPC request container.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. `make_box<Request>()` zero-init
// callers observe: empty body, null/0 src cursor, None pending_guard.
//
// Marshal-deprecation step 1: the request no longer carries a Marshal.
// `body` owns the frame bytes; `src` is the serde-shaped read cursor
// (BufferSource) over them. The cursor state persists from the server
// header parse (xid, rpc_id) into the generated handler's argument
// reads — consumers build a `BinaryReadArchive` over
// `make_source_proxy(&req->src)` (RefMut adapter, cursor advances in
// place). INVARIANT: `src` borrows `body`'s heap buffer, so `body` is
// filled exactly once (request_fill_body) before `src` is pointed at
// it, and Request always lives behind a Box (Vec's heap data is stable
// under Box moves).
#if RUSTYCPP_RUST
struct Request {
    body: Vec<u8>,
    src: BufferSource,
    xid: i64,
    pending_guard: rusty::Option<rusty::Box<PendingRequestGuard>>,
}

impl Request {
    fn attach_pending_guard(&mut self, counter: &rusty::Arc<rusty::sync::atomic::AtomicI32>) {
        if self.pending_guard.is_none() && counter.is_valid() {
            counter.fetch_add(1i32, rusty::sync::atomic::Ordering::Relaxed);
            self.pending_guard = rusty::Some(rusty::make_box::<PendingRequestGuard>(counter.clone()));
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=server.request version=1 rust_sha256=88cbb52b4b7ce92056535e16eb87eb9a3bbe49f8c22e43361f798782e7053a5f*/
struct Request;

struct Request {
    rusty::Vec<uint8_t> body;
    BufferSource src;
    int64_t xid;
    rusty::Option<rusty::Box<PendingRequestGuard>> pending_guard;

    void attach_pending_guard(const rusty::Arc<rusty::sync::atomic::AtomicI32>& counter);
};


void Request::attach_pending_guard(const rusty::Arc<rusty::sync::atomic::AtomicI32>& counter) {
    if (this->pending_guard.is_none() && counter.is_valid()) {
        counter->fetch_add(static_cast<int32_t>(1), rusty::sync::atomic::Ordering::Relaxed);
        this->pending_guard = rusty::Option<rusty::Box<PendingRequestGuard>>(rusty::make_box<PendingRequestGuard>(rusty::clone(counter)));
    }
}
/*RUSTYCPP:GEN-END id=server.request*/

// Forward declaration for WeakServerConnection
class ServerConnection;

// Type alias for Arc weak reference (must be before Service for __dispatch__)
using WeakServerConnection = rusty::sync::Weak<ServerConnection>;

// @interface
// @safe - Pure virtual interface. All declarations carry per-method `// @safe`.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. Tier 2.3 of the rrr trait sweep.
//
// `__get_service__()` was a 1-liner default impl (`return *this;`) that
// the only "non-default" implementer (`ServiceTypedBoxAdapter`) also
// implemented as `return *this;` — i.e., it carried no semantics over
// just dereferencing the `Box<Service>` directly. The
// `for_each_service` callback now uses `**guard` instead. Dropping
// the method is what lets the trait be a clean DSL trait (the DSL
// `pub trait` emit has no default-impl grammar).
#if RUSTYCPP_RUST
pub trait Service {
    fn __reg_to__(&mut self, server: &mut Server, svc_index: usize) -> i32;
    fn __dispatch__(&mut self, rpc_id: i32, req: Box<Request>, sconn: WeakServerConnection);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=server.service version=1 rust_sha256=b614182f7824c551d93fd9d265efc7369734a5e86ebac38004bf7badb39dbb4f*/
class Service;

class Service {
public:
    virtual ~Service() noexcept(false) {}
    virtual int32_t __reg_to__(Server& server, size_t svc_index) = 0;
    virtual void __dispatch__(int32_t rpc_id, rusty::Box<Request> req, WeakServerConnection sconn) = 0;
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    Service(Service&&) = delete;
    Service& operator=(Service&&) = delete;
protected:
    Service() = default;
};

template <class U> class ServiceAdapter;
template <class U> class ServiceAdapterRef;
template <class U> class ServiceAdapterRefMut;
/*RUSTYCPP:GEN-END id=server.service*/

using ServiceProxy = rusty::Box<Service>;

// Pass-through factory for services that already inherit Service.
// @safe - Box move.
#if RUSTYCPP_RUST
fn make_service_proxy_from_box(svc: Box<Service>) -> ServiceProxy {
    svc
}
#endif
/*RUSTYCPP:GEN-BEGIN id=server.5 version=1 rust_sha256=f9d7e881e8cffa3964df38067d765e145057dc24b0bc47c67c80d486b1bc0335*/
ServiceProxy make_service_proxy_from_box(rusty::Box<Service> svc) {
    return std::move(svc);
}
/*RUSTYCPP:GEN-END id=server.5*/

// `ServiceBoxShim<T>` — the generic Box-holding Service implementor
// (generic #[cpp_inherit]; Box gives owning mutable access, so no
// constness dance at all).
#if RUSTYCPP_RUST
struct ServiceBoxShim<T> {
    svc_: Box<T>,
}

#[cpp_inherit]
impl<T> Service for ServiceBoxShim<T> {
    fn __reg_to__(&mut self, server: &mut Server, svc_index: usize) -> i32 {
        self.svc_.__reg_to__(server, svc_index)
    }

    fn __dispatch__(&mut self, rpc_id: i32, req: Box<Request>, sconn: WeakServerConnection) {
        self.svc_.__dispatch__(rpc_id, req, sconn)
    }
}

// @safe - wraps a typed Box<T> in the ServiceBoxShim above; Box move
// only. Merged into this block so the factory sits beside the shim it
// builds. The generic deliberately remains unconstrained: an incompatible
// service produces a diagnostic when this shim instantiates its forwarding
// methods, which is the same check the registration path actually relies on.
fn make_service_proxy_from_typed_box<T>(svc: Box<T>) -> ServiceProxy {
    rusty::make_box::<ServiceBoxShim<T>>(svc)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=server.service_shim version=1 rust_sha256=196690969dff94cb163e04fedd989bb804f097321cbaac347a83fa0438565f3a*/
template<typename T>
struct ServiceBoxShim;

template<typename T>
struct ServiceBoxShim : public Service {
    rusty::Box<T> svc_;
    ServiceBoxShim(rusty::Box<T> svc__init) : Service(), svc_(std::move(svc__init)) {}
    ServiceBoxShim(ServiceBoxShim&& other) noexcept : Service(), svc_(std::move(other.svc_)) {}


    int32_t __reg_to__(Server& server, size_t svc_index) {
        return this->svc_->__reg_to__(server, std::move(svc_index));
    }
    void __dispatch__(int32_t rpc_id, rusty::Box<Request> req, WeakServerConnection sconn) {
        this->svc_->__dispatch__(std::move(rpc_id), std::move(req), std::move(sconn));
    }
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = rusty::is_send<T>::value;
    static constexpr bool is_sync = rusty::is_sync<T>::value;
};

template<typename T>
ServiceProxy make_service_proxy_from_typed_box(rusty::Box<T> svc) {
    return rusty::make_box<ServiceBoxShim<T>>(std::move(svc));
}
/*RUSTYCPP:GEN-END id=server.service_shim*/



// Forward-declared atomic typedefs (full definitions repeated below near
// Server, where the original definitions live). Hoisted here because
// RpcServiceContext lowered to inline-Rust DSL references them in its
// field types. (The DSL does spell rusty::sync::atomic directly.)
// C++ allows redundant identical `using` declarations at namespace scope.
using ServerPendingRequestsAtomic = rusty::sync::atomic::AtomicI32;
using ServerDropHeartbeatRepliesAtomic = rusty::sync::atomic::AtomicBool;

/**
 * Shared context for RPC service dispatch.
 *
 * This struct is shared between Server, ServerListener, and ServerConnection
 * via Arc<RpcServiceContext> to avoid raw pointer dependencies.
 *
 * SAFETY: The struct is constructed once in Server::start() and shared via Arc.
 * All fields are immutable after construction. Services use RefCell for interior
 * mutability, allowing non-const __dispatch__ calls through const Arc access.
 *
 * NOTE: RefCell is single-threaded. All RPC dispatches must occur on the same thread.
 */
// @safe - All fields are const after construction; the factory just moves
// owned containers into place. No syscalls, no raw pointers.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is the
// source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. The previous public ctor was
// replaced with a `fn new` factory; the 6 `Arc<RpcServiceContext>::make`
// callers were flipped to `Arc::new_(RpcServiceContext::new_(...))`.
// The atomic-field types reuse the `ServerPendingRequestsAtomic` and
// `ServerDropHeartbeatRepliesAtomic` typedefs (defined below) so the DSL
// emit names match Server's existing pattern.
#if RUSTYCPP_RUST
struct RpcServiceContext {
    rpc_to_service: HashMap<i32, usize>,
    fast_rpc_ids: HashSet<i32>,
    services: Vec<RefCell<ServiceProxy>>,
    addr: std::string,
    pending_requests: Arc<ServerPendingRequestsAtomic>,
    drop_heartbeat_replies: Arc<ServerDropHeartbeatRepliesAtomic>,
    server_instance_id: u64,
}

impl RpcServiceContext {
    fn new(
        rpc_map: HashMap<i32, usize>,
        fast_rpc_set: HashSet<i32>,
        svcs: Vec<RefCell<ServiceProxy>>,
        address: std::string,
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
#endif
/*RUSTYCPP:GEN-BEGIN id=server.rpc_service_context version=1 rust_sha256=bd8f74e7bb86875f04ef69793e587526f572c45107731574d786dd4948e50da7*/
struct RpcServiceContext;

struct RpcServiceContext {
    rusty::HashMap<int32_t, size_t> rpc_to_service;
    rusty::HashSet<int32_t> fast_rpc_ids;
    rusty::Vec<rusty::RefCell<ServiceProxy>> services;
    std::string addr;
    rusty::Arc<ServerPendingRequestsAtomic> pending_requests;
    rusty::Arc<ServerDropHeartbeatRepliesAtomic> drop_heartbeat_replies;
    uint64_t server_instance_id;

    static RpcServiceContext new_(rusty::HashMap<int32_t, size_t> rpc_map, rusty::HashSet<int32_t> fast_rpc_set, rusty::Vec<rusty::RefCell<ServiceProxy>> svcs, std::string address, rusty::Arc<ServerPendingRequestsAtomic> pending_counter, rusty::Arc<ServerDropHeartbeatRepliesAtomic> drop_heartbeats, uint64_t instance_id);
};


RpcServiceContext RpcServiceContext::new_(rusty::HashMap<int32_t, size_t> rpc_map, rusty::HashSet<int32_t> fast_rpc_set, rusty::Vec<rusty::RefCell<ServiceProxy>> svcs, std::string address, rusty::Arc<ServerPendingRequestsAtomic> pending_counter, rusty::Arc<ServerDropHeartbeatRepliesAtomic> drop_heartbeats, uint64_t instance_id) {
    return RpcServiceContext{.rpc_to_service = std::move(rpc_map), .fast_rpc_ids = std::move(fast_rpc_set), .services = std::move(svcs), .addr = std::move(address), .pending_requests = std::move(pending_counter), .drop_heartbeat_replies = std::move(drop_heartbeats), .server_instance_id = std::move(instance_id)};
}
/*RUSTYCPP:GEN-END id=server.rpc_service_context*/

// 5g1: legacy `ServerListener` class deleted. The channel layer's
// `TcpListener` (registered via `ChannelFactoryProxy::make_listener()`)
// is the sole accept-loop implementation; `Server::start(addr)`
// auto-installs a default TCP factory (5f) when no explicit factory
// is bound.

// @safe - Methods that genuinely cross into unsafe ops (channel proxy
// pointer extraction, raw byte arithmetic in `decode_request_and_dispatch`,
// const_cast-through-Arc in callbacks, rusty::Mutex::lock + ChannelConnectionProxy
// method dispatch) carry their own `// @unsafe` overrides; the rest of the
// class is analyzed as @safe by default. Mirrors the Tier-4 flip on `Server`.
// Uses rusty::Mutex for thread-safe interior mutability, Arc for shared ownership.
// Aliases for the reply / run_async callback types (the DSL parser can't
// take `Function<Sig>` as a generic type argument).
using ServerReplyFn = rusty::Function<void(BinaryWriteArchive&)>;


// Free-fn implementations of the channel-dispatch / Marshal-operator /
// fiber / closure-heavy methods; the DSL methods below delegate to these.
// The private decode/dispatch helpers take raw `const std::uint8_t*`
// (not DSL-emittable as struct methods) and are pure free fns called only
// from the others. All defined in the impl namespace at the bottom of the
// file; each carries its own `// @unsafe` at the definition site.
void sconn_reply(const ServerConnection& self, const Request& req,
                 i32 error_code, ServerReplyFn write_fn);
void sconn_decode_request_and_dispatch(const ServerConnection& self,
                                       const std::uint8_t* bytes, std::size_t size);
void sconn_dispatch_response_frame_via_channel(const ServerConnection& self,
                                               const std::uint8_t* bytes, std::size_t size);

// ServerConnection — one client connection's server-side state. All fields
// are already rusty (Arc / rusty::Mutex / Cell / Weak), so the struct is
// borrow-checked. The reply/dispatch/decode/close/bind bodies (Marshal
// operators, fiber spawns, channel proxy dispatch, closures) live in the
// `sconn_*` free fns the DSL methods delegate to.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block is the source
// of truth; the transpiler regenerates the matching GEN block. The former
// private fields + `friend class Server` are gone — the DSL emits a public
// struct, and Server reaches the (now public) fields directly. The former
// templated `reply<F>` is de-templated to a single `reply(req, code,
// ServerReplyFn)`: Function has SBO, so the `[&]` reply lambdas stay inline
// (no per-reply alloc). The static `rpc_id_missing_s` is hoisted to a
// file-scope global in the impl namespace.
//
// @safe - the delegating methods forward to the `sconn_*` free fns, which
// carry their own `// @unsafe`.
#if RUSTYCPP_RUST
enum ServerConnStatus {
    CONNECTED,
    CLOSED,
}

struct ServerConnection {
    ctx_: Arc<RpcServiceContext>,
    // Cell, matching how Server already holds shutdown_phase_field: an
    // Arc<ServerConnection> is shared, so state changes go through interior
    // mutability rather than callers const_cast-ing to get a &mut.
    status_: Cell<ServerConnStatus>,
    weak_self_: WeakServerConnection,
    channel_proxy_: rusty::Mutex<Option<ChannelConnectionProxy>>,
    channel_mode_: Cell<bool>,
    count: i32,
}

impl ServerConnection {
    #[cpp_ctor] fn new(ctx: Arc<RpcServiceContext>, socket: i32) -> ServerConnection {
        ServerConnection {
            ctx_: ctx,
            status_: Cell::new(ServerConnStatus::CONNECTED),
            weak_self_: Default::default(),
            channel_proxy_: rusty::Mutex::<Option<ChannelConnectionProxy>>::new(None),
            channel_mode_: Cell::new(false),
            count: 0i32,
        }
    }

    fn install_self_weak_for_testing(&mut self, weak: WeakServerConnection) {
        self.weak_self_ = weak;
    }

    fn is_channel_mode(&self) -> bool {
        self.channel_mode_.get()
    }

    fn connected(&self) -> bool {
        self.status_.get() == ServerConnStatus::CONNECTED
    }

    fn is_closed(&self) -> bool {
        self.status_.get() == ServerConnStatus::CLOSED
    }

    fn reply(&self, req: &Request, error_code: i32, write_fn: ServerReplyFn) {
        sconn_reply(self, req, error_code, write_fn)
    }

    fn close(&self) {
        if self.status_.get() == ServerConnStatus::CONNECTED {
            self.status_.set(ServerConnStatus::CLOSED);
            log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("server@{} close ServerConnection", self.ctx_.addr.c_str()));
            // Tear down the channel proxy. Idempotent per channel-layer contract.
            let mut guard = self.channel_proxy_.lock().unwrap();
            if (*guard).is_some() {
                // Annotated binding: Box method dispatch lowers correctly on
                // its own, but the transpiler cannot see the element type
                // THROUGH the Mutex guard, so it emitted `.close()` on the Box
                // instead of `->close()`. Naming the type restores it.
                let proxy: &mut Box<ChannelConnectionBase> = (*guard).as_mut().unwrap();
                proxy.close();
            }
        }
    }

    // Mirrors ClientConnection::bind_channel_direct (client.cpp): weak
    // clones — one per closure — so the callbacks never cycle through
    // `channel_proxy_`; callbacks installed BEFORE the proxy moves into
    // the slot so none of them runs under the rusty::Mutex.
    fn bind_channel(&mut self, mut proxy: ChannelConnectionProxy) {
        if !proxy.is_valid() {
            return;
        }
        let weak_frame: WeakServerConnection = self.weak_self_.clone();
        let weak_closed: WeakServerConnection = self.weak_self_.clone();
        let weak_error: WeakServerConnection = self.weak_self_.clone();
        {
            // Concrete `Box<..>`, not the ChannelConnectionProxy alias:
            // through the alias the pointer-like check fails and the calls
            // lower to `.set_on_frame(..)` (dot) instead of `->` (docs 7.50).
            let ch: &mut Box<ChannelConnectionBase> = &mut proxy;
            ch.set_on_frame(move |f: &ChannelFrame| {
                let sconn_opt = weak_frame.upgrade();
                if sconn_opt.is_none() {
                    return;
                }
                let sconn = sconn_opt.unwrap();
                // Dispatch only READS the connection (status_ via Cell,
                // ctx_ through the Arc), so it takes a const&.
                sconn_decode_request_and_dispatch((*sconn), f.payload, f.size);
            });
            // on_closed runs the existing close path so the connection
            // transitions to CLOSED. The channel-layer contract guarantees
            // on_closed fires exactly once; close() is itself idempotent
            // (status_ == CLOSED short-circuits).
            ch.set_on_closed(move |reason: ChannelError| {
                let sconn_opt = weak_closed.upgrade();
                if sconn_opt.is_none() {
                    return;
                }
                let sconn = sconn_opt.unwrap();
                (*sconn).close();
            });
            // on_error logs and force-closes. Per the channel-layer
            // contract, fatal errors are followed by on_closed, so the
            // close() here is also defensive — close() is idempotent.
            ch.set_on_error(move |err: ChannelError, msg: std::string_view| {
                let sconn_opt = weak_error.upgrade();
                if sconn_opt.is_none() {
                    return;
                }
                let sconn = sconn_opt.unwrap();
                log_line(Log::WARN, 0i32, core::ptr::null(), std::format("rrr::ServerConnection: channel error {}: {}",
                         channel_error_to_string(err), msg));
                (*sconn).close();
            });
        }
        {
            let mut guard = self.channel_proxy_.lock().unwrap();
            *guard = rusty::Some(proxy);
        }
        self.channel_mode_.set(true);
    }

    fn run_async(&self, f: rusty::Function<dyn FnMut()>) -> i32 {
        if !f {
            log_line(Log::WARN, 0i32, core::ptr::null(), std::format("rrr::ServerConnection::run_async called with empty callback"));
            return EINVAL;
        }
        f();
        0i32
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=server.server_connection version=1 rust_sha256=c796671286e5465081483a267c4d4f7fbea70bf80d37ba60cf1a877a0f8353c8*/
enum class ServerConnStatus;
constexpr ServerConnStatus ServerConnStatus_CONNECTED();
constexpr ServerConnStatus ServerConnStatus_CLOSED();
struct ServerConnection;

enum class ServerConnStatus {
    CONNECTED,
    CLOSED
};
inline constexpr ServerConnStatus ServerConnStatus_CONNECTED() { return ServerConnStatus::CONNECTED; }
inline constexpr ServerConnStatus ServerConnStatus_CLOSED() { return ServerConnStatus::CLOSED; }

struct ServerConnection {
    rusty::Arc<RpcServiceContext> ctx_;
    rusty::Cell<ServerConnStatus> status_;
    WeakServerConnection weak_self_;
    rusty::Mutex<rusty::Option<ChannelConnectionProxy>> channel_proxy_;
    rusty::Cell<bool> channel_mode_;
    int32_t count;

    ServerConnection(rusty::Arc<RpcServiceContext> ctx, int32_t socket);
    void install_self_weak_for_testing(WeakServerConnection weak);
    bool is_channel_mode() const;
    bool connected() const;
    bool is_closed() const;
    void reply(const Request& req, int32_t error_code, ServerReplyFn write_fn) const;
    void close() const;
    void bind_channel(ChannelConnectionProxy proxy);
    int32_t run_async(rusty::Function<void()> f) const;
};


ServerConnection::ServerConnection(rusty::Arc<RpcServiceContext> ctx, int32_t socket)
    : ctx_(std::move(ctx))
    , status_(rusty::Cell<ServerConnStatus>::new_(rusty::clone(rusty::clone(ServerConnStatus_CONNECTED()))))
    , weak_self_(rusty::default_like<WeakServerConnection>())
    , channel_proxy_(rusty::Mutex<rusty::Option<ChannelConnectionProxy>>::new_(rusty::Option<ChannelConnectionProxy>{rusty::None}))
    , channel_mode_(rusty::Cell<bool>::new_(false))
    , count(static_cast<int32_t>(0))
{}

void ServerConnection::install_self_weak_for_testing(WeakServerConnection weak) {
    this->weak_self_ = std::move(weak);
}

bool ServerConnection::is_channel_mode() const {
    return this->channel_mode_.get();
}

bool ServerConnection::connected() const {
    return this->status_.get() == rusty::clone(ServerConnStatus_CONNECTED());
}

bool ServerConnection::is_closed() const {
    return this->status_.get() == rusty::clone(ServerConnStatus_CLOSED());
}

void ServerConnection::reply(const Request& req, int32_t error_code, ServerReplyFn write_fn) const {
    sconn_reply((*this), req, std::move(error_code), std::move(write_fn));
}

void ServerConnection::close() const {
    if (this->status_.get() == rusty::clone(ServerConnStatus_CONNECTED())) {
        this->status_.set(rusty::clone(rusty::clone(ServerConnStatus_CLOSED())));
        log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("server@{} close ServerConnection", (*this->ctx_).addr.c_str()));
        auto guard = this->channel_proxy_.lock().unwrap();
        if (((*guard)).is_some()) {
            rusty::Box<ChannelConnectionBase>& proxy = ((*guard)).as_mut().unwrap();
            proxy->close();
        }
    }
}

void ServerConnection::bind_channel(ChannelConnectionProxy proxy) {
    if (rusty::detail::rust_not(proxy.is_valid())) {
        return;
    }
    WeakServerConnection weak_frame = rusty::clone(this->weak_self_);
    WeakServerConnection weak_closed = rusty::clone(this->weak_self_);
    WeakServerConnection weak_error = rusty::clone(this->weak_self_);
    {
        rusty::Box<ChannelConnectionBase>& ch = proxy;
        ch->set_on_frame([=, weak_frame = std::move(weak_frame)](const ChannelFrame& f) {
auto sconn_opt = weak_frame.upgrade();
if (sconn_opt.is_none()) {
    return;
}
const auto sconn = sconn_opt.unwrap();
sconn_decode_request_and_dispatch((rusty::detail::deref_if_pointer_like(sconn)), f.payload, f.size);
});
        ch->set_on_closed([=, weak_closed = std::move(weak_closed)](ChannelError reason) {
auto sconn_opt = weak_closed.upgrade();
if (sconn_opt.is_none()) {
    return;
}
const auto sconn = sconn_opt.unwrap();
((rusty::detail::deref_if_pointer_like(sconn))).close();
});
        ch->set_on_error([=, weak_error = std::move(weak_error)](ChannelError err, std::string_view msg) {
auto sconn_opt = weak_error.upgrade();
if (sconn_opt.is_none()) {
    return;
}
const auto sconn = sconn_opt.unwrap();
log_line(rusty::clone(rusty::clone(Log::WARN)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("rrr::ServerConnection: channel error {}: {}", channel_error_to_string(std::move(err)), std::move(msg)));
((rusty::detail::deref_if_pointer_like(sconn))).close();
});
    }
    {
        auto guard = this->channel_proxy_.lock().unwrap();
        *guard = rusty::Option<ChannelConnectionProxy>(std::move(proxy));
    }
    this->channel_mode_.set(true);
}

int32_t ServerConnection::run_async(rusty::Function<void()> f) const {
    if (rusty::detail::rust_not(f)) {
        log_line(rusty::clone(rusty::clone(Log::WARN)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("rrr::ServerConnection::run_async called with empty callback"));
        return EINVAL;
    }
    f();
    return static_cast<int32_t>(0);
}
/*RUSTYCPP:GEN-END id=server.server_connection*/

}  // export namespace rrr

// @safe - DeferredReply (RAII wrapper for deferred RPC replies) and
// Server (which owns the channel listener + accepted ServerConnection
// Arcs). Both classes carry their own descriptive `// @safe` blocks
// with per-method `// @unsafe` overrides on the socket
// / rusty::Mutex-extraction paths.
export namespace rrr {

// @safe - RAII wrapper for deferred RPC replies with move semantics

// Forward declarations so the DSL-emitted `DeferredReply::reply()` /
// `reply_error()` method bodies (which sit inside the GEN block
// further down) can name the dispatch helpers — the helpers' actual
// definitions live in the impl namespace at the bottom of the file
// alongside the rest of the `*_impl` escape hatches.
class DeferredReply;

// `DeferredReply` — once-fire async-reply handle handed to user
// service handlers. Holds the inbound request, a weak handle on
// the server connection, the write-side archive callback the
// rcc_rpc-generated dispatcher built around the typed reply
// struct, and a cleanup callback for any heap state the wrapper
// allocated for the request.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block below.
//
// Authentic-Rust shape (vs the pre-DSL C++ form):
//   * Callbacks are `Option<Box<dyn FnOnce(...) + Send>>`, not a
//     bare `Function<...>` paired with a separate `replied_: bool`
//     flag. The `Option::take()` consumes-on-call pattern encodes
//     "fire at most once" in the type, so the bool flag is gone.
//     `reply()` / `reply_error()` early-return cleanly when the
//     callback is `None`.
//   * `impl Drop` consumes the cleanup callback via `take()` and
//     invokes it once. Matches the C++ `~DeferredReply() { if
//     (cleanup_) cleanup_(); }` shape, just expressed in Rust
//     idiom and emitted as a generated destructor.
//   * `run_async` no longer returns `EINVAL` on an empty callback
//     — `Box<dyn FnOnce>` can't be empty by type. The one test
//     that exercised that defensive branch is dropped.
//
// Out-of-line C++ helpers (`deferred_reply_*_impl`) handle the
// `WeakServerConnection::upgrade()` + `sconn->reply(...)` plumbing
// plus `Log_warn` / `Log_debug` calls; the C++ logging macros
// don't translate through the DSL, and the helper lets us keep
// the dispatch shape identical to the pre-migration code.
#if RUSTYCPP_RUST
struct DeferredReply {
    req_field: Box<Request>,
    weak_sconn_field: WeakServerConnection,
    archive_reply_field: Option<Box<dyn FnOnce(&mut BinaryWriteArchive) + Send>>,
    cleanup_field: Option<Box<dyn FnOnce() + Send>>,
}

impl DeferredReply {
    fn new(req: Box<Request>,
           weak_sconn: WeakServerConnection,
           archive_reply: Box<dyn FnOnce(&mut BinaryWriteArchive) + Send>,
           cleanup: Box<dyn FnOnce() + Send>) -> DeferredReply {
        DeferredReply {
            req_field: req,
            weak_sconn_field: weak_sconn,
            archive_reply_field: Some(archive_reply),
            cleanup_field: Some(cleanup),
        }
    }

    fn run_async(&mut self, f: Box<dyn FnOnce() + Send>) -> i32 {
        f();
        0i32
    }

    fn reply(&mut self) {
        let cb_opt = self.archive_reply_field.take();
        if cb_opt.is_none() {
            unsafe { log_line(Log::WARN, 0i32, core::ptr::null(), std::format("DeferredReply::reply() called multiple times, ignoring")) };
            return;
        }
        // `mut` is redundant in Rust (a non-mut binding can still be moved
        // out of) and rustc will warn about it. It is here because the
        // transpiler only drops `const` for locals it can prove are
        // consumed, and it cannot follow this one: the move target is a
        // method whose receiver type comes from
        // `weak_sconn_field.upgrade().unwrap()`. Without `mut` it emits
        // `const auto cb` and then `std::move(cb)`, which selects
        // rusty::Function's deleted copy ctor. TODO: teach the consumed
        // detector this receiver chain, then drop the `mut`.
        let mut cb = cb_opt.unwrap();
        let sconn_opt = self.weak_sconn_field.upgrade();
        if sconn_opt.is_some() {
            let sconn = sconn_opt.unwrap();
            (*sconn).reply(&*self.req_field, 0, cb);
        } else {
            unsafe { log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("Connection closed before reply sent, dropping reply")) };
        }
    }

    fn reply_error(&mut self, error_code: i32) {
        if self.archive_reply_field.take().is_none() {
            unsafe { log_line(Log::WARN, 0i32, core::ptr::null(), std::format("DeferredReply::reply_error() called multiple times, ignoring")) };
            return;
        }
        let sconn_opt = self.weak_sconn_field.upgrade();
        if sconn_opt.is_some() {
            let sconn = sconn_opt.unwrap();
            let mut no_writer: ServerReplyFn = Default::default();
            (*sconn).reply(&*self.req_field, error_code, no_writer);
        } else {
            unsafe { log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("Connection closed before error reply sent, dropping reply")) };
        }
    }
}

impl Drop for DeferredReply {
    fn drop(&mut self) {
        if let Some(cleanup) = self.cleanup_field.take() {
            cleanup();
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=deferred_reply.0 version=1 rust_sha256=0d7b1dd4ec445d2703e9e53ac0e578f13d2936e40781c415ec6a98b7af080862*/
struct DeferredReply;

struct DeferredReply {
    rusty::Box<Request> req_field;
    WeakServerConnection weak_sconn_field;
    rusty::Option<rusty::Function<void(BinaryWriteArchive&)>> archive_reply_field;
    rusty::Option<rusty::Function<void()>> cleanup_field;
    mutable bool _rusty_forgotten = false;
    DeferredReply(rusty::Box<Request> req_field_init, WeakServerConnection weak_sconn_field_init, rusty::Option<rusty::Function<void(BinaryWriteArchive&)>> archive_reply_field_init, rusty::Option<rusty::Function<void()>> cleanup_field_init) : req_field(std::move(req_field_init)), weak_sconn_field(std::move(weak_sconn_field_init)), archive_reply_field(std::move(archive_reply_field_init)), cleanup_field(std::move(cleanup_field_init)) {}
    DeferredReply(const DeferredReply&) = delete;
    DeferredReply(DeferredReply&& other) noexcept : req_field(std::move(other.req_field)), weak_sconn_field(std::move(other.weak_sconn_field)), archive_reply_field(std::move(other.archive_reply_field)), cleanup_field(std::move(other.cleanup_field)) {
        this->_rusty_forgotten = other._rusty_forgotten;
        other._rusty_forgotten = true;
    }
    DeferredReply& operator=(const DeferredReply&) = delete;
    DeferredReply& operator=(DeferredReply&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        this->~DeferredReply();
        new (this) DeferredReply(std::move(other));
        return *this;
    }
    void rusty_mark_forgotten() const noexcept { _rusty_forgotten = true; rusty::detail::mark_forgotten_if_supported(this->req_field); rusty::detail::mark_forgotten_if_supported(this->weak_sconn_field); rusty::detail::mark_forgotten_if_supported(this->archive_reply_field); rusty::detail::mark_forgotten_if_supported(this->cleanup_field); }


    static DeferredReply new_(rusty::Box<Request> req, WeakServerConnection weak_sconn, rusty::Function<void(BinaryWriteArchive&)> archive_reply, rusty::Function<void()> cleanup);
    int32_t run_async(rusty::Function<void()> f);
    void reply();
    void reply_error(int32_t error_code);
    ~DeferredReply() noexcept(false);
};


DeferredReply DeferredReply::new_(rusty::Box<Request> req, WeakServerConnection weak_sconn, rusty::Function<void(BinaryWriteArchive&)> archive_reply, rusty::Function<void()> cleanup) {
    return DeferredReply(std::move(req), std::move(weak_sconn), rusty::Option<rusty::Function<void(BinaryWriteArchive&)>>(std::move(archive_reply)), rusty::Option<rusty::Function<void()>>(std::move(cleanup)));
}

int32_t DeferredReply::run_async(rusty::Function<void()> f) {
    f();
    return static_cast<int32_t>(0);
}

void DeferredReply::reply() {
    auto cb_opt = this->archive_reply_field.take();
    if (cb_opt.is_none()) {
        // @unsafe
        {
            log_line(rusty::clone(rusty::clone(Log::WARN)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("DeferredReply::reply() called multiple times, ignoring"));
        }
        return;
    }
    auto cb = cb_opt.unwrap();
    auto sconn_opt = this->weak_sconn_field.upgrade();
    if (sconn_opt.is_some()) {
        const auto sconn = sconn_opt.unwrap();
        ((rusty::detail::deref_if_pointer_like(sconn))).reply(rusty::detail::deref_if_pointer_like(this->req_field), 0, std::move(cb));
    } else {
        // @unsafe
        {
            log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("Connection closed before reply sent, dropping reply"));
        }
    }
}

void DeferredReply::reply_error(int32_t error_code) {
    if (this->archive_reply_field.take().is_none()) {
        // @unsafe
        {
            log_line(rusty::clone(rusty::clone(Log::WARN)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("DeferredReply::reply_error() called multiple times, ignoring"));
        }
        return;
    }
    auto sconn_opt = this->weak_sconn_field.upgrade();
    if (sconn_opt.is_some()) {
        const auto sconn = sconn_opt.unwrap();
        ServerReplyFn no_writer = rusty::default_like<ServerReplyFn>();
        ((rusty::detail::deref_if_pointer_like(sconn))).reply(rusty::detail::deref_if_pointer_like(this->req_field), std::move(error_code), std::move(no_writer));
    } else {
        // @unsafe
        {
            log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("Connection closed before error reply sent, dropping reply"));
        }
    }
}

DeferredReply::~DeferredReply() noexcept(false) {
    if (_rusty_forgotten) { return; }
    if (auto&& _iflet_scrutinee = this->cleanup_field.take(); _iflet_scrutinee.is_some()) {
        decltype(auto) cleanup = _iflet_scrutinee.unwrap();
        cleanup();
    }
}
/*RUSTYCPP:GEN-END id=deferred_reply.0*/


// Default drain / graceful-shutdown timeout. Was the inline `= 30000`
// default arg on `Server::drain()` and `Server::graceful_shutdown()`
// before the DSL prep dropped those defaults; preserved here so call
// sites that previously relied on the implicit value can keep the same
// behaviour by passing it explicitly.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block with the C++ constexpr.
#if RUSTYCPP_RUST
const kDefaultDrainTimeoutMs: u64 = 30000u64;
#endif
/*RUSTYCPP:GEN-BEGIN id=server.const_drain_timeout version=1 rust_sha256=33e39f14c5b597708d9bf6b9c5a742d0bb58df09e9976f26e91613b8b6430b9c*/
constexpr uint64_t kDefaultDrainTimeoutMs = static_cast<uint64_t>(30000);
/*RUSTYCPP:GEN-END id=server.const_drain_timeout*/

// Shutdown coordination state — guarded by `Server::shutdown_state_field`
// (a `rusty::Mutex<ShutdownState>`). Defined as its own DSL struct
// (not nested inside Server, since the DSL transpiler does not parse
// nested-struct declarations inside an `impl` block). The DSL emits
// a C++20 aggregate with a public `shutdown` field; the existing
// `ShutdownState{}` value-init site in `Server::new_()` keeps working
// because `bool` value-inits to `false`.
#if RUSTYCPP_RUST
struct ShutdownState {
    shutdown: bool,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=server.shutdown_state version=1 rust_sha256=7a8622d12ba6546419c9fe006ee93220a163e214052642fc0dafa2cdd71daf2a*/
struct ShutdownState;

struct ShutdownState {
    bool shutdown;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};
/*RUSTYCPP:GEN-END id=server.shutdown_state*/


// @unsafe - strlen over the reinterpret_cast'ed addr. Returns an owned
// std::string for the DSL `start()` body (`*const i8` is how the DSL
// spells the incoming `const char*`, and std::string wants char*).
//
// `c_char` is the module-local `using c_char = char` alias declared
// above `export namespace rrr`: the DSL's own `char` is Rust's 4-byte
// scalar and lowers to `char32_t`, and `core::ffi::c_char` lowers to a
// `rusty::ffi::c_char` the rusty headers do not define.
#if RUSTYCPP_RUST
fn server_dsl_addr_to_string(addr: *const i8) -> std::string {
    let p: *const c_char = addr as *const c_char;
    std::string(p, strlen(p))
}
#endif
/*RUSTYCPP:GEN-BEGIN id=server.12 version=1 rust_sha256=0dfe87d649c435d725b844c43f0c996c4c5ab101404c1cc42b833d60aed893b4*/
std::string server_dsl_addr_to_string(const int8_t* addr);

std::string server_dsl_addr_to_string(const int8_t* addr) {
    const c_char* p = reinterpret_cast<const c_char*>(addr);
    return std::string(p, strlen(p));
}
/*RUSTYCPP:GEN-END id=server.12*/

// Forward declaration of Server to allow helper signatures to refer
// to it. The DSL emits the full definition below.
class Server;

// Type aliases for the atomic counters carried inside the DSL Server
// struct. Defined outside the DSL block so the inline-Rust grammar
// keeps one name for the counter type (the DSL grammar does
// not yet recognize that template).
using ServerPendingRequestsAtomic = rusty::sync::atomic::AtomicI32;
using ServerDropHeartbeatRepliesAtomic = rusty::sync::atomic::AtomicBool;

// Helper free functions that the DSL `Server` method bodies delegate
// to. Defined out-of-line in plain C++ because the DSL grammar can't
// express things like `std::random_device` / `std::chrono` / catch
// blocks / iteration with the rusty::Mutex<T>'s lambda predicate.
//
// Each helper has the same logic the legacy Server out-of-line method
// had — moved verbatim so the DSL stays a thin shim.

// Pick the PollThread to use (auto-create one if caller didn't supply
// one). Used by the ctor.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
#if RUSTYCPP_RUST
fn server_resolve_poll_thread(
        poll_thread_worker: rusty::Option<rusty::Arc<PollThread>>)
        -> rusty::Option<rusty::Arc<PollThread>> {
    if poll_thread_worker.is_none() {
        return rusty::Some(PollThread::create());
    }
    poll_thread_worker
}
#endif
/*RUSTYCPP:GEN-BEGIN id=server.resolve_poll_thread version=1 rust_sha256=979240d50170fdeae7be5e0be14f77e42ff023ce0241cd5650d26cc67dc296c4*/
rusty::Option<rusty::Arc<PollThread>> server_resolve_poll_thread(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker) {
    if (poll_thread_worker.is_none()) {
        return rusty::Option<rusty::Arc<PollThread>>(PollThread::create());
    }
    return std::move(poll_thread_worker);
}
/*RUSTYCPP:GEN-END id=server.resolve_poll_thread*/

// @unsafe - one micro-kernel is left here: constructing a
// std::random_device and drawing from it. Everything built ON it (the
// mix, the shift) is DSL below. `std::random_device()` lowers to a
// prvalue initializer, which C++17 guaranteed elision turns back into
// the plain default-construct the hand-written kernel spelled.
#if RUSTYCPP_RUST
fn server_random_u64() -> u64 {
    let mut rd: std::random_device = std::random_device();
    ((rd() as u64) << 32) | (rd() as u64)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=server.15 version=1 rust_sha256=e743d2464b29684bd5574ec278822f321658b1da3c6372809f6c9f767a87a62c*/
uint64_t server_random_u64();

uint64_t server_random_u64() {
    std::random_device rd = std::random_device();
    return ((((static_cast<uint64_t>(rd()))) << 32)) | ((static_cast<uint64_t>(rd())));
}
/*RUSTYCPP:GEN-END id=server.15*/

#if RUSTYCPP_RUST
// @safe - rrr's own clock (Time::now microseconds, scaled to the nano
// range the id-mix historically used; entropy comes from the random_u64
// mix, not clock granularity). std::chrono is gone.
fn server_now_nanos() -> u64 {
    (Time::now(true) as u64) * 1000u64
}

// Block until do_shutdown() flips the flag. The old note here claimed
// "the DSL grammar can't express the wait-while lambda binding"; that is
// wrong -- a closure predicate over the guard lowers to exactly the
// `[&](ShutdownState& s) { return rust_not(s.shutdown); }` the hand
// written kernel spelled, including the std::move onto the guard.
fn server_wait_for_shutdown_impl(state: &rusty::Mutex<ShutdownState>,
                                 cond: &rusty::Box<rusty::Condvar>) {
    log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("Server::wait_for_shutdown"));
    let mut guard = state.lock().unwrap();
    guard = cond.wait_while(guard, |s: &mut ShutdownState| { !s.shutdown }).unwrap();
    log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("Server::wait_for_shutdown - done"));
}

fn server_generate_instance_id() -> u64 {
    let time_component: u64 = server_now_nanos();
    let random_component: u64 = server_random_u64();
    let pid_component: u64 = (rusty::sys::process::getpid() as u64) << 48;
    // 0x7fff_ffff_ffff_ffff is std::numeric_limits<int64_t>::max(): the id
    // is kept non-negative because it crosses the wire as a signed i64.
    let mut id: u64 = (time_component ^ random_component ^ pid_component)
        & 0x7fffffffffffffff;
    if id == 0 {
        id = 1;
    }
    log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("Server: generated instance_id={}", id));
    id
}
#endif
/*RUSTYCPP:GEN-BEGIN id=server.instance_id version=1 rust_sha256=2630e3dade75e8bc023f1108dec11bc38c1138701ec98619286e912661ff163c*/
uint64_t server_now_nanos();
uint64_t server_generate_instance_id();

uint64_t server_now_nanos() {
    return ((static_cast<uint64_t>(Time::now(true)))) * static_cast<uint64_t>(1000);
}

void server_wait_for_shutdown_impl(const rusty::Mutex<ShutdownState>& state, const rusty::Box<rusty::Condvar>& cond) {
    log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("Server::wait_for_shutdown"));
    auto guard = state.lock().unwrap();
    guard = cond->wait_while(std::move(guard), [&](ShutdownState& s) {
return rusty::detail::rust_not(s.shutdown);
}).unwrap();
    log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("Server::wait_for_shutdown - done"));
}

uint64_t server_generate_instance_id() {
    const uint64_t time_component = server_now_nanos();
    const uint64_t random_component = server_random_u64();
    const uint64_t pid_component = ((static_cast<uint64_t>(rusty::sys::process::getpid()))) << 48;
    uint64_t id = (((rusty::detail::deref_if_pointer_like(time_component) ^ rusty::detail::deref_if_pointer_like(random_component)) ^ rusty::detail::deref_if_pointer_like(pid_component))) & static_cast<uint64_t>(9223372036854775807);
    if (rusty::detail::deref_if_pointer_like(id) == static_cast<uint64_t>(0)) {
        id = static_cast<uint64_t>(1);
    }
    log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("Server: generated instance_id={}", std::move(id)));
    return std::move(id);
}
/*RUSTYCPP:GEN-END id=server.instance_id*/







// Drain phase-FSM + timed busy-wait, authored as inline Rust DSL. The
// atomic loads are expressed directly now — the server_atomic_* wrapper
// kernels are gone, since `a.load(Ordering::Relaxed)` on an `&Arc<Atomic>`
// lowers to exactly `a->load(...)`, which is all the wrappers did.
//
// The already-in-phase debug line used to omit the phase name, on the
// grounds that "the DSL cannot drive the *_to_string varargs safely".
// That cause expired: Log_* is now a std::format variadic TEMPLATE
// (std::format_string), not C varargs, so the std::string_view that
// shutdown_phase_to_string returns is type-checked and formats fine.
// The name is restored — without it you cannot tell WHICH phase drain
// bailed out on. See playbook §7.26.
#if RUSTYCPP_RUST
fn server_drain_impl(phase: &rusty::Cell<ShutdownPhase>,
                     pending: &rusty::Arc<ServerPendingRequestsAtomic>,
                     timeout_ms: u64) -> bool {
    let current_phase = phase.get();
    if current_phase != ShutdownPhase::RUNNING
        && current_phase != ShutdownPhase::STOP_ACCEPTING {
        log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("Server::drain: already past the draining phases ({})",
                  shutdown_phase_to_string(current_phase)));
        return pending.load(rusty::sync::atomic::Ordering::Relaxed) == 0;
    }
    log_line(Log::INFO, 0i32, core::ptr::null(), std::format("Server::drain: transitioning to DRAINING, pending={}",
             pending.load(rusty::sync::atomic::Ordering::Relaxed)));
    phase.set(ShutdownPhase::DRAINING);
    let start_us = rusty::sys::time::clock_monotonic_us();
    let timeout_us = timeout_ms * 1000;
    while pending.load(rusty::sync::atomic::Ordering::Relaxed) > 0 {
        let elapsed_us = rusty::sys::time::clock_monotonic_us() - start_us;
        if elapsed_us >= timeout_us {
            log_line(Log::WARN, 0i32, core::ptr::null(), std::format("Server::drain: timeout after {} ms, pending={}",
                     timeout_ms, pending.load(rusty::sync::atomic::Ordering::Relaxed)));
            return false;
        }
        rusty::sys::time::sleep_us(1000);
    }
    log_line(Log::INFO, 0i32, core::ptr::null(), std::format("Server::drain: completed, all requests drained"));
    true
}
#endif
/*RUSTYCPP:GEN-BEGIN id=server.drain_impl version=1 rust_sha256=3950048bc0100400dc4662418d26c28a22bcc9936a52860259424b848edacf66*/
bool server_drain_impl(const rusty::Cell<ShutdownPhase>& phase, const rusty::Arc<ServerPendingRequestsAtomic>& pending, uint64_t timeout_ms) {
    const auto current_phase = phase.get();
    if ((rusty::detail::deref_if_pointer_like(current_phase) != rusty::clone(ShutdownPhase_RUNNING())) && (rusty::detail::deref_if_pointer_like(current_phase) != rusty::clone(ShutdownPhase_STOP_ACCEPTING()))) {
        log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("Server::drain: already past the draining phases ({})", shutdown_phase_to_string(std::move(current_phase))));
        return pending->load(rusty::sync::atomic::Ordering::Relaxed) == 0;
    }
    log_line(rusty::clone(rusty::clone(Log::INFO)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("Server::drain: transitioning to DRAINING, pending={}", pending->load(rusty::sync::atomic::Ordering::Relaxed)));
    phase.set(rusty::clone(rusty::clone(ShutdownPhase_DRAINING())));
    const auto start_us = rusty::sys::time::clock_monotonic_us();
    const auto timeout_us = rusty::detail::deref_if_pointer_like(timeout_ms) * 1000;
    while (pending->load(rusty::sync::atomic::Ordering::Relaxed) > 0) {
        const auto elapsed_us = rusty::sys::time::clock_monotonic_us() - rusty::detail::deref_if_pointer_like(start_us);
        if (rusty::detail::deref_if_pointer_like(elapsed_us) >= rusty::detail::deref_if_pointer_like(timeout_us)) {
            log_line(rusty::clone(rusty::clone(Log::WARN)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("Server::drain: timeout after {} ms, pending={}", std::move(timeout_ms), pending->load(rusty::sync::atomic::Ordering::Relaxed)));
            return false;
        }
        rusty::sys::time::sleep_us(1000);
    }
    log_line(rusty::clone(rusty::clone(Log::INFO)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("Server::drain: completed, all requests drained"));
    return true;
}
/*RUSTYCPP:GEN-END id=server.drain_impl*/

// Forward declaration only: the DSL definition further down emits no
// declaration of its own, and `server_run_shutdown_hooks` (next) calls it.
void server_invoke_shutdown_hook_safely(ShutdownHook& hook);

// NOTE: hooks run WHILE the mutex is held. That is the pre-existing
// behaviour and is preserved deliberately — request_queue.cpp collects
// callbacks under the lock and invokes them outside it, but changing the
// order here would alter shutdown semantics, not just style.
#if RUSTYCPP_RUST
fn server_run_shutdown_hooks(hooks: &rusty::Mutex<Vec<ShutdownHook>>) {
    unsafe { log_line(Log::INFO, 0i32, core::ptr::null(), std::format("Server::graceful_shutdown: transitioning to CLOSING, executing hooks")); }
    let mut guard = hooks.lock().unwrap();
    for hook in &mut (*guard) {
        server_invoke_shutdown_hook_safely(&mut hook);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=server.14 version=1 rust_sha256=08dd52a27dda1fcb7aa71e16c2c31e9b2af0ad6c57faa4f240f348a9fa3a0ae2*/
void server_run_shutdown_hooks(const rusty::Mutex<rusty::Vec<ShutdownHook>>& hooks) {
    // @unsafe
    {
        log_line(rusty::clone(rusty::clone(Log::INFO)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("Server::graceful_shutdown: transitioning to CLOSING, executing hooks"));
    }
    auto guard = hooks.lock().unwrap();
    for (auto&& hook : rusty::for_in(rusty::iter_mut((*guard)))) {
        server_invoke_shutdown_hook_safely(hook);
    }
}
/*RUSTYCPP:GEN-END id=server.14*/

// strtoll's `char**` out-parameter has no direct DSL spelling: `*mut i8`
// lowers to `int8_t*`, which `deref_if_pointer_like` would DEREFERENCE in
// the `end == start` comparison (only plain `char` pointers are its
// str-carrier special case). One alias fixes both.
using ServerParseEndPtr = char*;

// @unsafe - strtoll over a raw `char**` endptr (inside the DSL body).
//
// No try/catch left. strtoll reports "no conversion" through the endptr and
// saturates out-of-range input at LLONG_MIN/LLONG_MAX, both of which the
// int32 range test rejects — so this is std::stoi's language exactly
// (invalid_argument -> None, out_of_range -> None) with the throw removed
// rather than caught. Returning Option keeps the failure signal distinct
// from a legitimately parsed value (the pre-Option code folded both into
// -1, so a literal "-5" was indistinguishable from a throw). Parity-tested
// against std::stoi over 29 inputs — whitespace, signs, partial parses,
// INT32 boundaries, ERANGE saturation: 0 mismatches.
#if RUSTYCPP_RUST
fn server_parse_port(text: &std::string) -> Option<i32> {
    let start = text.c_str();
    let mut end: ServerParseEndPtr = core::ptr::null_mut();
    let v: i64 = unsafe { strtoll(start, &mut end, 10) };
    if end == start {
        return None;
    }
    if v < -2147483648i64 || v > 2147483647i64 {
        return None;
    }
    Some(v as i32)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=server.17 version=1 rust_sha256=9414a791df7ff391d0c7b5a8afc6cbe47ca075d863ee52e441032409754806b3*/
rusty::Option<int32_t> server_parse_port(const std::string& text);

rusty::Option<int32_t> server_parse_port(const std::string& text) {
    const auto start = text.c_str();
    ServerParseEndPtr end = rusty::ptr::null_mut();
    const int64_t v = strtoll(std::move(start), &end, 10);
    if (rusty::detail::deref_if_pointer_like(end) == rusty::detail::deref_if_pointer_like(start)) {
        return rusty::Option<int32_t>{rusty::None};
    }
    if ((rusty::detail::deref_if_pointer_like(v) < static_cast<int64_t>(-2147483648)) || (rusty::detail::deref_if_pointer_like(v) > static_cast<int64_t>(2147483647))) {
        return rusty::Option<int32_t>{rusty::None};
    }
    return rusty::Option<int32_t>(static_cast<int32_t>(v));
}
/*RUSTYCPP:GEN-END id=server.17*/

// @unsafe - the one irreducible step: recovering an exception's message
// needs a rethrow plus a typed catch. `None` is the old `catch (...)` arm —
// a payload that is not a std::exception, and so has no what().
inline rusty::Option<std::string> server_exception_message(std::exception_ptr p) {
    try { std::rethrow_exception(p); }
    catch (const std::exception& e) { return rusty::Some(std::string(e.what())); }
    catch (...) { return rusty::None; }
}

// The invoker is DSL now: `std::panic::catch_unwind` lowers to
// `rusty::panic::catch_unwind`, whose `catch (...)` + exception_ptr payload
// IS the old two-arm try/catch. Both messages survive verbatim — the
// std::exception arm still logs `e.what()`, which is what tells a throwing
// hook apart from one that failed opaquely. Runtime-checked against the old
// kernel: identical messages, nothing escapes, a later hook still runs.
#if RUSTYCPP_RUST
fn server_invoke_shutdown_hook_safely(hook: &mut ShutdownHook) {
    let r = std::panic::catch_unwind(|| { hook(); });
    if r.is_ok() {
        return;
    }
    let msg = server_exception_message(r.unwrap_err());
    if msg.is_some() {
        unsafe { log_line(Log::ERROR, 0i32, core::ptr::null(), std::format("Server::graceful_shutdown: hook threw exception: {}", msg.unwrap())); }
    } else {
        unsafe { log_line(Log::ERROR, 0i32, core::ptr::null(), std::format("Server::graceful_shutdown: hook threw unknown exception")); }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=server.20 version=1 rust_sha256=1ca0deb5c1447760fb7d71240c04a7e7f1af854c462aff0a96498cb50df08042*/
void server_invoke_shutdown_hook_safely(ShutdownHook& hook) {
    auto r = rusty::panic::catch_unwind([&]() {
hook();
});
    if (r.is_ok()) {
        return;
    }
    auto msg = server_exception_message(r.unwrap_err());
    if (msg.is_some()) {
        // @unsafe
        {
            log_line(rusty::clone(rusty::clone(Log::ERROR)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("Server::graceful_shutdown: hook threw exception: {}", msg.unwrap()));
        }
    } else {
        // @unsafe
        {
            log_line(rusty::clone(rusty::clone(Log::ERROR)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("Server::graceful_shutdown: hook threw unknown exception"));
        }
    }
}
/*RUSTYCPP:GEN-END id=server.20*/


// `Server` — RPC server facade. Authored as inline Rust DSL: the
// `#if RUSTYCPP_RUST` block below is the source of truth; the
// transpiler regenerates the matching `/*RUSTYCPP:GEN-BEGIN ... END*/`
// block. Drop trait emits a real destructor that runs the channel-
// listener / accepted-connection teardown.
//
// Behavioral diffs from the previous C++ class:
//   * Fields are no longer marked `public`/`private` separately;
//     the DSL emits all as public. The trailing `_` on each field is
//     replaced with `_field` because the transpiler considers
//     `pending_services_` to collide with the would-be
//     `pending_services()` accessor; same convention as other
//     migrated classes (Client, CircuitBreaker, etc.).
//   * The user-declared move ctor/move-assign are emitted by the DSL
//     itself (they respect the Drop-protocol `_rusty_forgotten`
//     flag). Copy stays implicitly disabled by the `NoCopy` base.
//   * `~Server()` is no longer `virtual override` — the DSL emits a
//     non-virtual destructor that runs the close logic. Server has
//     no derived classes; the `virtual` was inherited from `NoCopy`'s
//     `virtual ~NoCopy() = default;` and is harmless to drop.
//   * Out-of-line method bodies (`start`, `unreg`, `do_shutdown`,
//     `wait_for_shutdown`, `add_shutdown_hook`, `stop_accepting`,
//     `drain`, `graceful_shutdown`, `get_bound_port`) are translated
//     into the DSL block.
//   * `for_each_service<F>` and `reg_service_typed<T>` are DSL member
//     templates — a DSL `fn f<T>(..)` emits a real `template<typename T>`
//     method, so neither stays hand-written.
//
// @safe - Methods that genuinely cross into unsafe ops (socket I/O via the
// channel-layer's TcpListener, Pthread primitives, raw
// pointer extraction from ChannelListenerProxy, etc.) carry their own
// `// @unsafe` overrides; the rest of the class is now analyzed as @safe
// by default. Mirrors the Tier-4 flip on `Client`.
#if RUSTYCPP_RUST
// Accepted-connection registry shared between the Server and its
// listener's on_accept closure. `closed` flips in ~Server; a late
// accept observing it closes the connection instead of pushing.
struct ChannelSconns {
    closed: bool,
    conns: Vec<Arc<ServerConnection>>,
}

struct Server {
    pending_services_field: Vec<ServiceProxy>,
    pending_rpc_to_service_field: HashMap<i32, usize>,
    pending_fast_rpc_ids_field: HashSet<i32>,
    ctx_field: Option<Arc<RpcServiceContext>>,
    poll_thread_field: Option<Arc<PollThread>>,
    shutdown_state_field: Mutex<ShutdownState>,
    shutdown_cond_field: Box<Condvar>,
    shutdown_phase_field: Cell<ShutdownPhase>,
    shutdown_hooks_field: rusty::Mutex<Vec<ShutdownHook>>,
    pending_requests_field: Arc<ServerPendingRequestsAtomic>,
    drop_heartbeat_replies_field: Arc<ServerDropHeartbeatRepliesAtomic>,
    instance_id_field: u64,
    channel_factory_field: Option<ChannelFactoryProxy>,
    channel_listener_field: Option<ChannelListenerProxy>,
    // Arc-shared: the listener's on_accept closure runs on the poll
    // thread and can fire after ~Server on another thread (listener
    // close is an in-order poll-thread job, not a fence). Sharing the
    // state keeps a late accept writing into live memory (ASan-caught
    // UAF: rpc_client_pool_test), and the `closed` marker makes it
    // CORRECT too: an accept that loses the race against ~Server must
    // close the connection instead of parking it in an orphaned vector
    // nobody will ever close (the client then stays "connected" forever
    // — the state_integration >5s !connected() stall).
    channel_sconns_field: Arc<rusty::Mutex<ChannelSconns>>,
}

impl Drop for Server {
    // 5e/5f teardown. The channel-mode listener close is scheduled on
    // the poll thread via a OneTimeJob so commands are processed in
    // order (mirrors `Client::close`'s 4g3c3 fix); dropping the Box
    // inside the job then releases the backend listener, closing its
    // fd. Accepted channel connections are closed eagerly so peers see
    // EOF immediately; close() is idempotent everywhere here. Services
    // outlive this via each ServerConnection's Arc<RpcServiceContext>.
    fn drop(&mut self) {
        if self.channel_listener_field.is_some() {
            let listener_opt: Option<ChannelListenerProxy> =
                core::mem::take(&mut self.channel_listener_field);
            let mut listener_box: Box<ChannelListenerBase> = listener_opt.unwrap();
            let close_job: Arc<OneTimeJob> =
                Arc::<OneTimeJob>::new_(OneTimeJob::new_(move || {
                    listener_box.close();
                }));
            // Implicit Arc<OneTimeJob> -> Arc<Job> upcast via rusty::Arc's
            // template ctor (U* convertible to T*).
            let pt: &Arc<PollThread> = self.poll_thread_field.as_ref().unwrap();
            pt.add(close_job);
        }
        {
            let mut guard = self.channel_sconns_field.lock().unwrap();
            (*guard).closed = true;
            let mut i: usize = 0usize;
            while i < (*guard).conns.len() {
                (*(*guard).conns[i]).close();
                i += 1usize;
            }
            (*guard).conns.clear();
        }
        self.ctx_field = None;
    }
}

impl Server {
    fn new(poll_thread_worker: Option<Arc<PollThread>>) -> Server {
        Server {
            pending_services_field: Vec::<ServiceProxy>(),
            pending_rpc_to_service_field: HashMap::<i32, usize>(),
            pending_fast_rpc_ids_field: HashSet::<i32>(),
            ctx_field: None,
            poll_thread_field: server_resolve_poll_thread(poll_thread_worker),
            shutdown_state_field: Mutex::<ShutdownState>(ShutdownState {}),
            shutdown_cond_field: Box::new(Condvar {}),
            shutdown_phase_field: Cell::<ShutdownPhase>::new(ShutdownPhase::RUNNING),
            shutdown_hooks_field: rusty::Mutex::<Vec<ShutdownHook>>::new(Vec::<ShutdownHook>()),
            pending_requests_field: Arc::<ServerPendingRequestsAtomic>::make(0i32),
            drop_heartbeat_replies_field: Arc::<ServerDropHeartbeatRepliesAtomic>::make(false),
            instance_id_field: server_generate_instance_id(),
            channel_factory_field: None,
            channel_listener_field: None,
            channel_sconns_field: Arc::<rusty::Mutex<ChannelSconns>>::new_(
                rusty::Mutex::<ChannelSconns>::new(ChannelSconns {
                    closed: false,
                    conns: Vec::<Arc<ServerConnection>>(),
                })),
        }
    }

    fn set_channel_factory(&mut self, factory: ChannelFactoryProxy) {
        if !factory.is_valid() {
            return;
        }
        self.channel_factory_field = Some(factory);
    }

    fn is_channel_factory_bound(&self) -> bool {
        self.channel_factory_field.is_some()
    }

    fn reg_service(&mut self, svc: Box<Service>) {
        self.pending_services_field.push(make_service_proxy_from_box(svc));
        let svc_index: usize = self.pending_services_field.size() - 1usize;
        (*self.pending_services_field[svc_index]).__reg_to__(self, svc_index);
    }

    fn reg_service_proxy(&mut self, proxy: ServiceProxy) {
        self.pending_services_field.push(proxy);
        let svc_index: usize = self.pending_services_field.size() - 1usize;
        (*self.pending_services_field[svc_index]).__reg_to__(self, svc_index);
    }

    fn reg_rpc(&mut self, rpc_id: i32, svc_index: usize) -> i32 {
        if self.pending_rpc_to_service_field.contains_key(rpc_id) {
            return EEXIST;
        }
        self.pending_rpc_to_service_field.insert(rpc_id, svc_index);
        0i32
    }

    fn reg_fast_rpc(&mut self, rpc_id: i32, svc_index: usize) -> i32 {
        let ret: i32 = self.reg_rpc(rpc_id, svc_index);
        if ret != 0i32 {
            return ret;
        }
        self.pending_fast_rpc_ids_field.insert(rpc_id);
        0i32
    }

    fn unreg(&mut self, rpc_id: i32) {
        self.pending_rpc_to_service_field.remove(rpc_id);
        self.pending_fast_rpc_ids_field.remove(rpc_id);
    }

    fn do_shutdown(&self) {
        let guard = self.shutdown_state_field.lock().unwrap();
        (*guard).shutdown = true;
        self.shutdown_cond_field.notify_all();
    }

    fn wait_for_shutdown(&self) {
        server_wait_for_shutdown_impl(self.shutdown_state_field, self.shutdown_cond_field);
    }

    fn add_shutdown_hook(&self, hook: ShutdownHook) {
        let guard = self.shutdown_hooks_field.lock().unwrap();
        (*guard).push(hook);
    }

    fn stop_accepting(&mut self) {
        if (self.shutdown_phase_field.get() as i32) != (ShutdownPhase::RUNNING as i32) {
            return;
        }
        self.shutdown_phase_field.set(ShutdownPhase::STOP_ACCEPTING);
        if self.channel_listener_field.is_some() {
            // Named Box type so the call lowers to `->close()` (playbook §7.13).
            let listener: &mut Box<ChannelListenerBase> =
                self.channel_listener_field.as_mut().unwrap();
            listener.close();
        }
    }

    fn drain(&self, timeout_ms: u64) -> bool {
        server_drain_impl(self.shutdown_phase_field, self.pending_requests_field, timeout_ms)
    }

    fn graceful_shutdown(&mut self, drain_timeout_ms: u64) {
        self.stop_accepting();
        self.drain(drain_timeout_ms);
        self.shutdown_phase_field.set(ShutdownPhase::CLOSING);
        server_run_shutdown_hooks(self.shutdown_hooks_field);
        self.do_shutdown();
        self.shutdown_phase_field.set(ShutdownPhase::STOPPED);
    }

    fn phase(&self) -> ShutdownPhase {
        self.shutdown_phase_field.get()
    }

    fn pending_request_count(&self) -> i32 {
        self.pending_requests_field.load(rusty::sync::atomic::Ordering::Relaxed)
    }

    fn increment_pending(&self) {
        self.pending_requests_field.fetch_add(1i32, rusty::sync::atomic::Ordering::Relaxed);
    }

    fn decrement_pending(&self) {
        self.pending_requests_field.fetch_sub(1i32, rusty::sync::atomic::Ordering::Relaxed);
    }

    fn set_drop_heartbeat_replies(&self, drop: bool) {
        self.drop_heartbeat_replies_field.store(drop, rusty::sync::atomic::Ordering::Release);
    }

    fn drop_heartbeat_replies(&self) -> bool {
        self.drop_heartbeat_replies_field.load(rusty::sync::atomic::Ordering::Acquire)
    }

    fn instance_id(&self) -> u64 {
        self.instance_id_field
    }

    fn service_count(&self) -> usize {
        if self.ctx_field.is_some() {
            return self.ctx_field.as_ref().unwrap().services.size();
        }
        self.pending_services_field.size()
    }

    fn addr(&self) -> std::string {
        self.ctx_field.as_ref().unwrap().addr
    }

    // Freeze the pending registrations into an immutable
    // RpcServiceContext, auto-install a TcpFactory when none is bound,
    // make + wire the channel listener, and bind. The accept callback
    // uses the get_mut mint window twice on the freshly made
    // ServerConnection Arc (weak wiring, then bind_channel) — the same
    // two windows the old kernel used — and reaches the server's
    // connection table through a captured raw pointer (the Server
    // outlives its listener; the callback dies with the listener).
    fn start(&mut self, bind_addr: *const i8) -> i32 {
        if bind_addr.is_null() {
            log_line(Log::ERROR, 0i32, core::ptr::null(), std::format("rrr::Server::start: bind_addr is NULL!"));
            return -1;
        }
        let addr_str: std::string = server_dsl_addr_to_string(bind_addr);

        // Wrap each service in RefCell for interior mutability. The
        // pending Vec is taken whole (ServiceProxy is a Box — no
        // per-element default to take against) and drained in order.
        let mut pending: Vec<ServiceProxy> =
            core::mem::take(&mut self.pending_services_field);
        let mut wrapped_services: Vec<RefCell<ServiceProxy>> = Vec::new();
        for svc in pending.drain(..) {
            wrapped_services.push(RefCell::<ServiceProxy>::new(svc));
        }

        // Create immutable RpcServiceContext from pending registration
        // data.
        self.ctx_field = Some(Arc::<RpcServiceContext>::new_(RpcServiceContext::new(
            core::mem::take(&mut self.pending_rpc_to_service_field),
            core::mem::take(&mut self.pending_fast_rpc_ids_field),
            wrapped_services,
            addr_str.clone(),
            self.pending_requests_field.clone(),
            self.drop_heartbeat_replies_field.clone(),
            self.instance_id_field,
        )));

        // Auto-install a default TcpFactory if the caller hasn't bound
        // one.
        if !self.is_channel_factory_bound() {
            let tcp_factory: Arc<TcpFactory> = Arc::<TcpFactory>::new_(
                TcpFactory::new(self.poll_thread_field.as_ref().unwrap().clone()));
            self.set_channel_factory(make_tcp_factory_proxy(tcp_factory));
        }

        if self.is_channel_factory_bound() {
            let listener_opt = {
                let factory: &mut Box<ChannelFactoryBase> =
                    self.channel_factory_field.as_mut().unwrap();
                factory.make_listener()
            };
            if listener_opt.is_none() {
                log_line(Log::ERROR, 0i32, core::ptr::null(), std::format("rrr::Server::start: factory->make_listener() returned a null proxy (factory backend={})",
                          "unknown"));
                self.ctx_field = None;
                return -1;
            }
            let mut listener: ChannelListenerProxy = listener_opt.unwrap();

            let sconns_arc: Arc<rusty::Mutex<ChannelSconns>> =
                self.channel_sconns_field.clone();
            let ctx_arc: Arc<RpcServiceContext> =
                self.ctx_field.as_ref().unwrap().clone();

            {
                let ch: &mut Box<ChannelListenerBase> = &mut listener;
                ch.set_on_accept(move |conn_proxy: ChannelConnectionProxy| {
                    if !conn_proxy.is_valid() {
                        return;
                    }
                    let mut sconn: Arc<ServerConnection> =
                        Arc::<ServerConnection>::make(ctx_arc.clone(), -1i32);
                    // get_mut, not const_cast: the Arc was just made and
                    // is still uniquely owned — exactly when Arc::get_mut
                    // yields a &mut.
                    {
                        let opt = sconn.get_mut();
                        let mut_sconn: &mut ServerConnection = opt.unwrap();
                        mut_sconn.install_self_weak_for_testing(
                            rusty::sync::downgrade(sconn));
                    }
                    {
                        let opt2 = sconn.get_mut();
                        let mut_sconn2: &mut ServerConnection = opt2.unwrap();
                        mut_sconn2.bind_channel(conn_proxy);
                    }
                    {
                        let mut guard = sconns_arc.lock().unwrap();
                        if (*guard).closed {
                            // This accept lost the race against ~Server:
                            // close the connection now or nobody ever will
                            // (an orphaned-but-open sconn kept the peer
                            // "connected" indefinitely — the >5s
                            // !connected() stall in state_integration).
                            (*sconn).close();
                            return;
                        }
                        (*guard).conns.push(sconn);
                    }
                });
                ch.set_on_error(move |err: ChannelError, msg: std::string_view| {
                    log_line(Log::WARN, 0i32, core::ptr::null(), std::format("rrr::Server: channel listener error {}: {}",
                             channel_error_to_string(err), msg));
                });
            }

            let listen_err = {
                let ch2: &mut Box<ChannelListenerBase> = &mut listener;
                ch2.listen(addr_str.clone())
            };
            if listen_err != ChannelError::None {
                log_line(Log::ERROR, 0i32, core::ptr::null(), std::format("rrr::Server::start: channel listener failed to bind {}: {}",
                          addr_str, channel_error_to_string(listen_err)));
                self.ctx_field = None;
                return -1;
            }

            self.channel_listener_field = Some(listener);
            return 0;
        }

        verify(false);
        -1
    }

    fn get_bound_port(&self) -> i32 {
        if self.channel_listener_field.is_none() {
            return -1;
        }
        // Named Box type so local_address() lowers to `->` (playbook §7.13).
        let listener: &Box<ChannelListenerBase> =
            self.channel_listener_field.as_ref().unwrap();
        let local: std::string = listener.local_address();
        // find_last_of, not rfind: `rfind` matches Rust's str::rfind
        // signature, so the transpiler maps it to rusty::str_runtime::rfind
        // — a namespace that only exists in whole-file cppm boilerplate, not
        // in the headers the DSL path sees (playbook §7.14). find_last_of has
        // no Rust counterpart, so it lowers as a plain member call.
        let colon = local.find_last_of(":");
        if colon == std::string::npos {
            log_line(Log::ERROR, 0i32, core::ptr::null(), std::format("Server::get_bound_port: malformed local_address {}", local.c_str()));
            return -1;
        }
        let parsed = server_parse_port(local.substr(colon + 1));
        if parsed.is_none() {
            log_line(Log::ERROR, 0i32, core::ptr::null(), std::format("Server::get_bound_port: failed to parse port from {}", local.c_str()));
            return -1;
        }
        parsed.unwrap()
    }

    fn reg_service_typed<T>(&mut self, svc: Box<T>) {
        self.pending_services_field.push(make_service_proxy_from_typed_box::<T>(svc));
        let svc_index: usize = self.pending_services_field.size() - 1usize;
        (*self.pending_services_field[svc_index]).__reg_to__(self, svc_index);
    }

    fn for_each_service<F>(&self, callback: F) {
        let n: usize = self.ctx_field.as_ref().unwrap().services.size();
        let mut i: usize = 0usize;
        while i < n {
            let mut guard = self.ctx_field.as_ref().unwrap().services[i].borrow_mut();
            let svc: &mut Service = &mut **guard;
            callback(*svc);
            i += 1usize;
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=server.1 version=1 rust_sha256=8cf2e1a10acd208fb69628a9e9d5de6df40b8d67891d3ec42059e6d5e6e67f14*/
struct ChannelSconns;
struct Server;

struct ChannelSconns {
    bool closed;
    rusty::Vec<rusty::Arc<ServerConnection>> conns;
};

struct Server {
    rusty::Vec<ServiceProxy> pending_services_field;
    rusty::HashMap<int32_t, size_t> pending_rpc_to_service_field;
    rusty::HashSet<int32_t> pending_fast_rpc_ids_field;
    rusty::Option<rusty::Arc<RpcServiceContext>> ctx_field;
    rusty::Option<rusty::Arc<PollThread>> poll_thread_field;
    rusty::Mutex<ShutdownState> shutdown_state_field;
    rusty::Box<rusty::Condvar> shutdown_cond_field;
    rusty::Cell<ShutdownPhase> shutdown_phase_field;
    rusty::Mutex<rusty::Vec<ShutdownHook>> shutdown_hooks_field;
    rusty::Arc<ServerPendingRequestsAtomic> pending_requests_field;
    rusty::Arc<ServerDropHeartbeatRepliesAtomic> drop_heartbeat_replies_field;
    uint64_t instance_id_field;
    rusty::Option<ChannelFactoryProxy> channel_factory_field;
    rusty::Option<ChannelListenerProxy> channel_listener_field;
    rusty::Arc<rusty::Mutex<ChannelSconns>> channel_sconns_field;
    mutable bool _rusty_forgotten = false;
    Server(rusty::Vec<ServiceProxy> pending_services_field_init, rusty::HashMap<int32_t, size_t> pending_rpc_to_service_field_init, rusty::HashSet<int32_t> pending_fast_rpc_ids_field_init, rusty::Option<rusty::Arc<RpcServiceContext>> ctx_field_init, rusty::Option<rusty::Arc<PollThread>> poll_thread_field_init, rusty::Mutex<ShutdownState> shutdown_state_field_init, rusty::Box<rusty::Condvar> shutdown_cond_field_init, rusty::Cell<ShutdownPhase> shutdown_phase_field_init, rusty::Mutex<rusty::Vec<ShutdownHook>> shutdown_hooks_field_init, rusty::Arc<ServerPendingRequestsAtomic> pending_requests_field_init, rusty::Arc<ServerDropHeartbeatRepliesAtomic> drop_heartbeat_replies_field_init, uint64_t instance_id_field_init, rusty::Option<ChannelFactoryProxy> channel_factory_field_init, rusty::Option<ChannelListenerProxy> channel_listener_field_init, rusty::Arc<rusty::Mutex<ChannelSconns>> channel_sconns_field_init) : pending_services_field(std::move(pending_services_field_init)), pending_rpc_to_service_field(std::move(pending_rpc_to_service_field_init)), pending_fast_rpc_ids_field(std::move(pending_fast_rpc_ids_field_init)), ctx_field(std::move(ctx_field_init)), poll_thread_field(std::move(poll_thread_field_init)), shutdown_state_field(std::move(shutdown_state_field_init)), shutdown_cond_field(std::move(shutdown_cond_field_init)), shutdown_phase_field(std::move(shutdown_phase_field_init)), shutdown_hooks_field(std::move(shutdown_hooks_field_init)), pending_requests_field(std::move(pending_requests_field_init)), drop_heartbeat_replies_field(std::move(drop_heartbeat_replies_field_init)), instance_id_field(std::move(instance_id_field_init)), channel_factory_field(std::move(channel_factory_field_init)), channel_listener_field(std::move(channel_listener_field_init)), channel_sconns_field(std::move(channel_sconns_field_init)) {}
    Server(const Server&) = delete;
    Server(Server&& other) noexcept : pending_services_field(std::move(other.pending_services_field)), pending_rpc_to_service_field(std::move(other.pending_rpc_to_service_field)), pending_fast_rpc_ids_field(std::move(other.pending_fast_rpc_ids_field)), ctx_field(std::move(other.ctx_field)), poll_thread_field(std::move(other.poll_thread_field)), shutdown_state_field(std::move(other.shutdown_state_field)), shutdown_cond_field(std::move(other.shutdown_cond_field)), shutdown_phase_field(std::move(other.shutdown_phase_field)), shutdown_hooks_field(std::move(other.shutdown_hooks_field)), pending_requests_field(std::move(other.pending_requests_field)), drop_heartbeat_replies_field(std::move(other.drop_heartbeat_replies_field)), instance_id_field(std::move(other.instance_id_field)), channel_factory_field(std::move(other.channel_factory_field)), channel_listener_field(std::move(other.channel_listener_field)), channel_sconns_field(std::move(other.channel_sconns_field)) {
        this->_rusty_forgotten = other._rusty_forgotten;
        other._rusty_forgotten = true;
    }
    Server& operator=(const Server&) = delete;
    Server& operator=(Server&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        this->~Server();
        new (this) Server(std::move(other));
        return *this;
    }
    void rusty_mark_forgotten() const noexcept { _rusty_forgotten = true; rusty::detail::mark_forgotten_if_supported(this->pending_services_field); rusty::detail::mark_forgotten_if_supported(this->pending_rpc_to_service_field); rusty::detail::mark_forgotten_if_supported(this->pending_fast_rpc_ids_field); rusty::detail::mark_forgotten_if_supported(this->ctx_field); rusty::detail::mark_forgotten_if_supported(this->poll_thread_field); rusty::detail::mark_forgotten_if_supported(this->shutdown_state_field); rusty::detail::mark_forgotten_if_supported(this->shutdown_cond_field); rusty::detail::mark_forgotten_if_supported(this->shutdown_phase_field); rusty::detail::mark_forgotten_if_supported(this->shutdown_hooks_field); rusty::detail::mark_forgotten_if_supported(this->pending_requests_field); rusty::detail::mark_forgotten_if_supported(this->drop_heartbeat_replies_field); rusty::detail::mark_forgotten_if_supported(this->instance_id_field); rusty::detail::mark_forgotten_if_supported(this->channel_factory_field); rusty::detail::mark_forgotten_if_supported(this->channel_listener_field); rusty::detail::mark_forgotten_if_supported(this->channel_sconns_field); }


    ~Server() noexcept(false);
    static Server new_(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker);
    void set_channel_factory(ChannelFactoryProxy factory);
    bool is_channel_factory_bound() const;
    void reg_service(rusty::Box<Service> svc);
    void reg_service_proxy(ServiceProxy proxy);
    int32_t reg_rpc(int32_t rpc_id, size_t svc_index);
    int32_t reg_fast_rpc(int32_t rpc_id, size_t svc_index);
    void unreg(int32_t rpc_id);
    void do_shutdown() const;
    void wait_for_shutdown() const;
    void add_shutdown_hook(ShutdownHook hook) const;
    void stop_accepting();
    bool drain(uint64_t timeout_ms) const;
    void graceful_shutdown(uint64_t drain_timeout_ms);
    ShutdownPhase phase() const;
    int32_t pending_request_count() const;
    void increment_pending() const;
    void decrement_pending() const;
    void set_drop_heartbeat_replies(bool drop) const;
    bool drop_heartbeat_replies() const;
    uint64_t instance_id() const;
    size_t service_count() const;
    std::string addr() const;
    int32_t start(const int8_t* bind_addr);
    int32_t get_bound_port() const;
    template<typename T>
    void reg_service_typed(rusty::Box<T> svc);
    template<typename F>
    void for_each_service(F callback) const;
};


Server::~Server() noexcept(false) {
    if (_rusty_forgotten) { return; }
    if (this->channel_listener_field.is_some()) {
        rusty::Option<ChannelListenerProxy> listener_opt = rusty::mem::take(this->channel_listener_field);
        rusty::Box<ChannelListenerBase> listener_box = listener_opt.unwrap();
        const rusty::Arc<OneTimeJob> close_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob::new_([=, listener_box = std::move(listener_box)]() mutable {
listener_box->close();
}));
        const rusty::Arc<PollThread>& pt = this->poll_thread_field.as_ref().unwrap();
        pt->add(std::move(close_job));
    }
    {
        auto guard = this->channel_sconns_field->lock().unwrap();
        (rusty::detail::deref_if_pointer_like(guard)).closed = true;
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::len((rusty::detail::deref_if_pointer_like(guard)).conns)) {
            ((rusty::detail::deref_if_pointer_like((rusty::detail::deref_if_pointer_like(guard)).conns[i]))).close();
            i += static_cast<size_t>(1);
        }
        (rusty::detail::deref_if_pointer_like(guard)).conns.clear();
    }
    this->ctx_field = rusty::Option<rusty::Arc<RpcServiceContext>>{rusty::None};
}

Server Server::new_(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker) {
    return Server(rusty::Vec<ServiceProxy>(), rusty::HashMap<int32_t, size_t>(), rusty::HashSet<int32_t>(), rusty::Option<rusty::Arc<RpcServiceContext>>{rusty::None}, server_resolve_poll_thread(std::move(poll_thread_worker)), rusty::Mutex<ShutdownState>(ShutdownState{}), rusty::Box<rusty::Condvar>::new_(rusty::Condvar{}), rusty::Cell<ShutdownPhase>::new_(rusty::clone(rusty::clone(ShutdownPhase_RUNNING()))), rusty::Mutex<rusty::Vec<ShutdownHook>>::new_(rusty::Vec<ShutdownHook>()), rusty::Arc<ServerPendingRequestsAtomic>::make(static_cast<int32_t>(0)), rusty::Arc<ServerDropHeartbeatRepliesAtomic>::make(false), server_generate_instance_id(), rusty::Option<ChannelFactoryProxy>{rusty::None}, rusty::Option<ChannelListenerProxy>{rusty::None}, rusty::Arc<rusty::Mutex<ChannelSconns>>::new_(rusty::Mutex<ChannelSconns>::new_(ChannelSconns{.closed = false, .conns = rusty::Vec<rusty::Arc<ServerConnection>>()})));
}

void Server::set_channel_factory(ChannelFactoryProxy factory) {
    if (rusty::detail::rust_not(factory.is_valid())) {
        return;
    }
    this->channel_factory_field = rusty::Option<ChannelFactoryProxy>(std::move(factory));
}

bool Server::is_channel_factory_bound() const {
    return this->channel_factory_field.is_some();
}

void Server::reg_service(rusty::Box<Service> svc) {
    this->pending_services_field.push(make_service_proxy_from_box(std::move(svc)));
    const size_t svc_index = this->pending_services_field.size() - static_cast<size_t>(1);
    ((rusty::detail::deref_if_pointer_like(this->pending_services_field[svc_index]))).__reg_to__((*this), std::move(svc_index));
}

void Server::reg_service_proxy(ServiceProxy proxy) {
    this->pending_services_field.push(std::move(proxy));
    const size_t svc_index = this->pending_services_field.size() - static_cast<size_t>(1);
    ((rusty::detail::deref_if_pointer_like(this->pending_services_field[svc_index]))).__reg_to__((*this), std::move(svc_index));
}

int32_t Server::reg_rpc(int32_t rpc_id, size_t svc_index) {
    if (this->pending_rpc_to_service_field.contains_key(std::move(rpc_id))) {
        return EEXIST;
    }
    this->pending_rpc_to_service_field.insert(std::move(rpc_id), std::move(svc_index));
    return static_cast<int32_t>(0);
}

int32_t Server::reg_fast_rpc(int32_t rpc_id, size_t svc_index) {
    int32_t ret = this->reg_rpc(std::move(rpc_id), std::move(svc_index));
    if (rusty::detail::deref_if_pointer_like(ret) != static_cast<int32_t>(0)) {
        return std::move(ret);
    }
    this->pending_fast_rpc_ids_field.insert(std::move(rpc_id));
    return static_cast<int32_t>(0);
}

void Server::unreg(int32_t rpc_id) {
    this->pending_rpc_to_service_field.remove(std::move(rpc_id));
    this->pending_fast_rpc_ids_field.remove(std::move(rpc_id));
}

void Server::do_shutdown() const {
    auto guard = this->shutdown_state_field.lock().unwrap();
    (*guard).shutdown = true;
    this->shutdown_cond_field->notify_all();
}

void Server::wait_for_shutdown() const {
    server_wait_for_shutdown_impl(this->shutdown_state_field, this->shutdown_cond_field);
}

void Server::add_shutdown_hook(ShutdownHook hook) const {
    auto guard = this->shutdown_hooks_field.lock().unwrap();
    ((*guard)).push(std::move(hook));
}

void Server::stop_accepting() {
    if (((static_cast<int32_t>(this->shutdown_phase_field.get()))) != ((static_cast<int32_t>(ShutdownPhase_RUNNING())))) {
        return;
    }
    this->shutdown_phase_field.set(rusty::clone(rusty::clone(ShutdownPhase_STOP_ACCEPTING())));
    if (this->channel_listener_field.is_some()) {
        rusty::Box<ChannelListenerBase>& listener = this->channel_listener_field.as_mut().unwrap();
        listener->close();
    }
}

bool Server::drain(uint64_t timeout_ms) const {
    return server_drain_impl(this->shutdown_phase_field, this->pending_requests_field, std::move(timeout_ms));
}

void Server::graceful_shutdown(uint64_t drain_timeout_ms) {
    this->stop_accepting();
    this->drain(std::move(drain_timeout_ms));
    this->shutdown_phase_field.set(rusty::clone(rusty::clone(ShutdownPhase_CLOSING())));
    server_run_shutdown_hooks(this->shutdown_hooks_field);
    this->do_shutdown();
    this->shutdown_phase_field.set(rusty::clone(rusty::clone(ShutdownPhase_STOPPED())));
}

ShutdownPhase Server::phase() const {
    return this->shutdown_phase_field.get();
}

int32_t Server::pending_request_count() const {
    return this->pending_requests_field->load(rusty::sync::atomic::Ordering::Relaxed);
}

void Server::increment_pending() const {
    this->pending_requests_field->fetch_add(static_cast<int32_t>(1), rusty::sync::atomic::Ordering::Relaxed);
}

void Server::decrement_pending() const {
    this->pending_requests_field->fetch_sub(static_cast<int32_t>(1), rusty::sync::atomic::Ordering::Relaxed);
}

void Server::set_drop_heartbeat_replies(bool drop) const {
    this->drop_heartbeat_replies_field->store(std::move(drop), rusty::sync::atomic::Ordering::Release);
}

bool Server::drop_heartbeat_replies() const {
    return this->drop_heartbeat_replies_field->load(rusty::sync::atomic::Ordering::Acquire);
}

uint64_t Server::instance_id() const {
    return this->instance_id_field;
}

size_t Server::service_count() const {
    if (this->ctx_field.is_some()) {
        return (*this->ctx_field.as_ref().unwrap()).services.size();
    }
    return this->pending_services_field.size();
}

std::string Server::addr() const {
    return (*this->ctx_field.as_ref().unwrap()).addr;
}

int32_t Server::start(const int8_t* bind_addr) {
    if ((bind_addr == nullptr)) {
        log_line(rusty::clone(rusty::clone(Log::ERROR)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("rrr::Server::start: bind_addr is NULL!"));
        return -1;
    }
    const std::string addr_str = server_dsl_addr_to_string(bind_addr);
    rusty::Vec<ServiceProxy> pending = rusty::mem::take(this->pending_services_field);
    rusty::Vec<rusty::RefCell<ServiceProxy>> wrapped_services = rusty::Vec<rusty::RefCell<ServiceProxy>>::new_();
    for (auto&& svc : rusty::for_in(pending.drain(rusty::range_full()))) {
        wrapped_services.push(rusty::RefCell<ServiceProxy>::new_(std::move(svc)));
    }
    this->ctx_field = rusty::Option<rusty::Arc<RpcServiceContext>>(rusty::Arc<RpcServiceContext>::new_(RpcServiceContext::new_(rusty::mem::take(this->pending_rpc_to_service_field), rusty::mem::take(this->pending_fast_rpc_ids_field), std::move(wrapped_services), rusty::clone(addr_str), rusty::clone(this->pending_requests_field), rusty::clone(this->drop_heartbeat_replies_field), this->instance_id_field)));
    if (!this->is_channel_factory_bound()) {
        const rusty::Arc<TcpFactory> tcp_factory = rusty::Arc<TcpFactory>::new_(TcpFactory::new_(rusty::clone(this->poll_thread_field.as_ref().unwrap())));
        this->set_channel_factory(make_tcp_factory_proxy(std::move(tcp_factory)));
    }
    if (this->is_channel_factory_bound()) {
        auto listener_opt = [&]() { rusty::Box<ChannelFactoryBase>& factory = this->channel_factory_field.as_mut().unwrap();
return factory->make_listener(); }();
        if (listener_opt.is_none()) {
            log_line(rusty::clone(rusty::clone(Log::ERROR)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("rrr::Server::start: factory->make_listener() returned a null proxy (factory backend={})", "unknown"));
            this->ctx_field = rusty::Option<rusty::Arc<RpcServiceContext>>{rusty::None};
            return -1;
        }
        ChannelListenerProxy listener = listener_opt.unwrap();
        rusty::Arc<rusty::Mutex<ChannelSconns>> sconns_arc = rusty::clone(this->channel_sconns_field);
        rusty::Arc<RpcServiceContext> ctx_arc = rusty::clone(this->ctx_field.as_ref().unwrap());
        {
            rusty::Box<ChannelListenerBase>& ch = listener;
            ch->set_on_accept([=, ctx_arc = std::move(ctx_arc), sconns_arc = std::move(sconns_arc)](ChannelConnectionProxy conn_proxy) {
if (rusty::detail::rust_not(conn_proxy.is_valid())) {
    return;
}
rusty::Arc<ServerConnection> sconn = rusty::Arc<ServerConnection>::make(rusty::clone(ctx_arc), static_cast<int32_t>(-1));
{
    auto opt = sconn.get_mut();
    ServerConnection& mut_sconn = opt.unwrap();
    mut_sconn.install_self_weak_for_testing(rusty::sync::downgrade(std::move(sconn)));
}
{
    auto opt2 = sconn.get_mut();
    ServerConnection& mut_sconn2 = opt2.unwrap();
    mut_sconn2.bind_channel(std::move(conn_proxy));
}
{
    auto guard = sconns_arc->lock().unwrap();
    if ((rusty::detail::deref_if_pointer_like(guard)).closed) {
        ((rusty::detail::deref_if_pointer_like(sconn))).close();
        return;
    }
    (rusty::detail::deref_if_pointer_like(guard)).conns.push(std::move(sconn));
}
});
            ch->set_on_error([=](ChannelError err, std::string_view msg) {
log_line(rusty::clone(rusty::clone(Log::WARN)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("rrr::Server: channel listener error {}: {}", channel_error_to_string(std::move(err)), std::move(msg)));
});
        }
        const auto listen_err = [&]() { rusty::Box<ChannelListenerBase>& ch2 = listener;
return ch2->listen(rusty::clone(addr_str)); }();
        if (rusty::detail::deref_if_pointer_like(listen_err) != rusty::detail::deref_if_pointer_like(ChannelError::None)) {
            log_line(rusty::clone(rusty::clone(Log::ERROR)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("rrr::Server::start: channel listener failed to bind {}: {}", std::move(addr_str), channel_error_to_string(std::move(listen_err))));
            this->ctx_field = rusty::Option<rusty::Arc<RpcServiceContext>>{rusty::None};
            return -1;
        }
        this->channel_listener_field = rusty::Option<ChannelListenerProxy>(std::move(listener));
        return static_cast<int32_t>(0);
    }
    verify(false);
    return -1;
}

int32_t Server::get_bound_port() const {
    if (this->channel_listener_field.is_none()) {
        return -1;
    }
    const rusty::Box<ChannelListenerBase>& listener = this->channel_listener_field.as_ref().unwrap();
    const std::string local = listener->local_address();
    const auto colon = local.find_last_of(":");
    if (rusty::detail::deref_if_pointer_like(colon) == rusty::detail::deref_if_pointer_like(std::string::npos)) {
        log_line(rusty::clone(rusty::clone(Log::ERROR)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("Server::get_bound_port: malformed local_address {}", local.c_str()));
        return -1;
    }
    auto parsed = server_parse_port(local.substr(rusty::detail::deref_if_pointer_like(colon) + 1));
    if (parsed.is_none()) {
        log_line(rusty::clone(rusty::clone(Log::ERROR)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("Server::get_bound_port: failed to parse port from {}", local.c_str()));
        return -1;
    }
    return parsed.unwrap();
}

template<typename T>
void Server::reg_service_typed(rusty::Box<T> svc) {
    this->pending_services_field.push(make_service_proxy_from_typed_box<T>(std::move(svc)));
    const size_t svc_index = this->pending_services_field.size() - static_cast<size_t>(1);
    ((rusty::detail::deref_if_pointer_like(this->pending_services_field[svc_index]))).__reg_to__((*this), std::move(svc_index));
}

template<typename F>
void Server::for_each_service(F callback) const {
    const size_t n = (*this->ctx_field.as_ref().unwrap()).services.size();
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
        auto&& guard = (*this->ctx_field.as_ref().unwrap()).services[i].borrow_mut();
        Service& svc = rusty::detail::deref_if_pointer_like(rusty::detail::deref_if_pointer_like(guard));
        callback(svc);
        i += static_cast<size_t>(1);
    }
}
/*RUSTYCPP:GEN-END id=server.1*/



}  // export namespace rrr

// @safe - Implementation namespace. Out-of-class definitions inherit
// their per-method `// @unsafe` annotations from the matching
// declarations above. Free-function impl details carry their own
// `// @unsafe` markers individually.
namespace rrr {



// Build the reply body (header + user payload) into a BufferSink and
// dispatch through the bound channel proxy. (Was the templated
// `reply<F>`; de-templated to a `ServerReplyFn` — Function SBO keeps
// the `[&]` reply lambdas inline, no per-reply alloc.) An empty
// `write_fn` (the former 2-arg empty-reply overload) writes just the
// header. Authored as inline Rust DSL: both archive types are
// single-field aggregates, so the struct literals here are the same
// shapes the serde impls in serializable.cpp already use.
#if RUSTYCPP_RUST
fn sconn_reply(sconn: &ServerConnection, req: &Request,
               error_code: i32, write_fn: ServerReplyFn) {
    let mut body_sink: BufferSink = BufferSink { bytes: Vec::<u8>::new() };
    // `ar` is a &mut alias: bare reference args pass as lvalues, where a
    // by-value local would get move-wrapped at its last use and fail to
    // bind the serialize overloads' `BinaryWriteArchive&`.
    let mut ar_store = BinaryWriteArchive { sink_: make_sink_proxy(&raw mut body_sink) };
    let ar: &mut BinaryWriteArchive = &mut ar_store;
    Serialize_::serialize(v64::new(req.xid), ar);
    Serialize_::serialize(v32::new(error_code), ar);
    Serialize_::serialize(v64::new((*sconn.ctx_).server_instance_id as i64), ar);
    if write_fn {
        write_fn(ar);
    }
    sconn_dispatch_response_frame_via_channel((*sconn), body_sink.bytes.as_ptr(),
                                              body_sink.bytes.len());
}
#endif
/*RUSTYCPP:GEN-BEGIN id=server.16 version=1 rust_sha256=33782bd7bde013e52f888edc7878adca073b78aeff03d42a5c6f7ea815243e6f*/
void sconn_reply(const ServerConnection& sconn, const Request& req, int32_t error_code, ServerReplyFn write_fn) {
    BufferSink body_sink = BufferSink{.bytes = rusty::Vec<uint8_t>::new_()};
    auto ar_store = BinaryWriteArchive{.sink_ = make_sink_proxy(&body_sink)};
    BinaryWriteArchive& ar = ar_store;
    Serialize_::serialize(v64::new_(req.xid), ar);
    Serialize_::serialize(v32::new_(std::move(error_code)), ar);
    Serialize_::serialize(v64::new_(static_cast<int64_t>((rusty::detail::deref_if_pointer_like(sconn.ctx_)).server_instance_id)), ar);
    if (write_fn) {
        write_fn(ar);
    }
    sconn_dispatch_response_frame_via_channel((sconn), rusty::as_ptr(body_sink.bytes), rusty::len(body_sink.bytes));
}
/*RUSTYCPP:GEN-END id=server.16*/

// @unsafe - 5b/5c/5d: bind a channel proxy and flip the channel-mode latch.
//
// Mirrors `ClientConnection::bind_channel_direct`. After binding, the
// proxy serves as both the outbound dispatch sink (5b — `reply<F>(...)`
// calls `proxy->send_frame(...)`) and the inbound demux source (5c —
// installs `on_frame(...)` to call `decode_request_and_dispatch`).
// 5d also wires `on_closed` / `on_error` to transition the connection
// to CLOSED so the server's poll loop / accept-tracking notices the
// peer-side close (no orphan ServerConnection in the per-listener
// connection map).
//
// `weak_self_` MUST be initialized before calling this (the
// callbacks capture it; if the connection is destroyed, the upgrade
// fails and the callback short-circuits). The accept path in
// subsequent leaves (5e) wires `weak_self_` immediately after
// construction.
// @unsafe - 5c: decode one channel-mode request frame and dispatch.
//
// Mirrors the per-packet body of `handle_read` minus the size-framed
// Fill the request body from the wire bytes, then point the read cursor
// at the filled buffer. Must be called at most once per Request, before
// any read.
//
// Was a `reserve` + `memcpy` + `set_len` kernel over a raw pointer.
// Taking a slice (rule 2) lets extend_from_slice do the same work —
// reserve then copy, no zero-init — with the length carried by the
// argument instead of trusted alongside it.
#if RUSTYCPP_RUST
fn request_fill_body(req: &mut Request, bytes: &[u8]) {
    req.body.clear();
    req.body.extend_from_slice(bytes);
    req.src = BufferSource::new_(req.body.data(), req.body.len());
}
#endif
/*RUSTYCPP:GEN-BEGIN id=server.fill_body version=1 rust_sha256=b0dcfd92c216bc6b26d1cd742ac509bf3b28fb216a0687e47d0d340de4215899*/
void request_fill_body(Request& req, std::span<const uint8_t> bytes) {
    Request* req_shadow1 = &req;
    (*req_shadow1).body.clear();
    (*req_shadow1).body.extend_from_slice(bytes);
    (*req_shadow1).src = BufferSource::new_((*req_shadow1).body.data(), rusty::len((*req_shadow1).body));
}
/*RUSTYCPP:GEN-END id=server.fill_body*/

// I/O loop: the channel layer has already stripped the 4-byte size
// prefix, so the body is `[xid:v64][rpc_id:i32][user-args]`.


// The slow-path fiber body, as a free fn so the spawn closure stays
// the single-call shape (§7.60 — the inline-argument closure emission
// mis-infers return types on multi-statement bodies).
#if RUSTYCPP_RUST
// The "no handler for rpc_id" warning-dedup set. Hoisted out of
// ServerConnection (a DSL struct carries no static data member) and now
// module-scope DSL: it emits an `extern` declaration plus an `inline`
// definition. Linkage widens from `static` (internal) to inline/module,
// which is benign -- this is the non-exported `namespace rrr` and
// server.cpp is the module's only TU.
static g_rpc_id_missing: rusty::Mutex<rusty::HashSet<i32>> =
    rusty::Mutex::<rusty::HashSet<i32>>::new(rusty::HashSet::<i32>::new());

fn sconn_dispatch_in_fiber(ctx: Arc<RpcServiceContext>, svc_index: usize,
                           rpc_id: i32, req: Box<Request>,
                           weak_this: WeakServerConnection) {
    let mut guard = (*ctx).services[svc_index].borrow_mut();
    let svc: &mut Box<Service> = &mut *guard;
    svc.__dispatch__(rpc_id, req, weak_this);
}

// Decode one channel-mode request frame and dispatch. The body is
// `[xid:v64][rpc_id:i32][user-args]` (the channel layer already
// stripped the size prefix). Bytes are copied into the Request BEFORE
// any path that may yield — the channel-layer contract makes them
// valid only for this callback. The archive is a view over req.src;
// the cursor advance persists into the handler's reads.
fn sconn_decode_request_and_dispatch(sconn: &ServerConnection,
                                     bytes: *const u8, size: usize) {
    if sconn.status_.get() == ServerConnStatus::CLOSED {
        return;
    }
    let mut req_box = rusty::make_box::<Request>();
    if size > 0usize {
        request_fill_body(&mut *req_box,
                          unsafe { core::slice::from_raw_parts(bytes, size) });
    }

    // Malformed frame (not enough bytes for an xid): drop it — there
    // is no valid xid to reply against. v64 is 1-8 bytes; an empty
    // body means there is no xid at all.
    if (*req_box).src.remaining() == 0usize {
        log_line(Log::WARN, 0i32, core::ptr::null(), std::format("rrr::ServerConnection: empty channel-mode request frame, dropping"));
        return;
    }
    let mut header_ar = BinaryReadArchive {
        source_: make_source_proxy(&raw mut (*req_box).src),
    };
    let mut v_xid = v64::new(0i64);
    Deserialize_::deserialize(&mut v_xid, header_ar);
    (*req_box).xid = v_xid.get();
    (*req_box).attach_pending_guard((*sconn.ctx_).pending_requests.clone());

    // sizeof(i32) spelled as its value: not enough bytes for rpc_id.
    if (*req_box).src.remaining() < 4usize {
        let mut empty_fn1: ServerReplyFn = Default::default();
        sconn_reply((*sconn), (*req_box), EINVAL, empty_fn1);
        return;
    }

    let mut rpc_id: i32 = 0i32;
    Deserialize_::deserialize(&mut rpc_id, header_ar);
    if rpc_id == (kInternalHeartbeatRpcId as i32) {
        let hb: &Arc<ServerDropHeartbeatRepliesAtomic> =
            &(*sconn.ctx_).drop_heartbeat_replies;
        if !hb.load(rusty::sync::atomic::Ordering::Acquire) {
            let mut empty_fn2: ServerReplyFn = Default::default();
            sconn_reply((*sconn), (*req_box), 0i32, empty_fn2);
        }
        return;
    }

    let svc_index_opt = (*sconn.ctx_).rpc_to_service.get(rpc_id);
    if svc_index_opt.is_none() {
        let mut surpress_warning = false;
        {
            let mut guard = g_rpc_id_missing.lock().unwrap();
            if !(*guard).contains(rpc_id) {
                (*guard).insert(rpc_id);
            } else {
                surpress_warning = true;
            }
        }
        if !surpress_warning {
            log_line(Log::WARN, 0i32, core::ptr::null(), std::format("rrr::ServerConnection: no handler for rpc_id = {} (channel-mode dispatch)",
                     rpc_id));
        }
        let mut empty_fn3: ServerReplyFn = Default::default();
        sconn_reply((*sconn), (*req_box), ENOENT, empty_fn3);
        return;
    }

    let svc_index: usize = *svc_index_opt.unwrap();
    let weak_this = sconn.weak_self_.clone();
    if (*sconn.ctx_).fast_rpc_ids.contains(rpc_id) {
        // Fast inline dispatch — no fiber spawn.
        let mut guard = (*sconn.ctx_).services[svc_index].borrow_mut();
        let svc: &mut Box<Service> = &mut *guard;
        svc.__dispatch__(rpc_id, req_box, weak_this);
    } else {
        // Slow path — spawn a fiber so the handler can yield (e.g. for
        // nested RPC calls). The ctx Arc clone keeps the services alive
        // even if the connection is closed mid-flight.
        let ctx2 = sconn.ctx_.clone();
        let job_fn = move || {
            sconn_dispatch_in_fiber(ctx2, svc_index, rpc_id, req_box, weak_this);
        };
        Fiber::create_run(job_fn);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=server.19 version=1 rust_sha256=ae79afac57a969fcb91a4534ab1a037980554734547d49ea2455126759c79b30*/
extern rusty::Mutex<rusty::HashSet<int32_t>> g_rpc_id_missing;

inline rusty::Mutex<rusty::HashSet<int32_t>> g_rpc_id_missing = rusty::Mutex<rusty::HashSet<int32_t>>::new_(rusty::HashSet<int32_t>::new_());

void sconn_dispatch_in_fiber(rusty::Arc<RpcServiceContext> ctx, size_t svc_index, int32_t rpc_id, rusty::Box<Request> req, WeakServerConnection weak_this) {
    auto&& guard = (rusty::detail::deref_if_pointer_like(ctx)).services[svc_index].borrow_mut();
    rusty::Box<Service>& svc = rusty::detail::deref_if_pointer_like(guard);
    svc->__dispatch__(std::move(rpc_id), std::move(req), std::move(weak_this));
}

void sconn_decode_request_and_dispatch(const ServerConnection& sconn, const uint8_t* bytes, size_t size) {
    if (sconn.status_.get() == rusty::clone(ServerConnStatus_CLOSED())) {
        return;
    }
    auto req_box = rusty::make_box<Request>();
    if (rusty::detail::deref_if_pointer_like(size) > static_cast<size_t>(0)) {
        request_fill_body(rusty::detail::deref_if_pointer_like(req_box), rusty::from_raw_parts(bytes, std::move(size)));
    }
    if ((rusty::detail::deref_if_pointer_like(req_box)).src.remaining() == static_cast<size_t>(0)) {
        log_line(rusty::clone(rusty::clone(Log::WARN)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("rrr::ServerConnection: empty channel-mode request frame, dropping"));
        return;
    }
    auto header_ar = BinaryReadArchive{.source_ = make_source_proxy(&(rusty::detail::deref_if_pointer_like(req_box)).src)};
    auto v_xid = v64::new_(static_cast<int64_t>(0));
    Deserialize_::deserialize(v_xid, header_ar);
    (rusty::detail::deref_if_pointer_like(req_box)).xid = v_xid.get();
    ((rusty::detail::deref_if_pointer_like(req_box))).attach_pending_guard(rusty::clone((rusty::detail::deref_if_pointer_like(sconn.ctx_)).pending_requests));
    if ((rusty::detail::deref_if_pointer_like(req_box)).src.remaining() < static_cast<size_t>(4)) {
        ServerReplyFn empty_fn1 = rusty::default_like<ServerReplyFn>();
        sconn_reply((sconn), (rusty::detail::deref_if_pointer_like(req_box)), EINVAL, std::move(empty_fn1));
        return;
    }
    int32_t rpc_id = static_cast<int32_t>(0);
    Deserialize_::deserialize(rpc_id, header_ar);
    if (rusty::detail::deref_if_pointer_like(rpc_id) == ((static_cast<int32_t>(kInternalHeartbeatRpcId)))) {
        const rusty::Arc<ServerDropHeartbeatRepliesAtomic>& hb = (rusty::detail::deref_if_pointer_like(sconn.ctx_)).drop_heartbeat_replies;
        if (rusty::detail::rust_not(hb->load(rusty::sync::atomic::Ordering::Acquire))) {
            ServerReplyFn empty_fn2 = rusty::default_like<ServerReplyFn>();
            sconn_reply((sconn), (rusty::detail::deref_if_pointer_like(req_box)), static_cast<int32_t>(0), std::move(empty_fn2));
        }
        return;
    }
    auto svc_index_opt = (rusty::detail::deref_if_pointer_like(sconn.ctx_)).rpc_to_service.get(std::move(rpc_id));
    if (svc_index_opt.is_none()) {
        auto surpress_warning = false;
        {
            auto guard = g_rpc_id_missing.lock().unwrap();
            if (rusty::detail::rust_not(rusty::contains((rusty::detail::deref_if_pointer_like(guard)), std::move(rpc_id)))) {
                ((rusty::detail::deref_if_pointer_like(guard))).insert(std::move(rpc_id));
            } else {
                surpress_warning = true;
            }
        }
        if (rusty::detail::rust_not(surpress_warning)) {
            log_line(rusty::clone(rusty::clone(Log::WARN)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("rrr::ServerConnection: no handler for rpc_id = {} (channel-mode dispatch)", std::move(rpc_id)));
        }
        ServerReplyFn empty_fn3 = rusty::default_like<ServerReplyFn>();
        sconn_reply((sconn), (rusty::detail::deref_if_pointer_like(req_box)), ENOENT, std::move(empty_fn3));
        return;
    }
    size_t svc_index = rusty::detail::deref_if_pointer_like(svc_index_opt.unwrap());
    auto weak_this = rusty::clone(sconn.weak_self_);
    if (rusty::contains((rusty::detail::deref_if_pointer_like(sconn.ctx_)).fast_rpc_ids, std::move(rpc_id))) {
        auto&& guard = (rusty::detail::deref_if_pointer_like(sconn.ctx_)).services[svc_index].borrow_mut();
        rusty::Box<Service>& svc = rusty::detail::deref_if_pointer_like(guard);
        svc->__dispatch__(std::move(rpc_id), std::move(req_box), std::move(weak_this));
    } else {
        auto ctx2 = rusty::clone(sconn.ctx_);
        auto job_fn = [=, ctx2 = std::move(ctx2), req_box = std::move(req_box), rpc_id = std::move(rpc_id), svc_index = std::move(svc_index), weak_this = std::move(weak_this)]() mutable {
sconn_dispatch_in_fiber(std::move(ctx2), std::move(svc_index), std::move(rpc_id), std::move(req_box), std::move(weak_this));
};
        Fiber::create_run(std::move(job_fn));
    }
}
/*RUSTYCPP:GEN-END id=server.19*/

// @unsafe - 5b: dispatch a reply-frame body through the bound proxy.
//
// Locks the rusty::Mutex briefly to extract the proxy pointer, then
// drops the guard so the actual `send_frame` happens without holding
// the lock (the proxy's `send_frame` is internally thread-safe per
// the channel-layer contract). Errors are observable via the
// proxy's installed `on_error` / `on_closed` callbacks; the return
// value is intentionally discarded — the RPC layer mirrors the
// legacy fd path's behavior of not surfacing send-side errors from
// `reply()`.
// @unsafe - Box::get raw extraction for the DSL dispatch body below —
// the pointer must OUTLIVE the guard (send happens without the lock,
// per the channel-layer contract that send_frame is internally
// thread-safe).
//
// The old note here claimed Box's own `.get()` was "a handle method the
// DSL's autoderef would misroute to the pointee"; that cause EXPIRED --
// the pointer-like arrow rewrite only fires for a `let`-annotated
// `Box<..>` binding, so the chained form lowers verbatim.
#if RUSTYCPP_RUST
fn sconn_proxy_ptr(slot: &rusty::Option<ChannelConnectionProxy>)
        -> *mut ChannelConnectionBase {
    slot.as_ref().unwrap().get()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=server.24 version=1 rust_sha256=aeece4bdeb41ec642fcc7957630ca890664baa9caab38afe647f82cd53587962*/
ChannelConnectionBase* sconn_proxy_ptr(const rusty::Option<ChannelConnectionProxy>& slot) {
    return slot.as_ref().unwrap().get();
}
/*RUSTYCPP:GEN-END id=server.24*/

// Dispatch a reply-frame body through the bound proxy. Locks the
// rusty::Mutex briefly to extract the proxy pointer, then drops the
// guard so the actual `send_frame` happens without holding the lock.
// Errors are observable via the proxy's installed on_error/on_closed
// callbacks; the return value is deliberately discarded — the RPC
// layer mirrors the legacy fd path's behavior of not surfacing
// send-side errors from `reply()`.
#if RUSTYCPP_RUST
fn sconn_dispatch_response_frame_via_channel(sconn: &ServerConnection,
                                             bytes: *const u8, size: usize) {
    let mut conn_ptr: *mut ChannelConnectionBase = core::ptr::null_mut();
    {
        let guard = sconn.channel_proxy_.lock().unwrap();
        if (*guard).is_none() {
            log_line(Log::WARN, 0i32, core::ptr::null(), std::format("rrr::ServerConnection::dispatch_response_frame_via_channel: channel mode flipped on but proxy is unbound (race?). Dropping reply."));
            return;
        }
        conn_ptr = sconn_proxy_ptr((*guard));
    }
    let frame = ChannelFrame { payload: bytes, size: size };
    let _ = (*conn_ptr).send_frame(frame);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=server.18 version=1 rust_sha256=9fe9d5d1deb78bf9a023ccd7ef2fe41f3c470ece8483dc9e863adc683bdd42db*/
void sconn_dispatch_response_frame_via_channel(const ServerConnection& sconn, const uint8_t* bytes, size_t size) {
    ChannelConnectionBase* conn_ptr = rusty::ptr::null_mut();
    {
        const auto&& guard = rusty::deref_call(sconn.channel_proxy_.lock(), rusty::detail::__mdisp_unwrap{});
        if (((rusty::detail::deref_if_pointer_like(guard))).is_none()) {
            log_line(rusty::clone(rusty::clone(Log::WARN)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("rrr::ServerConnection::dispatch_response_frame_via_channel: channel mode flipped on but proxy is unbound (race?). Dropping reply."));
            return;
        }
        conn_ptr = sconn_proxy_ptr((rusty::detail::deref_if_pointer_like(guard)));
    }
    const auto frame = ChannelFrame{.payload = bytes, .size = std::move(size)};
    static_cast<void>(((*conn_ptr)).send_frame(std::move(frame)));
}
/*RUSTYCPP:GEN-END id=server.18*/

// @unsafe - Executes callback inline for API compatibility.

// @safe - Closes connection.
//
// 5g2: legacy `::close(socket_)` block deleted (the field is gone).
// Channel proxy close is the only fd-tearing-down path.
//
// The channel-layer proxy.close() is idempotent and safe under
// recursive entry: close() may be called from `on_closed` which 5d
// installs, and 5d's on_closed → close() → proxy.close() →
// (idempotent) on_closed re-fires without effect.

// @safe - Out-of-line dispatch for `DeferredReply::reply`. Takes
// `archive_reply_field` via `Option::take()` so the second call
// (or a call after `reply_error`) sees `None` and warns instead of
// double-dispatching. Mirrors the pre-DSL `if (replied_)` + flip
// behaviour without needing a separate bool.
// @unsafe { Log_warn / Log_debug + sconn->reply<F> template call
//           with a moved-in Function lvalue }

// @safe - Out-of-line dispatch for `DeferredReply::reply_error`.
// Same once-fire shape via `Option::take()`; error path doesn't
// invoke the archive callback (the empty-reply overload of
// `ServerConnection::reply` only takes the error code).
// @unsafe { Log_warn / Log_debug + sconn->reply 2-arg overload }

// ============================================================================
// Server impl helpers — free functions called from the DSL block.
// ============================================================================


}  // namespace rrr
