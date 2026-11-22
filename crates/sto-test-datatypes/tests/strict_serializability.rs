use std::{
    collections::BTreeMap,
    num::NonZeroUsize,
    panic::{catch_unwind, AssertUnwindSafe},
    sync::{
        atomic::{AtomicU64, Ordering},
        mpsc::{self, Receiver, RecvTimeoutError, Sender},
        Arc,
    },
    thread,
    time::{Duration, Instant},
};

use sto_core::{AbortReason, AccessError, CommitInfo, CommitOutcome, Runtime, RuntimeConfig};
use sto_test_datatypes::TxnHashMap;

const WORKER_COUNT: usize = 3;
const TRANSACTIONS_PER_WORKER: usize = 2;
const KEY_COUNT: usize = 5;
const SEED_COUNT: u64 = 128;
const BUCKET_COUNT: usize = 16;
const HISTORY_TIMEOUT: Duration = Duration::from_secs(10);
const WORKER_SHUTDOWN_TIMEOUT: Duration = Duration::from_secs(10);

type Model = [Option<u64>; KEY_COUNT];

const INITIAL_MODEL: Model = [Some(10), Some(20), Some(30), Some(40), None];

#[derive(Clone, Copy, Debug)]
enum PlannedOperation {
    Read { key: usize },
    Write { key: usize, value: u64 },
}

#[derive(Clone, Copy, Debug)]
enum RecordedOperation {
    Read {
        key: usize,
        observed: Option<u64>,
    },
    Write {
        key: usize,
        value: u64,
        previous: Option<u64>,
    },
}

#[derive(Debug)]
struct TransactionRecord {
    id: usize,
    worker: usize,
    transaction: usize,
    invocation: u64,
    response: u64,
    operations: Vec<RecordedOperation>,
    access_conflict: Option<AccessError>,
    outcome: CommitOutcome,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum RoundPhase {
    Start,
    Invoked,
    InitialReadsComplete,
}

impl RoundPhase {
    const ALL: [Self; 3] = [Self::Start, Self::Invoked, Self::InitialReadsComplete];
}

#[derive(Debug)]
enum CoordinatorEvent {
    Arrived {
        worker: usize,
        transaction: usize,
        phase: RoundPhase,
    },
    Failed {
        worker: usize,
        detail: String,
    },
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct PhaseRelease {
    transaction: usize,
    phase: RoundPhase,
}

struct WorkerRun {
    seed: u64,
    worker_index: usize,
    runtime: Arc<Runtime>,
    map: TxnHashMap<u64, u64>,
    keys: [u64; KEY_COUNT],
    clock: Arc<AtomicU64>,
    events: Sender<CoordinatorEvent>,
    releases: Receiver<PhaseRelease>,
    deadline: Instant,
}

fn mix(mut value: u64) -> u64 {
    value = value.wrapping_add(0x9e37_79b9_7f4a_7c15);
    value = (value ^ (value >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
    value = (value ^ (value >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
    value ^ (value >> 31)
}

fn plan(seed: u64, worker: usize, transaction: usize) -> [PlannedOperation; 5] {
    let entropy = mix(seed ^ ((worker as u64) << 16) ^ ((transaction as u64) << 32));
    let base = entropy as usize % KEY_COUNT;
    let stride = if entropy & 1 == 0 { 1 } else { KEY_COUNT - 1 };
    let read_first = (base + worker * stride) % KEY_COUNT;
    let read_second = (read_first + stride) % KEY_COUNT;
    let write_first = read_first;
    let write_second = (read_second + stride) % KEY_COUNT;
    let value_base =
        (1_u64 << 63) | (seed << 24) | ((worker as u64) << 16) | ((transaction as u64) << 8);

    [
        PlannedOperation::Read { key: read_first },
        PlannedOperation::Read { key: read_second },
        PlannedOperation::Write {
            key: write_first,
            value: value_base | 1,
        },
        PlannedOperation::Write {
            key: write_second,
            value: value_base | 2,
        },
        PlannedOperation::Read { key: write_first },
    ]
}

fn yield_for(seed: u64, worker: usize, transaction: usize, operation: usize) {
    let entropy = mix(seed
        ^ ((worker as u64) << 12)
        ^ ((transaction as u64) << 20)
        ^ ((operation as u64) << 28));
    for _ in 0..entropy & 3 {
        thread::yield_now();
    }
}

fn execute_operation(
    map: &TxnHashMap<u64, u64>,
    keys: &[u64; KEY_COUNT],
    transaction: &mut sto_core::Transaction<'_, sto_core::Active>,
    operation: PlannedOperation,
) -> Result<RecordedOperation, AccessError> {
    match operation {
        PlannedOperation::Read { key } => {
            let observed = map.get(transaction, &keys[key])?;
            Ok(RecordedOperation::Read { key, observed })
        }
        PlannedOperation::Write { key, value } => {
            let previous = map.insert(transaction, keys[key], value)?;
            Ok(RecordedOperation::Write {
                key,
                value,
                previous,
            })
        }
    }
}

fn receive_until<T>(
    receiver: &Receiver<T>,
    deadline: Instant,
    description: &str,
) -> Result<T, String> {
    let Some(remaining) = deadline.checked_duration_since(Instant::now()) else {
        return Err(format!("timed out waiting for {description}"));
    };
    receiver
        .recv_timeout(remaining)
        .map_err(|error| match error {
            RecvTimeoutError::Timeout => format!("timed out waiting for {description}"),
            RecvTimeoutError::Disconnected => {
                format!("channel disconnected while waiting for {description}")
            }
        })
}

fn arrive_and_wait(
    worker: usize,
    transaction: usize,
    phase: RoundPhase,
    events: &Sender<CoordinatorEvent>,
    releases: &Receiver<PhaseRelease>,
    deadline: Instant,
) -> Result<(), String> {
    events
        .send(CoordinatorEvent::Arrived {
            worker,
            transaction,
            phase,
        })
        .map_err(|_| {
            format!("worker {worker} could not report transaction {transaction} phase {phase:?}")
        })?;
    let release = receive_until(
        releases,
        deadline,
        &format!("transaction {transaction} phase {phase:?} release for worker {worker}"),
    )?;
    if release != (PhaseRelease { transaction, phase }) {
        return Err(format!(
            "worker {worker} received out-of-order release {release:?} while waiting for \
             transaction {transaction} phase {phase:?}"
        ));
    }
    Ok(())
}

fn run_worker(run: WorkerRun) -> Result<Vec<TransactionRecord>, String> {
    let WorkerRun {
        seed,
        worker_index,
        runtime,
        map,
        keys,
        clock,
        events,
        releases,
        deadline,
    } = run;
    let mut worker = runtime.attach().unwrap();
    let mut records = Vec::with_capacity(TRANSACTIONS_PER_WORKER);

    for transaction_index in 0..TRANSACTIONS_PER_WORKER {
        let planned = plan(seed, worker_index, transaction_index);

        // Every worker finishes the previous transaction before the next round.
        arrive_and_wait(
            worker_index,
            transaction_index,
            RoundPhase::Start,
            &events,
            &releases,
            deadline,
        )?;
        let invocation = clock.fetch_add(1, Ordering::SeqCst) + 1;
        let mut transaction = worker.begin().unwrap();

        // Record every invocation before any transaction starts its operations.
        arrive_and_wait(
            worker_index,
            transaction_index,
            RoundPhase::Invoked,
            &events,
            &releases,
            deadline,
        )?;
        let mut operations = Vec::with_capacity(planned.len());
        let mut access_conflict = None;

        for (operation_index, operation) in planned[..2].iter().copied().enumerate() {
            yield_for(seed, worker_index, transaction_index, operation_index);
            match execute_operation(&map, &keys, &mut transaction, operation) {
                Ok(operation) => operations.push(operation),
                Err(error @ AccessError::Conflict(_)) => {
                    access_conflict = Some(error);
                    break;
                }
                Err(error) => panic!(
                    "seed {seed}, worker {worker_index}, transaction {transaction_index}: \
                     unexpected access failure: {error:?}"
                ),
            }
        }

        // All initial reads complete before any worker begins its writes.
        arrive_and_wait(
            worker_index,
            transaction_index,
            RoundPhase::InitialReadsComplete,
            &events,
            &releases,
            deadline,
        )?;
        if access_conflict.is_none() {
            for (operation_index, operation) in planned[2..].iter().copied().enumerate() {
                yield_for(seed, worker_index, transaction_index, operation_index + 2);
                match execute_operation(&map, &keys, &mut transaction, operation) {
                    Ok(operation) => operations.push(operation),
                    Err(error @ AccessError::Conflict(_)) => {
                        access_conflict = Some(error);
                        break;
                    }
                    Err(error) => panic!(
                        "seed {seed}, worker {worker_index}, transaction {transaction_index}: \
                         unexpected access failure: {error:?}"
                    ),
                }
            }
        }

        yield_for(seed, worker_index, transaction_index, planned.len());
        let outcome = transaction.commit().unwrap_or_else(|error| {
            panic!(
                "seed {seed}, worker {worker_index}, transaction {transaction_index}: \
                 unexpected commit failure: {error:?}"
            )
        });
        let response = clock.fetch_add(1, Ordering::SeqCst) + 1;

        if access_conflict.is_some() {
            assert!(
                matches!(outcome, CommitOutcome::Aborted(_)),
                "a transaction with an access conflict committed"
            );
        }
        if matches!(outcome, CommitOutcome::Committed(_)) {
            assert_eq!(
                operations.len(),
                planned.len(),
                "a committed transaction did not complete its operation list"
            );
        }

        records.push(TransactionRecord {
            id: worker_index * TRANSACTIONS_PER_WORKER + transaction_index,
            worker: worker_index,
            transaction: transaction_index,
            invocation,
            response,
            operations,
            access_conflict,
            outcome,
        });
    }

    Ok(records)
}

fn coordinate_rounds(
    events: &Receiver<CoordinatorEvent>,
    releases: &[Sender<PhaseRelease>],
    deadline: Instant,
) -> Result<(), String> {
    for transaction in 0..TRANSACTIONS_PER_WORKER {
        for phase in RoundPhase::ALL {
            let mut arrived = [false; WORKER_COUNT];
            for _ in 0..WORKER_COUNT {
                match receive_until(
                    events,
                    deadline,
                    &format!("transaction {transaction} phase {phase:?} arrivals"),
                )? {
                    CoordinatorEvent::Arrived {
                        worker,
                        transaction: arrived_transaction,
                        phase: arrived_phase,
                    } => {
                        if worker >= WORKER_COUNT {
                            return Err(format!("arrival named unknown worker {worker}"));
                        }
                        if arrived_transaction != transaction || arrived_phase != phase {
                            return Err(format!(
                                "worker {worker} arrived for transaction {arrived_transaction} \
                                 phase {arrived_phase:?}; expected transaction {transaction} \
                                 phase {phase:?}"
                            ));
                        }
                        if std::mem::replace(&mut arrived[worker], true) {
                            return Err(format!(
                                "worker {worker} reported transaction {transaction} phase \
                                 {phase:?} twice"
                            ));
                        }
                    }
                    CoordinatorEvent::Failed { worker, detail } => {
                        return Err(format!("worker {worker} failed: {detail}"));
                    }
                }
            }

            let release = PhaseRelease { transaction, phase };
            for (worker, sender) in releases.iter().enumerate() {
                sender.send(release).map_err(|_| {
                    format!(
                        "could not release worker {worker} for transaction {transaction} \
                         phase {phase:?}"
                    )
                })?;
            }
        }
    }
    Ok(())
}

fn panic_detail(payload: Box<dyn std::any::Any + Send>) -> String {
    if let Some(message) = payload.downcast_ref::<&str>() {
        (*message).to_owned()
    } else if let Some(message) = payload.downcast_ref::<String>() {
        message.clone()
    } else {
        String::from("non-string panic payload")
    }
}

fn collect_worker_results(
    receiver: &Receiver<(usize, Result<Vec<TransactionRecord>, String>)>,
    deadline: Instant,
) -> Result<Vec<TransactionRecord>, String> {
    let mut records_by_worker: Vec<Option<Vec<TransactionRecord>>> =
        std::iter::repeat_with(|| None).take(WORKER_COUNT).collect();
    for _ in 0..WORKER_COUNT {
        let (worker, result) = receive_until(receiver, deadline, "worker result")?;
        if worker >= WORKER_COUNT {
            return Err(format!("result named unknown worker {worker}"));
        }
        if records_by_worker[worker].is_some() {
            return Err(format!("worker {worker} returned two results"));
        }
        records_by_worker[worker] = Some(result?);
    }

    let mut records = Vec::with_capacity(WORKER_COUNT * TRANSACTIONS_PER_WORKER);
    for (worker, worker_records) in records_by_worker.into_iter().enumerate() {
        let Some(worker_records) = worker_records else {
            return Err(format!("worker {worker} returned no result"));
        };
        records.extend(worker_records);
    }
    Ok(records)
}

fn finish_workers(handles: Vec<thread::JoinHandle<()>>, deadline: Instant) -> Result<(), String> {
    while handles.iter().any(|handle| !handle.is_finished()) {
        if Instant::now() >= deadline {
            return Err(String::from("timed out waiting for worker threads to stop"));
        }
        thread::yield_now();
    }
    for handle in handles {
        handle
            .join()
            .map_err(|payload| format!("worker wrapper panicked: {}", panic_detail(payload)))?;
    }
    Ok(())
}

fn collect_concurrent_history(
    seed: u64,
    runtime: &Arc<Runtime>,
    map: &TxnHashMap<u64, u64>,
    keys: [u64; KEY_COUNT],
) -> Result<Vec<TransactionRecord>, String> {
    let deadline = Instant::now()
        .checked_add(HISTORY_TIMEOUT)
        .expect("the bounded test deadline fits in Instant");
    let clock = Arc::new(AtomicU64::new(0));
    let (event_sender, event_receiver) = mpsc::channel();
    let (result_sender, result_receiver) = mpsc::channel();
    let mut release_senders = Vec::with_capacity(WORKER_COUNT);
    let mut handles = Vec::with_capacity(WORKER_COUNT);

    for worker_index in 0..WORKER_COUNT {
        let (release_sender, release_receiver) = mpsc::channel();
        release_senders.push(release_sender);
        let runtime = Arc::clone(runtime);
        let map = map.clone();
        let clock = Arc::clone(&clock);
        let worker_events = event_sender.clone();
        let failure_events = event_sender.clone();
        let worker_results = result_sender.clone();
        handles.push(thread::spawn(move || {
            let result = catch_unwind(AssertUnwindSafe(|| {
                run_worker(WorkerRun {
                    seed,
                    worker_index,
                    runtime,
                    map,
                    keys,
                    clock,
                    events: worker_events,
                    releases: release_receiver,
                    deadline,
                })
            }));
            let result = match result {
                Ok(result) => result,
                Err(payload) => Err(format!("panicked: {}", panic_detail(payload))),
            };
            if let Err(detail) = &result {
                let _ = failure_events.send(CoordinatorEvent::Failed {
                    worker: worker_index,
                    detail: detail.clone(),
                });
            }
            let _ = worker_results.send((worker_index, result));
        }));
    }
    drop(event_sender);
    drop(result_sender);

    let history = coordinate_rounds(&event_receiver, &release_senders, deadline)
        .and_then(|()| collect_worker_results(&result_receiver, deadline));
    drop(release_senders);
    let shutdown_deadline = Instant::now()
        .checked_add(WORKER_SHUTDOWN_TIMEOUT)
        .expect("the bounded worker shutdown deadline fits in Instant");
    let finish = finish_workers(handles, shutdown_deadline);

    match (history, finish) {
        (Ok(records), Ok(())) => Ok(records),
        (Err(history_error), Ok(())) => Err(history_error),
        (Ok(_), Err(finish_error)) => Err(finish_error),
        (Err(history_error), Err(finish_error)) => Err(format!(
            "{history_error}; worker shutdown also failed: {finish_error}"
        )),
    }
}

fn replay_transaction(record: &TransactionRecord, model: &mut Model) -> bool {
    for operation in &record.operations {
        match *operation {
            RecordedOperation::Read { key, observed } => {
                if model[key] != observed {
                    return false;
                }
            }
            RecordedOperation::Write {
                key,
                value,
                previous,
            } => {
                if model[key] != previous {
                    return false;
                }
                model[key] = Some(value);
            }
        }
    }
    true
}

fn find_serial_order(
    records: &[TransactionRecord],
    initial: Model,
    final_state: Model,
) -> Option<Vec<usize>> {
    let committed: Vec<_> = records
        .iter()
        .filter(|record| matches!(record.outcome, CommitOutcome::Committed(_)))
        .collect();
    let mut predecessors = vec![0_u64; committed.len()];

    for (before_index, before) in committed.iter().enumerate() {
        for (after_index, after) in committed.iter().enumerate() {
            if before.response < after.invocation {
                predecessors[after_index] |= 1_u64 << before_index;
            }
        }
    }

    fn search(
        committed: &[&TransactionRecord],
        predecessors: &[u64],
        final_state: Model,
        used: u64,
        model: Model,
        order: &mut Vec<usize>,
    ) -> bool {
        if order.len() == committed.len() {
            return model == final_state;
        }

        for candidate in 0..committed.len() {
            let bit = 1_u64 << candidate;
            if used & bit != 0 || predecessors[candidate] & !used != 0 {
                continue;
            }

            let mut next_model = model;
            if !replay_transaction(committed[candidate], &mut next_model) {
                continue;
            }
            order.push(committed[candidate].id);
            if search(
                committed,
                predecessors,
                final_state,
                used | bit,
                next_model,
                order,
            ) {
                return true;
            }
            order.pop();
        }
        false
    }

    let mut order = Vec::with_capacity(committed.len());
    search(
        &committed,
        &predecessors,
        final_state,
        0,
        initial,
        &mut order,
    )
    .then_some(order)
}

fn distinct_bucket_keys(map: &TxnHashMap<u64, u64>) -> [u64; KEY_COUNT] {
    let mut keys = [0_u64; KEY_COUNT];
    let mut buckets = [usize::MAX; KEY_COUNT];
    let mut found = 0;

    for candidate in 0..100_000_u64 {
        let bucket = map.bucket_index(&candidate);
        if buckets[..found].contains(&bucket) {
            continue;
        }
        keys[found] = candidate;
        buckets[found] = bucket;
        found += 1;
        if found == KEY_COUNT {
            break;
        }
    }

    assert_eq!(found, KEY_COUNT, "could not find enough distinct buckets");
    for first in 0..KEY_COUNT {
        for second in first + 1..KEY_COUNT {
            assert_ne!(
                map.bucket_index(&keys[first]),
                map.bucket_index(&keys[second])
            );
        }
    }
    keys
}

fn initialize_map(runtime: &Arc<Runtime>, map: &TxnHashMap<u64, u64>, keys: &[u64; KEY_COUNT]) {
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    for (key, value) in INITIAL_MODEL.iter().copied().enumerate() {
        if let Some(value) = value {
            assert_eq!(
                map.insert(&mut transaction, keys[key], value).unwrap(),
                None
            );
        }
    }
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
}

fn observe_final_state(
    runtime: &Arc<Runtime>,
    map: &TxnHashMap<u64, u64>,
    keys: &[u64; KEY_COUNT],
) -> (Model, BTreeMap<u64, u64>) {
    let mut worker = runtime.attach().unwrap();
    let mut transaction = worker.begin().unwrap();
    let complete_map = map.to_btree_map(&mut transaction).unwrap();
    let mut state = [None; KEY_COUNT];
    for (key, value) in state.iter_mut().enumerate() {
        *value = complete_map.get(&keys[key]).copied();
    }
    assert!(matches!(
        transaction.commit().unwrap(),
        CommitOutcome::Committed(_)
    ));
    (state, complete_map)
}

fn materialize_model(model: Model, keys: &[u64; KEY_COUNT]) -> BTreeMap<u64, u64> {
    keys.iter()
        .copied()
        .zip(model)
        .filter_map(|(key, value)| value.map(|value| (key, value)))
        .collect()
}

fn committed_record(
    id: usize,
    invocation: u64,
    response: u64,
    operations: Vec<RecordedOperation>,
) -> TransactionRecord {
    TransactionRecord {
        id,
        worker: id,
        transaction: 0,
        invocation,
        response,
        operations,
        access_conflict: None,
        outcome: CommitOutcome::Committed(CommitInfo::new(None)),
    }
}

#[test]
fn checker_accepts_a_valid_history_and_ignores_aborted_transactions() {
    let first = committed_record(
        0,
        1,
        2,
        vec![
            RecordedOperation::Read {
                key: 0,
                observed: Some(10),
            },
            RecordedOperation::Write {
                key: 0,
                value: 111,
                previous: Some(10),
            },
        ],
    );
    let second = committed_record(
        1,
        3,
        4,
        vec![
            RecordedOperation::Read {
                key: 0,
                observed: Some(111),
            },
            RecordedOperation::Write {
                key: 1,
                value: 222,
                previous: Some(20),
            },
        ],
    );
    let aborted = TransactionRecord {
        id: 2,
        worker: 2,
        transaction: 0,
        invocation: 1,
        response: 4,
        operations: vec![RecordedOperation::Read {
            key: 0,
            observed: Some(u64::MAX),
        }],
        access_conflict: None,
        outcome: CommitOutcome::Aborted(AbortReason::Explicit),
    };
    let mut final_state = INITIAL_MODEL;
    final_state[0] = Some(111);
    final_state[1] = Some(222);

    assert_eq!(
        find_serial_order(&[first, second, aborted], INITIAL_MODEL, final_state),
        Some(vec![0, 1])
    );
}

#[test]
fn checker_rejects_a_write_skew_cycle() {
    let first = committed_record(
        0,
        1,
        4,
        vec![
            RecordedOperation::Read {
                key: 1,
                observed: Some(20),
            },
            RecordedOperation::Write {
                key: 0,
                value: 111,
                previous: Some(10),
            },
        ],
    );
    let second = committed_record(
        1,
        2,
        3,
        vec![
            RecordedOperation::Read {
                key: 0,
                observed: Some(10),
            },
            RecordedOperation::Write {
                key: 1,
                value: 222,
                previous: Some(20),
            },
        ],
    );
    let mut final_state = INITIAL_MODEL;
    final_state[0] = Some(111);
    final_state[1] = Some(222);

    assert_eq!(
        find_serial_order(&[first, second], INITIAL_MODEL, final_state),
        None
    );
}

#[test]
fn checker_enforces_real_time_order() {
    let first = committed_record(
        0,
        1,
        2,
        vec![RecordedOperation::Write {
            key: 0,
            value: 111,
            previous: Some(10),
        }],
    );
    let second = committed_record(
        1,
        3,
        4,
        vec![RecordedOperation::Read {
            key: 0,
            observed: Some(10),
        }],
    );
    let mut final_state = INITIAL_MODEL;
    final_state[0] = Some(111);

    assert_eq!(
        find_serial_order(&[first, second], INITIAL_MODEL, final_state),
        None
    );
}

#[test]
fn bounded_concurrent_histories_are_strictly_serializable() {
    let mut total_commits = 0;
    for seed in 0..SEED_COUNT {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let map = TxnHashMap::with_bucket_count(
            &runtime,
            NonZeroUsize::new(BUCKET_COUNT).expect("the test bucket count is nonzero"),
        )
        .unwrap();
        let keys = distinct_bucket_keys(&map);
        initialize_map(&runtime, &map, &keys);

        let mut records = collect_concurrent_history(seed, &runtime, &map, keys)
            .unwrap_or_else(|error| panic!("seed {seed} coordination failed: {error}"));
        records.sort_by_key(|record| record.id);
        assert_eq!(records.len(), WORKER_COUNT * TRANSACTIONS_PER_WORKER);
        for record in &records {
            assert_eq!(
                record.id,
                record.worker * TRANSACTIONS_PER_WORKER + record.transaction
            );
            assert!(record.invocation < record.response);
            if record.access_conflict.is_some() {
                assert!(matches!(record.outcome, CommitOutcome::Aborted(_)));
            }
        }
        total_commits += records
            .iter()
            .filter(|record| matches!(record.outcome, CommitOutcome::Committed(_)))
            .count();

        let (final_state, complete_map) = observe_final_state(&runtime, &map, &keys);
        assert_eq!(
            complete_map,
            materialize_model(final_state, &keys),
            "seed {seed} left an unexpected physical key in the map"
        );
        let serial_order = find_serial_order(&records, INITIAL_MODEL, final_state);
        assert!(
            serial_order.is_some(),
            "seed {seed} has no strict serial order\n\
             initial: {INITIAL_MODEL:?}\n\
             final: {final_state:?}\n\
             history: {records:#?}"
        );
    }
    assert!(
        total_commits >= SEED_COUNT as usize,
        "only {total_commits} of {} concurrent transactions committed",
        SEED_COUNT as usize * WORKER_COUNT * TRANSACTIONS_PER_WORKER
    );
}
