use std::mem::{align_of, offset_of, size_of};
use std::sync::atomic::Ordering;
use std::sync::{Arc, Barrier, Mutex};
use std::thread;

use srpc::completion_tracker::{
    completion_status_to_string, CompletedEntry, CompletionQueryResult,
    CompletionStatus, CompletionTracker, CompletionTrackerConfig,
};

fn assert_copy<T: Copy>() {}
fn assert_send_sync<T: Send + Sync>() {}

#[test]
fn public_record_enum_and_trait_contracts_are_pinned() {
    assert_copy::<CompletionTrackerConfig>();
    assert_send_sync::<CompletionTrackerConfig>();
    assert_send_sync::<CompletedEntry>();
    assert_send_sync::<CompletionTracker>();
    assert_send_sync::<CompletionQueryResult>();

    assert_eq!(size_of::<CompletionTrackerConfig>(), 24);
    assert_eq!(align_of::<CompletionTrackerConfig>(), 8);
    assert_eq!(offset_of!(CompletionTrackerConfig, ttl_ms), 0);
    assert_eq!(offset_of!(CompletionTrackerConfig, max_entries), 8);
    assert_eq!(offset_of!(CompletionTrackerConfig, enabled), 16);

    assert_eq!(size_of::<CompletedEntry>(), 16);
    assert_eq!(align_of::<CompletedEntry>(), 8);
    assert_eq!(offset_of!(CompletedEntry, xid), 0);
    assert_eq!(offset_of!(CompletedEntry, timestamp_ms), 8);

    assert_eq!(size_of::<CompletionStatus>(), size_of::<i32>());
    assert_eq!(align_of::<CompletionStatus>(), align_of::<i32>());
    assert_eq!(CompletionStatus::NOT_FOUND as i32, 0);
    assert_eq!(CompletionStatus::COMPLETED as i32, 1);
    assert_eq!(CompletionStatus::COMPLETED_WITH_ERROR as i32, 2);
    assert_eq!(CompletionStatus::EXPIRED as i32, 3);

    assert_eq!(size_of::<CompletionQueryResult>(), 12);
    assert_eq!(align_of::<CompletionQueryResult>(), 4);
    assert_eq!(offset_of!(CompletionQueryResult, status), 0);
    assert_eq!(offset_of!(CompletionQueryResult, error_code), 4);
    assert_eq!(offset_of!(CompletionQueryResult, has_cached_response), 8);
}

#[test]
fn configuration_entry_and_query_contracts_match_production() {
    let defaults = CompletionTrackerConfig::defaults();
    assert_eq!(defaults.ttl_ms, 60_000);
    assert_eq!(defaults.max_entries, 100_000);
    assert!(defaults.enabled);

    let small = CompletionTrackerConfig::small();
    assert_eq!(small.ttl_ms, 30_000);
    assert_eq!(small.max_entries, 10_000);
    assert!(small.enabled);

    let large = CompletionTrackerConfig::large();
    assert_eq!(large.ttl_ms, 300_000);
    assert_eq!(large.max_entries, 1_000_000);
    assert!(large.enabled);

    let disabled = CompletionTrackerConfig::disabled();
    assert!(!disabled.enabled);

    let entry = CompletedEntry::new(17, 100);
    assert_eq!(entry.xid, 17);
    assert_eq!(entry.timestamp_ms, 100);
    assert!(!entry.is_expired(110, 10));
    assert!(entry.is_expired(111, 10));
    assert!(!entry.is_expired(u64::MAX, 0));

    let wrapped = CompletedEntry::new(9, u64::MAX - 4);
    assert!(wrapped.is_expired(6, 10));

    let not_found = CompletionQueryResult::not_found();
    let success = CompletionQueryResult::completed(0, true);
    let failure = CompletionQueryResult::completed(-7, false);
    let expired = CompletionQueryResult::expired();
    assert_eq!(not_found.status as i32, CompletionStatus::NOT_FOUND as i32);
    assert_eq!(success.status as i32, CompletionStatus::COMPLETED as i32);
    assert_eq!(failure.status as i32, CompletionStatus::COMPLETED_WITH_ERROR as i32);
    assert_eq!(expired.status as i32, CompletionStatus::EXPIRED as i32);
    assert!(!not_found.is_completed());
    assert!(success.is_completed());
    assert!(failure.is_completed());
    assert!(!expired.is_completed());
    assert_eq!(completion_status_to_string(CompletionStatus::NOT_FOUND), "NOT_FOUND");
    assert_eq!(completion_status_to_string(CompletionStatus::COMPLETED), "COMPLETED");
    assert_eq!(
        completion_status_to_string(CompletionStatus::COMPLETED_WITH_ERROR),
        "COMPLETED_WITH_ERROR"
    );
    assert_eq!(completion_status_to_string(CompletionStatus::EXPIRED), "EXPIRED");
}

#[test]
fn tracking_expiry_eviction_and_configuration_preserve_behavior() {
    let mut tracker = CompletionTracker::with_config(CompletionTrackerConfig {
        ttl_ms: 10,
        max_entries: 2,
        enabled: true,
    });
    assert!(tracker.enabled());
    assert_eq!(tracker.config().max_entries, 2);

    tracker.mark_completed(10, 100);
    tracker.mark_completed(10, 1_000);
    assert_eq!(tracker.size(), 1);
    assert_eq!(tracker.total_tracked(), 1);
    assert!(tracker.is_completed(10, 110));
    assert!(!tracker.is_completed(10, 111));
    assert_eq!(tracker.size(), 0);

    tracker.mark_completed(20, 200);
    tracker.mark_completed(30, 300);
    tracker.mark_completed(40, 400);
    assert_eq!(tracker.size(), 2);
    assert_eq!(tracker.total_tracked(), 4);
    assert_eq!(tracker.evictions(), 1);
    assert!(!tracker.is_completed(20, 400));
    assert!(tracker.is_completed(30, 300));
    assert!(tracker.remove(30));
    assert!(!tracker.remove(30));

    tracker.mark_completed(50, 500);
    assert_eq!(tracker.evict_expired(511), 2);
    assert_eq!(tracker.size(), 0);
    assert_eq!(tracker.evictions(), 3);

    tracker.set_config(CompletionTrackerConfig::disabled());
    assert!(!tracker.enabled());
    tracker.mark_completed(60, 600);
    assert_eq!(tracker.size(), 0);
    assert!(!tracker.is_completed(60, 600));

    tracker.set_config(CompletionTrackerConfig::defaults());
    tracker.mark_completed(70, 700);
    tracker.clear();
    assert_eq!(tracker.size(), 0);
}

#[test]
fn relaxed_counters_have_unsigned_wrapping_and_reset_contracts() {
    let mut tracker = CompletionTracker::with_config(CompletionTrackerConfig {
        ttl_ms: 0,
        max_entries: 1,
        enabled: true,
    });

    tracker.total_tracked_.store(u64::MAX, Ordering::Relaxed);
    tracker.mark_completed(1, 100);
    assert_eq!(tracker.total_tracked(), 0);

    tracker.queries_.store(u64::MAX, Ordering::Relaxed);
    assert!(!tracker.is_completed(-1, 100));
    assert_eq!(tracker.queries(), 0);

    tracker.query_hits_.store(u64::MAX, Ordering::Relaxed);
    assert!(tracker.is_completed(1, 100));
    assert_eq!(tracker.query_hits(), 0);

    tracker.evictions_.store(u64::MAX, Ordering::Relaxed);
    tracker.mark_completed(2, 100);
    assert_eq!(tracker.evictions(), 0);

    tracker.reset_stats();
    assert_eq!(tracker.total_tracked(), 0);
    assert_eq!(tracker.queries(), 0);
    assert_eq!(tracker.query_hits(), 0);
    assert_eq!(tracker.evictions(), 0);
    assert_eq!(tracker.hit_rate(), 0.0);
}

#[test]
fn safe_rust_callers_can_repeat_the_mutating_api_across_threads() {
    const THREADS: usize = 8;
    const OPS_PER_THREAD: usize = 500;
    const ROUNDS: usize = 3;

    for _ in 0..ROUNDS {
        // The public mutating methods retain `&mut self` so rusty-cpp preserves
        // the existing non-const C++ ABI. Safe Rust therefore serializes that
        // API at the receiver; the C++ three-lane gate separately calls the
        // internally synchronized methods directly from all worker threads.
        let tracker = Arc::new(Mutex::new(CompletionTracker::new()));
        let start = Arc::new(Barrier::new(THREADS));
        let mut workers = Vec::with_capacity(THREADS);

        for thread_index in 0..THREADS {
            let tracker = Arc::clone(&tracker);
            let start = Arc::clone(&start);
            workers.push(thread::spawn(move || {
                start.wait();
                for operation in 0..OPS_PER_THREAD {
                    let xid = (thread_index * OPS_PER_THREAD + operation) as i64;
                    let mut tracker = tracker.lock().expect("tracker lock poisoned");
                    tracker.mark_completed(xid, 1_000);
                    assert!(tracker.is_completed(xid, 1_000));
                }
            }));
        }

        for worker in workers {
            worker.join().expect("tracker worker panicked");
        }

        let tracker = tracker.lock().expect("tracker lock poisoned");
        let expected = (THREADS * OPS_PER_THREAD) as u64;
        assert_eq!(tracker.size() as u64, expected);
        assert_eq!(tracker.total_tracked(), expected);
        assert_eq!(tracker.queries(), expected);
        assert_eq!(tracker.query_hits(), expected);
        assert_eq!(tracker.evictions(), 0);
        assert_eq!(tracker.hit_rate(), 1.0);
    }
}
