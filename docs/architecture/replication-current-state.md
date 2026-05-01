# Replication Current State

This document is the code-verified description of the replication stack that is currently checked into the repository.

If another document disagrees with this page, trust:

1. the code in `src/`
2. the active sweep scripts in `scripts/`
3. this page

## What Is Actually in the Tree

Mako currently supports:

- **Paxos**
- **Multi-Raft**
- **Single-Raft**

All three backends are exercised through the same benchmark harness and produce comparable `results.csv` outputs.

The current repository is no longer in the older "single-Raft unified log wins outright" state described by some historical notes. It also is no longer the simpler "single-Raft only works because of one heavy apply thread" design. The checked-in code includes a **parallel replay pool** that materially changes the single-Raft behavior.

## Source-of-Truth Files

Read these first when validating behavior:

- `src/deptran/replication_helper.cc`
- `src/deptran/raft_main_helper.cc`
- `src/deptran/raft/raft_worker.cc`
- `src/deptran/raft/server.cc`
- `src/mako/mako.hh`
- `src/mako/replay_pool.cc`
- `scripts/run_scalability_sweep.sh`

## Runtime Dispatch Model

At runtime, Mako calls into `replication_helper`, which dispatches to:

- `paxos_impl::*` for Paxos
- `raft_impl::*` for Raft

This means most of the Mako benchmark code does not need separate Paxos and Raft call sites.

The key consequence for readers is:

- if you want to understand "how Mako talks to consensus," start at `replication_helper` and then follow the selected helper implementation

## Role Model in `dbtest`

`dbtest` is not symmetric across roles.

### `localhost`

- treated as the workload-generating leader-side process
- starts benchmark worker threads
- reports throughput / latency / abort-rate metrics

### `p1`, `p2`, and similar peers

- start replication services and callback paths
- primarily replay committed work
- provide the replay-side evidence used by sweep scripts, especially `replay_batch_p1` and `replay_batch_p2`

This distinction is visible in `BenchmarkConfig::getLeaderConfig()` and in the `run_workers()` gating in `dbtest`.

## Paxos Path

The Paxos path remains the original Mako replication bridge:

- Mako submits serialized transaction logs
- Paxos orders and commits them
- callbacks update watermark state and replay committed entries

The important current note is that the benchmark harness now treats Paxos the same way as the Raft backends for CPU accounting and replay metrics.

## Raft Path

The Raft path is split into:

1. **helper / worker integration**
2. **consensus state machine**
3. **callback routing back into Mako**

### `raft_main_helper`

This file owns the Mako-facing helper functions for Raft.

Its main jobs are:

- create workers during `setup()`
- select a worker for a partition
- launch network services and heartbeat/control services
- register leader and follower replay callbacks
- bridge leader-change events back into the callback layer

### `RaftWorker`

`RaftWorker` owns:

- the submit queue
- the submit thread
- callback registration
- leader/follower callback dispatch into Mako

The submit flow is:

1. `add_log_to_nc(...)`
2. `find_worker(par_id)`
3. `enqueue_to_worker(...)`
4. `EnqueueLog(...)`
5. `SubmitLoop()`
6. `Submit(...)`
7. `RaftServer::Start(...)`

### `RaftServer`

`RaftServer` owns:

- leader election
- heartbeat loop
- append / vote RPC handlers
- commit index tracking
- apply queue infrastructure in single-Raft mode

## Multi-Raft vs Single-Raft

### Multi-Raft

- one worker and one Raft server per partition
- one heartbeat loop per partition
- `find_worker(par_id)` searches the worker vector
- apply remains inline in the Raft path

### Single-Raft

- one worker and one Raft server handle all partitions
- `find_worker(par_id)` always returns worker 0
- extra ports are served by stub RPC servers
- committed entries are funneled through one apply queue before callback dispatch

The single-Raft architecture is explained in more detail in `doc/single_raft_design.md`, but the critical update is below.

## The Current Apply / Replay Split

This is the most important architectural fact that older docs often miss.

### Old mental model

Older docs often assume:

- committed entry arrives
- single apply thread does the heavy database replay itself
- replication bottleneck is dominated directly by that thread

### Current mental model

The current tree splits the work:

1. **Consensus path**
   - Raft commits an entry.
2. **Apply dispatch**
   - In single-Raft, `EnqueueCommittedEntries()` moves committed entries onto an apply queue.
   - `StartApplyThread()` drains that queue on thread `raft_apply`.
3. **Heavy replay**
   - The callback path in `mako.hh` hands the expensive database replay to `ReplayPool`.
   - Replay workers run as `replay_0`, `replay_1`, and so on.

So in current single-Raft:

- the apply thread is still important
- but it is mostly a lightweight staging thread now
- the expensive follower-side replay has moved into the replay pool

## ReplayPool

`ReplayPool` is process-wide and is initialized from `mako.hh`.

Important properties:

- size is controlled by `MAKO_REPLAY_THREADS`
- tasks are sharded by `par_id % N`
- each worker maintains its own deferred `un_replay` queues
- replay threads perform watermark safety checks before replay
- successful replay increments `BenchmarkConfig::replay_batch_`

This is why sweep scripts can compare:

- worker CPU
- replay CPU
- apply CPU

as distinct buckets.

## What "1:1" Means

In the current experiments, **1:1** means:

- worker thread count `t`
- replay pool size `t`

This is implemented by `scripts/sweep_single_raft_1to1.sh`, which repeatedly runs the shared sweep harness with `MAKO_REPLAY_THREADS=t`.

It does **not** mean:

- one Raft group per worker
- one process per worker
- one apply thread per worker

## Sweep Harness and Metrics

The canonical script is:

- `scripts/run_scalability_sweep.sh`

Key output columns:

- `throughput_ops_sec`: leader-side throughput reported by the benchmark
- `replay_batch_p1`, `replay_batch_p2`: follower replay progress counters
- `replay_threads`: replay-pool size for wrapper scripts that append it
- `role_worker_mean`, `role_worker_peak`: CPU for `worker_*` threads
- `role_replay_mean`, `role_replay_peak`: CPU for `replay_*` threads
- `role_apply_peak`: CPU for `raft_apply` or `paxos_apply`

This role bucketing works because the runtime explicitly names the relevant threads.

## Active Experiment Wrappers

The current wrappers are:

- `scripts/sweep_single_raft_1to1.sh`
  - single-Raft with `replay_threads = worker_threads`
- `scripts/run_non_persistence_sweep.sh`
  - three-way no-persistence sweep
- `scripts/run_simulated_persistence_sweep.sh`
  - three-way simulated-persistence sweep using fake-disk knobs
- `scripts/overnight_four_way.sh`
  - four-condition no-disk comparison, including the no-pool single-Raft baseline

## Results Layout

Current output roots are:

- `results/benchmarks/raft-single/`
- `results/benchmarks/raft-multi/`
- `results/benchmarks/paxos/`
- `results/benchmarks/non-persistence-results/`
- `results/benchmarks/simulated-persistence-results/`

The root-level file:

- `results/benchmarks/final_sweeps.log`

is the best quick summary of recent long-running benchmark activity already present in the workspace.

## What Is Historical

The following kinds of documents are still useful but should not be read as canonical behavior:

- old migration plans under `docs/migration/raft/`
- experiment analyses under `docs/dev/`
- earlier handoff notes that conclude single-Raft only flatlines
- thesis prose that has not yet been updated

Use them to understand how the code evolved, not to decide what the code does today.
