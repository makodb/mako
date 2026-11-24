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
| 2 | Establish a storage-only native build and an idiomatic Rust application API over STO/Masstree. Separate storage runtime from worker/replay policy. | Cargo links against only the explicit Masstree native dependency set; local transaction/application tests pass. | Complete |
| 3 | Port shard clients/receivers, remote participants, admission, scan delivery, migration host, gateway and process lifecycle. Use native SRPC. | Real local and multi-process transactions, abort/cleanup, scans, warehouse relocation and shutdown tests. | Complete |
| 4 | Move replication application integration, callbacks, replay, snapshots and recovery into Rust using the existing verified Raft core and native SRPC. | Replicated transactions, failover, replay, snapshot and restart tests; no C++ Raft host dependency. | Complete |
| 5 | Port supported workloads, loaders, CLI/examples and integration tests. Switch default build and scripts to Rust. | TPC-C and simple-transaction cases for local, sharded and replicated modes; supported configuration compatibility. | In progress |
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

+ Stage 2 complete: the storage archive no longer depends on `TThread` or the
  application's epoch runtime. Thread registration now owns only native RCU
  identity allocation. The old application delegates to that allocator while
  its replacement is in progress. `native::Database`, `Worker`, `Table`, and
  `Transaction` provide an owned Rust API with same-thread workers, table
  pinning, commit/abort, point operations and bounded directional scans.
+ `/tmp/mako-native-rust-adapter-suite-03.log`: 271 Masstree/STO-tree cases and
  78 application-adapter cases passed against `libmako_native_storage.a`.
  The latter include four public Rust API cases for CRUD/scan/abort/lifetime,
  forgotten-guard cleanup, concurrent conflicts and foreign-database rejection.
  Existing poison/limit tests retain the old runner's fresh-process contract.
  Earlier aggregate runs exposed that contract and were corrected in the runner,
  without removing or weakening cases.
+ `/tmp/mako-native-rust-components-01.log`: the independent `sto-core` and
  `sto-test-datatypes` suites passed, including compile-fail cases and doctests;
  the new API's !Send doctest and all-target Clippy also passed.
+ `/tmp/mako-native-rust-storage-audit-02.log`: fresh-profile build and CTest
  audit passed. The compile database contains only the 16 named native storage
  sources plus the toolchain standard-library module. The archive has no
  application worker/epoch, shard client/receiver, SRPC, or DepTran definitions.
+ This profile is still opt-in and currently builds the storage dependency and
  Rust test targets. Stage 3 starts the production application port; the default
  C++ application is not yet switched or deleted. Paxos removal is committed as
  `195ceb8a0`.

+ Stage 3 checkpoint: native Rust SRPC now carries retained STO participant
  requests. `crates/mako-runtime` owns bounded affine worker lanes, exact request
  identity checks, terminal reply retention, coordinator callbacks, table
  ownership, shutdown and migration admission epochs. The existing STO scalar
  coordinator still chooses timestamps and drives certification/installation.
  No transaction or migration algorithm was replaced with a second engine.
+ The owned STO API now supports versioned reads/scans, remote dependency
  observations, retained participant phases and typed coordinator callbacks.
  Body errors release remote sessions. Dropping an installed participant fences
  its worker rather than reporting a successful abort. Uncertain receiver and
  coordinator outcomes retain their migration epoch pins.
+ `/tmp/mako-native-rust-participant-tests-02.log`: 81 adapter cases passed.
  `/tmp/mako-native-rust-runtime-unit-03.log`: eight admission, epoch and native
  SRPC cases passed without the native-storage feature. The participant and
  coordinator integration suite in
  `/tmp/mako-native-rust-runtime-integration-02.log` passed 13 cases in fresh
  processes, including real two-shard transactions, body-error cleanup,
  retained log failure, duplicate INSTALL receipts and stale ABORT rejection.
  All-target Clippy passed for the new runtime with its storage feature.
+ This checkpoint is a library/runtime port, not the completed production
  switch. Stage 3 still needs migration host/bootstrap, route and scan orchestration,
  gateway behavior and multi-process coverage. Stages 4 through 6 remain pending.
  CMake now exposes separate `rust_runtime_tests` and `rust_shard_integration_tests`
  targets alongside the existing transaction-manager and Masstree targets.
+ The migration storage adapter now has an owned Rust registry and affine
  worker. It calls the existing `shard-host-core` storage verbs and retains
  table handles until every registered worker stops. Raw/warehouse scans,
  copying/deletion, registry lifetime and foreign-database rejection passed in
  `/tmp/mako-native-rust-migration-api-01.log`: 83 adapter cases and Clippy.
  `/tmp/mako-native-rust-runtime-cmake-01.log` also confirms the new CMake
  integration target and adapter target pass. Native sharding Host callbacks
  and process bootstrap are the next step, not yet connected by this checkpoint.
+ Stage 3 migration-host checkpoint: `mako-runtime::migration` now owns native
  table provisioning, raw/warehouse handle identities, sharding Host callbacks,
  native SRPC control delivery, epoch cuts and stop/join. It reuses the existing
  sharding actors and storage verbs. Callback anchors own Weak references so
  failed startup and concurrent shutdown cannot leave dangling host pointers.
+ `/tmp/mako-native-rust-migration-integration-02.log`: 15 native runtime cases
  and all-target Clippy passed. The relocation case holds an admitted transaction
  across source freeze, commits a later value, then checks final capture,
  destination reads and source cleanup. It also relocates a warehouse into a
  lazily provisioned destination and checks local replicated-reference reads.
  A separate failed-cold-start case exercises retained opener callbacks after
  their owner has gone. The first test run exposed a test-port reservation race;
  listeners now support port zero and report their actual bound address.
+ `/tmp/mako-native-rust-cluster-tests-01.log`: all 69 existing sharding library
  tests passed after adding owned callback anchors. The previous native SRPC
  participant/coordinator checkpoint is committed as `a392f4e68`.
+ Stage 3 remains in progress. Multi-process coverage, complete paged/routed
  scan delivery, gateway behavior and the production process entry point remain.
+ Stage 3 scan checkpoint: native requests now carry the checked FullScan
  identity/cursor format. The receiver uses the existing Page serializer; the
  coordinator uses FullScan admission and completion checks. Routed scans take
  all segments from one protected snapshot, retain exact binary cursors, carry
  scan dependencies and apply limits across segments in either direction.
+ `/tmp/mako-native-rust-scans-integration-01.log`: 16 native runtime/integration
  cases and all-target Clippy passed. The new case migrates half a raw table,
  scans through both owners over multiple byte-limited pages, and checks binary
  key extensions at a page boundary, reverse inclusive bounds and global limits.
  Migration-host integration is committed as `e1e82649c`.
+ Gateway/client-service behavior and multi-process production startup remain
  before stage 3 can close. Replication, workloads, defaults and C++ retirement
  in stages 4 through 6 are still outstanding.
+ Stage 3 gateway checkpoint: native workers now execute the existing verified
  auto-commit gateway protocol, with route discovery, insert/delete result bits,
  retained read bytes, forwarding and exact terminal deduplication. Session
  grouping retains the existing behavior: each operation commits independently.
  The Rust client uses the existing verified pending-request sequencer and can
  build without the native-storage feature.
+ `/tmp/mako-native-rust-gateway-integration-03.log`: 20 runtime/integration
  cases and all-target Clippy passed. A proxy discards a successful forwarded
  write reply; the retry resolves through the destination receipt with one log
  submission. Other cases cover uncertain local commits, duplicate INSERT/DELETE
  result bits, retained GET bytes and SDK routing identity across retries.
  `/tmp/mako-native-rust-gateway-client-only-02.log`: all nine default-feature
  runtime/client unit cases passed without Masstree linking. The older RPC unit
  tests now bind port zero directly to eliminate their parallel port-picking race.
+ Paged/routed scans are committed as `8a67e366a`. The next stage-3 work is the
  native process entry point and multi-process coverage. This still does not
  switch the production default or complete replication/workload retirement.
+ Stage 3 is complete. The native-storage CMake profile now builds `mako_node`
  through Cargo. Its JSON configuration validates topology/schema before startup,
  owns loaders, listeners and migration hosts, reports readiness, and joins workers
  on SIGTERM/SIGINT or controlled stdin shutdown. The profile remains opt-in.
+ `/tmp/mako-native-rust-node-integration-01.log`: an independent child shard
  advances its own clock, then participates in a distributed commit with equal
  local/remote metadata above that dependency. Body-error cleanup releases remote
  locks. Clippy and the CMake executable target passed.
+ `/tmp/mako-native-rust-node-integration-02.log`: all 22 runtime cases and
  all-target Clippy passed. Two separate native server processes migrate raw and
  warehouse data, lazily create destination tables, route subsequent reads/writes
  through the gateway, and shut down cleanly. Schema log IDs now respect the
  existing format's delete bit instead of admitting unusable IDs above 32767.
+ Stage 4 starts with the owned replay API and durable application adapter.
  Reuse the native Raft host and existing RocksDB checkpoint machinery. Local
  proposals must not replay into their own locked Masstree rows; snapshots must
  use committed durable state and retain uncertain-outcome fencing.
+ Stage 4 replay/replication checkpoint: owned `Worker::replay` delegates to
  the existing STO compare/apply path. A checked Rust decoder consumes complete
  local log frames before exposing rows. `DurableRows` uses the existing
  sto-rocksdb atomic batch and checkpoint-generation implementation, retaining
  source metadata and tombstones. Schema identities and applied-entry hashes
  reject mismatched reopen, snapshot and replay inputs.
+ The native Raft application adapter keeps owned pending proposal identities
  so a leader never replays into its own retained locks. Followers apply through
  a dedicated affine worker. Quorum admission guards every transaction; changed
  leadership invalidates guards. Unknown installed outcomes fence the replica
  until restart rebuilds RAM from committed durable state. A former leader with
  live speculative transactions also requires recovery before foreign replay.
+ `/tmp/mako-native-rust-replication-integration-03.log`: all 28 runtime cases,
  all-target Clippy for changed application/adapter crates, and the native CMake
  server target passed. Cases cover real Raft commits, tombstones, checkpoint
  transfer to a missing peer, failover, lost-quorum fencing, and a native server
  restart in a fresh process. The first process-restart run exposed readiness
  being reported from the previous applied term; readiness now requires a known
  leader and an applied prefix in the current Raft term.
+ `/tmp/mako-native-rust-durable-01.log`: durable rows, schema refusal, duplicate
  position checks, tombstones and checkpoint-generation tests passed. Its owned
  replay test initially attached two native workers on one OS thread; the test
  now releases the first worker before checking foreign-database rejection.
  `/tmp/mako-native-rust-replication-check-01.log` confirms that replay case passes.
+ The existing runtime explicitly rejects live migration with replication replay
  (`src/cluster/runtime.rs`); native configuration preserves that restriction.
  Migration metadata restart remains inspection-only, as before. Replicated
  loading must use committed transactions after leader readiness, never the
  volatile process's initial-row shortcut. This port does not promise a new
  combined migration/consensus recovery protocol.
+ Stage 4 remains in progress: durable client retry receipts and routing after
  replica leadership changes need review and coverage, along with supported
  persistence/configuration workflows. Stages 5 and 6 remain outstanding. No
  default switch or C++ application retirement is claimed by this checkpoint.
+ Durable gateway receipts now share the committed row batch, including exact
  GET bytes and no-change INSERT/DELETE outcomes. A new leader checks receipts
  under quorum admission before executing an operation. The Rust SDK can discover
  a configured replica group's leader while retaining the exact pending request.
  Distributed clients discover participants once per attempt and pin those
  endpoints through cleanup; they never redirect an in-flight INSTALL.
+ `/tmp/mako-native-rust-receipts-crash-01.log`: a SIGKILL/restart case recovers
  INSERT, DELETE and GET receipts, returns their original results, and leaves
  later row generations unchanged. Altered requests cannot reuse that identity.
  `/tmp/mako-native-rust-replica-clients-01.log`: real replica failover preserves
  those receipts and permits the next distributed transaction to select the new
  leader. Discovery checks the configured shard and Raft group identity.
+ `/tmp/mako-native-rust-replica-clients-02.log`: all 29 runtime cases pass;
  all ten default-feature runtime/client unit tests also pass without native
  storage settings. Gateway session grouping remains tied to its gateway;
  the SDK reports closure if discovery moves an active group to another replica.
+ Remaining stage-4 work is configuration wiring for replica groups and supported
  local disk persistence. The core replicated path and its client recovery checks
  are implemented; production-default and workload retirement are still pending.
+ Stage 4 is complete. Native configuration now carries replica groups for
  gateway forwarding, and the forwarding test continues through a leader change.
  Local disk persistence uses the same atomic row/receipt store and owned replay
  path without starting Raft. Creation is explicit, sync policy is recorded, and
  uncertain local writes fence the node. Durable startup uses transactional
  loading; live migration still has no supported durable restart protocol and
  cannot be combined with these persistence settings.
+ `/tmp/mako-native-rust-persistence-final-01.log`: all 30 runtime cases and
  all-target application Clippy passed. The local disk case recovers committed
  rows and exact client receipts after SIGKILL. CMake built `mako_node` plus the
  existing native `rocksdb_raft_server`, `rocksdb_raft_client` and
  `rocksdb_raft_bench`, using Cargo/SRPC directly.
+ Stage 5 pacing: first port TPC-C schemas/codecs, loaders and supported
  transactions through the native application API, with local/sharded/replicated
  behavior checks. Then port benchmark/simple-transaction entry points and
  supported YAML/CLI workflows, including replica-group configuration and worker
  lifecycle. Only after those gates pass will the native profile become default.
  Stage 6 then removes replaced C++/transpiler paths and runs the final proof,
  storage-boundary and dependency audits.
+ TPC-C data codecs, deterministic bounded loading and all five transaction
  bodies now use the native shard client. Physical keys preserve origin-local
  warehouse IDs, customer data at district+100, and delivery balances at
  district+200. Loader progress and completion share ordinary transaction
  durability; incomplete loads verify their committed prefix and completed
  loads do not overwrite later workload changes. History date reservations
  retain a restart-safe high-water mark.
+ `/tmp/mako-native-rust-tpcc-load-tests-01.log`: all-target Clippy and three
  focused application cases passed. Concrete codec vectors match existing
  fused-kernel layouts; interrupted loading resumes; two native shards execute
  NewOrder, Payment, Delivery, OrderStatus and StockLevel. A missing late item
  aborts the order header and prior remote stock work. Workload scheduling,
  executable/CLI/config ports, persistent workload checks, and the default
  switch remain outstanding. Stage 5 is still in progress.
+ Native `mako_bench` now owns workload threads, the configured TPC-C mix,
  local loading and all-shard completion checks. Each worker owns its STO
  context and native SRPC stream. Replicas remain available after the workload;
  controlled shutdown joins work before closing the node. Single-shard runs
  can explicitly exit after completion. The native CMake profile builds it.
+ The replicated workload exposed two lifecycle gaps: readiness could conflict
  with another shard reserving dates, and a new client could not reuse a
  cleaned-up loader stream. Readiness now retries confirmed aborts. An explicit
  OPEN binds a receiver-generated lease; replacement requires physical cleanup,
  stale requests cannot use retired leases, and uncertain installed outcomes
  remain fenced. Paged scans bind their identity to that participant lease.
  Failed remote cleanup now returns Unknown immediately instead of a retryable
  conflict followed by a later fence. The unreleased native wire version is 2.
+ `/tmp/mako-native-rust-stream-handoff-03.log`: all 38 runtime cases,
  all-target Clippy, and native CMake server/benchmark targets passed. Coverage
  includes two shard processes with three Raft replicas each running remote
  NewOrders, verified against committed order counters; three local workload
  threads; and two executable runs reopening the same persistent TPC-C data
  without reloading it. The separate default-feature library suite has 11
  passing cases (`/tmp/mako-native-rust-stream-handoff-02.log`). Earlier runs
  exposed the startup conflict, client handoff and scan identity issues above.
+ Remaining stage-5 work: the micro workload, configuration/CLI compatibility,
  simple-transaction/examples and colocated orchestration, then the default
  switch. The old production profile and C++ application remain until those
  ports pass. Stage 6 retirement and final proof/dependency audits are pending.
+ The native benchmark now also runs the micro workload: four operations by
  default, a configurable read/write mix, and an optional first access on a
  different shard. It preserves the existing big-endian item keys and padded
  eight-byte values. Micro loading uses the same bounded resumable transaction
  path, keeps a separate completion marker, and refuses mixing TPC-C and micro
  layouts in one database. Metrics identify the selected workload.
+ `/tmp/mako-native-rust-micro-tests-01.log`: all-target Clippy, the two-shard
  micro workload/rollback case, interrupted-loader regression, and persistent
  executable restart regression passed. Configuration/CLI ports, remaining
  examples and the production default switch are still outstanding.
+ Native configuration now accepts typed JSON and YAML, including anchors,
  through the Rust `serde-saphyr` parser with only its deserialize feature.
  Configuration files are bounded to 4 MiB. A benchmark file can select one
  node or several colocated shards; validation rejects ambiguous selection,
  duplicate owners/listeners/directories and incompatible local topologies.
  The supervisor starts, monitors and joins each shard's workload separately.
  Complete unreplicated local clusters can exit after all their workers finish.
+ `/tmp/mako-native-rust-yaml-colocated-01.log`: all-target Clippy passed,
  as did YAML anchor/invalid-configuration checks, two colocated shards running
  the micro workload, the two-shard/three-replica TPC-C executable case, and
  persistent executable restart. Legacy CLI/configuration translation and
  remaining application examples are next; no default switch is claimed.
+ Native CLI compatibility now reads both existing topology formats, including
  site selection, shard selection and colocated shard lists. Worker count,
  runtime, readiness timeout, CPU duty cycle, Rust engine selection and the micro
  workload have native options. Raft configuration accepts mode/site fragments
  and explicit shard-qualified files for colocated groups. Durable topology mode
  requires an explicit data directory; `--create` allows first creation.
+ Per-worker Raft logs convert to one native log per shard using the first
  group's peer ports. Conversion checks membership consistency and reports the
  grouping change and retired mode batch/retry knobs. The native runtime uses
  committed load markers instead of the old `--sync-dir` barrier. `--print-config`
  emits validated JSON without starting listeners or creating storage.
+ `/tmp/mako-native-rust-compat-tests-01.log`: all-target Clippy, four native
  configuration/CLI cases, the multithreaded TPC-C case and the two replicated
  shard workload passed. Tests read the repository's role-based and site-map
  YAML and its six-group Raft configuration, checking ports, membership, worker
  selection and the absence of filesystem side effects during conversion.
  Remaining stage 5: application examples and launch scripts, then default
  native build/CI. Stage 6 C++/transpiler retirement remains pending.
