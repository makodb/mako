//! Reproducible Milestone 1 benchmark for the transactional Mako cache.
//!
//! Public mode orchestrates fresh child processes. Internal child modes own
//! exactly one database namespace, which is required by mako-cache and also
//! prevents cross-arm RocksDB interference.

use std::collections::{BTreeMap, BTreeSet};
use std::env;
use std::ffi::OsString;
use std::fmt::Write as _;
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::os::unix::ffi::{OsStrExt, OsStringExt};
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitStatus, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Barrier};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use mako_cache::{Db as MakoDb, Options as MakoOptions};
use mrx::{Db as MrxDb, Options as MrxOptions, WriteBatch as MrxWriteBatch};
use mrx_core::{BlobOp, Blobs};
use mrx_rocks::{Durability, RocksBlobs};

const REPORT_PROTOCOL: &str = "mako-milestone1-benchmark-v1";
const SAMPLE_PROTOCOL: &str = "mako-m1-sample-v1";
const RECOVERY_PROTOCOL: &str = "mako-m1-recovery-v1";
const CHECKPOINT_PROTOCOL: &str = "mako-m1-checkpoint-v1";
const CHECKPOINT_RESULT_PROTOCOL: &str = "mako-m1-checkpoint-result-v1";
const CHECKPOINT_RUN_PROTOCOL: &str = "mako-m1-checkpoint-run-v1";
const VALUE_BYTES: usize = 128;
const KEY_BYTES: usize = 8;
const LOW_CONTENTION_WINDOW: usize = 256;
const SEED_BATCH: usize = 64;
const ASYNC_MUTATION_CAPACITY: usize = 1 << 18;
const RETRY_MULTIPLIER: u64 = 1_000;
const FORCED_COLLISION_ROUNDS: u64 = 64;
const CHILD_CAPTURE_BYTES: usize = 1024 * 1024;
const MAKO_LOG_PREFIX: &[u8] = b"\0mako-cache\0\x01L";
const MAKO_DATA_PREFIX: &[u8] = b"\0mako-cache\0\x01D";

type AnyResult<T> = Result<T, String>;

#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
enum Arm {
    Mako,
    Mrx,
    Rocks,
}

impl Arm {
    const ALL: [Self; 3] = [Self::Mako, Self::Mrx, Self::Rocks];

    fn name(self) -> &'static str {
        match self {
            Self::Mako => "mako",
            Self::Mrx => "mrx",
            Self::Rocks => "rocks",
        }
    }

    fn parse(value: &str) -> AnyResult<Self> {
        match value {
            "mako" => Ok(Self::Mako),
            "mrx" => Ok(Self::Mrx),
            "rocks" => Ok(Self::Rocks),
            _ => Err(format!("unknown benchmark arm {value:?}")),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
enum Workload {
    Read,
    Write,
    Rmw,
}

impl Workload {
    const ALL: [Self; 3] = [Self::Read, Self::Write, Self::Rmw];

    fn name(self) -> &'static str {
        match self {
            Self::Read => "read",
            Self::Write => "write",
            Self::Rmw => "rmw",
        }
    }

    fn parse(value: &str) -> AnyResult<Self> {
        match value {
            "read" => Ok(Self::Read),
            "write" => Ok(Self::Write),
            "rmw" => Ok(Self::Rmw),
            _ => Err(format!("unknown workload {value:?}")),
        }
    }

    fn operations_per_key(self) -> u64 {
        match self {
            Self::Read | Self::Write => 1,
            Self::Rmw => 2,
        }
    }

    fn writes(self) -> bool {
        self != Self::Read
    }
}

#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
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

    fn parse(value: &str) -> AnyResult<Self> {
        match value {
            "low" => Ok(Self::Low),
            "high" => Ok(Self::High),
            _ => Err(format!("unknown contention profile {value:?}")),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Profile {
    Smoke,
    Acceptance,
}

impl Profile {
    fn name(self) -> &'static str {
        match self {
            Self::Smoke => "smoke",
            Self::Acceptance => "acceptance",
        }
    }

    fn parse(value: &str) -> AnyResult<Self> {
        match value {
            "smoke" => Ok(Self::Smoke),
            "acceptance" => Ok(Self::Acceptance),
            _ => Err(format!(
                "profile must be smoke or acceptance, found {value:?}"
            )),
        }
    }

    fn repetitions(self) -> usize {
        match self {
            Self::Smoke => 1,
            Self::Acceptance => 7,
        }
    }

    fn child_timeout(self) -> Duration {
        match self {
            Self::Smoke => Duration::from_secs(120),
            Self::Acceptance => Duration::from_secs(600),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
struct Configuration {
    arm: Arm,
    workload: Workload,
    transaction_size: usize,
    contention: Contention,
    workers: usize,
}

impl Configuration {
    fn semantic_class(self) -> &'static str {
        if self.transaction_size == 1 && self.workload != Workload::Rmw {
            return "common_point_contract";
        }
        match self.arm {
            Arm::Mako => "transactional_occ_reference",
            Arm::Mrx => "weaker_nonatomic_no_occ_baseline",
            Arm::Rocks if self.workload == Workload::Write => "weaker_atomic_batch_no_occ_baseline",
            Arm::Rocks => "weaker_nonsnapshot_no_occ_baseline",
        }
    }

    fn keyspace(self) -> usize {
        match self.contention {
            Contention::High => self.transaction_size,
            Contention::Low => self.workers * LOW_CONTENTION_WINDOW.max(self.transaction_size * 4),
        }
    }
}

#[derive(Clone, Debug)]
struct ChildSpec {
    configuration: Configuration,
    target_commits: u64,
    warmup_commits: u64,
    path: PathBuf,
}

#[derive(Clone, Copy, Debug, Default)]
struct PhaseStats {
    duration_ns: u64,
    commits: u64,
    conflicts: u64,
    attempt_p50_ns: u64,
    attempt_p99_ns: u64,
    logical_p50_ns: u64,
    logical_p99_ns: u64,
}

#[derive(Clone, Copy, Debug, Default)]
struct BackendStats {
    keys: u64,
    key_bytes: u64,
    value_bytes: u64,
    log_keys: u64,
    log_key_bytes: u64,
    log_value_bytes: u64,
    data_keys: u64,
    physical_bytes: u64,
}

#[derive(Clone, Debug)]
struct SampleResult {
    configuration: Configuration,
    target_commits: u64,
    warmup_commits: u64,
    keyspace: usize,
    warmup_conflicts: u64,
    phase: PhaseStats,
    drain_ns: u64,
    logical_operations: u64,
    measured_mutation_bytes: u64,
    total_mutation_bytes: u64,
    live_user_bytes: u64,
    checksum: u64,
    backend: BackendStats,
}

#[derive(Clone, Debug)]
struct RecoveryResult {
    configuration: Configuration,
    keyspace: usize,
    open_ns: u64,
    validation_ns: u64,
    checksum: u64,
}

#[derive(Clone, Debug)]
struct CombinedResult {
    repetition: usize,
    sample: SampleResult,
    recovery: RecoveryResult,
}

#[derive(Debug)]
struct RunOptions {
    profile: Profile,
    data_root: PathBuf,
    output: Option<PathBuf>,
    checkpoint: Option<PathBuf>,
    resume: bool,
    keep_data: bool,
}

struct Checkpoint {
    file: Option<File>,
}

struct ChildCapture {
    bytes: Vec<u8>,
    total_bytes: u64,
}

#[derive(Clone)]
enum Database {
    Mako(Arc<MakoDb>),
    Mrx(Arc<MrxDb>),
    Rocks(Arc<RocksBlobs>),
}

impl Database {
    fn open(configuration: Configuration, path: &Path) -> AnyResult<Self> {
        match configuration.arm {
            Arm::Mako => {
                let mut options = MakoOptions::default();
                options.cache.writeback.capacity = async_queue_capacity(configuration);
                MakoDb::open(path, options)
                    .map(|db| Self::Mako(Arc::new(db)))
                    .map_err(|error| format!("open mako-cache: {error}"))
            }
            Arm::Mrx => {
                let mut options = MrxOptions::default();
                options.cache.log_slots = async_queue_capacity(configuration);
                MrxDb::open(path, options)
                    .map(|db| Self::Mrx(Arc::new(db)))
                    .map_err(|error| format!("open mrx: {error}"))
            }
            Arm::Rocks => RocksBlobs::open(path, Durability::Wal)
                .map(|db| Self::Rocks(Arc::new(db)))
                .map_err(|error| format!("open raw RocksDB: {error}")),
        }
    }

    fn seed(&self, keyspace: usize) -> AnyResult<()> {
        for base in (0..keyspace).step_by(SEED_BATCH) {
            let end = keyspace.min(base + SEED_BATCH);
            match self {
                Self::Mako(db) => {
                    let mut transaction = db.transaction().map_err(error_string)?;
                    for index in base..end {
                        transaction
                            .put(&key_bytes(index), &value_bytes(0))
                            .map_err(error_string)?;
                    }
                    transaction.commit().map_err(error_string)?;
                }
                Self::Mrx(db) => {
                    let mut batch = MrxWriteBatch::new();
                    for index in base..end {
                        batch.put(&key_bytes(index), &value_bytes(0));
                    }
                    db.write(batch).map_err(error_string)?;
                }
                Self::Rocks(db) => {
                    let owned: Vec<_> = (base..end)
                        .map(|index| (key_bytes(index), value_bytes(0)))
                        .collect();
                    let operations: Vec<_> = owned
                        .iter()
                        .map(|(key, value)| BlobOp::Put {
                            key: key.as_slice(),
                            val: value.as_slice(),
                        })
                        .collect();
                    db.write_batch(&operations).map_err(error_string)?;
                }
            }
        }
        Ok(())
    }

    fn attempt(
        &self,
        configuration: Configuration,
        keys: &[usize],
        commit_barrier: Option<&Barrier>,
    ) -> AnyResult<bool> {
        match self {
            Self::Mako(db) => mako_attempt(db, configuration.workload, keys, commit_barrier),
            Self::Mrx(db) => mrx_attempt(db, configuration.workload, keys, commit_barrier),
            Self::Rocks(db) => rocks_attempt(db, configuration.workload, keys, commit_barrier),
        }
    }

    fn get(&self, index: usize) -> AnyResult<Option<Vec<u8>>> {
        let key = key_bytes(index);
        match self {
            Self::Mako(db) => db.get(&key).map_err(error_string),
            Self::Mrx(db) => db.get(&key).map_err(error_string),
            Self::Rocks(db) => db.get(&key).map_err(error_string),
        }
    }

    fn wait_applied(&self) -> AnyResult<u64> {
        let start = Instant::now();
        match self {
            Self::Mako(db) => {
                db.wait_applied().map_err(error_string)?;
            }
            Self::Mrx(db) => {
                db.flush().map_err(error_string)?;
            }
            Self::Rocks(_) => return Ok(0),
        }
        Ok(elapsed_ns(start))
    }

    fn close(self) -> AnyResult<()> {
        match self {
            Self::Mako(db) => {
                let db = Arc::try_unwrap(db)
                    .map_err(|_| "mako-cache benchmark retained a database reference".to_owned())?;
                db.close().map_err(error_string)?;
            }
            Self::Mrx(db) => {
                let db = Arc::try_unwrap(db)
                    .map_err(|_| "mrx benchmark retained a database reference".to_owned())?;
                db.close().map_err(error_string)?;
            }
            Self::Rocks(db) => {
                let db = Arc::try_unwrap(db)
                    .map_err(|_| "RocksDB benchmark retained a database reference".to_owned())?;
                drop(db);
            }
        }
        Ok(())
    }
}

fn main() {
    if let Err(error) = real_main() {
        eprintln!("mako-cache-bench: {error}");
        std::process::exit(2);
    }
}

fn real_main() -> AnyResult<()> {
    let arguments: Vec<String> = env::args().skip(1).collect();
    match arguments.first().map(String::as_str) {
        Some("run") => run_orchestrator(parse_run_options(&arguments[1..])?),
        Some("__sample") => {
            let result = run_sample(parse_child_spec(&arguments[1..])?)?;
            println!("{}", encode_sample_protocol(&result));
            Ok(())
        }
        Some("__recover") => {
            let (spec, checksum) = parse_recovery_spec(&arguments[1..])?;
            let result = run_recovery(spec, checksum)?;
            println!("{}", encode_recovery_protocol(&result));
            Ok(())
        }
        _ => Err(usage()),
    }
}

fn usage() -> String {
    "usage: mako-cache-bench run --profile smoke|acceptance --data-root PATH [--output FILE] [--checkpoint FILE] [--resume] [--keep-data]".to_owned()
}

fn parse_run_options(arguments: &[String]) -> AnyResult<RunOptions> {
    let mut profile = None;
    let mut data_root = None;
    let mut output = None;
    let mut checkpoint = None;
    let mut resume = false;
    let mut keep_data = false;
    let mut index = 0;
    while index < arguments.len() {
        match arguments[index].as_str() {
            "--profile" => {
                index += 1;
                profile = Some(Profile::parse(required_argument(
                    arguments,
                    index,
                    "--profile",
                )?)?);
            }
            "--data-root" => {
                index += 1;
                data_root = Some(PathBuf::from(required_argument(
                    arguments,
                    index,
                    "--data-root",
                )?));
            }
            "--output" => {
                index += 1;
                output = Some(PathBuf::from(required_argument(
                    arguments, index, "--output",
                )?));
            }
            "--checkpoint" => {
                index += 1;
                checkpoint = Some(PathBuf::from(required_argument(
                    arguments,
                    index,
                    "--checkpoint",
                )?));
            }
            "--resume" => resume = true,
            "--keep-data" => keep_data = true,
            other => return Err(format!("unknown option {other:?}; {}", usage())),
        }
        index += 1;
    }
    let profile = profile.ok_or_else(usage)?;
    if profile == Profile::Acceptance && cfg!(debug_assertions) {
        return Err("the acceptance profile requires --release".to_owned());
    }
    if profile == Profile::Acceptance && output.is_none() {
        return Err("the acceptance profile requires --output".to_owned());
    }
    if checkpoint.is_none() {
        checkpoint = output.as_deref().map(checkpoint_path_for_output);
    }
    if resume && checkpoint.is_none() {
        return Err("--resume requires --checkpoint or --output".to_owned());
    }
    let data_root = data_root.ok_or_else(|| "--data-root is required".to_owned())?;
    if !data_root.is_dir() {
        return Err(format!(
            "data root {} is not a directory",
            data_root.display()
        ));
    }
    Ok(RunOptions {
        profile,
        data_root,
        output,
        checkpoint,
        resume,
        keep_data,
    })
}

fn required_argument<'a>(arguments: &'a [String], index: usize, flag: &str) -> AnyResult<&'a str> {
    arguments
        .get(index)
        .map(String::as_str)
        .ok_or_else(|| format!("{flag} requires a value"))
}

fn checkpoint_path_for_output(output: &Path) -> PathBuf {
    let mut name = output.as_os_str().to_owned();
    name.push(".checkpoint");
    PathBuf::from(name)
}

fn parse_child_spec(arguments: &[String]) -> AnyResult<ChildSpec> {
    if arguments.len() != 8 {
        return Err("internal sample role received the wrong argument count".to_owned());
    }
    Ok(ChildSpec {
        configuration: Configuration {
            arm: Arm::parse(&arguments[0])?,
            workload: Workload::parse(&arguments[1])?,
            transaction_size: parse_positive(&arguments[2], "transaction size")?,
            contention: Contention::parse(&arguments[3])?,
            workers: parse_positive(&arguments[4], "worker count")?,
        },
        target_commits: parse_positive_u64(&arguments[5], "target commits")?,
        warmup_commits: parse_positive_u64(&arguments[6], "warmup commits")?,
        path: PathBuf::from(&arguments[7]),
    })
}

fn parse_recovery_spec(arguments: &[String]) -> AnyResult<(ChildSpec, u64)> {
    if arguments.len() != 9 {
        return Err("internal recovery role received the wrong argument count".to_owned());
    }
    let spec = parse_child_spec(&arguments[..8])?;
    let checksum = parse_u64(&arguments[8], "expected checksum")?;
    Ok((spec, checksum))
}

fn parse_positive(value: &str, label: &str) -> AnyResult<usize> {
    let parsed: usize = value
        .parse()
        .map_err(|_| format!("invalid {label} {value:?}"))?;
    if parsed == 0 || parsed.to_string() != value {
        return Err(format!("{label} must be a canonical positive integer"));
    }
    Ok(parsed)
}

fn parse_positive_u64(value: &str, label: &str) -> AnyResult<u64> {
    let parsed = parse_u64(value, label)?;
    if parsed == 0 {
        return Err(format!("{label} must be positive"));
    }
    Ok(parsed)
}

fn parse_u64(value: &str, label: &str) -> AnyResult<u64> {
    let parsed: u64 = value
        .parse()
        .map_err(|_| format!("invalid {label} {value:?}"))?;
    if parsed.to_string() != value {
        return Err(format!("{label} is not canonical"));
    }
    Ok(parsed)
}

fn error_string(error: impl std::fmt::Display) -> String {
    error.to_string()
}

fn key_bytes(index: usize) -> [u8; KEY_BYTES] {
    (index as u64).to_be_bytes()
}

fn value_bytes(counter: u64) -> Vec<u8> {
    let mut value = vec![0xa5; VALUE_BYTES];
    value[..8].copy_from_slice(&counter.to_le_bytes());
    value
}

fn decode_value(value: &[u8]) -> AnyResult<u64> {
    if value.len() != VALUE_BYTES || value[8..].iter().any(|byte| *byte != 0xa5) {
        return Err("benchmark read malformed value bytes".to_owned());
    }
    Ok(u64::from_le_bytes(
        value[..8].try_into().expect("eight-byte prefix"),
    ))
}

fn selected_keys(configuration: Configuration, worker: usize, committed: u64) -> Vec<usize> {
    match configuration.contention {
        Contention::High => (0..configuration.transaction_size).collect(),
        Contention::Low => {
            let base = worker * LOW_CONTENTION_WINDOW.max(configuration.transaction_size * 4);
            let window = LOW_CONTENTION_WINDOW.max(configuration.transaction_size * 4);
            (0..configuration.transaction_size)
                .map(|item| {
                    base + ((committed as usize * configuration.transaction_size + item) % window)
                })
                .collect()
        }
    }
}

fn mako_attempt(
    db: &MakoDb,
    workload: Workload,
    keys: &[usize],
    commit_barrier: Option<&Barrier>,
) -> AnyResult<bool> {
    let mut transaction = db.transaction().map_err(error_string)?;
    let mut live = true;
    for index in keys {
        let key = key_bytes(*index);
        let result = match workload {
            Workload::Read => transaction.get(&key).map(|value| value.map(|_| false)),
            Workload::Write => transaction.put(&key, &value_bytes(1)).map(Some),
            Workload::Rmw => match transaction.get(&key) {
                Ok(Some(value)) => {
                    let next = decode_value(&value)?
                        .checked_add(1)
                        .ok_or_else(|| "benchmark counter overflow".to_owned())?;
                    transaction.put(&key, &value_bytes(next)).map(Some)
                }
                Ok(None) => return Err("mako-cache benchmark key disappeared".to_owned()),
                Err(error) => Err(error),
            },
        };
        match result {
            Ok(Some(_)) => {}
            Ok(None) => return Err("mako-cache benchmark key disappeared".to_owned()),
            Err(error) if error.is_conflict() => {
                live = false;
                break;
            }
            Err(error) => return Err(error.to_string()),
        }
    }
    if let Some(barrier) = commit_barrier {
        barrier.wait();
    }
    if !live {
        return Ok(false);
    }
    match transaction.commit() {
        Ok(()) => Ok(true),
        Err(error) if error.is_conflict() => Ok(false),
        Err(error) => Err(error.to_string()),
    }
}

fn mrx_attempt(
    db: &MrxDb,
    workload: Workload,
    keys: &[usize],
    commit_barrier: Option<&Barrier>,
) -> AnyResult<bool> {
    let mut next_values = Vec::with_capacity(keys.len());
    if workload != Workload::Write {
        for index in keys {
            let value = db
                .get(&key_bytes(*index))
                .map_err(error_string)?
                .ok_or_else(|| "mrx benchmark key disappeared".to_owned())?;
            next_values.push(decode_value(&value)?.saturating_add(1));
        }
    }
    if let Some(barrier) = commit_barrier {
        barrier.wait();
    }
    if workload != Workload::Read {
        let mut batch = MrxWriteBatch::new();
        for (ordinal, index) in keys.iter().enumerate() {
            let counter = if workload == Workload::Write {
                1
            } else {
                next_values[ordinal]
            };
            batch.put(&key_bytes(*index), &value_bytes(counter));
        }
        db.write(batch).map_err(error_string)?;
    }
    Ok(true)
}

fn rocks_attempt(
    db: &RocksBlobs,
    workload: Workload,
    keys: &[usize],
    commit_barrier: Option<&Barrier>,
) -> AnyResult<bool> {
    let mut counters = Vec::with_capacity(keys.len());
    if workload != Workload::Write {
        for index in keys {
            let value = db
                .get(&key_bytes(*index))
                .map_err(error_string)?
                .ok_or_else(|| "RocksDB benchmark key disappeared".to_owned())?;
            counters.push(decode_value(&value)?.saturating_add(1));
        }
    }
    if let Some(barrier) = commit_barrier {
        barrier.wait();
    }
    if workload != Workload::Read {
        let owned: Vec<_> = keys
            .iter()
            .enumerate()
            .map(|(ordinal, index)| {
                let counter = if workload == Workload::Write {
                    1
                } else {
                    counters[ordinal]
                };
                (key_bytes(*index), value_bytes(counter))
            })
            .collect();
        let operations: Vec<_> = owned
            .iter()
            .map(|(key, value)| BlobOp::Put {
                key: key.as_slice(),
                val: value.as_slice(),
            })
            .collect();
        db.write_batch(&operations).map_err(error_string)?;
    }
    Ok(true)
}

#[derive(Debug, Default)]
struct WorkerStats {
    commits: u64,
    conflicts: u64,
    attempt_latencies: Vec<u64>,
    logical_latencies: Vec<u64>,
}

fn run_phase(
    database: &Database,
    configuration: Configuration,
    target_commits: u64,
    retain_latencies: bool,
) -> AnyResult<PhaseStats> {
    let ready_barrier = Arc::new(Barrier::new(configuration.workers + 1));
    let release_gate = Arc::new(AtomicBool::new(false));
    let collision_barrier = Arc::new(Barrier::new(configuration.workers));
    let forced_rounds = if configuration.contention == Contention::High && configuration.workers > 1
    {
        FORCED_COLLISION_ROUNDS.min(target_commits)
    } else {
        0
    };
    let mut handles = Vec::with_capacity(configuration.workers);
    for worker in 0..configuration.workers {
        let database = database.clone();
        let ready_barrier = Arc::clone(&ready_barrier);
        let release_gate = Arc::clone(&release_gate);
        let collision_barrier = Arc::clone(&collision_barrier);
        handles.push(thread::spawn(move || -> AnyResult<WorkerStats> {
            ready_barrier.wait();
            while !release_gate.load(Ordering::Acquire) {
                std::hint::spin_loop();
            }
            let mut result = WorkerStats::default();
            let mut logical_start = Instant::now();
            while result.commits != target_commits {
                let attempts = result.commits + result.conflicts;
                let retry_limit = target_commits
                    .checked_mul(RETRY_MULTIPLIER)
                    .ok_or_else(|| "retry limit overflow".to_owned())?;
                if attempts >= retry_limit {
                    return Err(format!(
                        "worker {worker} exhausted {retry_limit} attempts after {} commits and {} conflicts",
                        result.commits, result.conflicts
                    ));
                }
                let synchronize = attempts < forced_rounds;
                if synchronize {
                    collision_barrier.wait();
                }
                let keys = selected_keys(configuration, worker, result.commits);
                let attempt_start = Instant::now();
                let committed = database.attempt(
                    configuration,
                    &keys,
                    synchronize.then_some(collision_barrier.as_ref()),
                )?;
                if retain_latencies {
                    result.attempt_latencies.push(elapsed_ns(attempt_start));
                }
                if committed {
                    result.commits += 1;
                    if retain_latencies {
                        result.logical_latencies.push(elapsed_ns(logical_start));
                    }
                    logical_start = Instant::now();
                } else {
                    result.conflicts += 1;
                    thread::yield_now();
                }
            }
            Ok(result)
        }));
    }

    ready_barrier.wait();
    let start = Instant::now();
    release_gate.store(true, Ordering::Release);
    let mut aggregate = WorkerStats::default();
    for (worker, handle) in handles.into_iter().enumerate() {
        let worker_result = handle
            .join()
            .map_err(|_| format!("benchmark worker {worker} panicked"))??;
        aggregate.commits = aggregate
            .commits
            .checked_add(worker_result.commits)
            .ok_or_else(|| "commit count overflow".to_owned())?;
        aggregate.conflicts = aggregate
            .conflicts
            .checked_add(worker_result.conflicts)
            .ok_or_else(|| "conflict count overflow".to_owned())?;
        aggregate
            .attempt_latencies
            .extend(worker_result.attempt_latencies);
        aggregate
            .logical_latencies
            .extend(worker_result.logical_latencies);
    }
    let duration_ns = elapsed_ns(start);
    let expected = target_commits
        .checked_mul(configuration.workers as u64)
        .ok_or_else(|| "expected commit count overflow".to_owned())?;
    if aggregate.commits != expected {
        return Err(format!(
            "phase committed {} transactions, expected {expected}",
            aggregate.commits
        ));
    }
    Ok(PhaseStats {
        duration_ns,
        commits: aggregate.commits,
        conflicts: aggregate.conflicts,
        attempt_p50_ns: percentile(&mut aggregate.attempt_latencies, 50),
        attempt_p99_ns: percentile(&mut aggregate.attempt_latencies, 99),
        logical_p50_ns: percentile(&mut aggregate.logical_latencies, 50),
        logical_p99_ns: percentile(&mut aggregate.logical_latencies, 99),
    })
}

fn percentile(values: &mut [u64], percentile: usize) -> u64 {
    if values.is_empty() {
        return 0;
    }
    values.sort_unstable();
    let rank = (percentile * values.len()).div_ceil(100);
    values[rank.saturating_sub(1).min(values.len() - 1)]
}

fn elapsed_ns(start: Instant) -> u64 {
    u64::try_from(start.elapsed().as_nanos())
        .unwrap_or(u64::MAX)
        .max(1)
}

fn validate_database(
    database: &Database,
    configuration: Configuration,
    warmup_commits: u64,
    measured_commits: u64,
) -> AnyResult<u64> {
    let mut checksum = 0u64;
    for index in 0..configuration.keyspace() {
        let value = database
            .get(index)?
            .ok_or_else(|| format!("validation found missing key {index}"))?;
        let counter = decode_value(&value)?;
        match configuration.workload {
            Workload::Read if counter != 0 => {
                return Err(format!(
                    "read-only workload changed key {index} to {counter}"
                ));
            }
            Workload::Write if counter > 1 => {
                return Err(format!("write workload produced invalid counter {counter}"));
            }
            Workload::Read | Workload::Write => {}
            Workload::Rmw => {}
        }
        checksum = checksum
            .checked_add(counter)
            .ok_or_else(|| "validation checksum overflow".to_owned())?;
    }

    if configuration.workload == Workload::Rmw {
        let successful_transactions = warmup_commits
            .checked_add(measured_commits)
            .and_then(|count| count.checked_mul(configuration.workers as u64))
            .ok_or_else(|| "successful transaction count overflow".to_owned())?;
        let expected = successful_transactions
            .checked_mul(configuration.transaction_size as u64)
            .ok_or_else(|| "expected RMW checksum overflow".to_owned())?;
        if configuration.contention == Contention::Low || configuration.arm == Arm::Mako {
            if checksum != expected {
                return Err(format!(
                    "serializable RMW checksum is {checksum}, expected {expected}"
                ));
            }
        } else if checksum == 0 || checksum > expected {
            return Err(format!(
                "weaker RMW baseline checksum {checksum} is outside 1..={expected}"
            ));
        }
    }
    Ok(checksum)
}

fn validate_recovered(
    database: &Database,
    configuration: Configuration,
    expected_checksum: u64,
) -> AnyResult<u64> {
    let mut checksum = 0u64;
    for index in 0..configuration.keyspace() {
        let value = database
            .get(index)?
            .ok_or_else(|| format!("recovery validation found missing key {index}"))?;
        checksum = checksum
            .checked_add(decode_value(&value)?)
            .ok_or_else(|| "recovery checksum overflow".to_owned())?;
    }
    if checksum != expected_checksum {
        return Err(format!(
            "recovery checksum {checksum} differs from pre-close checksum {expected_checksum}"
        ));
    }
    Ok(checksum)
}

fn run_sample(spec: ChildSpec) -> AnyResult<SampleResult> {
    if spec.path.exists() {
        return Err(format!(
            "fresh sample path already exists: {}",
            spec.path.display()
        ));
    }
    let database = Database::open(spec.configuration, &spec.path)?;
    let keyspace = spec.configuration.keyspace();
    database.seed(keyspace)?;
    database.wait_applied()?;
    let warmup = run_phase(&database, spec.configuration, spec.warmup_commits, false)?;
    database.wait_applied()?;
    let phase = run_phase(&database, spec.configuration, spec.target_commits, true)?;
    let drain_ns = database.wait_applied()?;
    let checksum = validate_database(
        &database,
        spec.configuration,
        spec.warmup_commits,
        spec.target_commits,
    )?;
    database.close()?;
    let backend = inspect_backend(&spec.path, spec.configuration.arm)?;

    let logical_operations = phase
        .commits
        .checked_mul(spec.configuration.transaction_size as u64)
        .and_then(|value| value.checked_mul(spec.configuration.workload.operations_per_key()))
        .ok_or_else(|| "logical operation count overflow".to_owned())?;
    let bytes_per_mutation = (KEY_BYTES + VALUE_BYTES) as u64;
    let measured_mutation_bytes = if spec.configuration.workload.writes() {
        phase
            .commits
            .checked_mul(spec.configuration.transaction_size as u64)
            .and_then(|value| value.checked_mul(bytes_per_mutation))
            .ok_or_else(|| "measured mutation byte count overflow".to_owned())?
    } else {
        0
    };
    let warmup_mutations = if spec.configuration.workload.writes() {
        warmup
            .commits
            .checked_mul(spec.configuration.transaction_size as u64)
            .ok_or_else(|| "warmup mutation count overflow".to_owned())?
    } else {
        0
    };
    let measured_mutations = if spec.configuration.workload.writes() {
        phase
            .commits
            .checked_mul(spec.configuration.transaction_size as u64)
            .ok_or_else(|| "measured mutation count overflow".to_owned())?
    } else {
        0
    };
    let total_mutation_bytes = (keyspace as u64)
        .checked_add(warmup_mutations)
        .and_then(|mutations| mutations.checked_add(measured_mutations))
        .and_then(|mutations| mutations.checked_mul(bytes_per_mutation))
        .ok_or_else(|| "total mutation byte count overflow".to_owned())?;
    let live_user_bytes = (keyspace as u64)
        .checked_mul(bytes_per_mutation)
        .ok_or_else(|| "live user byte count overflow".to_owned())?;

    Ok(SampleResult {
        configuration: spec.configuration,
        target_commits: spec.target_commits,
        warmup_commits: spec.warmup_commits,
        keyspace,
        warmup_conflicts: warmup.conflicts,
        phase,
        drain_ns,
        logical_operations,
        measured_mutation_bytes,
        total_mutation_bytes,
        live_user_bytes,
        checksum,
        backend,
    })
}

fn run_recovery(spec: ChildSpec, expected_checksum: u64) -> AnyResult<RecoveryResult> {
    if !spec.path.is_dir() {
        return Err(format!("recovery path is missing: {}", spec.path.display()));
    }
    let open_start = Instant::now();
    let database = Database::open(spec.configuration, &spec.path)?;
    let open_ns = elapsed_ns(open_start);
    let validation_start = Instant::now();
    let checksum = validate_recovered(&database, spec.configuration, expected_checksum)?;
    let validation_ns = elapsed_ns(validation_start);
    database.wait_applied()?;
    database.close()?;
    Ok(RecoveryResult {
        configuration: spec.configuration,
        keyspace: spec.configuration.keyspace(),
        open_ns,
        validation_ns,
        checksum,
    })
}

fn inspect_backend(path: &Path, arm: Arm) -> AnyResult<BackendStats> {
    let backend = RocksBlobs::open(path, Durability::Wal).map_err(error_string)?;
    backend.flush().map_err(error_string)?;
    let mut keys = Vec::<Vec<u8>>::new();
    backend
        .for_each_key(&mut |key| keys.push(key.to_vec()))
        .map_err(error_string)?;
    let mut stats = BackendStats::default();
    for key in keys {
        let value = backend
            .get(&key)
            .map_err(error_string)?
            .ok_or_else(|| "backend key disappeared during inspection".to_owned())?;
        stats.keys += 1;
        stats.key_bytes = stats.key_bytes.saturating_add(key.len() as u64);
        stats.value_bytes = stats.value_bytes.saturating_add(value.len() as u64);
        if arm == Arm::Mako && key.starts_with(MAKO_LOG_PREFIX) {
            stats.log_keys += 1;
            stats.log_key_bytes = stats.log_key_bytes.saturating_add(key.len() as u64);
            stats.log_value_bytes = stats.log_value_bytes.saturating_add(value.len() as u64);
        } else if arm == Arm::Mako && key.starts_with(MAKO_DATA_PREFIX) {
            stats.data_keys += 1;
        } else if arm == Arm::Mako {
            return Err("mako-cache backend contained a foreign key".to_owned());
        } else {
            stats.data_keys += 1;
        }
    }
    drop(backend);
    stats.physical_bytes = allocated_bytes(path)?;
    Ok(stats)
}

fn allocated_bytes(path: &Path) -> AnyResult<u64> {
    let metadata =
        fs::symlink_metadata(path).map_err(|error| format!("stat {}: {error}", path.display()))?;
    if metadata.file_type().is_symlink() {
        return Err(format!("refusing to account symlink {}", path.display()));
    }
    let own = metadata.blocks().saturating_mul(512);
    if metadata.is_file() {
        return Ok(own);
    }
    let mut total = own;
    for entry in fs::read_dir(path).map_err(|error| format!("read {}: {error}", path.display()))? {
        let entry = entry.map_err(|error| format!("read directory entry: {error}"))?;
        total = total
            .checked_add(allocated_bytes(&entry.path())?)
            .ok_or_else(|| "allocated byte count overflow".to_owned())?;
    }
    Ok(total)
}

fn encode_sample_protocol(result: &SampleResult) -> String {
    let mut output = String::new();
    let configuration = result.configuration;
    write!(
        &mut output,
        "{SAMPLE_PROTOCOL} arm={} workload={} tx={} contention={} workers={} target={} warmup={} keyspace={} warmup_conflicts={} duration_ns={} commits={} conflicts={} attempt_p50_ns={} attempt_p99_ns={} logical_p50_ns={} logical_p99_ns={} drain_ns={} logical_ops={} measured_mutation_bytes={} total_mutation_bytes={} live_user_bytes={} checksum={} backend_keys={} backend_key_bytes={} backend_value_bytes={} log_keys={} log_key_bytes={} log_value_bytes={} data_keys={} physical_bytes={}",
        configuration.arm.name(),
        configuration.workload.name(),
        configuration.transaction_size,
        configuration.contention.name(),
        configuration.workers,
        result.target_commits,
        result.warmup_commits,
        result.keyspace,
        result.warmup_conflicts,
        result.phase.duration_ns,
        result.phase.commits,
        result.phase.conflicts,
        result.phase.attempt_p50_ns,
        result.phase.attempt_p99_ns,
        result.phase.logical_p50_ns,
        result.phase.logical_p99_ns,
        result.drain_ns,
        result.logical_operations,
        result.measured_mutation_bytes,
        result.total_mutation_bytes,
        result.live_user_bytes,
        result.checksum,
        result.backend.keys,
        result.backend.key_bytes,
        result.backend.value_bytes,
        result.backend.log_keys,
        result.backend.log_key_bytes,
        result.backend.log_value_bytes,
        result.backend.data_keys,
        result.backend.physical_bytes,
    )
    .expect("writing to String cannot fail");
    output
}

fn parse_sample_protocol(text: &str) -> AnyResult<SampleResult> {
    let mut fields = protocol_fields(text, SAMPLE_PROTOCOL)?;
    let configuration = Configuration {
        arm: Arm::parse(take_field(&mut fields, "arm")?)?,
        workload: Workload::parse(take_field(&mut fields, "workload")?)?,
        transaction_size: take_usize(&mut fields, "tx")?,
        contention: Contention::parse(take_field(&mut fields, "contention")?)?,
        workers: take_usize(&mut fields, "workers")?,
    };
    let result = SampleResult {
        configuration,
        target_commits: take_number(&mut fields, "target")?,
        warmup_commits: take_number(&mut fields, "warmup")?,
        keyspace: take_usize(&mut fields, "keyspace")?,
        warmup_conflicts: take_number(&mut fields, "warmup_conflicts")?,
        phase: PhaseStats {
            duration_ns: take_number(&mut fields, "duration_ns")?,
            commits: take_number(&mut fields, "commits")?,
            conflicts: take_number(&mut fields, "conflicts")?,
            attempt_p50_ns: take_number(&mut fields, "attempt_p50_ns")?,
            attempt_p99_ns: take_number(&mut fields, "attempt_p99_ns")?,
            logical_p50_ns: take_number(&mut fields, "logical_p50_ns")?,
            logical_p99_ns: take_number(&mut fields, "logical_p99_ns")?,
        },
        drain_ns: take_number(&mut fields, "drain_ns")?,
        logical_operations: take_number(&mut fields, "logical_ops")?,
        measured_mutation_bytes: take_number(&mut fields, "measured_mutation_bytes")?,
        total_mutation_bytes: take_number(&mut fields, "total_mutation_bytes")?,
        live_user_bytes: take_number(&mut fields, "live_user_bytes")?,
        checksum: take_number(&mut fields, "checksum")?,
        backend: BackendStats {
            keys: take_number(&mut fields, "backend_keys")?,
            key_bytes: take_number(&mut fields, "backend_key_bytes")?,
            value_bytes: take_number(&mut fields, "backend_value_bytes")?,
            log_keys: take_number(&mut fields, "log_keys")?,
            log_key_bytes: take_number(&mut fields, "log_key_bytes")?,
            log_value_bytes: take_number(&mut fields, "log_value_bytes")?,
            data_keys: take_number(&mut fields, "data_keys")?,
            physical_bytes: take_number(&mut fields, "physical_bytes")?,
        },
    };
    reject_remaining_fields(fields)?;
    validate_sample_result(&result)?;
    Ok(result)
}

fn encode_recovery_protocol(result: &RecoveryResult) -> String {
    let mut output = String::new();
    let configuration = result.configuration;
    write!(
        &mut output,
        "{RECOVERY_PROTOCOL} arm={} workload={} tx={} contention={} workers={} keyspace={} open_ns={} validation_ns={} checksum={}",
        configuration.arm.name(),
        configuration.workload.name(),
        configuration.transaction_size,
        configuration.contention.name(),
        configuration.workers,
        result.keyspace,
        result.open_ns,
        result.validation_ns,
        result.checksum,
    )
    .expect("writing to String cannot fail");
    output
}

fn parse_recovery_protocol(text: &str) -> AnyResult<RecoveryResult> {
    let mut fields = protocol_fields(text, RECOVERY_PROTOCOL)?;
    let configuration = Configuration {
        arm: Arm::parse(take_field(&mut fields, "arm")?)?,
        workload: Workload::parse(take_field(&mut fields, "workload")?)?,
        transaction_size: take_usize(&mut fields, "tx")?,
        contention: Contention::parse(take_field(&mut fields, "contention")?)?,
        workers: take_usize(&mut fields, "workers")?,
    };
    let result = RecoveryResult {
        configuration,
        keyspace: take_usize(&mut fields, "keyspace")?,
        open_ns: take_number(&mut fields, "open_ns")?,
        validation_ns: take_number(&mut fields, "validation_ns")?,
        checksum: take_number(&mut fields, "checksum")?,
    };
    reject_remaining_fields(fields)?;
    if result.open_ns == 0 || result.validation_ns == 0 {
        return Err("recovery protocol contains a zero duration".to_owned());
    }
    Ok(result)
}

fn protocol_fields<'a>(text: &'a str, protocol: &str) -> AnyResult<BTreeMap<&'a str, &'a str>> {
    if text.contains('\r') {
        return Err("child protocol must contain exactly one canonical line".to_owned());
    }
    let line = text.strip_suffix('\n').unwrap_or(text);
    if line.contains('\n') {
        return Err("child protocol must contain exactly one canonical line".to_owned());
    }
    let canonical: Vec<_> = line.split_ascii_whitespace().collect();
    if canonical.join(" ") != line {
        return Err("child protocol contains noncanonical whitespace".to_owned());
    }
    let mut tokens = canonical.into_iter();
    if tokens.next() != Some(protocol) {
        return Err(format!("child output is missing protocol {protocol}"));
    }
    let mut fields = BTreeMap::new();
    for token in tokens {
        let (key, value) = token
            .split_once('=')
            .ok_or_else(|| format!("malformed child field {token:?}"))?;
        if key.is_empty() || value.is_empty() || fields.insert(key, value).is_some() {
            return Err(format!("invalid or duplicate child field {key:?}"));
        }
    }
    Ok(fields)
}

fn take_field<'a>(fields: &mut BTreeMap<&'a str, &'a str>, key: &str) -> AnyResult<&'a str> {
    fields
        .remove(key)
        .ok_or_else(|| format!("child protocol is missing {key}"))
}

fn take_number<'a>(fields: &mut BTreeMap<&'a str, &'a str>, key: &str) -> AnyResult<u64> {
    parse_u64(take_field(fields, key)?, key)
}

fn take_usize<'a>(fields: &mut BTreeMap<&'a str, &'a str>, key: &str) -> AnyResult<usize> {
    let value = take_number(fields, key)?;
    usize::try_from(value).map_err(|_| format!("{key} does not fit usize"))
}

fn reject_remaining_fields(fields: BTreeMap<&str, &str>) -> AnyResult<()> {
    if fields.is_empty() {
        Ok(())
    } else {
        Err(format!(
            "child protocol has unknown fields: {}",
            fields.keys().copied().collect::<Vec<_>>().join(",")
        ))
    }
}

fn validate_sample_result(result: &SampleResult) -> AnyResult<()> {
    let configuration = result.configuration;
    if result.keyspace != configuration.keyspace() {
        return Err("child keyspace disagrees with configuration".to_owned());
    }
    let expected_commits = result
        .target_commits
        .checked_mul(configuration.workers as u64)
        .ok_or_else(|| "expected commits overflow".to_owned())?;
    if result.phase.commits != expected_commits {
        return Err(format!(
            "child reported {} commits, expected {expected_commits}",
            result.phase.commits
        ));
    }
    for (label, value) in [
        ("duration", result.phase.duration_ns),
        ("attempt p50", result.phase.attempt_p50_ns),
        ("attempt p99", result.phase.attempt_p99_ns),
        ("logical p50", result.phase.logical_p50_ns),
        ("logical p99", result.phase.logical_p99_ns),
    ] {
        if value == 0 {
            return Err(format!("child reported zero {label}"));
        }
    }
    if result.drain_ns == 0 && configuration.arm != Arm::Rocks {
        return Err("an asynchronous cache reported a zero drain duration".to_owned());
    }
    if result.phase.attempt_p50_ns > result.phase.attempt_p99_ns
        || result.phase.logical_p50_ns > result.phase.logical_p99_ns
    {
        return Err("child percentiles are not monotonic".to_owned());
    }
    if configuration.arm != Arm::Mako && result.phase.conflicts != 0 {
        return Err("a no-OCC baseline reported conflicts".to_owned());
    }
    if configuration.arm != Arm::Mako
        && (result.backend.log_keys != 0
            || result.backend.log_key_bytes != 0
            || result.backend.log_value_bytes != 0)
    {
        return Err("a no-log baseline reported commit-log bytes".to_owned());
    }
    if result.backend.keys != result.backend.log_keys + result.backend.data_keys {
        return Err("backend key classification is incomplete".to_owned());
    }
    let expected_operations = result
        .phase
        .commits
        .checked_mul(configuration.transaction_size as u64)
        .and_then(|value| value.checked_mul(configuration.workload.operations_per_key()))
        .ok_or_else(|| "expected logical operation count overflow".to_owned())?;
    if result.logical_operations != expected_operations {
        return Err("child logical operation count is inconsistent".to_owned());
    }
    let bytes_per_mutation = (KEY_BYTES + VALUE_BYTES) as u64;
    let expected_measured_mutations = if configuration.workload.writes() {
        result
            .phase
            .commits
            .checked_mul(configuration.transaction_size as u64)
            .ok_or_else(|| "expected measured mutation count overflow".to_owned())?
    } else {
        0
    };
    let expected_warmup_mutations = if configuration.workload.writes() {
        result
            .warmup_commits
            .checked_mul(configuration.workers as u64)
            .and_then(|value| value.checked_mul(configuration.transaction_size as u64))
            .ok_or_else(|| "expected warmup mutation count overflow".to_owned())?
    } else {
        0
    };
    let expected_total_mutations = (result.keyspace as u64)
        .checked_add(expected_warmup_mutations)
        .and_then(|value| value.checked_add(expected_measured_mutations))
        .ok_or_else(|| "expected total mutation count overflow".to_owned())?;
    if result.measured_mutation_bytes != expected_measured_mutations * bytes_per_mutation
        || result.total_mutation_bytes != expected_total_mutations * bytes_per_mutation
        || result.live_user_bytes != result.keyspace as u64 * bytes_per_mutation
    {
        return Err("child byte accounting is inconsistent".to_owned());
    }
    validate_storage_accounting(result)
}

fn benchmark_configurations(profile: Profile) -> Vec<Configuration> {
    let mut configurations = Vec::new();
    match profile {
        Profile::Smoke => {
            for arm in Arm::ALL {
                for (workload, transaction_size, contention) in [
                    (Workload::Read, 1, Contention::Low),
                    (Workload::Write, 4, Contention::Low),
                    (Workload::Rmw, 4, Contention::High),
                ] {
                    configurations.push(Configuration {
                        arm,
                        workload,
                        transaction_size,
                        contention,
                        workers: 2,
                    });
                }
            }
        }
        Profile::Acceptance => {
            for arm in Arm::ALL {
                for workload in Workload::ALL {
                    for transaction_size in [1, 4, 16, 64] {
                        for workers in [1, 4, 16] {
                            let contentions: &[Contention] = if workers == 1 {
                                &[Contention::Low]
                            } else {
                                &[Contention::Low, Contention::High]
                            };
                            for contention in contentions {
                                configurations.push(Configuration {
                                    arm,
                                    workload,
                                    transaction_size,
                                    contention: *contention,
                                    workers,
                                });
                            }
                        }
                    }
                }
            }
        }
    }
    configurations
}

fn target_commits(profile: Profile, transaction_size: usize) -> (u64, u64) {
    match profile {
        Profile::Smoke => (16, 4),
        Profile::Acceptance => (
            256usize.max(8192 / transaction_size) as u64,
            64usize.max(2048 / transaction_size) as u64,
        ),
    }
}

fn async_queue_capacity(configuration: Configuration) -> usize {
    match configuration.arm {
        // Mako reserves one queue record per transaction, whereas MRX reserves
        // one ticket per mutation. Scale Mako by transaction size so both can
        // absorb the same fixed mutation budget without timed backpressure.
        Arm::Mako => ASYNC_MUTATION_CAPACITY.div_ceil(configuration.transaction_size),
        Arm::Mrx => ASYNC_MUTATION_CAPACITY,
        Arm::Rocks => 0,
    }
}

fn async_queue_capacity_unit(arm: Arm) -> &'static str {
    match arm {
        Arm::Mako => "transaction_records",
        Arm::Mrx => "mutation_tickets",
        Arm::Rocks => "none",
    }
}

impl Checkpoint {
    fn open(options: &RunOptions, run_root: &Path) -> AnyResult<(Self, Vec<CombinedResult>)> {
        let Some(path) = options.checkpoint.as_deref() else {
            return Ok((Self { file: None }, Vec::new()));
        };
        let header = checkpoint_header(options);
        if options.resume {
            let (contents, durable_len, had_torn_tail) = read_checkpoint(path)?;
            let mut lines = contents.lines();
            if lines.next() != Some(header.as_str()) {
                return Err(format!(
                    "checkpoint {} belongs to a different profile, binary, machine, affinity, source, or native build",
                    path.display()
                ));
            }
            let mut results = Vec::new();
            let mut previous_run_root = None;
            for (index, line) in lines.enumerate() {
                if line.starts_with(CHECKPOINT_RUN_PROTOCOL) {
                    previous_run_root = Some(parse_checkpoint_run(line).map_err(|error| {
                        format!("checkpoint {} line {}: {error}", path.display(), index + 2)
                    })?);
                } else {
                    results.push(parse_checkpoint_result(line).map_err(|error| {
                        format!("checkpoint {} line {}: {error}", path.display(), index + 2)
                    })?);
                }
            }
            if let Some(previous_run_root) = previous_run_root {
                if options.keep_data {
                    eprintln!(
                        "prior interrupted data retained by --keep-data at {}",
                        previous_run_root.display()
                    );
                } else {
                    cleanup_stale_run_root(&options.data_root, &previous_run_root)?;
                }
            }
            let mut file = OpenOptions::new()
                .append(true)
                .open(path)
                .map_err(|error| format!("open checkpoint {}: {error}", path.display()))?;
            if had_torn_tail {
                file.set_len(durable_len).map_err(|error| {
                    format!("truncate torn checkpoint {}: {error}", path.display())
                })?;
                file.sync_data().map_err(|error| {
                    format!("sync truncated checkpoint {}: {error}", path.display())
                })?;
                eprintln!("discarded a torn final checkpoint line");
            }
            writeln!(file, "{}", encode_checkpoint_run(run_root))
                .map_err(|error| format!("append checkpoint run marker: {error}"))?;
            file.sync_data()
                .map_err(|error| format!("sync checkpoint run marker: {error}"))?;
            eprintln!(
                "resuming {} completed samples from {}",
                results.len(),
                path.display()
            );
            Ok((Self { file: Some(file) }, results))
        } else {
            let mut file = OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(path)
                .map_err(|error| {
                    format!(
                        "create checkpoint {}: {error}; pass --resume only for an intentional continuation",
                        path.display()
                    )
                })?;
            writeln!(file, "{header}")
                .map_err(|error| format!("write checkpoint {}: {error}", path.display()))?;
            writeln!(file, "{}", encode_checkpoint_run(run_root))
                .map_err(|error| format!("write checkpoint run marker: {error}"))?;
            file.sync_data()
                .map_err(|error| format!("sync checkpoint {}: {error}", path.display()))?;
            Ok((Self { file: Some(file) }, Vec::new()))
        }
    }

    fn append(&mut self, result: &CombinedResult) -> AnyResult<()> {
        let Some(file) = self.file.as_mut() else {
            return Ok(());
        };
        writeln!(file, "{}", encode_checkpoint_result(result))
            .map_err(|error| format!("append benchmark checkpoint: {error}"))?;
        file.sync_data()
            .map_err(|error| format!("sync benchmark checkpoint: {error}"))
    }
}

fn read_checkpoint(path: &Path) -> AnyResult<(String, u64, bool)> {
    let bytes =
        fs::read(path).map_err(|error| format!("read checkpoint {}: {error}", path.display()))?;
    let last_newline = bytes
        .iter()
        .rposition(|byte| *byte == b'\n')
        .ok_or_else(|| format!("checkpoint {} has no complete line", path.display()))?;
    let durable_len = last_newline + 1;
    let had_torn_tail = durable_len != bytes.len();
    let contents = String::from_utf8(bytes[..durable_len].to_vec())
        .map_err(|_| format!("checkpoint {} is not UTF-8", path.display()))?;
    Ok((contents, durable_len as u64, had_torn_tail))
}

fn encode_checkpoint_run(path: &Path) -> String {
    format!(
        "{CHECKPOINT_RUN_PROTOCOL} root={}",
        hex_encode(path.as_os_str().as_bytes())
    )
}

fn parse_checkpoint_run(line: &str) -> AnyResult<PathBuf> {
    let mut fields = protocol_fields(line, CHECKPOINT_RUN_PROTOCOL)?;
    let bytes = hex_decode(take_field(&mut fields, "root")?)?;
    reject_remaining_fields(fields)?;
    Ok(PathBuf::from(OsString::from_vec(bytes)))
}

fn checkpoint_header(options: &RunOptions) -> String {
    let canonical_data_root = options
        .data_root
        .canonicalize()
        .unwrap_or_else(|_| options.data_root.clone());
    format!(
        "{CHECKPOINT_PROTOCOL} profile={} binary={} host={} affinity={} data_root={} git={} fingerprint={}",
        options.profile.name(),
        executable_digest(),
        hex_encode(read_trimmed("/etc/hostname").as_bytes()),
        hex_encode(process_status_field("Cpus_allowed_list").as_bytes()),
        hex_encode(canonical_data_root.as_os_str().as_bytes()),
        command_line("git", &["rev-parse", "HEAD"]),
        native_fingerprint(),
    )
}

fn executable_digest() -> String {
    let Ok(path) = env::current_exe() else {
        return String::new();
    };
    let Ok(mut file) = File::open(path) else {
        return String::new();
    };
    let mut digest = 0xcbf2_9ce4_8422_2325u64;
    let mut buffer = [0u8; 64 * 1024];
    loop {
        let Ok(read) = file.read(&mut buffer) else {
            return String::new();
        };
        if read == 0 {
            break;
        }
        for byte in &buffer[..read] {
            digest ^= u64::from(*byte);
            digest = digest.wrapping_mul(0x0000_0100_0000_01b3);
        }
    }
    format!("{digest:016x}")
}

fn encode_checkpoint_result(result: &CombinedResult) -> String {
    format!(
        "{CHECKPOINT_RESULT_PROTOCOL} repetition={} sample={} recovery={}",
        result.repetition,
        hex_encode(encode_sample_protocol(&result.sample).as_bytes()),
        hex_encode(encode_recovery_protocol(&result.recovery).as_bytes()),
    )
}

fn parse_checkpoint_result(line: &str) -> AnyResult<CombinedResult> {
    let mut fields = protocol_fields(line, CHECKPOINT_RESULT_PROTOCOL)?;
    let repetition = take_usize(&mut fields, "repetition")?;
    let sample = String::from_utf8(hex_decode(take_field(&mut fields, "sample")?)?)
        .map_err(|_| "checkpoint sample is not UTF-8".to_owned())?;
    let recovery = String::from_utf8(hex_decode(take_field(&mut fields, "recovery")?)?)
        .map_err(|_| "checkpoint recovery is not UTF-8".to_owned())?;
    reject_remaining_fields(fields)?;
    let sample = parse_sample_protocol(&sample)?;
    let recovery = parse_recovery_protocol(&recovery)?;
    if sample.configuration != recovery.configuration
        || sample.keyspace != recovery.keyspace
        || sample.checksum != recovery.checksum
    {
        return Err("checkpoint sample and recovery disagree".to_owned());
    }
    Ok(CombinedResult {
        repetition,
        sample,
        recovery,
    })
}

fn hex_encode(bytes: &[u8]) -> String {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut output = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        output.push(DIGITS[(byte >> 4) as usize] as char);
        output.push(DIGITS[(byte & 0x0f) as usize] as char);
    }
    output
}

fn hex_decode(value: &str) -> AnyResult<Vec<u8>> {
    if !value.len().is_multiple_of(2) {
        return Err("checkpoint hex has odd length".to_owned());
    }
    value
        .as_bytes()
        .chunks_exact(2)
        .map(|pair| {
            let high = hex_digit(pair[0])?;
            let low = hex_digit(pair[1])?;
            Ok((high << 4) | low)
        })
        .collect()
}

fn hex_digit(byte: u8) -> AnyResult<u8> {
    match byte {
        b'0'..=b'9' => Ok(byte - b'0'),
        b'a'..=b'f' => Ok(byte - b'a' + 10),
        _ => Err("checkpoint hex is not canonical lowercase".to_owned()),
    }
}

fn run_orchestrator(options: RunOptions) -> AnyResult<()> {
    let run_root = create_run_root(&options.data_root)?;
    let (mut checkpoint, recovered) = match Checkpoint::open(&options, &run_root) {
        Ok(checkpoint) => checkpoint,
        Err(error) => {
            cleanup_generated_root(&run_root).map_err(|cleanup_error| {
                format!("{error}; additionally failed to remove unused run root: {cleanup_error}")
            })?;
            return Err(error);
        }
    };
    let result = run_orchestrator_inner(&options, &run_root, recovered, &mut checkpoint);
    match result {
        Ok(results) => {
            let report = encode_report(&options, &run_root, &results)?;
            write_report(options.output.as_deref(), &report)?;
            if options.keep_data {
                eprintln!("benchmark data retained at {}", run_root.display());
            } else {
                cleanup_generated_root(&run_root)?;
            }
            Ok(())
        }
        Err(error) => Err(format!(
            "{error}; incomplete benchmark data retained at {}",
            run_root.display()
        )),
    }
}

fn run_orchestrator_inner(
    options: &RunOptions,
    run_root: &Path,
    mut results: Vec<CombinedResult>,
    checkpoint: &mut Checkpoint,
) -> AnyResult<Vec<CombinedResult>> {
    let executable = env::current_exe().map_err(|error| format!("locate benchmark: {error}"))?;
    let configurations = benchmark_configurations(options.profile);
    results.reserve(
        configurations
            .len()
            .saturating_mul(options.profile.repetitions())
            .saturating_sub(results.len()),
    );
    let mut completed = validate_partial_results(options.profile, &configurations, &results)?;
    for repetition in 0..options.profile.repetitions() {
        let mut jobs = configurations.clone();
        deterministic_shuffle(&mut jobs, 0x6d61_6b6f_4d31_0001 ^ repetition as u64);
        for (ordinal, configuration) in jobs.into_iter().enumerate() {
            if completed.contains(&(repetition, configuration)) {
                continue;
            }
            let (target, warmup) = target_commits(options.profile, configuration.transaction_size);
            let path = run_root.join(format!(
                "r{repetition:02}-j{ordinal:03}-{}-{}-t{}-{}-w{}",
                configuration.arm.name(),
                configuration.workload.name(),
                configuration.transaction_size,
                configuration.contention.name(),
                configuration.workers,
            ));
            let spec = ChildSpec {
                configuration,
                target_commits: target,
                warmup_commits: warmup,
                path,
            };
            eprintln!(
                "[{}/{}] repetition {}: {} {} t{} {} w{}",
                results.len() + 1,
                configurations.len() * options.profile.repetitions(),
                repetition,
                configuration.arm.name(),
                configuration.workload.name(),
                configuration.transaction_size,
                configuration.contention.name(),
                configuration.workers,
            );
            let sample_output = run_child(
                &executable,
                &sample_arguments(&spec),
                options.profile.child_timeout(),
            )?;
            let sample = parse_sample_protocol(&sample_output)?;
            if sample.configuration != configuration {
                return Err("sample child returned the wrong configuration".to_owned());
            }
            let recovery_output = run_child(
                &executable,
                &recovery_arguments(&spec, sample.checksum),
                options.profile.child_timeout(),
            )?;
            let recovery = parse_recovery_protocol(&recovery_output)?;
            if recovery.configuration != configuration
                || recovery.keyspace != sample.keyspace
                || recovery.checksum != sample.checksum
            {
                return Err("recovery child disagrees with its sample".to_owned());
            }
            let combined = CombinedResult {
                repetition,
                sample,
                recovery,
            };
            if !options.keep_data {
                cleanup_database_path(&spec.path)?;
            }
            checkpoint.append(&combined)?;
            completed.insert((repetition, configuration));
            results.push(combined);
        }
    }
    validate_complete_results(options.profile, &configurations, &results)?;
    Ok(results)
}

fn sample_arguments(spec: &ChildSpec) -> Vec<String> {
    child_arguments("__sample", spec)
}

fn recovery_arguments(spec: &ChildSpec, checksum: u64) -> Vec<String> {
    let mut arguments = child_arguments("__recover", spec);
    arguments.push(checksum.to_string());
    arguments
}

fn child_arguments(role: &str, spec: &ChildSpec) -> Vec<String> {
    vec![
        role.to_owned(),
        spec.configuration.arm.name().to_owned(),
        spec.configuration.workload.name().to_owned(),
        spec.configuration.transaction_size.to_string(),
        spec.configuration.contention.name().to_owned(),
        spec.configuration.workers.to_string(),
        spec.target_commits.to_string(),
        spec.warmup_commits.to_string(),
        spec.path.as_os_str().to_string_lossy().into_owned(),
    ]
}

fn run_child(executable: &Path, arguments: &[String], timeout: Duration) -> AnyResult<String> {
    let mut child = Command::new(executable)
        .args(arguments)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|error| format!("start benchmark child: {error}"))?;
    let stdout = child
        .stdout
        .take()
        .ok_or_else(|| "benchmark child has no stdout pipe".to_owned())?;
    let stderr = child
        .stderr
        .take()
        .ok_or_else(|| "benchmark child has no stderr pipe".to_owned())?;
    // Drain both pipes while the process runs. Waiting first can deadlock when
    // a native diagnostic fills either pipe before the child exits.
    let stdout_reader = thread::spawn(move || read_child_stream(stdout, "stdout"));
    let stderr_reader = thread::spawn(move || read_child_stream(stderr, "stderr"));
    let deadline = Instant::now() + timeout;
    let (status, timed_out) = loop {
        if let Some(status) = child
            .try_wait()
            .map_err(|error| format!("poll benchmark child: {error}"))?
        {
            break (status, false);
        }
        if Instant::now() >= deadline {
            if let Err(kill_error) = child.kill() {
                let still_running = child
                    .try_wait()
                    .map_err(|error| format!("poll child after kill failure: {error}"))?
                    .is_none();
                if still_running {
                    return Err(format!(
                        "could not kill timed-out benchmark child: {kill_error}"
                    ));
                }
            }
            let status = child
                .wait()
                .map_err(|error| format!("reap timed-out benchmark child: {error}"))?;
            break (status, true);
        }
        thread::sleep(Duration::from_millis(20));
    };
    let stdout = join_child_reader(stdout_reader, "stdout")?;
    let stderr = join_child_reader(stderr_reader, "stderr")?;
    if timed_out {
        return Err(format!(
            "benchmark child timed out after {} seconds (status {status}): {}",
            timeout.as_secs(),
            child_diagnostic(&stderr)
        ));
    }
    require_child_success(status, &stderr)?;
    if stdout.total_bytes > 64 * 1024 {
        return Err(format!(
            "benchmark child output exceeded 64 KiB ({} bytes)",
            stdout.total_bytes
        ));
    }
    String::from_utf8(stdout.bytes).map_err(|_| "benchmark child output is not UTF-8".to_owned())
}

fn read_child_stream<R: Read>(mut stream: R, label: &str) -> AnyResult<ChildCapture> {
    let mut bytes = Vec::with_capacity(CHILD_CAPTURE_BYTES);
    let mut total_bytes = 0u64;
    let mut buffer = [0u8; 16 * 1024];
    loop {
        let read = stream
            .read(&mut buffer)
            .map_err(|error| format!("read benchmark child {label}: {error}"))?;
        if read == 0 {
            break;
        }
        total_bytes = total_bytes.saturating_add(read as u64);
        if read >= CHILD_CAPTURE_BYTES {
            bytes.clear();
            bytes.extend_from_slice(&buffer[read - CHILD_CAPTURE_BYTES..read]);
        } else {
            let overflow = bytes
                .len()
                .saturating_add(read)
                .saturating_sub(CHILD_CAPTURE_BYTES);
            if overflow != 0 {
                bytes.copy_within(overflow.., 0);
                bytes.truncate(bytes.len() - overflow);
            }
            bytes.extend_from_slice(&buffer[..read]);
        }
    }
    Ok(ChildCapture { bytes, total_bytes })
}

fn join_child_reader(
    reader: thread::JoinHandle<AnyResult<ChildCapture>>,
    label: &str,
) -> AnyResult<ChildCapture> {
    reader
        .join()
        .map_err(|_| format!("benchmark child {label} reader panicked"))?
}

fn child_diagnostic(capture: &ChildCapture) -> String {
    let diagnostic = String::from_utf8_lossy(&capture.bytes);
    if capture.total_bytes > capture.bytes.len() as u64 {
        format!(
            "[{} earlier bytes omitted] {}",
            capture.total_bytes - capture.bytes.len() as u64,
            diagnostic.trim()
        )
    } else {
        diagnostic.trim().to_owned()
    }
}

fn require_child_success(status: ExitStatus, stderr: &ChildCapture) -> AnyResult<()> {
    if status.success() {
        return Ok(());
    }
    Err(format!(
        "benchmark child exited with {status}: {}",
        child_diagnostic(stderr)
    ))
}

fn deterministic_shuffle<T>(values: &mut [T], mut state: u64) {
    for index in (1..values.len()).rev() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        let selected = (state as usize) % (index + 1);
        values.swap(index, selected);
    }
}

fn create_run_root(data_root: &Path) -> AnyResult<PathBuf> {
    let epoch = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|_| "system clock is before Unix epoch".to_owned())?
        .as_nanos();
    for suffix in 0..100u32 {
        let path = data_root.join(format!(
            "mako-m1-bench-{}-{epoch}-{suffix}",
            std::process::id()
        ));
        match fs::create_dir(&path) {
            Ok(()) => return Ok(path),
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(error) => return Err(format!("create {}: {error}", path.display())),
        }
    }
    Err("could not allocate a unique benchmark data directory".to_owned())
}

fn cleanup_database_path(path: &Path) -> AnyResult<()> {
    let name = path
        .file_name()
        .and_then(|value| value.to_str())
        .ok_or_else(|| "benchmark database path has no UTF-8 file name".to_owned())?;
    if !name.starts_with('r') || !name.contains("-j") {
        return Err(format!(
            "refusing to remove unexpected path {}",
            path.display()
        ));
    }
    fs::remove_dir_all(path).map_err(|error| format!("remove {}: {error}", path.display()))
}

fn cleanup_stale_run_root(data_root: &Path, path: &Path) -> AnyResult<()> {
    let expected_parent = data_root
        .canonicalize()
        .map_err(|error| format!("canonicalize {}: {error}", data_root.display()))?;
    let parent = path
        .parent()
        .ok_or_else(|| "checkpoint run root has no parent".to_owned())?
        .canonicalize()
        .map_err(|error| format!("canonicalize checkpoint parent: {error}"))?;
    let name = path
        .file_name()
        .and_then(|value| value.to_str())
        .ok_or_else(|| "checkpoint run root has no UTF-8 file name".to_owned())?;
    if parent != expected_parent || !is_generated_root_name(name) {
        return Err(format!(
            "refusing to remove checkpoint path outside the benchmark root: {}",
            path.display()
        ));
    }
    if path.exists() {
        fs::remove_dir_all(path)
            .map_err(|error| format!("remove stale benchmark run {}: {error}", path.display()))?;
        eprintln!("removed stale benchmark run {}", path.display());
    }
    Ok(())
}

fn cleanup_generated_root(path: &Path) -> AnyResult<()> {
    let name = path
        .file_name()
        .and_then(|value| value.to_str())
        .ok_or_else(|| "benchmark root has no UTF-8 file name".to_owned())?;
    if !is_generated_root_name(name) {
        return Err(format!(
            "refusing to remove unexpected root {}",
            path.display()
        ));
    }
    fs::remove_dir(path).map_err(|error| format!("remove {}: {error}", path.display()))
}

fn is_generated_root_name(name: &str) -> bool {
    let Some(suffix) = name.strip_prefix("mako-m1-bench-") else {
        return false;
    };
    let mut fields = suffix.split('-');
    let valid_number = |field: Option<&str>| {
        field.is_some_and(|value| {
            !value.is_empty() && value.bytes().all(|byte| byte.is_ascii_digit())
        })
    };
    valid_number(fields.next())
        && valid_number(fields.next())
        && valid_number(fields.next())
        && fields.next().is_none()
}

fn validate_partial_results(
    profile: Profile,
    configurations: &[Configuration],
    results: &[CombinedResult],
) -> AnyResult<BTreeSet<(usize, Configuration)>> {
    let expected_set: BTreeSet<_> = configurations.iter().copied().collect();
    let mut completed = BTreeSet::new();
    for result in results {
        let configuration = result.sample.configuration;
        if result.repetition >= profile.repetitions() {
            return Err(format!(
                "checkpoint repetition {} is outside profile range",
                result.repetition
            ));
        }
        if !expected_set.contains(&configuration) {
            return Err("checkpoint contains an unexpected configuration".to_owned());
        }
        let (target, warmup) = target_commits(profile, configuration.transaction_size);
        if result.sample.target_commits != target || result.sample.warmup_commits != warmup {
            return Err("checkpoint uses a different workload duration".to_owned());
        }
        validate_sample_result(&result.sample)?;
        if result.recovery.configuration != configuration
            || result.recovery.keyspace != result.sample.keyspace
            || result.recovery.checksum != result.sample.checksum
        {
            return Err("checkpoint sample and recovery disagree".to_owned());
        }
        if !completed.insert((result.repetition, configuration)) {
            return Err("checkpoint contains duplicate results".to_owned());
        }
    }
    Ok(completed)
}

fn validate_complete_results(
    profile: Profile,
    configurations: &[Configuration],
    results: &[CombinedResult],
) -> AnyResult<()> {
    let expected = configurations.len() * profile.repetitions();
    if results.len() != expected {
        return Err(format!(
            "benchmark result matrix is incomplete: expected {expected}, found {}",
            results.len()
        ));
    }
    validate_partial_results(profile, configurations, results)?;
    for configuration in configurations {
        if configuration.arm == Arm::Mako
            && configuration.contention == Contention::High
            && configuration.workers > 1
            && configuration.workload.writes()
        {
            let conflicts: u64 = results
                .iter()
                .filter(|result| result.sample.configuration == *configuration)
                .map(|result| result.sample.phase.conflicts)
                .sum();
            if conflicts == 0 {
                return Err(format!(
                    "Mako high-contention {} t{} w{} generated no conflicts",
                    configuration.workload.name(),
                    configuration.transaction_size,
                    configuration.workers
                ));
            }
        }
    }
    Ok(())
}

fn validate_storage_accounting(sample: &SampleResult) -> AnyResult<()> {
    let configuration = sample.configuration;
    let expected_data = configuration.keyspace() as u64;
    if sample.backend.data_keys != expected_data || sample.backend.physical_bytes == 0 {
        return Err("backend data-key or allocated-byte accounting is invalid".to_owned());
    }
    let expected_logs = if configuration.arm == Arm::Mako {
        let seed_records = configuration.keyspace().div_ceil(SEED_BATCH) as u64;
        if configuration.workload.writes() {
            seed_records
                + sample.warmup_commits * configuration.workers as u64
                + sample.phase.commits
        } else {
            seed_records
        }
    } else {
        0
    };
    if sample.backend.log_keys != expected_logs {
        return Err(format!(
            "backend has {} log records, expected {expected_logs}",
            sample.backend.log_keys
        ));
    }
    Ok(())
}

fn encode_report(
    options: &RunOptions,
    run_root: &Path,
    results: &[CombinedResult],
) -> AnyResult<String> {
    let mut output = String::with_capacity(results.len().saturating_mul(1_200));
    output.push_str("{\n  \"protocol\":");
    push_json_string(&mut output, REPORT_PROTOCOL);
    output.push_str(",\n  \"completed\":true");
    output.push_str(",\n  \"profile\":");
    push_json_string(&mut output, options.profile.name());
    output.push_str(",\n  \"metadata\":{");
    json_string_field(
        &mut output,
        "hostname",
        &read_trimmed("/etc/hostname"),
        true,
    );
    json_string_field(
        &mut output,
        "kernel",
        &read_trimmed("/proc/sys/kernel/osrelease"),
        false,
    );
    json_string_field(&mut output, "cpu_model", &cpu_model(), false);
    json_string_field(
        &mut output,
        "cpu_affinity",
        &process_status_field("Cpus_allowed_list"),
        false,
    );
    json_string_field(
        &mut output,
        "load_average",
        &read_trimmed("/proc/loadavg"),
        false,
    );
    json_number_field(
        &mut output,
        "available_parallelism",
        thread::available_parallelism().map_or(0, usize::from) as u64,
        false,
    );
    json_string_field(
        &mut output,
        "git_head",
        &command_line("git", &["rev-parse", "HEAD"]),
        false,
    );
    json_string_field(
        &mut output,
        "git_worktree_state",
        if command_line(
            "git",
            &["status", "--porcelain=v1", "--untracked-files=all"],
        )
        .is_empty()
        {
            "clean"
        } else {
            "dirty"
        },
        false,
    );
    json_string_field(
        &mut output,
        "benchmark_executable_digest_fnv1a64",
        &executable_digest(),
        false,
    );
    json_string_field(
        &mut output,
        "rustc",
        &command_line("rustc", &["--version"]),
        false,
    );
    json_string_field(
        &mut output,
        "native_build_dir",
        &env::var("MAKO_BUILD_DIR").unwrap_or_default(),
        false,
    );
    json_string_field(
        &mut output,
        "native_fingerprint",
        &native_fingerprint(),
        false,
    );
    json_string_field(
        &mut output,
        "data_root",
        &options.data_root.as_os_str().to_string_lossy(),
        false,
    );
    json_string_field(
        &mut output,
        "run_directory",
        &run_root.as_os_str().to_string_lossy(),
        false,
    );
    let generated = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|_| "system clock is before Unix epoch".to_owned())?
        .as_secs();
    json_number_field(&mut output, "generated_unix_seconds", generated, false);
    output.push_str("},\n  \"methodology\":{");
    json_number_field(
        &mut output,
        "repetitions",
        options.profile.repetitions() as u64,
        true,
    );
    json_number_field(&mut output, "value_bytes", VALUE_BYTES as u64, false);
    json_number_field(&mut output, "retry_multiplier", RETRY_MULTIPLIER, false);
    json_number_field(
        &mut output,
        "forced_collision_rounds",
        FORCED_COLLISION_ROUNDS,
        false,
    );
    json_number_field(
        &mut output,
        "async_mutation_capacity",
        ASYNC_MUTATION_CAPACITY as u64,
        false,
    );
    json_string_field(
        &mut output,
        "async_capacity_rule",
        "MRX uses one ticket per mutation; Mako uses ceil(async_mutation_capacity / transaction_size) transaction records; every measured phase fits without queue backpressure",
        false,
    );
    json_string_field(&mut output, "rocksdb_durability", "wal_sync_false", false);
    json_string_field(
        &mut output,
        "ack_scope",
        "foreground calls through transaction acknowledgement",
        false,
    );
    json_string_field(
        &mut output,
        "value_cache_mode",
        "unbounded; Phase 1G eviction is explicitly deferred and each dataset fits in RAM",
        false,
    );
    json_string_field(
        &mut output,
        "applied_scope",
        "seed and warmup are drained before timing; measured ACK duration plus its immediate applied-barrier drain; not a per-transaction applied percentile; no WAL sync or memtable flush",
        false,
    );
    json_string_field(
        &mut output,
        "latency_scope",
        "logical transaction latency includes all OCC retries; attempt percentiles are also retained",
        false,
    );
    json_string_field(
        &mut output,
        "recovery_scope",
        "warm page cache after the uniform post-timing memtable flush used for storage accounting; open-only and open-plus-full-validation reported separately",
        false,
    );
    json_string_field(
        &mut output,
        "log_amplification",
        "unreclaimed persisted commit-record key and value bytes (reported separately) divided by all seeded/warmup/measured user mutation bytes",
        false,
    );
    json_string_field(
        &mut output,
        "semantic_rule",
        "only size-one read/write rows share a point contract; all weaker multi-key/RMW baselines are labeled",
        false,
    );
    output.push_str("},\n  \"samples\":[\n");
    for (index, result) in results.iter().enumerate() {
        if index != 0 {
            output.push_str(",\n");
        }
        encode_combined_json(&mut output, result);
    }
    output.push_str("\n  ],\n  \"summaries\":[\n");
    let grouped = group_results(results);
    for (index, (configuration, group)) in grouped.iter().enumerate() {
        if index != 0 {
            output.push_str(",\n");
        }
        encode_summary_json(&mut output, *configuration, group);
    }
    output.push_str("\n  ]\n}\n");
    Ok(output)
}

fn encode_combined_json(output: &mut String, result: &CombinedResult) {
    let sample = &result.sample;
    let recovery = &result.recovery;
    let configuration = sample.configuration;
    let attempts = sample.phase.commits + sample.phase.conflicts;
    let end_to_end_ns = sample.phase.duration_ns.saturating_add(sample.drain_ns);
    let log_bytes = sample.backend.log_key_bytes + sample.backend.log_value_bytes;
    output.push_str("    {");
    json_number_field(output, "repetition", result.repetition as u64, true);
    json_string_field(output, "arm", configuration.arm.name(), false);
    json_string_field(output, "workload", configuration.workload.name(), false);
    json_number_field(
        output,
        "transaction_size",
        configuration.transaction_size as u64,
        false,
    );
    json_string_field(output, "contention", configuration.contention.name(), false);
    json_number_field(output, "workers", configuration.workers as u64, false);
    json_string_field(
        output,
        "semantic_class",
        configuration.semantic_class(),
        false,
    );
    json_number_field(
        output,
        "async_queue_capacity",
        async_queue_capacity(configuration) as u64,
        false,
    );
    json_string_field(
        output,
        "async_queue_capacity_unit",
        async_queue_capacity_unit(configuration.arm),
        false,
    );
    json_number_field(
        output,
        "target_commits_per_worker",
        sample.target_commits,
        false,
    );
    json_number_field(
        output,
        "warmup_commits_per_worker",
        sample.warmup_commits,
        false,
    );
    json_number_field(output, "commits", sample.phase.commits, false);
    json_number_field(output, "conflicts", sample.phase.conflicts, false);
    json_float_field(
        output,
        "abort_rate",
        ratio(sample.phase.conflicts, attempts),
        false,
    );
    json_number_field(output, "ack_duration_ns", sample.phase.duration_ns, false);
    json_number_field(output, "drain_duration_ns", sample.drain_ns, false);
    json_number_field(output, "ack_applied_duration_ns", end_to_end_ns, false);
    json_float_field(
        output,
        "ack_transactions_per_second",
        rate(sample.phase.commits, sample.phase.duration_ns),
        false,
    );
    json_float_field(
        output,
        "applied_transactions_per_second",
        rate(sample.phase.commits, end_to_end_ns),
        false,
    );
    json_float_field(
        output,
        "logical_operations_per_second",
        rate(sample.logical_operations, sample.phase.duration_ns),
        false,
    );
    json_number_field(output, "attempt_p50_ns", sample.phase.attempt_p50_ns, false);
    json_number_field(output, "attempt_p99_ns", sample.phase.attempt_p99_ns, false);
    json_number_field(output, "logical_p50_ns", sample.phase.logical_p50_ns, false);
    json_number_field(output, "logical_p99_ns", sample.phase.logical_p99_ns, false);
    json_number_field(output, "recovery_open_ns", recovery.open_ns, false);
    json_number_field(
        output,
        "recovery_open_validate_ns",
        recovery.open_ns.saturating_add(recovery.validation_ns),
        false,
    );
    json_number_field(
        output,
        "measured_mutation_bytes",
        sample.measured_mutation_bytes,
        false,
    );
    json_number_field(
        output,
        "total_mutation_bytes",
        sample.total_mutation_bytes,
        false,
    );
    json_number_field(
        output,
        "backend_log_key_bytes",
        sample.backend.log_key_bytes,
        false,
    );
    json_number_field(
        output,
        "backend_log_value_bytes",
        sample.backend.log_value_bytes,
        false,
    );
    json_number_field(output, "backend_log_bytes", log_bytes, false);
    json_float_field(
        output,
        "log_amplification",
        ratio(log_bytes, sample.total_mutation_bytes),
        false,
    );
    json_number_field(
        output,
        "backend_logical_bytes",
        sample.backend.key_bytes + sample.backend.value_bytes,
        false,
    );
    json_float_field(
        output,
        "backend_logical_amplification",
        ratio(
            sample.backend.key_bytes + sample.backend.value_bytes,
            sample.live_user_bytes,
        ),
        false,
    );
    json_number_field(
        output,
        "backend_allocated_bytes",
        sample.backend.physical_bytes,
        false,
    );
    json_float_field(
        output,
        "allocated_amplification",
        ratio(sample.backend.physical_bytes, sample.live_user_bytes),
        false,
    );
    json_number_field(output, "backend_keys", sample.backend.keys, false);
    json_number_field(output, "commit_records", sample.backend.log_keys, false);
    json_number_field(output, "validation_checksum", sample.checksum, false);
    output.push('}');
}

fn group_results(results: &[CombinedResult]) -> BTreeMap<Configuration, Vec<&CombinedResult>> {
    let mut groups = BTreeMap::<Configuration, Vec<&CombinedResult>>::new();
    for result in results {
        groups
            .entry(result.sample.configuration)
            .or_default()
            .push(result);
    }
    groups
}

fn encode_summary_json(
    output: &mut String,
    configuration: Configuration,
    results: &[&CombinedResult],
) {
    let values = |measure: fn(&CombinedResult) -> f64| {
        results
            .iter()
            .map(|result| measure(result))
            .collect::<Vec<_>>()
    };
    output.push_str("    {");
    json_string_field(output, "arm", configuration.arm.name(), true);
    json_string_field(output, "workload", configuration.workload.name(), false);
    json_number_field(
        output,
        "transaction_size",
        configuration.transaction_size as u64,
        false,
    );
    json_string_field(output, "contention", configuration.contention.name(), false);
    json_number_field(output, "workers", configuration.workers as u64, false);
    json_string_field(
        output,
        "semantic_class",
        configuration.semantic_class(),
        false,
    );
    json_number_field(output, "samples", results.len() as u64, false);
    json_float_field(
        output,
        "median_ack_transactions_per_second",
        median(values(|result| {
            rate(result.sample.phase.commits, result.sample.phase.duration_ns)
        })),
        false,
    );
    json_float_field(
        output,
        "median_applied_transactions_per_second",
        median(values(|result| {
            rate(
                result.sample.phase.commits,
                result
                    .sample
                    .phase
                    .duration_ns
                    .saturating_add(result.sample.drain_ns),
            )
        })),
        false,
    );
    json_float_field(
        output,
        "median_abort_rate",
        median(values(|result| {
            ratio(
                result.sample.phase.conflicts,
                result.sample.phase.commits + result.sample.phase.conflicts,
            )
        })),
        false,
    );
    json_float_field(
        output,
        "median_logical_p50_ns",
        median(values(|result| result.sample.phase.logical_p50_ns as f64)),
        false,
    );
    json_float_field(
        output,
        "median_logical_p99_ns",
        median(values(|result| result.sample.phase.logical_p99_ns as f64)),
        false,
    );
    json_float_field(
        output,
        "median_recovery_open_ns",
        median(values(|result| result.recovery.open_ns as f64)),
        false,
    );
    json_float_field(
        output,
        "median_recovery_open_validate_ns",
        median(values(|result| {
            result
                .recovery
                .open_ns
                .saturating_add(result.recovery.validation_ns) as f64
        })),
        false,
    );
    json_float_field(
        output,
        "median_log_amplification",
        median(values(|result| {
            ratio(
                result.sample.backend.log_key_bytes + result.sample.backend.log_value_bytes,
                result.sample.total_mutation_bytes,
            )
        })),
        false,
    );
    output.push('}');
}

fn rate(count: u64, duration_ns: u64) -> f64 {
    if duration_ns == 0 {
        0.0
    } else {
        count as f64 * 1_000_000_000.0 / duration_ns as f64
    }
}

fn ratio(numerator: u64, denominator: u64) -> f64 {
    if denominator == 0 {
        0.0
    } else {
        numerator as f64 / denominator as f64
    }
}

fn median(mut values: Vec<f64>) -> f64 {
    if values.is_empty() {
        return 0.0;
    }
    values.sort_by(f64::total_cmp);
    if values.len().is_multiple_of(2) {
        (values[values.len() / 2 - 1] + values[values.len() / 2]) / 2.0
    } else {
        values[values.len() / 2]
    }
}

fn json_string_field(output: &mut String, key: &str, value: &str, first: bool) {
    if !first {
        output.push(',');
    }
    push_json_string(output, key);
    output.push(':');
    push_json_string(output, value);
}

fn json_number_field(output: &mut String, key: &str, value: u64, first: bool) {
    if !first {
        output.push(',');
    }
    push_json_string(output, key);
    write!(output, ":{value}").expect("writing to String cannot fail");
}

fn json_float_field(output: &mut String, key: &str, value: f64, first: bool) {
    assert!(value.is_finite(), "benchmark metrics must be finite");
    if !first {
        output.push(',');
    }
    push_json_string(output, key);
    write!(output, ":{value:.9}").expect("writing to String cannot fail");
}

fn push_json_string(output: &mut String, value: &str) {
    output.push('"');
    for character in value.chars() {
        match character {
            '"' => output.push_str("\\\""),
            '\\' => output.push_str("\\\\"),
            '\n' => output.push_str("\\n"),
            '\r' => output.push_str("\\r"),
            '\t' => output.push_str("\\t"),
            character if character <= '\u{1f}' => {
                write!(output, "\\u{:04x}", character as u32)
                    .expect("writing to String cannot fail");
            }
            character => output.push(character),
        }
    }
    output.push('"');
}

fn write_report(path: Option<&Path>, report: &str) -> AnyResult<()> {
    match path {
        Some(path) => {
            let mut file = OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(path)
                .map_err(|error| format!("create report {}: {error}", path.display()))?;
            file.write_all(report.as_bytes())
                .map_err(|error| format!("write report {}: {error}", path.display()))?;
            file.sync_all()
                .map_err(|error| format!("sync report {}: {error}", path.display()))?;
            eprintln!("benchmark report written to {}", path.display());
        }
        None => print!("{report}"),
    }
    Ok(())
}

fn read_trimmed(path: &str) -> String {
    fs::read_to_string(path)
        .map(|value| value.trim().to_owned())
        .unwrap_or_default()
}

fn cpu_model() -> String {
    fs::read_to_string("/proc/cpuinfo")
        .ok()
        .and_then(|contents| {
            contents.lines().find_map(|line| {
                let (key, value) = line.split_once(':')?;
                (key.trim() == "model name").then(|| value.trim().to_owned())
            })
        })
        .unwrap_or_default()
}

fn process_status_field(wanted: &str) -> String {
    fs::read_to_string("/proc/self/status")
        .ok()
        .and_then(|contents| {
            contents.lines().find_map(|line| {
                let (key, value) = line.split_once(':')?;
                (key == wanted).then(|| value.trim().to_owned())
            })
        })
        .unwrap_or_default()
}

fn command_line(program: &str, arguments: &[&str]) -> String {
    Command::new(program)
        .args(arguments)
        .current_dir(repository_root())
        .output()
        .ok()
        .filter(|output| output.status.success())
        .and_then(|output| String::from_utf8(output.stdout).ok())
        .map(|output| output.trim().to_owned())
        .unwrap_or_default()
}

fn repository_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .and_then(Path::parent)
        .expect("benchmark crate lives below repository root")
        .to_path_buf()
}

fn native_fingerprint() -> String {
    let Some(build) = env::var_os("MAKO_BUILD_DIR") else {
        return String::new();
    };
    let manifest = PathBuf::from(build).join("generated/mako_local_build_manifest.json");
    let Ok(contents) = fs::read_to_string(manifest) else {
        return String::new();
    };
    extract_json_string(&contents, "fingerprint").unwrap_or_default()
}

fn extract_json_string(document: &str, key: &str) -> Option<String> {
    let needle = format!("\"{key}\"");
    let rest = document.split_once(&needle)?.1;
    let rest = rest.trim_start().strip_prefix(':')?.trim_start();
    let quoted = rest.strip_prefix('"')?;
    let end = quoted.find('"')?;
    Some(quoted[..end].to_owned())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    fn sample() -> SampleResult {
        let configuration = Configuration {
            arm: Arm::Mako,
            workload: Workload::Write,
            transaction_size: 4,
            contention: Contention::Low,
            workers: 2,
        };
        let keyspace = configuration.keyspace();
        let target_commits = 16;
        let warmup_commits = 4;
        let commits = target_commits * configuration.workers as u64;
        let log_keys = keyspace.div_ceil(SEED_BATCH) as u64
            + warmup_commits * configuration.workers as u64
            + commits;
        SampleResult {
            configuration,
            target_commits,
            warmup_commits,
            keyspace,
            warmup_conflicts: 3,
            phase: PhaseStats {
                duration_ns: 1_000_000,
                commits,
                conflicts: 7,
                attempt_p50_ns: 100,
                attempt_p99_ns: 900,
                logical_p50_ns: 150,
                logical_p99_ns: 1_200,
            },
            drain_ns: 50_000,
            logical_operations: commits * configuration.transaction_size as u64,
            measured_mutation_bytes: commits
                * configuration.transaction_size as u64
                * (KEY_BYTES + VALUE_BYTES) as u64,
            total_mutation_bytes: 91_392,
            live_user_bytes: keyspace as u64 * (KEY_BYTES + VALUE_BYTES) as u64,
            checksum: 123,
            backend: BackendStats {
                keys: keyspace as u64 + log_keys,
                key_bytes: 20_000,
                value_bytes: 70_000,
                log_keys,
                log_key_bytes: 2_000,
                log_value_bytes: 40_000,
                data_keys: keyspace as u64,
                physical_bytes: 131_072,
            },
        }
    }

    fn combined() -> CombinedResult {
        let sample = sample();
        let recovery = RecoveryResult {
            configuration: sample.configuration,
            keyspace: sample.keyspace,
            open_ns: 123,
            validation_ns: 456,
            checksum: sample.checksum,
        };
        CombinedResult {
            repetition: 0,
            sample,
            recovery,
        }
    }

    fn temporary_directory(label: &str) -> PathBuf {
        let epoch = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let path = env::temp_dir().join(format!(
            "mako-cache-bench-test-{label}-{}-{epoch}",
            std::process::id()
        ));
        fs::create_dir(&path).unwrap();
        path
    }

    fn checkpoint_options(root: &Path, checkpoint: PathBuf, resume: bool) -> RunOptions {
        RunOptions {
            profile: Profile::Smoke,
            data_root: root.to_path_buf(),
            output: None,
            checkpoint: Some(checkpoint),
            resume,
            keep_data: false,
        }
    }

    #[test]
    fn child_sample_protocol_round_trips_and_is_strict() {
        let expected = sample();
        let encoded = encode_sample_protocol(&expected);
        let actual = parse_sample_protocol(&(encoded.clone() + "\n")).unwrap();
        assert_eq!(actual.configuration, expected.configuration);
        assert_eq!(actual.phase.commits, expected.phase.commits);
        assert_eq!(
            actual.backend.log_value_bytes,
            expected.backend.log_value_bytes
        );

        assert!(parse_sample_protocol(&(encoded.clone() + "\n\n")).is_err());
        assert!(parse_sample_protocol(&encoded.replacen(" arm=", "  arm=", 1)).is_err());
        assert!(parse_sample_protocol(&(encoded + " extra=1")).is_err());
    }

    #[test]
    fn child_recovery_protocol_round_trips() {
        let expected = RecoveryResult {
            configuration: sample().configuration,
            keyspace: sample().keyspace,
            open_ns: 123,
            validation_ns: 456,
            checksum: 789,
        };
        let actual =
            parse_recovery_protocol(&(encode_recovery_protocol(&expected) + "\n")).unwrap();
        assert_eq!(actual.configuration, expected.configuration);
        assert_eq!(actual.open_ns, 123);
        assert_eq!(actual.checksum, 789);
    }

    #[test]
    fn percentile_uses_nearest_rank_and_preserves_bounds() {
        let mut values = vec![100, 1, 99, 2];
        assert_eq!(percentile(&mut values, 50), 2);
        assert_eq!(percentile(&mut values, 99), 100);
        assert_eq!(percentile(&mut [], 99), 0);
    }

    #[test]
    fn semantic_classes_never_claim_equivalence_for_weaker_transactions() {
        let mut configuration = sample().configuration;
        configuration.arm = Arm::Mrx;
        assert_eq!(
            configuration.semantic_class(),
            "weaker_nonatomic_no_occ_baseline"
        );
        configuration.arm = Arm::Rocks;
        assert_eq!(
            configuration.semantic_class(),
            "weaker_atomic_batch_no_occ_baseline"
        );
        configuration.transaction_size = 1;
        assert_eq!(configuration.semantic_class(), "common_point_contract");
        configuration.workload = Workload::Rmw;
        assert_ne!(configuration.semantic_class(), "common_point_contract");
    }

    #[test]
    fn acceptance_matrix_is_complete_without_useless_single_worker_conflict_rows() {
        let configurations = benchmark_configurations(Profile::Acceptance);
        assert_eq!(configurations.len(), 180);
        assert_eq!(
            configurations
                .iter()
                .copied()
                .collect::<BTreeSet<_>>()
                .len(),
            configurations.len()
        );
        assert!(configurations.iter().all(|configuration| {
            configuration.workers != 1 || configuration.contention == Contention::Low
        }));
        assert_eq!(
            configurations.len() * Profile::Acceptance.repetitions(),
            1_260
        );
        for configuration in configurations {
            let (target, _) = target_commits(Profile::Acceptance, configuration.transaction_size);
            let measured_mutations =
                target * configuration.workers as u64 * configuration.transaction_size as u64;
            assert!(measured_mutations <= ASYNC_MUTATION_CAPACITY as u64);
            match configuration.arm {
                Arm::Mako => assert!(
                    async_queue_capacity(configuration) * configuration.transaction_size
                        >= ASYNC_MUTATION_CAPACITY
                ),
                Arm::Mrx => {
                    assert_eq!(async_queue_capacity(configuration), ASYNC_MUTATION_CAPACITY)
                }
                Arm::Rocks => assert_eq!(async_queue_capacity(configuration), 0),
            }
        }
    }

    #[test]
    fn deterministic_shuffle_is_a_replayable_permutation() {
        let mut first: Vec<_> = (0..100).collect();
        let mut second = first.clone();
        deterministic_shuffle(&mut first, 42);
        deterministic_shuffle(&mut second, 42);
        assert_eq!(first, second);
        assert_ne!(first, (0..100).collect::<Vec<_>>());
        first.sort_unstable();
        assert_eq!(first, (0..100).collect::<Vec<_>>());
    }

    #[test]
    fn json_strings_escape_every_control_boundary() {
        let mut output = String::new();
        push_json_string(&mut output, "a\"b\\c\n\t\u{1f}");
        assert_eq!(output, "\"a\\\"b\\\\c\\n\\t\\u001f\"");
        assert_eq!(
            extract_json_string("{ \"fingerprint\" : \"abc123\" }", "fingerprint"),
            Some("abc123".to_owned())
        );
    }

    #[test]
    fn storage_gate_requires_exact_mako_record_count() {
        let valid = sample();
        validate_storage_accounting(&valid).unwrap();
        let mut invalid = valid.clone();
        invalid.backend.log_keys -= 1;
        invalid.backend.keys -= 1;
        assert!(validate_storage_accounting(&invalid).is_err());
    }

    #[test]
    fn child_arguments_are_lossless() {
        let expected = ChildSpec {
            configuration: sample().configuration,
            target_commits: 16,
            warmup_commits: 4,
            path: PathBuf::from("/tmp/mako benchmark path"),
        };
        let arguments = child_arguments("__sample", &expected);
        let actual = parse_child_spec(&arguments[1..]).unwrap();
        assert_eq!(actual.configuration, expected.configuration);
        assert_eq!(actual.path, expected.path);
    }

    #[test]
    fn checkpoint_resume_discards_only_a_torn_tail_and_stale_generated_root() {
        let root = temporary_directory("resume");
        let checkpoint_path = root.join("run.checkpoint");
        let first_run = create_run_root(&root).unwrap();
        fs::create_dir(first_run.join("partial-child")).unwrap();
        let options = checkpoint_options(&root, checkpoint_path.clone(), false);
        let (mut checkpoint, recovered) = Checkpoint::open(&options, &first_run).unwrap();
        assert!(recovered.is_empty());
        checkpoint.append(&combined()).unwrap();
        drop(checkpoint);
        OpenOptions::new()
            .append(true)
            .open(&checkpoint_path)
            .unwrap()
            .write_all(b"mako-m1-checkpoint-result-v1 torn")
            .unwrap();

        let second_run = create_run_root(&root).unwrap();
        let options = checkpoint_options(&root, checkpoint_path.clone(), true);
        let (checkpoint, recovered) = Checkpoint::open(&options, &second_run).unwrap();
        assert_eq!(recovered.len(), 1);
        assert_eq!(recovered[0].sample.checksum, combined().sample.checksum);
        assert!(!first_run.exists());
        assert!(second_run.exists());
        drop(checkpoint);
        let contents = fs::read_to_string(&checkpoint_path).unwrap();
        assert!(!contents.contains(" torn"));

        fs::remove_dir(&second_run).unwrap();
        fs::remove_file(&checkpoint_path).unwrap();
        fs::remove_dir(&root).unwrap();
    }

    #[test]
    fn checkpoint_resume_honors_keep_data() {
        let root = temporary_directory("keep");
        let checkpoint_path = root.join("run.checkpoint");
        let first_run = create_run_root(&root).unwrap();
        let options = checkpoint_options(&root, checkpoint_path.clone(), false);
        let (checkpoint, _) = Checkpoint::open(&options, &first_run).unwrap();
        drop(checkpoint);

        let second_run = create_run_root(&root).unwrap();
        let mut options = checkpoint_options(&root, checkpoint_path.clone(), true);
        options.keep_data = true;
        let (checkpoint, recovered) = Checkpoint::open(&options, &second_run).unwrap();
        assert!(recovered.is_empty());
        assert!(first_run.exists());
        drop(checkpoint);

        fs::remove_dir(&first_run).unwrap();
        fs::remove_dir(&second_run).unwrap();
        fs::remove_file(&checkpoint_path).unwrap();
        fs::remove_dir(&root).unwrap();
    }

    #[test]
    fn child_stream_capture_drains_and_keeps_the_diagnostic_tail() {
        let input: Vec<_> = (0..CHILD_CAPTURE_BYTES + 17)
            .map(|index| (index % 251) as u8)
            .collect();
        let capture = read_child_stream(Cursor::new(&input), "test").unwrap();
        assert_eq!(capture.total_bytes, input.len() as u64);
        assert_eq!(capture.bytes.len(), CHILD_CAPTURE_BYTES);
        assert_eq!(capture.bytes, input[input.len() - CHILD_CAPTURE_BYTES..]);
    }

    #[test]
    fn generated_root_validation_rejects_lookalikes() {
        assert!(is_generated_root_name("mako-m1-bench-123-456-7"));
        assert!(!is_generated_root_name("mako-m1-bench-important"));
        assert!(!is_generated_root_name("mako-m1-bench-123-456-7-extra"));
        assert!(hex_decode("00feff").is_ok());
        assert!(hex_decode("0").is_err());
        assert!(hex_decode("FF").is_err());
    }
}
