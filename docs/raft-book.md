# The Raft Book

This guide explains the **current Raft implementation as used by Mako today**.

It is intentionally narrower than the older migration and thesis material. The goal here is to make the checked-in code understandable without forcing a new reader through abandoned intermediate designs.

For the shortest path, read these first:

1. [Architecture Overview](architecture/overview.md)
2. [Replication Current State](architecture/replication-current-state.md)
3. this file

## What Raft Means in This Repository

Raft is one of the runtime-selectable replication backends behind Mako's transaction layer.

Important consequences:

- Mako does not directly call Raft-specific code from most benchmark paths.
- The benchmark harness talks to `replication_helper`, which dispatches to Raft or Paxos.
- Raft is embedded in the same process as the Mako benchmark and storage code.

The main files are:

- `src/deptran/replication_helper.*`
- `src/deptran/raft_main_helper.cc`
- `src/deptran/raft/raft_worker.*`
- `src/deptran/raft/server.*`
- `src/mako/mako.hh`
- `src/mako/replay_pool.*`

## Two Raft Layouts

### Multi-Raft

- one `RaftWorker` and one `RaftServer` per partition
- one heartbeat loop per partition
- per-partition ownership in `find_worker(par_id)`
- inline apply on the Raft side

### Single-Raft

- one `RaftWorker` and one `RaftServer` handle all partitions
- `find_worker(par_id)` always returns worker 0
- stub RPC servers bind the extra partition ports
- one shared consensus log and one shared heartbeat loop
- committed entries flow through a lightweight apply queue before callback dispatch

## Submission Path

The current Raft submission path is:

1. Mako calls `add_log_to_nc(log, len, par_id, batch_size)`.
2. `raft_main_helper` chooses a worker with `find_worker(par_id)`.
3. `enqueue_to_worker(...)` increments bookkeeping and queues the payload.
4. `RaftWorker::SubmitLoop()` drains the queue.
5. `RaftWorker::Submit(...)` wraps the payload in a `TpcCommitCommand`.
6. `RaftServer::Start(...)` appends the command to the Raft log if the worker is leader for that partition.

Single-Raft preserves the original partition identity by storing `par_id` in the generated command payload, so the callback path can still route replay correctly.

## Replication Path

The leader-side consensus engine is `RaftServer::HeartbeatLoop()`.

In broad strokes it:

- waits on a short event/timer cadence
- computes commit advancement from follower `match_index_`
- sends `AppendEntries` RPCs to followers in parallel
- processes the responses in one pass
- advances `commitIndex`

Multi-Raft and Single-Raft differ mainly in what happens after commit:

- **multi-Raft** applies inline
- **single-Raft** enqueues committed entries to the apply queue

## Current Apply / Replay Model

This is the main place where older docs tend to be wrong.

### What the code does now

In current single-Raft:

1. `EnqueueCommittedEntries()` moves committed log entries to `apply_queue_`.
2. `StartApplyThread()` drains that queue on thread `raft_apply`.
3. The callback path in `mako.hh` updates watermark bookkeeping and usually hands the heavy replay work to `ReplayPool`.
4. `ReplayPool` runs the expensive Masstree replay on `replay_*` threads.

So:

- the apply thread is real and still important
- but the heavy follower-side database replay is no longer supposed to happen there

This is why the benchmark harness now records separate CPU buckets for:

- worker threads
- replay threads
- apply thread

## ReplayPool

`ReplayPool` is a process-wide helper for follower replay.

Key properties:

- initialized from `src/mako/mako.hh`
- sized by `MAKO_REPLAY_THREADS`
- sharded by `par_id % N`
- preserves per-partition ordering while letting different partitions replay concurrently
- maintains per-worker deferred queues for entries that are not yet safe under the watermark checks

The "1:1 replay pool" experiments simply set:

- `MAKO_REPLAY_THREADS = worker_threads`

## Leader and Follower Callback Roles

Raft leadership can change, so `RaftWorker` keeps both leader and follower callback registrations and selects the correct one dynamically.

The important current behavior is:

- callbacks are registered per partition in single-Raft mode
- a leader change re-applies callback routing so that Mako keeps receiving the right role-specific behavior

## Preferred Leader Behavior

The current Raft implementation still contains preferred-leader logic:

- startup election bias toward the preferred replica
- leadership transfer support
- monitor-thread-based handling of transfer-related behavior

This is part of the live implementation, but it is not the main point of the current replay-pool scalability work.

## What to Ignore in Older Docs

If you see older Raft docs that claim:

- `applyLogs()` is the whole follower replay path
- `StartApplyFiber()` is the real apply engine
- current single-Raft conclusions can be derived from raw leader throughput alone

those docs are describing an older stage of the project.

Use this file plus [Replication Current State](architecture/replication-current-state.md) instead.
