# Mako RocksDB-Compatible Interface

This document describes Mako's RocksDB-compatible `ITable` and `IDatabase` interfaces, located in `src/rocks_interface/idb.hh`. It covers three things:

- **Conceptual model** — how Mako's STO/MassTrans/Masstree building blocks map to RocksDB's (`DB`, `ColumnFamily`, `Transaction`, `Snapshot`, `Iterator`, persistence, …), what aligns, and where the two systems irreconcilably diverge.
- **Interface reference** — the abstract `ITable` and `IDatabase` methods, their semantics, and worked examples.
- **RocksDB migration & extensibility** — a `RocksDB API → Mako` translation table, a compatibility feasibility matrix, and a roadmap for what extending the interface further would unlock.

Read the conceptual section first if you're deciding *whether* Mako's interface fits your use case; jump to the interface reference if you're wiring code against it.

> **Backend terminology:** Current Mako transactions use STO `Transaction`
> with MassTrans/Masstree through `mbta_wrapper`. Older revisions of this
> document used “Silo” as shorthand for that lineage. The original Silo
> `transaction_base`/`dbtuple`/`txn_btree` engine is retired and guarded
> against compilation. Literal `SiloRuntime` references below remain current:
> it is live allocator, RCU, and Masstree runtime support for STO/MassTrans.
>
> **Supported local-facade backend:** `Options::storage_engine` must be `"cpp"`
> (the default). The facade rejects any other value before consuming its
> process-lifetime open admission. Rust STO is selected only by the closed
> single-shard TPC-C comparison adapter; `mako::DB` does not expose it.

---

## Conceptual Model

### Vocabulary at a glance

| RocksDB concept | Mako STO/MassTrans analogue | Aligned? |
|---|---|---|
| `DB` (a directory of SSTables + WAL) | `SiloRuntime` (per-shard runtime) | Partially — see §1 |
| `ColumnFamily` (namespace within a DB) | `abstract_ordered_index` / `mbtree` instance | Yes — see §2 |
| `Key` (`Slice`, arbitrary bytes) | `varkey` / `lcdf::Str` (byte string) | Yes |
| `Value` (`Slice`, arbitrary bytes) | Encoded strings held by MassTrans values | Largely aligned — see §7 |
| `Transaction` | Local `mako::DB` exposes STO OCC through an opaque token | Yes locally; remote transaction-shaped RPC is unsupported — see §3 |
| `OptimisticTransactionDB` (OCC) | STO OCC with optional opacity checking | Yes — see §3 |
| `TransactionDB` (pessimistic 2PL) | No equivalent | No |
| `Snapshot` (explicit `SetSnapshot`) | No snapshot handle; OCC reads plus commit validation | Diverges — see §4 |
| `WriteBatch` (atomic multi-key write) | Implicit within a single txn | Yes — see §3 |
| `Iterator` (stateful, snapshot-pinned) | `Scan` callback (single pass, per-node consistency only) | Diverges — see §8 |
| `MergeOperator` | No equivalent | No |
| `CompactionFilter` | No equivalent | No |
| `Comparator` (customizable key ordering) | Bytewise lex only (Masstree layered trie) | Limited |
| `WAL` / `SST files` / `LSM` (persistence architecture) | Independent RocksDB-backed WAL layer (`RocksDBPersistence`) | Fundamentally different — see §5 |
| `BackupEngine`, `Checkpoint` | Masstree `checkpoint.hh` + `kvio.hh` exist but are not wired into `mbtree` | Absent from user-facing API |

### 1. Storage container: `DB` vs `SiloRuntime`

**RocksDB `DB`.** A `DB` is a directory on disk containing SSTables + a WAL. One process opens as many `DB` instances as it wants, each with its own path, WAL, block cache, and set of Column Families. `DB::Open("/tmp/foo")` and `DB::Open("/tmp/bar")` are entirely independent.

**Mako `SiloRuntime`.** Declared in `src/mako/silo_runtime.h:38-244`. A runtime bundles the resources for one "site" or shard: its own `MasstreeContext` (epoch counter, threadinfo list), per-site core-id allocator, per-site memory allocator, per-site ticker, per-site RCU system. Multiple runtimes can coexist in one process via `SiloRuntime::Create()`; threads bind to exactly one via `SiloRuntime::BindCurrentThread(runtime)`.

**Mapping.** A `SiloRuntime` is closer to a RocksDB `DB` than to a shared
allocator or a global. The low-level runtime type can represent multiple sites,
but the current `mako::DB` facade still configures process-global
`BenchmarkConfig`, `init_env()`, `initWithDB()`, and background-service state. It
therefore admits at most one local `DB` initialization in a process lifetime.
`Close()` does not reset that native state or permit a later reopen; another
`DB::Open` returns `Busy`. Independent multi-DB facade instances are not yet
supported.

The one meaningful mismatch: RocksDB's `DB` corresponds to a persistence directory. Mako's `SiloRuntime` corresponds to an in-memory shard. The `path` argument to `mako::DB::Open` is currently always ignored; it does not select persistence or shard identity.

### 2. Namespaces: `ColumnFamily` vs table / index

**RocksDB `ColumnFamily`.** A CF is a logical KV namespace within a single `DB`. All CFs in one DB share the WAL and block cache, but their key spaces are disjoint. Every put/get/delete/iterator takes a `ColumnFamilyHandle*`. A DB has at minimum one CF (`default`).

**STO/MassTrans tables.** Each table is a separate `abstract_ordered_index`
(`src/mako/storage/abstract_ordered_index.h`), instantiated by the native C++
wrapper as `mbta_ordered_index` over `MassTrans`. The historical
`typed_txn_btree` implementation belongs to the retired original Silo engine.
TPC-C creates approximately ten primary and secondary table instances.

**Mapping.** RocksDB CF ↔ Mako table ↔ MassTrans/Masstree instance, all natural fits. `IDatabase::GetTable(name)` already returns an `ITable*` per name — this is functionally equivalent to `DB::CreateColumnFamily` / `DB::GetColumnFamilyHandle`.

**Divergences to note**:

- **Shared WAL semantics don't apply**: RocksDB CFs share a WAL, so cross-CF atomic writes are cheap. Mako has no per-runtime WAL; atomicity comes from the txn layer instead. Effect: multi-table atomicity in Mako uses the transaction, not a `WriteBatch`.
- **CF creation is dynamic in RocksDB, bounded in Mako**: RocksDB lets you `CreateColumnFamily` at runtime. Local non-replicated Mako can create tables on demand through `GetTable(name)`, up to `NUM_TABLES_PER_SHARD` (currently 200 logical names). Replicated deployments must create a static schema in the same deterministic order on every site before helpers, serving threads, or workers start; dynamic replicated schema is unsupported.
- **Options per CF**: RocksDB supports per-CF options (block size, compression, comparator). Mako has no per-table options — all masstree instances behave identically.

### 3. Transaction semantics

**RocksDB.** Two flavors: `OptimisticTransactionDB` (OCC, validates read-set at commit) and `TransactionDB` (pessimistic 2PL, locks acquired eagerly). Both use `class Transaction` with `Put`/`Get`/`GetForUpdate`/`Commit`/`Rollback`. Isolation: snapshot isolation by default; serializable with `SetSnapshot()` + `GetForUpdate()`. Supports 2PC (`Prepare`).

**STO/MassTrans.** OCC with optional opacity checking. The low-level
`abstract_db::new_txn(flags, arena, buf)` starts ambient thread-local state (and
the native wrapper returns null); local `mako::DB::BeginTransaction` supplies a
non-null compatibility token over that state. Operations are staged in a
per-transaction item set and validated at commit. Aborts throw
`abstract_abort_exception` or set an error flag; retry is caller-driven.
Isolation is **serializable** via read-set validation at commit, stricter than
RocksDB's default snapshot isolation. The public facade exposes no durable
RocksDB-style `Prepare`; Mako separately has an internal cross-shard 2PC path.

`RemoteDB` implements the same virtual shape, but its SRPC token-taking path is
only scaffolding: the server executes each Put/Get/Delete independently, and
Commit/Rollback only retire a tracking ID. It provides neither transaction
atomicity nor rollback and is unsupported for transactional use. The only
implemented end-to-end remote surface is `ConnectNontxn` with token-free point
operations; its thread-per-connection server is still outside the bounded
production profile described below.

**Mapping**:

| Feature | RocksDB (`OptimisticTransactionDB`) | STO/MassTrans | Notes |
|---|---|---|---|
| `BeginTransaction` | `db->BeginTransaction(...)` → `Transaction*` | `db->BeginTransaction()` → `void*` | Present |
| `Put/Get/Delete` in txn | `txn->Put(cf, k, v)` | `table->Put(txn, k, v)` | Present; API shape flipped (Mako passes `void* txn` first) |
| `GetForUpdate` | `txn->GetForUpdate(...)` | No equivalent | STO OCC doesn't distinguish — every read is tracked implicitly |
| Isolation level | Snapshot (default) or serializable | Always serializable | Mako is stricter |
| Read-your-writes | Yes | Not implemented in the current MassTrans path | Do not rely on it through the compatibility facade |
| Commit | `txn->Commit()` returning `Status` | `db->commit_txn(txn)` returning `bool` | Aligned semantics; different signatures |
| Rollback | `txn->Rollback()` | `db->abort_txn(txn)` | Aligned |
| Retry on conflict | Caller loop | Caller loop | Aligned |
| 2PC (`Prepare`) | Supported | No public durable `Prepare`; internal cross-shard 2PC is separate | Gap |
| `SetName` (named txn for recovery) | Supported | Not supported | Gap |

**No pessimistic flavor**. RocksDB's `TransactionDB` (2PL) has no counterpart. Any consumer requiring lock-based blocking semantics can't be supported without a fresh implementation.

**Non-transactional access.** Mako's `abstract_ordered_index` also exposes a
**non-transactional API** mirroring Masstree's operation set — `get / put /
insert / remove / scan / rscan` without a txn handle. On STO/MassTrans, each
operation runs as a one-operation OCC transaction and retries
`Transaction::Abort` conflicts. Terminal failures such as timestamp exhaustion
are not conflicts; the local compatibility facade converts them to an error
status rather than retrying indefinitely. This is the analog of RocksDB's plain
`db->Put/Get/Delete` outside any `Transaction`. See
[`storage-interface.md`](storage-interface.md) for the full contract, including
the constraint that these methods must not be called from a thread with an open
transaction.

### 4. Snapshots

**RocksDB.** `Snapshot* s = db->GetSnapshot()` captures a global sequence number. Reads with `ReadOptions{.snapshot=s}` see the committed state as of that seq. `ReleaseSnapshot(s)` decrements a refcount. Snapshots pin resources (SSTables can't be compacted away). Explicit inside a txn via `txn->SetSnapshot()`.

**STO/MassTrans.** No explicit snapshot handle and no general as-of-start MVCC
view. The default MassTrans path reads current values, records their versions,
and validates the complete read set at commit. A conflict aborts the transaction
instead of serving an older version. There is no snapshot object to share with a
different code path or retain past commit.

**Mapping.** RocksDB's snapshot API is `Status::NotSupported` territory for Mako. Any RocksDB code that does `s = db->GetSnapshot(); ... use s ...; ReleaseSnapshot(s);` outside a transaction cannot be directly ported. Two workaround patterns:

- Wrap the region in a Mako transaction (`BeginTransaction` → reads →
  `Commit`) when serializable success-or-retry semantics are sufficient. This
  does not provide a stable historical view while the transaction is running.
- Read at higher isolation via GetForUpdate-equivalent — but STO has no such distinction.

Callers relying on cross-txn snapshot handles (e.g., long-running analytical queries against a snapshot fixed at some past time) don't have a natural mapping.

### 5. Persistence architecture

The most fundamental divergence. Worth stating clearly:

**RocksDB's persistence is intrinsic**. `db->Put(...)` writes to memtable + WAL; on memtable flush, immutable SSTables land on disk; compaction merges them. `sync=true` fsyncs the WAL. Recovery replays the WAL from the last checkpoint. Persistence is not a separate layer — it *is* the storage engine.

**Mako/Masstree persistence is external and optional**. Masstree is a pure in-memory index; its `checkpoint.hh` (`ckstate::visit_value`) and `kvio.hh` (`kvout` msgpack serializer) exist as infrastructure but are **not wired into `mbtree`**. Durability in Mako is provided by an entirely separate component: `src/mako/rocksdb_persistence.h` uses RocksDB as a partitioned write-ahead log, and `src/deptran/raft/rocksdb_log_storage.hpp` uses RocksDB for Raft's consensus log. These are Mako's persistence layers; masstree itself doesn't participate.

**Mapping implications**:

- `Options.write_buffer_size`, `Options.max_write_buffer_number`, all LSM tuning: N/A. Absorbed silently by an options struct, unused.
- `WriteOptions.sync = true`: cannot be honored by the in-memory path. Either accept silently (misleading) or return `Status::NotSupported`.
- `WriteOptions.disableWAL`: N/A (no WAL to disable).
- `db->Flush()`, `db->CompactRange()`, `db->GetProperty("rocksdb.stats")`: N/A.
- `db->NewCheckpoint(...)` / `BackupEngine`: could conceptually be built on `ckstate` + `kvio` — real work if pursued, but the primitives exist.

**Practical guidance**: don't try to make the compat layer look persistent. The right model is "RocksDB API shape, in-memory backing, durability via the separate `RocksDBPersistence` WAL if needed at a higher layer."

### 6. Concurrency & epoch model

**RocksDB.** Multiple threads share a `DB`; internal locking. Global monotonic sequence number ordered by write time. Snapshots reference a seq.

**Mako/STO.** Multiple threads share a `SiloRuntime` after
`BindCurrentThread`. The native STO epoch worker advances its global epoch about
every 100 ms. Epochs are used for RCU deferred reclamation, not durability or
commit visibility. STO commit versions come from a process-wide atomic counter;
Mako's replication path also allocates a process-wide logical timestamp.

**Mapping.** Not directly observable from RocksDB's API — mostly internal. But two visible knock-on effects:

- A committed delete removes the key from the tree during install. Its retired
  storage is reclaimed only after an RCU grace period; this delay is not visible
  as a successful point read.
- No monotonic global sequence number to expose as `SequenceNumber` in a RocksDB-shaped API. Any consumer relying on RocksDB's sequence numbers for external ordering can't be served.

### 7. Values: opaque bytes vs typed rows

**RocksDB.** Values are opaque `Slice`s. Applications serialize their own structs to bytes. `PinnableSlice` avoids copying on read.

**STO/MassTrans.** Values are encoded strings stored by MassTrans. TPC-C still
uses schema-generated record types in application code, then encodes those
records before staging them through `mbta_ordered_index`.

**Mapping.** The `ITable` interface (below) takes `std::string` for both key and
value, using `mako::Encode()` where the Mako storage format requires it. The
compatibility layer's opaque-byte model is preserved end-to-end.

Encoding and wrapper copies can add overhead relative to RocksDB's direct
application-to-memtable path; measure that cost for latency-sensitive uses.

### 8. Iteration model

**RocksDB.** `Iterator* it = db->NewIterator(ro, cf);` returns a stateful, snapshot-pinned cursor. Methods: `SeekToFirst`, `SeekToLast`, `Seek(key)`, `SeekForPrev(key)`, `Next`, `Prev`, `Valid`, `key`, `value`, `status`. `~Iterator` releases resources. The iterator observes a consistent snapshot regardless of concurrent writes.

**MassTrans/Masstree.** No stateful iterator. `mbtree::search_range_call(lower, upper, callback)` is push-based: the caller provides a callback invoked for each key until it returns false. `ITable::Scan` and `ITable::ReverseScan` follow this shape. Consistency: Masstree's normal per-node version-check retry; **not** a pinned snapshot across the whole scan.

**Mapping**:

- Callback-based scan is functional and already in `ITable`. Consumers who can restructure code to callbacks can use it directly.
- Consumers who need a *stateful* pull-based iterator (`for (it->SeekToFirst(); it->Valid(); it->Next())`) need an adapter. Two designs:
  - **Chunked materialization**: on `SeekToFirst`/`Seek`, run `search_range_call` collecting up to N pairs into a buffer; on `Next` past the buffer, refill from the next key onward. Memory is bounded, but there is no point-in-time consistency across chunks.
  - **Transaction-scoped iterator**: reads can be tracked together in an STO transaction, but the caller must buffer their effects and use them only after Commit succeeds. A failed commit invalidates the observed result, and the default non-opaque profile does not guarantee a consistent intermediate view while callbacks run.
- Snapshot-pinned iterators outside a transaction (RocksDB's default) have no clean Mako mapping.

### 9. Secondary indexes

**RocksDB.** No first-class secondary index; applications maintain them manually as auxiliary CFs.

**Mako.** Same — no automatic secondary indexes. TPC-C's `customer_name_idx` (`tpcc.h:80-87`) is a separate MassTrans/Masstree instance keyed by `(warehouse, district, last_name, first_name) → customer_id`, updated by the application whenever the primary `customer` table is updated.

**Mapping.** Both systems put secondary index maintenance on the application. No compat gap.

### 10. Extension points RocksDB has that Mako doesn't

Three RocksDB features are architecturally absent from Mako/Masstree:

- **`MergeOperator`**: RocksDB supports read-modify-write with user-defined associative merge functions (`db->Merge(key, delta)`). At read time, RocksDB replays all pending merges. Mako has no equivalent — a Mako-side merge would require the application to Get → modify → Put inside a transaction.
- **`CompactionFilter`**: RocksDB invokes user callbacks during compaction to filter or transform values (e.g., TTL-based expiry). Mako has no background compaction, so nothing to hook into.
- **Custom `Comparator`**: RocksDB accepts a custom key comparator per CF; the default is bytewise lex. Masstree's trie-of-B+trees hardcodes bytewise lex order internally; there's no way to swap in a custom comparator without rewriting Masstree.

None of these have workarounds. `NotSupported` is the honest answer for all three.

---

## Interface Summary

### ITable

| Method | What it does | What it lacks | Why this implementation |
|--------|-------------|---------------|------------------------|
| `Put` | Write a key-value pair | No blind-write shortcut; value must be `mako::Encode()`'d | |
| `Get` | Read a value by key | No bloom-filter hint or short-circuit path | |
| `Delete` | Remove a key | No range delete | |
| `GetName` | Return the table name | — | |
| `Scan` | Forward range scan [start, end); returns `NotSupported` if `num_shards > 1` | No stateful iterator; cross-shard not implemented | Keys are hash-distributed rather than range-distributed, so a future cross-shard scan also needs an explicit global-order contract |
| `ReverseScan` | Reverse range scan descending; returns `NotSupported` if `num_shards > 1` | No stateful iterator; cross-shard not yet implemented | Same as Scan |
| `Exists` | Check key presence without reading value | Does a full Get internally; no bloom-filter hint | Chosen over a separate existence flag to reuse the OCC read-set tracking already done by Get |
| `Insert` | Insert only if key absent | Duplicate returns `InvalidArgument`; caller must still Commit or Rollback the active transaction | An existence read plus `transInsert` makes put-if-absent serializable: validation catches a racing insert rather than silently overwriting it |
| `GetApproximateSize` | Approximate key count for the local shard | Local shard only; not a transactional snapshot | Relaxed atomic aggregate updated in `install()` |

### ITable non-transactional methods (2026-07)

`ITable` also carries a non-transactional surface (no `void* txn`
parameter). On STO/MassTrans each operation is a self-contained,
immediately-visible one-operation OCC transaction on the owning shard, so
writes use the normal commit path. Semantics: `Put` = blind
overwrite (OK); `Insert` = put-if-absent (`InvalidArgument` if
present); `Delete` = real remove (`NotFound` if absent); `Get` = OK /
`NotFound`; `Exists` = OK + flag. Values are raw bytes in both
directions (the backend applies its storage encoding internally,
unlike the transactional methods, which require caller-Encoded
values). Defaults
return `NotSupported`; `LocalTable` implements them over the L3
non-txn API and `RemoteTable` over the self-contained non-txn request
types (14-17). Callers must not have an open transaction on the
calling thread. See
[`storage-interface.md`](storage-interface.md).

The `RemoteDB` KV path was re-based onto this machinery: the previous
implementation "wrote" via `shard_put` — staging + locking a 2PC
participant write that nothing ever committed (never visible, never
replicated, lock leaked) — and "deleted" via an empty-value put. Both
decoupled-client server paths (the raw-struct handlers and
`MakoClientService`) now run `ShardReceiver::RunNontxnOp`.
`RemoteDB::ConnectNontxn(host, port)` + `GetTable(name, table_id)`
give a client that interoperates with `ClientTcpServer` end-to-end. That server
currently creates an OS thread per connection and is outside the bounded
production profile. The SRPC transaction-shaped path has a live handler, but
its data operations are independent and therefore non-atomic.

### IDatabase

| Method | What it does | What it lacks | Why this implementation |
|--------|-------------|---------------|------------------------|
| `BeginTransaction` | Local DB starts an STO transaction and returns an opaque token | No isolation-level choice; RemoteDB path is non-atomic scaffolding | |
| `Commit` | Local DB validates and publishes all writes | No auto-retry on OCC abort; RemoteDB only retires its tracking ID | |
| `Rollback` | Local DB discards staged writes | No savepoints; RemoteDB cannot undo prior data RPCs | |
| `GetTable` | Get or create a bounded table proxy by name | Remote table must already exist; local creation returns `nullptr` at capacity; replicated schemas must be static | Local non-replicated lookup is metadata-only and on-demand |
| `ListTables` | List names of tables opened in this session | Only reflects tables opened via `GetTable`; not a full schema query | Remote DB has no name→table mapping, so listing all tables is not possible; local DB returns only opened tables for the same interface consistency |
| `Connect` / `Disconnect` / `IsConnected` | Connection lifecycle management | Local `Connect` validates open state but does no transport work; `Disconnect` is a no-op; `IsConnected` mirrors open state | Common virtual shape only; it does not imply equal transactional capability |
| `InitThread` | Initialize per-thread database context | Required for local DB; no-op for `RemoteDB` | |
| `EndThread` | End the current thread's matching context | Same-thread pairing is mandatory for local DB | Prefer the scoped guard |
| `HasThreadContext` | Report whether this thread's attachment prerequisite is satisfied | Does not describe connection or server-side state | Always true for `RemoteDB` |

---

## ITable API

The `ITable` interface provides key-value table operations within a transaction
context. Every storage operation on a facade-backed local `ITable`, including
non-transactional verbs and `GetApproximateSize`, requires a live
`ScopedDatabaseThreadContext`; the metadata-only `GetName` accessor does not.
The examples below assume they run inside the guard shown in “Basic CRUD.”
`RemoteTable` has no client-side attachment.

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

**Current limitation:** Only works in single-shard deployments. It returns
`Status::NotSupported` if more than one shard is present. Cross-shard scan is
not implemented. Because keys are hash-distributed rather than
range-distributed, any future implementation also needs an explicit
global-order contract.

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
// keys contains scan_key_039, scan_key_038, ..., scan_key_021 in descending order
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

Return an approximate count of keys in the **local shard** of this table. It
does not require a transaction handle, but a local table still requires the
database thread context.

The count is a relaxed atomic aggregate updated in the commit phase
(`install()`). It is race-free under concurrent inserts and deletes but is not a
transactionally consistent snapshot. It only reflects data in the local shard;
a cluster-wide count would require a remote RPC scan (not yet implemented).

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

On local `mako::DB`, begin a new transaction and return a non-null opaque token
to pass to table operations. Local tokens name thread-local ambient transaction
state, are valid only on the creating thread until `Commit` or `Rollback`, and
may reuse the same address for a later transaction. Do not retain or reuse a
resolved token. Because address reuse is intentional, a stale token passed
during a later active attempt cannot be distinguished from that attempt's live
token and is an unchecked caller-contract violation.

**Errors:** Local setup failures throw; local success does not use `nullptr` as
a sentinel. `RemoteDB` may return null, and its token-taking path is unsupported
for transactional use because server data operations are not atomic.

### Commit

```cpp
virtual void Commit(void* txn) = 0;
```

On local `mako::DB`, commit a transaction and make all writes atomically visible.
The in-memory facade does not make them durable; persistence and replication are
separate layers. `RemoteDB::Commit` only retires an experimental tracking token.

### Rollback

```cpp
virtual void Rollback(void* txn) = 0;
```

On local `mako::DB`, abort a transaction and discard its staged writes.
`RemoteDB::Rollback` cannot undo data RPCs that have already executed.

### GetTable

```cpp
virtual ITable* GetTable(const std::string& name) = 0;
```

Retrieve (or create) a table by name. This is a metadata operation and does not
require `InitThread()`. The returned pointer is owned by the database; stop all
uses before `Close()` and do not retain it across close or destruction. Local
creation is bounded by `NUM_TABLES_PER_SHARD`, currently 200 logical names. The
implementation preflights every shard and returns `nullptr` on exhaustion
without consuming an ID or partially creating the logical table. Existing
table lookups and data remain usable at capacity.

In replicated deployments, create the complete schema in the same order on
every site during startup, before helpers, transport serving, or workers begin.
Runtime schema changes cannot yet guarantee cross-process table-ID agreement
and are unsupported.

**Returns:** Pointer to `ITable`, `nullptr` on failure.

### ListTables

```cpp
virtual std::vector<std::string> ListTables() = 0;
```

Return the names of all tables currently tracked by the database (i.e., all
tables previously accessed via `GetTable`). This is metadata-only and does not
require `InitThread()`, but it must be quiesced before `Close()`. Tables that
exist in the underlying store but have never been opened in this session will
not appear.

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

Connection lifecycle methods. For local `mako::DB`, `Connect` performs no
transport work but succeeds only while the facade is open, `Disconnect` is a
no-op, and `IsConnected` mirrors open state. For `mako::RemoteDB`, these
describe its RPC connection.

### Thread context lifecycle

```cpp
virtual void InitThread() {}
virtual void EndThread() {}
```

Initialize the current thread for database operations. This is required for a
standalone local database and for a replicated leader before transactions or
table operations. A follower/learner local facade is replay-only and rejects
application operation attachment; `RemoteDB` needs no client-side attachment.
`InitThread()` and `EndThread()` must be paired on the same thread, with
`EndThread()` after its last database operation and before closing the local
database. Prefer `ScopedDatabaseThreadContext`, which provides that pairing on
every lexical exit; the referenced `IDatabase` must outlive the guard. Nested
attachment and an unmatched direct `EndThread()` throw `std::logic_error` before
TLS state is changed. `Close()` returns `Busy` while any local thread context is
reserved, including while its backend attachment or teardown is running.
Destroying the facade after such a failed `Close()` terminates the process. The
guard type itself is non-copyable and non-movable. If a heap-owned guard is
transferred and destroyed on another OS thread, its destructor terminates
because the underlying STO, RCU, and shard-client state is thread-local.

Teardown aborts an unfinished transaction before retiring its RCU participation
and destroying its shard client. A later attachment on the same OS thread
reuses that thread's quiesced native worker slot. Slots are not recycled across
different OS threads. The process may use at most `MAX_THREADS` (currently 460)
distinct native STO threads over its lifetime, and each shard admits at most
the configured warehouses count of distinct non-loader workers. Production
callers must use a bounded, fixed worker pool. `ClientTcpServer` currently
creates an OS thread per connection and is outside this supported profile until
it uses persistent workers.
The facade permits only one local `mako::DB` initialization per process because
its configuration and native services remain process-global. `Close()` ends use
of the facade but deliberately does not make the process eligible for reopen.
It is not a drain barrier: before calling it, the application must externally
quiesce metadata calls, table-pointer users, and every borrowed pointer returned
by `GetDB()`. Those uses are not reference-counted. `GetDB()` stops publishing
the pointer once close begins, but a previously returned pointer is not made
safe by that check and must never survive `Close()`.

This release adds `EndThread()` and `HasThreadContext()` virtuals to
`IDatabase`, and changes `InitThread()` from an immediately self-balanced helper
to a persistent attachment that may throw. Rebuild all C++ consumers against
the new headers; prebuilt objects using the old `IDatabase` vtable are not ABI
compatible.

---

## Usage Examples

### Basic CRUD

```cpp
#include <mako.hh>
#include "rocks_interface/db.hh"

mako::DB* db = nullptr;
mako::Options opts;
opts.num_threads = 1;
mako::Status status = mako::DB::Open(opts, "/tmp/mako_db", &db);
if (!status.ok()) {
    std::cerr << status.ToString() << std::endl;
    return;
}

// Schema creation is metadata-only. Do it before transaction workers start.
mako::ITable* tbl = db->GetTable("my_table");
if (tbl == nullptr) {
    status = db->Close();
    if (status.ok()) delete db;
    return;
}

{
    mako::ScopedDatabaseThreadContext thread_context(*db);

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
}

status = db->Close();
if (!status.ok()) {
    // Quiesce the remaining borrower or context, then retry. Do not delete db.
    std::cerr << status.ToString() << std::endl;
    return;
}
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

## Current consumers

Three programs currently use the ITable / IDatabase interface:

| Consumer | Role | Notes |
|---|---|---|
| `examples/simpleTransactionRep.cc` | Database server + transaction test with colocated / server-only / remote-client modes | Exercises the shared virtual shape in Docker CI, but does not prove atomic RemoteDB transactions; that path remains scaffolding. |
| `examples/makoCon.cc` | Redis-compatible server built on top of `mako::DB`; MULTI/EXEC wraps in one transaction | Proof of concept, not in CI. Its Rust workers attach for process lifetime and it has no cooperative server shutdown, so it does not exercise `EndThread`. |
| `examples/rocksdbInterfaceTest.cc` | Integration and lifecycle test for the local facade | Registered in CTest as `test_rocksdb_interface_lifecycle`; covers the Rocks-like operations, attachment rules, close admission, and process-lifetime reopen rejection. |

No internal Mako subsystem consumes `IDatabase`. The Mako runtime (TPC-C and
`dbtest`) uses `mbta_wrapper`/`mbta_ordered_index` and the abstract ordered-index
interface directly. The original `txn_proto2_impl`/`typed_txn_btree` path is
retired. RocksDB-backed producers call the real RocksDB C API directly.

The compat interface is an **external-consumer surface**. Local `mako::DB` is
the transaction-capable implementation. Sharing the interface with `RemoteDB`
provides source-level shape only; callers must inspect backend capabilities and
must not infer remote transaction atomicity.

---

## Compatibility Summary Matrix

Feasibility assessment for every notable RocksDB feature, from the conceptual analysis above:

| RocksDB feature | Mako support today | Feasibility to add | Notes |
|---|---|---|---|
| Multiple DBs | Low-level `SiloRuntime` can represent several sites; `mako::DB` cannot | Requires instance-owned configuration, logger, callbacks, and service teardown | The facade admits one initialization per process lifetime |
| Column families | Via `GetTable(name)` returning per-name `ITable`, bounded to 200 logical names | Already there for the bounded profile | Distributed schemas must be created identically and in deterministic order before serving starts |
| Put/Get/Delete | Yes locally; token-free remote point path is implemented | — | Remote token-taking methods are non-atomic scaffolding |
| MultiGet | No | Straightforward as batch of Get in one txn | Easy |
| WriteBatch (atomic) | No, use txn | Straightforward as internal single-op txn | Easy |
| Range delete | No | Feasible via `Scan` + per-key delete | Medium (perf concern) |
| Iterator (stateful, pull-based) | No, only callback Scan | Chunked-materialisation adapter | Medium |
| Snapshot outside txn | No | No clean mapping | Hard/impossible |
| OCC transactions | Yes on local `mako::DB` | Extension: expose a typed transaction object | RemoteDB does not yet provide transaction atomicity |
| Pessimistic 2PL transactions | No | Would require new locking layer over masstree | Hard |
| RocksDB-style durable `Prepare` | No | Would require recovery-facing durability and coordinator support | Mako's internal cross-shard 2PC is not this API |
| MergeOperator | No | No equivalent | Impossible without RMW loop |
| CompactionFilter | No | No compaction to hook | Impossible |
| Custom Comparator | No | Masstree hardcodes bytewise | Impossible without Masstree rewrite |
| Bloom filter, prefix seek | No | Masstree's trie structure IS prefix-optimised natively; bloom N/A for in-memory | Trivial to expose "prefix seek" as callback semantics |
| Persistence (WAL, SSTables) | Via separate `RocksDBPersistence` layer | Would replace Masstree's whole storage engine | Out of scope |
| Backup / Checkpoint | Infrastructure exists (`checkpoint.hh`, `kvio.hh`), unwired | Feasible if wanted | Medium project |
| Options tuning | No per-table options | Could expose applicable in-memory settings explicitly | RocksDB compression and compaction options have no Masstree equivalent |
| `Flush`, `CompactRange` | No | N/A | `NotSupported` |
| `GetProperty("rocksdb.stats")` | Minimal | Could expose masstree counters | Cosmetic |

---

## RocksDB → Mako Migration Guide

Method-level mapping for translating RocksDB code to Mako's interface:

| RocksDB | Mako | Notes |
|---------|------|-------|
| `DB::Open(opts, path, &db)` | `mako::DB::Open(opts, path, &db)` | Local facade accepts only `storage_engine = "cpp"`, admits one initialization per process lifetime, and ignores path |
| `db->Put(wo, key, value)` | `table->Put(txn, key, mako::Encode(value))` | Encode value; use txn handle |
| `db->Get(ro, key, &value)` | `table->Get(txn, key, value)` | Value is decoded automatically |
| `db->Delete(wo, key)` | `table->Delete(txn, key)` | Needs txn |
| `db->NewIterator(ro)` → seek/next | `table->Scan(txn, start, end, cb)` | Replace stateful iterator with callback |
| Reverse iterator | `table->ReverseScan(txn, start, end, cb)` | Callback-based |
| `db->KeyMayExist(...)` | `table->Exists(txn, key, &exists)` | Exact check, not bloom filter hint |
| `db->Merge(wo, key, value)` | Not available | No merge operators |
| `db->GetSnapshot()` | Not available | No snapshot isolation |
| `db->GetColumnFamilyHandle(name)` | `db->GetTable(name)` | Returns a borrowed `ITable*` or `nullptr`; catalog is bounded and distributed schemas are startup-only |
| `db->DefaultColumnFamily()` | `db->GetTable("default")` or any name | |
| `db->ListColumnFamilies(...)` | `db->ListTables()` | Only lists opened tables |
| `WriteBatch` | Multiple `Put`/`Delete` in one `BeginTransaction/Commit` | |
| `db->Close()` | `Status s = db->Close(); if (s.ok()) delete db;` | Quiesce all callers and end every thread context first; never delete after an ignored `Busy` result |

---

## Implications for Extending the Interface

The existing interface covers the operations used by the current in-tree
consumers. Possible extensions include:

**High value, low-to-medium effort**:
- **WriteBatch as a single-txn wrapper**: `makoCon.cc` already does this manually — its Redis `MULTI/EXEC` path calls `BeginTransaction` → op → op → `Commit`. Exposing an explicit `WriteBatch` lets it drop that custom wrapper and lets any RocksDB code using `WriteBatch` compile against `mako::DB` unchanged.
- **MultiGet as batched read within one implicit txn**: `makoCon.cc`'s Redis `MGET` currently issues N individual `Get` calls, each in its own transaction. A native `MultiGet` can validate all reads together and publish results only after a successful commit, with less overhead than N transactions.
- **Stateful iterator adapter**: no current consumer needs this. Useful when porting RocksDB code that uses `for (it->SeekToFirst(); it->Valid(); it->Next())`. Worth building only when a specific porting target motivates it.
- **Explicit `Transaction` class** (`OptimisticTransactionDB`-shaped): `simpleTransactionRep.cc` gets what it needs from the `BeginTransaction/Commit/Rollback` triple already. A separate `Transaction` object is mostly a shape match for RocksDB code being ported in; low urgency compared to WriteBatch/MultiGet.

**Medium value, medium effort**:
- **Cross-shard Scan** (the existing single-shard limitation; requires RPC fan-out for `RemoteDB`).
- **Transaction-scoped read adapter** with success-or-retry semantics; this is not a snapshot API, and `db->GetSnapshot()` remains `NotSupported`.
- **Range delete** as `Scan` + per-key delete inside a txn.

**Low value or infeasible**:
- Pessimistic `TransactionDB` (no locking substrate in Mako).
- RocksDB-style durable `Prepare`.
- `MergeOperator`, `CompactionFilter`, custom `Comparator`.
- Persistence-adjacent (`Flush`, `CompactRange`, `BackupEngine`, `Checkpoint` unless the existing `ckstate`/`kvio` infrastructure is wired up as a follow-on project).

**Deliberately don't try**:
- Making `WriteOptions.sync` mean anything for in-memory paths.
- Exposing a `SequenceNumber` (Mako has no monotonic global counter).
- Snapshots that outlive a transaction.

---

## Related files

- `src/rocks_interface/idb.hh` — the abstract `IDatabase` and `ITable` interfaces this doc describes.
- `src/rocks_interface/db.hh` — local `mako::DB` implementation.
- `src/rocks_interface/remote_db.hh` — remote-client `mako::RemoteDB` implementation.
- `src/rocks_interface/local_table.hh` — local table backing.
- `src/mako/silo_runtime.h` — the `SiloRuntime` container that each local DB wraps.
- [`masstree-test-plan.md`](masstree-test-plan.md) — masstree correctness testing.
