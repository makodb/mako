# Standalone service retirement qualification

Qualified on 2026-11-26 UTC.

This change removes the separate RocksDB/Raft service. The supported application
is the distributed runtime at any shard count, with local or Raft logging and
optional Masstree acceleration over persistent RocksDB rows.

Removed entry points include `rocksdb_raft_server`, `rocksdb_raft_client`,
`rocksdb_raft_bench`, their RPC/C client API, the `ROCKSDB_RAFT` CMake profile and
the service-only launchers. The native `OptimisticTransactionDB` benchmark wrapper
was removed from `mako-rocksdb-ffi` after its last application consumer was deleted.
The Rust `mako-rocksdb` compatibility API continues to use the distributed protocol.

The replacement `ci/replication.sh` checks shared storage/Raft components and
the `ROCKSDB_RUNTIME` application profile. The component gate receives no Masstree
native-link settings. Performance remains in the separate weekly/manual suite.

## Coverage retained in the main stack

| Behavior | Current test owner |
|---|---|
| Atomic writes across column families, read-your-writes, rollback, point/range conflicts, iteration and reopen | [`mako-rocksdb/tests/native.rs`](../../../crates/mako-rocksdb/tests/native.rs): the same `transaction_contract` runs with local and Raft logs, with and without Masstree |
| One shard replicated across three members, leader loss and saved transaction outcome | `replicated_facade_keeps_followers_open_and_resolves_after_failover` in the same API suite |
| Checkpoint/restart, lagging replica recovery, unavailable quorum, leader discovery and prepared intent recovery | [`replication_contract.rs`](../../../crates/mako-runtime/tests/support/replication_contract.rs), instantiated by both storage adapters |
| Crashes between durable commit and row materialization; failed application and recovery | [`materialization.rs`](../../../crates/mako-runtime/tests/materialization.rs), [`participant.rs`](../../../crates/mako-runtime/tests/participant.rs) and [`protocol.rs`](../../../crates/raft-host/tests/protocol.rs) |
| Stale leader read rejection, uncertain proposal resolution, interrupted snapshot staging/authority/publication | [`raft-host/tests/protocol.rs`](../../../crates/raft-host/tests/protocol.rs) |
| Log formats, WAL recovery and durable base storage | `raft-store` component tests |

The new one-shard transaction contract cases use a single-member Raft group to
exercise replicated logging deterministically. The existing three-member tests
cover quorum and failover. Cross-shard rejection from the removed service is not
a requirement of the distributed API; its multi-shard transaction tests remain.
The removed C API and service JSON configuration are not compatibility promises
of the current application.

## Commands

All compilation and tests run in the development container with the pinned Rust
toolchain. This working tree uses base revision
`95e7b225b1b1383169a55382186dc2de52de61bc`, plus the cache and service retirement
changes. Build directories and Cargo artifacts are local qualification outputs.

```sh
CI_BUILD_JOBS=32 \
  CARGO_TARGET_DIR="$PWD/build_docker/rocks-runtime-profile/cargo" \
  REPLICATION_BUILD_DIR="$PWD/build_service_retirement/rocks" \
  bash ci/replication.sh

cmake -S . -B build_cache_retirement -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DMAKO_NATIVE_CARGO_PROFILE=dev \
  -DMAKO_NATIVE_CARGO_TARGET="$PWD/build_retired/cargo"
cmake --build build_cache_retirement --parallel 32 \
  --target rust_shard_integration_tests rust_native_quality rust_native_dependency_audit

python3 scripts/upper_check.py --cpus 32
python3 -m unittest discover -s scripts/ci
python3 -m unittest discover -s scripts/tests
python3 scripts/component_inventory.py --check
```

## Results

| Check | Result | Evidence |
|---|---|---|
| Shared transaction, clock, storage and Raft components | 869 passing test/doc-test executions across feature configurations | [replication.log](replication.log) |
| Native runtime/API with optional Masstree | 128 fresh-process cases passed | [native.log](native.log) |
| Upper protocol gate | Passed in 155.903 seconds on 32 CPUs; source hashes match | [upper-report.json](upper-report.json), [upper.log](upper.log) |
| RocksDB-only runtime/API | 49 fresh-process cases passed | [replication.log](replication.log) |
| RocksDB-only dependency/symbol audit | All 8 executable audits passed | [rocks-runtime-audit.json](rocks-runtime-audit.json) |
| Native production links | All 7 executable links and 27 Rust archives passed | [native-dependency-audit.json](native-dependency-audit.json) |
| Native strict Clippy and application doc tests | Passed with warnings denied; 2 doc tests passed | [native.log](native.log) |
| Binding strict Clippy and all-target compilation | Passed with warnings denied | [binding-clippy.log](binding-clippy.log), [binding-check.log](binding-check.log) |
| Python CI/inventory/accounting checks | 11 + 15 unit tests passed | [selector.log](selector.log), [python.log](python.log) |
| Removed build/dependency entry points | Retired profile rejected, service targets and Cargo dependencies absent | [retired-profile.log](retired-profile.log), [build-targets.txt](build-targets.txt), [workspace.json](workspace.json) |

All checks passed. No performance or full sanitizer campaign was run for this
retirement.

The upper gate includes 49 workspace tests, 24 sharding tests, production Verus
proofs and semantic negative controls. Verification errors reported for those
negative controls are expected rejections. All recorded source hashes match
the final files.

The refreshed inventory contains 27 Cargo packages and 7 support accounting
units, grouped as 8 implementation components plus shared tests/models/tooling.
Total nonblank source lines decreased from 339,246 to 333,076, a reduction of
6,170. The generated census, manual component sums and current documentation
file links were checked.
