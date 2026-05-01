# Raft Status

This page summarizes the **current status of Raft in the repository** and explains how to read the older migration notes in this folder.

## Current Status

Raft is implemented and active in the repository in two layouts:

- **single-Raft**
- **multi-Raft**

It is exercised through the same outer benchmark harness used for Paxos and is no longer a stub or partial integration.

## What Changed Since the Older Migration Notes

Several older files in this folder focused on an intermediate design where:

- follower replay behavior was explained mainly through `applyLogs()`
- single-Raft conclusions were driven by older apply-thread bottleneck experiments
- migration progress was tracked as if Raft were still being wired up feature by feature

The checked-in code has moved past that point.

The biggest architectural change is:

- **heavy follower-side replay now runs in `ReplayPool`**
- in single-Raft, the `raft_apply` thread is mostly a lightweight staging / dispatch thread

## Canonical Current Docs

Use these pages for the current behavior:

- [Architecture Overview](../../architecture/overview.md)
- [Replication Current State](../../architecture/replication-current-state.md)
- [Benchmark Sweeps](../../performance/benchmark-sweeps.md)
- [Raft Book](../../raft-book.md)

## How to Use the Rest of This Folder

Treat the remaining docs here as:

- migration history
- detailed helper/API context
- explanations of why certain abstractions exist

Do not treat them as final conclusions about current performance or replay behavior unless you verify the claim against the code.
