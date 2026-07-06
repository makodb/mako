# Redis Performance Comparison: Old makoCon vs New Redis Adapter

Date: 2026-07-06

## Revisions

- Old implementation: `ec0f3cdbf516c2999dd5ffe13dfb34954a96f9cb`
  - Isolated checkout: `/home/users/ssoumojit/mako-perf-old`
  - Binary: `/tmp/mako-perf-old-build4/makoCon`
  - Old Redis surface is limited to the tiny makoCon subset, so only plain `SET`
    and `GET` are used for numeric comparisons.
- New implementation: `6873823b075593ead4096a409c8eadc594e0fc0a`
  - PR checkout: `/home/users/ssoumojit/mako-fork-pr`
  - Binary used: `/tmp/mako-fork-pr-build/makoCon`
  - The existing new build directory is not writable by this user, so the
    already-built PR binary was used.
- New selectable idle strategy implementation:
  - Same PR checkout with the old idle `sleep(Duration::from_millis(1))`
    removed.
  - Default/preferred path: `poll(2)` over the listener and connected client
    sockets. Blocking command deadlines are used as poll timeouts.
  - Research/ablation path: `MAKO_REDIS_IDLE_STRATEGY=yield` uses
    `std::thread::yield_now()`.
  - Binary used for final five-run comparison:
    `/tmp/mako-fork-pr-poll-build/makoCon`

## Environment

- Tool: `redis-benchmark`
- Host/port: `127.0.0.1:6380`
- Payload size: `128` bytes
- Key range: `-r 10000` for single-client runs
- Old server is hardcoded to 8 workers and port 6380.
- New server was run with both `MAKO_REDIS_THREADS=8` and
  `MAKO_REDIS_THREADS=1`.

## Comparable Numeric Results

These are the only fully comparable runs completed by both implementations:
single-client plain `SET` and `GET`.

| Command | Clients | Old 8-worker makoCon | New 8-worker adapter | New 1-worker adapter |
|---|---:|---:|---:|---:|
| `SET` | 1 | 17,094.02 req/s, p50 0.047 ms | 811.10 req/s, p50 1.215 ms | 809.78 req/s, p50 1.215 ms |
| `GET` | 1 | 18,214.94 req/s, p50 0.047 ms | 825.56 req/s, p50 1.199 ms | 823.93 req/s, p50 1.199 ms |

## Idle-Yield Ablation

The new Rust server loop originally slept for 1 ms whenever a worker made no
progress. Replacing that idle sleep with `std::thread::yield_now()` restores
single-client plain `SET`/`GET` performance to the old implementation's range.
This strongly suggests the original single-client regression was dominated by
event-loop idle scheduling, not by the mere presence of additional Redis
commands.

| Command | Clients | Original new 8-worker adapter | Idle-yield ablation |
|---|---:|---:|---:|
| `SET` | 1 | 811.10 req/s, p50 1.215 ms | 18,450.19 req/s, p50 0.039 ms |
| `GET` | 1 | 825.56 req/s, p50 1.199 ms | 21,052.63 req/s, p50 0.031 ms |
| `SET` | 8 | 38,167.94 req/s, p50 0.103 ms | 45,045.04 req/s, p50 0.087 ms |
| `GET` | 8 | 8,368.20 req/s, p50 1.167 ms | 47,393.37 req/s, p50 0.079 ms |

Note: `yield_now()` is an ablation, not necessarily the final production
policy. It improves latency but can burn more CPU when idle. A proper poller
or adaptive backoff should be evaluated before merging this behavior as-is.

## Poll-Based Implementation

Replacing the idle sleep with `poll(2)` keeps the single-client performance in
the old implementation's range while avoiding the intentional idle busy-yield
of the ablation. This is the better development candidate than raw
`yield_now()`. The final implementation keeps both flows available from one
binary, with `poll` as the default and `yield` available through
`MAKO_REDIS_IDLE_STRATEGY=yield`.

| Command | Clients | Old 8-worker makoCon | Original new 8-worker adapter | Idle-yield ablation | Poll-based implementation |
|---|---:|---:|---:|---:|---:|
| `SET` | 1 | 17,094.02 req/s, p50 0.047 ms | 811.10 req/s, p50 1.215 ms | 18,450.19 req/s, p50 0.039 ms | 16,393.44 req/s, p50 0.047 ms |
| `GET` | 1 | 18,214.94 req/s, p50 0.047 ms | 825.56 req/s, p50 1.199 ms | 21,052.63 req/s, p50 0.031 ms | 17,985.61 req/s, p50 0.047 ms |
| `SET` | 8 | no completed result in observed window | 38,167.94 req/s, p50 0.103 ms | 45,045.04 req/s, p50 0.087 ms | 43,290.04 req/s, p50 0.111 ms |
| `GET` | 8 | no completed result in observed window | 8,368.20 req/s, p50 1.167 ms | 47,393.37 req/s, p50 0.079 ms | 37,593.98 req/s, p50 0.135 ms |

The poll-based version is slightly slower than the pure yield ablation under
load, but it still meets the old-compatible single-client target and is much
safer for idle or low-traffic deployments.

## Five-Run Idle Strategy Comparison

These runs use the final selectable-idle-strategy binary
`/tmp/mako-fork-pr-poll-build/makoCon`. `poll` is the default and preferred
mode. `yield` is enabled with `MAKO_REDIS_IDLE_STRATEGY=yield`. Each row below
summarizes five `redis-benchmark` repetitions.

| Strategy | Command | Clients | Median req/s | Mean req/s | Median p50 |
|---|---|---:|---:|---:|---:|
| `poll` | `SET` | 1 | 16,233.77 | 16,092.70 | 0.055 ms |
| `poll` | `GET` | 1 | 15,797.79 | 15,937.11 | 0.055 ms |
| `poll` | `SET` | 8 | 54,644.81 | 54,543.40 | 0.095 ms |
| `poll` | `GET` | 8 | 50,251.26 | 50,681.09 | 0.095 ms |
| `yield` | `SET` | 1 | 17,152.66 | 18,301.87 | 0.039 ms |
| `yield` | `GET` | 1 | 20,080.32 | 19,915.51 | 0.039 ms |
| `yield` | `SET` | 8 | 53,763.44 | 54,023.02 | 0.079 ms |
| `yield` | `GET` | 8 | 46,728.97 | 47,911.31 | 0.087 ms |

Interpretation: `yield` remains the latency/throughput ablation and wins the
single-client median numbers, but `poll` remains preferred because it avoids
intentional idle spinning while staying in the old implementation's
single-client performance range.

## New-Only Completed Concurrency Runs

The old implementation did not produce a completed `redis-benchmark` result
line for observed concurrent runs (`c=8` and `c=50`), so these are not
apples-to-apples throughput comparisons. They are included to show that the new
implementation handles benchmark concurrency that the old one did not complete
within the observed window.

| Command | Server mode | Clients | Result |
|---|---|---:|---:|
| `SET` | new, 8 workers | 8 | 38,167.94 req/s, p50 0.103 ms |
| `GET` | new, 8 workers | 8 | 8,368.20 req/s, p50 1.167 ms |
| `SET` | new, 1 worker | 50 | 40,650.41 req/s, p50 1.087 ms |
| `GET` | new, 1 worker | 50 | 62,111.80 req/s, p50 0.471 ms |

## Excluded / Caveats

- `PING_INLINE` is excluded: the old parser did not handle the inline benchmark
  frame and produced parse errors.
- `PING_BULK` is excluded from the table: `redis-benchmark` did not emit a
  usable result line against the old server.
- Old concurrent `SET` runs began with high instantaneous rates but did not
  produce a final result line in the observed window and had to be interrupted:
  - `c=8`, `n=10000`: appeared stalled after initial progress.
  - `c=50`, `n=50000`: appeared stalled after initial progress.
- The new adapter has much broader Redis semantics than the old path. The
  single-client old-vs-new table intentionally compares only the shared plain
  string command surface.
- Both old and new benchmark server processes needed `SIGKILL` after benchmark
  runs because they did not exit promptly on `SIGTERM`.

## Commands Used

Old single-client:

```bash
redis-benchmark -h 127.0.0.1 -p 6380 -t set -n 10000 -c 1 -d 128 -r 10000 -q
redis-benchmark -h 127.0.0.1 -p 6380 -t get -n 10000 -c 1 -d 128 -r 10000 -q
```

New single-client:

```bash
MAKO_REDIS_THREADS=8 MAKO_PORT=6380 /tmp/mako-fork-pr-build/makoCon
redis-benchmark -h 127.0.0.1 -p 6380 -t set -n 10000 -c 1 -d 128 -r 10000 -q
redis-benchmark -h 127.0.0.1 -p 6380 -t get -n 10000 -c 1 -d 128 -r 10000 -q

MAKO_REDIS_THREADS=1 MAKO_PORT=6380 /tmp/mako-fork-pr-build/makoCon
redis-benchmark -h 127.0.0.1 -p 6380 -t set -n 10000 -c 1 -d 128 -r 10000 -q
redis-benchmark -h 127.0.0.1 -p 6380 -t get -n 10000 -c 1 -d 128 -r 10000 -q
```

Idle-yield ablation:

```bash
MAKO_REDIS_THREADS=8 MAKO_PORT=6380 MAKO_REDIS_IDLE_STRATEGY=yield /tmp/mako-fork-pr-poll-build/makoCon
redis-benchmark -h 127.0.0.1 -p 6380 -t set -n 10000 -c 1 -d 128 -r 10000 -q
redis-benchmark -h 127.0.0.1 -p 6380 -t get -n 10000 -c 1 -d 128 -r 10000 -q
redis-benchmark -h 127.0.0.1 -p 6380 -t set,get -n 10000 -c 8 -d 128 -r 50000 -q
```

Poll-based implementation:

```bash
MAKO_REDIS_THREADS=8 MAKO_PORT=6380 /tmp/mako-fork-pr-poll-build/makoCon
redis-benchmark -h 127.0.0.1 -p 6380 -t set -n 10000 -c 1 -d 128 -r 10000 -q
redis-benchmark -h 127.0.0.1 -p 6380 -t get -n 10000 -c 1 -d 128 -r 10000 -q
redis-benchmark -h 127.0.0.1 -p 6380 -t set,get -n 10000 -c 8 -d 128 -r 50000 -q
```
