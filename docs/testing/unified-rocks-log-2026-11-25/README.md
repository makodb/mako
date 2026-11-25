# Unified RocksDB log qualification

This change starts at `c3938df78` on `unified-rocks-log`. The runtime uses one
log adapter and application codec for both local and Raft ordering. Both drivers
write the `mako-rocks-log/1` RocksDB format. `source-inputs.json` and `upper.json`
record the tested source bytes; the latter also retains the proof run's original
working-tree inventory. Documentation and the final storage refusal test were
added after that proof run. None of the proof inputs changed afterward.

## Results

| Check | Result |
| --- | --- |
| Isolated upper gate, 32 CPUs | 156.026 seconds, within the 300-second limit |
| Production Verus cores | 1495 sharding, 40 storage-host and 46 protocol entries verified |
| Semantic negative controls | All 20 rejected for proof failures |
| Backend-independent executable cases | 57 passed |
| Shared log checks | Six passed, including actual RocksDB process exit and format/ownership refusals |
| Full raft-store suite | 46 passed; the later refusal case brings the distinct total to 47 |
| Raft host with fault hooks | 10 unit and 14 protocol cases passed |
| Native runtime, debug and release | 54 cases passed in each profile, in fresh processes |
| Focused ASan and TSan | Eight cases passed under each; no startup retries |
| Direct RocksDB service configuration | Six passed |
| Strict Clippy and native API doctests | Passed |
| Native source/dependency audits | 16 storage sources, 12 production executables and 40 Rust archives |
| Default local launcher and reopen | Two shards committed 40 transactions, reopened, then continued from 40 to 80 |

The host's ignored `snapshot_process` test is a subprocess helper launched by
its crash-test parent. It is not an omitted standalone scenario. The store's
legacy segmented-WAL component tests remain separate from the shared journal
checks; production hosts use the new journal.

The format tests compare actual local and Raft driver entries, including a
binary command, and distinguish append from commitment. Fault checks cover
unsaved atomic batches, committed-history replacement, malformed prefixes,
application failure after saved commitment, replay exactly once, an applied
prefix ahead of authority, exclusive directory ownership and refusal to create
over an existing log. The legacy refusal case checks retained bytes directly.
Native cases additionally cover prepared transaction recovery, SIGKILL, Raft
failover, snapshots, gateway receipts and application regressions.

## Commands and environment

All compilation, tests and proofs ran inside `mako-build:merge`, with the checkout
mounted at `/var/tmp/mako-shard-rust`. The ordinary toolchain is Rust 1.97.1.
The configured native environment supplies the existing `build_retired` storage
archive and the native integration/link settings. Equivalent commands are:

```sh
bash ci/upper.sh --cpus 32
cargo test --locked --manifest-path src/deptran/raft/store/Cargo.toml --features rocksdb
cargo test --locked --manifest-path crates/Cargo.toml -p raft-host --features test-hooks -- --test-threads=1
cargo test --locked --manifest-path crates/Cargo.toml -p rocksdb-raft-service --features test-hooks -- --test-threads=1
cmake --build build_retired --parallel 32 --target rust_shard_integration_tests
cmake --build build_retired --parallel 32 --target rust_runtime_release_tests rust_native_quality
cmake --build build_retired --target native_storage_source_audit rust_native_dependency_audit
cargo clippy --locked --manifest-path src/deptran/raft/store/Cargo.toml --features rocksdb --all-targets -- -D warnings
cargo clippy --locked --manifest-path crates/Cargo.toml -p raft-host --features test-hooks --all-targets -- -D warnings
```

`raft-store` has its own manifest outside the `crates` workspace. Runtime tests
use `scripts/run_native_rust_tests.py`, which gives each test a fresh process.
Some migration cases intentionally poison process-wide state.

The upper gate disables C/C++ compilation and checks Cargo dependencies and
artifacts. It verifies the production cores and rejects every selected semantic
negative control on this run. Its results do not depend on storage, SRPC or
Raft tests. The bounded workload cases test correctness; performance remains
weekly/manual.

The sanitizer helper rebuilds changed Rust code against the previously qualified
native archives and instrumented standard libraries in
`build_native_san_address_05` and `build_native_san_thread_05`, with nightly
2026-08-12. Run the saved `sanitizer.py` with argument `address` or `thread` inside that
environment. TSan uses a container with
`--security-opt seccomp=unconfined` so its own ASLR re-execution can run; no host
setting changed. Existing reviewed suppressions and leak checks were retained.
No suppression was added. This is a focused qualification, not a new full native
component or UBSan campaign.

## Evidence and limits

The JSON records retain source and native-archive hashes, test inventories,
instrumentation settings and diagnostic log paths/hashes. `checks.json` covers
the component, quality, audit and launcher runs. Full logs remain at the recorded
local paths. The [plan](../../plans/unified-rocks-log.md),
[book](../../mako-book.md) and [contracts](../../developer/upper-layer-contracts.md)
define the guarantees and assumptions.

New local and Raft stores share `<data>/log/base` and `<data>/rows`. Existing
segmented-WAL or applied-only local stores are refused intact. No automatic
conversion or mode-switch tool is included. Mode, membership/group and durability
policy remain fenced. Local command history has no compaction yet; Raft keeps
its snapshot path. Existing terminal transaction retention and persistent live
migration restrictions remain. Explicit async writes do not promise power-crash
durability, and volatile mode has no restart guarantee.

The proofs check upper state transitions under storage/RPC/Raft contracts.
The crash models and native regressions exercise the new storage implementation;
they are not a proof of RocksDB or whole-stack crash recovery.

## Explicit backend interface follow-up

The follow-up to `63d70287c` exposes `MemoryLog`, `LocalLog` and `ReplicatedLog`
through `CommitLog: Journal`. It removes the store's implicit memory fallback
and preserves the shared local/Raft disk format. MemoryLog is for testing;
native fixtures select it explicitly with `volatile: true`. LocalLog and
ReplicatedLog are typed specializations accepting their own configuration types.

`upper-backends.json` records the refreshed isolated gate: 149.566 seconds on
32 CPUs, 58 executable cases, all three production proof suites and all 20
semantic negative controls. `backend-interface.json` records all 55 native debug
cases and the final native/protocol Clippy and doctest checks. The first quality
run found an unused import in the new test; after its removal, the shared
contract case and quality checks passed. The shared helper checks duplicate
operations, immutable outcomes, changed descriptors and durability mismatch
against all three backends. The memory case also runs in the upper gate without
any native dependency.

The commands are the same native debug, upper and quality commands listed above;
the protocol quality check adds `cargo clippy --locked --manifest-path
crates/Cargo.toml -p mako-protocol --all-targets --no-deps -- -D warnings` with
C/C++ compilers disabled. This interface follow-up does not rerun the unchanged
storage components, release suite or sanitizers; their earlier records retain
the scope and source hashes stated above.
