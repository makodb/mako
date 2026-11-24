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
| `proofs` | Native sharding and host Verus gates and negative controls |

Run `BUILD_DIR=build bash ci/native_rust.sh GATE`. The transaction-manager gate
never receives native linkage settings. Adapter and runtime tests explicitly
link storage. Fresh-process runners preserve tests that intentionally fence or
poison process-global state.

For direct RocksDB/Raft, `bash ci/rocksdb_raft.sh` runs native unit tests and
process tests for leader loss, commit boundaries, snapshots and restart.
For sanitizer qualification, use `scripts/ci/run_rust_sto_sanitizer.sh` and
its checked-in workflow. Sanitizer builds require fresh output directories.

## Launchers and diagnostics

`examples/run_native.py` owns every process it starts. It assigns ephemeral
ports and stores configuration/logs below the selected build directory.
It retains failures. `mako_node`, `mako_bench`, `mako_transactions`, `mako_redis`,
`mako_client` and `mako_admin` expose `--help`.

The compatibility CLI reads the supported legacy topology/configuration
shapes in Rust. It does not load legacy implementation files or use a C++
backend. See [retirement mapping](native-rust-retirement.md) for removed
launchers and their replacement coverage.
