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
inline constexpr int kRequestQueueRejectedError = EAGAIN;
inline constexpr int kRequestQueueExpiredError = ETIMEDOUT;

/**
 * A queued RPC request awaiting transmission.
 */
struct QueuedRequest {
    i64 xid;                           // Request transaction ID
    i32 rpc_id;                        // RPC method ID
    std::uint64_t timestamp_us;        // When queued, monotonic microseconds
    uint32_t retry_count;              // Number of retries
    rusty::Arc<Marshal> payload;       // Serialized request data
    rusty::Function<void(int)> callback; // Completion callback (error_code)
    uint32_t ttl_ms;                   // TTL in milliseconds

    // @safe - rusty::sys::time::clock_monotonic_us is @safe.
    QueuedRequest()
        : xid(0)
        , rpc_id(0)
        , timestamp_us(rusty::sys::time::clock_monotonic_us())
        , retry_count(0)
        , payload(rusty::Arc<Marshal>::make())
        , ttl_ms(30000)
    {}

    // @safe - delegates to rusty::sys::time::clock_monotonic_us.
    bool is_expired() const {
        const std::uint64_t now_us = rusty::sys::time::clock_monotonic_us();
        const std::uint64_t elapsed_us = now_us - timestamp_us;
        return (elapsed_us / 1000) > ttl_ms;
    }

    // @safe - delegates to rusty::sys::time::clock_monotonic_us.
    uint32_t age_ms() const {
        const std::uint64_t now_us = rusty::sys::time::clock_monotonic_us();
        return static_cast<uint32_t>((now_us - timestamp_us) / 1000);
    }
};

// Configuration for RequestQueue.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. The `#[cpp_ctor] fn new()`
// lowers to a real default constructor `RequestQueueConfig()` whose
// initializer-list mirrors the previous in-class `= 1000`, `= 30000`,
// etc. defaults — so all existing `RequestQueueConfig config;`
// call-sites keep getting the same defaults without source changes.
//
// `defaults()` / `small()` / `large()` / `disabled()` factories use
// `RequestQueueConfig {}` (empty struct literal -> value-init ->
// calls the cpp_ctor-emitted default ctor) followed by per-field
// assignment. The transpiler maps a populated literal
// `RequestQueueConfig { field: value }` to a designated initializer,
// which requires an aggregate; the cpp_ctor disqualifies the struct
// from aggregate-init, hence the empty-literal-then-mutate idiom.
#if RUSTYCPP_RUST
struct RequestQueueConfig {
    max_size: usize,
    default_ttl_ms: u32,
    overflow_strategy: OverflowStrategy,
    enabled: bool,
}

impl RequestQueueConfig {
    #[cpp_ctor]
    fn new() -> RequestQueueConfig {
        RequestQueueConfig {
            max_size: 1000usize,
            default_ttl_ms: 30000u32,
            overflow_strategy: OverflowStrategy::DROP_OLDEST,
            enabled: true,
        }
    }

    fn defaults() -> RequestQueueConfig {
        RequestQueueConfig {}
    }

    fn small() -> RequestQueueConfig {
        let mut config: RequestQueueConfig = RequestQueueConfig {};
        config.max_size = 10usize;
        config.default_ttl_ms = 5000u32;
        config
    }

    fn large() -> RequestQueueConfig {
        let mut config: RequestQueueConfig = RequestQueueConfig {};
        config.max_size = 10000usize;
        config.default_ttl_ms = 60000u32;
        config
    }

    fn disabled() -> RequestQueueConfig {
        let mut config: RequestQueueConfig = RequestQueueConfig {};
        config.enabled = false;
        config.max_size = 0usize;
        config
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=request_queue.1 version=1 rust_sha256=e7b73afe0660581ce7ad12fd407833889b0c0790f3f55dad4168e8d03bf50686*/
struct RequestQueueConfig;

struct RequestQueueConfig {
    size_t max_size;
    uint32_t default_ttl_ms;
    OverflowStrategy overflow_strategy;
    bool enabled;

    RequestQueueConfig();
    static RequestQueueConfig defaults();
    static RequestQueueConfig small();
    static RequestQueueConfig large();
    static RequestQueueConfig disabled();
};


RequestQueueConfig::RequestQueueConfig()
    : max_size(static_cast<size_t>(1000))
    , default_ttl_ms(static_cast<uint32_t>(30000))
    , overflow_strategy(OverflowStrategy::DROP_OLDEST)
    , enabled(true)
{}

RequestQueueConfig RequestQueueConfig::defaults() {
    return RequestQueueConfig{};
}

RequestQueueConfig RequestQueueConfig::small() {
    RequestQueueConfig config = RequestQueueConfig{};
    config.max_size = static_cast<size_t>(10);
    config.default_ttl_ms = static_cast<uint32_t>(5000);
    return std::move(config);
}

RequestQueueConfig RequestQueueConfig::large() {
    RequestQueueConfig config = RequestQueueConfig{};
    config.max_size = static_cast<size_t>(10000);
    config.default_ttl_ms = static_cast<uint32_t>(60000);
    return std::move(config);
}

RequestQueueConfig RequestQueueConfig::disabled() {
    RequestQueueConfig config = RequestQueueConfig{};
    config.enabled = false;
    config.max_size = static_cast<size_t>(0);
    return std::move(config);
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
