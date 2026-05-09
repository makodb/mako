# Preferred Leader Election — Design and Motivation

## What This Document Covers

This document explains the design rationale, high-level mechanism, and safety argument for the preferred leader extension to Raft. This is a novel contribution beyond the standard Raft protocol: it adds deterministic leader placement while preserving all of Raft's safety properties.

**Key source files**:
- `src/deptran/raft/server.h` — Preferred leader member variables and inline methods (lines 113-146, 575-636)
- `src/deptran/raft/server.cc` — `GetElectionTimeout()` (lines 348-369), `setIsLeader()` (lines 443-554), `OnTimeoutNow()` (lines 1476-1575), `StopLeadershipTransferMonitoring()` (lines 1577-1586), `StartLeadershipTransferMonitoring()` (lines 1588-1671), `ShouldTransferLeadership()` (lines 1681-1725), `InitiateLeadershipTransfer()` (lines 1727-1827)
- `src/deptran/raft/commo.cc` — `SendTimeoutNow()` (lines 228-285), `SendAppendEntries()` with `trigger_election_now` (lines 96-175)
- `src/deptran/raft_main_helper.cc` — `SetPreferredLeader()` call site (lines 300-349)

---

## 1. Why Preferred Leader?

Standard Raft makes no guarantee about which replica becomes leader. After a leader failure, any follower can win the election, depending on random timeouts and network conditions. This is sufficient for correctness, but suboptimal for systems like Mako that have operational preferences.

### Problem: Non-Deterministic Leader Placement

In a geo-replicated system:
1. **Data locality**: The leader handles all client reads/writes. If the leader is far from the data or the clients, every operation incurs cross-datacenter latency.
2. **Cross-shard coordination**: Mako runs distributed transactions across shards. If each shard's leader is in a different datacenter, every cross-shard operation requires cross-datacenter communication.
3. **Operational control**: Operators want to place leaders on specific machines for capacity planning, maintenance windows, and resource isolation.

### Solution: Preferred Leader

The preferred leader mechanism ensures that a designated replica becomes the leader whenever possible, while still allowing any replica to serve as leader during failures (preserving fault tolerance).

**Key design goals**:
- **Deterministic placement**: A configured preferred replica will become leader within seconds of startup or recovery
- **Automatic failover**: If the preferred replica fails, standard Raft election selects an alternative leader
- **Automatic failback**: When the preferred replica recovers, leadership automatically transfers back to it
- **Safety preservation**: All Raft safety properties are maintained — no data loss, no split-brain

---

## 2. How It Differs from Standard Raft

| Aspect | Standard Raft | Preferred Leader Extension |
|--------|--------------|---------------------------|
| Election timeout | Same for all replicas (150-300ms) | Asymmetric: 150-300ms for preferred, 500ms-2s for others |
| Leader selection | First to timeout wins | Preferred wins startup races; non-preferred leaders transfer back |
| Leader failover | Random follower wins | Any follower can still win (safety preserved) |
| Leader failback | Not supported | Automatic via monitoring thread and transfer protocol |
| Post-election behavior | Leader stays indefinitely | Non-preferred leaders actively monitor and transfer |
| Transfer mechanism | Not in standard Raft | Piggybacked `trigger_election_now` in AppendEntries |

### What Is NOT Changed

The preferred leader extension does not modify any of these standard Raft mechanisms:
- Log replication protocol
- Vote granting rules (term check, log up-to-date check)
- Commit rule (majority replication in current term)
- Leader completeness property
- AppendEntries consistency check

---

## 3. Design Overview

The preferred leader system operates in three phases:

### Phase 1: Startup — Election Bias

At startup, all replicas begin as followers. The preferred replica is configured via `SetPreferredLeader(site_id)`, which is called during initialization (in `raft_main_helper.cc`). The system biases elections toward the preferred replica using asymmetric timeouts:

```
GetElectionTimeout():
  |
  +-- AmIPreferredLeader()?
  |     YES → 150-300ms (short timeout, wins election races)
  |     NO  → Is startup grace period (<5 seconds)?
  |             YES → 1-2 seconds (very long, lets preferred win)
  |             NO  → 500ms-1 second (medium, enables failover)
```

During the 5-second startup grace period, non-preferred replicas wait 1-2 seconds before starting elections. The preferred replica waits only 150-300ms. This creates a high probability that the preferred replica wins the first election.

**Why a grace period?** At startup, all replicas boot simultaneously. Without the grace period, a non-preferred replica with a slightly shorter random timeout could win before the preferred replica even starts. The 5-second grace period ensures the preferred replica has time to initialize and win the first election.

**After the grace period**, non-preferred replicas use 500ms-1 second timeouts. This is longer than the preferred replica's 150-300ms, giving the preferred replica an advantage in subsequent elections, but short enough to detect leader failures within a reasonable time.

### Phase 2: Monitoring — Non-Preferred Leader Detection

When a non-preferred replica wins an election (e.g., because the preferred replica was down during failover), it becomes a "non-preferred leader." This triggers the monitoring system:

```
setIsLeader(true):
  |
  +-- become_new_leader?
        |
        +-- AmIPreferredLeader()?
              NO → StartLeadershipTransferMonitoring()
```

The monitoring thread runs in the background (a `std::thread`, not a fiber) and checks every 1 second whether the preferred replica is alive and caught up:

```
StartLeadershipTransferMonitoring():
  |
  +-- Wait MIN_STABLE_TIME_US (500ms) after becoming leader
  +-- Loop every 1 second:
  |     +-- Still leader? Still non-preferred?
  |     +-- ShouldTransferLeadership()?
  |     |     +-- Am I leader? (must be)
  |     |     +-- Am I non-preferred? (must be)
  |     |     +-- Is preferred configured? (must be)
  |     |     +-- Not already transferring? (must not be)
  |     |     +-- Is preferred in peer list? (must be)
  |     |     +-- Is preferred caught up? (match_index >= commitIndex)
  |     |           YES → Return true
  |     +-- If should_transfer:
  |           InitiateLeadershipTransfer()
  |           break
```

The `ShouldTransferLeadership()` check verifies:
1. This replica is the current leader
2. This replica is NOT the preferred leader
3. A preferred leader is configured (`!= INVALID_SITEID`)
4. No transfer is already in progress
5. The preferred replica is known (exists in `match_index_`)
6. The preferred replica is caught up (`match_index_[preferred] >= commitIndex`)

The "caught up" check is the critical safety condition — transferring to a replica that hasn't received all committed entries could lose data.

### Phase 3: Transfer — Piggybacked Protocol

When conditions are met, `InitiateLeadershipTransfer()` executes the actual leadership transfer:

```
InitiateLeadershipTransfer():
  |
  +-- [Under lock] Set transferring_leadership_ = true
  +-- [Under lock] Send heartbeat to ALL replicas with trigger_election_now=true
  +-- [Release lock] Sleep 20ms (ensure packets sent)
  +-- [Under lock] setIsLeader(false) — step down
```

The transfer uses a **piggybacked approach**: instead of a separate `TimeoutNow` RPC, the transfer signal is embedded in the regular heartbeat `EmptyAppendEntries` RPC via the `trigger_election_now` flag. This has two advantages:
1. **Atomic notification**: All replicas learn about the transfer simultaneously
2. **Timer reset**: The heartbeat resets non-preferred replicas' election timers, preventing them from starting elections

**When replicas receive `trigger_election_now=true`**:

| Recipient | Action |
|-----------|--------|
| Preferred replica | Waits 30ms, then calls `RequestVote()` to start election |
| Non-preferred replica | Logs the event, does nothing (its election timer was just reset) |

The 30ms delay before the preferred replica starts election gives the old leader time to step down and for other replicas to process the heartbeat (resetting their timers). This prevents election storms.

---

## 4. Transfer Protocol — Detailed Sequence

```
Non-Preferred Leader              Preferred Follower           Other Follower
       |                                |                          |
  [ShouldTransfer? YES]                 |                          |
       |                                |                          |
  [Set transferring_=true]              |                          |
       |                                |                          |
  [EmptyAppendEntries                   |                          |
   trigger_election_now=true] --------->|                          |
       |                         [Reset timer]                     |
  [EmptyAppendEntries                   |                          |
   trigger_election_now=true] ------------------------------------>|
       |                                |                   [Reset timer]
       |                                |                   [Log, do nothing]
  [Sleep 20ms]                          |                          |
       |                         [Wait 30ms]                       |
  [setIsLeader(false)]                  |                          |
  [Now follower]                        |                          |
       |                         [RequestVote()]                   |
       |                                |                          |
       |                         [Increment term]                  |
       |                                |                          |
       |<--- RequestVote(new_term) -----|                          |
       |                                |---- RequestVote -------->|
       |                                |                          |
  [Grant vote]                          |                   [Grant vote]
       |--- VoteReply(yes) ------------>|                          |
       |                                |<----- VoteReply(yes) ----|
       |                                |                          |
       |                         [Won election!]                   |
       |                         [setIsLeader(true)]               |
       |                                |                          |
       |<---- AppendEntries ------------|--- AppendEntries ------->|
       |     (new leader heartbeat)     |  (new leader heartbeat)  |
```

### Timing Analysis

| Event | Time |
|-------|------|
| T=0 | Non-preferred leader sends trigger_election_now heartbeat |
| T=0-5ms | Heartbeat arrives at all replicas (LAN) |
| T=20ms | Old leader steps down (`setIsLeader(false)`) |
| T=30ms | Preferred replica starts election (`RequestVote()`) |
| T=30-35ms | Vote requests arrive at all replicas |
| T=35-40ms | Vote replies arrive at preferred replica |
| T=40ms | Preferred replica becomes leader |

Total transfer time: ~40ms on LAN.

---

## 5. The `TimeoutNow` RPC — Direct Transfer

In addition to the piggybacked approach, the system also implements a standalone `TimeoutNow` RPC (`SendTimeoutNow()` in `commo.cc:228-285`). This RPC directly instructs a replica to start an election immediately.

**`OnTimeoutNow()` handler edge cases** (`server.cc:1476-1575`):

| Edge Case | Response |
|-----------|----------|
| Server shutting down (`stop_`) | Ignore, reply default |
| Stale TimeoutNow (`leaderTerm < currentTerm`) | Ignore |
| Leader ahead (`leaderTerm > currentTerm`) | Update term, step down if leader, then start election |
| Already leader | Reply `success=true` (goal achieved) |
| Already candidate (`req_voting_`) | Reply `success=true` (already trying) |
| Currently transferring (`transferring_leadership_`) | Ignore |
| Valid request | Call `RequestVote()`, reply with result |

The `TimeoutNow` RPC is available for external use (e.g., operational tools) but the primary transfer mechanism uses the piggybacked approach for its atomicity guarantees.

---

## 6. Dynamic Election Timeout — `GetElectionTimeout()`

**Location**: `server.cc:348-369`

The election timeout is the core mechanism that biases elections toward the preferred replica:

```
State                              Timeout Range        Purpose
─────────────────────────────────────────────────────────────────
Preferred replica (always)         150-300ms            Win elections quickly
Non-preferred, grace period        1-2 seconds          Let preferred win startup
Non-preferred, after grace         500ms-1 second       Enable failover detection
```

The grace period is defined as the first 5 seconds after startup (`startup_timestamp_`). After this period, non-preferred replicas reduce their timeout to 500ms-1s, which is still long enough to give the preferred replica an advantage but short enough to detect leader failures.

**Comparison to standard Raft**:
- Standard Raft: All replicas use 150-300ms (from the paper)
- This implementation: Preferred uses 150-300ms, others use 500ms-2s
- The asymmetry creates a probabilistic guarantee that the preferred replica wins elections
- But any replica CAN still win if the preferred is down (safety preserved)

---

## 7. Configuration — `SetPreferredLeader()`

The preferred leader is configured at runtime via `SetPreferredLeader(site_id)` (`server.h:587-604`).

### Startup Configuration (`raft_main_helper.cc:300-349`)

During initialization, the system automatically selects the preferred leader:

```
For each Raft partition:
  1. Get all sites in this partition
  2. Find the site with locale_id == 0 (localhost)
  3. Call raft_server->SetPreferredLeader(preferred_site_id)
```

In the current configuration, **localhost (locale_id=0) is always the preferred leader** for every partition. In a multi-partition setup (e.g., 6 partitions with 3 replicas each = 18 sites), each partition independently selects its localhost replica as preferred.

### Dynamic Reconfiguration

`SetPreferredLeader()` can be called at any time to change the preferred leader:

```cpp
void SetPreferredLeader(siteid_t site_id) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    preferred_leader_site_id_ = site_id;
    // If I'm a non-preferred leader, start monitoring for transfer
    if (!AmIPreferredLeader() && is_leader_ && looping_) {
        StartLeadershipTransferMonitoring();
    }
}
```

If the current leader is now non-preferred (because the preferred was changed to a different site), monitoring starts immediately.

---

## 8. Safety Argument

The preferred leader extension preserves all five Raft safety properties from the original paper:

### 8.1 Election Safety

**Property**: At most one leader can be elected in a given term.

**Preserved because**: The preferred leader mechanism only affects election *timing* (via asymmetric timeouts), not election *rules*. A candidate still needs a majority of votes to win. The `trigger_election_now` flag causes the preferred replica to start an election, but it must still collect votes using the standard RequestVote protocol. No replica grants its vote to two candidates in the same term.

### 8.2 Leader Append-Only

**Property**: A leader never overwrites or deletes entries in its log; it only appends new entries.

**Preserved because**: The transfer mechanism doesn't modify any logs. The old leader simply steps down (`setIsLeader(false)`). The new leader appends entries normally.

### 8.3 Log Matching

**Property**: If two logs contain an entry with the same index and term, then the logs are identical in all entries through that index.

**Preserved because**: The transfer mechanism doesn't modify logs. The `trigger_election_now` flag is part of `EmptyAppendEntries` (heartbeat), which doesn't carry log entries. Normal AppendEntries consistency checks still apply.

### 8.4 Leader Completeness

**Property**: If a log entry is committed in a given term, that entry will be present in the logs of the leaders of all higher-numbered terms.

**Preserved because**: `ShouldTransferLeadership()` only returns `true` when the preferred replica's `match_index >= commitIndex`. This means the preferred replica has ALL committed entries before the transfer begins. Combined with standard Raft vote restriction (candidates must have an up-to-date log), the preferred replica will win elections and maintain all committed entries.

### 8.5 State Machine Safety

**Property**: If a server has applied a log entry at a given index to its state machine, no other server will ever apply a different log entry for that index.

**Preserved because**: This follows from Leader Completeness. Since the preferred replica has all committed entries before becoming leader, it will never commit a different entry at any index.

### Transfer-Specific Safety

Beyond the five standard properties, the transfer protocol has additional safety guarantees:

1. **No dual leadership**: The old leader steps down (`setIsLeader(false)`) after sending the transfer signal. Even if the transfer fails (preferred doesn't win), the old leader is now a follower and won't accept writes.

2. **No lost writes**: `ShouldTransferLeadership()` checks `match_index_[preferred] >= commitIndex`, ensuring all committed entries are replicated to the preferred replica before transfer.

3. **No election storms**: The piggybacked approach resets all non-preferred replicas' election timers simultaneously. Combined with the 30ms delay before the preferred starts its election, this prevents multiple replicas from competing.

4. **Bounded transfer time**: If the preferred replica doesn't win the election within its timeout, normal Raft election proceeds. The system doesn't block indefinitely.

---

## 9. Member Variables

**Location**: `server.h:113-132`

| Variable | Type | Default | Purpose |
|----------|------|---------|---------|
| `preferred_leader_site_id_` | `siteid_t` | `INVALID_SITEID` (i.e., `(siteid_t)-1`) | Site ID of the preferred leader |
| `leader_last_commit_index_` | `uint64_t` | `0` | Leader's commit index (from heartbeats, used by `HaveCaughtUp()`) |
| `transferring_leadership_` | `bool` | `false` | True during an active transfer |
| `leadership_transfer_start_time_` | `uint64_t` | `0` | Timestamp when transfer started |
| `leadership_monitor_stop_` | `std::atomic<bool>` | `false` | Signal to stop the monitoring thread |
| `leadership_monitor_thread_` | `std::thread` | (default) | Background monitoring thread |
| `startup_timestamp_` | `uint64_t` | `0` | Server startup time (for grace period) |

### Inline Helper Methods

```cpp
// Check if this replica is the configured preferred leader
bool AmIPreferredLeader() const {
    return preferred_leader_site_id_ != INVALID_SITEID &&
           site_id_ == preferred_leader_site_id_;
}

// Check if this replica has caught up to the leader's commit level
// (Used by preferred follower to know when it can safely become leader)
bool HaveCaughtUp() const {
    return commitIndex >= leader_last_commit_index_;
}
```

---

## 10. Failure Modes and Recovery

### Preferred Replica Fails

1. Non-preferred replica's election timeout fires (500ms-1s)
2. Normal Raft election proceeds — any follower can win
3. New leader serves requests normally
4. When preferred recovers, it receives heartbeats and starts replicating
5. Once caught up (`match_index >= commitIndex`), non-preferred leader transfers back

### Non-Preferred Leader Fails During Transfer

1. Transfer signal sent but old leader crashes before stepping down
2. Preferred replica may have started an election — if it wins, it becomes leader normally
3. If preferred doesn't win, another replica's election timeout fires and normal election proceeds
4. No data loss: committed entries are on a majority by definition

### Preferred Replica Slow/Partitioned

1. `ShouldTransferLeadership()` checks `match_index >= commitIndex`
2. If preferred is slow, its match index lags behind commit index
3. Transfer doesn't happen until preferred catches up
4. System continues operating with non-preferred leader indefinitely

### Configuration Change

1. `SetPreferredLeader(new_site_id)` called at runtime
2. If current leader is now non-preferred, monitoring starts immediately
3. Standard transfer protocol takes over
4. Can disable preferred leader entirely with `SetPreferredLeader(INVALID_SITEID)`

---

## Related Documents

- [Implementation Details](implementation.md) — Method-level walkthrough of all preferred leader functions
- [Testing](testing.md) — Test binaries and correctness verification
- [Leader Election](../02-raft-core/leader_election.md) — Standard Raft election mechanism
- [Log Replication](../02-raft-core/log_replication.md) — How match_index_ tracks follower progress
- [RPC Layer](../02-raft-core/rpc_layer.md) — SendTimeoutNow and SendAppendEntries with trigger_election_now
