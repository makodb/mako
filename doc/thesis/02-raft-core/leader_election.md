# Leader Election Mechanism

## What This Document Covers

This document provides a complete walkthrough of the Raft leader election mechanism as implemented, including the election trigger, vote broadcasting, quorum detection, vote granting logic, split vote handling, and term advancement. It concludes with a full sequence diagram of a successful election cycle.

**Key source files**:
- `src/deptran/raft/server.cc` — `RequestVote()`, `StartElectionTimer()`, `OnRequestVote()`, `doVote()`
- `src/deptran/raft/commo.cc` — `BroadcastVote()`
- `src/deptran/raft/commo.h` — `RaftVoteQuorumEvent`
- `src/rrr/reactor/quorum_event.h` — `QuorumEvent` base class

---

## 1. Election Trigger

Elections are triggered by the election timer coroutine (`StartElectionTimer()`, `server.cc:1188-1228`). The coroutine runs continuously on every server and checks whether enough time has passed since the last heartbeat.

### Timer Loop

```
StartElectionTimer():
  resetTimer("start election timer")
  last_heartbeat_time_ = now()

  Fiber::create_run:
    WHILE !stop_:
      election_timeout = GetElectionTimeout()
      sleep(random(HEARTBEAT_INTERVAL*2, HEARTBEAT_INTERVAL*4))

      time_elapsed = now() - last_heartbeat_time_

      IF !IsLeader() AND time_elapsed > election_timeout:
        req_voting_ = true
        RequestVote()
        WHILE req_voting_: sleep(wait_int_)
```

The timer fires when:
1. This server is **not** the leader
2. `time_elapsed` exceeds the dynamic election timeout

The timeout is **not** fired if:
- The server received a valid `AppendEntries` from a current-term leader (which calls `resetTimer()`)
- The server granted a vote (which also calls `resetTimer()`)

### Dynamic Timeout Values

| Condition | Range | Purpose |
|-----------|-------|---------|
| Preferred leader | 150-300 ms | Win elections quickly after failure |
| Non-preferred (grace period, first 5s) | 1-2 s | Let preferred leader win startup election |
| Non-preferred (normal) | 500 ms - 1 s | Standard Raft randomized timeout |

The randomization prevents synchronized elections across servers.

---

## 2. The Election: RequestVote()

**Location**: `server.cc:995-1118`

When the timer fires, `RequestVote()` executes the candidate's side of the election:

### Step 1: Shutdown Guard (`server.cc:1000-1003`)

```cpp
if (stop_) {
    return false;  // Prevent crash during destructor teardown
}
```

This prevents calling virtual methods through a collapsed vtable if the destructor has already started.

### Step 2: Become Candidate (`server.cc:1016-1034`)

Under the mutex:
1. Increment `currentTerm`
2. Vote for self: `vote_for_ = site_id_`
3. Persist both to stable storage
4. Record last log index and term for the log up-to-date check

```cpp
currentTerm++;
vote_for_ = site_id_;
PersistTermAndVote();
```

### Step 3: Broadcast Vote RPCs (`server.cc:1041`)

```cpp
auto sp_quorum = ((RaftCommo*)(this->commo_))->BroadcastVote(
    par_id, lst_idx, lst_term, loc_id, term);
```

This sends `RequestVote` RPCs to all peers and returns a `RaftVoteQuorumEvent` for collecting responses.

### Step 4: Wait for Quorum (`server.cc:1042`)

```cpp
sp_quorum->wait(1000000);  // 1 second timeout
```

The wait returns when:
- A majority votes YES (`sp_quorum->yes()`)
- A majority votes NO (`sp_quorum->no()`)
- The 1-second timeout expires (`sp_quorum->timeouted_`)

### Step 5: Process Result (`server.cc:1048-1117`)

Three outcomes:

**Won** (`sp_quorum->yes()`):
1. Verify term hasn't advanced during the wait (stale election guard)
2. `setIsLeader(true)` — initialize leader state, update views
3. Reach the legacy Jetpack recovery entry point (unsupported and under a separate audit)
4. Fire `leader_change_cb_(true)` to notify the upper layer

**Lost** (`sp_quorum->no()`):
1. `setIsLeader(false)` — ensure follower state
2. Update `currentTerm` if a higher term was observed in responses
3. Reset `vote_for_` on term advancement

**Timeout** (neither majority):
1. Return `false` — the election timer will retry with a new random timeout

---

## 3. BroadcastVote() — Sending Vote RPCs

**Location**: `commo.cc:177-210`

```
BroadcastVote(par_id, lst_log_idx, lst_log_term, self_id, cur_term):
  |
  +-- n = partition size (total replicas)
  +-- Create RaftVoteQuorumEvent(n, n/2)
  |     (n total, n/2 quorum threshold)
  |
  +-- FOR each peer in partition (skip self):
  |     Send async Vote RPC with callback:
  |       callback(future):
  |         Extract (term, vote_granted) from reply
  |         quorum_event->FeedResponse(vote_granted, term)
  |
  +-- RETURN quorum_event
```

The quorum threshold is `n/2` (not `n/2 + 1`) because the candidate already counts its own vote. For a 3-node cluster, `n/2 = 1`, so the candidate needs 1 additional vote (total 2 out of 3 = majority).

---

## 4. RaftVoteQuorumEvent — Quorum Detection

**Location**: `commo.h:11-38`, inherits from `QuorumEvent` (`src/rrr/reactor/quorum_event.h:18-112`)

### Class Hierarchy

```
Event (base reactor event)
    |
    v
QuorumEvent (generic quorum voting)
    |
    v
RaftVoteQuorumEvent (Raft-specific vote tracking)
```

### Key Members (from `QuorumEvent`)

| Member | Type | Purpose |
|--------|------|---------|
| `n_voted_yes_` | `int32_t` | Count of YES votes |
| `n_voted_no_` | `int32_t` | Count of NO votes |
| `n_total_` | `int32_t` | Total replicas in the partition |
| `quorum_` | `int32_t` | Quorum threshold (n/2) |
| `highest_term_` | `int64_t` | Highest term seen in any response |
| `timeouted_` | `bool` | Whether the wait timed out |

### Quorum Logic

```cpp
// Majority voted YES
bool yes() {
    return n_voted_yes_ >= quorum_;
}

// Majority voted NO (impossible to reach quorum)
bool no() {
    return n_voted_no_ > (n_total_ - quorum_);
}
```

For a 3-node cluster (`n_total_=3`, `quorum_=1`):
- `yes()` when `n_voted_yes_ >= 1` (self + 1 peer = 2/3 majority)
- `no()` when `n_voted_no_ > 2` (impossible to reach quorum)

### FeedResponse()

```cpp
void FeedResponse(bool y, ballot_t term) {
    if (y) {
        vote_yes();     // Increment n_voted_yes_, call test()
    } else {
        vote_no();      // Increment n_voted_no_, call test()
        if (term > highest_term_) {
            highest_term_ = term;  // Track highest term for term advancement
        }
    }
}
```

The `test()` call (via `vote_yes()`/`vote_no()`) triggers `is_ready()` evaluation, which wakes the waiting coroutine if quorum is reached.

---

## 5. Vote Granting: OnRequestVote()

**Location**: `server.cc:1120-1185`

When a server receives a `RequestVote` RPC, it evaluates whether to grant the vote:

### Decision Tree

```
OnRequestVote(lst_log_idx, lst_log_term, can_id, can_term):
  |
  +-- [1] can_term < currentTerm?
  |     YES -> REJECT (stale candidate)
  |
  +-- [2] Already voted for different candidate in this term?
  |     (can_term == currentTerm && vote_for_ != INVALID && vote_for_ != can_id)
  |     YES -> REJECT (already voted)
  |
  +-- [3] Already voted for THIS candidate? (idempotent)
  |     (can_term == currentTerm && vote_for_ == can_id)
  |     YES -> GRANT (safe to re-grant)
  |
  +-- [4] Log up-to-date check:
  |     my_last_term = raft_logs_[lastLogIndex].term
  |     my_last_idx  = lastLogIndex
  |
  |     Candidate at least as up-to-date?
  |       (lst_log_term > my_last_term) OR
  |       (lst_log_term == my_last_term AND lst_log_idx >= my_last_idx)
  |     YES -> GRANT
  |     NO  -> REJECT
```

### The doVote() Helper (`server.h:161-202`)

All vote responses go through `doVote()`, which handles:

1. **Term advancement**: If `can_term > currentTerm`, the voter steps down, updates its term, resets `vote_for_`, and persists:
   ```cpp
   if (can_term > currentTerm) {
       setIsLeader(false);
       currentTerm = can_term;
       vote_for_ = INVALID_SITEID;
       PersistTermAndVote();
   }
   ```

2. **Vote recording**: If granting, records the vote and resets the election timer:
   ```cpp
   if (vote) {
       setIsLeader(false);
       vote_for_ = can_id;
       PersistVote();
       resetTimer("granted vote");
   }
   ```

3. **Reply**: Sets `*vote_granted` and `*reply_term = currentTerm`, invokes callback.

### Why Resetting the Timer on Vote Grant Matters

When a follower grants a vote, it resets its election timer. This prevents the voter from immediately starting its own election, giving the candidate time to win and begin sending heartbeats.

---

## 6. Split Vote Handling

Split votes occur when two candidates start elections simultaneously and neither reaches a majority. Raft handles this via **randomized election timeouts**:

1. Each candidate that loses (or times out) returns to follower state
2. The next election attempt uses a fresh random timeout from `GetElectionTimeout()`
3. With high probability, one candidate's timer fires before the other
4. That candidate wins the next election uncontested

The preferred leader mechanism further reduces split votes by giving the preferred replica a shorter timeout (150-300ms vs 500ms-1s), making it statistically likely to start elections first.

---

## 7. Term Advancement

Term advancement ensures the system makes progress and stale leaders step down:

### Where Terms Advance

| Location | Trigger | Action |
|----------|---------|--------|
| `RequestVote()` line 1021 | Starting election | `currentTerm++`, vote for self |
| `doVote()` line 177-186 | Vote request with higher term | `currentTerm = can_term`, step down |
| `OnAppendEntries()` line 1305-1313 | AppendEntries with higher term | `currentTerm = leaderTerm`, step down |
| `HeartbeatLoop()` line 859-871 | Follower response with higher term | Step down, `currentTerm = ret_term` |
| `RequestVote()` line 1100-1106 | Lost election, observed higher term | `currentTerm = new_term` |

### Invariant: Monotonically Increasing

`currentTerm` never decreases. Every term change is persisted before any response is sent, ensuring crash safety.

---

## 8. Complete Election Cycle — Sequence Diagram

The following shows a successful election in a 3-node cluster where Server A (preferred leader) wins.

```
Time    Server A (Preferred)         Server B                    Server C
 |      State: Follower              State: Follower             State: Follower
 |
 |      [Election timer fires]
 |      term: 0 -> 1
 |      vote_for_ = A
 |      PersistTermAndVote()
 |      State: Candidate
 |
 |      ---- RequestVote(term=1, lastIdx=5, lastTerm=1) ------>
 |      ---- RequestVote(term=1, lastIdx=5, lastTerm=1) ---------------------->
 |
 |                                   [OnRequestVote]            [OnRequestVote]
 |                                   can_term(1) >= myTerm(0)   can_term(1) >= myTerm(0)
 |                                   Log up-to-date? YES        Log up-to-date? YES
 |                                   vote_for_ = A              vote_for_ = A
 |                                   PersistVote()              PersistVote()
 |                                   resetTimer()               resetTimer()
 |
 |      <--- Reply(term=1, granted=true) -----
 |      <--- Reply(term=1, granted=true) ------------------------------------
 |
 |      [RaftVoteQuorumEvent]
 |      n_voted_yes_ = 2 >= quorum_(1)
 |      sp_quorum->yes() = true
 |
 |      [setIsLeader(true)]
 |      Initialize match_index_: {B:0, C:0}
 |      Initialize next_index_:  {B:6, C:6}
 |      Update new_view_ (leader=A, term=1)
 |      Fire leader_change_cb_(true)
 |      State: Leader
 |
 |      [HeartbeatLoop wakes]
 |      ---- AppendEntries(term=1, prevIdx=5, commit=5) ------>
 |      ---- AppendEntries(term=1, prevIdx=5, commit=5) ---------------------->
 |
 |                                   [resetTimer]               [resetTimer]
 |                                   (heartbeat received)       (heartbeat received)
 |
 v      Leader established, normal operation begins
```

### Timing Breakdown

| Phase | Duration | Bounded By |
|-------|----------|------------|
| Election timeout | 150-300 ms (preferred) | `GetElectionTimeout()` |
| Vote RPC round-trip | < 1 ms (local) | Network latency |
| Quorum wait | < 1 ms (local) | Slowest voter |
| Total election | ~150-300 ms | Dominated by timeout |

In production with network latency, the vote RPC round-trip adds 1-10ms per hop. The total election time is still dominated by the election timeout, not the RPC latency.

---

## 9. Leader Change Notification

When a server transitions to or from leader, it fires a callback to notify the upper system layer:

### Registration

`RaftWorker::SetupBase()` (`src/deptran/raft/raft_worker.cc:50-59`) registers the callback:

```cpp
raft_server->RegisterLeaderChangeCallback([this](bool leader) {
    {
        std::lock_guard<std::recursive_mutex> guard(election_state_lock);
        is_leader = leader ? 1 : 0;
    }
    uint32_t par_id = site_info_ ? site_info_->partition_id_ : 0;
    NotifyRaftLeaderChange(par_id, leader);
});
```

### Firing

In `setIsLeader()` (`server.cc:569-577`):
- `leader_change_cb_(true)` when `become_new_leader`
- `leader_change_cb_(false)` when `become_new_follower`

This allows clients to retarget requests to the new leader after elections.

---

## 10. Edge Cases and Robustness

### Pre-Vote Not Implemented

This implementation does not use the pre-vote extension (Section 9.6 of the Raft dissertation). A partitioned server will increment its term on each election timeout and may disrupt the cluster when it reconnects. The preferred leader mechanism partially mitigates this by biasing elections toward a known-good replica.

### Stale Election Guard

After winning a quorum, the candidate checks whether its term has advanced during the wait (`server.cc:1050`):

```cpp
if (term != currentTerm) {
    return false;  // Abandon: another election happened while we waited
}
```

This prevents a stale election from overwriting a newer leader.

### Shutdown Safety

The `stop_` flag is checked before every `RequestVote()` call to prevent virtual method calls through a collapsed vtable during destructor execution.

---

## Related Documents

- [Protocol Overview](protocol_overview.md) — High-level Raft architecture
- [Server Implementation](server_implementation.md) — Full `RaftServer` walkthrough
- [Log Replication](log_replication.md) — How committed entries are replicated
- [Preferred Leader](../03-preferred-leader/mechanism.md) — Leadership transfer protocol
