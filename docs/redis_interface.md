# Mako Redis-Compatible Interface

This document describes Mako's Redis-compatible `makoCon` interface, centered on the Rust RESP layer, the Rust/C++ transaction FFI, and the C++ Masstree-backed execution path.

---

## Interface Summary

### Redis Command Layer

| Command | What it does | What it lacks | Why this implementation |
|---------|-------------|---------------|-------------------------|
| `GET key` | Reads one key and returns a Redis bulk string or nil | No type metadata; string-only at this phase | Reuses the existing `mbta_sharded_ordered_index::Get` path and returns raw Redis bytes after C++ strips Mako storage metadata |
| `SET key value` | Writes one string value | No `NX`/`XX`/TTL options yet | Keeps value encoding in C++ so the Redis layer does not know Mako's internal value layout |
| `DEL key [key ...]` | Deletes one or more keys and returns the count of keys that existed | No asynchronous deletion | Redis `UNLINK` aliases to this path because Phase 1 has no lazy-free subsystem |
| `UNLINK key [key ...]` | Alias of `DEL` | No async free semantics | Provides client compatibility without adding background deletion machinery |
| `EXISTS key [key ...]` | Checks one or more keys and returns the count present | No bloom-filter/cache shortcut; remote tables use `remoteGet()` and may copy the value | Uses a dedicated local no-copy existence path that participates in OCC read observation |
| `PING` | Connection smoke command; queues inside `MULTI` | No server metadata | Handled entirely in Rust |
| `MULTI` / `EXEC` / `DISCARD` | Queues commands and executes supported operations through one C++ transaction | No `WATCH`; no Lua | Mako's transaction model is the replacement for Redis WATCH/Lua-style optimistic wrappers |

### FFI Contract

| Field / opcode | What it does | What it lacks | Why this implementation |
|----------------|-------------|---------------|-------------------------|
| `TXN_OP_GET = 1` | Read operation | One key per FFI op | Keeps the C++ transaction executor simple |
| `TXN_OP_SET = 2` | Write operation | SET options are not represented yet | Phase 1 only supports plain Redis `SET key value` |
| `TXN_OP_DELETE = 3` / `TXN_OP_DEL` | Delete operation | Delete and unlink are not distinguished | `UNLINK` is intentionally an alias at this phase |
| `TXN_OP_EXISTS = 4` | Existence operation | No value bytes are returned | Existence needs presence, not value materialization |
| `TxnOpResult::success` | Operation/backend success | Does not encode key presence | Separates backend failure from Redis nil / missing-key semantics |
| `TxnOpResult::value_present` | Key presence bit | No value bytes by itself | Required to distinguish missing keys from existing empty bulk strings |
| `TxnOpResult::data_ptr/data_len` | Returned value bytes for `GET` | Null for non-value operations | Keeps Redis bytes opaque to Rust while preserving empty-string correctness |
| `MakoMetrics` / `cpp_get_metrics` | Supplies `INFO mako` counters and uptime | `txn_retries` is 0 until the Redis path has a retry loop | Keeps Redis INFO formatting in Rust while reading executor counters from C++ |

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

- `SET NX` / `SET XX`
- TTL options
- `MGET` / `MSET`
- `WATCH` / `UNWATCH`
- Lua scripting
- Redis Cluster
- Sentinel
- Streams
- Pub/Sub
- persistence through the Redis interface

Those are intentionally outside the foundational FFI step.
