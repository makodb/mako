# Preferred Leader Election — Implementation Details

## What This Document Covers

This document provides a method-by-method walkthrough of every function that implements the preferred leader system. It covers member variables, inline helpers, the dynamic election timeout, the monitoring thread, the transfer decision logic, the piggybacked transfer protocol, the `OnTimeoutNow` RPC handler, the destruction sequence, and integration points with the rest of the Raft server.

**Key source files**:
- `src/deptran/raft/server.h` — Member variables (lines 113-132), inline helpers (lines 135-146), `SetPreferredLeader()` (lines 587-604), `GetPreferredLeader()` (lines 610-612)
- `src/deptran/raft/server.cc` — `GetElectionTimeout()` (lines 348-369), `Setup()` (lines 372-374), `setIsLeader()` (lines 443-554), `HeartbeatLoop()` comment (lines 949-960), `~RaftServer()` (lines 963-984), `StartElectionTimer()` (lines 1188-1228), `OnAppendEntries()` trigger_election_now (lines 1428-1462), `OnTimeoutNow()` (lines 1476-1575), `StopLeadershipTransferMonitoring()` (lines 1577-1586), `StartLeadershipTransferMonitoring()` (lines 1588-1671), `ShouldTransferLeadership()` (lines 1681-1725), `InitiateLeadershipTransfer()` (lines 1727-1827)
- `src/deptran/raft/commo.cc` — `SendAppendEntries()` with `trigger_election_now` (lines 96-175), `SendTimeoutNow()` (lines 228-285)
- `src/deptran/raft_main_helper.cc` — Startup configuration (lines 300-349), `set_preferred_leader()` runtime API (lines 650-679)

---

## 1. Member Variables

**Location**: `server.h:113-132`

```cpp
// Preferred Replica System state
siteid_t preferred_leader_site_id_ = INVALID_SITEID;     // (siteid_t)-1 means disabled
uint64_t leader_last_commit_index_ = 0;                   // Leader's commitIndex (from heartbeats)
bool transferring_leadership_ = false;                    // True during active transfer
uint64_t leadership_transfer_start_time_ = 0;             // Transfer start timestamp (microseconds)
std::atomic<bool> leadership_monitor_stop_{false};        // Signal to stop monitor thread
std::thread leadership_monitor_thread_;                   // Background OS thread (not a fiber)
uint64_t startup_timestamp_ = 0;                          // Server start time for grace period
```

### Design Rationale

| Variable | Why It Exists |
|----------|---------------|
| `preferred_leader_site_id_` | Central configuration — every decision branches on this |
| `leader_last_commit_index_` | Allows followers to know when they've caught up via `HaveCaughtUp()` |
| `transferring_leadership_` | Prevents double-transfer and suppresses elections during transfer |
| `leadership_transfer_start_time_` | Could be used for transfer timeout (not currently enforced) |
| `leadership_monitor_stop_` | Thread-safe signal (atomic) for clean monitor shutdown |
| `leadership_monitor_thread_` | OS thread (not fiber) because the monitor must survive reactor loop changes |
| `startup_timestamp_` | Enables the 5-second grace period for startup election bias |

---

## 2. Inline Helper Methods

### `AmIPreferredLeader()` (`server.h:136-138`)

```cpp
bool AmIPreferredLeader() const {
    return preferred_leader_site_id_ != INVALID_SITEID &&
           site_id_ == preferred_leader_site_id_;
}
```

**Logic**: Returns `true` only if (a) a preferred leader is configured AND (b) this replica's `site_id_` matches. Both conditions are necessary — when `preferred_leader_site_id_ == INVALID_SITEID`, the system operates as standard Raft.

**Called by**: `GetElectionTimeout()`, `setIsLeader()`, `SetPreferredLeader()`, `ShouldTransferLeadership()`, `StartLeadershipTransferMonitoring()`, `OnAppendEntries()` (trigger_election_now handling).

### `HaveCaughtUp()` (`server.h:142-145`)

```cpp
bool HaveCaughtUp() const {
    return commitIndex >= leader_last_commit_index_;
}
```

**Logic**: Compares this replica's `commitIndex` against the leader's last known `commitIndex` (received via AppendEntries heartbeats). Used by followers to know when they have all committed entries.

**Note**: `leader_last_commit_index_` is updated from the `leaderCommitIndex` field in AppendEntries RPCs. This variable is declared but not currently written to in the codebase — the actual catch-up check in `ShouldTransferLeadership()` uses the leader's own `match_index_` (which is more direct and authoritative).

---

## 3. `SetPreferredLeader()` — Configuration Entry Point

**Location**: `server.h:587-604`

```cpp
void SetPreferredLeader(siteid_t site_id) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    siteid_t old_preferred = preferred_leader_site_id_;
    preferred_leader_site_id_ = site_id;

    if (old_preferred != site_id) {
        Log_info("[LEADERSHIP-TRANSFER] Site %d: Preferred leader set to %d",
                 site_id_, site_id);
    }

    // If I'm a non-preferred leader, start monitoring for transfer opportunity
    if (!AmIPreferredLeader() && is_leader_ && looping_) {
        StartLeadershipTransferMonitoring();
    }
}
```

**Key behavior**:
1. Updates the preferred site ID under lock
2. If this replica is currently a leader AND is now non-preferred → immediately starts monitoring
3. Thread-safe: protected by `mtx_` (recursive mutex)

### Call Sites

| Caller | When | Context |
|--------|------|---------|
| `raft_main_helper.cc:340` | Startup | Sets localhost (locale_id==0) as preferred for each partition |
| `set_preferred_leader():666` | Runtime API | Allows dynamic reconfiguration from external code |

### Startup Configuration Flow (`raft_main_helper.cc:300-349`)

```
For each raft_worker in raft_workers_g:
    partition_id = raft_server->partition_id_
    partition_sites = config->SitesByPartitionId(partition_id)

    For each site in partition_sites:
        if site.locale_id == 0:   // localhost
            preferred_site_id = site.id
            break

    raft_server->SetPreferredLeader(preferred_site_id)
```

In a multi-partition deployment, each partition independently selects the localhost replica as its preferred leader.

---

## 4. `GetElectionTimeout()` — Asymmetric Timeouts

**Location**: `server.cc:348-369`

```cpp
uint64_t RaftServer::GetElectionTimeout() {
    uint64_t base_timeout;
    uint64_t current_time = Time::now();
    bool in_grace_period = (current_time - startup_timestamp_) < 5000000; // 5 seconds

    if (AmIPreferredLeader()) {
        base_timeout = 150000; // 150ms
        uint64_t jitter = RandomGenerator::rand(0, 150000);
        return base_timeout + jitter; // 150-300ms
    } else if (in_grace_period) {
        base_timeout = 1000000; // 1s
        uint64_t jitter = RandomGenerator::rand(0, 1000000);
        return base_timeout + jitter; // 1-2s
    } else {
        base_timeout = 500000; // 500ms
        uint64_t jitter = RandomGenerator::rand(0, 500000);
        return base_timeout + jitter; // 500ms-1s
    }
}
```

### Timeout Decision Table

| Condition | Base (us) | Jitter (us) | Total Range | Purpose |
|-----------|-----------|-------------|-------------|---------|
| Preferred replica | 150,000 | 0-150,000 | 150-300ms | Win elections quickly |
| Non-preferred, startup grace (<5s) | 1,000,000 | 0-1,000,000 | 1-2s | Let preferred win first |
| Non-preferred, after grace | 500,000 | 0-500,000 | 500ms-1s | Enable failover |

### Called by: `StartElectionTimer()` (`server.cc:1197`)

```cpp
void RaftServer::StartElectionTimer() {
    // ...
    while(!stop_) {
        uint64_t election_timeout = GetElectionTimeout();

        Fiber::sleep(RandomGenerator::rand(HEARTBEAT_INTERVAL * 2, HEARTBEAT_INTERVAL * 4));

        auto time_elapsed = Time::now() - last_heartbeat_time_;

        if (!IsLeader() && time_elapsed > election_timeout) {
            req_voting_ = true;
            if (stop_) return;
            RequestVote();
            while(req_voting_) {
                Fiber::sleep(wait_int_);
                if(stop_) return;
            }
        }
    }
}
```

The election timer wakes periodically and compares elapsed time since last heartbeat against the dynamic timeout. Because the timeout is recomputed each iteration via `GetElectionTimeout()`, the grace period naturally expires — after 5 seconds, non-preferred replicas use the shorter 500ms-1s timeout.

---

## 5. `setIsLeader()` — Transfer Integration Point

**Location**: `server.cc:443-554`

When a replica transitions to leader, `setIsLeader(true)` checks if it should start monitoring:

```
setIsLeader(isLeader):
    |
    +-- become_new_leader = isLeader && !was_leader
    +-- become_new_follower = !isLeader && was_leader
    |
    +-- IF isLeader:
    |     Reset match_index_ and next_index_ for all peers
    |
    +-- is_leader_ = isLeader
    |
    +-- IF become_new_leader:
    |     transferring_leadership_ = false   // Clear any stale transfer flag
    |     Update view (old_view_ → new_view_)
    |     Update communicator view
    |     IF !AmIPreferredLeader() && looping_:
    |         StartLeadershipTransferMonitoring()  // <<< KEY INTEGRATION
    |
    +-- IF become_new_follower:
          resetTimer("became follower")  // Prevent instant re-election
```

**Why check `looping_`?** The `looping_` flag indicates the HeartbeatLoop is running. During initialization (before `Setup()` is called), `looping_` is `false`. Starting the monitoring thread before the heartbeat loop would be premature.

**Why clear `transferring_leadership_`?** If this replica just won an election, any previous transfer (where this replica was giving away leadership) is now moot. Clearing the flag prevents stale state from blocking future transfers.

---

## 6. `StartLeadershipTransferMonitoring()` — Background Monitor

**Location**: `server.cc:1588-1671`

This method launches an OS thread (not a fiber) that periodically checks if the preferred replica is ready to become leader.

```cpp
void RaftServer::StartLeadershipTransferMonitoring() {
    // Reset stop flag if previously set
    if (leadership_monitor_stop_.load()) {
        leadership_monitor_stop_ = false;
    }

    // Stop any existing monitor thread
    if (leadership_monitor_thread_.joinable()) {
        leadership_monitor_stop_ = true;
        leadership_monitor_thread_.join();         // Wait for old thread to finish
        leadership_monitor_stop_ = false;
    }

    // Launch new monitoring thread
    leadership_monitor_thread_ = std::thread([this]() {
        const uint64_t CHECK_INTERVAL_MS = 1000;       // Check every 1 second
        const uint64_t MIN_STABLE_TIME_US = 500000;    // Wait 0.5 seconds after becoming leader

        uint64_t became_leader_time = Time::now();

        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(CHECK_INTERVAL_MS));

            bool should_transfer = false;
            {
                std::lock_guard<std::recursive_mutex> lock(mtx_);

                if (leadership_monitor_stop_) break;    // Shutdown requested
                if (stop_) break;                       // Server shutting down
                if (!is_leader_) break;                 // No longer leader
                if (AmIPreferredLeader()) break;        // Became preferred (dynamic reconfig)

                // Wait for stability
                uint64_t time_as_leader = Time::now() - became_leader_time;
                if (time_as_leader < MIN_STABLE_TIME_US) {
                    continue;                           // Too soon, check again next iteration
                }

                if (ShouldTransferLeadership()) {
                    should_transfer = true;
                }
            } // Lock released before calling InitiateLeadershipTransfer

            if (should_transfer) {
                InitiateLeadershipTransfer();
                break;  // Exit after transferring
            }
        }
    });
}
```

### Design Decisions

1. **OS thread vs. fiber**: Uses `std::thread` instead of `Fiber::create_run` because the monitor needs to survive reactor loop reconfigurations and must be stoppable via `join()`.

2. **Lock-then-release pattern**: The lock is acquired to check shared state, then released before calling `InitiateLeadershipTransfer()`. This prevents deadlock — `InitiateLeadershipTransfer()` acquires the same lock internally.

3. **Stability wait (500ms)**: After becoming leader, the system waits 500ms before considering transfer. This allows the new leader to establish itself, replicate entries, and update `match_index_` for followers.

4. **Exit conditions**: The thread exits on any of: stop requested, server shutting down, no longer leader, became preferred leader, or transfer completed.

---

## 7. `ShouldTransferLeadership()` — Decision Logic

**Location**: `server.cc:1681-1725`

```
ShouldTransferLeadership():
    |
    +-- [1] is_leader_?                              NO → return false
    +-- [2] AmIPreferredLeader()?                    YES → return false
    +-- [3] preferred_leader_site_id_ == INVALID?    YES → return false
    +-- [4] transferring_leadership_?                YES → return false
    +-- [5] preferred in match_index_?               NO → return false
    +-- [6] match_index_[preferred] >= commitIndex?  NO → return false
    |
    +-- All checks passed → return true
```

### Check Details

| # | Check | Why |
|---|-------|-----|
| 1 | Must be leader | Only leaders can initiate transfer |
| 2 | Must not be preferred | Preferred leaders don't transfer away |
| 3 | Must have preferred configured | No target = no transfer |
| 4 | Must not be already transferring | Prevents double-transfer |
| 5 | Preferred must be in peer list | Preferred must be a known replica in `match_index_` |
| 6 | Preferred must be caught up | `match_index_[preferred] >= commitIndex` ensures no data loss |

**Safety-critical check**: #6 is the key safety property. The `match_index_` for the preferred replica reflects the highest log index known to be replicated there. If `match_index_[preferred] >= commitIndex`, then all committed entries exist on the preferred replica, making it safe to become leader.

---

## 8. `InitiateLeadershipTransfer()` — The Transfer Protocol

**Location**: `server.cc:1727-1827`

This method executes the actual leadership transfer using the piggybacked approach.

### Step-by-Step Walkthrough

```
InitiateLeadershipTransfer():
    |
    [1] if (stop_) return;              // Abort if shutting down
    |
    [2] { LOCK mtx_
          target = preferred_leader_site_id_
          transferring_leadership_ = true
          leadership_transfer_start_time_ = Time::now()

          FOR each peer in match_index_:
              prevLogIndex = next_index_[peer] - 1
              prevLogTerm = logs_[prevLogIndex]->term

              commo()->SendAppendEntries(
                  peer, partition_id_, commitIndex, 0, true, site_id_,
                  currentTerm, prevLogIndex, prevLogTerm, commitIndex,
                  nullptr,  // Empty = heartbeat
                  0,
                  true      // trigger_election_now = true <<<
              )
        } // UNLOCK
    |
    [3] sleep(20ms)                     // Let packets transmit
    |
    [4] { LOCK mtx_
          setIsLeader(false)            // Step down
        } // UNLOCK
```

### Why Piggybacked Approach?

The transfer signal is sent as a regular `EmptyAppendEntries` with `trigger_election_now=true`. This was chosen over a separate `TimeoutNow` RPC for these reasons:

1. **Atomic notification**: All replicas receive the signal via the same heartbeat mechanism, ensuring synchronization.
2. **Timer reset**: The heartbeat resets non-preferred replicas' election timers, preventing them from starting elections while the preferred replica starts its election.
3. **No new RPC needed on the critical path**: Reuses existing infrastructure.

### What Happens on Each Replica

When a replica receives `EmptyAppendEntries` with `trigger_election_now=true` (`server.cc:1428-1462`):

**Preferred replica** (in `OnAppendEntries`):
```cpp
if (AmIPreferredLeader()) {
    if (!IsLeader()) {
        Fiber::create_run([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            if (stop_) return;
            RequestVote();
        });
    }
}
```

Waits 30ms (to let the old leader step down and heartbeats reach other replicas), then starts election. The 30ms wait prevents the preferred replica from starting an election while the old leader is still active.

**Non-preferred replica**:
```cpp
else {
    Log_info("[PIGGYBACKED-TRANSFER] Site %d (non-preferred): Received transfer signal");
}
```

Does nothing — its election timer was just reset by the heartbeat, so it won't start competing elections.

---

## 9. `OnTimeoutNow()` — Direct Transfer RPC Handler

**Location**: `server.cc:1476-1575`

This handler processes the standalone `TimeoutNow` RPC (as opposed to the piggybacked `trigger_election_now` flag). It's available as an alternative transfer mechanism.

### Edge Case Handling

```
OnTimeoutNow(leaderTerm, leaderSiteId, *followerTerm, *success, cb):
    |
    +-- *followerTerm = currentTerm
    +-- *success = false
    |
    +-- [Case 0] stop_?                  → return (shutting down)
    +-- [Case 1] leaderTerm < currentTerm? → return (stale)
    +-- [Case 1b] leaderTerm > currentTerm?
    |     → currentTerm = leaderTerm
    |     → vote_for_ = INVALID_SITEID
    |     → if is_leader_: setIsLeader(false)
    +-- [Case 2] is_leader_?             → success=true, return (already leader)
    +-- [Case 3] req_voting_?            → success=true, return (already electing)
    +-- [Case 4] transferring_leadership_? → return (in transfer)
    |
    +-- [Valid] RequestVote()
          → success = election_started
          → cb()
```

**Notable behaviors**:
- Cases 2 and 3 return `success=true` because the goal (becoming leader or attempting to) is already being achieved
- Case 1b updates the term before proceeding — the replica may need to step down from leadership if the leader's term is higher
- The mutex (`mtx_`) is held for the entire handler duration — this prevents concurrent state modifications

---

## 10. `StopLeadershipTransferMonitoring()` — Clean Shutdown

**Location**: `server.cc:1577-1586`

```cpp
void RaftServer::StopLeadershipTransferMonitoring() {
    leadership_monitor_stop_ = true;

    if (leadership_monitor_thread_.joinable()) {
        leadership_monitor_thread_.detach();  // Don't join — avoid deadlock
    }
}
```

**Why `detach()` instead of `join()`?** The destructor calls `StopLeadershipTransferMonitoring()`, and the monitor thread acquires `mtx_`. If the destructor held `mtx_` and tried to `join()`, deadlock would occur. By detaching, the thread is signaled to stop (via `leadership_monitor_stop_`) and can exit on its own schedule.

**Called by**: `~RaftServer()` (line 980) during server shutdown.

---

## 11. `~RaftServer()` — Destruction Sequence

**Location**: `server.cc:963-984`

```
~RaftServer():
    |
    [1] stop_ = true                           // Signal all coroutines to stop
    [2] looping_ = false                       // Stop HeartbeatLoop
    [3] ready_for_replication_->set(1)         // Wake HeartbeatLoop if sleeping
    [4] StopLeadershipTransferMonitoring()     // Signal and detach monitor thread
    [5] sleep(50ms)                            // Let detached coroutines see stop_=true
```

The 50ms sleep at step 5 is critical. Without it, detached coroutines (election timer, leadership transfer) might still be running when the vtable is destroyed, causing crashes when they call virtual methods like `RequestVote()`.

---

## 12. `HeartbeatLoop()` — Transfer Delegation

**Location**: `server.cc:949-960`

The HeartbeatLoop previously contained leadership transfer logic, which was removed and delegated to the monitoring thread:

```cpp
// ============================================================================
// LEADERSHIP TRANSFER: Handled by Monitor Thread
// ============================================================================
// Leadership transfer is now handled by StartLeadershipTransferMonitoring() thread,
// not here in HeartbeatLoop. This prevents race conditions and double-triggering.
// The monitor thread is started in setIsLeader() when becoming a non-preferred leader.
```

This design decision prevents a race condition where both HeartbeatLoop and the monitoring thread could independently decide to transfer, causing duplicate `InitiateLeadershipTransfer()` calls.

---

## 13. Integration Points Summary

The preferred leader system integrates with the following core Raft methods:

| Core Method | Integration |
|------------|-------------|
| `Setup()` | Records `startup_timestamp_` for grace period |
| `StartElectionTimer()` | Calls `GetElectionTimeout()` for dynamic timeout |
| `setIsLeader(true)` | Starts monitoring if non-preferred leader |
| `setIsLeader(false)` | Resets election timer (prevents instant re-election) |
| `HeartbeatLoop()` | Comment-only: documents transfer is handled by monitor thread |
| `OnAppendEntries()` | Handles `trigger_election_now` flag |
| `~RaftServer()` | Stops monitoring thread |

### Call Graph

```
SetPreferredLeader(site_id)
    |
    +-- [If non-preferred leader] --> StartLeadershipTransferMonitoring()

setIsLeader(true)
    |
    +-- [If non-preferred] --> StartLeadershipTransferMonitoring()

StartLeadershipTransferMonitoring()       [OS thread, loops every 1s]
    |
    +-- ShouldTransferLeadership()
    |     +-- match_index_[preferred] >= commitIndex?
    |
    +-- InitiateLeadershipTransfer()
          +-- SendAppendEntries(trigger_election_now=true) to ALL
          +-- sleep(20ms)
          +-- setIsLeader(false)

OnAppendEntries(trigger_election_now=true)  [On preferred replica]
    |
    +-- sleep(30ms)
    +-- RequestVote()                       [Starts election]
    +-- [Wins election] → setIsLeader(true) [Preferred is now leader]

~RaftServer()
    |
    +-- StopLeadershipTransferMonitoring()
```

---

## 14. Full Sequence Diagram — Leadership Transfer

```
Time  Non-Pref Leader (site 1)     Preferred Follower (site 0)     Other Follower (site 2)
─────────────────────────────────────────────────────────────────────────────────────────────
 T=0  MonitorThread: ShouldTransfer?
      YES: match[0]=5, commit=5
      |
 T=0  transferring_=true
      SendAppendEntries to site 0
        trigger_election_now=true
      SendAppendEntries to site 2  ─────────────────────────────────>  Receive heartbeat
        trigger_election_now=true    Receive heartbeat                  Reset election timer
      |                              Reset election timer               Log: "non-preferred,
      |                              AmIPreferred? YES                         do nothing"
      |                              Not leader → start timer
      |                              |
T=20  sleep(20ms)                    |
      setIsLeader(false)             |
      [Now follower]                 |
      resetTimer("became follower")  |
      |                              |
T=30  |                              sleep(30ms) done
      |                              RequestVote()
      |                              currentTerm++ (→ T+1)
      |                              BroadcastVote to sites 1,2
      |                              |
T=31  Receive RequestVote(T+1)       |                                 Receive RequestVote(T+1)
      T+1 > myTerm → update          |                                 T+1 > myTerm → update
      Log up-to-date? YES            |                                 Log up-to-date? YES
      Grant vote                     |                                 Grant vote
      |                              |
T=35  |                              Receive 2 YES votes
      |                              Quorum reached (2/2 needed)
      |                              setIsLeader(true)
      |                              AmIPreferred? YES → no monitor
      |                              Start sending heartbeats
      |                              |
T=40  Receive heartbeat              |                                 Receive heartbeat
      [Preferred is leader!]         [Leader, term T+1]                [Follower, term T+1]
```

---

## 15. Constants and Tuning Parameters

| Constant | Value | Location | Purpose |
|----------|-------|----------|---------|
| `INVALID_SITEID` | `(siteid_t)-1` | `server.h:16` | Sentinel for "no preferred leader" |
| Preferred election timeout | 150-300ms | `GetElectionTimeout()` | Win elections quickly |
| Grace period timeout | 1-2s | `GetElectionTimeout()` | Let preferred win at startup |
| Normal non-preferred timeout | 500ms-1s | `GetElectionTimeout()` | Failover detection |
| Grace period duration | 5 seconds | `GetElectionTimeout()` | Startup bias window |
| Monitor check interval | 1 second | `StartLeadershipTransferMonitoring()` | How often to check transfer conditions |
| Stability wait | 500ms | `StartLeadershipTransferMonitoring()` | Delay after becoming leader |
| Pre-stepdown sleep | 20ms | `InitiateLeadershipTransfer()` | Ensure packets sent |
| Preferred pre-election wait | 30ms | `OnAppendEntries()` trigger | Let old leader step down |
| Destructor sleep | 50ms | `~RaftServer()` | Let coroutines see stop_ |

---

## Related Documents

- [Design and Motivation](design.md) — Why preferred leader exists, safety argument
- [Testing](testing.md) — How preferred leader was tested
- [Server Implementation](../02-raft-core/server_implementation.md) — Full RaftServer walkthrough
- [Leader Election](../02-raft-core/leader_election.md) — Standard Raft election mechanism
- [RPC Layer](../02-raft-core/rpc_layer.md) — SendTimeoutNow and SendAppendEntries RPCs
