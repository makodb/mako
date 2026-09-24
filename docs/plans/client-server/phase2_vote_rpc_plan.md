# Phase 2: Vote RPC Changes - Implementation Plan

## Overview

This phase implements speculative voting for Raft elections. The key idea is to separate memory votes (immediate response) from durable votes (after fsync). This allows faster leader election while maintaining safety.

## Current Implementation Analysis

### Vote Flow (Current)
1. Candidate increments term, votes for self, persists (sync)
2. Candidate broadcasts RequestVote RPCs
3. Each follower checks vote conditions, persists vote (sync), responds
4. Candidate waits for quorum of responses
5. If quorum achieved → becomes leader

### Problem
- Vote response is blocked on fsync (can be 10-100ms per vote)
- Total election time = RTT + max(fsync times)

### Solution
- Respond with VoteGranted immediately (memory vote)
- Send VoteDurable message after fsync completes (durable vote)
- Leader tracks both vote types separately

## Implementation Details

### 2.1 Follower Side Changes

**File: server.cc - OnRequestVote**

Current code (simplified):
```cpp
void OnRequestVote(...) {
    // ... vote decision logic ...
    if (should_grant_vote) {
        vote_for_ = can_id;
        PersistState(...);  // BLOCKS on fsync
        doVote(..., true);  // Responds after persist
    }
}
```

New code:
```cpp
void OnRequestVote(...) {
    // ... vote decision logic ...
    if (should_grant_vote) {
        vote_for_ = can_id;
        // Respond IMMEDIATELY (memory vote)
        doVote(..., true);  // Responds now

        // Start async persistence
        StartAsyncVotePersist(can_id, can_term);
    }
}

void StartAsyncVotePersist(siteid_t can_id, ballot_t term) {
    // Schedule async fsync
    // On complete: send VoteDurable RPC to candidate
}

void OnVoteFsyncComplete(siteid_t can_id, ballot_t term) {
    // Send VoteDurable { term, voterId: site_id_ } to candidate
    if (auto proxy = GetProxyFor(can_id)) {
        proxy->async_VoteDurable(term, site_id_);
    }
}
```

**Key Changes:**
- Remove blocking fsync before response
- Add async fsync with callback
- Send VoteDurable RPC on fsync complete

### 2.2 Leader Side Changes

**File: server.cc - RequestVote / Vote Response Handler**

Current code:
```cpp
auto sp_quorum = commo->BroadcastVote(...);
sp_quorum->wait(timeout);
if (sp_quorum->yes()) {
    setIsLeader(true);
}
```

New code:
```cpp
auto sp_quorum = commo->BroadcastVote(...);
sp_quorum->wait(timeout);
if (sp_quorum->yes()) {
    // Memory quorum achieved → speculative leader
    specVoters_ = sp_quorum->GetVoters();
    specVoters_.insert(site_id_);  // self vote
    securedLeader_ = false;
    ResetSpeculativeState();
    setIsLeader(true);
}
```

**New VoteDurable Handler:**
```cpp
void OnVoteDurable(ballot_t term, siteid_t voterId) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    if (term != currentTerm) return;  // Stale
    if (!is_leader_) return;          // Not leader anymore

    durableVoters_.insert(voterId);

    // Check if secured quorum reached
    size_t quorum = GetQuorumSize();
    if (!securedLeader_ && durableVoters_.size() >= quorum) {
        securedLeader_ = true;
        Log_info("[SPEC-RAFT] Site %d: Became secured leader with %zu durable votes",
                 site_id_, durableVoters_.size());
    }
}
```

### 2.3 New VoteDurable Message Type

**File: rcc_rpc.rpc (RPC definition)**

Add new RPC:
```
service RaftService {
    existing Vote(...);

    // NEW: Durable vote confirmation
    VoteDurable(u64 term, siteid_t voter_id) -> (bool_t ack);
}
```

**File: service.h/cc**

Add handler:
```cpp
RpcHandler(VoteDurable, 2,
           const uint64_t&, term,
           const siteid_t&, voter_id,
           bool_t*, ack) {
    *ack = false;
}

void RaftServiceImpl::HandleVoteDurable(
    const uint64_t& term,
    const siteid_t& voter_id,
    bool_t* ack,
    srpc::DeferredReply defer) {
    RaftServer* svr = GetServer();
    if (svr) {
        svr->OnVoteDurable(term, voter_id);
        *ack = true;
    }
    defer.reply();
}
```

## Async Persistence Implementation

### Option A: Background Thread (Simple)
```cpp
void StartAsyncVotePersist(siteid_t can_id, ballot_t term) {
    std::thread([this, can_id, term]() {
        PersistState(currentTerm, vote_for_, "async vote persist");
        OnVoteFsyncComplete(can_id, term);
    }).detach();
}
```

### Option B: Event Loop Integration (Production)
Use existing Reactor/PollMgr for non-blocking fsync with callback.

### Recommendation
Start with Option A for simplicity. Option B can be optimized later.

## Thread Safety

All new operations must hold `mtx_`:
- `specVoters_`, `durableVoters_` modifications
- `securedLeader_` state changes
- VoteDurable RPC handling

## Testing Strategy

1. **Unit test**: Verify specVoters populated on vote response
2. **Unit test**: Verify durableVoters populated on VoteDurable RPC
3. **Unit test**: Verify securedLeader transitions correctly
4. **Integration test**: Full election with spec→secured transition

## File Changes Summary

| File | Changes |
|------|---------|
| `rcc_rpc.rpc` | Add VoteDurable RPC definition |
| `rcc_rpc.h` | Regenerated from rpc file |
| `service.h` | Add VoteDurable handler declaration |
| `service.cc` | Add HandleVoteDurable implementation |
| `server.h` | Add OnVoteDurable, StartAsyncVotePersist declarations |
| `server.cc` | Modify OnRequestVote, add VoteDurable handling |
| `commo.h` | Extend RaftVoteQuorumEvent to track voters |
| `commo.cc` | Add VoteDurable RPC sending |

## Invariants

1. `specVoters_.size() >= durableVoters_.size()` (always)
2. `securedLeader_ == true` implies `durableVoters_.size() >= quorum`
3. If `securedLeader_ == false`, speculative entries may be rolled back

## Rollback Safety

If candidate becomes spec leader but not secured:
- New leader could emerge in same term (voter crashes, revotes)
- Spec leader must handle notifyRestart by removing from specVoters
- If specVoters drops below quorum → step down

This is handled in Phase 4 (notifyRestart integration).
