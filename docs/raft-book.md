# The Raft Book

A comprehensive developer guide for the Raft consensus implementation in Mako.

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Architecture Overview](#2-architecture-overview)
3. [Core Data Structures](#3-core-data-structures)
4. [Leader Election](#4-leader-election)
5. [Log Replication](#5-log-replication)
6. [Log Application and Commit](#6-log-application-and-commit)
7. [Speculative Voting and Replication](#7-speculative-voting-and-replication)
8. [Preferred Leader Election](#8-preferred-leader-election)
9. [Leadership Transfer](#9-leadership-transfer)
10. [Mako Integration](#10-mako-integration)
11. [Log Persistence](#11-log-persistence)
12. [Snapshotting and Log Compaction](#12-snapshotting-and-log-compaction)
13. [RPC Communication Layer](#13-rpc-communication-layer)
14. [Configuration](#14-configuration)
15. [Build System](#15-build-system)
16. [Testing](#16-testing)
17. [Failure Scenarios and Recovery](#17-failure-scenarios-and-recovery)
18. [Known Issues and Workarounds](#18-known-issues-and-workarounds)
19. [Debugging](#19-debugging)
20. [Membership Changes](#20-membership-changes)

---

## 1. Introduction

Mako supports **two pluggable replication backends**: Paxos (default) and Raft. This guide covers the Raft implementation in depth — the protocol, integration with Mako's transaction system, and practical development guidance.

### Raft in Mako

Raft is **not** a separate client-server system. Each Mako node runs Raft **in the same process** as its transaction layer:

```
Process (e.g., localhost:38100)
  +-- Mako Worker Threads (TPC-C transactions)
  +-- RaftWorker (bridge between Mako and Raft)
  +-- RaftServer (consensus state machine)
  +-- RaftCommo (inter-node RPC)
```

Mako worker threads call a **local function** (`add_log_to_nc()`) to submit transaction logs to Raft — this is not an RPC call, it's an in-process function invocation.

### Why Raft?

- Well-understood protocol with clear correctness arguments
- Strong leader model simplifies reasoning about log ordering
- Built-in leader election (no separate election service)
- Easier to debug than Multi-Paxos (single log per partition)

### Key Files

| File | Purpose | LOC |
|------|---------|-----|
| `src/deptran/raft/server.{h,cc}` | Core Raft state machine | ~1600 |
| `src/deptran/raft/raft_worker.{h,cc}` | Worker thread management, callbacks | |
| `src/deptran/raft/commo.{h,cc}` | RPC communication for Raft messages | |
| `src/deptran/raft/frame.{h,cc}` | Protocol frame registration | |
| `src/deptran/raft/service.{h,cc}` | Incoming RPC handlers | |
| `src/deptran/raft/coordinator.{h,cc}` | Client submission bridge | |
| `src/deptran/raft/test.{h,cc}` | Lab-style test framework | |
| `src/deptran/raft/testconf.{h,cc}` | Test configuration and helpers | |
| `src/deptran/raft_main_helper.{h,cc}` | Mako-Raft bridge layer | ~550 |

---

## 2. Architecture Overview

```
+---------------------------------------------------------------+
|                    Mako Transaction Layer                       |
|  (Worker threads execute TPC-C, serialize tx logs)             |
+------------------------------+--------------------------------+
                               |  add_log_to_nc() [local call]
+------------------------------v--------------------------------+
|                    raft_main_helper                             |
|  (Bridge: routes logs, registers callbacks, manages workers)   |
+------------------------------+--------------------------------+
                               |
+------------------------------v--------------------------------+
|                    RaftWorker                                   |
|  (Submit queue, batch aggregation, callback dispatch)          |
+------------------------------+--------------------------------+
                               |
+------------------------------v--------------------------------+
|                    RaftServer                                   |
|  +------------------+  +------------------+                    |
|  | Leader Election  |  | Log Replication  |                    |
|  | - RequestVote    |  | - AppendEntries  |                    |
|  | - Term tracking  |  | - Commit index   |                    |
|  | - Timeout bias   |  | - Match index    |                    |
|  +------------------+  +------------------+                    |
|  +------------------+  +------------------+                    |
|  | Log Management   |  | State Machine    |                    |
|  | - raft_logs_     |  | - applyLogs()    |                    |
|  | - Persistence    |  | - app_next_ cb   |                    |
|  +------------------+  +------------------+                    |
+------------------------------+--------------------------------+
                               |  Raft RPCs
+------------------------------v--------------------------------+
|                    RaftCommo                                    |
|  (SendAppendEntries, BroadcastVote, SendTimeoutNow, etc.)     |
+---------------------------------------------------------------+
```

### Registration

Raft is registered as `MODE_RAFT` via the Frame factory pattern:

```cpp
REG_FRAME(MODE_RAFT, vector<string>({"raft"}), RaftFrame);
```

This allows runtime switching between Paxos, Raft, and other consensus protocols using YAML configuration:

```yaml
mode:
  ab: raft    # Use Raft for atomic broadcast
```

---

## 3. Core Data Structures

### RaftServer State

```cpp
class RaftServer {
    // Persistent state (must survive restarts)
    uint64_t currentTerm;          // Latest term seen
    siteid_t vote_for_;            // Who we voted for in current term
    map<slotid_t, RaftData> raft_logs_;  // Log entries

    // Volatile state (all servers)
    uint64_t commitIndex;          // Highest committed log index
    uint64_t executeIndex;         // Highest applied log index
    uint64_t lastLogIndex;         // Highest log index stored
    bool is_leader_;               // Current leadership status

    // Volatile state (leaders only)
    map<siteid_t, uint64_t> match_index_;  // Highest replicated index per follower
    map<siteid_t, uint64_t> next_index_;   // Next index to send to each follower

    // Speculative state (Phase 7)
    uint64_t specCommitIndex_;     // Memory-quorum commit index
    uint64_t securedLogIndex_;     // Disk-quorum commit index
    set<siteid_t> specVoters_;     // Replicas that voted in memory
    set<siteid_t> durableVoters_;  // Replicas that persisted vote

    // Preferred leader state
    siteid_t preferred_leader_site_id_;
    uint64_t startup_timestamp_;
    uint64_t last_heartbeat_from_preferred_time_;
};
```

### RaftData (Log Entry)

```cpp
struct RaftData {
    ballot_t term;                            // Term when entry was created
    shared_ptr<Marshallable> log_;            // The command payload
    ballot_t prevTerm;                        // For retry logic
    slotid_t slot_id;                         // Log index
    ballot_t ballot;                          // Command ballot
};
```

---

## 4. Leader Election

### Election Flow

1. **Timeout detection**: Follower's election timer expires (150ms-2s depending on role)
2. **Become candidate**: Increment `currentTerm`, vote for self
3. **Persist**: Write `currentTerm` and `vote_for_` to disk (critical for safety)
4. **Broadcast**: Send `RequestVote` RPC to all peers with `(lastLogIndex, lastLogTerm, candidateTerm)`
5. **Collect votes**: Wait for majority response (with 1-second timeout)
6. **Win or lose**: If majority grants vote, become leader and begin heartbeats

### Vote Decision (OnRequestVote)

A server grants its vote if:
- Candidate's term >= server's current term
- Server hasn't already voted for a different candidate in this term
- Candidate's log is at least as up-to-date:
  - `candidate.lastLogTerm > self.lastLogTerm`, OR
  - `candidate.lastLogTerm == self.lastLogTerm AND candidate.lastLogIndex >= self.lastLogIndex`

### Election Timeout

Dynamic timeouts based on role (see [Preferred Leader Election](#8-preferred-leader-election)):

| Role | Timeout Range | Purpose |
|------|--------------|---------|
| Preferred replica | 150-300ms | Win elections quickly |
| Non-preferred (grace period, 0-5s) | 1-2s | Let preferred win at startup |
| Non-preferred (after grace) | 500ms-1s | Can take over if preferred fails |

### Safety Properties

- **Term monotonicity**: Term always increases; seeing a higher term causes immediate step-down
- **Vote persistence**: Vote is persisted to disk before responding (prevents double-voting after crash)
- **Log completeness**: Candidate with the most up-to-date log wins

---

## 5. Log Replication

### AppendEntries RPC

The leader sends `AppendEntries` to all followers, either as heartbeats (empty) or with log entries:

```
Leader sends:
  - prevLogIndex, prevLogTerm  (for consistency check)
  - entries[]                  (log entries to replicate, or empty for heartbeat)
  - leaderCommitIndex          (leader's commit index)

Follower checks:
  1. leaderTerm >= currentTerm?             (accept leader authority)
  2. leaderPrevLogIndex <= lastLogIndex?     (have log space)
  3. raft_logs_[prevLogIndex].term == prevLogTerm?  (consistency match)

If all pass:
  - Append entries to log
  - Update commitIndex = min(leaderCommitIndex, lastLogIndex)
  - Return (ok=1, currentTerm, lastLogIndex)

If any fail:
  - Return (ok=0, currentTerm, lastLogIndex)
  - Leader decrements next_index_ and retries
```

### Parallel Replication

AppendEntries RPCs are sent to all followers in parallel using async callbacks:

```cpp
// In HeartbeatLoop (server.cc)
for (each follower) {
    commo->SendAppendEntries2(follower, cmd, prevLogIndex, prevLogTerm, ...);
    // Non-blocking: callback processes response
}
```

### Commit Index Calculation

The leader calculates the new commit index each heartbeat cycle:

1. Collect `match_index_[follower]` from all followers
2. Sort the values; new commit index = median (quorum position)
3. Only advance if the entry at the new index has the leader's current term
4. Trigger `applyLogs()` when commit index advances

### Batch Optimization

When `RAFT_BATCH_OPTIMIZATION` is enabled, multiple log entries are aggregated into a single `TpcBatchCommand` per AppendEntries RPC, reducing per-entry overhead.

---

## 6. Log Application and Commit

### applyLogs()

Committed entries are applied to the state machine in order:

```cpp
void RaftServer::applyLogs() {
    while (executeIndex < commitIndex) {
        auto instance = raft_logs_[executeIndex + 1];
        if (instance.log_ exists) {
            app_next_(executeIndex + 1, instance.log_);  // Callback to Mako
            executeIndex++;
        } else {
            break;  // Stop at gaps
        }
    }
}
```

**Key properties:**
- Always applies in strict order (slot 1, 2, 3, ...)
- Stops if a slot is missing (no holes)
- Calls `app_next_` callback for each entry (registered by Mako layer)
- Garbage collects old entries (`executeIndex - 60000`)

### Client Notification (Phase 5.3)

The leader can notify clients about entry status:

| Status | Meaning |
|--------|---------|
| `SPECULATIVE` | Entry reached memory quorum |
| `DURABLE` | Entry reached disk quorum with secured leader |
| `ROLLEDBACK` | Entry discarded (leader stepped down) |

#### Reason-Aware Rollback Notification

When a leader steps down, `NotifyRollback(StepDownReason)` differentiates rollback behavior based on why the step-down occurred:

| StepDownReason | Rollback Range | Rationale |
|----------------|---------------|-----------|
| `UnsecuredFailure` | `(commitIndex, lastLogIndex]` | Lost speculative quorum while unsecured. All current-term entries are suspect since no durable quorum was ever achieved. |
| `SecuredFailure` | `(securedLogIndex_, specCommitIndex_]` | Lost quorum but was secured leader. Entries up to `securedLogIndex_` are durably committed and safe. Only unsecured entries above that are suspect. |
| `HigherTerm` | None (no rollback sent) | Saw higher term from another server. Entries may still be valid under the new leader, so no premature rollback notification is sent. |

After notification, all pending callbacks are cleared and notification tracking indices are reset, regardless of reason (the server is no longer leader).

---

## 7. Speculative Voting and Replication

### Overview

Standard Raft requires disk persistence before responding to RPCs. Speculative mode decouples memory acknowledgment from disk persistence for lower latency:

```
Standard:   Receive -> Persist to disk -> Respond
Speculative: Receive -> Respond (memory ack) -> Persist async -> Send durable ack
```

### Speculative Voting

1. Follower receives `RequestVote`
2. Responds immediately with memory vote (speculative)
3. Persists vote asynchronously in background
4. Sends `VoteDurable` RPC after fsync completes

Leader tracks two voter sets:
- `specVoters_`: Replicas that voted in memory (fast)
- `durableVoters_`: Replicas that persisted vote (safe)

Leader becomes **secured** when `durableVoters_ >= quorum`.

### Speculative Replication

1. Follower receives `AppendEntries`
2. Appends to in-memory log, responds immediately (memory ack)
3. Persists asynchronously
4. Sends `AppendEntriesDurable` RPC after fsync

Leader tracks:
- `specCommitIndex_`: Advances on memory quorum
- `securedLogIndex_`: Advances on durable quorum (if secured leader)

### Invariants

```
securedLogIndex_ <= specCommitIndex_ <= lastLogIndex
durableVoters_ is a subset of specVoters_ (conceptually)
If durableVoters_ < quorum AND specVoters_ < quorum: step down
```

---

## 8. Preferred Leader Election

### Problem

Under CPU contention (e.g., 6 partitions running TPC-C), heartbeats can be delayed 500ms-1s. Without bias, non-preferred replicas may interpret delayed heartbeats as leader failure and start unnecessary elections, causing leadership churn.

### Phase 1: Election Timeout Bias

```cpp
uint64_t RaftServer::GetElectionTimeout() {
    uint64_t now = Time::now();
    bool in_grace = (now - startup_timestamp_) < 5000000;  // 5s grace

    if (AmIPreferredLeader()) {
        return 150000 + rand(0, 150000);  // 150-300ms
    } else if (in_grace) {
        return 1000000 + rand(0, 1000000);  // 1-2s (let preferred win)
    } else {
        return 500000 + rand(0, 500000);  // 500ms-1s (can take over)
    }
}
```

### Phase 2: Conditional Election Suppression

Non-preferred replicas check whether the preferred leader recently sent a heartbeat before starting an election:

```cpp
// In StartElectionTimer callback
if (!AmIPreferredLeader() && preferred_leader_site_id_ != INVALID_SITEID) {
    uint64_t time_since = now - last_heartbeat_from_preferred_time_;
    if (time_since < 1500000) {  // 1.5 seconds
        // Preferred is alive but slow - suppress election
        reset_timer_and_return();
    }
    // No heartbeat for 1.5s -> preferred is dead, allow election
}
```

This eliminates leadership churn under load while still allowing fast failover when the preferred leader truly fails.

---

## 9. Leadership Transfer

### TimeoutNow Protocol

When a non-preferred leader detects that the preferred replica has caught up:

1. **Monitor**: Non-preferred leader checks `match_index_[preferred] == lastLogIndex`
2. **Transfer**: Send `TimeoutNow` RPC to preferred replica
3. **Immediate election**: Preferred replica bypasses election timeout, calls `RequestVote()` immediately
4. **Win**: Preferred replica wins due to election timeout bias (<1 second total)

### Safety

- Transfer only happens after preferred replica has all committed logs
- 30ms delay before transfer election prevents storms
- Non-preferred can still take over if preferred subsequently fails

---

## 10. Mako Integration

### The Bridge: raft_main_helper

`raft_main_helper.{h,cc}` bridges Mako's transaction layer and Raft:

```cpp
// Global state
vector<shared_ptr<RaftWorker>> raft_workers_g;  // Process-local workers
map<int, callback> leader_replay_cb;            // Leader commit callbacks
map<int, callback> follower_replay_cb;          // Follower replay callbacks
```

### Key API Functions

| Function | Purpose |
|----------|---------|
| `setup(argc, argv)` | Initialize Raft workers from YAML config |
| `add_log_to_nc(log, len, par_id, batch, hint_out)` | Submit transaction log to local Raft. Returns `bool` (false = not leader). Optional `hint_out` receives the last known leader's site_id for redirection. |
| `RaftServer::GetLeaderHint()` | Returns the last known leader's site_id (`INVALID_SITEID` if unknown). Tracked via AppendEntries and InstallSnapshot RPCs. |
| `register_for_leader_par_id_return(cb)` | Register callback for leader log application |
| `register_for_follower_par_id_return(cb)` | Register callback for follower log replay |
| `get_outstanding_logs(par_id)` | Query uncommitted log count |
| `shutdown_paxos()` | Graceful shutdown (name is historical) |

### Log Flow: Transaction to Commitment

```
Step 1: Mako worker serializes transaction
  Transaction::serialize_util() -> add_log_to_nc(bytes, len, partition_id, batch)

Step 2: raft_main_helper routes to local RaftWorker
  find_worker(par_id) -> enqueue_to_worker(worker, log, ...)

Step 3: RaftWorker submits to RaftServer
  RaftServer::Start(cmd, &index, &term)
  -> Appends to raft_logs_[++lastLogIndex]
  -> Signals ready_for_replication_ event

Step 4: Leader replicates via HeartbeatLoop
  SendAppendEntries2() to all followers (parallel)

Step 5: Followers receive and apply
  OnAppendEntries() -> append to log -> applyLogs()
  -> app_next_(slot, cmd) -> follower_replay_cb

Step 6: Leader commits after quorum
  commitIndex advances -> applyLogs()
  -> app_next_(slot, cmd) -> leader_replay_cb

Step 7: Mako receives committed transaction
  Callback decodes bytes -> update watermark -> notify client
```

### Watermark Integration

Mako's speculative execution uses watermarks (not built into Raft):

- **Leader callback**: Encodes `watermark = timestamp * 10 + leader_status`
- **Follower callback**: Checks watermark; if not leader, queues for deferred replay
- **Safety check**: `sync_logger::safety_check(timestamp, watermark)` before follower replay

### Non-Leader Log Rejection

When `add_log_to_nc()` is called on a non-leader node, the call returns `false` and provides a leader hint:

```cpp
uint16_t leader_hint = 0;
bool ok = add_log_to_nc(log, len, par_id, batch, &leader_hint);
if (!ok) {
    // leader_hint contains the last known leader's site_id (or INVALID_SITEID)
    // Caller can redirect to the correct leader or abort gracefully
}
```

This differs from Paxos (where workers only run on the leader node). The preferred leader system ensures stable leadership, and the leader hint enables callers to redirect transactions when needed.

---

## 11. Log Persistence

### Configuration

```bash
MAKO_RAFT_PERSISTENCE=1              # Enable disk persistence
MAKO_RAFT_ASYNC_PERSISTENCE=1        # Async mode (persist in background)
MAKO_RAFT_PERSISTENCE_PATH=/var/raft # Storage path (default: /tmp)
```

### What is Persisted

| State | Criticality | When |
|-------|-------------|------|
| `currentTerm` | CRITICAL | On every term change |
| `vote_for_` | CRITICAL | Before responding to RequestVote |
| Log entries | CRITICAL | After appending (sync or async) |
| `commitIndex` | HIGH | After advancing |
| `specCommitIndex_` | HIGH | After speculative commit index advances, on leadership transitions |
| `securedLogIndex_` | HIGH | After secured log index advances, on leadership transitions |

### Metadata Keys

| Key | Constant | Value |
|-----|----------|-------|
| `"currentTerm"` | `META_TERM` | Current term number |
| `"vote_for"` | `META_VOTE_FOR` | Voted-for candidate (site ID) |
| `"commitIndex"` | `META_COMMIT_INDEX` | Highest committed log index |
| `"specCommitIndex"` | `META_SPEC_COMMIT_INDEX` | Highest speculatively committed log index |
| `"securedLogIndex"` | `META_SECURED_LOG_INDEX` | Highest durably committed log index |

### Integration Points

1. **OnRequestVote**: `PersistTermAndVote()` before responding
2. **OnAppendEntries**: `PersistLogEntries()` after appending
3. **SetLocalAppend**: `PersistLogEntry()` after leader appends
4. **Constructor**: `RecoverFromStorage()` on restart
5. **specCommitIndex advancement**: `PersistSpeculativeIndicesToLogStorage()` after memory ack quorum
6. **securedLogIndex advancement**: `PersistSpeculativeIndicesToLogStorage()` after durable ack quorum
7. **ResetSpeculativeState**: `PersistSpeculativeIndicesToLogStorage()` on leadership transitions
8. **PersistCommitIndexToLogStorage**: Also persists speculative indices alongside commitIndex

### Recovery Invariant

On recovery, the system enforces: `securedLogIndex_ <= specCommitIndex_ <= lastLogIndex`. If persisted values violate this (e.g., due to log truncation), they are clamped to valid values with a warning log.

### Async Persistence

In async mode, entries are persisted in a background thread:

```cpp
async_threads_.push_back({
    std::thread([this, entries]() {
        log_storage_->put(entries);   // fsync to disk
        SendDurableAck();             // Notify leader
        completion_flag->store(true); // Signal done
    }),
    completion_flag
});
```

The destructor joins all async threads to ensure clean shutdown.

---

## 12. Snapshotting and Log Compaction

Raft logs grow unboundedly without compaction. Snapshotting captures the state machine at a given log index/term, then discards log entries before that point.

### Configuration

```bash
MAKO_RAFT_SNAPSHOTS=1                # Enable snapshot support
MAKO_RAFT_SNAPSHOT_PATH=/var/raft    # Custom snapshot storage path
MAKO_RAFT_SNAPSHOT_INTERVAL=10000    # Entries between snapshots (default: 10000)
```

### Storage Architecture

Three layers in `src/rrr/rpc/`:

| Layer | File | Purpose |
|-------|------|---------|
| `SnapshotManager` | `snapshot_manager.hpp` | Abstract interface for snapshot CRUD |
| `FileSnapshotManager` | `file_snapshot_manager.hpp` | File-based implementation with retention policy |
| `SnapshotFormat` | `snapshot_format.hpp` | Binary serialization with CRC32 checksums |

### Binary Wire Format

```
Magic (4B) | Version (4B) | Header Size (4B) | Data Size (8B) |
Compression (1B) | Checksum Type (1B) | Last Index (8B) | Last Term (8B) |
Timestamp (8B) | Header CRC (4B) | Padding (2B) | Data... | Data CRC (4B)
```

- **Magic**: `0x504E4153` ("SNAP" in little-endian)
- **CRC32 checksums** on both header and data for corruption detection
- **52-byte fixed header** (8-byte aligned)
- File naming: `snapshot_<index>_<term>.snap` with `.tmp` suffix during writes (atomic rename on finalize)

### RaftServer Integration

The `snapshot_manager_` field in `RaftServer` is initialized during `Setup()` when `MAKO_RAFT_SNAPSHOTS=1`:

```cpp
void SetSnapshotManager(shared_ptr<SnapshotManager> manager);
shared_ptr<SnapshotManager> GetSnapshotManager() const;
bool HasSnapshot() const;          // Check if any snapshot exists
uint64_t GetSnapshotIndex() const; // Last snapshotted log index (snapidx_)
uint64_t GetSnapshotTerm() const;  // Term of last snapshotted entry (snapterm_)
size_t CompactLog(uint64_t up_to_index); // Discard entries before index
```

On initialization, if a prior snapshot exists on disk, `snapidx_` and `snapterm_` are loaded from it. These fields are used by `RequestVote` and `AppendEntries` for log consistency checks.

### Snapshot Recovery on Startup

After loading snapshot metadata, `InitializeSnapshotManager()` advances the server's state to reflect the snapshot. Since `InitializeSnapshotManager()` runs after `RecoverFromStorage()` in `Setup()`, log-recovered values may already be set. The recovery logic only advances values (using `>` checks), never goes backwards:

- `executeIndex` is set to `max(executeIndex, snapidx_)` -- the snapshot represents already-applied state
- `commitIndex` is set to `max(commitIndex, snapidx_)` and persisted via `PersistCommitIndexToLogStorage()`
- `lastLogIndex` is set to `max(lastLogIndex, snapidx_)` -- entries up to the snapshot are implicitly part of the log
- `min_active_slot_` is set to `max(min_active_slot_, snapidx_ + 1)` -- entries at or before the snapshot index have been compacted

This ensures a server that restarts with a snapshot but no log entries does not start with `executeIndex=0` and `commitIndex=0`, which would cause it to re-request already-applied entries.

### CreateSnapshot

`CreateSnapshot()` is called automatically from `applyLogs()` when `executeIndex - snapidx_ > snapshot_threshold_`. It serializes the state machine data, persists via `snapshot_manager_->TakeSnapshot()`, updates `snapidx_`/`snapterm_`, and calls `CompactLog()` to discard old entries and advance `min_active_slot_`. The threshold is configurable via `MAKO_RAFT_SNAPSHOT_INTERVAL` env var or `SetSnapshotThreshold()`.

**State machine snapshot hooks**: If `create_sm_snapshot_cb_` is registered (e.g., by `ReplicatedDB`), `CreateSnapshot()` calls it to produce the snapshot data instead of the default 16-byte placeholder (executeIndex + term). Similarly, `OnInstallSnapshot()` calls `load_sm_snapshot_cb_` (if set) to load the received snapshot data into the state machine.

```cpp
// RaftServer callback registration
void SetStateMachineSnapshotCallbacks(
    std::function<std::string()> create_cb,
    std::function<void(const std::string&)> load_cb);
```

**ReplicatedDB integration**: `ReplicatedDB` registers these callbacks in its constructor. `CreateStateMachineSnapshot()` uses `rocksdb_checkpoint_create()` to produce a consistent checkpoint, serializes all files into a binary blob (format: `num_files(4) + [name_len(4) + name + file_size(8) + file_data]*`), and cleans up the temporary checkpoint directory. `LoadStateMachineSnapshot()` deserializes the blob, closes the current RocksDB, destroys the old data directory, writes the checkpoint files, reopens the database, and reloads `last_applied_index_` from the snapshot's metadata.

**Startup wiring**: When the `MAKO_REPLICATED_DB=1` environment variable is set, `RaftServer::Setup()` automatically creates a `ReplicatedDB` instance after `InitializeSnapshotManager()` completes. It registers the `ApplyEntry` method as the `app_next_` callback via `RegLearnerAction`, so committed Raft entries are applied to local RocksDB. The DB path defaults to `/tmp/mako_replicated_db_<site_id>` but can be overridden with `MAKO_REPLICATED_DB_PATH`. The instance is accessible via `GetReplicatedDB()`. Initialization order: `RecoverFromStorage()` -> `InitializeSnapshotManager()` -> ReplicatedDB creation -> membership config -> heartbeat loops.

### InstallSnapshot RPC

When a follower is too far behind (its `next_index_` points to compacted log entries), the leader sends the full snapshot via `InstallSnapshot` RPC instead of `AppendEntries`.

**Follower handler** (`OnInstallSnapshot()` in `server.cc`):
1. Rejects if leader term < currentTerm (stale leader)
2. Updates term and steps down if leader has higher term
3. Resets election timer
4. Saves snapshot via `snapshot_manager_->TakeSnapshot()`
5. Updates `snapidx_`/`snapterm_`, discards old log entries
6. Advances `commitIndex`/`executeIndex`/`lastLogIndex`

**Leader sender** (`SendInstallSnapshot()` in `commo.cc`):
- Sends full snapshot in one RPC (no chunking)
- Callback receives follower's current term

### HeartbeatLoop Integration

The leader's `HeartbeatLoop()` detects when a follower has fallen too far behind and automatically sends `InstallSnapshot` instead of `AppendEntries`. This happens in PHASE 1 of the heartbeat loop, during the per-follower iteration:

1. For each follower, the leader checks if `next_index_[follower] < min_active_slot_` and `snapshot_manager_` is set
2. If so, loads the latest snapshot via `snapshot_manager_->LoadLatestSnapshot()`
3. Sends `InstallSnapshot` RPC via `commo()->SendInstallSnapshot()`
4. The callback handles three cases:
   - **Follower has higher term**: leader steps down (same as AppendEntries rejection)
   - **Term changed since send**: stale response, ignored
   - **Success**: updates `next_index_[follower] = snap_index + 1` and `match_index_[follower] = snap_index`
5. The normal `AppendEntries` path is skipped for this follower (via `continue`)

After the follower installs the snapshot and responds, subsequent heartbeats resume normal `AppendEntries` replication for any entries after the snapshot index.

### Planned Features

- ~~**Recovery**: On startup, load snapshot state before replaying log entries~~ (Implemented: see "Snapshot Recovery on Startup" above)
- ~~**State machine snapshots**: Hook CreateSnapshot/InstallSnapshot into ReplicatedDB for real RocksDB checkpoint-based snapshots~~ (Implemented: see "State machine snapshot hooks" above)
- **Snapshot compression**: Add configurable compression (Snappy/LZ4) to reduce snapshot transfer size
- **Streaming InstallSnapshot**: Chunk large snapshots instead of sending in a single RPC

See `docs/dev/raft_snapshot_design.md` for the full design document.

---

## 13. RPC Communication Layer

### RaftCommo

`RaftCommo` handles all inter-replica communication:

| Method | Purpose |
|--------|---------|
| `SendAppendEntries2()` | Async log replication to a follower |
| `BroadcastVote()` | Parallel RequestVote to all replicas |
| `SendVoteDurable()` | Notify leader of persisted vote |
| `SendAppendEntriesDurable()` | Notify leader of persisted entries |
| `SendInstallSnapshot()` | Send full snapshot to lagging follower |
| `SendTimeoutNow()` | Trigger immediate election on target |
| `SendNotifyRestart()` | Notify peers of server restart |
| `RetryPendingNotifyRestart()` | Retry notifications to PENDING peers |

### Restart Notification

When a server restarts, it broadcasts a restart notification to all peers. Peers reconnect their RPC proxies to avoid stale connections:

```cpp
enum class NotifyRestartStatus {
    ACKNOWLEDGED,   // Peer reconnected
    DOWN,           // Peer is down (skip retry)
    PENDING         // Need to retry
};
```

PENDING notifications are retried every few heartbeat cycles.

### Proxy Management

RaftCommo maintains `rpc_par_proxies_[partition_id][replica_id]` for each peer. During tests, proxies can be swapped with backup maps to simulate network partitions.

---

## 14. Configuration

### YAML Configuration

**Production Mako Raft config** (`config/raft.yml`):
```yaml
mode:
  ab: raft       # Atomic broadcast: raft
  batch: false
  retry: 20
```

Mako executes transactions through mbta/STO. This fragment selects only its
replication backend; DepTran transaction-protocol selectors are unsupported.

The former Rule concurrency-control mode and its Jetpack configuration are
retired. Generic Jetpack recovery machinery remains legacy code under a
separate audit and is not a supported configuration.

**Cluster topology** (separate file):
```yaml
site:
  server:
    - ["s101:9000", "s102:9001", "s103:9002"]

process:
  s101: localhost
  s102: p1
  s103: p2

host:
  localhost: 127.0.0.1
  p1: 10.0.1.100
  p2: 10.0.2.100
```

### Available Configurations

| File | Purpose |
|------|---------|
| `raft.yml` | Production Mako Raft replication mode |
| `raft_lab_test.yml` | Five-server Raft lab harness |

### Per-Shard Configs

For multi-shard deployments:
- `config/1leader_2followers/raft2_shardidx0.yml` (2 replicas, shard 0)
- `config/1leader_2followers/raft6_shardidx0.yml` (6 replicas, shard 0)

### Environment Variable Overrides

| Variable | Default | Description |
|----------|---------|-------------|
| `MAKO_RAFT_HEARTBEAT_INTERVAL_US` | `5000` (prod) / `100000` (test) | Heartbeat interval in microseconds. Overrides the compile-time default at `Setup()` time. Also controls the election-timeout sleep range (`2x`--`4x` the interval). |
| `MAKO_RAFT_PERSISTENCE` | (unset) | Set to `1` or `true` to enable log persistence |
| `MAKO_RAFT_ASYNC_PERSISTENCE` | (unset) | Set to `1` or `true` for async disk persistence |
| `MAKO_RAFT_PERSISTENCE_PATH` | `/tmp` | Base directory for persistence files |
| `MAKO_RAFT_SNAPSHOTS` | (unset) | Set to `1` to enable snapshot manager |
| `MAKO_RAFT_SNAPSHOT_PATH` | `/tmp` | Base directory for snapshot files |
| `MAKO_RAFT_LOG_RETENTION_WINDOW` | `5000` | Number of log entries to retain after compaction. Compaction is also coordinated with snapshots: entries beyond the latest snapshot index are never removed. |

The heartbeat interval and log retention window can also be changed at runtime via the C++ API:
```cpp
server->SetHeartbeatInterval(10000);  // 10ms
uint64_t current = server->GetHeartbeatInterval();

server->SetLogRetentionWindow(2000);  // keep 2000 entries
uint64_t window = server->GetLogRetentionWindow();
```

---

## 15. Build System

### Build Targets

| Command | Description | Binary |
|---------|-------------|--------|
| `make -j32` | Default (Paxos) | `dbtest` |
| `make mako-raft -j64` | Mako with Raft | `dbtest` + Raft test binaries |
| `make raft-test -j32` | Raft lab tests only | `deptran_server` with test fibers |

### CMake Flags

| Flag | Default | Effect |
|------|---------|--------|
| `MAKO_USE_RAFT` | OFF | Use `raft_main_helper.cc`, build Raft executables |
| `RAFT_TEST` | OFF | Enable `RAFT_TEST_CORO=1` for lab test fibers |

### Output Binaries

| Binary | Purpose |
|--------|---------|
| `build/dbtest` | Main Mako binary with Raft replication |
| `build/deptran_server` | Raft lab harness (`make raft-test` only) |
| `build/simpleRaft` | Simple Raft replication test |
| `build/simpleTransactionRepRaft` | Transaction replication test |
| `build/testPreferredReplicaStartup` | Preferred leader startup test |
| `build/testPreferredReplicaLogReplication` | Log replication test |
| `build/testNoOps` | NO-OP and watermark test |

**Warning**: `make raft-test` enables special fibers for the lab harness. Normal configs (`1c1s3r1p.yml`, `12c1s3r1p.yml`) will **not work** with this build.

---

## 16. Testing

### CI Tests

```bash
# Build
make mako-raft -j64

# Run all Raft CI tests
./ci/ci_mako_raft.sh all

# Individual tests
./ci/ci_mako_raft.sh simpleRaft                  # Basic 3-node replication
./ci/ci_mako_raft.sh shard1ReplicationRaft        # 1-shard TPC-C
./ci/ci_mako_raft.sh shard2ReplicationRaft        # 2-shard TPC-C
./ci/ci_mako_raft.sh shard1ReplicationSimpleRaft  # Data integrity
./ci/ci_mako_raft.sh shard2ReplicationSimpleRaft  # Data integrity
./ci/ci_mako_raft.sh cleanup                      # Kill processes
```

### Lab Test Framework

The lab test infrastructure (`test.h`, `testconf.h`) provides fine-grained control:

**Server Management:**
```cpp
OneLeader()           // Wait for single leader election
NCommitted(index)     // Count servers that committed at index
DoAgreement(cmd, n)   // Submit and wait for consensus
Kill(server_id)       // Destroy server
Restart(server_id)    // Recreate from persistent state
Disconnect(server_id) // Network partition
Reconnect(server_id)  // Restore connectivity
```

**Test Categories:**

| Category | Tests |
|----------|-------|
| Basic consensus | `testInitialElection`, `testReElection`, `testBasicAgree`, `testFailAgree` |
| Persistence | `testPersistence`, `testLeaderFollowerPersistence`, `testComprehensiveCrashRecovery` |
| Partitions | `testPartitionPlusRestart`, `testRejoin`, `testUnreliableAgree` |
| Speculative | `testSpeculativeLeaderElection`, `testSpecCommitIndexAdvances`, `testSpeculativeInvariantsHold` |
| Preferred leader | `testPreferredReplicaStartup`, `testPreferredReplicaLogReplication` |

### Speculative State Queries (for tests)

```cpp
bool IsSecuredLeader(siteid_t svr);
uint64_t GetSpecCommitIndex(siteid_t svr);
uint64_t GetSecuredLogIndex(siteid_t svr);
size_t GetSpecVotersCount(siteid_t svr);
size_t GetDurableVotersCount(siteid_t svr);
bool VerifySpecInvariants(siteid_t svr);
```

### Running the Raft Lab Harness (without Mako)

```bash
make raft-test -j32
./build/deptran_server -f config/raft_lab_test.yml
```

Regular Mako-Raft builds intentionally do not contain `deptran_server`.
The lab harness is server-only and rejects configurations with client sites.

---

## 17. Failure Scenarios and Recovery

### Leader Crash During Replication

```
Before: Leader appended entry 5 to own log, sent to followers 1,3 (not 2,4)
Crash:  Leader dies before quorum
Result: Entry 5 was never committed (never reached quorum)
        New leader elected from {1,2,3,4}
        If winner has entry 5: replicates to others
        If winner doesn't: entry 5 is lost (safe — never committed)
```

### Leader Crash After Quorum Commit

```
Before: Leader committed entry 5 (replicated to quorum)
Crash:  Leader dies
Result: Entry 5 is committed and will survive
        New leader must have entry 5 (log completeness guarantee)
        Followers already applied it
```

### Follower Crash Before Persistence

```
Before: Follower received entry 5 in memory (not yet fsynced)
Crash:  Follower dies
Restart: Loads last persisted state (without entry 5)
Result: Leader retries AppendEntries, follower gets entry 5 again
        No data loss
```

### Network Partition During Speculative Commit

```
Before: Leader reached memory quorum for entry 5 (specCommitIndex_=5)
        Disk quorum only for entries 1-3 (securedLogIndex_=3)
Partition: Leader isolated from majority

Leader side: Loses quorum -> steps down -> ROLLEDBACK for entries 4-5
Majority side: Elects new leader (may or may not have entry 5)

Result: Safe — entry 5 only discarded if leader was isolated
        If new leader has entry 5, it becomes committed
```

---

## 18. Known Issues and Workarounds

### Non-Leader Log Rejection (Resolved)

**Previously**: `add_log_to_nc()` silently dropped logs on non-leader nodes.

**Fix**: `add_log_to_nc()` now returns `false` when called on a non-leader, and provides a leader hint (the last known leader's site_id) via an optional output parameter. `RaftServer::GetLeaderHint()` tracks the leader identity from AppendEntries and InstallSnapshot RPCs.

**Impact**: Callers can now detect non-leader submission and redirect transactions to the correct leader. Combined with the preferred leader system, this provides a complete solution for leader routing.

### Leader Shutdown Hang

**Issue**: Leader process may hang during shutdown due to a race between poll thread drain and `Server::~Server()`.

**Root cause**: `sconns_ctr_` never reaches 0 because the poll thread exits before processing all `CmdRemovePollable` commands.

**Workaround**: Tests verify replication based on follower results (which exit cleanly). Leader hang is tolerated.

### Follower Replication Bottleneck

**Issue**: `applyLogs()` has a reentrancy guard (`in_applying_logs_`) that can cause work to be dropped when AppendEntries arrives during log application.

**Impact**: Under heavy load, followers may fall behind and only apply 1-30% of expected entries.

**Workaround**: The preferred solutions are:
1. Track `needs_reapply_` flag and loop until no more work
2. Use a dedicated async apply thread with a condition variable

---

## 19. Debugging

### Log Prefixes

| Prefix | Content |
|--------|---------|
| `[RAFT_ELECTION]` | Leader election events |
| `[APPEND_RPC]` | AppendEntries sends/receives |
| `[COMMIT-CALC]` | Leader's commit index calculation |
| `[APPLY-LOGS]` | Log application and execution |
| `[SPEC-RAFT]` | Speculative voting/commit events |
| `[TIMER_RESET]` | Election timeout resets |
| `[RAFT-PERSISTENCE]` | Persistence operations |
| `[RAFT-SNAPSHOT]` | Snapshot operations |
| `[SPEC-INVARIANTS]` | Invariant violations |
| `[RAFT-ADD-LOG]` | Log submission to Raft |

### Environment Variables

```bash
MAKO_RAFT_PERSISTENCE=1          # Enable persistence
MAKO_RAFT_ASYNC_PERSISTENCE=1    # Async persistence
MAKO_RAFT_PERSISTENCE_PATH=/tmp  # Storage path
MAKO_RAFT_SNAPSHOTS=1            # Enable snapshots
MAKO_RAFT_SNAPSHOT_PATH=/tmp     # Snapshot storage path
MAKO_RAFT_SNAPSHOT_INTERVAL=10000 # Entries between snapshots
MAKO_DISABLE_JETPACK=1           # Keep legacy Jetpack recovery disabled
```

There is no supported setting that enables the legacy recovery subsystem while
its separate audit is pending.

### Checking Test Results

```bash
# For simple test, check follower callbacks
grep "RESULTS.*follower_callbacks=" raft_a2.log raft_a3.log

# For TPC-C test, check throughput
grep "agg_persist_throughput" shard0-localhost-*.log

# Check follower replay batches
grep "replay_batch:" shard0-p1-*.log

# Check for leadership churn
grep "RAFT_ELECTION" *.log | wc -l
```

### Stress Testing

```bash
# Unreliable network (in test framework)
config->SetUnreliable();  # 1/10 chance of packet loss

# Kill and restart servers
config->Kill(server_id);
config->Restart(server_id);

# Network partitions
config->Disconnect(server_id);
// ... do work ...
config->Reconnect(server_id);
```

---

## 20. Membership Changes

The Raft implementation supports single-server membership changes via `AddServer` and `RemoveServer` RPCs. The protocol ensures safety by allowing only one server to change at a time, maintaining quorum overlap between old and new configurations.

### Configuration Tracking

`RaftServer` maintains a dynamic membership configuration:

```cpp
std::set<siteid_t> current_config_;      // Active replica set
bool config_change_pending_ = false;     // True when a config entry is in-flight
uint64_t pending_config_index_ = 0;      // Log index of pending config entry
std::set<siteid_t> learners_;            // Servers being caught up (not yet in quorum)
uint64_t catchup_threshold_ = 100;       // Entries within lastLogIndex to consider "caught up"
```

`current_config_` is initialized from `Config::SitesByPartitionId()` during `Setup()`. All quorum calculations use `GetQuorumSize()` (which returns `current_config_.size() / 2 + 1`) instead of the static `Config::GetConfig()->GetPartitionSize()`.

### RPCs

Both RPCs are leader-only. Non-leaders return `success=false` with `leader_hint` set to the last known leader's site ID.

**AddServer(term, new_server_id, new_server_addr)**: Adds a server to the configuration. The server is first added as a **learner** (receives log entries but does not count towards quorum). Once caught up, it is automatically promoted to a full member. Rejects if:
- This server is not the leader
- A config change is already pending (`config_change_pending_ == true`)
- The server is already in `current_config_`
- The server is already a learner (`learners_`)

**RemoveServer(term, server_id)**: Removes a server from the configuration. Rejects if:
- This server is not the leader
- A config change is already pending
- The server is not in `current_config_`
- Removing would leave zero servers (minimum cluster size is 1)

### New Server Catch-Up (Learner Protocol)

When `AddServer` is called, the new server is added as a **learner** rather than immediately joining the quorum. This prevents a slow/empty server from blocking commits.

**Lifecycle:**
1. `OnAddServer` inserts the server into `learners_` (not `current_config_`).
2. The server is added to `next_index_` and `match_index_` so `HeartbeatLoop` sends it `AppendEntries` RPCs.
3. Learners are **excluded** from the commit index median calculation -- they don't count towards quorum.
4. Each heartbeat round, `CheckAndPromoteLearners()` checks if any learner's `match_index_` is within `catchup_threshold_` of the leader's `lastLogIndex`.
5. When caught up, `PromoteLearner()` moves the server from `learners_` to `current_config_` and clears `config_change_pending_`.

**Key properties:**
- Learners receive the same replication traffic as full members (via `HeartbeatLoop`).
- The quorum size (`GetQuorumSize()`) is based only on `current_config_`, not `learners_`.
- The `catchup_threshold_` (default 100 entries) is configurable per server.
- `IsLearner(id)` and `GetLearners()` provide read-only access to learner state.

### Current Limitations

The current implementation applies configuration changes directly in memory rather than through Raft log entries. This means:
- Config changes are not replicated to followers
- Config changes do not survive leader failure

See `docs/dev/raft_membership_change_design.md` for the full protocol design.

### Tests

- **Test 73** (`testAddServerBasic`): Verifies initial config size, adds a server, checks config grows and quorum updates.
- **Test 74** (`testRemoveServerBasic`): Adds then removes a server, verifies config shrinks and quorum updates.
- **Test 75** (`testRejectDuplicateConfigChange`): Verifies `config_change_pending_` blocks concurrent changes, and non-leaders reject config change requests.
- **Test 76** (`testNewServerCatchUp`): Verifies the learner catch-up lifecycle -- server added as learner (not in quorum), not promoted when far behind, promoted when within threshold, quorum updates correctly, threshold boundary behavior.

---

*This document consolidates the Raft implementation documentation from across the Mako project. For detailed phase implementation plans, see `docs/migration/raft/` and `docs/plans/log-persistence/`. For the Mako integration layer, see `docs/migration/raft/architecture-analysis.md` and `docs/migration/raft/mako-explained.md`.*
