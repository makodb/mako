# Redis API Compatibility Report v2

This report summarizes the Redis-compatible `makoCon` surface implemented in
this PR. The semantic target is `examples/makoCon.cc`.
`examples/makoConMultiTrd.cc` is ABI/link-compatible only for extended Redis
commands.

## Supported Surface

- Strings: `GET`, `SET` with `NX`/`XX`/`GET`/TTL options, `GETSET`, `SETNX`,
  `MGET`, `MSET`, `MSETNX`, `APPEND`, `STRLEN`.
- Counters: `INCR`, `INCRBY`, `DECR`, `DECRBY`, `INCRBYFLOAT`.
- Key operations: `DEL`, `UNLINK`, `EXISTS`, `TYPE`, `KEYS`, `SCAN`, `DBSIZE`.
- TTL: `EXPIRE`, `PEXPIRE`, `EXPIREAT`, `PEXPIREAT`, `TTL`, `PTTL`, `PERSIST`.
- Sets: `SADD`, `SMEMBERS`, `SISMEMBER`, `SREM`, `SCARD`, `SMOVE`, `SPOP`,
  `SRANDMEMBER`, `SINTER`, `SUNION`, `SDIFF`, and store variants.
- Lists: non-blocking push/pop/range/mutation/move commands implemented in the
  Phase 7 scope.
- Sorted sets: core add/score/remove/cardinality/range/rank/count/pop/scan
  commands implemented in the Phase 8 scope.
- Pub/Sub: `SUBSCRIBE`, `UNSUBSCRIBE`, `PSUBSCRIBE`, `PUNSUBSCRIBE`,
  `PUBLISH`, `PUBSUB CHANNELS`, `PUBSUB NUMSUB`, `PUBSUB NUMPAT`.
- Connection/setup: `PING`, `HELLO`, `CLIENT`, `COMMAND`, `CONFIG`,
  `RESET`, `QUIT`, `SELECT 0`, `AUTH`, `ECHO`, `INFO`, `WAIT 0 0`.
- Transactions: `MULTI`, `EXEC`, `DISCARD` for supported commands.

## Scoped Metrics

`INFO` exposes a small compatibility surface:

- `connected_clients`
- `total_connections_received`
- `mako_txn_commits`
- `mako_txn_aborts`
- `mako_txn_retries`
- `mako_uptime_seconds`
- Pub/Sub channel/pattern counters from the Rust handler

This does not claim full Redis `INFO clients` or Redis persistence metrics.

## Known Divergences

The authoritative divergence list is
`tools/redis_compat/known_divergences.txt`. Current categories include:

- deterministic `SPOP`/`SRANDMEMBER` member choice;
- collection logical keys not yet visible through `KEYS`/`SCAN`/`DBSIZE`;
- `SADD` then `SMOVE` of the same newly-added member inside one `EXEC`;
- basic `LPOS` only, without `RANK`/`COUNT`/`MAXLEN`;
- deferred sorted-set store, lex, and range-removal variants;
- `makoConMultiTrd.cc` extended-command semantic gap.

## Operational Characterization

Latest local validation used `makoCon`, `MAKO_REDIS_THREADS=1`, one shard, and
no replication.

| Check | Result |
|---|---|
| Rust unit tests | 24 passed |
| Redis compatibility pytest | 90 passed |
| `makoCon` and `makoConMultiTrd` build | passed with existing `libunwind` linker warning |
| Phase 11 `redis-benchmark SET` | 22,727.27 req/s, p50 0.415 ms |
| Phase 11 `redis-benchmark GET` | 30,303.03 req/s, p50 0.143 ms |
| Phase 11 memtier `SET` | 23,973.34 ops/sec, p50 0.407 ms, p99 0.919 ms |
| Phase 11 memtier `GET` | 34,314.73 ops/sec, p50 0.271 ms, p99 1.367 ms |

## Acceptance Status

Phase 12 acceptance is not yet a clean release gate.

Latest artifact:
`tools/redis_compat/acceptance/ACCEPTANCE_20260609_134456_11fa4764.txt`

Latest G2-focused artifact:
`tools/redis_compat/acceptance/ACCEPTANCE_20260612_194706_8cacf577.txt`

| Acceptance line | Status | Detail |
|---|---|---|
| G1 wire compatibility | PASS | 90 passed |
| G2 bank transfer | PASS with local multi-shard fixture | `MAKO_G2_USE_LOCAL_FIXTURE=1` preserves the bank invariant over `MAKO_LOCAL_SHARDS=0,1,2` |
| G2 cross-shard demo | PASS when opted in | `G2_DEMO_START_REDIS_CLUSTER=1` captures Redis Cluster cross-slot rejection beside the Mako bank invariant |
| G3 failover durability | N/A | requires replicated Mako start/kill/recover hooks |
| G4 Elle isolation | N/A | requires Elle jar/history; built-in RMW smoke is opt-in |
| Throughput guard | PASS | 29,411.76 req/s, p50 0.311 ms |
| Memtier p99 guard | PASS | 28,104.10 ops/sec, p99 0.311 ms |
| TCL semantic guard | N/A | bootstrap hook added; Redis TCL tests are not vendored by default |
| INFO metrics guard | PASS | scoped metrics present |
| Soak guard | PASS | 1029 SET/GET ops, RSS/FD/thread sample recorded |
| Restart durability guard | N/A | restart hooks added; current Redis path is in-memory |
| Client failover guard | N/A | requires one or more failover targets |
| RESP fuzz guard | PASS | deterministic 80-frame fuzz run, delayed `PING` checks passed |

The orchestrator treats harness exit code `78` as `N/A`. That lets checked-in
harnesses distinguish missing environment from real failures.

## Fixture Hooks

The harnesses now include external fixture hooks:

- `tools/redis_compat/fixtures/makocon_local.sh` starts/stops one local
  `makoCon` for smoke and restart-hook testing.
- `tools/redis_compat/fixtures/makocon_multishard.sh` starts/stops one local
  Redis-facing `makoCon` with three local shard tables for the G2 bank
  transfer fixture.
- `tools/redis_compat/fixtures/redis_cluster.sh` starts a local Redis Cluster
  comparison fixture.
- `tools/redis_compat/run_cross_shard_demo.py` captures the Phase 12 G2
  side-by-side result in `docs/cross_shard_atomicity_demo.md`.
- `tools/redis_compat/bootstrap_redis_tests.sh` links or fetches Redis TCL
  tests when explicitly requested.
- `tools/redis_compat/fixtures/README.md` lists the environment variables for
  G2, G3, G4, TCL, restart durability, and client failover.

These hooks change the G2 local-fixture status only. A local single-shard
`makoCon` run remains a smoke test, and the local multi-shard process is still
not proof of replicated failover durability or Elle-checked serializability.

G2 follow-up checked: the checked-in `makoCon` env knobs can run a Redis-facing
process with multiple local shard tables. Remote shard RPCs still require
long-lived backing shard servers; the existing `bash/shard.sh` / `dbtest` path
is a benchmark runner and does not provide a stable Redis backing-shard fixture.

Validation note: the fuzz guard now uses deterministic bytes and delayed
liveness checks. During manual validation, a longer-tail post-fuzz server exit
was observed after the guard had passed. Treat this as a follow-up stability
item; the checked-in guard is reproducible but not a substitute for a longer
fuzz-plus-soak run.
