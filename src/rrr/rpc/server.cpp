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

#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/cell.hpp>
#include <rusty/function.hpp>
#include <rusty/option.hpp>
#include <rusty/unsafe_cell.hpp>
#include <rusty/vec.hpp>
#include <rusty/rusty.hpp>

export module rrr.server;

import std;
import rrr.basetypes;
import rrr.callback_wrapper;
import rrr.channel;
import rrr.debugging;
import rrr.epoll_wrapper;
import rrr.internal_protocol;
import rrr.logging;
import rrr.marshal;
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
export namespace rrr {

class Server;
class ServerConnection;
struct RpcServiceContext;

/**
 * Server shutdown phases for graceful shutdown support.
 * Progression: RUNNING -> STOP_ACCEPTING -> DRAINING -> CLOSING -> STOPPED
 */
enum class ShutdownPhase {
    RUNNING,         // Normal operation
    STOP_ACCEPTING,  // Not accepting new connections
    DRAINING,        // Waiting for in-flight requests to complete
    CLOSING,         // Closing all connections
    STOPPED          // Fully stopped
};

// @safe - Convert ShutdownPhase to string for logging
inline const char* shutdown_phase_to_string(ShutdownPhase phase) {
    switch (phase) {
        case ShutdownPhase::RUNNING: return "RUNNING";
        case ShutdownPhase::STOP_ACCEPTING: return "STOP_ACCEPTING";
        case ShutdownPhase::DRAINING: return "DRAINING";
        case ShutdownPhase::CLOSING: return "CLOSING";
        case ShutdownPhase::STOPPED: return "STOPPED";
        default: return "UNKNOWN";
    }
}

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
// @safe - RAII guard for one in-flight request.
struct PendingRequestGuard {
    rusty::Arc<std::atomic<int>> pending_counter;

    explicit PendingRequestGuard(rusty::Arc<std::atomic<int>> counter)
        : pending_counter(std::move(counter)) {
        if (pending_counter.is_valid()) {
            auto* counter_ptr = const_cast<std::atomic<int>*>(pending_counter.get());
            counter_ptr->fetch_add(1, std::memory_order_relaxed);
        }
    }

    ~PendingRequestGuard() {
        if (pending_counter.is_valid()) {
            auto* counter_ptr = const_cast<std::atomic<int>*>(pending_counter.get());
            counter_ptr->fetch_sub(1, std::memory_order_relaxed);
        }
    }

    PendingRequestGuard(PendingRequestGuard&&) = default;
    PendingRequestGuard& operator=(PendingRequestGuard&&) = default;
    PendingRequestGuard(const PendingRequestGuard&) = delete;
    PendingRequestGuard& operator=(const PendingRequestGuard&) = delete;
};

// @safe - Simple request container
struct Request {
    Marshal m;
    i64 xid;
    rusty::Option<rusty::Box<PendingRequestGuard>> pending_guard{rusty::None};

    // @safe - Attach request-lifetime pending counter guard once.
    void attach_pending_guard(const rusty::Arc<std::atomic<int>>& counter) {
        if (pending_guard.is_none() && counter.is_valid()) {
            pending_guard = rusty::Some(rusty::make_box<PendingRequestGuard>(counter.clone()));
        }
    }
};

// Forward declaration for WeakServerConnection
class ServerConnection;

// Type alias for Arc weak reference (must be before Service for __dispatch__)
using WeakServerConnection = rusty::sync::Weak<ServerConnection>;

// @interface
// @safe - Pure virtual interface. All declarations carry per-method `// @safe`.
class Service {
public:
    virtual ~Service() = default;
    // Virtual method for service registration with index (used by reg_service)
    // Returns list of RPC IDs that this service handles
    // @safe
    virtual int __reg_to__(Server&, size_t svc_index) = 0;

    // @safe - Virtual dispatch method for handling RPC requests
    // Each service implements this to route requests to the appropriate handler
    // Uses virtual dispatch to avoid raw pointer capture and static_cast
    virtual void __dispatch__(i32 rpc_id, rusty::Box<Request> req, WeakServerConnection sconn) = 0;

    // Return a reference to the underlying Service instance. Default
    // returns `*this`; the typed-box adapter overrides this to return
    // its Service-shaped adapter self (the wrapped concrete T is not
    // a Service). Used by `Server::for_each_service` for cleanup hooks
    // that need to inspect the concrete service.
    // @safe - returns a reference to `*this`; no cast through void*.
    virtual Service& __get_service__() { return *this; }
};

using ServiceProxy = rusty::Box<Service>;

// Pass-through factory for services that already inherit Service.
// @safe - Box move.
inline ServiceProxy make_service_proxy_from_box(rusty::Box<Service> svc) {
  return svc;
}

// Concept matching the structural shape of a service: the duck-typed
// `__reg_to__` / `__dispatch__` pair. Generated rcc_rpc.h services
// satisfy this without inheriting `Service`; `ServiceTypedBoxAdapter`
// bridges them into a `Box<Service>`.
template <typename T>
concept ServiceLike = requires(
    T& svc,
    Server& server,
    size_t svc_index,
    i32 rpc_id,
    rusty::Box<Request> req,
    WeakServerConnection weak_sconn) {
  { svc.__reg_to__(server, svc_index) } -> std::convertible_to<int>;
  { svc.__dispatch__(rpc_id, std::move(req), std::move(weak_sconn)) } -> std::same_as<void>;
};

// Adapter that wraps a Box<T> for a duck-typed T and exposes it as a
// concrete subclass of Service.
// @safe - Pure adapter; forwards `__reg_to__` / `__dispatch__` into the
// wrapped Box<T>. No raw pointer math, no syscalls.
template <ServiceLike T>
class ServiceTypedBoxAdapter : public Service {
 public:
  explicit ServiceTypedBoxAdapter(rusty::Box<T> svc) : svc_(std::move(svc)) {}

  int __reg_to__(Server& server, size_t svc_index) override {
    return svc_->__reg_to__(server, svc_index);
  }

  void __dispatch__(i32 rpc_id, rusty::Box<Request> req, WeakServerConnection sconn) override {
    svc_->__dispatch__(rpc_id, std::move(req), std::move(sconn));
  }

  // Returns `*this` (the adapter itself, which IS a Service). The wrapped
  // T does not inherit `Service`, so we can't expose it through a
  // Service&-shaped callback; callers needing the concrete T should
  // hold the typed handle separately.
  // @safe - returns a reference to `*this`; no cast through void*.
  Service& __get_service__() override { return *this; }

 private:
  rusty::Box<T> svc_;
};

// @safe - Wraps a typed Box<T> in the ServiceTypedBoxAdapter; Box move only.
template <ServiceLike T>
inline ServiceProxy make_service_proxy_from_typed_box(rusty::Box<T> svc) {
  return rusty::make_box<ServiceTypedBoxAdapter<T>>(std::move(svc));
}

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
// @safe - All fields are const after construction; the ctor just moves
// owned containers into place. No syscalls, no raw pointers.
struct RpcServiceContext {
    // Maps RPC ID to service index for dispatch (immutable after setup)
    const rusty::HashMap<i32, size_t> rpc_to_service;
    // RPC IDs that should be dispatched inline (no per-request server fiber).
    const rusty::HashSet<i32> fast_rpc_ids;

    // Owned service proxies wrapped in RefCell for interior mutability
    // RefCell allows mutable access through const reference (borrow_mut)
    const rusty::Vec<rusty::RefCell<ServiceProxy>> services;

    // Server address for logging (immutable after setup)
    const std::string addr;
    // Shared in-flight request counter for dispatch-lifetime tracking.
    const rusty::Arc<std::atomic<int>> pending_requests;
    // Test/runtime toggle to intentionally drop heartbeat probe replies.
    const rusty::Arc<std::atomic<bool>> drop_heartbeat_replies;
    // Stable server instance ID for restart detection in response headers.
    const uint64_t server_instance_id;

    // Constructor taking ownership of all data
    RpcServiceContext(rusty::HashMap<i32, size_t> rpc_map,
                      rusty::HashSet<i32> fast_rpc_set,
                      rusty::Vec<rusty::RefCell<ServiceProxy>> svcs,
                      std::string address,
                      rusty::Arc<std::atomic<int>> pending_counter,
                      rusty::Arc<std::atomic<bool>> drop_heartbeats,
                      uint64_t instance_id)
        : rpc_to_service(std::move(rpc_map))
        , fast_rpc_ids(std::move(fast_rpc_set))
        , services(std::move(svcs))
        , addr(std::move(address))
        , pending_requests(std::move(pending_counter))
        , drop_heartbeat_replies(std::move(drop_heartbeats))
        , server_instance_id(instance_id) {}
};

// 5g1: legacy `ServerListener` class deleted. The channel layer's
// `TcpListener` (registered via `ChannelFactoryProxy::make_listener()`)
// is the sole accept-loop implementation; `Server::start(addr)`
// auto-installs a default TCP factory (5f) when no explicit factory
// is bound.

// @safe - Methods that genuinely cross into unsafe ops (channel proxy
// pointer extraction, raw byte arithmetic in `decode_request_and_dispatch`,
// const_cast-through-Arc in callbacks, SpinMutex::lock + ChannelConnectionProxy
// method dispatch) carry their own `// @unsafe` overrides; the rest of the
// class is analyzed as @safe by default. Mirrors the Tier-4 flip on `Server`.
// Uses SpinMutex for thread-safe interior mutability, Arc for shared ownership.
class ServerConnection {
    // Handles individual client connections
    // SAFETY: Thread-safe with spinlocks, proper Arc lifetime management

    friend class Server;
    // 5g1: ServerListener class deleted; the friend declaration is
    // retired along with it.

    // 5g2: legacy fd-path fields removed:
    //   - `Marshal in_` (read buffer)
    //   - `SpinMutex<Marshal> out_` (write buffer)
    //   - `int socket_` (fd; channel layer's TcpConnection owns it)
    //   - `Cell<bool> pending_write_update_` (was poll-loop write
    //     mode flag — TcpConnection manages its own equivalent now)

    rusty::Arc<RpcServiceContext> ctx_;  // Shared dispatch context

    enum {
        CONNECTED, CLOSED
    } status_;

    // Weak pointer to self, initialized after creation
    // Used to pass weak reference to async handlers
    WeakServerConnection weak_self_;

    // outbound reply through the
    // channel layer.
    //
    // When `bind_channel(...)` is called with a non-null
    // `ChannelConnectionProxy`, `channel_proxy_` owns it and
    // `channel_mode_` flips to true. From that point on, `reply(...)`
    // builds the response body in a temporary `Marshal`, extracts
    // the bytes, and dispatches via `proxy->send_frame(...)`. The
    // legacy `out_` Marshal-as-syscall-buffer + `pending_write_update_`
    // poll-loop write plumbing is bypassed.
    //
    // Mutable + SpinMutex so the const `reply<F>` template path can
    // lock it briefly to dispatch a frame from any thread (mirrors
    // the client-side `direct_channel_` discipline).
    mutable SpinMutex<rusty::Option<ChannelConnectionProxy>>
        channel_proxy_{rusty::Option<ChannelConnectionProxy>(rusty::None)};
    rusty::Cell<bool> channel_mode_{false};

public:
    /**
     * Closes the connection and cleans up resources.
     * Called by:
     * 1: PollThreadWorker::do_close_pollable() for thread-safe close
     * 2: handle_error() for error handling
     */
    // @unsafe - Calls Log_debug then tears down the channel proxy via a
    // raw-pointer deref. The inner deref is inside a `// @unsafe { }` block
    // in the definition, but the @unsafe-block scope doesn't reach into
    // rusty-cpp's null-safety pass for nested if-bodies. Treat the whole
    // method as unsafe to match the definition.
    void close();

private:
    // used to surpress multiple "no handler for rpc_id=..." errro
    // SpinMutex provides thread-safe interior mutability
    static SpinMutex<rusty::HashSet<i32>> rpc_id_missing_s;

public:
    // Jetpack-specific member
    int count = 0;

    // Public destructor - Arc prevents premature destruction
    // @safe - Simple destructor
    ~ServerConnection();

    // @safe - Initializes connection. The `socket` parameter is
    // retained for source-compatibility with existing call sites
    // (e.g. `Server::start`'s on_accept hook still passes -1) but is
    // no longer stored — the channel layer's `TcpConnection` owns
    // the fd. New callers should pass -1.
    ServerConnection(rusty::Arc<RpcServiceContext> ctx, int /*socket*/);

    // Test-only: install the self-pointer before code paths that need
    // to upgrade it (e.g., the on_frame callback installed in
    // `bind_channel`). Production wires `weak_self_` via the listener
    // accept path. Tests that construct `ServerConnection` directly
    // via `Arc::make` must call this before any channel-mode code
    // path that captures the weak.
    // @safe - Direct field assignment; rusty::sync::Weak move-assign is now @safe.
    // Callers must guarantee the weak refers to the same Arc that owns this object.
    void install_self_weak_for_testing(WeakServerConnection weak) {
        weak_self_ = std::move(weak);
    }

    /**
     * bind a `ChannelConnectionProxy`
     * to this connection.
     *
     * Once bound, outbound `reply(...)` calls route through
     * `proxy->send_frame(...)` and skip the legacy `out_` Marshal +
     * `pending_write_update_` poll-loop write plumbing.
     *
     * Calling with a default-constructed (null) proxy is a no-op.
     * Calling more than once replaces the previously-bound proxy.
     */
    // @unsafe - Records the proxy under SpinMutex interior storage.
    void bind_channel(ChannelConnectionProxy proxy);

    // @safe - True if `bind_channel` has been called with a non-null proxy.
    bool is_channel_mode() const { return channel_mode_.get(); }

    // @safe - Simple status check
    bool connected() {
      return status_ == CONNECTED;
    }

    /**
     * Send a reply message with callback-based marshaling.
     *
     * Reply message format:
     * <size> <xid> <error_code> [<server_instance_id>] <ret1> <ret2> ... <retN>
     * NOTE: size does not include size itself (<xid>..<retN>).
     * If the size high-bit flag is set, <server_instance_id> is present.
     *
     * The write_fn callback receives a Marshal& to write <ret1>..<retN>.
     *
     * Currently used errno:
     * 0: everything is fine
     * ENOENT: method not found
     * EINVAL: invalid packet (field missing)
     */
    // @safe - Sends reply with callback for marshaling response data.
    //
    // 5g2: legacy `out_` Marshal-as-syscall-buffer branch deleted.
    // Channel mode is the only path. The body's wire layout is
    // `[xid:v64][error_code:v32][server_instance_id:v64][reply_data]`
    // — the channel layer prepends the 4-byte size header on the
    // wire. The legacy fd path's high-bit "extended-header" flag is
    // implicit (server always emits the instance id; the client
    // always reads it).
    //
    // Tests that construct a ServerConnection without calling
    // `bind_channel(...)` will silently drop replies (the channel
    // proxy is unbound — `dispatch_response_frame_via_channel` logs
    // a warning and returns). Production paths via Server::start
    // always bind_channel before any reply.
    template<typename F>
    void reply(const Request& req, i32 error_code, F&& write_fn) const {
        // Build response body directly into a contiguous `BufferSink`.
        // Header (`v64 xid`, `v32 error_code`,
        // `v64 server_instance_id`) and the user payload accumulate
        // in `body_sink.bytes`; passed straight to the channel layer
        // with no `Marshal` chunks and no intermediate `body_bytes`
        // copy.  Mirrors the same simplification on the client send
        // path.
        BufferSink body_sink;
        BinaryWriteArchive ar(&body_sink);
        static_assert(std::is_invocable_v<F&, BinaryWriteArchive&>,
                      "reply write_fn must accept BinaryWriteArchive&");
        ar << v64(req.xid);
        ar << v32(error_code);
        ar << v64(static_cast<i64>(ctx_->server_instance_id));
        write_fn(ar);
        dispatch_response_frame_via_channel(body_sink.bytes.data(),
                                            body_sink.bytes.len());
    }

    // @safe - Sends empty reply
    void reply(const Request& req, i32 error_code = 0) const {
        reply(req, error_code, [](BinaryWriteArchive&) {});
    }

    // @unsafe - Invokes the caller-supplied `rusty::Function<void()>` callback
    // inline. The callback's body is opaque to the borrow checker; treating
    // the wrapper as @unsafe matches the out-of-line definition.
    // Takes callback by value to avoid const-propagation issues in rusty-cpp.
    int run_async(rusty::Function<void()> f);

    // @safe - Check if connection was closed
    // Called by poll loop to detect and remove closed connections
    bool is_closed() const {
        return status_ == CLOSED;
    }

private:
    // 5b: extracted reply dispatch path, kept out of the templated
    // `reply<F>` body so the implementation can sit in `server.cpp`.
    // Locks the `channel_proxy_` SpinMutex briefly to call
    // `proxy->send_frame({bytes, size})`. Errors from `send_frame`
    // (`ChannelError::WouldBlock`, `ConnectionReset`, ...) are
    // observable via the proxy's `on_error` / `on_closed` callbacks
    // — the reply-side return value is intentionally discarded.
    // @unsafe - SpinMutex::lock + ChannelConnectionProxy method dispatch.
    void dispatch_response_frame_via_channel(const std::uint8_t* bytes,
                                             std::size_t size) const;

    // 5c: channel-mode inbound demux. Parses one decoded frame
    // (`[xid:v64][rpc_id:i32][user-args]` — the channel layer has
    // already stripped the 4-byte size prefix) and routes to the
    // service registered for that rpc_id, mirroring the per-packet
    // body of `handle_read` minus the size-framed I/O loop. Called
    // from the `on_frame` callback installed by `bind_channel(...)`.
    // @unsafe - Drives Marshal / Box<Request> / Service dispatch.
    void decode_request_and_dispatch(const std::uint8_t* bytes,
                                     std::size_t size);

};

}  // export namespace rrr

// @safe - DeferredReply (RAII wrapper for deferred RPC replies) and
// Server (which owns the channel listener + accepted ServerConnection
// Arcs). Both classes carry their own descriptive `// @safe` blocks
// with per-method `// @unsafe` overrides on the socket / std::atomic
// / SpinMutex-extraction paths.
export namespace rrr {

// @safe - RAII wrapper for deferred RPC replies with move semantics
// Handler receives DeferredReply by value (moved) and calls defer.reply()
// The destructor handles cleanup (deletes in_ and out_ params allocated by wrapper)
class DeferredReply {
    rusty::Box<rrr::Request> req_;
    WeakServerConnection weak_sconn_;
    // stored callback signature is now
    // `void(BinaryWriteArchive&)` so generated `defer` handlers can
    // write through the archive layer matching every other Phase 3d
    // emission point.  `ServerConnection::reply<F>` is dual-signature
    // so the inner dispatch picks the archive branch
    // automatically when this callback runs.
    rusty::Function<void(BinaryWriteArchive&)> archive_reply_;
    rusty::Function<void()> cleanup_;
    bool replied_ = false;  // Track if reply was sent

public:
    // Movable but not copyable
    DeferredReply(DeferredReply&& other) = default;
    DeferredReply& operator=(DeferredReply&& other) = default;
    DeferredReply(const DeferredReply&) = delete;
    DeferredReply& operator=(const DeferredReply&) = delete;

    // @safe - Initializes deferred reply with move semantics
    DeferredReply(rusty::Box<rrr::Request> req, WeakServerConnection weak_sconn,
                  rusty::Function<void(BinaryWriteArchive&)> archive_reply,
                  rusty::Function<void()> cleanup)
        : req_(std::move(req)), weak_sconn_(weak_sconn),
          archive_reply_(std::move(archive_reply)), cleanup_(std::move(cleanup)) {}

    // @safe - Cleanup destructor
    // SAFETY: cleanup_() deletes in_ and out_ params allocated by wrapper
    ~DeferredReply() {
        if (cleanup_) {
            cleanup_();
        }
        // req_ automatically cleaned up by rusty::Box destructor
    }

    // @unsafe - Invokes the caller-supplied `rusty::Function<void()>` callback
    // inline; returns EINVAL on empty callback. Treated as @unsafe for the
    // same reason as the ServerConnection overload above.
    // Takes callback by value to avoid const-propagation issues in rusty-cpp.
    int run_async(rusty::Function<void()> f);

    // @safe - Sends reply using callback-based API
    // Can only be called once (checked by replied_ flag)
    // Uses weak pointer upgrade (safe: returns Option)
    void reply() {
        if (replied_) {
            Log_warn("DeferredReply::reply() called multiple times, ignoring");
            return;
        }
        replied_ = true;

        auto sconn_opt = weak_sconn_.upgrade();
        if (sconn_opt.is_some()) {
            auto sconn = sconn_opt.unwrap();
            // No const_cast needed: reply() is now a const method with interior mutability
            sconn->reply(*req_, 0, archive_reply_);
        } else {
            // Connection closed, silently drop reply
            Log_debug("Connection closed before reply sent, dropping reply");
        }
        // Object will be destroyed when it goes out of scope, destructor calls cleanup_()
    }

    // @safe - Sends error reply (no payload) using the original request context.
    // Can only be called once (checked by replied_ flag).
    void reply_error(i32 error_code) {
        if (replied_) {
            Log_warn("DeferredReply::reply_error() called multiple times, ignoring");
            return;
        }
        replied_ = true;

        auto sconn_opt = weak_sconn_.upgrade();
        if (sconn_opt.is_some()) {
            auto sconn = sconn_opt.unwrap();
            sconn->reply(*req_, error_code);
        } else {
            Log_debug("Connection closed before error reply sent, dropping reply");
        }
    }
};

// Default drain / graceful-shutdown timeout. Was the inline `= 30000`
// default arg on `Server::drain()` and `Server::graceful_shutdown()`
// before the DSL prep dropped those defaults; preserved here so call
// sites that previously relied on the implicit value can keep the same
// behaviour by passing it explicitly.
inline constexpr uint64_t kDefaultDrainTimeoutMs = 30000;

// Shutdown coordination state — guarded by `Server::shutdown_state_field`
// (a `rusty::Mutex<ShutdownState>`). Defined outside the DSL block so the
// inline-Rust source can refer to it by an opaque type name (the DSL
// transpiler does not parse nested-struct declarations inside an `impl`
// block).
struct ShutdownState { bool shutdown = false; };

// @unsafe - reinterpret_cast<const char*> on the addr param. Lives
// outside the DSL block so the inline-Rust grammar doesn't have to
// reason about `std::ffi::c_char` (which triggers a transpiler-side
// `proc_macro_runtime` import explosion). Used by the DSL `start()`
// body to bridge `*const i8` (Rust DSL) to `const char*` (legacy
// Log_error / strlen signatures).
inline const char* server_dsl_addr_to_cstr(const int8_t* addr) {
    return reinterpret_cast<const char*>(addr);
}

// Forward declaration of Server to allow helper signatures to refer
// to it. The DSL emits the full definition below.
class Server;

// Type aliases for the atomic counters carried inside the DSL Server
// struct. Defined outside the DSL block so the inline-Rust grammar
// doesn't have to parse `std::atomic<...>` (the DSL grammar does
// not yet recognize that template).
using ServerPendingRequestsAtomic = std::atomic<int32_t>;
using ServerDropHeartbeatRepliesAtomic = std::atomic<bool>;

// Helper free functions that the DSL `Server` method bodies delegate
// to. Defined out-of-line in plain C++ because the DSL grammar can't
// express things like `std::random_device` / `std::chrono` / catch
// blocks / iteration with the rusty::Mutex<T>'s lambda predicate.
//
// Each helper has the same logic the legacy Server out-of-line method
// had — moved verbatim so the DSL stays a thin shim.

// @safe - Pick the PollThread to use (auto-create one if caller didn't
// supply one). Used by the ctor.
inline rusty::Option<rusty::Arc<PollThread>> server_resolve_poll_thread(
        rusty::Option<rusty::Arc<PollThread>> poll_thread_worker) {
    if (poll_thread_worker.is_none()) {
        return rusty::Some(PollThread::create());
    }
    return std::move(poll_thread_worker);
}

// @unsafe - std::random_device may use system entropy sources.
inline uint64_t server_generate_instance_id() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    uint64_t time_component = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    std::random_device rd;
    uint64_t random_component = static_cast<uint64_t>(rd()) << 32 |
                                static_cast<uint64_t>(rd());
    uint64_t pid_component =
        static_cast<uint64_t>(rusty::sys::process::getpid()) << 48;
    uint64_t id = (time_component ^ random_component ^ pid_component)
        & static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if (id == 0) {
        id = 1;
    }
    Log_debug("Server: generated instance_id=%lu", id);
    return id;
}

// @unsafe - rusty::Mutex::lock + rusty::Condvar::wait_while with a
// closure predicate. The DSL grammar can't express the wait-while
// lambda binding, so the call is forwarded here.
inline void server_wait_for_shutdown_impl(
        const rusty::Mutex<ShutdownState>& state,
        const rusty::Box<rusty::Condvar>& cond) {
    Log_debug("Server::wait_for_shutdown");
    auto guard = state.lock().unwrap();
    guard = cond->wait_while(std::move(guard),
        [](ShutdownState& s) { return !s.shutdown; }).unwrap();
    Log_debug("Server::wait_for_shutdown - done");
}

// @unsafe - std::atomic::load (modeled as non-safe by rusty-cpp).
inline int32_t server_atomic_load_int(
        const rusty::Arc<ServerPendingRequestsAtomic>& a) {
    return a->load(std::memory_order_relaxed);
}

// @unsafe - const_cast + std::atomic::fetch_add.
inline void server_atomic_fetch_add_int(
        const rusty::Arc<ServerPendingRequestsAtomic>& a, int32_t delta) {
    auto* ptr = const_cast<ServerPendingRequestsAtomic*>(a.get());
    ptr->fetch_add(delta, std::memory_order_relaxed);
}

// @unsafe - const_cast + std::atomic::fetch_sub.
inline void server_atomic_fetch_sub_int(
        const rusty::Arc<ServerPendingRequestsAtomic>& a, int32_t delta) {
    auto* ptr = const_cast<ServerPendingRequestsAtomic*>(a.get());
    ptr->fetch_sub(delta, std::memory_order_relaxed);
}

// @unsafe - const_cast + std::atomic::store.
inline void server_atomic_store_bool(
        const rusty::Arc<ServerDropHeartbeatRepliesAtomic>& a, bool v) {
    auto* ptr = const_cast<ServerDropHeartbeatRepliesAtomic*>(a.get());
    ptr->store(v, std::memory_order_release);
}

// @unsafe - std::atomic::load.
inline bool server_atomic_load_bool(
        const rusty::Arc<ServerDropHeartbeatRepliesAtomic>& a) {
    return a->load(std::memory_order_acquire);
}

// @unsafe - Atomic loop + sleep. The DSL can't easily express the
// busy-wait loop so the body is moved here verbatim from the legacy
// `Server::drain` out-of-line.
inline bool server_drain_impl(
        const rusty::Cell<ShutdownPhase>& phase,
        const rusty::Arc<ServerPendingRequestsAtomic>& pending,
        uint64_t timeout_ms) {
    auto current_phase = phase.get();
    if (current_phase != ShutdownPhase::RUNNING &&
        current_phase != ShutdownPhase::STOP_ACCEPTING) {
        Log_debug("Server::drain: already in phase %s",
                  shutdown_phase_to_string(current_phase));
        return pending->load(std::memory_order_relaxed) == 0;
    }
    Log_info("Server::drain: transitioning to DRAINING, pending=%d",
             pending->load(std::memory_order_relaxed));
    phase.set(ShutdownPhase::DRAINING);
    const std::uint64_t start_us =
        rusty::sys::time::clock_monotonic_us();
    const std::uint64_t timeout_us = timeout_ms * 1000;
    while (pending->load(std::memory_order_relaxed) > 0) {
        const std::uint64_t elapsed_us =
            rusty::sys::time::clock_monotonic_us() - start_us;
        if (elapsed_us >= timeout_us) {
            Log_warn("Server::drain: timeout after %lu ms, pending=%d",
                     timeout_ms,
                     pending->load(std::memory_order_relaxed));
            return false;
        }
        rusty::sys::time::sleep_us(1000);
    }
    Log_info("Server::drain: completed, all requests drained");
    return true;
}

// @unsafe - try/catch + callback execution.
inline void server_run_shutdown_hooks(
        const SpinMutex<rusty::Vec<ShutdownHook>>& hooks) {
    Log_info("Server::graceful_shutdown: transitioning to CLOSING, executing hooks");
    auto guard = hooks.lock().unwrap();
    for (auto& hook : *guard) {
        try {
            hook();
        } catch (const std::exception& e) {
            Log_error("Server::graceful_shutdown: hook threw exception: %s", e.what());
        } catch (...) {
            Log_error("Server::graceful_shutdown: hook threw unknown exception");
        }
    }
}

// @unsafe - Box::get + virtual dispatch through ChannelListenerBase.
// Lives here so the DSL doesn't have to navigate the `.as_ref()
// .unwrap()` -> Box auto-deref chain (which currently emits
// `(box).close()` instead of `(*box).close()`).
inline void server_close_channel_listener_if_bound(
        const rusty::Option<ChannelListenerProxy>& listener_opt) {
    if (listener_opt.is_some()) {
        auto* listener = listener_opt.as_ref().unwrap().get();
        listener->close();
    }
}

// @unsafe - std::stoi / std::string ops / try/catch.
inline int32_t server_get_bound_port_impl(
        const rusty::Option<ChannelListenerProxy>& listener_opt) {
    if (listener_opt.is_none()) {
        return -1;
    }
    std::string local;
    {
        auto* listener = listener_opt.as_ref().unwrap().get();
        local = listener->local_address();
    }
    auto colon = local.rfind(':');
    if (colon == std::string::npos) {
        Log_error("Server::get_bound_port: malformed local_address %s",
                  local.c_str());
        return -1;
    }
    try {
        int port = std::stoi(local.substr(colon + 1));
        return port;
    } catch (const std::exception&) {
        Log_error("Server::get_bound_port: failed to parse port from %s",
                  local.c_str());
        return -1;
    }
}

// Forward declaration for the `server_start_impl` helper — the body
// is defined in plain C++ further down because it references Server's
// fields, makes complex on_accept / on_error lambda captures, and
// drives Arc<RpcServiceContext> moves that the DSL grammar can't
// express directly.
int32_t server_start_impl(Server& self, const int8_t* bind_addr);

// Forward declaration for the `server_drop_impl` helper — the body
// is defined in plain C++ further down. Auto-deref on
// `Box<ChannelListenerBase>` for `.close()` and on
// `Arc<ServerConnection>` (via `Vec[i]`) for `.close()` doesn't fire
// in the DSL emission for these complex chained accesses, so the
// body is hand-written.
void server_drop_impl(Server& self);

// @safe - Iterate over each registered service and invoke the
// callback. Lives here as a free function template so the DSL's
// `for_each_service<F>` can delegate without trying to emit the
// `auto guard = ...borrow_mut(); (*guard)->method()` double-deref
// (which the transpiler currently mishandles, see comments above).
// Forward-declared here; defined after the DSL emits the Server
// struct so the body can access Server's fields.
template<typename F>
inline void server_for_each_service_impl(const Server& self, F&& callback);

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
//   * `for_each_service<F>` (template) and `reg_service_typed<T>`
//     (template) stay OUTSIDE the DSL as hand-written templates —
//     the DSL grammar can't express template methods.
//
// @safe - Methods that genuinely cross into unsafe ops (socket I/O via the
// channel-layer's TcpListener, Pthread / std::atomic primitives, raw
// pointer extraction from ChannelListenerProxy, etc.) carry their own
// `// @unsafe` overrides; the rest of the class is now analyzed as @safe
// by default. Mirrors the Tier-4 flip on `Client`.
#if RUSTYCPP_RUST
struct Server {
    pending_services_field: rusty::Vec<ServiceProxy>,
    pending_rpc_to_service_field: rusty::HashMap<i32, usize>,
    pending_fast_rpc_ids_field: rusty::HashSet<i32>,
    ctx_field: rusty::Option<rusty::Arc<RpcServiceContext>>,
    poll_thread_field: rusty::Option<rusty::Arc<PollThread>>,
    shutdown_state_field: rusty::Mutex<ShutdownState>,
    shutdown_cond_field: rusty::Box<rusty::Condvar>,
    shutdown_phase_field: rusty::Cell<ShutdownPhase>,
    shutdown_hooks_field: SpinMutex<rusty::Vec<ShutdownHook>>,
    pending_requests_field: rusty::Arc<ServerPendingRequestsAtomic>,
    drop_heartbeat_replies_field: rusty::Arc<ServerDropHeartbeatRepliesAtomic>,
    instance_id_field: u64,
    channel_factory_field: rusty::Option<ChannelFactoryProxy>,
    channel_listener_field: rusty::Option<ChannelListenerProxy>,
    channel_sconns_field: SpinMutex<rusty::Vec<rusty::Arc<ServerConnection>>>,
}

impl Drop for Server {
    fn drop(&mut self) {
        server_drop_impl(self);
    }
}

impl Server {
    #[cpp_ctor]
    fn new(poll_thread_worker: rusty::Option<rusty::Arc<PollThread>>) -> Server {
        Server {
            pending_services_field: Vec::<ServiceProxy>(),
            pending_rpc_to_service_field: rusty::HashMap::<i32, usize>(),
            pending_fast_rpc_ids_field: rusty::HashSet::<i32>(),
            ctx_field: rusty::None,
            poll_thread_field: server_resolve_poll_thread(poll_thread_worker),
            shutdown_state_field: rusty::Mutex::<ShutdownState>(ShutdownState {}),
            shutdown_cond_field: rusty::make_box::<rusty::Condvar>(),
            shutdown_phase_field: rusty::Cell::<ShutdownPhase>::new(ShutdownPhase::RUNNING),
            shutdown_hooks_field: SpinMutex::<rusty::Vec<ShutdownHook>>::new(Vec::<ShutdownHook>()),
            pending_requests_field: rusty::Arc::<ServerPendingRequestsAtomic>::make(0i32),
            drop_heartbeat_replies_field: rusty::Arc::<ServerDropHeartbeatRepliesAtomic>::make(false),
            instance_id_field: server_generate_instance_id(),
            channel_factory_field: rusty::None,
            channel_listener_field: rusty::None,
            channel_sconns_field: SpinMutex::<rusty::Vec<rusty::Arc<ServerConnection>>>::new(Vec::<rusty::Arc<ServerConnection>>()),
        }
    }

    fn set_channel_factory(&mut self, factory: ChannelFactoryProxy) {
        if !factory {
            return;
        }
        self.channel_factory_field = rusty::Some(factory);
    }

    fn is_channel_factory_bound(&self) -> bool {
        self.channel_factory_field.is_some()
    }

    fn reg_service(&mut self, svc: rusty::Box<Service>) {
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
        server_close_channel_listener_if_bound(self.channel_listener_field);
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
        server_atomic_load_int(self.pending_requests_field)
    }

    fn increment_pending(&self) {
        server_atomic_fetch_add_int(self.pending_requests_field, 1i32);
    }

    fn decrement_pending(&self) {
        server_atomic_fetch_sub_int(self.pending_requests_field, 1i32);
    }

    fn set_drop_heartbeat_replies(&self, drop: bool) {
        server_atomic_store_bool(self.drop_heartbeat_replies_field, drop);
    }

    fn drop_heartbeat_replies(&self) -> bool {
        server_atomic_load_bool(self.drop_heartbeat_replies_field)
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

    fn start(&mut self, bind_addr: *const i8) -> i32 {
        server_start_impl(self, bind_addr)
    }

    fn get_bound_port(&self) -> i32 {
        server_get_bound_port_impl(self.channel_listener_field)
    }

    fn reg_service_typed<T>(&mut self, svc: rusty::Box<T>) {
        self.pending_services_field.push(make_service_proxy_from_typed_box::<T>(svc));
        let svc_index: usize = self.pending_services_field.size() - 1usize;
        (*self.pending_services_field[svc_index]).__reg_to__(self, svc_index);
    }

    fn for_each_service<F>(&self, callback: F) {
        server_for_each_service_impl(self, callback);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=server.1 version=1 rust_sha256=2772009b5e40b708a4af11a2a5cd3f33ac3f0987a03e0216f6b46079359c9484*/
struct Server;

struct Server {
    rusty::Vec<ServiceProxy> pending_services_field;
    rusty::HashMap<int32_t, size_t> pending_rpc_to_service_field;
    rusty::HashSet<int32_t> pending_fast_rpc_ids_field;
    rusty::Option<rusty::Arc<RpcServiceContext>> ctx_field;
    rusty::Option<rusty::Arc<PollThread>> poll_thread_field;
    rusty::Mutex<ShutdownState> shutdown_state_field;
    rusty::Box<rusty::Condvar> shutdown_cond_field;
    rusty::Cell<ShutdownPhase> shutdown_phase_field;
    SpinMutex<rusty::Vec<ShutdownHook>> shutdown_hooks_field;
    rusty::Arc<ServerPendingRequestsAtomic> pending_requests_field;
    rusty::Arc<ServerDropHeartbeatRepliesAtomic> drop_heartbeat_replies_field;
    uint64_t instance_id_field;
    rusty::Option<ChannelFactoryProxy> channel_factory_field;
    rusty::Option<ChannelListenerProxy> channel_listener_field;
    SpinMutex<rusty::Vec<rusty::Arc<ServerConnection>>> channel_sconns_field;
    mutable bool _rusty_forgotten = false;
    Server(rusty::Vec<ServiceProxy> pending_services_field_init, rusty::HashMap<int32_t, size_t> pending_rpc_to_service_field_init, rusty::HashSet<int32_t> pending_fast_rpc_ids_field_init, rusty::Option<rusty::Arc<RpcServiceContext>> ctx_field_init, rusty::Option<rusty::Arc<PollThread>> poll_thread_field_init, rusty::Mutex<ShutdownState> shutdown_state_field_init, rusty::Box<rusty::Condvar> shutdown_cond_field_init, rusty::Cell<ShutdownPhase> shutdown_phase_field_init, SpinMutex<rusty::Vec<ShutdownHook>> shutdown_hooks_field_init, rusty::Arc<ServerPendingRequestsAtomic> pending_requests_field_init, rusty::Arc<ServerDropHeartbeatRepliesAtomic> drop_heartbeat_replies_field_init, uint64_t instance_id_field_init, rusty::Option<ChannelFactoryProxy> channel_factory_field_init, rusty::Option<ChannelListenerProxy> channel_listener_field_init, SpinMutex<rusty::Vec<rusty::Arc<ServerConnection>>> channel_sconns_field_init) : pending_services_field(std::move(pending_services_field_init)), pending_rpc_to_service_field(std::move(pending_rpc_to_service_field_init)), pending_fast_rpc_ids_field(std::move(pending_fast_rpc_ids_field_init)), ctx_field(std::move(ctx_field_init)), poll_thread_field(std::move(poll_thread_field_init)), shutdown_state_field(std::move(shutdown_state_field_init)), shutdown_cond_field(std::move(shutdown_cond_field_init)), shutdown_phase_field(std::move(shutdown_phase_field_init)), shutdown_hooks_field(std::move(shutdown_hooks_field_init)), pending_requests_field(std::move(pending_requests_field_init)), drop_heartbeat_replies_field(std::move(drop_heartbeat_replies_field_init)), instance_id_field(std::move(instance_id_field_init)), channel_factory_field(std::move(channel_factory_field_init)), channel_listener_field(std::move(channel_listener_field_init)), channel_sconns_field(std::move(channel_sconns_field_init)) {}
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
    void rusty_mark_forgotten() const noexcept { _rusty_forgotten = true; }


    ~Server() noexcept(false);
    Server(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker);
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
    server_drop_impl((*this));
}

Server::Server(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker)
    : pending_services_field(rusty::Vec<ServiceProxy>())
    , pending_rpc_to_service_field(rusty::HashMap<int32_t, size_t>())
    , pending_fast_rpc_ids_field(rusty::HashSet<int32_t>())
    , ctx_field(rusty::None)
    , poll_thread_field(server_resolve_poll_thread(std::move(poll_thread_worker)))
    , shutdown_state_field(rusty::Mutex<ShutdownState>(ShutdownState{}))
    , shutdown_cond_field(rusty::make_box<rusty::Condvar>())
    , shutdown_phase_field(rusty::Cell<ShutdownPhase>::new_(rusty::clone(rusty::clone(ShutdownPhase::RUNNING))))
    , shutdown_hooks_field(SpinMutex<rusty::Vec<ShutdownHook>>::new_(rusty::Vec<ShutdownHook>()))
    , pending_requests_field(rusty::Arc<ServerPendingRequestsAtomic>::make(static_cast<int32_t>(0)))
    , drop_heartbeat_replies_field(rusty::Arc<ServerDropHeartbeatRepliesAtomic>::make(false))
    , instance_id_field(server_generate_instance_id())
    , channel_factory_field(rusty::None)
    , channel_listener_field(rusty::None)
    , channel_sconns_field(SpinMutex<rusty::Vec<rusty::Arc<ServerConnection>>>::new_(rusty::Vec<rusty::Arc<ServerConnection>>()))
{}

void Server::set_channel_factory(ChannelFactoryProxy factory) {
    if (!factory) {
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
    if (((static_cast<int32_t>(this->shutdown_phase_field.get()))) != ((static_cast<int32_t>(ShutdownPhase::RUNNING)))) {
        return;
    }
    this->shutdown_phase_field.set(rusty::clone(rusty::clone(ShutdownPhase::STOP_ACCEPTING)));
    server_close_channel_listener_if_bound(this->channel_listener_field);
}

bool Server::drain(uint64_t timeout_ms) const {
    return server_drain_impl(this->shutdown_phase_field, this->pending_requests_field, std::move(timeout_ms));
}

void Server::graceful_shutdown(uint64_t drain_timeout_ms) {
    this->stop_accepting();
    this->drain(std::move(drain_timeout_ms));
    this->shutdown_phase_field.set(rusty::clone(rusty::clone(ShutdownPhase::CLOSING)));
    server_run_shutdown_hooks(this->shutdown_hooks_field);
    this->do_shutdown();
    this->shutdown_phase_field.set(rusty::clone(rusty::clone(ShutdownPhase::STOPPED)));
}

ShutdownPhase Server::phase() const {
    return this->shutdown_phase_field.get();
}

int32_t Server::pending_request_count() const {
    return server_atomic_load_int(this->pending_requests_field);
}

void Server::increment_pending() const {
    server_atomic_fetch_add_int(this->pending_requests_field, static_cast<int32_t>(1));
}

void Server::decrement_pending() const {
    server_atomic_fetch_sub_int(this->pending_requests_field, static_cast<int32_t>(1));
}

void Server::set_drop_heartbeat_replies(bool drop) const {
    server_atomic_store_bool(this->drop_heartbeat_replies_field, std::move(drop));
}

bool Server::drop_heartbeat_replies() const {
    return server_atomic_load_bool(this->drop_heartbeat_replies_field);
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
    return server_start_impl((*this), bind_addr);
}

int32_t Server::get_bound_port() const {
    return server_get_bound_port_impl(this->channel_listener_field);
}

template<typename T>
void Server::reg_service_typed(rusty::Box<T> svc) {
    this->pending_services_field.push(make_service_proxy_from_typed_box<T>(std::move(svc)));
    const size_t svc_index = this->pending_services_field.size() - static_cast<size_t>(1);
    ((rusty::detail::deref_if_pointer_like(this->pending_services_field[svc_index]))).__reg_to__((*this), std::move(svc_index));
}

template<typename F>
void Server::for_each_service(F callback) const {
    server_for_each_service_impl((*this), std::move(callback));
}
/*RUSTYCPP:GEN-END id=server.1*/

// Body of the for_each_service helper. Defined after the GEN block
// so it can refer to Server's fields directly. The DSL's
// `for_each_service<F>` member template delegates here. Iterates
// over the immutable `services` Vec inside `ctx_field`, calling
// `(*guard)->__get_service__()` for each entry.
template<typename F>
inline void server_for_each_service_impl(const Server& self, F&& callback) {
    auto& ctx = self.ctx_field.as_ref().unwrap();
    for (size_t i = 0; i < ctx->services.size(); ++i) {
        auto guard = ctx->services[i].borrow_mut();
        callback((*guard)->__get_service__());
    }
}

#if 0  // Legacy hand-written Server class — kept under #if 0 during DSL migration.

// @safe - Methods that genuinely cross into unsafe ops (socket I/O via the
// channel-layer's TcpListener, Pthread / std::atomic primitives, raw
// pointer extraction from ChannelListenerProxy, etc.) carry their own
// `// @unsafe` overrides; the rest of the class is now analyzed as @safe
// by default. Mirrors the Tier-4 flip on `Client`.
// Thread-safe connection management uses rusty::SpinMutex.
class Server: public NoCopy {
    friend class ServerConnection;
 public:
    // Pending registration data (before start() is called)
    // These are moved into RpcServiceContext in start()
    rusty::Vec<ServiceProxy> pending_services_;
    rusty::HashMap<i32, size_t> pending_rpc_to_service_;
    rusty::HashSet<i32> pending_fast_rpc_ids_;

    // Shared context containing RPC dispatch info and services
    // Created in start() after all registrations are complete
    // None until start() is called
    rusty::Option<rusty::Arc<RpcServiceContext>> ctx_;

    // Poll thread for async I/O — shared with the channel layer's
    // TcpListener / TcpFactory. Was previously also shared with the
    // legacy `ServerListener` (deleted in 5g1).
    rusty::Option<rusty::Arc<PollThread>> poll_thread_;

    // 5g1: `Option<Arc<ServerListener>> server_listener_` deleted
    // (the legacy listener class is gone — channel mode is the only
    // accept-loop path).

    // Shutdown coordination - allows workers to wait for shutdown signal
    //
    // `shutdown_cond_` is held behind a `rusty::Box` because the
    // underlying `std::condition_variable` (and thus `rusty::Condvar`)
    // is non-movable. Box gives Server a movable storage cell — needed
    // both for the `static new_()` factory below and for the upcoming
    // inline-Rust DSL form (the DSL emits an explicit move ctor; a
    // raw `Condvar` field would make it ill-formed).
    struct ShutdownState { bool shutdown = false; };
    rusty::Mutex<ShutdownState> shutdown_state_{ShutdownState{}};
    rusty::Box<rusty::Condvar> shutdown_cond_{rusty::make_box<rusty::Condvar>()};

    // Graceful shutdown support
    rusty::Cell<ShutdownPhase> shutdown_phase_{ShutdownPhase::RUNNING};
    SpinMutex<rusty::Vec<ShutdownHook>> shutdown_hooks_;
    rusty::Arc<std::atomic<int>> pending_requests_{rusty::Arc<std::atomic<int>>::make(0)};
    rusty::Arc<std::atomic<bool>> drop_heartbeat_replies_{
        rusty::Arc<std::atomic<bool>>::make(false)};

    // Server restart detection: unique instance ID generated on startup
    // Used by clients to detect server restarts (ID changes after restart)
    uint64_t instance_id_;

    // channel-factory scaffolding.
    //
    // When a `ChannelFactoryProxy` is bound via `set_channel_factory(...)`,
    // 5e routes `Server::start(addr)` through `factory->make_listener()
    // -> listener.set_on_accept(...) -> listener->listen(addr)` instead
    // of the legacy `ServerListener`'s `socket(2)+bind(2)+listen(2)+
    // accept(2)+epoll` path.
    //
    rusty::Option<ChannelFactoryProxy> channel_factory_{rusty::None};

    // channel-mode listener +
    // accepted-connection tracking.
    //
    // `channel_listener_` owns the `ChannelListenerProxy` returned by
    // `factory->make_listener()`. It is held on the server so the
    // listener's lifetime matches the server's; dropping it (in
    // `~Server` or `stop_accepting`) tears down the listening socket
    // via the proxy's `close()` and the underlying TcpListener's
    // destructor.
    //
    // `channel_sconns_` holds Arcs to each accepted channel-mode
    // ServerConnection so they outlive the on_accept callback's stack
    // frame. Each on_closed (wired in 5d) marks the connection CLOSED
    // but does not remove it from the vec — eviction is deferred to
    // `~Server`'s drop. SpinMutex so concurrent on_accept invocations
    // (the channel layer can fire on_accept on the poll thread while
    // a user thread iterates) stay safe.
    rusty::Option<ChannelListenerProxy> channel_listener_{rusty::None};
    mutable SpinMutex<rusty::Vec<rusty::Arc<ServerConnection>>>
        channel_sconns_{rusty::Vec<rusty::Arc<ServerConnection>>()};

public:
    // @safe - Creates server with optional PollThread
    // SAFETY: Shared ownership of PollThread via Arc<Mutex<>>
    //
    // No default arg — all callers already pass an explicit
    // `rusty::Option<rusty::Arc<PollThread>>` (Some or None).
    // Removing the default lines the ctor up with the DSL form,
    // which can't express default arguments.
    Server(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker);
    // @safe - Destroys server and requests close for all connections
    // SAFETY: Arc<RpcServiceContext> ensures services live until all connections are done
    virtual ~Server() noexcept override;

    // @safe - Restore implicit move that the user-declared virtual
    // destructor + NoCopy-base destructor suppressed. Every Server
    // field is movable: rusty::Vec, HashMap, HashSet, Option,
    // Mutex<T>, Box<Condvar> (Condvar itself isn't movable; the Box
    // wrapper above gives it movable storage), Cell, SpinMutex over
    // Vec, Arc<atomic<>>, uint64_t, Option<ChannelFactoryProxy>,
    // Option<ChannelListenerProxy>. Copy stays implicitly disabled
    // by the `NoCopy` base (and we never copied Servers in the first
    // place — they live behind a raw `new Server(...)`).
    Server(Server&&) noexcept = default;
    Server& operator=(Server&&) noexcept = default;

    // @safe - Rust-style factory matching the inline-Rust DSL
    // `fn new(poll_thread_worker) -> Self` form. Returns a Server by
    // value; C++17 mandatory copy-elision installs it directly at
    // the caller's storage slot.
    //
    // Existing callers continue to use `new Server(arg)` for raw-
    // pointer allocation; new code that wants a value-style Server
    // should prefer `Server::new_(arg)`.
    static Server new_(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker) {
        return Server{std::move(poll_thread_worker)};
    }

    // @unsafe - Starts server on specified address (raw pointer dereference)
    int start(const char* bind_addr);

    /**
     * bind a `ChannelFactoryProxy`
     * to this server.
     *
     * Once bound, subsequent leaves (5e/5f) will route `start(addr)`
     * through `factory->make_listener() -> listener.set_on_accept(...)
     * -> listener->listen(addr)` instead of the legacy
     * `ServerListener` socket path. For 5a this setter is purely
     * scaffolding — `start()` does not read the field yet.
     *
     * Calling with a default-constructed (null) proxy is a no-op.
     * Calling more than once replaces the previously-bound factory.
     */
    // @unsafe - Records the factory under Option interior storage.
    void set_channel_factory(ChannelFactoryProxy factory) {
        if (!factory) return;
        // @unsafe { ChannelFactoryProxy move }
        channel_factory_ = rusty::Some(std::move(factory));
    }

    // @safe - True if `set_channel_factory` has been called with a non-null proxy.
    bool is_channel_factory_bound() const {
        return channel_factory_.is_some();
    }

    // @safe - rusty::Vec::push/size/operator[] + Box move + Service proxy
    // dispatch are all @safe at the boundary.
    // Must be called before start().
    void reg_service(rusty::Box<Service> svc) {
        pending_services_.push(make_service_proxy_from_box(std::move(svc)));
        // Get index AFTER push - this is the position of the service we just added
        size_t svc_index = pending_services_.size() - 1;
        // Register handlers using the index (service is safely stored in pending_services_)
        pending_services_[svc_index]->__reg_to__(*this, svc_index);
    }

    void reg_service_proxy(ServiceProxy proxy) {
        pending_services_.push(std::move(proxy));
        size_t svc_index = pending_services_.size() - 1;
        pending_services_[svc_index]->__reg_to__(*this, svc_index);
    }

    // @safe - Same composition as the legacy overload.
    //
    // Renamed from `reg_service` to `reg_service_typed` so there's no
    // overload set on `reg_service` (Rust DSL forbids overloaded
    // method names). Callers that registered a service-like type that
    // does NOT derive from `Service` (e.g. test-only `EchoService` in
    // `rpc_inmemory_channel_e2e_test.cc`) switch to
    // `reg_service_typed<T>(...)`. Service-derived callers keep using
    // the `reg_service(rusty::Box<Service>)` overload above.
    template <ServiceLike T>
      requires (!std::derived_from<T, Service>)
    void reg_service_typed(rusty::Box<T> svc) {
        pending_services_.push(make_service_proxy_from_typed_box<T>(std::move(svc)));
        size_t svc_index = pending_services_.size() - 1;
        pending_services_[svc_index]->__reg_to__(*this, svc_index);
    }

    /**
     * The svc_func need to do this:
     *
     *  {
     *     // process request
     *     ..
     *
     *     // send reply using callback-based API
     *     server_connection->reply(*req, 0, [&](BinaryWriteArchive& out) {
     *         out << reply_content;
     *     });
     *
     *     // cleanup resource - automatic via rusty::Box/rusty::Arc ownership
     *     // No manual release needed
     *  }
     */
    // @safe - Registers an RPC ID to be handled by the service at svc_index
    // The actual dispatch is done via ServiceFacade::__dispatch__
    // No pointers or type erasure - just maps rpc_id to service index
    // Must be called before start().
    int reg_rpc(i32 rpc_id, size_t svc_index) {
        // disallow duplicate rpc_id
        if (pending_rpc_to_service_.contains_key(rpc_id)) {
            return EEXIST;
        }
        pending_rpc_to_service_.insert(rpc_id, svc_index);
        return 0;
    }

    // @safe - Registers an RPC ID for fast inline dispatch on server side.
    // Must be called before start().
    int reg_fast_rpc(i32 rpc_id, size_t svc_index) {
        int ret = reg_rpc(rpc_id, svc_index);
        if (ret != 0) {
            return ret;
        }
        pending_fast_rpc_ids_.insert(rpc_id);
        return 0;
    }

    // @safe - Unregisters RPC handler
    void unreg(i32 rpc_id);

    // @safe - Signals shutdown to waiting threads
    void do_shutdown();

    // @unsafe - Blocks the caller on `shutdown_cond_->wait_while(...)`. The
    // wait predicate runs arbitrary code under the mutex; treating the
    // wrapper as @unsafe matches the out-of-line definition.
    void wait_for_shutdown();

    // === Graceful Shutdown API ===

    /**
     * Add a shutdown hook to be called during graceful shutdown.
     * Hooks are called in order of registration during the CLOSING phase.
     * @param hook Callback function to execute during shutdown
     */
    // @unsafe - Stores the caller-supplied hook for later invocation. The
    // hook is opaque and will be called during shutdown; treating the
    // registration site as @unsafe matches the out-of-line definition.
    void add_shutdown_hook(ShutdownHook hook);

    /**
     * Stop accepting new connections but keep existing ones active.
     * Transitions to STOP_ACCEPTING phase.
     */
    // @unsafe - Calls PollThread::request_close
    void stop_accepting();

    /**
     * Wait for all in-flight requests to complete.
     * Transitions to DRAINING phase and waits until pending_requests_ reaches 0.
     * @param timeout_ms Maximum time to wait in milliseconds
     * @return true if all requests completed, false if timeout
     */
    // @unsafe - Uses std::atomic::load
    //
    // No default arg — the previous default of 30000ms is preserved
    // by `kDefaultDrainTimeoutMs` below, which callers can pass
    // explicitly. Removing the default lines the signature up with
    // the DSL form, which can't express default arguments.
    bool drain(uint64_t timeout_ms);

    /**
     * Perform graceful shutdown:
     * 1. Stop accepting new connections
     * 2. Drain existing requests (with timeout)
     * 3. Execute shutdown hooks
     * 4. Close all connections
     * @param drain_timeout_ms Timeout for drain phase
     */
    // @unsafe - Calls stop_accepting() and drain() which are unsafe
    //
    // No default arg — see `drain()` above. Callers should pass
    // `kDefaultDrainTimeoutMs` for the historical 30s default.
    void graceful_shutdown(uint64_t drain_timeout_ms);

    /**
     * Get current shutdown phase.
     */
    // @safe - Phase getter
    ShutdownPhase phase() const {
        return shutdown_phase_.get();
    }

    /**
     * Get count of pending (in-flight) requests.
     */
    // @safe - Atomic load is encapsulated in the inner @unsafe block.
    int pending_request_count() const {
        // @unsafe { std::atomic::load is not borrow-checked }
        { return pending_requests_->load(std::memory_order_relaxed); }
    }

    /**
     * Increment pending request count. Called when starting to process a request.
     */
    // @safe - Atomic fetch_add is encapsulated in the inner @unsafe block.
    void increment_pending() {
        // @unsafe { const_cast on Box ptr + std::atomic::fetch_add }
        {
            auto* pending_ptr = const_cast<std::atomic<int>*>(pending_requests_.get());
            pending_ptr->fetch_add(1, std::memory_order_relaxed);
        }
    }

    /**
     * Decrement pending request count. Called when request completes.
     */
    // @safe - Atomic fetch_sub is encapsulated in the inner @unsafe block.
    void decrement_pending() {
        // @unsafe { const_cast on Box ptr + std::atomic::fetch_sub }
        {
            auto* pending_ptr = const_cast<std::atomic<int>*>(pending_requests_.get());
            pending_ptr->fetch_sub(1, std::memory_order_relaxed);
        }
    }

    // @safe - Toggle dropping of internal heartbeat probe replies.
    void set_drop_heartbeat_replies(bool drop) {
        // @unsafe - std::atomic::store is currently modeled as non-safe.
        {
            auto* drop_ptr = const_cast<std::atomic<bool>*>(drop_heartbeat_replies_.get());
            drop_ptr->store(drop, std::memory_order_release);
        }
    }

    // @safe - Read drop-heartbeat toggle.
    bool drop_heartbeat_replies() const {
        // @unsafe - std::atomic::load is currently modeled as non-safe.
        { return drop_heartbeat_replies_->load(std::memory_order_acquire); }
    }

    /**
     * Get the server's unique instance ID.
     * This ID is generated on server startup and can be used by clients
     * to detect server restarts (the ID changes after restart).
     */
    // @safe - Simple getter
    uint64_t instance_id() const {
        return instance_id_;
    }

    // @safe - Iterate over all registered services
    // Useful for cleanup operations like flushing recorders at shutdown.
    // The callback receives a Service& reference; use dynamic_cast to get concrete type.
    // Must be called after start().
    template<typename F>
    void for_each_service(F&& callback) {
        // @unsafe
        {
        auto& ctx = ctx_.as_ref().unwrap();
        for (size_t i = 0; i < ctx->services.size(); ++i) {
            auto guard = ctx->services[i].borrow_mut();
            callback((*guard)->__get_service__());
        }
        }
    }

    // @safe - Option ops + Vec::size are @safe in the library.
    // Can be called before or after start().
    size_t service_count() const {
        if (ctx_.is_some()) {
            return ctx_.as_ref().unwrap()->services.size();
        }
        return pending_services_.size();
    }

    // Returns the server address (copy to avoid reference through Arc)
    // Must be called after start().
    std::string addr() const {
        return ctx_.as_ref().unwrap()->addr;
    }

    /**
     * Get the actual port the server is bound to.
     * Useful when starting with port 0 (ephemeral port assignment).
     * Must be called after start().
     * @return The bound port number, or -1 if not yet started
     */
    // @unsafe - Calls getsockname
    int get_bound_port() const;
};

#endif  // End of legacy Server class wrap

}  // export namespace rrr

// @safe - Implementation namespace. Out-of-class definitions inherit
// their per-method `// @unsafe` annotations from the matching
// declarations above. The anonymous-namespace `stat_*` helpers and
// other free-function impl details carry their own `// @unsafe`
// markers individually.
namespace rrr {

#ifdef RPC_STATISTICS

static const int g_stat_server_batching_size = 1000;
static int g_stat_server_batching[g_stat_server_batching_size];
static int g_stat_server_batching_idx;
static uint64_t g_stat_server_batching_report_time = 0;
static const uint64_t g_stat_server_batching_report_interval = 1000 * 1000 * 1000;

// @unsafe - Uses global mutable state (single-threaded context)
static void stat_server_batching(size_t batch) {
    g_stat_server_batching_idx = (g_stat_server_batching_idx + 1) % g_stat_server_batching_size;
    g_stat_server_batching[g_stat_server_batching_idx] = batch;
    uint64_t now = base::rdtsc();
    if (now - g_stat_server_batching_report_time > g_stat_server_batching_report_interval) {
        // do report
        int min = numeric_limits<int>::max();
        int max = 0;
        int sum_count = 0;
        int sum = 0;
        for (int i = 0; i < g_stat_server_batching_size; i++) {
            if (g_stat_server_batching[i] == 0) {
                continue;
            }
            if (g_stat_server_batching[i] > max) {
                max = g_stat_server_batching[i];
            }
            if (g_stat_server_batching[i] < min) {
                min = g_stat_server_batching[i];
            }
            sum += g_stat_server_batching[i];
            sum_count++;
            g_stat_server_batching[i] = 0;
        }
        double avg = double(sum) / sum_count;
        Log::info("* SERVER BATCHING: min=%d avg=%.1lf max=%d", min, avg, max);
        g_stat_server_batching_report_time = now;
    }
}

// rpc_id -> <count, cumulative>
static rusty::HashMap<i32, pair<Counter, Counter>> g_stat_rpc_counter;
static uint64_t g_stat_server_rpc_counting_report_time = 0;
static const uint64_t g_stat_server_rpc_counting_report_interval = 1000 * 1000 * 1000;

// @unsafe - Uses global mutable state (single-threaded context)
static void stat_server_rpc_counting(i32 rpc_id) {
    g_stat_rpc_counter[rpc_id].first.next();

    uint64_t now = base::rdtsc();
    if (now - g_stat_server_rpc_counting_report_time > g_stat_server_rpc_counting_report_interval) {
        // do report
        for (auto it: g_stat_rpc_counter) {
            i32 counted_rpc_id = it.first;
            i64 count = it.second.first.peek_next();
            it.second.first.reset();
            it.second.second.next(count);
            i64 cumulative = it.second.second.peek_next();
            Log::info("* RPC COUNT: id=%#08x count=%ld cumulative=%ld", counted_rpc_id, count, cumulative);
        }
        g_stat_server_rpc_counting_report_time = now;
    }
}

#endif // RPC_STATISTICS


// Static member definitions for missing RPC ID tracking
// SpinMutex wraps the unordered_set for thread-safe access
SpinMutex<rusty::HashSet<i32>> ServerConnection::rpc_id_missing_s{rusty::HashSet<i32>()};


// @safe - Initializes connection. 5g2: `socket_` field deleted;
// `socket` parameter is ignored (kept on the signature for source
// compatibility with existing call sites).
ServerConnection::ServerConnection(rusty::Arc<RpcServiceContext> ctx,
                                   int /*socket*/)
        : ctx_(std::move(ctx)), status_(CONNECTED) {
}

// @safe - Arc prevents premature destruction of RpcServiceContext
ServerConnection::~ServerConnection() {
    // Arc reference to RpcServiceContext is automatically released
}

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
void ServerConnection::bind_channel(ChannelConnectionProxy proxy) {
    if (!proxy) return;

    // Install callbacks BEFORE moving the proxy into the slot, so
    // the callbacks can capture a Weak<ServerConnection> without
    // holding the SpinMutex.
    WeakServerConnection weak_self = weak_self_;

    // @unsafe - lambda capture, channel proxy mutator
    proxy->set_on_frame([weak_self](const ChannelFrame& f) {
        auto sconn_opt = weak_self.upgrade();
        if (sconn_opt.is_none()) return;
        auto sconn = sconn_opt.unwrap();
        auto* mut_sconn = const_cast<ServerConnection*>(sconn.get());
        mut_sconn->decode_request_and_dispatch(f.payload, f.size);
    });
    // 5d: on_closed runs the existing close path so the connection
    // transitions to CLOSED. The channel-layer contract guarantees
    // on_closed fires exactly once; close() is itself idempotent
    // (status_ == CLOSED short-circuits).
    proxy->set_on_closed([weak_self](ChannelError /*reason*/) {
        auto sconn_opt = weak_self.upgrade();
        if (sconn_opt.is_none()) return;
        auto sconn = sconn_opt.unwrap();
        auto* mut_sconn = const_cast<ServerConnection*>(sconn.get());
        mut_sconn->close();
    });
    // 5d: on_error logs and force-closes. Per the channel-layer
    // contract, fatal errors are followed by on_closed, so the
    // close() here is also defensive — close() is idempotent.
    proxy->set_on_error([weak_self](ChannelError err,
                                    std::string_view message) {
        auto sconn_opt = weak_self.upgrade();
        if (sconn_opt.is_none()) return;
        auto sconn = sconn_opt.unwrap();
        Log_warn("rrr::ServerConnection: channel error %s: %.*s",
                 channel_error_to_string(err),
                 static_cast<int>(message.size()), message.data());
        auto* mut_sconn = const_cast<ServerConnection*>(sconn.get());
        mut_sconn->close();
    });

    // @unsafe { SpinMutex::lock + ChannelConnectionProxy move }
    {
        auto guard = channel_proxy_.lock().unwrap();
        *guard = rusty::Some(std::move(proxy));
    }
    channel_mode_.set(true);
}

// @unsafe - 5c: decode one channel-mode request frame and dispatch.
//
// Mirrors the per-packet body of `handle_read` minus the size-framed
// I/O loop: the channel layer has already stripped the 4-byte size
// prefix, so the body is `[xid:v64][rpc_id:i32][user-args]`.
void ServerConnection::decode_request_and_dispatch(
        const std::uint8_t* bytes, std::size_t size) {
    if (status_ == CLOSED) {
        return;
    }

    // Build a Request and copy the frame's bytes into its Marshal.
    // The channel-layer contract makes `bytes` valid only for the
    // duration of this callback, so we must copy before any code path
    // that may yield (e.g. `Fiber::create_run`).
    auto req_box = rusty::make_box<Request>();
    Request& req = *req_box;
    if (size > 0) {
        req.m.write(bytes, size);
    }

    // Header parse: xid + rpc_id. If the frame is malformed (less
    // than enough bytes for xid), drop it (no valid xid to reply
    // against). v64 is variable-length 1-8 bytes; an empty Marshal
    // means there's no xid.
    if (req.m.content_size() == 0) {
        Log_warn("rrr::ServerConnection: empty channel-mode request frame, "
                 "dropping");
        return;
    }
    v64 v_xid;
    req.m >> v_xid;
    req.xid = v_xid.get();
    req.attach_pending_guard(ctx_->pending_requests);

    if (req.m.content_size() < sizeof(i32)) {
        reply(req, EINVAL);
        return;
    }

    i32 rpc_id;
    req.m >> rpc_id;
    if (rpc_id == static_cast<i32>(kInternalHeartbeatRpcId)) {
        // @unsafe - std::atomic::load
        if (!ctx_->drop_heartbeat_replies->load(std::memory_order_acquire)) {
            reply(req, 0);
        }
        return;
    }

#ifdef RPC_STATISTICS
    stat_server_rpc_counting(rpc_id);
#endif // RPC_STATISTICS

    auto svc_index_opt = ctx_->rpc_to_service.get(rpc_id);
    if (svc_index_opt.is_none()) {
        bool surpress_warning = false;
        {
            auto guard = rpc_id_missing_s.lock().unwrap();
            if (!guard->contains(rpc_id)) {
                guard->insert(rpc_id);
            } else {
                surpress_warning = true;
            }
        }
        if (!surpress_warning) {
            Log_warn("rrr::ServerConnection: no handler for rpc_id = %d "
                     "(channel-mode dispatch)", rpc_id);
        }
        reply(req, ENOENT);
        return;
    }

    size_t svc_index = svc_index_opt.unwrap();
    auto weak_this = weak_self_;
    if (ctx_->fast_rpc_ids.contains(rpc_id)) {
        // Fast inline dispatch — no fiber spawn.
        auto guard = ctx_->services[svc_index].borrow_mut();
        (*guard)->__dispatch__(rpc_id, std::move(req_box), weak_this);
    } else {
        // Slow path — spawn a fiber so the handler can yield (e.g.
        // for nested RPC calls). Capture an Arc<RpcServiceContext>
        // clone so the fiber stays valid even if the connection is
        // closed mid-flight.
        auto ctx = ctx_.clone();
        Fiber::create_run([ctx, svc_index, rpc_id,
                           req = std::move(req_box),
                           weak_this]() mutable {
            auto guard = ctx->services[svc_index].borrow_mut();
            (*guard)->__dispatch__(rpc_id, std::move(req), weak_this);
        }, __FILE__, __LINE__);
    }
}

// @unsafe - 5b: dispatch a reply-frame body through the bound proxy.
//
// Locks the SpinMutex briefly to extract the proxy pointer, then
// drops the guard so the actual `send_frame` happens without holding
// the lock (the proxy's `send_frame` is internally thread-safe per
// the channel-layer contract). Errors are observable via the
// proxy's installed `on_error` / `on_closed` callbacks; the return
// value is intentionally discarded — the RPC layer mirrors the
// legacy fd path's behavior of not surfacing send-side errors from
// `reply()`.
void ServerConnection::dispatch_response_frame_via_channel(
        const std::uint8_t* bytes, std::size_t size) const {
    ChannelConnectionBase* conn = nullptr;
    // @unsafe { SpinMutex::lock + Box::get + raw pointer extraction }
    {
        auto guard = channel_proxy_.lock().unwrap();
        if (guard->is_none()) {
            Log_warn("rrr::ServerConnection::dispatch_response_frame_via_channel: "
                     "channel mode flipped on but proxy is unbound (race?). "
                     "Dropping reply.");
            return;
        }
        conn = guard->as_ref().unwrap().get();
    }
    ChannelFrame frame{bytes, size};
    // @unsafe - virtual method dispatch through ChannelConnectionBase*
    (void)conn->send_frame(frame);
}

// @unsafe - Executes callback inline for API compatibility.
int ServerConnection::run_async(rusty::Function<void()> f) {
  if (!f) {
    Log_warn("rrr::ServerConnection::run_async called with empty callback");
    return EINVAL;
  }
  f();
  return 0;
}

// @safe - Closes connection.
//
// 5g2: legacy `::close(socket_)` block deleted (the field is gone).
// Channel proxy close is the only fd-tearing-down path.
//
// The channel-layer proxy.close() is idempotent and safe under
// recursive entry: close() may be called from `on_closed` which 5d
// installs, and 5d's on_closed → close() → proxy.close() →
// (idempotent) on_closed re-fires without effect.
void ServerConnection::close() {
    if (status_ == CONNECTED) {
        status_ = CLOSED;
        Log_debug("server@%s close ServerConnection",
                  ctx_->addr.c_str());
        // Tear down the channel proxy. Idempotent per channel-layer contract.
        // @unsafe { SpinMutex::lock + Box::get + virtual dispatch }
        {
            auto guard = channel_proxy_.lock().unwrap();
            if (guard->is_some()) {
                auto* conn = guard->as_ref().unwrap().get();
                conn->close();
            }
        }
    }
}

// @unsafe - Executes callback inline for API compatibility.
int DeferredReply::run_async(rusty::Function<void()> f) {
    if (!f) {
        Log_warn("rrr::DeferredReply::run_async called with empty callback");
        return EINVAL;
    }
    f();
    return 0;
}

// ============================================================================
// Legacy hand-written Server out-of-line method definitions.
//
// These out-of-line defs are kept under #if 0 during DSL migration —
// the DSL block emits the matching method bodies.
#if 0

// @safe - Constructs server with PollThread
// ctx_ starts as None; created in start() after all registrations
Server::Server(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker /* =... */) {
    if (poll_thread_worker.is_none()) {  // Check if Option is None
        poll_thread_ = rusty::Some(PollThread::create());
    } else {
        poll_thread_ = std::move(poll_thread_worker);
    }

    // Generate unique instance ID for restart detection
    // Combines timestamp, random component, and process ID for uniqueness
    // @unsafe - std::random_device may use system entropy sources
    {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        uint64_t time_component = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());

        std::random_device rd;
        uint64_t random_component = static_cast<uint64_t>(rd()) << 32 |
                                    static_cast<uint64_t>(rd());

        uint64_t pid_component =
            static_cast<uint64_t>(rusty::sys::process::getpid()) << 48;

        // Mix components with XOR for final ID
        instance_id_ = (time_component ^ random_component ^ pid_component)
            & static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        if (instance_id_ == 0) {
            instance_id_ = 1;
        }

        Log_debug("Server: generated instance_id=%lu", instance_id_);
    }
}

// @safe - Destroys server and requests close for all connections
// Arc<RpcServiceContext> ensures services live until all connections are done
Server::~Server() noexcept {
    // 5g1: legacy `server_listener_` cleanup branch deleted (the
    // class is gone). Channel-mode listener cleanup follows.

    // 5e/5f: tear down the channel-mode listener (if bound). The
    // proxy's close() is idempotent; dropping the Box afterwards
    // releases the proxy's underlying TcpListener (or other backend),
    // which closes its listening fd via its destructor.
    //
    // Note: the listener may have just been registered with the
    // poll thread (the channel layer auto-registers via add_proxy
    // inside `listen()`). Calling close() directly from the user
    // thread races against the poll thread's pending CmdAddPollable
    // — by the time the poll thread reads `fd()`, it could already
    // be -1, tripping `Epoll::Add`'s fd>=0 verify. We schedule the
    // close on the poll thread via a OneTimeJob so commands are
    // processed in order (mirrors `Client::close`'s 4g3c3 fix). The
    // proxy holds an Arc<TcpListener> so the close() call inside
    // the job sees a live listener even though the original Box has
    // been moved into the lambda.
    if (channel_listener_.is_some()) {
        // Move the Box directly into the lambda — OneTimeJob now
        // uses rusty::Function (move-only) under the hood (see
        // L5g), so move-only captures are fine and the prior
        // std::shared_ptr wrap that existed only to satisfy
        // std::function's copyable requirement is no longer needed.
        auto close_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob::new_(
            [listener_box = std::move(channel_listener_).unwrap()]() mutable {
                listener_box->close();
            }));
        auto close_job_base = rusty::Arc<Job>(close_job);
        poll_thread_.as_ref().unwrap()->add(std::move(close_job_base));
        channel_listener_ = rusty::None;
    }
    // 5f: actively close each accepted channel-mode ServerConnection
    // before dropping the Arcs. ServerConnection::close() drives the
    // bound channel proxy's close() which closes the underlying
    // TcpConnection's fd, so the peer (Client) sees EOF immediately
    // — without this active close, the TcpConnection's other Arc
    // (held by the poll thread's pollable proxy) would keep it alive
    // and the client would only notice on its next request attempt.
    // close() is idempotent (gated on status_ == CONNECTED), so
    // already-closed connections are no-ops.
    {
        auto guard = channel_sconns_.lock().unwrap();
        for (auto& sconn : *guard) {
            // @unsafe - const_cast through Arc::get for close()
            auto& mut_sconn = const_cast<ServerConnection&>(*sconn.get());
            mut_sconn.close();
        }
        // Drop the Arcs. The ChannelConnectionProxy stored inside
        // each ServerConnection drops along with the ServerConnection,
        // releasing the underlying TcpConnection's other refcount;
        // that fd is then closed by the poll-thread cleanup chain.
        guard->clear();
    }

    // No need to wait for connections - Arc<RpcServiceContext> ensures services
    // stay alive until the last ServerConnection drops its Arc reference.
    // Services are automatically cleaned up when last Arc is dropped.
    ctx_ = rusty::None;
}

// 5g1: legacy `ServerListener` implementation deleted. The
// channel layer's `TcpListener` handles bind/listen/accept; see
// `Server::start` (channel-mode listen path) and
// `src/rrr/rpc/tcp_channel.cpp`.

// @unsafe - Starts server listening (pointer dereference)
int Server::start(const char* bind_addr) {
  if (!bind_addr) {
    Log_error("rrr::Server::start: bind_addr is NULL!");
    return -1;
  }

  // Wrap each service in RefCell for interior mutability.
  rusty::Vec<rusty::RefCell<ServiceProxy>> wrapped_services;
  for (size_t i = 0; i < pending_services_.size(); ++i) {
    wrapped_services.push(rusty::RefCell<ServiceProxy>(std::move(pending_services_[i])));
  }

  // Create immutable RpcServiceContext from pending registration data
  std::string addr_str(bind_addr, strlen(bind_addr));
  ctx_ = rusty::Some(rusty::Arc<RpcServiceContext>::make(
      std::move(pending_rpc_to_service_),
      std::move(pending_fast_rpc_ids_),
      std::move(wrapped_services),
      addr_str,
      pending_requests_,
      drop_heartbeat_replies_,
      instance_id_));

  // auto-install default TcpFactory.
  //
  // If the caller hasn't bound a factory via `set_channel_factory(...)`,
  // construct one wrapping a default `TcpFactory(poll_thread_)` so the
  // channel-mode path below is unconditional. Mirrors the client-side
  // post-4g4 pattern (`Client::connect` auto-installs a default TCP
  // factory). The legacy `ServerListener` socket path remains in
  // `server.cpp` for now but is unreachable from `Server::start` — 5g
  // deletes it.
  if (!is_channel_factory_bound()) {
    auto tcp_factory = rusty::Arc<TcpFactory>::make(
        poll_thread_.as_ref().unwrap().clone());
    set_channel_factory(make_tcp_factory_proxy(std::move(tcp_factory)));
  }

  // channel-mode listen path.
  //
  // Routes `start(addr)` through `factory->make_listener() ->
  // listener.set_on_accept(...) -> listener->listen(addr)` instead
  // of the legacy `ServerListener` socket path. The on_accept
  // callback constructs a `ServerConnection` bound to the new
  // channel proxy (via 5b/5c/5d's `bind_channel(...)`) and parks
  // it in `channel_sconns_` so its lifetime is tied to the
  // `Server` (not the on_accept stack frame).
  if (is_channel_factory_bound()) {
    rusty::Option<ChannelListenerProxy> listener_opt;
    // @unsafe { Box::get + proxy method dispatch }
    {
      auto* factory = channel_factory_.as_ref().unwrap().get();
      listener_opt = factory->make_listener();
    }
    if (listener_opt.is_none()) {
      Log_error("rrr::Server::start: factory->make_listener() returned a "
                "null proxy (factory backend=%s)",
                /*best-effort name*/ "unknown");
      ctx_ = rusty::None;
      return -1;
    }
    ChannelListenerProxy listener = std::move(listener_opt).unwrap();

    // Capture for the on_accept lambda. `this` outlives the listener
    // because Server owns `channel_listener_` (and `channel_sconns_`)
    // — destroying Server drops the listener which in turn waits for
    // any in-flight on_accept callback to complete (channel-layer
    // contract).
    Server* server_ptr = this;
    rusty::Arc<RpcServiceContext> ctx_arc = ctx_.as_ref().unwrap().clone();

    // @unsafe - lambda capture, channel proxy mutator
    listener->set_on_accept([server_ptr, ctx_arc](
                                ChannelConnectionProxy conn_proxy) {
      if (!conn_proxy) return;
      auto sconn = rusty::Arc<ServerConnection>::make(
          ctx_arc.clone(), /*socket=*/-1);
      auto& mut_sconn = const_cast<ServerConnection&>(*sconn.get());
      // Wire `weak_self_` so bind_channel's installed callbacks can
      // upgrade to a strong ref.
      mut_sconn.install_self_weak_for_testing(rusty::sync::downgrade(sconn));
      mut_sconn.bind_channel(std::move(conn_proxy));
      // Park the Arc on the server so the on_frame / on_closed
      // callbacks (which only hold a Weak) keep observing a live
      // connection. SpinMutex::lock + Vec::push are both @safe.
      {
        auto guard = server_ptr->channel_sconns_.lock().unwrap();
        guard->push(std::move(sconn));
      }
    });
    listener->set_on_error([](ChannelError err, std::string_view msg) {
      Log_warn("rrr::Server: channel listener error %s: %.*s",
               channel_error_to_string(err),
               static_cast<int>(msg.size()), msg.data());
    });

    ChannelError listen_err = listener->listen(std::string_view(bind_addr));
    if (listen_err != ChannelError::None) {
      Log_error("rrr::Server::start: channel listener failed to bind %s: %s",
                bind_addr, channel_error_to_string(listen_err));
      ctx_ = rusty::None;
      return -1;
    }

    // Park the listener on the server so its lifetime matches Server's.
    // @unsafe { Option assignment }
    channel_listener_ = rusty::Some(std::move(listener));
    return 0;
  }

  // 5g1: legacy `ServerListener` fallback deleted. The
  // `is_channel_factory_bound()` guard above is unconditionally true
  // post-5f (auto-installed default `TcpFactory`), so this fallthrough
  // is unreachable. We `verify(false)` defensively in case a future
  // refactor reintroduces a path that bypasses the auto-install.
  verify(false);
  return -1;
}

// @unsafe - Unregisters RPC mapping from pending map (must be called before start())
void Server::unreg(i32 rpc_id) {
    pending_rpc_to_service_.remove(rpc_id);
    pending_fast_rpc_ids_.remove(rpc_id);
}

// @unsafe - Signals shutdown to waiting threads
void Server::do_shutdown() {
    Log_debug("Server::do_shutdown");
    {
        auto guard = shutdown_state_.lock().unwrap();
        guard->shutdown = true;
    }
    shutdown_cond_->notify_all();
}

// @unsafe - Blocks until shutdown is signaled
void Server::wait_for_shutdown() {
    Log_debug("Server::wait_for_shutdown");
    auto guard = shutdown_state_.lock().unwrap();
    guard = shutdown_cond_->wait_while(std::move(guard),
        [](ShutdownState& s) { return !s.shutdown; }).unwrap();
    Log_debug("Server::wait_for_shutdown - done");
}

// === Graceful Shutdown Implementation ===

// @unsafe - Thread-safe hook registration
void Server::add_shutdown_hook(ShutdownHook hook) {
    auto guard = shutdown_hooks_.lock().unwrap();
    guard->push(std::move(hook));
}

// @unsafe - Calls PollThread::request_close
void Server::stop_accepting() {
    if (shutdown_phase_.get() != ShutdownPhase::RUNNING) {
        Log_debug("Server::stop_accepting: already in phase %s",
                  shutdown_phase_to_string(shutdown_phase_.get()));
        return;
    }

    Log_info("Server::stop_accepting: transitioning to STOP_ACCEPTING");
    shutdown_phase_.set(ShutdownPhase::STOP_ACCEPTING);

    // 5g1: legacy `server_listener_` close branch deleted (the
    // class is gone). Channel-mode listener close follows.

    // 5e: close the channel-mode listener if bound. The proxy's
    // close() is idempotent and refuses further accepts; existing
    // accepted connections in `channel_sconns_` are unaffected (they
    // continue to serve in-flight requests until drained / shut down).
    if (channel_listener_.is_some()) {
        // @unsafe { Box::get + virtual dispatch }
        {
            auto* listener = channel_listener_.as_ref().unwrap().get();
            listener->close();
        }
        Log_info("Server::stop_accepting: channel listener closed, "
                 "no longer accepting connections");
    }
    // Note: stop_accepting() is typically called well after the
    // listener has been registered with the poll thread (via the
    // channel-layer's auto-register in TcpListener::listen), so the
    // direct `proxy->close()` above doesn't race with a pending
    // `CmdAddPollable`. ~Server's teardown takes a more defensive
    // approach (scheduling the close via a OneTimeJob) because in
    // tests the listener may have just been registered.
}

// @unsafe - Uses std::atomic::load
bool Server::drain(uint64_t timeout_ms) {
    auto current_phase = shutdown_phase_.get();
    if (current_phase != ShutdownPhase::RUNNING &&
        current_phase != ShutdownPhase::STOP_ACCEPTING) {
        Log_debug("Server::drain: already in phase %s",
                  shutdown_phase_to_string(current_phase));
        return pending_requests_->load(std::memory_order_relaxed) == 0;
    }

    Log_info("Server::drain: transitioning to DRAINING, pending=%d",
             pending_requests_->load(std::memory_order_relaxed));
    shutdown_phase_.set(ShutdownPhase::DRAINING);

    // Wait for pending requests with timeout. Clock + sleep flow
    // through rusty::sys::time::* (each @safe with an inner @unsafe block
    // around the libc call).
    const std::uint64_t start_us =
        rusty::sys::time::clock_monotonic_us();
    const std::uint64_t timeout_us = timeout_ms * 1000;
    while (pending_requests_->load(std::memory_order_relaxed) > 0) {
        const std::uint64_t elapsed_us =
            rusty::sys::time::clock_monotonic_us() - start_us;
        if (elapsed_us >= timeout_us) {
            Log_warn("Server::drain: timeout after %lu ms, pending=%d",
                     timeout_ms, pending_requests_->load(std::memory_order_relaxed));
            return false;
        }

        // Brief sleep to avoid busy-waiting.
        rusty::sys::time::sleep_us(1000);  // 1ms
    }

    Log_info("Server::drain: completed, all requests drained");
    return true;
}

// @unsafe - Calls stop_accepting() and drain() which are unsafe
void Server::graceful_shutdown(uint64_t drain_timeout_ms) {
    Log_info("Server::graceful_shutdown: starting graceful shutdown");

    // Stop accepting new connections
    stop_accepting();  // @unsafe

    // Drain existing requests
    bool drained = drain(drain_timeout_ms);  // @unsafe
    if (!drained) {
        Log_warn("Server::graceful_shutdown: drain timed out, proceeding with shutdown");
    }

    // Execute shutdown hooks
    Log_info("Server::graceful_shutdown: transitioning to CLOSING, executing hooks");
    shutdown_phase_.set(ShutdownPhase::CLOSING);

    {
        auto guard = shutdown_hooks_.lock().unwrap();
        for (auto& hook : *guard) {
            // @unsafe - callback execution
            {
                try {
                    hook();
                } catch (const std::exception& e) {
                    Log_error("Server::graceful_shutdown: hook threw exception: %s", e.what());
                } catch (...) {
                    Log_error("Server::graceful_shutdown: hook threw unknown exception");
                }
            }
        }
    }

    // Close all connections (destructor handles this)
    // Signal shutdown to any waiting threads
    do_shutdown();

    Log_info("Server::graceful_shutdown: transitioning to STOPPED");
    shutdown_phase_.set(ShutdownPhase::STOPPED);
}

// @safe - 5g1: re-implemented atop the channel-layer's
// `ChannelListenerProxy::local_address()` (which `TcpListener` fills
// in via `getsockname` after a successful `bind(2)`). The returned
// string is `host:port`; we parse out the port suffix.
int Server::get_bound_port() const {
    if (channel_listener_.is_none()) {
        return -1;
    }
    std::string local;
    // @unsafe { Box::get + virtual dispatch }
    {
        auto* listener = channel_listener_.as_ref().unwrap().get();
        local = listener->local_address();
    }
    auto colon = local.rfind(':');
    if (colon == std::string::npos) {
        Log_error("Server::get_bound_port: malformed local_address %s",
                  local.c_str());
        return -1;
    }
    try {
        int port = std::stoi(local.substr(colon + 1));
        return port;
    } catch (const std::exception&) {
        Log_error("Server::get_bound_port: failed to parse port from %s",
                  local.c_str());
        return -1;
    }
}

#endif  // End of legacy Server out-of-line method defs wrap

// @unsafe - The full start() body. Lifted verbatim from the legacy
// out-of-line method, only with `self->` instead of bare member
// names. Kept out of the DSL because on_accept / on_error lambda
// captures (capturing `Server*` + `Arc<RpcServiceContext>`), the
// `make_listener` factory call sequence, and the channel-error /
// listen-error branching don't translate cleanly to the DSL grammar.
int32_t server_start_impl(Server& self, const int8_t* bind_addr_raw) {
    const char* bind_addr = server_dsl_addr_to_cstr(bind_addr_raw);
    if (!bind_addr) {
        Log_error("rrr::Server::start: bind_addr is NULL!");
        return -1;
    }

    // Wrap each service in RefCell for interior mutability.
    rusty::Vec<rusty::RefCell<ServiceProxy>> wrapped_services;
    for (size_t i = 0; i < self.pending_services_field.size(); ++i) {
        wrapped_services.push(rusty::RefCell<ServiceProxy>(
            std::move(self.pending_services_field[i])));
    }

    // Create immutable RpcServiceContext from pending registration data
    std::string addr_str(bind_addr, strlen(bind_addr));
    self.ctx_field = rusty::Some(rusty::Arc<RpcServiceContext>::make(
        std::move(self.pending_rpc_to_service_field),
        std::move(self.pending_fast_rpc_ids_field),
        std::move(wrapped_services),
        addr_str,
        self.pending_requests_field,
        self.drop_heartbeat_replies_field,
        self.instance_id_field));

    // auto-install default TcpFactory if the caller hasn't bound one.
    if (!self.is_channel_factory_bound()) {
        auto tcp_factory = rusty::Arc<TcpFactory>::make(
            self.poll_thread_field.as_ref().unwrap().clone());
        self.set_channel_factory(make_tcp_factory_proxy(std::move(tcp_factory)));
    }

    if (self.is_channel_factory_bound()) {
        rusty::Option<ChannelListenerProxy> listener_opt;
        // @unsafe { Box::get + proxy method dispatch }
        {
            auto* factory = self.channel_factory_field.as_ref().unwrap().get();
            listener_opt = factory->make_listener();
        }
        if (listener_opt.is_none()) {
            Log_error("rrr::Server::start: factory->make_listener() returned a "
                      "null proxy (factory backend=%s)",
                      "unknown");
            self.ctx_field = rusty::None;
            return -1;
        }
        ChannelListenerProxy listener = std::move(listener_opt).unwrap();

        Server* server_ptr = &self;
        rusty::Arc<RpcServiceContext> ctx_arc =
            self.ctx_field.as_ref().unwrap().clone();

        // @unsafe - lambda capture, channel proxy mutator
        listener->set_on_accept([server_ptr, ctx_arc](
                                    ChannelConnectionProxy conn_proxy) {
            if (!conn_proxy) return;
            auto sconn = rusty::Arc<ServerConnection>::make(
                ctx_arc.clone(), /*socket=*/-1);
            auto& mut_sconn = const_cast<ServerConnection&>(*sconn.get());
            mut_sconn.install_self_weak_for_testing(rusty::sync::downgrade(sconn));
            mut_sconn.bind_channel(std::move(conn_proxy));
            {
                auto guard = server_ptr->channel_sconns_field.lock().unwrap();
                guard->push(std::move(sconn));
            }
        });
        listener->set_on_error([](ChannelError err, std::string_view msg) {
            Log_warn("rrr::Server: channel listener error %s: %.*s",
                     channel_error_to_string(err),
                     static_cast<int>(msg.size()), msg.data());
        });

        ChannelError listen_err = listener->listen(std::string_view(bind_addr));
        if (listen_err != ChannelError::None) {
            Log_error("rrr::Server::start: channel listener failed to bind %s: %s",
                      bind_addr, channel_error_to_string(listen_err));
            self.ctx_field = rusty::None;
            return -1;
        }

        self.channel_listener_field = rusty::Some(std::move(listener));
        return 0;
    }

    verify(false);
    return -1;
}

// @unsafe - The destructor body. Lifted from the legacy `~Server()`
// out-of-line, only with `self.field` instead of bare member names.
// Kept out of the DSL because the OneTimeJob lambda init-capture
// (`[listener_box = std::move(...).unwrap()]`) and the channel-
// connection iteration through SpinMutex guard rely on auto-deref
// quirks that don't emit cleanly through the DSL pipeline.
void server_drop_impl(Server& self) {
    // 5e/5f: tear down the channel-mode listener (if bound). The
    // proxy's close() is idempotent; dropping the Box afterwards
    // releases the proxy's underlying TcpListener (or other
    // backend), which closes its listening fd via its destructor.
    // We schedule the close on the poll thread via a OneTimeJob so
    // commands are processed in order (mirrors `Client::close`'s
    // 4g3c3 fix).
    if (self.channel_listener_field.is_some()) {
        auto close_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob::new_(
            [listener_box = std::move(self.channel_listener_field).unwrap()]() mutable {
                listener_box->close();
            }));
        auto close_job_base = rusty::Arc<Job>(close_job);
        self.poll_thread_field.as_ref().unwrap()->add(std::move(close_job_base));
        self.channel_listener_field = rusty::None;
    }
    // 5f: actively close each accepted channel-mode ServerConnection
    // before dropping the Arcs. ServerConnection::close() drives
    // the bound channel proxy's close() which closes the underlying
    // TcpConnection's fd, so the peer (Client) sees EOF
    // immediately. close() is idempotent.
    {
        auto guard = self.channel_sconns_field.lock().unwrap();
        for (auto& sconn : *guard) {
            // @unsafe - const_cast through Arc::get for close()
            auto& mut_sconn = const_cast<ServerConnection&>(*sconn.get());
            mut_sconn.close();
        }
        guard->clear();
    }
    // No need to wait for connections - Arc<RpcServiceContext>
    // ensures services stay alive until the last ServerConnection
    // drops its Arc reference.
    self.ctx_field = rusty::None;
}

}  // namespace rrr
