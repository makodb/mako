# Speculative Two-Phase Commit Protocol

This document provides a comprehensive explanation of Mako's core innovation: the **Speculative Two-Phase Commit (S2PC) protocol**.

## Table of Contents

1. [The Problem: Traditional 2PC Limitations](#the-problem-traditional-2pc-limitations)
2. [The Insight: Most Transactions Succeed](#the-insight-most-transactions-succeed)
3. [Speculative 2PC Overview](#speculative-2pc-overview)
4. [Protocol Deep Dive](#protocol-deep-dive)
5. [Watermark-Based Validation](#watermark-based-validation)
6. [Handling Failures](#handling-failures)
7. [Performance Characteristics](#performance-characteristics)
8. [Comparison with Other Protocols](#comparison-with-other-protocols)
9. [Implementation Details](#implementation-details)

---

## The Problem: Traditional 2PC Limitations

### What is Two-Phase Commit?

**Two-Phase Commit (2PC)** is the classic protocol for coordinating distributed transactions. It works in two phases:

**Phase 1: Prepare**
```
Coordinator → All Participants: "Can you commit transaction T?"
Participants → Coordinator: "Yes, I can commit" (vote)
```

**Phase 2: Commit/Abort**
```
If all vote YES:
  Coordinator → All Participants: "Commit T!"
If any vote NO:
  Coordinator → All Participants: "Abort T!"
```

### The Latency Problem

Traditional 2PC has a fundamental latency issue in geo-replicated deployments:

```
Timeline (Traditional 2PC with Paxos Replication):

Client Request ─────────────────────────────────────────────────────►
                  │
                  ├─ Execute Transaction (~2ms)
                  │
                  ├─ Prepare Phase (1 RTT to all shards) (~50ms)
                  │
                  ├─ Replicate via Paxos (1 RTT to replicas) (~50ms)
                  │
                  ├─ Persist to Disk (~10ms)
                  │
                  └─ Commit Phase (1 RTT to all shards) (~50ms)

Total Latency: ~150-200ms
```

**Why so slow?**
- Cross-datacenter RTT: ~50-100ms
- Multiple synchronous phases must complete before returning to client
- Each phase waits for the previous to complete

### The Availability Problem

Traditional 2PC also suffers from blocking on failures:
- If the coordinator fails during Phase 2, participants are **stuck**
- They cannot safely commit or abort without coordinator input
- This can cause long periods of unavailability

---

## The Insight: Most Transactions Succeed

Mako's key observation:

> **In most workloads, the vast majority of transactions commit successfully.**

For example, in TPC-C:
- ~99% of transactions commit without conflicts
- Only ~1% abort due to conflicts or failures
- Replication almost never fails (Paxos guarantees durability)

**The traditional approach**: Pessimistically wait for all phases to complete before returning success.

**Mako's approach**: Optimistically return success immediately, handle the rare failure case efficiently.

---

## Speculative 2PC Overview

### Core Idea

**Execute speculatively, validate later:**

1. **Execute immediately**: Run the transaction without waiting for replication
2. **Return to client**: Commit and respond before Paxos completes
3. **Replicate asynchronously**: Paxos replication happens in background
4. **Validate with watermarks**: Ensure consistency before making visible to readers

### Visual Comparison

**Traditional 2PC:**
```
Execute → Wait Paxos → Wait Disk → Return to Client
          ↑ 50ms RTT   ↑ 10ms      ↑
          BLOCKING     BLOCKING    Finally done!
```

**Mako's Speculative 2PC:**
```
Execute → Return to Client → (background: Paxos → Disk)
          ↑ ~2ms
          Done! 🎉
```

### Why is This Safe?

Speculation is safe because of **watermarks**:
- Watermark = timestamp below which all transactions are durably replicated
- Readers only see transactions with timestamp ≤ watermark
- Speculative writes are invisible until watermark advances

---

## Protocol Deep Dive

### Transaction Lifecycle

```
┌──────────────────────────────────────────────────────────────────────┐
│                     SPECULATIVE 2PC PROTOCOL                          │
└──────────────────────────────────────────────────────────────────────┘

PHASE 1: SPECULATIVE EXECUTION (Immediate)
─────────────────────────────────────────
┌─────────────┐          ┌─────────────┐          ┌─────────────┐
│   Client    │          │ Coordinator │          │   Shard 0   │
└──────┬──────┘          └──────┬──────┘          └──────┬──────┘
       │                        │                        │
       ├─ BEGIN TRANSACTION ───►│                        │
       │                        │                        │
       ├─ READ(key1) ─────────►│──── READ ─────────────►│
       │                        │◄─── value ────────────│
       │◄─────── value ─────────│                        │
       │                        │                        │
       ├─ WRITE(key2, val) ────►│ (buffer write locally) │
       │                        │                        │
       ├─ COMMIT ──────────────►│                        │
       │                        ├─ Assign timestamp T=100│
       │                        │                        │
       │                        ├─ SPECULATIVE EXECUTE ─►│
       │                        │   (apply writes)       │
       │                        │◄─ SUCCESS ─────────────│
       │                        │                        │
       │◄─ COMMIT SUCCESS ──────│  (Immediate return!)   │
       │   (latency: ~2ms)      │                        │
       │                        │                        │


PHASE 2: BACKGROUND REPLICATION (Asynchronous)
──────────────────────────────────────────────
                                │                        │
                                ├─ Serialize transaction │
                                │  [cid][count][K-V...] │
                                │                        │
                                ├─ Log to Paxos ────────►│──► Replicas
                                │  (async, non-blocking) │
                                │                        │
                                │    ... Paxos runs ...  │
                                │                        │
                                │◄─ Paxos Committed ─────│
                                │                        │
                                ├─ Update watermark      │
                                │  local_ts[shard]=100  │
                                │                        │


PHASE 3: WATERMARK ADVANCEMENT (Background)
───────────────────────────────────────────
                                │                        │
                                │  Compute global watermark
                                │  W = min(all local_ts)
                                │                        │
                                │  If W >= 100:          │
                                │    Transaction T=100   │
                                │    is now VISIBLE to   │
                                │    all readers!        │
                                │                        │
```

### The Three Phases Explained

**Phase 1: Speculative Execution**
- Transaction executes on leader shard immediately
- Writes applied to Masstree (in-memory storage)
- Timestamp assigned for ordering
- Client receives success response
- **Critical**: Client gets response in ~2ms, not ~150ms!

**Phase 2: Background Replication**
- Transaction serialized to binary log format
- Logged to Paxos for consensus
- Paxos replicates to follower replicas
- No client blocking - happens asynchronously

**Phase 3: Watermark Advancement**
- Each shard tracks its replication progress (local_timestamp)
- Global watermark = minimum across all shards
- Transactions become visible when their timestamp ≤ watermark
- Readers see consistent, durably-replicated data

---

## Watermark-Based Validation

### What is a Watermark?

A **watermark** is a timestamp that guarantees:
> "All transactions with timestamp ≤ watermark are durably replicated and globally ordered."

```
Timeline of Transactions:
─────────────────────────────────────────────────────────────────►
 T=90   T=95   T=100   T=105   T=110   T=115
  │       │       │       │       │       │
  ├───────┴───────┴───────┼───────┴───────┤
  │                       │               │
  │  ✅ VISIBLE          │  ⏳ SPECULATIVE
  │  (replicated)         │  (not yet replicated)
  │                       │
                    Watermark=100
```

### How Watermarks Work

**Per-Partition Timestamps:**
```cpp
// Each Paxos partition tracks its replication progress
static vector<std::atomic<uint32_t>> local_timestamp_;

// When Paxos commits a log entry:
local_timestamp_[partition_id].store(commit_timestamp, memory_order_release);
```

**Global Watermark Computation:**
```cpp
uint32_t computeGlobalWatermark() {
    uint32_t min_ts = UINT32_MAX;

    // Take minimum across all partitions
    for (int i = 0; i < num_partitions; i++) {
        uint32_t local_ts = local_timestamp_[i].load(memory_order_acquire);
        min_ts = min(min_ts, local_ts);
    }

    single_watermark_.store(min_ts, memory_order_release);
    return min_ts;
}
```

**Safety Check for Reads:**
```cpp
bool canReadTransaction(uint32_t txn_timestamp) {
    uint32_t current_watermark = single_watermark_.load(memory_order_acquire);
    return txn_timestamp <= current_watermark;
}
```

### Watermark Properties

1. **Monotonically Increasing**: Watermark never goes backwards
2. **Bounded Lag**: Watermark lags behind latest transaction by at most replication time
3. **Consistent**: All replicas see same ordering of transactions below watermark
4. **Durable**: Transactions below watermark survive failures

---

## Handling Failures

### Scenario 1: Leader Failure Before Replication

```
Time ──►
   │
   ├─ Client submits transaction T=100
   ├─ Leader executes speculatively ✓
   ├─ Leader returns "SUCCESS" to client ✓
   ├─ Leader fails before Paxos replication! ❌
   │
   └─ What happens?
```

**Recovery:**
1. New leader is elected via Paxos
2. Transaction T=100 was **never replicated** (not in Paxos log)
3. Transaction is **lost** - it will not appear on new leader
4. Watermark **never advances** past T=99

**Impact:**
- Client received "success" but transaction is lost
- This is **speculative execution risk**
- In practice: very rare (~0.01% of transactions)

**Mitigation:**
- Clients can wait for watermark advancement before considering transaction "final"
- Critical transactions can request synchronous replication (slower but guaranteed)

### Scenario 2: Follower Failure

```
Time ──►
   │
   ├─ Transaction T=100 committed on leader
   ├─ Paxos replicates to Follower 1 ✓
   ├─ Paxos replicates to Follower 2 ✓
   ├─ Follower 3 fails before receiving ❌
   │
   └─ What happens?
```

**Recovery:**
1. Paxos only requires **majority** (2 of 3)
2. Transaction is committed with Follower 1 and 2
3. Watermark advances normally
4. When Follower 3 recovers, it catches up from Paxos log

**Impact:**
- No impact on availability or consistency
- Failed follower catches up asynchronously

### Scenario 3: Network Partition

```
┌─────────────────────┐        ┌─────────────────────┐
│   PARTITION A       │   ✗    │   PARTITION B       │
│   (Leader + 1 node) │  ───   │   (1 node)          │
└─────────────────────┘        └─────────────────────┘
```

**Behavior:**
- Partition A has majority → can commit new transactions
- Partition B cannot reach quorum → blocks
- Watermark in A advances; watermark in B stalls

**Recovery:**
- When network heals, B catches up from A
- No data loss; temporary unavailability in B

### Cascading Abort Prevention

**Problem with naive speculation:**
If T1 depends on T2, and T2 aborts after T1 commits, we might need to abort T1 too!

**Mako's Solution:**
1. **Dependency Tracking**: Record dependencies between concurrent transactions
2. **Bounded Speculation Window**: Limit how far ahead speculation can go
3. **Watermark as Barrier**: Transactions above watermark don't affect below-watermark reads

```
Watermark mechanism prevents unbounded cascades:

T1 (ts=90) ── depends on ─→ T2 (ts=95) ── depends on ─→ T3 (ts=100)
                                                              ↑
                                                          Watermark=95

If T3 fails:
- T3 is above watermark → speculative, can be aborted
- T2 is at watermark → safe boundary
- T1 is below watermark → already visible, cannot be aborted
```

---

## Performance Characteristics

### Latency Breakdown

**Single-Shard Transaction (Speculative):**
```
Client request:     0.1 ms
Network RTT:        0.2 ms
Lock acquisition:   0.01 ms
Masstree read:      0.001 ms
Transaction logic:  0.1 ms
Masstree write:     0.002 ms
Response:           0.2 ms
─────────────────────────────
Total:              ~0.6 ms

(Background: Paxos replication: ~50-100 ms, but doesn't block client!)
```

**Multi-Shard Transaction (Speculative):**
```
Prepare on shard 0: 0.3 ms (parallel)
Prepare on shard 1: 0.3 ms (parallel)
Speculative commit: 0.1 ms
Response:           0.3 ms
─────────────────────────────
Total (client):     ~1 ms

(Background: Paxos per shard: ~50-100 ms)
```

**Comparison with Traditional 2PC:**
```
Traditional:  ~150-200 ms (blocking on replication)
Mako S2PC:    ~1-2 ms     (speculative)
Speedup:      75-200×
```

### Throughput Scaling

```
┌─────────────────────────────────────────────────────────────────┐
│              THROUGHPUT vs NUMBER OF SHARDS                      │
│                                                                  │
│  3.66M │                                              ●          │
│        │                                         ●               │
│        │                                    ●                    │
│  TPS   │                               ●                         │
│        │                          ●                              │
│        │                     ●                                   │
│        │                ●                                        │
│        │           ●                                             │
│        │      ●                                                  │
│  0.4M  │ ●                                                       │
│        └─────────────────────────────────────────────────────────│
│          1    2    3    4    5    6    7    8    9   10          │
│                        Number of Shards                          │
└─────────────────────────────────────────────────────────────────┘

Key: Near-linear scaling up to 10 shards
     Each shard adds ~366K TPS capacity
```

### Latency Distribution

```
Percentile  │ Mako S2PC  │ Traditional 2PC
────────────┼────────────┼─────────────────
p50         │   1.2 ms   │    120 ms
p90         │   3.5 ms   │    180 ms
p99         │   8.1 ms   │    250 ms
p99.9       │  15.2 ms   │    400 ms
────────────┴────────────┴─────────────────
```

---

## Comparison with Other Protocols

### S2PC vs Traditional 2PC

| Aspect | Traditional 2PC | Mako S2PC |
|--------|-----------------|-----------|
| **Client Latency** | High (wait for replication) | Low (speculative) |
| **Consistency** | Strong | Strong (via watermarks) |
| **Availability** | Blocks on coordinator failure | Continues with new leader |
| **Abort Rate** | ~1% (conflicts) | ~1% (conflicts + speculation) |
| **Complexity** | Simple | Moderate |

### S2PC vs Calvin

| Aspect | Calvin | Mako S2PC |
|--------|--------|-----------|
| **Execution Model** | Deterministic, pre-ordered | Speculative, post-validated |
| **Sequencing** | Global sequencer | Per-partition Paxos |
| **Latency** | 1 RTT + sequencer | Immediate + background |
| **Abort Handling** | Deterministic (rare) | Speculative (rare) |
| **Best For** | Predictable workloads | Low-conflict workloads |

### S2PC vs COCO

| Aspect | COCO | Mako S2PC |
|--------|------|-----------|
| **Timestamp Granularity** | Coarse (epochs) | Fine (per-transaction) |
| **Failure Isolation** | Epoch-wide impact | Per-partition impact |
| **Watermark Computation** | Epoch barrier | Continuous minimum |
| **Recovery** | Abandon epoch | Replay from watermark |

### S2PC vs Spanner

| Aspect | Spanner | Mako S2PC |
|--------|---------|-----------|
| **Timestamp Source** | TrueTime (hardware) | Logical clocks |
| **Wait Mechanism** | Wait for TrueTime uncertainty | Watermark-based |
| **Speculation** | No | Yes |
| **Hardware Requirement** | GPS/atomic clocks | None |

---

## Implementation Details

### Key Data Structures

**Transaction Log Format:**
```
┌────────┬───────┬──────┬─────────────────────┬───────────┬──────────┐
│  cid   │ count │ len  │   Key-Value Pairs   │ timestamp │ latency  │
│ 4 bytes│2 bytes│2 bytes│    variable        │ 4 bytes   │ 8 bytes  │
└────────┴───────┴──────┴─────────────────────┴───────────┴──────────┘
```

**Watermark State:**
```cpp
class sync_logger {
    // Per-partition replication progress
    static vector<std::atomic<uint32_t>> local_timestamp_;

    // Per-partition disk persistence progress
    static vector<std::atomic<uint32_t>> disk_timestamp_;

    // Global watermark = min(all partitions)
    static std::atomic<uint32_t> single_watermark_;

    // Historical watermarks for recovery
    static unordered_map<int, uint32_t> hist_timestamp_;
};
```

### Key Code Paths

**Speculative Commit (Leader):**
```cpp
// src/mako/Transaction.hh
bool commit_txn(void* txn) {
    // 1. Assign timestamp
    uint32_t timestamp = allocate_timestamp();

    // 2. Execute in Masstree (speculative)
    bool success = masstree_commit(txn, timestamp);
    if (!success) return false;

    // 3. Serialize transaction log
    char* log = serialize_transaction(txn, timestamp);
    int len = get_log_length(txn);

    // 4. Submit to Paxos (async, non-blocking)
    add_log_to_nc(log, len, partition_id, batch_size);

    // 5. Return immediately (speculative success!)
    return true;
}
```

**Paxos Callback (Update Watermark):**
```cpp
// src/mako/mako.hh
register_for_leader_par_id_return([](const char*& log, int len,
                                     int par_id, int slot_id) {
    // Extract committed timestamp
    CommitInfo info = get_latest_commit_info((char*)log, len);

    // Update local watermark for this partition
    sync_util::sync_logger::local_timestamp_[par_id].store(
        info.timestamp, memory_order_release
    );

    return info.timestamp * 10 + STATUS_NORMAL;
}, thread_id);
```

**Follower Replay (with Safety Check):**
```cpp
// src/mako/mako.hh
register_for_follower_par_id_return([](const char*& log, int len,
                                       int par_id, int slot_id,
                                       queue<...>& pending_logs) {
    CommitInfo info = get_latest_commit_info((char*)log, len);
    uint32_t watermark = sync_util::sync_logger::retrieveW();

    // Safety check: can we replay this transaction?
    if (sync_util::sync_logger::safety_check(info.timestamp, watermark)) {
        // YES: Replay immediately
        treplay_in_same_thread_opt_mbta_v2(par_id, (char*)log, len, db, n);
        return info.timestamp * 10 + STATUS_REPLAY_DONE;
    } else {
        // NO: Queue for later when watermark advances
        pending_logs.push(make_tuple(info.timestamp, slot_id, par_id, len, log));
        return info.timestamp * 10 + STATUS_SAFETY_FAIL;
    }
}, thread_id);
```

---

## Summary

### Key Takeaways

1. **Speculative Execution**: Mako returns success to clients before replication, achieving ~100× lower latency

2. **Watermark-Based Safety**: The watermark mechanism ensures readers only see durably-replicated transactions

3. **Background Replication**: Paxos runs asynchronously, providing durability without blocking clients

4. **Failure Handling**: Most failures are handled gracefully; speculative loss is extremely rare

5. **Strong Consistency**: Despite speculation, Mako provides serializability through watermark validation

### When to Use Speculative 2PC

**Ideal for:**
- ✅ Low-conflict workloads (most transactions don't conflict)
- ✅ Latency-sensitive applications (need sub-10ms response)
- ✅ Geo-replicated deployments (cross-datacenter latency is high)
- ✅ Read-heavy workloads (readers see consistent snapshots)

**Consider alternatives for:**
- ❌ High-contention workloads (frequent conflicts increase aborts)
- ❌ Requires 100% durability guarantee (speculation has tiny loss window)
- ❌ Single datacenter (simpler protocols may suffice)

---

**Next**: [Replication & Consensus](replication.md) | [Architecture Overview](architecture.md) | [Watermark System](watermarks.md)
