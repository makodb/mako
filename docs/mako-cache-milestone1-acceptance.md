# Mako cache Milestone 1 acceptance

Status: **CURRENT DETACHED-WRITEBACK PASS** on 2026-08-30. The latest code and
performance evidence is for candidate `153e14c78`. The earlier native-record
rewrite and the full comparative acceptance remain below as historical
evidence; newer measurements do not retroactively alter those artifacts.

Milestone 1 is complete for its declared scope: one process, one recovered
cache namespace, C++ STO/MassTrans transactions behind the public revision-0 C
ABI plus its fingerprint-checked build-private fast extension, the safe Rust
transaction/cache layer, and asynchronous atomic RocksDB application. Phase 1G
eviction and all distributed work remain deferred.

## Current detached holder fast path

Candidate `153e14c78fc1a1ea6efa68713b5bda8b87d6ce44` moves the
checksum-none, single-producer, one-Put acknowledgement path off record
construction and RocksDB replay:

1. Rust passes the unique producer's persistent SPSC control and a
   capacity-limit snapshot to the fused C++ terminal. It retains the stable
   local next-sequence cursor for cold-result decoding.
2. C++ checks capacity, selects the next dense generation and its masked
   holder, acquires the STO write locks, allocates the Mako timestamp, performs
   final read and predicate validation, installs and cleans up the transaction,
   transfers the staged `std::string` value into that holder, and publishes the
   acknowledgement witness.
3. The foreground returns after the dense acknowledgement prefix reaches the
   transaction. It does not encode a commit record or call RocksDB.
4. The sole serialized consumer, normally the named `mako-writeback` OS thread,
   reads the holder, encodes the record, applies the transaction and log entry
   in one RocksDB `WriteBatch`, releases the holder generation, and advances
   the applied watermark. `wait_applied()` and shutdown can help execute that
   same serialized drain. The normal thread can be pinned to a CPU outside the
   foreground affinity set.

The production default remains checksummed v3 records. The explicit
`RecordChecksum::None` option emits a distinct, self-describing v4 format and
skips the foreground CRC scan. Recovery accepts mixed v3/v4 history. V4 still
performs structural validation but cannot detect arbitrary payload corruption,
so disabling CRC is an intentional durability tradeoff rather than a silent
downgrade.

### Matched one-worker hot-path result

The controlled one-worker `zoo-002` run used CPU 0, writeback CPU 16, checksum
`none`, a 1,048,576-entry queue, 1,048,576 warmup transactions, and 262,144
measured transactions. Values are three-run medians from exact perf intervals:

| Path | Cycles/txn | Instructions/txn | Branches/txn | Cycle-normalized throughput |
| --- | ---: | ---: | ---: | ---: |
| Raw STO/Masstree C ABI | 1,022.488 | 2,563.285 | 482.943 | 100.00% |
| Fused C++ holder terminal | 1,051.967 | 2,642.406 | 488.970 | 97.20% |
| Full Rust cache acknowledgement gate | 1,077.375 | 2,673.446 | 495.175 | 94.91% |

The full cache gate is therefore within the requested roughly 95% of raw C ABI
throughput by cycles. It retains 97.64% of the fused native terminal. Wall-time
samples were frequency-sensitive, so cycles, instructions, and branches are
the primary comparison rather than tuning to a fraction of one percent. The
source also records PGO as future work; this result does not depend on PGO.

### Final concurrent scaling run

The retained
[final scaling report](benchmarks/mako-cache-scaling-zoo002-20260830-detached-holder.json)
has SHA-256
`98ac3e5a4b3781c76dc31e076179907849aa3977e44737ce29e4fff8930707ff`.
It records exact Git HEAD `153e14c78`, a clean worktree, Rust 1.95.0, native
fingerprint
`e1a0e042b0ebf3a493a729f4dde29b91282cc2e5d1141500894e0e2bb601f6b3`,
foreground CPUs 0-31, writeback CPU 32, and checksum `none`. All 84 unique
sample/recovery pairs and an independent accounting audit passed.

This matrix deliberately uses `ForegroundMode::Concurrent` for every row so
one binary can cover 1 through 32 workers. Its W1 row therefore does not use
the exclusive single-producer fast path measured above. Throughput is median
thousands of transactions per second across seven repetitions. `Applied`
includes the immediate asynchronous drain. Foreground CPU throughput divides
commits by summed workload-thread CPU time and excludes the writeback thread;
it is a diagnostic, not aggregate wall throughput.

| Workers | Read ACK | Read applied | Write ACK | Write applied | Write foreground CPU |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 759.3 | 759.3 | 640.0 | 93.2 | 643.9 |
| 2 | 1,256.2 | 1,256.1 | 625.4 | 134.5 | 324.8 |
| 4 | 1,668.3 | 1,668.2 | 706.3 | 145.8 | 193.4 |
| 8 | 1,390.1 | 1,390.1 | 698.5 | 141.1 | 115.5 |
| 16 | 1,631.7 | 1,631.7 | 747.8 | 136.1 | 82.4 |
| 32 | 1,858.1 | 1,858.1 | 255.8 | 104.7 | 9.7 |

Against the retained pre-rewrite run, write throughput changed as follows:

| Workers | Old ACK | Final ACK | Gain | Old applied | Final applied | Gain |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 119.9 | 640.0 | 5.337x | 44.8 | 93.2 | 2.082x |
| 2 | 106.6 | 625.4 | 5.869x | 46.7 | 134.5 | 2.882x |
| 4 | 35.5 | 706.3 | 19.885x | 24.2 | 145.8 | 6.019x |
| 8 | 20.1 | 698.5 | 34.760x | 15.8 | 141.1 | 8.942x |
| 16 | 11.1 | 747.8 | 67.544x | 9.7 | 136.1 | 14.094x |
| 32 | 7.2 | 255.8 | 35.504x | 6.6 | 104.7 | 15.884x |

Read ACK changed between -1.48% and +7.36%, consistent with a write-specific
optimization. W32 write ACK still drops from W16 and remains the next
foreground contention target. The dedicated writer and foreground CPU metric
make that limitation visible without charging background replay CPU to a
workload worker.

Current functional evidence includes the 119/119 native `mako-cache` tests,
13/13 fake-ABI tests under pinned Miri, 17/17 benchmark tests, the fresh
hooks-off C++ suite with 75 passes and one expected hook-only skip, and 27/27
supporting `mrx-ffi`, `mrx-masstree`, `mrx`, and `mtree-sys` tests. Two
independent holder hot-path unsafe-code reviews reported no remaining
actionable finding.

## Previous native-record and bounded-batching validation

The 2026-08-29 rewrite keeps STO/MassTrans/Masstree in C++, but moves commit-
record construction to STO's canonical write set. Rust preallocates one exact-
size buffer. After the complete write set is locked, a short per-database ticket
turn orders `MakoTimestamp` allocation, final validation, and dense `CacheSeq`
binding. Native code then retires that turn, serializes and checksums directly
into the buffer while retaining the write locks, and installs the transaction.
Rust attaches the witnessed bytes in constant time and acknowledges only across
a dense Ready prefix.

The background path validates and materializes records, then applies a
contiguous prefix in one atomic RocksDB `WriteBatch`, bounded by 64 records and
1 MiB of encoded record bytes by default. Transaction boundaries and order are
preserved inside that physical batch. `wait_applied()` never processes beyond
the acknowledgement snapshot it captured. A permanent structural record error
latches the earliest failing sequence and fail-stops later work; allocation and
backend failures remain retryable. Neither acknowledgement nor the applied
watermark claims disk synchronization.

The frozen production snapshot had source-tree digest
`191d0ef64732ae67fb16d2d944c5f48ffa7afd9e5f5bc18a48346d13ee91fb80`
at Git HEAD `c4fe90fb418618f751771fc1f854618ba001cde4`. The source-drift guard
matched before and after every fresh build and after the benchmark. The final
patch adds only validation artifacts, documentation, and test-only mutation
cleanup after that run; no measured production source changed. The fresh
hooks-off native fingerprint was
`a7b05a47436b86b764c7b3f8078f986d4125e5bdf6c2e803a9cd54e11b95fb55`.

Current functional evidence includes the 66/66 hook-enabled native ABI suite,
the 56/56 fresh hooks-off zoo-2 ABI suite, the complete required-native
`mako-local` suite, all 96 `mako-cache` tests, 38/38 focused writeback tests,
100/100 point and 100/100 predicate ordering runs, 13/13 Miri fake-ABI tests,
and a [12/12 isolated mutation report](benchmarks/mako-cache-mutations-20260829-final.json)
(SHA-256 `c6d791e0e476d6a3d1396486700fc7be533e9f7e45ed3613889af55cb4bb1d69`)
with zero survivors or harness errors. Strict Clippy, package-scoped formatting,
symbol, fingerprint, crash, history, recovery, and independent implementation-
audit gates are also clean.

### Focused before/after scaling run

The retained [pre-rewrite report](benchmarks/mako-cache-scaling-zoo002-20260828-current-v1.json)
(SHA-256 `0d3bbb0b40a30e993ea10de6dde396c5538a1def6e15c7a9a039fb63d5c27fcc`)
and [native-record rewrite report](benchmarks/mako-cache-scaling-zoo002-20260829-frozen-thin-log.json)
(SHA-256 `25237bdcf5b39bd17bc7c1664545c0cde5f6024463e31063a491d356df3e24c3`)
record the same `zoo-002` host, Git HEAD, Rust toolchain, scaling profile, CPU
set 0-31, transaction size, and seven-repetition protocol. Both reports mark
their worktrees dirty, and the older run did not retain a whole-tree digest;
therefore this is a controlled comparison of the two archived executions, not
a claim that the exact old dirty source tree is reconstructible from the
repository. Native manifests inspected during validation established matching
CMake 4.3.4, Clang 22.1.8, Release, `-march=native`, `STO_RMW=ON`,
`OPACITY=OFF`, and hooks-off settings. The JSON report-time load averages were
`1.61 1.64 1.93` before the rewrite and `7.40 5.78 4.00` after it; the latter
was sampled after the matrix and includes its decaying self-load. The rewrite
report contains exactly 84 unique, validated samples: read and write at 1, 2,
4, 8, 16, and 32 workers. All expected commits, recovery checksums, record/key
counts, ACK-plus-drain arithmetic, and independently recomputed medians match.

Throughput below is thousands of transactions per second; ratios are
rewrite/pre-rewrite. Read applied throughput is included even though it is
effectively identical to ACK for this workload.

| Workload | Workers | Old ACK | Rewrite ACK | Ratio | Old applied | Rewrite applied | Ratio |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| read | 1 | 725.1 | 685.1 | 0.945x | 725.1 | 685.1 | 0.945x |
| read | 2 | 1,180.4 | 1,159.5 | 0.982x | 1,180.3 | 1,159.4 | 0.982x |
| read | 4 | 1,682.5 | 1,653.5 | 0.983x | 1,682.4 | 1,653.4 | 0.983x |
| read | 8 | 1,411.0 | 1,445.0 | 1.024x | 1,410.9 | 1,445.0 | 1.024x |
| read | 16 | 1,519.9 | 1,491.1 | 0.981x | 1,519.9 | 1,491.1 | 0.981x |
| read | 32 | 1,769.9 | 1,712.7 | 0.968x | 1,769.9 | 1,712.7 | 0.968x |
| write | 1 | 119.9 | 384.9 | 3.210x | 44.8 | 87.3 | 1.950x |
| write | 2 | 106.6 | 507.0 | 4.758x | 46.7 | 104.6 | 2.241x |
| write | 4 | 35.5 | 438.2 | 12.339x | 24.2 | 135.2 | 5.583x |
| write | 8 | 20.1 | 386.6 | 19.236x | 15.8 | 132.0 | 8.367x |
| write | 16 | 11.1 | 444.6 | 40.155x | 9.7 | 126.3 | 13.081x |
| write | 32 | 7.2 | 57.3 | 7.951x | 6.6 | 56.9 | 8.634x |

The rewrite is write-path-specific: read ACK changes range from -5.5% to
+2.4%. Write ACK improves 3.21x at one worker and 40.16x at 16 workers, while
applied throughput improves 1.95x and 13.08x at those endpoints. W32 still
drops sharply in absolute throughput from W16, despite remaining about 8x over
the old implementation. Its ACK and applied rates converge and drain time is
only 6-31 ms, pointing toward a high-concurrency foreground/native/dense-ACK
bottleneck rather than a RocksDB backlog. W4-W16 ACK samples also have roughly
9-14% coefficient of variation. Both limitations remain explicit profiling
work, not hidden by the improvement.

## Historical candidate and retained evidence

For historical candidate `6574cf47c`, PASS means that the functional and
contract gates are green and that the
complete comparative measurement ran under a fully recorded, resumable
protocol with its correctness and accounting invariants independently checked.
No performance SLA was declared before the run, so PASS does not mean that
Mako beat either baseline. The run exposed the serious concurrent-write
scaling cost that drove the native-record rewrite; the rewrite's W32 collapse
remains the next profiling target.

- Implementation candidate:
  `6574cf47c3233f208d5b2e68790e411c2ea3debe`
  (`mako-cache: complete milestone 1 contract`). The benchmark recorded a
  clean candidate worktree.
- Machine-readable report:
  [mako-cache-milestone1-zoo-002.json](benchmarks/mako-cache-milestone1-zoo-002.json),
  1,516,365 bytes,
  SHA-256 `b0298c614fad1bcb8cafd5df60a61ea500c8e95b616d5646b97baa5c843111e0`.
- Native build fingerprint:
  `fbe93635a0bf87e53233da237e7769305f07a1eed51808710aeced31bbb9bcfd`.
- Benchmark executable: FNV-1a64 `0d207a0798d214ee`; SHA-256
  `fc7ab869757fada9aecb1da626be3a37ddbf76a04e864dc3d5264caf1ed82257`.
- Host: `zoo-002`, Linux `6.8.0-137-generic`, AMD EPYC 7702P (64 physical,
  128 logical CPUs). The run was pinned to physical cores 0-15; the recorded
  load average was `2.07 2.34 2.55`.
- Toolchains: CMake 3.31.6, Homebrew Clang/libc++ 21.1.8, and Rust 1.97.1.
  The native tree was Release, `STO_RMW=ON`, `OPACITY=OFF`, and
  `MAKO_LOCAL_TEST_HOOKS=OFF`.

The exact measurement invocation, from the repository root on `zoo-002`, was:

```bash
taskset -c 0-15 env \
  MAKO_BUILD_DIR="$PWD/build_milestone1_zoo2_final" \
  MAKO_LOCAL_REQUIRE_NATIVE=1 \
  CARGO_TARGET_DIR="$PWD/build_milestone1_zoo2_final/cargo-target-benchmark" \
  "$PWD/build_milestone1_zoo2_final/cargo-target-benchmark/release/mako-cache-bench" run \
  --profile acceptance \
  --data-root /tmp \
  --output "$PWD/docs/benchmarks/mako-cache-milestone1-zoo-002.json" \
  --checkpoint "$PWD/build_milestone1_zoo2_final/mako-m1-zoo2.acceptance.checkpoint"
```

The run used local `/tmp` database directories rather than the NFS-backed
source tree. It completed all 1,260 sample/recovery pairs and exited zero. Its
synced checkpoint contains the matching identity header and all 1,260 result
records, so an interruption would have resumed only missing pairs.

## Matrix and measurement contract

The acceptance profile contains 180 configurations and seven repetitions of
each configuration:

- arms: transactional `mako-cache`, the existing `mrx` point cache, and raw
  RocksDB through `mrx-rocks`;
- workloads: read, write, and read/modify/write;
- transaction sizes: 1, 4, 16, and 64;
- workers: 1, 4, and 16;
- low contention everywhere, plus high contention for the multiworker rows.

Each sample and recovery observation runs in fresh child processes. Target
commits per worker are `max(256, 8192 / transaction_size)` and warmup commits
are `max(64, 2048 / transaction_size)`. The asynchronous arms share a
`2^18`-mutation capacity budget: MRX counts mutation tickets, while Mako counts
`ceil(2^18 / transaction_size)` whole-transaction records. Seed and warmup are
drained before timing.

`ack` ends when foreground calls return. `applied` adds the immediate
post-interval writeback barrier; that barrier is not a per-transaction
applied-latency percentile. RocksDB uses WAL with `sync=false`, and no WAL sync
is requested. The harness issues no explicit memtable flush during timing;
automatic RocksDB background flush/compaction was neither disabled nor
instrumented and is part of this default-profile measurement. This qualifies
the finalized JSON's shorthand rather than altering the hashed artifact.
Recovery is warm-cache open and open-plus-validation after the harness's
uniform explicit post-timing flush.

Only size-one read/write rows have a common point-operation contract. Mako is
the OCC reference for transactional rows. MRX multi-key/RMW rows are labeled
`weaker_nonatomic_no_occ_baseline`; raw RocksDB is labeled
`weaker_atomic_batch_no_occ_baseline` for writes and
`weaker_nonsnapshot_no_occ_baseline` for RMW. Those weaker rows provide
context, not equivalent-transaction speedups.

## Independent invariant gate

The finalized JSON passed two independent runs of a separate
schema-and-arithmetic validator. It checked all 1,260 unique sample coordinates
and all 180 unique seven-sample summaries, rejected duplicate JSON keys, and
recomputed every published median. It also checked, per sample:

- target, warmup, commit, keyspace, and queue-capacity formulas;
- `ack + drain = applied`, positive and ordered latency/recovery percentiles,
  and every derived rate and amplification value;
- mutation bytes, RocksDB logical/allocated bytes, Mako commit-record counts,
  backend key counts, and the absence of commit-log bytes in both baselines;
- zero baseline conflicts and positive aggregate Mako conflicts in every
  configured high-contention write/RMW group;
- exact read/write checksums and exact serializable RMW checksums for Mako;
  weaker high-contention RMW checksums were required only to remain within the
  valid lossy-update bound.

The implementation candidate also passed the fresh Release native profile:
CTest `MakoLocal` 7/7, the complete required-native `mako-local` and
`mako-cache` suites (including 57 cache unit tests and the crash matrices), the
four integrated Milestone 1 overload/shutdown/exhaustion tests, 13/13 Miri
fake-ABI tests, 7/7 doctests, and 13/13 benchmark tests. Focused strict Clippy
was green. The broader pre-existing all-target Clippy command still reports an
unrelated Rust 1.97 lint at `mako-local/tests/overhead.rs:386`.

## Representative common point-contract results

All values below are medians of seven repetitions. Throughput is millions of
transactions per second, latency is retry-inclusive, recovery is warm-cache,
and `log amp` is unreclaimed commit-record key/value bytes divided by all
seeded, warmup, and measured user-mutation bytes.

Mako's read-only log amplification comes from the seeded commit records; the
timed reads themselves do not generate commit records.

| Scenario | Arm | ACK Mtxn/s | Applied Mtxn/s | Abort % | p50 us | p99 us | Open ms | Open + validate ms | Log amp |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| read / low / W1 | Mako | 1.253 | 1.253 | 0 | 0.640 | 1.510 | 133.655 | 133.984 | 1.131 |
| read / low / W1 | MRX | 2.008 | 2.008 | 0 | 0.290 | 0.790 | 147.802 | 149.438 | 0 |
| read / low / W1 | Rocks | 0.328 | 0.328 | 0 | 2.800 | 4.780 | 147.959 | 149.599 | 0 |
| read / low / W16 | Mako | 1.633 | 1.633 | 0 | 6.000 | 45.472 | 189.060 | 193.501 | 1.131 |
| read / low / W16 | MRX | 7.633 | 7.633 | 0 | 0.330 | 0.810 | 157.711 | 184.893 | 0 |
| read / low / W16 | Rocks | 3.168 | 3.168 | 0 | 2.070 | 5.750 | 131.632 | 158.090 | 0 |
| write / low / W1 | Mako | 0.117 | 0.046 | 0 | 7.970 | 19.091 | 239.236 | 239.412 | 1.498 |
| write / low / W1 | MRX | 0.892 | 0.737 | 0 | 0.830 | 1.610 | 163.578 | 165.279 | 0 |
| write / low / W1 | Rocks | 0.119 | 0.119 | 0 | 6.800 | 19.751 | 135.421 | 137.059 | 0 |
| write / low / W16 | Mako | 0.011 | 0.010 | 0 | 106.465 | 17,941.967 | 1,668.665 | 1,671.130 | 1.498 |
| write / low / W16 | MRX | 5.162 | 3.680 | 0 | 0.940 | 2.070 | 165.351 | 192.232 | 0 |
| write / low / W16 | Rocks | 0.211 | 0.211 | 0 | 80.053 | 145.606 | 148.500 | 174.745 | 0 |
| write / high / W16 | Mako | 0.013 | 0.011 | 58.006 | 92.495 | 19,253.048 | 1,537.006 | 1,537.016 | 1.507 |
| write / high / W16 | MRX | 1.617 | 1.617 | 0 | 1.650 | 118.705 | 140.587 | 140.610 | 0 |
| write / high / W16 | Rocks | 0.202 | 0.202 | 0 | 77.884 | 128.776 | 127.908 | 127.978 | 0 |

Because these rows share a point contract, ratios are meaningful here:

| Scenario | Mako/MRX ACK | Mako/Rocks ACK | Mako/MRX applied | Mako/Rocks applied |
| --- | ---: | ---: | ---: | ---: |
| read / low / W1 | 0.6240x | 3.8165x | 0.6239x | 3.8162x |
| read / low / W16 | 0.2140x | 0.5155x | 0.2140x | 0.5155x |
| write / low / W1 | 0.1307x | 0.9805x | 0.0630x | 0.3909x |
| write / low / W16 | 0.0021x | 0.0522x | 0.0026x | 0.0456x |
| write / high / W16 | 0.0079x | 0.0634x | 0.0070x | 0.0563x |

The important historical performance result is the collapse in Mako's concurrent point
write throughput, even without conflicts. The W16 result is stable across
repetitions: ACK maximum/minimum is 1.022x and p99 spans 17.42-19.39 ms, so it
is not one noisy sample. That diagnosis applies only to the historical
candidate. The rewrite replaces the linear pending-record lookup with dense
queue-token indexing, constructs records directly in native STO, and batches
contiguous records. Current scaling results are reported above; these
historical W16 values must not be read as current performance.

## Representative transactional context (semantically non-equivalent baselines)

These rows must not be interpreted as speedups. `OCC` means
`transactional_occ_reference`, `non-atomic` means
`weaker_nonatomic_no_occ_baseline`, `atomic batch` means
`weaker_atomic_batch_no_occ_baseline`, and `nonsnapshot` means
`weaker_nonsnapshot_no_occ_baseline`.

| Scenario | Arm | Semantics | ACK Mtxn/s | Applied Mtxn/s | Abort % | p99 us | Open + validate ms | Log amp | Median checksum / strong |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| write / low / T16 / W16 | Mako | OCC | 0.131 | 0.013 | 0 | 371.677 | 610.107 | 1.148 | 4,096 / 4,096 |
| write / low / T16 / W16 | MRX | non-atomic | 0.452 | 0.211 | 0 | 32.061 | 185.997 | 0 | 4,096 / 4,096 |
| write / low / T16 / W16 | Rocks | atomic batch | 0.029 | 0.029 | 0 | 780.256 | 163.085 | 0 | 4,096 / 4,096 |
| RMW / low / T16 / W16 | Mako | OCC | 0.142 | 0.013 | 0 | 281.473 | 603.438 | 1.148 | 163,840 / 163,840 |
| RMW / low / T16 / W16 | MRX | non-atomic | 0.379 | 0.209 | 0 | 37.432 | 184.795 | 0 | 163,840 / 163,840 |
| RMW / low / T16 / W16 | Rocks | nonsnapshot | 0.028 | 0.028 | 0 | 821.288 | 170.936 | 0 | 163,840 / 163,840 |
| RMW / high / T16 / W16 | Mako | OCC | 0.033 | 0.013 | 96.085 | 2,069.147 | 562.359 | 1.149 | 163,840 / 163,840 |
| RMW / high / T16 / W16 | MRX | non-atomic | 0.243 | 0.243 | 0 | 487.013 | 156.389 | 0 | 18,336 / 163,840 |
| RMW / high / T16 / W16 | Rocks | nonsnapshot | 0.027 | 0.027 | 0 | 1,183.845 | 146.401 | 0 | 10,311 / 163,840 |

The high-contention RMW checksum is the semantic distinction in concrete
form: Mako preserves all 163,840 increments through OCC retries, while both
weaker baselines lose updates. Their throughput numbers are therefore context,
not measurements of an equivalent transaction implementation.

## Acceptance conclusion and deferred scope

The functional gates and the final comparative evidence gate are complete, so
Milestone 1 is accepted. The historical concurrent-write collapse was largely
removed by the native-record rewrite. The new W32 foreground drop is carried
forward as a narrower profiling target; changing it does not require weakening
the transaction or writeback contract established here.

This acceptance deliberately does not claim:

- cross-host reproducibility beyond this seven-repetition `zoo-002` run;
- cold-cache or WAL-replay recovery;
- durable ACK or durable applied state (`sync=false` remains intentional);
- per-transaction applied latency from the phase-level drain measurement;
- bounded resident values or reclaimed commit-record history (Phase 1G);
- more than one recovered cache namespace in a process; or
- distributed routing, sharding, 2PC, replication, or failure recovery.
