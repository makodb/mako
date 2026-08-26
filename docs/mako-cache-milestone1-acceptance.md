# Mako cache Milestone 1 acceptance

Status: **PASS** on 2026-08-26.

Milestone 1 is complete for its declared scope: one process, one recovered
cache namespace, C++ STO/MassTrans transactions behind the revision-0 C ABI,
the safe Rust transaction/cache layer, and asynchronous atomic RocksDB
application. Phase 1G eviction and all distributed work remain deferred.

Here, PASS means that the functional and contract gates are green and that the
complete comparative measurement ran under a fully recorded, resumable
protocol with its correctness and accounting invariants independently checked.
No performance SLA was declared before the run, so PASS does not mean that
Mako beat either baseline. The run in fact exposes a serious concurrent-write
scaling cost that should drive the next optimization work.

## Candidate and retained evidence

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

The important performance result is the collapse in Mako's concurrent point
write throughput, even without conflicts. The W16 result is stable across
repetitions: ACK maximum/minimum is 1.022x and p99 spans 17.42-19.39 ms, so it
is not one noisy sample. A plausible first profiling target is
`Writeback::resolve`'s linear pending-record search while holding the state
mutex, but that is a source-level hypothesis, not a proven diagnosis. This is
not an acceptance-artifact failure: all commits and checksums are correct, the
report has complete provenance, and no throughput threshold was predeclared.
It is a concrete follow-up priority, not evidence hidden behind the Milestone
1 PASS.

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
Milestone 1 is accepted. The measured concurrent-write scaling problem is
carried forward as optimization work; changing it does not require weakening
the transaction or writeback contract established here.

This acceptance deliberately does not claim:

- cross-host reproducibility beyond this seven-repetition `zoo-002` run;
- cold-cache or WAL-replay recovery;
- durable ACK or durable applied state (`sync=false` remains intentional);
- per-transaction applied latency from the phase-level drain measurement;
- bounded resident values or reclaimed commit-record history (Phase 1G);
- more than one recovered cache namespace in a process; or
- distributed routing, sharding, 2PC, replication, or failure recovery.
