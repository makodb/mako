# Phase 5: Step Down and Client Notification

## Overview

When a speculative leader needs to step down, different situations require
different handling:
- UnsecuredFailure: Lost speculative quorum, all current-term entries are suspect
- SecuredFailure: Lost quorum but was secured, only unsecured entries are suspect
- HigherTerm: Saw higher term, entries may still be valid (not necessarily rollback)

## Implementation Scope

### 5.1 Step Down Handler (This Phase)

Add a stepDown(reason) function that:
1. Records the reason for stepping down
2. Logs the event for debugging/auditing
3. Calls existing setIsLeader(false) to transition to follower
4. Resets election timer

**Note**: Client notification (5.2, 5.3) requires significant infrastructure:
- Tracking pending client callbacks per log index
- Integration with transaction processing layer
- This is deferred as it requires understanding Mako's client callback system

### 5.2 Client Notification (Deferred)

The client notification system requires:
- Map<Index, ClientCallback> for tracking pending callbacks
- Integration with transaction layer to capture callbacks
- SPECULATIVE/DURABLE/ROLLEDBACK status codes
- This is deferred as it's a larger architectural change

### 5.3 Phase 6 Already Done

Phase 6 (New Leader Recovery) is already implemented:
- RequestVote() initializes specVoters_, durableVoters_, etc. when winning election
- setIsLeader(true) handles leader-specific initialization
- No additional work needed

## Implementation

### StepDownReason enum

```cpp
enum class StepDownReason {
  UnsecuredFailure,  // Lost spec quorum while unsecured
  SecuredFailure,    // Lost quorum but was secured
  HigherTerm         // Saw higher term from another server
};
```

### stepDown() function

```cpp
void RaftServer::stepDown(StepDownReason reason) {
  // Log the reason
  Log_info("[SPEC-RAFT] Site %d: Stepping down (reason=%s)", ...);

  // TODO (future): Notify pending clients based on reason
  // if (reason == UnsecuredFailure) notifyClientsRollback(all current-term entries)
  // else if (reason == SecuredFailure) notifyClientsRollback(unsecured entries)

  // Reset speculative state
  ResetSpeculativeState();

  // Become follower
  setIsLeader(false);

  // Reset timer
  resetTimer("step down");
}
```

## LOC Estimate

~50 LOC total:
- server.h: 15 LOC (enum, function declaration)
- server.cc: 35 LOC (stepDown implementation)

## Usage

Replace existing step-down code with stepDown() calls:
- OnPeerRestart when losing quorum -> stepDown(UnsecuredFailure)
- HeartbeatLoop when seeing higher term -> stepDown(HigherTerm)
