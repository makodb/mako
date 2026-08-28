#![deny(unsafe_code)]

#[cfg(not(mtree_native_integration))]
fn main() -> std::process::ExitCode {
    eprintln!(
        "sto_masstree_compare requires MAKO_MTREE_NATIVE_INTEGRATION=1 and the native link environment"
    );
    std::process::ExitCode::FAILURE
}

#[cfg(mtree_native_integration)]
fn main() -> std::process::ExitCode {
    match benchmark::run() {
        Ok(()) => std::process::ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("sto_masstree_compare: {error}");
            std::process::ExitCode::FAILURE
        }
    }
}

#[cfg(mtree_native_integration)]
mod benchmark {
    use std::{
        env,
        sync::{
            atomic::{AtomicU8, AtomicUsize, Ordering},
            Arc, Barrier,
        },
        thread,
        time::{Duration, Instant},
    };

    use masstree::{Runtime as MasstreeRuntime, RuntimeConfig as MasstreeRuntimeConfig, Worker};
    use sto_core::{
        AbortReason, AccessError, CommitOutcome, Runtime, RuntimeConfig, WorkerContext,
    };
    #[cfg(feature = "fixed-u64")]
    use sto_masstree::{FixedU64Batch, FixedU64Mutation, FixedU64Table};
    use sto_masstree::{
        PointMutation, PointReadBatch, PointSession, RegistryLayout, Table, TableConfig,
        TerminalReadVisitOutcome, Value,
    };

    const DEFAULT_THREADS: usize = 1;
    const DEFAULT_KEYSPACE: u64 = 100_000;
    const DEFAULT_OPS_PER_TXN: usize = 10;
    const DEFAULT_WRITE_PERCENT: u32 = 50;
    const DEFAULT_WARMUP_MS: u64 = 1_000;
    const DEFAULT_DURATION_MS: u64 = 3_000;
    const DEFAULT_SEED: u64 = 1;
    const PREPOPULATE_BATCH: u64 = 64;
    const MAX_OPS_PER_TXN: usize = 32_768;

    const PHASE_WAIT: u8 = 0;
    const PHASE_WARMUP: u8 = 1;
    const PHASE_QUIESCE: u8 = 2;
    const PHASE_MEASURE: u8 = 3;
    const PHASE_STOP: u8 = 4;
    const PHASE_FAILED: u8 = 5;

    // SplitMix64's Weyl-sequence increment.
    const SPLITMIX_GAMMA: u64 = 0x9e37_79b9_7f4a_7c15;

    fn splitmix_scramble(mut value: u64) -> u64 {
        value = (value ^ (value >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        value = (value ^ (value >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        value ^ (value >> 31)
    }

    fn worker_random_state(seed: u64, thread_id: usize) -> u64 {
        // A direct thread offset by SPLITMIX_GAMMA selects nearby positions in
        // one stream. Scramble the selector so workers do not reuse shifted
        // copies of almost every key/write pair.
        splitmix_scramble(seed.wrapping_add((thread_id as u64 + 1).wrapping_mul(SPLITMIX_GAMMA)))
    }

    #[derive(Clone, Copy, Debug)]
    enum ValueMode {
        Binary,
        #[cfg(feature = "fixed-u64")]
        FixedU64,
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
        value_mode: ValueMode,
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
                value_mode: ValueMode::Binary,
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
                    "--value-mode" => config.value_mode = parse_value_mode(&value)?,
                    _ => return Err(format!("unknown argument {flag}")),
                }
            }

            if config.threads == 0 {
                return Err("--threads must be positive".into());
            }
            if config.keyspace == 0 {
                return Err("--keyspace must be positive".into());
            }
            if config.ops_per_txn == 0 {
                return Err("--ops-per-txn must be positive".into());
            }
            if config.ops_per_txn > MAX_OPS_PER_TXN {
                return Err("--ops-per-txn must not exceed 32768".into());
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

    fn parse_value_mode(value: &str) -> Result<ValueMode, String> {
        match value {
            "binary" => Ok(ValueMode::Binary),
            #[cfg(feature = "fixed-u64")]
            "fixed-u64" => Ok(ValueMode::FixedU64),
            #[cfg(not(feature = "fixed-u64"))]
            "fixed-u64" => {
                Err("--value-mode fixed-u64 requires the sto-masstree fixed-u64 feature".into())
            }
            _ => Err(format!(
                "invalid value for --value-mode: {value} (expected binary or fixed-u64)"
            )),
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
        key_number: u64,
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

    enum AttemptBody {
        Ready(u64),
        Retry,
        Fatal(String),
    }

    trait BenchmarkTable: Clone + Send + Sync + 'static {
        type Batch;

        const ENGINE: &'static str;

        fn batch_with_capacity(capacity: usize) -> Self::Batch;

        fn prepopulate(
            &self,
            native_worker: &Worker,
            sto_worker: &mut WorkerContext,
            keyspace: u64,
        ) -> Result<(), String>;

        fn attempt(
            &self,
            native_worker: &Worker,
            sto_worker: &mut WorkerContext,
            operations: &[Operation],
            keys: &[[u8; 8]],
            batch: &mut Self::Batch,
            has_writes: bool,
        ) -> Result<AttemptOutcome, String>;
    }

    pub(super) fn run() -> Result<(), String> {
        let config = Config::parse()?;
        let native_thread_limit = u32::try_from(
            config
                .threads
                .checked_add(1)
                .ok_or("native thread limit overflow")?,
        )
        .map_err(|_| "native thread limit exceeds u32".to_owned())?;
        let retained_key_bytes = config
            .keyspace
            .checked_mul(8)
            .ok_or("keyspace byte limit overflow")?;
        let transaction_capacity = config
            .ops_per_txn
            .checked_add(1)
            .ok_or("transaction capacity overflow")?
            .max(PREPOPULATE_BATCH as usize + 1);

        let native_runtime = MasstreeRuntime::new(
            MasstreeRuntimeConfig::new().with_max_threads(native_thread_limit),
        )
        .map_err(|error| format!("create native Masstree runtime: {error:?}"))?;
        let native_loader = native_runtime
            .attach()
            .map_err(|error| format!("attach native loader: {error:?}"))?;

        let sto_runtime = Runtime::new(
            RuntimeConfig::new()
                .with_max_workers(
                    config
                        .threads
                        .checked_add(1)
                        .ok_or("STO worker limit overflow")?,
                )
                .with_max_items_per_transaction(transaction_capacity)
                .with_max_locks_per_transaction(transaction_capacity),
        )
        .map_err(|error| format!("create STO runtime: {error:?}"))?;
        let table_config = TableConfig::new()
            .with_max_retained_records(config.keyspace)
            .with_max_retained_key_bytes(retained_key_bytes)
            .with_max_consumed_record_ids(config.keyspace)
            // The benchmark preloads its complete bounded keyspace, so
            // paying the arena's startup/memory cost removes lazy segment
            // lookup from the timed RecordId resolution path.
            .with_registry_layout(RegistryLayout::EagerContiguous {
                max_bytes: 8 * 1024 * 1024,
            });
        match config.value_mode {
            ValueMode::Binary => {
                let tree = native_runtime
                    .create_tree(&native_loader)
                    .map_err(|error| format!("create native Masstree tree: {error:?}"))?;
                let table = Table::new(&sto_runtime, tree, table_config)
                    .map_err(|error| format!("create STO Masstree table: {error:?}"))?;
                prepare_and_run(config, native_runtime, sto_runtime, native_loader, table)
            }
            #[cfg(feature = "fixed-u64")]
            ValueMode::FixedU64 => {
                let table =
                    FixedU64Table::new(&sto_runtime, &native_runtime, &native_loader, table_config)
                        .map_err(|error| {
                            format!("create fixed-u64 STO Masstree table: {error:?}")
                        })?;
                prepare_and_run(config, native_runtime, sto_runtime, native_loader, table)
            }
        }
    }

    fn prepare_and_run<T: BenchmarkTable>(
        config: Config,
        native_runtime: MasstreeRuntime,
        sto_runtime: Arc<Runtime>,
        native_loader: Worker,
        table: T,
    ) -> Result<(), String> {
        let mut sto_loader = sto_runtime
            .attach()
            .map_err(|error| format!("attach STO loader: {error:?}"))?;
        table.prepopulate(&native_loader, &mut sto_loader, config.keyspace)?;
        native_loader
            .quiesce()
            .map_err(|error| format!("quiesce native loader: {error:?}"))?;
        drop(sto_loader);
        drop(native_loader);
        run_table(config, native_runtime, sto_runtime, table)
    }

    fn run_table<T: BenchmarkTable>(
        config: Config,
        native_runtime: MasstreeRuntime,
        sto_runtime: Arc<Runtime>,
        table: T,
    ) -> Result<(), String> {
        let phase = Arc::new(AtomicU8::new(PHASE_WAIT));
        let quiesced = Arc::new(AtomicUsize::new(0));
        let ready = Arc::new(Barrier::new(config.threads + 1));
        let mut handles = Vec::with_capacity(config.threads);
        for thread_id in 0..config.threads {
            let native_runtime = native_runtime.clone();
            let sto_runtime = Arc::clone(&sto_runtime);
            let table = table.clone();
            let phase = Arc::clone(&phase);
            let quiesced = Arc::clone(&quiesced);
            let ready = Arc::clone(&ready);
            handles.push(thread::spawn(move || {
                let result = run_worker(
                    thread_id,
                    config,
                    &native_runtime,
                    &sto_runtime,
                    &table,
                    &phase,
                    &quiesced,
                    &ready,
                );
                if result.is_err() {
                    phase.store(PHASE_FAILED, Ordering::Release);
                }
                result
            }));
        }

        ready.wait();
        if phase
            .compare_exchange(
                PHASE_WAIT,
                PHASE_WARMUP,
                Ordering::AcqRel,
                Ordering::Acquire,
            )
            .is_err()
        {
            return join_worker_errors(handles, "worker failed before warmup");
        }
        thread::sleep(Duration::from_millis(config.warmup_ms));

        if phase.load(Ordering::Acquire) != PHASE_WARMUP {
            return join_worker_errors(handles, "worker failed during warmup");
        }

        if phase
            .compare_exchange(
                PHASE_WARMUP,
                PHASE_QUIESCE,
                Ordering::AcqRel,
                Ordering::Acquire,
            )
            .is_err()
        {
            return join_worker_errors(handles, "worker failed before quiescence");
        }
        while quiesced.load(Ordering::Acquire) != config.threads {
            if phase.load(Ordering::Acquire) == PHASE_FAILED {
                return join_worker_errors(handles, "worker failed while quiescing");
            }
            thread::yield_now();
        }

        let measurement_start = Instant::now();
        if phase
            .compare_exchange(
                PHASE_QUIESCE,
                PHASE_MEASURE,
                Ordering::AcqRel,
                Ordering::Acquire,
            )
            .is_err()
        {
            return join_worker_errors(handles, "worker failed before measurement");
        }
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
                "\"engine\":\"{}\",",
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
            T::ENGINE,
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

    impl BenchmarkTable for Table {
        type Batch = PointReadBatch;

        const ENGINE: &'static str = "rust-sto-masstree";

        fn batch_with_capacity(capacity: usize) -> Self::Batch {
            PointReadBatch::with_capacity(capacity)
        }

        fn prepopulate(
            &self,
            native_worker: &Worker,
            sto_worker: &mut WorkerContext,
            keyspace: u64,
        ) -> Result<(), String> {
            prepopulate_binary(self, native_worker, sto_worker, keyspace)
        }

        fn attempt(
            &self,
            native_worker: &Worker,
            sto_worker: &mut WorkerContext,
            operations: &[Operation],
            keys: &[[u8; 8]],
            batch: &mut Self::Batch,
            has_writes: bool,
        ) -> Result<AttemptOutcome, String> {
            attempt_binary_transaction(
                self,
                native_worker,
                sto_worker,
                operations,
                keys,
                batch,
                has_writes,
            )
        }
    }

    #[cfg(feature = "fixed-u64")]
    impl BenchmarkTable for FixedU64Table {
        type Batch = FixedU64Batch;

        const ENGINE: &'static str = "rust-sto-masstree-fixed-u64";

        fn batch_with_capacity(capacity: usize) -> Self::Batch {
            FixedU64Batch::with_capacity(capacity)
        }

        fn prepopulate(
            &self,
            native_worker: &Worker,
            _sto_worker: &mut WorkerContext,
            keyspace: u64,
        ) -> Result<(), String> {
            for logical_key in 0..keyspace {
                self.insert_initial(native_worker, &logical_key.to_be_bytes(), logical_key)
                    .map_err(|error| format!("fixed-u64 preload access failed: {error:?}"))?;
            }
            self.finish_initial_load()
                .map_err(|error| format!("seal fixed-u64 initial load: {error:?}"))
        }

        fn attempt(
            &self,
            native_worker: &Worker,
            sto_worker: &mut WorkerContext,
            operations: &[Operation],
            keys: &[[u8; 8]],
            batch: &mut Self::Batch,
            has_writes: bool,
        ) -> Result<AttemptOutcome, String> {
            attempt_fixed_u64_transaction(
                self,
                native_worker,
                sto_worker,
                operations,
                keys,
                batch,
                has_writes,
            )
        }
    }

    fn prepopulate_binary(
        table: &Table,
        native_worker: &Worker,
        sto_worker: &mut WorkerContext,
        keyspace: u64,
    ) -> Result<(), String> {
        let mut first = 0_u64;
        while first < keyspace {
            let end = first.saturating_add(PREPOPULATE_BATCH).min(keyspace);
            'batch_attempt: loop {
                let mut transaction = sto_worker
                    .begin()
                    .map_err(|error| format!("begin preload transaction: {error:?}"))?;
                for logical_key in first..end {
                    let key = logical_key.to_be_bytes();
                    let value = logical_key.to_le_bytes();
                    match table.put(&mut transaction, native_worker, &key, &value) {
                        Ok(_) => {}
                        Err(AccessError::Conflict(_)) => {
                            transaction.abort();
                            continue 'batch_attempt;
                        }
                        Err(error) => {
                            transaction.abort();
                            return Err(format!("preload access failed: {error:?}"));
                        }
                    }
                }
                match transaction
                    .commit()
                    .map_err(|error| format!("preload commit failed: {error:?}"))?
                {
                    CommitOutcome::Committed(_) => break,
                    CommitOutcome::Aborted(AbortReason::Conflict(_)) => {}
                    CommitOutcome::Aborted(reason) => {
                        return Err(format!("preload transaction aborted: {reason:?}"));
                    }
                }
            }
            first = end;
        }
        Ok(())
    }

    #[cfg(feature = "fixed-u64")]
    fn attempt_fixed_u64_transaction(
        table: &FixedU64Table,
        native_worker: &Worker,
        sto_worker: &mut WorkerContext,
        operations: &[Operation],
        keys: &[[u8; 8]],
        batch: &mut FixedU64Batch,
        has_writes: bool,
    ) -> Result<AttemptOutcome, String> {
        if !has_writes {
            return attempt_fixed_u64_terminal_read(
                table,
                native_worker,
                sto_worker,
                operations,
                keys,
                batch,
            );
        }
        if keys.len() != operations.len() {
            return Err("timed fixed-u64 key batch has the wrong operation count".into());
        }

        let mut transaction = sto_worker
            .begin()
            .map_err(|error| format!("begin timed fixed-u64 transaction: {error:?}"))?;
        let mut checksum = 0_u64;
        let visited = match table.modify_fixed(
            &mut transaction,
            native_worker,
            keys,
            batch,
            |index, current| {
                checksum = checksum.wrapping_add(current);
                if operations[index].write {
                    FixedU64Mutation::Put(current.wrapping_add(1))
                } else {
                    FixedU64Mutation::Keep
                }
            },
        ) {
            Ok(Some(visited)) => visited,
            Ok(None) => {
                transaction.abort();
                return Err(
                    "prepopulated fixed-u64 key disappeared or batch was not unique".into(),
                );
            }
            Err(AccessError::Conflict(_)) => {
                transaction.abort();
                return Ok(AttemptOutcome::Retry);
            }
            Err(error) => {
                transaction.abort();
                return Err(format!("timed fixed-u64 batch failed: {error:?}"));
            }
        };
        if visited != operations.len() {
            transaction.abort();
            return Err("timed fixed-u64 batch visited the wrong item count".into());
        }

        match transaction
            .commit()
            .map_err(|error| format!("timed fixed-u64 commit failed: {error:?}"))?
        {
            CommitOutcome::Committed(_) => Ok(AttemptOutcome::Committed(checksum)),
            CommitOutcome::Aborted(AbortReason::Conflict(_)) => Ok(AttemptOutcome::Retry),
            CommitOutcome::Aborted(reason) => {
                Err(format!("timed fixed-u64 transaction aborted: {reason:?}"))
            }
        }
    }

    #[cfg(feature = "fixed-u64")]
    fn attempt_fixed_u64_terminal_read(
        table: &FixedU64Table,
        native_worker: &Worker,
        sto_worker: &mut WorkerContext,
        operations: &[Operation],
        keys: &[[u8; 8]],
        batch: &mut FixedU64Batch,
    ) -> Result<AttemptOutcome, String> {
        if keys.len() != operations.len() {
            return Err("timed fixed-u64 key batch has the wrong operation count".into());
        }

        let transaction = sto_worker
            .begin_terminal_read_batch()
            .map_err(|error| format!("begin timed fixed-u64 terminal read: {error:?}"))?;
        let mut checksum = 0_u64;
        let outcome = match table.visit_fixed_terminal(
            transaction,
            native_worker,
            keys,
            batch,
            |_index, value| checksum = checksum.wrapping_add(value),
        ) {
            Ok(outcome) => outcome,
            Err(AccessError::Conflict(_)) => return Ok(AttemptOutcome::Retry),
            Err(error) => {
                return Err(format!(
                    "timed fixed-u64 terminal batch visit failed: {error:?}"
                ));
            }
        };
        let (transaction, visited) = match outcome {
            TerminalReadVisitOutcome::Ready {
                transaction,
                visited,
            } => (transaction, visited),
            TerminalReadVisitOutcome::RetryOrdinary => {
                return Err("prepopulated fixed-u64 key disappeared before timed read".into());
            }
        };
        if visited != operations.len() {
            transaction.abort();
            return Err("timed fixed-u64 terminal batch visited the wrong item count".into());
        }

        match transaction
            .commit()
            .map_err(|error| format!("timed fixed-u64 terminal commit failed: {error:?}"))?
        {
            CommitOutcome::Committed(_) => Ok(AttemptOutcome::Committed(checksum)),
            CommitOutcome::Aborted(AbortReason::Conflict(_)) => Ok(AttemptOutcome::Retry),
            CommitOutcome::Aborted(reason) => Err(format!(
                "timed fixed-u64 terminal transaction aborted: {reason:?}"
            )),
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn run_worker<T: BenchmarkTable>(
        thread_id: usize,
        config: Config,
        native_runtime: &MasstreeRuntime,
        sto_runtime: &Arc<Runtime>,
        table: &T,
        phase: &AtomicU8,
        quiesced: &AtomicUsize,
        ready: &Barrier,
    ) -> Result<WorkerStats, String> {
        let native_worker = native_runtime
            .attach()
            .map_err(|error| format!("attach native worker {thread_id}: {error:?}"));
        let sto_worker = sto_runtime
            .attach()
            .map_err(|error| format!("attach STO worker {thread_id}: {error:?}"));
        // Every spawned thread reaches the barrier even if attachment failed,
        // so a configuration error cannot strand the coordinator forever.
        ready.wait();
        let native_worker = native_worker?;
        let mut sto_worker = sto_worker?;

        let initial_state = worker_random_state(config.seed, thread_id);
        let mut random = SplitMix64::new(initial_state);
        let mut previous_phase = PHASE_WAIT;
        let mut reported_quiescence = false;
        let mut operations = Vec::with_capacity(config.ops_per_txn);
        let mut keys = Vec::with_capacity(config.ops_per_txn);
        let mut batch = T::batch_with_capacity(config.ops_per_txn);
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
                    // Warmup work must not advance the deterministic measured
                    // stream differently in faster and slower engines.
                    random = SplitMix64::new(initial_state);
                }
                PHASE_WARMUP | PHASE_MEASURE => {}
                _ => return Err(format!("worker {thread_id} observed invalid phase")),
            }
            previous_phase = current_phase;
            reported_quiescence = false;

            operations.clear();
            keys.clear();
            let mut has_writes = false;
            for _ in 0..config.ops_per_txn {
                let mut key = random.next_u64() % config.keyspace;
                while operations
                    .iter()
                    .any(|operation: &Operation| operation.key_number == key)
                {
                    key = key.wrapping_add(1) % config.keyspace;
                }
                let write = random.next_u64() % 100 < u64::from(config.write_percent);
                has_writes |= write;
                operations.push(Operation {
                    key_number: key,
                    write,
                });
                keys.push(key.to_be_bytes());
            }

            let mut logical_attempts = 0_u64;
            let mut logical_aborts = 0_u64;
            let committed_checksum = loop {
                if matches!(phase.load(Ordering::Acquire), PHASE_STOP | PHASE_FAILED) {
                    break None;
                }
                logical_attempts = logical_attempts.wrapping_add(1);
                match table.attempt(
                    &native_worker,
                    &mut sto_worker,
                    &operations,
                    &keys,
                    &mut batch,
                    has_writes,
                )? {
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

        native_worker
            .quiesce()
            .map_err(|error| format!("quiesce native worker {thread_id}: {error:?}"))?;
        Ok(stats)
    }

    fn attempt_binary_transaction(
        table: &Table,
        native_worker: &Worker,
        sto_worker: &mut WorkerContext,
        operations: &[Operation],
        keys: &[[u8; 8]],
        read_batch: &mut PointReadBatch,
        has_writes: bool,
    ) -> Result<AttemptOutcome, String> {
        if !has_writes {
            return attempt_terminal_read(
                table,
                native_worker,
                sto_worker,
                operations,
                keys,
                read_batch,
            );
        }

        let mut transaction = sto_worker
            .begin()
            .map_err(|error| format!("begin timed transaction: {error:?}"))?;
        let body = {
            let mut session = table.point_session(&mut transaction, native_worker);
            let body = execute_mutations(&mut session, operations, keys, read_batch);
            match session.close() {
                Ok(()) => body,
                Err(AccessError::Conflict(_)) if !matches!(&body, AttemptBody::Fatal(_)) => {
                    AttemptBody::Retry
                }
                Err(error) => {
                    AttemptBody::Fatal(format!("close timed point session failed: {error:?}"))
                }
            }
        };

        let checksum = match body {
            AttemptBody::Ready(checksum) => checksum,
            AttemptBody::Retry => {
                transaction.abort();
                return Ok(AttemptOutcome::Retry);
            }
            AttemptBody::Fatal(error) => {
                transaction.abort();
                return Err(error);
            }
        };

        match transaction
            .commit()
            .map_err(|error| format!("timed commit failed: {error:?}"))?
        {
            CommitOutcome::Committed(_) => Ok(AttemptOutcome::Committed(checksum)),
            CommitOutcome::Aborted(AbortReason::Conflict(_)) => Ok(AttemptOutcome::Retry),
            CommitOutcome::Aborted(reason) => Err(format!("timed transaction aborted: {reason:?}")),
        }
    }

    fn attempt_terminal_read(
        table: &Table,
        native_worker: &Worker,
        sto_worker: &mut WorkerContext,
        operations: &[Operation],
        keys: &[[u8; 8]],
        read_batch: &mut PointReadBatch,
    ) -> Result<AttemptOutcome, String> {
        if keys.len() != operations.len() {
            return Err("timed key batch has the wrong operation count".into());
        }

        let transaction = sto_worker
            .begin_terminal_read_batch()
            .map_err(|error| format!("begin timed terminal read: {error:?}"))?;
        let mut checksum = 0_u64;
        let mut fatal = None;
        let outcome = match table.visit_fixed_terminal(
            transaction,
            native_worker,
            keys,
            read_batch,
            |index, snapshot| {
                if fatal.is_some() {
                    return;
                }
                let Some(value) = snapshot else {
                    fatal = Some(format!(
                        "prepopulated key {} disappeared during timed run",
                        operations[index].key_number
                    ));
                    return;
                };
                let value_bytes: [u8; 8] = match value.as_ref().try_into() {
                    Ok(bytes) => bytes,
                    Err(_) => {
                        fatal = Some("timed read returned a non-u64 value".into());
                        return;
                    }
                };
                checksum = checksum.wrapping_add(u64::from_le_bytes(value_bytes));
            },
        ) {
            Ok(outcome) => outcome,
            Err(AccessError::Conflict(_)) => return Ok(AttemptOutcome::Retry),
            Err(error) => {
                return Err(format!("timed terminal batch visit failed: {error:?}"));
            }
        };

        let (transaction, visited) = match outcome {
            TerminalReadVisitOutcome::Ready {
                transaction,
                visited,
            } => (transaction, visited),
            TerminalReadVisitOutcome::RetryOrdinary => {
                return Err("prepopulated key disappeared before timed terminal read".into());
            }
        };
        if let Some(error) = fatal {
            transaction.abort();
            return Err(error);
        }
        if visited != operations.len() {
            transaction.abort();
            return Err("timed terminal batch visited the wrong item count".into());
        }

        match transaction
            .commit()
            .map_err(|error| format!("timed terminal commit failed: {error:?}"))?
        {
            CommitOutcome::Committed(_) => Ok(AttemptOutcome::Committed(checksum)),
            CommitOutcome::Aborted(AbortReason::Conflict(_)) => Ok(AttemptOutcome::Retry),
            CommitOutcome::Aborted(reason) => {
                Err(format!("timed terminal transaction aborted: {reason:?}"))
            }
        }
    }

    fn execute_mutations(
        session: &mut PointSession<'_, '_>,
        operations: &[Operation],
        keys: &[[u8; 8]],
        read_batch: &mut PointReadBatch,
    ) -> AttemptBody {
        if keys.len() != operations.len() {
            return AttemptBody::Fatal("timed key batch has the wrong operation count".into());
        }

        let mut checksum = 0_u64;
        let mut fatal = None;
        let visited = match session.modify_fixed_visit(keys, read_batch, |index, snapshot| {
            if fatal.is_some() {
                return PointMutation::Keep;
            }
            let operation = &operations[index];
            let Some(value) = snapshot else {
                fatal = Some(format!(
                    "prepopulated key {} disappeared during timed run",
                    operation.key_number
                ));
                return PointMutation::Keep;
            };
            let value_bytes: [u8; 8] = match value.as_ref().try_into() {
                Ok(bytes) => bytes,
                Err(_) => {
                    fatal = Some("timed read returned a non-u64 value".into());
                    return PointMutation::Keep;
                }
            };
            let current = u64::from_le_bytes(value_bytes);
            // Fold every successful logical observation, including the read
            // that precedes a write. Attempt-local sums are published only
            // after commit, so retries never double count.
            checksum = checksum.wrapping_add(current);

            if operation.write {
                let replacement = current.wrapping_add(1).to_le_bytes();
                PointMutation::Put(Value::from(replacement.as_slice()))
            } else {
                PointMutation::Keep
            }
        }) {
            Ok(visited) => visited,
            Err(AccessError::Conflict(_)) => return AttemptBody::Retry,
            Err(error) => {
                return AttemptBody::Fatal(format!("timed fused batch failed: {error:?}"));
            }
        };
        if let Some(error) = fatal {
            return AttemptBody::Fatal(error);
        }
        if visited != operations.len() {
            return AttemptBody::Fatal("timed fused batch visited the wrong item count".into());
        }
        AttemptBody::Ready(checksum)
    }
}
