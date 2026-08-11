module;

#include <cstdint>
#include <cstdlib>

#include <rusty/mutex.hpp>
#include <rusty/rusty.hpp>
#include <rusty/sync/atomic.hpp>

export module rrr.completion_tracker;

import std;
import rusty;

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
use std::collections::{HashSet, VecDeque};
use std::sync::atomic::AtomicU64;
use std::sync::atomic::Ordering;
use std::sync::Mutex;

#[repr(C)]
#[cfg_attr(not(any()), derive(Clone, Copy))]
pub struct CompletionTrackerConfig {
    pub ttl_ms: u64,
    pub max_entries: usize,
    pub enabled: bool,
}

impl CompletionTrackerConfig {
    pub fn new() -> CompletionTrackerConfig {
        CompletionTrackerConfig {
            ttl_ms: 60000u64,
            max_entries: 100000usize,
            enabled: true,
        }
    }

    pub fn defaults() -> CompletionTrackerConfig {
        CompletionTrackerConfig::new()
    }

    pub fn small() -> CompletionTrackerConfig {
        CompletionTrackerConfig {
            ttl_ms: 30000u64,
            max_entries: 10000usize,
            enabled: true,
        }
    }

    pub fn large() -> CompletionTrackerConfig {
        CompletionTrackerConfig {
            ttl_ms: 300000u64,
            max_entries: 1000000usize,
            enabled: true,
        }
    }

    pub fn disabled() -> CompletionTrackerConfig {
        CompletionTrackerConfig {
            ttl_ms: 60000u64,
            max_entries: 100000usize,
            enabled: false,
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=completion_tracker.1 version=1 rust_sha256=60c59fb827b8b892f219e58326898ed3e109756b328828d2a69a757990a8892a*/
struct CompletionTrackerConfig;

using rusty::HashSet;
using rusty::VecDeque;

using rusty::sync::atomic::AtomicU64;

using rusty::sync::atomic::Ordering;

using rusty::Mutex;

struct CompletionTrackerConfig {
    uint64_t ttl_ms;
    size_t max_entries;
    bool enabled;

    static CompletionTrackerConfig new_();
    static CompletionTrackerConfig defaults();
    static CompletionTrackerConfig small();
    static CompletionTrackerConfig large();
    static CompletionTrackerConfig disabled();
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
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
#[repr(C)]
pub struct CompletedEntry {
    pub xid: i64,
    pub timestamp_ms: u64,
}

impl CompletedEntry {
    pub fn new(x: i64, ts: u64) -> CompletedEntry {
        CompletedEntry { xid: x, timestamp_ms: ts }
    }

    pub fn is_expired(&self, current_time_ms: u64, ttl_ms: u64) -> bool {
        if ttl_ms == 0u64 { return false; }
        current_time_ms > self.timestamp_ms.wrapping_add(ttl_ms)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=completion_tracker.2 version=1 rust_sha256=d118c50b3f9ce25edc668f69eaa26437072c6a6364bf716aab53d733bcd5f2d3*/
struct CompletedEntry;

struct CompletedEntry {
    int64_t xid;
    uint64_t timestamp_ms;

    static CompletedEntry new_(int64_t x, uint64_t ts);
    bool is_expired(uint64_t current_time_ms, uint64_t ttl_ms) const;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};


CompletedEntry CompletedEntry::new_(int64_t x, uint64_t ts) {
    return CompletedEntry{.xid = std::move(x), .timestamp_ms = std::move(ts)};
}

bool CompletedEntry::is_expired(uint64_t current_time_ms, uint64_t ttl_ms) const {
    if (rusty::detail::deref_if_pointer_like(ttl_ms) == static_cast<uint64_t>(0)) {
        return false;
    }
    return rusty::detail::deref_if_pointer_like(current_time_ms) > rusty::wrapping_add(this->timestamp_ms, static_cast<std::remove_cvref_t<decltype(this->timestamp_ms)>>(std::move(ttl_ms)));
}
/*RUSTYCPP:GEN-END id=completion_tracker.2*/

// ===========================================================================
// CompletionTracker
// ===========================================================================

/**
 * @safe - Server-side tracker for completed request XIDs
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
// TTL-based expiry. The mutable config and containers are protected by
// rusty::Mutex and the statistics use relaxed AtomicU64 operations. Config is
// snapshotted before container locks, and every two-container operation locks
// completed_set_ before lru_list_. Individual counters are atomic; hit_rate()
// and reset_stats() intentionally are not multi-counter linearizable snapshots.
// Authored as inline-Rust DSL:
// the whole class (struct + two #[cpp_ctor] factories + all methods) is the
// source of truth; the transpiler regenerates the matching GEN block. The
// config / entry / status / query-result types are already DSL (above).
//
// Two ctors via #[cpp_ctor]: `new()` (uses CompletionTrackerConfig::defaults()
// — the test's `CompletionTracker tracker_;` default-constructs) and
// `with_config(config)`. The LRU storage is a rusty::VecDeque<CompletedEntry>
// (front = most recent; push_front / pop_back); mid-list removal in
// is_completed / remove / evict_expired uses the VecDeque port's
// `remove(index)`. All bodies are pure rusty (Mutex guards, relaxed atomics,
// HashSet + VecDeque ops, index loops) — no raw pointers, syscalls, iterators,
// or operator-overload chains.
#if RUSTYCPP_RUST
#[repr(C)]
pub struct CompletionTracker {
    pub config_: Mutex<CompletionTrackerConfig>,
    pub lru_list_: Mutex<VecDeque<CompletedEntry>>,
    pub completed_set_: Mutex<HashSet<i64>>,
    pub total_tracked_: AtomicU64,
    pub queries_: AtomicU64,
    pub query_hits_: AtomicU64,
    pub evictions_: AtomicU64,
}

impl CompletionTracker {
    #[cfg_attr(any(), cpp_ctor)]
    pub fn new() -> CompletionTracker {
        CompletionTracker {
            config_: Mutex::new(CompletionTrackerConfig::defaults()),
            lru_list_: Mutex::<VecDeque<CompletedEntry>>::new(VecDeque::<CompletedEntry>::new()),
            completed_set_: Mutex::<HashSet<i64>>::new(HashSet::<i64>::new()),
            total_tracked_: AtomicU64::new(0u64),
            queries_: AtomicU64::new(0u64),
            query_hits_: AtomicU64::new(0u64),
            evictions_: AtomicU64::new(0u64),
        }
    }

    #[cfg_attr(any(), cpp_ctor)]
    pub fn with_config(config: self::CompletionTrackerConfig) -> CompletionTracker {
        CompletionTracker {
            config_: Mutex::new(config),
            lru_list_: Mutex::<VecDeque<CompletedEntry>>::new(VecDeque::<CompletedEntry>::new()),
            completed_set_: Mutex::<HashSet<i64>>::new(HashSet::<i64>::new()),
            total_tracked_: AtomicU64::new(0u64),
            queries_: AtomicU64::new(0u64),
            query_hits_: AtomicU64::new(0u64),
            evictions_: AtomicU64::new(0u64),
        }
    }

    pub fn enabled(&self) -> bool {
        let guard = self.config_.lock().unwrap();
        guard.enabled
    }

    pub fn config(&self) -> CompletionTrackerConfig {
        let guard = self.config_.lock().unwrap();
        *guard
    }

    pub fn set_config(&mut self, config: self::CompletionTrackerConfig) {
        let mut guard = self.config_.lock().unwrap();
        *guard = config;
    }

    #[allow(clippy::len_zero)]
    pub fn mark_completed(&mut self, xid: i64, current_time_ms: u64) {
        let cfg = self.config();
        if !cfg.enabled {
            return;
        }
        let mut set_guard = self.completed_set_.lock().unwrap();
        if set_guard.contains(&xid) {
            return;
        }
        let mut list_guard = self.lru_list_.lock().unwrap();
        while list_guard.len() >= cfg.max_entries && list_guard.len() > 0usize {
            // Loop condition guarantees non-empty; VecDeque::back()
            // returns Option<&T> in real Rust.
            let oldest_xid: i64 = list_guard.back().unwrap().xid;
            set_guard.remove(&oldest_xid);
            list_guard.pop_back();
            self.evictions_.fetch_add(1u64, Ordering::Relaxed);
        }
        list_guard.push_front(CompletedEntry::new(xid, current_time_ms));
        set_guard.insert(xid);
        self.total_tracked_.fetch_add(1u64, Ordering::Relaxed);
    }

    pub fn is_completed(&mut self, xid: i64, current_time_ms: u64) -> bool {
        let cfg = self.config();
        self.queries_.fetch_add(1u64, Ordering::Relaxed);
        if !cfg.enabled {
            return false;
        }
        let mut set_guard = self.completed_set_.lock().unwrap();
        if !set_guard.contains(&xid) {
            return false;
        }
        let mut list_guard = self.lru_list_.lock().unwrap();
        let mut i: usize = 0;
        while i < list_guard.len() {
            if list_guard[i].xid == xid {
                if list_guard[i].is_expired(current_time_ms, cfg.ttl_ms) {
                    set_guard.remove(&xid);
                    list_guard.remove(i);
                    return false;
                }
                self.query_hits_.fetch_add(1u64, Ordering::Relaxed);
                return true;
            }
            i += 1usize;
        }
        false
    }

    pub fn remove(&mut self, xid: i64) -> bool {
        let mut set_guard = self.completed_set_.lock().unwrap();
        if !set_guard.contains(&xid) {
            return false;
        }
        set_guard.remove(&xid);
        let mut list_guard = self.lru_list_.lock().unwrap();
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

    pub fn clear(&mut self) {
        let mut set_guard = self.completed_set_.lock().unwrap();
        let mut list_guard = self.lru_list_.lock().unwrap();
        set_guard.clear();
        list_guard.clear();
    }

    pub fn size(&self) -> usize {
        let guard = self.completed_set_.lock().unwrap();
        guard.len()
    }

    pub fn total_tracked(&self) -> u64 {
        self.total_tracked_.load(Ordering::Relaxed)
    }

    pub fn queries(&self) -> u64 {
        self.queries_.load(Ordering::Relaxed)
    }

    pub fn query_hits(&self) -> u64 {
        self.query_hits_.load(Ordering::Relaxed)
    }

    pub fn hit_rate(&self) -> f64 {
        let q: u64 = self.queries_.load(Ordering::Relaxed);
        if q == 0u64 {
            return 0.0f64;
        }
        (self.query_hits_.load(Ordering::Relaxed) as f64) / (q as f64)
    }

    pub fn evictions(&self) -> u64 {
        self.evictions_.load(Ordering::Relaxed)
    }

    pub fn reset_stats(&mut self) {
        self.total_tracked_.store(0u64, Ordering::Relaxed);
        self.queries_.store(0u64, Ordering::Relaxed);
        self.query_hits_.store(0u64, Ordering::Relaxed);
        self.evictions_.store(0u64, Ordering::Relaxed);
    }

    pub fn evict_expired(&mut self, current_time_ms: u64) -> usize {
        let cfg = self.config();
        if !cfg.enabled || cfg.ttl_ms == 0u64 {
            return 0usize;
        }
        let mut set_guard = self.completed_set_.lock().unwrap();
        let mut list_guard = self.lru_list_.lock().unwrap();
        let mut evicted: usize = 0;
        let mut i: usize = 0;
        while i < list_guard.len() {
            if list_guard[i].is_expired(current_time_ms, cfg.ttl_ms) {
                let xid: i64 = list_guard[i].xid;
                set_guard.remove(&xid);
                list_guard.remove(i);
                evicted += 1usize;
            } else {
                i += 1usize;
            }
        }
        self.evictions_.fetch_add(evicted as u64, Ordering::Relaxed);
        evicted
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=completion_tracker.tracker version=1 rust_sha256=f65ca56a038809bf28b765f068450c0154d7335b73ead3244957864132ec0b13*/
struct CompletionTracker;

struct CompletionTracker {
    rusty::Mutex<CompletionTrackerConfig> config_;
    rusty::Mutex<rusty::VecDeque<CompletedEntry>> lru_list_;
    rusty::Mutex<rusty::HashSet<int64_t>> completed_set_;
    rusty::sync::atomic::AtomicU64 total_tracked_;
    rusty::sync::atomic::AtomicU64 queries_;
    rusty::sync::atomic::AtomicU64 query_hits_;
    rusty::sync::atomic::AtomicU64 evictions_;

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
    : config_(rusty::Mutex<CompletionTrackerConfig>::new_(CompletionTrackerConfig::defaults()))
    , lru_list_(rusty::Mutex<rusty::VecDeque<CompletedEntry>>::new_(rusty::VecDeque<CompletedEntry>::new_()))
    , completed_set_(rusty::Mutex<rusty::HashSet<int64_t>>::new_(rusty::HashSet<int64_t>::new_()))
    , total_tracked_(AtomicU64::new_(static_cast<uint64_t>(0)))
    , queries_(AtomicU64::new_(static_cast<uint64_t>(0)))
    , query_hits_(AtomicU64::new_(static_cast<uint64_t>(0)))
    , evictions_(AtomicU64::new_(static_cast<uint64_t>(0)))
{}

CompletionTracker::CompletionTracker(CompletionTrackerConfig config)
    : config_(rusty::Mutex<CompletionTrackerConfig>::new_(std::move(config)))
    , lru_list_(rusty::Mutex<rusty::VecDeque<CompletedEntry>>::new_(rusty::VecDeque<CompletedEntry>::new_()))
    , completed_set_(rusty::Mutex<rusty::HashSet<int64_t>>::new_(rusty::HashSet<int64_t>::new_()))
    , total_tracked_(AtomicU64::new_(static_cast<uint64_t>(0)))
    , queries_(AtomicU64::new_(static_cast<uint64_t>(0)))
    , query_hits_(AtomicU64::new_(static_cast<uint64_t>(0)))
    , evictions_(AtomicU64::new_(static_cast<uint64_t>(0)))
{}

bool CompletionTracker::enabled() const {
    auto guard = this->config_.lock().unwrap();
    return std::move((*guard).enabled);
}

CompletionTrackerConfig CompletionTracker::config() const {
    auto guard = this->config_.lock().unwrap();
    return std::move(*guard);
}

void CompletionTracker::set_config(CompletionTrackerConfig config) {
    auto guard = this->config_.lock().unwrap();
    *guard = std::move(config);
}

void CompletionTracker::mark_completed(int64_t xid, uint64_t current_time_ms) {
    const auto cfg = this->config();
    if (rusty::detail::rust_not(cfg.enabled)) {
        return;
    }
    auto set_guard = this->completed_set_.lock().unwrap();
    if (rusty::contains(set_guard, &xid)) {
        return;
    }
    auto list_guard = this->lru_list_.lock().unwrap();
    while ((rusty::len(list_guard) >= rusty::detail::deref_if_pointer_like(cfg.max_entries)) && (rusty::len(list_guard) > static_cast<size_t>(0))) {
        int64_t oldest_xid = (*list_guard).back().unwrap().xid;
        (*set_guard).remove(oldest_xid);
        (*list_guard).pop_back();
        this->evictions_.fetch_add(static_cast<uint64_t>(1), Ordering::Relaxed);
    }
    (*list_guard).push_front(CompletedEntry::new_(std::move(xid), std::move(current_time_ms)));
    (*set_guard).insert(std::move(xid));
    this->total_tracked_.fetch_add(static_cast<uint64_t>(1), Ordering::Relaxed);
}

bool CompletionTracker::is_completed(int64_t xid, uint64_t current_time_ms) {
    const auto cfg = this->config();
    this->queries_.fetch_add(static_cast<uint64_t>(1), Ordering::Relaxed);
    if (rusty::detail::rust_not(cfg.enabled)) {
        return false;
    }
    auto set_guard = this->completed_set_.lock().unwrap();
    if (rusty::detail::rust_not(rusty::contains(set_guard, &xid))) {
        return false;
    }
    auto list_guard = this->lru_list_.lock().unwrap();
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::len(list_guard)) {
        if (rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.xid); }) { return (__r.xid); } else if constexpr (requires { (__r.xid_field); }) { return (__r.xid_field); } else if constexpr (requires { ((*__r).xid); }) { return ((*__r).xid); } else { return ((*__r).xid_field); } }(list_guard[i])) == rusty::detail::deref_if_pointer_like(xid)) {
            if (list_guard[i].is_expired(std::move(current_time_ms), std::move(cfg.ttl_ms))) {
                (*set_guard).remove(xid);
                (*list_guard).remove(std::move(i));
                return false;
            }
            this->query_hits_.fetch_add(static_cast<uint64_t>(1), Ordering::Relaxed);
            return true;
        }
        i += static_cast<size_t>(1);
    }
    return false;
}

bool CompletionTracker::remove(int64_t xid) {
    auto set_guard = this->completed_set_.lock().unwrap();
    if (rusty::detail::rust_not(rusty::contains(set_guard, &xid))) {
        return false;
    }
    (*set_guard).remove(xid);
    auto list_guard = this->lru_list_.lock().unwrap();
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::len(list_guard)) {
        if (rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.xid); }) { return (__r.xid); } else if constexpr (requires { (__r.xid_field); }) { return (__r.xid_field); } else if constexpr (requires { ((*__r).xid); }) { return ((*__r).xid); } else { return ((*__r).xid_field); } }(list_guard[i])) == rusty::detail::deref_if_pointer_like(xid)) {
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
    return this->total_tracked_.load(Ordering::Relaxed);
}

uint64_t CompletionTracker::queries() const {
    return this->queries_.load(Ordering::Relaxed);
}

uint64_t CompletionTracker::query_hits() const {
    return this->query_hits_.load(Ordering::Relaxed);
}

double CompletionTracker::hit_rate() const {
    const uint64_t q = this->queries_.load(Ordering::Relaxed);
    if (rusty::detail::deref_if_pointer_like(q) == static_cast<uint64_t>(0)) {
        return 0.0;
    }
    return ((static_cast<double>(this->query_hits_.load(Ordering::Relaxed)))) / ((static_cast<double>(q)));
}

uint64_t CompletionTracker::evictions() const {
    return this->evictions_.load(Ordering::Relaxed);
}

void CompletionTracker::reset_stats() {
    this->total_tracked_.store(static_cast<uint64_t>(0), Ordering::Relaxed);
    this->queries_.store(static_cast<uint64_t>(0), Ordering::Relaxed);
    this->query_hits_.store(static_cast<uint64_t>(0), Ordering::Relaxed);
    this->evictions_.store(static_cast<uint64_t>(0), Ordering::Relaxed);
}

size_t CompletionTracker::evict_expired(uint64_t current_time_ms) {
    const auto cfg = this->config();
    if (rusty::detail::rust_not(cfg.enabled) || (rusty::detail::deref_if_pointer_like(cfg.ttl_ms) == static_cast<uint64_t>(0))) {
        return static_cast<size_t>(0);
    }
    auto set_guard = this->completed_set_.lock().unwrap();
    auto list_guard = this->lru_list_.lock().unwrap();
    size_t evicted = static_cast<size_t>(0);
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::len(list_guard)) {
        if (list_guard[i].is_expired(std::move(current_time_ms), std::move(cfg.ttl_ms))) {
            int64_t xid = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.xid); }) { return (__r.xid); } else if constexpr (requires { (__r.xid_field); }) { return (__r.xid_field); } else if constexpr (requires { ((*__r).xid); }) { return ((*__r).xid); } else { return ((*__r).xid_field); } }(list_guard[i]);
            (*set_guard).remove(xid);
            (*list_guard).remove(std::move(i));
            evicted += static_cast<size_t>(1);
        } else {
            i += static_cast<size_t>(1);
        }
    }
    this->evictions_.fetch_add(static_cast<uint64_t>(evicted), Ordering::Relaxed);
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
#[allow(non_camel_case_types)]
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum CompletionStatus {
    NOT_FOUND = 0,
    COMPLETED = 1,
    COMPLETED_WITH_ERROR = 2,
    EXPIRED = 3,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=completion_tracker.status version=1 rust_sha256=a48fefff92903ed338b3c0842d2a4a63514fb845f76077202420b596527a6995*/
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
#[repr(C)]
pub struct CompletionQueryResult {
    pub status: CompletionStatus,
    pub error_code: i32,
    pub has_cached_response: bool,
}

impl CompletionQueryResult {
    pub fn new() -> CompletionQueryResult {
        CompletionQueryResult {
            status: CompletionStatus::NOT_FOUND,
            error_code: 0i32,
            has_cached_response: false,
        }
    }

    pub fn not_found() -> CompletionQueryResult {
        CompletionQueryResult::new()
    }

    pub fn completed(err_code: i32, has_response: bool) -> CompletionQueryResult {
        let s: CompletionStatus = if err_code == 0i32 {
            CompletionStatus::COMPLETED
        } else {
            CompletionStatus::COMPLETED_WITH_ERROR
        };
        CompletionQueryResult { status: s, error_code: err_code, has_cached_response: has_response }
    }

    pub fn expired() -> CompletionQueryResult {
        CompletionQueryResult {
            status: CompletionStatus::EXPIRED,
            error_code: 0i32,
            has_cached_response: false,
        }
    }

    pub fn is_completed(&self) -> bool {
        self.status == CompletionStatus::COMPLETED || self.status == CompletionStatus::COMPLETED_WITH_ERROR
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=completion_tracker.3 version=1 rust_sha256=fbfdcdd3c0d60edae5db0b70989deacca5bd8424fa0629ce36bc9b8fb258dc43*/
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
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};


CompletionQueryResult CompletionQueryResult::new_() {
    return CompletionQueryResult{.status = rusty::clone(rusty::clone(CompletionStatus_NOT_FOUND())), .error_code = static_cast<int32_t>(0), .has_cached_response = false};
}

CompletionQueryResult CompletionQueryResult::not_found() {
    return CompletionQueryResult::new_();
}

CompletionQueryResult CompletionQueryResult::completed(int32_t err_code, bool has_response) {
    CompletionStatus s = (rusty::detail::deref_if_pointer_like(err_code) == static_cast<int32_t>(0) ? rusty::clone(CompletionStatus_COMPLETED()) : rusty::clone(CompletionStatus_COMPLETED_WITH_ERROR()));
    return CompletionQueryResult{.status = std::move(s), .error_code = std::move(err_code), .has_cached_response = std::move(has_response)};
}

CompletionQueryResult CompletionQueryResult::expired() {
    return CompletionQueryResult{.status = rusty::clone(rusty::clone(CompletionStatus_EXPIRED())), .error_code = static_cast<int32_t>(0), .has_cached_response = false};
}

bool CompletionQueryResult::is_completed() const {
    return (rusty::detail::deref_if_pointer_like(this->status) == rusty::clone(CompletionStatus_COMPLETED())) || (rusty::detail::deref_if_pointer_like(this->status) == rusty::clone(CompletionStatus_COMPLETED_WITH_ERROR()));
}
/*RUSTYCPP:GEN-END id=completion_tracker.3*/

// @safe - Convert status to string for logging
// Returns &'static str (-> std::string_view), not const char*: the DSL
// cannot spell a literal as a raw pointer. Callers use EXPECT_EQ, not
// EXPECT_STREQ. The varargs-UB objection to this is expired -- Log_* is a
// std::format variadic template now, not C varargs. See playbook 7.26.
#if RUSTYCPP_RUST
#[allow(unreachable_patterns)]
pub fn completion_status_to_string(status: CompletionStatus) -> &'static str {
    match status {
        CompletionStatus::NOT_FOUND => "NOT_FOUND",
        CompletionStatus::COMPLETED => "COMPLETED",
        CompletionStatus::COMPLETED_WITH_ERROR => "COMPLETED_WITH_ERROR",
        CompletionStatus::EXPIRED => "EXPIRED",
        _ => "UNKNOWN",
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=completion_tracker.6 version=1 rust_sha256=3888f61daa438b0021425e7f0554606f4f7b9e0a24e1dd6a4b7b09aff8275456*/
std::string_view completion_status_to_string(CompletionStatus status) {
    return ({ auto&& _m = status; std::optional<std::string_view> _match_value; bool _m_matched = false; if (!_m_matched && (_m == CompletionStatus::NOT_FOUND)) { _match_value.emplace(std::move(std::string_view("NOT_FOUND"))); _m_matched = true; } if (!_m_matched && (_m == CompletionStatus::COMPLETED)) { _match_value.emplace(std::move(std::string_view("COMPLETED"))); _m_matched = true; } if (!_m_matched && (_m == CompletionStatus::COMPLETED_WITH_ERROR)) { _match_value.emplace(std::move(std::string_view("COMPLETED_WITH_ERROR"))); _m_matched = true; } if (!_m_matched && (_m == CompletionStatus::EXPIRED)) { _match_value.emplace(std::move(std::string_view("EXPIRED"))); _m_matched = true; } if (!_m_matched) { _match_value.emplace(std::move(std::string_view("UNKNOWN"))); _m_matched = true; } if (!_m_matched) { rusty::intrinsics::unreachable_panic(); } std::move(_match_value).value(); });
}
/*RUSTYCPP:GEN-END id=completion_tracker.6*/


}  // export namespace rrr
