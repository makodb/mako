# Architecture Overview

This document explains the architecture that is currently implemented in the repository.

The most important fact for a new reader is that **Mako is replication-agnostic at the benchmark / transaction layer**. The benchmark harness, Mako transaction code, and most of the operational scripts interact with a small replication helper API, and that helper dispatches to either Paxos or Raft at runtime.

For the current replication details, continue with [Replication Current State](replication-current-state.md).

## High-Level Shape

At a high level the codebase has five layers:

1. **Benchmark + Mako transaction layer**
   - Generates TPC-C and other workloads.
   - Builds transaction logs and watermark metadata.
   - Lives mostly under `src/mako/`.
2. **Replication dispatch layer**
   - Presents one API to Mako regardless of backend.
   - Selects Paxos or Raft at runtime.
   - Lives in `src/deptran/replication_helper.*`.
3. **Backend integration layer**
   - Bridges Mako's log submission and callback model into the chosen backend.
   - Paxos path centers on `paxos_main_helper` / `PaxosWorker`.
   - Raft path centers on `raft_main_helper` / `RaftWorker`.
4. **Consensus backend**
   - Paxos: per-partition Multi-Paxos path.
   - Raft: single-instance or multi-instance depending on `SINGLE_RAFT_INSTANCE`.
5. **Replay + storage**
   - Followers replay committed logs into Masstree / Mako state.
   - Persistence is optional and routed through RocksDB-backed paths.

## Runtime Dispatch

The current backend selection model is:

- `--replication paxos` or `--replication raft` can be passed explicitly.
- If not passed, config inspection can still drive the backend choice.
- Mako code calls helper functions such as `setup()`, `add_log_to_nc()`, and callback registration functions without knowing which backend is active.

This keeps most of the benchmark and transaction code protocol-neutral.

## Leader vs Follower Roles

The current `dbtest` flow is intentionally asymmetric:

- The `localhost` role is treated as the workload-generating leader side.
- Non-`localhost` roles (`p1`, `p2`, and related follower roles) initialize replication state and primarily replay committed logs.
- `BenchmarkConfig::getLeaderConfig()` is the key runtime check used to decide whether to launch worker threads that generate benchmark traffic.

This matters when reading logs and experiments:

- **leader logs** show client-side throughput, latency, and abort rates
- **follower logs** are the main source for replay progress such as `replay_batch`

## Raft Modes

The checked-in Raft code supports two layouts:

### Multi-Raft

- One `RaftWorker` and one `RaftServer` per partition.
- One heartbeat loop per partition.
- Apply stays inline in the Raft path.

### Single-Raft

- One `RaftWorker` and one `RaftServer` handle all partitions on a process.
- Extra partition ports are served by stub RPC servers that route into the same Raft instance.
- Submission is still partition-tagged, but all partitions share one consensus log and one heartbeat loop.

Single-Raft is the mode that interacts most strongly with the replay-pool work described below.

## Replay Path

The current implementation separates three pieces of follower-side work:

1. **Consensus commit bookkeeping**
   - Raft or Paxos decides that an entry is committed.
2. **Apply dispatch**
   - In single-Raft, committed entries are moved onto a lightweight apply queue.
   - The apply thread is named `raft_apply` and exists mainly to keep the consensus path responsive.
3. **Heavy replay into the database**
   - The expensive Masstree replay work is now handled by `ReplayPool`.
   - Replay threads are named `replay_*`.
   - The pool size is controlled by `MAKO_REPLAY_THREADS`.

This is the main architectural change that newer docs must reflect: **the single-Raft apply thread is no longer the place where the expensive replay work happens**.

## 1:1 Replay Threads

Current scripts use the phrase **"1:1"** to mean:

- `worker_threads = replay_threads`

not:

- one Raft instance per worker
- one OS process per worker

The canonical wrapper for this experiment is:

- `scripts/sweep_single_raft_1to1.sh`

It repeatedly runs the shared sweep harness with:

- `MAKO_REPLAY_THREADS=t`
- worker thread count `t`

## Benchmark Harness

The canonical sweep script is:

- `scripts/run_scalability_sweep.sh`

Other scripts are wrappers around it:

- `scripts/sweep_single_raft_1to1.sh`
- `scripts/run_non_persistence_sweep.sh`
- `scripts/run_simulated_persistence_sweep.sh`
- `scripts/overnight_four_way.sh`

Those wrappers differ mostly in:

- which build directory they use
- which backend they label
- whether they force `MAKO_REPLAY_THREADS`
- where they store output

See [Benchmark Sweeps](../performance/benchmark-sweeps.md) for the full layout.

## File Map

The most important current files are:

- `src/mako/mako.hh`: benchmark + callback wiring, replay-pool bootstrap, persistence hooks
- `src/mako/replay_pool.*`: parallel follower replay workers
- `src/deptran/replication_helper.*`: protocol dispatch API
- `src/deptran/raft_main_helper.cc`: Mako-to-Raft bridge
- `src/deptran/raft/raft_worker.*`: submission queue and callback bridge
- `src/deptran/raft/server.*`: Raft state machine, heartbeat loop, commit/apply path
- `src/deptran/paxos_worker.cc`: Mako-to-Paxos bridge
- `scripts/run_scalability_sweep.sh`: canonical benchmark harness

## Recommended Next Reads

- [Replication Current State](replication-current-state.md)
- [Benchmark Sweeps](../performance/benchmark-sweeps.md)
- [Transport Backends](../developer/transport-backends.md)
