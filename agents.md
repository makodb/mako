# Component accounting

Treat the transaction manager as one logical component across single-participant
and multi-participant execution. Masstree transaction support and its wrapper are
a separate component. Keep the Masstree index and its ABI/native support separate
from that transaction adapter. Cargo packages and support directories are
accounting units within these components.

List a component separately only when it can be developed, compiled and tested
on its own, with other components accessed through defined interfaces. Record
its owned code, provided/required contracts and local build/test entry points.
Dependencies through those contracts are allowed. A directory, Cargo package
or LOC subtotal alone does not establish a component boundary. Cross-component
integration tests are additional coverage, not the component's only test suite.
The tests/models/tooling row is a shared accounting group, not a component.

After the standalone cache/service retirement and Masstree transaction split, the list contains nine
implementation components and one shared support group, covering 27 Cargo packages and
7 support accounting units. Counts below were recomputed from the current
source inventory.

This table is a snapshot of the
[component inventory](docs/architecture/component-inventory.json). The
[book](docs/mako-book.md#component-size-and-interfaces) describes the measurement
rules, interfaces and proposed size review thresholds.

| Component | What it contains | Source LOC | Total LOC |
|---|---|---:|---:|
| Transaction manager and runtime | Shared OCC/2PC, range guards, RocksDB transaction adapter and node runtime | 38,766 | 63,877 |
| Masstree transaction support and wrapper | Versioned rows/scans, transactional index adapter, owned Rust API and retained native ABI | 37,703 | 50,837 |
| RocksDB-compatible Rust API | Application-facing database, transactions, batches, column families and iterators | 1,479 | 2,513 |
| Sharding and migration | Routing, ownership, admission and range transfer through storage and range-guard interfaces | 17,594 | 32,450 |
| Raft and durable log | Consensus, replication host and shared local/replicated journal storage | 16,616 | 24,154 |
| SRPC | RPC transport, connections, serialization, reactor/fibers and platform bindings | 20,946 | 38,648 |
| Masstree | Native concurrent ordered index and its own tests/tools | 24,931 | 33,935 |
| Masstree integration and native support | Rust index wrapper, mtree ABI, allocator, RCU and thread registration | 12,693 | 13,773 |
| RocksDB storage contracts | Atomic byte-store contracts and policy adapter over the maintained rocksdb crate | 554 | 1,116 |
| Tests, models and tooling | Cross-component tests, verification models, benchmarks, automation and historical material | 6,000 | 71,852 |
| **Total across all components** |  | **177,282** | **333,155** |

LOC means nonblank lines, including comments. Source excludes separate test,
proof and tooling files but includes inline tests and proofs. Total includes all
counted files. External RocksDB implementation code is excluded. The transaction
manager row includes the entire runtime crate, including gateway, configuration
and migration glue. Package ownership determines the sums; a row is not a
measurement of an isolated executable's linked code.

## Reading the transaction count

The 177,282 Source LOC total covers every component in the table. The transaction
manager/runtime group now accounts for 38,766 Source LOC and 63,877 Total LOC. It
includes shared OCC, the RocksDB adapter and application runtime, beyond the protocol
state machines. The backend-independent `mako-protocol` and `mako-protocol-core`
crates together account for 3,241 Source LOC; they depend on the other transaction
code to enforce concurrency, apply data and recover durable decisions.

Its largest contributors are `sto-core` at 18,223 Source LOC and `mako-runtime`
at 11,730. The separate Masstree transaction component owns `sto-masstree` at
23,166 and `sto-tpcc-ffi` at 14,537. Source also includes inline tests: the terminal test modules in
`sto-masstree/src/lib.rs` and `sto-tpcc-ffi/src/lib.rs` alone contribute 12,468
nonblank lines. Their tests remain useful; moving them out would change Source
classification without reducing Total LOC.

This refresh corrects two standalone `tests.rs` files previously classified as
Source: 658 lines in `sto-masstree` and 413 in `mako-rocksdb`. That is an accounting
correction, with no production code deletion. See the
[transaction breakdown](docs/mako-book.md#why-the-transaction-group-is-large)
for responsibilities and candidates for simplification. The split changes
ownership and build/test boundaries; it does not delete transaction functionality.

## Masstree transaction component

`sto-masstree` and `sto-tpcc-ffi` compile and test independently through
`./docker_build.sh ci masstreeTx 32`. The `MASSTREE_TX` CMake profile builds only
these libraries and their core/index dependencies. Its default build runs no
tests; `masstree_tx_tests` runs local tests and doctests, including `fixed-u64`.
Use the fresh-process runner for native cases. The component gate also runs the
wrapper's strict Clippy check and audits declared and compiled dependencies.
RocksDB, SRPC, Raft and the application runtime must remain outside this build.

The adapter implements `sto-core::TObject` locking, validation, installation and
cleanup against the `masstree` index API. The wrapper exports owned `Database`,
`Worker`, `Table`, `Transaction` and `Participant` APIs, replay and checkpoints.
`mako-storage-masstree` belongs to the runtime and consumes this interface.
Pure `mako-protocol-core` and `shard-host-core` decision contracts are shared
dependencies; the full protocol/sharding runtime is not. Core OCC tests and
Masstree index tests remain separate suites. See the
[book](docs/mako-book.md#masstree-transaction-component) for commands and boundaries.

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
`mako-protocol::range_lock`, reexported by `mako-storage-api`. Each runtime Store
owns one range-lock domain for either data engine. Reservations block newly
admitted overlapping access; earlier admission pins may finish. Migration I/O
requires a drained, range- and shard-bound scope. Guard drop never unlocks;
explicit completion follows terminal ownership publication and cleanup.
Recovery invalidates old guard incarnations and reconstructs reservations.
Drain is currently conservative across all earlier shard work.
Run `cargo test --locked --manifest-path crates/Cargo.toml -p mako-protocol --test range_lock`
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
transaction manager/runtime. Those adapters require separate integration tests.
They do not move into the sharding component just because they handle migration.

## Package ownership for this snapshot

Each inventory entry belongs to exactly one row:

- Transaction manager and runtime: `mako-protocol-core`, `mako-protocol`, `mako-runtime`, `mako-clock`, `mako-storage-api`, `mako-storage-masstree`, `mako-storage-rocksdb`, `sto-core`, `sto-rocksdb`.
- Masstree transaction support and wrapper: `sto-masstree`, `sto-tpcc-ffi`.
- RocksDB-compatible Rust API: `mako-rocksdb`.
- Sharding and migration: `mako-sharding`, `shard-host-core`.
- Raft and durable log: `raft-core`, `raft-host`, `raft-store`.
- SRPC: `srpc`, `srpc-native`.
- Masstree: `native-masstree`.
- Masstree integration and native support: `masstree`, `mtree-sys`, `native-storage-support`.
- RocksDB storage contracts: `mako-rocksdb-store`.
- Tests, models and tooling: `historical-models`, `mako-history`, `protocol-models`, `raft-replay`, `repository-tooling`, `srpc-bench`, `srpc-rust-source-audit`, `srpc-verify`, `sto-test-datatypes`, `system-tests`.

Refresh the ledger and book tables with
`python3 scripts/component_inventory.py --write` in the development container.
Then recompute this table using the ownership list above. The script's `--check`
checks the ledger and generated book tables; it does not check this snapshot.

## Supported storage paths

Keep the distributed RocksDB transaction path, with optional Masstree acceleration.
Both modes use the common OCC + 2PC protocol and local/Raft journal. Masstree
checkpoints rows into RocksDB; recovery replays the committed gap from the log.

The standalone `mako-cache` and `mrx` engines, cache-only `mako-engine*` facades,
`mrxdb` C API, `mtx` ABI, cache-only clock sequence, tools and CI entry points
are retired. Shared byte-store contracts moved into
[`mako-rocksdb-store`](crates/mako-rocksdb-store/src/bytes.rs), and the timestamp type
moved into [`mako-clock`](crates/mako-clock/src/timestamp.rs). No cache feature or
cache engine remains in the active Cargo graph. Historical cache reports record
earlier revisions. See the [retirement plan](docs/plans/retire-standalone-caches.md).

A one-shard deployment uses the same distributed runtime and transaction
protocol as a multi-shard deployment. The separate RocksDB/Raft service,
RPC/C client API and native optimistic benchmark wrapper are retired. See the
[service retirement plan](docs/plans/retire-rocksdb-raft-service.md).

The row, log and migration-metadata adapters share `rocksdb = 0.25.0` and its
pinned native RocksDB 11.8.1. Keep native ownership and FFI in upstream. Preserve
Mako durability, recovery and snapshot contracts in the small safe adapters.
