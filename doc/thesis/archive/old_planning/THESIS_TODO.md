# Thesis TODO

This is the working checklist for turning the current implementation and result
set into a paper-style thesis. It is scoped to the thesis rewrite, not general
code cleanup.

## Current Priority

- Keep using `results/thesis_results/` as the clean entry point for result data.
- Let the current resume sweep finish before making final disk and variance
  claims.
- Use `doc/thesis/paper_style_thesis.md` as the planning draft.
- Do not rewrite `doc/thesis/complete_thesis.md` until the final figure set and
  result directories are locked.

## Result Collection

- Confirm the completed no-disk four-way sweep is the canonical no-disk result.
- Confirm ReplayPool sensitivity has all points: `0,1,2,4,8,11`.
- Confirm disk persistence has full `t=1..11` rows for no-disk, NVMe, and
  Cloud-SSD.
- Confirm disk CSVs include nonzero FakeDisk counters for disk-enabled rows.
- Confirm no-disk rows have zero or disabled FakeDisk counters.
- Confirm variance reruns have enough repeated `t=11` rows to compute mean,
  standard deviation, and CV.
- Run `bash scripts/organize_thesis_results.sh` after major result changes.

## Data Sanity Checks

- Use `throughput_ops_sec` as the headline committed-work throughput.
- Do not use `replay_batch_p1` or `replay_batch_p2` as throughput.
- Check `exit_code` is `0` for rows used in final graphs.
- Check worker CPU columns are populated:
  `worker_mean_cpu_pct`, `worker_peak_cpu_pct`, `role_worker_mean`,
  `role_worker_peak`.
- Check replay/apply CPU columns are populated for ReplayPool claims:
  `role_replay_mean`, `role_replay_peak`, `role_apply_peak`.
- For disk claims, check:
  `fake_cluster_total_bytes`, `fake_cluster_total_writes`,
  `fake_cluster_raft_log_bytes`, `fake_max_wait_us`.

## Figures and Graphs

- Create the architecture diagrams listed in
  `doc/thesis/THESIS_FIGURES_AND_GRAPHS.md`.
- Generate the no-disk four-way scalability graph.
- Generate the worker CPU utilization graph.
- Generate the ReplayPool sensitivity graph.
- Generate the replay/apply CPU breakdown graph.
- Generate the disk throughput graph.
- Generate the FakeDisk bytes/writes proof graph or table.
- Create a compact validation matrix table.
- Keep raw implementation maps and exhaustive script details in appendices.

## Writing Tasks

- Rewrite the introduction around one central question:
  can Raft replace Paxos in Mako without sacrificing throughput?
- Keep the background focused on Mako, Paxos, worker threads, and replay.
- Present Multi-Raft as the direct Paxos-shaped Raft design.
- Present Single-Raft as a consolidation design that needed replay-path
  engineering.
- Present ReplayPool as the fix that separates Raft apply staging from heavy
  follower replay.
- Give preferred leader election a full section or strong subsection.
- Explain preferred leader in Mako terms: workers submit to leaders, followers
  replay committed logs.
- Keep persistence claims scoped to simulated persistence with measured
  FakeDisk bytes and writes.
- Move textbook Raft protocol details to the appendix.
- Move source-file inventories to the appendix.

## Preferred Leader Section

- Explain why random Raft leader placement is a problem for Mako's benchmark
  and deployment model.
- Describe startup bias with shorter preferred timeout.
- Describe normal failover when the preferred replica is unavailable.
- Describe safe failback only after the preferred replica catches up.
- State clearly that vote rules, log matching, and commit rules are unchanged.
- Cite tests as validation evidence rather than adding a performance graph.

## Evaluation Writing

- For every graph, write the question it answers before presenting the result.
- After every graph, write the interpretation immediately.
- Separate "throughput result" from "mechanism evidence."
- For no-disk scalability, emphasize fair harness and committed-work metric.
- For ReplayPool, emphasize why `1:1` means `MAKO_REPLAY_THREADS = worker_threads`.
- For disk, explain that Cloud-SSD flattening must be interpreted alongside
  FakeDisk byte/write counters.
- Include limitations: FakeDisk is a model, not a production SSD evaluation.

## Cleanup Before Final Thesis

- Decide final canonical result directories and record them in
  `doc/thesis/evaluation_artifacts.md`.
- Delete or archive stale result links only after the current sweep finishes.
- Regenerate `results/thesis_results/README.md`.
- Make sure every final figure has:
  axis labels, units, source CSV, and one-sentence takeaway.
- Make sure the thesis never says `1:1` means one Raft group per worker.
- Make sure the thesis never says the current `raft_apply` thread performs all
  heavy replay work.

## Done Criteria

- The thesis has a clear story arc from Paxos to Raft to ReplayPool.
- Preferred leader is credited as a real Mako-specific systems contribution.
- The evaluation proves both performance and mechanism.
- Disk results have byte/write proof statistics.
- The final document reads like a systems thesis, not an autogenerated code
  reference manual.
