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
/*RUSTYCPP:GEN-DISPATCH-BEGIN*/
namespace rusty { namespace detail {
RUSTY_METHOD_DISPATCH(unwrap)
} } // namespace rusty::detail (issue #31 deref_call dispatch)
/*RUSTYCPP:GEN-DISPATCH-END*/

export namespace rrr {

// `ReplyBuffer` — the Future's reply payload, serde-shaped (Marshal-
// deprecation step 2, same design as the server-side Request): `body`
// owns the reply bytes (bulk-filled once by `reply_buffer_fill` when
// the response frame resolves the future), `src` is the read cursor
// over them. The cursor advances in place across `get_reply()` reads,
// exactly as the old reply Marshal's read_pos_ did. INVARIANT: `src`
// borrows `body`'s heap buffer; the buffer is filled at most once and
// the ReplyBuffer never moves after fill (it lives inside the Future's
// RefCell).
#if RUSTYCPP_RUST
struct ReplyBuffer {
    body: Vec<u8>,
    src: BufferSource,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.reply_buffer version=1 rust_sha256=0be1bfd55252f8ae9f86d9849c56febc9efa018454c7dd2f1595e275bcee451d*/
struct ReplyBuffer;

struct ReplyBuffer {
    rusty::Vec<uint8_t> body;
    BufferSource src;
};
/*RUSTYCPP:GEN-END id=client.reply_buffer*/

// Fill the reply body from the wire bytes, then point the read cursor at
// the filled buffer. Call at most once per ReplyBuffer, before any read.
//
// Was a `reserve` + `memcpy` + `set_len` kernel over a raw pointer;
// taking a slice (rule 2) lets extend_from_slice do the same work with
// the length carried by the argument. Both callers already computed a
// sub-range by pointer arithmetic, so they now build that span
// explicitly at the boundary where the arithmetic belongs.
#if RUSTYCPP_RUST
// @safe - value-init factory (empty body, null/0 cursor). The old excuse
// ("the DSL has no spelling for a null-pointer BufferSource literal") is
// expired: core::ptr::null() lowers to rusty::ptr::null() and is already
// used elsewhere in this file's DSL.
fn reply_buffer_empty() -> ReplyBuffer {
    ReplyBuffer {
        body: Vec::<u8>::new(),
        src: BufferSource::new_(core::ptr::null(), 0usize),
    }
}

fn reply_buffer_fill(rb: &mut ReplyBuffer, bytes: &[u8]) {
    rb.body.clear();
    rb.body.extend_from_slice(bytes);
    rb.src = BufferSource::new_(rb.body.data(), rb.body.len());
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.reply_fill version=1 rust_sha256=c96ace134d7963d20872ba5ca944623d7b7da585a9b3236944f0e5957cd19772*/
ReplyBuffer reply_buffer_empty() {
    return ReplyBuffer{.body = rusty::Vec<uint8_t>::new_(), .src = BufferSource::new_(rusty::ptr::null(), static_cast<size_t>(0))};
}

void reply_buffer_fill(ReplyBuffer& rb, std::span<const uint8_t> bytes) {
    ReplyBuffer* rb_shadow1 = &rb;
    (*rb_shadow1).body.clear();
    (*rb_shadow1).body.extend_from_slice(bytes);
    (*rb_shadow1).src = BufferSource::new_((*rb_shadow1).body.data(), rusty::len((*rb_shadow1).body));
}
/*RUSTYCPP:GEN-END id=client.reply_fill*/

// Decode one value from a reply. The cursor lives in ReplyBuffer::src, not in
// the temporary RefMut or BinaryReadArchive, so repeated calls with fresh
// `get_reply()` guards continue from the previous byte position. Keeping this
// helper one-value-at-a-time gives it valid Rust syntax and removes the orphan
// C++ stream operators plus the variadic parameter-pack bridge.
#if RUSTYCPP_RUST
fn deserialize_from<T>(mut src: rusty::RefMut<ReplyBuffer>, value: &mut T) {
    let mut ar = BinaryReadArchive {
        source_: make_source_proxy(&raw mut (*src).src),
    };
    Deserialize_::deserialize(value, &mut ar);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.3 version=1 rust_sha256=65a6933e659e12626046310ba5aa0a14f321445c634728a40c0332cf74bb8dca*/
template<typename T>
void deserialize_from(rusty::RefMut<ReplyBuffer> src, T& value) {
    auto ar = BinaryReadArchive{.source_ = make_source_proxy(&(*src).src)};
    Deserialize_::deserialize(value, ar);
}
/*RUSTYCPP:GEN-END id=client.3*/

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
    return BufferingConfig{.behavior = rusty::clone(rusty::clone(DisconnectBehavior_QUEUE())), .max_pending = static_cast<size_t>(1000), .default_ttl_ms = static_cast<uint32_t>(30000), .overflow = rusty::clone(rusty::clone(OverflowStrategy::DROP_OLDEST)), .enabled = true};
}

BufferingConfig BufferingConfig::defaults() {
    return BufferingConfig::new_();
}

BufferingConfig BufferingConfig::disabled() {
    return BufferingConfig{.behavior = rusty::clone(rusty::clone(DisconnectBehavior_FAIL_FAST())), .max_pending = static_cast<size_t>(1000), .default_ttl_ms = static_cast<uint32_t>(30000), .overflow = rusty::clone(rusty::clone(OverflowStrategy::DROP_OLDEST)), .enabled = false};
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
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
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

// FutureAttr's callback is the same `Option<Arc<Function<...const>>>`-backed
// wrapper used by the channel-tier callback typedefs in channel.hpp:
// default construction is `None`, copies clone the Arc, and callable
// construction is explicit through `from_callable`. Sharing the wrapper
// keeps FutureAttr cheap to propagate through generated rcc_rpc.h stubs.
#if RUSTYCPP_RUST
type FutureCallback = detail::CallbackWrapper<rusty::Function<dyn Fn(rusty::Arc<Future>)>>;
#endif
/*RUSTYCPP:GEN-BEGIN id=client.future_callback version=1 rust_sha256=fe900146cd1293c21b4714a130cdbf3d512adb1246c45be73c1aca6949ae0e40*/
using FutureCallback = detail::CallbackWrapper<rusty::Function<void(rusty::Arc<Future>) const>>;
/*RUSTYCPP:GEN-END id=client.future_callback*/

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

// (fut_secs is gone: rusty::Condvar::wait_timeout_us_while takes a plain
//  microsecond count, so std::chrono never appears in rrr code.)

#if RUSTYCPP_RUST
struct FutureState {
    ready: bool,
    timed_out: bool,
    completion_callbacks: Vec<rusty::Function<dyn FnMut()>>,
}

impl FutureState {
    fn new() -> FutureState {
        FutureState { ready: false, timed_out: false, completion_callbacks: Vec::<rusty::Function<dyn FnMut()>>::new() }
    }
}

struct Future {
    xid_: i64,
    error_code_: Cell<i32>,
    attr_: FutureAttr,
    reply_: RefCell<ReplyBuffer>,
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
            reply_: RefCell::<ReplyBuffer>::new(reply_buffer_empty()),
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
        if self.timeout_ > 0u64 {
            let sec: f64 = (self.timeout_ as f64) / 1000000.0;
            self.timed_wait(sec);
            return;
        }
        let guard = self.state_.lock().unwrap();
        // rusty::Condvar is @safe; wait WHILE not-ready and not-timed-out.
        self.ready_cond_.wait_while(guard, |s| !s.ready && !s.timed_out).unwrap();
    }

    fn timed_wait(&self, sec: f64) {
        let guard = self.state_.lock().unwrap();
        let micros: u64 = (sec * 1000000.0) as u64;
        let mut result = self.ready_cond_.wait_timeout_us_while(guard, micros, |s| !s.ready && !s.timed_out).unwrap();
        let mut guard = result.0;
        let condition_became_false: bool = result.1;
        if !condition_became_false && !(*guard).ready {
            (*guard).timed_out = true;
            self.error_code_.set(ETIMEDOUT);
            self.timeout_type_.set(TimeoutType::RESPONSE_TIMEOUT);
        }
    }

    fn wait_with_options(&self) -> bool {
        let opts = self.get_options();
        if opts.timeout_ms == 0u64 {
            self.wait();
            return self.ready();
        }
        let sec: f64 = (opts.timeout_ms as f64) / 1000.0f64;
        self.timed_wait(sec);
        self.ready() && !self.timed_out()
    }

    fn timed_out(&self) -> bool {
        let guard = self.state_.lock().unwrap();
        guard.timed_out
    }

    fn add_completion_callback(&self, callback: rusty::Function<dyn FnMut()>) -> bool {
        let guard = self.state_.lock().unwrap();
        if guard.ready || guard.timed_out {
            return false;
        }
        guard.completion_callbacks.push(callback);
        true
    }

    fn get_reply(&self) -> rusty::RefMut<ReplyBuffer> {
        self.wait();
        self.reply_.borrow_mut()
    }

    fn get_error_code(&self) -> i32 {
        if self.timeout_ > 0u64 {
            let x: f64 = (self.timeout_ as f64) / 1000000.0f64;
            self.timed_wait(x);
        } else {
            self.wait();
        }
        self.error_code_.get()
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
        let mut should_callback: bool = false;
        let mut completion_callbacks: Vec<rusty::Function<dyn FnMut()>> = Vec::new();
        {
            let guard = self.state_.lock().unwrap();
            if !guard.timed_out {
                guard.ready = true;
            }
            should_callback = guard.ready;
            completion_callbacks = rusty::mem::take(&mut guard.completion_callbacks);
        }
        // Notify waiters after dropping the lock.
        self.ready_cond_.notify_all();
        for callback in &mut completion_callbacks {
            if callback {
                callback();
            }
        }
        if should_callback && self.attr_.callback.has_value() {
            let x = self.attr_.callback.clone();
            x.callable()(self_arc);
        }
    }

    fn safe_release(fu: Arc<Future>) {
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.future version=1 rust_sha256=c458ab2eb0ed58076d4d82faaca96708452887b06787bd81ad02199ef40aa41c*/
struct FutureState;
struct Future;

struct FutureState {
    bool ready;
    bool timed_out;
    rusty::Vec<rusty::Function<void()>> completion_callbacks;

    static FutureState new_();
};

struct Future {
    int64_t xid_;
    rusty::Cell<int32_t> error_code_;
    FutureAttr attr_;
    rusty::RefCell<ReplyBuffer> reply_;
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
    bool add_completion_callback(rusty::Function<void()> callback) const;
    rusty::RefMut<ReplyBuffer> get_reply() const;
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
    return FutureState{.ready = false, .timed_out = false, .completion_callbacks = rusty::Vec<rusty::Function<void()>>::new_()};
}

Future::Future(int64_t xid, FutureAttr attr)
    : xid_(std::move(xid))
    , error_code_(rusty::Cell<int32_t>::new_(static_cast<int32_t>(0)))
    , attr_(std::move(attr))
    , reply_(rusty::RefCell<ReplyBuffer>::new_(reply_buffer_empty()))
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
    if (rusty::detail::deref_if_pointer_like(this->timeout_) > static_cast<uint64_t>(0)) {
        double sec = ((static_cast<double>(this->timeout_))) / 1000000.0;
        this->timed_wait(std::move(sec));
        return;
    }
    auto guard = this->state_.lock().unwrap();
    this->ready_cond_.wait_while(std::move(guard), [&](auto&& s) { return rusty::detail::rust_not([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.ready); }) { return (__r.ready); } else if constexpr (requires { (__r.ready_field); }) { return (__r.ready_field); } else if constexpr (requires { ((*__r).ready); }) { return ((*__r).ready); } else { return ((*__r).ready_field); } }(s)) && rusty::detail::rust_not([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.timed_out); }) { return (__r.timed_out); } else if constexpr (requires { (__r.timed_out_field); }) { return (__r.timed_out_field); } else if constexpr (requires { ((*__r).timed_out); }) { return ((*__r).timed_out); } else { return ((*__r).timed_out_field); } }(s)); }).unwrap();
}

void Future::timed_wait(double sec) const {
    auto guard = this->state_.lock().unwrap();
    const uint64_t micros = rusty::float_to_int_cast<uint64_t>((rusty::detail::deref_if_pointer_like(sec) * 1000000.0));
    auto result = this->ready_cond_.wait_timeout_us_while(std::move(guard), std::move(micros), [&](auto&& s) { return rusty::detail::rust_not([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.ready); }) { return (__r.ready); } else if constexpr (requires { (__r.ready_field); }) { return (__r.ready_field); } else if constexpr (requires { ((*__r).ready); }) { return ((*__r).ready); } else { return ((*__r).ready_field); } }(s)) && rusty::detail::rust_not([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.timed_out); }) { return (__r.timed_out); } else if constexpr (requires { (__r.timed_out_field); }) { return (__r.timed_out_field); } else if constexpr (requires { ((*__r).timed_out); }) { return ((*__r).timed_out); } else { return ((*__r).timed_out_field); } }(s)); }).unwrap();
    auto guard_shadow1 = std::move(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(result)));
    const bool condition_became_false = rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._1; }) return (std::forward<decltype(__t)>(__t)._1); else if constexpr (requires { std::get<1>(std::forward<decltype(__t)>(__t)); }) return std::get<1>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._1; }) return ((*std::forward<decltype(__t)>(__t))._1); else return std::get<1>(*std::forward<decltype(__t)>(__t)); })(result));
    if (!condition_became_false && rusty::detail::rust_not((rusty::detail::deref_if_pointer_like(guard_shadow1)).ready)) {
        (rusty::detail::deref_if_pointer_like(guard_shadow1)).timed_out = true;
        this->error_code_.set(std::move(ETIMEDOUT));
        this->timeout_type_.set(rusty::clone(rusty::clone(TimeoutType::RESPONSE_TIMEOUT)));
    }
}

bool Future::wait_with_options() const {
    const auto opts = this->get_options();
    if (rusty::detail::deref_if_pointer_like(opts.timeout_ms) == static_cast<uint64_t>(0)) {
        this->wait();
        return this->ready();
    }
    double sec = ((static_cast<double>(opts.timeout_ms))) / 1000.0;
    this->timed_wait(std::move(sec));
    return this->ready() && !this->timed_out();
}

bool Future::timed_out() const {
    auto guard = this->state_.lock().unwrap();
    return std::move((*guard).timed_out);
}

bool Future::add_completion_callback(rusty::Function<void()> callback) const {
    auto guard = this->state_.lock().unwrap();
    if (rusty::detail::deref_if_pointer_like((*guard).ready) || rusty::detail::deref_if_pointer_like((*guard).timed_out)) {
        return false;
    }
    (*guard).completion_callbacks.push(std::move(callback));
    return true;
}

rusty::RefMut<ReplyBuffer> Future::get_reply() const {
    this->wait();
    return this->reply_.borrow_mut();
}

int32_t Future::get_error_code() const {
    if (rusty::detail::deref_if_pointer_like(this->timeout_) > static_cast<uint64_t>(0)) {
        double x = ((static_cast<double>(this->timeout_))) / 1000000.0;
        this->timed_wait(std::move(x));
    } else {
        this->wait();
    }
    return this->error_code_.get();
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
    bool should_callback = false;
    rusty::Vec<rusty::Function<void()>> completion_callbacks = rusty::Vec<rusty::Function<void()>>::new_();
    {
        auto guard = this->state_.lock().unwrap();
        if (!(*guard).timed_out) {
            (*guard).ready = true;
        }
        should_callback = std::move((*guard).ready);
        completion_callbacks = rusty::mem::take((*guard).completion_callbacks);
    }
    this->ready_cond_.notify_all();
    for (auto&& callback : rusty::for_in(rusty::iter_mut(completion_callbacks))) {
        if (callback) {
            callback();
        }
    }
    if (rusty::detail::deref_if_pointer_like(should_callback) && this->attr_.callback.has_value()) {
        const auto x = rusty::clone(this->attr_.callback);
        x.callable()(std::move(self_arc));
    }
}

void Future::safe_release(rusty::Arc<Future> fu) {
}
/*RUSTYCPP:GEN-END id=client.future*/

// (The RPC co_await feature is removed: the TypedFutureAwaiter /
//  TypedFutureResultAwaiter pair and their factories are gone, and
//  rpcgen no longer emits `operator co_await` / `await_*` wrappers.
//  Callers use the sync wrappers or async_* + callbacks.)

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
// `ReconnectState` — the auto-reconnect gating flags. Authored as
// inline Rust DSL: rusty::sync::atomic::Atomic<T> has const load/store
// (interior mutability built-in — the old `mutable` dies) AND a
// value-moving move ctor (the old hand-written one dies with it; the
// owning ClientConnection's synthesized move needs exactly that, and
// the connection is never actually moved at runtime — Arc-held).
#if RUSTYCPP_RUST
struct ReconnectState {
    reconnecting_: AtomicBool,
    reconnect_abort_: AtomicBool,
    // auto-reconnect attempt counter — incremented before the
    // reconnect-thread spawn in on_channel_closed_fan_out; tests
    // inspect it to verify the fan-out reached the policy branch.
    channel_reconnect_attempts_: AtomicU64,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.reconnect_state version=1 rust_sha256=132322dc95f8a834bce71a65e8ed16b9c5b9d2f18cb275bfc9382c552942ff01*/
struct ReconnectState;

struct ReconnectState {
    rusty::sync::atomic::AtomicBool reconnecting_;
    rusty::sync::atomic::AtomicBool reconnect_abort_;
    rusty::sync::atomic::AtomicU64 channel_reconnect_attempts_;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};
/*RUSTYCPP:GEN-END id=client.reconnect_state*/

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
// `rusty::Mutex::new(make_prefilled_cb_slots())`.
#if RUSTYCPP_RUST
fn make_prefilled_cb_slots() -> Vec<rusty::Option<AsyncReplyCallback>> {
    let mut slots = Vec::<rusty::Option<AsyncReplyCallback>>::new();
    slots.reserve(kAsyncSlotCount);
    let mut i: usize = 0;
    while i < kAsyncSlotCount {
        slots.push(rusty::None);
        i += 1;
    }
    slots
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.prefilled_slots version=1 rust_sha256=f2c0805e924e4128ef618283f6b299d82ba01abc2b45e8aaa010cd9a0457be60*/
rusty::Vec<rusty::Option<AsyncReplyCallback>> make_prefilled_cb_slots() {
    auto slots = rusty::Vec<rusty::Option<AsyncReplyCallback>>::new_();
    slots.reserve(std::move(kAsyncSlotCount));
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(kAsyncSlotCount)) {
        slots.push(rusty::None);
        i += 1;
    }
    return std::move(slots);
}
/*RUSTYCPP:GEN-END id=client.prefilled_slots*/

// Hoisted above the ClientConnection DSL struct (Phase 5 flip): the DSL
// parser rejects a `rusty::Function<void(...)>` field/param type.
using OnReconnectCompleteCallbackFn = rusty::Function<void(bool)>;
using OnServerRestartCallbackFn = rusty::Function<void(uint64_t, uint64_t)>;

// Forward declarations of the clientconn_* free fns the DSL struct's
// generated out-of-line method defs delegate to (the friend decls that
// previously provided visibility are gone with the class).
struct ClientConnection;
inline RequestQueue make_pending_queue(const RequestQueueConfig& c);
void clientconn_enqueue_heartbeat_probe(const ClientConnection& self);
void clientconn_run_recv_loop(const ClientConnection& self);
void clientconn_decode_response_and_notify(const ClientConnection& self, const std::uint8_t* bytes, std::size_t size);
ChannelError clientconn_dispatch_frame_via_channel(const ClientConnection& self, const std::uint8_t* body_bytes, std::size_t body_size);
int clientconn_reconnect(const ClientConnection& self, rusty::Function<void(bool)> on_complete);
int clientconn_connect_via_factory(const ClientConnection& self, const int8_t* addr);
void clientconn_bind_channel_via_poll_thread(const ClientConnection& self, ChannelConnectionProxy channel);
RpcError clientconn_map_system_error(i32 err);
uint64_t clientconn_monotonic_ms_now();
// (by-value F: matches the DSL generic's lowering — write_fn is
// consumed by the single write it performs)
template<typename F>
FutureResult clientconn_request_via_channel(const ClientConnection& conn, i32 rpc_id, const FutureAttr& attr, F write_fn);
template<typename F>
FutureResult clientconn_request_with_options(const ClientConnection& self, i32 rpc_id, const RequestOptions& options, const FutureAttr& attr, F write_fn);
template<typename F>
rusty::Result<rusty::Unit, i32> clientconn_request_async(const ClientConnection& conn, i32 rpc_id, F write_fn, AsyncReplyCallback on_reply);

// @safe - Client-side socket handler exposed to poll loop via Pollable
// proxy facade.  Methods that genuinely cross socket I/O, Marshal byte
// chains, fiber dispatch, cross-thread queues, or raw pointer ops carry
// per-method `// @unsafe` overrides; the rest inherit `@safe` from this
// class umbrella.
// Uses rusty::Mutex for thread-safe interior mutability, Arc for shared ownership.
#if RUSTYCPP_RUST
struct ClientConnection {
    poll_thread_worker_: Arc<PollThread>,
    fiber_channel_: rusty::Mutex<Option<Box<FiberChannel>>>,
    direct_channel_: rusty::Mutex<Option<ChannelConnectionProxy>>,
    channel_mode_: Cell<bool>,
    factory_: rusty::Mutex<Option<ChannelFactoryProxy>>,
    xid_counter_: Counter,
    pending_fu_: rusty::Mutex<HashMap<i64, Arc<Future>>>,
    pending_cb_slots_: rusty::Mutex<Vec<Option<AsyncReplyCallback>>>,
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
    fn drop(&mut self) {
        unsafe { self.reconnect_.reconnect_abort_.store(true, rusty::sync::atomic::Ordering::Release); }
        unsafe { self.reconnect_.reconnecting_.store(false, rusty::sync::atomic::Ordering::Release); }
        self.invalidate_pending_futures();
    }
}

impl ClientConnection {
    #[cpp_ctor]
    fn new(poll_thread_worker: Arc<PollThread>) -> ClientConnection {
        ClientConnection {
            poll_thread_worker_: poll_thread_worker,
            fiber_channel_: rusty::Mutex::<Option<Box<FiberChannel>>>::new(Option::<Box<FiberChannel>>(None)),
            direct_channel_: rusty::Mutex::<Option<ChannelConnectionProxy>>::new(Option::<ChannelConnectionProxy>(None)),
            channel_mode_: Cell::<bool>::new(false),
            factory_: rusty::Mutex::<Option<ChannelFactoryProxy>>::new(Option::<ChannelFactoryProxy>(None)),
            xid_counter_: Counter::new(0i64),
            pending_fu_: rusty::Mutex::<HashMap<i64, Arc<Future>>>::new(HashMap::<i64, Arc<Future>>::new()),
            pending_cb_slots_: rusty::Mutex::<Vec<Option<AsyncReplyCallback>>>::new(make_prefilled_cb_slots()),
            state_machine_: ConnectionStateMachine::new(),
            reconnect_policy_: Cell::<ReconnectPolicy>::new(ReconnectPolicy::new()),
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
    fn on_channel_closed_fan_out(&self) {
        let prev_state = self.state_machine_.state();
        let abort_flag: bool = unsafe { self.reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire) };
        let user_initiated_closing: bool =
            (prev_state as i32) == (ConnectionState::DISCONNECTING as i32)
            || (prev_state as i32) == (ConnectionState::DISCONNECTED as i32)
            || abort_flag;

        if !user_initiated_closing {
            self.invoke_error_callback(ECONNRESET, "channel closed");
            self.state_machine_.force_state(ConnectionState::FAILED);
        }

        self.heartbeat_manager_.reset();
        self.invalidate_pending_futures();

        if !user_initiated_closing {
            self.invoke_disconnected_callback();
        }

        // Trigger channel-mode auto-reconnect if the policy allows. The
        // counter is bumped the moment the fan-out reaches this branch (the
        // observability signal tests assert), then a spawn does the work
        // unless reconnect was aborted.
        let addr: std::string = self.reconnect_address_.get();
        if self.reconnect_policy_.get().auto_reconnect && !addr.empty() {
            unsafe { self.reconnect_.channel_reconnect_attempts_.fetch_add(1, rusty::sync::atomic::Ordering::AcqRel); }

            let reconnect_aborted: bool = unsafe { self.reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire) };
            if reconnect_aborted {
                return;
            }
            let weak_conn: WeakClientConnection = self.weak_self_.clone();
            rusty::thread::spawn(move || {
                let conn_opt = weak_conn.upgrade();
                if conn_opt.is_none() {
                    return;
                }
                let conn = conn_opt.unwrap();
                let conn_aborted: bool = unsafe { (*conn).reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire) };
                if !(*conn).reconnect_policy_.get().auto_reconnect || conn_aborted {
                    return;
                }
                let state = (*conn).connection_state();
                if (state as i32) == (ConnectionState::FAILED as i32)
                    || (state as i32) == (ConnectionState::DISCONNECTED as i32) {
                    if (*conn).is_factory_bound() {
                        unsafe { log_line(Log::INFO, 0i32, core::ptr::null(), std::format("rrr::ClientConnection: channel-mode auto-reconnect (factory) triggered after on_closed")); }
                        // Reset the channel-mode latch + drop the stale FiberChannel
                        // before re-connecting (connect verifies !is_connected and
                        // bind_channel needs the slot empty). connect reads
                        // reconnect_address_ itself, so we just re-run it.
                        (*conn).reset_channel_mode_for_reconnect();
                        let reconnect_addr: std::string = (*conn).reconnect_address_.get();
                        let _ = (*conn).connect(reconnect_addr.c_str() as *const i8);
                        return;
                    }
                    unsafe { log_line(Log::INFO, 0i32, core::ptr::null(), std::format("rrr::ClientConnection: channel-mode auto-reconnect (legacy) triggered after on_closed")); }
                    (*conn).reconnect(OnReconnectCompleteCallbackFn {});
                }
            }).detach();
        }
    }
    // connect/bind cluster: &self over interior-mutable state (channels are
    // rusty::Mutex, reconnect_address_ is Cell), so reachable through a shared Arc.
    fn connect_via_factory(&self, addr: *const i8) -> i32 { clientconn_connect_via_factory(self, addr) }
    fn reset_channel_mode_for_reconnect(&self) {
        {
            let guard = self.fiber_channel_.lock().unwrap();
            *guard = rusty::None;
        }
        {
            let guard = self.direct_channel_.lock().unwrap();
            *guard = rusty::None;
        }
        self.channel_mode_.set(false);
        self.state_machine_.force_state(ConnectionState::DISCONNECTED);
    }
    fn connect(&self, addr: *const i8) -> i32 {
        verify(!self.state_machine_.is_connected());

        if !self.state_machine_.transition_to(ConnectionState::CONNECTING) {
            unsafe { log_line(Log::ERROR, 0i32, core::ptr::null(), std::format("rrr::ClientConnection: cannot connect from state {}",
                               connection_state_to_string(self.state_machine_.state()))); }
            self.invoke_error_callback(EINVAL, "invalid state for connect");
            return EINVAL;
        }

        // Channel mode is the only path: Client::connect auto-installs a TCP
        // factory before calling this. connect_via_factory issues
        // factory->connect(addr), hands the proxy to bind_channel_direct, and
        // records reconnect_address_ for the close-side reconnect spawn.
        if !self.is_factory_bound() {
            unsafe { log_line(Log::ERROR, 0i32, core::ptr::null(), std::format("rrr::ClientConnection::connect: factory not bound. Channel mode requires a ChannelFactoryProxy installed via Client::set_channel_factory(...) or auto-installed by Client::connect (the latter happens unconditionally now).")); }
            self.state_machine_.transition_to(ConnectionState::FAILED);
            self.invoke_error_callback(EINVAL, "no channel factory bound");
            return EINVAL;
        }
        self.connect_via_factory(addr)
    }
    fn bind_channel(&self, channel: ChannelConnectionProxy) {
        if !channel.is_valid() {
            return;
        }
        // Move the proxy into a heap-allocated FiberChannel (make_box
        // constructs in-place; FiberChannel is move-deleted because its
        // callbacks capture `this`). bind_callbacks must run AFTER the Box is
        // in its final slot so those [this]-captures pin to a stable address.
        {
            let mut guard = self.fiber_channel_.lock().unwrap();
            *guard = rusty::Some(rusty::make_box::<FiberChannel>(channel));
            let fc: &mut Box<FiberChannel> = (*guard).as_mut().unwrap();
            (*fc).bind_callbacks();
        }
        self.channel_mode_.set(true);

        // Capture a Weak so the parked recv-loop fiber doesn't extend the
        // connection lifetime (would cycle through fiber_channel_ ownership).
        let weak_self: WeakClientConnection = self.weak_self_.clone();
        // The recv-loop fiber must live on the reactor that fires the proxy's
        // on_frame/on_closed callbacks (single-threaded IntEvent signaling);
        // the caller picks the thread (see bind_channel_via_poll_thread).
        Fiber::create_run_impl(move || {
            let conn_opt = weak_self.upgrade();
            if conn_opt.is_none() {
                return;
            }
            let conn = conn_opt.unwrap();
            (*conn).run_recv_loop();
        }, __FILE__, __LINE__);
    }
    fn bind_channel_via_poll_thread(&self, channel: ChannelConnectionProxy) { clientconn_bind_channel_via_poll_thread(self, channel); }
    // Direct on_frame / on_closed binding: bypasses FiberChannel and the
    // recv-loop fiber entirely, installing the callbacks on the proxy itself.
    // Both fire on whichever thread the channel layer dispatches from -- for
    // TCP that is the poll thread, the same one whose handle_read parses the
    // frames. send_frame remains callable from any thread (dispatch_frame_via
    // _channel uses it from user threads).
    //
    // Callbacks are installed BEFORE the proxy moves into `direct_channel_`.
    // Once it is in the slot, dropping the slot drops the callbacks, so any
    // in-flight dispatch must complete before the drop -- the same contract
    // the FiberChannel destructor honours.
    fn bind_channel_direct(&self, mut channel: ChannelConnectionProxy) {
        if !channel.is_valid() {
            return;
        }
        // Weak, one clone per closure: a strong capture would cycle through
        // `direct_channel_` + the callbacks and leak the connection.
        let weak_frame: WeakClientConnection = self.weak_self_.clone();
        let weak_closed: WeakClientConnection = self.weak_self_.clone();
        {
            // Concrete `Box<..>`, not the ChannelConnectionProxy alias: through
            // the alias the pointer-like check fails and the calls lower to
            // `channel.set_on_frame(..)` (dot) instead of `->` (docs 7.50).
            let ch: &mut Box<ChannelConnectionBase> = &mut channel;
            ch.set_on_frame(OnFrameCallback::from_callable(move |f: &ChannelFrame| {
                let conn_opt = weak_frame.upgrade();
                if conn_opt.is_none() {
                    return;
                }
                let conn = conn_opt.unwrap();
                (*conn).decode_response_and_notify(f.payload, f.size);
            }));
            ch.set_on_closed(OnClosedCallback::from_callable(move |reason: ChannelError| {
                let conn_opt = weak_closed.upgrade();
                if conn_opt.is_none() {
                    return;
                }
                let conn = conn_opt.unwrap();
                (*conn).on_channel_closed_fan_out();
            }));
            // on_error is not surfaced in this mode: the channel-layer contract
            // follows a fatal error with on_closed, so the fan-out covers it.
            ch.set_on_error(OnErrorCallback::from_callable(move |err: ChannelError, msg: std::string_view| {
            }));
        }
        {
            let mut guard = self.direct_channel_.lock().unwrap();
            *guard = rusty::Some(channel);
        }
        self.channel_mode_.set(true);
    }
    fn bind_factory(&mut self, factory: ChannelFactoryProxy) {
        if !factory.is_valid() {
            return;
        }
        let guard = self.factory_.lock().unwrap();
        *guard = rusty::Some(factory);
    }
    fn abort_reconnect(&mut self) { unsafe { self.reconnect_.reconnect_abort_.store(true, rusty::sync::atomic::Ordering::Release); } }
    fn set_callback_manager(&mut self, callback_manager: &Arc<CallbackManager>) {
        if callback_manager.is_valid() {
            self.callback_manager_ = callback_manager.clone();
        }
    }

    // --- delegating methods (&self → const free fns) ---
    fn invalidate_pending_futures(&self) {
        // Drain the slim async-callback slots first. Take each callback out
        // under the lock via Option::take (mem::take leaves None behind, so
        // the fixed-size slot vector keeps its shape), then fire them outside
        // the lock with ENOTCONN + a null reply view.
        let mut drained_callbacks: Vec<AsyncReplyCallback> = Vec::new();
        {
            let cb_guard = self.pending_cb_slots_.lock().unwrap();
            let mut i: usize = 0;
            while i < cb_guard.len() {
                if cb_guard[i].is_some() {
                    drained_callbacks.push(rusty::mem::take(&mut cb_guard[i]).unwrap());
                }
                i += 1usize;
            }
        }
        for cb in &mut drained_callbacks {
            self.metrics_.record_request_dropped();
            cb(ENOTCONN, core::ptr::null(), 0);
        }

        // Drain the pending-future map in one pass: HashMap::drain() empties
        // the map as it yields each (xid, Arc<Future>) entry, replacing the
        // prior iterate-then-clear. The lock is held through the notify loop
        // below, matching the original (the map is already empty by then).
        let mut futures: Vec<Arc<Future>> = Vec::new();
        let guard = self.pending_fu_.lock().unwrap();
        for (_xid, fu) in guard.drain() {
            futures.push(fu);
        }
        for fu in &futures {
            self.metrics_.record_request_dropped();
            (*fu).error_code_.set(ENOTCONN);
            (*fu).notify_ready(fu.clone());
        }
    }
    fn fail_pending_future(&self, xid: i64, err: i32) {
        let mut fu_opt: Option<Arc<Future>> = None;
        {
            let pending_guard = self.pending_fu_.lock().unwrap();
            let fu_ptr = (*pending_guard).get(xid);
            if fu_ptr.is_some() {
                fu_opt = Some(fu_ptr.unwrap().clone());
                (*pending_guard).remove(xid);
            }
        }
        if fu_opt.is_some() {
            let fu = fu_opt.unwrap();
            self.metrics_.record_request_dropped();
            (*fu).error_code_.set(err);
            (*fu).notify_ready(fu.clone());
        }
    }
    fn close(&self) {
        let prev_state = self.state_machine_.state();
        let was_connected: bool = self.state_machine_.is_connected();
        if was_connected {
            self.state_machine_.transition_to(ConnectionState::DISCONNECTING);
        }

        // Tear down the channel proxy(ies). The channel layer's close() is
        // idempotent + thread-safe per the facade contract. The `&mut` local
        // is load-bearing: deref-through-a-guard-chain drops the deref
        // (docs 7.50), and a `&` binding would lower to `const Box<T>&`
        // while close() is &mut self.
        {
            let mut guard = self.direct_channel_.lock().unwrap();
            if (*guard).is_some() {
                let ch: &mut Box<ChannelConnectionBase> = (*guard).as_mut().unwrap();
                (*ch).close();
            }
        }
        {
            let mut guard = self.fiber_channel_.lock().unwrap();
            if (*guard).is_some() {
                let fc: &mut Box<FiberChannel> = (*guard).as_mut().unwrap();
                (*fc).close();
            }
        }

        if was_connected {
            self.state_machine_.transition_to(ConnectionState::DISCONNECTED);
        } else if !self.state_machine_.is_terminal() {
            self.state_machine_.force_state(ConnectionState::DISCONNECTED);
        }
        self.heartbeat_manager_.reset();
        self.invalidate_pending_futures();

        if (prev_state as i32) == (ConnectionState::CONNECTED as i32)
            || (prev_state as i32) == (ConnectionState::DISCONNECTING as i32) {
            self.invoke_disconnected_callback();
        }
    }
    fn mark_closing(&self) {
        unsafe { self.reconnect_.reconnect_abort_.store(true, rusty::sync::atomic::Ordering::Release); }
        if self.state_machine_.is_connected() {
            self.state_machine_.transition_to(ConnectionState::DISCONNECTING);
        }
        self.invalidate_pending_futures();
    }
    fn reconnect(&self, on_complete: OnReconnectCompleteCallbackFn) -> i32 { clientconn_reconnect(self, on_complete) }
    fn set_buffering_config(&self, config: &BufferingConfig) {
        self.buffering_config_.set(config);
        if !self.pending_queue_.empty() {
            self.pending_queue_.clear_all(ECONNABORTED);
        }
        self.pending_queue_.update_config(config.to_queue_config());
    }
    fn set_heartbeat_config(&self, config: &HeartbeatConfig) {
        self.heartbeat_manager_.set_config(config);
        // Capture a weak self-handle by move so the escaping timeout closure
        // does not keep the connection alive (mirrors the legacy [weak_conn]
        // C++ lambda; a move closure's owned capture is escape-safe).
        let weak_conn: WeakClientConnection = self.weak_self_.clone();
        self.heartbeat_manager_.set_on_timeout(move || {
            let conn_opt = weak_conn.upgrade();
            if conn_opt.is_none() {
                return;
            }
            let conn = conn_opt.unwrap();
            if !(*conn).connected() {
                return;
            }
            unsafe { log_line(Log::WARN, 0i32, core::ptr::null(), std::format("rrr::ClientConnection: heartbeat timeout for {}", (*conn).host().c_str())); }
            (*conn).handle_error();
        });
    }
    fn heartbeat_config(&self) -> HeartbeatConfig { self.heartbeat_manager_.config() }
    fn set_circuit_breaker_config(&self, config: &CircuitBreakerConfig) { self.circuit_breaker_.set_config(config); }
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
    fn record_circuit_state_transition(&self, before: CircuitState, after: CircuitState) {
        if before == after {
            return;
        }
        match after {
            CircuitState::OPEN => self.metrics_.record_circuit_open_transition(),
            CircuitState::HALF_OPEN => self.metrics_.record_circuit_half_open_transition(),
            CircuitState::CLOSED => self.metrics_.record_circuit_closed_transition(),
            _ => {},
        }
    }
    fn record_circuit_result(&self, err: i32) {
        let before = self.circuit_breaker_.state();
        if err == 0i32 {
            self.circuit_breaker_.record_success();
        } else if self.should_trip_circuit_for_error(err) {
            self.circuit_breaker_.record_failure();
        }
        let after = self.circuit_breaker_.state();
        self.record_circuit_state_transition(before, after);
    }
    fn invoke_error_callback(&self, err: i32, message: &std::string) {
        if !self.callback_manager_.is_valid() {
            return;
        }
        (*self.callback_manager_).invoke_on_error(clientconn_map_system_error(err), message);
    }
    fn invoke_disconnected_callback(&self) {
        if !self.callback_manager_.is_valid() {
            return;
        }
        (*self.callback_manager_).invoke_on_disconnected();
    }
    fn invoke_reconnecting_callback(&self) {
        if !self.callback_manager_.is_valid() {
            return;
        }
        (*self.callback_manager_).invoke_on_reconnecting();
    }
    fn invoke_reconnected_callback(&self, success: bool) {
        if !self.callback_manager_.is_valid() {
            return;
        }
        (*self.callback_manager_).invoke_on_reconnected(success);
    }
    fn invoke_connected_callback(&self) {
        if !self.callback_manager_.is_valid() {
            return;
        }
        (*self.callback_manager_).invoke_on_connected();
    }
    fn dispatch_frame_via_channel(&self, body_bytes: *const u8, body_size: usize) -> ChannelError { clientconn_dispatch_frame_via_channel(self, body_bytes, body_size) }
    fn handle_error(&self) {
        let prev_state = self.state_machine_.state();
        let abort_flag: bool = unsafe { self.reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire) };
        let user_initiated_closing: bool =
            (prev_state as i32) == (ConnectionState::DISCONNECTING as i32)
            || (prev_state as i32) == (ConnectionState::DISCONNECTED as i32)
            || abort_flag;

        if !user_initiated_closing {
            self.invoke_error_callback(ECONNRESET, "connection error");
            self.state_machine_.force_state(ConnectionState::FAILED);
        }
        self.close();

        if user_initiated_closing {
            return;
        }
        self.invoke_disconnected_callback();

        // Trigger policy-driven reconnect automatically after transport failures.
        let reconnect_aborted: bool = unsafe { self.reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire) };
        if self.reconnect_policy_.get().auto_reconnect && !reconnect_aborted {
            let addr: std::string = self.reconnect_address_.get();
            if addr.empty() {
                return;
            }
            let weak_conn: WeakClientConnection = self.weak_self_.clone();
            rusty::thread::spawn(move || {
                let conn_opt = weak_conn.upgrade();
                if conn_opt.is_none() {
                    return;
                }
                let conn = conn_opt.unwrap();
                let conn_aborted: bool = unsafe { (*conn).reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire) };
                if !(*conn).reconnect_policy_.get().auto_reconnect || conn_aborted {
                    return;
                }
                let state = (*conn).connection_state();
                if (state as i32) == (ConnectionState::FAILED as i32)
                    || (state as i32) == (ConnectionState::DISCONNECTED as i32) {
                    unsafe { log_line(Log::INFO, 0i32, core::ptr::null(), std::format("rrr::ClientConnection: auto-reconnect triggered after connection failure")); }
                    (*conn).reconnect(OnReconnectCompleteCallbackFn {});
                }
            }).detach();
        }
    }
    fn check_pending_write_update(&self) -> bool {
        if self.state_machine_.is_connected() && !self.paused_.get() {
            if self.heartbeat_manager_.check_timeout() {
                return false;
            }
            if self.heartbeat_manager_.should_send_heartbeat() {
                self.enqueue_heartbeat_probe();
                self.heartbeat_manager_.on_heartbeat_sent();
                return true;
            }
        }
        false
    }
    fn handle_free(&self, xid: i64) {
        let guard = self.pending_fu_.lock().unwrap();
        if guard.remove(xid).is_some() {
            self.metrics_.record_request_dropped();
        }
    }
    fn is_factory_bound(&self) -> bool { (*self.factory_.lock().unwrap()).is_some() }
    fn channel_reconnect_attempts_count(&self) -> u64 { unsafe { self.reconnect_.channel_reconnect_attempts_.load(rusty::sync::atomic::Ordering::Acquire) } }
    fn set_reconnect_policy(&self, policy: &ReconnectPolicy) { self.reconnect_policy_.set(policy); }
    fn is_reconnecting(&self) -> bool { unsafe { self.reconnect_.reconnecting_.load(rusty::sync::atomic::Ordering::Acquire) } }
    fn pending_future_count(&self) -> usize { self.pending_fu_.lock().unwrap().len() }
    fn replay_pending_requests_for_test(&self) -> usize { self.replay_pending_requests() }
    fn update_pending_queue_config_for_test(&self, config: &RequestQueueConfig) { self.pending_queue_.update_config(config); }
    fn set_on_server_restart(&self, callback: OnServerRestartCallbackFn) { self.on_server_restart_.replace(callback); }
    fn check_server_instance(&self, new_id: u64) -> bool {
        let old_id = self.server_instance_id_.get();
        self.server_instance_id_.set(new_id);
        if old_id != 0u64 && old_id != new_id {
            unsafe { log_line(Log::INFO, 0i32, core::ptr::null(), std::format("Server restart detected: old_id={} new_id={}", old_id, new_id)); }
            let cb_ref = self.on_server_restart_.borrow_mut();
            if *cb_ref {
                (*cb_ref)(old_id, new_id);
            }
            return true;
        }
        false
    }
    fn set_keepalive(&self, config: &KeepaliveConfig) { self.keepalive_config_.set(config); }
    fn on_request_dispatched(&self, bytes: usize) {
        self.metrics_.record_bytes_sent(bytes as u64);
        self.update_last_activity(clientconn_monotonic_ms_now());
    }
    fn on_response_received(&self, bytes: usize) {
        self.metrics_.record_bytes_received(bytes as u64);
        self.update_last_activity(clientconn_monotonic_ms_now());
    }
    fn host(&self) -> std::string { self.host_ }

    // --- static delegators ---
    fn should_trip_circuit_for_error(err: i32) -> bool {
        if err == 0i32 {
            return false;
        }
        if err == ENOTCONN || err == ECONNREFUSED || err == ECONNRESET
            || err == ECONNABORTED || err == ETIMEDOUT || err == EHOSTUNREACH
            || err == ENETUNREACH || err == EPIPE {
            return true;
        }
        false
    }
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
    fn handle_write(&self) -> i32 { PollMode::NO_CHANGE }
    fn handle_read(&self) -> bool { false }
    fn is_closed(&self) -> bool { self.state_machine_.is_terminal() }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.8 version=1 rust_sha256=9f3b4a2030e0f4776be648d93e3b2de9087f53182e1edf11ca1f401a987c5fb2*/
struct ClientConnection;

struct ClientConnection {
    rusty::Arc<PollThread> poll_thread_worker_;
    rusty::Mutex<rusty::Option<rusty::Box<FiberChannel>>> fiber_channel_;
    rusty::Mutex<rusty::Option<ChannelConnectionProxy>> direct_channel_;
    rusty::Cell<bool> channel_mode_;
    rusty::Mutex<rusty::Option<ChannelFactoryProxy>> factory_;
    Counter xid_counter_;
    rusty::Mutex<rusty::HashMap<int64_t, rusty::Arc<Future>>> pending_fu_;
    rusty::Mutex<rusty::Vec<rusty::Option<AsyncReplyCallback>>> pending_cb_slots_;
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
    ClientConnection(rusty::Arc<PollThread> poll_thread_worker__init, rusty::Mutex<rusty::Option<rusty::Box<FiberChannel>>> fiber_channel__init, rusty::Mutex<rusty::Option<ChannelConnectionProxy>> direct_channel__init, rusty::Cell<bool> channel_mode__init, rusty::Mutex<rusty::Option<ChannelFactoryProxy>> factory__init, Counter xid_counter__init, rusty::Mutex<rusty::HashMap<int64_t, rusty::Arc<Future>>> pending_fu__init, rusty::Mutex<rusty::Vec<rusty::Option<AsyncReplyCallback>>> pending_cb_slots__init, ConnectionStateMachine state_machine__init, rusty::Cell<ReconnectPolicy> reconnect_policy__init, ReconnectState reconnect__init, rusty::Cell<std::string> reconnect_address__init, rusty::Cell<BufferingConfig> buffering_config__init, RequestQueue pending_queue__init, rusty::Cell<uint64_t> server_instance_id__init, rusty::RefCell<OnServerRestartCallbackFn> on_server_restart__init, rusty::Cell<KeepaliveConfig> keepalive_config__init, HeartbeatManager heartbeat_manager__init, CircuitBreaker circuit_breaker__init, rusty::Arc<CallbackManager> callback_manager__init, rusty::Cell<uint64_t> last_activity_time__init, ConnectionMetrics metrics__init, WeakClientConnection weak_self__init, std::string host__init, uint64_t packets__init, rusty::Cell<bool> paused__init, bool is_client_mode__init) : poll_thread_worker_(std::move(poll_thread_worker__init)), fiber_channel_(std::move(fiber_channel__init)), direct_channel_(std::move(direct_channel__init)), channel_mode_(std::move(channel_mode__init)), factory_(std::move(factory__init)), xid_counter_(std::move(xid_counter__init)), pending_fu_(std::move(pending_fu__init)), pending_cb_slots_(std::move(pending_cb_slots__init)), state_machine_(std::move(state_machine__init)), reconnect_policy_(std::move(reconnect_policy__init)), reconnect_(std::move(reconnect__init)), reconnect_address_(std::move(reconnect_address__init)), buffering_config_(std::move(buffering_config__init)), pending_queue_(std::move(pending_queue__init)), server_instance_id_(std::move(server_instance_id__init)), on_server_restart_(std::move(on_server_restart__init)), keepalive_config_(std::move(keepalive_config__init)), heartbeat_manager_(std::move(heartbeat_manager__init)), circuit_breaker_(std::move(circuit_breaker__init)), callback_manager_(std::move(callback_manager__init)), last_activity_time_(std::move(last_activity_time__init)), metrics_(std::move(metrics__init)), weak_self_(std::move(weak_self__init)), host_(std::move(host__init)), packets_(std::move(packets__init)), paused_(std::move(paused__init)), is_client_mode_(std::move(is_client_mode__init)) {}
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
    void rusty_mark_forgotten() const noexcept { _rusty_forgotten = true; rusty::detail::mark_forgotten_if_supported(this->poll_thread_worker_); rusty::detail::mark_forgotten_if_supported(this->fiber_channel_); rusty::detail::mark_forgotten_if_supported(this->direct_channel_); rusty::detail::mark_forgotten_if_supported(this->channel_mode_); rusty::detail::mark_forgotten_if_supported(this->factory_); rusty::detail::mark_forgotten_if_supported(this->xid_counter_); rusty::detail::mark_forgotten_if_supported(this->pending_fu_); rusty::detail::mark_forgotten_if_supported(this->pending_cb_slots_); rusty::detail::mark_forgotten_if_supported(this->state_machine_); rusty::detail::mark_forgotten_if_supported(this->reconnect_policy_); rusty::detail::mark_forgotten_if_supported(this->reconnect_); rusty::detail::mark_forgotten_if_supported(this->reconnect_address_); rusty::detail::mark_forgotten_if_supported(this->buffering_config_); rusty::detail::mark_forgotten_if_supported(this->pending_queue_); rusty::detail::mark_forgotten_if_supported(this->server_instance_id_); rusty::detail::mark_forgotten_if_supported(this->on_server_restart_); rusty::detail::mark_forgotten_if_supported(this->keepalive_config_); rusty::detail::mark_forgotten_if_supported(this->heartbeat_manager_); rusty::detail::mark_forgotten_if_supported(this->circuit_breaker_); rusty::detail::mark_forgotten_if_supported(this->callback_manager_); rusty::detail::mark_forgotten_if_supported(this->last_activity_time_); rusty::detail::mark_forgotten_if_supported(this->metrics_); rusty::detail::mark_forgotten_if_supported(this->weak_self_); rusty::detail::mark_forgotten_if_supported(this->host_); rusty::detail::mark_forgotten_if_supported(this->packets_); rusty::detail::mark_forgotten_if_supported(this->paused_); rusty::detail::mark_forgotten_if_supported(this->is_client_mode_); }


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
    int32_t handle_write() const;
    bool handle_read() const;
    bool is_closed() const;
};


ClientConnection::~ClientConnection() noexcept(false) {
    if (_rusty_forgotten) { return; }
    // @unsafe
    {
        this->reconnect_.reconnect_abort_.store(true, rusty::sync::atomic::Ordering::Release);
    }
    // @unsafe
    {
        this->reconnect_.reconnecting_.store(false, rusty::sync::atomic::Ordering::Release);
    }
    this->invalidate_pending_futures();
}

ClientConnection::ClientConnection(rusty::Arc<PollThread> poll_thread_worker)
    : poll_thread_worker_(std::move(poll_thread_worker))
    , fiber_channel_(rusty::Mutex<rusty::Option<rusty::Box<FiberChannel>>>::new_(rusty::Option<rusty::Box<FiberChannel>>(rusty::None)))
    , direct_channel_(rusty::Mutex<rusty::Option<ChannelConnectionProxy>>::new_(rusty::Option<ChannelConnectionProxy>(rusty::None)))
    , channel_mode_(rusty::Cell<bool>::new_(false))
    , factory_(rusty::Mutex<rusty::Option<ChannelFactoryProxy>>::new_(rusty::Option<ChannelFactoryProxy>(rusty::None)))
    , xid_counter_(Counter::new_(static_cast<int64_t>(0)))
    , pending_fu_(rusty::Mutex<rusty::HashMap<int64_t, rusty::Arc<Future>>>::new_(rusty::HashMap<int64_t, rusty::Arc<Future>>()))
    , pending_cb_slots_(rusty::Mutex<rusty::Vec<rusty::Option<AsyncReplyCallback>>>::new_(make_prefilled_cb_slots()))
    , state_machine_(ConnectionStateMachine::new_())
    , reconnect_policy_(rusty::Cell<ReconnectPolicy>::new_(ReconnectPolicy::new_()))
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
    const auto prev_state = this->state_machine_.state();
    const bool abort_flag = this->reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire);
    const bool user_initiated_closing = ((((static_cast<int32_t>(prev_state))) == ((static_cast<int32_t>(ConnectionState::DISCONNECTING)))) || (((static_cast<int32_t>(prev_state))) == ((static_cast<int32_t>(ConnectionState::DISCONNECTED))))) || rusty::detail::deref_if_pointer_like(abort_flag);
    if (!user_initiated_closing) {
        this->invoke_error_callback(ECONNRESET, "channel closed");
        this->state_machine_.force_state(rusty::clone(rusty::clone(ConnectionState::FAILED)));
    }
    this->heartbeat_manager_.reset();
    this->invalidate_pending_futures();
    if (!user_initiated_closing) {
        this->invoke_disconnected_callback();
    }
    const std::string addr = this->reconnect_address_.get();
    if (rusty::detail::deref_if_pointer_like(this->reconnect_policy_.get().auto_reconnect) && rusty::detail::rust_not(addr.empty())) {
        // @unsafe
        {
            this->reconnect_.channel_reconnect_attempts_.fetch_add(1, rusty::sync::atomic::Ordering::AcqRel);
        }
        const bool reconnect_aborted = this->reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire);
        if (reconnect_aborted) {
            return;
        }
        WeakClientConnection weak_conn = rusty::clone(this->weak_self_);
        rusty::thread::spawn([=, weak_conn = std::move(weak_conn)]() {
auto conn_opt = weak_conn.upgrade();
if (conn_opt.is_none()) {
    return;
}
const auto conn = conn_opt.unwrap();
const bool conn_aborted = (rusty::detail::deref_if_pointer_like(conn)).reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire);
if (rusty::detail::rust_not((rusty::detail::deref_if_pointer_like(conn)).reconnect_policy_.get().auto_reconnect) || rusty::detail::deref_if_pointer_like(conn_aborted)) {
    return;
}
const auto state = ((rusty::detail::deref_if_pointer_like(conn))).connection_state();
if ((((static_cast<int32_t>(state))) == ((static_cast<int32_t>(ConnectionState::FAILED)))) || (((static_cast<int32_t>(state))) == ((static_cast<int32_t>(ConnectionState::DISCONNECTED))))) {
    if (((rusty::detail::deref_if_pointer_like(conn))).is_factory_bound()) {
        // @unsafe
        {
            log_line(rusty::clone(rusty::clone(Log::INFO)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("rrr::ClientConnection: channel-mode auto-reconnect (factory) triggered after on_closed"));
        }
        ((rusty::detail::deref_if_pointer_like(conn))).reset_channel_mode_for_reconnect();
        const std::string reconnect_addr = (rusty::detail::deref_if_pointer_like(conn)).reconnect_address_.get();
        static_cast<void>(((rusty::detail::deref_if_pointer_like(conn))).connect(rusty::detail::ptr_cast<const int8_t*>(reconnect_addr.c_str())));
        return;
    }
    // @unsafe
    {
        log_line(rusty::clone(rusty::clone(Log::INFO)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("rrr::ClientConnection: channel-mode auto-reconnect (legacy) triggered after on_closed"));
    }
    ((rusty::detail::deref_if_pointer_like(conn))).reconnect(OnReconnectCompleteCallbackFn{});
}
}).detach();
    }
}

int32_t ClientConnection::connect_via_factory(const int8_t* addr) const {
    return clientconn_connect_via_factory((*this), addr);
}

void ClientConnection::reset_channel_mode_for_reconnect() const {
    {
        auto guard = this->fiber_channel_.lock().unwrap();
        *guard = rusty::None;
    }
    {
        auto guard = this->direct_channel_.lock().unwrap();
        *guard = rusty::None;
    }
    this->channel_mode_.set(false);
    this->state_machine_.force_state(rusty::clone(rusty::clone(ConnectionState::DISCONNECTED)));
}

int32_t ClientConnection::connect(const int8_t* addr) const {
    verify(rusty::detail::rust_not(this->state_machine_.is_connected()));
    if (rusty::detail::rust_not(this->state_machine_.transition_to(rusty::clone(rusty::clone(ConnectionState::CONNECTING))))) {
        // @unsafe
        {
            log_line(rusty::clone(rusty::clone(Log::ERROR)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("rrr::ClientConnection: cannot connect from state {}", connection_state_to_string(this->state_machine_.state())));
        }
        this->invoke_error_callback(EINVAL, "invalid state for connect");
        return EINVAL;
    }
    if (!this->is_factory_bound()) {
        // @unsafe
        {
            log_line(rusty::clone(rusty::clone(Log::ERROR)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("rrr::ClientConnection::connect: factory not bound. Channel mode requires a ChannelFactoryProxy installed via Client::set_channel_factory(...) or auto-installed by Client::connect (the latter happens unconditionally now)."));
        }
        this->state_machine_.transition_to(rusty::clone(rusty::clone(ConnectionState::FAILED)));
        this->invoke_error_callback(EINVAL, "no channel factory bound");
        return EINVAL;
    }
    return this->connect_via_factory(addr);
}

void ClientConnection::bind_channel(ChannelConnectionProxy channel) const {
    if (rusty::detail::rust_not(channel.is_valid())) {
        return;
    }
    {
        auto guard = this->fiber_channel_.lock().unwrap();
        *guard = rusty::Some(rusty::make_box<FiberChannel>(std::move(channel)));
        rusty::Box<FiberChannel>& fc = ((*guard)).as_mut().unwrap();
        ((rusty::detail::deref_if_pointer_like(fc))).bind_callbacks();
    }
    this->channel_mode_.set(true);
    WeakClientConnection weak_self = rusty::clone(this->weak_self_);
    Fiber::create_run_impl([=, weak_self = std::move(weak_self)]() {
auto conn_opt = weak_self.upgrade();
if (conn_opt.is_none()) {
    return;
}
const auto conn = conn_opt.unwrap();
((rusty::detail::deref_if_pointer_like(conn))).run_recv_loop();
}, __FILE__, __LINE__);
}

void ClientConnection::bind_channel_via_poll_thread(ChannelConnectionProxy channel) const {
    clientconn_bind_channel_via_poll_thread((*this), std::move(channel));
}

void ClientConnection::bind_channel_direct(ChannelConnectionProxy channel) const {
    if (rusty::detail::rust_not(channel.is_valid())) {
        return;
    }
    WeakClientConnection weak_frame = rusty::clone(this->weak_self_);
    WeakClientConnection weak_closed = rusty::clone(this->weak_self_);
    {
        rusty::Box<ChannelConnectionBase>& ch = channel;
        ch->set_on_frame(OnFrameCallback::from_callable([=, weak_frame = std::move(weak_frame)](const ChannelFrame& f) {
auto conn_opt = weak_frame.upgrade();
if (conn_opt.is_none()) {
    return;
}
const auto conn = conn_opt.unwrap();
((rusty::detail::deref_if_pointer_like(conn))).decode_response_and_notify(f.payload, f.size);
}));
        ch->set_on_closed(OnClosedCallback::from_callable([=, weak_closed = std::move(weak_closed)](ChannelError reason) {
auto conn_opt = weak_closed.upgrade();
if (conn_opt.is_none()) {
    return;
}
const auto conn = conn_opt.unwrap();
((rusty::detail::deref_if_pointer_like(conn))).on_channel_closed_fan_out();
}));
        ch->set_on_error(OnErrorCallback::from_callable([=](ChannelError err, std::string_view msg) {
}));
    }
    {
        auto guard = this->direct_channel_.lock().unwrap();
        *guard = rusty::Option<ChannelConnectionProxy>(std::move(channel));
    }
    this->channel_mode_.set(true);
}

void ClientConnection::bind_factory(ChannelFactoryProxy factory) {
    if (rusty::detail::rust_not(factory.is_valid())) {
        return;
    }
    auto guard = this->factory_.lock().unwrap();
    *guard = rusty::Option<ChannelFactoryProxy>(std::move(factory));
}

void ClientConnection::abort_reconnect() {
    // @unsafe
    {
        this->reconnect_.reconnect_abort_.store(true, rusty::sync::atomic::Ordering::Release);
    }
}

void ClientConnection::set_callback_manager(const rusty::Arc<CallbackManager>& callback_manager) {
    if (callback_manager.is_valid()) {
        this->callback_manager_ = rusty::clone(callback_manager);
    }
}

void ClientConnection::invalidate_pending_futures() const {
    rusty::Vec<AsyncReplyCallback> drained_callbacks = rusty::Vec<AsyncReplyCallback>::new_();
    {
        auto cb_guard = this->pending_cb_slots_.lock().unwrap();
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::len(cb_guard)) {
            if (cb_guard[i].is_some()) {
                drained_callbacks.push(rusty::mem::take(cb_guard[i]).unwrap());
            }
            i += static_cast<size_t>(1);
        }
    }
    for (auto&& cb : rusty::for_in(rusty::iter_mut(drained_callbacks))) {
        this->metrics_.record_request_dropped();
        cb(ENOTCONN, rusty::ptr::null(), 0);
    }
    rusty::Vec<rusty::Arc<Future>> futures = rusty::Vec<rusty::Arc<Future>>::new_();
    auto guard = this->pending_fu_.lock().unwrap();
    for (auto&& _for_item : rusty::for_in((*guard).drain())) {
        auto&& _xid = rusty::detail::deref_if_pointer(std::get<0>(rusty::detail::deref_if_pointer(_for_item)));
        auto&& fu = rusty::detail::deref_if_pointer(std::get<1>(rusty::detail::deref_if_pointer(_for_item)));
        futures.push(std::move(fu));
    }
    for (auto&& fu : rusty::for_in(rusty::iter(futures))) {
        this->metrics_.record_request_dropped();
        (rusty::detail::deref_if_pointer_like(fu)).error_code_.set(std::move(ENOTCONN));
        ((rusty::detail::deref_if_pointer_like(fu))).notify_ready(rusty::clone(fu));
    }
}

void ClientConnection::fail_pending_future(int64_t xid, int32_t err) const {
    rusty::Option<rusty::Arc<Future>> fu_opt = rusty::Option<rusty::Arc<Future>>{rusty::None};
    {
        auto pending_guard = this->pending_fu_.lock().unwrap();
        auto fu_ptr = ((*pending_guard)).get(std::move(xid));
        if (fu_ptr.is_some()) {
            fu_opt = rusty::Option<rusty::Arc<Future>>(rusty::clone(fu_ptr.unwrap()));
            ((*pending_guard)).remove(std::move(xid));
        }
    }
    if (fu_opt.is_some()) {
        const auto fu = fu_opt.unwrap();
        this->metrics_.record_request_dropped();
        (rusty::detail::deref_if_pointer_like(fu)).error_code_.set(std::move(err));
        ((rusty::detail::deref_if_pointer_like(fu))).notify_ready(rusty::clone(fu));
    }
}

void ClientConnection::close() const {
    const auto prev_state = this->state_machine_.state();
    const bool was_connected = this->state_machine_.is_connected();
    if (was_connected) {
        this->state_machine_.transition_to(rusty::clone(rusty::clone(ConnectionState::DISCONNECTING)));
    }
    {
        auto guard = this->direct_channel_.lock().unwrap();
        if (((*guard)).is_some()) {
            rusty::Box<ChannelConnectionBase>& ch = ((*guard)).as_mut().unwrap();
            ((rusty::detail::deref_if_pointer_like(ch))).close();
        }
    }
    {
        auto guard = this->fiber_channel_.lock().unwrap();
        if (((*guard)).is_some()) {
            rusty::Box<FiberChannel>& fc = ((*guard)).as_mut().unwrap();
            ((rusty::detail::deref_if_pointer_like(fc))).close();
        }
    }
    if (was_connected) {
        this->state_machine_.transition_to(rusty::clone(rusty::clone(ConnectionState::DISCONNECTED)));
    } else if (rusty::detail::rust_not(this->state_machine_.is_terminal())) {
        this->state_machine_.force_state(rusty::clone(rusty::clone(ConnectionState::DISCONNECTED)));
    }
    this->heartbeat_manager_.reset();
    this->invalidate_pending_futures();
    if ((((static_cast<int32_t>(prev_state))) == ((static_cast<int32_t>(ConnectionState::CONNECTED)))) || (((static_cast<int32_t>(prev_state))) == ((static_cast<int32_t>(ConnectionState::DISCONNECTING))))) {
        this->invoke_disconnected_callback();
    }
}

void ClientConnection::mark_closing() const {
    // @unsafe
    {
        this->reconnect_.reconnect_abort_.store(true, rusty::sync::atomic::Ordering::Release);
    }
    if (this->state_machine_.is_connected()) {
        this->state_machine_.transition_to(rusty::clone(rusty::clone(ConnectionState::DISCONNECTING)));
    }
    this->invalidate_pending_futures();
}

int32_t ClientConnection::reconnect(OnReconnectCompleteCallbackFn on_complete) const {
    return clientconn_reconnect((*this), std::move(on_complete));
}

void ClientConnection::set_buffering_config(const BufferingConfig& config) const {
    this->buffering_config_.set(std::move(config));
    if (rusty::detail::rust_not(this->pending_queue_.empty())) {
        this->pending_queue_.clear_all(ECONNABORTED);
    }
    this->pending_queue_.update_config(config.to_queue_config());
}

void ClientConnection::set_heartbeat_config(const HeartbeatConfig& config) const {
    this->heartbeat_manager_.set_config(config);
    WeakClientConnection weak_conn = rusty::clone(this->weak_self_);
    this->heartbeat_manager_.set_on_timeout([=, weak_conn = std::move(weak_conn)]() {
auto conn_opt = weak_conn.upgrade();
if (conn_opt.is_none()) {
    return;
}
const auto conn = conn_opt.unwrap();
if (rusty::detail::rust_not(((rusty::detail::deref_if_pointer_like(conn))).connected())) {
    return;
}
// @unsafe
{
    log_line(rusty::clone(rusty::clone(Log::WARN)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("rrr::ClientConnection: heartbeat timeout for {}", ((rusty::detail::deref_if_pointer_like(conn))).host().c_str()));
}
((rusty::detail::deref_if_pointer_like(conn))).handle_error();
});
}

HeartbeatConfig ClientConnection::heartbeat_config() const {
    return this->heartbeat_manager_.config();
}

void ClientConnection::set_circuit_breaker_config(const CircuitBreakerConfig& config) const {
    this->circuit_breaker_.set_config(config);
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
    if (rusty::detail::rust_not(allowed)) {
        this->metrics_.record_circuit_open_rejection();
    }
    return std::move(allowed);
}

void ClientConnection::record_circuit_state_transition(CircuitState before, CircuitState after) const {
    if (rusty::detail::deref_if_pointer_like(before) == rusty::detail::deref_if_pointer_like(after)) {
        return;
    }
    switch (after) {
    case CircuitState::OPEN:
    {
        this->metrics_.record_circuit_open_transition();
        break;
    }
    case CircuitState::HALF_OPEN:
    {
        this->metrics_.record_circuit_half_open_transition();
        break;
    }
    case CircuitState::CLOSED:
    {
        this->metrics_.record_circuit_closed_transition();
        break;
    }
    default:
    {
        break;
    }
    }
}

void ClientConnection::record_circuit_result(int32_t err) const {
    auto before = this->circuit_breaker_.state();
    if (rusty::detail::deref_if_pointer_like(err) == static_cast<int32_t>(0)) {
        this->circuit_breaker_.record_success();
    } else if (this->should_trip_circuit_for_error(std::move(err))) {
        this->circuit_breaker_.record_failure();
    }
    auto after = this->circuit_breaker_.state();
    this->record_circuit_state_transition(std::move(before), std::move(after));
}

void ClientConnection::invoke_error_callback(int32_t err, const std::string& message) const {
    if (rusty::detail::rust_not(this->callback_manager_.is_valid())) {
        return;
    }
    ((rusty::detail::deref_if_pointer_like(this->callback_manager_))).invoke_on_error(clientconn_map_system_error(std::move(err)), message);
}

void ClientConnection::invoke_disconnected_callback() const {
    if (rusty::detail::rust_not(this->callback_manager_.is_valid())) {
        return;
    }
    ((rusty::detail::deref_if_pointer_like(this->callback_manager_))).invoke_on_disconnected();
}

void ClientConnection::invoke_reconnecting_callback() const {
    if (rusty::detail::rust_not(this->callback_manager_.is_valid())) {
        return;
    }
    ((rusty::detail::deref_if_pointer_like(this->callback_manager_))).invoke_on_reconnecting();
}

void ClientConnection::invoke_reconnected_callback(bool success) const {
    if (rusty::detail::rust_not(this->callback_manager_.is_valid())) {
        return;
    }
    ((rusty::detail::deref_if_pointer_like(this->callback_manager_))).invoke_on_reconnected(std::move(success));
}

void ClientConnection::invoke_connected_callback() const {
    if (rusty::detail::rust_not(this->callback_manager_.is_valid())) {
        return;
    }
    ((rusty::detail::deref_if_pointer_like(this->callback_manager_))).invoke_on_connected();
}

ChannelError ClientConnection::dispatch_frame_via_channel(const uint8_t* body_bytes, size_t body_size) const {
    return clientconn_dispatch_frame_via_channel((*this), body_bytes, std::move(body_size));
}

void ClientConnection::handle_error() const {
    const auto prev_state = this->state_machine_.state();
    const bool abort_flag = this->reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire);
    const bool user_initiated_closing = ((((static_cast<int32_t>(prev_state))) == ((static_cast<int32_t>(ConnectionState::DISCONNECTING)))) || (((static_cast<int32_t>(prev_state))) == ((static_cast<int32_t>(ConnectionState::DISCONNECTED))))) || rusty::detail::deref_if_pointer_like(abort_flag);
    if (!user_initiated_closing) {
        this->invoke_error_callback(ECONNRESET, "connection error");
        this->state_machine_.force_state(rusty::clone(rusty::clone(ConnectionState::FAILED)));
    }
    this->close();
    if (user_initiated_closing) {
        return;
    }
    this->invoke_disconnected_callback();
    const bool reconnect_aborted = this->reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire);
    if (rusty::detail::deref_if_pointer_like(this->reconnect_policy_.get().auto_reconnect) && !reconnect_aborted) {
        const std::string addr = this->reconnect_address_.get();
        if (addr.empty()) {
            return;
        }
        WeakClientConnection weak_conn = rusty::clone(this->weak_self_);
        rusty::thread::spawn([=, weak_conn = std::move(weak_conn)]() {
auto conn_opt = weak_conn.upgrade();
if (conn_opt.is_none()) {
    return;
}
const auto conn = conn_opt.unwrap();
const bool conn_aborted = (rusty::detail::deref_if_pointer_like(conn)).reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire);
if (rusty::detail::rust_not((rusty::detail::deref_if_pointer_like(conn)).reconnect_policy_.get().auto_reconnect) || rusty::detail::deref_if_pointer_like(conn_aborted)) {
    return;
}
const auto state = ((rusty::detail::deref_if_pointer_like(conn))).connection_state();
if ((((static_cast<int32_t>(state))) == ((static_cast<int32_t>(ConnectionState::FAILED)))) || (((static_cast<int32_t>(state))) == ((static_cast<int32_t>(ConnectionState::DISCONNECTED))))) {
    // @unsafe
    {
        log_line(rusty::clone(rusty::clone(Log::INFO)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("rrr::ClientConnection: auto-reconnect triggered after connection failure"));
    }
    ((rusty::detail::deref_if_pointer_like(conn))).reconnect(OnReconnectCompleteCallbackFn{});
}
}).detach();
    }
}

bool ClientConnection::check_pending_write_update() const {
    if (this->state_machine_.is_connected() && rusty::detail::rust_not(this->paused_.get())) {
        if (this->heartbeat_manager_.check_timeout()) {
            return false;
        }
        if (this->heartbeat_manager_.should_send_heartbeat()) {
            this->enqueue_heartbeat_probe();
            this->heartbeat_manager_.on_heartbeat_sent();
            return true;
        }
    }
    return false;
}

void ClientConnection::handle_free(int64_t xid) const {
    auto guard = this->pending_fu_.lock().unwrap();
    if ((*guard).remove(std::move(xid)).is_some()) {
        this->metrics_.record_request_dropped();
    }
}

bool ClientConnection::is_factory_bound() const {
    return ((*this->factory_.lock().unwrap())).is_some();
}

uint64_t ClientConnection::channel_reconnect_attempts_count() const {
    // @unsafe
    {
        return this->reconnect_.channel_reconnect_attempts_.load(rusty::sync::atomic::Ordering::Acquire);
    }
}

void ClientConnection::set_reconnect_policy(const ReconnectPolicy& policy) const {
    this->reconnect_policy_.set(std::move(policy));
}

bool ClientConnection::is_reconnecting() const {
    // @unsafe
    {
        return this->reconnect_.reconnecting_.load(rusty::sync::atomic::Ordering::Acquire);
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
    this->on_server_restart_.replace(std::move(callback));
}

bool ClientConnection::check_server_instance(uint64_t new_id) const {
    const auto old_id = this->server_instance_id_.get();
    this->server_instance_id_.set(std::move(new_id));
    if ((rusty::detail::deref_if_pointer_like(old_id) != static_cast<uint64_t>(0)) && (rusty::detail::deref_if_pointer_like(old_id) != rusty::detail::deref_if_pointer_like(new_id))) {
        // @unsafe
        {
            log_line(rusty::clone(rusty::clone(Log::INFO)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("Server restart detected: old_id={} new_id={}", std::move(old_id), std::move(new_id)));
        }
        auto cb_ref = this->on_server_restart_.borrow_mut();
        if (*cb_ref) {
            (*cb_ref)(std::move(old_id), std::move(new_id));
        }
        return true;
    }
    return false;
}

void ClientConnection::set_keepalive(const KeepaliveConfig& config) const {
    this->keepalive_config_.set(std::move(config));
}

void ClientConnection::on_request_dispatched(size_t bytes) const {
    this->metrics_.record_bytes_sent(static_cast<uint64_t>(bytes));
    this->update_last_activity(clientconn_monotonic_ms_now());
}

void ClientConnection::on_response_received(size_t bytes) const {
    this->metrics_.record_bytes_received(static_cast<uint64_t>(bytes));
    this->update_last_activity(clientconn_monotonic_ms_now());
}

std::string ClientConnection::host() const {
    return this->host_;
}

bool ClientConnection::should_trip_circuit_for_error(int32_t err) {
    if (rusty::detail::deref_if_pointer_like(err) == static_cast<int32_t>(0)) {
        return false;
    }
    if ((((((((rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ENOTCONN)) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ECONNREFUSED))) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ECONNRESET))) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ECONNABORTED))) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ETIMEDOUT))) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EHOSTUNREACH))) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ENETUNREACH))) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EPIPE))) {
        return true;
    }
    return false;
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

int32_t ClientConnection::handle_write() const {
    return PollMode::NO_CHANGE;
}

bool ClientConnection::handle_read() const {
    return false;
}

bool ClientConnection::is_closed() const {
    return this->state_machine_.is_terminal();
}
/*RUSTYCPP:GEN-END id=client.8*/

}  // export namespace rrr


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
//     contains a non-copyable `rusty::Mutex` field.
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
    pending_factory_field: rusty::Mutex<Option<ChannelFactoryProxy>>,
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
            pending_factory_field: rusty::Mutex::<Option<ChannelFactoryProxy>>::new(Option::<ChannelFactoryProxy>(None)),
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
        if !factory.is_valid() {
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
/*RUSTYCPP:GEN-BEGIN id=client.1 version=1 rust_sha256=e909790c93a0b7e470f3b1323ced8e1b10a6975a91c18e50e3e7283b6df9adcd*/
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
    rusty::Mutex<rusty::Option<ChannelFactoryProxy>> pending_factory_field;
    ConnectionMetrics empty_metrics_field;
    mutable bool _rusty_forgotten = false;
    Client(rusty::RefCell<rusty::Option<rusty::Arc<ClientConnection>>> connection_field_init, rusty::Arc<PollThread> poll_thread_worker_field_init, rusty::Cell<bool> is_client_mode_field_init, rusty::Cell<int64_t> time_field_init, rusty::Cell<uint64_t> timeout_field_init, rusty::Cell<int32_t> rpc_id_field_init, rusty::Cell<KeepaliveConfig> pending_keepalive_config_field_init, rusty::Cell<HeartbeatConfig> pending_heartbeat_config_field_init, rusty::Cell<CircuitBreakerConfig> pending_circuit_breaker_config_field_init, rusty::Cell<ReconnectPolicy> pending_reconnect_policy_field_init, rusty::Arc<CallbackManager> callback_manager_field_init, rusty::Mutex<rusty::Option<ChannelFactoryProxy>> pending_factory_field_init, ConnectionMetrics empty_metrics_field_init) : connection_field(std::move(connection_field_init)), poll_thread_worker_field(std::move(poll_thread_worker_field_init)), is_client_mode_field(std::move(is_client_mode_field_init)), time_field(std::move(time_field_init)), timeout_field(std::move(timeout_field_init)), rpc_id_field(std::move(rpc_id_field_init)), pending_keepalive_config_field(std::move(pending_keepalive_config_field_init)), pending_heartbeat_config_field(std::move(pending_heartbeat_config_field_init)), pending_circuit_breaker_config_field(std::move(pending_circuit_breaker_config_field_init)), pending_reconnect_policy_field(std::move(pending_reconnect_policy_field_init)), callback_manager_field(std::move(callback_manager_field_init)), pending_factory_field(std::move(pending_factory_field_init)), empty_metrics_field(std::move(empty_metrics_field_init)) {}
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
    void rusty_mark_forgotten() const noexcept { _rusty_forgotten = true; rusty::detail::mark_forgotten_if_supported(this->connection_field); rusty::detail::mark_forgotten_if_supported(this->poll_thread_worker_field); rusty::detail::mark_forgotten_if_supported(this->is_client_mode_field); rusty::detail::mark_forgotten_if_supported(this->time_field); rusty::detail::mark_forgotten_if_supported(this->timeout_field); rusty::detail::mark_forgotten_if_supported(this->rpc_id_field); rusty::detail::mark_forgotten_if_supported(this->pending_keepalive_config_field); rusty::detail::mark_forgotten_if_supported(this->pending_heartbeat_config_field); rusty::detail::mark_forgotten_if_supported(this->pending_circuit_breaker_config_field); rusty::detail::mark_forgotten_if_supported(this->pending_reconnect_policy_field); rusty::detail::mark_forgotten_if_supported(this->callback_manager_field); rusty::detail::mark_forgotten_if_supported(this->pending_factory_field); rusty::detail::mark_forgotten_if_supported(this->empty_metrics_field); }


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
    return Client(rusty::RefCell<rusty::Option<rusty::Arc<ClientConnection>>>::new_(rusty::Option<rusty::Arc<ClientConnection>>{rusty::None}), std::move(poll_thread_worker), rusty::Cell<bool>::new_(false), rusty::Cell<int64_t>::new_(static_cast<int64_t>(0)), rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)), rusty::Cell<int32_t>::new_(static_cast<int32_t>(0)), rusty::Cell<KeepaliveConfig>::new_(KeepaliveConfig{}), rusty::Cell<HeartbeatConfig>::new_(HeartbeatConfig::disabled()), rusty::Cell<CircuitBreakerConfig>::new_(CircuitBreakerConfig::disabled()), rusty::Cell<ReconnectPolicy>::new_(ReconnectPolicy::conservative()), rusty::Arc<CallbackManager>::new_(CallbackManager::new_()), rusty::Mutex<rusty::Option<ChannelFactoryProxy>>::new_(rusty::Option<ChannelFactoryProxy>(rusty::None)), ConnectionMetrics::new_());
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
    return ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->request(std::move(rpc_id), attr, std::move(write_fn));
}

template<typename F>
FutureResult Client::request_with_options(int32_t rpc_id, const RequestOptions& options, F write_fn) const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_none()) {
        return FutureResult::Err(ENOTCONN);
    }
    this->rpc_id_field.set(std::move(rpc_id));
    return ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->request_with_options(std::move(rpc_id), options, FutureAttr{}, std::move(write_fn));
}

template<typename F>
auto Client::request_async(int32_t rpc_id, F write_fn, AsyncReplyCallback on_reply) const -> rusty::Result<rusty::Unit, int32_t> {
    const auto guard = this->connection_field.borrow();
    if (guard->is_none()) {
        return rusty::Result<rusty::Unit, int32_t>::Err(ENOTCONN);
    }
    this->rpc_id_field.set(std::move(rpc_id));
    return ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->request_async(std::move(rpc_id), std::move(write_fn), std::move(on_reply));
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
        if ((*guard).is_some()) {
            ChannelFactoryProxy moved = (*guard).take().unwrap();
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
        auto& conn_ref = ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap();
        const bool was_connected = conn_ref->connected();
        conn_ref->mark_closing();
        if (was_connected) {
            rusty::Arc<ClientConnection> conn_arc = rusty::clone(conn_ref);
            const rusty::Arc<OneTimeJob> close_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob::new_([=, conn_arc = std::move(conn_arc)]() {
conn_arc->close();
}));
            this->poll_thread_worker_field->add(std::move(close_job));
        }
    }
}

void Client::handle_free(int64_t xid) const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->handle_free(std::move(xid));
    }
}

void Client::pause() const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->pause();
    }
}

void Client::resume() const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->resume();
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
    return ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->reconnect(std::move(on_complete));
}

void Client::set_channel_factory(ChannelFactoryProxy factory) const {
    if (rusty::detail::rust_not(factory.is_valid())) {
        return;
    }
    auto guard = this->pending_factory_field.lock().unwrap();
    *guard = rusty::Option<ChannelFactoryProxy>(std::move(factory));
}

bool Client::has_pending_channel_factory() const {
    auto guard = this->pending_factory_field.lock().unwrap();
    return (*guard).is_some();
}

size_t Client::pending_request_count() const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        return ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->pending_request_count();
    }
    return static_cast<size_t>(0);
}

void Client::clear_pending_requests(int32_t error_code) const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->clear_pending_requests(std::move(error_code));
    }
}

bool Client::is_reconnecting() const {
    const auto guard = this->connection_field.borrow();
    return guard->is_some() && ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->is_reconnecting();
}

rusty::String Client::host() const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        return ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->host();
    }
    return rusty::String{};
}

bool Client::connected() const {
    const auto guard = this->connection_field.borrow();
    return guard->is_some() && ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->connected();
}

ConnectionState Client::connection_state() const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        return ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->connection_state();
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
        return rusty::Option<rusty::Arc<ClientConnection>>(rusty::clone(([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()));
    }
    return rusty::Option<rusty::Arc<ClientConnection>>{rusty::None};
}

uint64_t Client::server_instance_id() const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        return ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->server_instance_id();
    }
    return static_cast<uint64_t>(0);
}

void Client::set_on_server_restart(OnServerRestartCallbackFn callback) const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->set_on_server_restart(std::move(callback));
    }
}

bool Client::check_server_instance(uint64_t new_id) const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        return ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->check_server_instance(std::move(new_id));
    }
    return false;
}

void Client::set_reconnect_policy(const ReconnectPolicy& policy) const {
    this->pending_reconnect_policy_field.set(policy);
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->set_reconnect_policy(policy);
    }
}

void Client::set_buffering_config(const BufferingConfig& config) const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->set_buffering_config(config);
    }
}

void Client::set_keepalive(const KeepaliveConfig& config) const {
    this->pending_keepalive_config_field.set(config);
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->set_keepalive(config);
    }
}

KeepaliveConfig Client::keepalive_config() const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        return ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->keepalive_config();
    }
    return this->pending_keepalive_config_field.get();
}

void Client::set_heartbeat(const HeartbeatConfig& config) const {
    this->pending_heartbeat_config_field.set(config);
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->set_heartbeat_config(config);
    }
}

HeartbeatConfig Client::heartbeat_config() const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        return ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->heartbeat_config();
    }
    return this->pending_heartbeat_config_field.get();
}

void Client::set_circuit_breaker(const CircuitBreakerConfig& config) const {
    this->pending_circuit_breaker_config_field.set(config);
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->set_circuit_breaker_config(config);
    }
}

CircuitBreakerConfig Client::circuit_breaker_config() const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        return ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->circuit_breaker_config();
    }
    return this->pending_circuit_breaker_config_field.get();
}

CircuitState Client::circuit_breaker_state() const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        return ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->circuit_breaker_state();
    }
    return rusty::clone(rusty::clone(CircuitState::CLOSED));
}

bool Client::is_idle(uint64_t idle_ms, uint64_t current_time_ms) const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        return ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->is_idle(std::move(idle_ms), std::move(current_time_ms));
    }
    return false;
}

bool Client::validate_connection() const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        return ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->validate_connection();
    }
    return false;
}

const ConnectionMetrics& Client::metrics() const {
    const auto guard = this->connection_field.borrow();
    if (guard->is_some()) {
        return ([&](auto&& __b) -> decltype(auto) { if constexpr (requires { (*__b).as_ref(); }) { return (*__b).as_ref(); } else { return __b.as_ref(); } })(guard).unwrap()->metrics();
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


// Forward declaration + free-fn declarations for the clientpool_* helpers
// that the DSL ClientPool's generated method defs delegate to: the floored
// network-I/O get_client and the four get_mut-mutating cleanup methods keep
// their proven hand-written C++ bodies (defined far below, near where the
// old out-of-line method defs lived).
struct ClientPool;
// The generated `ClientPool::pool_config()` returns `std::move(*guard)` —
// it moves out of the value the mutex protects. That is correct ONLY while
// PoolConfig is trivially copyable, where the move degrades to a copy and
// the stored config is left intact. Add a non-trivial member (a
// std::string, say) and that same line would silently GUT the pool's
// config on every read. Fail the build instead of shipping that.
// Authored as inline Rust DSL: a top-level `const _: () = assert!(cond,
// "prose")` lowers to a namespace-scope `static_assert` (same idiom as
// `envelope_assert_in_type_list` in serializable_envelope.cpp). The
// emitted message is the stringified assertion, so the prose survives
// nested inside it.
#if RUSTYCPP_RUST
const _: () = assert!(std::is_trivially_copyable::<PoolConfig>::value,
    "ClientPool::pool_config() returns std::move(*guard); a non-trivially-copyable PoolConfig would be moved OUT of the mutex and leave the stored config wrecked. Give pool_config() an explicit copy before relaxing this.");
#endif
/*RUSTYCPP:GEN-BEGIN id=client.14 version=1 rust_sha256=c46ad82d389216713065061581cc71ba116c0198775d92cb164dbbfecd95953e*/
static_assert(std::is_trivially_copyable<PoolConfig>::value, "assert ! (std :: is_trivially_copyable ::< PoolConfig >:: value , \"ClientPool::pool_config() returns std::move(*guard); a non-trivially-copyable PoolConfig would be moved OUT of the mutex and leave the stored config wrecked. Give pool_config() an explicit copy before relaxing this.\")");
/*RUSTYCPP:GEN-END id=client.14*/

// Health check on an EXPLICIT config snapshot. The config is passed in
// rather than read from `self` because every caller below runs inside the
// `state_` critical section: reading `config_` there would acquire the
// config lock while holding `state_`, inverting the order against
// `get_client` (which snapshots the config before locking `state_`).
bool clientpool_is_client_healthy_with(PoolConfig cfg, const rusty::Arc<Client>& client);
size_t clientpool_get_healthy_client_count(const ClientPool& self, const std::string& addr);
size_t clientpool_remove_unhealthy_clients(const ClientPool& self, const std::string& addr);
size_t clientpool_close_idle_clients(const ClientPool& self, const std::string& addr, uint64_t current_time_ms);
size_t clientpool_remove_all_unhealthy(const ClientPool& self);
size_t clientpool_close_all_idle(const ClientPool& self, uint64_t current_time_ms);
rusty::Option<rusty::Arc<Client>> clientpool_get_client(const ClientPool& self, const std::string& addr);

// @safe - Thread-safe pool of client connections using Arc.
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is the
// source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. PoolState bundles the rusty::Mutex-guarded
// per-address client cache + load-balancer state (get_client touches both
// under one lock). The plain `fn new(...) -> ClientPool` lowers to a
// value-returning `ClientPool::new_(...)` factory (no `#[cpp_ctor]`); the
// four get_mut cleanup methods + the @unsafe get_client delegate to
// hand-written free fns.
#if RUSTYCPP_RUST
struct PoolState {
    cache: BTreeMap<std::string, Vec<Arc<Client>>>,
    lb_state: BTreeMap<std::string, LoadBalancerState>,
}

impl PoolState {
    fn new() -> PoolState {
        PoolState {
            cache: BTreeMap::<std::string, Vec<Arc<Client>>>::new(),
            lb_state: BTreeMap::<std::string, LoadBalancerState>::new(),
        }
    }
}

// `config_` is a Mutex, not a Cell. PoolConfig is ~40 bytes of plain
// members, so a Cell read racing a `set_pool_config` write is a torn
// read — UB, not merely a stale value. set_pool_config is public and
// callable at any time, so that race is reachable.
//
// LOCK ORDER INVARIANT: never acquire `config_` while holding `state_`.
// Every kernel snapshots the config FIRST and then takes `state_`, so the
// two locks are never held together and there is no ordering hazard. This
// is why the health check takes its config as an argument
// (`clientpool_is_client_healthy_with`) rather than reading `config_`
// itself — it is called from inside the `state_` critical section, and
// re-reading there would invert the order against `get_client`.
struct ClientPool {
    poll_thread_worker_: Option<Arc<PollThread>>,
    state_: rusty::Mutex<PoolState>,
    config_: rusty::Mutex<PoolConfig>,
}

impl Drop for ClientPool {
    fn drop(&mut self) {
        let guard = self.state_.lock().unwrap();
        for (_addr, clients) in (*guard).cache.iter() {
            for client in &clients {
                (*client).close();
            }
        }
        if self.poll_thread_worker_.is_some() {
            (*self.poll_thread_worker_.as_ref().unwrap()).shutdown();
        }
    }
}

impl ClientPool {
    fn new(poll_thread_worker: Option<Arc<PollThread>>, config: PoolConfig) -> ClientPool {
        verify(config.min_connections > 0);
        verify(config.max_connections >= config.min_connections);
        let mut ptw: Option<Arc<PollThread>> = poll_thread_worker;
        if ptw.is_none() {
            ptw = Some(PollThread::create());
        }
        ClientPool {
            poll_thread_worker_: ptw,
            state_: rusty::Mutex::<PoolState>::new(PoolState::new()),
            config_: rusty::Mutex::<PoolConfig>::new(config),
        }
    }

    fn set_pool_config(&self, config: PoolConfig) {
        let guard = self.config_.lock().unwrap();
        (*guard) = config;
    }

    fn pool_config(&self) -> PoolConfig {
        let guard = self.config_.lock().unwrap();
        (*guard)
    }

    fn is_client_healthy(&self, client: &Arc<Client>) -> bool {
        clientpool_is_client_healthy_with(self.pool_config(), client)
    }

    fn get_healthy_client_count(&self, addr: &std::string) -> usize {
        clientpool_get_healthy_client_count(self, addr)
    }

    fn total_client_count(&self) -> usize {
        let guard = self.state_.lock().unwrap();
        let mut count: usize = 0;
        for (_addr, clients) in (*guard).cache.iter() {
            count += clients.len();
        }
        count
    }

    fn address_count(&self) -> usize {
        let guard = self.state_.lock().unwrap();
        (*guard).cache.len()
    }

    fn remove_unhealthy_clients(&self, addr: &std::string) -> usize {
        clientpool_remove_unhealthy_clients(self, addr)
    }

    fn close_idle_clients(&self, addr: &std::string, current_time_ms: u64) -> usize {
        clientpool_close_idle_clients(self, addr, current_time_ms)
    }

    fn remove_all_unhealthy(&self) -> usize {
        clientpool_remove_all_unhealthy(self)
    }

    fn close_all_idle(&self, current_time_ms: u64) -> usize {
        clientpool_close_all_idle(self, current_time_ms)
    }

    fn get_client(&self, addr: &std::string) -> Option<Arc<Client>> {
        clientpool_get_client(self, addr)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.client_pool version=1 rust_sha256=1c052d979698a369ae493d8875a0384bd60cbc8fdd3d911f428339fcaa439119*/
struct PoolState;
struct ClientPool;

struct PoolState {
    rusty::BTreeMap<std::string, rusty::Vec<rusty::Arc<Client>>> cache;
    rusty::BTreeMap<std::string, LoadBalancerState> lb_state;

    static PoolState new_();
};

struct ClientPool {
    rusty::Option<rusty::Arc<PollThread>> poll_thread_worker_;
    rusty::Mutex<PoolState> state_;
    rusty::Mutex<PoolConfig> config_;
    mutable bool _rusty_forgotten = false;
    ClientPool(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker__init, rusty::Mutex<PoolState> state__init, rusty::Mutex<PoolConfig> config__init) : poll_thread_worker_(std::move(poll_thread_worker__init)), state_(std::move(state__init)), config_(std::move(config__init)) {}
    ClientPool(const ClientPool&) = delete;
    ClientPool(ClientPool&& other) noexcept : poll_thread_worker_(std::move(other.poll_thread_worker_)), state_(std::move(other.state_)), config_(std::move(other.config_)) {
        this->_rusty_forgotten = other._rusty_forgotten;
        other._rusty_forgotten = true;
    }
    ClientPool& operator=(const ClientPool&) = delete;
    ClientPool& operator=(ClientPool&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        this->~ClientPool();
        new (this) ClientPool(std::move(other));
        return *this;
    }
    void rusty_mark_forgotten() const noexcept { _rusty_forgotten = true; rusty::detail::mark_forgotten_if_supported(this->poll_thread_worker_); rusty::detail::mark_forgotten_if_supported(this->state_); rusty::detail::mark_forgotten_if_supported(this->config_); }


    ~ClientPool() noexcept(false);
    static ClientPool new_(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker, PoolConfig config);
    void set_pool_config(PoolConfig config) const;
    PoolConfig pool_config() const;
    bool is_client_healthy(const rusty::Arc<Client>& client) const;
    size_t get_healthy_client_count(const std::string& addr) const;
    size_t total_client_count() const;
    size_t address_count() const;
    size_t remove_unhealthy_clients(const std::string& addr) const;
    size_t close_idle_clients(const std::string& addr, uint64_t current_time_ms) const;
    size_t remove_all_unhealthy() const;
    size_t close_all_idle(uint64_t current_time_ms) const;
    rusty::Option<rusty::Arc<Client>> get_client(const std::string& addr) const;
};


PoolState PoolState::new_() {
    return PoolState{.cache = rusty::BTreeMap<std::string, rusty::Vec<rusty::Arc<Client>>>::new_(), .lb_state = rusty::BTreeMap<std::string, LoadBalancerState>::new_()};
}

ClientPool::~ClientPool() noexcept(false) {
    if (_rusty_forgotten) { return; }
    auto guard = this->state_.lock().unwrap();
    for (auto&& _for_item : rusty::for_in(rusty::iter((*guard).cache))) {
        auto&& _addr = rusty::detail::deref_if_pointer(std::get<0>(rusty::detail::deref_if_pointer(_for_item)));
        auto&& clients = rusty::detail::deref_if_pointer(std::get<1>(rusty::detail::deref_if_pointer(_for_item)));
        for (auto&& client : rusty::for_in(rusty::iter(clients))) {
            ((rusty::detail::deref_if_pointer_like(client))).close();
        }
    }
    if (this->poll_thread_worker_.is_some()) {
        ((rusty::detail::deref_if_pointer_like(this->poll_thread_worker_.as_ref().unwrap()))).shutdown();
    }
}

ClientPool ClientPool::new_(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker, PoolConfig config) {
    verify(rusty::detail::deref_if_pointer_like(config.min_connections) > 0);
    verify(rusty::detail::deref_if_pointer_like(config.max_connections) >= rusty::detail::deref_if_pointer_like(config.min_connections));
    rusty::Option<rusty::Arc<PollThread>> ptw = poll_thread_worker;
    if (ptw.is_none()) {
        ptw = rusty::Option<rusty::Arc<PollThread>>(PollThread::create());
    }
    return ClientPool(std::move(ptw), rusty::Mutex<PoolState>::new_(PoolState::new_()), rusty::Mutex<PoolConfig>::new_(std::move(config)));
}

void ClientPool::set_pool_config(PoolConfig config) const {
    auto guard = this->config_.lock().unwrap();
    (*guard) = std::move(config);
}

PoolConfig ClientPool::pool_config() const {
    auto guard = this->config_.lock().unwrap();
    return std::move((*guard));
}

bool ClientPool::is_client_healthy(const rusty::Arc<Client>& client) const {
    return clientpool_is_client_healthy_with(this->pool_config(), client);
}

size_t ClientPool::get_healthy_client_count(const std::string& addr) const {
    return clientpool_get_healthy_client_count((*this), addr);
}

size_t ClientPool::total_client_count() const {
    auto guard = this->state_.lock().unwrap();
    size_t count = static_cast<size_t>(0);
    for (auto&& _for_item : rusty::for_in(rusty::iter((*guard).cache))) {
        auto&& _addr = rusty::detail::deref_if_pointer(std::get<0>(rusty::detail::deref_if_pointer(_for_item)));
        auto&& clients = rusty::detail::deref_if_pointer(std::get<1>(rusty::detail::deref_if_pointer(_for_item)));
        count += rusty::len(clients);
    }
    return std::move(count);
}

size_t ClientPool::address_count() const {
    auto guard = this->state_.lock().unwrap();
    return rusty::len((*guard).cache);
}

size_t ClientPool::remove_unhealthy_clients(const std::string& addr) const {
    return clientpool_remove_unhealthy_clients((*this), addr);
}

size_t ClientPool::close_idle_clients(const std::string& addr, uint64_t current_time_ms) const {
    return clientpool_close_idle_clients((*this), addr, std::move(current_time_ms));
}

size_t ClientPool::remove_all_unhealthy() const {
    return clientpool_remove_all_unhealthy((*this));
}

size_t ClientPool::close_all_idle(uint64_t current_time_ms) const {
    return clientpool_close_all_idle((*this), std::move(current_time_ms));
}

rusty::Option<rusty::Arc<Client>> ClientPool::get_client(const std::string& addr) const {
    return clientpool_get_client((*this), addr);
}
/*RUSTYCPP:GEN-END id=client.client_pool*/

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

// @safe - Mutex::lock + Condvar::wait_timeout_while are @safe; the only
// escape is the `std::chrono::duration<double>` ctor.

// @unsafe - Condvar::notify_all + user callback dispatch outside the lock.

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

// @safe - ctor helper: RequestQueue has a config-taking ctor, not a new_().
#if RUSTYCPP_RUST
fn make_pending_queue(c: &RequestQueueConfig) -> RequestQueue {
    RequestQueue(c)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.38 version=1 rust_sha256=b6ae865426332ccf95e9461479fbc2bde420aa762ef1e3b4e9a24255e45fb164*/
RequestQueue make_pending_queue(const RequestQueueConfig& c) {
    return RequestQueue(c);
}
/*RUSTYCPP:GEN-END id=client.38*/

// @safe - delegates to rusty::sys::time::clock_monotonic_us.
// Authored as inline Rust DSL (module-scope free fn, mirroring
// heartbeat_time_us): the `#if RUSTYCPP_RUST` block is the source of
// truth; the transpiler regenerates the matching GEN block below.
#if RUSTYCPP_RUST
fn clientconn_monotonic_ms_now() -> u64 { rusty::sys::time::clock_monotonic_us() / 1000 }
#endif
/*RUSTYCPP:GEN-BEGIN id=client.monotonic_ms_now version=1 rust_sha256=c1745ab0c4e5cc288ad2cb1d465716128edb56af6e760616553c187d34c2d1a6*/
uint64_t clientconn_monotonic_ms_now();

uint64_t clientconn_monotonic_ms_now() {
    return rusty::sys::time::clock_monotonic_us() / static_cast<uint64_t>(1000);
}
/*RUSTYCPP:GEN-END id=client.monotonic_ms_now*/


// @safe - HashMap::get returns Option<V&> now; rusty::Mutex::lock returns
// LockResult; Arc::clone is @safe. Only notify_ready stays @unsafe.


// @unsafe - Drives channel proxy close + invalidates futures.
//
// 4g3c3: The legacy `if (socket_ >= 0) ::close(socket_)` block has
// been removed; channel mode is unconditional and the channel layer
// (TcpConnection) owns its own fd. We instead drive `close()` on the
// bound channel proxy(ies). Close is idempotent (channel-layer
// contract), so it's fine if `on_channel_closed_fan_out` then fires
// `on_closed` after this method returns.
// const: every mutation routes through rusty::Mutex / Cell / Function /
// heartbeat_manager_ — all interior-mutable.


// @unsafe - Establishes TCP/IPC connection to server
// Contains syscalls, raw pointers, and other unsafe operations


// Attempts to reconnect to the last connected address. Authored as
// inline Rust DSL — the four helper lambdas here capture the enclosing
// scope BY REFERENCE, which non-move DSL closures now lower to
// (`[&]`), so the historical "closure-capture-by-ref" floor is gone.
// The param is named `self_`, not `self`: a DSL param named `self` is
// swallowed into a receiver and the body would emit `this->`.
#if RUSTYCPP_RUST
fn clientconn_reconnect(self_: &ClientConnection, on_complete: OnReconnectCompleteCallbackFn) -> i32 {
    // Reset the abort latch before delegating (folded in from the former
    // const `reconnect` facade): the Client::reconnect path needs a stale
    // abort=true from a prior close() cleared, and the close-fan-out spawn
    // path only reaches here with abort already false, so the reset is a
    // no-op there. `reconnect_` is a mutable atomic, so const self suffices.
    unsafe { self_.reconnect_.reconnect_abort_.store(false, rusty::sync::atomic::Ordering::Release); }

    let complete_callback = |result: i32| -> i32 {
        if on_complete {
            on_complete(result == 0i32);
        }
        result
    };

    let aborted: bool = unsafe { self_.reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire) };
    if aborted {
        return complete_callback(ECANCELED);
    }

    let wait_for_inflight_reconnect = || -> i32 {
        loop {
            let busy: bool = unsafe { self_.reconnect_.reconnecting_.load(rusty::sync::atomic::Ordering::Acquire) };
            if !busy {
                break;
            }
            let cancel: bool = unsafe { self_.reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire) };
            if cancel {
                return ECANCELED;
            }
            if self_.state_machine_.is_connected() {
                return 0i32;
            }
            Time::sleep(5000u64);
        }
        if self_.state_machine_.is_connected() {
            return 0i32;
        }
        INT_MIN
    };

    let reconnecting: bool = unsafe { self_.reconnect_.reconnecting_.load(rusty::sync::atomic::Ordering::Acquire) };
    if reconnecting {
        let waited: i32 = wait_for_inflight_reconnect();
        if waited != INT_MIN {
            return complete_callback(waited);
        }
    }

    // Check if we have an address to reconnect to
    if self_.reconnect_address_.get().empty() {
        log_line(Log::ERROR, 0i32, core::ptr::null(), std::format("rrr::ClientConnection: no address to reconnect to"));
        return complete_callback(EINVAL);
    }

    // Can only reconnect from FAILED or DISCONNECTED state
    if !self_.state_machine_.can_connect() {
        log_line(Log::ERROR, 0i32, core::ptr::null(), std::format("rrr::ClientConnection: cannot reconnect from state {}",
                  connection_state_to_string(self_.state_machine_.state())));
        return complete_callback(EINVAL);
    }

    loop {
        let mut expected: bool = false;
        let won: bool = unsafe {
            self_.reconnect_.reconnecting_.compare_exchange(expected, true,
                rusty::sync::atomic::Ordering::AcqRel,
                rusty::sync::atomic::Ordering::Acquire).is_ok()
        };
        if won {
            break;
        }
        let waited: i32 = wait_for_inflight_reconnect();
        if waited != INT_MIN {
            return complete_callback(waited);
        }
    }
    self_.invoke_reconnecting_callback();

    let complete_reconnect = |success: bool, result: i32| -> i32 {
        unsafe { self_.reconnect_.reconnecting_.store(false, rusty::sync::atomic::Ordering::Release); }
        self_.invoke_reconnected_callback(success);

        if success {
            log_line(Log::INFO, 0i32, core::ptr::null(), std::format("rrr::ClientConnection: reconnected to {}", self_.reconnect_address_.get().c_str()));

            // Record reconnection in metrics
            self_.metrics_.record_reconnect();

            // Sweep the disconnect-buffering queue. Entries that ran past
            // their TTL while the connection was down resolve their
            // futures with `kRequestQueueExpiredError` and bump
            // `queue_dropped_requests`. Non-stale entries remain in the
            // queue for a future replay path.
            self_.pending_queue_.expire_stale();
            return complete_callback(0i32);
        }
        if result == ECANCELED {
            log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("rrr::ClientConnection: reconnect cancelled for {}",
                      self_.reconnect_address_.get().c_str()));
        } else {
            log_line(Log::ERROR, 0i32, core::ptr::null(), std::format("rrr::ClientConnection: reconnection failed to {}: {}",
                      self_.reconnect_address_.get().c_str(), result));
        }
        complete_callback(result)
    };

    let reconnect_once = || -> i32 {
        let cancel: bool = unsafe { self_.reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire) };
        if cancel {
            return ECANCELED;
        }
        // 4g3c2: `socket_ = -1` reset removed. socket_ is unused in
        // channel mode (the channel proxy's TcpConnection owns the fd);
        // the `connect()` call below routes through `connect_via_factory`
        // which produces a fresh proxy + fresh fd internally.
        self_.connect(self_.reconnect_address_.get().c_str() as *const i8)
    };

    let abort_now: bool = unsafe { self_.reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire) };
    if abort_now {
        return complete_reconnect(false, ECANCELED);
    }

    // Another reconnect attempt can complete between the pre-CAS state check and
    // this thread acquiring reconnect ownership.
    if self_.state_machine_.is_connected() {
        return complete_reconnect(true, 0i32);
    }

    if !self_.state_machine_.can_connect() {
        return complete_reconnect(false, EINVAL);
    }

    // First attempt happens immediately.
    let mut result: i32 = reconnect_once();
    if result == 0i32 {
        return complete_reconnect(true, 0i32);
    }

    // Follow configured backoff/retry policy for subsequent attempts.
    let policy: ReconnectPolicy = self_.reconnect_policy_.get();
    let mut calc = ReconnectCalculator::new(policy);
    while calc.should_retry() {
        let cancel: bool = unsafe { self_.reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire) };
        if cancel {
            return complete_reconnect(false, ECANCELED);
        }

        let delay_ms: u32 = calc.next_delay_ms();
        if delay_ms > 0u32 {
            Time::sleep((delay_ms as u64) * 1000u64);
        }

        let cancel2: bool = unsafe { self_.reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire) };
        if cancel2 {
            return complete_reconnect(false, ECANCELED);
        }

        // Another path may have re-established connection while sleeping.
        if self_.state_machine_.is_connected() {
            return complete_reconnect(true, 0i32);
        }

        if !self_.state_machine_.can_connect() {
            return complete_reconnect(false, EINVAL);
        }

        log_line(Log::DEBUG, 0i32, core::ptr::null(), std::format("rrr::ClientConnection: reconnect retry #{} to {}",
                  calc.retry_count(), self_.reconnect_address_.get().c_str()));
        result = reconnect_once();
        if result == 0i32 {
            return complete_reconnect(true, 0i32);
        }
    }

    complete_reconnect(false, result)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.25 version=1 rust_sha256=ce84d76524c7cd8ced68f713d71d01a090130fc931c16bf6d2132f28bee98c7d*/
int32_t clientconn_reconnect(const ClientConnection& self_, OnReconnectCompleteCallbackFn on_complete) {
    // @unsafe
    {
        self_.reconnect_.reconnect_abort_.store(false, rusty::sync::atomic::Ordering::Release);
    }
    const auto complete_callback = [&](int32_t result) -> int32_t {
if (on_complete) {
    on_complete(rusty::detail::deref_if_pointer_like(result) == static_cast<int32_t>(0));
}
return std::move(result);
};
    const bool aborted = self_.reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire);
    if (aborted) {
        return complete_callback(ECANCELED);
    }
    const auto wait_for_inflight_reconnect = [&]() -> int32_t {
while (true) {
    const bool busy = self_.reconnect_.reconnecting_.load(rusty::sync::atomic::Ordering::Acquire);
    if (!busy) {
        break;
    }
    const bool cancel = self_.reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire);
    if (cancel) {
        return ECANCELED;
    }
    if (self_.state_machine_.is_connected()) {
        return static_cast<int32_t>(0);
    }
    Time::sleep(static_cast<uint64_t>(5000));
}
if (self_.state_machine_.is_connected()) {
    return static_cast<int32_t>(0);
}
return INT_MIN;
};
    const bool reconnecting = self_.reconnect_.reconnecting_.load(rusty::sync::atomic::Ordering::Acquire);
    if (reconnecting) {
        const int32_t waited = wait_for_inflight_reconnect();
        if (rusty::detail::deref_if_pointer_like(waited) != rusty::detail::deref_if_pointer_like(INT_MIN)) {
            return complete_callback(std::move(waited));
        }
    }
    if (self_.reconnect_address_.get().empty()) {
        log_line(rusty::clone(rusty::clone(Log::ERROR)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("rrr::ClientConnection: no address to reconnect to"));
        return complete_callback(EINVAL);
    }
    if (rusty::detail::rust_not(self_.state_machine_.can_connect())) {
        log_line(rusty::clone(rusty::clone(Log::ERROR)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("rrr::ClientConnection: cannot reconnect from state {}", connection_state_to_string(self_.state_machine_.state())));
        return complete_callback(EINVAL);
    }
    while (true) {
        bool expected = false;
        const bool won = self_.reconnect_.reconnecting_.compare_exchange(std::move(expected), true, rusty::sync::atomic::Ordering::AcqRel, rusty::sync::atomic::Ordering::Acquire).is_ok();
        if (won) {
            break;
        }
        const int32_t waited = wait_for_inflight_reconnect();
        if (rusty::detail::deref_if_pointer_like(waited) != rusty::detail::deref_if_pointer_like(INT_MIN)) {
            return complete_callback(std::move(waited));
        }
    }
    self_.invoke_reconnecting_callback();
    const auto complete_reconnect = [&](bool success, int32_t result) -> int32_t {
// @unsafe
{
    self_.reconnect_.reconnecting_.store(false, rusty::sync::atomic::Ordering::Release);
}
self_.invoke_reconnected_callback(std::move(success));
if (success) {
    log_line(rusty::clone(rusty::clone(Log::INFO)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("rrr::ClientConnection: reconnected to {}", self_.reconnect_address_.get().c_str()));
    self_.metrics_.record_reconnect();
    self_.pending_queue_.expire_stale();
    return complete_callback(static_cast<int32_t>(0));
}
if (rusty::detail::deref_if_pointer_like(result) == rusty::detail::deref_if_pointer_like(ECANCELED)) {
    log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("rrr::ClientConnection: reconnect cancelled for {}", self_.reconnect_address_.get().c_str()));
} else {
    log_line(rusty::clone(rusty::clone(Log::ERROR)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("rrr::ClientConnection: reconnection failed to {}: {}", self_.reconnect_address_.get().c_str(), std::move(result)));
}
return complete_callback(std::move(result));
};
    const auto reconnect_once = [&]() -> int32_t {
const bool cancel = self_.reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire);
if (cancel) {
    return ECANCELED;
}
return self_.connect(rusty::detail::ptr_cast<const int8_t*>(self_.reconnect_address_.get().c_str()));
};
    const bool abort_now = self_.reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire);
    if (abort_now) {
        return complete_reconnect(false, ECANCELED);
    }
    if (self_.state_machine_.is_connected()) {
        return complete_reconnect(true, static_cast<int32_t>(0));
    }
    if (rusty::detail::rust_not(self_.state_machine_.can_connect())) {
        return complete_reconnect(false, EINVAL);
    }
    int32_t result = reconnect_once();
    if (rusty::detail::deref_if_pointer_like(result) == static_cast<int32_t>(0)) {
        return complete_reconnect(true, static_cast<int32_t>(0));
    }
    ReconnectPolicy policy = self_.reconnect_policy_.get();
    auto calc = ReconnectCalculator::new_(std::move(policy));
    while (calc.should_retry()) {
        const bool cancel = self_.reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire);
        if (cancel) {
            return complete_reconnect(false, ECANCELED);
        }
        const uint32_t delay_ms = calc.next_delay_ms();
        if (rusty::detail::deref_if_pointer_like(delay_ms) > static_cast<uint32_t>(0)) {
            Time::sleep(((static_cast<uint64_t>(delay_ms))) * static_cast<uint64_t>(1000));
        }
        const bool cancel2 = self_.reconnect_.reconnect_abort_.load(rusty::sync::atomic::Ordering::Acquire);
        if (cancel2) {
            return complete_reconnect(false, ECANCELED);
        }
        if (self_.state_machine_.is_connected()) {
            return complete_reconnect(true, static_cast<int32_t>(0));
        }
        if (rusty::detail::rust_not(self_.state_machine_.can_connect())) {
            return complete_reconnect(false, EINVAL);
        }
        log_line(rusty::clone(rusty::clone(Log::DEBUG)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("rrr::ClientConnection: reconnect retry #{} to {}", calc.retry_count(), self_.reconnect_address_.get().c_str()));
        result = reconnect_once();
        if (rusty::detail::deref_if_pointer_like(result) == static_cast<int32_t>(0)) {
            return complete_reconnect(true, static_cast<int32_t>(0));
        }
    }
    return complete_reconnect(false, std::move(result));
}
/*RUSTYCPP:GEN-END id=client.25*/




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
#if RUSTYCPP_RUST
fn clientconn_request_via_channel<F>(conn: &ClientConnection, rpc_id: i32,
                                     attr: &FutureAttr, mut write_fn: F) -> FutureResult {
    if !conn.allow_request_with_circuit_metrics() {
        return FutureResult::Err(EBUSY);
    }
    conn.pending_queue_.expire_stale();
    if !conn.state_machine_.is_connected() {
        let buffering_cfg = conn.buffering_config_.get();
        if buffering_cfg.enabled && buffering_cfg.behavior == DisconnectBehavior::QUEUE {
            let fu = Future::create(conn.xid_counter_.next(1i64), attr);
            let fu_for_cb = fu.clone();
            let mut qr = QueuedRequest::new();
            qr.xid = (*fu).xid_;
            qr.rpc_id = rpc_id;
            qr.ttl_ms = buffering_cfg.default_ttl_ms;
            // Raw self-pointer capture (== the old [this]); the callback
            // outlives this call but not the connection.
            let conn_ptr: *const ClientConnection = &raw const *conn;
            let cb_fn = move |err: i32| {
                (*conn_ptr).metrics_.record_queue_drop();
                (*fu_for_cb).error_code_.set(err);
                (*fu_for_cb).notify_ready(fu_for_cb.clone());
            };
            qr.callback = cb_fn;
            if conn.pending_queue_.enqueue(qr) {
                return FutureResult::Ok(fu);
            }
            return FutureResult::Err(kRequestQueueRejectedError);
        }
        conn.record_circuit_result(ENOTCONN);
        return FutureResult::Err(ENOTCONN);
    }
    {
        let mut direct_guard = conn.direct_channel_.lock().unwrap();
        if (*direct_guard).is_some() {
            let proxy: &Box<ChannelConnectionBase> = (*direct_guard).as_ref().unwrap();
            if proxy.is_closed() {
                conn.record_circuit_result(ENOTCONN);
                return FutureResult::Err(ENOTCONN);
            }
        } else {
            let guard2 = conn.fiber_channel_.lock().unwrap();
            let mut chan_dead = (*guard2).is_none();
            if !chan_dead {
                let fc: &Box<FiberChannel> = (*guard2).as_ref().unwrap();
                if fc.is_closed() {
                    chan_dead = true;
                }
            }
            if chan_dead {
                conn.record_circuit_result(ENOTCONN);
                return FutureResult::Err(ENOTCONN);
            }
        }
    }

    let fu = Future::create(conn.xid_counter_.next(1i64), attr);
    {
        let mut pending_guard = conn.pending_fu_.lock().unwrap();
        (*pending_guard).insert((*fu).xid_, fu.clone());
    }

    // sconn_reply's archive shape: aggregate literals + the &mut alias
    // (bare reference args pass as lvalues where a by-value local would
    // be move-wrapped at its last use).
    let mut body_sink: BufferSink = BufferSink { bytes: Vec::<u8>::new() };
    let mut ar_store = BinaryWriteArchive { sink_: make_sink_proxy(&raw mut body_sink) };
    let ar: &mut BinaryWriteArchive = &mut ar_store;
    Serialize_::serialize(v64::new((*fu).xid_), ar);
    Serialize_::serialize(rpc_id, ar);
    write_fn(ar);

    let ch_err = conn.dispatch_frame_via_channel(body_sink.bytes.as_ptr(),
                                                 body_sink.bytes.len());
    if ch_err != ChannelError::None {
        {
            let mut pending_guard2 = conn.pending_fu_.lock().unwrap();
            (*pending_guard2).remove((*fu).xid_);
        }
        conn.record_circuit_result(EIO);
        return FutureResult::Err(EIO);
    }

    conn.metrics_.record_request_sent();
    conn.on_request_dispatched(body_sink.bytes.len());
    FutureResult::Ok(fu)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.23 version=1 rust_sha256=b577481cb6c86338451ae76a9b1ae7b6a04bcdf2d840fb62fd843d62e41823c6*/
template<typename F>
FutureResult clientconn_request_via_channel(const ClientConnection& conn, int32_t rpc_id, const FutureAttr& attr, F write_fn) {
    if (rusty::detail::rust_not(conn.allow_request_with_circuit_metrics())) {
        return FutureResult::Err(EBUSY);
    }
    conn.pending_queue_.expire_stale();
    if (rusty::detail::rust_not(conn.state_machine_.is_connected())) {
        const auto buffering_cfg = conn.buffering_config_.get();
        if (rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.enabled); }) { return (__r.enabled); } else if constexpr (requires { (__r.enabled_field); }) { return (__r.enabled_field); } else if constexpr (requires { ((*__r).enabled); }) { return ((*__r).enabled); } else { return ((*__r).enabled_field); } }(buffering_cfg)) && (rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.behavior); }) { return (__r.behavior); } else if constexpr (requires { (__r.behavior_field); }) { return (__r.behavior_field); } else if constexpr (requires { ((*__r).behavior); }) { return ((*__r).behavior); } else { return ((*__r).behavior_field); } }(buffering_cfg)) == rusty::clone(DisconnectBehavior_QUEUE()))) {
            auto fu = Future::create(conn.xid_counter_.next(static_cast<int64_t>(1)), attr);
            auto fu_for_cb = rusty::clone(fu);
            auto qr = QueuedRequest::new_();
            qr.xid = (rusty::detail::deref_if_pointer_like(fu)).xid_;
            qr.rpc_id = std::move(rpc_id);
            qr.ttl_ms = std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.default_ttl_ms); }) { return (__r.default_ttl_ms); } else if constexpr (requires { (__r.default_ttl_ms_field); }) { return (__r.default_ttl_ms_field); } else if constexpr (requires { ((*__r).default_ttl_ms); }) { return ((*__r).default_ttl_ms); } else { return ((*__r).default_ttl_ms_field); } }(buffering_cfg));
            const ClientConnection* conn_ptr = &conn;
            auto cb_fn = [=, conn_ptr = std::move(conn_ptr), fu_for_cb = std::move(fu_for_cb)](int32_t err) mutable {
(*conn_ptr).metrics_.record_queue_drop();
(rusty::detail::deref_if_pointer_like(fu_for_cb)).error_code_.set(std::move(err));
((rusty::detail::deref_if_pointer_like(fu_for_cb))).notify_ready(rusty::clone(fu_for_cb));
};
            qr.callback = std::move(cb_fn);
            if (conn.pending_queue_.enqueue(std::move(qr))) {
                return FutureResult::Ok(std::move(fu));
            }
            return FutureResult::Err(std::move(kRequestQueueRejectedError));
        }
        conn.record_circuit_result(ENOTCONN);
        return FutureResult::Err(ENOTCONN);
    }
    {
        auto&& direct_guard = rusty::deref_call(conn.direct_channel_.lock(), rusty::detail::__mdisp_unwrap{});
        if (((rusty::detail::deref_if_pointer_like(direct_guard))).is_some()) {
            const rusty::Box<ChannelConnectionBase>& proxy = ((rusty::detail::deref_if_pointer_like(direct_guard))).as_ref().unwrap();
            if (proxy->is_closed()) {
                conn.record_circuit_result(ENOTCONN);
                return FutureResult::Err(ENOTCONN);
            }
        } else {
            const auto&& guard2 = rusty::deref_call(conn.fiber_channel_.lock(), rusty::detail::__mdisp_unwrap{});
            auto chan_dead = ((rusty::detail::deref_if_pointer_like(guard2))).is_none();
            if (rusty::detail::rust_not(chan_dead)) {
                const rusty::Box<FiberChannel>& fc = ((rusty::detail::deref_if_pointer_like(guard2))).as_ref().unwrap();
                if (fc->is_closed()) {
                    chan_dead = true;
                }
            }
            if (chan_dead) {
                conn.record_circuit_result(ENOTCONN);
                return FutureResult::Err(ENOTCONN);
            }
        }
    }
    auto fu = Future::create(conn.xid_counter_.next(static_cast<int64_t>(1)), attr);
    {
        auto&& pending_guard = rusty::deref_call(conn.pending_fu_.lock(), rusty::detail::__mdisp_unwrap{});
        ((rusty::detail::deref_if_pointer_like(pending_guard))).insert((rusty::detail::deref_if_pointer_like(fu)).xid_, rusty::clone(fu));
    }
    BufferSink body_sink = BufferSink{.bytes = std::conditional_t<true, rusty::Vec<uint8_t>, F>::new_()};
    auto ar_store = BinaryWriteArchive{.sink_ = make_sink_proxy(&body_sink)};
    BinaryWriteArchive& ar = ar_store;
    Serialize_::serialize(v64::new_((rusty::detail::deref_if_pointer_like(fu)).xid_), ar);
    Serialize_::serialize(rpc_id, ar);
    write_fn(ar);
    const auto ch_err = conn.dispatch_frame_via_channel(rusty::as_ptr(body_sink.bytes), rusty::len(body_sink.bytes));
    if (rusty::detail::deref_if_pointer_like(ch_err) != rusty::detail::deref_if_pointer_like(ChannelError::None)) {
        {
            auto&& pending_guard2 = rusty::deref_call(conn.pending_fu_.lock(), rusty::detail::__mdisp_unwrap{});
            ((rusty::detail::deref_if_pointer_like(pending_guard2))).remove((rusty::detail::deref_if_pointer_like(fu)).xid_);
        }
        conn.record_circuit_result(EIO);
        return FutureResult::Err(EIO);
    }
    conn.metrics_.record_request_sent();
    conn.on_request_dispatched(rusty::len(body_sink.bytes));
    return FutureResult::Ok(std::move(fu));
}
/*RUSTYCPP:GEN-END id=client.23*/

// Slim async-callback request (no Arc<Future>, no HashMap node) — the
// slot-based twin of request_via_channel, same DSL shapes throughout.
#if RUSTYCPP_RUST
fn clientconn_request_async<F>(conn: &ClientConnection, rpc_id: i32,
                               mut write_fn: F, mut on_reply: AsyncReplyCallback)
                               -> Result<(), i32> {
    if !conn.allow_request_with_circuit_metrics() {
        return Result::<(), i32>::Err(EBUSY);
    }
    if !conn.state_machine_.is_connected() {
        conn.record_circuit_result(ENOTCONN);
        return Result::<(), i32>::Err(ENOTCONN);
    }
    {
        let mut direct_guard = conn.direct_channel_.lock().unwrap();
        if (*direct_guard).is_some() {
            let proxy: &Box<ChannelConnectionBase> = (*direct_guard).as_ref().unwrap();
            if proxy.is_closed() {
                conn.record_circuit_result(ENOTCONN);
                return Result::<(), i32>::Err(ENOTCONN);
            }
        } else {
            let guard2 = conn.fiber_channel_.lock().unwrap();
            let mut chan_dead = (*guard2).is_none();
            if !chan_dead {
                let fc: &Box<FiberChannel> = (*guard2).as_ref().unwrap();
                if fc.is_closed() {
                    chan_dead = true;
                }
            }
            if chan_dead {
                conn.record_circuit_result(ENOTCONN);
                return Result::<(), i32>::Err(ENOTCONN);
            }
        }
    }

    let xid: i64 = conn.xid_counter_.next(1i64);
    let slot: usize = (xid as usize) % kAsyncSlotCount;
    {
        let mut guard = conn.pending_cb_slots_.lock().unwrap();
        if (*guard)[slot].is_some() {
            conn.record_circuit_result(EBUSY);
            return Result::<(), i32>::Err(EBUSY);
        }
        (*guard)[slot] = Some(on_reply);
    }

    let mut body_sink: BufferSink = BufferSink { bytes: Vec::<u8>::new() };
    let mut ar_store = BinaryWriteArchive { sink_: make_sink_proxy(&raw mut body_sink) };
    let ar: &mut BinaryWriteArchive = &mut ar_store;
    Serialize_::serialize(v64::new(xid), ar);
    Serialize_::serialize(rpc_id, ar);
    write_fn(ar);

    let ch_err = conn.dispatch_frame_via_channel(body_sink.bytes.as_ptr(),
                                                 body_sink.bytes.len());
    if ch_err != ChannelError::None {
        let mut guard = conn.pending_cb_slots_.lock().unwrap();
        (*guard)[slot] = None;
        conn.record_circuit_result(EIO);
        return Result::<(), i32>::Err(EIO);
    }
    conn.metrics_.record_request_sent();
    conn.on_request_dispatched(body_sink.bytes.len());
    Result::<(), i32>::Ok(())
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.24 version=1 rust_sha256=11aab555f20808485d1db946928fcf899505bb0218e926310db5a68f6daa722e*/
template<typename F>
rusty::Result<rusty::Unit, int32_t> clientconn_request_async(const ClientConnection& conn, int32_t rpc_id, F write_fn, AsyncReplyCallback on_reply) {
    if (rusty::detail::rust_not(conn.allow_request_with_circuit_metrics())) {
        return rusty::Result<rusty::Unit, int32_t>::Err(EBUSY);
    }
    if (rusty::detail::rust_not(conn.state_machine_.is_connected())) {
        conn.record_circuit_result(ENOTCONN);
        return rusty::Result<rusty::Unit, int32_t>::Err(ENOTCONN);
    }
    {
        auto&& direct_guard = rusty::deref_call(conn.direct_channel_.lock(), rusty::detail::__mdisp_unwrap{});
        if (((rusty::detail::deref_if_pointer_like(direct_guard))).is_some()) {
            const rusty::Box<ChannelConnectionBase>& proxy = ((rusty::detail::deref_if_pointer_like(direct_guard))).as_ref().unwrap();
            if (proxy->is_closed()) {
                conn.record_circuit_result(ENOTCONN);
                return rusty::Result<rusty::Unit, int32_t>::Err(ENOTCONN);
            }
        } else {
            const auto&& guard2 = rusty::deref_call(conn.fiber_channel_.lock(), rusty::detail::__mdisp_unwrap{});
            auto chan_dead = ((rusty::detail::deref_if_pointer_like(guard2))).is_none();
            if (rusty::detail::rust_not(chan_dead)) {
                const rusty::Box<FiberChannel>& fc = ((rusty::detail::deref_if_pointer_like(guard2))).as_ref().unwrap();
                if (fc->is_closed()) {
                    chan_dead = true;
                }
            }
            if (chan_dead) {
                conn.record_circuit_result(ENOTCONN);
                return rusty::Result<rusty::Unit, int32_t>::Err(ENOTCONN);
            }
        }
    }
    const int64_t xid = conn.xid_counter_.next(static_cast<int64_t>(1));
    const size_t slot = ((static_cast<size_t>(xid))) % rusty::detail::deref_if_pointer_like(kAsyncSlotCount);
    {
        auto&& guard = rusty::deref_call(conn.pending_cb_slots_.lock(), rusty::detail::__mdisp_unwrap{});
        if ((rusty::detail::deref_if_pointer_like(guard))[slot].is_some()) {
            conn.record_circuit_result(EBUSY);
            return rusty::Result<rusty::Unit, int32_t>::Err(EBUSY);
        }
        (rusty::detail::deref_if_pointer_like(guard))[slot] = rusty::Option<AsyncReplyCallback>(std::move(on_reply));
    }
    BufferSink body_sink = BufferSink{.bytes = std::conditional_t<true, rusty::Vec<uint8_t>, F>::new_()};
    auto ar_store = BinaryWriteArchive{.sink_ = make_sink_proxy(&body_sink)};
    BinaryWriteArchive& ar = ar_store;
    Serialize_::serialize(v64::new_(std::move(xid)), ar);
    Serialize_::serialize(rpc_id, ar);
    write_fn(ar);
    const auto ch_err = conn.dispatch_frame_via_channel(rusty::as_ptr(body_sink.bytes), rusty::len(body_sink.bytes));
    if (rusty::detail::deref_if_pointer_like(ch_err) != rusty::detail::deref_if_pointer_like(ChannelError::None)) {
        auto&& guard = rusty::deref_call(conn.pending_cb_slots_.lock(), rusty::detail::__mdisp_unwrap{});
        (rusty::detail::deref_if_pointer_like(guard))[slot] = rusty::None;
        conn.record_circuit_result(EIO);
        return rusty::Result<rusty::Unit, int32_t>::Err(EIO);
    }
    conn.metrics_.record_request_sent();
    conn.on_request_dispatched(rusty::len(body_sink.bytes));
    return rusty::Result<rusty::Unit, int32_t>::Ok(std::make_tuple());
}
/*RUSTYCPP:GEN-END id=client.24*/

// The two former @unsafe kernels for the DSL below are now DSL
// themselves; both stated causes expired. (A third,
// classify_request_failure, used to sit here purely to host an
// `#if EWOULDBLOCK != EAGAIN`; it is now DSL, inside the block below.)
#if RUSTYCPP_RUST
// @safe - BinaryWriteArchive stopped being "a hand-written type with a
// real C++ constructor" when serializable.cpp made it a single-field DSL
// aggregate, so a struct literal builds it — the same literal the three
// other archive sites in this file already spell inline. The parameter
// stays `*mut BufferSink` (not `&mut`) so the emitted signature keeps a
// POINTER, which is what the caller's `&mut args_sink` lowers to.
fn make_write_archive(sink: *mut BufferSink) -> BinaryWriteArchive {
    BinaryWriteArchive { sink_: make_sink_proxy(sink) }
}

// @unsafe - copies the attempt's unread reply region into the coordinator
// future's buffer. Two simultaneous RefCell borrows (two DISTINCT
// Futures, so no re-entrant borrow) plus a raw sub-slice of the borrowed
// body — spelled exactly as clientconn_decode_response_and_notify below
// already spells the same fill: `ptr::add` + `core::slice::from_raw_parts`
// inside `unsafe`, which is what retired the "span has no DSL form"
// excuse. Takes REFERENCES, not pointers: `&Arc<Future>` lowers to
// `const rusty::Arc<Future>&`, and the caller's `&attempt_fu` collapses to
// the handle itself.
fn request_copy_reply(final_fu: &Arc<Future>, attempt_fu: &Arc<Future>) {
    let mut attempt_reply = (*attempt_fu).reply_.borrow_mut();
    let reply_size: usize = (*attempt_reply).src.remaining();
    if reply_size > 0usize {
        let base: *const u8 = (*attempt_reply).body.data();
        let start: usize = (*attempt_reply).src.pos();
        let mut final_reply = (*final_fu).reply_.borrow_mut();
        reply_buffer_fill(&mut *final_reply, unsafe {
            core::slice::from_raw_parts(base.add(start), reply_size)
        });
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.36 version=1 rust_sha256=2ccf4a743ef549a76c2a2464e1b00791f975be29ca5da2dc298ee97f9565d97a*/
BinaryWriteArchive make_write_archive(BufferSink* sink) {
    return BinaryWriteArchive{.sink_ = make_sink_proxy(sink)};
}

void request_copy_reply(const rusty::Arc<Future>& final_fu, const rusty::Arc<Future>& attempt_fu) {
    auto&& attempt_reply = (rusty::detail::deref_if_pointer_like(attempt_fu)).reply_.borrow_mut();
    const size_t reply_size = (rusty::detail::deref_if_pointer_like(attempt_reply)).src.remaining();
    if (rusty::detail::deref_if_pointer_like(reply_size) > static_cast<size_t>(0)) {
        const uint8_t* base = (rusty::detail::deref_if_pointer_like(attempt_reply)).body.data();
        const size_t start = (rusty::detail::deref_if_pointer_like(attempt_reply)).src.pos();
        auto&& final_reply = (rusty::detail::deref_if_pointer_like(final_fu)).reply_.borrow_mut();
        reply_buffer_fill(rusty::detail::deref_if_pointer_like(final_reply), rusty::from_raw_parts(rusty::ptr::add(base, std::move(start)), std::move(reply_size)));
    }
}
/*RUSTYCPP:GEN-END id=client.36*/

// Same as request_via_channel, plus serialize-once for safe retry replay
// and an async retry/backoff spawn. Extracted from the inline
// request_with_options(rpc_id, options, attr, write_fn) template method.
//
// Authored as inline Rust DSL. The 2026-08-03 "KERNEL by verdict" note
// listed four floors; three have since dissolved and the fourth moved:
// std::chrono is gone from this file (Time::now/Time::sleep), non-move
// closures now lower to real `[&]` captures (which is what
// finish_terminal/set_terminal_timeout need to mutate `retry_count`
// across the retry loop), the replay payload stays a Vec<u8> so the
// reinterpret_cast string build disappears, and the preprocessor
// conditional is gone entirely — classify_request_failure is now DSL
// in this same block (see the note on its body below).
//
// Idiom note: a callable argument must be passed through a NAMED
// `&mut` binding (`let ar_ref: &mut BinaryWriteArchive = &mut ar;`) —
// spelling `write_fn(&mut ar)` lowers to a POINTER argument, which will
// not bind to the `BinaryWriteArchive&` the callables take.
#if RUSTYCPP_RUST
// Pure classification of an errno; captures nothing. The original
// `#if EWOULDBLOCK != EAGAIN` guard existed only to avoid a duplicate
// switch case label; in an if-else `|| err == EWOULDBLOCK` is a
// harmless redundancy on Linux (EAGAIN == EWOULDBLOCK) and keeps the
// intent without a preprocessor conditional — exactly the reshape
// already shipped in clientconn_map_system_error above. It lives in
// THIS block rather than one of its own so the call below stays a
// same-block call.
fn classify_request_failure(err: i32) -> TimeoutType {
    if err == ENOTCONN || err == ECONNREFUSED || err == ECONNRESET
        || err == ECONNABORTED || err == EHOSTUNREACH || err == ENETUNREACH {
        return TimeoutType::CONNECT_TIMEOUT;
    }
    if err == ETIMEDOUT || err == EAGAIN || err == EWOULDBLOCK {
        return TimeoutType::REQUEST_TIMEOUT;
    }
    TimeoutType::NONE
}

fn clientconn_request_with_options<F>(self_: &ClientConnection, rpc_id: i32,
                                      options: &RequestOptions,
                                      attr: &FutureAttr, write_fn: F) -> FutureResult {
    // Serialize args once so retries can replay identical payload safely.
    let mut args_sink: BufferSink = Default::default();
    let mut ar: BinaryWriteArchive = make_write_archive(&mut args_sink);
    let ar_ref: &mut BinaryWriteArchive = &mut ar;
    write_fn(ar_ref);
    // Keep the replay payload as bytes (was a reinterpret_cast'd
    // std::string round-trip).
    let args_bytes: rusty::Vec<u8> = args_sink.bytes.clone();

    // Non-idempotent operations must never be retried even if max_retries is set.
    let mut effective_options: RequestOptions = *options;
    if !effective_options.idempotent {
        effective_options.max_retries = 0u16;
    }

    // Return a coordinator future immediately; internal attempts run async.
    let final_fu: rusty::Arc<Future> = Future::create(self_.xid_counter_.next(1), *attr);
    let mut waiter_options: RequestOptions = effective_options;
    waiter_options.timeout_ms = 0u64;  // Internal attempts own timeout behavior.
    (*final_fu).set_options(waiter_options);

    let weak_conn = self_.weak_self_.clone();
    // The spawned closure MOVES what it captures, so the coordinator
    // future needs its own handle: without this clone the `move ||`
    // capture leaves the `Ok(final_fu)` below returning a moved-from
    // (null) Arc. The hand-written original captured `final_fu` by copy.
    let final_fu_task: rusty::Arc<Future> = final_fu.clone();
    rusty::thread::spawn(move || {
        let start_us: u64 = Time::now(true);
        let mut retry_count: u16 = 0u16;

        let finish_terminal = |err: i32, timeout_type: TimeoutType| {
            let conn_opt = weak_conn.upgrade();
            if conn_opt.is_some() {
                let conn = conn_opt.unwrap();
                if timeout_type == TimeoutType::CONNECT_TIMEOUT
                    || timeout_type == TimeoutType::REQUEST_TIMEOUT
                    || timeout_type == TimeoutType::RESPONSE_TIMEOUT
                    || timeout_type == TimeoutType::TOTAL_TIMEOUT {
                    (*conn).metrics_.record_request_timeout();
                } else if err != 0i32 {
                    (*conn).metrics_.record_request_failed();
                }
            }
            if timeout_type != TimeoutType::NONE {
                let mut state_guard = (*final_fu_task).state_.lock().unwrap();
                (*state_guard).timed_out = true;
            }
            (*final_fu_task).error_code_.set(err);
            (*final_fu_task).timeout_type_.set(timeout_type);
            (*final_fu_task).retry_count_.set(retry_count);
            (*final_fu_task).notify_ready(final_fu_task.clone());
        };

        let set_terminal_timeout = |timeout_type: TimeoutType| {
            finish_terminal(ETIMEDOUT, timeout_type);
        };

        loop {
            let elapsed_ms: u64 = (Time::now(true) - start_us) / 1000u64;
            if effective_options.is_total_timeout_exceeded(elapsed_ms) {
                set_terminal_timeout(TimeoutType::TOTAL_TIMEOUT);
                return;
            }

            let conn_opt = weak_conn.upgrade();
            if conn_opt.is_none() {
                finish_terminal(ENOTCONN, TimeoutType::CONNECT_TIMEOUT);
                return;
            }

            let conn = conn_opt.unwrap();
            let replay = |m: &mut BinaryWriteArchive| {
                if !args_bytes.is_empty() {
                    (*m).write_bytes(args_bytes.as_ptr(), args_bytes.len());
                }
            };
            // (Default::default() infers only in typed-let position, not
            // as a bare argument.)
            let empty_attr: FutureAttr = Default::default();
            let attempt_result = (*conn).request(rpc_id, empty_attr, replay);
            if attempt_result.is_err() {
                let err: i32 = attempt_result.unwrap_err();
                finish_terminal(err, classify_request_failure(err));
                return;
            }

            let attempt_fu: rusty::Arc<Future> = attempt_result.unwrap();
            let mut attempt_options: RequestOptions = effective_options;
            if effective_options.total_timeout_ms > 0u64 {
                let remaining_ms: u64 = effective_options.remaining_time_ms(elapsed_ms);
                if remaining_ms == 0u64 {
                    (*conn).handle_free((*attempt_fu).xid_);
                    set_terminal_timeout(TimeoutType::TOTAL_TIMEOUT);
                    return;
                }
                if attempt_options.timeout_ms == 0u64 || attempt_options.timeout_ms > remaining_ms {
                    attempt_options.timeout_ms = remaining_ms;
                }
            }
            (*attempt_fu).set_options(attempt_options);
            if (*attempt_fu).wait_with_options() {
                (*final_fu_task).error_code_.set((*attempt_fu).error_code_.get());
                (*final_fu_task).retry_count_.set(retry_count);
                if (*attempt_fu).error_code_.get() == 0i32 {
                    request_copy_reply(&final_fu_task, &attempt_fu);
                }
                (*final_fu_task).notify_ready(final_fu_task.clone());
                return;
            }

            // Timed-out attempts are no longer useful; release pending map slot.
            (*conn).handle_free((*attempt_fu).xid_);

            if !effective_options.can_retry(retry_count) {
                set_terminal_timeout((*attempt_fu).get_timeout_type());
                return;
            }

            (*conn).metrics_.record_retry_attempt();
            let backoff_delay_ms: u64 = effective_options.calculate_delay_ms(retry_count);
            if backoff_delay_ms > 0u64 {
                if effective_options.total_timeout_ms > 0u64 {
                    let elapsed_before_sleep: u64 = (Time::now(true) - start_us) / 1000u64;
                    let remaining_ms: u64 = effective_options.remaining_time_ms(elapsed_before_sleep);
                    if remaining_ms == 0u64 || backoff_delay_ms >= remaining_ms {
                        set_terminal_timeout(TimeoutType::TOTAL_TIMEOUT);
                        return;
                    }
                }
                Time::sleep(backoff_delay_ms * 1000u64);
            }

            retry_count += 1u16;
            (*final_fu_task).retry_count_.set(retry_count);
        }
    }).detach();

    FutureResult::Ok(final_fu)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.33 version=1 rust_sha256=bfd38431bb3150ebf50d4ace2cf62e6bb1bfde910f40ad4d7855ba7eea697495*/
TimeoutType classify_request_failure(int32_t err) {
    if ((((((rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ENOTCONN)) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ECONNREFUSED))) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ECONNRESET))) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ECONNABORTED))) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EHOSTUNREACH))) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ENETUNREACH))) {
        return rusty::clone(TimeoutType::CONNECT_TIMEOUT);
    }
    if (((rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ETIMEDOUT)) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EAGAIN))) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EWOULDBLOCK))) {
        return rusty::clone(TimeoutType::REQUEST_TIMEOUT);
    }
    return rusty::clone(rusty::clone(TimeoutType::NONE));
}

template<typename F>
FutureResult clientconn_request_with_options(const ClientConnection& self_, int32_t rpc_id, const RequestOptions& options, const FutureAttr& attr, F write_fn) {
    BufferSink args_sink = rusty::default_like<BufferSink>();
    BinaryWriteArchive ar = make_write_archive(&args_sink);
    BinaryWriteArchive& ar_ref = ar;
    write_fn(ar_ref);
    rusty::Vec<uint8_t> args_bytes = rusty::clone(args_sink.bytes);
    RequestOptions effective_options = options;
    if (rusty::detail::rust_not(effective_options.idempotent)) {
        effective_options.max_retries = static_cast<uint16_t>(0);
    }
    rusty::Arc<Future> final_fu = Future::create(self_.xid_counter_.next(1), attr);
    RequestOptions waiter_options = effective_options;
    waiter_options.timeout_ms = static_cast<uint64_t>(0);
    ((rusty::detail::deref_if_pointer_like(final_fu))).set_options(std::move(waiter_options));
    auto weak_conn = rusty::clone(self_.weak_self_);
    rusty::Arc<Future> final_fu_task = rusty::clone(final_fu);
    rusty::thread::spawn([=, args_bytes = std::move(args_bytes), effective_options = std::move(effective_options), final_fu_task = std::move(final_fu_task), rpc_id = std::move(rpc_id), weak_conn = std::move(weak_conn)]() mutable {
const uint64_t start_us = Time::now(true);
uint16_t retry_count = static_cast<uint16_t>(0);
const auto finish_terminal = [&](int32_t err, TimeoutType timeout_type) {
auto conn_opt = weak_conn.upgrade();
if (conn_opt.is_some()) {
    const auto conn = conn_opt.unwrap();
    if ((((rusty::detail::deref_if_pointer_like(timeout_type) == rusty::clone(TimeoutType::CONNECT_TIMEOUT)) || (rusty::detail::deref_if_pointer_like(timeout_type) == rusty::clone(TimeoutType::REQUEST_TIMEOUT))) || (rusty::detail::deref_if_pointer_like(timeout_type) == rusty::clone(TimeoutType::RESPONSE_TIMEOUT))) || (rusty::detail::deref_if_pointer_like(timeout_type) == rusty::clone(TimeoutType::TOTAL_TIMEOUT))) {
        (rusty::detail::deref_if_pointer_like(conn)).metrics_.record_request_timeout();
    } else if (rusty::detail::deref_if_pointer_like(err) != static_cast<int32_t>(0)) {
        (rusty::detail::deref_if_pointer_like(conn)).metrics_.record_request_failed();
    }
}
if (rusty::detail::deref_if_pointer_like(timeout_type) != rusty::clone(TimeoutType::NONE)) {
    auto&& state_guard = rusty::deref_call((rusty::detail::deref_if_pointer_like(final_fu_task)).state_.lock(), rusty::detail::__mdisp_unwrap{});
    (rusty::detail::deref_if_pointer_like(state_guard)).timed_out = true;
}
(rusty::detail::deref_if_pointer_like(final_fu_task)).error_code_.set(std::move(err));
(rusty::detail::deref_if_pointer_like(final_fu_task)).timeout_type_.set(std::move(timeout_type));
(rusty::detail::deref_if_pointer_like(final_fu_task)).retry_count_.set(std::move(retry_count));
((rusty::detail::deref_if_pointer_like(final_fu_task))).notify_ready(rusty::clone(final_fu_task));
};
const auto set_terminal_timeout = [&](TimeoutType timeout_type) {
finish_terminal(ETIMEDOUT, std::move(timeout_type));
};
while (true) {
    const uint64_t elapsed_ms = ((Time::now(true) - rusty::detail::deref_if_pointer_like(start_us))) / static_cast<uint64_t>(1000);
    if (effective_options.is_total_timeout_exceeded(std::move(elapsed_ms))) {
        set_terminal_timeout(rusty::clone(rusty::clone(TimeoutType::TOTAL_TIMEOUT)));
        return;
    }
    auto conn_opt = weak_conn.upgrade();
    if (conn_opt.is_none()) {
        finish_terminal(ENOTCONN, rusty::clone(rusty::clone(TimeoutType::CONNECT_TIMEOUT)));
        return;
    }
    const auto conn = conn_opt.unwrap();
    const auto replay = [&](BinaryWriteArchive& m) {
if (rusty::detail::rust_not(rusty::is_empty(args_bytes))) {
    ((m)).write_bytes(rusty::as_ptr(args_bytes), rusty::len(args_bytes));
}
};
    const FutureAttr empty_attr = rusty::default_like<FutureAttr>();
    auto attempt_result = ((rusty::detail::deref_if_pointer_like(conn))).request(std::move(rpc_id), std::move(empty_attr), std::move(replay));
    if (attempt_result.is_err()) {
        int32_t err = attempt_result.unwrap_err();
        finish_terminal(std::move(err), classify_request_failure(std::move(err)));
        return;
    }
    const rusty::Arc<Future> attempt_fu = attempt_result.unwrap();
    RequestOptions attempt_options = effective_options;
    if (rusty::detail::deref_if_pointer_like(effective_options.total_timeout_ms) > static_cast<uint64_t>(0)) {
        uint64_t remaining_ms = effective_options.remaining_time_ms(std::move(elapsed_ms));
        if (rusty::detail::deref_if_pointer_like(remaining_ms) == static_cast<uint64_t>(0)) {
            ((rusty::detail::deref_if_pointer_like(conn))).handle_free((rusty::detail::deref_if_pointer_like(attempt_fu)).xid_);
            set_terminal_timeout(rusty::clone(rusty::clone(TimeoutType::TOTAL_TIMEOUT)));
            return;
        }
        if ((rusty::detail::deref_if_pointer_like(attempt_options.timeout_ms) == static_cast<uint64_t>(0)) || (rusty::detail::deref_if_pointer_like(attempt_options.timeout_ms) > rusty::detail::deref_if_pointer_like(remaining_ms))) {
            attempt_options.timeout_ms = std::move(remaining_ms);
        }
    }
    ((rusty::detail::deref_if_pointer_like(attempt_fu))).set_options(std::move(attempt_options));
    if (((rusty::detail::deref_if_pointer_like(attempt_fu))).wait_with_options()) {
        (rusty::detail::deref_if_pointer_like(final_fu_task)).error_code_.set((rusty::detail::deref_if_pointer_like(attempt_fu)).error_code_.get());
        (rusty::detail::deref_if_pointer_like(final_fu_task)).retry_count_.set(std::move(retry_count));
        if ((rusty::detail::deref_if_pointer_like(attempt_fu)).error_code_.get() == static_cast<int32_t>(0)) {
            request_copy_reply(final_fu_task, attempt_fu);
        }
        ((rusty::detail::deref_if_pointer_like(final_fu_task))).notify_ready(rusty::clone(final_fu_task));
        return;
    }
    ((rusty::detail::deref_if_pointer_like(conn))).handle_free((rusty::detail::deref_if_pointer_like(attempt_fu)).xid_);
    if (rusty::detail::rust_not(effective_options.can_retry(std::move(retry_count)))) {
        set_terminal_timeout(((rusty::detail::deref_if_pointer_like(attempt_fu))).get_timeout_type());
        return;
    }
    (rusty::detail::deref_if_pointer_like(conn)).metrics_.record_retry_attempt();
    const uint64_t backoff_delay_ms = effective_options.calculate_delay_ms(std::move(retry_count));
    if (rusty::detail::deref_if_pointer_like(backoff_delay_ms) > static_cast<uint64_t>(0)) {
        if (rusty::detail::deref_if_pointer_like(effective_options.total_timeout_ms) > static_cast<uint64_t>(0)) {
            const uint64_t elapsed_before_sleep = ((Time::now(true) - rusty::detail::deref_if_pointer_like(start_us))) / static_cast<uint64_t>(1000);
            const uint64_t remaining_ms = effective_options.remaining_time_ms(std::move(elapsed_before_sleep));
            if ((rusty::detail::deref_if_pointer_like(remaining_ms) == static_cast<uint64_t>(0)) || (rusty::detail::deref_if_pointer_like(backoff_delay_ms) >= rusty::detail::deref_if_pointer_like(remaining_ms))) {
                set_terminal_timeout(rusty::clone(rusty::clone(TimeoutType::TOTAL_TIMEOUT)));
                return;
            }
        }
        Time::sleep(rusty::detail::deref_if_pointer_like(backoff_delay_ms) * static_cast<uint64_t>(1000));
    }
    retry_count += static_cast<uint16_t>(1);
    (rusty::detail::deref_if_pointer_like(final_fu_task)).retry_count_.set(std::move(retry_count));
}
}).detach();
    return FutureResult::Ok(std::move(final_fu));
}
/*RUSTYCPP:GEN-END id=client.33*/

// Dispatch one frame body through the bound channel proxy.
//
// 4g1c: direct-channel binding takes precedence over the FiberChannel
// binding (only one is bound at a time per ClientConnection
// lifecycle). Sends run under the slot's lock here (unlike the
// server's reply path) — both bindings' send_frame are brief.
#if RUSTYCPP_RUST
fn clientconn_dispatch_frame_via_channel(conn: &ClientConnection,
                                         body_bytes: *const u8,
                                         body_size: usize) -> ChannelError {
    if !conn.channel_mode_.get() {
        return ChannelError_ConnectionReset();
    }
    {
        let mut guard = conn.direct_channel_.lock().unwrap();
        if (*guard).is_some() {
            let p: &mut Box<ChannelConnectionBase> = (*guard).as_mut().unwrap();
            return p.send_frame(ChannelFrame { payload: body_bytes, size: body_size });
        }
    }
    let mut guard2 = conn.fiber_channel_.lock().unwrap();
    if (*guard2).is_none() {
        return ChannelError_ConnectionReset();
    }
    let p2: &mut Box<FiberChannel> = (*guard2).as_mut().unwrap();
    p2.send_frame(ChannelFrame { payload: body_bytes, size: body_size })
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.16 version=1 rust_sha256=35ba0b3851cd9cb41fb82deb8ba68f14d4cb5f6ef5619835225057055e0a7a04*/
ChannelError clientconn_dispatch_frame_via_channel(const ClientConnection& conn, const uint8_t* body_bytes, size_t body_size) {
    if (rusty::detail::rust_not(conn.channel_mode_.get())) {
        return ChannelError_ConnectionReset();
    }
    {
        auto&& guard = rusty::deref_call(conn.direct_channel_.lock(), rusty::detail::__mdisp_unwrap{});
        if (((rusty::detail::deref_if_pointer_like(guard))).is_some()) {
            rusty::Box<ChannelConnectionBase>& p = ((rusty::detail::deref_if_pointer_like(guard))).as_mut().unwrap();
            return p->send_frame(ChannelFrame{.payload = body_bytes, .size = std::move(body_size)});
        }
    }
    auto&& guard2 = rusty::deref_call(conn.fiber_channel_.lock(), rusty::detail::__mdisp_unwrap{});
    if (((rusty::detail::deref_if_pointer_like(guard2))).is_none()) {
        return ChannelError_ConnectionReset();
    }
    rusty::Box<FiberChannel>& p2 = ((rusty::detail::deref_if_pointer_like(guard2))).as_mut().unwrap();
    return p2->send_frame(ChannelFrame{.payload = body_bytes, .size = std::move(body_size)});
}
/*RUSTYCPP:GEN-END id=client.16*/

// @unsafe - Enqueue one internal heartbeat probe through the bound
// channel proxy.
//
// 4g3c3: legacy fd path removed. Channel mode is the only path; the
// `out_` Marshal that backed the fd path is gone. Callers (the
// poll-loop tick) only fire heartbeats on connected clients, which
// always have a bound channel by construction.
#if RUSTYCPP_RUST
fn clientconn_enqueue_heartbeat_probe(conn: &ClientConnection) {
    // Build the heartbeat frame body and dispatch through the channel
    // proxy. Same archive shape as the server's sconn_reply: aggregate
    // struct literals + the &mut alias so serialize's Archive& binds.
    let mut body_sink: BufferSink = BufferSink { bytes: Vec::<u8>::new() };
    let mut ar_store = BinaryWriteArchive { sink_: make_sink_proxy(&raw mut body_sink) };
    let ar: &mut BinaryWriteArchive = &mut ar_store;
    Serialize_::serialize(v64::new(conn.xid_counter_.next(1i64)), ar);
    Serialize_::serialize(kInternalHeartbeatRpcId as i32, ar);
    // Send-side errors are ignored here (same as the legacy fd path).
    let _ = conn.dispatch_frame_via_channel(body_sink.bytes.as_ptr(),
                                            body_sink.bytes.len());
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.17 version=1 rust_sha256=f4c3301fe04c220beb004d902035b761603958f4dd27a55c394bbb0c8aefffc3*/
void clientconn_enqueue_heartbeat_probe(const ClientConnection& conn) {
    BufferSink body_sink = BufferSink{.bytes = rusty::Vec<uint8_t>::new_()};
    auto ar_store = BinaryWriteArchive{.sink_ = make_sink_proxy(&body_sink)};
    BinaryWriteArchive& ar = ar_store;
    Serialize_::serialize(v64::new_(conn.xid_counter_.next(static_cast<int64_t>(1))), ar);
    Serialize_::serialize(static_cast<int32_t>(kInternalHeartbeatRpcId), ar);
    static_cast<void>(conn.dispatch_frame_via_channel(rusty::as_ptr(body_sink.bytes), rusty::len(body_sink.bytes)));
}
/*RUSTYCPP:GEN-END id=client.17*/


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
// @unsafe - strlen over the reinterpret_cast'ed addr; owned string for
// the DSL body. The old excuse ("`*const i8` has no char* spelling in the
// DSL") is EXPIRED — `as *const c_char` lowers to the same
// `reinterpret_cast<const char*>`, and `std::string(p, strlen(p))` is a
// plain DSL call expression. The one bit of scaffolding is the alias: a
// DSL `*const char` lowers to `const char32_t*` (Rust's `char` is a
// 32-bit scalar), so the byte-pointer spelling has to come from C++.
using c_char = char;
#if RUSTYCPP_RUST
fn clientconn_addr_to_string(addr: *const i8) -> std::string {
    let p: *const c_char = addr as *const c_char;
    std::string(p, strlen(p))
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.34 version=1 rust_sha256=78aadfdf5cbfe810d389571f608fa97c82e0a98bbb84893b3f115ffbe6785209*/
std::string clientconn_addr_to_string(const int8_t* addr);

std::string clientconn_addr_to_string(const int8_t* addr) {
    const c_char* p = reinterpret_cast<const c_char*>(addr);
    return std::string(p, strlen(p));
}
/*RUSTYCPP:GEN-END id=client.34*/

// The factory-driven connect. The factory is used IN PLACE through
// the Box while the rusty::Mutex guard is held — the proxy is
// move-only (no clone), connect is synchronous per the channel-layer
// contract, and the factory is effectively read-only after
// bind_factory, so briefly holding the lock across the syscall does
// not contend with the dispatch path (which locks fiber_channel_).
// 4g1c: on success bind_channel_direct installs on_frame/on_closed
// directly on the proxy — no FiberChannel, no recv-loop fiber.
#if RUSTYCPP_RUST
fn clientconn_connect_via_factory(conn: &ClientConnection, addr_i8: *const i8) -> i32 {
    let addr_str: std::string = clientconn_addr_to_string(addr_i8);
    {
        let mut guard = conn.factory_.lock().unwrap();
        if (*guard).is_none() {
            log_line(Log::ERROR, 0i32, core::ptr::null(), std::format("rrr::ClientConnection::connect_via_factory: factory unbound at the moment of connect (race against bind_factory)"));
            conn.state_machine_.transition_to(ConnectionState::FAILED);
            conn.invoke_error_callback(ENOTCONN, "factory unbound");
            return ENOTCONN;
        }
        let bound: &mut Box<ChannelFactoryBase> = (*guard).as_mut().unwrap();
        let mut result: ConnectResult = bound.connect(addr_str.clone());
        if result.error != ChannelError::None || result.connection.is_none() {
            let err_name = channel_error_to_string(result.error);
            let err_str: std::string = format!("factory connect failed: {}", err_name);
            log_line(Log::ERROR, 0i32, core::ptr::null(), std::format("rrr::ClientConnection: {} (addr={})", err_str, addr_str));
            conn.state_machine_.transition_to(ConnectionState::FAILED);
            // Map the channel error onto an errno-shaped value the
            // legacy call sites expect.
            let mut rc: i32 = ENOTCONN;
            if result.error == ChannelError::ConnectionRefused {
                rc = ECONNREFUSED;
            } else if result.error == ChannelError::AddressInvalid {
                rc = EINVAL;
            }
            conn.invoke_error_callback(rc, err_str);
            return rc;
        }
        let mut conn_proxy = result.connection.take().unwrap();
        conn.bind_channel_direct(conn_proxy);
    }

    // Record address for the close fan-out's reconnect spawn — it
    // re-runs the factory connect with the same target.
    conn.reconnect_address_.set(addr_str);

    // Mirror the fd path's terminal transition: the channel layer's
    // own state (proxy.is_closed()) becomes the source of truth, but
    // we still drive the legacy state machine through CONNECTED so
    // existing health-check / metric APIs keep working.
    if !conn.state_machine_.transition_to(ConnectionState::CONNECTED) {
        conn.state_machine_.force_state(ConnectionState::CONNECTED);
    }
    // Record connect timestamp so metrics_.connect_time_ms() is
    // non-zero from the moment a request can be issued; seed
    // last_activity_time_ so is_idle() measures time since connect.
    {
        let now: u64 = clientconn_monotonic_ms_now();
        conn.metrics_.record_connect(now);
        conn.update_last_activity(now);
    }
    conn.invoke_connected_callback();
    0i32
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.21 version=1 rust_sha256=e36d68397ad899d0beca96a9041a86a50d101ae4bba04b58e43fa9977e31e0d8*/
int32_t clientconn_connect_via_factory(const ClientConnection& conn, const int8_t* addr_i8) {
    std::string addr_str = clientconn_addr_to_string(addr_i8);
    {
        auto&& guard = rusty::deref_call(conn.factory_.lock(), rusty::detail::__mdisp_unwrap{});
        if (((rusty::detail::deref_if_pointer_like(guard))).is_none()) {
            log_line(rusty::clone(rusty::clone(Log::ERROR)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("rrr::ClientConnection::connect_via_factory: factory unbound at the moment of connect (race against bind_factory)"));
            conn.state_machine_.transition_to(rusty::clone(rusty::clone(ConnectionState::FAILED)));
            conn.invoke_error_callback(ENOTCONN, "factory unbound");
            return ENOTCONN;
        }
        rusty::Box<ChannelFactoryBase>& bound = ((rusty::detail::deref_if_pointer_like(guard))).as_mut().unwrap();
        ConnectResult result = bound->connect(rusty::clone(addr_str));
        if ((rusty::detail::deref_if_pointer_like(result.error) != rusty::detail::deref_if_pointer_like(ChannelError::None)) || result.connection.is_none()) {
            const auto err_name = channel_error_to_string(std::move(result.error));
            const std::string err_str = std::format("factory connect failed: {}" , err_name);
            log_line(rusty::clone(rusty::clone(Log::ERROR)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("rrr::ClientConnection: {} (addr={})", std::move(err_str), std::move(addr_str)));
            conn.state_machine_.transition_to(rusty::clone(rusty::clone(ConnectionState::FAILED)));
            int32_t rc = ENOTCONN;
            if (rusty::detail::deref_if_pointer_like(result.error) == rusty::detail::deref_if_pointer_like(ChannelError::ConnectionRefused)) {
                rc = ECONNREFUSED;
            } else if (rusty::detail::deref_if_pointer_like(result.error) == rusty::detail::deref_if_pointer_like(ChannelError::AddressInvalid)) {
                rc = EINVAL;
            }
            conn.invoke_error_callback(std::move(rc), std::move(err_str));
            return std::move(rc);
        }
        auto conn_proxy = result.connection.take().unwrap();
        conn.bind_channel_direct(std::move(conn_proxy));
    }
    conn.reconnect_address_.set(std::move(addr_str));
    if (rusty::detail::rust_not(conn.state_machine_.transition_to(rusty::clone(rusty::clone(ConnectionState::CONNECTED))))) {
        conn.state_machine_.force_state(rusty::clone(rusty::clone(ConnectionState::CONNECTED)));
    }
    {
        const uint64_t now = clientconn_monotonic_ms_now();
        conn.metrics_.record_connect(std::move(now));
        conn.update_last_activity(std::move(now));
    }
    conn.invoke_connected_callback();
    return static_cast<int32_t>(0);
}
/*RUSTYCPP:GEN-END id=client.21*/


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


// @unsafe - Channel-mode bind that schedules the recv-loop fiber
// spawn onto the *poll thread*.
//
// Used by production code paths (factory-driven `connect` /
// reconnect) that run on the user thread but need the recv-loop
// fiber on the poll thread — same thread the channel proxy's
// callbacks fire on. Submits a `OneTimeJob` whose `Work()` runs
// `run_recv_loop()` from a fiber that the poll thread's
// `trigger_job` spawns on its own reactor.
// @unsafe - heap-constructs the FiberChannel wrapper around the moved
// proxy. The old note here ("cross-file #[cpp_ctor] construction ...
// so the boxed construction stays a 3-line kernel") is STALE: the DSL
// never needed to NAME FiberChannel's cpp_ctor factory -- `make_box`
// forwards straight to the real C++ constructor, and
// `rusty::make_box::<T>(x)` lowers verbatim to
// `rusty::make_box<T>(std::move(x))`.
//
// `Box::<T>::new(T::new(x))` is NOT the spelling to use here: it lowers
// to `rusty::Box<T>::new_(FiberChannel::new_(...))`, and `new_` does
// not exist for a #[cpp_ctor] type.
//
// The former `inline` is dropped: this fn sits in `rrr.client`'s
// non-exported impl namespace (module linkage, single module unit) with
// exactly one caller (clientconn_bind_channel_via_poll_thread, below),
// so no other TU could ever define it and the vague-linkage specifier
// bought nothing.
#if RUSTYCPP_RUST
fn clientconn_make_fiber_channel(ch: ChannelConnectionProxy) -> rusty::Box<FiberChannel> {
    rusty::make_box::<FiberChannel>(ch)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.37 version=1 rust_sha256=00b46e808a7daf8c16201eb8c013e160e8db131469cdf47337d19e2ad802caa2*/
rusty::Box<FiberChannel> clientconn_make_fiber_channel(ChannelConnectionProxy ch) {
    return rusty::make_box<FiberChannel>(std::move(ch));
}
/*RUSTYCPP:GEN-END id=client.37*/

// The recv-job body, as a free fn so the OneTimeJob closure stays the
// single-call shape (see the inference-bug note at the closure site).
#if RUSTYCPP_RUST
fn clientconn_recv_job_entry(weak_self: WeakClientConnection) {
    let conn_opt = weak_self.upgrade();
    if conn_opt.is_some() {
        let c = conn_opt.unwrap();
        (*c).run_recv_loop();
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.20 version=1 rust_sha256=ecb683d6581300d207099562ed5d476a2aa62546a04eae5774a3f6b8077eb5a8*/
void clientconn_recv_job_entry(WeakClientConnection weak_self) {
    auto conn_opt = weak_self.upgrade();
    if (conn_opt.is_some()) {
        const auto c = conn_opt.unwrap();
        ((rusty::detail::deref_if_pointer_like(c))).run_recv_loop();
    }
}
/*RUSTYCPP:GEN-END id=client.20*/

#if RUSTYCPP_RUST
fn clientconn_bind_channel_via_poll_thread(conn: &ClientConnection,
                                           mut channel: ChannelConnectionProxy) {
    if !channel.is_valid() {
        return;
    }
    // Move the proxy into the heap-allocated FiberChannel and flip the
    // latch on the calling thread — pure data mutations; the recv-loop
    // fiber doesn't observe them until the OneTimeJob below is
    // submitted. bind_callbacks() runs after the Box address is final.
    {
        let mut guard = conn.fiber_channel_.lock().unwrap();
        *guard = Some(clientconn_make_fiber_channel(channel));
        let fc: &mut Box<FiberChannel> = (*guard).as_mut().unwrap();
        fc.bind_callbacks();
    }
    conn.channel_mode_.set(true);

    let weak_self: WeakClientConnection = conn.weak_self_.clone();

    // Schedule the recv-loop fiber spawn onto the poll thread. The
    // poll thread's `trigger_job` calls `Fiber::create_run` from its
    // own reactor, so the resulting fiber's IntEvent waits and the
    // `on_frame` callback's signal both land on the same thread.
    // (The closure is bound to a local first: the inline-argument
    // closure path mis-infers a return type here — the ::new_ note at
    // ClientProxy::close — while the let-bound path emits it clean.)
    let job_fn = move || {
        clientconn_recv_job_entry(weak_self);
    };
    let recv_job: Arc<OneTimeJob> =
        Arc::<OneTimeJob>::new_(OneTimeJob::new_(job_fn));
    // Implicit Arc<OneTimeJob> -> Arc<Job> upcast for the queue.
    let pt: &Arc<PollThread> = &conn.poll_thread_worker_;
    pt.add(recv_job);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.19 version=1 rust_sha256=4380d1d29c7b3ffce51fd856d343113b2169cb864fb4b54a9d285c83db2ba92f*/
void clientconn_bind_channel_via_poll_thread(const ClientConnection& conn, ChannelConnectionProxy channel) {
    if (rusty::detail::rust_not(channel.is_valid())) {
        return;
    }
    {
        auto&& guard = rusty::deref_call(conn.fiber_channel_.lock(), rusty::detail::__mdisp_unwrap{});
        rusty::detail::deref_if_pointer_like(guard) = rusty::Some(clientconn_make_fiber_channel(std::move(channel)));
        rusty::Box<FiberChannel>& fc = ((rusty::detail::deref_if_pointer_like(guard))).as_mut().unwrap();
        fc->bind_callbacks();
    }
    conn.channel_mode_.set(true);
    WeakClientConnection weak_self = rusty::clone(conn.weak_self_);
    auto job_fn = [=, weak_self = std::move(weak_self)]() {
clientconn_recv_job_entry(std::move(weak_self));
};
    const rusty::Arc<OneTimeJob> recv_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob::new_(std::move(job_fn)));
    const rusty::Arc<PollThread>& pt = conn.poll_thread_worker_;
    pt->add(std::move(recv_job));
}
/*RUSTYCPP:GEN-END id=client.19*/




// @unsafe - Drives Marshal / Future / pending_fu_ from a fiber.
//
// Recv-loop body: blocks on `FiberChannel::recv_frame()` and forwards
// each frame's body to `decode_response_and_notify`. Returns when
// the channel closes (recv_frame returns None) or when the wrapper
// goes away.
//
// We resolve the FiberChannel raw pointer ONCE under a brief lock
// and then drop the rusty::Mutex guard — `recv_frame()` yields the
// fiber (parking on an `IntEvent`), and holding a lock across the
// yield would block other threads racing on `dispatch_frame_via_channel`
// (or, on the same reactor, prevent other fibers from running). The
// raw pointer stays valid because the spawning lambda keeps an
// `Arc<ClientConnection>` alive for the fiber's lifetime, and the
// connection owns the `Box<FiberChannel>`.
// @unsafe - Box::get raw extraction for the DSL loop below (the
// pointer must outlive the guard — recv_frame() parks the fiber, and
// holding the lock across the yield would block dispatch_frame racers;
// the spawning lambda's Arc<ClientConnection> keeps the Box alive).
// The hand-written body's `const_cast` was a NO-OP — `rusty::Box<T>::get()
// const` (box.hpp:465) already returns a non-const `T*` — so the DSL body
// is a plain `as_ref().unwrap().get()`.
#if RUSTYCPP_RUST
fn clientconn_fiber_channel_ptr(slot: &Option<Box<FiberChannel>>) -> *mut FiberChannel {
    slot.as_ref().unwrap().get()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.35 version=1 rust_sha256=ce9057c513ff92f0887a5c09e8ab701471c8fd1f96a8a3e52e14ef27a24cbb0e*/
FiberChannel* clientconn_fiber_channel_ptr(const rusty::Option<rusty::Box<FiberChannel>>& slot) {
    return slot.as_ref().unwrap().get();
}
/*RUSTYCPP:GEN-END id=client.35*/

#if RUSTYCPP_RUST
fn clientconn_run_recv_loop(conn: &ClientConnection) {
    let mut fc: *mut FiberChannel = core::ptr::null_mut();
    {
        let guard = conn.fiber_channel_.lock().unwrap();
        if (*guard).is_none() {
            return;
        }
        fc = clientconn_fiber_channel_ptr((*guard));
    }
    loop {
        let frame_opt: Option<OwnedFrame> = (*fc).recv_frame();
        if frame_opt.is_none() {
            // Channel closed. Run the close-side fan-out (sub-leaf 4d):
            // cancel pending futures with ENOTCONN, fire error /
            // disconnected callbacks, and trigger auto-reconnect if the
            // policy allows. The fiber then exits, dropping its
            // Arc<ClientConnection> capture.
            conn.on_channel_closed_fan_out();
            return;
        }
        let frame = frame_opt.unwrap();
        conn.decode_response_and_notify(frame.bytes.as_ptr(), frame.bytes.len());
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.18 version=1 rust_sha256=b0efddd535fa0ea387b7a621195020bbbe3b6affa88d29d295ebfd72d949c592*/
void clientconn_run_recv_loop(const ClientConnection& conn) {
    FiberChannel* fc = rusty::ptr::null_mut();
    {
        const auto&& guard = rusty::deref_call(conn.fiber_channel_.lock(), rusty::detail::__mdisp_unwrap{});
        if (((rusty::detail::deref_if_pointer_like(guard))).is_none()) {
            return;
        }
        fc = clientconn_fiber_channel_ptr((rusty::detail::deref_if_pointer_like(guard)));
    }
    while (true) {
        rusty::Option<OwnedFrame> frame_opt = ((*fc)).recv_frame();
        if (frame_opt.is_none()) {
            conn.on_channel_closed_fan_out();
            return;
        }
        const auto frame = frame_opt.unwrap();
        conn.decode_response_and_notify(rusty::as_ptr(frame.bytes), rusty::len(frame.bytes));
    }
}
/*RUSTYCPP:GEN-END id=client.18*/


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


// Decode one response frame body and resolve the matching pending
// slot. Header parse goes directly over the input bytes via
// BufferSource + BinaryReadArchive (no intermediate Marshal); a
// truncated frame aborts inside read_exact, matching the legacy
// short-read behaviour. Fast path first: the slim async-callback slot
// (request_async users) avoids touching the future map entirely.
#if RUSTYCPP_RUST
fn clientconn_decode_response_and_notify(conn: &ClientConnection,
                                         bytes: *const u8, size: usize) {
    // Account for every inbound frame body byte and bump the activity
    // clock so metrics_.bytes_received() and is_idle() reflect real
    // I/O regardless of which dispatch slot the reply maps onto.
    conn.on_response_received(size);
    let mut src = BufferSource::new(bytes, size);
    let mut ar = BinaryReadArchive { source_: make_source_proxy(&raw mut src) };

    let mut v_reply_xid = v64::new(0i64);
    let mut v_error_code = v32::new(0i32);
    // In channel mode the extended-header flag is consumed by the
    // framing layer; the server always emits the extended form.
    let mut v_server_instance_id = v64::new(0i64);
    Deserialize_::deserialize(&mut v_reply_xid, ar);
    Deserialize_::deserialize(&mut v_error_code, ar);
    Deserialize_::deserialize(&mut v_server_instance_id, ar);
    conn.check_server_instance(v_server_instance_id.get() as u64);

    let parsed_header_size: usize = src.pos();
    let response_payload_bytes: usize = size - parsed_header_size;
    conn.heartbeat_manager_.on_pong_received();

    {
        let slot: usize = (v_reply_xid.get() as usize) % kAsyncSlotCount;
        let mut cb_opt: Option<AsyncReplyCallback> = None;
        {
            let mut guard = conn.pending_cb_slots_.lock().unwrap();
            if (*guard)[slot].is_some() {
                cb_opt = core::mem::take(&mut (*guard)[slot]);
            }
        }
        if cb_opt.is_some() {
            let mut cb = cb_opt.unwrap();
            let err_code: i32 = v_error_code.get();
            if err_code == 0i32 {
                conn.metrics_.record_request_completed();
            } else {
                conn.metrics_.record_request_failed();
            }
            conn.record_circuit_result(err_code);
            cb(err_code, bytes.add(parsed_header_size),
               response_payload_bytes);
            return;
        }
    }

    let mut fu_opt: Option<Arc<Future>> = None;
    {
        let mut guard = conn.pending_fu_.lock().unwrap();
        let fu_ptr = (*guard).get(v_reply_xid.get());
        if fu_ptr.is_some() {
            fu_opt = Some(fu_ptr.unwrap().clone());
            (*guard).remove(v_reply_xid.get());
        }
    }

    if fu_opt.is_some() {
        let fu = fu_opt.unwrap();
        verify((*fu).xid_ == v_reply_xid.get());
        (*fu).error_code_.set(v_error_code.get());
        if response_payload_bytes > 0usize {
            let mut rb_guard = (*fu).reply_.borrow_mut();
            reply_buffer_fill(&mut *rb_guard, unsafe {
                core::slice::from_raw_parts(
                    bytes.add(parsed_header_size),
                    response_payload_bytes)
            });
        }
        if v_error_code.get() == 0i32 {
            conn.metrics_.record_request_completed();
        } else {
            conn.metrics_.record_request_failed();
        }
        conn.record_circuit_result(v_error_code.get());
        (*fu).notify_ready(fu.clone());
    }
    // No matching future (timed out or replaced) -> drop the payload.
    // With channel-mode framing the input bytes are owned by the
    // caller and freed on return -- nothing to drain.
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.22 version=1 rust_sha256=eb40ae721af9096c9e1348fa5af3e13bf10956dea31dff320a98a45a05512073*/
void clientconn_decode_response_and_notify(const ClientConnection& conn, const uint8_t* bytes, size_t size) {
    conn.on_response_received(std::move(size));
    auto src = BufferSource::new_(bytes, std::move(size));
    auto ar = BinaryReadArchive{.source_ = make_source_proxy(&src)};
    auto v_reply_xid = v64::new_(static_cast<int64_t>(0));
    auto v_error_code = v32::new_(static_cast<int32_t>(0));
    auto v_server_instance_id = v64::new_(static_cast<int64_t>(0));
    Deserialize_::deserialize(v_reply_xid, ar);
    Deserialize_::deserialize(v_error_code, ar);
    Deserialize_::deserialize(v_server_instance_id, ar);
    conn.check_server_instance(static_cast<uint64_t>(v_server_instance_id.get()));
    const size_t parsed_header_size = src.pos();
    const size_t response_payload_bytes = rusty::detail::deref_if_pointer_like(size) - rusty::detail::deref_if_pointer_like(parsed_header_size);
    conn.heartbeat_manager_.on_pong_received();
    {
        const size_t slot = ((static_cast<size_t>(v_reply_xid.get()))) % rusty::detail::deref_if_pointer_like(kAsyncSlotCount);
        rusty::Option<AsyncReplyCallback> cb_opt = rusty::Option<AsyncReplyCallback>{rusty::None};
        {
            auto&& guard = rusty::deref_call(conn.pending_cb_slots_.lock(), rusty::detail::__mdisp_unwrap{});
            if ((rusty::detail::deref_if_pointer_like(guard))[slot].is_some()) {
                cb_opt = rusty::mem::take((rusty::detail::deref_if_pointer_like(guard))[slot]);
            }
        }
        if (cb_opt.is_some()) {
            auto cb = cb_opt.unwrap();
            const int32_t err_code = v_error_code.get();
            if (rusty::detail::deref_if_pointer_like(err_code) == static_cast<int32_t>(0)) {
                conn.metrics_.record_request_completed();
            } else {
                conn.metrics_.record_request_failed();
            }
            conn.record_circuit_result(std::move(err_code));
            cb(std::move(err_code), rusty::ptr::add(bytes, std::move(parsed_header_size)), std::move(response_payload_bytes));
            return;
        }
    }
    rusty::Option<rusty::Arc<Future>> fu_opt = rusty::Option<rusty::Arc<Future>>{rusty::None};
    {
        auto&& guard = rusty::deref_call(conn.pending_fu_.lock(), rusty::detail::__mdisp_unwrap{});
        auto fu_ptr = ((rusty::detail::deref_if_pointer_like(guard))).get(v_reply_xid.get());
        if (fu_ptr.is_some()) {
            fu_opt = rusty::Option<rusty::Arc<Future>>(rusty::clone(fu_ptr.unwrap()));
            ((rusty::detail::deref_if_pointer_like(guard))).remove(v_reply_xid.get());
        }
    }
    if (fu_opt.is_some()) {
        const auto fu = fu_opt.unwrap();
        verify(rusty::detail::deref_if_pointer_like((rusty::detail::deref_if_pointer_like(fu)).xid_) == v_reply_xid.get());
        (rusty::detail::deref_if_pointer_like(fu)).error_code_.set(v_error_code.get());
        if (rusty::detail::deref_if_pointer_like(response_payload_bytes) > static_cast<size_t>(0)) {
            auto&& rb_guard = (rusty::detail::deref_if_pointer_like(fu)).reply_.borrow_mut();
            reply_buffer_fill(rusty::detail::deref_if_pointer_like(rb_guard), rusty::from_raw_parts(rusty::ptr::add(bytes, std::move(parsed_header_size)), std::move(response_payload_bytes)));
        }
        if (v_error_code.get() == static_cast<int32_t>(0)) {
            conn.metrics_.record_request_completed();
        } else {
            conn.metrics_.record_request_failed();
        }
        conn.record_circuit_result(v_error_code.get());
        ((rusty::detail::deref_if_pointer_like(fu))).notify_ready(rusty::clone(fu));
    }
}
/*RUSTYCPP:GEN-END id=client.22*/

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

// @safe - Checks whether an error should contribute to circuit tripping.


// @safe - Maps errno-style errors into structured RpcError categories.
// Authored as inline Rust DSL (module-scope free fn mirroring
// clientconn_monotonic_ms_now / should_trip_circuit_for_error): the
// `#if RUSTYCPP_RUST` block is the source of truth; the transpiler
// regenerates the GEN block below. The original switch's
// `#if EWOULDBLOCK != EAGAIN` guard existed only to avoid a duplicate case
// label; in an if-else `|| err == EWOULDBLOCK` is a harmless redundancy on
// Linux (EAGAIN == EWOULDBLOCK) and keeps the intent without a preprocessor.
#if RUSTYCPP_RUST
fn clientconn_map_system_error(err: i32) -> RpcError {
    if err == 0i32 { return RpcError::OK; }
    if err == ENOTCONN { return RpcError::NOT_CONNECTED; }
    if err == ECONNREFUSED { return RpcError::CONNECTION_REFUSED; }
    if err == ECONNRESET { return RpcError::CONNECTION_RESET; }
    if err == ENETUNREACH { return RpcError::NETWORK_UNREACHABLE; }
    if err == EHOSTUNREACH { return RpcError::HOST_UNREACHABLE; }
    if err == ECONNABORTED || err == EPIPE { return RpcError::CONNECTION_CLOSED; }
    if err == EBUSY { return RpcError::CIRCUIT_OPEN; }
    if err == ETIMEDOUT { return RpcError::RESPONSE_TIMEOUT; }
    if err == EAGAIN || err == EWOULDBLOCK { return RpcError::REQUEST_TIMEOUT; }
    if err == EINVAL { return RpcError::INVALID_ARGUMENT; }
    RpcError::UNKNOWN_ERROR
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.map_system_error version=1 rust_sha256=07653e63f46045f528908f0c74b92dd871ad83e789db92417a1873b3e69c420d*/
RpcError clientconn_map_system_error(int32_t err) {
    if (rusty::detail::deref_if_pointer_like(err) == static_cast<int32_t>(0)) {
        return rusty::clone(RpcError::OK);
    }
    if (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ENOTCONN)) {
        return rusty::clone(RpcError::NOT_CONNECTED);
    }
    if (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ECONNREFUSED)) {
        return rusty::clone(RpcError::CONNECTION_REFUSED);
    }
    if (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ECONNRESET)) {
        return rusty::clone(RpcError::CONNECTION_RESET);
    }
    if (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ENETUNREACH)) {
        return rusty::clone(RpcError::NETWORK_UNREACHABLE);
    }
    if (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EHOSTUNREACH)) {
        return rusty::clone(RpcError::HOST_UNREACHABLE);
    }
    if ((rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ECONNABORTED)) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EPIPE))) {
        return rusty::clone(RpcError::CONNECTION_CLOSED);
    }
    if (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EBUSY)) {
        return rusty::clone(RpcError::CIRCUIT_OPEN);
    }
    if (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(ETIMEDOUT)) {
        return rusty::clone(RpcError::RESPONSE_TIMEOUT);
    }
    if ((rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EAGAIN)) || (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EWOULDBLOCK))) {
        return rusty::clone(RpcError::REQUEST_TIMEOUT);
    }
    if (rusty::detail::deref_if_pointer_like(err) == rusty::detail::deref_if_pointer_like(EINVAL)) {
        return rusty::clone(RpcError::INVALID_ARGUMENT);
    }
    return rusty::clone(rusty::clone(RpcError::UNKNOWN_ERROR));
}
/*RUSTYCPP:GEN-END id=client.map_system_error*/




// 4g3c3: ClientConnection no longer implements the Pollable role.
// The channel layer's TcpConnection owns the fd and the
// handle_read/write/error duty. These overrides remain for ABI
// compatibility (PollableProxy facade conformance via the templated
// adapter) but their bodies are no-ops.



// ============================================================================
// ClientPool implementation
// ============================================================================

// Pure predicate over a config snapshot + Client metrics — no shared
// state touched, so this one carries none of the pool's unwrap-copy
// hazard (which lives in the map-walking helpers below).
#if RUSTYCPP_RUST
fn clientpool_is_client_healthy_with(cfg: PoolConfig, client: &rusty::Arc<Client>) -> bool {
    if !cfg.health_check_enabled {
        return true;
    }
    if !(*client).connected() {
        return false;
    }
    let requests_sent: u64 = (*client).metrics().requests_sent();
    if requests_sent < cfg.min_requests_for_health {
        return true;
    }
    let success_rate: u64 = (*client).metrics().success_rate_percent();
    success_rate >= cfg.unhealthy_threshold_percent
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.26 version=1 rust_sha256=e08d5a2d942633516a58801155559ac18403638e228e3c3fa262c54abc8983a8*/
bool clientpool_is_client_healthy_with(PoolConfig cfg, const rusty::Arc<Client>& client) {
    if (rusty::detail::rust_not(cfg.health_check_enabled)) {
        return true;
    }
    if (rusty::detail::rust_not(((rusty::detail::deref_if_pointer_like(client))).connected())) {
        return false;
    }
    const uint64_t requests_sent = ((rusty::detail::deref_if_pointer_like(client))).metrics().requests_sent();
    if (rusty::detail::deref_if_pointer_like(requests_sent) < rusty::detail::deref_if_pointer_like(cfg.min_requests_for_health)) {
        return true;
    }
    const uint64_t success_rate = ((rusty::detail::deref_if_pointer_like(client))).metrics().success_rate_percent();
    return rusty::detail::deref_if_pointer_like(success_rate) >= rusty::detail::deref_if_pointer_like(cfg.unhealthy_threshold_percent);
}
/*RUSTYCPP:GEN-END id=client.26*/

// @safe - rusty::Mutex::lock + BTreeMap ops + is_client_healthy are all @safe.
// One-step typed unwrap (§7.37): `let clients: &Vec<..> = opt.unwrap()` binds
// a reference. The earlier untyped `let clients = opt.unwrap()` lowered to a
// Vec COPY and corrupted the cached Arcs — that hazard is why this fn stayed
// hand-written; ASan-gated now.
#if RUSTYCPP_RUST
fn clientpool_get_healthy_client_count(self_: &ClientPool, addr: &std::string) -> usize {
    // Config snapshot BEFORE `state_`, per the lock-order invariant.
    let cfg: PoolConfig = self_.pool_config();
    let guard = self_.state_.lock().unwrap();
    let mut count: usize = 0usize;
    let clients_opt = (*guard).cache.get(addr);
    if clients_opt.is_some() {
        let clients: &Vec<Arc<Client>> = clients_opt.unwrap();
        let mut i: usize = 0usize;
        while i < (*clients).len() {
            if clientpool_is_client_healthy_with(cfg, &(*clients)[i]) {
                count += 1usize;
            }
            i += 1usize;
        }
    }
    count
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.27 version=1 rust_sha256=d509ce8ecb1a0303fc4af634115a461eac9a2964128e17eaf011b35ed0162694*/
size_t clientpool_get_healthy_client_count(const ClientPool& self_, const std::string& addr) {
    const PoolConfig cfg = self_.pool_config();
    const auto&& guard = rusty::deref_call(self_.state_.lock(), rusty::detail::__mdisp_unwrap{});
    size_t count = static_cast<size_t>(0);
    auto clients_opt = (rusty::detail::deref_if_pointer_like(guard)).cache.get(addr);
    if (clients_opt.is_some()) {
        const rusty::Vec<rusty::Arc<Client>>& clients = clients_opt.unwrap();
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::len((clients))) {
            if (clientpool_is_client_healthy_with(std::move(cfg), (clients)[i])) {
                count += static_cast<size_t>(1);
            }
            i += static_cast<size_t>(1);
        }
    }
    return std::move(count);
}
/*RUSTYCPP:GEN-END id=client.27*/

// @safe - rusty::Mutex::lock + BTreeMap/Vec ops + is_client_healthy are @safe.
// One-step typed &mut unwrap (§7.37) binds the cached Vec by reference.
#if RUSTYCPP_RUST
fn clientpool_remove_unhealthy_clients(self_: &ClientPool, addr: &std::string) -> usize {
    // Config snapshot BEFORE `state_`, per the lock-order invariant.
    let cfg: PoolConfig = self_.pool_config();
    let mut guard = self_.state_.lock().unwrap();
    let mut removed: usize = 0usize;
    // Probe with get(): an intermediate `let opt = ...get_mut(..)` binding
    // lowers to `auto&` on a temporary Option (won't compile). The chained
    // one-step unwrap below binds the inner &mut directly (§7.37).
    let has_entry: bool = (*guard).cache.get(addr).is_some();
    if has_entry {
        let clients: &mut Vec<Arc<Client>> = (*guard).cache.get_mut(addr).unwrap();
        // Remove unhealthy clients, but keep at least min_connections.
        let mut kept: Vec<Arc<Client>> = Vec::<Arc<Client>>();
        kept.reserve((*clients).len());
        let mut i: usize = 0usize;
        while i < (*clients).len() {
            let client: &Arc<Client> = &(*clients)[i];
            if (*clients).len() - removed <= cfg.min_connections as usize {
                kept.push(client.clone());
                i += 1usize;
                continue;
            }
            if !clientpool_is_client_healthy_with(cfg, client) {
                (*client).close();
                removed += 1usize;
            } else {
                kept.push(client.clone());
            }
            i += 1usize;
        }
        *clients = kept;

        // Remove empty entries from cache
        if (*clients).is_empty() {
            (*guard).cache.remove(addr);
        }
    }
    removed
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.28 version=1 rust_sha256=ed9ca5462df4cb50ecf1bc77a1c62a6d62febedc655a5d7c411712d82dcea56f*/
size_t clientpool_remove_unhealthy_clients(const ClientPool& self_, const std::string& addr) {
    const PoolConfig cfg = self_.pool_config();
    auto&& guard = rusty::deref_call(self_.state_.lock(), rusty::detail::__mdisp_unwrap{});
    size_t removed = static_cast<size_t>(0);
    const bool has_entry = (rusty::detail::deref_if_pointer_like(guard)).cache.get(addr).is_some();
    if (has_entry) {
        rusty::Vec<rusty::Arc<Client>>& clients = (rusty::detail::deref_if_pointer_like(guard)).cache.get_mut(addr).unwrap();
        rusty::Vec<rusty::Arc<Client>> kept = rusty::Vec<rusty::Arc<Client>>();
        kept.reserve(rusty::len((clients)));
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::len((clients))) {
            const rusty::Arc<Client>& client = (clients)[i];
            if ((rusty::len((clients)) - rusty::detail::deref_if_pointer_like(removed)) <= (static_cast<size_t>(cfg.min_connections))) {
                kept.push(rusty::clone(client));
                i += static_cast<size_t>(1);
                continue;
            }
            if (rusty::detail::rust_not(clientpool_is_client_healthy_with(std::move(cfg), client))) {
                ((rusty::detail::deref_if_pointer_like(client))).close();
                removed += static_cast<size_t>(1);
            } else {
                kept.push(rusty::clone(client));
            }
            i += static_cast<size_t>(1);
        }
        clients = std::move(kept);
        if (rusty::is_empty(((clients)))) {
            (rusty::detail::deref_if_pointer_like(guard)).cache.remove(addr);
        }
    }
    return std::move(removed);
}
/*RUSTYCPP:GEN-END id=client.28*/

// @safe - rusty::Mutex::lock + BTreeMap/Vec ops + is_idle/close are @safe.
// Twin of remove_unhealthy_clients above (idle predicate instead of
// health); same get() probe + chained one-step &mut unwrap (§7.37).
#if RUSTYCPP_RUST
fn clientpool_close_idle_clients(self_: &ClientPool, addr: &std::string, current_time_ms: u64) -> usize {
    let cfg: PoolConfig = self_.pool_config();

    // If idle timeout is 0, no timeout
    if cfg.idle_timeout_ms == 0u64 {
        return 0usize;
    }

    let mut guard = self_.state_.lock().unwrap();
    let mut closed: usize = 0usize;
    let has_entry: bool = (*guard).cache.get(addr).is_some();
    if has_entry {
        let clients: &mut Vec<Arc<Client>> = (*guard).cache.get_mut(addr).unwrap();
        let mut kept: Vec<Arc<Client>> = Vec::<Arc<Client>>();
        kept.reserve((*clients).len());
        let mut i: usize = 0usize;
        while i < (*clients).len() {
            let client: &Arc<Client> = &(*clients)[i];
            if (*clients).len() - closed <= cfg.min_connections as usize {
                kept.push(client.clone());
                i += 1usize;
                continue;
            }
            if (*client).is_idle(cfg.idle_timeout_ms, current_time_ms) {
                (*client).close();
                closed += 1usize;
            } else {
                kept.push(client.clone());
            }
            i += 1usize;
        }
        *clients = kept;

        if (*clients).is_empty() {
            (*guard).cache.remove(addr);
        }
    }
    closed
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.29 version=1 rust_sha256=93c98fdb8228522e93026994afd257100d1833965aee36b5ae2e9530101bf350*/
size_t clientpool_close_idle_clients(const ClientPool& self_, const std::string& addr, uint64_t current_time_ms) {
    const PoolConfig cfg = self_.pool_config();
    if (rusty::detail::deref_if_pointer_like(cfg.idle_timeout_ms) == static_cast<uint64_t>(0)) {
        return static_cast<size_t>(0);
    }
    auto&& guard = rusty::deref_call(self_.state_.lock(), rusty::detail::__mdisp_unwrap{});
    size_t closed = static_cast<size_t>(0);
    const bool has_entry = (rusty::detail::deref_if_pointer_like(guard)).cache.get(addr).is_some();
    if (has_entry) {
        rusty::Vec<rusty::Arc<Client>>& clients = (rusty::detail::deref_if_pointer_like(guard)).cache.get_mut(addr).unwrap();
        rusty::Vec<rusty::Arc<Client>> kept = rusty::Vec<rusty::Arc<Client>>();
        kept.reserve(rusty::len((clients)));
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::len((clients))) {
            const rusty::Arc<Client>& client = (clients)[i];
            if ((rusty::len((clients)) - rusty::detail::deref_if_pointer_like(closed)) <= (static_cast<size_t>(cfg.min_connections))) {
                kept.push(rusty::clone(client));
                i += static_cast<size_t>(1);
                continue;
            }
            if (((rusty::detail::deref_if_pointer_like(client))).is_idle(std::move(cfg.idle_timeout_ms), std::move(current_time_ms))) {
                ((rusty::detail::deref_if_pointer_like(client))).close();
                closed += static_cast<size_t>(1);
            } else {
                kept.push(rusty::clone(client));
            }
            i += static_cast<size_t>(1);
        }
        clients = std::move(kept);
        if (rusty::is_empty(((clients)))) {
            (rusty::detail::deref_if_pointer_like(guard)).cache.remove(addr);
        }
    }
    return std::move(closed);
}
/*RUSTYCPP:GEN-END id=client.29*/

// @safe - rusty::Mutex::lock + BTreeMap/Vec ops are @safe.
// Keys-snapshot walker: the per-key body mutates `cache` via remove, so
// project the keys first (explicit iter/loop — the proven BTreeMap DSL
// idiom); then the wave-27 kept-swap per entry.
#if RUSTYCPP_RUST
fn clientpool_remove_all_unhealthy(self_: &ClientPool) -> usize {
    // Config snapshot BEFORE `state_`, per the lock-order invariant on
    // ClientPool. This read used to sit after the lock, which was the one
    // site in the pool that acquired the two in the opposite order from
    // get_client.
    let cfg: PoolConfig = self_.pool_config();
    let mut guard = self_.state_.lock().unwrap();
    let mut total_removed: usize = 0usize;

    let mut keys: Vec<std::string> = Vec::<std::string>();
    {
        let mut it = (*guard).cache.iter();
        loop {
            let e = it.next();
            if e.is_none() {
                break;
            }
            let kv = e.unwrap();
            keys.push(kv.0.clone());
        }
    }
    let mut empty_keys: Vec<std::string> = Vec::<std::string>();
    let mut k: usize = 0usize;
    while k < keys.len() {
        let addr: &std::string = &keys[k];
        let has_entry: bool = (*guard).cache.get(addr).is_some();
        if !has_entry {
            k += 1usize;
            continue;
        }
        let clients: &mut Vec<Arc<Client>> = (*guard).cache.get_mut(addr).unwrap();
        let mut removed: usize = 0usize;
        let mut kept: Vec<Arc<Client>> = Vec::<Arc<Client>>();
        kept.reserve((*clients).len());
        let mut i: usize = 0usize;
        while i < (*clients).len() {
            let client: &Arc<Client> = &(*clients)[i];
            if (*clients).len() - removed <= cfg.min_connections as usize {
                kept.push(client.clone());
                i += 1usize;
                continue;
            }
            if !clientpool_is_client_healthy_with(cfg, client) {
                (*client).close();
                removed += 1usize;
            } else {
                kept.push(client.clone());
            }
            i += 1usize;
        }
        *clients = kept;
        total_removed += removed;
        if (*clients).is_empty() {
            empty_keys.push(addr.clone());
        }
        k += 1usize;
    }
    let mut j: usize = 0usize;
    while j < empty_keys.len() {
        let key: &std::string = &empty_keys[j];
        (*guard).cache.remove(key);
        j += 1usize;
    }
    total_removed
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.30 version=1 rust_sha256=0547ef7d80d892b01851222313028d081b49051408d9ddb39833d3b9547d13dc*/
size_t clientpool_remove_all_unhealthy(const ClientPool& self_) {
    const PoolConfig cfg = self_.pool_config();
    auto&& guard = rusty::deref_call(self_.state_.lock(), rusty::detail::__mdisp_unwrap{});
    size_t total_removed = static_cast<size_t>(0);
    rusty::Vec<std::string> keys = rusty::Vec<std::string>();
    {
        auto it = rusty::iter((rusty::detail::deref_if_pointer_like(guard)).cache);
        while (true) {
            auto e = it.next();
            if (e.is_none()) {
                break;
            }
            const auto kv = e.unwrap();
            keys.push(rusty::clone(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(kv))));
        }
    }
    rusty::Vec<std::string> empty_keys = rusty::Vec<std::string>();
    size_t k = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(k) < rusty::len(keys)) {
        const std::string& addr = keys[k];
        const bool has_entry = (rusty::detail::deref_if_pointer_like(guard)).cache.get(addr).is_some();
        if (!has_entry) {
            k += static_cast<size_t>(1);
            continue;
        }
        rusty::Vec<rusty::Arc<Client>>& clients = (rusty::detail::deref_if_pointer_like(guard)).cache.get_mut(addr).unwrap();
        size_t removed = static_cast<size_t>(0);
        rusty::Vec<rusty::Arc<Client>> kept = rusty::Vec<rusty::Arc<Client>>();
        kept.reserve(rusty::len((clients)));
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::len((clients))) {
            const rusty::Arc<Client>& client = (clients)[i];
            if ((rusty::len((clients)) - rusty::detail::deref_if_pointer_like(removed)) <= (static_cast<size_t>(cfg.min_connections))) {
                kept.push(rusty::clone(client));
                i += static_cast<size_t>(1);
                continue;
            }
            if (rusty::detail::rust_not(clientpool_is_client_healthy_with(std::move(cfg), client))) {
                ((rusty::detail::deref_if_pointer_like(client))).close();
                removed += static_cast<size_t>(1);
            } else {
                kept.push(rusty::clone(client));
            }
            i += static_cast<size_t>(1);
        }
        clients = std::move(kept);
        total_removed += removed;
        if (rusty::is_empty(((clients)))) {
            empty_keys.push(rusty::clone(addr));
        }
        k += static_cast<size_t>(1);
    }
    size_t j = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(j) < rusty::len(empty_keys)) {
        const std::string& key = empty_keys[j];
        (rusty::detail::deref_if_pointer_like(guard)).cache.remove(key);
        j += static_cast<size_t>(1);
    }
    return std::move(total_removed);
}
/*RUSTYCPP:GEN-END id=client.30*/

// @safe - rusty::Mutex::lock + BTreeMap/Vec ops are @safe.
// Same keys-snapshot drain as remove_all_unhealthy, idle predicate.
#if RUSTYCPP_RUST
fn clientpool_close_all_idle(self_: &ClientPool, current_time_ms: u64) -> usize {
    let cfg: PoolConfig = self_.pool_config();
    if cfg.idle_timeout_ms == 0u64 {
        return 0usize;
    }

    let mut guard = self_.state_.lock().unwrap();
    let mut total_closed: usize = 0usize;

    let mut keys: Vec<std::string> = Vec::<std::string>();
    {
        let mut it = (*guard).cache.iter();
        loop {
            let e = it.next();
            if e.is_none() {
                break;
            }
            let kv = e.unwrap();
            keys.push(kv.0.clone());
        }
    }
    let mut empty_keys: Vec<std::string> = Vec::<std::string>();
    let mut k: usize = 0usize;
    while k < keys.len() {
        let addr: &std::string = &keys[k];
        let has_entry: bool = (*guard).cache.get(addr).is_some();
        if !has_entry {
            k += 1usize;
            continue;
        }
        let clients: &mut Vec<Arc<Client>> = (*guard).cache.get_mut(addr).unwrap();
        let mut closed: usize = 0usize;
        let mut kept: Vec<Arc<Client>> = Vec::<Arc<Client>>();
        kept.reserve((*clients).len());
        let mut i: usize = 0usize;
        while i < (*clients).len() {
            let client: &Arc<Client> = &(*clients)[i];
            if (*clients).len() - closed <= cfg.min_connections as usize {
                kept.push(client.clone());
                i += 1usize;
                continue;
            }
            if (*client).is_idle(cfg.idle_timeout_ms, current_time_ms) {
                (*client).close();
                closed += 1usize;
            } else {
                kept.push(client.clone());
            }
            i += 1usize;
        }
        *clients = kept;
        total_closed += closed;
        if (*clients).is_empty() {
            empty_keys.push(addr.clone());
        }
        k += 1usize;
    }
    let mut j: usize = 0usize;
    while j < empty_keys.len() {
        let key: &std::string = &empty_keys[j];
        (*guard).cache.remove(key);
        j += 1usize;
    }
    total_closed
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.31 version=1 rust_sha256=5fe33f16752501c42d792789908119a8dce7e00f43cccd4d94a1895d02ad2ecf*/
size_t clientpool_close_all_idle(const ClientPool& self_, uint64_t current_time_ms) {
    const PoolConfig cfg = self_.pool_config();
    if (rusty::detail::deref_if_pointer_like(cfg.idle_timeout_ms) == static_cast<uint64_t>(0)) {
        return static_cast<size_t>(0);
    }
    auto&& guard = rusty::deref_call(self_.state_.lock(), rusty::detail::__mdisp_unwrap{});
    size_t total_closed = static_cast<size_t>(0);
    rusty::Vec<std::string> keys = rusty::Vec<std::string>();
    {
        auto it = rusty::iter((rusty::detail::deref_if_pointer_like(guard)).cache);
        while (true) {
            auto e = it.next();
            if (e.is_none()) {
                break;
            }
            const auto kv = e.unwrap();
            keys.push(rusty::clone(rusty::detail::deref_if_pointer(([](auto&& __t) -> decltype(auto) { if constexpr (requires { __t._0; }) return (std::forward<decltype(__t)>(__t)._0); else if constexpr (requires { std::get<0>(std::forward<decltype(__t)>(__t)); }) return std::get<0>(std::forward<decltype(__t)>(__t)); else if constexpr (requires { (*__t)._0; }) return ((*std::forward<decltype(__t)>(__t))._0); else return std::get<0>(*std::forward<decltype(__t)>(__t)); })(kv))));
        }
    }
    rusty::Vec<std::string> empty_keys = rusty::Vec<std::string>();
    size_t k = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(k) < rusty::len(keys)) {
        const std::string& addr = keys[k];
        const bool has_entry = (rusty::detail::deref_if_pointer_like(guard)).cache.get(addr).is_some();
        if (!has_entry) {
            k += static_cast<size_t>(1);
            continue;
        }
        rusty::Vec<rusty::Arc<Client>>& clients = (rusty::detail::deref_if_pointer_like(guard)).cache.get_mut(addr).unwrap();
        size_t closed = static_cast<size_t>(0);
        rusty::Vec<rusty::Arc<Client>> kept = rusty::Vec<rusty::Arc<Client>>();
        kept.reserve(rusty::len((clients)));
        size_t i = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::len((clients))) {
            const rusty::Arc<Client>& client = (clients)[i];
            if ((rusty::len((clients)) - rusty::detail::deref_if_pointer_like(closed)) <= (static_cast<size_t>(cfg.min_connections))) {
                kept.push(rusty::clone(client));
                i += static_cast<size_t>(1);
                continue;
            }
            if (((rusty::detail::deref_if_pointer_like(client))).is_idle(std::move(cfg.idle_timeout_ms), std::move(current_time_ms))) {
                ((rusty::detail::deref_if_pointer_like(client))).close();
                closed += static_cast<size_t>(1);
            } else {
                kept.push(rusty::clone(client));
            }
            i += static_cast<size_t>(1);
        }
        clients = std::move(kept);
        total_closed += closed;
        if (rusty::is_empty(((clients)))) {
            empty_keys.push(rusty::clone(addr));
        }
        k += static_cast<size_t>(1);
    }
    size_t j = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(j) < rusty::len(empty_keys)) {
        const std::string& key = empty_keys[j];
        (rusty::detail::deref_if_pointer_like(guard)).cache.remove(key);
        j += static_cast<size_t>(1);
    }
    return std::move(total_closed);
}
/*RUSTYCPP:GEN-END id=client.31*/


// @unsafe - Drives Client::connect / reconnect synchronously; the state_
// lock + BTreeMap ops are @safe but the network I/O underneath is not
// (it is reached through clientpool_connect_client below).
#if RUSTYCPP_RUST
// The `const int8_t*` the rrr wire type wants is spelled `addr.c_str()
// as *const i8`, which lowers to the same reinterpret_cast the old
// kernel wrote by hand. Kept in THIS block, not its own, so caller and
// callee share one `#if RUSTYCPP_RUST` region.
fn clientpool_connect_client(client: &Arc<Client>, addr: &std::string) -> i32 {
    client.connect(addr.c_str() as *const i8, true)
}

fn clientpool_get_client(self_: &ClientPool, addr: &std::string) -> Option<Arc<Client>> {
    let mut sp_cl: Option<Arc<Client>> = None;
    let cfg: PoolConfig = self_.pool_config();
    let num_connections: i32 = cfg.min_connections;

    let mut guard = self_.state_.lock().unwrap();

    // Get or create load balancer state for this address. select() takes
    // &LoadBalancerState (round-robin advances through a Cell), so the
    // shared get() probe is enough.
    let has_lb: bool = (*guard).lb_state.get(addr).is_some();
    if !has_lb {
        (*guard).lb_state.insert(addr.clone(), LoadBalancerState::new());
    }
    let lb_state: &LoadBalancerState = (*guard).lb_state.get(addr).unwrap();

    let has_cached: bool = (*guard).cache.get(addr).is_some();
    if has_cached {
        let clients: &mut Vec<Arc<Client>> = (*guard).cache.get_mut(addr).unwrap();
        let client_count: i32 = (*clients).len() as i32;

        // Use load balancer to select starting index
        let start_idx: usize = LoadBalancer::select(
            cfg.load_balancing, clients, lb_state,
            RandomGenerator::rand(0i32, RAND_MAX) as usize);

        let mut i: i32 = 0i32;
        while i < client_count {
            let idx: usize = (start_idx + i as usize) % (client_count as usize);
            let client: &Arc<Client> = &(*clients)[idx];

            // Check if client is connected and healthy
            if (*client).connected() && clientpool_is_client_healthy_with(cfg, client) {
                sp_cl = Some(client.clone());
                break;
            }

            // Try to reconnect failed/disconnected clients
            let state: ConnectionState = (*client).connection_state();
            if (state as i32) == (ConnectionState::FAILED as i32)
                || (state as i32) == (ConnectionState::DISCONNECTED as i32) {
                let state_name = connection_state_to_string(state);
                log_line(Log::INFO, 0i32, core::ptr::null(), std::format("ClientPool: client to {} in state {}, attempting reconnect",
                         addr, state_name));
                if (*client).try_reconnect_if_needed() {
                    log_line(Log::INFO, 0i32, core::ptr::null(), std::format("ClientPool: reconnected to {} successfully", addr));
                    sp_cl = Some(client.clone());
                    break;
                } else {
                    log_line(Log::WARN, 0i32, core::ptr::null(), std::format("ClientPool: reconnect to {} failed", addr));
                }
            }
            i += 1i32;
        }

        // If no healthy client found after trying reconnects, recreate all connections
        if sp_cl.is_none() {
            log_line(Log::INFO, 0i32, core::ptr::null(), std::format("ClientPool: all clients to {} failed, recreating connections", addr));
            // Close old connections
            let mut ci: usize = 0usize;
            while ci < (*clients).len() {
                (*(*clients)[ci]).close();
                ci += 1usize;
            }
            (*clients).clear();

            // Create new connections (use min_connections)
            let mut ok: bool = true;
            let mut n: i32 = 0i32;
            while n < num_connections {
                let client: Arc<Client> =
                    Client::create(self_.poll_thread_worker_.as_ref().unwrap().clone());
                (*client).set_client_mode(true);
                if clientpool_connect_client(&client, addr) != 0i32 {
                    log_line(Log::WARN, 0i32, core::ptr::null(), std::format("ClientPool: failed to create new connection to {}", addr));
                    ok = false;
                    break;
                }
                (*clients).push(client);
                n += 1i32;
            }

            if ok && !(*clients).is_empty() {
                let pick: usize =
                    RandomGenerator::rand(0i32, (*clients).len() as i32 - 1i32) as usize;
                sp_cl = Some((*clients)[pick].clone());
            } else {
                // Remove from cache if we can't connect
                (*guard).cache.remove(addr);
            }
        }
    } else {
        // No cached connections - create new ones
        let mut parallel_clients: Vec<Arc<Client>> = Vec::<Arc<Client>>();
        let mut ok: bool = true;
        let mut n2: i32 = 0i32;
        while n2 < num_connections {
            let client: Arc<Client> =
                Client::create(self_.poll_thread_worker_.as_ref().unwrap().clone());
            (*client).set_client_mode(true);  // Jetpack: mark as client
            if clientpool_connect_client(&client, addr) != 0i32 {
                ok = false;
                break;
            }
            parallel_clients.push(client);
            n2 += 1i32;
        }
        if ok {
            let pick2: usize =
                RandomGenerator::rand(0i32, parallel_clients.len() as i32 - 1i32) as usize;
            sp_cl = Some(parallel_clients[pick2].clone());
            (*guard).cache.insert(addr.clone(), parallel_clients);
        }
        // If not ok, parallel_clients cleans up via the Arc drops
    }
    sp_cl
}
#endif
/*RUSTYCPP:GEN-BEGIN id=client.32 version=1 rust_sha256=07480a4753932596067003721d0c610a9436996d7f24b0c3eb7dfc083bdac708*/
int32_t clientpool_connect_client(const rusty::Arc<Client>& client, const std::string& addr) {
    return client->connect(rusty::detail::ptr_cast<const int8_t*>(addr.c_str()), true);
}

rusty::Option<rusty::Arc<Client>> clientpool_get_client(const ClientPool& self_, const std::string& addr) {
    rusty::Option<rusty::Arc<Client>> sp_cl = rusty::Option<rusty::Arc<Client>>{rusty::None};
    const PoolConfig cfg = self_.pool_config();
    const int32_t num_connections = cfg.min_connections;
    auto&& guard = rusty::deref_call(self_.state_.lock(), rusty::detail::__mdisp_unwrap{});
    const bool has_lb = (rusty::detail::deref_if_pointer_like(guard)).lb_state.get(addr).is_some();
    if (!has_lb) {
        (rusty::detail::deref_if_pointer_like(guard)).lb_state.insert(rusty::clone(addr), LoadBalancerState::new_());
    }
    const LoadBalancerState& lb_state = (rusty::detail::deref_if_pointer_like(guard)).lb_state.get(addr).unwrap();
    const bool has_cached = (rusty::detail::deref_if_pointer_like(guard)).cache.get(addr).is_some();
    if (has_cached) {
        rusty::Vec<rusty::Arc<Client>>& clients = (rusty::detail::deref_if_pointer_like(guard)).cache.get_mut(addr).unwrap();
        const int32_t client_count = static_cast<int32_t>(rusty::len((clients)));
        const size_t start_idx = LoadBalancer::select(std::move(cfg.load_balancing), clients, lb_state, static_cast<size_t>(RandomGenerator::rand(static_cast<int32_t>(0), RAND_MAX)));
        int32_t i = static_cast<int32_t>(0);
        while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(client_count)) {
            const size_t idx = ((rusty::detail::deref_if_pointer_like(start_idx) + (static_cast<size_t>(i)))) % ((static_cast<size_t>(client_count)));
            const rusty::Arc<Client>& client = (clients)[idx];
            if (((rusty::detail::deref_if_pointer_like(client))).connected() && clientpool_is_client_healthy_with(std::move(cfg), client)) {
                sp_cl = rusty::Option<rusty::Arc<Client>>(rusty::clone(client));
                break;
            }
            const ConnectionState state = ((rusty::detail::deref_if_pointer_like(client))).connection_state();
            if ((((static_cast<int32_t>(state))) == ((static_cast<int32_t>(ConnectionState::FAILED)))) || (((static_cast<int32_t>(state))) == ((static_cast<int32_t>(ConnectionState::DISCONNECTED))))) {
                const auto state_name = connection_state_to_string(std::move(state));
                log_line(rusty::clone(rusty::clone(Log::INFO)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("ClientPool: client to {} in state {}, attempting reconnect", addr, std::move(state_name)));
                if (((rusty::detail::deref_if_pointer_like(client))).try_reconnect_if_needed()) {
                    log_line(rusty::clone(rusty::clone(Log::INFO)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("ClientPool: reconnected to {} successfully", addr));
                    sp_cl = rusty::Option<rusty::Arc<Client>>(rusty::clone(client));
                    break;
                } else {
                    log_line(rusty::clone(rusty::clone(Log::WARN)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("ClientPool: reconnect to {} failed", addr));
                }
            }
            i += static_cast<int32_t>(1);
        }
        if (sp_cl.is_none()) {
            log_line(rusty::clone(rusty::clone(Log::INFO)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("ClientPool: all clients to {} failed, recreating connections", addr));
            size_t ci = static_cast<size_t>(0);
            while (rusty::detail::deref_if_pointer_like(ci) < rusty::len((clients))) {
                ((rusty::detail::deref_if_pointer_like((clients)[ci]))).close();
                ci += static_cast<size_t>(1);
            }
            ((clients)).clear();
            bool ok = true;
            int32_t n = static_cast<int32_t>(0);
            while (rusty::detail::deref_if_pointer_like(n) < rusty::detail::deref_if_pointer_like(num_connections)) {
                rusty::Arc<Client> client = Client::create(rusty::clone(self_.poll_thread_worker_.as_ref().unwrap()));
                ((rusty::detail::deref_if_pointer_like(client))).set_client_mode(true);
                if (clientpool_connect_client(client, addr) != static_cast<int32_t>(0)) {
                    log_line(rusty::clone(rusty::clone(Log::WARN)), static_cast<int32_t>(0), rusty::ptr::null(), std::format("ClientPool: failed to create new connection to {}", addr));
                    ok = false;
                    break;
                }
                ((clients)).push(std::move(client));
                n += static_cast<int32_t>(1);
            }
            if (rusty::detail::deref_if_pointer_like(ok) && rusty::detail::rust_not(rusty::is_empty(((clients))))) {
                const size_t pick = static_cast<size_t>(RandomGenerator::rand(static_cast<int32_t>(0), (static_cast<int32_t>(rusty::len((clients)))) - static_cast<int32_t>(1)));
                sp_cl = rusty::Option<rusty::Arc<Client>>(rusty::clone((clients)[pick]));
            } else {
                (rusty::detail::deref_if_pointer_like(guard)).cache.remove(addr);
            }
        }
    } else {
        rusty::Vec<rusty::Arc<Client>> parallel_clients = rusty::Vec<rusty::Arc<Client>>();
        bool ok = true;
        int32_t n2 = static_cast<int32_t>(0);
        while (rusty::detail::deref_if_pointer_like(n2) < rusty::detail::deref_if_pointer_like(num_connections)) {
            rusty::Arc<Client> client = Client::create(rusty::clone(self_.poll_thread_worker_.as_ref().unwrap()));
            ((rusty::detail::deref_if_pointer_like(client))).set_client_mode(true);
            if (clientpool_connect_client(client, addr) != static_cast<int32_t>(0)) {
                ok = false;
                break;
            }
            parallel_clients.push(std::move(client));
            n2 += static_cast<int32_t>(1);
        }
        if (ok) {
            const size_t pick2 = static_cast<size_t>(RandomGenerator::rand(static_cast<int32_t>(0), (static_cast<int32_t>(rusty::len(parallel_clients))) - static_cast<int32_t>(1)));
            sp_cl = rusty::Option<rusty::Arc<Client>>(rusty::clone(parallel_clients[pick2]));
            (rusty::detail::deref_if_pointer_like(guard)).cache.insert(rusty::clone(addr), std::move(parallel_clients));
        }
    }
    return std::move(sp_cl);
}
/*RUSTYCPP:GEN-END id=client.32*/

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
