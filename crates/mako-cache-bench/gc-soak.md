# Log GC sustained benchmark

`gc_soak` runs the public cache API until a deadline, waits for every
acknowledged transaction to reach RocksDB, closes the cache, and validates all
values in a fresh process. The standalone binary is in the benchmark package
so its build does not enable `mako-cache`'s development-only RocksDB hooks.

The default workload updates 256 fixed keys per worker with 128-byte values.
Every update records a worker-local counter in the value. Each worker owns
disjoint keys, and the final counter vector is sufficient to reconstruct and
check every expected value. Recovery must restore the acknowledged transaction
count, and one further write checks that progress advances after reopening.

The default run lasts 960 seconds, covering three five-minute retention
windows. It uses the ordinary concurrent API even at one worker. Production
settings remain unchanged: 1,024 queue slots per worker, up to 64 records per
apply batch, CRC32C records, RocksDB WAL enabled, and `sync=false`. Producer
threads pin to CPUs `0..workers-1`, the Mako writer pins to CPU 32, and RocksDB
helper threads inherit CPUs 0-39. These are distinct physical cores on zoo-002.
The launcher refuses to run with CPU boost enabled and checks it again after
each run. It records the governor but does not change it.

Build with the same required-native environment used by the other benchmarks:

```bash
cargo build --locked --release --manifest-path crates/Cargo.toml \
  -p mako-cache-bench --bin gc_soak
```

Provide an existing local output directory. The launcher creates a unique
child for every worker count, holds the shared zoo-002 benchmark lock, and
records machine details and the binary hash. It preserves failed databases;
successful scratch databases are deleted only after the verifier succeeds.
The reports and expected-state manifests remain available.

```bash
bash scripts/run_mako_cache_gc_soak.sh \
  /absolute/path/to/gc_soak /var/tmp/your-created-output-directory candidate
```

The default sweep is 1, 4, 8, 16, and 32 workers. Set
`MAKO_GC_SOAK_WORKERS=1` for one case or `MAKO_GC_SOAK_SECONDS=5` for a startup
smoke. `MAKO_GC_SOAK_RATE_PER_WORKER` optionally caps each producer's rate; zero
means unrestricted writes subject to normal queue backpressure. Rate-capped
samples are storage-capacity observations, not peak throughput measurements.

Every ten seconds, each `run.jsonl` reports ACK and applied counters, queue
occupancy, cache health including GC status, physical database file blocks,
free disk space, process block-write bytes, and cumulative CPU ticks grouped
by thread name. Concurrent RocksDB file removal is tolerated during disk
sampling. The launcher stops admission if the database exceeds 96 GiB or the
filesystem falls below 80 GiB free. An external timeout also bounds a stuck
backend call or recovery. A stopped or failed run is not an accepted sample.

The final `drained` event reports both ACK and fully-applied throughput, drain
time, conflicts, sampled p99 ACK latency, and the maximum observed queue
occupancy. Sample gaps vary from 128 to 383 commits so they cannot lock onto
the power-of-two apply batch size. Each sample includes normal capacity waits
and OCC retries. A fixed-size histogram reports the upper edge of the p99
bucket, with at most about 6.25% quantization error. It does not retain an
unbounded vector of latency samples. Queue occupancy is sampled and can miss
short peaks. The existing API does not expose individual queued-record age.
The `mako-writeback` CPU total includes replay and GC; it must not be labeled
GC-only CPU. GC status separately reports completed-attempt wall time,
including backend I/O waits. Process block-write bytes include WAL and compaction traffic,
and are not the logical batch byte count.

`verify.jsonl` records fresh-process reopen time separately from checking every expected
key. The new process uses the existing operating-system page cache. The cache
is unavailable to the verifier until its normal recovery
finishes. A successful run drains its complete acknowledged tail before close;
the measurement does not test recovery of the accepted volatile ACK tail.

Compare the pre-GC source and GC candidate with identical binary source,
queue limits, checksum policy, CPU placement, RocksDB version, native build,
and duration. Keep all samples, including discarded pilots, identifiable in
the report. Evaluate retained log bytes and disk-growth slopes after the
retention window, alongside ACK/apply throughput and reopen time. Time-based
GC does not promise a fixed SST byte ceiling because RocksDB controls
compaction. A shorter real-time soak plus a deterministic injected-clock
lifetime test must be reported as those two tests, not a longer physical soak.

The report reader emits JSON on stdout without modifying the raw run files:

```bash
python3 scripts/summarize_mako_cache_gc_soak.py /var/tmp/your-created-output-directory
```

It preserves every periodic sample and reports throughput and disk-growth
slopes after the first ten minutes. `drained_and_verified` records functional
completion only, including smoke tests. `comparative_accepted` additionally
requires a binary explicitly selected with `--approve-binary SHA256`, the
960-second default protocol, and no directory exclusion. No binaries are
approved by default. Repeat `--approve-binary` for the baseline and final
candidate, and use `--exclude PREFIX=REASON` to label diagnostics or discarded
pilots. `exclusion_reasons` explains every rejected comparison sample.
Acceptance also requires `completed.json`, emitted only after both subprocesses
exit successfully, the final boost check passes, and task scratch cleanup
succeeds. A `verified` event alone is insufficient because close runs afterward.
Historical runs can receive an explicitly retrospective marker only when their
successful launcher exit and final boost check have been independently observed.
