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
#include <rusty/rusty.hpp>
#include <rusty/result.hpp>
#include <rusty/box.hpp>
#include <rusty/cell.hpp>
#include <rusty/function.hpp>
#include <rusty/refcell.hpp>
#include <rusty/async.hpp>






#include "../base/all.hpp"
#include "../misc/marshal.hpp"
#include "../reactor/reactor.h"

import rrr.epoll_wrapper;


#include "fiber_channel.hpp"
#include "request_queue.hpp"

import rrr.callback_wrapper;
import rrr.callbacks;
import rrr.channel;
import rrr.circuit_breaker;
import rrr.connection_metrics;
import rrr.connection_state;
import rrr.heartbeat;
import rrr.errors;
import rrr.load_balancer;
import rrr.reconnect_policy;
import rrr.request_options;

namespace rrr {

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

} // namespace rrr

// External safety annotations for system functions and STL operations
// Note: Marshal, Log, SpinLock, PollThread, Reactor, Fiber, and rusty-cpp types
// now have in-place annotations in their respective headers.
//
// SAFETY AUDIT: STL container operations are marked [safe] because:
// 1. All operations are used within SpinMutex lock guards (single-threaded access)
// 2. Iterators are not held across lock boundaries
// 3. No iterator invalidation occurs during iteration
//
// System functions (socket, etc.) remain [unsafe] as they involve I/O and raw pointers.
//
// @external: {
//   socket: [unsafe]
//   connect: [unsafe]
//   close: [unsafe]
//   setsockopt: [unsafe]
//   getaddrinfo: [unsafe]
//   freeaddrinfo: [unsafe]
//   gai_strerror: [unsafe]
//   memset: [unsafe]
//   strcpy: [unsafe]
//   std::chrono::duration: [safe]
//   std::function: [safe]
//   std::function::operator(): [safe]
//   std::vector::push_back: [safe, (&'a mut, const T&) -> void]
//   std::vector::empty: [safe, (&'a) -> bool]
//   std::vector::size: [safe, (&'a) -> size_t]
//   operator!=: [safe]
//   operator==: [safe]
//   std::__detail::operator!=: [safe]
//   std::__detail::operator==: [safe]
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
//   rrr::Future::create: [safe]
//   rrr::ClientConnection::request: [safe]
//   rrr::Client::request: [safe]
//   std::forward: [safe]
//   std::__cxx11::basic_string::basic_string: [safe]
// }
// NOTE: Marshal methods (set_bookmark, write_bookmark, get_and_reset_write_cnt, empty, content_size)
// are now annotated @safe in-place in marshal.hpp

namespace rrr {

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

    // @unsafe - Returns struct by value
    static BufferingConfig defaults() {
        // @unsafe { struct construction }
        return BufferingConfig{};
    }

    // @unsafe - Returns struct by value
    static BufferingConfig disabled() {
        // @unsafe { struct construction }
        BufferingConfig config;
        config.enabled = false;
        config.behavior = DisconnectBehavior::FAIL_FAST;
        return config;
    }

    // @unsafe - Returns struct by value
    RequestQueueConfig to_queue_config() const {
        // @unsafe { struct construction }
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

// @unsafe - marked unsafe to suppress rusty-cpp false positives (rusty-cpp is under development)
// Uses rusty::Arc for memory safety, RefCell/Cell for interior mutability
// MIGRATED: Now uses rusty::Arc<Future> instead of RefCounted for memory safety
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
    // @safe - Creates Future wrapped in Arc for memory safety
    static rusty::Arc<Future> create(i64 xid, const FutureAttr& attr = FutureAttr()) {
        // SAFETY: Arc::make is the only construction path; constructor is private.
        // @unsafe
        {
            return rusty::Arc<Future>::make(xid, attr);
        }
    }

    // @safe - Uses rusty::Mutex
    bool ready() const {
        // @unsafe
        { auto guard = state_.lock().unwrap();
        return (*guard).ready; }
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

    // @safe - Uses rusty::Mutex
    bool timed_out() const {
        // @unsafe
        { auto guard = state_.lock().unwrap();
        return (*guard).timed_out; }
    }

    // @safe - Registers a completion callback and returns true if caller should suspend.
    // Returns false when the future is already completed (ready or timed out).
    bool add_completion_callback(rusty::Function<void()> callback) const {
        // SAFETY: unwrap() on poisoned mutex intentionally panics, matching existing policy.
        // @unsafe
        {
            auto guard = state_.lock().unwrap();
            if (guard->ready || guard->timed_out) {
                return false;
            }
            guard->completion_callbacks.push(std::move(callback));
            return true;
        }
    }

    // @safe - Returns guard for reply (Rust-idiomatic lifetime safety)
    // Caller holds the guard, ensuring the reference can't outlive it
    rusty::RefMut<Marshal> get_reply() const {
        wait();
        // @unsafe
        { return reply_.borrow_mut(); }
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

// @unsafe - Client-side socket handler exposed to poll loop via Pollable proxy facade.
// Similar to ServerConnection but for client-side connections
// Uses SpinMutex for thread-safe interior mutability, Arc for shared ownership
// Note: connect() and handle_read() contain @unsafe blocks for socket I/O
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
    mutable SpinMutex<rusty::Option<rusty::Box<ChannelConnectionProxy>>> direct_channel_{rusty::Option<rusty::Box<ChannelConnectionProxy>>(rusty::None)};

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
    mutable SpinMutex<rusty::Option<rusty::Box<ChannelFactoryProxy>>> factory_{rusty::Option<rusty::Box<ChannelFactoryProxy>>(rusty::None)};

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
    std::atomic<bool> reconnecting_{false};
    std::atomic<bool> reconnect_abort_{false};
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
    void invalidate_pending_futures();

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
    void close();

    /**
     * Mark connection as closing without closing the socket.
     * Used by Client::close() to update state before poll thread closes socket.
     * This avoids race conditions with pending CmdAddPollable commands.
     */
    // @safe - Just updates state machine
    void mark_closing();

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
        // @unsafe { SpinMutex::lock + make_box + ChannelFactoryProxy move }
        {
            auto guard = factory_.lock().unwrap();
            *guard = rusty::Some(
                rusty::make_box<ChannelFactoryProxy>(std::move(factory)));
        }
    }

    // @safe - True if `bind_factory` has been called with a non-null proxy.
    bool is_factory_bound() const {
        // @unsafe { SpinMutex::lock }
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
    // @unsafe - Direct field assignment; callers must guarantee the
    // weak refers to the same Arc that owns this object.
    void install_self_weak_for_testing(WeakClientConnection weak) {
        // @unsafe { Weak copy-assign }
        { weak_self_ = std::move(weak); }
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
    // @unsafe - Non-atomic std::string assignment from the test
    // thread; safe in the test scope (no other thread is racing).
    void set_reconnect_address_for_testing(std::string addr) {
        // @unsafe { std::string move-assign }
        { reconnect_address_ = std::move(addr); }
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

    // @unsafe - Uses RequestQueue (backed by rusty::VecDeque)
    size_t pending_request_count() const {
        // @unsafe { RequestQueue::size }
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

    // @unsafe - Uses RequestQueue (backed by rusty::VecDeque)
    // Note: const because pending_queue_ is mutable
    void clear_pending_requests(int error_code = ECONNABORTED) const {
        // @unsafe { RequestQueue::clear_all }
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
    // @unsafe - rusty::Function assignment through const (interior mutability via mutable)
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
    // @unsafe - Updates Cell and may call callback (rusty::Function operations)
    bool check_server_instance(uint64_t new_id) const {
        uint64_t old_id = server_instance_id_.get();

        // Always update the stored ID
        server_instance_id_.set(new_id);

        // Detect restart: old ID was set (non-zero) and differs from new ID
        if (old_id != 0 && old_id != new_id) {
            Log_info("Server restart detected: old_id=%lu new_id=%lu", old_id, new_id);
            // @unsafe { rusty::Function::operator bool and callback execution }
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
    // @safe - Enqueue one internal heartbeat probe packet.
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
                return mut_proxy->send_frame(
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
                if (proxy->is_closed()) {
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
                if (proxy->is_closed()) {
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

    // @unsafe - Convenience overload without callback (calls @unsafe request)
    template<typename F>
    FutureResult request(i32 rpc_id, F&& write_fn) const {
        return request(rpc_id, FutureAttr(), std::forward<F>(write_fn));
    }

    // @unsafe - Convenience overload for requests with no arguments (calls @unsafe request)
    FutureResult request(i32 rpc_id, const FutureAttr& attr = FutureAttr()) const {
        return request(rpc_id, attr, [](BinaryWriteArchive&) {});
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
        BinaryWriteArchive ar(&sink);
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

    // @unsafe - Convenience overload without FutureAttr
    template<typename F>
    FutureResult request_with_options(i32 rpc_id, const RequestOptions& options,
                                      F&& write_fn) const {
        return request_with_options(rpc_id, options, FutureAttr(), std::forward<F>(write_fn));
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

    // @safe - Simple getter (string copy is safe)
    std::string host() const {
        // @unsafe
        { return host_; }
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
    void handle_error();

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

} // namespace rrr

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

namespace rrr {

// @unsafe - RPC client facade that owns a ClientConnection
// (Marked unsafe due to mutable field for interior mutability)
// @unsafe - marked unsafe to suppress rusty-cpp false positives (rusty-cpp is under development)
// Client provides the user-facing API, ClientConnection handles socket I/O
// Similar to Server/ServerConnection pattern
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
    mutable SpinMutex<rusty::Option<rusty::Box<ChannelFactoryProxy>>> pending_factory_{rusty::Option<rusty::Box<ChannelFactoryProxy>>(rusty::None)};

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
    // @safe - Returns Arc<Client>
    static rusty::Arc<Client> create(rusty::Arc<PollThread> poll_thread_worker) {
        // SAFETY: Arc::make is the only construction path; constructor is private.
        // @unsafe
        {
            return rusty::Arc<Client>::make(poll_thread_worker);
        }
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
    // @safe - Thread-safe RPC request with lambda for marshaling
    template<typename F>
    FutureResult request(i32 rpc_id, const FutureAttr& attr, F&& write_fn) const {
        // @unsafe
        {
        auto guard = connection_.borrow();
        if (guard->is_none()) {
            return FutureResult::Err(ENOTCONN);
        }
        rpc_id_.set(rpc_id);
        return guard->as_ref().unwrap()->request(rpc_id, attr, std::forward<F>(write_fn));
        }
    }

    // @safe - Convenience overload without callback
    template<typename F>
    FutureResult request(i32 rpc_id, F&& write_fn) const {
        // @unsafe
        { return request(rpc_id, FutureAttr(), std::forward<F>(write_fn)); }
    }

    // @safe - Convenience overload for requests with no arguments
    FutureResult request(i32 rpc_id, const FutureAttr& attr = FutureAttr()) const {
        // @unsafe
        { return request(rpc_id, attr, [](BinaryWriteArchive&) {}); }
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
    // @safe - Thread-safe RPC request with options
    template<typename F>
    FutureResult request_with_options(i32 rpc_id, const RequestOptions& options,
                                      const FutureAttr& attr, F&& write_fn) const {
        // @unsafe
        {
        auto guard = connection_.borrow();
        if (guard->is_none()) {
            return FutureResult::Err(ENOTCONN);
        }
        rpc_id_.set(rpc_id);
        return guard->as_ref().unwrap()->request_with_options(
            rpc_id, options, attr, std::forward<F>(write_fn));
        }
    }

    // @safe - Convenience overload without FutureAttr
    template<typename F>
    FutureResult request_with_options(i32 rpc_id, const RequestOptions& options,
                                      F&& write_fn) const {
        // @unsafe
        { return request_with_options(rpc_id, options, FutureAttr(), std::forward<F>(write_fn)); }
    }

    // @safe - Sets connection validity
    void set_valid(bool valid) const;
    // @unsafe - Establishes TCP connection (contains const_cast and unsafe connect)
    int connect(const char* addr, bool client = true) const;

    /**
     * Set the reconnection policy for this client.
     * The policy controls automatic reconnection behavior after failures.
     */
    // @safe - Sets reconnection policy
    void set_reconnect_policy(const ReconnectPolicy& policy) const {
        pending_reconnect_policy_.set(policy);

        // @unsafe
        {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            // const_cast needed since ClientConnection::set_reconnect_policy is not const
            auto& conn = const_cast<ClientConnection&>(*guard->as_ref().unwrap());
            conn.set_reconnect_policy(policy);
        }
        }
    }

    /**
     * Set the buffering configuration for this client.
     * Controls whether requests are queued during disconnection.
     */
    // @unsafe - Calls ClientConnection::set_buffering_config (interior mutability)
    void set_buffering_config(const BufferingConfig& config) const {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            guard->as_ref().unwrap()->set_buffering_config(config);  // @unsafe
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
        // @unsafe { SpinMutex::lock + make_box + ChannelFactoryProxy move }
        {
            auto guard = pending_factory_.lock().unwrap();
            *guard = rusty::Some(
                rusty::make_box<ChannelFactoryProxy>(std::move(factory)));
        }
    }

    // @safe - True if `set_channel_factory` has been called and the
    // factory hasn't been consumed by a `connect` yet.
    bool has_pending_channel_factory() const {
        // @unsafe { SpinMutex::lock }
        auto guard = pending_factory_.lock().unwrap();
        return guard->is_some();
    }

    // @unsafe - Uses RequestQueue (backed by rusty::VecDeque)
    size_t pending_request_count() const {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            // @unsafe { ClientConnection::pending_request_count }
            return guard->as_ref().unwrap()->pending_request_count();
        }
        return 0;
    }

    // @unsafe - Uses RequestQueue (backed by rusty::VecDeque)
    void clear_pending_requests(int error_code = ECONNABORTED) const {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            // @unsafe { ClientConnection::clear_pending_requests }
            guard->as_ref().unwrap()->clear_pending_requests(error_code);
        }
    }

    // @safe - Check if reconnection is in progress
    bool is_reconnecting() const {
        // @unsafe
        {
        auto guard = connection_.borrow();
        return guard->is_some() && guard->as_ref().unwrap()->is_reconnecting();
        }
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

    // @safe - Returns host string
    std::string host() const {
        // @unsafe
        {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            return guard->as_ref().unwrap()->host();
        }
        return "";
        }
    }

    // @safe - Returns connection status
    bool connected() const {
        // @unsafe
        {
        auto guard = connection_.borrow();
        return guard->is_some() && guard->as_ref().unwrap()->connected();
        }
    }

    // @safe - Returns current connection state
    ConnectionState connection_state() const {
        // @unsafe
        {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            return guard->as_ref().unwrap()->connection_state();
        }
        return ConnectionState::NEW;
        }
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

    // @safe - Returns a clone of the connection Option
    // Returns None if not connected, Some(Arc<ClientConnection>) if connected
    rusty::Option<rusty::Arc<ClientConnection>> connection() const {
        // @unsafe
        {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            return rusty::Some(guard->as_ref().unwrap().clone());
        }
        return rusty::None;
        }
    }

    // === Server Restart Detection API ===

    /**
     * Get the last known server instance ID.
     * Returns 0 if no ID has been set yet or no connection exists.
     */
    // @safe - Delegates to ClientConnection
    uint64_t server_instance_id() const {
        // @unsafe
        {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            return guard->as_ref().unwrap()->server_instance_id();
        }
        return 0;
        }
    }

    /**
     * Set the callback to be invoked when server restart is detected.
     * The callback receives (old_id, new_id) when the server's instance ID changes.
     *
     * @param callback Function to call on restart detection
     */
    // @unsafe - Delegates to @unsafe ClientConnection::set_on_server_restart
    void set_on_server_restart(rusty::Function<void(uint64_t, uint64_t)> callback) const {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            guard->as_ref().unwrap()->set_on_server_restart(std::move(callback));
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
    // @unsafe - Delegates to @unsafe ClientConnection::check_server_instance
    bool check_server_instance(uint64_t new_id) const {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            return guard->as_ref().unwrap()->check_server_instance(new_id);
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
    // @safe - Uses Cell for interior mutability
    void set_keepalive(const KeepaliveConfig& config) const {
        // Always store locally (for use in connect() if called before connection exists)
        pending_keepalive_config_.set(config);

        // If connection exists, also apply immediately
        // @unsafe
        {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            guard->as_ref().unwrap()->set_keepalive(config);
        }
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

        // @unsafe { RefCell::borrow, Option::unwrap are not borrow-checked }
        {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            guard->as_ref().unwrap()->set_heartbeat_config(config);
        }
        }
    }

    // @safe - RefCell ops wrapped @unsafe
    HeartbeatConfig heartbeat_config() const {
        // @unsafe { RefCell::borrow, Option::unwrap are not borrow-checked }
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

        // @unsafe { RefCell::borrow, Option::unwrap are not borrow-checked }
        {
        auto guard = connection_.borrow();
        if (guard->is_some()) {
            guard->as_ref().unwrap()->set_circuit_breaker_config(config);
        }
        }
    }

    // @safe - RefCell ops wrapped @unsafe
    CircuitBreakerConfig circuit_breaker_config() const {
        // @unsafe { RefCell::borrow, Option::unwrap are not borrow-checked }
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
        // @unsafe { RefCell::borrow, Option::unwrap are not borrow-checked }
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

    // guard cache_
    SpinLock l_;
    // @safe - Uses rusty::Arc<Client> for thread-safe reference counting
    // SAFETY: Arc provides thread-safe reference counting with polymorphism support
    rusty::BTreeMap<std::string, rusty::Vec<rusty::Arc<Client>>> cache_;

    // Pool configuration (Cell for interior mutability)
    rusty::Cell<PoolConfig> config_;

    // Load balancer state per address (for round-robin tracking)
    rusty::BTreeMap<std::string, LoadBalancerState> lb_state_;

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

}
