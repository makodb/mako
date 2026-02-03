# Phase 5.3: Client Notification Callbacks

## Overview

This document describes the implementation plan for client notification callbacks
in the speculative Raft system.

## Design Goals

1. Notify clients when their entries reach speculative commit (memory quorum)
2. Notify clients when their entries reach durable commit (disk quorum with secured leader)
3. Notify clients of rollback when leader steps down (best-effort)

## Architecture Decisions

### Approach: Separate Registration Method

Instead of modifying the existing `Start()` signature (which has many callers),
we'll add a new `RegisterCommitCallback()` method:

```cpp
// Called after Start() to register a callback for notification
void RegisterCommitCallback(uint64_t index,
                            std::function<void(CommitStatus)> callback);
```

Benefits:
- Backwards compatible - existing callers of Start() continue to work
- Test code can optionally use callbacks without modifying all tests
- Clean separation of concerns

### Commit Status Enum

```cpp
enum class CommitStatus {
  SPECULATIVE,  // Entry reached memory quorum
  DURABLE,      // Entry reached disk quorum with secured leader
  ROLLEDBACK    // Entry will not commit (leader stepped down)
};
```

### Data Structures

```cpp
// In server.h, add to RaftServer class:
std::map<uint64_t, std::function<void(CommitStatus)>> pendingCallbacks_;
uint64_t lastSpecNotifiedIndex_ = 0;   // Track which indices we've notified
uint64_t lastDurableNotifiedIndex_ = 0;
```

## Implementation Tasks

### Task 5.3.1: Add CommitStatus enum and callback storage (~30 LOC)
- Add enum in server.h
- Add pendingCallbacks_ map
- Add tracking indices

### Task 5.3.2: Implement RegisterCommitCallback() (~20 LOC)
- Store callback in map
- Handle edge case: if already committed, invoke immediately

### Task 5.3.3: Notify on specCommitIndex advance (~30 LOC)
- In OnAppendEntriesReply, when specCommitIndex advances
- Call SPECULATIVE for indices (lastSpecNotifiedIndex, newSpecCommitIndex]

### Task 5.3.4: Notify on securedLogIndex advance (~30 LOC)
- In OnAppendEntriesDurable, when securedLogIndex advances
- Call DURABLE for indices (lastDurableNotifiedIndex, newSecuredLogIndex]

### Task 5.3.5: Notify on step-down (~30 LOC)
- In stepDown(), for entries > securedLogIndex_
- Call ROLLEDBACK and clear callbacks

### Task 5.3.6: Clean up callbacks for committed entries (~15 LOC)
- After notifying DURABLE, remove callback from map

### Task 5.3.7: Add tests (~150 LOC)
- testSpeculativeCommitNotification
- testDurableCommitNotification
- testNotificationOrdering

### Task 5.3.8: Update testconf to expose callback registration (~20 LOC)

## Total Estimated LOC: ~325

## Safety Considerations

1. Callbacks are invoked while holding mtx_ lock - keep callbacks lightweight
2. ROLLEDBACK is best-effort - only works if leader is alive during step-down
3. Callbacks must be thread-safe if they touch shared state

## Testing Strategy

1. Unit tests verify each notification type works
2. Integration tests verify ordering (SPECULATIVE before DURABLE)
3. Stress tests verify callbacks under rapid leadership changes
