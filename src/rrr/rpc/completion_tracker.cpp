module;

#include <cstdint>
#include <cstdlib>

#include <rusty/cell.hpp>
#include <rusty/mutex.hpp>
#include <rusty/rusty.hpp>

export module rrr.completion_tracker;

import std;
import rusty;
import rrr.idempotency;

export namespace rrr {


// ===========================================================================
// CompletionTrackerConfig
// ===========================================================================

// Configuration for CompletionTracker.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below
// is the source of truth; the transpiler regenerates the matching
// GEN-BEGIN block. The plain `fn new()` lowers to a
// `static CompletionTrackerConfig new_()` factory.
//
// Now that there is no cpp_ctor, CompletionTrackerConfig is a pure
// aggregate; the preset bodies use the populated DSL literal form
// `CompletionTrackerConfig { ttl_ms: ..., ... }` which lowers to a
// clean designated initializer.
#if RUSTYCPP_RUST
struct CompletionTrackerConfig {
    ttl_ms: u64,
    max_entries: usize,
    enabled: bool,
}

impl CompletionTrackerConfig {
    fn new() -> CompletionTrackerConfig {
        CompletionTrackerConfig {
            ttl_ms: 60000u64,
            max_entries: 100000usize,
            enabled: true,
        }
    }

    fn defaults() -> CompletionTrackerConfig {
        CompletionTrackerConfig::new()
    }

    fn small() -> CompletionTrackerConfig {
        CompletionTrackerConfig {
            ttl_ms: 30000u64,
            max_entries: 10000usize,
            enabled: true,
        }
    }

    fn large() -> CompletionTrackerConfig {
        CompletionTrackerConfig {
            ttl_ms: 300000u64,
            max_entries: 1000000usize,
            enabled: true,
        }
    }

    fn disabled() -> CompletionTrackerConfig {
        CompletionTrackerConfig {
            ttl_ms: 60000u64,
            max_entries: 100000usize,
            enabled: false,
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=completion_tracker.1 version=1 rust_sha256=7e32fc065aab499e699818c6fb08acf209183d782c763409b341bb17eead3316*/
struct CompletionTrackerConfig;

struct CompletionTrackerConfig {
    uint64_t ttl_ms;
    size_t max_entries;
    bool enabled;

    static CompletionTrackerConfig new_();
    static CompletionTrackerConfig defaults();
    static CompletionTrackerConfig small();
    static CompletionTrackerConfig large();
    static CompletionTrackerConfig disabled();
};


CompletionTrackerConfig CompletionTrackerConfig::new_() {
    return CompletionTrackerConfig{.ttl_ms = static_cast<uint64_t>(60000), .max_entries = static_cast<size_t>(100000), .enabled = true};
}

CompletionTrackerConfig CompletionTrackerConfig::defaults() {
    return CompletionTrackerConfig::new_();
}

CompletionTrackerConfig CompletionTrackerConfig::small() {
    return CompletionTrackerConfig{.ttl_ms = static_cast<uint64_t>(30000), .max_entries = static_cast<size_t>(10000), .enabled = true};
}

CompletionTrackerConfig CompletionTrackerConfig::large() {
    return CompletionTrackerConfig{.ttl_ms = static_cast<uint64_t>(300000), .max_entries = static_cast<size_t>(1000000), .enabled = true};
}

CompletionTrackerConfig CompletionTrackerConfig::disabled() {
    return CompletionTrackerConfig{.ttl_ms = static_cast<uint64_t>(60000), .max_entries = static_cast<size_t>(100000), .enabled = false};
}
/*RUSTYCPP:GEN-END id=completion_tracker.1*/

// ===========================================================================
// CompletedEntry
// ===========================================================================

/**
 * @safe - Entry for tracking completed XIDs.
 *
 * Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
 * the source of truth; the transpiler regenerates the matching
 * `RUSTYCPP:GEN-BEGIN ... END` block. `fn new(x, ts) -> Self` lowers
 * to a static `CompletedEntry::new_(x, ts)` factory.
 *
 * The DSL emits a pure C++20 aggregate (no in-class default
 * initializers, no user-declared ctors). Brace-init and designated-
 * init still work. Default-construct (`CompletedEntry e;`) is now
 * uninitialized; the one test that relied on `{0, 0}` defaults moves
 * to an explicit `CompletedEntry::new_(0, 0)`.
 */
#if RUSTYCPP_RUST
struct CompletedEntry {
    xid: i64,
    timestamp_ms: u64,
}

impl CompletedEntry {
    fn new(x: i64, ts: u64) -> CompletedEntry {
        CompletedEntry { xid: x, timestamp_ms: ts }
    }

    fn is_expired(&self, current_time_ms: u64, ttl_ms: u64) -> bool {
        if ttl_ms == 0u64 { return false; }
        current_time_ms > self.timestamp_ms + ttl_ms
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=completion_tracker.2 version=1 rust_sha256=a8f4fe4b150667270eeed4db72615d281ea9b0c0ba0d468201cf4e60c3e856c3*/
struct CompletedEntry;

struct CompletedEntry {
    int64_t xid;
    uint64_t timestamp_ms;

    static CompletedEntry new_(int64_t x, uint64_t ts);
    bool is_expired(uint64_t current_time_ms, uint64_t ttl_ms) const;
};


CompletedEntry CompletedEntry::new_(int64_t x, uint64_t ts) {
    return CompletedEntry{.xid = std::move(x), .timestamp_ms = std::move(ts)};
}

bool CompletedEntry::is_expired(uint64_t current_time_ms, uint64_t ttl_ms) const {
    if (rusty::detail::deref_if_pointer_like(ttl_ms) == static_cast<uint64_t>(0)) {
        return false;
    }
    return rusty::detail::deref_if_pointer_like(current_time_ms) > (rusty::detail::deref_if_pointer_like(this->timestamp_ms) + rusty::detail::deref_if_pointer_like(ttl_ms));
}
/*RUSTYCPP:GEN-END id=completion_tracker.2*/

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
// `CompletionStatus` — result of a completion query. Authored as
// inline Rust DSL: the `#if RUSTYCPP_RUST` block below is the source
// of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
#if RUSTYCPP_RUST
#[repr(u8)]
enum CompletionStatus {
    NOT_FOUND = 0,
    COMPLETED = 1,
    COMPLETED_WITH_ERROR = 2,
    EXPIRED = 3,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=completion_tracker.status version=1 rust_sha256=5a8ff53e7d4b35269cb9d71788d0564ed2bb5f64332622e6888d5048af0616fd*/
enum class CompletionStatus;
constexpr CompletionStatus CompletionStatus_NOT_FOUND();
constexpr CompletionStatus CompletionStatus_COMPLETED();
constexpr CompletionStatus CompletionStatus_COMPLETED_WITH_ERROR();
constexpr CompletionStatus CompletionStatus_EXPIRED();

enum class CompletionStatus {
    NOT_FOUND = 0,
    COMPLETED = 1,
    COMPLETED_WITH_ERROR = 2,
    EXPIRED = 3
};
inline constexpr CompletionStatus CompletionStatus_NOT_FOUND() { return CompletionStatus::NOT_FOUND; }
inline constexpr CompletionStatus CompletionStatus_COMPLETED() { return CompletionStatus::COMPLETED; }
inline constexpr CompletionStatus CompletionStatus_COMPLETED_WITH_ERROR() { return CompletionStatus::COMPLETED_WITH_ERROR; }
inline constexpr CompletionStatus CompletionStatus_EXPIRED() { return CompletionStatus::EXPIRED; }
/*RUSTYCPP:GEN-END id=completion_tracker.status*/

/**
 * @safe - Result struct for completion queries.
 *
 * Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
 * the source of truth; the transpiler regenerates the matching
 * `RUSTYCPP:GEN-BEGIN ... END` block. `fn new() -> Self` lowers to a
 * `static CompletionQueryResult new_()` factory (the NOT_FOUND
 * default); `not_found`/`completed`/`expired` are explicit named
 * factories. Both `completed`'s default args (`err_code = 0`,
 * `has_response = false`) are dropped — every existing caller already
 * passes both args explicitly.
 */
#if RUSTYCPP_RUST
struct CompletionQueryResult {
    status: CompletionStatus,
    error_code: i32,
    has_cached_response: bool,
}

impl CompletionQueryResult {
    fn new() -> CompletionQueryResult {
        CompletionQueryResult {
            status: CompletionStatus::NOT_FOUND,
            error_code: 0i32,
            has_cached_response: false,
        }
    }

    fn not_found() -> CompletionQueryResult {
        CompletionQueryResult::new()
    }

    fn completed(err_code: i32, has_response: bool) -> CompletionQueryResult {
        let s: CompletionStatus = if err_code == 0i32 {
            CompletionStatus::COMPLETED
        } else {
            CompletionStatus::COMPLETED_WITH_ERROR
        };
        CompletionQueryResult { status: s, error_code: err_code, has_cached_response: has_response }
    }

    fn expired() -> CompletionQueryResult {
        CompletionQueryResult {
            status: CompletionStatus::EXPIRED,
            error_code: 0i32,
            has_cached_response: false,
        }
    }

    fn is_completed(&self) -> bool {
        self.status == CompletionStatus::COMPLETED || self.status == CompletionStatus::COMPLETED_WITH_ERROR
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=completion_tracker.3 version=1 rust_sha256=6742787d7b990417abb19cf8a8c18444d1bc7ba24218f8e51589d36d4426058a*/
struct CompletionQueryResult;

struct CompletionQueryResult {
    CompletionStatus status;
    int32_t error_code;
    bool has_cached_response;

    static CompletionQueryResult new_();
    static CompletionQueryResult not_found();
    static CompletionQueryResult completed(int32_t err_code, bool has_response);
    static CompletionQueryResult expired();
    bool is_completed() const;
};


CompletionQueryResult CompletionQueryResult::new_() {
    return CompletionQueryResult{.status = rusty::clone(rusty::clone(CompletionStatus::NOT_FOUND)), .error_code = static_cast<int32_t>(0), .has_cached_response = false};
}

CompletionQueryResult CompletionQueryResult::not_found() {
    return CompletionQueryResult::new_();
}

CompletionQueryResult CompletionQueryResult::completed(int32_t err_code, bool has_response) {
    CompletionStatus s = (rusty::detail::deref_if_pointer_like(err_code) == static_cast<int32_t>(0) ? rusty::clone(CompletionStatus::COMPLETED) : rusty::clone(CompletionStatus::COMPLETED_WITH_ERROR));
    return CompletionQueryResult{.status = std::move(s), .error_code = std::move(err_code), .has_cached_response = std::move(has_response)};
}

CompletionQueryResult CompletionQueryResult::expired() {
    return CompletionQueryResult{.status = rusty::clone(rusty::clone(CompletionStatus::EXPIRED)), .error_code = static_cast<int32_t>(0), .has_cached_response = false};
}

bool CompletionQueryResult::is_completed() const {
    return (rusty::detail::deref_if_pointer_like(this->status) == rusty::clone(CompletionStatus::COMPLETED)) || (rusty::detail::deref_if_pointer_like(this->status) == rusty::clone(CompletionStatus::COMPLETED_WITH_ERROR));
}
/*RUSTYCPP:GEN-END id=completion_tracker.3*/

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
