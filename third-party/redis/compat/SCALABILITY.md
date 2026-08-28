# Redis-Over-Mako Scalability

This report applies the useful parts of the Rolis scalability evaluation to
Mako's Redis compatibility layer:

- vary available server workers instead of holding the server at 32 workers;
- report total and per-worker throughput;
- compare Redis-over-Mako with a direct Mako transaction baseline;
- report server CPU, latency, repeat variance, and scaling efficiency; and
- keep preload, replication, and client-generation effects explicit.

## Worker Target Status

The shared-listener `makoCon` implementation is a correctness and performance
target from 1 through 32 Redis request workers for the command surface and
single-shard topology covered by this directory. The full worker sweep, active
worker accounting, bounded-retry runs, and standard plus hot-key G4 histories
are the evidence for that scope.

This supersedes earlier one-worker-only wording. It does not establish complete
Redis compatibility, production readiness, cross-shard scan semantics, or every
replication/failover topology; those remain separate claims with their own
acceptance checks.

## 2026-08-22–24 Paper-Profile Evaluation

The paper-profile run strengthens the upstream-merged measurements with a
10-second warmup, five 30-second samples per point, deterministic workload
ordering, repeat-level CPU sampling, and two-sided Student-t 95% confidence
intervals. It uses the same ag2 host, one million preloaded keys, 8-byte values,
an 80% GET / 20% SET uniform-random workload, two closed-loop clients per
server worker, and disjoint physical CPU pools for server and client work.

All Redis-facing rows in the tables below were repeated after bounding the
latency collector. The earlier unbounded-client runs are retained as diagnostic
artifacts but are not used in the consolidated paper tables.

`P1`, `P64`, and `P512` refer to pipeline depths 1, 64, and 512. They are not
partition counts. Every Redis-facing measurement below uses one unreplicated
Mako shard. At pipeline depths above one, the latency column is the time to
complete a submitted pipeline batch; it must not be described as independent
open-loop per-command latency.

### Pipeline scale-up

| Pipeline | Workers | Mean commands/s | 95% CI half-width | Direct retention | Mean batch p99 |
|---:|---:|---:|---:|---:|---:|
| 1 | 1 | 50,021 | ±5,328 | 3.36% | 55.6 us |
| 1 | 8 | 426,472 | ±24,896 | 3.84% | 66.0 us |
| 1 | 16 | 842,470 | ±31,076 | 4.16% | 69.8 us |
| 1 | 32 | 1,238,517 | ±4,265 | 4.05% | 111.6 us |
| 64 | 1 | 881,554 | ±691 | 59.20% | 156.6 us |
| 64 | 8 | 5,685,910 | ±7,090 | 51.20% | 196.6 us |
| 64 | 16 | 9,663,730 | ±12,827 | 47.71% | 240.8 us |
| 64 | 32 | 14,934,387 | ±11,521 | 48.87% | 349.0 us |
| 512 | 1 | 1,149,771 | ±1,541 | 77.22% | 779.6 us |
| 512 | 8 | 6,694,881 | ±10,776 | 60.28% | 1,151.6 us |
| 512 | 16 | 10,856,525 | ±19,267 | 53.60% | 1,505.4 us |
| 512 | 32 | 16,343,933 | ±35,377 | 53.48% | 2,366.6 us |

At 32 workers, depth 512 provides 9.4% more throughput than depth 64 but raises
mean batch p99 latency from 349.0 us to 2.367 ms. Depth 64 is therefore the
balanced operating point used for the sensitivity and stability experiments.
The repeated 32-worker depth-64 mixed run measured 14,907,975 commands/s, only
0.18% below the scale-up run, providing an independent stability check.

The direct-Mako mixed baseline measured 1,489,049, 11,106,032, 20,253,412, and
30,558,149 operations/s at 1, 8, 16, and 32 workers. Its coefficient of
variation ranged from 0.06% to 0.13%. The direct path remains an in-process
upper bound: it excludes TCP, RESP parsing and serialization, Redis semantics,
TTL checks, and response allocation.

### Operation mix at 32 workers and pipeline depth 64

| Workload | Redis-over-Mako/s | 95% CI half-width | Direct Mako/s | Retention | Mean batch p99 |
|---|---:|---:|---:|---:|---:|
| GET | 15,890,069 | ±13,666 | 31,010,980 | 51.24% | 326.8 us |
| 80% GET / 20% SET | 14,907,975 | ±11,774 | 30,670,770 | 48.61% | 348.6 us |
| SET | 13,281,871 | ±4,504 | 30,342,902 | 43.77% | 387.8 us |

The write-only path is 16.4% slower than the read-only path and retains a
smaller fraction of direct Mako throughput. This is consistent with additional
Redis adapter and response work on the mutation path, but the experiment does
not isolate individual CPU costs.

### Value size at 32 workers and pipeline depth 64

| Value | Redis-over-Mako/s | 95% CI half-width | Direct Mako/s | Retention | Mean batch p99 |
|---:|---:|---:|---:|---:|---:|
| 8 B | 14,907,975 | ±11,774 | 30,670,770 | 48.61% | 348.6 us |
| 64 B | 14,735,656 | ±16,423 | 29,620,385 | 49.75% | 354.8 us |
| 1 KiB | 5,272,502 | ±8,199 | 18,071,723 | 29.18% | 1,490.2 us |

Throughput is nearly unchanged between 8-byte and 64-byte values. At 1 KiB,
Redis-over-Mako throughput falls by 64.6% from the 8-byte result, while direct
Mako falls by 41.1%. The additional decline in retention, from 48.61% to
29.18%, bounds the combined cost of larger RESP messages, copying, allocation,
and same-host socket traffic; it does not identify one of those costs as the
sole cause.

### Bounded-client stability validation

The original long-run client retained every batch-latency observation. During
the first 30-minute attempt it reached approximately 209 GB RSS after the load
phase and then spent one CPU core merging and sorting the samples. The Mako
server remained responsive; this was a load-generator failure, not a server
liveness failure. That attempt is preserved as
`benchmark_logs/paper_20260822_soak_p64_failed_unbounded_latency_209gb/`.

The corrected client uses deterministic per-thread reservoir sampling. A
full-path regression observed 78,865,920 batch completions while retaining the
configured 65,536 samples, and passed. In the corrected 30-minute P64 soak, the
client observed 26,944,817,408 completions and retained 4,194,304 samples
(65,536 for each of 64 client threads). It sustained 14,969,054 commands/s;
batch p50, p95, and p99 were 248, 299, and 343 us.

During the connected load window, server RSS stayed between 887,668 and
888,456 KiB and ended 768 KiB below its starting value. File descriptors held
at 325 and threads held at 67. Observed client snapshots remained at 61,424
KiB RSS. The wrapper and both benchmark processes exited cleanly. These are
single-run stability measurements, not confidence intervals. The corrected
artifact is `benchmark_logs/paper_20260822_soak_p64/`.

### Preload robustness validation

Two 64-byte paper attempts failed before measurement because the loader treated
the first transient `-ERR backend` as fatal. A minimized one-million-key test
showed that both 8- and 32-thread loaders can complete after server startup, so
the value size and concurrency are not unsupported. Listener PING and worker
thread visibility are necessary but do not eliminate occasional preload
backend failures.

The harness now polls for the expected worker threads, records a five-second
backend-settle interval, and uses bounded retry only for idempotent preload
SETs. Each retry reconnects after one millisecond; exhausting 100 retries for a
key still fails the run. The successful 64-byte and 1-KiB preloads each logged
10 retries across one million keys. Timed workloads remain fail-fast. The
failed attempts are preserved alongside the successful artifacts.

### Scope and artifacts

These measurements support same-host overhead, scale-up, operation-mix, and
value-size claims for successful single-key Redis commands. They are not an
apples-to-apples comparison with published Rolis YCSB++ or Mako TPC-C
transactions, which perform multiple database operations and include different
replication and client-placement guarantees.

The exact protocol is in `PAPER_EVALUATION.md`. Consolidated tables and figures
are generated by `summarize_paper_evaluation.py` under
`benchmark_logs/paper_20260824_summary/`. Repeat-level evidence and manifests
are in:

- `benchmark_logs/paper_20260822_direct/`
- `benchmark_logs/paper_20260823_p1_reservoir/`
- `benchmark_logs/paper_20260823_p64_reservoir/`
- `benchmark_logs/paper_20260823_p512_reservoir/`
- `benchmark_logs/paper_20260822_mix_direct/`
- `benchmark_logs/paper_20260823_mix_p64_reservoir/`
- `benchmark_logs/paper_20260822_v64_direct/`
- `benchmark_logs/paper_20260824_v64_p64_reservoir_retryfix/`
- `benchmark_logs/paper_20260822_v1024_direct/`
- `benchmark_logs/paper_20260824_v1024_p64_reservoir_retryfix/`
- `benchmark_logs/paper_20260822_soak_p64/`

## 2026-08-22 Upstream-Merged PR 72 Results

The upstream-merged checkout was measured on `zoo-002` (ag2), an AMD EPYC
7702P with 64 physical cores and 128 logical CPUs. The source state is merge
commit `e5d7f585475fa9fbc854f5073add0250bf098986` plus the uncommitted PR 72
adapter migration recorded by each manifest. Clang 22.1.2, CMake 4.3.4, and a
`RelWithDebInfo` build produced both `makoCon` and `makoRedisDirectBench`.

The capacity runs used physical server CPUs 0-31 and separate physical client
CPUs 32-63, one million preloaded keys, 8-byte values, GET, SET, and uniform
80% GET / 20% SET workloads, a two-second warmup, and three 20-second samples.
Replication and benchmark-only ablations were disabled. The matched method
uses two closed-loop clients per worker; this is essential when comparing with
the prior Rolis-style and 2026-08-15 checkpoints.

### Mixed-Workload Capacity

| Pipeline | Workers | Clients | Redis-over-Mako/s | Direct Mako/s | Retention | Server cores | p50 | p99 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 16 | 32 | 812,229 | 20,285,165 | 4.00% | 14.45 | 33 us | 70 us |
| 64 | 16 | 32 | 9,644,631 | 20,285,165 | 47.55% | 15.07 | 186 us | 243 us |
| 512 | 16 | 32 | 10,935,750 | 20,285,165 | 53.91% | 14.87 | 1,307 us | 1,517 us |
| 1 | 32 | 64 | 1,237,922 | 30,464,167 | 4.06% | 26.89 | 46 us | 107 us |
| 64 | 32 | 64 | 14,885,505 | 30,464,167 | 48.86% | 29.00 | 247 us | 355 us |
| 512 | 32 | 64 | 16,300,217 | 30,464,167 | 53.51% | 28.47 | 1,802 us | 2,555 us |

Every Redis-facing row completed with zero client-visible backend failures and
zero reported aborts. Pipeline depth 64 remains the more balanced operating
point: depth 512 adds 13.4% throughput at 16 workers and 9.5% at 32 workers,
while increasing p99 latency by roughly 6.2x and 7.2x, respectively.

The first post-merge diagnostic mistakenly used the runner default of one
client per worker. At 16 workers it measured 395,349, 5,940,836, and 8,384,531
mixed operations/s at depths 1, 64, and 512. Those are valid lower-concurrency
curves, but they are not comparable with the earlier two-client capacity
method. This distinction explains the apparent roughly twofold regression
seen before the manifests were compared.

The 2026-08-15 pipeline checkpoint later in this document ran on `zoo-003`
(Intel Xeon E5-2683 v4), used one 10-second sample per depth, and reported
10.51 million operations/s at depth 64 and 12.25 million at depth 512 for 16
workers. The new ag2 measurements use three samples twice as long. Because the
hosts differ, the remaining 8-11% absolute gap at 16 workers is not evidence of
a code regression. The ag2 curve continues to 32 workers, where it reaches
14.89 million and 16.30 million operations/s.

### Post-Merge Correctness And Robustness

After performance testing, the same 32-worker `makoCon` binary passed 105/105
focused pytest cases, 8/8 ecosystem-client rows, 114/114 declared command
probe cases, all 11 scoped Redis Tcl files, a 97,109-operation G4 serializable
history, a 10-second soak, and 80 deterministic RESP fuzz cases. The acceptance
runner now checks helper file existence rather than executable mode, because
the tracked Python and shell helpers are intentionally mode `100644` and are
invoked through their interpreters. The generated Jedis project also pins Java
17 source and target levels for older Maven compiler-plugin versions.

G2 multi-shard, G3 failover, restart durability, and client failover require
explicit fixture commands and were N/A on this local ag2 run. They are not
included in the pass count.

### 2026-08-22 Artifacts

Matched two-client capacity evidence:

- `benchmark_logs/scalability_upstream_p1_c2_20260822/`
- `benchmark_logs/scalability_upstream_p64_c2_20260822/`
- `benchmark_logs/scalability_upstream_p512_c2_20260822/`

One-client diagnostic evidence:

- `benchmark_logs/scalability_upstream_p1_20260822/`
- `benchmark_logs/scalability_upstream_p64_20260822/`
- `benchmark_logs/scalability_upstream_p512_20260822/`

Each directory contains `manifest.json`, `scalability_raw.csv`, and
`scalability_summary.csv`. The final acceptance artifact is
`acceptance/ACCEPTANCE_20260822_215535_e5d7f585.txt`.

## 2026-07-27 Method

The measured host was `zoo-002`, an AMD EPYC 7702P with 64 physical cores,
128 logical CPUs, one NUMA node, and 503 GiB RAM. The run used commit
`d995f2b9d00695f045fb6ef16d2b404bfcff3e8a` plus the source and binary hashes
recorded in `manifest.json`.

| Setting | Value |
|---|---|
| Server workers | 1, 2, 4, 8, 16, 24, 32 |
| Server CPU set | Physical cores 0-31 |
| Saturating clients | Two closed-loop clients per server worker |
| Client CPU set | Separate physical cores 32-63, including their SMT siblings when needed |
| Keyspace | 1,000,000 preloaded decimal keys |
| Values | 8 bytes |
| Workloads | GET, SET, and uniform-random 80% GET / 20% SET |
| Warmup and samples | 2-second warmup, then three 10-second samples |
| Redis connection model | Persistent TCP, one outstanding command per connection, no pipelining |
| Mako topology | One shard, no replication |

Preload is outside every timed sample. This matters: a focused 32-worker run
needed 283,251 internal abort/retry attempts to insert one million new keys,
while the following 10-second mixed update/read sample committed 8,423,199
operations with zero aborts or retries.

The direct baseline uses `makoRedisDirectBench` against the same Mako table,
key format, key count, value size, and workload mix. It excludes TCP, RESP,
Redis command semantics, TTL metadata checks, adapter locks, and response
allocation. It is an in-process upper bound, not a Redis-equivalent
implementation.

## Saturated Results

All configured request workers were active at every point. Helper CPU stayed
below 0.19 core, so request workers account for nearly all measured server CPU.

| Workers | Clients | GET/s | SET/s | Mixed/s | Mixed server cores | Mixed p99 |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 2 | 40,955 | 39,357 | 40,378 | 0.93 | 60 us |
| 2 | 4 | 97,173 | 96,864 | 93,496 | 1.78 | 60 us |
| 4 | 8 | 216,406 | 206,748 | 218,768 | 3.69 | 59 us |
| 8 | 16 | 456,080 | 424,510 | 414,700 | 7.19 | 67 us |
| 16 | 32 | 776,142 | 714,744 | 756,587 | 14.10 | 77 us |
| 24 | 48 | **873,175** | **836,640** | **855,728** | 20.39 | 111 us |
| 32 | 64 | 844,160 | 824,105 | 829,871 | 27.97 | 154 us |

The useful interpretation is:

- Throughput scales strongly through 16 workers and peaks at 24.
- From 24 to 32 workers, GET falls 3.3%, SET falls 1.5%, and mixed falls 3.0%.
- GET per-worker throughput peaks at 57,010 operations/s at 8 workers, then
  falls to 26,380 operations/s at 32 workers.
- Mixed speedup is 21.19x at 24 workers and 20.55x at 32 workers relative to
  one worker. Scaling efficiency is 88.3% and 64.2%, respectively.
- At 32 workers the process uses about 28 cores, but adding clients beyond 64
  did not improve a calibration run. This is a capacity plateau for this
  closed-loop setup, not proof that every hardware execution unit is saturated.
- A focused 32-worker/64-client GET sample used 27.9 server cores and 27.1
  client cores at 842,879 operations/s. Same-host client generation is
  substantial, so this experiment does not establish a server-only maximum.
- High-worker points were stable. At 24 workers, coefficient of variation was
  0.42% for GET, 0.52% for SET, and 1.47% for mixed.

## Direct Baseline

| Workers | Direct GET/s | Redis GET/s | Redis as direct % | Direct GET/s per worker |
|---:|---:|---:|---:|---:|
| 1 | 1,594,813 | 40,955 | 2.57% | 1,594,813 |
| 2 | 3,367,012 | 97,173 | 2.89% | 1,683,506 |
| 4 | 6,542,843 | 216,406 | 3.31% | 1,635,711 |
| 8 | 11,722,455 | 456,080 | 3.89% | 1,465,307 |
| 16 | 21,333,808 | 776,142 | 3.64% | 1,333,363 |
| 24 | 26,965,436 | 873,175 | 3.24% | 1,123,560 |
| 32 | 31,740,839 | 844,160 | 2.66% | 991,901 |

Direct Mako continues gaining throughput through 32 workers, although its own
per-worker rate declines after two workers. Redis-over-Mako plateaus earlier,
so the 24-to-32 decline is not explained by Mako alone. Likely contributors
include protocol and syscall cost, adapter bookkeeping, lock/cache contention,
and same-host client generation. This experiment does not isolate those costs.

The 2.6-3.9% Redis/direct ratio is not directly comparable to Rolis's reported
Silo retention percentage. Rolis compares closer transaction paths, while this
experiment compares an in-process transaction loop with a synchronous TCP
Redis service that performs additional semantics.

## 2026-08-14 Specialized GET/SET Checkpoint

A focused before/after run on `zoo-003` (Intel Xeon E5-2683 v4, 32 physical
cores) measured the allocation-light C++ plain-string path. Server workers used
physical cores 0-15 and clients used physical cores 16-31. The workload used
one million 8-byte values, two closed-loop clients per worker, 80% GET / 20%
SET, no pipelining, a 2-second warmup, and one 10-second sample.

The benchmark client used for this checkpoint later proved to read RESP header
lines one byte per `recv`. Keep these numbers as a historical implementation
checkpoint, not as the authoritative capacity result. The 2026-08-15 runs below
use buffered reply reads.

| Workers | Before mixed/s | After mixed/s | Change | Before ops/server-core | After ops/server-core | p99 before | p99 after |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 36,312 | 36,703 | +1.1% | 41,834 | 44,380 | 90 us | 81 us |
| 16 | 457,082 | 457,973 | +0.2% | 33,220 | 33,905 | 184 us | 182 us |

All 16 workers were active and neither run reported an abort. The single repeat
makes this a directional implementation checkpoint, not a replacement for the
three-repeat 1-32-worker sweep above.

The final binary also passed 10-second, 16-client G4 histories at 16 workers:
19,784 committed operations across 10 keys and 19,455 operations on one hot
key, with no serialization anomalies in either history.

System `perf` was unavailable because the host sets
`kernel.perf_event_paranoid=4`. A pre-change gperftools CPU profile captured
44,575 samples; 27,177 (61.0%) landed in `recv`, `send`, `poll`,
`epoll_wait`, or `accept` stacks. Executable hot leaves also included
Masstree lookup/validation, generic executor teardown, malloc/free, RESP
parsing, and client-buffer splitting. This explains why removing generic
collection state and response allocation improves CPU efficiency but cannot
close the direct-Mako gap without batching or pipelining.

The specialized ABI returns a borrowed thread-local GET value. Unconditional
SET uses the direct path only when the primary string record already exists;
missing or collection-backed keys fall back to the generic executor so
cross-type replacement semantics remain centralized.

## 2026-08-15 Pipeline And Contention Checkpoint

The final focused run used PR-head commit
`049f605c059962604242bf799c9240171c5df62f` plus the changes described here on
`zoo-003`. Sixteen server workers were pinned to physical CPUs 0-15 and 32
closed-loop clients to physical CPUs 16-31. The workload retained one million
preloaded keys, 8-byte values, an 80% GET / 20% SET uniform mix, a 2-second
warmup, and one 10-second sample per depth. Replication was disabled.

The matched 16-thread direct-Mako run completed 20,466,840 operations/s using
15.99 CPU cores. It is still an in-process upper bound: Redis-over-Mako also
does TCP, RESP, TTL semantics, adapter synchronization, and replies.

The RESP client now buffers socket reads and sends each configured pipeline in
one write. Per-command latency is measured from the batch send until that
command's reply is completely read, so later replies in a batch include queue
time behind earlier replies. The server still executes and commits every Redis
command as a separate Mako transaction in connection order; this is Redis
pipelining, not transaction coalescing.

| Pipeline depth | Mixed/s | Direct-Mako retention | Server cores | p50 | p99 |
|---:|---:|---:|---:|---:|---:|
| 1 | 599,596 | 2.93% | 13.95 | 46 us | 145 us |
| 2 | 1,038,679 | 5.07% | 13.98 | 53 us | 166 us |
| 4 | 1,979,712 | 9.67% | 14.11 | 55 us | 166 us |
| 8 | 3,620,252 | 17.69% | 14.47 | 59 us | 149 us |
| 16 | 7,261,538 | 35.48% | 14.96 | 61 us | 121 us |
| 32 | 9,345,760 | 45.66% | 15.34 | 98 us | 167 us |
| 64 | 10,511,615 | 51.36% | 15.30 | 172 us | 274 us |
| 128 | 11,336,156 | 55.39% | 15.22 | 311 us | 540 us |
| 256 | 11,934,983 | 58.31% | 15.19 | 586 us | 934 us |
| 512 | **12,248,035** | **59.84%** | 15.15 | 1,136 us | 1,797 us |

There are two useful operating points:

- Depth 64 retains 51.4% of direct throughput with p99 below 0.3 ms.
- Depth 512 is the highest measured throughput point at 59.8% of direct, but gains only
  16.5% over depth 64 while increasing p99 by roughly 6.6x.

The non-pipelined result remains only 2.93% of direct throughput. High pipeline
depth makes aggregate throughput comparable enough for bulk workloads; it does
not make the latency or per-command connection model equivalent to direct Mako.
One repeat per depth also makes this a focused checkpoint, not a replacement
for a multi-repeat worker sweep.

### Controlled Ablations And Optimizations

The first buffered-client measurements used the specialized executor before
statistics and keyspace-lock sharding. Benchmark-only ablations deliberately
changed semantics and were never used for correctness claims.

| Configuration | Depth 1 mixed/s | Depth 4 mixed/s | Change from preceding normal point |
|---|---:|---:|---:|
| Normal specialized path | 536,782 | 1,419,115 | baseline |
| Skip TTL metadata | 605,969 | 1,536,181 | +12.9% / +8.2% |
| Skip all database work | 650,329 | 2,258,917 | +21.2% / +59.2% vs baseline |
| Exact per-worker metrics | 541,089 | 1,514,993 | +0.8% / +6.8% vs baseline |
| Per-key fast-path lock | 554,211 | 1,976,425 | +2.4% / +30.5% vs metrics-only |

These controls led to two semantics-preserving changes:

- Redis and Mako INFO counters use cache-line-separated per-worker atomics and
  exact aggregation instead of a globally contended increment per command.
- Specialized GET/SET use one of the existing 256 key stripes. `FLUSHDB` takes
  the exclusive generic keyspace lock and then all stripes before scanning,
  retaining keyspace exclusion without a shared-reader cache line on every
  plain command.

The no-TTL control shows that TTL metadata is a secondary cost. The no-database
ceiling shows substantial protocol, dispatch, scheduling, and reply cost even
without transactions. At saturated pipeline depths, the formerly shared
keyspace lock was the largest removable contention point found in this pass.

System sampling with Linux `perf` remained unavailable because
`kernel.perf_event_paranoid=4`; no flamegraph is claimed for this checkpoint.

### Checkpoint Validation

With all benchmark ablations disabled, the final binary passed:

- 40/40 Rust unit tests and 105/105 focused Python compatibility tests;
- exact aggregation of 3,200 sharded fast transactions and 3,202 processed
  commands across 32 pipelined connections;
- 30 `FLUSHDB` rounds concurrent with eight GET/SET clients, followed by a
  successful health write/read; and
- 10-second G4 histories with 20,704 operations over 10 keys and 20,529
  operations on one hot key, with no serialization anomalies.

Git-ignored evidence is under these directories:

- `benchmark_logs/ag3_pr72_final_pipeline_sweep_20260815/`
- `benchmark_logs/ag3_pr72_final_pipeline_tail_20260815/`
- `benchmark_logs/ag3_pr72_final_validation_20260815/`

## One-To-One Client Curve

A separate one-client-per-worker run reached 724,595 GET/s, 696,064 SET/s, and
722,039 mixed operations/s at 32 workers. It used only 18.4-18.9 server cores,
or about 58-59% of the allocated pool, with p99 below 73 us. Keep this curve as
a lower-load latency/concurrency result. Do not use it as the Rolis-style
capacity curve.

## Correctness Finding

The first high-concurrency run exposed one client-visible `-ERR backend` during
the 24-worker mixed warmup. SET already retried transient Mako aborts, while the
raw fast-path GET had only one attempt. GET and SET now use the same bounded
32-attempt policy. General core storage commands use the same retry policy, so
behavior does not depend on the fast, parsed, or transactional path. All 40
Rust unit tests pass. Both complete post-change worker sweeps plus a focused
32-worker metrics run completed without a client-visible backend failure.

The exact failed command in the original warmup was not logged, so the evidence
supports the retry fix but does not prove that a GET abort caused that one
failure. The benchmark now includes GET/SET context in future failure messages.

## Artifacts

Tracked plots:

- [Redis throughput](scalability_results/2026-07-27/scalability_redis_throughput.svg)
- [Redis per-worker throughput](scalability_results/2026-07-27/scalability_redis_per_worker.svg)
- [Direct and Redis throughput](scalability_results/2026-07-27/scalability_throughput.svg)
- [Direct and Redis per-worker throughput](scalability_results/2026-07-27/scalability_per_worker.svg)

The Git-ignored raw evidence is under:

`third-party/redis/compat/benchmark_logs/rolis_style_20260727_saturated/`

Important files:

- `manifest.json`: topology, exact settings, host details, source hashes, and
  binary hashes.
- `scalability_raw.csv`: all 126 repeat-level rows.
- `scalability_summary.csv`: means, standard deviations, speedup, efficiency,
  server/client CPU, latency, and direct-baseline ratios.
- `scalability_*.svg`: generated throughput and per-worker plots.
- `redis_*_pidstat.txt`: per-thread server CPU samples.
- `redis_server_*w.log`: one server log per worker count.

## Reproduce

Build `makoCon` and `makoRedisDirectBench`, then run:

```bash
export MAKO_SCALING_BUILD_DIR=/path/to/mako/build
export MAKO_SCALING_WORKERS='1 2 4 8 16 24 32'
export MAKO_SCALING_KEYS=1000000
export MAKO_SCALING_DURATION=10
export MAKO_SCALING_WARMUP=2
export MAKO_SCALING_REPEATS=3
export MAKO_SCALING_CLIENTS_PER_WORKER=2
export MAKO_SCALING_PIPELINE_DEPTH=64
export MAKO_SCALING_CLIENT_CPUS='32-63,96-127'
export MAKO_SCALING_OUT_DIR=/tmp/mako-redis-scalability

/usr/bin/python3 \
  third-party/redis/compat/run_scalability_benchmark.py

/usr/bin/python3 \
  third-party/redis/compat/plot_scalability.py \
  /tmp/mako-redis-scalability/scalability_summary.csv
```

Do not run destructive correctness suites, another `makoCon`, or competing
CPU-heavy work during this benchmark.
