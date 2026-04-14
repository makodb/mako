# Phase 2.2: Log Replay

## Problem Statement

After node restart, committed log entries are recovered from persistent storage, but they are NOT replayed to the state machine. The issue is timing:

1. `RecoverFromStorage()` loads entries into memory (`raft_logs_`/`logs_`)
2. It sets `commitIndex`/`max_committed_slot_` but not `executeIndex`/`max_executed_slot_`
3. The `app_next_` callback is not yet registered when recovery runs

**Initialization sequence:**
```
ServerWorker::SetupBase()
  └─ rep_sched_ = CreateScheduler()
  └─ InitializeRecovery()           ← RecoverFromStorage() called HERE
      └─ Sets: commitIndex=X, executeIndex=0
  └─ RegLearnerAction()             ← app_next_ registered HERE (AFTER recovery)
```

The gap: `executeIndex=0` but `commitIndex=X` after recovery. Normally `applyLogs()` would execute entries `1..X`, but it can't run during `RecoverFromStorage()` because `app_next_` isn't registered yet.

## Solution

Add `ReplayCommittedEntries()` method that is called AFTER `RegLearnerAction()`:

1. Check if `app_next_` is valid
2. Apply all entries from `executeIndex+1` to `commitIndex` (Raft) or `max_executed_slot_+1` to `max_committed_slot_` (Paxos)
3. Log replay statistics

## Implementation

### RaftServer Changes (~30 LOC)

**Add to server.h:**
```cpp
// @unsafe - Calls app_next_ which may have side effects
void ReplayCommittedEntries();
```

**Add to server.cc:**
```cpp
void RaftServer::ReplayCommittedEntries() {
  if (!app_next_) {
    Log_warn("[RAFT-REPLAY] Site %d: No app_next_ callback, skipping replay", site_id_);
    return;
  }

  std::lock_guard<std::recursive_mutex> lock(mtx_);

  slotid_t start = executeIndex + 1;
  slotid_t end = commitIndex;

  if (start > end) {
    Log_info("[RAFT-REPLAY] Site %d: No entries to replay (executeIndex=%lu >= commitIndex=%lu)",
             site_id_, executeIndex, commitIndex);
    return;
  }

  Log_info("[RAFT-REPLAY] Site %d: Replaying entries %lu..%lu", site_id_, start, end);

  size_t replayed = 0;
  for (slotid_t id = start; id <= end; id++) {
    auto instance = GetRaftInstance(id);
    if (instance && instance->log_) {
      app_next_(id, instance->log_);
      executeIndex = id;
      replayed++;
    } else {
      Log_warn("[RAFT-REPLAY] Site %d: Missing log entry at slot %lu, stopping replay", site_id_, id);
      break;
    }
  }

  Log_info("[RAFT-REPLAY] Site %d: Replayed %zu entries, executeIndex now %lu",
           site_id_, replayed, executeIndex);
}
```

### PaxosServer Changes (~30 LOC)

**Add to server.h:**
```cpp
// @unsafe - Calls app_next_ which may have side effects
void ReplayCommittedEntries();
```

**Add to server.cc:**
```cpp
void PaxosServer::ReplayCommittedEntries() {
  if (!app_next_) {
    Log_warn("[PAXOS-REPLAY] Site %d: No app_next_ callback, skipping replay", site_id_);
    return;
  }

  std::lock_guard<std::recursive_mutex> lock(mtx_);

  slotid_t start = max_executed_slot_ + 1;
  slotid_t end = max_committed_slot_;

  if (start > end) {
    Log_info("[PAXOS-REPLAY] Site par %d loc %d: No entries to replay (max_executed=%lu >= max_committed=%lu)",
             partition_id_, loc_id_, max_executed_slot_, max_committed_slot_);
    return;
  }

  Log_info("[PAXOS-REPLAY] Site par %d loc %d: Replaying entries %lu..%lu",
           partition_id_, loc_id_, start, end);

  size_t replayed = 0;
  for (slotid_t id = start; id <= end; id++) {
    auto it = logs_.find(id);
    if (it != logs_.end() && it->second && it->second->committed_cmd_) {
      app_next_(id, it->second->committed_cmd_);
      max_executed_slot_ = id;
      replayed++;
    } else {
      Log_warn("[PAXOS-REPLAY] Site par %d loc %d: Missing committed entry at slot %lu, stopping replay",
               partition_id_, loc_id_, id);
      break;
    }
  }

  Log_info("[PAXOS-REPLAY] Site par %d loc %d: Replayed %zu entries, max_executed now %lu",
           partition_id_, loc_id_, replayed, max_executed_slot_);
}
```

### ServerWorker Changes (~20 LOC)

**Modify SetupBase() to call replay AFTER RegLearnerAction:**
```cpp
  // add callbacks to execute commands to rep_sched_
  if (rep_sched_ && tx_sched_) {
    rep_sched_->RegLearnerAction(std::bind(
        static_cast<int(TxLogServer::*)(int, shared_ptr<Marshallable>)>(&TxLogServer::Next),
        tx_sched_,
        std::placeholders::_1,
        std::placeholders::_2));

    // Phase 2.2: Replay committed entries after callback is registered
    if (auto* raft_server = dynamic_cast<RaftServer*>(rep_sched_)) {
      raft_server->ReplayCommittedEntries();
    }
    if (auto* paxos_server = dynamic_cast<PaxosServer*>(rep_sched_)) {
      paxos_server->ReplayCommittedEntries();
    }
  }
```

## Key Points

1. **Timing**: Replay must happen AFTER `RegLearnerAction()` when `app_next_` is valid
2. **Idempotency**: If no entries to replay, log and return gracefully
3. **Safety**: Lock mutex during replay to prevent concurrent modifications
4. **Logging**: Comprehensive logging for debugging recovery issues

## Files Modified

- `src/deptran/raft/server.h` - Add ReplayCommittedEntries declaration
- `src/deptran/raft/server.cc` - Add ReplayCommittedEntries implementation
- `src/deptran/paxos/server.h` - Add ReplayCommittedEntries declaration
- `src/deptran/paxos/server.cc` - Add ReplayCommittedEntries implementation
- `src/deptran/server_worker.cc` - Call ReplayCommittedEntries after RegLearnerAction

## Testing

1. Run `./ci/ci.sh shard1Replication` - Paxos test
2. Run `./ci/ci.sh shard1ReplicationRaft` - Raft test

Both should pass with log messages indicating replay (or "No entries to replay" if fresh start).

## LOC Estimate

~80 LOC total (within <500 LOC limit)
