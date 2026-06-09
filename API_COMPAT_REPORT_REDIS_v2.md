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
| Redis compatibility pytest | 90 passed, 1 xfailed |
| `makoCon` and `makoConMultiTrd` build | passed with existing `libunwind` linker warning |
| Phase 11 `redis-benchmark SET` | 22,727.27 req/s, p50 0.415 ms |
| Phase 11 `redis-benchmark GET` | 30,303.03 req/s, p50 0.143 ms |
| Phase 11 memtier `SET` | 23,973.34 ops/sec, p50 0.407 ms, p99 0.919 ms |
| Phase 11 memtier `GET` | 34,314.73 ops/sec, p50 0.271 ms, p99 1.367 ms |

## Acceptance Status

Phase 12 acceptance is not yet a clean release gate.

Latest artifact:
`tools/redis_compat/acceptance/ACCEPTANCE_20260609_125648_5ebde060.txt`

| Acceptance line | Status | Detail |
|---|---|---|
| G1 wire compatibility | PASS | 90 passed, 1 xfailed |
| G2 bank transfer | N/A | missing `run_bank_transfer.py` |
| G3 failover durability | N/A | missing `run_failover_durability.py` |
| G4 Elle isolation | N/A | missing `run_elle_isolation.py` |
| Throughput guard | PASS | 33,333.34 req/s, p50 0.151 ms |
| Memtier p99 guard | PASS | 39,980.81 ops/sec, p99 0.231 ms |
| TCL semantic guard | N/A | missing `run_tcl_suite.sh` |
| INFO metrics guard | PASS | scoped metrics present |
| RESP fuzz guard | N/A | missing `run_fuzz.sh` |
| Soak guard | N/A | missing `run_soak.sh` |
| Restart durability guard | N/A | missing `run_restart_durability.py` |
| Client failover guard | N/A | missing `run_client_failover.py` |

The orchestrator records missing harnesses as `N/A` instead of converting them
into passes.
