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
