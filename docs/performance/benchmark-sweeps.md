# Benchmark Sweeps

This document explains the benchmark harness and results layout that are currently used for Paxos, Multi-Raft, and Single-Raft.

If you want one file that explains how `results/benchmarks/**` is produced, start here.

## Canonical Harness

The canonical sweep script is:

- `scripts/run_scalability_sweep.sh`

Everything else is a wrapper around it.

Its responsibilities are:

- choose the backend label (`paxos`, `raft-single`, `raft-multi`)
- create the results directory and `scalability_latest` symlink
- launch the appropriate replica roles
- monitor process-level CPU and per-thread CPU
- extract throughput / latency / abort / replay metrics from logs
- write `results.csv` and `summary.txt`

## Inputs That Matter Most

The most important environment or CLI inputs are:

- `BUILD_DIR`
- `BENCH_ROOT_OVERRIDE`
- `MAKO_REPLAY_THREADS`
- `INTER_RUN_SLEEP`
- `MAKO_PERSIST_ROOT`
- `MAKO_PERSIST_FAKE_DISK`
- `MAKO_PERSIST_BW_MBPS`
- `MAKO_PERSIST_LATENCY_US`

## Wrapper Scripts

### `scripts/sweep_single_raft_1to1.sh`

Purpose:

- run single-Raft while setting `MAKO_REPLAY_THREADS=t` for each worker count `t`

This is the current meaning of the "1:1 replay pool" experiment.

Output shape:

- `results/benchmarks/raft-single/single_1to1_<timestamp>/`
- nested per-thread sweep outputs under `per_t/`
- one merged `results.csv` at the wrapper root

### `scripts/run_non_persistence_sweep.sh`

Purpose:

- run the three current no-persistence backends back-to-back:
  - single-Raft 1:1 replay pool
  - multi-Raft
  - Paxos

Output root:

- `results/benchmarks/non-persistence-results/<timestamp>/`

### `scripts/run_simulated_persistence_sweep.sh`

Purpose:

- run the same three backends with persistence-enabled builds and fake-disk throttling

Output root:

- `results/benchmarks/simulated-persistence-results/<disk_label>_<timestamp>/`

### `scripts/overnight_four_way.sh`

Purpose:

- compare four no-disk conditions:
  - single-Raft with no replay pool
  - single-Raft with 1:1 replay pool
  - multi-Raft
  - Paxos

This script is explicitly intended to preserve the old broken single-Raft baseline as a comparison point.

## Build Directory Conventions

The current scripts assume these build roots:

- `build/`: single-Raft, no persistence
- `build_multi/`: multi-Raft, no persistence
- `build_paxos/`: Paxos, no persistence
- `build_disk/`: single-Raft, persistence enabled
- `build_multi_disk/`: multi-Raft, persistence enabled
- `build_paxos_disk/`: Paxos, persistence enabled

## Backend Labels

The harness uses three backend labels:

- `paxos`
- `raft-single`
- `raft-multi`

These labels determine the per-backend results roots under `results/benchmarks/`.

## Key Metrics

### Leader-side metrics

These are extracted from leader logs:

- `throughput_ops_sec`
- `per_core_throughput`
- `avg_latency_ms`
- `avg_persist_latency_ms`
- `agg_abort_rate`

### Follower replay metrics

These are extracted from follower logs:

- `replay_batch_p1`
- `replay_batch_p2`

These counters are the most direct measure of follower replay progress as seen by the scripts.

### CPU metrics

The harness records both:

- process-level CPU
- per-thread CPU

Per-thread logs are bucketed by thread name:

- `worker_*`
- `replay_*`
- `raft_apply`
- `paxos_apply`

The corresponding CSV columns are:

- `role_worker_mean`
- `role_worker_peak`
- `role_replay_mean`
- `role_replay_peak`
- `role_apply_peak`

## Results Layout

### Per-backend direct sweeps

`run_scalability_sweep.sh` writes:

- `results/benchmarks/<backend>/scalability_<timestamp>/`
- `results/benchmarks/<backend>/scalability_latest -> scalability_<timestamp>`

Each output contains:

- `results.csv`
- `summary.txt`
- `throughput_vs_threads.png`
- `logs/`

### Wrapper outputs

Wrappers copy or merge those per-backend sweeps into higher-level directories such as:

- `results/benchmarks/non-persistence-results/<timestamp>/`
- `results/benchmarks/simulated-persistence-results/<timestamp>/`

## Live Workspace Note

In the current workspace snapshot, recent activity is visible under:

- `results/benchmarks/non-persistence-results/`
- `results/benchmarks/simulated-persistence-results/`
- `results/benchmarks/final_sweeps.log`

Those are useful when trying to understand what the latest long-running scripts already produced.

## Operational Notes

- The harness kills old `dbtest` processes before each run.
- It intentionally sleeps between runs to let ports and TCP state drain.
- `pkill -x dbtest` is used instead of broader `pgrep -f` matching to avoid killing the sweep shell itself.
- Persistence sweeps can clean a shared persistence root between runs.

## Recommended Reading After This

- [Replication Current State](../architecture/replication-current-state.md)
- [Architecture Overview](../architecture/overview.md)
