# Thesis TODO - Current Execution Notes

This file tracks the thesis-adjacent work that still needs to happen **without** editing `doc/thesis/complete_thesis.md` yet.

The goal is to keep the implementation state, benchmark workflow, and thesis-writing backlog aligned.

## Current State

The checked-in codebase now reflects this architecture:

- Mako is replication-agnostic at the top level.
- Paxos, Multi-Raft, and Single-Raft all run through the same outer sweep harness.
- Single-Raft no longer relies on one heavy apply thread to perform follower replay directly.
- Heavy follower replay work now runs in `ReplayPool`, sized by `MAKO_REPLAY_THREADS`.
- The current "1:1" experiment means `replay_threads = worker_threads`.

This means older prose that explains single-Raft purely as an apply-thread bottleneck is incomplete for the current repository.

## Current Source of Truth

Use these docs and files before updating thesis notes:

- `docs/architecture/overview.md`
- `docs/architecture/replication-current-state.md`
- `docs/performance/benchmark-sweeps.md`
- `AGENT_HANDOFF.md`
- `src/deptran/raft_main_helper.cc`
- `src/deptran/raft/server.cc`
- `src/mako/mako.hh`
- `src/mako/replay_pool.cc`

## Evidence Already Present in the Workspace

Recent benchmark output is already present under:

- `results/benchmarks/non-persistence-results/`
- `results/benchmarks/simulated-persistence-results/`
- `results/benchmarks/raft-single/`
- `results/benchmarks/raft-multi/`
- `results/benchmarks/paxos/`
- `results/benchmarks/final_sweeps.log`

These directories are the first place to look before deciding a rerun is necessary.

## Current Script Map

The active workflow is:

- `scripts/run_scalability_sweep.sh`
  - canonical one-backend sweep harness
- `scripts/sweep_single_raft_1to1.sh`
  - single-Raft with `MAKO_REPLAY_THREADS=t`
- `scripts/run_non_persistence_sweep.sh`
  - three-way no-persistence comparison
- `scripts/run_simulated_persistence_sweep.sh`
  - three-way simulated-persistence comparison
- `scripts/overnight_four_way.sh`
  - includes the no-pool single-Raft baseline for comparison

## What Is Settled in the Code

1. The repository now has a distinct **apply staging thread** and **replay pool** story in single-Raft.
2. The benchmark harness measures worker, replay, and apply CPU separately.
3. Single-Raft vs Multi-Raft vs Paxos is now discussed through the shared sweep harness, not through isolated ad hoc scripts.
4. The checked-in Raft configs use the new `27xxx` / `28xxx` port ranges.

## Immediate Documentation/Analysis Follow-Through

These items should stay aligned with the codebase:

1. Keep `AGENT_HANDOFF.md` current when the experiment story changes.
2. Keep the docs in `docs/` current; do not let migration-era notes silently become the default explanation.
3. Continue treating `doc/thesis/complete_thesis.md` as intentionally untouched until the thesis rewrite phase.

## Thesis Writing Backlog

### Phase 1 - Consolidate evidence

- Pull the current no-persistence and simulated-persistence numbers from the existing result directories.
- Decide which directories are the canonical inputs for plots and tables.
- Verify that the CSV columns needed for the thesis are present and consistently populated:
  - throughput
  - latency
  - abort rate
  - replay batches
  - replay thread counts
  - role-based CPU metrics

### Phase 2 - Lock the narrative

Before rewriting thesis chapters, explicitly decide which claim the thesis is making about single-Raft:

- is the focus "single-Raft plus replay pool recovers scalability"
- or "single-Raft remains a useful architectural variant but the interesting result is the replay-path decoupling"

That decision should be made from the current result directories, not from older handoff notes.

### Phase 3 - Rewrite thesis prose later

When thesis prose work resumes:

- update `doc/thesis/complete_thesis.md`
- update chapter summaries that still tell the old single-Raft-only story
- make sure prose distinguishes:
  - consensus commit path
  - apply staging
  - heavy replay path

Do not start this phase from older migration docs or old handoff files.

## If New Experiments Are Needed

Only rerun after checking the existing workspace results first.

Most likely reruns:

1. refresh the no-persistence three-way comparison
2. refresh the simulated-persistence three-way comparison
3. use the four-way no-disk sweep only when the no-pool single-Raft baseline is needed explicitly

## Reminder

`doc/thesis/complete_thesis.md` is intentionally not updated here.
