# Phase 4: notifyRestart Integration for Speculative Replication

## Overview

When a follower restarts, it loses all in-memory state including:
1. Its memory vote (if it voted in the current term before persisting)
2. Memory-acked log entries (not yet fsynced)

The leader must handle notifyRestart by invalidating any speculative state
that depended on the restarted server.

## Design Goals

1. **Safety**: Remove restarted server from specVoters and memoryAcks
2. **Liveness**: Step down if we lose speculative quorum and aren't secured
3. **Simplicity**: Minimal changes to existing notifyRestart flow

## Implementation

### 4.1 Leader Handler Modifications

**File: `src/deptran/raft/service.cc` - HandleNotifyRestart()**

After reconnection handling, if this server is the leader:
1. Remove restarted server from `specVoters_`
2. Remove restarted server from `memoryAcks_[i]` for all `i > securedLogIndex_`
3. Check if we still have speculative quorum
4. If `!securedLeader_ && |specVoters_| < quorum`: step down

**New method in server.h/cc:**
```cpp
void RaftServer::OnPeerRestart(siteid_t restarted_site_id) {
    // Remove from specVoters (their vote is no longer reliable)
    specVoters_.erase(restarted_site_id);

    // Remove from memoryAcks for unsecured entries
    // Only entries > securedLogIndex are affected (secured entries are durable)
    for (auto& [idx, acks] : memoryAcks_) {
        if (idx > securedLogIndex_) {
            acks.erase(restarted_site_id);
        }
    }

    // Check if we've lost speculative vote quorum
    if (!securedLeader_) {
        size_t quorum = (partition_size / 2) + 1;
        if (specVoters_.size() < quorum) {
            // Step down - we're an unsecured leader who lost spec quorum
            // TODO (Phase 5): Implement stepDown(UnsecuredFailure)
            Log_warn("[SPEC-RAFT] Lost speculative quorum due to restart - would step down");
        }
    }
}
```

## LOC Estimate

~30 LOC total:
- server.h: 5 LOC (OnPeerRestart declaration)
- server.cc: 25 LOC (OnPeerRestart implementation)
- service.cc: ~5 LOC (call OnPeerRestart after reconnection)

## Testing Strategy

This is difficult to test without Phase 5's stepDown implementation.
For now, verify:
1. specVoters_ is updated on peer restart
2. memoryAcks_ is updated on peer restart
3. Log warnings when quorum would be lost
