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

struct Future;
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

// @safe - Future is the async result handle for one RPC. Its state is all
// rusty interior-mutability primitives (Cell / RefCell / Mutex / Condvar),
// so it analyzes @safe; the Condvar / std::chrono / callback-dispatch bodies
// (wait / timed_wait / notify_ready / get_reply / get_error_code /
// wait_with_options) live in `fut_*` free fns the DSL methods delegate to.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block is the source
// of truth; the transpiler regenerates the matching GEN block. The former
// private ctor/dtor + `friend Arc/Client/ClientConnection` are gone — the
// DSL emits a public struct, and Client/ClientConnection reach the (now
// public) fields directly. The lifecycle invariant (constructed only via
// `create()` / `Arc::make`, held only through Arc) is unchanged by
// convention. `safe_release` collapses to a single no-op `Arc<Future>`
// overload (the dead `Future*` overload is dropped; the `FutureResult`
// callers now `(void)`-discard).

// CompletionFn: the DSL can't parse `Function<void()>` as a generic type
// argument, so alias it (mirrors OnFrameCallback / QueuedRequestCallback).
using CompletionFn = rusty::Function<void()>;

// Free-fn implementations of the Condvar / std::chrono / callback-heavy
// methods; the DSL methods below delegate to these. Defined in the Future
// implementation section further down. `self` is `const Future&` — every
// mutation goes through interior mutability, so const is correct.
void                   fut_wait(const Future& self);
void                   fut_timed_wait(const Future& self, double sec);
bool                   fut_wait_with_options(const Future& self);
rusty::RefMut<Marshal> fut_get_reply(const Future& self);
i32                    fut_get_error_code(const Future& self);
void                   fut_notify_ready(const Future& self, rusty::Arc<Future> self_arc);

#if RUSTYCPP_RUST
struct FutureState {
    ready: bool,
    timed_out: bool,
    completion_callbacks: Vec<CompletionFn>,
}

impl FutureState {
    fn new() -> FutureState {
        FutureState { ready: false, timed_out: false, completion_callbacks: Vec::<CompletionFn>::new() }
    }
}

struct Future {
    xid_: i64,
    error_code_: Cell<i32>,
    attr_: FutureAttr,
    reply_: RefCell<Marshal>,
    timeout_: u64,
    state_: Mutex<FutureState>,
    ready_cond_: Condvar,
    options_: Cell<RequestOptions>,
    timeout_type_: Cell<TimeoutType>,
    retry_count_: Cell<u16>,
}

impl Future {
    #[cpp_ctor] fn new(xid: i64, attr: FutureAttr) -> Future {
        Future {
            xid_: xid,
            error_code_: Cell::new(0i32),
            attr_: attr,
            reply_: RefCell::<Marshal>::new(Marshal::new()),
            timeout_: 1000000u64,
            state_: Mutex::<FutureState>::new(FutureState::new()),
            ready_cond_: rusty::Condvar::new(),
            options_: Cell::new(RequestOptions::defaults()),
            timeout_type_: Cell::new(TimeoutType::NONE),
            retry_count_: Cell::new(0u16),
        }
    }

    fn create(xid: i64, attr: FutureAttr) -> Arc<Future> {
        Arc::<Future>::make(xid, attr)
    }

    fn ready(&self) -> bool {
        let guard = self.state_.lock().unwrap();
        guard.ready
    }

    fn wait(&self) {
        fut_wait(self)
    }

    fn timed_wait(&self, sec: f64) {
        fut_timed_wait(self, sec)
    }

    fn wait_with_options(&self) -> bool {
        fut_wait_with_options(self)
    }

    fn timed_out(&self) -> bool {
        let guard = self.state_.lock().unwrap();
        guard.timed_out
    }

    fn add_completion_callback(&self, callback: CompletionFn) -> bool {
        let guard = self.state_.lock().unwrap();
        if guard.ready || guard.timed_out {
            return false;
        }
        guard.completion_callbacks.push(callback);
        true
    }

    fn get_reply(&self) -> rusty::RefMut<Marshal> {
        fut_get_reply(self)
    }

    fn get_error_code(&self) -> i32 {
        fut_get_error_code(self)
    }

    fn get_xid(&self) -> i64 {
        self.xid_
    }

    fn get_options(&self) -> RequestOptions {
        self.options_.get()
    }

    fn set_options(&self, opts: &RequestOptions) {
        self.options_.set(opts)
    }

    fn get_timeout_type(&self) -> TimeoutType {
        self.timeout_type_.get()
    }

    fn set_timeout_type(&mut self, type_: TimeoutType) {
        self.timeout_type_.set(type_)
    }

    fn get_retry_count(&self) -> u16 {
        self.retry_count_.get()
    }

    fn increment_retry_count(&mut self) -> u16 {
        let current = self.retry_count_.get();
        self.retry_count_.set(current + 1u16);
        current + 1u16
    }

    fn should_retry(&self) -> bool {
        let opts = self.options_.get();
        opts.can_retry(self.retry_count_.get())
    }

    fn notify_ready(&self, self_arc: Arc<Future>) {
        fut_notify_ready(self, self_arc)
    }

    fn safe_release(fu: Arc<Future>) {
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.future version=1 rust_sha256=01b516b1a7c1ea038fb5c31219e5cae9fc48f09aa582d4f1a1bba9eb851f769d*/
struct FutureState;
struct Future;

struct FutureState {
    bool ready;
    bool timed_out;
    rusty::Vec<CompletionFn> completion_callbacks;

    static FutureState new_();
};

struct Future {
    int64_t xid_;
    rusty::Cell<int32_t> error_code_;
    FutureAttr attr_;
    rusty::RefCell<Marshal> reply_;
    uint64_t timeout_;
    rusty::Mutex<FutureState> state_;
    rusty::Condvar ready_cond_;
    rusty::Cell<RequestOptions> options_;
    rusty::Cell<TimeoutType> timeout_type_;
    rusty::Cell<uint16_t> retry_count_;

    Future(int64_t xid, FutureAttr attr);
    static rusty::Arc<Future> create(int64_t xid, FutureAttr attr);
    bool ready() const;
    void wait() const;
    void timed_wait(double sec) const;
    bool wait_with_options() const;
    bool timed_out() const;
    bool add_completion_callback(CompletionFn callback) const;
    rusty::RefMut<Marshal> get_reply() const;
    int32_t get_error_code() const;
    int64_t get_xid() const;
    RequestOptions get_options() const;
    void set_options(const RequestOptions& opts) const;
    TimeoutType get_timeout_type() const;
    void set_timeout_type(TimeoutType type_);
    uint16_t get_retry_count() const;
    uint16_t increment_retry_count();
    bool should_retry() const;
    void notify_ready(rusty::Arc<Future> self_arc) const;
    static void safe_release(rusty::Arc<Future> fu);
};


FutureState FutureState::new_() {
    return FutureState{.ready = false, .timed_out = false, .completion_callbacks = rusty::Vec<CompletionFn>::new_()};
}

Future::Future(int64_t xid, FutureAttr attr)
    : xid_(std::move(xid))
    , error_code_(rusty::Cell<int32_t>::new_(static_cast<int32_t>(0)))
    , attr_(std::move(attr))
    , reply_(rusty::RefCell<Marshal>::new_(Marshal::new_()))
    , timeout_(static_cast<uint64_t>(1000000))
    , state_(rusty::Mutex<FutureState>::new_(FutureState::new_()))
    , ready_cond_(rusty::Condvar::new_())
    , options_(rusty::Cell<RequestOptions>::new_(RequestOptions::defaults()))
    , timeout_type_(rusty::Cell<TimeoutType>::new_(rusty::clone(rusty::clone(TimeoutType::NONE))))
    , retry_count_(rusty::Cell<uint16_t>::new_(static_cast<uint16_t>(0)))
{}

rusty::Arc<Future> Future::create(int64_t xid, FutureAttr attr) {
    return rusty::Arc<Future>::make(std::move(xid), std::move(attr));
}

bool Future::ready() const {
    auto guard = this->state_.lock().unwrap();
    return std::move((*guard).ready);
}

void Future::wait() const {
    fut_wait((*this));
}

void Future::timed_wait(double sec) const {
    fut_timed_wait((*this), std::move(sec));
}

bool Future::wait_with_options() const {
    return fut_wait_with_options((*this));
}

bool Future::timed_out() const {
    auto guard = this->state_.lock().unwrap();
    return std::move((*guard).timed_out);
}

bool Future::add_completion_callback(CompletionFn callback) const {
    auto guard = this->state_.lock().unwrap();
    if (rusty::detail::deref_if_pointer_like((*guard).ready) || rusty::detail::deref_if_pointer_like((*guard).timed_out)) {
        return false;
    }
    (*guard).completion_callbacks.push(std::move(callback));
    return true;
}

rusty::RefMut<Marshal> Future::get_reply() const {
    return fut_get_reply((*this));
}

int32_t Future::get_error_code() const {
    return fut_get_error_code((*this));
}

int64_t Future::get_xid() const {
    return this->xid_;
}

RequestOptions Future::get_options() const {
    return this->options_.get();
}

void Future::set_options(const RequestOptions& opts) const {
    this->options_.set(std::move(opts));
}

TimeoutType Future::get_timeout_type() const {
    return this->timeout_type_.get();
}

void Future::set_timeout_type(TimeoutType type_) {
    this->timeout_type_.set(std::move(type_));
}

uint16_t Future::get_retry_count() const {
    return this->retry_count_.get();
}

uint16_t Future::increment_retry_count() {
    const auto current = this->retry_count_.get();
    this->retry_count_.set(rusty::detail::deref_if_pointer_like(current) + static_cast<uint16_t>(1));
    return rusty::detail::deref_if_pointer_like(current) + static_cast<uint16_t>(1);
}

bool Future::should_retry() const {
    const auto opts = this->options_.get();
    return opts.can_retry(this->retry_count_.get());
}

void Future::notify_ready(rusty::Arc<Future> self_arc) const {
    fut_notify_ready((*this), std::move(self_arc));
}

void Future::safe_release(rusty::Arc<Future> fu) {
}
/*RUSTYCPP:GEN-END id=client.future*/

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

// Hand-written sub-struct holding ClientConnection's reconnect-coordination
// atomics. CAS + acquire/release ordering cannot be modeled by rusty::Cell
// (single-thread, Copy-only), so these stay std::atomic; ClientConnection
// holds this by value (as a `mutable` member, so const methods can drive the
// atomics — atomic semantics make it race-free), and the eventual DSL struct
// declares it as an opaque field. The recv/reconnect machinery accesses the
// atomics directly via `reconnect_.<field>`.
struct ReconnectState {
  mutable std::atomic<bool> reconnecting_{false};
  mutable std::atomic<bool> reconnect_abort_{false};
  // auto-reconnect attempt counter — incremented before the reconnect-thread
  // spawn in on_channel_closed_fan_out; tests inspect it to verify the
  // fan-out reached the reconnect-policy branch.
  mutable std::atomic<uint64_t> channel_reconnect_attempts_{0};

  ReconnectState() = default;
  // std::atomic is not movable, but the DSL-emitted move ctor of the owning
  // ClientConnection (impl Drop synthesizes one that std::moves every field)
  // needs ReconnectState to be movable. Move the current atomic VALUES; the
  // owning connection is never actually moved at runtime (held via Arc), so
  // this only needs to compile.
  ReconnectState(ReconnectState&& o) noexcept
      : reconnecting_(o.reconnecting_.load(std::memory_order_relaxed)),
        reconnect_abort_(o.reconnect_abort_.load(std::memory_order_relaxed)),
        channel_reconnect_attempts_(
            o.channel_reconnect_attempts_.load(std::memory_order_relaxed)) {}
  ReconnectState(const ReconnectState&) = delete;
  ReconnectState& operator=(const ReconnectState&) = delete;
  ReconnectState& operator=(ReconnectState&&) = delete;
};

// Async-callback type for request_async. Hoisted from a nested
// ClientConnection typedef to namespace scope so the DSL struct (Phase 5
// flip) can reference it as a field type — the DSL parser rejects an
// in-struct `using` alias, but a namespace-scope alias resolves fine as a
// field/param type.
using AsyncReplyCallback = rusty::Function<
    void(i32 /*error_code*/, const uint8_t* /*reply_bytes*/, size_t /*reply_size*/)>;

// @unsafe - Build the pre-filled async-callback slot vector
// (kAsyncSlotCount Nones) for ClientConnection::pending_cb_slots_.
// Factored out of the ctor body because the Phase 5 DSL `#[cpp_ctor]` has
// no loop-capable body — it field-inits pending_cb_slots_ via
// `SpinMutex::new(make_prefilled_cb_slots())`.
inline rusty::Vec<rusty::Option<AsyncReplyCallback>> make_prefilled_cb_slots() {
  rusty::Vec<rusty::Option<AsyncReplyCallback>> slots;
  slots.reserve(kAsyncSlotCount);
  for (size_t i = 0; i < kAsyncSlotCount; ++i) {
    slots.push(rusty::None);
  }
  return slots;
}

// Hoisted above the ClientConnection DSL struct (Phase 5 flip): the DSL
// parser rejects a `rusty::Function<void(...)>` field/param type.
using OnReconnectCompleteCallbackFn = rusty::Function<void(bool)>;
using OnServerRestartCallbackFn = rusty::Function<void(uint64_t, uint64_t)>;

// Forward declarations of the clientconn_* free fns the DSL struct's
// generated out-of-line method defs delegate to (the friend decls that
// previously provided visibility are gone with the class).
struct ClientConnection;
inline RequestQueue make_pending_queue(const RequestQueueConfig& c);
void clientconn_drop(const ClientConnection& self);
void clientconn_mark_closing(const ClientConnection& self);
void clientconn_record_circuit_result(const ClientConnection& self, i32 err);
void clientconn_record_circuit_state_transition(const ClientConnection& self, CircuitState before, CircuitState after);
void clientconn_invoke_error_callback(const ClientConnection& self, i32 err, const std::string& message);
void clientconn_invoke_disconnected_callback(const ClientConnection& self);
void clientconn_invoke_reconnecting_callback(const ClientConnection& self);
void clientconn_invoke_reconnected_callback(const ClientConnection& self, bool success);
void clientconn_invoke_connected_callback(const ClientConnection& self);
void clientconn_set_buffering_config(const ClientConnection& self, const BufferingConfig& config);
void clientconn_set_heartbeat_config(const ClientConnection& self, const HeartbeatConfig& config);
void clientconn_set_circuit_breaker_config(const ClientConnection& self, const CircuitBreakerConfig& config);
void clientconn_fail_pending_future(const ClientConnection& self, i64 xid, int err);
void clientconn_handle_free(const ClientConnection& self, i64 xid);
void clientconn_enqueue_heartbeat_probe(const ClientConnection& self);
bool clientconn_check_pending_write_update(const ClientConnection& self);
void clientconn_invalidate_pending_futures(const ClientConnection& self);
void clientconn_close(const ClientConnection& self);
void clientconn_reset_channel_mode_for_reconnect(const ClientConnection& self);
void clientconn_run_recv_loop(const ClientConnection& self);
void clientconn_handle_error(const ClientConnection& self);
void clientconn_decode_response_and_notify(const ClientConnection& self, const std::uint8_t* bytes, std::size_t size);
void clientconn_on_channel_closed_fan_out(const ClientConnection& self);
ChannelError clientconn_dispatch_frame_via_channel(const ClientConnection& self, const std::uint8_t* body_bytes, std::size_t body_size);
int clientconn_connect(const ClientConnection& self, const int8_t* addr);
int clientconn_reconnect(const ClientConnection& self, rusty::Function<void(bool)> on_complete);
int clientconn_connect_via_factory(const ClientConnection& self, const int8_t* addr);
void clientconn_bind_channel(const ClientConnection& self, ChannelConnectionProxy channel);
void clientconn_bind_channel_via_poll_thread(const ClientConnection& self, ChannelConnectionProxy channel);
void clientconn_bind_channel_direct(const ClientConnection& self, ChannelConnectionProxy channel);
bool clientconn_should_trip_circuit_for_error(i32 err);
RpcError clientconn_map_system_error(i32 err);
void clientconn_bind_factory(ClientConnection& self, ChannelFactoryProxy factory);
void clientconn_set_reconnect_policy(const ClientConnection& self, const ReconnectPolicy& policy);
void clientconn_set_on_server_restart(const ClientConnection& self, rusty::Function<void(uint64_t, uint64_t)> callback);
bool clientconn_check_server_instance(const ClientConnection& self, uint64_t new_id);
void clientconn_on_request_dispatched(const ClientConnection& self, size_t bytes);
void clientconn_on_response_received(const ClientConnection& self, size_t bytes);
uint64_t clientconn_monotonic_ms_now();
template<typename F>
FutureResult clientconn_request_via_channel(const ClientConnection& self, i32 rpc_id, const FutureAttr& attr, F&& write_fn);
template<typename F>
FutureResult clientconn_request_with_options(const ClientConnection& self, i32 rpc_id, const RequestOptions& options, const FutureAttr& attr, F&& write_fn);
template<typename F>
rusty::Result<rusty::Unit, i32> clientconn_request_async(const ClientConnection& self, i32 rpc_id, F&& write_fn, AsyncReplyCallback on_reply);
bool operator==(const rusty::Arc<ClientConnection>& lhs, const rusty::Arc<ClientConnection>& rhs);

// @safe - Client-side socket handler exposed to poll loop via Pollable
// proxy facade.  Methods that genuinely cross socket I/O, Marshal byte
// chains, fiber dispatch, cross-thread queues, or raw pointer ops carry
// per-method `// @unsafe` overrides; the rest inherit `@safe` from this
// class umbrella.
// Uses SpinMutex for thread-safe interior mutability, Arc for shared ownership.
#if RUSTYCPP_RUST
struct ClientConnection {
    poll_thread_worker_: Arc<PollThread>,
    fiber_channel_: SpinMutex<Option<Box<FiberChannel>>>,
    direct_channel_: SpinMutex<Option<ChannelConnectionProxy>>,
    channel_mode_: Cell<bool>,
    factory_: SpinMutex<Option<ChannelFactoryProxy>>,
    xid_counter_: Counter,
    pending_fu_: SpinMutex<HashMap<i64, Arc<Future>>>,
    pending_cb_slots_: SpinMutex<Vec<Option<AsyncReplyCallback>>>,
    state_machine_: ConnectionStateMachine,
    reconnect_policy_: Cell<ReconnectPolicy>,
    reconnect_: ReconnectState,
    reconnect_address_: Cell<std::string>,
    buffering_config_: Cell<BufferingConfig>,
    pending_queue_: RequestQueue,
    server_instance_id_: Cell<u64>,
    on_server_restart_: RefCell<OnServerRestartCallbackFn>,
    keepalive_config_: Cell<KeepaliveConfig>,
    heartbeat_manager_: HeartbeatManager,
    circuit_breaker_: CircuitBreaker,
    callback_manager_: Arc<CallbackManager>,
    last_activity_time_: Cell<u64>,
    metrics_: ConnectionMetrics,
    weak_self_: WeakClientConnection,
    host_: std::string,
    packets_: u64,
    paused_: Cell<bool>,
    is_client_mode_: bool,
}

impl Drop for ClientConnection {
    fn drop(&mut self) { clientconn_drop(self); }
}

impl ClientConnection {
    #[cpp_ctor]
    fn new(poll_thread_worker: Arc<PollThread>) -> ClientConnection {
        ClientConnection {
            poll_thread_worker_: poll_thread_worker,
            fiber_channel_: SpinMutex::<Option<Box<FiberChannel>>>::new(Option::<Box<FiberChannel>>(None)),
            direct_channel_: SpinMutex::<Option<ChannelConnectionProxy>>::new(Option::<ChannelConnectionProxy>(None)),
            channel_mode_: Cell::<bool>::new(false),
            factory_: SpinMutex::<Option<ChannelFactoryProxy>>::new(Option::<ChannelFactoryProxy>(None)),
            xid_counter_: Counter::new(0i64),
            pending_fu_: SpinMutex::<HashMap<i64, Arc<Future>>>::new(HashMap::<i64, Arc<Future>>::new()),
            pending_cb_slots_: SpinMutex::<Vec<Option<AsyncReplyCallback>>>::new(make_prefilled_cb_slots()),
            state_machine_: ConnectionStateMachine::new(),
            reconnect_policy_: Cell::<ReconnectPolicy>::new(ReconnectPolicy {}),
            reconnect_: ReconnectState {},
            reconnect_address_: Cell::<std::string>::new(std::string {}),
            buffering_config_: Cell::<BufferingConfig>::new(BufferingConfig::defaults()),
            pending_queue_: make_pending_queue(BufferingConfig::defaults().to_queue_config()),
            server_instance_id_: Cell::<u64>::new(0u64),
            on_server_restart_: RefCell::<OnServerRestartCallbackFn>::new(OnServerRestartCallbackFn {}),
            keepalive_config_: Cell::<KeepaliveConfig>::new(KeepaliveConfig {}),
            heartbeat_manager_: HeartbeatManager::new(HeartbeatConfig::disabled()),
            circuit_breaker_: CircuitBreaker::new(CircuitBreakerConfig::disabled()),
            callback_manager_: Arc::<CallbackManager>::new(CallbackManager::new()),
            last_activity_time_: Cell::<u64>::new(0u64),
            metrics_: ConnectionMetrics::new(),
            weak_self_: WeakClientConnection {},
            host_: std::string {},
            packets_: 0u64,
            paused_: Cell::<bool>::new(false),
            is_client_mode_: false,
        }
    }

    // --- delegating methods (&mut self → non-const free fns) ---
    // recv-loop cluster: &self over interior-mutable state, so it is callable
    // directly through a shared Arc<ClientConnection> (no const_cast at the
    // fiber/job/channel-callback spawn sites).
    fn run_recv_loop(&self) { clientconn_run_recv_loop(self); }
    fn decode_response_and_notify(&self, bytes: *const u8, size: usize) { clientconn_decode_response_and_notify(self, bytes, size); }
    fn on_channel_closed_fan_out(&self) { clientconn_on_channel_closed_fan_out(self); }
    // connect/bind cluster: &self over interior-mutable state (channels are
    // SpinMutex, reconnect_address_ is Cell), so reachable through a shared Arc.
    fn connect_via_factory(&self, addr: *const i8) -> i32 { clientconn_connect_via_factory(self, addr) }
    fn reset_channel_mode_for_reconnect(&self) { clientconn_reset_channel_mode_for_reconnect(self); }
    fn connect(&self, addr: *const i8) -> i32 { clientconn_connect(self, addr) }
    fn bind_channel(&self, channel: ChannelConnectionProxy) { clientconn_bind_channel(self, channel); }
    fn bind_channel_via_poll_thread(&self, channel: ChannelConnectionProxy) { clientconn_bind_channel_via_poll_thread(self, channel); }
    fn bind_channel_direct(&self, channel: ChannelConnectionProxy) { clientconn_bind_channel_direct(self, channel); }
    fn bind_factory(&mut self, factory: ChannelFactoryProxy) { clientconn_bind_factory(self, factory); }
    fn abort_reconnect(&mut self) { unsafe { self.reconnect_.reconnect_abort_.store(true, std::memory_order_release); } }
    fn set_callback_manager(&mut self, callback_manager: &Arc<CallbackManager>) {
        if callback_manager.is_valid() {
            self.callback_manager_ = callback_manager.clone();
        }
    }

    // --- delegating methods (&self → const free fns) ---
    fn invalidate_pending_futures(&self) { clientconn_invalidate_pending_futures(self); }
    fn fail_pending_future(&self, xid: i64, err: i32) { clientconn_fail_pending_future(self, xid, err); }
    fn close(&self) { clientconn_close(self); }
    fn mark_closing(&self) { clientconn_mark_closing(self); }
    fn reconnect(&self, on_complete: OnReconnectCompleteCallbackFn) -> i32 { clientconn_reconnect(self, on_complete) }
    fn set_buffering_config(&self, config: &BufferingConfig) { clientconn_set_buffering_config(self, config); }
    fn set_heartbeat_config(&self, config: &HeartbeatConfig) { clientconn_set_heartbeat_config(self, config); }
    fn heartbeat_config(&self) -> HeartbeatConfig { self.heartbeat_manager_.config() }
    fn set_circuit_breaker_config(&self, config: &CircuitBreakerConfig) { clientconn_set_circuit_breaker_config(self, config); }
    fn circuit_breaker_config(&self) -> CircuitBreakerConfig { self.circuit_breaker_.config() }
    fn enqueue_heartbeat_probe(&self) { clientconn_enqueue_heartbeat_probe(self); }
    fn allow_request_with_circuit_metrics(&self) -> bool {
        let before = self.circuit_breaker_.state();
        let allowed = self.circuit_breaker_.allow_request();
        let after = self.circuit_breaker_.state();
        self.record_circuit_state_transition(before, after);
        if !allowed {
            self.metrics_.record_circuit_open_rejection();
        }
        allowed
    }
    fn record_circuit_state_transition(&self, before: CircuitState, after: CircuitState) { clientconn_record_circuit_state_transition(self, before, after); }
    fn record_circuit_result(&self, err: i32) { clientconn_record_circuit_result(self, err); }
    fn invoke_error_callback(&self, err: i32, message: &std::string) { clientconn_invoke_error_callback(self, err, message); }
    fn invoke_disconnected_callback(&self) { clientconn_invoke_disconnected_callback(self); }
    fn invoke_reconnecting_callback(&self) { clientconn_invoke_reconnecting_callback(self); }
    fn invoke_reconnected_callback(&self, success: bool) { clientconn_invoke_reconnected_callback(self, success); }
    fn invoke_connected_callback(&self) { clientconn_invoke_connected_callback(self); }
    fn dispatch_frame_via_channel(&self, body_bytes: *const u8, body_size: usize) -> ChannelError { clientconn_dispatch_frame_via_channel(self, body_bytes, body_size) }
    fn handle_error(&self) { clientconn_handle_error(self); }
    fn check_pending_write_update(&self) -> bool { clientconn_check_pending_write_update(self) }
    fn handle_free(&self, xid: i64) { clientconn_handle_free(self, xid); }
    fn is_factory_bound(&self) -> bool { (*self.factory_.lock().unwrap()).is_some() }
    fn channel_reconnect_attempts_count(&self) -> u64 { unsafe { self.reconnect_.channel_reconnect_attempts_.load(std::memory_order_acquire) } }
    fn set_reconnect_policy(&self, policy: &ReconnectPolicy) { clientconn_set_reconnect_policy(self, policy); }
    fn is_reconnecting(&self) -> bool { unsafe { self.reconnect_.reconnecting_.load(std::memory_order_acquire) } }
    fn pending_future_count(&self) -> usize { self.pending_fu_.lock().unwrap().len() }
    fn replay_pending_requests_for_test(&self) -> usize { self.replay_pending_requests() }
    fn update_pending_queue_config_for_test(&self, config: &RequestQueueConfig) { self.pending_queue_.update_config(config); }
    fn set_on_server_restart(&self, callback: OnServerRestartCallbackFn) { clientconn_set_on_server_restart(self, callback); }
    fn check_server_instance(&self, new_id: u64) -> bool { clientconn_check_server_instance(self, new_id) }
    fn set_keepalive(&self, config: &KeepaliveConfig) { self.keepalive_config_.set(config); }
    fn on_request_dispatched(&self, bytes: usize) { clientconn_on_request_dispatched(self, bytes); }
    fn on_response_received(&self, bytes: usize) { clientconn_on_response_received(self, bytes); }
    fn host(&self) -> std::string { self.host_ }

    // --- static delegators ---
    fn should_trip_circuit_for_error(err: i32) -> bool { clientconn_should_trip_circuit_for_error(err) }
    fn map_system_error(err: i32) -> RpcError { clientconn_map_system_error(err) }

    // --- generic request trio ---
    fn request<F>(&self, rpc_id: i32, attr: &FutureAttr, write_fn: F) -> FutureResult { clientconn_request_via_channel(self, rpc_id, attr, write_fn) }
    fn request_with_options<F>(&self, rpc_id: i32, options: &RequestOptions, attr: &FutureAttr, write_fn: F) -> FutureResult { clientconn_request_with_options(self, rpc_id, options, attr, write_fn) }
    fn request_async<F>(&self, rpc_id: i32, write_fn: F, on_reply: AsyncReplyCallback) -> rusty::Result<rusty::Unit, i32> { clientconn_request_async(self, rpc_id, write_fn, on_reply) }

    // --- trivial inline accessors ---
    fn is_channel_mode(&self) -> bool { self.channel_mode_.get() }
    fn install_self_weak_for_testing(&mut self, weak: WeakClientConnection) { self.weak_self_ = weak; }
    fn force_connected_for_testing(&mut self) { self.state_machine_.force_state(ConnectionState::CONNECTED); }
    fn set_reconnect_address_for_testing(&self, addr: std::string) { self.reconnect_address_.set(addr); }
    fn connected(&self) -> bool { self.state_machine_.is_connected() }
    fn connection_state(&self) -> ConnectionState { self.state_machine_.state() }
    fn reconnect_policy(&self) -> ReconnectPolicy { self.reconnect_policy_.get() }
    fn buffering_config(&self) -> BufferingConfig { self.buffering_config_.get() }
    fn pending_request_count(&self) -> usize { self.pending_queue_.size() }
    fn clear_pending_requests(&self, error_code: i32) { self.pending_queue_.clear_all(error_code); }
    fn server_instance_id(&self) -> u64 { self.server_instance_id_.get() }
    fn keepalive_config(&self) -> KeepaliveConfig { self.keepalive_config_.get() }
    fn circuit_breaker_state(&self) -> CircuitState { self.circuit_breaker_.state() }
    fn update_last_activity(&self, current_time_ms: u64) { self.last_activity_time_.set(current_time_ms); }
    fn last_activity_time(&self) -> u64 { self.last_activity_time_.get() }
    fn is_idle(&self, idle_ms: u64, current_time_ms: u64) -> bool {
        let last: u64 = self.last_activity_time_.get();
        if last == 0u64 { return false; }
        (current_time_ms - last) > idle_ms
    }
    fn validate_connection(&self) -> bool { self.state_machine_.is_connected() }
    fn metrics(&self) -> &ConnectionMetrics { &self.metrics_ }
    fn replay_pending_requests(&self) -> usize { 0usize }
    fn apply_keepalive_options(&mut self) {}
    fn fd(&self) -> i32 { -1 }
    fn pause(&self) { self.paused_.set(true); }
    fn resume(&self) { self.paused_.set(false); }
    fn poll_mode(&self) -> i32 { PollMode::READ }
    fn content_size(&self) -> usize { 0usize }
    fn handle_write(&mut self) -> i32 { PollMode::NO_CHANGE }
    fn handle_read(&mut self) -> bool { false }
    fn is_closed(&self) -> bool { self.state_machine_.is_terminal() }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.8 version=1 rust_sha256=24717ae48b1fbb576f48f547a936df860afb8e8bd028a8bf0852cc5b3517ecfe*/
struct ClientConnection;

struct ClientConnection {
    rusty::Arc<PollThread> poll_thread_worker_;
    SpinMutex<rusty::Option<rusty::Box<FiberChannel>>> fiber_channel_;
    SpinMutex<rusty::Option<ChannelConnectionProxy>> direct_channel_;
    rusty::Cell<bool> channel_mode_;
    SpinMutex<rusty::Option<ChannelFactoryProxy>> factory_;
    Counter xid_counter_;
    SpinMutex<rusty::HashMap<int64_t, rusty::Arc<Future>>> pending_fu_;
    SpinMutex<rusty::Vec<rusty::Option<AsyncReplyCallback>>> pending_cb_slots_;
    ConnectionStateMachine state_machine_;
    rusty::Cell<ReconnectPolicy> reconnect_policy_;
    ReconnectState reconnect_;
    rusty::Cell<std::string> reconnect_address_;
    rusty::Cell<BufferingConfig> buffering_config_;
    RequestQueue pending_queue_;
    rusty::Cell<uint64_t> server_instance_id_;
    rusty::RefCell<OnServerRestartCallbackFn> on_server_restart_;
    rusty::Cell<KeepaliveConfig> keepalive_config_;
    HeartbeatManager heartbeat_manager_;
    CircuitBreaker circuit_breaker_;
    rusty::Arc<CallbackManager> callback_manager_;
    rusty::Cell<uint64_t> last_activity_time_;
    ConnectionMetrics metrics_;
    WeakClientConnection weak_self_;
    std::string host_;
    uint64_t packets_;
    rusty::Cell<bool> paused_;
    bool is_client_mode_;
    mutable bool _rusty_forgotten = false;
    ClientConnection(rusty::Arc<PollThread> poll_thread_worker__init, SpinMutex<rusty::Option<rusty::Box<FiberChannel>>> fiber_channel__init, SpinMutex<rusty::Option<ChannelConnectionProxy>> direct_channel__init, rusty::Cell<bool> channel_mode__init, SpinMutex<rusty::Option<ChannelFactoryProxy>> factory__init, Counter xid_counter__init, SpinMutex<rusty::HashMap<int64_t, rusty::Arc<Future>>> pending_fu__init, SpinMutex<rusty::Vec<rusty::Option<AsyncReplyCallback>>> pending_cb_slots__init, ConnectionStateMachine state_machine__init, rusty::Cell<ReconnectPolicy> reconnect_policy__init, ReconnectState reconnect__init, rusty::Cell<std::string> reconnect_address__init, rusty::Cell<BufferingConfig> buffering_config__init, RequestQueue pending_queue__init, rusty::Cell<uint64_t> server_instance_id__init, rusty::RefCell<OnServerRestartCallbackFn> on_server_restart__init, rusty::Cell<KeepaliveConfig> keepalive_config__init, HeartbeatManager heartbeat_manager__init, CircuitBreaker circuit_breaker__init, rusty::Arc<CallbackManager> callback_manager__init, rusty::Cell<uint64_t> last_activity_time__init, ConnectionMetrics metrics__init, WeakClientConnection weak_self__init, std::string host__init, uint64_t packets__init, rusty::Cell<bool> paused__init, bool is_client_mode__init) : poll_thread_worker_(std::move(poll_thread_worker__init)), fiber_channel_(std::move(fiber_channel__init)), direct_channel_(std::move(direct_channel__init)), channel_mode_(std::move(channel_mode__init)), factory_(std::move(factory__init)), xid_counter_(std::move(xid_counter__init)), pending_fu_(std::move(pending_fu__init)), pending_cb_slots_(std::move(pending_cb_slots__init)), state_machine_(std::move(state_machine__init)), reconnect_policy_(std::move(reconnect_policy__init)), reconnect_(std::move(reconnect__init)), reconnect_address_(std::move(reconnect_address__init)), buffering_config_(std::move(buffering_config__init)), pending_queue_(std::move(pending_queue__init)), server_instance_id_(std::move(server_instance_id__init)), on_server_restart_(std::move(on_server_restart__init)), keepalive_config_(std::move(keepalive_config__init)), heartbeat_manager_(std::move(heartbeat_manager__init)), circuit_breaker_(std::move(circuit_breaker__init)), callback_manager_(std::move(callback_manager__init)), last_activity_time_(std::move(last_activity_time__init)), metrics_(std::move(metrics__init)), weak_self_(std::move(weak_self__init)), host_(std::move(host__init)), packets_(std::move(packets__init)), paused_(std::move(paused__init)), is_client_mode_(std::move(is_client_mode__init)) {}
    ClientConnection(const ClientConnection&) = delete;
    ClientConnection(ClientConnection&& other) noexcept : poll_thread_worker_(std::move(other.poll_thread_worker_)), fiber_channel_(std::move(other.fiber_channel_)), direct_channel_(std::move(other.direct_channel_)), channel_mode_(std::move(other.channel_mode_)), factory_(std::move(other.factory_)), xid_counter_(std::move(other.xid_counter_)), pending_fu_(std::move(other.pending_fu_)), pending_cb_slots_(std::move(other.pending_cb_slots_)), state_machine_(std::move(other.state_machine_)), reconnect_policy_(std::move(other.reconnect_policy_)), reconnect_(std::move(other.reconnect_)), reconnect_address_(std::move(other.reconnect_address_)), buffering_config_(std::move(other.buffering_config_)), pending_queue_(std::move(other.pending_queue_)), server_instance_id_(std::move(other.server_instance_id_)), on_server_restart_(std::move(other.on_server_restart_)), keepalive_config_(std::move(other.keepalive_config_)), heartbeat_manager_(std::move(other.heartbeat_manager_)), circuit_breaker_(std::move(other.circuit_breaker_)), callback_manager_(std::move(other.callback_manager_)), last_activity_time_(std::move(other.last_activity_time_)), metrics_(std::move(other.metrics_)), weak_self_(std::move(other.weak_self_)), host_(std::move(other.host_)), packets_(std::move(other.packets_)), paused_(std::move(other.paused_)), is_client_mode_(std::move(other.is_client_mode_)) {
        this->_rusty_forgotten = other._rusty_forgotten;
        other._rusty_forgotten = true;
    }
    ClientConnection& operator=(const ClientConnection&) = delete;
    ClientConnection& operator=(ClientConnection&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        this->~ClientConnection();
        new (this) ClientConnection(std::move(other));
        return *this;
    }
    void rusty_mark_forgotten() const noexcept { _rusty_forgotten = true; }


    ~ClientConnection() noexcept(false);
    ClientConnection(rusty::Arc<PollThread> poll_thread_worker);
    void run_recv_loop() const;
    void decode_response_and_notify(const uint8_t* bytes, size_t size) const;
    void on_channel_closed_fan_out() const;
    int32_t connect_via_factory(const int8_t* addr) const;
    void reset_channel_mode_for_reconnect() const;
    int32_t connect(const int8_t* addr) const;
    void bind_channel(ChannelConnectionProxy channel) const;
    void bind_channel_via_poll_thread(ChannelConnectionProxy channel) const;
    void bind_channel_direct(ChannelConnectionProxy channel) const;
    void bind_factory(ChannelFactoryProxy factory);
    void abort_reconnect();
    void set_callback_manager(const rusty::Arc<CallbackManager>& callback_manager);
    void invalidate_pending_futures() const;
    void fail_pending_future(int64_t xid, int32_t err) const;
    void close() const;
    void mark_closing() const;
    int32_t reconnect(OnReconnectCompleteCallbackFn on_complete) const;
    void set_buffering_config(const BufferingConfig& config) const;
    void set_heartbeat_config(const HeartbeatConfig& config) const;
    HeartbeatConfig heartbeat_config() const;
    void set_circuit_breaker_config(const CircuitBreakerConfig& config) const;
    CircuitBreakerConfig circuit_breaker_config() const;
    void enqueue_heartbeat_probe() const;
    bool allow_request_with_circuit_metrics() const;
    void record_circuit_state_transition(CircuitState before, CircuitState after) const;
    void record_circuit_result(int32_t err) const;
    void invoke_error_callback(int32_t err, const std::string& message) const;
    void invoke_disconnected_callback() const;
    void invoke_reconnecting_callback() const;
    void invoke_reconnected_callback(bool success) const;
    void invoke_connected_callback() const;
    ChannelError dispatch_frame_via_channel(const uint8_t* body_bytes, size_t body_size) const;
    void handle_error() const;
    bool check_pending_write_update() const;
    void handle_free(int64_t xid) const;
    bool is_factory_bound() const;
    uint64_t channel_reconnect_attempts_count() const;
    void set_reconnect_policy(const ReconnectPolicy& policy) const;
    bool is_reconnecting() const;
    size_t pending_future_count() const;
    size_t replay_pending_requests_for_test() const;
    void update_pending_queue_config_for_test(const RequestQueueConfig& config) const;
    void set_on_server_restart(OnServerRestartCallbackFn callback) const;
    bool check_server_instance(uint64_t new_id) const;
    void set_keepalive(const KeepaliveConfig& config) const;
    void on_request_dispatched(size_t bytes) const;
    void on_response_received(size_t bytes) const;
    std::string host() const;
    static bool should_trip_circuit_for_error(int32_t err);
    static RpcError map_system_error(int32_t err);
    template<typename F>
    FutureResult request(int32_t rpc_id, const FutureAttr& attr, F write_fn) const;
    template<typename F>
    FutureResult request_with_options(int32_t rpc_id, const RequestOptions& options, const FutureAttr& attr, F write_fn) const;
    template<typename F>
    auto request_async(int32_t rpc_id, F write_fn, AsyncReplyCallback on_reply) const -> rusty::Result<rusty::Unit, int32_t>;
    bool is_channel_mode() const;
    void install_self_weak_for_testing(WeakClientConnection weak);
    void force_connected_for_testing();
    void set_reconnect_address_for_testing(std::string addr) const;
    bool connected() const;
    ConnectionState connection_state() const;
    ReconnectPolicy reconnect_policy() const;
    BufferingConfig buffering_config() const;
    size_t pending_request_count() const;
    void clear_pending_requests(int32_t error_code) const;
    uint64_t server_instance_id() const;
    KeepaliveConfig keepalive_config() const;
    CircuitState circuit_breaker_state() const;
    void update_last_activity(uint64_t current_time_ms) const;
    uint64_t last_activity_time() const;
    bool is_idle(uint64_t idle_ms, uint64_t current_time_ms) const;
    bool validate_connection() const;
    const ConnectionMetrics& metrics() const;
    size_t replay_pending_requests() const;
    void apply_keepalive_options();
    int32_t fd() const;
    void pause() const;
    void resume() const;
    int32_t poll_mode() const;
    size_t content_size() const;
    int32_t handle_write();
    bool handle_read();
    bool is_closed() const;
};


ClientConnection::~ClientConnection() noexcept(false) {
    if (_rusty_forgotten) { return; }
    clientconn_drop((*this));
}

ClientConnection::ClientConnection(rusty::Arc<PollThread> poll_thread_worker)
    : poll_thread_worker_(std::move(poll_thread_worker))
    , fiber_channel_(SpinMutex<rusty::Option<rusty::Box<FiberChannel>>>::new_(rusty::Option<rusty::Box<FiberChannel>>(rusty::None)))
    , direct_channel_(SpinMutex<rusty::Option<ChannelConnectionProxy>>::new_(rusty::Option<ChannelConnectionProxy>(rusty::None)))
    , channel_mode_(rusty::Cell<bool>::new_(false))
    , factory_(SpinMutex<rusty::Option<ChannelFactoryProxy>>::new_(rusty::Option<ChannelFactoryProxy>(rusty::None)))
    , xid_counter_(Counter::new_(static_cast<int64_t>(0)))
    , pending_fu_(SpinMutex<rusty::HashMap<int64_t, rusty::Arc<Future>>>::new_(rusty::HashMap<int64_t, rusty::Arc<Future>>()))
    , pending_cb_slots_(SpinMutex<rusty::Vec<rusty::Option<AsyncReplyCallback>>>::new_(make_prefilled_cb_slots()))
    , state_machine_(ConnectionStateMachine::new_())
    , reconnect_policy_(rusty::Cell<ReconnectPolicy>::new_(ReconnectPolicy{}))
    , reconnect_(ReconnectState{})
    , reconnect_address_(rusty::Cell<std::string>::new_(std::string{}))
    , buffering_config_(rusty::Cell<BufferingConfig>::new_(BufferingConfig::defaults()))
    , pending_queue_(make_pending_queue(BufferingConfig::defaults().to_queue_config()))
    , server_instance_id_(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , on_server_restart_(rusty::RefCell<OnServerRestartCallbackFn>::new_(OnServerRestartCallbackFn{}))
    , keepalive_config_(rusty::Cell<KeepaliveConfig>::new_(KeepaliveConfig{}))
    , heartbeat_manager_(HeartbeatManager::new_(HeartbeatConfig::disabled()))
    , circuit_breaker_(CircuitBreaker::new_(CircuitBreakerConfig::disabled()))
    , callback_manager_(rusty::Arc<CallbackManager>::new_(CallbackManager::new_()))
    , last_activity_time_(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , metrics_(ConnectionMetrics::new_())
    , weak_self_(WeakClientConnection{})
    , host_(std::string{})
    , packets_(static_cast<uint64_t>(0))
    , paused_(rusty::Cell<bool>::new_(false))
    , is_client_mode_(false)
{}

void ClientConnection::run_recv_loop() const {
    clientconn_run_recv_loop((*this));
}

void ClientConnection::decode_response_and_notify(const uint8_t* bytes, size_t size) const {
    clientconn_decode_response_and_notify((*this), bytes, std::move(size));
}

void ClientConnection::on_channel_closed_fan_out() const {
    clientconn_on_channel_closed_fan_out((*this));
}

int32_t ClientConnection::connect_via_factory(const int8_t* addr) const {
    return clientconn_connect_via_factory((*this), addr);
}

void ClientConnection::reset_channel_mode_for_reconnect() const {
    clientconn_reset_channel_mode_for_reconnect((*this));
}

int32_t ClientConnection::connect(const int8_t* addr) const {
    return clientconn_connect((*this), addr);
}

void ClientConnection::bind_channel(ChannelConnectionProxy channel) const {
    clientconn_bind_channel((*this), std::move(channel));
}

void ClientConnection::bind_channel_via_poll_thread(ChannelConnectionProxy channel) const {
    clientconn_bind_channel_via_poll_thread((*this), std::move(channel));
}

void ClientConnection::bind_channel_direct(ChannelConnectionProxy channel) const {
    clientconn_bind_channel_direct((*this), std::move(channel));
}

void ClientConnection::bind_factory(ChannelFactoryProxy factory) {
    clientconn_bind_factory((*this), std::move(factory));
}

void ClientConnection::abort_reconnect() {
    // @unsafe
    {
        this->reconnect_.reconnect_abort_.store(true, std::memory_order_release);
    }
}

void ClientConnection::set_callback_manager(const rusty::Arc<CallbackManager>& callback_manager) {
    if (callback_manager.is_valid()) {
        this->callback_manager_ = rusty::clone(callback_manager);
    }
}

void ClientConnection::invalidate_pending_futures() const {
    clientconn_invalidate_pending_futures((*this));
}

void ClientConnection::fail_pending_future(int64_t xid, int32_t err) const {
    clientconn_fail_pending_future((*this), std::move(xid), std::move(err));
}

void ClientConnection::close() const {
    clientconn_close((*this));
}

void ClientConnection::mark_closing() const {
    clientconn_mark_closing((*this));
}

int32_t ClientConnection::reconnect(OnReconnectCompleteCallbackFn on_complete) const {
    return clientconn_reconnect((*this), std::move(on_complete));
}

void ClientConnection::set_buffering_config(const BufferingConfig& config) const {
    clientconn_set_buffering_config((*this), config);
}

void ClientConnection::set_heartbeat_config(const HeartbeatConfig& config) const {
    clientconn_set_heartbeat_config((*this), config);
}

HeartbeatConfig ClientConnection::heartbeat_config() const {
    return this->heartbeat_manager_.config();
}

void ClientConnection::set_circuit_breaker_config(const CircuitBreakerConfig& config) const {
    clientconn_set_circuit_breaker_config((*this), config);
}

CircuitBreakerConfig ClientConnection::circuit_breaker_config() const {
    return this->circuit_breaker_.config();
}

void ClientConnection::enqueue_heartbeat_probe() const {
    clientconn_enqueue_heartbeat_probe((*this));
}

bool ClientConnection::allow_request_with_circuit_metrics() const {
    auto before = this->circuit_breaker_.state();
    auto allowed = this->circuit_breaker_.allow_request();
    auto after = this->circuit_breaker_.state();
    this->record_circuit_state_transition(std::move(before), std::move(after));
    if (!allowed) {
        this->metrics_.record_circuit_open_rejection();
    }
    return std::move(allowed);
}

void ClientConnection::record_circuit_state_transition(CircuitState before, CircuitState after) const {
    clientconn_record_circuit_state_transition((*this), std::move(before), std::move(after));
}

void ClientConnection::record_circuit_result(int32_t err) const {
    clientconn_record_circuit_result((*this), std::move(err));
}

void ClientConnection::invoke_error_callback(int32_t err, const std::string& message) const {
    clientconn_invoke_error_callback((*this), std::move(err), message);
}

void ClientConnection::invoke_disconnected_callback() const {
    clientconn_invoke_disconnected_callback((*this));
}

void ClientConnection::invoke_reconnecting_callback() const {
    clientconn_invoke_reconnecting_callback((*this));
}

void ClientConnection::invoke_reconnected_callback(bool success) const {
    clientconn_invoke_reconnected_callback((*this), std::move(success));
}

void ClientConnection::invoke_connected_callback() const {
    clientconn_invoke_connected_callback((*this));
}

ChannelError ClientConnection::dispatch_frame_via_channel(const uint8_t* body_bytes, size_t body_size) const {
    return clientconn_dispatch_frame_via_channel((*this), body_bytes, std::move(body_size));
}

void ClientConnection::handle_error() const {
    clientconn_handle_error((*this));
}

bool ClientConnection::check_pending_write_update() const {
    return clientconn_check_pending_write_update((*this));
}

void ClientConnection::handle_free(int64_t xid) const {
    clientconn_handle_free((*this), std::move(xid));
}

bool ClientConnection::is_factory_bound() const {
    return ((*this->factory_.lock().unwrap())).is_some();
}

uint64_t ClientConnection::channel_reconnect_attempts_count() const {
    // @unsafe
    {
        return this->reconnect_.channel_reconnect_attempts_.load(std::memory_order_acquire);
    }
}

void ClientConnection::set_reconnect_policy(const ReconnectPolicy& policy) const {
    clientconn_set_reconnect_policy((*this), policy);
}

bool ClientConnection::is_reconnecting() const {
    // @unsafe
    {
        return this->reconnect_.reconnecting_.load(std::memory_order_acquire);
    }
}

size_t ClientConnection::pending_future_count() const {
    return rusty::len(this->pending_fu_.lock().unwrap());
}

size_t ClientConnection::replay_pending_requests_for_test() const {
    return this->replay_pending_requests();
}

void ClientConnection::update_pending_queue_config_for_test(const RequestQueueConfig& config) const {
    this->pending_queue_.update_config(config);
}

void ClientConnection::set_on_server_restart(OnServerRestartCallbackFn callback) const {
    clientconn_set_on_server_restart((*this), std::move(callback));
}

bool ClientConnection::check_server_instance(uint64_t new_id) const {
    return clientconn_check_server_instance((*this), std::move(new_id));
}

void ClientConnection::set_keepalive(const KeepaliveConfig& config) const {
    this->keepalive_config_.set(std::move(config));
}

void ClientConnection::on_request_dispatched(size_t bytes) const {
    clientconn_on_request_dispatched((*this), std::move(bytes));
}

void ClientConnection::on_response_received(size_t bytes) const {
    clientconn_on_response_received((*this), std::move(bytes));
}

std::string ClientConnection::host() const {
    return this->host_;
}

bool ClientConnection::should_trip_circuit_for_error(int32_t err) {
    return clientconn_should_trip_circuit_for_error(std::move(err));
}

RpcError ClientConnection::map_system_error(int32_t err) {
    return clientconn_map_system_error(std::move(err));
}

template<typename F>
FutureResult ClientConnection::request(int32_t rpc_id, const FutureAttr& attr, F write_fn) const {
    return clientconn_request_via_channel((*this), std::move(rpc_id), attr, std::move(write_fn));
}

template<typename F>
FutureResult ClientConnection::request_with_options(int32_t rpc_id, const RequestOptions& options, const FutureAttr& attr, F write_fn) const {
    return clientconn_request_with_options((*this), std::move(rpc_id), options, attr, std::move(write_fn));
}

template<typename F>
auto ClientConnection::request_async(int32_t rpc_id, F write_fn, AsyncReplyCallback on_reply) const -> rusty::Result<rusty::Unit, int32_t> {
    return clientconn_request_async((*this), std::move(rpc_id), std::move(write_fn), std::move(on_reply));
}

bool ClientConnection::is_channel_mode() const {
    return this->channel_mode_.get();
}

void ClientConnection::install_self_weak_for_testing(WeakClientConnection weak) {
    this->weak_self_ = std::move(weak);
}

void ClientConnection::force_connected_for_testing() {
    this->state_machine_.force_state(rusty::clone(rusty::clone(ConnectionState::CONNECTED)));
}

void ClientConnection::set_reconnect_address_for_testing(std::string addr) const {
    this->reconnect_address_.set(std::move(addr));
}

bool ClientConnection::connected() const {
    return this->state_machine_.is_connected();
}

ConnectionState ClientConnection::connection_state() const {
    return this->state_machine_.state();
}

ReconnectPolicy ClientConnection::reconnect_policy() const {
    return this->reconnect_policy_.get();
}

BufferingConfig ClientConnection::buffering_config() const {
    return this->buffering_config_.get();
}

size_t ClientConnection::pending_request_count() const {
    return this->pending_queue_.size();
}

void ClientConnection::clear_pending_requests(int32_t error_code) const {
    this->pending_queue_.clear_all(std::move(error_code));
}

uint64_t ClientConnection::server_instance_id() const {
    return this->server_instance_id_.get();
}

KeepaliveConfig ClientConnection::keepalive_config() const {
    return this->keepalive_config_.get();
}

CircuitState ClientConnection::circuit_breaker_state() const {
    return this->circuit_breaker_.state();
}

void ClientConnection::update_last_activity(uint64_t current_time_ms) const {
    this->last_activity_time_.set(std::move(current_time_ms));
}

uint64_t ClientConnection::last_activity_time() const {
    return this->last_activity_time_.get();
}

bool ClientConnection::is_idle(uint64_t idle_ms, uint64_t current_time_ms) const {
    const uint64_t last = this->last_activity_time_.get();
    if (rusty::detail::deref_if_pointer_like(last) == static_cast<uint64_t>(0)) {
        return false;
    }
    return ((rusty::detail::deref_if_pointer_like(current_time_ms) - rusty::detail::deref_if_pointer_like(last))) > rusty::detail::deref_if_pointer_like(idle_ms);
}

bool ClientConnection::validate_connection() const {
    return this->state_machine_.is_connected();
}

const ConnectionMetrics& ClientConnection::metrics() const {
    return this->metrics_;
}

size_t ClientConnection::replay_pending_requests() const {
    return static_cast<size_t>(0);
}

void ClientConnection::apply_keepalive_options() {
}

int32_t ClientConnection::fd() const {
    return -1;
}

void ClientConnection::pause() const {
    this->paused_.set(true);
}

void ClientConnection::resume() const {
    this->paused_.set(false);
}

int32_t ClientConnection::poll_mode() const {
    return PollMode::READ;
}

size_t ClientConnection::content_size() const {
    return static_cast<size_t>(0);
}

int32_t ClientConnection::handle_write() {
    return PollMode::NO_CHANGE;
}

bool ClientConnection::handle_read() {
    return false;
}

bool ClientConnection::is_closed() const {
    return this->state_machine_.is_terminal();
}
/*RUSTYCPP:GEN-END id=client.8*/

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
using OnConnectedCallbackFn           = rusty::Function<void() const>;
using OnErrorCallbackFn               = rusty::Function<void(RpcError,
                                                             const std::string&) const>;
using OnReconnectedCallbackFn         = rusty::Function<void(bool) const>;

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

    fn request_async<F>(&self, rpc_id: i32, write_fn: F, on_reply: AsyncReplyCallback) -> Result<(), i32> {
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

        let result: i32 = mut_conn.connect(addr);

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
/*RUSTYCPP:GEN-BEGIN id=client.1 version=1 rust_sha256=fe0df5f77fe22ebca4f3cc79429628d8da74d9dffa22ff5c22cf524c66fea5a4*/
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
    auto request_async(int32_t rpc_id, F write_fn, AsyncReplyCallback on_reply) const -> rusty::Result<rusty::Unit, int32_t>;
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
auto Client::request_async(int32_t rpc_id, F write_fn, AsyncReplyCallback on_reply) const -> rusty::Result<rusty::Unit, int32_t> {
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
    int32_t result = mut_conn.connect(addr);
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

// The DSL `struct Future` declares the data + delegating methods; these
// `fut_*` free fns hold the Condvar / std::chrono / callback-dispatch
// bodies. `self` is `const Future&` — all mutation is through interior
// mutability (Cell / RefCell / Mutex / Condvar).

// @unsafe - rusty::Mutex + rusty::Condvar::wait_while.
void fut_wait(const Future& self) {
  // Respect the future's configured timeout so a lost / never-arriving reply
  // can't wedge the caller forever. Mirrors fut_get_error_code: `timeout_` is
  // in microseconds; a value of 0 means "no timeout" (caller explicitly opted
  // out of bounding). Without this, a single dropped reply turns into a
  // permanent hang (see the StressPipelined livelock investigation).
  if (self.timeout_ > 0) {
    double sec = static_cast<double>(self.timeout_) / 1000000.0;
    fut_timed_wait(self, sec);
    return;
  }
  auto guard = self.state_.lock().unwrap();
  // wait_while: waits WHILE condition is TRUE, stops when FALSE
  // We want to wait while NOT ready and NOT timed_out
  guard = self.ready_cond_.wait_while(std::move(guard), [](FutureState& s) {
    return !s.ready && !s.timed_out;
  }).unwrap();
}

// @safe - Mutex::lock + Condvar::wait_timeout_while are @safe; the only
// escape is the `std::chrono::duration<double>` ctor.
void fut_timed_wait(const Future& self, double sec) {
  auto guard = self.state_.lock().unwrap();
  std::chrono::duration<double> duration;
  // @unsafe { std::chrono::duration ctor is not borrow-checked }
  { duration = std::chrono::duration<double>(sec); }
  // wait_timeout_while: waits WHILE condition is TRUE
  // Returns pair<Guard, bool> where bool = true if condition became false
  auto result = self.ready_cond_.wait_timeout_while(
    std::move(guard),
    duration,
    [](FutureState& s) { return !s.ready && !s.timed_out; }
  ).unwrap();
  guard = std::move(result.first);
  bool condition_became_false = result.second;

  // If condition is still true (timed out while still waiting)
  if (!condition_became_false && !guard->ready) {
    guard->timed_out = true;
    self.error_code_.set(ETIMEDOUT);
    self.timeout_type_.set(TimeoutType::RESPONSE_TIMEOUT);
  }
}

// @unsafe - drives timed_wait (std::chrono); pure flow control otherwise.
bool fut_wait_with_options(const Future& self) {
  auto opts = self.get_options();
  if (opts.timeout_ms == 0) {
    self.wait();  // No timeout
    return self.ready();
  }
  double sec = static_cast<double>(opts.timeout_ms) / 1000.0;
  self.timed_wait(sec);
  return self.ready() && !self.timed_out();
}

// @safe - waits, then hands out a RefMut into the reply buffer. The caller
// holds the guard, so the reference can't outlive it.
rusty::RefMut<Marshal> fut_get_reply(const Future& self) {
  self.wait();
  return self.reply_.borrow_mut();
}

// @unsafe - drives timed_wait (std::chrono) on the configured timeout.
i32 fut_get_error_code(const Future& self) {
  if (self.timeout_ > 0) {
    double x = self.timeout_;
    x = x / 1000000;
    // @unsafe
    { self.timed_wait(x); }
  } else {
    self.wait();
  }
  return self.error_code_.get();
}

// @unsafe - Condvar::notify_all + user callback dispatch outside the lock.
void fut_notify_ready(const Future& self, rusty::Arc<Future> self_arc) {
  bool should_callback = false;  // Initialized here
  rusty::Vec<rusty::Function<void()>> completion_callbacks;
  {
    auto guard = self.state_.lock().unwrap();
    if (!guard->timed_out) {
      guard->ready = true;
    }
    should_callback = guard->ready;
    completion_callbacks = std::move(guard->completion_callbacks);
  }  // Guard dropped here, releasing lock before notify

  self.ready_cond_.notify_all();

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
  if (should_callback && self.attr_.callback) {
    auto x = self.attr_.callback;
    x(self_arc);
  }
}

// ============================================================================
// ClientConnection implementation
// ============================================================================

// @safe - Initializes connection (only stores references)
// State machine defaults to NEW state

// @safe - Simple destructor
// @unsafe - ClientConnection drop body, extracted as a free fn so the DSL
// `impl Drop for ClientConnection` (Phase 5) maps to it. Aborts any in-flight
// reconnect (atomics in the carved ReconnectState), then cancels all pending
// futures/callbacks. `reconnect_` is mutable + invalidate_pending_futures is
// const, so a const self suffices.
// ============================================================================
// Phase 5 flip: free fns for trivial methods whose bodies aren't cleanly
// DSL-inline. The all-public DSL struct needs no friend declarations.
// ============================================================================
// @safe - Container support for Arc<ClientConnection> (was an inline friend).
bool operator==(const rusty::Arc<ClientConnection>& lhs, const rusty::Arc<ClientConnection>& rhs) {
  return lhs.get() == rhs.get();
}

// @safe - ctor helper: RequestQueue has a config-taking ctor, not a new_().
inline RequestQueue make_pending_queue(const RequestQueueConfig& c) {
  return RequestQueue(c);
}

// @unsafe - Records the factory under SpinMutex interior mutability.
void clientconn_bind_factory(ClientConnection& self, ChannelFactoryProxy factory) {
  if (!factory) return;
  { auto guard = self.factory_.lock().unwrap(); *guard = rusty::Some(std::move(factory)); }
}
// @safe - reconnect_policy_ is rusty::Cell<ReconnectPolicy>; the &self setter
// writes through Cell::set (interior mutability, borrow-checked, no const_cast).
void clientconn_set_reconnect_policy(const ClientConnection& self, const ReconnectPolicy& policy) {
  self.reconnect_policy_.set(policy);
}
// @safe - on_server_restart_ is rusty::RefCell<Function>; RefCell::replace swaps
// the callback under interior mutability — no const_cast move-assign.
void clientconn_set_on_server_restart(const ClientConnection& self, rusty::Function<void(uint64_t, uint64_t)> callback) {
  self.on_server_restart_.replace(std::move(callback));
}
// @safe - Cell get/set + RefCell borrow_mut; Log_info shim is @safe. The
// Function::operator() is non-const, so it is invoked through a mutable borrow
// (RefCell::borrow_mut is itself const — interior mutability), no const_cast.
bool clientconn_check_server_instance(const ClientConnection& self, uint64_t new_id) {
  uint64_t old_id = self.server_instance_id_.get();
  self.server_instance_id_.set(new_id);
  if (old_id != 0 && old_id != new_id) {
    Log_info("Server restart detected: old_id=%lu new_id=%lu", old_id, new_id);
    auto cb_ref = self.on_server_restart_.borrow_mut();
    if (*cb_ref) {
      (*cb_ref)(old_id, new_id);
    }
    return true;
  }
  return false;
}
// @safe - delegates to rusty::sys::time::clock_monotonic_us.
uint64_t clientconn_monotonic_ms_now() {
  return rusty::sys::time::clock_monotonic_us() / 1000;
}
// @safe - Record one outbound frame's body size + bump the activity clock.
void clientconn_on_request_dispatched(const ClientConnection& self, size_t bytes) {
  self.metrics_.record_bytes_sent(static_cast<uint64_t>(bytes));
  self.update_last_activity(clientconn_monotonic_ms_now());
}
// @safe - Record one inbound frame's body size + bump the activity clock.
void clientconn_on_response_received(const ClientConnection& self, size_t bytes) {
  self.metrics_.record_bytes_received(static_cast<uint64_t>(bytes));
  self.update_last_activity(clientconn_monotonic_ms_now());
}
void clientconn_drop(const ClientConnection& self) {
  self.reconnect_.reconnect_abort_.store(true, std::memory_order_release);
  self.reconnect_.reconnecting_.store(false, std::memory_order_release);
  self.invalidate_pending_futures();
}


// @unsafe - Cancels all pending futures with error, protected by SpinMutex.
// const: every mutation goes through SpinMutex / Counter / Future's
// own const-callable methods.
void clientconn_invalidate_pending_futures(const ClientConnection& self) {
  // Drain the slim async-callback slots first.  Move callbacks out
  // under the lock, then fire them outside the lock with ENOTCONN +
  // null reply view.
  rusty::Vec<AsyncReplyCallback> drained_callbacks;
  {
    auto cb_guard = self.pending_cb_slots_.lock().unwrap();
    for (size_t i = 0; i < cb_guard->len(); ++i) {
      if ((*cb_guard)[i].is_some()) {
        drained_callbacks.push(std::move((*cb_guard)[i].unwrap()));
        (*cb_guard)[i] = rusty::None;
      }
    }
  }
  for (auto& cb: drained_callbacks) {
    self.metrics_.record_request_dropped();
    cb(ENOTCONN, nullptr, 0);
  }

  list<rusty::Arc<Future>> futures;
  auto guard = self.pending_fu_.lock().unwrap();
  // HashMap's STL iterator yields std::tuple<const K&, V&>, not
  // std::pair, so the value is at std::get<1>(it), not it.second.
  for (auto it: *guard) {
    futures.push_back(std::get<1>(it));  // Copy Arc
  }
  guard->clear();  // Clear map (releases its Arc references)
  // Guard dropped here, releasing lock

  for (auto& fu: futures) {
    self.metrics_.record_request_dropped();
    fu->error_code_.set(ENOTCONN);
    fu->notify_ready(fu);  // Pass Arc to self for callback safety
    // Arc auto-released when list destroyed
  }
}


// @safe - HashMap::get returns Option<V&> now; SpinMutex::lock returns
// LockResult; Arc::clone is @safe. Only notify_ready stays @unsafe.
void clientconn_fail_pending_future(const ClientConnection& self, i64 xid, int err) {
  rusty::Option<rusty::Arc<Future>> fu_opt = rusty::None;
  {
    auto pending_guard = self.pending_fu_.lock().unwrap();
    auto fu_ptr = pending_guard->get(xid);
    if (fu_ptr.is_some()) {
      fu_opt = rusty::Some(fu_ptr.unwrap().clone());
      pending_guard->remove(xid);
    }
  }  // Drop lock before notifying callback/future waiters

  if (fu_opt.is_some()) {
    auto fu = fu_opt.unwrap();
    self.metrics_.record_request_dropped();
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
void clientconn_close(const ClientConnection& self) {
  ConnectionState prev_state = self.state_machine_.state();
  const bool was_connected = self.state_machine_.is_connected();
  if (was_connected) {
    // Transition to DISCONNECTING state while preserving normal lifecycle semantics.
    self.state_machine_.transition_to(ConnectionState::DISCONNECTING);
  }

  // Tear down the channel proxy(ies). The channel layer's `close()`
  // is idempotent and thread-safe per the facade contract.
  // @unsafe { SpinMutex::lock + Box::get + proxy method dispatch }
  {
    auto guard = self.direct_channel_.lock().unwrap();
    if (guard->is_some()) {
      auto* conn = guard->as_ref().unwrap().get();
      conn->close();
    }
  }
  // @unsafe { SpinMutex::lock + FiberChannel::close }
  {
    auto guard = self.fiber_channel_.lock().unwrap();
    if (guard->is_some()) {
      auto* fc = const_cast<FiberChannel*>(
          guard->as_ref().unwrap().get());
      fc->close();
    }
  }

  if (was_connected) {
    // Transition to DISCONNECTED state for clean shutdown.
    self.state_machine_.transition_to(ConnectionState::DISCONNECTED);
  } else if (!self.state_machine_.is_terminal()) {
    // If not connected and not already terminal, force to DISCONNECTED.
    self.state_machine_.force_state(ConnectionState::DISCONNECTED);
  }
  self.heartbeat_manager_.reset();
  self.invalidate_pending_futures();

  if (prev_state == ConnectionState::CONNECTED ||
      prev_state == ConnectionState::DISCONNECTING) {
    self.invoke_disconnected_callback();
  }
}


// @safe - StateMachine is @safe; only std::atomic::store and the call
// into still-@unsafe invalidate_pending_futures need an @unsafe wrap.
// const: state_machine_, reconnect_abort_, and invalidate_pending_futures
// are all const-callable.
// @unsafe - mark_closing body (extracted as a free fn for the DSL method to
// delegate to). std::atomic::store + invalidate_pending_futures.
void clientconn_mark_closing(const ClientConnection& self) {
  self.reconnect_.reconnect_abort_.store(true, std::memory_order_release);
  if (self.state_machine_.is_connected()) {
    // Mark as in-progress close, but do not enter terminal state yet.
    // The poll-thread close callback performs the actual fd close and final state transition.
    self.state_machine_.transition_to(ConnectionState::DISCONNECTING);
  }
  self.invalidate_pending_futures();
}


// @safe - SpinMutex::lock + HashMap::remove + Counter::record are all @safe.
void clientconn_handle_free(const ClientConnection& self, i64 xid) {
  auto guard = self.pending_fu_.lock().unwrap();
  if (guard->remove(xid).is_some()) {
    self.metrics_.record_request_dropped();
    // Arc auto-released when removed from map
  }
  // Guard dropped here, releasing lock
}


// @unsafe - Establishes TCP/IPC connection to server
// Contains syscalls, raw pointers, and other unsafe operations
int clientconn_connect(const ClientConnection& self, const int8_t* addr_i8) {
  // DSL emits `const int8_t*` for `*const i8`; the legacy body uses char*.
  const char* addr = reinterpret_cast<const char*>(addr_i8);
  verify(!self.state_machine_.is_connected());

  // Transition to CONNECTING state
  if (!self.state_machine_.transition_to(ConnectionState::CONNECTING)) {
    Log_error("rrr::ClientConnection: cannot connect from state %s",
              connection_state_to_string(self.state_machine_.state()));
    self.invoke_error_callback(EINVAL, "invalid state for connect");
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
  if (!self.is_factory_bound()) {
    Log_error("rrr::ClientConnection::connect: factory not bound. "
              "Channel mode requires a ChannelFactoryProxy installed via "
              "Client::set_channel_factory(...) or auto-installed by "
              "Client::connect (the latter happens unconditionally now).");
    self.state_machine_.transition_to(ConnectionState::FAILED);
    self.invoke_error_callback(EINVAL, "no channel factory bound");
    return EINVAL;
  }
  return self.connect_via_factory(addr_i8);
}


// @unsafe - Attempts to reconnect to the last connected address
int clientconn_reconnect(const ClientConnection& self, rusty::Function<void(bool)> on_complete) {
  // Reset the abort latch before delegating (folded in from the former
  // const `reconnect` facade): the Client::reconnect path needs a stale
  // abort=true from a prior close() cleared, and the close-fan-out spawn
  // path only reaches here with abort already false, so the reset is a
  // no-op there. `reconnect_` is a mutable atomic, so const self suffices.
  self.reconnect_.reconnect_abort_.store(false, std::memory_order_release);
  auto complete_callback = [&](int result) -> int {
    if (on_complete) on_complete(result == 0);
    return result;
  };

  if (self.reconnect_.reconnect_abort_.load(std::memory_order_acquire)) {
    return complete_callback(ECANCELED);
  }

  auto wait_for_inflight_reconnect = [&]() -> int {
    while (self.reconnect_.reconnecting_.load(std::memory_order_acquire)) {
      if (self.reconnect_.reconnect_abort_.load(std::memory_order_acquire)) {
        return ECANCELED;
      }
      if (self.state_machine_.is_connected()) {
        return 0;
      }
      rusty::thread::sleep(std::chrono::milliseconds(5));
    }

    if (self.state_machine_.is_connected()) {
      return 0;
    }
    return INT_MIN;
  };

  if (self.reconnect_.reconnecting_.load(std::memory_order_acquire)) {
    int waited = wait_for_inflight_reconnect();
    if (waited != INT_MIN) {
      return complete_callback(waited);
    }
  }

  // Check if we have an address to reconnect to
  if (self.reconnect_address_.get().empty()) {
    Log_error("rrr::ClientConnection: no address to reconnect to");
    return complete_callback(EINVAL);
  }

  // Can only reconnect from FAILED or DISCONNECTED state
  if (!self.state_machine_.can_connect()) {
    Log_error("rrr::ClientConnection: cannot reconnect from state %s",
              connection_state_to_string(self.state_machine_.state()));
    return complete_callback(EINVAL);
  }

  while (true) {
    bool expected = false;
    if (self.reconnect_.reconnecting_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      break;
    }

    int waited = wait_for_inflight_reconnect();
    if (waited != INT_MIN) {
      return complete_callback(waited);
    }
  }
  self.invoke_reconnecting_callback();

  auto complete_reconnect = [&](bool success, int result) -> int {
    self.reconnect_.reconnecting_.store(false, std::memory_order_release);
    self.invoke_reconnected_callback(success);

    if (success) {
      Log_info("rrr::ClientConnection: reconnected to %s", self.reconnect_address_.get().c_str());

      // Record reconnection in metrics
      self.metrics_.record_reconnect();

      // Sweep the disconnect-buffering queue. Entries that ran past
      // their TTL while the connection was down resolve their
      // futures with `kRequestQueueExpiredError` and bump
      // `queue_dropped_requests`. Non-stale entries remain in the
      // queue for a future replay path.
      self.pending_queue_.expire_stale();
      return complete_callback(0);
    } else {
      if (result == ECANCELED) {
        Log_debug("rrr::ClientConnection: reconnect cancelled for %s",
                  self.reconnect_address_.get().c_str());
      } else {
        Log_error("rrr::ClientConnection: reconnection failed to %s: %d",
                  self.reconnect_address_.get().c_str(), result);
      }
      return complete_callback(result);
    }
  };

  auto reconnect_once = [&]() -> int {
    if (self.reconnect_.reconnect_abort_.load(std::memory_order_acquire)) {
      return ECANCELED;
    }
    // 4g3c2: `socket_ = -1` reset removed. socket_ is unused in
    // channel mode (the channel proxy's TcpConnection owns the fd);
    // the `connect()` call below routes through `connect_via_factory`
    // which produces a fresh proxy + fresh fd internally.
    // @unsafe { const_cast — connect mutates state_machine_; reconnect is a
    // const facade over the non-const connect path }
    return self.connect(reinterpret_cast<const int8_t*>(self.reconnect_address_.get().c_str()));
  };

  if (self.reconnect_.reconnect_abort_.load(std::memory_order_acquire)) {
    return complete_reconnect(false, ECANCELED);
  }

  // Another reconnect attempt can complete between the pre-CAS state check and
  // this thread acquiring reconnect ownership.
  if (self.state_machine_.is_connected()) {
    return complete_reconnect(true, 0);
  }

  if (!self.state_machine_.can_connect()) {
    return complete_reconnect(false, EINVAL);
  }

  // First attempt happens immediately.
  int result = reconnect_once();
  if (result == 0) {
    return complete_reconnect(true, 0);
  }

  // Follow configured backoff/retry policy for subsequent attempts.
  const ReconnectPolicy policy = self.reconnect_policy_.get();
  auto calc = ReconnectCalculator::new_(policy);
  while (calc.should_retry()) {
    if (self.reconnect_.reconnect_abort_.load(std::memory_order_acquire)) {
      return complete_reconnect(false, ECANCELED);
    }

    uint32_t delay_ms = calc.next_delay_ms();
    if (delay_ms > 0) {
      rusty::thread::sleep(std::chrono::milliseconds(delay_ms));
    }

    if (self.reconnect_.reconnect_abort_.load(std::memory_order_acquire)) {
      return complete_reconnect(false, ECANCELED);
    }

    // Another path may have re-established connection while sleeping.
    if (self.state_machine_.is_connected()) {
      return complete_reconnect(true, 0);
    }

    if (!self.state_machine_.can_connect()) {
      return complete_reconnect(false, EINVAL);
    }

    Log_debug("rrr::ClientConnection: reconnect retry #%u to %s",
              calc.retry_count(), self.reconnect_address_.get().c_str());
    result = reconnect_once();
    if (result == 0) {
      return complete_reconnect(true, 0);
    }
  }

  return complete_reconnect(false, result);
}


// @safe - Uses interior mutability (rusty::Cell / const RequestQueue methods)
void clientconn_set_buffering_config(const ClientConnection& self, const BufferingConfig& config) {
  // buffering_config_ is now rusty::Cell<BufferingConfig>; the &self method
  // writes it through Cell::set — interior mutability, borrow-checked, no
  // const_cast. pending_queue_ ops are const (RequestQueue made const for the flip).
  self.buffering_config_.set(config);
  // Clear any pending requests since config changed (queue's mutex is not
  // movable, so we clear rather than recreate).
  if (!self.pending_queue_.empty()) {
    self.pending_queue_.clear_all(ECONNABORTED);
  }
  self.pending_queue_.update_config(config.to_queue_config());
}


// @safe - HeartbeatManager is @safe; Weak copy-assign is now @safe; the
// lambda body only calls @safe methods + Log_warn (a @safe template shim).
// One inner @unsafe block remains for the const_cast.
void clientconn_set_heartbeat_config(const ClientConnection& self, const HeartbeatConfig& config) {
  // HeartbeatManager::set_config/set_on_timeout assign plain fields →
  // non-const; const_cast matches the prior `mutable HeartbeatManager`.
  auto& hb = const_cast<ClientConnection&>(self);
  hb.heartbeat_manager_.set_config(config);
  WeakClientConnection weak_conn = self.weak_self_;
  hb.heartbeat_manager_.set_on_timeout([weak_conn]() {
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


// @safe - CircuitBreaker class is @safe; set_config is @safe.
void clientconn_set_circuit_breaker_config(const ClientConnection& self, const CircuitBreakerConfig& config) {
  // CircuitBreaker::set_config assigns a plain config field → non-const;
  // const_cast matches the prior `mutable CircuitBreaker` access pattern.
  const_cast<ClientConnection&>(self).circuit_breaker_.set_config(config);
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

// @unsafe - Counter::next, Marshal operators, channel proxy dispatch.
// Channel-mode counterpart of request(): marshals [v64 xid][i32 rpc_id]
// [user args] into a contiguous BufferSink and dispatches through the
// bound proxy. Extracted from the inline request_via_channel template
// method; the DSL flip's generic method delegates here.
template<typename F>
FutureResult clientconn_request_via_channel(const ClientConnection& self, i32 rpc_id,
                                            const FutureAttr& attr, F&& write_fn) {
    if (!self.allow_request_with_circuit_metrics()) {
        return FutureResult::Err(EBUSY);
    }
    self.pending_queue_.expire_stale();
    if (!self.state_machine_.is_connected()) {
        const BufferingConfig buffering_cfg = self.buffering_config_.get();
        if (buffering_cfg.enabled &&
            buffering_cfg.behavior == DisconnectBehavior::QUEUE) {
            auto fu = Future::create(self.xid_counter_.next(1), attr);
            auto fu_for_cb = fu;  // Arc clone for the callback.
            auto qr = QueuedRequest::new_();
            qr.xid     = fu->xid_;
            qr.rpc_id  = rpc_id;
            qr.ttl_ms  = buffering_cfg.default_ttl_ms;
            // Capture an explicit self-pointer (== the old [this]); the
            // callback outlives this call but not the connection.
            const ClientConnection* self_ptr = &self;
            qr.callback = rusty::Function<void(int)>(
                [fu_for_cb, self_ptr](int err) mutable {
                    self_ptr->metrics_.record_queue_drop();
                    fu_for_cb->error_code_.set(err);
                    fu_for_cb->notify_ready(fu_for_cb);
                });
            if (self.pending_queue_.enqueue(std::move(qr))) {
                return FutureResult::Ok(std::move(fu));
            }
            return FutureResult::Err(kRequestQueueRejectedError);
        }
        self.record_circuit_result(ENOTCONN);
        return FutureResult::Err(ENOTCONN);
    }
    {
        auto direct_guard = self.direct_channel_.lock().unwrap();
        if (direct_guard->is_some()) {
            auto& proxy = *direct_guard->as_ref().unwrap();
            if (proxy.is_closed()) {
                self.record_circuit_result(ENOTCONN);
                return FutureResult::Err(ENOTCONN);
            }
        } else {
            auto guard = self.fiber_channel_.lock().unwrap();
            if (guard->is_none() ||
                guard->as_ref().unwrap()->is_closed()) {
                self.record_circuit_result(ENOTCONN);
                return FutureResult::Err(ENOTCONN);
            }
        }
    }

    auto fu = Future::create(self.xid_counter_.next(1), attr);
    {
        auto pending_guard = self.pending_fu_.lock().unwrap();
        pending_guard->insert(fu->xid_, fu);
    }

    BufferSink body_sink;
    BinaryWriteArchive ar(&body_sink);
    static_assert(std::is_invocable_v<F&, BinaryWriteArchive&>,
                  "request write_fn must accept BinaryWriteArchive&");
    ar << v64(fu->xid_);
    ar << rpc_id;
    write_fn(ar);

    const ChannelError ch_err =
        self.dispatch_frame_via_channel(body_sink.bytes.data(),
                                        body_sink.bytes.len());
    if (ch_err != ChannelError::None) {
        {
            auto pending_guard = self.pending_fu_.lock().unwrap();
            pending_guard->remove(fu->xid_);
        }
        self.record_circuit_result(EIO);
        return FutureResult::Err(EIO);
    }

    self.metrics_.record_request_sent();
    self.on_request_dispatched(body_sink.bytes.len());
    return FutureResult::Ok(fu);
}

// @unsafe - Slim async-callback request (no Arc<Future>, no HashMap node).
// Extracted from the inline request_async template method.
template<typename F>
rusty::Result<rusty::Unit, i32> clientconn_request_async(
    const ClientConnection& self, i32 rpc_id, F&& write_fn,
    AsyncReplyCallback on_reply) {
    if (!self.allow_request_with_circuit_metrics()) {
        return rusty::Result<rusty::Unit, i32>::Err(EBUSY);
    }
    if (!self.state_machine_.is_connected()) {
        self.record_circuit_result(ENOTCONN);
        return rusty::Result<rusty::Unit, i32>::Err(ENOTCONN);
    }
    {
        auto direct_guard = self.direct_channel_.lock().unwrap();
        if (direct_guard->is_some()) {
            auto& proxy = *direct_guard->as_ref().unwrap();
            if (proxy.is_closed()) {
                self.record_circuit_result(ENOTCONN);
                return rusty::Result<rusty::Unit, i32>::Err(ENOTCONN);
            }
        } else {
            auto guard = self.fiber_channel_.lock().unwrap();
            if (guard->is_none() ||
                guard->as_ref().unwrap()->is_closed()) {
                self.record_circuit_result(ENOTCONN);
                return rusty::Result<rusty::Unit, i32>::Err(ENOTCONN);
            }
        }
    }

    const i64 xid = self.xid_counter_.next(1);
    const size_t slot = static_cast<size_t>(xid) % kAsyncSlotCount;
    {
        auto guard = self.pending_cb_slots_.lock().unwrap();
        if ((*guard)[slot].is_some()) {
            self.record_circuit_result(EBUSY);
            return rusty::Result<rusty::Unit, i32>::Err(EBUSY);
        }
        (*guard)[slot] = rusty::Some(std::move(on_reply));
    }

    BufferSink body_sink;
    BinaryWriteArchive ar(&body_sink);
    static_assert(std::is_invocable_v<F&, BinaryWriteArchive&>,
                  "request_async write_fn must accept BinaryWriteArchive&");
    ar << v64(xid);
    ar << rpc_id;
    write_fn(ar);

    const ChannelError ch_err =
        self.dispatch_frame_via_channel(body_sink.bytes.data(),
                                        body_sink.bytes.len());
    if (ch_err != ChannelError::None) {
        auto guard = self.pending_cb_slots_.lock().unwrap();
        (*guard)[slot] = rusty::None;
        self.record_circuit_result(EIO);
        return rusty::Result<rusty::Unit, i32>::Err(EIO);
    }
    self.metrics_.record_request_sent();
    self.on_request_dispatched(body_sink.bytes.len());
    return rusty::Result<rusty::Unit, i32>::Ok(rusty::Unit{});
}

// @unsafe - Same as request_via_channel, plus serialize-once for safe
// retry replay + an async retry/backoff spawn. Extracted from the inline
// request_with_options(rpc_id, options, attr, write_fn) template method.
template<typename F>
FutureResult clientconn_request_with_options(const ClientConnection& self, i32 rpc_id,
                                             const RequestOptions& options,
                                             const FutureAttr& attr, F&& write_fn) {
    // Serialize args once so retries can replay identical payload safely.
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
    auto final_fu = Future::create(self.xid_counter_.next(1), attr);
    RequestOptions waiter_options = effective_options;
    waiter_options.timeout_ms = 0;  // Internal attempts own timeout behavior.
    final_fu->set_options(waiter_options);

    auto weak_conn = self.weak_self_;
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

// @unsafe - Dispatch one frame body through the bound channel proxy.
//
// 4g1c: direct-channel binding takes precedence over the FiberChannel
// binding (only one is bound at a time per ClientConnection
// lifecycle). SpinMutex::lock, Option::as_mut, Box deref.
ChannelError clientconn_dispatch_frame_via_channel(const ClientConnection& self,
                                                   const std::uint8_t* body_bytes,
                                                   std::size_t body_size) {
  if (!self.channel_mode_.get()) return ChannelError::ConnectionReset;
  {
    auto guard = self.direct_channel_.lock().unwrap();
    if (guard->is_some()) {
      auto& mut_proxy = *guard->as_mut().unwrap();
      return mut_proxy.send_frame(ChannelFrame{body_bytes, body_size});
    }
  }
  auto guard = self.fiber_channel_.lock().unwrap();
  if (guard->is_none()) return ChannelError::ConnectionReset;
  return guard->as_mut().unwrap()->send_frame(
      ChannelFrame{body_bytes, body_size});
}

// @unsafe - Enqueue one internal heartbeat probe through the bound
// channel proxy.
//
// 4g3c3: legacy fd path removed. Channel mode is the only path; the
// `out_` Marshal that backed the fd path is gone. Callers (the
// poll-loop tick) only fire heartbeats on connected clients, which
// always have a bound channel by construction.
void clientconn_enqueue_heartbeat_probe(const ClientConnection& self) {
  // Build the heartbeat frame body and dispatch through the channel proxy.
  Marshal body;
  body << v64(self.xid_counter_.next(1));
  body << static_cast<i32>(kInternalHeartbeatRpcId);
  const std::size_t body_size = body.content_size();
  std::vector<std::uint8_t> body_bytes;
  if (body_size > 0) {
    body_bytes.resize(body_size);
    verify(body.read(body_bytes.data(), body_size) == body_size);
  }
  // Send-side errors are ignored here (same as the legacy fd path).
  (void)self.dispatch_frame_via_channel(body_bytes.data(), body_size);
}


// @unsafe - Reset channel-mode state for a factory-driven reconnect
//. Drops the closed FiberChannel,
// flips `channel_mode_` off, and forces the state machine to
// DISCONNECTED so `connect()`'s `verify(!is_connected())` passes.
// Caller: the spawn body inside `on_channel_closed_fan_out` when a
// factory is bound.
void clientconn_reset_channel_mode_for_reconnect(const ClientConnection& self) {
  // SpinMutex::lock + Option::take are both @safe.
  {
    auto guard = self.fiber_channel_.lock().unwrap();
    *guard = rusty::None;
  }
  // 4g1c: also drop the direct-channel slot so reconnect can rebind
  // a fresh proxy with fresh callbacks.
  {
    auto guard = self.direct_channel_.lock().unwrap();
    *guard = rusty::None;
  }
  self.channel_mode_.set(false);
  self.state_machine_.force_state(ConnectionState::DISCONNECTED);
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
int clientconn_connect_via_factory(const ClientConnection& self, const int8_t* addr_i8) {
  // DSL emits `const int8_t*` for `*const i8`; the legacy body uses char*.
  const char* addr = reinterpret_cast<const char*>(addr_i8);
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
    auto guard = self.factory_.lock().unwrap();
    if (guard->is_none()) {
      Log_error(
          "rrr::ClientConnection::connect_via_factory: factory unbound at "
          "the moment of connect (race against bind_factory)");
      self.state_machine_.transition_to(ConnectionState::FAILED);
      self.invoke_error_callback(ENOTCONN, "factory unbound");
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
      self.state_machine_.transition_to(ConnectionState::FAILED);
      // Map the channel error onto an errno-shaped value the legacy
      // call sites expect.
      const int rc = (result.error == ChannelError::ConnectionRefused)
                       ? ECONNREFUSED
                     : (result.error == ChannelError::AddressInvalid)
                       ? EINVAL
                       : ENOTCONN;
      self.invoke_error_callback(rc, err_str);
      return rc;
    }
    // Sub-leaf 4g1c: bypass FiberChannel + recv-loop fiber entirely.
    // Install on_frame/on_closed callbacks directly on the channel
    // proxy. on_frame runs on the poll thread (where the channel
    // layer fires it) and calls decode_response_and_notify inline —
    // no IntEvent, no fiber yield, no waiting_events_ churn. This
    // works around the deeper reactor/fiber wedge documented in 4g1b.
    self.bind_channel_direct(result.connection.unwrap());
  }

  // Record address for the close fan-out's reconnect spawn — it
  // re-runs the factory connect with the same target. std::string
  // assignment from a const char* is benign in @safe code.
  self.reconnect_address_.set(addr);

  // Mirror the fd path's terminal transition: the channel layer's
  // own state (proxy.is_closed()) becomes the source of truth, but
  // we still drive the legacy state machine through CONNECTED so
  // existing health-check / metric APIs (`connected()`,
  // `connection_state()`) keep working.
  if (!self.state_machine_.transition_to(ConnectionState::CONNECTED)) {
    self.state_machine_.force_state(ConnectionState::CONNECTED);
  }
  // Record connect timestamp so `metrics_.connect_time_ms()` is
  // non-zero from the moment a request can be issued. The metric
  // tests assert `> 0`; the absolute value (steady-clock-relative)
  // is informational.
  {
    uint64_t now = current_time_ms();
    self.metrics_.record_connect(now);
    // Seed `last_activity_time_` so `is_idle()` measures time since
    // connect (or since the most recent send/recv) rather than
    // returning false forever because no I/O has happened yet.
    self.update_last_activity(now);
  }
  self.invoke_connected_callback();
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
void clientconn_bind_channel(const ClientConnection& self, ChannelConnectionProxy channel) {
  if (!channel) return;

  // Move the proxy into a heap-allocated `FiberChannel` so the
  // recv-loop fiber can hold a stable pointer to the wrapper across
  // its parking lifetime. `FiberChannel` is move-deleted (its
  // callbacks capture `this`), so we use `make_box` which constructs
  // in-place via perfect-forwarded `new` rather than moving.
  // rusty::make_box + SpinMutex::lock + Option::operator= are all @safe.
  {
    auto guard = self.fiber_channel_.lock().unwrap();
    *guard = rusty::Some(rusty::make_box<FiberChannel>(std::move(channel)));
    // The FiberChannel ctor only inits fields; bind_callbacks wires
    // the [this]-capturing on_frame/on_closed/on_error lambdas onto
    // the owned channel proxy. Must run after the Box-allocated
    // FiberChannel is in its final memory location (so `this` is
    // pinned).
    guard->as_ref().unwrap()->bind_callbacks();
  }
  self.channel_mode_.set(true);

  // Capture a Weak<> so the parked fiber doesn't extend the
  // connection's lifetime (which would create a cycle via
  // `fiber_channel_` ownership).
  WeakClientConnection weak_self = self.weak_self_;

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
    conn->run_recv_loop();
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
void clientconn_bind_channel_via_poll_thread(
    const ClientConnection& self, ChannelConnectionProxy channel) {
  if (!channel) return;

  // Move the proxy into the heap-allocated FiberChannel and flip
  // the latch on the calling thread — these are pure data
  // mutations and the recv-loop fiber doesn't observe them until
  // after we submit the OneTimeJob below.
  // rusty::make_box + SpinMutex::lock + Option::operator= are all @safe.
  {
    auto guard = self.fiber_channel_.lock().unwrap();
    *guard = rusty::Some(rusty::make_box<FiberChannel>(std::move(channel)));
    // Wire up the on_frame/on_closed/on_error lambdas on the just-
    // installed FiberChannel (see comment in the make_box site above
    // — bind_callbacks() runs after the Box address is final).
    guard->as_ref().unwrap()->bind_callbacks();
  }
  self.channel_mode_.set(true);

  WeakClientConnection weak_self = self.weak_self_;

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
    conn->run_recv_loop();
  }));
  // Upcast Arc<OneTimeJob> -> Arc<Job> for the PollThread queue.
  auto recv_job_base = rusty::Arc<Job>(recv_job);
  self.poll_thread_worker_->add(std::move(recv_job_base));
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
void clientconn_bind_channel_direct(const ClientConnection& self, ChannelConnectionProxy channel) {
  if (!channel) return;

  // Capture a weak ref so the proxy's installed callbacks don't
  // extend the ClientConnection's lifetime (avoids a refcount cycle
  // through `direct_channel_` + the callbacks).
  WeakClientConnection weak_self = self.weak_self_;

  // Install callbacks BEFORE moving the proxy into the slot. After
  // the move, the proxy lives in `direct_channel_`; the lambdas
  // capture only the weak self-ref.
  // @unsafe { lambda capture, channel proxy mutator }
  channel->set_on_frame([weak_self](const ChannelFrame& f) {
    auto conn_opt = weak_self.upgrade();
    if (conn_opt.is_none()) return;
    auto conn = conn_opt.unwrap();
    conn->decode_response_and_notify(f.payload, f.size);
  });
  channel->set_on_closed([weak_self](ChannelError /*reason*/) {
    auto conn_opt = weak_self.upgrade();
    if (conn_opt.is_none()) return;
    auto conn = conn_opt.unwrap();
    conn->on_channel_closed_fan_out();
  });
  // on_error is not surfaced to the RPC layer in this binding mode
  // (the channel-layer contract follows fatal errors with on_closed,
  // so on_channel_closed_fan_out covers the recovery path).
  channel->set_on_error([](ChannelError, std::string_view) {});

  // Move the proxy into the slot and flip the channel-mode latch.
  // SpinMutex::lock + Option::operator= are both @safe.
  {
    auto guard = self.direct_channel_.lock().unwrap();
    *guard = rusty::Some(std::move(channel));
  }
  self.channel_mode_.set(true);
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
void clientconn_run_recv_loop(const ClientConnection& self) {
  FiberChannel* fc = nullptr;
  {
    auto guard = self.fiber_channel_.lock().unwrap();
    if (guard->is_none()) return;
    // @unsafe { Box::get returns raw pointer }
    fc = const_cast<FiberChannel*>(guard->as_ref().unwrap().get());
  }
  while (true) {
    rusty::Option<OwnedFrame> frame_opt = fc->recv_frame();
    if (frame_opt.is_none()) {
      // Channel closed. Run the close-side fan-out (sub-leaf 4d):
      // cancel pending futures with ENOTCONN, fire error / disconnected
      // callbacks, and trigger auto-reconnect if the policy allows. The
      // fiber then exits, dropping its Arc<ClientConnection> capture.
      self.on_channel_closed_fan_out();
      return;
    }
    auto frame = std::move(frame_opt).unwrap();
    self.decode_response_and_notify(frame.bytes.data(), frame.bytes.size());
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
void clientconn_decode_response_and_notify(const ClientConnection& self, const std::uint8_t* bytes,
                                                  std::size_t size) {
  // Account for every inbound frame body byte and bump the activity
  // clock so `metrics_.bytes_received()` and `is_idle()` reflect real
  // I/O regardless of which dispatch slot the reply maps onto.
  self.on_response_received(size);
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
  self.check_server_instance(static_cast<uint64_t>(v_server_instance_id.get()));

  size_t parsed_header_size = src.pos();
  size_t response_payload_bytes = size - parsed_header_size;
  self.heartbeat_manager_.on_pong_received();

  // Fast path: slim async-callback slot (request_async users).
  // Check first — for callback-only callers this is the dominant
  // pattern and we can avoid touching the HashMap entirely.
  {
    const size_t slot = static_cast<size_t>(v_reply_xid.get())
                          % kAsyncSlotCount;
    rusty::Option<AsyncReplyCallback> cb_opt = rusty::None;
    {
      auto guard = self.pending_cb_slots_.lock().unwrap();
      if ((*guard)[slot].is_some()) {
        cb_opt = std::move((*guard)[slot]);
        (*guard)[slot] = rusty::None;
      }
    }
    if (cb_opt.is_some()) {
      auto cb = std::move(cb_opt.unwrap());
      const i32 err_code = static_cast<i32>(v_error_code.get());
      if (err_code == 0) {
        self.metrics_.record_request_completed();
      } else {
        self.metrics_.record_request_failed();
      }
      self.record_circuit_result(err_code);
      cb(err_code, bytes + parsed_header_size, response_payload_bytes);
      return;
    }
  }

  rusty::Option<rusty::Arc<Future>> fu_opt = rusty::None;
  {
    auto guard = self.pending_fu_.lock().unwrap();
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
      self.metrics_.record_request_completed();
    } else {
      self.metrics_.record_request_failed();
    }
    self.record_circuit_result(v_error_code.get());

    fu->notify_ready(fu);
  }
  // No matching future (timed out or replaced) → drop the payload.
  // The legacy fd path drained the bytes through a throwaway Marshal
  // to keep its chunk list balanced, but that was an idiom of the
  // legacy fd reader; with channel-mode framing the input bytes are
  // owned by the caller and freed on return — nothing to drain.
}

// @unsafe - one-line delegator to the friend free fn (extraction
// step toward the DSL flip).

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
void clientconn_on_channel_closed_fan_out(const ClientConnection& self) {
  ConnectionState prev_state = self.state_machine_.state();
  const bool user_initiated_closing =
      prev_state == ConnectionState::DISCONNECTING ||
      prev_state == ConnectionState::DISCONNECTED ||
      self.reconnect_.reconnect_abort_.load(std::memory_order_acquire);

  if (!user_initiated_closing) {
    self.invoke_error_callback(ECONNRESET, "channel closed");
    self.state_machine_.force_state(ConnectionState::FAILED);
  }

  self.heartbeat_manager_.reset();
  self.invalidate_pending_futures();

  if (!user_initiated_closing) {
    self.invoke_disconnected_callback();
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
  if (self.reconnect_policy_.get().auto_reconnect &&
      // std::string::empty() is a pure const accessor, safe in @safe code.
      !self.reconnect_address_.get().empty()) {
    self.reconnect_.channel_reconnect_attempts_.fetch_add(1, std::memory_order_acq_rel);

    if (self.reconnect_.reconnect_abort_.load(std::memory_order_acquire)) {
      // Caller requested no reconnect (typically: connection
      // tearing down). Counter is still bumped for observability.
      return;
    }
    auto weak_conn = self.weak_self_;
    rusty::thread::spawn([weak_conn]() {
      auto conn_opt = weak_conn.upgrade();
      if (conn_opt.is_none()) {
        return;
      }
      auto conn = conn_opt.unwrap();
      if (!conn->reconnect_policy_.get().auto_reconnect ||
          conn->reconnect_.reconnect_abort_.load(std::memory_order_acquire)) {
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
        if (conn.get() == nullptr) {
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
          conn->reset_channel_mode_for_reconnect();
          // `connect` reads `reconnect_address_` itself (set by
          // the original connect call), so we just call it.
          (void)conn->connect(reinterpret_cast<const int8_t*>(conn->reconnect_address_.get().c_str()));
          return;
        }
        Log_info(
            "rrr::ClientConnection: channel-mode auto-reconnect (legacy) "
            "triggered after on_closed");
        // @unsafe - reconnect mutates socket/state and performs network I/O.
        conn->reconnect(nullptr);
      }
    }).detach();
  }
}

// @safe - Checks whether an error should contribute to circuit tripping.
bool clientconn_should_trip_circuit_for_error(i32 err) {
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
void clientconn_record_circuit_state_transition(const ClientConnection& self,
    CircuitState before,
    CircuitState after) {
  if (before == after) {
    return;
  }

  switch (after) {
    case CircuitState::OPEN:
      self.metrics_.record_circuit_open_transition();
      break;
    case CircuitState::HALF_OPEN:
      self.metrics_.record_circuit_half_open_transition();
      break;
    case CircuitState::CLOSED:
      self.metrics_.record_circuit_closed_transition();
      break;
    default:
      break;
  }
}


// @safe - CircuitBreaker is @safe; should_trip_circuit_for_error is @safe;
// record_circuit_state_transition is @safe.
void clientconn_record_circuit_result(const ClientConnection& self, i32 err) {
  CircuitState before = self.circuit_breaker_.state();
  if (err == 0) {
    self.circuit_breaker_.record_success();
  } else if (self.should_trip_circuit_for_error(err)) {
    self.circuit_breaker_.record_failure();
  }
  CircuitState after = self.circuit_breaker_.state();
  self.record_circuit_state_transition(before, after);
}


// @safe - Maps errno-style errors into structured RpcError categories.
RpcError clientconn_map_system_error(i32 err) {
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
void clientconn_invoke_error_callback(const ClientConnection& self, i32 err, const std::string& message) {
  if (!self.callback_manager_) {
    return;
  }
  self.callback_manager_->invoke_on_error(self.map_system_error(err), message);
}

// @safe - CallbackManager is @safe; Arc::operator bool is @safe.
void clientconn_invoke_disconnected_callback(const ClientConnection& self) {
  if (!self.callback_manager_) {
    return;
  }
  self.callback_manager_->invoke_on_disconnected();
}

// @safe - CallbackManager is @safe; Arc::operator bool is @safe.
void clientconn_invoke_reconnecting_callback(const ClientConnection& self) {
  if (!self.callback_manager_) {
    return;
  }
  self.callback_manager_->invoke_on_reconnecting();
}

// @safe - CallbackManager is @safe; Arc::operator bool is @safe.
void clientconn_invoke_reconnected_callback(const ClientConnection& self, bool success) {
  if (!self.callback_manager_) {
    return;
  }
  self.callback_manager_->invoke_on_reconnected(success);
}

// @safe - CallbackManager is @safe; Arc::operator bool is @safe.
void clientconn_invoke_connected_callback(const ClientConnection& self) {
  if (!self.callback_manager_) {
    return;
  }
  self.callback_manager_->invoke_on_connected();
}

// @unsafe - Error handler - transitions to FAILED state.
// const: state_machine_, atomics (mutable), close/invoke_*_callback,
// and the reconnect spawn are all callable through a const ref.
void clientconn_handle_error(const ClientConnection& self) {
  ConnectionState prev_state = self.state_machine_.state();
  const bool user_initiated_closing =
      prev_state == ConnectionState::DISCONNECTING ||
      prev_state == ConnectionState::DISCONNECTED ||
      self.reconnect_.reconnect_abort_.load(std::memory_order_acquire);

  if (!user_initiated_closing) {
    self.invoke_error_callback(ECONNRESET, "connection error");
    // Force transition to FAILED state (from any state)
    self.state_machine_.force_state(ConnectionState::FAILED);
  }
  // @unsafe - calls close() which does system calls
  { self.close(); }

  if (user_initiated_closing) {
    return;
  }
  self.invoke_disconnected_callback();

  // Trigger policy-driven reconnect automatically after transport failures.
  if (self.reconnect_policy_.get().auto_reconnect &&
      !self.reconnect_.reconnect_abort_.load(std::memory_order_acquire)) {
    // std::string::empty() is a pure const accessor; safe in @safe code.
    if (self.reconnect_address_.get().empty()) {
      return;
    }
    auto weak_conn = self.weak_self_;
    rusty::thread::spawn([weak_conn]() {
        auto conn_opt = weak_conn.upgrade();
        if (conn_opt.is_none()) {
          return;
        }

        auto conn = conn_opt.unwrap();
        if (!conn->reconnect_policy_.get().auto_reconnect ||
            conn->reconnect_.reconnect_abort_.load(std::memory_order_acquire)) {
          return;
        }

        auto state = conn->connection_state();
        if (state == ConnectionState::FAILED || state == ConnectionState::DISCONNECTED) {
          Log_info("rrr::ClientConnection: auto-reconnect triggered after connection failure");
          // @unsafe - reconnect mutates socket/state and performs network I/O.
          if (conn.get() != nullptr) {
            conn->reconnect(nullptr);
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
bool clientconn_check_pending_write_update(const ClientConnection& self) {
  if (self.state_machine_.is_connected() && !self.paused_.get()) {
    if (const_cast<ClientConnection&>(self).heartbeat_manager_.check_timeout()) {
      // Timeout callback already transitioned connection through error handling.
      return false;
    }
    if (self.heartbeat_manager_.should_send_heartbeat()) {
      self.enqueue_heartbeat_probe();
      self.heartbeat_manager_.on_heartbeat_sent();
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

// @safe - Validate connection liveness via state machine alone.
//
// 4g3c3: the legacy `getsockopt(SO_ERROR)` health probe is gone — we
// don't own an fd. The channel layer surfaces transport errors via
// `on_error` / `on_closed`, which the connection's fan-out routes
// into the state machine. So the state-machine check is the
// authoritative liveness signal.

}  // namespace rrr
