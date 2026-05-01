# Single Raft Design

This document explains the **current** single-Raft layout in the repository.

It focuses on the implementation that is checked in now, not the older intermediate design where the single apply thread was assumed to do all follower replay work itself.

## Overview

Single-Raft means:

- one `RaftWorker`
- one `RaftServer`
- one Raft log
- one leader election domain
- one heartbeat loop

for all partitions hosted by a process.

Extra partition ports are still bound, but they are served by stub RPC servers that forward into the single real Raft instance.

## Architecture

### Before: Multi-Raft

```text
Partition 0 -> RaftWorker0 -> RaftServer0
Partition 1 -> RaftWorker1 -> RaftServer1
Partition 2 -> RaftWorker2 -> RaftServer2
```

### After: Single-Raft

```text
Partition 0 --\
Partition 1 ---+-> submit queue -> RaftWorker0 -> RaftServer0
Partition 2 --/                         |
                                        +-> stub RPC servers on extra ports
```

## Submit Path

Leader-side submit flow:

1. Mako calls `add_log_to_nc(log, len, par_id, batch_size)`.
2. `find_worker(par_id)` always returns `raft_workers_g[0]`.
3. `enqueue_to_worker(...)` increments bookkeeping and queues the payload.
4. `RaftWorker::SubmitLoop()` drains the queue.
5. `RaftWorker::Submit(...)` wraps the payload and preserves `par_id`.
6. `RaftServer::Start(...)` appends the command to the single log.

The key detail is that single-Raft still preserves partition identity in the command payload so the callback path can route replay to the right partition logic.

## Replication Path

1. `HeartbeatLoop()` sends `AppendEntries` to followers.
2. Followers can receive those RPCs on the main partition port or on one of the stub ports.
3. All of those RPCs terminate at the same `RaftServer`.
4. The leader advances `commitIndex` from follower `match_index_` values.

## Current Apply / Replay Path

This is the part that changed most from older docs.

### What happens now

1. Committed entries are queued by `EnqueueCommittedEntries(...)`.
2. `StartApplyThread()` drains that queue on thread `raft_apply`.
3. `RaftWorker::Next()` routes the committed entry to the correct partition callback.
4. In the Mako follower callback path, the expensive database replay is usually handed to `ReplayPool`.
5. `ReplayPool` runs that heavy work on `replay_*` threads.

### What that means

The apply thread is still important because it keeps the consensus path responsive, but the heavy database replay is no longer supposed to happen there.

Older descriptions that say "single-Raft uses one apply thread for replay" are incomplete for the current code.

## Per-Partition Callback Routing

Single-Raft stores per-partition leader and follower callbacks on the single worker.

That lets one shared Raft log still dispatch committed entries to the correct:

- watermark logic
- replay queue
- partition-local bookkeeping

Leader changes re-apply the callback routing so all known partitions stay wired correctly.

## Stub RPC Servers

Remote replicas still expect all partition ports to exist.

Single-Raft therefore creates lightweight stub RPC servers for the extra ports:

- they bind the expected addresses
- they register the same Raft service implementation
- they forward requests into the single real `RaftServer`

This keeps the wire-level topology compatible with the multi-Raft expectation while collapsing consensus into one local instance.

## Debugging Tips

If single-Raft looks wrong, check:

1. `find_worker(par_id)` behavior in `raft_main_helper.cc`
2. callback registration and leader-change handling
3. `EnqueueCommittedEntries(...)` and `StartApplyThread()`
4. whether `ReplayPool` is enabled and how many `replay_*` threads exist
5. follower `replay_batch` counts and per-role CPU columns in the sweep CSVs

## Related Docs

- `docs/architecture/replication-current-state.md`
- `docs/performance/benchmark-sweeps.md`
- `docs/raft-book.md`
