# OCC + 2PC qualification

The implementation is on `occ-2pc`, starting from `7cdaecafe`. Foundation
commit `7c3275139` adds the verified states and journals. The following cutover
commit changes the runtime, client and gateway and updates the book.
`upper-final.json` and `native-source-inputs.json` record the checked source
bytes, including files that were uncommitted when qualification ran.

## Results

| Check | Result |
| --- | --- |
| Final upper gate, 32 logical CPUs | 158.940 seconds, below the 300-second limit |
| Production Verus cores | 1495 sharding, 40 storage-host and 46 protocol entries verified |
| Semantic negative controls | All 20 rejected for proof failures |
| Backend-independent executable cases | 57 passed, none ignored |
| Native debug runtime | 52 passed before the final panic guard |
| Final native release runtime | 53 passed in fresh processes |
| Focused ASan and TSan | Eight cases passed under each sanitizer, including the final guard |
| Clippy and API doctests | Native runtime/adapter and ordinary protocol checks passed |
| Native source/dependency audits | 16 storage source files, 12 production executables and 40 Rust archives |

`upper-cutover.json` preserves the earlier 152.304-second run. The final gate
reruns every positive proof and negative control; it does not reuse a previous
proof result. C/C++ compilers are disabled, and Cargo dependency and artifact
checks reject native backends from this gate. This is a single measured final
run, not a repeated timing qualification. Performance remains weekly/manual.

The native cases cover real SRPC, cross-shard point/absence/scan conflicts,
prepared ownership across listener teardown, exact identities, migration drain,
local durable restart, process SIGKILL, replicated recovery, checkpoint and leader
changes, gateway receipts, Redis atomicity and the TPC-C/application regressions.
The test named `benchmark_process` checks bounded workload correctness; this
qualification does not claim a throughput result.

The final panic case injects a journal panic after the authoritative Commit has
been saved. It checks that another worker cannot begin, the decision cannot turn
into Abort, the intent remains unresolved and migration cannot drain. The
instrumented debug runs include this case. The earlier ordinary debug inventory
is retained with its narrower scope stated in `native-debug.json`.

## Commands and environment

All compilation, execution and verification ran inside the `mako-build:merge`
container, using `/var/tmp/mako-shard-rust` as the mounted checkout. The ordinary
build uses the pinned Rust 1.97.1 and the existing `build_retired` native archive.
The standard equivalent commands inside that configured container are:

```sh
bash ci/upper.sh --cpus 32
cmake --build build_retired --parallel 32 --target rust_native_quality
cmake --build build_retired --parallel 32 --target rust_runtime_release_tests
cmake --build build_retired --target native_storage_source_audit rust_native_dependency_audit
CC=/bin/false CXX=/bin/false CARGO_TARGET_DIR=build_docker/upper/target \
  cargo clippy --locked --manifest-path crates/Cargo.toml \
  -p mako-protocol --all-targets --no-deps -- -D warnings
```

The native CMake target uses `scripts/run_native_rust_tests.py`. Each case gets
its own process because existing migration tests deliberately poison shared
process state. Combining those tests in a single ordinary Cargo test process
is not the supported runner.

`address.json` and `thread.json` retain the eight-case inventories, native archive
hashes and instrumentation manifests. Changed Rust code was rebuilt against the
previously qualified `build_native_san_address_05` and `build_native_san_thread_05`
archives, the private instrumented standard libraries and nightly 2026-08-12.
Run `python3 docs/testing/occ-2pc-2026-11-25/sanitizer.py address` or `thread`
inside that instrumented environment to repeat the focused cases. This is not
a new full component or UBSan campaign.

The first TSan attempts failed before executing tests because container seccomp
blocked the sanitizer's ASLR personality change. The passing TSan container used
`--security-opt seccomp=unconfined`; no host ASLR setting changed. The helper may
retry only that exact pre-main failure and records every retry. Instrumentation,
leak checking and the existing reviewed native TSan suppressions remained as
configured in the manifests. No new suppression was added for this change.

## Evidence and scope

The JSON records retain test names, timings, source hashes and local diagnostic
paths. `quality.json` records Clippy, doctest and source-audit log hashes.
`native-dependencies.json` records the audited executable and native dependency
inventories. Full logs remain under the recorded build directories and `/tmp`.

The [book](../../mako-book.md), [implementation plan](../../plans/occ-2pc.md) and
[contracts](../../developer/upper-layer-contracts.md) define the guarantees and
remaining trust. Verus checks executable state transitions under those contracts.
Finite histories and native regressions exercise their composition; they do not
constitute a whole-stack serializability or crash-recovery proof.

Intentional boundaries remain: trusted peers, ordinary 2PC blocking during an
unavailable coordinator, volatile mode without restart guarantees, retained
terminal records without reclamation, no mixed-version rolling upgrade, and no
live migration with persistent or replicated shards. These limits are stated in
the book and were not silently relaxed by this cutover.
