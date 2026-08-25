# Integrating Raft Consensus into the Mako Distributed Transaction System

## Abstract

This thesis documents the design, implementation, testing, and performance evaluation of a Raft consensus module integrated into the Mako distributed transaction system as an alternative replication backend to its existing Multi-Paxos atomic broadcast layer. The work encompasses a complete Raft protocol implementation with leader election, log replication, and safety guarantees; a novel preferred leader election mechanism that provides deterministic leader placement in geo-replicated deployments while preserving all Raft safety invariants; a runtime-switchable integration architecture that allows operators to choose between Paxos and Raft with a single configuration change; comprehensive standalone and continuous integration test suites; and a performance evaluation comparing both protocols under industry-standard TPC-C workloads.

The key findings are that Raft and Multi-Paxos achieve near-identical throughput in multi-shard configurations where cross-shard coordination dominates the transaction latency, while Multi-Paxos retains a single-shard throughput advantage due to its ability to pipeline consensus instances. Raft offers operational advantages including 25% fewer processes per shard, built-in leader election, and deterministic leader placement through the preferred leader mechanism. Additionally, consolidating multiple per-partition Raft groups into a single Raft instance yields a 51.6% throughput improvement with dramatically reduced variance (CV 1.9% vs 34.6%), by eliminating election interference and reducing heartbeat overhead.

**Author contribution scope**: The Raft module, its integration with Mako, standalone tests, preferred leader election, and the CI test suite were implemented by the author. Mako itself (storage engine, concurrency control, transaction coordination, sharding) is pre-existing infrastructure.

---

## Table of Contents

- [Chapter 1: Introduction](#chapter-1-introduction)
  - [Motivation](#motivation)
  - [Contributions](#contributions)
  - [Document Organisation](#document-organisation)
- [Chapter 2: The Mako System](#chapter-2-the-mako-system)
  - [System Overview](#system-overview)
  - [Core Components](#core-components)
  - [Transaction Flow](#transaction-flow)
  - [Shard Architecture and Replication Topology](#shard-architecture-and-replication-topology)
  - [The Existing Multi-Paxos Path](#the-existing-multi-paxos-path)
  - [Where Raft Fits In](#where-raft-fits-in)
- [Chapter 3: Raft Protocol Implementation](#chapter-3-raft-protocol-implementation)
  - [Raft Fundamentals](#raft-fundamentals)
  - [Mapping the Raft Paper to Implementation](#mapping-the-raft-paper-to-implementation)
  - [Leader Election](#leader-election)
  - [Log Replication](#log-replication)
  - [Commit and State Machine Application](#commit-and-state-machine-application)
  - [Safety Properties](#safety-properties)
  - [Extensions Beyond the Raft Paper](#extensions-beyond-the-raft-paper)
  - [The Transaction Coordinator](#the-transaction-coordinator)
  - [The RPC Layer](#the-rpc-layer)
- [Chapter 4: Preferred Leader Election](#chapter-4-preferred-leader-election)
  - [Motivation](#motivation-1)
  - [Design Overview](#design-overview)
  - [The Three Phases](#the-three-phases)
  - [The Transfer Protocol](#the-transfer-protocol)
  - [Safety Argument](#safety-argument)
  - [Failure Modes and Recovery](#failure-modes-and-recovery)
- [Chapter 5: Integration with Mako](#chapter-5-integration-with-mako)
  - [The Integration Challenge](#the-integration-challenge)
  - [Dispatcher Architecture](#dispatcher-architecture)
  - [The Raft Worker Bridge](#the-raft-worker-bridge)
  - [Lifecycle and Initialisation](#lifecycle-and-initialisation)
  - [Log Submission and Callback Paths](#log-submission-and-callback-paths)
  - [Integration Challenges and Bugs](#integration-challenges-and-bugs)
- [Chapter 6: Testing and Validation](#chapter-6-testing-and-validation)
  - [Standalone Raft Tests](#standalone-raft-tests)
  - [Continuous Integration Test Suite](#continuous-integration-test-suite)
  - [Test Scenarios and Pass Criteria](#test-scenarios-and-pass-criteria)
- [Chapter 7: From Many to One — Consolidating Raft Instances](#chapter-7-from-many-to-one--consolidating-raft-instances)
  - [What Is a Partition?](#what-is-a-partition)
  - [The Problem with Per-Partition Raft Groups](#the-problem-with-per-partition-raft-groups)
  - [The Consolidated Design](#the-consolidated-design)
  - [Experimental Evaluation](#experimental-evaluation)
  - [Interpreting the Results](#interpreting-the-results)
  - [Trade-offs and Applicability](#trade-offs-and-applicability)
  - [Implications for Geo-Replication](#implications-for-geo-replication)
- [Chapter 8: Performance Evaluation](#chapter-8-performance-evaluation)
  - [Benchmark Methodology](#benchmark-methodology)
  - [Single-Shard Results](#single-shard-results)
  - [Multi-Shard Results](#multi-shard-results)
  - [Analysis: Why Paxos Is Faster in Single-Shard Mode](#analysis-why-paxos-is-faster-in-single-shard-mode)
  - [Analysis: Why Throughput Converges in Multi-Shard Mode](#analysis-why-throughput-converges-in-multi-shard-mode)
  - [Replication Batching Behaviour](#replication-batching-behaviour)
  - [Production Deployment Implications](#production-deployment-implications)
  - [Threats to Validity](#threats-to-validity)
- [Chapter 9: Log Persistence and Recovery](#chapter-9-log-persistence-and-recovery)
  - [Persistent Log Storage](#persistent-log-storage)
  - [Crash Recovery](#crash-recovery)
  - [Snapshot Support](#snapshot-support)
- [Chapter 10: Appendix](#chapter-10-appendix)
  - [Glossary](#glossary)
  - [Memory Safety with RustyCpp](#memory-safety-with-rustycpp)

---

# Chapter 1: Introduction

## Motivation

Modern distributed databases must replicate data across multiple servers to provide fault tolerance and high availability. When a server fails, the remaining replicas must continue serving requests without data loss. This requirement is satisfied by consensus protocols — algorithms that ensure a group of servers agree on the same sequence of operations even in the presence of failures.

The choice of consensus protocol has far-reaching implications for a distributed system's performance, operational complexity, and correctness guarantees. Two families of protocols dominate the landscape: Paxos (and its multi-instance variant, Multi-Paxos) and Raft. Both provide the same theoretical safety guarantees — agreement, validity, and termination under majority availability — but they differ significantly in how they structure the protocol, how they handle leader election, and how they constrain log ordering.

Mako is a speculative distributed transaction system designed for high-throughput OLTP workloads with geo-replication. It was built to push the boundaries of transaction throughput by executing transactions speculatively before consensus completes, allowing the system to pipeline work across replication rounds. Mako was originally built with Multi-Paxos as its sole atomic broadcast protocol for replicating committed transactions across servers within each data shard. While Multi-Paxos is a well-understood and performant protocol, there are compelling reasons to support Raft as an alternative:

1. **Understandability**: Raft was explicitly designed to be more understandable than Paxos, with a clearer decomposition into leader election, log replication, and safety sub-problems. This makes the replication layer easier to reason about, debug, and extend. In a complex system like Mako where the replication layer interacts with speculative execution, concurrency control, and cross-shard coordination, understandability is not merely an academic virtue — it directly affects the speed and correctness of development.

2. **Built-in leader election**: Multi-Paxos requires an external leader election mechanism, while Raft integrates leader election directly into the protocol through the RequestVote RPC. This eliminates an entire subsystem from the operational surface area. In production deployments, leader election bugs are among the most dangerous because they can cause split-brain scenarios where two servers both believe they are leader.

3. **Deterministic leader placement**: Standard Raft can be extended with a preferred leader mechanism that provides deterministic leader placement — critical for geo-replicated deployments where data locality affects latency. If a leader is located in a datacenter far from its clients, every read and write operation pays cross-datacenter latency. In a system like Mako with cross-shard transactions, non-deterministic leader placement can cause cascading latency when each shard's leader is in a different datacenter.

4. **Operational flexibility**: Supporting both protocols gives operators the ability to choose the replication backend that best fits their deployment scenario, switching between them with a single configuration change. Different workloads have different characteristics — a single-shard deployment with high-frequency transactions may benefit from Multi-Paxos's pipelining, while a multi-shard deployment with cross-shard coordination may prefer Raft's simplicity and resource efficiency.

5. **Resource efficiency**: Raft's all-voter topology (every replica participates in consensus) eliminates the need for a separate learner replica, reducing the process count by 25% compared to the Paxos topology (which uses 3 voters plus 1 learner per shard). In large deployments with hundreds of shards, this translates directly to infrastructure cost savings.

This thesis documents the design and implementation of a complete Raft consensus module integrated into Mako, along with a novel preferred leader election mechanism, comprehensive testing infrastructure, and a comparative performance evaluation against the existing Multi-Paxos implementation. The work demonstrates that Raft and Multi-Paxos achieve near-identical throughput in multi-shard configurations (where cross-shard coordination dominates), while Multi-Paxos retains an advantage in single-shard configurations due to its ability to pipeline consensus instances. Importantly, Raft achieves this performance parity with significantly fewer resources and simpler operational characteristics.

## Contributions

The contributions of this work are:

1. **A complete Raft implementation** integrated into a production-grade distributed transaction system, including leader election, log replication, batch optimisation, and persistent state management.

2. **A preferred leader election mechanism** that provides deterministic leader placement while preserving all five Raft safety properties. This includes asymmetric election timeouts, a background leadership transfer monitor, and a piggybacked transfer protocol.

3. **A runtime-switchable integration architecture** that allows Mako to use either Multi-Paxos or Raft as its atomic broadcast layer, selectable at startup with no code changes. The dispatcher pattern ensures the rest of the system is agnostic to the replication protocol.

4. **A comprehensive test suite** including 11 standalone Raft correctness tests (election, agreement, fault tolerance, concurrency, and the Figure 8 scenario from the Raft paper) and a continuous integration pipeline with single-shard, multi-shard, simple transaction, and replication correctness tests.

5. **A comparative performance evaluation** under TPC-C workloads showing that Raft achieves equivalent throughput to Multi-Paxos in multi-shard configurations while using 25% fewer processes, with Multi-Paxos retaining a single-shard advantage due to pipelining.

## Document Organisation

This thesis is organised as follows:

- **Chapter 2** describes the Mako system architecture, its core components, transaction flow, and shard topology — providing the context needed to understand where the Raft module fits.

- **Chapter 3** covers the Raft protocol implementation in detail: the core consensus algorithm, how it maps to the original Raft paper, extensions made for the Mako integration, and the RPC layer.

- **Chapter 4** presents the preferred leader election mechanism — its motivation, three-phase design, transfer protocol, and safety argument.

- **Chapter 5** describes how Raft was integrated into Mako alongside the existing Paxos path: the dispatcher architecture, the bridge between Mako's transaction layer and Raft's consensus layer, and the practical challenges encountered.

- **Chapter 6** covers the testing infrastructure: standalone Raft correctness tests and the CI integration test suite.

- **Chapter 7** describes the consolidation of multiple per-partition Raft instances into a single Raft instance, explaining the motivation, design, and the dramatic performance improvements it achieves.

- **Chapter 8** presents the performance evaluation: methodology, results, and analysis comparing Raft and Multi-Paxos under TPC-C workloads.

- **Chapter 9** describes the persistent log storage subsystem, crash recovery, and snapshot support.

- **Chapter 10** provides a glossary of terms and a description of the memory safety approach using RustyCpp annotations.

---

# Chapter 2: The Mako System

## System Overview

Mako is a speculative distributed transaction system with geo-replication, designed for high-throughput OLTP workloads. It is the system described in the OSDI'25 paper and builds upon the Janus codebase (OSDI'16: "Consolidating Concurrency Control and Consensus for Commits under Conflicts"). Mako extends Janus with speculative execution and a high-performance in-memory storage engine.

To understand the significance of Mako's design, it helps to consider the fundamental tension in distributed transaction systems. On one hand, transactions must be serialisable — every replica must apply the same operations in the same order — which requires consensus across geographically distributed replicas with non-trivial network latency. On the other hand, high throughput demands that the system not sit idle waiting for consensus to complete before starting new work. Mako resolves this tension through speculative execution: it begins processing subsequent transactions before the replication of previous transactions has completed, rolling back speculatively executed work only if the consensus outcome differs from what was predicted.

The key properties of Mako are:

- **Speculative execution**: Transactions execute optimistically before consensus completes, allowing work to proceed in parallel with replication. When consensus confirms the expected outcome, the speculative work is committed. When consensus reveals a conflict, the speculative work is rolled back and retried. In well-partitioned workloads with low contention, the vast majority of speculative executions succeed, making this approach highly efficient.

- **Geo-replication**: Data is replicated across geographically distributed datacenters for fault tolerance and availability. Each shard's replicas are spread across datacenters, and the consensus protocol ensures that a majority of replicas agree on every committed operation. This means the system can survive the loss of an entire datacenter without data loss.

- **Sharding**: Data is horizontally partitioned across multiple shards for scalability, with each shard independently replicated. Sharding allows the system to scale by adding more shards, each with its own replication group and consensus instance. Cross-shard transactions are coordinated through Two-Phase Commit (2PC), which adds latency but is necessary for multi-shard atomicity.

- **Pluggable replication**: The atomic broadcast layer is abstracted behind a unified interface, allowing different consensus protocols to be swapped at runtime. This is the architectural property that makes the work in this thesis possible — by designing the replication interface as a plug point, the Raft module can be integrated without modifying any of the upper layers.

## Core Components

Mako's architecture consists of several major components that work together to process distributed transactions:

```
+------------------------------------------------------------------+
|                        Mako System                                |
|                                                                   |
|  +-------------+    +---------------+    +-------------------+    |
|  |   Client    |--->| Transaction   |--->|   Transaction     |   |
|  | (benchmark) |    |  Coordinator  |    |    Scheduler      |   |
|  +-------------+    +-------+-------+    +--------+----------+   |
|                             |                     |               |
|                    +--------v---------+  +--------v----------+   |
|                    |   Communicator   |  |  Masstree Storage |   |
|                    | (RPC to replicas)|  |  (in-memory index)|   |
|                    +--------+---------+  +-------------------+   |
|                             |                                     |
|              +--------------v-----------------+                   |
|              |    Atomic Broadcast Layer       |                  |
|              |  (Multi-Paxos  OR  Raft)        |                  |
|              +--------------------------------+                   |
+------------------------------------------------------------------+
```

**Storage Engine: Masstree.** Mako uses Masstree, a high-performance in-memory trie/B-tree hybrid, as its primary index structure. Masstree provides lock-free reads via optimistic version validation, fine-grained concurrency control at the leaf level, and efficient range scans and point lookups. This combination makes it well-suited for the high-concurrency OLTP workloads that Mako targets.

**Concurrency Control: OCC.** Mako uses Optimistic Concurrency Control (OCC) as its default transaction isolation mechanism. Transactions execute speculatively, reading and writing to local state without acquiring locks. At commit time, a validation phase checks for conflicts — if validation succeeds, changes are committed; otherwise the transaction aborts and retries. This optimistic approach maximises concurrency when conflicts are rare, which is typical in well-partitioned OLTP workloads.

**Atomic Broadcast Layer.** The atomic broadcast layer ensures total order of committed operations across all replicas within a shard. This is the layer where consensus protocols operate. Mako originally supported only Multi-Paxos; this thesis adds Raft as an alternative. Both protocols implement the same abstract interface, allowing runtime switching with a single configuration change.

**Sharding.** Data is horizontally partitioned across shards. Each shard holds a subset of the keyspace (for example, a range of TPC-C warehouses), has its own independent replication group of leader and follower replicas, runs its own consensus instance per partition, and can be placed on a different set of machines. Cross-shard transactions use Two-Phase Commit (2PC) coordinated by the transaction coordinator.

**Protocol Factory.** Mako uses a factory pattern to create protocol-specific components. Each protocol (OCC, 2PL, Paxos, Raft, and others) registers a factory subclass that creates the appropriate coordinator, scheduler, communicator, and RPC services. This architecture enables protocol polymorphism — the upper layers of the system need not know which consensus protocol is in use.

### Speculative Execution and the Replication Layer

Mako's speculative execution model has a direct and important relationship with the replication layer. In a traditional (non-speculative) system, the transaction processing pipeline is strictly sequential: execute → replicate → apply → respond. Each step must complete before the next begins, and the system sits idle during replication.

Mako breaks this sequential dependency by executing subsequent transactions speculatively while the replication of previous transactions is in flight. The speculation is based on the prediction that most transactions will succeed (no OCC conflicts, no 2PC aborts). When this prediction is correct — which it is in the vast majority of cases for well-partitioned workloads — the system achieves significantly higher throughput by overlapping computation with replication.

This speculation creates a subtle requirement for the replication layer: it must be able to handle a backlog of uncommitted entries without blocking the transaction pipeline. If the replication layer cannot keep up with the submission rate, the backlog grows and eventually the system must stall. This means the replication layer's throughput ceiling directly bounds the system's speculative execution throughput. In the single-shard performance analysis (Chapter 7), the replication layer is precisely this ceiling — Multi-Paxos's pipelining allows a deeper submission backlog without stalling, explaining its throughput advantage.

The speculation also affects the application callback path. When the replication layer commits an entry and invokes the application callback, Mako must reconcile the committed result with the speculatively executed result. If they match (the common case), the speculative work is simply confirmed. If they differ (a conflict was detected after speculative execution began), the speculative work is rolled back and the transaction is retried. The efficiency of this reconciliation process — how quickly the system detects and resolves mismatches between speculative and committed state — is another factor that affects end-to-end throughput.

## Transaction Flow

Understanding how transactions flow through the system is essential for understanding where the replication layer fits — and why it can be swapped without affecting the rest of the system.

A transaction in Mako proceeds through the following stages:

1. **Dispatch**: The client (typically a benchmark driver) submits a transaction to the transaction coordinator. The coordinator analyses the transaction to determine which shards hold the data being accessed. For example, a TPC-C NewOrder transaction accessing warehouse 3 and warehouse 7 would touch two shards. The coordinator dispatches transaction "pieces" — sub-operations that read or write data on a single shard — to the appropriate shards.

2. **Communication**: The communicator sends the transaction pieces via RPC to the target shards. Each shard has a leader replica that handles writes, and the communicator must know which replica is the current leader for each shard. Leader discovery is handled by view tracking — each server maintains a view of which replica is the leader for each partition.

3. **Execution**: The transaction scheduler on each shard executes the pieces against the local Masstree storage, performing reads and writes. Under OCC, this execution is speculative — the pieces execute without acquiring locks, reading the current values and buffering writes. At commit time, OCC validates that no conflicting writes have occurred since the reads. If validation fails, the transaction aborts and retries.

4. **Atomic Broadcast**: The leader submits the committed log entry to the replication layer. This is the **only step where Paxos and Raft differ** — the call is dispatched to the appropriate implementation through the unified replication interface. The consensus protocol ensures that the committed entry is replicated to a majority of the shard's replicas before it is considered durably committed. This step is the focus of this thesis.

5. **Application**: Once a log entry is committed (replicated to a majority), a callback is invoked to apply the committed entry to the state machine on each replica. On the leader, application confirms the transaction result to the coordinator. On followers, application replays the committed entry to bring the follower's state into consistency with the leader. Both leaders and followers use the same callback interface, which is a key design simplification.

6. **Coordination**: For cross-shard transactions (those that touch data on multiple shards), the coordinator runs a Two-Phase Commit (2PC) protocol:
   - **Prepare phase**: The coordinator sends a Prepare message to each participating shard. Each shard votes to commit or abort based on whether its local piece succeeded.
   - **Commit/Abort phase**: If all shards vote to commit, the coordinator sends a Commit message. If any shard votes to abort, the coordinator sends an Abort message. Each shard then applies or rolls back its local changes.

   The 2PC protocol adds significant latency to cross-shard transactions because it requires a round-trip to all participating shards on top of the per-shard replication latency. As shown in Chapter 8, this coordination latency dominates in multi-shard configurations, making the choice of replication protocol less significant.

7. **Response**: The client receives the commit or abort result from the coordinator.

The critical observation is that steps 1-3 and 5-7 are identical regardless of which replication protocol is in use. Only step 4 differs between Paxos and Raft. This clean separation is what makes the pluggable replication architecture possible. The replication layer is a black box to the rest of the system: it accepts log entries, replicates them, and invokes callbacks when they are committed. Whether that replication happens via Multi-Paxos or Raft is invisible to the upper layers.

## Shard Architecture and Replication Topology

Mako partitions data by shard and further subdivides each shard into partitions. In a TPC-C deployment, each shard handles a set of warehouses, and each shard has multiple worker threads, each handling one partition. Each partition has its own independent consensus instance — this is important to understand: it is not one Raft group per shard, but one per partition.

Each shard has a replication group of replicas spread across datacenters. The topology differs between the two protocols:

- **Multi-Paxos topology**: 4 replicas per shard — 3 voting acceptors plus 1 non-voting learner. The learner receives committed entries for backup and read scaling but does not participate in consensus rounds.

- **Raft topology**: 3 replicas per shard — all voting. There is no separate learner role; all replicas participate in consensus and receive entries as part of the normal AppendEntries protocol.

This topology difference means Raft uses 25% fewer processes per shard, which has implications for resource consumption, CPU contention, and operational complexity. Both topologies provide the same fault tolerance (tolerating 1 replica failure out of 3 voters), since quorum in both cases requires 2 out of 3 voters.

Cross-shard transactions introduce additional latency through the 2PC protocol. Even on localhost, the coordination overhead (~10ms round-trip for Prepare + Commit across shards) dominates over the sub-millisecond replication latency. This has important implications for the relative performance of Raft and Paxos, as discussed in Chapter 8.

## The Existing Multi-Paxos Path

Before Raft was added, Mako used Multi-Paxos as its sole atomic broadcast protocol. Multi-Paxos is a well-known variant of the Paxos consensus algorithm optimised for the common case where a stable leader exists. It operates on a sequence of numbered slots, each representing one consensus instance. The key architectural properties of the Multi-Paxos implementation are:

- **Per-instance parallelism**: Multi-Paxos can process multiple consensus instances simultaneously. While instance N is in the Accept phase, instance N+1 can already be in the Prepare phase. This pipelining allows the leader to overlap network round-trips across instances, which is a significant throughput advantage in high-frequency workloads.

- **Slot-based state management**: Each slot tracks its own ballot numbers and accepted/committed commands independently.

- **Separate learner role**: The learner receives committed entries asynchronously and does not add latency to the commit path.

The Paxos initialisation sequence creates worker processes, registers RPC handlers, establishes connections to replicas, starts the submit queue, and begins leader heartbeats. This pattern is mirrored by the Raft integration, which follows a similar multi-phase boot sequence (see Chapter 5).

## Where Raft Fits In

The replication layer is abstracted behind a unified API that dispatches to either the Paxos or Raft implementation at runtime. The architecture looks like this:

```
                    Unified Replication API
                          |
            +-------------+-------------+
            |                           |
     Paxos Implementation       Raft Implementation
            |                           |
     PaxosWorker                  RaftWorker
            |                           |
     PaxosServer                  RaftServer
```

The dispatch is controlled by a single global variable that stores the current replication type. This can be set through three mechanisms:

1. **Command-line flag**: Specifying `--replication raft` at startup.
2. **Automatic detection**: Scanning the YAML configuration file for `ab: raft` in the mode section.
3. **Programmatic API**: Setting the replication type directly in code.

The key functions dispatched through this interface include lifecycle operations (setup, shutdown), log submission, callback registration for leader and follower events, and epoch management. This design ensures that the rest of the Mako system — the storage engine, concurrency control, transaction coordination, and benchmarking infrastructure — is completely agnostic to the choice of replication protocol.

---

# Chapter 3: Raft Protocol Implementation

## Raft Fundamentals

Raft is a consensus protocol designed by Ongaro and Ousterhout (2014) to be more understandable than Paxos while providing equivalent safety guarantees. The Raft paper explicitly identifies understandability as a first-class design goal, arguing that a protocol that is easier to understand is easier to implement correctly and extend for real-world systems. This design philosophy has practical consequences: the protocol is structured around a strong leader model where all decisions flow through a single authoritative server, making the control flow easier to reason about compared to Paxos's more symmetric design.

The strong leader model means that clients always interact with the leader, and the leader is solely responsible for deciding the order of log entries. This eliminates a class of concurrency issues that arise in leaderless or multi-leader protocols. However, it also means that the leader is a potential bottleneck — all writes must pass through the leader, and the leader must communicate with a majority of followers for every committed entry.

Raft decomposes the consensus problem into three sub-problems:

1. **Leader election**: At most one leader per term; all other servers are followers. The leader handles all client requests and coordinates log replication.
2. **Log replication**: The leader receives commands from clients, appends them to its log, and replicates the log entries to followers.
3. **Safety**: If a log entry has been committed (replicated to a majority), no future leader will have a different entry at that log index.

Each server is in one of three states at any time:

```
                  timeout, start election
            +-----------------------------------+
            |                                   |
            v          receives majority        |
        +-----------+    votes won    +--------+------+
 start  |           |--------------->|                |
------->| Follower  |               |   Candidate    |
        |           |<--------------|                |
        +-----------+  discovers    +--------+------+
            ^          current leader        |
            |          or new term           |
            |                               |
            |     discovers server with     |
            |        higher term            |
            |                               v
            |                       +-----------+
            +-----------------------|           |
                                   |  Leader   |
                                   |           |
                                   +-----------+
```

The protocol uses **terms** — monotonically increasing integers that act as logical clocks. Each term begins with an election. A server's current term is the highest term it has seen, and any message carrying a higher term causes the server to update its term and revert to follower state. This term mechanism is fundamental to Raft's safety: it ensures that stale leaders from previous terms cannot interfere with the current leader's operation.

## Mapping the Raft Paper to Implementation

The implementation closely follows the Raft paper, mapping its core concepts to a layered class architecture:

- **The Raft server** implements the core state machine, handling leader election, log replication, and state transitions. It inherits from the base transaction log server, which provides the application callback for feeding committed entries back into Mako's transaction processing pipeline. This inheritance relationship is the fundamental integration point between Raft consensus and Mako's upper layers.

- **The Raft communicator** handles all outgoing RPC communication — sending AppendEntries for replication, broadcasting vote requests during elections, and sending leadership transfer signals.

- **The RPC service** handles incoming RPCs by dispatching them to the appropriate server methods.

- **The protocol factory** creates all Raft-specific components (server, communicator, coordinator, RPC service) and manages their lifecycle and ownership.

- **The transaction coordinator** is the entry point for client command submission, handling the interface between the transaction layer and the consensus layer.

The state maintained by the implementation maps directly to the Raft paper's specification:

**Persistent state** (must survive restarts): The current term, the candidate voted for in the current term (at most one vote per term for election safety), and the replicated log entries. These are optionally persisted to a storage backend (such as RocksDB) so that a restarted server can recover its state.

**Volatile state** (on all servers): The commit index (highest log index known to be replicated on a majority) and the execute index (highest log index applied to the state machine, always less than or equal to the commit index).

**Volatile state** (on leaders only): For each follower, the next index (the next log entry to send) and the match index (the highest log index known to be replicated on that follower). These are used to track replication progress and compute the commit index.

## Leader Election

Leader election is the mechanism by which Raft ensures that exactly one server acts as the authoritative coordinator of log replication at any given time. The leader is responsible for accepting client commands, appending them to the log, and replicating them to followers. When the leader fails, a new leader must be elected quickly and safely — quickly to minimise downtime, and safely to ensure that no committed data is lost during the transition.

The leader election mechanism follows the Raft paper with dynamic timeout tuning to support the preferred leader extension (detailed in Chapter 4). Understanding the election mechanism in detail is important because the quality of leader election directly affects system availability: faster elections mean shorter periods where the system cannot process writes.

**Election trigger.** Every server runs an election timer. If a follower does not receive communication from a valid leader within its election timeout, it transitions to candidate state and begins an election. The timeout is randomised to prevent repeated split votes — if all servers used the same timeout, they would all start elections simultaneously and split the vote indefinitely. The randomisation is achieved by choosing a timeout uniformly at random from a range (for example, 500ms to 1000ms for standard replicas). This means that when a leader fails, one follower will typically time out before the others and start an election, giving it a head start in collecting votes.

**The election process.** When a server becomes a candidate, it executes a carefully ordered sequence of operations:

1. **Increment term**: The candidate moves to a new term, signalling that it is attempting to become leader in a new epoch. Terms are the fundamental mechanism by which Raft distinguishes between different leader regimes.
2. **Vote for self**: The candidate votes for itself. This is critical — without the self-vote, the candidate would need votes from all other servers rather than from a majority minus one.
3. **Persist state**: The new term and vote are persisted to stable storage before any messages are sent. This ordering is essential for safety: if the server crashes after sending vote requests but before persisting its vote, it could vote for a different candidate after restart, potentially creating two leaders in the same term.
4. **Broadcast vote requests**: The candidate sends vote request messages to all other servers in the cluster. Each request contains the candidate's term, the candidate's log length and last log term (for the up-to-date check), and the candidate's identifier.
5. **Wait for quorum**: The candidate waits for responses. It wins if it receives a majority of votes. It loses if a majority rejects it or the election times out.

**Vote granting.** When a server receives a vote request, it evaluates two conditions before granting its vote:

- **Term check**: The candidate's term must be at least as high as the server's current term. If the candidate has a lower term, the vote is rejected outright.
- **Log up-to-date check**: The candidate's log must be at least as up-to-date as the voter's log. "Up-to-date" is determined by comparing the term of the last log entry (higher term wins) and, if terms are equal, the length of the log (longer log wins). This check ensures that only candidates with all committed entries can win elections — a critical safety property.

Additionally, each server grants at most one vote per term. If a server has already voted for a different candidate in the same term, it rejects the request. However, it will re-grant a vote to the same candidate (idempotency), which handles RPC retries safely.

When a voter grants a vote, it resets its election timer. This prevents the voter from immediately starting its own election, which could interfere with the candidate that just received its vote.

**Quorum detection.** The implementation uses an event-based quorum tracker that tallies vote responses as they arrive. A candidate needs votes from a strict majority (more than half the cluster). For a 3-node cluster, this means 2 votes (including the candidate's self-vote). If a majority is reached, the candidate becomes leader. If a majority reject or the election times out, the candidate returns to follower state and a new election will eventually begin.

The quorum tracker also monitors the highest term seen in any vote response. If a voter responds with a term higher than the candidate's, it means a more recent election has occurred and the candidate is stale. The candidate immediately updates its term to the higher value and reverts to follower state. This ensures that a candidate from a stale term does not disrupt a cluster that has already elected a more recent leader.

The event-based design (as opposed to a polling-based design) means that the quorum decision is made as soon as the last needed vote arrives, with no polling delay. This is important for minimising election duration: in a system where leader absence causes writes to stall, even a few milliseconds of unnecessary delay in the quorum decision adds directly to the unavailability window.

**Leader state initialisation.** When a candidate wins an election and becomes leader, it must initialise the volatile leader state before sending any heartbeats. Specifically, it resets the next index for each follower to one past its own last log index (optimistically assuming followers are up to date) and resets the match index for each follower to zero (conservatively assuming nothing has been confirmed).

This initialisation must happen before the first heartbeat is sent. A subtle bug was discovered during development where the heartbeat loop could start before the leader state was fully initialised, causing the leader to send AppendEntries messages with stale next indices from a previous leadership period. This manifested as followers rejecting entries they should have accepted, causing unnecessary log reconciliation and temporary throughput loss. The fix was to ensure that leader state initialisation is part of the state transition (in the method that sets the leadership flag), not part of the heartbeat loop's setup, guaranteeing that the state is ready before any heartbeat is sent.

**Split vote handling.** Split votes occur when no candidate receives a majority — for example, in a 5-node cluster where two candidates each receive 2 votes. Raft handles this through randomised election timeouts: after a failed election, each server waits a random interval before trying again, making it statistically unlikely that the same split occurs repeatedly. The probability of consecutive split votes decreases geometrically: with random timeouts in a range of [T, 2T], the probability that two servers choose timeouts within a small interval decreases with each round. In practice, split votes are resolved within one or two rounds.

**Term advancement.** Terms advance in several situations: when a follower's election timer fires, when a candidate or leader discovers a message with a higher term, or when a candidate receives a vote reply indicating a higher term. The invariant is that terms only increase — no server ever decreases its term. This monotonicity is fundamental to Raft's safety.

Every point in the code where a message is received checks the message's term against the server's current term. If the message carries a higher term, the server immediately updates its term, clears its vote (since the vote was for the old term), and reverts to follower state. This "term supremacy" rule ensures that stale servers — those that were partitioned and continued operating in an old term — are quickly brought into line when they rejoin the cluster. The check is applied uniformly to all message types (vote requests, vote responses, AppendEntries, and heartbeats), eliminating any possibility of a stale server interfering with the current leader's operation.

## Log Replication

Log replication is the core of Raft's operation — it is the mechanism by which the leader ensures that all servers in the cluster maintain identical copies of the replicated log. The replicated log is the source of truth for the system: every committed entry in the log represents an operation that has been agreed upon by a majority of servers and will never be lost or contradicted.

The design of log replication has profound implications for throughput and latency. Each new command requires at least one round-trip between the leader and a majority of followers before it can be committed. The leader must track the replication progress of every follower individually, handle diverged logs from previous terms, and batch entries efficiently to amortise per-entry overhead. The implementation makes several design choices beyond the basic Raft paper to optimise for Mako's high-throughput workload.

**Command submission.** When a client submits a command, the leader appends it to its local log, assigns it the next sequential log index and the current term, persists the entry if a storage backend is configured, and signals the replication loop that new entries are available. The index assignment is atomic — no two entries can receive the same index — which is guaranteed by the leader's exclusive access to the log append path.

**The replication loop.** The leader runs a background loop that continuously sends log entries to followers. For each follower, the leader:

1. Determines which entries the follower needs (based on the next index tracked for that follower)
2. Prepares an AppendEntries message containing the entries, the term and index of the entry immediately preceding the new entries (for consistency checking), and the leader's current commit index
3. Sends the message and processes the response

**Follower processing.** When a follower receives an AppendEntries message, it performs several checks:

- **Term check**: If the leader's term is less than the follower's current term, the message is rejected. The follower is aware of a more recent term and this leader is stale.
- **Consistency check**: The follower verifies that it has an entry at the "previous log index" with the matching "previous log term." This check implements the Log Matching Property — if the follower doesn't have the expected entry, it means the follower's log has diverged from the leader's and reconciliation is needed.
- **Entry appending**: If the consistency check passes, the follower appends the new entries to its log, overwriting any conflicting entries at those indices (entries from previous terms that were never committed).
- **Commit index update**: The follower advances its commit index to the minimum of the leader's commit index and the index of the last new entry.

**Commit advancement on the leader.** The leader determines which entries are committed by examining the match indices of all followers. An entry at index N is committed when a majority of servers (including the leader) have that entry in their logs. The implementation computes this by sorting the match indices and taking the median value — the middle value in a sorted list of N values represents the point at which a majority has replicated.

Critically, the leader only commits entries from the current term. This is the "term safety rule" from Figure 8 of the Raft paper: a leader cannot retroactively commit entries from previous terms by counting replicas, because doing so could violate the Leader Completeness property. Instead, once an entry from the current term is committed, all preceding entries are implicitly committed as well.

**State machine application.** Once an entry is committed, it is applied to the state machine through the application callback. The implementation applies entries strictly in order, from the execute index up to the commit index. For each entry, the callback feeds the committed command back into Mako's transaction processing pipeline. Both leaders and followers execute this application path — the only difference is timing (leaders learn about commits first through quorum calculation; followers learn through the leader's commit index in subsequent AppendEntries messages).

An important design decision is that the implementation releases the Raft mutex before calling the application callback. This allows concurrent AppendEntries processing to continue while the state machine is applying entries, which is critical for throughput. However, it requires careful handling to avoid races — an "apply pending" flag is used to ensure that application calls are not re-entered.

**Log reconciliation.** When a follower's consistency check fails (because its log has diverged from the leader's), the leader must reconcile the logs by finding the point where they agree and resending everything after that point. This situation arises when a server was partitioned from the leader and accepted entries from a different leader in a previous term, or when a server was offline and missed entries.

Standard Raft, as described in the paper, handles this by decrementing the next index by one on each rejection and retrying. This approach is simple and correct but can be very slow: if the follower's log has diverged by N entries, it takes N round trips to find the agreement point. In a high-throughput system like Mako that processes tens of thousands of entries per second, N can be large, making linear backoff unacceptable for production use.

This implementation uses a three-tier optimisation that reduces the worst-case reconciliation from O(N) to O(log N) round trips:

1. **Fast backoff**: When a follower rejects an AppendEntries message, it reports its last log index in the response. If this index is less than the leader's next index for that follower, the leader jumps directly to one past the follower's last index. This handles the common case where the follower is simply behind (it missed entries while offline) rather than diverged (it has conflicting entries from a different term). Fast backoff resolves the vast majority of reconciliation cases in a single round trip.

2. **Exponential backoff**: If the fast path doesn't apply (the follower has entries the leader doesn't expect, indicating a true divergence) and the next index is large (greater than 10), the leader halves the next index. This binary-search-like approach reduces O(N) to O(log N) round trips for long diverged logs. Each halving step eliminates half the remaining search space.

3. **Linear backoff**: For small next indices (10 or less), the leader decrements by one to find the exact agreement point. Linear backoff at small indices avoids the risk of exponential backoff overshooting past the beginning of the log, which would require special error handling.

This three-tier strategy is a significant practical improvement over the paper's approach. In a system processing 100,000 entries per second, a server that was offline for 10 seconds would have a 1,000,000-entry gap. Standard Raft would need up to 1 million round trips to reconcile; the fast backoff path resolves this in a single round trip (because the follower simply reports its last index). Even in the worst case of a true divergence, exponential backoff resolves a million-entry divergence in about 20 round trips.

The reconciliation process also handles the important edge case of **stale entries from previous terms**. When a follower receives entries from a new leader that conflict with entries it accepted from a previous leader (entries that were never committed), the follower overwrites the conflicting entries. This is safe because uncommitted entries are not guaranteed to be durable — only committed entries (replicated to a majority) have that guarantee. The follower trusts the current leader's log as authoritative, which is correct because the current leader must have won an election with a log that is at least as up-to-date as a majority of servers.

**Heartbeats.** Even when there are no new entries to replicate, the leader sends periodic empty AppendEntries messages (heartbeats) to maintain its authority and prevent followers from starting elections. The heartbeat interval is shorter than the minimum election timeout, ensuring that a healthy leader always refreshes followers' election timers before they expire.

### The Heartbeat Loop as the Replication Engine

The heartbeat loop is the central nervous system of the Raft leader — it is far more than a simple keep-alive mechanism. In the implementation, the heartbeat loop is a continuously running background coroutine that handles all outgoing replication, commit advancement, and follower state tracking. Understanding its architecture is essential for understanding the system's throughput characteristics.

**Event-driven design.** Rather than polling on a fixed timer, the heartbeat loop uses an event-driven design with a bounded wait. The loop sleeps on an event object with a timeout equal to the heartbeat interval. This has two operating modes:

- *Idle mode*: When no new entries are pending, the event times out after the heartbeat interval and the loop sends empty heartbeats to all followers. This maintains the leader's authority at minimum cost.
- *Active mode*: When a new log entry is submitted, the submit path signals the event, waking the heartbeat loop immediately. This means that new entries are replicated within microseconds of submission, rather than waiting for the next heartbeat interval. The result is that replication latency is bounded by network round-trip time, not by heartbeat interval.

This event-driven approach is a significant optimisation over a naive timer-based design. In a system processing thousands of transactions per second, a fixed heartbeat interval of even 50ms would introduce unacceptable latency. The event-driven approach reduces replication latency to the minimum achievable while still providing heartbeat functionality during idle periods.

**Per-follower replication with independent tracking.** The heartbeat loop iterates over all followers and handles each independently. For each follower, the leader:

1. Computes the set of entries to send (from the follower's next index to the leader's last log index)
2. Retrieves the previous log index and previous log term for the consistency check
3. Sends the AppendEntries message asynchronously
4. Processes the response with a bounded timeout (500ms to prevent a slow follower from blocking heartbeats to other followers)

This per-follower independence is critical for availability. If one follower is slow or partitioned, the leader continues sending heartbeats and entries to the other followers without delay. A design that waited for all followers to respond before sending the next round would allow a single slow follower to stall the entire cluster.

**Mutex management.** The heartbeat loop follows a carefully designed lock acquisition pattern to balance correctness and concurrency:

1. *Acquire the mutex* at the start of each iteration to read consistent state (term, log, match indices)
2. *Release the mutex* before sending RPCs to avoid holding the lock during network operations
3. *Re-acquire the mutex* after receiving responses to update state (match indices, next indices, commit index)
4. *Release the mutex* before applying committed entries to the state machine

This release-before-RPC pattern is essential for throughput. If the mutex were held during network round-trips (which can take milliseconds even on localhost), all other Raft operations (incoming AppendEntries from other partitions, client submissions, election timer checks) would block. The lock is only held during brief state reads and updates, never during I/O.

**Commit index advancement within the loop.** After processing all follower responses, the heartbeat loop recomputes the commit index. The algorithm sorts the match indices of all servers (including the leader's own last log index) and takes the median value. The median of N values is the highest value that at least ⌈N/2⌉ servers have reached — exactly the majority threshold required for commitment.

However, advancing the commit index requires an additional safety check: the entry at the new commit index must be from the current term. This is the term safety rule from the Raft paper (Figure 8). Without this check, a newly elected leader could advance the commit index to include entries from a previous term that are on a majority of servers but were never committed. Those entries might later be overwritten by a different leader in the same term range, violating Leader Completeness. By requiring that only current-term entries advance the commit index, and relying on the fact that committing a current-term entry implicitly commits all preceding entries, this safety hole is closed.

A further defensive measure caps the commit index at the leader's own last log index. While theoretically the median of match indices should never exceed the leader's log (since the leader includes its own log length in the computation), this cap provides an additional safety margin against edge cases in the sorted array computation.

**Response handling.** When the leader receives a response to an AppendEntries message, it handles four cases:

1. *Lost RPC* (timeout with no response): The leader does nothing — the next heartbeat iteration will retry. This is the most common failure mode in real networks and is handled entirely by the heartbeat loop's natural retry behaviour.
2. *Higher term in response*: The follower has a higher term than the leader, meaning a new election has occurred. The leader steps down immediately — it is no longer the authoritative coordinator.
3. *Log conflict* (follower rejected the consistency check): The leader applies the three-tier backoff strategy described above to find the agreement point and retry.
4. *Success*: The leader advances the follower's match index and next index, bringing the follower closer to (or at) the leader's current state.

**Batch optimisation integration.** The heartbeat loop supports two replication modes that can be selected at compile time:

- *Non-batched mode*: Each AppendEntries message contains a single log entry. This is simpler but generates more RPCs under high load.
- *Batched mode*: Multiple log entries are packed into a single batch command. The leader collects all entries from a follower's next index through the leader's last log index and wraps them in a batch structure. This dramatically reduces RPC count — instead of N RPCs for N entries, a single RPC suffices. Followers unpack the batch and persist all entries in a single I/O operation.

The batched mode is the default for production use. The batch size adapts naturally to the system's throughput: under high load, more entries accumulate between heartbeat iterations, resulting in larger batches with better amortisation. Under low load, batches are small (often single entries), adding negligible overhead.

## Commit and State Machine Application

The commit and application process deserves special attention because it is the point where Raft consensus connects to Mako's transaction processing. It is the handoff point between the consensus layer (which ensures agreement) and the application layer (which actually processes transactions). Getting this handoff right is critical for both correctness and performance.

**The commit point.** A log entry is considered committed when it has been replicated to a majority of servers and the leader has advanced its commit index to include that entry. Once committed, the entry is permanent — no future leader will ever have a different entry at that index. This permanence is what makes the commit point the right place to trigger application: the system can safely apply the entry's command to the state machine, knowing it will never need to be rolled back.

**Application callback.** When the commit index advances, the Raft server invokes the application callback registered by the upper layer. This callback receives the slot index and the committed command, and applies it to the local state machine. On the leader, application means the transaction result can be returned to the client — the transaction is durably committed. On followers, application means the follower's state is brought into consistency with the leader's.

The application callback is the same interface regardless of whether Paxos or Raft is the underlying protocol. Both protocols invoke it when entries are committed, which is what allows the rest of the Mako system to be agnostic to the replication backend. This clean callback interface is a key enabler of the pluggable replication architecture.

**Ordering guarantee.** Entries are applied in strict log order — from execute index +1 through the commit index. This ordering is essential for determinism: all servers apply the same commands in the same order, producing identical state. If entries were applied out of order, different servers could reach different states even with the same committed log.

**Concurrency design.** An important design decision is that the implementation releases the Raft mutex before calling the application callback. This is a deliberate trade-off:

- *Why release the lock*: If the mutex were held during application, incoming AppendEntries messages from the leader would block until application completes. For long-running applications (such as applying a TPC-C transaction with multiple index lookups), this could significantly reduce replication throughput. Releasing the lock allows the replication loop to continue processing concurrently with application.

- *Why this is safe*: An "apply pending" flag prevents re-entrant calls to the application path. When the lock is released, the flag is set. If another thread finds the flag set, it knows that application is already in progress and defers. The applicator thread re-acquires the lock after each entry and checks whether additional entries have been committed in the meantime, looping until all committed entries are applied.

- *Why this matters for Mako*: In Mako, the application callback feeds committed commands into the transaction processing pipeline, which involves Masstree index operations, OCC validation, and potentially cross-shard coordination. These operations are not instantaneous, so holding the Raft mutex during application would create a significant bottleneck.

**Garbage collection.** As the log grows, old entries that have been applied by all servers can be removed. The implementation tracks a minimum active slot (the lowest index that any server might still need) and periodically removes entries below this watermark. This prevents unbounded memory growth in long-running deployments. The garbage collection threshold is set conservatively to ensure that slow followers can still catch up before their needed entries are removed.

## Safety Properties

The implementation preserves all five safety properties from the Raft paper:

**Election Safety**: At most one leader can be elected in a given term. This is enforced by the single-vote-per-term rule: each server persists its vote and will not grant a second vote to a different candidate in the same term.

**Leader Append-Only**: A leader never overwrites or deletes entries in its log; it only appends new entries. The command submission path only appends to the end of the log — there is no code path that modifies existing leader entries.

**Log Matching**: If two logs contain an entry with the same index and term, then all preceding entries are identical. This is enforced by the consistency check in the AppendEntries handler: before accepting new entries, the follower verifies that its entry at the previous log index has the expected term.

**Leader Completeness**: If a log entry is committed in a given term, that entry will be present in the logs of all future leaders. This is enforced by the log up-to-date check in vote granting: a candidate cannot win an election unless its log is at least as up-to-date as a majority of servers, which guarantees it has all committed entries.

**State Machine Safety**: If a server has applied a log entry at a given index, no other server will ever apply a different entry at that index. This follows from Leader Completeness — since every leader has all committed entries, it will never propose a different entry at a committed index. The execute index only advances forward, ensuring applied entries are never un-applied.

## Extensions Beyond the Raft Paper

Several extensions beyond the standard Raft protocol were implemented to support the Mako integration:

**Batched log replication.** Instead of sending entries one at a time as described in the paper, the implementation batches multiple log entries into a single AppendEntries RPC. The leader collects all entries from a follower's next index through the leader's last log index and packs them into a single batch command. This dramatically reduces RPC overhead and improves throughput, especially under high-frequency workloads. Followers persist the entire batch in a single I/O operation for efficiency.

**Integration with Two-Phase Commit.** Unlike standalone Raft implementations that replicate arbitrary byte strings, this implementation replicates transaction commit records. The log entries contain serialised transaction commands that are fed back into Mako's transaction processing pipeline through the application callback. The coordinator handles the WRONG_LEADER error case by redirecting clients to the current leader using view tracking data.

**Persistent state management.** The implementation optionally persists Raft state to a RocksDB backend. After every term change or vote grant, the new term and vote are written to stable storage. When the leader appends new entries, they are persisted before acknowledgement. The commit index is persisted when it advances. On restart, the server recovers its state from storage and replays committed entries through the application callback to restore the state machine.

## The Transaction Coordinator

The Raft transaction coordinator is the entry point for client command submission. It bridges the gap between Mako's transaction layer, which wants to submit a command and receive a commit notification, and the Raft consensus layer, which requires the command to be replicated to a majority before commitment.

The coordinator operates through a phase state machine:

1. **Submission**: The coordinator receives a command from the transaction layer.
2. **Leader check**: If this replica is not the leader, the coordinator returns a WRONG_LEADER error with the current view data (which leader to contact). This allows the transaction layer to redirect the command.
3. **Append**: If this is the leader, the command is submitted to the Raft server for local append and replication.
4. **Wait for commitment**: The coordinator waits for the entry to be committed (replicated to a majority). It monitors both the commit index and the current term — if the term changes (indicating a leadership change), the submission is aborted.
5. **Notification**: Once committed, the coordinator notifies the transaction layer of the result.

An important design consideration is **slot allocation**. When the Raft server appends an entry, it assigns a slot ID (log index) using a shared mutable cell. This allows the coordinator to know which slot to wait for without a race condition between append and monitoring.

## The RPC Layer

The Raft module uses a four-layer RPC architecture for inter-node communication:

1. **RPC definitions**: The protocol defines four RPCs — RequestVote for elections, AppendEntries for log replication, EmptyAppendEntries for heartbeats (with an additional trigger_election_now flag for leadership transfer), and TimeoutNow for direct leadership transfer.

2. **The communicator**: The outgoing RPC layer maintains connections to all peers and provides high-level methods for sending messages. It handles connection management, proxy creation, and response tracking. The communicator supports both synchronous and asynchronous RPC patterns.

3. **The RPC service**: The incoming RPC layer dispatches received messages to the appropriate handler methods on the Raft server. It uses a macro-based code generation system to eliminate boilerplate for RPC handler registration.

4. **The protocol factory**: The factory creates all components, manages their ownership relationships, and registers RPC services with the server framework. Each partition gets its own set of Raft components (server, communicator, service), maintaining independence between partitions.

The RPC layer also provides **disconnection simulation** for testing. In standalone tests, individual server connections can be programmatically disconnected and reconnected, allowing tests to simulate network partitions and verify correct behaviour during failures.

---

# Chapter 4: Preferred Leader Election

## Motivation

Standard Raft makes no guarantee about which replica becomes leader. After a leader failure, any follower can win the election depending on random timeouts and network conditions. While this is sufficient for correctness, it is suboptimal for systems like Mako that operate in geo-replicated environments.

The problems with non-deterministic leader placement are:

1. **Data locality**: The leader handles all client reads and writes. If the leader is located far from the data or the clients, every operation incurs cross-datacenter latency, significantly degrading performance.

2. **Cross-shard coordination**: Mako runs distributed transactions across shards. If each shard's leader is in a different datacenter, every cross-shard operation requires cross-datacenter communication for the 2PC protocol, multiplying latency.

3. **Operational control**: Operators want to place leaders on specific machines for capacity planning, maintenance windows, and resource isolation. Non-deterministic placement makes this impossible.

The preferred leader mechanism solves these problems by ensuring that a designated replica becomes the leader whenever possible, while still allowing any replica to serve as leader during failures. The key design goals are:

- **Deterministic placement**: The configured preferred replica becomes leader within seconds of startup or recovery.
- **Automatic failover**: If the preferred replica fails, standard Raft election selects an alternative leader with no special handling required.
- **Automatic failback**: When the preferred replica recovers and catches up, leadership automatically transfers back to it.
- **Safety preservation**: All five Raft safety properties are maintained — no data loss, no split-brain, no relaxed consistency.

## Design Overview

The preferred leader extension modifies Raft in carefully targeted ways. It changes election timing and adds a leadership transfer protocol, but does not touch any of the core consensus mechanisms:

| Aspect | Standard Raft | Preferred Leader Extension |
|--------|--------------|---------------------------|
| Election timeout | Same for all replicas | Asymmetric: shorter for preferred, longer for others |
| Leader selection | First to timeout wins | Preferred wins startup races; non-preferred leaders transfer back |
| Leader failover | Random follower wins | Any follower can still win (safety preserved) |
| Leader failback | Not supported | Automatic via monitoring thread and transfer protocol |
| Post-election behaviour | Leader stays indefinitely | Non-preferred leaders actively monitor and transfer |

Critically, the following standard Raft mechanisms are **not** modified: the log replication protocol, vote granting rules (term check, log up-to-date check), the commit rule (majority replication in current term), the Leader Completeness property, and the AppendEntries consistency check. The preferred leader extension is purely an optimisation of leader placement, not a change to the consensus algorithm.

## The Three Phases

The preferred leader system operates in three phases:

### Phase 1: Startup — Election Bias

At startup, all replicas begin as followers. The preferred replica is configured during initialisation (typically the local replica for data locality). The system biases elections toward the preferred replica using **asymmetric election timeouts**:

- **Preferred replica**: 150-300ms timeout (short — wins election races)
- **Non-preferred replicas during startup grace period** (first 5 seconds): 1-2 second timeout (very long — lets the preferred win)
- **Non-preferred replicas after grace period**: 500ms-1 second timeout (medium — enables failover detection)

The grace period is essential for reliable startup behaviour. At startup, all replicas boot simultaneously. Without the grace period, a non-preferred replica with a slightly shorter random timeout could win before the preferred replica even finishes initialising. The 5-second grace period ensures the preferred replica has time to start up and win the first election.

After the grace period, non-preferred replicas reduce their timeout to 500ms-1 second. This is still longer than the preferred replica's 150-300ms, maintaining the bias, but short enough to detect leader failures within a reasonable time for failover.

### Phase 2: Monitoring — Non-Preferred Leader Detection

When a non-preferred replica wins an election (for example, because the preferred replica was down during a failure), it becomes a "non-preferred leader." This triggers a background monitoring thread that checks periodically (every second) whether the preferred replica is alive and caught up.

The monitoring thread evaluates several conditions before initiating a transfer:

1. This replica must still be the current leader
2. This replica must not be the preferred leader
3. A preferred leader must be configured
4. No transfer must already be in progress
5. The preferred replica must exist in the cluster
6. **The preferred replica must be caught up** — its match index must be greater than or equal to the current commit index

The "caught up" check (condition 6) is the critical safety condition. Transferring leadership to a replica that has not received all committed entries would violate Leader Completeness and could lose data. The monitoring thread does not initiate a transfer until the preferred replica has provably received every committed entry.

A minimum stability time (500ms) is enforced after becoming leader before any transfer is considered, preventing oscillation when multiple leadership changes occur in rapid succession.

### Phase 3: Transfer — Piggybacked Protocol

When all conditions are met, the transfer protocol executes:

```
Non-Preferred Leader              Preferred Follower           Other Follower
       |                                |                          |
  [ShouldTransfer? YES]                 |                          |
       |                                |                          |
  [Set transferring flag]               |                          |
       |                                |                          |
  [Send heartbeat with                  |                          |
   transfer signal to ALL] ------------>|                          |
       |                         [Reset timer]                     |
       | -------------------------------------------------------->|
       |                                |                   [Reset timer]
       |                                |                   [Do nothing]
  [Wait 20ms]                           |                          |
       |                         [Wait 30ms]                       |
  [Step down to follower]               |                          |
       |                         [Start election]                  |
       |                                |                          |
       |<--- Vote request --------------|---- Vote request ------->|
       |                                |                          |
  [Grant vote]                          |                   [Grant vote]
       |--- Vote reply --------------->|                          |
       |                                |<----- Vote reply --------|
       |                                |                          |
       |                         [Won election!]                   |
       |                         [Become leader]                   |
       |                                |                          |
       |<---- Heartbeat ----------------|--- Heartbeat ----------->|
```

The transfer uses a **piggybacked approach**: instead of a separate RPC, the transfer signal is embedded in the regular heartbeat message via a `trigger_election_now` flag. This has two key advantages:

1. **Atomic notification**: All replicas learn about the transfer simultaneously through the same heartbeat.
2. **Timer reset**: The heartbeat resets non-preferred replicas' election timers, preventing them from starting competing elections.

When replicas receive the transfer signal:
- The **preferred replica** waits a brief interval (30ms), then starts an election.
- **Non-preferred replicas** log the event and do nothing — their election timers were just reset by the heartbeat.

The 30ms delay before the preferred replica starts its election gives the old leader time to step down and for all replicas to process the heartbeat (resetting their timers). This prevents election storms.

In addition to the piggybacked approach, a standalone **TimeoutNow RPC** is also implemented. This RPC directly instructs a specific replica to start an election immediately. It handles various edge cases: stale requests, servers that are already leaders, servers that are already candidates, and servers that are shutting down. The TimeoutNow RPC is available for operational use but the primary transfer mechanism uses the piggybacked approach for its atomicity guarantees.

**Transfer timing.** The total transfer time is approximately 40ms on a local network. Each timing value was chosen carefully to prevent race conditions:

- **T=0ms**: Non-preferred leader sends heartbeat with transfer signal to all replicas simultaneously.
- **T=0-5ms**: Heartbeat arrives at all replicas, resetting their election timers and delivering the transfer signal.
- **T=20ms**: Old leader steps down to follower state. The 20ms delay ensures that the heartbeat has been received by all replicas before the old leader stops responding. Without this delay, a replica that hasn't received the heartbeat yet might start its own election (because the old leader stopped sending heartbeats before the transfer signal arrived).
- **T=30ms**: Preferred replica starts election. The 30ms delay (10ms after the old leader steps down) ensures that the old leader is no longer acting as leader when the preferred replica's RequestVote messages arrive. Without this delay, the old leader might reject the vote request because it still believes it is leader.
- **T=35-40ms**: Vote round completes. Since all other replicas' election timers were just reset by the heartbeat (at T=0-5ms), they will not start competing elections. The preferred replica is the only candidate, so it wins quickly.
- **T=40ms**: Preferred replica becomes leader and begins sending heartbeats.

The total transfer downtime (the period where no leader is serving requests) is approximately 20ms — from T=20ms (old leader steps down) to T=40ms (new leader begins serving). This is well below the typical election timeout (150-300ms for the preferred replica), ensuring that the transfer is faster than a standard election.

## Safety Argument

The preferred leader extension preserves all five Raft safety properties:

**Election Safety** (at most one leader per term): The preferred leader mechanism only affects election *timing* through asymmetric timeouts, not election *rules*. A candidate still needs a majority of votes to win. The transfer signal causes the preferred replica to start an election, but it must still collect votes using the standard RequestVote protocol. No server grants its vote to two candidates in the same term.

**Leader Append-Only** (leaders never overwrite or delete their entries): The transfer mechanism does not modify any logs. The old leader simply steps down. The new leader appends entries normally.

**Log Matching** (same index and term implies identical prefix): The transfer mechanism does not modify logs. The transfer signal is carried in a heartbeat message that contains no log entries. Normal AppendEntries consistency checks continue to apply.

**Leader Completeness** (committed entries are present in all future leaders): The monitoring thread only initiates a transfer when the preferred replica's match index is greater than or equal to the commit index — meaning the preferred replica has all committed entries. Combined with the standard Raft vote restriction (candidates must have an up-to-date log), the preferred replica will win elections and maintain all committed entries.

**State Machine Safety** (applied entries are never contradicted): This follows from Leader Completeness. Since the preferred replica has all committed entries before becoming leader, it will never commit a different entry at any previously committed index.

Beyond the five standard properties, the transfer protocol provides additional guarantees:

- **No dual leadership**: The old leader steps down after sending the transfer signal. Even if the transfer fails, the old leader is now a follower and will not accept writes.
- **No lost writes**: The caught-up check ensures all committed entries are replicated to the preferred replica before transfer.
- **No election storms**: The piggybacked approach resets all non-preferred replicas' election timers simultaneously, and the 30ms delay prevents competing elections.
- **Bounded transfer time**: If the preferred replica does not win the election within its timeout, normal Raft election proceeds. The system does not block indefinitely.

## Failure Modes and Recovery

**Preferred replica fails.** Non-preferred replicas' election timers fire (500ms-1s), normal Raft election proceeds, and any follower can win. The system continues serving requests. When the preferred replica recovers, it receives heartbeats, replicates the leader's log, and once caught up, the non-preferred leader transfers back.

**Non-preferred leader fails during transfer.** The transfer signal may have been sent but the old leader crashes before stepping down. If the preferred replica wins the election it started, it becomes leader normally. If it doesn't win, another replica's election timeout fires and normal election proceeds. No data loss occurs because committed entries are on a majority by definition.

**Preferred replica is slow or partitioned.** The monitoring thread checks that the preferred replica is caught up before initiating transfer. If the preferred is slow, its match index lags behind the commit index, and the transfer simply does not happen. The system continues operating with the non-preferred leader indefinitely until the preferred catches up.

**Configuration change.** The preferred leader can be changed at runtime. If the current leader becomes non-preferred due to a configuration change, the monitoring thread starts immediately and will initiate a transfer once the newly preferred replica is caught up.

---

# Chapter 5: Integration with Mako

## The Integration Challenge

Integrating a new consensus protocol into an existing distributed transaction system is substantially more complex than implementing the protocol in isolation. A standalone Raft implementation — even one that passes all the standard correctness tests — is only the beginning. The real engineering challenge is making it work within the constraints of an existing system that was designed around a different protocol.

To appreciate the difficulty, consider the gap between a textbook Raft implementation and what Mako needs:

- A textbook Raft implementation replicates opaque byte strings. Mako needs to replicate structured transaction commit records that feed back into a transaction processing pipeline.
- A textbook implementation runs in isolation. Mako needs Raft to coexist with Multi-Paxos in the same binary, switchable at runtime.
- A textbook implementation has a single log per cluster. Mako runs one consensus instance per partition, with potentially dozens of partitions per shard.
- A textbook implementation handles its own lifecycle. Mako has a multi-phase boot sequence that must be mirrored exactly.
- A textbook implementation doesn't interact with 2PC. Mako's cross-shard transactions require the Raft layer to coordinate with the 2PC coordinator, handling leader redirection and epoch management.

The key challenges are:

1. **Interface compatibility**: The new protocol must expose the same interface as the existing one — same lifecycle operations, same submission semantics, same callback mechanisms — so the upper layers don't need modification. Even a small semantic difference (for example, different ordering of callback invocations) can cause subtle bugs in the upper layers that only manifest under specific workloads.

2. **Coexistence**: Both protocols must coexist in the same binary, with runtime switching. This means shared build infrastructure (both must compile and link without conflicts), non-conflicting RPC definitions (Raft's RPCs must not collide with Paxos's), and a clean dispatch mechanism that routes calls correctly based on a single configuration flag.

3. **State synchronisation**: The Raft module's notion of committed entries, leader identity, and epoch must map correctly onto Mako's concepts of watermarks, partition leaders, and transaction epochs. Mako uses watermarks to track how far the state machine has progressed, and these watermarks must be updated correctly by both the Raft and Paxos paths.

4. **Lifecycle management**: Initialisation, shutdown, and failure recovery must work correctly for the Raft path, following the same patterns as the Paxos path. Mako's boot sequence involves multiple phases (server creation, RPC registration, connection establishment, heartbeat startup, submit loop activation), and the Raft integration must replicate this sequence exactly.

5. **Error handling**: The Raft path must handle the same error conditions as the Paxos path — leader failures, network partitions, slow followers, stale terms — and surface them through the same error codes and recovery mechanisms that the upper layers expect.

## Dispatcher Architecture

The integration is built around a dispatcher pattern that routes replication calls to the correct implementation at runtime.

**The replication type.** A single global state variable stores the current replication type (Paxos or Raft). This uses a thread-safe interior mutability pattern that allows reads and writes without locking for single-word values.

**The dispatch macro.** A macro-based dispatch mechanism routes each replication API call to the appropriate namespace. When the replication type is set to Raft, calls are dispatched to the Raft implementation namespace; when set to Paxos, they go to the Paxos implementation namespace. This macro expands at each call site, so there is no virtual dispatch overhead — just a conditional branch on the replication type.

**Protocol detection.** The system detects which protocol to use through two mechanisms:

1. An explicit command-line flag that directly sets the replication type.
2. Automatic detection from the YAML configuration file: the system scans for `ab: raft` or `ab: multi_paxos` in the mode configuration and sets the replication type accordingly.

**The unified API.** The dispatcher exposes a unified replication API covering:

- **Lifecycle functions**: Setup, secondary setup (for service launch and preferred leader configuration), and shutdown.
- **Log submission functions**: Submit, add_log, and variants for different queuing strategies.
- **Callback registration**: Registering callbacks for leader events (commit notifications) and follower events (replayed entry notifications), including watermark-based callbacks for production use.
- **Epoch and election functions**: Getting and setting the current epoch, registering leader election callbacks, and sending no-op entries for epoch synchronisation.
- **Network and benchmark functions**: Client connection management, leader discovery, and benchmark callbacks.

**Namespace symmetry.** The Paxos and Raft implementation namespaces mirror each other's function signatures, ensuring the dispatch macro works for all API functions. However, there are structural differences in how some operations are implemented. For example, Raft registers callbacks directly with each Raft server instance, while Paxos uses a global callback table. These differences are hidden behind the common interface.

## The Raft Worker Bridge

The Raft Worker is the central bridge between Mako's transaction layer and Raft's consensus layer. There is one worker per partition, and each worker owns a complete Raft protocol stack: the server, communicator, RPC service, and associated state.

The worker performs several key functions:

**Setup chain.** The worker initialises in multiple phases:

1. **Base setup**: Creates the protocol factory, which in turn creates the Raft server, communicator, coordinator, and executor.
2. **Service setup**: Starts the RPC server, registering Raft's RPC handlers with the network layer so the server can receive incoming messages from peers.
3. **Communication setup**: Establishes outgoing RPC connections to all peers in the cluster. This is done after service setup to ensure peers can respond.
4. **Heartbeat setup**: Starts the control-plane heartbeat mechanism for peer liveness detection.
5. **Post-setup**: Ensures all peers have completed setup (via a barrier), then starts the background submit thread.

**Log submission pipeline.** Client commands flow through a queuing system:

1. The upper layer calls the submit function, which places the command in a pending log queue.
2. A background submit loop continuously drains this queue, sending accumulated entries to the Raft server for consensus.
3. The submit loop supports batching: it collects multiple pending entries and submits them together for efficiency.
4. Graceful shutdown drains any remaining entries before stopping the loop.

**Two-role callback architecture.** A distinctive design decision in the Raft worker is the separation of committed entry callbacks into leader and follower roles. Rather than a single callback that handles all committed entries regardless of role, the worker maintains separate callback registrations for leader events and follower events:

- *Leader callbacks* are invoked when the leader commits entries that it originally proposed. On the leader, commitment means the transaction result can be returned to the client. Leader callbacks trigger the transaction coordinator's commit notification path, which unblocks the client.
- *Follower callbacks* are invoked when a follower applies entries that it received from the leader. On followers, application means the follower's state machine is being brought into consistency with the leader's. Follower callbacks trigger the follower's replay path, which updates the local Masstree index.

This role-aware callback separation enables Mako to optimise differently for leaders (where the priority is notifying the client quickly) and followers (where the priority is efficient batch replay). A single-callback design would force the application layer to check the server's role on every committed entry, adding overhead on the hot path.

The callback determines the server's current role dynamically at invocation time rather than at registration time. This handles the case where a server's role changes between registration and invocation — for example, a leader that steps down while entries are being applied. The role check is a simple boolean flag read, adding negligible overhead.

**Command encoding and protocol evolution.** The log entries replicated by Raft are not arbitrary byte strings — they are structured transaction commit records that must be deserialised and fed back into Mako's transaction processing pipeline. The encoding scheme reveals an interesting aspect of iterative protocol evolution.

The Mako codebase originally supported only the Janus transaction protocol, where log entries contained structured transaction "pieces" with typed key-value pairs. When the Raft module was integrated, the command encoding had to be compatible with this existing structure while also supporting Mako's own serialised transaction format.

The solution uses a multi-layer wrapping structure: Mako's raw serialised transaction bytes are wrapped in a transaction commit command structure (which includes metadata like the transaction ID and term), which is in turn serialised using the existing marshalling infrastructure. On the receive side, the worker's callback function detects the data format (Mako's serialised bytes vs. legacy key-value pairs) and routes to the appropriate deserialisation path. This dual-format support allows the Raft module to work with both the standalone test infrastructure (which uses simple key-value pairs) and the full Mako transaction pipeline (which uses serialised transaction records).

This encoding design is a pragmatic concession to backward compatibility. A clean-sheet design would use a single encoding format, but the need to support both standalone testing and production use with minimal code changes made the dual-format approach the practical choice.

**Threading and concurrency model.** The Raft worker uses a coroutine-based concurrency model inherited from Mako's fiber framework. The key concurrent activities within each worker are:

- *The heartbeat loop*: A long-running coroutine that handles outgoing replication and commit advancement (described in Chapter 3).
- *The election timer*: A long-running coroutine that monitors the election timeout and triggers elections when the leader is unresponsive.
- *The submit loop*: A background thread that drains the pending log queue and feeds entries to the Raft server.
- *Incoming RPC handlers*: Coroutines spawned by the RPC framework to handle incoming AppendEntries, RequestVote, and other messages.
- *The leadership transfer monitor*: An OS thread (not a coroutine) that periodically checks whether leadership should be transferred to the preferred replica.

The use of OS threads for the leadership transfer monitor (rather than coroutines) is a deliberate design choice. Coroutines in the fiber framework are cooperatively scheduled — they only yield at explicit yield points. The monitor thread needs to sleep for a full second between checks, and blocking a coroutine for a full second would prevent other coroutines from running on the same thread. An OS thread can sleep without affecting other coroutines.

Shutdown of these concurrent activities is coordinated through a stop flag that all activities check periodically. When the flag is set, each activity completes its current operation and exits. A brief delay after setting the stop flag allows all activities to notice the flag before the worker's destructor runs, preventing use-after-free bugs when a coroutine accesses object state after the destructor has collapsed the virtual function table.

**Leadership queries.** The worker provides methods to query leadership status for a partition, check whether a partition exists on this server, and receive notifications when leadership changes. These are used by the upper layer to route client requests to the correct leader. Leadership change notifications propagate up through a callback chain: the Raft server notifies the worker, the worker notifies the helper layer, and the helper layer notifies the upper Mako layer. This chain ensures that all layers have a consistent view of which replica is the current leader for each partition.

## Lifecycle and Initialisation

The Raft lifecycle within Mako follows a sequence that mirrors the existing Paxos lifecycle:

**Setup phase.** The top-level setup function creates Raft workers for each partition. Each worker is configured with the partition's replication group (the set of replica addresses), preferred leader information, and callback registrations. After all workers are created, a secondary setup phase launches RPC services and configures preferred leaders.

**Preferred leader configuration.** During the secondary setup phase, the system iterates over all partitions and selects the preferred leader for each one. In the current configuration, the local replica (the one running on the same machine as the benchmark client) is always configured as preferred. This ensures that the leader is co-located with the client for minimum latency.

**Steady state.** During normal operation, the Raft server handles leader election and log replication autonomously. The upper layer interacts with it only through log submission and callback registration. Leader change notifications propagate up through a callback chain: the Raft server notifies the worker, the worker notifies the helper layer, and the helper layer notifies the upper Mako layer.

**Shutdown.** Shutdown is deceptively complex in a system with multiple concurrent activities, asynchronous RPC connections, and coroutines that may be blocked on I/O. The implementation uses a two-phase shutdown sequence:

1. **Wait for shutdown**: Blocks until all submitted entries have been processed and committed, then signals the submit thread to stop. This phase ensures that no user data is lost during shutdown — every entry that was submitted before shutdown was initiated will be committed before the system stops. The wait uses a polling loop that checks the submit queue's drain status.
2. **Active shutdown**: Stops the Raft server (which sets the stop flag for the election timer, heartbeat loop, and leadership transfer monitor), disconnects from all peers (closing RPC connections), and releases resources. A brief sleep after setting the stop flag allows detached coroutines to notice the flag and exit before the server's destructor runs.

The active shutdown sequence is ordered carefully to prevent cascading failures. The election timer is stopped first (to prevent the server from starting elections during shutdown), then the heartbeat loop (to prevent the server from sending RPCs to disconnecting peers), then the leadership transfer monitor, and finally the RPC connections. Reversing this order — disconnecting peers before stopping the heartbeat loop — would cause the heartbeat loop to encounter connection errors and potentially trigger error handling code that is itself being torn down.

## Log Submission and Callback Paths

Understanding the data flow through the system is critical for understanding the integration:

**Submission path** (Mako → Raft):
1. Mako's transaction layer calls the replication submit function
2. The dispatcher routes to the Raft implementation
3. The helper layer finds the appropriate worker for the target partition
4. The worker enqueues the command in its pending log queue
5. The submit loop dequeues and wraps the command for Raft
6. The Raft coordinator submits the wrapped command to the Raft server
7. The Raft server appends to its log and replicates to followers

**Callback path** (Raft → Mako):
1. The Raft server commits an entry (majority replicated)
2. The application callback is invoked with the slot index and command
3. The worker's callback function deserialises the command
4. Registered watermark or simple callbacks are invoked
5. Mako's transaction layer processes the committed entry
6. Watermarks and epoch markers are updated

**No-op entries.** The system supports no-operation entries for epoch synchronisation. These are empty commands submitted to Raft that advance the commit index without carrying any data. They are used to synchronise epochs across partitions — when a new epoch begins, no-op entries are submitted to all partitions to ensure they all advance past the epoch boundary. The no-op format includes an epoch identifier for tracking.

## Integration Challenges and Bugs

The integration of Raft into Mako was not a straightforward "plug and play" exercise. The following sections describe the most significant challenges encountered, the root causes that were identified, and the engineering solutions that were applied. These challenges are documented in detail because they represent the kind of real-world integration difficulties that are often invisible in academic papers but dominate the engineering effort in practice.

### Dispatcher Routing and Silent Failures

**Problem.** The dispatch macro needed to correctly route all API functions to the Raft implementation. In early development, missing function mappings or signature mismatches caused silent failures where calls would go to the Paxos implementation even when Raft was configured. The system would appear to work — because Paxos is a valid implementation — but the Raft code was never being exercised.

**Root cause.** The dispatch macro is a text-substitution mechanism that generates function calls at compile time. If a Raft implementation function has a slightly different signature than the Paxos equivalent (for example, different parameter types or different return types), the compiler would silently bind to the Paxos version through implicit conversion. There was no compile-time check that both implementations had matching signatures.

**Solution.** A systematic testing approach was adopted: each API function was tested individually with both replication types, with assertions verifying that the correct implementation was called. Additionally, the build system was configured to compile both paths in all builds (not just when Raft was enabled), catching signature mismatches early.

**Lesson.** Dispatch mechanisms that rely on naming conventions rather than interface enforcement (such as C++ abstract base classes or Rust traits) are fragile. Future refactoring should consider using a proper interface class that both implementations inherit from, providing compile-time guarantees of interface compatibility.

### Replication Type Auto-Detection

**Problem.** The system needed to detect whether to use Raft or Paxos by scanning the YAML configuration file for `ab: raft` or `ab: multi_paxos`. However, some initialisation code that depended on the replication type ran before the configuration files were parsed, leading to the Paxos path being used even when the configuration specified Raft.

**Root cause.** The initialisation sequence had an ordering dependency: the replication type needed to be set before any replication-dependent code executed, but the configuration parser ran later in the boot sequence. This is a common class of bug in complex systems with multi-phase initialisation.

**Solution.** A dedicated auto-detection step was added early in the boot sequence, before any replication-dependent code. This step scans the command-line arguments for configuration file paths, opens each file, and checks for the `ab:` field. Additionally, an explicit command-line flag (`--replication raft`) was added as a fallback mechanism that sets the replication type before any configuration parsing occurs.

**Lesson.** In systems with multi-phase initialisation, the order of operations is a critical correctness property. Configuration detection must happen before the first use of the configured value, not just before the "main" initialisation phase.

### Cross-Shard RPC Failures During Leader Elections

**Problem.** During Raft leader elections in multi-shard configurations, the brief period without a leader (typically 300ms-2s depending on election timeout) caused cross-shard RPC failures. Because cross-shard transactions use 2PC, which requires the leader of each shard to participate, a leaderless shard cannot process cross-shard transactions. This resulted in elevated abort ratios during and immediately after elections.

**Root cause.** Standard Raft does not guarantee a bound on election duration. If multiple candidates start elections simultaneously (split votes), several rounds may be needed before a leader is elected. Each round costs one election timeout period. During this time, the shard cannot process writes.

**Solution.** The preferred leader mechanism (Chapter 4) significantly mitigated this issue by: (1) biasing elections toward a single preferred replica, reducing the probability of split votes; (2) using shorter timeouts for the preferred replica, reducing election duration; and (3) providing deterministic leader placement at startup, ensuring all shards have leaders quickly after a cold start. Additionally, the 2PC coordinator was made more resilient to transient leader absence by retrying after a short delay rather than immediately aborting.

**Lesson.** In a multi-shard system, the availability of individual shard leaders affects the throughput of the entire system because cross-shard transactions span multiple shards. Reducing election duration is not just about availability of a single shard — it affects global system throughput.

### Race Conditions in Callback Registration

**Problem.** Callbacks registered with the Raft server before it was fully initialised could be silently lost. This manifested as followers failing to invoke application callbacks after committing entries — the entries were committed (visible in the log) but never applied to the state machine.

**Root cause.** The callback registration API stored callbacks in the Raft server's internal data structures. If a callback was registered during the setup phase (before the server's data structures were fully initialised), the registration would overwrite uninitialised memory or be overwritten by subsequent initialisation steps.

**Solution.** Callback registrations are now buffered during the setup phase and applied to the server only after initialisation is complete. Additionally, when a leadership change occurs, the callback layer re-registers callbacks with the new leader's server instance, because different partitions may have different leaders and each leader has its own callback table.

**Lesson.** When integrating components with different lifecycle phases (setup, running, shutdown), care must be taken to ensure that inter-component registrations happen at the right point in each component's lifecycle. A "registration buffer" pattern — where registrations are queued during setup and applied at the ready state — is a general solution to this class of problem.

### Coroutine Shutdown and Virtual Function Table Races

**Problem.** During shutdown, the Raft server occasionally crashed with segmentation faults in the election timer or heartbeat loop coroutines. The crashes were intermittent — they depended on the precise timing of coroutine scheduling relative to object destruction — making them extremely difficult to reproduce and diagnose.

**Root cause.** The Raft server uses detached coroutines (fibers) for the election timer and heartbeat loop. These coroutines hold a `this` pointer to the Raft server and call virtual methods on it. When the Raft server's destructor runs, it collapses the virtual function table (vtable) as part of C++ object destruction. If a detached coroutine reads the stop flag (which is `false`), gets preempted, and then the destructor runs (setting the stop flag and collapsing the vtable), the coroutine resumes and calls a virtual method through the now-invalid vtable, causing a crash.

The fundamental issue is a time-of-check-to-time-of-use (TOCTOU) race: the coroutine checks the stop flag and decides to continue, but by the time it executes the next statement, the object has been destroyed. This race is inherent in any system that uses detached coroutines with shared mutable state and object destruction.

**Solution.** The destructor sets the stop flag and then sleeps for a brief interval (100ms) before allowing C++ destruction to proceed. This gives the detached coroutines time to notice the stop flag and exit cleanly before the vtable is collapsed. Additionally, each coroutine checks the stop flag immediately before calling any virtual method, minimising the TOCTOU window. While this sleep is not a theoretically perfect solution (a coroutine could be blocked on I/O for longer than 100ms), it works reliably in practice because the coroutines' blocking operations have bounded timeouts.

**Lesson.** Detached coroutines interacting with objects that have non-trivial destructors are a source of subtle lifetime bugs. The Rust ownership model, which prevents references from outliving the objects they refer to, would catch this class of bug at compile time. The RustyCpp annotations applied to the Raft module (see Chapter 10) are a step toward this kind of compile-time safety, marking the coroutine-spawning methods as `@unsafe` to flag them for manual review.

### Process Cleanup in Multi-Process Tests

**Problem.** Multi-process test configurations (where each replica runs as a separate OS process) required robust cleanup to avoid stale processes from previous runs interfering with new tests. A stale process holding a port would cause the next test to fail at startup with a "port already in use" error, leading to cascading test failures.

**Root cause.** When a test failed (for example, due to a crash or hang), the cleanup code might not execute, leaving processes running. Even when cleanup did execute, some processes were resistant to graceful termination (for example, a process blocked in a network read could not be interrupted by SIGTERM).

**Solution.** A three-layer cleanup approach was implemented: (1) process-specific termination targeting known process names; (2) pattern-based cleanup using signal-based termination for any remaining processes; (3) port availability verification before each test, which catches cases where cleanup was incomplete. The cleanup uses aggressive termination signals to ensure that even hung processes are killed.

**Lesson.** Multi-process test infrastructure needs to be paranoid about cleanup. Tests should not assume that previous tests cleaned up correctly. Every test should start with a verification that its required resources (ports, data directories) are available, and should perform its own independent cleanup before starting.

---

# Chapter 6: Testing and Validation

## Standalone Raft Tests

Testing a consensus protocol implementation is fundamentally different from testing a sequential program. The challenge is that consensus protocols operate in a non-deterministic environment: messages can be delayed, reordered, or lost; servers can crash and restart at any point; and multiple servers execute concurrently. A correct implementation must produce the right result under all possible interleavings of these events — not just the "happy path."

The standalone test suite validates the correctness of the Raft implementation in isolation, without the complexity of the full Mako system. By testing in isolation, the suite can focus on Raft's core properties (election safety, log matching, leader completeness) without interference from Mako's transaction processing, OCC validation, or cross-shard coordination. This separation of concerns is essential: if a test fails, the root cause is in the Raft implementation, not in the interaction between Raft and Mako.

**Test design philosophy.** The test suite is modelled after the MIT 6.824 distributed systems course's Raft lab tests, which have been refined over many years of student implementations and are widely regarded as a thorough correctness validation for Raft. The key design principles are:

- *Black-box testing*: Tests interact with the Raft cluster only through the public API (submit commands, query leaders, check agreement). They do not inspect internal state, set breakpoints, or rely on implementation-specific behaviour. This makes the tests robust to implementation changes — as long as the protocol semantics are correct, the tests pass.
- *Failure injection through topology manipulation*: Rather than injecting faults at the code level (mocking functions, corrupting data structures), failures are simulated by manipulating the network topology. Disconnecting a server is equivalent to a crash from the perspective of other servers — they can no longer communicate with it. Reconnecting is equivalent to a restart. This approach tests the protocol's response to realistic failure patterns.
- *Progressive difficulty*: Tests are ordered so that each test assumes the properties verified by all previous tests. If test 3 (basic agreement) fails, there is no point running test 8 (backup reconciliation) because the more complex test depends on basic agreement working correctly. This ordering makes debugging efficient — the first failing test identifies the most fundamental problem.
- *Deterministic failure scenarios*: Each test creates a specific, reproducible failure scenario (for example, "disconnect the leader and two followers, submit entries, then reconnect"). This is in contrast to random testing (which the unreliable agreement test also provides), which catches different bugs. Both approaches are valuable: deterministic tests validate specific properties, random tests find unexpected interactions.

**Test infrastructure.** The test framework provides:

- A configurable cluster (typically 5 servers for comprehensive testing, though 3 servers are used for some tests)
- Network simulation with programmatic disconnection and reconnection of individual servers. This is the key testing tool — it allows the test to create arbitrary network partitions, simulate server crashes (by disconnecting a server from all peers), and test recovery (by reconnecting). The simulation operates at the RPC level, dropping messages to and from specific servers.
- Leader detection (querying all servers to find the one that believes it is leader). This is inherently racy — a server may believe it is leader but another server may have already won a later election. The test helpers account for this by retrying and checking for stability.
- Agreement checking (verifying that all connected servers agree on the same log entries at a given index). This is the fundamental correctness check: if a command was committed at index N, every server that has an entry at index N must have the same entry.
- Helpers for waiting on leader election, submitting commands, and checking log consistency. These helpers handle the asynchronous nature of the protocol — they wait with timeouts for expected events to occur.

The test framework runs all servers in a single process using a coroutine-based scheduler. This has two advantages: (1) it eliminates real network latency, making tests fast (seconds instead of minutes), and (2) it provides deterministic control over the network topology, allowing tests to create specific failure scenarios that would be difficult to reproduce with real networking.

**Test progression.** The 11 test cases follow a logical progression from basic correctness to increasingly challenging scenarios. This progression is deliberate: each test builds on the assumptions validated by previous tests, and the tests are ordered so that simpler properties are verified before more complex ones.

**Phase 1: Basic Correctness (Tests 1-3)**

1. **Initial election**: Verifies that a leader is elected within a few seconds of startup and that the leader is stable (doesn't change without failures). This is the most fundamental test — if the system cannot elect a leader, nothing else works. The test also verifies that exactly one leader exists (not zero, not two), checking the Election Safety property.

2. **Re-election**: Disconnects the leader (simulating a crash) and verifies that a new leader is elected from the remaining servers. Then reconnects the old leader and verifies it recognises the new leader and becomes a follower. This test validates the failover path — the most important operational scenario in production. It also verifies that a stale leader correctly steps down when it discovers a higher term.

3. **Basic agreement**: Submits commands to the leader and verifies that all servers have the same committed log entries in the same order. This is the core functional test: the leader appends entries, replicates them, and all servers agree. It validates the Log Matching property and the commit mechanism.

**Phase 2: Fault Tolerance (Tests 4-6)**

4. **Fail agreement**: Disconnects one follower, submits commands, and verifies that the remaining majority (2 out of 3, or 3 out of 5) can still commit. Then reconnects the follower and verifies it catches up by receiving the missed entries through the normal replication mechanism. This test validates that the system tolerates minority failures without losing availability — the fundamental promise of consensus.

5. **Fail no agreement**: Disconnects enough servers to break quorum (for example, 3 out of 5) and verifies that commands **cannot** be committed — the leader accepts them but they remain uncommitted because a majority is unreachable. Then reconnects servers, restoring the majority, and verifies that the system recovers and the previously submitted commands are eventually committed. This test validates that the system does not falsely commit entries when quorum is lost — a critical safety property.

6. **Rejoin**: Simulates a partitioned leader that continues accepting writes (which cannot be committed because the leader is partitioned from the majority). Meanwhile, the majority elects a new leader and commits new entries. When the partitioned leader reconnects, it discovers the new leader's higher term, steps down, and its stale uncommitted entries are overwritten by the new leader's committed entries. This test validates that uncommitted entries from a stale leader do not override committed entries from the current leader.

**Phase 3: Concurrency and Efficiency (Tests 7-9)**

7. **Concurrent starts**: Submits multiple commands simultaneously from different threads and verifies that all committed commands appear in the log. The order of concurrent submissions is non-deterministic (the leader may interleave them in any order), but every committed command must be present. This test stresses the leader's ability to handle concurrent submissions without losing entries or producing duplicate entries.

8. **Backup**: A stress test for log reconciliation. Disconnects the leader and one follower, then submits many commands to the partitioned leader (which cannot commit because it only has itself — no majority). Meanwhile, the remaining servers elect a new leader and commit their own entries. When the partitioned servers reconnect, the new leader must reconcile the diverged logs — overwriting the stale entries on the old leader with the committed entries. With many stale entries, this exercises the three-tier log reconciliation optimisation (fast/exponential/linear backoff). The test verifies that reconciliation completes within a reasonable time.

9. **Count**: Verifies that the number of RPCs exchanged between servers is bounded — the system should not generate excessive communication. This is an efficiency test, not a correctness test, but it catches a common bug: if the leader sends unnecessary RPCs (for example, sending the entire log on every heartbeat instead of just new entries), the system wastes bandwidth and CPU. The test sets an upper bound on RPC count and verifies it is not exceeded.

**Phase 4: Adversarial Scenarios (Tests 10-11)**

10. **Unreliable agreement**: Enables simulated network unreliability — messages are randomly dropped and delayed — and verifies that commands are still eventually committed correctly. This test validates the protocol's resilience to message loss, which is the expected operating condition in real networks. Raft handles message loss through retransmission via the heartbeat loop: if an AppendEntries message is lost, the next heartbeat will include the same entries.

11. **Figure 8**: This is the most important safety test. It implements the specific scenario from Figure 8 of the Raft paper, which demonstrates why a leader cannot commit entries from previous terms by counting replicas. The scenario creates a situation where:
    - A leader in term 2 replicates an entry to a minority of servers
    - That leader crashes and a new leader is elected in term 3
    - The new leader replicates entries, then crashes
    - A third leader is elected, and the entry from term 2 is now on a majority — but it must NOT be committed because committing it could violate Leader Completeness

    The correct behaviour is that the leader only commits entries from its current term, and entries from previous terms are committed implicitly when a current-term entry is committed. The test verifies that the implementation handles this correctly by checking that no committed entry is ever overwritten.

**Preferred leader tests.** Three additional test binaries validate the preferred leader mechanism using the Mako replication API (the same API used in production):

- **Startup test**: Verifies that the preferred replica becomes leader after startup within the expected time window.
- **Log replication test**: Verifies that entries submitted to the preferred leader are correctly replicated to all followers and that follower callbacks are invoked with the correct data.
- **No-op test**: Verifies that no-operation entries for epoch synchronisation are correctly replicated and applied, with callbacks reporting the expected epoch markers.

## Continuous Integration Test Suite

While the standalone tests validate Raft's correctness in an idealised single-process environment, the CI test suite validates the complete Mako system with Raft replication in realistic multi-process configurations. This distinction is important: bugs that don't manifest in the standalone tests — such as RPC serialisation errors, process startup race conditions, port conflicts, and shutdown hangs — can appear when real processes communicate over real TCP/IP connections.

The CI test suite serves as the ultimate acceptance test for the Raft integration: if all CI tests pass, the Raft path is ready for production use. The suite includes tests at every level of system complexity:

- **Simple replication**: Basic Raft consensus without transactions, validating that log entries are correctly replicated.
- **Single-shard TPC-C**: Full transaction processing with Raft replication, validating throughput and correctness for single-shard workloads.
- **Multi-shard TPC-C**: Cross-shard transactions with Raft replication, validating 2PC coordination with Raft and correctness under cross-shard abort/retry.
- **Simple transactions with integrity verification**: End-to-end data integrity checks, verifying that all replicas have identical committed state.

**Script architecture.** The CI script follows a five-phase pattern for each test:

1. **Setup**: Clean the environment (kill stale processes from previous runs, remove old data directories and log files), configure ports for each replica, and prepare log directories. This cleanup is critical for test isolation — a stale process from a failed test that still holds a port will cause the next test to fail at startup.
2. **Launch**: Start replica processes (followers first, then leader) with the appropriate configuration files. The startup order matters because the leader needs followers to be listening before it can establish RPC connections. A brief stabilisation delay after startup allows leader election to complete.
3. **Monitor**: Wait for the test to complete by polling for expected output in log files, rather than using fixed sleep durations. Polling is more robust than sleeping because it adapts to the actual system speed — a fast machine finishes sooner, while a slow machine gets more time. The polling checks for specific keywords in the log output that indicate completion.
4. **Analyse**: Extract metrics from log files and check pass/fail criteria. Each test has specific thresholds: throughput must be above a minimum, abort ratios must be below a maximum, and verification keywords must be present.
5. **Report**: Output results (PASS or FAIL with details) and clean up all processes to leave a clean environment for the next test.

**Process management.** Multi-process tests require robust process lifecycle management. The CI infrastructure uses a three-layer cleanup approach:

1. **Process-specific termination**: Kill specific processes by name and port, targeting known process types.
2. **Pattern-based cleanup**: Kill any remaining processes matching broader patterns, catching processes that weren't cleanly terminated.
3. **Port availability verification**: Before each test, verify that the required ports are free. This catches cases where a process from a previous test is still lingering.

This aggressive approach was necessary because lingering processes from failed tests could occupy ports and interfere with subsequent tests. In early development, this was one of the most common sources of test failures — a test that should have passed would fail because a zombie process from a previous failed test was still holding a port.

**Known issues and workarounds.** The CI suite includes workarounds for several known issues:

- **Leader shutdown hang**: In some configurations, the leader process hangs during shutdown after the benchmark completes. This is handled by using a timeout-based kill mechanism rather than waiting for graceful shutdown.
- **Port range separation**: Raft tests use different port ranges than Paxos tests to avoid conflicts when both test suites run on the same machine.

## Test Scenarios and Pass Criteria

The CI test suite includes the following scenarios:

**Simple Raft replication.** Launches a 3-replica cluster, submits key-value entries through the Mako replication API, and verifies that all followers receive all entries. Pass criterion: each follower reports at least 300 committed entry callbacks.

**1-shard TPC-C with Raft replication.** Launches a 1-shard, 3-replica configuration, runs the TPC-C benchmark, and verifies that throughput is above a minimum threshold and that followers successfully replay committed entries. Pass criterion: positive throughput reported and followers report replay_batch values greater than a threshold.

**2-shard TPC-C with Raft replication.** Launches a 2-shard, 3-replicas-per-shard configuration, runs TPC-C, and verifies that both shards achieve positive throughput with abort ratios below 40%. This tests cross-shard transaction handling with Raft. Pass criterion: both shards report positive throughput and acceptable abort ratios.

**1-shard simple transaction with Raft.** Runs the simple transaction workload with replication and verifies data integrity across all replicas. Pass criterion: ALL VERIFICATIONS PASSED on all replicas.

**2-shard simple transaction with Raft.** Same as above but with 2 shards, testing cross-shard simple transactions with Raft. Pass criterion: ALL VERIFICATIONS PASSED on all replicas in both shards.

All test scenarios are also run with Paxos for comparison, ensuring that the Raft tests achieve comparable correctness to the established Paxos tests.

---

# Chapter 7: From Many to One — Consolidating Raft Instances

## What Is a Partition?

Before diving into the consolidation, it is important to clarify what we mean by a *partition*, as this term is central to the discussion that follows.

In Mako, data is first divided into **shards** — coarse-grained units of the keyspace that can be placed on different machines. In a TPC-C deployment, for example, each shard handles a range of warehouses. Shards are the unit of replication: each shard has its own replication group (a set of leader and follower replicas spread across datacenters), and each shard can be placed on a different set of machines for fault isolation.

Within a single shard, the data is further subdivided into **partitions**. A partition is simply a slice of the shard's data, assigned to a dedicated worker thread for processing. If a shard has 6 warehouse threads, it has 6 partitions — one per thread. Each partition handles a subset of the transactions for that shard. Partitions exist for concurrency: by giving each worker thread its own partition, the system avoids contention between threads within the same shard.

Crucially, each partition has its own independent consensus instance. It is not one Raft group per shard, but one per partition. In a configuration with 1 shard and 6 partitions (which is the setup used in this chapter's benchmarks), there are 6 independent Raft groups — all on the same machine, all replicating to the same set of follower machines, but each maintaining its own leader election, its own log, and its own heartbeat loop. This is the architectural starting point that motivated the consolidation described below.

## The Problem with Per-Partition Raft Groups

The previous chapters described an architecture in which every partition maintains its own independent Raft group — its own leader, its own election timer, its own replicated log, and its own heartbeat loop. This per-partition design is a natural first step: it mirrors the logical independence of partitions and keeps the consensus layer simple. In practice, however, running many Raft groups on a single machine introduces subtle performance problems that only become visible under measurement.

When every partition runs its own Raft group, the system on each server looks like this:

```
Partition 0 → Raft Group 0   (election, log, heartbeat)
Partition 1 → Raft Group 1   (election, log, heartbeat)
Partition 2 → Raft Group 2   (election, log, heartbeat)
   ...            ...
Partition 5 → Raft Group 5   (election, log, heartbeat)
```

Each group elects its own leader, maintains its own heartbeat timer, and replicates its own log independently. While conceptually clean, this design suffers from four interrelated problems:

**Election interference.** When a server starts up (or recovers from a network partition), all 6 groups begin their election timers simultaneously. Each election involves random timeouts and vote requests. With 6 partitions, the probability that at least one group experiences a split vote — where two candidates split the majority and neither wins — becomes significant. When a split vote occurs, that partition's throughput drops to zero until the next election attempt succeeds. Because the elections are independent, some partitions may be fully operational while others are still trying to elect a leader, leading to highly variable aggregate throughput.

**Heartbeat amplification.** The Raft protocol requires the leader to send periodic heartbeats (AppendEntries RPCs) to each follower to maintain its authority. With 6 groups, each with 2 followers, the leader machine sends 12 heartbeat streams concurrently, each consuming CPU cycles, network bandwidth, and poll-thread attention.

**Throughput unpredictability.** Because election timing is randomised, some runs of the system start up smoothly (all six groups elect leaders quickly) and achieve high throughput, while other runs experience one or more delayed elections and achieve significantly lower throughput. This creates a bimodal distribution in observed performance: the system is either "lucky" (all elections succeeded) or "unlucky" (some elections stalled), with little in between.

**Operational burden.** Monitoring, debugging, and reasoning about 6 independent Raft groups is harder than reasoning about one. Each group has its own term, its own leader, and its own commit progress. Diagnosing a throughput regression requires checking all 6 groups to determine which one (if any) experienced a leadership disruption.

## The Consolidated Design

The single-instance design replaces 6 independent Raft groups with one shared group that serves all partitions within the shard:

```
Partition 0 ──→ ┐
Partition 1 ──→ │
Partition 2 ──→ ├──→  Single Raft Instance  ──→  Single Replicated Log
Partition 3 ──→ │     (1 leader, 1 election, 1 heartbeat loop)
Partition 4 ──→ │
Partition 5 ──→ ┘
```

All partitions submit their transaction log entries to the same Raft instance. The Raft protocol replicates these entries in a single, interleaved log. When entries are committed and applied, the system routes each entry back to the partition it originated from.

The key insight is that Raft does not care about the *meaning* of the entries it replicates — it simply ensures that all replicas agree on the same sequence of entries. By tagging each entry with its partition identifier, we can multiplex all partitions through one consensus group and demultiplex on the apply path.

### How Routing Works

Each log entry carries a partition identifier that records which partition submitted it. When the Raft instance commits an entry and hands it to the application layer for execution, the system inspects this identifier and dispatches the entry to the correct partition's callback handler. This is analogous to how a network switch routes packets based on destination addresses — the consensus layer acts as a reliable, totally-ordered broadcast channel, and the routing layer ensures each entry reaches the right consumer.

### Maintaining Connectivity

In Mako's architecture, remote replicas establish network connections to every partition's port. With multiple Raft groups, each group listens on its own port. With a single Raft group, only one port is "real" — the others must still accept connections to avoid breaking remote replicas that expect to connect to them. The solution is lightweight forwarding servers on the unused ports that transparently redirect all traffic to the single Raft instance. Remote replicas are completely unaware of this consolidation; from their perspective, the system looks identical to the multi-instance configuration.

### Decoupling Entry Application from Replication

One subtle but important change is the introduction of a dedicated thread for applying committed entries. In the multi-instance design, each Raft group applies its entries on the same thread that handles RPC communication. If applying an entry is slow (for example, replaying a complex transaction on a follower), the RPC thread is blocked and cannot respond to heartbeats, potentially triggering unnecessary elections.

In the consolidated design, committed entries are placed into a queue and applied by a separate background thread. This ensures that the RPC thread remains responsive to heartbeats even when transaction replay is slow, which is particularly important because the single Raft instance handles a higher volume of entries than any individual group did before.

### Election Timeout Adjustments

Because the single instance handles more work per heartbeat cycle, election timeouts are increased to give the system more breathing room:

| Role | Multi-Instance | Single Instance |
|------|---------------|-----------------|
| Preferred leader | 150–300 ms | 300–600 ms |
| Non-preferred (grace period) | 1–2 s | 5–10 s |
| Non-preferred (normal) | 0.5–1 s | 3–6 s |

These longer timeouts prevent false election triggers during periods of high load, without meaningfully increasing failover time in genuine failure scenarios.

## Experimental Evaluation

To quantify the impact of this consolidation, we benchmarked both configurations using the same test scenario: a single shard with 6 partitions, 3 replicas, running the TPC-C benchmark with 6 warehouse threads. Each configuration was run 10 times on identical hardware to capture the distribution of outcomes, not just a single point estimate.

### Throughput Results

| Run | Single Instance (ops/sec) | Multiple Instances (ops/sec) |
|-----|---------------------------|------------------------------|
| 1   | 215,503                   | 190,831                      |
| 2   | 207,083                   | 91,464                       |
| 3   | 209,322                   | 199,653                      |
| 4   | 216,419                   | 160,187                      |
| 5   | 210,284                   | 88,979                       |
| 6   | 208,124                   | 88,078                       |
| 7   | 205,792                   | 89,871                       |
| 8   | 208,597                   | 198,647                      |
| 9   | 205,853                   | 149,909                      |
| 10  | 204,853                   | 121,899                      |

### Summary Statistics

| Metric | Single Instance | Multiple Instances | Change |
|--------|-----------------|--------------------|--------|
| Mean throughput   | 209,183 ops/sec | 137,952 ops/sec | **+51.6%** |
| Median throughput | 208,361 ops/sec | 135,904 ops/sec | +53.3% |
| Minimum           | 204,853 ops/sec | 88,078 ops/sec  | +132.6% |
| Maximum           | 216,419 ops/sec | 199,653 ops/sec | +8.4%  |
| Standard deviation| 3,955 ops/sec   | 47,774 ops/sec  | −91.7% |
| Coefficient of variation | 1.9%    | 34.6%           | −32.7 pp |

### Follower Replay Progress

The number of entries successfully replayed on followers during each test provides a second perspective on consistency:

| Metric | Single Instance | Multiple Instances |
|--------|-----------------|--------------------|
| Mean   | 10,848 entries  | 5,265 entries      |
| Median | 11,578 entries  | 4,759 entries      |
| Min    | 9,030 entries   | 976 entries        |
| Max    | 12,346 entries  | 12,069 entries     |

The single instance consistently replays roughly twice as many entries, with far less variation. The multi-instance minimum of 976 entries (compared to 9,030 for the single instance) indicates that some runs experienced severe replication delays, likely due to the election interference discussed earlier.

## Interpreting the Results

Three patterns stand out in the data:

**The single instance is both faster and more predictable.** Its mean throughput is 51.6% higher, but perhaps more importantly, its coefficient of variation is 1.9% compared to 34.6%. In practical terms, this means that the single instance delivers approximately the same performance on every run, while the multi-instance configuration is a gamble — some runs are nearly as fast, but others achieve barely half the peak throughput.

**The multi-instance results are bimodal.** Looking at the raw data, the multi-instance runs cluster into two groups: roughly 190,000–200,000 ops/sec when all six elections succeed quickly, and roughly 88,000–91,000 ops/sec when one or more elections stall. This bimodality is a direct consequence of independent elections — the system's aggregate throughput is limited by its slowest partition, and independent elections create a lottery where some partitions may be slow to elect a leader.

**The worst case improves dramatically.** The single instance's worst run (204,853 ops/sec) is 2.3 times better than the multi-instance worst run (88,078 ops/sec). For systems that need to provide throughput guarantees — which is most production systems — the worst case matters more than the average. The consolidation transforms Raft's throughput from unpredictable to reliable.

These improvements stem from three reinforcing factors. First, a single election eliminates the possibility of split elections on individual partitions. Second, a single heartbeat loop reduces the RPC overhead from 12 streams to 2. Third, the background apply thread prevents slow transaction replay from disrupting heartbeat responses.

## Trade-offs and Applicability

The consolidation is not without trade-offs. The following table summarises the key differences:

| Aspect | Multiple Instances | Single Instance |
|--------|--------------------|-----------------|
| Mean throughput | 137,952 ops/sec | 209,183 ops/sec |
| Throughput predictability | Low (CV 34.6%) | High (CV 1.9%) |
| Partition isolation | Full — each partition fails independently | Shared — a Raft failure affects all partitions |
| Failover granularity | Per-partition | All-or-nothing |
| Number of elections | 6 independent | 1 global |
| Heartbeat overhead | O(partitions x replicas) | O(replicas) |

**When the single instance is the better choice.** When partitions are co-located on the same physical server (as is typical in Mako's deployment model), they already share the same failure domain — if the server crashes, all partitions go down together regardless of how many Raft groups they use. In this setting, per-partition Raft groups provide only the *illusion* of isolation without the *reality*, while incurring real performance costs. The single instance is the natural choice.

**When multiple instances may be preferable.** If partitions are distributed across different physical servers with genuinely independent failure domains, per-partition Raft groups allow one partition to continue operating even when another's server fails. This scenario is less common in Mako but could arise in other system designs.

## Implications for Geo-Replication

The single Raft instance interacts particularly well with the preferred leader election mechanism described in Chapter 4. With a single Raft instance, one election determines the leader for all partitions on that server. Combined with preferred leader election, this means the system can guarantee that a specific datacenter holds leadership for all partitions with a single election cycle.

Under the multi-instance design, it was theoretically possible for different partitions to elect leaders in different datacenters — partition 0 might elect a leader in the US datacenter while partition 1 elects a leader in the EU datacenter. This "split leadership" scenario would force cross-shard transactions to coordinate across datacenters even when it is unnecessary. The single instance eliminates this possibility entirely: either the server is the leader for all partitions or it is the leader for none. This all-or-nothing property simplifies routing, reduces cross-datacenter traffic, and makes the system's behaviour easier to reason about in a geo-replicated setting.

---

# Chapter 8: Performance Evaluation

## Benchmark Methodology

The performance evaluation aims to answer two fundamental questions: (1) How does Raft's throughput compare to Multi-Paxos's under realistic workloads? (2) Under what conditions does the choice of replication protocol matter for performance?

To answer these questions, we run both protocols under the same workloads and configurations, varying the number of shards (1 vs 2) to study the effect of cross-shard coordination.

**Test environment.** All benchmarks were run on a single localhost machine with all replicas co-located. This eliminates network latency as a variable — measured throughput reflects CPU overhead and synchronisation costs rather than network round-trip time. While this does not represent production deployment conditions, it provides a controlled environment for comparing the two protocols.

There is an important trade-off in this approach. By running on localhost, we eliminate the most significant real-world latency source (network round-trips between replicas), which means the measured throughput differences are larger than what would be observed in a geo-replicated deployment. However, this is deliberate: by removing network latency, we isolate the protocol-level differences in CPU overhead and synchronisation costs, which are the factors that the protocol choice directly influences.

**Build configuration.** All tests use release-mode compilation with full optimisation, OCC concurrency control, and the jemalloc memory allocator. The srpc TCP/IP RPC transport backend is used for all communication. The same build configuration is used for both Paxos and Raft tests, ensuring that performance differences are attributable to the protocol and not to build differences.

**Primary workload.** The primary workload is TPC-C, the industry-standard OLTP benchmark. TPC-C was chosen because it is widely understood, has a well-defined transaction mix, and exercises both read and write paths with varying conflict profiles. TPC-C models a wholesale supplier with five transaction types:

| Transaction | Mix Weight | Description | Cross-shard? |
|-------------|-----------|-------------|--------------|
| NewOrder | 45% | Create a new order (read + write) | Yes — can span warehouses on different shards |
| Payment | 43% | Process a payment (read + write) | Yes — can span warehouses |
| Delivery | 4% | Deliver pending orders (batch write) | No |
| OrderStatus | 4% | Query order status (read-only) | No |
| StockLevel | 4% | Check stock levels (read-only) | No |

NewOrder and Payment together account for 88% of the workload and are the only transactions that can span multiple shards. This makes them the primary drivers of cross-shard coordination overhead and the most important transactions for understanding the multi-shard performance profile.

A secondary workload — simple key-value transactions — is used for data integrity verification. This workload writes known key-value pairs to the leader and verifies that all replicas receive identical data after replication. It serves as a correctness test rather than a performance test.

**Test configurations.**

| Parameter | 1-Shard Paxos | 1-Shard Raft | 2-Shard Paxos | 2-Shard Raft |
|-----------|--------------|-------------|--------------|-------------|
| Shards | 1 | 1 | 2 | 2 |
| Replicas per shard | 3 voters + 1 learner | 3 voters | 3 voters + 1 learner | 3 voters |
| Total processes | 4 | 3 | 8 | 6 |
| Worker threads | 6 | 6 | 6 per shard | 6 per shard |
| Warehouses per shard | 6 | 6 | 6 | 6 |

**Metrics.** The primary metric is aggregate persisted throughput (ops/sec), which measures committed transactions per second. Additional metrics include per-transaction latency (average, p50, p99), abort ratios, and follower replay batch counts.

## Single-Shard Results

| Metric | Paxos | Raft | Difference |
|--------|-------|------|------------|
| Aggregate throughput | 133,931 ops/sec | 96,463 ops/sec | Raft 28.0% lower |
| Replicas | 4 processes | 3 processes | Raft 25% fewer |

**Per-transaction latency (1-shard):**

| Transaction | Paxos Latency | Raft Latency | Faster |
|-------------|---------------|--------------|--------|
| NewOrder | 0.0451 ms | 0.0390 ms | Raft (13.5% lower) |
| Payment | 0.0329 ms | 0.0815 ms | Paxos (59.6% lower) |
| Delivery | 0.1378 ms | 0.1155 ms | Raft (16.2% lower) |
| OrderStatus | 0.0141 ms | 0.0113 ms | Raft (19.9% lower) |
| StockLevel | 0.1034 ms | 0.1094 ms | Paxos (5.5% lower) |

Notably, Raft achieves lower per-transaction latency for 3 out of 5 transaction types, yet Paxos achieves 28% higher aggregate throughput. This apparent paradox is explained by the pipelining analysis below.

**Follower replication (1-shard):**

| Metric | Paxos Follower | Raft Follower |
|--------|---------------|---------------|
| Replay batches | 669 | 3,674 |
| Ratio | 1x | 5.5x |

Raft followers process 5.5 times more replay batches than Paxos followers for the same workload, indicating that Raft uses smaller, more frequent batches while Paxos batches more aggressively.

## Multi-Shard Results

| Metric | Paxos (per shard) | Raft (per shard) | Difference |
|--------|-------------------|------------------|------------|
| Throughput | 8,501 ops/sec | 8,536 ops/sec | Raft 0.4% higher (within noise) |
| Total throughput | 17,003 ops/sec | 17,071 ops/sec | Essentially equal |
| Remote abort ratio | 1.28% | 2.64% | Raft 2.1x higher |

Both protocols experience dramatic throughput reduction when cross-shard transactions are introduced:

| Protocol | 1-Shard | 2-Shard (per shard) | Drop Factor |
|----------|---------|---------------------|-------------|
| Paxos | 133,931 ops/sec | 8,501 ops/sec | 15.8x |
| Raft | 96,463 ops/sec | 8,536 ops/sec | 11.3x |

## Analysis: Why Paxos Is Faster in Single-Shard Mode

The 28% single-shard throughput advantage for Paxos is a significant result that merits careful analysis. It would be easy to conclude that "Paxos is simply faster than Raft," but this conclusion would be misleading — as the multi-shard results show, the advantage disappears entirely when cross-shard coordination enters the picture. The single-shard result reveals something specific about the architectural differences between the two protocols, not a general performance ordering.

The 28% gap is explained by several factors, which we decompose below:

**Multi-Paxos pipelining (estimated 20-25% of the gap).** This is the primary architectural reason. Multi-Paxos can process multiple consensus instances simultaneously — while instance N is in the Accept phase, instance N+1 can already be in the Prepare phase. The leader can overlap network round-trips across instances, keeping its proposal pipeline full without waiting for sequential commits.

Raft, by contrast, enforces strict sequential commit ordering. Entries are applied in strict order from the execute index to the commit index. If entry N is slow to replicate, entries N+1, N+2, and beyond cannot be committed or applied until N is committed. This sequential ordering is a correctness requirement (the replicated log must be identical across all replicas), but it limits concurrency compared to Multi-Paxos's per-instance parallelism.

In a single-shard test with no cross-shard coordination, the replication layer is the primary bottleneck, making this pipelining difference decisive.

**Batch size difference (estimated 10-15% of the gap).** The replay batch data reveals that Paxos uses larger batches (~200 entries per batch) compared to Raft (~26 entries per batch). Larger batches reduce per-entry overhead: fewer RPCs, fewer I/O synchronisation points, and better amortisation of fixed costs. The pipelining design naturally accumulates more entries before follower replay, leading to larger batches.

**Test harness differences (negligible impact on measurement).** Both protocols use the same 30-second internal benchmark runtime, configured identically via `BenchmarkConfig::runtime_` (default 30 seconds in `src/mako/benchmarks/benchmark_config.h:60`). The `dbtest` binary computes throughput (`agg_persist_throughput`) over this 30-second window in both cases. The test harness scripts differ in how they manage process lifetime — the Paxos script polls for the benchmark completion marker and exits shortly after, while the Raft script waits a fixed 60 seconds before stopping processes — but this difference affects only the shell script's wall-clock time, not the benchmark measurement window itself. Since both protocols measure throughput over the same 30-second internal runtime, there is no duration-related measurement bias.

**Process count (estimated 3-5% in favour of Raft).** Paxos runs 4 processes per shard while Raft runs 3, so the extra Paxos learner consumes CPU that could otherwise go to voters. This partially offsets Paxos's advantages — despite having one more process competing for CPU, Paxos still achieves 28% higher throughput, meaning the pipelining advantage more than compensates.

**Why Paxos wins aggregate despite Raft winning per-transaction latency.** Despite Raft being faster for 3 of 5 transaction types, Paxos achieves higher aggregate throughput because Payment (43% of the TPC-C mix) is where Paxos has its largest per-transaction advantage (59.6% lower latency), and aggregate throughput depends on the replication layer's ability to process concurrent commits, not just individual transaction latency. Paxos's pipelining allows deeper commit concurrency regardless of per-transaction timings.

## Analysis: Why Throughput Converges in Multi-Shard Mode

When cross-shard transactions are introduced, the bottleneck shifts from the replication layer to **cross-shard coordination**. TPC-C's NewOrder and Payment transactions can span two shards, requiring a 2PC protocol. The coordination latency (~10ms round-trip even on localhost) dominates the per-transaction cost. Replication latency (sub-millisecond on localhost for both protocols) becomes negligible relative to the coordination overhead.

This is analogous to Amdahl's Law: when the serial component (cross-shard coordination) dominates, improvements to the parallel component (replication) yield diminishing returns. Both protocols converge to the same 2-shard throughput (~8,500 ops/sec per shard), confirming that the replication layer is no longer the bottleneck.

Paxos drops more dramatically (15.8x vs 11.3x) because it starts from a higher single-shard baseline. Both protocols are equally bottlenecked by cross-shard coordination, so they converge to the same absolute throughput.

**Higher remote abort ratio under Raft.** Raft's remote abort ratio (2.64%) is 2.1 times higher than Paxos's (1.28%). This difference is a second-order consequence of Raft's sequential commit ordering. In Raft, entries are committed and applied in strict log order. When cross-shard 2PC is in progress, the participating shards hold tentative state (locks or speculative results) for the duration of the 2PC protocol. Because Raft's sequential ordering can delay the commitment of individual entries (if earlier entries in the log are not yet committed), the tentative state is held for longer, extending the window during which a concurrent transaction accessing the same data will observe a conflict and abort.

Multi-Paxos's pipelined commit allows individual consensus instances to commit independently, so the 2PC protocol can complete as soon as the specific instance for that transaction is committed, without waiting for preceding instances. This shorter tentative-state window reduces the probability of overlapping access, explaining the lower abort ratio.

Despite the higher abort ratio, Raft achieves the same throughput as Paxos in multi-shard mode because aborted transactions are retried and ultimately succeed. The cost of a retry (re-executing the transaction and re-running 2PC) is small relative to the initial cost, and the retry typically succeeds because the conflicting transaction has completed. The 2.64% abort ratio means that fewer than 3% of transactions require a retry — a modest overhead that does not measurably affect aggregate throughput.

## Replication Batching Behaviour

The replay batch metric reveals fundamental differences in how the two protocols batch replication, and these differences are rooted in their architectural designs rather than being tuneable parameters.

- **Raft**: More frequent, smaller batches (~26 entries per batch). Each heartbeat interval triggers a batch of accumulated entries. The leader collects entries from each follower's next index through the leader's last log index. Because Raft commits entries sequentially and the heartbeat loop processes followers in rounds, the natural batch size is determined by the number of entries submitted between heartbeat iterations.
- **Paxos**: Less frequent, larger batches (~200 entries per batch). The pipelining design naturally accumulates more entries before follower replay. Because Multi-Paxos can have many consensus instances in flight simultaneously, the follower accumulates entries from multiple instances before replaying them as a batch. Bulk commit notifications batch multiple instances together, and the learner (which does not participate in consensus) receives committed entries in large bursts.

Paxos's larger batch sizes reduce per-entry overhead (fewer RPCs, fewer I/O synchronisation points), contributing to its single-shard throughput advantage.

The 5.5x replay batch count difference (3,674 for Raft vs. 669 for Paxos) is inversely proportional to the batch size difference (26 vs. 200 entries per batch). Both protocols process approximately the same total number of entries (since they handle the same workload), but they distribute those entries across different numbers of batches. Larger batches amortise fixed costs better: each batch requires one RPC, one network round-trip, and one I/O synchronisation point regardless of how many entries it contains. The 8x difference in entries per batch translates into a meaningful throughput difference when the replication layer is the bottleneck (single-shard mode), but becomes irrelevant when a larger bottleneck dominates (multi-shard coordination).

This architectural difference between sequential commit (Raft) and pipelined commit (Paxos) is fundamental — it cannot be eliminated by tuning parameters. The only way to achieve Paxos-like batching in Raft would be to introduce parallel commit semantics (allowing entries from the current term to commit independently), which would require changes to the core Raft protocol and has implications for the safety argument. This is a topic for future research.

## Production Deployment Implications

The performance results presented above were measured on a single machine with localhost networking. Before drawing conclusions about production deployments, it is important to consider how these results would change under real-world conditions.

### When to Choose Raft

Raft is the preferred choice under the following conditions:

**Resource efficiency matters.** In a deployment with hundreds of shards, the 25% process reduction (3 replicas per shard instead of 4) translates to hundreds of fewer processes. Each process consumes CPU, memory, and network resources. For a 100-shard deployment, Raft uses 300 processes while Paxos uses 400. This difference translates directly to infrastructure cost savings.

**Operational simplicity is valued.** Raft's built-in leader election eliminates the need for an external leader election mechanism. The preferred leader extension provides deterministic leader placement without any external coordination. This reduces the number of systems that operators need to configure, monitor, and troubleshoot. In production, simpler systems fail less often and are easier to diagnose when they do fail.

**Multi-shard workloads dominate.** When cross-shard transactions are the norm — which is the case for most OLTP workloads that cannot be perfectly partitioned — both protocols perform equally. The 28% single-shard advantage disappears, and Raft's resource efficiency becomes the decisive factor.

**Deterministic leader placement is needed.** For geo-replicated deployments, the preferred leader mechanism provides data locality guarantees that standard Raft (and Paxos) cannot. Operators can specify that each shard's leader should be in the datacenter closest to the shard's primary clients, minimising read and write latency.

### When to Choose Paxos

Paxos is the preferred choice under the following conditions:

**Single-shard throughput is critical.** For workloads that can be perfectly partitioned into independent shards with no cross-shard transactions, Paxos's 28% throughput advantage is significant. This applies to workloads like social media feeds (partitioned by user) or IoT data ingestion (partitioned by device).

**Learner replicas are needed.** Paxos's learner role provides a non-voting read replica that can serve read-only queries without participating in consensus rounds. This is useful for read-heavy workloads where read scaling is more important than write throughput. The Raft implementation does not currently support non-voting learners, though this could be added in the future.

**Pipelining benefits outweigh complexity.** For workloads with very high-frequency small transactions (such as message queues or event logs), Multi-Paxos's ability to pipeline consensus instances across slots provides a throughput advantage that cannot be matched by Raft's sequential commit ordering.

### Performance Parity in Real-World Deployments

For most real-world deployments with multiple shards and cross-shard transactions, the results suggest that Raft and Paxos perform equivalently. The reasons are:

1. **Cross-shard coordination dominates**: In a deployment with N shards, any transaction that touches more than one shard must execute a 2PC protocol. The 2PC latency (multiple network round-trips across shards) dwarfs the per-shard replication latency, making the choice of replication protocol irrelevant for throughput.

2. **Network latency dominates in geo-replication**: In a geo-replicated deployment, the network round-trip time between replicas is 1-100ms (depending on geography), while the protocol-level CPU overhead difference is measured in microseconds. The 28% throughput difference measured on localhost (where network latency is zero) would shrink to a few percent or less in a real deployment.

3. **The bottleneck shifts**: Production systems are rarely bottlenecked by the replication protocol. More common bottlenecks include disk I/O (for persistent storage), network bandwidth (for large transactions), application logic (for complex stored procedures), and cross-shard coordination (for distributed transactions).

The practical conclusion is that the choice between Raft and Paxos should be driven by **operational considerations** (simplicity, process count, leader placement, learner support) rather than raw throughput. Both protocols are "fast enough" for the vast majority of production workloads.

| Factor | Paxos | Raft |
|--------|-------|------|
| Single-shard throughput | Higher (133,931 ops/sec) | Lower (96,463 ops/sec) |
| Multi-shard throughput | ~8,500 ops/sec/shard | ~8,500 ops/sec/shard |
| Process overhead | 33% more (learner) | Baseline |
| Leader election | External mechanism | Built-in (with preferred leader) |
| Log ordering | Per-instance (pipelined) | Sequential |
| Follower replay latency | Higher (large batches) | Lower (small batches) |
| Correctness | Verified | Verified |
| Operational complexity | Higher | Lower |

## Threats to Validity

**Single-node testing.** All benchmarks run on a single machine with localhost networking. Production deployments spread replicas across machines with real network latency. The relative performance of Raft vs Paxos may differ when network latency is the dominant factor.

**Single run (Paxos vs Raft comparison).** The Paxos vs Raft comparison results in this chapter are from a single CI run. The single vs multiple Raft instance comparison in Chapter 7 uses 10 runs per configuration with statistical analysis, demonstrating that variance can be significant (particularly for the multi-instance configuration with CV 34.6%).

**Small scale.** The tests use 1-2 shards with 3 replicas each. Production systems may run hundreds of shards. Scaling effects are not captured.

**Test harness differences.** Both protocols use the same 30-second internal benchmark runtime (`BenchmarkConfig::runtime_ = 30`), but the test harness scripts differ in process lifecycle management. The Paxos script polls for completion and exits shortly after, while the Raft script waits a fixed 60 seconds. This does not affect the benchmark measurement window.

---

# Chapter 9: Log Persistence and Recovery

## Persistent Log Storage

Persistence is what transforms a consensus protocol from a theoretical algorithm into a practical system. Without persistence, a server that crashes and restarts has no memory of its previous state — it doesn't know what term it was in, whom it voted for, or what log entries it had accepted. In a Raft cluster, this amnesia is dangerous: a restarted server that has forgotten its vote could vote again in the same term for a different candidate, violating Election Safety. A server that has forgotten its log entries could accept conflicting entries from a new leader, violating Log Matching.

The Raft protocol requires three categories of state to be persisted to survive crashes:

1. **Current term and vote**: The server must remember the highest term it has seen and whom it voted for in that term. Without this, a restarted server could vote twice in the same term, potentially creating two leaders.
2. **Log entries**: Once a server has accepted a log entry from a leader, it must persist it. Without this, a restarted server could "forget" entries that were part of a committed majority, potentially losing committed data if enough servers restart simultaneously.
3. **Commit index**: While technically recoverable (the leader will tell the server what is committed), persisting the commit index enables faster recovery by allowing the server to replay committed entries without waiting for the leader.

The implementation includes a pluggable log storage subsystem that abstracts these persistence requirements behind a clean interface. The storage interface defines operations for:

- **Log entry operations**: Writing, reading, and deleting individual log entries. Batch write operations are supported for atomically persisting multiple entries in a single I/O operation, which is critical for the batch replication optimisation. Without batch writes, persisting N entries would require N separate fsyncs, each costing ~1ms on a typical SSD. With batch writes, a single fsync suffices for the entire batch.
- **Metadata operations**: Persisting and reading the current term, voted-for candidate, commit index, and other protocol state that must survive restarts. Metadata writes are on the critical path — they must complete before the server can send messages or grant votes, because the safety argument depends on the server remembering its state across crashes.
- **Compaction**: Removing log entries that have been included in a snapshot, preventing unbounded storage growth. Without compaction, a server that has been running for days would accumulate millions of log entries, consuming memory and slowing recovery.

Two implementations are provided:

**In-memory storage** serves as a fast, non-durable backend primarily used for testing. All data is stored in memory and lost on restart. This is the default when no persistent storage is configured.

**RocksDB storage** provides durable persistence using Facebook's RocksDB embedded key-value store. It is configured with synchronous writes (fsync on every write) to guarantee that persisted state survives process crashes and power failures. Write batches are used for atomic multi-entry writes — either all entries in a batch are persisted or none are, preventing partially written state.

**Persistence design trade-offs.** The persistence layer makes several deliberate trade-offs between durability, performance, and recovery speed:

*Metadata durability is prioritised over log durability.* Term and vote changes are persisted synchronously on every change, because violating the one-vote-per-term invariant can create two leaders in the same term — a catastrophic safety violation. Log entries, by contrast, can afford slightly relaxed durability: if a follower crashes after accepting an entry but before persisting it, the entry is simply re-sent by the leader on the next heartbeat. The asymmetry in durability requirements reflects the asymmetry in safety consequences: losing a term/vote state leads to safety violations, while losing an uncommitted log entry leads to a temporary performance degradation (the leader resends it).

*The commit index is persisted but not on the critical path.* Persisting the commit index is strictly optional for safety — the leader will communicate the current commit index in subsequent AppendEntries messages. However, persisting it enables faster recovery: without a persisted commit index, a restarted server must wait for the leader to tell it which entries are committed before it can replay the state machine. With a persisted commit index, the server can begin replaying committed entries immediately upon restart, reducing recovery time from "wait for leader heartbeat" (potentially hundreds of milliseconds) to "read from local storage" (sub-millisecond).

*Batch persistence amortises fsync costs.* Individual fsync operations are the most expensive part of persistent storage, each costing roughly 1ms on a typical SSD. By batching multiple log entries into a single write operation, the implementation amortises this cost across many entries. Under high load, a single batch write might persist 50-100 entries with a single fsync, reducing the per-entry persistence cost from 1ms to 10-20 microseconds — a 50-100x improvement.

The Raft server integrates with the storage backend at several points in the protocol:

- After every term change or vote grant, the new term and vote are persisted immediately. This synchronous persistence is on the critical path for elections: a candidate must persist its self-vote before broadcasting vote requests, because a crash between broadcasting and persisting could allow the candidate to vote for a different candidate after restart.
- When the leader appends new log entries, they are persisted before being sent to followers. This ensures that the leader does not send entries it cannot recover after a crash.
- When the commit index advances, it is persisted asynchronously. The commit index is used to accelerate recovery but is not required for safety.
- Followers persist received entries as part of the AppendEntries handler, before acknowledging. This ensures that an acknowledged entry is durable on the follower, which is necessary for the commit rule (an entry committed on a majority of servers must survive any single server's crash).

## Crash Recovery

Crash recovery is the process by which a Raft server reconstitutes its state after a process crash or machine reboot. The recovery process must satisfy two properties: (1) the recovered server must not violate any Raft safety property (it must not vote twice, accept conflicting entries, or lose committed data), and (2) the recovered server must be able to rejoin the cluster and eventually catch up with the current state.

The first property is guaranteed by the persistence design: because the term, vote, and log entries are persisted before the server acts on them, a restarted server will have the same durable state as before the crash. It will not grant a duplicate vote because it reads the persisted vote. It will not accept conflicting entries because its persisted log provides the correct consistency check baseline.

The second property is guaranteed by Raft's log reconciliation mechanism: when the restarted server rejoins the cluster as a follower, the current leader will send it any entries it missed during its downtime via the normal AppendEntries mechanism.

When a Raft server restarts after a crash, it must recover its state to a consistent point before rejoining the cluster. The recovery process proceeds as follows:

1. **State detection**: The recovery manager determines the recovery mode based on available state. If no persisted data exists, it is a fresh start. If persisted data exists, normal recovery proceeds. An environment variable can force a fresh start even when persisted data exists.

2. **Metadata recovery**: The current term, voted-for candidate, and commit index are loaded from the storage backend.

3. **Log recovery**: All persisted log entries are loaded into memory, reconstructing the in-memory log map.

4. **State replay**: Committed entries (those with indices up to the recovered commit index) are replayed through the application callback. This brings the state machine back to the state it was in before the crash.

5. **Cluster rejoining**: The server resumes normal operation as a follower. It will receive AppendEntries from the current leader, which will bring it up to date with any entries committed during its downtime.

The recovery design ensures that no committed data is lost across crashes. Entries that were received but not yet committed may be overwritten by the current leader during log reconciliation, which is correct behaviour — uncommitted entries are not guaranteed to be durable.

## Snapshot Support

For long-running deployments, replaying the entire log from the beginning on every restart becomes impractical. Consider a system that has been running for a week, processing 100,000 transactions per second — the log would contain over 60 billion entries. Replaying this log on restart could take hours or days, making the server effectively unavailable. The snapshot mechanism addresses this by periodically capturing the state machine state at a point-in-time, allowing log entries prior to that point to be discarded.

The fundamental insight behind snapshots is that the state machine state at any point is a deterministic function of the initial state plus all log entries applied up to that point. Therefore, instead of storing the entire log history, the system can store the state machine state at index N and only the log entries from N+1 onward. On recovery, it loads the snapshot (which brings the state machine to the state at index N) and then replays only the entries from N+1 onward.

**Snapshot format.** Snapshots use a binary format with a fixed-size header containing:

- A magic number for format identification (to distinguish snapshot files from other data)
- The snapshot term and index (indicating which log entry the snapshot covers — this is the logical point-in-time of the snapshot)
- A metadata length field (for any additional metadata beyond the state machine state)
- A CRC32 checksum for integrity verification (to detect corruption from disk errors or partial writes)

The snapshot body contains the serialised state machine state. The format is designed to be self-describing — a recovery process can verify the snapshot's integrity before loading it, falling back to an older snapshot if the most recent one is corrupted.

**Atomic writes.** Snapshots are written atomically using a write-to-temporary-then-rename pattern:

1. The snapshot data is written to a temporary file in the same directory as the final snapshot.
2. The temporary file is fsynced to guarantee that all data has been flushed to the storage device. Without this fsync, a power failure after the rename could leave a snapshot file that appears valid (correct name) but contains zeroes or garbage (data still in the OS page cache).
3. The temporary file is renamed to the final path. On POSIX filesystems, rename is atomic — either the old file or the new file exists at the final path, never a partially written file.

This three-step pattern ensures that a crash at any point during snapshot writing does not corrupt the existing snapshot. The worst that can happen is that the temporary file is left behind, which is cleaned up on the next startup.

**Retention policy.** The snapshot manager maintains a configurable number of recent snapshots (typically 3). Older snapshots are deleted to prevent unbounded storage growth while maintaining a safety margin:

- The most recent snapshot is used for normal recovery.
- The previous snapshot provides a fallback if the most recent one is corrupted.
- The snapshot before that provides an additional margin of safety.

This redundancy is important because snapshot corruption, while rare, does occur in practice (due to disk errors, firmware bugs, or software defects).

**Log compaction.** After a snapshot is successfully created at index N, log entries with indices up to N can be removed from both in-memory storage and the persistent backend. This is the payoff of the snapshot mechanism: it bounds the log size and memory usage regardless of how long the system has been running. The compaction is safe because any server that needs entries before N can be sent the snapshot instead — the Raft paper describes an InstallSnapshot RPC for this purpose, though the current implementation relies on all servers staying close enough to the leader that log entries alone suffice for catch-up.

---

# Conclusion

This thesis has presented the design, implementation, and evaluation of a Raft consensus module integrated into the Mako distributed transaction system. The work demonstrates that it is possible to add a complete alternative replication protocol to an existing distributed database without modifying the upper layers, achieving performance parity in the most realistic deployment scenarios.

The key findings are:

1. **Performance parity in multi-shard mode**: Raft and Multi-Paxos achieve near-identical throughput (~8,500 ops/sec per shard) when cross-shard transactions are present. This is the most relevant scenario for production deployments, where data is typically distributed across many shards.

2. **Single-shard Paxos advantage explained**: Multi-Paxos's 28% throughput advantage in single-shard mode is attributable to its ability to pipeline consensus instances and its larger batch sizes. This advantage disappears when the bottleneck shifts from replication to cross-shard coordination.

3. **Resource efficiency**: Raft uses 25% fewer processes per shard (3 voting replicas vs. 4 for Paxos, which adds a non-voting learner). In large deployments, this translates directly to infrastructure cost savings.

4. **Operational simplicity**: Raft's built-in leader election and the preferred leader extension eliminate the need for external leader management infrastructure, simplifying deployment and operations.

5. **Correctness verification**: The comprehensive test suite — 11 standalone correctness tests covering the Raft paper's scenarios, plus CI integration tests covering single-shard, multi-shard, simple, and replicated transaction workloads — provides confidence that the implementation is correct.

The preferred leader election mechanism is a novel contribution that provides deterministic leader placement while preserving all five Raft safety properties. The three-phase design (startup bias, monitoring, piggybacked transfer) handles the complete lifecycle of leader placement: initial election, failover, and failback. The safety argument demonstrates that the extension is a pure optimisation — it changes when elections happen, not how they work.

### Lessons Learned

The process of integrating Raft into a production-grade distributed transaction system yielded several lessons that are broadly applicable to systems engineering:

**The gap between specification and implementation is vast.** The Raft paper is 18 pages. The implementation is thousands of lines. The paper describes the protocol's steady-state behaviour, its safety invariants, and the key mechanisms. It does not describe how to handle shutdown races, how to manage mutex scope across asynchronous RPC calls, how to integrate with a multi-phase boot sequence, or how to handle the dozens of edge cases that arise when a consensus protocol interacts with a transaction processing pipeline. The specification is the starting point, not the destination.

**Concurrency bugs dominate development time.** The most time-consuming bugs were not algorithmic errors in the Raft protocol but concurrency issues at the integration boundaries. Examples include: the heartbeat loop holding a mutex during RPC calls, blocking all other Raft operations; callback registrations being overwritten during initialisation; and detached coroutines accessing object state after the destructor had run. These bugs were difficult to reproduce because they depended on specific timing between concurrent operations, and they were difficult to diagnose because the symptoms (hangs, crashes, silent data loss) were far removed from the root causes.

**Testing infrastructure is as important as the implementation.** The ability to programmatically disconnect and reconnect servers in the standalone test suite was invaluable for validating edge cases that would be nearly impossible to trigger reliably in a multi-process environment. The CI test suite caught integration bugs that the standalone tests could not — such as RPC serialisation errors, port conflicts, and process startup race conditions. Both levels of testing were essential; neither alone would have been sufficient.

**Operational simplicity has compounding returns.** Raft's built-in leader election eliminated an entire subsystem that would otherwise need to be built, tested, and maintained. The preferred leader extension added deterministic leader placement without introducing external dependencies. Each reduction in operational complexity compounds — fewer components means fewer interactions, fewer failure modes, and fewer things to monitor and debug.

**Performance analysis requires careful methodology.** The single-shard comparison initially appeared to show a clear Paxos advantage, but deeper analysis revealed that the gap is attributable to fundamental architectural differences (pipelining and batch size) rather than incidental measurement conditions. Both protocols use the same 30-second internal benchmark runtime, ensuring a fair comparison. The multi-shard comparison, which more closely represents production workloads, showed near-identical performance. Drawing conclusions from a single configuration would have been misleading.

**Future work.** Several directions for future work emerge from this thesis:

- **Geo-replicated benchmarks**: Running the same comparison on a geo-distributed cluster with real network latency would provide more production-representative results and likely show even smaller performance differences between the protocols.
- **Log compaction with InstallSnapshot**: Implementing the InstallSnapshot RPC would allow very slow or newly added replicas to catch up via a snapshot transfer rather than replaying the entire log, which is critical for operational scenarios where a replica has been offline for an extended period.
- **Non-voting learner support in Raft**: Adding a non-voting learner role to the Raft implementation would provide read scaling capabilities comparable to Paxos's learner, while maintaining Raft's simpler all-voter consensus. This would eliminate one of the remaining advantages of the Paxos topology.
- **Performance optimisation**: Investigating Raft-specific optimisations such as parallel commit (committing entries from multiple terms simultaneously), read leases (serving reads from followers without leader involvement), and pre-vote (reducing disruption from partitioned servers) could narrow the single-shard throughput gap and improve availability during network instability.
- **Formal verification**: The safety argument for the preferred leader extension is informal. A formal verification using a tool like TLA+ or Coq would provide stronger guarantees and could uncover edge cases not considered in the informal argument.

---

# Chapter 10: Appendix

## Glossary

### Raft-Specific Terms

| Term | Definition |
|------|------------|
| **Term** | A monotonically increasing integer that acts as a logical clock in Raft. Each term begins with an election. |
| **Log index** | The position of an entry in the replicated log, starting from 1. Each entry has a unique (term, index) pair. |
| **Commit index** | The highest log index known to be replicated on a majority of servers. Entries up to the commit index are safe to apply. |
| **Execute index** | The highest log index applied to the state machine. Always less than or equal to the commit index. |
| **Match index** | Leader-maintained: the highest log index known to be replicated on a given follower. Used to compute the commit index. |
| **Next index** | Leader-maintained: the next log index to send to a given follower. Decremented on rejection (backtracking). |
| **Vote** | The candidate a server voted for in the current term. At most one vote per term. Persisted to stable storage. |
| **Election timeout** | Random duration after which a follower that has not heard from a leader becomes a candidate. Randomisation prevents repeated split votes. |
| **Heartbeat** | An empty AppendEntries RPC sent by the leader to maintain authority and prevent follower elections. |
| **Split vote** | When no candidate receives a majority, ending the term without a leader. Randomised timeouts make repeats unlikely. |
| **Quorum** | A majority of servers: n/2 + 1 for a cluster of n nodes. For 3 nodes, quorum is 2. |
| **Leader completeness** | Safety property: if an entry is committed in a given term, it will be present in all future leaders' logs. |
| **Preferred leader** | Extension: a designated node the system biases toward electing as leader. |
| **Leadership transfer** | The process of a leader voluntarily stepping down so the preferred leader can take over. |
| **TimeoutNow RPC** | Extension RPC that tells a follower to immediately start an election, bypassing the normal timeout. |

### Mako-Specific Terms

| Term | Definition |
|------|------------|
| **Shard** | A horizontal partition of the database. Each shard is independently replicated. |
| **Partition** | A subdivision of a shard's keyspace. Each partition has its own consensus instance. Not to be confused with network partitions. |
| **Partition group** | A set of replicas responsible for the same partition. One is leader; others are followers. |
| **Watermark** | A progress marker indicating which log entries have been committed and applied. Used for garbage collection. |
| **Epoch** | A logical time boundary. No-op entries synchronise epochs across partitions. |
| **No-op** | A no-operation log entry for epoch synchronisation. Contains no data but advances the commit index. |
| **Atomic broadcast** | The replication layer ensuring total order of operations across replicas. Supports Multi-Paxos and Raft. |
| **OCC** | Optimistic Concurrency Control: transactions execute speculatively and validate at commit time. |
| **Masstree** | A high-performance in-memory trie/B-tree hybrid used as Mako's storage engine. |
| **TPC-C** | Industry-standard OLTP benchmark modelling a wholesale distributor with 5 transaction types. |
| **2PC** | Two-Phase Commit: the protocol for cross-shard transactions. |

### System-Specific Terms

| Term | Definition |
|------|------------|
| **srpc** | Mako's custom RPC framework: TCP/IP-based with ~10-50 microsecond latency. The default transport. |
| **eRPC** | An alternative RDMA-based RPC backend (~1-2 microsecond latency). Not used for Raft testing. |
| **RustyCpp** | A static analysis tool enforcing Rust-style ownership and borrowing rules on C++ code. |
| **Frame** | A factory class in Mako's protocol architecture. Each protocol has a Frame subclass creating protocol-specific components. |

## Memory Safety with RustyCpp

Memory safety is a critical concern for systems software written in C++. The Raft module handles concurrent access from multiple threads (the replication loop, the election timer, the application callback, and incoming RPCs), manages dynamically allocated objects with complex ownership relationships (log entries, RPC responses, protocol state), and operates in a long-running server process where memory leaks accumulate over time. These characteristics make memory safety bugs — use-after-free, double deletion, dangling references, and data races — both likely and dangerous.

All new code in the Raft module follows Rust-style memory safety conventions enforced by the RustyCpp static analysis tool. RustyCpp brings Rust's ownership and borrowing model to C++ through a combination of smart pointer types that encode ownership semantics and a static analysis pass that checks for violations.

### The Motivation for Rust-Style Safety in C++

The decision to use Rust-style safety conventions rather than traditional C++ memory management (raw pointers, manual new/delete, or standard smart pointers) was motivated by several factors:

1. **Prevention over detection**: Traditional C++ memory debugging tools (Valgrind, AddressSanitizer, ThreadSanitizer) detect bugs at runtime, but only when the specific buggy code path is executed. RustyCpp detects potential violations at compile time, catching bugs before they reach testing.

2. **Ownership clarity**: Rust's ownership model forces the programmer to think explicitly about who owns each object and how ownership is transferred. This is particularly valuable in a consensus protocol where objects (log entries, RPC responses) are passed between threads and components.

3. **Interior mutability**: The Raft module has global mutable state (the replication type, protocol configuration) that is accessed from multiple threads. Rust's interior mutability pattern (Cell/RefCell) provides a safe way to manage this state without ad-hoc locking.

### Safety Annotations

Every function and significant code block is annotated as either safe or unsafe:

- **Safe functions** perform no unsafe operations — no raw pointer manipulation, no I/O, no calls to unchecked code. The static analysis tool verifies that safe functions only call other safe functions, forming a transitive safety chain.

- **Unsafe functions** call non-borrow-checked code such as STL I/O (std::cerr, std::cout), legacy Mako functions, or third-party library APIs. The "unsafe" annotation is not a mark of shame — it is an honest acknowledgment that the function interacts with code outside the borrow-checked boundary. The key requirement is that every unsafe call site is explicitly annotated, making it easy to audit the codebase for potential safety issues.

### Ownership Types

Instead of standard C++ smart pointers, the codebase uses Rust-style equivalents that encode ownership semantics more precisely:

- **Single-ownership heap allocation** (analogous to Rust's Box<T>): For objects owned by exactly one component with no sharing. Used for protocol state that belongs to a single Raft server instance.
- **Thread-safe reference-counted shared ownership** (analogous to Rust's Arc<T>): For objects shared across threads with reference counting. Used for log entries that are shared between the replication loop and the application callback.
- **Single-thread shared ownership** (analogous to Rust's Rc<T>): For objects shared within a single thread. Used for component references within the protocol factory.
- **Interior mutability types** (analogous to Rust's Cell<T> and RefCell<T>): For safely mutating through shared references. Used for global mutable state like the replication type configuration.

### Global State Pattern

Global mutable state (such as the replication type) uses an interior mutability type that provides thread-safe read and write operations for trivially copyable values without locking. This pattern replaces the traditional C++ approach of using a global variable protected by a mutex, which would add unnecessary synchronisation overhead for a configuration value that is set once at startup and read frequently thereafter.

### Safety Coverage

Across the Raft module, approximately 77% of methods are annotated as safe, with the remaining 23% marked as unsafe primarily due to:

- **I/O operations**: Logging, error output, and file I/O are inherently unsafe in the borrow-checking model because they involve system calls that cannot be statically verified.
- **Legacy code calls**: Calls to pre-existing Mako code that has not been migrated to the Rust-style ownership model.
- **Third-party library interactions**: Calls to RocksDB, YAML parsers, and RPC framework code.

Files that heavily include third-party headers (such as the YAML parser or RocksDB headers) are excluded from borrow checking entirely because the headers themselves generate hundreds of false positive violations. These exclusions are documented in the build configuration with explanations of why the exclusion is necessary.

The long-term goal is to incrementally increase the safe percentage as more legacy code is migrated to Rust-style ownership. Each migration makes the codebase safer and easier to reason about, even if 100% safe coverage is impractical due to the need to interact with non-Rust-style C++ code.
