# Redis-Over-Mako Scalability

This report applies the useful parts of the Rolis scalability evaluation to
Mako's Redis compatibility layer:

- vary available server workers instead of holding the server at 32 workers;
- report total and per-worker throughput;
- compare Redis-over-Mako with a direct Mako transaction baseline;
- report server CPU, latency, repeat variance, and scaling efficiency; and
- keep preload, replication, and client-generation effects explicit.

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
