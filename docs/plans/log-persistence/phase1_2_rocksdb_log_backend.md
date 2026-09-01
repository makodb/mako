# Phase 1.2: RocksDB Log Backend

## Overview

Implement `RocksDBLogStorage` - a persistent storage backend that implements the `LogStorage` interface using RocksDB. This enables durable log storage for Raft/Paxos consensus protocols.

## Current State Analysis

### Existing RocksDB Usage
- `src/mako/rocksdb_persistence.h/cc` - Mako's persistence for transaction logs
- RocksDB linked as system library in CMakeLists.txt
- Key format: `"shard_id:partition_id:epoch:seq_num"`
- Uses partitioned databases for high concurrency

### LogStorage Interface (Phase 1.1)
- `src/srpc/rpc/log_storage.hpp` - Abstract interface
- `src/srpc/rpc/memory_log_storage.hpp` - In-memory implementation
- Uses `rusty::Mutex`, `rusty::Cell`, `rusty::Option` for thread safety

## Design

### Key Format
```
log:{slot_id}     - Log entries (padded to 20 digits for lexicographic ordering)
meta:{key}        - Metadata (term, voted_for, commit_index, etc.)
```

Example keys:
- `log:00000000000000000001` - Entry at slot 1
- `log:00000000000001000000` - Entry at slot 1000000
- `meta:current_term` - Current term
- `meta:voted_for` - Last voted candidate

### Value Serialization
Use `Marshal` for LogEntry serialization:
```cpp
LogEntry entry(slot_id, term, cmd, committed);
Marshal m;
entry.to_marshal(m);
std::string value = m.dump();  // Binary blob
```

### Class Structure
```cpp
class RocksDBLogStorage : public LogStorage {
private:
    // Database
    rusty::Box<rocksdb::DB> db_;
    std::string db_path_;
    rusty::Cell<bool> is_open_{false};

    // Configuration
    rocksdb::Options options_;
    rocksdb::WriteOptions write_options_;
    rocksdb::ReadOptions read_options_;

    // Helper methods
    std::string make_log_key(slotid_t slot_id) const;
    std::string make_meta_key(const std::string& key) const;
    bool serialize_entry(const LogEntry& entry, std::string* out) const;
    bool deserialize_entry(const std::string& data, LogEntry* out) const;

public:
    RocksDBLogStorage(const std::string& db_path);
    ~RocksDBLogStorage();

    bool open();  // Open/create database
    // All LogStorage interface methods...
};
```

### RocksDB Configuration
```cpp
rocksdb::Options options;
options.create_if_missing = true;
options.max_open_files = 256;
options.write_buffer_size = 64 * 1024 * 1024;  // 64MB
options.target_file_size_base = 64 * 1024 * 1024;
options.compression = rocksdb::kLZ4Compression;  // Good balance
options.max_background_jobs = 4;

rocksdb::WriteOptions write_options;
write_options.sync = true;  // Durable writes for consensus logs
```

## Implementation Tasks

### Task 1: Core Implementation (~200 LOC)
- Create `src/srpc/rpc/rocksdb_log_storage.hpp`
- Implement constructor/destructor with `rusty::Box` for DB handle
- Implement key formatting helpers
- Implement serialization using Marshal

### Task 2: Single Entry Operations (~50 LOC)
- `get(slotid_t)` - Get entry by key
- `put(const LogEntry&)` - Put entry
- `remove(slotid_t)` - Delete entry

### Task 3: Batch Operations (~60 LOC)
- `get_range(start, end)` - Range scan with iterator
- `put_batch(entries)` - WriteBatch for atomicity
- `remove_range(start, end)` - DeleteRange or individual deletes

### Task 4: Index Queries (~40 LOC)
- `get_first_index()` - SeekToFirst with iterator
- `get_last_index()` - SeekToLast with iterator
- `get_term(slotid_t)` - Get and deserialize for term
- `size()` - Count entries (may need to cache)
- `empty()` - Check if any entries

### Task 5: Metadata and Lifecycle (~50 LOC)
- `set_metadata(key, value)` - Put with meta: prefix
- `get_metadata(key)` - Get with meta: prefix
- `sync()` - Flush WAL
- `close()` - Close DB handle
- `is_open()` - Check open state
- `clear()` - DeleteRange all keys

### Task 6: Unit Tests (~200 LOC)
- Reuse test patterns from `test/rpc_log_storage_test.cc`
- Test persistence across reopen
- Test large entries
- Test concurrent access

## File Structure
```
src/srpc/rpc/
├── log_storage.hpp           # Interface (existing)
├── memory_log_storage.hpp    # In-memory (existing)
└── rocksdb_log_storage.hpp   # RocksDB (new, ~250 LOC)

test/
└── rpc_rocksdb_log_storage_test.cc  # Tests (new, ~200 LOC)
```

## RustyCpp Compliance

- Use `rusty::Box<rocksdb::DB>` for DB ownership (or raw pointer with manual cleanup since RocksDB doesn't use shared_ptr)
- Use `rusty::Cell<bool>` for `is_open_` state
- Use `rusty::Option<T>` for return types
- Mark methods with `@safe` or `@unsafe` annotations
- Use `@unsafe` for RocksDB calls (C library, not borrow-checked)

## CMakeLists.txt Changes

Add to test targets:
```cmake
# RocksDB Log Storage Tests (Phase 1.2)
add_executable(test_rpc_rocksdb_log_storage test/rpc_rocksdb_log_storage_test.cc)
target_include_directories(test_rpc_rocksdb_log_storage PRIVATE ${TEST_INCLUDE_DIRS} ${GTEST_INCLUDE_DIRS})
target_compile_options(test_rpc_rocksdb_log_storage PRIVATE ${TEST_COMPILE_OPTIONS})
target_link_libraries(test_rpc_rocksdb_log_storage ${TEST_LINK_LIBS} ${GTEST_LIBRARIES} ${GTEST_MAIN_LIBRARIES} rocksdb pthread)
add_test(NAME test_rpc_rocksdb_log_storage COMMAND test_rpc_rocksdb_log_storage)
```

## Estimated LOC

| Component | LOC |
|-----------|-----|
| RocksDBLogStorage class | 250 |
| Unit tests | 200 |
| CMakeLists.txt changes | 10 |
| **Total** | **~460** |

## Success Criteria

1. All LogStorage interface methods implemented
2. Data persists across process restarts
3. All unit tests pass (including thread safety)
4. sync() provides durability guarantees
5. Compatible with RustyCpp safety requirements
6. No memory leaks (valgrind clean)
