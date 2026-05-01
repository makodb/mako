# Mako Documentation

This repository contains two overlapping bodies of documentation:

- **Current-state docs**: explain what the code and scripts do today.
- **Historical / migration / thesis docs**: useful context, but they may describe intermediate designs that no longer match the checked-in implementation.

If a document conflicts with the current code, treat the code and the active sweep scripts as authoritative.

## Read This First

For a new engineer or agent, use this order:

1. [Architecture Overview](architecture/overview.md)
2. [Replication Current State](architecture/replication-current-state.md)
3. [Benchmark Sweeps](performance/benchmark-sweeps.md)
4. [Transport Backends](developer/transport-backends.md)
5. [User Manual](user-manual.md)

These pages are maintained as the primary source of truth for the current repository.

## Current-State Guides

### Getting Started

- [Introduction](getting-started/introduction.md)
- [Quick Start](getting-started/quickstart.md)
- [Installation](getting-started/installation.md)
- [Concepts](getting-started/concepts.md)
- [Docker Setup](getting-started/docker.md)

### Architecture

- [Architecture Overview](architecture/overview.md)
- [Replication Current State](architecture/replication-current-state.md)
- [Client-Server Architecture](architecture/client-server.md)
- [Multi-Shard Single Process](architecture/multi-shard.md)
- [Speculative 2PC](architecture/speculative-2pc.md)
- [Paxos Notes](architecture/paxos.md)

### Operations and Experiments

- [User Manual](user-manual.md)
- [Benchmark Sweeps](performance/benchmark-sweeps.md)
- [Profiling](performance/profiling.md)
- [CPU Throttling](performance/cpu_throttling.md)
- [Disk Persistence](persistence/disk_persistence.md)

### Developer Guides

- [Development Setup](developer/development.md)
- [Coroutines & Reactor](developer/coroutines.md)
- [Fiber API](developer/fiber-api.md)
- [Transport Backends](developer/transport-backends.md)
- [RPC Overview](rpc/overview.md)
- [RPC API](rpc/api.md)

## Reference Material

- [Mako Book](mako-book.md)
- [Raft Book](raft-book.md)
- [SRPC Book](srpc-book.md)
- [Glossary](reference/glossary.md)
- [Function Dependencies](reference/function-dependencies.md)
- [TPC-C Sharding](reference/tpcc-sharding.md)

## Historical / Use With Caution

These folders are still useful, but many files describe migration plans, intermediate experiments, or abandoned conclusions:

- `docs/migration/**`
- `docs/dev/**`
- `docs/plans/**`
- many `docs/rpc/phase*.md` files
- `doc/thesis/**` except where you are explicitly working on thesis prose

When you need one of these docs, first read:

- [Replication Current State](architecture/replication-current-state.md)
- [Benchmark Sweeps](performance/benchmark-sweeps.md)

Then treat older docs as supporting context rather than canonical behavior.
