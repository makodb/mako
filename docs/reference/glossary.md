# Glossary

A comprehensive reference of terms used in Mako documentation.

---

## A

### ACID
Acronym for **Atomicity**, **Consistency**, **Isolation**, and **Durability** - the four properties that define reliable database transactions. Mako provides full ACID guarantees with serializability isolation.

### Abort
To cancel a transaction before it commits, discarding all its changes. Aborts can occur due to conflicts, timeouts, or explicit client request.

---

## B

### Ballot
In Paxos, a unique identifier used by proposers to order proposals. Higher ballots take precedence over lower ones. Each leader election typically uses a new ballot.

### Batch
A collection of operations (reads, writes, or log entries) that are processed together for efficiency. Mako batches Paxos log entries to amortize consensus overhead.

### Borrow Checking
Static analysis technique (from Rust) that verifies memory safety at compile time. Mako uses RustyCpp for borrow checking in C++ code.

---

## C

### Commit
To finalize a transaction, making all its changes durable and visible. In Mako, commit can be *speculative* (return before replication) or *synchronous* (wait for replication).

### Concurrency Control
Mechanisms to manage simultaneous transaction execution while maintaining consistency. Mako uses optimistic concurrency control with timestamp ordering.

### Conflict
A situation where two transactions access the same key and at least one writes. Conflicts can cause transaction aborts.

### Coordinator
The component that orchestrates distributed transactions across multiple shards. The coordinator manages the two-phase commit protocol.

### Coroutine
A lightweight cooperative thread that can yield control voluntarily. Mako's SRPC framework uses coroutines for efficient concurrent I/O.

---

## D

### Datacenter
A physical facility housing servers. Mako supports geo-replication across multiple datacenters.

### Dependency
A relationship between transactions where one transaction's outcome affects another. Mako tracks dependencies for proper ordering.

### Durability
The ACID property guaranteeing that committed transactions survive system failures. Mako achieves durability through Paxos replication and RocksDB persistence.

---

## E

### Epoch
A period of time during which certain system properties hold. In Mako, epochs are used for garbage collection and failure recovery.

---

## F

### Follower
In Paxos/Raft, a replica that accepts proposals from the leader but doesn't initiate them. Followers can become leaders if the current leader fails.

### Frame
In Mako's architecture, a protocol-specific module that handles transaction processing. Different frames implement different concurrency control protocols.

---

## G

### Geo-Replication
Distributing data across geographically distant locations. Mako supports geo-replication across multiple datacenters using Paxos.

---

## H

### Hot Shard
A shard that receives disproportionately more traffic than others, often due to uneven data distribution. Hot shards are a common performance bottleneck.

---

## I

### Isolation
The ACID property defining how concurrent transactions interact. Mako provides *serializability*, the strongest isolation level.

### In-Memory
Storing and accessing data entirely in RAM rather than disk. Mako uses Masstree as an in-memory index for fast access.

---

## J

### Janus
A distributed transaction protocol from OSDI'16 that influenced Mako's design. Mako retains this lineage and the project-wide `janus::` namespace, but the standalone Janus protocol implementation is retired.

---

## L

### Leader
In Paxos/Raft, the replica that proposes values and coordinates consensus. Transactions are executed on the leader shard.

### Leader Election
The process by which replicas choose a new leader when the current leader fails. Paxos/Raft provides safe leader election.

### Local Timestamp
The timestamp of the most recently committed transaction on a particular partition. Used to compute the global watermark.

---

## M

### Mako
The speculative distributed transaction system described in this documentation. Named for the mako shark, known for its speed.

### Masstree
A high-performance in-memory B+tree used as Mako's primary storage engine. Optimized for cache efficiency and concurrent access.

### Multi-Paxos
An optimization of Paxos that allows a stable leader to skip the prepare phase for multiple proposals. Used for efficient log replication.

---

## N

### NO-OP
A "no operation" log entry used as a heartbeat or synchronization point in Paxos. NO-OPs trigger watermark computation.

### Node
A physical or virtual machine running Mako software. Can host multiple shards.

---

## O

### OCC (Optimistic Concurrency Control)
A concurrency control method that assumes conflicts are rare and validates at commit time. Mako uses OCC internally.

---

## P

### Partition
A logical subdivision of data within a shard. Each partition has its own Paxos/Raft group for replication.

### Paxos
A consensus protocol for achieving agreement in distributed systems. Mako uses Multi-Paxos for log replication.

### Persistence
Writing data to durable storage (disk). Mako uses RocksDB for asynchronous persistence.

### Prepare
The first phase of two-phase commit where participants vote on whether they can commit.

### Process
An operating system process running Mako code. A single process can host multiple sites/partitions.

---

## Q

### Quorum
The minimum number of replicas required to make progress. For N replicas, quorum is typically ⌊N/2⌋ + 1.

---

## R

### Raft
A consensus protocol similar to Paxos but designed for understandability. Mako supports both Paxos and Raft.

### Reactor
The event loop pattern used in SRPC for handling I/O. Each reactor manages multiple coroutines in a single thread.

### Replica
A copy of a shard's data maintained for fault tolerance. Multiple replicas form a Paxos/Raft group.

### Replication
Copying data across multiple replicas for durability and availability. Mako uses Paxos/Raft for consistent replication.

### RocksDB
An embedded key-value store used for persistent storage. Mako uses RocksDB for durability.

### RustyCpp
A library providing Rust-like smart pointers and borrow checking for C++. Used for memory safety in Mako.

---

## S

### Safety Check
The validation that determines if a transaction can be replayed on a follower. Compares transaction timestamp to watermark.

### Scheduler
The component that manages transaction execution on a shard. Handles locking, ordering, and conflict detection.

### Serializability
The strongest isolation level where concurrent transactions appear to execute serially. Mako provides serializability by default.

### Shard
A horizontal partition of data. Mako distributes data across multiple shards for scalability.

### Site
In Mako's configuration, a logical entity (server or client) that participates in the system. Mapped to processes and hosts.

### Slot
In Paxos, a position in the replicated log. Each committed transaction occupies a slot.

### Speculation
Executing a transaction before full consensus is reached, optimistically assuming success. Mako's key innovation.

### Speculative 2PC
Mako's protocol that returns to clients before replication completes, using watermarks for safety.

### SRPC
"Simple RPC" - Mako's custom RPC and coroutine framework.

---

## T

### Timestamp
A logical clock value assigned to transactions for ordering. Mako uses timestamps for concurrency control and watermark computation.

### Transaction
A sequence of operations that execute atomically. Either all operations succeed, or none do.

### TPC-C
A widely-used database benchmark simulating an e-commerce workload. Mako uses TPC-C for performance evaluation.

### Two-Phase Commit (2PC)
A protocol for coordinating distributed transactions. Consists of prepare and commit phases.

---

## W

### Watermark
A timestamp below which all transactions are guaranteed to be durably replicated. Central to Mako's speculative execution safety.

### Worker
A thread that executes transactions or processes requests. Mako uses multiple worker threads per shard for parallelism.

### Write-Ahead Log (WAL)
A log where changes are written before applying to main storage. Used for durability and recovery.

---

## Y

### YAML
The configuration file format used by Mako. Defines cluster topology, benchmarks, and settings.

---

## Symbols and Abbreviations

| Abbreviation | Meaning |
|--------------|---------|
| 2PC | Two-Phase Commit |
| ACID | Atomicity, Consistency, Isolation, Durability |
| API | Application Programming Interface |
| DC | Datacenter |
| I/O | Input/Output |
| KV | Key-Value |
| OCC | Optimistic Concurrency Control |
| OLAP | Online Analytical Processing |
| OLTP | Online Transaction Processing |
| RAM | Random Access Memory |
| RPC | Remote Procedure Call |
| RTT | Round-Trip Time |
| SQL | Structured Query Language |
| SSD | Solid State Drive |
| TPS | Transactions Per Second |

---

## See Also

- [Architecture Overview](../architecture.md) - System design and components
- [Key Concepts](../concepts.md) - Fundamental concepts explained
- [Configuration Reference](../config.md) - YAML configuration options

---

**Next**: [Architecture Overview](../architecture.md) | [Key Concepts](../concepts.md) | [FAQ](../faq/general.md)
