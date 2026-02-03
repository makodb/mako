# Phase 3.4: Log Compaction

## Problem Statement

After taking a snapshot at index N, log entries 1..N can be safely removed because:
1. The snapshot contains the state at index N
2. Recovery can load the snapshot and replay entries N+1 onwards

Log compaction reduces:
- Memory usage (in-memory log entries)
- Disk usage (persisted log entries)
- Recovery time (fewer entries to replay)

## Design

### Compaction Strategy

1. **When to compact**: After successfully taking a snapshot
2. **What to remove**: Entries from first slot up to (but not including) last_included_index
3. **Safety**: Keep entry at last_included_index for term verification

### Integration Points

#### RaftServer

```cpp
bool RaftServer::TakeSnapshotAndCompact(const char* data, size_t size) {
    // 1. Take snapshot at current commit index
    if (!snapshot_manager_->TakeSnapshot(commitIndex, currentTerm, data, size)) {
        return false;
    }

    // 2. Compact log entries up to snapshot
    if (log_storage_) {
        slotid_t first = log_storage_->first_slot_id().unwrap_or(1);
        log_storage_->remove_range(first, commitIndex);
    }

    return true;
}
```

#### PaxosServer

```cpp
bool PaxosServer::TakeSnapshotAndCompact(const char* data, size_t size) {
    // 1. Take snapshot at current commit slot
    if (!snapshot_manager_->TakeSnapshot(max_committed_slot_, cur_epoch, data, size)) {
        return false;
    }

    // 2. Compact log entries
    if (log_storage_) {
        log_storage_->remove_range(min_active_slot_, max_committed_slot_);
    }

    // 3. Update in-memory log
    FreeSlots();  // Already frees slots < max_executed_slot_ - 100

    return true;
}
```

### Recovery Integration

On recovery, if a snapshot exists:
1. Load latest snapshot
2. Set commit/execute index to snapshot's last_included_index
3. Replay only entries after last_included_index

This is handled in Phase 2 (already implemented).

## Implementation

Add `CompactLog()` method to both RaftServer and PaxosServer that:
1. Checks if snapshot manager is available
2. Gets the last snapshot's index
3. Removes log entries before that index

## Key Considerations

1. **Atomicity**: Snapshot must be persisted before log compaction
2. **Safety margin**: Keep some entries after snapshot for safety
3. **Concurrent access**: Acquire locks during compaction
4. **Memory**: Free in-memory entries too (Paxos logs_ map)

## Files Modified

- `src/deptran/raft/server.h`: Add CompactLog() declaration
- `src/deptran/raft/server.cc`: Add CompactLog() implementation
- `src/deptran/paxos/server.h`: Add CompactLog() declaration
- `src/deptran/paxos/server.cc`: Add CompactLog() implementation

## LOC Estimate

~80 LOC for compaction integration
