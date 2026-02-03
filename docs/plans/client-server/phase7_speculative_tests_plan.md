# Phase 7: Speculative Raft Tests

## Overview

Add unit tests for speculative Raft functionality. Tests extend the existing
RaftLabTest framework in test.h/cc.

## Test Structure

Rather than creating separate speculative_test.{h,cc} files, we extend the
existing test infrastructure to add speculative Raft tests. This approach:
1. Reuses RaftTestConfig infrastructure (Kill, Restart, Disconnect, etc.)
2. Keeps all Raft tests in one place
3. Simpler build integration

## Implementation Approach

### Phase 7.1: Extend testconf.h/cc

Add methods to RaftTestConfig to query speculative state:
```cpp
// Query speculative state from a server
bool IsSecuredLeader(siteid_t svr);
uint64_t GetSpecCommitIndex(siteid_t svr);
uint64_t GetSecuredLogIndex(siteid_t svr);
size_t GetSpecVotersCount(siteid_t svr);
size_t GetDurableVotersCount(siteid_t svr);
```

### Phase 7.1: Add Tests to test.cc

Add test methods to RaftLabTest class:
1. `testSpeculativeLeaderElection` - Verify leader becomes speculative first
2. `testSecuredLeaderVoteDurable` - Verify VoteDurable advances to secured
3. `testSpecCommitIndexAdvances` - Verify specCommitIndex advances on memory quorum
4. `testInvariantsHold` - Verify securedLogIndex <= specCommitIndex always

### Build Configuration

Tests use existing build: `make raft-test -j32`
Run with: `./build/deptran_server ./config/raft_lab_test.yml`

## LOC Estimate

- testconf.h: ~20 LOC (declarations)
- testconf.cc: ~50 LOC (implementations)
- test.h: ~10 LOC (test declarations)
- test.cc: ~150 LOC (test implementations)
- **Total: ~230 LOC**

## Test List (Phase 7.1)

1. **testSpeculativeLeaderElection**
   - Start election
   - Leader wins with memory votes
   - Verify securedLeader = false initially
   - Wait for VoteDurable
   - Verify securedLeader = true

2. **testSpecCommitIndexAdvances**
   - Submit entry to leader
   - Verify specCommitIndex advances when memory quorum reached
   - Verify securedLogIndex advances when durable quorum reached (if secured)

3. **testInvariantsHold**
   - Submit multiple entries
   - At all times verify: securedLogIndex <= specCommitIndex <= lastLogIndex

## Dependencies

- Requires existing test infrastructure (RaftTestConfig)
- Requires RAFT_TEST_CORO build flag
- Requires public accessors for speculative state (already added in Phase 1)
