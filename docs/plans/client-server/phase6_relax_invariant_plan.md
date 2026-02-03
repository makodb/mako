# Phase 6: Relax durableVoters ⊆ specVoters Invariant

## Problem Statement

The original invariant `durableVoters ⊆ specVoters` only holds initially. After crashes, a node
can be in `durableVoters` but not in `specVoters`:

```
1. B sends VoteDurable → added to durableVoters
2. B crashes, restarts, sends notifyRestart
3. A removes B from specVoters (memory vote is gone)
4. Now: B ∈ durableVoters but B ∉ specVoters
```

## Key Insight

If `|durableVoters| >= quorum`, those votes are on disk. Even if those nodes crash and restart,
they can't vote for anyone else in this term. Therefore, we are the unique leader of this term —
we're secured.

## Race Condition Being Fixed

```
Timeline (current behavior, suboptimal):
1. specVoters = {A, B, C}, durableVoters = {A, B}  (quorum in 3-node)
2. C sends VoteDurable (in flight on network)
3. B crashes → notifyRestart → specVoters = {A, C}
4. C crashes → notifyRestart → specVoters = {A}
5. !securedLeader && specVoters < quorum → STEP DOWN ❌
6. C's VoteDurable arrives → durableVoters = {A, B, C} (too late!)
```

With the fix:
```
5. Check durableVoters = {A, B} >= quorum → securedLeader = true ✓
6. Continue as secured leader (improved availability)
```

## Implementation Changes

### 6.4.1 Modify OnPeerRestart in server.cc

Current code (lines 2309-2318):
```cpp
// Check if we've lost speculative vote quorum (only matters if not secured)
if (!securedLeader_ && is_leader_) {
  size_t quorum = (Config::GetConfig()->GetPartitionSize(partition_id_) / 2) + 1;
  // +1 for our own vote
  size_t vote_count = specVoters_.size() + 1;
  if (vote_count < quorum) {
    // We've lost speculative quorum as an unsecured leader
    stepDown(StepDownReason::UnsecuredFailure);
    return;  // Don't verify invariants after stepping down
  }
}
```

New code:
```cpp
// Check if we need to become secured or step down
if (!securedLeader_ && is_leader_) {
  size_t quorum = (Config::GetConfig()->GetPartitionSize(partition_id_) / 2) + 1;

  // NEW: Check if durable quorum is sufficient for secured status
  // +1 for our own durable vote (self-vote is always durable)
  size_t durable_vote_count = durableVoters_.size() + 1;
  if (durable_vote_count >= quorum) {
    // We have durable quorum - become secured leader
    securedLeader_ = true;
    Log_info("[SPEC-RAFT] Site %d: Became secured via durable quorum (%zu/%zu) "
             "despite spec quorum loss", site_id_, durable_vote_count, quorum);
  } else {
    // Check speculative quorum
    size_t vote_count = specVoters_.size() + 1;
    if (vote_count < quorum) {
      // No durable quorum AND no speculative quorum - must step down
      stepDown(StepDownReason::UnsecuredFailure);
      return;
    }
  }
}
```

### 6.4.2 Update Invariants Documentation

In VerifySpeculativeInvariants() and comments, clarify:
- `durableVoters` and `specVoters` are independent sets after crashes
- `|durableVoters| >= quorum` is sufficient for `securedLeader = true`
- The invariant `durableVoters ⊆ specVoters` is explicitly relaxed

## Test Cases

### Test 41: testDurableQuorumPreemptsStepDown
- Setup: 5-node cluster, leader A with durableVoters = {B, C} (quorum = 3, +1 for self = 3)
- Action: D and E restart (removing from specVoters), making specVoters < quorum
- Expected: Leader doesn't step down because durableVoters >= quorum

### Test 42: testSecuredViaDurableAfterSpecLoss
- Setup: 5-node cluster, unsecured leader A
- Action: Follower B sends VoteDurable then restarts
- Expected: A becomes secured (durableVoters reaches quorum) despite B being removed from specVoters

## Safety Argument

If `|durableVoters| >= quorum`:
1. Those nodes have `votedFor = me` on disk
2. Even if they crash and restart, they'll see `votedFor = us` after recovery
3. They cannot vote for any other candidate in this term
4. Therefore, no other candidate can win election in this term
5. Therefore, we are the **unique leader** → we're secured

This is the standard Raft safety argument applied to durable votes.
