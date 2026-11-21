# Rust cache over Rust STO: paired performance investigation

The final production-source comparison completed on 2026-09-13. This is not
a performance acceptance claim. The C ABI remains the default, and no
acceptance budget or default-engine cutover has been approved.

## Final production-source results

All 132 fresh-process cells passed: 60 engine/foreground-ACK cells and 72
broader cache cells, each of the latter with full drain and a fresh exact
recovery. The final audit binds raw output, cell records, requested workload,
machine state and executable identity. All 256 source files and 21 native/link
inputs match the completed build snapshot. Every foreground PMU sample had
full running time, and no low-contention main cell reported a conflict.

The remaining gaps are substantial. At one worker, Rust STO reached 1.112
million engine commits/s versus 1.913 million for C ABI, 41.9% lower. Its
foreground ACK rate was 0.732 versus 1.159 million/s, 36.8% lower. At 32
workers, ACK reached 16.869 versus 20.142 million/s, 16.2% lower. The broader
main comparison still showed 37.1% to 42.4% lower four-key write throughput
and 31.6% to 38.4% lower RMW throughput. One-key reads reached 3.253 versus
5.184 million transactions/s at 32 workers, 37.2% lower.

The [final report and retained evidence](mako-cache-rust-sto-2026-09-12/final-production-2026-09-13/README.md)
contain every median/range, protocol, source/native manifest, binary hash,
machine snapshot and raw cell result. The main workload uses CRC32C and
queues of 262,144 divided by transaction size, not the earlier sustained
soak's 1,024-record queues. Its measured interval includes fresh thread
attachment, key selection, latency collection and joins. It is separate
from the steady-worker, undrained foreground ACK interval.

No new sustained GC soak was run on this final source. Earlier soak and
private-experiment results below retain their own source and binary
identities. They are not pooled into these final medians. Performance
acceptance and changing the default remain explicit open decisions.

## Retained changes in the initial frozen baseline

- Enable the existing bounded atomic value representation for cache tables.
  Values through 160 bytes avoid ArcSwap publication; larger values retain
  the existing shared representation. Stable entries grow from 64 to 192
  bytes. This is an explicit space/throughput tradeoff, not a free reduction
  in memory use.
- Reuse the wrapper's canonical touch log across transactions on a worker.
  The active prefix resets first-touch metadata, effects, keys, and values.
  Recycling keeps at most 64 entries and 64 KiB of backing storage per worker.
- Retire owned wrapper buffers before a successful fused ACK returns. The
  outer cache deliberately forgets its consumed facade on this path; relying
  on Rust `Drop` leaked the old touch-log/key allocations on every success.
  Retirement is idempotent if a caller instead drops the consumed facade,
  including after a newer transaction has begun.

The borrowed-staging experiment was removed. It eliminated one allocation
but improved ACK throughput by only about 2%, increased instructions, and
required additional lifetime handling for repeated writes, uncertain cleanup,
holder transfer, and unwinding. The final adapter uses the safe owned-value
STO API. No clock implementation or global Cargo profile was changed.

## Protocol and provenance

Measurements run on `zoo-002`, an AMD EPYC 7702P host, with CPU boost already disabled
and all governors left at `schedutil`. Cooperating performance jobs take
`/var/tmp/mako-zoo2-perf-shuai.lock`. Workers use physical CPUs `0..W-1`,
writeback uses CPU 32, and remaining process threads start on CPUs 33 through 63.
Machine/process snapshots and all governor settings are retained per cell.

Both engines use native production libraries, without fake ABI or test
hooks. The C++ archives use Clang 22, effective `-O2 -g -march=native`, RMW
enabled, and opacity disabled. Their build host target is Zen 1. Rust 1.95
uses `-C target-cpu=znver1 -C force-frame-pointers=yes -C debuginfo=1` and the
workspace release settings, `opt-level=3`, one codegen unit, and LTO disabled.
The separate thin-LTO experiment overrides LTO only in private benchmark
target directories; it does not change the workspace profile.

The Rust binaries are linked locally against copied zoo-002 RocksDB 8.9.1
using a linker wrapper that puts that library directory first. This avoids
accidentally selecting the local RocksDB 9.10. Both remote arms load the same
carried jemalloc library, matching the native build contract, and LLVM 22's
libc++/libc++abi. A remote build against a different jemalloc was rejected by
the native fingerprint verifier; stale CMake fingerprints also failed closed
and were rebuilt before the C ABI arm was linked.

Base revision: `9cd856c18107db33c868ee4e57f631e2577e6d4f`, plus the shared
integration worktree changes. Raw reports and immutable executables are under:

- zoo-002: `/var/tmp/mako-engine-perf-20260912.iz63eX`
- build host: `/var/tmp/mako-engine-perf-20260912.trsFuD`

Each diagnostic directory retains `protocol.json`, executable SHA-256 hashes,
raw stdout/stderr, per-cell environment snapshots, and `results.json`.
Source/archive snapshots accompany the build artifacts. Earlier variants are
retained as experiments, not mixed into final-source medians.

## Workloads

`engine_compare` separates three fixed-count workloads. Each worker updates
256 disjoint keys, with 8-byte keys and 128-byte values. It warms up with
65,536 commits per worker, then measures 262,144 commits per worker. All final key values and
commit counts are checked outside every worker's measured interval.

- `engine`: direct engine begin, put, and commit.
- `engine-logged`: the same operation with a synchronous one-put record sink,
  excluding the cache and RocksDB.
- `cache-ack`: cache foreground ACK with a deliberately large bounded queue
  that can hold every admitted operation. After checking visible values and
  accounting, the process discards its unapplied tail. This is not sustained
  storage throughput and is not presented as a drained/reopened result.

Three repetitions alternate engine order. Per-thread hardware cycles and
instructions cover the measured region; all reported diagnostic cells had
full PMU running time. Rust allocation counts come from a separate
instrumented build; they do not count native C++ allocations.

`gc_soak` is the separate sustained workload: production 1,024-record queues
per worker, 64-record batches, CRC32C, RocksDB WAL enabled, `sync=false`,
unthrottled admission, a complete drain, close, and verification in a new
process. Every final key, recovered applied count, and a post-reopen write is
checked. The paired launcher records 10-second RSS/HWM samples and rejects
engine identity, protocol, count, duration, or governor mismatches.
The launchers remove only the successful sample's generated RocksDB files.
Reports and expected-state manifests remain; failed or interrupted databases
are retained for inspection.

## Initial diagnosis

The original adapter performed four measured Rust allocations totaling 544
bytes per one-put transaction. A warmed-up two-million-commit W1 run achieved
about 862k engine commits/s and 710k logged commits/s; the earlier short 645k
pilot was not a stable comparison.

The original engine profile attributed 10.85% to ArcSwap debt repayment and
another roughly 3% to committed-value publication. The logged profile showed
1.08% in `SystemTime::duration_since`, 0.90% in timestamp allocation, and about
2.8% in an unresolved vDSO symbol. These samples did not justify replacing
the Rust physical clock with native RDTSCP.

A later branch-level audit corrected an initial commit-ID inference. C++ has
a global commit-ID allocator, but the production non-opacity,
non-consistency-check one-put update uses a row-local version increment and
does not call it. The measured unchanged-policy Rust STO binaries always
reserve a runtime-global commit ID with an `AcqRel` atomic increment.
That is a real high-thread scaling asymmetry in this baseline;
removing it requires preserving the Rust core's ordering and validation
contracts, not merely deleting an atomic operation.

The unchanged-policy engine measurements are consistent with increasing
contention: native cycles/commit were 1,055 at W1 and 1,094 at W32; Rust STO
rose from 1,797 to 3,020 while instructions changed only from 4,117 to 4,201.
This supports investigation of shared state, but does not attribute all of
the difference to the commit-ID counter. No naive per-worker ID range was
tested as a purportedly correct optimization. Such ranges can publish an
older ID after a different worker writes the same key, violating version
admission and ordered-ID contracts.

After pooling, direct engine/logged commits use one Rust allocation of 128
bytes; ACK uses two totaling 256 bytes. The removed borrowed experiment
reduced these to zero and one respectively, without a commensurate throughput
gain.

## Non-LTO paired medians

Units below are millions of committed transactions per second. Each row is
the median of three fresh-process cells; every cell passed value/accounting
checks. These are foreground diagnostics, not final acceptance.

| Mode | Workers | C ABI, paired with pooled | Rust STO, atomic only | Rust STO, pooled | Removed borrowed experiment |
|---|---:|---:|---:|---:|---:|
| Engine | 1 | 1.841 | 1.002 | 1.106 | 1.136 |
| Engine | 4 | 7.219 | 3.808 | 4.161 | 4.194 |
| Logged engine | 1 | 1.476 | 0.809 | 0.874 | 0.865 |
| Logged engine | 4 | 4.277 | 2.983 | 3.180 | 3.194 |
| Cache ACK | 1 | 1.157 | 0.570 | 0.723 | 0.743 |
| Cache ACK | 4 | 4.450 | 2.193 | 2.807 | 2.854 |

Atomic-only and borrowed variants each had their own interleaved C ABI arm;
their exact reference medians remain in their raw reports. The C ABI column
above belongs to the pooled experiment, not a denominator shared across runs.

The retained pooled changes improved ACK throughput by about 27% at W1 and 28% at W4 over
atomic-only. Nevertheless, pooled Rust STO remained about 37% below its paired
C ABI ACK reference at W1/W4. That gap prevents a performance-based default
switch on this evidence.

## Full unchanged-policy foreground sweep

The frozen non-LTO binaries completed all 90 cells, three repetitions for
each engine, mode, and worker count. Every cell passed PMU, environment,
engine identity, worker/mode, committed-count, and final-value checks.
Median millions of commits/s:

| Workers | Engine C ABI | Engine STO | Logged C ABI | Logged STO | ACK C ABI | ACK STO |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 1.883 | 1.105 | 1.456 | 0.851 | 1.155 | 0.732 |
| 4 | 7.334 | 4.125 | 4.562 | 3.205 | 4.471 | 2.858 |
| 8 | 14.745 | 6.857 | 1.332 | 1.800 | 8.222 | 4.942 |
| 16 | 28.612 | 12.652 | 1.017 | 1.576 | 14.827 | 8.653 |
| 32 | 55.373 | 20.840 | 0.952 | 1.469 | 19.545 | 10.946 |

The logged mode uses the generic callback record terminal. Even with a
per-worker namespace, that terminal takes the shared HLC gate; the cache's
single-producer holder terminal does not. Its W8-and-up drop is therefore a
different synchronization path, not a measurement of serialization cost
alone. Do not subtract logged throughput from engine-only or cache ACK
throughput to estimate the cost of record encoding.

Two later launcher checks now reject mismatched echoed worker/mode fields
and retain a scratch database if any count, engine, environment, or PMU check
fails. All 162 preserved pooled, thin-LTO, and final foreground records also
passed the strengthened checks. These launcher changes do not affect the
measured worker loop.

## Private thin-LTO experiment

The same pooled source was rebuilt with private target directories and
`CARGO_PROFILE_RELEASE_LTO=thin` for both engines. All 36 cells passed. Median
throughput in millions of commits/s:

| Mode | Workers | C ABI | Rust STO |
|---|---:|---:|---:|
| Engine | 1 | 1.862 | 1.190 |
| Engine | 4 | 7.516 | 4.477 |
| Logged engine | 1 | 1.470 | 0.952 |
| Logged engine | 4 | 4.369 | 3.396 |
| Cache ACK | 1 | 1.152 | 0.806 |
| Cache ACK | 4 | 4.493 | 3.113 |

Thin LTO improved Rust STO ACK throughput by about 11% over the non-LTO pooled
experiment, but it still trailed its paired C ABI reference by 30% at W1 and 31% at W4.
W1 ACK instructions fell from 5,174 to 4,956 per commit, versus 3,254 for
C ABI. This does not justify changing the default or the global release
profile, which explicitly disables LTO because of an existing mixed-staticlib
linking constraint.

Disassembly still shows a 208-byte result copied between core transaction
begin and the wrapper, plus post-call frame/worker cleanup handling around
the out-of-line terminal. Those are candidates for a later measured, scoped
change, not evidence that a broad protocol rewrite is needed.

The four 60-second thin-LTO sustained diagnostic cells all completed their
drain and fresh-process reopen. They used production queue limits, not the
large ACK-only queues:

| Workers | Engine | ACK commits/s | Drain seconds | Reopen seconds | Maximum sampled run RSS, MiB |
|---|---|---:|---:|---:|---:|
| 1 | C ABI | 70,385 | 0.016 | 54.77 | 123.7 |
| 1 | Rust STO | 71,232 | 0.013 | 54.15 | 113.9 |
| 4 | C ABI | 69,714 | 0.014 | 54.22 | 125.7 |
| 4 | Rust STO | 71,323 | 0.013 | 54.46 | 118.8 |

The writeback thread used essentially one full CPU. These short runs were
storage-bound and did not reach the five-minute retention window. They
provide no multi-cycle GC or steady-state retention claim. Reopen cost is
reported separately and is not hidden in the foreground throughput figure.

## Private ownership-transfer experiment

Private non-LTO variants tested caller-visible transaction begin and terminal
ownership transfers, separately and combined. The protocol driver stayed
out of line; no new unsafe code or borrowed values were involved. All 72
W1/W4 cells passed. ACK medians in millions of commits/s:

| Workers | Unchanged STO | Terminal only | Begin only | Combined |
|---|---:|---:|---:|---:|
| 1 | 0.730 | 0.721 | 0.729 | 0.721 |
| 4 | 2.825 | 2.792 | 2.825 | 2.801 |

Direct engine throughput improved by about 1% to 3%, but the cache ACK path
did not improve. None of these private variants was promoted. Their patches,
binary hashes, and raw reports remain available with the other artifacts.

A later provenance audit found that all four private variants used Rust
1.97.1, not the frozen worktree's Rust 1.95. Their build script ran outside
the toolchain-pinned worktree without an explicit toolchain override. Their
internal comparisons therefore share one compiler, but must not be combined
with the frozen Rust 1.95 measurements. The compiler difference also means
that their baseline's 22-instruction difference from the frozen baseline
cannot be attributed to the ownership patches. Subsequent private builds
pin Rust 1.95 explicitly and verify their baseline executable hash against
the frozen candidate.

## Preserved unchanged-policy sustained prefix

The first 120-second coordinator was interrupted after 55.68 seconds to
leave room for that allocator-contract review. It produced no completed
sample. Its database and `run-attempt.json` remain under
`sustained-final-120/r1-w1-c-abi.49u2aay0`, with `completed=false`,
`KeyboardInterrupt`, and child return code `-15`. It is excluded from every
throughput comparison.

The restarted 120-second sweep completed all ten cells in its first
repetition and the Rust STO W1 cell in its second repetition. All eleven
completed cells drained and passed fresh-process recovery. First-repetition
results follow; these are individual cells, not three-run medians.

| Workers | C ABI ACK commits/s | STO ACK commits/s | C ABI reopen seconds | STO reopen seconds |
|---|---:|---:|---:|---:|
| 1 | 70,482 | 71,099 | 106.37 | 108.56 |
| 4 | 70,249 | 70,201 | 107.04 | 108.73 |
| 8 | 69,238 | 70,798 | 106.77 | 107.55 |
| 16 | 70,263 | 68,877 | 107.88 | 105.51 |
| 32 | 69,445 | 69,084 | 106.60 | 107.40 |

The second Rust STO W1 cell achieved 70,427 ACK commits/s and reopened in
106.41 seconds. The next cell was interrupted during reopen to give the
new record-local candidate the measurement slot. Its attempted reopen ran
73.41 seconds, recorded `completed=false` and child return code `-15`, and
retained its database. It is not counted as a verified result. The completed
prefix remains under `sustained-final-120-v2`; saved per-cell records, raw
events, expected manifests, protocol, and governor settings passed the new
resume validator before the slot changed hands.

Resume skips only an exact verified prefix with matching executable hashes,
workload, environment, and reports. Incomplete attempts remain untouched and
restart in new scratch directories. Twelve mocked driver tests cover resume
and the existing fail-closed checks.

No 960-second baseline cell has started. The short sustained prefix does not
reach the five-minute retention window and provides no multi-cycle GC claim.
Candidate foreground measurements and the completed candidate-versus-C-ABI
960-second GC pair follow below.

## Record-local version candidate

The next candidate opts cache tables into a record-local, nonopaque version
domain. It does not reserve the old runtime-global commit ID for these
transactions. Core defaults and ordered callers retain the ordered policy;
cache HLC timestamps and log ordering are unchanged. The owned-value API,
buffer pooling, and non-LTO build policy are unchanged. The new source and
binary hashes are recorded in `record-local-build.json`.
These measurements use the preserved pre-format source snapshot and immutable
binaries. A later workspace-wide formatting pass does not relabel them.

All 72 cells at W1/W4/W16/W32 passed, with three alternating repetitions.
Median millions of commits/s:

| Workers | Engine C ABI | Engine STO | Logged C ABI | Logged STO | ACK C ABI | ACK STO |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 1.879 | 1.088 | 1.464 | 0.840 | 1.141 | 0.735 |
| 4 | 5.198 | 4.151 | 4.469 | 3.140 | 4.433 | 2.878 |
| 16 | 29.167 | 15.206 | 1.025 | 1.597 | 14.696 | 9.153 |
| 32 | 58.042 | 28.960 | 0.951 | 1.552 | 19.824 | 11.125 |

The C ABI W4 engine samples were 7.470, 5.195, and 5.198 million commits/s.
That spread is flagged. Process snapshots showed varying `lxd` activity, but
they do not establish the cause. No other workload was stopped or changed.
An independent W4/W8 check retained the original results and added 36 passing
cells. Its C ABI W4 engine samples were 7.362, 7.379, and 7.426 million
commits/s. The check's separate medians follow:

| Workers | Engine C ABI | Engine STO | Logged C ABI | Logged STO | ACK C ABI | ACK STO |
|---|---:|---:|---:|---:|---:|---:|
| 4 | 7.379 | 4.214 | 4.525 | 3.147 | 4.430 | 2.826 |
| 8 | 14.390 | 7.227 | 1.327 | 1.886 | 8.328 | 5.232 |

Against the earlier unchanged-policy Rust STO sweep, candidate engine
throughput improved by 20% at W16 and 39% at W32. W32 cycles/commit fell from
3,020 to 2,081, while instructions increased from 4,201 to 4,238. The ACK path
improved by only 6% at W16 and 2% at W32, and W1 remained nearly unchanged.
These are cross-run comparisons, not interleaved old-versus-new samples.
The paired native references and the independent W4 check remain visible
above. The remaining ACK gap still prevents a performance-based default
switch on these results.

Current-candidate profiles sampled user cycles at 1,999 Hz with frame-pointer
call graphs. All three workloads passed final values and counts, with no
lost samples. The retained foreground reports filter by worker thread ID,
excluding writeback, RocksDB, and main-thread work. They still include worker
warmup and verification, so their percentages are not measured-loop PMU
percentages. Weighting every raw foreground sample by its recorded cycle
period attributes 0.91% to `Clock::try_allocate_restricted_timestamp` at W1
ACK and 45.72% at W32 ACK. The retained `perf report` symbol tables report
0.96% and 47.25% against their smaller filtered symbol-table denominators.
These methods are kept distinct. Both show that shared timestamp allocation
accounts for nearly half the sampled high-thread foreground ACK cost.

The roughly 5% self share of the benchmark's per-put closure is not all
workload generation. Its sampled prologue and key/value setup account for
0.15% of raw foreground cycle periods in engine mode and 0.11% in ACK mode.
The rest falls in its return epilogue or inlined API and transaction-result
handling. These are code-range samples with PMU skid, not exact instruction
latencies. Startup, warmup, verification, and teardown also occupy much of
the short process lifetime, but do not enter the measured throughput interval.

In the current W1 engine profile, native `find_unlocked`, `reach_leaf`, and
`point_get_validated` account for about 9% of self cycles together. Their W1
ACK foreground share is about 7%. Recorded call chains expose additional
admission work. Raw-period-weighted samples whose stacks contain the trusted
native get bridge account for 20.25% of W1 engine cycles and 15.50% of W1 ACK
cycles. These inclusive costs are not a promised optimization gain; any
replacement must still perform its own checks and lookup. The engine
profile's vDSO time-helper samples all fall under native point lookup through
the ticker and RCU guard. The libc offset `0x188e35` in the reports resolves
to `__memcpy_avx_unaligned_erms` using the remote library's matching debug
build ID. It is not an unidentified clock cost.

Disassembly also confirms that the native restricted ACK path issues a
write-prefetch hint for its HLC before validation; the Rust path does not.
The Rust HLC shares a cache line with several other fields, but this
benchmark's ordinary per-worker ACK commits neither read nor write those
neighbors. External status polling is outside that finding. The
profile therefore supports true sharing of the HLC itself, not a claim of
proven false sharing. The private hint-only experiment below tests the
prefetch difference without changing timestamp allocation or ordering semantics.

## Long GC, drain, and recovery results

Both W32 arms completed a requested 960-second run with production queues,
CRC32C, WAL, and `sync=false`, then drained and passed verification in a fresh
process. Each recovered the exact applied count, checked all 8,192 final
keys, and completed a post-reopen write. These are one long run per engine,
not a full 960-second sweep across every worker count.

| Engine | Committed and recovered | ACK commits/s | Drain seconds | Reopen seconds | Maximum sampled run RSS, MiB | Maximum sampled reopen RSS, MiB |
|---|---:|---:|---:|---:|---:|---:|
| C ABI | 48,052,960 | 50,048 | 0.345 | 207.60 | 327.3 | 1,702.1 |
| Record-local STO | 50,358,159 | 52,299 | 0.067 | 222.47 | 246.1 | 1,710.1 |

Actual ACK intervals were 960.128 and 962.887 seconds. The second includes
workers finishing their final admitted operations after the stop signal;
the throughput denominator is not rounded down to 960. Applied throughput
was 50,031 and 52,295 commits/s. Largest observed recovery `VmHWM` values were
2,100.7 and 2,132.3 MiB, so the running-phase RSS figures do not describe
recovery's temporary memory demand. The two reopens also processed different
retained-log sizes; their times are not a same-data engine-only comparison.

Both runs reclaimed records across repeated GC passes. C ABI ended with
32,548,928 reclaimed and 15,504,032 retained; STO ended with 34,170,607
reclaimed and 16,187,552 retained. All 190 periodic status samples were
healthy with zero GC errors, and observed reclamation counts never decreased.
The samples captured 70 distinct GC cutoff values for C ABI and 71 for STO.
After 600 seconds, retained counts ranged from 9.78M to 16.62M for C ABI and
10.95M to 17.04M for STO rather than growing with total committed records.
Sampled RSS in that interval ranged from 247 to 310 MiB and 193 to 237 MiB.
These are observed bounds for this fixed-key 16-minute workload, not a proof
for arbitrary key cardinality or unlimited process lifetime.

The lower throughput after GC becomes active must not be hidden by quoting
the roughly 70k/s short pre-GC results. This pair establishes completed
drain, recovery, and multi-pass reclamation under the stated workload; it
does not remove the foreground performance gap. Successful generated scratch
databases were removed after verification. Raw logs, memory samples, and
expected-state manifests remain; interrupted baseline databases remain too.

## Private HLC prefetch comparison

The hint experiment completed 90 cells under Rust 1.95, non-LTO, with the
original fixed workload. Its unchanged baseline is byte-identical to the
record-local candidate. A no-hint control retains the same capability check,
opaque assembly operands, function sizes, and global addresses as the hint
variant. Only the hint and local instruction padding differ. This separates
the hardware hint from a changed HLC address or support-flag layout.

All cells passed value, count, environment, and PMU checks. ACK medians in
millions of commits/s:

| Workers | Unchanged STO | Matched no-hint control | Prefetch | Hint versus control |
|---|---:|---:|---:|---:|
| 1 | 0.738 | 0.739 | 0.725 | -1.9% |
| 4 | 2.859 | 2.863 | 2.781 | -2.9% |
| 8 | 5.308 | 5.385 | 5.465 | +1.5% |
| 16 | 9.094 | 9.489 | 10.302 | +8.6% |
| 32 | 11.159 | 11.175 | 16.900 | +51.2% |

At W32, cycles/commit fell from 5,233 in the control to 3,347 with the hint;
instructions changed from 5,353 to 5,340. The engine-only negative control,
which never calls this hook, showed no comparable scaling gain and retained
the same instruction counts. The hint has a measured low-thread cost as
well as a large W32 benefit. After the later cache rejection, the hint alone
was applied to production source following separate native and isolated
Miri checks, retained in `prefetch-promotion-qualification/`. Final-source
package qualification and paired timing remain separate from these private
binary results. No adaptive clock policy or worker-count threshold was
introduced.

A separate 30-cell pair then compared the immutable prefetch binary directly
with C ABI on the same original fixed workload. All three repetitions at
each worker count passed. Executable hashes still matched after the run,
and boost remained disabled. ACK medians in millions of commits/s:

| Workers | C ABI | Prefetch STO | STO versus C ABI |
|---|---:|---:|---:|
| 1 | 1.152 | 0.727 | -36.8% |
| 4 | 4.469 | 2.784 | -37.7% |
| 8 | 7.989 | 5.451 | -31.8% |
| 16 | 14.881 | 10.196 | -31.5% |
| 32 | 19.803 | 16.940 | -14.5% |

The hint closes much of the W32 ACK gap but does not establish parity with
C ABI. At W1, measured instructions per commit remain 3,257 for C ABI and
5,243 for prefetch STO. The resolved-identity-cache trial was paused at that
point because review found that a cached hit could skip the native runtime
health check. The later corrected trial and its rejection are recorded below.

## Broader cache transaction comparison

The foreground and sustained results above measure one-put overwrites, not
the whole transaction interface. A separate current-policy production pair
completed 12 configurations, with three repetitions per engine and a
fresh recovery after every sample:

- Read one key at W1, W4, and W32; read four keys at W1 and W4.
- Write four keys at W1 and W4.
- Read-modify-write one or four keys at W1 and W4 with disjoint worker keys.
- Read-modify-write four shared keys at W4 with high contention.

These binaries use neither private prefetch nor the resolved-identity cache.
They include the new untimed exact per-key sample and recovery verifier.
The source archive, executable hashes, and as-built native fingerprint are
recorded in `current-policy-main-build.json`. Later CMake test wiring does
not change those identities. Independent executable copies are listed in
`current-policy-main-preserved-binaries.json`; their old build target
directories were subsequently reused for a separate private experiment.

This uses the existing main benchmark, not `engine_compare`. Read phases
measure 262,144 commits per worker after 65,536 warmup commits. Write and RMW
phases measure 65,536 divided by transaction size, after 16,384 divided by
transaction size warmup commits. Both use CRC32C and the same cache options.
The foreground mode is `Concurrent`, which maps to per-worker engine log
ordering. Each queue's capacity is 262,144 divided by transaction size.

Seed and warmup writes drain before measurement. Warmup and measured phases
use different threads, so worker attachment and cold scratch allocation are
inside the measured phase. Timing also includes selected-key allocation,
latency recording, and thread joins. Workers and unpinned helper threads
share CPUs `0..W-1`; only writeback is explicitly pinned to CPU 32. This is
a separate end-to-end client comparison, not a steady-worker PMU comparison.
It does not cover W32 RMW or the full acceptance grid.

The first launcher attempt preserved ten successful read sample/recovery
pairs, then stopped on an incorrect metadata-lane expectation. Its first
write child passed exact state validation, but the launcher expected one
lane instead of the seed lane plus separate warmup and measured-thread
lanes. Those results and the unrecovered write database remain as an
explicitly incomplete attempt, excluded from final medians. The corrected
launcher derives exact metadata counts and byte extents from those lanes,
has independent regression coverage, and starts all 72 sample/recovery cells
in a fresh output directory, covering 36 C ABI/STO engine pairs. All 72
restarted cells passed exact per-key state checks,
full drain, fresh-process recovery, byte accounting, and environment/hash
checks. No low-contention cell reported a conflict. Median throughput in
thousands of committed transactions per second:

| Workload | Keys/transaction | Contention | Workers | C ABI | STO | STO versus C ABI |
|---|---:|---|---:|---:|---:|---:|
| Read | 1 | Low | 1 | 629.2 | 501.1 | -20.4% |
| Read | 1 | Low | 4 | 926.4 | 798.8 | -13.8% |
| Read | 1 | Low | 32 | 709.9 | 697.8 | -1.7% |
| Read | 4 | Low | 1 | 327.9 | 272.8 | -16.8% |
| Read | 4 | Low | 4 | 1,082.7 | 931.2 | -14.0% |
| Write | 4 | Low | 1 | 224.5 | 128.6 | -42.7% |
| Write | 4 | Low | 4 | 498.1 | 308.8 | -38.0% |
| RMW | 1 | Low | 1 | 363.9 | 225.1 | -38.1% |
| RMW | 1 | Low | 4 | 843.6 | 527.3 | -37.5% |
| RMW | 4 | Low | 1 | 144.3 | 96.4 | -33.2% |
| RMW | 4 | Low | 4 | 373.3 | 252.2 | -32.4% |
| RMW | 4 | High | 4 | 138.3 | 93.6 | -32.3% |

High-contention RMW had median conflict counts of 276,407 for C ABI and
322,176 for STO, alongside 65,536 successful measured commits in each cell.
Median logical p99 latencies were 87.7 and 168.7 microseconds. Every key's
counter was checked, including after recovery. Recovery open times across
the matrix ranged from 0.166 to 5.035 seconds.

Both engines share a serialization point in read-only cache commits. They
take the cache's read-only outcome-fence mutex and inspect every writer slot before
completing a concurrent read-only transaction. This source-level observation
explains why these figures must not be presented as standalone engine
point-get scaling; it is not a sampled cost attribution. The broader results
confirm functional coverage beyond overwrites, while the multi-key write
and RMW performance gaps remain.

## Rejected direct-mapped resolved-identity cache

The corrected private cache completed 171 W1 cells: three arms, three
position-balanced repetitions, and engine/ACK workloads with varied keys.
All cells passed exact counts, values, workload identity, PMU, executable
hash, and environment checks. The mixed-table case is engine-only because
the cache's public keyspace is single-table. No high-thread cache subset ran.

This is a new common workload driver with runtime cardinality, distribution,
key-width, and table-count controls. Its medians are internally paired and
must not be divided by the earlier fixed-driver results. The three arms
are baseline, checked identity cache, and checked cache plus the earlier
prefetch hint. Cache-only throughput changes against its paired baseline:

| W1 workload | Modeled hit rate | Engine change | ACK change | Cache plus prefetch ACK change |
|---|---:|---:|---:|---:|
| Cycle 256 keys | 59.8% | +3.4% | +2.4% | +2.9% |
| Cycle 1,024 keys | 14.1% | -8.9% | -4.3% | -2.7% |
| Cycle 16K keys | 0% | -11.4% | -7.9% | -6.0% |
| Uniform 16K keys | 3.2% | -4.2% | -4.2% | -3.7% |
| Skewed 16K keys | 84.5% | +11.1% | +11.4% | +9.2% |
| Streaming new keys | 0% | -5.9% | -3.0% | -2.0% |
| 256 keys, 32 bytes | 61.7% | +2.4% | +3.8% | +3.2% |
| 256 keys, 33 bytes | Bypass | -3.7% | -1.0% | -0.3% |
| 256 keys, 64 bytes | Bypass | -3.7% | -2.1% | -1.1% |
| Two tables, 256 keys each | 37.9% | -5.0% | Not supported | Not supported |

Hit rates are exact source-model predictions for this schedule and hash,
not sampled cache counters. Direct-map collisions prevent even the 256-key
cycle from being an all-hit workload. In the all-miss 16K engine cycle,
instructions/commit increased from 4,317 to 4,587. The 33-byte bypass case
increased from 4,262 to 4,333. Miss and bypass regressions therefore include
extra executed instructions, not just noisy process RSS measurements.

The cache reserves 512 entries of 64 bytes, or 32 KiB per worker/database
attachment, plus allocator and control-word overhead. At 32 workers this
adds 1 MiB of entry capacity; at the facade's 460-worker cap it adds
14.375 MiB per database. Historical database attachments follow the existing
process-lifetime retention model and are not globally bounded. Whole-process
RSS samples also include setup, verification, and queued records. They do
not isolate that 32 KiB allocation.

Only the corrected health-admitting variant was timed. The first private
cache used a resolved-record API that skipped native singleton health.
The replacement checks the existing native health ABI inside the core
access callback, before staging, and preserves error propagation into the
table and core runtime. Eight independent subprocess checks exercised the
actual native poison transition, including two negative controls that
restored the old hit path and committed incorrectly. Release benchmark
binaries contain neither the poison helper nor the test-only status override.

Private Miri checks passed all six table tests and nine unchanged engine
tests with isolation and leak checking enabled. The original full engine
run stopped because the prefetch fixture queried realtime under isolation.
A separate fixture-only copy seeded its stamp deterministically and passed
all ten engine tests. The original failure, both source identities, patch,
and logs are preserved in `private-resolved-miri`; this does not claim a
ten-test pass on the unchanged measured source or hardware-hint validation.

The source patches, health proof, functional checks, and full timing records
remain as evidence. This direct-mapped cache is rejected as a generic
default because its distribution-specific gains do not outweigh the miss
and bypass regressions. Neither the cache nor its combined-prefetch variant
was promoted to production.

## Raw STO attribution diagnostic

A separate fixed-workload binary compared ordinary facade transactions with
direct Rust core/Table transactions. Twelve fresh-process cells covered W1
and W4, three alternating repetitions per implementation. Both modes use
the same binary, Rust 1.95 non-LTO policy, owned values, native point lookup,
table limits, bounded atomic values, and record-local versions. Both are
unlogged, with no Mako HLC, queue writes, or RocksDB. Every final key and
commit count passed verification; PMU coverage was 1.0 in all cells.

| Workers | Facade M commits/s | Raw M commits/s | Raw throughput change | Facade cycles/commit | Raw cycles/commit | Facade instructions/commit | Raw instructions/commit |
|---|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1.071 | 1.332 | +24.4% | 1,855 | 1,490 | 4,298 | 3,517 |
| 4 | 4.040 | 4.959 | +22.8% | 1,953 | 1,592 | 4,310 | 3,528 |

Raw access omits facade TLS lookup, active-state tracking, facade identity
and transaction-limit checks, touch-log copies, canonical log/witness
bookkeeping, and terminal error translation. Core and table checks remain.
The result attributes their combined omission, not the cost of frame copies
alone. Raw access does not implement the full facade contract and is not a
production replacement. No boxing or other production change was included.
Its absolute rates must not be combined with other driver binaries.

The raw/facade protocol, results, exact source patch and manifest, build
script, and functional evidence are retained as `private-raw-sto-*`.
The immutable executable has SHA-256
`05894af04c9eeb785c6e9d556f7e5472daa627d3ccc398b2840c4439d7d4f96c`.

## Private concurrent-reader fence comparison

The reader candidate removes the cache's reader-only mutex and guard. It
keeps the packed ordering operation, all 460 writer-slot loads, exact active
generation draining, unknown-outcome checks, and engine validation. Neither
arm includes prefetch. Three alternating repetitions of read-one-key at
W1, W4, and W32 ran on both engines, with a fresh recovery after every sample.
All 36 sample/recovery cells passed exact state, counts, byte accounting,
zero warmup/measured conflicts, and pre/post hash and environment checks.
This uses the broader main driver's timing and placement described above.

| Engine | Workers | Baseline M commits/s | Reader M commits/s | Change |
|---|---:|---:|---:|---:|
| C ABI | 1 | 0.624 | 0.585 | -6.2% |
| C ABI | 4 | 0.933 | 1.705 | +82.7% |
| C ABI | 32 | 0.712 | 4.631 | +550.6% |
| STO | 1 | 0.518 | 0.532 | +2.8% |
| STO | 4 | 0.866 | 1.432 | +65.4% |
| STO | 32 | 0.733 | 3.034 | +313.8% |

The C ABI W1 regression appeared in every repetition. Median foreground
CPU time increased from 1,595 to 1,698 ns/commit, while the worker CPU/wall
ratio stayed at 0.9943. Attempt p50/p99 changed from 1,220/1,390 to
1,330/1,510 ns; logical p50/p99 changed from 1,380/1,580 to 1,480/1,690 ns.
All six W1 children used the same CPU-0 process placement and CPU-32
writeback setting. These observations implicate on-CPU cost, not extra
blocking or descheduling, but do not separate instructions, data/code layout,
or clock frequency. Main does not record measured-region PMU counters.
The W4 C ABI baseline also varied from 0.771 to 0.993 M commits/s.

The actual compile roots, 254 files each, differ only in cache `lib.rs` and
the Loom test file. Source archives and build/binary/native hashes are in
`private-reader-fence-build.json`. Copied build logs preserve a rejected
stale Cargo-cache attempt whose reader executable equaled the baseline;
both candidates were forcibly rebuilt before the four distinct executables
were released. No timing used the stale artifact. The three completed Loom
models, library and failure tests, original incomplete model attempt, and
mutex negative control remain in `private-reader-fence-build/`.

This is private read-only evidence, not a write-path improvement or final
production qualification. The low-thread regression motivated the following
layout and scan-bound controls.

A W1-only follow-up compared three variants in 18 position-rotated cells.
The third retained the original initialized mutex field but never locked it.
Separate release probes confirmed that its cache and fence sizes, alignment,
and all field offsets matched the original baseline in both engines.
This controls object layout, not code placement or allocator-selected
addresses. All 18 sample/recovery cells passed the same strict checks.

| W1 engine | Baseline commits/s | Removed field | Matched layout | Removed versus baseline | Matched versus baseline |
|---|---:|---:|---:|---:|---:|
| C ABI | 626,575 | 590,284 | 628,000 | -5.8% | +0.2% |
| STO | 515,114 | 533,337 | 505,922 | +3.5% | -1.8% |

C ABI worker CPU time was 1,587/1,684/1,583 ns per commit for
baseline/removed-field/matched-layout. STO measured 1,931/1,866/1,967 ns.
Preserving the original object layout removed the C ABI W1 regression in
this trial, but modestly regressed STO. No high-thread matched-layout trial
has run, and this experiment includes neither prefetch nor a writer-slot
scan-bound change. The probe, build, source, and timing evidence are retained
in `private-reader-layout-*`.

## Registered-prefix reader scan

The next private candidate keeps mutex removal and bounds the scan using
the existing monotone, non-recycling writer-slot registration count. It
loads that count after the packed ordering cut. A prefix writer registered
before its packed Release operation, so the acquired cut establishes that
the later counter load cannot omit it. The same slot-generation loads,
exact-generation waits, unknown-outcome checks, and engine validation remain.
No hot writer RMW, prefetch, or inert mutex field was added.

Two independent unbounded weak-memory models passed. An early-bound
negative control failed, and a source-isolated mutation of the actual helper
was killed by its call-order regression. The deterministic regression is
not itself a weak-memory proof. The private C ABI library passed 205 tests
and the STO hook-enabled library passed 206 before timing.

The fresh 54-cell matrix compared baseline, no-mutex reader, and bounded
reader at W1/W4/W32 in three position-rotated repetitions. All 108 child
sample/recovery phases passed exact state/count/byte checks, with zero
warmup and measured conflicts. All frozen executable and machine checks
passed. Median throughput is in millions of read commits per second:

| Engine | Workers | Baseline | No-mutex reader | Bounded reader | Bound versus baseline | Bound versus no-mutex |
|---|---:|---:|---:|---:|---:|---:|
| C ABI | 1 | 0.627 | 0.589 | 0.854 | +36.3% | +45.0% |
| C ABI | 4 | 0.956 | 1.700 | 2.276 | +138.0% | +33.9% |
| C ABI | 32 | 0.702 | 4.753 | 5.276 | +651.5% | +11.0% |
| STO | 1 | 0.522 | 0.531 | 0.690 | +32.2% | +29.8% |
| STO | 4 | 0.873 | 1.431 | 1.692 | +93.8% | +18.3% |
| STO | 32 | 0.732 | 3.090 | 3.267 | +346.5% | +5.7% |

This removes the earlier C ABI W1 regression in the tested workload and
improves both engines at every tested worker count. The scan bound counts
historical registered threads, not currently active writers. It can still
reach 460 in a long-lived process; these fresh-process measurements do not
claim the same gain after that history. The 460-slot allocation is unchanged.
The results also do not establish write-path gains or STO/C ABI parity.

`private-reader-bound-*` retains the exact protocol, results, source archive
identity, build inputs, patches, tests, and mutation evidence. The source
archive contains 254 source files plus one documented pre-existing Python
bytecode artifact. Final production source and timing will have their own
identities; these private binaries contain no prefetch.
