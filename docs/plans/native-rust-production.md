# Native Rust production migration

Requested 2026-11-24. Worktree `/var/tmp/mako-shard-rust`, branch
`sharding-on-rust-engine`, starting revision `4703ec10a`.

## Objective and completion criteria

Remove Paxos. Rewrite the remaining first-party application and integration
code in Rust. Production uses `src/srpc` through Cargo and rustc, with no
SRPC/Lion Rust-to-C++ transpilation. Keep C++ only for Masstree, RocksDB, and
the small storage ABI/runtime they actually require. SRPC's existing C OS
kernels and architecture context-switch assembly remain platform bindings;
they do not justify retaining C++ networking or application policy.

The previous retirement made the transaction engine Rust-only. It did not
complete this migration. Neither deleting build targets nor running a new
reduced demonstration counts as preserving the existing application.

Completion requires:

- No Paxos implementation, selectable mode, service, or production dependency.
- Native Rust production executables, using the existing transaction manager,
  sharding/migration machinery, SRPC and verified Raft core.
- No production C++ application, networking, replication, replay-policy, or
  workload objects, including generated C++ from Rust.
- An explicit native storage source list and build/link audit. No application
  code hidden in that list or behind storage-named directories.
- Preserved supported transaction, migration, replication, persistence and
  application behavior, with executable regression evidence.
- Rust transaction-manager tests independent of Masstree; separate Masstree
  component/ABI tests; Rust application and integration tests.
- Updated default build, CI, examples and documentation, and committed stages.

## Stages

| Stage | Work | Exit evidence | Status |
| --- | --- | --- | --- |
| 1 | Delete Paxos protocol, worker, services and selection. Preserve shared Raft state under neutral names. | Raft builds and relevant regression tests; removed protocol rejected; source/symbol audit. | Complete |
| 2 | Establish a storage-only native build and an idiomatic Rust application API over STO/Masstree. Separate storage runtime from worker/replay policy. | Cargo links against only the explicit Masstree native dependency set; local transaction/application tests pass. | In progress |
| 3 | Port shard clients/receivers, remote participants, admission, scan delivery, migration host, gateway and process lifecycle. Use native SRPC. | Real local and multi-process transactions, abort/cleanup, scans, warehouse relocation and shutdown tests. | Pending |
| 4 | Move replication application integration, callbacks, replay, snapshots and recovery into Rust using the existing verified Raft core and native SRPC. | Replicated transactions, failover, replay, snapshot and restart tests; no C++ Raft host dependency. | Pending |
| 5 | Port supported workloads, loaders, CLI/examples and integration tests. Switch default build and scripts to Rust. | TPC-C and simple-transaction cases for local, sharded and replicated modes; supported configuration compatibility. | Pending |
| 6 | Delete replaced C++ and obsolete transpiler/build paths. Update documentation and audit the complete dependency graph. | Clean default build, targeted/full relevant tests, proof gates, storage-boundary sanitizer checks and native object audit. | Pending |

Stages may overlap where a dependency requires it. A stage is complete only
after its checks pass; failures and remaining gaps stay recorded here. Work
continues without requesting confirmation for the authorized migration.

## Behavior to preserve

| Area | Required coverage |
| --- | --- |
| Transaction manager | Existing STO unit/property/compile-fail tests and proofs, without a Masstree dependency. |
| Native tree | Separate Masstree lifecycle, reclamation, point/range and ABI tests. |
| Application operations | Begin/get/put/insert/delete/scan/commit/abort, metadata, table ownership and bounded callbacks. |
| Distributed transactions | Participant retention, conflict/abort cleanup, dependencies, retries, remote scans and worker affinity. |
| Sharding and migration | Admission/fencing, source/target copy and replay, relocation during live traffic, route publication and restart. |
| Replication | Raft-only selection, leader/follower application, durability, unknown outcomes, replay ordering, failover and snapshots. |
| Persistence | Cache writeback and fresh-process recovery; existing direct RocksDB/Raft service continues to work. |
| Runtime | Start/stop/join, thread registration, connection teardown and multi-shard single-process ownership. |
| Workloads | Existing supported TPC-C transaction mix and loaders, simple transactions and current configuration/CLI workflows. |

Use the repository's existing container runner for builds and tests. Record
commands/logs and results at each checkpoint. Existing Rust modules are reused
directly; do not create a parallel transaction engine, sharding protocol or RPC
implementation. Do not remove a test merely because the port has not reached
the behavior it checks.

## Initial audit

The active native application still compiles `rust_sto_tpcc_wrapper.cc`, the
migration host and RPC adapters, Mako clients/receiver, startup/workloads,
replay/thread support, and DepTran integration. `src/rocks_interface` contains
gateway/application code as well as storage API declarations. Its directory
name is not an exemption.

`src/srpc` already has canonical Rust modules and native runtime tests.
`crates/raft-host` already uses the verified Raft core and native SRPC.
`src/cluster` is native Rust. `sto-core`, `sto-masstree`, and `sto-tpcc-ffi`
already implement transactions and retained participants. The port must connect
these existing components while preserving the current application contracts.

## Checkpoints

- Plan created before implementation; baseline worktree was clean.

+ Stage 1: Paxos protocol, worker, coordinator, RPC service, payload membership,
  mode and obsolete experiment configs removed. Existing Raft script names that
  contain "paxos" remain temporary compatibility names, with Raft-only selection.
  Raft application epoch/status definitions no longer import Paxos headers.
  Build passed for `dbtest`, `simpleTransactionRep`, `sto_tpcc_bench`,
  `test_raft_application_log`, and `test_legacy_raft_log`.
  Evidence: `/tmp/mako-native-rust-paxos-build-04.log` and
  `/tmp/mako-native-rust-paxos-tests-01.log`. Both separate-process and colocated
  two-shard Raft protocol cases passed, as did both log-format cases. A negative
  CLI check rejected Paxos and `nm` found no implementation symbols in the
  replication archive. This still validates the temporary C++ application.
+ The inline emitter generated an empty `namespace @unresolved-trait` when
  regenerating application command membership without the deleted payloads.
  Removed that empty declaration; retained the Rust source and valid generated
  membership definitions. This temporary compatibility header is removed when
  the native Rust application replaces its consumers.
+ Stage 2 initial evidence: a fresh `NATIVE_RUST` profile builds an explicit
  16-source storage archive, without configuring SRPC C++ or any transpiler.
  `/tmp/mako-native-rust-storage-build-01.log` records the build/source audit.
  The new owned application API uses the existing STO implementation and pins
  table lifetimes across attempts. Two native lifecycle tests, the !Send worker
  compile-fail doctest, and Clippy passed against that archive in
  `/tmp/mako-native-rust-api-tests-02.log`. Full adapter coverage is next.
