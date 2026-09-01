# Raft-Mako Thesis Documentation

## What This Document Covers

This is the master table of contents and reading guide for the thesis documentation on **integrating Raft consensus into the Mako distributed transaction system**. The documentation covers the design, implementation, testing, and performance evaluation of a Raft replication module that operates as an alternative to Mako's existing Multi-Paxos atomic broadcast layer.

**Author contribution scope**: The Raft module, its integration with Mako, standalone tests, preferred leader election, and the CI test suite were implemented by the author. Mako itself (storage engine, concurrency control, transaction coordination, sharding) is pre-existing infrastructure.

---

## Document Map

All documents are organized under `doc/thesis/` in topical subfolders. Each document is self-contained but cross-references related documents.

### Chapter 1: Mako System Overview (`01-mako-overview/`)

| Document | Description |
|----------|-------------|
| [`system_architecture.md`](01-mako-overview/system_architecture.md) | High-level Mako architecture: Masstree storage, OCC concurrency control, atomic broadcast layer, sharding, and transaction flow. Where Raft/Paxos plug in. |
| [`build_system.md`](01-mako-overview/build_system.md) | CMake build system, `MAKO_USE_RAFT` flag, runtime protocol switching via `--replication`, YAML configuration format, port allocation scheme. |

### Chapter 2: Raft Protocol Implementation (`02-raft-core/`)

| Document | Description |
|----------|-------------|
| [`protocol_overview.md`](02-raft-core/protocol_overview.md) | How this implementation maps to the Raft paper: key classes, state transitions, deviations and extensions. |
| [`server_implementation.md`](02-raft-core/server_implementation.md) | `RaftServer` deep dive: all member variables, `OnRequestVote()`, `OnAppendEntries()`, `Start()`, `applyLogs()`, timers, persistence hooks. |
| [`leader_election.md`](02-raft-core/leader_election.md) | Election mechanism: trigger, vote collection, quorum detection, split-vote handling, term advancement. Includes sequence diagram. |
| [`log_replication.md`](02-raft-core/log_replication.md) | Log replication: leader sends entries, follower consistency check, `match_index`/`next_index` tracking, commit advancement, backtracking, heartbeats. |
| [`coordinator.md`](02-raft-core/coordinator.md) | `CoordinatorRaft`: transaction submission via `Submit()`, `WRONG_LEADER` retry logic, quorum calculation. |
| [`rpc_layer.md`](02-raft-core/rpc_layer.md) | Communication infrastructure: `RaftCommo`, `RaftServiceImpl`, `RaftFrame` factory, RPC macros, wire format. |

### Chapter 3: Preferred Leader Election (`03-preferred-leader/`)

| Document | Description |
|----------|-------------|
| [`design.md`](03-preferred-leader/design.md) | Motivation for preferred leader (data locality, operational control), design overview, safety argument. |
| [`implementation.md`](03-preferred-leader/implementation.md) | Implementation details: `SetPreferredLeader()`, `AmIPreferredLeader()`, `HaveCaughtUp()`, `ShouldTransferLeadership()`, `InitiateLeadershipTransfer()`, `OnTimeoutNow()`, dynamic election timeouts. Includes sequence diagram. |
| [`testing.md`](03-preferred-leader/testing.md) | Test binaries: `testPreferredReplicaStartup`, `testPreferredReplicaLogReplication`, `testNoOps`. |

### Chapter 4: Mako Integration (`04-mako-integration/`)

| Document | Description |
|----------|-------------|
| [`architecture.md`](04-mako-integration/architecture.md) | Integration architecture: `replication_helper.h` dispatcher, `DISPATCH_RAFT_OR_PAXOS` macro, `rusty::Cell<ReplicationType>` global state, `detect_replication_type_from_config()`. |
| [`raft_worker.md`](04-mako-integration/raft_worker.md) | `RaftWorker` bridge: setup chain, leader/follower callbacks, log submission pipeline, `PendingLog` queue, `Next()` callback. |
| [`raft_main_helper.md`](04-mako-integration/raft_main_helper.md) | `raft_main_helper.cc` glue code: `raft_impl` namespace, `setup()`/`setup2()`, leadership change handling, NO-OP entries, `wait_for_local_leadership()`. |
| [`mako_hooks.md`](04-mako-integration/mako_hooks.md) | Mako-side integration: `setup_leader_election_callbacks()`, `detect_replication_type_from_config()`, `is_using_raft()` checks, `shard_raft.sh` vs `shard.sh`. |
| [`challenges.md`](04-mako-integration/challenges.md) | Integration bugs fixed: dispatcher routing, replication type auto-detection, cross-shard RPC failures during leader elections, race conditions, process cleanup. |

### Chapter 5: Standalone Raft Testing (`05-standalone-testing/`)

| Document | Description |
|----------|-------------|
| [`test_framework.md`](05-standalone-testing/test_framework.md) | `RaftLabTest` and `RaftTestConfig`: coroutine-based harness, 5-server setup, network simulation helpers. |
| [`test_cases.md`](05-standalone-testing/test_cases.md) | All 11 test cases documented: `testInitialElection`, `testReElection`, `testBasicAgree`, `testFailAgree`, `testFailNoAgree`, `testRejoin`, `testConcurrentStarts`, `testBackup`, `testCount`, `testUnreliableAgree`, `testFigure8`. |
| [`config_files.md`](05-standalone-testing/config_files.md) | `raft_lab_test.yml` structure, how test configs differ from production. |

### Chapter 6: CI Integration Testing (`06-ci-testing/`)

| Document | Description |
|----------|-------------|
| [`ci_script.md`](06-ci-testing/ci_script.md) | `ci_mako_raft.sh` documentation: script structure, process management, port management. |
| [`test_scenarios.md`](06-ci-testing/test_scenarios.md) | Each CI scenario: simpleRaft, shard1ReplicationRaft, shard2ReplicationRaft, shard1ReplicationSimpleRaft, shard2ReplicationSimpleRaft. Binaries, configs, pass/fail criteria. |
| [`example_scripts.md`](06-ci-testing/example_scripts.md) | Shell script walkthroughs: `simpleRaft.sh`, all `test_*_raft.sh` scripts, `shard_raft.sh`. |

### Chapter 7: Performance Analysis (`07-performance/`)

| Document | Description |
|----------|-------------|
| [`methodology.md`](07-performance/methodology.md) | Benchmark methodology: test environment, workload (TPC-C), configuration, metrics, caveats. |
| [`results.md`](07-performance/results.md) | Detailed results: 1-shard and 2-shard TPC-C throughput, latency breakdown, replay_batch, abort ratios. |
| [`analysis.md`](07-performance/analysis.md) | Why Paxos is faster in single-shard, why 2-shard is equal, throughput drop factors, architectural implications. |
| [`figures.md`](07-performance/figures.md) | Throughput bar charts, scaling line charts, architectural comparison tables (ASCII/Mermaid). |

### Chapter 8: Log Persistence and Recovery (`08-persistence/`)

| Document | Description |
|----------|-------------|
| [`log_storage.md`](08-persistence/log_storage.md) | `LogStorage` interface, `InMemoryLogStorage`, `RocksDBLogStorage`, integration with `RaftServer`. |
| [`recovery.md`](08-persistence/recovery.md) | Crash recovery: `RecoveryManager`, `ReplayCommittedEntries()`, resolving uncommitted entries. |
| [`snapshots.md`](08-persistence/snapshots.md) | `SnapshotManager`, `FileSnapshotManager`, snapshot format, `CompactLog()`, retention. |

### Chapter 9: Appendix (`09-appendix/`)

| Document | Description |
|----------|-------------|
| [`file_reference.md`](09-appendix/file_reference.md) | Complete file listing: all Raft files, Paxos files (for comparison), integration files, configs, test scripts, CI scripts. |
| [`configuration_reference.md`](09-appendix/configuration_reference.md) | YAML configuration reference: mode fields, replication group structure, port allocation. |
| [`glossary.md`](09-appendix/glossary.md) | Terms and definitions: Raft-specific, Mako-specific, and system-specific terminology. |
| [`rustycpp_safety.md`](09-appendix/rustycpp_safety.md) | RustyCpp safety annotations in Raft code: `@safe` vs `@unsafe` methods, `rusty::` types used, borrow checking status. |

---

## Suggested Reading Paths

### Quick Overview (~30 minutes)

For readers who want to understand the contribution at a high level:

1. **This README** -- document map and glossary
2. [`01-mako-overview/system_architecture.md`](01-mako-overview/system_architecture.md) -- what Mako is and where Raft fits
3. [`04-mako-integration/architecture.md`](04-mako-integration/architecture.md) -- how Raft was integrated alongside Paxos
4. [`07-performance/results.md`](07-performance/results.md) -- benchmark results
5. [`07-performance/analysis.md`](07-performance/analysis.md) -- what the results mean

### Implementation Deep Dive (~2 hours)

For readers who want to understand the Raft implementation:

1. [`02-raft-core/protocol_overview.md`](02-raft-core/protocol_overview.md) -- mapping to the Raft paper
2. [`02-raft-core/server_implementation.md`](02-raft-core/server_implementation.md) -- `RaftServer` internals
3. [`02-raft-core/leader_election.md`](02-raft-core/leader_election.md) -- election mechanism
4. [`02-raft-core/log_replication.md`](02-raft-core/log_replication.md) -- replication protocol
5. [`03-preferred-leader/design.md`](03-preferred-leader/design.md) -- preferred leader extension
6. [`03-preferred-leader/implementation.md`](03-preferred-leader/implementation.md) -- how it works in code

### Integration Story (~1.5 hours)

For readers interested in how a consensus protocol is grafted onto an existing system:

1. [`01-mako-overview/system_architecture.md`](01-mako-overview/system_architecture.md) -- the existing system
2. [`01-mako-overview/build_system.md`](01-mako-overview/build_system.md) -- how both protocols coexist in one binary
3. [`04-mako-integration/architecture.md`](04-mako-integration/architecture.md) -- the dispatcher pattern
4. [`04-mako-integration/raft_worker.md`](04-mako-integration/raft_worker.md) -- the bridge between Mako and Raft
5. [`04-mako-integration/raft_main_helper.md`](04-mako-integration/raft_main_helper.md) -- glue code
6. [`04-mako-integration/challenges.md`](04-mako-integration/challenges.md) -- bugs encountered and fixed

### Testing and Validation (~1 hour)

For readers interested in correctness and performance validation:

1. [`05-standalone-testing/test_framework.md`](05-standalone-testing/test_framework.md) -- test infrastructure
2. [`05-standalone-testing/test_cases.md`](05-standalone-testing/test_cases.md) -- what each test verifies
3. [`06-ci-testing/test_scenarios.md`](06-ci-testing/test_scenarios.md) -- CI test scenarios and pass criteria
4. [`07-performance/methodology.md`](07-performance/methodology.md) -- how benchmarks were run
5. [`07-performance/results.md`](07-performance/results.md) -- results
6. [`07-performance/analysis.md`](07-performance/analysis.md) -- analysis

---

## Key Source File Reference

These are the primary source files discussed across the documentation, organized by component:

### Core Raft Protocol
| File | Lines | Description |
|------|------:|-------------|
| `src/deptran/raft/server.h` | ~636 | `RaftServer` class: state machine, election, replication, persistence |
| `src/deptran/raft/server.cc` | ~800 | `RaftServer` implementation |
| `src/deptran/raft/coordinator.h` | ~60 | `CoordinatorRaft`: transaction submission to Raft |
| `src/deptran/raft/coordinator.cc` | ~80 | Coordinator implementation with `WRONG_LEADER` retry |
| `src/deptran/raft/commo.h` | ~100 | `RaftCommo`: RPC communication layer |
| `src/deptran/raft/commo.cc` | ~200 | Send/broadcast implementations |
| `src/deptran/raft/service.h` | ~60 | `RaftServiceImpl`: RPC handler registration |
| `src/deptran/raft/service.cc` | ~100 | RPC handler implementations |
| `src/deptran/raft/frame.h` | ~80 | `RaftFrame`: factory for protocol components |
| `src/deptran/raft/frame.cc` | ~120 | Factory method implementations |
| `src/deptran/raft/macros.h` | ~77 | RPC handler code generation macros |

### Raft-Mako Bridge
| File | Lines | Description |
|------|------:|-------------|
| `src/deptran/raft/raft_worker.h` | ~150 | `RaftWorker`: connects Mako watermarks to Raft replication |
| `src/deptran/raft/raft_worker.cc` | ~400 | Worker setup, callbacks, submit pipeline |
| `src/deptran/raft_main_helper.cc` | ~680 | `raft_impl` namespace: all functions the dispatcher calls |
| `src/deptran/replication_helper.h` | ~200 | `DISPATCH_RAFT_OR_PAXOS` macro, unified API declarations |
| `src/deptran/replication_helper.cc` | ~300 | Runtime dispatcher, `ReplicationType` global state |
| `src/mako/mako.hh` | -- | `detect_replication_type_from_config()`, `setup_leader_election_callbacks()` |

### Standalone Tests
| File | Lines | Description |
|------|------:|-------------|
| `src/deptran/raft/test.h` | ~100 | `RaftLabTest`: coroutine-based test harness |
| `src/deptran/raft/test.cc` | ~600 | 11 test cases (election, agreement, figure 8, etc.) |
| `src/deptran/raft/testconf.h` | ~80 | `RaftTestConfig`: test utilities |
| `src/deptran/raft/testconf.cc` | ~400 | Network simulation, leader detection, agreement checks |

### Paxos (for Comparison)
| File | Description |
|------|-------------|
| `src/deptran/paxos/server.h` | Multi-Paxos server (pre-existing) |
| `src/deptran/paxos/coordinator.h` | Paxos coordinator (pre-existing) |
| `src/deptran/paxos/commo.h` | Paxos communication (pre-existing) |
| `src/deptran/paxos/frame.h` | Paxos factory (pre-existing) |

---

## Glossary

### Raft-Specific Terms

| Term | Definition |
|------|------------|
| **Term** | A monotonically increasing integer that acts as a logical clock in Raft. Each term begins with an election. A node's `currentTerm` is the highest term it has seen. |
| **Log index** | The position of an entry in the replicated log, starting from 1. Each entry has a unique `(term, index)` pair. |
| **Commit index** (`commitIndex`) | The highest log index known to be replicated on a majority of servers. Entries up to the commit index are safe to apply to the state machine. |
| **Execute index** (`executeIndex`) | The highest log index that has been applied to the state machine. Always `executeIndex <= commitIndex`. |
| **Last log index** (`lastLogIndex`) | The index of the last entry in a server's local log. |
| **Match index** (`match_index_[i]`) | Leader-maintained: the highest log index known to be replicated on follower `i`. Used to compute `commitIndex`. |
| **Next index** (`next_index_[i]`) | Leader-maintained: the next log index to send to follower `i`. Initialized to `lastLogIndex + 1`; decremented on rejection (backtracking). |
| **Vote** (`vote_for_`) | The candidate a server voted for in the current term. At most one vote per term (election safety). Persisted to stable storage. |
| **Election timeout** | Random duration (default 0.4--0.7s) after which a follower that has not heard from a leader becomes a candidate and starts an election. Randomization prevents repeated split votes. |
| **Heartbeat** | An empty `AppendEntries` RPC sent by the leader at regular intervals (`HEARTBEAT_INTERVAL`) to maintain authority and prevent follower elections. |
| **Split vote** | When no candidate receives a majority of votes in an election, causing the term to end without a leader. The randomized election timeout makes repeated splits unlikely. |
| **Quorum** | A majority of servers: `n/2 + 1` for a cluster of `n` nodes. For 3 nodes, quorum = 2. |
| **Leader completeness** | Safety property: if a log entry is committed in a given term, that entry will be present in the log of any leader for all higher terms. |
| **Preferred leader** | Extension to standard Raft: a designated node that the system biases toward electing as leader, using shorter election timeouts and a `TimeoutNow` RPC for leadership transfer. |
| **Leadership transfer** | The process of an existing leader voluntarily stepping down so a preferred leader can take over, via the `TimeoutNow` RPC mechanism. |
| **`TimeoutNow` RPC** | A Raft extension RPC that tells a follower to immediately start an election, bypassing the normal election timeout. Used for deterministic leadership transfer. |

### Mako-Specific Terms

| Term | Definition |
|------|------------|
| **Shard** | A horizontal partition of the database. Each shard holds a subset of the data (e.g., a range of TPC-C warehouses) and is independently replicated. |
| **Partition** | Within Mako, a subdivision of a shard's keyspace. Each shard may have multiple partitions, each with its own Raft/Paxos instance. Not to be confused with network partitions. |
| **Partition group** | A set of replicas (across different machines) responsible for the same partition. One replica is the leader; others are followers. |
| **Watermark** | A progress marker indicating which log entries have been committed and applied. Used for garbage collection and follower catch-up. |
| **Epoch** | A logical time boundary. NO-OP entries are used to synchronize epochs across partitions. |
| **NO-OP** | A no-operation log entry submitted to Raft for epoch/watermark synchronization. Contains no actual data but advances the commit index. |
| **Atomic broadcast (`ab`)** | The replication layer that ensures total order of operations across replicas. Mako supports `multi_paxos` and `raft` as atomic broadcast implementations. |
| **OCC** | Optimistic Concurrency Control: Mako's default transaction isolation mechanism. Transactions execute speculatively and are validated at commit time. |
| **Masstree** | A high-performance in-memory trie/B-tree hybrid used as Mako's storage engine. |
| **`dbtest`** | The main benchmark binary that runs TPC-C workloads on Mako. |
| **`simpleTransactionRep`** | A simpler test binary for basic key-value transactions with replication (Paxos variant). |
| **`simpleTransactionRepRaft`** | The Raft variant of `simpleTransactionRep`. |
| **`replay_batch`** | A metric reported by followers indicating how many batches of replicated log entries they have replayed (applied to their local state machine). |
| **`agg_persist_throughput`** | Aggregate persisted throughput: the primary performance metric in `dbtest`, measuring committed transactions per second. |

### System-Specific Terms

| Term | Definition |
|------|------------|
| **srpc** | Mako's custom RPC framework: a TCP/IP-based request/response system with ~10--50 us latency. Used as the default transport. |
| **eRPC** | An alternative high-performance RDMA-based RPC backend (~1--2 us latency). Not used for Raft testing. |
| **DPDK** | Data Plane Development Kit: kernel bypass networking. Available as an optional transport but not used for Raft. |
| **RustyCpp** | A static analysis tool that enforces Rust-style ownership and borrowing rules on C++ code. All new code must pass borrow checking. |
| **`@safe`** | RustyCpp annotation indicating a function has no unsafe operations (no raw pointer manipulation, no I/O, no calls to unchecked code). |
| **`@unsafe`** | RustyCpp annotation indicating a function calls non-borrow-checked code (STL I/O, legacy functions, third-party libraries). |
| **`rusty::Cell<T>`** | A RustyCpp type providing interior mutability for `Copy` types, similar to Rust's `Cell<T>`. Thread-safe for single-word types. |
| **`rusty::Arc<T>`** | A RustyCpp type for thread-safe reference-counted shared ownership, similar to Rust's `Arc<T>`. |
| **`rusty::Box<T>`** | A RustyCpp type for single-ownership heap allocation, similar to Rust's `Box<T>`. Replaces `std::unique_ptr`. |
| **Frame** | A factory class in Mako's protocol architecture. Each protocol (Raft, Paxos, OCC, etc.) has a `Frame` subclass that creates protocol-specific components. |
| **TPC-C** | Transaction Processing Performance Council benchmark C: a standard OLTP workload simulating a wholesale distributor with 5 transaction types (NewOrder, Payment, Delivery, OrderStatus, StockLevel). |
| **2PC** | Two-Phase Commit: the protocol used for cross-shard transactions. The coordinator first prepares all shards, then commits or aborts. |

---

## Related Documents

- [`doc/paxos_vs_raft_comparison.md`](../paxos_vs_raft_comparison.md) -- Existing performance comparison data (source for Chapter 7)
- [`CLAUDE.md`](../../CLAUDE.md) -- Project build instructions and development guidelines
- [`ci/ci_mako_raft.sh`](../../ci/ci_mako_raft.sh) -- Raft CI test script (source for Chapter 6)
- [`ci/ci.sh`](../../ci/ci.sh) -- Paxos CI test script (for comparison)
