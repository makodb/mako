# Mako RocksDB-Compatible Interface

This document describes Mako's RocksDB-compatible `ITable` and `IDatabase` interfaces, located in `src/mako/idb.hh`.

---

## Interface Summary

### ITable

| Method | What it does | What it lacks | Why this implementation |
|--------|-------------|---------------|------------------------|
| `Put` | Write a key-value pair | No blind-write shortcut; value must be `mako::Encode()`'d | |
| `Get` | Read a value by key | No bloom-filter hint or short-circuit path | |
| `Delete` | Remove a key | No range delete | |
| `GetName` | Return the table name | — | |
| `Scan` | Forward range scan [start, end); returns `NotSupported` if `num_shards > 1` | No stateful iterator; cross-shard not yet implemented | Reverted to local-shard after discussion with Shuai; cross-shard support will be built on top of his upcoming changes. Multi-shard scan is non-trivial because keys are hash-distributed, not range-distributed — results across shards have no global ordering guarantee |
| `ReverseScan` | Reverse range scan descending; returns `NotSupported` if `num_shards > 1` | No stateful iterator; cross-shard not yet implemented | Same as Scan |
| `Exists` | Check key presence without reading value | Does a full Get internally; no bloom-filter hint | Chosen over a separate existence flag to reuse the OCC read-set tracking already done by Get |
| `Insert` | Insert only if key absent | Aborts transaction on duplicate | Uses `transInsert` instead of `transPut` — non-obvious distinction; `transInsert` registers the key in the OCC write-set so a concurrent insert on the same key causes abort rather than silent overwrite |
| `GetApproximateSize` | Approximate key count for the local shard | Local shard only; count may be stale | Counter updated under lock in `install()` (commit phase) rather than atomics in the hot path, as reviewer noted atomics are too expensive for an approximate metric |

### IDatabase

| Method | What it does | What it lacks | Why this implementation |
|--------|-------------|---------------|------------------------|
| `BeginTransaction` | Start a transaction, return opaque handle | No isolation-level choice | |
| `Commit` | Commit all writes in the transaction | No auto-retry on OCC abort | |
| `Rollback` | Discard all writes since `BeginTransaction` | No savepoints | |
| `GetTable` | Get or create a table proxy by name | Remote table must already exist on the server | |
| `ListTables` | List names of tables opened in this session | Only reflects tables opened via `GetTable`; not a full schema query | Remote DB has no name→table mapping, so listing all tables is not possible; local DB returns only opened tables for the same interface consistency |
| `Connect` / `Disconnect` / `IsConnected` | Connection lifecycle management | No-op for local DB | Exists on `IDatabase` so the same client code works for both local and remote without branching |
| `InitThread` | Initialize per-thread database context | Required for local DB; no-op for `RemoteDB` | |

---

## ITable API

The `ITable` interface provides key-value table operations within a transaction context.

### Put

```cpp
virtual Status Put(void* txn, const std::string& key, const std::string& value) = 0;
```

Write a key-value pair. The value **must** be encoded with `mako::Encode(value)` before passing.

**Returns:** `Status::OK()` on success.

**Example:**
```cpp
void* txn = db->BeginTransaction();
std::string encoded = mako::Encode("hello");
Status s = table->Put(txn, "mykey", encoded);
db->Commit(txn);
```

### Get

```cpp
virtual Status Get(void* txn, const std::string& key, std::string& value) = 0;
```

Read a value by key. The returned value has `EXTRA_BITS_FOR_VALUE` metadata stripped automatically.

**Returns:** `Status::OK()` on success, `Status::NotFound()` if the key does not exist.

**Example:**
```cpp
void* txn = db->BeginTransaction();
std::string value;
Status s = table->Get(txn, "mykey", value);
if (s.ok()) { /* use value */ }
db->Commit(txn);
```

### Delete

```cpp
virtual Status Delete(void* txn, const std::string& key) = 0;
```

Remove a key-value pair from the table.

**Returns:** `Status::OK()` on success.

**Example:**
```cpp
void* txn = db->BeginTransaction();
Status s = table->Delete(txn, "mykey");
db->Commit(txn);
```

### GetName

```cpp
virtual const std::string& GetName() const = 0;
```

Return the table name.

**Example:**
```cpp
std::cout << "Table: " << table->GetName() << std::endl;
```

### Scan

```cpp
virtual Status Scan(void* txn,
                    const std::string& start_key,
                    const std::string* end_key,
                    std::function<bool(const std::string& key, const std::string& value)> callback) = 0;
```

Forward range scan. Iterates keys from `start_key` (inclusive) up to `end_key` (exclusive, or until end of table if `end_key` is `nullptr`). The callback receives each key-value pair and returns `true` to continue or `false` to stop early.

Values passed to the callback are already stripped of internal metadata — no decoding needed.

**Current limitation:** Only works in single-shard deployments. Returns `Status::NotSupported` if more than one shard is present. Cross-shard scan is not yet implemented — it will be built on top of Shuai's upcoming changes. Note that when implemented, cross-shard scan cannot guarantee globally ordered results because keys are hash-distributed across shards, not range-distributed.

**Returns:** `Status::OK()` on success, `Status::NotSupported()` in multi-shard mode, `Status::InvalidArgument()` for an invalid range or null callback, `Status::IOError()` if the transaction was aborted.

**Example:**
```cpp
void* txn = db->BeginTransaction();
std::string end = "scan_key_040";
std::vector<std::string> keys;
Status s = table->Scan(txn, "scan_key_020", &end,
    [&](const std::string& key, const std::string& value) -> bool {
        keys.push_back(key);
        return true;  // continue
    });
db->Commit(txn);
// keys contains scan_key_020 .. scan_key_039 in order
```

To scan to the end of the table:
```cpp
table->Scan(txn, "prefix_", nullptr, callback);
```

### ReverseScan

```cpp
virtual Status ReverseScan(void* txn,
                           const std::string& start_key,
                           const std::string* end_key,
                           std::function<bool(const std::string& key, const std::string& value)> callback) = 0;
```

Reverse range scan. Iterates keys from `start_key` (inclusive) down to `end_key` (exclusive), delivering results in descending key order. Callback semantics are identical to `Scan`.

**Current limitation:** Same as `Scan` — returns `Status::NotSupported` in multi-shard deployments.

**Returns:** `Status::OK()` on success, `Status::NotSupported()` in multi-shard mode, `Status::InvalidArgument()` for an invalid range or null callback, `Status::IOError()` if the transaction was aborted.

**Example:**
```cpp
void* txn = db->BeginTransaction();
std::string end = "scan_key_020";
std::vector<std::string> keys;
Status s = table->ReverseScan(txn, "scan_key_039", &end,
    [&](const std::string& key, const std::string& value) -> bool {
        keys.push_back(key);
        return true;
    });
db->Commit(txn);
// keys contains scan_key_039, scan_key_038, ..., scan_key_020 in descending order
```

### Exists

```cpp
virtual Status Exists(void* txn, const std::string& key, bool* exists) = 0;
```

Check whether a key exists without reading its value (avoids unnecessary data copy). Sets `*exists = true` if found, `*exists = false` if not found.

**Returns:** `Status::OK()` even when key is absent (the result is in `*exists`). Returns an error status only if an unexpected error occurs.

**Example:**
```cpp
void* txn = db->BeginTransaction();
bool exists = false;
Status s = table->Exists(txn, "mykey", &exists);
if (s.ok() && exists) { /* key is present */ }
db->Commit(txn);
```

### Insert

```cpp
virtual Status Insert(void* txn, const std::string& key, const std::string& value) = 0;
```

Insert a key only if it does not already exist (Put-if-not-exists semantics). The value **must** be encoded with `mako::Encode(value)` before passing.

Uses the native `transInsert` path (not `transPut`), providing correct insert OCC semantics. Duplicate detection is performed via a Get check before the insert. Aborted concurrent transactions that overlap with this key may cause OCC-level retries.

**Returns:** `Status::OK()` if the key was inserted. `Status::InvalidArgument("Key already exists")` if the key is already present.

**Example:**
```cpp
void* txn = db->BeginTransaction();
std::string encoded = mako::Encode("initial_value");
Status s = table->Insert(txn, "newkey", encoded);
if (s.IsInvalidArgument()) {
    // key already exists — handle conflict
}
db->Commit(txn);
```

### GetApproximateSize

```cpp
virtual Status GetApproximateSize(size_t* size) = 0;
```

Return an approximate count of keys in the **local shard** of this table. Does not require a transaction handle.

The count is tracked via a plain `size_t` updated under lock in the commit phase (`install()`). Only reflects data in the local shard — for a cluster-wide count, a remote RPC scan would be needed (not yet implemented).

**Returns:** `Status::OK()` with `*size` set to the approximate local key count.

**Example:**
```cpp
size_t sz = 0;
Status s = table->GetApproximateSize(&sz);
if (s.ok()) {
    printf("Approximate size: %zu\n", sz);
}
```

---

## IDatabase API

The `IDatabase` interface manages transactions, table access, and connection lifecycle.

### BeginTransaction

```cpp
virtual void* BeginTransaction() = 0;
```

Begin a new transaction. Returns an opaque transaction handle to pass to table operations.

**Returns:** Transaction handle (opaque pointer), `nullptr` on failure.

### Commit

```cpp
virtual void Commit(void* txn) = 0;
```

Commit a transaction, making all writes durable and visible.

### Rollback

```cpp
virtual void Rollback(void* txn) = 0;
```

Abort a transaction, discarding all writes made since `BeginTransaction`.

### GetTable

```cpp
virtual ITable* GetTable(const std::string& name) = 0;
```

Retrieve (or create) a table by name. The returned pointer is owned by the database and valid for the lifetime of the `IDatabase` instance.

**Returns:** Pointer to `ITable`, `nullptr` on failure.

### ListTables

```cpp
virtual std::vector<std::string> ListTables() = 0;
```

Return the names of all tables currently tracked by the database (i.e., all tables previously accessed via `GetTable`). Tables that exist in the underlying store but have never been opened in this session will not appear.

**Returns:** Vector of table name strings.

**Example:**
```cpp
db->GetTable("users");
db->GetTable("orders");
db->GetTable("products");
std::vector<std::string> names = db->ListTables();
// names contains {"users", "orders", "products"} (order unspecified)
```

### Connect / Disconnect / IsConnected

```cpp
virtual Status Connect() { return Status::OK(); }
virtual void Disconnect() {}
virtual bool IsConnected() const { return true; }
```

Connection lifecycle methods. For local `mako::DB` these are no-ops (always connected). For `mako::RemoteDB` these establish/close the RPC connection.

### InitThread

```cpp
virtual void InitThread() {}
```

Initialize the current thread for database operations. Required for leader nodes before performing transactions. No-op for follower/learner nodes and remote DB.

---

## Usage Examples

### Basic CRUD

```cpp
#include "mako/mako.hh"
#include "mako/db.hh"

mako::DB* db = nullptr;
mako::Options opts;
opts.num_threads = 1;
mako::DB::Open(opts, "/tmp/mako_db", &db);

db->InitThread();
ITable* tbl = db->GetTable("my_table");

// Write
void* txn = db->BeginTransaction();
std::string enc = mako::Encode("world");
tbl->Put(txn, "hello", enc);
db->Commit(txn);

// Read
txn = db->BeginTransaction();
std::string val;
tbl->Get(txn, "hello", val);
db->Commit(txn);

delete db;
```

### Forward Scan

```cpp
void* txn = db->BeginTransaction();
std::string end_key = "key_050";
std::vector<std::pair<std::string,std::string>> results;
tbl->Scan(txn, "key_000", &end_key,
    [&](const std::string& k, const std::string& v) -> bool {
        results.emplace_back(k, v);
        return true;
    });
db->Commit(txn);
```

### Reverse Scan

```cpp
void* txn = db->BeginTransaction();
std::string end_key = "key_000";
std::vector<std::string> keys_desc;
tbl->ReverseScan(txn, "key_049", &end_key,
    [&](const std::string& k, const std::string&) -> bool {
        keys_desc.push_back(k);
        return true;
    });
db->Commit(txn);
```

### Exists Check

```cpp
void* txn = db->BeginTransaction();
bool exists = false;
tbl->Exists(txn, "some_key", &exists);
db->Commit(txn);
if (exists) { /* ... */ }
```

### Conditional Insert

```cpp
void* txn = db->BeginTransaction();
std::string enc = mako::Encode("new_value");
Status s = tbl->Insert(txn, "unique_key", enc);
if (s.IsInvalidArgument()) {
    db->Rollback(txn);  // key existed, abort
} else {
    db->Commit(txn);
}
```

### Approximate Size

```cpp
size_t sz = 0;
tbl->GetApproximateSize(&sz);
printf("~%zu keys in table\n", sz);
```

### ListTables

```cpp
auto names = db->ListTables();
for (const auto& n : names) {
    printf("  table: %s\n", n.c_str());
}
```

---

## Known Limitations vs RocksDB

| Feature | RocksDB | Mako |
|---------|---------|------|
| Iterators | Stateful `Iterator` object; seek, next, prev | Callback-based `Scan`/`ReverseScan`; no stateful iterator |
| Snapshots | `GetSnapshot()` / `ReleaseSnapshot()` | Not supported; reads see latest committed state |
| Merge operators | `Merge()` with user-defined operators | Not supported |
| Column families | Multi-CF per DB | Single index per `GetTable()` name |
| WriteBatch | Atomic multi-table batch | Not supported; use `BeginTransaction/Commit` |
| Prefix iterators / bloom filters | Configurable prefix extractors | Not supported |
| Compaction filters | Background key eviction hooks | Not supported |
| GetApproximateSize | Returns real approximate count | Local shard only; counter updated under lock at commit time (install phase) |
| Persistence | WAL + SST files | In-memory (RocksDB persistence is a separate Mako layer) |
| Range deletions | `DeleteRange()` | Not supported; delete individually |
| Transactions | Optimistic or pessimistic | Masstree-native OCC with MVCC |

---

## Migration Guide: RocksDB API → Mako Equivalents

| RocksDB | Mako | Notes |
|---------|------|-------|
| `DB::Open(opts, path, &db)` | `mako::DB::Open(opts, path, &db)` | Options struct differs; no column families |
| `db->Put(wo, key, value)` | `table->Put(txn, key, mako::Encode(value))` | Encode value; use txn handle |
| `db->Get(ro, key, &value)` | `table->Get(txn, key, value)` | Value is decoded automatically |
| `db->Delete(wo, key)` | `table->Delete(txn, key)` | Needs txn |
| `db->NewIterator(ro)` → seek/next | `table->Scan(txn, start, end, cb)` | Replace stateful iterator with callback |
| Reverse iterator | `table->ReverseScan(txn, start, end, cb)` | Callback-based |
| `db->KeyMayExist(...)` | `table->Exists(txn, key, &exists)` | Exact check, not bloom filter hint |
| `db->Merge(wo, key, value)` | Not available | No merge operators |
| `db->GetSnapshot()` | Not available | No snapshot isolation |
| `db->GetColumnFamilyHandle(name)` | `db->GetTable(name)` | Returns `ITable*` |
| `db->DefaultColumnFamily()` | `db->GetTable("default")` or any name | |
| `db->ListColumnFamilies(...)` | `db->ListTables()` | Only lists opened tables |
| `WriteBatch` | Multiple `Put`/`Delete` in one `BeginTransaction/Commit` | |
| `db->Close()` | `delete db` (destructor calls Close) | |
