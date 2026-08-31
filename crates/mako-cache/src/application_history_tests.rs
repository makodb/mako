//! Application-aware full-history coverage for visible, acknowledged, and
//! asynchronously applied cache state.

use std::sync::{Arc, Condvar, Mutex, OnceLock, mpsc};
use std::time::{Duration, Instant};

use mako_history::{
    ApplicationCheckFailureKind, ApplicationCommit, ApplicationCommitOutcome, ApplicationHistory,
    BackendAttempt, BackendAttemptOutcome, CacheSeq, CheckOptions, FrontierObservation, History,
    Interval, LogicalClock, ModelMutation, Observation, Operation, Semantics, State, TerminalCall,
    TerminalOutcome, TimedOperation, Transaction as HistoryTransaction, WaitAppliedObservation,
    WaitAppliedOutcome, check_application, state_insert,
};
use mrx_core::fakes::MemBlobs;
use mrx_core::{BlobError, BlobOp, Blobs};

use crate::record::{BackendKey, CommitRecord, DEFAULT_TABLE_ID, Mutation, classify_backend_key};
use crate::{Cache, CacheOptions, Transaction};

const PREFIX: &[u8] = b"phase1f/application/";
const CHAIN: &[u8] = b"phase1f/application/chain";
const A: &[u8] = b"phase1f/application/a";
const B: &[u8] = b"phase1f/application/b";
const C: &[u8] = b"phase1f/application/c";
const KEYS: &[&[u8]] = &[CHAIN, A, B, C];
const CONCURRENT_FIRST: &[u8] = b"phase1f/application/concurrent-first";
const CONCURRENT_SECOND: &[u8] = b"phase1f/application/concurrent-second";

#[derive(Debug, Default)]
struct BackendGate {
    entered: bool,
    released: bool,
}

#[derive(Debug, Default)]
struct ResponseGate {
    entered: bool,
    released: bool,
}

fn response_gate() -> &'static (Mutex<ResponseGate>, Condvar) {
    static GATE: OnceLock<(Mutex<ResponseGate>, Condvar)> = OnceLock::new();
    GATE.get_or_init(|| (Mutex::new(ResponseGate::default()), Condvar::new()))
}

fn reset_response_gate() {
    let (gate, _) = response_gate();
    let mut state = gate.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    *state = ResponseGate::default();
}

fn wait_for_delayed_response() {
    let (gate, changed) = response_gate();
    let deadline = Instant::now() + Duration::from_secs(5);
    let mut state = gate.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    while !state.entered {
        let now = Instant::now();
        if now >= deadline {
            drop(state);
            panic!("first native commit did not reach its response gate");
        }
        let (next, timeout) = changed
            .wait_timeout(state, deadline - now)
            .expect("response gate poisoned while waiting");
        state = next;
        if timeout.timed_out() && !state.entered {
            drop(state);
            panic!("first native commit did not reach its response gate");
        }
    }
}

fn release_delayed_response() {
    let (gate, changed) = response_gate();
    let mut state = gate.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    state.released = true;
    changed.notify_all();
}

fn delay_after_native_commit() {
    let (gate, changed) = response_gate();
    let mut state = gate.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    state.entered = true;
    changed.notify_all();
    while !state.released {
        state = changed
            .wait(state)
            .expect("response gate poisoned while delaying commit response");
    }
}

struct ReleaseResponseOnDrop;

impl Drop for ReleaseResponseOnDrop {
    fn drop(&mut self) {
        release_delayed_response();
    }
}

/// In-memory backend whose pre-call gate makes a real write-back backlog
/// deterministic and whose transcript is decoded through the production log
/// reader. The gate sits before the recorded backend invocation, so snapshots
/// taken while it is closed are stably before the backend attempt.
#[derive(Debug)]
struct RecordingBlobs {
    inner: MemBlobs,
    clock: Arc<LogicalClock>,
    attempts: Mutex<Vec<BackendAttempt>>,
    gate: Mutex<BackendGate>,
    changed: Condvar,
}

impl RecordingBlobs {
    fn new(clock: Arc<LogicalClock>) -> Self {
        Self {
            inner: MemBlobs::new(),
            clock,
            attempts: Mutex::new(Vec::new()),
            gate: Mutex::new(BackendGate::default()),
            changed: Condvar::new(),
        }
    }

    fn wait_until_blocked(&self) {
        let deadline = Instant::now() + Duration::from_secs(5);
        let mut gate = self.gate.lock().expect("backend gate poisoned");
        while !gate.entered {
            let now = Instant::now();
            assert!(now < deadline, "write-back did not reach the backend gate");
            let (next, timeout) = self
                .changed
                .wait_timeout(gate, deadline - now)
                .expect("backend gate poisoned while waiting");
            gate = next;
            assert!(
                !timeout.timed_out() || gate.entered,
                "write-back did not reach the backend gate"
            );
        }
    }

    fn release(&self) {
        let mut gate = self.gate.lock().expect("backend gate poisoned");
        gate.released = true;
        self.changed.notify_all();
    }

    fn attempts(&self) -> Vec<BackendAttempt> {
        self.attempts
            .lock()
            .expect("backend transcript poisoned")
            .clone()
    }

    fn application_state(&self) -> State {
        let mut state = State::new();
        for (key, value) in self.inner.snapshot() {
            if let BackendKey::Data { table_id, key } = classify_backend_key(&key) {
                if table_id == DEFAULT_TABLE_ID && key.starts_with(PREFIX) {
                    state_insert(&mut state, table_id, key.to_vec(), value);
                }
            }
        }
        state
    }

    fn decode_attempt(operations: &[BlobOp<'_>]) -> Vec<(CacheSeq, Vec<ModelMutation>)> {
        let mut decoded = Vec::new();
        let mut offset = 0;
        while offset < operations.len() {
            let (log_key, encoded) = match operations.get(offset) {
                Some(BlobOp::Put { key, val }) => (*key, *val),
                Some(BlobOp::Delete { .. }) | None => {
                    panic!("each cache transaction must begin with its commit-record put")
                }
            };
            let record = CommitRecord::decode(
                log_key,
                encoded,
                CacheOptions::default().writeback.max_record_bytes,
            )
            .expect("decode recorded backend attempt");
            let end = offset
                .checked_add(record.mutations().len() + 1)
                .expect("materialized backend batch length overflow");
            assert!(
                end <= operations.len(),
                "materialized backend batch length differs from its commit record"
            );
            for (index, (actual, expected)) in operations[offset + 1..end]
                .iter()
                .zip(record.mutations())
                .enumerate()
            {
                let identical = match (actual, expected) {
                    (
                        BlobOp::Put {
                            key: actual_key,
                            val: actual_value,
                        },
                        Mutation::Put {
                            table_id,
                            key,
                            value,
                        },
                    ) => {
                        matches!(
                            classify_backend_key(actual_key),
                            BackendKey::Data {
                                table_id: actual_table,
                                key: actual_data_key,
                            } if actual_table == *table_id && actual_data_key == key
                        ) && *actual_value == value
                    }
                    (BlobOp::Delete { key: actual_key }, Mutation::Delete { table_id, key }) => {
                        matches!(
                            classify_backend_key(actual_key),
                            BackendKey::Data {
                                table_id: actual_table,
                                key: actual_data_key,
                            } if actual_table == *table_id && actual_data_key == key
                        )
                    }
                    (BlobOp::Put { .. }, Mutation::Delete { .. })
                    | (BlobOp::Delete { .. }, Mutation::Put { .. }) => false,
                };
                assert!(
                    identical,
                    "materialized backend operation {} differs from its decoded commit mutation: expected {expected:?}, observed {actual:?}",
                    index + 1
                );
            }
            let sequence =
                CacheSeq::new(record.sequence().get()).expect("cache sequence is nonzero");
            let mutations = record
                .mutations()
                .iter()
                .map(|mutation| match mutation {
                    Mutation::Put {
                        table_id,
                        key,
                        value,
                    } => ModelMutation::put(*table_id, key.clone(), value.clone()),
                    Mutation::Delete { table_id, key } => {
                        ModelMutation::delete(*table_id, key.clone())
                    }
                })
                .collect();
            decoded.push((sequence, mutations));
            offset = end;
        }
        assert!(!decoded.is_empty(), "cache backend batch must not be empty");
        for pair in decoded.windows(2) {
            assert_eq!(
                pair[1].0.get(),
                pair[0].0.get() + 1,
                "one physical backend batch must contain a dense sequence prefix"
            );
        }
        decoded
    }
}

fn transcript_record(sequence: u64, mako_timestamp: u32, mutations: Vec<Mutation>) -> CommitRecord {
    crate::record::PreparedCommitRecord::prepare(
        mutations,
        CacheOptions::default().writeback.max_record_bytes,
    )
    .expect("prepare transcript record")
    .bind(
        crate::record::CommitSeq::new(sequence).expect("nonzero transcript sequence"),
        mako_local::MakoTimestamp::new(mako_timestamp).expect("nonzero transcript timestamp"),
    )
    .finalize()
}

#[test]
fn application_transcript_decodes_every_record_in_a_physical_batch() {
    let records = [
        transcript_record(
            1,
            11,
            vec![Mutation::Put {
                table_id: DEFAULT_TABLE_ID,
                key: b"phase1f/transcript/a".to_vec(),
                value: b"one".to_vec(),
            }],
        ),
        transcript_record(
            2,
            12,
            vec![Mutation::Delete {
                table_id: DEFAULT_TABLE_ID,
                key: b"phase1f/transcript/b".to_vec(),
            }],
        ),
    ];
    let mut operations = Vec::new();
    for record in &records {
        record.append_backend_ops(&mut operations);
    }

    let decoded = RecordingBlobs::decode_attempt(&operations);
    assert_eq!(
        decoded,
        vec![
            (
                CacheSeq::new(1).unwrap(),
                vec![ModelMutation::put(
                    DEFAULT_TABLE_ID,
                    b"phase1f/transcript/a",
                    b"one",
                )],
            ),
            (
                CacheSeq::new(2).unwrap(),
                vec![ModelMutation::delete(
                    DEFAULT_TABLE_ID,
                    b"phase1f/transcript/b",
                )],
            ),
        ]
    );
}

#[test]
fn application_transcript_projects_atomic_failure_to_front_then_full_retry() {
    let records = [
        transcript_record(
            1,
            21,
            vec![Mutation::Put {
                table_id: DEFAULT_TABLE_ID,
                key: b"phase1f/transcript/retry-a".to_vec(),
                value: b"one".to_vec(),
            }],
        ),
        transcript_record(
            2,
            22,
            vec![Mutation::Put {
                table_id: DEFAULT_TABLE_ID,
                key: b"phase1f/transcript/retry-b".to_vec(),
                value: b"two".to_vec(),
            }],
        ),
    ];
    let mut operations = Vec::new();
    for record in &records {
        record.append_backend_ops(&mut operations);
    }

    let clock = Arc::new(LogicalClock::default());
    let backend = RecordingBlobs::new(clock);
    backend.release();
    backend.inner.fail_next_writes(1);

    assert!(backend.write_batch(&operations).is_err());
    assert!(
        backend.inner.snapshot().is_empty(),
        "the failed physical batch must apply no partial state"
    );
    assert!(backend.write_batch(&operations).is_ok());

    let attempts = backend.attempts();
    assert_eq!(
        attempts
            .iter()
            .map(|attempt| (attempt.seq.get(), attempt.outcome))
            .collect::<Vec<_>>(),
        vec![
            (1, BackendAttemptOutcome::Failed),
            (1, BackendAttemptOutcome::Succeeded),
            (2, BackendAttemptOutcome::Succeeded),
        ]
    );
    for pair in attempts.windows(2) {
        assert!(
            pair[1].interval.invocation > pair[0].interval.response.unwrap(),
            "projected logical attempts must be strictly sequential"
        );
    }
    assert_eq!(backend.inner.batch_count(), 1);
}

#[test]
#[should_panic(expected = "materialized backend batch length differs from its commit record")]
fn application_transcript_rejects_a_partial_materialized_batch() {
    let record = crate::record::PreparedCommitRecord::prepare(
        vec![Mutation::Put {
            table_id: DEFAULT_TABLE_ID,
            key: b"phase1f/transcript/partial".to_vec(),
            value: b"must-be-materialized".to_vec(),
        }],
        CacheOptions::default().writeback.max_record_bytes,
    )
    .expect("prepare transcript tripwire")
    .bind(
        crate::record::CommitSeq::new(1).unwrap(),
        mako_local::MakoTimestamp::new(1).unwrap(),
    )
    .finalize();
    let complete = record.backend_ops();

    let _ = RecordingBlobs::decode_attempt(&complete[..1]);
}

/// Ensure a failed assertion cannot leave `Cache::drop` joining a writer that
/// is still parked at the deterministic backend gate.
struct ReleaseBackendOnDrop<'backend>(&'backend RecordingBlobs);

impl Drop for ReleaseBackendOnDrop<'_> {
    fn drop(&mut self) {
        self.0.release();
    }
}

impl Blobs for RecordingBlobs {
    fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
        self.inner.get(key)
    }

    fn write_batch(&self, operations: &[BlobOp<'_>]) -> Result<(), BlobError> {
        let decoded = Self::decode_attempt(operations);
        let mut gate = self.gate.lock().expect("backend gate poisoned");
        gate.entered = true;
        self.changed.notify_all();
        while !gate.released {
            gate = self
                .changed
                .wait(gate)
                .expect("backend gate poisoned while blocked");
        }
        drop(gate);

        let result = self.inner.write_batch(operations);
        let outcome = if result.is_ok() {
            BackendAttemptOutcome::Succeeded
        } else {
            BackendAttemptOutcome::Failed
        };
        let mut attempts = self.attempts.lock().expect("backend transcript poisoned");
        // The independent application checker models one logical record per
        // attempt. A successful physical multi-record batch therefore becomes
        // consecutive zero-overlap logical effects. An atomic failed batch is
        // represented by its front record only: no later sequence was eligible
        // to advance, and the same physical prefix will be retried from there.
        let logical_records = if result.is_ok() { decoded.len() } else { 1 };
        for (sequence, mutations) in decoded.into_iter().take(logical_records) {
            let invocation = self.clock.next();
            let response = self.clock.next();
            attempts.push(BackendAttempt::new(
                sequence,
                Interval::completed(invocation, response),
                mutations,
                outcome,
            ));
        }
        result
    }

    fn for_each_key(&self, f: &mut dyn FnMut(&[u8])) -> Result<(), BlobError> {
        self.inner.for_each_key(f)
    }
}

type TestCache = Cache<Arc<RecordingBlobs>>;

struct RecordedTransaction<'cache> {
    cache: &'cache TestCache,
    clock: &'cache LogicalClock,
    native: Transaction<'cache, Arc<RecordingBlobs>>,
    history: HistoryTransaction,
}

impl<'cache> RecordedTransaction<'cache> {
    fn begin(id: u32, cache: &'cache TestCache, clock: &'cache LogicalClock) -> Self {
        let invocation = clock.next();
        let native = cache
            .transaction()
            .expect("begin recorded cache transaction");
        let response = clock.next();
        Self {
            cache,
            clock,
            native,
            history: HistoryTransaction::new(id, Interval::completed(invocation, response)),
        }
    }

    fn put(&mut self, key: &[u8], value: &[u8]) {
        let invocation = self.clock.next();
        let created = self.native.put(key, value).expect("recorded put");
        let response = self.clock.next();
        self.history.push(TimedOperation::completed(
            invocation,
            response,
            Operation::put(DEFAULT_TABLE_ID, key.to_vec(), value.to_vec()),
            Observation::Put { created },
        ));
    }

    fn remove(&mut self, key: &[u8]) {
        let invocation = self.clock.next();
        let existed = self.native.remove(key).expect("recorded remove");
        let response = self.clock.next();
        self.history.push(TimedOperation::completed(
            invocation,
            response,
            Operation::remove(DEFAULT_TABLE_ID, key.to_vec()),
            Observation::Remove { existed },
        ));
    }

    fn commit(self) -> (HistoryTransaction, ApplicationCommit) {
        self.commit_with_expected_sequence(None)
    }

    fn commit_as(self, sequence: CacheSeq) -> (HistoryTransaction, ApplicationCommit) {
        self.commit_with_expected_sequence(Some(sequence))
    }

    fn commit_with_expected_sequence(
        mut self,
        expected_sequence: Option<CacheSeq>,
    ) -> (HistoryTransaction, ApplicationCommit) {
        let invocation = self.clock.next();
        self.native.commit().expect("commit recorded transaction");
        let response = self.clock.next();
        let acknowledged = self.cache.highest_acknowledged_sequence();
        let sequence = expected_sequence.unwrap_or_else(|| {
            CacheSeq::new(acknowledged).expect("write commit receives a sequence")
        });
        assert!(
            acknowledged >= sequence.get(),
            "expected sequence {} was not acknowledged after its commit",
            sequence
        );
        self.history.finish(TerminalCall::commit(
            invocation,
            response,
            TerminalOutcome::Committed,
        ));
        let id = self.history.id;
        (
            self.history,
            ApplicationCommit::new(
                id,
                Interval::completed(invocation, response),
                ApplicationCommitOutcome::AcknowledgedWrite { seq: sequence },
            ),
        )
    }
}

fn visible_state_for(cache: &TestCache, keys: &[&[u8]]) -> State {
    let mut state = State::new();
    let mut transaction = cache.transaction().expect("begin visible snapshot");
    for &key in keys {
        if let Some(value) = transaction.get(key).expect("read visible snapshot") {
            state_insert(&mut state, DEFAULT_TABLE_ID, key.to_vec(), value);
        }
    }
    transaction.commit().expect("commit visible snapshot");
    state
}

fn visible_state(cache: &TestCache) -> State {
    visible_state_for(cache, KEYS)
}

fn observe_frontier(
    cache: &TestCache,
    backend: &RecordingBlobs,
    clock: &LogicalClock,
) -> FrontierObservation {
    let invocation = clock.next();
    let acknowledged = CacheSeq::new(cache.highest_acknowledged_sequence());
    let applied = CacheSeq::new(cache.applied_sequence());
    let visible_state = visible_state(cache);
    let backend_state = backend.application_state();
    let response = clock.next();
    FrontierObservation {
        interval: Interval::completed(invocation, response),
        highest_acknowledged: acknowledged,
        applied,
        visible_state: Some(visible_state),
        backend_state: Some(backend_state),
    }
}

fn observe_frontier_for(
    cache: &TestCache,
    backend: &RecordingBlobs,
    clock: &LogicalClock,
    keys: &[&[u8]],
) -> FrontierObservation {
    let invocation = clock.next();
    let acknowledged = CacheSeq::new(cache.highest_acknowledged_sequence());
    let applied = CacheSeq::new(cache.applied_sequence());
    let visible_state = visible_state_for(cache, keys);
    let backend_state = backend.application_state();
    let response = clock.next();
    FrontierObservation {
        interval: Interval::completed(invocation, response),
        highest_acknowledged: acknowledged,
        applied,
        visible_state: Some(visible_state),
        backend_state: Some(backend_state),
    }
}

fn final_state() -> State {
    let mut state = State::new();
    state_insert(&mut state, DEFAULT_TABLE_ID, CHAIN.to_vec(), b"v3".to_vec());
    state_insert(&mut state, DEFAULT_TABLE_ID, C.to_vec(), b"three".to_vec());
    state
}

fn concurrent_final_state() -> State {
    let mut state = State::new();
    state_insert(
        &mut state,
        DEFAULT_TABLE_ID,
        CONCURRENT_FIRST.to_vec(),
        b"first".to_vec(),
    );
    state_insert(
        &mut state,
        DEFAULT_TABLE_ID,
        CONCURRENT_SECOND.to_vec(),
        b"second".to_vec(),
    );
    state
}

fn recorded_concurrent_prefix_acknowledgement() -> ApplicationHistory {
    reset_response_gate();
    let clock = Arc::new(LogicalClock::default());
    let backend = Arc::new(RecordingBlobs::new(Arc::clone(&clock)));
    let cache = Cache::from_backend(Arc::clone(&backend), CacheOptions::default())
        .expect("open concurrent application-history cache");
    let _release_backend_on_unwind = ReleaseBackendOnDrop(&backend);

    let (first, second) = std::thread::scope(|scope| {
        let _release_response_on_unwind = ReleaseResponseOnDrop;
        let first_cache = &cache;
        let first_clock = &clock;
        let first_worker = scope.spawn(move || {
            let mut transaction = RecordedTransaction::begin(1, first_cache, first_clock);
            transaction.put(CONCURRENT_FIRST, b"first");
            // Park only after the native commit C ABI has returned. Silo has
            // released its write locks, but the bound cache slot is not Ready,
            // so this reorders wrapper responses without violating the native
            // phase-observer contract.
            crate::failpoint::install_post_native_commit_observer(delay_after_native_commit);
            let result = transaction.commit_as(CacheSeq::new(1).unwrap());
            crate::failpoint::clear_post_native_commit_observer();
            result
        });

        wait_for_delayed_response();
        let (second_tx, second_rx) = mpsc::sync_channel(1);
        let second_cache = &cache;
        let second_clock = &clock;
        let second_worker = scope.spawn(move || {
            let mut transaction = RecordedTransaction::begin(2, second_cache, second_clock);
            transaction.put(CONCURRENT_SECOND, b"second");
            second_tx
                .send(transaction.commit_as(CacheSeq::new(2).unwrap()))
                .expect("receive the second commit result");
        });

        let second_bound_deadline = Instant::now() + Duration::from_secs(5);
        while cache.queued_transactions() != 2 && Instant::now() < second_bound_deadline {
            std::thread::yield_now();
        }
        assert_eq!(
            cache.queued_transactions(),
            2,
            "the second native commit did not bind behind the parked first commit"
        );
        assert!(
            matches!(
                second_rx.recv_timeout(Duration::from_millis(30)),
                Err(mpsc::RecvTimeoutError::Timeout)
            ),
            "a Ready suffix was acknowledged across an unresolved prefix"
        );
        release_delayed_response();
        let first = first_worker
            .join()
            .unwrap_or_else(|panic| std::panic::resume_unwind(panic));
        let second = second_rx
            .recv_timeout(Duration::from_secs(5))
            .expect("second commit completes after the prefix resolves");
        second_worker
            .join()
            .unwrap_or_else(|panic| std::panic::resume_unwind(panic));
        (first, second)
    });

    backend.wait_until_blocked();
    let keys = [CONCURRENT_FIRST, CONCURRENT_SECOND];
    let lagging = observe_frontier_for(&cache, &backend, &clock, &keys);
    assert_eq!(lagging.highest_acknowledged, CacheSeq::new(2));
    assert_eq!(lagging.applied, None);
    assert_eq!(
        lagging.visible_state.as_ref(),
        Some(&concurrent_final_state())
    );
    assert_eq!(lagging.backend_state.as_ref(), Some(&State::new()));

    backend.release();
    let wait_invocation = clock.next();
    let target = cache.wait_applied().expect("apply concurrent history");
    let wait_response = clock.next();
    assert_eq!(target, 2);
    let wait = WaitAppliedObservation {
        interval: Interval::completed(wait_invocation, wait_response),
        outcome: WaitAppliedOutcome::Ok {
            target: Some(CacheSeq::new(target).unwrap()),
        },
    };
    let applied = observe_frontier_for(&cache, &backend, &clock, &keys);
    assert_eq!(applied.applied, CacheSeq::new(2));
    assert_eq!(
        applied.visible_state.as_ref(),
        Some(&concurrent_final_state())
    );
    assert_eq!(
        applied.backend_state.as_ref(),
        Some(&concurrent_final_state())
    );

    let attempts = backend.attempts();
    assert_eq!(attempts.len(), 2);
    assert_eq!(attempts[0].seq, CacheSeq::new(1).unwrap());
    assert_eq!(attempts[1].seq, CacheSeq::new(2).unwrap());
    assert_eq!(
        attempts[0].mutations,
        vec![ModelMutation::put(
            DEFAULT_TABLE_ID,
            CONCURRENT_FIRST,
            b"first"
        )]
    );
    assert_eq!(
        attempts[1].mutations,
        vec![ModelMutation::put(
            DEFAULT_TABLE_ID,
            CONCURRENT_SECOND,
            b"second"
        )]
    );
    assert_eq!(cache.close().expect("close concurrent cache"), 2);

    let mut transactions = History::new(State::new());
    transactions.set_observed_final_state(concurrent_final_state());
    transactions.push(first.0).push(second.0);
    let mut history = ApplicationHistory::new(transactions);
    history.commits.extend([first.1, second.1]);
    history.backend_attempts = attempts;
    history.frontiers.extend([lagging, applied]);
    history.waits.push(wait);
    history
}

fn recorded_application_history() -> ApplicationHistory {
    let clock = Arc::new(LogicalClock::default());
    let backend = Arc::new(RecordingBlobs::new(Arc::clone(&clock)));
    let cache = Cache::from_backend(Arc::clone(&backend), CacheOptions::default())
        .expect("open application-history cache");

    let mut transactions = Vec::new();
    let mut commits = Vec::new();

    let mut first = RecordedTransaction::begin(1, &cache, &clock);
    first.put(CHAIN, b"v1");
    first.put(A, b"one");
    let (transaction, commit) = first.commit();
    transactions.push(transaction);
    commits.push(commit);
    let _release_backend_on_unwind = ReleaseBackendOnDrop(&backend);
    backend.wait_until_blocked();

    let mut second = RecordedTransaction::begin(2, &cache, &clock);
    second.put(CHAIN, b"stale-intermediate");
    second.put(CHAIN, b"v2");
    second.remove(A);
    second.put(B, b"two");
    let (transaction, commit) = second.commit();
    transactions.push(transaction);
    commits.push(commit);

    let mut third = RecordedTransaction::begin(3, &cache, &clock);
    third.put(CHAIN, b"v3");
    third.remove(B);
    third.put(C, b"three");
    let (transaction, commit) = third.commit();
    transactions.push(transaction);
    commits.push(commit);

    let lagging = observe_frontier(&cache, &backend, &clock);
    assert_eq!(lagging.highest_acknowledged, CacheSeq::new(3));
    assert_eq!(lagging.applied, None);
    assert_eq!(lagging.visible_state.as_ref(), Some(&final_state()));
    assert_eq!(lagging.backend_state.as_ref(), Some(&State::new()));

    let (invoked_tx, invoked_rx) = mpsc::channel();
    let wait = std::thread::scope(|scope| {
        let cache = &cache;
        let clock = &clock;
        let waiter = scope.spawn(move || {
            let invocation = clock.next();
            invoked_tx.send(()).expect("signal barrier invocation");
            let target = cache.wait_applied().expect("wait for recorded prefix");
            let response = clock.next();
            WaitAppliedObservation {
                interval: Interval::completed(invocation, response),
                outcome: WaitAppliedOutcome::Ok {
                    target: Some(CacheSeq::new(target).expect("nonzero barrier target")),
                },
            }
        });
        invoked_rx.recv().expect("barrier was invoked");
        backend.release();
        waiter.join().expect("barrier worker panicked")
    });

    let applied = observe_frontier(&cache, &backend, &clock);
    assert_eq!(applied.highest_acknowledged, CacheSeq::new(3));
    assert_eq!(applied.applied, CacheSeq::new(3));
    assert_eq!(applied.visible_state.as_ref(), Some(&final_state()));
    assert_eq!(applied.backend_state.as_ref(), Some(&final_state()));
    assert_eq!(cache.close().expect("close recorded cache"), 3);

    let recovered = Cache::from_backend(Arc::clone(&backend), CacheOptions::default())
        .expect("reopen recorded cache");
    let recovered_frontier = observe_frontier(&recovered, &backend, &clock);
    assert_eq!(recovered_frontier.applied, CacheSeq::new(3));
    assert_eq!(
        recovered_frontier.visible_state.as_ref(),
        Some(&final_state())
    );
    assert_eq!(
        recovered_frontier.backend_state.as_ref(),
        Some(&final_state())
    );
    assert_eq!(recovered.close().expect("close recovered cache"), 3);

    let mut transaction_history = History::new(State::new());
    transaction_history.set_observed_final_state(final_state());
    for transaction in transactions {
        transaction_history.push(transaction);
    }
    let mut history = ApplicationHistory::new(transaction_history);
    history.commits = commits;
    history.backend_attempts = backend.attempts();
    history.frontiers = vec![lagging, applied, recovered_frontier];
    history.waits.push(wait);
    history
}

#[test]
fn real_cache_history_connects_visibility_acknowledgement_and_ordered_application() {
    let history = recorded_application_history();
    let witness = check_application(
        &history,
        Semantics::StrictSerializability,
        CheckOptions::default(),
    )
    .unwrap_or_else(|error| panic!("real application history failed:\n{error}"));
    assert_eq!(witness.cache_order.serialization, vec![1, 2, 3]);
    assert_eq!(witness.successful_backend_prefix, 3);

    // Exercise the exact same checker path with a deliberate decoded-batch
    // divergence. The explicit marker prevents a renamed key, stale binary, or
    // missed injection from turning this negative tripwire falsely green.
    let mut divergent = history.clone();
    let mut injection_consumed = false;
    for attempt in &mut divergent.backend_attempts {
        if attempt.seq == CacheSeq::new(2).unwrap() {
            for mutation in &mut attempt.mutations {
                if let ModelMutation::Put { key, value, .. } = mutation {
                    if key.as_slice() == CHAIN {
                        *value = b"injected-divergence".to_vec();
                        injection_consumed = true;
                    }
                }
            }
        }
    }
    assert!(
        injection_consumed,
        "divergence injection did not match its live batch"
    );
    let failure = check_application(
        &divergent,
        Semantics::StrictSerializability,
        CheckOptions::default(),
    )
    .expect_err("the application oracle accepted an injected batch divergence");
    assert_eq!(failure.kind, ApplicationCheckFailureKind::MutationMismatch);
    assert!(failure.replay.contains("injected-divergence") || failure.detail.contains("expected"));
}

#[test]
fn real_concurrent_cache_history_waits_for_dense_acknowledgement_prefix() {
    let history = recorded_concurrent_prefix_acknowledgement();
    let witness = check_application(
        &history,
        Semantics::StrictSerializability,
        CheckOptions::default(),
    )
    .unwrap_or_else(|error| panic!("concurrent application history failed:\n{error}"));
    assert_eq!(witness.cache_order.serialization, vec![1, 2]);
    assert_eq!(witness.successful_backend_prefix, 2);
}
