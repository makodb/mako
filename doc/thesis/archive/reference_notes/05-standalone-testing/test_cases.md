# Individual Test Case Documentation

## 1. Overview

The standalone Raft test suite contains 11 sequential test cases that
verify correctness properties of the Raft consensus implementation.  Tests
are executed in order by `RaftLabTest::Run()` (`test.cc:10-37`) via a
short-circuit OR chain — the first failure stops execution.

State is carried between tests: `index_` tracks the next expected commit
index, `init_rpcs_` records initial election RPCs, and `committed_cmds`
accumulates per-server commit logs.

Each test begins with `Init2(id, desc)` which verifies preconditions:
no servers are disconnected and the network is reliable.

## 2. Test 1: Initial Election (`testInitialElection`)

**Source**: `test.cc:91-139`

**What it tests**: A leader is elected from a fresh 5-server cluster,
all servers agree on a single term, and leadership is stable.

**Procedure**:

1. Sleep `ELECTIONTIMEOUT / 10` (500 ms) to allow election timers to start
2. Call `OneLeader()` — assert a leader exists
3. Record `init_rpcs_` by summing `RpcCount()` across all 5 servers
4. Call `OneTerm()` — assert all servers agree on the same term
5. Call `OneTerm()` again — assert the term hasn't changed
6. Call `OneLeader(leader)` — assert the same server is still leader

**Expected outcome**: Exactly one leader, unanimous term, stable
leadership.

**What bugs it catches**:
- Election timers that never fire
- Split brain (multiple leaders in the same term)
- Spurious re-elections in a healthy cluster

## 3. Test 2: Re-Election (`testReElection`)

**Source**: `test.cc:141-237`

**What it tests**: The cluster elects a new leader after the current
leader is disconnected, and correctly handles quorum loss and restoration.

**Procedure**:

1. Find current leader via `OneLeader()`
2. `Disconnect(leader)` — simulate leader failure
3. Sleep `ELECTIONTIMEOUT` (5 s), call `OneLeader()` — assert new leader
   differs from old
4. `Reconnect(oldLeader)` — old leader rejoins
5. Sleep `ELECTIONTIMEOUT`, call `OneLeader(leader)` — assert current
   leader unchanged (old leader doesn't disrupt)
6. Disconnect leader + 2 more servers (total 3/5 down) — no quorum
7. Call `NoLeader()` — assert no leader can be elected
8. Reconnect one server (3/5 up) — quorum restored
9. Sleep `ELECTIONTIMEOUT`, call `OneLeader()` — leader elected
10. Reconnect remaining servers, verify stable leadership

**Expected outcome**: New leader after disconnect, no leader without
quorum, leader elected when quorum restored.

**What bugs it catches**:
- Failure to detect leader timeout
- Leader elected without majority (safety violation)
- Old leader not stepping down after reconnection
- Incorrect quorum calculation

## 4. Test 3: Basic Agreement (`testBasicAgree`)

**Source**: `test.cc:239-267`

**What it tests**: Three sequential agreements are committed by all 5
servers at the expected log indices.

**Procedure**:

For `i` in 1..3:
1. `AssertNoneCommitted(index_)` — no premature commits at this index
2. `DoAgreeAndAssertIndex(index_+300, NSERVERS, index_)` — submit
   command, wait for all 5 servers to commit, verify index
3. Increment `index_`

Command values are `index_ + 300` (e.g., 301, 302, 303).

**Expected outcome**: Three commits at consecutive indices, all 5
servers agree on values.

**What bugs it catches**:
- Log replication failures
- Index assignment errors
- Value corruption during replication

## 5. Test 4: Agreement Despite Follower Failure (`testFailAgree`)

**Source**: `test.cc:269-293`

**What it tests**: Agreements succeed when 2 of 5 followers are
disconnected (quorum = 3 servers), and disconnected followers catch up
after reconnection.

**Procedure**:

1. Find leader, disconnect 2 followers (next 1, next 2 relative to leader)
2. Submit 4 agreements (401-404) with `n = NSERVERS - 2` (3 servers)
3. Reconnect both followers, sleep `ELECTIONTIMEOUT`
4. Submit 2 more agreements (405-406) with `n = NSERVERS` (all 5)

**Expected outcome**: First 4 agreements commit with 3 servers.  After
reconnection, followers catch up and all 5 servers commit the last 2.

**What bugs it catches**:
- Quorum logic requiring more than majority
- Log replication not resuming after reconnection
- Followers failing to catch up via AppendEntries

## 6. Test 5: No Agreement Without Quorum (`testFailNoAgree`)

**Source**: `test.cc:295-319`

**What it tests**: When 3 of 5 followers are disconnected, `Start()`
accepts commands (leader still thinks it's leader) but they cannot be
committed.

**Procedure**:

1. Find leader, disconnect 3 followers
2. `Start(leader, 501)` — assert it returns true (leader accepts)
3. Verify returned index and positive term
4. Sleep `ELECTIONTIMEOUT`
5. `AssertNoneCommitted(index)` — command not committed (no quorum)
6. Reconnect all 3 followers, sleep `ELECTIONTIMEOUT`
7. `DoAgreeAndAssertWaitSuccess(502, NSERVERS)` — agreement works again

**Expected outcome**: Command accepted but not committed without quorum.
After quorum restored, new agreements succeed.

**What bugs it catches**:
- Leader committing without majority (safety violation)
- Leader rejecting commands when it should accept
- System unable to recover after quorum restoration

## 7. Test 6: Rejoin of Disconnected Leader (`testRejoin`)

**Source**: `test.cc:321-356`

**What it tests**: An old leader that accumulated uncommitted entries
(602-604) correctly discards them after rejoining, and the cluster
continues operating through multiple leader changes.

**Procedure**:

1. Commit entry 601 with all servers
2. Disconnect leader1, sleep `ELECTIONTIMEOUT`
3. `Start()` entries 602-604 on disconnected leader1 (these cannot commit)
4. New leader2 commits entries 605-606 with `NSERVERS - 1`
5. Disconnect leader2
6. Reconnect leader1
7. Sleep `ELECTIONTIMEOUT` — leader3 elected (not leader2)
8. Commit entries 607-608 with `NSERVERS - 1`
9. Reconnect leader2
10. Commit entry 609 with all `NSERVERS`

**Expected outcome**: Entries 602-604 are overwritten by the new leader's
log.  The cluster survives two consecutive leader changes.

**What bugs it catches**:
- Old leader's uncommitted entries persisting incorrectly
- AppendEntries not overwriting conflicting entries
- Failure to elect leader after multiple disconnections

## 8. Test 7: Concurrent Starts (`testConcurrentStarts`)

**Source**: `test.cc:382-446`

**What it tests**: 5 concurrent `pthread` `Start()` calls to the same
leader all result in committed entries with correct values.

**Procedure** (retried up to 5 times if term changes):

1. Find leader, call `Start(leader, 701)` to verify leadership
2. Launch 5 `pthread` threads, each calling `Start(leader, 701+i)`
3. Join all threads, collect returned indices
4. If term moved on, retry from step 1
5. `Wait()` for each returned index to be committed by all servers
6. Verify all 5 command values (701-705) appear in the committed set

**Expected outcome**: All 5 concurrent commands committed at distinct
indices with correct values.

**What bugs it catches**:
- Race conditions in `Start()` / log append
- Duplicate index assignment
- Lost commands under concurrency

## 9. Test 8: Leader Backs Up Quickly (`testBackup`)

**Source**: `test.cc:448-492`

**What it tests**: When a leader accumulates 50 uncommitted entries on
a minority, and then a new leader with different entries takes over, the
old leader's log is replaced quickly.

**Procedure**:

1. Disconnect 3 followers (leaving leader1 + 1 follower)
2. `Start()` 50 entries (800-849) on leader1 — cannot commit (no quorum)
3. Disconnect leader1 + its follower, reconnect the 3 other servers
4. Sleep `ELECTIONTIMEOUT` — new leader elected among the 3
5. `DoAgreeAndAssertIndex()` 50 entries (801-850) with 3 servers
6. Reconnect old leader1 + follower
7. Sleep `ELECTIONTIMEOUT`, submit entry 851 via new leader
8. Sleep `2 * ELECTIONTIMEOUT`
9. `AssertNCommitted(index, NSERVERS)` — all 5 servers committed

**Expected outcome**: The 50 incorrect entries on the old minority are
replaced by the 50 correct entries.  All 5 servers converge after
reconnection.

**What bugs it catches**:
- Slow log backfill (O(n) AppendEntries retries instead of efficient
  nextIndex backup)
- Conflicting entries not overwritten
- Old leader refusing to accept new leader's entries

## 10. Test 9: RPC Count Verification (`testCount`)

**Source**: `test.cc:494-572`

**What it tests**: The Raft implementation does not send excessive RPCs.
Checks three scenarios: initial election, agreement rounds, and idle
period.

**Procedure** (retried up to 5 times):

1. Assert `init_rpcs_ > 1 && init_rpcs_ <= 30` (from test 1)
2. Reset RPC counters, find leader
3. `Start()` 10 entries sequentially to the leader
4. `Wait()` for all 10 to be committed by all servers
5. `rpcs()` — assert total RPCs `<= COMMITRPCS(10)` = 55
6. Sleep 1 second (idle period)
7. `rpcs()` — assert total RPCs `<= 60` during idle

**Expected outcome**: Bounded RPC counts — efficient heartbeating and
replication.

**What bugs it catches**:
- Excessive heartbeat frequency
- Redundant AppendEntries retransmissions
- Broadcasting to all servers on every heartbeat even when logs are
  up to date

**RPC bounds**:

| Scenario | Upper Bound | Formula |
|----------|-------------|---------|
| Initial election | 30 | Empirical |
| 10 agreements | 55 | `(10 + 1) * 5` |
| 1 second idle | 60 | Empirical |

## 11. Test 10: Unreliable Agreement (`testUnreliableAgree`)

**Source**: `test.cc:593-631`

**What it tests**: Agreements complete correctly under simulated
unreliable network conditions (random disconnections and delays).

**Procedure**:

1. `SetUnreliable(true)` — activates `netctlLoop` (1/10 disconnect
   chance, 0-26 ms random delays per 100 ms period)
2. For 50 iterations:
   a. Launch 4 `pthread` threads, each calling
      `DoAgreement(1000+iter, 1, true)` (retry enabled)
   b. If any thread fails, record failure and break
   c. Main thread also calls `DoAgreement(1000+iter, 1, true)`
3. `SetUnreliable(false)` — restore reliable network
4. Join all threads
5. Assert no failures occurred (`retvals.size() == 0`)
6. Advance `index_` by `50 * 5`
7. `DoAgreeAndAssertWaitSuccess(1060, NSERVERS)` — final agreement
   with all servers

**Expected outcome**: All 50 iterations complete despite network
unreliability.  Total ~250 concurrent agreements succeed.

**What bugs it catches**:
- Liveness failures under packet loss
- Election instability with network delays
- Deadlocks or race conditions under concurrent load

## 12. Test 11: Figure 8 (`testFigure8`)

**Source**: `test.cc:633-731`

**What it tests**: The leader completeness property from Raft Figure 8
(Ongaro & Ousterhout, 2014).  A leader must not commit entries from a
previous term by counting replicas alone — it must first commit an entry
from its own term.

**Scenario** (retried up to 10 times):

```
Phase 1: Initial state
  S1=leader, S2=follower, S3=S4=S5=followers
  Commit entry 1100 at all 5 servers

Phase 2: Partial replication
  Disconnect S3, S4, S5
  Start(S1, 1101) → replicated to S1 and S2 only
  Sleep ELECTIONTIMEOUT
  Assert 1101 NOT committed (no quorum)

Phase 3: New leader with different entry
  Disconnect S2, disconnect S1
  Reconnect S3, S4, S5
  S3 elected as leader2
  Start(leader2, 1102) at same index as 1101
  Assert 1102 NOT committed (leader2 isolated)

Phase 4: Old leader or its follower becomes leader
  Disconnect leader2
  Reconnect S1 and S2 (who have 1101 at the index)
  Reconnect one of S3/S4/S5
  leader3 elected (must be S1 or S2 for the test to proceed)

Phase 5: Verification
  Sleep ELECTIONTIMEOUT
  Assert index is STILL not committed (leader3 cannot commit
    1101 from an older term by counting replicas)
  DoAgreement(1103) — commit a new entry in current term
  Now 1101 should be committed (indirectly, by the new entry
    advancing the commit index past it)
  Assert 1101 is committed at the correct index
  Verify ServerCommitted(leader3, index, 1101)
```

**Expected outcome**: Entry 1101 from a previous term is NOT committed
until the new leader commits its own entry (1103) in the current term.
This prevents the unsafe scenario where a leader counts replicas of old
entries and commits them, only to have them overwritten by a different
leader.

**What bugs it catches**:
- Leader committing previous-term entries by replica count alone
  (violates Leader Completeness Property)
- Incorrect commitIndex advancement logic
- Log entries from different terms at the same index not handled
  correctly

**Why it retries**: The test requires `leader3` to be either S1 or S2
(who hold entry 1101).  With 3 eligible servers and random election,
this has a 2/3 success probability per attempt.  Up to 10 retries
provide > 99.99% overall success probability.

## 13. Test Progression Summary

The tests form a logical progression of increasing complexity:

| Phase | Tests | Properties Verified |
|-------|-------|--------------------|
| Election | 1-2 | Leader election, re-election, quorum requirement |
| Basic agreement | 3-5 | Log replication, quorum semantics, no false commits |
| Fault tolerance | 6, 8 | Leader rejoin, log conflict resolution, backfill speed |
| Concurrency | 7 | Thread safety of `Start()` |
| Efficiency | 9 | RPC count bounds |
| Stress | 10 | Unreliable network with concurrent load |
| Safety | 11 | Leader completeness (Raft's core safety property) |

## 14. Index Tracking Across Tests

The `index_` field starts at 1 and advances as entries are committed.
Each test assumes the previous test left `index_` at the correct value:

| Test | Starting `index_` | Entries Committed | Ending `index_` |
|------|------------------|-------------------|-----------------|
| 1 | 1 | 0 | 1 |
| 2 | 1 | 0 | 1 |
| 3 | 1 | 3 | 4 |
| 4 | 4 | 6 | 10 |
| 5 | 10 | 2 | 12 |
| 6 | 12 | 9 | 21 |
| 7 | 21 | 6 | 27 |
| 8 | 27 | 52 | 79 |
| 9 | 79 | ~11 | ~90 |
| 10 | ~90 | ~251 | ~341 |
| 11 | ~341 | varies | varies |

Note: Tests 9-11 use approximate values because they depend on retry
paths and concurrent submissions.
