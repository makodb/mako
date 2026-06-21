# Mako Redis-Compatible Interface

This document describes Mako's Redis-compatible `makoCon` interface, centered on the Rust RESP layer, the Rust/C++ transaction FFI, and the C++ Masstree-backed execution path.

---

## Interface Summary

### Redis Command Layer

| Command | What it does | What it lacks | Why this implementation |
|---------|-------------|---------------|-------------------------|
| `GET key` | Reads one key and returns a Redis bulk string or nil | No type metadata; string-only at this phase | Reuses the existing `mbta_sharded_ordered_index::Get` path and returns raw Redis bytes after C++ strips Mako storage metadata |
| `SET key value [NX|XX] [GET] [EX|PX|EXAT|PXAT ttl] [KEEPTTL]` | Writes one string value with Redis string options | Expiry is lazy on Redis reads/deletes, not background cleanup | Keeps conditional writes and TTL metadata inside the C++ transaction executor |
| `GETSET key value`, `SETNX key value` | Compatibility wrappers over `SET` options | No TTL option on these wrappers | Reuses the same conditional SET FFI path |
| `MGET`, `MSET`, `MSETNX` | Multi-key string reads and writes | `MSETNX` is scoped to one FFI request | Expands to per-key operations while returning one Redis reply |
| `APPEND`, `STRLEN` | String append and length | String type only | Uses C++ read-modify-write inside one transaction with Rust-side bounded retry for growing values |
| `INCR`, `INCRBY`, `DECR`, `DECRBY`, `INCRBYFLOAT` | Counter updates | No integer overflow parity tests yet | Uses C++ read-modify-write inside one transaction with Rust-side bounded retry |
| `EXPIRE`, `PEXPIRE`, `EXPIREAT`, `PEXPIREAT`, `TTL`, `PTTL`, `PERSIST` | Adds, reads, or removes key expiry metadata for strings, sets, lists, zsets, and hashes | No background expiry scanner | Stores absolute Unix millisecond expiry under an internal TTL key and checks it inside C++ transactions |
| `KEYS pattern`, `SCAN cursor [MATCH p] [COUNT n] [TYPE string]`, `DBSIZE` | Enumerates user-visible string keys | Current `makoCon` single-shard path only; collection keys are hidden until typed keyspace scan is added | Uses the sharded Masstree scan path with an opaque numeric cursor and Rust-side glob filtering |
| `TYPE key` | Returns `string`, `set`, `list`, `zset`, `hash`, or `none` | No stream/module type surface | Uses the logical-key FFI path so expired keys are hidden |
| `SADD`, `SMEMBERS`, `SISMEMBER`, `SREM`, `SCARD`, `SMOVE`, `SPOP`, `SRANDMEMBER`, `SSCAN` | Basic Redis set operations | Random member selection is deterministic first-member selection in this phase | Stores members as internal composite keys and maintains cardinality metadata transactionally |
| `SINTER`, `SUNION`, `SDIFF`, `SINTERSTORE`, `SUNIONSTORE`, `SDIFFSTORE` | Set algebra and store variants | Implemented by scanning composite set members in the local transaction executor | Computes algebra in C++ and writes `*STORE` destinations inside the same OCC transaction |
| `LPUSH`, `RPUSH`, `LPOP`, `RPOP`, `LLEN`, `LINDEX`, `LRANGE` | Basic non-blocking Redis list operations | Blocking pops are intentionally out of scope | Stores elements as internal composite keys and maintains head/tail metadata transactionally |
| `LSET`, `LREM`, `LTRIM`, `LINSERT`, `LPUSHX`, `RPUSHX`, `LPOS`, `LMOVE`, `RPOPLPUSH` | Non-blocking list mutation and move operations | Blocking list commands are separate compatibility shims | Rewrites affected logical list contents inside the same OCC transaction; `LPOS` supports `RANK`, `COUNT`, and `MAXLEN` |
| `HSET`, `HSETNX`, `HMSET`, `HGET`, `HMGET`, `HGETALL`, `HDEL`, `HEXISTS`, `HLEN`, `HKEYS`, `HVALS`, `HSTRLEN`, `HINCRBY`, `HINCRBYFLOAT`, `HRANDFIELD`, `HSCAN` | Core Redis hash operations | Random field selection is deterministic first-field selection in this phase | Stores fields as internal composite keys and maintains hash cardinality metadata transactionally |
| `ZADD`, `ZSCORE`, `ZINCRBY`, `ZREM`, `ZCARD`, `ZRANGE`, `ZREVRANGE`, `ZRANGEBYSCORE`, `ZRANGEBYLEX`, `ZRANK`, `ZREVRANK`, `ZCOUNT`, `ZLEXCOUNT`, `ZREMRANGEBYSCORE`, `ZREMRANGEBYRANK`, `ZREMRANGEBYLEX`, `ZRANGESTORE`, `ZUNION`/`ZINTER`/`ZDIFF` and store variants, `ZPOPMIN`, `ZPOPMAX`, `ZMPOP`, `ZRANDMEMBER`, `ZSCAN` | Sorted-set coverage is scoped to the standalone `makoCon` Redis target | Stores member and score indexes transactionally and stages zset updates inside one `EXEC` |
| `SUBSCRIBE`, `UNSUBSCRIBE`, `PSUBSCRIBE`, `PUNSUBSCRIBE`, `PUBLISH`, `PUBSUB CHANNELS/NUMSUB/NUMPAT` | In-memory Redis Pub/Sub | Process-local only; no persistence or delivery after disconnect | Adds Rust-side subscriber mode, channel registry, pattern matching, and outbound queues without touching Mako storage |
| `DEL key [key ...]` | Deletes one or more keys and returns the count of keys that existed | No asynchronous deletion | Redis `UNLINK` aliases to this path because Phase 1 has no lazy-free subsystem |
| `UNLINK key [key ...]` | Alias of `DEL` | No async free semantics | Provides client compatibility without adding background deletion machinery |
| `EXISTS key [key ...]` | Checks one or more keys and returns the count present | No bloom-filter/cache shortcut; remote tables use `remoteGet()` and may copy the value | Uses a dedicated local no-copy existence path that participates in OCC read observation |
| `PING` | Connection smoke command; queues inside `MULTI` | No server metadata | Handled entirely in Rust |
| `HELLO [proto ...]` | Returns a parseable RESP handshake map | Minimal capability map only | Lets Redis clients complete connection setup |
| `CLIENT GETNAME/SETNAME/SETINFO/ID/NO-EVICT/REPLY/LIST` | Handles common client metadata calls | Minimal metadata only | Keeps client libraries from failing during handshake |
| `COMMAND`, `COMMAND DOCS`, `COMMAND COUNT`, `COMMAND INFO` | Returns parseable command metadata stubs | Minimal metadata only | Satisfies client/tooling probes without claiming full Redis metadata parity |
| `CONFIG GET/SET/RESETSTAT` | Handles common client configuration probes | Static compatibility values; no runtime server reconfiguration | Lets Redis clients and benchmarks complete setup probes |
| `RESET`, `QUIT`, `SELECT 0`, `AUTH`, `ECHO` | Handles common connection commands | `AUTH` is a no-op trust-boundary shim; only DB 0 is accepted | Matches the plan's connection-compatibility scope |
| `INFO [section]` | Returns parseable `server`, `clients`, `stats`, and `mako` sections | Small scoped metric surface only | Exposes the counters required by the plan without claiming full Redis INFO parity |
| `WAIT 0 0` | Returns `0` as a no-replication compatibility shim | No replica waiting | Lets clients that issue `WAIT` after writes continue |
| `MULTI` / `EXEC` / `DISCARD` | Queues commands and executes supported operations through one C++ transaction | No `WATCH`; no Lua | Mako's transaction model is the replacement for Redis WATCH/Lua-style optimistic wrappers |

### FFI Contract

| Field / opcode | What it does | What it lacks | Why this implementation |
|----------------|-------------|---------------|-------------------------|
| `TXN_OP_GET = 1` | Read operation | One key per FFI op | Keeps the C++ transaction executor simple |
| `TXN_OP_SET = 2` | Write operation | TTL expiry is lazy, not background cleanup | SET flags represent `NX`, `XX`, `GET`, `SETNX`, `MSETNX`, and TTL handling |
| `TXN_OP_DELETE = 3` / `TXN_OP_DEL` | Delete operation | Delete and unlink are not distinguished | `UNLINK` is intentionally an alias at this phase |
| `TXN_OP_EXISTS = 4` | Existence operation | No value bytes are returned | Existence needs presence, not value materialization |
| `TXN_OP_APPEND`, `TXN_OP_STRLEN`, `TXN_OP_INCRBY`, `TXN_OP_INCRBYFLOAT` | Phase 3 string/counter operations | String-only command surface | Keeps read-modify-write logic in one C++ transaction |
| `TXN_OP_EXPIRE`, `TXN_OP_TTL`, `TXN_OP_PERSIST` | Phase 4 TTL-family operations | No background expiry scanner | Keeps expiry decisions and lazy deletion inside one C++ transaction |
| `TXN_OP_SCAN` | Phase 5 key enumeration and `DBSIZE` | Single local shard only | Reuses the sharded Masstree scan path and returns cursor-key batches to Rust |
| `TXN_OP_SADD` through `TXN_OP_SET_ALGEBRA` | Phase 6 set operations | Local composite-key set storage only | Keeps set member updates, cardinality metadata, and set-store writes in one transaction |
| `TXN_OP_TYPE` | Logical key type lookup | Current replies are string/set/list/zset/hash/none | Keeps logical type expiry checks in C++ |
| `TXN_OP_LPUSH` through `TXN_OP_LPOS` | Phase 7 list operations | Non-blocking list surface only | Keeps element writes, head/tail metadata, and list moves in one transaction |
| `TXN_OP_ZADD` through `TXN_OP_ZRANDMEMBER` | Sorted-set operations, including lex ranges, range removal, range-store, algebra, pop, random-member, and scan | Standalone `makoCon` is the semantic target | Keeps member index, score index, and zset metadata updates in one transaction; Rust owns scan cursor pagination |
| `TXN_OP_HSET` through `TXN_OP_HSCAN` | Core hash operations | Store/field-randomness is deterministic in this phase | Keeps field records and hash metadata updates in one transaction; Rust owns scan cursor pagination |
| `TxnOpResult::success` | Operation/backend success | Does not encode key presence | Separates backend failure from Redis nil / missing-key semantics |
| `TxnOpResult::value_present` | Key presence bit | No value bytes by itself | Required to distinguish missing keys from existing empty bulk strings |
| `TxnOpResult::data_ptr/data_len` | Returned value bytes for `GET` | Null for non-value operations | Keeps Redis bytes opaque to Rust while preserving empty-string correctness |
| `MakoMetrics` / `cpp_get_metrics` | Supplies `INFO mako` counters and uptime | Retry count is best-effort Redis-layer retry attempts | Keeps Redis INFO formatting in Rust while reading executor counters from C++ |

---

## Rust RESP Layer

The Rust layer lives in `third-party/redis/rust-lib/src/lib.rs`.

It owns:

- TCP listener setup.
- RESP parsing.
- per-connection `MULTI` queueing.
- expansion of variadic Redis commands into per-key FFI operations.
- aggregation of per-key FFI results back into one Redis reply.

### Variadic Command Expansion

`DEL`, `UNLINK`, and `EXISTS` accept multiple keys. The Rust layer expands each Redis command into one `TxnOperation` per key, but records the span belonging to the original Redis command.

For example:

```redis
EXISTS a b c
```

becomes:

```text
TXN_OP_EXISTS(a)
TXN_OP_EXISTS(b)
TXN_OP_EXISTS(c)
```

Rust then counts the returned `value_present` bits and emits one Redis integer reply.

### Response Mapping

| FFI result | Redis response |
|------------|----------------|
| `GET success=true, value_present=true, data_len>0` | Bulk string with bytes |
| `GET success=true, value_present=true, data_len=0` | Empty bulk string |
| `GET success=true, value_present=false` | Nil bulk string |
| `SET success=true` | `+OK` |
| `DEL` / `UNLINK` | Integer count of keys that existed |
| `EXISTS` | Integer count of keys that exist |

---

## C++ Transaction Execution

The C++ Redis entry point is `third-party/redis/cpp/makoCon.cc`.

All supported data operations go through `execute_transaction()`. Single commands are sent as one-operation transactions. `MULTI` / `EXEC` sends the queued operations as one transaction.

`examples/makocon_ffi_impl.hh` holds the shared C/Rust FFI helpers used by both
Redis binaries:

- static layout checks for `TxnOperation`, `TxnRequest`, `TxnOpResult`, and
  `TxnResponse`;
- response allocation/freeing;
- `INFO mako` metric population;
- Redis-layer retry accounting.

`third-party/redis/cpp/makoConMultiTrd.cc` remains ABI-compatible only. Its queue worker
still implements only the legacy `GET`/`SET`/`DEL`/`EXISTS` storage verbs, so
the broader Redis command semantics are not claimed for that binary. If
MultiTrd becomes a correctness target, it needs to call the same transaction
executor as `makoCon.cc` instead of keeping a separate queue-side command
implementation.

### Thread Initialization

Redis worker threads must initialize both:

1. the current `SiloRuntime` binding, and
2. the underlying benchmark DB thread state.

The Redis server calls:

```cpp
SiloRuntime::Current()->BindToCurrentThread();
db->thread_init(false, 0);
```

Each Redis worker owns a nonblocking set of client sockets and processes ready
sockets on the same initialized Mako worker. `MAKO_REDIS_THREADS` can raise the
worker count up to the available local config limit.

This is required because `mako::DB::InitThread()` can no-op before leader configuration is installed, but the Redis path still needs Masstree/STO thread-local state.

### In-Batch Existence State

Within one `EXEC`, Redis replies must reflect command order. The C++ executor keeps a small per-batch `batch_exists` map so this sequence is answered correctly:

```redis
MULTI
EXISTS k
DEL k
EXISTS k
EXEC
```

Expected result:

```text
[1, 1, 0]
```

This state affects Redis-visible replies inside the current batch; persistent state still changes through Mako's normal transaction commit path.

---

## Optional Cache Mode

The default Redis path is not a cache. It executes supported commands through
Mako transactions. The current phase plan adds Redis command compatibility for
cache-style workloads, but it does not add an in-process Redis-layer cache.
Cache mode means cache-hit reads can return from Redis-layer memory before
calling Mako; cache misses still read Mako and may populate the cache.
The table below is a future design note only. If cache mode is added later,
Mako remains the source of truth and cache benchmarks must be reported
separately from the default transactional path.

Why cache mode is not in the current plan: Mako's claim is transactional,
distributed consistency. A Redis-layer cache would need a coherence mechanism
such as invalidation, version checks, or epoch checks so it does not return stale
hits after writes through another Mako path, shard movement, or failover. Without
that mechanism, cache mode could violate Mako's serializability and failover
claims.

| Cache scope | Possible implementation | Shortcoming | Mako claim at risk if wrong |
|-------------|-------------------------|-------------|-----------------------------|
| `GET key` read-through | Return cached value on hit; on miss read Mako and populate cache | Stale value after write | Strong consistency / serializable reads |
| `MGET key...` read-through | Check cache per key; fetch misses from Mako | Mixed old/new values across keys | Multi-key atomic behavior |
| `EXISTS key...` | Cache key presence or derive from cached value | Negative entries can become stale | Strong consistency |
| `STRLEN key` | Derive length from cached value | Wrong length after update | Redis compatibility correctness |
| Negative cache for missing keys | Cache nil/missing result | Later `SET` can make cached miss false | Strong consistency |
| `SET` / `GETSET` / `SETNX` write-through | Update cache only after Mako commit succeeds | Updating before commit exposes aborted writes | ACID / serializable transactions |
| `MSET` / `MSETNX` write-through | Update all touched keys after one committed request | Partial cache update can expose non-atomic state | Multi-key atomicity |
| `DEL` / `UNLINK` invalidation | Invalidate touched keys after commit | Early invalidation is safe but lowers hit rate; missed invalidation is stale | Strong consistency |
| `INCR*` cache update | Cache the committed numeric result | Retrying/aborting can publish wrong counter | ACID / Redis counter correctness |
| `APPEND` cache update | Cache the committed appended value | Large values increase cache memory and copy cost | High-performance claim |
| TTL-aware cache | Store cache expiry with Redis TTL metadata | Cache expiry before/after storage expiry can disagree | Redis TTL correctness |
| Cache inside `MULTI` / `EXEC` | Bypass cache for reads or update cache after committed `EXEC` | Using cache mid-transaction can miss queued writes | Multi-key atomicity / serializability |
| External Mako writers | Require versioning, invalidation feed, or disable cache | Redis cache cannot see writes outside Redis path | Strong consistency / failover |
| Distributed/sharded Mako | Shard-local cache with versioned invalidation | Failover or shard move can keep stale values | Geographic distribution / automatic failover |
| Per-worker local cache | Fast local hash map per Redis worker | Duplicate entries and cross-worker invalidation cost | Strong consistency / memory overhead |
| Shared cache | One process-wide cache with locking/sharding | Lock contention can erase performance gains | High-performance claim |
| Admission/eviction policy | Cache hot keys only; evict by size/TTL | Bad policy can add overhead without hit-rate gain | High-performance claim |

If a new cache phase is approved later, cache mode should start with read-through
`GET` / `MGET` outside transactions, plus invalidation or write-through for
every Redis write command that can change cached keys. It should not be used for
performance claims unless the benchmark reports hit rate, command mix, cache
size, and whether external Mako writers were present.

---

## Existence Path

`EXISTS` should not materialize values.

The intended local path is:

```text
makoCon.cc
-> mbta_sharded_ordered_index::Exists
-> mbta_sharded_ordered_index::exists
-> abstract_ordered_index::exists
-> mbta_ordered_index::exists
-> MassTrans::transExists
```

`MassTrans::transExists` performs:

1. unlocked Masstree lookup,
2. validity check,
3. stable version observation via `atomicObserve`,
4. transaction read observation,
5. not-found tracking through `ensureNotFound`.

It does not call `atomicRead()`, because `atomicRead()` copies the stored value. Existence commands only need the presence bit.

In multiversion mode, `transExists` uses `MultiVersionValue::mvExists`, which walks the version chain and checks delete markers without calling `mvGET()` or copying the selected value into a temporary string.

Remote tables currently use `remoteGet()`, so that path may copy the value. Phase 1 does not add a remote `EXISTS` RPC.

The default `abstract_ordered_index::exists()` still falls back to `get()` so other implementations remain source-compatible.

---

## Key Enumeration

Phase 5 adds user-visible string-key enumeration:

- `KEYS pattern` returns matching keys.
- `SCAN cursor [MATCH pattern] [COUNT n] [TYPE string]` returns `[cursor, keys]`.
- `DBSIZE` counts visible string keys.

The cursor is an opaque numeric token so Redis clients such as `redis-py` can
parse it. Cursor `0` starts and ends a scan. Rust maps nonzero cursor tokens to
the last scanned user key, owns glob filtering, and C++ owns the transactional
scan while skipping internal metadata keys such as `\x01TTL:<key>`.

`SCAN` is scoped to current `makoCon`'s single-shard configuration. A future
multi-shard Redis server must either add remote scan coordination or reject
`SCAN`; returning one local shard as if it were global would be silently
incomplete. That is a correctness limitation, not a Redis parser limitation.

Set, list, sorted-set, and hash keys use internal composite records outside the visible
`table_key_` namespace. That prevents leaking `\x01S:`, `\x01S#:`, `\x01L:`,
`\x01L#:`, `\x01Z:`, `\x01ZS:`, `\x01Z#:`, `\x01H:`, and `\x01H#:` records, but it means `KEYS`,
`SCAN`, and `DBSIZE` do not yet report collections as logical keys. Typed
keyspace enumeration should be added with a shared logical key index rather than
exposing composite storage details.

---

## Set Storage

Phase 6 adds Redis set commands with composite storage:

- Member key: `\x01S:<u64 set-len><set><member>` -> `"1"`.
- Cardinality key: `\x01S#:<u64 set-len><set>` -> decimal member count.
- Redis clients cannot create keys beginning with `0x01`.

Set writes update member keys and cardinality metadata in the same C++
transaction. `SINTERSTORE`, `SUNIONSTORE`, and `SDIFFSTORE` reconcile the
destination set in place, deleting stale members and inserting new members
without delete-then-reinsert for overlapping members.

`SSCAN key cursor [MATCH pattern] [COUNT n]` is implemented in Rust on top of
the transactional set-member read path. The cursor is an opaque numeric token
owned by the Rust handler, and internal set composite keys remain hidden.

The set-name length prefix avoids collisions between keys and members containing
separator bytes such as `:`.

TTL metadata is still keyed by the visible Redis key. When a set expires, the
logical expiry path deletes the bare string key, all set composite members, the
set cardinality key, and the TTL metadata inside the same transaction.

Current limitation: a single `EXEC` that creates a set member and then moves
that same newly-created member with `SMOVE` needs staged set writes. The case is
tracked as an expected compatibility gap.

---

## List Storage

Phase 7 adds non-blocking Redis list commands with composite storage:

- Element key: `\x01L:<u64 list-len><list><ordered-index>` -> element bytes.
- Metadata key: `\x01L#:<u64 list-len><list>` -> binary `{head, tail}` offsets.
- Redis clients cannot create keys beginning with `0x01`.

The list-name length prefix avoids collisions between list names and binary
element/index bytes. The ordered index uses a fixed-width order-preserving
encoding so left and right pushes can grow by moving head/tail offsets.

List commands use a transaction-local list overlay. Reads and writes inside one
`EXEC` observe earlier list updates, and dirty lists are flushed to composite
keys once before commit. This avoids delete-then-reinsert of the same internal
key inside one Mako transaction while preserving Redis command order.

The flush writes the affected logical list inside the same OCC transaction. This
favors semantic correctness over maintaining a second incremental mutation path
for reshaping commands in Phase 7.

TTL metadata is still keyed by the visible Redis key. When a list expires, the
logical expiry path deletes the bare string key, list elements, list metadata,
and TTL metadata inside the same transaction.

Blocking list commands (`BLPOP`, `BRPOP`, `BLMOVE`, and related variants) are
not implemented; they remain out of scope per the plan's killed-feature list.

---

## Sorted-Set Storage

Phase 8 adds core Redis sorted-set commands with composite storage:

- Member key: `\x01Z:<u64 zset-len><zset><member>` -> score text.
- Score key: `\x01ZS:<u64 zset-len><zset><encoded-score><member>` -> `"1"`.
- Cardinality key: `\x01Z#:<u64 zset-len><zset>` -> decimal member count.
- Redis clients cannot create keys beginning with `0x01`.

Scores are encoded for the score index with order-preserving IEEE-754 double
encoding: positives flip the sign bit, negatives flip all bits, then bytes are
stored big-endian. `NaN` is rejected.

Sorted-set commands use a transaction-local overlay. Reads inside one `EXEC`
observe earlier zset writes, and dirty zsets are flushed once before commit.
The flush updates changed member records in place and only deletes stale score
index records, avoiding delete-then-reinsert of the same member key.

TTL metadata is keyed by the visible Redis key. When a sorted set expires, the
logical expiry path deletes the bare string key, zset member records, score
index records, cardinality metadata, and TTL metadata inside the same
transaction.

`ZSCAN key cursor [MATCH pattern] [COUNT n]` is implemented in Rust on top of
the transactional zset member/score read path. The cursor is an opaque numeric
token owned by the Rust handler.

Current limitations:

- `KEYS`, `SCAN`, and `DBSIZE` still do not expose sorted sets as logical keys.

---

## Pub/Sub

Phase 9 adds Redis Pub/Sub in the Rust RESP handler only.

- `SUBSCRIBE channel ...` and `PSUBSCRIBE pattern ...` put the connection into
  subscriber mode.
- Subscriber-mode connections accept only subscribe, unsubscribe, `PING`,
  `QUIT`, and `RESET`.
- `PUBLISH channel message` enqueues RESP messages to matching subscriber
  connections.
- `PUBLISH` inside `MULTI` is queued and delivered when `EXEC` runs.
- `PUBSUB CHANNELS [pattern]`, `PUBSUB NUMSUB [channel ...]`, and
  `PUBSUB NUMPAT` read the in-memory registry.
- `INFO clients` and `INFO stats` expose `pubsub_channels` and
  `pubsub_patterns`.

Pub/Sub does not use Mako storage. Messages are not persisted, are not replayed
after disconnect, and are scoped to the current `makoCon` process. This matches
Redis Pub/Sub's ephemeral delivery model while keeping Mako's transaction
executor unchanged.

---

## Encoding Boundary

Mako values contain internal storage metadata. The Redis layer must not expose that metadata to clients.

The chosen boundary is:

- C++ owns `mako::Encode()` and decode/metadata stripping.
- Rust treats Redis keys and values as opaque byte arrays.
- FFI presence is carried separately via `value_present`.

This means an existing empty Redis value is not confused with a missing key:

```text
value_present=true, data_len=0   -> empty bulk string
value_present=false              -> nil
```

---

## Out Of Scope For Phase 1

Phase 1 does not implement:

- `MGET` / `MSET`
- `WATCH` / `UNWATCH`
- Lua scripting
- Redis Cluster
- Sentinel
- Streams
- Pub/Sub
- persistence through the Redis interface

Those are intentionally outside the foundational FFI step.

## TTL Expiry

`SET` TTL options and standalone TTL commands store expiry metadata in the
reserved internal `\x01TTL:<key>` namespace. The primary `makoCon` path enforces
expiry lazily on Redis reads, existence checks, deletes, TTL checks, and
read-modify-write commands. There is no background expiry scanner yet.

Supported Phase 4 commands:

- `EXPIRE key seconds [NX|XX] [GT|LT]`
- `PEXPIRE key milliseconds [NX|XX] [GT|LT]`
- `EXPIREAT key unix-seconds [NX|XX] [GT|LT]`
- `PEXPIREAT key unix-milliseconds [NX|XX] [GT|LT]`
- `TTL key`
- `PTTL key`
- `PERSIST key`

`TTL` and `PTTL` return Redis-compatible integer states:

- `-2` when the key does not exist or has expired during the check.
- `-1` when the key exists without expiry metadata.
- a positive remaining lifetime when expiry metadata exists.

`NX` is mutually exclusive with `XX`, `GT`, and `LT`. `GT` and `LT` are mutually
exclusive with each other. `XX` can combine with `GT` or `LT`, matching Redis.
