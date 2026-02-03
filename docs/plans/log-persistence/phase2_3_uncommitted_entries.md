# Phase 2.3: Uncommitted Entry Handling

## Problem Statement

After recovery, there may be uncommitted entries in the log:
- **Raft**: `lastLogIndex > commitIndex` - entries that were replicated but not committed
- **Paxos**: `max_accepted_slot_ > max_committed_slot_` - entries that got Accept but not Commit

These entries need to be resolved through the consensus protocol:
1. If the entry reached quorum, it will eventually be committed
2. If a conflicting entry was committed elsewhere, the entry will be overwritten

## Analysis

### Raft Handling (Already Implemented)

The Raft protocol handles uncommitted entries correctly:

1. **OnAppendEntries** checks:
   - `term_ok = (leaderCurrentTerm >= this->currentTerm)`
   - `index_ok = (leaderPrevLogIndex <= this->lastLogIndex)`
   - `prev_term_ok = (leaderPrevLogIndex == 0 || local_prev_term == leaderPrevLogTerm)`

2. If a recovering node becomes **follower**:
   - Leader will send AppendEntries
   - If logs conflict, leader backtracks until logs match
   - Uncommitted entries are overwritten by leader's entries

3. If a recovering node becomes **leader**:
   - HeartbeatLoop will replicate uncommitted entries
   - Entries will be committed when majority responds

### Paxos Handling (Already Implemented)

The Paxos protocol handles uncommitted entries:

1. **OnBulkPrepare/OnPrepare** checks ballot and returns accepted values
2. **OnBulkAccept/OnAccept** accepts values with higher ballot
3. Uncommitted entries can be overwritten by new leader with higher epoch

## Implementation

Phase 2.3 adds monitoring and logging for uncommitted entries, since the consensus
protocols already handle them correctly.

### RaftServer Changes (~15 LOC)

**Add to server.h:**
```cpp
// @safe - Returns count of uncommitted entries
size_t GetUncommittedCount() const;
```

**Add to server.cc:**
```cpp
size_t RaftServer::GetUncommittedCount() const {
  if (lastLogIndex > commitIndex) {
    return lastLogIndex - commitIndex;
  }
  return 0;
}
```

**Modify ReplayCommittedEntries() to log uncommitted count:**
```cpp
size_t uncommitted = GetUncommittedCount();
if (uncommitted > 0) {
  Log_info("[RAFT-RECOVERY] Site %d: %zu uncommitted entries (lastLogIndex=%lu, commitIndex=%lu) - will be resolved by consensus",
           site_id_, uncommitted, lastLogIndex, commitIndex);
}
```

### PaxosServer Changes (~15 LOC)

**Add to server.h:**
```cpp
// @safe - Returns count of uncommitted entries
size_t GetUncommittedCount() const;
```

**Add to server.cc:**
```cpp
size_t PaxosServer::GetUncommittedCount() const {
  if (max_accepted_slot_ > max_committed_slot_) {
    return max_accepted_slot_ - max_committed_slot_;
  }
  return 0;
}
```

**Modify ReplayCommittedEntries() to log uncommitted count:**
```cpp
size_t uncommitted = GetUncommittedCount();
if (uncommitted > 0) {
  Log_info("[PAXOS-RECOVERY] Site par %d loc %d: %zu uncommitted entries (max_accepted=%lu, max_committed=%lu) - will be resolved by consensus",
           partition_id_, loc_id_, uncommitted, max_accepted_slot_, max_committed_slot_);
}
```

## Key Points

1. **No Special Action Required**: The consensus protocols already handle uncommitted entries correctly
2. **Logging for Visibility**: Add logging so operators can see if there are uncommitted entries
3. **Count Method**: Add method to query uncommitted count for monitoring

## Files Modified

- `src/deptran/raft/server.h` - Add GetUncommittedCount declaration
- `src/deptran/raft/server.cc` - Add implementation and logging
- `src/deptran/paxos/server.h` - Add GetUncommittedCount declaration
- `src/deptran/paxos/server.cc` - Add implementation and logging

## Testing

1. Run `./ci/ci.sh shard1Replication` - Paxos test
2. Run `./ci/ci.sh shard1ReplicationRaft` - Raft test

Both should pass. Fresh start will show "No entries to replay" and no uncommitted entries.

## LOC Estimate

~30 LOC total (within <500 LOC limit)
