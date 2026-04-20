module;

#include <rusty/rusty.hpp>

#include <rusty/arc.hpp>
#include <rusty/option.hpp>

export module rrr:rpc.request_queue;

import <chrono>;
import <cerrno>;
import <list>;
import <functional>;
import <mutex>;

import :base.basetypes;
import :misc.marshal;


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
    std::chrono::steady_clock::time_point timestamp;  // When queued
    uint32_t retry_count;              // Number of retries
    rusty::Arc<Marshal> payload;       // Serialized request data
    std::function<void(int)> callback; // Completion callback (error_code)
    uint32_t ttl_ms;                   // TTL in milliseconds

    // @unsafe - Constructor uses std::chrono
    QueuedRequest()
        : xid(0)
        , rpc_id(0)
        , timestamp(std::chrono::steady_clock::now())
        , retry_count(0)
        , payload(rusty::Arc<Marshal>::make())
        , ttl_ms(30000)
    {}

    // @unsafe - Uses std::chrono
    bool is_expired() const {
        // @unsafe { std::chrono operations }
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - timestamp).count();
        return static_cast<uint32_t>(elapsed_ms) > ttl_ms;
    }

    // @unsafe - Uses std::chrono
    uint32_t age_ms() const {
        // @unsafe { std::chrono operations }
        auto now = std::chrono::steady_clock::now();
        return static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - timestamp).count());
    }
};

/**
 * Configuration for RequestQueue.
 */
struct RequestQueueConfig {
    size_t max_size = 1000;            // Maximum queue entries
    uint32_t default_ttl_ms = 30000;   // 30 second default TTL
    OverflowStrategy overflow_strategy = OverflowStrategy::DROP_OLDEST;
    bool enabled = true;

    // @unsafe - Returns struct by value
    static RequestQueueConfig defaults() {
        // @unsafe { struct construction }
        return RequestQueueConfig{};
    }

    // @unsafe - Returns struct by value
    static RequestQueueConfig small() {
        // @unsafe { struct construction }
        RequestQueueConfig config;
        config.max_size = 10;
        config.default_ttl_ms = 5000;
        return config;
    }

    // @unsafe - Returns struct by value
    static RequestQueueConfig large() {
        // @unsafe { struct construction }
        RequestQueueConfig config;
        config.max_size = 10000;
        config.default_ttl_ms = 60000;
        return config;
    }

    // @unsafe - Returns struct by value
    static RequestQueueConfig disabled() {
        // @unsafe { struct construction }
        RequestQueueConfig config;
        config.enabled = false;
        config.max_size = 0;
        return config;
    }
};

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
class RequestQueue {
private:
    RequestQueueConfig config_;
    std::list<QueuedRequest> queue_;  // Using list to avoid move assignment issues with Marshal
    // @unsafe { std::mutex for thread-safe concurrent access }
    mutable std::mutex mutex_;

public:
    // @unsafe - Constructor uses defaults() which returns struct
    explicit RequestQueue(RequestQueueConfig config = RequestQueueConfig::defaults())
        : config_(config)
    {}

    // === Enqueue/Dequeue Operations ===

    // @unsafe - Uses std::list and std::mutex
    // Returns true if queued, false if rejected
    bool enqueue(QueuedRequest request) {
        if (!config_.enabled) {
            if (request.callback) {
                // @unsafe { callback invocation }
                try {
                    request.callback(kRequestQueueRejectedError);
                } catch (...) {}
            }
            return false;
        }

        // @unsafe { std::mutex lock, std::list operations }
        std::lock_guard<std::mutex> lock(mutex_);

        if (queue_.size() >= config_.max_size) {
            switch (config_.overflow_strategy) {
                case OverflowStrategy::DROP_OLDEST:
                    // Remove oldest and proceed
                    if (!queue_.empty()) {
                        auto oldest = std::move(queue_.front());
                        queue_.pop_front();
                        // Invoke callback outside lock would be better,
                        // but for simplicity we do it here with error code
                        if (oldest.callback) {
                            // @unsafe { callback invocation }
                            try {
                                oldest.callback(kRequestQueueRejectedError);
                            } catch (...) {}
                        }
                    }
                    break;

                case OverflowStrategy::DROP_NEWEST:
                    if (request.callback) {
                        // @unsafe { callback invocation }
                        try {
                            request.callback(kRequestQueueRejectedError);
                        } catch (...) {}
                    }
                    return false;  // Reject new request

                case OverflowStrategy::FAIL_FAST:
                    if (request.callback) {
                        // @unsafe { callback invocation }
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

        // @unsafe { std::list push_back }
        queue_.push_back(std::move(request));
        return true;
    }

    // @unsafe - Uses std::list and std::mutex
    rusty::Option<QueuedRequest> dequeue() {
        // @unsafe { std::mutex lock, std::list operations }
        std::lock_guard<std::mutex> lock(mutex_);

        if (queue_.empty()) {
            return rusty::None;
        }

        auto request = std::move(queue_.front());
        queue_.pop_front();
        return rusty::Some(std::move(request));
    }

    // @unsafe - Uses std::list and std::mutex
    // Note: Returns pointer that should be used immediately while lock is held
    // For thread-safety, prefer dequeue() instead
    bool peek(QueuedRequest& out) const {
        // @unsafe { std::mutex lock, std::list operations }
        std::lock_guard<std::mutex> lock(mutex_);

        if (queue_.empty()) {
            return false;
        }

        // @unsafe { struct assignment }
        out = queue_.front();
        return true;
    }

    // === Expiration ===

    // @unsafe - Uses std::list and std::mutex
    size_t expire_stale() {
        rusty::Vec<std::function<void(int)>> callbacks_to_invoke;
        size_t removed = 0;

        {
            // @unsafe { std::mutex lock }
            std::lock_guard<std::mutex> lock(mutex_);

            size_t original_size = queue_.size();
            auto it = queue_.begin();
            while (it != queue_.end()) {
                if (it->is_expired()) {
                    if (it->callback) {
                        callbacks_to_invoke.push(std::move(it->callback));
                    }
                    it = queue_.erase(it);
                } else {
                    ++it;
                }
            }

            removed = original_size - queue_.size();
        }

        // Invoke callbacks outside lock
        for (const auto& cb : callbacks_to_invoke) {
            // @unsafe { callback invocation }
            try {
                cb(kRequestQueueExpiredError);
            } catch (...) {}
        }

        return removed;
    }

    // === Size and State ===

    // @unsafe - Uses std::list and std::mutex
    size_t size() const {
        // @unsafe { std::mutex lock, std::list::size }
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    // @unsafe - Uses std::list and std::mutex
    bool empty() const {
        // @unsafe { std::mutex lock, std::list::empty }
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    // @unsafe - Uses std::list and std::mutex
    bool full() const {
        // @unsafe { std::mutex lock, std::list::size }
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size() >= config_.max_size;
    }

    // @unsafe - Uses std::list and std::mutex
    size_t remaining_capacity() const {
        // @unsafe { std::mutex lock, std::list::size }
        std::lock_guard<std::mutex> lock(mutex_);
        return config_.max_size > queue_.size() ?
               config_.max_size - queue_.size() : 0;
    }

    // === Clear and Reset ===

    // @unsafe - Uses std::list and std::mutex
    void clear_all(int error_code = -3) {
        rusty::Vec<std::function<void(int)>> callbacks_to_invoke;

        {
            // @unsafe { std::mutex lock, std::list operations }
            std::lock_guard<std::mutex> lock(mutex_);

            for (auto& req : queue_) {
                if (req.callback) {
                    callbacks_to_invoke.push(std::move(req.callback));
                }
            }
            queue_.clear();
        }

        // Invoke callbacks outside lock
        for (const auto& cb : callbacks_to_invoke) {
            // @unsafe { callback invocation }
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

    // @unsafe - Update configuration (clears queue if not empty)
    void update_config(const RequestQueueConfig& config) {
        // @unsafe { std::mutex lock, config assignment }
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;
        // Note: Caller should clear queue before calling if needed
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

} // namespace rrr
