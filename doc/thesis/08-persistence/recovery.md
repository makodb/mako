# Crash Recovery Process

## 1. Overview

When a Mako replica restarts after a crash, the recovery system detects
whether persistent state exists, loads it, and resumes consensus
participation.  The recovery process is managed by `RecoveryManager`,
which coordinates between the storage backend and the consensus server.

## 2. RecoveryMode Enum

**File**: `src/srpc/rpc/recovery_manager.hpp` (267 lines, line 33-37)

```cpp
enum class RecoveryMode {
    FRESH_START,       // No previous state found
    NORMAL_RECOVERY,   // Previous state found, recover from storage
    FORCED_FRESH       // User requested fresh start even if data exists
};
```

The three modes handle different startup scenarios:

| Mode | When Used | Action |
|------|-----------|--------|
| `FRESH_START` | No RocksDB database at the storage path | Create new empty database |
| `NORMAL_RECOVERY` | Valid RocksDB database found (contains `CURRENT` file) | Load metadata and log entries, rebuild in-memory state |
| `FORCED_FRESH` | User set `force_fresh_start = true` | Delete existing database if `clear_on_forced_fresh`, create new |

## 3. RecoveryConfig

**File**: `src/srpc/rpc/recovery_manager.hpp` (lines 42-72)

```cpp
struct RecoveryConfig {
    std::string storage_path;            // line 44: RocksDB path
    bool force_fresh_start = false;      // line 45: force fresh even if data exists
    uint32_t recovery_timeout_ms = 30000;  // line 46: 30 second timeout
    bool verify_on_recovery = true;      // line 47: verify data integrity
    bool clear_on_forced_fresh = true;   // line 48: delete old data on forced fresh
};
```

### 3.1 Factory Method

`RecoveryConfig::for_replica()` (line 56-71) constructs a per-replica
configuration:

```cpp
static RecoveryConfig for_replica(uint32_t partition_id, uint32_t locale_id) {
    RecoveryConfig config = defaults();
    const char* user = std::getenv("USER");
    config.storage_path = "/tmp/" + std::string(user ? user : "unknown")
        + "_mako_log_shard" + std::to_string(partition_id)
        + "_replica" + std::to_string(locale_id);
    return config;
}
```

This generates paths like `/tmp/alice_mako_log_shard0_replica0`.

## 4. RecoveryResult

**File**: `src/srpc/rpc/recovery_manager.hpp` (lines 77-102)

```cpp
struct RecoveryResult {
    RecoveryMode mode;               // line 79: detected mode
    bool success;                    // line 80: whether recovery succeeded
    std::string error_message;       // line 81: error details
    uint64_t recovered_entries;      // line 82: number of entries restored
    uint64_t recovered_term;         // line 83: Raft currentTerm
    uint64_t recovered_epoch;        // line 84: Paxos cur_epoch
    uint64_t recovery_time_ms;       // line 85: time taken
};
```

Factory methods:

| Method | Line | Purpose |
|--------|------|---------|
| `success_fresh()` | 88-93 | Create a success result for fresh starts |
| `failure(error)` | 96-101 | Create a failure result with error message |

## 5. RecoveryManager

**File**: `src/srpc/rpc/recovery_manager.hpp` (lines 116-265)

### 5.1 Internal State

```cpp
class RecoveryManager {
    RecoveryConfig config_;                           // line 261
    std::shared_ptr<LogStorage> storage_;              // line 262
    rusty::Cell<bool> initialized_;                    // line 263
    rusty::Cell<RecoveryMode> detected_mode_;          // line 264
};
```

### 5.2 Mode Detection

`detect_mode()` (line 125-145) determines the recovery mode:

```
1. If config.force_fresh_start == true:
     return FORCED_FRESH

2. If storage_path does not exist:
     return FRESH_START

3. If storage_path/CURRENT file exists:
     return NORMAL_RECOVERY   (valid RocksDB database)

4. Otherwise:
     return FRESH_START   (directory exists but no valid DB)
```

The `CURRENT` file is RocksDB's manifest pointer — its presence
indicates a valid, openable database.

### 5.3 Storage Creation

`create_storage()` (line 148-179) creates the appropriate storage
backend:

```
1. Detect mode via detect_mode()
2. If FORCED_FRESH and clear_on_forced_fresh:
     std::filesystem::remove_all(storage_path)
3. Create RocksDBLogStorage at config.storage_path
4. Call storage->open()
5. Set initialized_ = true
6. Return shared_ptr<LogStorage>
```

### 5.4 Generic Recovery Template

`recover()` (line 214-258) is a template method that coordinates
recovery for any consensus protocol:

```cpp
template<typename SetStorageFn, typename RecoverFn, typename GetStatsFn>
RecoveryResult recover(
    SetStorageFn set_storage,    // fn(shared_ptr<LogStorage>)
    RecoverFn recover_fn,        // fn() -> void
    GetStatsFn get_stats         // fn(RecoveryResult&) -> void
);
```

The recovery sequence:

```
1. Record start time

2. If FRESH_START or FORCED_FRESH:
     call set_storage(storage_)
     return RecoveryResult::success_fresh()

3. For NORMAL_RECOVERY:
     a. call set_storage(storage_)
     b. call recover_fn()           // loads metadata + log entries
     c. call get_stats(result)      // extracts term/epoch stats
     d. Record end time
     e. Log recovery info
     f. Return RecoveryResult with statistics
```

## 6. Recovery Integration in Server Startup

**File**: `src/deptran/server_worker.cc` (lines 380-449)

### 6.1 Sequence Diagram

```
ServerWorker                RecoveryManager        LogStorage         RaftServer
    |                           |                      |                  |
    |  create config            |                      |                  |
    |  for_replica(pid, lid)    |                      |                  |
    |-------------------------->|                      |                  |
    |                           |                      |                  |
    |  create_storage()         |                      |                  |
    |-------------------------->|                      |                  |
    |                           |  detect_mode()       |                  |
    |                           |---+                  |                  |
    |                           |<--+                  |                  |
    |                           |                      |                  |
    |                           |  new RocksDBLogStorage                  |
    |                           |--------------------->|                  |
    |                           |  open()              |                  |
    |                           |--------------------->|                  |
    |                           |<---------------------|                  |
    |<--------------------------|                      |                  |
    |                           |                      |                  |
    |  recover(set, recover, stats)                    |                  |
    |-------------------------->|                      |                  |
    |                           |  set_storage(storage)|                  |
    |                           |--------------------------------------------->|
    |                           |                      |                  |
    |                           |  RecoverFromStorage()|                  |
    |                           |--------------------------------------------->|
    |                           |                      |  get_metadata()  |
    |                           |                      |<-----------------|
    |                           |                      |  get_range()     |
    |                           |                      |<-----------------|
    |                           |                      |                  |
    |                           |  get_stats(result)   |                  |
    |                           |---+                  |                  |
    |                           |<--+                  |                  |
    |<--------------------------|                      |                  |
    | RecoveryResult            |                      |                  |
```

### 6.2 Raft Recovery Call

```cpp
// Create recovery config
srpc::RecoveryConfig config =
    srpc::RecoveryConfig::for_replica(partition_id, locale_id);

// Create manager and storage
srpc::RecoveryManager recovery_manager(config);
auto storage = recovery_manager.create_storage();

// Run recovery
recovery_manager.recover(
    // set_storage: provide storage to Raft server
    [raft_server, &storage](std::shared_ptr<srpc::LogStorage> s) {
        raft_server->SetLogStorage(s);
    },
    // recover: load state from storage
    [raft_server]() {
        return raft_server->RecoverFromStorage();
    },
    // get_stats: extract recovered metadata
    [&storage](srpc::RecoveryResult& r) {
        auto term_opt = storage->get_metadata("currentTerm");
        if (term_opt.is_some()) {
            r.recovered_term = std::stoull(term_opt.unwrap());
        }
    });
```

### 6.3 What RecoverFromStorage Does (Raft)

When called, `RecoverFromStorage()` performs:

1. **Load metadata**:
   - `currentTerm` from `log_storage_->get_metadata(META_TERM)`
   - `vote_for` from `log_storage_->get_metadata(META_VOTE_FOR)`
   - `commitIndex` from `log_storage_->get_metadata(META_COMMIT_INDEX)`

2. **Load log entries**:
   - `log_storage_->get_range(first_index, last_index)` to retrieve
     all persisted entries
   - Rebuild in-memory log from persisted entries

3. **Restore state**:
   - Set `currentTerm_` to recovered term
   - Set `vote_for_` to recovered vote
   - Set `commitIndex_` to recovered commit index
   - Populate in-memory log data structure

### 6.4 What RecoverFromStorage Does (Paxos)

The Paxos equivalent recovers:

1. **Load metadata**:
   - `cur_epoch` from `log_storage_->get_metadata(META_EPOCH)`
   - `max_committed_slot` from `get_metadata(META_MAX_COMMITTED)`
   - `max_executed_slot` from `get_metadata(META_MAX_EXECUTED)`

2. **Load log entries**: Same `get_range()` call
3. **Replay committed**: `ReplayCommittedEntries()` replays entries
   from `executeIndex` to `commitIndex`

## 7. Resolving Uncommitted Entries

After recovery, the server may have log entries that were persisted but
not committed before the crash.  These are resolved via the normal
consensus protocol:

### 7.1 Raft

- If the recovered server is a follower, the current leader will send
  `AppendEntries` RPCs that either confirm the uncommitted entries
  (if the leader has them) or overwrite them (if the leader has
  different entries at those indices).
- If the recovered server becomes leader, it commits a no-op entry
  in its current term to establish authority and indirectly commit
  any uncommitted entries from previous terms.

### 7.2 Paxos

- The `GetUncommittedCount()` method (line 106 in paxos/server.h)
  identifies how many entries need resolution.
- The Paxos leader re-proposes uncommitted slots to achieve consensus.

## 8. Fresh Start vs Recovery Detection

The detection logic in `detect_mode()` uses filesystem checks:

```
storage_path exists?
├── No  → FRESH_START
│         (First ever run for this shard/replica)
└── Yes
    └── CURRENT file exists?
        ├── No  → FRESH_START
        │         (Corrupted or incomplete previous DB)
        └── Yes → NORMAL_RECOVERY
                  (Valid RocksDB database to recover from)
```

The `CURRENT` file is RocksDB's manifest pointer.  If the database was
in the middle of a compaction when the crash occurred, RocksDB's own
recovery mechanisms handle WAL replay when `Open()` is called.

## 9. Storage Cleanup in CI Tests

CI test scripts clean persistent state before each run:

```bash
# From test scripts (e.g., test_1shard_replication_raft.sh):
rm -rf /tmp/${USER}_mako_rocksdb_shard*
```

This ensures each test run starts with `FRESH_START` mode, preventing
stale state from previous runs from affecting results.
