# Phase 3: AppendEntries RPC Changes for Speculative Replication

## Overview

Phase 3 extends the AppendEntries RPC to support speculative commits with async persistence.
The key idea is separating "memory acks" (immediate response after log append) from "durable acks"
(response after fsync), enabling low-latency speculative commits.

## Design Goals

1. **Low latency**: Followers respond immediately after appending to in-memory log
2. **Durability tracking**: Leader separately tracks memory vs durable acknowledgments
3. **Backward compatibility**: Existing RPC structure largely preserved with new ackType field
4. **Minimal overhead**: Durable ack sent via second response, not new RPC type

## Implementation Strategy

### 3.3 Modify Message Type (Must do first - prerequisite for 3.1 and 3.2)

**File: `src/deptran/rcc_rpc.rpc`**

Add ackType to AppendEntries response. The RPC file format doesn't support enums directly,
so we use uint64_t with 0=Memory, 1=Durable.

```
// Current:
defer AppendEntries(... |
      uint64_t followerAppendOK,
      uint64_t followerCurrentTerm,
      uint64_t followerLastLogIndex);

// New:
defer AppendEntries(... |
      uint64_t followerAppendOK,
      uint64_t followerCurrentTerm,
      uint64_t followerLastLogIndex,
      uint64_t followerAckType);  // 0=Memory, 1=Durable
```

**File: `src/deptran/raft/commo.h`**

Add ackType field to AppendEntriesResponse struct:
```cpp
struct AppendEntriesResponse {
  shared_ptr<IntEvent> event;
  uint64_t status = 0;
  uint64_t term = 0;
  uint64_t last_log_index = 0;
  uint64_t ack_type = 0;  // 0=Memory, 1=Durable (NEW)
};
```

Add AckType enum for clarity:
```cpp
enum class AckType : uint64_t {
  Memory = 0,
  Durable = 1
};
```

### 3.1 Follower Side (AppendEntries Handler)

**File: `src/deptran/raft/server.cc` - OnAppendEntries()**

Current behavior:
1. Validate term, prevLogIndex, prevLogTerm
2. Append entries to in-memory log
3. Call PersistLogEntry() synchronously
4. Update commitIndex
5. Return response

New behavior:
1. Validate term, prevLogIndex, prevLogTerm
2. Append entries to in-memory log
3. Return response immediately with ackType=Memory
4. Start async fsync in detached thread
5. On fsync complete: send second response with ackType=Durable

**Implementation approach:**

Instead of modifying the RPC to send two responses (complex), we use a simpler approach:
- The initial response is sent with ackType=Memory (after memory append, before fsync)
- A separate async "AppendEntriesDurable" RPC is sent after fsync completes

Wait - looking at the TODO.md more carefully:

> - [ ] On fsync complete: send `AppendEntriesResponse` with `ackType: Durable`

This implies a second response, not a new RPC. But srpc RPC framework doesn't support
multiple responses per request. So we have two options:

**Option A: New AppendEntriesDurable RPC (simpler)**
- Follower sends regular response with ackType=Memory
- After fsync, follower sends new `AppendEntriesDurable(term, follower_id, last_log_index)` RPC

**Option B: Piggyback on next heartbeat (less traffic but higher latency)**
- Track "pending durable" indices
- On next heartbeat response, include durable_last_log_index

**Decision: Use Option A** - New RPC for explicit durable notification, similar to VoteDurable pattern.

### 3.2 Leader Side (AppendEntries Response Handler)

**File: `src/deptran/raft/server.cc` - HeartbeatLoop()**

Current behavior (in response processing):
- On success: update match_index, next_index

New behavior:
- On ackType=Memory: add follower to memoryAcks_[lastLogIndex]
- If |memoryAcks_[index]| >= quorum: update specCommitIndex
- On ackType=Durable (via new RPC): add follower to durableAcks_[lastLogIndex]
- If securedLeader_ && |durableAcks_[index]| >= quorum: update securedLogIndex

**File: `src/deptran/raft/server.h` and `server.cc`**

Add new handler for AppendEntriesDurable RPC:
```cpp
void OnAppendEntriesDurable(const ballot_t& term,
                            const siteid_t& follower_id,
                            const uint64_t& last_log_index,
                            bool_t* acknowledged,
                            rusty::Function<void()> cb);
```

## New Files/Changes Summary

| File | Change |
|------|--------|
| `src/deptran/rcc_rpc.rpc` | Add `followerAckType` output param to AppendEntries; Add `AppendEntriesDurable` RPC |
| `src/deptran/raft/commo.h` | Add AckType enum; Add ack_type to AppendEntriesResponse |
| `src/deptran/raft/commo.cc` | Parse ack_type in callback; Add SendAppendEntriesDurable() |
| `src/deptran/raft/service.h` | Add AppendEntriesDurable RPC handler macro |
| `src/deptran/raft/service.cc` | Add HandleAppendEntriesDurable() |
| `src/deptran/raft/server.h` | Add OnAppendEntriesDurable() declaration |
| `src/deptran/raft/server.cc` | Modify OnAppendEntries() for async fsync; Add OnAppendEntriesDurable(); Modify HeartbeatLoop() for ack tracking |

## LOC Estimate

- Phase 3.3 (message type): ~30 LOC
- Phase 3.1 (follower side): ~80 LOC
- Phase 3.2 (leader side): ~100 LOC
- **Total: ~210 LOC** (well under 500 LOC limit)

## Testing Strategy

After implementation:
1. Run existing Raft tests to ensure backward compatibility
2. Verify memory acks are tracked correctly
3. Verify durable acks are sent and tracked after fsync
4. Verify specCommitIndex advances on memory quorum
5. Verify securedLogIndex advances on durable quorum (when securedLeader_)

## Dependencies

- Phase 1 (State Extensions): COMPLETED - provides memoryAcks_, durableAcks_, specCommitIndex_, securedLogIndex_
- Phase 2 (Vote RPC Changes): COMPLETED - provides VoteDurable pattern to follow
