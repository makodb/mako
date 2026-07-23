# Redis Compatibility Validation

This directory contains the correctness, ecosystem-client, claim, robustness,
and performance harnesses for Mako's Redis-compatible interface. Redis is the
wire protocol and command API; the default `makoCon` target executes data
operations through Mako transactions and Masstree. Redis is not a second
database behind this interface.

The semantic target is `third-party/redis/cpp/makoCon.cc` with
`MAKO_REDIS_BACKEND=mako`, which is the default. The optional `memory` backend
and `makoConMultiTrd` are not correctness targets for the results below.

## Latest Validation Snapshot

The following results were collected on 2026-07-21 from branch
`redis-compat-phase3`, based on commit
`c28e39f2affee7c74ecb9747342c0811dd560053` plus the Redis compatibility
hardening documented in this snapshot. The server used 32 Redis request workers
on a 128-logical-CPU host. Tests were run serially against the same final
rebuilt binary on port 6396.

| Check | Result | What it establishes |
|---|---:|---|
| Rust unit tests | PASS, 38/38 | RESP parsing, reply formatting, command classification, worker wakeups, blocked-client ordering, and retry eligibility |
| Focused pytest suite | PASS, 105/105 | Redis and Mako agree on the claimed command behavior covered by local tests |
| Command-tier probe | PASS, P0 34/34, P1 48/48, P2 32/32 | All 114 declared probe cases completed successfully on Mako |
| Ecosystem client matrix | PASS, 8/8 rows | Common clients and tools can connect and exercise the scoped command surface |
| Redis 7.4 Tcl semantic guard | PASS, 11/11 scoped files | In-scope upstream Redis command semantics pass under the documented filters |
| G4 serializable RMW oracle | PASS, 91,615 operations | 16 concurrent clients updating 10 keys produced contiguous commits and matching final values |
| RESP fuzz guard | PASS, 80 cases | Random/malformed frames did not kill the server; immediate and delayed health checks passed |
| Soak guard | PASS, 577 serial SET/GET pairs in 10 seconds | Basic repeated operation and process-resource liveness; this is not a throughput benchmark |
| Worker CPU benchmark | PASS | Active request workers matched persistent client count and the server did not saturate its configured workers |

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
| Full acceptance orchestrator | `run_acceptance.sh` |
| Intentional incompatibilities | `known_divergences.txt` |

## Focused Pytest Coverage

The 105 collected cases are project-owned tests. Seven set cases are ports of
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
semantics without claiming Redis object encodings, debug internals, replication
behavior, or multiple logical databases.

Explicit skips are kept in `tcl_known_skips.txt`:

- Redis Streams and stream consumer-group behavior.
- Stream-specific blocking timeout behavior.
- Copying streams and stream consumer groups.
- Keyspace notification events generated by mutations.

Whole-file exclusions are recorded in `tcl_scope.txt`: Redis Cluster,
Sentinel, modules, RDB/AOF internals, Lua scripting, Streams, ACL, TLS, and
client-side caching. A skip is not counted as a pass for that excluded feature.

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
  hidden set, list, or sorted-set composite keys.
- `makoConMultiTrd` remains ABI-compatible but is not the extended-command
  semantic target.
- Mako Redis is a single-keyspace service. `SELECT 0` is accepted, while
  `FLUSHDB` and `FLUSHALL` clear that one keyspace.

These are design differences, not test passes. Any newly discovered in-scope
failure must be fixed or added to the divergence file with review.

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
