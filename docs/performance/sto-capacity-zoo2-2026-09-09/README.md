# Rust STO capacity fix, 2026-09-09

The Rust adapter no longer imposes table-name record ceilings. Its registry
grows within a configurable shared structural-memory budget. Ordinary budget
or quota exhaustion aborts the transaction attempt and stops the benchmark
with a checked failure, without reporting a successful throughput result.

The final executable reached 46,141,818 order-line records with zero transaction
aborts. All six hosted CI jobs passed on the final implementation commit.
The 30-sample performance sweep completed with median paired Rust slowdowns
of 0.54% at one worker and 0.99% at 16 workers versus the previous build.
This validates this change, not unlimited growth or general production
readiness. Three matched blocks at each boundary count do not prove a tight
performance bound.

The implementation is on [PR #91](https://github.com/makodb/mako/pull/91):

- [2d3504a](https://github.com/makodb/mako/commit/2d3504a277b3ee66c8fea10099accd2290884f31)
  implements sparse growth, shared budget accounting, checked exhaustion,
  diagnostics, and regression tests.
- [a3ad9a1](https://github.com/makodb/mako/commit/a3ad9a110727f5ecc937cbeee23fe40152df719f)
  makes the failure diagnostic one bounded write so concurrent shutdown
  logging cannot split its marker. It adds a deterministic regression test.

The [design](../../architecture/rust-sto.md) defines the registry contract in
section 14.2 and the integration contract in section 17.1.
The [operations guide](../sto-tpcc-rust-pgo.md) documents the settings.

## Limits and failure behavior

The sparse registry allocates stable 1,024-entry segments as needed. A small
root points to doubling-size directory buckets, so a large configured ID
limit does not require an equally large allocation at startup. Existing record
resolution does not acquire the growth mutex.

A shared `RegistryBudget` reserves structural bytes before allocation. Each
segment uses one reservation, split among independent allocation owners.
Charges remain live as long as their storage does. Failed allocations return
unused reservations; already-published records retain their addresses.

| Wrapper setting | Default | Scope |
| --- | --- | --- |
| `MAKO_STO_TPCC_REGISTRY_MEMORY` | `8G` | Shared structural registry bytes across tables. |
| `MAKO_STO_TPCC_MAX_RETAINED_RECORDS` | `u64::MAX` | Independent per-table retained-record quota. |
| `MAKO_STO_TPCC_MAX_CONSUMED_RECORD_IDS` | `u64::MAX` | Independent per-table consumed-ID quota. |
| `MAKO_STO_TPCC_MAX_RETAINED_KEY_BYTES` | `u64::MAX` | Independent per-table retained-key-byte quota. |

The registry also enforces its `isize::MAX` addressable ID limit. Environment
settings reject zero, empty strings, leading zeros, signs, whitespace, and
overflow. Invalid configuration exits with status 2. Registry memory accepts
uppercase binary `K`, `M`, and `G` suffixes; numeric quotas do not.

Ordinary capacity failure returns `STO_TPCC_RESOURCE_EXHAUSTED`, status 6,
after aborting the active attempt, releasing its locks, and closing the native
transaction scope. Staged writes and logical-row-count changes do not commit.
Cleanup failure, poison, or uncertain publication takes fatal or quarantine
precedence. Abort remains idempotent; a new attempt requires a fresh begin.

The wrapper handles startup, load, and worker failures. It joins started
threads, emits `TPCC_RESOURCE_EXHAUSTED phase=startup|load|run`, exits with
status 3, and emits no successful `TPCC_BENCH_RESULT`. The failure record uses
one write of at most 512 bytes. Control characters are replaced with spaces
and oversized details are truncated. Usage counters are reported while
workers are quiescent, outside the measured interval.

## Exact final executable

The final native rebuild and relink used source `a3ad9a1`. All 123 files in the
Rust crate subtree matched the captured PGO build byte-for-byte, so the relink
reused its unchanged Rust archive and profile. All 122 native compile command lines
matched the previous build. Source and link-input checks passed before and
after the relink. See [the retained provenance](final-relink/provenance.json)
and [compile-command comparison](final-relink/final-native-compile-command-comparison.json).

| Artifact | SHA-256 |
| --- | --- |
| Final executable | `3e32ba7d47a6de1b080d14cdc4955d04ecee0ece32a969a2dcfdf3925a2e31fe` |
| Reused Rust PGO archive | `7d29c5077d9a62b9bdce4c4b1a7f1c7eecc206fc7cc92138e8b921321a579b87` |
| Reused Rust profile | `c14043bbd29a8f74459bb2ed1bfcb98917313ee508fd4c2a7f2a9412dadea62b` |

The executable is retained at
`zoo-002:/var/tmp/sto-capacity-20260909.kKklOX/pgo-final-atomic-output/sto_tpcc_bench`.
The build used Rust 1.95.0, Rust LLVM 22.1.2, Clang 22.1.8, and CMake 3.31.6.
Only Rust was PGO-trained. Training used one worker and warehouse on CPU 10,
the default workload mix, 60 seconds, 2G native memory, and an 8G registry
budget. Validation and measurement memory settings are recorded separately.

## Growth beyond the removed ceilings

The exact final executable ran for 180 seconds on CPU 10 with one worker and
warehouse, the `45,43,4,4,4` transaction mix, 4G native memory, an 8G registry
budget, default numeric quotas, and `--slow-exit`.

| Observation | Value |
| --- | ---: |
| Committed transactions | 10,187,161 |
| Aborted transactions | 0 |
| Measured transaction interval | 181.202301 seconds |
| `order_line_0` retained records and consumed IDs | 46,141,818 |
| `order_line_0` retained key bytes | 738,269,088 bytes |
| `order_line_0` structural registry bytes | 3,236,168,416 bytes |
| `new_order_0` retained records | 4,593,284 |
| `oorder_0` and `oorder_c_id_idx_0` retained records, each | 4,614,284 |
| Database structural registry bytes | 4,557,103,264 bytes |
| Remaining shared registry budget | 4,032,831,328 bytes |
| Peak process RSS reported by GNU time | 6,433,792 KiB |
| Process exit status | 0 |

This crosses the old order-line limits of 16 million retained records,
20 million consumed IDs, and 512 MiB of retained keys. It also crosses the
old 4-million retained-record limits of the three smaller growing tables
above. It does not cross every table's former consumed-ID or history limit.

The [raw log](final-relink/growth-180s.log) has SHA-256
`86033a5779141163f458378ec0f204ca7729a16d277042e462e6d2a40baaca5d`.
Its throughput is capacity evidence only. This long run did not use the
performance sweep's interference checks.

## Correctness and sanitizer evidence

The exact final native integration build passed. All
[31 boundary CTests](final-relink/ctest-boundaries.log) passed, followed by
all four capacity-failure smoke tests on the final relinked executable.
These cover startup, loading, one-worker exhaustion, and concurrent exhaustion.

The unchanged Rust implementation passed 205 `sto-masstree` unit tests,
five public-contract tests, and three documentation tests. The complete pinned
Miri gate passed 481 tests across 37 groups. The final 13 registry-growth
tests also passed focused Miri and full-standard-library TSan runs.
See [the registry evidence](registry-prepaid/README.md) and
[complete Miri log](final-build/miri-full.log).

The [local full ASan gate](asan-v4/RESULT.md) passed on the registry
implementation before the diagnostic-only C++ change. It ran 18 Rust-labeled
CTests and both RMW tests. Four exact native lifecycle cases retain the
existing leak qualification, totaling 40 allocations and 12,800 bytes. Other
documented native lifecycle and intentional-quarantine qualifications also
remain. This is not unqualified leak-free evidence.

The [new diagnostic regression](diagnostic-fix/RESULT.md) passed 100 records
under native execution, ASan, UBSan, and TSan. It also passed through the actual
project CMake target in Release and ASan builds. A logger handshake proves the
old fragmented-write pattern fails and checks the new complete records.
No suppressions were used for this standalone test. The final registered
inventory contains 19 Rust-labeled CTests.

These tests cover specific ownership, rollback, accounting, and concurrency
cases. Miri does not execute native Masstree; focused registry TSan is not a
substitute for the complete native TSan gate.

All six hosted CI jobs passed for `a3ad9a1`:
[CI](https://github.com/makodb/mako/actions/runs/34329193601) and
[sanitizers](https://github.com/makodb/mako/actions/runs/34329193602).
These include Release build and tests, Miri ownership checks, the SRPC dual
compile, and ASan, UBSan, and TSan native-boundary gates. The
[per-commit record](hosted-ci/final-a3ad9a1/README.md) retains final job metadata.
No rerun or cancellation was used to obtain these passes. Later evidence-only
commits do not change which revision these recorded CI results cover.
The previous revision's UBSan job failed because concurrent shutdown logging
split the expected failure marker. The [failed run](hosted-ci/old-2d3504a/README.md)
is retained as diagnostic evidence, not relabeled as a pass. That finding
prompted `a3ad9a1`.

## Controlled performance comparison

The final sweep completed all 30 samples without a process failure or rejected
measurement interval. It measured the repository's in-memory STO TPC-C
workload, not an audited TPC-C result. Each process loaded a fresh database,
used the default mix, and measured for 10 seconds. Workers and warehouses
matched. Both engines and both revisions used 4G native memory; Rust used an
8G registry budget. Quota overrides and diagnostic fallbacks were unset.

The 30 samples include three matched old/new blocks at both one and
16 workers, plus one candidate C++/Rust pair at each of two, four, and eight
workers. Each matched block measures both engines on both revisions. The
baseline executable has SHA-256
`b2d484ffb6c80fe31cd03e09119e3885300516c1587218984ec5b9aedba8f716`;
its source tree matches `81aa134884219ad148d960c78decca12630648e3`.

Selected physical CPUs start at CPU 10. Each launch checks that those CPUs
and their SMT siblings are at least 95% idle over two seconds. Runs align
after an LXD restart with a three-second initial wait and bounded repeated
quiet checks. A process must start within the 20-second launch deadline.
Competing benchmarks and LXD activity during the measurement interval
invalidate the sample. Rejected attempts remain in the evidence.

There were 36 prelaunch quiet checks, six of which failed before a process
started. The unchanged waiting policy found an admissible window for each
launch. No measured pair was rejected or retried.

| Workers | Matched blocks | Median paired Rust change | Median paired C++ change | Observed Rust changes |
| --- | ---: | ---: | ---: | --- |
| 1 | 3 | -0.54% | -0.66% | -0.54%, -0.85%, -0.37% |
| 16 | 3 | -0.99% | -1.04% | -3.32%, -0.22%, -0.99% |

Each change compares a new-build throughput with its matched old-build run
for the same engine.
These are medians of paired ratios, not ratios of the median throughputs.
The first 16-worker block was slower than the other two. The C++ controls also
moved, but C++ code changed in this patch, so normalizing by C++ cannot isolate
the cost of Rust registry accounting. Raw paired changes are the primary
comparison; normalized changes are supplemental.

The approximate 95% log-t interval for the geometric-mean Rust change is
-1.18% to +0.02% at one worker and -5.46% to +2.59% at 16 workers. These
three-block estimates depend on distributional assumptions and do not prove
equivalence or a worst-case slowdown bound.

The candidate-only C++/Rust sweep produced these median throughputs:

| Workers and warehouses | Runs per engine | C++ transactions/s | Rust transactions/s |
| --- | ---: | ---: | ---: |
| 1 | 3 | 40,592 | 57,810 |
| 2 | 1 | 81,897 | 111,449 |
| 4 | 1 | 166,515 | 217,955 |
| 8 | 1 | 328,606 | 416,275 |
| 16 | 3 | 653,218 | 838,433 |

Median paired Rust/C++ ratios range from 1.267 at eight workers to 1.422 at
one worker. The two-, four-, and eight-worker cells have only one pair each;
they are sweep observations, not repeated regression controls.
See [the candidate summary](performance-final/new-summary.csv),
[all six old/new controls](performance-final/old-new-paired-controls.csv), and
[paired statistics and qualifications](performance-final/old-new-paired-summary.json).

The [endpoint identity check](performance-final/identity-postflight.json) ran
after all processes exited. Both executables, configuration, live and archived
runner, archived controller, Rust archive, and Rust profile matched their
launch or relink hashes. This checks identities at the endpoints, not continuous
filesystem monitoring. The [measurement evidence](performance-final/evidence-validation.json)
records the accepted samples and guard counts.

The earlier 2G comparison stopped when the baseline Rust executable exhausted
one native per-worker region and aborted. Its accepted samples are not mixed
into the final 4G sweep. At 16 workers, 2G divides into 128 MiB regions; 4G
provides 256 MiB regions. Unused space in another region cannot satisfy an
allocation in a full region. This is separate from the Rust registry budget.
A subsequent four-sample 4G pilot used the pre-diagnostic executable and is
also excluded from the final results. The
[incomplete 2G attempt](performance-diagnostic-2g-incomplete/incomplete.json)
and [4G pilot](performance-diagnostic-4g-pilot/pilot.json) retain their raw
samples and failure or pause records separately. See [the earlier report](history.md).
The [pre-optimization controls](performance-diagnostic-preoptimization/performance-boundaries-v3/README.md)
also remain separate, including their earlier waiting-policy differences.
None of those samples contributes to the final tables above.

## Remaining operational limits

The registry budget is not an RSS or total-heap cap. It excludes separately
allocated variable-size payloads, fixed table and budget controls, dense
caches, worker scratch, allocator overhead, and native Masstree memory.
Inline payload fields are included in the charged record stride. Native memory
must be configured separately and sized for each worker's allocation region.
Configured-budget recovery does not guarantee recovery from allocator or
OS out-of-memory failures.

Published IDs and tombstones are not reclaimed or reused. Aborted attempts
can leave interned tombstones, consumed IDs, and structural allocations behind.
Transaction rollback does not promise allocation rollback.

The TPC-C ABI is closed and changed. Rebuild the Rust library, C header, and
C++ wrapper together. A zero-initialized C `max_registry_bytes` field selects
8 GiB; an explicit zero environment setting is invalid. Existing public Rust
constructors retain their finite numeric defaults. Callers choosing larger
quotas should use the budget-aware constructors explicitly.

## Retained evidence

Raw logs retain their original contents and machine-local paths. Earlier
builds and failed attempts are kept separate from final-source evidence.
[history.md](history.md) records the pre-diagnostic validation chronology.

To check the retained final relink and growth evidence:

```sh
python3 docs/performance/sto-capacity-zoo2-2026-09-09/final-relink/verify.py
```

The [verifier](final-relink/README.md) checks 17 retained artifact hashes,
source hashes pinned to `a3ad9a1`, the 31-test inventory, four exit-3 smokes,
and the repeated growth result. The large executable, archive, and build
artifacts remain on `zoo-002`; their recorded identities are checked for
consistency, but the portable verifier does not rehash those remote bytes.
The [diagnostic-fix bundle](diagnostic-fix/RESULT.md) has a separate checksum
manifest and source-pinned verifier.

To verify the final performance package without executing benchmarks or
reading the recorded remote paths:

```sh
python3 docs/performance/sto-capacity-zoo2-2026-09-09/performance-final/verify-performance.py docs/performance/sto-capacity-zoo2-2026-09-09/performance-final
```

This checks the retained hashes, the fixed 30-sample plan, raw benchmark
results, timing markers, quiet and measurement guards, summary arithmetic,
and endpoint identity evidence. The final package has 88 hashed artifacts.
Its [manifest](performance-final/artifact-hashes.json) has SHA-256
`c12a186d4cb68ce27d633f37730d33f2d15b20a3e564318e93298f903c498bfd`.
Normal and optimized Python passed. Tests on a copy rejected altered raw
throughput both at the checksum check and, after updating only the copied
manifest, at the raw-result consistency check. Originals remained unchanged.
The separate `collect-remote-evidence.py`
is the original remote collector and writes files. Do not use it as the
portable verifier.

The earlier core-evidence checker remains available:

```sh
python3 docs/performance/sto-capacity-zoo2-2026-09-09/audit.py
```

It verifies the pinned `2d3504a` core evidence, including the earlier
46.29-million-record growth run and 30-test inventory. It does not verify the
later executable or substitute for the final-source checks.
