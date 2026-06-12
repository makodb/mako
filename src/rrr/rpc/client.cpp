// rrr.client — RPC client (formerly client.hpp + client.cpp).
//
// Owns ClientConnection (socket I/O + framing), Client (user-facing
// facade), Future (async reply delivery), and the bulk reconnect
// helpers. Sits above the channel layer (`tcp_channel`,
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
import rusty;
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
import rrr.rand;
import rrr.request_options;
import rrr.request_queue;
import rrr.serializable;
import rrr.tcp_channel;
import rrr.threading;

// ===========================================================================
// Block 1: forward decls (from former client.hpp:50-78)
// ===========================================================================
// @safe - first-half namespace block: Future / TypedFuture awaiters
// + the BufferingConfig / KeepaliveConfig / PoolConfig POD structs.
// ClientConnection (declared in the second block below) retains its
// class-level `// @unsafe`. Methods that genuinely cross into
// network I/O / socket fd / Marshal byte ops keep their existing
// per-method `// @unsafe` annotations.
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
    rrr::BinaryReadArchive ar(rrr::make_source_proxy(&*guard));
    ar >> value;
    return guard;
}

template<typename U>
rusty::RefMut<Marshal>&& operator>>(rusty::RefMut<Marshal>&& guard, U& value) {
    rrr::BinaryReadArchive ar(rrr::make_source_proxy(&*guard));
    ar >> value;
    return std::move(guard);
}

}  // export namespace rrr

// ===========================================================================
// Block 2: Future, ClientConnection (from former client.hpp:130-1963)
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

// `DisconnectBehavior` — categorical tag for what Client::request_*
// does when the connection is down. Authored as inline Rust DSL: the
// `#if RUSTYCPP_RUST` block below is the source of truth; the
// transpiler regenerates the matching `RUSTYCPP:GEN-BEGIN ... END`
// block.
#if RUSTYCPP_RUST
enum DisconnectBehavior {
    QUEUE,
    FAIL_FAST,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.disconnect_behavior version=1 rust_sha256=1264991ea2319838074ef74280655bc750a6fe3de14ac2eee3d01644d5f5a66f*/
enum class DisconnectBehavior;
constexpr DisconnectBehavior DisconnectBehavior_QUEUE();
constexpr DisconnectBehavior DisconnectBehavior_FAIL_FAST();

enum class DisconnectBehavior {
    QUEUE,
    FAIL_FAST
};
inline constexpr DisconnectBehavior DisconnectBehavior_QUEUE() { return DisconnectBehavior::QUEUE; }
inline constexpr DisconnectBehavior DisconnectBehavior_FAIL_FAST() { return DisconnectBehavior::FAIL_FAST; }
/*RUSTYCPP:GEN-END id=client.disconnect_behavior*/

/**
 * Configuration for request buffering during disconnection.
 *
 * Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
 * the source of truth; the transpiler regenerates the matching
 * `RUSTYCPP:GEN-BEGIN ... END` block. The plain `fn new()` lowers to
 * a `static BufferingConfig new_()` factory (returning the default
 * preset, matching what the prior default ctor produced). Callers
 * use `BufferingConfig::defaults()` / `::disabled()` / brace-init.
 */
#if RUSTYCPP_RUST
struct BufferingConfig {
    behavior: DisconnectBehavior,
    max_pending: usize,
    default_ttl_ms: u32,
    overflow: OverflowStrategy,
    enabled: bool,
}

impl BufferingConfig {
    fn new() -> BufferingConfig {
        BufferingConfig {
            behavior: DisconnectBehavior::QUEUE,
            max_pending: 1000usize,
            default_ttl_ms: 30000u32,
            overflow: OverflowStrategy::DROP_OLDEST,
            enabled: true,
        }
    }

    fn defaults() -> BufferingConfig {
        BufferingConfig::new()
    }

    fn disabled() -> BufferingConfig {
        BufferingConfig {
            behavior: DisconnectBehavior::FAIL_FAST,
            max_pending: 1000usize,
            default_ttl_ms: 30000u32,
            overflow: OverflowStrategy::DROP_OLDEST,
            enabled: false,
        }
    }

    fn to_queue_config(&self) -> RequestQueueConfig {
        RequestQueueConfig {
            max_size: self.max_pending,
            default_ttl_ms: self.default_ttl_ms,
            overflow_strategy: self.overflow,
            enabled: self.enabled,
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.0a version=1 rust_sha256=7e0ba144f0837e66cb88578d52aa0282acc8079cc5aab29d696aa63430b38c56*/
struct BufferingConfig;

struct BufferingConfig {
    DisconnectBehavior behavior;
    size_t max_pending;
    uint32_t default_ttl_ms;
    OverflowStrategy overflow;
    bool enabled;

    static BufferingConfig new_();
    static BufferingConfig defaults();
    static BufferingConfig disabled();
    RequestQueueConfig to_queue_config() const;
};


BufferingConfig BufferingConfig::new_() {
    return BufferingConfig{.behavior = rusty::clone(rusty::clone(DisconnectBehavior::QUEUE)), .max_pending = static_cast<size_t>(1000), .default_ttl_ms = static_cast<uint32_t>(30000), .overflow = rusty::clone(rusty::clone(OverflowStrategy::DROP_OLDEST)), .enabled = true};
}

BufferingConfig BufferingConfig::defaults() {
    return BufferingConfig::new_();
}

BufferingConfig BufferingConfig::disabled() {
    return BufferingConfig{.behavior = rusty::clone(rusty::clone(DisconnectBehavior::FAIL_FAST)), .max_pending = static_cast<size_t>(1000), .default_ttl_ms = static_cast<uint32_t>(30000), .overflow = rusty::clone(rusty::clone(OverflowStrategy::DROP_OLDEST)), .enabled = false};
}

RequestQueueConfig BufferingConfig::to_queue_config() const {
    return RequestQueueConfig{.max_size = this->max_pending, .default_ttl_ms = this->default_ttl_ms, .overflow_strategy = this->overflow, .enabled = this->enabled};
}
/*RUSTYCPP:GEN-END id=client.0a*/

/**
 * TCP Keepalive configuration for connection health monitoring.
 *
 * Configures OS-level TCP keepalive probes to detect dead connections.
 * When enabled, the OS will send keepalive probes after the connection
 * has been idle for `idle_sec` seconds, then at `interval_sec` intervals.
 * If `count` probes go unanswered, the connection is considered dead.
 */
// @safe - POD config struct for TCP keepalive settings.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. The plain `fn new()` lowers to
// a `static KeepaliveConfig new_()` factory (returning the relaxed
// preset, which matches the prior default ctor). Callers construct
// via the factory presets (`KeepaliveConfig::aggressive()`,
// `::relaxed()`, `::disabled()`) or via brace-init / designated-init
// (the emitted struct is a C++20 aggregate).
#if RUSTYCPP_RUST
struct KeepaliveConfig {
    enabled: bool,
    idle_sec: i32,
    interval_sec: i32,
    count: i32,
}

impl KeepaliveConfig {
    fn new() -> KeepaliveConfig {
        KeepaliveConfig { enabled: true, idle_sec: 60i32, interval_sec: 10i32, count: 5i32 }
    }

    fn aggressive() -> KeepaliveConfig {
        KeepaliveConfig { enabled: true, idle_sec: 10i32, interval_sec: 2i32, count: 3i32 }
    }

    fn relaxed() -> KeepaliveConfig {
        KeepaliveConfig { enabled: true, idle_sec: 60i32, interval_sec: 10i32, count: 5i32 }
    }

    fn disabled() -> KeepaliveConfig {
        KeepaliveConfig { enabled: false, idle_sec: 0i32, interval_sec: 0i32, count: 0i32 }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.0 version=1 rust_sha256=35cfe5f339a1d6ac9c5e1b261893967b5f5d11ba49ade28e603408ab3ecad43d*/
struct KeepaliveConfig;

struct KeepaliveConfig {
    bool enabled;
    int32_t idle_sec;
    int32_t interval_sec;
    int32_t count;

    static KeepaliveConfig new_();
    static KeepaliveConfig aggressive();
    static KeepaliveConfig relaxed();
    static KeepaliveConfig disabled();
};


KeepaliveConfig KeepaliveConfig::new_() {
    return KeepaliveConfig{.enabled = true, .idle_sec = static_cast<int32_t>(60), .interval_sec = static_cast<int32_t>(10), .count = static_cast<int32_t>(5)};
}

KeepaliveConfig KeepaliveConfig::aggressive() {
    return KeepaliveConfig{.enabled = true, .idle_sec = static_cast<int32_t>(10), .interval_sec = static_cast<int32_t>(2), .count = static_cast<int32_t>(3)};
}

KeepaliveConfig KeepaliveConfig::relaxed() {
    return KeepaliveConfig{.enabled = true, .idle_sec = static_cast<int32_t>(60), .interval_sec = static_cast<int32_t>(10), .count = static_cast<int32_t>(5)};
}

KeepaliveConfig KeepaliveConfig::disabled() {
    return KeepaliveConfig{.enabled = false, .idle_sec = static_cast<int32_t>(0), .interval_sec = static_cast<int32_t>(0), .count = static_cast<int32_t>(0)};
}
/*RUSTYCPP:GEN-END id=client.0*/

/**
 * ClientPool configuration for health-aware connection pooling.
 *
 * Controls connection limits, health checking, and idle timeout behavior.
 */
// @safe - POD config struct for pool settings.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. The plain `fn new()` lowers to
// a `static PoolConfig new_()` factory (returning the balanced
// preset). Callers use `PoolConfig::defaults()`,
// `PoolConfig::aggressive()`, `PoolConfig::conservative()`,
// `PoolConfig::no_health_check()`, or brace-init.
#if RUSTYCPP_RUST
struct PoolConfig {
    min_connections: i32,
    max_connections: i32,
    idle_timeout_ms: u64,
    health_check_enabled: bool,
    unhealthy_threshold_percent: u64,
    min_requests_for_health: u64,
    load_balancing: LoadBalancingStrategy,
}

impl PoolConfig {
    fn new() -> PoolConfig {
        PoolConfig {
            min_connections: 1i32,
            max_connections: 4i32,
            idle_timeout_ms: 300000u64,
            health_check_enabled: true,
            unhealthy_threshold_percent: 50u64,
            min_requests_for_health: 10u64,
            load_balancing: LoadBalancingStrategy::RANDOM,
        }
    }

    fn defaults() -> PoolConfig {
        PoolConfig::new()
    }

    fn aggressive() -> PoolConfig {
        PoolConfig {
            min_connections: 2i32,
            max_connections: 8i32,
            idle_timeout_ms: 60000u64,
            health_check_enabled: true,
            unhealthy_threshold_percent: 70u64,
            min_requests_for_health: 5u64,
            load_balancing: LoadBalancingStrategy::RANDOM,
        }
    }

    fn conservative() -> PoolConfig {
        PoolConfig {
            min_connections: 1i32,
            max_connections: 2i32,
            idle_timeout_ms: 600000u64,
            health_check_enabled: true,
            unhealthy_threshold_percent: 30u64,
            min_requests_for_health: 20u64,
            load_balancing: LoadBalancingStrategy::RANDOM,
        }
    }

    fn no_health_check() -> PoolConfig {
        PoolConfig {
            min_connections: 1i32,
            max_connections: 4i32,
            idle_timeout_ms: 300000u64,
            health_check_enabled: false,
            unhealthy_threshold_percent: 50u64,
            min_requests_for_health: 10u64,
            load_balancing: LoadBalancingStrategy::RANDOM,
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.0b version=1 rust_sha256=a4fbb6b0a843e27688a5f6cebb6842f15c51f7695e85a969d9ba6c7e38ffbfc1*/
struct PoolConfig;

struct PoolConfig {
    int32_t min_connections;
    int32_t max_connections;
    uint64_t idle_timeout_ms;
    bool health_check_enabled;
    uint64_t unhealthy_threshold_percent;
    uint64_t min_requests_for_health;
    LoadBalancingStrategy load_balancing;

    static PoolConfig new_();
    static PoolConfig defaults();
    static PoolConfig aggressive();
    static PoolConfig conservative();
    static PoolConfig no_health_check();
};


PoolConfig PoolConfig::new_() {
    return PoolConfig{.min_connections = static_cast<int32_t>(1), .max_connections = static_cast<int32_t>(4), .idle_timeout_ms = static_cast<uint64_t>(300000), .health_check_enabled = true, .unhealthy_threshold_percent = static_cast<uint64_t>(50), .min_requests_for_health = static_cast<uint64_t>(10), .load_balancing = rusty::clone(rusty::clone(LoadBalancingStrategy::RANDOM))};
}

PoolConfig PoolConfig::defaults() {
    return PoolConfig::new_();
}

PoolConfig PoolConfig::aggressive() {
    return PoolConfig{.min_connections = static_cast<int32_t>(2), .max_connections = static_cast<int32_t>(8), .idle_timeout_ms = static_cast<uint64_t>(60000), .health_check_enabled = true, .unhealthy_threshold_percent = static_cast<uint64_t>(70), .min_requests_for_health = static_cast<uint64_t>(5), .load_balancing = rusty::clone(rusty::clone(LoadBalancingStrategy::RANDOM))};
}

PoolConfig PoolConfig::conservative() {
    return PoolConfig{.min_connections = static_cast<int32_t>(1), .max_connections = static_cast<int32_t>(2), .idle_timeout_ms = static_cast<uint64_t>(600000), .health_check_enabled = true, .unhealthy_threshold_percent = static_cast<uint64_t>(30), .min_requests_for_health = static_cast<uint64_t>(20), .load_balancing = rusty::clone(rusty::clone(LoadBalancingStrategy::RANDOM))};
}

PoolConfig PoolConfig::no_health_check() {
    return PoolConfig{.min_connections = static_cast<int32_t>(1), .max_connections = static_cast<int32_t>(4), .idle_timeout_ms = static_cast<uint64_t>(300000), .health_check_enabled = false, .unhealthy_threshold_percent = static_cast<uint64_t>(50), .min_requests_for_health = static_cast<uint64_t>(10), .load_balancing = rusty::clone(rusty::clone(LoadBalancingStrategy::RANDOM))};
}
/*RUSTYCPP:GEN-END id=client.0b*/

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

// @safe - Simple attribute struct for Future callbacks.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. The DSL emits a pure aggregate
// (single `FutureCallback callback` field); `FutureAttr()` default-
// construction continues to work via the implicit aggregate default
// (which calls `FutureCallback`'s own default ctor — same as the
// dropped `FutureAttr() = FutureCallback{}`). Use `FutureAttr::new_(cb)`
// to attach a callback.
#if RUSTYCPP_RUST
struct FutureAttr {
    callback: FutureCallback,
}

impl FutureAttr {
    fn new(cb: FutureCallback) -> FutureAttr {
        FutureAttr { callback: cb }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.0c version=1 rust_sha256=63f9e806df0c099603647a4e598fed6cee607ada9faca2c6d7a515cdf7578ce3*/
struct FutureAttr;

struct FutureAttr {
    FutureCallback callback;

    static FutureAttr new_(FutureCallback cb);
};


FutureAttr FutureAttr::new_(FutureCallback cb) {
    return FutureAttr{.callback = std::move(cb)};
}
/*RUSTYCPP:GEN-END id=client.0c*/

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
    rusty::Cell<RequestOptions> options_{RequestOptions::defaults()};  // Request options (timeout, retry config)
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

// Type alias for Arc weak reference to ClientConnection
using WeakClientConnection = rusty::sync::Weak<ClientConnection>;

// Async-callback slot array size — slim alternative to `pending_fu_` for
// callers that don't need an `Arc<Future>` handle (no sync-wait,
// no retry, no reply-buffer inspection — just "call me back when
// the reply arrives"). Indexed by `xid % kAsyncSlotCount`. At
// typical in-flight depths (a few thousand), collisions are
// impossible. See `ClientConnection::request_async` below.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block with the C++ constexpr. Moved
// out of `ClientConnection` class scope (was `static constexpr size_t
// kAsyncSlotCount` there) because the DSL emits constants at namespace
// scope. Every call site uses the unqualified name and resolves via
// namespace lookup, which still finds it.
#if RUSTYCPP_RUST
const kAsyncSlotCount: usize = 16384;
#endif
/*RUSTYCPP:GEN-BEGIN id=client.async_slot_count version=1 rust_sha256=57a7b55ab412027a575a05239198cafdcdf9f2a955fcf5ebbe2dd8788b45714c*/
constexpr size_t kAsyncSlotCount = static_cast<size_t>(16384);
/*RUSTYCPP:GEN-END id=client.async_slot_count*/

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
    mutable Counter xid_counter_ = Counter::new_(0);

    // Map of pending futures awaiting responses (protected by SpinMutex)
    SpinMutex<rusty::HashMap<i64, rusty::Arc<Future>>> pending_fu_{rusty::HashMap<i64, rusty::Arc<Future>>()};

public:
    using AsyncReplyCallback = rusty::Function<
        void(i32 /*error_code*/, const uint8_t* /*reply_bytes*/, size_t /*reply_size*/)>;
private:
    mutable SpinMutex<rusty::Vec<rusty::Option<AsyncReplyCallback>>>
        pending_cb_slots_;

    // Connection state machine for lifecycle management
    ConnectionStateMachine state_machine_{ConnectionStateMachine::new_()};

    // Reconnection policy and state
    mutable ReconnectPolicy reconnect_policy_;  // mutable for const set_reconnect_policy()
    // mutable: std::atomic::store / .load are interior-mutable in
    // semantics but std::atomic::store is not declared `const` (it has
    // `volatile` overloads only). Mark mutable so const methods can
    // legally call store() — atomic semantics make this race-free.
    mutable std::atomic<bool> reconnecting_{false};
    mutable std::atomic<bool> reconnect_abort_{false};
    std::string reconnect_address_;  // Address to reconnect to

    // Request buffering during disconnection. The DSL-emitted
    // BufferingConfig no longer carries in-class default initializers,
    // so use the explicit `defaults()` factory to match the pre-DSL
    // behaviour (enabled=true, behavior=QUEUE).
    mutable BufferingConfig buffering_config_ = BufferingConfig::defaults();
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
    ConnectionMetrics metrics_{ConnectionMetrics::new_()};

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
    // @safe - Sets reconnection policy. Const because the field is `mutable`
    // (interior-mutability sleeve so Client::set_reconnect_policy — which
    // is itself const-callable through Arc<Client> — can delegate here).
    void set_reconnect_policy(const ReconnectPolicy& policy) const {
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

    // @unsafe - Const facade over the non-const `reconnect`. Lets
    // Client::reconnect — itself const through Arc<Client> — delegate
    // without surfacing the const_cast in the DSL. Also resets the
    // mutable `reconnect_abort_` atomic before delegating — matches
    // the legacy `Client::reconnect` body ordering. Without this reset
    // tests like rpc_metrics_test::QueueDropCounter... see a stale
    // abort=true left over from a prior close() and reconnect bails.
    int reconnect(rusty::Function<void(bool)> on_complete) const {
        reconnect_abort_.store(false, std::memory_order_release);
        // @unsafe { const_cast<ClientConnection*>(this) — see decl }
        { return const_cast<ClientConnection*>(this)->reconnect(std::move(on_complete)); }
    }

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
    /// @safe - delegates to rusty::sys::time::clock_monotonic_us.
    static uint64_t monotonic_ms_now() {
        return rusty::sys::time::clock_monotonic_us() / 1000;
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
                auto fu = Future::create(xid_counter_.next(1), attr);
                auto fu_for_cb = fu;  // Arc clone for the callback.
                auto qr = QueuedRequest::new_();
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
        auto fu = Future::create(xid_counter_.next(1), attr);

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
    rusty::Result<rusty::Unit, i32> request_async(
        i32 rpc_id, F&& write_fn, AsyncReplyCallback on_reply) const {
        if (!allow_request_with_circuit_metrics()) {
            return rusty::Result<rusty::Unit, i32>::Err(EBUSY);
        }
        // Liveness check (mirrors request_via_channel). The state-
        // machine check runs first to close the
        // `Client::close()`-schedules-async-proxy-close race; see the
        // explanatory comment in `request_via_channel`.
        if (!state_machine_.is_connected()) {
            record_circuit_result(ENOTCONN);
            return rusty::Result<rusty::Unit, i32>::Err(ENOTCONN);
        }
        {
            auto direct_guard = direct_channel_.lock().unwrap();
            if (direct_guard->is_some()) {
                auto& proxy = *direct_guard->as_ref().unwrap();
                if (proxy.is_closed()) {
                    record_circuit_result(ENOTCONN);
                    return rusty::Result<rusty::Unit, i32>::Err(ENOTCONN);
                }
            } else {
                auto guard = fiber_channel_.lock().unwrap();
                if (guard->is_none() ||
                    guard->as_ref().unwrap()->is_closed()) {
                    record_circuit_result(ENOTCONN);
                    return rusty::Result<rusty::Unit, i32>::Err(ENOTCONN);
                }
            }
        }

        const i64 xid = xid_counter_.next(1);
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
                return rusty::Result<rusty::Unit, i32>::Err(EBUSY);
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
            return rusty::Result<rusty::Unit, i32>::Err(EIO);
        }
        metrics_.record_request_sent();
        on_request_dispatched(body_sink.bytes.len());
        return rusty::Result<rusty::Unit, i32>::Ok(rusty::Unit{});
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
        BinaryWriteArchive ar(make_sink_proxy(&serialized_args));
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
        auto final_fu = Future::create(xid_counter_.next(1), attr);
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

// Type aliases for `rusty::Function<…>` parameter types used in Client's
// public API. Defined outside any future inline-Rust DSL block so the
// DSL source can refer to them by an opaque type name (the DSL grammar
// does not accept C++ function-type template arguments like
// `<void(bool) const>`). Same pattern as `HeartbeatTimeoutCallback`
// (rrr/heartbeat.cpp) and `StateChangeCallback` (rrr/connection_state.cpp).
//
// Naming convention: `…CallbackFn` for the bare (move-only) Function<…>
// shape that Client's `add_on_*` / `reconnect` / `set_on_server_restart`
// methods consume. The `…Callback` aliases without `Fn` in
// rrr/callbacks.cpp wrap the same Function shape in `rusty::Arc<…>`
// (a shared, clone-friendly handle), so the two namespaces are distinct.
using OnReconnectCompleteCallbackFn   = rusty::Function<void(bool)>;
using OnConnectedCallbackFn           = rusty::Function<void() const>;
using OnErrorCallbackFn               = rusty::Function<void(RpcError,
                                                             const std::string&) const>;
using OnReconnectedCallbackFn         = rusty::Function<void(bool) const>;
using OnServerRestartCallbackFn       = rusty::Function<void(uint64_t, uint64_t)>;

// @unsafe - reinterpret_cast<const char*> on the addr param. Lives
// outside the DSL block so the inline-Rust grammar doesn't have to
// reason about `std::ffi::c_char` (which triggers a transpiler-side
// `proc_macro_runtime` import explosion). Used by the DSL `connect()`
// body to bridge `*const i8` (Rust DSL) to `const char*` (legacy
// ClientConnection signature).
inline const char* client_dsl_addr_to_cstr(const int8_t* addr) {
    return reinterpret_cast<const char*>(addr);
}

// `Client` — user-facing RPC client facade. Authored as inline Rust
// DSL: the `#if RUSTYCPP_RUST` block below is the source of truth; the
// transpiler regenerates the matching `/*RUSTYCPP:GEN-BEGIN ... END*/`
// block. Drop trait emits a real destructor that calls `close()`.
//
// Behavioral diffs from the previous C++ class:
//   * Fields are no longer marked `private`; the DSL emits all as
//     public. The trailing `_` on each field is replaced with `_field`
//     because the transpiler considers `connection_` to collide with
//     the `connection()` accessor; same convention as other migrated
//     classes (CircuitBreaker, ConnectionMetrics, etc.).
//   * The user-declared move ctor/move-assign are dropped; the DSL
//     emits its own that respects the Drop-protocol `_rusty_forgotten`
//     flag. Copy ctor/assign emit `= delete` because the struct
//     contains a non-copyable `SpinMutex` field.
//   * `host()` now returns `rusty::String` instead of `std::string`.
//   * `metrics()` now returns `ConnectionMetrics` by value (works
//     because ConnectionMetrics is now Atomic-backed and copyable).
//     The previous static-local empty metrics shim is gone.
//   * `connect()`, `close()`, `reconnect()`, `set_valid()`,
//     `handle_free()`, `pause()`, `resume()` were previously out-of-
//     line; their bodies are now translated into the DSL block.
//   * `request_async<F>` is back in the DSL. Both it and
//     `ClientConnection::request_async` now return
//     `rusty::Result<rusty::Unit, i32>` (where `rusty::Unit` is
//     `std::tuple<>`). The transpiler defaults to emitting
//     `rusty::Unit` for Rust's `()` since rusty-cpp commit `32b718d`,
//     so the DSL surface reads `rusty::Result<(), i32>` and the
//     generated C++ matches `ClientConnection::request_async`'s
//     return type exactly.
#if RUSTYCPP_RUST
struct Client {
    connection_field: RefCell<Option<Arc<ClientConnection>>>,
    poll_thread_worker_field: Arc<PollThread>,
    is_client_mode_field: Cell<bool>,
    time_field: Cell<i64>,
    timeout_field: Cell<u64>,
    rpc_id_field: Cell<i32>,
    pending_keepalive_config_field: Cell<KeepaliveConfig>,
    pending_heartbeat_config_field: Cell<HeartbeatConfig>,
    pending_circuit_breaker_config_field: Cell<CircuitBreakerConfig>,
    pending_reconnect_policy_field: Cell<ReconnectPolicy>,
    callback_manager_field: Arc<CallbackManager>,
    pending_factory_field: SpinMutex<Option<ChannelFactoryProxy>>,
    // Per-Client empty metrics used as the no-connection fallback by
    // `metrics()` (returns a live ref). Per-instance rather than
    // program-global so a `static const ConnectionMetrics` isn't
    // needed in the DSL. Cheap because ConnectionMetrics is just 18
    // Atomic<u64> fields.
    empty_metrics_field: ConnectionMetrics,
}

impl Drop for Client {
    fn drop(&mut self) {
        self.close();
    }
}

impl Client {
    fn new(poll_thread_worker: Arc<PollThread>) -> Client {
        Client {
            connection_field: RefCell::<Option<Arc<ClientConnection>>>::new(None),
            poll_thread_worker_field: poll_thread_worker,
            is_client_mode_field: Cell::<bool>::new(false),
            time_field: Cell::<i64>::new(0i64),
            timeout_field: Cell::<u64>::new(0u64),
            rpc_id_field: Cell::<i32>::new(0i32),
            pending_keepalive_config_field: Cell::<KeepaliveConfig>::new(KeepaliveConfig {}),
            pending_heartbeat_config_field: Cell::<HeartbeatConfig>::new(HeartbeatConfig::disabled()),
            pending_circuit_breaker_config_field: Cell::<CircuitBreakerConfig>::new(CircuitBreakerConfig::disabled()),
            pending_reconnect_policy_field: Cell::<ReconnectPolicy>::new(ReconnectPolicy::conservative()),
            callback_manager_field: Arc::<CallbackManager>::new(CallbackManager::new()),
            pending_factory_field: SpinMutex::<Option<ChannelFactoryProxy>>::new(Option::<ChannelFactoryProxy>(None)),
            empty_metrics_field: ConnectionMetrics::new(),
        }
    }

    fn create(poll_thread_worker: Arc<PollThread>) -> Arc<Client> {
        Arc::<Client>::new(Client::new(poll_thread_worker))
    }

    fn set_client_mode(&self, v: bool) { self.is_client_mode_field.set(v); }
    fn client_mode(&self) -> bool { self.is_client_mode_field.get() }
    fn set_time(&self, v: i64) { self.time_field.set(v); }
    fn time(&self) -> i64 { self.time_field.get() }
    fn set_timeout(&self, v: u64) { self.timeout_field.set(v); }
    fn timeout(&self) -> u64 { self.timeout_field.get() }
    fn set_rpc_id(&self, v: i32) { self.rpc_id_field.set(v); }
    fn rpc_id(&self) -> i32 { self.rpc_id_field.get() }

    fn request<F>(&self, rpc_id: i32, attr: &FutureAttr, write_fn: F) -> FutureResult {
        let guard = self.connection_field.borrow();
        if guard.is_none() {
            return FutureResult::Err(ENOTCONN);
        }
        self.rpc_id_field.set(rpc_id);
        guard.as_ref().unwrap().request(rpc_id, attr, write_fn)
    }

    fn request_with_options<F>(&self, rpc_id: i32, options: &RequestOptions, write_fn: F) -> FutureResult {
        let guard = self.connection_field.borrow();
        if guard.is_none() {
            return FutureResult::Err(ENOTCONN);
        }
        self.rpc_id_field.set(rpc_id);
        guard.as_ref().unwrap().request_with_options(rpc_id, options, FutureAttr {}, write_fn)
    }

    fn request_async<F>(&self, rpc_id: i32, write_fn: F, on_reply: ClientConnection::AsyncReplyCallback) -> Result<(), i32> {
        let guard = self.connection_field.borrow();
        if guard.is_none() {
            return Result::<(), i32>::Err(ENOTCONN);
        }
        self.rpc_id_field.set(rpc_id);
        guard.as_ref().unwrap().request_async(rpc_id, write_fn, on_reply)
    }

    fn set_valid(&self, _valid: bool) {}

    fn connect(&self, addr: *const i8, client: bool) -> i32 {
        let conn: Arc<ClientConnection> =
            Arc::<ClientConnection>::make(self.poll_thread_worker_field.clone());
        let opt = conn.get_mut();
        verify(opt.is_some());
        let mut_conn: &mut ClientConnection = opt.unwrap();

        mut_conn.weak_self_ = conn.clone();
        mut_conn.set_callback_manager(self.callback_manager_field.clone());
        mut_conn.is_client_mode_ = client;
        self.is_client_mode_field.set(client);

        mut_conn.set_keepalive(self.pending_keepalive_config_field.get());
        mut_conn.set_heartbeat_config(self.pending_heartbeat_config_field.get());
        mut_conn.set_circuit_breaker_config(self.pending_circuit_breaker_config_field.get());
        mut_conn.set_reconnect_policy(self.pending_reconnect_policy_field.get());

        if !self.has_pending_channel_factory() {
            let tcp_factory: Arc<TcpFactory> = Arc::<TcpFactory>::new_(TcpFactory::new(self.poll_thread_worker_field.clone()));
            self.set_channel_factory(make_tcp_factory_proxy(tcp_factory));
        }

        {
            let guard = self.pending_factory_field.lock().unwrap();
            if guard.is_some() {
                let mut moved: ChannelFactoryProxy = guard.take().unwrap();
                mut_conn.bind_factory(moved);
            }
        }

        let result: i32 = mut_conn.connect(client_dsl_addr_to_cstr(addr));

        if result == 0i32 {
            let store_guard = self.connection_field.borrow_mut();
            *store_guard = Some(conn);
        }

        result
    }

    fn close(&self) {
        let guard = self.connection_field.borrow_mut();
        if guard.is_some() {
            let conn_ref = guard.as_ref().unwrap();
            let was_connected: bool = conn_ref.connected();
            conn_ref.mark_closing();
            if was_connected {
                let conn_arc: Arc<ClientConnection> = conn_ref.clone();
                // NOTE: keep the trailing-underscore C++ spelling here.
                // When written as Rust-idiomatic `::new(...)`, the transpiler
                // adds a spurious `-> rusty::Arc<PollThread>` return type to
                // the inner lambda (inferred from the next statement's
                // receiver type) and the lambda body becomes ill-typed.
                // Tracked as a transpiler bug; use `::new_(...)` until fixed.
                let close_job: Arc<OneTimeJob> =
                    Arc::<OneTimeJob>::new_(OneTimeJob::new_(move || {
                        conn_arc.close();
                    }));
                // Implicit Arc<OneTimeJob> -> Arc<Job> upcast via rusty::Arc's
                // template ctor (U* convertible to T*).
                self.poll_thread_worker_field.add(close_job);
            }
        }
    }

    fn handle_free(&self, xid: i64) {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            guard.as_ref().unwrap().handle_free(xid);
        }
    }

    fn pause(&self) {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            guard.as_ref().unwrap().pause();
        }
    }

    fn resume(&self) {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            guard.as_ref().unwrap().resume();
        }
    }

    fn reconnect(&self, on_complete: OnReconnectCompleteCallbackFn) -> i32 {
        let guard = self.connection_field.borrow();
        if guard.is_none() {
            if on_complete {
                on_complete(false);
            }
            return ENOTCONN;
        }
        guard.as_ref().unwrap().reconnect(on_complete)
    }

    fn set_channel_factory(&self, factory: ChannelFactoryProxy) {
        if !factory {
            return;
        }
        let guard = self.pending_factory_field.lock().unwrap();
        *guard = Some(factory);
    }

    fn has_pending_channel_factory(&self) -> bool {
        let guard = self.pending_factory_field.lock().unwrap();
        guard.is_some()
    }

    fn pending_request_count(&self) -> usize {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            return guard.as_ref().unwrap().pending_request_count();
        }
        0usize
    }

    fn clear_pending_requests(&self, error_code: i32) {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            guard.as_ref().unwrap().clear_pending_requests(error_code);
        }
    }

    fn is_reconnecting(&self) -> bool {
        let guard = self.connection_field.borrow();
        guard.is_some() && guard.as_ref().unwrap().is_reconnecting()
    }

    fn host(&self) -> String {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            return guard.as_ref().unwrap().host();
        }
        String {}
    }

    fn connected(&self) -> bool {
        let guard = self.connection_field.borrow();
        guard.is_some() && guard.as_ref().unwrap().connected()
    }

    fn connection_state(&self) -> ConnectionState {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            return guard.as_ref().unwrap().connection_state();
        }
        ConnectionState::NEW
    }

    fn try_reconnect_if_needed(&self) -> bool {
        let state: ConnectionState = self.connection_state();
        if (state as i32) == (ConnectionState::CONNECTED as i32) {
            return true;
        }
        if (state as i32) == (ConnectionState::FAILED as i32)
            || (state as i32) == (ConnectionState::DISCONNECTED as i32) {
            let result: i32 = self.reconnect(OnReconnectCompleteCallbackFn {});
            return result == 0i32;
        }
        false
    }

    fn connection(&self) -> Option<Arc<ClientConnection>> {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            return Some(guard.as_ref().unwrap().clone());
        }
        None
    }

    fn server_instance_id(&self) -> u64 {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            return guard.as_ref().unwrap().server_instance_id();
        }
        0u64
    }

    fn set_on_server_restart(&self, callback: OnServerRestartCallbackFn) {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            guard.as_ref().unwrap().set_on_server_restart(callback);
        }
    }

    fn check_server_instance(&self, new_id: u64) -> bool {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            return guard.as_ref().unwrap().check_server_instance(new_id);
        }
        false
    }

    fn set_reconnect_policy(&self, policy: &ReconnectPolicy) {
        self.pending_reconnect_policy_field.set(*policy);
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            guard.as_ref().unwrap().set_reconnect_policy(policy);
        }
    }

    fn set_buffering_config(&self, config: &BufferingConfig) {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            guard.as_ref().unwrap().set_buffering_config(config);
        }
    }

    fn set_keepalive(&self, config: &KeepaliveConfig) {
        self.pending_keepalive_config_field.set(*config);
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            guard.as_ref().unwrap().set_keepalive(config);
        }
    }

    fn keepalive_config(&self) -> KeepaliveConfig {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            return guard.as_ref().unwrap().keepalive_config();
        }
        self.pending_keepalive_config_field.get()
    }

    fn set_heartbeat(&self, config: &HeartbeatConfig) {
        self.pending_heartbeat_config_field.set(*config);
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            guard.as_ref().unwrap().set_heartbeat_config(config);
        }
    }

    fn heartbeat_config(&self) -> HeartbeatConfig {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            return guard.as_ref().unwrap().heartbeat_config();
        }
        self.pending_heartbeat_config_field.get()
    }

    fn set_circuit_breaker(&self, config: &CircuitBreakerConfig) {
        self.pending_circuit_breaker_config_field.set(*config);
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            guard.as_ref().unwrap().set_circuit_breaker_config(config);
        }
    }

    fn circuit_breaker_config(&self) -> CircuitBreakerConfig {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            return guard.as_ref().unwrap().circuit_breaker_config();
        }
        self.pending_circuit_breaker_config_field.get()
    }

    fn circuit_breaker_state(&self) -> CircuitState {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            return guard.as_ref().unwrap().circuit_breaker_state();
        }
        CircuitState::CLOSED
    }

    fn is_idle(&self, idle_ms: u64, current_time_ms: u64) -> bool {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            return guard.as_ref().unwrap().is_idle(idle_ms, current_time_ms);
        }
        false
    }

    fn validate_connection(&self) -> bool {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            return guard.as_ref().unwrap().validate_connection();
        }
        false
    }

    fn metrics(&self) -> &ConnectionMetrics {
        let guard = self.connection_field.borrow();
        if guard.is_some() {
            return &guard.as_ref().unwrap().metrics();
        }
        &self.empty_metrics_field
    }

    fn has_connection(&self) -> bool {
        let guard = self.connection_field.borrow();
        guard.is_some()
    }

    fn add_on_connected(&self, cb: OnConnectedCallbackFn) {
        self.callback_manager_field.add_on_connected(cb);
    }
    fn add_on_disconnected(&self, cb: OnConnectedCallbackFn) {
        self.callback_manager_field.add_on_disconnected(cb);
    }
    fn add_on_error(&self, cb: OnErrorCallbackFn) {
        self.callback_manager_field.add_on_error(cb);
    }
    fn add_on_reconnecting(&self, cb: OnConnectedCallbackFn) {
        self.callback_manager_field.add_on_reconnecting(cb);
    }
    fn add_on_reconnected(&self, cb: OnReconnectedCallbackFn) {
        self.callback_manager_field.add_on_reconnected(cb);
    }
    fn clear_connection_callbacks(&self) {
        self.callback_manager_field.clear_all();
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.1 version=1 rust_sha256=86639c0ee98fe75994366254889e0f3c1bae251c0a27d891d34bf488613f7798*/
struct Client;

struct Client {
    rusty::RefCell<rusty::Option<rusty::Arc<ClientConnection>>> connection_field;
    rusty::Arc<PollThread> poll_thread_worker_field;
    rusty::Cell<bool> is_client_mode_field;
    rusty::Cell<int64_t> time_field;
    rusty::Cell<uint64_t> timeout_field;
    rusty::Cell<int32_t> rpc_id_field;
    rusty::Cell<KeepaliveConfig> pending_keepalive_config_field;
    rusty::Cell<HeartbeatConfig> pending_heartbeat_config_field;
    rusty::Cell<CircuitBreakerConfig> pending_circuit_breaker_config_field;
    rusty::Cell<ReconnectPolicy> pending_reconnect_policy_field;
    rusty::Arc<CallbackManager> callback_manager_field;
    SpinMutex<rusty::Option<ChannelFactoryProxy>> pending_factory_field;
    ConnectionMetrics empty_metrics_field;
    mutable bool _rusty_forgotten = false;
    Client(rusty::RefCell<rusty::Option<rusty::Arc<ClientConnection>>> connection_field_init, rusty::Arc<PollThread> poll_thread_worker_field_init, rusty::Cell<bool> is_client_mode_field_init, rusty::Cell<int64_t> time_field_init, rusty::Cell<uint64_t> timeout_field_init, rusty::Cell<int32_t> rpc_id_field_init, rusty::Cell<KeepaliveConfig> pending_keepalive_config_field_init, rusty::Cell<HeartbeatConfig> pending_heartbeat_config_field_init, rusty::Cell<CircuitBreakerConfig> pending_circuit_breaker_config_field_init, rusty::Cell<ReconnectPolicy> pending_reconnect_policy_field_init, rusty::Arc<CallbackManager> callback_manager_field_init, SpinMutex<rusty::Option<ChannelFactoryProxy>> pending_factory_field_init, ConnectionMetrics empty_metrics_field_init) : connection_field(std::move(connection_field_init)), poll_thread_worker_field(std::move(poll_thread_worker_field_init)), is_client_mode_field(std::move(is_client_mode_field_init)), time_field(std::move(time_field_init)), timeout_field(std::move(timeout_field_init)), rpc_id_field(std::move(rpc_id_field_init)), pending_keepalive_config_field(std::move(pending_keepalive_config_field_init)), pending_heartbeat_config_field(std::move(pending_heartbeat_config_field_init)), pending_circuit_breaker_config_field(std::move(pending_circuit_breaker_config_field_init)), pending_reconnect_policy_field(std::move(pending_reconnect_policy_field_init)), callback_manager_field(std::move(callback_manager_field_init)), pending_factory_field(std::move(pending_factory_field_init)), empty_metrics_field(std::move(empty_metrics_field_init)) {}
    Client(const Client&) = delete;
    Client(Client&& other) noexcept : connection_field(std::move(other.connection_field)), poll_thread_worker_field(std::move(other.poll_thread_worker_field)), is_client_mode_field(std::move(other.is_client_mode_field)), time_field(std::move(other.time_field)), timeout_field(std::move(other.timeout_field)), rpc_id_field(std::move(other.rpc_id_field)), pending_keepalive_config_field(std::move(other.pending_keepalive_config_field)), pending_heartbeat_config_field(std::move(other.pending_heartbeat_config_field)), pending_circuit_breaker_config_field(std::move(other.pending_circuit_breaker_config_field)), pending_reconnect_policy_field(std::move(other.pending_reconnect_policy_field)), callback_manager_field(std::move(other.callback_manager_field)), pending_factory_field(std::move(other.pending_factory_field)), empty_metrics_field(std::move(other.empty_metrics_field)) {
        this->_rusty_forgotten = other._rusty_forgotten;
        other._rusty_forgotten = true;
    }
    Client& operator=(const Client&) = delete;
    Client& operator=(Client&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        this->~Client();
        new (this) Client(std::move(other));
        return *this;
    }
    void rusty_mark_forgotten() const noexcept { _rusty_forgotten = true; }


    ~Client() noexcept(false);
    static Client new_(rusty::Arc<PollThread> poll_thread_worker);
    static rusty::Arc<Client> create(rusty::Arc<PollThread> poll_thread_worker);
    void set_client_mode(bool v) const;
    bool client_mode() const;
    void set_time(int64_t v) const;
    int64_t time() const;
    void set_timeout(uint64_t v) const;
    uint64_t timeout() const;
    void set_rpc_id(int32_t v) const;
    int32_t rpc_id() const;
    template<typename F>
    FutureResult request(int32_t rpc_id, const FutureAttr& attr, F write_fn) const;
    template<typename F>
    FutureResult request_with_options(int32_t rpc_id, const RequestOptions& options, F write_fn) const;
    template<typename F>
    auto request_async(int32_t rpc_id, F write_fn, ClientConnection::AsyncReplyCallback on_reply) const -> rusty::Result<rusty::Unit, int32_t>;
    void set_valid(bool _valid) const;
    int32_t connect(const int8_t* addr, bool client) const;
    void close() const;
    void handle_free(int64_t xid) const;
    void pause() const;
    void resume() const;
    int32_t reconnect(OnReconnectCompleteCallbackFn on_complete) const;
    void set_channel_factory(ChannelFactoryProxy factory) const;
    bool has_pending_channel_factory() const;
    size_t pending_request_count() const;
    void clear_pending_requests(int32_t error_code) const;
    bool is_reconnecting() const;
    rusty::String host() const;
    bool connected() const;
    ConnectionState connection_state() const;
    bool try_reconnect_if_needed() const;
    rusty::Option<rusty::Arc<ClientConnection>> connection() const;
    uint64_t server_instance_id() const;
    void set_on_server_restart(OnServerRestartCallbackFn callback) const;
    bool check_server_instance(uint64_t new_id) const;
    void set_reconnect_policy(const ReconnectPolicy& policy) const;
    void set_buffering_config(const BufferingConfig& config) const;
    void set_keepalive(const KeepaliveConfig& config) const;
    KeepaliveConfig keepalive_config() const;
    void set_heartbeat(const HeartbeatConfig& config) const;
    HeartbeatConfig heartbeat_config() const;
    void set_circuit_breaker(const CircuitBreakerConfig& config) const;
    CircuitBreakerConfig circuit_breaker_config() const;
    CircuitState circuit_breaker_state() const;
    bool is_idle(uint64_t idle_ms, uint64_t current_time_ms) const;
    bool validate_connection() const;
    const ConnectionMetrics& metrics() const;
    bool has_connection() const;
    void add_on_connected(OnConnectedCallbackFn cb) const;
    void add_on_disconnected(OnConnectedCallbackFn cb) const;
    void add_on_error(OnErrorCallbackFn cb) const;
    void add_on_reconnecting(OnConnectedCallbackFn cb) const;
    void add_on_reconnected(OnReconnectedCallbackFn cb) const;
    void clear_connection_callbacks() const;
};


Client::~Client() noexcept(false) {
    if (_rusty_forgotten) { return; }
    this->close();
}

Client Client::new_(rusty::Arc<PollThread> poll_thread_worker) {
    return Client(rusty::RefCell<rusty::Option<rusty::Arc<ClientConnection>>>::new_(rusty::Option<rusty::Arc<ClientConnection>>{rusty::None}), std::move(poll_thread_worker), rusty::Cell<bool>::new_(false), rusty::Cell<int64_t>::new_(static_cast<int64_t>(0)), rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)), rusty::Cell<int32_t>::new_(static_cast<int32_t>(0)), rusty::Cell<KeepaliveConfig>::new_(KeepaliveConfig{}), rusty::Cell<HeartbeatConfig>::new_(HeartbeatConfig::disabled()), rusty::Cell<CircuitBreakerConfig>::new_(CircuitBreakerConfig::disabled()), rusty::Cell<ReconnectPolicy>::new_(ReconnectPolicy::conservative()), rusty::Arc<CallbackManager>::new_(CallbackManager::new_()), SpinMutex<rusty::Option<ChannelFactoryProxy>>::new_(rusty::Option<ChannelFactoryProxy>(rusty::None)), ConnectionMetrics::new_());
}

rusty::Arc<Client> Client::create(rusty::Arc<PollThread> poll_thread_worker) {
    return rusty::Arc<Client>::new_(Client::new_(std::move(poll_thread_worker)));
}

void Client::set_client_mode(bool v) const {
    this->is_client_mode_field.set(std::move(v));
}

bool Client::client_mode() const {
    return this->is_client_mode_field.get();
}

void Client::set_time(int64_t v) const {
    this->time_field.set(std::move(v));
}

int64_t Client::time() const {
    return this->time_field.get();
}

void Client::set_timeout(uint64_t v) const {
    this->timeout_field.set(std::move(v));
}

uint64_t Client::timeout() const {
    return this->timeout_field.get();
}

void Client::set_rpc_id(int32_t v) const {
    this->rpc_id_field.set(std::move(v));
}

int32_t Client::rpc_id() const {
    return this->rpc_id_field.get();
}

template<typename F>
FutureResult Client::request(int32_t rpc_id, const FutureAttr& attr, F write_fn) const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_none()) {
        return FutureResult::Err(ENOTCONN);
    }
    this->rpc_id_field.set(std::move(rpc_id));
    return guard->as_ref().unwrap()->request(std::move(rpc_id), attr, std::move(write_fn));
}

template<typename F>
FutureResult Client::request_with_options(int32_t rpc_id, const RequestOptions& options, F write_fn) const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_none()) {
        return FutureResult::Err(ENOTCONN);
    }
    this->rpc_id_field.set(std::move(rpc_id));
    return guard->as_ref().unwrap()->request_with_options(std::move(rpc_id), options, FutureAttr{}, std::move(write_fn));
}

template<typename F>
auto Client::request_async(int32_t rpc_id, F write_fn, ClientConnection::AsyncReplyCallback on_reply) const -> rusty::Result<rusty::Unit, int32_t> {
    const auto guard = this->connection_field.borrow();
    if (guard->is_none()) {
        return rusty::Result<rusty::Unit, int32_t>::Err(ENOTCONN);
    }
    this->rpc_id_field.set(std::move(rpc_id));
    return guard->as_ref().unwrap()->request_async(std::move(rpc_id), std::move(write_fn), std::move(on_reply));
}

void Client::set_valid(bool _valid) const {
}

int32_t Client::connect(const int8_t* addr, bool client) const {
    rusty::Arc<ClientConnection> conn = rusty::Arc<ClientConnection>::make(rusty::clone(this->poll_thread_worker_field));
    auto opt = conn.get_mut();
    verify(opt.is_some());
    ClientConnection& mut_conn = opt.unwrap();
    mut_conn.weak_self_ = rusty::clone(conn);
    mut_conn.set_callback_manager(rusty::clone(this->callback_manager_field));
    mut_conn.is_client_mode_ = std::move(client);
    this->is_client_mode_field.set(std::move(client));
    mut_conn.set_keepalive(rusty::detail::deref_if_pointer_like(this->pending_keepalive_config_field.get()));
    mut_conn.set_heartbeat_config(this->pending_heartbeat_config_field.get());
    mut_conn.set_circuit_breaker_config(this->pending_circuit_breaker_config_field.get());
    mut_conn.set_reconnect_policy(rusty::detail::deref_if_pointer_like(this->pending_reconnect_policy_field.get()));
    if (!this->has_pending_channel_factory()) {
        const rusty::Arc<TcpFactory> tcp_factory = rusty::Arc<TcpFactory>::new_(TcpFactory::new_(rusty::clone(this->poll_thread_worker_field)));
        this->set_channel_factory(make_tcp_factory_proxy(std::move(tcp_factory)));
    }
    {
        auto guard = this->pending_factory_field.lock().unwrap();
        if (guard->is_some()) {
            ChannelFactoryProxy moved = guard->take().unwrap();
            mut_conn.bind_factory(std::move(moved));
        }
    }
    int32_t result = mut_conn.connect(client_dsl_addr_to_cstr(addr));
    if (rusty::detail::deref_if_pointer_like(result) == static_cast<int32_t>(0)) {
        auto store_guard = this->connection_field.borrow_mut();
        *store_guard = rusty::Option<rusty::Arc<ClientConnection>>(std::move(conn));
    }
    return std::move(result);
}

void Client::close() const {
    auto guard = this->connection_field.borrow_mut();
    if (guard->is_some()) {
        auto& conn_ref = guard->as_ref().unwrap();
        const bool was_connected = conn_ref->connected();
        conn_ref->mark_closing();
        if (was_connected) {
            const rusty::Arc<ClientConnection> conn_arc = rusty::clone(conn_ref);
            const rusty::Arc<OneTimeJob> close_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob::new_([=, conn_arc = std::move(conn_arc)]() mutable {
conn_arc->close();
}));
            this->poll_thread_worker_field->add(std::move(close_job));
        }
    }
}

void Client::handle_free(int64_t xid) const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        guard->as_ref().unwrap()->handle_free(std::move(xid));
    }
}

void Client::pause() const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        guard->as_ref().unwrap()->pause();
    }
}

void Client::resume() const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        guard->as_ref().unwrap()->resume();
    }
}

int32_t Client::reconnect(OnReconnectCompleteCallbackFn on_complete) const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_none()) {
        if (on_complete) {
            on_complete(false);
        }
        return ENOTCONN;
    }
    return guard->as_ref().unwrap()->reconnect(std::move(on_complete));
}

void Client::set_channel_factory(ChannelFactoryProxy factory) const {
    if (!factory) {
        return;
    }
    auto guard = this->pending_factory_field.lock().unwrap();
    *guard = rusty::Option<ChannelFactoryProxy>(std::move(factory));
}

bool Client::has_pending_channel_factory() const {
    auto guard = this->pending_factory_field.lock().unwrap();
    return guard->is_some();
}

size_t Client::pending_request_count() const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        return guard->as_ref().unwrap()->pending_request_count();
    }
    return static_cast<size_t>(0);
}

void Client::clear_pending_requests(int32_t error_code) const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        guard->as_ref().unwrap()->clear_pending_requests(std::move(error_code));
    }
}

bool Client::is_reconnecting() const {
    const auto guard = this->connection_field.borrow();
    return guard->is_some() && guard->as_ref().unwrap()->is_reconnecting();
}

rusty::String Client::host() const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        return guard->as_ref().unwrap()->host();
    }
    return rusty::String{};
}

bool Client::connected() const {
    const auto guard = this->connection_field.borrow();
    return guard->is_some() && guard->as_ref().unwrap()->connected();
}

ConnectionState Client::connection_state() const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        return guard->as_ref().unwrap()->connection_state();
    }
    return rusty::clone(rusty::clone(ConnectionState::NEW));
}

bool Client::try_reconnect_if_needed() const {
    const ConnectionState state = this->connection_state();
    if (((static_cast<int32_t>(state))) == ((static_cast<int32_t>(ConnectionState::CONNECTED)))) {
        return true;
    }
    if ((((static_cast<int32_t>(state))) == ((static_cast<int32_t>(ConnectionState::FAILED)))) || (((static_cast<int32_t>(state))) == ((static_cast<int32_t>(ConnectionState::DISCONNECTED))))) {
        const int32_t result = this->reconnect(OnReconnectCompleteCallbackFn{});
        return rusty::detail::deref_if_pointer_like(result) == static_cast<int32_t>(0);
    }
    return false;
}

rusty::Option<rusty::Arc<ClientConnection>> Client::connection() const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        return rusty::Option<rusty::Arc<ClientConnection>>(rusty::clone(guard->as_ref().unwrap()));
    }
    return rusty::Option<rusty::Arc<ClientConnection>>{rusty::None};
}

uint64_t Client::server_instance_id() const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        return guard->as_ref().unwrap()->server_instance_id();
    }
    return static_cast<uint64_t>(0);
}

void Client::set_on_server_restart(OnServerRestartCallbackFn callback) const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        guard->as_ref().unwrap()->set_on_server_restart(std::move(callback));
    }
}

bool Client::check_server_instance(uint64_t new_id) const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        return guard->as_ref().unwrap()->check_server_instance(std::move(new_id));
    }
    return false;
}

void Client::set_reconnect_policy(const ReconnectPolicy& policy) const {
    this->pending_reconnect_policy_field.set(policy);
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        guard->as_ref().unwrap()->set_reconnect_policy(policy);
    }
}

void Client::set_buffering_config(const BufferingConfig& config) const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        guard->as_ref().unwrap()->set_buffering_config(config);
    }
}

void Client::set_keepalive(const KeepaliveConfig& config) const {
    this->pending_keepalive_config_field.set(config);
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        guard->as_ref().unwrap()->set_keepalive(config);
    }
}

KeepaliveConfig Client::keepalive_config() const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        return guard->as_ref().unwrap()->keepalive_config();
    }
    return this->pending_keepalive_config_field.get();
}

void Client::set_heartbeat(const HeartbeatConfig& config) const {
    this->pending_heartbeat_config_field.set(config);
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        guard->as_ref().unwrap()->set_heartbeat_config(config);
    }
}

HeartbeatConfig Client::heartbeat_config() const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        return guard->as_ref().unwrap()->heartbeat_config();
    }
    return this->pending_heartbeat_config_field.get();
}

void Client::set_circuit_breaker(const CircuitBreakerConfig& config) const {
    this->pending_circuit_breaker_config_field.set(config);
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        guard->as_ref().unwrap()->set_circuit_breaker_config(config);
    }
}

CircuitBreakerConfig Client::circuit_breaker_config() const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        return guard->as_ref().unwrap()->circuit_breaker_config();
    }
    return this->pending_circuit_breaker_config_field.get();
}

CircuitState Client::circuit_breaker_state() const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        return guard->as_ref().unwrap()->circuit_breaker_state();
    }
    return rusty::clone(rusty::clone(CircuitState::CLOSED));
}

bool Client::is_idle(uint64_t idle_ms, uint64_t current_time_ms) const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        return guard->as_ref().unwrap()->is_idle(std::move(idle_ms), std::move(current_time_ms));
    }
    return false;
}

bool Client::validate_connection() const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        return guard->as_ref().unwrap()->validate_connection();
    }
    return false;
}

const ConnectionMetrics& Client::metrics() const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        return guard->as_ref().unwrap()->metrics();
    }
    return this->empty_metrics_field;
}

bool Client::has_connection() const {
    const auto guard = this->connection_field.borrow();
    return guard->is_some();
}

void Client::add_on_connected(OnConnectedCallbackFn cb) const {
    this->callback_manager_field->add_on_connected(std::move(cb));
}

void Client::add_on_disconnected(OnConnectedCallbackFn cb) const {
    this->callback_manager_field->add_on_disconnected(std::move(cb));
}

void Client::add_on_error(OnErrorCallbackFn cb) const {
    this->callback_manager_field->add_on_error(std::move(cb));
}

void Client::add_on_reconnecting(OnConnectedCallbackFn cb) const {
    this->callback_manager_field->add_on_reconnecting(std::move(cb));
}

void Client::add_on_reconnected(OnReconnectedCallbackFn cb) const {
    this->callback_manager_field->add_on_reconnected(std::move(cb));
}

void Client::clear_connection_callbacks() const {
    this->callback_manager_field->clear_all();
}
/*RUSTYCPP:GEN-END id=client.1*/


// @safe - Thread-safe pool of client connections using Arc
// MIGRATED: Now uses rusty::Arc<Client> for cached connections
class ClientPool {

    // owns a shared reference to PollThread
    rusty::Option<rusty::Arc<rrr::PollThread>> poll_thread_worker_;

    // Mutex-protected state. Bundling cache + load-balancer state in a
    // single SpinMutex matches the access pattern (get_client touches
    // both under one lock) and replaces the prior `SpinLock l_ +
    // unprotected fields` pattern with rusty's RAII guard.
    // Thin std::map subclass that exposes the BTreeMap-style surface
    // (`get`, `remove`, `keys`, `len`, two-arg `insert`) so the rest of
    // ClientPool keeps using the rusty-idiomatic call style we wrote
    // it against. Switched away from `rusty::BTreeMap` because the
    // transpiled BTreeMap port has unresolved transpiler bugs that
    // surface when iter() / clone() / remove() are instantiated.
    // Migrate back when the upstream BTreeMap port is fixed.
    template<typename K, typename V>
    struct CompatMap : std::map<K, V> {
        using std::map<K, V>::map;
        // BTreeMap::get(K) -> Option<V&>
        ::rusty::Option<V&> get(const K& key) {
            auto it = this->find(key);
            if (it == this->end()) return ::rusty::Option<V&>(::rusty::None);
            return ::rusty::Option<V&>(it->second);
        }
        ::rusty::Option<const V&> get(const K& key) const {
            auto it = this->find(key);
            if (it == this->end()) return ::rusty::Option<const V&>(::rusty::None);
            return ::rusty::Option<const V&>(it->second);
        }
        // BTreeMap::insert(K, V) -> Option<V> (old value if any)
        ::rusty::Option<V> insert(K key, V value) {
            auto it = this->find(key);
            if (it != this->end()) {
                V old = std::move(it->second);
                it->second = std::move(value);
                return ::rusty::Option<V>(std::move(old));
            }
            std::map<K, V>::emplace(std::move(key), std::move(value));
            return ::rusty::Option<V>(::rusty::None);
        }
        // BTreeMap::remove(K) -> Option<V>
        ::rusty::Option<V> remove(const K& key) {
            auto it = this->find(key);
            if (it == this->end()) return ::rusty::Option<V>(::rusty::None);
            V v = std::move(it->second);
            this->erase(it);
            return ::rusty::Option<V>(std::move(v));
        }
        // BTreeMap::len() -> size_t
        std::size_t len() const { return this->size(); }
        // BTreeMap::keys() — snapshot keys into a rusty::Vec for caller.
        ::rusty::Vec<K> keys() const {
            ::rusty::Vec<K> out;
            for (const auto& kv : *this) out.push(kv.first);
            return out;
        }
    };

    struct PoolState {
        // @safe - rusty::Arc<Client> for thread-safe reference counting.
        CompatMap<std::string, rusty::Vec<rusty::Arc<Client>>> cache;
        // Load balancer state per address (for round-robin tracking).
        CompatMap<std::string, LoadBalancerState> lb_state;
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
      state_machine_(ConnectionStateMachine::new_()),
      heartbeat_manager_(HeartbeatManager::new_(HeartbeatConfig::disabled())),
      circuit_breaker_(CircuitBreaker::new_(CircuitBreakerConfig::disabled())),
      callback_manager_(rusty::Arc<CallbackManager>::new_(CallbackManager::new_())),
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
  // HashMap's STL iterator yields std::tuple<const K&, V&>, not
  // std::pair, so the value is at std::get<1>(it), not it.second.
  for (auto it: *guard) {
    futures.push_back(std::get<1>(it));  // Copy Arc
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
  auto calc = ReconnectCalculator::new_(reconnect_policy_);
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
  body << v64(xid_counter_.next(1));
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
    // The FiberChannel ctor only inits fields; bind_callbacks wires
    // the [this]-capturing on_frame/on_closed/on_error lambdas onto
    // the owned channel proxy. Must run after the Box-allocated
    // FiberChannel is in its final memory location (so `this` is
    // pinned).
    guard->as_ref().unwrap()->bind_callbacks();
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
    // Wire up the on_frame/on_closed/on_error lambdas on the just-
    // installed FiberChannel (see comment in the make_box site above
    // — bind_callbacks() runs after the Box address is final).
    guard->as_ref().unwrap()->bind_callbacks();
  }
  channel_mode_.set(true);

  WeakClientConnection weak_self = weak_self_;

  // Schedule the recv-loop fiber spawn onto the poll thread. The
  // poll thread's `trigger_job` calls `Fiber::create_run` from
  // its own reactor, so the resulting fiber's IntEvent waits and
  // the `on_frame` callback's signal both land on the same
  // thread.
  // @unsafe { Arc::new_ + rusty::Function + cross-thread queue }
  auto recv_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob::new_([weak_self]() {
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
    auto key_vec = guard->cache.keys();
    keys.reserve(key_vec.size());
    // CompatMap::keys() returns a snapshot rusty::Vec; iterate with
    // STL-style range-for instead of the old Rust-iter `.next()` loop.
    for (auto& addr : key_vec) {
      keys.push(std::string(addr));
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
    auto key_vec = guard->cache.keys();
    keys.reserve(key_vec.size());
    // CompatMap::keys() returns a snapshot rusty::Vec; iterate with
    // STL-style range-for instead of the old Rust-iter `.next()` loop.
    for (auto& addr : key_vec) {
      keys.push(std::string(addr));
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
    guard->lb_state.insert(addr, LoadBalancerState::new_());
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
        static_cast<size_t>(RandomGenerator::rand(0, RAND_MAX))
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
        if (client->connect(reinterpret_cast<const int8_t*>(addr.c_str()), true) != 0) {
          Log_warn("ClientPool: failed to create new connection to %s", addr.c_str());
          ok = false;
          break;
        }
        clients.push(client);
      }

      if (ok && !clients.is_empty()) {
        sp_cl = rusty::Some(clients[static_cast<size_t>(RandomGenerator::rand(0, static_cast<int>(clients.size()) - 1))].clone());
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
      if (client->connect(reinterpret_cast<const int8_t*>(addr.c_str()), true) != 0) {
        ok = false;
        break;
      }
      parallel_clients.push(client);
    }
    if (ok) {
      sp_cl = rusty::Some(parallel_clients[static_cast<size_t>(RandomGenerator::rand(0, static_cast<int>(parallel_clients.size()) - 1))].clone());
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
