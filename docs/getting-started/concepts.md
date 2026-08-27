# Key Concepts

Understanding these core concepts will help you effectively use and deploy Mako.

## Table of Contents

1. [Shards](#shards)
2. [Replicas](#replicas)
3. [Transactions](#transactions)
4. [Speculative Execution](#speculative-execution)
5. [Consistency Levels](#consistency-levels)
6. [Storage Engines](#storage-engines)
7. [Network Transports](#network-transports)

---

## Shards

### What is a Shard?

A **shard** (also called a partition) is a horizontal slice of your data. Mako distributes data across multiple shards to achieve horizontal scalability.

```
Total Data
├── Shard 0: keys with hash(key) % N == 0
├── Shard 1: keys with hash(key) % N == 1
├── Shard 2: keys with hash(key) % N == 2
└── Shard N-1: keys with hash(key) % N == N-1
```

### How Sharding Works

**Data Distribution**: Each key is mapped to a shard using a hash function:
```
shard_id = hash(key) % num_shards
```

For example, with 4 shards:
- `user:1001` → hash → shard 2
- `user:1002` → hash → shard 0
- `user:1003` → hash → shard 1
- `user:1004` → hash → shard 3

### Benefits of Sharding

✅ **Scalability**: Add more shards to handle more data and traffic
✅ **Parallelism**: Operations on different shards execute independently
✅ **Performance**: Distribute load across multiple servers
✅ **Isolation**: Failures in one shard don't affect others

### Shard Configuration

Define shards in your YAML configuration:

```yaml
site:
  server:
    - ["s101:8100"]  # Shard 0
    - ["s102:8100"]  # Shard 1
    - ["s103:8100"]  # Shard 2
```

### Multi-Shard Transactions

Mako supports **distributed transactions** spanning multiple shards:

```cpp
// Transaction accessing multiple shards
txn->Put("user:1001", data);    // Shard 2
txn->Put("user:1002", data);    // Shard 0
txn->Commit();  // Coordinated across shards
```

Mako's speculative 2PC protocol ensures ACID guarantees across all shards.

---

## Replicas

### What is a Replica?

A **replica** is a copy of a shard's data maintained for fault tolerance. Each shard can have multiple replicas across different servers or datacenters.

```
Shard 0
├── Primary Replica (s101)
├── Replica 1 (s201)
├── Replica 2 (s301)
└── Replica 3 (s401)
```

### Replication with Paxos

Mako uses **Paxos** consensus to keep replicas synchronized:

1. **Propose**: Coordinator proposes a write to all replicas
2. **Vote**: Replicas vote on the proposal
3. **Commit**: Once majority agrees, write is committed
4. **Replicate**: All replicas apply the write

### Replica Configuration

Configure replicas in YAML:

```yaml
site:
  server:
    # Shard 0 with 3 replicas
    - ["s101:8100", "s201:8101", "s301:8102"]
    # Shard 1 with 3 replicas
    - ["s102:8100", "s202:8101", "s302:8102"]
```

The first server in each list is the **leader** (primary replica).

### Fault Tolerance

With `N` replicas, Mako can tolerate `⌊N/2⌋` failures:

- **3 replicas**: Tolerates 1 failure
- **5 replicas**: Tolerates 2 failures
- **7 replicas**: Tolerates 3 failures

### Geo-Replication

Replicas can be distributed across datacenters:

```yaml
process:
  s101: dc1-server1  # Datacenter 1
  s201: dc2-server1  # Datacenter 2
  s301: dc3-server1  # Datacenter 3

host:
  dc1-server1: 10.0.1.100
  dc2-server1: 10.0.2.100
  dc3-server1: 10.0.3.100
```

**Benefits**:
- 🌍 Low latency for geo-distributed users
- 🛡️ Datacenter-level fault tolerance
- 💾 Data locality compliance

---

## Transactions

### What is a Transaction?

A **transaction** is a sequence of reads and writes that execute atomically—either all operations succeed or all fail.

### ACID Properties

Mako provides full ACID guarantees:

- **Atomicity**: All-or-nothing execution
- **Consistency**: Database remains in valid state
- **Isolation**: Serializability (strongest level)
- **Durability**: Committed data survives failures

### Transaction Lifecycle

```
1. BEGIN
   ↓
2. READ/WRITE operations
   ↓
3. COMMIT or ABORT
   ↓
4. Persistence (async)
```

### Example Transaction

```cpp
// Start transaction
auto txn = BeginTransaction();

// Read operation
std::string balance = txn->Get("account:1001:balance");
int amount = std::stoi(balance);

// Write operation
amount += 100;
txn->Put("account:1001:balance", std::to_string(amount));

// Commit (atomic)
txn->Commit();  // Returns immediately with speculative execution
```

### Distributed Transactions

Transactions can span multiple shards:

```cpp
// Transfer between accounts on different shards
auto txn = BeginTransaction();

// Debit from account on shard 0
int balance1 = txn->Get("account:1001:balance");
txn->Put("account:1001:balance", balance1 - 100);

// Credit to account on shard 1
int balance2 = txn->Get("account:2001:balance");
txn->Put("account:2001:balance", balance2 + 100);

// Atomic commit across both shards
txn->Commit();
```

### Conflict Resolution

When transactions conflict, Mako uses:

- **Dependency tracking**: Build conflict graph
- **Topological ordering**: Determine commit order
- **Selective abort**: Only abort minimal set of transactions

---

## Speculative Execution

### The Problem with Traditional 2PC

Traditional two-phase commit blocks on consensus:

```
Traditional:
Execute → Wait for Paxos → Wait for Persistence → Return to Client
         ⏰ ~50ms RTT      ⏰ ~10ms disk
Total: ~60-100ms latency
```

### Mako's Solution: Speculative 2PC

Mako executes transactions **speculatively** without waiting:

```
Speculative:
Execute → Return to Client (immediately!)
         ↓ (background)
         Paxos → Persistence

Total: ~2-5ms latency
```

### How It Works

1. **Execute speculatively**: Run transaction without waiting for consensus
2. **Track dependencies**: Record conflicts with other transactions
3. **Background replication**: Asynchronously replicate via Paxos
4. **Handle failures**: If replication fails, cascade abort dependent transactions

### Key Innovation

**Bounded cascading aborts**: Mako uses novel mechanisms to prevent unbounded cascading when failures occur:

- **Watermark-based commit**: Only expose transactions that are safely replicated
- **Dependency pruning**: Limit the cascade depth
- **Fast recovery**: Quickly stabilize after failures

### Performance Impact

Speculative execution achieves:
- **8.6× higher throughput** than state-of-the-art systems
- **Near-local latency** (~2ms vs ~60ms for traditional 2PC)
- **Full ACID guarantees** despite speculation

---

## Consistency Levels

### Serializability

Mako provides **strict serializability**, the strongest consistency level:

- Transactions appear to execute in a total order
- Order respects real-time precedence
- No anomalies (dirty reads, lost updates, write skew, etc.)

### Read Operations

**Single-key reads**: Direct read from local shard
```cpp
std::string value = txn->Get("user:1001");
```

**Multi-key reads**: Coordinated across shards
```cpp
std::string v1 = txn->Get("user:1001");  // Shard 0
std::string v2 = txn->Get("user:2001");  // Shard 1
```

### Write Operations

**Writes are buffered** until commit:
```cpp
txn->Put("key1", "value1");  // Buffered
txn->Put("key2", "value2");  // Buffered
txn->Commit();               // Atomically applied
```

### Snapshot Isolation *(To be implemented)*

Future versions may support configurable isolation levels:
- Read Committed
- Snapshot Isolation
- Serializable (current default)

---

## Storage Engines

### Masstree (In-Memory)

**Masstree** is the primary storage engine:

- **Concurrent B+tree** optimized for multi-core
- **In-memory** for ultra-fast access
- **Cache-friendly** memory layout
- **Lock-free reads** for high concurrency

**Performance**:
- Millions of operations per second per core
- Sub-microsecond read latency
- Optimized for modern CPUs

### RocksDB (Persistent)

**RocksDB** provides optional persistence:

- **Write-ahead log (WAL)** for durability
- **Asynchronous writes** don't block transactions
- **Background compaction** maintains performance
- **Configurable** persistence level

**Configuration**:
```cpp
// Enable RocksDB persistence
persistence.persistAsync(log_data, size, shard_id, partition_id,
    [](bool success) {
        // Callback when persisted
    });
```

### Hybrid Approach

Mako uses **both** engines:
1. **Masstree**: Primary storage for fast access
2. **RocksDB**: Background persistence for durability
3. **Recovery**: Replay WAL from RocksDB on restart

---

## Network Transports

Mako supports multiple network transport layers:

### TCP/IP (Default)

Standard TCP sockets via RRR framework:
- **Cross-platform**: Works everywhere
- **Reliable**: Built-in retransmission
- **Configuration**: Standard socket options

### DPDK *(Advanced)*

**Data Plane Development Kit** for kernel bypass:
- **Ultra-low latency**: Bypass OS network stack
- **High throughput**: 10-100 Gbps
- **Requires**: Special hardware and configuration

Enable with: `cmake -DDPDK_ENABLED=ON`

### RDMA *(Advanced)*

**Remote Direct Memory Access** for InfiniBand:
- **Zero-copy**: Direct memory transfer
- **Sub-microsecond latency**
- **Requires**: InfiniBand hardware

---

## Configuration Hierarchy

Understanding how Mako organizes deployments:

```
Host (Physical Machine)
 ├─ Process (OS Process)
 │   ├─ Site/Partition (Thread)
 │   │   ├─ Shard 0
 │   │   └─ Reactor (Event Loop)
 │   └─ Site/Partition (Thread)
 │       └─ Shard 1
 └─ Process (OS Process)
     └─ ...
```

**Hierarchy**:
1. **Host**: Physical or virtual machine (IP address)
2. **Process**: OS process running `dbtest`
3. **Site**: Logical partition (thread) within process
4. **Shard**: Data partition managed by site

**Example mapping**:
```yaml
# Sites (servers and clients)
site:
  server:
    - ["s101:8100", "s201:8101"]  # Shard 0, 2 replicas
  client:
    - ["c101"]                     # Client 1

# Site to process mapping
process:
  s101: localhost
  s201: localhost
  c101: localhost

# Process to host mapping
host:
  localhost: 127.0.0.1
```

---

## Summary

**Key Takeaways**:

- 🔀 **Shards**: Horizontal data partitioning for scalability
- 📋 **Replicas**: Copies for fault tolerance (Paxos consensus)
- 🔄 **Transactions**: ACID guarantees with serializability
- ⚡ **Speculative Execution**: Near-local latency with background replication
- 🎯 **Consistency**: Strict serializability by default
- 💾 **Storage**: Masstree (memory) + RocksDB (disk)
- 🌐 **Transport**: TCP/IP, DPDK, RDMA

---

**Next**: [Configuration Reference](config.md) | [Architecture Overview](architecture.md) | [Quick Start](quickstart.md)
