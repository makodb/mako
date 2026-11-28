# Distributed STO consolidation qualification

Completed in `/var/tmp/mako-shard-rust`, branch `rust-rocksdb-compatible-api`,
on the working tree based at `7cf616061c009991c5de96aa938ca79709c454ac`.
This includes the previously staged storage-wrapper/workload extraction. No
new commit or remote update is represented by this record.

The [plan](../../plans/distributed-sto-consolidation.md) records ownership and
scope. All commands below ran inside the build container with Rust 1.97.1.
Production Verus files and tool pins are hashed in [upper-report.json](upper-report.json).

| Check | Result | Evidence |
|---|---|---|
| Upper protocol gate, 32 CPUs | 180.53 seconds; 218 Rust cases; 1497 sharding + 40 host + 42 STO + 10 historical + 1 retention verified obligations; 29 semantic controls rejected | [log](sto-upper.log), [report](upper-report.json) |
| STO and synthetic datatypes, all features | 469 test/doctest passes, including 72 capability doctests; 18 compile-fail fixtures checked | [tests](sto-core-full.log), [strict Clippy](sto-core-clippy.log) |
| Local-only STO with clock/install test hooks | 389 test/doctest passes | [log](sto-local-core.log) |
| Clock mutations | Nine active mutants killed by their named tests; three retired cache controls recorded separately | [report](sto-clock-mutations.json) |
| Independent Masstree transaction component | 307 fresh-process cases, four doctests, strict wrapper Clippy and dependency audit | [log](sto-masstree-component.log), [dependencies](masstree-dependencies.json) |
| Native runtime, RocksDB facade and workloads | 130 fresh-process cases; strict Clippy; native node/workload build and support tests | [tests](sto-native.log), [Clippy](sto-clippy.log), [build](sto-native-build.log) |
| RocksDB transaction and Raft-host components | 47 cases | [log](sto-rocks-components.log) |
| RocksDB-only runtime and facade | 51 fresh-process cases | [log](sto-rocks-runtime.log) |
| RocksDB-only production build | Dependency graph and eight executable symbol audits | [build](sto-rocks-build.log), [audit](rocks-runtime-audit.json) |
| Two-shard RocksDB TPC-C correctness smoke | Completed successfully; all child processes joined | [log](sto-rocks-tpcc.log) |

The fast gate was `python3 scripts/upper_check.py --cpus 32`. Component commands
were `cargo test -p sto-core -p sto-test-datatypes --all-features`, the same STO
command with `--no-default-features --features test-install-observer,clock-test-hooks`,
and `ci/masstree_tx.sh`. Cargo commands used `--locked --manifest-path
crates/Cargo.toml`. Native application cases used `scripts/run_native_rust_tests.py`
with runtime replication/RocksDB, facade Masstree and workload replication/RocksDB
features. The RocksDB-only CMake profile ran `rocks_runtime_audit` then
`rust_runtime_tests`; its smoke command was `python3 examples/run_native.py tpcc
--build build_rocks_crate/runtime --engine rocksdb --shards 2 --workers 2
--count 30 --seconds 30`. This is a correctness smoke, not a performance campaign.

CI-selection tests, Masstree dependency negative controls and workload-boundary
negative controls passed, as did `check_sto_boundary.py` and
`check_workload_boundary.py`. External dependency versions remain unchanged.
The upper source audit verifies canonical module mappings, pinned Verus dependencies
and absence of conditional proof bypasses. It does not claim verification of all
STO or runtime implementation code.

One initial RocksDB conversion case hit a checkpoint RPC deadline while the upper
proof gate was also running. The [initial log](sto-rocks-runtime-initial.log) is
retained. The [isolated rerun](sto-rocks-conversion-recheck.log) passed in 0.7 seconds,
and the full RocksDB rerun passed without code or timeout changes. The failure
was not reproduced; no cause or fix is claimed.
