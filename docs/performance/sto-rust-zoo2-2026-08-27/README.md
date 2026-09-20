# Rust STO/Masstree comparison on `zoo-002`

Date: 2026-08-27<br>
Source: `codex/sto-rust` at `214724b2ae01c416774b7c2399bcee1626561a21`

## Result

The current Rust implementation is a correctness baseline, not yet a
performance-competitive replacement for C++ STO. C++ won every measured cell.
At one thread it was 7.0–7.4x faster. The Rust engine reached its peak at four
threads, then lost 60–65% of that peak by 64 threads; C++ continued scaling to
64 physical cores. The largest observed gap was 238.1x in the 64-thread
read-only workload.

This is a comparison of the current integrations, not an intrinsic
Rust-versus-C++ result. The Rust route includes a deliberately defensive,
globally synchronized C ABI and Rust record registry; the C++ route calls its
Masstree directly.

Values below are medians of three five-second samples. `Mops/s` counts logical
operations in committed transactions; every transaction has ten unique-key
operations. `C++/Rust` is the median of the three same-seed, per-repetition
throughput ratios.

### Ten reads per transaction

| Threads | C++ Mops/s | Rust Mops/s | C++/Rust | C++ aborts | Rust aborts |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 6.080 | 0.863 | 7.036x | 0.00% | 0.00% |
| 2 | 12.063 | 1.573 | 7.669x | 0.00% | 0.00% |
| 4 | 24.106 | 2.609 | 9.195x | 0.00% | 0.00% |
| 8 | 46.731 | 1.575 | 29.677x | 0.00% | 0.00% |
| 16 | 91.523 | 1.190 | 76.907x | 0.00% | 0.00% |
| 32 | 172.461 | 1.030 | 165.984x | 0.00% | 0.00% |
| 64 | 220.360 | 0.925 | 238.113x | 0.00% | 0.00% |

### Ten operations, 5% writes

Every write operation first reads the current value and then stages
`value.wrapping_add(1)`.

| Threads | C++ Mops/s | Rust Mops/s | C++/Rust | C++ aborts | Rust aborts |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 5.662 | 0.765 | 7.400x | 0.00% | 0.00% |
| 2 | 11.303 | 1.378 | 8.212x | 0.003% | 0.002% |
| 4 | 22.495 | 2.241 | 10.029x | 0.009% | 10.255% |
| 8 | 41.899 | 1.517 | 27.577x | 3.394% | 3.100% |
| 16 | 77.500 | 1.191 | 65.642x | 4.630% | 0.974% |
| 32 | 120.629 | 1.024 | 117.292x | 19.156% | 0.768% |
| 64 | 143.231 | 0.886 | 161.445x | 59.930% | 1.896% |

### Ten operations, 50% writes

| Threads | C++ Mops/s | Rust Mops/s | C++/Rust | C++ aborts | Rust aborts |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 3.967 | 0.546 | 7.300x | 0.00% | 0.00% |
| 2 | 7.900 | 0.972 | 8.140x | 0.026% | 0.028% |
| 4 | 15.761 | 1.631 | 9.707x | 0.081% | 26.458% |
| 8 | 26.721 | 1.242 | 21.519x | 13.213% | 7.607% |
| 16 | 48.757 | 0.929 | 52.469x | 17.834% | 1.619% |
| 32 | 80.396 | 0.760 | 105.168x | 31.450% | 2.145% |
| 64 | 98.527 | 0.593 | 166.299x | 65.460% | 5.870% |

The C++ 64-thread speedups over one thread were 36.24x, 25.30x, and 24.84x
for read-only, 5%-write, and 50%-write respectively. Rust peaked at four
threads at roughly 3x its one-thread rate. Its 64-thread rates were only
35.5%, 39.5%, and 36.3% of those four-thread peaks.

The low Rust abort rates at high thread counts do not indicate better conflict
handling: the Rust engine is processing far fewer concurrent attempts. Its
repeatable abort spike at exactly four threads also merits investigation.

## What the profile says

A follow-up `perf` profile on the same `zoo-002` binaries and the read-only
workload demonstrates that the post-four-thread collapse is global-lock
contention, not Masstree traversal. The profiler was attached only after all
workers existed, so preload is not included. A one-second, 32-thread profile
collected about 16,000 samples: 73.11% landed in the kernel's
`native_queued_spin_lock_slowpath`, split between futex wake (38.68%) and wait
(34.40%) paths. Actual Masstree `reach_leaf` traversal was only 0.57% self.

The corresponding one-second `perf stat` windows show the collapse directly:

| Engine/threads | Task clock | Instructions | IPC | Context switches | Migrations | Diagnostic throughput |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Rust, 4 | 3.97 s | 15.00 B | 1.15 | 5,816 | 3 | 2.610 Mops/s |
| Rust, 64 | 45.53 s | 19.01 B | 0.13 | 444,836 | 7,928 | 0.914 Mops/s |
| C++, 64 | 63.30 s | 197.7 B | 1.07 | 467 | 4 | 230.13 Mops/s |

At 64 threads the Rust/C-ABI path used 11.5x the CPU time of its four-thread
run to retire only 1.27x the instructions at 35% of the throughput. It also
made about 952x as many context switches as the 64-thread direct C++ path.
These are separate diagnostic runs, not substitutions for the matrix above.

The sampled stacks and source identify three global synchronization points in
each Rust-side point-access path:

1. The C ABI validates a tree and worker by locking the process-wide runtime
   mutex twice per operation through
   [`checked_tree` and `checked_thread`](../../../src/mako/storage/mtree_abi.cc#L214).
   `checked_thread` also linearly scans the attached-worker vector while
   holding that mutex.
2. [`mt_get`](../../../src/mako/storage/mtree_abi.cc#L909) takes the tree-wide
   `std::shared_mutex` even for a point read.
3. Rust STO calls `RegisteredResource::validate_binding` from
   [`with_item_inner`](../../../crates/sto-core/src/transaction.rs#L220); its
   [implementation](../../../crates/sto-core/src/runtime.rs#L487) takes the
   object-wide class-map mutex for every logical operation.

Thus each key read in the ten-key transaction crosses two exclusive C-ABI
runtime-mutex sections, one C++ shared-mutex section, and one exclusive Rust
binding-mutex section.

Code inspection shows another shared read-side lock in the Rust
[`Registry::resolve`](../../../crates/sto-masstree/src/lib.rs#L1548)
(`RwLock<Vec<...>>`) on every RecordId resolution. These mechanisms are
intentionally conservative correctness scaffolding, but they cannot stay on
the production hot path.

The one-thread gap has not yet been decomposed by a dedicated microbenchmark.
Likely contributors visible in the implementation include copying every key
into a new `Arc<[u8]>`, erased/boxed transaction items and hash lookup, repeated
`Arc`/`ArcSwap` reference-count traffic, and the hardened C-boundary validation
performed for every point operation. Treat those as profile targets, not as
separately quantified conclusions. A one-thread cycle profile did at least
place `TableShared::resolve_verified` first among Rust-side symbols at 12.93%,
with allocator, hash/rehash, item teardown, mutex validation, and per-operation
RCU bookkeeping also prominent.

## Method

The comparison uses a purpose-built paired harness because the repository's
historical `sto/concurrent.cc` workload is stale, is not wired into CMake, and
cannot be built in this checkout. Both new harnesses implement the same:

- uniform SplitMix64 streams and two random draws per logical operation;
- 100,000 prepopulated eight-byte big-endian keys and logical `u64` values;
- unique keys within each ten-operation transaction;
- read-before-write behavior and wrapping `u64` increments;
- retry of the exact same materialized transaction until commit;
- ready, two-second warmup, quiesce, RNG reset, five-second measurement, stop;
- counting only transactions that start and finish inside measurement; and
- JSON schema and accounting invariants.

The runner shuffled workload/thread cells deterministically, alternated which
engine ran first, and used seeds 1, 2, and 3 for the three repetitions. Each
process was restricted with `taskset` to the first `N` CPUs from 0–63. Those
CPU IDs are one hardware thread from each physical core; SMT siblings 64–127
were excluded. Threads could still migrate within the assigned set.

The C++ engine is
`MassTrans<uint64_t, versioned_value_struct<uint64_t>, false>`. The Rust engine
is native `sto-core` plus `sto-masstree`, with the Masstree directory reached
through its stable C ABI. Both are non-opaque OCC configurations. They execute
the same logical workload, but their physical record layouts differ: C++ keeps
the `uint64_t` in its generic value box, while Rust keeps an eight-byte
little-endian value in the Rust record registry and stores a RecordId in its
Masstree.

## Host and build

- Host: `zoo-002.lab.compas.cs.stonybrook.edu`
- OS: Ubuntu 24.04.4 LTS, kernel 6.8.0-137-generic
- CPU: one AMD EPYC 7702P socket, 64 physical cores, 128 SMT CPUs, one NUMA node
- Memory: 503 GiB
- Frequency policy: `schedutil`, boost enabled
- C++: Clang 22.1.8, effective `-O2 -g -DNDEBUG -march=native
  -fno-omit-frame-pointer`
- Rust: rustc 1.95.0, release `opt-level=2`, debug info 1, 16 codegen units,
  no LTO, `target-cpu=native`, frame pointers enabled
- Allocation: glibc malloc for both (`USE_MALLOC_MODE=0`); neither binary loads
  jemalloc
- C++ runtime: both binaries resolve libc++, libc++abi, and libunwind from LLVM
  22 via embedded RUNPATH; the C++ process also loads system libstdc++
  transitively through RocksDB

The exact structured environment and binary hashes are in
[`environment.json`](environment.json). The C++ benchmark binary SHA-256 is
`cefeb576...924c11`; the Rust binary SHA-256 is `a30808a...a3093`.

## Validation and artifacts

Before the timed run:

- the Rust workspace passed formatting, all tests, and strict Clippy;
- native Rust/C-ABI linking and mixed-workload smoke tests passed;
- C++ generic-value read, update, and contended-transaction smokes passed; and
- the existing packed `versioned_str_struct` Masstree tests passed 4/4.

An independent post-run audit found exactly 126 samples and 42 cells with
three repetitions each, seeds 1–3, unique run order 1–126, correct commands and
affinities, finite rates, and no schema, timing, or accounting errors. Every
sample satisfies `attempts = commits + aborts` and
`logical_ops = commits * ops_per_txn`. The median cell coefficient of variation
was 0.57%; 39 of 42 cells were below 2%, and the maximum was 4.02%. The gaps
above are therefore much larger than observed run-to-run variation.

The run interval was 2026-08-27 21:46:26–22:01:45 UTC.

- [`raw.jsonl`](raw.jsonl): all 126 samples and exact commands
- [`summary.csv`](summary.csv): engine/scenario/thread medians and ranges
- [`run.json`](run.json): runner configuration and schedule metadata
- [`environment.json`](environment.json): source, host, build, linkage, and hashes
- [`profile/profile.json`](profile/profile.json): follow-up profiler commands,
  counters, hashes, and the accompanying raw `perf stat` outputs

## Caveats

- This is end-to-end harness throughput, not isolated STO time. Workload
  materialization is inside the timed loop and uses a C++ small string versus a
  Rust `[u8; 8]` before the Rust table copies the key.
- Warmup writes remain in each engine's table. Faster engines therefore begin
  measurement with larger numeric values. Values are fixed-width and only
  incremented, so this should be throughput-neutral, but checksums are not
  comparable between engines.
- `taskset` constrains each process to physical cores but does not pin each
  worker to one specific core.
- The host kept its normal `schedutil`/boost settings and is a shared lab
  machine. Randomized order, alternating engines, and three low-variance
  repetitions mitigate but do not eliminate environmental noise.
- Glibc malloc was selected for allocator parity. Results can differ from a
  usual Masstree deployment tuned around jemalloc.
- The C++ target inherits Mako's large native link closure. That affects build
  and process startup, but startup and prepopulation are outside measurement.
- Three five-second repetitions on one host and one keyspace are a development
  baseline, not a publication-grade performance study.
- The `/var/tmp` binaries are volatile and were not committed; their SHA-256
  hashes and exact build configuration are preserved in `environment.json`.

## Next optimization order

1. Move C-ABI handle validation, tree structural synchronization, and STO
   resource-binding validation off the point-operation hot path while retaining
   fail-closed lifetime and thread-affinity checks.
2. Replace the record registry's shared `RwLock<Vec<...>>` lookup with bounded,
   stable, lock-free or sharded slots.
3. Profile and remove per-operation key allocation, boxing, hashing, and
   reference-count churn.
4. Re-run this exact matrix after each change; do not tune around the current
   low high-thread abort rate because it is a symptom of serialization.
