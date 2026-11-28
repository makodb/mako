# Native Masstree wrapper performance qualification

This comparison measures local transactions through the production generic byte
row adapter and Distributed STO. It excludes RPC, Raft and durable logging.
It does not compare the retired fixed-u64 backend. The 80% performance gate
passed on November 27, 2026. Correctness qualification also passed.

The reference is `MassTrans<std::string, versioned_str_struct, false>` at
`0d26e2e0b`. Its transaction, index and packed-string implementations are
unchanged. `reference-string.patch` adapts that revision's numeric benchmark to
string rows and adds committed-write conservation. The original required value
footer remains present. The Rust row holds the same eight logical payload bytes.
Both use the same random stream, ten unique keys per transaction, retry behavior,
warmup/quiescence/measurement phases and seed reset. Each successful process
checks every increment, including warmup and boundary transactions.

`local_environment.cc` supplies the original disabled-admission behavior.
Excluded RPC and log entry points abort if reached. Rusty is a benchmark-only
support dependency for the historical reference. Production builds do not
compile SRPC or Rust applications into C++.

## Build and reproduce

Run every command inside the build container from the repository root. The
recorded toolchain is Clang/libc++ 22 and the workspace Rust toolchain. Both
compilers use native CPU instructions. The Rust executable uses full LTO; the
normal development profile remains unchanged. Qualification does not claim
these numbers for unoptimized builds.

```sh
export PATH=/opt/rust/bin:$PATH RUSTUP_TOOLCHAIN=/opt/rust CARGO_BUILD_JOBS=32
cmake -S . -B build_sto_perf -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DMAKO_BUILD_PROFILE=MASSTREE_TX -DUSE_MALLOC_MODE=1 \
  '-DCMAKE_CXX_FLAGS=-stdlib=libc++ -march=native'
cmake --build build_sto_perf --target mako_native_storage --parallel 32
export CARGO_TARGET_DIR=$PWD/build_sto_perf/cargo MAKO_BUILD_DIR=$PWD/build_sto_perf
export LIBCXX_DIR=/usr/lib/llvm-22/lib MAKO_MTREE_NATIVE_INTEGRATION=1
export MAKO_MTREE_NATIVE_LIB_DIRS=$PWD/build_sto_perf:/usr/lib/llvm-22/lib:/usr/lib/x86_64-linux-gnu
export MAKO_MTREE_NATIVE_LIBS=static:-bundle=mako_native_storage,dylib=c++,dylib=c++abi,dylib=numa,dylib=pthread,dylib=jemalloc
export RUSTFLAGS='-C target-cpu=native' CARGO_PROFILE_RELEASE_LTO=fat
cargo build --locked --manifest-path crates/Cargo.toml \
  -p sto-masstree --example native_parity --release
```

The yaml-cpp checkout has been removed from production. Fetch its pinned
historical headers only for this reference build, or use an existing local
checkout containing that commit:

```sh
mkdir -p build_sto_reference/source/third-party/yaml-cpp
git archive 0d26e2e0b src | tar -x -C build_sto_reference/source
git clone --filter=blob:none --no-checkout https://github.com/jbeder/yaml-cpp/ \
  build_sto_reference/yaml-git
git -C build_sto_reference/yaml-git archive \
  89ff142b991af432b5d7a7cee55282f082a7e629 include | \
  tar -x -C build_sto_reference/source/third-party/yaml-cpp
```

For a fresh reference build, extract `src` from `git archive 0d26e2e0b` into
`build_sto_reference/source`. Extract `include` from the yaml-cpp submodule
revision `89ff142b991af432b5d7a7cee55282f082a7e629` into
`build_sto_reference/source/third-party/yaml-cpp`. These are benchmark inputs,
not restored production sources. The Rusty submodule must be
`7e0c201f1b0d548f0166dc9ee700f24bc18066a4`, as at the reference revision.

```sh
cmake -S third-party/rusty-cpp -B build_sto_reference/rusty -G Ninja \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++-22 \
  '-DCMAKE_CXX_FLAGS=-stdlib=libc++ -D_LIBCPP_NO_ABI_TAG' \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_MODULE_STD=ON \
  -DCMAKE_EXPERIMENTAL_CXX_IMPORT_STD=451f2fe2-a8a2-47c3-bc32-94786d8fc91b \
  -DBUILD_TESTING=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build_sto_reference/rusty --target rusty --parallel 12
cp docs/testing/sto-80/local_environment.cc build_sto_reference/local_environment.cc
patch -o build_sto_reference/string_compare.cc \
  build_sto_reference/source/src/mako/benchmarks/sto_masstree_compare.cc \
  docs/testing/sto-80/reference-string.patch
python3 docs/testing/sto-80/compile-clean.py
python3 docs/testing/sto-80/build-string-clean.py
python3 docs/testing/sto-80/run_pairs.py --threads 1 2 4 8 16 32 \
  --repeats 5 --duration-ms 3000 --output build_sto_perf/qualification.jsonl
```

The paired runner records exact commands and executable hashes and alternates
order by seed. The pass criterion is Rust/C++ median throughput of at least
0.80 for **each** thread-count/workload cell. It never averages reads and writes
or stops early when a trial passes. No other build or test runs during timing.
CPU IDs 24 through 55 select 32 distinct physical cores on this machine;
adjust the runner's affinity for another host. Its small controller thread
shares the selected mask in both implementations.

The Rust benchmark uses a short synchronous RCU region around the access loop,
closed before commit. The equivalent application API is
`Transaction::with_access`. A region must not cover I/O, await or 2PC waits.
The default per-operation RCU mode remains supported but is a separate
performance mode. These point-workload ratios do not establish parity for
large values, insert-heavy workloads, scans, checkpoints or distributed work.
Those remain separate manual/weekly performance measurements. Native tests
cover binary values, misses, insert/split predicates, history and scans.

## Measured results

Five alternating-order pairs per cell used seeds 1 through 5, 700ms warmup and
3000ms measurement. All 120 processes passed committed-write conservation.
These are ratios of median committed-operation throughput, not peak trials.
The machine is an AMD Ryzen Threadripper 2990WX with 32 physical cores and 64
logical CPUs. Process affinity selected distinct physical cores; individual
workers and NUMA placement were not separately pinned. No build or test job
ran alongside the measurements.

| Threads | C++ reads Mops/s | Rust reads Mops/s | Rust/C++ | C++ mixed Mops/s | Rust mixed Mops/s | Rust/C++ |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 3.214 | 2.988 | 93.0% | 2.205 | 1.971 | 89.4% |
| 2 | 6.719 | 6.146 | 91.5% | 4.682 | 4.023 | 85.9% |
| 4 | 13.320 | 12.287 | 92.3% | 9.350 | 8.052 | 86.1% |
| 8 | 26.184 | 24.003 | 91.7% | 16.552 | 14.877 | 89.9% |
| 16 | 53.887 | 51.614 | 95.8% | 31.695 | 28.502 | 89.9% |
| 32 | 102.872 | 96.596 | 93.9% | 56.241 | 47.645 | 84.7% |

[Raw trials](qualification.jsonl) record commands, executable hashes, committed
operations, retries and conservation results. The [summary](qualification.summary.json)
retains unrounded medians. The [source manifest](source-manifest.json) identifies this dirty worktree;
these results are not attributed to its unchanged HEAD commit.

Single-thread qualification passed before multithread tuning began. The
[first passing single-thread trials](single-first-pass.jsonl) reached 81.8%
reads and 80.2% mixed updates. An earlier full matrix passed through 16 threads
but reached only 75.5% on 32-thread mixed updates. That
[failed matrix](before-hot-row.jsonl), its [summary](before-hot-row.summary.json)
and [source manifest](before-hot-row-manifest.json) are retained.
The final row layout moves ordinary update state into the first cache line and
passes the checked table owner through private ABI version 4, avoiding a row
backpointer and a second dirty cache line on small local updates. Its full
matrix passes every cell, with a minimum ratio of 84.7%.

The original 100% parity goal remains unestablished. This record qualifies the
user's revised 80% target for the workloads and build settings above.

## Correctness qualification

All builds, executable tests, proofs and measurements ran inside the build
container. Native cases each ran in a fresh process. No production source
changed between the timing matrix and these final checks.

| Check | Result | Evidence |
|---|---|---|
| Isolated Masstree transaction component | 70 executable cases, 8 doctests, strict wrapper Clippy and dependency audit passed | [Component log](component.log) |
| Independent shared STO tests | 368 executable cases and 72 doctests passed | [Core log](core.log) |
| Runtime, RocksDB-compatible API and workloads | 135 fresh-process cases passed with replication and both storage engines | [Runtime log](runtime.log) |
| Upper layer, 32 CPUs | 218 scenarios and all 29 negative controls passed in 164.893 seconds | [Log](upper.log), [report](upper-report.json) |
| Native ASan | 70 cases passed | [Manifest](address/manifest.txt), [report](address/tests.json), [case output](address/cases.log) |
| Native TSan | 70 cases passed | [Manifest](thread/manifest.txt), [report](thread/tests.json), [case output](thread/cases.log) |
| Native UBSan | 70 cases passed | [Manifest](undefined/manifest.txt), [report](undefined/tests.json), [case output](undefined/cases.log) |
| Component census and boundary tool regressions | 7 cases passed; generated inventory is current | [Tooling log](tooling.log) |

The core suite ran before the final native row layout change; its source did
not change afterward. Upper verification includes 1,497 sharding, 40 host,
42 production STO, 10 historical coordinator and one Raft retention obligation.
The production C++ boundary is tested under its contracts, not proved by Verus.

Each sanitizer used a fresh native build and checked instrumentation in all 16
native sources. ASan and TSan also instrument Rust and the pinned standard
library. UBSan covers C++; it does not claim Rust instrumentation. The
[exact sanitizer command](sanitizer-command.txt) records the toolchain/cache
paths, compiler options and packages. Existing Masstree race/bounds/root-lifetime
suppressions remain in force. ASan disables leak reporting only for the one
existing intentional-quarantine case. No suppression or quarantine case was
added. Each `cases.log` preserves individual outputs and their original paths;
`tests.json` retains the original runner report. Runtime integration ran without
sanitizers.

The earlier [failed runtime run](runtime-before-leaf-fix.log) exposed stale-leaf
conflicts being reported during staging instead of commit. The fix retains the
first leaf observation, advances it only for a matching own insertion, and lets
final certification reject the phantom. Existing facade assertions were kept.
A new test covers missing/range reads followed by a foreign insertion and later
staging; another covers local/timestamped history changes and retained snapshots.

Raw data includes failed checkpoints rather than replacing them with passing
trials. Performance remains a separate manual/weekly job; normal builds and
component checks do not invoke the benchmark.
