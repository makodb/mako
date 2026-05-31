module;

#include <cstdint>
#include <cstdlib>

#include <rusty/cell.hpp>
#include <rusty/mutex.hpp>
#include <rusty/rusty.hpp>

export module rrr.completion_tracker;

import std;
import rrr.idempotency;

export namespace rrr {


// ===========================================================================
// CompletionTrackerConfig
// ===========================================================================

// Configuration for CompletionTracker.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below
// is the source of truth; the transpiler regenerates the matching
// GEN-BEGIN block. `#[cpp_ctor] fn new()` lowers to a real default
// constructor `CompletionTrackerConfig()` whose initializer-list
// reproduces the previous `= 60000`, `= 100000`, `= true` in-class
// defaults — so `CompletionTrackerConfig cfg;` default-init still
// produces the same values without source changes.
//
// `defaults()` / `small()` / `large()` / `disabled()` factories use
// the empty-literal-then-mutate idiom for the same reason as the
// other config migrations: a populated DSL literal would map to a
// C++ designated initializer, which the cpp_ctor disqualifies.
#if RUSTYCPP_RUST
struct CompletionTrackerConfig {
    ttl_ms: u64,
    max_entries: usize,
    enabled: bool,
}

impl CompletionTrackerConfig {
    #[cpp_ctor]
    fn new() -> CompletionTrackerConfig {
        CompletionTrackerConfig {
            ttl_ms: 60000u64,
            max_entries: 100000usize,
            enabled: true,
        }
    }

    fn defaults() -> CompletionTrackerConfig {
        CompletionTrackerConfig {}
    }

    fn small() -> CompletionTrackerConfig {
        let mut cfg: CompletionTrackerConfig = CompletionTrackerConfig {};
        cfg.ttl_ms = 30000u64;
        cfg.max_entries = 10000usize;
        cfg
    }

    fn large() -> CompletionTrackerConfig {
        let mut cfg: CompletionTrackerConfig = CompletionTrackerConfig {};
        cfg.ttl_ms = 300000u64;
        cfg.max_entries = 1000000usize;
        cfg
    }

    fn disabled() -> CompletionTrackerConfig {
        let mut cfg: CompletionTrackerConfig = CompletionTrackerConfig {};
        cfg.enabled = false;
        cfg
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=completion_tracker.1 version=1 rust_sha256=5f09e5d23be1daaa2008390e8240bb97a167a34ac4b2bf4a003e21dfbb121a82*/
struct CompletionTrackerConfig;

struct CompletionTrackerConfig {
    uint64_t ttl_ms;
    size_t max_entries;
    bool enabled;

    CompletionTrackerConfig();
    static CompletionTrackerConfig defaults();
    static CompletionTrackerConfig small();
    static CompletionTrackerConfig large();
    static CompletionTrackerConfig disabled();
};


CompletionTrackerConfig::CompletionTrackerConfig()
    : ttl_ms(static_cast<uint64_t>(60000))
    , max_entries(static_cast<size_t>(100000))
    , enabled(true)
{}

CompletionTrackerConfig CompletionTrackerConfig::defaults() {
    return CompletionTrackerConfig{};
}

CompletionTrackerConfig CompletionTrackerConfig::small() {
    CompletionTrackerConfig cfg = CompletionTrackerConfig{};
    cfg.ttl_ms = static_cast<uint64_t>(30000);
    cfg.max_entries = static_cast<size_t>(10000);
    return std::move(cfg);
}

CompletionTrackerConfig CompletionTrackerConfig::large() {
    CompletionTrackerConfig cfg = CompletionTrackerConfig{};
    cfg.ttl_ms = static_cast<uint64_t>(300000);
    cfg.max_entries = static_cast<size_t>(1000000);
    return std::move(cfg);
}

CompletionTrackerConfig CompletionTrackerConfig::disabled() {
    CompletionTrackerConfig cfg = CompletionTrackerConfig{};
    cfg.enabled = false;
    return std::move(cfg);
}
/*RUSTYCPP:GEN-END id=completion_tracker.1*/

// ===========================================================================
// CompletedEntry
// ===========================================================================

/**
 * @safe - Entry for tracking completed XIDs.
 *
 * Aggregate POD: no user-declared constructors. The previous default
 * ctor and 2-arg value ctor were dropped in favour of the Rust-style
 * `static new_(...)` factory, which mirrors what the inline-Rust DSL
 * emits for `fn new(...) -> Self` (without `#[cpp_ctor]`). Future DSL
 * migration is now a body-level translation; no `#[cpp_ctor]` needed.
 *
 * Aggregate-init (`CompletedEntry{}`, `CompletedEntry{xid, ts}`) and
 * designated-init still work because the struct remains an aggregate.
 */
struct CompletedEntry {
    int64_t xid = 0;
    uint64_t timestamp_ms = 0;

    // @safe - Rust-style factory matching the DSL `fn new(x, ts) -> Self` form.
    static CompletedEntry new_(int64_t x, uint64_t ts) {
        return CompletedEntry{.xid = x, .timestamp_ms = ts};
    }

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
// @safe - LRU completion-XID tracker backed by rusty::Mutex<std::list> and
// rusty::Mutex<HashSet>. All public methods are pure rusty operations
// (Cell get/set, Mutex lock, HashSet/list mutations). No raw pointers,
// syscalls, or operator-overload chains.
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
        list_guard->push_front(CompletedEntry::new_(xid, current_time_ms));
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


}  // export namespace rrr
