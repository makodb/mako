//! Paired engine-only, record-terminal, and cache ACK measurements.
//!
//! Run one mode in each fresh process. The cache ACK mode has enough queue
//! capacity for every admitted write and discards the unapplied tail after
//! checking visibility and accounting. Use gc_soak for drained storage rates.

use std::collections::BTreeMap;
use std::num::NonZeroU64;
use std::path::PathBuf;
use std::ptr::NonNull;
use std::sync::{Arc, Barrier};
use std::time::Instant;

use mako_cache::{Db, Options, RecordChecksum};
use mako_engine::{CacheOrderMode, CommitDisposition, CommitRecordTarget, LocalDb};
use nix::libc;
use nix::sched::{sched_setaffinity, CpuSet};
use nix::unistd::Pid;

type Result<T> = std::result::Result<T, Box<dyn std::error::Error + Send + Sync>>;
const KEYS: u64 = 256;
const VALUE_BYTES: usize = 128;

#[cfg(feature = "allocation-profile")]
mod allocations {
    use std::alloc::{GlobalAlloc, Layout, System};
    use std::cell::Cell;
    thread_local! {
        static COUNTS: Cell<Option<(u64, u64)>> = const { Cell::new(None) };
    }
    pub struct Counting;
    unsafe impl GlobalAlloc for Counting {
        unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
            count(layout.size());
            unsafe { System.alloc(layout) }
        }
        unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
            count(layout.size());
            unsafe { System.alloc_zeroed(layout) }
        }
        unsafe fn realloc(&self, ptr: *mut u8, old: Layout, size: usize) -> *mut u8 {
            count(size);
            unsafe { System.realloc(ptr, old, size) }
        }
        unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
            unsafe { System.dealloc(ptr, layout) }
        }
    }
    fn count(bytes: usize) {
        let _ = COUNTS.try_with(|counter| {
            if let Some((count, total)) = counter.get() {
                counter.set(Some((count + 1, total + bytes as u64)));
            }
        });
    }
    pub fn start() {
        COUNTS.with(|counter| counter.set(Some((0, 0))));
    }
    pub fn stop() -> (u64, u64) {
        COUNTS.with(|counter| counter.replace(None).unwrap())
    }
}

#[cfg(feature = "allocation-profile")]
#[global_allocator]
static ALLOCATOR: allocations::Counting = allocations::Counting;

// Linux perf_event_attr version 0. The kernel supports this 64-byte prefix.
#[repr(C)]
#[derive(Default)]
struct PerfAttr {
    kind: u32,
    size: u32,
    config: u64,
    sample_period: u64,
    sample_type: u64,
    read_format: u64,
    flags: u64,
    wakeup_events: u32,
    bp_type: u32,
    config1: u64,
}

struct Perf {
    cycles: i32,
    instructions: i32,
}

impl Perf {
    fn open() -> Result<Self> {
        let event = |config: u64, group: i32| -> Result<i32> {
            let attr = PerfAttr {
                size: std::mem::size_of::<PerfAttr>() as u32,
                config,
                read_format: 1 | 2 | 8, // time_enabled, time_running, group
                flags: 1 | (1 << 5) | (1 << 6), // disabled, exclude kernel/HV
                ..PerfAttr::default()
            };
            // SAFETY: attr is the ABI's initialized prefix, pid 0 is this
            // thread, and group is either -1 or our live leader descriptor.
            let fd =
                unsafe { libc::syscall(libc::SYS_perf_event_open, &attr, 0, -1, group, 8) } as i32;
            if fd < 0 {
                return Err(std::io::Error::last_os_error().into());
            }
            Ok(fd)
        };
        let cycles = event(0, -1)?;
        let instructions = match event(1, cycles) {
            Ok(fd) => fd,
            Err(error) => {
                unsafe { libc::close(cycles) };
                return Err(error);
            }
        };
        Ok(Self {
            cycles,
            instructions,
        })
    }
    fn control(&self, request: libc::c_ulong) {
        // SAFETY: this is a live perf leader descriptor; 1 selects the group.
        assert_eq!(unsafe { libc::ioctl(self.cycles, request, 1) }, 0);
    }
    fn start(&self) {
        self.control(0x2403); // reset
        self.control(0x2400); // enable
    }
    fn stop(&self) -> [u64; 5] {
        self.control(0x2401); // disable
        let mut values = [0u64; 5];
        // Group result: nr, time_enabled, time_running, cycles, instructions.
        let bytes = unsafe {
            libc::read(
                self.cycles,
                values.as_mut_ptr().cast(),
                std::mem::size_of_val(&values),
            )
        };
        assert_eq!(bytes, std::mem::size_of_val(&values) as isize);
        assert_eq!(values[0], 2);
        values
    }
}

impl Drop for Perf {
    fn drop(&mut self) {
        unsafe {
            libc::close(self.instructions);
            libc::close(self.cycles);
        }
    }
}

fn pin(cpu: usize) -> Result<()> {
    let mut set = CpuSet::new();
    set.set(cpu)?;
    sched_setaffinity(Pid::from_raw(0), &set)?;
    Ok(())
}

fn thread_cpu_nanoseconds() -> Result<u128> {
    let mut time = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    // SAFETY: time is writable and the thread CPU clock has no external state.
    if unsafe { libc::clock_gettime(libc::CLOCK_THREAD_CPUTIME_ID, &mut time) } != 0 {
        return Err(std::io::Error::last_os_error().into());
    }
    Ok(time.tv_sec as u128 * 1_000_000_000 + time.tv_nsec as u128)
}

fn key(worker: usize, iteration: u64) -> [u8; 8] {
    ((worker as u64) * KEYS + iteration % KEYS).to_be_bytes()
}

fn value(worker: usize, iteration: u64) -> [u8; VALUE_BYTES] {
    let mut bytes = [0x5au8; VALUE_BYTES];
    bytes[..8].copy_from_slice(&(worker as u64).to_be_bytes());
    bytes[8..16].copy_from_slice(&iteration.to_be_bytes());
    bytes
}

fn expected_iteration(key: u64, final_count: u64) -> u64 {
    final_count - 1 - (final_count - 1 - key) % KEYS
}

/// Last measured/warmup iteration that writes `key_index` when every iteration
/// stages `items` distinct keys `key(worker, iteration * items + item)`.
fn expected_iteration_multi(key_index: u64, final_count: u64, items: u64) -> u64 {
    let mut iteration = final_count;
    // Coverage of one iteration's key window repeats every KEYS/ gcd windows;
    // KEYS + items steps always contain the last writer.
    let mut budget = KEYS + items;
    while iteration > 0 && budget > 0 {
        iteration -= 1;
        budget -= 1;
        let base = iteration * items;
        for item in 0..items {
            if (base + item) % KEYS == key_index {
                return iteration;
            }
        }
    }
    iteration
}

fn number(flags: &mut BTreeMap<String, String>, name: &str, default: u64) -> Result<u64> {
    flags
        .remove(name)
        .map(|value| value.parse().map_err(Into::into))
        .unwrap_or(Ok(default))
}

fn main() {
    if let Err(error) = run() {
        eprintln!("engine_compare failed: {error}");
        std::process::exit(1);
    }
}

fn run() -> Result<()> {
    let mut args = std::env::args().skip(1);
    let mode = args
        .next()
        .ok_or("expected engine, engine-logged, or cache-ack")?;
    if !matches!(mode.as_str(), "engine" | "engine-logged" | "cache-ack") {
        return Err("expected engine, engine-logged, or cache-ack".into());
    }
    let mut flags = BTreeMap::new();
    while let Some(flag) = args.next() {
        let arg = args.next().ok_or("flag missing value")?;
        if flags.insert(flag, arg).is_some() {
            return Err("duplicate flag".into());
        }
    }
    let workers = usize::try_from(number(&mut flags, "--workers", 1)?)?;
    let explicit_worker_cpus = flags.remove("--worker-cpus");
    if explicit_worker_cpus.is_some() && flags.contains_key("--worker-cpu-start") {
        return Err("--worker-cpus conflicts with --worker-cpu-start".into());
    }
    let worker_cpu_start = usize::try_from(number(&mut flags, "--worker-cpu-start", 0)?)?;
    let creation_delay_ms = number(&mut flags, "--creation-delay-ms", 0)?;
    let iterations = number(&mut flags, "--iterations", 262144)?;
    let warmup = number(&mut flags, "--warmup", 65536)?;
    let writeback_cpu = usize::try_from(number(&mut flags, "--writeback-cpu", 32)?)?;
    let perf_flag = number(&mut flags, "--perf", 1)?;
    let use_perf = perf_flag == 1;
    let items = number(&mut flags, "--items", 1)?;
    let path = flags.remove("--path").map(PathBuf::from);
    if !flags.is_empty()
        || workers == 0
        || workers > 32
        || iterations == 0
        || warmup < KEYS
        || items == 0
        || items > KEYS
        || perf_flag > 1
    {
        return Err(
            "invalid arguments, workers must be 1..32, warmup at least 256, items 1..=256".into(),
        );
    }
    if items != 1 && mode != "engine" {
        return Err("--items above one is available in engine mode only".into());
    }
    let total_per_worker = warmup.checked_add(iterations).ok_or("iteration overflow")?;
    total_per_worker
        .checked_mul(items)
        .ok_or("item schedule overflow")?;
    total_per_worker
        .checked_mul(workers as u64)
        .ok_or("commit count overflow")?;
    if mode == "engine-logged" && total_per_worker >= (1 << 48) {
        return Err("logged sequence exceeds its worker lane".into());
    }
    let worker_cpus = if let Some(text) = &explicit_worker_cpus {
        let mut cpus = Vec::with_capacity(workers);
        for part in text.split(',') {
            if part.is_empty() || !part.bytes().all(|byte| byte.is_ascii_digit()) {
                return Err("--worker-cpus expects comma-separated unsigned CPU IDs".into());
            }
            let cpu: usize = part.parse()?;
            if cpus.contains(&cpu) {
                return Err("--worker-cpus must not contain duplicates".into());
            }
            cpus.push(cpu);
        }
        if cpus.len() != workers {
            return Err("--worker-cpus needs exactly one CPU per worker".into());
        }
        cpus
    } else {
        let end = worker_cpu_start
            .checked_add(workers)
            .ok_or("CPU range overflow")?;
        (worker_cpu_start..end).collect()
    };
    let mut requested_cpus = CpuSet::new();
    for &cpu in &worker_cpus {
        requested_cpus.set(cpu)?;
    }
    if mode == "cache-ack" && worker_cpus.contains(&writeback_cpu) {
        return Err("writeback CPU overlaps worker CPUs".into());
    }
    if mode != "cache-ack" && path.is_some() {
        return Err("--path is available in cache-ack mode only".into());
    }
    // Check every requested CPU before opening a database or starting work.
    // Restore the helper mask inherited from the launcher after probing it.
    let helper_affinity = nix::sched::sched_getaffinity(Pid::from_raw(0))?;
    let affinity_result = (|| -> Result<()> {
        for &cpu in &worker_cpus {
            pin(cpu)?;
        }
        if mode == "cache-ack" {
            pin(writeback_cpu)?;
        }
        Ok(())
    })();
    nix::sched::sched_setaffinity(Pid::from_raw(0), &helper_affinity)?;
    affinity_result?;
    let queue_capacity = usize::try_from(total_per_worker)?
        .checked_next_power_of_two()
        .ok_or("queue overflow")?;
    let features = mako_engine::features()?;
    if features.test_commit_observer()
        || features.test_cleanup_failures()
        || features.test_timestamp_clock()
    {
        return Err("performance binary must not enable native or Rust engine test hooks".into());
    }
    let cache = if mode == "cache-ack" {
        let path = path.ok_or("cache-ack needs a fresh --path")?;
        if path.exists() {
            return Err("cache-ack path must not already exist".into());
        }
        let mut options = Options::default();
        options.cache.record_checksum = RecordChecksum::None;
        options.cache.writeback.capacity = queue_capacity;
        options.cache.writeback_cpu = Some(writeback_cpu);
        Some(Arc::new(Db::open(path, options)?))
    } else {
        None
    };
    let engine = if cache.is_none() {
        let db = LocalDb::open()?;
        db.bind_trusted_table("compare", 1)?;
        // SAFETY: this fresh facade has no transactions or other owners yet.
        unsafe { db.claim_cache_order_namespace(CacheOrderMode::PerWorker)? };
        Some(Arc::new(db))
    } else {
        None
    };
    let barrier = Arc::new(Barrier::new(workers + 1));
    let measured_done = Arc::new(Barrier::new(workers));
    let mut handles = Vec::new();
    for worker in 0..workers {
        let worker_cpu = worker_cpus[worker];
        let barrier = Arc::clone(&barrier);
        let measured_done = Arc::clone(&measured_done);
        let cache = cache.clone();
        let engine = engine.clone();
        let logged = mode == "engine-logged";
        handles.push(std::thread::spawn(move || {
            let result = (|| -> Result<_> {
                pin(worker_cpu)?;
                let perf = use_perf.then(Perf::open).transpose()?;
                let mut record = [0u8; 256];
                let mut put = |iteration| -> Result<()> {
                    // Item 0 of iteration i is key(i * items); the remaining
                    // items are key(i * items + j), so the write schedule is
                    // exactly expected_iteration_multi's model.
                    let key = crate::key(worker, iteration * items);
                    let value = value(worker, iteration);
                    if let Some(db) = &cache {
                        db.put(&key, &value)?;
                    } else {
                        let (table, mut tx) =
                            engine.as_ref().unwrap().trusted_bound_transaction()?;
                        tx.put(&table, &key, &value)?;
                        for item in 1..items {
                            let item_key = crate::key(worker, iteration * items + item);
                            tx.put(&table, &item_key, &value)?;
                        }
                        if logged {
                            let candidate = tx
                                .unchecked_one_put_record_candidate()
                                .ok_or("missing one-put candidate")?;
                            let sequence =
                                NonZeroU64::new(((worker as u64 + 1) << 48) | (iteration + 1))
                                    .unwrap();
                            // SAFETY: this fresh candidate is current, and the
                            // distinct lane sequence and stack buffer belong to
                            // this transaction until its synchronous return.
                            let report = unsafe {
                                tx.commit_report_with_unchecked_one_put_record_target(
                                    candidate,
                                    |_, plan| {
                                        assert!(plan.exact_record_bytes() <= record.len());
                                        Some(CommitRecordTarget::from_raw_parts(
                                            sequence,
                                            NonNull::new(record.as_mut_ptr()).unwrap(),
                                            plan.exact_record_bytes(),
                                        ))
                                    },
                                )
                            };
                            if report.commit.disposition != CommitDisposition::Committed
                                || report.commit.cleanup.is_err()
                                || !report.completion_contract_valid
                                || !report.record_written
                            {
                                return Err(format!("record commit failed: {report:?}").into());
                            }
                            std::hint::black_box(&record);
                        } else {
                            tx.commit()?;
                        }
                    }
                    Ok(())
                };
                for iteration in 0..warmup {
                    put(iteration)?;
                }
                barrier.wait();
                // The diagnostic envelope includes PMU setup/teardown, but no
                // seeding, barriers, verification, or output.
                let diagnostic_start = Instant::now();
                let cpu_start = thread_cpu_nanoseconds()?;
                #[cfg(feature = "allocation-profile")]
                allocations::start();
                if let Some(perf) = &perf {
                    perf.start();
                }
                let start = Instant::now();
                for iteration in warmup..total_per_worker {
                    put(iteration)?;
                }
                let nanoseconds = start.elapsed().as_nanos();
                let counts = perf.as_ref().map(Perf::stop).unwrap_or([0; 5]);
                #[cfg(feature = "allocation-profile")]
                let allocated = allocations::stop();
                #[cfg(not(feature = "allocation-profile"))]
                let allocated = (0, 0);
                let cpu_nanoseconds = thread_cpu_nanoseconds()? - cpu_start;
                let diagnostic_nanoseconds = diagnostic_start.elapsed().as_nanos();
                measured_done.wait();
                // Verify every final key after the measured region, on the same
                // attached worker, without flushing the ACK benchmark's queue.
                for index in 0..KEYS {
                    let expected = value(
                        worker,
                        if items == 1 {
                            expected_iteration(index, total_per_worker)
                        } else {
                            expected_iteration_multi(index, total_per_worker, items)
                        },
                    );
                    let actual = if let Some(db) = &cache {
                        db.get(&key(worker, index))?
                    } else {
                        let (table, mut tx) =
                            engine.as_ref().unwrap().trusted_bound_transaction()?;
                        let actual = tx.get(&table, &key(worker, index))?;
                        tx.commit()?;
                        actual
                    };
                    if actual.as_deref() != Some(expected.as_slice()) {
                        return Err(format!("value mismatch worker {worker} key {index}").into());
                    }
                }
                Ok((
                    nanoseconds,
                    counts,
                    allocated,
                    cpu_nanoseconds,
                    diagnostic_nanoseconds,
                ))
            })();
            if let Err(error) = &result {
                // A pre-barrier failure must terminate the sample instead
                // of leaving peers permanently waiting at the barrier.
                eprintln!("worker {worker} failed: {error}");
                std::process::exit(1);
            }
            result
        }));
        // Changes initial worker registration only, never timed transaction work.
        if creation_delay_ms != 0 {
            std::thread::sleep(std::time::Duration::from_millis(creation_delay_ms));
        }
    }
    barrier.wait();
    let mut nanoseconds = 0;
    let mut cycles = 0;
    let mut instructions = 0;
    let mut min_running_ratio = 1.0f64;
    let mut allocation_count = 0;
    let mut allocation_bytes = 0;
    for (worker, handle) in handles.into_iter().enumerate() {
        let (elapsed, counts, allocated, cpu_nanoseconds, diagnostic_nanoseconds) =
            handle.join().expect("worker panicked")?;
        eprintln!("WORKER {{\"worker\":{worker},\"cpu\":{},\"wall_ns\":{elapsed},\"cpu_ns\":{cpu_nanoseconds},\"diagnostic_wall_ns\":{diagnostic_nanoseconds},\"cycles\":{},\"instructions\":{},\"pmu_time_enabled_ns\":{},\"pmu_time_running_ns\":{}}}",
            worker_cpus[worker], counts[3], counts[4], counts[1], counts[2]);
        nanoseconds = nanoseconds.max(elapsed);
        cycles += counts[3];
        instructions += counts[4];
        if use_perf {
            if counts[1] == 0 || counts[2] == 0 {
                return Err("PMU did not run during the measured region".into());
            }
            min_running_ratio = min_running_ratio.min(counts[2] as f64 / counts[1] as f64);
        }
        allocation_count += allocated.0;
        allocation_bytes += allocated.1;
    }
    let commits = iterations * workers as u64;
    let (acknowledged, applied, queued) = if let Some(db) = &cache {
        let status = db.status()?;
        if !status.is_healthy()
            || status.acknowledged_transactions != total_per_worker * workers as u64
        {
            return Err(format!("cache accounting or health failed: {status:?}").into());
        }
        (
            status.acknowledged_transactions,
            status.applied_watermark.sequence(),
            status.queued_transactions,
        )
    } else {
        (total_per_worker * workers as u64, 0, 0)
    };
    let worker_cpu_start_json = if explicit_worker_cpus.is_some() {
        "null".to_owned()
    } else {
        worker_cpu_start.to_string()
    };
    println!("{{\"mode\":\"{mode}\",\"engine\":\"{}\",\"workers\":{workers},\"iterations_per_worker\":{iterations},\"warmup_per_worker\":{warmup},\"keys_per_worker\":{KEYS},\"value_bytes\":{VALUE_BYTES},\"checksum\":\"none\",\"queue_capacity_per_worker\":{queue_capacity},\"commits\":{commits},\"max_worker_nanoseconds\":{nanoseconds},\"transactions_per_second\":{:.3},\"cycles_per_commit\":{:.3},\"instructions_per_commit\":{:.3},\"pmu_min_running_ratio\":{min_running_ratio:.8},\"perf_enabled\":{use_perf},\"allocation_profile\":{},\"rust_allocation_count\":{allocation_count},\"rust_allocated_bytes\":{allocation_bytes},\"rust_allocations_per_commit\":{:.3},\"rust_allocated_bytes_per_commit\":{:.3},\"acknowledged\":{acknowledged},\"applied_before_abort\":{applied},\"queued_before_abort\":{queued},\"verified_keys\":{},\"drained\":false,\"items_per_transaction\":{items},\"worker_cpu_start\":{worker_cpu_start_json},\"worker_cpus\":{worker_cpus:?},\"writeback_cpu\":{writeback_cpu},\"creation_delay_ms\":{creation_delay_ms},\"verified\":true}}",
        mako_cache::engine_name(), commits as f64 * 1e9 / nanoseconds as f64,
        cycles as f64 / commits as f64, instructions as f64 / commits as f64,
        cfg!(feature = "allocation-profile"), allocation_count as f64 / commits as f64,
        allocation_bytes as f64 / commits as f64, workers as u64 * KEYS);
    if let Some(db) = cache {
        Arc::try_unwrap(db)
            .map_err(|_| "live cache owner after workers joined")?
            .abort_without_flush()?;
    }
    Ok(())
}
