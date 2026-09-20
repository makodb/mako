use super::*;

use std::{
    any::Any,
    panic::{catch_unwind, AssertUnwindSafe},
    sync::{
        mpsc::{self, Receiver, RecvTimeoutError, Sender},
        Arc,
    },
    thread,
    time::{Duration, Instant},
};

use mako_history::{
    check_strict_serializability, state_insert, CheckOptions, History, Interval, LogicalClock,
    Observation, Operation, Row, ScanDirection as HistoryScanDirection, State, TerminalCall,
    TerminalOutcome, TimedOperation, Transaction as HistoryTransaction,
};
use sto_core::{AbortReason, CommitOutcome, RuntimeConfig, WorkerContext};
use sto_test_datatypes::TxnVec;

const TABLE_ID: u64 = 1;
const WORKER_COUNT: usize = 3;
const TRANSACTIONS_PER_WORKER: usize = 2;
const SEED_COUNT: u64 = 128;
const KEY_COUNT: usize = 8;
const LANE_WIDTH: usize = KEY_COUNT / 2;
const MIN_OVERLAPPING_PAIRS: usize = SEED_COUNT as usize;
const MIN_OVERLAPPING_SEEDS: usize = (SEED_COUNT * 3 / 4) as usize;
const MAX_RETAINED_RECORDS: u64 = 32;
const FULL_SCAN_LIMIT: usize = MAX_RETAINED_RECORDS as usize + 1;
const ROUND_TIMEOUT: Duration = Duration::from_secs(20);
const SHUTDOWN_TIMEOUT: Duration = Duration::from_secs(10);
const DEPENDENCY_READER_ID: u32 = 1;
const DEPENDENCY_WRITER_ID: u32 = 2;
const DEPENDENCY_KEY: &[u8] = b"dependency\0\xff";
const DEPENDENCY_OLD_VALUE: &[u8] = b"old\0value";
const DEPENDENCY_NEW_VALUE: &[u8] = b"new\xffvalue";

const KEYS: [&[u8]; KEY_COUNT] = [
    b"", b"\0", b"\0a", b"a\0", b"a\xff", b"b", b"\x80\0", b"\xff",
];

const INITIAL_ROWS: [(&[u8], &[u8]); 5] = [
    (KEYS[0], b"empty-key\0"),
    (KEYS[1], b"zero\0"),
    (KEYS[3], b""),
    (KEYS[5], b"base\xff"),
    (KEYS[7], b"tail\0\xff"),
];

const SCAN_RANGES: [(&[u8], &[u8]); 2] = [(b"", b"a\x80"), (b"a\x80", b"\xff\xff")];

#[derive(Clone, Copy, Debug)]
enum MemoryMode {
    RegistryId,
    DirectToken,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum RoundPhase {
    Start,
    Begun,
    InitialReadsComplete,
}

impl RoundPhase {
    const ALL: [Self; 3] = [Self::Start, Self::Begun, Self::InitialReadsComplete];
}

#[derive(Debug)]
enum WorkerCommand {
    Release {
        seed: u64,
        transaction: usize,
        phase: RoundPhase,
    },
}

#[derive(Debug)]
enum WorkerEvent {
    Arrived {
        seed: u64,
        worker: usize,
        transaction: usize,
        phase: RoundPhase,
    },
    Finished {
        seed: u64,
        worker: usize,
        transactions: Vec<HistoryTransaction>,
    },
    Failed {
        worker: usize,
        detail: String,
    },
}

#[derive(Debug)]
enum DependencyEvent {
    Begun,
    Finished(HistoryTransaction),
    Failed(String),
}

fn table_config() -> TableConfig {
    TableConfig::new()
        .with_max_retained_records(MAX_RETAINED_RECORDS)
        .with_max_retained_key_bytes(1_024)
        .with_max_consumed_record_ids(MAX_RETAINED_RECORDS)
        .with_scan_chunk_records(2)
        .with_scan_initial_key_arena_bytes(1)
        .with_scan_max_key_arena_bytes(1_024)
        .with_max_scan_chunks(MAX_RETAINED_RECORDS as usize)
        .with_max_scan_physical_records(MAX_RETAINED_RECORDS as usize)
}

fn make_table(mode: MemoryMode, runtime: &Arc<Runtime>) -> Table {
    match mode {
        MemoryMode::RegistryId => Table::new_memory(runtime, table_config()),
        MemoryMode::DirectToken => Table::new_memory_direct(runtime, table_config()),
    }
}

fn mix(mut value: u64) -> u64 {
    value = value.wrapping_add(0x9e37_79b9_7f4a_7c15);
    value = (value ^ (value >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
    value = (value ^ (value >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
    value ^ (value >> 31)
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

fn unique_value(seed: u64, worker: usize, transaction: usize, operation: u8) -> Vec<u8> {
    vec![
        0xff,
        seed as u8,
        (seed >> 8) as u8,
        0,
        worker as u8,
        transaction as u8,
        operation,
        0x80,
    ]
}

fn plan(seed: u64, worker: usize, transaction: usize) -> Vec<Operation> {
    let entropy = mix(seed ^ ((worker as u64) << 16) ^ ((transaction as u64) << 32));
    let lane = match worker {
        0 => 0,
        1 => 1,
        _ => (entropy >> 16) as usize % SCAN_RANGES.len(),
    };
    let lane_base = lane * LANE_WIDTH;
    let local_first = entropy as usize % LANE_WIDTH;
    let stride = ((entropy >> 8) as usize % (LANE_WIDTH - 1)) + 1;
    let first = lane_base + local_first;
    let second = lane_base + (local_first + stride) % LANE_WIDTH;
    let third = lane_base + (local_first + 2 * stride) % LANE_WIDTH;
    let range = SCAN_RANGES[lane];

    vec![
        Operation::get(TABLE_ID, KEYS[first]),
        Operation::scan(
            TABLE_ID,
            range.0,
            Some(range.1.to_vec()),
            HistoryScanDirection::Forward,
        ),
        Operation::put(
            TABLE_ID,
            KEYS[first],
            unique_value(seed, worker, transaction, 2),
        ),
        Operation::insert(
            TABLE_ID,
            KEYS[second],
            unique_value(seed, worker, transaction, 3),
        ),
        Operation::remove(TABLE_ID, KEYS[third]),
        // This upsert exercises tombstone resurrection in the same transaction.
        Operation::put(
            TABLE_ID,
            KEYS[third],
            unique_value(seed, worker, transaction, 5),
        ),
        Operation::scan(
            TABLE_ID,
            range.0,
            Some(range.1.to_vec()),
            HistoryScanDirection::Reverse,
        ),
    ]
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

fn execute_operation(
    table: &Table,
    transaction: &mut Transaction<'_, Active>,
    operation: &Operation,
) -> Result<Observation, AccessError> {
    match operation {
        Operation::Get { table: id, key } => {
            assert_eq!(*id, TABLE_ID);
            table
                .get_inner(transaction, None, key)
                .map(|value| Observation::Get(value.map(|value| value.to_vec())))
        }
        Operation::Put {
            table: id,
            key,
            value,
        } => {
            assert_eq!(*id, TABLE_ID);
            table
                .put_inner(transaction, None, key, value.as_slice())
                .map(|previous| Observation::PutWithPrevious {
                    previous: previous.map(|value| value.to_vec()),
                })
        }
        Operation::Insert {
            table: id,
            key,
            value,
        } => {
            assert_eq!(*id, TABLE_ID);
            table
                .insert_inner(transaction, None, key, value.as_slice())
                .map(|outcome| Observation::InsertWithPrevious {
                    previous: match outcome {
                        InsertOutcome::Inserted => None,
                        InsertOutcome::AlreadyPresent(value) => Some(value.to_vec()),
                    },
                })
        }
        Operation::Remove { table: id, key } => {
            assert_eq!(*id, TABLE_ID);
            table.remove_inner(transaction, None, key).map(|previous| {
                Observation::RemoveWithPrevious {
                    previous: previous.map(|value| value.to_vec()),
                }
            })
        }
        Operation::Scan {
            table: id,
            lower,
            upper,
            direction,
        } => {
            assert_eq!(*id, TABLE_ID);
            let table_direction = match direction {
                HistoryScanDirection::Forward => ScanDirection::Forward,
                HistoryScanDirection::Reverse => ScanDirection::Reverse,
            };
            let mut request = ScanRequest::new(table_direction, FULL_SCAN_LIMIT)
                .with_lower(ScanBound::Included(lower));
            if let Some(upper) = upper {
                request = request.with_upper(ScanBound::Excluded(upper));
            }
            table.scan_inner(transaction, None, request).map(|rows| {
                Observation::Scan(
                    rows.into_iter()
                        .map(|row| Row::new(row.key().to_vec(), row.value().to_vec()))
                        .collect(),
                )
            })
        }
    }
}

fn arrive_and_wait(
    seed: u64,
    worker: usize,
    transaction: usize,
    phase: RoundPhase,
    events: &Sender<WorkerEvent>,
    commands: &Receiver<WorkerCommand>,
    deadline: Instant,
) -> Result<(), String> {
    events
        .send(WorkerEvent::Arrived {
            seed,
            worker,
            transaction,
            phase,
        })
        .map_err(|_| {
            format!(
                "worker {worker} could not report seed {seed}, transaction {transaction}, phase {phase:?}"
            )
        })?;
    match receive_until(
        commands,
        deadline,
        &format!(
            "seed {seed}, transaction {transaction}, phase {phase:?} release for worker {worker}"
        ),
    )? {
        WorkerCommand::Release {
            seed: released_seed,
            transaction: released_transaction,
            phase: released_phase,
        } if released_seed == seed
            && released_transaction == transaction
            && released_phase == phase => Ok(()),
        command => Err(format!(
            "worker {worker} received out-of-order command {command:?} while waiting for seed {seed}, transaction {transaction}, phase {phase:?}"
        )),
    }
}

#[allow(
    clippy::too_many_arguments,
    reason = "history intervals need the complete deterministic execution coordinates"
)]
fn record_operation(
    seed: u64,
    worker: usize,
    transaction_index: usize,
    operation_index: usize,
    table: &Table,
    transaction: &mut Transaction<'_, Active>,
    clock: &LogicalClock,
    operation: Operation,
) -> Result<(TimedOperation, bool), String> {
    yield_for(seed, worker, transaction_index, operation_index);
    let invocation = clock.next();
    let result = execute_operation(table, transaction, &operation);
    let response = clock.next();
    match result {
        Ok(observation) => Ok((
            TimedOperation::completed(invocation, response, operation, observation),
            false,
        )),
        Err(AccessError::Conflict(_)) => Ok((
            TimedOperation::completed(invocation, response, operation, Observation::Conflict),
            true,
        )),
        Err(error) => Err(format!(
            "seed {seed}, worker {worker}, transaction {transaction_index}, operation {operation_index} failed: {error:?}"
        )),
    }
}

#[allow(clippy::too_many_arguments)]
fn run_transaction(
    seed: u64,
    worker: usize,
    transaction_index: usize,
    table: &Table,
    sto_worker: &mut WorkerContext,
    clock: &LogicalClock,
    events: &Sender<WorkerEvent>,
    commands: &Receiver<WorkerCommand>,
    deadline: Instant,
) -> Result<HistoryTransaction, String> {
    arrive_and_wait(
        seed,
        worker,
        transaction_index,
        RoundPhase::Start,
        events,
        commands,
        deadline,
    )?;

    let begin_invocation = clock.next();
    let mut transaction = sto_worker.begin().map_err(|error| {
        format!(
            "seed {seed}, worker {worker}, transaction {transaction_index} begin failed: {error:?}"
        )
    })?;
    let begin_response = clock.next();
    let id = (worker * TRANSACTIONS_PER_WORKER + transaction_index + 1) as u32;
    let mut recorded =
        HistoryTransaction::new(id, Interval::completed(begin_invocation, begin_response));

    arrive_and_wait(
        seed,
        worker,
        transaction_index,
        RoundPhase::Begun,
        events,
        commands,
        deadline,
    )?;

    let planned = plan(seed, worker, transaction_index);
    let mut conflicted = false;
    for (operation_index, operation) in planned[..2].iter().cloned().enumerate() {
        let (operation, conflict) = record_operation(
            seed,
            worker,
            transaction_index,
            operation_index,
            table,
            &mut transaction,
            clock,
            operation,
        )?;
        recorded.push(operation);
        if conflict {
            conflicted = true;
            break;
        }
    }

    arrive_and_wait(
        seed,
        worker,
        transaction_index,
        RoundPhase::InitialReadsComplete,
        events,
        commands,
        deadline,
    )?;

    if !conflicted {
        for (offset, operation) in planned[2..].iter().cloned().enumerate() {
            let operation_index = offset + 2;
            let (operation, conflict) = record_operation(
                seed,
                worker,
                transaction_index,
                operation_index,
                table,
                &mut transaction,
                clock,
                operation,
            )?;
            recorded.push(operation);
            if conflict {
                conflicted = true;
                break;
            }
        }
    }

    if conflicted {
        transaction.abort();
        // A conflict observation deliberately has no terminal call. That is the
        // history schema for an operation-level OCC rejection.
        return Ok(recorded);
    }

    yield_for(seed, worker, transaction_index, planned.len());
    let invocation = clock.next();
    let outcome = transaction.commit().map_err(|error| {
        format!(
            "seed {seed}, worker {worker}, transaction {transaction_index} commit failed: {error:?}"
        )
    })?;
    let response = clock.next();
    let outcome = match outcome {
        CommitOutcome::Committed(_) => TerminalOutcome::Committed,
        CommitOutcome::Aborted(AbortReason::Conflict(_)) => TerminalOutcome::Conflict,
        CommitOutcome::Aborted(_) => TerminalOutcome::Aborted,
    };
    recorded.finish(TerminalCall::commit(invocation, response, outcome));
    Ok(recorded)
}

#[allow(
    clippy::too_many_arguments,
    reason = "each worker owns all runtime, schedule, and reporting inputs"
)]
fn run_worker(
    seed: u64,
    worker: usize,
    runtime: Arc<Runtime>,
    table: Table,
    clock: Arc<LogicalClock>,
    events: Sender<WorkerEvent>,
    commands: Receiver<WorkerCommand>,
    deadline: Instant,
) -> Result<(), String> {
    let mut sto_worker = runtime
        .attach()
        .map_err(|error| format!("worker {worker} attach failed: {error:?}"))?;
    let mut transactions = Vec::with_capacity(TRANSACTIONS_PER_WORKER);
    for transaction in 0..TRANSACTIONS_PER_WORKER {
        transactions.push(run_transaction(
            seed,
            worker,
            transaction,
            &table,
            &mut sto_worker,
            &clock,
            &events,
            &commands,
            deadline,
        )?);
    }
    events
        .send(WorkerEvent::Finished {
            seed,
            worker,
            transactions,
        })
        .map_err(|_| format!("worker {worker} could not report completion for seed {seed}"))
}

fn panic_detail(payload: Box<dyn Any + Send>) -> String {
    if let Some(message) = payload.downcast_ref::<&str>() {
        (*message).to_owned()
    } else if let Some(message) = payload.downcast_ref::<String>() {
        message.clone()
    } else {
        String::from("non-string panic payload")
    }
}

fn coordinate_round(
    seed: u64,
    commands: &[Sender<WorkerCommand>],
    events: &Receiver<WorkerEvent>,
    deadline: Instant,
) -> Result<Vec<HistoryTransaction>, String> {
    for transaction in 0..TRANSACTIONS_PER_WORKER {
        for phase in RoundPhase::ALL {
            let mut arrived = [false; WORKER_COUNT];
            for _ in 0..WORKER_COUNT {
                match receive_until(
                    events,
                    deadline,
                    &format!("seed {seed}, transaction {transaction}, phase {phase:?} arrivals"),
                )? {
                    WorkerEvent::Arrived {
                        seed: arrived_seed,
                        worker,
                        transaction: arrived_transaction,
                        phase: arrived_phase,
                    } if arrived_seed == seed
                        && arrived_transaction == transaction
                        && arrived_phase == phase
                        && worker < WORKER_COUNT =>
                    {
                        if std::mem::replace(&mut arrived[worker], true) {
                            return Err(format!(
                                "worker {worker} reported seed {seed}, transaction {transaction}, phase {phase:?} twice"
                            ));
                        }
                    }
                    WorkerEvent::Failed { worker, detail } => {
                        return Err(format!("seed {seed}: worker {worker} failed: {detail}"));
                    }
                    event => {
                        return Err(format!(
                            "seed {seed}, transaction {transaction}, phase {phase:?}: unexpected event {event:?}"
                        ));
                    }
                }
            }

            for (worker, commands) in commands.iter().enumerate() {
                commands
                    .send(WorkerCommand::Release {
                        seed,
                        transaction,
                        phase,
                    })
                    .map_err(|_| {
                        format!(
                            "could not release worker {worker} for seed {seed}, transaction {transaction}, phase {phase:?}"
                        )
                    })?;
            }
        }
    }

    let mut by_worker: Vec<Option<Vec<HistoryTransaction>>> =
        std::iter::repeat_with(|| None).take(WORKER_COUNT).collect();
    for _ in 0..WORKER_COUNT {
        match receive_until(events, deadline, &format!("seed {seed} worker results"))? {
            WorkerEvent::Finished {
                seed: finished_seed,
                worker,
                transactions,
            } if finished_seed == seed && worker < WORKER_COUNT => {
                if by_worker[worker].replace(transactions).is_some() {
                    return Err(format!(
                        "worker {worker} returned two results for seed {seed}"
                    ));
                }
            }
            WorkerEvent::Failed { worker, detail } => {
                return Err(format!("seed {seed}: worker {worker} failed: {detail}"));
            }
            event => {
                return Err(format!(
                    "seed {seed}: unexpected event while collecting results: {event:?}"
                ));
            }
        }
    }

    let mut transactions = Vec::with_capacity(WORKER_COUNT * TRANSACTIONS_PER_WORKER);
    for (worker, records) in by_worker.into_iter().enumerate() {
        let records =
            records.ok_or_else(|| format!("worker {worker} returned no result for seed {seed}"))?;
        transactions.extend(records);
    }
    Ok(transactions)
}

fn run_round(
    seed: u64,
    runtime: &Arc<Runtime>,
    table: &Table,
    clock: &Arc<LogicalClock>,
) -> Result<Vec<HistoryTransaction>, String> {
    let deadline = Instant::now()
        .checked_add(ROUND_TIMEOUT)
        .expect("round deadline fits in Instant");
    let (events, event_receiver) = mpsc::channel();
    let mut commands = Vec::with_capacity(WORKER_COUNT);
    let mut handles = Vec::with_capacity(WORKER_COUNT);

    for worker in 0..WORKER_COUNT {
        let (command_sender, command_receiver) = mpsc::channel();
        commands.push(command_sender);
        let runtime = Arc::clone(runtime);
        let table = table.clone();
        let clock = Arc::clone(clock);
        let worker_events = events.clone();
        let failure_events = events.clone();
        handles.push(Some(thread::spawn(move || {
            let result = catch_unwind(AssertUnwindSafe(|| {
                run_worker(
                    seed,
                    worker,
                    runtime,
                    table,
                    clock,
                    worker_events,
                    command_receiver,
                    deadline,
                )
            }));
            let error = match result {
                Ok(Ok(())) => return,
                Ok(Err(error)) => error,
                Err(payload) => format!("panicked: {}", panic_detail(payload)),
            };
            let _ = failure_events.send(WorkerEvent::Failed {
                worker,
                detail: error,
            });
        })));
    }
    drop(events);

    let coordinated = coordinate_round(seed, &commands, &event_receiver, deadline);
    // Dropping every release sender wakes peers if one worker failed before
    // reaching a rendezvous. Their own waits are deadline bounded as well.
    drop(commands);

    let shutdown_deadline = Instant::now()
        .checked_add(SHUTDOWN_TIMEOUT)
        .expect("shutdown deadline fits in Instant");
    while handles.iter().flatten().any(|handle| !handle.is_finished())
        && Instant::now() < shutdown_deadline
    {
        thread::yield_now();
    }

    let mut join_errors = Vec::new();
    for (worker, handle) in handles.iter_mut().enumerate() {
        let handle = handle.take().expect("each worker has one join handle");
        if !handle.is_finished() {
            join_errors.push(format!("worker {worker} did not stop before the deadline"));
            drop(handle);
            continue;
        }
        if let Err(payload) = handle.join() {
            join_errors.push(format!(
                "worker {worker} wrapper panicked: {}",
                panic_detail(payload)
            ));
        }
    }
    match (coordinated, join_errors.is_empty()) {
        (Ok(transactions), true) => Ok(transactions),
        (Err(error), true) => Err(error),
        (Ok(_), false) => Err(join_errors.join("; ")),
        (Err(error), false) => Err(format!("{error}; {}", join_errors.join("; "))),
    }
}

fn initial_state() -> State {
    let mut state = State::new();
    for (key, value) in INITIAL_ROWS {
        state_insert(&mut state, TABLE_ID, key, value);
    }
    state
}

fn expect_commit(outcome: Result<CommitOutcome, sto_core::CommitFailure>, context: &str) {
    match outcome {
        Ok(CommitOutcome::Committed(_)) => {}
        Ok(other) => panic!("{context} did not commit: {other:?}"),
        Err(error) => panic!("{context} failed: {error:?}"),
    }
}

fn prepare_key_domain(table: &Table, worker: &mut WorkerContext) -> Result<(), String> {
    let mut transaction = worker
        .begin()
        .map_err(|error| format!("initialization begin failed: {error:?}"))?;
    // Intern every generated key before scans begin. Abstractly absent keys stay
    // tombstones, while later insert/remove/resurrection remains transactional.
    for key in KEYS {
        table
            .get_inner(&mut transaction, None, key)
            .map_err(|error| format!("initial lookup for {key:?} failed: {error:?}"))?;
    }
    for (key, value) in INITIAL_ROWS {
        table
            .put_presence_inner(&mut transaction, None, key, value)
            .map_err(|error| format!("initial put for {key:?} failed: {error:?}"))?;
    }
    match transaction
        .commit()
        .map_err(|error| format!("initialization commit failed: {error:?}"))?
    {
        CommitOutcome::Committed(_) => {}
        outcome => return Err(format!("initialization did not commit: {outcome:?}")),
    }
    table
        .seal_directory_structure()
        .map_err(|error| format!("directory seal failed: {error:?}"))
}

fn reset_table(table: &Table, worker: &mut WorkerContext) -> Result<(), String> {
    let mut transaction = worker
        .begin()
        .map_err(|error| format!("reset begin failed: {error:?}"))?;
    let rows = table
        .scan_inner(
            &mut transaction,
            None,
            ScanRequest::new(ScanDirection::Forward, FULL_SCAN_LIMIT)
                .with_lower(ScanBound::Included(b"")),
        )
        .map_err(|error| format!("reset scan failed: {error:?}"))?;
    if rows.len() == FULL_SCAN_LIMIT {
        return Err(String::from("reset scan reached its completeness limit"));
    }
    for row in rows {
        table
            .remove_presence_inner(&mut transaction, None, row.key())
            .map_err(|error| format!("reset remove for {:?} failed: {error:?}", row.key()))?;
    }
    for (key, value) in INITIAL_ROWS {
        table
            .put_presence_inner(&mut transaction, None, key, value)
            .map_err(|error| format!("reset put for {key:?} failed: {error:?}"))?;
    }
    match transaction
        .commit()
        .map_err(|error| format!("reset commit failed: {error:?}"))?
    {
        CommitOutcome::Committed(_) => Ok(()),
        outcome => Err(format!("reset transaction did not commit: {outcome:?}")),
    }
}

fn observe_complete_state(table: &Table, worker: &mut WorkerContext) -> Result<State, String> {
    let mut transaction = worker
        .begin()
        .map_err(|error| format!("observer begin failed: {error:?}"))?;
    let rows = table
        .scan_inner(
            &mut transaction,
            None,
            ScanRequest::new(ScanDirection::Forward, FULL_SCAN_LIMIT)
                .with_lower(ScanBound::Included(b"")),
        )
        .map_err(|error| format!("observer scan failed: {error:?}"))?;
    if rows.len() == FULL_SCAN_LIMIT {
        return Err(String::from("observer scan reached its completeness limit"));
    }
    let mut state = State::new();
    for row in rows {
        if state_insert(
            &mut state,
            TABLE_ID,
            row.key().to_vec(),
            row.value().to_vec(),
        )
        .is_some()
        {
            return Err(String::from("observer scan returned a duplicate key"));
        }
    }
    match transaction
        .commit()
        .map_err(|error| format!("observer commit failed: {error:?}"))?
    {
        CommitOutcome::Committed(_) => Ok(state),
        outcome => Err(format!("observer transaction did not commit: {outcome:?}")),
    }
}

fn is_committed(transaction: &HistoryTransaction) -> bool {
    transaction
        .terminal
        .as_ref()
        .is_some_and(|terminal| terminal.outcome == Some(TerminalOutcome::Committed))
}

fn overlapping_committed_pairs(history: &History) -> usize {
    let committed: Vec<_> = history
        .transactions
        .iter()
        .filter(|transaction| is_committed(transaction))
        .collect();
    let mut pairs = 0;
    for (index, first) in committed.iter().enumerate() {
        let first_end = first
            .terminal
            .as_ref()
            .and_then(|terminal| terminal.interval.response)
            .expect("a committed terminal call has a response");
        for second in &committed[index + 1..] {
            let second_end = second
                .terminal
                .as_ref()
                .and_then(|terminal| terminal.interval.response)
                .expect("a committed terminal call has a response");
            if first.begin.invocation < second_end && second.begin.invocation < first_end {
                pairs += 1;
            }
        }
    }
    pairs
}

fn record_required_operation(
    table: &Table,
    transaction: &mut Transaction<'_, Active>,
    clock: &LogicalClock,
    operation: Operation,
    context: &str,
) -> Result<TimedOperation, String> {
    let invocation = clock.next();
    let result = execute_operation(table, transaction, &operation);
    let response = clock.next();
    result
        .map(|observation| TimedOperation::completed(invocation, response, operation, observation))
        .map_err(|error| format!("{context} failed: {error:?}"))
}

fn commit_required(
    transaction: Transaction<'_, Active>,
    recorded: &mut HistoryTransaction,
    clock: &LogicalClock,
    context: &str,
) -> Result<(), String> {
    let invocation = clock.next();
    let result = transaction.commit();
    let response = clock.next();
    match result.map_err(|error| format!("{context} commit failed: {error:?}"))? {
        CommitOutcome::Committed(_) => {
            recorded.finish(TerminalCall::commit(
                invocation,
                response,
                TerminalOutcome::Committed,
            ));
            Ok(())
        }
        CommitOutcome::Aborted(reason) => {
            let outcome = match reason {
                AbortReason::Conflict(_) => TerminalOutcome::Conflict,
                _ => TerminalOutcome::Aborted,
            };
            recorded.finish(TerminalCall::commit(invocation, response, outcome));
            Err(format!("{context} did not commit: {reason:?}"))
        }
    }
}

fn run_dependency_reader(
    runtime: Arc<Runtime>,
    table: Table,
    clock: Arc<LogicalClock>,
    events: &Sender<DependencyEvent>,
    release: &Receiver<()>,
    deadline: Instant,
) -> Result<HistoryTransaction, String> {
    let mut worker = runtime
        .attach()
        .map_err(|error| format!("dependency reader attach failed: {error:?}"))?;
    let begin_invocation = clock.next();
    let mut transaction = worker
        .begin()
        .map_err(|error| format!("dependency reader begin failed: {error:?}"))?;
    let begin_response = clock.next();
    let mut recorded = HistoryTransaction::new(
        DEPENDENCY_READER_ID,
        Interval::completed(begin_invocation, begin_response),
    );
    events
        .send(DependencyEvent::Begun)
        .map_err(|_| String::from("dependency reader could not report begin"))?;
    receive_until(release, deadline, "dependency writer commit release")?;

    recorded.push(record_required_operation(
        &table,
        &mut transaction,
        &clock,
        Operation::get(TABLE_ID, DEPENDENCY_KEY),
        "dependency reader get",
    )?);
    commit_required(transaction, &mut recorded, &clock, "dependency reader")?;
    Ok(recorded)
}

fn join_dependency_reader(handle: thread::JoinHandle<()>) -> Result<(), String> {
    let deadline = Instant::now()
        .checked_add(SHUTDOWN_TIMEOUT)
        .expect("dependency shutdown deadline fits in Instant");
    while !handle.is_finished() && Instant::now() < deadline {
        thread::yield_now();
    }
    if !handle.is_finished() {
        drop(handle);
        return Err(String::from(
            "dependency reader did not stop before the shutdown deadline",
        ));
    }
    handle.join().map_err(|payload| {
        format!(
            "dependency reader wrapper panicked: {}",
            panic_detail(payload)
        )
    })
}

fn dependency_state(value: &[u8]) -> State {
    let mut state = State::new();
    state_insert(&mut state, TABLE_ID, DEPENDENCY_KEY, value);
    state
}

fn run_dependency_history(mode: MemoryMode) -> Result<(), String> {
    let runtime = Runtime::new(RuntimeConfig::default())
        .map_err(|error| format!("{mode:?} dependency runtime creation failed: {error:?}"))?;
    let table = make_table(mode, &runtime);
    let mut writer_worker = runtime
        .attach()
        .map_err(|error| format!("{mode:?} dependency writer attach failed: {error:?}"))?;

    let mut initialize = writer_worker
        .begin()
        .map_err(|error| format!("{mode:?} dependency initialization begin failed: {error:?}"))?;
    table
        .put_inner(&mut initialize, None, DEPENDENCY_KEY, DEPENDENCY_OLD_VALUE)
        .map_err(|error| format!("{mode:?} dependency initialization put failed: {error:?}"))?;
    match initialize
        .commit()
        .map_err(|error| format!("{mode:?} dependency initialization commit failed: {error:?}"))?
    {
        CommitOutcome::Committed(_) => {}
        outcome => {
            return Err(format!(
                "{mode:?} dependency initialization did not commit: {outcome:?}"
            ));
        }
    }
    table
        .seal_directory_structure()
        .map_err(|error| format!("{mode:?} dependency directory seal failed: {error:?}"))?;

    let clock = Arc::new(LogicalClock::default());
    let deadline = Instant::now()
        .checked_add(ROUND_TIMEOUT)
        .expect("dependency deadline fits in Instant");
    let (events, event_receiver) = mpsc::channel();
    let (release, release_receiver) = mpsc::channel();
    let reader_runtime = Arc::clone(&runtime);
    let reader_table = table.clone();
    let reader_clock = Arc::clone(&clock);
    let reader_events = events.clone();
    let failure_events = events.clone();
    let handle = thread::spawn(move || {
        let result = catch_unwind(AssertUnwindSafe(|| {
            run_dependency_reader(
                reader_runtime,
                reader_table,
                reader_clock,
                &reader_events,
                &release_receiver,
                deadline,
            )
        }));
        let event = match result {
            Ok(Ok(recorded)) => DependencyEvent::Finished(recorded),
            Ok(Err(error)) => DependencyEvent::Failed(error),
            Err(payload) => DependencyEvent::Failed(format!("panicked: {}", panic_detail(payload))),
        };
        let _ = failure_events.send(event);
    });
    drop(events);

    let coordinated = (|| {
        match receive_until(&event_receiver, deadline, "dependency reader begin")? {
            DependencyEvent::Begun => {}
            DependencyEvent::Failed(error) => return Err(error),
            event => {
                return Err(format!(
                    "unexpected dependency event before begin: {event:?}"
                ))
            }
        }

        let begin_invocation = clock.next();
        let mut writer = writer_worker
            .begin()
            .map_err(|error| format!("{mode:?} dependency writer begin failed: {error:?}"))?;
        let begin_response = clock.next();
        let mut writer_record = HistoryTransaction::new(
            DEPENDENCY_WRITER_ID,
            Interval::completed(begin_invocation, begin_response),
        );
        writer_record.push(record_required_operation(
            &table,
            &mut writer,
            &clock,
            Operation::put(TABLE_ID, DEPENDENCY_KEY, DEPENDENCY_NEW_VALUE),
            "dependency writer put",
        )?);
        commit_required(writer, &mut writer_record, &clock, "dependency writer")?;
        release
            .send(())
            .map_err(|_| String::from("could not release dependency reader"))?;

        let reader_record =
            match receive_until(&event_receiver, deadline, "dependency reader completion")? {
                DependencyEvent::Finished(recorded) => recorded,
                DependencyEvent::Failed(error) => return Err(error),
                event => {
                    return Err(format!(
                        "unexpected dependency event after writer commit: {event:?}"
                    ));
                }
            };
        Ok((reader_record, writer_record))
    })();
    drop(release);
    let shutdown = join_dependency_reader(handle);
    let (reader_record, writer_record) = match (coordinated, shutdown) {
        (Ok(records), Ok(())) => records,
        (Err(error), Ok(())) | (Ok(_), Err(error)) => return Err(error),
        (Err(error), Err(shutdown)) => {
            return Err(format!("{error}; reader shutdown failed: {shutdown}"));
        }
    };

    let final_state = observe_complete_state(&table, &mut writer_worker)
        .map_err(|error| format!("{mode:?} dependency final observation failed: {error}"))?;
    let mut history = History::new(dependency_state(DEPENDENCY_OLD_VALUE));
    history.set_observed_final_state(final_state);
    // Put the reader first so insertion order cannot accidentally satisfy the
    // required serialization edge.
    history.push(reader_record);
    history.push(writer_record);
    if overlapping_committed_pairs(&history) != 1 {
        return Err(format!(
            "{mode:?} dependency history did not contain one overlapping committed pair\n{}",
            history.to_replay_text()
        ));
    }
    let witness =
        check_strict_serializability(&history, CheckOptions::default()).map_err(|error| {
            format!("{mode:?} dependency history failed strict serializability:\n{error}")
        })?;
    if witness.serialization != [DEPENDENCY_WRITER_ID, DEPENDENCY_READER_ID] {
        return Err(format!(
            "{mode:?} dependency witness did not order writer before reader: {:?}\n{}",
            witness.serialization,
            history.to_replay_text()
        ));
    }
    Ok(())
}

fn has_full_operation_coverage(history: &History) -> bool {
    let mut get = false;
    let mut put = false;
    let mut insert = false;
    let mut remove = false;
    let mut forward = false;
    let mut reverse = false;
    for transaction in &history.transactions {
        if !is_committed(transaction) {
            continue;
        }
        for operation in &transaction.operations {
            match &operation.operation {
                Operation::Get { .. } => get = true,
                Operation::Put { .. } => put = true,
                Operation::Insert { .. } => insert = true,
                Operation::Remove { .. } => remove = true,
                Operation::Scan { direction, .. } => match direction {
                    HistoryScanDirection::Forward => forward = true,
                    HistoryScanDirection::Reverse => reverse = true,
                },
            }
        }
    }
    get && put && insert && remove && forward && reverse
}

fn run_history_sweep(mode: MemoryMode) -> Result<(), String> {
    let runtime = Runtime::new(RuntimeConfig::default())
        .map_err(|error| format!("runtime creation failed: {error:?}"))?;
    let table = make_table(mode, &runtime);
    let mut observer = runtime
        .attach()
        .map_err(|error| format!("observer attach failed: {error:?}"))?;
    prepare_key_domain(&table, &mut observer)?;
    let clock = Arc::new(LogicalClock::default());
    let mut committed_total = 0;
    let mut active_seeds = 0;
    let mut overlapping_pair_total = 0;
    let mut overlapping_seeds = 0;

    for seed in 0..SEED_COUNT {
        reset_table(&table, &mut observer).map_err(|error| format!("seed {seed}: {error}"))?;
        let reset_state = observe_complete_state(&table, &mut observer)
            .map_err(|error| format!("seed {seed}: {error}"))?;
        if reset_state != initial_state() {
            return Err(format!(
                "seed {seed}: reset state mismatch: expected {:?}, observed {:?}",
                initial_state(),
                reset_state
            ));
        }

        let transactions = run_round(seed, &runtime, &table, &clock)?;
        let committed = transactions
            .iter()
            .filter(|transaction| is_committed(transaction))
            .count();
        if committed != 0 {
            active_seeds += 1;
        }
        committed_total += committed;

        let final_state = observe_complete_state(&table, &mut observer)
            .map_err(|error| format!("seed {seed}: {error}"))?;
        let mut history = History::new(initial_state());
        history.set_observed_final_state(final_state);
        for transaction in transactions {
            history.push(transaction);
        }
        let overlapping_pairs = overlapping_committed_pairs(&history);
        overlapping_pair_total += overlapping_pairs;
        if overlapping_pairs != 0 {
            overlapping_seeds += 1;
        }
        if committed != 0 && !has_full_operation_coverage(&history) {
            return Err(format!(
                "seed {seed}: committed operation coverage was incomplete\n{}",
                history.to_replay_text()
            ));
        }
        check_strict_serializability(&history, CheckOptions::default()).map_err(|error| {
            format!("{mode:?} seed {seed} failed strict serializability:\n{error}")
        })?;
    }
    if committed_total < SEED_COUNT as usize {
        return Err(format!(
            "{mode:?}: only {committed_total} transactions committed across {SEED_COUNT} seeds"
        ));
    }
    if active_seeds < (SEED_COUNT / 2) as usize {
        return Err(format!(
            "{mode:?}: committed activity occurred in only {active_seeds} of {SEED_COUNT} seeds"
        ));
    }
    if overlapping_pair_total < MIN_OVERLAPPING_PAIRS {
        return Err(format!(
            "{mode:?}: only {overlapping_pair_total} overlapping committed pairs; expected at least {MIN_OVERLAPPING_PAIRS}"
        ));
    }
    if overlapping_seeds < MIN_OVERLAPPING_SEEDS {
        return Err(format!(
            "{mode:?}: overlapping commits occurred in only {overlapping_seeds} of {SEED_COUNT} seeds; expected at least {MIN_OVERLAPPING_SEEDS}"
        ));
    }
    Ok(())
}

#[test]
fn registry_id_memory_histories_are_strictly_serializable() {
    if let Err(error) = run_history_sweep(MemoryMode::RegistryId)
        .and_then(|()| run_dependency_history(MemoryMode::RegistryId))
    {
        panic!("{error}");
    }
}

#[test]
fn direct_token_memory_histories_are_strictly_serializable() {
    if let Err(error) = run_history_sweep(MemoryMode::DirectToken)
        .and_then(|()| run_dependency_history(MemoryMode::DirectToken))
    {
        panic!("{error}");
    }
}

#[test]
fn memory_table_composes_atomically_with_transactional_vector() {
    let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
    let table = Table::new_memory(&runtime, table_config());
    let values = TxnVec::from_iter(&runtime, [10_u64]).unwrap();
    let mut worker = runtime.attach().unwrap();

    let mut initialize = worker.begin().unwrap();
    assert!(!table
        .put_presence_inner(&mut initialize, None, b"mixed\0key", b"old\xff" as &[u8])
        .unwrap());
    expect_commit(initialize.commit(), "mixed-adapter initialization");

    let mut committed = worker.begin().unwrap();
    assert_eq!(
        table
            .put_inner(&mut committed, None, b"mixed\0key", b"new\0value" as &[u8])
            .unwrap()
            .as_deref(),
        Some(b"old\xff" as &[u8])
    );
    assert_eq!(values.set(&mut committed, 0, 20).unwrap().unwrap(), 10);
    expect_commit(committed.commit(), "mixed-adapter transaction");

    let mut aborted = worker.begin().unwrap();
    table
        .put_presence_inner(&mut aborted, None, b"mixed\0key", b"discarded" as &[u8])
        .unwrap();
    assert_eq!(values.set(&mut aborted, 0, 30).unwrap().unwrap(), 20);
    aborted.abort();

    let mut observe = worker.begin().unwrap();
    assert_eq!(
        table
            .get_inner(&mut observe, None, b"mixed\0key")
            .unwrap()
            .as_deref(),
        Some(b"new\0value" as &[u8])
    );
    assert_eq!(values.get(&mut observe, 0).unwrap().unwrap(), 20);
    expect_commit(observe.commit(), "mixed-adapter observation");
}
