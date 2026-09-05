# Single Raft Design Document

## Overview

This document describes the **Single Raft** architecture where exactly one Raft instance (one `RaftWorker`, one `RaftServer`, one log, one leader election) handles all partitions. Previously, each partition had its own independent Raft group, leading to N redundant elections, heartbeat loops, and logs.

## Architecture

### Before (Per-Shard Raft)
```
Partition 0 → RaftWorker0 → RaftServer0  (port 27001)
Partition 1 → RaftWorker1 → RaftServer1  (port 27002)
Partition 2 → RaftWorker2 → RaftServer2  (port 27003)
(3 separate logs, 3 elections, 3 heartbeat loops)
```

### After (Single Raft)
```
Partition 0 ──→ ┐
Partition 1 ──→ ├→ [Queue] → RaftWorker0 → RaftServer0 (port 27001)
Partition 2 ──→ ┘
                    Stub RPC servers on ports 27002, 27003
                    (1 log, 1 election, 1 heartbeat loop)
```

## Data Flow

### Submit Path (Leader)
1. Mako worker thread calls `add_log_to_nc(log, len, par_id, batch_size)`
2. `find_worker(par_id)` → always returns `raft_workers_g[0]` (the single worker)
3. `enqueue_to_worker()` → `EnqueueLog()` → submit queue
4. `SubmitLoop()` drains queue, calls `Submit(log, len, par_id)`
5. `Submit()` calls `CreateRaftLogCommand(log, len, tx_id, par_id)` — **stores `par_id` in `SimpleCommand::partition_id_`**
6. `RaftServer::Start()` appends to the single Raft log

### Replication Path
1. Leader's `HeartbeatLoop()` sends `AppendEntries` RPCs to followers
2. Followers receive on their main port OR stub ports (both route to the same `RaftServer`)
3. Followers append to their single log, respond with success
4. Leader advances `commitIndex` based on majority `match_index_`

### Callback Routing (Apply Path)
1. `StartApplyFiber()` detects `executeIndex < commitIndex`
2. Calls `app_next_(slot_id, cmd)` → `RaftWorker::Next()`
3. `Next()` extracts `par_id` from `SimpleCommand::partition_id_` inside the committed entry
4. Looks up per-partition callback:
   - `leader_callbacks_by_partition_[par_id]` if leader
   - `follower_callbacks_by_partition_[par_id]` if follower
   - Falls back to `leader_callback_par_id_return_` / `follower_callback_par_id_return_` if maps are empty
5. Calls the callback with partition-specific `un_replay_logs_by_partition_[par_id]`

## Per-Partition Callback Maps

```cpp
// In RaftWorker (raft_worker.h)
std::map<uint32_t, watermark_callback_t> leader_callbacks_by_partition_;
std::map<uint32_t, watermark_callback_t> follower_callbacks_by_partition_;
std::map<uint32_t, std::queue<...>> un_replay_logs_by_partition_;
```

Registered via:
- `register_leader_callback_for_partition(par_id, cb)` — called from `apply_callbacks_for_partition()`
- `register_follower_callback_for_partition(par_id, cb)` — same

These are called from `raft_main_helper.cc::apply_callbacks_for_partition()` which in turn is triggered by:
- `register_for_leader_par_id_return()` / `register_for_follower_par_id_return()` — stores in `leader_replay_cb` / `follower_replay_cb` maps, then calls `apply_callbacks_for_partition()`
- Leader change events — `handle_leader_change_impl()` re-applies callbacks

## Stub RPC Servers

Remote replicas' `Communicator` objects expect to connect to ALL partition ports (e.g., 27001-27006 for 6 partitions). With only one real `RaftWorker` bound to port 27001, ports 27002-27006 would be unbound.

Solution: Create lightweight `srpc::Server` instances on each extra port, registering the same `RaftService` pointing to the single `RaftServer`. This means:
- All incoming `AppendEntries` and `RequestVote` RPCs reach the single Raft instance regardless of which port they arrive on
- Each stub has its own `PollThread` for I/O
- Stubs are created in `create_stub_servers()` and destroyed in `destroy_stub_servers()`

## Leader Change Notification

When the single Raft instance changes leadership, ALL registered partitions must be notified (not just partition 0). The `SetupBase()` callback iterates `leader_callbacks_by_partition_` and `follower_callbacks_by_partition_` to collect all known `par_id` values and calls `NotifyRaftLeaderChange()` for each.

## Files Modified

| File | Key Changes |
|------|-------------|
| `src/deptran/raft/raft_worker.h` | Added per-partition callback maps, `watermark_callback_t` typedef, `register_*_for_partition()` methods, updated `CreateRaftLogCommand` signature to take `par_id` |
| `src/deptran/raft/raft_worker.cc` | `CreateRaftLogCommand()` stores `par_id`; `Next()` extracts `par_id` from entry and routes to correct callback map; `IsLeader()`/`IsPartition()` work globally; per-partition registration methods; leader change notifies all partitions |
| `src/deptran/raft_main_helper.cc` | `setup()` creates 1 worker, stores all site infos; `find_worker()` always returns worker 0; `create_stub_servers()`/`destroy_stub_servers()` for extra ports; `apply_callbacks_for_partition()` uses per-partition registration; `setup2()` configures 1 worker; shutdown cleans up stubs |

## Files NOT Modified (Reference Only)

- `src/deptran/raft/server.h/.cc` — RaftServer replicates opaque entries, unchanged
- `src/mako/mako.hh` — Mako callbacks unchanged
- `src/mako/benchmarks/sto/sync_util.hh` — Watermark logic unchanged

## Debugging Playbook

Add `[SINGLE-RAFT]` prefixed logging at these points if something breaks:

1. **`Next()`**: Log extracted `par_id`, callback found (yes/no), encoded return value
2. **`Submit()`**: Log `par_id` being submitted, Raft log index returned
3. **`CreateRaftLogCommand()`**: Log `par_id` stored in `partition_id_`
4. **`server_launch_worker()`**: Log stub server count and ports
5. **`apply_callbacks_for_partition()`**: Log which `par_id` callback registered on which worker
6. **`HeartbeatLoop` in server.cc**: If `match_index` stops advancing, log `next_index_[follower]`, `min_active_slot_`, `lastLogIndex`, batch size

**Key diagnostic**: If `replay_batch` gets stuck:
- Check follower logs for `FOLLOWER-AE` messages. If they stop → issue is in leader's batch assembly or RPC delivery
- If they continue but `replay_batch` doesn't increase → issue is in callback or watermark
- If `match_index` gets stuck → check if GC (`min_active_slot_`) is evicting entries before followers replicate them

## Edge Cases

1. **GC vs Replication**: With all partitions sharing one log, entry rate is N times higher. The 60,000-entry GC buffer should handle ~100 seconds at 600 entries/sec.
2. **Watermark Starvation**: The advancer skips partitions with `local_timestamp_[i] == 0`. Safe for 1-shard tests.
3. **RegLearnerAction duplication**: `RaftServer::RegLearnerAction()` overwrites `app_next_`, so multiple calls are idempotent.
4. **Stub server service registration**: `CreateRpcServices()` creates new service instances per call, each pointing to the same `rep_sched_` (single `RaftServer`). Safe.
