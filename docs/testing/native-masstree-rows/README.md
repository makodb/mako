# Native Masstree row qualification

The production path uses C++ rows and buffers through safe Rust handles.
Distributed STO owns the live transaction frame and read/write set. This record
covers the port described in [the plan](../../plans/native-masstree-rows.md).
Qualification passed on the dirty implementation worktree at
`/var/tmp/mako-shard-rust`. The sanitizer manifests and upper report record the
source hashes; these results are not attributed to the unchanged HEAD commit.

## Test port

The previous component included several committed-value layouts, a synthetic
Rust directory, specialized batches/caches and a Rust C facade. These have been
removed from production. The test count therefore changes; it is not evidence
by itself that the replacement has the same coverage.

| Prior area | Current coverage or explicit retirement |
|---|---|
| Binary CRUD, empty values, duplicate mutations, abort and cross-table atomicity | `sto-masstree-api/tests/native_api.rs` and `native_contract.rs`. Buffer sizes include 0, former inline/cell boundaries and 65,537 bytes. |
| Native history, replay, retained locks and phase ordering | `sto-masstree/tests/native_integration.rs` retains the existing history/replay assertions. `sto-masstree-api/tests/participant.rs` covers repeated locking, frozen final intents, historical insert/delete cardinality, retained prepare checkpoints and installed-abandon fencing. |
| Concurrent transaction histories | `sto-masstree/tests/history_oracle.rs` keeps the strict-serializability checker, schedules, coverage thresholds and 128-seed sweep per mode. The two modes are now ordered and record-local versions, replacing registry-ID and direct-token storage layouts. |
| Mixed version contracts and other transaction objects | `sto-masstree/tests/local_versions_native.rs` retains the ordered/local table and `TxnCell` composition assertions. Generic core/capability/cleanup tests remain independent of Masstree. |
| Scans, binary order, bounds, tombstones and phantom rejection | Existing native scan assertions remain in `native_integration.rs`; the history oracle checks concurrent scans, and the shared storage conformance suite checks the application contract. |
| Ownership, snapshot lifetime and row memory accounting | `masstree/tests/native_rows.rs` checks native pointer sharing, snapshots after replacement/table destruction, history pruning, native locks, fencing, binary key order and budget admission/reclamation. |
| Capacity exhaustion and healthy reuse | `sto-masstree-api/tests/capacity.rs` covers row/key/shared native budgets, scan limits, lock limits, atomic abort and access to already allocated rows. Consumed registry-ID quotas are retired because there is no registry-ID allocator. |
| Indeterminate publication quarantine | `native::tests::post_install_failure_marks_runtime_indeterminate` injects a hook failure after actual native installation. The exact quarantine inventory moves to this test. The removed Rust row-count hook is no longer an injection site. |
| Wrong-thread and escaping Rust/C borrows | Safe owners and compile-fail doctests enforce worker affinity, exclusive transaction access and borrowed-value lifetime. The private FFI module documents pointer provenance and native owner lifetimes. Generic index handle/ABI tests remain intact. |
| Registry segments, eager/lazy arenas, stable160/worker-local cells, direct-token caches, fixed-u64 store | These implementations and their layout/optimization-specific tests are retired. The shared binary CRUD, capacity, ordering, lifetime and atomicity behaviors are tested above. |
| Fixed batch C endpoints, trusted raw pointer shortcuts, resolved caches and callback scratch reuse | The redundant Rust C facade and these entry points are retired. Production used the owned Rust API, which now calls STO directly. Native row FFI remains private. Ordered duplicate mutation behavior is tested through the safe API. |
| Pinned lookahead and synthetic directory fixtures | The unused specialized interface is retired. Production migration uses the generic storage scan contract and external range guards; runtime migration regressions remain required. |
| Old adapter comparison/PGO recipes | Retired with their specialized backends. `mako_bench` still provides the node workloads for separate weekly/manual performance runs. No throughput improvement is claimed here. |

## Qualification commands

All builds and tests run in the build container. Native cases run in separate
processes because some intentionally poison state. `ci/masstree_tx.sh` compiles
and tests the component without RocksDB, SRPC, Raft or application dependencies.
The independent upper gate remains `python3 scripts/upper_check.py --cpus 32`.
Runtime tests separately exercise both storage paths, journal replay, checkpoints,
migration and the RocksDB-compatible facade.

Native sanitizer qualification must keep the existing suppression sources and
exact quarantine inventory. There is no new general leak/race suppression for
the row wrapper. Upper-layer proofs assume the native storage contract; they do
not prove C++ memory safety. Native row tests and sanitizers cover that boundary.

## Initial checks

The first component run passed 53 executable cases but failed its doctest command
because CMake still requested the retired `fixed-u64` feature. The command was
corrected, and the isolated component run then passed. The first sealing
regression exposed an error mapping that treated a sealed-directory refusal as
an adapter fault. It now reports an unsupported operation, aborts that attempt
and permits later access to existing rows. The regression passes.

The failed initial component log is retained as
[`component-initial.json`](component-initial.json). Earlier command output
also caught a missing C++ `<exception>` include, two stale Rust item-view method
calls and Clippy formatting/type issues; these were corrected before qualification.


## Final results

| Check | Result | Evidence |
|---|---|---|
| Independent component build, native tests, doctests, strict wrapper Clippy and dependency audit | 53 executable cases and 8 doctests passed | [Component log](component.log) |
| Runtime, RocksDB-compatible API and workloads, with replication and both storage paths | 117 fresh-process tests passed, including migration, prepared recovery, failover, checkpoint and process restart | [Runtime log](runtime.log) |
| Upper-layer gate on 32 logical CPUs | 218 executable scenarios; 1,497 sharding, 40 host, 42 production STO, 10 historical coordinator and 1 Raft retention obligations; all 29 negative controls rejected; 161.905 seconds | [Log](upper.log), [source hashes and stage report](upper-report.json) |
| Component census and dependency-tool controls | 10 tests passed | [Tooling log](tooling.log) |
| ASan, native rows/index and Rust adapters | 53 cases passed | [Manifest](address/manifest.txt), [report](address/tests.json), [build/run log](address/gate.log) |
| TSan, native rows/index and Rust adapters | 53 cases passed | [Manifest](thread/manifest.txt), [report](thread/tests.json), [build/run log](thread/gate.log) |
| UBSan, native rows/index and Rust adapters | 53 cases passed | [Manifest](undefined/manifest.txt), [report](undefined/tests.json), [build/run log](undefined/gate.log) |

Each sanitizer used a fresh native build and audited instrumentation in all 16
native sources, including the new row object. ASan and TSan also instrumented
Rust with the pinned nightly and the previously built, instrumented standard
library, whose hash and sanitizer symbols were checked. UBSan instruments C++;
it does not claim Rust UBSan instrumentation. The
[exact command](sanitizer-command.txt) records the flags, caches and package scope.
The container permitted the sanitizer's ASLR setup with
`--security-opt seccomp=unconfined`.

The existing Masstree TSan race suppressions and UBSan bounds suppression still
qualify those results. ASan retains only the existing process-lived tree-root
leak suppression, and disables leak reporting for the exact inventoried
post-install quarantine case. No suppression was added for native rows. Each
report includes the unmodified output of its individual cases. These are component sanitizer results;
the 117 runtime tests above ran without sanitizers. The upper proofs assume
native storage behavior and do not establish C++ memory safety.

No throughput benchmark ran. The change removes alternate layouts and duplicate
ownership; its performance should be measured through the separate node workload
suite before claiming a speedup.
