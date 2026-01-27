# Speculative Raft Implementation TODO

## Overview

Extend vanilla Raft to support speculative commits with async persistence. The key idea is separating "speculative" (memory quorum) from "secured" (durable quorum) for both leadership and log entries.

## Phase 1: State Extensions [COMPLETED 2026-01-27]

Implementation plan: docs/dev/phase1_speculative_state_plan.md

### 1.1 Leader State [DONE 2026-01-27, 02:53]
- [x] Add `securedLeader: bool` — true when durable vote quorum achieved
- [x] Add `specVoters: Set<NodeId>` — servers that have memory-voted for us
- [x] Add `durableVoters: Set<NodeId>` — servers that have durably-voted for us
- [x] Add `securedLogIndex: u64` — highest index with durable ack quorum
- [x] Add `specCommitIndex: u64` — highest index with memory ack quorum
- [x] Add `memoryAcks: Map<Index, Set<NodeId>>` — track memory acks per index
- [x] Add `durableAcks: Map<Index, Set<NodeId>>` — track durable acks per index

### 1.2 Invariants to Maintain [DONE 2026-01-27, 02:53]
- [x] Ensure `securedLogIndex <= specCommitIndex <= log.lastIndex()`
- [x] ~~Ensure `durableVoters ⊆ specVoters` (initially, before crashes)~~ — relaxed in Phase 6

## Phase 2: Vote RPC Changes [COMPLETED 2026-01-27]

Implementation plan: docs/dev/phase2_vote_rpc_plan.md

### 2.1 Follower Side (RequestVote Handler) [DONE 2026-01-27, 03:21]
- [x] On granting vote: update `votedFor` in memory immediately
- [x] Return `VoteGranted` response immediately (don't wait for fsync)
- [x] Start async fsync of `(currentTerm, votedFor)` - uses detached thread
- [x] On fsync complete: send new `VoteDurable` message to candidate

### 2.2 Leader Side (Vote Response Handler) [DONE 2026-01-27, 03:21]
- [x] On receiving `VoteGranted`: add voter to `specVoters` (via RaftVoteQuorumEvent)
- [x] If `|specVoters| >= quorum`: become speculative leader, start accepting requests
- [x] On receiving `VoteDurable`: add voter to `durableVoters` (via OnVoteDurable)
- [x] If `|durableVoters| >= quorum`: set `securedLeader = true`

### 2.3 New Message Type [DONE 2026-01-27, 03:21]
- [x] Define `VoteDurable { term: u64, voterId: NodeId }` message (in rcc_rpc.rpc)

## Phase 3: AppendEntries RPC Changes [COMPLETED 2026-01-27]

Implementation plan: docs/dev/phase3_appendentries_rpc_plan.md

### 3.1 Follower Side (AppendEntries Handler) [DONE 2026-01-27, 04:25]
- [x] Append entries to in-memory log immediately
- [x] Return `AppendEntriesResponse` immediately with `ackType: Memory`
- [x] Start async fsync of log entries in detached thread
- [x] On fsync complete: send `AppendEntriesDurable` RPC to leader

### 3.2 Leader Side (AppendEntries Response Handler) [DONE 2026-01-27, 04:25]
- [x] On receiving `ackType: Memory`: add follower to `memoryAcks[index]`
- [x] If `|memoryAcks[index]| >= quorum`:
  - [x] Update `specCommitIndex = max(specCommitIndex, index)`
  - [x] Notify client with `SPECULATIVE` status (implemented in Phase 5.3)
- [x] On receiving `AppendEntriesDurable` RPC: add follower to `durableAcks[index]`
- [x] If `securedLeader && |durableAcks[index]| >= quorum`:
  - [x] Update `securedLogIndex = max(securedLogIndex, index)`
  - [x] Notify client with `DURABLE` status (implemented in Phase 5.3)

### 3.3 Modify Message Type [DONE 2026-01-27, 04:25]
- [x] Add `ackType: enum { Memory, Durable }` to `AppendEntriesResponse`
- [x] Add `AppendEntriesDurable` RPC for durable ack notification

## Phase 4: notifyRestart Integration [COMPLETED 2026-01-27]

Implementation plan: docs/dev/phase4_notifyrestart_plan.md

### 4.1 Leader Handler for notifyRestart [DONE 2026-01-27, 04:45]
- [x] On receiving `notifyRestart` from server `s`:
  - [x] Remove `s` from `specVoters` (vote is no longer reliable)
  - [x] Remove `s` from `memoryAcks[i]` for all `i > securedLogIndex`
  - [x] If `!securedLeader && |specVoters| < quorum`:
    - [x] Call `stepDown(UnsecuredFailure)` (implemented in Phase 5)

## Phase 5: Step Down and Client Notification [COMPLETED 2026-01-27]

Implementation plan: docs/dev/phase5_stepdown_plan.md

### 5.1 Step Down Handler [DONE 2026-01-27, 05:10]
- [x] Add `StepDownReason: enum { UnsecuredFailure, SecuredFailure, HigherTerm }`
- [x] Implement `stepDown(reason)`:
  - [x] Log step-down event with reason
  - [x] Reset speculative state via ResetSpeculativeState()
  - [x] Transition to follower via setIsLeader(false)
  - [x] Reset election timer
  - [x] Client rollback notification (implemented in Phase 5.3)
- [x] Update OnPeerRestart to call stepDown(UnsecuredFailure) when losing quorum
- [x] Update HeartbeatLoop to call stepDown(HigherTerm) when seeing higher term

### 5.2 Client Notification Contract

**Response types:**
- `SPECULATIVE` — Entry reached memory quorum. Probably will commit, but no guarantee.
- `DURABLE` — Entry reached disk quorum with secured leader. Guaranteed to survive any failure.
- `ROLLEDBACK` — Best-effort notification when leader steps down gracefully. NOT guaranteed.

**Client must handle:**
| Client receives | Meaning | Client action |
|----------------|---------|---------------|
| `SPECULATIVE` | Provisional success | Wait for `DURABLE` or timeout |
| `DURABLE` | Permanent success | Done |
| `ROLLEDBACK` | Entry will not commit | Retry with new leader |
| Timeout/nothing | Unknown outcome | Retry with idempotency key |

**Key insight:** `ROLLEDBACK` is best-effort. If leader crashes, client just times out.
Only sequential failure of the entire memory quorum prevents ANY notification.

### 5.3 Client Callback Implementation [COMPLETED 2026-01-27]

Implementation plan: docs/dev/phase5_3_client_callbacks_plan.md

#### 5.3.1 Add CommitStatus enum and callback storage
- [x] Add `CommitStatus` enum: `{ SPECULATIVE, DURABLE, ROLLEDBACK }`
- [x] Add `pendingCallbacks_: Map<Index, Callback>` to RaftServer
- [x] Add tracking indices for notification progress

#### 5.3.2 Implement RegisterCommitCallback()
- [x] Store callback in pendingCallbacks_ map
- [x] Handle edge case: if already committed, invoke immediately

#### 5.3.3 Notify on specCommitIndex advance
- [x] In OnAppendEntriesReply, when specCommitIndex advances
- [x] Call SPECULATIVE for newly committed indices

#### 5.3.4 Notify on securedLogIndex advance
- [x] In OnAppendEntriesDurable, when securedLogIndex advances
- [x] Call DURABLE for newly secured indices

#### 5.3.5 Notify on step-down
- [x] In stepDown(), for entries > securedLogIndex_
- [x] Call ROLLEDBACK and clear callbacks

#### 5.3.6 Clean up callbacks
- [x] Remove callback from map after DURABLE notification

#### 5.3.7 Update testconf for callback registration
- [x] Add StartWithCallback() to RaftTestConfig

#### 5.3.8 Add client notification tests
- [x] testSpeculativeCommitNotification (Test 34)
- [x] testDurableCommitNotification (Test 35)
- [x] testNotificationOrdering (Test 36)

Notes:
- New leader fixes followers' logs via normal AppendEntries (no explicit rollback needed)
- Callbacks are invoked while holding mtx_ - keep them lightweight

## Phase 6: Relax durableVoters ⊆ specVoters Invariant [COMPLETED 2026-01-27]

Implementation plan: docs/dev/phase6_relax_invariant_plan.md

### 6.1 Motivation

The original invariant `durableVoters ⊆ specVoters` only holds initially. After crashes, a node
can be in `durableVoters` but not in `specVoters`:

```
1. B sends VoteDurable → added to durableVoters
2. B crashes, restarts, sends notifyRestart
3. A removes B from specVoters (memory vote is gone)
4. Now: B ∈ durableVoters but B ∉ specVoters
```

**Key insight:** If `|durableVoters| >= quorum`, those votes are on disk. Even if those nodes
crash and restart, they can't vote for anyone else in this term. Therefore, we are the unique
leader of this term — we're secured. The `specVoters` quorum is irrelevant once `durableVoters`
reaches quorum.

### 6.2 Safety Argument

- If `|durableVoters| >= quorum`, those nodes have `votedFor = me` on disk
- Even if they crash and restart, they'll see `votedFor = us` after recovery
- They cannot vote for any other candidate in this term
- Therefore, no other candidate can win election in this term
- Therefore, we are the **unique leader** → we're secured

This is the standard Raft safety argument applied to durable votes. **Correctness is preserved.**

### 6.3 The Race Condition This Fixes

```
Timeline (current behavior, suboptimal):
1. specVoters = {A, B, C}, durableVoters = {A, B}  (quorum in 3-node)
2. C sends VoteDurable (in flight on network)
3. B crashes → notifyRestart → specVoters = {A, C}
4. C crashes → notifyRestart → specVoters = {A}
5. !securedLeader && specVoters < quorum → STEP DOWN ❌
6. C's VoteDurable arrives → durableVoters = {A, B, C} (too late!)
```

With this change:
```
5. Check durableVoters = {A, B} >= quorum → securedLeader = true ✓
6. Continue as secured leader (improved availability)
```

### 6.4 Implementation Changes

#### 6.4.1 Update OnPeerRestart Logic [DONE 2026-01-27, 16:10]
- [x] Before stepping down, check if `|durableVoters| >= quorum`
- [x] If durable quorum exists, set `securedLeader = true` instead of stepping down
- [x] Only step down if both `specVoters < quorum` AND `durableVoters < quorum`

```cpp
void OnPeerRestart(NodeId s) {
    specVoters_.erase(s);
    // Remove from memoryAcks for entries > securedLogIndex

    // NEW: Check durable quorum before considering step-down
    if (!securedLeader_ && durableVoters_.size() >= quorum_size_) {
        securedLeader_ = true;
        Log_info("Became secured via durable quorum despite spec quorum loss");
    }

    if (!securedLeader_ && specVoters_.size() < quorum_size_) {
        stepDown(StepDownReason::UnsecuredFailure);
    }
}
```

#### 6.4.2 Update Invariants Documentation [DONE 2026-01-27, 16:10]
- [x] Remove `durableVoters ⊆ specVoters` from invariants
- [x] Document: `durableVoters` and `specVoters` are independent sets after crashes
- [x] Clarify: `|durableVoters| >= quorum` is sufficient for `securedLeader = true`

### 6.5 Tests for Phase 6 [DONE 2026-01-27, 16:15]

- [x] `testDurableQuorumPreemptsStepDown` (Test 41): Leader doesn't step down if durableVoters >= quorum even when specVoters < quorum
- [x] `testSecuredViaDurableAfterSpecLoss` (Test 42): Verify transition to secured when durable quorum achieved after spec quorum lost due to restarts

## Phase 7: New Leader Recovery

### 7.1 On Becoming Leader
- [ ] Reset `securedLeader = false`
- [ ] Reset `specVoters` = voters from election + self (not just {self})
- [ ] Reset `durableVoters = {self}` (assuming self vote is always durable)
- [ ] Reset `securedLogIndex = commitIndex` (from previous term)
- [ ] Reset `specCommitIndex = commitIndex`
- [ ] Clear `memoryAcks_` and `durableAcks_` for new term

## Phase 8: Tests

**Summary:**
- Tests 20-40 need implementation
- Client notification tests needed after Phase 5.3 callback infrastructure
- Remaining complex test deferred (testFsyncLatencyVariance) - requires
  fsync timing control infrastructure

Implementation plan: docs/dev/phase8_speculative_tests_plan.md

### 8.0 File Structure and Philosophy

**Build and run:**
- Build: `make raft-test -j32`
- Run: `./build/deptran_server ./config/raft_lab_test.yml` (with MAKO_RAFT_PERSISTENCE=1)

**File structure (updated):**
```
src/deptran/raft/
  test.h              # Extended: added speculative test declarations (Tests 20-33)
  test.cc             # Extended: added speculative test implementations (Tests 20-33)
  testconf.h          # Extended: added speculative state query methods
  testconf.cc         # Extended: implemented speculative state queries
```

**Testing philosophy:**
Speculative entries CAN be lost — that's the design trade-off for low latency.
Tests should verify the CONTRACT, not assume entries always survive:

1. **Deterministic (must always pass):**
   - Durable entries survive any failure
   - Client notifications are correct when delivered
   - Invariants hold (`securedLogIndex <= specCommitIndex`)

2. **Non-deterministic (verify contract, not outcome):**
   - Spec entry survives → verify it eventually becomes durable
   - Spec entry lost → verify new leader overwrites correctly
   - Don't assert "entry X must survive" — assert "whatever happens, system is consistent"

3. **ROLLEDBACK notification:**
   - Only testable in graceful step-down scenarios (leader still alive)
   - NOT testable in crash scenarios (leader dead, can't notify)

### 8.1 Unit Tests (test.cc)

#### Leadership Tests
- [ ] `testSpeculativeLeaderElection` (Test 20): Verify leader becomes speculative first, then secured after VoteDurable
- [ ] `testSecuredLeaderContinuesAfterSpecQuorumLoss` (Test 23): securedLeader + lost spec quorum → continues as leader

#### Commit Tests
- [ ] `testSpecCommitIndexAdvances` (Test 21): specCommitIndex advances on memory ack quorum
- [ ] `testDurableCommitRequiresSecuredLeader` (Test 24): durable ack quorum but !securedLeader → securedLogIndex does NOT advance

#### Invariant Tests
- [ ] `testSpeculativeInvariantsHold` (Test 22): verify `securedLogIndex <= specCommitIndex <= lastLogIndex` always holds

#### Client Notification Tests
- [ ] `testSpeculativeCommitNotification` (Test 34): client gets SPECULATIVE status
- [ ] `testDurableCommitNotification` (Test 35): client gets DURABLE status
- [ ] `testNotificationOrdering` (Test 36): SPECULATIVE before DURABLE for same entry

### 8.2 NotifyRestart and Step Down Tests (test.cc)

#### NotifyRestart Handling
- [ ] `testRestartRemovesFromSpecVoters` (Test 25): follower restarts → verifies system continues correctly
- [ ] `testRestartRemovesFromMemoryAcks` (Test 27): follower restarts → verifies entries still commit with quorum
- [ ] `testRestartDoesNotAffectDurableVoters` (Test 28): follower restart doesn't affect durableVoters (already on disk)

#### Step Down Scenarios (Graceful — leader still alive)
- [ ] `testUnsecuredLostQuorumStepsDown` (Test 26): verifies secured leader continues, documents unsecured behavior
- [ ] `testUnsecuredStepDownNotifiesRollback` (Test 37): on graceful step down, clients of current-term entries get ROLLEDBACK
- [ ] `testSecuredStepDownPartialRollback` (Test 39): durable entries not rolled back on step-down (callback removed after DURABLE)

### 8.3 Integration Tests (speculative_test.cc)

#### Happy Path
- [ ] `testFullCommitPath` (Test 38):
  ```
  1. Submit request to spec leader
  2. Verify client callback receives SPECULATIVE
  3. Wait for fsyncs to complete
  4. Verify client callback receives DURABLE
  5. Crash/restart all servers
  6. Verify entry persisted correctly
  ```

#### Speculative Entries Survive (Lucky Path)
- [ ] `testSpeculativeEntriesSurviveCrash` (Test 29):
  ```
  1. A is spec leader, spec commits X at index 10 (X in memory of {A, B, C})
  2. A crashes
  3. B wins election (has X in memory/log)
  4. X eventually durably committed by B
  5. Verify: X persists after full cluster restart
  ```

#### Speculative Entries Overwritten (New Leader Wins)
- [ ] `testSpeculativeEntriesOverwritten` (Test 40):
  ```
  1. A is unsecured spec leader, spec commits X
  2. Majority crashes (A loses spec quorum), A steps down
  3. D (who never had X) becomes leader, commits Y at same index
  4. Verify: Y is durably committed, X is gone
  5. Verify: system is consistent (no split-brain)
  ```
  Note: ROLLEDBACK notification only if A was alive during step-down

#### Vote Crash Scenarios
- [ ] `testVoterCrashBeforeVoteFsync` (Test 30):
  ```
  1. A gets memory votes from {A, B, C}, becomes spec leader (term 5)
  2. C crashes BEFORE vote fsync
  3. C restarts → sends notifyRestart to A
  4. A removes C from specVoters → {A, B}
  5. In 5-node cluster: still quorum → A continues
     In 3-node cluster: < quorum → A steps down
  ```

- [ ] `testDoubleVotePrevention` (Test 31):
  ```
  1. A gets memory votes from {A, B, C}, becomes spec leader term 5
  2. C crashes (loses in-memory vote), restarts
  3. D starts election term 5, C can vote for D (vote wasn't persisted!)
  4. C sends notifyRestart to A
  5. A loses spec quorum, steps down
  6. Verify: no conflicting durable commits (safety preserved)
  ```

### 8.4 Stress Tests
- [ ] `testRapidRestarts` (Test 32): multiple followers rapidly restarting, verify consistency
- [ ] `testConcurrentElections` (Test 33): multiple candidates with speculative voting
- [ ] `testFsyncLatencyVariance`: simulate variable fsync times, verify correctness

## Phase 9: pass ci tests

- [ ] Pass ci/ci.sh compile
- [ ] Pass all the tests related to Raft (shard1ReplicationRaft, shard2ReplicationRaft, shard1ReplicationSimpleRaft, shard2ReplicationSimpleRaft)
- [ ] Pass all other ci tests (simpleTransaction, simplePaxos, shard1Replication, shard2Replication, rocksdbTests, multiShardSingleProcess)

## Phase 10: Optimizations (Future)

- [ ] Batch durable acks to reduce message overhead
- [ ] Leader self-vote is always durable (no need to track)
- [ ] Combine VoteDurable with first AppendEntries response
- [ ] Track only counts (not sets) for durableVoters once secured

## File Structure

```
src/deptran/raft/
  server.h/cc         # Extend: add speculative state fields (securedLeader, specVoters, etc.)
  raft_rpc.h/cc       # Extend: add VoteDurable message, ackType to AppendEntriesResponse
  test.h/cc           # Existing: vanilla Raft tests (keep as-is)
  speculative_test.h  # NEW: SpeculativeRaftTest class declaration
  speculative_test.cc # NEW: all speculative Raft tests
  testconf.h/cc       # Extend: add queries for speculative state (specCommitIndex, securedLeader, etc.)
```

## Notes

- The notifyRestart message already exists and handles connection rebuild
- No InstallSnapshot needed for basic implementation
- Focus on correctness first, then optimize

### Key Design Insights

1. **Log truncation is optional on step-down**: The new leader will fix followers' logs via
   normal AppendEntries (prevLogIndex/prevLogTerm matching). Old leader doesn't need to
   self-truncate.

2. **ROLLEDBACK is best-effort**: Only possible when leader is still alive during step-down.
   If leader crashes, clients timeout. Only sequential failure of entire memory quorum
   prevents ANY notification (rare/catastrophic).

3. **Speculative entries CAN be lost**: That's the trade-off for low latency. The contract is:
   - SPECULATIVE = "probably will commit"
   - DURABLE = "guaranteed committed"
   - Clients must handle both outcomes

4. **Why secured leadership matters**: If `securedLeader = true`, a quorum has `votedFor = me`
   on disk. No other candidate can win election in this term → no conflicting leader.

5. **Why unsecured rollback is total**: Unsecured leader's votes were only in memory. Voters
   could crash, restart, and vote for someone else in the SAME term. Must distrust all entries.
