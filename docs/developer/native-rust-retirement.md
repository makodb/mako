# Native Rust source retirement

The application now runs through Cargo/rustc. Root CMake builds its native
storage dependency and invokes Cargo; it does not translate Rust to C++.
`NATIVE_RUST` is the default production profile. `ROCKSDB_RAFT` remains the
separate direct-storage service profile.

The migration is complete. The [completed plan](../plans/native-rust-production.md#completion-evidence)
records the production build, dependency audits, regression/proof gates and
full ASan, UBSan and TSan qualification, with their tested revisions.

## Remaining native code

`cmake/native-storage-sources.txt` lists the 16 first-party C++ production
translation units: Masstree implementation, `mtree_abi`, `mtx_abi`, storage
thread registration, allocator, counters, RCU, ticker and storage runtime.
`cmake/native-storage-headers.txt` records the required Mako header closure.
Root `third-party/rusty-cpp` supplies handwritten storage utility headers.
Its transpiler is not built or invoked. RocksDB uses its external C ABI.

Masstree's separate C++ tests and benchmark remain under `src/masstree/tests`.
Their vector/map/set oracles use the C++ standard library, so running the tree
suite does not require generating C++ collections from Rust.

SRPC retains nine C OS-binding files, architecture fiber assembly and their
C declarations. `src/srpc/scripts/native-kernels.json` pins their contents.
They implement platform operations, not application or transport policy.
There is no SRPC C++ build or provider, including a generated one.

Historical C/C++ snippets in archived reports and benchmark platform tools
are not production build inputs. Vendored storage/test dependencies remain
third-party code. The dependency audit checks what executables actually link.

## Replacement mapping

| Retired implementation or driver | Native implementation and regression gate |
| --- | --- |
| Paxos protocol, workers, service and mode | Removed; Raft is the supported replication protocol. Unsupported selection fails. |
| C++ transaction manager and workload wrappers | `sto-core`, `sto-test-datatypes`, `sto-tpcc-ffi`; independent transaction tests, owned API tests and compile-fail worker-affinity doctest |
| C++ shard clients, receiver and transaction admission | `mako-runtime`; participant/coordinator integration, conflict cleanup, replayed messages, stream leases and unknown-outcome fencing |
| C++ migration host and storage fixture drivers | `src/cluster`, `shard-host-core`, `mako-runtime::migration`; raw/warehouse relocation, live admission drain, paginated scans and separate-process migration |
| C++ Raft host and transpiler shell | `raft-host` using retained `raft-core`/`raft-store`; quorum/no-op/read-barrier, isolation, failover, divergent tails, compaction and crash/restart tests |
| C++ replay and persistence integration | Runtime replication/local persistence; durable receipts, prepare/decision uncertainty, checkpoint installation, SIGKILL recovery and resumable loads |
| `dbtest`, `sto_tpcc_bench`, simple transaction binaries | `mako_bench`, `mako_transactions`, `mako_node`; all five TPC-C operations, concurrent orders, counters, micro workload and native topology matrix |
| C++ gateway and Redis wrapper | `mako_client`, `mako_admin`, `mako_redis`; native process tests for clients, migration administration, binary values and transactional MULTI/EXEC |
| C++ SRPC runtime and generated adapter tests | Canonical Rust SRPC workspace tests/doctests; copied-source independence with C++ tools disabled, native kernel inventory and negative controls |
| C++ RocksDB service glue and old kill-test driver | `rocksdb-raft-service`; `ci/rocksdb_raft.sh`, process fault boundaries, missing-peer snapshots and recovery |
| Legacy Raft performance launchers | `rocksdb_raft_bench` and `scripts/rocksdb_raft/performance.py`; matched native OCC/STO and replicated-service measurements |
| Old C++ TPC-C PGO/relink scripts | Removed with their C++ executable graph. Native workload measurement uses `mako_bench`; Rust-only tree PGO remains in `build_sto_masstree_pgo.sh`. Old PGO artifacts are not claimed as native results. |

The obsolete C++ launcher scripts have been removed. Familiar supported
`examples/test_*.sh` entry points dispatch to `examples/run_native.py`.
Historical result processors and data remain for reading old experiments;
they do not start a supported application.

## Audit and test boundaries

`ci/native_rust.sh audit` checks the compiler source/header closure, native
archive symbols, Cargo link metadata, Rust archive members and link maps for
all 12 production programs. Rust archives cannot hide bundled C++ objects.
The external gateway/admin clients must not link Masstree.

Transaction-manager tests never receive native linkage settings. Tree tests
and application integration are separate gates. The native poison proof builds
a test-only helper while checking that production inputs stay unchanged and
that no helper symbol appears in production storage.

The sanitizer driver preserves exact quarantine identities, leak tripwires,
instrumentation checks and a single rebuilt std for ASan/TSan. Proof gates
retain semantic negative controls. See the
[plan and recorded evidence](../plans/native-rust-production.md) for completion
status; a rewritten gate is not considered qualified until it has run.

## Unsupported combinations

Live migration rejects Raft replication and local disk persistence. Migration
metadata reopen is inspection, not data-serving recovery. Native disk formats
do not import the old C++ WAL. Unknown installed outcomes fence the runtime;
the subsequent [OCC + 2PC change](../mako-book.md#durability-and-recovery) adds
durable prepared-transaction and coordinator-decision recovery.
Gateway operations auto-commit, while Redis MULTI/EXEC is transactional.
