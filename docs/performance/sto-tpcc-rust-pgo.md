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
process, uses one warehouse, runs for 60 seconds, selects the Rust engine, and
sets the standard `45,43,4,4,4` TPC-C mix explicitly. Useful overrides are:

- `STO_TPCC_PGO_TRAIN_CPU`
- `STO_TPCC_PGO_TRAIN_SECONDS`
- `STO_TPCC_PGO_TRAIN_MIX`
- `STO_TPCC_PGO_CONFIG`
- `STO_TPCC_PGO_SITE`
- `STO_TPCC_PGO_BUILD_JOBS` (clean mode only)
- `CMAKE`, `CARGO`, `RUSTC`, `LLVM_PROFDATA`, `TASKSET`, and `PYTHON3`

The training log must contain exactly one valid `TPCC_BENCH_RESULT`: one Rust
worker, one warehouse, the requested duration, zero aborts, consistent
attempt/abort totals, mix counters that sum to commits, and at least one commit
for every transaction type with nonzero configured weight. Raw build-time
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
training configuration hash, CPU information, commands, logs, ELF notes,
dynamic dependencies, raw and merged profiles, and final artifact hashes. The
source, Rust toolchain, and training configuration must remain unchanged
between generate, train, and profile-use.

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

## Final one-worker standard-mix result

The final controlled comparison ran on `zoo-002` on 2026-09-01. It used one
worker, one warehouse, CPU 10, a fixed 2 GHz frequency, disabled boost, and the
explicit `45,43,4,4,4` NewOrder, Payment, Delivery, OrderStatus, and StockLevel
mix. The Rust implementation was built with Rust 1.95.0 and LLVM PGO. The
native graph used Clang 22.1.8.

The 60-second profile-generate run committed 3,166,318 transactions with zero
aborts over 60.399 measured seconds, or 52,423.022 transactions/s. Three paired
five-second trials then produced:

| Engine | Median txn/s | Minimum | Maximum | Median abort % |
| --- | ---: | ---: | ---: | ---: |
| C++ STO/Masstree | 47,069.715 | 47,016.582 | 47,281.370 | 0.000 |
| Rust STO/Masstree with PGO | 45,395.309 | 45,210.856 | 45,450.501 | 0.000 |

The median of the three paired Rust/C++ ratios was **96.050840%**. The final
one-worker gap was **3.949160%**. All six samples had zero aborts, and every
pair was accepted on its first attempt.

### Measurement controls

Each process loaded a fresh database. Engine order alternated across pairs.
Before every engine sample, the runner waited for a fresh LXD daemon restart
and then required a stable two-second quiet window. CPU 10 and its SMT sibling,
CPU 74, had to be at least 95% idle, and no recognized STO, compiler, or `perf`
job could be active. Every CPU frequency policy was held in userspace mode at
2 GHz with boost disabled during the comparison, then restored to `schedutil`
with boost enabled.

All five `MAKO_STO_TPCC_DISABLE_*` fallback variables were explicitly unset
during PGO and comparison. Each raw comparison record contains only the
explicit workload mix in `environment_overrides`, confirming that no fallback
switch reached either engine.

### Evidence

The PGO artifact directory is
`/var/tmp/sto-rust-stocklevel-pgo-20260901T0235Z`. Its relevant identifiers
are:

- source-state SHA-256:
  `bfbf306a9fb12298b03a13cc8d55a8d1503da3ad4b886870c25f27e13c994f5b`
- optimized benchmark SHA-256:
  `b93d71c63954e31d75d1a2c50333586a537b33979cafaef277cf139cb7011d36`
- optimized Rust archive SHA-256:
  `76054fa132ce72b238d5b8065e1f6da0268688f5642aea2f650801e5f29f33fd`
- merged profile SHA-256:
  `b94fd5c8fbc6240f751a9e33a712703ec556cf5429d65af0279c09547d75ca78`

The paired result directory is
`/var/tmp/sto-rust-stocklevel-final-aligned-pgo-20260901T0259Z`. Its evidence
hashes are:

- `raw.jsonl`:
  `e015df74183cd87c894ced0391e74306762c3fbe059697371ca171dfe1232dd9`
- `summary.csv`:
  `8e256860f8f4228a00604b409d5a9412a20f346faa35fb92bc6d70669410791c`
- `run.json`:
  `0843334d79023cd2ec2e767e5889764509955a2b2cf5586cb662205d6199ddaa`

`STATUS`, `training-result.json`, `provenance.txt`,
`artifacts-sha256.txt`, `run.json`, `raw.jsonl`, and `summary.csv` retain the
full commands and controls. These `/var/tmp` paths are host-local and
temporary; the hashes are the durable identifiers. The benchmark used Git
head `f977554efb816a3c58669108ceeeb3b4b2a14f59` plus the recorded dirty source
patch. Later working-tree changes hardened benchmark controls, repaired native
test isolation, and updated documentation; they did not alter the measured
benchmark binary.

### Optimization history and scope

The first completed pass34d comparison measured Rust at 54.410992% of C++.
The final result follows transaction-level fusion of the Payment, NewOrder,
Delivery, and StockLevel hot paths, resolved presence reads for NewOrder,
fixed-capacity batch and scan paths, and Rust-only PGO. This progression cuts
the measured one-worker penalty from 45.589008% to 3.949160%.

This result establishes one-worker parity for this local standard mix. It does
not establish multiworker scaling. The `target-cpu=native` PGO binary is
specific to the `zoo-002` processor and should be rebuilt for another host.

## Current controlled 1-to-16-worker sweep

The current clean-source reference is a three-repetition sweep from the same
host on 2026-09-01. It uses Git head
`d1d5c5d1c22deb67828bd9e6d12f4b750da87399`, including exact measurement
markers immediately before worker release and after worker join and elapsed
time capture. This sweep supersedes the one-worker number above when assessing
the current benchmark and also measures scaling through 16 workers.

The PGO training run used one Rust worker and the standard `45,43,4,4,4` mix.
It committed 3,161,584 transactions with zero aborts over 60.401305 measured
seconds, or 52,342.975 transactions/s. The comparison used one warehouse per
worker, five measured seconds per process, and 15 matched pairs:

| Workers and warehouses | C++ median txn/s | Rust median txn/s | Paired Rust/C++ median | Paired range | C++ scale-up | Rust scale-up |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 47,128.163 | 45,480.880 | **96.289%** | 96.081–96.542% | 1.000x | 1.000x |
| 2 | 96,697.623 | 90,090.300 | **93.029%** | 92.197–94.229% | 2.052x | 1.981x |
| 4 | 196,867.861 | 170,413.159 | **87.540%** | 86.216–88.068% | 4.177x | 3.747x |
| 8 | 383,108.617 | 328,380.029 | **85.715%** | 82.466–92.747% | 8.129x | 7.220x |
| 16 | 784,914.451 | 590,534.944 | **78.843%** | 75.054–79.305% | 16.655x | 12.984x |

Each percentage is the median of three paired Rust/C++ throughput ratios, not
the ratio of the two throughput medians. The one-worker penalty is therefore
3.711%. The gap widens with worker and warehouse count. Relative to each
engine's own one-worker median, C++ reaches 16.655x at 16 workers while Rust
reaches 12.984x. This design changes concurrency and dataset size together, so
it does not isolate their individual effects. The Rust profile was trained
only at one worker, so the high-worker result also includes possible PGO
workload mismatch.

Abort rates were small throughout. Across all accepted samples, C++ recorded
3,522 aborts in 22,611,497 attempts (0.015576%), and Rust recorded 1,341 in
18,750,657 attempts (0.007152%). Thus abort frequency does not explain Rust's
lower multithread throughput.

### Sweep controls and audit

The runner used physical CPUs 10 through 25, adding SMT siblings 74 through 89
to the idle guard. It held all 128 policies at 2 GHz in userspace mode with
boost disabled, then restored `schedutil` and boost. Seed 4 shuffled the cell
order and balanced which engine ran first across worker counts. Every engine
sample loaded a fresh database and started in its own newly aligned LXD restart
interval after a two-second window in which each guarded CPU was at least 95%
idle and no recognized benchmark, compiler, or `perf` process was present.

All 30 accepted samples contain exactly one start marker and one end marker.
Their stored LXD journal activity is empty, and a separate post-run journal
query found no activity in any of the 30 exact measurement intervals. Every
row satisfies `attempts = commits + aborts`, its transaction counters sum to
commits, and its only recorded environment override is
`MAKO_TPCC_WORKLOAD_MIX=45,43,4,4,4`; all diagnostic fallback variables were
absent. The generated `summary.csv` was independently recomputed and matched
at six decimal places.

Two attempts for the first 8-worker pair were discarded after an unrelated
Mako compilation appeared on the host. Three engine samples were discarded in
total. The guard recorded the competing Cargo and Clang processes, waited for
the host to become quiet, and accepted the third attempt. All other pairs were
accepted on their first attempt.

### Sweep evidence

The PGO artifact directory is
`/var/tmp/sto-rust-exact-markers-pgo-20260901T0430Z` on `zoo-002`:

- source-state SHA-256:
  `1285b5ab92d0bd467d4872afebc5a819a377c3be89c7f1e52a5ce5ec156064f6`
- optimized benchmark SHA-256:
  `2953193e9b577a9958845cacd30f946c9f62166546de8572aa95b2a556d47b53`
- optimized Rust archive SHA-256:
  `02284a8115384f97fcf81ac060bbe887f2da1aa9d93a457f1a462ae8741e6a24`
- merged profile SHA-256:
  `9d4e9efd997ab4789f0a28da98d6f604cb2f20745536bbe9c3cb47b083d8c6dd`

The result directory is
`/var/tmp/sto-rust-tpcc-sweep-exact-pgo-20260901T0435Z`:

- `raw.jsonl`:
  `9504ed0c1dac2a1c04c9f4d6404aece2f559b4e982c21aea85689de770ab21a2`
- `summary.csv`:
  `134c55e8c94ada29b40a5d33f57badd1557e37c1702acb0e6fded8bb4e306b8a`
- `run.json`:
  `1ba814a7f03bfdeeb3e1e3b7e78174d3749a1e69830566a6874c016abb463106`

These paths are host-local evidence. This is a single-shard concurrency sweep
whose dataset grows from one to 16 warehouses along with the worker count. It
is not an official tpmC result and is not a fixed-dataset scaling experiment.
