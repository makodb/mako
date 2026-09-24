# Complete File Reference

## 1. Raft Implementation (`src/deptran/raft/`)

Selected core files are listed below; line counts are a source snapshot and
may drift as the implementation evolves.

| File | Lines | Description |
|------|-------|-------------|
| `server.cc` | 1,829 | Core Raft server: leader election, log replication, apply state machine, persistence methods |
| `server.h` | 637 | RaftServer class, RaftData structures, log storage integration, replica tracking |
| `raft_worker.cc` | 615 | Worker thread: log submission, batching, watermark management, poll thread lifecycle |
| `raft_worker.h` | 167 | RaftWorker class with submit thread, polling, callback mechanisms |
| `test.cc` | 740 | 11 standalone Raft lab tests: election, agreement, replication, network partition |
| `testconf.cc` | 585 | Test configuration: replica maps, commit callbacks, network simulation (Disconnect/Reconnect) |
| `commo.cc` | 287 | Raft RPC communication: AppendEntries, RequestVote, TimeoutNow, ForwardToLearner |
| `frame.cc` | 206 | Frame registration: protocol initialisation and coordinator/commo factory methods |
| `coordinator.cc` | 199 | Transaction coordinator: command submission to leader, quorum response handling |
| `testconf.h` | 183 | RaftTestConfig class: NSERVERS=5, ELECTIONTIMEOUT=5s, test helpers |
| `commo.h` | 128 | RaftCommo interface, RaftVoteQuorumEvent for collecting vote responses |
| `service.cc` | 112 | RPC service handlers: HandleVote, HandleAppendEntries, HandleTimeoutNow |
| `service.h` | 83 | RaftServiceImpl class defining RPC handler declarations |
| `coordinator.h` | 82 | CoordinatorRaft class: slot hints via `Arc<Cell<slotid_t>>`, quorum logic |
| `macros.h` | 76 | Convenience macros: RAFT_CREATE_EV, LOG_AT_SLOT, LEADER_LOG, etc. |
| `frame.h` | 49 | RaftFrame interface for protocol-specific transaction processing |
| `test.h` | 43 | RaftLabTest class header: 11 test method declarations |

## 2. Paxos Implementation (`src/deptran/paxos/`)

Selected core files are listed below; line counts are a source snapshot and
may drift as the implementation evolves.

| File | Lines | Description |
|------|-------|-------------|
| `server.cc` | 1,025 | Core Paxos server: prepare/accept, ballot tracking, instance management, persistence |
| `commo.cc` | 514 | Communication: Prepare, Accept, Forward, ForwardToLearner RPCs |
| `coordinator.cc` | 432 | Multi-Paxos coordinator: command submission, bulk operations, quorum coordination |
| `server.h` | 246 | PaxosServer class: log storage, ballot state, slot management, snapshot integration |
| `service.cc` | 233 | RPC service: Prepare, Accept, Forward, ForwardToLearner handlers |
| `frame.cc` | 117 | Frame registration and initialisation for Multi-Paxos mode |
| `coordinator.h` | 112 | CoordinatorMultiPaxos class for bulk operations |
| `commo.h` | 108 | MultiPaxosCommo and quorum event definitions |
| `service.h` | 92 | MultiPaxosServiceImpl class |
| `frame.h` | 33 | MultiPaxosFrame header |

## 3. Integration Files

| File | Lines | Description |
|------|-------|-------------|
| `src/deptran/raft_main_helper.cc` | ~200 | Main entry helper for Raft mode: YAML parsing, startup orchestration |
| `src/deptran/replication_helper.h` | ~80 | `DISPATCH_RAFT_OR_PAXOS` macro for runtime protocol switching |
| `src/deptran/replication_helper.cc` | ~60 | Dispatcher using `rusty::Cell<ReplicationType>` for global state |
| `src/mako/mako.hh` | ~800 | Mako main header: `wait_for_termination()`, follower replay loop, `replay_batch` metric |
| `src/mako/benchmarks/bench.cc` | ~750 | TPC-C benchmark: `agg_persist_throughput` calculation (line 720-747) |
| `src/deptran/server_worker.cc` | ~500 | Server worker: RecoveryManager integration, storage initialisation |

## 4. Persistence Layer (`src/srpc/rpc/`)

| File | Lines | Description |
|------|-------|-------------|
| `file_snapshot_manager.hpp` | 531 | FileSnapshotManager: atomic writes, FileSnapshotReader/Writer, retention policy |
| `rocksdb_log_storage.hpp` | 480 | RocksDBLogStorage: persistent storage with sync=true, LZ4, WriteBatch |
| `snapshot_format.hpp` | 373 | Binary format: 52-byte header, CRC32 checksums, serialisation/deserialisation |
| `log_storage.hpp` | 302 | LogEntry struct, LogStorage abstract interface (15 methods) |
| `snapshot_manager.hpp` | 294 | SnapshotMetadata, SnapshotManager interface, SnapshotConfig |
| `memory_log_storage.hpp` | 292 | InMemoryLogStorage: thread-safe testing implementation |
| `recovery_manager.hpp` | 267 | RecoveryManager: mode detection, storage creation, recovery coordination |

## 5. Test Files

### 5.1 C++ Test Binaries (`examples/mako-raft-tests/`)

| File | Lines | Description |
|------|-------|-------------|
| `simpleTransactionRepRaft.cc` | 675 | Transaction replication test: 3 workers executing TPC-C style transactions |
| `testNoOps.cc` | 531 | NO-OPS watermark synchronisation: verifies epoch advancement across 5 replicas |
| `testPreferredReplicaLogReplication.cc` | 422 | Log replication: 25 logs wrapped in TpcCommitCommand batches to 5 replicas |
| `testPreferredReplicaStartup.cc` | 264 | Preferred leader startup: TimeoutNow leadership transfer protocol test |
| `simpleRaft.cc` | 226 | Basic Raft sanity: submits 300 logs (100/partition x 3 partitions) |

### 5.2 Test Shell Scripts (`examples/mako-raft-tests/`)

| File | Lines | CI | Description |
|------|-------|----|-------------|
| `simpleRaft.sh` | 120 | Yes | Basic Raft replication (3 replicas, 300 logs, 40s) |
| `test_1shard_replication_raft.sh` | 153 | Yes | 1-shard TPC-C with Raft (6 threads, 60s) |
| `test_2shard_replication_raft.sh` | 208 | Yes | 2-shard TPC-C with Raft (6 threads, 120s polling) |
| `test_1shard_replication_simple_raft.sh` | 149 | Yes | 1-shard simple tx with Raft (40s) |
| `test_2shard_replication_simple_raft.sh` | 171 | Yes | 2-shard simple tx with Raft (60s) |
| `run_test1_preferred_startup.sh` | 361 | No | Preferred leader election (5-node, 35s) |
| `run_test_log_replication.sh` | 159 | No | Log replication to 5 replicas |
| `run_test_noops.sh` | 256 | No | NO-OPS watermark synchronisation |

### 5.3 Unit Tests (`test/`)

| File | Lines | Description |
|------|-------|-------------|
| `rpc_rocksdb_log_storage_test.cc` | 531 | Google Test suite: 9 categories, thread safety, persistence verification |

## 6. CI Scripts

| File | Lines | Description |
|------|-------|-------------|
| `ci/ci_mako_raft.sh` | 252 | Standalone Raft CI entry point: compile + 5 test functions |
| `ci/ci.sh` | 553 | Primary CI script: Paxos + Raft tests, memory limits, config management |

## 7. Shard Launch Scripts

| File | Lines | Description |
|------|-------|-------------|
| `bash/shard_raft.sh` | 39 | Raft-dedicated shard launcher: raft port range (27xxx), occ_raft.yml |
| `bash/shard.sh` | 62 | Unified launcher: 7th arg selects paxos/raft, paxos port range (17xxx) |

## 8. Configuration Files

### 8.1 Raft Configs (`config/`)

| File | Description |
|------|-------------|
| `occ_raft.yml` | OCC concurrency control + Raft atomic broadcast |
| `none_raft.yml` | No CC + Raft |
| `raft_lab_test.yml` | 5-server standalone test config (cc:none, ab:raft) |

Rule-based configuration is retired. The remaining generic Jetpack recovery
code is legacy and is being audited separately.

### 8.2 Raft Cluster Topologies (`config/1leader_2followers/`)

| File | Description |
|------|-------------|
| `raft2_shardidx0.yml` | 2-partition, 3 replicas, ports 27xxx |
| `raft6_shardidx0.yml` | 6-partition shard 0, 3 replicas, ports 27xxx |
| `raft6_shardidx1.yml` | 6-partition shard 1, 3 replicas, ports 27xxx |

### 8.3 Paxos Configs (for comparison)

| File | Description |
|------|-------------|
| `occ_paxos.yml` | OCC + Paxos |
| `paxos6_shardidx0.yml` | 6-partition shard 0, 4 replicas (3+learner), ports 17xxx |
| `paxos6_shardidx1.yml` | 6-partition shard 1, 4 replicas, ports 17xxx |

### 8.4 Shard Configs (`src/mako/config/`)

| File | Description |
|------|-------------|
| `local-shards1-warehouses6.yml` | 1 shard, 6 warehouses |
| `local-shards2-warehouses6.yml` | 2 shards, 6 warehouses each |

### 8.5 Test Cluster Configs (`config/`)

| File | Description |
|------|-------------|
| `1c1s3r1p_cluster_test.yml` | 1 client, 1 shard, 3 replicas, 1 partition (CI tests) |
| `1c1s5r1p_cluster_test.yml` | 1 client, 1 shard, 5 replicas, 1 partition (standalone tests) |
