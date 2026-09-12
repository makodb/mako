#![cfg(mtree_native_integration)]

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
use masstree::{Runtime as MasstreeRuntime, RuntimeConfig as MasstreeRuntimeConfig};
use sto_core::{AbortReason, AccessError, CommitOutcome, Runtime, RuntimeConfig, WorkerContext};
use sto_masstree::{
    InsertOutcome, ScanBound, ScanDirection as MasstreeScanDirection, ScanRequest, Table,
    TableConfig,
};

const TABLE_ID: u64 = 1;
const WORKER_COUNT: usize = 3;
const TRANSACTIONS_PER_WORKER: usize = 2;
const SCANNER_WORKER: usize = 0;
const DEPENDENT_READER_WORKER: usize = 1;
const DEPENDENCY_WRITER_WORKER: usize = 2;
const DEPENDENCY_TRANSACTION: usize = 1;
const SEED_COUNT: u64 = 128;
const KEY_COUNT: usize = 8;
const MAX_RETAINED_RECORDS: u64 = 64;
const FULL_SCAN_LIMIT: usize = MAX_RETAINED_RECORDS as usize + 1;
const ROUND_TIMEOUT: Duration = Duration::from_secs(20);
const IDLE_TIMEOUT: Duration = Duration::from_secs(60);
const SHUTDOWN_TIMEOUT: Duration = Duration::from_secs(10);

// Keys 0..4 form the scanned low range, key 4 is the middle partition, and
// keys 5 through 7 form the high partition. The corpus includes empty, NUL-bearing,
// empty-value, and high-byte cases.
const KEYS: [&[u8]; KEY_COUNT] = [
    b"", b"\0", b"\0a", b"a\0", b"a\xff", b"b", b"\x80\0", b"\xff",
];

const INITIAL_ROWS: [(&[u8], &[u8]); 4] = [
    (KEYS[0], b"empty-key\0"),
    (KEYS[1], b"zero\0"),
    (KEYS[3], b""),
    (KEYS[7], b"tail\0\xff"),
];

#[derive(Clone, Copy, Debug)]
enum TableMode {
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
    Run {
        seed: u64,
        deadline: Instant,
    },
    Release {
        seed: u64,
        transaction: usize,
        phase: RoundPhase,
    },
    Stop,
}

#[derive(Debug)]
enum WorkerEvent {
    Online {
        worker: usize,
    },
    Arrived {
        seed: u64,
        worker: usize,
        transaction: usize,
        phase: RoundPhase,
    },
    RoundFinished {
        seed: u64,
        worker: usize,
        transactions: Vec<HistoryTransaction>,
    },
    Failed {
        worker: usize,
        detail: String,
    },
}

struct WorkerPool {
    commands: Vec<Sender<WorkerCommand>>,
    events: Receiver<WorkerEvent>,
    handles: Vec<Option<thread::JoinHandle<()>>>,
    stopped: bool,
}

struct QuiesceOnDrop<'worker>(&'worker masstree::Worker);

impl Drop for QuiesceOnDrop<'_> {
    fn drop(&mut self) {
        let _ = self.0.quiesce();
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

fn transaction_id(worker: usize, transaction: usize) -> u32 {
    (worker * TRANSACTIONS_PER_WORKER + transaction + 1) as u32
}

fn plan(seed: u64, worker: usize, transaction: usize) -> Vec<Operation> {
    let entropy = mix(seed ^ ((worker as u64) << 16) ^ ((transaction as u64) << 32));
    let point_keys = |domain: [usize; 3]| {
        let offset = entropy as usize % domain.len();
        (
            domain[offset],
            domain[(offset + 1) % domain.len()],
            domain[(offset + 2) % domain.len()],
        )
    };
    let point_plan = |domain: [usize; 3]| {
        let (first, second, third) = point_keys(domain);
        vec![
            Operation::get(TABLE_ID, KEYS[first]),
            Operation::get(TABLE_ID, KEYS[second]),
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
            Operation::get(TABLE_ID, KEYS[first]),
        ]
    };

    // Worker 0 always exercises scan read-your-writes in the low partition.
    // Worker 2 contends there in wave 0, then writes the middle partition in
    // wave 1. Worker 1 mutates the high partition and reads that middle write
    // after it commits. This gives each seed a scan conflict and an overlapping
    // committed writer/reader dependency.
    match (worker, transaction) {
        (SCANNER_WORKER, _) => {
            let first = entropy as usize % 4;
            let stride = if entropy & 1 == 0 { 1 } else { 3 };
            let second = (first + stride) % 4;
            let third = (first + 2 * stride) % 4;
            vec![
                Operation::get(TABLE_ID, KEYS[first]),
                Operation::scan(
                    TABLE_ID,
                    Vec::new(),
                    Some(KEYS[4].to_vec()),
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
                Operation::scan(
                    TABLE_ID,
                    Vec::new(),
                    Some(KEYS[4].to_vec()),
                    HistoryScanDirection::Reverse,
                ),
            ]
        }
        (1, 0) => point_plan([5, 6, 7]),
        (1, 1) => {
            let (first, second, third) = point_keys([5, 6, 7]);
            vec![
                Operation::get(TABLE_ID, KEYS[first]),
                Operation::get(TABLE_ID, KEYS[second]),
                Operation::get(TABLE_ID, KEYS[4]),
                Operation::put(
                    TABLE_ID,
                    KEYS[first],
                    unique_value(seed, worker, transaction, 3),
                ),
                Operation::insert(
                    TABLE_ID,
                    KEYS[second],
                    unique_value(seed, worker, transaction, 4),
                ),
                Operation::remove(TABLE_ID, KEYS[third]),
                Operation::get(TABLE_ID, KEYS[first]),
            ]
        }
        (2, 0) => point_plan([0, 1, 2]),
        (2, 1) => vec![
            Operation::get(TABLE_ID, KEYS[4]),
            Operation::get(TABLE_ID, KEYS[4]),
            Operation::put(
                TABLE_ID,
                KEYS[4],
                unique_value(seed, worker, transaction, 2),
            ),
            Operation::get(TABLE_ID, KEYS[4]),
        ],
        _ => unreachable!("the test has exactly three workers and two transaction waves"),
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

fn execute_operation(
    table: &Table,
    transaction: &mut sto_core::Transaction<'_, sto_core::Active>,
    native_worker: &masstree::Worker,
    operation: &Operation,
) -> Result<Observation, AccessError> {
    match operation {
        Operation::Get { table: id, key } => {
            assert_eq!(*id, TABLE_ID);
            table
                .get(transaction, native_worker, key)
                .map(|value| Observation::Get(value.map(|value| value.to_vec())))
        }
        Operation::Put {
            table: id,
            key,
            value,
        } => {
            assert_eq!(*id, TABLE_ID);
            table
                .put(transaction, native_worker, key, value)
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
                .insert(transaction, native_worker, key, value)
                .map(|outcome| Observation::InsertWithPrevious {
                    previous: match outcome {
                        InsertOutcome::Inserted => None,
                        InsertOutcome::AlreadyPresent(value) => Some(value.to_vec()),
                    },
                })
        }
        Operation::Remove { table: id, key } => {
            assert_eq!(*id, TABLE_ID);
            table
                .remove(transaction, native_worker, key)
                .map(|previous| Observation::RemoveWithPrevious {
                    previous: previous.map(|value| value.to_vec()),
                })
        }
        Operation::Scan {
            table: id,
            lower,
            upper,
            direction,
        } => {
            assert_eq!(*id, TABLE_ID);
            let native_direction = match direction {
                HistoryScanDirection::Forward => MasstreeScanDirection::Forward,
                HistoryScanDirection::Reverse => MasstreeScanDirection::Reverse,
            };
            let mut request = ScanRequest::new(native_direction, FULL_SCAN_LIMIT)
                .with_lower(ScanBound::Included(lower));
            if let Some(upper) = upper {
                request = request.with_upper(ScanBound::Excluded(upper));
            }
            table.scan(transaction, native_worker, request).map(|rows| {
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
    reason = "the recorder keeps the native call and all diagnostic coordinates together"
)]
fn record_operation(
    seed: u64,
    worker: usize,
    transaction_index: usize,
    operation_index: usize,
    table: &Table,
    transaction: &mut sto_core::Transaction<'_, sto_core::Active>,
    native_worker: &masstree::Worker,
    clock: &LogicalClock,
    operation: Operation,
) -> Result<(TimedOperation, bool), String> {
    yield_for(seed, worker, transaction_index, operation_index);
    let invocation = clock.next();
    let result = execute_operation(table, transaction, native_worker, &operation);
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
    native_worker: &masstree::Worker,
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
    let id = transaction_id(worker, transaction_index);
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
            native_worker,
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
                native_worker,
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
    reason = "one worker round carries thread-affine handles plus bounded coordinator state"
)]
fn run_worker_round(
    seed: u64,
    worker: usize,
    table: &Table,
    sto_worker: &mut WorkerContext,
    native_worker: &masstree::Worker,
    clock: &LogicalClock,
    events: &Sender<WorkerEvent>,
    commands: &Receiver<WorkerCommand>,
    deadline: Instant,
) -> Result<Vec<HistoryTransaction>, String> {
    let mut transactions = Vec::with_capacity(TRANSACTIONS_PER_WORKER);
    for transaction in 0..TRANSACTIONS_PER_WORKER {
        transactions.push(run_transaction(
            seed,
            worker,
            transaction,
            table,
            sto_worker,
            native_worker,
            clock,
            events,
            commands,
            deadline,
        )?);
    }
    native_worker
        .quiesce()
        .map_err(|error| format!("seed {seed}, worker {worker} failed to quiesce: {error:?}"))?;
    Ok(transactions)
}

fn worker_loop(
    worker: usize,
    native_runtime: MasstreeRuntime,
    sto_runtime: Arc<Runtime>,
    table: Table,
    clock: Arc<LogicalClock>,
    events: Sender<WorkerEvent>,
    commands: Receiver<WorkerCommand>,
) -> Result<(), String> {
    let native_worker = native_runtime
        .attach()
        .map_err(|error| format!("worker {worker} native attach failed: {error:?}"))?;
    let _quiesce_on_exit = QuiesceOnDrop(&native_worker);
    let mut sto_worker = sto_runtime
        .attach()
        .map_err(|error| format!("worker {worker} STO attach failed: {error:?}"))?;
    events
        .send(WorkerEvent::Online { worker })
        .map_err(|_| format!("worker {worker} could not report startup"))?;

    loop {
        match commands.recv_timeout(IDLE_TIMEOUT) {
            Ok(WorkerCommand::Run { seed, deadline }) => {
                let transactions = run_worker_round(
                    seed,
                    worker,
                    &table,
                    &mut sto_worker,
                    &native_worker,
                    &clock,
                    &events,
                    &commands,
                    deadline,
                )?;
                events
                    .send(WorkerEvent::RoundFinished {
                        seed,
                        worker,
                        transactions,
                    })
                    .map_err(|_| {
                        format!("worker {worker} could not report completion for seed {seed}")
                    })?;
            }
            Ok(WorkerCommand::Stop) => {
                native_worker.quiesce().map_err(|error| {
                    format!("worker {worker} failed to quiesce at shutdown: {error:?}")
                })?;
                return Ok(());
            }
            Ok(command @ WorkerCommand::Release { .. }) => {
                return Err(format!(
                    "worker {worker} received stray command {command:?} while idle"
                ));
            }
            Err(RecvTimeoutError::Timeout) => {
                return Err(format!("worker {worker} timed out waiting for a job"));
            }
            Err(RecvTimeoutError::Disconnected) => return Ok(()),
        }
    }
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

fn store_round_finished(
    seed: u64,
    event: WorkerEvent,
    by_worker: &mut [Option<Vec<HistoryTransaction>>],
) -> Result<(), String> {
    match event {
        WorkerEvent::RoundFinished {
            seed: finished_seed,
            worker,
            transactions,
        } if finished_seed == seed && worker < WORKER_COUNT => {
            if by_worker[worker].replace(transactions).is_some() {
                return Err(format!(
                    "worker {worker} returned two results for seed {seed}"
                ));
            }
            Ok(())
        }
        WorkerEvent::Failed { worker, detail } => {
            Err(format!("seed {seed}: worker {worker} failed: {detail}"))
        }
        event => Err(format!(
            "seed {seed}: unexpected event while collecting results: {event:?}"
        )),
    }
}

impl WorkerPool {
    fn new(
        native_runtime: &MasstreeRuntime,
        sto_runtime: &Arc<Runtime>,
        table: &Table,
        clock: &Arc<LogicalClock>,
    ) -> Result<Self, String> {
        let (events, event_receiver) = mpsc::channel();
        let mut commands = Vec::with_capacity(WORKER_COUNT);
        let mut handles = Vec::with_capacity(WORKER_COUNT);

        for worker in 0..WORKER_COUNT {
            let (command_sender, command_receiver) = mpsc::channel();
            commands.push(command_sender);
            let native_runtime = native_runtime.clone();
            let sto_runtime = Arc::clone(sto_runtime);
            let table = table.clone();
            let clock = Arc::clone(clock);
            let worker_events = events.clone();
            let failure_events = events.clone();
            handles.push(Some(thread::spawn(move || {
                let result = catch_unwind(AssertUnwindSafe(|| {
                    worker_loop(
                        worker,
                        native_runtime,
                        sto_runtime,
                        table,
                        clock,
                        worker_events,
                        command_receiver,
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

        let mut pool = Self {
            commands,
            events: event_receiver,
            handles,
            stopped: false,
        };
        let deadline = Instant::now()
            .checked_add(ROUND_TIMEOUT)
            .expect("startup deadline fits in Instant");
        if let Err(error) = pool.wait_until_online(deadline) {
            let _ = pool.shutdown();
            return Err(error);
        }
        Ok(pool)
    }

    fn wait_until_online(&self, deadline: Instant) -> Result<(), String> {
        let mut online = [false; WORKER_COUNT];
        for _ in 0..WORKER_COUNT {
            match receive_until(&self.events, deadline, "worker startup")? {
                WorkerEvent::Online { worker } if worker < WORKER_COUNT => {
                    if std::mem::replace(&mut online[worker], true) {
                        return Err(format!("worker {worker} reported startup twice"));
                    }
                }
                WorkerEvent::Online { worker } => {
                    return Err(format!("unknown worker {worker} reported startup"));
                }
                WorkerEvent::Failed { worker, detail } => {
                    return Err(format!("worker {worker} failed during startup: {detail}"));
                }
                event => return Err(format!("unexpected startup event {event:?}")),
            }
        }
        Ok(())
    }

    fn run_round(&self, seed: u64) -> Result<Vec<HistoryTransaction>, String> {
        let deadline = Instant::now()
            .checked_add(ROUND_TIMEOUT)
            .expect("round deadline fits in Instant");
        for (worker, commands) in self.commands.iter().enumerate() {
            commands
                .send(WorkerCommand::Run { seed, deadline })
                .map_err(|_| format!("could not send seed {seed} to worker {worker}"))?;
        }
        let mut by_worker: Vec<Option<Vec<HistoryTransaction>>> =
            std::iter::repeat_with(|| None).take(WORKER_COUNT).collect();

        for transaction in 0..TRANSACTIONS_PER_WORKER {
            for phase in RoundPhase::ALL {
                let mut arrived = [false; WORKER_COUNT];
                for _ in 0..WORKER_COUNT {
                    match receive_until(
                        &self.events,
                        deadline,
                        &format!(
                            "seed {seed}, transaction {transaction}, phase {phase:?} arrivals"
                        ),
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

                let release = |worker: usize| {
                    self.commands[worker]
                        .send(WorkerCommand::Release {
                            seed,
                            transaction,
                            phase,
                        })
                        .map_err(|_| {
                            format!(
                                "could not release worker {worker} for seed {seed}, transaction {transaction}, phase {phase:?}"
                            )
                        })
                };
                if transaction == DEPENDENCY_TRANSACTION
                    && phase == RoundPhase::InitialReadsComplete
                {
                    // Keep the dependent reader active while the middle
                    // writer commits, then let the reader observe that write.
                    release(DEPENDENCY_WRITER_WORKER)?;
                    let event = receive_until(
                        &self.events,
                        deadline,
                        &format!("seed {seed} dependency writer completion"),
                    )?;
                    store_round_finished(seed, event, &mut by_worker)?;
                    if by_worker[DEPENDENCY_WRITER_WORKER].is_none() {
                        return Err(format!(
                            "seed {seed}: a worker other than the dependency writer finished first"
                        ));
                    }
                    release(DEPENDENT_READER_WORKER)?;
                    let event = receive_until(
                        &self.events,
                        deadline,
                        &format!("seed {seed} dependent reader completion"),
                    )?;
                    store_round_finished(seed, event, &mut by_worker)?;
                    if by_worker[DEPENDENT_READER_WORKER].is_none() {
                        return Err(format!(
                            "seed {seed}: a worker other than the dependent reader finished second"
                        ));
                    }
                    release(SCANNER_WORKER)?;
                } else {
                    for worker in 0..WORKER_COUNT {
                        release(worker)?;
                    }
                }
            }
        }

        while by_worker.iter().any(Option::is_none) {
            let event = receive_until(
                &self.events,
                deadline,
                &format!("seed {seed} worker results"),
            )?;
            store_round_finished(seed, event, &mut by_worker)?;
        }

        let mut transactions = Vec::with_capacity(WORKER_COUNT * TRANSACTIONS_PER_WORKER);
        for (worker, records) in by_worker.into_iter().enumerate() {
            let records = records
                .ok_or_else(|| format!("worker {worker} returned no result for seed {seed}"))?;
            transactions.extend(records);
        }
        Ok(transactions)
    }

    fn shutdown(&mut self) -> Result<(), String> {
        if !self.stopped {
            for commands in &self.commands {
                let _ = commands.send(WorkerCommand::Stop);
            }
            self.stopped = true;
        }

        let deadline = Instant::now()
            .checked_add(SHUTDOWN_TIMEOUT)
            .expect("shutdown deadline fits in Instant");
        while self
            .handles
            .iter()
            .flatten()
            .any(|handle| !handle.is_finished())
            && Instant::now() < deadline
        {
            thread::yield_now();
        }

        let mut errors = Vec::new();
        for (worker, handle) in self.handles.iter_mut().enumerate() {
            let Some(handle) = handle.take() else {
                continue;
            };
            if !handle.is_finished() {
                errors.push(format!("worker {worker} did not stop before the deadline"));
                drop(handle);
                continue;
            }
            if let Err(payload) = handle.join() {
                errors.push(format!(
                    "worker {worker} wrapper panicked: {}",
                    panic_detail(payload)
                ));
            }
        }
        if errors.is_empty() {
            Ok(())
        } else {
            Err(errors.join("; "))
        }
    }
}

impl Drop for WorkerPool {
    fn drop(&mut self) {
        let _ = self.shutdown();
    }
}

fn initial_state() -> State {
    let mut state = State::new();
    for (key, value) in INITIAL_ROWS {
        state_insert(&mut state, TABLE_ID, key, value);
    }
    state
}

fn table_config() -> TableConfig {
    TableConfig::new()
        .with_max_retained_records(MAX_RETAINED_RECORDS)
        .with_max_retained_key_bytes(1_024)
        .with_max_consumed_record_ids(256)
        .with_max_scan_physical_records(MAX_RETAINED_RECORDS as usize)
}

fn prime_key_domain(
    table: &Table,
    sto_worker: &mut WorkerContext,
    native_worker: &masstree::Worker,
) -> Result<(), String> {
    let mut transaction = sto_worker
        .begin()
        .map_err(|error| format!("key-domain setup begin failed: {error:?}"))?;
    let mut prime = |key: &[u8]| -> Result<(), String> {
        let value = table
            .get(&mut transaction, native_worker, key)
            .map_err(|error| format!("key-domain setup get for {key:?} failed: {error:?}"))?;
        if value.is_some() {
            return Err(format!(
                "fresh key-domain setup unexpectedly found a value for {key:?}"
            ));
        }
        Ok(())
    };

    for key in &KEYS[..4] {
        prime(key)?;
    }
    // General-table records currently share one lock target per 16 stable
    // IDs. Burn tombstone IDs between logical partitions so the disjoint
    // workload also uses disjoint physical lock targets.
    for index in 0..12_u8 {
        prime(&[0xfe, 0, index])?;
    }
    prime(KEYS[4])?;
    for index in 0..15_u8 {
        prime(&[0xfe, 1, index])?;
    }
    for key in &KEYS[5..] {
        prime(key)?;
    }
    match transaction
        .commit()
        .map_err(|error| format!("key-domain setup commit failed: {error:?}"))?
    {
        CommitOutcome::Committed(_) => Ok(()),
        outcome => Err(format!(
            "key-domain setup transaction did not commit: {outcome:?}"
        )),
    }
}

fn reset_table(
    table: &Table,
    sto_worker: &mut WorkerContext,
    native_worker: &masstree::Worker,
) -> Result<(), String> {
    let mut transaction = sto_worker
        .begin()
        .map_err(|error| format!("reset begin failed: {error:?}"))?;
    let rows = table
        .scan(
            &mut transaction,
            native_worker,
            ScanRequest::new(MasstreeScanDirection::Forward, FULL_SCAN_LIMIT)
                .with_lower(ScanBound::Included(b"")),
        )
        .map_err(|error| format!("reset scan failed: {error:?}"))?;
    if rows.len() == FULL_SCAN_LIMIT {
        return Err(String::from("reset scan reached its completeness limit"));
    }
    let keys: Vec<Vec<u8>> = rows.iter().map(|row| row.key().to_vec()).collect();
    for key in keys {
        table
            .remove(&mut transaction, native_worker, &key)
            .map_err(|error| format!("reset remove for {key:?} failed: {error:?}"))?;
    }
    for (key, value) in INITIAL_ROWS {
        table
            .put(&mut transaction, native_worker, key, value)
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

fn observe_complete_state(
    table: &Table,
    sto_worker: &mut WorkerContext,
    native_worker: &masstree::Worker,
) -> Result<State, String> {
    let mut transaction = sto_worker
        .begin()
        .map_err(|error| format!("observer begin failed: {error:?}"))?;
    let rows = table
        .scan(
            &mut transaction,
            native_worker,
            ScanRequest::new(MasstreeScanDirection::Forward, FULL_SCAN_LIMIT)
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

fn has_full_operation_coverage(history: &History) -> bool {
    let mut get = false;
    let mut put = false;
    let mut insert = false;
    let mut remove = false;
    let mut forward = false;
    let mut reverse = false;
    for transaction in history.transactions.iter().filter(|transaction| {
        transaction
            .terminal
            .as_ref()
            .is_some_and(|terminal| terminal.outcome == Some(TerminalOutcome::Committed))
    }) {
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

fn committed_interval(transaction: &HistoryTransaction) -> Option<(u64, u64)> {
    let terminal = transaction.terminal.as_ref()?;
    (terminal.outcome == Some(TerminalOutcome::Committed))
        .then_some((transaction.begin.invocation, terminal.interval.response?))
}

fn has_overlapping_committed_pair(history: &History) -> bool {
    let committed: Vec<_> = history
        .transactions
        .iter()
        .filter_map(committed_interval)
        .collect();
    committed.iter().enumerate().any(|(left_index, left)| {
        committed[left_index + 1..]
            .iter()
            .any(|right| left.0 < right.1 && right.0 < left.1)
    })
}

fn has_required_committed_dependency(history: &History, seed: u64) -> bool {
    let writer_id = transaction_id(DEPENDENCY_WRITER_WORKER, DEPENDENCY_TRANSACTION);
    let reader_id = transaction_id(DEPENDENT_READER_WORKER, DEPENDENCY_TRANSACTION);
    let Some(writer) = history
        .transactions
        .iter()
        .find(|transaction| transaction.id == writer_id)
    else {
        return false;
    };
    let Some(reader) = history
        .transactions
        .iter()
        .find(|transaction| transaction.id == reader_id)
    else {
        return false;
    };
    let (Some(writer_interval), Some(reader_interval)) =
        (committed_interval(writer), committed_interval(reader))
    else {
        return false;
    };
    let overlaps = writer_interval.0 < reader_interval.1 && reader_interval.0 < writer_interval.1;
    let expected = unique_value(seed, DEPENDENCY_WRITER_WORKER, DEPENDENCY_TRANSACTION, 2);
    let writer_recorded_value = writer.operations.iter().any(|timed| {
        matches!(
            (&timed.operation, &timed.observation),
            (
                Operation::Put { key, value, .. },
                Some(Observation::PutWithPrevious { .. })
            ) if key.as_slice() == KEYS[4] && value == &expected
        )
    });
    let reader_observed_value = reader.operations.iter().any(|timed| {
        matches!(
            (&timed.operation, &timed.observation),
            (Operation::Get { key, .. }, Some(Observation::Get(Some(value))))
                if key.as_slice() == KEYS[4] && value == &expected
        )
    });
    overlaps && writer_recorded_value && reader_observed_value
}

fn run_table_sweep(
    mode: TableMode,
    native_runtime: &MasstreeRuntime,
    native_worker: &masstree::Worker,
) -> Result<(), String> {
    let sto_runtime = Runtime::new(RuntimeConfig::default())
        .map_err(|error| format!("STO runtime creation failed: {error:?}"))?;
    let table = match mode {
        TableMode::RegistryId => {
            let tree = native_runtime
                .create_tree(native_worker)
                .map_err(|error| format!("native tree creation failed: {error:?}"))?;
            Table::new(&sto_runtime, tree, table_config())
                .map_err(|error| format!("registry-ID table creation failed: {error:?}"))?
        }
        TableMode::DirectToken => {
            Table::new_direct(&sto_runtime, native_runtime, native_worker, table_config())
                .map_err(|error| format!("direct-token table creation failed: {error:?}"))?
        }
    };
    let mut sto_worker = sto_runtime
        .attach()
        .map_err(|error| format!("main STO attach failed: {error:?}"))?;
    prime_key_domain(&table, &mut sto_worker, native_worker)?;
    let clock = Arc::new(LogicalClock::default());
    let mut pool = WorkerPool::new(native_runtime, &sto_runtime, &table, &clock)?;
    let mut test_result = Ok(());
    let mut committed_total = 0_usize;
    let mut seeds_with_commit = 0_usize;
    let mut committed_coverage_seen = false;

    for seed in 0..SEED_COUNT {
        let result = (|| {
            reset_table(&table, &mut sto_worker, native_worker)
                .map_err(|error| format!("seed {seed}: {error}"))?;
            let reset_state = observe_complete_state(&table, &mut sto_worker, native_worker)
                .map_err(|error| format!("seed {seed}: {error}"))?;
            if reset_state != initial_state() {
                return Err(format!(
                    "seed {seed}: reset state mismatch: expected {:?}, observed {:?}",
                    initial_state(),
                    reset_state
                ));
            }

            let transactions = pool.run_round(seed)?;
            let committed = transactions
                .iter()
                .filter(|transaction| {
                    transaction.terminal.as_ref().is_some_and(|terminal| {
                        terminal.outcome == Some(TerminalOutcome::Committed)
                    })
                })
                .count();
            committed_total += committed;
            if committed != 0 {
                seeds_with_commit += 1;
            }

            let final_state = observe_complete_state(&table, &mut sto_worker, native_worker)
                .map_err(|error| format!("seed {seed}: {error}"))?;
            let mut history = History::new(initial_state());
            history.set_observed_final_state(final_state);
            for transaction in transactions {
                history.push(transaction);
            }
            let full_committed_coverage = has_full_operation_coverage(&history);
            committed_coverage_seen |= full_committed_coverage;
            if committed < 2 || !has_overlapping_committed_pair(&history) {
                return Err(format!(
                    "seed {seed}: expected at least two overlapping committed transactions, observed {committed}\n{}",
                    history.to_replay_text()
                ));
            }
            if !has_required_committed_dependency(&history, seed) {
                return Err(format!(
                    "seed {seed}: the overlapping writer/reader dependency was not observed\n{}",
                    history.to_replay_text()
                ));
            }
            if !full_committed_coverage {
                return Err(format!(
                    "seed {seed}: committed-operation coverage was incomplete\n{}",
                    history.to_replay_text()
                ));
            }
            let witness = check_strict_serializability(&history, CheckOptions::default())
                .map_err(|error| format!("seed {seed} failed strict serializability:\n{error}"))?;
            let writer_id = transaction_id(DEPENDENCY_WRITER_WORKER, DEPENDENCY_TRANSACTION);
            let reader_id = transaction_id(DEPENDENT_READER_WORKER, DEPENDENCY_TRANSACTION);
            let writer_position = witness
                .serialization
                .iter()
                .position(|id| *id == writer_id)
                .ok_or_else(|| {
                    format!(
                        "seed {seed}: dependency writer T{writer_id} is absent from witness {:?}\n{}",
                        witness.serialization,
                        history.to_replay_text()
                    )
                })?;
            let reader_position = witness
                .serialization
                .iter()
                .position(|id| *id == reader_id)
                .ok_or_else(|| {
                    format!(
                        "seed {seed}: dependent reader T{reader_id} is absent from witness {:?}\n{}",
                        witness.serialization,
                        history.to_replay_text()
                    )
                })?;
            if writer_position >= reader_position {
                return Err(format!(
                    "seed {seed}: witness {:?} did not order dependency writer T{writer_id} before reader T{reader_id}\n{}",
                    witness.serialization,
                    history.to_replay_text()
                ));
            }
            Ok(())
        })();
        if let Err(error) = result {
            test_result = Err(error);
            break;
        }
    }

    if test_result.is_ok()
        && (committed_total < 2 * SEED_COUNT as usize
            || seeds_with_commit != SEED_COUNT as usize
            || !committed_coverage_seen)
    {
        test_result = Err(format!(
            "commit coverage was too low: {committed_total} commits across {seeds_with_commit} of {SEED_COUNT} seeds; full committed-operation coverage: {committed_coverage_seen}"
        ));
    }

    let shutdown = pool.shutdown();
    match (test_result, shutdown) {
        (Ok(()), Ok(())) => Ok(()),
        (Err(error), Ok(())) | (Ok(()), Err(error)) => Err(error),
        (Err(error), Err(shutdown)) => Err(format!("{error}; worker shutdown failed: {shutdown}")),
    }
}

fn run_history_sweep() -> Result<(), String> {
    let native_runtime = MasstreeRuntime::new(MasstreeRuntimeConfig::new())
        .map_err(|error| format!("native runtime creation failed: {error:?}"))?;
    let native_worker = native_runtime
        .attach()
        .map_err(|error| format!("main native attach failed: {error:?}"))?;
    let _quiesce_on_exit = QuiesceOnDrop(&native_worker);

    for mode in [TableMode::RegistryId, TableMode::DirectToken] {
        run_table_sweep(mode, &native_runtime, &native_worker)
            .map_err(|error| format!("mode {mode:?}: {error}"))?;
        native_worker
            .quiesce()
            .map_err(|error| format!("mode {mode:?}: main native quiesce failed: {error:?}"))?;
    }
    Ok(())
}

#[test]
fn native_masstree_histories_are_strictly_serializable() {
    if let Err(error) = run_history_sweep() {
        panic!("{error}");
    }
}
