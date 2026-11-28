# Native RocksDB layout qualification

Historical qualification of the native-layout introduction. The subsequent
[current-format cleanup](../../plans/drop-old-mako-formats.md) removes the
old Mako format readers and converter described below.


Qualified on 2026-11-27 in `/var/tmp/mako-shard-rust`, using the configured
`mako-build:merge` container, pinned Rust toolchain and maintained `rocksdb`
0.25.0 crate. No performance benchmark was run.

The saved upper report's source hashes match the final source tree. Its large
`1-working-diff.log` artifact is stored losslessly as `1-working-diff.log.gz`;
the other stage logs retain their original names and bytes.

The [plan](../../plans/native-rocksdb-layout.md) is complete. Both engines keep
raw user keys and values in native column families and store Mako metadata in
`__mako_internal_v2`. Native batches commit rows and metadata atomically. A stopped
ordinary RocksDB directory can be adopted without re-encoding its user records.
The initial scan still takes time proportional to the data size.

| Check | Result | Evidence |
|---|---|---|
| RocksDB storage contracts and transactional storage | 47 passed | [storage-final.log](storage-final.log) |
| Raft host and LocalLog | 27 passed in fresh processes | [raft-final.log](raft-final.log) |
| Broad runtime/API integration, before final seed/retry fixes | 130 passed in fresh processes | [runtime-full.log](runtime-full.log) |
| Final affected runtime/API recovery suites | 73 passed in fresh processes | [recovery-qualified.log](recovery-qualified.log) |
| All runtime/API test targets with both engines | Compiled | [build-final.log](build-final.log) |
| Strict storage Clippy | Passed | [storage-clippy-final.log](storage-clippy-final.log) |
| Strict runtime, API, storage adapter and log Clippy | Passed | [runtime-clippy-final.log](runtime-clippy-final.log) |
| Engine-free executable tests, Verus proofs and semantic negative controls | Passed in 160.741 seconds on 32 CPUs | [upper-final.log](upper-final.log), [report](upper-final/report.json) |
| Component ledger and dependency boundaries | Passed | [accounting.log](accounting.log) |

The final focused run selects both libraries and the `native`, `conversion`,
`log_contract`, `materialization`, `engine_checkpoint` and `rocks_local` targets.
It covers the final adoption and journal-identity changes, including upstream
fixtures/readers, direct/Masstree toggling, adopted rows through migration and
Raft failover, missing-log rejection, and conversion of adopted data under both
log modes. The converter preserves original seed rows and later committed rows.
The old prefixed-key format also has an end-to-end conversion regression with
pending prepares, decisions, receipts and tombstones.

Storage cases include binary and empty keys, multiple families, native
checkpoint publication, generation leases, schema/size/comparator rejection and
restart at four adoption cutpoints. Failed preflight remains retryable with the
correct schema. Both log modes reject a different initial-data identity.

Native integration and proof tests remain separate. `native-fixtures` opts the
facade's integration target into upstream RocksDB. CMake's native/runtime test
targets and the sanitizer runner enable it. The upper gate leaves it disabled,
audits normal/build/development dependencies, and sets both C and C++ compilers
to `/bin/false`.

## Reproduction

Run these inside the build container. Native tests additionally require the
normal Masstree archive/link environment described in the development guide.

```sh
cargo test --locked --manifest-path crates/Cargo.toml \
  -p mako-rocksdb-store -p sto-rocksdb \
  --features mako-rocksdb-store/test-hooks,sto-rocksdb/test-hooks -- --test-threads=1
python3 scripts/run_native_rust_tests.py --manifest crates/Cargo.toml --package raft-host
python3 scripts/run_native_rust_tests.py --manifest crates/Cargo.toml \
  --package mako-runtime --package mako-rocksdb \
  --features mako-runtime/replication,mako-runtime/rocksdb-engine,mako-runtime/test-hooks,mako-rocksdb/masstree-engine,mako-rocksdb/native-fixtures \
  --lib --test native --test conversion --test log_contract \
  --test materialization --test engine_checkpoint --test rocks_local
python3 scripts/upper_check.py --cpus 32
python3 scripts/component_inventory.py --check
```

Omit the target selectors from the runtime command to run the broad integration
suite. Performance qualification remains weekly/manual.

## Limits and diagnostics

Adoption requires a stopped writer, bytewise ordering and supported key/value
limits. The public path becomes a relative symlink into its sibling `.mako`
control directory. Backups must preserve the selected generation, control state
and journal together. Once adopted, writes must go through Mako. With Masstree,
native readers see the materialized prefix until flush/checkpoint or recovery.

Every Raft replica must start from the same plain database image or a checkpoint
of that image. Initial-state hashes bind peer messages and journal authority;
this implementation does not distribute a leader-only seed to empty replicas.
The [book](../../mako-book.md#native-rocksdb-directories-and-adoption) documents
the directory layout, compatibility limits and offline upgrade procedure.

Earlier checkpoint RPC timeouts remain in `runtime.log` and the diagnostic
logs. They did not recur in eleven isolated repetitions, the full 130-case run
or the final 73-case run. Their cause was not established; no timeout or oracle
was relaxed. Qualification also exposed and fixed generation lease accounting
and an initial-state import mutex recursion.

`recovery-final.log` retains a failed new test fixture that tried to reopen an
existing journal with `create: true`. The corrected fixture explicitly reopens
it with `create: false`. `runtime-clippy.log` records a shared test module's
dead-code warning; making that test module public fixed the warning without
changing its cases or adding a lint exemption. Later passing logs are named
explicitly in the table above.
