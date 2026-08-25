# Architecture Overview

This document provides a comprehensive overview of Mako's system architecture.

## Table of Contents

1. [High-Level Architecture](#high-level-architecture)
2. [Component Overview](#component-overview)
3. [Transaction Lifecycle](#transaction-lifecycle)
4. [Speculative Two-Phase Commit](#speculative-two-phase-commit)
5. [Replication with Paxos](#replication-with-paxos)
6. [Storage Architecture](#storage-architecture)
7. [Network Communication](#network-communication)
8. [Concurrency and Threading](#concurrency-and-threading)

---

## High-Level Architecture

Mako is a **distributed transactional key-value store** with a layered architecture:

```
┌──────────────────────────────────────────────────────────────┐
│                     Client Applications                       │
│        (RocksDB API / Redis API / Native C++ API)            │
└──────────────────────┬───────────────────────────────────────┘
                       │
┌──────────────────────▼───────────────────────────────────────┐
│                  Transaction Coordinators                     │
│  ┌────────────┬──────────────┬──────────────┬─────────────┐ │
│  │  Mako 2PC  │  Paxos Mgr   │  Dependency  │  Watermark  │ │
│  │  Protocol  │              │   Tracker    │   Manager   │ │
│  └────────────┴──────────────┴──────────────┴─────────────┘ │
└──────────────────────┬───────────────────────────────────────┘
                       │
┌──────────────────────▼───────────────────────────────────────┐
│              SRPC Communication Layer                          │
│  ┌─────────────┬──────────────┬────────────┬──────────────┐ │
│  │ TCP Sockets │  Coroutines  │  Reactor   │  Event Loop  │ │
│  └─────────────┴──────────────┴────────────┴──────────────┘ │
│         (Optional: DPDK / RDMA)                              │
└──────────────────────┬───────────────────────────────────────┘
                       │
┌──────────────────────▼───────────────────────────────────────┐
│               Sharded Data Partitions                         │
│  ┌──────────────────┬──────────────────┬──────────────────┐ │
│  │    Shard 0       │     Shard 1      │     Shard N      │ │
│  │  ┌────────────┐  │  ┌────────────┐  │  ┌────────────┐  │ │
│  │  │  Leader    │  │  │  Leader    │  │  │  Leader    │  │ │
│  │  ├────────────┤  │  ├────────────┤  │  ├────────────┤  │ │
│  │  │ Follower 1 │  │  │ Follower 1 │  │  │ Follower 1 │  │ │
│  │  ├────────────┤  │  ├────────────┤  │  ├────────────┤  │ │
│  │  │ Follower 2 │  │  │ Follower 2 │  │  │ Follower 2 │  │ │
│  │  └────────────┘  │  └────────────┘  │  └────────────┘  │ │
│  └──────────────────┴──────────────────┴──────────────────┘ │
└──────────────────────┬───────────────────────────────────────┘
                       │
┌──────────────────────▼───────────────────────────────────────┐
│                   Storage Engines                             │
│  ┌──────────────────────────┬─────────────────────────────┐ │
│  │   Masstree (In-Memory)   │   RocksDB (Persistent)      │ │
│  │  • Concurrent B+tree     │  • LSM-tree on disk         │ │
│  │  • Lock-free reads       │  • Write-ahead log (WAL)    │ │
│  │  • Cache-friendly        │  • Asynchronous writes      │ │
│  └──────────────────────────┴─────────────────────────────┘ │
└──────────────────────────────────────────────────────────────┘
```

---

## Component Overview

### Transaction Coordinator

The **Transaction Coordinator** orchestrates distributed transactions:

**Responsibilities**:
- Begin/commit/abort transactions
- Coordinate reads/writes across multiple shards
- Track dependencies between transactions
- Manage speculative execution
- Handle conflict resolution

**Key classes** (in `src/mako/` and `src/deptran/`):
- `Coordinator`: Main coordinator logic (with protocol-specific subclasses)
- `Transaction`: Individual transaction state

### Shard Server

Each **Shard** manages a partition of data:

**Responsibilities**:
- Store and retrieve key-value pairs
- Execute transaction operations locally
- Participate in consensus (Paxos)
- Replicate data to followers
- Handle shard-local concurrency

**Key classes**:
- `SchedulerClassic` / `TxLogServer`: Schedule transaction execution on shard
- `CoordinatorMultiPaxos`: Paxos consensus implementation

### SRPC Communication Layer

**SRPC** (Custom RPC framework) provides:

**Features**:
- **Asynchronous RPC**: Non-blocking remote procedure calls
- **Coroutines**: Lightweight concurrency (thousands of concurrent operations)
- **Reactor Pattern**: Event-driven I/O without threads
- **Multiple transports**: TCP/IP, DPDK, RDMA

See **[Coroutines & Reactor Guide](coroutines_guide.md)** for details.

### Storage Engines

**Masstree** (primary):
- In-memory concurrent B+tree
- Optimized for multi-core CPUs
- Sub-microsecond read latency
- Millions of ops/sec per core

**RocksDB** (optional persistence):
- LSM-tree based persistent storage
- Write-ahead logging for durability
- Asynchronous writes (don't block transactions)
- Background compaction

---

## Transaction Lifecycle

### Phase 1: Transaction Execution

```
Client                Coordinator              Shard 0         Shard 1
  │                        │                      │               │
  ├─BEGIN────────────────→│                      │               │
  │                        │                      │               │
  ├─PUT(key1, val1)──────→│                      │               │
  │                        │──READ(key1)────────→│               │
  │                        │←────value───────────│               │
  │                        │                      │               │
  ├─GET(key2)────────────→│                      │               │
  │                        │──READ(key2)─────────────────────────→│
  │                        │←────value──────────────────────────── │
  │←──value────────────────│                      │               │
  │                        │                      │               │
  ├─PUT(key2, val2)──────→│                      │               │
  │                        │                      │               │
  ├─COMMIT────────────────→│                      │               │
```

**Steps**:
1. **BEGIN**: Client starts transaction
2. **READS**: Fetch current values from shards
3. **WRITES**: Buffer writes locally (not yet visible)
4. **COMMIT**: Coordinator initiates commit protocol

### Phase 2: Speculative Commit

```
Coordinator         Shard 0          Shard 1        Replicas
  │                   │                │                │
  ├─PREPARE──────────→│                │                │
  │                   ├─check conflicts│                │
  │                   ├─assign deps────┤                │
  │                   │←──PREPARED─────│                │
  │                   │                │                │
  ├─PREPARE───────────────────────────→│                │
  │                   │                ├─check conflicts│
  │                   │                ├─assign deps────┤
  │                   │←──PREPARED─────│                │
  │                   │                │                │
  ├─COMMIT (speculative)─→│            │                │
  │                   ├─apply writes───┤                │
  │                   ├─return success─┤                │
  │←──SUCCESS─────────│                │                │
  │                   │                │                │
 Client               │─(background)──────────────────→│
 receives             │  Paxos replication              │
 success!             │                │                │
                      │←──replicated───────────────────│
```

**Key innovation**: Client receives success **before** replication completes!

### Phase 3: Background Replication

```
Leader Shard          Follower 1       Follower 2       Follower 3
  │                      │                │                │
  ├─PROPOSE─────────────→│                │                │
  ├─PROPOSE──────────────────────────────→│                │
  ├─PROPOSE───────────────────────────────────────────────→│
  │                      │                │                │
  │←──PROMISE───────────│                │                │
  │←──PROMISE──────────────────────────── │                │
  │                      │                │                │
  ├─ACCEPT──────────────→│                │                │
  ├─ACCEPT───────────────────────────────→│                │
  │                      │                │                │
  │←──ACCEPTED──────────│                │                │
  │←──ACCEPTED─────────────────────────── │                │
  │                      │                │                │
  ├─COMMIT──────────────→│                │                │
  ├─COMMIT───────────────────────────────→│                │
  │                      │                │                │
 Majority reached!     Apply            Apply
 Transaction durable   writes           writes
```

**Paxos ensures**:
- Majority agreement before commit
- Fault tolerance (N/2 failures)
- Consistent replication

---

## Speculative Two-Phase Commit

### Traditional 2PC vs. Speculative 2PC

**Traditional 2PC** (slow):
```
Execute → Wait Paxos → Wait Persist → Return to Client
          (50ms RTT)   (10ms disk)
Total: ~60-100ms
```

**Mako's Speculative 2PC** (fast):
```
Execute → Return to Client  (~2-5ms)
          ↓ (background, async)
          Paxos → Persist
```

### How Speculation Works

1. **Optimistic Execution**: Assume replication will succeed
2. **Dependency Tracking**: Record conflicts with other transactions
3. **Watermark-Based Commit**: Only expose stable transactions to reads
4. **Cascading Abort Prevention**: Limit abort cascades on failure

### Dependency Graph

Mako tracks dependencies between concurrent transactions:

```
T1: READ(x), WRITE(x)
T2: READ(x), WRITE(y)
T3: READ(y), WRITE(z)

Dependency Graph:
T1 ─depends→ T2 ─depends→ T3

If T1 aborts → T2 must abort → T3 must abort (cascade)
Mako limits cascade depth using watermarks!
```

### Watermark Mechanism

```
Timeline:
  T1(committed) → T2(committed) → T3(committed) → T4(replicating) → T5(executing)
                                                  ↑
                                            Watermark
                                    (Transactions before watermark are durable)
```

**Reads only see transactions ≤ watermark** = Strong consistency

**Watermark advances when**:
- Transaction successfully replicated to majority
- All prior dependencies are durable

---

## Replication with Paxos

### Multi-Paxos Protocol

Mako uses **Multi-Paxos** for efficient replication:

**Leader Election**:
1. Followers elect a stable leader
2. Leader proposes all values
3. Skip PREPARE phase after first proposal (optimization)

**Replication Flow**:
```
1. Leader receives transaction commit
2. Leader proposes to followers
3. Majority responds with ACCEPT
4. Leader commits locally
5. Followers apply committed value
```

### Fault Tolerance

**Scenarios**:

**Leader Failure**:
- Detect via heartbeat timeout
- Elect new leader (Paxos)
- New leader catches up from followers
- Resume normal operation

**Follower Failure**:
- Leader continues with remaining replicas
- Requires only majority (N/2 + 1)
- Failed replica catches up when recovered

**Network Partition**:
- Majority partition continues
- Minority partition cannot commit (safety)
- Automatic reconciliation when partition heals

---

## Storage Architecture

### Masstree: Concurrent In-Memory Index

**Structure**:
```
              Root
               │
        ┌──────┼──────┐
        │      │      │
     Node1  Node2  Node3
        │      │      │
     ┌──┴──┐ ┌┴──┐ ┌─┴─┐
    L1  L2  L3 L4  L5 L6  (Leaf nodes with key-value pairs)
```

**Features**:
- **Lock-free reads**: No locks for read operations
- **Multi-version**: Support concurrent readers/writers
- **Cache-friendly**: Optimized memory layout
- **Scalable**: Linear scaling with CPU cores

### RocksDB: Persistent Storage

**LSM-Tree Architecture**:
```
Memory:
  MemTable (active writes)
  ↓ (flush when full)
Disk:
  Level 0: [SST] [SST] [SST]  (newest)
  Level 1: [SST] [SST]
  Level 2: [SST]
  ...
  Level N: [SST]              (oldest, largest)
```

**Write Path**:
1. Write to WAL (write-ahead log) for durability
2. Write to MemTable (in-memory)
3. Return success to application
4. **Background**: Flush MemTable → SST files
5. **Background**: Compact SST files

**Read Path**:
1. Check MemTable
2. Check SST files (Level 0 → Level N)
3. Return value or NOT_FOUND

---

## Network Communication

### SRPC Framework

**Architecture**:
```
Application Thread
      │
      ├─ Create Coroutine ────→ Reactor Thread
      │                            │
      │                            ├─ Event Loop
      │                            │   ├─ Poll sockets
      │                            │   ├─ Timer events
      │                            │   └─ Wake coroutines
      │                            │
      │                            ├─ Coroutine 1 (suspended)
      │                            ├─ Coroutine 2 (running)
      │                            ├─ Coroutine 3 (suspended)
      │                            └─ ...
      │
      └─ Wait for result
```

**Benefits**:
- **Thousands of concurrent RPCs** in single thread
- **No thread-per-connection** overhead
- **Automatic batching** of network I/O
- **Low latency**: Context switch in nanoseconds

See **[Coroutines Guide](coroutines_guide.md)** for details.

### Transport Options

**TCP/IP** (default):
- Standard sockets
- Works everywhere
- Good performance

**DPDK** (kernel bypass):
- 10-100 Gbps throughput
- Sub-microsecond latency
- Requires special NIC

**RDMA** (InfiniBand):
- Zero-copy networking
- Lowest latency
- Requires InfiniBand hardware

---

## Concurrency and Threading

### Thread Model

```
Main Thread
  ├─ Reactor Thread 1
  │    ├─ Shard 0 (Coroutines)
  │    └─ Event Loop
  ├─ Reactor Thread 2
  │    ├─ Shard 1 (Coroutines)
  │    └─ Event Loop
  ├─ Reactor Thread 3
  │    ├─ Shard 2 (Coroutines)
  │    └─ Event Loop
  └─ Background Threads
       ├─ RocksDB Compaction
       ├─ Metrics Collection
       └─ Garbage Collection
```

**Design Principles**:
- **One reactor per shard**: Avoid cross-thread synchronization
- **Coroutines within reactor**: Lightweight concurrency
- **Shared-nothing**: Each shard owns its data
- **Lock-free**: Minimize locking where possible

### Memory Safety with RustyCpp

Mako is migrating to **RustyCpp** for memory safety:

**Smart Pointers**:
- `rusty::Box<T>` - Single ownership
- `rusty::Arc<T>` - Thread-safe sharing
- `rusty::Rc<T>` - Single-thread sharing
- `rusty::Cell<T>` - Interior mutability

**Borrow Checking** *(experimental)*:
- Static analysis of pointer lifetimes
- Prevent use-after-free
- Catch memory leaks

See **[RustyCpp Migration Plan](srpc-rustycpp-migration-plan.md)** for details.

---

## Performance Characteristics

### Latency Breakdown

**Local transaction** (single shard, no replication):
```
Client request:       0.1 ms
Network RTT:          0.2 ms
Masstree read:        0.001 ms
Transaction logic:    0.1 ms
Masstree write:       0.002 ms
Response:             0.2 ms
───────────────────────────
Total:               ~0.6 ms
```

**Distributed transaction** (2 shards, speculative):
```
Prepare phase:        0.5 ms (parallel to both shards)
Speculative commit:   0.1 ms
Client response:      0.3 ms
───────────────────────────
Total (client sees): ~1 ms

Background replication: 50 ms (doesn't block client)
```

### Throughput Scaling

**Single shard**:
- 960K TPS with 24 threads
- Linear scaling up to 24 cores
- Memory bandwidth becomes bottleneck after 24 cores

**10 shards (geo-replicated)**:
- 3.66M TPS total
- 366K TPS per shard
- Limited by cross-datacenter network latency

---

## Summary

Mako's architecture achieves high performance through:

- ⚡ **Speculative execution**: Decouple execution from replication
- 🔄 **Paxos consensus**: Fault-tolerant replication
- 💾 **Hybrid storage**: Fast in-memory + durable persistence
- 🌐 **Efficient networking**: Coroutine-based RPC framework
- 🧵 **Lock-free concurrency**: Minimize synchronization overhead
- 🛡️ **Memory safety**: RustyCpp smart pointers

---

**Next**: [Speculative 2PC Protocol](speculative-2pc.md) | [Replication & Consensus](replication.md) | [Storage Engine Details](masstree.md)
