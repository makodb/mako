# Component accounting

Treat the transaction manager as one logical component across single-participant
and multi-participant execution. RocksDB transaction support and Masstree
transaction support each form a separate component. Keep the Masstree index
and its ABI/native support separate from its transaction adapter. Cargo packages and support directories are
accounting units within these components.

List a component separately only when it can be developed, compiled and tested
on its own, with other components accessed through defined interfaces. Record
its owned code, provided/required contracts and local build/test entry points.
Dependencies through those contracts are allowed. A directory, Cargo package
or LOC subtotal alone does not establish a component boundary. Cross-component
integration tests are additional coverage, not the component's only test suite.
The tests/models/tooling row is a shared accounting group, not a component.

The table now has twelve implementation components and one shared support
group. Distributed STO owns reusable transaction behavior, and the node runtime
owns concrete effects. Both deployment sizes use the same OCC+2PC driver.

| Component | What it contains | Source LOC | Total LOC |
|---|---|---:|---:|
| Distributed STO | OCC, object contracts, clocks, generic OCC+2PC, range guards and verified decisions | 22,028 | 34,936 |
| Mako node runtime | Node services, RPC/storage/log effects, migration integration and recovery I/O | 10,999 | 18,792 |
| RocksDB transaction support | Transactional rows, conflict handling, prepare/install and recovery in sto-rocksdb | 3,417 | 4,641 |
| Masstree transaction support and wrapper | Native C++ rows/buffers, thin STO callbacks and safe Rust participant API | 4,270 | 8,264 |
| RocksDB-compatible Rust API | Database, transactions, batches, column families and iterators | 1,486 | 2,640 |
| Sharding and migration | Routing, ownership and range transfer through storage and range-guard contracts | 17,594 | 32,681 |
| Raft and durable log | Consensus, replication host and shared local/replicated journal storage | 16,652 | 24,229 |
| SRPC | RPC transport, serialization, reactor/fibers and platform bindings | 20,946 | 38,648 |
| Masstree | Native concurrent ordered index and its own tests/tools | 24,931 | 33,935 |
| Masstree integration and native support | Rust index wrapper, mtree ABI, allocator, RCU and thread registration | 12,888 | 13,981 |
| RocksDB storage contracts | Atomic byte-store contracts over the maintained rocksdb crate | 603 | 1,165 |
| Rust workloads | TPC-C, micro workload, benchmark configuration/CLI and mako_bench | 2,914 | 4,225 |
| Tests, models and tooling | Cross-component tests, models, benchmarks, automation and historical material | 6,000 | 70,318 |
| **Total** | | 144,728 | 288,455 |

LOC means nonblank lines including comments and inline tests/proofs. Source
excludes separate test, proof and tooling files; Total includes all categories.
External RocksDB is excluded. Workload implementations now count as Source in
their own leaf component; the executable entry point remains Aux. The
[book](docs/mako-book.md#component-size-and-interfaces) and
[inventory](docs/architecture/component-inventory.json) record the rules.

## RocksDB-compatible API boundary

The facade's owned production dependencies are `sto-core`, `mako-runtime`
and `mako-storage-api`. STO supplies wire/journal contracts, and runtime
supplies transaction services.
Routing goes through the transaction layer's routed point and scan methods.
Keep `mako-sharding` as a development dependency for the migration integration
test that sends control requests. Do not turn that test dependency into a
production edge. The book's component diagram distinguishes both paths.
The optional `native-fixtures` feature enables upstream RocksDB only for native
file-format integration tests. Keep it out of the engine-free upper gate and
enable it in native test, quality and sanitizer targets.

## RocksDB transaction component

`sto-rocksdb` owns transactional RocksDB rows, conflict handling, prepared state,
installation and recovery. It depends on `sto-core` and `mako-rocksdb-store`,
with no Masstree, runtime, RPC or Raft dependency. Its owned API includes
`Database`, `Table`, `Worker`, `Transaction`, `Prepared` and `Checkpoint`.
The 961 Source/Total LOC of `mako-storage-rocksdb` remain in the runtime row:
that adapter connects this storage API to `mako-storage-api`, just as
`mako-storage-masstree` connects the Masstree transaction component.

Inside the container, build with `cargo build --locked --manifest-path
crates/Cargo.toml -p sto-rocksdb`. Run its component tests with `cargo test
--locked --manifest-path crates/Cargo.toml -p sto-rocksdb --all-targets
--features test-hooks -- --test-threads=1`. These commands compile RocksDB
and STO dependencies without the node runtime or Masstree.

## Masstree transaction component

`sto-masstree` and `sto-masstree-api` compile and test independently through
`./docker_build.sh ci masstreeTx 32`. The `MASSTREE_TX` CMake profile builds only
these libraries and their core/index dependencies. Its default build runs no
tests; `masstree_tx_tests` runs native row, adapter and ownership tests plus doctests.
Use the fresh-process runner for native cases. The component gate also runs the
wrapper's strict Clippy check and audits declared and compiled dependencies.
RocksDB, SRPC, Raft and the application runtime must remain outside this build.

The adapter implements `sto-core::TObject` locking, validation, installation and
cleanup against native C++ row operations. C++ owns committed buffers, row locks,
versions, histories and allocation/reclamation. Rust holds safe native handles
and forwards the STO contract; STO owns the sole live read/write set. The wrapper exports owned `Database`,
`Worker`, `Table`, `Transaction` and `Participant` APIs, replay and checkpoints.
`mako-storage-masstree` belongs to the runtime and consumes this interface.
The wrapper depends only on `sto-core`, `sto-masstree` and the
Masstree index API. It must not depend on `shard-host-core`,
sharding, runtime, RPC, Raft or workloads. Core OCC tests and
Masstree index tests remain separate suites. See the
[book](docs/mako-book.md#masstree-transaction-component) for commands and boundaries.

The native row accounting unit includes `src/mako/storage/mtree_rows.{cc,h}`,
`crates/masstree/src/native_rows.rs` and its native tests. Count these with the
transaction component even though the safe FFI module compiles in `masstree`.
The generic integer index and RCU/thread ABI remain index dependencies.
The old Rust row registry, alternative layouts, fixed-u64 store, resolved caches
and Rust C storage ABI are retired. Frozen redo comes from STO items and retains
native buffers until serialization. Keep this boundary free of a second
transaction manager or mutation ledger. See the
[native row plan](docs/plans/native-masstree-rows.md).

## Sharding and migration boundary

`mako-sharding` and `shard-host-core` own routing, ownership, admission, migration
actors and storage-host decisions. They have no Cargo dependency on `mako-runtime`,
`sto-core` or the transaction coordinator. They still require the runtime's
transaction concurrency guarantees: publish frozen admission, reject new access
to that range, and drain earlier admitted work before final transfer. Prepared
or unresolved work must retain protection until safely resolved. Migration row
effects use the runtime's transaction locking/validation/commit path. The
application runtime calls their
public routing/admission/control APIs and supplies storage/RPC callbacks through
`HostAbi`, plus the typed `range_lock::{RangeLocks, RangeGuard}` contract.
`Source`, `Store`, `RawStore` and `DurableKv` define
the protocol's storage and persistence contracts.

The shared transaction layer implements range exclusion in
`sto_core::protocol::range_lock`, reexported by `mako-storage-api`. Each runtime Store
owns one range-lock domain for either data engine. Reservations block newly
admitted overlapping access; earlier admission pins may finish. Migration I/O
requires a drained, range- and shard-bound scope. Guard drop never unlocks;
explicit completion follows terminal ownership publication and cleanup.
Recovery invalidates old guard incarnations and reconstructs reservations.
Drain is currently conservative across all earlier shard work.
Run `cargo test --locked --manifest-path crates/Cargo.toml -p sto-core --features protocol --test protocol_range_lock`
in the container for the engine-free contract suite. Sharding owns migration
policy; the shared transaction layer owns exclusion and its local tests.

The following local checks run inside the development container:

```sh
cargo build --locked --manifest-path src/cluster/Cargo.toml --no-default-features --features protocol
cargo test --locked --manifest-path src/cluster/Cargo.toml --no-default-features --features protocol
cargo build --locked --manifest-path crates/Cargo.toml -p shard-host-core
```

The protocol profile excludes RocksDB, Masstree, SRPC, Raft and the transaction
manager/runtime. The full `mako-sharding` package also has local host/metadata
tests, run with `cargo test --locked --manifest-path src/cluster/Cargo.toml`.
That default profile includes its RocksDB metadata adapter and builds RocksDB;
it still does not require the transaction manager or application runtime.
`shard-host-core` is checked by Verus, not executable unit cases. Its proofs and
the sharding proofs run in `ci/upper.sh` and the extended sharding proof gate.

Application-specific migration workers, transactional row effects, logging and
SRPC wiring remain in `crates/mako-runtime/src/migration*.rs`, counted under
node runtime. Those adapters require separate integration tests.
They do not move into the sharding component just because they handle migration.

## Rust workload boundary

`mako-workloads` owns TPC-C, the micro workload and `mako_bench`. No component
may depend on it, including through development/build dependencies or source
includes. `scripts/check_workload_boundary.py` enforces that rule with negative
controls in both application build profiles. `rust_workload_tests` runs its
local/integration suite; performance measurement stays weekly/manual.

## Component ownership

- Distributed STO: `sto-core`.
- Mako node runtime: `mako-runtime`, `mako-storage-api`, `mako-storage-masstree`, `mako-storage-rocksdb`.
- RocksDB transaction support: `sto-rocksdb`.
- Masstree transaction support and wrapper: `sto-masstree`, `sto-masstree-api`, `native-masstree-rows`.
- RocksDB-compatible Rust API: `mako-rocksdb`.
- Sharding and migration: `mako-sharding`, `shard-host-core`.
- Raft and durable log: `raft-core`, `raft-host`, `raft-store`.
- SRPC: `srpc`, `srpc-native`.
- Masstree: `native-masstree`.
- Masstree integration and native support: `masstree`, `mtree-sys`, `native-storage-support`.
- RocksDB storage contracts: `mako-rocksdb-store`.
- Rust workloads: `mako-workloads`.
- Tests, models and tooling: `mako-history`, `sto-test-datatypes`, `raft-replay`, `srpc-bench`, `srpc-rust-source-audit`, `srpc-verify`, `historical-models`, `protocol-models`, `system-tests`, `repository-tooling`.

## Supported storage paths

Keep the distributed RocksDB transaction path, with optional Masstree acceleration.
Both modes use the common OCC + 2PC protocol and local/Raft journal. Masstree
checkpoints rows into RocksDB; recovery replays the committed gap from the log.

The standalone `mako-cache` and `mrx` engines, cache-only `mako-engine*` facades,
`mrxdb` C API, `mtx` ABI, cache-only clock sequence, tools and CI entry points
are retired. Shared byte-store contracts moved into
[`mako-rocksdb-store`](crates/mako-rocksdb-store/src/bytes.rs), and the timestamp type
moved into [`sto_core::clock`](crates/sto-core/src/clock/timestamp.rs). No cache feature or
cache engine remains in the active Cargo graph. Historical cache reports record
earlier revisions. See the [retirement plan](docs/plans/retire-standalone-caches.md).

A one-shard deployment uses the same distributed runtime and transaction
protocol as a multi-shard deployment. The separate RocksDB/Raft service,
RPC/C client API and native optimistic benchmark wrapper are retired. See the
[service retirement plan](docs/plans/retire-rocksdb-raft-service.md).

The row, log and migration-metadata adapters share `rocksdb = 0.25.0` and its
pinned native RocksDB 11.8.1. Keep native ownership and FFI in upstream. Preserve
Mako durability, recovery and snapshot contracts in the small safe adapters.

## Core and runtime test boundaries

Test shared OCC through `cargo test --locked --manifest-path crates/Cargo.toml
-p sto-core -p sto-test-datatypes`. Verify shared protocol decisions through
`ci/upper.sh`, using model storage and transport contracts. The runtime composes
those contracts with real components; its integration tests run separately.
These commands run inside the development container.

Refresh the ledger with `python3 scripts/component_inventory.py --write`, then
recompute the logical table from the ownership list. `--check` validates the
JSON ledger and generated book inventory, not the logical snapshot tables.

## Distributed STO boundary

`sto-core` owns OCC, retained participants, clocks, generic OCC+2PC coordination,
journal/wire contracts and verified transaction decisions. `protocol` enables
network protocol types and the driver; storage components need only
`distributed`. Neither feature imports runtime, sharding, RPC, Raft or an engine.
`mako-runtime` provides node services and concrete effects, including gateway
routing policy. Raft owns log retention and has no STO dependency.

The former `mako-clock`, `mako-protocol-core` and `mako-protocol` packages are
retired. Do not recreate wrapper packages under those names. Keep canonical
verified files under `sto-core/src/protocol/verified`; preserve the source audit
and semantic controls. Historical scalar coordinator coverage lives under
`sto-core/verification` and must stay outside production exports. Run
`python3 scripts/upper_check.py --cpus 32` for the engine-free gate and
`python3 scripts/check_sto_boundary.py` for dependency controls, inside the
container. Full STO tests use `-p sto-core --all-features`; native storage tests
remain separate and use the fresh-process runner.

## Storage format compatibility

Support the current native RocksDB user-family layout and local/Raft journal.
Do not retain old Mako format readers or upgrade tools during this development
phase. Ordinary RocksDB adoption and restarting with Masstree enabled or disabled
remain supported. Keep rejection checks for unsupported or damaged directories;
never silently recreate their data or log.
