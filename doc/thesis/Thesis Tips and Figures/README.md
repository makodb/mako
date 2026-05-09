# Thesis Tips And Figures

This is the active figure and thesis-support folder. Use this folder when
assembling the LaTeX thesis.

## Final Manual Figures

Use the PDF versions in LaTeX. PNG versions are for preview/review.

| Figure | File | Purpose |
| --- | --- | --- |
| 1 | `figures/manual/fig01_mako_baseline_replication.pdf` | Original Mako Multi-Paxos replication path |
| 2 | `figures/manual/fig02_replication_topologies.pdf` | Original Multi-Paxos vs Multi-Raft vs Single-Raft topology |
| 3 | `figures/manual/fig03_preferred_leader_election.pdf` | Preferred leader election for Mako's leader-side submission path |
| 4 | `figures/manual/fig04_single_raft_replay_bottleneck.pdf` | Single-Raft replay bottleneck before ReplayPool |
| 5 | `figures/manual/fig05_replay_pool_pipeline.pdf` | ReplayPool pipeline that keeps Raft apply lightweight |
| 6 | `figures/manual/fig06_simulated_disk_proof_path.pdf` | Simulated-disk proof counters for persistence results |

## Generated Evaluation Graphs

These are the main generated graphs used by the evaluation chapter.

| Graph | File | Purpose |
| --- | --- | --- |
| 7 | `figures/graphs/fig07_no_disk_scalability.pdf` | no-disk throughput and follower replay progress |
| 8 | `figures/graphs/fig08_worker_cpu_utilization.pdf` | worker CPU utilization |
| 9 | `figures/graphs/fig09_replay_pool_matrix.pdf` | ReplayPool sensitivity across worker counts |
| 10 | `figures/graphs/fig10_replay_pool_matrix_heatmap.pdf` | replay completeness heatmap |
| 11 | `figures/graphs/fig11_disk_persistence_throughput.pdf` | no-disk, NVMe, and Cloud-SSD throughput |
| 12 | `figures/graphs/fig12_simulated_disk_proof_cloudssd.pdf` | Cloud-SSD flattening with storage-work proof |
| 13 | `figures/graphs/fig13_cloudssd_bytes_per_txn.pdf` | normalized bytes per committed transaction |

Supporting only:

- `figures/graphs/support_replay_pool_sensitivity_t11.pdf` is the older fixed
  `t=11` ReplayPool slice. The matrix graph is the thesis-facing version.
- `figures/graphs/headline_variance_t11.csv` is supporting stability data, not
  a main-body figure.

## Frozen Data

Use `data_snapshot/` when quoting numbers in the thesis text. It freezes the
CSV and Markdown files used to generate the current graph set, so the prose
does not accidentally drift when newer benchmark runs are added.

## Figure Captions

Suggested captions for the manual figures:

1. Original Mako replication path. Leader-side Mako worker threads execute
   transactions and submit serialized transaction log records through a common
   replication interface. The original Multi-Paxos backend establishes
   committed log order per partition, and follower replicas replay committed
   logs into local database state.
2. Replication topologies evaluated in this thesis. The original Multi-Paxos
   backend uses per-partition replication streams. Multi-Raft preserves that
   Paxos-shaped parallelism with one Raft group per partition, while
   Single-Raft consolidates partition commands into one Raft group per process
   and routes commands by partition identifier.
3. Preferred leader election aligns Raft leadership with Mako's
   leader-oriented submission path. The preferred replica is favored when it is
   healthy and caught up, while ordinary Raft failover remains available.
4. Single-Raft replay bottleneck before ReplayPool. Many Mako worker threads
   can feed one ordered Raft log, but without replay offload the follower-side
   apply and replay path can accumulate backlog. As a result, leader-side
   throughput can look healthy while follower replay progress falls behind
   committed work.
5. Final Single-Raft replay pipeline. The Raft apply path stages committed
   payloads, while ReplayPool workers perform heavy follower-side replay in
   parallel.
6. Source-tagged simulated-disk instrumentation. Persistence writes are
   charged to a simulated disk model and reported as bytes, writes, bytes per
   committed transaction, and source breakdowns, allowing disk throughput
   changes to be explained with measured storage work.

## Drawing Rules

- Put full explanatory captions in LaTeX, not inside images.
- Use PDF for the thesis and PNG only for previews.
- Keep major box titles short and readable.
- Use exact internal names only when they help trace a result to logs or CSV
  columns. For example, use reader-facing labels such as `Raft log writes`,
  with smaller subtitles such as `tag: raft_log`.
- Do not add a figure unless it helps the reader understand a mechanism or a
  result claim.
- Keep Figure 6 terminology as `simulated disk` in the thesis-facing figure.
  Use `FakeDisk` only when referring to implementation names or CSV/log
  counters.

## Thesis Checklist

- The introduction should ask whether Raft can replace Paxos in Mako without
  sacrificing throughput.
- Preferred leader should be credited as a real Mako-specific systems
  contribution.
- Multi-Raft should be presented as the direct Paxos-shaped Raft design.
- Single-Raft should be presented as the consolidation design that needs
  replay-path engineering.
- ReplayPool should be described as the mechanism that separates lightweight
  Raft apply staging from heavy follower replay.
- Disk claims should stay scoped to simulated persistence with measured bytes,
  writes, and bytes per transaction.
- Do not say `1:1` means one Raft group per worker. It means replay workers
  equal Mako worker threads.
- Do not treat leader throughput alone as proof that Single-Raft is healthy;
  follower replay progress must also be checked.

## Useful Guidelines

University formatting PDFs are in `guidelines/`. Keep them here rather than in
the thesis root.
