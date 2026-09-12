# Log GC sustained performance

Status: running on 2026-09-09. The one-worker baseline is accepted. The first
GC candidate exposed a collection bottleneck and is a diagnostic run. The
full sweep remains incomplete.
Functional validation is recorded separately in
[mako-cache-log-gc-validation.md](mako-cache-log-gc-validation.md).

## Measurement

This compares the pre-GC cache with the versioned-checkpoint, five-minute log
GC candidate. Each worker count runs for 960 seconds using the normal
concurrent API, default queue capacity of 1,024 records per worker, up to
64 records per apply batch, CRC32C records, and RocksDB WAL with `sync=false`.
The worker counts are 1, 4, 8, 16, and 32. Each worker repeatedly updates 256
disjoint keys with 128-byte values containing a counter and repeated padding.

The test records foreground ACK throughput and fully applied throughput,
sampled ACK p99, queue occupancy, physical disk growth, process block writes,
GC status and wall time, complete drain time, and fresh-process reopen time. The fresh
verifier checks every value and recovered transaction count, then commits
another update to check that progress advances. Only successfully drained and
verified pairs with a successful launcher completion marker enter the accepted
results. The marker covers subprocess exits, final boost check, and cleanup;
emitted `verified` events alone do not prove that the subsequent close succeeded.
Reopen runs in a new process and uses the existing operating-system page cache.

The test measures sustained backend throughput under bounded-queue
backpressure. Earlier million-transactions-per-second foreground benchmarks
used large queues and abandoned unapplied records. Their numbers measure a
different workload and must not be compared directly with these results.

ACK latency samples use variable gaps of 128-383 transactions to avoid
aliasing the 64-record apply batches. The p99 is the upper edge of a bounded
histogram bucket, with at most about 6.25% quantization error. Queue peaks are
sampled every ten seconds. Individual queued-record age is not exposed by the
current API. GC `total_duration` is completed-attempt wall time, including
backend I/O wait; the writer's CPU total includes both replay and GC.
Periodic counters are captured before disk and free-space inspection, but their
timestamp is captured afterward. This introduces monitor-delay skew in the
periodic throughput estimates. Overall ACK/applied timing is measured directly
and is unaffected. The shared workload source is unchanged between arms.

## Machine and builds

- Host: zoo-002, AMD EPYC 7702P, 64 physical cores and 128 logical CPUs.
- Kernel: Linux 6.8.0-137-generic.
- Workers: distinct physical CPUs 0 through `workers - 1`; Mako writeback:
  CPU 32; process and RocksDB helpers: CPUs 0-39.
- CPU boost: disabled before and after each run. Governor: `schedutil`,
  unchanged; configured frequency range: 1.5-2.0 GHz.
- Database storage: local `/var/tmp` on a TOSHIBA DT01ACA2 1.8 TiB rotational
  HDD. Throughput and compaction results reflect this HDD's capacity.
- RocksDB: zoo-002 system library 8.9.1; jemalloc: system `libjemalloc.so.2`.
- Rust: 1.97.1, optimized release, one codegen unit, LTO disabled,
  `panic=abort`.
- Native C++: clang 22.1.8, CMake `RelWithDebInfo`, actual ABI compile flags
  include `-O2 -g -DNDEBUG -march=native`, `STO_RMW=ON`, `OPACITY=OFF`,
  and `MAKO_LOCAL_TEST_HOOKS=OFF`.

Both arms use the same native archives and dynamic libraries. The native
fingerprint check ran normally and the Rust native wrappers were verified
byte-identical to the worktree. Builds used the verified native snapshot on
zoo-005 and linked against a private copy of zoo-002's RocksDB 8.9.1 library,
then ran on zoo-002 with its actual system library. The exact native yaml-cpp
library was copied beside both binaries. No fingerprint check was bypassed.

Source and binary SHA-256 identities:

| Item | SHA-256 |
| --- | --- |
| Shared benchmark source | `42e36f90b5798b3e0203c2ab00791f3903d0a7348d752c95cbdd19175d314536` |
| Pre-GC binary | `7fde4f32d93935c7ae36db7e1e6900aac141a8f2abf1f6e049543bd653d2a630` |
| Initial GC diagnostic binary | `4ed650f65029a13cb381522819c549cd67b49ac9b6e849a53c2492bd2384468d` |
| Streaming GC candidate binary | `efc89731fa945033afdcb304277eef513bf8022312660e3492c4eeb272b49f7d` |
| Pre-GC source archive | `3813d1f6f84c00333689e129d3bc6a38bb0c6ba3f35376b76740550055fdbcdc` |
| Initial GC diagnostic source archive | `193a2ffc4f0d5e670be9581592dd8a8ca79346d404938741091c38afc0724dca` |
| Streaming GC candidate source archive | `22623fa348503dea3db785967459fa8ae59d081c998eb0b4c76fed908f5d390f` |
| Native `libmako.a` | `d79c0e3a58fd384db43f82c88d9f969e4ba287191f2af769fbc24b2456e6b771` |
| Native build manifest | `88443645649acc7d79fad9a2c1c2156a8e12cfc7fb48270e3a9cfb47b0502ca8` |
| RocksDB 8.9.1 | `5e5c1b9f0adf5eef29131d8957089601b71ca9f2d9092831030bafc30e6861a6` |
| jemalloc library | `567efa52ecf445d5966025e1dcfd6b927a986472d272be3742f74a4a373baf12` |
| Native yaml-cpp library | `bac4ece41b05e51c68da36230773d4b09d4c14e8f66b0051e2d4f9e3e000f8b3` |

The pre-GC cache/core/RocksDB-adapter source is Git `58120af0`. The subsequent
backend-ownership fix changes API visibility and test support, with no hot-path
change, so this is the pre-GC reference. The GC candidate archive captures the
uncommitted implementation including the final recovery lane-owner checks and
the bounded-iterator GC replacement. The earlier diagnostic archive predates
the iterator change.
The native source/build is
`/var/tmp/mako-r1-e282a44b2-release.goXMD3/src` and `prod2/build` on zoo-005.

## Accepted results

### One worker

Pre-GC baseline, fully drained and verified:

- 52,078,551 transactions in 960.035 seconds of foreground work.
- ACK throughput: 54,246.5 transactions/s. Fully applied throughput:
  54,245.9 transactions/s.
- Applied throughput after 600 seconds: 42,463.0 transactions/s.
- Sampled ACK p99 upper bound: 0.655 ms from 203,847 samples.
- Largest observed queue: 1,024 records. Final drain: 10.992 ms.
- Physical database range after 600 seconds: 1.833-3.381 GiB. End-of-write
  size: 2.811 GiB. Process block writes: 38.97 GiB, or 803.4 bytes/transaction.
- New-process open: 529.679 seconds. All 256 values, the recovered count,
  and the next transaction passed verification.
- No cache health, backend, or record failures.

The baseline retains all 52.08 million application logs. Its throughput
declines as lifetime history and compaction work grow. The candidate result is
pending, so no GC throughput or restart improvement is claimed yet.

The final comparison will include throughput and physical disk range after
600 seconds, once two complete retention windows have elapsed. Retained
logical bytes and the physical SST/compaction envelope are separate measures.
This later interval is not proof of a permanent steady throughput. Expiration
lags admission by five minutes: the first collection window deletes the faster
pre-GC history, while the next deletes the slower history admitted during that
first collection window. The resulting delayed load can cause rate swings.
The primary comparison is overall fully applied throughput for each complete
960-second run. The measured disk envelope covers only that duration.
The [compact JSON report](mako-cache-log-gc-performance.json) retains precise
values and marks incomplete runs and smoke tests outside comparative acceptance.

## Reproduction and artifacts

### Initial GC bottleneck

The first real retention boundary exposed expensive per-log RocksDB point
reads inside the collection loop. Between 310.511 and 340.555 seconds,
applied throughput fell to about 1,479 transactions/s while GC reclaimed
663,503 logs. Those collections consumed about 29.79 seconds of wall time.
The cache remained healthy and reported no backend or GC errors.

A 20-second, 99 Hz profile of the writer captured 1,982 samples with no lost
samples. About 73.37% of sampled writer CPU was under `RocksBlobs::get`,
including RocksDB SST/memtable comparisons and repeated index seeks. This
supports replacing the point-read loop with a bounded iterator starting at
the lane's retained frontier. The atomic delete/metadata batch and retry
protocol do not need to change.

The run at `candidate-w1.OJ7j1z` and profile
`candidate-initial-gc.perf.data` are diagnostics, excluded from final
comparative acceptance. No final GC throughput or restart claim is made from
that run.

This diagnostic completed all 960 seconds, drained 32,549,520 transactions,
and verified all final values and progress in a fresh process. Its reopen
took 107.156 seconds. The writer had reclaimed 24,115,727 logs and retained
8,433,793, with no reported GC errors. A bounded-iterator replacement is
being measured separately; the profile and superseded implementation exclude
this diagnostic from the accepted comparison.

The bounded iterator removes repeated point seeks, but GC still decodes each
expired record with CRC and shape checks, then writes delete markers and one
lane checkpoint per batch of up to 1,024 records. It shares the writer with
replay, alternating one GC batch with an apply opportunity, not equal CPU-time
shares. RocksDB still processes deletion WAL, memtable, and compaction work.
There is no foreground fsync, but bounded-queue backpressure makes that
background work visible in sustained ACK throughput.

### Run files

The benchmark source is
[gc_soak.rs](../../crates/mako-cache-bench/src/bin/gc_soak.rs). Its
[protocol guide](../../crates/mako-cache-bench/gc-soak.md) describes flags,
sampling precision, disk limits, cleanup, and the report reader.

Zoo-002 output root:
`/var/tmp/mako-gc-soak-20260909.CbySGi`.
Each generated run directory retains `machine.txt`, `run.jsonl`,
`verify.jsonl`, stderr, and its expected-state manifest. The launcher removes
only its completed scratch RocksDB after verification. Failed or discarded
pilots retain their databases. Disk checks stop a sample above 96 GiB of
database files or below 80 GiB filesystem free space.

The five-second `smoke` and interrupted `baseline` pilot used fixed-stride
latency sampling. They are excluded from comparative acceptance because that
stride could alias apply batching. The corrected arm is `baseline-jitter`.

The comparison source archives are `baseline-source.tar.gz` and
`stream-source.tar.gz`, beside the native fingerprint manifest in the
output root. Local frozen source/build roots on zoo-005 are
`/var/tmp/mako-gc-soak-baseline.yD9EQy` and
`/var/tmp/mako-gc-soak-stream.vB2Hke`.
The misleadingly named `candidate-source-final.tar.gz` is preserved only as
the initial diagnostic source, not the final comparison candidate.

The baseline W1 and initial diagnostic W1 predate completion-marker support.
Their markers explicitly record retrospective evidence from successful launcher
exit and final boost check. Initial smoke tests remain excluded regardless of
their verification events. The first streaming W1 was already running when the
marker was introduced; it also requires observed launcher success before any
retrospective marker can be attached.

```bash
env LD_LIBRARY_PATH=/var/tmp/mako-gc-soak-20260909.CbySGi:/home/users/shuai/.linuxbrew/opt/llvm@22/lib \
  bash scripts/run_mako_cache_gc_soak.sh \
  /var/tmp/mako-gc-soak-20260909.CbySGi/gc-soak-stream \
  /var/tmp/mako-gc-soak-20260909.CbySGi stream
```

The real-time soak spans 16 minutes per sample. The separate injected-clock
test covers 50 logical minutes at a five-minute retention setting and checks
that retained history plateaus. These are two different checks; no 150-minute
physical soak is claimed.
