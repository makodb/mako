use mrx_rocks::{Durability, OptimisticCommitError, OptimisticDatabase, OptimisticTransaction};
use parking_lot::Mutex;
use rocksdb_raft_service::{
    client::{Client, Transaction as RemoteTransaction},
    config::{ClientConfig, DurabilityMode},
    wire::{Error, ErrorCode, Operation, Request, RequestId, Response},
};
use serde_json::json;
use std::{
    fs,
    path::PathBuf,
    sync::{
        atomic::{AtomicU64, Ordering},
        mpsc, Arc, Barrier,
    },
    thread,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};
use sto_rocksdb::{Database, Position};

type Result<T, E = Box<dyn std::error::Error>> = std::result::Result<T, E>;
type TxResult<T> = std::result::Result<T, Error>;
#[derive(Clone)]
struct Options {
    mode: String,
    phase: String,
    workload: String,
    config: Option<PathBuf>,
    path: Option<PathBuf>,
    durability: DurabilityMode,
    ready_file: Option<PathBuf>,
    start_file: Option<PathBuf>,
    threads: usize,
    seconds: u64,
    client: u64,
    group: u64,
    table: String,
    prefix: Vec<u8>,
    operations: usize,
    value_bytes: usize,
    hot_keys: usize,
    dataset_keys: usize,
    worker_offset: usize,
    total_workers: usize,
    scan_limit: usize,
}
impl Options {
    fn parse() -> Result<Self> {
        let mut o = Self {
            mode: "replicated".into(),
            phase: "measure".into(),
            workload: "disjoint".into(),
            durability: DurabilityMode::Sync,
            config: None,
            path: None,
            ready_file: None,
            start_file: None,
            threads: 8,
            seconds: 30,
            client: (SystemTime::now().duration_since(UNIX_EPOCH)?.as_nanos() as u64)
                ^ u64::from(std::process::id()),
            group: 1,
            table: "bench".into(),
            prefix: b"a/".to_vec(),
            operations: 4,
            value_bytes: 100,
            hot_keys: 8,
            dataset_keys: 4096,
            worker_offset: 0,
            total_workers: 0,
            scan_limit: 16,
        };
        let mut args = std::env::args().skip(1);
        while let Some(arg) = args.next() {
            if arg == "--help" {
                println!("rocksdb_raft_bench --mode native|sto|replicated --durability sync|async --phase preload|measure --workload point|disjoint|hot|multikey|scan [--config CONFIG | --path DB] --threads N --seconds N [--client-id U64] [--group N] [--table NAME] [--prefix TEXT] [--operations N] [--value-bytes N] [--hot-keys N] [--dataset-keys N] [--worker-offset N] [--total-workers N] [--scan-limit N] [--ready-file PATH --start-file PATH]\nDurability defaults to sync. Async retains WAL but loses power/machine-crash durability; it is not durable quorum. Preload is explicit and untimed. Native is actual RocksDB OptimisticTransactionDB, with tracked point reads but no phantom certification. STO is the unreplicated component reference. Use performance.py for matched process layouts and resource metrics.");
                std::process::exit(0);
            }
            let value = args.next().ok_or_else(|| format!("{arg} needs a value"))?;
            match arg.as_str() {
                "--mode" => o.mode = value,
                "--phase" => o.phase = value,
                "--workload" => o.workload = value,
                "--durability" => {
                    o.durability = match value.as_str() {
                        "sync" => DurabilityMode::Sync,
                        "async" => DurabilityMode::Async,
                        _ => return Err("--durability must be sync or async".into()),
                    }
                }
                "--config" => o.config = Some(value.into()),
                "--path" => o.path = Some(value.into()),
                "--ready-file" => o.ready_file = Some(value.into()),
                "--start-file" => o.start_file = Some(value.into()),
                "--threads" => o.threads = value.parse()?,
                "--seconds" => o.seconds = value.parse()?,
                "--client-id" => o.client = value.parse()?,
                "--group" => o.group = value.parse()?,
                "--table" => o.table = value,
                "--prefix" => o.prefix = value.into_bytes(),
                "--operations" => o.operations = value.parse()?,
                "--value-bytes" => o.value_bytes = value.parse()?,
                "--hot-keys" => o.hot_keys = value.parse()?,
                "--dataset-keys" => o.dataset_keys = value.parse()?,
                "--worker-offset" => o.worker_offset = value.parse()?,
                "--total-workers" => o.total_workers = value.parse()?,
                "--scan-limit" => o.scan_limit = value.parse()?,
                _ => return Err(format!("unknown option {arg}").into()),
            }
        }
        if o.total_workers == 0 {
            o.total_workers = o.threads;
        }
        if !matches!(o.mode.as_str(), "native" | "sto" | "replicated")
            || !matches!(o.phase.as_str(), "preload" | "measure")
            || !matches!(
                o.workload.as_str(),
                "point" | "disjoint" | "hot" | "multikey" | "scan"
            )
            || o.threads == 0
            || o.threads > 1024
            || o.seconds == 0
            || o.seconds > 86400
            || o.operations == 0
            || o.operations > 128
            || !(8..=262144).contains(&o.value_bytes)
            || o.dataset_keys == 0
            || o.dataset_keys > 1_000_000
            || o.total_workers > o.dataset_keys
            || o.worker_offset
                .checked_add(o.threads)
                .is_none_or(|n| n > o.total_workers)
            || o.hot_keys == 0
            || o.hot_keys > o.dataset_keys
            || o.scan_limit == 0
            || o.scan_limit > 128
            || o.scan_limit > o.dataset_keys
            || o.operations > o.dataset_keys / o.total_workers
            || o.operations.saturating_mul(o.value_bytes) > 8 * 1024 * 1024
            || o.scan_limit
                .saturating_mul(o.value_bytes + o.prefix.len() + 8)
                > 8 * 1024 * 1024
            || o.prefix.len() + 8 > 128
            || o.ready_file.is_some() != o.start_file.is_some()
        {
            return Err("invalid bounded benchmark options".into());
        }
        Ok(o)
    }
    fn writes(&self) -> bool {
        !matches!(self.workload.as_str(), "point" | "scan")
    }
    fn width(&self) -> usize {
        if self.workload == "multikey" {
            self.operations
        } else {
            1
        }
    }
}

// Fixed memory, 64 subdivisions per power of two: <=1.5625% relative upper-bound
// error above 64ns, <=1ns below. Percentiles describe commits.
struct Histogram {
    bins: Vec<u64>,
    count: u64,
    total_ns: u128,
    max_ns: u64,
}
impl Default for Histogram {
    fn default() -> Self {
        Self {
            bins: vec![0; 64 * 64],
            count: 0,
            total_ns: 0,
            max_ns: 0,
        }
    }
}
impl Histogram {
    fn record(&mut self, duration: Duration) {
        let n = duration.as_nanos().min(u64::MAX as u128) as u64;
        let exponent = (63 - n.max(1).leading_zeros()) as usize;
        let width = 1u64 << exponent.saturating_sub(6);
        let sub = n.saturating_sub(1u64 << exponent) / width;
        self.bins[exponent * 64 + sub as usize] += 1;
        self.count += 1;
        self.total_ns += n as u128;
        self.max_ns = self.max_ns.max(n);
    }
    fn percentile(&self, p: u64) -> f64 {
        if self.count == 0 {
            return 0.0;
        }
        let target = self.count.saturating_mul(p).div_ceil(100);
        let mut count = 0;
        for (index, value) in self.bins.iter().enumerate() {
            count += value;
            if count >= target {
                let exponent = index / 64;
                let width = 1u128 << exponent.saturating_sub(6);
                return (((1u128 << exponent) + ((index % 64 + 1) as u128) * width - 1)
                    .min(self.max_ns as u128)) as f64
                    / 1000.0;
            }
        }
        self.max_ns as f64 / 1000.0
    }
    fn merge(&mut self, other: Self) {
        self.count += other.count;
        self.total_ns += other.total_ns;
        self.max_ns = self.max_ns.max(other.max_ns);
        for (a, b) in self.bins.iter_mut().zip(other.bins) {
            *a += b;
        }
    }
    fn json(&self) -> serde_json::Value {
        let bins: Vec<_> = self
            .bins
            .iter()
            .enumerate()
            .filter(|(_, count)| **count != 0)
            .map(|(index, count)| (index, *count))
            .collect();
        json!({"samples": self.count, "p50": self.percentile(50), "p95": self.percentile(95), "p99": self.percentile(99),
            "mean": if self.count == 0 { 0.0 } else { self.total_ns as f64 / self.count as f64 / 1000.0 }, "max": self.max_ns as f64 / 1000.0,
            "sparse_bins": bins})
    }
}
#[derive(Default)]
struct Timing {
    begin: Duration,
    operations: Duration,
    commit: Duration,
}
#[derive(Default)]
struct Stats {
    committed: u64,
    conflicts: u64,
    busy: u64,
    unknown: u64,
    errors: u64,
    total: Histogram,
    attempts: Histogram,
    begin: Histogram,
    operations: Histogram,
    commit: Histogram,
}
impl Stats {
    fn record(&mut self, elapsed: Duration, timing: Timing, result: TxResult<()>) {
        self.attempts.record(elapsed);
        match result {
            Ok(()) => {
                self.committed += 1;
                self.total.record(elapsed);
                self.begin.record(timing.begin);
                self.operations.record(timing.operations);
                self.commit.record(timing.commit);
            }
            Err(error) => match error.code {
                ErrorCode::Conflict => self.conflicts += 1,
                ErrorCode::Busy => self.busy += 1,
                ErrorCode::UnknownOutcome => self.unknown += 1,
                _ => {
                    self.errors += 1;
                    if self.errors == 1 {
                        eprintln!("benchmark first error: {error}");
                    }
                }
            },
        }
    }
    fn merge(&mut self, other: Self) {
        self.committed += other.committed;
        self.conflicts += other.conflicts;
        self.busy += other.busy;
        self.unknown += other.unknown;
        self.errors += other.errors;
        self.total.merge(other.total);
        self.attempts.merge(other.attempts);
        self.begin.merge(other.begin);
        self.operations.merge(other.operations);
        self.commit.merge(other.commit);
    }
}
fn storage_error(e: impl std::fmt::Display) -> Error {
    Error::new(ErrorCode::StorageFailure, e.to_string())
}
fn sto_error(e: sto_rocksdb::Error) -> Error {
    Error::new(
        if matches!(
            e,
            sto_rocksdb::Error::Conflict | sto_rocksdb::Error::AlreadyExists
        ) {
            ErrorCode::Conflict
        } else {
            ErrorCode::StorageFailure
        },
        e.to_string(),
    )
}
fn invalid(message: &str) -> Error {
    Error::new(ErrorCode::Invalid, message)
}
trait Operations {
    fn get(&mut self, key: &[u8]) -> TxResult<Option<Vec<u8>>>;
    fn put(&mut self, key: &[u8], value: &[u8]) -> TxResult<()>;
    fn scan(
        &mut self,
        lower: &[u8],
        upper: &[u8],
        limit: usize,
    ) -> TxResult<Vec<(Vec<u8>, Vec<u8>)>>;
}
impl Operations for OptimisticTransaction<'_> {
    fn get(&mut self, key: &[u8]) -> TxResult<Option<Vec<u8>>> {
        OptimisticTransaction::get(self, key).map_err(storage_error)
    }
    fn put(&mut self, key: &[u8], value: &[u8]) -> TxResult<()> {
        OptimisticTransaction::put(self, key, value).map_err(storage_error)
    }
    fn scan(
        &mut self,
        lower: &[u8],
        upper: &[u8],
        limit: usize,
    ) -> TxResult<Vec<(Vec<u8>, Vec<u8>)>> {
        OptimisticTransaction::scan(self, lower, upper, limit).map_err(storage_error)
    }
}
struct LocalOps<'a, 'b> {
    tx: &'a mut sto_rocksdb::Transaction<'b>,
    table: u64,
}
impl Operations for LocalOps<'_, '_> {
    fn get(&mut self, key: &[u8]) -> TxResult<Option<Vec<u8>>> {
        self.tx.get(self.table, key).map_err(sto_error)
    }
    fn put(&mut self, key: &[u8], value: &[u8]) -> TxResult<()> {
        self.tx.put(self.table, key, value).map_err(sto_error)
    }
    fn scan(
        &mut self,
        lower: &[u8],
        upper: &[u8],
        limit: usize,
    ) -> TxResult<Vec<(Vec<u8>, Vec<u8>)>> {
        self.tx
            .scan(self.table, lower, Some(upper), limit)
            .map_err(sto_error)
    }
}
struct RemoteOps<'a> {
    client: &'a mut Client,
    tx: &'a RemoteTransaction,
    table: &'a str,
}
impl Operations for RemoteOps<'_> {
    fn get(&mut self, key: &[u8]) -> TxResult<Option<Vec<u8>>> {
        self.client.get(self.tx, self.table, key)
    }
    fn put(&mut self, key: &[u8], value: &[u8]) -> TxResult<()> {
        self.client.put(self.tx, self.table, key, value)
    }
    fn scan(
        &mut self,
        lower: &[u8],
        upper: &[u8],
        limit: usize,
    ) -> TxResult<Vec<(Vec<u8>, Vec<u8>)>> {
        match self.client.execute(
            self.tx,
            self.tx.handle.group,
            self.table,
            Operation::Scan {
                start: lower.to_vec(),
                end: Some(upper.to_vec()),
                limit,
            },
        )? {
            Response::Rows { rows } => Ok(rows),
            _ => Err(invalid("invalid Scan response")),
        }
    }
}
fn verify_value(value: &[u8], options: &Options) -> TxResult<u64> {
    if value.len() != options.value_bytes || value[8..].iter().any(|v| *v != b'x') {
        return Err(invalid("dataset value shape mismatch"));
    }
    let counter = u64::from_be_bytes(value[..8].try_into().expect("checked value size"));
    if !options.writes() && counter != 0 {
        return Err(invalid("read-only dataset unexpectedly modified"));
    }
    Ok(counter)
}
struct Work<'a> {
    options: &'a Options,
    keys: &'a [Vec<u8>],
    lane: usize,
    sequence: u64,
    preload: Option<(usize, usize)>,
    expected: Option<&'a [u64]>,
}
impl Work<'_> {
    fn execute(&self, tx: &mut impl Operations, value: &mut [u8]) -> TxResult<()> {
        let o = self.options;
        if let Some((start, end)) = self.preload {
            value[..8].fill(0);
            for key in &self.keys[start..end] {
                tx.put(key, value)?;
            }
            return Ok(());
        }
        if o.workload == "scan" {
            let start =
                (self.sequence as usize * o.scan_limit) % (o.dataset_keys - o.scan_limit + 1);
            let rows = tx.scan(
                &self.keys[start],
                &self.keys[start + o.scan_limit],
                o.scan_limit,
            )?;
            if rows.len() != o.scan_limit {
                return Err(invalid("scan returned incorrect row count"));
            }
            for ((key, observed), expected) in
                rows.iter().zip(&self.keys[start..start + o.scan_limit])
            {
                if key != expected {
                    return Err(invalid("scan returned incorrect key/order"));
                }
                verify_value(observed, o)?;
            }
            return Ok(());
        }
        let slots = o.dataset_keys / o.total_workers;
        let start = if o.workload == "hot" {
            self.sequence as usize % o.hot_keys
        } else {
            (o.worker_offset + self.lane) * slots
                + (self.sequence as usize % (slots / o.width())) * o.width()
        };
        for (offset, key) in self.keys[start..start + o.width()].iter().enumerate() {
            let observed = tx
                .get(key)?
                .ok_or_else(|| invalid("preloaded key missing"))?;
            let counter = verify_value(&observed, o)?;
            if let Some(expected) = self.expected {
                let index = start - (o.worker_offset + self.lane) * slots + offset;
                if counter != expected[index] {
                    return Err(invalid("disjoint read-modify-write counter mismatch"));
                }
            }
            if o.writes() {
                value[..8].copy_from_slice(
                    &counter
                        .checked_add(1)
                        .ok_or_else(|| invalid("counter overflow"))?
                        .to_be_bytes(),
                );
                tx.put(key, value)?;
            }
        }
        Ok(())
    }
}
struct Apply {
    position: Position,
    bytes: Vec<u8>,
    reply: mpsc::SyncSender<TxResult<()>>,
}
struct Local {
    db: Arc<Database>,
    table: u64,
    certification: Mutex<()>,
    next: AtomicU64,
    send: mpsc::SyncSender<Apply>,
}
struct Replies {
    send: mpsc::SyncSender<TxResult<()>>,
    receive: mpsc::Receiver<TxResult<()>>,
}
impl Replies {
    fn new() -> Self {
        let (send, receive) = mpsc::sync_channel(1);
        Self { send, receive }
    }
}
fn local_iteration(
    local: &Local,
    worker: &mut sto_rocksdb::Worker,
    replies: &Replies,
    request: RequestId,
    work: &Work<'_>,
    value: &mut [u8],
    timing: &mut Timing,
) -> TxResult<()> {
    let begin = Instant::now();
    let mut tx = worker.begin().map_err(sto_error)?;
    timing.begin = begin.elapsed();
    let operations = Instant::now();
    let result = work.execute(
        &mut LocalOps {
            tx: &mut tx,
            table: local.table,
        },
        value,
    );
    timing.operations = operations.elapsed();
    result?;
    let commit = Instant::now();
    let result = (|| {
        let mut prepared = tx.prepare(request.into()).map_err(sto_error)?;
        {
            let _gate = local.certification.lock();
            prepared.certify().map_err(sto_error)?;
            if prepared.is_read_only() {
                prepared.complete_read_only().map_err(sto_error)?;
                return Ok(());
            }
            prepared.mark_submitted().map_err(sto_error)?;
            let bytes = prepared.take_bytes().map_err(sto_error)?;
            let position = Position {
                index: local.next.fetch_add(1, Ordering::Relaxed),
                term: 1,
            };
            if local
                .send
                .send(Apply {
                    position,
                    bytes,
                    reply: replies.send.clone(),
                })
                .is_err()
            {
                local.db.fence();
                return Err(storage_error("local apply queue stopped"));
            }
        }
        replies
            .receive
            .recv()
            .map_err(|e| Error::new(ErrorCode::UnknownOutcome, e.to_string()))??;
        prepared.mark_committed().map_err(sto_error)?;
        prepared.complete_applied().map_err(sto_error)?;
        Ok(())
    })();
    timing.commit = commit.elapsed();
    result
}
fn native_iteration(
    db: &OptimisticDatabase,
    work: &Work<'_>,
    value: &mut [u8],
    timing: &mut Timing,
) -> TxResult<()> {
    let begin = Instant::now();
    let mut tx = db.begin().map_err(storage_error)?;
    timing.begin = begin.elapsed();
    let operations = Instant::now();
    let result = work.execute(&mut tx, value);
    timing.operations = operations.elapsed();
    result?;
    let commit = Instant::now();
    let result = tx.commit().map_err(|error| match error {
        OptimisticCommitError::Conflict => {
            Error::new(ErrorCode::Conflict, "native optimistic conflict")
        }
        OptimisticCommitError::Native(error) => storage_error(error),
    });
    timing.commit = commit.elapsed();
    result
}
fn replicated_iteration(
    client: &mut Client,
    request: RequestId,
    work: &Work<'_>,
    value: &mut [u8],
    timing: &mut Timing,
) -> TxResult<()> {
    let begin = Instant::now();
    let tx = client.begin(work.options.group, request)?;
    timing.begin = begin.elapsed();
    let operations = Instant::now();
    let result = work.execute(
        &mut RemoteOps {
            client,
            tx: &tx,
            table: &work.options.table,
        },
        value,
    );
    timing.operations = operations.elapsed();
    if let Err(error) = result {
        let _ = client.rollback(tx);
        return Err(error);
    }
    let commit = Instant::now();
    let result = client.commit(tx).and_then(|response| match response {
        Response::Committed { .. } => Ok(()),
        _ => Err(invalid("invalid Commit response")),
    });
    timing.commit = commit.elapsed();
    result
}
fn process_io() -> serde_json::Value {
    let mut values = serde_json::Map::new();
    if let Ok(contents) = fs::read_to_string("/proc/self/io") {
        for line in contents.lines() {
            if let Some((key, value)) = line.split_once(':') {
                if let Ok(value) = value.trim().parse::<u64>() {
                    values.insert(key.into(), json!(value));
                }
            }
        }
    }
    serde_json::Value::Object(values)
}
fn run() -> Result<()> {
    let options = Arc::new(Options::parse()?);
    if let Some(warning) = options.durability.warning() {
        eprintln!("{warning}");
    }
    // The final sentinel is an exclusive scan bound, not part of the dataset.
    let keys: Arc<Vec<Vec<u8>>> = Arc::new(
        (0..=options.dataset_keys)
            .map(|index| {
                let mut key = options.prefix.clone();
                key.extend_from_slice(&(index as u64).to_be_bytes());
                key
            })
            .collect(),
    );
    let mut native = None;
    let mut local = None;
    let mut config = None;
    let mut apply_thread = None;
    match options.mode.as_str() {
        "native" => {
            native = Some(Arc::new(OptimisticDatabase::open(
                options.path.as_deref().ok_or("native requires --path")?,
                if options.durability.sync_writes() {
                    Durability::Sync
                } else {
                    Durability::Wal
                },
            )?))
        }
        "sto" => {
            let db = Database::open(
                options.path.as_deref().ok_or("sto requires --path")?,
                options.group,
                options.durability.sync_writes(),
            )?;
            let table = match db.lookup_table(&options.table)? {
                Some(table) => table.id,
                None if options.phase == "preload" => {
                    let position = Position {
                        index: db.applied()?.index + 1,
                        term: 1,
                    };
                    let command = sto_rocksdb::Command::create_table(
                        options.group,
                        sto_rocksdb::RequestId {
                            client: options.client,
                            sequence: u64::MAX,
                        },
                        options.table.clone(),
                    )?;
                    db.apply(position, &command.encode()?)?;
                    db.lookup_table(&options.table)?
                        .ok_or("catalog apply missing table")?
                        .id
                }
                None => return Err("table missing: run --phase preload first".into()),
            };
            let (send, receive) = mpsc::sync_channel::<Apply>(options.threads);
            let application = db.clone();
            apply_thread = Some(thread::spawn(move || {
                while let Ok(work) = receive.recv() {
                    let result = application
                        .apply(work.position, &work.bytes)
                        .map(|_| ())
                        .map_err(storage_error);
                    if result.is_err() {
                        application.fence();
                    }
                    let _ = work.reply.send(result);
                }
            }));
            local = Some(Arc::new(Local {
                table,
                certification: Mutex::new(()),
                next: AtomicU64::new(db.applied()?.index + 1),
                db,
                send,
            }));
        }
        "replicated" => {
            let c = Arc::new(ClientConfig::read(
                options
                    .config
                    .as_ref()
                    .ok_or("replicated requires --config")?,
            )?);
            let mut client = Client::new(c.clone());
            client.wait_for_leader(options.group, Duration::from_secs(30))?;
            match client.group_request(Request::Status {
                group: options.group,
            })? {
                Response::Status { durability, .. } if durability == options.durability => {}
                _ => return Err("benchmark durability does not match service status".into()),
            }
            if !matches!(
                client.group_request(Request::LookupTable {
                    group: options.group,
                    name: options.table.clone()
                })?,
                Response::Table { table: Some(_) }
            ) {
                if options.phase != "preload" {
                    return Err("table missing: run --phase preload first".into());
                }
                client.create_table(
                    options.group,
                    RequestId {
                        client: options.client,
                        sequence: u64::MAX,
                    },
                    &options.table,
                )?;
            }
            config = Some(c);
        }
        _ => unreachable!(),
    }
    if options.phase == "preload" {
        let mut value = vec![b'x'; options.value_bytes];
        let mut worker = local.as_ref().map(|l| l.db.attach_worker()).transpose()?;
        let mut client = config.clone().map(Client::new);
        let replies = Replies::new();
        // Bound both command size and the number of synchronous SRPC operations.
        let batch = (1024 * 1024 / (options.value_bytes + options.prefix.len() + 8)).clamp(1, 64);
        for start in (0..options.dataset_keys).step_by(batch) {
            let work = Work {
                options: &options,
                keys: &keys,
                lane: 0,
                sequence: 0,
                preload: Some((start, (start + batch).min(options.dataset_keys))),
                expected: None,
            };
            let request = RequestId {
                client: options.client,
                sequence: start as u64 + 1,
            };
            let mut timing = Timing::default();
            if let Some(db) = &native {
                native_iteration(db, &work, &mut value, &mut timing)?;
            } else if let Some(local) = &local {
                local_iteration(
                    local,
                    worker.as_mut().expect("local worker"),
                    &replies,
                    request,
                    &work,
                    &mut value,
                    &mut timing,
                )?;
            } else {
                replicated_iteration(
                    client.as_mut().expect("remote client"),
                    request,
                    &work,
                    &mut value,
                    &mut timing,
                )?;
            }
        }
        drop(worker);
        drop(local);
        if let Some(thread) = apply_thread {
            thread.join().map_err(|_| "local apply thread panicked")?;
        }
        println!(
            "{}",
            json!({"phase": "preload", "mode": options.mode, "durability": options.durability, "power_crash_durable": options.durability.sync_writes(), "durability_warning": options.durability.warning(), "group": options.group, "dataset_keys": options.dataset_keys, "value_bytes": options.value_bytes})
        );
        return Ok(());
    }
    let ready = Arc::new(Barrier::new(options.threads + 1));
    let start = Arc::new(Barrier::new(options.threads + 1));
    let mut threads = Vec::with_capacity(options.threads);
    for lane in 0..options.threads {
        let options = options.clone();
        let keys = keys.clone();
        let ready = ready.clone();
        let start = start.clone();
        let native = native.clone();
        let local = local.clone();
        let config = config.clone();
        threads.push(thread::spawn(move || {
            let mut worker = local.as_ref().map(|l| l.db.attach_worker());
            let mut client = config.map(Client::new);
            let replies = Replies::new();
            let mut value = vec![b'x'; options.value_bytes];
            let mut stats = Stats::default();
            let slots = options.dataset_keys / options.total_workers;
            let mut expected = matches!(options.workload.as_str(), "disjoint" | "multikey")
                .then(|| vec![0u64; slots]);
            ready.wait();
            start.wait();
            let end = Instant::now() + Duration::from_secs(options.seconds);
            let mut sequence = 0u64;
            while Instant::now() < end {
                sequence += 1;
                let work = Work {
                    options: &options,
                    keys: &keys,
                    lane,
                    sequence,
                    preload: None,
                    expected: expected.as_deref(),
                };
                let request = RequestId {
                    client: options.client,
                    sequence: ((lane as u64) << 48) | sequence,
                };
                let mut timing = Timing::default();
                let started = Instant::now();
                let result = if let Some(db) = &native {
                    native_iteration(db, &work, &mut value, &mut timing)
                } else if let Some(local) = &local {
                    match worker.as_mut().expect("local worker") {
                        Ok(worker) => local_iteration(
                            local,
                            worker,
                            &replies,
                            request,
                            &work,
                            &mut value,
                            &mut timing,
                        ),
                        Err(error) => Err(storage_error(error)),
                    }
                } else {
                    replicated_iteration(
                        client.as_mut().expect("remote client"),
                        request,
                        &work,
                        &mut value,
                        &mut timing,
                    )
                };
                if result.is_ok() {
                    if let Some(expected) = expected.as_mut() {
                        let first =
                            (sequence as usize % (slots / options.width())) * options.width();
                        for counter in &mut expected[first..first + options.width()] {
                            *counter += 1;
                        }
                    }
                }
                let fatal = result
                    .as_ref()
                    .is_err_and(|e| !matches!(e.code, ErrorCode::Conflict | ErrorCode::Busy));
                stats.record(started.elapsed(), timing, result);
                if fatal {
                    break;
                }
            }
            stats
        }));
    }
    ready.wait();
    if let (Some(ready_file), Some(start_file)) = (&options.ready_file, &options.start_file) {
        fs::write(ready_file, std::process::id().to_string())?;
        let deadline = Instant::now() + Duration::from_secs(120);
        while !start_file.exists() {
            if Instant::now() >= deadline {
                return Err("timed out waiting for benchmark start file".into());
            }
            thread::sleep(Duration::from_millis(2));
        }
    }
    let mut stats = Stats::default();
    let io_before = process_io();
    let stat_before = fs::read_to_string("/proc/self/stat").ok();
    let started = Instant::now();
    start.wait();
    for worker in threads {
        stats.merge(worker.join().map_err(|_| "benchmark worker panicked")?);
    }
    let seconds = started.elapsed().as_secs_f64();
    let io_after = process_io();
    let stat_after = fs::read_to_string("/proc/self/stat").ok();
    drop(local);
    if let Some(thread) = apply_thread {
        thread.join().map_err(|_| "local apply thread panicked")?;
    }
    let attempted = stats.committed + stats.conflicts + stats.busy + stats.unknown + stats.errors;
    let fraction = |count| {
        if attempted == 0 {
            0.0
        } else {
            count as f64 / attempted as f64
        }
    };
    let logical_write_bytes = if options.writes() {
        stats.committed
            * options.width() as u64
            * (options.prefix.len() + 8 + options.value_bytes) as u64
    } else {
        0
    };
    println!(
        "{}",
        json!({"mode": options.mode, "phase": "measure", "workload": options.workload,
        "durability": options.durability, "power_crash_durable": options.durability.sync_writes(), "durability_warning": options.durability.warning(),
        "client_id": options.client, "pid": std::process::id(), "group": options.group, "table": options.table,
        "threads": options.threads, "worker_offset": options.worker_offset, "total_workers_per_shard": options.total_workers,
        "seconds": seconds, "requested_seconds": options.seconds, "dataset_keys_per_shard": options.dataset_keys,
        "operations_per_transaction": options.width(), "scan_limit": options.scan_limit,
        "value_bytes": options.value_bytes, "hot_keys": options.hot_keys, "logical_write_bytes": logical_write_bytes,
        "attempted": attempted, "committed": stats.committed, "conflicts": stats.conflicts, "busy": stats.busy,
        "conflicts_or_busy": stats.conflicts + stats.busy, "unknown": stats.unknown, "errors": stats.errors,
        "rates_per_second": {"committed": stats.committed as f64 / seconds, "conflicts": stats.conflicts as f64 / seconds,
            "busy": stats.busy as f64 / seconds, "unknown": stats.unknown as f64 / seconds, "errors": stats.errors as f64 / seconds},
        "fractions_of_attempts": {"committed": fraction(stats.committed), "conflicts": fraction(stats.conflicts), "busy": fraction(stats.busy), "unknown": fraction(stats.unknown), "errors": fraction(stats.errors)},
        "transactions_per_second": stats.committed as f64 / seconds,
        "latency_us": {"committed_total": stats.total.json(), "all_attempts_total": stats.attempts.json(),
            "committed_begin": stats.begin.json(), "committed_operations": stats.operations.json(), "committed_commit": stats.commit.json()},
        "histogram": "4096 fixed bins per histogram; <=1ns absolute upper-bound error below 64ns, <=1.5625% relative upper-bound error thereafter; percentile units microseconds; phases include committed attempts only",
        "phase_caveat": "service begin includes leader discovery, RPC and read barrier; commit includes certification, encoding, quorum and apply (or read-only certification). These are client wall intervals, not internal CPU attribution; local STO has no quorum barrier",
        "scan_caveat": "native scans track returned keys only and do not certify absent ranges/phantoms; STO scans certify coarse table generations; fixed preloaded read-only scan workload is not a phantom-equivalence claim",
        "process_io_before": io_before, "process_io_after": io_after, "process_stat_before": stat_before, "process_stat_after": stat_after})
    );
    if stats.errors > 0 || stats.unknown > 0 || stats.committed == 0 {
        return Err(
            "benchmark has unexpected errors, unknown outcomes, or zero commits (see metrics)"
                .into(),
        );
    }
    Ok(())
}
fn main() {
    if let Err(error) = run() {
        eprintln!("{error}");
        std::process::exit(1);
    }
}
