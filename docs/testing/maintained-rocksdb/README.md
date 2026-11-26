# Maintained RocksDB qualification

This change replaces the handwritten row, log and sharding-metadata C bindings
with `rocksdb = 0.25.0`, using `librocksdb-sys = 0.19.0+11.8.1`. Mako's
`mako-rocksdb-store` retains the byte-store contract and storage policies.
Upstream owns native handles, FFI and memory management. Owned snapshots retain
the database through `self_cell = 1.3.0`.

## Forward compatibility fixture

Before changing the bindings, `compat-probe.rs` wrote the bundled
`legacy-8.9.1.tar.gz` using the container's system RocksDB 8.9.1 and the original
adapters. It wrote binary and empty values, a checkpoint, a later tombstone, a
log range deletion and unflushed WAL suffixes. It exited without destructors.
The archive checksum is recorded in `legacy-fixture.sha256`; the archive has
not been rewritten by the new engine.

The same probe's `read` mode checks all values through the new adapters. The
successful run is in `legacy-reopen.log`. This qualifies the fixture's forward
recovery; it does not promise that older RocksDB can reopen files modified by
the new engine, or replace qualification of every historical database option.

To reproduce inside the development container, extract a fresh copy of the
archive and create a temporary Cargo package outside the main workspace with
`compat-probe.rs` as `src/main.rs`. Declare these dependencies with paths resolved
from the temporary package:

```toml
[workspace]
[dependencies]
row_store = { package = "mako-rocksdb-store", path = "../../crates/mako-rocksdb-store" }
raft-store = { path = "../../src/deptran/raft/store", features = ["rocksdb"] }
```

Then run `cargo run -- read PATH_TO_EXTRACTED_legacy-8.9.1`. The `write` mode
must use the old adapters and engine to reproduce the original fixture.

## Validation

All builds and tests run in the development container with Rust 1.97.1 and a
32-job Cargo budget. Performance tests and a full sanitizer campaign are
separate from this correctness qualification.

| Check | Result | Evidence |
|---|---|---|
| Row store, transactional adapter and Raft host | 68 tests passed; `snapshot_process` remains an ignored child fixture | `components.log` |
| Runtime/API with optional Masstree | 128 fresh-process cases passed | `native.log` |
| Raft store | 47 tests passed | `raft-store.log` |
| RocksDB-only runtime/API | 49 fresh-process cases passed | `rocks-runtime.log` |
| RocksDB-only graph/symbols | 8 executable audits passed; no Masstree dependencies | `rocks-runtime-audit.json` |
| Native production links | 7 executable links and 27 Rust archives passed | `native-dependency-audit.json`, `native.log` |
| Application strict Clippy and doc tests | Passed; 2 doc tests passed | `native.log` |
| Adapter strict Clippy | Passed with warnings denied | `adapters-clippy.log` |
| Old 8.9.1 SST/checkpoint/WAL recovery | Passed | `legacy-reopen.log` |
| Sharding metadata and actors | 69 tests passed, including corruption, uncertain settlement and writer exclusion | `metadata.log` |
| Upper protocols | Passed in 231.862 seconds on 32 CPUs, including Verus and semantic negative controls | `upper-report.json`, `upper.log` |
| TSan native dependency | 363 instrumented translation units; shared library hooks checked | `tsan-dependency.log`, `tsan-instrumentation.json` |
| CI selection and census/accounting unit tests | 11 + 15 passed | `selector.log`, `python.log` |

All listed checks passed. Both runtime profiles were qualified sequentially.
The container dependency script no longer installs system `librocksdb-dev`;
codec development packages remain for the external TSan build. The TSan
library's C header matches the crate's bundled header byte for byte. A full
container image rebuild and full Rust sanitizer campaign were not run.

The first runtime attempts shared a Cargo output directory while building two
feature profiles concurrently. The RocksDB-only `mako_node` replaced the native
suite's child executable, producing an explicit "Masstree data engine is not
compiled" error. Those attempts are retained in `*-concurrent-attempt.log`.
A separate checkpoint RPC timed out during that concurrent run; the unchanged
case passed alone in 0.80 seconds in `conversion-recheck.log`. Final profile
qualification runs sequentially to prevent executable replacement.

The component census counts 27 Cargo packages and 7 support units. First-party
nonblank source decreased by 789 lines; all counted files decreased by 840 lines.
The storage-contract component now has 554 source lines, including contract
comments and inline tests, down from 1,048. External RocksDB and its Rust binding
are excluded from first-party counts.


For normal component and RocksDB-only runtime qualification, run
`CI_BUILD_JOBS=32 bash ci/replication.sh` inside the development container.
For the optional Masstree profile, configure `NATIVE_RUST` and run the
`rust_shard_integration_tests`, `rust_native_quality` and
`rust_native_dependency_audit` CMake targets. Run profiles sequentially when
sharing a Cargo target directory. `python3 scripts/upper_check.py --cpus 32`
is the independent upper protocol gate.

`consistency.log` records proof input identity, documentation links and the
original fixture archive hash. `inventory.log` records the refreshed census.

Saved text logs have trailing whitespace and final blank lines normalized.
