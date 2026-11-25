# Native Rust development

Mako's default production build is native Rust. Start with the repository
[README](../../README.md) for container setup, programs and supported behavior.

## Build profiles

Inside the container:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
```

`MAKO_BUILD_PROFILE=NATIVE_RUST` compiles the 16 explicitly listed Masstree
storage support files and builds the Rust applications with Cargo. The native
archive is linked explicitly; Rust archives must not bundle C++ objects.
`MAKO_BUILD_PROFILE=ROCKSDB_RAFT` builds the direct RocksDB service.

Use a separate build directory for storage's C++ component tests:

```sh
cmake -S . -B build_storage -G Ninja -DCMAKE_BUILD_TYPE=Release -DMAKO_STORAGE_TESTS_ONLY=ON
cmake --build build_storage --parallel 4
ctest --test-dir build_storage --output-on-failure
```

The production source audit deliberately excludes these test objects.
`BUILD_DIR=build bash ci/native_rust.sh storage` manages the same separate
profile under `build/storage-tests`.

## Test selection

For distributed transaction, sharding and migration work, use:

```sh
./docker_build.sh ci upper
```

This runs the production Verus proofs, selected semantic negative controls and
small deterministic scenarios. It uses four logical CPUs and has a five-minute
wall deadline. It does not compile Masstree, RocksDB, SRPC or Raft. Storage,
local transaction effects, delivery, logging and time come through the explicit
[upper-layer contracts](upper-layer-contracts.md). Production adapters call the
same core functions as this suite.

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
| `components` | Independent STO, support libraries, tree/ABI, adapters and cache |
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

For direct RocksDB/Raft, `bash ci/rocksdb_raft.sh` runs native unit tests and
process tests for leader loss, commit boundaries, snapshots and restart.
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
It retains failures. `mako_node`, `mako_bench`, `mako_transactions`, `mako_redis`,
`mako_client` and `mako_admin` expose `--help`.

The compatibility CLI reads the supported legacy topology/configuration
shapes in Rust. It does not load legacy implementation files or use a C++
backend. See [retirement mapping](native-rust-retirement.md) for removed
launchers and their replacement coverage.
