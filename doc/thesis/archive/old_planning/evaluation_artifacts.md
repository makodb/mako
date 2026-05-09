# Thesis Evaluation Artifacts

This file tracks the experiments and figures needed by
`paper_style_thesis.md`. It is intentionally separate from
`complete_thesis.md`.

## Canonical Metrics

- `throughput_ops_sec`: leader-side committed throughput.
- `role_worker_mean` / `role_worker_peak`: Mako worker CPU.
- `role_replay_mean` / `role_replay_peak`: ReplayPool CPU.
- `role_apply_peak`: Raft/Paxos apply-thread CPU.
- `fake_cluster_total_bytes`: total FakeDisk bytes across parsed logs.
- `fake_cluster_total_writes`: total FakeDisk writes across parsed logs.
- `fake_max_wait_us`: maximum modeled FakeDisk wait seen in the run.

Do not use `replay_batch_p1` or `replay_batch_p2` as leader throughput. They
are follower replay progress counters.

## Required Result Sets

| Evidence | Required command/script | Output |
| --- | --- | --- |
| Main no-disk parity | `scripts/run_non_persistence_sweep.sh` | `results/benchmarks/non-persistence-results/<stamp>/` |
| Disk proof | `scripts/run_focused_disk_proof_sweep.sh` | `results/benchmarks/disk_compare_replay/disk_proof_table.md` |
| Single-Raft bottleneck/fix | `scripts/overnight_four_way.sh` | `results/benchmarks/overnight_four_way_<stamp>/` |
| ReplayPool sensitivity | `scripts/sweep_replay_pool.sh 11 "0 1 2 4 8 11"` | `results/benchmarks/replay_pool_sweep_<stamp>/` |
| Headline variance | `scripts/run_thesis_followup_sweeps.sh` with `RUN_VARIANCE=1` | `results/benchmarks/thesis_headline_variance_<stamp>/` |

## Active/Queued Runs

- The focused disk proof sweep was launched in tmux session
  `mako_proof_sweep_20260504_230526`.
- Follow-up sweeps should wait for that session to finish before binding
  benchmark ports.

## Figure Plan

1. No-disk scalability: Paxos vs Multi-Raft vs Single-Raft+ReplayPool.
2. Four-way no-disk: Single-Raft no-pool vs Single-Raft+ReplayPool vs
   Multi-Raft vs Paxos.
3. ReplayPool sensitivity at `t=11`.
4. Disk proof table/plot: throughput plus FakeDisk bytes/writes.
5. Validation matrix in the thesis body.

## Acceptance Criteria

- Final disk rows have nonzero FakeDisk byte/write counters for disk-enabled
  runs.
- No-disk rows have disabled or zero FakeDisk counters.
- Four-way results include the no-pool Single-Raft baseline.
- ReplayPool sensitivity includes `MAKO_REPLAY_THREADS=0` and `11`.
- Headline graphs use current-commit data or explicitly label historical data.
