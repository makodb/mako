# Agent Handoff - Current Mako Replication State

**Last updated:** 2026-05-04  
**Branch:** `mako-krish-new`

This file is the fast onboarding note for agents and engineers working in this repository.

## Read This First

Before making claims about the replication architecture, read:

1. `docs/index.md`
2. `docs/architecture/overview.md`
3. `docs/architecture/replication-current-state.md`
4. `docs/performance/benchmark-sweeps.md`
5. `docs/user-manual.md`
6. `CLAUDE.md`

For thesis writing, start with the new paper-style draft and evidence tracker:

1. `doc/thesis/paper_style_thesis.md`
2. `doc/thesis/evaluation_artifacts.md`
3. `results/benchmarks/disk_compare_replay/thesis_followup_latest_paths.txt` once the thesis follow-up sweeps finish

If a thesis note or old migration doc disagrees with the current code, trust:

1. code in `src/`
2. active scripts in `scripts/`
3. the current-state docs above

## What Is Implemented Now

The repository currently supports three replication backends through one outer benchmark harness:

- **Paxos**
- **Multi-Raft**
- **Single-Raft**

The key architectural fact is that the checked-in single-Raft path is **not** the old "one apply thread does the heavy follower replay" design anymore.

Current single-Raft behavior:

- one `RaftWorker` / `RaftServer` handle all partitions on a process
- committed entries are staged through a lightweight apply queue
- the `raft_apply` thread drains that queue
- the expensive follower-side database replay is pushed into `ReplayPool`
- replay workers run on `replay_*` threads
- replay pool size is controlled by `MAKO_REPLAY_THREADS`

## The Meaning of "1:1"

In the current experiments, **1:1** means:

- `replay_threads = worker_threads`

It does **not** mean:

- one Raft group per worker
- one process per worker
- one apply thread per worker

The canonical wrapper for this is:

- `scripts/sweep_single_raft_1to1.sh`

## Current Process Roles

`dbtest` is asymmetric across roles:

- `localhost` starts the benchmark workload and produces leader-side throughput / latency / abort metrics
- followers such as `p1` and `p2` mainly replay committed work and produce the `replay_batch` evidence used by the sweep scripts

This distinction is enforced through `BenchmarkConfig::getLeaderConfig()`.

## Key Files

### Replication dispatch

- `src/deptran/replication_helper.*`

### Raft path

- `src/deptran/raft_main_helper.cc`
- `src/deptran/raft/raft_worker.*`
- `src/deptran/raft/server.*`

### Mako callback + replay path

- `src/mako/mako.hh`
- `src/mako/replay_pool.*`

### Benchmark harness

- `scripts/run_scalability_sweep.sh`
- `scripts/sweep_single_raft_1to1.sh`
- `scripts/run_non_persistence_sweep.sh`
- `scripts/run_simulated_persistence_sweep.sh`
- `scripts/overnight_four_way.sh`

## Current Builds

The script ecosystem currently assumes these build roots:

- `build/` - single-Raft, no persistence
- `build_multi/` - multi-Raft, no persistence
- `build_paxos/` - Paxos, no persistence
- `build_disk/` - single-Raft, persistence enabled
- `build_multi_disk/` - multi-Raft, persistence enabled
- `build_paxos_disk/` - Paxos, persistence enabled

## Current Config / Port Notes

The checked-in Raft configs under `config/1leader_2followers/` now use the `27xxx` / `28xxx` port ranges rather than the older `55xxx` / `56xxx` ranges.

If older docs mention the old ports, they are historical.

## Benchmark Outputs Already Present in This Workspace

Useful current directories:

- `results/benchmarks/non-persistence-results/`
- `results/benchmarks/simulated-persistence-results/`
- `results/benchmarks/raft-single/`
- `results/benchmarks/raft-multi/`
- `results/benchmarks/paxos/`
- `results/benchmarks/final_sweeps.log`

`final_sweeps.log` is the fastest way to see what long-running scripts recently did.

Active thesis/evaluation jobs as of 2026-05-04:

- `mako_proof_sweep_20260504_230526` is the focused disk-proof sweep for no-disk, NVMe, and Cloud-SSD at `t=1,6,11`.
- `mako_thesis_followup_20260504_234603` waits for the proof sweep to finish, then runs the four-way no-disk sweep and ReplayPool sensitivity sweep.
- Follow logs under `sweep_logs/`, especially `sweep_logs/mako_proof_sweep_20260504_230526.log` and `sweep_logs/mako_thesis_followup_20260504_234603.log`.

## How to Read the Current CSVs

The important columns are:

- `throughput_ops_sec`
- `avg_latency_ms`
- `agg_abort_rate`
- `replay_batch_p1`
- `replay_batch_p2`
- `replay_threads`
- `role_worker_mean`, `role_worker_peak`
- `role_replay_mean`, `role_replay_peak`
- `role_apply_peak`

Those role columns only make sense because the runtime names:

- worker threads as `worker_*`
- replay threads as `replay_*`
- apply thread as `raft_apply` or `paxos_apply`

## Historical Trap to Avoid

Several old docs still preserve one of these outdated stories:

- single-Raft only makes sense as a unified-log win over everything else
- the single apply thread is the entire replay bottleneck story
- `applyLogs()` alone explains the current follower replay path

Do not use those narratives as current truth without checking the code.

## Ground Rules

1. Do not update `doc/thesis/complete_thesis.md` unless explicitly asked.
2. Prefer the current-state docs over migration-era docs when onboarding.
3. If you need to explain current replication behavior, include `ReplayPool`.
4. If you need to explain current experiments, start from `run_scalability_sweep.sh`.
5. If you need to explain single-Raft, distinguish:
   - consensus/apply staging
   - heavy follower replay

## Most Likely Next Tasks

- interpret the existing no-persistence and simulated-persistence sweep outputs
- refine thesis-facing plots and prose
- keep docs aligned with code as the replay-pool and sweep workflow evolve
