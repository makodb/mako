# Use Cases: When to Use Mako

This document helps you decide whether Mako is the right choice for your application and provides guidance on common use cases.

## Table of Contents

1. [Decision Framework](#decision-framework)
2. [Ideal Use Cases](#ideal-use-cases)
3. [Not Recommended Use Cases](#not-recommended-use-cases)
4. [Comparison with Alternatives](#comparison-with-alternatives)
5. [Migration Considerations](#migration-considerations)

---

## Decision Framework

### Is Mako Right for You?

Answer these questions to determine if Mako fits your needs:

```
START
  │
  ▼
┌─────────────────────────────────────┐
│ Do you need distributed             │
│ transactions with ACID guarantees?  │
└─────────────────┬───────────────────┘
                  │
         ┌───────┴───────┐
        YES             NO ──────► Consider simpler systems
         │                         (Redis, Memcached, DynamoDB)
         ▼
┌─────────────────────────────────────┐
│ Is your workload primarily          │
│ key-value operations?               │
└─────────────────┬───────────────────┘
                  │
         ┌───────┴───────┐
        YES             NO ──────► Consider SQL databases
         │                         (PostgreSQL, MySQL, CockroachDB)
         ▼
┌─────────────────────────────────────┐
│ Is latency critical                 │
│ (need sub-10ms response)?           │
└─────────────────┬───────────────────┘
                  │
         ┌───────┴───────┐
        YES             NO ──────► Traditional 2PC may work
         │                         (but Mako is still faster!)
         ▼
┌─────────────────────────────────────┐
│ Do you have multiple datacenters    │
│ or need geo-replication?            │
└─────────────────┬───────────────────┘
                  │
         ┌───────┴───────┐
        YES             NO ──────► Mako still beneficial for
         │                         single-DC performance
         ▼
┌─────────────────────────────────────┐
│ Can your data fit in memory         │
│ (with 64GB+ per shard)?             │
└─────────────────┬───────────────────┘
                  │
         ┌───────┴───────┐
        YES             NO ──────► Consider disk-based systems
         │                         (RocksDB, TiKV)
         ▼
  ┌─────────────┐
  │  Mako is a  │
  │ great fit!  │
  └─────────────┘
```

### Quick Compatibility Check

| Requirement | Mako Support |
|-------------|--------------|
| ACID transactions | ✅ Yes |
| Serializability | ✅ Yes |
| Geo-replication | ✅ Yes |
| Sub-10ms latency | ✅ Yes |
| Key-value operations | ✅ Yes |
| SQL queries | ❌ No |
| Complex joins | ❌ No |
| Dataset > memory | ⚠️ Limited |
| Eventual consistency | ⚠️ Overkill |

---

## Ideal Use Cases

### Use Case 1: Financial Transactions

**Scenario:**
Global payment processing system handling card transactions across multiple regions.

**Why Mako?**
- ✅ **ACID required**: Money transfers must be atomic
- ✅ **Low latency**: User experience depends on fast response
- ✅ **Geo-replication**: Users in US, EU, Asia need local access
- ✅ **High throughput**: Millions of transactions per day

**Architecture:**
```
US Datacenter          EU Datacenter          Asia Datacenter
┌─────────────┐        ┌─────────────┐        ┌─────────────┐
│ Mako Shard 0│◄──────►│ Mako Shard 0│◄──────►│ Mako Shard 0│
│ (Leader)    │ Paxos  │ (Follower)  │ Paxos  │ (Follower)  │
├─────────────┤        ├─────────────┤        ├─────────────┤
│ Mako Shard 1│◄──────►│ Mako Shard 1│◄──────►│ Mako Shard 1│
│ (Follower)  │        │ (Leader)    │        │ (Follower)  │
├─────────────┤        ├─────────────┤        ├─────────────┤
│ Mako Shard 2│◄──────►│ Mako Shard 2│◄──────►│ Mako Shard 2│
│ (Follower)  │        │ (Follower)  │        │ (Leader)    │
└─────────────┘        └─────────────┘        └─────────────┘
```

**Sample Transaction:**
```cpp
// Transfer $100 from account A to account B
auto txn = db->begin_transaction();

// Read balances
auto balance_a = txn->get("account:" + account_a + ":balance");
auto balance_b = txn->get("account:" + account_b + ":balance");

// Check sufficient funds
if (std::stod(balance_a) < 100.0) {
    txn->abort();
    return INSUFFICIENT_FUNDS;
}

// Update balances atomically
txn->put("account:" + account_a + ":balance", std::to_string(std::stod(balance_a) - 100));
txn->put("account:" + account_b + ":balance", std::to_string(std::stod(balance_b) + 100));

// Record transaction
txn->put("txn:" + txn_id, serialize(transaction_record));

return txn->commit() ? SUCCESS : FAILED;
```

### Use Case 2: Real-Time Inventory Management

**Scenario:**
E-commerce platform managing inventory across multiple warehouses with real-time stock updates.

**Why Mako?**
- ✅ **Consistency**: Avoid overselling (stock can't go negative)
- ✅ **Speed**: Fast checkout experience
- ✅ **Scalability**: Handle Black Friday traffic spikes
- ✅ **Multi-region**: Warehouses in different locations

**Schema Design:**
```
Key Pattern                          Shard Strategy
──────────────────────────────────────────────────
product:{product_id}:info            HASH(product_id)
product:{product_id}:stock:{wh_id}   HASH(product_id)
order:{order_id}                     HASH(order_id)
cart:{user_id}                       HASH(user_id)
```

**Sample Transaction:**
```cpp
// Reserve inventory for order
auto txn = db->begin_transaction();

for (const auto& item : order.items) {
    auto stock_key = "product:" + item.product_id + ":stock:" + warehouse_id;
    int current_stock = std::stoi(txn->get(stock_key));

    if (current_stock < item.quantity) {
        txn->abort();
        return OUT_OF_STOCK;
    }

    txn->put(stock_key, std::to_string(current_stock - item.quantity));
}

txn->put("order:" + order_id, serialize(order));
return txn->commit() ? ORDER_PLACED : FAILED;
```

### Use Case 3: Gaming Leaderboards and State

**Scenario:**
Multiplayer online game with real-time score updates and player state management.

**Why Mako?**
- ✅ **Low latency**: Games need < 50ms response
- ✅ **Consistency**: No duplicate rewards or lost progress
- ✅ **High throughput**: Thousands of concurrent players
- ✅ **In-memory**: Fast access to player state

**Sample Transactions:**
```cpp
// Award points after game completion
auto txn = db->begin_transaction();

// Get current score
auto score_key = "player:" + player_id + ":score";
int current_score = std::stoi(txn->get(score_key));

// Update score
int new_score = current_score + points_earned;
txn->put(score_key, std::to_string(new_score));

// Update leaderboard position
txn->put("leaderboard:global:" + std::to_string(new_score) + ":" + player_id, "1");
// Delete old position
txn->delete("leaderboard:global:" + std::to_string(current_score) + ":" + player_id);

// Record match history
txn->put("match:" + match_id, serialize(match_result));

txn->commit();
```

### Use Case 4: Session Management

**Scenario:**
Web application managing user sessions across multiple application servers.

**Why Mako?**
- ✅ **Speed**: Every request needs session lookup
- ✅ **Consistency**: Session data must be accurate
- ✅ **Distribution**: Multiple app servers need access
- ✅ **Simple**: Key-value is perfect fit

**Usage:**
```cpp
// Create session
void create_session(const string& session_id, const User& user) {
    auto txn = db->begin_transaction();
    txn->put("session:" + session_id, serialize(user));
    txn->put("session:" + session_id + ":expires", expiry_time);
    txn->commit();
}

// Validate session
optional<User> validate_session(const string& session_id) {
    auto txn = db->begin_transaction();
    auto user_data = txn->get("session:" + session_id);
    auto expires = txn->get("session:" + session_id + ":expires");

    if (user_data.empty() || std::stol(expires) < now()) {
        return nullopt;
    }

    // Extend session (touch)
    txn->put("session:" + session_id + ":expires", new_expiry);
    txn->commit();

    return deserialize<User>(user_data);
}
```

### Use Case 5: Distributed Caching with Consistency

**Scenario:**
Replace inconsistent cache with strongly consistent distributed store.

**Why Mako?**
- ✅ **Speed**: Comparable to Redis for reads
- ✅ **Consistency**: No stale reads, no cache invalidation bugs
- ✅ **Simplicity**: No separate cache layer to manage

**Before (Cache + Database):**
```
Read: Check cache → Miss? → Read DB → Update cache → Return
      Cache inconsistency bugs, thundering herd, complex invalidation

Write: Update DB → Invalidate cache (hope it works!)
       Race conditions, inconsistent reads
```

**After (Mako):**
```
Read: Read Mako → Return
      Always consistent, no invalidation needed

Write: Write Mako → Return
       Atomic, consistent, durable
```

---

## Not Recommended Use Cases

### ❌ Complex Analytical Queries

**Scenario:**
Business intelligence with aggregations, joins, and complex filters.

**Why not Mako?**
- No SQL support
- No query optimizer
- Key-value model doesn't support joins

**Better alternatives:**
- PostgreSQL, MySQL (OLTP + analytics)
- ClickHouse, DuckDB (OLAP)
- Snowflake, BigQuery (data warehouse)

### ❌ Document Database Use Cases

**Scenario:**
Storing and querying JSON documents with nested structures.

**Why not Mako?**
- No secondary indexes
- No document queries
- Limited data model

**Better alternatives:**
- MongoDB
- Couchbase
- FerretDB (PostgreSQL + MongoDB API)

### ❌ Large Object Storage

**Scenario:**
Storing images, videos, or files.

**Why not Mako?**
- All data in memory
- Not designed for large values
- No streaming support

**Better alternatives:**
- S3, MinIO (object storage)
- PostgreSQL with BLOB (smaller files)

### ❌ High-Contention Workloads

**Scenario:**
Many transactions competing for same keys (e.g., global counters).

**Why not Mako?**
- High abort rate due to conflicts
- Speculative execution less effective
- Single shard becomes bottleneck

**Better alternatives:**
- Sharded counters in any system
- Redis with atomic operations
- Custom conflict resolution

### ❌ Very Large Datasets

**Scenario:**
Petabyte-scale data that can't fit in memory.

**Why not Mako?**
- All active data must fit in memory
- Cost-prohibitive at scale

**Better alternatives:**
- TiKV (distributed KV on disk)
- Cassandra, ScyllaDB (wide-column)
- Spanner (SQL, disk-based)

---

## Comparison with Alternatives

### Mako vs. Redis

| Feature | Mako | Redis |
|---------|------|-------|
| **Data model** | Key-value | Key-value + data structures |
| **Transactions** | Full ACID, multi-key | Limited (MULTI/EXEC) |
| **Consistency** | Strong (serializable) | Eventually consistent (cluster) |
| **Replication** | Paxos (CP) | Async (AP) |
| **Persistence** | In-memory + async disk | In-memory + RDB/AOF |
| **Use case** | Transactions | Caching, pub/sub |

**Choose Mako when:** You need multi-key transactions with strong consistency.
**Choose Redis when:** You need data structures, pub/sub, or eventual consistency is OK.

### Mako vs. PostgreSQL

| Feature | Mako | PostgreSQL |
|---------|------|------------|
| **Data model** | Key-value | Relational |
| **Query language** | API only | SQL |
| **Transactions** | ACID | ACID |
| **Latency** | Sub-millisecond | Milliseconds |
| **Scaling** | Horizontal (sharded) | Vertical (primary) |
| **Geo-replication** | Built-in | Extensions |

**Choose Mako when:** You need low-latency KV with geo-replication.
**Choose PostgreSQL when:** You need SQL, joins, or complex queries.

### Mako vs. CockroachDB

| Feature | Mako | CockroachDB |
|---------|------|-------------|
| **Data model** | Key-value | SQL (PostgreSQL compatible) |
| **Replication** | Paxos | Raft |
| **Latency** | ~2ms (speculative) | ~50ms (wait for consensus) |
| **Consistency** | Serializable | Serializable |
| **Geo-replication** | Yes | Yes |
| **Speculation** | Yes | No |

**Choose Mako when:** Latency is critical, KV is sufficient.
**Choose CockroachDB when:** You need SQL, joins, PostgreSQL compatibility.

### Mako vs. Spanner

| Feature | Mako | Spanner |
|---------|------|---------|
| **Data model** | Key-value | SQL |
| **Timestamps** | Logical | TrueTime (hardware) |
| **Consistency** | Serializable | External consistency |
| **Latency** | ~2ms (speculative) | ~10ms (TrueTime wait) |
| **Cost** | Open source | Cloud service |
| **Speculation** | Yes | No |

**Choose Mako when:** On-premise, latency-critical, KV sufficient.
**Choose Spanner when:** Need SQL, external consistency, managed service.

### Mako vs. DynamoDB

| Feature | Mako | DynamoDB |
|---------|------|----------|
| **Data model** | Key-value | Document/KV |
| **Transactions** | Full ACID | Limited transactions |
| **Consistency** | Strong | Eventually consistent (default) |
| **Latency** | ~2ms | ~5-10ms |
| **Scaling** | Manual sharding | Automatic |
| **Cost** | Self-hosted | Pay-per-use |

**Choose Mako when:** Need strong consistency, self-hosting.
**Choose DynamoDB when:** Want managed service, auto-scaling, AWS integration.

---

## Migration Considerations

### Migrating from Single-Node Database

**Steps:**
1. **Analyze data model**: Map tables to KV schema
2. **Design sharding**: Choose shard keys for even distribution
3. **Implement adapter**: Map existing API to Mako API
4. **Shadow testing**: Run both systems in parallel
5. **Gradual cutover**: Migrate traffic incrementally

**Example: RocksDB to Mako**
```cpp
// Before: Single-node RocksDB
rocksdb::Status s = db->Put(write_options, key, value);

// After: Mako (transparent replacement)
txn->put(key, value);
txn->commit();
```

### Migrating from Redis

**Considerations:**
- Redis data structures → Mako KV encoding
- Pub/sub needs separate solution
- Lua scripts → Application logic

**Example: Session Store**
```cpp
// Before: Redis
redis.set("session:" + id, data);
redis.expire("session:" + id, 3600);

// After: Mako
txn->put("session:" + id, data);
txn->put("session:" + id + ":expires", now + 3600);
txn->commit();
// Note: Implement TTL cleanup separately
```

### Migrating to Mako from Traditional 2PC

**Benefits:**
- 30× lower latency
- Same consistency guarantees
- Simpler failure handling

**Considerations:**
- Speculative nature means tiny window of potential loss
- May need watermark awareness for read-after-write

---

## Summary

### Mako Sweet Spot

```
┌─────────────────────────────────────────────────────────────────┐
│                      MAKO SWEET SPOT                             │
│                                                                  │
│  ✅ Key-value workloads                                         │
│  ✅ Strong consistency required                                 │
│  ✅ Low latency critical (< 10ms)                               │
│  ✅ Geo-replication needed                                      │
│  ✅ High throughput (100K+ TPS)                                 │
│  ✅ Data fits in memory (< 1TB per shard)                       │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Decision Matrix

| Use Case | Mako | Redis | PostgreSQL | CockroachDB |
|----------|------|-------|------------|-------------|
| Payment processing | ⭐⭐⭐⭐⭐ | ⭐⭐ | ⭐⭐⭐ | ⭐⭐⭐⭐ |
| Session management | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ | ⭐⭐⭐ | ⭐⭐⭐ |
| Inventory management | ⭐⭐⭐⭐⭐ | ⭐⭐ | ⭐⭐⭐⭐ | ⭐⭐⭐⭐ |
| Gaming state | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐ | ⭐⭐⭐ | ⭐⭐⭐ |
| Analytics | ⭐ | ⭐ | ⭐⭐⭐⭐ | ⭐⭐⭐ |
| Document storage | ⭐⭐ | ⭐⭐ | ⭐⭐⭐ | ⭐⭐⭐ |
| Large objects | ⭐ | ⭐ | ⭐⭐ | ⭐⭐ |

---

**Next**: [Introduction](introduction.md) | [Architecture Overview](architecture.md) | [Quick Start](quickstart.md)
