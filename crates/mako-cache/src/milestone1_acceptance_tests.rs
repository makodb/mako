//! Integrated acceptance coverage for the bounded single-machine cache.
//!
//! The lower-level write-back suite proves each queue transition separately.
//! These tests retain a real [`Cache`] around the native Silo transaction path
//! and exercise the Milestone 1 overload and shutdown contracts end to end.

use std::env;
use std::process::Command;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Barrier, Condvar, Mutex, mpsc};
use std::time::{Duration, Instant};

use mrx_core::fakes::MemBlobs;
use mrx_core::{BlobError, BlobOp, Blobs};

use crate::record::{BackendKey, CommitSeq, DEFAULT_TABLE_ID, Mutation, PreparedCommitRecord};
use crate::{Cache, CacheOptions, Error, LocalError, MakoTimestamp, WritebackConfig};

const WAIT_LIMIT: Duration = Duration::from_secs(5);
const NEAR_EXHAUSTION_ROLE_ENV: &str = "MAKO_CACHE_NEAR_EXHAUSTION_ROLE";

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
            assert!(
                seed.put(key.as_bytes(), b"seed")
                    .expect("stage overload seed")
            );
        }
        seed.commit().expect("commit overload seed");
    }
    let base_sequence = cache.wait_applied().expect("drain overload seed");
    let base_batches = backend.inner.batch_count();
    assert_eq!(base_sequence, WORKERS as u64);
    assert_eq!(base_batches, WORKERS as u64);
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
                assert!(
                    !cache_transaction
                        .put(key.as_bytes(), value.as_bytes())
                        .expect("stage existing disjoint overload write")
                );
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
    let queue_saturated = wait_until(|| completed.load(Ordering::SeqCst) >= CAPACITY);
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
        completed_while_blocked, CAPACITY,
        "more commits acknowledged than the closed write-back queue can retain"
    );
    assert_eq!(acknowledged_while_blocked, base_sequence + CAPACITY as u64);
    assert_eq!(applied_while_blocked, base_sequence);
    assert_eq!(queued_while_blocked, CAPACITY);
    assert!(
        maximum_observed_queue.load(Ordering::SeqCst) <= CAPACITY,
        "observed queue occupancy exceeded its configured bound"
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
    assert_eq!(backend.inner.batch_count(), ACCEPTED as u64);

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
    assert_eq!(backend.batch_count(), 1);

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
        1,
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
    let maximum = mako_local::MAX_MAKO_TIMESTAMP;
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
        MakoTimestamp::new(maximum - 1).expect("MAX-1 is a valid Mako timestamp"),
    )
    .finalize();
    backend
        .write_batch(&recovered.backend_ops())
        .expect("seed complete MAX-1 backend record");

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
                BackendKey::Log(sequence) if sequence.get() == 2 => Some(
                    crate::record::CommitRecord::decode(
                        &key,
                        &encoded,
                        WritebackConfig::default().max_record_bytes,
                    )
                    .expect("decode MAX transaction record")
                    .mako_timestamp(),
                ),
                BackendKey::Log(_) | BackendKey::Data { .. } | BackendKey::Foreign => None,
            },
        )
        .expect("find MAX transaction record");
    assert_eq!(final_timestamp.get(), maximum);

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
    assert_eq!(backend.batch_count(), 2);
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
