# Native Rust development

Mako's default production build is native Rust. Start with the repository
[README](../../README.md) for container setup, programs and supported behavior.

## Build profiles

Inside the container:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
```

`MAKO_BUILD_PROFILE=NATIVE_RUST` compiles the 15 explicitly listed Masstree
storage support files and builds the Rust applications with Cargo. The native
archive is linked explicitly; first-party Rust archives must not bundle C++ objects.
The maintained `rocksdb` crate builds its pinned native engine and compression
codecs. Upstream dependency archives are audited separately from Mako code.
Do not point it at the container's older system RocksDB. A sanitizer override must
provide matching 11.8.1 library and headers via `ROCKSDB_LIB_DIR` and
`ROCKSDB_INCLUDE_DIR`.
`MAKO_BUILD_PROFILE=ROCKSDB_RUNTIME` builds the same distributed runtime without
Masstree. One-shard and multi-shard deployments use the same transaction path.

Use a separate build directory for storage's C++ component tests:

```sh
cmake -S . -B build_storage -G Ninja -DCMAKE_BUILD_TYPE=Release -DMAKO_STORAGE_TESTS_ONLY=ON
cmake --build build_storage --parallel 4
ctest --test-dir build_storage --output-on-failure
```

The production source audit deliberately excludes these test objects.
`BUILD_DIR=build bash ci/native_rust.sh storage` manages the same separate
profile under `build/storage-tests`.

### Masstree transaction component

`sto-masstree` and `sto-masstree-api` own the transaction adapter and its Rust/native
wrapper. They share `sto-core` and the underlying Masstree index but have their
own compilation and local-test entry point:

```sh
./docker_build.sh ci masstreeTx 32
# Or inside the container:
CI_BUILD_JOBS=32 bash ci/masstree_tx.sh
```

This uses `build_masstree_tx/cargo`, independently of application builds. Set
`MASSTREE_TX_BUILD_DIR` for a different component directory. For compilation
without tests, run inside the container:

```sh
cmake -S . -B build_masstree_tx -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DMAKO_BUILD_PROFILE=MASSTREE_TX -DMAKO_NATIVE_CARGO_PROFILE=dev
cmake --build build_masstree_tx --parallel 32
# Explicit local test and quality targets:
cmake --build build_masstree_tx --parallel 32 --target masstree_tx_tests masstree_tx_quality
```

The default build compiles the two libraries and audits their dependency graph
and actual artifacts. No application targets, RocksDB, SRPC or Raft are built.
The only native archive is the audited Masstree/index support closure. Only STO and clock cores are shared dependencies. The wrapper does not depend
on sharding, protocol decisions, node runtime or workloads.

Local tests cover both adapter packages, including native byte rows and retained participant cases,
owned lifetimes, local participant transitions and replay/checkpoints. Native cases run in fresh processes. Doctests and the wrapper's
strict Clippy check retain their existing coverage. Core OCC tests, Rust/native
index tests and distributed runtime integration remain separate suites. The
compatibility target `rust_application_adapter_tests` delegates to this suite.

## Test selection

For distributed transaction, sharding and migration work, use:

```sh
./docker_build.sh ci upper
```

This runs the production Verus proofs, selected semantic negative controls and
small deterministic scenarios. It defaults to four logical CPUs and has a five-minute
wall deadline. It does not compile Masstree, RocksDB, SRPC or Raft. Storage,
local transaction effects, delivery, logging and time come through the explicit
[upper-layer contracts](upper-layer-contracts.md). Production adapters call the
same core functions as this suite.

Use more CPUs on a development machine with an explicit budget:

```sh
./docker_build.sh ci upper 16
# Inside the container:
bash ci/upper.sh --cpus 16
```

The budget sets process affinity, Cargo jobs and Verus workers together. It is
capped by the calling process's available CPU affinity. `MAKO_UPPER_CPUS` also
sets the default inside the container. Reports record requested and actual CPU
counts; proof coverage and the five-minute deadline remain the same.

On a new checkout, download and compile separately inside the container:

```sh
bash ci/upper.sh --bootstrap
bash ci/upper.sh
```

Bootstrap never reports a passing correctness check. Every normal invocation
reruns all positive proofs and scenarios, even if Cargo has nothing to compile.
Stage logs, dependency inventories, source identity and timing reports are
under `$BUILD_DIR/upper/runs`. `python3 scripts/qualify_upper.py` measures five
complete runs, including representative source touches to force incremental
recompilation. Plain `cmake --build` and Cargo compilation run no test suites.

| Gate | Coverage |
| --- | --- |
| `components` | Shared OCC, support libraries, tree/ABI and the separately owned Masstree transaction suite |
| `storage` | C++ Masstree and storage runtime tests; C11 ABI header checks |
| `health` | Actual native poison transition with a separate test-only helper |
| `quality` | Runtime/adapter Clippy and compile-fail API doctests |
| `runtime` | Native services and fresh-process integration in debug and release |
| `workloads` | Sharded/colocated transactions, TPC-C, persistence and throttling |
| `audit` | Source/header closure, Cargo archives and all production executable links |
| `srpc` | Native kernel pins, negative controls and copied Cargo-only independence |
| `proofs` | Extended native/independent sharding models, full controls and recovery examples |

Run `BUILD_DIR=build bash ci/native_rust.sh GATE`. The transaction-manager gate
never receives native linkage settings. Adapter and runtime tests explicitly
link storage. Fresh-process runners preserve tests that intentionally fence or
poison process-global state.

`./docker_build.sh ci replication` runs shared storage/Raft component tests and
the distributed RocksDB runtime suite, including one-shard transactions, leader
loss, uncertain outcomes, snapshots and restart.
For sanitizer qualification, use `scripts/ci/run_rust_sto_sanitizer.sh` and
its checked-in workflow. Sanitizer builds require fresh output directories.

CI selects component jobs from their source paths. Shared contract changes also
select the real adapters and affected components. The full integration and
extended correctness jobs run nightly and on demand. The upper development
check remains a separate job and never waits for component compilation.

Performance runs only in the weekly/manual workflow. After compiling the
production binaries, run it locally inside the container with:

```sh
python3 scripts/ci/weekly_performance.py --build build --output build/performance \
  --baseline build/performance-baseline/results.json --update-baseline
```

It warms each topology, records three samples, and retains raw shard metrics,
binary/revision identity and environment metadata. A comparison is valid only
when the environment and workload configuration match. Set the repository
variable `MAKO_PERF_RUNNER` to a dedicated runner label for stable performance
comparisons. The default hosted runner gives diagnostic samples; its shared
hardware does not support a release performance threshold. No performance
command is part of compilation or `ci upper`.

## Launchers and diagnostics

`examples/run_native.py` owns every process it starts. It assigns ephemeral
ports and stores configuration/logs below the selected build directory.
It retains failures. Select `--engine rocksdb` for the `ROCKSDB_RUNTIME` build;
the default `--engine masstree` uses the optional Masstree data layer.
`mako_node`, `mako_bench`, `mako_transactions`, `mako_redis`,
`mako_client` and `mako_admin` expose `--help`.

The compatibility CLI reads the supported legacy topology/configuration
shapes in Rust. It does not load legacy implementation files or use a C++
backend. See [retirement mapping](native-rust-retirement.md) for removed
launchers and their replacement coverage.
