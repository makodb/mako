# Single Raft Instance vs Multiple Raft Instances: Architectural Analysis

## Overview

This document describes the architectural differences between the **multiple Raft instances** approach (commit `4f99ffb6`) and the **single Raft instance** approach (commit `bba1a5d4`), both on the `mako-krish-new` branch.

## Multiple Raft Instances (Before)

In the original design, each partition has its own independent Raft group:

```
Partition 0 → RaftWorker0 → RaftServer0  (port 27001)
Partition 1 → RaftWorker1 → RaftServer1  (port 27002)
Partition 2 → RaftWorker2 → RaftServer2  (port 27003)
...
Partition N → RaftWorkerN → RaftServerN  (port 27000+N+1)
```

Each RaftServer maintains its own:
- Raft log (`raft_logs_`)
- Term and vote state (`currentTerm`, `vote_for_`)
- Leader election timer and heartbeat loop
- `commitIndex` / `executeIndex` tracking
- `match_index_` / `next_index_` per follower

### Characteristics
- **N independent elections**: Each partition group independently elects a leader.
- **N heartbeat loops**: Each leader sends heartbeats to its own followers.
- **N Raft logs**: Log entries are partitioned by design — partition 0's data only goes into RaftServer0's log.
- **Per-partition isolation**: A slow follower on partition 1 does not affect partition 0's commit progress.
- **Resource overhead**: N threads for heartbeat loops, N sets of timers, N RPC services.

## Single Raft Instance (After)

The single Raft instance consolidates all partitions into one Raft group:

```
Partition 0 ──→ ┐
Partition 1 ──→ ├→ [Queue] → RaftWorker0 → RaftServer0 (port 27001)
Partition 2 ──→ ┘
                    Stub RPC servers on ports 27002, 27003, ...
                    (1 log, 1 election, 1 heartbeat loop)
```

### Key Architectural Changes

#### 1. Single Worker Creation (`raft_main_helper.cc`)
- **Before**: `setup()` created N `RaftWorker` objects, one per `server_infos` entry.
- **After**: `setup()` creates exactly 1 `RaftWorker` from the first site_info (partition 0). All other site_infos are stored in `all_site_infos_g` for stub server creation.

#### 2. Stub RPC Servers (`raft_main_helper.cc`)
- **Problem**: Remote replicas' `Communicator` objects expect to connect to ALL partition ports (e.g., 27001-27006 for 6 partitions). With only one real `RaftWorker` on port 27001, ports 27002+ would be unbound.
- **Solution**: `create_stub_servers()` creates lightweight `srpc::Server` instances on each extra port, registering the same `RaftServiceImpl` pointing to the single `RaftServer`. All incoming `AppendEntries` and `RequestVote` RPCs reach the single Raft instance regardless of which port they arrive on.

#### 3. Per-Partition Callback Routing (`raft_worker.h/cc`)
- **Before**: Each `RaftWorker` had its own `leader_callback_par_id_return_` / `follower_callback_par_id_return_`.
- **After**: The single `RaftWorker` has per-partition callback maps:
  ```cpp
  std::map<uint32_t, watermark_callback_t> leader_callbacks_by_partition_;
  std::map<uint32_t, watermark_callback_t> follower_callbacks_by_partition_;
  std::map<uint32_t, std::queue<...>> un_replay_logs_by_partition_;
  ```
- `Next()` extracts `par_id` from the committed entry's `SimpleCommand::partition_id_` and routes to the correct partition's callback.

#### 4. Partition ID in Log Entries (`raft_worker.cc`)
- **Before**: `CreateRaftLogCommand(log, len, tx_id)` — no `par_id` needed since each worker knew its partition.
- **After**: `CreateRaftLogCommand(log, len, tx_id, par_id)` — `par_id` is stored in `SimpleCommand::partition_id_` so `Next()` can route correctly during apply.

#### 5. Worker Lookup (`raft_main_helper.cc`)
- **Before**: `find_worker(par_id)` searched all workers for one matching the partition.
- **After**: `find_worker(par_id)` always returns `raft_workers_g[0]` (the single worker). The `par_id` parameter is ignored.

#### 6. Leader Election Timeout Changes (`server.cc`)
Election timeouts were significantly increased for single-Raft mode:

| Role | Multi-Raft | Single-Raft |
|------|------------|-------------|
| Preferred leader | 150-300ms | 300-600ms |
| Non-preferred (grace) | 1-2s | 5-10s |
| Non-preferred (normal) | 500ms-1s | 3-6s |

The increase prevents spurious elections during the higher-latency operations of a single Raft instance handling all partitions.

#### 7. Background Apply Thread (`server.h/cc`)
- **New**: A dedicated `std::thread` (`apply_thread_`) drains committed entries from an `apply_queue_` (a `std::deque` protected by `apply_queue_mtx_`).
- **Purpose**: Decouples entry application (which calls slow `treplay` on followers) from RPC processing on the PollThread. This prevents the follower from becoming unresponsive to `AppendEntries` RPCs while applying entries.
- `EnqueueCommittedEntries()` is called from `OnAppendEntries()` when `commitIndex` advances, pushing entries to the queue.
- `StartApplyFiber()` becomes a lightweight monitor that logs status every 5 seconds.

#### 8. STO Thread ID Fix (`mako.hh`)
- **Problem**: In single-Raft, all partitions share one thread. `ThreadDBWrapperMbta::getDB()` calls `TThread::set_id(par_id)`, changing the thread ID per partition. But STO's `Transaction::threadid_` was cached at creation time. The mismatch between `try_lock` (uses `threadid_`) and `check_version` (uses `TThread::id()`) caused permanent commit failure.
- **Fix**: `Sto::update_threadid()` is called after `getDB()` to sync `Transaction::threadid_` with `TThread::id()`.

#### 9. Loading Phase Safety Check Bypass (`mako.hh`)
- **Problem**: In single-Raft, loading tail entries for other partitions may arrive after `ADVANCER_MARKER` fires for partition 0. The advancer previously reset all `local_timestamp_[i]` to 0, making `watermark=0`, causing those late entries to fail `safety_check`.
- **Fix**: `start_advancer()` no longer resets timestamps to 0 — it computes the initial watermark from current values. Additionally, during the loading phase (`noops_cnt == 0`), `safety_check` is bypassed entirely.

## Simplifications

1. **Single election**: Only one leader election for all partitions, reducing election storm risk.
2. **Single heartbeat loop**: One set of RPCs to all followers instead of N.
3. **Simpler `find_worker()`**: Always returns worker 0.
4. **Single preferred leader config**: Only partition 0's config needed.

## Added Complexity

1. **Stub servers**: Extra RPC servers on unused ports to maintain connectivity.
2. **Per-partition callback maps**: Routing logic in `Next()` to dispatch to the correct partition.
3. **Partition ID in log entries**: Extra field needed for routing.
4. **Background apply thread**: New threading model for entry application.
5. **STO threadid fix**: New call to `Sto::update_threadid()` to handle shared-thread semantics.
6. **Loading phase bypass**: New logic to handle watermark during loading.

## Files Modified (Between `4f99ffb6` and `bba1a5d4`)

| File | Lines Changed | Key Changes |
|------|--------------|-------------|
| `src/deptran/raft/server.cc` | +590/-378 | Background apply thread, apply queue, election timeout increases, StartApplyFiber, EnqueueCommittedEntries |
| `src/deptran/raft_main_helper.cc` | +252/-176 | Single worker creation, stub servers, per-partition callbacks, simplified setup/launch |
| `src/deptran/raft/raft_worker.cc` | +141/-79 | Per-partition callback maps, CreateRaftLogCommand with par_id, Next() routing |
| `doc/single_raft_design.md` | +117 | New design document |
| `src/mako/benchmarks/sto/sync_util.hh` | +38/-14 | Don't reset timestamps in start_advancer(), advancer debug logging |
| `src/deptran/raft/raft_worker.h` | +21/-2 | Per-partition callback types and maps, updated CreateRaftLogCommand signature |
| `src/deptran/raft/server.h` | +21 | Apply queue, apply thread, StartApplyFiber, EnqueueCommittedEntries declarations |
| `src/mako/mako.hh` | +21/-8 | Sto::update_threadid() fix, loading phase bypass |
| `src/mako/benchmarks/sto/MassTrans.hh` | +8/-8 | Minor (diagnostic changes, reverted) |
| `src/mako/benchmarks/sto/ThreadPool.cc` | -1 | Minor cleanup |

**Total**: +832 insertions, -378 deletions across 10 files.
