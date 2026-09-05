# Design Principles and Philosophy

This document articulates the core design principles that guide Mako's architecture and implementation. Understanding these principles helps users and contributors make informed decisions about how to use and extend the system.

## Table of Contents

1. [Core Philosophy](#core-philosophy)
2. [The Seven Principles](#the-seven-principles)
3. [Trade-offs and Choices](#trade-offs-and-choices)
4. [Anti-Patterns to Avoid](#anti-patterns-to-avoid)
5. [Evolution and Future Directions](#evolution-and-future-directions)

---

## Core Philosophy

### The Central Insight

> **Optimize for the common case, handle the rare case correctly.**

Mako is built on the observation that in real-world distributed database workloads:

- **99%+ of transactions commit successfully** without conflicts
- **Replication almost never fails** (Paxos guarantees durability)
- **Network partitions are rare** and typically brief
- **Hardware failures are infrequent** in modern datacenters

Traditional distributed databases treat every transaction as if it might fail, adding latency to **every operation** to handle rare failure cases. Mako inverts this:

1. **Assume success**: Execute optimistically, return quickly
2. **Verify in background**: Confirm durability asynchronously
3. **Handle failures gracefully**: When rare failures occur, recover correctly

This philosophy yields **100× lower latency** for the 99% of transactions that succeed, while correctly handling the 1% that don't.

### The Mako Equation

```
Traditional Latency = Execute + Wait(Replication) + Wait(Persistence)
                    = 2ms + 50ms + 10ms = 62ms

Mako Latency       = Execute + Return
                    = 2ms + 0.1ms = 2.1ms

Speedup            = 62ms / 2.1ms ≈ 30×
```

The background work still happens—it just doesn't block the user.

---

## The Seven Principles

### Principle 1: Speculation with Safety Nets

> **"Move fast, but always have a fallback."**

**The Idea:**
Execute transactions speculatively before confirmation, but maintain invariants that allow safe recovery if speculation fails.

**Implementation:**
- Transactions execute immediately on the leader
- Watermarks track what's been durably replicated
- Readers only see transactions below the watermark
- If a speculative transaction fails to replicate, it's invisible to readers

**Example:**
```cpp
// Speculative commit - returns immediately
bool success = commit_transaction(txn);
// Client sees success in ~2ms

// Background: Paxos replication
replicate_to_followers(txn);
// Takes ~50ms but doesn't block client

// Safety: Watermark prevents premature visibility
if (timestamp <= watermark) {
    make_visible_to_readers(txn);
}
```

**Why This Works:**
- Fast path is truly fast (no waiting)
- Safety is maintained by watermark invariant
- Rare failures are handled correctly

---

### Principle 2: Parallelism Over Sequentiality

> **"Never wait when you can parallelize."**

**The Idea:**
Identify dependencies and parallelize everything that doesn't have a true dependency.

**Implementation:**
- Multiple Paxos streams (one per partition) run in parallel
- Shards operate independently
- Replication and persistence happen concurrently
- Multiple transactions execute concurrently on each shard

**Architecture:**
```
┌─────────────────────────────────────────────────────────┐
│                   PARALLEL EXECUTION                     │
├─────────────────────────────────────────────────────────┤
│                                                          │
│  Shard 0        Shard 1        Shard 2        Shard 3   │
│  ┌─────┐        ┌─────┐        ┌─────┐        ┌─────┐   │
│  │ Txn │        │ Txn │        │ Txn │        │ Txn │   │
│  │ A   │        │ B   │        │ C   │        │ D   │   │
│  └──┬──┘        └──┬──┘        └──┬──┘        └──┬──┘   │
│     │              │              │              │       │
│     │ (parallel)   │ (parallel)   │ (parallel)   │       │
│     ▼              ▼              ▼              ▼       │
│  ┌─────┐        ┌─────┐        ┌─────┐        ┌─────┐   │
│  │Paxos│        │Paxos│        │Paxos│        │Paxos│   │
│  │  0  │        │  1  │        │  2  │        │  3  │   │
│  └─────┘        └─────┘        └─────┘        └─────┘   │
│                                                          │
│  All shards operate independently and in parallel!       │
└─────────────────────────────────────────────────────────┘
```

**Contrast with Sequential:**
```
Traditional: Txn A → wait → Txn B → wait → Txn C → wait → Txn D
Mako:        Txn A, B, C, D all execute concurrently!
```

---

### Principle 3: Shared-Nothing Architecture

> **"Eliminate coordination bottlenecks."**

**The Idea:**
Each shard should be as independent as possible. Cross-shard coordination should be minimized and localized.

**Implementation:**
- Each shard has its own Masstree storage
- Each shard has its own Paxos group
- Each shard has its own watermark tracking
- Only multi-shard transactions require coordination

**Benefits:**
```
Single-shard transaction:  Execute entirely locally, no coordination
Multi-shard transaction:   Coordinate only involved shards
Failed shard:              Other shards continue operating
Added shard:               Linear throughput increase
```

**Design Implication:**
Design your schema to maximize single-shard transactions:
```yaml
# GOOD: User data sharded by user_id
# Single-shard for most user operations
shard_key: user_id

# BAD: Random sharding
# Every transaction touches multiple shards
shard_key: hash(random)
```

---

### Principle 4: Memory-First Storage

> **"The fastest I/O is no I/O."**

**The Idea:**
Keep hot data in memory. Use disk only for durability, not for reads.

**Implementation:**
- **Masstree**: High-performance in-memory B+tree for all reads/writes
- **RocksDB**: Background persistence for durability
- **Paxos logs**: Replicated across replicas for fault tolerance

**Data Flow:**
```
READ PATH (fast):
Client → Masstree (in-memory) → Response
         No disk access!

WRITE PATH (speculative):
Client → Masstree → [async] → Paxos → [async] → RocksDB
         Fast!       Background      Background
```

**Performance Impact:**
```
Read latency:   ~1 μs (memory)
Write latency:  ~2 ms (speculative commit)
                vs ~60 ms (wait for disk)
```

---

### Principle 5: Cooperative Concurrency

> **"Coroutines, not threads."**

**The Idea:**
Use cooperative multitasking (coroutines) instead of preemptive multitasking (threads) for I/O-bound operations.

**Implementation:**
- SRPC framework provides lightweight coroutines
- Reactor pattern for event-driven I/O
- Thousands of concurrent operations per thread
- No thread synchronization needed within a reactor

**Why Coroutines?**
```
Threads:
- Heavy: ~1-8 MB stack per thread
- Slow context switch: ~1-10 μs
- Complex: Need locks for shared state
- Limited: ~10,000 threads practical limit

Coroutines:
- Light: ~4-64 KB stack per coroutine
- Fast context switch: ~10-100 ns
- Simple: No locks needed (cooperative)
- Scalable: 100,000+ coroutines easily
```

**Example:**
```cpp
// Multiple RPC calls in a single thread
reactor->CreateRunCoroutine([&]() {
    // This "blocks" but actually yields to other coroutines
    auto result1 = rpc_call_to_shard_0();
    auto result2 = rpc_call_to_shard_1();

    // Thousands of other coroutines run while we wait!

    process(result1, result2);
});
```

---

### Principle 6: Strong Types, Safe Memory

> **"Catch bugs at compile time, not runtime."**

**The Idea:**
Use the type system and smart pointers to prevent memory errors and logic bugs.

**Implementation:**
- RustyCpp smart pointers (`rusty::Box`, `rusty::Arc`, `rusty::Rc`)
- Borrow checking for memory safety
- Explicit lifetime annotations
- No raw pointer arithmetic in new code

**Smart Pointer Mapping:**
```cpp
// USE THIS               NOT THIS
rusty::Box<T>            std::unique_ptr<T>    // Single ownership
rusty::Arc<T>            std::shared_ptr<T>    // Thread-safe shared
rusty::Rc<T>             std::shared_ptr<T>    // Single-thread shared
rusty::Cell<T>           mutable field         // Interior mutability
```

**Safety Annotations:**
```cpp
// @safe - Pure function, no side effects
const char* status_to_string(Status s);

// @unsafe - Calls non-borrow-checked code
void call_legacy_function() {
    legacy_code();  // @unsafe
}
```

---

### Principle 7: Simplicity at the Interface

> **"Simple things should be simple; complex things should be possible."**

**The Idea:**
The common use case should be trivial. Advanced use cases should be well-documented.

**Example - Simple Case:**
```cpp
// Most users just need this
auto txn = db->begin_transaction();
txn->put("user:1001", "Alice");
txn->commit();
```

**Example - Advanced Case:**
```cpp
// Power users can tune behavior
auto txn = db->begin_transaction({
    .isolation = Isolation::SERIALIZABLE,
    .timeout_ms = 5000,
    .retry_policy = RetryPolicy::EXPONENTIAL_BACKOFF,
    .speculative = false  // Wait for replication
});
```

**Configuration Hierarchy:**
```yaml
# Sensible defaults - just works
site:
  server: [["s1:8100"]]
  client: [["c1"]]

# vs. Full control when needed
site:
  server:
    - ["dc1-s1:8100", "dc2-s1:8100", "dc3-s1:8100"]
    - ["dc1-s2:8100", "dc2-s2:8100", "dc3-s2:8100"]
  client: [["dc1-c1"], ["dc2-c1"]]

performance:
  threads_per_shard: 24
  batch_size: 100
  paxos_timeout_ms: 5000
```

---

## Trade-offs and Choices

Every design decision involves trade-offs. Here are the key ones Mako makes:

### Trade-off 1: Latency vs. Durability Guarantee

**Choice:** Speculative execution returns before replication completes.

**Pros:**
- ~30× lower latency
- Better user experience
- Higher throughput

**Cons:**
- Tiny window where committed transaction could be lost
- ~0.01% of transactions during leader failure

**Mitigation:**
- Watermarks ensure readers don't see unconfirmed data
- Critical applications can request synchronous commit

### Trade-off 2: Memory Usage vs. Disk I/O

**Choice:** Keep all working data in memory.

**Pros:**
- Sub-microsecond read latency
- No disk I/O on read path
- Predictable performance

**Cons:**
- Dataset must fit in memory
- Higher hardware cost

**Mitigation:**
- Tiered storage (hot in memory, cold on disk) is possible
- Memory is increasingly cheap

### Trade-off 3: Simplicity vs. Flexibility

**Choice:** Opinionated defaults with escape hatches.

**Pros:**
- Easy to get started
- Fewer configuration errors
- Consistent behavior

**Cons:**
- May not be optimal for every workload
- Some advanced features require code changes

**Mitigation:**
- Well-documented configuration options
- Extension points for customization

### Trade-off 4: Consistency vs. Availability

**Choice:** Strong consistency (serializability) by default.

**Pros:**
- Predictable semantics
- No anomalies
- Easier to reason about

**Cons:**
- Unavailable during minority partition
- Higher latency than eventual consistency

**Mitigation:**
- Speculative execution reduces latency impact
- Most applications need strong consistency anyway

---

## Anti-Patterns to Avoid

### Anti-Pattern 1: Hot Shard

**Problem:** All transactions touch the same shard (e.g., global counter).

**Symptoms:**
- One shard at 100% CPU, others idle
- Poor throughput scaling
- High contention, many aborts

**Solution:**
```cpp
// BAD: Global counter
txn->increment("global_counter");

// GOOD: Sharded counters
int shard = rand() % num_shards;
txn->increment("counter_shard_" + shard);
// Aggregate counters periodically
```

### Anti-Pattern 2: Cross-Shard Transaction Storm

**Problem:** Most transactions touch multiple shards.

**Symptoms:**
- High coordination overhead
- Latency spikes
- Reduced throughput

**Solution:**
```cpp
// BAD: User profile in shard 0, orders in shard 1
user_profile_shard = hash("user") % num_shards;
order_shard = hash("order") % num_shards;
// Every "user places order" touches 2 shards!

// GOOD: Co-locate related data
all_user_data_shard = hash(user_id) % num_shards;
// User profile AND orders in same shard
```

### Anti-Pattern 3: Ignoring Watermarks

**Problem:** Assuming speculative commits are immediately visible.

**Symptoms:**
- Read-after-write returns stale data
- Inconsistent results
- Confusing behavior

**Solution:**
```cpp
// BAD: Assume immediate visibility
txn1->put("key", "value");
txn1->commit();
// Immediately read in new transaction
txn2->get("key");  // Might not see "value"!

// GOOD: Wait for watermark or use same transaction
txn1->put("key", "value");
auto old_value = txn1->get("key");  // Same transaction sees write
txn1->commit();
// OR wait for watermark advancement
wait_for_watermark(txn1->timestamp());
txn2->get("key");  // Now guaranteed to see "value"
```

### Anti-Pattern 4: Long-Running Transactions

**Problem:** Holding locks for extended periods.

**Symptoms:**
- High abort rate
- Timeout errors
- Blocking other transactions

**Solution:**
```cpp
// BAD: Long computation inside transaction
txn->begin();
for (int i = 0; i < 10000; i++) {
    expensive_computation(txn->get("key" + i));
}
txn->commit();

// GOOD: Minimize transaction scope
auto data = txn1->batch_get(keys);
auto results = expensive_computation(data);  // Outside transaction
txn2->batch_put(results);
```

---

## Evolution and Future Directions

### Current Limitations

1. **In-Memory Only**: Dataset must fit in RAM
2. **Fixed Sharding**: Dynamic resharding not yet implemented
3. **Key-Value Model**: No complex queries or joins
4. **Single Leader per Shard**: Leader failure causes brief unavailability

### Planned Improvements

1. **Tiered Storage**: Automatic hot/cold data management
2. **Dynamic Sharding**: Add/remove shards without downtime
3. **Query Layer**: SQL-like interface on top of key-value
4. **Multi-Leader**: Reduce leader failure impact

### Research Directions

1. **Predictive Speculation**: Use ML to predict which transactions will conflict
2. **Adaptive Watermarks**: Dynamic watermark advancement based on workload
3. **Cross-Region Optimization**: Optimize for specific geo-topologies

---

## Summary

### The Mako Way

1. **Optimize for the common case** - Most transactions succeed; don't penalize them
2. **Parallelize aggressively** - Independent operations should run concurrently
3. **Isolate failures** - One component's failure shouldn't cascade
4. **Memory is king** - Disk is for durability, not reads
5. **Cooperate, don't preempt** - Coroutines over threads
6. **Type safety matters** - Catch bugs early
7. **Keep it simple** - Complexity is the enemy of reliability

### When Mako Shines

- ✅ Geo-replicated deployments
- ✅ Low-conflict, high-throughput workloads
- ✅ Latency-sensitive applications
- ✅ Strong consistency requirements

### When to Consider Alternatives

- ❌ High-conflict workloads (consider OCC or pessimistic locking)
- ❌ Complex queries (consider SQL databases)
- ❌ Datasets >> available memory (consider disk-based systems)
- ❌ Eventual consistency is sufficient (consider simpler systems)

---

**Next**: [Architecture Overview](architecture.md) | [Speculative 2PC](speculative-2pc.md) | [Configuration Reference](config.md)
