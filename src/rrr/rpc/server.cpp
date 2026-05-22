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
    mutable SpinMutex<rusty::Option<rusty::Box<ChannelConnectionProxy>>>
        channel_proxy_{rusty::Option<rusty::Box<ChannelConnectionProxy>>(rusty::None)};
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

    // @safe - 5g2: ServerConnection no longer owns an fd. Always
    // returns -1; retained only for ABI compatibility with the
    // PollableProxy facade.
    int fd() const {
        return -1;
    }

    // @safe - Returns poll mode based on output buffer
    // Uses const_cast for interior mutability (SpinLock marked as external)
    int poll_mode() const;

    // @safe - Returns buffered input/output bytes for diagnostics.
    size_t content_size();

    // @safe - Writes buffered data to socket
    // SAFETY: Protected by output spinlock (SpinLock marked as external)
    // Returns new poll mode, or MODE_NO_CHANGE if no update needed
    int handle_write();

    // @safe - Reads and processes RPC requests
    // Memory-safe: Uses Box for request ownership, virtual dispatch for handlers,
    // Arc for shared context, RefCell for interior mutability, Fiber::create_run for async.
    bool handle_read();  // Batching mode: reads ALL available requests

    // @safe - Error handler
    void handle_error();

    // @safe - 5g2: `pending_write_update_` field deleted; the
    // channel layer's `TcpConnection` manages its own
    // pending-write tracking. Always returns false; retained for
    // ABI compatibility with the PollableProxy facade.
    bool check_pending_write_update() const {
        return false;
    }

    // @safe - Check if connection was closed
    // Called by poll loop to detect and remove closed connections
    bool is_closed() const {
        return status_ == CLOSED;
    }

    // @safe - Explicit server-side no-op (kept for API compatibility).
    void handle_free();

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
    struct ShutdownState { bool shutdown = false; };
    rusty::Mutex<ShutdownState> shutdown_state_{ShutdownState{}};
    rusty::Condvar shutdown_cond_;

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
    rusty::Option<rusty::Box<ChannelFactoryProxy>> channel_factory_{rusty::None};

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
    rusty::Option<rusty::Box<ChannelListenerProxy>> channel_listener_{rusty::None};
    mutable SpinMutex<rusty::Vec<rusty::Arc<ServerConnection>>>
        channel_sconns_{rusty::Vec<rusty::Arc<ServerConnection>>()};

public:
    // @safe - Creates server with optional PollThread
    // SAFETY: Shared ownership of PollThread via Arc<Mutex<>>
    Server(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker = rusty::None);
    // @safe - Destroys server and requests close for all connections
    // SAFETY: Arc<RpcServiceContext> ensures services live until all connections are done
    virtual ~Server() noexcept override;

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
    // @unsafe - Records the factory under Box+Option interior storage.
    void set_channel_factory(ChannelFactoryProxy factory) {
        if (!factory) return;
        // @unsafe { make_box + ChannelFactoryProxy move }
        channel_factory_ = rusty::Some(
            rusty::make_box<ChannelFactoryProxy>(std::move(factory)));
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
    template <ServiceLike T>
      requires (!std::derived_from<T, Service>)
    void reg_service(rusty::Box<T> svc) {
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

    // @unsafe - Blocks the caller on `shutdown_cond_.wait_while(...)`. The
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
     * @param timeout_ms Maximum time to wait in milliseconds (default: 30 seconds)
     * @return true if all requests completed, false if timeout
     */
    // @unsafe - Uses std::atomic::load
    bool drain(uint64_t timeout_ms = 30000);

    /**
     * Perform graceful shutdown:
     * 1. Stop accepting new connections
     * 2. Drain existing requests (with timeout)
     * 3. Execute shutdown hooks
     * 4. Close all connections
     * @param drain_timeout_ms Timeout for drain phase (default: 30 seconds)
     */
    // @unsafe - Calls stop_accepting() and drain() which are unsafe
    void graceful_shutdown(uint64_t drain_timeout_ms = 30000);

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

    // @unsafe { SpinMutex::lock + make_box + ChannelConnectionProxy move }
    {
        auto guard = channel_proxy_.lock().unwrap();
        *guard = rusty::Some(
            rusty::make_box<ChannelConnectionProxy>(std::move(proxy)));
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
    ChannelConnectionProxy* proxy = nullptr;
    // @unsafe { SpinMutex::lock + raw pointer extraction }
    {
        auto guard = channel_proxy_.lock().unwrap();
        if (guard->is_none()) {
            Log_warn("rrr::ServerConnection::dispatch_response_frame_via_channel: "
                     "channel mode flipped on but proxy is unbound (race?). "
                     "Dropping reply.");
            return;
        }
        proxy = const_cast<ChannelConnectionProxy*>(
            guard->as_ref().unwrap().get());
    }
    ChannelFrame frame{bytes, size};
    // @unsafe - virtual method dispatch through Box<ChannelConnectionBase>
    (void)(*proxy)->send_frame(frame);
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

// @safe - 5g2: stubbed. The legacy `in_`/`out_` Marshal buffers are
// gone; channel mode buffers frames inside `TcpConnection`. Returns
// 0 for ABI compatibility with PollableProxy facade conformance.
size_t ServerConnection::content_size() {
    return 0;
}

// @unsafe - Explicit no-op for server connection API compatibility.
void ServerConnection::handle_free() {
    Log_warn("rrr::ServerConnection::handle_free() is a no-op on server connections");
}

// @safe - 5g2: stubbed. ServerConnection no longer implements the
// Pollable role — the channel layer's `TcpConnection` owns the fd
// and drives `handle_read`/`handle_write`/`handle_error` on its own
// pollable proxy. Inbound dispatch happens via the on_frame
// callback installed in `bind_channel(...)` (5c). This stub remains
// only for ABI compatibility (PollableProxy facade conformance);
// the body is unreachable from production paths.
bool ServerConnection::handle_read() {
    return false;
}

// @safe - 5g2: stubbed (Pollable facade ABI only). Channel mode's
// outbound writes go through `proxy->send_frame(...)` directly; no
// `out_` Marshal buffer to drain.
int ServerConnection::handle_write() {
    return PollMode::NO_CHANGE;
}

// @safe - Error handler. In channel mode, the bound channel proxy's
// `on_error` callback (wired in 5d) calls `close()` directly; this
// remains for legacy callers and as a defensive entry point.
void ServerConnection::handle_error() {
    this->close();
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
        // @unsafe
        {
            auto guard = channel_proxy_.lock().unwrap();
            if (guard->is_some()) {
                auto* proxy = const_cast<ChannelConnectionProxy*>(
                    guard->as_ref().unwrap().get());
                (*proxy)->close();
            }
        }
    }
}

// @safe - 5g2: stubbed. The channel layer's `TcpConnection` manages
// its own poll-mode state via `pending_write_update_` on the
// TcpConnection itself; this `ServerConnection` Pollable accessor
// is unreachable from production but kept for ABI compatibility.
int ServerConnection::poll_mode() const {
    return PollMode::READ;
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

        uint64_t pid_component = static_cast<uint64_t>(getpid()) << 48;

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
        auto close_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob(
            [listener_box = std::move(channel_listener_).unwrap()]() mutable {
                (*listener_box)->close();
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
    ChannelListenerProxy listener;
    // @unsafe { Box<ChannelFactoryProxy>::get + proxy method dispatch }
    {
      auto* factory = const_cast<ChannelFactoryProxy*>(
          channel_factory_.as_ref().unwrap().get());
      listener = (*factory)->make_listener();
    }
    if (!listener) {
      Log_error("rrr::Server::start: factory->make_listener() returned a "
                "null proxy (factory backend=%s)",
                /*best-effort name*/ "unknown");
      ctx_ = rusty::None;
      return -1;
    }

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
      // connection.
      // @unsafe { SpinMutex::lock + Vec::push }
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
    // @unsafe { make_box + Option assignment }
    channel_listener_ = rusty::Some(
        rusty::make_box<ChannelListenerProxy>(std::move(listener)));
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
    shutdown_cond_.notify_all();
}

// @unsafe - Blocks until shutdown is signaled
void Server::wait_for_shutdown() {
    Log_debug("Server::wait_for_shutdown");
    auto guard = shutdown_state_.lock().unwrap();
    guard = shutdown_cond_.wait_while(std::move(guard),
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
        // @unsafe { Box::get + ChannelListenerProxy method dispatch }
        {
            auto* listener = const_cast<ChannelListenerProxy*>(
                channel_listener_.as_ref().unwrap().get());
            (*listener)->close();
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

    // Wait for pending requests with timeout
    // @unsafe - uses std::chrono
    {
        auto start = std::chrono::steady_clock::now();
        auto timeout = std::chrono::milliseconds(timeout_ms);

        while (pending_requests_->load(std::memory_order_relaxed) > 0) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed >= timeout) {
                Log_warn("Server::drain: timeout after %lu ms, pending=%d",
                         timeout_ms, pending_requests_->load(std::memory_order_relaxed));
                return false;
            }

            // Brief sleep to avoid busy-waiting
            // @unsafe - usleep syscall
            usleep(1000);  // 1ms
        }
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
    // @unsafe { Box<ChannelListenerProxy>::get + proxy method dispatch }
    {
        auto* listener = const_cast<ChannelListenerProxy*>(
            channel_listener_.as_ref().unwrap().get());
        local = (*listener)->local_address();
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

}  // namespace rrr
