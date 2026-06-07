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
import rrr.marshal;
import rrr.threading;

export namespace rrr {


/**
 * Strategy for handling queue overflow.
 */
enum class OverflowStrategy {
    DROP_OLDEST,   // Remove oldest request to make room
    DROP_NEWEST,   // Reject new request if queue full
    FAIL_FAST      // Immediately fail the request with error callback
};

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
 *     `payload = Arc::<Marshal>::make()`, `ttl_ms = 30000`).
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
    payload: Arc<Marshal>,
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
            payload: Arc::<Marshal>::make(),
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
/*RUSTYCPP:GEN-BEGIN id=request_queue.2 version=1 rust_sha256=e1030e3c7232098b7e5960fca2e18aac19e4cdf30c22f01e504c54a63b9feb1b*/
struct QueuedRequest;

struct QueuedRequest {
    int64_t xid;
    int32_t rpc_id;
    uint64_t timestamp_us;
    uint32_t retry_count;
    rusty::Arc<Marshal> payload;
    QueuedRequestCallback callback;
    uint32_t ttl_ms;

    static QueuedRequest new_();
    bool is_expired() const;
    uint32_t age_ms() const;
};


QueuedRequest QueuedRequest::new_() {
    return QueuedRequest{.xid = static_cast<int64_t>(0), .rpc_id = static_cast<int32_t>(0), .timestamp_us = queued_request_time_us(), .retry_count = static_cast<uint32_t>(0), .payload = rusty::Arc<Marshal>::make(), .callback = QueuedRequestCallback{}, .ttl_ms = static_cast<uint32_t>(30000)};
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
};


RequestQueueConfig RequestQueueConfig::new_() {
    return RequestQueueConfig{.max_size = static_cast<size_t>(1000), .default_ttl_ms = static_cast<uint32_t>(30000), .overflow_strategy = rusty::clone(rusty::clone(OverflowStrategy::DROP_OLDEST)), .enabled = true};
}

RequestQueueConfig RequestQueueConfig::defaults() {
    return RequestQueueConfig::new_();
}

RequestQueueConfig RequestQueueConfig::small() {
    return RequestQueueConfig{.max_size = static_cast<size_t>(10), .default_ttl_ms = static_cast<uint32_t>(5000), .overflow_strategy = rusty::clone(rusty::clone(OverflowStrategy::DROP_OLDEST)), .enabled = true};
}

RequestQueueConfig RequestQueueConfig::large() {
    return RequestQueueConfig{.max_size = static_cast<size_t>(10000), .default_ttl_ms = static_cast<uint32_t>(60000), .overflow_strategy = rusty::clone(rusty::clone(OverflowStrategy::DROP_OLDEST)), .enabled = true};
}

RequestQueueConfig RequestQueueConfig::disabled() {
    return RequestQueueConfig{.max_size = static_cast<size_t>(0), .default_ttl_ms = static_cast<uint32_t>(30000), .overflow_strategy = rusty::clone(rusty::clone(OverflowStrategy::DROP_OLDEST)), .enabled = false};
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
// @safe - SpinMutex<VecDeque<QueuedRequest>>-backed pending-request queue.
// All public methods are already explicitly @safe from Tier 2; class-level
// annotation lets the constructor and any future unannotated helpers
// inherit @safe by default.
class RequestQueue {
private:
    RequestQueueConfig config_;
    // VecDeque ring-buffer wrapped in SpinMutex for thread-safe access.
    // SpinMutex owns its T, replacing the prior `mutex_ + queue_` pair
    // pattern with a single rusty-style "data inside the lock" container.
    // The VecDeque's placement-new ring-buffer preserves the same
    // "no move-assignment of QueuedRequest's Marshal-bearing payload
    // after enqueue" property the prior std::list comment cited.
    mutable SpinMutex<rusty::VecDeque<QueuedRequest>> queue_;

public:
    // @safe - Default ctor argument is the @safe defaults() factory; the
    // RequestQueueConfig POD copy is trivially safe.
    explicit RequestQueue(RequestQueueConfig config = RequestQueueConfig::defaults())
        : config_(config)
    {}

    // === Enqueue/Dequeue Operations ===

    // @safe - SpinMutex::lock, VecDeque ops, and rusty::Function::operator()
    // are all @safe in the library; the body's try/catch is not analyzed.
    // Returns true if queued, false if rejected
    bool enqueue(QueuedRequest request) {
        if (!config_.enabled) {
            if (request.callback) {
                try {
                    request.callback(kRequestQueueRejectedError);
                } catch (...) {}
            }
            return false;
        }

        auto guard = queue_.lock().unwrap();

        if (guard->size() >= config_.max_size) {
            switch (config_.overflow_strategy) {
                case OverflowStrategy::DROP_OLDEST:
                    // Remove oldest and proceed
                    if (!guard->is_empty()) {
                        auto oldest = guard->pop_front();
                        // Invoke callback outside lock would be better,
                        // but for simplicity we do it here with error code
                        if (oldest.callback) {
                            try {
                                oldest.callback(kRequestQueueRejectedError);
                            } catch (...) {}
                        }
                    }
                    break;

                case OverflowStrategy::DROP_NEWEST:
                    if (request.callback) {
                        try {
                            request.callback(kRequestQueueRejectedError);
                        } catch (...) {}
                    }
                    return false;  // Reject new request

                case OverflowStrategy::FAIL_FAST:
                    if (request.callback) {
                        try {
                            request.callback(kRequestQueueRejectedError);
                        } catch (...) {}
                    }
                    return false;
            }
        }

        // Set default TTL if not specified
        if (request.ttl_ms == 0) {
            request.ttl_ms = config_.default_ttl_ms;
        }

        guard->push_back(std::move(request));
        return true;
    }

    // @safe - SpinMutex::lock + VecDeque ops are all @safe in the library.
    rusty::Option<QueuedRequest> dequeue() {
        auto guard = queue_.lock().unwrap();

        if (guard->is_empty()) {
            return rusty::None;
        }

        return rusty::Some(guard->pop_front());
    }

    // removed `peek(QueuedRequest&)` — its `out = guard->front();`
    // copy-assignment relied on QueuedRequest being copyable, which is
    // no longer the case after the callback field migrated from
    // std::function to move-only rusty::Function.  The method had no
    // production callers (tests-only, used for inspection of xid
    // post-enqueue); coverage moved to size()/empty().

    // === Expiration ===

    // @safe - SpinMutex::lock + VecDeque::extract_if/size/is_empty/pop_front
    // and rusty::Function ops are all @safe in the library.
    size_t expire_stale() {
        rusty::Vec<rusty::Function<void(int)>> callbacks_to_invoke;
        size_t removed = 0;

        {
            auto guard = queue_.lock().unwrap();

            // Extract expired elements via extract_if. The predicate is
            // const-only (rusty::Function<bool(const T&)>) and cannot mutate
            // the element, so we drain callbacks via pop_front afterward.
            auto expired = guard->extract_if(
                rusty::Function<bool(const QueuedRequest&)>(
                    [](const QueuedRequest& r) { return r.is_expired(); }));
            removed = expired.size();
            while (!expired.is_empty()) {
                auto req = expired.pop_front();
                if (req.callback) {
                    callbacks_to_invoke.push(std::move(req.callback));
                }
            }
        }

        // Invoke callbacks outside lock.  rusty::Function::operator()
        // is non-const, so iterate by mutable reference.
        for (auto& cb : callbacks_to_invoke) {
            try {
                cb(kRequestQueueExpiredError);
            } catch (...) {}
        }

        return removed;
    }

    // === Size and State ===

    // @safe - SpinMutex::lock + VecDeque::size are @safe in the library.
    size_t size() const {
        auto guard = queue_.lock().unwrap();
        return guard->size();
    }

    // @safe - SpinMutex::lock + VecDeque::is_empty are @safe in the library.
    bool empty() const {
        auto guard = queue_.lock().unwrap();
        return guard->is_empty();
    }

    // @safe - SpinMutex::lock + VecDeque::size are @safe in the library.
    bool full() const {
        auto guard = queue_.lock().unwrap();
        return guard->size() >= config_.max_size;
    }

    // @safe - SpinMutex::lock + VecDeque::size are @safe in the library.
    size_t remaining_capacity() const {
        auto guard = queue_.lock().unwrap();
        return config_.max_size > guard->size() ?
               config_.max_size - guard->size() : 0;
    }

    // === Clear and Reset ===

    // @safe - SpinMutex::lock + VecDeque ops + rusty::Function are @safe.
    void clear_all(int error_code = -3) {
        rusty::Vec<rusty::Function<void(int)>> callbacks_to_invoke;

        {
            auto guard = queue_.lock().unwrap();

            for (auto& req : *guard) {
                if (req.callback) {
                    callbacks_to_invoke.push(std::move(req.callback));
                }
            }
            guard->clear();
        }

        // Invoke callbacks outside lock.  rusty::Function::operator()
        // is non-const, so iterate by mutable reference.
        for (auto& cb : callbacks_to_invoke) {
            try {
                cb(error_code);
            } catch (...) {}
        }
    }

    // === Configuration ===

    // @safe - Get configuration (read-only)
    // @lifetime: (&'a) -> &'a
    const RequestQueueConfig& config() const {
        return config_;
    }

    // @safe - Check if queue is enabled
    bool enabled() const {
        return config_.enabled;
    }

    // @safe - Get maximum queue size
    size_t max_size() const {
        return config_.max_size;
    }

    // @safe - SpinMutex::lock is @safe; the body only assigns a
    // RequestQueueConfig POD into the member field under the lock.
    void update_config(const RequestQueueConfig& config) {
        // Take the queue's lock to serialize against in-flight enqueue/dequeue
        // operations so config_ updates are observed atomically with respect
        // to those operations.
        auto guard = queue_.lock().unwrap();
        (void)guard;
        config_ = config;
        // Note: Caller should clear queue before calling if needed.
    }
};

// @safe - Convert overflow strategy to string
inline const char* overflow_strategy_to_string(OverflowStrategy strategy) {
    switch (strategy) {
        case OverflowStrategy::DROP_OLDEST: return "DROP_OLDEST";
        case OverflowStrategy::DROP_NEWEST: return "DROP_NEWEST";
        case OverflowStrategy::FAIL_FAST: return "FAIL_FAST";
        default: return "UNKNOWN";
    }
}


}  // export namespace rrr
