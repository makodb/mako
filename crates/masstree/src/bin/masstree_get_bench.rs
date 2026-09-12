#![deny(unsafe_code)]

#[cfg(not(mtree_native_integration))]
fn main() -> std::process::ExitCode {
    eprintln!(
        "masstree_get_bench requires MAKO_MTREE_NATIVE_INTEGRATION=1 and the native link environment"
    );
    std::process::ExitCode::FAILURE
}

#[cfg(mtree_native_integration)]
fn main() -> std::process::ExitCode {
    match benchmark::run() {
        Ok(()) => std::process::ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("masstree_get_bench: {error}");
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

    use masstree::{InsertOutcome, RecordId, Runtime, RuntimeConfig, Tree};

    const DEFAULT_THREADS: usize = 1;
    const DEFAULT_KEYSPACE: u64 = 100_000;
    const DEFAULT_BATCH_SIZE: usize = 256;
    const DEFAULT_READS_PER_SCOPE: usize = 0;
    const DEFAULT_WARMUP_MS: u64 = 1_000;
    const DEFAULT_DURATION_MS: u64 = 3_000;
    const DEFAULT_SEED: u64 = 1;
    const MAX_BATCH_SIZE: usize = 1 << 20;

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
        batch_size: usize,
        reads_per_scope: usize,
        warmup_ms: u64,
        duration_ms: u64,
        seed: u64,
    }

    impl Default for Config {
        fn default() -> Self {
            Self {
                threads: DEFAULT_THREADS,
                keyspace: DEFAULT_KEYSPACE,
                batch_size: DEFAULT_BATCH_SIZE,
                reads_per_scope: DEFAULT_READS_PER_SCOPE,
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
                    "--batch-size" => config.batch_size = parse_value(&flag, &value)?,
                    "--reads-per-scope" => config.reads_per_scope = parse_value(&flag, &value)?,
                    "--warmup-ms" => config.warmup_ms = parse_value(&flag, &value)?,
                    "--duration-ms" => config.duration_ms = parse_value(&flag, &value)?,
                    "--seed" => config.seed = parse_value(&flag, &value)?,
                    _ => return Err(format!("unknown argument {flag}")),
                }
            }

            if config.threads == 0 {
                return Err("--threads must be positive".into());
            }
            if config.keyspace == 0 || config.keyspace == u64::MAX {
                return Err("--keyspace must be in 1..u64::MAX".into());
            }
            if config.batch_size == 0 || config.batch_size > MAX_BATCH_SIZE {
                return Err("--batch-size must be in 1..=1048576".into());
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
        lookups: u64,
        checksum: u64,
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
        let runtime = Runtime::new(RuntimeConfig::new().with_max_threads(native_thread_limit))
            .map_err(|error| format!("create native Masstree runtime: {error:?}"))?;
        let loader = runtime
            .attach()
            .map_err(|error| format!("attach native loader: {error:?}"))?;
        let tree = runtime
            .create_tree(&loader)
            .map_err(|error| format!("create native Masstree tree: {error:?}"))?;
        prepopulate(&tree, &loader, config.keyspace)?;
        loader
            .quiesce()
            .map_err(|error| format!("quiesce native loader: {error:?}"))?;
        drop(loader);

        let phase = Arc::new(AtomicU8::new(PHASE_WAIT));
        let quiesced = Arc::new(AtomicUsize::new(0));
        let ready = Arc::new(Barrier::new(config.threads + 1));
        let mut handles = Vec::with_capacity(config.threads);
        for thread_id in 0..config.threads {
            let runtime = runtime.clone();
            let tree = tree.clone();
            let phase = Arc::clone(&phase);
            let quiesced = Arc::clone(&quiesced);
            let ready = Arc::clone(&ready);
            handles.push(thread::spawn(move || {
                let result = run_worker(
                    thread_id, config, &runtime, &tree, &phase, &quiesced, &ready,
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
            aggregate.lookups = aggregate.lookups.wrapping_add(stats.lookups);
            aggregate.checksum = aggregate.checksum.wrapping_add(stats.checksum);
        }

        let elapsed_ns = u64::try_from(elapsed.as_nanos()).unwrap_or(u64::MAX);
        let ops_per_sec = aggregate.lookups as f64 / elapsed.as_secs_f64();
        println!(
            concat!(
                "BENCH_RESULT={{",
                "\"engine\":\"rust-masstree-safe-get\",",
                "\"threads\":{},",
                "\"keyspace\":{},",
                "\"batch_size\":{},",
                "\"reads_per_scope\":{},",
                "\"warmup_ms\":{},",
                "\"duration_ms\":{},",
                "\"seed\":{},",
                "\"lookups\":{},",
                "\"elapsed_ns\":{},",
                "\"ops_per_sec\":{:.6},",
                "\"checksum\":{}",
                "}}"
            ),
            config.threads,
            config.keyspace,
            config.batch_size,
            config.reads_per_scope,
            config.warmup_ms,
            config.duration_ms,
            config.seed,
            aggregate.lookups,
            elapsed_ns,
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

    fn prepopulate(tree: &Tree, worker: &masstree::Worker, keyspace: u64) -> Result<(), String> {
        for key_number in 0..keyspace {
            let key = key_number.to_be_bytes();
            let candidate = RecordId::new(key_number + 1).expect("bounded key produces nonzero ID");
            match tree.get_or_insert(worker, &key, candidate) {
                Ok(InsertOutcome::Inserted(winner)) if winner == candidate => {}
                Ok(outcome) => {
                    return Err(format!(
                        "prepopulate key {key_number} returned unexpected outcome: {outcome:?}"
                    ));
                }
                Err(error) => {
                    return Err(format!("prepopulate key {key_number} failed: {error:?}"));
                }
            }
        }
        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
    fn run_worker(
        thread_id: usize,
        config: Config,
        runtime: &Runtime,
        tree: &Tree,
        phase: &AtomicU8,
        quiesced: &AtomicUsize,
        ready: &Barrier,
    ) -> Result<WorkerStats, String> {
        let worker = runtime
            .attach()
            .map_err(|error| format!("attach native worker {thread_id}: {error:?}"));
        let lookup_keys = prepare_lookup_keys(thread_id, config);
        ready.wait();
        let worker = worker?;
        let lookup_keys = lookup_keys?;

        let mut previous_phase = PHASE_WAIT;
        let mut reported_quiescence = false;
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
                PHASE_WARMUP | PHASE_MEASURE => {}
                _ => return Err(format!("worker {thread_id} observed invalid phase")),
            }
            reported_quiescence = false;

            let batch_checksum = run_lookup_batch(
                thread_id,
                config.reads_per_scope,
                tree,
                &worker,
                &lookup_keys,
            )?;

            if current_phase == PHASE_MEASURE
                && previous_phase == PHASE_MEASURE
                && phase.load(Ordering::Acquire) == PHASE_MEASURE
            {
                stats.lookups = stats
                    .lookups
                    .wrapping_add(u64::try_from(lookup_keys.len()).unwrap_or(u64::MAX));
                stats.checksum = stats.checksum.wrapping_add(batch_checksum);
            }
            previous_phase = current_phase;
        }

        worker
            .quiesce()
            .map_err(|error| format!("quiesce native worker {thread_id}: {error:?}"))?;
        Ok(stats)
    }

    fn run_lookup_batch(
        thread_id: usize,
        reads_per_scope: usize,
        tree: &Tree,
        worker: &masstree::Worker,
        lookup_keys: &[(u64, [u8; 8])],
    ) -> Result<u64, String> {
        let mut checksum = 0_u64;
        if reads_per_scope <= 1 {
            for (key_number, key) in lookup_keys {
                let found = tree
                    .get(worker, key)
                    .map_err(|error| format!("worker {thread_id} lookup failed: {error:?}"))?;
                checksum = checksum.wrapping_add(validate_lookup(thread_id, *key_number, found)?);
            }
            return Ok(checksum);
        }

        // Keep every scope inside one prepared/accounted batch. The last
        // chunk can be shorter when the batch size is not evenly divisible.
        for keys in lookup_keys.chunks(reads_per_scope) {
            let mut scope = tree.read_scope(worker).map_err(|error| {
                format!("worker {thread_id} begin read scope failed: {error:?}")
            })?;
            for (key_number, key) in keys {
                let found = scope.get(key).map_err(|error| {
                    format!("worker {thread_id} scoped lookup failed: {error:?}")
                })?;
                checksum = checksum.wrapping_add(validate_lookup(thread_id, *key_number, found)?);
            }
            scope.close().map_err(|error| {
                format!("worker {thread_id} close read scope failed: {error:?}")
            })?;
        }
        Ok(checksum)
    }

    fn validate_lookup(
        thread_id: usize,
        key_number: u64,
        found: Option<RecordId>,
    ) -> Result<u64, String> {
        let expected = RecordId::new(key_number + 1).expect("prepared key has nonzero ID");
        if found != Some(expected) {
            return Err(format!(
                "worker {thread_id} key {key_number} resolved to {found:?}, expected {expected:?}"
            ));
        }
        Ok(expected.get())
    }

    fn prepare_lookup_keys(
        thread_id: usize,
        config: Config,
    ) -> Result<Vec<(u64, [u8; 8])>, String> {
        let initial_state = worker_random_state(config.seed, thread_id);
        let mut random = SplitMix64::new(initial_state);
        let mut keys = Vec::new();
        keys.try_reserve_exact(config.batch_size)
            .map_err(|_| "lookup batch allocation failed".to_owned())?;
        for _ in 0..config.batch_size {
            let key_number = random.next_u64() % config.keyspace;
            keys.push((key_number, key_number.to_be_bytes()));
        }
        Ok(keys)
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
}
