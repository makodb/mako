// rrr.client — RPC client (formerly client.hpp + client.cpp).
//
// Owns ClientConnection (socket I/O + framing), Client (user-facing
// facade), Future / FutureGroup (async reply delivery), and the bulk
// reconnect helpers. Sits above the channel layer (`tcp_channel`,
// `inmemory_channel`) which the connection consumes via the
// transport-agnostic `ChannelConnectionProxy`.
module;

#include <cstddef>
#include <cstdint>

#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/cell.hpp>
#include <rusty/function.hpp>
#include <rusty/option.hpp>
#include <rusty/rusty.hpp>

export module rrr.client;

import std;
import rrr.basetypes;
import rrr.callback_wrapper;
import rrr.callbacks;
import rrr.channel;
import rrr.circuit_breaker;
import rrr.connection_metrics;
import rrr.connection_state;
import rrr.debugging;
import rrr.epoll_wrapper;
import rrr.errors;
import rrr.fiber_channel;
import rrr.heartbeat;
import rrr.internal_protocol;
import rrr.load_balancer;
import rrr.logging;
import rrr.marshal;
import rrr.misc;
import rrr.reactor;
import rrr.reconnect_policy;
import rrr.request_options;
import rrr.request_queue;
import rrr.serializable;
import rrr.tcp_channel;
import rrr.threading;

// ===========================================================================
// Block 1: forward decls (from former client.hpp:50-78)
// ===========================================================================
// @safe - first-half namespace block: Future / FutureGroup / TypedFuture
// awaiters + the BufferingConfig / KeepaliveConfig / PoolConfig POD
// structs. ClientConnection (declared in the second block below)
// retains its class-level `// @unsafe`. Methods that genuinely cross
// into network I/O / socket fd / Marshal byte ops keep their
// existing per-method `// @unsafe` annotations.
export namespace rrr {

// Stream operator for RefMut<Marshal> — supports the
// `fu->get_reply() >> x` pattern.  Each read dispatches through
// a `BinaryReadArchive` over a fresh `MarshalSource` so the
// format-decode contract matches the rpcgen-emitted dispatchers.
// The archive is a thin format wrapper — its read
// state lives on the underlying `Marshal`'s read cursor, so
// constructing a new archive per `>>` call produces the same byte
// stream as a single chained reader.  We return the guard
// reference for chaining; subsequent `>>` calls in a chain
// (`fu->get_reply() >> a >> b >> c`) all hit this same overload.
template<typename U>
rusty::RefMut<Marshal>& operator>>(rusty::RefMut<Marshal>& guard, U& value) {
    rrr::MarshalSource src(&*guard);
    rrr::BinaryReadArchive ar(rrr::make_source_proxy(&src));
    ar >> value;
    return guard;
}

template<typename U>
rusty::RefMut<Marshal>&& operator>>(rusty::RefMut<Marshal>&& guard, U& value) {
    rrr::MarshalSource src(&*guard);
    rrr::BinaryReadArchive ar(rrr::make_source_proxy(&src));
    ar >> value;
    return std::move(guard);
}

}  // export namespace rrr

// ===========================================================================
// Block 2: Future, FutureGroup, ClientConnection (from former client.hpp:130-1963)
// ===========================================================================
// @safe - second-half namespace block. Same rules as block 1; the
// ClientConnection class declared inside retains its class-level
// `// @unsafe` and every method that crosses into network I/O or
// Marshal ops carries an existing per-method override.
export namespace rrr {

// 4g4: the migration switch (`srpc_use_channel()`,
// `srpc_set_use_channel_for_testing(...)`,
// `srpc_reset_use_channel_for_testing()`) and its env-var triggers
// (`SRPC_USE_CHANNEL`, `SRPC_DISABLE_CHANNEL`) are gone. Channel
// mode is unconditional; `Client::connect` auto-installs a default
// TCP `ChannelFactoryProxy` when none has been bound via
// `set_channel_factory(...)`.

/**
 * Behavior when a request is made while disconnected.
 */
enum class DisconnectBehavior {
    QUEUE,      // Queue requests for later replay (default)
    FAIL_FAST   // Immediately fail with ENOTCONN
};

/**
 * Configuration for request buffering during disconnection.
 */
struct BufferingConfig {
    DisconnectBehavior behavior = DisconnectBehavior::QUEUE;
    size_t max_pending = 1000;           // Max queued requests
    uint32_t default_ttl_ms = 30000;     // 30 second TTL
    OverflowStrategy overflow = OverflowStrategy::DROP_OLDEST;
    bool enabled = true;

    // @safe - Aggregate-initialized POD factory.
    static BufferingConfig defaults() {
        return BufferingConfig{};
    }

    // @safe - Aggregate-initialized POD factory.
    static BufferingConfig disabled() {
        BufferingConfig config;
        config.enabled = false;
        config.behavior = DisconnectBehavior::FAIL_FAST;
        return config;
    }

    // @safe - Aggregate-initialized POD factory.
    RequestQueueConfig to_queue_config() const {
        RequestQueueConfig qc;
        qc.max_size = max_pending;
        qc.default_ttl_ms = default_ttl_ms;
        qc.overflow_strategy = overflow;
        qc.enabled = enabled;
        return qc;
    }
};

/**
 * TCP Keepalive configuration for connection health monitoring.
 *
 * Configures OS-level TCP keepalive probes to detect dead connections.
 * When enabled, the OS will send keepalive probes after the connection
 * has been idle for `idle_sec` seconds, then at `interval_sec` intervals.
 * If `count` probes go unanswered, the connection is considered dead.
 */
// @safe - POD config struct for TCP keepalive settings
struct KeepaliveConfig {
    bool enabled = true;       // Enable TCP keepalive probes
    int idle_sec = 60;         // TCP_KEEPIDLE: seconds before first probe
    int interval_sec = 10;     // TCP_KEEPINTVL: seconds between probes
    int count = 5;             // TCP_KEEPCNT: max unanswered probes before dropping

    // @safe - Default constructor
    KeepaliveConfig() = default;

    // @safe - Copy constructor
    KeepaliveConfig(const KeepaliveConfig&) = default;

    // @safe - Copy assignment
    KeepaliveConfig& operator=(const KeepaliveConfig&) = default;

    // Fast detection preset: 10s idle, 2s interval, 3 probes = 16s to detect failure
    // @safe - POD struct copy is memory-safe
    static KeepaliveConfig aggressive() {
        KeepaliveConfig config;
        config.enabled = true;
        config.idle_sec = 10;
        config.interval_sec = 2;
        config.count = 3;
        return config;
    }

    // Standard preset: 60s idle, 10s interval, 5 probes = 110s to detect failure
    // @safe - POD struct copy is memory-safe
    static KeepaliveConfig relaxed() {
        KeepaliveConfig config;
        config.enabled = true;
        config.idle_sec = 60;
        config.interval_sec = 10;
        config.count = 5;
        return config;
    }

    // Disabled preset
    // @safe - POD struct copy is memory-safe
    static KeepaliveConfig disabled() {
        KeepaliveConfig config;
        config.enabled = false;
        config.idle_sec = 0;
        config.interval_sec = 0;
        config.count = 0;
        return config;
    }
};

/**
 * ClientPool configuration for health-aware connection pooling.
 *
 * Controls connection limits, health checking, and idle timeout behavior.
 */
// @safe - POD config struct for pool settings
struct PoolConfig {
    int min_connections = 1;             // Minimum connections per address
    int max_connections = 4;             // Maximum connections per address
    uint64_t idle_timeout_ms = 300000;   // 5 minutes default (0 = no timeout)
    bool health_check_enabled = true;    // Enable health-based removal
    uint64_t unhealthy_threshold_percent = 50;  // Remove if success rate < this %
    uint64_t min_requests_for_health = 10;      // Min requests before health check
    LoadBalancingStrategy load_balancing = LoadBalancingStrategy::RANDOM;  // Selection strategy

    // @safe - Default constructor
    PoolConfig() = default;

    // @safe - Copy constructor
    PoolConfig(const PoolConfig&) = default;

    // @safe - Copy assignment
    PoolConfig& operator=(const PoolConfig&) = default;

    // Default preset: balanced settings
    // @safe - POD struct copy is memory-safe
    static PoolConfig defaults() {
        return PoolConfig();
    }

    // Aggressive preset: more connections, shorter timeout, stricter health
    // @safe - POD struct copy is memory-safe
    static PoolConfig aggressive() {
        PoolConfig config;
        config.min_connections = 2;
        config.max_connections = 8;
        config.idle_timeout_ms = 60000;  // 1 minute
        config.health_check_enabled = true;
        config.unhealthy_threshold_percent = 70;  // Stricter
        config.min_requests_for_health = 5;
        return config;
    }

    // Conservative preset: fewer connections, longer timeout, lenient health
    // @safe - POD struct copy is memory-safe
    static PoolConfig conservative() {
        PoolConfig config;
        config.min_connections = 1;
        config.max_connections = 2;
        config.idle_timeout_ms = 600000;  // 10 minutes
        config.health_check_enabled = true;
        config.unhealthy_threshold_percent = 30;  // More lenient
        config.min_requests_for_health = 20;
        return config;
    }

    // Disabled health checking preset
    // @safe - POD struct copy is memory-safe
    static PoolConfig no_health_check() {
        PoolConfig config;
        config.health_check_enabled = false;
        return config;
    }
};

class Future;
// @unsafe - Forward declarations
class Client;
class ClientConnection;

// Type alias for Future result (replaces nullable Future* returns)
// Ok(Arc<Future>) on success, Err(error_code) on failure
using FutureResult = rusty::Result<rusty::Arc<Future>, i32>;

// FutureAttr's callback is the same `Arc<Function<...const>>`-backed
// wrapper used by the channel-tier callback typedefs in channel.hpp:
// default-constructible (empty Function inside the Arc), copyable
// (Arc clone = atomic refcount bump), implicit construction from any
// compatible callable, `operator bool` / `operator()`.  Sharing the
// wrapper keeps the API surface identical to the prior std::function
// (so the 92+ existing `fuattr.callback = lambda;` callsites compile
// unchanged) while letting FutureAttr propagate through generated
// rcc_rpc.h proxy stubs cheaply.
using FutureCallback = detail::CallbackWrapper<void(rusty::Arc<Future>) const>;

// @safe - Simple attribute struct for Future callbacks
struct FutureAttr {
    FutureAttr(FutureCallback cb = FutureCallback{}) : callback(std::move(cb)) { }

    // callback should be fast, otherwise it hurts rpc performance
    // Receives Arc<Future> for lifetime safety (callback keeps Future alive)
    FutureCallback callback;
};

// @safe - Methods that genuinely cross into unsafe ops (network I/O,
// std::chrono use, etc.) carry their own `// @unsafe` overrides; the
// rest of the class is now analyzed as @safe by default.
// Uses rusty::Arc for memory safety, RefCell/Cell for interior mutability
class Future {
    friend class rusty::Arc<Future>;  // Allow Arc to construct/destroy
    friend class Client;              // Client needs to call private constructor and set error
    friend class ClientConnection;    // ClientConnection needs access to set error and notify

    struct State {
        bool ready = false;
        bool timed_out = false;
        rusty::Vec<rusty::Function<void()>> completion_callbacks;
    };

    i64 xid_;
    rusty::Cell<i32> error_code_;  // Cell for interior mutability of Copy type

    FutureAttr attr_;
    rusty::RefCell<Marshal> reply_;  // RefCell for interior mutability with runtime borrow checking

    uint64_t timeout_{1000000}; // default timeout 1s (jetpack)
    rusty::Mutex<State> state_;  // Mutex protects State (ready/timed_out flags)
    rusty::Condvar ready_cond_;  // Uses interior mutability (const methods like Rust's &self)

    // Retry support
    rusty::Cell<RequestOptions> options_;     // Request options (timeout, retry config)
    rusty::Cell<TimeoutType> timeout_type_{TimeoutType::NONE};  // Type of timeout that occurred
    rusty::Cell<uint16_t> retry_count_{0};    // Number of retries attempted

    // @safe - Uses rusty::Mutex and rusty::Condvar together (Rust-like pattern)
    // Takes Arc<Future> self parameter for callback safety
    void notify_ready(rusty::Arc<Future> self) const;

    // Private destructor - only Arc can delete
    // @safe - RAII destructors handle cleanup automatically
    ~Future() = default;

    // Private constructor - only Arc factory can create
    // @safe - Default initialization with RAII primitives
    Future(i64 xid, const FutureAttr& attr = FutureAttr())
            : xid_(xid), error_code_(0), attr_(attr), reply_(), state_(State{}),
              ready_cond_() {
    }

public:

    // Factory method for Arc creation
    // @safe - Arc::make is @safe in the library.
    static rusty::Arc<Future> create(i64 xid, const FutureAttr& attr = FutureAttr()) {
        return rusty::Arc<Future>::make(xid, attr);
    }

    // @safe - rusty::Mutex::lock / Result::unwrap / MutexGuard::operator* are
    // all @safe in the library.
    bool ready() const {
        auto guard = state_.lock().unwrap();
        return (*guard).ready;
    }

    // @safe - Uses rusty::Mutex and rusty::Condvar together
    void wait() const;

    // @safe - Uses rusty::Mutex and rusty::Condvar together
    void timed_wait(double sec) const;

    // @unsafe - rusty-cpp false positive: sec IS initialized
    // Wait using configured options timeout
    // Returns true if ready, false if timed out
    bool wait_with_options() const {
        auto opts = options_.get();
        if (opts.timeout_ms == 0) {
            wait();  // No timeout
            return ready();
        }
        double sec = static_cast<double>(opts.timeout_ms) / 1000.0;
        timed_wait(sec);
        return ready() && !timed_out();
    }

    // @safe - rusty::Mutex::lock + MutexGuard ops are @safe.
    bool timed_out() const {
        auto guard = state_.lock().unwrap();
        return (*guard).timed_out;
    }

    // @safe - rusty::Mutex::lock + rusty::Vec::push + rusty::Function move
    // are @safe. unwrap() on poisoned mutex intentionally panics, matching
    // existing policy.
    bool add_completion_callback(rusty::Function<void()> callback) const {
        auto guard = state_.lock().unwrap();
        if (guard->ready || guard->timed_out) {
            return false;
        }
        guard->completion_callbacks.push(std::move(callback));
        return true;
    }

    // @safe - rusty::RefCell::borrow_mut is @safe (RefCell namespace).
    // Caller holds the guard, ensuring the reference can't outlive it.
    rusty::RefMut<Marshal> get_reply() const {
        wait();
        return reply_.borrow_mut();
    }

    // @safe - Calls wait methods, uses @unsafe for timed_wait which uses std::chrono
    i32 get_error_code() const {
        if (timeout_ > 0) {
            double x = timeout_;
            x = x / 1000000;
            // @unsafe
            { timed_wait(x); }
        } else {
            wait();
        }
        return error_code_.get();
    }

    // @safe - Simple getter
    i64 get_xid() const {
        return xid_;
    }

    // =========================================================================
    // Retry Support Accessors
    // =========================================================================

    // @safe - Get request options
    RequestOptions get_options() const {
        return options_.get();
    }

    // @safe - Set request options
    void set_options(const RequestOptions& opts) const {
        options_.set(opts);
    }

    // @safe - Get timeout type that occurred
    TimeoutType get_timeout_type() const {
        return timeout_type_.get();
    }

    // @safe - Set timeout type
    void set_timeout_type(TimeoutType type) {
        timeout_type_.set(type);
    }

    // @safe - Get current retry count
    uint16_t get_retry_count() const {
        return retry_count_.get();
    }

    // @safe - Increment retry count and return new value
    uint16_t increment_retry_count() {
        uint16_t current = retry_count_.get();
        retry_count_.set(current + 1);
        return current + 1;
    }

    // @safe - Check if should retry based on options and current state
    bool should_retry() const {
        auto opts = options_.get();
        return opts.can_retry(retry_count_.get());
    }

    // =========================================================================
    // Compatibility shim for legacy code that calls Future::safe_release()
    // =========================================================================
    // With rusty::Arc, manual release is no longer needed - Arc automatically
    // cleans up when the last reference goes out of scope. These are NO-OP
    // functions that exist solely for backward compatibility with existing
    // call sites (raft/macros.h, fpga_raft/commo.cc, paxos/commo.cc, etc.)
    //
    // Old pattern (raw pointer):
    //   Future* fu = proxy->async_Something(fuattr);
    //   Future::safe_release(fu);  // Manual cleanup required
    //
    // New pattern (Arc):
    //   auto fu = proxy->async_Something(fuattr);
    //   // No cleanup needed - Arc handles it automatically when fu goes out of scope
    //   Future::safe_release(fu);  // NO-OP, just for compatibility
    // =========================================================================

    // @safe - NO-OP: Arc automatically releases when dropped
    static inline void safe_release(rusty::Arc<Future> fu) {
        (void)fu;  // Intentionally empty - Arc handles cleanup
    }

    // @safe - NO-OP: Legacy overload for any remaining raw pointer usage in old code paths
    static inline void safe_release(Future* fu) {
        (void)fu;  // Intentionally empty - should not be called in new code
    }

    // @safe - NO-OP: Overload for FutureResult (Result<Arc<Future>, i32>) - jetpack compatibility
    static inline void safe_release(FutureResult fu_result) {
        (void)fu_result;  // Intentionally empty - Arc handles cleanup
    }
};

// @safe - Awaiter for generated typed RPC futures.
// co_await returns the same typed resolve() result as sync wrappers.
template<typename TypedFuture>
class TypedFutureAwaiter {
    static_assert(
        std::is_same_v<
            decltype(std::declval<const TypedFuture&>().raw_future()),
            rusty::Arc<Future>>,
        "TypedFuture must expose raw_future() returning rusty::Arc<rrr::Future>");

    using ResolveResult = decltype(std::declval<const TypedFuture&>().resolve());

public:
    explicit TypedFutureAwaiter(TypedFuture typed_future)
        : typed_future_(std::move(typed_future)) { }

    bool await_ready() const {
        return typed_future_.ready();
    }

    bool await_suspend(std::coroutine_handle<> handle) const {
        auto* ctx = rusty::current_context();
        if (ctx != nullptr && ctx->waker != nullptr) {
            auto waker = *(ctx->waker);
            return typed_future_.raw_future()->add_completion_callback(
                [waker]() mutable { waker.wake(); });
        }
        return typed_future_.raw_future()->add_completion_callback(
            [handle]() mutable { handle.resume(); });
    }

    ResolveResult await_resume() const {
        return typed_future_.resolve();
    }

private:
    TypedFuture typed_future_;
};

// @safe - Helper to build TypedFutureAwaiter with type deduction.
template<typename TypedFuture>
TypedFutureAwaiter<TypedFuture> make_typed_future_awaitable(TypedFuture typed_future) {
    return TypedFutureAwaiter<TypedFuture>(std::move(typed_future));
}

// @safe - Awaiter for Result<TypedFuture, i32> returned by async_* proxy methods.
// This allows `co_await proxy.await_xxx(req)` and preserves immediate send errors.
template<typename TypedFuture>
class TypedFutureResultAwaiter {
    using ResolveResult = decltype(std::declval<const TypedFuture&>().resolve());

public:
    explicit TypedFutureResultAwaiter(rusty::Result<TypedFuture, i32> typed_future_result)
        : typed_future_result_(std::move(typed_future_result)) { }

    bool await_ready() const {
        return typed_future_result_.is_err() || typed_future_result_.unwrap().ready();
    }

    bool await_suspend(std::coroutine_handle<> handle) const {
        if (typed_future_result_.is_err()) {
            return false;
        }
        auto* ctx = rusty::current_context();
        if (ctx != nullptr && ctx->waker != nullptr) {
            auto waker = *(ctx->waker);
            return typed_future_result_.unwrap().raw_future()->add_completion_callback(
                [waker]() mutable { waker.wake(); });
        }
        return typed_future_result_.unwrap().raw_future()->add_completion_callback(
            [handle]() mutable { handle.resume(); });
    }

    ResolveResult await_resume() const {
        if (typed_future_result_.is_err()) {
            return ResolveResult::Err(typed_future_result_.unwrap_err());
        }
        return typed_future_result_.unwrap().resolve();
    }

private:
    rusty::Result<TypedFuture, i32> typed_future_result_;
};

// @safe - Helper to build TypedFutureResultAwaiter with type deduction.
template<typename TypedFuture>
TypedFutureResultAwaiter<TypedFuture> make_typed_future_result_awaitable(
    rusty::Result<TypedFuture, i32> typed_future_result) {
    return TypedFutureResultAwaiter<TypedFuture>(std::move(typed_future_result));
}

// @safe - RAII container for managing multiple futures
// MIGRATED: Now uses Arc<Future> for automatic memory management
class FutureGroup {
private:
    rusty::Vec<rusty::Arc<Future>> futures_;

public:
    // @safe - Adds future to group
    void add(rusty::Arc<Future> f) {
        if (!f) {  // Check Arc validity (empty Arc check)
            Log_error("Invalid Future object passed to FutureGroup!");
            return;
        }
        futures_.push(std::move(f));
    }

    // @safe - Waits for all futures in group
    void wait_all() {
        for (auto& f : futures_) {
            f->wait();
        }
    }

    // @safe - Destructor waits for futures, Arc auto-cleanup
    ~FutureGroup() {
        wait_all();
        // Arc auto-released when vector destroyed - no manual release needed
    }
};

// Type alias for Arc weak reference to ClientConnection
using WeakClientConnection = rusty::sync::Weak<ClientConnection>;

// @safe - Client-side socket handler exposed to poll loop via Pollable
// proxy facade.  Methods that genuinely cross socket I/O, Marshal byte
// chains, fiber dispatch, cross-thread queues, or raw pointer ops carry
// per-method `// @unsafe` overrides; the rest inherit `@safe` from this
// class umbrella.
// Uses SpinMutex for thread-safe interior mutability, Arc for shared ownership.
class ClientConnection {
    friend class Client;
    friend class ClientPool;

    // Shared reference to PollThread for async communication.
    // 4g3c3: ClientConnection no longer owns an fd or in/out Marshal
    // buffers; the channel layer's `TcpConnection` owns the fd and
    // drives the read/write loop. The `poll_thread_worker_` field
    // remains because tests construct `ClientConnection` directly
    // with an Arc<PollThread> and the channel-factory production
    // path still routes through it for thread affinity.
    rusty::Arc<PollThread> poll_thread_worker_;

    //
    // When the client is in channel mode, `fiber_channel_` owns a
    // `FiberChannel` wrapper around the `ChannelConnectionProxy`; the
    // wrapper exposes blocking `send_frame` / `recv_frame` so that the
    // recv-loop fiber spawned in `bind_channel` can drive response
    // demux top-to-bottom (no callback unrolling). The proxy itself is
    // moved into the wrapper, so this field is the single owner of the
    // channel-side state. `socket_` becomes irrelevant in channel mode
    // and the inherited `Pollable` I/O methods (`handle_read`,
    // `handle_write`) short-circuit to no-ops.
    //
    // `channel_mode_` is the opt-in latch flipped by `bind_channel` so
    // the routing logic can branch without inspecting the Option (and
    // without touching the FiberChannel from threads that don't own
    // it).
    //
    // SpinMutex<Option<Box<>>> shape:
    //   - SpinMutex — thread-safe interior mutability so `request(...)`
    //     const paths can lock the wrapper to send outbound frames
    //     concurrently from many user threads while the poll thread's
    //     recv-loop fiber briefly locks to extract the FiberChannel
    //     pointer. (Sub-leaf 4g1 swapped this from `RefCell` because
    //     RefCell's non-atomic borrow_state corrupts under multi-thread
    //     access — observed in `RPCTest.MultiThreadedStressTest` with
    //     100 user threads.)
    //   - Option   — None until `bind_channel` is called; Some after.
    //   - Box      — stable address: the recv-loop fiber holds a raw
    //     pointer to the wrapper (not the connection itself, which
    //     could move under it via Arc clones).
    //
    // Lifetime: a parked recv-loop fiber holds a `Weak<ClientConnection>`
    // and the FiberChannel pointer (the FiberChannel outlives the
    // fiber as long as the wrapper sits in this field). Full close /
    // reconnect cleanup is wired in sub-leaves 4d/4e — for 4c2, the
    // fiber simply exits when the proxy's `on_closed` fires (the only
    // way to break out of `recv_frame` other than process exit).
    mutable SpinMutex<rusty::Option<rusty::Box<FiberChannel>>> fiber_channel_{rusty::Option<rusty::Box<FiberChannel>>(rusty::None)};

    // direct-callback channel binding.
    //
    // When `bind_channel_direct(...)` is called instead of
    // `bind_channel*(...)`, this slot owns the channel proxy and
    // installs `on_frame` / `on_closed` callbacks directly on it.
    // No `FiberChannel`, no recv-loop fiber, no `IntEvent` per
    // received frame — `on_frame` invokes `decode_response_and_notify`
    // inline on whichever thread the channel layer fires it on
    // (typically the poll thread for TCP). Closer to the legacy
    // fd-path's `handle_read → notify_ready` flow that scales to
    // 200+ threads on a shared poll thread.
    //
    // Bypasses the deeper reactor/fiber wedge documented in 4g1b.
    // The legacy `fiber_channel_` slot stays for unit-test
    // compatibility (those tests use `bind_channel(...)` and drive
    // FakeChannelStub::deliver() from the test thread).
    //
    // Box-wrapped to retain the same indirection shape used by
    // `fiber_channel_` and `factory_` (the alias is already a
    // `rusty::Box<ChannelConnectionBase>`; the outer Box keeps the
    // Option's value-type uniform across slots).
    mutable SpinMutex<rusty::Option<ChannelConnectionProxy>> direct_channel_{rusty::Option<ChannelConnectionProxy>(rusty::None)};

    rusty::Cell<bool> channel_mode_{false};

    // channel-mode factory.
    //
    // When a `ChannelFactoryProxy` is bound via `bind_factory()`,
    // `connect(addr)` and the close fan-out's reconnect path route
    // through `factory_->connect(addr)` instead of issuing a raw
    // socket(2) + connect(2) + register-pollable sequence. The
    // factory's returned proxy is then handed to `bind_channel(...)`
    // automatically, so the connection enters channel mode without
    // a separate caller-driven setup step.
    //
    // Default-constructed (`None`) means "no factory bound" — the
    // legacy fd path stays in use. Sub-leaf 4f adds the migration
    // switch that selects between the two paths; sub-leaf 4g removes
    // the legacy fd path entirely.
    //
    // Boxed to retain the same indirection shape as `fiber_channel_`
    // (the alias is already a `rusty::Box<ChannelFactoryBase>`; the
    // outer Box keeps the Option's value-type uniform across slots).
    // SpinMutex (not RefCell) for the same reason as `fiber_channel_`:
    // the close fan-out's reconnect spawn calls `connect_via_factory`
    // from a separate thread, which can race against user-thread
    // accessors like `is_factory_bound()` (sub-leaf 4g1).
    mutable SpinMutex<rusty::Option<ChannelFactoryProxy>> factory_{rusty::Option<ChannelFactoryProxy>(rusty::None)};

    // Transaction ID counter for RPC requests
    // mutable because Counter uses atomics internally for thread-safe interior mutability
    mutable Counter xid_counter_;

    // Map of pending futures awaiting responses (protected by SpinMutex)
    SpinMutex<rusty::HashMap<i64, rusty::Arc<Future>>> pending_fu_{rusty::HashMap<i64, rusty::Arc<Future>>()};

public:
    // Async-callback slot array — slim alternative to `pending_fu_` for
    // callers that don't need an `Arc<Future>` handle (no sync-wait,
    // no retry, no reply-buffer inspection — just "call me back when
    // the reply arrives").  Indexed by `xid % kAsyncSlotCount`.  At
    // typical in-flight depths (a few thousand), collisions are
    // impossible.  See `request_async` below.
    static constexpr size_t kAsyncSlotCount = 16384;
    using AsyncReplyCallback = rusty::Function<
        void(i32 /*error_code*/, const uint8_t* /*reply_bytes*/, size_t /*reply_size*/)>;
private:
    mutable SpinMutex<rusty::Vec<rusty::Option<AsyncReplyCallback>>>
        pending_cb_slots_;

    // Connection state machine for lifecycle management
    ConnectionStateMachine state_machine_;

    // Reconnection policy and state
    ReconnectPolicy reconnect_policy_;
    // mutable: std::atomic::store / .load are interior-mutable in
    // semantics but std::atomic::store is not declared `const` (it has
    // `volatile` overloads only). Mark mutable so const methods can
    // legally call store() — atomic semantics make this race-free.
    mutable std::atomic<bool> reconnecting_{false};
    mutable std::atomic<bool> reconnect_abort_{false};
    std::string reconnect_address_;  // Address to reconnect to

    // Request buffering during disconnection
    mutable BufferingConfig buffering_config_;  // mutable for const set_buffering_config()
    mutable RequestQueue pending_queue_;  // mutable for const request() access

    // Server restart detection: tracks server instance ID
    // 0 means no ID received yet (initial state)
    rusty::Cell<uint64_t> server_instance_id_{0};

    // Callback invoked when server restart is detected (ID changes).
    // Parameters: (old_id, new_id).  rusty::Function is move-only;
    // the setter takes the callback by value-with-move and the
    // callsite below invokes it by reference (no copy).
    mutable rusty::Function<void(uint64_t, uint64_t)> on_server_restart_;

    // TCP Keepalive configuration for connection health monitoring (Cell for interior mutability)
    rusty::Cell<KeepaliveConfig> keepalive_config_;
    // Heartbeat manager integrated into poll loop write/read lifecycle.
    mutable HeartbeatManager heartbeat_manager_;
    // Circuit breaker integrated into live request dispatch.
    mutable CircuitBreaker circuit_breaker_;
    // Lifecycle callback hooks shared with Client facade.
    rusty::Arc<CallbackManager> callback_manager_;

    // Last activity timestamp for idle detection (milliseconds since epoch)
    // Updated on send/receive operations
    rusty::Cell<uint64_t> last_activity_time_{0};

    // Connection health metrics
    ConnectionMetrics metrics_;

    // Weak pointer to self, initialized after creation
    // Used to pass weak reference for poll thread registration
    WeakClientConnection weak_self_;

    // Jetpack-specific members
    std::string host_;
    uint64_t packets_{0};
    rusty::Cell<bool> paused_{false};  // Cell for interior mutability
    bool is_client_mode_{false};  // Jetpack: distinguishes client vs server mode

    // @safe - Cancels all pending futures (has internal @unsafe blocks)
    // SAFETY: Protected by spinlock
    void invalidate_pending_futures() const;

    // @safe - Fail one pending future by xid if it still exists.
    // Safe to call repeatedly; only first call for a given xid has effect.
    void fail_pending_future(i64 xid, int err) const;

    // channel-mode response demux
    // and close-side fan-out.
    //
    // `run_recv_loop` blocks the calling fiber on
    // `FiberChannel::recv_frame` and dispatches each decoded response
    // body to the matching pending future. On `recv_frame` returning
    // None (channel closed), it calls `on_channel_closed_fan_out`
    // before exiting so the connection's reliability layer (error
    // callback, pending-future cancellation, reconnect attempt) sees
    // the close.
    //
    // `decode_response_and_notify` is the single-frame helper called
    // by the loop on each iteration; it parses the body and resolves
    // / drops the matching future.
    // @unsafe - Drives Marshal / Future / pending_fu_ from a fiber.
    void run_recv_loop();
    // @unsafe - Same body, factored for testability.
    void decode_response_and_notify(const std::uint8_t* bytes,
                                    std::size_t size);
    // @unsafe - Channel-mode close fan-out: error/disconnected
    // callbacks, pending-future cancellation, reconnect spawn.
    void on_channel_closed_fan_out();

    // @unsafe - Channel-factory connect helper (sub-leaf 4e). Calls
    // `factory_->connect(addr)` and routes the returned proxy
    // through `bind_channel(...)`. Caller is `connect(addr)` which
    // already verified the factory is bound.
    int connect_via_factory(const char* addr);

    // @unsafe - Reset channel-mode state so a subsequent `connect`
    // call can re-bind. Used by the factory-driven reconnect path
    // in `on_channel_closed_fan_out` (sub-leaf 4e). Drops the stale
    // `FiberChannel` (its proxy is closed by definition at this
    // point), clears the `channel_mode_` latch, and forces the
    // state machine to DISCONNECTED so `connect`'s
    // `verify(!is_connected())` passes. The recv-loop fiber from
    // the old binding has already exited (recv_frame returned None
    // before the fan-out ran).
    void reset_channel_mode_for_reconnect();

    // observable counter for channel-mode
    // auto-reconnect attempts. Incremented before the reconnect
    // thread spawn in `on_channel_closed_fan_out`. Tests inspect
    // this to verify the fan-out reached the reconnect-policy branch
    // without having to drive a real reconnect (sub-leaf 4e wires
    // the factory-based reconnect that supersedes the legacy fd
    // path).
    std::atomic<uint64_t> channel_reconnect_attempts_{0};

public:
    /**
     * Closes the connection and cleans up resources.
     * Called by:
     * 1: PollThreadWorker::do_close_pollable() for thread-safe close
     * 2: handle_error() for error handling
     */
    // @safe - Closes connection and cleans up
    // SAFETY: Thread-safe cleanup sequence
    void close() const;

    /**
     * Mark connection as closing without closing the socket.
     * Used by Client::close() to update state before poll thread closes socket.
     * This avoids race conditions with pending CmdAddPollable commands.
     */
    // @safe - Just updates state machine
    void mark_closing() const;

    // Public destructor for Arc compatibility
    // @safe - Simple destructor
    ~ClientConnection();

    // @safe - Initializes connection (only stores references)
    ClientConnection(rusty::Arc<PollThread> poll_thread_worker);

    /**
     * Bind a `ChannelConnectionProxy` to this connection
     *.
     *
     * Once bound, the connection enters "channel mode": outbound frames
     * are routed through the channel via `request_via_channel`, and a
     * recv-loop fiber is spawned to drive inbound response demux on
     * top of a `FiberChannel` wrapper around the proxy.
     *
     * **Threading**: the recv-loop fiber is spawned on the *current*
     * thread's reactor via `Fiber::create_run`. For the channel-layer
     * threading contract to hold (callbacks fire on the same reactor
     * the fiber lives on), `bind_channel` must be called from the
     * thread that owns the proxy's callback dispatch — typically the
     * poll thread for production, the test thread for fakes. Cross-
     * thread scheduling of the fiber spawn is sub-leaf 4e's concern.
     *
     * Calling this more than once is undefined for now (subsequent
     * leaves may relax that). Calling it with a default-constructed
     * (null) proxy is a no-op.
     */
    // @unsafe - Spawns recv-loop fiber, constructs FiberChannel wrapper.
    void bind_channel(ChannelConnectionProxy channel);

    /**
     * bind that schedules the recv-loop
     * fiber spawn onto the poll thread.
     *
     * Used by production code paths (factory-driven `connect` /
     * reconnect) that run on the user thread but need the
     * recv-loop fiber on the poll thread — same reactor that fires
     * the channel proxy's callbacks (e.g. `TcpConnection::handle_read`'s
     * `on_frame`). Submits a `OneTimeJob` to the poll thread whose
     * `Work()` calls `run_recv_loop()` from a fiber that the
     * `trigger_job` body spawns on its own reactor.
     *
     * Use `bind_channel(...)` (above) when the calling thread is
     * also the thread that fires the proxy's callbacks
     * (single-threaded fake-channel unit tests).
     */
    // @unsafe - Submits a OneTimeJob across thread boundary.
    void bind_channel_via_poll_thread(ChannelConnectionProxy channel);

    /**
     * direct on_frame callback.
     *
     * Bind the channel without `FiberChannel` and without the
     * recv-loop fiber. Installs `on_frame` and `on_closed` callbacks
     * directly on the channel proxy; on_frame calls
     * `decode_response_and_notify` inline; on_closed calls
     * `on_channel_closed_fan_out` inline. Both fire on whichever
     * thread the channel layer drives them on (typically the poll
     * thread for TCP). No `IntEvent` allocation per frame, no
     * fiber yield, no `waiting_events_` churn.
     *
     * Used by `connect_via_factory(...)` for production TCP. The
     * legacy `bind_channel(...)` / `bind_channel_via_poll_thread(...)`
     * paths stay for unit-test compatibility (they use FiberChannel
     * + recv-loop fiber, which works fine in single-threaded test
     * contexts).
     *
     * After this call, `is_channel_mode()` returns true. The
     * `direct_channel_` slot owns the proxy.
     */
    // @unsafe - Captures Weak<ClientConnection> in proxy callbacks.
    void bind_channel_direct(ChannelConnectionProxy channel);

    // @safe - True if `bind_channel` has been called with a non-null proxy.
    bool is_channel_mode() const { return channel_mode_.get(); }

    /**
     * Bind a `ChannelFactoryProxy` to this connection
     *.
     *
     * Once bound, `connect(addr)` and the close fan-out's reconnect
     * spawn route through `factory->connect(addr)` instead of the
     * legacy socket(2) + connect(2) + register-pollable sequence.
     * The factory's returned proxy is automatically handed to
     * `bind_channel(...)` on success — callers don't need to wire
     * channel mode manually.
     *
     * Calling with a default-constructed (null) proxy is a no-op.
     * Calling more than once replaces the previously-bound factory;
     * an in-flight reconnect that already grabbed the old factory
     * will complete with the old factory.
     */
    // @unsafe - Records the factory under SpinMutex interior mutability.
    void bind_factory(ChannelFactoryProxy factory) {
        if (!factory) return;
        // SpinMutex::lock + Box move-assign are both @safe.
        {
            auto guard = factory_.lock().unwrap();
            *guard = rusty::Some(std::move(factory));
        }
    }

    // @safe - SpinMutex::lock and Option::is_some are both @safe.
    bool is_factory_bound() const {
        auto guard = factory_.lock().unwrap();
        return guard->is_some();
    }

    // Test-only: install the self-pointer before code paths that need
    // to upgrade it (e.g., the recv-loop fiber spawned in
    // `bind_channel`). Production code goes through `Client::connect`,
    // which wires `weak_self_` automatically as part of the
    // freshly-constructed-Arc init dance. Tests that construct
    // `ClientConnection` directly via `Arc::make` must call this
    // before any channel-mode code path that captures the weak.
    // @safe - Direct field assignment; rusty::sync::Weak move-assign is now @safe.
    // Callers must guarantee the weak refers to the same Arc that owns this object.
    void install_self_weak_for_testing(WeakClientConnection weak) {
        weak_self_ = std::move(weak);
    }

    // Test-only: drive the state machine to `CONNECTED`. Production
    // gets here via `connect(addr)` after the channel is bound; tests
    // that construct a `ClientConnection` + `bind_channel(stub)`
    // directly (without going through `Client::connect`) need this so
    // the `request_via_channel` state-machine gate doesn't short-
    // circuit them with ENOTCONN.
    // @unsafe - state-machine mutation outside the connect() lifecycle.
    void force_connected_for_testing() {
        state_machine_.force_state(ConnectionState::CONNECTED);
    }

    // observable counter for
    // channel-mode close fan-out's reconnect spawn. Tests verify the
    // fan-out reached the reconnect branch by checking this counter
    // pre/post a synthesized `on_closed`.
    // @safe - Atomic load.
    uint64_t channel_reconnect_attempts_count() const {
        return channel_reconnect_attempts_.load(std::memory_order_acquire);
    }

    // Test-only: seed the reconnect-target address. Production wires
    // this through `connect(addr)`. Channel-mode tests that want to
    // verify the close fan-out's reconnect-policy branch check this
    // before the synthesized `on_closed`.
    // @safe - single std::string move-assign on a non-shared field.
    void set_reconnect_address_for_testing(std::string addr) {
        reconnect_address_ = std::move(addr);
    }

    // Test-only: short-circuit the reconnect spawn body before it
    // reaches the legacy fd `reconnect()` path. Tests rely on the
    // close fan-out's counter bumping *before* the spawn, while the
    // spawned thread itself aborts immediately.
    // @safe - Atomic store.
    void abort_reconnect() {
        reconnect_abort_.store(true, std::memory_order_release);
    }

    // @safe - Simple status check using state machine
    bool connected() const {
        return state_machine_.is_connected();
    }

    // @safe - Get current connection state
    ConnectionState connection_state() const {
        return state_machine_.state();
    }

    /**
     * Establish TCP connection to remote server.
     * Returns 0 on success, error code on failure.
     */
    // @unsafe - Establishes TCP connection
    // SAFETY: Proper socket creation and error handling
    int connect(const char* addr);

    /**
     * Set the reconnection policy for this connection.
     * The policy controls automatic reconnection behavior after failures.
     */
    // @safe - Sets reconnection policy (wraps operator= in @unsafe block)
    void set_reconnect_policy(const ReconnectPolicy& policy) {
        // @unsafe - struct assignment operator not annotated
        { reconnect_policy_ = policy; }
    }

    // @safe - Get the current reconnection policy
    // @lifetime: (&'a) -> &'a
    const ReconnectPolicy& reconnect_policy() const {
        return reconnect_policy_;
    }

    // @safe - Check if a reconnection attempt is in progress
    bool is_reconnecting() const {
        return reconnecting_.load(std::memory_order_acquire);
    }

    /**
     * Attempt to reconnect to the last connected address.
     * Can only be called when connection is in FAILED or DISCONNECTED state.
     *
     * @param on_complete Optional callback called with success/failure result
     * @return 0 on success (reconnection started), error code on failure
     */
    // @unsafe - Attempts reconnection (calls connect which has socket operations)
    int reconnect(rusty::Function<void(bool)> on_complete = nullptr);

    /**
     * Set the buffering configuration for this connection.
     * Controls whether requests are queued during disconnection.
     * Note: const because internal state is mutable.
     */
    // @safe - Sets buffering configuration
    void set_buffering_config(const BufferingConfig& config) const;

    // @safe - Get the current buffering configuration
    // @lifetime: (&'a) -> &'a
    const BufferingConfig& buffering_config() const {
        return buffering_config_;
    }

    // @safe - RequestQueue::size is @safe (lock + VecDeque::size).
    size_t pending_request_count() const {
        return pending_queue_.size();
    }

    // @unsafe - Uses SpinMutex + rusty::HashMap access
    size_t pending_future_count() const {
        auto pending_guard = pending_fu_.lock().unwrap();
        return pending_guard->len();
    }

#ifdef RPC_TEST_HOOKS
    // @unsafe - Test hook: replays queue through non-const internal method.
    size_t replay_pending_requests_for_test() const {
        return const_cast<ClientConnection*>(this)->replay_pending_requests();
    }

    // @safe - Test hook: update queue policy without clearing current queue.
    void update_pending_queue_config_for_test(const RequestQueueConfig& config) const {
        pending_queue_.update_config(config);
    }
#endif

    // @safe - RequestQueue::clear_all is @safe.
    // Note: const because pending_queue_ is mutable
    void clear_pending_requests(int error_code = ECONNABORTED) const {
        pending_queue_.clear_all(error_code);
    }

    // === Server Restart Detection API ===

    /**
     * Get the last known server instance ID.
     * Returns 0 if no ID has been set yet (initial state).
     */
    // @safe - Simple getter using Cell
    uint64_t server_instance_id() const {
        return server_instance_id_.get();
    }

    /**
     * Set the callback to be invoked when server restart is detected.
     * The callback receives (old_id, new_id) when the server's instance ID changes.
     *
     * @param callback Function to call on restart detection
     */
    // @safe - rusty::Function move-assign is @safe; interior mutability via
    // mutable on_server_restart_ is sound because the assignment happens
    // through a const method that owns the only thread-visible reference.
    void set_on_server_restart(rusty::Function<void(uint64_t, uint64_t)> callback) const {
        on_server_restart_ = std::move(callback);
    }

    /**
     * Check and update the server instance ID.
     * If the new ID differs from the stored ID (and stored ID was non-zero),
     * the on_server_restart callback is invoked.
     *
     * @param new_id The new server instance ID
     * @return true if server restart was detected, false otherwise
     */
    // @safe - rusty::Cell get/set + rusty::Function operator bool / call are
    // @safe in the library; Log_info template shim is @safe.
    bool check_server_instance(uint64_t new_id) const {
        uint64_t old_id = server_instance_id_.get();

        // Always update the stored ID
        server_instance_id_.set(new_id);

        // Detect restart: old ID was set (non-zero) and differs from new ID
        if (old_id != 0 && old_id != new_id) {
            Log_info("Server restart detected: old_id=%lu new_id=%lu", old_id, new_id);
            if (on_server_restart_) {
                on_server_restart_(old_id, new_id);
            }
            return true;
        }
        return false;
    }

    // === Connection Validation API ===

    /**
     * Configure TCP keepalive for the connection.
     * Should be called before connect() or after reconnection.
     *
     * @param config The keepalive configuration to apply
     */
    // @safe - Uses Cell for interior mutability
    void set_keepalive(const KeepaliveConfig& config) const {
        keepalive_config_.set(config);
    }

    /**
     * Get the current keepalive configuration.
     */
    // @safe - Returns copy via Cell::get()
    KeepaliveConfig keepalive_config() const {
        return keepalive_config_.get();
    }

    /**
     * Configure connection-level heartbeat behavior.
     * Heartbeat probes are emitted by the poll loop when enabled.
     */
    // @safe - Replaces heartbeat manager state/config
    void set_heartbeat_config(const HeartbeatConfig& config) const;

    // @safe - Get current heartbeat configuration
    HeartbeatConfig heartbeat_config() const;

    // @safe - Configure circuit breaker behavior
    void set_circuit_breaker_config(const CircuitBreakerConfig& config) const;

    // @safe - Get current circuit breaker configuration
    CircuitBreakerConfig circuit_breaker_config() const;

    // @safe - Get current circuit breaker state
    CircuitState circuit_breaker_state() const {
        return circuit_breaker_.state();
    }

    /**
     * Update the last activity timestamp.
     * Called on send/receive operations to track connection activity.
     *
     * @param current_time_ms Current time in milliseconds (e.g., from steady_clock)
     */
    // @safe - Stores timestamp in Cell
    void update_last_activity(uint64_t current_time_ms) const {
        last_activity_time_.set(current_time_ms);
    }

    /// Monotonic millisecond clock used by the instrumentation hooks.
    /// @unsafe { std::chrono is not borrow-checked but is memory-safe }
    static uint64_t monotonic_ms_now() {
        auto now = std::chrono::steady_clock::now();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count());
    }

    /// Record one outbound frame's body size + bump the activity clock.
    /// Called from the send-side dispatch path so the metrics counters
    /// and `is_idle()` reflect real I/O. `bytes` is the body length the
    /// channel layer accepted (the 4-byte size prefix is excluded — it
    /// is added by the channel and is constant per send).
    void on_request_dispatched(size_t bytes) const {
        metrics_.record_bytes_sent(static_cast<uint64_t>(bytes));
        update_last_activity(monotonic_ms_now());
    }

    /// Record one inbound frame's body size + bump the activity clock.
    /// Called from `decode_response_and_notify`; `bytes` is the body
    /// length the framing layer delivered.
    void on_response_received(size_t bytes) const {
        metrics_.record_bytes_received(static_cast<uint64_t>(bytes));
        update_last_activity(monotonic_ms_now());
    }

    /**
     * Get the last activity timestamp (milliseconds since epoch).
     */
    // @safe - Simple getter using Cell
    uint64_t last_activity_time() const {
        return last_activity_time_.get();
    }

    /**
     * Check if the connection is idle (no activity for idle_ms milliseconds).
     *
     * @param idle_ms Idle threshold in milliseconds
     * @param current_time_ms Current time in milliseconds (e.g., from steady_clock)
     * @return true if idle for longer than threshold, false otherwise
     */
    // @safe - Simple timestamp comparison
    bool is_idle(uint64_t idle_ms, uint64_t current_time_ms) const {
        auto last = last_activity_time_.get();
        if (last == 0) return false;  // No activity recorded yet
        return (current_time_ms - last) > idle_ms;
    }

    /**
     * Validate the connection is still alive and healthy.
     *
     * Checks:
     * 1. Connection state is CONNECTED
     * 2. Socket is valid (fd >= 0)
     * 3. Socket has no pending errors (getsockopt SO_ERROR)
     *
     * @return true if connection is healthy, false if it needs reconnection
     */
    // @unsafe - Uses getsockopt system call
    bool validate_connection() const;

    /**
     * Get the connection metrics.
     * Returns a const reference to the metrics for this connection.
     */
    // @safe - Simple getter
    // @lifetime: (&'a) -> &'a
    const ConnectionMetrics& metrics() const {
        return metrics_;
    }

private:
    // @safe - Replay queued requests after reconnection
    // Returns number of requests replayed
    size_t replay_pending_requests();

    // @unsafe - Apply keepalive options to socket
    // Called after socket creation in connect()
    void apply_keepalive_options();
    // @unsafe - Builds a Marshal body via operator<< (rusty-cpp treats
    // operator overloads as @unsafe by default) and dispatches it through
    // the channel proxy. The function is internally safe but its body uses
    // patterns the analyzer flags; mark the wrapper @unsafe to match.
    void enqueue_heartbeat_probe() const;
    // @safe - Evaluate circuit breaker gate and update metrics.
    bool allow_request_with_circuit_metrics() const;
    // @safe - Whether an error should be counted as a circuit failure.
    static bool should_trip_circuit_for_error(i32 err);
    // @safe - Record circuit state transitions in metrics.
    void record_circuit_state_transition(CircuitState before, CircuitState after) const;
    // @safe - Record one request result in the circuit breaker.
    void record_circuit_result(i32 err) const;
    // @safe - Map system errno-style code into structured RPC error.
    static RpcError map_system_error(i32 err);
    // @safe - Invoke registered on_error callbacks.
    void invoke_error_callback(i32 err, const std::string& message) const;
    // @safe - Invoke registered on_disconnected callbacks.
    void invoke_disconnected_callback() const;
    // @safe - Invoke registered on_reconnecting callbacks.
    void invoke_reconnecting_callback() const;
    // @safe - Invoke registered on_reconnected callbacks.
    void invoke_reconnected_callback(bool success) const;
    // @safe - Invoke registered on_connected callbacks.
    void invoke_connected_callback() const;

public:
    // @safe - Share callback manager with facade so pre-connect registrations persist.
    void set_callback_manager(const rusty::Arc<CallbackManager>& callback_manager) {
        if (callback_manager.is_valid()) {
            callback_manager_ = callback_manager.clone();
        }
    }


    /**
     * Send an RPC request with a lambda for writing arguments.
     * Thread-safe: lock is acquired and released within this single call.
     *
     * The request packet format is: <size> <xid> <rpc_id> <arg1> <arg2> ... <argN>
     * NOTE: size does not include the size itself (<xid>..<argN>).
     *
     * If disconnected and buffering is enabled (QUEUE behavior), the request
     * is queued for replay after reconnection. The returned Future will be
     * completed when the request is eventually sent and a response received.
     *
     * Returns Result<Arc<Future>, i32>:
     *   - Ok(Arc<Future>) on success (or queued for later)
     *   - Err(error_code) on failure (e.g., ENOTCONN if not connected and buffering disabled)
     */
    // @unsafe - Thread-safe RPC request with lambda for marshaling
    // 4g3b: channel mode is the only path. Always dispatch through
    // `request_via_channel(...)`. The legacy fd-path branch (with
    // `out_.lock()`, state_machine gating, queue_request fallback)
    // has been removed.
    template<typename F>
    FutureResult request(i32 rpc_id, const FutureAttr& attr, F&& write_fn) const {
        return request_via_channel(rpc_id, attr, std::forward<F>(write_fn));
    }

private:
    // @unsafe - Dispatch a fully-marshaled frame body (without the
    // 4-byte size prefix) through the bound channel's FiberChannel
    // wrapper. The proxy's underlying TcpConnection (or in-memory
    // backend) encodes the size prefix internally via
    // `frame_codec_encode_into`.
    //
    // Used by `request_via_channel` and `enqueue_heartbeat_probe`
    // when the client is in channel mode. Returns the channel's
    // ChannelError; callers translate to errno where needed.
    //
    // SAFETY: both `direct_channel_` and `fiber_channel_` are
    // protected by SpinMutex (sub-leaf 4g1). The underlying
    // `send_frame` is non-suspending and the proxy's outbound queue
    // is internally thread-safe.
    ChannelError dispatch_frame_via_channel(const std::uint8_t* body_bytes,
                                            std::size_t body_size) const {
        if (!channel_mode_.get()) return ChannelError::ConnectionReset;
        // 4g1c: direct-channel binding takes precedence over the
        // FiberChannel binding (only one is bound at a time per
        // ClientConnection lifecycle).
        // @unsafe - SpinMutex::lock, Option::as_mut, Box deref
        {
            auto guard = direct_channel_.lock().unwrap();
            if (guard->is_some()) {
                auto& mut_proxy = *guard->as_mut().unwrap();
                return mut_proxy.send_frame(
                    ChannelFrame{body_bytes, body_size});
            }
        }
        // @unsafe - SpinMutex::lock, Option::as_mut, Box deref
        auto guard = fiber_channel_.lock().unwrap();
        if (guard->is_none()) return ChannelError::ConnectionReset;
        return guard->as_mut().unwrap()->send_frame(
            ChannelFrame{body_bytes, body_size});
    }

    /**
     * Channel-mode counterpart of `request`. Mirrors the legacy fd path's bookkeeping (state checks,
     * `pending_fu_` insert, circuit-breaker accounting, metrics) but
     * dispatches the outbound frame through the bound
     * `ChannelConnectionProxy` instead of the legacy `out_` Marshal +
     * `update_mode(WRITE)` plumbing.
     *
     * Frame body layout matches what the legacy path emits at the
     * payload level — `[v64 xid][i32 rpc_id][user-marshaled args]` —
     * so the on-the-wire bytes are identical once the channel layer
     * prepends the 4-byte size header (via
     * `frame_codec_encode_into` inside `TcpConnection::send_frame`).
     *
     * On `ChannelError::WouldBlock` / transport faults, the future
     * is removed from `pending_fu_` and the call returns `EIO`. The
     * RPC layer's existing reconnect / circuit-breaker policy
     * handles the lifecycle from there (sub-leaves 4d / 4e wire the
     * close / on_error callbacks).
     */
    // @unsafe - Counter::next, Marshal operators, channel proxy dispatch
    template<typename F>
    FutureResult request_via_channel(i32 rpc_id,
                                     const FutureAttr& attr,
                                     F&& write_fn) const {
        if (!allow_request_with_circuit_metrics()) {
            return FutureResult::Err(EBUSY);
        }

        // Lazy expiration sweep: keeps the queued-request TTL contract
        // honored without a background timer. Callbacks installed
        // below resolve the corresponding Future with
        // kRequestQueueExpiredError and bump `queue_dropped_requests`.
        pending_queue_.expire_stale();

        // Channel-mode connection state is owned by the channel
        // wrapper; if it reports closed, fail-fast. We also short-
        // circuit on the legacy state machine before reaching the
        // channel proxy: `Client::close` schedules the proxy close
        // asynchronously on the poll thread, so for a brief window
        // the proxy still reports `is_closed() == false` while the
        // state machine has transitioned out of CONNECTED. Requests
        // landing in that window would otherwise succeed against a
        // closing channel; consulting the state machine first
        // guarantees the rejection (and the circuit-breaker
        // transition) the integration tests assert on.
        // 4g1c: check both bindings — direct_channel_ takes
        // precedence over fiber_channel_ when both are present
        // (in practice only one is bound per ClientConnection
        // lifecycle).
        if (!state_machine_.is_connected()) {
            // Buffering: when the user enabled QUEUE behavior we
            // accept the request, park a Future in the pending
            // queue, and let `expire_stale()` or a future
            // replay-on-reconnect path resolve it. The queue's
            // overflow policy (DROP_OLDEST / DROP_NEWEST /
            // FAIL_FAST) decides which entry to drop when
            // `max_pending` is reached.
            if (buffering_config_.enabled &&
                buffering_config_.behavior == DisconnectBehavior::QUEUE) {
                auto fu = Future::create(xid_counter_.next(), attr);
                auto fu_for_cb = fu;  // Arc clone for the callback.
                QueuedRequest qr;
                qr.xid     = fu->xid_;
                qr.rpc_id  = rpc_id;
                qr.ttl_ms  = buffering_config_.default_ttl_ms;
                qr.callback = rusty::Function<void(int)>(
                    [fu_for_cb, this](int err) mutable {
                        // Queue overflow / TTL expiry both count
                        // toward `queue_dropped_requests`. The
                        // future resolves with the queue error
                        // code so callers can distinguish from
                        // ENOTCONN if they care.
                        metrics_.record_queue_drop();
                        fu_for_cb->error_code_.set(err);
                        fu_for_cb->notify_ready(fu_for_cb);
                    });
                if (pending_queue_.enqueue(std::move(qr))) {
                    return FutureResult::Ok(std::move(fu));
                }
                // The queue's overflow callback already fired,
                // resolving the rejected future and bumping the
                // metric. Surface the error to the caller.
                return FutureResult::Err(kRequestQueueRejectedError);
            }
            record_circuit_result(ENOTCONN);
            return FutureResult::Err(ENOTCONN);
        }
        {
            auto direct_guard = direct_channel_.lock().unwrap();
            if (direct_guard->is_some()) {
                auto& proxy = *direct_guard->as_ref().unwrap();
                if (proxy.is_closed()) {
                    record_circuit_result(ENOTCONN);
                    return FutureResult::Err(ENOTCONN);
                }
            } else {
                auto guard = fiber_channel_.lock().unwrap();
                if (guard->is_none() ||
                    guard->as_ref().unwrap()->is_closed()) {
                    record_circuit_result(ENOTCONN);
                    return FutureResult::Err(ENOTCONN);
                }
            }
        }

        // @unsafe { Counter::next }
        auto fu = Future::create(xid_counter_.next(), attr);

        {
            auto pending_guard = pending_fu_.lock().unwrap();
            pending_guard->insert(fu->xid_, fu);
        }

        // Build frame body directly into a contiguous `BufferSink`.
        // Header (`v64 xid`, `i32 rpc_id`) plus user payload (via
        // `write_fn`) accumulate in `body_sink.bytes`
        // (`rusty::Vec<uint8_t>`) and are passed straight to the
        // channel layer — no `Marshal` chunk allocations and no
        // intermediate `body_bytes` copy.  Eliminates one heap
        // allocation + one memcpy per outbound RPC.
        BufferSink body_sink;
        BinaryWriteArchive ar(&body_sink);
        static_assert(std::is_invocable_v<F&, BinaryWriteArchive&>,
                      "request write_fn must accept BinaryWriteArchive&");
        ar << v64(fu->xid_);
        ar << rpc_id;
        write_fn(ar);

        const ChannelError ch_err =
            dispatch_frame_via_channel(body_sink.bytes.data(),
                                       body_sink.bytes.len());
        if (ch_err != ChannelError::None) {
            {
                auto pending_guard = pending_fu_.lock().unwrap();
                pending_guard->remove(fu->xid_);
            }
            record_circuit_result(EIO);
            return FutureResult::Err(EIO);
        }

        metrics_.record_request_sent();
        on_request_dispatched(body_sink.bytes.len());
        return FutureResult::Ok(fu);
    }

    // ------------------------------------------------------------------
    // Slim async-callback request — no `Arc<Future>`, no `Mutex<State>`,
    // no `RefCell<Marshal>` reply buffer.  For callers that don't need
    // to inspect/wait on the reply via a Future handle (the dominant
    // pattern in high-throughput RPC), this shaves ~10% throughput vs
    // `request(...)` by eliminating the Future allocation + HashMap
    // node and replacing the pending-map lookup with a flat-array
    // index.
    //
    // `on_reply` fires on the poll thread when the reply is demuxed
    // (or on the channel's close path with `error_code = ENOTCONN`
    // and reply pointer = nullptr).  The reply byte view is owned by
    // the channel layer's frame buffer for the duration of the
    // callback only — copy out anything you need before returning.
    //
    // Returns Result<void, i32>:
    //   - Ok() if the frame was queued for send.
    //   - Err(error_code) on send-time failure (no callback fires).
public:
    template<typename F>
    rusty::Result<void, i32> request_async(
        i32 rpc_id, F&& write_fn, AsyncReplyCallback on_reply) const {
        if (!allow_request_with_circuit_metrics()) {
            return rusty::Result<void, i32>::Err(EBUSY);
        }
        // Liveness check (mirrors request_via_channel). The state-
        // machine check runs first to close the
        // `Client::close()`-schedules-async-proxy-close race; see the
        // explanatory comment in `request_via_channel`.
        if (!state_machine_.is_connected()) {
            record_circuit_result(ENOTCONN);
            return rusty::Result<void, i32>::Err(ENOTCONN);
        }
        {
            auto direct_guard = direct_channel_.lock().unwrap();
            if (direct_guard->is_some()) {
                auto& proxy = *direct_guard->as_ref().unwrap();
                if (proxy.is_closed()) {
                    record_circuit_result(ENOTCONN);
                    return rusty::Result<void, i32>::Err(ENOTCONN);
                }
            } else {
                auto guard = fiber_channel_.lock().unwrap();
                if (guard->is_none() ||
                    guard->as_ref().unwrap()->is_closed()) {
                    record_circuit_result(ENOTCONN);
                    return rusty::Result<void, i32>::Err(ENOTCONN);
                }
            }
        }

        const i64 xid = xid_counter_.next();
        const size_t slot = static_cast<size_t>(xid) % kAsyncSlotCount;

        // Insert callback into the slim slot.  Collision should be
        // impossible at typical in-flight depths (xid % 16384 unique
        // for in-flight count < 16384).
        {
            auto guard = pending_cb_slots_.lock().unwrap();
            if ((*guard)[slot].is_some()) {
                // Slot collision — caller must drop request and retry,
                // or fall back to `request(...)` (HashMap path).
                record_circuit_result(EBUSY);
                return rusty::Result<void, i32>::Err(EBUSY);
            }
            (*guard)[slot] = rusty::Some(std::move(on_reply));
        }

        // Build frame body — same shape as request_via_channel.
        BufferSink body_sink;
        BinaryWriteArchive ar(&body_sink);
        static_assert(std::is_invocable_v<F&, BinaryWriteArchive&>,
                      "request_async write_fn must accept BinaryWriteArchive&");
        ar << v64(xid);
        ar << rpc_id;
        write_fn(ar);

        const ChannelError ch_err =
            dispatch_frame_via_channel(body_sink.bytes.data(),
                                       body_sink.bytes.len());
        if (ch_err != ChannelError::None) {
            // Cleanup: remove the slot (no callback should fire).
            auto guard = pending_cb_slots_.lock().unwrap();
            (*guard)[slot] = rusty::None;
            record_circuit_result(EIO);
            return rusty::Result<void, i32>::Err(EIO);
        }
        metrics_.record_request_sent();
        on_request_dispatched(body_sink.bytes.len());
        return rusty::Result<void, i32>::Ok();
    }

private:
    // 4g3b: queue_request<F> removed — it was the legacy
    // disconnect-buffering replay helper invoked only by the
    // pre-channel `request()` branch. Channel mode owns its own
    // close-side fan-out (`on_channel_closed_fan_out`) and reconnect
    // path (`reconnect_via_factory`).

public:

    // @safe - Convenience overload; delegates to the @unsafe full request.
    template<typename F>
    FutureResult request(i32 rpc_id, F&& write_fn) const {
        // @unsafe { delegate to @unsafe request(rpc_id, attr, write_fn) }
        {
            return request(rpc_id, FutureAttr(), std::forward<F>(write_fn));
        }
    }

    // @safe - Convenience overload (no args); delegates to the @unsafe full request.
    FutureResult request(i32 rpc_id, const FutureAttr& attr = FutureAttr()) const {
        // @unsafe { delegate to @unsafe request(rpc_id, attr, write_fn) }
        {
            return request(rpc_id, attr, [](BinaryWriteArchive&) {});
        }
    }

    // =========================================================================
    // Request with Options (Timeout/Retry Support)
    // =========================================================================

    /**
     * Send an RPC request with explicit options for timeout and retry.
     * Sets the options on the returned Future for use with wait_with_options().
     *
     * @param rpc_id The RPC method ID
     * @param options Request options (timeout, retry config)
     * @param attr Future attributes (callback, etc.)
     * @param write_fn Lambda to write request arguments
     * @return Result<Arc<Future>, i32>
     */
    // @unsafe - Same as request(), plus sets options
    template<typename F>
    FutureResult request_with_options(i32 rpc_id, const RequestOptions& options,
                                      const FutureAttr& attr, F&& write_fn) const {
        // Serialize args once so retries can replay identical payload safely.
        // write_fn is exclusively
        // BinaryWriteArchive&-shaped now (Marshal& branch removed).
        Marshal serialized_args;
        static_assert(std::is_invocable_v<F&, BinaryWriteArchive&>,
                      "request_with_options write_fn must accept BinaryWriteArchive&");
        MarshalSink sink(&serialized_args);
        BinaryWriteArchive ar(make_sink_proxy(&sink));
        write_fn(ar);
        std::string args_bytes;
        size_t args_size = serialized_args.content_size();
        if (args_size > 0) {
            args_bytes.resize(args_size);
            verify(serialized_args.read(args_bytes.data(), args_size) == args_size);
        }

        // Non-idempotent operations must never be retried even if max_retries is set.
        RequestOptions effective_options = options;
        if (!effective_options.idempotent) {
            effective_options.max_retries = 0;
        }

        // Return a coordinator future immediately; internal attempts run async.
        auto final_fu = Future::create(xid_counter_.next(), attr);
        RequestOptions waiter_options = effective_options;
        waiter_options.timeout_ms = 0;  // Internal attempts own timeout behavior.
        final_fu->set_options(waiter_options);

        auto weak_conn = weak_self_;
        rusty::thread::spawn([weak_conn, rpc_id, effective_options, final_fu, args_bytes = std::move(args_bytes)]() mutable {
            auto start_time = std::chrono::steady_clock::now();
            uint16_t retry_count = 0;

            auto classify_request_failure = [](int err) -> TimeoutType {
                if (err == ENOTCONN || err == ECONNREFUSED || err == ECONNRESET ||
                    err == ECONNABORTED || err == EHOSTUNREACH || err == ENETUNREACH) {
                    return TimeoutType::CONNECT_TIMEOUT;
                }
                if (err == ETIMEDOUT || err == EAGAIN
#if EWOULDBLOCK != EAGAIN
                    || err == EWOULDBLOCK
#endif
                ) {
                    return TimeoutType::REQUEST_TIMEOUT;
                }
                return TimeoutType::NONE;
            };

            auto finish_terminal = [&](int err, TimeoutType timeout_type) {
                auto conn_opt = weak_conn.upgrade();
                if (conn_opt.is_some()) {
                    auto conn = conn_opt.unwrap();
                    if (timeout_type == TimeoutType::CONNECT_TIMEOUT ||
                        timeout_type == TimeoutType::REQUEST_TIMEOUT ||
                        timeout_type == TimeoutType::RESPONSE_TIMEOUT ||
                        timeout_type == TimeoutType::TOTAL_TIMEOUT) {
                        conn->metrics_.record_request_timeout();
                    } else if (err != 0) {
                        conn->metrics_.record_request_failed();
                    }
                }
                if (timeout_type != TimeoutType::NONE) {
                    auto state_guard = final_fu->state_.lock().unwrap();
                    state_guard->timed_out = true;
                }
                final_fu->error_code_.set(err);
                final_fu->timeout_type_.set(timeout_type);
                final_fu->retry_count_.set(retry_count);
                final_fu->notify_ready(final_fu);
            };

            auto set_terminal_timeout = [&](TimeoutType timeout_type) {
                finish_terminal(ETIMEDOUT, timeout_type);
            };

            while (true) {
                auto now = std::chrono::steady_clock::now();
                uint64_t elapsed_ms = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count());
                if (effective_options.is_total_timeout_exceeded(elapsed_ms)) {
                    set_terminal_timeout(TimeoutType::TOTAL_TIMEOUT);
                    return;
                }

                auto conn_opt = weak_conn.upgrade();
                if (conn_opt.is_none()) {
                    finish_terminal(ENOTCONN, TimeoutType::CONNECT_TIMEOUT);
                    return;
                }

                auto conn = conn_opt.unwrap();
                auto attempt_result = conn->request(rpc_id, FutureAttr(), [&](BinaryWriteArchive& m) {
                    if (!args_bytes.empty()) {
                        m.write_bytes(args_bytes.data(), args_bytes.size());
                    }
                });
                if (attempt_result.is_err()) {
                    int err = attempt_result.unwrap_err();
                    finish_terminal(err, classify_request_failure(err));
                    return;
                }

                auto attempt_fu = attempt_result.unwrap();
                RequestOptions attempt_options = effective_options;
                if (effective_options.total_timeout_ms > 0) {
                    uint64_t remaining_ms = effective_options.remaining_time_ms(elapsed_ms);
                    if (remaining_ms == 0) {
                        conn->handle_free(attempt_fu->xid_);
                        set_terminal_timeout(TimeoutType::TOTAL_TIMEOUT);
                        return;
                    }
                    if (attempt_options.timeout_ms == 0 || attempt_options.timeout_ms > remaining_ms) {
                        attempt_options.timeout_ms = remaining_ms;
                    }
                }
                attempt_fu->set_options(attempt_options);
                if (attempt_fu->wait_with_options()) {
                    final_fu->error_code_.set(attempt_fu->error_code_.get());
                    final_fu->retry_count_.set(retry_count);
                    if (attempt_fu->error_code_.get() == 0) {
                        auto attempt_reply = attempt_fu->reply_.borrow_mut();
                        size_t reply_size = attempt_reply->content_size();
                        if (reply_size > 0) {
                            final_fu->reply_.borrow_mut()->read_from_marshal(*attempt_reply, reply_size);
                        }
                    }
                    final_fu->notify_ready(final_fu);
                    return;
                }

                // Timed-out attempts are no longer useful; release pending map slot.
                conn->handle_free(attempt_fu->xid_);

                if (!effective_options.can_retry(retry_count)) {
                    set_terminal_timeout(attempt_fu->get_timeout_type());
                    return;
                }

                conn->metrics_.record_retry_attempt();
                uint64_t backoff_delay_ms = effective_options.calculate_delay_ms(retry_count);
                if (backoff_delay_ms > 0) {
                    if (effective_options.total_timeout_ms > 0) {
                        auto before_sleep = std::chrono::steady_clock::now();
                        uint64_t elapsed_before_sleep = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                before_sleep - start_time).count());
                        uint64_t remaining_ms = effective_options.remaining_time_ms(elapsed_before_sleep);
                        if (remaining_ms == 0 || backoff_delay_ms >= remaining_ms) {
                            set_terminal_timeout(TimeoutType::TOTAL_TIMEOUT);
                            return;
                        }
                    }
                    rusty::thread::sleep(std::chrono::milliseconds(backoff_delay_ms));
                }

                retry_count++;
                final_fu->retry_count_.set(retry_count);
            }
        }).detach();

        return FutureResult::Ok(final_fu);
    }

    // @safe - Convenience overload without FutureAttr; delegates to
    // the @unsafe full request_with_options.
    template<typename F>
    FutureResult request_with_options(i32 rpc_id, const RequestOptions& options,
                                      F&& write_fn) const {
        // @unsafe { delegate to @unsafe request_with_options(rpc_id, options, attr, write_fn) }
        {
            return request_with_options(rpc_id, options, FutureAttr(), std::forward<F>(write_fn));
        }
    }

    // @safe - 4g3c3/4g3d: `ClientConnection` no longer owns an fd; the
    // channel layer's `TcpConnection` does. This accessor always
    // returns -1 and is retained ONLY for `PollableProxy` facade
    // conformance — `PollableTypedArcAdapter<ClientConnection>::fd()`
    // (in `pollable_proxy.h`) is instantiated by deptran's
    // host-scoped retention map (`Reactor::clients_` —
    // `src/deptran/communicator.cc`). Do not add new callers; use
    // `host()` for a peer identifier.
    int fd() const {
        return -1;
    }

    // @safe - Returning std::string by value is a safe copy.
    std::string host() const {
        return host_;
    }

    // @safe - Jetpack: pause/resume for flow control (Cell for interior mutability)
    void pause() const { paused_.set(true); }
    // @safe
    void resume() const { paused_.set(false); }

    // @safe - Returns poll mode based on output buffer
    int poll_mode() const;

    // @safe - 4g3c3: in_/out_ Marshal buffers removed; the channel
    // layer owns frame buffering. Returns 0 for ABI compatibility
    // with the PollableProxy facade.
    size_t content_size() {
        return 0;
    }

    // @safe - Writes buffered data to socket
    // SAFETY: Protected by output spinlock
    // Returns new poll mode, or MODE_NO_CHANGE if no update needed
    int handle_write();

    // @safe - Reads and processes RPC responses
    bool handle_read();

    // @safe - Error handler
    void handle_error() const;

    // @safe - Check heartbeat tick and pending write update flag.
    bool check_pending_write_update() const;

    // @safe - Check if connection was closed
    // Called by poll loop to detect and remove closed connections
    bool is_closed() const {
        return state_machine_.is_terminal();
    }

    // @safe - Jetpack: handle_free for explicit future cleanup
    void handle_free(i64 xid) const;

    // Comparison operator for container support
    friend bool operator==(const rusty::Arc<ClientConnection>& lhs, const rusty::Arc<ClientConnection>& rhs) {
        return lhs.get() == rhs.get();
    }

    // Hash function for containers
    friend struct std::hash<rusty::Arc<ClientConnection>>;
};

}  // export namespace rrr

// std::hash specialization (must be in namespace std, attached to global module)
// from former client.hpp:1965-1974
// @safe - Hash specialization for rusty::Arc<ClientConnection>
namespace std {
template<>
struct hash<rusty::Arc<rrr::ClientConnection>> {
    // @safe - Simple pointer hash
    size_t operator()(const rusty::Arc<rrr::ClientConnection>& arc) const {
        return hash<const rrr::ClientConnection*>()(arc.get());
    }
};
}

// ===========================================================================
// Block 3: Client facade + bulk-reconnect (from former client.hpp:1976-end)
// ===========================================================================
// @safe - third-half namespace block: Client + ClientPool facades.
// Same rules as blocks 1 and 2 — existing class-level and per-method
// annotations stand; network-touching methods retain their
// `// @unsafe`.
export namespace rrr {

// @safe - The interior-mutable `mutable RefCell<...>` field is sound
// because RefCell enforces runtime borrow rules. Methods that drive
// socket I/O through ClientConnection carry their own `// @unsafe`
// overrides; the rest of the class is analyzed as @safe by default.
// Client provides the user-facing API, ClientConnection handles socket I/O.
// Similar to Server/ServerConnection pattern.
class Client {
    // The underlying connection that handles socket I/O
    // RefCell for interior mutability (const methods need to delegate to connection)
    mutable rusty::RefCell<rusty::Option<rusty::Arc<ClientConnection>>> connection_;

    // Shared Arc to PollThread - used to create ClientConnection
    rusty::Arc<PollThread> poll_thread_worker_;

    // Jetpack-specific members using Cell for interior mutability of Copy types
    rusty::Cell<bool> is_client_mode_{false};
    rusty::Cell<long> time_{0};
    rusty::Cell<uint64_t> timeout_{0};
    rusty::Cell<i32> rpc_id_{0};

    // Pending keepalive config (applied when connection is created) - Cell for interior mutability
    rusty::Cell<KeepaliveConfig> pending_keepalive_config_;
    // Pending heartbeat config (applied when connection is created)
    rusty::Cell<HeartbeatConfig> pending_heartbeat_config_{HeartbeatConfig::disabled()};
    // Pending circuit-breaker config (applied when connection is created)
    rusty::Cell<CircuitBreakerConfig> pending_circuit_breaker_config_{CircuitBreakerConfig::disabled()};
    // Pending reconnect config (applied when connection is created)
    rusty::Cell<ReconnectPolicy> pending_reconnect_policy_{ReconnectPolicy()};
    // Shared lifecycle callback manager, wired into active ClientConnection.
    rusty::Arc<CallbackManager> callback_manager_{rusty::Arc<CallbackManager>::make()};

    // pending channel factory.
    //
    // When set via `set_channel_factory(...)`, every subsequent
    // `Client::connect(addr)` pushes a clone-equivalent of this
    // proxy into the freshly-constructed `ClientConnection`. The
    // connection then routes its `connect(addr)` and reconnect
    // spawn through the factory instead of the legacy fd path.
    //
    // SpinMutex because `ChannelFactoryProxy` (a `rusty::Box<…Base>`)
    // is move-only and we need to assign through this const facade
    // without exposing the private member to friends. Box wrapper
    // keeps the indirection shape uniform with
    // `ClientConnection::factory_`.
    // SpinMutex (not RefCell) because Client::connect can be called
    // from any thread and reads/consumes pending_factory_ on each
    // call — sub-leaf 4g1.
    mutable SpinMutex<rusty::Option<ChannelFactoryProxy>> pending_factory_{rusty::Option<ChannelFactoryProxy>(rusty::None)};

public:
    // @safe - Jetpack-specific public members (Cell for interior mutability through Arc)
    // These are accessed through getters/setters for thread-safety
    void set_client_mode(bool v) const { is_client_mode_.set(v); }
    // @safe
    bool client_mode() const { return is_client_mode_.get(); }
    // @safe
    void set_time(long v) const { time_.set(v); }
    // @safe
    long time() const { return time_.get(); }
    // @safe
    void set_timeout(uint64_t v) const { timeout_.set(v); }
    // @safe
    uint64_t timeout() const { return timeout_.get(); }
    // @safe
    void set_rpc_id(i32 v) const { rpc_id_.set(v); }
    // @safe
    i32 rpc_id() const { return rpc_id_.get(); }

    // @safe - Cleanup destructor (has internal @unsafe blocks)
    // SAFETY: Connection cleanup handled by ClientConnection
    virtual ~Client();

    // @safe - Simple initialization
    Client(rusty::Arc<PollThread> poll_thread_worker):
        connection_(rusty::None),
        poll_thread_worker_(poll_thread_worker) { }

    // Factory method to create Client with Arc
    // @safe - Arc::make is @safe in the library.
    static rusty::Arc<Client> create(rusty::Arc<PollThread> poll_thread_worker) {
        return rusty::Arc<Client>::make(poll_thread_worker);
    }

    /**
     * Send an RPC request with a lambda for writing arguments.
     * Thread-safe: lock is acquired and released within this single call.
     *
     * The request packet format is: <size> <xid> <rpc_id> <arg1> <arg2> ... <argN>
     *
     * Returns Result<Arc<Future>, i32>:
     *   - Ok(Arc<Future>) on success
     *   - Err(error_code) on failure (e.g., ENOTCONN if not connected)
     */
    // @safe - Thread-safe RPC request with lambda for marshaling.
    // RefCell::borrow / Option / Cell::set / ClientConnection::request are
    // all @safe at their boundaries; the marshaling write_fn is also @safe.
    template<typename F>
    FutureResult request(i32 rpc_id, const FutureAttr& attr, F&& write_fn) const {
        auto guard = connection_.borrow();
        if (guard->is_none()) {
            return FutureResult::Err(ENOTCONN);
        }
        rpc_id_.set(rpc_id);
        return guard->as_ref().unwrap()->request(rpc_id, attr, std::forward<F>(write_fn));
    }

    // @safe - Convenience overload without callback
    template<typename F>
    FutureResult request(i32 rpc_id, F&& write_fn) const {
        return request(rpc_id, FutureAttr(), std::forward<F>(write_fn));
    }

    // @safe - Convenience overload for requests with no arguments
    FutureResult request(i32 rpc_id, const FutureAttr& attr = FutureAttr()) const {
        return request(rpc_id, attr, [](BinaryWriteArchive&) {});
    }

    // Slim async-callback request — no Arc<Future> allocation.  See
    // ClientConnection::request_async for the full contract.
    template<typename F>
    rusty::Result<void, i32> request_async(
        i32 rpc_id, F&& write_fn,
        ClientConnection::AsyncReplyCallback on_reply) const {
        auto guard = connection_.borrow();
        if (guard->is_none()) {
            return rusty::Result<void, i32>::Err(ENOTCONN);
        }
        rpc_id_.set(rpc_id);
        return guard->as_ref().unwrap()->request_async(
            rpc_id, std::forward<F>(write_fn), std::move(on_reply));
    }

    // =========================================================================
    // Request with Options (Timeout/Retry Support)
    // =========================================================================

    /**
     * Send an RPC request with explicit options for timeout and retry.
     * Sets the options on the returned Future for use with wait_with_options().
     */
    // @safe - Thread-safe RPC request with options.
    template<typename F>
    FutureResult request_with_options(i32 rpc_id, const RequestOptions& options,
                                      const FutureAttr& attr, F&& write_fn) const {
        auto guard = connection_.borrow();
        if (guard->is_none()) {
            return FutureResult::Err(ENOTCONN);
        }
        rpc_id_.set(rpc_id);
        return guard->as_ref().unwrap()->request_with_options(
            rpc_id, options, attr, std::forward<F>(write_fn));
    }

    // @safe - Convenience overload without FutureAttr
    template<typename F>
    FutureResult request_with_options(i32 rpc_id, const RequestOptions& options,
                                      F&& write_fn) const {
        return request_with_options(rpc_id, options, FutureAttr(), std::forward<F>(write_fn));
    }

    // @safe - Sets connection validity
    void set_valid(bool valid) const;
    // @unsafe - Establishes TCP connection (contains const_cast and unsafe connect)
    int connect(const char* addr, bool client = true) const;

    /**
     * Set the reconnection policy for this client.
     * The policy controls automatic reconnection behavior after failures.
     */
    // @safe - Cell::set + RefCell::borrow + Option + ClientConnection delegate.
    void set_reconnect_policy(const ReconnectPolicy& policy) const {
        pending_reconnect_policy_.set(policy);

        auto guard = connection_.borrow();
        if (guard->is_some()) {
            // const_cast needed since ClientConnection::set_reconnect_policy is not const
            auto& conn = const_cast<ClientConnection&>(*guard->as_ref().unwrap());
            conn.set_reconnect_policy(policy);
        }
    }

    /**
     * Set the buffering configuration for this client.
     * Controls whether requests are queued during disconnection.
     */
    // @safe - RefCell ops + inner @unsafe set_buffering_config wrapped @unsafe.
    void set_buffering_config(const BufferingConfig& config) const {
        // @unsafe { RefCell::borrow, Option::unwrap, @unsafe ClientConnection::set_buffering_config }
        {
            auto guard = connection_.borrow();
            if (guard->is_some()) {
                guard->as_ref().unwrap()->set_buffering_config(config);
            }
        }
    }

    /**
     * Install a `ChannelFactoryProxy`.
     *
     * Subsequent `Client::connect(addr)` calls will route through
     * the factory: `factory->connect(addr)` returns a
     * `ChannelConnectionProxy` that the client automatically hands
     * to `ClientConnection::bind_channel(...)`. The legacy
     * socket(2) + connect(2) path is bypassed entirely. The same
     * factory is reused by the close fan-out's reconnect spawn
     * (re-running `factory->connect(addr)`).
     *
     * Calling more than once replaces the previous pending factory
     * for *future* `Client::connect` calls; an already-active
     * `ClientConnection` keeps its previously-bound factory.
     *
     * Calling with a default-constructed (null) proxy is a no-op.
     *
     * Default factory: callers can build a `make_tcp_factory_proxy(
     * rusty::Arc<TcpFactory>::make(poll_thread))` and install it
     * here. The TCP backend is functionally equivalent to the
     * legacy fd path; the abstraction lets test fixtures plug in
     * an in-memory backend (sub-leaf "in-memory channel backend"
     * in the workstream TODO).
     */
    // @unsafe - Records the factory under SpinMutex interior mutability.
    void set_channel_factory(ChannelFactoryProxy factory) const {
        if (!factory) return;
        // SpinMutex::lock + Box move-assign are both @safe.
        {
            auto guard = pending_factory_.lock().unwrap();
            *guard = rusty::Some(std::move(factory));
        }
    }

    // @safe - True if `set_channel_factory` has been called and the
    // factory hasn't been consumed by a `connect` yet.
    bool has_pending_channel_factory() const {
        // SpinMutex::lock and Option::is_some are both @safe.
        auto guard = pending_factory_.lock().unwrap();
        return guard->is_some();
    }

    // @safe - ClientConnection::pending_request_count is @safe.
    size_t pending_request_count() const {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            return guard->as_ref().unwrap()->pending_request_count();
        }
        return 0;
    }

    // @safe - ClientConnection::clear_pending_requests is @safe.
    void clear_pending_requests(int error_code = ECONNABORTED) const {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            guard->as_ref().unwrap()->clear_pending_requests(error_code);
        }
    }

    // @safe - RefCell::borrow + Option ops are @safe.
    bool is_reconnecting() const {
        auto guard = connection_.borrow();
        return guard->is_some() && guard->as_ref().unwrap()->is_reconnecting();
    }

    /**
     * Attempt to reconnect to the last connected address.
     * Can only be called when connection is in FAILED or DISCONNECTED state.
     *
     * @param on_complete Optional callback called with success/failure result
     * @return 0 on success (reconnection started), error code on failure
     */
    // @unsafe - Attempts reconnection (calls connect which has socket operations)
    int reconnect(rusty::Function<void(bool)> on_complete = nullptr) const;

    // @safe - Register lifecycle callbacks.
    // Each callback is wrapped in an Arc<Function const> by CallbackManager so
    // it can be cloned-out under lock and invoked without holding it.
    void add_on_connected(rusty::Function<void() const> cb) const {
        callback_manager_->add_on_connected(std::move(cb));
    }

    // @safe - Register lifecycle callbacks.
    void add_on_disconnected(rusty::Function<void() const> cb) const {
        callback_manager_->add_on_disconnected(std::move(cb));
    }

    // @safe - Register lifecycle callbacks.
    void add_on_error(rusty::Function<void(RpcError, const std::string&) const> cb) const {
        callback_manager_->add_on_error(std::move(cb));
    }

    // @safe - Register lifecycle callbacks.
    void add_on_reconnecting(rusty::Function<void() const> cb) const {
        callback_manager_->add_on_reconnecting(std::move(cb));
    }

    // @safe - Register lifecycle callbacks.
    void add_on_reconnected(rusty::Function<void(bool) const> cb) const {
        callback_manager_->add_on_reconnected(std::move(cb));
    }

    // @safe - Clear all lifecycle callbacks.
    void clear_connection_callbacks() const {
        callback_manager_->clear_all();
    }

    // @safe - Pauses the connection
    void pause() const;
    // @safe - Resumes the connection
    void resume() const;

    // reentrant, could be called multiple times
    // @safe - Closes socket and cleans up
    void close() const;

    // @safe - Jetpack compatibility wrapper
    void close_and_release() {
        close();
    }

    // 4g3d: Client::fd() removed — `Client` no longer owns a socket.
    // The channel layer's `TcpConnection` owns the fd; users that need
    // a peer identifier should use `host()` (or `peer_address()` on
    // the underlying channel proxy in the future).

    // @safe - RefCell::borrow + Option + ClientConnection::host (safe getter).
    std::string host() const {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            return guard->as_ref().unwrap()->host();
        }
        return "";
    }

    // @safe - RefCell::borrow + Option + ClientConnection::connected.
    bool connected() const {
        auto guard = connection_.borrow();
        return guard->is_some() && guard->as_ref().unwrap()->connected();
    }

    // @safe - RefCell::borrow + Option + ClientConnection::connection_state.
    ConnectionState connection_state() const {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            return guard->as_ref().unwrap()->connection_state();
        }
        return ConnectionState::NEW;
    }

    /**
     * Try to reconnect if the connection is in a failed state.
     * Returns true if connection is now available (either was already connected
     * or reconnection succeeded), false if reconnection failed or not possible.
     *
     * This is a convenience method that checks the connection state and
     * attempts reconnection only if needed.
     */
    // @unsafe - May call reconnect which does socket operations
    bool try_reconnect_if_needed() const {
        auto state = connection_state();
        if (state == ConnectionState::CONNECTED) {
            return true;  // Already connected
        }
        if (state == ConnectionState::FAILED || state == ConnectionState::DISCONNECTED) {
            // Try to reconnect
            int result = reconnect();
            return result == 0;
        }
        // CONNECTING or other states - can't help
        return false;
    }

    // @safe - RefCell::borrow + Option + Arc::clone are @safe.
    // Returns None if not connected, Some(Arc<ClientConnection>) if connected.
    rusty::Option<rusty::Arc<ClientConnection>> connection() const {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            return rusty::Some(guard->as_ref().unwrap().clone());
        }
        return rusty::None;
    }

    // === Server Restart Detection API ===

    /**
     * Get the last known server instance ID.
     * Returns 0 if no ID has been set yet or no connection exists.
     */
    // @safe - RefCell::borrow + Option + ClientConnection::server_instance_id.
    uint64_t server_instance_id() const {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            return guard->as_ref().unwrap()->server_instance_id();
        }
        return 0;
    }

    /**
     * Set the callback to be invoked when server restart is detected.
     * The callback receives (old_id, new_id) when the server's instance ID changes.
     *
     * @param callback Function to call on restart detection
     */
    // @safe - Inner ClientConnection::set_on_server_restart is @safe;
    // only the RefCell::borrow + Option::unwrap need an @unsafe wrap.
    void set_on_server_restart(rusty::Function<void(uint64_t, uint64_t)> callback) const {
        // RefCell::borrow + Option::unwrap are both @safe.
        {
            auto guard = connection_.borrow();
            if (guard->is_some()) {
                guard->as_ref().unwrap()->set_on_server_restart(std::move(callback));
            }
        }
    }

    /**
     * Check and update the server instance ID.
     * If the new ID differs from the stored ID (and stored ID was non-zero),
     * the on_server_restart callback is invoked.
     *
     * @param new_id The new server instance ID
     * @return true if server restart was detected, false otherwise
     */
    // @safe - Inner ClientConnection::check_server_instance is @safe;
    // only the RefCell::borrow + Option::unwrap need an @unsafe wrap.
    bool check_server_instance(uint64_t new_id) const {
        // RefCell::borrow + Option::unwrap are both @safe.
        {
            auto guard = connection_.borrow();
            if (guard->is_some()) {
                return guard->as_ref().unwrap()->check_server_instance(new_id);
            }
        }
        return false;
    }

    // === Connection Validation API ===

    /**
     * Configure TCP keepalive for the connection.
     * If called before connect(), the config is stored and applied when connection is created.
     * If called after connect(), the config is applied immediately to the existing connection.
     *
     * @param config The keepalive configuration to apply
     */
    // @safe - Cell::set + RefCell::borrow + Option + ClientConnection::set_keepalive.
    void set_keepalive(const KeepaliveConfig& config) const {
        // Always store locally (for use in connect() if called before connection exists)
        pending_keepalive_config_.set(config);

        // If connection exists, also apply immediately
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            guard->as_ref().unwrap()->set_keepalive(config);
        }
    }

    /**
     * Get the current keepalive configuration.
     * Returns the connection's config if connected, or the pending config otherwise.
     */
    // @safe - Returns copy via Cell::get()
    KeepaliveConfig keepalive_config() const {
        // @unsafe
        {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            return guard->as_ref().unwrap()->keepalive_config();
        }
        }
        // Return pending config if no connection exists
        return pending_keepalive_config_.get();
    }

    /**
     * Configure heartbeat behavior.
     * If called before connect(), the config is stored and applied at connect time.
     * If called after connect(), the config is applied immediately.
     */
    // @safe - Uses Cell for interior mutability; RefCell ops wrapped @unsafe
    void set_heartbeat(const HeartbeatConfig& config) const {
        pending_heartbeat_config_.set(config);

        // RefCell::borrow + Option::unwrap are both @safe via namespace inheritance.
        {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            guard->as_ref().unwrap()->set_heartbeat_config(config);
        }
        }
    }

    // @safe - RefCell ops wrapped @unsafe
    HeartbeatConfig heartbeat_config() const {
        // RefCell::borrow + Option::unwrap are both @safe via namespace inheritance.
        {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            return guard->as_ref().unwrap()->heartbeat_config();
        }
        }
        return pending_heartbeat_config_.get();
    }

    /**
     * Configure circuit breaker behavior.
     * If called before connect(), the config is stored and applied at connect time.
     * If called after connect(), the config is applied immediately.
     */
    // @safe - Uses Cell for interior mutability; RefCell ops wrapped @unsafe
    void set_circuit_breaker(const CircuitBreakerConfig& config) const {
        pending_circuit_breaker_config_.set(config);

        // RefCell::borrow + Option::unwrap are both @safe via namespace inheritance.
        {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            guard->as_ref().unwrap()->set_circuit_breaker_config(config);
        }
        }
    }

    // @safe - RefCell ops wrapped @unsafe
    CircuitBreakerConfig circuit_breaker_config() const {
        // RefCell::borrow + Option::unwrap are both @safe via namespace inheritance.
        {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            return guard->as_ref().unwrap()->circuit_breaker_config();
        }
        }
        return pending_circuit_breaker_config_.get();
    }

    // @safe - RefCell ops wrapped @unsafe
    CircuitState circuit_breaker_state() const {
        // RefCell::borrow + Option::unwrap are both @safe via namespace inheritance.
        {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            return guard->as_ref().unwrap()->circuit_breaker_state();
        }
        }
        return CircuitState::CLOSED;
    }

    /**
     * Check if the connection is idle.
     *
     * @param idle_ms Idle threshold in milliseconds
     * @param current_time_ms Current time in milliseconds (e.g., from steady_clock)
     * @return true if idle for longer than threshold, false if not idle or no connection
     */
    // @safe - Delegates to @safe ClientConnection::is_idle
    bool is_idle(uint64_t idle_ms, uint64_t current_time_ms) const {
        // @unsafe
        {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            return guard->as_ref().unwrap()->is_idle(idle_ms, current_time_ms);
        }
        return false;
        }
    }

    /**
     * Validate the connection is still alive and healthy.
     *
     * @return true if connection is healthy, false if it needs reconnection or doesn't exist
     */
    // @unsafe - Delegates to ClientConnection::validate_connection (uses getsockopt syscall)
    bool validate_connection() const {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            // @unsafe { getsockopt system call }
            return guard->as_ref().unwrap()->validate_connection();
        }
        return false;
    }

    /**
     * Get the connection metrics.
     * Returns reference to empty metrics if no connection exists.
     */
    // @safe - Returns const reference (static empty for no-connection case)
    // @lifetime: (&'a) -> &'a
    const ConnectionMetrics& metrics() const {
        static const ConnectionMetrics empty_metrics;
        // @unsafe
        {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            return guard->as_ref().unwrap()->metrics();
        }
        }
        return empty_metrics;
    }

    /**
     * Check if a connection exists and has metrics.
     * Use this to verify before calling metrics() if you need to distinguish
     * between "no connection" and "connection with zero activity".
     */
    // @safe - Simple connection check
    bool has_connection() const {
        // @unsafe
        {
        auto guard = connection_.borrow();
        return guard->is_some();
        }
    }

    // @safe - Jetpack: handle_free for explicit future cleanup
    void handle_free(i64 xid) const;

};

// @safe - Thread-safe pool of client connections using Arc
// MIGRATED: Now uses rusty::Arc<Client> for cached connections
class ClientPool {
    rrr::Rand rand_;

    // owns a shared reference to PollThread
    rusty::Option<rusty::Arc<rrr::PollThread>> poll_thread_worker_;

    // Mutex-protected state. Bundling cache + load-balancer state in a
    // single SpinMutex matches the access pattern (get_client touches
    // both under one lock) and replaces the prior `SpinLock l_ +
    // unprotected fields` pattern with rusty's RAII guard.
    struct PoolState {
        // @safe - rusty::Arc<Client> for thread-safe reference counting.
        rusty::BTreeMap<std::string, rusty::Vec<rusty::Arc<Client>>> cache;
        // Load balancer state per address (for round-robin tracking).
        rusty::BTreeMap<std::string, LoadBalancerState> lb_state;
    };
    mutable SpinMutex<PoolState> state_;

    // Pool configuration (Cell for interior mutability)
    rusty::Cell<PoolConfig> config_;

    // Helper: Check if a client is considered healthy
    // @safe - Uses metrics to determine health
    bool is_client_healthy(const rusty::Arc<Client>& client) const;

public:
    // @safe - Creates pool with optional PollThread and config
    ClientPool(rusty::Option<rusty::Arc<rrr::PollThread>> poll_thread_worker = rusty::None,
               const PoolConfig& config = PoolConfig::defaults());
    // @safe - Closes all cached connections
    ~ClientPool();

    // === Configuration ===

    // @safe - Set pool configuration
    void set_pool_config(const PoolConfig& config);

    // @safe - Get current pool configuration
    PoolConfig pool_config() const;

    // === Client Access ===

    // return cached client connection
    // on error, return None
    // @unsafe - Gets or creates client connection
    // SAFETY: Contains raw pointer dereference
    rusty::Option<rusty::Arc<rrr::Client>> get_client(const std::string& addr);

    // === Health Management ===

    // @safe - Get count of healthy clients for an address
    size_t get_healthy_client_count(const std::string& addr);

    // @safe - Remove unhealthy clients for an address
    // Returns number of clients removed
    size_t remove_unhealthy_clients(const std::string& addr);

    // @safe - Close idle clients for an address
    // Returns number of clients closed
    // @param current_time_ms Current time in milliseconds (e.g., from steady_clock)
    size_t close_idle_clients(const std::string& addr, uint64_t current_time_ms);

    // === Pool-wide Operations ===

    // @safe - Remove all unhealthy clients from all addresses
    // Returns total number of clients removed
    size_t remove_all_unhealthy();

    // @safe - Close all idle clients from all addresses
    // Returns total number of clients closed
    // @param current_time_ms Current time in milliseconds
    size_t close_all_idle(uint64_t current_time_ms);

    // @safe - Get total number of cached clients across all addresses
    size_t total_client_count();

    // @safe - Get number of addresses with cached clients
    size_t address_count();

    // === Bulk Reconnection ===

    /**
     * Result of a bulk reconnection operation.
     */
    struct BulkReconnectResult {
        size_t total;       // Total clients attempted
        size_t succeeded;   // Number that reconnected successfully
        size_t failed;      // Number that failed to reconnect
        size_t skipped;     // Number skipped (already connected)
    };

    /**
     * Configuration for bulk reconnection.
     */
    struct BulkReconnectConfig {
        uint32_t max_concurrent = 10;     // Max concurrent reconnections
        uint32_t delay_between_ms = 10;   // Delay between batches
        bool skip_connected = true;       // Skip already connected clients

        // @safe - Default constructor creates POD struct
        BulkReconnectConfig() = default;

        // Presets
        // @safe - Pure function creating POD struct
        static BulkReconnectConfig defaults() {
            return BulkReconnectConfig{};
        }

        // @safe - Pure function creating POD struct
        static BulkReconnectConfig fast() {
            BulkReconnectConfig cfg;
            cfg.max_concurrent = 50;
            cfg.delay_between_ms = 0;
            return cfg;
        }

        // @safe - Pure function creating POD struct
        static BulkReconnectConfig gentle() {
            BulkReconnectConfig cfg;
            cfg.max_concurrent = 5;
            cfg.delay_between_ms = 50;
            return cfg;
        }
    };

    /**
     * Reconnect all clients for a specific address.
     *
     * @param addr The address to reconnect clients for
     * @param config Configuration for the bulk operation
     * @return Result containing success/failure counts
     */
    // @unsafe - Calls reconnect on clients
    BulkReconnectResult reconnect_all(const std::string& addr,
                                      const BulkReconnectConfig& config = BulkReconnectConfig::defaults());

    /**
     * Reconnect all clients across all addresses.
     *
     * @param config Configuration for the bulk operation
     * @return Result containing success/failure counts
     */
    // @unsafe - Calls reconnect on clients
    BulkReconnectResult reconnect_all(const BulkReconnectConfig& config = BulkReconnectConfig::defaults());

};

}  // export namespace rrr

// ===========================================================================
// Implementation (from former client.cpp)
// ===========================================================================

// Original client.cpp had `using namespace std;` at TU scope. The
// implementation body relies on unqualified `list<>`, `string`,
// `std::*` shorthand. Re-introduce inside the module's purview so
// the impl block compiles without rewriting hundreds of call sites.
using namespace std;

// @safe - impl namespace. Out-of-class definitions inherit their
// existing per-method `// @safe` / `// @unsafe` annotations from the
// matching declarations in the export blocks above.
namespace rrr {
// Helper function to get current time in milliseconds
// @safe - delegates to rusty::sys::time::clock_monotonic_us, itself @safe.
static uint64_t current_time_ms() {
    return rusty::sys::time::clock_monotonic_us() / 1000;
}

// 4g4: the migration switch (`srpc_use_channel()` and the test-only
// `srpc_set_use_channel_for_testing` / `srpc_reset_use_channel_for_testing`
// helpers) and its env-var triggers (`SRPC_USE_CHANNEL`,
// `SRPC_DISABLE_CHANNEL`) are gone. Channel mode is unconditional;
// `Client::connect` auto-installs a default TCP `ChannelFactoryProxy`
// when none has been bound via `set_channel_factory(...)`.

// ============================================================================
// Future implementation
// ============================================================================

// @unsafe - Uses rusty::Mutex and rusty::Condvar together
void Future::wait() const {
  auto guard = state_.lock().unwrap();
  // wait_while: waits WHILE condition is TRUE, stops when FALSE
  // We want to wait while NOT ready and NOT timed_out
  guard = ready_cond_.wait_while(std::move(guard), [](State& s) {
    return !s.ready && !s.timed_out;
  }).unwrap();
}

// @safe - SpinMutex::lock + Condvar::wait_timeout_while are @safe;
// the only escape is the `std::chrono::duration<double>` ctor.
void Future::timed_wait(double sec) const {
  auto guard = state_.lock().unwrap();
  std::chrono::duration<double> duration;
  // @unsafe { std::chrono::duration ctor is not borrow-checked }
  { duration = std::chrono::duration<double>(sec); }
  // wait_timeout_while: waits WHILE condition is TRUE
  // Returns pair<Guard, bool> where bool = true if condition became false
  auto result = ready_cond_.wait_timeout_while(
    std::move(guard),
    duration,
    [](State& s) { return !s.ready && !s.timed_out; }
  ).unwrap();
  guard = std::move(result.first);
  bool condition_became_false = result.second;

  // If condition is still true (timed out while still waiting)
  if (!condition_became_false && !guard->ready) {
    guard->timed_out = true;
    error_code_.set(ETIMEDOUT);
    timeout_type_.set(TimeoutType::RESPONSE_TIMEOUT);
  }
}

// @unsafe - rusty-cpp false positive: should_callback IS initialized
void Future::notify_ready(rusty::Arc<Future> self) const {
  bool should_callback = false;  // Initialized here
  rusty::Vec<rusty::Function<void()>> completion_callbacks;
  {
    auto guard = state_.lock().unwrap();
    if (!guard->timed_out) {
      guard->ready = true;
    }
    should_callback = guard->ready;
    completion_callbacks = std::move(guard->completion_callbacks);
  }  // Guard dropped here, releasing lock before notify

  ready_cond_.notify_all();

  // rusty::Function::operator bool() reports presence; iterate by
  // mutable ref so we can call non-const operator().
  for (auto& callback : completion_callbacks) {
    if (callback) {
      callback();
    }
  }

  // Execute callback outside lock to avoid deadlock.  The wrapper is
  // copyable (Arc clone = refcount++); we hold a local copy `x` so the
  // user callable stays alive across the invocation even if the
  // FutureAttr field is dropped concurrently.
  if (should_callback && attr_.callback) {
    auto x = attr_.callback;
    x(self);
  }
}

// ============================================================================
// ClientConnection implementation
// ============================================================================

// @safe - Initializes connection (only stores references)
// State machine defaults to NEW state
ClientConnection::ClientConnection(rusty::Arc<PollThread> poll_thread_worker)
    : poll_thread_worker_(poll_thread_worker),
      state_machine_(),
      heartbeat_manager_(HeartbeatConfig::disabled()),
      circuit_breaker_(CircuitBreakerConfig::disabled()),
      callback_manager_(rusty::Arc<CallbackManager>::make()),
      pending_queue_(buffering_config_.to_queue_config()) {
  // Pre-fill the async-callback slot array with `None`s so
  // `pending_cb_slots_[xid % N]` is always a valid in-bounds slot.
  auto guard = pending_cb_slots_.lock().unwrap();
  guard->reserve(kAsyncSlotCount);
  for (size_t i = 0; i < kAsyncSlotCount; ++i) {
    guard->push(rusty::None);
  }
}

// @safe - Simple destructor
ClientConnection::~ClientConnection() {
  reconnect_abort_.store(true, std::memory_order_release);
  reconnecting_.store(false, std::memory_order_release);
  invalidate_pending_futures();
}

// @unsafe - Cancels all pending futures with error, protected by SpinMutex.
// const: every mutation goes through SpinMutex / Counter / Future's
// own const-callable methods.
void ClientConnection::invalidate_pending_futures() const {
  // Drain the slim async-callback slots first.  Move callbacks out
  // under the lock, then fire them outside the lock with ENOTCONN +
  // null reply view.
  rusty::Vec<AsyncReplyCallback> drained_callbacks;
  {
    auto cb_guard = pending_cb_slots_.lock().unwrap();
    for (size_t i = 0; i < cb_guard->len(); ++i) {
      if ((*cb_guard)[i].is_some()) {
        drained_callbacks.push(std::move((*cb_guard)[i].unwrap()));
        (*cb_guard)[i] = rusty::None;
      }
    }
  }
  for (auto& cb: drained_callbacks) {
    metrics_.record_request_dropped();
    cb(ENOTCONN, nullptr, 0);
  }

  list<rusty::Arc<Future>> futures;
  auto guard = pending_fu_.lock().unwrap();
  for (auto it: *guard) {
    futures.push_back(it.second);  // Copy Arc
  }
  guard->clear();  // Clear map (releases its Arc references)
  // Guard dropped here, releasing lock

  for (auto& fu: futures) {
    metrics_.record_request_dropped();
    fu->error_code_.set(ENOTCONN);
    fu->notify_ready(fu);  // Pass Arc to self for callback safety
    // Arc auto-released when list destroyed
  }
}

// @safe - HashMap::get returns Option<V&> now; SpinMutex::lock returns
// LockResult; Arc::clone is @safe. Only notify_ready stays @unsafe.
void ClientConnection::fail_pending_future(i64 xid, int err) const {
  rusty::Option<rusty::Arc<Future>> fu_opt = rusty::None;
  {
    auto pending_guard = pending_fu_.lock().unwrap();
    auto fu_ptr = pending_guard->get(xid);
    if (fu_ptr.is_some()) {
      fu_opt = rusty::Some(fu_ptr.unwrap().clone());
      pending_guard->remove(xid);
    }
  }  // Drop lock before notifying callback/future waiters

  if (fu_opt.is_some()) {
    auto fu = fu_opt.unwrap();
    metrics_.record_request_dropped();
    fu->error_code_.set(err);
    // @unsafe - Future::notify_ready uses interior mutability + callback execution.
    { fu->notify_ready(fu); }
  }
}

// @unsafe - Drives channel proxy close + invalidates futures.
//
// 4g3c3: The legacy `if (socket_ >= 0) ::close(socket_)` block has
// been removed; channel mode is unconditional and the channel layer
// (TcpConnection) owns its own fd. We instead drive `close()` on the
// bound channel proxy(ies). Close is idempotent (channel-layer
// contract), so it's fine if `on_channel_closed_fan_out` then fires
// `on_closed` after this method returns.
// const: every mutation routes through SpinMutex / Cell / Function /
// heartbeat_manager_ — all interior-mutable.
void ClientConnection::close() const {
  ConnectionState prev_state = state_machine_.state();
  const bool was_connected = state_machine_.is_connected();
  if (was_connected) {
    // Transition to DISCONNECTING state while preserving normal lifecycle semantics.
    state_machine_.transition_to(ConnectionState::DISCONNECTING);
  }

  // Tear down the channel proxy(ies). The channel layer's `close()`
  // is idempotent and thread-safe per the facade contract.
  // @unsafe { SpinMutex::lock + Box::get + proxy method dispatch }
  {
    auto guard = direct_channel_.lock().unwrap();
    if (guard->is_some()) {
      auto* conn = guard->as_ref().unwrap().get();
      conn->close();
    }
  }
  // @unsafe { SpinMutex::lock + FiberChannel::close }
  {
    auto guard = fiber_channel_.lock().unwrap();
    if (guard->is_some()) {
      auto* fc = const_cast<FiberChannel*>(
          guard->as_ref().unwrap().get());
      fc->close();
    }
  }

  if (was_connected) {
    // Transition to DISCONNECTED state for clean shutdown.
    state_machine_.transition_to(ConnectionState::DISCONNECTED);
  } else if (!state_machine_.is_terminal()) {
    // If not connected and not already terminal, force to DISCONNECTED.
    state_machine_.force_state(ConnectionState::DISCONNECTED);
  }
  heartbeat_manager_.reset();
  invalidate_pending_futures();

  if (prev_state == ConnectionState::CONNECTED ||
      prev_state == ConnectionState::DISCONNECTING) {
    invoke_disconnected_callback();
  }
}

// @safe - StateMachine is @safe; only std::atomic::store and the call
// into still-@unsafe invalidate_pending_futures need an @unsafe wrap.
// const: state_machine_, reconnect_abort_, and invalidate_pending_futures
// are all const-callable.
void ClientConnection::mark_closing() const {
  // @unsafe { std::atomic::store + invalidate_pending_futures (still @unsafe) }
  {
    reconnect_abort_.store(true, std::memory_order_release);
    if (state_machine_.is_connected()) {
      // Mark as in-progress close, but do not enter terminal state yet.
      // The poll-thread close callback performs the actual fd close and final state transition.
      state_machine_.transition_to(ConnectionState::DISCONNECTING);
    }
    invalidate_pending_futures();
  }
}

// @safe - SpinMutex::lock + HashMap::remove + Counter::record are all @safe.
void ClientConnection::handle_free(i64 xid) const {
  auto guard = pending_fu_.lock().unwrap();
  if (guard->remove(xid).is_some()) {
    metrics_.record_request_dropped();
    // Arc auto-released when removed from map
  }
  // Guard dropped here, releasing lock
}

// @unsafe - Establishes TCP/IPC connection to server
// Contains syscalls, raw pointers, and other unsafe operations
int ClientConnection::connect(const char* addr) {
  verify(!state_machine_.is_connected());

  // Transition to CONNECTING state
  if (!state_machine_.transition_to(ConnectionState::CONNECTING)) {
    Log_error("rrr::ClientConnection: cannot connect from state %s",
              connection_state_to_string(state_machine_.state()));
    invoke_error_callback(EINVAL, "invalid state for connect");
    return EINVAL;
  }

  // channel mode is the only path.
  //
  // Channel mode is non-negotiable post-4g3a, and `Client::connect`
  // always installs a default TCP factory before calling this method
  // (see `Client::connect` for the auto-install logic). The legacy
  // socket(2) + connect(2) + register-pollable path has been deleted.
  //
  // `connect_via_factory` issues `factory->connect(addr)`, hands the
  // returned proxy to `bind_channel_direct(...)`, and records
  // `reconnect_address_` for the close-side reconnect spawn.
  if (!is_factory_bound()) {
    Log_error("rrr::ClientConnection::connect: factory not bound. "
              "Channel mode requires a ChannelFactoryProxy installed via "
              "Client::set_channel_factory(...) or auto-installed by "
              "Client::connect (the latter happens unconditionally now).");
    state_machine_.transition_to(ConnectionState::FAILED);
    invoke_error_callback(EINVAL, "no channel factory bound");
    return EINVAL;
  }
  return connect_via_factory(addr);
}

// @unsafe - Attempts to reconnect to the last connected address
int ClientConnection::reconnect(rusty::Function<void(bool)> on_complete) {
  auto complete_callback = [&](int result) -> int {
    if (on_complete) on_complete(result == 0);
    return result;
  };

  if (reconnect_abort_.load(std::memory_order_acquire)) {
    return complete_callback(ECANCELED);
  }

  auto wait_for_inflight_reconnect = [&]() -> int {
    while (reconnecting_.load(std::memory_order_acquire)) {
      if (reconnect_abort_.load(std::memory_order_acquire)) {
        return ECANCELED;
      }
      if (state_machine_.is_connected()) {
        return 0;
      }
      rusty::thread::sleep(std::chrono::milliseconds(5));
    }

    if (state_machine_.is_connected()) {
      return 0;
    }
    return INT_MIN;
  };

  if (reconnecting_.load(std::memory_order_acquire)) {
    int waited = wait_for_inflight_reconnect();
    if (waited != INT_MIN) {
      return complete_callback(waited);
    }
  }

  // Check if we have an address to reconnect to
  if (reconnect_address_.empty()) {
    Log_error("rrr::ClientConnection: no address to reconnect to");
    return complete_callback(EINVAL);
  }

  // Can only reconnect from FAILED or DISCONNECTED state
  if (!state_machine_.can_connect()) {
    Log_error("rrr::ClientConnection: cannot reconnect from state %s",
              connection_state_to_string(state_machine_.state()));
    return complete_callback(EINVAL);
  }

  while (true) {
    bool expected = false;
    if (reconnecting_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      break;
    }

    int waited = wait_for_inflight_reconnect();
    if (waited != INT_MIN) {
      return complete_callback(waited);
    }
  }
  invoke_reconnecting_callback();

  auto complete_reconnect = [&](bool success, int result) -> int {
    reconnecting_.store(false, std::memory_order_release);
    invoke_reconnected_callback(success);

    if (success) {
      Log_info("rrr::ClientConnection: reconnected to %s", reconnect_address_.c_str());

      // Record reconnection in metrics
      metrics_.record_reconnect();

      // Sweep the disconnect-buffering queue. Entries that ran past
      // their TTL while the connection was down resolve their
      // futures with `kRequestQueueExpiredError` and bump
      // `queue_dropped_requests`. Non-stale entries remain in the
      // queue for a future replay path.
      pending_queue_.expire_stale();
      return complete_callback(0);
    } else {
      if (result == ECANCELED) {
        Log_debug("rrr::ClientConnection: reconnect cancelled for %s",
                  reconnect_address_.c_str());
      } else {
        Log_error("rrr::ClientConnection: reconnection failed to %s: %d",
                  reconnect_address_.c_str(), result);
      }
      return complete_callback(result);
    }
  };

  auto reconnect_once = [&]() -> int {
    if (reconnect_abort_.load(std::memory_order_acquire)) {
      return ECANCELED;
    }
    // 4g3c2: `socket_ = -1` reset removed. socket_ is unused in
    // channel mode (the channel proxy's TcpConnection owns the fd);
    // the `connect()` call below routes through `connect_via_factory`
    // which produces a fresh proxy + fresh fd internally.
    return connect(reconnect_address_.c_str());
  };

  if (reconnect_abort_.load(std::memory_order_acquire)) {
    return complete_reconnect(false, ECANCELED);
  }

  // Another reconnect attempt can complete between the pre-CAS state check and
  // this thread acquiring reconnect ownership.
  if (state_machine_.is_connected()) {
    return complete_reconnect(true, 0);
  }

  if (!state_machine_.can_connect()) {
    return complete_reconnect(false, EINVAL);
  }

  // First attempt happens immediately.
  int result = reconnect_once();
  if (result == 0) {
    return complete_reconnect(true, 0);
  }

  // Follow configured backoff/retry policy for subsequent attempts.
  ReconnectCalculator calc(reconnect_policy_);
  while (calc.should_retry()) {
    if (reconnect_abort_.load(std::memory_order_acquire)) {
      return complete_reconnect(false, ECANCELED);
    }

    uint32_t delay_ms = calc.next_delay_ms();
    if (delay_ms > 0) {
      rusty::thread::sleep(std::chrono::milliseconds(delay_ms));
    }

    if (reconnect_abort_.load(std::memory_order_acquire)) {
      return complete_reconnect(false, ECANCELED);
    }

    // Another path may have re-established connection while sleeping.
    if (state_machine_.is_connected()) {
      return complete_reconnect(true, 0);
    }

    if (!state_machine_.can_connect()) {
      return complete_reconnect(false, EINVAL);
    }

    Log_debug("rrr::ClientConnection: reconnect retry #%u to %s",
              calc.retry_count(), reconnect_address_.c_str());
    result = reconnect_once();
    if (result == 0) {
      return complete_reconnect(true, 0);
    }
  }

  return complete_reconnect(false, result);
}

// @unsafe - Uses interior mutability (const method modifying mutable members)
void ClientConnection::set_buffering_config(const BufferingConfig& config) const {
  // @unsafe - struct assignment operator
  { buffering_config_ = config; }

  // Clear any pending requests since config changed
  // Note: We can't recreate the queue (mutex not movable), so just clear
  // @unsafe - const propagation through mutable member
  {
    if (!pending_queue_.empty()) {
      pending_queue_.clear_all(ECONNABORTED);
    }

    // Update the queue's internal config to match
    pending_queue_.update_config(config.to_queue_config());
  }
}

// @safe - HeartbeatManager is @safe; Weak copy-assign is now @safe; the
// lambda body only calls @safe methods + Log_warn (a @safe template shim).
// One inner @unsafe block remains for the const_cast.
void ClientConnection::set_heartbeat_config(const HeartbeatConfig& config) const {
  heartbeat_manager_.set_config(config);
  WeakClientConnection weak_conn = weak_self_;
  heartbeat_manager_.set_on_timeout([weak_conn]() {
    auto conn_opt = weak_conn.upgrade();
    if (conn_opt.is_none()) {
      return;
    }
    auto conn = conn_opt.unwrap();
    if (!conn->connected()) {
      return;
    }
    Log_warn("rrr::ClientConnection: heartbeat timeout for %s", conn->host().c_str());
    // handle_error is const-callable; conn.get() returns const T* but
    // that's fine now.
    conn->handle_error();
  });
}

// @safe - HeartbeatManager class is @safe; config() returns by value.
HeartbeatConfig ClientConnection::heartbeat_config() const {
  return heartbeat_manager_.config();
}

// @safe - CircuitBreaker class is @safe; set_config is @safe.
void ClientConnection::set_circuit_breaker_config(const CircuitBreakerConfig& config) const {
  circuit_breaker_.set_config(config);
}

// @safe - CircuitBreaker class is @safe; config() returns by value.
CircuitBreakerConfig ClientConnection::circuit_breaker_config() const {
  return circuit_breaker_.config();
}

// @safe - No-op stub returning a constant. (The RequestQueue methods
// it nominally documents are themselves @safe in Tier 2 anyway.)
// 4g3c2: replay_pending_requests() reduced to a no-op stub. The
// underlying queue (`pending_queue_`) is always empty in channel mode
// because `queue_request<F>(...)` was deleted in 4g3b. The function
// itself is kept for the test-only accessor
// `replay_pending_requests_for_test()` (used by 3 DISABLED_*
// buffering tests as documentation of prior behavior). It returns
// the dequeue count, which is 0 by construction now.
size_t ClientConnection::replay_pending_requests() {
  return 0;
}

// @unsafe - Enqueue one internal heartbeat probe through the bound
// channel proxy.
//
// 4g3c3: legacy fd path removed. Channel mode is the only path; the
// `out_` Marshal that backed the fd path is gone. Callers (the
// poll-loop tick) only fire heartbeats on connected clients, which
// always have a bound channel by construction.
void ClientConnection::enqueue_heartbeat_probe() const {
  // Build the heartbeat frame body and dispatch through the channel
  // proxy. The channel layer adds the 4-byte size prefix internally;
  // the body bytes match what the legacy fd path's
  // `set_bookmark` / `write_bookmark` produced, so the wire format
  // is unchanged.
  Marshal body;
  body << v64(xid_counter_.next());
  body << static_cast<i32>(kInternalHeartbeatRpcId);
  const std::size_t body_size = body.content_size();
  std::vector<std::uint8_t> body_bytes;
  if (body_size > 0) {
    body_bytes.resize(body_size);
    verify(body.read(body_bytes.data(), body_size) == body_size);
  }
  // Errors here are observable via the `on_error` callback when
  // sub-leaf 4d wires it; for now we ignore the return code, same
  // as the legacy fd path which never surfaced send-side errors
  // from the heartbeat probe.
  (void)dispatch_frame_via_channel(body_bytes.data(), body_size);
}

// @unsafe - Reset channel-mode state for a factory-driven reconnect
//. Drops the closed FiberChannel,
// flips `channel_mode_` off, and forces the state machine to
// DISCONNECTED so `connect()`'s `verify(!is_connected())` passes.
// Caller: the spawn body inside `on_channel_closed_fan_out` when a
// factory is bound.
void ClientConnection::reset_channel_mode_for_reconnect() {
  // SpinMutex::lock + Option::take are both @safe.
  {
    auto guard = fiber_channel_.lock().unwrap();
    *guard = rusty::None;
  }
  // 4g1c: also drop the direct-channel slot so reconnect can rebind
  // a fresh proxy with fresh callbacks.
  // SpinMutex::lock + Option::take are both @safe.
  {
    auto guard = direct_channel_.lock().unwrap();
    *guard = rusty::None;
  }
  channel_mode_.set(false);
  state_machine_.force_state(ConnectionState::DISCONNECTED);
}

// @unsafe - Channel-factory connect path.
//
// Calls the bound `ChannelFactoryProxy::connect(addr)` to obtain a
// `ChannelConnectionProxy`, then hands it to `bind_channel(...)`.
// Mirrors the legacy fd-path's bookkeeping: records the address for
// reconnect, transitions the state machine to CONNECTED on success,
// invokes the connected callback, and reports errors through the
// usual `invoke_error_callback` path. Caller is `connect(addr)`,
// which already transitioned the state to CONNECTING and verified
// the factory binding.
int ClientConnection::connect_via_factory(const char* addr) {
  // Take a *clone* of the bound factory so we can call `connect` on
  // it without holding the RefCell guard across what may be a
  // blocking syscall (TCP handshake, address resolution). The
  // ChannelFactoryProxy's underlying type (e.g. TcpFactory wrapped in
  // an Arc<TcpFactory> adapter) is reference-counted, so copying the
  // proxy is cheap. We don't have a generic clone() on
  // rusty::Box<ChannelFactoryBase>, so we use the proxy in place
  // through the Box wrapper while the SpinMutex guard is held.
  // @unsafe { SpinMutex::lock + ChannelFactoryProxy copy }
  {
    auto guard = factory_.lock().unwrap();
    if (guard->is_none()) {
      Log_error(
          "rrr::ClientConnection::connect_via_factory: factory unbound at "
          "the moment of connect (race against bind_factory)");
      state_machine_.transition_to(ConnectionState::FAILED);
      invoke_error_callback(ENOTCONN, "factory unbound");
      return ENOTCONN;
    }
    // The proxy (rusty::Box<ChannelFactoryBase>) is move-only; we
    // can't clone. Use it in place via the Box wrapper. The
    // SpinMutex guard is held across the connect() syscall — the
    // caller's perspective is that
    // connect is synchronous (channel-layer contract), and the
    // factory itself is read-only (bind_factory is essentially
    // one-shot per Client lifecycle), so holding the lock briefly
    // while we issue the syscall doesn't introduce contention with
    // the dispatch path (which locks `fiber_channel_`, not
    // `factory_`).
    auto* bound = guard->as_ref().unwrap().get();
    ConnectResult result = bound->connect(std::string_view(addr));
    if (result.error != ChannelError::None || result.connection.is_none()) {
      const auto err_str = std::string("factory connect failed: ")
          + channel_error_to_string(result.error);
      Log_error("rrr::ClientConnection: %s (addr=%s)", err_str.c_str(), addr);
      state_machine_.transition_to(ConnectionState::FAILED);
      // Map the channel error onto an errno-shaped value the legacy
      // call sites expect.
      const int rc = (result.error == ChannelError::ConnectionRefused)
                       ? ECONNREFUSED
                     : (result.error == ChannelError::AddressInvalid)
                       ? EINVAL
                       : ENOTCONN;
      invoke_error_callback(rc, err_str);
      return rc;
    }
    // Sub-leaf 4g1c: bypass FiberChannel + recv-loop fiber entirely.
    // Install on_frame/on_closed callbacks directly on the channel
    // proxy. on_frame runs on the poll thread (where the channel
    // layer fires it) and calls decode_response_and_notify inline —
    // no IntEvent, no fiber yield, no waiting_events_ churn. This
    // works around the deeper reactor/fiber wedge documented in 4g1b.
    bind_channel_direct(result.connection.unwrap());
  }

  // Record address for the close fan-out's reconnect spawn — it
  // re-runs the factory connect with the same target. std::string
  // assignment from a const char* is benign in @safe code.
  reconnect_address_ = addr;

  // Mirror the fd path's terminal transition: the channel layer's
  // own state (proxy.is_closed()) becomes the source of truth, but
  // we still drive the legacy state machine through CONNECTED so
  // existing health-check / metric APIs (`connected()`,
  // `connection_state()`) keep working.
  if (!state_machine_.transition_to(ConnectionState::CONNECTED)) {
    state_machine_.force_state(ConnectionState::CONNECTED);
  }
  // Record connect timestamp so `metrics_.connect_time_ms()` is
  // non-zero from the moment a request can be issued. The metric
  // tests assert `> 0`; the absolute value (steady-clock-relative)
  // is informational.
  {
    uint64_t now = current_time_ms();
    metrics_.record_connect(now);
    // Seed `last_activity_time_` so `is_idle()` measures time since
    // connect (or since the most recent send/recv) rather than
    // returning false forever because no I/O has happened yet.
    update_last_activity(now);
  }
  invoke_connected_callback();
  return 0;
}

// @unsafe - Spawns recv-loop fiber, constructs FiberChannel wrapper.
//
//   - 4a flipped the `channel_mode_` latch.
//   - 4b routed outbound frames through the proxy.
//   - 4c2 wraps the proxy in a `FiberChannel` and spawns a recv-loop
//     fiber that drives response demux from `recv_frame()` calls.
//
// The fiber is spawned on the *current* thread's reactor. Per the
// channel-layer threading contract, the proxy's callbacks fire on the
// reactor that owns the underlying connection — typically the poll
// thread for production TCP, or the test thread for fake channels.
// Calling `bind_channel` from any other thread leaves the recv-loop
// fiber on the wrong reactor and would race the IntEvent signaling
// path. Cross-thread scheduling of the spawn is sub-leaf 4e's
// concern; for 4c2 we document the constraint and rely on the
// caller.
void ClientConnection::bind_channel(ChannelConnectionProxy channel) {
  if (!channel) return;

  // Move the proxy into a heap-allocated `FiberChannel` so the
  // recv-loop fiber can hold a stable pointer to the wrapper across
  // its parking lifetime. `FiberChannel` is move-deleted (its
  // callbacks capture `this`), so we use `make_box` which constructs
  // in-place via perfect-forwarded `new` rather than moving.
  // rusty::make_box + SpinMutex::lock + Option::operator= are all @safe.
  {
    auto guard = fiber_channel_.lock().unwrap();
    *guard = rusty::Some(rusty::make_box<FiberChannel>(std::move(channel)));
  }
  channel_mode_.set(true);

  // Capture a Weak<> so the parked fiber doesn't extend the
  // connection's lifetime (which would create a cycle via
  // `fiber_channel_` ownership).
  WeakClientConnection weak_self = weak_self_;

  // Spawn the recv-loop fiber on the *current* thread's reactor.
  // Per the channel-layer threading contract, the recv-loop fiber
  // must live on the same reactor that fires the proxy's
  // `on_frame` / `on_closed` callbacks (so the `IntEvent` it parks
  // on can be signaled cross-fiber within one thread —
  // cross-thread `IntEvent::set` is unsafe). Caller is responsible
  // for choosing the right thread:
  //   - Fake-channel unit tests call `bind_channel(...)` from the
  //     test thread, where they also drive `deliver()` /
  //     `deliver_closed()`. The recv-loop fiber lives on the test
  //     thread; everything stays single-threaded.
  //   - Production TCP / factory paths use
  //     `bind_channel_via_poll_thread(...)` (sub-leaf 4f) which
  //     submits a `OneTimeJob` to the poll thread, where the
  //     spawn — and therefore the resulting fiber — lands on the
  //     same reactor that fires `TcpConnection::handle_read`'s
  //     `on_frame` callback.
  Fiber::create_run([weak_self]() mutable {
    auto conn_opt = weak_self.upgrade();
    if (conn_opt.is_none()) return;
    auto conn = conn_opt.unwrap();
    auto* mut_conn = const_cast<ClientConnection*>(conn.get());
    mut_conn->run_recv_loop();
  }, __FILE__, __LINE__);
}

// @unsafe - Channel-mode bind that schedules the recv-loop fiber
// spawn onto the *poll thread*.
//
// Used by production code paths (factory-driven `connect` /
// reconnect) that run on the user thread but need the recv-loop
// fiber on the poll thread — same thread the channel proxy's
// callbacks fire on. Submits a `OneTimeJob` whose `Work()` runs
// `run_recv_loop()` from a fiber that the poll thread's
// `trigger_job` spawns on its own reactor.
void ClientConnection::bind_channel_via_poll_thread(
    ChannelConnectionProxy channel) {
  if (!channel) return;

  // Move the proxy into the heap-allocated FiberChannel and flip
  // the latch on the calling thread — these are pure data
  // mutations and the recv-loop fiber doesn't observe them until
  // after we submit the OneTimeJob below.
  // rusty::make_box + SpinMutex::lock + Option::operator= are all @safe.
  {
    auto guard = fiber_channel_.lock().unwrap();
    *guard = rusty::Some(rusty::make_box<FiberChannel>(std::move(channel)));
  }
  channel_mode_.set(true);

  WeakClientConnection weak_self = weak_self_;

  // Schedule the recv-loop fiber spawn onto the poll thread. The
  // poll thread's `trigger_job` calls `Fiber::create_run` from
  // its own reactor, so the resulting fiber's IntEvent waits and
  // the `on_frame` callback's signal both land on the same
  // thread.
  // @unsafe { Arc::new_ + rusty::Function + cross-thread queue }
  auto recv_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob([weak_self]() {
    auto conn_opt = weak_self.upgrade();
    if (conn_opt.is_none()) return;
    auto conn = conn_opt.unwrap();
    auto* mut_conn = const_cast<ClientConnection*>(conn.get());
    mut_conn->run_recv_loop();
  }));
  // Upcast Arc<OneTimeJob> -> Arc<Job> for the PollThread queue.
  auto recv_job_base = rusty::Arc<Job>(recv_job);
  poll_thread_worker_->add(std::move(recv_job_base));
}

// @unsafe - Direct on_frame / on_closed callback binding.
//
// Bypasses FiberChannel + recv-loop fiber entirely. Installs the
// callbacks directly on the channel proxy:
//   - on_frame: builds a copy of the frame bytes (the channel-layer
//     contract makes the frame.payload pointer valid only during the
//     callback) and calls decode_response_and_notify on the same
//     thread the channel layer fires on. For TCP, that's the poll
//     thread — same thread that handles_read parses frames.
//   - on_closed: invokes on_channel_closed_fan_out on the same
//     thread.
//
// The proxy's other thread-safety properties carry over: send_frame
// is callable from any thread (we use it that way from
// dispatch_frame_via_channel in user threads).
//
// Stores the proxy in `direct_channel_`. The connection's
// `Arc<ClientConnection>` lifetime is captured weakly in the
// callbacks, so the connection can be torn down without leaving a
// dangling pointer in the proxy's installed callbacks. When
// `direct_channel_` is destroyed, the proxy's destructor drops the
// callbacks, so any in-flight callback dispatch from the channel
// layer must complete before drop is allowed (this matches the
// FiberChannel destructor's contract).
void ClientConnection::bind_channel_direct(ChannelConnectionProxy channel) {
  if (!channel) return;

  // Capture a weak ref so the proxy's installed callbacks don't
  // extend the ClientConnection's lifetime (avoids a refcount cycle
  // through `direct_channel_` + the callbacks).
  WeakClientConnection weak_self = weak_self_;

  // Install callbacks BEFORE moving the proxy into the slot. After
  // the move, the proxy lives in `direct_channel_`; the lambdas
  // capture only the weak self-ref.
  // @unsafe { lambda capture, channel proxy mutator }
  channel->set_on_frame([weak_self](const ChannelFrame& f) {
    auto conn_opt = weak_self.upgrade();
    if (conn_opt.is_none()) return;
    auto conn = conn_opt.unwrap();
    auto* mut_conn = const_cast<ClientConnection*>(conn.get());
    mut_conn->decode_response_and_notify(f.payload, f.size);
  });
  channel->set_on_closed([weak_self](ChannelError /*reason*/) {
    auto conn_opt = weak_self.upgrade();
    if (conn_opt.is_none()) return;
    auto conn = conn_opt.unwrap();
    auto* mut_conn = const_cast<ClientConnection*>(conn.get());
    mut_conn->on_channel_closed_fan_out();
  });
  // on_error is not surfaced to the RPC layer in this binding mode
  // (the channel-layer contract follows fatal errors with on_closed,
  // so on_channel_closed_fan_out covers the recovery path).
  channel->set_on_error([](ChannelError, std::string_view) {});

  // Move the proxy into the slot and flip the channel-mode latch.
  // SpinMutex::lock + Option::operator= are both @safe.
  {
    auto guard = direct_channel_.lock().unwrap();
    *guard = rusty::Some(std::move(channel));
  }
  channel_mode_.set(true);
}

// @unsafe - Drives Marshal / Future / pending_fu_ from a fiber.
//
// Recv-loop body: blocks on `FiberChannel::recv_frame()` and forwards
// each frame's body to `decode_response_and_notify`. Returns when
// the channel closes (recv_frame returns None) or when the wrapper
// goes away.
//
// We resolve the FiberChannel raw pointer ONCE under a brief lock
// and then drop the SpinMutex guard — `recv_frame()` yields the
// fiber (parking on an `IntEvent`), and holding a lock across the
// yield would block other threads racing on `dispatch_frame_via_channel`
// (or, on the same reactor, prevent other fibers from running). The
// raw pointer stays valid because the spawning lambda keeps an
// `Arc<ClientConnection>` alive for the fiber's lifetime, and the
// connection owns the `Box<FiberChannel>`.
void ClientConnection::run_recv_loop() {
  FiberChannel* fc = nullptr;
  {
    auto guard = fiber_channel_.lock().unwrap();
    if (guard->is_none()) return;
    // @unsafe { Box::get returns raw pointer }
    fc = const_cast<FiberChannel*>(guard->as_ref().unwrap().get());
  }
  while (true) {
    rusty::Option<OwnedFrame> frame_opt = fc->recv_frame();
    if (frame_opt.is_none()) {
      // Channel closed. Run the close-side fan-out (sub-leaf 4d):
      // cancel pending futures with ENOTCONN, fire error /
      // disconnected callbacks, and trigger auto-reconnect if the
      // policy allows. The fiber then exits, dropping its
      // Arc<ClientConnection> capture so the connection can finish
      // teardown if no other strong refs remain.
      on_channel_closed_fan_out();
      return;
    }
    auto frame = std::move(frame_opt).unwrap();
    decode_response_and_notify(frame.bytes.data(), frame.bytes.size());
  }
}

// @unsafe - Marshal operators, Future::notify_ready, pending_fu_ map.
//
// Decode one response frame body and resolve the matching pending
// future. The body layout mirrors the legacy fd path's payload (i.e.,
// what arrives after the 4-byte size prefix in `client.cpp::handle_read`):
//
//     [v64 reply_xid][v32 error_code][v64 server_instance_id][user-marshaled reply]
//
// The channel layer consumes the size prefix (and with it the
// `kResponseHeaderExtFlag` bit), so we lose the runtime signal that
// distinguishes legacy responses (no instance ID) from extended
// responses (with instance ID). The current SRPC server always emits
// the extended form (`server.hpp::reply` sets
// `include_instance_id = true`), so channel mode unconditionally
// reads the instance ID. Sub-leaf 4f's migration switch / parity
// pass will revisit if a legacy-server interop path needs the bit
// surfaced through `ChannelFrame`.
void ClientConnection::decode_response_and_notify(const std::uint8_t* bytes,
                                                  std::size_t size) {
  // Account for every inbound frame body byte and bump the activity
  // clock so `metrics_.bytes_received()` and `is_idle()` reflect real
  // I/O regardless of which dispatch slot the reply maps onto.
  on_response_received(size);
  // parse the response header directly from
  // the input bytes via BufferSource + BinaryReadArchive — no
  // intermediate `Marshal body` allocation.  The payload tail (if
  // any) is written into the matching Future's `reply_` Marshal via
  // a single byte-copy.  BufferSource bounds reads to `size`, so a
  // truncated frame aborts inside `BinaryReadArchive::read_exact`
  // (matches the legacy `Marshal::operator>>` behaviour on short
  // reads).
  BufferSource src(bytes, size);
  BinaryReadArchive ar(&src);

  v64 v_reply_xid;
  v32 v_error_code;
  // See the function-header note: in channel mode the extended-header
  // flag is consumed by the framing layer.  We assume the server
  // always emits the extended form (matches `server.hpp` today).
  v64 v_server_instance_id;
  ar >> v_reply_xid >> v_error_code >> v_server_instance_id;
  check_server_instance(static_cast<uint64_t>(v_server_instance_id.get()));

  size_t parsed_header_size = src.pos();
  size_t response_payload_bytes = size - parsed_header_size;
  heartbeat_manager_.on_pong_received();

  // Fast path: slim async-callback slot (request_async users).
  // Check first — for callback-only callers this is the dominant
  // pattern and we can avoid touching the HashMap entirely.
  {
    const size_t slot = static_cast<size_t>(v_reply_xid.get())
                          % kAsyncSlotCount;
    rusty::Option<AsyncReplyCallback> cb_opt = rusty::None;
    {
      auto guard = pending_cb_slots_.lock().unwrap();
      if ((*guard)[slot].is_some()) {
        cb_opt = std::move((*guard)[slot]);
        (*guard)[slot] = rusty::None;
      }
    }
    if (cb_opt.is_some()) {
      auto cb = std::move(cb_opt.unwrap());
      const i32 err_code = static_cast<i32>(v_error_code.get());
      if (err_code == 0) {
        metrics_.record_request_completed();
      } else {
        metrics_.record_request_failed();
      }
      record_circuit_result(err_code);
      cb(err_code, bytes + parsed_header_size, response_payload_bytes);
      return;
    }
  }

  rusty::Option<rusty::Arc<Future>> fu_opt = rusty::None;
  {
    auto guard = pending_fu_.lock().unwrap();
    auto fu_ptr = guard->get(v_reply_xid.get());
    if (fu_ptr.is_some()) {
      fu_opt = rusty::Some(fu_ptr.unwrap().clone());
      guard->remove(v_reply_xid.get());
    }
  }

  if (fu_opt.is_some()) {
    auto fu = fu_opt.unwrap();
    verify(fu->xid_ == v_reply_xid.get());
    fu->error_code_.set(v_error_code.get());
    if (response_payload_bytes > 0) {
      fu->reply_.borrow_mut()->write(bytes + parsed_header_size,
                                     response_payload_bytes);
    }

    if (v_error_code.get() == 0) {
      metrics_.record_request_completed();
    } else {
      metrics_.record_request_failed();
    }
    record_circuit_result(v_error_code.get());

    fu->notify_ready(fu);
  }
  // No matching future (timed out or replaced) → drop the payload.
  // The legacy fd path drained the bytes through a throwaway Marshal
  // to keep its chunk list balanced, but that was an idiom of the
  // legacy fd reader; with channel-mode framing the input bytes are
  // owned by the caller and freed on return — nothing to drain.
}

// @unsafe - Channel-mode close fan-out.
//
// Mirrors the legacy fd path's `handle_error` for channel-mode
// connections: when the recv-loop fiber sees `recv_frame()` return
// None (channel closed by peer or transport fault), this method
// runs the same reliability fan-out:
//
//   1. Force the connection state to FAILED (unless the user
//      already initiated the close — DISCONNECTING/DISCONNECTED).
//   2. Invoke the error callback with ECONNRESET (only on
//      non-user-initiated paths).
//   3. Reset the heartbeat manager so a future reconnect starts
//      from a clean baseline.
//   4. Invalidate every pending future (`ENOTCONN`).
//   5. Invoke the disconnected callback (only on
//      non-user-initiated paths, matching `close()`'s contract).
//   6. If `reconnect_policy_.auto_reconnect` is set and a
//      `reconnect_address_` was recorded, increment the
//      `channel_reconnect_attempts_` counter and spawn a thread
//      that will call `reconnect()`. Sub-leaf 4e replaces the
//      legacy `reconnect()` body with a factory-driven path; for
//      4d the spawn is observable through the counter without
//      requiring tests to actually drive the fd reconnect.
//
// Skips the socket-close half of `close()` (`::close(socket_)`,
// state transitions through DISCONNECTING) — channel mode never
// owned the fd, and the channel layer has already torn down its
// underlying transport.
void ClientConnection::on_channel_closed_fan_out() {
  ConnectionState prev_state = state_machine_.state();
  const bool user_initiated_closing =
      prev_state == ConnectionState::DISCONNECTING ||
      prev_state == ConnectionState::DISCONNECTED ||
      reconnect_abort_.load(std::memory_order_acquire);

  if (!user_initiated_closing) {
    invoke_error_callback(ECONNRESET, "channel closed");
    state_machine_.force_state(ConnectionState::FAILED);
  }

  heartbeat_manager_.reset();
  invalidate_pending_futures();

  if (!user_initiated_closing) {
    invoke_disconnected_callback();
  }

  // Trigger auto-reconnect if the policy allows. Channel-mode
  // reconnect is wired in sub-leaf 4e (factory-based); for 4d we
  // bump an observable counter the moment the fan-out reaches the
  // reconnect-policy branch, then conditionally spawn the legacy
  // fd reconnect path. The counter is the observability signal:
  // tests verify it incremented by setting
  // `reconnect_abort_=true` (so the spawn short-circuits without
  // actually calling `reconnect()`). Production callers that want
  // a real reconnect leave the abort flag false and rely on the
  // spawn.
  if (reconnect_policy_.auto_reconnect &&
      // std::string::empty() is a pure const accessor, safe in @safe code.
      !reconnect_address_.empty()) {
    channel_reconnect_attempts_.fetch_add(1, std::memory_order_acq_rel);

    if (reconnect_abort_.load(std::memory_order_acquire)) {
      // Caller requested no reconnect (typically: connection
      // tearing down). Counter is still bumped for observability.
      return;
    }
    auto weak_conn = weak_self_;
    rusty::thread::spawn([weak_conn]() {
      auto conn_opt = weak_conn.upgrade();
      if (conn_opt.is_none()) {
        return;
      }
      auto conn = conn_opt.unwrap();
      if (!conn->reconnect_policy_.auto_reconnect ||
          conn->reconnect_abort_.load(std::memory_order_acquire)) {
        return;
      }
      auto state = conn->connection_state();
      if (state == ConnectionState::FAILED ||
          state == ConnectionState::DISCONNECTED) {
        // factory-driven reconnect.
        //
        // When a `ChannelFactoryProxy` is bound, the fan-out's
        // reconnect spawn re-runs the same factory connect path
        // that the original `connect(addr)` took (factory ->
        // connect -> bind_channel) instead of the legacy fd
        // `reconnect()` (which re-opens a raw socket). The
        // factory-aware path also re-arms the recv-loop fiber via
        // `bind_channel`, so a successful reconnect resumes
        // request demux without a manual setup step.
        auto* mut_conn = const_cast<ClientConnection*>(conn.get());
        if (mut_conn == nullptr) {
          return;
        }
        if (conn->is_factory_bound()) {
          Log_info(
              "rrr::ClientConnection: channel-mode auto-reconnect "
              "(factory) triggered after on_closed");
          // Reset the channel-mode latch + drop the stale
          // FiberChannel before calling connect again — connect's
          // verify(!is_connected()) requires the state machine to
          // be non-CONNECTED, and the new bind_channel needs the
          // option slot empty so it can install fresh callbacks.
          mut_conn->reset_channel_mode_for_reconnect();
          // `connect` reads `reconnect_address_` itself (set by
          // the original connect call), so we just call it.
          (void)mut_conn->connect(conn->reconnect_address_.c_str());
          return;
        }
        Log_info(
            "rrr::ClientConnection: channel-mode auto-reconnect (legacy) "
            "triggered after on_closed");
        // @unsafe - reconnect mutates socket/state and performs network I/O.
        mut_conn->reconnect();
      }
    }).detach();
  }
}

// @safe - CircuitBreaker and ConnectionMetrics are both @safe classes;
// record_circuit_state_transition is @safe.
bool ClientConnection::allow_request_with_circuit_metrics() const {
  CircuitState before = circuit_breaker_.state();
  bool allowed = circuit_breaker_.allow_request();
  CircuitState after = circuit_breaker_.state();
  record_circuit_state_transition(before, after);
  if (!allowed) {
    metrics_.record_circuit_open_rejection();
  }
  return allowed;
}

// @safe - Checks whether an error should contribute to circuit tripping.
bool ClientConnection::should_trip_circuit_for_error(i32 err) {
  switch (err) {
    case 0:
      return false;
    case ENOTCONN:
    case ECONNREFUSED:
    case ECONNRESET:
    case ECONNABORTED:
    case ETIMEDOUT:
    case EHOSTUNREACH:
    case ENETUNREACH:
    case EPIPE:
      return true;
    default:
      return false;
  }
}

// @safe - Track circuit breaker state transitions in metrics.
void ClientConnection::record_circuit_state_transition(
    CircuitState before,
    CircuitState after) const {
  if (before == after) {
    return;
  }

  switch (after) {
    case CircuitState::OPEN:
      metrics_.record_circuit_open_transition();
      break;
    case CircuitState::HALF_OPEN:
      metrics_.record_circuit_half_open_transition();
      break;
    case CircuitState::CLOSED:
      metrics_.record_circuit_closed_transition();
      break;
    default:
      break;
  }
}

// @safe - CircuitBreaker is @safe; should_trip_circuit_for_error is @safe;
// record_circuit_state_transition is @safe.
void ClientConnection::record_circuit_result(i32 err) const {
  CircuitState before = circuit_breaker_.state();
  if (err == 0) {
    circuit_breaker_.record_success();
  } else if (should_trip_circuit_for_error(err)) {
    circuit_breaker_.record_failure();
  }
  CircuitState after = circuit_breaker_.state();
  record_circuit_state_transition(before, after);
}

// @safe - Maps errno-style errors into structured RpcError categories.
RpcError ClientConnection::map_system_error(i32 err) {
  switch (err) {
    case 0:
      return RpcError::OK;
    case ENOTCONN:
      return RpcError::NOT_CONNECTED;
    case ECONNREFUSED:
      return RpcError::CONNECTION_REFUSED;
    case ECONNRESET:
      return RpcError::CONNECTION_RESET;
    case ENETUNREACH:
      return RpcError::NETWORK_UNREACHABLE;
    case EHOSTUNREACH:
      return RpcError::HOST_UNREACHABLE;
    case ECONNABORTED:
    case EPIPE:
      return RpcError::CONNECTION_CLOSED;
    case EBUSY:
      return RpcError::CIRCUIT_OPEN;
    case ETIMEDOUT:
      return RpcError::RESPONSE_TIMEOUT;
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
      return RpcError::REQUEST_TIMEOUT;
    case EINVAL:
      return RpcError::INVALID_ARGUMENT;
    default:
      return RpcError::UNKNOWN_ERROR;
  }
}

// @safe - CallbackManager is @safe; Arc::operator bool is @safe;
// map_system_error is @safe.
void ClientConnection::invoke_error_callback(i32 err, const std::string& message) const {
  if (!callback_manager_) {
    return;
  }
  callback_manager_->invoke_on_error(map_system_error(err), message);
}

// @safe - CallbackManager is @safe; Arc::operator bool is @safe.
void ClientConnection::invoke_disconnected_callback() const {
  if (!callback_manager_) {
    return;
  }
  callback_manager_->invoke_on_disconnected();
}

// @safe - CallbackManager is @safe; Arc::operator bool is @safe.
void ClientConnection::invoke_reconnecting_callback() const {
  if (!callback_manager_) {
    return;
  }
  callback_manager_->invoke_on_reconnecting();
}

// @safe - CallbackManager is @safe; Arc::operator bool is @safe.
void ClientConnection::invoke_reconnected_callback(bool success) const {
  if (!callback_manager_) {
    return;
  }
  callback_manager_->invoke_on_reconnected(success);
}

// @safe - CallbackManager is @safe; Arc::operator bool is @safe.
void ClientConnection::invoke_connected_callback() const {
  if (!callback_manager_) {
    return;
  }
  callback_manager_->invoke_on_connected();
}

// @unsafe - Error handler - transitions to FAILED state.
// const: state_machine_, atomics (mutable), close/invoke_*_callback,
// and the reconnect spawn are all callable through a const ref.
void ClientConnection::handle_error() const {
  ConnectionState prev_state = state_machine_.state();
  const bool user_initiated_closing =
      prev_state == ConnectionState::DISCONNECTING ||
      prev_state == ConnectionState::DISCONNECTED ||
      reconnect_abort_.load(std::memory_order_acquire);

  if (!user_initiated_closing) {
    invoke_error_callback(ECONNRESET, "connection error");
    // Force transition to FAILED state (from any state)
    state_machine_.force_state(ConnectionState::FAILED);
  }
  // @unsafe - calls close() which does system calls
  { close(); }

  if (user_initiated_closing) {
    return;
  }
  invoke_disconnected_callback();

  // Trigger policy-driven reconnect automatically after transport failures.
  if (reconnect_policy_.auto_reconnect &&
      !reconnect_abort_.load(std::memory_order_acquire)) {
    // std::string::empty() is a pure const accessor; safe in @safe code.
    if (reconnect_address_.empty()) {
      return;
    }
    auto weak_conn = weak_self_;
    rusty::thread::spawn([weak_conn]() {
        auto conn_opt = weak_conn.upgrade();
        if (conn_opt.is_none()) {
          return;
        }

        auto conn = conn_opt.unwrap();
        if (!conn->reconnect_policy_.auto_reconnect ||
            conn->reconnect_abort_.load(std::memory_order_acquire)) {
          return;
        }

        auto state = conn->connection_state();
        if (state == ConnectionState::FAILED || state == ConnectionState::DISCONNECTED) {
          Log_info("rrr::ClientConnection: auto-reconnect triggered after connection failure");
          // @unsafe - reconnect mutates socket/state and performs network I/O.
          auto* mut_conn = const_cast<ClientConnection*>(conn.get());
          if (mut_conn != nullptr) {
            mut_conn->reconnect();
          }
        }
      }).detach();
  }
}

// @unsafe - Poll-loop heartbeat tick.
//
// 4g3c3: ClientConnection is no longer registered as a Pollable on
// the poll thread, so this method is unreachable from the poll loop
// itself. The heartbeat manager is still driven from internal
// timers; this method is preserved on the Pollable facade for ABI
// compatibility (deptran's `Reactor::clients_` still wraps
// ClientConnection in `PollableProxy` for host-scoped lifetime
// retention — see `src/deptran/communicator.cc`). The body retains
// the heartbeat probe so any caller that does drive it (e.g. tests
// invoking through the proxy) keeps working. The
// `pending_write_update_` flag has been removed: it gated the
// legacy fd-path's write-mode flip, which the channel layer now
// owns internally.
bool ClientConnection::check_pending_write_update() const {
  if (state_machine_.is_connected() && !paused_.get()) {
    if (heartbeat_manager_.check_timeout()) {
      // Timeout callback already transitioned connection through error handling.
      return false;
    }
    if (heartbeat_manager_.should_send_heartbeat()) {
      // @unsafe
      {
        enqueue_heartbeat_probe();
      }
      heartbeat_manager_.on_heartbeat_sent();
      return true;
    }
  }
  return false;
}

// 4g3c3: ClientConnection no longer implements the Pollable role.
// The channel layer's TcpConnection owns the fd and the
// handle_read/write/error duty. These overrides remain for ABI
// compatibility (PollableProxy facade conformance via the templated
// adapter) but their bodies are no-ops.
int ClientConnection::handle_write() {
  return PollMode::NO_CHANGE;
}

bool ClientConnection::handle_read() {
  return false;
}

int ClientConnection::poll_mode() const {
  return PollMode::READ;
}

// ============================================================================
// Client implementation (facade that delegates to ClientConnection)
// ============================================================================

// @unsafe - Cleanup destructor, uses request_close() for thread-safe close
Client::~Client() {
  close();  // Delegate to close() which uses request_close()
}

// @safe - 4g3c3: legacy `out_` Marshal removed; `valid_id` was a flag
// on the (now-deleted) outbound buffer used by the Python jetpack
// bindings. In channel mode, outbound framing is owned by the
// channel layer and there's no per-connection `valid_id` flag to
// flip. Kept on the API surface for binding compatibility; behavior
// is now a no-op.
void Client::set_valid(bool valid) const {
  (void)valid;
}

// @unsafe - 4g3c3: schedules ClientConnection::close on the poll thread.
//
// The legacy `request_close(fd)` path was for fd-path teardown
// ordering: the close() body needed to run on the poll thread to
// avoid racing with pending `CmdAddPollable` commands. The same
// ordering constraint exists in channel mode — the TcpConnection's
// `add_proxy` call is asynchronous (it queues `CmdAddPollable` to
// the poll thread). If `Client::close` were to drive
// `TcpConnection::close()` from the user thread, the proxy's `fd()`
// could read -1 by the time the poll thread processed the still-
// queued `CmdAddPollable`, tripping `Epoll::Add`'s fd>=0 verify.
//
// We instead submit a `OneTimeJob` to the poll thread; by the time
// it runs, every `CmdAddPollable` queued before the job has already
// been processed (the MPSC queue + per-thread reactor preserves
// command ordering). The job drives `ClientConnection::close()`
// which closes the channel proxy synchronously on the poll thread
// (no further enqueue).
//
// Note: does NOT clear the connection object so reconnect() can work.
// The connection object retains the address for reconnection.
void Client::close() const {
  auto guard = connection_.borrow_mut();
  if (guard->is_some()) {
    auto& conn = const_cast<ClientConnection&>(*guard->as_ref().unwrap());
    const bool was_connected = conn.connected();
    conn.mark_closing();
    if (was_connected) {
      // @unsafe - schedules channel proxy close on poll thread; uses
      // const_cast inside the lambda and calls non-borrow-checked
      // PollThread::add (an unannotated reactor primitive).
      {
        auto conn_arc = guard->as_ref().unwrap().clone();
        auto close_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob([conn_arc]() {
          // close() is const-callable; conn_arc.get() returns const T*.
          conn_arc->close();
        }));
        auto close_job_base = rusty::Arc<Job>(close_job);
        poll_thread_worker_->add(std::move(close_job_base));
      }
    }
    // Don't clear connection to None - we need it for reconnect()
  }
}

// @safe - Inner ClientConnection::handle_free is now @safe; only the
// RefCell::borrow + Option::unwrap need an @unsafe wrap.
void Client::handle_free(i64 xid) const {
  // RefCell::borrow + Option::unwrap are both @safe.
  {
    auto guard = connection_.borrow();
    if (guard->is_some()) {
      guard->as_ref().unwrap()->handle_free(xid);
    }
  }
}

// @safe - Inner ClientConnection::pause is @safe (Cell::set);
// only the RefCell::borrow + Option::unwrap need an @unsafe wrap.
void Client::pause() const {
  // RefCell::borrow + Option::unwrap are both @safe.
  {
    auto guard = connection_.borrow();
    if (guard->is_some()) {
      guard->as_ref().unwrap()->pause();
    }
  }
}

// @safe - Inner ClientConnection::resume is @safe (Cell::set);
// only the RefCell::borrow + Option::unwrap need an @unsafe wrap.
void Client::resume() const {
  // RefCell::borrow + Option::unwrap are both @safe.
  {
    auto guard = connection_.borrow();
    if (guard->is_some()) {
      guard->as_ref().unwrap()->resume();
    }
  }
}

// @unsafe - Establishes TCP/IPC connection to server
// Uses Arc::get_mut() for exclusive mutable access during initialization
int Client::connect(const char* addr, bool client) const {
  // Create the ClientConnection
  auto conn = rusty::Arc<ClientConnection>::make(poll_thread_worker_);

  // Use get_mut() since we're the sole owner (strong_count == 1)
  // This is Rust's idiomatic pattern for init-before-sharing
  auto opt = conn.get_mut();
  verify(opt.is_some());  // Must succeed for freshly-created Arc
  ClientConnection& mut_conn = opt.unwrap();

  // Initialize fields through mutable reference (no const_cast needed).
  // Weak pointer move-assign is @safe since the Tier-1.3 sweep.
  mut_conn.weak_self_ = conn;
  mut_conn.set_callback_manager(callback_manager_);
  mut_conn.is_client_mode_ = client;
  is_client_mode_.set(client);

  // Apply pending keepalive config before connecting
  mut_conn.set_keepalive(pending_keepalive_config_.get());
  // Apply pending heartbeat config before connecting
  mut_conn.set_heartbeat_config(pending_heartbeat_config_.get());
  // Apply pending circuit-breaker config before connecting
  mut_conn.set_circuit_breaker_config(pending_circuit_breaker_config_.get());
  // Apply pending reconnect policy before connecting
  mut_conn.set_reconnect_policy(pending_reconnect_policy_.get());

  // 4g4: channel mode is unconditional — auto-install a default TCP
  // `ChannelFactoryProxy` if the caller hasn't already bound one via
  // `set_channel_factory(...)`. The connection's `connect(addr)` and
  // reconnect spawn route through `factory->connect(addr)` ->
  // `bind_channel_direct(...)`.
  if (!has_pending_channel_factory()) {
    auto tcp_factory =
        rusty::Arc<TcpFactory>::make(poll_thread_worker_);
    set_channel_factory(make_tcp_factory_proxy(std::move(tcp_factory)));
  }

  // push the pending channel factory
  // into the new ClientConnection. Once bound, the connection's
  // `connect(addr)` and reconnect spawn route through the factory
  // (`factory->connect(addr)` -> `bind_channel(...)`) instead of
  // the legacy fd path. Take the proxy by std::move because
  // `ChannelFactoryProxy` (rusty::Box<ChannelFactoryBase>) is
  // move-only; the factory is a one-shot push per Client lifecycle
  // (re-bind via `set_channel_factory` to install a different one —
  // affects subsequent Client::connect calls).
  // @unsafe { SpinMutex::lock + ChannelFactoryProxy move }
  {
    auto guard = pending_factory_.lock().unwrap();
    if (guard->is_some()) {
      // Move the proxy out of the Option directly — the alias
      // is `rusty::Box<ChannelFactoryBase>`, so unwrap() yields
      // the move-only Box, no inner deref needed.
      ChannelFactoryProxy moved = std::move(*guard).unwrap();
      mut_conn.bind_factory(std::move(moved));
      *guard = rusty::None;  // single-use; tests can re-bind
    }
  }

  // Call connect through mutable reference
  int result = 0;
  // @unsafe - Low-level TCP/IPC connection (or factory-driven)
  {
    result = mut_conn.connect(addr);
  }

  if (result == 0) {
    // Connection successful, store it
    auto guard = connection_.borrow_mut();
    *guard = rusty::Some(std::move(conn));
  }

  return result;
}

// @unsafe - Attempts to reconnect to the last connected address
int Client::reconnect(rusty::Function<void(bool)> on_complete) const {
  auto guard = connection_.borrow();
  if (guard->is_none()) {
    Log_error("rrr::Client: no connection to reconnect");
    if (on_complete) on_complete(false);
    return ENOTCONN;
  }

  // Need to get mutable access to the connection for reconnect
  auto& conn = const_cast<ClientConnection&>(*guard->as_ref().unwrap());
  conn.reconnect_abort_.store(false, std::memory_order_release);

  // @unsafe - reconnect does socket operations
  {
    // rusty::Function is move-only — std::move into the inner call.
    return conn.reconnect(std::move(on_complete));
  }
}


// ============================================================================
// ClientPool implementation
// ============================================================================

// @safe - Constructs pool with PollThread ownership
ClientPool::ClientPool(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker /* =? */,
                       const PoolConfig& config /* =? */)
    : config_(config) {

  verify(config.min_connections > 0);
  verify(config.max_connections >= config.min_connections);
  if (poll_thread_worker.is_none()) {
    poll_thread_worker_ = rusty::Some(PollThread::create());
  } else {
    poll_thread_worker_ = std::move(poll_thread_worker);
  }
}

// @safe - Set pool configuration
void ClientPool::set_pool_config(const PoolConfig& config) {
  config_.set(config);
}

// @safe - Get current pool configuration
PoolConfig ClientPool::pool_config() const {
  return config_.get();
}

// @unsafe - Check if a client is considered healthy
bool ClientPool::is_client_healthy(const rusty::Arc<Client>& client) const {
  auto cfg = config_.get();

  // If health checking is disabled, all clients are considered healthy
  if (!cfg.health_check_enabled) {
    return true;
  }

  // Must be connected to be healthy
  if (!client->connected()) {
    return false;
  }

  // Check metrics-based health
  const auto& metrics = client->metrics();
  auto requests_sent = metrics.requests_sent();

  // Not enough data to judge health
  if (requests_sent < cfg.min_requests_for_health) {
    return true;  // Assume healthy until proven otherwise
  }

  // Check success rate
  auto success_rate = metrics.success_rate_percent();
  return success_rate >= cfg.unhealthy_threshold_percent;
}

// @safe - Destroys pool and all cached connections
ClientPool::~ClientPool() {
  // rusty::BTreeMap iter `operator*()` returns
  // `std::tuple<const K&, V&>` (post-2026-04 API).
  auto guard = state_.lock().unwrap();
  for (auto&& [_addr, clients] : guard->cache) {
    for (auto& client : clients) {
      client->close();
    }
  }

  // Shutdown PollThread if we own it
  if (poll_thread_worker_.is_some()) {
    poll_thread_worker_.as_ref().unwrap()->shutdown();
  }
}

// @safe - SpinMutex::lock + BTreeMap ops + is_client_healthy are all @safe.
size_t ClientPool::get_healthy_client_count(const std::string& addr) {
  auto guard = state_.lock().unwrap();
  size_t count = 0;
  auto clients_opt = guard->cache.get(addr);
  if (clients_opt.is_some()) {
    // rusty::BTreeMap::get returns `Option<V&>` (post-2026-04
    // API), so unwrap() yields a reference, not a pointer.
    auto& clients = clients_opt.unwrap();
    for (const auto& client : clients) {
      if (is_client_healthy(client)) {
        count++;
      }
    }
  }
  return count;
}

// @safe - SpinMutex::lock + BTreeMap/Vec ops + is_client_healthy are @safe.
size_t ClientPool::remove_unhealthy_clients(const std::string& addr) {
  auto guard = state_.lock().unwrap();
  size_t removed = 0;
  auto clients_opt = guard->cache.get(addr);
  if (clients_opt.is_some()) {
    // BTreeMap::get returns `Option<V&>`; unwrap() yields a
    // reference. Use `.` instead of `->`, drop the `*` deref.
    auto& clients = clients_opt.unwrap();
    auto cfg = config_.get();

    // Remove unhealthy clients, but keep at least min_connections.
    rusty::Vec<rusty::Arc<Client>> kept;
    kept.reserve(clients.len());
    for (const auto& client : clients) {
      if (clients.len() - removed <= static_cast<size_t>(cfg.min_connections)) {
        kept.push(client.clone());
        continue;
      }
      if (!is_client_healthy(client)) {
        client->close();
        removed++;
      } else {
        kept.push(client.clone());
      }
    }
    clients = std::move(kept);

    // Remove empty entries from cache
    if (clients.is_empty()) {
      guard->cache.remove(addr);
    }
  }
  return removed;
}

// @safe - SpinMutex::lock + BTreeMap/Vec ops + is_idle/close are @safe.
size_t ClientPool::close_idle_clients(const std::string& addr, uint64_t current_time_ms) {
  auto cfg = config_.get();

  // If idle timeout is 0, no timeout
  if (cfg.idle_timeout_ms == 0) {
    return 0;
  }

  auto guard = state_.lock().unwrap();
  size_t closed = 0;
  auto clients_opt = guard->cache.get(addr);
  if (clients_opt.is_some()) {
    // BTreeMap::get returns `Option<V&>`.
    auto& clients = clients_opt.unwrap();

    rusty::Vec<rusty::Arc<Client>> kept;
    kept.reserve(clients.len());
    for (const auto& client : clients) {
      if (clients.len() - closed <= static_cast<size_t>(cfg.min_connections)) {
        kept.push(client.clone());
        continue;
      }
      if (client->is_idle(cfg.idle_timeout_ms, current_time_ms)) {
        client->close();
        closed++;
      } else {
        kept.push(client.clone());
      }
    }
    clients = std::move(kept);

    if (clients.is_empty()) {
      guard->cache.remove(addr);
    }
  }
  return closed;
}

// @safe - SpinMutex::lock + BTreeMap/Vec ops are @safe.
size_t ClientPool::remove_all_unhealthy() {
  auto guard = state_.lock().unwrap();
  size_t total_removed = 0;
  auto cfg = config_.get();

  // BTreeMap::keys() now returns `keys_range` (a transient
  // iterator-shaped object), not `Vec<K>`. Drain into a Vec so the
  // subsequent loop body — which mutates `cache` via `remove(...)`
  // — doesn't iterate while modifying.
  rusty::Vec<std::string> keys;
  {
    auto it = guard->cache.keys();
    keys.reserve(it.len());
    for (auto opt = it.next(); opt.is_some(); opt = it.next()) {
      keys.push(std::string(opt.unwrap()));
    }
  }
  rusty::Vec<std::string> empty_keys;
  for (const auto& addr : keys) {
    auto clients_opt = guard->cache.get(addr);
    if (clients_opt.is_none()) {
      continue;
    }
    // BTreeMap::get returns `Option<V&>`.
    auto& clients = clients_opt.unwrap();
    size_t removed = 0;
    rusty::Vec<rusty::Arc<Client>> kept;
    kept.reserve(clients.len());
    for (const auto& client : clients) {
      if (clients.len() - removed <= static_cast<size_t>(cfg.min_connections)) {
        kept.push(client.clone());
        continue;
      }
      if (!is_client_healthy(client)) {
        client->close();
        removed++;
      } else {
        kept.push(client.clone());
      }
    }
    clients = std::move(kept);
    total_removed += removed;
    if (clients.is_empty()) {
      empty_keys.push(addr);
    }
  }
  for (const auto& addr : empty_keys) {
    guard->cache.remove(addr);
  }
  return total_removed;
}

// @safe - SpinMutex::lock + BTreeMap/Vec ops are @safe.
size_t ClientPool::close_all_idle(uint64_t current_time_ms) {
  auto cfg = config_.get();
  if (cfg.idle_timeout_ms == 0) {
    return 0;
  }

  auto guard = state_.lock().unwrap();
  size_t total_closed = 0;

  // same drain pattern as remove_all_unhealthy above —
  // BTreeMap::keys() returns a transient `keys_range`.
  rusty::Vec<std::string> keys;
  {
    auto it = guard->cache.keys();
    keys.reserve(it.len());
    for (auto opt = it.next(); opt.is_some(); opt = it.next()) {
      keys.push(std::string(opt.unwrap()));
    }
  }
  rusty::Vec<std::string> empty_keys;
  for (const auto& addr : keys) {
    auto clients_opt = guard->cache.get(addr);
    if (clients_opt.is_none()) {
      continue;
    }
    auto& clients = clients_opt.unwrap();
    size_t closed = 0;
    rusty::Vec<rusty::Arc<Client>> kept;
    kept.reserve(clients.len());
    for (const auto& client : clients) {
      if (clients.len() - closed <= static_cast<size_t>(cfg.min_connections)) {
        kept.push(client.clone());
        continue;
      }
      if (client->is_idle(cfg.idle_timeout_ms, current_time_ms)) {
        client->close();
        closed++;
      } else {
        kept.push(client.clone());
      }
    }
    clients = std::move(kept);
    total_closed += closed;
    if (clients.is_empty()) {
      empty_keys.push(addr);
    }
  }
  for (const auto& addr : empty_keys) {
    guard->cache.remove(addr);
  }
  return total_closed;
}

// @safe - SpinMutex::lock + BTreeMap/Vec ops are @safe.
size_t ClientPool::total_client_count() {
  auto guard = state_.lock().unwrap();
  size_t count = 0;
  // BTreeMap iter returns `tuple<const K&, V&>`.
  for (auto&& [_addr, clients] : guard->cache) {
    count += clients.size();
  }
  return count;
}

// @safe - SpinMutex::lock + BTreeMap::len are @safe.
size_t ClientPool::address_count() {
  auto guard = state_.lock().unwrap();
  return guard->cache.len();
}

// @unsafe - Async reconnect loop uses nanosleep + std::atomic for batching.
// The state_ access at the top is @safe; the reconnection driver below is
// what makes this function unsafe overall.
ClientPool::BulkReconnectResult ClientPool::reconnect_all(
    const std::string& addr, const BulkReconnectConfig& config) {

  BulkReconnectResult result{0, 0, 0, 0};

  // Collect clients to reconnect
  rusty::Vec<rusty::Arc<Client>> clients_to_reconnect;
  {
    auto guard = state_.lock().unwrap();
    auto clients_opt = guard->cache.get(addr);
    if (clients_opt.is_some()) {
      // BTreeMap::get returns `Option<V&>`.
      auto& clients = clients_opt.unwrap();
      for (const auto& client : clients) {
        auto state = client->connection_state();
        if (config.skip_connected && state == ConnectionState::CONNECTED) {
          result.skipped++;
        } else {
          clients_to_reconnect.push(client);
        }
      }
    }
  }

  result.total = clients_to_reconnect.size() + result.skipped;

  // Reconnect in batches with rate limiting
  size_t i = 0;
  while (i < clients_to_reconnect.size()) {
    // Process a batch
    size_t batch_end = std::min(i + config.max_concurrent,
                                clients_to_reconnect.size());

    // Track reconnection results for this batch
    rusty::Vec<std::atomic<int>> batch_results(batch_end - i);
    for (auto& r : batch_results) r.store(-1);

    // Start async reconnections
    for (size_t j = i; j < batch_end; j++) {
      size_t idx = j - i;
      clients_to_reconnect[j]->reconnect([&batch_results, idx](bool success) {
        batch_results[idx].store(success ? 0 : 1);
      });
    }

    // Wait for batch to complete (simple polling)
    bool all_done = false;
    while (!all_done) {
      all_done = true;
      for (const auto& r : batch_results) {
        if (r.load() == -1) {
          all_done = false;
          break;
        }
      }
      if (!all_done) {
        rusty::sys::time::sleep_us(1000);  // 1ms
      }
    }

    // Count results
    for (const auto& r : batch_results) {
      if (r.load() == 0) {
        result.succeeded++;
      } else {
        result.failed++;
      }
    }

    i = batch_end;

    // Delay between batches
    if (config.delay_between_ms > 0 && i < clients_to_reconnect.size()) {
      rusty::sys::time::sleep_us(
          static_cast<std::uint64_t>(config.delay_between_ms) * 1000);
    }
  }

  return result;
}

// @unsafe - Delegates to per-address reconnect_all which has the async
// driver. The state_ snapshot taken at the top is @safe.
ClientPool::BulkReconnectResult ClientPool::reconnect_all(const BulkReconnectConfig& config) {
  BulkReconnectResult total_result{0, 0, 0, 0};

  // Get list of addresses
  rusty::Vec<std::string> addresses;
  {
    auto guard = state_.lock().unwrap();
    // BTreeMap iter returns `tuple<const K&, V&>`.
    for (auto&& [addr, _clients] : guard->cache) {
      addresses.push(addr);
    }
  }

  // Reconnect each address
  for (const auto& addr : addresses) {
    auto result = reconnect_all(addr, config);
    total_result.total += result.total;
    total_result.succeeded += result.succeeded;
    total_result.failed += result.failed;
    total_result.skipped += result.skipped;
  }

  return total_result;
}

// @unsafe - Drives Client::connect / reconnect synchronously; the state_
// lock + BTreeMap ops are @safe but the network I/O underneath is not.
rusty::Option<rusty::Arc<Client>> ClientPool::get_client(const string& addr) {
  rusty::Option<rusty::Arc<Client>> sp_cl = rusty::None;
  auto cfg = config_.get();
  int num_connections = cfg.min_connections;

  auto guard = state_.lock().unwrap();

  // Get or create load balancer state for this address
  auto lb_state_opt = guard->lb_state.get(addr);
  if (lb_state_opt.is_none()) {
    guard->lb_state.insert(addr, LoadBalancerState{});
    lb_state_opt = guard->lb_state.get(addr);
  }
  // BTreeMap::get returns `Option<V&>`; unwrap() is a reference.
  auto& lb_state = lb_state_opt.unwrap();

  auto clients_opt = guard->cache.get(addr);
  if (clients_opt.is_some()) {
    auto& clients = clients_opt.unwrap();
    int client_count = static_cast<int>(clients.size());

    // Use load balancer to select starting index
    size_t start_idx = LoadBalancer::select(
        cfg.load_balancing,
        clients,
        lb_state,
        rand_()
    );

    for (int i = 0; i < client_count; i++) {
      int idx = (start_idx + i) % client_count;
      auto& client = clients[idx];

      // Check if client is connected and healthy
      if (client->connected() && is_client_healthy(client)) {
        sp_cl = rusty::Some(client.clone());
        break;
      }

      // Try to reconnect failed/disconnected clients
      auto state = client->connection_state();
      if (state == ConnectionState::FAILED || state == ConnectionState::DISCONNECTED) {
        Log_info("ClientPool: client to %s in state %s, attempting reconnect",
                 addr.c_str(), connection_state_to_string(state));
        if (client->try_reconnect_if_needed()) {
          Log_info("ClientPool: reconnected to %s successfully", addr.c_str());
          sp_cl = rusty::Some(client.clone());
          break;
        } else {
          Log_warn("ClientPool: reconnect to %s failed", addr.c_str());
        }
      }
    }

    // If no healthy client found after trying reconnects, recreate all connections
    if (sp_cl.is_none()) {
      Log_info("ClientPool: all clients to %s failed, recreating connections", addr.c_str());
      // Close old connections
      for (auto& client : clients) {
        client->close();
      }
      clients.clear();

      // Create new connections (use min_connections)
      bool ok = true;
      for (int i = 0; i < num_connections; i++) {
        auto client = Client::create(this->poll_thread_worker_.as_ref().unwrap().clone());
        client->set_client_mode(true);
        if (client->connect(addr.c_str()) != 0) {
          Log_warn("ClientPool: failed to create new connection to %s", addr.c_str());
          ok = false;
          break;
        }
        clients.push(client);
      }

      if (ok && !clients.is_empty()) {
        sp_cl = rusty::Some(clients[rand_() % clients.size()].clone());
      } else {
        // Remove from cache if we can't connect
        guard->cache.remove(addr);
      }
    }
  } else {
    // No cached connections - create new ones
    rusty::Vec<rusty::Arc<Client>> parallel_clients;
    bool ok = true;
    for (int i = 0; i < num_connections; i++) {
      auto client = Client::create(this->poll_thread_worker_.as_ref().unwrap().clone());
      client->set_client_mode(true);  // Jetpack: mark as client
      if (client->connect(addr.c_str()) != 0) {
        ok = false;
        break;
      }
      parallel_clients.push(client);
    }
    if (ok) {
      sp_cl = rusty::Some(parallel_clients[rand_() % parallel_clients.size()].clone());
      guard->cache.insert(addr, std::move(parallel_clients));
    }
    // If not ok, parallel_clients automatically cleaned up by Arc
  }
  return sp_cl;
}

// @safe - 4g3c3: keepalive is now configured by the channel layer's
// `TcpConnection` at construction time (see `tcp_channel.cpp`); the
// RPC layer no longer owns the fd and cannot issue setsockopt.
// `keepalive_config_` is still accepted via `set_keepalive` for API
// stability, but its effect on the live channel proxy is currently
// not propagated (the channel layer reads its own defaults at
// connect time). Tests that assert keepalive configuration belong
// at the channel layer.
void ClientConnection::apply_keepalive_options() {
  // No-op in channel mode.
}

// @safe - Validate connection liveness via state machine alone.
//
// 4g3c3: the legacy `getsockopt(SO_ERROR)` health probe is gone — we
// don't own an fd. The channel layer surfaces transport errors via
// `on_error` / `on_closed`, which the connection's fan-out routes
// into the state machine. So the state-machine check is the
// authoritative liveness signal.
bool ClientConnection::validate_connection() const {
  return state_machine_.is_connected();
}

}  // namespace rrr
