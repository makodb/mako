# The Mako book

Mako's application runtime is Rust. The outer protocol combines distributed
transactions, sharding and range migration. The distributed transaction protocol
uses optimistic concurrency control and two-phase commit. It acquires every
participant's write locks before final read validation, saves one coordinator
decision, and then completes that decision at each participant.

C++ remains in Masstree, RocksDB and their storage ABI/runtime dependencies.
SRPC builds as Rust with its platform C and assembly bindings. Paxos and the C++
application backend are retired. Historical performance reports describe their
recorded revisions and do not measure this protocol.

The [OCC + 2PC plan](plans/occ-2pc.md) records implementation and qualification
status. The [upper-layer contracts](developer/upper-layer-contracts.md) distinguish
verified decisions from assumptions about storage, RPC and replication.

## Code map

The transaction manager uses the same OCC + 2PC driver for application
transactions with one participant and transactions with several participants.
Distributed STO owns OCC, clocks, verified decisions and the generic coordinator.
The node runtime supplies concrete effects and services.
Masstree transaction support and its wrapper form a separate component above
the index. The rows below locate implementation responsibilities; not every
crate is a separate logical component.

| Responsibility area | Code | Responsibility |
| --- | --- | --- |
| Distributed protocol decisions | `crates/sto-core/src/protocol/verified` | Verus-checked admission, write-lock barrier, prepare votes and immutable decisions |
| Protocol composition | `crates/sto-core/src/protocol` | Coordinator driver, journal transitions, identities, wire encoding and transport contracts |
| Rust RocksDB API | `crates/mako-rocksdb` | Compatible transaction API subset, column families, atomic batches, bounded iterators and saved-result resolution |
| Runtime | `crates/mako-runtime` | Native adapters, service-owned participant workers, client, gateway, process ownership and recovery |
| Sharding and migration | `src/cluster` | Immutable routing snapshots, admission, range transfer, receipts and publication |
| Storage-host decisions | `crates/shard-host-core` | Checked translation between migration effects and physical storage |
| Transaction manager: shared OCC machinery | `crates/sto-core` | Read/write sets, locking, validation and terminal cleanup shared by participant execution |
| Masstree transaction support and wrapper | `crates/sto-masstree`, `crates/sto-masstree-api` | Masstree adapter, versioned rows, scans, retained affine workers and owned redo |
| Log persistence | `src/deptran/raft/store/src/journal.rs` | Shared RocksDB entries, commit prefix, identity and snapshot format for local and Raft ordering |
| Applied storage | `crates/mako-storage-rocksdb`, `crates/sto-rocksdb`, runtime `log/materialization.rs` | Common RocksDB rows and protocol state, direct or batched from the optional Masstree layer |
| Replication | `crates/raft-host`, `src/deptran/raft` | Raft authority, committed prefixes, read barriers, snapshots and recovery |
| RPC | `src/srpc`, runtime `rpc.rs` | Rust transport, reactor, bounded owned requests and pending-reply cancellation |
| Rust workloads | `crates/mako-workloads` | TPC-C, micro workload, benchmark CLI/configuration and executable; no incoming production dependency |

The outer protocol's development gate does not build Masstree, RocksDB, SRPC or
Raft. Their contracts have small test implementations. Component and native
integration tests run separately.

## Component size and interfaces

This census is the baseline for deciding where to split components and where to
reduce their dependencies. A Cargo package is an enforced Rust boundary. Native
storage, platform bindings, models and repository tooling have separate accounting
buckets. These are accounting units within the logical components. In
particular, distributed STO and node orchestration have distinct
build/test boundaries. Single- and multi-participant execution use the same
protocol. A
directory inside a crate is only a possible extraction boundary.

For the logical component list, independent development and testing are the
criterion: a component must compile and run its own tests through defined
interfaces to its dependencies. Record its owned code, provided and required
contracts, and local build/test entry points. Package counts and LOC groupings
alone do not establish this property. Integration tests check composition in
addition to component tests. The shared tests/models/tooling group is an
accounting category rather than an implementation component.

### Sharding and migration boundary

The sharding component owns `mako-sharding` and `shard-host-core`. Neither has a
Cargo dependency on the transaction manager or application runtime. The runtime consumes public
routing, admission and migration-control APIs. Storage and transport effects
cross explicit contracts:

| Contract | Required behavior |
|---|---|
| `storage::Source` and `storage::Store`, `transfer::RawStore` | Ordered range scans, exact EOF and scoped row effects under migration authorization |
| `durable_kv::DurableKv` | Atomic metadata batches, explicit uncertain outcomes and settlement before recovery admission |
| `host::HostAbi` | Thread lifetime, table access, row scans/mutations and peer calls |
| `range_lock::RangeLocks` and `RangeGuard` | Reserve a migration range, drain prior transactions, authorize bounded migration I/O and explicitly finish after terminal publication/cleanup |

`HostAbi` currently uses a C-compatible callback layout supplied by Rust runtime
adapters. This is a defined interface, though its unsafe lifetime requirements
and broad operation set remain candidates for improvement. The sharding library
does not import the implementations behind those callbacks.

Sharding still depends on transaction concurrency guarantees through this
interfaces. Publishing frozen admission and reserving the range prevent new
access to the migrating range. A range guard waits for earlier admitted work
before final transfer; prepared or unresolved work must retain protection until
safely resolved. Physical migration writes require a guard scope and use the
runtime's ordinary participant locking, validation and commit path. Isolated
sharding proofs assume these contracts. The shared range-lock tests check the
implementation without native engines; adapter/integration tests check wiring.

Compile and test its protocol profile independently inside the container:

```sh
cargo build --locked --manifest-path src/cluster/Cargo.toml --no-default-features --features protocol
cargo test --locked --manifest-path src/cluster/Cargo.toml --no-default-features --features protocol
cargo build --locked --manifest-path crates/Cargo.toml -p shard-host-core
```

This profile uses no RocksDB, Masstree, SRPC, Raft or transaction-manager/runtime
implementation. `ci/upper.sh` separately runs the same sharding scenarios and
production Verus proofs with dependency audits. `shard-host-core` uses Verus
proofs for correctness checks; plain Cargo test has no executable cases.
Testing the default
`mako-sharding` profile also exercises its host and RocksDB metadata code without
the transaction manager/runtime. Its existing 69-test qualification is recorded
in [the metadata log](testing/maintained-rocksdb/metadata.log).

Application integration remains in `mako-runtime/src/migration.rs`,
`migration_storage.rs` and `migration_metadata.rs`. Those modules bind the
contracts to transaction workers, durable decisions and SRPC. Their code stays
in the transaction manager/runtime count and needs integration coverage. The
17,594 Source LOC sharding subtotal includes the library's own host/metadata
code as well as the protocol and shared decision core.

### Measurement rules

The tables count tracked files in this checkout. **NLOC means nonblank physical
source lines, including comments.** Source includes inline tests and inline Verus
specifications and proofs. Tests counts separately identified test files; Proofs
counts separately identified proof and model files. Neither column is a measure
of coverage. Aux includes executable entry points, examples, benchmark harnesses, build
scripts and automation. The Rust workload library counts as Source in its own
component; moving it out of the runtime changes that classification. Gen counts files whose first line marks them as generated.
These categories partition the counted files, so their sum has no duplicates.

This conservative size measure includes the contracts and explanations a reader
must understand. It does not estimate compiled instructions or executable-only
LOC. In particular, moving an inline test into a test file changes the category
without reducing the total. Review both Source and the total before claiming a
size reduction. The classification rules are in
[`component_inventory.py`](../scripts/component_inventory.py); the
[JSON ledger](architecture/component-inventory.json) records every counted file,
its owner, category, physical lines and nonblank lines.

The census includes Rust, C/C++, headers, assembly, Python, shell, TLA and build
source. It excludes build outputs, data, logs, lockfiles, configuration and prose.
Documentation source copies and `third-party` source are explicitly excluded in
the ledger. The vendored `scripts/pprof` utility is outside the owned-source
census. Submodule contents are not traversed. RocksDB, Lion, rusty-cpp storage
utilities, GoogleTest, Verus and registry dependencies have their own upstream
size budgets; their code is not counted as maintained in this checkout. Cargo manifests
supply dependency edges but are not included in source LOC.

Out / in counts distinct owned Cargo packages directly depended on / directly
depending on this package through normal dependencies. It takes the union of
optional features and target-specific dependencies, not one executable's link
closure. Development and build dependencies are separate entries in the ledger.
Native linkage, callbacks, wire messages and shared source imports need the
additional contract map below. A zero here does not mean an isolated component.

Pub hits is a source-level count of unrestricted Rust `pub` declaration lines in
Source files. It includes methods, types, constants, modules and reexports, and
excludes `pub(crate)`. It is a trend indicator, not an exact exported-symbol
count. Private enclosing modules, cfg gates, macros, trait methods, public fields
and proof code prevent that interpretation. Contract operation counts provide a
more useful measure for selected adapter interfaces.

### Component dependencies

`A --> B` means A uses B. Optional engines/logs are shown together. The
component ownership graph and the Cargo graph are acyclic. `sto-core` is the
distributed transaction engine. `mako-runtime` connects it to node services and
concrete effects. `sto_core::Runtime` is the OCC owner inside that engine.

```text
Rust workloads (TPC-C / micro / mako_bench) [leaf application]
 +--> Mako node runtime
 |     +--> Distributed STO [sto-core]
 |     +--> Sharding and migration
 |     |     `--> upstream rocksdb crate [metadata, engine-host profile]
 |     +--> SRPC
 |     +--> Raft and durable log
 |     |     +--> SRPC
 |     |     `--> upstream rocksdb crate --> RocksDB
 |     +--> RocksDB transaction support
 |     |     +--> Distributed STO
 |     |     `--> RocksDB storage contracts
 |     |           `--> upstream rocksdb crate --> RocksDB
 |     `--> Masstree transaction support and wrapper [optional]
 |           +--> Distributed STO
 |           `--> Masstree integration and native support --> Masstree
 `--> SRPC [workload process helpers]

RocksDB-compatible Rust API --> Mako node runtime
                           `--> Distributed STO [wire/journal contracts]
```

STO has no production dependency on the node runtime, sharding, Raft, SRPC,
Masstree or RocksDB. Its storage contracts and the protocol's `Effects`,
`Transport` and `Journal` interfaces let tests substitute small implementations.
The runtime implements these effects and supplies sharding range guards and
migration I/O. Raft owns checkpoint-based log retention and does not import STO.

The generic Masstree wrapper supplies owned handles, row operations and retained
participant phases. `mako-storage-masstree` binds that API to the node's storage
contracts. Its only engine dependency is STO. The RocksDB wrapper follows the
same direction. Neither wrapper imports node services or workload code.

The RocksDB facade uses `mako-runtime`, `mako-storage-api` and STO's protocol
contracts. Routed reads/writes/scans use the transaction interface. Its migration
integration test alone imports sharding control request types. Tests, models and
tooling are outside this production graph.

### Distributed STO and node runtime

`sto-core` combines the former clock, verified decision and protocol packages
with the transaction engine. There are no compatibility crates under the old
names. The engine owns the reusable OCC+2PC driver, global lock barrier,
admission and completion decisions, range exclusion, clocks, journal state
transitions, transport records and object-level locking/validation/installation.
The same driver handles one participant and many participants.

`mako-runtime` owns workers and process lifetimes, RPC servers and clients,
gateway routing policy, migration integration, concrete storage adapters,
LocalLog/ReplicatedLog effects and checkpoint/recovery I/O. For example, STO
asks `Effects::prepare` for a vote; the runtime sends the RPC, invokes a storage
participant and persists its prepare record. The runtime does not define a
second generic transaction coordinator.

The `distributed` feature provides retained participant and timestamp support.
The `protocol` feature adds coordinator, journal and wire modules and enables
`distributed`. Storage component builds use the former without pulling in
serialization or node services. `clock-test-hooks` and `test-install-observer`
are test-only features. `scripts/check_sto_boundary.py` rejects forbidden
production dependencies and is part of the upper gate.

Verus checks canonical production files under
`sto-core/src/protocol/verified`; Cargo compiles those same files. This verifies
specified decision functions, not the entire Rust engine or runtime. The
obsolete scalar coordinator is retained under `sto-core/verification` as a
historical test/proof target, with its four semantic controls. It is absent from
production module exports. Raft's canonical retention rule has its own proof
target and publication-order mutation control. The gateway's engine-free test
harness includes the canonical runtime source and shares the runtime's test
cases, so moving ownership does not replace it with a model.

Run the fast protocol checks in the container with
`python3 scripts/upper_check.py --cpus 32`. Run full STO component tests with
`cargo test --locked --manifest-path crates/Cargo.toml -p sto-core --all-features`.
Backend tests remain separate. The clock mutation runner is now
`crates/sto-core/clock-mutations/run.py`; it records three retired cache-sequence
controls separately from active HLC controls.

<!-- component-inventory:start -->

Counted 33 accounting units across 1,026 owned source files:
**288,455 nonblank lines**
and 309,634 physical lines including blanks. Excluded
153 tracked source files under documentation or third-party paths.

| Component | Total | Source | Tests | Proofs | Aux | Gen | Out / in | Pub hits |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `historical-models` | 12,119 | 0 | 0 | 10,787 | 1,332 | 0 | 0 / 0 | 0 |
| `mako-history` | 4,856 | 3,549 | 1,307 | 0 | 0 | 0 | 0 / 0 | 80 |
| `mako-rocksdb` | 2,640 | 1,486 | 1,125 | 0 | 29 | 0 | 3 / 0 | 99 |
| `mako-rocksdb-store` | 1,165 | 603 | 562 | 0 | 0 | 0 | 0 / 1 | 27 |
| `mako-runtime` | 16,272 | 9,405 | 6,383 | 0 | 484 | 0 | 9 / 2 | 168 |
| `mako-sharding` | 31,787 | 16,700 | 2,134 | 12,847 | 106 | 0 | 0 / 1 | 1,046 |
| `mako-storage-api` | 1,312 | 386 | 926 | 0 | 0 | 0 | 1 / 5 | 51 |
| `mako-storage-masstree` | 247 | 247 | 0 | 0 | 0 | 0 | 3 / 1 | 2 |
| `mako-storage-rocksdb` | 961 | 961 | 0 | 0 | 0 | 0 | 2 / 1 | 5 |
| `mako-workloads` | 4,225 | 2,914 | 1,242 | 0 | 69 | 0 | 3 / 0 | 106 |
| `masstree` | 4,095 | 3,014 | 620 | 0 | 461 | 0 | 1 / 2 | 109 |
| `mtree-sys` | 495 | 495 | 0 | 0 | 0 | 0 | 0 / 1 | 133 |
| `native-masstree` | 33,935 | 24,931 | 7,983 | 0 | 1,021 | 0 | 0 / 0 | 0 |
| `native-masstree-rows` | 2,181 | 1,736 | 445 | 0 | 0 | 0 | 0 / 0 | 53 |
| `native-storage-support` | 9,391 | 9,379 | 0 | 0 | 12 | 0 | 0 / 0 | 0 |
| `protocol-models` | 18,907 | 0 | 0 | 18,242 | 665 | 0 | 0 / 0 | 0 |
| `raft-core` | 14,278 | 9,060 | 1,020 | 4,198 | 0 | 0 | 0 / 2 | 439 |
| `raft-host` | 5,341 | 4,199 | 1,142 | 0 | 0 | 0 | 3 / 1 | 51 |
| `raft-replay` | 1,305 | 1,165 | 140 | 0 | 0 | 0 | 1 / 0 | 17 |
| `raft-store` | 4,610 | 3,393 | 1,217 | 0 | 0 | 0 | 0 / 1 | 181 |
| `repository-tooling` | 21,125 | 0 | 1,255 | 0 | 19,870 | 0 | 0 / 0 | 0 |
| `shard-host-core` | 894 | 894 | 0 | 0 | 0 | 0 | 0 / 1 | 66 |
| `srpc` | 37,694 | 19,992 | 16,358 | 0 | 1,228 | 116 | 0 / 4 | 1,205 |
| `srpc-bench` | 304 | 0 | 0 | 0 | 304 | 0 | 1 / 0 | 0 |
| `srpc-native` | 954 | 954 | 0 | 0 | 0 | 0 | 0 / 0 | 0 |
| `srpc-rust-source-audit` | 279 | 0 | 0 | 0 | 279 | 0 | 0 / 0 | 0 |
| `srpc-verify` | 502 | 0 | 0 | 502 | 0 | 0 | 0 / 0 | 0 |
| `sto-core` | 34,936 | 22,028 | 11,802 | 201 | 905 | 0 | 0 / 8 | 707 |
| `sto-masstree` | 4,089 | 1,397 | 2,375 | 0 | 317 | 0 | 2 / 1 | 56 |
| `sto-masstree-api` | 1,994 | 1,137 | 846 | 0 | 11 | 0 | 3 / 1 | 57 |
| `sto-rocksdb` | 4,641 | 3,417 | 1,224 | 0 | 0 | 0 | 2 / 2 | 111 |
| `sto-test-datatypes` | 2,882 | 1,286 | 1,596 | 0 | 0 | 0 | 1 / 0 | 45 |
| `system-tests` | 8,039 | 0 | 8,039 | 0 | 0 | 0 | 0 / 0 | 0 |
| **Total** | **288,455** | **144,728** | **69,741** | **46,777** | **27,093** | **116** | | |

#### Declared local production dependencies

These are direct normal Cargo dependencies across all declared features and targets.
The JSON also lists external, build and development dependencies, with optional flags.

| Consumer | Owned dependencies |
| --- | --- |
| `mako-history` | None |
| `mako-rocksdb` | `mako-runtime`, `mako-storage-api`, `sto-core` |
| `mako-rocksdb-store` | None |
| `mako-runtime` | `mako-sharding`, `mako-storage-api`, `mako-storage-masstree`, `mako-storage-rocksdb`, `raft-host`, `shard-host-core`, `srpc`, `sto-core`, `sto-rocksdb` |
| `mako-sharding` | None |
| `mako-storage-api` | `sto-core` |
| `mako-storage-masstree` | `mako-storage-api`, `sto-core`, `sto-masstree-api` |
| `mako-storage-rocksdb` | `mako-storage-api`, `sto-rocksdb` |
| `mako-workloads` | `mako-runtime`, `mako-storage-api`, `srpc` |
| `masstree` | `mtree-sys` |
| `mtree-sys` | None |
| `raft-core` | None |
| `raft-host` | `raft-core`, `raft-store`, `srpc` |
| `raft-replay` | `raft-core` |
| `raft-store` | None |
| `shard-host-core` | None |
| `srpc` | None |
| `srpc-bench` | `srpc` |
| `srpc-rust-source-audit` | None |
| `srpc-verify` | None |
| `sto-core` | None |
| `sto-masstree` | `masstree`, `sto-core` |
| `sto-masstree-api` | `masstree`, `sto-core`, `sto-masstree` |
| `sto-rocksdb` | `mako-rocksdb-store`, `sto-core` |
| `sto-test-datatypes` | `sto-core` |

#### Contract operation counts

Counts include provided executable methods and exclude inherited and erased spec/proof methods. Names are recorded
in the JSON. These are selected cross-component traits, not every public trait.

| Contract source | Trait | Methods |
| --- | --- | ---: |
| [`src/cluster/range_lock.rs`](../src/cluster/range_lock.rs) | `RangeLocks` | 2 |
| [`src/cluster/range_lock.rs`](../src/cluster/range_lock.rs) | `RangeGuard` | 3 |
| [`crates/mako-rocksdb/src/backend.rs`](../crates/mako-rocksdb/src/backend.rs) | `Backend` | 5 |
| [`crates/mako-rocksdb/src/backend.rs`](../crates/mako-rocksdb/src/backend.rs) | `Session` | 7 |
| [`crates/sto-core/src/protocol/commit.rs`](../crates/sto-core/src/protocol/commit.rs) | `Effects` | 7 |
| [`crates/sto-core/src/protocol/contracts.rs`](../crates/sto-core/src/protocol/contracts.rs) | `Transport` | 2 |
| [`crates/sto-core/src/protocol/contracts.rs`](../crates/sto-core/src/protocol/contracts.rs) | `ParticipantEngine` | 5 |
| [`crates/sto-core/src/protocol/contracts.rs`](../crates/sto-core/src/protocol/contracts.rs) | `CommitLog` | 13 |
| [`crates/sto-core/src/protocol/contracts.rs`](../crates/sto-core/src/protocol/contracts.rs) | `CommitPermit` | 2 |
| [`crates/sto-core/src/protocol/journal.rs`](../crates/sto-core/src/protocol/journal.rs) | `Journal` | 4 |
| [`crates/mako-storage-api/src/lib.rs`](../crates/mako-storage-api/src/lib.rs) | `Backend` | 4 |
| [`crates/mako-storage-api/src/lib.rs`](../crates/mako-storage-api/src/lib.rs) | `PersistentApplication` | 15 |
| [`crates/mako-storage-api/src/lib.rs`](../crates/mako-storage-api/src/lib.rs) | `TableBackend` | 3 |
| [`crates/mako-storage-api/src/lib.rs`](../crates/mako-storage-api/src/lib.rs) | `WorkerBackend` | 7 |
| [`crates/mako-storage-api/src/lib.rs`](../crates/mako-storage-api/src/lib.rs) | `LocalTransaction` | 7 |
| [`crates/mako-storage-api/src/lib.rs`](../crates/mako-storage-api/src/lib.rs) | `ParticipantBackend` | 10 |
| [`crates/sto-core/src/object.rs`](../crates/sto-core/src/object.rs) | `TObject` | 12 |
| [`crates/mako-rocksdb-store/src/bytes.rs`](../crates/mako-rocksdb-store/src/bytes.rs) | `Blobs` | 6 |
| [`src/srpc/rpc/server.rs`](../src/srpc/rpc/server.rs) | `Service` | 2 |
| [`src/srpc/rpc/channel.rs`](../src/srpc/rpc/channel.rs) | `ChannelConnectionBase` | 9 |
| [`src/srpc/rpc/channel.rs`](../src/srpc/rpc/channel.rs) | `ChannelListenerBase` | 6 |
| [`src/srpc/rpc/channel.rs`](../src/srpc/rpc/channel.rs) | `ChannelFactoryBase` | 3 |
| [`crates/raft-host/src/lib.rs`](../crates/raft-host/src/lib.rs) | `StateMachine` | 5 |
| [`src/cluster/storage.rs`](../src/cluster/storage.rs) | `Source` | 1 |
| [`src/cluster/storage.rs`](../src/cluster/storage.rs) | `Store` | 4 |
| [`src/cluster/transfer.rs`](../src/cluster/transfer.rs) | `RawStore` | 3 |
| [`src/cluster/durable_kv.rs`](../src/cluster/durable_kv.rs) | `DurableKv` | 3 |

#### Largest source files

This table uses the Source category, so inline tests and proofs remain included.

| File | Nonblank lines |
| --- | ---: |
| [`src/srpc/reactor/reactor.rs`](../src/srpc/reactor/reactor.rs) | 5,247 |
| [`crates/sto-core/src/transaction.rs`](../crates/sto-core/src/transaction.rs) | 4,358 |
| [`src/srpc/rpc/client.rs`](../src/srpc/rpc/client.rs) | 3,273 |
| [`src/deptran/raft/core/src/node.rs`](../src/deptran/raft/core/src/node.rs) | 2,974 |
| [`src/masstree/json.hh`](../src/masstree/json.hh) | 2,900 |
| [`src/mako/storage/mtree_abi.cc`](../src/mako/storage/mtree_abi.cc) | 2,566 |
| [`crates/sto-core/src/lock.rs`](../crates/sto-core/src/lock.rs) | 2,498 |
| [`crates/sto-core/src/direct_commit.rs`](../crates/sto-core/src/direct_commit.rs) | 2,357 |
| [`crates/sto-core/src/trans_item.rs`](../crates/sto-core/src/trans_item.rs) | 2,257 |
| [`src/masstree/clp.c`](../src/masstree/clp.c) | 2,245 |
| [`src/srpc/rpc/tcp_channel.rs`](../src/srpc/rpc/tcp_channel.rs) | 2,148 |
| [`crates/mako-history/src/application.rs`](../crates/mako-history/src/application.rs) | 1,915 |
| [`crates/raft-host/src/runtime.rs`](../crates/raft-host/src/runtime.rs) | 1,909 |
| [`src/deptran/raft/core/src/heartbeat.rs`](../src/deptran/raft/core/src/heartbeat.rs) | 1,884 |
| [`src/masstree/mtd.cc`](../src/masstree/mtd.cc) | 1,600 |
| [`crates/masstree/src/native.rs`](../crates/masstree/src/native.rs) | 1,546 |
| [`src/masstree/mtclient.cc`](../src/masstree/mtclient.cc) | 1,539 |
| [`crates/masstree/src/lib.rs`](../crates/masstree/src/lib.rs) | 1,468 |
| [`src/srpc/rpc/server.rs`](../src/srpc/rpc/server.rs) | 1,437 |
| [`crates/sto-masstree/src/lib.rs`](../crates/sto-masstree/src/lib.rs) | 1,397 |

<!-- component-inventory:end -->

### Why the transaction group is large

The logical table separates reusable cores, node orchestration, storage-specific
transaction support and application workloads. These have independent compile/test
entry points; they are not separate local and distributed transaction engines.
The same protocol handles one participant and many participants.

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

Distributed STO owns the generic coordinator and transaction rules as well as
frames, object contracts, terminal cleanup and clocks. The node runtime owns
concrete storage/RPC/log effects, services and recovery I/O. `sto-rocksdb` owns
the separate RocksDB transaction component. Both integration adapters,
`mako-storage-rocksdb` and `mako-storage-masstree`, remain with the runtime.
The separation follows backend-independent engine behavior versus node wiring.

The inventory counts inline tests as Source. Distributed STO still contains
large inline test modules. Moving tests would change classification without
reducing Total LOC. The Masstree reduction removes the Rust committed-row
registry, alternate layouts and redundant C-style facade. Its native row code
and safe FFI handles count with the transaction component, so moving work into
C++ does not hide those lines. See the [native row plan](plans/native-masstree-rows.md)
and [test-port record](testing/native-masstree-rows/README.md).

### Masstree transaction component

The Masstree leaf holds the address of a native C++ row. The row owns committed
binary buffers, version history and lock state. A native ownership list supports
reclamation; reads never traverse that list. There is no Rust committed-row
registry, user-value mirror or ID-to-value index.

```text
Rust application API
        |
Distributed STO: one live read/write set, OCC and participant phases
        |
Rust TObject adapter and safe native handles
        |
C++ row operations: lookup, locks, snapshots, history, install, reclamation
        |
Masstree ordered index
```

| Owned unit | Responsibility | Source LOC | Total LOC |
|---|---|---:|---:|
| `sto-masstree` | STO observations, predicate tracking and callback forwarding | 1,397 | 4,089 |
| `sto-masstree-api` | Safe database/table/worker API and retained participant ownership | 1,137 | 1,994 |
| `native-masstree-rows` | C++ row storage, safe FFI handles and native contract tests | 1,736 | 2,181 |

`native-masstree-rows` is an accounting unit in this same logical component.
Its files are `src/mako/storage/mtree_rows.{cc,h}`,
`crates/masstree/src/native_rows.rs` and `crates/masstree/tests/native_rows.rs`.
The generic index, integer ABI, allocator, RCU and thread support keep their
separate ownership. `mako-storage-masstree` remains a runtime adapter.

| Boundary | Contract |
|---|---|
| STO ↔ transaction adapter | `TObject` registers observations and deferred intents, acquires write locks, validates reads, installs and releases. STO is the only transaction manager. |
| Adapter → native rows | Safe handles retain C++ allocations. Native snapshots pin immutable buffers, validation covers the current OCC generation, and native guards retain locks across 2PC waits. No short RCU guard crosses a wait. |
| Native rows → index | A private append-only directory stores native row addresses. Native thread checks and operation-scoped RCU protect index calls. Missing keys and scans record native leaf versions; unrelated leaves do not invalidate them. Own insertions update exactly matching observations and register split leaves. |
| Runtime → wrapper | `sto_masstree_api::native::{Database, Worker, Table, Transaction, Participant}` provides thread-affine execution, frozen prepare rows, replay and checkpoints. |
| Runtime → durability | The LocalLog or ReplicatedLog records the distributed decision. RocksDB persists rows/checkpoints. Masstree recovery replays the committed gap, as before. |

Missing reads do not allocate rows. The native row embeds up to 16 bytes beside
its 48-bit version and 16-bit lock owner. An unshared embedded value can change
in place. Owned snapshots pin immutable bytes; an outstanding pin forces a
replacement buffer. Larger values and retained histories use packed native
allocations. An embedded snapshot can retain its row allocation after table
destruction. Copied reads use atomic bytes and a post-copy version check; heap
copies, snapshot pins and history changes use a short native latch.

Small staged-value and publication-node allocations use bounded native scratch
pools of 64 blocks per kind per thread. Pools contain only unreferenced raw
allocations and drain at thread exit. Row lookup prefetches the native row,
adapting original STO's value-prefetch pattern. Ordinary small-value updates
modify one row cache line; history links and allocation ownership occupy the
next line. The safe Rust handle supplies the checked table owner through the
private ABI, so each row needs no table backpointer. Generic integer
index values keep their separate no-prefetch contract.

C++ allocates replacement publication state before final certification. Install
transfers preallocated state and performs no allocation. Dropping a native lock with
an unresolved outcome fences its table; failures after STO installation retain
an indeterminate frame. Abandoned installed participants fence their worker.

Visibility selection runs in C++. Historical values and tombstones retain their
own distributed metadata, while OCC validation checks the current row generation.
STO retains the registered table and checks its incarnation before borrowing a
row token. Its existing direct local plan retains that table through explicit
lock-ticket release or quarantine. Retained participants use owning guards,
which share a weak lifetime lease. Neither path introduces a second write set.
Every writing observation is checked after all write locks are acquired. The
authoritative history floor permits pruning; owned buffer snapshots remain
valid after pruning or table destruction. Native budgets bound accounted row
headers and keys across tables. Payload/history allocations and allocator
metadata are separate, so the budget is not an RSS limit.

Frozen redo is collected from STO's authoritative items at validation. It retains
native buffers until serialization. The runtime consumes logical rows directly,
without a second mutable write ledger or a legacy encode/decode round trip.
Ordinary reads copy bytes once into the application's requested `Vec`; borrowing
a native value neither allocates nor copies its payload.

The November 27 performance qualification reached the revised 80% target.
Single-thread qualification passed first. The final five-pair matrix at 1, 2,
4, 8, 16 and 32 threads reached 91.5% to 95.8% of original C++ STO throughput
for reads and 84.7% to 89.9% for mixed updates. It measures local eight-byte
point workloads with synchronous access-scoped RCU and optimized release builds.
The [performance record](testing/sto-80/README.md) contains exact build settings,
raw trials, the earlier failing 32-thread checkpoint, and final correctness
results. These include 70 native cases under each of ASan, TSan and UBSan,
135 runtime cases and the upper gate in 164.893 seconds. Larger-value, scan and
distributed performance remain separate measurements.

The old Rust C storage ABI, registry arenas, alternative Rust value layouts,
fixed-u64 store and resolved caches are retired. Native embedded bytes are part
of the one generic byte-row implementation. The safe Rust API provides
`Transaction::get_into` for reusable output buffers and `with_access` for several
synchronous operations in one native RCU region:

```rust,ignore
let mut tx = worker.begin()?;
tx.with_access(|tx| {
    tx.get_into(&table, b"key", &mut buffer)?;
    tx.put(&table, b"other", &buffer)
})?;
tx.commit()?;
```

The region closes on success, error or unwind, before commit or any 2PC wait.
The callback must not perform I/O, block or await. Per-operation regions remain
available. A stale absence/range leaf observation stays stale through later
staging; only an exactly matching own-insertion observation advances. Final
certification rejects a foreign phantom, including after later own insertions.

The [80% performance record](testing/sto-80/README.md) compares the local wrapper
with the pinned original C++ string STO implementation. It records compiler
settings, synchronous access scope, commands, executable hashes and paired
trials. This local point-workload result does not establish distributed or
large-value throughput. `mako_bench` remains the node workload entry point.
Performance suites run manually or weekly, independently of compilation.

`MAKO_BUILD_PROFILE=MASSTREE_TX` independently compiles the component and its
core/index dependencies. RocksDB, Raft, SRPC and the application runtime stay
outside this build. Run `./docker_build.sh ci masstreeTx 32` for local tests,
doctests, strict wrapper Clippy and the compiled-dependency audit. Native cases
run in fresh processes. Compilation alone runs no tests or benchmarks.

Upper-layer Verus proofs still assume the storage contract. They do not prove
the C++ implementation. Native contract tests, the concurrent serializability
oracle, storage conformance and sanitizer checks exercise that implementation.
The [test-port record](testing/native-masstree-rows/README.md) records which
old optimizations were retired and where their supported behavior is tested.

#### Possible future Masstree optimizations

The November 27 comparison with original C++ MassTrans at `0d26e2e0b`
identified the following remaining opportunities. These are proposals; their
gains have not been measured. The completed
[80% qualification](testing/sto-80/README.md) covers the current implementation
and its eight-byte point workloads.

| Candidate | Original behavior and current difference | Possible change | Workloads to measure |
|---|---|---|---|
| Transaction scratch allocation | Original STO packs staged objects into a reusable transaction arena. The current wrapper uses separate pooled, reference-counted value and publication objects. | Evaluate native transaction arenas or combined preparation objects to reduce bookkeeping and improve locality. Preserve ownership when prepared state or snapshots outlive execution. | Mixed writes, including the current point benchmark |
| First write without a preceding read | Original MassTrans can observe the version of an existing row without retaining its value. The first current `set` still pins and drops an old-value snapshot to establish its observation. | Add a metadata-only observation for this path, retaining any earlier read observation and the same conflict rules. | Blind puts; little expected benefit for read-modify-write transactions |
| In-place updates beyond 16 bytes | Original `stuffed_str` retains capacity and overwrites values that fit. Current in-place updates cover at most 16 bytes; larger values use replacement buffers. | Evaluate capacity reuse for larger unshared values, with copy-on-write for pinned snapshots and safe concurrent copied reads. | Larger values and repeated updates within retained capacity |
| Fewer Rust/C++ calls | Original C++ combines lookup, value access and version handling within one compiled implementation. Current lookup and copied read cross the native boundary separately. | Evaluate fused native storage operations, such as lookup plus copied read. Keep read-your-writes and authoritative transaction observations in STO. | Point reads and updates |
| Instruction-cache placement | Original `MassTrans::install` explicitly aligns its entry to keep the generated byte-copy loop within an instruction-cache line. Current native installation has no equivalent explicit layout treatment. | Inspect generated code and measure function or loop placement. The useful alignment depends on the current compiler and instruction layout. | The current small-value benchmark, with repeated paired trials |

For the current point benchmark, first inspect instruction placement, then
evaluate preparation-object overhead and FFI calls. Blind writes and larger
values need separate workload cases to establish their benefits. Measure one
change at a time against the pinned reference and retain failed results.
Performance checks remain manual or weekly, separate from ordinary compilation.

The original source references are `src/mako/sto/Packer.hh` for
`TransactionBuffer`, `src/mako/sto/stuffed_str.hh` for capacity reuse, and
`src/mako/sto/MassTrans.hh` for `handlePutFound` and `install`, all at
`0d26e2e0b`. The [reference build recipe](testing/sto-80/README.md#build-and-reproduce)
explains how to extract that source. Current implementation points are
[native row storage](../src/mako/storage/mtree_rows.cc),
[safe native handles](../crates/masstree/src/native_rows.rs) and the
[STO adapter](../crates/sto-masstree/src/lib.rs).

Keep storage-specific work in C++ behind safe Rust handles, with one
authoritative STO read/write set. Any optimization must preserve immutable
owned snapshots, retained prepared state and locks across 2PC waits, allocation
before irreversible publication, and final validation after all write locks
are acquired. Short RCU regions must still close before waits. Recheck the
affected ownership and conflict contracts with component tests and sanitizers;
keep the existing serializability oracles, suppression inventory and upper-layer
contracts intact. Some current bookkeeping provides these guarantees and needs
an equivalent mechanism before it can be removed.

### RocksDB transaction component

`sto-rocksdb` provides transactional rows, conflict handling, prepare/install
state and recovery directly over RocksDB. Its 3,417 Source LOC and 4,641 Total
LOC are separate from runtime orchestration. It uses `sto-core` and
`mako-rocksdb-store`; it has no dependency on Masstree, the node runtime, RPC
or Raft. `Database`, `Table`, `Worker`, `Transaction`, `Prepared`, `Checkpoint`
and the distributed participant API define its storage boundary.

`mako-storage-rocksdb` contributes 961 Source/Total LOC to the runtime component.
It connects that API to `mako-storage-api`, matching the ownership of
`mako-storage-masstree`. The application-facing `mako-rocksdb` compatibility API
remains above the node runtime.

Build and test the component inside the container:

```sh
cargo build --locked --manifest-path crates/Cargo.toml -p sto-rocksdb
cargo test --locked --manifest-path crates/Cargo.toml -p sto-rocksdb \
  --all-targets --features test-hooks -- --test-threads=1
```

These commands use RocksDB and STO dependencies without Masstree or node
services. Runtime integration is covered separately by `ci/replication.sh`.

### Rust workload leaf

TPC-C is implemented in `mako-workloads/src/tpcc`. It uses the transaction
client for all five transactions. The same package owns loading/resume markers,
the micro workload, benchmark statistics, topology/CLI conversion and
`mako_bench`. Runtime `configuration::read` and node configuration remain generic.
There is no active C++ TPC-C implementation to port.

No production component, test dependency or build dependency may import this
package. `scripts/check_workload_boundary.py` checks every owned Cargo manifest,
including aliases, optional and target-specific dependencies, and rejects direct
Rust source imports/includes. Both executable build profiles run the audit; their test gates run the
negative controls. The Masstree component audit separately rejects runtime,
workloads, sharding, RPC, Raft and durable engines.

Inside the container, the existing native profile provides:

```sh
cmake --build build --target mako_bench workload_boundary_audit
cmake --build build --target rust_workload_tests
```

For Cargo-only compilation, use `-p mako-workloads --features replication,rocksdb-engine
--bin mako_bench` with native linkage configured, or `--features logging,rocksdb-engine`
for RocksDB-only. The executable name and launch/configuration syntax are unchanged.
Moved tests retain all five transaction operations, conflict rollback, cross-shard
atomicity, loader resume, durable reopen, replicated order counts and process shutdown.
For a RocksDB-only build, select `--engine rocksdb` when using
`examples/run_native.py`. The launcher emits an explicit RocksDB data directory
alongside the local/Raft log. Performance campaigns remain weekly/manual.

### RocksDB dependency ownership

All three persistent adapters use the maintained community
[`rocksdb` crate, version 0.25.0](https://docs.rs/rocksdb/0.25.0/rocksdb/).
`mako-rocksdb-store` handles rows and checkpoints, `raft-store` handles the shared
local/Raft journal, and `mako-sharding::rocks_metadata` handles migration metadata.
Mako keeps the contracts and persistence policies; upstream owns the C binding,
native handles, error allocations, iterators and snapshot destruction. These
three adapters forbid handwritten unsafe Rust.

Cargo builds the crate's pinned RocksDB 11.8.1 through `librocksdb-sys`. Each
executable uses one native implementation. The default build no longer links the
container's separate RocksDB 8.9.1. Compression codecs remain enabled so existing
compressed SST files can be read. Native builds require a C++ compiler and
libclang; the existing development container provides both. Upper protocol tests
still disable native storage features and do not compile RocksDB.

`mako-rocksdb-store` retains the `Blobs` interface, atomic ordered batches, explicit
WAL/sync policies and bounded scans. An owned snapshot uses `self_cell` to retain
an `Arc` database owner alongside upstream's borrowed snapshot. This preserves
transaction snapshot lifetimes without raw pointers or lifetime casts.
Metadata retains its independent writer lock, checksum and strict WAL recovery,
and fencing after uncertain writes. Local/Raft record encodings are unchanged.
The application-facing `mako-rocksdb` API continues to run Mako's distributed
transactions above these storage adapters.

The native engine upgrade requires forward recovery qualification. Reopening
files written by 11.8.1 with an older engine is not a supported downgrade promise.
An explicit external library override, used by TSan, must supply both
`ROCKSDB_LIB_DIR` and `ROCKSDB_INCLUDE_DIR` for the same pinned engine version.
See the [implementation plan](plans/maintained-rocksdb.md) and
[qualification record](testing/maintained-rocksdb/README.md).

### What the boundaries carry

The dependency table gives the complete declared local Cargo edges. The following
map names the contracts that cross the main architectural boundaries. Follow the
source links for signatures, error variants and proof preconditions.

| Consumer and provider | Interface and responsibility |
| --- | --- |
| Application → `mako-rocksdb` → runtime | `DB`, `Transaction`, CF handles and batches use [`backend::Backend` and `Session`](../crates/mako-rocksdb/src/backend.rs). Sessions preserve attempt identity, record point/range observations and resolve Unknown through the distributed coordinator. See [Rust RocksDB compatibility](#rust-rocksdb-compatibility). |
| Runtime → distributed STO | [`commit::Effects`](../crates/sto-core/src/protocol/commit.rs) separates locking, registration, prepare, decision and completion. STO's verified module decides legal transitions. The runtime performs effects and owns native lifetimes. |
| Protocol/runtime → storage plugins | [`mako-storage-api`](../crates/mako-storage-api/src/lib.rs) defines database, worker, table, local transaction and retained participant contracts. Masstree and RocksDB adapters must track absence/range reads, lock before validation, retain uncertain prepares and install only the saved decision. |
| Transaction manager OCC machinery → transactional objects | [`sto-core::TObject`](../crates/sto-core/src/object.rs) defines object-local state, locking, point/predicate validation, installation and cleanup. `sto-masstree` and test datatypes implement these obligations. `sto-core` must remain independent of Masstree. |
| Runtime → Masstree transaction component | `mako-storage-masstree` consumes [`sto-masstree-api::native`](../crates/sto-masstree-api/src/native.rs); `sto-masstree` owns engine-specific transaction state. Their independent build/test boundary is described [above](#masstree-transaction-component). |
| Runtime → local/replicated journal | [`Journal`](../crates/sto-core/src/protocol/journal.rs), [`CommitLog` and `CommitPermit`](../crates/sto-core/src/protocol/contracts.rs) distinguish persistence, authority, saved decisions and applied progress. MemoryLog is the test implementation. LocalLog and ReplicatedLog share the RocksDB journal format. |
| Materialization/checkpoint → persistent rows | [`PersistentApplication`](../crates/mako-storage-api/src/lib.rs) atomically installs rows, protocol metadata and applied position. Direct RocksDB and optional Masstree use the same persistent format. Log replay fills the gap after a Masstree checkpoint. |
| Runtime → Raft host → Raft core/store | [`raft-host::Host` and `StateMachine`](../crates/raft-host/src/lib.rs) carry proposals, authority, read barriers, committed application and checkpoint publication. The host drives `raft-core`; `raft-store::journal` owns persistent entries and metadata. `raft-replay` is a separate replay/checking tool. |
| Node runtime and Raft host → SRPC | [`Transport`](../crates/sto-core/src/protocol/contracts.rs) exchanges bounded owned requests and replies; [`runtime::rpc`](../crates/mako-runtime/src/rpc.rs) binds them to Rust SRPC services. [`protocol::wire`](../crates/sto-core/src/protocol/wire.rs), runtime wire modules and Raft codecs are compatibility boundaries, even though Cargo sees only a package dependency. |
| Sharding → runtime storage host | [`Source` and `Store`](../src/cluster/storage.rs), [`RawStore`](../src/cluster/transfer.rs), and [`DurableKv`](../src/cluster/durable_kv.rs) cover ordered range scans, exact EOF, scoped mutations and atomic metadata writes with Unknown outcomes. Admission and drain protect migration ranges. |
| Sharding host → native adapters | [`HostAbi`](../src/cluster/host.rs) contains row/reply sinks and storage/admin callbacks. The runtime migration adapter applies `shard-host-core` scan bounds and supplies physical effects through the transaction/storage contracts. These callback edges are absent from Cargo's dependency counts. |
| Sharding → transactional range exclusion | [`RangeLocks` and `RangeGuard`](../src/cluster/range_lock.rs) bind acquisition, drainage and I/O authorization to a migration plan. The shared implementation is [`sto_core::protocol::range_lock`](../crates/sto-core/src/protocol/range_lock.rs); the runtime supplies the same provider for both storage modes. |
| Rust ordered index → native storage | `masstree` → `mtree-sys` → [`mtree_abi.h`](../src/mako/storage/mtree_abi.h). The ABIs carry byte slices, scans, opaque ownership and thread/RCU lifetime requirements. The support closure includes allocator, RCU and registration code. |
| RocksDB adapters → external RocksDB | `mako-rocksdb-store` implements byte-store contracts over the maintained `rocksdb` crate; `sto-rocksdb` adds transactional row/version state. `raft-store` and sharding metadata use the same upstream crate. RocksDB atomicity/durability remain lower-layer obligations. |
| Verification and comparison tools → components | `mako-history` checks histories; `sto-test-datatypes` supplies object implementations. `srpc-verify`, `tla/mako` and protocol proof files import or model canonical source. Component benchmarks remain separate from the outer protocol's development gate. |

Here, owned means maintained in this checkout, including the Masstree fork and
its storage support. It does not imply original authorship.

The native Masstree bucket counts the maintained source tree, including files
outside the current link. It is not the C++ linked-size claim. The default native
build compiles the 15 translation units listed in
[`native-storage-sources.txt`](../cmake/native-storage-sources.txt), with the
reviewed support closure in
[`native-storage-headers.txt`](../cmake/native-storage-headers.txt). SRPC's native
bucket is C and assembly, with roles and architecture selection recorded in
[`native-kernels.json`](../src/srpc/scripts/native-kernels.json). It does not
represent a C++ RPC backend.

Source imports also cross boundaries without a normal Cargo edge. Sharding
proofs import `shard-host-core` and `tla/mako` files with `#[path]`; SRPC's generated
crate view and verification harness reuse canonical SRPC modules. Some integration tests reuse another crate's contract test source. The ledger charges each physical file to one owner,
regardless of how many times it is imported.

`raft-store` still contains the earlier segmented WAL and its component tests,
alongside the production RocksDB journal. `historical-models` includes
`tla/mako-original` and `Janus.tla`. `repository-tooling` includes older experiment
scripts as well as current automation. These are maintenance inventory, not a
claim that every counted file runs in the distributed application.

### Size and interface review policy

Use these as initial review thresholds, not automatic build failures. They are
proposed budgets; the existing baseline records exceptions without requiring an
unrelated rewrite.

| Measure | Review trigger | Required review |
| --- | --- | --- |
| Source per component | Over 5,000 NLOC | Identify independent state/ownership responsibilities and an extraction boundary. Above 10,000, record an extraction plan or a concrete reason to keep it together. |
| Source per file | Over 1,000 NLOC | Inspect responsibility boundaries and embedded tests/proofs before splitting. A file move alone is not component decomposition. |
| Owned normal dependencies | More than 5 direct dependencies | Review whether this is deliberate composition or access to another component's internals. Composition roots can justify a larger count. |
| Adapter contract | More than 15 executable methods on one trait | Check whether all operations share one lifetime, authority and failure model. Include inherited operations when reviewing the real interface. |
| Cross-component interface change | Any new callback family, wire operation, persisted field or dependency cycle | Record caller/provider, ownership, ordering, failure/Unknown handling, compatibility and the contract test or proof that checks it. |

`CommitLog` currently declares 13 executable methods and inherits four from
`Journal`. Its effective 17-method contract already crosses the proposed
interface threshold. Review the journal state operations separately from
authority, checkpoint and gateway receipt operations before adding more.

A component split should reduce what each side must understand. Keep state and
its invariants together; expose a small request/result or trait boundary. Avoid
moving half an invariant behind callbacks or creating a cycle just to meet a
line budget. High fan-in identifies contracts with many consumers, so changing
those contracts needs broader compatibility review. Pub-hit growth is a prompt
to inspect visibility, not a numerical acceptance rule.

The largest files and package totals above identify where to investigate first.
For example, `sto-core` includes inline tests, so Source LOC includes more
than executable production code. The Masstree adapter now keeps its behavioral
regressions in separate native integration suites.
Candidate boundaries within the current crates are:

| Current component | Candidate responsibilities to assess separately |
| --- | --- |
| `mako-runtime` | Participant/coordinator services; journal application and checkpoint/recovery; migration host; gateway/Redis/API transport; node/configuration and CLI tools. Keep the final node wiring as a composition root. |
| `mako-sharding` | Routing/admission; range transfer; durable migration actors; native host bindings; proof-only modules. Preserve the independent models and same-source refinement links. |
| `sto-core` | Transaction frame and commit protocols; object capability contracts; optional specialized transaction datatypes. Preserve one owner for terminal cleanup. |
| `sto-masstree` | STO callback forwarding and predicate observations. Keep native row storage in C++ and core tests independent of real storage. |
| `sto-masstree-api` | Safe storage facade and affine participant workers. Preserve one ownership capsule and collect redo from the core write set. |
| `srpc` | RPC/channel/client/server policy; reactor/fiber execution; serialization and platform leaves. Keep platform bindings independently auditable. |
| `raft-core` and `raft-store` | Consensus transitions and their proofs; storage interface; production journal versus retained segmented-WAL inspection code. Do not count moving proof code out as reducing consensus complexity. |

Before an extraction, record its current files, total NLOC, consumers and
contract operations from the ledger. Afterward, compare both resulting owners,
new dependencies and the operations crossing the new boundary. Run each
component's focused tests and the relevant contract/proof checks. Performance
qualification stays in its separate scheduled suite.

## Supported storage composition

The distributed application supports RocksDB with the transaction wrapper, or
that same persistent store with optional Masstree acceleration. Both use the
common OCC + 2PC protocol and local/Raft journal. The Masstree mode checkpoints
rows into RocksDB and recovers the committed gap from the log.

The earlier standalone `mako-cache` and `mrx` engines, cache-only
`mako-engine*` facades, `mrxdb` C API, `mtx` ABI, cache benchmarks and their CI
entry points are retired. Their generic byte-store contracts now live with the
storage adapter in `mako-rocksdb-store`; the shared timestamp type lives in
`sto_core::clock`. No standalone cache engine or cache feature remains in the active
Cargo graph. The standalone RocksDB/Raft service and its separate RPC/C client
API are also retired. Deploy the distributed runtime with one shard for the
single-shard service case; it uses the same OCC + 2PC path and storage adapters.
`ci/replication.sh` qualifies the shared components and the RocksDB runtime.

The [retirement plan](plans/retire-standalone-caches.md) records the scope and
validation. Historical cache reports describe their original revisions; they
are not instructions for building the supported storage paths.

## Transaction execution

A `Client` identifies its coordinator shard and worker stream. Each attempt has
a random client incarnation and an increasing attempt number. Each participant
also issues a stream lease. The global transaction ID and participant lease are
different identities; a lease cannot replace a transaction ID during recovery.

[`Transaction::commit`](../crates/mako-runtime/src/client.rs) calls the common
[`commit::coordinate`](../crates/sto-core/src/protocol/commit.rs) driver for any
participant count. The Rust RocksDB API uses that client. A one-participant
application transaction has the same locking, prepare, saved-decision and
completion semantics as a transaction with several participants.

The repository also exposes lower-level local entry points.
[`WorkerBackend::begin` and `LocalTransaction`](../crates/mako-storage-api/src/lib.rs)
support loading and component transactions. `sto-core::LocalCommit` retains
local execution support for direct storage users and component tests. These entry points are
outside the distributed application's commit path. Grouping the transaction
manager as one architectural component does not imply those APIs have been
removed.

`Client::begin` enrolls the coordinator's local participant. Reads, scans and
private writes use the same retained worker service on local and remote shards.
The caller's `sto::Worker` argument still enforces database ownership and the
public borrowing contract. It does not own the prepared transaction frame.

Execution is optimistic. A transaction reads committed versions and its own
private writes. The storage adapter records present and absent point reads and
scan predicates. A failed operation poisons that attempt, so ignoring an
operation error cannot commit the successful prefix of a transaction body.
Remote writes are staged immediately as private intents. They do not acquire
all write locks until the commit driver begins preparation.

Participant membership, routes and the read/write sets stop changing before
preparation. The coordinator includes its own shard even for an empty or
read-only local participant. Read-only participants follow the same validation
and decision protocol.

## OCC and two-phase commit

The production entry points are `sto_core::protocol::commit::coordinate`, runtime
`client.rs::NativeEffects`, and `prepared.rs::NativeEngine`.

1. **Lock every participant.** Freeze execution and acquire each participant's
   write locks. A conflict causes cleanup before any Prepare is sent. Each
   participant returns a timestamp proposal above its read dependencies and its
   durability policy. The driver rejects mixed durable and volatile membership.
2. **Register and prepare.** After every lock acknowledgement, choose the common
   timestamp and save `Begin` at the coordinator. Send the exact frozen
   descriptor to every participant. Each validates its point reads, absent
   reads and scan predicates, exports owned redo, and saves `Prepare` before
   returning Yes. Prepare publishes no committed rows.
3. **Record the decision.** Commit requires every member's Yes vote. Otherwise
   the coordinator chooses Abort. Its journal atomically establishes one
   immutable decision. A lost acknowledgement means Unknown until that record
   can be read or the identical operation retried.
4. **Complete the decision.** Participants check the coordinator authority and
   save completion. Commit publishes the saved intent; Abort discards it.
   Completion is idempotent. Locks and migration pins remain until the physical
   terminal action finishes. A native installation failure after saved Commit
   fences the shard for recovery.
5. **Reply to the client.** Success requires Commit acknowledgements from every
   member. A saved Commit with missing acknowledgements is still committed and
   returns Unknown to the caller. It must never become a retryable Abort.

The lock barrier and validation round are substeps of Prepare. They do not add
another transaction-commit phase. There is no shard-wide certification lock.

For example, A reads x and writes y, while B reads y and writes x. Both cannot
commit with their original observations: after all write locks are acquired,
at least one final validation observes a changed version or a conflicting lock.
The same obligation applies to absent keys and range predicates. Protecting
only existing point rows would leave phantom cycles possible.

Under the storage contract, the global lock barrier supplies a candidate serial
position. All of the transaction's reads precede that barrier; final validation
follows it. A conflicting writer that could invalidate an observation must
change its version or hold protection detectable by validation. The storage
adapter must enforce this for every writer and predicate. The upper state proof
alone does not prove that physical storage supplies these facts.

### Participant states and uncertain results

| State | Meaning | May release protection without a saved decision? |
| --- | --- | --- |
| Executing | Collecting private intents and observations | Yes |
| Locked | Frozen write locks acknowledged; no Yes yet | Yes |
| Preparing | Validated intent exists; prepare append may have succeeded | No |
| Prepared | Prepare is saved and Yes can be returned | No |
| Committed / Aborted | Saved outcome physically completed | Protection has been released |

Preparing is necessary because a storage error can follow persistence. A
participant must not treat a lost prepare acknowledgement as proof that it never
voted Yes. Higher terms, new stream claims, delayed ordinary Abort messages and
listener teardown cannot discard Preparing or Prepared work.

`Transaction::id()` exposes the stable ID. `Client::resolve(store, id)` resolves
the client's last registered attempt, reads or establishes the authoritative
decision, and retries completion without running the transaction body again.
It clears the client's fence only after every participant acknowledges that
outcome. A missing registration or unavailable authority remains Unknown.

## Durability and recovery

All three backends implement `CommitLog: Journal`. The shared contract includes
journal apply/lookup, immutable decisions, durability reporting, admission,
readiness and shutdown. Every backend owns its journal; the store never supplies
a hidden memory fallback.

| Implementation | Use | Storage |
| --- | --- | --- |
| `MemoryLog` | Isolated tests and explicit volatile native test fixtures | Memory only, with no restart guarantee |
| `LocalLog` | Production without Raft | Local RocksDB log |
| `ReplicatedLog` | Production with Raft | Raft-ordered RocksDB log |

`MemoryLog` lives in `sto_core::protocol::journal` and has no native backend dependencies.
Native test configuration must opt into it with `volatile: true`. `LocalLog`
and `ReplicatedLog` live in `mako_runtime::log`; they are typed specializations
of one implementation, accepting only their respective local or Raft config.
Both submit the same application command bytes and call the same state machine.
The upper protocol uses their backend-independent `Journal` contract.

Each participating shard chooses one journal policy:

| Policy | Journal acknowledgement | Restart guarantee |
| --- | --- | --- |
| Volatile | In-memory transition | None |
| Local persistent | Synchronous RocksDB log append and committed state-machine application | Local process restart |
| Replicated | Current Raft authority and applied committed record, with synchronous durable storage | Recovery from the surviving committed replica prefix |

A configuration with synchronous writes disabled cannot advertise durable
transactions. All members of a transaction must agree on the durability policy.

Both production modes use `<log-root>/log/base` for the ordered RocksDB log.
The configured `engine.directory` holds the common applied RocksDB state.
The `kind: masstree` compatibility spelling derives `<log-root>/data`. The log stores index/term/payload
entries, hard state, committed prefix, durable sequence, snapshot boundary and
exact authority metadata in one versioned format. Raft additionally uses the
vote and election-term fields. RocksDB WAL is enabled; synchronous atomic
batches supply the default durability barrier. The production Raft host no longer
uses a separate segmented WAL.

The common keys are `f` (format version), `a` (exact authority and sync policy),
`i` (store identity), `h` (term, vote and committed index), `c` (durable transition
sequence), `s` (snapshot boundary/reference), and `e` followed by a big-endian
index (entry). Entry values contain a little-endian term and a tagged payload:
`0` is a no-op; `1` precedes application command bytes. Local mode uses term 1
and no vote. The shared store validates transitions before persisting them.

A local append atomically saves its command and advances the committed prefix,
then calls the application state machine. Raft saves entries and advances that
prefix only through its consensus decisions. In either mode, recovery replays
committed commands above the application's saved position. Failure after a saved
command fences the driver until recovery, preserving an uncertain or committed
outcome. Append alone cannot make an uncommitted Raft suffix visible.

Sharing bytes does not make local and replicated authority interchangeable.
Reopen checks the exact mode, group/membership and sync policy. Changing a
configuration cannot turn an old Raft suffix into local committed data.

The journal records `Begin`, `Prepare`, `Decide` and `Complete`. A prepare stores
the full descriptor, final row intents and any gateway receipt. Completion
records committed rows, the participant outcome and the gateway receipt. The
direct adapter persists them immediately; the Masstree path batches them into
RocksDB while the committed log covers the gap. Prepared rows stay separate from committed rows.
The descriptor binds protocol version, transaction ID, common timestamp,
membership, participant leases and recovery endpoints.

Before foreground admission after startup or a leadership change, the service
loads unresolved prepares and reconstructs their native write protection. It
compares reconstructed redo with the saved intent. A mismatch fences the shard;
it does not turn a promised Commit into an Abort. Only after all unresolved
intents have been protected can new transactions execute.

A coordinator authority can save Abort for a still-pending registration. This
races atomically with the foreground Commit writer: whichever decision is saved
first remains authoritative. A participant cannot make that choice on its own.
If the coordinator is unreachable, prepared work blocks until authority returns.

Retained workers retry resolution after inactivity. Unprepared work may expire
and abort after 30 seconds without progress. Preparing and Prepared work instead
ask the coordinator to resolve the decision. Recovery retries preserve the
same ID and intent; no timeout authorizes an autonomous prepared abort.

A former leader with active native transactions can fail closed when a different
leader's committed prefix reaches it. Restart reconstructs its state from the
durable authority. This is a conservative availability limit of the native
adapter, not a second way to choose transaction outcomes.

Closing an RPC listener preserves prepared workers in the `Store`. Explicit
shard shutdown joins those workers and closes their native scopes. Durable
records survive and must be recovered before serving again. Volatile mode loses
its state on process shutdown. Terminal journal entries currently have no
reclamation policy; removing them without a stale-request fence is unsafe.
Both local and Raft logs trim a covered prefix after durable checkpoint
publication. Protocol records and receipts remain in that checkpoint until a
separate safe reclamation protocol exists.

The shared log directory format is `mako-rocks-log/1`. Only the current native
RocksDB row layout and transaction-journal commands are supported. Retired row
mirrors, file-only checkpoints, prefixed-key directories, row-command journals
and the C++ WAL have no compatibility reader or upgrade tool. Startup rejects
unsupported layouts while leaving their files intact. Use fresh Mako directories
when developing across incompatible format changes. Mixed old/new protocol
operation is unsupported.

Local logs reclaim obsolete checkpoint generations and abandoned attempts only
after validating recovered authority or flushing the replacement snapshot
reference. The current checkpoint remains intact. Raft retirement separately
waits for outgoing snapshot streams to release their image leases. Both
transaction adapters use the same native RocksDB checkpoint format. Protocol decisions, receipts and replay fingerprints
remain retained until a separate stale-request-safe garbage collection protocol
exists.

## RocksDB with optional Masstree

RocksDB is the final data store in both configurations. Select the transaction
adapter with one flag and keep the data and log directories unchanged:

```yaml
engine:
  kind: rocksdb
  directory: /data/shard/data
  masstree: true # false uses the direct RocksDB STO adapter
persistence:
  directory: /data/shard
  create: false
  sync_writes: true
```

The direct adapter applies each committed command atomically to RocksDB. With
Masstree enabled, its STO adapter serves transactions from a full in-memory
committed image. Each transaction still saves its Prepare and decision through
LocalLog or ReplicatedLog. Already committed effects accumulate in bounded
batches for RocksDB. No second validation or transaction decision occurs during
materialization. Undecided Prepare rows remain invisible.

The default flush targets are 128 log records, 8 MiB of estimated buffered data,
50,000 row effects, or a 100 ms timer. A single legal command larger than a target
flushes alone. Checkpoints and orderly shutdown also flush. A failed or uncertain
flush fences admission, leaving recovery to the committed log.

Let C be the committed log position, A the applied application position and D the
materialized RocksDB position. Healthy application cuts satisfy D <= A <= C. Direct mode
keeps D = A; Masstree can have D < A. Each RocksDB batch atomically saves its rows,
tombstones, logical versions, protocol metadata, receipts, replay identities and
D. A checkpoint pauses committed publication, flushes D to A, then creates and
publishes the native RocksDB snapshot before the host trims covered log entries.
This includes a saved Complete even if its affine worker has not installed yet.

Recovery loads the RocksDB base and replays the committed log suffix above D.
Masstree reconstructs its in-memory image before restoring unresolved prepares,
write protection and migration pins. Previous RAM progress never authorizes
skipping replay. The two modes use the same owner/schema identity and checkpoint
format. Compatible replicas in one Raft group may use different adapters.

Stop the node, change only `masstree`, and restart with `create: false` to switch
modes. No conversion or data copy is required. This is a restart toggle; it does
not add live switching, eviction or lazy cache misses. Masstree must fit the full
committed image in RAM. Old file-only Masstree stores are unsupported. The [adjustment plan](plans/rocksdb-optional-masstree.md) tracks the change.

## Gateway and application APIs

The gateway commits each storage operation independently. Session grouping does
not undo already committed operations. Redis MULTI/EXEC uses the distributed
transaction client instead.

Gateway writes, reads, unsuccessful inserts and missing-key deletes all use the
same OCC + 2PC driver. The saved prepare includes the exact response. Completion
persists that response with the row effects, allowing an identical gateway
request to recover its result after a lost reply or failover. Reusing a gateway
identity with another payload is rejected. A pending prepared gateway operation
returns Unknown and cannot be executed as a fresh operation.

The native SDK accepts owned keys and values. Routed requests include the table,
owner and ownership generation. Paged scans verify cursor identity, ordering,
bounds, explicit EOF and snapshot provenance before returning rows to the caller.
Transaction resource and wire limits remain enforced independently of the scan
page size. Individual wire messages and durable journal commands are bounded to
4 MiB. Volatile prepared intents may aggregate bounded messages up to 128 MiB,
which supports chunked large Redis values without enlarging the RPC frame limit.

## Rust RocksDB compatibility

`crates/mako-rocksdb` provides a Rust API subset modeled on the community
[`rocksdb` crate 0.25.0](https://github.com/rust-rocksdb/rust-rocksdb/tree/v0.25.0).
Its `OptimisticTransactionDB` and `DB` alias run Mako's distributed OCC + 2PC.
Every committed write reaches the configured local or Raft decision log.
The API supports direct RocksDB and RocksDB with optional Masstree through the
same types and methods. It does not introduce another native RocksDB binding.

Enable the `rocksdb-engine` Cargo feature. Enable `masstree-engine` as well to
allow `Options::set_masstree(true)`. With default features disabled, the crate
contains only the facade and backend contracts, which the fast upper gate tests
without RocksDB, Masstree, SRPC or Raft.

Concrete integration tests enable `native-fixtures` to create and inspect files
with the upstream `rocksdb` crate. The native and RocksDB runtime CMake test
targets enable it explicitly. It is absent from ordinary facade builds and the
upper gate, so fixture dependencies cannot pull a native engine into that gate.

```rust
use mako_rocksdb::{OptimisticTransactionDB, Options};

let mut options = Options::default();
options.create_if_missing(true);
let db = OptimisticTransactionDB::open(&options, "/data/mako")?;
let tx = db.transaction();
tx.put(b"account:alice", b"100")?;
tx.put(b"account:bob", b"200")?;
tx.commit()?;
assert_eq!(db.get(b"account:alice")?, Some(b"100".to_vec()));
```

`open` starts a durable single-shard node with four concurrent transaction
streams by default. `open_default` enables creation. `open_cf` accepts the full
set of column-family names and includes `default` automatically. Reopening must
supply the same family set; order does not matter. Names map to stable raw table
IDs. `set_error_if_exists` and `create_if_missing` are enforced. These methods create the native layout described below and can adopt a stopped
ordinary RocksDB database. Existing column-family names, keys and values retain
their application encoding. Opening an existing database does not require
`create_if_missing`; `open_cf` must name its complete user-family set. Old Mako
prefixed-key directories are unsupported; there is no upgrade tool.

For distributed deployments, `DB::open_config(MakoConfig)` starts and owns one
shard using the existing runtime configuration. Raw governed or static tables
become named column families, including a required `default` table. Opening
returns once the listener starts, so replicas can be started before a quorum
forms. `ready()` reports recovered coordinator authority on this handle;
`wait_ready(Duration)` waits with a deadline. Followers stay open and replicate
while not ready to coordinate transactions. Single-node `open` waits for readiness. Configure
all shard peers, the same schema and worker count on every shard, and local
persistence or Raft with synchronous durability. Warehouse tables and the
runtime's origin-local `replicated` table kind are rejected by this API. The
`replication` node setting still enables the normal Raft log. Point operations
and scans use the transaction's captured migration routing epoch.

The [compiled example](../crates/mako-rocksdb/examples/rocksdb_transactions.rs)
shows transactions and batches across column families. In the build container:

```sh
cargo run --locked --manifest-path crates/Cargo.toml -p mako-rocksdb \
  --features rocksdb-engine --example rocksdb_transactions -- /tmp/mako-api
```

Use the normal native build environment and `--features masstree-engine` to add
`--masstree` to the example's arguments. The same directory can be reopened with
that flag enabled or disabled.

### Native RocksDB directories and adoption

Both direct RocksDB and optional Masstree persist user rows in native column
families. Application keys have no Mako prefix or table ID; values have no Mako
envelope. The `__mako_internal_v2` family holds the catalog, conflict generations,
logical timestamps, deletion history, protocol records and applied-log position.
One native `WriteBatch` atomically updates rows and metadata across families.
Snapshots and checkpoints cover every family. Runtime features such as migration
may also create internal table families; the public API exposes its configured
application families. The row format is `STORDB02`, or `STORDBA2` for weak async
component configurations. The distributed facade requires synchronous logging.

First opening a plain database validates its family set and visible rows. A
second bounded scan initializes metadata and an initial-state digest. User
records are not re-encoded. Startup takes time proportional to the data size;
Masstree also loads the working image into memory. The previous writer must be
stopped throughout adoption. An active native writer, incompatible comparator,
reserved metadata family, malformed database, key above 1,024 bytes, or value
above 65,535 bytes causes an explicit error. Custom merge operators,
user-defined timestamp comparators and TTL semantics are unsupported.

Adoption persists its identity before moving the directory or writing metadata.
No transactions run before publication. A process crash during the move,
metadata scan or publication can resume with the same schema. Missing metadata
on an established Mako database is corruption, never permission to adopt it again.
Failed preflight leaves the original database retryable with corrected options.
The runtime removes the bootstrap marker after establishing durable journal
ownership. A later missing journal is an error.

Checkpoint installation still replaces whole generations. The public native
path is a relative symlink to the selected generation in a sibling control
directory. Adoption renames the original directory into it on the same
filesystem, preserving its user records. For `DB::open_cf` at `/data/orders`:

```text
/data/orders -> orders.mako/generation-0
/data/orders.mako/
  CURRENT                Mako generation selector
  LOCK                   Mako database lease
  ADOPTING               retained until initial journal ownership is established
  generation-0/          native CURRENT, MANIFEST, SST and WAL files
  install-<index>-<term>/ installed native checkpoint, when needed
  node/log/              shared local or replicated journal
```

Publication switches the selector and link; recovery repairs an interrupted
link update. Native handles open the resolved generation. Mako keeps old
files until their snapshot leases end. In explicit node configurations,
`engine.directory` is the public data path, its sibling `.mako` directory
controls generations, and the configured persistence/replication directory
continues to locate the journal.

Use ordinary RocksDB readers on a stopped database or an exported checkpoint.
Read-only openers can select the user families; read-write openers must include
all families, including metadata. Once adopted, mutations must go through Mako.
Copying only the public symlink is not a backup: retain the selected generation,
control state and journal together. With Masstree, native readers see the
materialized prefix. Runtime `flush()` or checkpointing drains the committed
gap; recovery uses the journal after interrupted shutdown.

For Raft bootstrap, seed every replica from the same stopped plain RocksDB image
or a RocksDB checkpoint of that image before starting the group. A digest of initial table
identities and contents binds peer messages and both local and replicated journal
identities. Pairing a seed with another database's journal is rejected. Different
seeds cannot vote or append to each other's group. This version requires
pre-seeding replicas; it does not distribute a leader-only seed automatically.
Subsequent replication, snapshots and failover use the normal Raft path. For
multiple shards, initial rows must belong to the configured shard; bootstrap
rejects populated families owned by another shard. Adopted rows can later move
through transactional migration.

This file layout preserves Mako's OCC+2PC semantics. Distributed timestamps,
ownership and recovery records remain Mako metadata. LocalLog and ReplicatedLog
keep `mako-rocks-log/1`. Raft peer envelopes now use version 3 for synchronous
and version 4 for asynchronous groups to bind the initial state; upgrade every
replica together. See the [implementation plan](plans/native-rocksdb-layout.md).
The [qualification record](testing/native-rocksdb-layout/README.md) records the
original storage, recovery and engine-free verification results. The
[current-format cleanup](plans/drop-old-mako-formats.md) retires its old-format
conversion work.

### Supported operations and options

| API | Behavior |
| --- | --- |
| DB `get`, `put`, `delete`, and their `_cf` / `_opt` variants | Each call is one certified distributed transaction. Deleting an absent key succeeds. |
| `transaction`, `transaction_opt` | Shared-reference transaction methods with consuming `commit`. A transaction starts on its first operation. |
| Transaction `get`, `get_cf`, read-option variants, `get_for_update`, `get_for_update_cf` | Reads include staged writes. Both exclusive settings record OCC dependencies; write locks are acquired at prepare. |
| Transaction `put`, `put_cf`, `delete`, `delete_cf` | Stage work until commit. An operation error poisons the attempt even if the application ignores it. |
| `rollback` and Drop | Abort staged work. Successful explicit rollback allows reuse as a new attempt. Uncertain cleanup stays fenced. |
| `multi_get` on DB or transaction | Preserve key order, duplicates and missing values. DB multi-get returns errors for all keys if final certification fails. |
| `WriteBatch`, `write`, `write_opt` | Ordered puts and deletes commit atomically across families and shards. A foreign handle or invalid batch rejects the batch. |
| `iterator`, `iterator_opt`, `iterator_cf`, `iterator_cf_opt` | Bytewise Start, End and From seek modes, forward or reverse. ReadOptions provide inclusive lower and exclusive upper bounds. |
| `WriteOptions::set_sync` | Both values use synchronous durability. The facade does not weaken the configured durable log. |
| `WriteOptions::disable_wal` | `false` is accepted; `true` returns NotSupported before work starts. |
| `OptimisticTransactionOptions::set_snapshot` | `false` is accepted; `true` returns NotSupported. |
| `Options::set_max_transactions` | Mako extension for the single-node coordinator stream limit. Distributed configuration uses `workers`. |

Only listed methods and options are supported. There is no pessimistic
`TransactionDB` alias, MVCC snapshot, savepoint, merge operator, raw/pinned
iterator, dynamic column-family creation/deletion, SST import/export, custom
comparator or RocksDB tuning facade. Unsupported methods are absent, and
unsupported values of implemented options return an error. For `transaction_opt`,
that error is reported by the first operation or commit. No C/C++ public
compatibility layer is included.

Iterators eagerly collect at most 4,096 rows and 4 MiB. Exceeding either limit
returns an Incomplete item, never a successful truncated range. Use narrower
bounds for larger scans. A DB iterator certifies its read transaction before
returning rows. A transaction iterator leaves final certification to commit;
its buffered rows do not change after later writes in that transaction. Iterators
can themselves yield errors, so collect or handle their `Result` items.
Keys are limited to 1,024 bytes and values to 65,535 bytes. Batches are limited
to 4,096 operations and 4 MiB of key/value bytes; encoded durable transaction
limits can reject a smaller batch because they also include metadata.

### Isolation and uncertain results

All Mako transaction reads, absence observations and ranges participate in OCC
validation after the global write-lock barrier. This differs from plain reads in
RocksDB's optimistic transaction API. It does not provide an MVCC view fixed at
transaction creation. Values read during a transaction become certified only
when commit succeeds. The facade adds no automatic transaction retry.

DB is Send + Sync. A transaction is Send and not Sync. Internal threads own
native workers and SRPC listeners, and the handle joins them during cleanup.
Each started transaction reserves one coordinator stream until its terminal
result is received. Exhausting the limit returns Busy rather than waiting while
the same caller may hold another transaction.

Busy reports a conflict or occupied stream. TryAgain reports changed routing or
unavailable authority. An UnknownCommit error carries `transaction_id()` when an
attempt exists. Save that identity and call `db.resolve(id)` against its
coordinator. Resolution completes the recorded Commit or Abort at every
participant and never reruns application code. It also works after reopening
the coordinator with the same durable data. A missing registration stays unknown;
it does not authorize treating the attempt as aborted. A lost commit reply keeps
its stream unavailable until resolution succeeds.

`pending_transactions()` lists uncertain results and failed Drop cleanup seen by
this DB handle. The list itself is not persistent. Keep returned identities in
the application if they must survive application restart. Resolution refuses a
stream with a live transaction. Preserve an ID before commit with
`tx.transaction_id()` after the first operation, or obtain it from the error.

The retired `mrxdb_rocksdb_compat.h` cache API did not supply these
distributed transaction semantics. The [implementation plan](plans/rust-rocksdb-api.md)
records the completed work. The [qualification record](testing/rust-rocksdb-api-2026-11-25/README.md)
contains the upper, native, sanitizer and build results. The final upper gate
completed in 159 seconds on 32 CPUs, including the facade contract tests.

## Sharding and migration

Routing uses immutable snapshots. An admitted transaction retains its snapshot
and epoch pin through execution and terminal cleanup. Ownership generations
prevent an old grant from becoming valid merely because a range later returns
to the same owner number.

Range migration stages and pre-copies destination data while the source serves.
It then publishes Frozen metadata and reserves the source range through the
transactional range-lock interface, waiting for earlier pins to drain. After
joining the copy worker and fencing its generation, it reconciles
complete ordered scans, including destination-only deletions. Explicit EOF
certifies coverage. The old source is retired before the new ownership snapshot
is published, and both participants must finish before reporting completion.

Prepared transactions retain their migration pins across client disconnects and
listener closure. Migration must wait for their authoritative decision and
physical completion. It must not interpret a missing client or an expired RPC
as permission to drain them.

Migration uses the selected data engine and transaction log. Durable mode
requires `sync_writes: true`; Masstree and direct RocksDB work with LocalLog or
ReplicatedLog, including transfers between adapters. Replicas within one
Raft group may choose different adapters over the common RocksDB format. Volatile fixtures still use MemoryLog and have
no restart guarantee.

The durable coordinator and participant journals live in a reserved internal
table in the selected engine. Their atomic batches use the same OCC + 2PC log
as user rows. The common RocksDB checkpoints therefore include
control state without a second metadata database. Application schemas and
ordinary data requests cannot access this namespace. Checkpoint identity binds
the migration format; enabling migration on an existing fixed-placement
directory requires a separate migration of authority. Toggling the data engine
does not enable migration.

Startup and leader changes restore retained transaction locks and epoch pins
before opening ordinary admission. Recovered prepares can resolve only their
saved decisions; they cannot execute fresh operations using recovery admission.
The migration actor rebuilds new range guards after publishing recovered Frozen
state. No pre-crash guard or epoch number proves that work has drained. Recovery settles
old internal row operations before examining migration journals, then resumes
the coordinator's saved phase and any required copy or cleanup. A saved Commit
is never changed to Abort. Temporary peer recovery failures are retried.

Control handles and row callbacks retain the admitting log term. Metadata
reads and source scans confirm quorum authority; writes bind the log proposal
itself to the captured term. Replica-group discovery finds the current owner
leader. Unknown physical outcomes fence or stop the process until recovery.
The older `MAKO_SHARD_METADATA_DIR` inspection path is separate and cannot be
combined with application-log-backed recovery.

The [migration chapter](architecture/sharding-migration.md) gives the protocol
proofs. Actual engine effects, serialization, filesystem durability, the Rust
runtime authority scopes and RPC delivery remain adapter obligations. Crash
qualification and release evidence are tracked in the
[optional-Masstree plan](plans/rocksdb-optional-masstree.md).

### Transactional range guards

Sharding calls `RangeLocks::acquire(plan)` after publishing the source's Frozen
or destination's Stage admission. The returned guard is initially pending.
`drained()` polls for earlier admitted work; `enter()` returns an affine scope
only after drainage. The scope authorizes physical callbacks for that shard and
the exact logical half-open range. Raw keys use their bytes as coordinates;
warehouse tables use the warehouse coordinate for all physical row aliases.

The shared implementation lives in `sto_core::protocol::range_lock` and is reexported
by `mako-storage-api`. The runtime owns one domain per Store and checks normal
point/range access against its reservations. It uses the same admission pins as
transaction execution, prepared intents and uncertain-work quarantine. Both
Masstree and RocksDB use this implementation; the index does not implement a
second migration protocol.

The first implementation drains every earlier admission pin in that shard,
including unrelated work. New accesses outside the reserved range continue.
Final copy retries while the destination guard drains; it does not turn an
ordinary wait into a migration abort. Cancellation, shutdown and authority
changes still interrupt that wait.
Precise overlap-based draining is a future optimization, not a current promise.
The guard excludes ordinary access, including absent keys and future inserts;
migration row effects still take the normal OCC write locks and journal their
decisions. An unguarded source precopy scan remains advisory and cannot certify
final transfer completeness.

Canonical plan identity includes generation, source/destination, bounds, nonce
and old ownership grants. Reuse with another identity is rejected. Completed
identities remain recorded to reject delayed reacquisition. Recovery increments
the guard incarnation, invalidating old handles even when a migration generation
is reused during reconstruction. Reset requires closed admission and settled
physical effects. Outstanding I/O scopes prevent reset or release.

Neither dropping a guard nor losing a callback releases the reservation.
`finish()` is explicit and follows the terminal ownership publication and any
required cleanup. On Commit, old-owner admission remains non-serving after
release; stale routing grants cannot regain access. Abort may release a pending
guard after restoring the appropriate admission, without releasing any retained
transaction pin. The old epoch callback slots remain in the retained host ABI,
but live migration requires the typed provider. The live peer handshake version
rejects peers using the previous contract; persisted row/journal formats are
unchanged.

The range-lock implementation is ordinary Rust with deterministic and concurrent
contract tests. It is not claimed as a new Verus proof. Existing actor proofs
retain their explicit storage/quiescence assumptions, and the upper gate runs
the engine-free range tests alongside those proofs:

```sh
cargo test --locked --manifest-path crates/Cargo.toml -p sto-core --features protocol --test protocol_range_lock
```

See the [revision plan](plans/migration-range-guard.md) and
[qualification record](testing/migration-range-guard/README.md).

## Storage format policy

Mako is under active development and does not preserve compatibility with its
retired formats. The converter, old row mirror, file-only checkpoint reader and
prefixed-key decoder have been removed. The runtime uses native RocksDB column
families for durable data, with optional Masstree materialization and the common
local/Raft transaction journal. Unsupported existing Mako directories are
rejected rather than converted or overwritten.

This policy does not change adoption of supported ordinary RocksDB databases.
It also does not require conversion to enable or disable Masstree: stop the
node, change `engine.masstree`, and reopen the same current-format data and log.
Recovery and migration tests exercise that shared format. The
[cleanup plan](plans/drop-old-mako-formats.md) records the retired code and checks.

## Workers, RPC and ownership

Each participant stream runs on one affine native worker. Only owned requests,
redo and responses cross threads. The participant service creates workers as
streams arrive and retains them for later attempts. It currently bounds the
service to 256 participant workers; gateway streams use a separate ID range.

The RPC poll thread decodes owned bounded messages and dispatches work. It does
not run blocking journal or transaction work. A control worker performs recovery
admission and outcome lookup; participant workers serialize execution, prepare
and completion for their streams. A local transport uses the same service
without a socket round trip.

The deployment assumes trusted configured peers and non-Byzantine components.
Owner, transaction and lease checks detect stale or mismatched messages; they
are not a cryptographic authentication scheme. SRPC timeouts bound the caller's
wait and cancel pending futures, but do not prove that a remote effect stopped.

Rust ownership enforces worker affinity and keeps table and database allocations
alive. A prepared participant belongs to the service, so dropping a client frame
cannot invoke its ordinary abort-on-drop path. If that worker panics, its cleanup
fences the shard before releasing native locks and quarantines the migration pin.
The saved decision remains authoritative; serving resumes only after recovery.

## Clocks and identities

Keep these domains separate:

| Value | Purpose |
| --- | --- |
| Native OCC version and lock bits | Validate physical observations and conflicting writes |
| Distributed timestamp and application term | Common transaction row metadata and replay ordering |
| Transaction ID | Identify one coordinator attempt and deduplicate its records |
| Stream lease and request sequence | Fence retired senders and replay exact execution replies |
| Raft term and index | Identify consensus authority and committed log position |
| Routing version and admission epoch | Fence ownership and determine migration drain |

The distributed runtime uses bounded typed timestamps from `sto_core::clock`. It
selects the maximum proposal after the lock barrier; each proposal exceeds that
participant's read dependency. Exhaustion is an error, never wraparound. The
existing row/redo encoding is `timestamp * 10 + application_term`. It is not a
Raft term or physical wall-clock timestamp.

Historical cache timestamp and ABI formats are documented in
[cache timestamp design](reference/cache-timestamp-design.md) and the
[local ABI contract](reference/mako-local-abi-v1.md). Those standalone cache
implementations are retired; their formats are not supported application APIs.

## Build, configuration and programs

Use the build container and pinned Rust toolchain:

```sh
./docker_build.sh build
./docker_build.sh ci upper 32
```

`NATIVE_RUST` is the default CMake profile and includes both data adapters.
`ROCKSDB_RUNTIME` builds the same distributed applications without Masstree.
Both profiles support one shard or multiple shards with local or Raft logging.
The standalone `ROCKSDB_RAFT` service profile is retired.
The retired `MAKO` application profile is rejected.
Native source and link audits constrain the remaining C++ dependency closure.

The primary programs are `mako_node`, `mako_transactions`, `mako_bench`,
`mako_client`, `mako_admin` and `mako_redis`. Each has `--help`. JSON/YAML parsing
and CLI compatibility live under runtime `configuration.rs` and
`configuration/`. Validation runs before creating databases, listeners or
workers.

Node configuration specifies owner count, listen addresses, worker counts,
tables, peer endpoints and optional replica groups. Choose either local
`persistence` with a log directory or `replication`. Configure the RocksDB
data directory and optional Masstree layer with `engine`. Both log policies
default to synchronous writes. Omitting both is an error unless `volatile: true` explicitly selects
development/test mode. Volatile mode cannot be combined with either persistent
policy. Creating a new durable directory is explicit; use `create: false` when
reopening it. Initial durable data is loaded through
transactions after authority is ready. Colocated shards require explicit ports
and consistent topology and schema.

Inside the container, a bounded two-shard example is:

```sh
python3 examples/run_native.py transactions --build build --shards 2 --workers 3 --count 50
```

The launcher uses Masstree over RocksDB data and a local RocksDB log by default. Add `--replicas 3` for Raft, or
`--volatile` for an explicitly ephemeral development run. The launcher
owns its processes, assigns ports and retains failure diagnostics. Consult its
`--help` for supported combinations.

## Development tests and proofs

Use the fast upper gate for ordinary transaction, sharding and migration changes:

```sh
./docker_build.sh ci upper 32
```

It disables C/C++ compilers, checks the dependency graph, verifies the production
Verus cores, runs semantic negative controls, and executes deterministic
contract tests, including the backend-independent Rust RocksDB facade. It has a five-minute deadline. Choose a CPU budget suitable for
the machine. Detailed reports record the source inputs and wall time. The
[OCC + 2PC qualification](testing/occ-2pc-2026-11-25/README.md) records the
checks for that cutover. The later
[unified-log qualification](testing/unified-rocks-log-2026-11-25/README.md) records
the shared local/Raft format, crash/refusal tests and explicit backend contract
checks. The optional-Masstree upper run completed in 175 seconds on 32 CPUs,
including 29 semantic negative controls. Backend/component, process recovery,
release, sanitizer and link-audit evidence is recorded separately in the
[optional-Masstree qualification](testing/rocksdb-masstree-2026-11-25/README.md).

The 2PC proof covers the lock barrier, participant-specific Yes votes, the saved
decision requirement, decision immutability, completion acknowledgements and
retained prepared protection. Journal encoding and native mappings are ordinary
Rust tested separately. Small exhaustive histories check committed point,
absence and range observations against a serial-order oracle. They complement
the state proofs; they are not a proof of Masstree or a distributed storage stack.

Run component and adapter tests at their own boundaries. Native integration
checks include real SRPC, cross-shard write skew, prepared teardown, RocksDB
restart, Raft failover and gateway receipt recovery. Storage, RPC and Raft
component suites remain separate. Sanitizers use the repository's dedicated
instrumented builds and fresh processes for poison/fail-stop cases.

Performance belongs to the manual/weekly workflow. Do not run a throughput
benchmark on every edit or use an old speculative-protocol result as a target
that changes commit semantics. See [development](developer/development.md) and
the [native test mapping](developer/native-rust-retirement.md) for commands.

## Operational interpretation

A conflict before a saved decision is a normal abort and may be retried as a new
attempt. Unknown means the caller lacks a confirmed terminal result. Resolve
its stable ID or exact gateway request; do not repeat the application body.

A prepared transaction can block when its coordinator authority is unavailable.
Restore that authority or its committed replica prefix. Releasing its locks or
deleting its journal entry is not a recovery procedure. A native installation
fault requires shard recovery, and new admission stays fenced meanwhile.

For a stuck migration drain, inspect unresolved transactions and their saved
coordinator decisions as well as active execution pins. For a failed startup,
check the retained configuration, directory identity, ports and journal recovery
error. Avoid killing unrelated processes or deleting durable state to clear an
error.
