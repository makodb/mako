#![cfg(have_mako)]

//! End-to-end cache coverage for native cleanup uncertainty.
//!
//! Every scenario owns a fresh OS worker because cleanup failure permanently
//! quarantines that worker. The native hooks are absent from production
//! builds, so the ordinary profile records a deliberate runtime skip. The
//! dedicated hook profile sets `MAKO_CACHE_REQUIRE_NATIVE_CRASH_HOOKS`, turning
//! a missing hook-capable archive into a hard failure.

use std::sync::Arc;

use mako_cache::{ApplyError, Cache, CacheOptions, Error};
use mako_local::{
    arm_test_cleanup_failure, features, quarantined_worker_count, worker_health,
    Error as LocalError, TestCleanupBoundary, WorkerHealth,
};
use mrx_core::fakes::MemBlobs;

const REQUIRE_NATIVE_HOOKS_ENV: &str = "MAKO_CACHE_REQUIRE_NATIVE_CRASH_HOOKS";
const CLEANUP_SCENARIO_ENV: &str = "MAKO_CACHE_CLEANUP_SCENARIO";

type TestCache = Cache<Arc<MemBlobs>>;

fn open(backend: &Arc<MemBlobs>, options: CacheOptions) -> TestCache {
    Cache::from_backend(Arc::clone(backend), options).expect("open cleanup-test cache")
}

fn fresh_quarantined_worker(name: &str, scenario: impl FnOnce() + Send + 'static) {
    let worker_name = format!("mako-cache-cleanup-{name}");
    std::thread::Builder::new()
        .name(worker_name)
        .spawn(move || {
            assert_eq!(
                worker_health().expect("query fresh worker health"),
                WorkerHealth::NotAttached,
                "the scenario must start on a genuinely fresh OS worker"
            );
            let quarantined_before =
                quarantined_worker_count().expect("read initial quarantine count");

            scenario();

            assert_eq!(
                worker_health().expect("query poisoned worker health"),
                WorkerHealth::Poisoned
            );
            assert_eq!(
                quarantined_worker_count().expect("read final quarantine count"),
                quarantined_before + 1,
                "one uncertain cleanup must quarantine exactly one worker"
            );
        })
        .expect("spawn fresh cleanup-test worker")
        .join()
        .unwrap_or_else(|panic| std::panic::resume_unwind(panic));
}

fn assert_nothing_published(cache: &TestCache, backend: &MemBlobs) {
    assert_eq!(cache.queued_transactions(), 0);
    assert_eq!(cache.highest_acknowledged_sequence(), 0);
    assert_eq!(cache.applied_sequence(), 0);
    assert_eq!(cache.flush().expect("empty application barrier"), 0);
    assert_eq!(backend.batch_count(), 0);
    assert!(backend.snapshot().is_empty());
    match cache.transaction() {
        Err(Error::Native(LocalError::WorkerPoisoned)) => {}
        Ok(_) => panic!("a quarantined worker admitted another cache transaction"),
        Err(other) => panic!("unexpected post-quarantine admission result: {other}"),
    }
}

fn preparation_error_drop_quarantines_without_a_slot() {
    let backend = Arc::new(MemBlobs::new());
    let mut options = CacheOptions::default();
    options.writeback.max_record_bytes = 1;
    let cache = open(&backend, options);

    let mut transaction = cache.transaction().expect("begin oversized transaction");
    assert!(transaction
        .put(b"cleanup/preparation", b"never-installed")
        .expect("stage oversized transaction"));
    arm_test_cleanup_failure(TestCleanupBoundary::Abort)
        .expect("arm preparation-error drop cleanup failure");

    let error = transaction
        .commit()
        .expect_err("record preparation must reject the transaction");
    assert!(matches!(error, Error::Native(LocalError::WorkerPoisoned)));

    // Native preflight rejects the oversized record and immediately aborts the
    // now-sealed transaction. The injected abort-cleanup failure is more severe
    // than the size error and therefore reaches the public result directly.
    assert_nothing_published(&cache, &backend);
    assert_eq!(cache.close().expect("close empty write-back queue"), 0);
}

fn explicit_abort_quarantines_without_a_slot() {
    let backend = Arc::new(MemBlobs::new());
    let cache = open(&backend, CacheOptions::default());

    let mut transaction = cache
        .transaction()
        .expect("begin explicitly aborted transaction");
    assert!(transaction
        .put(b"cleanup/explicit-abort", b"never-installed")
        .expect("stage explicitly aborted write"));
    arm_test_cleanup_failure(TestCleanupBoundary::Abort)
        .expect("arm explicit-abort cleanup failure");

    assert!(matches!(
        transaction.abort(),
        Err(Error::Native(LocalError::WorkerPoisoned))
    ));
    assert_nothing_published(&cache, &backend);
    assert_eq!(cache.close().expect("close empty write-back queue"), 0);
}

fn active_transaction_drop_quarantines_without_a_slot() {
    let backend = Arc::new(MemBlobs::new());
    let cache = open(&backend, CacheOptions::default());

    let mut transaction = cache.transaction().expect("begin dropped transaction");
    assert!(transaction
        .put(b"cleanup/drop", b"never-installed")
        .expect("stage dropped write"));
    arm_test_cleanup_failure(TestCleanupBoundary::Abort)
        .expect("arm drop-driven abort cleanup failure");
    drop(transaction);

    // Cache::Transaction has no fallible Drop result to inspect; the worker
    // health and process-wide counter checked by fresh_quarantined_worker are
    // the observable proof that native Drop did not silently report success.
    assert_nothing_published(&cache, &backend);
    assert_eq!(cache.close().expect("close empty write-back queue"), 0);
}

fn ambiguous_commit_pins_the_complete_record() {
    let backend = Arc::new(MemBlobs::new());
    let cache = open(&backend, CacheOptions::default());

    let mut transaction = cache.transaction().expect("begin ambiguous commit");
    assert!(transaction
        .put(b"cleanup/ambiguous", b"possibly-installed")
        .expect("stage ambiguous write"));
    arm_test_cleanup_failure(TestCleanupBoundary::Commit).expect("arm commit cleanup failure");

    let error = transaction
        .commit()
        .expect_err("uncertain native commit must not be acknowledged");
    let sequence = match error {
        Error::UnknownCommitOutcome {
            sequence,
            source: LocalError::WorkerPoisoned,
            cleanup: Some(LocalError::WorkerPoisoned),
        } => sequence,
        other => panic!("unexpected ambiguous-commit result: {other}"),
    };
    assert_eq!(sequence.get(), 1);

    assert_eq!(cache.queued_transactions(), 1);
    assert_eq!(cache.highest_acknowledged_sequence(), 0);
    assert_eq!(cache.applied_sequence(), 0);
    assert_eq!(
        cache
            .flush()
            .expect("an unacknowledged pin is outside the flush snapshot"),
        0
    );
    assert_eq!(backend.batch_count(), 0);
    assert!(backend.snapshot().is_empty());

    match cache.transaction() {
        Err(Error::Apply(ApplyError::UnknownOutcome { sequence: pinned })) => {
            assert_eq!(pinned, sequence)
        }
        Ok(_) => panic!("a pinned unknown outcome admitted a later transaction"),
        Err(other) => panic!("unexpected post-pin admission result: {other}"),
    }
    assert!(matches!(cache.close(), Err(Error::Runtime(_))));
    assert_eq!(backend.batch_count(), 0);
    assert!(backend.snapshot().is_empty());
}

fn run_isolated_scenario(name: &str) {
    let status = std::process::Command::new(std::env::current_exe().unwrap())
        .arg("--exact")
        .arg("cleanup_uncertainty_quarantines_fresh_cache_workers")
        .env(CLEANUP_SCENARIO_ENV, name)
        .status()
        .expect("run cleanup-quarantine subprocess");
    assert!(status.success(), "cleanup scenario {name} failed: {status}");
}

#[test]
fn cleanup_uncertainty_quarantines_fresh_cache_workers() {
    let cleanup_hooks = features()
        .expect("read native cleanup-hook capability")
        .test_cleanup_failures();
    let hooks_required = std::env::var_os(REQUIRE_NATIVE_HOOKS_ENV).is_some();
    assert!(
        cleanup_hooks || !hooks_required,
        "{REQUIRE_NATIVE_HOOKS_ENV} requires a native build configured with MAKO_LOCAL_TEST_HOOKS=ON"
    );
    if !cleanup_hooks {
        eprintln!("native cleanup seams are disabled; hook-only cache cleanup cases skipped");
        return;
    }

    if let Some(scenario) = std::env::var_os(CLEANUP_SCENARIO_ENV) {
        match scenario.to_str().expect("cleanup scenario must be UTF-8") {
            "preparation-error-drop" => fresh_quarantined_worker(
                "preparation-error-drop",
                preparation_error_drop_quarantines_without_a_slot,
            ),
            "explicit-abort" => fresh_quarantined_worker(
                "explicit-abort",
                explicit_abort_quarantines_without_a_slot,
            ),
            "active-transaction-drop" => fresh_quarantined_worker(
                "active-transaction-drop",
                active_transaction_drop_quarantines_without_a_slot,
            ),
            "ambiguous-commit" => fresh_quarantined_worker(
                "ambiguous-commit",
                ambiguous_commit_pins_the_complete_record,
            ),
            other => panic!("unknown cleanup scenario: {other}"),
        }
        return;
    }

    for scenario in [
        "preparation-error-drop",
        "explicit-abort",
        "active-transaction-drop",
        "ambiguous-commit",
    ] {
        run_isolated_scenario(scenario);
    }
}
