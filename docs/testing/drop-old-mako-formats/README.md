# Current-format storage qualification

Qualified on 2026-11-27 in the configured build container. The
[cleanup plan](../../plans/drop-old-mako-formats.md) is complete.

Removed the prefixed-key reader, legacy row mirror, file-only checkpoint reader,
old row-command journal decoder, converter-only import APIs, `mako_convert`, and
the unused `durable-storage` compatibility feature. The net change removes
2,019 nonblank Rust lines, including comments and tests. Current native RocksDB
adoption, local/Raft transaction logging, and optional Masstree remain supported.

Prepare/replay/tombstone assertions moved from the old mirror tests to
`mako-runtime/src/application/tests.rs`, which exercises the current protocol
index and persistent application contract. `tests/reopen.rs` replaces converter
process scenarios with reopening and engine switching. Adopted-data API tests
now reopen the same files and retain their seed/journal identity checks.
The storage component explicitly rejects an old Mako directory without moving
or overwriting its files.

| Check | Result | Evidence |
|---|---|---|
| `sto-rocksdb`, with test hooks | 26 passed | [storage.log](storage.log) |
| Full runtime and RocksDB-compatible API integration | 128 passed in fresh processes | [runtime.log](runtime.log) |
| Strict Clippy for affected storage/runtime/API packages | Passed | [clippy-final.log](clippy-final.log) |
| All runtime/API test targets with both engines | Compiled | [build.log](build.log) |
| Native and RocksDB-only CMake configurations | Passed; neither exposes `mako_convert` | [build-profiles.log](build-profiles.log) |
| RocksDB-only runtime/API, all targets | Compiled | [build-profiles.log](build-profiles.log) |
| Engine-free tests, Verus proofs and negative controls | Passed in 166.065 seconds on 32 CPUs | [upper.log](upper.log), [report](upper-report.json) |
| Component/dependency boundaries and tooling regressions | Passed | [boundaries.log](boundaries.log) |

The upper source audit now checks protocol metadata validation and native
checkpoint synchronization/publication. No proof or semantic negative control
was removed. The saved source hashes match the qualified source tree. Full stage
logs, including the expected semantic failures, are preserved in
[upper-stage-logs.tar.gz](upper-stage-logs.tar.gz). Earlier Clippy diagnostics are
retained separately; no lint exemption was added to resolve them.

Run inside the build container, with its native link environment for Masstree:

```sh
cargo test --locked --manifest-path crates/Cargo.toml -p sto-rocksdb \
  --features test-hooks -- --test-threads=1
python3 scripts/run_native_rust_tests.py --manifest crates/Cargo.toml \
  --package mako-runtime --package mako-rocksdb \
  --features mako-runtime/replication,mako-runtime/rocksdb-engine,mako-runtime/test-hooks,mako-rocksdb/masstree-engine,mako-rocksdb/native-fixtures
python3 scripts/upper_check.py --cpus 32
python3 scripts/component_inventory.py --check
```

No performance benchmark was run. Historical format-introduction reports remain
historical records; they do not imply current compatibility with old Mako files.
