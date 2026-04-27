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
/**
 * Request Completion Tracking for RPC (Phase 4.3)
 *
 * Tracks completed request XIDs on the server side to allow clients
 * to query if a request completed after reconnection.
 *
 * Integrates with IdempotencyCache for efficient storage.
 *
 * Use case:
 *   1. Client sends request with XID
 *   2. Server processes request, marks XID as completed
 *   3. Network failure - client reconnects
 *   4. Client queries: "Did XID X complete?"
 *   5. Server responds: yes (with cached result) or no
 *
 * Thread-safe via rusty::Mutex.
 */


#include <rusty/cell.hpp>
#include <rusty/mutex.hpp>




#include "idempotency.hpp"

namespace rrr {

// ===========================================================================
// CompletionTrackerConfig
// ===========================================================================

/**
 * @safe - Configuration for CompletionTracker
 */
struct CompletionTrackerConfig {
    uint64_t ttl_ms = 60000;        // Time-to-live for completed XIDs (60 seconds)
    size_t max_entries = 100000;    // Maximum XIDs to track
    bool enabled = true;            // Enable/disable tracking

    // @safe - Default constructor
    CompletionTrackerConfig() = default;

    // @safe - Copy constructor
    CompletionTrackerConfig(const CompletionTrackerConfig&) = default;

    // @safe - Copy assignment
    CompletionTrackerConfig& operator=(const CompletionTrackerConfig&) = default;

    // @safe - Default preset
    static CompletionTrackerConfig defaults() {
        return CompletionTrackerConfig{};
    }

    // @safe - Small preset (fewer entries, shorter TTL)
    static CompletionTrackerConfig small() {
        CompletionTrackerConfig cfg;
        cfg.ttl_ms = 30000;      // 30 seconds
        cfg.max_entries = 10000;
        return cfg;
    }

    // @safe - Large preset (more entries, longer TTL)
    static CompletionTrackerConfig large() {
        CompletionTrackerConfig cfg;
        cfg.ttl_ms = 300000;     // 5 minutes
        cfg.max_entries = 1000000;
        return cfg;
    }

    // @safe - Disabled preset
    static CompletionTrackerConfig disabled() {
        CompletionTrackerConfig cfg;
        cfg.enabled = false;
        return cfg;
    }
};

// ===========================================================================
// CompletedEntry
// ===========================================================================

/**
 * @safe - Entry for tracking completed XIDs
 */
struct CompletedEntry {
    int64_t xid = 0;
    uint64_t timestamp_ms = 0;

    // @safe - Default constructor
    CompletedEntry() = default;

    // @safe - Constructor with values
    CompletedEntry(int64_t x, uint64_t ts) : xid(x), timestamp_ms(ts) {}

    // @safe - Check if entry has expired
    bool is_expired(uint64_t current_time_ms, uint64_t ttl_ms) const {
        if (ttl_ms == 0) return false;  // No expiration
        return current_time_ms > timestamp_ms + ttl_ms;
    }
};

// ===========================================================================
// CompletionTracker
// ===========================================================================

/**
 * @unsafe - Server-side tracker for completed request XIDs
 *
 * Uses LRU eviction when at capacity. Thread-safe via rusty::Mutex.
 *
 * Usage on Server:
 *   1. After processing request: tracker.mark_completed(xid, timestamp)
 *   2. On client reconnection query: tracker.is_completed(xid)
 *
 * Can be used standalone or integrated with IdempotencyCache.
 */
class CompletionTracker {
    // Configuration
    rusty::Cell<CompletionTrackerConfig> config_;

    // LRU list of completed XIDs (front = most recent)
    using LruList = std::list<CompletedEntry>;
    rusty::Mutex<LruList> lru_list_;

    // Set for O(1) lookup
    rusty::Mutex<rusty::HashSet<int64_t>> completed_set_;

    // Statistics
    rusty::Cell<uint64_t> total_tracked_{0};
    rusty::Cell<uint64_t> queries_{0};
    rusty::Cell<uint64_t> query_hits_{0};
    rusty::Cell<uint64_t> evictions_{0};

public:
    // @safe - Constructor
    explicit CompletionTracker(const CompletionTrackerConfig& config = CompletionTrackerConfig::defaults())
        : config_(config), lru_list_(LruList{}), completed_set_(rusty::HashSet<int64_t>{}) {}

    // @safe - Check if tracking is enabled
    bool enabled() const {
        return config_.get().enabled;
    }

    // @safe - Get configuration
    CompletionTrackerConfig config() const {
        return config_.get();
    }

    // @safe - Update configuration
    void set_config(const CompletionTrackerConfig& config) {
        config_.set(config);
    }

    /**
     * @unsafe - Mark a request as completed
     *
     * @param xid The request XID that completed
     * @param current_time_ms Current timestamp for TTL tracking
     */
    void mark_completed(int64_t xid, uint64_t current_time_ms) {
        auto cfg = config_.get();
        if (!cfg.enabled) {
            return;
        }

        auto set_guard = completed_set_.lock().unwrap();

        // Skip if already tracked
        if (set_guard->contains(xid)) {
            return;
        }

        auto list_guard = lru_list_.lock().unwrap();

        // Evict if at capacity
        while (list_guard->size() >= cfg.max_entries && !list_guard->empty()) {
            auto& oldest = list_guard->back();
            set_guard->remove(oldest.xid);
            list_guard->pop_back();
            evictions_.set(evictions_.get() + 1);
        }

        // Add new entry
        list_guard->push_front(CompletedEntry{xid, current_time_ms});
        set_guard->insert(xid);
        total_tracked_.set(total_tracked_.get() + 1);
    }

    /**
     * @unsafe - Check if a request XID was completed
     *
     * @param xid The request XID to check
     * @param current_time_ms Current timestamp for TTL check
     * @return true if the request was completed and entry hasn't expired
     */
    bool is_completed(int64_t xid, uint64_t current_time_ms) {
        auto cfg = config_.get();
        queries_.set(queries_.get() + 1);

        if (!cfg.enabled) {
            return false;
        }

        auto set_guard = completed_set_.lock().unwrap();

        // Quick check if XID exists
        if (!set_guard->contains(xid)) {
            return false;
        }

        // Need to check TTL by finding the entry
        auto list_guard = lru_list_.lock().unwrap();

        for (auto it = list_guard->begin(); it != list_guard->end(); ++it) {
            if (it->xid == xid) {
                if (it->is_expired(current_time_ms, cfg.ttl_ms)) {
                    // Expired - remove it
                    set_guard->remove(xid);
                    list_guard->erase(it);
                    return false;
                }
                query_hits_.set(query_hits_.get() + 1);
                return true;
            }
        }

        // Should not happen if set is in sync with list
        return false;
    }

    /**
     * @unsafe - Remove a completed entry (for invalidation)
     */
    bool remove(int64_t xid) {
        auto set_guard = completed_set_.lock().unwrap();

        if (!set_guard->contains(xid)) {
            return false;
        }

        set_guard->remove(xid);

        auto list_guard = lru_list_.lock().unwrap();
        for (auto it = list_guard->begin(); it != list_guard->end(); ++it) {
            if (it->xid == xid) {
                list_guard->erase(it);
                break;
            }
        }

        return true;
    }

    /**
     * @safe - Clear all tracked completions
     */
    void clear() {
        auto set_guard = completed_set_.lock().unwrap();
        auto list_guard = lru_list_.lock().unwrap();

        set_guard->clear();
        list_guard->clear();
    }

    // === Statistics ===

    // @safe - Get number of currently tracked XIDs
    size_t size() const {
        auto guard = completed_set_.lock().unwrap();
        return guard->len();
    }

    // @safe - Get total XIDs ever tracked
    uint64_t total_tracked() const {
        return total_tracked_.get();
    }

    // @safe - Get number of queries
    uint64_t queries() const {
        return queries_.get();
    }

    // @safe - Get number of query hits
    uint64_t query_hits() const {
        return query_hits_.get();
    }

    // @safe - Get hit rate (0.0 to 1.0)
    double hit_rate() const {
        uint64_t q = queries_.get();
        if (q == 0) return 0.0;
        return static_cast<double>(query_hits_.get()) / static_cast<double>(q);
    }

    // @safe - Get eviction count
    uint64_t evictions() const {
        return evictions_.get();
    }

    // @safe - Reset statistics
    void reset_stats() {
        total_tracked_.set(0);
        queries_.set(0);
        query_hits_.set(0);
        evictions_.set(0);
    }

    /**
     * @unsafe - Evict expired entries
     *
     * Call periodically to clean up stale entries.
     * Returns number of entries evicted.
     */
    size_t evict_expired(uint64_t current_time_ms) {
        auto cfg = config_.get();
        if (!cfg.enabled || cfg.ttl_ms == 0) {
            return 0;
        }

        auto set_guard = completed_set_.lock().unwrap();
        auto list_guard = lru_list_.lock().unwrap();

        size_t evicted = 0;
        auto it = list_guard->begin();
        while (it != list_guard->end()) {
            if (it->is_expired(current_time_ms, cfg.ttl_ms)) {
                set_guard->remove(it->xid);
                it = list_guard->erase(it);
                evicted++;
            } else {
                ++it;
            }
        }

        evictions_.set(evictions_.get() + evicted);
        return evicted;
    }
};

// ===========================================================================
// CompletionQueryResult
// ===========================================================================

/**
 * @safe - Result of a completion query
 */
enum class CompletionStatus : uint8_t {
    NOT_FOUND = 0,      // XID not in completion log
    COMPLETED = 1,      // XID completed successfully
    COMPLETED_WITH_ERROR = 2,  // XID completed with error
    EXPIRED = 3         // XID was completed but entry expired
};

/**
 * @safe - Result struct for completion queries
 */
struct CompletionQueryResult {
    CompletionStatus status = CompletionStatus::NOT_FOUND;
    int32_t error_code = 0;         // Only valid if COMPLETED or COMPLETED_WITH_ERROR
    bool has_cached_response = false;  // True if IdempotencyCache has the response

    // @safe - Default constructor
    CompletionQueryResult() = default;

    // @safe - Create NOT_FOUND result
    static CompletionQueryResult not_found() {
        return CompletionQueryResult{};
    }

    // @safe - Create COMPLETED result
    static CompletionQueryResult completed(int32_t err_code = 0, bool has_response = false) {
        CompletionQueryResult r;
        r.status = (err_code == 0) ? CompletionStatus::COMPLETED : CompletionStatus::COMPLETED_WITH_ERROR;
        r.error_code = err_code;
        r.has_cached_response = has_response;
        return r;
    }

    // @safe - Create EXPIRED result
    static CompletionQueryResult expired() {
        CompletionQueryResult r;
        r.status = CompletionStatus::EXPIRED;
        return r;
    }

    // @safe - Check if completed (with or without error)
    bool is_completed() const {
        return status == CompletionStatus::COMPLETED ||
               status == CompletionStatus::COMPLETED_WITH_ERROR;
    }
};

// @safe - Convert status to string for logging
inline const char* completion_status_to_string(CompletionStatus status) {
    switch (status) {
        case CompletionStatus::NOT_FOUND: return "NOT_FOUND";
        case CompletionStatus::COMPLETED: return "COMPLETED";
        case CompletionStatus::COMPLETED_WITH_ERROR: return "COMPLETED_WITH_ERROR";
        case CompletionStatus::EXPIRED: return "EXPIRED";
        default: return "UNKNOWN";
    }
}

} // namespace rrr
