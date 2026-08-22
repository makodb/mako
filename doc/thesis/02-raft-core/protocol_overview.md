# Raft Protocol Implementation Overview

## What This Document Covers

This document provides a high-level overview of the Raft consensus protocol as implemented in the Mako codebase. It maps the implementation to the original Raft paper (Ongaro & Ousterhout, 2014), documents deviations and extensions, and serves as an entry point into the more detailed documents in this chapter.

**Note**: The Raft implementation is a core thesis contribution. This overview covers the architecture; subsequent documents cover each component in depth.

---

## 1. Raft Fundamentals Recap

Raft is a consensus protocol that ensures a replicated log is consistent across a cluster of servers. It decomposes consensus into three sub-problems:

1. **Leader election** -- At most one leader per term; other servers are followers.
2. **Log replication** -- The leader receives commands, appends them to its log, and replicates to followers.
3. **Safety** -- If a log entry is committed, no future leader will have a different entry at that index.

Each server is in one of three states at any time:

```
                  timeout, start election
            +-----------------------------------+
            |                                   |
            v          receives majority        |
        +-----------+    votes won    +--------+------+
 start  |           |--------------->|                |
------->| Follower  |               |   Candidate    |
        |           |<--------------|                |
        +-----------+  discovers    +--------+------+
            ^          current leader        |
            |          or new term           |
            |                               |
            |     discovers server with     |
            |        higher term            |
            |                               v
            |                       +-----------+
            +-----------------------|           |
                                   |  Leader   |
                                   |           |
                                   +-----------+
```

**Reference**: Ongaro, D. and Ousterhout, J. (2014). "In Search of an Understandable Consensus Algorithm." USENIX ATC '14.

---

## 2. Implementation-to-Paper Mapping

The implementation maps Raft concepts to classes as follows:

| Raft Paper Concept | Implementation Class | Source File |
|--------------------|---------------------|-------------|
| State machine / Raft server | `RaftServer` | `src/deptran/raft/server.h`, `server.cc` |
| RPC transport layer | `RaftCommo` | `src/deptran/raft/commo.h`, `commo.cc` |
| RPC request handlers | `RaftServiceImpl` | `src/deptran/raft/service.h`, `service.cc` |
| Protocol factory | `RaftFrame` | `src/deptran/raft/frame.h`, `frame.cc` |
| Client interface / command submission | `CoordinatorRaft` | `src/deptran/raft/coordinator.h`, `coordinator.cc` |
| Log entry | `RaftData` | `src/deptran/raft/server.h` |
| RPC definitions | `RaftService` proxy/service | `src/deptran/rcc_rpc.h` |

### Class Hierarchy

```
Frame (protocol factory base)
  |
  +-- RaftFrame
        |
        +-- creates RaftServer (extends TxLogServer)
        +-- creates RaftCommo (extends Communicator)
        +-- creates CoordinatorRaft (extends CoordinatorBase)
        +-- creates RaftServiceImpl (extends RaftService)
```

`RaftServer` inherits from `TxLogServer`, which provides the `app_next_` callback for applying committed log entries to the upper-level transaction system. This is the integration point between Raft consensus and Mako's transaction processing pipeline.

---

## 3. Core State Variables

The Raft paper defines persistent and volatile state. Here is how the implementation maps them:

### Persistent State (on all servers)

| Paper Variable | Implementation | Location |
|----------------|---------------|----------|
| `currentTerm` | `uint64_t currentTerm` | `server.h:274` |
| `votedFor` | `siteid_t vote_for_` | `server.h:89` |
| `log[]` | `map<slotid_t, shared_ptr<RaftData>> raft_logs_` | `server.h:277` |

Persistence is handled by optional `LogStorage` backend (`server.h:52`). When configured, `PersistTermAndVote()` and `PersistLogEntry()` write to stable storage.

### Volatile State (on all servers)

| Paper Variable | Implementation | Location |
|----------------|---------------|----------|
| `commitIndex` | `uint64_t commitIndex` | `server.h:275` |
| `lastApplied` | `uint64_t executeIndex` | `server.h:276` |

### Volatile State (on leaders)

| Paper Variable | Implementation | Location |
|----------------|---------------|----------|
| `nextIndex[]` | `std::map<siteid_t, uint64_t> next_index_` | `server.h:82` |
| `matchIndex[]` | `std::map<siteid_t, uint64_t> match_index_` | `server.h:81` |

### Additional State (implementation-specific)

| Variable | Purpose | Location |
|----------|---------|----------|
| `is_leader_` | Current role flag | `server.h:91` |
| `lastLogIndex` | Cached last log index | `server.h:273` |
| `preferred_leader_site_id_` | Preferred leader for geo-distribution | `server.h:126` |
| `transferring_leadership_` | Leadership transfer in progress | `server.h:128` |

---

## 4. RPC Interface

The implementation defines four RPCs (in `src/deptran/rcc_rpc.h`):

### RequestVote RPC

```
Request:  (lst_log_idx: u64, lst_log_term: ballot, can_id: siteid, can_term: ballot)
Response: (reply_term: ballot, vote_granted: bool)
```

- Sent by candidates to request votes during elections.
- Handler: `RaftServiceImpl::HandleVote()` -> `RaftServer::OnRequestVote()`

### AppendEntries RPC

```
Request:  (slot: u64, ballot: ballot, leaderCurrentTerm: u64, leaderSiteId: siteid,
           leaderPrevLogIndex: u64, leaderPrevLogTerm: u64, leaderCommitIndex: u64,
           cmd: MarshallDeputy, leaderNextLogTerm: u64)
Response: (followerAppendOK: u64, followerCurrentTerm: u64, followerLastLogIndex: u64)
```

- Sent by leader to replicate log entries and as heartbeats.
- Handler: `RaftServiceImpl::HandleAppendEntries()` -> `RaftServer::OnAppendEntries()`

### EmptyAppendEntries RPC (Heartbeat)

```
Request:  (slot: u64, ballot: ballot, leaderCurrentTerm: u64, leaderSiteId: siteid,
           leaderPrevLogIndex: u64, leaderPrevLogTerm: u64, leaderCommitIndex: u64,
           trigger_election_now: bool)
Response: (followerAppendOK: u64, followerCurrentTerm: u64, followerLastLogIndex: u64)
```

- Heartbeat variant with no log payload. The `trigger_election_now` flag supports the leadership transfer protocol.
- Handler: `RaftServiceImpl::HandleEmptyAppendEntries()` -> `RaftServer::OnAppendEntries()` with `cmd=nullptr`

### TimeoutNow RPC

```
Request:  (leaderTerm: u64, leaderSiteId: siteid)
Response: (followerTerm: u64, success: bool)
```

- Sent by a stepping-down leader to trigger immediate election on the preferred replica.
- Handler: `RaftServiceImpl::HandleTimeoutNow()` -> `RaftServer::OnTimeoutNow()`
- **Not in the original Raft paper** -- part of the preferred leader extension.

---

## 5. Algorithm Summary

### 5.1 Leader Election

The election algorithm follows the Raft paper with dynamic timeout tuning:

1. **Timer expiry** -- Follower's election timer fires (`StartElectionTimer()`, `server.cc:1188`)
2. **Become candidate** -- Increment `currentTerm`, vote for self, persist (`RequestVote()`, `server.cc:1016`)
3. **Broadcast votes** -- Send RequestVote RPC to all peers (`RaftCommo::BroadcastVote()`)
4. **Collect votes** -- `RaftVoteQuorumEvent` tallies responses; majority wins
5. **Become leader** -- Initialize `next_index_[]` and `match_index_[]`, start heartbeats (`setIsLeader(true)`, `server.cc:444`)

**Election timeout ranges**:

| Role | Timeout Range | Purpose |
|------|---------------|---------|
| Preferred leader | 150-300 ms | Win elections quickly |
| Non-preferred (grace period, first 5s) | 1-2 s | Let preferred leader win first |
| Non-preferred (normal) | 500 ms - 1 s | Standard Raft randomized timeout |

See [Leader Election](leader_election.md) for the full walkthrough.

### 5.2 Log Replication

The leader replicates entries through a heartbeat loop (`HeartbeatLoop()`, `server.cc:636`):

1. **Client submission** -- `CoordinatorRaft::Submit()` calls `RaftServer::Start()` to append locally
2. **Broadcast** -- Heartbeat loop sends `AppendEntries` RPCs with batched entries
3. **Follower acceptance** -- `OnAppendEntries()` performs consistency check, appends, updates `commitIndex`
4. **Commit advancement** -- Leader computes majority-replicated index from `match_index_[]`
5. **Application** -- `applyLogs()` calls `app_next_()` callback for each committed entry

**Optimizations over standard Raft**:
- **Batch replication**: Multiple entries sent in a single `AppendEntries` RPC via `TpcBatchCommand`
- **Fast log reconciliation**: On rejection, follower reports its `lastLogIndex`, allowing the leader to jump `next_index_` directly instead of decrementing one-by-one
- **Exponential backoff**: When no follower hint is available, `next_index_` is halved rather than decremented

See [Log Replication](log_replication.md) for the full walkthrough.

### 5.3 Safety

The implementation preserves the Raft safety properties:

- **Election Safety**: At most one leader per term -- enforced by `vote_for_` persistence and single-vote-per-term check in `OnRequestVote()` (`server.cc:1141-1147`)
- **Leader Append-Only**: Leader never overwrites or deletes its own entries -- `Start()` only appends (`server.cc:1230`)
- **Log Matching**: If two logs contain an entry with the same index and term, all preceding entries are identical -- enforced by `prevLogTerm` check in `OnAppendEntries()` (`server.cc:1286`)
- **Leader Completeness**: If an entry is committed in a term, it will be present in all future leaders' logs -- enforced by the log up-to-date check in vote granting (`server.cc:1158-1181`)
- **State Machine Safety**: Applied entries are never un-applied -- `executeIndex` only advances forward in `applyLogs()` (`server.cc:581`)

---

## 6. Key Deviations from the Raft Paper

### 6.1 Preferred Leader Election (Novel Contribution)

Standard Raft elects whichever candidate wins first. This implementation adds a **preferred leader** mechanism for geo-distributed deployments where a specific replica should be leader for locality:

- **Dynamic election timeouts**: Preferred replica uses 150-300ms; others use 500ms-2s. This gives the preferred replica a statistical advantage in winning elections.
- **Leadership transfer protocol**: When a non-preferred replica becomes leader (e.g., after a failure), a background monitor thread (`StartLeadershipTransferMonitoring()`) watches for the preferred replica to catch up. Once caught up, the current leader steps down and triggers an immediate election on the preferred replica via `TimeoutNow` RPC.
- **Piggybacked transfer signal**: The `trigger_election_now` flag in `EmptyAppendEntries` allows the stepping-down leader to signal all replicas simultaneously.

See [Preferred Leader Election](../03-preferred-leader/mechanism.md) for the full design.

### 6.2 Integration with Two-Phase Commit

Unlike standalone Raft implementations, this one integrates with Mako's distributed transaction system:

- **Command type**: Log entries are `TpcCommitCommand` objects (transaction commit records), not arbitrary byte strings
- **Application callback**: `applyLogs()` invokes `app_next_(slot_id, cmd)`, which feeds committed commands back into Mako's transaction processing pipeline
- **WRONG_LEADER handling**: When `CoordinatorRaft::Submit()` detects it's not on the leader, it returns `WRONG_LEADER` with view data so the client can redirect
- **View tracking**: `RaftFrame` maintains view data that maps partition IDs to current leader information

### 6.3 Batched Log Replication

The paper describes sending entries one at a time. This implementation batches multiple entries per RPC:

```
Leader log:  [..., entry_5, entry_6, entry_7, entry_8]
                                                  ^
                                            lastLogIndex

Follower (next_index=6):
  Receives: [entry_6, entry_7, entry_8] in one AppendEntries RPC
```

This is controlled by `RAFT_BATCH_OPTIMIZATION` and uses `TpcBatchCommand` to bundle entries.

### 6.4 Optimized Log Reconciliation

Standard Raft decrements `next_index` by 1 on each rejected `AppendEntries`. This implementation uses three strategies:

1. **Fast backoff** -- If follower reports `lastLogIndex < next_index - 1`, jump directly to `lastLogIndex + 1`
2. **Exponential backoff** -- If no hint available and `next_index > 10`, halve it
3. **Linear backoff** -- If `next_index <= 10`, decrement by 1

This reduces O(n) round trips to O(log n) in the worst case for log reconciliation.

### 6.5 Persistent State Management

The implementation optionally persists Raft state to RocksDB:

- `PersistTermAndVote()` -- Called after every term change or vote grant
- `PersistLogEntry()` -- Called in `SetLocalAppend()` when leader appends new entries
- `PersistCommitIndex()` -- Called when `commitIndex` advances
- `RecoverFromStorage()` -- Restores state on restart
- `ReplayCommittedEntries()` -- Replays committed entries through `app_next_` after recovery

### 6.6 Jetpack Recovery

On leader election, the new leader reaches the generic `JetpackRecoveryEntry()`
path unless it is disabled by `MAKO_DISABLE_JETPACK`. The Rule protocol that
supplied witness data is retired, so this remaining recovery machinery is
legacy, unsupported, and under a separate audit.

---

## 7. Component Interaction Diagram

The following shows how the major components interact during a typical command submission:

```
Client (Mako Transaction Layer)
    |
    | Submit(cmd)
    v
CoordinatorRaft
    |
    | Start(cmd)          Creates log entry
    v
RaftServer (Leader)
    |
    | SetLocalAppend()    Appends to local log, persists
    | Signal ready_for_replication_
    v
HeartbeatLoop             Runs continuously on leader
    |
    | For each follower:
    | SendAppendEntries2()
    v
RaftCommo ----RPC----> RaftServiceImpl (Follower)
                            |
                            | HandleAppendEntries()
                            v
                       RaftServer (Follower)
                            |
                            | OnAppendEntries()
                            |   - Term check
                            |   - Log consistency check
                            |   - Append entries
                            |   - Update commitIndex
                            |   - applyLogs()
                            v
                       app_next_(slot, cmd)
                            |
                            v
                       Mako Transaction Layer

    Meanwhile, back on the leader:
    HeartbeatLoop
        |
        | Update match_index_ from responses
        | Compute new commitIndex (majority)
        | applyLogs()
        v
    app_next_(slot, cmd)
        |
        v
    Mako Transaction Layer
```

---

## 8. File Map

| File | Lines | Purpose |
|------|-------|---------|
| `src/deptran/raft/server.h` | ~280 | RaftServer class definition, member variables |
| `src/deptran/raft/server.cc` | ~1830 | Full Raft algorithm implementation |
| `src/deptran/raft/commo.h` | ~70 | RaftCommo class definition |
| `src/deptran/raft/commo.cc` | ~290 | RPC sending: AppendEntries, Vote, TimeoutNow |
| `src/deptran/raft/service.h` | ~50 | RaftServiceImpl class definition |
| `src/deptran/raft/service.cc` | ~115 | RPC handlers: HandleVote, HandleAppendEntries |
| `src/deptran/raft/frame.h` | ~60 | RaftFrame class definition |
| `src/deptran/raft/frame.cc` | ~210 | Factory methods, RPC service registration |
| `src/deptran/raft/coordinator.h` | ~50 | CoordinatorRaft class definition |
| `src/deptran/raft/coordinator.cc` | ~200 | Command submission, WRONG_LEADER handling |
| `src/deptran/rcc_rpc.h` | (relevant: 1491-1700) | RPC ID definitions, wire format |

---

## Related Documents

- [Server Implementation](server_implementation.md) -- Deep dive into `RaftServer` internals
- [Leader Election](leader_election.md) -- Full election cycle walkthrough
- [Log Replication](log_replication.md) -- Replication mechanism and optimizations
- [Coordinator](coordinator.md) -- Transaction submission via `CoordinatorRaft`
- [RPC Layer](rpc_layer.md) -- Communication infrastructure
- [Preferred Leader](../03-preferred-leader/mechanism.md) -- Leadership transfer protocol
- [System Architecture](../01-mako-overview/system_architecture.md) -- Mako system overview
