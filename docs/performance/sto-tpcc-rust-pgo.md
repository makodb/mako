# Rust STO TPC-C profile-guided build

[`build_sto_tpcc_pgo.sh`](../../scripts/build_sto_tpcc_pgo.sh) applies LLVM
instrumentation PGO to the Rust `libsto_tpcc_ffi.a` used by
`sto_tpcc_bench`. The benchmark driver and its native C++ dependency graph
remain ordinary Release objects. The resulting comparison therefore measures
PGO of the Rust STO path rather than PGO of the TPC-C workload driver.

Install the LLVM tools for the exact Rust toolchain selected for the build:

```sh
rustup component add llvm-tools-preview
```

The script rejects an `llvm-profdata` whose LLVM major version differs from
`rustc`. An instrumented Rust static library also needs the matching
`libprofiler_builtins` at the final C++ link. The script locates that archive in
the selected Rust sysroot and links it together with
`-Wl,-u,__llvm_profile_runtime`; omitting either part can produce an executable
that links incorrectly or never writes a usable profile.

## Recommended: reuse a validated native build

Fresh CMake trees can encounter Clang/CMake C++ standard-module BMI conflicts
while rebuilding this repository's large native graph. PGO does not require
recompiling those C++ objects. Point the script at an existing validated
Release build instead:

```sh
STO_TPCC_PGO_NATIVE_BUILD=/absolute/path/to/validated-native-build \
  scripts/build_sto_tpcc_pgo.sh /var/tmp/sto-tpcc-rust-pgo
```

The supplied directory must be a Ninja CMake build of `sto_tpcc_bench` from
the same physical checkout. It must contain `CMakeCache.txt`, `build.ninja`,
and the already-built object/archive graph referenced by the benchmark link
edge. Validate the native build before starting PGO, for example by building
and smoke-testing its ordinary benchmark once:

```sh
cmake --build /absolute/path/to/validated-native-build \
  --target sto_tpcc_bench --parallel 8
/absolute/path/to/validated-native-build/sto_tpcc_bench --help
```

Reuse-native mode does not invoke that build or change any file in it. It:

1. builds the profile-generate Rust archive in `OUTPUT/cargo-generate`;
2. extracts the one `sto_tpcc_bench` link command from Ninja;
3. replaces only the Rust archive and output path, removes the Ninja depfile
   side effect, and adds the Rust profile runtime;
4. links `OUTPUT/sto_tpcc_bench-generate` and trains it;
5. merges the raw profiles with the matching `llvm-profdata`;
6. builds the profile-use archive in `OUTPUT/cargo-use`; and
7. repeats the same native link with no instrumentation runtime, producing
   `OUTPUT/sto_tpcc_bench`.

The parser requires an explicit, auditable archive argument and rejects
ambiguous link edges or response-file layouts. CMake configure arguments after
`--` are intentionally unavailable in this mode: the supplied native graph,
including its compiler and link options, is authoritative.

## Clean-build fallback

With no `STO_TPCC_PGO_NATIVE_BUILD`, the script preserves the original clean
workflow: it configures separate `cmake-generate` and `cmake-use` trees and
gives each one an isolated Cargo target directory.

```sh
scripts/build_sto_tpcc_pgo.sh /var/tmp/sto-tpcc-rust-pgo -- \
  -DCMAKE_C_COMPILER=/usr/bin/clang-22 \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++-22
```

The clean profile-generate configuration injects both the forced LLVM profile
runtime symbol and the matching `libprofiler_builtins`. Its final benchmark is
`OUTPUT/cmake-use/sto_tpcc_bench`. Prefer reuse-native mode on hosts where a
fresh native build fails in C++ module generation.

The output directory must be new, outside the checkout, and contain no
whitespace. The script never reuses an existing output directory.

## Training controls

The default training run pins one worker to the first CPU allowed to the
process, uses one warehouse, runs for 60 seconds, selects the Rust engine, sets
the standard `45,43,4,4,4` TPC-C mix, and gives the benchmark a `1G` allocator.
Useful overrides are:

- `STO_TPCC_PGO_TRAIN_CPU`
- `STO_TPCC_PGO_TRAIN_SECONDS`
- `STO_TPCC_PGO_TRAIN_MIX`
- `STO_TPCC_PGO_TRAIN_ALLOCATOR_MEMORY`
- `STO_TPCC_PGO_CONFIG`
- `STO_TPCC_PGO_SITE`
- `STO_TPCC_PGO_BUILD_JOBS` (clean mode only)
- `CMAKE`, `CARGO`, `RUSTC`, `LLVM_PROFDATA`, `TASKSET`, and `PYTHON3`

The training log must contain exactly one valid `TPCC_BENCH_RESULT`: one Rust
worker, one warehouse, the requested duration, zero aborts, consistent
attempt/abort totals, mix counters that sum to commits, and at least one commit
for every transaction type with nonzero configured weight. The builder passes
the allocator setting through `MAKO_TPCC_ALLOCATOR_MEMORY` and records the
effective allocator and mix in `provenance.txt`, `training.log`, and the
`environment_overrides` object in `training-result.json`. Raw build-time
profiles are kept separate and are never merged into the workload profile.

The builder rejects these diagnostic fallback variables even when they are set
to an empty value:

- `MAKO_STO_TPCC_DISABLE_PAYMENT_PREFIX`
- `MAKO_STO_TPCC_DISABLE_PAYMENT_FULL`
- `MAKO_STO_TPCC_DISABLE_NEW_ORDER_FULL`
- `MAKO_STO_TPCC_DISABLE_DELIVERY_FULL`
- `MAKO_STO_TPCC_DISABLE_STOCK_LEVEL_FULL`

This prevents a stale diagnostic shell environment from silently training a
mix of Rust and scalar C++ transaction paths. The all-unset state is recorded
in `provenance.txt` and `training.log`.

## Reproducibility and provenance

Every run records the Git head, working-tree patch and untracked source files,
a source-state digest, Rust/Cargo/LLVM versions and executable hashes, the
shard-configuration hash, explicit training settings, CPU information,
commands, logs, ELF notes, dynamic dependencies, raw and merged profiles, and
final artifact hashes. The source, Rust toolchain, and training configuration
must remain unchanged between generate, train, and profile-use.

Reuse-native mode additionally verifies and records:

- the canonical CMake source root, Ninja generator, Release build type, cached
  C++ compiler, compiler version, and original Rust archive location;
- the original Ninja link command and each transformed relink command;
- every explicit native object/archive/shared-library input plus
  `CMakeCache.txt`, `build.ninja`, and `rules.ninja`; and
- a native-graph digest checked before and after both relinks.

Those checks prove which immutable graph was reused and that it did not change
during PGO. They cannot reconstruct the historical source revision from which
an arbitrary pre-existing object was compiled; selecting a genuinely
validated native build remains the caller's explicit provenance decision.
Artifact paths in `/var/tmp` and their SHA-256 values are run-specific evidence,
not durable repository locations.

## Implementation measured

### Dense Item and Stock identity caches

The TPC-C integration gives the shared Item table and each warehouse-local
Stock table a dense cache with 100,000 exact slots. Each slot is one
`AtomicU64` and stores only a stable `ResolvedRecord` identity. It does not
store row bytes, a lock, or an OCC version. A cache hit skips the Masstree
directory traversal, then performs the normal STO observation and validation
against the current record state.

The fused NewOrder path reads Item through this cache and uses it while
modifying Stock. StockLevel uses it for Stock reads. These paths build compact,
caller-owned arrays for unresolved positions, resolve those misses in one
fixed-width native batch, and then visit every input in its original order.
They do not allocate a miss vector during the transaction. A resolved miss
publishes its identity into the empty slot with a checked atomic operation.

A Stock cache binds to one positive warehouse ID because its dense index is the
item ID. If one table handle receives a different warehouse prefix, it
permanently disables its dense path and resumes ordinary directory resolution.
The cache is therefore optional for correctness and cannot silently reuse a
Stock identity for the wrong warehouse.

### Post-load static directories

After loader transactions and their synchronization barriers finish, the
benchmark calls the database's `on_load_complete` hook. The Rust wrapper seals
the physical directories for `customer`, `customer_name_idx`, `district`,
`item`, `stock`, `stock_data`, and `warehouse`. Existing records remain
readable and mutable. New physical keys are rejected, and scans no longer take
the Rust structural read gate because later directory publication is
impossible. Tables that grow during TPC-C execution remain open.

The final sweep measures this combined implementation, including the earlier
fused transaction and fixed-batch work. It is not an isolated dense-cache or
directory-seal ablation, so the results below must not be attributed to one
change alone.

### Allocator capacity

The benchmark previously fixed its NUMA allocator at `1G` total. At 16 workers
that gives each worker 64 MiB before huge-page rounding. A 16-worker Rust run
exhausted one native RCU arena while a Masstree leaf split allocated another
node. This was a capacity failure, not a throughput result.

The comparison runner now accepts `--allocator-memory` and passes the selected
`MAKO_TPCC_ALLOCATOR_MEMORY` value to both engines. It records the value in
every raw sample and as `allocator_memory` in `run.json`. The final sweep uses
`2G`, which gives 128 MiB per worker at 16 workers before huge-page rounding.
The benchmark default remains `1G` for callers that do not select a value.

## Current final results

> Measurement status: complete. The values below come from the recorded PGO
> and guarded comparison artifacts named in the evidence section.

### PGO training

The profile-generate run uses one Rust worker, one warehouse, 60 configured
seconds, and the explicit `45,43,4,4,4` NewOrder, Payment, Delivery,
OrderStatus, and StockLevel mix. The builder controls the allocator through
`STO_TPCC_PGO_TRAIN_ALLOCATOR_MEMORY`, verifies the training result, excludes
build-time profiles from the merged workload profile, and requires all five
diagnostic fallback variables to be unset.

- host and date: `zoo-002`, 2026-09-04
- Git head: `105351016a8d2ae89d560857c32f85f6111df6f3`
- recorded source state:
  `3bf56754aa3ae3456318c071e07a483f7ba1cc27d92a5f43862feab327757af8`
- Rust, LLVM, and native C++ toolchains: Rust 1.95.0, Rust LLVM 22.1.2,
  and Homebrew Clang 22.1.8
- training CPU and allocator: CPU 10 and `2G`
- measured training time: 60.414727 seconds
- commits, aborts, and throughput: 2,172,465 commits, zero aborts, and
  35,959.196 txn/s

The profile-use binary uses `target-cpu=native` and must be rebuilt for a host
with a different processor.

### Guarded sweep configuration

The comparison uses one warehouse per worker, worker counts `1,2,4,8,16`,
three paired repetitions per count, five measured seconds per process, mix
`45,43,4,4,4`, schedule seed 4, and allocator `2G`. Physical worker CPUs are
`10,11,12,13,14,15,16,18,19,20,21,22,23,24,25,26`. The runner also guards
their SMT siblings. A controller outside the measured worker set holds every
CPU frequency policy at 2 GHz in userspace mode with boost disabled, then
restores `schedutil` and boost.

Each process loads a fresh database. The runner shuffles worker-count cells and
alternates which engine runs first. Before each pair and again between engines,
it waits for a new LXD restart followed by a two-second quiet window. Every
selected worker CPU and SMT sibling must be at least 95 percent idle, and no
recognized benchmark, compiler, or `perf` process may be active.

The benchmark emits timestamped `TPCC_BENCH_MEASURE_START` immediately before
worker release and `TPCC_BENCH_MEASURE_END` after worker join and elapsed-time
capture. The runner requires exactly one of each marker and a positive marked
interval. LXD journal activity inside that interval rejects the complete pair.
Activity during setup or teardown does not reject a sample.

A process timeout before the start marker rejects the complete pair and uses
the same bounded retry path as the other guard failures. The runner keeps the
partial standard output, standard error, and a separate timeout JSON record.
A timeout after measured work starts is terminal. This distinction prevents a
slow or failed measured run from being silently retried until it looks fast.

### Throughput

| Workers and warehouses | C++ median txn/s | Rust median txn/s | Paired Rust/C++ median | Paired range | Rust gap |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 47,304.380 | 57,679.565 | 122.017% | 121.873-122.053% | +22.017% |
| 2 | 97,081.750 | 111,222.685 | 114.548% | 113.746-115.337% | +14.548% |
| 4 | 196,527.667 | 213,345.496 | 108.557% | 107.424-109.058% | +8.557% |
| 8 | 356,438.970 | 380,287.488 | 106.691% | 105.264-107.701% | +6.691% |
| 16 | 788,150.387 | 782,418.546 | 99.059% | 98.971-106.778% | -0.941% |

Each percentage is the median of the three paired Rust/C++ throughput ratios,
not the ratio of the two throughput medians.

A positive Rust gap means Rust is faster. A negative value means Rust is
slower. At 16 workers, the implementation is within 0.941 percent of C++ by
the paired median. This replaces the historical 21.157 percent deficit. Rust
is faster than C++ at each lower worker count in this sweep.

The runner accepted all 30 samples in 15 pairs and rejected zero pair
attempts. Pre-launch quiet-window skips do not create a sample. C++ recorded
22,513,754 attempts, 22,510,198 commits, and 3,556 aborts. Rust recorded
23,677,698 attempts, 23,675,493 commits, and 2,205 aborts. Every accepted row
satisfies `attempts = commits + aborts`, and its five transaction counters sum
to commits. Its recorded environment overrides are exactly
`MAKO_TPCC_ALLOCATOR_MEMORY=2G` and
`MAKO_TPCC_WORKLOAD_MIX=45,43,4,4,4`. The accepted measurement intervals must
have empty LXD journal activity and no recorded competitor. An independent
post-run query found zero LXD journal records across all 30 measurement
intervals; the minimum accepted pre-launch CPU idle value was 95 percent.

This is a single-shard concurrency sweep. The dataset grows from one to 16
warehouses with the worker count, so it does not isolate concurrency from
dataset size. It is not an official tpmC result or a fixed-dataset scaling
experiment.

### Evidence

The PGO artifact directory is
`/var/tmp/sto-rust-dense-release-pgo-20260904-resume1`:

- source-state SHA-256:
  `3bf56754aa3ae3456318c071e07a483f7ba1cc27d92a5f43862feab327757af8`
- native-graph SHA-256:
  `e7b52c395852f2556bd9a8a15ea1fbc71e2adab9fe4c0b2b849f4e07a4f72823`
- optimized benchmark SHA-256:
  `d9baffa4d9cd9f843ea07a00fe98dfe190550d6f1b91118a5a393820278ed492`
- optimized Rust archive SHA-256:
  `9188463b8f4c59639b34f783d2c14eb50a6f4361ce0c2292e5a287af88fa648c`
- merged profile SHA-256:
  `5684fc957ce8ce28f226894e7217834225c5dee08d97410c2d44dae6bbffa8e4`

The guarded comparison directory is
`/var/tmp/sto-rust-dense-release-sweep-20260904-resume1`:

- `raw.jsonl`:
  `553fb60e2cae939a1028f539e5f8e6ca52ca5e380b76706439c18756f5cd1493`
- `summary.csv`:
  `314efd5de6c7f7dbecc53ff260b426990b505bb089d4ffceeeff4068b5ccd29d`
- `run.json`:
  `dd6197a6b4f2b18567e8d32fe52ea0bd61494c063ed12c62d4b2b5911c0de353`

`STATUS`, `training-result.json`, `provenance.txt`,
`artifacts-sha256.txt`, `run.json`, `raw.jsonl`, timeout records, and
`summary.csv` retain the commands and controls. Paths under `/var/tmp` are
host-local evidence. The hashes identify the exact files after those paths are
removed.

The source-state digest covers the measured implementation and the placeholder
version of this result section. Replacing those placeholders with the sealed
artifact values above is the only post-measurement source-tree edit.

## Historical baseline

The previous three-repetition sweep ran on `zoo-002` on 2026-09-01. It used
the earlier source state and the old fixed `1G` allocator. These values explain
why the 16-worker path received more work, but they are not a controlled
ablation against the current implementation.

| Workers and warehouses | C++ median txn/s | Rust median txn/s | Paired Rust/C++ median |
| ---: | ---: | ---: | ---: |
| 1 | 47,128.163 | 45,480.880 | 96.289% |
| 2 | 96,697.623 | 90,090.300 | 93.029% |
| 4 | 196,867.861 | 170,413.159 | 87.540% |
| 8 | 383,108.617 | 328,380.029 | 85.715% |
| 16 | 784,914.451 | 590,534.944 | 78.843% |

The historical PGO artifact was
`/var/tmp/sto-rust-exact-markers-pgo-20260901T0430Z`, and its comparison output
was `/var/tmp/sto-rust-tpcc-sweep-exact-pgo-20260901T0435Z`. Those host-local
paths may no longer exist. The old report recorded their hashes before this
section was replaced; Git history preserves that report.
