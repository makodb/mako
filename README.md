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

`NATIVE_RUST` is the default CMake profile. `ROCKSDB_RUNTIME` builds the same
distributed runtime without Masstree. Both support one shard or multiple shards
with local or replicated logging. The separate RocksDB/Raft service is retired.
All persistent adapters use the maintained `rocksdb` crate and its pinned native
RocksDB build. See [dependency ownership](docs/mako-book.md#rocksdb-dependency-ownership).

For everyday upper-layer protocol work, run `./docker_build.sh ci upper`.
It verifies distributed transaction, sharding and migration decisions against
small backend contracts without building the physical engines, RPC or Raft.
The check has a five-minute deadline; component/integration checks and weekly
performance have separate commands. See [development](docs/developer/development.md).

Run a two-shard transaction example or a TPC-C workload inside the container:

```sh
python3 examples/run_native.py transactions --build build --shards 2 --workers 3 --count 50
python3 examples/run_native.py tpcc --build build --shards 2 --workers 3 --count 500
python3 examples/run_native.py transactions --build build --shards 2 --replicas 3 --colocated
```

The launcher assigns ports, writes configuration under `build/runs`, owns its
child processes and retains diagnostics on failure. Local RocksDB logging is
the default; use `--replicas 3` for Raft or `--volatile` for development. Use `--help` for
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

Each program documents its arguments with `--help`. Runtime configuration and
CLI compatibility live in `crates/mako-runtime/src/configuration.rs` and `configuration/cli.rs`.

## Tests and proofs

Transaction-manager tests use synthetic datatypes and do not link Masstree.
Masstree index tests, Masstree transaction support and runtime integration run as
separate gates. Tests that poison process-wide native state run in fresh
processes.

The Masstree transaction component (`sto-masstree` and `sto-tpcc-ffi`) has its
own build and local tests: `./docker_build.sh ci masstreeTx 32`. It compiles no
RocksDB, Raft, SRPC or application runtime. See the
[component profile](docs/developer/development.md#masstree-transaction-component)
for compilation without tests and individual targets.

```sh
# Inside the container, after building:
BUILD_DIR=build bash ci/native_rust.sh components
BUILD_DIR=build bash ci/native_rust.sh storage
BUILD_DIR=build bash ci/native_rust.sh runtime
BUILD_DIR=build bash ci/native_rust.sh workloads
BUILD_DIR=build bash ci/native_rust.sh audit
bash ci/native_rust.sh srpc
bash ci/native_rust.sh proofs
bash ci/replication.sh
```

The sanitizer workflow runs ASan, UBSan and TSan separately. ASan/TSan use a
pinned nightly and an instrumented standard library. UBSan instruments the
native code; stable Rust has no UBSan compiler mode. See
[sanitizer driver and coverage contract](scripts/ci/run_rust_sto_sanitizer.sh).

## Current behavior and limits

- Distributed transactions use OCC + 2PC: acquire every write lock before
  final validation, save one coordinator decision, then complete every member.
  Resolve Unknown by transaction ID; never rerun its body. Raft is the supported
  replication protocol. `MemoryLog`, `LocalLog` and `ReplicatedLog` share one
  transaction-log interface. Local and replicated logs use the same RocksDB
  format. Production selects local persistence or Raft; `MemoryLog` is for tests
  and requires `volatile: true` in native node configuration.
- Live migration uses shared transactional range guards with either data engine.
  Durable migration requires synchronous LocalLog or ReplicatedLog recovery of
  rows, prepared transactions and control state. Reopening migration metadata
  alone does not recover row data.
- The gateway's operation API commits each operation. Its session grouping does
  not undo already committed operations. Redis MULTI/EXEC uses a distributed
  transaction.
- Redis accepts keys up to 8 KiB and values up to 16 MiB in volatile mode,
  stored in bounded transactional chunks. Durable Redis rejects transactions
  above 2 MiB of logical command data before installation. Other transaction
  resource limits still apply.
- Native durable formats are not compatible with the retired C++ WAL. Durable
  prepare, decision and completion records support transaction recovery before
  new admission. The shared log format requires fresh log directories; legacy
  segmented logs and applied-only local stores are refused intact pending offline
  conversion. Mixed old/new transaction-protocol operation is unsupported.
  See [the book](docs/mako-book.md) for durability and recovery limits.

See [development](docs/developer/development.md),
[native source retirement and test mapping](docs/developer/native-rust-retirement.md),
and the [implementation plan and evidence](docs/plans/native-rust-production.md).
Historical experiment reports describe their recorded revisions; their
performance figures do not measure the current native Rust application.
