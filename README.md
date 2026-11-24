# Mako

Mako runs its transaction manager, sharding and migration, replication host,
RPC services, workloads and command-line programs as native Rust. SRPC builds
through Cargo. Paxos and the C++ application backend have been removed.

C++ remains in Masstree, RocksDB and the storage ABI/runtime required by those
engines. SRPC also has reviewed C platform bindings and context-switch assembly.
The production build audits this boundary, including executable link maps.

## Build and run

Use the build container. The supported native SRPC platform is Linux on x86_64
or aarch64. The workspace pins Rust 1.97.1; the storage build uses Clang/libc++.

```sh
git submodule update --init --recursive
./docker_build.sh build
./docker_build.sh ci nativeRust
```

Inside the container, the equivalent default build is:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
BUILD_DIR=build bash ci/native_rust.sh all
```

`NATIVE_RUST` is the default CMake profile. `ROCKSDB_RAFT` builds the direct
RocksDB service without Masstree. The retired `MAKO` profile is rejected.

Run a two-shard transaction example or a TPC-C workload inside the container:

```sh
python3 examples/run_native.py transactions --build build --shards 2 --workers 3 --count 50
python3 examples/run_native.py tpcc --build build --shards 2 --workers 3 --count 500
python3 examples/run_native.py transactions --build build --shards 2 --replicas 3 --colocated
```

The launcher assigns ports, writes configuration under `build/runs`, owns its
child processes and retains diagnostics on failure. Add `--persistent` for
local disk persistence, or `--replicas 3` for Raft replication. Use `--help` for
configuration, duration and CPU-throttling options. The familiar
`examples/test_*.sh` workload entry points use this launcher.

## Programs

| Program | Purpose |
| --- | --- |
| `mako_node` | Native shard server, optionally colocating shards |
| `mako_bench` | TPC-C and micro workloads and resumable loaders |
| `mako_transactions` | Distributed transaction example and invariant checks |
| `mako_redis` | Redis-compatible server, including transactional MULTI/EXEC |
| `mako_client`, `mako_admin` | Gateway operations and sharding administration |
| `mako-cache-bench`, `engine_compare`, `gc_soak` | Storage/cache measurements |
| `rocksdb_raft_server`, `rocksdb_raft_client`, `rocksdb_raft_bench` | Direct RocksDB/Raft service and measurements |

Each program documents its arguments with `--help`. Runtime configuration and
CLI compatibility live in `crates/mako-runtime/src/configuration.rs` and `configuration/cli.rs`.

## Tests and proofs

Transaction-manager tests use synthetic datatypes and do not link Masstree.
Masstree component tests, application adapters and runtime integration run as
separate gates. Tests that poison process-wide native state run in fresh
processes.

```sh
# Inside the container, after building:
BUILD_DIR=build bash ci/native_rust.sh components
BUILD_DIR=build bash ci/native_rust.sh storage
BUILD_DIR=build bash ci/native_rust.sh runtime
BUILD_DIR=build bash ci/native_rust.sh workloads
BUILD_DIR=build bash ci/native_rust.sh audit
bash ci/native_rust.sh srpc
bash ci/native_rust.sh proofs
bash ci/rocksdb_raft.sh
```

The sanitizer workflow runs ASan, UBSan and TSan separately. ASan/TSan use a
pinned nightly and an instrumented standard library. UBSan instruments the
native code; stable Rust has no UBSan compiler mode. See
[sanitizer instructions](scripts/ci/rust_sto_cache_sanitizers.md).

## Current behavior and limits

- Raft is the supported replication protocol. Unknown installed outcomes fence
  the affected runtime rather than report a successful abort.
- Live migration supports volatile, unreplicated shards. Replicated or locally
  persistent migration is rejected. Reopening migration metadata alone does
  not recover row data.
- The gateway's operation API commits each operation. Its session grouping does
  not undo already committed operations. Redis MULTI/EXEC uses a distributed
  transaction.
- Redis accepts keys up to 8 KiB and values up to 16 MiB in volatile mode,
  stored in bounded transactional chunks. Durable Redis rejects transactions
  above 2 MiB of logical command data before installation. Other transaction
  resource limits still apply.
- Native durable formats are not compatible with the retired C++ WAL. This
  migration does not add a durable distributed coordinator recovery protocol.

See [development](docs/developer/development.md),
[native source retirement and test mapping](docs/developer/native-rust-retirement.md),
and the [implementation plan and evidence](docs/plans/native-rust-production.md).
Historical experiment reports describe their recorded revisions; their
performance figures do not measure the current native Rust application.
