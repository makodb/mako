# Mako System Architecture

## What This Document Covers

This document provides a high-level overview of the Mako distributed transaction system: its purpose, core components, transaction flow, shard architecture, and — critically — where the atomic broadcast (replication) layer plugs in. Understanding this architecture is essential for appreciating why integrating Raft was both necessary and non-trivial.

**Note**: Mako itself is pre-existing infrastructure. The author's contribution is the Raft replication module and its integration with Mako (documented in [Chapter 4](../04-mako-integration/architecture.md)).

---

## 1. What Mako Is

Mako is a **speculative distributed transaction system with geo-replication**, designed for high throughput OLTP workloads. It is the system described in the OSDI'25 paper. Key properties:

- **Speculative execution**: Transactions execute optimistically before consensus completes
- **Geo-replication**: Data is replicated across geographically distributed datacenters
- **Sharding**: Data is horizontally partitioned across multiple shards for scalability
- **Pluggable replication**: The atomic broadcast layer (Multi-Paxos or Raft) is swappable at runtime

Mako builds on the Janus codebase (OSDI'16: "Consolidating Concurrency Control and Consensus for Commits under Conflicts") and extends it with speculative execution and a Masstree-based storage engine.

---

## 2. Core Components

```
+------------------------------------------------------------------+
|                        Mako System                                |
|                                                                   |
|  +-------------+    +---------------+    +-------------------+    |
|  |   Client    |--->| TxnCoordinator|--->|   TxnScheduler    |   |
|  | (benchmark) |    |  (dispatch)   |    | (execute & order) |   |
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
|              |  via replication_helper.h        |                  |
|              +--------------------------------+                   |
+------------------------------------------------------------------+
```

### 2.1 Storage Engine: Masstree

Masstree is a high-performance in-memory trie/B-tree hybrid that serves as Mako's primary index structure. It provides:

- Lock-free reads via optimistic version validation
- Fine-grained concurrency control at the leaf level
- Efficient range scans and point lookups

Source: `src/mako/masstree/` (the Masstree library) and `src/mako/benchmarks/` (integration with TPC-C).

### 2.2 Concurrency Control: OCC

Mako uses **Optimistic Concurrency Control** (OCC) as its default transaction isolation mechanism (`src/deptran/occ/`):

- Transactions execute speculatively, reading and writing to local state
- At commit time, a validation phase checks for conflicts
- If validation succeeds, changes are committed; otherwise the transaction aborts and retries
- Configuration: `cc: occ` in the mode YAML

### 2.3 Atomic Broadcast Layer

The atomic broadcast layer ensures **total order** of committed operations across all replicas within a shard. This is where consensus protocols plug in:

- **Multi-Paxos** (`src/deptran/paxos/`): The original replication backend
- **Raft** (`src/deptran/raft/`): The author's contribution, integrated as an alternative

Both protocols implement the same abstract interface exposed through `src/deptran/replication_helper.h`, allowing runtime switching with a single configuration change (`ab: multi_paxos` vs `ab: raft`).

### 2.4 Sharding

Data is horizontally partitioned across **shards**. Each shard:

- Holds a subset of the keyspace (e.g., a range of TPC-C warehouses)
- Has its own independent replication group (leader + followers)
- Runs its own Raft or Paxos instance per partition
- Can be placed on a different set of machines

Cross-shard transactions use **Two-Phase Commit (2PC)** coordinated by the `TxnCoordinator`.

---

## 3. Transaction Flow

A transaction in Mako proceeds through the following stages:

```
Client (dbtest)
    |
    v
[1] TxnCoordinator::DoTxAsync()     -- Dispatch transaction pieces to shards
    |
    v
[2] Communicator::BroadcastDispatch()  -- Send pieces via RPC to target shards
    |
    v
[3] TxnScheduler (TxLogServer)        -- Execute piece on local storage
    |   - Read/write Masstree index
    |   - OCC validation
    |
    v
[4] Atomic Broadcast (Paxos/Raft)     -- Replicate committed log entry
    |   - Leader submits to replication_helper::submit()
    |   - Dispatched to paxos_impl::submit() or raft_impl::submit()
    |   - Consensus achieved across replicas
    |
    v
[5] app_next_ callback                -- Apply committed entry to state machine
    |   - Follower replays committed log entries
    |   - Updates watermark / epoch
    |
    v
[6] Coordinator collects results       -- For cross-shard: 2PC prepare/commit
    |
    v
[7] Client receives commit/abort
```

### Key observations:

1. **Steps 1-3** are identical regardless of replication protocol
2. **Step 4** is the only point where Paxos and Raft differ
3. **Step 5** uses the `app_next_` callback registered in `TxLogServer` (`src/deptran/scheduler.h:354`), which both Paxos and Raft invoke when entries are committed
4. For **single-shard transactions**, steps 1-5 complete the transaction
5. For **cross-shard transactions**, step 6 adds a 2PC coordination phase

---

## 4. Key Classes

### 4.1 Frame (`src/deptran/frame.h`)

The **factory pattern** hub for creating protocol-specific components. Each protocol (OCC, Paxos, Raft) registers a `Frame` subclass that creates the right coordinator, scheduler, communicator, and RPC services.

```
Frame (base class)
 ├── MultiPaxosFrame    (src/deptran/paxos/frame.h)
 ├── RaftFrame          (src/deptran/raft/frame.h)
 ├── OccFrame           (src/deptran/occ/)
 └── ...other protocols
```

Key factory methods (`src/deptran/frame.h:51-79`):
- `CreateCoordinator()`: Protocol-specific transaction coordinator
- `CreateScheduler()`: Protocol-specific scheduler (TxLogServer)
- `CreateCommo()`: Protocol-specific communicator
- `CreateRpcServices()`: RPC service handlers

Protocol name-to-mode mapping is defined in `src/deptran/frame.cc:466-495`:
- `"occ"` -> `MODE_OCC`
- `"multi_paxos"` -> `MODE_MULTI_PAXOS`
- `"raft"` -> `MODE_RAFT`

### 4.2 TxnCoordinator (`src/deptran/coordinator.h`)

Manages the lifecycle of a distributed transaction:
- Dispatches transaction pieces to target shards
- Tracks completion status: `n_dispatch_`, `n_dispatch_ack_`, `n_prepare_req_`, `n_finish_ack_` (lines 91-96)
- Coordinates 2PC for cross-shard transactions via `SendPrepare()`, `SendCommit()`
- Key field: `commo_` (line 76) — the Communicator used for RPC
- Key field: `client_status_` (line 55) — shared `rusty::Arc<ClientStatus>` for statistics

### 4.3 TxLogServer / TxnScheduler (`src/deptran/scheduler.h:332-685`)

The scheduler runs on each replica and manages:
- **Transaction execution**: Processes pieces against local storage
- **Log application**: `app_next_` callback (line 354) — invoked by the replication layer when a log entry is committed
- **Epoch management**: `epoch_mgr_`, `jepoch_`, `oepoch_` (lines 370-375)
- **Legacy Jetpack recovery**: Generic recovery machinery pending a separate
  audit; unsupported after retirement of its Rule witness producer
- **Database interface**: `kv_table_` for application data, `database_` for checksums (lines 534-548)

The `app_next_` callback is the critical integration point: both Paxos and Raft call this callback when entries are committed, making the rest of the system agnostic to the replication protocol.

### 4.4 Communicator (`src/deptran/communicator.h:372-622`)

The RPC layer for inter-node communication:
- **Connection management**: `rpc_clients_` map using `rusty::Arc<rrr::Client>` (line 380)
- **Leader tracking**: `leader_cache_` for partition-to-leader mapping (line 383)
- **Broadcast primitives**: `BroadcastDispatch()`, `SendPrepare()`, `SendCommit()` (lines 478-500)
- **View management**: `partition_views_` tracks current leader for each partition (line 389)

### 4.5 Tx (`src/deptran/tx.h:40-153`)

Runtime state for an individual transaction:
- `tid_`: Transaction ID (line 47)
- `mdb_txn_`: Database transaction handle (line 51)
- Data operations: `ReadColumn()`, `WriteColumn()`, `Query()` (lines 91-125)
- Workspace: `ws_` holds intermediate results (line 54)

---

## 5. Shard Architecture

### 5.1 Data Partitioning

Mako partitions data by **shard** and further subdivides each shard into **partitions**. In TPC-C:
- Each shard handles a set of warehouses (e.g., warehouses 0-5 on shard 0, 6-11 on shard 1)
- Each shard has `nthreads` worker threads, each handling one partition

Configuration example (`src/mako/config/local-shards2-warehouses6.yml`):
```yaml
shards: 2          # Two shards
replicas: 3        # Three replicas per shard
warehouses: 12     # Total warehouses (6 per shard)
```

### 5.2 Replica Topology

Each shard has a **replication group** of replicas across datacenters:

```
Shard 0 Replication Group:
  +-----------+    +-----------+    +-----------+    +-----------+
  | localhost |    |    p1     |    |    p2     |    |  learner  |
  |  (leader) |    | (follower)|    | (follower)|    | (Paxos   |
  | port 31000|    | port 32000|    | port 33000|    |   only)  |
  +-----------+    +-----------+    +-----------+    +-----------+

Shard 1 Replication Group:
  +-----------+    +-----------+    +-----------+    +-----------+
  | localhost |    |    p1     |    |    p2     |    |  learner  |
  |  (leader) |    | (follower)|    | (follower)|    | (Paxos   |
  | port 31006|    | port 32006|    | port 33006|    |   only)  |
  +-----------+    +-----------+    +-----------+    +-----------+
```

**Paxos topology**: 4 replicas per shard (3 voters + 1 learner)
**Raft topology**: 3 replicas per shard (all voters, no learner)

This topology difference (33% fewer processes for Raft) has performance implications discussed in [Chapter 7](../07-performance/analysis.md).

### 5.3 Cross-Shard Transactions

When a transaction touches data on multiple shards, Mako uses **Two-Phase Commit (2PC)**:

1. **Prepare phase**: Coordinator sends `Prepare` to all participating shards
2. **Commit phase**: If all shards vote "yes", coordinator sends `Commit`; otherwise `Abort`

Cross-shard coordination latency (typically ~10ms on localhost) dominates over replication latency, which is why single-shard and multi-shard performance differ dramatically (see [Performance Results](../07-performance/results.md)).

---

## 6. The Existing Paxos Path

Before Raft was added, Mako used **Multi-Paxos** as its sole atomic broadcast protocol.

### Multi-Paxos Implementation (`src/deptran/paxos/`)

Key classes:
- **`PaxosServer`** (`server.h:27-99`): Manages slots, ballots, and log persistence
  - Slot tracking: `min_active_slot_`, `max_executed_slot_`, `max_committed_slot_`, `cur_open_slot_` (lines 29-37)
  - Paxos state per slot: `PaxosData` with `max_ballot_seen_`, `max_ballot_accepted_`, `accepted_cmd_`, `committed_cmd_` (lines 14-20)
  - Log persistence via `log_storage_` with metadata keys: `"cur_epoch"`, `"max_committed_slot"`, `"max_executed_slot"` (lines 55-58)
- **`MultiPaxosFrame`** (`frame.h:12-33`): Factory for Paxos components
- **`MultiPaxosCommo`** (`commo.h`): Paxos RPC communication
- **`MultiPaxosCoordinator`** (`coordinator.h`): Paxos proposal submission

### Paxos Worker Setup (`src/deptran/paxos_main_helper.cc`)

The Paxos initialization sequence (lines 86-123):
1. `worker->SetupService()` — Register RPC handlers
2. `worker->SetupCommo()` — Establish connections to replicas
3. `worker->InitQueueRead()` — Start processing submit queue
4. `worker->SetupHeartbeat()` — Start leader heartbeats

This pattern is mirrored by the Raft integration's `raft_main_helper.cc`, which follows a similar `SetupBase() -> SetupService() -> SetupCommo() -> SetupHeartbeat()` chain (see [raft_main_helper documentation](../04-mako-integration/raft_main_helper.md)).

---

## 7. Where Raft Plugs In

The replication layer is abstracted behind `src/deptran/replication_helper.h`, which provides a **unified API** that dispatches to either `paxos_impl::` or `raft_impl::` at runtime:

```
                    replication_helper.h
                    (unified API)
                          |
            +-------------+-------------+
            |                           |
     paxos_impl::                raft_impl::
  (paxos_main_helper.cc)    (raft_main_helper.cc)
            |                           |
     PaxosWorker                  RaftWorker
            |                           |
     PaxosServer                  RaftServer
```

The dispatch is controlled by a single global:
```cpp
// src/deptran/replication_helper.cc
static rusty::Cell<ReplicationType> g_replication_type{ReplicationType::PAXOS};
```

This can be set via:
1. **CLI flag**: `--replication raft` (parsed in `dbtest.cc`)
2. **Auto-detection**: `detect_replication_type_from_config()` scans YAML config for `ab: raft`
3. **Programmatic**: `janus::set_replication_type(janus::ReplicationType::RAFT)`

The key functions dispatched through this interface include:
- `setup()` / `setup2()`: Initialization
- `submit()`: Log submission
- `add_log()`: Log entry addition
- `register_for_leader()` / `register_for_follower()`: Callback registration
- `get_epoch()` / `set_epoch()`: Epoch management

For the full integration story, see [Chapter 4: Mako Integration](../04-mako-integration/architecture.md).

---

## Related Documents

- [Build System and Configuration](build_system.md) — How to build, configure, and switch between Paxos and Raft
- [Raft Protocol Implementation](../02-raft-core/protocol_overview.md) — The Raft consensus protocol as implemented
- [Integration Architecture](../04-mako-integration/architecture.md) — How Raft was integrated into Mako
- [Performance Comparison](../07-performance/results.md) — Paxos vs Raft benchmark results
