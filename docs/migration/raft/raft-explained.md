# Raft in Mako – Plain-English Guide

**Location**: `src/deptran/raft`  
**Audience**: Anyone trying to understand, run, or debug the supported Raft path without digging through every source file.
**Updated**: 2026-08-22

---

## 1. What Problem This Code Solves

- Mako stores client commands in a replicated log so that all replicas execute the same sequence.
- The Raft module makes sure **one leader** orders log entries, **followers** copy them, and everyone applies them in order.
- Legacy Jetpack recovery entry points are layered on top, but the Rule protocol
  that supplied their witness data is retired. The generic recovery stack is
  unsupported while it undergoes a separate audit.

---

## 2. File Map (Start Here When Browsing)

| File | Why it matters |
|------|----------------|
| `server.h/.cc` | The Raft state machine. Handles elections, heartbeats, replication, log application, and Jetpack entry points. |
| `coordinator.h/.cc` | Bridges client submissions to `RaftServer::Start`, waits for commit, and reports success/errors back. |
| `commo.h/.cc` | Wraps the RPC layer. Sends `Vote` and `AppendEntries` messages and collects replies. |
| `service.h/.cc` | Incoming RPC handlers that delegate to `RaftServer`. |
| `frame.h/.cc` | Connects the Raft pieces into the generic deptran framework (creates scheduler, coordinator, communicator, RPC service). |
| `test*.h/.cc` | Coroutine-based lab test harness (optional) that mimics MIT 6.824’s Raft suite. |
| `config/*.yml` | Ready-made configurations: `none_raft` (plain), `occ_raft` (OCC), `raft_lab_test` (five-node lab). |
| `../scheduler.cc` | Legacy generic Jetpack recovery functions (`JetpackRecoveryEntry`, `JetpackRecovery`), pending a separate audit. |

---

## 3. Key Concepts (Terminology)

- **Leader**: The replica that accepts client commands, assigns them log indices, and replicates them.
- **Follower**: Keeps a copy of the log, replies to `AppendEntries`, and can promote to leader if needed.
- **Term**: Monotonically increasing number that identifies leadership epochs.
- **Log entry**: A command (`shared_ptr<Marshallable>`) plus Raft metadata (`term`, `slot_id`, etc.).
- **commitIndex**: Highest log index known to be committed (stored on a majority).
- **executeIndex**: Highest index whose command has been applied to the state machine.
- **Jetpack**: A legacy recovery layer that exchanges witness sets after
  failover. It has no supported configuration after Rule retirement.

---

## 4. Important Data Structures

```
RaftData {
  ballot_t term;                  // Term of the entry
  shared_ptr<Marshallable> log_;  // The actual command
  shared_ptr<Marshallable> accepted_cmd_;
  shared_ptr<Marshallable> committed_cmd_;
  // Extra fields for retries / recovery (prevTerm, slot_id, ballot)
}
```

- `raft_logs_`: `map<slotid_t, shared_ptr<RaftData>>` – authoritative log.
- `match_index_`: map of follower → last replicated index (leader only).
- `next_index_`: map of follower → next index to send (leader only).
- `currentTerm`, `vote_for_`, `is_leader_`, `commitIndex`, `executeIndex` – standard Raft book-keeping.
- `timer_` (wrapped in `rusty::Box`): election timeout timer.
- `ready_for_replication_`: coroutine event that nudges the leader to send heartbeats or new entries.

---

## 5. How Leadership Works (Step-by-Step)

```
[Follower state]
  |
  | (no heartbeat for ~10×HEARTBEAT_INTERVAL)
  v
[StartElectionTimer()] ---> [RequestVote()]
  • increase currentTerm
  • figure out lastLogIndex/lastLogTerm
  • BroadcastVote() to peers
     - quorum replies collected via RaftVoteQuorumEvent
  • votes? -> setIsLeader(true)
             reset match_index_/next_index_
             call legacy JetpackRecoveryEntry()
```

- `resetTimer()` runs on every heartbeat or acceptable `AppendEntries`. It keeps the node from triggering an election.
- `setIsLeader()` also updates `new_view_` for the legacy recovery path and tells the communicator about the new leader.
- If a follower sees a higher term in `AppendEntries`, it steps down (`setIsLeader(false)`).

---

## 6. How Log Replication Works

```
Client -> CoordinatorRaft::Submit(cmd)
  • verify leader (else reply WRONG_LEADER with current view)
  • store callback, call GotoNextPhase()
    -> AppendEntries()
         RaftServer::Start(cmd, &index, &term)
         SetLocalAppend(cmd, ...)
           lastLogIndex++, store RaftData
         return index, term
    -> Wait until commitIndex >= index
    -> run commit callback, finish

Leader HeartbeatLoop()
  • run every HEARTBEAT_INTERVAL
  • for each follower:
      determine prevLogIndex/prevLogTerm from next_index_
      choose payload:
        - cmd = nullptr -> heartbeat
        - cmd = RaftData->log_ -> real log entry
      SendAppendEntries2() (per follower async RPC)
      when reply arrives:
        update match_index_, next_index_
        if majority has index -> advance commitIndex
        call applyLogs()

Follower OnAppendEntries()
  • check term and log continuity
  • append new log entries (batch or single)
  • advance commitIndex := min(leaderCommitIndex, lastLogIndex)
  • call applyLogs()
  • reply with {ok?, currentTerm, lastLogIndex}
```

`applyLogs()` iterates from `executeIndex + 1` to `commitIndex`, calls `app_next_(slot, command)` (into the transaction layer), and trims old entries via `removeCmd()`.

---

## 7. How Jetpack Recovery Fits In

The generic Jetpack entry points remain in the replication stack and are
reached on the **leader** when `setIsLeader(true)` fires:

1. `RaftServer::setIsLeader(true)` updates the view and calls `TxLogServer::JetpackRecoveryEntry()`.
2. `JetpackRecoveryEntry()` does two things (`src/deptran/scheduler.cc`):
   - `JetpackBeginRecovery()`: broadcasts a “begin recovery” message to the old view and waits for a majority acknowledgement.
   - `JetpackRecovery()`: gathers witness sets (IDs of commands that might need re-application) from replicas, merges them, and ensures those commands are re-run.
3. After the recovery routine completes, the new leader resumes normal log replication.

This is an implementation map, not a supported deployment recipe. The Rule
transaction protocol that populated witness sets and all of its configurations
are retired. Keep the remaining generic recovery path disabled until its
separate audit is complete.

---

## 8. How to Build

```bash
# Standard build (Paxos + Raft targets)
make -j32

# Optional: enable Raft lab test harness
cmake -B build -DRAFT_TEST=ON
cmake --build build -j32
```

The CMake flag links in coroutine-based tests and extra tracing. Production builds can stay on plain `make`.

---

## 9. Quick Start – Running Raft

### Single client / single shard / three replicas

```bash
./build/deptran_server \
  -f config/none_raft.yml \
  -f config/1c1s3r1p.yml \
  -f config/rw.yml \
  -f config/client_closed.yml \
  -f config/concurrent_1.yml \
  -d 30 -P localhost
```

### Higher concurrency (12 clients × 12 threads)

```bash
./build/deptran_server \
  -f config/none_raft.yml \
  -f config/12c1s3r1p.yml \
  -f config/rw.yml \
  -f config/client_closed.yml \
  -f config/concurrent_12.yml \
  -d 30 -P localhost
```

### Failover scope

The former Rule/Jetpack failover scenario is retired. Plain Raft configurations
can exercise consensus leader failover, but they do not validate the legacy
generic Jetpack recovery subsystem.

**Flags**  
- `-d` duration in seconds  
- `-P` host alias from `config/hosts*.yml` (default `localhost`)  
- Use `src/mako/update_config.sh` to refresh host mappings for distributed runs.

---

## 10. Testing Checklist

### Production sanity tests

Run any of the commands above and tail logs:

```
tail -f leader.log follower1.log follower2.log
```

Look for:
- `setIsLeader` messages when leadership changes.
- `Heartbeat` / `AppendEntries` logs showing follower progress.
- No legacy `[JETPACK-RECOVERY]` activity in supported configurations.

### Raft lab suite (optional but powerful)

1. Enable with `cmake -DRAFT_TEST=ON`.
2. Launch the harness:
   ```bash
   ./build/deptran_server -f config/raft_lab_test.yml
   ```
3. Tests run automatically inside `RaftLabTest::Run()` (`test.cc`). Highlights:
   - `testInitialElection`: ensures exactly one leader emerges.
   - `testReElection`: disconnect old leader, expect new leader.
   - `testBasicAgree`: commit entries without failures.
   - `testFailAgree` / `testFailNoAgree`: majority/minority failure cases.
   - `testConcurrentStarts`: stress concurrency under leader churn.
   - `testUnreliableAgree` & `testFigure8`: drop/delay RPCs to mimic real-world networks.
4. Outputs `TEST X Passed/Failed` and final RPC counts; failures include useful assertions.

### Reference implementation (optional)

`third-party/erpc/apps/smr` contains a minimal Raft+eRPC key-value store. It uses willemt/raft rather than the deptran Raft, but it’s handy for understanding raw RPC payloads. (Run `third-party/erpc/apps/smr/raft-install.sh` first.)

---

## 11. Troubleshooting Cheatsheet

| Symptom | What to check |
|---------|---------------|
| Frequent leader churn | Increase `HEARTBEAT_INTERVAL`, confirm followers reset timers (`resetTimer()` logs). |
| Followers “reject” append | Ensure `prevLogIndex` and `prevLogTerm` match; inspect `next_index_` adjustments in `HeartbeatLoop()`. |
| Commands stuck uncommitted | Confirm majority of followers reachable; look at `match_index_` values and network connectivity. |
| WRONG_LEADER errors in clients | `CoordinatorRaft::Submit()` attaches latest `View`; inspect `View` in logs, verify communicator knows the correct leader. |
| Legacy Jetpack output appears | Treat the recovery subsystem as unsupported; keep it disabled and record the configuration and `[JETPACK-RECOVERY]` logs for the separate audit. |
| Lab tests hang | Requires 5 servers (as defined in `config/raft_lab_test.yml`). Ensure `RAFT_TEST` build and that all services are on localhost ports 9000–9004. |

Useful debug macros:
- `RAFT_LEADER_ELECTION_DEBUG` – verbose election traces.
- `JETPACK_RECOVERY_DEBUG` – extra logging for auditing the legacy recovery code.

---

## 12. Quick Reference – Call Flow Diagram

```
Client
  |
  | Submit()
  v
CoordinatorRaft
  |  (check IsLeader?)
  |--> WRONG_LEADER -> client (with view)
  |
  | AppendEntries()
  v
RaftServer::Start()
  • SetLocalAppend()
  • return index/term
  |
  | HeartbeatLoop() replicates via SendAppendEntries2()
  |
  | On follower success -> commitIndex advances
  v
applyLogs()
  • app_next_(slot, command) into Tx engine
  |
  -> Coordinator sees commit, calls callback -> client OK
```

---

## 13. Need More Detail?

- Read `server.cc` from the top; every major action logs via `Log_info` / `Log_debug`.
- `CoordinatorRaft::AppendEntries()` (`coordinator.cc`) shows exactly how client callbacks are wired.
- `TxLogServer::JetpackRecovery()` (`scheduler.cc`) documents the legacy witness
  handling that remains under separate audit.
- Still confused? Open an issue with exact log snippets and the command you ran; include `config/*.yml` used.

---

Happy hacking! The goal is to pinpoint the file/class you need within a minute and understand the major flows without bouncing through every header.
