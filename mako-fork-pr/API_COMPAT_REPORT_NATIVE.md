# Mako Native API Compatibility Report (vs RocksDB)

**Date:** 2026-03-18
**Mako Commit:** `c68e2136` (branch `mako-dev`)
**Analysis Method:** Source code review of `src/mako/db.hh`, `src/mako/idb.hh`, `src/mako/benchmarks/mbta_wrapper.hh`, `src/mako/benchmarks/sto/MassTrans.hh`, config files
**Host OS:** Linux 5.15.0-133-generic (x86_64)

---

## Executive Summary

| Metric | Count |
|--------|-------|
| RocksDB features evaluated | 35 |
| Mako equivalent exists | **10** |
| Partial equivalent | **5** |
| Not supported | **20** |
| **RocksDB API Compatibility Score** | **28.6%** |

**Mako is NOT a drop-in RocksDB replacement.** It is a distributed transactional database that uses RocksDB internally for log persistence. The two systems solve different problems at different layers:

- **RocksDB:** Low-level embedded key-value storage engine (iterators, snapshots, column families, compaction)
- **Mako:** High-level distributed transactional database (ACID transactions, Paxos/Raft replication, cross-shard RPC, OCC)

Mako provides capabilities RocksDB lacks (distributed transactions, replication, sharding) but lacks many RocksDB primitives (iterators, snapshots, column families, merge operators, compaction control).

---

## R1.1: Core API Surface Comparison

### Mako's Public API (from source analysis)

#### `mako::DB` (`src/mako/db.hh`)

| Method | Signature | RocksDB Equivalent |
|--------|-----------|-------------------|
| `Open` | `static Status Open(const Options&, const string&, DB**)` | `DB::Open()` ✅ |
| `Close` | `Status Close()` | `DB::Close()` ✅ |
| `IsOpen` | `bool IsOpen() const` | — (no equivalent) |
| `GetDB` | `abstract_db* GetDB()` | — (internal) |
| `InitThread` | `void InitThread()` | — (RocksDB uses implicit TLS) |
| `BeginTransaction` | `void* BeginTransaction()` | `TransactionDB::BeginTransaction()` ✅ |
| `Commit` | `void Commit(void* txn)` | `Transaction::Commit()` ✅ |
| `Rollback` | `void Rollback(void* txn)` | `Transaction::Rollback()` ✅ |
| `GetTable` | `ITable* GetTable(const string&)` | `DB::GetColumnFamily()` (partial) |
| `Connect` | `Status Connect()` | — (RocksDB is embedded) |
| `Disconnect` | `void Disconnect()` | — (RocksDB is embedded) |

#### `ITable` (`src/mako/idb.hh`)

| Method | Signature | RocksDB Equivalent |
|--------|-----------|-------------------|
| `Put` | `Status Put(void* txn, const string& key, const string& value)` | `DB::Put()` / `Transaction::Put()` ✅ |
| `Get` | `Status Get(void* txn, const string& key, string& value)` | `DB::Get()` / `Transaction::Get()` ✅ |
| `Delete` | `Status Delete(void* txn, const string& key)` | `DB::Delete()` / `Transaction::Delete()` ✅ |
| `GetName` | `const string& GetName() const` | — |

#### `abstract_ordered_index` (`src/mako/benchmarks/abstract_ordered_index.h`)

| Method | Description | RocksDB Equivalent |
|--------|-------------|-------------------|
| `get()` | Transactional key lookup | `Transaction::Get()` ✅ |
| `put()` | Transactional key write | `Transaction::Put()` ✅ |
| `insert()` | Optimized insert (key must not exist) | — (no conditional insert) |
| `remove()` | Transactional key delete | `Transaction::Delete()` ✅ |
| `scan()` | Forward range scan with callback | `Iterator` (partial) |
| `rscan()` | Reverse range scan with callback | `Iterator::Prev()` (partial) |
| `put_mbta()` | Conditional put with comparator | — (unique to Mako) |
| `size()` | Return index size | `DB::GetProperty("rocksdb.estimate-num-keys")` (partial) |
| `clear()` | Clear all data | — |

---

## R1.2: Iterator Support

### Finding: **Callback-based range queries, NOT iterator-based**

Mako provides `scan()` and `rscan()` methods that use a callback pattern:

```cpp
// Mako scan API
class scan_callback {
    virtual bool invoke(const char *keyp, size_t keylen, const string &value) = 0;
};
void scan(void *txn, const string &start_key, const string *end_key,
          scan_callback &callback, str_arena *arena);
```

**vs RocksDB iterator API:**
```cpp
// RocksDB iterator API
Iterator* it = db->NewIterator(read_options);
for (it->Seek(start); it->Valid() && it->key() < end; it->Next()) {
    // Process it->key(), it->value()
}
```

| Feature | Mako | RocksDB |
|---------|------|---------|
| Forward scan | `scan()` callback | `Iterator::Seek()` + `Next()` |
| Reverse scan | `rscan()` callback | `Iterator::SeekForPrev()` + `Prev()` |
| Random access | Not supported | `Iterator::Seek(key)` |
| Hold position | Not supported | Iterator persists position |
| Cancel mid-scan | Return `false` from callback | Caller controls loop |
| Transactional | Yes (within OCC transaction) | Via `ReadOptions::snapshot` |

**Gap: Medium.** Mako's callback-based scans are functionally equivalent for most use cases but lack the flexibility of stateful iterators. A RocksDB user who relies on iterator positioning, prefix seeks, or holding an iterator across multiple operations would need to restructure their code.

---

## R1.3: Batch Write Support

### Finding: **Transactions replace WriteBatch**

| Feature | RocksDB WriteBatch | Mako Transaction |
|---------|-------------------|------------------|
| Atomic multi-key writes | Yes | Yes |
| Read within batch | **No** | **Yes** (full RMW) |
| Isolation from concurrent reads | No (readers may see partial) | Yes (OCC snapshot) |
| Conditional writes | No | Yes (`put_mbta` comparator) |
| Performance | Faster (no validation) | Slower (OCC validation overhead) |
| Abort/rollback | No (apply or don't) | Yes (`abort_txn()`) |

**Gap: Easy.** Mako's transactions are a superset of WriteBatch — they do everything WriteBatch does plus reads and conditional writes. A RocksDB `WriteBatch` user can directly use `begin_txn()` + multiple `Put()` calls + `commit_txn()`. The only difference is performance: OCC validation adds overhead that WriteBatch doesn't have.

---

## R1.4: Snapshot Support

### Finding: **NOT SUPPORTED**

| Feature | RocksDB | Mako |
|---------|---------|------|
| `GetSnapshot()` | Yes — returns point-in-time snapshot | **No** |
| `ReadOptions::snapshot` | Yes — consistent reads at snapshot time | **No** |
| `ReleaseSnapshot()` | Yes | **No** |
| Snapshot across threads | Yes | **No** |
| Long-lived snapshots | Yes (persists until released) | **No** |

**Mako's alternative:** OCC transactions provide snapshot-like consistency within a single transaction's lifetime. But snapshots cannot be held across transactions, shared between threads, or persisted.

**Gap: Hard.** No workaround exists for use cases requiring long-lived, cross-transaction snapshots (e.g., backup, analytics queries, CDC).

---

## R1.5: Column Family Support

### Finding: **Tables as partial equivalent**

| Feature | RocksDB Column Families | Mako Tables |
|---------|------------------------|-------------|
| Logical data separation | Yes | Yes (via `open_index("name")`) |
| Independent options | Yes (per-CF compression, compaction) | **No** (all tables share config) |
| Atomic cross-CF writes | Yes (single WriteBatch) | Yes (single OCC transaction) |
| Create at runtime | Yes (`CreateColumnFamily()`) | Yes (`open_index()` creates on first use) |
| Drop at runtime | Yes (`DropColumnFamily()`) | **No** |
| List existing | Yes (`ListColumnFamilies()`) | **No** |

**Gap: Medium.** Mako's tables provide basic logical separation but lack per-table configuration, runtime deletion, and enumeration.

---

## R1.6: Merge Operator Support

### Finding: **NOT SUPPORTED**

| Feature | RocksDB | Mako |
|---------|---------|------|
| `DB::Merge(key, value)` | Yes | **No** |
| Custom `MergeOperator` | Yes (associative, full) | **No** |
| Atomic increment | Via merge operator | Via OCC read-modify-write |
| Append to value | Via merge operator | Via OCC read-modify-write |

**Mako's alternative:** OCC transactions achieve atomic read-modify-write naturally:
```cpp
begin_txn();
Get(key, &val);
val = modify(val);
Put(key, val);
commit_txn();  // Aborts if key was modified concurrently
```

**Gap: Medium.** OCC RMW works but has higher overhead than merge operators (requires read + write + validation vs single write). High-contention counters will have high abort rates with Mako's approach vs zero contention with merge operators.

---

## R1.7: Compaction and Maintenance

### Finding: **NOT SUPPORTED (different architecture)**

| Feature | RocksDB | Mako |
|---------|---------|------|
| `CompactRange()` | Yes | **No** (Masstree doesn't compact) |
| `GetProperty()` stats | Yes (100+ properties) | **No** |
| `GetApproximateSizes()` | Yes | **No** |
| Background compaction | Yes (configurable) | N/A (in-memory Masstree) |
| LSM-tree levels | Yes (tunable) | N/A |
| Block cache | Yes (configurable) | N/A (data is in-memory) |

**Gap: N/A (architectural difference).** Masstree is an in-memory B-tree, not an LSM-tree. Compaction is a RocksDB-specific concern. Mako's RocksDB usage is for log persistence only (append-only), not as the primary data store.

---

## R1.8: Configuration and Options

### RocksDB Options vs Mako Options

| RocksDB Option | Mako Equivalent |
|---------------|-----------------|
| `write_buffer_size` | N/A (Masstree in-memory) |
| `max_open_files` | N/A |
| `compression` | N/A (in-memory) |
| `num_levels` | N/A (not LSM) |
| `max_write_buffer_number` | N/A |
| `target_file_size_base` | N/A |
| `sync` (WriteOptions) | Not exposed |
| `disableWAL` | `DISABLE_DISK` build flag |
| `verify_checksums` | N/A |

### Mako-Specific Options

| Option | Description | RocksDB Equivalent |
|--------|-------------|-------------------|
| `num_threads` | Worker thread count | N/A |
| `num_shards` | Data partitions | N/A (single-node) |
| `shard_index` | This node's shard | N/A |
| `replication.enabled` | Paxos/Raft replication | N/A |
| `replication.type` | "paxos" or "raft" | N/A |
| `replication.is_leader` | Leader/follower role | N/A |
| `client.server_hosts` | Remote shard addresses | N/A |
| `config_file` | YAML config path | N/A |

**Gap: Easy for basic usage, Hard for tuning.** RocksDB's extensive tuning knobs (100+ options) don't apply to Mako's in-memory architecture. This is an architectural difference, not a missing feature.

---

## R1.9: Transaction API Comparison

### RocksDB TransactionDB vs Mako

| Feature | RocksDB TransactionDB | Mako |
|---------|----------------------|------|
| `BeginTransaction()` | Yes | Yes (`new_txn()` / `BeginTransaction()`) |
| `Transaction::Put()` | Yes | Yes |
| `Transaction::Get()` | Yes | Yes |
| `Transaction::Delete()` | Yes | Yes |
| `Transaction::Commit()` | Yes | Yes (`commit_txn()`) |
| `Transaction::Rollback()` | Yes | Yes (`abort_txn()`) |
| `GetForUpdate()` | Yes (pessimistic lock) | **No** (OCC only) |
| `SetSavepoint()` | Yes | **No** |
| `RollbackToSavepoint()` | Yes | **No** |
| Transaction timeout | Yes (configurable) | **No** |
| Deadlock detection | Yes (for pessimistic) | N/A (OCC has no deadlocks) |
| Isolation level | Configurable | **Serializable only** (OCC) |
| Two-phase commit | Yes (`Prepare()`) | **No** (internal 2PC for cross-shard) |
| Cross-shard atomicity | **No** | **Yes** (built-in) |

**Gap: Medium.** Mako's transaction API covers the basic CRUD operations but lacks savepoints, pessimistic locking, configurable isolation levels, and user-visible two-phase commit. However, Mako provides cross-shard transactions that RocksDB TransactionDB cannot do.

---

## R1.10: Complete Gap Analysis

### API Compatibility Matrix

| RocksDB Feature | Mako Status | Migration Difficulty |
|-----------------|-------------|---------------------|
| **Core CRUD** | | |
| `DB::Open()` | ✅ Supported | Easy |
| `DB::Close()` | ✅ Supported | Easy |
| `DB::Put()` | ✅ Supported | Easy |
| `DB::Get()` | ✅ Supported | Easy |
| `DB::Delete()` | ✅ Supported | Easy |
| `DB::DestroyDB()` | ❌ Not supported | Medium |
| **Iterators** | | |
| `NewIterator()` | ⚠️ Callback-based `scan()` | Medium |
| `Iterator::Seek()` | ⚠️ Via `scan(start_key, ...)` | Medium |
| `Iterator::Next()/Prev()` | ⚠️ Implicit in callback | Medium |
| `Iterator::Valid()` | ❌ No stateful iterator | Medium |
| **Batch Operations** | | |
| `WriteBatch` | ✅ Via transactions (superset) | Easy |
| `DB::Write(batch)` | ✅ Via `commit_txn()` | Easy |
| **Snapshots** | | |
| `GetSnapshot()` | ❌ Not supported | Hard |
| `ReadOptions::snapshot` | ❌ Not supported | Hard |
| `ReleaseSnapshot()` | ❌ Not supported | Hard |
| **Column Families** | | |
| `CreateColumnFamily()` | ⚠️ Via `open_index()` | Medium |
| `DropColumnFamily()` | ❌ Not supported | Medium |
| `ListColumnFamilies()` | ❌ Not supported | Medium |
| **Merge** | | |
| `DB::Merge()` | ❌ Not supported | Medium |
| Custom `MergeOperator` | ❌ Not supported | Hard |
| **Transactions** | | |
| `BeginTransaction()` | ✅ Supported | Easy |
| `Commit()` | ✅ Supported | Easy |
| `Rollback()` | ✅ Supported | Easy |
| `GetForUpdate()` | ❌ Not supported (OCC only) | Medium |
| `SetSavepoint()` | ❌ Not supported | Medium |
| Transaction timeout | ❌ Not supported | Medium |
| **Compaction/Maintenance** | | |
| `CompactRange()` | ❌ N/A (in-memory) | N/A |
| `GetProperty()` | ❌ Not supported | Easy |
| `GetApproximateSizes()` | ❌ Not supported | Easy |
| **Configuration** | | |
| WriteOptions (sync, WAL) | ❌ Not exposed | Medium |
| ReadOptions (checksums) | ❌ N/A (in-memory) | N/A |
| Compression | ❌ N/A (in-memory) | N/A |
| **Mako-Only Features** | | |
| Cross-shard transactions | ✅ Mako only | — |
| Paxos/Raft replication | ✅ Mako only | — |
| Built-in sharding | ✅ Mako only | — |
| Conditional put (comparator) | ✅ Mako only | — |
| OCC serializable isolation | ✅ Mako only | — |

### Score Summary

| Category | Supported | Partial | Unsupported | N/A |
|----------|-----------|---------|-------------|-----|
| Core CRUD (6) | 5 | 0 | 1 | 0 |
| Iterators (4) | 0 | 3 | 1 | 0 |
| Batch (2) | 2 | 0 | 0 | 0 |
| Snapshots (3) | 0 | 0 | 3 | 0 |
| Column Families (3) | 0 | 1 | 2 | 0 |
| Merge (2) | 0 | 0 | 2 | 0 |
| Transactions (6) | 3 | 0 | 3 | 0 |
| Compaction (3) | 0 | 0 | 1 | 2 |
| Configuration (3) | 0 | 0 | 1 | 2 |
| **Total (35)** | **10** | **5** | **20** | — |

**Compatibility Score: (10 + 5×0.5) / 35 × 100 = 35.7%** (counting partial as 0.5)

---

## Honest Assessment

### "Distributed RocksDB Alternative" Claim

**Verdict: Misleading if interpreted as API-compatible; Accurate if interpreted as role-compatible.**

- ❌ Mako is NOT a drop-in replacement for RocksDB at the API level (35.7% compatibility)
- ❌ A RocksDB application cannot run on Mako without significant code changes
- ✅ Mako fills the same **role** as RocksDB (persistent KV store) with **additional capabilities** (distribution, transactions)
- ✅ Mako's core CRUD operations (Open, Put, Get, Delete, Close) match RocksDB's basic interface

**Better framing:** "Mako is a distributed transactional database built on Masstree (in-memory) with RocksDB-backed log persistence." It is NOT an API-compatible alternative to RocksDB — it is a higher-level system that happens to use RocksDB internally.

### "Redis Alternative with Transactions" Claim (cross-reference)

From the Redis API compatibility report: **7.7% command compatibility** (7/91 commands). makoCon implements only PING, SET, GET, DEL, MULTI, EXEC, DISCARD. This is even less compatible than the native API's RocksDB comparison.

**Better framing:** "makoCon exposes a Redis-protocol interface for basic transactional key-value operations." It is NOT a Redis alternative for users who need data structures, pub/sub, scripting, or cache semantics.

### What a RocksDB User Can Do with Mako Today

**Without code changes:** Nothing. The API signatures differ.

**With minimal changes (Easy migration):**
- Basic Put/Get/Delete operations
- Multi-key atomic writes (WriteBatch → transactions)
- Open/Close lifecycle

**Requiring restructuring (Medium migration):**
- Range scans (iterators → callbacks)
- Column families (→ named tables)
- Merge operators (→ OCC read-modify-write transactions)
- Pessimistic locking (→ OCC with retry)

**Not possible (Hard/impossible migration):**
- Long-lived snapshots for backup/analytics
- Compaction tuning (different architecture)
- Custom merge operators with server-side logic
- Configurable isolation levels

---

## Recommendations

1. **Reframe marketing claims:** "Distributed transactional database" is more accurate than "Distributed RocksDB Alternative." The API surface is fundamentally different.

2. **Add snapshot support:** This is the most impactful missing feature for RocksDB migration. Snapshots enable backup, analytics, and CDC use cases.

3. **Add iterator API wrapper:** Wrap `scan()` callbacks in a C++ iterator interface for easier migration from RocksDB codebases.

4. **Expose RocksDB write options:** Allow users to control sync-on-commit for durability tuning.

5. **Add `EXISTS` operation:** Both the native and Redis APIs lack a simple key-existence check without fetching the full value.

6. **Document the architectural difference:** A migration guide explaining "Mako is not RocksDB — here's what's different and why" would help users set correct expectations.

---

## Test File Location

This report is based on source code analysis, not runtime testing. The native concurrency tests (`examples/nativeConcurrencyTest.cc`) validate the transaction API behavior. The Redis API compatibility tests (`tests/correctness/test_api_compat.py`) validate the Redis command surface.
