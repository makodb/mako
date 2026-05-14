#pragma once

// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
// @unsafe - RPC server module uses mutable spinlocks
#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/cell.hpp>
#include <rusty/function.hpp>
#include <rusty/option.hpp>
#include <rusty/unsafe_cell.hpp>
#include <rusty/vec.hpp>
#include <rusty/rusty.hpp>  // For rusty::Mutex, rusty::Condvar

// 5g3: socket-path system headers (`<pthread.h>`, `<sys/socket.h>`,
// `<netdb.h>`) removed — no `bind(2)` / `accept(2)` / `getaddrinfo`
// / `pthread_*` references remain in the RPC server header. The
// channel layer's `TcpListener` / `TcpConnection` own those
// syscalls.


// External safety annotations for system functions and STL operations.
// Note: Marshal, Log, SpinLock, PollThread, Reactor, Fiber, and
// rusty-cpp types now have in-place annotations in their respective
// headers.
//
// 5g3: the legacy `bind/listen/accept/usleep` system-call annotations
// were retired along with the legacy `ServerListener` socket path
// (5g1). The channel layer's `tcp_channel.{hpp,cpp}` carries its own
// annotations for the relevant syscalls.
//
// @external: {
//   operator!=: [safe]
//   operator==: [safe]
//   std::vector::push_back: [safe, (&'a mut, const T&) -> void]
//   std::vector::empty: [safe, (&'a) -> bool]
//   std::vector::size: [safe, (&'a) -> size_t]
//   const_cast: [unsafe]
//   rrr::Counter::next: [safe, (&'a mut) -> i64]
//   Log_error: [safe]
//   Log_debug: [safe]
//   Log_warn: [safe]
//   rrr::operator<<: [safe]
//   rrr::operator>>: [safe]
//   operator<<: [safe]
//   operator>>: [safe]
//   write_fn: [safe]
//   rrr::ServerConnection::write_fn: [safe]
// }
// NOTE: Marshal methods (set_bookmark, write_bookmark, get_and_reset_write_cnt, empty, content_size)
// are now annotated @safe in-place in marshal.hpp

// @unsafe - RPC module uses mutable spinlocks; channel layer handles
// raw sockets and pthread primitives transitively.




#include "../base/all.hpp"
#include "../misc/marshal.hpp"
#include "../reactor/epoll_wrapper.h"
#include "../reactor/reactor.h"


#include "channel.hpp"
#include "internal_protocol.hpp"
#include "utils.hpp"

namespace rrr {

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

    // Return a typed pointer to the underlying service instance. Default
    // returns `this`; the typed-box adapter overrides this to return its
    // wrapped concrete T*. Used by `Server::for_each_service` for
    // cleanup hooks that need to inspect the concrete service.
    virtual void* __get_service__() { return static_cast<void*>(this); }
};

using ServiceProxy = rusty::Box<Service>;

// Pass-through factory for services that already inherit Service.
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

  // Returns `this` (the adapter itself, which IS a Service). The wrapped
  // T does not inherit `Service`, so we can't expose it through a
  // Service*-shaped callback; callers needing the concrete T should
  // hold the typed handle separately.
  void* __get_service__() override { return static_cast<void*>(static_cast<Service*>(this)); }

 private:
  rusty::Box<T> svc_;
};

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

// @unsafe - Socket-backed connection handler exposed to poll loop via Pollable proxy facade.
// Uses SpinMutex for thread-safe interior mutability, Arc for shared ownership
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
    // Boxed for the same `pro::proxy<F>` cyclic-constraint workaround
    // applied to `ClientConnection::direct_channel_`. Mutable +
    // SpinMutex so the const `reply<F>` template path can lock it
    // briefly to dispatch a frame from any thread (mirrors the
    // client-side `direct_channel_` discipline).
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
    // @safe - Closes connection and cleans up
    // SAFETY: Thread-safe with server connection lock
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
    // @unsafe - Direct field assignment; callers must guarantee the
    // weak refers to the same Arc that owns this object.
    void install_self_weak_for_testing(WeakServerConnection weak) {
        // @unsafe { Weak copy-assign }
        { weak_self_ = std::move(weak); }
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

    // @safe - Delegates to thread pool (currently a no-op stub)
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

} // namespace rrr


namespace rrr {

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

    // @safe - Executes callback inline; returns error on empty callback.
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

        // @unsafe - weak pointer upgrade (safe operation, but rusty-cpp needs annotation)
        {
            auto sconn_opt = weak_sconn_.upgrade();
            if (sconn_opt.is_some()) {
                auto sconn = sconn_opt.unwrap();
                // No const_cast needed: reply() is now a const method with interior mutability
                sconn->reply(*req_, 0, archive_reply_);
            } else {
                // Connection closed, silently drop reply
                Log_debug("Connection closed before reply sent, dropping reply");
            }
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

        // @unsafe - weak pointer upgrade (safe operation, but rusty-cpp needs annotation)
        {
            auto sconn_opt = weak_sconn_.upgrade();
            if (sconn_opt.is_some()) {
                auto sconn = sconn_opt.unwrap();
                sconn->reply(*req_, error_code);
            } else {
                Log_debug("Connection closed before error reply sent, dropping reply");
            }
        }
    }
};

// @unsafe - Main RPC server managing connections
// SAFETY: Thread-safe connection management with spinlocks
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
    // `Box`ed because `pro::proxy<F>` triggers a cyclic-constraint
    // diagnostic when used directly as the value type of `rusty::Option`
    // (same workaround applied to `ClientConnection::factory_`).
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
        if (!factory.has_value()) return;
        // @unsafe { make_box + ChannelFactoryProxy move }
        channel_factory_ = rusty::Some(
            rusty::make_box<ChannelFactoryProxy>(std::move(factory)));
    }

    // @safe - True if `set_channel_factory` has been called with a non-null proxy.
    bool is_channel_factory_bound() const {
        return channel_factory_.is_some();
    }

    // @safe - Registers legacy virtual service and transfers ownership to Server.
    // Must be called before start().
    void reg_service(rusty::Box<Service> svc) {
        // @unsafe
        {
        pending_services_.push(make_service_proxy_from_box(std::move(svc)));
        // Get index AFTER push - this is the position of the service we just added
        size_t svc_index = pending_services_.size() - 1;
        // Register handlers using the index (service is safely stored in pending_services_)
        pending_services_[svc_index]->__reg_to__(*this, svc_index);
        }
    }

    void reg_service_proxy(ServiceProxy proxy) {
        pending_services_.push(std::move(proxy));
        size_t svc_index = pending_services_.size() - 1;
        pending_services_[svc_index]->__reg_to__(*this, svc_index);
    }

    // @safe - Registers typed service implementation without inheriting Service.
    // Must be called before start().
    template <ServiceLike T>
      requires (!std::derived_from<T, Service>)
    void reg_service(rusty::Box<T> svc) {
        // @unsafe
        {
        pending_services_.push(make_service_proxy_from_typed_box<T>(std::move(svc)));
        size_t svc_index = pending_services_.size() - 1;
        pending_services_[svc_index]->__reg_to__(*this, svc_index);
        }
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
        // @unsafe
        {
            if (pending_rpc_to_service_.contains_key(rpc_id)) {
                return EEXIST;
            }
            pending_rpc_to_service_.insert(rpc_id, svc_index);
            return 0;
        }
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

    // @safe - Blocks until shutdown is signaled
    void wait_for_shutdown();

    // === Graceful Shutdown API ===

    /**
     * Add a shutdown hook to be called during graceful shutdown.
     * Hooks are called in order of registration during the CLOSING phase.
     * @param hook Callback function to execute during shutdown
     */
    // @safe - Thread-safe hook registration
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
    // @unsafe - Uses std::atomic::load
    int pending_request_count() const {
        return pending_requests_->load(std::memory_order_relaxed);  // @unsafe
    }

    /**
     * Increment pending request count. Called when starting to process a request.
     */
    // @unsafe - Uses std::atomic::fetch_add
    void increment_pending() {
        auto* pending_ptr = const_cast<std::atomic<int>*>(pending_requests_.get());
        pending_ptr->fetch_add(1, std::memory_order_relaxed);  // @unsafe
    }

    /**
     * Decrement pending request count. Called when request completes.
     */
    // @unsafe - Uses std::atomic::fetch_sub
    void decrement_pending() {
        auto* pending_ptr = const_cast<std::atomic<int>*>(pending_requests_.get());
        pending_ptr->fetch_sub(1, std::memory_order_relaxed);  // @unsafe
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
            callback(*static_cast<Service*>((*guard)->__get_service__()));
        }
        }
    }

    // @safe - Returns the number of registered services
    // Can be called before or after start().
    size_t service_count() const {
        // @unsafe
        {
        if (ctx_.is_some()) {
            return ctx_.as_ref().unwrap()->services.size();
        }
        return pending_services_.size();
        }
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

} // namespace rrr
