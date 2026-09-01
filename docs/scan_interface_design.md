# Scan Interface Design: Local vs. Cross-Shard

**Status:** Decided — going with Option C (split interface)
**Context:** PR #60 (RocksDB-compatible interface), reviewer comment from shenweihai1

---

## Background

Mako shards data across multiple shard servers. Each shard server holds **one shard's data only**. Keys are assigned to shards via a hash function (FNV-1a in `mbta_sharded_ordered_index::hash_key`). Because of this hash-based distribution, the keyspace is **not globally ordered** across shards — adjacent keys (by lexicographic order) can land on different shards.

The current `ITable` interface exposes `Scan` and `ReverseScan`, which now correctly scan the **local shard only** by delegating to `mbta_sharded_ordered_index::scan()`. A globally ordered scan across all shard servers is not yet supported.

---

## The Problem with a Naive Global Scan

A globally correct `Scan(start_key, end_key, callback)` must visit all shards, because any shard may hold keys in the requested range. The straightforward approach — send an RPC to each shard server, collect results, merge in order — has several fundamental problems:

### 1. Every call is O(num_shards) RPCs

Even a small range scan must fan out to every shard, because the hash function makes it impossible to know ahead of time which shards hold keys in a given range. With N shards, every `Scan` call issues N RPCs before returning a single key.

### 2. Keys are not globally ordered across shards

Because sharding is hash-based, keys within one shard's ordered index are locally ordered, but two shards' ordered ranges do not interleave cleanly. A merge of N sorted streams produces a globally ordered result only if the entire dataset fits in memory (or if an external merge sort is used). For large tables this is impractical.

### 3. Stopping early is not safe

If the callback returns `false` (stop early) after visiting shard 0, we cannot skip shards 1…N-1 — they may hold keys that fall within the scanned range. Early termination is only safe when the key distribution is **globally ordered** (i.e., shard i holds all keys ≤ some boundary, shard i+1 holds all keys > that boundary). Hash-based sharding does not provide this.

### 4. The current `Scan` contract would be silently wrong

If `Scan` sent RPCs to all shards and stopped early on the first empty shard, it would return an incomplete result set with no error. This is a correctness bug that is hard for callers to detect.

---

## Design Options

### Option A: Local scan only (current implementation)

`Scan` and `ReverseScan` scan only the local shard. Cross-shard scans are not supported.

**Pros:**
- Correct and predictable — no hidden RPCs
- No assumptions about key distribution
- Zero additional latency beyond a normal local read

**Cons:**
- Unusable for applications that need to scan data spread across shards
- Callers must be aware that results are shard-local

**Current state:** This is what the code does today.

---

### Option B: Global scan with "globally ordered keys" assumption

Allow `Scan` to issue RPCs to all shards, but **require** that the key distribution is globally ordered (i.e., the application uses range-partitioning, not hash-partitioning). Under this assumption:

- Each shard holds a disjoint, contiguous key range
- A merge of N sorted streams is efficient and correct
- Early termination is safe once all shards report no more keys in range

**Pros:**
- Gives callers a single, familiar `Scan` call that works globally
- Early termination is safe and efficient

**Cons:**
- **Requires abandoning hash-based sharding** for tables that will be scanned
- Clients cannot use custom hash functions for key-to-shard assignment
- The system must enforce or document the globally-ordered constraint — violating it silently produces wrong results
- Still O(N) RPCs per call even with early termination; shard servers must each handle the scan sub-request

---

### Option C (chosen): Split interface — `Scan` (local) + `RemoteScan` (global)

Expose two separate methods on `ITable`:

```cpp
// Local shard only — fast, no RPCs
Status Scan(void* txn,
            const std::string& start_key,
            const std::string* end_key,
            std::function<bool(const std::string&, const std::string&)> callback);

// All shards — requires globally ordered key distribution assumption
Status RemoteScan(void* txn,
                  const std::string& start_key,
                  const std::string* end_key,
                  std::function<bool(const std::string&, const std::string&)> callback);
```

The application calls `Scan` when it knows keys are local, and `RemoteScan` when it needs a global range. The burden of choosing correctly falls on the caller.

---

## Option C Tradeoffs in Detail

### Correctness

**`Scan` (local):**
Always correct. `mbta_sharded_ordered_index::scan()` filters to the local shard by `getShardIndex()`, so results are exactly the keys this shard owns. No assumptions about global key distribution are needed.

**`RemoteScan` (global):**
Correct **only if the application uses globally ordered (range-partitioned) keys** — i.e., shard `i` owns all keys in a contiguous lexicographic range, and shard `i+1` owns the next range. With hash-based sharding (the current default), the results of `RemoteScan` would be incomplete and non-deterministic, since keys in the requested range are scattered across all shards with no ordering guarantee.

This constraint is invisible at the type level — both methods have the same signature. Callers must understand the partition scheme of the table they are scanning.

### Latency

**`Scan`:** One local index traversal. Latency is bounded by the number of keys in the result range on the local shard, not the cluster size.

**`RemoteScan`:** Fans out to all N shard servers in parallel. Total latency is dominated by the slowest shard response plus merge time. With N=8 shards and ~50 µs RPC latency each, even an empty `RemoteScan` costs ~50 µs of network round-trip (assuming fully parallel fan-out). A scan that returns many keys may also be limited by merge buffer memory.

### Throughput

**`Scan`:** Scales with local CPU and memory bandwidth. Multiple threads can call `Scan` concurrently with no cross-shard coordination.

**`RemoteScan`:** Each call consumes one RPC slot on every shard server simultaneously. Under high concurrency, `RemoteScan` calls can saturate the RPC layer across the entire cluster even if each individual shard is lightly loaded. Applications should rate-limit `RemoteScan` accordingly.

### Key Distribution Constraint

The globally-ordered-keys requirement for `RemoteScan` has downstream implications:

- **No custom hash functions for scanned tables.** Keys must be assigned to shards by range, not by hash. If a table is sharded by hash (the default), `RemoteScan` cannot be used on it.
- **Shard boundaries must be known and stable.** The client issuing `RemoteScan` must either know the shard boundary keys or ask the cluster coordinator. Rebalancing (splitting/merging shards) invalidates any cached boundaries.
- **Mixed-scheme clusters are possible but require per-table metadata.** A cluster can have some tables with hash sharding (use `Scan` only) and others with range sharding (may use `RemoteScan`), but the application must track which scheme each table uses. There is currently no mechanism in `ITable` or `IDatabase` to encode this.

### Early Termination

**`Scan`:** The callback can return `false` to stop. This is safe — there is only one shard to scan.

**`RemoteScan`:** The callback returning `false` mid-scan has ambiguous semantics:
- If results are delivered in globally sorted order (requires buffering all N shards' results first), early termination skips delivering later keys that were already fetched — wasting the RPC cost.
- If results are delivered as they arrive from each shard (unordered, streaming), early termination may leave some shard RPCs in flight. The implementation must cancel or drain them to avoid resource leaks.

In either case, early termination does **not** reduce RPC fan-out — all N shards are contacted regardless, because under hash sharding you cannot know in advance which shards have keys in range.

### Merge Strategy

`RemoteScan` must collect results from N shards and deliver them to the callback. There are two sub-options:

**Buffered merge (sorted delivery):**
Fetch all results from all shards, sort by key, then deliver to callback in order. Produces a globally sorted stream. Requires O(result_size) memory on the client and adds a full-scan-completion latency before the first key is delivered.

**Streaming (unordered delivery):**
Deliver each key to the callback as its shard response arrives. First key is delivered quickly, memory usage is O(in-flight keys). Results are **not** globally sorted — callers that assume sorted order will break. Suitable for aggregation (count, sum) but not for ordered iteration.

The right choice depends on the caller's use case. The API could expose this as a flag, or two separate calls (`RemoteScan` for sorted, `RemoteScanUnordered` for streaming).

### Transactional Semantics

**`Scan`:** Runs within the caller's local transaction. Consistent with other reads in the same transaction under Masstree OCC.

**`RemoteScan`:** Each shard executes its sub-scan in its own local transaction. There is **no distributed snapshot** — a write committed on shard 2 between shard 1's and shard 2's sub-scan will be visible in the `RemoteScan` result, while a write on shard 1 committed after shard 1's sub-scan completed will not. This means `RemoteScan` provides **per-shard snapshot isolation**, not cluster-wide snapshot isolation.

For read-only analytics workloads this may be acceptable. For workloads that require a consistent view of the entire keyspace at a point in time, a distributed snapshot protocol (e.g., a two-phase read with a global timestamp) would be needed — which is currently out of scope.

### Partial Failure

If one shard server is unavailable or times out during `RemoteScan`:

- **Fail-fast (recommended):** Return an error immediately. The caller knows the result is incomplete. Simple to implement and reason about.
- **Partial results:** Deliver results from available shards and annotate the response with which shards failed. Requires a richer return type than `Status`.
- **Retry with backoff:** Retry the failed shard sub-scan. Adds latency and complexity; still fails if the shard is down for longer than the retry window.

Fail-fast is the right default. Partial-result semantics can be added later if there is a concrete use case.

### API Placement: `ITable` vs `IDatabase`

Putting `RemoteScan` on `ITable` (as proposed) means the table object must have access to the cluster topology (addresses of all shard servers) to fan out the RPCs. This couples the table interface to cluster configuration, which is currently only known by `IDatabase`.

Putting it on `IDatabase` is architecturally cleaner — the database object already manages connections and knows the shard layout — but it requires passing a table name or ID as a parameter, which is less ergonomic.

A practical middle ground: keep the method on `ITable` but have `RemoteTable` (which already holds a `RemoteDB*` back-pointer) implement it by asking `RemoteDB` to issue the fan-out. `LocalTable` would return `NotSupported` since a server-side local table has no RPC client. This preserves the `ITable` interface while keeping transport logic in `RemoteDB`.

---

## Key Tradeoffs Summary

| | `Scan` (local) | `RemoteScan` (global) |
|---|---|---|
| Correctness | Always correct | Requires globally ordered (range-partitioned) keys |
| Latency | Local read latency | O(1) parallel RPC round-trips + merge |
| Throughput impact | Local only | Fan-out to all N shards per call |
| Custom hash functions | Allowed | Not compatible |
| Early termination | Safe, reduces work | Does not reduce RPC fan-out |
| Result ordering | Locally sorted | Sorted only with buffered merge |
| Transactional consistency | Per-transaction OCC | Per-shard snapshot (no global snapshot) |
| Partial failure | N/A | Fail-fast recommended |
| Implementation complexity | Done | High (`scanRemoteAll` RPC + merge layer) |

---

## Open Questions for Discussion with Shuai

1. **Should `RemoteScan` enforce the range-partition requirement, or leave it to the caller?**
   Without enforcement, a caller using hash-sharded keys will silently get wrong results. The interface could require a `partition_scheme` tag on each table, and return `InvalidArgument` if `RemoteScan` is called on a hash-sharded table.

2. **Sorted (buffered) or unordered (streaming) delivery for `RemoteScan`?**
   If callers need sorted output, the full result must be buffered before delivery — which adds latency and memory pressure. If unordered is acceptable for most use cases, streaming is much simpler to implement.

3. **Should `RemoteScan` live on `ITable` or `IDatabase`?**
   As described above, `IDatabase` is architecturally cleaner but less ergonomic. Placing it on `RemoteTable` backed by `RemoteDB` is a practical compromise.

4. **What partial-failure behavior is expected?**
   Fail-fast is the simplest default. Is there a use case that requires partial results?

5. **Is per-shard snapshot isolation acceptable, or is a global snapshot needed?**
   If the answer is "global snapshot required", `RemoteScan` depends on a distributed timestamp mechanism that does not currently exist in Mako.
