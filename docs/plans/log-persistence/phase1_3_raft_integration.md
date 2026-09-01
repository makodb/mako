# Phase 1.3: Raft Integration

## Overview

Integrate LogStorage into RaftServer to provide durable persistence for consensus state. This enables Raft nodes to recover their state after crashes.

## Current State Analysis

### RaftServer Location
- `src/deptran/raft/server.h` - Class definition (515 lines)
- `src/deptran/raft/server.cc` - Implementation (1591 lines)

### State That Must Persist

| Variable | Type | Description | Persistence Priority |
|----------|------|-------------|---------------------|
| `currentTerm` | uint64_t | Current term (monotonically increasing) | CRITICAL |
| `vote_for_` | siteid_t | Who we voted for in current term | CRITICAL |
| `logs_/raft_logs_` | map<slotid_t, RaftData> | Log entries | CRITICAL |
| `commitIndex` | uint64_t | Highest committed entry | HIGH |
| `lastLogIndex` | uint64_t | Index of last log entry | Derived from logs |

### Key Methods That Modify State

1. **OnRequestVote** (line 890) - Updates term and vote_for
2. **OnAppendEntries** (line 1042) - Updates term, vote_for, logs, commitIndex
3. **SetLocalAppend** (line 277) - Appends log entries (leader)
4. **RequestVote** (line 766) - Increments term, votes for self

## Design

### LogStorage Member

```cpp
// Add to RaftServer class
private:
    std::shared_ptr<srpc::LogStorage> log_storage_;

    // Metadata keys
    static constexpr const char* META_TERM = "currentTerm";
    static constexpr const char* META_VOTE_FOR = "vote_for";
    static constexpr const char* META_COMMIT_INDEX = "commitIndex";
```

### Persistence Strategy

1. **Term/Vote Updates**: Persist immediately with sync() before responding
2. **Log Entries**: Persist with optional batching, sync() before acking
3. **Commit Index**: Persist without sync (can be recovered from logs)

### New Methods

```cpp
// Initialize LogStorage
void SetLogStorage(std::shared_ptr<srpc::LogStorage> storage);

// Recover state from storage
bool RecoverFromStorage();

// Helper to convert RaftData to LogEntry
srpc::LogEntry ToLogEntry(slotid_t slot_id, const RaftData& data);

// Truncate log entries after a given index
void TruncateLogAfter(slotid_t index);
```

### Integration Points

#### 1. OnRequestVote (line 890)
```cpp
// After term update (line ~149)
if (can_term > currentTerm) {
    currentTerm = can_term;
    vote_for_ = INVALID_SITEID;
    PersistTermAndVote();  // NEW
}

// After granting vote (line ~157)
if (vote) {
    vote_for_ = can_id;
    PersistVote();  // NEW
}
```

#### 2. OnAppendEntries (line 1042)
```cpp
// After term update from leader (line ~1075-1078)
if (leaderCurrentTerm > currentTerm) {
    currentTerm = leaderCurrentTerm;
    vote_for_ = INVALID_SITEID;
    PersistTermAndVote();  // NEW
}

// After appending entries (batch optimization path)
PersistLogEntries(entries);  // NEW

// After commitIndex update (line ~1126-1129)
if (leaderCommitIndex > commitIndex) {
    commitIndex = min(leaderCommitIndex, lastLogIndex);
    PersistCommitIndex();  // NEW
}
```

#### 3. SetLocalAppend (line 277)
```cpp
// After appending entry
auto instance = GetRaftInstance(lastLogIndex);
instance->log_ = cmd;
instance->term = currentTerm;
PersistLogEntry(lastLogIndex, *instance);  // NEW
```

#### 4. RequestVote (line 766)
```cpp
// After incrementing term for election (line ~792-793)
currentTerm++;
vote_for_ = site_id_;
PersistTermAndVote();  // NEW
```

#### 5. Constructor / Initialization
```cpp
// After initialization
if (log_storage_ && log_storage_->is_open()) {
    RecoverFromStorage();
}
```

## Implementation Tasks

### Task 1: Add LogStorage Integration to Header (~50 LOC)
- Add `log_storage_` member
- Add metadata key constants
- Declare new methods

### Task 2: Implement Persistence Helpers (~80 LOC)
- `SetLogStorage()`
- `PersistTermAndVote()`
- `PersistVote()`
- `PersistLogEntry()`
- `PersistLogEntries()`
- `PersistCommitIndex()`

### Task 3: Implement Recovery (~60 LOC)
- `RecoverFromStorage()`
- `ToLogEntry()` helper
- `TruncateLogAfter()`

### Task 4: Integrate Persistence Calls (~60 LOC)
- Modify `OnRequestVote`
- Modify `OnAppendEntries`
- Modify `SetLocalAppend`
- Modify `RequestVote`

### Task 5: Unit Tests (~100 LOC)
- Test state persistence
- Test recovery after crash
- Test log truncation

## File Changes

| File | Changes |
|------|---------|
| `src/deptran/raft/server.h` | Add log_storage_ member, add helper method declarations |
| `src/deptran/raft/server.cc` | Implement persistence helpers, integrate calls |
| `test/raft_persistence_test.cc` | New test file |
| `CMakeLists.txt` | Add test |

## Estimated LOC

| Component | LOC |
|-----------|-----|
| Header changes | 50 |
| Persistence helpers | 80 |
| Recovery implementation | 60 |
| Integration changes | 60 |
| Unit tests | 100 |
| **Total** | **~350** |

## RustyCpp Compliance

- Use `std::shared_ptr<LogStorage>` (LogStorage interface uses rusty types internally)
- Mark methods as `@safe` or `@unsafe` as appropriate
- Use `rusty::Option` where applicable

## Success Criteria

1. Term and vote_for persist across restarts
2. Log entries persist across restarts
3. Commit index recovered correctly
4. No data loss for committed entries
5. All existing tests still pass
6. New persistence tests pass
