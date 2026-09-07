#![cfg(all(have_mako, feature = "test-support"))]

use std::env;
use std::sync::atomic::{AtomicU32, AtomicU64, AtomicUsize, Ordering};
use std::sync::Arc;

use mako_cache::{Cache, CacheOptions};
use mako_local::{MakoTimestamp, TestCommitPhase, WorkerHealth};
use mrx_core::fakes::MemBlobs;

const REQUIRE_NATIVE_HOOKS_ENV: &str = "MAKO_CACHE_REQUIRE_NATIVE_CRASH_HOOKS";

static OBSERVED_PHYSICAL_US: AtomicU64 = AtomicU64::new(0);
static OBSERVED_LOGICAL: AtomicU32 = AtomicU32::new(0);
static OBSERVED_ORIGIN: AtomicU32 = AtomicU32::new(0);
static TIMESTAMP_CALLBACKS: AtomicUsize = AtomicUsize::new(0);

fn observe_timestamp(phase: TestCommitPhase, mako_timestamp: Option<MakoTimestamp>) {
    if phase == TestCommitPhase::MakoTimestampAllocated {
        let mako_timestamp = mako_timestamp.expect("allocated phase carries a timestamp");
        OBSERVED_PHYSICAL_US.store(mako_timestamp.physical_us(), Ordering::SeqCst);
        OBSERVED_LOGICAL.store(mako_timestamp.logical(), Ordering::SeqCst);
        OBSERVED_ORIGIN.store(mako_timestamp.origin(), Ordering::SeqCst);
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

    OBSERVED_PHYSICAL_US.store(0, Ordering::SeqCst);
    OBSERVED_LOGICAL.store(0, Ordering::SeqCst);
    OBSERVED_ORIGIN.store(0, Ordering::SeqCst);
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
    let native_timestamp = MakoTimestamp::new(
        OBSERVED_PHYSICAL_US.load(Ordering::SeqCst),
        OBSERVED_LOGICAL.load(Ordering::SeqCst),
        OBSERVED_ORIGIN.load(Ordering::SeqCst),
    )
    .expect("native observer returned a timestamp with a nonzero origin");

    let applied = cache.wait_applied();
    let persisted_timestamps = mako_cache::test_support::decoded_log_timestamps(&backend);
    let applied_timestamp = cache.applied_watermark().mako_timestamp();
    let closed = cache.close();

    // Assert only after the cache is closed. Mutation tests deliberately make
    // these values disagree; keeping a live cache here would make its Drop
    // cleanup panic during assertion unwinding and obscure the exact failure.
    assert_eq!(applied.expect("apply observed transaction"), 1);
    assert_eq!(closed.expect("close timestamp cache"), 1);
    // Concurrent cache slot zero owns lane tag one in the physical log ID.
    const FIRST_WORKER_LOG_ID: u64 = (1u64 << 48) | 1;
    assert_eq!(
        persisted_timestamps,
        vec![(FIRST_WORKER_LOG_ID, native_timestamp)],
        "the record must carry the exact timestamp allocated at the native serialization point"
    );
    assert_eq!(
        applied_timestamp.expect("applied timestamp"),
        native_timestamp,
        "the applied frontier must name the exact persisted record timestamp"
    );
}
