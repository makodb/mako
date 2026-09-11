//! Sustained, bounded-queue cache workload with complete drain and cold reopen.
//!
//! Unlike the foreground-only sweep, this binary uses production defaults and
//! runs until a wall-clock deadline while RocksDB catches up or applies queue
//! backpressure. `run` and `verify` must be separate processes. No test-support
//! feature or direct backend access is used. Keep the same binary source when
//! building the pre-GC reference and GC candidate.

use std::collections::BTreeMap;
use std::fs::{self, OpenOptions};
use std::io::Write;
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Barrier};
use std::thread;
use std::time::{Duration, Instant};

use mako_cache::{Db, Options};
use nix::sched::{sched_setaffinity, CpuSet};
use nix::unistd::Pid;

type Result<T> = std::result::Result<T, Box<dyn std::error::Error + Send + Sync>>;
const HISTOGRAM_BUCKETS: usize = 64 * 16;
const LATENCY_SAMPLE_STRIDE: u64 = 256;

fn main() {
    if let Err(error) = main_result() {
        eprintln!("gc_soak failed: {error}");
        std::process::exit(1);
    }
}

fn main_result() -> Result<()> {
    let mut args = std::env::args().skip(1);
    let mode = args.next().ok_or("expected run or verify")?;
    let mut flags = BTreeMap::new();
    while let Some(flag) = args.next() {
        let value = args.next().ok_or("flag missing value")?;
        if flags.insert(flag, value).is_some() {
            return Err("duplicate flag".into());
        }
    }
    let path = PathBuf::from(flags.remove("--path").ok_or("missing --path")?);
    let manifest = PathBuf::from(flags.remove("--manifest").ok_or("missing --manifest")?);
    let writeback_cpu = number(&mut flags, "--writeback-cpu", 32)? as usize;
    let mut options = Options::default();
    options.cache.writeback_cpu = Some(writeback_cpu);
    if mode == "verify" {
        if !flags.is_empty() {
            return Err(format!("unknown verify flags: {flags:?}").into());
        }
        return verify(&path, &manifest, options);
    }
    if mode != "run" {
        return Err("expected run or verify".into());
    }
    let workers = number(&mut flags, "--workers", 1)? as usize;
    let seconds = number(&mut flags, "--seconds", 960)?;
    let sample_seconds = number(&mut flags, "--sample-seconds", 10)?;
    let keys = number(&mut flags, "--keys-per-worker", 256)?;
    let value_bytes = number(&mut flags, "--value-bytes", 128)? as usize;
    let disk_limit = number(&mut flags, "--disk-limit-bytes", 96 << 30)?;
    let minimum_free = number(&mut flags, "--minimum-free-bytes", 80 << 30)?;
    let rate = number(&mut flags, "--rate-per-worker", 0)?;
    if !flags.is_empty() {
        return Err(format!("unknown flags: {flags:?}").into());
    }
    if workers == 0
        || workers > 32
        || keys == 0
        || keys > u32::MAX as u64
        || value_bytes < 16
        || value_bytes > 1 << 20
        || seconds == 0
        || sample_seconds == 0
    {
        return Err("invalid worker, key, value, or duration setting".into());
    }
    if path.exists() || manifest.exists() {
        return Err("run requires fresh database and manifest paths".into());
    }
    if fs::read_to_string("/sys/devices/system/cpu/cpufreq/boost")?.trim() != "0" {
        return Err("CPU boost must be disabled before benchmarking".into());
    }
    println!(
        "{{\"event\":\"protocol\",\"version\":1,\"workers\":{workers},\"seconds\":{seconds},\"keys_per_worker\":{keys},\"value_bytes\":{value_bytes},\"rate_per_worker\":{rate},\"queue_capacity_per_worker\":{},\"max_batch_records\":{},\"writeback_cpu\":{writeback_cpu},\"checksum\":\"crc32c\",\"wal\":true,\"sync\":false,\"latency_sample_stride\":{LATENCY_SAMPLE_STRIDE},\"options\":{}}}",
        options.cache.writeback.capacity,
        options.cache.writeback.max_batch_records,
        json_string(&format!("{options:?}")),
    );
    let db = Arc::new(Db::open(&path, options)?);
    let barrier = Arc::new(Barrier::new(workers + 1));
    let started = Arc::new(AtomicBool::new(false));
    let stop = Arc::new(AtomicBool::new(false));
    let mut handles = Vec::with_capacity(workers);
    for worker in 0..workers {
        let db = Arc::clone(&db);
        let barrier = Arc::clone(&barrier);
        let started = Arc::clone(&started);
        let stop = Arc::clone(&stop);
        handles.push(thread::spawn(move || -> Result<WorkerResult> {
            let mut cpus = CpuSet::new();
            let pinned = cpus
                .set(worker)
                .and_then(|()| sched_setaffinity(Pid::from_raw(0), &cpus));
            // Always rendezvous even if affinity failed, so another worker
            // cannot hang at the barrier while its peer has already returned.
            barrier.wait();
            pinned?;
            while !started.load(Ordering::Acquire) {
                std::hint::spin_loop();
            }
            let start = Instant::now();
            let mut result = WorkerResult::default();
            let mut value = vec![0x5a; value_bytes];
            let mut sampled_start = Instant::now();
            // Jitter sample positions so a power-of-two sample stride cannot
            // alias the 64-record apply batches and systematically miss queue
            // waits. The PRNG runs only on sampled commits, outside the timed
            // logical operation. The average gap is about 256 commits.
            let mut random = 0x9e37_79b9_7f4a_7c15u64 ^ (worker as u64 + 1);
            let mut next_sample = worker as u64;
            while !stop.load(Ordering::Relaxed) {
                let ordinal = result.commits;
                let key = ((worker as u64) << 32 | ordinal % keys).to_be_bytes();
                value[..8].copy_from_slice(&ordinal.to_be_bytes());
                value[8..16].copy_from_slice(&key);
                let sampled = ordinal == next_sample;
                if sampled {
                    sampled_start = Instant::now();
                }
                loop {
                    match db.put(&key, &value) {
                        Ok(()) => break,
                        Err(error) if error.is_conflict() => result.conflicts += 1,
                        Err(error) => {
                            stop.store(true, Ordering::Release);
                            return Err(error.into());
                        }
                    }
                    if stop.load(Ordering::Relaxed) {
                        return Ok(result);
                    }
                }
                result.commits += 1;
                if sampled {
                    let nanos = sampled_start.elapsed().as_nanos().min(u64::MAX as u128) as u64;
                    result.histogram[bucket(nanos)] += 1;
                    random ^= random << 13;
                    random ^= random >> 7;
                    random ^= random << 17;
                    next_sample += LATENCY_SAMPLE_STRIDE / 2 + random % LATENCY_SAMPLE_STRIDE;
                }
                if rate != 0 && result.commits % 64 == 0 {
                    let due = Duration::from_secs_f64(result.commits as f64 / rate as f64);
                    if let Some(delay) = due.checked_sub(start.elapsed()) {
                        thread::sleep(delay);
                    }
                }
            }
            Ok(result)
        }));
    }
    barrier.wait();
    let io_before = io_written();
    let start = Instant::now();
    started.store(true, Ordering::Release);
    let mut maximum_queue = 0;
    let mut last_sample = Instant::now();
    let mut failure = None;
    while start.elapsed() < Duration::from_secs(seconds) && !stop.load(Ordering::Acquire) {
        thread::sleep(Duration::from_millis(100));
        if last_sample.elapsed() < Duration::from_secs(sample_seconds) {
            continue;
        }
        last_sample = Instant::now();
        let status = db.status()?;
        maximum_queue = maximum_queue.max(status.queued_transactions);
        let disk_bytes = disk_usage(&path)?;
        let free_bytes = free_bytes(&path)?;
        println!(
            "{{\"event\":\"sample\",\"elapsed_seconds\":{:.3},\"acknowledged\":{},\"applied\":{},\"queue_records\":{},\"disk_bytes\":{disk_bytes},\"free_bytes\":{free_bytes},\"process_write_bytes\":{},\"thread_cpu_ticks\":{},\"status\":{}}}",
            start.elapsed().as_secs_f64(), status.acknowledged_transactions,
            status.applied_watermark.sequence(), status.queued_transactions,
            io_written().saturating_sub(io_before), thread_cpu_ticks()?, json_string(&format!("{status:?}")),
        );
        if !status.is_healthy() {
            failure = Some(format!("cache became unhealthy: {status:?}"));
            break;
        }
        if disk_bytes > disk_limit || free_bytes < minimum_free {
            failure = Some(format!(
                "disk budget reached: database {disk_bytes}, free {free_bytes}"
            ));
            break;
        }
    }
    stop.store(true, Ordering::Release);
    let mut results = Vec::with_capacity(workers);
    for handle in handles {
        results.push(handle.join().map_err(|_| "foreground worker panicked")??);
    }
    let ack_seconds = start.elapsed().as_secs_f64();
    let commits: u64 = results.iter().map(|worker| worker.commits).sum();
    let conflicts: u64 = results.iter().map(|worker| worker.conflicts).sum();
    let mut histogram = [0; HISTOGRAM_BUCKETS];
    for result in &results {
        for (combined, count) in histogram.iter_mut().zip(result.histogram) {
            *combined += count;
        }
    }
    let drain_start = Instant::now();
    let applied = db.wait_applied()?;
    let drain_seconds = drain_start.elapsed().as_secs_f64();
    let applied_seconds = start.elapsed().as_secs_f64();
    let status = db.status()?;
    if applied != commits || status.queued_transactions != 0 {
        return Err(format!("drain mismatch: {commits} commits, {status:?}").into());
    }
    let disk_bytes = disk_usage(&path)?;
    println!(
        "{{\"event\":\"drained\",\"commits\":{commits},\"ack_seconds\":{ack_seconds:.6},\"applied_seconds\":{applied_seconds:.6},\"drain_seconds\":{drain_seconds:.6},\"ack_tps\":{:.3},\"applied_tps\":{:.3},\"ack_p99_upper_ns\":{},\"ack_latency_samples\":{},\"conflicts\":{conflicts},\"maximum_sampled_queue\":{maximum_queue},\"disk_bytes\":{disk_bytes},\"process_write_bytes\":{},\"status\":{}}}",
        commits as f64 / ack_seconds, commits as f64 / applied_seconds,
        percentile_upper(&histogram, 99), histogram.iter().sum::<u64>(),
        io_written().saturating_sub(io_before), json_string(&format!("{status:?}")),
    );
    Arc::try_unwrap(db)
        .map_err(|_| "cache is still shared at close")?
        .close()?;
    if let Some(failure) = failure {
        return Err(failure.into());
    }
    if fs::read_to_string("/sys/devices/system/cpu/cpufreq/boost")?.trim() != "0" {
        return Err("CPU boost changed during run".into());
    }
    let mut output = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(manifest)?;
    writeln!(output, "gc-soak-v1 {workers} {keys} {value_bytes}")?;
    for worker in results {
        writeln!(output, "{}", worker.commits)?;
    }
    output.sync_all()?;
    Ok(())
}

fn verify(path: &Path, manifest: &Path, options: Options) -> Result<()> {
    let expected = fs::read_to_string(manifest)?;
    let mut words = expected.split_whitespace();
    if words.next() != Some("gc-soak-v1") {
        return Err("unknown expected-state format".into());
    }
    let workers: usize = words.next().ok_or("missing workers")?.parse()?;
    let keys: u64 = words.next().ok_or("missing keys")?.parse()?;
    let value_bytes: usize = words.next().ok_or("missing value bytes")?.parse()?;
    let counts: Vec<u64> = words
        .map(str::parse)
        .collect::<std::result::Result<_, _>>()?;
    if counts.len() != workers || counts.iter().any(|count| *count < keys) {
        return Err("incomplete worker expected state".into());
    }
    let open_start = Instant::now();
    let db = Db::open(path, options)?;
    let open_seconds = open_start.elapsed().as_secs_f64();
    let expected_total: u64 = counts.iter().sum();
    if db.applied_sequence() != expected_total {
        return Err("recovered applied count differs from completed run".into());
    }
    let validation_start = Instant::now();
    for (worker, commits) in counts.iter().enumerate() {
        for slot in 0..keys {
            let last = commits - 1 - (commits - 1 + keys - slot) % keys;
            let key = ((worker as u64) << 32 | slot).to_be_bytes();
            let mut expected = vec![0x5a; value_bytes];
            expected[..8].copy_from_slice(&last.to_be_bytes());
            expected[8..16].copy_from_slice(&key);
            if db.get(&key)?.as_deref() != Some(expected.as_slice()) {
                return Err(
                    format!("recovered value mismatch at worker {worker}, slot {slot}").into(),
                );
            }
        }
    }
    let validation_seconds = validation_start.elapsed().as_secs_f64();
    // This also exercises sequence/clock reseeding after recovery. The child
    // is the only process that has opened the namespace since close.
    db.put(b"gc-soak-reopen-probe", b"ok")?;
    if db.wait_applied()? != expected_total + 1 {
        return Err("reopen progress did not advance".into());
    }
    let status = db.status()?;
    println!(
        "{{\"event\":\"verified\",\"workers\":{workers},\"commits\":{expected_total},\"keys\":{},\"open_seconds\":{open_seconds:.6},\"validation_seconds\":{validation_seconds:.6},\"disk_bytes\":{},\"status\":{}}}",
        keys * workers as u64, disk_usage(path)?, json_string(&format!("{status:?}")),
    );
    db.close()?;
    Ok(())
}

struct WorkerResult {
    commits: u64,
    conflicts: u64,
    histogram: [u64; HISTOGRAM_BUCKETS],
}

impl Default for WorkerResult {
    fn default() -> Self {
        Self {
            commits: 0,
            conflicts: 0,
            histogram: [0; HISTOGRAM_BUCKETS],
        }
    }
}

fn number(flags: &mut BTreeMap<String, String>, name: &str, default: u64) -> Result<u64> {
    Ok(match flags.remove(name) {
        Some(value) => value.parse()?,
        None => default,
    })
}

fn bucket(value: u64) -> usize {
    let exponent = (63 - value.max(1).leading_zeros()) as usize;
    let shift = exponent.saturating_sub(4);
    exponent * 16 + ((value >> shift) as usize & 15)
}

fn percentile_upper(histogram: &[u64; HISTOGRAM_BUCKETS], percentile: u64) -> u64 {
    let target = (histogram.iter().sum::<u64>() * percentile).div_ceil(100);
    if target == 0 {
        return 0;
    }
    let mut covered = 0;
    for (index, count) in histogram.iter().enumerate() {
        covered += count;
        if covered >= target {
            let exponent = index / 16;
            let lower = index % 16;
            return if exponent < 4 {
                lower as u64
            } else {
                (16 + lower as u64 + 1)
                    .checked_shl((exponent - 4) as u32)
                    .unwrap_or(u64::MAX)
                    .saturating_sub(1)
            };
        }
    }
    u64::MAX
}

fn json_string(value: &str) -> String {
    let mut output = String::from("\"");
    for character in value.chars() {
        match character {
            '"' => output.push_str("\\\""),
            '\\' => output.push_str("\\\\"),
            '\n' => output.push_str("\\n"),
            '\r' => output.push_str("\\r"),
            '\t' => output.push_str("\\t"),
            c if c < ' ' => output.push_str(&format!("\\u{:04x}", c as u32)),
            c => output.push(c),
        }
    }
    output.push('"');
    output
}

fn disk_usage(path: &Path) -> Result<u64> {
    let mut bytes = 0;
    for entry in fs::read_dir(path)? {
        let path = entry?.path();
        // RocksDB can remove obsolete files while the monitor is walking.
        let metadata = match fs::symlink_metadata(&path) {
            Ok(metadata) => metadata,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => continue,
            Err(error) => return Err(error.into()),
        };
        bytes += metadata.blocks() * 512;
        if metadata.is_dir() {
            bytes += disk_usage(&path)?;
        }
    }
    Ok(bytes)
}

fn free_bytes(path: &Path) -> Result<u64> {
    let output = std::process::Command::new("df")
        .args(["-B1", "--output=avail"])
        .arg(path)
        .output()?;
    if !output.status.success() {
        return Err("df failed".into());
    }
    Ok(std::str::from_utf8(&output.stdout)?
        .split_whitespace()
        .last()
        .ok_or("df returned no available bytes")?
        .parse()?)
}

fn io_written() -> u64 {
    fs::read_to_string("/proc/self/io")
        .ok()
        .and_then(|text| {
            text.lines().find_map(|line| {
                line.strip_prefix("write_bytes:")
                    .and_then(|value| value.trim().parse().ok())
            })
        })
        .unwrap_or(0)
}

fn thread_cpu_ticks() -> Result<String> {
    let mut named = BTreeMap::<String, u64>::new();
    for entry in fs::read_dir("/proc/self/task")? {
        let path = entry?.path();
        let stat = match fs::read_to_string(path.join("stat")) {
            Ok(stat) => stat,
            Err(_) => continue,
        };
        let Some((_, tail)) = stat.rsplit_once(')') else {
            continue;
        };
        let columns: Vec<_> = tail.split_whitespace().collect();
        if columns.len() < 13 {
            continue;
        }
        let name = fs::read_to_string(path.join("comm")).unwrap_or_default();
        let user: u64 = columns[11].parse()?;
        let system: u64 = columns[12].parse()?;
        *named.entry(name.trim().to_owned()).or_default() += user + system;
    }
    Ok(format!(
        "{{{}}}",
        named
            .into_iter()
            .map(|(name, ticks)| format!("{}:{ticks}", json_string(&name)))
            .collect::<Vec<_>>()
            .join(",")
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn histogram_percentile_is_a_bounded_upper_estimate() {
        for value in [0, 1, 7, 15, 16, 31, 255, 1000, 100_000, 1_000_000_000] {
            let mut histogram = [0; HISTOGRAM_BUCKETS];
            histogram[bucket(value)] = 1;
            let upper = percentile_upper(&histogram, 99);
            assert!(upper >= value, "{value}: {upper}");
            assert!(upper <= value.saturating_add(value / 16).max(value));
        }
    }
}
