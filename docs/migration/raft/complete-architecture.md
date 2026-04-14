# Mako + Raft: Complete System Architecture

**Date**: 2025-11-06
**Purpose**: Comprehensive visual diagram of entire Mako+Raft system

---

## The Complete Picture: Mako + Raft Architecture

```
╔═══════════════════════════════════════════════════════════════════════════════════════════════════════════╗
║                                    MAKO DISTRIBUTED DATABASE SYSTEM                                        ║
║                                    with Raft-based Replication                                             ║
╚═══════════════════════════════════════════════════════════════════════════════════════════════════════════╝

┌─────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                         CLIENT LAYER                                                      │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐                                │
│  │   Client 1   │  │   Client 2   │  │   Client 3   │  │   Client N   │                                │
│  │              │  │              │  │              │  │              │                                │
│  │  [TPC-C txn] │  │  [TPC-C txn] │  │  [TPC-C txn] │  │  [TPC-C txn] │                                │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘                                │
│         │                 │                 │                 │                                          │
│         └─────────────────┴─────────────────┴─────────────────┘                                          │
│                                     │                                                                     │
│                              (Load Balancing)                                                             │
│                                     │                                                                     │
└─────────────────────────────────────┼─────────────────────────────────────────────────────────────────────┘
                                      │
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                    DATACENTER LAYER                                                       │
│                                                                                                           │
│  ┌─────────────────────────────────────────────────────────────────────────────────────────────────┐   │
│  │                            DATACENTER 1 (Primary - us-east)                                       │   │
│  │                                                                                                   │   │
│  │  ╔═══════════════════════════════════════════════════════════════════════════════════════════╗  │   │
│  │  ║                              SHARD 0 (3 Replicas)                                          ║  │   │
│  │  ║                                                                                             ║  │   │
│  │  ║  ┌────────────────────────────────────────────────────────────────────────────────────┐  ║  │   │
│  │  ║  │  REPLICA 0 (Process: localhost:38100) - LEADER                                      │  ║  │   │
│  │  ║  │  ┌──────────────────────────────────────────────────────────────────────────────┐  │  ║  │   │
│  │  ║  │  │                    MAKO TRANSACTION LAYER                                      │  │  ║  │   │
│  │  ║  │  │                                                                                │  │  ║  │   │
│  │  ║  │  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐         │  │  ║  │   │
│  │  ║  │  │  │  Worker     │  │  Worker     │  │  Worker     │  │  Worker     │   ...   │  │  ║  │   │
│  │  ║  │  │  │  Thread 0   │  │  Thread 1   │  │  Thread 2   │  │  Thread N   │         │  │  ║  │   │
│  │  ║  │  │  │             │  │             │  │             │  │             │         │  │  ║  │   │
│  │  ║  │  │  │ [Execute]   │  │ [Execute]   │  │ [Execute]   │  │ [Execute]   │         │  │  ║  │   │
│  │  ║  │  │  │ [Lock]      │  │ [Lock]      │  │ [Lock]      │  │ [Lock]      │         │  │  ║  │   │
│  │  ║  │  │  │ [Read/Write]│  │ [Read/Write]│  │ [Read/Write]│  │ [Read/Write]│         │  │  ║  │   │
│  │  ║  │  │  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘         │  │  ║  │   │
│  │  ║  │  │         │                │                │                │                 │  │  ║  │   │
│  │  ║  │  │         │   serialize_util(timestamp)                      │                 │  │  ║  │   │
│  │  ║  │  │         └────────────────┴────────────────┴────────────────┘                 │  │  ║  │   │
│  │  ║  │  │                                  │                                            │  │  ║  │   │
│  │  ║  │  │                                  ▼                                            │  │  ║  │   │
│  │  ║  │  │         ┌──────────────────────────────────────────────────────┐             │  │  ║  │   │
│  │  ║  │  │         │  Mako Transaction Log Format:                        │             │  │  ║  │   │
│  │  ║  │  │         │  [cid][count][len][K-V pairs...][timestamp][latency] │             │  │  ║  │   │
│  │  ║  │  │         └────────────────────┬─────────────────────────────────┘             │  │  ║  │   │
│  │  ║  │  │                              │                                                │  │  ║  │   │
│  │  ║  │  │                              │ add_log_to_nc(log, len, par_id, batch)        │  │  ║  │   │
│  │  ║  │  │                              ▼                                                │  │  ║  │   │
│  │  ║  │  └──────────────────────────────────────────────────────────────────────────────┘  │  ║  │   │
│  │  ║  │                                 │  (Local function call)                           │  ║  │   │
│  │  ║  │  ┌──────────────────────────────┼──────────────────────────────────────────────┐  │  ║  │   │
│  │  ║  │  │            RAFT REPLICATION LAYER (raft_main_helper.cc)                      │  │  ║  │   │
│  │  ║  │  │                              │                                                │  │  ║  │   │
│  │  ║  │  │                              ▼                                                │  │  ║  │   │
│  │  ║  │  │         ┌────────────────────────────────────────────┐                       │  │  ║  │   │
│  │  ║  │  │         │  find_worker(par_id)                       │                       │  │  ║  │   │
│  │  ║  │  │         │  → raft_workers_g[0] (global in process)   │                       │  │  ║  │   │
│  │  ║  │  │         └─────────────────┬──────────────────────────┘                       │  │  ║  │   │
│  │  ║  │  │                           │                                                   │  │  ║  │   │
│  │  ║  │  │                           ▼                                                   │  │  ║  │   │
│  │  ║  │  │         ┌────────────────────────────────────────────┐                       │  │  ║  │   │
│  │  ║  │  │         │  RaftWorker (Partition 0)                  │                       │  │  ║  │   │
│  │  ║  │  │         │                                             │                       │  │  ║  │   │
│  │  ║  │  │         │  IsLeader(0)? → TRUE ✓                     │                       │  │  ║  │   │
│  │  ║  │  │         │                                             │                       │  │  ║  │   │
│  │  ║  │  │         │  ┌────────────────────────────────┐        │                       │  │  ║  │   │
│  │  ║  │  │         │  │  Submit Queue (batching)       │        │                       │  │  ║  │   │
│  │  ║  │  │         │  │  [log1][log2][log3]...         │        │                       │  │  ║  │   │
│  │  ║  │  │         │  └────────────┬───────────────────┘        │                       │  │  ║  │   │
│  │  ║  │  │         │               │ SubmitThread()             │                       │  │  ║  │   │
│  │  ║  │  │         │               ▼                             │                       │  │  ║  │   │
│  │  ║  │  │         │  ┌────────────────────────────────┐        │                       │  │  ║  │   │
│  │  ║  │  │         │  │  RaftServer                    │        │                       │  │  ║  │   │
│  │  ║  │  │         │  │  - currentTerm: 5              │        │                       │  │  ║  │   │
│  │  ║  │  │         │  │  - commitIndex: 142            │        │                       │  │  ║  │   │
│  │  ║  │  │         │  │  - lastLogIndex: 145           │        │                       │  │  ║  │   │
│  │  ║  │  │         │  │  - is_leader_: TRUE            │        │                       │  │  ║  │   │
│  │  ║  │  │         │  │  - vote_for_: 0 (self)         │        │                       │  │  ║  │   │
│  │  ║  │  │         │  │                                 │        │                       │  │  ║  │   │
│  │  ║  │  │         │  │  Raft Log:                     │        │                       │  │  ║  │   │
│  │  ║  │  │         │  │  [1: term=1, cmd=...]          │        │                       │  │  ║  │   │
│  │  ║  │  │         │  │  [2: term=1, cmd=...]          │        │                       │  │  ║  │   │
│  │  ║  │  │         │  │  [3: term=2, cmd=...]          │        │                       │  │  ║  │   │
│  │  ║  │  │         │  │  ...                            │        │                       │  │  ║  │   │
│  │  ║  │  │         │  │  [145: term=5, cmd=...]        │        │                       │  │  ║  │   │
│  │  ║  │  │         │  └────────────┬───────────────────┘        │                       │  │  ║  │   │
│  │  ║  │  │         │               │                             │                       │  │  ║  │   │
│  │  ║  │  │         │               │ HeartbeatLoop()             │                       │  │  ║  │   │
│  │  ║  │  │         │               │ + AppendEntries RPCs        │                       │  │  ║  │   │
│  │  ║  │  │         │               ▼                             │                       │  │  ║  │   │
│  │  ║  │  │         └───────────────┼─────────────────────────────┘                       │  │  ║  │   │
│  │  ║  │  │                         │                                                     │  │  ║  │   │
│  │  ║  │  └─────────────────────────┼─────────────────────────────────────────────────────┘  │  ║  │   │
│  │  ║  │                            │                                                        │  ║  │   │
│  │  ║  │                            │  Raft RPCs (AppendEntries, RequestVote)               │  ║  │   │
│  │  ║  └────────────────────────────┼────────────────────────────────────────────────────────┘  ║  │   │
│  │  ║                               │                                                           ║  │   │
│  │  ║         ┌─────────────────────┼─────────────────────┐                                    ║  │   │
│  │  ║         │                     │                     │                                    ║  │   │
│  │  ║         ▼                     ▼                     ▼                                    ║  │   │
│  │  ║  ┌─────────────────┐   ┌─────────────────┐   ┌─────────────────┐                       ║  │   │
│  │  ║  │  REPLICA 1      │   │  REPLICA 2      │   │  REPLICA 3      │                       ║  │   │
│  │  ║  │  (p1:38101)     │   │  (p2:38102)     │   │  (learner:...)  │                       ║  │   │
│  │  ║  │  FOLLOWER       │   │  FOLLOWER       │   │  LEARNER        │                       ║  │   │
│  │  ║  │                 │   │                 │   │                 │                       ║  │   │
│  │  ║  │  Mako Workers   │   │  Mako Workers   │   │  NO Workers     │                       ║  │   │
│  │  ║  │  (6 threads)    │   │  (6 threads)    │   │  (Read-only)    │                       ║  │   │
│  │  ║  │       │         │   │       │         │   │                 │                       ║  │   │
│  │  ║  │       ▼         │   │       ▼         │   │                 │                       ║  │   │
│  │  ║  │  add_log_to_nc()│   │  add_log_to_nc()│   │                 │                       ║  │   │
│  │  ║  │       │         │   │       │         │   │                 │                       ║  │   │
│  │  ║  │       ▼         │   │       ▼         │   │                 │                       ║  │   │
│  │  ║  │  IsLeader()?    │   │  IsLeader()?    │   │                 │                       ║  │   │
│  │  ║  │    FALSE ✗      │   │    FALSE ✗      │   │                 │                       ║  │   │
│  │  ║  │       │         │   │       │         │   │                 │                       ║  │   │
│  │  ║  │       ▼         │   │       ▼         │   │                 │                       ║  │   │
│  │  ║  │  ❌ DROPPED!    │   │  ❌ DROPPED!    │   │                 │                       ║  │   │
│  │  ║  │  (BUG!)         │   │  (BUG!)         │   │                 │                       ║  │   │
│  │  ║  │                 │   │                 │   │                 │                       ║  │   │
│  │  ║  │  RaftServer     │   │  RaftServer     │   │  RaftServer     │                       ║  │   │
│  │  ║  │  (follower)     │   │  (follower)     │   │  (learner)      │                       ║  │   │
│  │  ║  │       ▲         │   │       ▲         │   │       ▲         │                       ║  │   │
│  │  ║  │       │         │   │       │         │   │       │         │                       ║  │   │
│  │  ║  └───────┼─────────┘   └───────┼─────────┘   └───────┼─────────┘                       ║  │   │
│  │  ║          │                     │                     │                                 ║  │   │
│  │  ║          │  AppendEntries      │  AppendEntries      │  AppendEntries                  ║  │   │
│  │  ║          │  (from leader)      │  (from leader)      │  (from leader)                  ║  │   │
│  │  ║          └─────────────────────┴─────────────────────┘                                 ║  │   │
│  │  ║                                                                                          ║  │   │
│  │  ║  When logs are committed:                                                               ║  │   │
│  │  ║  ┌────────────────────────────────────────────────────────────────────────────┐        ║  │   │
│  │  ║  │  ALL replicas call: register_for_follower_par_id_return() callback         │        ║  │   │
│  │  ║  │                                                                              │        ║  │   │
│  │  ║  │  callback(log, len, par_id, slot_id) {                                      │        ║  │   │
│  │  ║  │      // Replay transaction on local Masstree                                │        ║  │   │
│  │  ║  │      CommitInfo info = get_latest_commit_info(log, len);                    │        ║  │   │
│  │  ║  │      treplay_in_same_thread_opt_mbta_v2(par_id, log, len, db, nshards);     │        ║  │   │
│  │  ║  │      return timestamp * 10 + status;                                        │        ║  │   │
│  │  ║  │  }                                                                           │        ║  │   │
│  │  ║  └────────────────────────────────────────────────────────────────────────────┘        ║  │   │
│  │  ╚═══════════════════════════════════════════════════════════════════════════════════════╝  │   │
│  │                                                                                               │   │
│  │  ╔═══════════════════════════════════════════════════════════════════════════════════════╗  │   │
│  │  ║                              SHARD 1 (3 Replicas)                                      ║  │   │
│  │  ║  Same structure as Shard 0, different partition IDs and data                          ║  │   │
│  │  ╚═══════════════════════════════════════════════════════════════════════════════════════╝  │   │
│  │                                                                                               │   │
│  │  ╔═══════════════════════════════════════════════════════════════════════════════════════╗  │   │
│  │  ║                              SHARD 2 (3 Replicas)                                      ║  │   │
│  │  ║  Same structure as Shard 0, different partition IDs and data                          ║  │   │
│  │  ╚═══════════════════════════════════════════════════════════════════════════════════════╝  │   │
│  │                                                                                               │   │
│  └───────────────────────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                                       │
│  ┌───────────────────────────────────────────────────────────────────────────────────────────────┐   │
│  │                            DATACENTER 2 (Secondary - us-west)                                  │   │
│  │  Same structure as Datacenter 1                                                                │   │
│  │  Can participate in Raft consensus for geo-replication                                         │   │
│  └───────────────────────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                                       │
└───────────────────────────────────────────────────────────────────────────────────────────────────────┘

═══════════════════════════════════════════════════════════════════════════════════════════════════════

                                      STORAGE LAYER (Per Replica)

┌─────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│  Each Replica Process Contains:                                                                          │
│                                                                                                           │
│  ┌────────────────────────────────────────────────────────────────────────────────────────────────┐     │
│  │  IN-MEMORY STORAGE (Masstree)                                                                   │     │
│  │                                                                                                  │     │
│  │  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐                              │     │
│  │  │  Partition 0     │  │  Partition 1     │  │  Partition N     │                              │     │
│  │  │                  │  │                  │  │                  │                              │     │
│  │  │  Masstree Index  │  │  Masstree Index  │  │  Masstree Index  │                              │     │
│  │  │                  │  │                  │  │                  │                              │     │
│  │  │  Key → Value     │  │  Key → Value     │  │  Key → Value     │                              │     │
│  │  │  (versioned)     │  │  (versioned)     │  │  (versioned)     │                              │     │
│  │  │                  │  │                  │  │                  │                              │     │
│  │  │  Timestamp: 1425 │  │  Timestamp: 1423 │  │  Timestamp: 1426 │                              │     │
│  │  └──────────────────┘  └──────────────────┘  └──────────────────┘                              │     │
│  │                                                                                                  │     │
│  └────────────────────────────────────────────────────────────────────────────────────────────────┘     │
│                                                                                                           │
│  ┌────────────────────────────────────────────────────────────────────────────────────────────────┐     │
│  │  PERSISTENT STORAGE (RocksDB) - Optional                                                        │     │
│  │                                                                                                  │     │
│  │  Asynchronously persists Raft logs for durability                                               │     │
│  │  Allows recovery after crashes                                                                  │     │
│  └────────────────────────────────────────────────────────────────────────────────────────────────┘     │
│                                                                                                           │
└───────────────────────────────────────────────────────────────────────────────────────────────────────────┘

═══════════════════════════════════════════════════════════════════════════════════════════════════════

                                    KEY CONCEPTS & TERMINOLOGY

┌─────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                                                                                           │
│  SHARD:                                                                                                   │
│  ├─ Horizontal partition of data (e.g., warehouses 0-10 in shard 0, warehouses 11-20 in shard 1)       │
│  ├─ Each shard is independently replicated                                                               │
│  └─ Allows horizontal scalability                                                                        │
│                                                                                                           │
│  PARTITION:                                                                                               │
│  ├─ Logical subdivision within a shard                                                                   │
│  ├─ Each partition has its own Raft group                                                                │
│  ├─ Identified by partition_id (0, 1, 2, ...)                                                           │
│  └─ Maps to worker thread pools                                                                          │
│                                                                                                           │
│  REPLICA:                                                                                                 │
│  ├─ Physical copy of a partition's data                                                                  │
│  ├─ Each partition has 3+ replicas (configurable)                                                        │
│  ├─ Replicas form a Raft consensus group                                                                 │
│  └─ One replica is leader, others are followers/learners                                                 │
│                                                                                                           │
│  PROCESS:                                                                                                 │
│  ├─ Single OS process (e.g., localhost:38100)                                                           │
│  ├─ Contains: Mako workers + RaftWorker + RaftServer + Storage                                          │
│  ├─ Each process can host multiple partitions                                                            │
│  └─ Identified by hostname:port (e.g., p1:38101)                                                        │
│                                                                                                           │
│  WORKER THREAD:                                                                                           │
│  ├─ Mako transaction executor (bench_worker)                                                             │
│  ├─ Executes TPC-C/TPC-A workload transactions                                                          │
│  ├─ Each thread has thread-local state (TThread::pid)                                                   │
│  └─ Calls add_log_to_nc() to replicate committed transactions                                           │
│                                                                                                           │
│  RAFT GROUP:                                                                                              │
│  ├─ Cluster of replicas for a single partition                                                           │
│  ├─ Uses Raft consensus for replication                                                                  │
│  ├─ Maintains consistent log across all replicas                                                         │
│  └─ Elects leader dynamically (unlike Paxos with static leader)                                         │
│                                                                                                           │
└───────────────────────────────────────────────────────────────────────────────────────────────────────────┘

═══════════════════════════════════════════════════════════════════════════════════════════════════════

                                    TRANSACTION FLOW (Step-by-Step)

┌─────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                                                                                           │
│  [1] CLIENT SUBMITS TRANSACTION                                                                          │
│      └─> TPC-C New Order transaction (multi-shard)                                                       │
│                                                                                                           │
│  [2] TRANSACTION COORDINATOR                                                                              │
│      ├─> Determines which shards are involved                                                            │
│      ├─> Contacts home shard's leader                                                                    │
│      └─> Initiates 2PC (Two-Phase Commit) if multi-shard                                                │
│                                                                                                           │
│  [3] MAKO WORKER THREAD (on leader replica)                                                              │
│      ├─> Executes transaction locally:                                                                   │
│      │   ├─> Acquires locks on keys                                                                      │
│      │   ├─> Reads from Masstree (versioned reads)                                                       │
│      │   ├─> Performs business logic                                                                     │
│      │   └─> Buffers writes in transaction writeset                                                      │
│      │                                                                                                    │
│      └─> Validation & Commit:                                                                             │
│          ├─> Checks for conflicts (OCC validation)                                                       │
│          ├─> Assigns timestamp                                                                            │
│          └─> If validation succeeds:                                                                      │
│              └─> serialize_util(timestamp) // Prepare for replication                                    │
│                                                                                                           │
│  [4] SERIALIZATION (Transaction.cc:620)                                                                  │
│      ├─> Serialize transaction to binary log:                                                            │
│      │   [cid][count][len][K1,V1][K2,V2]...[timestamp][latency_tracker]                                │
│      │                                                                                                    │
│      └─> Call: add_log_to_nc(log, len, TThread::getPartitionID(), batch_size)                          │
│                                                                                                           │
│  [5] RAFT SUBMISSION (raft_main_helper.cc:448)                                                           │
│      ├─> find_worker(par_id) → get local RaftWorker                                                     │
│      ├─> Check: worker->IsLeader(par_id)?                                                               │
│      │   ├─> YES (Leader): enqueue_to_worker() → submit to Raft                                         │
│      │   └─> NO (Follower): DROP! ❌ (CURRENT BUG)                                                       │
│      │                                                                                                    │
│      └─> If leader: log goes to RaftWorker submit queue                                                  │
│                                                                                                           │
│  [6] RAFT REPLICATION (RaftWorker + RaftServer)                                                          │
│      ├─> SubmitThread batches logs (batching for performance)                                           │
│      ├─> RaftServer::Start(cmd, &index, &term)                                                          │
│      │   ├─> Append to local Raft log at index                                                           │
│      │   ├─> Update lastLogIndex++                                                                        │
│      │   └─> Trigger AppendEntries to followers                                                           │
│      │                                                                                                    │
│      └─> HeartbeatLoop() sends AppendEntries RPCs:                                                        │
│          ├─> To follower p1:38101                                                                         │
│          ├─> To follower p2:38102                                                                         │
│          └─> To learner (read-only replica)                                                               │
│                                                                                                           │
│  [7] FOLLOWER RECEIVES AppendEntries                                                                     │
│      ├─> RaftServer::OnAppendEntries(...)                                                               │
│      ├─> Validates: prevLogIndex, prevLogTerm match                                                     │
│      ├─> Appends entries to local log                                                                    │
│      ├─> Updates commitIndex when majority reached                                                       │
│      └─> Responds: success=true                                                                           │
│                                                                                                           │
│  [8] LEADER COMMITS (when majority ACKs)                                                                 │
│      ├─> Receives AppendEntries responses                                                                │
│      ├─> Updates match_index for each follower                                                           │
│      ├─> When majority reached: commitIndex = N                                                          │
│      └─> applyLogs() → calls callback                                                                     │
│                                                                                                           │
│  [9] APPLICATION (ALL replicas)                                                                           │
│      ├─> Leader callback: register_for_leader_par_id_return()                                           │
│      ├─> Follower callback: register_for_follower_par_id_return()                                       │
│      │                                                                                                    │
│      └─> Callback executes:                                                                               │
│          ├─> get_latest_commit_info(log, len) → extract timestamp                                       │
│          ├─> treplay_in_same_thread_opt_mbta_v2() → replay to Masstree                                  │
│          │   ├─> Parse [cid][count][len][K-V pairs...]                                                   │
│          │   ├─> For each K-V pair: Masstree.put(key, value, timestamp)                                 │
│          │   └─> Update local watermark                                                                  │
│          │                                                                                                │
│          └─> Return: timestamp * 10 + status                                                              │
│                                                                                                           │
│  [10] RESPONSE TO CLIENT                                                                                  │
│       └─> Transaction committed successfully!                                                             │
│                                                                                                           │
└───────────────────────────────────────────────────────────────────────────────────────────────────────────┘

═══════════════════════════════════════════════════════════════════════════════════════════════════════

                              CONFIGURATION EXAMPLE (3 Shards, 3 Replicas Each)

┌─────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│  From: config/1c1s3r1p_cluster_test.yml                                                                  │
│                                                                                                           │
│  site:                                                                                                    │
│    server:                                                                                                │
│      - ["localhost:38100", "p1:38101", "p2:38102"]  ← Shard 0, Partition 0                              │
│                                                                                                           │
│  process:                                                                                                 │
│    localhost: localhost                                                                                   │
│    p1: p1                                                                                                 │
│    p2: p2                                                                                                 │
│                                                                                                           │
│  host:                                                                                                    │
│    localhost: 127.0.0.1  ← All on same machine (for testing)                                            │
│    p1: 127.0.0.1                                                                                          │
│    p2: 127.0.0.1                                                                                          │
│                                                                                                           │
│  Deployment command (production):                                                                        │
│    bash bash/shard.sh 3 0 6 localhost  # 3 shards, shard 0, 6 threads, process=localhost                │
│    bash bash/shard.sh 3 0 6 p1         # 3 shards, shard 0, 6 threads, process=p1                       │
│    bash bash/shard.sh 3 0 6 p2         # 3 shards, shard 0, 6 threads, process=p2                       │
│                                                                                                           │
│  This runs: ./build/dbtest --num-threads 6 --shard-index 0 -P <process> --is-replicated                │
│                                                                                                           │
└───────────────────────────────────────────────────────────────────────────────────────────────────────────┘

═══════════════════════════════════════════════════════════════════════════════════════════════════════

                                    THE BUG (Visualized)

┌─────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                                                                                           │
│  PROCESS: p1:38101 (FOLLOWER)                                                                            │
│                                                                                                           │
│  ┌─────────────────────────────────────────────────────────────────────────────────────┐                │
│  │  Mako Worker Thread 3                                                                │                │
│  │                                                                                       │                │
│  │  [Transaction Execution]                                                              │                │
│  │  1. Acquire locks on keys {W_123, S_456}                              ✓              │                │
│  │  2. Read warehouse W_123                                               ✓              │                │
│  │  3. Update stock S_456                                                 ✓              │                │
│  │  4. Validate (no conflicts)                                            ✓              │                │
│  │  5. Assign timestamp: 1427                                             ✓              │                │
│  │  6. Serialize to log: [cid=5][count=2][len=48][W_123...][S_456...]   ✓              │                │
│  │                                                                                       │                │
│  │  7. Call: add_log_to_nc(log, 58, 0, 1)                                               │                │
│  │            │                                                                          │                │
│  │            └──────────────────────┐                                                  │                │
│  └───────────────────────────────────┼──────────────────────────────────────────────────┘                │
│                                      │                                                                   │
│  ┌───────────────────────────────────▼──────────────────────────────────────────────────┐                │
│  │  raft_main_helper.cc::add_log_to_nc()                                                │                │
│  │                                                                                       │                │
│  │  auto* worker = find_worker(0);  // Returns local RaftWorker      ✓                 │                │
│  │                                                                                       │                │
│  │  if (!worker->IsLeader(0)) {                                                         │                │
│  │      // THIS PROCESS IS NOT LEADER!                                                  │                │
│  │      Log_info("partition 0 not led here, dropping");                                 │                │
│  │      return;  // ← ❌ TRANSACTION LOG DROPPED!                                       │                │
│  │  }                                                                                    │                │
│  │                                                                                       │                │
│  │  // Never reached on follower                                                        │                │
│  │  enqueue_to_worker(worker, log, len, par_id, batch_size);  ✗                        │                │
│  │                                                                                       │                │
│  └───────────────────────────────────────────────────────────────────────────────────────┘                │
│                                                                                                           │
│  RESULT:                                                                                                  │
│  ├─> Transaction executed locally on p1's Masstree                        ✓                              │
│  ├─> Locks acquired and released                                          ✓                              │
│  ├─> Local state modified                                                 ✓                              │
│  ├─> BUT: Log never replicated to cluster                                 ❌                             │
│  ├─> Other replicas (localhost, p2) never see this transaction            ❌                             │
│  └─> DATA INCONSISTENCY! p1 has different state than leader               ❌                             │
│                                                                                                           │
│  If p1 crashes and recovers:                                                                              │
│  └─> This transaction is LOST (not in Raft log)                           ❌                             │
│                                                                                                           │
└───────────────────────────────────────────────────────────────────────────────────────────────────────────┘

═══════════════════════════════════════════════════════════════════════════════════════════════════════

                                    THE FIX (Proposed)

┌─────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                                                                                           │
│  PROCESS: p1:38101 (FOLLOWER)                                                                            │
│                                                                                                           │
│  ┌─────────────────────────────────────────────────────────────────────────────────────┐                │
│  │  Mako Worker Thread 3                                                                │                │
│  │                                                                                       │                │
│  │  [Transaction Execution]                                                              │                │
│  │  1-6. Same as before...                                                   ✓          │                │
│  │                                                                                       │                │
│  │  7. Call: result = add_log_to_nc(log, 58, 0, 1)                                      │                │
│  │            │                                                                          │                │
│  │            └──────────────────────┐                                                  │                │
│  └───────────────────────────────────┼──────────────────────────────────────────────────┘                │
│                                      │                                                                   │
│  ┌───────────────────────────────────▼──────────────────────────────────────────────────┐                │
│  │  raft_main_helper.cc::add_log_to_nc() - NEW VERSION                                  │                │
│  │                                                                                       │                │
│  │  AddLogResult add_log_to_nc(...) {                                                   │                │
│  │      auto* worker = find_worker(par_id);                                             │                │
│  │                                                                                       │                │
│  │      if (!worker->IsLeader(par_id)) {                                                │                │
│  │          auto raft_server = worker->GetRaftServer();                                 │                │
│  │          siteid_t current_leader = raft_server->GetCurrentLeader();                  │                │
│  │                                                                                       │                │
│  │          Log_info("Not leader, current leader is site_id=%u", current_leader);       │                │
│  │                                                                                       │                │
│  │          return {                                                                     │                │
│  │              .status = AddLogResult::NOT_LEADER,                                     │                │
│  │              .suggested_leader_id = current_leader  // site_id = 0 (localhost)       │                │
│  │          };                                                                            │                │
│  │      }                                                                                │                │
│  │                                                                                       │                │
│  │      enqueue_to_worker(...);                                                         │                │
│  │      return {.status = AddLogResult::SUCCESS, .suggested_leader_id = INVALID};      │                │
│  │  }                                                                                    │                │
│  │                                                                                       │                │
│  └───────────────────────────────────────────┬───────────────────────────────────────────┘                │
│                                              │                                                           │
│                                              │ Returns NOT_LEADER                                        │
│                                              │                                                           │
│  ┌───────────────────────────────────────────▼───────────────────────────────────────────┐                │
│  │  Transaction.cc::serialize_util() - UPDATED                                           │                │
│  │                                                                                        │                │
│  │  auto result = add_log_to_nc(queueLog, pos, TThread::getPartitionID(), batch_size);  │                │
│  │                                                                                        │                │
│  │  if (!result.IsSuccess()) {                                                            │                │
│  │      if (result.status == AddLogResult::NOT_LEADER) {                                 │                │
│  │          Log_warn("Replication failed: not leader (leader=%u)",                       │                │
│  │                   result.suggested_leader_id);                                         │                │
│  │                                                                                        │                │
│  │          // ABORT transaction - it wasn't replicated!                                 │                │
│  │          throw TransactionAbort("replication_failed");                                 │                │
│  │      }                                                                                 │                │
│  │  }                                                                                     │                │
│  │                                                                                        │                │
│  └────────────────────────────────────────────────────────────────────────────────────────┘                │
│                                                                                                           │
│  RESULT:                                                                                                  │
│  ├─> Transaction aborted (locks released)                                 ✓                              │
│  ├─> Local Masstree changes rolled back                                   ✓                              │
│  ├─> Client receives error: "not leader"                                  ✓                              │
│  ├─> Client can retry to actual leader (localhost:38100)                  ✓                              │
│  └─> NO DATA INCONSISTENCY                                                ✓                              │
│                                                                                                           │
└───────────────────────────────────────────────────────────────────────────────────────────────────────────┘

═══════════════════════════════════════════════════════════════════════════════════════════════════════
