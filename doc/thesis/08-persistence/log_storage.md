# Persistent Log Storage

## 1. Overview

Both Raft and Paxos require durable storage to survive crashes.  The
persistence layer provides a common `LogStorage` interface with two
implementations: `InMemoryLogStorage` for testing and
`RocksDBLogStorage` for production.

All persistence code lives in `src/srpc/rpc/` and is shared between
Raft and Paxos.

## 2. LogEntry Structure

**File**: `src/srpc/rpc/log_storage.hpp` (302 lines)

The `LogEntry` struct is the fundamental unit of persistent storage:

```cpp
struct LogEntry {
    slotid_t slot_id;                          // line 44: log index
    ballot_t term;                             // line 45: Raft term / Paxos epoch
    ballot_t max_ballot_seen;                  // line 46: highest ballot seen (Paxos)
    ballot_t max_ballot_accepted;              // line 47: highest accepted ballot (Paxos)
    std::shared_ptr<Marshallable> command;     // line 48: replicated command
    bool committed;                            // line 49: whether entry is committed
    bool is_no_op;                             // line 50: no-op entry flag
};
```

Key methods:

| Method | Line | Purpose |
|--------|------|---------|
| `to_marshal(Marshal&)` | 87 | Serialize entry to wire format |
| `from_marshal(Marshal&)` | 108 | Deserialize entry from wire format |
| `operator<()` | 65 | Order entries by `slot_id` |
| `operator==()` | 70 | Compare entries by `slot_id` and `term` |

The structure supports both Raft and Paxos — Raft uses `slot_id` as
the log index and `term` as the Raft term, while Paxos uses `slot_id`
as the instance number and the `max_ballot_*` fields for Multi-Paxos
ballot tracking.

## 3. LogStorage Interface

**File**: `src/srpc/rpc/log_storage.hpp` (302 lines)

The abstract `LogStorage` class defines the storage contract:

### 3.1 Single Entry Operations

| Method | Line | Signature | Returns |
|--------|------|-----------|---------|
| `get()` | 160 | `get(slotid_t)` | `rusty::Option<LogEntry>` |
| `put()` | 168 | `put(const LogEntry&)` | `bool` |
| `remove()` | 176 | `remove(slotid_t)` | `bool` |

### 3.2 Batch Operations

| Method | Line | Signature | Returns |
|--------|------|-----------|---------|
| `get_range()` | 189 | `get_range(slotid_t start, slotid_t end)` | `std::vector<LogEntry>` |
| `put_batch()` | 197 | `put_batch(const std::vector<LogEntry>&)` | `bool` |
| `remove_range()` | 206 | `remove_range(slotid_t start, slotid_t end)` | `bool` |

### 3.3 Index Queries

| Method | Line | Signature | Returns |
|--------|------|-----------|---------|
| `get_first_index()` | 217 | `get_first_index()` | `slotid_t` |
| `get_last_index()` | 224 | `get_last_index()` | `slotid_t` |
| `get_term()` | 232 | `get_term(slotid_t)` | `rusty::Option<ballot_t>` |
| `size()` | 239 | `size()` | `size_t` |
| `empty()` | 246 | `empty()` | `bool` |

### 3.4 Metadata Operations

| Method | Line | Signature | Returns |
|--------|------|-----------|---------|
| `set_metadata()` | 259 | `set_metadata(const std::string& key, const std::string& value)` | `bool` |
| `get_metadata()` | 267 | `get_metadata(const std::string& key)` | `rusty::Option<std::string>` |

Metadata operations store key-value pairs for consensus state such as
`currentTerm`, `vote_for`, and `commitIndex`.

### 3.5 Lifecycle Operations

| Method | Line | Signature | Purpose |
|--------|------|-----------|---------|
| `sync()` | 278 | `sync()` | Force pending writes to durable storage |
| `close()` | 285 | `close()` | Release resources |
| `is_open()` | 292 | `is_open()` | Check if open and ready |
| `clear()` | 299 | `clear()` | Clear all entries and metadata |

## 4. InMemoryLogStorage

**File**: `src/srpc/rpc/memory_log_storage.hpp` (292 lines)

A thread-safe in-memory implementation used for testing.

### 4.1 Internal Structure

```cpp
class InMemoryLogStorage : public LogStorage {
    rusty::Mutex<std::map<slotid_t, LogEntry>> logs_;      // line 36
    rusty::Mutex<std::map<std::string, std::string>> metadata_;  // line 39
    rusty::Cell<bool> is_open_;                             // line 42
};
```

Uses `rusty::Mutex` for thread-safe access to the log map and metadata
map.  The `rusty::Cell<bool>` provides interior mutability for the
open state flag.

### 4.2 Key Characteristics

- All methods are annotated `@safe` (no raw pointer manipulation)
- `sync()` is a no-op (no durable storage to flush)
- `close()` clears all data and sets `is_open_` to false
- `reopen()` (line 270) allows re-opening after close for test scenarios
- `get_all()` (line 278) returns all entries as a vector (test utility)

### 4.3 Thread Safety

All operations acquire the `rusty::Mutex` before accessing `logs_` or
`metadata_`.  This ensures correct behaviour in concurrent test scenarios
such as multiple threads submitting entries while others read.

## 5. RocksDBLogStorage

**File**: `src/srpc/rpc/rocksdb_log_storage.hpp` (480 lines)

The production-grade persistent storage backend.

### 5.1 Internal Structure

```cpp
class RocksDBLogStorage : public LogStorage {
    rocksdb::DB* db_;                    // line 46: database handle
    std::string db_path_;                // line 47: path to database
    rocksdb::Options options_;           // line 50: RocksDB configuration
    rocksdb::WriteOptions write_options_;  // line 51: sync=true
    rocksdb::ReadOptions read_options_;    // line 52: verify_checksums=true
    rusty::Cell<bool> is_open_;            // line 55: open state
};
```

### 5.2 Key Prefixes

```cpp
static constexpr const char* LOG_PREFIX = "log:";     // line 58
static constexpr const char* META_PREFIX = "meta:";   // line 59
```

Log entries are stored with key format `log:{20-digit-padded-slot}`.
The 20-digit zero-padding ensures lexicographic ordering matches
numeric ordering, so RocksDB iterators return entries in index order.

Metadata is stored with key format `meta:{key}`.

### 5.3 RocksDB Configuration

Configured at construction (lines 100-111):

| Parameter | Value | Purpose |
|-----------|-------|---------|
| `create_if_missing` | `true` | Create database if not present |
| `max_open_files` | 256 | Limit file descriptor usage |
| `write_buffer_size` | 64 MB | In-memory write buffer before flush |
| `target_file_size_base` | 64 MB | Target SST file size |
| `compression` | LZ4 | Fast compression for log entries |
| `max_background_jobs` | 4 | Background compaction threads |
| `write_options_.sync` | `true` | **Ensures durability** — every write is fsynced |
| `read_options_.verify_checksums` | `true` | Verify data integrity on read |

The `sync = true` write option is critical for crash safety.  Without
it, RocksDB buffers writes in an OS page cache, and a crash could lose
committed entries.

### 5.4 Serialization

```cpp
// line 62-66: Key format
std::string make_log_key(slotid_t slot_id) {
    return LOG_PREFIX + zero_padded_20_digit(slot_id);
}

// line 74-81: Entry serialization via Marshal
void serialize_entry(const LogEntry& entry, std::string* output) {
    Marshal m;
    entry.to_marshal(m);
    *output = m.to_string();
}

// line 84-89: Entry deserialization
void deserialize_entry(const std::string& data, LogEntry* entry) {
    Marshal m(data);
    entry->from_marshal(m);
}
```

### 5.5 Batch Operations

The `put_batch()` method (line 231-248) uses RocksDB's `WriteBatch`
for atomic multi-entry writes:

```cpp
bool put_batch(const std::vector<LogEntry>& entries) override {
    rocksdb::WriteBatch batch;
    for (const auto& entry : entries) {
        std::string value;
        serialize_entry(entry, &value);
        batch.Put(make_log_key(entry.slot_id), value);
    }
    auto status = db_->Write(write_options_, &batch);
    return status.ok();
}
```

This ensures either all entries are persisted or none are — important
for maintaining log consistency across crashes.

### 5.6 Index Queries

`get_first_index()` (line 280-300) and `get_last_index()` (line 303-332)
use RocksDB iterators:

- `get_first_index()`: Seeks to `LOG_PREFIX`, returns the first matching
  key's slot ID.
- `get_last_index()`: Seeks to `"log;"` (one character past the prefix
  range), then calls `Prev()` to get the last log entry.

### 5.7 Lifecycle

- `open()` (line 124-136): Calls `rocksdb::DB::Open()` with configured
  options.
- `close()` (line 418-430): Deletes the `db_` pointer.
- `sync()` (line 405-415): Calls `db_->Flush()` with `wait=true`.
- `clear()` (line 438-454): Uses a `WriteBatch` to delete all keys.
- `destroy()` (line 473-477): Static method calling
  `rocksdb::DestroyDB()` to remove the database directory.

## 6. Raft Server Integration

**File**: `src/deptran/raft/server.h`

### 6.1 Storage Members

```cpp
std::shared_ptr<srpc::LogStorage> log_storage_;        // line 52
std::shared_ptr<srpc::SnapshotManager> snapshot_manager_;  // line 57
```

### 6.2 Metadata Keys

```cpp
static constexpr const char* META_TERM = "currentTerm";     // line 60
static constexpr const char* META_VOTE_FOR = "vote_for";    // line 61
static constexpr const char* META_COMMIT_INDEX = "commitIndex";  // line 62
```

### 6.3 Persistence Methods

| Method | Line | What It Persists |
|--------|------|------------------|
| `PersistTermAndVote()` | 65 | `currentTerm` and `vote_for` |
| `PersistVote()` | 68 | `vote_for` only |
| `PersistCommitIndex()` | 71 | `commitIndex` |
| `PersistLogEntry()` | 74 | Single log entry |
| `PersistLogEntries()` | 77 | Batch of log entries |

All persistence methods are annotated `@unsafe` because they perform
I/O through the RocksDB library.

### 6.4 When Persistence Is Called

- **`PersistTermAndVote()`**: Called in `doVote()` when advancing the
  current term (line 186 of server.h).
- **`PersistLogEntry()`**: Called after appending an entry to the
  in-memory log.
- **`PersistCommitIndex()`**: Called after advancing the commit index.
- **`PersistLogEntries()`**: Called during `AppendEntries` when
  receiving a batch of entries from the leader.

### 6.5 SetLogStorage and RecoverFromStorage

```cpp
void SetLogStorage(std::shared_ptr<srpc::LogStorage> storage);  // set storage
void RecoverFromStorage();  // restore state from persistent storage
```

`SetLogStorage()` is called during server initialisation to provide the
storage backend.  `RecoverFromStorage()` loads persisted metadata
(`currentTerm`, `vote_for`, `commitIndex`) and log entries from storage,
then rebuilds the in-memory log.

## 7. Paxos Server Integration

**File**: `src/deptran/paxos/server.h`

### 7.1 Metadata Keys

```cpp
static constexpr const char* META_EPOCH = "cur_epoch";           // line 56
static constexpr const char* META_MAX_COMMITTED = "max_committed_slot";  // line 57
static constexpr const char* META_MAX_EXECUTED = "max_executed_slot";    // line 58
```

### 7.2 Persistence Methods

| Method | Line | What It Persists |
|--------|------|------------------|
| `PersistEpoch()` | 61 | Current epoch |
| `PersistMaxCommitted()` | 62 | Max committed slot |
| `PersistLogEntry()` | 64 | Single Paxos log entry |
| `PersistLogEntries()` | 66 | Batch of entries |

### 7.3 Additional Paxos Methods

| Method | Line | Purpose |
|--------|------|---------|
| `SetLogStorage()` | 72 | Set storage backend |
| `GetLogStorage()` | 74 | Get storage backend |
| `RecoverFromStorage()` | 76 | Restore consensus state |
| `ReplayCommittedEntries()` | 98 | Replay from `executeIndex` to `commitIndex` |
| `GetUncommittedCount()` | 106 | Count uncommitted entries |
| `CompactLog()` | 115 | Remove entries covered by snapshot |

## 8. Storage Path Configuration

### 8.1 Log Storage Paths

Generated by `RecoveryConfig::for_replica()` (recovery_manager.hpp,
line 56-71):

```
/tmp/{USER}_mako_log_shard{partition_id}_replica{locale_id}
```

Example: `/tmp/alice_mako_log_shard0_replica0`

### 8.2 Snapshot Storage Paths

Generated by `SnapshotConfig::for_replica()` (snapshot_manager.hpp,
line 277-291):

```
/tmp/{USER}_mako_snapshot_shard{partition_id}_replica{locale_id}
```

### 8.3 User Isolation

Both path generators use `std::getenv("USER")` to create per-user
paths.  This prevents conflicts when multiple users run tests on the
same machine.

## 9. Unit Tests

**File**: `test/rpc_rocksdb_log_storage_test.cc` (531 lines)

The RocksDB implementation has comprehensive Google Test coverage:

| Category | Tests | Lines |
|----------|-------|-------|
| Basic operations | OpenAndClose, PutAndGet, GetNonExistent, PutOverwrite, Remove | 84-124 |
| Batch operations | PutBatch, GetRange, GetRangeEmpty, RemoveRange | 130-159 |
| Index queries | GetFirstIndex, GetLastIndex, GetTerm, SizeAndEmpty | 181-223 |
| Metadata | SetAndGetMetadata, GetMetadataNonExistent, OverwriteMetadata | 228-253 |
| Lifecycle | IsOpen, Sync, CloseIdempotent, Clear, OperationsOnClosedStorage | 259-296 |
| Persistence | PersistenceAcrossReopen, PersistenceWithFullLogEntry | 302-348 |
| Edge cases | LargeSlotIds, ZeroSlotId, InvalidRanges, LargeNumberOfEntries | 354-387 |
| Thread safety | ConcurrentPuts, ConcurrentReadsAndWrites, ConcurrentMetadata | 417-503 |
| Static methods | DestroyDatabase | 509-521 |
