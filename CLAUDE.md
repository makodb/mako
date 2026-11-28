# Working on Mako

Read `README.md` and `docs/developer/development.md` for the current build.
The production application is native Rust. Do not restore the retired C++
application, Paxos or Rust-to-C++ generation paths.

## Build discipline

Build and run tests inside the build container. Host-side source edits, reads
and Git operations are fine. Use `./docker_build.sh` for the normal workflow;
a worktree-specific container runner may already be configured in a session.
Do not modify a different worktree to make this worktree pass.

Default CMake profile: `NATIVE_RUST`. The other production profile is
`ROCKSDB_RUNTIME`. Rust code uses Cargo/rustc and the workspace toolchain pin.
Do not compile SRPC or Lion into C++.

Keep production C++ confined to `cmake/native-storage-sources.txt` and its
reviewed header closure. Masstree component tests have their own CMake profile,
`MAKO_STORAGE_TESTS_ONLY=ON`. RocksDB is an external storage dependency.
SRPC's small C/assembly platform kernel has a pinned inventory and negative
controls under `src/srpc/scripts`.

## Code ownership

- `crates/sto-core`: distributed STO, including clocks, OCC+2PC driver and verified
  decisions. Independent of runtime, sharding, RPC, Raft and concrete engines.
  `protocol` enables wire/journal/coordination; storage wrappers use `distributed`.
- `crates/sto-test-datatypes`: synthetic transaction test datatypes.
- `crates/masstree`, `mtree-sys`: index and ABI layers, separate from transaction support.
- `crates/sto-masstree`, `sto-masstree-api`: Masstree transaction component. Owns
  native C++ byte rows/locks/history and thin Rust STO callbacks, safe database/worker/table/transaction API and retained local participant support. The redundant Rust C storage ABI is retired.
  Native row ownership is in `src/mako/storage/mtree_rows.{cc,h}`; safe FFI handles
  are in `crates/masstree/src/native_rows.rs`, counted with this component.
- `src/cluster`: native sharding and migration actors and proofs.
- `crates/shard-host-core`: storage-independent host rules and proofs.
- `crates/mako-runtime`: workers, participants, native SRPC, migration host,
  gateway, Redis, generic configuration and production programs.
- `crates/mako-workloads`: Rust TPC-C, micro workload, benchmark configuration and
  `mako_bench`. No component may depend on this leaf package.
- `src/deptran/raft/{core,store,replay}`: Raft state machine and durable formats.
  The former C++ host and Rust transpiler shell have been removed.
- `crates/raft-host`: native Raft runtime using SRPC.
- `crates/mako-storage-rocksdb`, `mako-storage-masstree`: data adapters for the
  distributed runtime, including one-shard deployment. RocksDB persists rows;
  Masstree is optional. The standalone cache and RocksDB/Raft service are retired.

## Tests

`ci/native_rust.sh` exposes compile, components, storage, quality,
runtime, workloads, audit, srpc and proofs gates. `all` runs the application
build and regression gates; SRPC independence and proofs are separate CI jobs.
`ci/replication.sh` qualifies shared storage/Raft components and the distributed
RocksDB runtime, including one-shard transactions and failure recovery.
`./docker_build.sh ci masstreeTx [jobs]` independently compiles and tests the
Masstree transaction component. Its `MASSTREE_TX` profile and dedicated Cargo
directory exclude RocksDB, SRPC, Raft and application packages. Preserve the
dependency audit; use `masstree_tx_build` for compilation without tests.

Keep transaction-manager tests independent of Masstree. Test Masstree and its
ABI separately, and use real storage only for adapter/application integration.
Some tests deliberately poison process-global native state. Use the checked-in
fresh-process runners instead of aggregating those cases into one process.

Do not weaken a proof, oracle, quarantine inventory or sanitizer gate to make a
port pass. Preserve test behavior when moving its implementation. Document
unsupported behavior explicitly; target deletion alone is not a completed port.

Launchers must stop and join only children they own. Keep failed-run logs and
configuration. Avoid global `pkill`, shared temporary database paths and
unbounded shell polling. A passing workload includes checking exit statuses and
its transaction invariants.

## Current limits

See README's behavior section. Durable live migration requires synchronous
application-log recovery with LocalLog or ReplicatedLog. The older standalone
metadata inspection path cannot serve recovered data. Uncertain installed transactions fence
the runtime. Gateway operation sessions are auto-commit; Redis MULTI/EXEC uses
real transactions. Native durable files do not import the old C++ WAL.

Historical documents and experiment data record older revisions. Use current
source and build manifests for implementation decisions. Keep checkpoint
status and evidence in `docs/plans/native-rust-production.md` until the active
migration is complete.
