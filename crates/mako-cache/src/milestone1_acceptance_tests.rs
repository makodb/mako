//! Integrated acceptance coverage for the bounded single-machine cache.
//!
//! The lower-level write-back suite proves each queue transition separately.
//! These tests retain a real [`Cache`] around the native Silo transaction path
//! and exercise the Milestone 1 overload and shutdown contracts end to end.

use std::env;
use std::process::Command;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{mpsc, Arc, Barrier, Condvar, Mutex};
use std::time::{Duration, Instant};

use mrx_core::fakes::MemBlobs;
use mrx_core::{BlobError, BlobOp, Blobs};

use crate::checkpoint;
use crate::record::{
    classify_backend_key, BackendKey, CommitRecord, CommitSeq, Mutation, PreparedCommitRecord,
    DEFAULT_TABLE_ID,
};
use crate::writeback::{ApplyCoordinator, CoordinatorApplyOutcome};
use crate::{Cache, CacheOptions, Error, LocalError, MakoTimestamp, WritebackConfig};

const WAIT_LIMIT: Duration = Duration::from_secs(5);
const NEAR_EXHAUSTION_ROLE_ENV: &str = "MAKO_CACHE_NEAR_EXHAUSTION_ROLE";
const HOT_PHYSICAL_US_MAX: u64 = ((1u64 << 44) - 1) * 1_000;
const HOT_LOGICAL_MAX: u32 = (1u32 << 19) - 1;

#[derive(Debug, Default)]
struct BackendGate {
    entered: usize,
    released: bool,
}

/// An atomic in-memory backend whose writes stop before taking effect until
/// the controller opens the gate. Reads and recovery enumeration remain live.
#[derive(Debug, Default)]
struct BlockingBlobs {
    inner: MemBlobs,
    gate: Mutex<BackendGate>,
    changed: Condvar,
}

impl BlockingBlobs {
    fn wait_until_entered(&self) -> bool {
        let deadline = Instant::now() + WAIT_LIMIT;
        let mut gate = self.gate.lock().expect("backend gate poisoned");
        while gate.entered == 0 {
            let now = Instant::now();
            if now >= deadline {
                return false;
            }
            let (next, timeout) = self
                .changed
                .wait_timeout(gate, deadline - now)
                .expect("backend gate poisoned while waiting");
            gate = next;
            if timeout.timed_out() && gate.entered == 0 {
                return false;
            }
        }
        true
    }

    fn release(&self) {
        let mut gate = self.gate.lock().expect("backend gate poisoned");
        gate.released = true;
        self.changed.notify_all();
    }

    fn block(&self) {
        let mut gate = self.gate.lock().expect("backend gate poisoned");
        gate.entered = 0;
        gate.released = false;
    }
}

impl Blobs for BlockingBlobs {
    fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
        self.inner.get(key)
    }

    fn write_batch(&self, operations: &[BlobOp<'_>]) -> Result<(), BlobError> {
        // Initial format installation precedes transaction admission, so the
        // transaction-write gate must not prevent constructing an empty cache.
        if operations.iter().all(|operation| {
            matches!(operation,
            BlobOp::Put { key, .. } if *key == checkpoint::FORMAT_KEY)
        }) {
            return self.inner.write_batch(operations);
        }
        let mut gate = self.gate.lock().expect("backend gate poisoned");
        gate.entered = gate
            .entered
            .checked_add(1)
            .expect("backend attempt count overflow");
        self.changed.notify_all();
        while !gate.released {
            gate = self
                .changed
                .wait(gate)
                .expect("backend gate poisoned while blocked");
        }
        drop(gate);
        self.inner.write_batch(operations)
    }

    fn for_each_key(&self, callback: &mut dyn FnMut(&[u8])) -> Result<(), BlobError> {
        self.inner.for_each_key(callback)
    }

    fn for_each_entry(
        &self,
        callback: &mut dyn FnMut(&[u8], &[u8]) -> Result<(), BlobError>,
    ) -> Result<(), BlobError> {
        self.inner.for_each_entry(callback)
    }
}

/// Ensures an assertion cannot strand the cache's background writer in the
/// deterministic backend gate.
struct ReleaseOnDrop(Arc<BlockingBlobs>);

impl Drop for ReleaseOnDrop {
    fn drop(&mut self) {
        self.0.release();
    }
}

fn options(capacity: usize) -> CacheOptions {
    CacheOptions {
        writeback: WritebackConfig {
            capacity,
            // These acceptance cases assert the number of backend attempts
            // per logical transaction. Prefix batching has dedicated coverage
            // in writeback.rs, so keep that accounting deterministic here.
            max_batch_records: 1,
            max_apply_retries: 2,
            retry_delay: Duration::from_millis(10),
            ..WritebackConfig::default()
        },
        ..CacheOptions::default()
    }
}

fn wait_until(mut predicate: impl FnMut() -> bool) -> bool {
    let deadline = Instant::now() + WAIT_LIMIT;
    while !predicate() {
        if Instant::now() >= deadline {
            return false;
        }
        std::thread::yield_now();
    }
    true
}

#[test]
fn bounded_writeback_backpressures_sustained_concurrent_writers_then_recovers() {
    const CAPACITY: usize = 2;
    const WORKERS: usize = 8;
    const TOTAL_LANE_CAPACITY: usize = WORKERS * CAPACITY;
    const COMMITS_PER_WORKER: usize = 16;
    const TOTAL_COMMITS: usize = WORKERS * COMMITS_PER_WORKER;

    let backend = Arc::new(BlockingBlobs::default());
    let _release_on_unwind = ReleaseOnDrop(Arc::clone(&backend));
    let cache = Arc::new(
        Cache::from_backend(Arc::clone(&backend), options(CAPACITY))
            .expect("open overload acceptance cache"),
    );

    // MassTrans may legitimately conflict while concurrently growing the
    // shared tree, which is unrelated to this test's writeback contract.
    // Create every disjoint record sequentially, drain that prefix, and then
    // close the backend gate before measuring sustained updates.
    backend.release();
    for worker in 0..WORKERS {
        let mut seed = cache.transaction().expect("begin overload seed");
        for transaction in 0..COMMITS_PER_WORKER {
            let key = format!("milestone1/overload/{worker:02}/{transaction:02}");
            assert!(seed
                .put(key.as_bytes(), b"seed")
                .expect("stage overload seed"));
        }
        seed.commit().expect("commit overload seed");
    }
    let base_sequence = cache.wait_applied().expect("drain overload seed");
    let base_batches = backend.inner.batch_count();
    assert_eq!(base_sequence, WORKERS as u64);
    assert_eq!(
        base_batches,
        WORKERS as u64 + 1,
        "initial format batch plus seed transactions"
    );
    backend.block();

    let first_commit_ready = Arc::new(Barrier::new(WORKERS + 1));
    let completed = Arc::new(AtomicUsize::new(0));
    let maximum_observed_queue = Arc::new(AtomicUsize::new(0));

    let mut workers = Vec::with_capacity(WORKERS);
    for worker in 0..WORKERS {
        let cache = Arc::clone(&cache);
        let first_commit_ready = Arc::clone(&first_commit_ready);
        let completed = Arc::clone(&completed);
        let maximum_observed_queue = Arc::clone(&maximum_observed_queue);
        workers.push(std::thread::spawn(move || {
            for transaction in 0..COMMITS_PER_WORKER {
                let key = format!("milestone1/overload/{worker:02}/{transaction:02}");
                let value = format!("value-{worker:02}-{transaction:02}");
                let mut cache_transaction = cache.transaction().expect("begin overload commit");
                assert!(!cache_transaction
                    .put(key.as_bytes(), value.as_bytes())
                    .expect("stage existing disjoint overload write"));
                if transaction == 0 {
                    first_commit_ready.wait();
                }
                cache_transaction
                    .commit()
                    .expect("commit disjoint overload write");
                completed.fetch_add(1, Ordering::SeqCst);
                maximum_observed_queue.fetch_max(cache.queued_transactions(), Ordering::SeqCst);
            }
        }));
    }

    // Every worker has staged its first disjoint transaction before any of
    // them enters commit. Closing the backend gate then makes queue saturation
    // deterministic rather than scheduler- or RocksDB-speed-dependent.
    first_commit_ready.wait();
    let backend_was_blocked = backend.wait_until_entered();
    let queue_saturated = wait_until(|| completed.load(Ordering::SeqCst) >= TOTAL_LANE_CAPACITY);
    let completed_while_blocked = completed.load(Ordering::SeqCst);
    let acknowledged_while_blocked = cache.highest_acknowledged_sequence();
    let applied_while_blocked = cache.applied_sequence();
    let queued_while_blocked = cache.queued_transactions();

    // Release before any assertion or join, so a failing observation cannot
    // strand producers behind the full bounded queue.
    backend.release();
    for worker in workers {
        worker.join().expect("overload worker panicked");
    }

    assert!(
        backend_was_blocked,
        "write-back never reached the closed backend"
    );
    assert!(
        queue_saturated,
        "bounded queue never reached configured capacity"
    );
    assert_eq!(
        completed_while_blocked, TOTAL_LANE_CAPACITY,
        "workers did not stop at their combined per-lane capacity"
    );
    assert_eq!(
        acknowledged_while_blocked,
        base_sequence + TOTAL_LANE_CAPACITY as u64
    );
    assert_eq!(applied_while_blocked, base_sequence);
    assert_eq!(queued_while_blocked, TOTAL_LANE_CAPACITY);
    assert!(
        maximum_observed_queue.load(Ordering::SeqCst) <= TOTAL_LANE_CAPACITY,
        "observed queue occupancy exceeded the active lanes' combined bound"
    );
    assert_eq!(completed.load(Ordering::SeqCst), TOTAL_COMMITS);
    assert_eq!(
        cache.wait_applied().expect("apply sustained overload"),
        base_sequence + TOTAL_COMMITS as u64
    );
    assert_eq!(
        cache.applied_sequence(),
        base_sequence + TOTAL_COMMITS as u64
    );
    assert_eq!(
        backend.inner.batch_count(),
        base_batches + TOTAL_COMMITS as u64
    );

    let cache = Arc::try_unwrap(cache).unwrap_or_else(|_| panic!("worker retained cache handle"));
    assert_eq!(
        cache.close().expect("close overload acceptance cache"),
        base_sequence + TOTAL_COMMITS as u64
    );
}

#[test]
fn clean_cache_close_drains_every_acknowledged_transaction() {
    const ACCEPTED: usize = 6;

    let backend = Arc::new(BlockingBlobs::default());
    let _release_on_unwind = ReleaseOnDrop(Arc::clone(&backend));
    let cache = Cache::from_backend(Arc::clone(&backend), options(ACCEPTED))
        .expect("open clean-shutdown acceptance cache");

    cache
        .put(b"milestone1/close/00", b"value-00")
        .expect("commit first shutdown write");
    assert!(
        backend.wait_until_entered(),
        "write-back did not stop at the closed backend"
    );
    for index in 1..ACCEPTED {
        let key = format!("milestone1/close/{index:02}");
        let value = format!("value-{index:02}");
        cache
            .put(key.as_bytes(), value.as_bytes())
            .expect("commit shutdown backlog write");
    }

    assert_eq!(cache.highest_acknowledged_sequence(), ACCEPTED as u64);
    assert_eq!(cache.applied_sequence(), 0);
    assert_eq!(cache.queued_transactions(), ACCEPTED);

    let (invoked_tx, invoked_rx) = mpsc::channel();
    let (result_tx, result_rx) = mpsc::channel();
    let closer = std::thread::spawn(move || {
        invoked_tx.send(()).expect("signal close invocation");
        result_tx
            .send(cache.close())
            .expect("return close acceptance result");
    });
    invoked_rx
        .recv_timeout(WAIT_LIMIT)
        .expect("close worker did not start");
    let premature = result_rx.recv_timeout(Duration::from_millis(50)).ok();
    let returned_prematurely = premature.is_some();

    backend.release();
    let close_result = match premature {
        Some(result) => result,
        None => result_rx
            .recv_timeout(WAIT_LIMIT)
            .expect("clean close did not finish after backend release"),
    };
    closer.join().expect("clean-close worker panicked");

    assert!(
        !returned_prematurely,
        "clean close returned before its acknowledged backlog could be applied"
    );
    assert_eq!(
        close_result.expect("clean close must drain accepted transactions"),
        ACCEPTED as u64
    );
    assert_eq!(backend.inner.batch_count(), ACCEPTED as u64 + 1);

    let reopened = Cache::from_backend(Arc::clone(&backend), CacheOptions::default())
        .expect("reopen cleanly drained cache");
    assert_eq!(reopened.applied_sequence(), ACCEPTED as u64);
    for index in 0..ACCEPTED {
        let key = format!("milestone1/close/{index:02}");
        let value = format!("value-{index:02}");
        assert_eq!(
            reopened
                .get(key.as_bytes())
                .expect("read cleanly drained value")
                .as_deref(),
            Some(value.as_bytes())
        );
    }
    assert_eq!(
        reopened.close().expect("close reopened shutdown cache"),
        ACCEPTED as u64
    );
}

#[test]
fn forced_cache_stop_preserves_applied_prefix_and_discards_only_unapplied_tail() {
    let backend = Arc::new(MemBlobs::new());
    let mut cache_options = options(4);
    cache_options.writeback.retry_delay = Duration::from_secs(1);
    let cache = Cache::from_backend(Arc::clone(&backend), cache_options)
        .expect("open forced-stop acceptance cache");

    cache
        .put(b"milestone1/forced/prefix", b"applied")
        .expect("commit applied prefix");
    assert_eq!(cache.wait_applied().expect("apply forced-stop prefix"), 1);
    assert_eq!(backend.batch_count(), 2);

    // Every later backend attempt fails atomically. Both native transactions
    // are nevertheless visible and acknowledged, so abort_without_flush must
    // model loss of exactly this volatile, unapplied suffix.
    backend.fail_next_writes(usize::MAX);
    cache
        .put(b"milestone1/forced/tail-a", b"volatile-a")
        .expect("acknowledge first volatile tail transaction");
    cache
        .put(b"milestone1/forced/tail-b", b"volatile-b")
        .expect("acknowledge second volatile tail transaction");
    assert_eq!(cache.highest_acknowledged_sequence(), 3);
    assert_eq!(cache.applied_sequence(), 1);

    cache
        .abort_without_flush()
        .expect("forced cache stop must not drain the volatile tail");
    assert_eq!(
        backend.batch_count(),
        2,
        "forced stop applied a transaction from the failing volatile tail"
    );

    backend.fail_next_writes(0);
    let reopened = Cache::from_backend(Arc::clone(&backend), CacheOptions::default())
        .expect("reopen backend after forced cache stop");
    assert_eq!(reopened.applied_sequence(), 1);
    assert_eq!(
        reopened
            .get(b"milestone1/forced/prefix")
            .expect("read recovered applied prefix")
            .as_deref(),
        Some(&b"applied"[..])
    );
    assert_eq!(
        reopened
            .get(b"milestone1/forced/tail-a")
            .expect("read first discarded tail key"),
        None
    );
    assert_eq!(
        reopened
            .get(b"milestone1/forced/tail-b")
            .expect("read second discarded tail key"),
        None
    );
    assert_eq!(
        reopened.close().expect("close forced-stop recovery cache"),
        1
    );
}

fn near_exhaustion_child_role() {
    let maximum = MakoTimestamp::new(HOT_PHYSICAL_US_MAX, HOT_LOGICAL_MAX, 1)
        .expect("maximum hot timestamp has a nonzero origin");
    let maximum_minus_one = MakoTimestamp::new(HOT_PHYSICAL_US_MAX, HOT_LOGICAL_MAX - 1, 1)
        .expect("MAX-1 hot timestamp has a nonzero origin");
    let backend = Arc::new(MemBlobs::new());
    let recovered = PreparedCommitRecord::prepare(
        vec![Mutation::Put {
            table_id: DEFAULT_TABLE_ID,
            key: b"milestone1/exhaustion/recovered".to_vec(),
            value: b"max-minus-one".to_vec(),
        }],
        WritebackConfig::default().max_record_bytes,
    )
    .expect("prepare near-exhaustion recovery record")
    .bind(
        CommitSeq::new(1).expect("nonzero recovery sequence"),
        maximum_minus_one,
    )
    .finalize();
    seed_checkpoint(&*backend, &[recovered]);

    let cache = Cache::from_backend(Arc::clone(&backend), CacheOptions::default())
        .expect("reopen cache at MAX-1");
    assert_eq!(cache.applied_sequence(), 1);
    assert_eq!(
        cache
            .get(b"milestone1/exhaustion/recovered")
            .expect("read recovered MAX-1 value")
            .as_deref(),
        Some(&b"max-minus-one"[..])
    );

    cache
        .put(b"milestone1/exhaustion/final", b"maximum")
        .expect("MAX-1 recovery must leave MAX mintable exactly once");
    assert_eq!(cache.wait_applied().expect("apply MAX transaction"), 2);
    assert_eq!(cache.highest_acknowledged_sequence(), 2);

    let final_timestamp = backend
        .snapshot()
        .into_iter()
        .find_map(
            |(key, encoded)| match crate::record::classify_backend_key(&key) {
                BackendKey::Log(_) => {
                    let record = crate::record::CommitRecord::decode(
                        &key,
                        &encoded,
                        WritebackConfig::default().max_record_bytes,
                    )
                    .expect("decode MAX transaction record");
                    record
                        .mutations()
                        .iter()
                        .any(|mutation| mutation.key() == b"milestone1/exhaustion/final")
                        .then(|| record.mako_timestamp())
                }
                BackendKey::Data { .. }
                | BackendKey::Format
                | BackendKey::Lane(_)
                | BackendKey::Foreign => None,
            },
        )
        .expect("find MAX transaction record");
    assert_eq!(final_timestamp, maximum);

    let error = cache
        .put(b"milestone1/exhaustion/rejected", b"must-not-install")
        .expect_err("the transaction after MAX must fail timestamp allocation");
    assert!(matches!(
        error,
        Error::Native(LocalError::TimestampExhausted)
    ));
    assert_eq!(
        cache.highest_acknowledged_sequence(),
        2,
        "timestamp exhaustion consumed a cache sequence"
    );
    assert_eq!(
        cache
            .get(b"milestone1/exhaustion/rejected")
            .expect("read rejected post-MAX key"),
        None,
        "timestamp-exhausted transaction became visible"
    );
    assert_eq!(backend.batch_count(), 3);
    assert_eq!(cache.close().expect("close exhausted cache"), 2);
}

#[test]
fn recovery_near_timestamp_exhaustion_mints_maximum_once_then_fails_closed() {
    if env::var_os(NEAR_EXHAUSTION_ROLE_ENV).is_some() {
        near_exhaustion_child_role();
        return;
    }

    let output = Command::new(env::current_exe().expect("locate cache unit-test executable"))
        .arg("--exact")
        .arg(
            "milestone1_acceptance_tests::recovery_near_timestamp_exhaustion_mints_maximum_once_then_fails_closed",
        )
        .arg("--test-threads=1")
        .arg("--nocapture")
        .env(NEAR_EXHAUSTION_ROLE_ENV, "1")
        .output()
        .expect("spawn near-exhaustion cache child");
    assert!(
        output.status.success(),
        "near-exhaustion cache child failed:\nstdout:\n{}\nstderr:\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}

fn seed_checkpoint(backend: &MemBlobs, records: &[CommitRecord]) {
    let format = checkpoint::encode_format().expect("encode fixture format");
    backend
        .write_batch(&[BlobOp::Put {
            key: checkpoint::FORMAT_KEY,
            val: &format,
        }])
        .expect("install fixture format");
    let coordinator = ApplyCoordinator::empty();
    for record in records {
        assert!(matches!(
            coordinator
                .apply(backend, std::slice::from_ref(record))
                .unwrap(),
            CoordinatorApplyOutcome::Applied
        ));
    }
}

fn retained_log_count(backend: &MemBlobs) -> usize {
    backend
        .snapshot()
        .keys()
        .filter(|key| matches!(classify_backend_key(key), BackendKey::Log(_)))
        .count()
}

fn fixture_record(sequence: u64, physical_us: u64, mutations: Vec<Mutation>) -> CommitRecord {
    PreparedCommitRecord::prepare(mutations, WritebackConfig::default().max_record_bytes)
        .unwrap()
        .bind(
            CommitSeq::new(sequence).unwrap(),
            MakoTimestamp::new(physical_us, 0, 1).unwrap(),
        )
        .finalize()
}

fn fixture_put(key: &[u8], value: &[u8]) -> Mutation {
    Mutation::Put {
        table_id: DEFAULT_TABLE_ID,
        key: key.to_vec(),
        value: value.to_vec(),
    }
}

#[test]
fn gc_reclaims_all_history_then_recovers_current_values_tombstones_and_counters() {
    let backend = Arc::new(MemBlobs::new());
    let cache = Cache::from_backend(Arc::clone(&backend), options(8)).unwrap();
    cache.put(b"gc/live", b"old").unwrap();
    cache.put(b"gc/live", b"current").unwrap();
    cache.put(b"gc/deleted", b"gone").unwrap();
    assert!(cache.delete(b"gc/deleted").unwrap());
    assert_eq!(cache.wait_applied().unwrap(), 4);
    assert_eq!(retained_log_count(&backend), 4);
    let before = cache.applied_watermark();
    assert_eq!(cache.writeback.collect_expired_at(u64::MAX).unwrap(), 4);
    assert_eq!(retained_log_count(&backend), 0);
    let gc = cache.status().unwrap().log_gc;
    assert_eq!(gc.retained_records, 0);
    assert_eq!(gc.retained_bytes, 0);
    assert_eq!(gc.reclaimed_records, 4);
    assert_eq!(cache.close().unwrap(), 4);

    let reopened = Cache::from_backend(Arc::clone(&backend), options(8)).unwrap();
    assert_eq!(reopened.applied_watermark(), before);
    assert_eq!(reopened.highest_acknowledged_sequence(), 4);
    assert_eq!(
        reopened.get(b"gc/live").unwrap().as_deref(),
        Some(b"current".as_slice())
    );
    assert_eq!(reopened.get(b"gc/deleted").unwrap(), None);
    assert_eq!(reopened.status().unwrap().log_gc.reclaimed_records, 4);
    reopened.put(b"gc/after-reopen", b"new").unwrap();
    assert_eq!(reopened.wait_applied().unwrap(), 5);
    assert!(reopened.applied_watermark().mako_timestamp() > before.mako_timestamp());
    assert_eq!(retained_log_count(&backend), 1);
    assert_eq!(reopened.close().unwrap(), 5);
}

#[test]
fn gc_default_retention_uses_strict_five_minute_hlc_age() {
    let backend = Arc::new(MemBlobs::new());
    let cache = Cache::from_backend(Arc::clone(&backend), options(8)).unwrap();
    cache.put(b"retention/boundary", b"retained").unwrap();
    cache.wait_applied().unwrap();
    let physical_us = cache
        .applied_watermark()
        .mako_timestamp()
        .unwrap()
        .physical_us();
    let boundary = physical_us.checked_add(300_000_000).unwrap();
    assert_eq!(cache.writeback.collect_expired_at(boundary - 1).unwrap(), 0);
    assert_eq!(cache.writeback.collect_expired_at(boundary).unwrap(), 0);
    assert_eq!(retained_log_count(&backend), 1);
    assert_eq!(cache.writeback.collect_expired_at(boundary + 1).unwrap(), 1);
    assert_eq!(cache.writeback.collect_expired_at(0).unwrap(), 0);
    assert_eq!(retained_log_count(&backend), 0);
    assert_eq!(
        cache.get(b"retention/boundary").unwrap().as_deref(),
        Some(b"retained".as_slice())
    );
    cache.close().unwrap();
    let reopened = Cache::from_backend(backend, options(8)).unwrap();
    assert_eq!(reopened.applied_sequence(), 1);
    assert_eq!(
        reopened.get(b"retention/boundary").unwrap().as_deref(),
        Some(b"retained".as_slice())
    );
    reopened.close().unwrap();
}

#[test]
fn idle_background_writer_collects_expired_logs_without_new_transactions() {
    let backend = Arc::new(MemBlobs::new());
    // Other tests can advance the process HLC far into the future. An old
    // recovered lane makes this real-timer test independent of that floor and
    // verifies GC discovers lanes without an initialized foreground queue.
    seed_checkpoint(
        &backend,
        &[fixture_record(
            1,
            1_000,
            vec![fixture_put(b"idle-gc/key", b"value")],
        )],
    );
    let mut cache_options = options(8);
    cache_options.log_retention = Duration::ZERO;
    cache_options.gc_interval = Duration::from_millis(1);
    let cache = Cache::from_backend(Arc::clone(&backend), cache_options).unwrap();
    assert!(
        wait_until(|| cache.status().unwrap().log_gc.reclaimed_records == 1),
        "idle writer never scheduled GC"
    );
    assert_eq!(retained_log_count(&backend), 0);
    assert_eq!(
        cache.get(b"idle-gc/key").unwrap().as_deref(),
        Some(b"value".as_slice())
    );
    assert_eq!(cache.close().unwrap(), 1);
    let reopened = Cache::from_backend(backend, options(8)).unwrap();
    assert_eq!(reopened.applied_sequence(), 1);
    assert_eq!(
        reopened.get(b"idle-gc/key").unwrap().as_deref(),
        Some(b"value".as_slice())
    );
    reopened.close().unwrap();
}

#[test]
fn gc_recovery_rejects_tagged_lane_maximum_with_wrong_source_position() {
    let raw_base = 1_u64 << crate::record::LOG_LANE_SHIFT;
    let metadata = checkpoint::LaneMetadata {
        applied: 2,
        reclaimed: 2,
        max_timestamp: Some(MakoTimestamp::new(3_000, 0, 1).unwrap()),
        retained_bytes: 0,
    };
    let key_record = fixture_record(
        raw_base | 2,
        3_000,
        vec![fixture_put(b"lane-maximum/key", b"value")],
    );
    for (local, physical_us) in [(2, 2_000), (1, 3_000)] {
        // No retained log or other row can detect this contradiction. In a
        // strictly increasing tagged lane, A owns H and no earlier position can.
        let encoded = checkpoint::encode_row(
            MakoTimestamp::new(physical_us, 0, 1).unwrap(),
            CommitSeq::new(raw_base | local).unwrap(),
            Some(b"value"),
        )
        .unwrap();
        let backend = Arc::new(MemBlobs::seeded([
            (
                checkpoint::FORMAT_KEY.to_vec(),
                checkpoint::encode_format().unwrap(),
            ),
            (checkpoint::lane_key(1), metadata.encode().unwrap()),
            (key_record.data_keys()[0].clone(), encoded),
        ]));
        assert!(
            matches!(
                Cache::from_backend(backend, options(8)),
                Err(Error::BackendStateMismatch)
            ),
            "tagged source {local} with physical_us={physical_us} contradicted its lane maximum"
        );
    }
}

#[test]
fn gc_and_checkpoint_reopen_support_both_native_checksum_policies() {
    for record_checksum in [crate::RecordChecksum::Crc32c, crate::RecordChecksum::None] {
        let backend = Arc::new(MemBlobs::new());
        let config = CacheOptions {
            record_checksum,
            ..options(8)
        };
        let cache = Cache::from_backend(Arc::clone(&backend), config).unwrap();
        cache.put(b"checksum/live", b"value").unwrap();
        cache.put(b"checksum/deleted", b"old").unwrap();
        let mut transaction = cache.transaction().unwrap();
        assert!(transaction.remove(b"checksum/deleted").unwrap());
        transaction.commit().unwrap();
        assert_eq!(cache.wait_applied().unwrap(), 3);
        assert_eq!(cache.writeback.collect_expired_at(u64::MAX).unwrap(), 3);
        assert_eq!(retained_log_count(&backend), 0);
        assert_eq!(cache.status().unwrap().log_gc.retained_records, 0);
        cache.close().unwrap();

        let reopened = Cache::from_backend(Arc::clone(&backend), config).unwrap();
        assert_eq!(
            reopened.get(b"checksum/live").unwrap().as_deref(),
            Some(b"value".as_slice())
        );
        assert_eq!(reopened.get(b"checksum/deleted").unwrap(), None);
        assert_eq!(reopened.applied_sequence(), 3);
        reopened.put(b"checksum/live", b"next").unwrap();
        assert_eq!(reopened.wait_applied().unwrap(), 4);
        assert_eq!(reopened.writeback.collect_expired_at(u64::MAX).unwrap(), 1);
        assert_eq!(reopened.status().unwrap().log_gc.reclaimed_records, 4);
        reopened.close().unwrap();
    }
}

#[test]
fn recovered_checkpoint_filters_delayed_put_and_delete_after_all_logs_are_gone() {
    for newer in [
        fixture_put(b"delayed/key", b"new"),
        Mutation::Delete {
            table_id: DEFAULT_TABLE_ID,
            key: b"delayed/key".to_vec(),
        },
    ] {
        let backend = Arc::new(MemBlobs::new());
        let format = checkpoint::encode_format().unwrap();
        backend
            .write_batch(&[BlobOp::Put {
                key: checkpoint::FORMAT_KEY,
                val: &format,
            }])
            .unwrap();
        let high = fixture_record(
            (1_u64 << crate::record::LOG_LANE_SHIFT) | 1,
            1_000,
            vec![newer.clone()],
        );
        let data_key = high.data_keys()[0].clone();
        let coordinator = ApplyCoordinator::empty();
        coordinator.apply(&*backend, &[high]).unwrap();
        assert!(matches!(
            coordinator.gc_step(&*backend, 1_001, 4_096).unwrap(),
            crate::writeback::GcStep::Progress { records: 1, .. }
        ));
        assert_eq!(retained_log_count(&backend), 0);
        let winning_envelope = backend.get(&data_key).unwrap().unwrap();
        drop(coordinator);

        // This reconstructs the coordinator's winner index from persisted
        // checkpoint bytes. No in-memory index is copied into the new cache.
        let cache = Cache::from_backend(Arc::clone(&backend), options(8)).unwrap();
        let lane = cache.writeback.lane(1).unwrap();
        for (physical_us, older) in [
            (800, fixture_put(b"delayed/key", b"old")),
            (
                900,
                Mutation::Delete {
                    table_id: DEFAULT_TABLE_ID,
                    key: b"delayed/key".to_vec(),
                },
            ),
        ] {
            let mut permit = lane
                .writeback()
                .reserve_single(lane.producer(), vec![older])
                .unwrap();
            let mut bound = permit
                .bind(MakoTimestamp::new(physical_us, 0, 1).unwrap())
                .unwrap();
            bound.publish().unwrap();
            cache.wait_applied().unwrap();
            assert_eq!(
                backend.get(&data_key).unwrap().as_deref(),
                Some(winning_envelope.as_slice()),
                "delayed record changed the recovered winner envelope"
            );
        }
        let metadata = checkpoint::LaneMetadata::decode(
            &backend.get(&checkpoint::lane_key(2)).unwrap().unwrap(),
        )
        .unwrap();
        assert_eq!((metadata.applied, metadata.reclaimed), (2, 0));
        assert_eq!(metadata.max_timestamp.unwrap().physical_us(), 900);
        assert!(
            metadata.retained_bytes > 0,
            "all-stale batches still retain their recent logs"
        );
        assert_eq!(cache.applied_sequence(), 3);
        cache.close().unwrap();
        let reopened = Cache::from_backend(backend, options(8)).unwrap();
        let expected = match &newer {
            Mutation::Put { value, .. } => Some(value.clone()),
            _ => None,
        };
        assert_eq!(reopened.get(b"delayed/key").unwrap(), expected);
        assert_eq!(reopened.applied_sequence(), 3);
        reopened.close().unwrap();
    }
}

#[test]
fn gc_recovery_rejects_two_timestamps_claiming_one_reclaimed_transaction() {
    let first = fixture_record(
        1,
        1_000,
        vec![
            fixture_put(b"identity/a", b"a"),
            fixture_put(b"identity/b", b"b"),
        ],
    );
    let second = fixture_record(2, 3_000, vec![fixture_put(b"identity/other", b"c")]);
    let backend = Arc::new(MemBlobs::new());
    seed_checkpoint(&backend, &[first, second]);
    let cache = Cache::from_backend(Arc::clone(&backend), options(8)).unwrap();
    assert_eq!(cache.writeback.collect_expired_at(u64::MAX).unwrap(), 2);
    cache.close().unwrap();
    let row_key = backend
        .snapshot()
        .keys()
        .find(|key| {
            matches!(
                classify_backend_key(key),
                BackendKey::Data {
                    key: b"identity/b",
                    ..
                }
            )
        })
        .unwrap()
        .clone();
    // Each envelope has a valid CRC. The invalidity is the conflicting source
    // identity across surviving rows after both source logs have disappeared.
    let forged = checkpoint::encode_row(
        MakoTimestamp::new(2_000, 0, 1).unwrap(),
        CommitSeq::new(1).unwrap(),
        Some(b"b"),
    )
    .unwrap();
    backend
        .write_batch(&[BlobOp::Put {
            key: &row_key,
            val: &forged,
        }])
        .unwrap();
    let result = Cache::from_backend(backend, options(8));
    assert!(
        matches!(result, Err(Error::BackendStateMismatch)),
        "conflicting row identities were accepted"
    );
}

#[test]
fn gc_recovery_rejects_extra_row_not_written_by_its_retained_source() {
    let record = fixture_record(1, 1_000, vec![fixture_put(b"source/real", b"value")]);
    let unrelated = fixture_record(2, 2_000, vec![fixture_put(b"source/extra", b"forged")]);
    let backend = Arc::new(MemBlobs::new());
    seed_checkpoint(&backend, std::slice::from_ref(&record));
    let extra_key = &unrelated.data_keys()[0];
    let forged =
        checkpoint::encode_row(record.mako_timestamp(), record.sequence(), Some(b"forged"))
            .unwrap();
    backend
        .write_batch(&[BlobOp::Put {
            key: extra_key,
            val: &forged,
        }])
        .unwrap();
    let result = Cache::from_backend(backend, options(8));
    assert!(
        matches!(result, Err(Error::BackendStateMismatch)),
        "extra row outside its source transaction was accepted"
    );
}

#[derive(Debug, Default)]
struct AmbiguousGcBlobs {
    inner: MemBlobs,
    fail_gc: AtomicBool,
    applied_gc: AtomicBool,
    attempts: Mutex<Vec<Vec<(Vec<u8>, Option<Vec<u8>>)>>>,
}

impl Blobs for AmbiguousGcBlobs {
    fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
        self.inner.get(key)
    }
    fn for_each_key(&self, callback: &mut dyn FnMut(&[u8])) -> Result<(), BlobError> {
        self.inner.for_each_key(callback)
    }
    fn for_each_entry(
        &self,
        callback: &mut dyn FnMut(&[u8], &[u8]) -> Result<(), BlobError>,
    ) -> Result<(), BlobError> {
        self.inner.for_each_entry(callback)
    }
    fn write_batch(&self, operations: &[BlobOp<'_>]) -> Result<(), BlobError> {
        let is_gc = operations.iter().any(|operation| {
            matches!(operation,
            BlobOp::Delete { key } if matches!(classify_backend_key(key), BackendKey::Log(_)))
        });
        if is_gc {
            self.attempts.lock().unwrap().push(
                operations
                    .iter()
                    .map(|operation| match operation {
                        BlobOp::Put { key, val } => (key.to_vec(), Some(val.to_vec())),
                        BlobOp::Delete { key } => (key.to_vec(), None),
                    })
                    .collect(),
            );
            if self.fail_gc.load(Ordering::SeqCst) {
                if !self.applied_gc.swap(true, Ordering::SeqCst) {
                    self.inner.write_batch(operations)?;
                }
                return Err(BlobError("injected error after GC application".into()));
            }
        }
        self.inner.write_batch(operations)
    }
}

#[test]
fn ambiguous_gc_blocks_later_application_and_retries_identical_checkpoint_bytes() {
    let backend = Arc::new(AmbiguousGcBlobs::default());
    let cache = Cache::from_backend(Arc::clone(&backend), options(8)).unwrap();
    cache.put(b"ambiguous/before", b"checkpointed").unwrap();
    cache.wait_applied().unwrap();
    backend.fail_gc.store(true, Ordering::SeqCst);
    assert!(cache.writeback.collect_expired_at(u64::MAX).is_err());
    assert_eq!(
        retained_log_count(&backend.inner),
        0,
        "GC applied despite returning an error"
    );
    cache.put(b"ambiguous/after", b"queued").unwrap();
    assert_eq!(cache.highest_acknowledged_sequence(), 2);
    assert_eq!(cache.applied_sequence(), 1);
    assert!(cache.status().unwrap().log_gc.pending_retry);

    backend.fail_gc.store(false, Ordering::SeqCst);
    cache.wait_applied().unwrap();
    let attempts = backend.attempts.lock().unwrap().clone();
    assert!(attempts.len() >= 2);
    assert!(
        attempts.iter().all(|attempt| *attempt == attempts[0]),
        "GC retry changed operation bytes"
    );
    assert_eq!(
        cache.status().unwrap().log_gc.reclaimed_records,
        1,
        "ambiguous GC was counted twice"
    );
    assert_eq!(cache.close().unwrap(), 2);
    let reopened = Cache::from_backend(backend, options(8)).unwrap();
    assert_eq!(reopened.applied_sequence(), 2);
    assert_eq!(
        reopened.get(b"ambiguous/before").unwrap().as_deref(),
        Some(b"checkpointed".as_slice())
    );
    assert_eq!(
        reopened.get(b"ambiguous/after").unwrap().as_deref(),
        Some(b"queued".as_slice())
    );
    assert_eq!(reopened.close().unwrap(), 2);
}

#[test]
fn stopping_after_ambiguous_gc_recovers_checkpoint_and_drops_only_queued_transactions() {
    let backend = Arc::new(AmbiguousGcBlobs::default());
    let cache = Cache::from_backend(Arc::clone(&backend), options(8)).unwrap();
    cache
        .put(b"ambiguous-stop/before", b"checkpointed")
        .unwrap();
    cache.wait_applied().unwrap();
    backend.fail_gc.store(true, Ordering::SeqCst);
    assert!(cache.writeback.collect_expired_at(u64::MAX).is_err());
    cache.put(b"ambiguous-stop/after", b"volatile").unwrap();
    cache.abort_without_flush().unwrap();
    assert_eq!(retained_log_count(&backend.inner), 0);
    backend.fail_gc.store(false, Ordering::SeqCst);
    let reopened = Cache::from_backend(backend, options(8)).unwrap();
    assert_eq!(reopened.applied_sequence(), 1);
    assert_eq!(
        reopened.get(b"ambiguous-stop/before").unwrap().as_deref(),
        Some(b"checkpointed".as_slice())
    );
    assert_eq!(reopened.get(b"ambiguous-stop/after").unwrap(), None);
    assert_eq!(reopened.status().unwrap().log_gc.reclaimed_records, 1);
    assert_eq!(reopened.close().unwrap(), 1);
}

#[test]
fn gc_stops_at_first_unexpired_record_in_untagged_stream_despite_later_expired_records() {
    // The untagged stream does not guarantee monotonic HLCs. GC must stop at
    // the first unexpired record and preserve the dense suffix, even when
    // later records in lane order are expired.
    let backend = Arc::new(MemBlobs::new());
    let coordinator = ApplyCoordinator::empty();
    // Lane 0 (untagged): sequence 1, 2, 3 with reversed timestamps.
    for (sequence, physical_us) in [(1, 1_000), (2, 3_000), (3, 2_000)] {
        let record = fixture_record(
            sequence,
            physical_us,
            vec![fixture_put(b"untagged-gc/key", &sequence.to_be_bytes())],
        );
        assert!(matches!(
            coordinator.apply(&*backend, &[record]).unwrap(),
            CoordinatorApplyOutcome::Applied
        ));
    }
    // Cutoff at 1,500: record 1 (1,000) is expired, record 2 (3,000) is
    // unexpired, record 3 (2,000) is expired but after the unexpired one.
    let step = coordinator
        .gc_step(&*backend, 1_500, WritebackConfig::default().max_record_bytes)
        .unwrap();
    assert!(
        matches!(step, crate::writeback::GcStep::Progress { records: 1, .. }),
        "expected exactly one reclaimed record, got {step:?}"
    );
    // The dense suffix (2, 3] must remain.
    assert_eq!(retained_log_count(&backend), 2);
    let metadata = checkpoint::LaneMetadata::decode(
        &backend.get(&checkpoint::lane_key(0)).unwrap().unwrap(),
    )
    .unwrap();
    assert_eq!(metadata.reclaimed, 1);
    assert_eq!(metadata.applied, 3);

    // Recovery must accept the non-monotonic lane and reconstruct the winner.
    let cache = Cache::from_backend(backend, options(8)).unwrap();
    assert_eq!(cache.applied_sequence(), 3);
    assert_eq!(
        cache.get(b"untagged-gc/key").unwrap().as_deref(),
        Some(&3_u64.to_be_bytes()[..])
    );
    cache.close().unwrap();
}

#[test]
fn recovered_empty_database_advances_hlc_and_lane_ids_past_checkpoint_state() {
    // Apply one transaction, reclaim every log, delete the only key, close.
    // Reopening must still advance the HLC floor and lane sequence past the
    // recovered (now empty) state so new writes cannot reuse old identities.
    let backend = Arc::new(MemBlobs::new());
    let cache = Cache::from_backend(Arc::clone(&backend), options(8)).unwrap();
    cache.put(b"empty/sole", b"value").unwrap();
    cache.wait_applied().unwrap();
    assert_eq!(cache.writeback.collect_expired_at(u64::MAX).unwrap(), 1);
    assert!(cache.delete(b"empty/sole").unwrap());
    cache.wait_applied().unwrap();
    assert_eq!(cache.writeback.collect_expired_at(u64::MAX).unwrap(), 2);
    assert_eq!(retained_log_count(&backend), 0);
    let before_watermark = cache.applied_watermark();
    let before_sequence = cache.applied_sequence();
    cache.close().unwrap();

    let reopened = Cache::from_backend(Arc::clone(&backend), options(8)).unwrap();
    assert_eq!(reopened.applied_sequence(), before_sequence);
    assert_eq!(reopened.applied_watermark(), before_watermark);
    assert_eq!(reopened.get(b"empty/sole").unwrap(), None);

    // A new write must receive an HLC strictly greater than the recovered
    // floor, even though the database contains no live keys or retained logs.
    reopened.put(b"empty/after", b"new").unwrap();
    reopened.wait_applied().unwrap();
    assert!(reopened.applied_watermark().mako_timestamp() > before_watermark.mako_timestamp());
    assert_eq!(reopened.applied_sequence(), before_sequence + 1);

    // The new transaction's log must start at A + 1, not reuse sequence 1.
    assert_eq!(retained_log_count(&backend), 1);
    reopened.close().unwrap();
}
