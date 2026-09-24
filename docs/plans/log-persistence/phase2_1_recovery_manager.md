# Phase 2.1: Recovery Manager

## Overview

Create a Recovery Manager to detect fresh start vs recovery and coordinate the recovery sequence for Raft and Paxos servers.

## Current State Analysis

### Existing Recovery Methods

Both RaftServer and PaxosServer already have recovery methods from Phase 1.3/1.4:

**RaftServer** (`src/deptran/raft/server.cc:105-151`):
- `SetLogStorage(std::shared_ptr<srpc::LogStorage>)` - Set storage backend
- `GetLogStorage()` - Get current storage
- `RecoverFromStorage()` - Recover term, vote, commitIndex, log entries

**PaxosServer** (`src/deptran/paxos/server.cc:850-912`):
- `SetLogStorage(std::shared_ptr<srpc::LogStorage>)` - Set storage backend
- `GetLogStorage()` - Get current storage
- `RecoverFromStorage()` - Recover epoch, max_committed_slot, max_executed_slot, log entries

### Server Initialization Sequence

```
1. s_main.cc:main() → server_launch_worker()
2. ServerWorker::SetupBase()
   └─ rep_sched_ = rep_frame_->CreateScheduler()  ← RECOVERY HOOK POINT
   └─ Sets loc_id_, site_id_, rep_frame_
3. ServerWorker::SetupService()
   └─ Creates and starts RPC server
4. ServerWorker::SetupCommo()
   └─ Creates communication layer
   └─ Queues EnsureSetup() via PollThread
5. RaftServer::Setup() / PaxosServer setup
   └─ Starts election timer, heartbeat loop
```

## Design

### Recovery Manager Class

```cpp
// src/srpc/rpc/recovery_manager.hpp

namespace srpc {

enum class RecoveryMode {
  FRESH_START,     // No previous state, start fresh
  NORMAL_RECOVERY, // Previous state found, recover
  FORCED_FRESH     // User requested fresh start
};

struct RecoveryConfig {
  std::string storage_path;           // Path for RocksDB storage
  bool force_fresh_start{false};      // Force fresh start even if data exists
  uint32_t recovery_timeout_ms{30000};// Timeout for recovery operations
  bool verify_on_recovery{true};      // Verify data integrity after recovery

  static RecoveryConfig defaults() {
    return RecoveryConfig{};
  }
};

struct RecoveryResult {
  RecoveryMode mode;
  bool success;
  std::string error_message;
  uint64_t recovered_entries{0};
  uint64_t recovered_term{0};      // For Raft
  uint64_t recovered_epoch{0};     // For Paxos
  uint64_t recovery_time_ms{0};
};

class RecoveryManager {
public:
  // @safe - Constructor with config
  explicit RecoveryManager(RecoveryConfig config);

  // @safe - Detect if this is a fresh start or recovery
  RecoveryMode detect_mode() const;

  // @unsafe - Initialize storage backend
  std::shared_ptr<LogStorage> create_storage();

  // @unsafe - Perform recovery for Raft server
  RecoveryResult recover_raft(RaftServer* server);

  // @unsafe - Perform recovery for Paxos server
  RecoveryResult recover_paxos(PaxosServer* server);

  // @safe - Get the configured storage path
  const std::string& storage_path() const;

  // @safe - Check if recovery is needed
  bool needs_recovery() const;

private:
  RecoveryConfig config_;
  std::shared_ptr<LogStorage> storage_;
  RecoveryMode detected_mode_{RecoveryMode::FRESH_START};
  bool initialized_{false};
};

} // namespace srpc
```

### Integration Points

#### 1. ServerWorker Integration

Modify `ServerWorker::SetupBase()` to use RecoveryManager:

```cpp
// server_worker.cc:SetupBase()

// After CreateScheduler():
if (config_->recovery_enabled()) {
  RecoveryConfig rec_config;
  rec_config.storage_path = config_->get_storage_path(partition_id_, loc_id_);

  RecoveryManager recovery_manager(rec_config);
  auto storage = recovery_manager.create_storage();

  if (auto raft_sched = dynamic_cast<RaftServer*>(rep_sched_)) {
    raft_sched->SetLogStorage(storage);
    auto result = recovery_manager.recover_raft(raft_sched);
    if (!result.success) {
      Log_fatal("Raft recovery failed: %s", result.error_message.c_str());
    }
    Log_info("Raft recovery: mode=%d, entries=%lu, term=%lu",
             result.mode, result.recovered_entries, result.recovered_term);
  } else if (auto paxos_sched = dynamic_cast<PaxosServer*>(rep_sched_)) {
    paxos_sched->SetLogStorage(storage);
    auto result = recovery_manager.recover_paxos(paxos_sched);
    if (!result.success) {
      Log_fatal("Paxos recovery failed: %s", result.error_message.c_str());
    }
    Log_info("Paxos recovery: mode=%d, entries=%lu, epoch=%lu",
             result.mode, result.recovered_entries, result.recovered_epoch);
  }
}
```

#### 2. Storage Path Convention

Generate unique storage paths per shard/replica:

```
/tmp/<username>_mako_rocksdb_shard<partition_id>_replica<loc_id>/
├── log_entries/       # Log entries (slot_id -> LogEntry)
├── metadata/          # Metadata (term, vote, commit_index)
└── LOCK               # RocksDB lock file
```

### Implementation Details

#### detect_mode()

```cpp
RecoveryMode RecoveryManager::detect_mode() const {
  if (config_.force_fresh_start) {
    return RecoveryMode::FORCED_FRESH;
  }

  // Check if storage directory exists and has data
  std::string lock_file = config_.storage_path + "/LOCK";
  if (std::filesystem::exists(lock_file)) {
    // RocksDB was initialized before
    return RecoveryMode::NORMAL_RECOVERY;
  }

  return RecoveryMode::FRESH_START;
}
```

#### create_storage()

```cpp
std::shared_ptr<LogStorage> RecoveryManager::create_storage() {
  if (storage_) {
    return storage_;
  }

  storage_ = std::make_shared<RocksDBLogStorage>(config_.storage_path);
  if (!storage_->is_open()) {
    Log_error("Failed to open RocksDB at %s", config_.storage_path.c_str());
    return nullptr;
  }

  detected_mode_ = detect_mode();
  initialized_ = true;
  return storage_;
}
```

#### recover_raft()

```cpp
RecoveryResult RecoveryManager::recover_raft(RaftServer* server) {
  RecoveryResult result;
  result.mode = detected_mode_;

  auto start_time = std::chrono::steady_clock::now();

  if (detected_mode_ == RecoveryMode::FRESH_START ||
      detected_mode_ == RecoveryMode::FORCED_FRESH) {
    result.success = true;
    result.recovered_entries = 0;
    return result;
  }

  // Normal recovery
  if (!server->RecoverFromStorage()) {
    result.success = false;
    result.error_message = "RecoverFromStorage failed";
    return result;
  }

  // Gather statistics
  auto storage = server->GetLogStorage();
  result.recovered_entries = storage->size();

  auto term_opt = storage->get_metadata("currentTerm");
  if (term_opt.is_some()) {
    result.recovered_term = std::stoull(term_opt.unwrap());
  }

  auto end_time = std::chrono::steady_clock::now();
  result.recovery_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      end_time - start_time).count();

  result.success = true;
  return result;
}
```

## Estimated LOC

| Component | LOC |
|-----------|-----|
| recovery_manager.hpp | ~150 |
| ServerWorker integration | ~50 |
| Config integration | ~30 |
| **Total** | **~230** |

## RustyCpp Compliance

- Use `rusty::Cell<bool>` for `initialized_` flag
- Mark storage operations as `@unsafe`
- Use `std::shared_ptr<LogStorage>` for storage member

## Success Criteria

1. Fresh start detected correctly when no previous data exists
2. Recovery mode detected correctly when RocksDB has data
3. Raft/Paxos state recovered before election timer starts
4. Recovery errors logged and reported
5. All existing tests pass
6. Recovery time logged for monitoring

## Testing Plan

1. Unit tests for RecoveryManager
2. Integration test: fresh start scenario
3. Integration test: recovery scenario with pre-populated storage
4. Stress test: recovery under load
