# STO boundary and workload extraction

The generic storage package is now `sto-masstree-api`. It depends only on
`sto-core`, `mako-clock`, `sto-masstree` and `masstree`. Its component build
excludes protocol decisions, sharding, node runtime, RPC, Raft and workloads.
TPC-C was already Rust. Its implementation, benchmark driver/configuration,
`mako_bench` executable and tests moved into the leaf `mako-workloads` package.
No production, development or build dependency may point into that package.

## Retired interfaces and retained coverage

| Retired implementation | Supported implementation and coverage |
|---|---|
| `sto-tpcc-ffi` fused Payment/PaymentPrefix, NewOrder, Delivery and StockLevel kernels, request structs and dense Item/Stock cache policies | `mako-workloads/src/tpcc`; the moved `tests/tpcc.rs` checks codec corruption, all five operations, remote rollback, concurrent orders, totals, loader resume and persistent reopen. These implementations already used the current transaction client. |
| Scalar coordinator callback requests, remote-stage/flush and submit-log/commit callbacks in the old wrapper | `mako-protocol::commit` and runtime client/prepared services; `coordinator.rs` checks the global lock barrier and failure outcomes, `participant.rs` checks frozen prepares, replay, decision recovery and fencing. |
| Old wrapper `shard_host.rs` and owned `native::Migration` API | Runtime `migration_storage.rs` and `migration_ranges.rs`; migration/process/durable-migration tests check raw/warehouse scans, binary cursors, protected transfer, mixed engines, restart and failover. |
| Old `sto_tpcc_*`/`mako_sto_tpcc_*` storage names and `sto_tpcc_ffi.h` | Generic `sto_storage_*`/`mako_sto_storage_*` storage API and `sto_storage.h`; generic ABI, owned-handle, capacity, lifetime, cleanup and quarantine tests remain, plus C11 signature checks. |

Tests that only invoked deleted kernels or callbacks were retired with those
interfaces. The two local participant tests from the old `protocol.rs` suite
remain in `tests/participant.rs`, including historical tombstone/cardinality
assertions. Their helper now drives lock/timestamp/validate/install/finish
explicitly. The fixed-mutation cleanup test still injects the native close
failure and asserts that fatal cleanup takes precedence over retry/capacity.
The post-install quarantine case remains in the sanitizer inventory.

No safety, recovery or semantic-negative-control oracle was weakened. All
shard-host algorithms and E1-E7 generic contract proofs remain. The production
source audit now maps `scan_arguments` and `fixed_scan` to runtime migration
storage. The removed C shell no longer supplies E1-E7 effects. Upper storage,
range-guard and OCC+2PC adapter mappings cover the active composition; this
mapping audit is not a proof of the native adapter implementation.

The old crate name, fused C ABI and workload-specific cache policies are retired
compatibility entry points. The supported Rust RocksDB facade, transaction
client, executable names, configuration syntax and persistent formats remain.
No performance claim is made for this change; historical fused-kernel timings
must not be treated as measurements of the current workload. Performance tests
remain weekly/manual.

## Qualification

All builds/tests ran inside the development container. Source edits stopped
while checks ran. Native tests used fresh processes for each case.

- Isolated Masstree transaction component: 307 cases, four doctests, dependency
  and compiled-library audit, boundary negative controls and C11 header check.
- Native runtime, RocksDB facade and leaf workloads: 129 cases plus one
  compile-fail doctest. This includes the moved TPC-C and process/config tests.
- Strict Clippy for `sto-masstree-api`, `mako-runtime`, `mako-workloads` and
  `mako-rocksdb`: passed with all targets and native/logging/RocksDB features.
- Upper gate: passed in 177.819 seconds on 32 CPUs. Production proof totals are
  1,497 sharding, 40 host and 53 transaction-core obligations, plus semantic
  negative controls and engine-free scenarios.
- Workload leaf audit and three negative-control tests, Masstree boundary tests,
  inventory regression tests and CI gate-selection tests: passed.

- NATIVE_RUST and ROCKSDB_RUNTIME executable builds: passed. The RocksDB-only
  audit checked runtime, facade and workload dependency trees and all eight
  executables for forbidden Masstree symbols.
- RocksDB-only runtime/facade: 50 cases passed. A two-shard, two-worker TPC-C
  run passed with `--engine rocksdb`, committed transactions on both shards
  and joined every child process. The initial smoke run exposed the launcher's
  implicit Masstree configuration; the explicit engine option fixes that case
  and is exercised by the replication CI gate.
- The standalone MASSTREE_TX CMake build passed its native source audit and
  compiled-library audit with eight packages and no application dependencies.
- Miri-driver regression tests: four passed. This is validation of the renamed
  gate configuration, not a new Miri interpreter or sanitizer campaign.
- Final component inventory and grouped dependency DAG checks: passed.

Saved evidence includes the component dependency audit, upper report, native
and component test logs, and final build/workload checks in this directory.
Full build/audit logs remain under `build_docker/sto-boundary-*.log`, including
the initial failing launcher invocation. The upper run directory is
`build_docker/upper/runs/20261126T192113-dw0dqvdz`.
