//! Manual performance comparison with MassTrans at 0d26e2e0b. Same SplitMix
//! stream, unique keys per transaction, warmup/quiesce/measure and retries.
//! Uses the production byte-row adapter, including normal buffer ownership.
#[cfg(not(mtree_native_integration))]
fn main() {
    panic!("native_parity requires the MASSTREE_TX native build environment");
}
#[cfg(mtree_native_integration)]
fn main() {
    native::run();
}
#[cfg(mtree_native_integration)]
mod native {
    use masstree::{Runtime as NativeRuntime, RuntimeConfig as NativeConfig};
    use std::{
        sync::{
            atomic::{AtomicU64, AtomicU8, Ordering},
            Arc,
        },
        thread,
        time::{Duration, Instant},
    };
    use sto_core::{CommitOutcome, Runtime, RuntimeConfig};
    use sto_masstree::{Table, TableConfig};

    const GAMMA: u64 = 0x9E3779B97F4A7C15;
    fn scramble(mut n: u64) -> u64 {
        n = (n ^ (n >> 30)).wrapping_mul(0xBF58476D1CE4E5B9);
        n = (n ^ (n >> 27)).wrapping_mul(0x94D049BB133111EB);
        n ^ (n >> 31)
    }
    fn next(state: &mut u64) -> u64 {
        *state = state.wrapping_add(GAMMA);
        scramble(*state)
    }
    #[derive(Clone)]
    struct Config {
        threads: u64,
        keys: u64,
        ops: u64,
        writes: u64,
        warmup: u64,
        duration: u64,
        seed: u64,
        rcu_scope: u64,
    }
    impl Config {
        fn parse() -> Self {
            let mut c = Self {
                threads: 1,
                keys: 100000,
                ops: 10,
                writes: 50,
                warmup: 1000,
                duration: 3000,
                seed: 1,
                rcu_scope: 0,
            };
            let mut args = std::env::args().skip(1);
            while let Some(arg) = args.next() {
                let n = args
                    .next()
                    .expect("option value")
                    .parse()
                    .expect("integer value");
                match arg.as_str() {
                    "--threads" => c.threads = n,
                    "--keyspace" => c.keys = n,
                    "--ops-per-txn" => c.ops = n,
                    "--write-percent" => c.writes = n,
                    "--warmup-ms" => c.warmup = n,
                    "--duration-ms" => c.duration = n,
                    "--seed" => c.seed = n,
                    "--rcu-scope" => c.rcu_scope = n,
                    _ => panic!("unknown option {arg}"),
                }
            }
            assert!(
                c.threads > 0
                    && c.threads < 256
                    && c.ops > 0
                    && c.ops <= c.keys
                    && c.ops <= 32768
                    && c.writes <= 100
                    && c.rcu_scope <= 1
                    && c.duration > 0
            );
            c
        }
    }
    #[derive(Default)]
    struct Counts {
        commits: u64,
        attempts: u64,
        aborts: u64,
        checksum: u64,
        all_writes: u64,
    }
    struct Op {
        number: u64,
        key: [u8; 8],
        write: bool,
    }
    pub fn run() {
        // A failed worker must fail the process rather than leave phase waits hung.
        let hook = std::panic::take_hook();
        std::panic::set_hook(Box::new(move |e| {
            hook(e);
            std::process::exit(1);
        }));
        let c = Config::parse();
        let native =
            NativeRuntime::new(NativeConfig::new().with_max_threads(c.threads as u32 + 1)).unwrap();
        let runtime =
            Runtime::new(RuntimeConfig::default().with_max_workers(c.threads as usize + 1))
                .unwrap();
        let native_loader = native.attach().unwrap();
        let table = Arc::new(
            Table::new(
                &runtime,
                &native,
                &native_loader,
                TableConfig::new().with_max_retained_records(c.keys + 1),
            )
            .unwrap(),
        );
        let mut loader = runtime.attach().unwrap();
        for start in (0..c.keys).step_by(64) {
            let mut tx = loader.begin().unwrap();
            for key in start..(start + 64).min(c.keys) {
                table
                    .set(
                        &mut tx,
                        &native_loader,
                        &key.to_be_bytes(),
                        &key.to_le_bytes(),
                    )
                    .unwrap();
            }
            assert!(matches!(tx.commit().unwrap(), CommitOutcome::Committed(_)));
        }
        let phase = Arc::new(AtomicU8::new(0));
        let ready = Arc::new(AtomicU64::new(0));
        let quiesced = Arc::new(AtomicU64::new(0));
        let handles: Vec<_> = (0..c.threads)
            .map(|id| {
                let (runtime, native, table, c) =
                    (runtime.clone(), native.clone(), table.clone(), c.clone());
                let (phase, ready, quiesced) = (phase.clone(), ready.clone(), quiesced.clone());
                thread::spawn(move || {
                    let native_worker = native.attach().unwrap();
                    let mut worker = runtime.attach().unwrap();
                    let initial = scramble(c.seed.wrapping_add((id + 1).wrapping_mul(GAMMA)));
                    let mut random = initial;
                    let mut operations: Vec<Op> = Vec::with_capacity(c.ops as usize);
                    let mut result = Counts::default();
                    let mut bytes = Vec::with_capacity(8);
                    let mut previous = 0;
                    let mut reported = false;
                    ready.fetch_add(1, Ordering::Release);
                    loop {
                        let current = phase.load(Ordering::Acquire);
                        match current {
                            0 => {
                                thread::yield_now();
                                continue;
                            }
                            2 => {
                                if !reported {
                                    quiesced.fetch_add(1, Ordering::Release);
                                    reported = true;
                                }
                                previous = current;
                                thread::yield_now();
                                continue;
                            }
                            4 => break,
                            _ => {}
                        }
                        reported = false;
                        if current == 3 && previous != 3 {
                            random = initial;
                        }
                        previous = current;
                        operations.clear();
                        for _ in 0..c.ops {
                            let mut number = next(&mut random) % c.keys;
                            while operations.iter().any(|o| o.number == number) {
                                number = (number + 1) % c.keys;
                            }
                            operations.push(Op {
                                number,
                                key: number.to_be_bytes(),
                                write: next(&mut random) % 100 < c.writes,
                            });
                        }
                        let (mut attempts, mut aborts) = (0, 0);
                        let checksum = loop {
                            if phase.load(Ordering::Acquire) == 4 {
                                return result;
                            }
                            attempts += 1;
                            let _scope =
                                (c.rcu_scope != 0).then(|| native_worker.rcu_scope().unwrap());
                            let mut tx = worker.begin().unwrap();
                            let mut sum = 0u64;
                            let mut failed = false;
                            for op in &operations {
                                match table.get_into(&mut tx, &native_worker, &op.key, &mut bytes) {
                                    Ok(true) => {}
                                    Ok(false) => panic!("lost preloaded key"),
                                    Err(e) => {
                                        assert!(
                                            matches!(e, sto_core::AccessError::Conflict(_)),
                                            "{e:?}"
                                        );
                                        failed = true;
                                        break;
                                    }
                                };
                                let value =
                                    u64::from_le_bytes(bytes.as_slice().try_into().unwrap());
                                sum = sum.wrapping_add(value);
                                if op.write {
                                    if let Err(e) = table.set(
                                        &mut tx,
                                        &native_worker,
                                        &op.key,
                                        &(value + 1).to_le_bytes(),
                                    ) {
                                        assert!(
                                            matches!(e, sto_core::AccessError::Conflict(_)),
                                            "{e:?}"
                                        );
                                        failed = true;
                                        break;
                                    }
                                }
                            }
                            if let Some(scope) = _scope {
                                scope.close().unwrap();
                            }
                            if !failed
                                && matches!(tx.commit().unwrap(), CommitOutcome::Committed(_))
                            {
                                break sum;
                            }
                            aborts += 1;
                        };
                        result.all_writes += operations.iter().filter(|o| o.write).count() as u64;
                        if current == 3 && phase.load(Ordering::Acquire) == 3 {
                            result.commits += 1;
                            result.attempts += attempts;
                            result.aborts += aborts;
                            result.checksum = result.checksum.wrapping_add(checksum);
                        }
                    }
                    result
                })
            })
            .collect();
        while ready.load(Ordering::Acquire) != c.threads {
            thread::yield_now();
        }
        phase.store(1, Ordering::Release);
        thread::sleep(Duration::from_millis(c.warmup));
        phase.store(2, Ordering::Release);
        while quiesced.load(Ordering::Acquire) != c.threads {
            thread::yield_now();
        }
        let start = Instant::now();
        phase.store(3, Ordering::Release);
        thread::sleep(Duration::from_millis(c.duration));
        phase.store(4, Ordering::Release);
        let elapsed = start.elapsed();
        let mut total = Counts::default();
        for handle in handles {
            let r = handle.join().unwrap();
            total.commits += r.commits;
            total.attempts += r.attempts;
            total.aborts += r.aborts;
            total.checksum = total.checksum.wrapping_add(r.checksum);
            total.all_writes += r.all_writes;
        }
        // Conservation checks include warmup and boundary-crossing commits.
        let mut sum = 0u64;
        for start in (0..c.keys).step_by(64) {
            let mut tx = loader.begin().unwrap();
            for key in start..(start + 64).min(c.keys) {
                let value = table
                    .get(&mut tx, &native_loader, &key.to_be_bytes())
                    .unwrap()
                    .unwrap();
                let value = u64::from_le_bytes(value.as_ref().try_into().unwrap());
                assert!(value >= key);
                sum += value - key;
            }
            assert!(matches!(tx.commit().unwrap(), CommitOutcome::Committed(_)));
        }
        assert_eq!(sum, total.all_writes);
        assert_eq!(total.attempts, total.commits + total.aborts);
        assert!(total.commits > 0);
        let ops = total.commits * c.ops;
        println!("BENCH_RESULT={{\"engine\":\"native-byte-rows\",\"threads\":{},\"keyspace\":{},\"ops_per_txn\":{},\"write_percent\":{},\"seed\":{},\"rcu_scope\":{},\"commits\":{},\"attempts\":{},\"aborts\":{},\"logical_ops\":{},\"elapsed_ns\":{},\"ops_per_sec\":{},\"checksum\":{},\"verified_writes\":{}}}", c.threads, c.keys, c.ops, c.writes, c.seed, c.rcu_scope, total.commits, total.attempts, total.aborts, ops, elapsed.as_nanos(), ops as f64 / elapsed.as_secs_f64(), total.checksum, sum);
    }
}
