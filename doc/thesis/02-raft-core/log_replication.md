# Log Replication Mechanism

## What This Document Covers

This document provides a complete walkthrough of Raft log replication as implemented: how the leader sends entries to followers, how followers validate and append entries, how the leader tracks follower progress, how commit advancement works, how log conflicts are resolved, and how batching optimizes throughput.

**Key source files**:
- `src/deptran/raft/server.cc` — `HeartbeatLoop()`, `OnAppendEntries()`, `Start()`, `applyLogs()`
- `src/deptran/raft/commo.cc` — `SendAppendEntries2()`
- `src/deptran/tpc_command.h` — `TpcBatchCommand`, `TpcCommitCommand`

---

## 1. Overview

Log replication ensures every committed entry is durably stored on a majority of servers. The flow is:

```
Client submits command
        |
        v
  CoordinatorRaft::Submit()
        |
        v
  RaftServer::Start()           [Leader appends locally]
        |
        v
  HeartbeatLoop()               [Leader replicates to followers]
        |
        +----> SendAppendEntries2() ---RPC---> OnAppendEntries() [Follower]
        |                                           |
        |      <---- response (ok, term, lastIdx) --+
        |
        v
  Update match_index_, next_index_
  Compute new commitIndex (majority)
        |
        v
  applyLogs()                   [Feed to Mako transaction layer]
```

---

## 2. Leader: Sending Entries

### 2.1 HeartbeatLoop() — The Replication Engine

**Location**: `server.cc:636-961`

The `HeartbeatLoop()` runs as a coroutine on every server but only sends entries when the server is leader. It serves dual purposes: replicating new entries and sending heartbeats.

```
HeartbeatLoop():
  Initialize match_index_[peer] = 0, next_index_[peer] = 1 for all peers
  looping_ = true

  WHILE looping_:
    [A] Wait: ready_for_replication_->wait(HEARTBEAT_INTERVAL)
    [B] If !IsLeader(): continue

    [C] FOR each peer in next_index_:
      [C1] Compute new commitIndex (see Section 5)
      [C2] Prepare and send AppendEntries (see below)
      [C3] Process response (see Section 4)
```

**Wake mechanism**: The loop sleeps on `ready_for_replication_` (an `IntEvent`). It wakes either:
- When `HEARTBEAT_INTERVAL` elapses (5ms in production, 100ms in test mode)
- When `CoordinatorRaft` signals `ready_for_replication_->set(1)` after a new entry is appended via `Start()`

### 2.2 Preparing an AppendEntries

For each follower, the leader prepares an AppendEntries based on `next_index_[peer]`:

```
prevLogIndex = next_index_[peer] - 1
prevLogTerm  = raft_logs_[prevLogIndex].term

IF next_index_[peer] <= lastLogIndex:
    // There are entries to send
    Prepare cmd (single entry or batch)
ELSE:
    // No new entries — send heartbeat (cmd = nullptr)
```

**Safety checks** (server.cc:744-775):
- If `prevLogIndex > lastLogIndex`, reset `next_index_` to `lastLogIndex + 1`
- Null check on `GetRaftInstance(prevLogIndex)` to prevent crash

### 2.3 Non-Batch Mode

When `RAFT_BATCH_OPTIMIZATION` is not defined (server.cc:786-797):

```cpp
if (it->second <= lastLogIndex) {
    auto curInstance = GetRaftInstance(it->second);
    cmd = curInstance->log_;
    cmdLogTerm = curInstance->term;
}
```

Sends one entry at a time: the entry at index `next_index_[peer]`.

### 2.4 Batch Mode (Default)

When `RAFT_BATCH_OPTIMIZATION` is defined (server.cc:800-825):

```cpp
vector<shared_ptr<TpcCommitCommand>> batch_buffer_;
for (int idx = max(it->second, min_active_slot_); idx <= lastLogIndex; idx++) {
    auto curInstance = GetRaftInstance(idx);
    shared_ptr<TpcCommitCommand> curCmd = dynamic_pointer_cast<TpcCommitCommand>(curInstance->log_);
    curCmd->term = curInstance->term;
    batch_buffer_.push_back(curCmd);
}
shared_ptr<TpcBatchCommand> batch_cmd = make_shared<TpcBatchCommand>();
batch_cmd->AddCmds(batch_buffer_);
cmd = dynamic_pointer_cast<Marshallable>(batch_cmd);
```

All entries from `next_index_[peer]` through `lastLogIndex` are bundled into a single `TpcBatchCommand`. This reduces the number of RPCs from O(n) to O(1) per follower per heartbeat cycle, critical for high-throughput workloads.

**TpcBatchCommand structure** (`tpc_command.h:57-71`):
```cpp
class TpcBatchCommand : public Marshallable {
    vector<shared_ptr<TpcCommitCommand>> cmds_;  // Batched entries
    void AddCmds(vector<shared_ptr<TpcCommitCommand>>& cmds);
    Marshal& to_marshal(Marshal&) const override;  // Serialization
    Marshal& from_marshal(Marshal&) override;
};
```

### 2.5 SendAppendEntries2() — The RPC Call

**Location**: `commo.cc:26-94`

This function sends a single AppendEntries RPC to one follower:

```
SendAppendEntries2(site_id, par_id, ..., cmd, ..., ret_status*, ret_term*, ret_last_log_index*):
  |
  +-- Create IntEvent for synchronization
  +-- Find proxy for target site_id
  |
  +-- IF cmd == nullptr:
  |     Send EmptyAppendEntries (heartbeat)
  |     (trigger_election_now = false for normal heartbeats)
  |
  +-- ELSE:
  |     Wrap cmd in MarshallDeputy
  |     Send AppendEntries with serialized command
  |
  +-- Callback:
  |     Extract (ret_status, ret_term, ret_last_log_index) from reply
  |     Signal IntEvent
  |
  +-- RETURN IntEvent (caller waits on it)
```

The caller waits with a 500ms timeout (server.cc:847):
```cpp
r->wait(500000);
if (r->status_.get() == Event::TIMEOUT) {
    continue;  // Skip this follower, try again next heartbeat
}
```

This bounded wait prevents a slow or partitioned follower from stalling the leader's replication to other followers.

---

## 3. Follower: Receiving Entries

### 3.1 OnAppendEntries() — The Handler

**Location**: `server.cc:1272-1462`

When a follower receives an AppendEntries RPC, it performs three checks and then either accepts or rejects:

```
OnAppendEntries(leaderTerm, leaderPrevLogIndex, leaderPrevLogTerm,
                leaderCommitIndex, cmd, ...):
  |
  +-- [1] Pre-checks:
  |     term_ok     = (leaderTerm >= currentTerm)
  |     index_ok    = (leaderPrevLogIndex <= lastLogIndex)
  |     prev_term_ok = (prevLogIndex == 0 || local_term_at_prevIdx == leaderPrevLogTerm)
  |
  +-- [2] Term update:
  |     IF term_ok: resetTimer()
  |     IF leaderTerm > currentTerm: step down, update term, persist
  |
  +-- [3] Accept or Reject:
  |
  |     ALL checks pass → ACCEPT:
  |       Append entries (single or batch)
  |       Persist entries
  |       Update commitIndex = min(leaderCommitIndex, lastLogIndex)
  |       Reply: (ok=1, currentTerm, lastLogIndex)
  |       Release mutex, call applyLogs()
  |
  |     Any check fails → REJECT:
  |       Reply: (ok=0, currentTerm, lastLogIndex)
  |       Leader uses lastLogIndex for fast backoff
```

### 3.2 Consistency Check

The consistency check ensures the Raft Log Matching Property: if two logs contain an entry with the same index and term, all preceding entries are identical.

The check verifies:
1. **Term validity**: The leader's term is not stale (`leaderTerm >= currentTerm`)
2. **Index existence**: The follower's log extends at least to `leaderPrevLogIndex`
3. **Term match**: The entry at `leaderPrevLogIndex` has the same term the leader expects

If the term at `leaderPrevLogIndex` doesn't match, the follower's log diverged from the leader's at some point. The follower rejects, and the leader backtracks (see Section 4.2).

### 3.3 Entry Appending

**Non-batch mode** (server.cc:1330-1336):
```cpp
lastLogIndex = leaderPrevLogIndex + 1;
auto instance = GetRaftInstance(lastLogIndex);
instance->log_ = cmd;
instance->term = leaderNextLogTerm;
PersistLogEntry(lastLogIndex, *instance);
```

**Batch mode** (server.cc:1344-1358):
```cpp
auto cmds = dynamic_pointer_cast<TpcBatchCommand>(cmd);
int cnt = 0;
vector<pair<slotid_t, shared_ptr<RaftData>>> entries_to_persist;
for (shared_ptr<TpcCommitCommand>& c : cmds->cmds_) {
    cnt++;
    lastLogIndex = leaderPrevLogIndex + cnt;
    auto instance = GetRaftInstance(lastLogIndex);
    instance->log_ = c;
    instance->term = dynamic_pointer_cast<TpcCommitCommand>(c)->term;
    entries_to_persist.emplace_back(lastLogIndex, instance);
}
PersistLogEntries(entries_to_persist);  // Batch persist
```

In batch mode, all entries in the `TpcBatchCommand` are appended in a single pass, and persistence is batched into one `PersistLogEntries()` call.

### 3.4 Commit Index Advancement on Follower

After accepting entries (server.cc:1362-1368):
```cpp
if (leaderCommitIndex > commitIndex) {
    commitIndex = min(leaderCommitIndex, lastLogIndex);
    PersistCommitIndex();
}
```

The follower then calls `applyLogs()` to feed committed entries to the transaction layer.

### 3.5 Critical Design: Mutex Release During Apply

At server.cc:1377, the mutex is released before calling `applyLogs()`:

```cpp
mtx_.unlock();
if (need_apply) {
    applyLogs();  // Called WITHOUT holding the mutex
}
mtx_.lock();
```

This is deliberate: applying entries involves transaction processing that can be slow. Holding the mutex would block incoming AppendEntries RPCs, causing the leader to time out and potentially start unnecessary elections.

---

## 4. Leader: Processing Responses

### 4.1 Response Cases

After receiving a response from a follower, the leader handles four cases (server.cc:852-945):

**Case 0: Lost RPC** (`ret_status == false && ret_term == 0 && ret_last_log_index == 0`)
- Do nothing. The RPC was lost or the follower is unreachable.

**Case 1: Higher Term** (`ret_status == 0 && ret_term > term`)
- The follower has a higher term, meaning this leader is stale.
- Step down: `setIsLeader(false)`, update `currentTerm = ret_term`
- Skip remaining followers, restart the loop.

**Case 2: Log Conflict** (`ret_status == 0`)
- The follower rejected because its log doesn't match at `prevLogIndex`.
- Backtrack `next_index_` (see Section 4.2).

**Case 3: Success** (`ret_status == true`)
- Update tracking indices:
  - Non-batch: `match_index_ = next_index_; next_index_++`
  - Batch: `match_index_ = ret_last_log_index; next_index_ = ret_last_log_index + 1`
- Safety check: cap `match_index_` at `lastLogIndex`

### 4.2 Log Reconciliation — Backtracking next_index_

When a follower rejects an AppendEntries, the leader must find the correct `next_index_` where the logs agree. Three strategies are used (server.cc:873-898):

```
IF follower reported lastLogIndex AND lastLogIndex < next_index - 1:
    [FAST BACKOFF]
    next_index = follower's lastLogIndex + 1
    (Jump directly to where follower's log ends)

ELSE IF next_index > 10:
    [EXPONENTIAL BACKOFF]
    next_index = next_index / 2
    (Halve the gap to converge in O(log n) steps)

ELSE IF next_index > 1:
    [LINEAR BACKOFF]
    next_index--
    (Precise one-at-a-time near the log start)

ELSE:
    next_index = 1
    (Already at the beginning)
```

**Why three tiers?**

| Strategy | Convergence | When Used |
|----------|-------------|-----------|
| Fast backoff | O(1) | Follower reports actual last index |
| Exponential | O(log n) | Large gap, no follower hint |
| Linear | O(1) | Near log start, need precision |

Standard Raft decrements `next_index` by 1 on each rejection, requiring O(n) round trips in the worst case. This implementation's fast backoff reduces it to O(1) when the follower reports its last log index, and exponential backoff handles the case where the follower's response doesn't provide a useful hint.

---

## 5. Commit Advancement

### 5.1 Leader Computes commitIndex

The leader updates `commitIndex` in `HeartbeatLoop()` (server.cc:707-731):

```
Collect all match_index_ values into a sorted array
newCommitIndex = matchedIndices[(nservers - 1) / 2]  // Median = majority threshold

IF newCommitIndex > commitIndex
   AND raft_logs_[newCommitIndex].term == currentTerm:
    commitIndex = newCommitIndex
```

**Example for 3 servers** (leader + 2 followers):

```
match_index_ = {follower_B: 7, follower_C: 5}
Leader's own lastLogIndex = 10

Sorted: [5, 7]
Median index: (3-1)/2 = 1 → matchedIndices[1] = 7

Since leader also has entry 7, majority (leader + follower_B = 2/3) have it.
→ commitIndex = 7
```

### 5.2 The Term Safety Rule

An entry is only committed if its term equals `currentTerm` (server.cc:728):

```cpp
if (commitInstance && newCommitIndex > commitIndex
    && (commitInstance->term == currentTerm)) {
    commitIndex = newCommitIndex;
}
```

This implements the safety guarantee from Figure 8 of the Raft paper: a leader cannot commit entries from previous terms by counting replicas alone. It must first commit an entry from its own term, which implicitly commits all prior entries.

### 5.3 Follower Learns commitIndex

Followers learn the commit index from the `leaderCommitIndex` field in every AppendEntries RPC. On each accepted AppendEntries (server.cc:1363):

```cpp
commitIndex = min(leaderCommitIndex, lastLogIndex);
```

The `min` ensures a follower doesn't set `commitIndex` beyond what it has actually received.

---

## 6. Heartbeats

Heartbeats are empty AppendEntries RPCs (no log entries, `cmd = nullptr`). They serve three purposes:

1. **Lease renewal**: Reset followers' election timers to prevent unnecessary elections
2. **Commit propagation**: Carry the leader's `commitIndex` to followers
3. **Failure detection**: If a follower hasn't heard from the leader within the election timeout, it starts an election

### Heartbeat Interval

| Mode | `HEARTBEAT_INTERVAL` | Effective Send Rate |
|------|---------------------|---------------------|
| Production | 5,000 μs (5 ms) | ~200 heartbeats/sec |
| Test (`RAFT_TEST_CORO`) | 100,000 μs (100 ms) | ~10 heartbeats/sec |

The heartbeat interval must be much shorter than the election timeout to prevent false elections. With production settings: heartbeat = 5ms, election timeout = 150ms-1s, giving a safety factor of 30x-200x.

### Heartbeat Processing on Follower

When a follower receives an empty AppendEntries (server.cc:1303):
1. Reset election timer (`resetTimer()`)
2. Update `currentTerm` if leader's term is higher
3. Advance `commitIndex` if `leaderCommitIndex > commitIndex`
4. Apply committed entries via `applyLogs()`

---

## 7. Applying Committed Entries

### 7.1 applyLogs()

**Location**: `server.cc:581-625`

Both leaders and followers call `applyLogs()` after advancing `commitIndex`:

```
applyLogs():
  IF executeIndex < commitIndex: apply_pending_ = true
  IF in_applying_logs_: return  (reentrancy guard)
  in_applying_logs_ = true

  DO:
    apply_pending_ = false
    FOR id = executeIndex+1 TO commitIndex:
      instance = GetRaftInstance(id)
      IF instance AND instance->log_:
        app_next_(id, instance->log_)    ← Feeds to Mako transaction layer
        executeIndex = id
      ELSE: break
  WHILE apply_pending_  (new work arrived during apply)

  in_applying_logs_ = false

  // GC: remove entries > 60,000 slots behind executeIndex
```

### 7.2 The app_next_ Callback

`app_next_` is a callback registered by Mako's transaction layer (`TxLogServer::RegLearnerAction()`). When called with `(slot_id, cmd)`, it feeds the committed transaction into Mako's processing pipeline.

This is the integration boundary between Raft consensus and Mako's transaction system.

---

## 8. Complete Replication Sequence Diagram

The following shows a successful replication cycle in a 3-node cluster:

```
Time    Leader (A)                   Follower (B)              Follower (C)
 |
 |      Client: Submit(cmd)
 |      Start(cmd) → append at index 8
 |      ready_for_replication_->set(1)
 |
 |      [HeartbeatLoop wakes]
 |
 |      Compute commitIndex:
 |        match_index_ = {B:7, C:5}
 |        sorted = [5, 7]
 |        median = 7 → commitIndex = 7
 |        applyLogs() for 6,7
 |
 |      For B (next_index=8):
 |        prevLogIndex=7, prevLogTerm=1
 |        cmd = batch([entry_8])
 |        --- AppendEntries(term=1, prev=7, commit=7, batch=[8]) -->
 |
 |                                    [OnAppendEntries]
 |                                    term_ok=true, index_ok=true
 |                                    prev_term_ok=true → ACCEPT
 |                                    Append entry 8
 |                                    commitIndex = min(7, 8) = 7
 |                                    applyLogs() for 6,7
 |      <--- (ok=1, term=1, lastIdx=8) --
 |
 |      match_index_[B] = 8
 |      next_index_[B] = 9
 |
 |      For C (next_index=6):
 |        prevLogIndex=5, prevLogTerm=1
 |        cmd = batch([entry_6, entry_7, entry_8])
 |        --- AppendEntries(term=1, prev=5, commit=7, batch=[6,7,8]) ---------->
 |
 |                                                             [OnAppendEntries]
 |                                                             ACCEPT
 |                                                             Append entries 6,7,8
 |                                                             commitIndex = min(7,8) = 7
 |                                                             applyLogs() for 6,7
 |      <--- (ok=1, term=1, lastIdx=8) ------------------------------------------
 |
 |      match_index_[C] = 8
 |      next_index_[C] = 9
 |
 |      [Next heartbeat iteration]
 |      Compute commitIndex:
 |        match_index_ = {B:8, C:8}
 |        sorted = [8, 8]
 |        median = 8 → commitIndex = 8
 |        applyLogs() for 8
 |
 v      Entry 8 is now committed on all servers
```

### Timing

| Phase | Duration | Notes |
|-------|----------|-------|
| Start() → HeartbeatLoop wake | < 5 ms | Bounded by HEARTBEAT_INTERVAL |
| AppendEntries RPC round-trip | < 1 ms (local) | Network latency in production |
| Follower processing | < 1 ms | Log append + persistence |
| Commit: first RPC returns | ~5 ms | Entry committed (majority = 2/3) |
| Commit: all RPCs return | ~5-10 ms | All replicas up-to-date |

---

## 9. Log Conflict Resolution — Example

When a follower has divergent entries (e.g., from a crashed previous leader):

```
Leader log:   [1:a] [1:b] [2:c] [2:d] [3:e]
                                        ^
                                   lastLogIndex=5

Follower log: [1:a] [1:b] [2:c] [2:x] [2:y] [2:z]
                                              ^
                                        lastLogIndex=6
                            ^
                     Divergence at index 4
```

**Round 1**: Leader sends AppendEntries with prevLogIndex=4, prevLogTerm=2
- Follower: local_prev_term = 2 (for index 4, entry [2:x])
- prev_term_ok = (2 == 2) = true ... but the entries differ!
- Actually, the term matches, so follower accepts and overwrites index 5 with [3:e]

If the terms were different:
```
Leader log:   [1:a] [1:b] [3:c] [3:d]    term=3 at index 3
Follower log: [1:a] [1:b] [2:x] [2:y]    term=2 at index 3
```

**Round 1**: Leader sends AppendEntries with prevLogIndex=3, prevLogTerm=3
- Follower: local_prev_term = 2 ≠ 3 → REJECT
- Follower reports lastLogIndex=4

**Leader backtracks**: Fast backoff → next_index = 4+1 = 5? No, that's too high.
Exponential backoff → next_index = 3/2 = 1? Then try from beginning.

**Round 2**: Leader sends from index 2 (which matches on both)
- Follower accepts, overwrites divergent entries

---

## Related Documents

- [Protocol Overview](protocol_overview.md) — High-level Raft architecture
- [Server Implementation](server_implementation.md) — Full `RaftServer` walkthrough
- [Leader Election](leader_election.md) — Election cycle walkthrough
- [Coordinator](coordinator.md) — Transaction submission via `CoordinatorRaft`
- [Build System](../01-mako-overview/build_system.md) — `RAFT_BATCH_OPTIMIZATION` flag
