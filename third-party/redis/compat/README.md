# Redis Compatibility Validation

This directory contains the correctness, ecosystem-client, claim, robustness,
and performance harnesses for Mako's Redis-compatible interface. Redis is the
wire protocol and command API; the default `makoCon` target executes data
operations through Mako transactions and Masstree. Redis is not a second
database behind this interface.

An optional bounded string cache can be enabled with
`MAKO_REDIS_CACHE_MB=<MiB>`. It is a read-through cache for plain strings
without TTL: cache hits bypass Mako, fast plain SET refreshes an existing entry
after the Mako commit, and generic Redis writes invalidate affected keys after
commit. The next GET reloads an invalidated value. TTL-bearing values and
collections always use Mako. Cache
coherence currently covers writes made through this `makoCon` process; leave it
disabled (the default) if independent clients write the same Mako keyspace.
Cache hit, miss, insertion, eviction, invalidation, entry, and byte counters are
reported by `INFO mako`.

The semantic target is `third-party/redis/cpp/makoCon.cc` with
`MAKO_REDIS_BACKEND=mako`, which is the default. The optional `memory` backend
and `makoConMultiTrd` are not correctness targets for the results below.
The worker-scaling methodology and 2026-07-27 results are in
[`SCALABILITY.md`](SCALABILITY.md).
The 2026-08-24 bottleneck ablations, performance changes, and final validation
are in [`OPTIMIZATION_20260824.md`](OPTIMIZATION_20260824.md).

Worker scope is part of that semantic target: the shared-listener server is
validated with 1-32 request workers for the covered single-shard command and
concurrency surface. Historical one-worker-only wording is stale. This does not
extend the claim to complete Redis compatibility, every replication topology,
or unfinished cross-shard operations.

### 2026-08-22 Upstream-Merge Revalidation

PR 72 was merged locally with upstream `mako-dev` at merge commit
`e5d7f585475fa9fbc854f5073add0250bf098986` and rebuilt on `zoo-002` (ag2)
with Clang 22.1.2 and CMake 4.3.4. The Redis adapter and direct baseline were
migrated from the removed `Get`/`Put` storage facade to upstream `tx_get` and
`tx_put` helpers.

The post-merge binary passed 105/105 focused pytest cases, all 114 command
probe cases, all 11 scoped Redis Tcl files, the 8/8 ecosystem-client matrix,
a 97,109-operation G4 serializable history, the 10-second soak guard, and all
80 RESP fuzz cases. Fixture-dependent multi-shard, replication failover,
restart, and client-failover checks remain explicitly N/A in the local ag2
acceptance artifact; they are not counted as passes.

The matched capacity method uses two closed-loop clients per worker, one
million 8-byte values, an 80% GET / 20% SET uniform-random mix, a two-second
warmup, and three 20-second samples. At 16 workers, Redis-over-Mako measured
812,229 operations/s at pipeline depth 1, 9.64 million at depth 64, and 10.94
million at depth 512. The matched direct-Mako baseline was 20.29 million
operations/s. At 32 workers the three Redis-facing results were 1.24 million,
14.89 million, and 16.30 million operations/s, respectively.

The older 2026-08-15 pipeline checkpoint below was measured on `zoo-003`, not
ag2, and used one 10-second sample per depth. Its absolute numbers are retained
as historical evidence, not as an identical-host regression threshold. Full
ag2 methods, variance, CPU accounting, latency, and artifact paths are in
[`SCALABILITY.md`](SCALABILITY.md).

## Latest Validation Snapshot

### 2026-09-17 Phases 2 and 3 (final code commit `835cec52`)

Collected on ag2 (`zoo-002`) against the final code of phase 3 package 10, one
server per run on a private port, suites run serially. Every gate was also run
after each earlier package and stayed green throughout.

| Check | Result |
|---|---|
| Rust unit tests (`cargo test`) | PASS, 94/94 |
| `test_phase2_commands.py` | PASS, 1,328 checks, 0 failures |
| Focused pytest suite | PASS, 111 passed, 2 skipped (the two string-cache cases run only with `MAKO_REDIS_CACHE_MB` set) |
| Redis 7.4 Tcl semantic guard | PASS, 11/11 scoped files |

Non-gating Redis Tcl files, run for information:

| File | Result | Why the failures are not regressions |
|---|---|---|
| `unit/scripting`, EVAL half | 73 passed, 25 failed, 20 skipped | Script debugger, effects replication and other scripting internals the adapter does not claim |
| `unit/type/stream` | 66 passed, 6 failed, 8 skipped by tag | All six assert Redis's radix-tree node granularity for `~` trimming |
| `unit/type/stream-cgroups` | 53 passed, 2 failed, 3 skipped, stopped by 1 exception | `errorstat` counters, the dirty counter on history replay, and `RESTORE` of a real Redis RDB payload |
| `unit/geo` | 61 passed, stopped by 1 exception, 2 not reached | A single 20,000-member `GEOADD` hits the transaction size limit in [Known Limits](#known-limits) |

Throughput sanity after the final package: 15,870,779 operations/s on the
development profile (32 workers, pipeline depth 64, `MAKO_REDIS_CACHE_MB=256`),
0.15% below package 9 and 0.9% below package 8, inside the 5% regression bound
every package was held to. This
is a regression check, not a capacity claim; the capacity methodology is in
[`SCALABILITY.md`](SCALABILITY.md) and has not been rerun on this binary.

### 2026-07-21 Snapshot

The following results were collected on 2026-07-21 from branch
`redis-compat-phase3`, based on commit
`c28e39f2affee7c74ecb9747342c0811dd560053` plus the Redis compatibility
hardening documented in this snapshot. The server used 32 Redis request workers
on a 128-logical-CPU host. Tests were run serially against the same final
rebuilt binary on port 6396.

| Check | Result | What it establishes |
|---|---:|---|
| Rust unit tests | PASS, 40/40 (2026-07-27 rerun) | RESP parsing, reply formatting, command classification, worker wakeups, blocked-client ordering, and retry eligibility |
| Focused pytest suite | PASS, 105/105 | Redis and Mako agree on the claimed command behavior covered by local tests |
| Command-tier probe | PASS, P0 34/34, P1 48/48, P2 32/32 | All 114 declared probe cases completed successfully on Mako |
| Ecosystem client matrix | PASS, 8/8 rows | Common clients and tools can connect and exercise the scoped command surface |
| Redis 7.4 Tcl semantic guard | PASS, 11/11 scoped files | In-scope upstream Redis command semantics pass under the documented filters |
| G4 serializable RMW oracle | PASS, 91,615 operations | 16 concurrent clients updating 10 keys produced contiguous commits and matching final values |
| RESP fuzz guard | PASS, 80 cases | Random/malformed frames did not kill the server; immediate and delayed health checks passed |
| Soak guard | PASS, 577 serial SET/GET pairs in 10 seconds | Basic repeated operation and process-resource liveness; this is not a throughput benchmark |
| Worker CPU benchmark | PASS | Active request workers matched persistent client count and the server did not saturate its configured workers |

### 2026-08-15 PR 72 Delta

A focused 16-worker run at PR-head commit
`049f605c059962604242bf799c9240171c5df62f` plus the local changes in this
checkout added a specialized allocation-light GET/SET executor, exact
per-worker metric shards, distributed fast-path key locking, and a buffered
pipeline-capable scalability client.

The final 80% GET / 20% SET curve reached 10.51 million operations/s at
pipeline depth 64 with p99 274 us, or 51.4% of the matched 20.47 million
operations/s direct-Mako baseline. Depth 512 reached 12.25 million operations/s
(59.8% of direct) at p99 1.80 ms. With no pipelining it reached 599,596
operations/s (2.93% of direct), so latency-sensitive single-outstanding-command
traffic is still not comparable to direct Mako. Full method, ablations, CPU
accounting, and the latency tradeoff are in [`SCALABILITY.md`](SCALABILITY.md).

With benchmark-only ablations disabled, the final 16-worker binary passed all
40 Rust tests, all 105 focused pytest cases, exact sharded metric checks, 30
concurrent `FLUSHDB` rounds, and standard plus hot-key G4 histories. This is
additional evidence for the documented 1-32-worker single-shard scope; it does
not broaden the command or topology claims.

The generated ecosystem-client result file is
`third-party/redis/compat/client_test_results.csv`. CSV and JSON artifacts are
ignored by Git; the dated human-readable findings are retained here.

## Where The Tests Are

| Test or artifact | Location |
|---|---|
| Rust protocol and worker unit tests | `third-party/redis/rust-lib/src/lib.rs` |
| Focused Redis/Mako pytest cases | `third-party/redis/compat/test_*.py` |
| Kvrocks-derived set cases | `third-party/redis/compat/kvrocks_set_cases/` |
| Shared pytest Redis/Mako fixtures | `third-party/redis/compat/conftest.py` |
| Ecosystem-client runner | `third-party/redis/compat/run_client_tests.sh` |
| Latest client result CSV | `third-party/redis/compat/client_test_results.csv` |
| Command tiers and probe | `third-party/redis/compat/command_tiers.json`, `probe_commands.py` |
| Vendored Redis 7.4 tests | `third-party/redis/redis-tests/tests/` |
| Scoped Tcl runner and policy | `run_tcl_suite.sh`, `tcl_scope.txt`, `tcl_known_skips.txt` |
| G2 cross-shard claims | `run_bank_transfer.py`, `run_cross_shard_demo.py` |
| G3 failover claim | `run_failover_durability.py` |
| G4 isolation claim | `run_elle_isolation.py` |
| Robustness and operational guards | `run_fuzz.sh`, `run_soak.sh`, `run_restart_durability.py`, `run_client_failover.py` |
| Worker CPU sampler | `run_worker_cpu_benchmark.py` |
| Worker-scaling runner | `run_scalability_benchmark.py` |
| Paper evaluation protocol | `PAPER_EVALUATION.md` |
| 2026-08-24 optimization report | `OPTIMIZATION_20260824.md` |
| Paper soak runner | `run_paper_soak.sh` |
| Paper soak summarizer | `summarize_paper_soak.py` |
| Latency reservoir regression | `run_latency_sampling_regression.sh` |
| Paper results generator | `summarize_paper_evaluation.py` |
| RESP scalability client | `bench_resp_scalability.cpp` |
| Direct Mako baseline | `examples/makoRedisDirectBench.cc` |
| Scalability report and plots | `SCALABILITY.md`, `plot_scalability.py` |
| Full acceptance orchestrator | `run_acceptance.sh` |
| Intentional incompatibilities | `known_divergences.txt` |

## Focused Pytest Coverage

The table below is the 2026-07-21 breakdown of 105 cases; the suite now
collects 113. All are project-owned tests. Seven set cases are ports of
in-scope Apache Kvrocks behavior; their source and excluded cases are recorded
under `kvrocks_set_cases/`. They are not an unmodified run of the complete
Kvrocks test harness.

| Area | Cases |
|---|---:|
| Kvrocks-derived set behavior | 7 |
| Counters | 4 |
| Delete, unlink, and existence | 5 |
| Binary encoding round trip | 1 |
| Hashes | 5 |
| Lists | 8 |
| Multi-key strings | 4 |
| Pub/Sub | 5 |
| Scan family | 5 |
| Key scan and database size | 8 |
| SET options | 11 |
| Sets | 12 |
| Internal tag-index safety | 1 |
| TTL commands | 12 |
| TTL, CONFIG, and CLIENT behavior | 5 |
| TYPE, WAIT, TIME, and EXEC behavior | 5 |
| Sorted sets | 7 |
| **Total** | **105** |

These tests cover exact replies, missing versus empty values, binary values,
wrong-type behavior, conditional writes, expiry, transaction order, collection
operations, scans, blocking wakeups, and Pub/Sub behavior. They do not by
themselves prove full Redis compatibility, durability, sharded correctness, or
failover safety.

## Ecosystem Client Matrix

`run_client_tests.sh` tests both a reference Redis target and Mako where the
runner supports comparison. The latest generated CSV contains:

| Tool or client | Result | Latest detail |
|---|---:|---|
| pytest | PASS | 105 passed in 14.91 seconds |
| redis-cli | PASS | RESP3, PING, SET, MGET, ECHO, MULTI, and DEL on both targets |
| redis-py | PASS | Smoke completed on both targets |
| node-redis and ioredis | PASS | Both Node clients completed on both targets |
| Jedis | PASS | Java client smoke completed on both targets |
| redis-rs | PASS | Filtered Rust client smoke completed on both targets |
| redis_exporter | PASS | Scrape reported `redis_up 1` for Mako |
| fakeredis-py | PASS | 18 passed, 18 deliberately deselected, 1 warning |

The fakeredis and redis-rs rows are filtered to the claimed command surface.
They are not claims that every upstream test in those projects passes.

## Redis Tcl Semantic Guard

`run_tcl_suite.sh` runs upstream Redis 7.4 external-server tests against the
configured `MAKO_HOST` and `MAKO_PORT`. The final run passed all 11 scoped
files: string, hash, list, set, sorted set, expiry, scan, transaction, keyspace,
networking, and Pub/Sub.

The runner uses `--singledb --ignore-encoding --ignore-digest` and denies the
`slow`, `needs:debug`, and `needs:repl` tags. This tests external command
semantics without claiming Redis object encodings, debug internals, or
replication behavior. `--singledb` keeps the whole suite on database 0, so the
logical databases the adapter now offers are covered by
`test_phase2_commands.py` rather than by the TCL guard.

Explicit skips are kept in `tcl_known_skips.txt`:

- Keyspace notification events generated by mutations.

The three stream skips that lived there until package 9 -- `Blocking commands
ignores the timeout` in `unit/multi`, and `COPY basic usage for stream` and
`COPY basic usage for stream-cgroups` in `unit/keyspace` -- are gone, because
the adapter now implements streams and all three pass.

Whole-file exclusions are recorded in `tcl_scope.txt`: Redis Cluster,
Sentinel, modules, RDB/AOF internals, ACL, TLS, and client-side caching. A skip
is not counted as a pass for that excluded feature. `unit/scripting` and, since
package 9, `unit/type/stream` and `unit/type/stream-cgroups` are not in the
gated set either, but they are run as reported, non-gating checks: each
exercises Redis internals well beyond the command surface this adapter claims
(the script debugger and effects replication for the first; radix-tree macro
node trimming granularity, the private RDB stream payload and Redis's
`rdb_changes_since_last_save` bookkeeping for the other two), so their failures
are recorded in `known_divergences.txt` rather than treated as regressions.
`unit/type/stream` is run from a scratch copy with the `r multi` and `r exec`
lines removed from its `insert_into_stream_key` proc, so the 10,000 entries go
in as a pipeline instead of as one transaction; a transaction that large
crashes the executor, a limit that predates streams and that plain SET
reproduces at 5,000 commands. Nothing that file asserts changes.

The list suite specifically passes cross-worker blocking behavior, fairness for
multiple blocked clients, nested unblock order, blocking operations inside
transactions, `WATCH` invalidation caused by list moves, timeout handling, and
command-stat accounting. These cases exposed and now guard the worker-wakeup
and multi-key blocked-client ordering fixes.

## Command Probe

`probe_commands.py` executes every command listed in `command_tiers.json`
against reference Redis and Mako, classifies protocol/arity/command failures,
and writes a JSON result. The latest isolated run reported:

| Tier | Successful | Declared cases |
|---|---:|---:|
| P0 | 34 | 34 |
| P1 | 48 | 48 |
| P2 | 32 | 32 |
| **Total** | **114** | **114** |

The Mako-specific checks also confirmed that the internal `0x01` key prefix is
rejected and that `INFO` exposes connection and Mako transaction metrics.

The probe uses `FLUSHDB` and `FLUSHALL`. Do not run it concurrently with G2,
G4, Tcl, soak, or performance workloads; destructive cleanup can reset another
harness's keys and produce an invalid failure.

## Isolation And Robustness

The built-in G4 harness ran 16 clients for 30 seconds over 10 keys. Every
transaction read one value and incremented it in one `MULTI`/`EXEC`. It
accepted 91,615 operations, found no gaps or duplicates in committed values,
and confirmed each final value equaled its committed-write count.

This is a focused Redis-facing serializable read-modify-write oracle. It is not
the external Elle analyzer. External Elle remains available when an Elle JAR
and a compatible history are supplied.

The 10-second soak run completed 577 serial `redis-cli` SET/GET pairs and ended
with 465,600 kB RSS, 261 file descriptors, and 67 process threads. The resource
figures describe one process after the full test sequence and are not a memory
leak conclusion. The fuzz guard sent 80 deterministic valid and malformed RESP
payloads, then verified PING immediately and after the configured delay.

## Worker CPU Benchmark

The CPU test used the default Mako backend, a preloaded in-memory GET hit,
persistent clients, no Redis pipelining (`-P 1`), a one-second warmup, and five
one-second `pidstat` samples. Throughput came from the Mako `INFO`
`total_commands_processed` delta over the sample. Request-worker CPU is the sum
of the 32 Redis request threads; helper CPU is the remainder of the `makoCon`
process, including Mako transport threads. Client CPU is not included.

### One Load-Generator Thread

| Clients | Active request workers | Request-worker CPU | Helper CPU | Total cores | GET/s |
|---:|---:|---:|---:|---:|---:|
| 1 | 1 | 55.69% | 36.73% | 0.9242 | 16,108 |
| 2 | 2 | 123.16% | 30.53% | 1.5369 | 39,797 |
| 4 | 4 | 169.86% | 31.54% | 2.0140 | 50,350 |
| 8 | 8 | 198.40% | 32.34% | 2.3074 | 51,716 |
| 16 | 16 | 197.01% | 29.54% | 2.2655 | 49,296 |
| 32 | 32 | 222.04% | 34.25% | 2.5629 | 50,376 |

### One Load-Generator Thread Per Client

| Clients | Active request workers | Request-worker CPU | Helper CPU | Total cores | GET/s |
|---:|---:|---:|---:|---:|---:|
| 1 | 1 | 56.00% | 35.80% | 0.9180 | 17,795 |
| 2 | 2 | 110.18% | 37.52% | 1.4770 | 33,548 |
| 4 | 4 | 224.76% | 36.72% | 2.6148 | 63,297 |
| 8 | 8 | 444.92% | 35.52% | 4.8044 | 124,906 |
| 16 | 16 | 903.22% | 36.10% | 9.3932 | 251,890 |
| 32 | 32 | 1,935.73% | 27.94% | 19.6367 | 524,700 |

The server activates exactly N request workers for N persistent clients. The
single-thread generator plateaus around 50,000 GET/s after four clients, so its
small dip at higher client counts is a client-side load-generation limit, not a
loss of Mako workers. With matching generator threads, throughput scales to
524,700 GET/s. At 32 clients, all 32 request workers are active but the process
uses 19.64 cores, so the workers are not CPU-saturated.

This benchmark is only an in-memory Mako-backed GET-hit test. It does not
measure writes, mixed command workloads, large values, pipelining, vectors,
replication, persistence, cross-shard execution, or tail latency.

## Worker Scalability

The Rolis-style worker sweep is documented separately in
[`SCALABILITY.md`](SCALABILITY.md). It varies configured server workers instead
of holding the server at 32, measures GET/SET/80:20 mixed workloads, separates
server and client physical cores, and reports throughput, per-worker
throughput, speedup, efficiency, request/helper CPU, and p50/p95/p99 latency.
New runs also record load-generator process CPU so a client ceiling is visible.

The saturated two-client-per-worker run peaks at 24 workers: 873,175 GET/s,
836,640 SET/s, and 855,728 mixed operations/s. The one-client-per-worker run is
retained as a lower-load latency curve and is not presented as server capacity.

## Not Run Or Infrastructure-Dependent

These checks are not passes:

| Check | Status | Required setup or limitation |
|---|---:|---|
| G2 true cross-shard atomicity | N/A | Requires `MAKO_G2_MULTI_SHARD=1` and a real multi-shard fixture |
| G3 replicated failover durability | N/A | Requires start, kill, and recover hooks for a replicated Mako topology |
| Restart durability | N/A | Requires stop/start hooks; the current local Masstree Redis path is in-memory and is not a durability claim |
| Client failover | N/A | Requires `MAKO_FAILOVER_TARGETS` for a failover-capable deployment |
| memtier latency benchmark | N/A | `memtier_benchmark` is not installed at the configured path on this host |
| YCSB workloads | N/A | The YCSB 0.17 launcher and Redis binding are not installed at the configured path |
| External Elle analysis | N/A | No Elle JAR is present; the built-in G4 oracle passed |

The acceptance harness reports missing infrastructure with exit status 78 and
the label `N/A`. It must not be relabeled as `PASS`.

## Intentional Divergences

The authoritative list is `known_divergences.txt`. Current decisions include:

- `KEYS`, `SCAN`, and `DBSIZE` do not expose collection keys stored only as
  hidden set, list, hash, sorted-set, or stream composite keys.
- `HRANDFIELD` and `ZRANDMEMBER` return a consecutive window from a rotating
  offset rather than a random sample; `SPOP` and `SRANDMEMBER` are random.
- `makoConMultiTrd` remains ABI-compatible but is not the extended-command
  semantic target.
- Redis's sixteen logical databases are emulated with a hidden key prefix
  rather than separate Mako namespaces: `SELECT 0`..`SELECT 15` work, `FLUSHDB`
  clears the selected database and `FLUSHALL` all of them, and `SELECT` inside
  `MULTI` takes effect when it is queued rather than at `EXEC`.

These are design differences, not test passes. Any newly discovered in-scope
failure must be fixed or added to the divergence file with review.

## Known Limits

These are not design decisions; they are open problems.

- **Oversized transactions crash the server.** One command or one `MULTI`
  block whose Mako transaction needs more than 32,768 STO items segfaults
  `makoCon`. The boundary is exact and repeatable: `GEOADD` with 14,974 members
  in one command succeeds, and 14,975 crashes. A `MULTI` of about 5,000 `SET`s
  crashes, while a pipeline of 10,000 separate `SET`s is fine. gdb shows STO's
  item array (64 chunks of 512) overflowing at item 32,769; its bounds check is
  an assert that Release builds compile out. STO is untouched by this adapter.
  Fix options are an adapter-side size check that answers with an error, or a
  growable array or clean abort inside STO. Details are in
  `known_divergences.txt` ("Very large write transactions") and on PR 72.
- **Stubs.** `WAIT` answers 0, the replication section of `INFO` hard-codes the
  master role, `SAVE` and `SHUTDOWN` are refused, and `AUTH` accepts any
  password.

## Reproducing The Checks

Start reference Redis on port 6379 and a Mako-backed `makoCon` on the selected
port, then run the suites serially:

```bash
export MAKO_HOST=127.0.0.1
export MAKO_PORT=6380
export REDIS_HOST=127.0.0.1
export REDIS_PORT=6379
export PYTHON_BIN=/usr/bin/python3
export PYTHONPATH=third-party/redis/compat/_client_tmp/fakeredis-py/deps

cargo test --release --manifest-path third-party/redis/rust-lib/Cargo.toml

$PYTHON_BIN -m pytest third-party/redis/compat -q \
  --ignore=third-party/redis/compat/_client_tmp

bash third-party/redis/compat/run_client_tests.sh

TCL_COMPAT_FILE_TIMEOUT=120 \
  bash third-party/redis/compat/run_tcl_suite.sh

REDIS_COMPAT_PROBE_OUT=/tmp/redis_compat_probe.json \
  $PYTHON_BIN third-party/redis/compat/probe_commands.py

MAKO_G4_DURATION=30 \
MAKO_G4_HISTORY_OUT=/tmp/redis_compat_g4_history.json \
  $PYTHON_BIN third-party/redis/compat/run_elle_isolation.py

SOAK_SECONDS=10 bash third-party/redis/compat/run_soak.sh
FUZZ_CASES=80 bash third-party/redis/compat/run_fuzz.sh

MAKO_PID=$(pgrep -f '(^|/)makoCon$') \
  $PYTHON_BIN third-party/redis/compat/run_worker_cpu_benchmark.py
```

To run one Tcl file, set `TCL_COMPAT_FILES`, for example:

```bash
TCL_COMPAT_FILES=unit/type/list TCL_COMPAT_FILE_TIMEOUT=120 \
  bash third-party/redis/compat/run_tcl_suite.sh
```

`TCL_COMPAT_ONLY` passes Redis's exact test-name filter to the Tcl helper; it
does not select a file. Use `TCL_COMPAT_FILES` for file selection.

## Phase 2 and 3 Command Additions

Phases 2 and 3 of PR 72 add the following, all implemented in the adapter with
no changes below `makoCon`. Phase 2 is the bitmaps, keyspace, `DUMP`/`RESTORE`,
sharded Pub/Sub and observability rows; phase 3 is the rest. The package-by-package
plan and resulting command counts are in [`PHASE3_PLAN.md`](PHASE3_PLAN.md):

| Area | Commands |
|---|---|
| Bitmaps | `BITCOUNT` (BYTE/BIT ranges), `BITPOS`, `BITFIELD_RO` (GET), `BITFIELD` (GET/SET/INCRBY, `OVERFLOW WRAP/SAT/FAIL`, `#` offsets; the whole subcommand list runs as one atomic Mako op, and a call whose subcommands are all GET is dispatched to the read-only path), `BITOP` (AND/OR/XOR/NOT, atomic in one Mako transaction) |
| HyperLogLog | `PFADD`, `PFCOUNT` (single key and multi-key union), `PFMERGE` — dense sketch kept in a plain string value (`MHLL` header, 16384 one-byte registers), MurmurHash64A and the Ertl estimator from Redis `hyperloglog.c`, each command one atomic Mako op |
| Geo | `GEOADD` (`NX`/`XX`/`CH`), `GEOPOS`, `GEODIST`, `GEOHASH`, `GEOSEARCH` (`FROMMEMBER`/`FROMLONLAT`, `BYRADIUS`/`BYBOX`), `GEORADIUS`, `GEORADIUSBYMEMBER`, `GEORADIUS_RO`, `GEORADIUSBYMEMBER_RO`, `GEOSEARCHSTORE` (`STOREDIST`), `GEORADIUS`/`GEORADIUSBYMEMBER` `STORE`/`STOREDIST` — a geo key is a plain sorted set scored by Redis's 52-bit interleaved geohash, so `TYPE` is `zset` and every zset command still works on it. Built entirely from existing zset ops: GEOADD is one ZADD, GEOPOS/GEODIST/GEOHASH are one ZSCORE per member, and a search issues one by-score range read per geohash neighbor box in a single request, then filters by exact haversine distance (or Redis's rectangle test) in Rust. The storing forms run on the interactive session instead, because their write cannot be built until their own reads have answered: one open Mako transaction holds the `FROMMEMBER` center lookup, the neighbor-box reads, the destination delete and the ZADD of the matches, retried as a whole on an optimistic-concurrency abort exactly as a script is. The destination is replaced whatever type it held and deleted outright when nothing matched; without `STOREDIST` its score is the member's own geohash, so `GEOPOS`, `GEODIST` and `GEOHASH` all read it back, and with `STOREDIST` it is the distance from the center in the unit the search asked for. Redis's own `tests/unit/geo.tcl` is run as a non-gating check: 61 tests pass and the file then stops on its three fuzzy tests, which build one 20,000-member GEOADD, a single-transaction write set this executor cannot take (see `known_divergences.txt`, "Very large write transactions") — nothing geo-specific and unchanged by the storing forms |
| Hash field expiry | `HEXPIRE`, `HPEXPIRE`, `HEXPIREAT`, `HPEXPIREAT` (`NX`/`XX`/`GT`/`LT`, `FIELDS numfields ...`), `HTTL`, `HPTTL`, `HEXPIRETIME`, `HPEXPIRETIME`, `HPERSIST` — a field's expiration is a side key next to the field key in the hidden `\x01HX:` namespace holding the absolute Unix ms, so every existing hash command keeps its storage layout. Each command is one atomic Mako op replying with an array of per-field codes (`-2`/`0`/`1`/`2` for the HEXPIRE family, `-2`/`-1`/value for the rest). Expired fields are dropped on access by every hash read path — HGET, HMGET, HEXISTS, HSTRLEN, HGETALL, HKEYS, HVALS, HLEN, HSCAN, HRANDFIELD, and EXISTS/TYPE/DUMP — and the key disappears once its last field goes. HSET/HSETNX/HMSET discard a field's expiration when they overwrite the value, HINCRBY/HINCRBYFLOAT keep it, HDEL removes it with the field, RENAME and COPY carry it to the destination, and DEL/FLUSHDB/RESTORE/SORT STORE clear every one |
| Streams | `XADD` (`NOMKSTREAM`, `MAXLEN`/`MINID` with `=`/`~` and `LIMIT`, `*`, `<ms>-*` and explicit IDs), `XRANGE`, `XREVRANGE` (`-`/`+`, partial IDs, exclusive `(`, `COUNT`), `XLEN`, `XDEL`, `XTRIM`, `XSETID` (`ENTRIESADDED`, `MAXDELETEDID`), `XINFO STREAM [FULL [COUNT n]]`, `XINFO HELP`, `XREAD [COUNT n] [BLOCK ms] STREAMS key ... id ...` (`$`, `+` and explicit exclusive IDs, several streams in one call, blocking on the same parked-client path as `BLPOP`), `XGROUP CREATE/SETID/DESTROY/CREATECONSUMER/DELCONSUMER/HELP` (`MKSTREAM`, `ENTRIESREAD`), `XREADGROUP` (`>` and history replay, `COUNT`, `BLOCK`, `NOACK`), `XACK`, `XPENDING` (summary and extended, `IDLE`, exclusive ranges, one consumer), `XCLAIM` (`IDLE`/`TIME`/`RETRYCOUNT`/`FORCE`/`JUSTID`/`LASTID`), `XAUTOCLAIM` (`COUNT`, `JUSTID`, cursor, dropped IDs), `XINFO GROUPS`, `XINFO CONSUMERS` — a stream is five hidden composite-key families (meta, entries, groups, consumers, pending-entry lists) whose entry keys end in the 128-bit ID big-endian, so lexicographic order is ID order and an XRANGE is one storage key range with `COUNT` stopping the walk. A consumer group is one record per group, one per consumer and one per pending entry, and a per-consumer pending list is the group's filtered by owner, so `XACK`, `XCLAIM` and `XAUTOCLAIM` each write one record per entry. `entries-read` and `lag` follow Redis's own arithmetic, including reporting both as null while a tombstone sits between the group's position and the first entry. `TYPE` reports `stream`, and `EXISTS`, `DEL`, `EXPIRE`/`TTL`/`PERSIST`, `RENAME`, `COPY`, `MOVE`, `FLUSHDB`, `SORT ... STORE` and `DUMP`/`RESTORE` (a `MAKO_STREAM_DUMP` payload that carries the groups and their PELs) all handle it; `KEYS`/`SCAN` leave it out, exactly as they leave out every other collection. Every read-modify-write command is one atomic Mako op |
| Keyspace | `TOUCH`, `SORT_RO`, `SORT ... LIMIT offset count`, `OBJECT ENCODING/REFCOUNT/HELP`, approximate `MEMORY USAGE` |
| DUMP/RESTORE | string, set, sorted-set and stream payloads (`MAKO_STRING_DUMP`, `MAKO_SET_DUMP`, `MAKO_ZSET_DUMP`, `MAKO_STREAM_DUMP`), TTL and `ABSTTL` honored on RESTORE |
| Pub/Sub | `SPUBLISH`, `SSUBSCRIBE`, `SUNSUBSCRIBE`, `PUBSUB SHARDCHANNELS/SHARDNUMSUB` (process-local, like classic Pub/Sub) |
| Cluster | `CLUSTER INFO/MYID/SLOTS/SHARDS/NODES/KEYSLOT/COUNTKEYSINSLOT/GETKEYSINSLOT/HELP`, `READONLY`, `READWRITE`, `INFO cluster` — opt-in through `MAKO_REDIS_CLUSTER_MODE`. The default `off` answers all three commands with `ERR This instance has cluster support disabled` and reports `cluster_enabled:0`. `emulated` presents the single server as a one-node cluster owning slots 0-16383, the way Dragonfly's emulated mode does, so client libraries that will only speak to a cluster can build a slot map: `KEYSLOT` is Redis's CRC16 (XMODEM) of the hash tag mod 16384 with `keyHashSlot`'s `{...}` rules, and the advertised address comes from `MAKO_REDIS_ANNOUNCE_HOST`/`MAKO_REDIS_ANNOUNCE_PORT` falling back to `MAKO_HOST`/`MAKO_PORT`. Nothing is sharded: no `MOVED`/`ASK`, no slot migration, and `COUNTKEYSINSLOT`/`GETKEYSINSLOT` answer 0 and empty |
| Logical databases | `SELECT n` for 0..15, `MOVE key db`, `COPY ... DB n`, and per-database `KEYS`/`SCAN`/`DBSIZE`/`RANDOMKEY`/`FLUSHDB`/`INFO keyspace`, with `CONFIG GET databases` reporting 16 — database 0 keys are stored under exactly the bytes the client sends, so nothing about the existing keyspace moves, and a key in database 1..15 is stored under the hidden prefix `0x02 <db> ':'` applied to the Redis-visible name before any storage prefix, which is what makes every type, TTL, `WATCH`, lock stripe and `DUMP`/`RESTORE` mechanism work per database unchanged. The prefix is applied in one place, the parser's key validator, and stripped again in every reply that returns key names (`KEYS`, `SCAN`, `RANDOMKEY` and the key element of `BLPOP`/`BRPOP`/`BLMPOP`/`LMPOP`/`BZPOPMIN`/`BZPOPMAX`/`BZMPOP`/`ZMPOP`). `MOVE` is one atomic executor op that copies the whole object — string, set, list, hash, zset, TTL and hash-field TTLs — and deletes the source, answering 0 when the source is missing or the destination name is taken. `FLUSHDB` clears the selected database alone, `FLUSHALL` clears all of them, and Pub/Sub channels stay global as in Redis. A user key may not begin with `0x02`, for the same reason it may not begin with `0x01` |
| Monitoring | `MONITOR` — the client replies `OK` and then receives one status line per command any client runs on this process, in Redis's format `+<unix_seconds>.<microseconds> [<db> <ip>:<port>] "CMD" "arg"...`, with arguments quoted the way Redis's `sdscatrepr` quotes them (`\"`, `\\`, `\n`, `\r`, `\t`, `\a`, `\b`, and `\xHH` for every other non-printable byte) and AUTH/HELLO credentials redacted. Lines are delivered through the same weak-queue and worker-wake path as Pub/Sub, so a monitor parked on another worker thread is woken. `MULTI`, the commands queued after it and `EXEC` all appear, in that order; a monitor never sees its own commands but still gets their replies; `RESET`, `QUIT` and a disconnect all leave monitor mode; `INFO clients` reports `monitor_clients:<n>`. While any monitor is attached the raw GET/SET fast frame path falls through to the general parser so those two are reported too, gated by a single relaxed atomic load so nothing changes when no monitor is attached |
| Lua scripting | `EVAL`, `EVALSHA`, `EVAL_RO`, `EVALSHA_RO`, `SCRIPT LOAD`/`EXISTS`/`FLUSH`/`KILL`/`HELP` — real Lua 5.1 (mlua, vendored) with `KEYS`/`ARGV`, `redis.call`/`pcall`, `redis.error_reply`/`status_reply`/`sha1hex`/`log`/`setresp`, `redis.REDIS_VERSION` 7.4.0 and a small `cjson`. A script runs inside one interactive Mako transaction opened on its declared keys, so its `redis.call`s read its own writes, nothing it wrote is visible until it returns, and a script that fails or raises leaves the keyspace untouched. `redis.call` builds a RESP frame and goes through the ordinary command parser, so every command behaves and replies inside a script exactly as it does outside one, including the logical-database key prefix; replies convert to Lua by Redis's rules (integer to number, bulk to string, nil to false, status to `{ok=...}`, error raised or returned as `{err=...}`) and back (number truncated, false to nil, true to 1, array stopping at the first nil). A commit that loses an optimistic-concurrency race re-runs the whole script, which is why Redis's determinism requirement matters here too. `SCRIPT LOAD` returns the lowercase hex SHA1 into a process-global cache, `EVALSHA` of an unknown one answers `NOSCRIPT`, and a script running past `lua-time-limit` (also spelled `busy-reply-threshold`) makes other connections answer `BUSY` -- the script's busy hook keeps its own worker's connections answered, as Redis's does -- until it returns or `SCRIPT KILL` stops it — `UNKILLABLE` once it has written, `NOTBUSY` when nothing is running |
| Modules | `MODULE LIST`/`LOAD`/`LOADEX`/`UNLOAD`/`HELP` — recognized so a client library that probes for a module gets a usable answer instead of an unknown-command error. Nothing is emulated: there is no Redis module ABI below this adapter, so `LIST` is an empty array because no module is loaded, `LOAD` and `LOADEX` answer Redis's `ERR Error loading the extension. Please check the server logs.`, `UNLOAD` answers `ERR Error unloading module: no such module with that name`, and `HELP` is Redis's own help text. Queued inside `MULTI` like `CONFIG`. Redis's `tests/unit/moduleapi` stays out of the TCL scope |
| Observability shims | `SLOWLOG`, `LATENCY`, `ACL` (single implicit `default` user), `INFO keyspace` (`db0:keys=N`, cached 2 s per database; `MAKO_REDIS_INFO_ALL_DBS=1` also reports databases 1..15 that hold a key), `CONFIG GET` for `maxmemory-policy`, `timeout`, `maxclients`, `tcp-keepalive`, `hz`, `notify-keyspace-events`, `protected-mode`, `port` |

`test_phase2_commands.py HOST PORT [cluster]` exercises every addition against a
live server with a dependency-free RESP client. The optional third argument, or
`MAKO_REDIS_CLUSTER_MODE=emulated` in the environment, tells it to assert the
emulated cluster shapes rather than the disabled-mode errors, so the same script
covers a server started either way. Deliberate deviations are recorded in
`known_divergences.txt`.
