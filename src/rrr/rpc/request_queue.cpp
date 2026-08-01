module;

#include <cstdint>
#include <cstdlib>

#include <rusty/arc.hpp>
#include <rusty/function.hpp>
#include <rusty/option.hpp>
#include <rusty/rusty.hpp>
#include <rusty/vecdeque.hpp>

export module rrr.request_queue;

import std;
import rusty;
import rrr.basetypes;
import rrr.threading;

export namespace rrr {


// `OverflowStrategy` — categorical tag for how the queue should
// handle a push when at capacity. Authored as inline Rust DSL: the
// `#if RUSTYCPP_RUST` block below is the source of truth; the
// transpiler regenerates the matching `RUSTYCPP:GEN-BEGIN ... END`
// block with the C++ `enum class`.
#if RUSTYCPP_RUST
enum OverflowStrategy {
    DROP_OLDEST,
    DROP_NEWEST,
    FAIL_FAST,
}

fn overflow_strategy_to_string(strategy: OverflowStrategy) -> &'static str {
    match strategy {
        OverflowStrategy::DROP_OLDEST => "DROP_OLDEST",
        OverflowStrategy::DROP_NEWEST => "DROP_NEWEST",
        OverflowStrategy::FAIL_FAST => "FAIL_FAST",
        _ => "UNKNOWN",
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=request_queue.overflow_strategy version=1 rust_sha256=df1d44b6aab049105caa11c08a961da299d1ba5e4259380f867eef0e62be53f0*/
enum class OverflowStrategy;
constexpr OverflowStrategy OverflowStrategy_DROP_OLDEST();
constexpr OverflowStrategy OverflowStrategy_DROP_NEWEST();
constexpr OverflowStrategy OverflowStrategy_FAIL_FAST();
std::string_view overflow_strategy_to_string(OverflowStrategy strategy);

enum class OverflowStrategy {
    DROP_OLDEST,
    DROP_NEWEST,
    FAIL_FAST
};
inline constexpr OverflowStrategy OverflowStrategy_DROP_OLDEST() { return OverflowStrategy::DROP_OLDEST; }
inline constexpr OverflowStrategy OverflowStrategy_DROP_NEWEST() { return OverflowStrategy::DROP_NEWEST; }
inline constexpr OverflowStrategy OverflowStrategy_FAIL_FAST() { return OverflowStrategy::FAIL_FAST; }

std::string_view overflow_strategy_to_string(OverflowStrategy strategy) {
    return ({ auto&& _m = strategy; std::optional<std::string_view> _match_value; bool _m_matched = false; if (!_m_matched && (_m == OverflowStrategy::DROP_OLDEST)) { _match_value.emplace(std::move(std::string_view("DROP_OLDEST"))); _m_matched = true; } if (!_m_matched && (_m == OverflowStrategy::DROP_NEWEST)) { _match_value.emplace(std::move(std::string_view("DROP_NEWEST"))); _m_matched = true; } if (!_m_matched && (_m == OverflowStrategy::FAIL_FAST)) { _match_value.emplace(std::move(std::string_view("FAIL_FAST"))); _m_matched = true; } if (!_m_matched) { _match_value.emplace(std::move(std::string_view("UNKNOWN"))); _m_matched = true; } if (!_m_matched) { rusty::intrinsics::unreachable_panic(); } std::move(_match_value).value(); });
}
/*RUSTYCPP:GEN-END id=request_queue.overflow_strategy*/

// Canonical queue callback errors for caller observability.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block with the C++ constexpr. Same
// shape as the wire-protocol constants in internal_protocol.cpp —
// libc-macro RHS values (EAGAIN / ETIMEDOUT) get emitted verbatim.
#if RUSTYCPP_RUST
const kRequestQueueRejectedError: i32 = EAGAIN;
const kRequestQueueExpiredError: i32 = ETIMEDOUT;
#endif
/*RUSTYCPP:GEN-BEGIN id=request_queue.err_codes version=1 rust_sha256=f0652fbb44002bbb667042a6bbb01ba5cd204496acc11bf10713b53ab4321f2d*/
extern const int32_t kRequestQueueRejectedError;
extern const int32_t kRequestQueueExpiredError;

constexpr int32_t kRequestQueueRejectedError = EAGAIN;

constexpr int32_t kRequestQueueExpiredError = ETIMEDOUT;
/*RUSTYCPP:GEN-END id=request_queue.err_codes*/

// Type alias for QueuedRequest's completion callback. Defined outside
// the DSL block so the inline-Rust source can refer to it by an
// opaque type name (the DSL transpiler does not parse C++ function-
// template arguments like `<void(int)>`).
using QueuedRequestCallback = rusty::Function<void(int)>;

// Wrapper around rusty::sys::time::clock_monotonic_us, named so the
// DSL block below can call it as a simple identifier rather than the
// fully-qualified path. Authored as inline Rust DSL: the
// `#if RUSTYCPP_RUST` block below is the source of truth; the
// transpiler regenerates the matching `RUSTYCPP:GEN-BEGIN ... END`
// block. Same shape as `heartbeat_time_us` (heartbeat.cpp) and
// `current_time_us` (circuit_breaker.cpp).
#if RUSTYCPP_RUST
fn queued_request_time_us() -> u64 {
    rusty::sys::time::clock_monotonic_us()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=request_queue.queued_request_time_us version=1 rust_sha256=75814fdd205b8de30a538e2f38098e5f2c23f97f12f2952d426174ea0864a759*/
uint64_t queued_request_time_us();

uint64_t queued_request_time_us() {
    return rusty::sys::time::clock_monotonic_us();
}
/*RUSTYCPP:GEN-END id=request_queue.queued_request_time_us*/

/**
 * A queued RPC request awaiting transmission.
 *
 * Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
 * the source of truth; the transpiler regenerates the matching
 * `RUSTYCPP:GEN-BEGIN ... END` block. The plain `fn new()` lowers to
 * a static `QueuedRequest::new_()` factory.
 *
 * Behavioral diffs from the original C++ struct:
 *   * No user-defined default constructor — callers that previously
 *     default-constructed (`QueuedRequest req;`) now write
 *     `auto req = QueuedRequest::new_();` explicitly. The factory
 *     does the same field-init work the original ctor did
 *     (`timestamp_us = queued_request_time_us()`,
 *     `ttl_ms = 30000`).
 *   * Fields no longer marked private (the DSL emits all fields
 *     public); no callers reach into them through anything other
 *     than the public field names that were already public-by-
 *     designation in the aggregate-style original.
 */
#if RUSTYCPP_RUST
struct QueuedRequest {
    xid: i64,
    rpc_id: i32,
    timestamp_us: u64,
    retry_count: u32,
    callback: QueuedRequestCallback,
    ttl_ms: u32,
}

impl QueuedRequest {
    fn new() -> QueuedRequest {
        QueuedRequest {
            xid: 0i64,
            rpc_id: 0i32,
            timestamp_us: queued_request_time_us(),
            retry_count: 0u32,
            callback: QueuedRequestCallback {},
            ttl_ms: 30000u32,
        }
    }

    fn is_expired(&self) -> bool {
        let now_us: u64 = queued_request_time_us();
        let elapsed_us: u64 = now_us - self.timestamp_us;
        (elapsed_us / 1000u64) > (self.ttl_ms as u64)
    }

    fn age_ms(&self) -> u32 {
        let now_us: u64 = queued_request_time_us();
        ((now_us - self.timestamp_us) / 1000u64) as u32
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=request_queue.2 version=1 rust_sha256=7c5fc9b4686d94756ad85ce30fcf1ae36d88a924d7b1e36ea8c202985ae25bcd*/
struct QueuedRequest;

struct QueuedRequest {
    int64_t xid;
    int32_t rpc_id;
    uint64_t timestamp_us;
    uint32_t retry_count;
    QueuedRequestCallback callback;
    uint32_t ttl_ms;

    static QueuedRequest new_();
    bool is_expired() const;
    uint32_t age_ms() const;
};


QueuedRequest QueuedRequest::new_() {
    return QueuedRequest{.xid = static_cast<int64_t>(0), .rpc_id = static_cast<int32_t>(0), .timestamp_us = queued_request_time_us(), .retry_count = static_cast<uint32_t>(0), .callback = QueuedRequestCallback{}, .ttl_ms = static_cast<uint32_t>(30000)};
}

bool QueuedRequest::is_expired() const {
    const uint64_t now_us = queued_request_time_us();
    const uint64_t elapsed_us = rusty::detail::deref_if_pointer_like(now_us) - rusty::detail::deref_if_pointer_like(this->timestamp_us);
    return ((rusty::detail::deref_if_pointer_like(elapsed_us) / static_cast<uint64_t>(1000))) > ((static_cast<uint64_t>(this->ttl_ms)));
}

uint32_t QueuedRequest::age_ms() const {
    const uint64_t now_us = queued_request_time_us();
    return static_cast<uint32_t>((((rusty::detail::deref_if_pointer_like(now_us) - rusty::detail::deref_if_pointer_like(this->timestamp_us))) / static_cast<uint64_t>(1000)));
}
/*RUSTYCPP:GEN-END id=request_queue.2*/

// Configuration for RequestQueue.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. The plain `fn new()` lowers to
// a `static RequestQueueConfig new_()` factory. Callers construct via
// the factory (`auto config = RequestQueueConfig::new_();`) or one of
// the `defaults()` / `small()` / `large()` / `disabled()` presets.
//
// Now that there is no cpp_ctor, RequestQueueConfig is a pure
// aggregate; the preset bodies use the populated DSL literal form
// `RequestQueueConfig { max_size: ..., ... }` which lowers to a clean
// designated initializer `RequestQueueConfig{.max_size = ...}`.
#if RUSTYCPP_RUST
struct RequestQueueConfig {
    max_size: usize,
    default_ttl_ms: u32,
    overflow_strategy: OverflowStrategy,
    enabled: bool,
}

impl RequestQueueConfig {
    fn new() -> RequestQueueConfig {
        RequestQueueConfig {
            max_size: 1000usize,
            default_ttl_ms: 30000u32,
            overflow_strategy: OverflowStrategy::DROP_OLDEST,
            enabled: true,
        }
    }

    fn defaults() -> RequestQueueConfig {
        RequestQueueConfig::new()
    }

    fn small() -> RequestQueueConfig {
        RequestQueueConfig {
            max_size: 10usize,
            default_ttl_ms: 5000u32,
            overflow_strategy: OverflowStrategy::DROP_OLDEST,
            enabled: true,
        }
    }

    fn large() -> RequestQueueConfig {
        RequestQueueConfig {
            max_size: 10000usize,
            default_ttl_ms: 60000u32,
            overflow_strategy: OverflowStrategy::DROP_OLDEST,
            enabled: true,
        }
    }

    fn disabled() -> RequestQueueConfig {
        RequestQueueConfig {
            max_size: 0usize,
            default_ttl_ms: 30000u32,
            overflow_strategy: OverflowStrategy::DROP_OLDEST,
            enabled: false,
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=request_queue.1 version=1 rust_sha256=18bf9469715694e84270d5bde4d97e4b7daa0b11880dbefa385105d079f4294f*/
struct RequestQueueConfig;

struct RequestQueueConfig {
    size_t max_size;
    uint32_t default_ttl_ms;
    OverflowStrategy overflow_strategy;
    bool enabled;

    static RequestQueueConfig new_();
    static RequestQueueConfig defaults();
    static RequestQueueConfig small();
    static RequestQueueConfig large();
    static RequestQueueConfig disabled();
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};


RequestQueueConfig RequestQueueConfig::new_() {
    return RequestQueueConfig{.max_size = static_cast<size_t>(1000), .default_ttl_ms = static_cast<uint32_t>(30000), .overflow_strategy = rusty::clone(rusty::clone(OverflowStrategy_DROP_OLDEST())), .enabled = true};
}

RequestQueueConfig RequestQueueConfig::defaults() {
    return RequestQueueConfig::new_();
}

RequestQueueConfig RequestQueueConfig::small() {
    return RequestQueueConfig{.max_size = static_cast<size_t>(10), .default_ttl_ms = static_cast<uint32_t>(5000), .overflow_strategy = rusty::clone(rusty::clone(OverflowStrategy_DROP_OLDEST())), .enabled = true};
}

RequestQueueConfig RequestQueueConfig::large() {
    return RequestQueueConfig{.max_size = static_cast<size_t>(10000), .default_ttl_ms = static_cast<uint32_t>(60000), .overflow_strategy = rusty::clone(rusty::clone(OverflowStrategy_DROP_OLDEST())), .enabled = true};
}

RequestQueueConfig RequestQueueConfig::disabled() {
    return RequestQueueConfig{.max_size = static_cast<size_t>(0), .default_ttl_ms = static_cast<uint32_t>(30000), .overflow_strategy = rusty::clone(rusty::clone(OverflowStrategy_DROP_OLDEST())), .enabled = false};
}
/*RUSTYCPP:GEN-END id=request_queue.1*/

/**
 * Thread-safe queue for pending RPC requests.
 *
 * Stores requests during connection failures for later replay.
 * Provides configurable size limits, overflow strategies, and TTL expiration.
 *
 * Usage:
 *   RequestQueue queue;
 *
 *   QueuedRequest req;
 *   req.xid = 12345;
 *   req.rpc_id = 1;
 *   req.ttl_ms = 5000;
 *
 *   if (queue.enqueue(std::move(req))) {
 *       // Request queued successfully
 *   }
 *
 *   auto next = queue.dequeue();
 *   if (next.is_some()) {
 *       // Process request
 *   }
 */
// @safe - rusty::Mutex<VecDeque<QueuedRequest>>-backed pending-request queue.
// All public methods are already explicitly @safe from Tier 2; class-level
// annotation lets the constructor and any future unannotated helpers
// inherit @safe by default.
// `RequestQueue` — buffers outgoing RPC requests for reconnect/replay, with
// overflow + TTL-expiry policy, thread-safe via a rusty::Mutex<VecDeque>. Authored
// as inline-Rust DSL: the struct + two `#[cpp_ctor]` factories + the simple
// locking/accessor methods are DSL; the methods whose bodies use try/catch +
// callback invocation + range-for (not expressible in inline-Rust) delegate to
// hand-written free functions below. RequestQueueConfig / OverflowStrategy /
// QueuedRequest are already DSL aggregates (above). The locking methods are
// `&mut self` (non-const): the C++ original used `mutable rusty::Mutex` to lock
// from const methods; there are no `const RequestQueue` call sites
// (ClientConnection holds it in a `mutable` field), so non-const is equivalent
// and avoids a mutable-field annotation. `clear_all`'s default arg (-3) is
// dropped (callers pass it explicitly; the one 0-arg test call is updated).
struct RequestQueue;
inline void rq_invoke_callback_safely(rusty::Function<void(int)> cb, int err);
inline bool rq_enqueue(const RequestQueue& self, QueuedRequest request);
inline size_t rq_expire_stale(const RequestQueue& self);
inline void rq_clear_all(const RequestQueue& self, int error_code);
#if RUSTYCPP_RUST
struct RequestQueue {
    // Cell: RequestQueue is reached through a shared handle, so a config
    // swap goes through interior mutability rather than a const_cast (the
    // same treatment ServerConnection::status_ got).
    config_: Cell<RequestQueueConfig>,
    queue_: rusty::Mutex<VecDeque<QueuedRequest>>,
}

impl RequestQueue {
    #[cpp_ctor] fn new() -> RequestQueue {
        RequestQueue {
            config_: Cell::new(RequestQueueConfig::defaults()),
            queue_: rusty::Mutex::<VecDeque<QueuedRequest>>::new(VecDeque::<QueuedRequest>::new()),
        }
    }

    #[cpp_ctor] fn with_config(config: RequestQueueConfig) -> RequestQueue {
        RequestQueue {
            config_: Cell::new(config),
            queue_: rusty::Mutex::<VecDeque<QueuedRequest>>::new(VecDeque::<QueuedRequest>::new()),
        }
    }

    fn enqueue(&self, request: QueuedRequest) -> bool {
        rq_enqueue(self, request)
    }

    fn dequeue(&mut self) -> Option<QueuedRequest> {
        // VecDeque::pop_front() already returns Option in real Rust.
        let guard = self.queue_.lock().unwrap();
        guard.pop_front()
    }

    fn expire_stale(&self) -> usize {
        rq_expire_stale(self)
    }

    fn size(&self) -> usize {
        let guard = self.queue_.lock().unwrap();
        guard.size()
    }

    fn empty(&self) -> bool {
        let guard = self.queue_.lock().unwrap();
        guard.size() == 0usize
    }

    fn full(&mut self) -> bool {
        let guard = self.queue_.lock().unwrap();
        guard.size() >= self.config_.get().max_size
    }

    fn remaining_capacity(&mut self) -> usize {
        let guard = self.queue_.lock().unwrap();
        if self.config_.get().max_size > guard.size() {
            self.config_.get().max_size - guard.size()
        } else {
            0usize
        }
    }

    fn clear_all(&self, error_code: i32) {
        rq_clear_all(self, error_code)
    }

    fn config(&self) -> RequestQueueConfig {
        self.config_.get()
    }

    fn enabled(&self) -> bool {
        self.config_.get().enabled
    }

    fn max_size(&self) -> usize {
        self.config_.get().max_size
    }

    fn update_config(&self, config: RequestQueueConfig) {
        // Lock held while swapping so a concurrent enqueue cannot observe a
        // half-applied config, matching what the C++ helper did.
        let guard = self.queue_.lock().unwrap();
        self.config_.set(config);
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=request_queue.queue version=1 rust_sha256=aeebda810bbe4187a370b0cc1a9c4e9ad9c9f7b5a6a05ca47d67f87191336ea4*/
struct RequestQueue;

struct RequestQueue {
    rusty::Cell<RequestQueueConfig> config_;
    rusty::Mutex<rusty::VecDeque<QueuedRequest>> queue_;

    RequestQueue();
    RequestQueue(RequestQueueConfig config);
    bool enqueue(QueuedRequest request) const;
    rusty::Option<QueuedRequest> dequeue();
    size_t expire_stale() const;
    size_t size() const;
    bool empty() const;
    bool full();
    size_t remaining_capacity();
    void clear_all(int32_t error_code) const;
    RequestQueueConfig config() const;
    bool enabled() const;
    size_t max_size() const;
    void update_config(RequestQueueConfig config) const;
};


RequestQueue::RequestQueue()
    : config_(rusty::Cell<RequestQueueConfig>::new_(RequestQueueConfig::defaults()))
    , queue_(rusty::Mutex<rusty::VecDeque<QueuedRequest>>::new_(rusty::VecDeque<QueuedRequest>::new_()))
{}

RequestQueue::RequestQueue(RequestQueueConfig config)
    : config_(rusty::Cell<RequestQueueConfig>::new_(std::move(config)))
    , queue_(rusty::Mutex<rusty::VecDeque<QueuedRequest>>::new_(rusty::VecDeque<QueuedRequest>::new_()))
{}

bool RequestQueue::enqueue(QueuedRequest request) const {
    return rq_enqueue((*this), std::move(request));
}

rusty::Option<QueuedRequest> RequestQueue::dequeue() {
    auto guard = this->queue_.lock().unwrap();
    return (*guard).pop_front();
}

size_t RequestQueue::expire_stale() const {
    return rq_expire_stale((*this));
}

size_t RequestQueue::size() const {
    auto guard = this->queue_.lock().unwrap();
    return (*guard).size();
}

bool RequestQueue::empty() const {
    auto guard = this->queue_.lock().unwrap();
    return (*guard).size() == static_cast<size_t>(0);
}

bool RequestQueue::full() {
    auto guard = this->queue_.lock().unwrap();
    return (*guard).size() >= rusty::detail::deref_if_pointer_like(this->config_.get().max_size);
}

size_t RequestQueue::remaining_capacity() {
    auto guard = this->queue_.lock().unwrap();
    if (rusty::detail::deref_if_pointer_like(this->config_.get().max_size) > (*guard).size()) {
        return rusty::detail::deref_if_pointer_like(this->config_.get().max_size) - (*guard).size();
    } else {
        return static_cast<size_t>(0);
    }
}

void RequestQueue::clear_all(int32_t error_code) const {
    rq_clear_all((*this), std::move(error_code));
}

RequestQueueConfig RequestQueue::config() const {
    return this->config_.get();
}

bool RequestQueue::enabled() const {
    return this->config_.get().enabled;
}

size_t RequestQueue::max_size() const {
    return this->config_.get().max_size;
}

void RequestQueue::update_config(RequestQueueConfig config) const {
    auto guard = this->queue_.lock().unwrap();
    this->config_.set(std::move(config));
}
/*RUSTYCPP:GEN-END id=request_queue.queue*/

// @safe - Invoke a queued-request callback, swallowing any exception. No-op if
// the callback is null. Consumes the callback. The try/catch is not expressible
// in inline-Rust, so callback invocation lives here (the callbacks.cpp pattern).
inline void rq_invoke_callback_safely(rusty::Function<void(int)> cb, int err) {
    if (cb) {
        // @unsafe { invoking a stored rusty::Function + swallowing exceptions }
        try { cb(err); } catch (...) {}
    }
}

// @unsafe - enqueue: overflow policy + try/catch callback invocation (the
// try/catch and the interleaved rejection callbacks are not DSL-expressible).
inline bool rq_enqueue(const RequestQueue& self, QueuedRequest request) {
    if (!self.config_.get().enabled) {
        rq_invoke_callback_safely(std::move(request.callback), kRequestQueueRejectedError);
        return false;
    }
    auto guard = self.queue_.lock().unwrap();
    if ((*guard).size() >= self.config_.get().max_size) {
        switch (self.config_.get().overflow_strategy) {
            case OverflowStrategy::DROP_OLDEST:
                if ((*guard).size() > 0) {
                    auto oldest = (*guard).pop_front().unwrap();
                    rq_invoke_callback_safely(std::move(oldest.callback), kRequestQueueRejectedError);
                }
                break;
            case OverflowStrategy::DROP_NEWEST:
            case OverflowStrategy::FAIL_FAST:
                rq_invoke_callback_safely(std::move(request.callback), kRequestQueueRejectedError);
                return false;
        }
    }
    if (request.ttl_ms == 0) {
        request.ttl_ms = self.config_.get().default_ttl_ms;
    }
    (*guard).push_back(std::move(request));
    return true;
}

// @unsafe - expire_stale: extract_if + drain callbacks outside the lock with
// try/catch (the try/catch is not DSL-expressible).
inline size_t rq_expire_stale(const RequestQueue& self) {
    rusty::Vec<rusty::Function<void(int)>> callbacks_to_invoke;
    size_t removed = 0;
    {
        auto guard = self.queue_.lock().unwrap();
        auto expired = (*guard).extract_if(
            rusty::Function<bool(const QueuedRequest&)>(
                [](const QueuedRequest& r) { return r.is_expired(); }));
        removed = expired.size();
        while (expired.size() > 0) {
            auto req = expired.pop_front().unwrap();
            if (req.callback) {
                callbacks_to_invoke.push(std::move(req.callback));
            }
        }
    }
    for (auto& cb : callbacks_to_invoke) {
        // @unsafe { invoking + swallowing exceptions }
        try { cb(kRequestQueueExpiredError); } catch (...) {}
    }
    return removed;
}

// @unsafe - clear_all: drain callbacks (range-for over the guarded deque) +
// clear, invoke outside the lock with try/catch.
inline void rq_clear_all(const RequestQueue& self, int error_code) {
    rusty::Vec<rusty::Function<void(int)>> callbacks_to_invoke;
    {
        auto guard = self.queue_.lock().unwrap();
        for (auto& req : *guard) {
            if (req.callback) {
                callbacks_to_invoke.push(std::move(req.callback));
            }
        }
        (*guard).clear();
    }
    for (auto& cb : callbacks_to_invoke) {
        // @unsafe { invoking + swallowing exceptions }
        try { cb(error_code); } catch (...) {}
    }
}

// @safe - update_config: lock to serialize with in-flight enqueue/dequeue, then
// assign the POD config. (The lock-for-side-effect `(void)guard` reads cleaner
// as a free fn than in the DSL.)

// @safe - Convert overflow strategy to string


}  // export namespace rrr
