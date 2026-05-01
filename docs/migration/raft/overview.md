# Raft Migration Overview

This file is now a **historical landing page** for the Raft migration material.

The migration itself is no longer the main problem. Raft is implemented, benchmarked, and actively used in the repository. What matters now is distinguishing:

- current code-verified behavior
- older migration plans and intermediate experiment notes

## Read This First

For the current implementation, start with:

1. [Architecture Overview](../../architecture/overview.md)
2. [Replication Current State](../../architecture/replication-current-state.md)
3. [Benchmark Sweeps](../../performance/benchmark-sweeps.md)
4. [Raft Book](../../raft-book.md)

## What This Folder Is Good For

The rest of `docs/migration/raft/` is still useful for:

- understanding how the Paxos-to-Raft bridge was designed
- seeing the helper and callback interfaces in more detail
- tracing older implementation choices
- understanding why certain abstractions exist in the checked-in code

## What This Folder Is Not

Do not treat this folder as the primary source of truth for:

- the current apply vs replay path
- the meaning of the 1:1 replay experiments
- the active benchmark workflow
- the latest conclusions about single-Raft vs multi-Raft vs Paxos

Those are now documented in the current-state docs outside the migration folder.
