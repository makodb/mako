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
| `DEL key [key ...]` | Deletes one or more keys and returns the count of keys that existed | No asynchronous deletion | Redis `UNLINK` aliases to this path because Phase 1 has no lazy-free subsystem |
| `UNLINK key [key ...]` | Alias of `DEL` | No async free semantics | Provides client compatibility without adding background deletion machinery |
| `EXISTS key [key ...]` | Checks one or more keys and returns the count present | No bloom-filter/cache shortcut; remote tables use `remoteGet()` and may copy the value | Uses a dedicated local no-copy existence path that participates in OCC read observation |
| `PING` | Connection smoke command; queues inside `MULTI` | No server metadata | Handled entirely in Rust |
| `HELLO [proto ...]` | Returns a parseable RESP handshake map | Minimal capability map only | Lets Redis clients complete connection setup |
| `CLIENT GETNAME/SETNAME/SETINFO/ID/NO-EVICT/REPLY/LIST` | Handles common client metadata calls | Minimal metadata only | Keeps client libraries from failing during handshake |
| `COMMAND`, `COMMAND DOCS`, `COMMAND COUNT`, `COMMAND INFO` | Returns parseable command metadata stubs | Minimal metadata only | Satisfies client/tooling probes without claiming full Redis metadata parity |
| `CONFIG GET/SET/RESETSTAT` | Handles common client configuration probes | Static compatibility values; no runtime server reconfiguration | Lets Redis clients and benchmarks complete setup probes |
| `RESET`, `QUIT`, `SELECT 0`, `AUTH`, `ECHO` | Handles common connection commands | `AUTH` is a no-op trust-boundary shim; only DB 0 is accepted | Matches the plan's connection-compatibility scope |
| `INFO [section]` | Returns parseable `server`, `clients`, and `mako` sections | Small scoped metric surface only | Exposes the counters required by the plan without claiming full Redis INFO parity |
| `MULTI` / `EXEC` / `DISCARD` | Queues commands and executes supported operations through one C++ transaction | No `WATCH`; no Lua | Mako's transaction model is the replacement for Redis WATCH/Lua-style optimistic wrappers |

### FFI Contract

| Field / opcode | What it does | What it lacks | Why this implementation |
|----------------|-------------|---------------|-------------------------|
| `TXN_OP_GET = 1` | Read operation | One key per FFI op | Keeps the C++ transaction executor simple |
| `TXN_OP_SET = 2` | Write operation | TTL expiry is lazy, not background cleanup | SET flags represent `NX`, `XX`, `GET`, `SETNX`, `MSETNX`, and TTL handling |
| `TXN_OP_DELETE = 3` / `TXN_OP_DEL` | Delete operation | Delete and unlink are not distinguished | `UNLINK` is intentionally an alias at this phase |
| `TXN_OP_EXISTS = 4` | Existence operation | No value bytes are returned | Existence needs presence, not value materialization |
| `TXN_OP_APPEND`, `TXN_OP_STRLEN`, `TXN_OP_INCRBY`, `TXN_OP_INCRBYFLOAT` | Phase 3 string/counter operations | String-only command surface | Keeps read-modify-write logic in one C++ transaction |
| `TxnOpResult::success` | Operation/backend success | Does not encode key presence | Separates backend failure from Redis nil / missing-key semantics |
| `TxnOpResult::value_present` | Key presence bit | No value bytes by itself | Required to distinguish missing keys from existing empty bulk strings |
| `TxnOpResult::data_ptr/data_len` | Returned value bytes for `GET` | Null for non-value operations | Keeps Redis bytes opaque to Rust while preserving empty-string correctness |
| `MakoMetrics` / `cpp_get_metrics` | Supplies `INFO mako` counters and uptime | Retry count is best-effort Redis-layer retry attempts | Keeps Redis INFO formatting in Rust while reading executor counters from C++ |

---

## Rust RESP Layer

The Rust layer lives in `third-party/makocon/mako/rust-lib/src/lib.rs`.

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

The C++ Redis entry point is `examples/makoCon.cc`.

All supported data operations go through `execute_transaction()`. Single commands are sent as one-operation transactions. `MULTI` / `EXEC` sends the queued operations as one transaction.

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

`SET` TTL options store expiry metadata in the reserved internal
`\x01TTL:<key>` namespace. The primary `makoCon` path enforces expiry lazily on
Redis reads, existence checks, deletes, and read-modify-write commands. There is
no background expiry scanner yet.
