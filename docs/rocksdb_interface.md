# Mako RocksDB-Compatible Interface

This document describes Mako's RocksDB-compatible `ITable` and `IDatabase` interfaces, located in `src/mako/idb.hh`. These interfaces allow writing code that works with both local (`mako::DB`) and remote (`mako::RemoteDB`) database implementations.

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

**Returns:** `Status::OK()` on success, `Status::IOError()` if the transaction was aborted.

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

**Returns:** `Status::OK()` on success, `Status::IOError()` if the transaction was aborted.

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

Return an approximate count of keys in the table. Does not require a transaction handle.

The count is maintained via an atomic counter (`std::atomic<size_t>`) updated at operation time (not commit time). Aborted transactions may temporarily skew the count — hence "approximate".

**Returns:** `Status::OK()` with `*size` set to the approximate key count.

**Example:**
```cpp
size_t sz = 0;
Status s = table->GetApproximateSize(&sz);
if (s.ok()) {
    printf("Approximate size: %zu\n", sz);
}
```

### NewIterator

```cpp
virtual IIterator* NewIterator(void* txn) = 0;
```

Create a stateful iterator for this table. The returned iterator supports `Seek`, `SeekToFirst`, `SeekToLast`, `Next`, `Prev`, `Valid`, `key`, and `value` — matching the RocksDB Iterator API.

The caller owns the returned iterator and **must** `delete` it when done.

See the [IIterator API](#iiterator-api) section below for full method signatures.

**Returns:** Pointer to a new `IIterator`. Never `nullptr` for local tables.

**Example:**
```cpp
auto* it = table->NewIterator(txn);
for (it->SeekToFirst(); it->Valid(); it->Next()) {
    std::cout << it->key() << " = " << it->value() << std::endl;
}
delete it;
```

### DeleteRange

```cpp
virtual Status DeleteRange(void* txn,
                           const std::string& start_key,
                           const std::string* end_key,
                           size_t* deleted_count = nullptr) = 0;
```

Delete all keys in the range `[start_key, end_key)`. The range is **start-inclusive, end-exclusive**. If `end_key` is `nullptr`, all keys >= `start_key` are deleted (no upper bound).

The operation is **atomic** within the enclosing transaction: all deletes are part of the same OCC transaction passed in via `txn`. If the caller aborts/rolls back that transaction, none of the deletes take effect.

`deleted_count` (optional) is set to the number of keys successfully deleted. On partial failure it reflects how many were deleted before the error.

**Returns:** `Status::OK()` on success, `Status::IOError()` if the operation fails. For `RemoteTable`, always returns `Status::IOError("DeleteRange not supported on remote table")`.

**Behavior summary:**

| Condition | Behavior |
|-----------|----------|
| `start_key == end_key` | Empty range; deletes 0 keys |
| No keys in range | Returns `OK`, `deleted_count = 0` |
| `end_key == nullptr` | Deletes all keys >= `start_key` |
| Transaction abort | All deletes rolled back (atomicity) |

**Examples:**

Delete a range of keys:
```cpp
void* txn = db->BeginTransaction();
std::string end = "key_040";
size_t deleted = 0;
Status s = table->DeleteRange(txn, "key_020", &end, &deleted);
db->Commit(txn);
// keys key_020 through key_039 are gone; deleted == 20
```

Delete all keys >= a prefix:
```cpp
void* txn = db->BeginTransaction();
size_t deleted = 0;
Status s = table->DeleteRange(txn, "prefix_", nullptr, &deleted);
db->Commit(txn);
```

Delete entire table contents:
```cpp
void* txn = db->BeginTransaction();
size_t deleted = 0;
Status s = table->DeleteRange(txn, "", nullptr, &deleted);
db->Commit(txn);
// table is now empty; deleted == previous key count
```

**Limitations vs RocksDB `DeleteRange`:**

| Aspect | RocksDB | Mako |
|--------|---------|------|
| Write cost | O(1) — single tombstone record written to WAL/SST | O(N) — scans and deletes each key individually |
| Cleanup | Lazy; during compaction | Immediate; keys are deleted at commit time |
| Large ranges | Fast write, slow compaction | Slow write (linear in N), no compaction needed |
| Space reclamation | Deferred until compaction | Immediate |

For very large ranges (millions of keys), Mako's scan-then-delete approach is slower than RocksDB's tombstone strategy. For typical workloads (hundreds to thousands of keys), the difference is negligible.

---

## IIterator API

The `IIterator` interface (defined in `src/mako/idb.hh`) provides RocksDB-compatible stateful iteration over a table's key-value pairs.

The concrete implementation is `mako::BufferedIterator` (`src/mako/buffered_iterator.hh`), which buffers scan results at `Seek` time. See [Implementation Approach](#implementation-approach) below.

### Method Signatures

```cpp
class IIterator {
public:
    virtual ~IIterator() = default;

    // Position the iterator at the first key >= target
    virtual void Seek(const std::string& target) = 0;

    // Position the iterator at the first key
    virtual void SeekToFirst() = 0;

    // Position the iterator at the last key
    virtual void SeekToLast() = 0;

    // Move to the next key
    virtual void Next() = 0;

    // Move to the previous key
    virtual void Prev() = 0;

    // Returns true if the iterator is positioned at a valid entry
    virtual bool Valid() const = 0;

    // Return the key at the current position (only valid when Valid() == true)
    virtual std::string key() const = 0;

    // Return the value at the current position (only valid when Valid() == true)
    virtual std::string value() const = 0;

    // Return the status of the iterator (OK if no errors)
    virtual Status status() const = 0;
};
```

### Usage Examples

#### Forward Iteration

```cpp
auto* it = table->NewIterator(txn);
for (it->SeekToFirst(); it->Valid(); it->Next()) {
    std::cout << it->key() << " = " << it->value() << std::endl;
}
delete it;
```

#### Range Scan with Prefix

```cpp
auto* it = table->NewIterator(txn);
for (it->Seek("prefix_"); it->Valid() && it->key().starts_with("prefix_"); it->Next()) {
    // process matching keys
}
delete it;
```

#### Reverse Iteration

```cpp
auto* it = table->NewIterator(txn);
for (it->SeekToLast(); it->Valid(); it->Prev()) {
    // process in reverse order
}
delete it;
```

#### Bounded Seek

```cpp
auto* it = table->NewIterator(txn);
it->Seek("key_050");
if (it->Valid()) {
    std::cout << "First key >= key_050: " << it->key() << std::endl;
}
delete it;
```

#### Via IDatabase convenience method

```cpp
// No need to call GetTable first
auto* it = db->NewIterator(txn, "my_table");
if (it) {
    for (it->SeekToFirst(); it->Valid(); it->Next()) { ... }
    delete it;
}
```

### Implementation Approach

`BufferedIterator` uses a **buffered scan** strategy:

1. On `Seek()`, `SeekToFirst()`, or `SeekToLast()`, a single `table->Scan()` (or `ReverseScan()`) call populates a `std::vector<std::pair<std::string,std::string>>` with up to `max_buffer_size` entries.
2. `Next()` and `Prev()` move an integer cursor through the in-memory buffer.
3. `Valid()`, `key()`, and `value()` read from the current cursor position.

**Why buffered?** Mako's Masstree scan is callback-based and executes within a single OCC transaction. A true lazy iterator that pauses mid-scan and resumes later requires significant Masstree changes. The buffered approach is compatible with the existing scan infrastructure and covers the common case (scanning hundreds to thousands of keys).

**Memory trade-off:** The entire result set up to `max_buffer_size` (default: 10,000) is held in memory. For tables with millions of keys, use a smaller `max_buffer_size` and paginate with successive `Seek()` calls.

**Snapshot semantics:** Each call to `Seek`/`SeekToFirst`/`SeekToLast` runs a fresh scan and repopulates the buffer. Writes made between two `Seek` calls will be visible in the new buffer. The buffer itself is static — writes made *after* `Seek` but *before* iterating through the results are not reflected.

### Limitations vs RocksDB Iterators

| Feature | RocksDB | Mako BufferedIterator |
|---------|---------|----------------------|
| Seek laziness | Lazy (O(log N) seek) | Eager — entire scan runs at Seek time |
| Memory | O(1) cursor | O(buffer\_size) buffer |
| Reflect writes after Seek | Yes (live cursor into LSM) | No (snapshot at Seek time) |
| `SeekForPrev()` | Supported | Not implemented; approximate with `ReverseScan` |
| Prefix bloom filters | Configurable | Not supported |
| Large scans | Streaming | Use `max_buffer_size` + paginated Seeks |
| Multi-table / column family | Per-CF iterators | Per-`ITable` iterators only |

---

## IWriteBatch API

`IWriteBatch` (defined in `src/mako/idb.hh`) provides a RocksDB-compatible write-only atomic batch. Operations are queued in memory and applied atomically via a single OCC transaction on `Write()`. The concrete implementation is `mako::WriteBatch` (`src/mako/write_batch.hh`).

**Create a batch via `IDatabase::NewWriteBatch()`:**

```cpp
mako::IWriteBatch* batch = db->NewWriteBatch();
// ... queue operations ...
mako::Status s = batch->Write();
delete batch;
```

### Put

```cpp
virtual void Put(const std::string& table_name,
                 const std::string& key,
                 const std::string& value) = 0;
```

Queue a Put operation. The value is stored as-is (no pre-encoding required — `WriteBatch::Write()` encodes all values automatically before opening the transaction).

**Example:**
```cpp
batch->Put("users", "user:42", "Alice");
batch->Put("users", "user:43", "Bob");
```

### Delete

```cpp
virtual void Delete(const std::string& table_name,
                    const std::string& key) = 0;
```

Queue a Delete operation.

**Example:**
```cpp
batch->Delete("users", "user:99");
```

### DeleteRange

```cpp
virtual void DeleteRange(const std::string& table_name,
                         const std::string& start_key,
                         const std::string& end_key) = 0;
```

Queue a DeleteRange operation. Deletes all keys in `[start_key, end_key)` (start-inclusive, end-exclusive) when `Write()` is called.

**Example:**
```cpp
batch->DeleteRange("events", "event:2024-01-01", "event:2025-01-01");
```

### Count

```cpp
virtual size_t Count() const = 0;
```

Return the number of queued operations (Put + Delete + DeleteRange combined). Resets to 0 after a successful `Write()` or explicit `Clear()`.

### Clear

```cpp
virtual void Clear() = 0;
```

Discard all queued operations without applying them.

### Write

```cpp
virtual Status Write() = 0;
```

Apply all queued operations atomically in a single OCC transaction.

**Semantics:**
- Begins a transaction (`BeginTransaction`)
- Applies all queued ops in order: `Put` → `table->Put`, `Delete` → `table->Delete`, `DeleteRange` → `table->DeleteRange`
- Commits the transaction
- On success: clears the batch and returns `Status::OK()`
- On any op failure: rolls back the entire transaction and returns an error status
- On OCC abort (commit throws): rolls back and returns `Status::IOError("WriteBatch::Write: transaction aborted (OCC conflict)")`
- Empty batch: returns `Status::OK()` immediately (no transaction opened)

**Returns:** `Status::OK()` on success, error status on failure. All-or-nothing: either every operation commits or none do.

---

### Usage Examples

#### Basic Bulk Insert

```cpp
mako::IWriteBatch* batch = db->NewWriteBatch();
for (int i = 0; i < 1000; ++i) {
    batch->Put("orders", "order:" + std::to_string(i), serialize(order[i]));
}
mako::Status s = batch->Write();
if (!s.ok()) {
    // entire batch failed — handle error
}
delete batch;
```

#### Mixed Put / Delete / DeleteRange

```cpp
mako::IWriteBatch* batch = db->NewWriteBatch();
batch->Put("inventory", "item:new_product", "qty:100");
batch->Delete("inventory", "item:discontinued_sku");
batch->DeleteRange("sessions", "sess:2024-01-", "sess:2025-01-");  // purge old sessions
mako::Status s = batch->Write();
delete batch;
```

#### Cross-Table Atomic Batch

```cpp
// Transfer units atomically across two tables
mako::IWriteBatch* batch = db->NewWriteBatch();
batch->Put("accounts", "acct:alice", "balance:90");   // debit
batch->Put("accounts", "acct:bob",   "balance:110");  // credit
batch->Put("ledger",   "txn:001",    "debit:alice:10");
mako::Status s = batch->Write();  // all three commit or none do
delete batch;
```

#### Reusable Batch (multiple Write() calls)

```cpp
mako::IWriteBatch* batch = db->NewWriteBatch();

// First wave
for (int i = 0; i < 100; ++i) batch->Put("log", "log:" + std::to_string(i), "v");
batch->Write();  // applies 100 ops, then auto-clears

// Second wave
for (int i = 100; i < 200; ++i) batch->Put("log", "log:" + std::to_string(i), "v");
batch->Write();  // applies next 100 ops

delete batch;
```

---

### Behavior Summary

| Property | Value |
|----------|-------|
| Atomicity | All-or-nothing: all ops commit or all roll back |
| Read isolation | Write-only: no reads within a batch |
| Auto-clear | Batch clears automatically after successful `Write()` |
| OCC abort | Entire batch fails; retry the batch |
| Thread safety | One batch per thread; `WriteBatch` objects are not shared |
| Cross-table | Ops across multiple tables are committed in one transaction |
| Empty batch | `Write()` returns `OK` immediately (no transaction started) |

---

### Limitations vs RocksDB WriteBatch

| Aspect | RocksDB | Mako |
|--------|---------|------|
| Write cost | O(1) WAL append; no OCC overhead | Full OCC transaction; validation + lock step at commit |
| Large batches | Always succeed (append-only LSM) | May OCC-abort on contended keys; retry required |
| WAL group commit | Multiple batches can share one WAL write | Not supported |
| `Merge` operations | Supported via `WriteBatch::Merge()` | Not supported |
| `PutLogData` | Log-only entry in WAL | Not supported |
| Read-your-own-writes | `WriteBatchWithIndex` provides this | Not supported; WriteBatch is write-only |
| Persistence | Written to WAL and SST files | In-memory OCC; persistence via separate RocksDB layer |

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

Initialize the current thread for database operations. Must be called once per thread (including worker threads) before performing any transactions. Assigns a unique thread ID and initializes Masstree per-thread state (`mythreadinfo.ti`). Safe to call multiple times; only the first call per thread has effect. No-op for `mako::RemoteDB`.

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

## ISnapshot — Point-in-Time Consistent Reads

Mako supports a RocksDB-style snapshot API via `ISnapshot`. See
`docs/snapshot_design.md` for the full feasibility analysis.

### Creating and Releasing a Snapshot

```cpp
// Capture snapshot (O(1) — no data is copied yet)
mako::ISnapshot* snap = db->GetSnapshot();

// Read keys — table is scanned lazily on first access
std::string val;
mako::Status s = snap->Get("my_table", "key1", val);

// Release when done — frees all buffered data
db->ReleaseSnapshot(snap);
```

### Reading Consistent Data Under Concurrent Writes

```cpp
// All data is buffered at first-access time for each table.
// Writes that happen after the first access to a table are not visible.
mako::ISnapshot* snap = db->GetSnapshot();

// Force the snapshot buffer to be populated NOW (before any writes)
{ std::string v; snap->Get("orders", "order_0001", v); }

// Now spawn writers — snapshot is stable
// ... concurrent writes happen ...

// Still sees the original data
snap->Get("orders", "order_0001", val);  // original value
db->ReleaseSnapshot(snap);
```

### Snapshot Iterator

```cpp
mako::ISnapshot* snap = db->GetSnapshot();
mako::IIterator* it = snap->NewIterator("inventory");
for (it->SeekToFirst(); it->Valid(); it->Next()) {
    printf("%s -> %s\n", it->key().c_str(), it->value().c_str());
}
delete it;
db->ReleaseSnapshot(snap);
```

### Snapshot Sequence Numbers

```cpp
mako::ISnapshot* snap1 = db->GetSnapshot();
// ... writes ...
mako::ISnapshot* snap2 = db->GetSnapshot();
assert(snap2->GetSequenceNumber() > snap1->GetSequenceNumber());
```

### ISnapshot API Reference

```cpp
class ISnapshot {
public:
    // Read a key — returns NotFound if absent at capture time
    virtual Status Get(const std::string& table_name,
                       const std::string& key,
                       std::string& value) = 0;

    // Check key existence
    virtual Status Exists(const std::string& table_name,
                          const std::string& key,
                          bool* exists) = 0;

    // Create an iterator over the snapshot's view of the table
    // Caller owns and must delete the returned iterator
    virtual IIterator* NewIterator(const std::string& table_name) = 0;

    // Monotonically increasing ID (not tied to a global LSN)
    virtual uint64_t GetSequenceNumber() const = 0;
};
```

### Snapshot Limitations vs RocksDB

| Property | RocksDB Snapshot | Mako EagerSnapshot |
|----------|------------------|--------------------|
| Creation cost | O(1), atomic (pin SST files) | O(1) creation, O(N) on first table access |
| Memory cost | O(0) — immutable SST files stay alive | O(N) per buffered table |
| Cross-table consistency | Yes (global sequence number) | **No** — per-table; each table is captured independently |
| Reads after creation | Always return captured values | Returns values captured at **first access per table** |
| Writer interference | None (immutable files) | None (buffer is immutable after scan) |
| Use-after-free guard | N/A | No guard — do not use pointer after `ReleaseSnapshot()` |
| RemoteDB support | Yes | Returns `nullptr` (not supported) |

**Important:** Force the table buffer now by calling `snap->Get(table, any_key, val)` before concurrent writers run, if you need a snapshot before any concurrent modifications.

---

## Known Limitations vs RocksDB

| Feature | RocksDB | Mako |
|---------|---------|------|
| Iterators | Stateful `Iterator` object; seek, next, prev | `IIterator` via `NewIterator(txn)`; buffered implementation over `Scan`/`ReverseScan` |
| Snapshots | `GetSnapshot()` / `ReleaseSnapshot()` (global LSN, O(1), true MVCC) | `ISnapshot` via `db->GetSnapshot()`; per-table lazy buffering; O(N) on first table access; no cross-table consistency |
| Merge operators | `Merge()` with user-defined operators | Not supported |
| Column families | Multi-CF per DB | Single index per `GetTable()` name |
| WriteBatch | Atomic multi-table batch | `IWriteBatch` via `db->NewWriteBatch()`; OCC-backed; Put/Delete/DeleteRange; auto-clear on Write() |
| Prefix iterators / bloom filters | Configurable prefix extractors | Not supported |
| Compaction filters | Background key eviction hooks | Not supported |
| GetApproximateSize | Returns real approximate count | Atomic counter updated at operation time; aborted txns may temporarily skew count |
| Persistence | WAL + SST files | In-memory (RocksDB persistence is a separate Mako layer) |
| Range deletions | `DeleteRange()` | Supported via `ITable::DeleteRange(txn, start, end, &count)`; scan-then-delete strategy (O(N), not O(1) tombstone) |
| Transactions | Optimistic or pessimistic | Masstree-native OCC with MVCC |

---

## Migration Guide: RocksDB API → Mako Equivalents

| RocksDB | Mako | Notes |
|---------|------|-------|
| `DB::Open(opts, path, &db)` | `mako::DB::Open(opts, path, &db)` | Options struct differs; no column families |
| `db->Put(wo, key, value)` | `table->Put(txn, key, mako::Encode(value))` | Encode value; use txn handle |
| `db->Get(ro, key, &value)` | `table->Get(txn, key, value)` | Value is decoded automatically |
| `db->Delete(wo, key)` | `table->Delete(txn, key)` | Needs txn |
| `db->NewIterator(ro)` → seek/next | `table->NewIterator(txn)` → `Seek/Next/Valid/key/value` | `IIterator` with buffered scan; same call pattern as RocksDB |
| Reverse iterator | `it->SeekToLast(); it->Prev()` | `SeekToLast()` + `Prev()` on `IIterator`; or `table->ReverseScan(txn, ...)` |
| `db->KeyMayExist(...)` | `table->Exists(txn, key, &exists)` | Exact check, not bloom filter hint |
| `db->Merge(wo, key, value)` | Not available | No merge operators |
| `db->GetSnapshot()` | `db->GetSnapshot()` → `ISnapshot*` | Per-table lazy buffering; call `snap->Get(tbl,k,v)` before writes to lock in the buffer |
| `db->ReleaseSnapshot(snap)` | `db->ReleaseSnapshot(snap)` | Frees all buffers; pointer is invalid after this call |
| `db->GetColumnFamilyHandle(name)` | `db->GetTable(name)` | Returns `ITable*` |
| `db->DefaultColumnFamily()` | `db->GetTable("default")` or any name | |
| `db->ListColumnFamilies(...)` | `db->ListTables()` | Only lists opened tables |
| `db->DeleteRange(wo, cf, begin, end)` | `table->DeleteRange(txn, begin, &end, &count)` | O(N) scan-then-delete; atomic within txn |
| `WriteBatch` | `db->NewWriteBatch()` → `batch->Put/Delete/DeleteRange` → `batch->Write()` | OCC-backed; atomic; auto-clears on success |
| `db->Close()` | `delete db` (destructor calls Close) | |
