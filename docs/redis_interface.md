# Mako Redis-Compatible Interface

`makoCon` is a Redis-compatible server whose data lives in Mako. Clients speak
RESP2 or RESP3 to a Rust front end, which turns each command into operations
that a C++ executor runs inside Mako (STO/Masstree) transactions. Redis is the
wire protocol and command API only; no Redis server code or Redis runtime data
structure is involved.

As of 2026-09-24 the adapter recognizes 228 of the 251 top-level commands in the
Redis 7.4 command table. (The phase 3 plan counts 249 because it leaves out the
two internal HyperLogLog test commands, `PFDEBUG` and `PFSELFTEST`.) Four of the
228 are compatibility stubs, listed under [Known limits](#known-limits).

The semantic target is `third-party/redis/cpp/makoCon.cc` with
`MAKO_REDIS_BACKEND=mako`, the default. Related references:

| For | Read |
|---|---|
| Per-command behavior, validation results, how to run the suites | `third-party/redis/compat/README.md` |
| Every deliberate or known deviation from Redis | `third-party/redis/compat/known_divergences.txt` |
| Code map and conventions for adding a command | `third-party/redis/compat/PHASE3_CONTRIBUTING.md` |
| Worker-scaling and capacity measurements | `third-party/redis/compat/SCALABILITY.md` |

---

## Architecture

```text
Redis clients (RESP2/RESP3)
        |
Rust front end        third-party/redis/rust-lib/src/lib.rs, script.rs
  shared listener, N workers, RESP parsing, MULTI queue, WATCH,
  blocked clients, Pub/Sub and MONITOR registries, Lua interpreter
        |
C ABI                 third-party/redis/include/transaction_ffi.h
  batch call | raw GET/SET fast path | interactive session
        |
C++ executor          third-party/redis/cpp/makoCon.cc
  one Mako transaction per request, lock stripes, lazy TTL,
  deferred deletes and buffered writes
        |
Mako                  STO (OCC) over Masstree, optional Paxos replication
```

**Front end.** `rust_init(n)` starts one nonblocking listener shared by N worker
threads (`MAKO_REDIS_THREADS`). A connection stays on the worker that accepted
it. Each worker polls its sockets and a private wake socket, parses every
complete frame already buffered (so pipelining works), and runs commands
synchronously on its own Mako thread state. The listen address comes from
`MAKO_HOST` and `MAKO_PORT`, default `127.0.0.1:6380`.

**Three ways into the executor.**

| Entry point | Used for | Transaction shape |
|---|---|---|
| `cpp_execute_transaction` | Almost every command, and `MULTI`/`EXEC` | The whole op list is built in Rust first, then runs as one Mako transaction |
| `cpp_execute_fast_mako_string` | Plain `GET key` and unconditional `SET key value` | One-op transaction without the general parser; falls back to the batch call for anything else |
| `cpp_txn_begin` / `cpp_txn_execute` / `cpp_txn_commit` / `cpp_txn_abort` | Work whose later operations depend on earlier results: Lua scripts, `GEOSEARCHSTORE`, `GEORADIUS ... STORE`, and an `EXEC` whose queue contains one of the storing geo commands | One Mako transaction held open across several calls, on the keys declared at `begin` |

The batch call cannot feed one operation's result into the next, so any
read-then-write command is a single C++ op. The session calls exist for the
cases where that is impossible.

**Executor rules that shape every command.**

- **Locking.** Writes take stripe locks over 16,384 key stripes. Multi-key
  commands declare every source and destination key so all their stripes are
  held. `FLUSHDB`/`FLUSHALL` take the keyspace exclusively.
- **Optimistic retries.** An OCC abort is reported to Rust, which retries the
  command with a bounded budget. A script is re-run from the start.
- **Deferred deletes and buffered writes.** STO is built with
  `READ_MY_WRITES` off, so a record created inside a transaction cannot be
  removed and re-put, or written twice. The executor therefore buffers writes
  and defers deletes, then flushes both once before commit. This is what lets
  `SET k a; DEL k; SET k b` in one `EXEC`, `RENAME` onto an existing key, or a
  `*STORE` into its own source behave as Redis does.
- **Lazy TTL.** Expiry is checked on access inside the transaction. There is
  no background expiry scanner.
- **Replication.** With `MAKO_REPLICATION_ENABLED=1`, every committed write
  waits until its Paxos log batch is accepted by a majority before the reply is
  sent, and followers reject writes.

---

## Command surface

Detailed per-command behavior is in `third-party/redis/compat/README.md`. This
table is the map.

| Family | Commands | Notes |
|---|---|---|
| Connection and server | `PING`, `ECHO`, `HELLO`, `AUTH`, `CLIENT`, `COMMAND`, `CONFIG`, `INFO`, `RESET`, `QUIT`, `SELECT`, `TIME`, `WAIT`, `SAVE`, `SHUTDOWN` | `AUTH`, `WAIT`, `SAVE` and `SHUTDOWN` are stubs; see Known limits |
| Strings and counters | `GET`, `SET` (all options), `SETNX`, `SETEX`, `PSETEX`, `GETSET`, `GETDEL`, `GETEX`, `MGET`, `MSET`, `MSETNX`, `APPEND`, `STRLEN`, `GETRANGE`, `SETRANGE`, `SUBSTR`, `LCS`, `INCR`, `INCRBY`, `INCRBYFLOAT`, `DECR`, `DECRBY` | Read-modify-write runs in C++ inside the transaction |
| Bitmaps | `SETBIT`, `GETBIT`, `BITCOUNT`, `BITPOS`, `BITOP`, `BITFIELD`, `BITFIELD_RO` | `BITFIELD`'s whole subcommand list is one atomic op |
| HyperLogLog | `PFADD`, `PFCOUNT`, `PFMERGE` | Dense sketch stored as a string with an `MHLL` header; Redis's hash, register mapping and estimator |
| Keyspace and expiry | `DEL`, `UNLINK`, `EXISTS`, `TYPE`, `KEYS`, `SCAN`, `DBSIZE`, `RANDOMKEY`, `RENAME`, `RENAMENX`, `COPY`, `MOVE`, `TOUCH`, `DUMP`, `RESTORE`, `SORT`, `SORT_RO`, `OBJECT`, `MEMORY`, `EXPIRE`, `PEXPIRE`, `EXPIREAT`, `PEXPIREAT`, `EXPIRETIME`, `PEXPIRETIME`, `TTL`, `PTTL`, `PERSIST`, `FLUSHDB`, `FLUSHALL` | `KEYS`/`SCAN`/`DBSIZE` show string keys only; `DUMP` uses this adapter's own payload format |
| Logical databases | `SELECT 0`..`15`, `MOVE`, `COPY ... DB` | Emulated with a hidden key prefix; per-database `KEYS`/`SCAN`/`DBSIZE`/`FLUSHDB` |
| Transactions | `MULTI`, `EXEC`, `DISCARD`, `WATCH`, `UNWATCH` | The queue runs as one Mako transaction |
| Sets | `SADD`, `SREM`, `SISMEMBER`, `SMISMEMBER`, `SCARD`, `SMEMBERS`, `SPOP`, `SRANDMEMBER`, `SMOVE`, `SINTER`, `SINTERCARD`, `SINTERSTORE`, `SUNION`, `SUNIONSTORE`, `SDIFF`, `SDIFFSTORE`, `SSCAN` | `SPOP`/`SRANDMEMBER` sample with a per-thread random generator |
| Lists | `LPUSH`, `RPUSH`, `LPUSHX`, `RPUSHX`, `LPOP`, `RPOP`, `LLEN`, `LINDEX`, `LRANGE`, `LSET`, `LREM`, `LTRIM`, `LINSERT`, `LPOS`, `LMOVE`, `RPOPLPUSH`, `LMPOP`, `BLPOP`, `BRPOP`, `BRPOPLPUSH`, `BLMOVE`, `BLMPOP` | Blocking forms wake across workers |
| Hashes | `HSET`, `HSETNX`, `HMSET`, `HGET`, `HMGET`, `HGETALL`, `HDEL`, `HEXISTS`, `HLEN`, `HKEYS`, `HVALS`, `HSTRLEN`, `HINCRBY`, `HINCRBYFLOAT`, `HRANDFIELD`, `HSCAN` | |
| Hash field expiry | `HEXPIRE`, `HPEXPIRE`, `HEXPIREAT`, `HPEXPIREAT`, `HTTL`, `HPTTL`, `HEXPIRETIME`, `HPEXPIRETIME`, `HPERSIST` | Redis 7.4 semantics; expired fields are removed on access |
| Sorted sets | `ZADD`, `ZINCRBY`, `ZREM`, `ZCARD`, `ZCOUNT`, `ZLEXCOUNT`, `ZSCORE`, `ZMSCORE`, `ZRANK`, `ZREVRANK`, `ZRANGE`, `ZREVRANGE`, `ZRANGEBYSCORE`, `ZREVRANGEBYSCORE`, `ZRANGEBYLEX`, `ZREVRANGEBYLEX`, `ZRANGESTORE`, `ZREMRANGEBYSCORE`, `ZREMRANGEBYRANK`, `ZREMRANGEBYLEX`, `ZUNION`, `ZUNIONSTORE`, `ZINTER`, `ZINTERSTORE`, `ZINTERCARD`, `ZDIFF`, `ZDIFFSTORE`, `ZPOPMIN`, `ZPOPMAX`, `ZMPOP`, `ZRANDMEMBER`, `ZSCAN`, `BZPOPMIN`, `BZPOPMAX`, `BZMPOP` | |
| Geo | `GEOADD`, `GEOPOS`, `GEODIST`, `GEOHASH`, `GEOSEARCH`, `GEOSEARCHSTORE`, `GEORADIUS`, `GEORADIUSBYMEMBER`, `GEORADIUS_RO`, `GEORADIUSBYMEMBER_RO` | A geo key is a sorted set scored by Redis's 52-bit geohash; storing forms use the interactive session |
| Streams | `XADD`, `XRANGE`, `XREVRANGE`, `XLEN`, `XDEL`, `XTRIM`, `XSETID`, `XINFO`, `XREAD`, `XGROUP`, `XREADGROUP`, `XACK`, `XPENDING`, `XCLAIM`, `XAUTOCLAIM` | Consumer groups and blocking reads; `~` trimming trims exactly |
| Pub/Sub | `SUBSCRIBE`, `UNSUBSCRIBE`, `PSUBSCRIBE`, `PUNSUBSCRIBE`, `PUBLISH`, `PUBSUB`, `SPUBLISH`, `SSUBSCRIBE`, `SUNSUBSCRIBE` | Process-local; no keyspace notifications |
| Scripting | `EVAL`, `EVALSHA`, `EVAL_RO`, `EVALSHA_RO`, `SCRIPT` | Lua 5.1 (mlua, vendored); one Mako transaction per script run |
| Cluster emulation | `CLUSTER`, `READONLY`, `READWRITE` | Off by default; `MAKO_REDIS_CLUSTER_MODE=emulated` presents one node owning all slots |
| Monitoring and observability | `MONITOR`, `SLOWLOG`, `LATENCY`, `ACL` | `ACL` knows only the implicit `default` user |
| Modules | `MODULE` | Answers truthfully that no module is loaded; nothing can be loaded |

---

## Storage layout

Every Redis key maps to one or more Mako records. Collections never store their
members under the Redis-visible name, which is why a Redis key cannot begin with
byte `0x01`.

| Record | Storage key | Value |
|---|---|---|
| String | `table_key_` + key | the Mako-encoded value; a HyperLogLog is a string with an `MHLL` header and 16,384 one-byte registers |
| Key expiry | `\x01TTL:` + key | absolute expiry, Unix milliseconds |
| Set member / cardinality | `\x01S:` len key member / `\x01S#:` len key | `"1"` / member count |
| List element / bounds | `\x01L:` len key index / `\x01L#:` len key | element bytes / head and tail offsets |
| Hash field / count / field expiry | `\x01H:` len key field / `\x01H#:` len key / `\x01HX:` len key field | field value / field count / absolute expiry in ms |
| Sorted-set member / score index / cardinality | `\x01Z:` len key member / `\x01ZS:` len key score member / `\x01Z#:` len key | score / `"1"` / member count |
| Stream meta / entry | `\x01X#:` len key / `\x01X:` len key ms seq | length, last and first IDs, entries-added, max-deleted ID, group count / packed field-value list |
| Stream group / consumer / pending entry | `\x01XG:` len key group / `\x01XC:` len key group consumer / `\x01XP:` len key group ms seq | last-delivered ID, entries-read, counts / seen and active times, pending count / delivery time, delivery count, owner |

`len` is the eight-byte little-endian length of the logical key, so a key and a
member can never be confused. Sorted-set scores in the score index use an
order-preserving IEEE-754 encoding (positives flip the sign bit, negatives flip
all bits, big-endian), and `NaN` is rejected. Stream IDs are written as two
big-endian 64-bit halves, so key order is ID order and `XRANGE` is one key-range
read.

**Logical databases.** A key in database 1 to 15 is stored under
`0x02 <db byte> ':'` + key, applied to the Redis-visible name before any of the
prefixes above. Every mechanism (types, TTL, `WATCH`, lock stripes,
`DUMP`/`RESTORE`) therefore works per database unchanged, and database 0 keys
are stored exactly as the client sends them. A Redis key cannot begin with byte
`0x02` either. Replies that return key names strip the prefix again.

---

## Transactions, blocking and scripts

- **`MULTI`/`EXEC`.** Queued commands run as one Mako transaction on the batch
  path. Transaction-local overlays let later commands in the queue see earlier
  set, list and sorted-set changes, and a per-batch existence map keeps replies
  in command order, so `EXISTS k; DEL k; EXISTS k` answers `[1, 1, 0]`.
- **`WATCH`.** Key versions are tracked in Rust, lazily, so no bookkeeping
  happens until some client uses `WATCH`.
- **Blocking commands.** `BLPOP`, `BRPOP`, `BRPOPLPUSH`, `BLMOVE`, `BLMPOP`,
  `BZPOPMIN`, `BZPOPMAX`, `BZMPOP`, and `XREAD`/`XREADGROUP` with `BLOCK` run
  once; if nothing is ready the client is parked on its worker with its
  deadline. Any write that could satisfy it wakes the workers holding parked
  clients. Waiters on a key are served in arrival order within each command
  class, so a parked list pop never holds up a parked stream read. A blocking
  command inside `MULTI` does not block.
- **Scripts.** A script opens one interactive Mako transaction on its declared
  keys. Its `redis.call`s go through the ordinary parser, see the script's own
  writes, and stay invisible to others until the script returns. A script that
  errors leaves no writes behind. An OCC abort re-runs the whole script, so
  Redis's determinism rule matters here too. `SCRIPT LOAD` fills a
  process-global SHA1 cache. A script running past `lua-time-limit` makes other
  clients see `BUSY` until it finishes or `SCRIPT KILL` stops it.
- **Large keyspaces.** Standalone `FLUSHDB`, `FLUSHALL` and `DBSIZE` work in
  chunks of separate transactions, so they do not hit the transaction size
  limit described below.

---

## FFI contract

The ABI is plain C structs, pinned by compile-time layout checks in
`examples/makocon_ffi_impl.hh`.

| Type | Fields |
|---|---|
| `TxnOperation` | `op`, `key_ptr`/`key_len`, `val_ptr`/`val_len`, `flags`, `expire_at_ms`, `group_id` |
| `TxnRequest` | the operation array and its length |
| `TxnOpResult` | `success`, `value_present`, `data_ptr`/`data_len`, `int_value` |
| `TxnResponse` | the result array, freed by the C++ side |

`success` separates backend failure from Redis nil, and `value_present`
separates a missing key from an existing empty string. Options that do not fit
in `flags` travel in the value as a length-prefixed list (`pack_bytes_list`).
Operation numbers, grouped by family:

| Range | Family |
|---|---|
| 1-12 | strings, counters, expiry, key scan |
| 13-22 | sets and `TYPE` |
| 23-35 | lists |
| 36-45 | sorted sets and `FLUSHDB` |
| 46-58 | hashes |
| 59-67 | bits and ranges, blocking pop, `RENAME`, `SORT`, `DUMP`/`RESTORE` |
| 68-78 | extended sorted-set ops, `COPY`, `BITOP` |
| 79-86 | HyperLogLog, `BITFIELD`, hash field expiry, `MOVE` |
| 87-100 | streams |

`MakoMetrics` / `cpp_get_metrics` supply the `INFO mako` counters (commits,
aborts, retries, uptime and string-cache statistics).

---

## Encoding boundary

Mako values carry internal storage metadata that must never reach a client.

- C++ owns `mako::Encode()` and decoding or metadata stripping.
- Rust treats Redis keys and values as opaque bytes.
- Presence travels separately in `value_present`.

So an existing empty value is never confused with a missing key:

```text
value_present=true, data_len=0   -> empty bulk string
value_present=false              -> nil
```

---

## Existence path

`EXISTS` does not materialize values. The local path is:

```text
makoCon.cc
-> mbta_sharded_ordered_index::Exists
-> mbta_sharded_ordered_index::exists
-> abstract_ordered_index::exists
-> mbta_ordered_index::exists
-> MassTrans::transExists
```

`MassTrans::transExists` does an unlocked Masstree lookup, a validity check, a
stable version observation (`atomicObserve`), the transaction read observation,
and not-found tracking through `ensureNotFound`. It never calls `atomicRead()`,
which copies the value. In multiversion mode it uses
`MultiVersionValue::mvExists`, which walks the version chain without copying a
version. Remote tables still use `remoteGet()`, which may copy the value; there
is no remote `EXISTS` RPC. `abstract_ordered_index::exists()` falls back to
`get()` for other implementations.

---

## Optional backends and the string cache

**String cache (opt-in).** `MAKO_REDIS_CACHE_MB=<MiB>` enables a bounded
read-through cache for plain strings without TTL. A hit bypasses Mako, the fast
`SET` path refreshes an existing entry after the Mako commit, and every other
write invalidates the keys it touched after commit. Collections and TTL-bearing
values always go to Mako. Coherence covers only writes made through this
`makoCon` process, so leave the cache off if anything else writes the same Mako
keyspace. `INFO mako` reports hits, misses, insertions, evictions,
invalidations, entries and bytes.

**Memory backend.** `MAKO_REDIS_BACKEND=memory` keeps string and key state
inside the Rust server and bypasses Mako entirely for `GET`, `SET`, `MGET`,
`MSET`, `DEL`, `EXISTS`, `APPEND`, `STRLEN`, `INCR*`, simple TTL commands,
`TYPE` and `FLUSHDB`. Other commands return a backend error. It is a separate
cache-style backend for benchmarking, not a cache in front of Mako, and its
results must be reported separately.

**Why the cache stops at one process.** Mako's claim is serializable,
distributed consistency. A cache that is coherent across every writer would
need invalidation, version checks or epoch checks against writes arriving
through another path, a shard move or a failover. The table records what each
cache scope would put at risk:

| Cache scope | Possible implementation | Shortcoming | Mako claim at risk if wrong |
|---|---|---|---|
| `GET key` read-through | Return cached value on hit; on miss read Mako and populate | Stale value after write | Strong consistency / serializable reads |
| `MGET key...` read-through | Check cache per key; fetch misses from Mako | Mixed old/new values across keys | Multi-key atomic behavior |
| `EXISTS key...` | Cache key presence or derive from cached value | Negative entries can become stale | Strong consistency |
| Negative cache for missing keys | Cache nil results | A later `SET` makes the cached miss false | Strong consistency |
| `SET` / `GETSET` / `SETNX` write-through | Update cache only after the Mako commit | Updating before commit exposes aborted writes | ACID / serializable transactions |
| `MSET` / `MSETNX` write-through | Update all touched keys after one committed request | Partial update exposes non-atomic state | Multi-key atomicity |
| `DEL` / `UNLINK` invalidation | Invalidate touched keys after commit | A missed invalidation is stale | Strong consistency |
| TTL-aware cache | Store cache expiry with the Redis TTL | Cache and storage expiry can disagree | Redis TTL correctness |
| Cache inside `MULTI`/`EXEC` | Bypass for reads, update after committed `EXEC` | Mid-transaction use can miss queued writes | Serializability |
| External Mako writers | Versioning, an invalidation feed, or no cache | The Redis layer cannot see writes outside it | Strong consistency / failover |
| Distributed or sharded Mako | Shard-local cache with versioned invalidation | Failover or shard moves can keep stale values | Geo-distribution / automatic failover |

---

## Known limits

Items marked in `known_divergences.txt` carry their full reasoning there.

- **One oversized request crashes the server.** A single command or `MULTI`
  block whose transaction needs more than 32,768 STO items segfaults `makoCon`.
  The boundary is exact: `GEOADD` with 14,974 members in one command succeeds
  and 14,975 crashes, and a `MULTI` of about 5,000 `SET`s crashes. STO keeps a
  transaction's items in 64 chunks of 512; item 32,769 indexes past that array,
  and the only bounds check is an assert that Release builds compile out. STO is
  unchanged by this adapter, which is simply the first caller able to build
  transactions that large. The fix is either an adapter-side size check that
  answers with an error, or a growable item array or clean abort inside STO. The
  diagnosis is also recorded on PR 72.
- **Collections are invisible to `KEYS`, `SCAN` and `DBSIZE`.** They see string
  keys only (including HyperLogLogs). A proper fix is a shared logical-key index
  or a safe scan abstraction, not a scan over hidden composite prefixes.
- **Scans are local.** `SCAN` and `KEYS` cover the shards in this process;
  remote scan is not implemented.
- **`HRANDFIELD` and `ZRANDMEMBER` are not random.** They return a consecutive
  window starting at a rotating offset. `SPOP` and `SRANDMEMBER` do sample
  randomly.
- **Stubs.** `WAIT` always answers 0. The replication section of `INFO`
  hard-codes the master role. `SAVE` and `SHUTDOWN` are refused. `AUTH` accepts
  any password.
- **Process-local state.** Pub/Sub, sharded Pub/Sub, `MONITOR`, the script cache
  and blocked clients belong to one `makoCon` process.
- **No keyspace notifications.**
- **`EVAL` inside `MULTI`** runs when queued instead of at `EXEC`.
- **`KEYS`, `SCAN` and `DBSIZE` inside `MULTI`** do not see that transaction's
  own buffered writes.
- **`makoConMultiTrd`** is ABI-compatible only. Its session calls always fail,
  so scripts and the storing geo commands do not work there.

---

## Commands not implemented, and what blocks them

Twenty-three names in the Redis 7.4 command table are not recognized.

| Blocked by | Commands | What would have to change |
|---|---|---|
| Adapter work only | `FUNCTION`, `FCALL`, `FCALL_RO`, `SWAPDB`, `LOLWUT`, `ASKING`, `RESTORE-ASKING`, `MIGRATE` | `FUNCTION`/`FCALL` need a library registry on top of the Lua interpreter. `SWAPDB` rewrites two database prefixes in one transaction. `MIGRATE` can be a client connection that sends `RESTORE`, though only another Mako adapter can read the payload |
| Replication wrapper | `ROLE` | A read-only accessor for the leader flag, epoch and replica list, which also makes `WAIT` and `INFO replication` real |
| A consistent snapshot in storage | `BGSAVE`, `LASTSAVE` (and a real `SAVE`) | See below |
| No append-only file | `BGREWRITEAOF`, `WAITAOF` | There is no AOF; `WAITAOF` could at most alias `WAIT` |
| Consensus layer | `REPLICAOF`, `SLAVEOF`, `FAILOVER` | Paxos membership is static from configuration, there are no Paxos snapshots for bringing a new replica up to date, and leader election is incomplete. Redis also lets a server follow any other server, including a real Redis, which a consensus group cannot mean |
| Against the design | `PSYNC`, `SYNC`, `REPLCONF`, `SENTINEL` | Redis's own replication protocol and failover controller, both replaced by Paxos |
| Debug internals | `DEBUG`, `PFDEBUG`, `PFSELFTEST` | Not meaningful outside Redis's implementation |

**Why a snapshot is hard in Mako.** Transactions commit speculatively, before
replication completes. Each shard advances a watermark below which everything is
replicated, and the only consistent point across shards is the vector of those
watermarks. A snapshot therefore has to read the data as of that cut, which
needs older versions of every record written since. Mako keeps version chains
only so it can undo speculative writes, and reclaims them once they fall below
the watermark; a snapshot would have to hold reclamation back for its whole
duration, costing memory in proportion to write rate and snapshot time. All
shards would also have to agree on the cut. With replication off, version chains
are disabled altogether, so a snapshot becomes one read transaction over the
whole keyspace, which optimistic concurrency cannot hold. (Review comment by
Weihai Shen on PR 61: a snapshot needs a global vector watermark cut, slows
version reclamation, and needs coordination across shards.)

**Redis Stack modules.** JSON, Search, Bloom and Cuckoo filters, Top-K and
Count-Min sketch are not implemented. The probabilistic types and JSON are
adapter work of the same kind as HyperLogLog. Search is different: a query must
reach every shard's index and merge the answers, which needs a cross-shard scan
that Mako does not have yet.
