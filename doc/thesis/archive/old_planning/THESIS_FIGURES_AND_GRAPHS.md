# Thesis Figures and Graphs Plan

This file is the working plan for diagrams, graphs, and tables that should go
into the final thesis. The goal is to make the thesis read like a systems
paper: every figure should support one claim in the story, not just decorate a
chapter.

The model to follow is the ROLIS and Mako papers:

- Use architecture diagrams early to explain the execution and replication path.
- Use small mechanism diagrams when a design choice is hard to explain in text.
- Use evaluation graphs only when they answer a concrete question.
- Put raw implementation maps, exhaustive test listings, and script references
  in appendices or supporting docs.

## Figure Set

### Figure 1: Mako Baseline Replication Path

**Type:** conceptual architecture diagram

**Thesis claim:** Mako already has a leader-oriented transaction path and a
replication abstraction, which makes Paxos replacement possible.

**What to show:**

- Mako worker threads on the leader process.
- Transaction execution producing replicated transaction logs.
- Original Paxos backend behind the replication interface.
- Followers replaying committed logs.

**Where it belongs:** Background chapter.

**Notes:** This should be simple, similar in spirit to the Mako paper's system
architecture figure. Do not include low-level file names or class names.

### Figure 2: Paxos, Multi-Raft, and Single-Raft Topologies

**Type:** side-by-side topology diagram

**Thesis claim:** Multi-Raft preserves the Paxos-shaped per-partition
parallelism, while Single-Raft consolidates replication into one group per
process.

**What to show:**

- Paxos: partition/worker-facing replication streams.
- Multi-Raft: one Raft group per partition.
- Single-Raft: one Raft group carrying partition IDs inside commands.

**Where it belongs:** Raft backend design chapter.

**Notes:** This is one of the most important diagrams because it explains why
Multi-Raft is the direct baseline and why Single-Raft is a design tradeoff.

### Figure 3: Preferred Leader Election for Mako

**Type:** mechanism diagram

**Thesis claim:** Preferred leader is a real systems contribution because Mako
submits transaction logs through the leader-side process.

**What to show:**

- `localhost` as the preferred leader and transaction-submitting process.
- `p1` and `p2` as followers that can fail over.
- Shorter preferred timeout at startup.
- Failover to non-preferred leader if preferred is unavailable.
- Failback only after the preferred replica catches up.

**Where it belongs:** Preferred leader section.

**Notes:** This should not look like a generic Raft election tutorial. The
motivation must be Mako-specific: worker threads submit work to leaders.

### Figure 4: Single-Raft Bottleneck Before ReplayPool

**Type:** pipeline diagram

**Thesis claim:** A naive Single-Raft design risks serializing follower replay
behind one apply path.

**What to show:**

- Worker threads submit entries.
- Single Raft group orders entries.
- One apply/replay path becomes the narrow stage.
- Follower replay work grows as worker count increases.

**Where it belongs:** Single-Raft and Replay Offload chapter.

**Notes:** Label this as the bottleneck risk or earlier design, not the final
architecture.

### Figure 5: Final ReplayPool Pipeline

**Type:** pipeline diagram

**Thesis claim:** The final design keeps Raft apply lightweight and moves heavy
Mako replay into a parallel pool.

**What to show:**

- Raft commit path.
- `raft_apply` as lightweight staging and callback dispatch.
- copied committed payloads.
- `ReplayPool` workers doing heavy follower replay.
- `1:1` means `MAKO_REPLAY_THREADS = worker_threads`.

**Where it belongs:** Single-Raft and Replay Offload chapter.

**Notes:** This figure prevents a common misunderstanding: the current design
does not make the apply thread do all heavy replay work.

### Figure 6: Experiment Harness and Metrics

**Type:** small measurement diagram or table

**Thesis claim:** The Paxos, Multi-Raft, and Single-Raft comparisons are fair
because they use the same outer harness and metric extraction.

**What to show:**

- Same TPC-C workload.
- Same leader-side throughput extraction.
- Same worker CPU accounting.
- Same follower replay sanity counters.
- Same FakeDisk counters when persistence is enabled.

**Where it belongs:** Evaluation methodology.

**Notes:** This can be a compact table instead of a picture if space is tight.

## Main Evaluation Graphs

### Graph 1: No-Disk Four-Way Scalability

**Question answered:** Does Raft match Paxos throughput without disk
persistence?

**Series:**

- Single-Raft without ReplayPool.
- Single-Raft with 1:1 ReplayPool.
- Multi-Raft.
- Paxos.

**X-axis:** worker threads, `t=1..11`

**Y-axis:** committed throughput, `throughput_ops_sec`

**Input data:**

- `results/thesis_results/01_no_disk_four_way/single_raft_no_pool.csv`
- `results/thesis_results/01_no_disk_four_way/single_raft_1to1_replay_pool.csv`
- `results/thesis_results/01_no_disk_four_way/multi_raft.csv`
- `results/thesis_results/01_no_disk_four_way/paxos.csv`

**Interpretation to write:** This is the headline graph. It should show whether
Raft is in the same performance class as Paxos and whether Single-Raft remains
viable after replay-path engineering.

### Graph 2: Worker CPU Utilization vs Thread Count

**Question answered:** Are Mako worker threads actually busy, or is the
replication layer causing workers to idle?

**Series:**

- Single-Raft with 1:1 ReplayPool.
- Multi-Raft.
- Paxos.
- Optionally Single-Raft without ReplayPool for contrast.

**X-axis:** worker threads, `t=1..11`

**Y-axis:** `role_worker_mean` or `worker_mean_cpu_pct`

**Input data:** same CSVs as Graph 1.

**Interpretation to write:** This graph supports the claim that high throughput
comes from real committed work with busy workers, not from a misleading replay
counter or an idle benchmark harness.

### Graph 3: ReplayPool Sensitivity at t=11

**Question answered:** Why is `1:1` replay parallelism the chosen Single-Raft
configuration?

**Series:** one line for Single-Raft at fixed `worker_threads=11`

**X-axis:** replay threads, `0,1,2,4,8,11`

**Y-axis:** committed throughput, `throughput_ops_sec`

**Input data:**

- `results/thesis_results/02_replay_pool_sensitivity/summary.csv`

**Interpretation to write:** If throughput improves or stabilizes as replay
threads increase, this proves the important bottleneck is the replay path, not
Raft ordering itself.

### Graph 4: Replay and Apply CPU Breakdown

**Question answered:** Does ReplayPool move work away from the apply path?

**Series/options:**

- `role_replay_mean` vs replay thread count.
- `role_apply_peak` vs replay thread count.
- optionally `role_worker_mean` on the same or adjacent panel.

**X-axis:** replay threads

**Y-axis:** CPU percentage

**Input data:**

- ReplayPool sensitivity raw runs under
  `results/thesis_results/02_replay_pool_sensitivity/raw_run/`

**Interpretation to write:** This is a mechanism graph. It should explain why
the ReplayPool fix works rather than only showing throughput.

### Graph 5: Disk Persistence Throughput

**Question answered:** What happens when persistence cost is modeled?

**Series:**

- no disk.
- simulated NVMe.
- simulated Cloud-SSD.

**X-axis:** worker threads, `t=1..11`

**Y-axis:** committed throughput, `throughput_ops_sec`

**Input data:**

- `results/thesis_results/03_disk_persistence/no_disk/`
- `results/thesis_results/03_disk_persistence/nvme/`
- `results/thesis_results/03_disk_persistence/cloudssd/`

**Interpretation to write:** Use this after the FakeDisk proof graph/table. Do
not overclaim that this is a production storage evaluation.

### Graph 6: FakeDisk Bytes and Writes

**Question answered:** Are disk slowdowns backed by measured storage work?

**Series/options:**

- `fake_cluster_total_bytes` by thread count.
- `fake_cluster_total_writes` by thread count.
- `fake_cluster_raft_log_bytes` by thread count.
- `fake_max_wait_us` by thread count, probably as a separate panel.

**X-axis:** worker threads, `t=1..11`

**Y-axis:** bytes, writes, or wait time depending on panel.

**Input data:**

- same disk persistence CSVs as Graph 5.

**Interpretation to write:** This graph is the answer to Shuai's concern. It
shows the replicated/persisted data size and fake-disk work behind the disk
throughput curves.

### Graph 7: Bytes Per Committed Transaction

**Question answered:** Is the persisted data volume roughly proportional to the
amount of committed benchmark work?

**Computation:**

```text
bytes_per_txn = fake_cluster_total_bytes / committed_transactions
```

If the CSV only has throughput, estimate committed transactions as:

```text
committed_transactions = throughput_ops_sec * benchmark_duration_sec
```

**Input data:** disk persistence CSVs plus run duration from logs or harness.

**Interpretation to write:** This turns raw byte counters into a normalized
artifact that is easier to explain in prose.

### Graph 8: Headline Variance / Error Bars

**Question answered:** Are the main conclusions stable across reruns?

**Series:**

- Single-Raft 1:1 at `t=11`.
- Multi-Raft at `t=11`.
- Paxos at `t=11`.
- Cloud-SSD at `t=11` if the run completes cleanly.

**Y-axis:** throughput with mean and standard deviation.

**Input data:**

- `results/thesis_results/04_variance/`

**Interpretation to write:** This does not need to be a big graph. A compact
table with mean, standard deviation, and coefficient of variation may be better.

## Tables

### Table 1: Contributions and Evidence

Map each thesis contribution to one artifact:

| Contribution | Evidence |
| --- | --- |
| Raft backend | end-to-end Paxos/Raft benchmark using same harness |
| Preferred leader | placement/failover tests and design section |
| Multi-Raft baseline | no-disk scalability graph |
| Single-Raft + ReplayPool | four-way graph and ReplayPool sensitivity |
| Persistence proof | FakeDisk bytes/writes table |

### Table 2: Validation Matrix

Compact table covering:

- standalone Raft tests.
- preferred leader tests.
- Mako end-to-end replication tests.
- persistence/FakeDisk instrumentation checks.
- benchmark harness sanity checks.

### Table 3: Disk Proof Summary

Columns:

- backend.
- disk model.
- thread count.
- throughput.
- fake cluster bytes.
- fake cluster writes.
- fake Raft log bytes.
- max fake-disk wait.
- average bytes per fake write.

This table can come from `scripts/disk_proof_table.py` and should be one of
the main pieces of evidence for the persistence section.

## Figures to Avoid in the Body

Avoid these unless the advisor specifically asks for them:

- A full textbook Raft state-machine diagram.
- Method-by-method implementation diagrams.
- Large source-code file maps.
- Every latency percentile from every transaction type.
- Every old May 1 result that predates the current FakeDisk counters.
- Follower replay batch count as a headline throughput graph.

These can go to appendices if useful, but the body should stay claim-driven.

## Final Body Figure Order

Recommended body order:

1. Mako baseline replication path.
2. Paxos vs Multi-Raft vs Single-Raft topology.
3. Preferred leader election for Mako.
4. Single-Raft bottleneck before ReplayPool.
5. Final ReplayPool pipeline.
6. No-disk four-way scalability graph.
7. Worker CPU utilization graph.
8. ReplayPool sensitivity graph.
9. Disk throughput graph.
10. FakeDisk bytes/writes proof graph or table.
11. Validation matrix table.

This is enough for the thesis body. Extra implementation details belong in the
appendix.
