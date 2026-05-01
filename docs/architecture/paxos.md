# Paxos Notes

This page documents how Paxos fits into the current repository.

It is **not** the canonical benchmarking guide; for that, use [Benchmark Sweeps](../performance/benchmark-sweeps.md).

## Current Role of Paxos

Paxos remains a supported replication backend alongside Raft.

In the current tree it serves two purposes:

- a working replication backend for Mako
- a comparison baseline against Multi-Raft and Single-Raft in the active sweep scripts

## How Mako Reaches Paxos

The current call path is:

1. Mako transaction code builds serialized log entries.
2. `replication_helper` dispatches to the Paxos implementation.
3. `paxos_main_helper` / `PaxosWorker` bridge those entries into the Paxos server path.
4. Followers replay committed entries and increment replay counters used by the harness.

## Current Experiment Usage

The canonical Paxos runs in this repository use:

- `scripts/run_scalability_sweep.sh --backend paxos`
- `scripts/run_non_persistence_sweep.sh`
- `scripts/run_simulated_persistence_sweep.sh`
- `scripts/overnight_four_way.sh`

The sweep scripts assume a dedicated Paxos build directory such as:

- `build_paxos/`
- `build_paxos_disk/`

## Metrics That Matter

For current sweeps, look at:

- leader-side throughput and latency from the leader log
- `replay_batch_p1` and `replay_batch_p2` from follower logs
- thread-role CPU buckets in `results.csv`

Paxos is therefore measured with the same outer harness shape as the Raft backends.

## Historical Notes

Older microbenchmark commands that reference the legacy WAF flow are still useful as historical context, but they are not the normal path for the current thesis-oriented sweep workflow. Prefer the CMake builds and the sweep scripts under `scripts/`.
