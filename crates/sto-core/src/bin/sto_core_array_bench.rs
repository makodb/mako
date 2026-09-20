#![deny(unsafe_code)]

use std::{
    env,
    sync::{
        atomic::{AtomicU8, AtomicUsize, Ordering},
        Arc, Barrier,
    },
    thread,
    time::{Duration, Instant},
};

use sto_core::{
    AbortReason, AccessError, CommitOutcome, Runtime, RuntimeConfig, TxnArray, WorkerContext,
};

const DEFAULT_THREADS: usize = 1;
const DEFAULT_KEYSPACE: u64 = 100_000;
const DEFAULT_OPS_PER_TXN: usize = 10;
const DEFAULT_WRITE_PERCENT: u32 = 50;
const DEFAULT_WARMUP_MS: u64 = 1_000;
const DEFAULT_DURATION_MS: u64 = 3_000;
const DEFAULT_SEED: u64 = 1;
const MAX_OPS_PER_TXN: usize = 32_768;

const PHASE_WAIT: u8 = 0;
const PHASE_WARMUP: u8 = 1;
const PHASE_QUIESCE: u8 = 2;
const PHASE_MEASURE: u8 = 3;
const PHASE_STOP: u8 = 4;
const PHASE_FAILED: u8 = 5;

const SPLITMIX_GAMMA: u64 = 0x9e37_79b9_7f4a_7c15;

fn splitmix_scramble(mut value: u64) -> u64 {
    value = (value ^ (value >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
    value = (value ^ (value >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
    value ^ (value >> 31)
}

fn worker_random_state(seed: u64, thread_id: usize) -> u64 {
    splitmix_scramble(seed.wrapping_add((thread_id as u64 + 1).wrapping_mul(SPLITMIX_GAMMA)))
}

#[derive(Clone, Copy, Debug)]
struct Config {
    threads: usize,
    keyspace: u64,
    ops_per_txn: usize,
    write_percent: u32,
    warmup_ms: u64,
    duration_ms: u64,
    seed: u64,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            threads: DEFAULT_THREADS,
            keyspace: DEFAULT_KEYSPACE,
            ops_per_txn: DEFAULT_OPS_PER_TXN,
            write_percent: DEFAULT_WRITE_PERCENT,
            warmup_ms: DEFAULT_WARMUP_MS,
            duration_ms: DEFAULT_DURATION_MS,
            seed: DEFAULT_SEED,
        }
    }
}

impl Config {
    fn parse() -> Result<Self, String> {
        let mut config = Self::default();
        let mut arguments = env::args().skip(1);
        while let Some(flag) = arguments.next() {
            let value = arguments
                .next()
                .ok_or_else(|| format!("missing value for {flag}"))?;
            match flag.as_str() {
                "--threads" => config.threads = parse_value(&flag, &value)?,
                "--keyspace" => config.keyspace = parse_value(&flag, &value)?,
                "--ops-per-txn" => config.ops_per_txn = parse_value(&flag, &value)?,
                "--write-percent" => config.write_percent = parse_value(&flag, &value)?,
                "--warmup-ms" => config.warmup_ms = parse_value(&flag, &value)?,
                "--duration-ms" => config.duration_ms = parse_value(&flag, &value)?,
                "--seed" => config.seed = parse_value(&flag, &value)?,
                _ => return Err(format!("unknown argument {flag}")),
            }
        }

        if config.threads == 0 {
            return Err("--threads must be positive".into());
        }
        if config.keyspace == 0 || usize::try_from(config.keyspace).is_err() {
            return Err("--keyspace must be positive and fit usize".into());
        }
        if config.ops_per_txn == 0 || config.ops_per_txn > MAX_OPS_PER_TXN {
            return Err("--ops-per-txn must be in 1..=32768".into());
        }
        if u64::try_from(config.ops_per_txn).map_or(true, |ops| ops > config.keyspace) {
            return Err("--keyspace must be at least --ops-per-txn".into());
        }
        if config.write_percent > 100 {
            return Err("--write-percent must be in 0..=100".into());
        }
        if config.duration_ms == 0 {
            return Err("--duration-ms must be positive".into());
        }
        Ok(config)
    }
}

fn parse_value<T>(flag: &str, value: &str) -> Result<T, String>
where
    T: std::str::FromStr,
{
    value
        .parse()
        .map_err(|_| format!("invalid value for {flag}: {value}"))
}

#[derive(Clone, Copy)]
struct Operation {
    index: usize,
    write: bool,
}

struct SplitMix64 {
    state: u64,
}

impl SplitMix64 {
    fn new(state: u64) -> Self {
        Self { state }
    }

    fn next_u64(&mut self) -> u64 {
        self.state = self.state.wrapping_add(SPLITMIX_GAMMA);
        let mut value = self.state;
        value = (value ^ (value >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        value = (value ^ (value >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        value ^ (value >> 31)
    }
}

#[derive(Default)]
struct WorkerStats {
    commits: u64,
    attempts: u64,
    aborts: u64,
    logical_ops: u64,
    checksum: u64,
}

enum AttemptOutcome {
    Committed(u64),
    Retry,
}

fn main() -> std::process::ExitCode {
    match run() {
        Ok(()) => std::process::ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("sto_core_array_bench: {error}");
            std::process::ExitCode::FAILURE
        }
    }
}

fn run() -> Result<(), String> {
    let config = Config::parse()?;
    let runtime = Runtime::new(
        RuntimeConfig::new()
            .with_max_workers(config.threads)
            .with_max_items_per_transaction(config.ops_per_txn)
            .with_max_locks_per_transaction(config.ops_per_txn),
    )
    .map_err(|error| format!("create STO runtime: {error:?}"))?;
    let array_len = usize::try_from(config.keyspace).expect("validated keyspace fits usize");
    let array = TxnArray::new(&runtime, (0..config.keyspace).take(array_len))
        .map_err(|error| format!("create transactional array: {error:?}"))?;

    let phase = Arc::new(AtomicU8::new(PHASE_WAIT));
    let quiesced = Arc::new(AtomicUsize::new(0));
    let ready = Arc::new(Barrier::new(config.threads + 1));
    let mut handles = Vec::with_capacity(config.threads);
    for thread_id in 0..config.threads {
        let runtime = Arc::clone(&runtime);
        let array = array.clone();
        let phase = Arc::clone(&phase);
        let quiesced = Arc::clone(&quiesced);
        let ready = Arc::clone(&ready);
        handles.push(thread::spawn(move || {
            let result = run_worker(
                thread_id, config, &runtime, &array, &phase, &quiesced, &ready,
            );
            if result.is_err() {
                phase.store(PHASE_FAILED, Ordering::Release);
            }
            result
        }));
    }

    ready.wait();
    transition_phase(&phase, PHASE_WAIT, PHASE_WARMUP, "before warmup")?;
    thread::sleep(Duration::from_millis(config.warmup_ms));
    if phase.load(Ordering::Acquire) != PHASE_WARMUP {
        return join_worker_errors(handles, "worker failed during warmup");
    }

    transition_phase(&phase, PHASE_WARMUP, PHASE_QUIESCE, "before quiescence")?;
    while quiesced.load(Ordering::Acquire) != config.threads {
        if phase.load(Ordering::Acquire) == PHASE_FAILED {
            return join_worker_errors(handles, "worker failed while quiescing");
        }
        thread::yield_now();
    }

    let measurement_start = Instant::now();
    transition_phase(&phase, PHASE_QUIESCE, PHASE_MEASURE, "before measurement")?;
    thread::sleep(Duration::from_millis(config.duration_ms));
    phase.store(PHASE_STOP, Ordering::Release);
    let elapsed = measurement_start.elapsed();

    let mut aggregate = WorkerStats::default();
    for handle in handles {
        let stats = handle
            .join()
            .map_err(|_| "benchmark worker panicked".to_owned())??;
        aggregate.commits = aggregate.commits.wrapping_add(stats.commits);
        aggregate.attempts = aggregate.attempts.wrapping_add(stats.attempts);
        aggregate.aborts = aggregate.aborts.wrapping_add(stats.aborts);
        aggregate.logical_ops = aggregate.logical_ops.wrapping_add(stats.logical_ops);
        aggregate.checksum = aggregate.checksum.wrapping_add(stats.checksum);
    }

    let elapsed_ns = u64::try_from(elapsed.as_nanos()).unwrap_or(u64::MAX);
    let elapsed_seconds = elapsed.as_secs_f64();
    let txn_per_sec = aggregate.commits as f64 / elapsed_seconds;
    let ops_per_sec = aggregate.logical_ops as f64 / elapsed_seconds;
    println!(
        concat!(
            "BENCH_RESULT={{",
            "\"engine\":\"rust-sto-core-array\",",
            "\"threads\":{},",
            "\"keyspace\":{},",
            "\"ops_per_txn\":{},",
            "\"write_percent\":{},",
            "\"warmup_ms\":{},",
            "\"duration_ms\":{},",
            "\"seed\":{},",
            "\"commits\":{},",
            "\"attempts\":{},",
            "\"aborts\":{},",
            "\"logical_ops\":{},",
            "\"elapsed_ns\":{},",
            "\"txn_per_sec\":{:.6},",
            "\"ops_per_sec\":{:.6},",
            "\"checksum\":{}",
            "}}"
        ),
        config.threads,
        config.keyspace,
        config.ops_per_txn,
        config.write_percent,
        config.warmup_ms,
        config.duration_ms,
        config.seed,
        aggregate.commits,
        aggregate.attempts,
        aggregate.aborts,
        aggregate.logical_ops,
        elapsed_ns,
        txn_per_sec,
        ops_per_sec,
        aggregate.checksum,
    );
    Ok(())
}

fn transition_phase(phase: &AtomicU8, from: u8, to: u8, context: &str) -> Result<(), String> {
    phase
        .compare_exchange(from, to, Ordering::AcqRel, Ordering::Acquire)
        .map(|_| ())
        .map_err(|observed| format!("worker failed {context}; phase={observed}"))
}

#[allow(clippy::too_many_arguments)]
fn run_worker(
    thread_id: usize,
    config: Config,
    runtime: &Arc<Runtime>,
    array: &TxnArray<u64>,
    phase: &AtomicU8,
    quiesced: &AtomicUsize,
    ready: &Barrier,
) -> Result<WorkerStats, String> {
    let worker = runtime
        .attach()
        .map_err(|error| format!("attach STO worker {thread_id}: {error:?}"));
    ready.wait();
    let mut worker = worker?;

    let initial_state = worker_random_state(config.seed, thread_id);
    let mut random = SplitMix64::new(initial_state);
    let mut previous_phase = PHASE_WAIT;
    let mut reported_quiescence = false;
    let mut operations = Vec::with_capacity(config.ops_per_txn);
    let mut stats = WorkerStats::default();

    loop {
        let current_phase = phase.load(Ordering::Acquire);
        match current_phase {
            PHASE_WAIT => {
                std::hint::spin_loop();
                continue;
            }
            PHASE_QUIESCE => {
                if !reported_quiescence {
                    quiesced.fetch_add(1, Ordering::Release);
                    reported_quiescence = true;
                }
                previous_phase = PHASE_QUIESCE;
                thread::yield_now();
                continue;
            }
            PHASE_STOP | PHASE_FAILED => break,
            PHASE_MEASURE if previous_phase == PHASE_QUIESCE => {
                random = SplitMix64::new(initial_state);
            }
            PHASE_WARMUP | PHASE_MEASURE => {}
            _ => return Err(format!("worker {thread_id} observed invalid phase")),
        }
        previous_phase = current_phase;
        reported_quiescence = false;

        materialize_transaction(&mut random, config, &mut operations);
        let mut logical_attempts = 0_u64;
        let mut logical_aborts = 0_u64;
        let committed_checksum = loop {
            if matches!(phase.load(Ordering::Acquire), PHASE_STOP | PHASE_FAILED) {
                break None;
            }
            logical_attempts = logical_attempts.wrapping_add(1);
            match attempt_transaction(array, &mut worker, &operations)? {
                AttemptOutcome::Committed(checksum) => break Some(checksum),
                AttemptOutcome::Retry => {
                    logical_aborts = logical_aborts.wrapping_add(1);
                }
            }
        };

        let Some(committed_checksum) = committed_checksum else {
            break;
        };
        if current_phase == PHASE_MEASURE && phase.load(Ordering::Acquire) == PHASE_MEASURE {
            stats.commits = stats.commits.wrapping_add(1);
            stats.attempts = stats.attempts.wrapping_add(logical_attempts);
            stats.aborts = stats.aborts.wrapping_add(logical_aborts);
            stats.logical_ops = stats
                .logical_ops
                .wrapping_add(u64::try_from(operations.len()).unwrap_or(u64::MAX));
            stats.checksum = stats.checksum.wrapping_add(committed_checksum);
        }
    }
    Ok(stats)
}

fn materialize_transaction(
    random: &mut SplitMix64,
    config: Config,
    operations: &mut Vec<Operation>,
) {
    operations.clear();
    for _ in 0..config.ops_per_txn {
        let mut key_number = random.next_u64() % config.keyspace;
        while operations
            .iter()
            .any(|operation| operation.index == key_number as usize)
        {
            key_number = key_number.wrapping_add(1) % config.keyspace;
        }
        let write = random.next_u64() % 100 < u64::from(config.write_percent);
        operations.push(Operation {
            index: key_number as usize,
            write,
        });
    }
}

fn attempt_transaction(
    array: &TxnArray<u64>,
    worker: &mut WorkerContext,
    operations: &[Operation],
) -> Result<AttemptOutcome, String> {
    let mut transaction = worker
        .begin()
        .map_err(|error| format!("begin timed transaction: {error:?}"))?;
    let mut checksum = 0_u64;

    for operation in operations {
        let current = match array.get(&mut transaction, operation.index) {
            Ok(Ok(value)) => value,
            Ok(Err(error)) => {
                transaction.abort();
                return Err(format!("timed read escaped configured bounds: {error}"));
            }
            Err(AccessError::Conflict(_)) => {
                transaction.abort();
                return Ok(AttemptOutcome::Retry);
            }
            Err(error) => {
                transaction.abort();
                return Err(format!("timed read failed: {error:?}"));
            }
        };
        checksum = checksum.wrapping_add(current);

        if operation.write {
            match array.set(&mut transaction, operation.index, current.wrapping_add(1)) {
                Ok(Ok(())) => {}
                Ok(Err(error)) => {
                    transaction.abort();
                    return Err(format!("timed write escaped configured bounds: {error}"));
                }
                Err(AccessError::Conflict(_)) => {
                    transaction.abort();
                    return Ok(AttemptOutcome::Retry);
                }
                Err(error) => {
                    transaction.abort();
                    return Err(format!("timed write failed: {error:?}"));
                }
            }
        }
    }

    match transaction
        .commit()
        .map_err(|error| format!("timed commit failed: {error:?}"))?
    {
        CommitOutcome::Committed(_) => Ok(AttemptOutcome::Committed(checksum)),
        CommitOutcome::Aborted(AbortReason::Conflict(_)) => Ok(AttemptOutcome::Retry),
        CommitOutcome::Aborted(reason) => Err(format!("timed transaction aborted: {reason:?}")),
    }
}

fn join_worker_errors(
    handles: Vec<thread::JoinHandle<Result<WorkerStats, String>>>,
    fallback: &str,
) -> Result<(), String> {
    let mut first_error = None;
    for handle in handles {
        match handle.join() {
            Ok(Ok(_)) => {}
            Ok(Err(error)) if first_error.is_none() => first_error = Some(error),
            Err(_) if first_error.is_none() => {
                first_error = Some("benchmark worker panicked".to_owned());
            }
            _ => {}
        }
    }
    Err(first_error.unwrap_or_else(|| fallback.to_owned()))
}
