#![cfg(all(have_mako, feature = "test-support"))]

use std::env;
use std::sync::atomic::{AtomicU32, AtomicUsize, Ordering};
use std::sync::Arc;

use mako_cache::{Cache, CacheOptions};
use mako_local::{TestCommitPhase, WorkerHealth};
use mrx_core::fakes::MemBlobs;

const REQUIRE_NATIVE_HOOKS_ENV: &str = "MAKO_CACHE_REQUIRE_NATIVE_CRASH_HOOKS";

static OBSERVED_TIMESTAMP: AtomicU32 = AtomicU32::new(0);
static TIMESTAMP_CALLBACKS: AtomicUsize = AtomicUsize::new(0);

fn observe_timestamp(phase: TestCommitPhase, mako_timestamp: u32) {
    if phase == TestCommitPhase::MakoTimestampAllocated {
        OBSERVED_TIMESTAMP.store(mako_timestamp, Ordering::SeqCst);
        TIMESTAMP_CALLBACKS.fetch_add(1, Ordering::SeqCst);
    }
}

#[test]
fn native_timestamp_matches_the_persisted_record_and_applied_frontier() {
    let features = mako_local::features().expect("read native capabilities");
    if !features.test_commit_observer() {
        assert!(
            env::var_os(REQUIRE_NATIVE_HOOKS_ENV).is_none(),
            "{REQUIRE_NATIVE_HOOKS_ENV} requires a hook-enabled native archive"
        );
        return;
    }
    assert_eq!(
        mako_local::worker_health().expect("read initial worker health"),
        WorkerHealth::NotAttached
    );

    OBSERVED_TIMESTAMP.store(0, Ordering::SeqCst);
    TIMESTAMP_CALLBACKS.store(0, Ordering::SeqCst);
    let backend = Arc::new(MemBlobs::new());
    let cache = Cache::from_backend(Arc::clone(&backend), CacheOptions::default())
        .expect("open timestamp cache");
    mako_local::install_test_commit_observer(observe_timestamp)
        .expect("install timestamp observer");

    cache
        .put(b"timestamp/exact", b"carried-verbatim")
        .expect("commit observed transaction");
    mako_local::clear_test_commit_observer().expect("clear timestamp observer");
    assert_eq!(TIMESTAMP_CALLBACKS.load(Ordering::SeqCst), 1);
    let native_timestamp = OBSERVED_TIMESTAMP.load(Ordering::SeqCst);
    assert_ne!(native_timestamp, 0);

    assert_eq!(cache.wait_applied().expect("apply observed transaction"), 1);
    assert_eq!(
        mako_cache::test_support::decoded_log_timestamps(&backend),
        vec![(1, native_timestamp)],
        "the record must carry the exact timestamp allocated at the native serialization point"
    );
    assert_eq!(
        cache
            .applied_watermark()
            .mako_timestamp()
            .expect("applied timestamp")
            .get(),
        native_timestamp,
        "the applied frontier must name the exact persisted record timestamp"
    );
    cache.close().expect("close timestamp cache");
}
