//! Completed-request tracking for reconnection and idempotency decisions.
//!
//! Names, field order, presets, and receiver mutability intentionally match
//! the legacy `rrr.completion_tracker` C++ module. This file is also intended
//! to be the source from which that module is generated.

use std::cell::Cell;
use std::collections::{HashSet, VecDeque};
use std::sync::Mutex;

pub struct CompletionTrackerConfig {
    pub ttl_ms: u64,
    pub max_entries: usize,
    pub enabled: bool,
}

impl Copy for CompletionTrackerConfig {}

impl Clone for CompletionTrackerConfig {
    fn clone(&self) -> CompletionTrackerConfig {
        CompletionTrackerConfig {
            ttl_ms: self.ttl_ms,
            max_entries: self.max_entries,
            enabled: self.enabled,
        }
    }
}

impl CompletionTrackerConfig {
    pub fn new() -> CompletionTrackerConfig {
        CompletionTrackerConfig {
            ttl_ms: 60_000,
            max_entries: 100_000,
            enabled: true,
        }
    }

    pub fn defaults() -> CompletionTrackerConfig {
        CompletionTrackerConfig::new()
    }

    pub fn small() -> CompletionTrackerConfig {
        CompletionTrackerConfig {
            ttl_ms: 30_000,
            max_entries: 10_000,
            enabled: true,
        }
    }

    pub fn large() -> CompletionTrackerConfig {
        CompletionTrackerConfig {
            ttl_ms: 300_000,
            max_entries: 1_000_000,
            enabled: true,
        }
    }

    pub fn disabled() -> CompletionTrackerConfig {
        CompletionTrackerConfig {
            ttl_ms: 60_000,
            max_entries: 100_000,
            enabled: false,
        }
    }
}

pub struct CompletedEntry {
    pub xid: i64,
    pub timestamp_ms: u64,
}

impl CompletedEntry {
    pub fn new(x: i64, ts: u64) -> CompletedEntry {
        CompletedEntry {
            xid: x,
            timestamp_ms: ts,
        }
    }

    pub fn is_expired(&self, current_time_ms: u64, ttl_ms: u64) -> bool {
        if ttl_ms == 0 {
            return false;
        }
        current_time_ms > self.timestamp_ms.wrapping_add(ttl_ms)
    }
}

/// Server-side LRU set of completed request XIDs.
///
/// The completed set is always locked before the LRU list. Keeping one lock
/// order for every two-container operation preserves the legacy deadlock
/// avoidance contract.
pub struct CompletionTracker {
    pub config_: Cell<CompletionTrackerConfig>,
    pub lru_list_: Mutex<VecDeque<CompletedEntry>>,
    pub completed_set_: Mutex<HashSet<i64>>,
    pub total_tracked_: Cell<u64>,
    pub queries_: Cell<u64>,
    pub query_hits_: Cell<u64>,
    pub evictions_: Cell<u64>,
}

impl CompletionTracker {
    #[cfg_attr(any(), cpp_ctor)]
    pub fn new() -> CompletionTracker {
        CompletionTracker {
            config_: Cell::new(CompletionTrackerConfig::defaults()),
            lru_list_: Mutex::new(VecDeque::new()),
            completed_set_: Mutex::new(HashSet::new()),
            total_tracked_: Cell::new(0),
            queries_: Cell::new(0),
            query_hits_: Cell::new(0),
            evictions_: Cell::new(0),
        }
    }

    #[cfg_attr(any(), cpp_ctor)]
    pub fn with_config(config: self::CompletionTrackerConfig) -> CompletionTracker {
        CompletionTracker {
            config_: Cell::new(config),
            lru_list_: Mutex::new(VecDeque::new()),
            completed_set_: Mutex::new(HashSet::new()),
            total_tracked_: Cell::new(0),
            queries_: Cell::new(0),
            query_hits_: Cell::new(0),
            evictions_: Cell::new(0),
        }
    }

    pub fn enabled(&self) -> bool {
        self.config_.get().enabled
    }

    pub fn config(&self) -> CompletionTrackerConfig {
        self.config_.get()
    }

    pub fn set_config(&mut self, config: self::CompletionTrackerConfig) {
        self.config_.set(config);
    }

    pub fn mark_completed(&mut self, xid: i64, current_time_ms: u64) {
        let config = self.config_.get();
        if !config.enabled {
            return;
        }

        let mut completed = self.completed_set_.lock().unwrap();
        if completed.contains(&xid) {
            return;
        }

        let mut lru = self.lru_list_.lock().unwrap();
        while lru.len() >= config.max_entries && !lru.is_empty() {
            let oldest_xid = lru.back().unwrap().xid;
            completed.remove(&oldest_xid);
            lru.pop_back();
            self.evictions_.set(self.evictions_.get().wrapping_add(1));
        }

        lru.push_front(CompletedEntry::new(xid, current_time_ms));
        completed.insert(xid);
        self.total_tracked_
            .set(self.total_tracked_.get().wrapping_add(1));
    }

    pub fn is_completed(&mut self, xid: i64, current_time_ms: u64) -> bool {
        let config = self.config_.get();
        self.queries_.set(self.queries_.get().wrapping_add(1));
        if !config.enabled {
            return false;
        }

        let mut completed = self.completed_set_.lock().unwrap();
        if !completed.contains(&xid) {
            return false;
        }

        let mut lru = self.lru_list_.lock().unwrap();
        let mut index: usize = 0;
        while index < lru.len() {
            if lru[index].xid == xid {
                if lru[index].is_expired(current_time_ms, config.ttl_ms) {
                    completed.remove(&xid);
                    lru.remove(index);
                    return false;
                }
                self.query_hits_.set(self.query_hits_.get().wrapping_add(1));
                return true;
            }
            index += 1;
        }
        false
    }

    pub fn remove(&mut self, xid: i64) -> bool {
        let mut completed = self.completed_set_.lock().unwrap();
        if !completed.contains(&xid) {
            return false;
        }

        completed.remove(&xid);
        let mut lru = self.lru_list_.lock().unwrap();
        let mut index: usize = 0;
        while index < lru.len() {
            if lru[index].xid == xid {
                lru.remove(index);
                return true;
            }
            index += 1;
        }
        true
    }

    pub fn clear(&mut self) {
        let mut completed = self.completed_set_.lock().unwrap();
        let mut lru = self.lru_list_.lock().unwrap();
        completed.clear();
        lru.clear();
    }

    pub fn size(&self) -> usize {
        self.completed_set_.lock().unwrap().len()
    }

    pub fn total_tracked(&self) -> u64 {
        self.total_tracked_.get()
    }

    pub fn queries(&self) -> u64 {
        self.queries_.get()
    }

    pub fn query_hits(&self) -> u64 {
        self.query_hits_.get()
    }

    pub fn hit_rate(&self) -> f64 {
        let queries = self.queries_.get();
        if queries == 0 {
            return 0.0;
        }
        (self.query_hits_.get() as f64) / (queries as f64)
    }

    pub fn evictions(&self) -> u64 {
        self.evictions_.get()
    }

    pub fn reset_stats(&mut self) {
        self.total_tracked_.set(0);
        self.queries_.set(0);
        self.query_hits_.set(0);
        self.evictions_.set(0);
    }

    pub fn evict_expired(&mut self, current_time_ms: u64) -> usize {
        let config = self.config_.get();
        if !config.enabled || config.ttl_ms == 0 {
            return 0;
        }

        let mut completed = self.completed_set_.lock().unwrap();
        let mut lru = self.lru_list_.lock().unwrap();
        let mut evicted: usize = 0;
        let mut index: usize = 0;
        while index < lru.len() {
            if lru[index].is_expired(current_time_ms, config.ttl_ms) {
                let xid = lru[index].xid;
                completed.remove(&xid);
                lru.remove(index);
                evicted += 1;
            } else {
                index += 1;
            }
        }
        self.evictions_
            .set(self.evictions_.get().wrapping_add(evicted as u64));
        evicted
    }
}

#[allow(non_camel_case_types)]
#[derive(Clone, Copy, PartialEq, Eq)]
// Keep the original C++ `uint8_t` representation: this status crosses the
// generated module boundary and its one-byte ABI predates the inline DSL.
#[repr(u8)]
pub enum CompletionStatus {
    NOT_FOUND = 0,
    COMPLETED = 1,
    COMPLETED_WITH_ERROR = 2,
    EXPIRED = 3,
}

pub struct CompletionQueryResult {
    pub status: CompletionStatus,
    pub error_code: i32,
    pub has_cached_response: bool,
}

impl CompletionQueryResult {
    pub fn new() -> CompletionQueryResult {
        CompletionQueryResult {
            status: CompletionStatus::NOT_FOUND,
            error_code: 0,
            has_cached_response: false,
        }
    }

    pub fn not_found() -> CompletionQueryResult {
        CompletionQueryResult::new()
    }

    pub fn completed(err_code: i32, has_response: bool) -> CompletionQueryResult {
        let status = if err_code == 0 {
            CompletionStatus::COMPLETED
        } else {
            CompletionStatus::COMPLETED_WITH_ERROR
        };
        CompletionQueryResult {
            status,
            error_code: err_code,
            has_cached_response: has_response,
        }
    }

    pub fn expired() -> CompletionQueryResult {
        CompletionQueryResult {
            status: CompletionStatus::EXPIRED,
            error_code: 0,
            has_cached_response: false,
        }
    }

    pub fn is_completed(&self) -> bool {
        self.status == CompletionStatus::COMPLETED
            || self.status == CompletionStatus::COMPLETED_WITH_ERROR
    }
}

#[allow(unreachable_patterns)]
pub fn completion_status_to_string(status: self::CompletionStatus) -> &'static str {
    match status {
        CompletionStatus::NOT_FOUND => "NOT_FOUND",
        CompletionStatus::COMPLETED => "COMPLETED",
        CompletionStatus::COMPLETED_WITH_ERROR => "COMPLETED_WITH_ERROR",
        CompletionStatus::EXPIRED => "EXPIRED",
        _ => "UNKNOWN",
    }
}
