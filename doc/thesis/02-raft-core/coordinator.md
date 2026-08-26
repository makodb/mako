# CoordinatorRaft — Transaction Submission

## What This Document Covers

This document explains how transactions are submitted to the Raft consensus layer through `CoordinatorRaft`. It covers the `Submit()` entry point, the phase-based state machine, how `AppendEntries()` waits for commitment, `WRONG_LEADER` handling with view propagation, slot allocation via `Arc<Cell<slotid_t>>`, and how the coordinator integrates with Mako's transaction processing pipeline.

**Key source files**:
- `src/deptran/raft/coordinator.h` — Class definition (82 lines)
- `src/deptran/raft/coordinator.cc` — Implementation (181 lines)
- `src/deptran/raft/frame.cc` — `CreateCoordinator()` factory (lines 50-77)
- `src/deptran/coordinator.h` — Base `Coordinator` class
- `src/deptran/raft/raft_worker.cc` — Production Mako log-submission path

---

## 1. Class Hierarchy

```
Coordinator (base class, src/deptran/coordinator.h)
    |
    v
CoordinatorRaft (Raft-specific coordinator, src/deptran/raft/coordinator.h)
```

`CoordinatorRaft` extends the base `Coordinator` class, which provides:
- `commo_` — Pointer to the communicator
- `frame_` — Pointer to the protocol frame
- `phase_` — Current phase counter
- `committed_` — Whether the command was committed
- `commit_callback_` — Callback invoked after commitment
- `par_id_` — Partition ID
- `loc_id_` — Locale ID
- `mtx_` — Recursive mutex for thread safety

---

## 2. Key Members

| Member | Type | Purpose |
|--------|------|---------|
| `svr_` | `RaftServer*` | Pointer to the local Raft server instance |
| `cmd_` | `shared_ptr<Marshallable>` | The command being submitted |
| `slot_hint_` | `Arc<Cell<slotid_t>>` | Shared mutable slot counter (see Section 6) |
| `n_replica_` | `uint32_t` | Total replicas in the partition |
| `in_submission_` | `bool` | Debug flag: prevents concurrent submissions |
| `in_append_entries` | `bool` | Debug flag: prevents concurrent appends |
| `committed_` | `bool` | Whether the current command was committed |

### Phase Enum

```cpp
enum Phase { INIT_END = 0, PREPARE = 1, ACCEPT = 2, COMMIT = 3, FORWARD = 4 };
```

---

## 3. Submit() — The Entry Point

**Location**: `coordinator.cc:40-100`

This is the main entry point called by Mako's transaction layer to submit a committed transaction to Raft for replication.

### Flow

```
Submit(cmd, commit_callback, exe_callback):
  |
  +-- [1] IsLeader()?
  |     |
  |     +-- NO (WRONG_LEADER path):
  |     |     Set cmd->ret_ = WRONG_LEADER (-20)
  |     |     Attach view data (current leader info)
  |     |     Call commit_callback()
  |     |     Call app_next_(0, cmd) to update view
  |     |     RETURN
  |     |
  |     +-- YES (Leader path):
  |           Lock mutex
  |           Verify not already in submission
  |           Store cmd_ = cmd
  |           Store commit_callback_
  |           GotoNextPhase()
```

### How Production Mako Submits

The retired Classic transaction scheduler was the only transaction-engine
caller of `CoordinatorRaft::Submit()`. Production Mako submits raw log entries
from `RaftWorker::Submit()` and enters `RaftServer::Start()` directly:

```cpp
auto tpc_cmd = CreateRaftLogCommand(log_entry, length, tx_id, par_id);
uint64_t index = 0;
uint64_t term = 0;
raft_server->Start(std::move(tpc_cmd), &index, &term);
```

`CoordinatorRaft` remains the protocol-frame coordinator implementation, but
the production raw-log API does not create a Classic transaction or scheduler.

---

## 4. GotoNextPhase() — The Phase State Machine

**Location**: `coordinator.cc:151-179`

After `Submit()`, execution flows through a phase-based state machine:

```
GotoNextPhase():
  current_phase = phase_ % 4
  phase_++

  SWITCH current_phase:
    INIT_END (0):
      IF IsLeader():
        phase_++          // Skip PREPARE (not used in Raft)
        AppendEntries()   // Go directly to ACCEPT phase
        phase_++          // Advance to COMMIT
      ELSE:
        verify(0)         // Non-leader should have been handled in Submit()
      // FALL THROUGH to ACCEPT case

    ACCEPT (2):
      IF committed_:
        LeaderLearn()     // Call commit_callback
      ELSE:
        // Command not committed (term changed)

    PREPARE (1):
      AppendEntries()     // Alternate path

    COMMIT (3):
      // Terminal state, do nothing
```

For the leader path, the flow is: `INIT_END → AppendEntries() → COMMIT → LeaderLearn()`.

---

## 5. AppendEntries() — Waiting for Commitment

**Location**: `coordinator.cc:103-133`

This is where the coordinator hands the command to `RaftServer` and waits for it to be committed:

```
AppendEntries():
  |
  +-- Lock mutex
  +-- svr_->Start(cmd_, &index, &term)   // Append to leader's log
  |     Returns: index = log index, term = current term
  |
  +-- Signal ready_for_replication_->set(1)
  |     (Wake HeartbeatLoop to replicate immediately)
  |
  +-- POLLING LOOP:
  |     WHILE svr_->commitIndex < index:
  |       Wait 1ms (TimeoutEvent)
  |       IF svr_->currentTerm != term:
  |         // Leader changed! Term advanced.
  |         committed_ = false
  |         RETURN
  |
  +-- committed_ = true
```

### Key Design Points

1. **Blocking wait**: The coordinator polls `commitIndex` every 1ms until it reaches the entry's index. This is a cooperative wait using Mako's fiber system (`Reactor::create_sp_event<TimeoutEvent>(1000)->wait()`).

2. **Term change detection**: If `currentTerm` changes during the wait, the leader lost leadership. The command may or may not be committed by the new leader. The coordinator marks `committed_ = false` and returns, letting the upper layer retry.

3. **Immediate replication signal**: After `Start()`, the coordinator signals `ready_for_replication_` to wake the `HeartbeatLoop` immediately rather than waiting for the next heartbeat interval.

---

## 6. WRONG_LEADER Handling

**Location**: `coordinator.cc:42-85`

When `Submit()` is called on a non-leader, it returns `WRONG_LEADER` with view data so the client can redirect:

### The Error Code

Defined in `src/deptran/constants.h:78`:
```cpp
#define WRONG_LEADER (-20)
```

### What Happens

1. **Set error**: `tpc_cmd->ret_ = WRONG_LEADER`
2. **Attach view**: Create `ViewData` from `svr_->new_view_` (which contains the current leader info and term) and attach to `tpc_cmd->sp_view_data_`
3. **Handle empty view**: If `new_view_` is empty (no known leader), construct a view with `leader = -1`
4. **Call callbacks**: Still call `commit_callback()` and `app_next_(0, cmd)` so the upper layer can update its leader tracking and clean up

### View Propagation

The `WRONG_LEADER` response carries view data back through Mako's RPC layer. The client extracts the view to learn who the current leader is and retries the transaction there.

```
Client → Submit(cmd) → non-leader server
                            |
                            v
                     ret_ = WRONG_LEADER
                     sp_view_data_ = ViewData(leader=X, term=T)
                            |
                            v
                     Client receives WRONG_LEADER
                     Updates leader tracking
                     Retries on server X
```

---

## 7. Slot Allocation via Arc<Cell<slotid_t>>

**Location**: `coordinator.h:40`, `frame.h:19`, `frame.cc:69-71`

The `slot_hint_` field provides thread-safe slot ID allocation using RustyCpp's interior mutability pattern:

### How It Works

1. **Created once** in `RaftFrame` (`frame.h:19`):
   ```cpp
   rusty::Arc<rusty::Cell<slotid_t>> slot_hint_ =
       rusty::Arc<rusty::Cell<slotid_t>>::make(1);
   ```

2. **Shared** with each `CoordinatorRaft` via `Arc` copy (`frame.cc:69`):
   ```cpp
   coo->slot_hint_ = slot_hint_;  // Arc copy shares ownership
   ```

3. **Incremented** atomically for each new coordinator (`frame.cc:70-71`):
   ```cpp
   coo->slot_id_ = slot_hint_->get();
   slot_hint_->set(slot_hint_->get() + 1);
   ```

### Why Arc<Cell<T>>

- `Arc<T>` provides thread-safe reference counting (like `std::shared_ptr` but following Rust ownership model)
- `Cell<T>` provides interior mutability for `Copy` types (like `slotid_t`) without a mutex
- Together, they allow multiple coordinators to share a monotonically increasing counter without locks
- This follows the RustyCpp migration guidelines from `CLAUDE.md`

---

## 8. Quorum Calculation

**Location**: `coordinator.h:61-63`

```cpp
uint32_t GetQuorum() {
    return n_replica() / 2 + 1;
}
```

For a 3-node cluster: `3/2 + 1 = 2` (majority). This is the standard Raft quorum formula. Note that `CoordinatorRaft` doesn't directly use `GetQuorum()` for commit decisions — that logic lives in `RaftServer::HeartbeatLoop()`. The quorum is defined here for consistency and is used by other coordinator methods.

---

## 9. CreateCoordinator() — Factory Method

**Location**: `frame.cc:50-77`

Each transaction gets a fresh `CoordinatorRaft` instance:

```
RaftFrame::CreateCoordinator(coo_id, config, benchmark, client_status, id, txn_reg):
  |
  +-- Create new CoordinatorRaft(coo_id, benchmark, client_status, id)
  +-- Set coo->frame_ = this
  +-- Set coo->commo_ = commo_.get()           // Borrow communicator
  +-- Set coo->svr_ = svr_.get()               // Borrow Raft server
  +-- Set coo->slot_hint_ = slot_hint_          // Share Arc
  +-- Set coo->slot_id_ = slot_hint_->get()     // Allocate slot
  +-- Increment slot_hint_
  +-- Set coo->n_replica_ = partition size
  +-- Set coo->loc_id_ = locale ID
  +-- RETURN coo
```

The coordinator borrows pointers to the frame's owned `RaftCommo` and `RaftServer`. This is safe because the frame outlives all coordinators.

---

## 10. LeaderLearn() — Post-Commit Callback

**Location**: `coordinator.cc:144-148`

After successful commitment:

```cpp
void CoordinatorRaft::LeaderLearn() {
    lock(mtx_);
    commit_callback_();     // Notify upper layer of commit
    verify(phase_ == COMMIT);
    GotoNextPhase();        // Move to terminal state
}
```

The `commit_callback_` is set by `Submit()` from the caller's provided callback. This is how a frame-level caller learns that Raft has committed the entry.

---

## 11. Complete Submission Flow

```
Frame-level protocol caller
    |
    | Frame::CreateCoordinator() → new CoordinatorRaft
    | Submit(TpcCommitCommand)
    v
CoordinatorRaft::Submit()
    |
    +-- IsLeader()? YES
    |
    v
GotoNextPhase() → INIT_END
    |
    v
AppendEntries()
    |
    +-- RaftServer::Start(cmd) → append at index N, term T
    +-- Signal HeartbeatLoop to replicate
    +-- Poll: WHILE commitIndex < N
    |     Wait 1ms
    |     Check: term still T?
    +-- committed_ = true
    |
    v
GotoNextPhase() → COMMIT
    |
    v
LeaderLearn()
    |
    +-- commit_callback_()  → Notify caller
    |
    v
Submit() returns, command is committed
```

---

## Related Documents

- [Protocol Overview](protocol_overview.md) — High-level Raft architecture
- [Server Implementation](server_implementation.md) — `RaftServer::Start()` and `applyLogs()`
- [Log Replication](log_replication.md) — How entries are replicated after `Start()`
- [RPC Layer](rpc_layer.md) — Communication infrastructure
- [System Architecture](../01-mako-overview/system_architecture.md) — Where `CoordinatorRaft` fits in Mako
