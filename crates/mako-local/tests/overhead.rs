#![cfg(have_mako)]

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;
use std::fs::{self, OpenOptions};
use std::io::{Read, Write as _};
use std::path::{Path, PathBuf};
use std::process::{Command, ExitStatus, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Barrier};
use std::time::{Duration, Instant};

use mako_local::{Error, LocalDb, Table};

const PROTOCOL: &str = "mako-local-overhead-v1";
const REQUIRED_ENV: &str = "MAKO_LOCAL_REQUIRE_OVERHEAD";
const DRIVER_ENV: &str = "MAKO_LOCAL_OVERHEAD_DRIVER";
const SAFE_ROLE_ENV: &str = "MAKO_LOCAL_OVERHEAD_SAFE_ROLE";
const SAFE_OUTPUT_ENV: &str = "MAKO_LOCAL_OVERHEAD_SAFE_OUTPUT";
const ARTIFACT_ENV: &str = "MAKO_LOCAL_OVERHEAD_ARTIFACT_DIR";

const WORKERS: usize = 4;
const REPETITIONS: usize = 7;
const WARMUP_KEY_TOUCHES: usize = 2048;
const SAMPLE_KEY_TOUCHES: usize = 8192;
const MINIMUM_WARMUP_TRANSACTIONS: usize = 64;
const MINIMUM_SAMPLE_TRANSACTIONS: usize = 256;
const LOW_CONTENTION_WINDOWS: usize = 64;
const RETRY_LIMIT_MULTIPLIER: u64 = 1000;
const FORCED_COLLISION_ATTEMPTS: u64 = 512;
const FIRST_TABLE_ID: u64 = 81_000;
const CHILD_TIMEOUT: Duration = Duration::from_secs(300);
const CONFIGURATION_COUNT: usize = 3 * 4 * 2;
// Each surface has its own process: one main worker plus four new workers for
// each matrix configuration. This bounded 97-ID experiment is deliberately
// separate from Item 4's fixed-worker reuse/progress gate.
const LIFETIME_WORKER_IDS: usize = 1 + CONFIGURATION_COUNT * WORKERS;
const _: [(); 97] = [(); LIFETIME_WORKER_IDS];
const MINIMUM_HIGH_CONFLICT_RATE: f64 = 0.01;

// These are deliberately broad opt-in relative sanity ceilings, not a release
// throughput SLA. Every low-contention configuration first takes the median
// of seven samples; the gate checks each ratio plus per-workload median/max.
const ABI_OVER_DIRECT_SANITY_CEILING: f64 = 6.0;
const FAST_OVER_DIRECT_SANITY_CEILING: f64 = 6.0;
const SAFE_OVER_ABI_SANITY_CEILING: f64 = 6.0;
const TRUSTED_OVER_FAST_SANITY_CEILING: f64 = 6.0;
const TRUSTED_OVER_DIRECT_SANITY_CEILING: f64 = 6.0;

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum Surface {
    Direct,
    Abi,
    Fast,
    Safe,
    Trusted,
}

impl Surface {
    fn name(self) -> &'static str {
        match self {
            Self::Direct => "direct",
            Self::Abi => "abi",
            Self::Fast => "fast",
            Self::Safe => "safe",
            Self::Trusted => "trusted",
        }
    }

    fn parse(value: &str) -> Result<Self, String> {
        match value {
            "direct" => Ok(Self::Direct),
            "abi" => Ok(Self::Abi),
            "fast" => Ok(Self::Fast),
            "safe" => Ok(Self::Safe),
            "trusted" => Ok(Self::Trusted),
            _ => Err(format!("unknown benchmark surface {value:?}")),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum Workload {
    Read,
    Write,
    Rmw,
}

impl Workload {
    fn name(self) -> &'static str {
        match self {
            Self::Read => "read",
            Self::Write => "write",
            Self::Rmw => "rmw",
        }
    }

    fn parse(value: &str) -> Result<Self, String> {
        match value {
            "read" => Ok(Self::Read),
            "write" => Ok(Self::Write),
            "rmw" => Ok(Self::Rmw),
            _ => Err(format!("unknown benchmark workload {value:?}")),
        }
    }

    fn calls_per_key(self) -> u64 {
        match self {
            Self::Read | Self::Write => 1,
            Self::Rmw => 2,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum Contention {
    Low,
    High,
}

impl Contention {
    fn name(self) -> &'static str {
        match self {
            Self::Low => "low",
            Self::High => "high",
        }
    }

    fn parse(value: &str) -> Result<Self, String> {
        match value {
            "low" => Ok(Self::Low),
            "high" => Ok(Self::High),
            _ => Err(format!("unknown contention profile {value:?}")),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
struct Configuration {
    workload: Workload,
    transaction_size: usize,
    contention: Contention,
    ordinal: usize,
}

#[derive(Debug, Clone, Copy, Default)]
struct BatchStats {
    commits: u64,
    conflicts: u64,
    logical_operations: u64,
}

#[derive(Debug, Clone, Copy)]
struct Sample {
    configuration: Configuration,
    duration_ns: u64,
    stats: BatchStats,
    validated_keys: usize,
    final_sum: u64,
}

#[derive(Debug)]
struct ParsedRun {
    surface: Surface,
    samples: Vec<Sample>,
}

#[derive(Debug)]
struct ChildOutput {
    status: ExitStatus,
    stdout: Vec<u8>,
    stderr: Vec<u8>,
}

fn configurations() -> Vec<Configuration> {
    let mut result = Vec::new();
    let mut ordinal = 0;
    for workload in [Workload::Read, Workload::Write, Workload::Rmw] {
        for transaction_size in [1, 4, 16, 64] {
            for contention in [Contention::Low, Contention::High] {
                result.push(Configuration {
                    workload,
                    transaction_size,
                    contention,
                    ordinal,
                });
                ordinal += 1;
            }
        }
    }
    result
}

fn transactions_for(transaction_size: usize, warmup: bool) -> usize {
    let key_touches = if warmup {
        WARMUP_KEY_TOUCHES
    } else {
        SAMPLE_KEY_TOUCHES
    };
    let minimum = if warmup {
        MINIMUM_WARMUP_TRANSACTIONS
    } else {
        MINIMUM_SAMPLE_TRANSACTIONS
    };
    minimum.max(key_touches / transaction_size)
}

fn key_count(configuration: Configuration) -> usize {
    match configuration.contention {
        Contention::High => configuration.transaction_size,
        Contention::Low => WORKERS * configuration.transaction_size * LOW_CONTENTION_WINDOWS,
    }
}

fn selected_key(
    configuration: Configuration,
    worker: usize,
    successful_transaction: u64,
    item: usize,
) -> u64 {
    match configuration.contention {
        Contention::High => item as u64,
        Contention::Low => {
            let window = configuration.transaction_size * LOW_CONTENTION_WINDOWS;
            (worker * window
                + (successful_transaction as usize * configuration.transaction_size + item)
                    % window) as u64
        }
    }
}

fn key_bytes(key: u64) -> [u8; 8] {
    key.to_be_bytes()
}

fn value_bytes(value: u64) -> [u8; 8] {
    value.to_le_bytes()
}

fn decode_value(bytes: &[u8]) -> Result<u64, String> {
    let bytes: [u8; 8] = bytes
        .try_into()
        .map_err(|_| "benchmark read a malformed counter value".to_owned())?;
    Ok(u64::from_le_bytes(bytes))
}

fn seed_table(db: &LocalDb, table: &Table<'_>, count: usize) -> Result<(), String> {
    const BATCH_SIZE: usize = 64;
    for base in (0..count).step_by(BATCH_SIZE) {
        let mut transaction = db.transaction().map_err(|error| error.to_string())?;
        for key in base..count.min(base + BATCH_SIZE) {
            transaction
                .put(table, &key_bytes(key as u64), &value_bytes(0))
                .map_err(|error| error.to_string())?;
        }
        transaction.commit().map_err(|error| error.to_string())?;
    }
    Ok(())
}

enum Attempt {
    Committed { logical_operations: u64 },
    Conflict { logical_operations: u64 },
}

fn collision_rendezvous(barrier: Option<&Barrier>) {
    if let Some(barrier) = barrier {
        barrier.wait();
    }
}

fn run_attempt(
    db: &LocalDb,
    table: &Table<'_>,
    configuration: Configuration,
    worker: usize,
    successful_transaction: u64,
    collision_barrier: Option<&Barrier>,
    trusted: bool,
) -> Result<Attempt, String> {
    let mut transaction = if trusted {
        db.trusted_transaction(table)
    } else {
        db.transaction()
    }
    .map_err(|error| error.to_string())?;
    let mut logical_operations = 0;
    for item in 0..configuration.transaction_size {
        let key = key_bytes(selected_key(
            configuration,
            worker,
            successful_transaction,
            item,
        ));
        let mut value = 0;
        if configuration.workload != Workload::Write {
            match transaction.get(table, &key) {
                Ok(Some(bytes)) => {
                    value = decode_value(&bytes)?;
                    logical_operations += 1;
                }
                Ok(None) => return Err("safe benchmark key unexpectedly missing".to_owned()),
                Err(Error::Conflict) => {
                    collision_rendezvous(collision_barrier);
                    return Ok(Attempt::Conflict { logical_operations });
                }
                Err(error) => return Err(format!("safe benchmark get failed: {error}")),
            }
        }
        if configuration.workload != Workload::Read {
            let next = if configuration.workload == Workload::Rmw {
                value
                    .checked_add(1)
                    .ok_or_else(|| "benchmark counter overflow".to_owned())?
            } else {
                1
            };
            match transaction.put(table, &key, &value_bytes(next)) {
                Ok(_) => logical_operations += 1,
                Err(Error::Conflict) => {
                    collision_rendezvous(collision_barrier);
                    return Ok(Attempt::Conflict { logical_operations });
                }
                Err(error) => return Err(format!("safe benchmark put failed: {error}")),
            }
        }
    }
    collision_rendezvous(collision_barrier);
    match transaction.commit() {
        Ok(()) => Ok(Attempt::Committed { logical_operations }),
        Err(Error::Conflict) => Ok(Attempt::Conflict { logical_operations }),
        Err(error) => Err(format!("safe benchmark commit failed: {error}")),
    }
}

fn run_batch(
    db: &LocalDb,
    table: &Table<'_>,
    configuration: Configuration,
    worker: usize,
    target_commits: u64,
    collision_barrier: Option<&Barrier>,
    trusted: bool,
) -> Result<BatchStats, String> {
    let mut stats = BatchStats::default();
    let retry_limit = (target_commits + 1).max(target_commits * RETRY_LIMIT_MULTIPLIER);
    while stats.commits != target_commits {
        let attempts = stats.commits + stats.conflicts;
        if attempts >= retry_limit {
            return Err("safe benchmark exceeded its conflict retry budget".to_owned());
        }
        // Keep a bounded prefix of hot write/RMW attempts concurrent even on
        // a single-core scheduler. High-contention timings are diagnostic.
        let synchronized_collision =
            collision_barrier.filter(|_| attempts < target_commits.min(FORCED_COLLISION_ATTEMPTS));
        collision_rendezvous(synchronized_collision);
        match run_attempt(
            db,
            table,
            configuration,
            worker,
            stats.commits,
            synchronized_collision,
            trusted,
        )? {
            Attempt::Committed { logical_operations } => {
                stats.commits += 1;
                stats.logical_operations += logical_operations;
            }
            Attempt::Conflict { logical_operations } => {
                stats.conflicts += 1;
                stats.logical_operations += logical_operations;
                std::thread::yield_now();
            }
        }
    }
    Ok(stats)
}

fn run_concurrent(
    db: &Arc<LocalDb>,
    table_name: &[u8],
    table_id: u64,
    configuration: Configuration,
    trusted: bool,
) -> Result<Vec<(Duration, BatchStats)>, String> {
    let phases = 1 + REPETITIONS;
    let barrier = Arc::new(Barrier::new(WORKERS + 1));
    let collision_barrier = Arc::new(Barrier::new(WORKERS));
    let mut workers = Vec::with_capacity(WORKERS);

    for worker_index in 0..WORKERS {
        let db = Arc::clone(db);
        let barrier = Arc::clone(&barrier);
        let collision_barrier = Arc::clone(&collision_barrier);
        let table_name = table_name.to_vec();
        workers.push(std::thread::spawn(move || {
            let table = db
                .open_table(&table_name, table_id)
                .map_err(|error| format!("safe benchmark table reopen failed: {error}"));
            let mut results = vec![BatchStats::default(); phases];
            let mut failure = table.as_ref().err().cloned();

            for (phase, phase_result) in results.iter_mut().enumerate() {
                barrier.wait();
                if failure.is_none() {
                    let result = run_batch(
                        &db,
                        table.as_ref().expect("table checked above"),
                        configuration,
                        worker_index,
                        transactions_for(configuration.transaction_size, phase == 0) as u64,
                        (configuration.contention == Contention::High
                            && configuration.workload != Workload::Read)
                            .then_some(collision_barrier.as_ref()),
                        trusted,
                    );
                    match result {
                        Ok(stats) => *phase_result = stats,
                        Err(error) => failure = Some(error),
                    }
                }
                barrier.wait();
            }
            (results, failure)
        }));
    }

    let mut durations = Vec::with_capacity(phases);
    for _ in 0..phases {
        let start = Instant::now();
        barrier.wait();
        barrier.wait();
        durations.push(start.elapsed().max(Duration::from_nanos(1)));
    }

    let mut totals = vec![BatchStats::default(); phases];
    for (worker_index, worker) in workers.into_iter().enumerate() {
        let (results, failure) = worker
            .join()
            .map_err(|_| format!("safe benchmark worker {worker_index} panicked"))?;
        if let Some(failure) = failure {
            return Err(format!(
                "safe benchmark worker {worker_index} failed: {failure}"
            ));
        }
        for (total, result) in totals.iter_mut().zip(results) {
            total.commits += result.commits;
            total.conflicts += result.conflicts;
            total.logical_operations += result.logical_operations;
        }
    }

    Ok(durations.into_iter().zip(totals).skip(1).collect())
}

fn expected_configured_value(configuration: Configuration, key_index: usize) -> u64 {
    match configuration.workload {
        Workload::Read => 0,
        Workload::Write => 1,
        Workload::Rmw => {
            let warmup_transactions = transactions_for(configuration.transaction_size, true);
            let sample_transactions = transactions_for(configuration.transaction_size, false);
            if configuration.contention == Contention::High {
                return (WORKERS * (warmup_transactions + REPETITIONS * sample_transactions))
                    as u64;
            }
            let window = configuration.transaction_size * LOW_CONTENTION_WINDOWS;
            let local_key = key_index % window;
            let phase_hits = |transactions: usize| {
                let touches = transactions * configuration.transaction_size;
                touches / window + usize::from(local_key < touches % window)
            };
            (phase_hits(warmup_transactions) + REPETITIONS * phase_hits(sample_transactions)) as u64
        }
    }
}

fn validate_table(
    db: &LocalDb,
    table: &Table<'_>,
    configuration: Configuration,
) -> Result<u64, String> {
    const BATCH_SIZE: usize = 128;
    let count = key_count(configuration);
    let mut sum = 0u64;
    for base in (0..count).step_by(BATCH_SIZE) {
        let mut transaction = db.transaction().map_err(|error| error.to_string())?;
        for key in base..count.min(base + BATCH_SIZE) {
            let value = transaction
                .get(table, &key_bytes(key as u64))
                .map_err(|error| error.to_string())?
                .ok_or_else(|| "safe benchmark validation found a missing key".to_owned())?;
            let value = decode_value(&value)?;
            let expected = expected_configured_value(configuration, key);
            if value != expected {
                return Err(format!(
                    "safe benchmark configured-key value mismatch at key {key}: expected {expected}, found {value}"
                ));
            }
            sum = sum.wrapping_add(value);
        }
        transaction.commit().map_err(|error| error.to_string())?;
    }

    Ok(sum)
}

fn emit_sample(
    output: &mut String,
    surface: Surface,
    configuration: Configuration,
    index: usize,
    duration: Duration,
    stats: BatchStats,
    final_sum: u64,
) {
    writeln!(
        output,
        "sample mode {} workload {} size {} contention {} index {} duration_ns {} commits {} conflicts {} logical_ops {} validated_keys {} final_sum {}",
        surface.name(),
        configuration.workload.name(),
        configuration.transaction_size,
        configuration.contention.name(),
        index,
        u64::try_from(duration.as_nanos()).unwrap_or(u64::MAX),
        stats.commits,
        stats.conflicts,
        stats.logical_operations,
        key_count(configuration),
        final_sum,
    )
    .expect("writing to String cannot fail");
}

fn run_safe_surface(surface: Surface) -> Result<String, String> {
    if !matches!(surface, Surface::Safe | Surface::Trusted) {
        return Err(format!(
            "Rust benchmark role does not support surface {}",
            surface.name()
        ));
    }
    let trusted = surface == Surface::Trusted;
    let db = Arc::new(LocalDb::open().map_err(|error| error.to_string())?);
    let mut output = String::new();
    writeln!(&mut output, "{PROTOCOL}").unwrap();
    writeln!(
        &mut output,
        "meta mode {} workers {WORKERS} warmup_key_touches {WARMUP_KEY_TOUCHES} sample_key_touches {SAMPLE_KEY_TOUCHES} repetitions {REPETITIONS} lifetime_worker_ids {LIFETIME_WORKER_IDS}",
        surface.name(),
    )
    .unwrap();

    for configuration in configurations() {
        let table_name = format!("mako-local-overhead-{}", configuration.ordinal).into_bytes();
        let table_id = FIRST_TABLE_ID + configuration.ordinal as u64;
        let table = db
            .open_table(&table_name, table_id)
            .map_err(|error| error.to_string())?;
        seed_table(&db, &table, key_count(configuration))?;
        let samples = run_concurrent(&db, &table_name, table_id, configuration, trusted)?;
        let final_sum = validate_table(&db, &table, configuration)?;
        for (index, (duration, stats)) in samples.into_iter().enumerate() {
            emit_sample(
                &mut output,
                surface,
                configuration,
                index,
                duration,
                stats,
                final_sum,
            );
        }
    }
    writeln!(&mut output, "end mode {}", surface.name()).unwrap();
    Ok(output)
}

fn canonical_usize(token: &str, label: &str) -> Result<usize, String> {
    let value: usize = token
        .parse()
        .map_err(|_| format!("invalid {label} {token:?}"))?;
    if value.to_string() != token {
        return Err(format!("noncanonical {label} {token:?}"));
    }
    Ok(value)
}

fn canonical_u64(token: &str, label: &str) -> Result<u64, String> {
    let value: u64 = token
        .parse()
        .map_err(|_| format!("invalid {label} {token:?}"))?;
    if value.to_string() != token {
        return Err(format!("noncanonical {label} {token:?}"));
    }
    Ok(value)
}

fn canonical_parts(line: &str, number: usize) -> Result<Vec<&str>, String> {
    let parts: Vec<_> = line.split_ascii_whitespace().collect();
    if parts.is_empty() || parts.join(" ") != line {
        return Err(format!("line {number}: noncanonical whitespace"));
    }
    Ok(parts)
}

fn maximum_sample_conflicts(configuration: Configuration) -> u64 {
    let target_commits = transactions_for(configuration.transaction_size, false) as u64;
    let retry_limit = (target_commits + 1).max(target_commits * RETRY_LIMIT_MULTIPLIER);
    WORKERS as u64 * (retry_limit - target_commits)
}

fn aggregate_conflict_rate(
    samples: &[Sample],
    configuration: Configuration,
) -> Result<(u64, u64, f64), String> {
    let (commits, conflicts) = samples
        .iter()
        .filter(|sample| sample.configuration == configuration)
        .try_fold((0u64, 0u64), |(commits, conflicts), sample| {
            Ok::<_, String>((
                commits
                    .checked_add(sample.stats.commits)
                    .ok_or_else(|| "benchmark commit total overflowed u64".to_owned())?,
                conflicts
                    .checked_add(sample.stats.conflicts)
                    .ok_or_else(|| "benchmark conflict total overflowed u64".to_owned())?,
            ))
        })?;
    let attempts = commits
        .checked_add(conflicts)
        .ok_or_else(|| "benchmark attempt total overflowed u64".to_owned())?;
    let rate = if attempts == 0 {
        0.0
    } else {
        conflicts as f64 / attempts as f64
    };
    if !rate.is_finite() || !(0.0..=1.0).contains(&rate) {
        return Err(format!(
            "benchmark produced an invalid conflict rate {rate}"
        ));
    }
    Ok((commits, conflicts, rate))
}

fn parse_run(text: String, expected_surface: Surface) -> Result<ParsedRun, String> {
    if text.contains('\r') {
        return Err("benchmark output contains noncanonical carriage returns".to_owned());
    }
    let lines: Vec<_> = text.lines().collect();
    if lines.first().copied() != Some(PROTOCOL) {
        return Err(format!("benchmark output is missing {PROTOCOL:?}"));
    }
    if lines.len() < 3 {
        return Err("benchmark output is truncated".to_owned());
    }
    let meta = canonical_parts(lines[1], 2)?;
    if meta.len() != 13
        || meta[0] != "meta"
        || meta[1] != "mode"
        || meta[3] != "workers"
        || meta[5] != "warmup_key_touches"
        || meta[7] != "sample_key_touches"
        || meta[9] != "repetitions"
        || meta[11] != "lifetime_worker_ids"
    {
        return Err("line 2: malformed benchmark metadata".to_owned());
    }
    let surface = Surface::parse(meta[2])?;
    if surface != expected_surface {
        return Err(format!(
            "benchmark surface mismatch: expected {}, found {}",
            expected_surface.name(),
            surface.name()
        ));
    }
    if canonical_usize(meta[4], "worker count")? != WORKERS
        || canonical_usize(meta[6], "warmup key touches")? != WARMUP_KEY_TOUCHES
        || canonical_usize(meta[8], "sample key touches")? != SAMPLE_KEY_TOUCHES
        || canonical_usize(meta[10], "repetitions")? != REPETITIONS
        || canonical_usize(meta[12], "lifetime worker IDs")? != LIFETIME_WORKER_IDS
    {
        return Err("benchmark metadata disagrees with the stable workload".to_owned());
    }

    let expected_configurations = configurations();
    let configuration_by_key: BTreeMap<_, _> = expected_configurations
        .iter()
        .map(|configuration| {
            (
                (
                    configuration.workload,
                    configuration.transaction_size,
                    configuration.contention,
                ),
                *configuration,
            )
        })
        .collect();
    let mut seen = BTreeSet::new();
    let mut samples = Vec::new();

    for (zero_line, line) in lines[2..lines.len() - 1].iter().enumerate() {
        let number = zero_line + 3;
        let parts = canonical_parts(line, number)?;
        if parts.len() != 23
            || parts[0] != "sample"
            || parts[1] != "mode"
            || parts[3] != "workload"
            || parts[5] != "size"
            || parts[7] != "contention"
            || parts[9] != "index"
            || parts[11] != "duration_ns"
            || parts[13] != "commits"
            || parts[15] != "conflicts"
            || parts[17] != "logical_ops"
            || parts[19] != "validated_keys"
            || parts[21] != "final_sum"
        {
            return Err(format!("line {number}: malformed sample"));
        }
        if Surface::parse(parts[2])? != surface {
            return Err(format!("line {number}: mixed benchmark surfaces"));
        }
        let workload = Workload::parse(parts[4])?;
        let transaction_size = canonical_usize(parts[6], "transaction size")?;
        let contention = Contention::parse(parts[8])?;
        let configuration = *configuration_by_key
            .get(&(workload, transaction_size, contention))
            .ok_or_else(|| format!("line {number}: unknown benchmark configuration"))?;
        let index = canonical_usize(parts[10], "sample index")?;
        if index >= REPETITIONS || !seen.insert((configuration, index)) {
            return Err(format!("line {number}: duplicate or invalid sample index"));
        }
        let sample = Sample {
            configuration,
            duration_ns: canonical_u64(parts[12], "duration")?,
            stats: BatchStats {
                commits: canonical_u64(parts[14], "commit count")?,
                conflicts: canonical_u64(parts[16], "conflict count")?,
                logical_operations: canonical_u64(parts[18], "logical operation count")?,
            },
            validated_keys: canonical_usize(parts[20], "validated key count")?,
            final_sum: canonical_u64(parts[22], "final checksum")?,
        };
        validate_sample(sample, number)?;
        samples.push(sample);
    }

    let end = canonical_parts(lines.last().expect("checked nonempty"), lines.len())?;
    if end.as_slice() != ["end", "mode", surface.name()] {
        return Err("malformed benchmark end marker".to_owned());
    }
    if seen.len() != expected_configurations.len() * REPETITIONS {
        return Err(format!(
            "benchmark matrix is incomplete: expected {} samples, found {}",
            expected_configurations.len() * REPETITIONS,
            seen.len()
        ));
    }
    for configuration in expected_configurations
        .iter()
        .copied()
        .filter(|configuration| configuration.contention == Contention::High)
    {
        let (_, conflicts, rate) = aggregate_conflict_rate(&samples, configuration)?;
        if configuration.workload == Workload::Read {
            if conflicts != 0 {
                return Err(format!(
                    "{} shared-key read-only size {} unexpectedly reported {conflicts} conflicts",
                    surface.name(),
                    configuration.transaction_size
                ));
            }
        } else if rate < MINIMUM_HIGH_CONFLICT_RATE {
            return Err(format!(
                "{} high-contention {} size {} conflict rate {rate:.6} is below the required {MINIMUM_HIGH_CONFLICT_RATE:.6}",
                surface.name(),
                configuration.workload.name(),
                configuration.transaction_size
            ));
        }
    }
    Ok(ParsedRun { surface, samples })
}

fn validate_sample(sample: Sample, line: usize) -> Result<(), String> {
    if sample.duration_ns == 0 {
        return Err(format!("line {line}: zero-duration sample"));
    }
    let configuration = sample.configuration;
    let expected_commits =
        (WORKERS * transactions_for(configuration.transaction_size, false)) as u64;
    if sample.stats.commits != expected_commits {
        return Err(format!(
            "line {line}: expected {expected_commits} commits, found {}",
            sample.stats.commits
        ));
    }
    let minimum_operations = expected_commits
        * configuration.transaction_size as u64
        * configuration.workload.calls_per_key();
    if sample.stats.logical_operations < minimum_operations {
        return Err(format!(
            "line {line}: successful work requires at least {minimum_operations} operations"
        ));
    }
    if configuration.contention == Contention::Low && sample.stats.conflicts != 0 {
        return Err(format!(
            "line {line}: disjoint low-contention workers reported {} conflicts",
            sample.stats.conflicts
        ));
    }
    let maximum_conflicts = maximum_sample_conflicts(configuration);
    if sample.stats.conflicts > maximum_conflicts {
        return Err(format!(
            "line {line}: conflict count {} exceeds the runner retry bound {maximum_conflicts}",
            sample.stats.conflicts
        ));
    }
    let expected_keys = key_count(configuration);
    if sample.validated_keys != expected_keys {
        return Err(format!(
            "line {line}: expected {expected_keys} configured keys to be validated, found {}",
            sample.validated_keys
        ));
    }
    // `validated_keys` counts the configured key set checked value-by-value;
    // it does not claim that an unexpected-key scan proved table cardinality.
    let expected_sum = (0..expected_keys).fold(0u64, |sum, key| {
        sum.wrapping_add(expected_configured_value(configuration, key))
    });
    if sample.final_sum != expected_sum {
        return Err(format!(
            "line {line}: expected final checksum {expected_sum}, found {}",
            sample.final_sum
        ));
    }
    Ok(())
}

fn run_child(mut command: Command, label: &str) -> Result<ChildOutput, String> {
    command.stdout(Stdio::piped()).stderr(Stdio::piped());
    let mut child = command
        .spawn()
        .map_err(|error| format!("cannot start {label}: {error}"))?;
    let mut stdout = child
        .stdout
        .take()
        .ok_or_else(|| format!("cannot capture {label} stdout"))?;
    let mut stderr = child
        .stderr
        .take()
        .ok_or_else(|| format!("cannot capture {label} stderr"))?;
    let stdout_reader = std::thread::spawn(move || {
        let mut bytes = Vec::new();
        let result = stdout.read_to_end(&mut bytes);
        (result, bytes)
    });
    let stderr_reader = std::thread::spawn(move || {
        let mut bytes = Vec::new();
        let result = stderr.read_to_end(&mut bytes);
        (result, bytes)
    });

    let deadline = Instant::now() + CHILD_TIMEOUT;
    let status = loop {
        if let Some(status) = child
            .try_wait()
            .map_err(|error| format!("cannot poll {label}: {error}"))?
        {
            break status;
        }
        if Instant::now() >= deadline {
            let _ = child.kill();
            let _ = child.wait();
            let _ = stdout_reader.join();
            let _ = stderr_reader.join();
            return Err(format!("{label} exceeded the {CHILD_TIMEOUT:?} timeout"));
        }
        std::thread::sleep(Duration::from_millis(10));
    };
    let (stdout_result, stdout) = stdout_reader
        .join()
        .map_err(|_| format!("{label} stdout reader panicked"))?;
    stdout_result.map_err(|error| format!("cannot read {label} stdout: {error}"))?;
    let (stderr_result, stderr) = stderr_reader
        .join()
        .map_err(|_| format!("{label} stderr reader panicked"))?;
    stderr_result.map_err(|error| format!("cannot read {label} stderr: {error}"))?;
    Ok(ChildOutput {
        status,
        stdout,
        stderr,
    })
}

fn checked_text(output: ChildOutput, label: &str) -> Result<String, String> {
    let stdout = String::from_utf8(output.stdout)
        .map_err(|error| format!("{label} stdout is not UTF-8: {error}"))?;
    let stderr = String::from_utf8_lossy(&output.stderr);
    if !output.status.success() {
        return Err(format!(
            "{label} exited with {}\nstdout:\n{stdout}\nstderr:\n{stderr}",
            output.status
        ));
    }
    if !stderr.trim().is_empty() {
        return Err(format!("{label} wrote unexpected stderr:\n{stderr}"));
    }
    Ok(stdout)
}

fn temporary_directory() -> Result<PathBuf, String> {
    static NEXT: AtomicU64 = AtomicU64::new(0);
    let base = std::env::var_os(ARTIFACT_ENV)
        .map(PathBuf::from)
        .unwrap_or_else(std::env::temp_dir);
    fs::create_dir_all(&base)
        .map_err(|error| format!("cannot create benchmark artifact base {base:?}: {error}"))?;
    for _ in 0..1000 {
        let ordinal = NEXT.fetch_add(1, Ordering::Relaxed);
        let directory = base.join(format!(
            "mako-local-overhead-{}-{ordinal}",
            std::process::id()
        ));
        match fs::create_dir(&directory) {
            Ok(()) => return Ok(directory),
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(error) => {
                return Err(format!(
                    "cannot create benchmark artifact directory {directory:?}: {error}"
                ));
            }
        }
    }
    Err("could not allocate a unique benchmark artifact directory".to_owned())
}

fn run_driver(driver: &Path, surface: Surface) -> Result<String, String> {
    let mut command = Command::new(driver);
    command.arg(surface.name());
    checked_text(
        run_child(command, &format!("{} benchmark", surface.name()))?,
        &format!("{} benchmark", surface.name()),
    )
}

fn run_safe_child(directory: &Path, surface: Surface) -> Result<String, String> {
    if !matches!(surface, Surface::Safe | Surface::Trusted) {
        return Err(format!(
            "cannot run {} through the Rust benchmark role",
            surface.name()
        ));
    }
    let output_path = directory.join(format!("{}.txt", surface.name()));
    let executable = std::env::current_exe()
        .map_err(|error| format!("cannot locate benchmark test executable: {error}"))?;
    let mut command = Command::new(executable);
    command
        .arg("--exact")
        .arg("overhead_safe_role")
        .arg("--nocapture")
        .env(SAFE_ROLE_ENV, surface.name())
        .env(SAFE_OUTPUT_ENV, &output_path);
    let label = format!("{} Rust benchmark", surface.name());
    let child = run_child(command, &label)?;
    if !child.status.success() {
        return Err(format!(
            "{label} exited with {}\nstdout:\n{}\nstderr:\n{}",
            child.status,
            String::from_utf8_lossy(&child.stdout),
            String::from_utf8_lossy(&child.stderr)
        ));
    }
    fs::read_to_string(&output_path)
        .map_err(|error| format!("cannot read {label} output {output_path:?}: {error}"))
}

fn median_u64(mut values: Vec<u64>) -> f64 {
    assert!(!values.is_empty());
    values.sort_unstable();
    if values.len() % 2 == 1 {
        values[values.len() / 2] as f64
    } else {
        let right = values[values.len() / 2] as f64;
        let left = values[values.len() / 2 - 1] as f64;
        (left + right) / 2.0
    }
}

fn median_f64(mut values: Vec<f64>) -> f64 {
    assert!(!values.is_empty());
    values.sort_by(f64::total_cmp);
    if values.len() % 2 == 1 {
        values[values.len() / 2]
    } else {
        (values[values.len() / 2 - 1] + values[values.len() / 2]) / 2.0
    }
}

fn calculate_wrapper_tax(runs: &[ParsedRun]) -> Result<String, String> {
    let observed_order: Vec<_> = runs.iter().map(|run| run.surface).collect();
    if observed_order
        != [
            Surface::Direct,
            Surface::Abi,
            Surface::Fast,
            Surface::Safe,
            Surface::Trusted,
        ]
    {
        return Err(
            "benchmark surfaces must run in fixed direct -> ABI -> fast -> safe -> trusted order"
                .to_owned(),
        );
    }

    let mut medians = BTreeMap::new();
    for run in runs {
        for configuration in configurations() {
            let durations: Vec<_> = run
                .samples
                .iter()
                .filter(|sample| sample.configuration == configuration)
                .map(|sample| sample.duration_ns)
                .collect();
            if durations.len() != REPETITIONS {
                return Err(format!(
                    "{} is missing samples for {:?}",
                    run.surface.name(),
                    configuration
                ));
            }
            medians.insert((run.surface, configuration), median_u64(durations));
        }
    }

    let low: Vec<_> = configurations()
        .into_iter()
        .filter(|configuration| configuration.contention == Contention::Low)
        .collect();
    let mut summary = format!(
        "mako-local-overhead-method-v1 surface_order direct_then_abi_then_fast_then_safe_then_trusted process_isolation one_process_per_surface lifetime_worker_ids_per_surface {LIFETIME_WORKER_IDS} worker_id_scope bounded_matrix_not_fixed_worker_gate budget_kind enforced_opt_in_advisory_sanity_ceiling_not_release_sla\n"
    );

    for run in runs {
        for configuration in configurations()
            .into_iter()
            .filter(|configuration| configuration.contention == Contention::High)
        {
            let (commits, conflicts, rate) = aggregate_conflict_rate(&run.samples, configuration)?;
            let expectation = if configuration.workload == Workload::Read {
                "zero"
            } else {
                "at_least_0.01"
            };
            writeln!(
                &mut summary,
                "mako-local-conflict-rate-v1 mode {} workload {} size {} contention high samples {REPETITIONS} commits {commits} conflicts {conflicts} rate {rate:.6} expectation {expectation}",
                run.surface.name(),
                configuration.workload.name(),
                configuration.transaction_size,
            )
            .unwrap();
        }
    }

    let comparisons = [
        (
            Surface::Abi,
            Surface::Direct,
            ABI_OVER_DIRECT_SANITY_CEILING,
        ),
        (
            Surface::Fast,
            Surface::Direct,
            FAST_OVER_DIRECT_SANITY_CEILING,
        ),
        (Surface::Safe, Surface::Abi, SAFE_OVER_ABI_SANITY_CEILING),
        (
            Surface::Trusted,
            Surface::Fast,
            TRUSTED_OVER_FAST_SANITY_CEILING,
        ),
        (
            Surface::Trusted,
            Surface::Direct,
            TRUSTED_OVER_DIRECT_SANITY_CEILING,
        ),
    ];
    let mut violations = Vec::new();
    for (numerator, denominator, budget) in comparisons {
        let mut all_ratios = Vec::new();
        let mut by_workload: BTreeMap<Workload, Vec<f64>> = BTreeMap::new();
        for configuration in &low {
            let ratio =
                medians[&(numerator, *configuration)] / medians[&(denominator, *configuration)];
            all_ratios.push(ratio);
            by_workload
                .entry(configuration.workload)
                .or_default()
                .push(ratio);
            writeln!(
                &mut summary,
                "mako-local-wrapper-ratio-v1 numerator {} denominator {} workload {} size {} contention low statistic configuration_median ratio {ratio:.6} ceiling {budget:.6}",
                numerator.name(),
                denominator.name(),
                configuration.workload.name(),
                configuration.transaction_size,
            )
            .unwrap();
            if !ratio.is_finite() || ratio > budget {
                violations.push(format!(
                    "{}/{} {} size {} ratio {ratio:.6} exceeds {budget:.6}",
                    numerator.name(),
                    denominator.name(),
                    configuration.workload.name(),
                    configuration.transaction_size,
                ));
            }
        }

        for workload in [Workload::Read, Workload::Write, Workload::Rmw] {
            let workload_ratios = by_workload
                .get(&workload)
                .expect("all low-contention workloads are present");
            let workload_median = median_f64(workload_ratios.clone());
            let workload_maximum = workload_ratios.iter().copied().fold(0.0f64, f64::max);
            writeln!(
                &mut summary,
                "mako-local-wrapper-ratio-v1 numerator {} denominator {} workload {} contention low statistic workload_median ratio {workload_median:.6} ceiling {budget:.6}",
                numerator.name(),
                denominator.name(),
                workload.name(),
            )
            .unwrap();
            writeln!(
                &mut summary,
                "mako-local-wrapper-ratio-v1 numerator {} denominator {} workload {} contention low statistic workload_maximum_configuration_median ratio {workload_maximum:.6} ceiling {budget:.6}",
                numerator.name(),
                denominator.name(),
                workload.name(),
            )
            .unwrap();
            if !workload_median.is_finite() || workload_median > budget {
                violations.push(format!(
                    "{}/{} {} workload median {workload_median:.6} exceeds {budget:.6}",
                    numerator.name(),
                    denominator.name(),
                    workload.name(),
                ));
            }
            if !workload_maximum.is_finite() || workload_maximum > budget {
                violations.push(format!(
                    "{}/{} {} workload maximum {workload_maximum:.6} exceeds {budget:.6}",
                    numerator.name(),
                    denominator.name(),
                    workload.name(),
                ));
            }
        }

        let aggregate_median = median_f64(all_ratios.clone());
        let maximum = all_ratios.into_iter().fold(0.0f64, f64::max);
        writeln!(
            &mut summary,
            "mako-local-wrapper-ratio-v1 numerator {} denominator {} contention low statistic aggregate_median ratio {aggregate_median:.6} ceiling {budget:.6}",
            numerator.name(),
            denominator.name(),
        )
        .unwrap();
        writeln!(
            &mut summary,
            "mako-local-wrapper-ratio-v1 numerator {} denominator {} contention low statistic maximum_configuration_median ratio {maximum:.6} ceiling {budget:.6}",
            numerator.name(),
            denominator.name(),
        )
        .unwrap();
        if !maximum.is_finite() || maximum > budget {
            violations.push(format!(
                "{}/{} maximum configuration median {maximum:.6} exceeds {budget:.6}",
                numerator.name(),
                denominator.name(),
            ));
        }
    }

    if violations.is_empty() {
        Ok(summary)
    } else {
        Err(format!(
            "opt-in relative wrapper-tax advisory sanity ceiling exceeded\n{summary}violations:\n{}",
            violations.join("\n")
        ))
    }
}

fn preserve(
    directory: &Path,
    direct: Option<&str>,
    abi: Option<&str>,
    fast: Option<&str>,
    safe: Option<&str>,
    trusted: Option<&str>,
    summary: Option<&str>,
) -> Result<(), String> {
    for (name, value) in [
        ("direct.txt", direct),
        ("abi.txt", abi),
        ("fast.txt", fast),
        ("safe.txt", safe),
        ("trusted.txt", trusted),
        ("wrapper-tax.txt", summary),
    ] {
        if let Some(value) = value {
            fs::write(directory.join(name), value)
                .map_err(|error| format!("cannot preserve {name}: {error}"))?;
        }
    }
    Ok(())
}

#[test]
fn wrapper_overhead_gate() {
    if std::env::var_os(SAFE_ROLE_ENV).is_some() {
        return;
    }
    let required = std::env::var_os(REQUIRED_ENV).is_some();
    if cfg!(debug_assertions) {
        if required {
            panic!("wrapper-overhead measurements require cargo test --release");
        }
        eprintln!("skipping wrapper-overhead measurements in a debug Rust build");
        return;
    }
    let Some(driver) = std::env::var_os(DRIVER_ENV).map(PathBuf::from) else {
        if required {
            panic!("{DRIVER_ENV} is required but not set");
        }
        eprintln!("skipping native wrapper-overhead gate: {DRIVER_ENV} is not set");
        return;
    };
    assert!(driver.is_file(), "overhead driver is missing: {driver:?}");
    let directory = temporary_directory().expect("create benchmark artifact directory");

    let result = (|| -> Result<(Vec<ParsedRun>, String), String> {
        // Fixed same-host method: direct C++, raw ABI, trusted fast ABI,
        // public safe Rust, then the safe Rust trusted wrapper shipped by
        // mako-cache. calculate_wrapper_tax verifies this order before ratios.
        let direct_text = run_driver(&driver, Surface::Direct)?;
        preserve(&directory, Some(&direct_text), None, None, None, None, None)?;
        let direct = parse_run(direct_text, Surface::Direct)?;

        let abi_text = run_driver(&driver, Surface::Abi)?;
        preserve(&directory, None, Some(&abi_text), None, None, None, None)?;
        let abi = parse_run(abi_text, Surface::Abi)?;

        let fast_text = run_driver(&driver, Surface::Fast)?;
        preserve(&directory, None, None, Some(&fast_text), None, None, None)?;
        let fast = parse_run(fast_text, Surface::Fast)?;

        let safe_text = run_safe_child(&directory, Surface::Safe)?;
        preserve(&directory, None, None, None, Some(&safe_text), None, None)?;
        let safe = parse_run(safe_text, Surface::Safe)?;

        let trusted_text = run_safe_child(&directory, Surface::Trusted)?;
        preserve(
            &directory,
            None,
            None,
            None,
            None,
            Some(&trusted_text),
            None,
        )?;
        let trusted = parse_run(trusted_text, Surface::Trusted)?;

        let runs = vec![direct, abi, fast, safe, trusted];
        let summary = calculate_wrapper_tax(&runs)?;
        preserve(&directory, None, None, None, None, None, Some(&summary))?;
        Ok((runs, summary))
    })();

    match result {
        Ok((runs, summary)) => {
            for run in &runs {
                let high_conflicts: u64 = run
                    .samples
                    .iter()
                    .filter(|sample| {
                        sample.configuration.contention == Contention::High
                            && sample.configuration.workload != Workload::Read
                    })
                    .map(|sample| sample.stats.conflicts)
                    .sum();
                eprintln!(
                    "{} benchmark recorded {high_conflicts} high-contention OCC retries",
                    run.surface.name()
                );
            }
            eprint!("{summary}");
            // Results are reproducibility artifacts only on failure or when an
            // explicit artifact root is requested.
            if std::env::var_os(ARTIFACT_ENV).is_none() {
                let _ = fs::remove_file(directory.join("direct.txt"));
                let _ = fs::remove_file(directory.join("abi.txt"));
                let _ = fs::remove_file(directory.join("fast.txt"));
                let _ = fs::remove_file(directory.join("safe.txt"));
                let _ = fs::remove_file(directory.join("trusted.txt"));
                let _ = fs::remove_file(directory.join("wrapper-tax.txt"));
                let _ = fs::remove_dir(&directory);
            }
        }
        Err(error) => panic!(
            "wrapper-overhead gate failed: {error}\nartifacts retained at {}",
            directory.display()
        ),
    }
}

#[test]
fn overhead_safe_role() {
    let Some(surface) = std::env::var_os(SAFE_ROLE_ENV) else {
        return;
    };
    let surface = Surface::parse(
        surface
            .to_str()
            .expect("Rust benchmark surface must be valid UTF-8"),
    )
    .expect("Rust benchmark surface must be safe or trusted");
    let path = PathBuf::from(
        std::env::var_os(SAFE_OUTPUT_ENV).expect("safe benchmark output path is required"),
    );
    let result = run_safe_surface(surface).expect("run safe Rust overhead benchmark");
    let mut output = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&path)
        .expect("create safe benchmark output exactly once");
    output
        .write_all(result.as_bytes())
        .expect("write safe benchmark output");
}

#[test]
fn overhead_protocol_rejects_incomplete_or_inconsistent_results() {
    let malformed = format!(
        "{PROTOCOL}\nmeta mode safe workers {WORKERS} warmup_key_touches {WARMUP_KEY_TOUCHES} sample_key_touches {SAMPLE_KEY_TOUCHES} repetitions {REPETITIONS} lifetime_worker_ids {LIFETIME_WORKER_IDS}\nend mode safe\n"
    );
    assert!(parse_run(malformed, Surface::Safe).is_err());
}

#[test]
fn overhead_protocol_rejects_conflict_bounds_and_aggregate_overflow() {
    let configuration = configurations()
        .into_iter()
        .find(|configuration| {
            configuration.workload == Workload::Write
                && configuration.transaction_size == 1
                && configuration.contention == Contention::High
        })
        .expect("stable benchmark configuration exists");
    let expected_commits =
        (WORKERS * transactions_for(configuration.transaction_size, false)) as u64;
    let mut sample = Sample {
        configuration,
        duration_ns: 1,
        stats: BatchStats {
            commits: expected_commits,
            conflicts: maximum_sample_conflicts(configuration) + 1,
            logical_operations: expected_commits,
        },
        validated_keys: key_count(configuration),
        final_sum: key_count(configuration) as u64,
    };
    assert!(validate_sample(sample, 1).is_err());

    sample.stats.commits = u64::MAX;
    sample.stats.conflicts = 0;
    assert!(aggregate_conflict_rate(&[sample, sample], configuration).is_err());
}
