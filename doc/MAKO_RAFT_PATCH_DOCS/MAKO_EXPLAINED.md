# Mako: Speculative Distributed Transactions Explained

**Last Updated**: 2025-10-30
**Version**: OSDI'25 Submission
**Author**: Comprehensive analysis of the Mako codebase

---

## Table of Contents
1. [Executive Summary](#executive-summary)
2. [What is Mako?](#what-is-mako)
3. [Core Architecture](#core-architecture)
4. [How Paxos Powers Mako](#how-paxos-powers-mako)
5. [The Watermark System](#the-watermark-system)
6. [Transaction Lifecycle](#transaction-lifecycle)
7. [API and Interfaces](#api-and-interfaces)
8. [Failure Recovery](#failure-recovery)
9. [Performance Optimizations](#performance-optimizations)
10. [Comparison with Other Systems](#comparison-with-other-systems)

---

## Executive Summary

**Mako** is a high-performance, speculative distributed transaction system designed for geo-replicated deployments. Published in OSDI'25, Mako's key innovation is **watermark-based speculative execution** that allows transactions to execute immediately on leader replicas while ensuring strong consistency through delayed validation on follower replicas.

### Key Features
- **Speculative Execution**: Execute transactions before full replication completes
- **Strong Consistency**: Guarantee serializability despite speculation
- **Geo-Replication**: Built for multi-datacenter deployments
- **Paxos-based Replication**: Uses Multi-Paxos for fault-tolerant log replication
- **High Performance**: Masstree-based in-memory storage with optimized concurrency

### Performance Highlights
- **Low Latency**: Sub-millisecond transaction latency on leader
- **High Throughput**: Millions of transactions per second per shard
- **Efficient Replication**: Batched Paxos logging with minimal overhead

---

## What is Mako?

### The Problem Mako Solves

In geo-replicated distributed databases, there's a fundamental tension:
1. **Consistency** requires waiting for replication to complete
2. **Performance** demands low latency for user-facing transactions
3. **Availability** needs fault tolerance across datacenters

Traditional approaches pick two out of three. Mako achieves all three through **speculative execution with safety checks**.

### The Mako Solution

Mako's insight: **Most transactions don't conflict**, so we can:
1. **Execute speculatively** on the leader without waiting for replication
2. **Log to Paxos** asynchronously in the background
3. **Validate later** on followers using watermarks before making results visible
4. **Rollback only when necessary** (rare in practice)

This gives **low latency** (execute immediately) with **strong consistency** (validate before commit) and **high availability** (Paxos replication).

---

## Core Architecture

### System Components

```
┌─────────────────────────────────────────────────────────────┐
│                     Mako Architecture                       │
└─────────────────────────────────────────────────────────────┘

┌──────────────────┐              ┌──────────────────┐
│   Leader Shard   │              │  Follower Shard  │
│                  │              │                  │
│  ┌────────────┐  │              │  ┌────────────┐  │
│  │   Client   │  │              │  │   Client   │  │
│  │   Requests │  │              │  │   Requests │  │
│  └──────┬─────┘  │              │  └──────┬─────┘  │
│         │        │              │         │        │
│         ▼        │              │         ▼        │
│  ┌────────────┐  │              │  ┌────────────┐  │
│  │ Speculative│  │              │  │  Validation│  │
│  │ Execution  │  │              │  │   Layer    │  │
│  └──────┬─────┘  │              │  └──────┬─────┘  │
│         │        │              │         │        │
│         ▼        │              │         ▼        │
│  ┌────────────┐  │              │  ┌────────────┐  │
│  │  Masstree  │  │              │  │  Masstree  │  │
│  │   Storage  │  │              │  │   Storage  │  │
│  └──────┬─────┘  │              │  └──────┬─────┘  │
│         │        │              │         │        │
│         │ Log    │   Paxos Log  │         │ Replay │
│         ▼        │◄─────────────►│         ▼        │
│  ┌────────────┐  │              │  ┌────────────┐  │
│  │   Paxos    │  │              │  │   Paxos    │  │
│  │   Leader   │  │══════════════►│  │  Follower  │  │
│  └────────────┘  │   Replicate  │  └────────────┘  │
│                  │              │                  │
│  ┌────────────┐  │              │  ┌────────────┐  │
│  │ Watermark  │  │◄────Exchange──►│ Watermark  │  │
│  │  Manager   │  │              │  │  Manager   │  │
│  └────────────┘  │              │  └────────────┘  │
└──────────────────┘              └──────────────────┘
```

### Key Components Explained

#### 1. **abstract_db** - Database Interface
- Abstract interface for transactional storage
- Implemented by `mbta_wrapper` (Masstree-based transactional storage)
- Provides standard transaction operations: `begin_txn()`, `get()`, `put()`, `commit_txn()`

#### 2. **Masstree** - Storage Engine
- High-performance in-memory B+-tree optimized for cache locality
- Supports concurrent reads and writes
- Provides snapshot isolation for transactions

#### 3. **sync_logger** - Watermark Manager
- Tracks timestamps for speculative execution safety
- Maintains `local_timestamp_` per Paxos stream (partition)
- Computes `single_watermark_` across all partitions
- Provides `safety_check()` to validate transaction visibility

#### 4. **Paxos Integration**
- `PaxosServer`: Manages Multi-Paxos consensus
- `MultiPaxosCoordinator`: Coordinates Paxos rounds
- Logs transactions to replicas with slot-based ordering

---

## How Paxos Powers Mako

### The Paxos-Mako Connection

Mako uses **Multi-Paxos** not for transaction consensus, but for **replication log consensus**. Here's the detailed workflow:

### 1. Paxos Log Structure

```cpp
// From src/deptran/paxos/server.h
struct PaxosData {
  ballot_t max_ballot_seen_ = 0;
  ballot_t max_ballot_accepted_ = 0;
  shared_ptr<Marshallable> accepted_cmd_{nullptr};    // The transaction log
  shared_ptr<Marshallable> committed_cmd_{nullptr};   // Committed transaction
};

class PaxosServer : public TxLogServer {
  map<slotid_t, shared_ptr<PaxosData>> logs_{};  // Ordered log slots
  slotid_t min_active_slot_ = 0;
  slotid_t max_executed_slot_ = 0;
  slotid_t max_committed_slot_ = 0;
  //...
};
```

Each **Paxos slot** contains:
- **Slot ID**: Sequential ordering number
- **Ballot**: For leader election and conflict resolution
- **Transaction log**: Serialized transaction operations
- **Timestamp**: For watermark-based validation

### 2. Leader Side: Logging Transactions

When a transaction commits on the leader:

```cpp
// From src/mako/mako.hh - register_paxos_leader_callback
register_for_leader_par_id_return([&,thread_id](const char*& log, int len, int par_id, int slot_id, ...) {
    // 1. Extract transaction commit info from log
    CommitInfo commit_info = get_latest_commit_info((char *) log, len);
    timestamp = commit_info.timestamp;

    // 2. Update local watermark for this partition
    sync_util::sync_logger::local_timestamp_[par_id].store(commit_info.timestamp, memory_order_release);

    // 3. Paxos handles actual replication to followers (separate path)
    // This callback just tracks what was logged

    return static_cast<int>(timestamp * 10 + status);
}, thread_id);
```

**Key Points**:
- Leader executes transaction **immediately** (speculative)
- Transaction is **logged to Paxos** asynchronously
- Leader tracks its own watermark based on logged transactions
- Actual replication happens through Paxos's normal Accept/Commit protocol

### 3. Follower Side: Replaying Transactions

When a follower receives a Paxos log entry:

```cpp
// From src/mako/mako.hh - register_paxos_follower_callback
register_for_follower_par_id_return([&,thread_id](const char*& log, int len, int par_id, int slot_id, ...) {
    // 1. Extract transaction timestamp
    CommitInfo commit_info = get_latest_commit_info((char *) log, len);
    timestamp = commit_info.timestamp;

    // 2. Update local timestamp for this partition
    sync_util::sync_logger::local_timestamp_[par_id].store(commit_info.timestamp, memory_order_release);

    // 3. Get current global watermark
    uint32_t w = sync_util::sync_logger::retrieveW();

    // 4. Safety check: Can we replay this transaction?
    if (sync_util::sync_logger::safety_check(commit_info.timestamp, w)) {
        // YES: Replay transaction now
        treplay_in_same_thread_opt_mbta_v2(par_id, (char*)log, len, db, nthreads);
        status = mako::PaxosStatus::STATUS_REPLAY_DONE;
    } else {
        // NO: Queue for later replay
        un_replay_logs_.push(...);
        status = mako::PaxosStatus::STATUS_SAFETY_FAIL;
    }

    return static_cast<int>(timestamp * 10 + status);
}, thread_id);
```

**Key Points**:
- Followers receive Paxos logs in **slot order** (FIFO per partition)
- Each log entry contains a **serialized transaction**
- Follower checks **watermark** before replaying
- If watermark not advanced enough, transaction is **queued**
- Once watermark advances, queued transactions are replayed

### 4. Paxos Protocol Details

Mako uses **Multi-Paxos** with optimizations:

```cpp
// From src/deptran/paxos/server.h
void OnPrepare(slotid_t slot_id, ballot_t ballot, ...);    // Phase 1
void OnAccept(slotid_t slot_id, ballot_t ballot,
              shared_ptr<Marshallable> &cmd, ...);          // Phase 2a
void OnCommit(slotid_t slot_id, ballot_t ballot,
              shared_ptr<Marshallable> &cmd);               // Phase 2b
```

**Optimization: Bulk Prepare**
- Leader can prepare **ranges** of slots at once
- Reduces message overhead for stable leaders
- Implementation: `OnBulkPrepare()`

**Optimization: Batch Logging**
- Multiple transactions batched into single Paxos log entry
- Amortizes Paxos overhead across transactions

### 5. How Paxos Ensures Consistency

Even though Mako executes speculatively, Paxos guarantees:

1. **Log Ordering**: All replicas see transactions in the **same order** (slot-based)
2. **Durability**: Committed logs survive failures (replicated to majority)
3. **Leader Election**: If leader fails, follower can become new leader with complete log
4. **No Gaps**: Paxos fills gaps in log slots before advancing

### 6. The Paxos-Watermark Synchronization

```
Time ──►

Leader:
  [Txn1] ──Execute──► [Log to Paxos] ──► [Update local_timestamp_[0]]
                                             │
  [Txn2] ──Execute──► [Log to Paxos] ──► [Update local_timestamp_[1]]
                                             │
                                             ▼
                                    [Compute single_watermark_]
                                             │
                           ┌─────────────────┘
                           │
                           ▼ (Exchange watermarks)

Follower:                                    │
  [Receive Paxos Log] ──► [Safety Check] ───┤
                               │             │
                               ├─ Pass  ──►  [Replay Txn1]
                               │             │
                               └─ Fail  ──►  [Queue for later]
                                             │
                                             ▼
                                    [Wait for watermark]
```

The watermark acts as a **synchronization barrier** between Paxos replication (which is ordered) and transaction visibility (which can be out-of-order).

### 7. Special Paxos Messages

**NO-OPS (Heartbeats)**:
```cpp
// From src/mako/benchmarks/common2.h
int isNoops(const char *log, int len) {
    if (len==8 && log[0]=='n' && log[1]=='o' && log[2]=='-' &&
        log[3]=='o' && log[4]=='p' && log[5]=='s' && log[6]==':') {
        return log[7]-'0';  // Returns epoch number
    }
    return -1;
}
```

NO-OPS serve multiple purposes:
- **Heartbeat**: Keep Paxos alive during idle periods
- **Epoch Transition**: Mark end of one epoch, start of next
- **Watermark Sync Point**: Force global watermark computation across shards
- **Failure Detection**: Help detect failed replicas

When a NO-OP is received:
1. All threads wait until everyone receives it
2. Local watermarks are exchanged between shards
3. Global watermark is updated
4. Queued transactions below new watermark are replayed

---

## The Watermark System

The watermark system is Mako's secret sauce for safe speculation. Let's understand it deeply.

### What is a Watermark?

A **watermark** is a timestamp below which **all transactions are guaranteed to be replicated and ordered**.

```
Timestamp Timeline:
├─────────────────┼─────────────────┼─────────────────►
0              Watermark=100     Current=150

Transactions with timestamp ≤ 100:  ✓ Safe to make visible
Transactions with timestamp > 100:  ⚠ Still speculative
```

### Watermark Components

```cpp
// From src/mako/benchmarks/sto/sync_util.hh
class sync_logger {
    // Per-partition timestamps (one per Paxos stream)
    static vector<std::atomic<uint32_t>> local_timestamp_;

    // Global watermark (minimum across all partitions)
    static std::atomic<uint32_t> single_watermark_;

    // Disk persistence timestamps
    static vector<std::atomic<uint32_t>> disk_timestamp_;
};
```

**Three levels of timestamps**:

1. **local_timestamp_[i]**: Latest timestamp logged to Paxos for partition i
2. **disk_timestamp_[i]**: Latest timestamp persisted to disk for partition i
3. **single_watermark_**: MIN(all local_timestamp_, all disk_timestamp_)

### Computing the Watermark

```cpp
// From sync_util.hh
static uint32_t computeLocal() {
    uint32_t min_so_far = numeric_limits<uint32_t>::max();

    // Take minimum across all partitions
    for (int i=0; i<nthreads; i++) {
        auto repl_ts = local_timestamp_[i].load(memory_order_acquire);
        auto disk_ts = disk_timestamp_[i].load(memory_order_acquire);

        // For partition i, take min of replication and disk
        auto partition_min = min(repl_ts, disk_ts);

        // Overall watermark is min across all partitions
        if (partition_min >= single_watermark_.load(memory_order_acquire))
            min_so_far = min(min_so_far, partition_min);
    }

    // Update global watermark
    if (min_so_far != numeric_limits<uint32_t>::max()) {
        single_watermark_.store(min_so_far, memory_order_release);
    }

    return single_watermark_.load(memory_order_acquire);
}
```

**Why minimum?** Because watermark represents **guaranteed progress** - we can only advance it when **all partitions** have caught up.

### Safety Check

```cpp
static bool safety_check(uint32_t timestamp, uint32_t watermark) {
    if (!worker_running) return true;  // During shutdown, allow everything
    return timestamp <= watermark;      // Can only replay if below watermark
}
```

Simple but powerful: A transaction with timestamp T can only be made visible if T ≤ watermark.

### Watermark Exchange (Cross-Shard)

For multi-shard deployments, watermarks must be synchronized:

```cpp
// From sync_util.hh - Follower replicas exchange watermarks
// eRPC server thread
thread server_thread(&sync_logger::server_watermark_exchange);

// eRPC client thread
thread client_thread(&sync_logger::client_watermark_exchange);
```

**Protocol**:
1. Each shard computes its local watermark
2. Periodically, shards exchange watermarks via eRPC
3. Each shard takes **minimum** of received watermarks
4. This becomes the new `single_watermark_`

**Why exchange?** Cross-shard transactions may depend on transactions from other shards. We need global knowledge to ensure safety.

### Timestamp Encoding

```cpp
// Timestamps are encoded as: timestamp*10 + epoch
uint32_t encoded = commit_info.timestamp * 10 + epoch;
```

**Why multiply by 10?** To embed additional metadata (status codes) in last digit while preserving timestamp ordering.

### Watermark Advancement

Watermarks advance through two mechanisms:

**1. Natural Advancement** (Transaction logging):
```cpp
// When transaction commits
sync_util::sync_logger::local_timestamp_[par_id].store(
    commit_info.timestamp, memory_order_release
);
```

**2. Forced Advancement** (NO-OPS):
```cpp
// On NO-OP reception
if (par_id == 0) {
    uint32_t local_w = sync_util::sync_logger::computeLocal();
    // Exchange with other shards
    mako::NFSSync::set_key("noops_phase_"+std::to_string(shardIdx), ...);
}
```

### Historical Watermarks

```cpp
// From sync_util.hh
// term → shard watermark (for failure recovery)
static std::unordered_map<int, uint32_t> hist_timestamp;

static void update_stable_timestamp(int epoch, uint32_t tt) {
   hist_timestamp[epoch] = tt;
}
```

Mako tracks **historical watermarks per epoch** to support:
- Failure recovery (replay from last stable epoch)
- Garbage collection (free memory below old watermarks)
- Performance analysis (track watermark lag)

### The Watermark-Paxos Dance

```
┌─────────────┐         ┌─────────────┐         ┌─────────────┐
│   Leader    │         │   Paxos     │         │  Follower   │
└──────┬──────┘         └──────┬──────┘         └──────┬──────┘
       │                       │                       │
   1. Execute                  │                       │
      Txn @ T=100              │                       │
       │                       │                       │
   2. Log to Paxos             │                       │
       ├───────────────────────►                       │
       │                  3. Replicate                 │
       │                       ├───────────────────────►
       │                       │                  4. Receive
   5. Update                   │                      Log @ T=100
      local_ts[0]=100          │                       │
       │                       │                  6. Check watermark
   7. Compute                  │                      (W=50, T=100)
      watermark=50             │                       │
       │                       │                  7. T > W: Queue
   8. NO-OP                    │                       │
       ├───────────────────────►                       │
       │                  9. Replicate NO-OP           │
       │                       ├───────────────────────►
       │                       │                 10. All threads
       │                       │                     receive NO-OP
  11. Exchange watermarks◄─────┼───────────────────────┤
       │                       │                 12. Compute W=100
       │                       │                       │
       │                       │                 13. Replay queued
       │                       │                     Txn @ T=100 ✓
       │                       │                       │
```

---

## Transaction Lifecycle

Let's trace a transaction from start to finish.

### Phase 1: Client Initiates Transaction (Leader)

```cpp
// Client code
abstract_db *db = ...;
void *txn = db->new_txn(flags, arena, hint);

// Read operation
bool found = db->get(txn, table_id, key, value);

// Write operation
db->put(txn, table_id, key, value);

// Commit
bool success = db->commit_txn(txn);
```

### Phase 2: Speculative Execution (Leader)

The leader executes immediately **without waiting for Paxos**:

```cpp
// From src/mako/benchmarks/mbta_wrapper.hh (pseudocode)
bool commit_txn(void *txn) {
    // 1. Assign timestamp
    uint32_t timestamp = get_current_timestamp();

    // 2. Validate against current watermark
    if (timestamp > single_watermark + MAX_SPECULATION) {
        abort_txn(txn);  // Too far ahead
        return false;
    }

    // 3. Execute transaction in Masstree
    bool success = masstree_commit(txn, timestamp);

    // 4. If successful, log to Paxos (async)
    if (success) {
        serialize_and_log_to_paxos(txn, timestamp);
    }

    return success;
}
```

**Key**: Leader returns **immediately** to client, doesn't wait for Paxos.

### Phase 3: Paxos Logging (Asynchronous)

```cpp
// Serialize transaction
char* log_buffer = serialize_txn(txn);
int log_len = get_log_length(txn);

// Add to Paxos log (thread i handles partition i)
add_log_to_nc(log_buffer, log_len, partition_id);
```

Multi-Paxos handles:
- **Prepare**: Leader prepares slot
- **Accept**: Leader proposes value, followers accept
- **Commit**: Once majority accepts, log is committed
- **Learn**: All followers learn committed value

### Phase 4: Paxos Callback (Leader)

```cpp
// When Paxos commits the log entry
register_for_leader_par_id_return([](const char*& log, int len, int par_id, int slot_id) {
    // Extract timestamp from committed log
    CommitInfo commit_info = get_latest_commit_info((char*)log, len);

    // Update partition watermark
    sync_util::sync_logger::local_timestamp_[par_id].store(
        commit_info.timestamp, memory_order_release
    );

    // Transaction is now durable!
    return STATUS_NORMAL;
}, thread_id);
```

### Phase 5: Follower Receives Log

```cpp
register_for_follower_par_id_return([](const char*& log, int len, int par_id, int slot_id,
                                       std::queue<...>& un_replay_logs_) {
    // Extract transaction
    CommitInfo commit_info = get_latest_commit_info((char*)log, len);
    uint32_t timestamp = commit_info.timestamp;

    // Update local timestamp
    sync_util::sync_logger::local_timestamp_[par_id].store(
        timestamp, memory_order_release
    );

    // Get current watermark
    uint32_t w = sync_util::sync_logger::retrieveW();

    // Safety check
    if (sync_util::sync_logger::safety_check(timestamp, w)) {
        // Replay immediately
        treplay_in_same_thread_opt_mbta_v2(par_id, (char*)log, len, db, nthreads);
        return STATUS_REPLAY_DONE;
    } else {
        // Queue for later (timestamp too high)
        un_replay_logs_.push(make_tuple(timestamp, slot_id, par_id, len, log));
        return STATUS_SAFETY_FAIL;
    }
}, thread_id);
```

### Phase 6: Watermark Advancement

```cpp
// Periodically (or on NO-OP)
uint32_t new_watermark = sync_util::sync_logger::computeLocal();

// Process queued transactions
while (!un_replay_logs_.empty()) {
    auto it = un_replay_logs_.front();
    uint32_t queued_timestamp = std::get<0>(it);

    if (safety_check(queued_timestamp, new_watermark)) {
        // Now safe to replay!
        treplay_in_same_thread_opt_mbta_v2(...);
        un_replay_logs_.pop();
    } else {
        break;  // Still too early
    }
}
```

### Phase 7: Transaction Becomes Visible

Once replayed on follower:
- Transaction results are visible to reads
- Reads on follower see consistent snapshot
- Serializability is maintained

### Complete Flow Diagram

```
┌──────────────────────────────────────────────────────────────┐
│                    Transaction Lifecycle                      │
└──────────────────────────────────────────────────────────────┘

CLIENT              LEADER                PAXOS             FOLLOWER
  │                   │                     │                   │
  ├─Begin Txn────────►│                     │                   │
  │                   │                     │                   │
  ├─Read(K1)─────────►│──Read Masstree      │                   │
  │◄──────────Value───┤                     │                   │
  │                   │                     │                   │
  ├─Write(K2,V)──────►│──Write Masstree     │                   │
  │                   │   (speculative)     │                   │
  │                   │                     │                   │
  ├─Commit───────────►│──Assign timestamp   │                   │
  │                   │   T=100             │                   │
  │◄──────────OK──────┤ (immediate return!) │                   │
  │                   │                     │                   │
                      │──Serialize txn      │                   │
                      │                     │                   │
                      ├─Log(T=100)─────────►│                   │
                      │                 Prepare                 │
                      │                 Accept                  │
                      │                     ├─Replicate────────►│
                      │                     │               Receive
                      │                     │               Log(T=100)
                      │                     │                   │
                      │◄────Committed───────┤                   │
                      │                     │                   ├─Check W=50
                      │                     │                   ├─Queue(T > W)
                      │                     │                   │
                      ├─Update local_ts─────┤                   │
                      │  [0]=100            │                   │
                      │                     │                   │
                      ├─NO-OP(epoch++)─────►│                   │
                      │                     ├─Replicate────────►│
                      │                     │                   │
                      │◄────────────────────┼─Exchange W────────┤
                      ├─Compute W=100───────┼──────────────────►│
                      │                     │              Compute W=100
                      │                     │                   │
                      │                     │              ├─Replay T=100 ✓
                      │                     │              └─Make Visible
                      │                     │                   │
```

---

## API and Interfaces

### Database API (abstract_db)

The main interface for applications:

```cpp
class abstract_db {
public:
    // Transaction management
    virtual void *new_txn(uint64_t txn_flags,
                          str_arena &arena,
                          TxnProfileHint hint = HINT_DEFAULT) = 0;

    virtual bool commit_txn(void *txn) = 0;
    virtual void abort_txn(void *txn) = 0;

    // Data operations
    virtual bool get(void *txn,
                     uint64_t table_id,
                     const std::string &key,
                     std::string &value) = 0;

    virtual void put(void *txn,
                     uint64_t table_id,
                     const std::string &key,
                     const std::string &value) = 0;

    // Initialization
    virtual void thread_init(bool loader, int source=0) {}
    virtual void thread_end() {}
};
```

### Benchmark Interface

Applications implement `bench_worker`:

```cpp
class bench_worker {
public:
    virtual void run() = 0;  // Main workload loop

protected:
    abstract_db *db;         // Database handle
    int worker_id;           // Thread ID
    uint64_t ntxn_commits;   // Performance counters
};
```

### Paxos Integration API

For replication control:

```cpp
// Register callback for when Paxos commits logs
void register_for_leader_par_id_return(
    std::function<int(const char*& log, int len, int par_id, int slot_id, ...)> callback,
    int thread_id
);

void register_for_follower_par_id_return(
    std::function<int(const char*& log, int len, int par_id, int slot_id, ...)> callback,
    int thread_id
);

// Add transaction log to Paxos
void add_log_to_nc(char* log, int len, int partition_id);
```

### Watermark Management API

```cpp
namespace sync_util {
class sync_logger {
public:
    // Check if timestamp is safe
    static bool safety_check(uint32_t timestamp, uint32_t watermark);

    // Get current watermark
    static uint32_t retrieveW();

    // Compute new watermark across partitions
    static uint32_t computeLocal();

    // Update historical watermark
    static void update_stable_timestamp(int epoch, uint32_t timestamp);
};
}
```

### Configuration

```cpp
class BenchmarkConfig {
public:
    int getNthreads();           // Number of worker threads
    int getShardIndex();         // This shard's index
    int getNshards();            // Total number of shards
    bool getIsReplicated();      // Is replication enabled?
    bool getLeaderConfig();      // Is this the leader?
    //...
};
```

---

## Failure Recovery

Mako handles failures through Paxos replication and watermark-based recovery.

### Failure Scenarios

#### 1. Leader Failure

**Detection**: Paxos timeout (typically 10 seconds)

**Recovery**:
```
1. Follower detects leader timeout
2. Follower initiates Paxos leader election
3. New leader:
   a. Reads Paxos log from last checkpoint
   b. Replays all committed transactions
   c. Restores watermark to last stable value
   d. Continues from latest slot
```

**Impact**: 10-second outage + replay time (typically 1-2 seconds)

**Key Code**:
```cpp
// From src/mako/README.md - COCO failure recovery
// Leader failure with timeout 10 seconds:
// - Other shards are unaware of this failure (no perfect detector)
// - Healthy shards still execute transactions in current epoch
// - System blocks without throughput for 10s + 1RTT
```

#### 2. Follower Failure

**Detection**: Missed heartbeats

**Recovery**:
```
1. Follower restarts
2. Contacts Paxos leader
3. Requests missing log entries (from max_executed_slot_)
4. Replays logs to catch up
5. Updates watermark
6. Joins normal operation
```

**Impact**: No impact on leader or other followers. Catching up happens in background.

#### 3. Network Partition

**Scenario**: Datacenter split

**Behavior**:
```
- Majority partition: Continues operating (Paxos quorum maintained)
- Minority partition: Stops committing (cannot reach quorum)
- Watermark: Minority partition watermark stops advancing
```

**Recovery**:
```
1. Network heals
2. Minority partition rejoins
3. Replays missed Paxos logs
4. Watermark advances
5. Queued transactions become visible
```

### Checkpoint and Recovery

```cpp
// Periodic checkpointing (pseudocode)
void checkpoint() {
    uint32_t checkpoint_watermark = sync_util::sync_logger::retrieveW();

    // Save to disk:
    // 1. Watermark value
    save_watermark(checkpoint_watermark);

    // 2. Masstree snapshot
    masstree_->checkpoint();

    // 3. Paxos log metadata
    save_paxos_state(max_committed_slot_, cur_epoch);
}

void recover_from_checkpoint() {
    // Restore watermark
    uint32_t checkpoint_watermark = load_watermark();
    sync_util::sync_logger::setSingleWatermark(checkpoint_watermark);

    // Restore Masstree
    masstree_->recover();

    // Restore Paxos state
    slotid_t resume_slot = load_paxos_state();

    // Replay logs from checkpoint to latest
    replay_paxos_logs(resume_slot, max_committed_slot_);
}
```

### Comparison: Mako vs COCO Failure Recovery

From the README:

**COCO** (epoch-based):
```
- Uses single global epoch for all transactions
- If shard fails during epoch E:
  * All transactions in epoch E are abandoned
  * Other shards cannot advance epoch (need consensus)
  * System blocks for 10s timeout + 1RTT
  * Result: Zero throughput during failure
```

**Mako** (watermark-based):
```
- Uses per-partition watermarks
- If shard fails:
  * Only that shard's watermark stops advancing
  * Other shards continue with their watermarks
  * Global watermark = min(all shards)
  * Result: Degraded throughput but not zero
```

**Key Advantage**: Mako's watermark system is more **fine-grained** than COCO's epoch system, allowing better **failure isolation**.

---

## Performance Optimizations

### 1. Batched Paxos Logging

```cpp
// From mako.hh - Batching transactions
vector<shared_ptr<TpcCommitCommand>> batch_buffer_;
shared_ptr<TpcBatchCommand> batch_cmd = std::make_shared<TpcBatchCommand>();

// Add transactions to batch
batch_buffer_.push_back(curCmd);

// When batch is full or timeout, log batch
if (batch_buffer_.size() >= BATCH_SIZE || timeout_reached()) {
    batch_cmd->cmds_ = batch_buffer_;
    add_log_to_paxos(serialize(batch_cmd), batch_size);
    batch_buffer_.clear();
}
```

**Benefit**: Reduces Paxos overhead from O(n) to O(n/BATCH_SIZE)

### 2. Partitioned Paxos Streams

```cpp
// One Paxos stream per partition (thread)
for (int i = 0; i < BenchmarkConfig::getInstance().getNthreads(); i++) {
    register_paxos_leader_callback(thread_id = i);
}
```

**Benefit**: Parallel Paxos logging across partitions. No cross-partition ordering needed.

### 3. Async Disk Persistence

```cpp
#ifndef DISABLE_DISK
// Separate timestamp for disk vs replication
sync_util::sync_logger::disk_timestamp_[par_id].store(timestamp, memory_order_release);
#endif

// Watermark = min(replication_ts, disk_ts) per partition
auto partition_min = min(repl_ts, disk_ts);
```

**Benefit**: Watermark doesn't advance until both replicated AND persisted. But replication and disk happen in parallel.

### 4. Lock-Free Watermark Computation

```cpp
// All atomic operations - no locks!
static vector<std::atomic<uint32_t>> local_timestamp_;
static std::atomic<uint32_t> single_watermark_;

uint32_t computeLocal() {
    // memory_order_acquire/release ensures visibility
    auto repl_ts = local_timestamp_[i].load(memory_order_acquire);
    single_watermark_.store(min_so_far, memory_order_release);
}
```

**Benefit**: Watermark computation doesn't block transaction execution.

### 5. Masstree Cache Optimization

```cpp
#ifdef TUPLE_PREFETCH
    prefetch(tuple);
#endif

#ifdef BTREE_NODE_PREFETCH
    prefetch(btree_node);
#endif
```

**Benefit**: Reduces cache misses in hot paths.

### 6. eRPC for Watermark Exchange

```cpp
// High-performance RPC for cross-shard watermark exchange
thread server_thread(&sync_logger::server_watermark_exchange);
thread client_thread(&sync_logger::client_watermark_exchange);
```

**Benefit**: eRPC provides **sub-microsecond** RPC latency, critical for frequent watermark synchronization.

---

## Comparison with Other Systems

### Mako vs Traditional 2PC

| Aspect | 2PC | Mako |
|--------|-----|------|
| **Latency** | 2 RTTs (prepare + commit) | <1 RTT (speculative exec) |
| **Availability** | Blocks on coordinator failure | Continues with new leader |
| **Throughput** | Limited by prepare phase | High (no prepare wait) |
| **Consistency** | Strong | Strong (via watermarks) |

### Mako vs Calvin

| Aspect | Calvin | Mako |
|--------|---------|------|
| **Execution** | Deterministic, pre-ordered | Speculative, post-validation |
| **Replication** | Replicate inputs | Replicate inputs (Paxos logs) |
| **Latency** | 1 RTT + sequencing | Immediate + background Paxos |
| **Aborts** | Rare (deterministic) | Rare (speculation succeeds) |

### Mako vs COCO

| Aspect | COCO | Mako |
|--------|------|------|
| **Timestamp** | Single global epoch | Per-partition timestamps + watermark |
| **Failure Impact** | Blocks entire system | Degrades proportionally |
| **Watermark** | Epoch-based (coarse) | Timestamp-based (fine-grained) |
| **Recovery** | Abandon full epoch | Replay from watermark |

### Mako vs Spanner

| Aspect | Spanner | Mako |
|--------|---------|------|
| **Timestamps** | TrueTime (HW) | Logical timestamps |
| **Consistency** | External consistency | Serializability |
| **Speculation** | No (wait for TrueTime) | Yes (watermark-based) |
| **Geo-Replication** | Paxos per shard | Multi-Paxos per partition |

---

## Key Takeaways

### What Makes Mako Special?

1. **Speculation Without Compromise**: Execute immediately, validate later, rollback rarely
2. **Fine-Grained Watermarks**: Per-partition progress tracking for better failure isolation
3. **Paxos-Powered**: Leverage proven consensus for replication
4. **High Performance**: Masstree + eRPC + batching = millions of TPS

### When to Use Mako?

**Good Fit**:
- ✓ Geo-replicated deployments (multi-datacenter)
- ✓ Low-conflict workloads (most transactions don't conflict)
- ✓ Latency-sensitive applications (need sub-ms response)
- ✓ Strong consistency requirements (serializability)

**Poor Fit**:
- ✗ High-contention workloads (frequent aborts)
- ✗ Single datacenter (simpler systems work fine)
- ✗ Eventual consistency is acceptable (overkill)

### The Core Insight

**Mako's Philosophy**: "Most transactions don't conflict, so let's optimize for the common case (no conflict) while handling the rare case (conflict) correctly."

This is achieved through:
- **Speculative execution** (common case: fast)
- **Watermark validation** (rare case: safe)
- **Paxos replication** (always: durable)

---

## References

- Mako source code: `src/mako/`
- Paxos implementation: `src/deptran/paxos/`
- Masstree storage: `src/mako/masstree/`
- Configuration: `config/mako_*.yml`

**OSDI'25 Paper**: "Mako: Speculative Distributed Transactions with Geo-Replication"

---

**Document Version**: 1.0
**Last Updated**: 2025-10-30
**Contributors**: Comprehensive codebase analysis
