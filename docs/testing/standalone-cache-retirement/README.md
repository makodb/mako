# Standalone cache retirement qualification

Completed on 2026-11-26 UTC in `/var/tmp/mako-shard-rust`, branch
`rust-rocksdb-compatible-api`, over base revision
`95e7b225b1b1383169a55382186dc2de52de61bc`. These results cover the working-tree
retirement changes, not the unmodified base revision.

The supported distributed storage composition is RocksDB with optional Masstree
acceleration. Both modes retain the common OCC + 2PC protocol, local/Raft journal,
checkpoint and recovery formats. The standalone `mako-cache` and `mrx` stacks,
their cache-only facades, tools, C API and `mtx` ABI were removed. The shared
timestamp moved into `mako-clock`; byte-store contracts and the existing RocksDB
binding now live in `mako-rocksdb-ffi`. The direct RocksDB/Raft service remains.

The retirement removes 11 packages, leaving 28. The census contains 36 accounting
units and 339,246 total nonblank source lines, down 55,609. See
[`agents.md`](../../../agents.md) for logical components and
the [plan](../../plans/retire-standalone-caches.md) for scope.

## Results

| Check | Result | Evidence |
|---|---|---|
| Shared transaction, storage, clock and support suites | 447 test/doc-test cases passed | [native-tests.log](native-tests.log) |
| Runtime and Rust RocksDB-compatible API with optional Masstree available | 126 cases passed in fresh processes | [native-tests.log](native-tests.log) |
| RocksDB-only runtime and API | 48 cases passed in fresh processes | [rocks-only.log](rocks-only.log) |
| RocksDB-only dependency and symbol audit | All 8 executables passed, with no Masstree linkage | [rocks-runtime-audit.json](rocks-runtime-audit.json) |
| Final clock, RocksDB binding and transactional adapter tests | 76 cases passed | [contracts.log](contracts.log) |
| Strict Clippy for moved contracts and native application targets; application doc tests | Passed with warnings denied; 2 doc tests passed | [contracts-clippy.log](contracts-clippy.log), [quality-audit.log](quality-audit.log) |
| Native source audit | 15 storage C++ translation units; no application or generated C++ | [native-build.log](native-build.log) |
| Native dependency audit | 10 production executable links and 33 Rust archives passed | [native-dependency-audit.json](native-dependency-audit.json) |
| Isolated upper protocol gate | Passed in 166.949 seconds using 32 CPUs, within the 300-second budget | [upper-report.json](upper-report.json), [upper.log](upper.log) |
| Python inventory, CI, benchmark-driver accounting and Miri-driver checks | 15 + 11 + 4 unit tests passed | [python.log](python.log) |

The integration suites cover switching engines against the same persistent
files, migration in both storage modes, checkpoint/reopen, local and replicated
recovery, failover and uncertain commit outcomes. The upper gate runs 49 workspace
tests, 24 sharding tests, production Verus proofs and semantic negative controls.
Verification errors in `upper.log` belong to mutations that must be rejected.
All source hashes recorded in `upper-report.json` matched the final source.

The active Cargo metadata and CMake target list contain none of the retired
packages or cache tools. See [workspace.json](workspace.json) and
[build-targets.txt](build-targets.txt). The generated census check and local
documentation link check passed.

## Reproduction

All compilation, tests and proofs ran through the repository's development
container with Rust 1.97.1 and `CARGO_BUILD_JOBS=32`. The native build used a fresh
CMake directory and the existing Cargo artifact directory:

```sh
cmake -S . -B build_cache_retirement -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DMAKO_NATIVE_CARGO_PROFILE=dev \
  -DMAKO_NATIVE_CARGO_TARGET="$PWD/build_retired/cargo"
cmake --build build_cache_retirement --parallel 32 \
  --target rust_sto_transaction_tests rust_support_tests rust_shard_integration_tests
cmake --build build_cache_retirement --parallel 32 \
  --target rust_native_quality rust_native_dependency_audit

cmake -S . -B build_cache_retirement/rocks-only -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DMAKO_BUILD_PROFILE=ROCKSDB_RUNTIME \
  -DMAKO_RUST_TARGET_DIR="$PWD/build_docker/rocks-runtime-profile/cargo"
cmake --build build_cache_retirement/rocks-only --parallel 32 \
  --target rust_runtime_tests rocks_runtime_audit

python3 scripts/upper_check.py --cpus 32
python3 scripts/component_inventory.py --check
```

The final focused checks used:

```sh
export CARGO_TARGET_DIR="$PWD/build_retired/cargo"
cargo clippy --locked --manifest-path crates/Cargo.toml \
  -p mako-clock -p mako-rocksdb-ffi --all-features --all-targets --no-deps -- -D warnings
cargo test --locked --manifest-path crates/Cargo.toml \
  -p mako-clock -p mako-rocksdb-ffi -p sto-rocksdb \
  --features mako-clock/test-hooks,mako-rocksdb-ffi/required-native,sto-rocksdb/test-hooks
python3 -m unittest discover -s scripts/tests
python3 -m unittest discover -s scripts/ci
python3 -m unittest discover -s scripts -p test_run_rust_sto_miri.py
```

## Test limits

The full integration runs preceded a final transparent `Row` type alias in the
RocksDB binding. That alias resolves an existing Clippy type-complexity warning
without changing behavior. The 76 focused component tests, strict Clippy and
final native link audit ran after that change. The upper gate's source inputs
were unchanged.

Sanitizer and Miri entry points were updated and their driver checks passed;
full sanitizer/Miri campaigns were not rerun. No new unsafe implementation was
introduced. Performance campaigns were not run. Historical cache qualification
reports remain records of their original revisions.
