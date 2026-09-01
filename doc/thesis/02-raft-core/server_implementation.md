# RaftServer Implementation Deep Dive

## What This Document Covers

This document is a detailed walkthrough of the `RaftServer` class — the core Raft consensus state machine. It covers every member variable, the class hierarchy, and full algorithm walkthroughs for `OnRequestVote()`, `OnAppendEntries()`, `Start()`, `applyLogs()`, the election timer, log persistence, and RustyCpp safety annotations.

**Source files**: `src/deptran/raft/server.h` (638 lines) and `src/deptran/raft/server.cc` (1830 lines)

---

## 1. Class Hierarchy

```
Scheduler (base scheduling interface)
    |
    v
TxLogServer (transaction log server, provides app_next_ callback)
    |
    v
RaftServer (Raft consensus state machine)
```

- `Scheduler` — Base class for transaction scheduling (`src/deptran/scheduler.h`)
- `TxLogServer` — Extends `Scheduler` with log application infrastructure (`app_next_` callback), epoch management, and legacy Jetpack recovery code pending a separate audit (`src/deptran/scheduler.h:332`)
- `RaftServer` — Implements the Raft consensus protocol on top of `TxLogServer`

The key integration point is `app_next_`: a callback registered by Mako's transaction layer. When Raft commits a log entry, `applyLogs()` invokes `app_next_(slot_id, cmd)` to feed the committed command back into the transaction pipeline.

---

## 2. Member Variables

### 2.1 Persistent State (Must Survive Restarts)

| Variable | Type | Default | Location | Description |
|----------|------|---------|----------|-------------|
| `currentTerm` | `uint64_t` | `0` | `server.h:274` | Current election term. Monotonically increasing. |
| `vote_for_` | `siteid_t` | `INVALID_SITEID` | `server.h:89` | Candidate ID that received vote in current term, or `INVALID_SITEID` if none. |
| `raft_logs_` | `map<slotid_t, shared_ptr<RaftData>>` | `{}` | `server.h:277` | In-memory log entries indexed by slot ID. |

These are persisted via `PersistTermAndVote()` and `PersistLogEntry()` when a `LogStorage` backend is configured.

### 2.2 Volatile State (All Servers)

| Variable | Type | Default | Location | Description |
|----------|------|---------|----------|-------------|
| `lastLogIndex` | `uint64_t` | `0` | `server.h:273` | Index of the last log entry. |
| `commitIndex` | `uint64_t` | `0` | `server.h:275` | Index of highest committed entry (majority replicated). |
| `executeIndex` | `uint64_t` | `0` | `server.h:276` | Index of highest applied entry (fed to `app_next_`). |
| `is_leader_` | `bool` | `false` | `server.h:91` | Whether this server is the current leader. |
| `stop_` | `bool` | `false` | `server.h:88` | Shutdown signal. Checked by coroutines before calling virtual methods. |
| `last_heartbeat_time_` | `uint64_t` | `0` | `server.h:86` | Timestamp of last timer reset (for election timeout). |
| `req_voting_` | `bool` | `false` | `server.h:96` | True while an election is in progress. |
| `disconnected_` | `bool` | `false` | `server.h:95` | Whether this server is network-disconnected (for testing). |
| `looping_` | `bool` | `false` | `server.h:107` | Set when `HeartbeatLoop()` is running. Used as a shutdown guard. |

### 2.3 Volatile State (Leaders Only)

| Variable | Type | Default | Location | Description |
|----------|------|---------|----------|-------------|
| `match_index_` | `map<siteid_t, uint64_t>` | `{}` | `server.h:81` | Highest replicated index for each follower. |
| `next_index_` | `map<siteid_t, uint64_t>` | `{}` | `server.h:82` | Next log index to send to each follower. |

Initialized in `setIsLeader(true)`: `match_index_[peer] = 0`, `next_index_[peer] = lastLogIndex + 1` (`server.cc:460-466`).

### 2.4 Preferred Leader System

| Variable | Type | Default | Location | Description |
|----------|------|---------|----------|-------------|
| `preferred_leader_site_id_` | `siteid_t` | `INVALID_SITEID` | `server.h:126` | Configured preferred leader for this partition. |
| `leader_last_commit_index_` | `uint64_t` | `0` | `server.h:127` | Leader's commit index, cached from heartbeats. |
| `transferring_leadership_` | `bool` | `false` | `server.h:128` | True during leadership transfer protocol. |
| `leadership_transfer_start_time_` | `uint64_t` | `0` | `server.h:129` | When transfer was initiated (for timeout). |
| `leadership_monitor_stop_` | `atomic<bool>` | `false` | `server.h:130` | Signal to stop background monitor thread. |
| `leadership_monitor_thread_` | `std::thread` | — | `server.h:131` | Background thread checking for transfer opportunity. |
| `startup_timestamp_` | `uint64_t` | `0` | `server.h:132` | Server startup time (for grace period logic). |

### 2.5 Log Application Control

| Variable | Type | Default | Location | Description |
|----------|------|---------|----------|-------------|
| `in_applying_logs_` | `bool` | `false` | `server.h:97` | Reentrancy guard for `applyLogs()`. |
| `apply_pending_` | `atomic<bool>` | `false` | `server.h:98` | Tracks if new work arrived while applying logs. |
| `min_active_slot_` | `slotid_t` | `1` | `server.h:262` | Slots before this have been freed (GC boundary). |

### 2.6 Persistence and Snapshot

| Variable | Type | Location | Description |
|----------|------|----------|-------------|
| `log_storage_` | `shared_ptr<LogStorage>` | `server.h:52` | Optional persistent storage backend (RocksDB). |
| `snapshot_manager_` | `shared_ptr<SnapshotManager>` | `server.h:57` | Optional snapshot manager. |

### 2.7 Timer and Configuration

| Variable | Type | Default | Location | Description |
|----------|------|---------|----------|-------------|
| `timer_` | `rusty::Box<Timer>` | — | `server.h:85` | Owned timer for election timeout. RustyCpp `Box` ensures cleanup. |
| `snapidx_` | `slotid_t` | `0` | `server.h:92` | Snapshot index (entries before this are compacted). |
| `snapterm_` | `ballot_t` | `0` | `server.h:93` | Term of the last snapshot entry. |
| `failover_` | `bool` | `true` | `server.h:102` | Whether election failover is enabled. |

### 2.8 RaftData Structure

Each log entry is stored as a `RaftData` struct (`server.h:20-33`):

```cpp
struct RaftData {
  ballot_t max_ballot_seen_ = 0;       // Legacy Paxos fields
  ballot_t max_ballot_accepted_ = 0;
  shared_ptr<Marshallable> accepted_cmd_{nullptr};
  shared_ptr<Marshallable> committed_cmd_{nullptr};

  ballot_t term;                        // Term when entry was created
  shared_ptr<Marshallable> log_{nullptr}; // The actual command

  // For retries
  ballot_t prevTerm;
  slotid_t slot_id;
  ballot_t ballot;
};
```

---

## 3. OnRequestVote() — Vote Granting

**Location**: `server.cc:1120-1185`

This is the handler for incoming `RequestVote` RPCs from candidates.

### Algorithm Walkthrough

```
OnRequestVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term*, vote_granted*, cb)
  |
  +-- [1] Lock mutex
  |
  +-- [2] Stale term? (can_term < currentTerm)
  |     YES -> doVote(false) -> return
  |
  +-- [3] Already voted for someone else this term?
  |     (can_term == currentTerm && vote_for_ != INVALID && vote_for_ != can_id)
  |     YES -> doVote(false) -> return
  |
  +-- [4] Already voted for THIS candidate? (idempotent)
  |     (can_term == currentTerm && vote_for_ == can_id)
  |     YES -> doVote(true) -> return
  |
  +-- [5] Log up-to-date check:
  |     Compute my last log term and index
  |     Candidate's log at least as up-to-date?
  |       (lst_log_term > my_last_term) OR
  |       (lst_log_term == my_last_term AND lst_log_idx >= my_last_idx)
  |     YES -> doVote(true)
  |     NO  -> doVote(false)
```

### The doVote() Helper (`server.h:161-202`)

`doVote()` performs the actual vote response:

1. **Term advancement**: If `can_term > currentTerm`, steps down to follower, updates term, resets `vote_for_`, persists (`server.h:177-186`)
2. **Record vote**: If granting, sets `vote_for_ = can_id` and resets the election timer (`server.h:189-198`)
3. **Reply**: Sets `*vote_granted` and `*reply_term`, invokes callback

### Key Safety Properties

- **Single vote per term**: Line 1141 ensures at most one vote per term (except idempotent re-votes for the same candidate)
- **Log completeness**: Lines 1176 ensures only candidates with up-to-date logs can be elected
- **Persistence**: `PersistTermAndVote()` and `PersistVote()` are called before replying, ensuring crash safety

---

## 4. OnAppendEntries() — Log Replication

**Location**: `server.cc:1272-1462`

This is the handler for incoming `AppendEntries` RPCs from the leader.

### Algorithm Walkthrough

```
OnAppendEntries(leaderCurrentTerm, leaderSiteId, leaderPrevLogIndex,
                leaderPrevLogTerm, leaderCommitIndex, cmd, ..., trigger_election_now)
  |
  +-- [1] Lock mutex
  |
  +-- [2] Pre-checks:
  |     term_ok    = (leaderCurrentTerm >= currentTerm)
  |     index_ok   = (leaderPrevLogIndex <= lastLogIndex)
  |     prev_term_ok = (prevLogIndex == 0 || local_term_at_prevIndex == leaderPrevLogTerm)
  |
  +-- [3] If term_ok: reset election timer
  |     If leaderCurrentTerm > currentTerm: update term, step down, persist
  |
  +-- [4] All checks pass?
  |     |
  |     +-- YES (Accept):
  |     |     [4a] If cmd != null:
  |     |           Non-batch: append single entry at leaderPrevLogIndex + 1
  |     |           Batch: iterate TpcBatchCommand, append each entry
  |     |           Persist entries
  |     |     [4b] Update commitIndex = min(leaderCommitIndex, lastLogIndex)
  |     |     [4c] Set followerAppendOK = 1, report currentTerm and lastLogIndex
  |     |     [4d] Release mutex, call applyLogs() if needed, re-acquire mutex
  |     |
  |     +-- NO (Reject):
  |           Set followerAppendOK = 0, report currentTerm and lastLogIndex
  |           (Leader uses lastLogIndex for fast backoff)
  |
  +-- [5] Piggybacked leadership transfer:
  |     If trigger_election_now && AmIPreferredLeader() && !IsLeader():
  |       Sleep 30ms, then start election via RequestVote()
  |
  +-- [6] Unlock mutex, invoke callback
```

### Critical Design Decision: Mutex Release During Apply

At line 1377, the mutex is released *before* calling `applyLogs()`. This is deliberate: applying entries can be slow (involves transaction processing), and holding the mutex would block incoming AppendEntries RPCs. The `in_applying_logs_` / `apply_pending_` mechanism in `applyLogs()` handles concurrent arrivals safely.

### Batch Optimization (`RAFT_BATCH_OPTIMIZATION`)

When enabled, `AppendEntries` carries a `TpcBatchCommand` containing multiple `TpcCommitCommand` entries. The follower iterates the batch and appends all entries in one pass (`server.cc:1344-1358`), with a single batched `PersistLogEntries()` call.

---

## 5. Start() — Leader Appends Commands

**Location**: `server.cc:1230-1267`

This is the entry point for new commands from `CoordinatorRaft`.

```cpp
bool RaftServer::Start(shared_ptr<Marshallable>& cmd,
                       uint64_t* index, uint64_t* term,
                       slotid_t slot_id, ballot_t ballot)
{
    lock(mtx_);

    // Only leader can accept new commands
    if (!IsLeader()) {
        *index = 0; *term = 0;
        return false;
    }

    // Append to local log
    SetLocalAppend(cmd, term, index, slot_id, ballot);

    // SetLocalAppend returns old lastLogIndex; Start returns the new index
    verify(lastLogIndex == (*index) + 1);
    *index = lastLogIndex;
    return true;
}
```

### SetLocalAppend() — The Actual Append (`server.h:313-384`)

1. Records old `lastLogIndex` in `*index`
2. Increments `lastLogIndex`
3. Creates `RaftData` entry with `cmd`, `currentTerm`, `slot_id`, `ballot`
4. Persists the entry via `PersistLogEntry()`

After `Start()` returns, the caller (`CoordinatorRaft`) signals `ready_for_replication_` to wake the `HeartbeatLoop`, which replicates the new entry to followers.

---

## 6. applyLogs() — State Machine Application

**Location**: `server.cc:581-625`

This function applies committed but unapplied log entries to the state machine.

```
applyLogs()
  |
  +-- [1] If executeIndex < commitIndex: set apply_pending_ = true
  |
  +-- [2] If in_applying_logs_: return (let current apply handle it)
  |
  +-- [3] in_applying_logs_ = true
  |
  +-- [4] LOOP:
  |     |  apply_pending_ = false
  |     |  FOR id = executeIndex+1 TO commitIndex:
  |     |    instance = GetRaftInstance(id)
  |     |    if instance && instance->log_:
  |     |      app_next_(id, instance->log_)    <-- feeds to Mako transaction layer
  |     |      executeIndex = id
  |     |    else: break
  |     |
  |     +-- WHILE apply_pending_ (new work arrived during this iteration)
  |
  +-- [5] in_applying_logs_ = false
  |
  +-- [6] GC: remove old commands where slot + 60000 < executeIndex
```

### Concurrency Design

The `do-while` loop with `apply_pending_` is a lock-free work notification pattern:

- `applyLogs()` can be called concurrently from `HeartbeatLoop()` (leader) and `OnAppendEntries()` (follower)
- The `in_applying_logs_` flag prevents reentrancy — if another call arrives while applying, it sets `apply_pending_` and returns
- The current apply loop sees `apply_pending_` at the end and loops again, picking up the new work
- This guarantees no committed entry is ever missed, even under heavy load

### Garbage Collection

Lines 619-624 remove old entries to prevent unbounded memory growth. Entries are removed when they are more than 60,000 slots behind `executeIndex`.

---

## 7. Election Timer

### 7.1 StartElectionTimer() (`server.cc:1188-1228`)

Launches a coroutine that periodically checks if the election timeout has fired:

```
StartElectionTimer()
  |
  +-- resetTimer("start election timer")
  +-- last_heartbeat_time_ = now()
  |
  +-- Fiber::create_run([this]() {
        WHILE !stop_:
          election_timeout = GetElectionTimeout()
          sleep(random(HEARTBEAT_INTERVAL*2, HEARTBEAT_INTERVAL*4))

          time_elapsed = now() - last_heartbeat_time_

          IF !IsLeader() && time_elapsed > election_timeout:
            req_voting_ = true
            RequestVote()
            WHILE req_voting_: sleep(wait_int_)
      })
```

### 7.2 GetElectionTimeout() — Dynamic Timeout (`server.cc:348-369`)

Returns a timeout in microseconds based on the server's role:

| Condition | Timeout Range | Purpose |
|-----------|---------------|---------|
| Preferred leader (`AmIPreferredLeader()`) | 150-300 ms | Win elections quickly |
| Non-preferred, grace period (< 5s since startup) | 1-2 s | Let preferred leader win first |
| Non-preferred, after grace period | 500 ms - 1 s | Standard failover |

### 7.3 resetTimer() (`server.h:229-241`)

Updates `last_heartbeat_time_` to the current time and restarts the timer. Called on:
- Receiving valid `AppendEntries` from current-term leader
- Granting a vote
- Becoming a follower (stepping down)
- Reconnection

### 7.4 HEARTBEAT_INTERVAL

Defined as a preprocessor constant (`server.h:40-44`):
- **Test mode** (`RAFT_TEST_CORO`): 100,000 μs = 100 ms
- **Production mode**: 5,000 μs = 5 ms

---

## 8. HeartbeatLoop() — Leader Replication Loop

**Location**: `server.cc:636-961`

This is the main replication loop, running continuously on every server (but only active when leader).

### Loop Structure

```
HeartbeatLoop()
  |
  +-- Initialize match_index_ and next_index_ for all peers
  +-- looping_ = true
  |
  +-- WHILE looping_:
        |
        +-- [A] Wait: ready_for_replication_->wait(HEARTBEAT_INTERVAL)
        |         (woken by Start() or timer)
        |
        +-- [B] If !IsLeader(): continue (skip iteration)
        |
        +-- [C] FOR each follower in next_index_:
        |     |
        |     +-- [C1] Update commitIndex:
        |     |     Collect all match_index_ values
        |     |     Sort ascending
        |     |     newCommitIndex = matchedIndices[(N-1)/2]  (majority)
        |     |     If newCommitIndex > commitIndex && term matches: update
        |     |     If commitIndex > executeIndex: applyLogs()
        |     |
        |     +-- [C2] Prepare AppendEntries:
        |     |     prevLogIndex = next_index_[peer] - 1
        |     |     prevLogTerm = raft_logs_[prevLogIndex]->term
        |     |     cmd = batch of entries from next_index_ to lastLogIndex
        |     |
        |     +-- [C3] Send AppendEntries RPC (500ms timeout)
        |     |
        |     +-- [C4] Handle response:
        |           |
        |           +-- Higher term from follower:
        |           |     Step down (setIsLeader(false))
        |           |     Update currentTerm
        |           |
        |           +-- Rejected (log conflict):
        |           |     Fast backoff: next_index_ = follower's lastLogIndex + 1
        |           |     OR exponential backoff: next_index_ /= 2
        |           |     OR linear backoff: next_index_--
        |           |
        |           +-- Accepted:
        |                 Update match_index_ = follower's lastLogIndex
        |                 Update next_index_ = follower's lastLogIndex + 1
```

### Commit Index Calculation (`server.cc:707-731`)

The leader computes the new commit index by sorting all `match_index_` values and taking the median:

```
For N=3 (3 servers): matchedIndices[(3-1)/2] = matchedIndices[1]
  → The 2nd highest value, meaning at least 2 of 3 servers have it
  → This is the majority quorum (N/2 + 1 = 2)
```

An entry is only committed if its term equals `currentTerm` (the Raft paper's Figure 8 safety rule, line 728).

### Log Reconciliation Strategies (`server.cc:873-898`)

When a follower rejects `AppendEntries`, the leader backtracks `next_index_`:

1. **Fast backoff** (line 879): Use follower's reported `lastLogIndex` to jump directly. Reduces O(n) round trips to O(1) for simple divergence.
2. **Exponential backoff** (line 885): When `next_index > 10`, halve it. Converges in O(log n) steps.
3. **Linear backoff** (line 891): When `next_index <= 10`, decrement by 1 for precision.

---

## 9. Log Persistence

### 9.1 Persistence Functions

| Function | What It Persists | When Called | Location |
|----------|-----------------|-------------|----------|
| `PersistTermAndVote()` | `currentTerm`, `vote_for_` | Term change, vote grant | `server.cc:36-44` |
| `PersistVote()` | `vote_for_` only | Vote grant (optimization) | `server.cc:47-54` |
| `PersistCommitIndex()` | `commitIndex` | Commit advancement | `server.cc:57-64` |
| `PersistLogEntry()` | Single log entry | `SetLocalAppend()` on leader | `server.cc:67-80` |
| `PersistLogEntries()` | Batch of entries | Batch append on follower | `server.cc:83-102` |

All persistence functions check `log_storage_ && log_storage_->is_open()` before writing. If no storage backend is configured, they are no-ops.

### 9.2 Recovery (`server.cc:105-203`)

`RecoverFromStorage()` restores state on restart:

1. Reads `currentTerm` from metadata
2. Reads `vote_for_` from metadata
3. Reads `commitIndex` from metadata
4. Reads `lastLogIndex` from storage
5. Loads all log entries into `raft_logs_`

`ReplayCommittedEntries()` replays entries `[executeIndex+1, commitIndex]` through `app_next_()` after the callback is registered.

### 9.3 Log Compaction (`server.cc:206-256`)

`CompactLog(up_to_index)` removes entries covered by a snapshot:
- Safety check: never compacts beyond `commitIndex`
- Removes from both persistent storage and in-memory `raft_logs_`
- Updates `min_active_slot_`

---

## 10. setIsLeader() — State Transitions

**Location**: `server.cc:444-578`

This function handles all leader/follower transitions:

### Becoming Leader (`become_new_leader`, lines 491-538)

1. Initialize `match_index_` = 0, `next_index_` = `lastLogIndex + 1` for all peers (lines 460-468)
2. Clear `transferring_leadership_` flag (line 499)
3. Update `new_view_` with self as leader, notify communicator (lines 504-519)
4. Reach the legacy Jetpack recovery entry point (unsupported; separate audit pending)
5. If non-preferred leader: start `StartLeadershipTransferMonitoring()` (lines 534-538)
6. Fire `leader_change_cb_(true)` (lines 570-572)

### Becoming Follower (`become_new_follower`, lines 539-565)

1. Reset election timer to prevent immediate re-election (line 551)
2. Stop leadership transfer monitoring thread (line 562)
3. Fire `leader_change_cb_(false)` (lines 573-575)

---

## 11. Destructor — Safe Shutdown

**Location**: `server.cc:963-993`

```
~RaftServer()
  |
  +-- stop_ = true                    // Signal all coroutines
  +-- looping_ = false                // Stop HeartbeatLoop
  +-- ready_for_replication_->set(1)  // Wake sleeping HeartbeatLoop
  +-- StopLeadershipTransferMonitoring()
  +-- sleep(100ms)                    // Let detached coroutines see stop_
  +-- Log final stats
```

The 100ms sleep is critical: election timer and leadership transfer coroutines are detached, and without this delay, they may try to call virtual methods (like `RequestVote()`) through a collapsed vtable after the destructor runs.

---

## 12. RustyCpp Safety Annotations

### @safe Methods

| Method | Why Safe |
|--------|----------|
| `GetElectionTimeout()` | Read-only computation with local variables |
| `Setup()` | Creates coroutines with fiber framework |
| `IsLeader()` | Read-only accessor with shutdown guard |
| `applyLogs()` | Uses atomic flags for concurrency, calls `app_next_` |
| `IsDisconnected()` | Read-only accessor |
| `setIsLeader()` | Modifies owned state under mutex |
| `LogTermChange()` | Read-only logging |

### @unsafe Methods

| Method | Why Unsafe |
|--------|------------|
| `OnRequestVote()` | Calls `doVote()` with pointer outputs |
| `OnAppendEntries()` | Modifies log state, calls `applyLogs()` |
| `Start()` | Calls `SetLocalAppend()` with pointer outputs |
| `SetLocalAppend()` | Legacy I/O code, `dynamic_pointer_cast` chains |
| `HeartbeatLoop()` | Complex mutable state, `SendAppendEntries2()` with pointer outputs |
| `PersistTermAndVote()` | Uses `LogStorage` API (third-party) |
| `RecoverFromStorage()` | Uses `LogStorage` API, `std::stoull` |
| `commo()` | Raw pointer cast from `commo_` base pointer |
| `Disconnect()` | Modifies proxy maps |

### Key Pattern: Timer Ownership

The `timer_` field uses `rusty::Box<Timer>` instead of `std::unique_ptr<Timer>`:

```cpp
RaftServer::RaftServer(Frame* frame)
  : timer_(rusty::Box<Timer>::make(Timer()))  // server.cc:317
```

This follows the RustyCpp migration: `Box<T>` provides single-ownership semantics with automatic cleanup, replacing raw pointers or `unique_ptr`.

---

## 13. Constants and Configuration

| Constant | Value | Location | Purpose |
|----------|-------|----------|---------|
| `HEARTBEAT_INTERVAL` | 5,000 μs (prod) / 100,000 μs (test) | `server.h:41-44` | How often the leader sends heartbeats |
| `INVALID_SITEID` | `(siteid_t)-1` | `server.h:16` | Sentinel for "no site" |
| `NUM_BATCH_TIMER_RESET` | 100 | `server.h:17` | Batch counter threshold for timer reset |
| `SEC_BATCH_TIMER_RESET` | 1 | `server.h:18` | Time threshold for batched timer reset |
| GC threshold | 60,000 entries | `server.cc:620` | Slots beyond this behind `executeIndex` are freed |

---

## Related Documents

- [Protocol Overview](protocol_overview.md) — High-level Raft architecture
- [Leader Election](leader_election.md) — Election cycle walkthrough
- [Log Replication](log_replication.md) — Replication mechanism and optimizations
- [Coordinator](coordinator.md) — Transaction submission via `CoordinatorRaft`
- [Preferred Leader](../03-preferred-leader/mechanism.md) — Leadership transfer protocol
