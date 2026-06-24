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
// `CompletionTracker` — server-side LRU tracker of completed request XIDs with
// TTL-based expiry, thread-safe via rusty::Mutex. Authored as inline-Rust DSL:
// the whole class (struct + two #[cpp_ctor] factories + all methods) is the
// source of truth; the transpiler regenerates the matching GEN block. The
// config / entry / status / query-result types are already DSL (above).
//
// Two ctors via #[cpp_ctor]: `new()` (uses CompletionTrackerConfig::defaults()
// — the test's `CompletionTracker tracker_;` default-constructs) and
// `with_config(config)`. The LRU storage is a rusty::VecDeque<CompletedEntry>
// (front = most recent; push_front / pop_back); mid-list removal in
// is_completed / remove / evict_expired uses the VecDeque port's
// `remove(index)`. All bodies are pure rusty (Cell get/set, Mutex guard,
// HashSet + VecDeque ops, index loops) — no raw pointers, syscalls, iterators,
// or operator-overload chains.
#if RUSTYCPP_RUST
struct CompletionTracker {
    config_: Cell<CompletionTrackerConfig>,
    lru_list_: Mutex<VecDeque<CompletedEntry>>,
    completed_set_: Mutex<HashSet<i64>>,
    total_tracked_: Cell<u64>,
    queries_: Cell<u64>,
    query_hits_: Cell<u64>,
    evictions_: Cell<u64>,
}

impl CompletionTracker {
    #[cpp_ctor] fn new() -> CompletionTracker {
        CompletionTracker {
            config_: Cell::new(CompletionTrackerConfig::defaults()),
            lru_list_: Mutex::<VecDeque<CompletedEntry>>::new(VecDeque::<CompletedEntry>::new()),
            completed_set_: Mutex::<HashSet<i64>>::new(HashSet::<i64>::new()),
            total_tracked_: Cell::new(0u64),
            queries_: Cell::new(0u64),
            query_hits_: Cell::new(0u64),
            evictions_: Cell::new(0u64),
        }
    }

    #[cpp_ctor] fn with_config(config: CompletionTrackerConfig) -> CompletionTracker {
        CompletionTracker {
            config_: Cell::new(config),
            lru_list_: Mutex::<VecDeque<CompletedEntry>>::new(VecDeque::<CompletedEntry>::new()),
            completed_set_: Mutex::<HashSet<i64>>::new(HashSet::<i64>::new()),
            total_tracked_: Cell::new(0u64),
            queries_: Cell::new(0u64),
            query_hits_: Cell::new(0u64),
            evictions_: Cell::new(0u64),
        }
    }

    fn enabled(&self) -> bool {
        self.config_.get().enabled
    }

    fn config(&self) -> CompletionTrackerConfig {
        self.config_.get()
    }

    fn set_config(&mut self, config: CompletionTrackerConfig) {
        self.config_.set(config);
    }

    fn mark_completed(&mut self, xid: i64, current_time_ms: u64) {
        let cfg = self.config_.get();
        if !cfg.enabled {
            return;
        }
        let set_guard = self.completed_set_.lock().unwrap();
        if set_guard.contains(xid) {
            return;
        }
        let list_guard = self.lru_list_.lock().unwrap();
        while list_guard.len() >= cfg.max_entries && list_guard.len() > 0usize {
            let oldest_xid: i64 = list_guard.back().xid;
            set_guard.remove(oldest_xid);
            list_guard.pop_back();
            self.evictions_.set(self.evictions_.get() + 1u64);
        }
        list_guard.push_front(CompletedEntry::new(xid, current_time_ms));
        set_guard.insert(xid);
        self.total_tracked_.set(self.total_tracked_.get() + 1u64);
    }

    fn is_completed(&mut self, xid: i64, current_time_ms: u64) -> bool {
        let cfg = self.config_.get();
        self.queries_.set(self.queries_.get() + 1u64);
        if !cfg.enabled {
            return false;
        }
        let set_guard = self.completed_set_.lock().unwrap();
        if !set_guard.contains(xid) {
            return false;
        }
        let list_guard = self.lru_list_.lock().unwrap();
        let mut i: usize = 0;
        while i < list_guard.len() {
            if list_guard[i].xid == xid {
                if list_guard[i].is_expired(current_time_ms, cfg.ttl_ms) {
                    set_guard.remove(xid);
                    list_guard.remove(i);
                    return false;
                }
                self.query_hits_.set(self.query_hits_.get() + 1u64);
                return true;
            }
            i += 1usize;
        }
        false
    }

    fn remove(&mut self, xid: i64) -> bool {
        let set_guard = self.completed_set_.lock().unwrap();
        if !set_guard.contains(xid) {
            return false;
        }
        set_guard.remove(xid);
        let list_guard = self.lru_list_.lock().unwrap();
        let mut i: usize = 0;
        while i < list_guard.len() {
            if list_guard[i].xid == xid {
                list_guard.remove(i);
                return true;
            }
            i += 1usize;
        }
        true
    }

    fn clear(&mut self) {
        let set_guard = self.completed_set_.lock().unwrap();
        let list_guard = self.lru_list_.lock().unwrap();
        set_guard.clear();
        list_guard.clear();
    }

    fn size(&self) -> usize {
        let guard = self.completed_set_.lock().unwrap();
        guard.len()
    }

    fn total_tracked(&self) -> u64 {
        self.total_tracked_.get()
    }

    fn queries(&self) -> u64 {
        self.queries_.get()
    }

    fn query_hits(&self) -> u64 {
        self.query_hits_.get()
    }

    fn hit_rate(&self) -> f64 {
        let q: u64 = self.queries_.get();
        if q == 0u64 {
            return 0.0f64;
        }
        (self.query_hits_.get() as f64) / (q as f64)
    }

    fn evictions(&self) -> u64 {
        self.evictions_.get()
    }

    fn reset_stats(&mut self) {
        self.total_tracked_.set(0u64);
        self.queries_.set(0u64);
        self.query_hits_.set(0u64);
        self.evictions_.set(0u64);
    }

    fn evict_expired(&mut self, current_time_ms: u64) -> usize {
        let cfg = self.config_.get();
        if !cfg.enabled || cfg.ttl_ms == 0u64 {
            return 0usize;
        }
        let set_guard = self.completed_set_.lock().unwrap();
        let list_guard = self.lru_list_.lock().unwrap();
        let mut evicted: usize = 0;
        let mut i: usize = 0;
        while i < list_guard.len() {
            if list_guard[i].is_expired(current_time_ms, cfg.ttl_ms) {
                let xid: i64 = list_guard[i].xid;
                set_guard.remove(xid);
                list_guard.remove(i);
                evicted += 1usize;
            } else {
                i += 1usize;
            }
        }
        self.evictions_.set(self.evictions_.get() + (evicted as u64));
        evicted
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=completion_tracker.tracker version=1 rust_sha256=e0228c274c10531fd46b2b942e217eeef711e71805f5a18c8f3cc03d1c763b55*/
struct CompletionTracker;

struct CompletionTracker {
    rusty::Cell<CompletionTrackerConfig> config_;
    rusty::Mutex<rusty::VecDeque<CompletedEntry>> lru_list_;
    rusty::Mutex<rusty::HashSet<int64_t>> completed_set_;
    rusty::Cell<uint64_t> total_tracked_;
    rusty::Cell<uint64_t> queries_;
    rusty::Cell<uint64_t> query_hits_;
    rusty::Cell<uint64_t> evictions_;

    CompletionTracker();
    CompletionTracker(CompletionTrackerConfig config);
    bool enabled() const;
    CompletionTrackerConfig config() const;
    void set_config(CompletionTrackerConfig config);
    void mark_completed(int64_t xid, uint64_t current_time_ms);
    bool is_completed(int64_t xid, uint64_t current_time_ms);
    bool remove(int64_t xid);
    void clear();
    size_t size() const;
    uint64_t total_tracked() const;
    uint64_t queries() const;
    uint64_t query_hits() const;
    double hit_rate() const;
    uint64_t evictions() const;
    void reset_stats();
    size_t evict_expired(uint64_t current_time_ms);
};


CompletionTracker::CompletionTracker()
    : config_(rusty::Cell<CompletionTrackerConfig>::new_(CompletionTrackerConfig::defaults()))
    , lru_list_(rusty::Mutex<rusty::VecDeque<CompletedEntry>>::new_(rusty::VecDeque<CompletedEntry>::new_()))
    , completed_set_(rusty::Mutex<rusty::HashSet<int64_t>>::new_(rusty::HashSet<int64_t>::new_()))
    , total_tracked_(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , queries_(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , query_hits_(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , evictions_(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
{}

CompletionTracker::CompletionTracker(CompletionTrackerConfig config)
    : config_(rusty::Cell<CompletionTrackerConfig>::new_(std::move(config)))
    , lru_list_(rusty::Mutex<rusty::VecDeque<CompletedEntry>>::new_(rusty::VecDeque<CompletedEntry>::new_()))
    , completed_set_(rusty::Mutex<rusty::HashSet<int64_t>>::new_(rusty::HashSet<int64_t>::new_()))
    , total_tracked_(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , queries_(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , query_hits_(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , evictions_(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
{}

bool CompletionTracker::enabled() const {
    return this->config_.get().enabled;
}

CompletionTrackerConfig CompletionTracker::config() const {
    return this->config_.get();
}

void CompletionTracker::set_config(CompletionTrackerConfig config) {
    this->config_.set(std::move(config));
}

void CompletionTracker::mark_completed(int64_t xid, uint64_t current_time_ms) {
    const auto cfg = this->config_.get();
    if (!cfg.enabled) {
        return;
    }
    auto set_guard = this->completed_set_.lock().unwrap();
    if (rusty::contains(set_guard, std::move(xid))) {
        return;
    }
    auto list_guard = this->lru_list_.lock().unwrap();
    while ((rusty::len(list_guard) >= rusty::detail::deref_if_pointer_like(cfg.max_entries)) && (rusty::len(list_guard) > static_cast<size_t>(0))) {
        int64_t oldest_xid = (*list_guard).back().xid;
        (*set_guard).remove(std::move(oldest_xid));
        (*list_guard).pop_back();
        this->evictions_.set(this->evictions_.get() + static_cast<uint64_t>(1));
    }
    (*list_guard).push_front(CompletedEntry::new_(std::move(xid), std::move(current_time_ms)));
    (*set_guard).insert(std::move(xid));
    this->total_tracked_.set(this->total_tracked_.get() + static_cast<uint64_t>(1));
}

bool CompletionTracker::is_completed(int64_t xid, uint64_t current_time_ms) {
    const auto cfg = this->config_.get();
    this->queries_.set(this->queries_.get() + static_cast<uint64_t>(1));
    if (!cfg.enabled) {
        return false;
    }
    auto set_guard = this->completed_set_.lock().unwrap();
    if (!rusty::contains(set_guard, std::move(xid))) {
        return false;
    }
    auto list_guard = this->lru_list_.lock().unwrap();
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::len(list_guard)) {
        if (rusty::detail::deref_if_pointer_like(list_guard[i].xid) == rusty::detail::deref_if_pointer_like(xid)) {
            if (list_guard[i].is_expired(std::move(current_time_ms), std::move(cfg.ttl_ms))) {
                (*set_guard).remove(std::move(xid));
                (*list_guard).remove(std::move(i));
                return false;
            }
            this->query_hits_.set(this->query_hits_.get() + static_cast<uint64_t>(1));
            return true;
        }
        i += static_cast<size_t>(1);
    }
    return false;
}

bool CompletionTracker::remove(int64_t xid) {
    auto set_guard = this->completed_set_.lock().unwrap();
    if (!rusty::contains(set_guard, std::move(xid))) {
        return false;
    }
    (*set_guard).remove(std::move(xid));
    auto list_guard = this->lru_list_.lock().unwrap();
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::len(list_guard)) {
        if (rusty::detail::deref_if_pointer_like(list_guard[i].xid) == rusty::detail::deref_if_pointer_like(xid)) {
            (*list_guard).remove(std::move(i));
            return true;
        }
        i += static_cast<size_t>(1);
    }
    return true;
}

void CompletionTracker::clear() {
    auto set_guard = this->completed_set_.lock().unwrap();
    auto list_guard = this->lru_list_.lock().unwrap();
    (*set_guard).clear();
    (*list_guard).clear();
}

size_t CompletionTracker::size() const {
    auto guard = this->completed_set_.lock().unwrap();
    return rusty::len(guard);
}

uint64_t CompletionTracker::total_tracked() const {
    return this->total_tracked_.get();
}

uint64_t CompletionTracker::queries() const {
    return this->queries_.get();
}

uint64_t CompletionTracker::query_hits() const {
    return this->query_hits_.get();
}

double CompletionTracker::hit_rate() const {
    const uint64_t q = this->queries_.get();
    if (rusty::detail::deref_if_pointer_like(q) == static_cast<uint64_t>(0)) {
        return 0.0;
    }
    return ((static_cast<double>(this->query_hits_.get()))) / ((static_cast<double>(q)));
}

uint64_t CompletionTracker::evictions() const {
    return this->evictions_.get();
}

void CompletionTracker::reset_stats() {
    this->total_tracked_.set(static_cast<uint64_t>(0));
    this->queries_.set(static_cast<uint64_t>(0));
    this->query_hits_.set(static_cast<uint64_t>(0));
    this->evictions_.set(static_cast<uint64_t>(0));
}

size_t CompletionTracker::evict_expired(uint64_t current_time_ms) {
    const auto cfg = this->config_.get();
    if (!cfg.enabled || (rusty::detail::deref_if_pointer_like(cfg.ttl_ms) == static_cast<uint64_t>(0))) {
        return static_cast<size_t>(0);
    }
    auto set_guard = this->completed_set_.lock().unwrap();
    auto list_guard = this->lru_list_.lock().unwrap();
    size_t evicted = static_cast<size_t>(0);
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::len(list_guard)) {
        if (list_guard[i].is_expired(std::move(current_time_ms), std::move(cfg.ttl_ms))) {
            int64_t xid = list_guard[i].xid;
            (*set_guard).remove(std::move(xid));
            (*list_guard).remove(std::move(i));
            evicted += static_cast<size_t>(1);
        } else {
            i += static_cast<size_t>(1);
        }
    }
    this->evictions_.set(this->evictions_.get() + ((static_cast<uint64_t>(evicted))));
    return std::move(evicted);
}
/*RUSTYCPP:GEN-END id=completion_tracker.tracker*/

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
