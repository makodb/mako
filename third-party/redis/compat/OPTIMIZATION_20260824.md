# Redis-over-Mako performance optimization, 2026-08-24/25

This note records the diagnosis, code changes, and validation for the
Redis-over-Mako optimization on `zoo-002` (ag2). The checkout was based on
`e5d7f585475fa9fbc854f5073add0250bf098986` with the uncommitted PR 72 work in
`/home/users/ssoumojit/mako-pr`. All reported production measurements had the
benchmark-only ablations disabled.

The final evaluation used one Release build with `-O3 -DNDEBUG`. Its
`makoCon` SHA-256 is `45c80abfff40fa86274854c428484099a3adb95aa74e8e143fc84de355980b96`;
the direct-Mako benchmark built in the same tree is
`726ab0188929cd06bc9954fe65eeac93dc0465c3f555454d5524e18057b55797`.

## What was slow

The primary workload uses 32 server workers, two closed-loop clients per
worker, pipeline depth 64, one million uniformly selected keys, 8-byte values,
and an 80% GET / 20% SET mix. Each paper result has a 10-second warmup followed
by five 30-second measurements on disjoint physical server and client cores.

The correct pre-optimization server reached 14,934,387 operations/s with mean
p99 latency of 349 us. Three controlled ablations isolated the cost:

| Ablation | Mean operations/s | Change from correct baseline | Interpretation |
|---|---:|---:|---|
| No database work | 49,968,628 | +234.6% | RESP, FFI, and client traffic alone were not the limit |
| Skip TTL lookup | 15,499,315 | +3.8% | TTL metadata was measurable but not dominant |
| Skip key-stripe locks | 21,520,116 | +44.1% | False contention in the 256-lock table was the main correctable cost |

`perf` sampling was unavailable because the host has
`kernel.perf_event_paranoid=4`; the host security setting was not changed.
The ablations therefore provide the causal evidence for the optimization.

## Changes kept

The key-stripe table in `makoCon.cc` was increased from 256 to 16,384 mutexes.
Keys still map deterministically to a stripe, conflicting writes to the same
key still serialize, multi-key writes still acquire sorted unique stripes, and
`FLUSHDB` still excludes all key operations by acquiring the complete stripe
set. The larger table only makes unrelated keys far less likely to collide.
At the supported 32-worker limit its static memory cost remains below 1 MiB on
the tested standard library.

The existence and delete probes now reuse thread-local strings instead of
constructing scratch strings on every operation. The Rust socket writer now
clears a fully written response buffer in constant time and retains the
existing drain behavior for partial writes.

The diagnostic environment variables `MAKO_REDIS_BENCH_NO_DB`,
`MAKO_REDIS_BENCH_SKIP_TTL`, `MAKO_REDIS_BENCH_SKIP_LOCKS`, and
`MAKO_REDIS_BENCH_SKIP_MUTEX` are off by default and recorded in every
benchmark manifest. `MAKO_REDIS_BENCH_SKIP_MUTEX` retains the stripe hash but
does not acquire the mutex, allowing hash cost to be separated from mutex and
cache-line cost. These switches are not production optimizations and must
remain disabled for reported results.

Both lock switches apply only to the specialized plain GET/unconditional SET
executor. They do not bypass locking in `execute_transaction()`. The lock
ablation therefore explains the preloaded fast-path workload used here; it is
not evidence about MULTI or generic collection-command lock cost.

Pipeline commands were not combined into one Mako transaction. Doing so would
change Redis pipeline semantics and atomicity. A blind string upsert was not
added either. Redis collections occupy type-specific Mako key prefixes, so a
plain `SET` whose string record is missing must use the generic path to remove
any live set, hash, list, or sorted-set records before installing the string.
A general read cache was also not added: correct invalidation across TTL,
WATCH, transactions, deletes, and replication requires a separate design and
the TTL ablation showed that it would not address the principal bottleneck
found here.

No custom atomic spinlock replaced `std::mutex`. A cache-line-padded mutex
experiment was slower, and changing the synchronization primitive would have
increased the correctness and debugging burden without evidence of a gain.

## Follow-up ablations

The final binary contains a matched, default-off mutex ablation so the
remaining lock cost can be split more precisely. Five paper-profile repeats
of the same binary produced:

| Mode | Mean operations/s | 95% CI | Change from correct |
|---|---:|---:|---:|
| Correct, hash and mutex enabled | 19,764,330 | 19,750,560-19,778,100 | baseline |
| Hash enabled, mutex skipped | 21,469,489 | 21,451,278-21,487,701 | +8.63% |
| Hash and mutex skipped | 21,537,998 | 21,525,579-21,550,417 | +8.98% |

Only about 0.32% separates the two unsafe ablations. Replacing FNV with a
cheaper hash is therefore low priority; mutex acquisition and cache-line
movement account for nearly all of the residual gap. Padding each mutex to a
cache line reduced throughput to 19,653,993 operations/s, 0.56% below the
matched correct result, so that change was reverted.

The selected final validation binary uses `-O3`. Against the restored,
unpadded source, it reached 19,814,291 operations/s for the 8-byte workload
(+0.25% over O2) and 5,646,442 operations/s for the 1 KiB workload (+1.25%
over O2). This is a modest build-profile improvement, not a protocol or
atomicity change. LTO/PGO were not claimed without a separately trained and
validated artifact.

## Performance after the changes

The final paired O3 8-byte mixed result was 19,713,157 operations/s (95% CI
19,666,617-19,759,696; CV 0.190%) with mean p99 of 238 us. This is 32.0%
more throughput than the correct pre-optimization baseline and 31.8% lower
p99 latency. An independent run of the identical server binary measured
19,814,291 operations/s, showing about 0.5% run-to-run host variation; the
paired result below is used for retention because direct Mako was measured in
the same artifact.

| 32-worker workload | Redis-over-Mako ops/s | Direct Mako ops/s | Retained | Mean p99 |
|---|---:|---:|---:|---:|
| GET, 8-byte | 21,199,822 | 30,969,048 | 68.45% | 224 us |
| SET, 8-byte | 17,687,371 | 30,413,500 | 58.16% | 260 us |
| 80/20 mixed, 8-byte | 19,713,157 | 30,612,622 | 64.40% | 238 us |
| 80/20 mixed, 1 KiB | 5,647,837 | 18,086,055 | 31.23% | 1,054 us |

The 1 KiB case improved from 5,272,502 operations/s to 5,647,837 (+7.12%),
while mean p99 fell from 1,490 us to 1,054 us (-29.3%). The remaining
large-value gap is still dominated by network response construction,
serialization, and copies rather than the 8-byte lock path.

The optimized mixed-workload scale curve was:

| Workers | Operations/s | Speedup from one worker | Parallel efficiency |
|---:|---:|---:|---:|
| 1 | 889,705 | 1.00x | 100.0% |
| 8 | 6,313,357 | 7.10x | 88.7% |
| 16 | 11,869,955 | 13.34x | 83.4% |
| 32 | 19,713,157 | 22.16x | 69.2% |

The direct-Mako values are an in-process upper bound, not a comparison against
a networked system with equivalent Redis semantics. Published Mako or Rolis
figures are contextual only unless they are reproduced on the same host with
the same workload, CPU allocation, load generator, and durability guarantees.

## Validation

The final source and binary passed:

- 40/40 Rust unit tests in the release profile;
- 111/111 Redis/Mako pytest cases, including new collection-to-string, TTL,
  retry-counter, and concurrent fast-SET coverage;
- Redis CLI, redis-py, node-redis, ioredis, Jedis, redis-rs,
  redis_exporter, and fakeredis client checks;
- a G4 serializable history with 100,816 operations, 16 clients, and 10 hot
  keys. G4 exercises the generic `MULTI` + `GET` + `INCRBY` transaction path;
  it is not evidence for the raw fast-SET executor;
- 100 repeated concurrent fast-SET/GET tests using four writers, four readers,
  and 4 KiB values: 200,000 SETs and 200,000 GETs completed without a torn
  value, abort, or retry;
- all 11 scoped Redis Tcl files and all 114 command probes;
- 80 deterministic RESP fuzz cases;
- the bounded latency-reservoir regression, which observed 110,675,456
  latencies and retained exactly the configured 65,536 samples; and
- a 30-minute O3 paper-profile soak that sustained 19,793,296 operations/s
  with p99 of 233 us, zero transaction aborts, and 35,628,501,376 completed
  operations.

The final soak saved 348 five-second samples from the connected 1,800-second
window. RSS included a one-time client/latency-reservoir allocation and ranged
from 616,668 to 854,316 KiB over that complete window. A separately reported,
fixed 120-second post-start steady-state window contains 325 samples: RSS was
exactly 854,064 KiB throughout, its fitted slope was 0 KiB/hour, file
descriptors remained 325, and threads remained 67. Both the complete and
steady-state windows are present in `paper_soak_summary.json`; the startup
allocation is not discarded from the artifact.

Multi-shard atomicity and replicated failover remain fixture-dependent and
were not claimed by this optimization run.

Two earlier combined acceptance invocations each reported one intermittent
Tcl-file failure but discarded the component output. The complete Tcl suite
then passed on a fresh server, passed after an explicit G4-before-Tcl sequence,
and passed in the final instrumented end-to-end acceptance run. The acceptance
harness now preserves every component log so a recurrence will identify the
exact file and assertion rather than only reporting `10/11`.

## Artifacts

- `benchmark_logs/optimization_20260824_nodb_p64/`
- `benchmark_logs/optimization_20260824_skipttl_p64/`
- `benchmark_logs/optimization_20260824_skiplocks_p64/`
- `benchmark_logs/optimization_20260824_stripes16k_p64/`
- `benchmark_logs/optimization_20260824_stripes16k_buffers_p64/`
- `benchmark_logs/optimization_20260824_stripes16k_buffers_mix/`
- `benchmark_logs/optimization_20260824_stripes16k_buffers_v1024/`
- `benchmark_logs/optimization_20260824_stripes16k_buffers_scale/`
- `benchmark_logs/optimization_20260824_stripes16k_buffers_soak/`
- `benchmark_logs/optimization_20260824_matched_correct/`
- `benchmark_logs/optimization_20260824_matched_hashonly/`
- `benchmark_logs/optimization_20260824_matched_nolock/`
- `benchmark_logs/optimization_20260824_padded_mutex/` (rejected experiment)
- `benchmark_logs/optimization_20260824_o3_correct/`
- `benchmark_logs/optimization_20260824_o3_v1024_correct/`
- `benchmark_logs/optimization_20260824_o3_soak30m_correct/`
- `benchmark_logs/final_o3_20260825_mixed_scale/`
- `benchmark_logs/final_o3_20260825_get_set/`
- `benchmark_logs/final_o3_20260825_v1024/`
- `benchmark_logs/final_o3_20260825_soak30m/`
- `benchmark_logs/final_o3_20260825_latency_reservoir/`
- `atomic_logs/final_o3_20260825_rerun/`
- `acceptance/TCL_RERUN_20260825.log`
- `acceptance/ACCEPTANCE_20260825_121025_e5d7f585.txt`
- `acceptance/ACCEPTANCE_20260825_121025_e5d7f585_components/`

Every benchmark directory contains repeat-level CSV data, a statistical
summary, source and binary hashes, CPU topology, configuration, server logs,
and CPU samples. These files, rather than rounded values in this note, are the
source of record.
