#[path = "../src/rpc/completion_tracker.rs"]
mod completion_tracker;

use completion_tracker::{
    completion_status_to_string, CompletedEntry, CompletionQueryResult, CompletionStatus,
    CompletionTracker, CompletionTrackerConfig,
};

fn assert_send<T: Send>() {}

#[test]
fn presets_and_public_value_types_match_the_legacy_contract() {
    assert_send::<CompletionTracker>();
    assert_eq!(std::mem::size_of::<CompletionStatus>(), 1);
    assert_eq!(std::mem::align_of::<CompletionStatus>(), 1);

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
    assert_eq!(disabled.ttl_ms, 60_000);
    assert_eq!(disabled.max_entries, 100_000);
    assert!(!disabled.enabled);
}

#[test]
fn expiration_is_strict_at_the_boundary_and_uses_unsigned_wrapping() {
    let entry = CompletedEntry::new(7, 1_000);
    assert!(!entry.is_expired(1_100, 100));
    assert!(entry.is_expired(1_101, 100));
    assert!(!entry.is_expired(u64::MAX, 0));

    let wrapped = CompletedEntry::new(11, u64::MAX - 4);
    assert!(wrapped.is_expired(6, 10));
}

#[test]
fn duplicate_marks_do_not_refresh_or_reorder_an_entry() {
    let mut config = CompletionTrackerConfig::defaults();
    config.ttl_ms = 10;
    let mut tracker = CompletionTracker::with_config(config);

    tracker.mark_completed(10, 100);
    tracker.mark_completed(10, 1_000);
    assert_eq!(tracker.size(), 1);
    assert_eq!(tracker.total_tracked(), 1);
    assert!(!tracker.is_completed(10, 111));
    assert_eq!(tracker.queries(), 1);
    assert_eq!(tracker.query_hits(), 0);
}

#[test]
fn capacity_evicts_oldest_first_and_preserves_remaining_order() {
    let mut config = CompletionTrackerConfig::defaults();
    config.max_entries = 3;
    let mut tracker = CompletionTracker::with_config(config);

    tracker.mark_completed(1, 100);
    tracker.mark_completed(2, 101);
    tracker.mark_completed(3, 102);
    tracker.mark_completed(4, 103);

    assert_eq!(tracker.size(), 3);
    assert_eq!(tracker.evictions(), 1);
    assert!(!tracker.is_completed(1, 103));
    assert!(tracker.is_completed(2, 103));
    assert!(tracker.is_completed(3, 103));
    assert!(tracker.is_completed(4, 103));
}

#[test]
fn zero_capacity_retains_the_legacy_one_entry_edge_case() {
    let mut config = CompletionTrackerConfig::defaults();
    config.max_entries = 0;
    let mut tracker = CompletionTracker::with_config(config);

    tracker.mark_completed(1, 100);
    assert_eq!(tracker.size(), 1);
    tracker.mark_completed(2, 101);
    assert_eq!(tracker.size(), 1);
    assert_eq!(tracker.evictions(), 1);
    assert!(!tracker.is_completed(1, 101));
    assert!(tracker.is_completed(2, 101));
}

#[test]
fn explicit_expiry_removes_all_expired_entries_and_updates_only_evictions() {
    let mut config = CompletionTrackerConfig::defaults();
    config.ttl_ms = 100;
    let mut tracker = CompletionTracker::with_config(config);
    let mut xid = 1;
    while xid <= 5 {
        tracker.mark_completed(xid * 100, 1_000 + (xid as u64) * 50);
        xid += 1;
    }

    assert_eq!(tracker.evict_expired(1_251), 3);
    assert_eq!(tracker.size(), 2);
    assert_eq!(tracker.evictions(), 3);
    assert_eq!(tracker.queries(), 0);
    assert_eq!(tracker.query_hits(), 0);
}

#[test]
fn disabled_config_gates_storage_and_queries_without_clearing_old_data() {
    let mut tracker = CompletionTracker::new();
    tracker.mark_completed(7, 100);
    tracker.set_config(CompletionTrackerConfig::disabled());
    tracker.mark_completed(8, 100);

    assert_eq!(tracker.size(), 1);
    assert!(!tracker.is_completed(7, 100));
    assert_eq!(tracker.queries(), 1);

    tracker.set_config(CompletionTrackerConfig::defaults());
    assert!(tracker.is_completed(7, 100));
}

#[test]
fn remove_clear_and_stats_keep_their_independent_contracts() {
    let mut tracker = CompletionTracker::new();
    assert!(tracker.enabled());
    assert_eq!(tracker.config().ttl_ms, 60_000);
    tracker.mark_completed(1, 100);
    tracker.mark_completed(2, 100);
    assert!(tracker.is_completed(1, 100));
    assert!(!tracker.is_completed(99, 100));
    assert_eq!(tracker.hit_rate(), 0.5);

    assert!(tracker.remove(1));
    assert!(!tracker.remove(1));
    tracker.reset_stats();
    assert_eq!(tracker.total_tracked(), 0);
    assert_eq!(tracker.queries(), 0);
    assert_eq!(tracker.query_hits(), 0);
    assert_eq!(tracker.evictions(), 0);
    assert_eq!(tracker.size(), 1);

    tracker.clear();
    assert_eq!(tracker.size(), 0);
    assert_eq!(tracker.hit_rate(), 0.0);
}

#[test]
fn query_result_factories_and_status_names_are_exact() {
    let not_found = CompletionQueryResult::new();
    assert_eq!(not_found.status as i32, CompletionStatus::NOT_FOUND as i32);
    assert_eq!(not_found.error_code, 0);
    assert!(!not_found.has_cached_response);
    assert!(!not_found.is_completed());

    let alias = CompletionQueryResult::not_found();
    assert_eq!(alias.status as i32, CompletionStatus::NOT_FOUND as i32);

    let success = CompletionQueryResult::completed(0, true);
    assert_eq!(success.status as i32, CompletionStatus::COMPLETED as i32);
    assert!(success.has_cached_response);
    assert!(success.is_completed());

    let failure = CompletionQueryResult::completed(-7, false);
    assert_eq!(
        failure.status as i32,
        CompletionStatus::COMPLETED_WITH_ERROR as i32
    );
    assert_eq!(failure.error_code, -7);
    assert!(failure.is_completed());

    let expired = CompletionQueryResult::expired();
    assert_eq!(expired.status as i32, CompletionStatus::EXPIRED as i32);
    assert!(!expired.is_completed());

    assert_eq!(
        completion_status_to_string(CompletionStatus::NOT_FOUND),
        "NOT_FOUND"
    );
    assert_eq!(
        completion_status_to_string(CompletionStatus::COMPLETED),
        "COMPLETED"
    );
    assert_eq!(
        completion_status_to_string(CompletionStatus::COMPLETED_WITH_ERROR),
        "COMPLETED_WITH_ERROR"
    );
    assert_eq!(
        completion_status_to_string(CompletionStatus::EXPIRED),
        "EXPIRED"
    );
}

#[test]
fn tracker_can_move_between_threads_but_requires_unique_access() {
    let mut tracker = CompletionTracker::new();
    tracker.mark_completed(42, 500);

    let (size, hit) = std::thread::spawn(move || {
        let hit = tracker.is_completed(42, 500);
        (tracker.size(), hit)
    })
    .join()
    .unwrap();

    assert_eq!(size, 1);
    assert!(hit);
}
