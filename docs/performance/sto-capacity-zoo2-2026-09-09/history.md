# Capacity validation before the diagnostic fix

This is the earlier report for the registry implementation committed in
`2d3504a277b3ee66c8fea10099accd2290884f31`. References below to the final
implementation, build, and growth run mean that revision, before the C++
diagnostic fix in `a3ad9a110727f5ecc937cbeee23fe40152df719f`. The retained raw
logs and manifests have not changed. See [the current report](README.md) for
the final executable, repeated growth test, CI, and performance comparison.

This report records the registry-capacity fix and the validation completed for
it. The old table-name limits are removed, exhaustion has a checked transaction
outcome, and the benchmark stops cleanly when an operator-selected budget is
full. This is evidence for this change, not a general production-readiness
approval. Its growth run reached 46.29 million order-line records.
The later diagnostic fix required a fresh growth run and performance sweep,
recorded in the current report.

The design contract is in [rust-sto.md](../../architecture/rust-sto.md),
sections 14.2 and 17. The operational settings are also described in
[sto-tpcc-rust-pgo.md](../sto-tpcc-rust-pgo.md).

To check the retained final correctness and growth evidence from a repository
checkout:

```sh
python3 docs/performance/sto-capacity-zoo2-2026-09-09/audit.py
```

The [auditor](audit.py) verifies 19 evidence hashes, seven source hashes from
the implementation commit, test counts, the exact ASan qualifications, and
the 180-second growth result. It does not read the recorded machine-local
paths, rerun the original tests, or give a performance verdict. A copied
evidence set passed; changing its release-test summary caused the audit to
fail without modifying the originals.

## What changed

The lazy registry no longer allocates an outer directory proportional to the
configured ID limit. It uses a small root of exponentially sized sparse
buckets. Bucket pointer arrays and stable 1,024-entry record segments allocate
only when needed. Existing records stay at the same addresses while the
registry grows. First allocation uses a per-table growth mutex; ordinary
record resolution does not acquire it.

One `RegistryBudget` accounts for structural allocations across the database's
tables. Charges are reserved before allocation and released when the last
owner drops. This includes sparse directory arrays, record arenas, ownership
allocations, and record-lock storage. Allocation failure releases unused
charges; already-published storage stays allocated and accounted. Public Rust
callers can select a shared budget through `Table::new_with_budget` and
`Table::new_direct_with_budget`. Existing constructors keep their prior finite
numeric defaults, so callers enabling large quotas should select a budget
explicitly.

The TPC-C wrapper uses sparse growth for every table and removes its name-based
4-million/16-million retained-record and 6-million/20-million consumed-ID
ceilings. Its settings are now uniform:

| Environment variable | Default | Meaning |
| --- | --- | --- |
| `MAKO_STO_TPCC_REGISTRY_MEMORY` | `8G` | Shared structural registry allocation budget. |
| `MAKO_STO_TPCC_MAX_RETAINED_RECORDS` | `u64::MAX` | Independent retained-record quota for each table. |
| `MAKO_STO_TPCC_MAX_CONSUMED_RECORD_IDS` | `u64::MAX` | Independent consumed-ID quota for each table. |
| `MAKO_STO_TPCC_MAX_RETAINED_KEY_BYTES` | `u64::MAX` | Independent retained-key-byte quota for each table. |

Numeric limits remain subject to the registry's `isize::MAX` addressable ID
limit and the structural budget. Registry memory accepts a positive decimal
byte count with an optional uppercase `K`, `M`, or `G` binary multiplier.
Numeric quotas accept positive decimal integers. Empty strings, zero, leading
zeros, signs, whitespace, invalid suffixes, and overflow fail startup with
exit status 2. Unset variables select defaults.

Ordinary exhaustion returns `STO_TPCC_RESOURCE_EXHAUSTED`, status 6. The FFI ends
the active attempt, discards staged writes and logical-row-count deltas,
releases locks, and closes the native transaction scope before returning.
Idempotent abort and a fresh begin remain valid. Another commit without a new
begin does not. Cleanup failure, poison, or uncertain publication takes fatal
or quarantine precedence over the capacity result.

The C++ wrapper throws `storage_resource_exhausted`. Startup, loader, and
worker boundaries stop the workload, join started threads, and complete
thread cleanup. The process emits
`TPCC_RESOURCE_EXHAUSTED phase=startup|load|run`, exits with status 3, and does
not emit a successful `TPCC_BENCH_RESULT`. A full budget does not become an
unbounded transaction retry loop.

Database and table usage calls expose allocated structural bytes, retained
records, retained key bytes, consumed IDs, and the configured limits. These
are independent counters, not interchangeable memory measurements or an atomic
cross-counter snapshot during concurrent mutation. The benchmark reports
`STO_TPCC_CAPACITY` while workers are quiescent after load and after joins,
including failure paths. End-of-run reporting is outside the measured interval.
`STO_TPCC_NATIVE_ALLOCATOR` reports the separate native allocator configuration.

## Correctness validation

On `zoo-002`, the release `rust_sto_integration` target completed and all
30 selected release CTests passed. The selection includes the C11 ABI headers,
C++ wrapper checks, native Masstree tests, Rust and C++ TPC-C lifecycle tests,
configuration rejection, and transaction/lifetime regression tests. The four
new capacity CTests exercise startup, loading, one-worker runtime exhaustion,
and four-worker runtime exhaustion. Each checks the controlled exit and phase
marker instead of accepting a successful throughput result.

The Rust workspace/native tests cover sparse directory and segment growth,
stable record addresses, concurrent budget reservation, allocation-failure
accounting, independent quotas, and tiny-budget rollback through scalar,
batch, and fused transaction operations. Existing history checks continue to
test atomicity, abort behavior, and bounded strict serializability. These are
bounded tests, not a proof for every concurrent execution.

The first, pre-batching Miri gate passed 477 tests across 37 result groups on
pinned `nightly-2026-08-12`. The final-source rerun passed 481 tests, documented
under final validation below. The Clippy validation log also completed
successfully. Miri runs the
Rust ownership and ABI-range model without executing the external C++
Masstree library. Its script keeps the documented high-iteration history,
concurrency, and statistical exclusions. Exact intentional-quarantine cases
receive their existing separate leak-qualified reruns; this is not a blanket
leak exemption for the Miri suite.

## Focused ThreadSanitizer validation

The initial nine pure Rust `registry_growth_tests` passed under ThreadSanitizer, with
no findings and no suppressions. They cover concurrent growth within an exact
budget, shared-budget accounting, allocation-failure cleanup, and stable
record/lock lifetimes across directory growth.

The local run used pinned `nightly-2026-08-12`, `-Zbuild-std` to instrument the
full Rust standard library, `-Zsanitizer=thread`, and the external Clang 22.1.8
runtime. It built offline in a fresh `/dev/shm` target. These tests use the
in-memory directory adapter; this is supplemental Rust registry evidence,
not native Masstree, C ABI, or full TSan gate coverage.

[The exact command](registry-tsan/run.sh) and [test log](registry-tsan/run.log)
record the toolchains, source hashes, and nine passing results.
[Postflight evidence](registry-tsan/postflight.log) verifies TSan symbols and
instrumentation flags in the copied Cargo fingerprints for `core`, `std`,
`sto-core`, and the test executable. The [result record](registry-tsan/RESULT.md)
describes the scope; no binary build artifacts are included.

After batched reservations were added, all 13 registry growth tests passed
again under full-std TSan with no findings or suppressions. The separate
[final prepaid-reservation evidence](registry-prepaid/README.md) records that
run without changing the earlier evidence.

## AddressSanitizer validation and qualifications

### Final implementation, fresh v4 gate

The final implementation, which reserves a segment's structural budget once
and splits that reservation among independent allocation owners, passed the
full AddressSanitizer gate from a fresh
`/dev/shm/sto-capacity-asan-20260909-v4` build. The gate exited with status 0
and recorded `gate_status=passed`. This is an uninterrupted full-gate pass,
not a replay of the earlier v3 binaries.

The run used CMake 3.31.6, Clang 22.1.8, and pinned Rust
`nightly-2026-08-12`. First-party Rust and the standard library were rebuilt
for ASan and linked to the external Clang runtime. Native integration and
workspace tests passed, including all 205 `sto-masstree` unit tests. All 18
Rust-labeled CTests passed. The same exact four-test leak policy shown below
matched 4/16/4/16 allocations, totaling 40 allocations and 12,800 bytes. The
remaining 14 Rust-labeled tests, including startup and load exhaustion, passed
with leak detection enabled and no LSan suppression environment.

The gate then rebuilt the RMW profile, verified `READ_MY_WRITES=1` and ASan
instrumentation in both producer and consumer compile commands, checked the
exact CTest inventory, and passed both RMW tests. Existing intentional
quarantine and legacy native process-lifetime qualifications remained in
effect, including `detect_leaks=0` for those two RMW tests. A full gate pass
does not mean that these qualified cases are leak-free. No additional Rust
ASan finding was observed.

The [final v4 manifest](asan-v4/rust-sto-sanitizer-manifest.txt) and all CTest
logs are retained separately in `asan-v4/`. The manifest's SHA-256 is
`97231e076237877c6b870713a025e129bb45d58ccf12054dbce3ee1f84df909f`.
The [v4 run record](asan-v4/RESULT.md) contains the exact launch command and
full driver-log hash. The v3 manifests and their previously recorded hashes
remained unchanged after the v4 run.

### Earlier implementation, v3 diagnostic and follow-ups

The first usable ASan build was the local
`/dev/shm/sto-capacity-asan-20260909-v3` tree. It uses CMake 3.31.6,
Clang 22.1.8, and pinned Rust `nightly-2026-08-12`, with first-party Rust and
the standard library rebuilt for ASan and linked to the external Clang
runtime. Earlier setup attempts did not produce usable gate evidence because
of package-discovery and CMake module-build problems.

The original v3 gate remains a diagnostic run. Its workspace and native integration
tests passed, but the gate exited with status 8 when the two new runtime
exhaustion CTests exposed the already-known native Masstree constructor-root
retention. Its manifest was not rewritten to claim success.

After the policy was updated, a separate replay passed all 18 Rust-labeled
CTests using the same binaries. Four exact tests use the unchanged single
suppression rule `leak:mt_tree::mt_tree`:

| Exact CTest | Allocations | Bytes |
| --- | ---: | ---: |
| `test_sto_tpcc_rust_slow_exit` | 4 | 1,280 |
| `test_sto_tpcc_rust_concurrent` | 16 | 5,120 |
| `test_sto_tpcc_rust_run_resource_exhausted` | 4 | 1,280 |
| `test_sto_tpcc_rust_concurrent_resource_exhausted` | 16 | 5,120 |
| Total | 40 | 12,800 |

The updated gate's exact Python verifier accepted all four suppression tables.
A negative check with an incorrect allocation count failed before writing
evidence. The remaining 14 Rust-labeled CTests ran with leak detection enabled
and no LSan suppression environment. Startup and load exhaustion passed in
that unsuppressed group. The new runtime cases introduced no additional
suppression rule and no general leak-disable setting.

The original gate stopped before its RMW tail. A separately logged follow-up
reconfigured and rebuilt with `STO_RMW=ON`, verified `READ_MY_WRITES=1` and
ASan instrumentation in both producer and consumer compile commands, checked
the exact CTest inventory, and passed both insert/delete and
resurrection/delete RMW tests. These native C++ process-lifetime tests retain
the gate's existing `detect_leaks=0` qualification. Existing exact quarantine
tests and legacy native lifecycle tests also retain their documented leak
treatment. These qualifications prevent describing the whole run as
unqualified leak-clean evidence.

No additional Rust ASan finding was observed. The supplemental policy and RMW
manifests passed independently; they are not a fresh uninterrupted full-gate
run. Only sanitizer-policy and documentation changes separated the original
ASan build from the policy replay. No native or Rust implementation change
was made between them.

## Earlier source and build provenance

These are dirty-worktree validation results from branch `codex/sto-rust`,
based on commit `81aa134884219ad148d960c78decca12630648e3`. That commit alone
does not reproduce the tested changes. This section records the earlier,
pre-batching implementation. The final implementation and evidence are
identified separately below.

Compact raw evidence is retained beside this README:

- [Release CTest log](ctest-release.log).
- [PGO provenance](pgo-provenance.txt) and
  [artifact hashes](pgo-artifacts-sha256.txt).
- [Original diagnostic ASan manifest](rust-sto-sanitizer-manifest.txt),
  [supplemental policy manifest](rust-sto-sanitizer-supplemental-policy-manifest.txt),
  and [supplemental RMW manifest](rust-sto-sanitizer-supplemental-rmw-manifest.txt).
- The four `supplemental-test_sto_tpcc_rust_*.log` files contain the exact
  suppression tables. [The remaining Rust-label log](supplemental-remaining-rust-label.log)
  covers the unsuppressed cases; the two `supplemental-ctest-sto-rmw-*.log`
  files record the RMW passes.
- [Miri log](sto-capacity-miri-validation-full.log) and
  [Clippy log](sto-capacity-miri-validation-clippy.log).

These are byte-for-byte copies. Their original absolute paths remain in the
manifests. Large compilation logs and complete build trees are not copied.

The remote artifact root is
`zoo-002:/var/tmp/sto-capacity-20260909.kKklOX`. Its `build` directory contains
the release build. `ctest-release.log` records the exact 30-test selection;
`build-integration-final.log` records the completed integration target. Their
SHA-256 values are:

```text
ctest-release.log
8718863f77a204b00e91c0dd0c9f3bd0c749d5df1b09d1a0a3a57567c508105a
build-integration-final.log
13533b7384b008db88e477c47986b772373c983ea14f448823a8596280c28056
```

The completed `pgo-v2` artifact records Rust 1.95.0, Rust LLVM 22.1.2,
Clang 22.1.8, CMake 3.31.6, the native link inputs, toolchain details, and the
AMD EPYC 7702P host. It includes `source.patch`, `source-status.txt`, and
`source-untracked` alongside `provenance.txt`; do not substitute the base
commit for this captured source state. The artifact finished at
`2026-09-09T06:37:08Z`.

```text
pgo-v2 source_state_sha256
c2967721d3bd0210a298fa3dfe56258f5d9c82721431c6db9c42b768349e4e1e
pgo-v2 native_graph_sha256
a84dcc126416dbfeb6fb281389327a6a19a64f5dc993359929ff656b6eb1e7a5
pgo-v2/provenance.txt SHA-256
c2b9822895929b9f9c3e3552c8011f56f0873e79ef8cfdfe8c37314d2e1d7156
pgo-v2/sto_tpcc_bench SHA-256
3538efdbafb02894906bf80cb03db2744d10150ff1404c81753526d42b4d2369
```

Local ASan evidence lives under
`/dev/shm/sto-capacity-asan-20260909-v3`. The manifests include the log paths,
their hashes, compiler settings, and the tested binary/library hashes. The
original manifest remained byte-identical after both follow-ups:

```text
rust-sto-sanitizer-manifest.txt
3abd97cd6fbea5b8704215dc54d983ff5cc4776cb02d650c9be13990b21800b6
rust-sto-sanitizer-supplemental-policy-manifest.txt
3c01d55a5323ab809f664d683647b6f60aabe8f83d10a9c697660671e4c18661
rust-sto-sanitizer-supplemental-rmw-manifest.txt
64363bce857ec38ad0842f2fd279f8bd3a5907cf22d2a330af85fc4d6b23a984
updated scripts/ci/run_rust_sto_sanitizer.sh
66bcbc6222e0360be82125adb422fc64b4aa23020c2348147b8b74faeb7448c3
```

The Miri log is `/dev/shm/sto-capacity-miri-validation-full.log`, SHA-256
`1f4ab01f5287ed960275353a8568d1ab4f93c73b29114bcea38c2676e2a7afed`.
The Clippy log is `/dev/shm/sto-capacity-miri-validation-clippy.log`, SHA-256
`186a238558ed36518b0b639edc2b8e5ea5180d3fe7cc793d590bc1a82b654b8a`.
The compact copies above accompany this report. Other machine-local
artifact paths are evidence locations, not a guarantee of permanent retention.

## Remaining limits

The structural budget is not a Rust heap cap or an RSS limit. It excludes
separately allocated variable-size payloads, fixed table and budget controls,
dense caches, worker/transaction scratch, allocator overhead, and native
Masstree memory. Inline fields are part of the charged record stride. Operators
must measure the excluded allocations and configure native memory separately.
Graceful handling of configured budget or quota exhaustion does not guarantee
recovery from process or OS out-of-memory conditions. Infallible allocations
can still abort, and the OS can kill the process. This change does not promise
recovery from those failures.

Published IDs and tombstones are not reclaimed or reused. An aborted attempt
can leave interned tombstones, consumed IDs, and structural allocations behind.
Removing the old arbitrary table ceilings does not provide unlimited growth
or make allocation rollback part of transaction rollback. A configured budget
can still fill; it now yields a controlled resource outcome and diagnostics.

The closed TPC-C ABI changed. The trailing
`sto_tpcc_db_config.max_registry_bytes` field, status 6, and usage structures
require the Rust library, C header, and C++ wrapper to be rebuilt together.
The C field's zero value selects the 8 GiB default for zero-initialized callers;
an explicitly configured environment value of zero remains invalid. This is
not a promise of binary compatibility with a previously built caller.

## Performance and growth validation

### Final build and validation

The implementation is committed as
[`2d3504a277b3ee66c8fea10099accd2290884f31`](https://github.com/makodb/mako/commit/2d3504a277b3ee66c8fea10099accd2290884f31)
on [PR #91](https://github.com/makodb/mako/pull/91). The local and remote
validation ran before that commit, on the same implementation files. Their
original dirty-worktree manifests remain unchanged. Committing the files
does not turn those runs into fresh clean-checkout evidence.

The final implementation admits each lazy segment with one budget reservation
instead of 66. It splits that reservation into independently owned charges,
preserving the exact release behavior of detached arenas and lock targets.
See [the prepaid-reservation validation](registry-prepaid/README.md) for the
change and focused tests. Record and lock-target layouts did not change.

The rebuilt PGO executable is
`zoo-002:/var/tmp/sto-capacity-20260909.kKklOX/pgo-batched/sto_tpcc_bench`,
SHA-256 `03acf4188330e1397d381abb398dec5c9b388a077f4fb82973c9427ddfeba20c`.
Its [provenance](final-build/pgo-provenance.txt) and
[artifact hashes](final-build/pgo-artifacts-sha256.txt) record the frozen
source state and the same toolchain, flags, CPU 10, default workload mix,
60-second training interval, 2G native allocator, and 8G registry budget used
for the earlier build. It completed at `2026-09-09T07:28:19Z`.

The final remote native integration target passed again. Its log remains at
`zoo-002:/var/tmp/sto-capacity-20260909.kKklOX/build-prepaid-integration.log`,
SHA-256 `5b7ffcb57c2c2d000414dc6d585307945def48ca7d6fb8b288c09abb1c776b9b`.
All 30 selected CTests passed again in 40.66 seconds; the
[final CTest log](final-build/ctest-release.log) has SHA-256
`5c4ed72ddf4701e6d92f9d2dde3d58e98ca34af1a04bb3904a6d32a28cb988f6`.

The complete final-source Miri gate passed 481 tests across 37 groups with
zero failures. Its final `sto-masstree` group passed 199 tests and retained
the script's six existing exclusions. The three exposed-provenance warnings
come from the existing direct-record tests. See the
[full log](final-build/miri-full.log),
[frozen-source hashes](final-build/miri-source.sha256), and
[evidence hashes](final-build/miri-artifacts.sha256).

### Final growth beyond the old ceilings

The [180-second growth log](final-build/growth-180s.log) records a successful
run of the final PGO executable on `zoo-002`, pinned to CPU 10. It used one
worker and warehouse, the default `45,43,4,4,4` mix, 4G native allocator,
8G registry budget, default numeric quotas, and `--slow-exit`.

| Observation | Value |
| --- | ---: |
| Committed transactions | 10,221,020 |
| Aborted transactions | 0 |
| Measured transaction interval | 181.196146 seconds |
| `order_line_0` retained records and consumed IDs | 46,293,881 |
| `order_line_0` retained key bytes | 740,702,096 bytes |
| `order_line_0` structural registry bytes | 3,246,785,344 bytes |
| `new_order_0` retained records | 4,608,506 |
| `oorder_0` and `oorder_c_id_idx_0` retained records, each | 4,629,506 |
| Database structural registry bytes | 4,572,024,352 bytes |
| Remaining shared registry budget | 4,017,910,240 bytes |
| Peak process RSS reported by GNU time | 6,455,296 KiB |
| Process exit status | 0 |

This crosses the former order-line limits of 16 million retained records,
20 million consumed IDs, and 512 MiB of retained keys. It also crosses the
former 4-million retained-record limit of all three smaller growing tables.
It does not demonstrate crossing every table's former consumed-ID limit or
unbounded operation. The emitted rate, 56,408.594916 transactions/second,
is capacity evidence only. This long run did not use the throughput
comparison's interference checks and must not be compared with shorter runs.

The raw log's SHA-256 is
`cd3c99c415052cea7666e236f48e34e6f08cf4713305ef08c475c625631effd4`.

### Earlier growth check, before batched reservations

The [120-second growth log](growth-120s.log) records a successful run of the
earlier `pgo-v2` executable on `zoo-002`, pinned to CPU 10. It used one
worker, one warehouse, the default `45,43,4,4,4` transaction mix,
`MAKO_TPCC_ALLOCATOR_MEMORY=4G`,
`MAKO_STO_TPCC_REGISTRY_MEMORY=8G`, default numeric quotas, and `--slow-exit`.
The process exited with status 0 after joining its worker.

| Observation | Value |
| --- | ---: |
| Committed transactions | 6,878,948 |
| Aborted transactions | 0 |
| Measured transaction interval | 120.795149 seconds |
| `order_line_0` retained records | 31,247,378 |
| `order_line_0` consumed IDs | 31,247,378 |
| `order_line_0` retained key bytes | 499,958,048 bytes |
| `order_line_0` structural registry bytes | 2,190,933,288 bytes |
| Database structural registry bytes | 3,098,396,896 bytes |
| Remaining shared registry budget | 5,491,537,696 bytes |
| Peak process RSS reported by GNU time | 4,417,536 KiB |

The order-line table passed both the removed 16-million retained-record and
20-million consumed-ID limits without capacity failure. This demonstrates
growth beyond those limits for this workload, not unbounded operation.

The emitted rate was 56,947.220621 transactions/second. This was a capacity
validation run, not a guarded performance comparison. Do not use that rate
to infer a C++/Rust performance gap or a regression. The raw log's SHA-256 is
`22e182731d7d90cffeef85d69f74cdd0a875ec4d4053498a7169a05b4b556fda`.

### Controlled throughput comparison

The comparison uses the repository's in-memory STO TPC-C workload, not an
audited TPC-C result. Each process loads a fresh database, runs the default
`45,43,4,4,4` mix for 10 seconds, and uses equal worker and warehouse counts.
The native allocator is 4G and the Rust structural registry budget is 8G.
Numeric quota overrides and diagnostic fallback switches are unset.

Both engines run on the same physical-core mask, beginning with CPU 10.
The comparison checks that each selected core and its SMT sibling are at
least 95% idle over a two-second window before launch. Each process aligns
after an LXD restart, waits three seconds before that quiet check, and must
have no competing benchmark or LXD journal activity during the explicit
measurement window. Rejected windows and attempts remain in the evidence.
Activity during database loading or post-result destruction is recorded but
does not invalidate an otherwise clean measurement window.

A read-only timing probe found that LXD startup work often overlapped the
first quiet check. The final controller retries the same two-second check
within that restart cycle, with a 20-second launch deadline. Its initial
three-second wait, idle threshold, restart visibility checks, and
measurement-window rejection rules remain unchanged. This waiting policy
applies to every process in the final 4G comparison.

The previous Rust control is the pre-capacity-fix PGO executable with SHA-256
`b2d484ffb6c80fe31cd03e09119e3885300516c1587218984ec5b9aedba8f716`.
Its captured source tree matches the tree of `81aa134884219ad148d960c78decca12630648e3`.
Toolchains, build flags, training workload, configuration, and native link
inputs were checked for compatibility. The final candidate uses the PGO
executable identified under final validation above.

The final-source sweep in [the current report](README.md) supersedes this
comparison. No pre-diagnostic sample contributes to the final sweep. PGO
training and the growth run alone are not throughput-comparison evidence.

### Earlier 2G comparison, stopped by native allocator exhaustion

The first comparison used 2G native memory for both engines. It completed one
matched 16-worker block and two matched one-worker blocks, totaling 12 accepted
samples. During the next 16-worker baseline Rust process, the native allocator
reported `SiloRuntime[0]::AllocateUnmanagedWithLock: OOM` and the process
terminated with `SIGABRT`, return code -6. It emitted the measurement-start
marker at `2026-09-09T08:03:21.023770Z`, but no end marker or successful result.
The controller stopped. This failed run is not a throughput sample.

The exhausted allocator belongs to the previous build's native runtime, not
the new Rust registry budget. At 16 workers, 2G provides 128 MiB per worker
before huge-page rounding. This configuration did not have enough native
headroom for every run. The earlier successful samples do not establish that
the 2G setup is reliable, and the failure was not retried to obtain a better
result. The final comparison starts again with 4G for every process and keeps
all 2G results separate.

The failed run printed 16 separate native regions of `0x08000000` bytes,
128 MiB each. The allocator aborts when one region fills and cannot borrow
unused space from another region. Its message does not identify the exhausted
region or total native occupancy. This was a configured native-region limit,
not evidence of OS out-of-memory or exhaustion of the new shared registry
budget. At 4G, each of the 16 native regions receives 256 MiB.
See [native allocator initialization](../../../src/mako/mako.hh) and the
[`AllocateUnmanagedWithLock` exhaustion path](../../../src/mako/silo_runtime.cc).

During the 2G attempt, the waiting policy changed after ten accepted samples.
It originally skipped an entire restart cycle after a failed quiet check;
the revised policy retries that same check within the bounded interval.
Both revisions, the LXD timing probe, and rejected windows are retained as
diagnostic evidence. Neither revision relaxes the idle or measurement checks.
